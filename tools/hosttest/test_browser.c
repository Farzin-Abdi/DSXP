/* Drives filebrowser.c against a real directory tree.
 *
 * The listing is the first thing anyone sees, and it is easy to get
 * subtly wrong in ways that only show up on a card that has been near a
 * desktop machine: sidecar files that look like tracks, metadata folders,
 * entries that sort in the wrong place. Those are cheap to test here and
 * awkward to notice on hardware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "textfold.h"

/* devkitARM's headers define this; macOS puts it behind _DARWIN_C_SOURCE
 * and Linux behind limits.h, so pin it rather than chase both. */
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#include "filebrowser.c"

/* filebrowser.c asks this before every card write. Stubbed here so the
 * browser tests do not have to drag in the settings file, and so the gate
 * itself can be driven. */
static bool g_blockWrites = false;
bool Settings_CardWritesBlocked(void) { return g_blockWrites; }

/* The browser drops the favourites cache whenever it rewrites a
 * playlist, because the liked list is one. This test does not link that
 * module; what matters here is the browser, and the behaviour the call
 * exists for is checked in test_playlist against the real thing. */
void Favourites_Invalidate(void) { }


static int g_fail = 0;
static void Check(const char *what, bool ok, const char *detail)
{
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

static char g_root[512];

static void Touch(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_root, name);
    FILE *f = fopen(path, "wb");
    if (f) { fputc('x', f); fclose(f); }
}

static void MakeDir(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_root, name);
    mkdir(path, 0755);
}

static bool Listed(const FileBrowser *fb, const char *name)
{
    for (int i = 0; i < fb->count; i++)
        if (strcmp(fb->entries[i].name, name) == 0) return true;
    return false;
}

/* Names that are not ASCII still reach the screen as something.
 *
 * The font is ASCII 32..126. A Persian folder name went to it as raw
 * UTF-8 and came out as a row of question marks - not a font problem so
 * much as a plumbing one, because the decoder had known how to romanise
 * exactly that text for ID3 tags since long before, and the file list
 * simply never asked it. One table now, in textfold.c, used by both. */
static void TestNameFolding(void)
{
    printf("\nnon-ASCII names\n");

    static const struct { const char *in, *want, *what; } kCases[] = {
        { "\xd8\xa8\xd8\xa7\xd8\xb1\xd8\xa7\xd9\x86", "baran",
          "Persian letters" },
        { "\xdb\xb2\xdb\xb0\xdb\xb2\xdb\xb4", "2024",
          "Persian digits" },
        { "a \xe2\x80\x93 b", "a - b", "an en dash" },
        { "plain.mp3", "plain.mp3", "ASCII passes through" },

        /* Cyrillic, which is the one the font can do least about: not
         * one letter of it is in ASCII, so before this a Russian name
         * was question marks from end to end. */
        { "\xd0\x92\xd0\xb8\xd0\xba\xd1\x82\xd0\xbe\xd1\x80", "Viktor", "Russian" },
        { "\xd0\x9a\xd0\xb8\xd0\xbd\xd0\xbe", "Kino", "Russian, short" },
        { "\xd0\x96\xd1\x83\xd0\xba", "Zhuk",
          "Russian zhe, which is two letters in Latin" },
        { "\xd0\xa9\xd0\xb8", "Shchi", "Russian shcha, which is four" },

        /* Latin Extended-A, one from each end of the table and two from
         * the middle: a table indexed a slot out gives a plausible wrong
         * letter, so the cases have to be spread across it. */
        { "Bj\xc3\xb6rk", "Bjork", "Latin-1 accents" },
        { "\xc5\x81\xc3\xb3" "d\xc5\xba", "Lodz", "Polish" },
        { "Dvo\xc5\x99\xc3\xa1k", "Dvorak", "Czech" },
        { "\xc4\x86" "evapi", "Cevapi", "a C-acute, near the table's start" },
        { "\xc5\xbdi\xc5\xbe" "ek", "Zizek", "a Z-caron, near its end" },
        { "G\xc3\xb6t\xc3\xbcrk", "Goturk", "Turkish" },

        /* Greek, and the sharp s, which is the one letter here that is
         * two letters rather than one wearing a mark. */
        { "\xce\x9c\xce\xac\xce\xbd\xce\xbf\xcf\x82", "Manos", "Greek" },
        { "Stra\xc3\x9f" "e", "Strasse", "a sharp s spelled out" },

        /* Fullwidth punctuation, which is what a question mark in the
         * middle of an otherwise plain name usually turns out to be. */
        { "Dance \xef\xbc\x8d Afro", "Dance - Afro", "a fullwidth hyphen" },
        { "\xef\xbc\xa1\xef\xbc\xa2" "C", "ABC", "fullwidth letters" },
        { "a\xe3\x80\x80" "b", "a b", "an ideographic space" },
    };
    char out[128], buf[160];

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        TextFold_Utf8(kCases[i].in, out, sizeof(out));
        snprintf(buf, sizeof(buf), "%s -> \"%s\"", kCases[i].what, out);
        Check("a name folds to something the font can draw",
              strcmp(out, kCases[i].want) == 0, buf);
    }

    /* Truncated UTF-8 is what a FAT volume can hand back, and it must
     * not run off the end of the string or return nothing at all - a
     * name you cannot read is still better than a row you cannot see. */
    TextFold_Utf8("\xd8", out, sizeof(out));
    Check("a truncated sequence does not vanish or overrun",
          out[0] != '\0', out);

    /* The cap is honoured: folding can grow a string, because "kh" and
     * "sh" are two characters where the source was one. */
    char small[8];
    TextFold_Utf8("\xd8\xae\xd8\xae\xd8\xae\xd8\xae\xd8\xae\xd8\xae",
                  small, sizeof(small));
    snprintf(buf, sizeof(buf), "six kh into %d bytes -> \"%s\"",
             (int)sizeof(small), small);
    Check("folding respects the buffer it is given",
          strlen(small) < sizeof(small), buf);
}

/* A card that does not mount must leave the browser alone.
 *
 * Init used to clear the struct and then check, so a refused mount
 * handed the caller a browser with no cwd and no rows. On screen that is
 * "No MP3 files in this folder" over a folder nobody opened, and a
 * Refresh that opens "" and fails the same way - a file manager that
 * looks like it has lost the card and cannot be talked out of it.
 *
 * First, before anything else here mounts successfully: the mount is
 * cached after it works once, which is the other half of the same fix. */
static void TestFailedMount(void)
{
    printf("card that will not mount\n");

    FileBrowser fb;
    memset(&fb, 0, sizeof(fb));
    snprintf(fb.cwd, sizeof(fb.cwd), "/Music/Album");
    fb.count = 7;

    g_fatMountFails = true;
    bool ok = FileBrowser_Init(&fb, "/");
    g_fatMountFails = false;

    char buf[160];
    snprintf(buf, sizeof(buf), "returned %d, cwd \"%s\", %d rows",
             ok, fb.cwd, fb.count);
    Check("a refused mount says so and changes nothing",
          !ok && !strcmp(fb.cwd, "/Music/Album") && fb.count == 7, buf);
}

int main(int argc, char **argv)
{
    TestFailedMount();
    TestNameFolding();
    printf("file browser listing\n");

    snprintf(g_root, sizeof(g_root), "%s", argc > 1 ? argv[1] : "browser_root");
    mkdir(g_root, 0755);

    Touch("1_x.mp3");
    Touch("02 - song.MP3");        /* extension match must be case blind */
    Touch("notes.txt");
    Touch("nodot");

    /* What a Mac leaves behind on a FAT card: an AppleDouble sidecar per
     * file, plus its housekeeping folders. The sidecar is the nasty one -
     * it ends in .mp3 and is not audio. */
    Touch("._1_x.mp3");
    Touch("._02 - song.MP3");
    MakeDir(".Trashes");
    MakeDir(".Spotlight-V100");
    MakeDir("Album");

    FileBrowser fb;
    FileBrowser_Init(&fb, g_root);

    char buf[160];
    snprintf(buf, sizeof(buf), "%d entries listed", fb.count);

    Check("real tracks are listed",
          Listed(&fb, "1_x.mp3") && Listed(&fb, "02 - song.MP3"), buf);

    Check("AppleDouble sidecars are hidden",
          !Listed(&fb, "._1_x.mp3") && !Listed(&fb, "._02 - song.MP3"),
          "._name.mp3 ends in .mp3 but is not audio");

    Check("desktop metadata folders are hidden",
          !Listed(&fb, ".Trashes") && !Listed(&fb, ".Spotlight-V100"), NULL);

    Check("real folders still show", Listed(&fb, "Album"), NULL);

    Check("non-audio files are not listed",
          !Listed(&fb, "notes.txt") && !Listed(&fb, "nodot"), NULL);

    printf("\n%s\n", g_fail ? "FAILURES" : "ALL PASSED (0 failures)");
    return g_fail ? 1 : 0;
}
