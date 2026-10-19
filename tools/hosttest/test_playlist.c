/* M3U handling.
 *
 * Playlists arrive from desktop programs, and those write files this
 * player has no control over: CRLF line endings, '#' metadata lines,
 * backslash separators, blank lines, and paths that reach into
 * subfolders. Every one of those is a way to end up with a queue that is
 * empty or full of names that open nothing, and none of them is visible
 * by reading the parser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#include "filebrowser.c"


/* filebrowser.c asks this before every card write. Stubbed here so the
 * browser tests do not have to drag in the settings file, and so the gate
 * itself can be driven. */
static bool g_blockWrites = false;
bool Settings_CardWritesBlocked(void) { return g_blockWrites; }

#include "favourites.c"


static int g_fail = 0;
static void Check(const char *what, bool ok, const char *detail)
{
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

static char g_root[512];

/* A cheap content hash, enough to notice any change at all. Returns the
 * size through `outSize`, or -1 if the file is not there. */
static unsigned long Fingerprint(const char *path, long *outSize)
{
    FILE *f = fopen(path, "rb");
    if (!f) { if (outSize) *outSize = -1; return 0; }

    unsigned long h = 5381; long n = 0; int c;
    while ((c = fgetc(f)) != EOF) { h = h * 33u + (unsigned char)c; n++; }
    fclose(f);
    if (outSize) *outSize = n;
    return h;
}

static void Write(const char *name, const char *body)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_root, name);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(body, 1, strlen(body), f); fclose(f); }
}

/* The parser, lifted to run standalone - player.c drags in the decoder
 * and the mixer, neither of which this is about. */
#define PLAYLIST_MAX 256
static char s_queue[PLAYLIST_MAX][FB_NAME_LEN];
static int  s_queueCount;

static int ParseM3u(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    s_queueCount = 0;
    char line[FB_NAME_LEN * 2];

    while (s_queueCount < PLAYLIST_MAX && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        for (size_t i = 0; i < n; i++) if (line[i] == '\\') line[i] = '/';
        if (n >= FB_NAME_LEN) continue;
        memcpy(s_queue[s_queueCount], line, n + 1);
        s_queueCount++;
    }
    fclose(f);
    return s_queueCount;
}

/* Favourites, which are a playlist and nothing more.
 *
 * The point of storing them that way is that no new format, parser or
 * folder exists to go wrong - so what is worth checking is the parts
 * that are genuinely new: that a path goes in and comes out, that
 * toggling it twice leaves no trace, that the file stays something
 * another program could read, and that the card-writes gate is honoured
 * rather than quietly bypassed by a feature added after it.
 */
/* The Faves tab lists playlists and nothing else.
 *
 * It is the same browser the Files tab uses, pointed at the playlist
 * folder - which made it the file manager wearing a different name: it
 * offered "..", it offered any subfolder, and one tap left the user
 * somewhere the tab had never suggested it could go. Locked, it lists
 * the lists.
 */
static void TestLockedFolder(void)
{
    printf("\nfaves folder\n");

    /* The real playlist folder, not a stand-in beside it: the liked
     * list is written to wherever Favourites puts it, and this test is
     * about the folder that holds both. */
    char dir[1024], buf[256];
    mkdir(DSXP_DIR, 0777);
    snprintf(dir, sizeof(dir), "%s", PLAYLIST_DIR);
    mkdir(dir, 0777);

    /* The three things it must ignore: a subfolder, a loose track, and
     * the "-.." it would otherwise synthesize. */
    char sub[1200];
    snprintf(sub, sizeof(sub), "%s/Sub", dir); mkdir(sub, 0777);
    snprintf(sub, sizeof(sub), "%s/loose.mp3", dir);
    FILE *f = fopen(sub, "wb"); if (f) fclose(f);

    FileBrowser fb;
    if (!FileBrowser_InitLocked(&fb, dir)) {
        Check("the faves folder opens", false, NULL);
        return;
    }

    int dirs = 0, playlists = 0, other = 0, dotdot = 0;
    for (int i = 0; i < fb.count; i++) {
        if (!strcmp(fb.entries[i].name, "..")) dotdot++;
        else if (fb.entries[i].isDir)          dirs++;
        else if (fb.entries[i].isPlaylist)     playlists++;
        else                                   other++;
    }

    snprintf(buf, sizeof(buf), "%d entries: %d lists, %d folders, %d other, %d up",
             fb.count, playlists, dirs, other, dotdot);
    Check("it lists playlists and nothing else",
          dotdot == 0 && dirs == 0 && other == 0 && playlists >= 1, buf);

    /* And Liked is one of them, under that name. */
    bool sawLiked = false;
    for (int i = 0; i < fb.count; i++)
        if (!strcmp(fb.entries[i].name, FAVOURITES_NAME ".m3u")) sawLiked = true;
    Check("the liked list is in it, named Liked", sawLiked,
          FAVOURITES_NAME ".m3u");

    /* Liked sorts first whatever else is in there, and carries its own
     * flag rather than being recognised by the drawing code. */
    {
        char p2[1200];
        snprintf(p2, sizeof(p2), "%s/Aaa.m3u", dir);
        FILE *a = fopen(p2, "w"); if (a) { fputs("#EXTM3U\n", a); fclose(a); }
        snprintf(p2, sizeof(p2), "%s/Zzz.m3u", dir);
        FILE *z = fopen(p2, "w"); if (z) { fputs("#EXTM3U\n", z); fclose(z); }
    }
    FileBrowser_InitLocked(&fb, dir);
    snprintf(buf, sizeof(buf), "first row is \"%s\", %d rows",
             fb.count ? fb.entries[0].name : "(none)", fb.count);
    Check("the liked list sorts above everything else",
          fb.count >= 3 && fb.entries[0].isLiked, buf);

    /* The same browser, unlocked, is the file manager again - which is
     * what going back to the Files tab does with it. Locked and left
     * that way, the file manager showed playlists and would not climb
     * out of the folder. */
    FileBrowser open;
    if (FileBrowser_Init(&open, dir)) {
        int od = 0;
        for (int i = 0; i < open.count; i++)
            if (open.entries[i].isDir) od++;
        snprintf(buf, sizeof(buf), "%d folder rows, locked=%d", od, open.locked);
        Check("unlocked, the same folder shows folders and up again",
              od >= 1 && !open.locked, buf);
    }
}

/* Editing the liked list the way the minus button does.
 *
 * Both of these were reported from the device: the button did nothing
 * at all on the liked list, and a list that had been edited came back
 * shorter than one removal explains. */
static int CountPathLines(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), f)) if (line[0] == '/') n++;
    fclose(f);
    return n;
}

static int LikedPathCount(void)
{
    FILE *f = fopen(FAVOURITES_PATH, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), f)) if (line[0] == '/') n++;
    fclose(f);
    return n;
}

/* The list must never come back shorter than it went in.
 *
 * Reported from the device: one song liked, and everything else in the
 * list gone. The cache marked itself loaded before it had read
 * anything, so a read that failed left it saying "loaded, and empty" -
 * and the next like appended one path to that empty list and wrote the
 * whole file back over a hundred others. */
/* No write may destroy what it did not put there.
 *
 * Saving a new list used to replace a list of the same name outright -
 * including the liked one, which is reachable by typing "Liked". And
 * the one write that was not staged appended straight onto the file,
 * so a console that stopped partway left half a path welded to it. */
static void TestWritesNeverDestroy(void)
{
    char buf[256];
    char path[1024];

    /* An existing list, and a save that names it. */
    snprintf(path, sizeof(path), "%s/Focus.m3u", PLAYLIST_DIR);
    FILE *f = fopen(path, "w");
    if (!f) { Check("the fixture list can be written", false, path); return; }
    fprintf(f, "#EXTM3U\n/Music/old1.mp3\n/Music/old2.mp3\n");
    fclose(f);

    static FileBrowser mb;
    FileBrowser_Init(&mb, g_root);
    int marked = 0;
    for (int i = 0; i < mb.count; i++)
        if (!mb.entries[i].isDir && !mb.entries[i].isPlaylist) {
            mb.entries[i].marked = true;
            marked++;
        }

    char saved[512] = "";
    bool ok = FileBrowser_SavePlaylistIn(&mb, PLAYLIST_DIR, "Focus",
                                         saved, sizeof(saved));
    int after = CountPathLines(path);

    /* Both old entries still there, plus whatever was marked. */
    bool keptOld = false;
    {
        FILE *g = fopen(path, "r");
        char line[512];
        int seen = 0;
        while (g && fgets(line, sizeof(line), g))
            if (strstr(line, "old1.mp3") || strstr(line, "old2.mp3")) seen++;
        if (g) fclose(g);
        keptOld = (seen == 2);
    }
    snprintf(buf, sizeof(buf), "%d lines after saving %d marked over it, old %s",
             after, marked, keptOld ? "kept" : "DESTROYED");
    Check("saving over an existing list adds to it, never replaces it",
          ok && keptOld && after == 2 + marked, buf);

    /* And the liked list by name, which is the same door. */
    Favourites_Invalidate();
    remove(FAVOURITES_PATH);
    Favourites_Toggle("/Music/liked1.mp3");
    Favourites_Toggle("/Music/liked2.mp3");
    int likesBefore = LikedPathCount();

    FileBrowser_SavePlaylistIn(&mb, PLAYLIST_DIR, FAVOURITES_NAME,
                               saved, sizeof(saved));
    Favourites_Invalidate();
    int likesAfter = LikedPathCount();

    snprintf(buf, sizeof(buf), "%d likes before, %d after", likesBefore, likesAfter);
    Check("and that holds when the name typed is the liked list",
          likesBefore == 2 && likesAfter == 2 + marked &&
          Favourites_Contains("/Music/liked1.mp3"), buf);

    remove(path);
    Favourites_Invalidate();
}

static void TestLikedNeverShrinks(void)
{
    char buf[256];

    /* A list this program can read, and then cannot. */
    Favourites_Invalidate();
    remove(FAVOURITES_PATH);
    Favourites_Toggle("/Music/A.mp3");
    Favourites_Toggle("/Music/B.mp3");
    Favourites_Toggle("/Music/C.mp3");

    int before = LikedPathCount();
    Favourites_Invalidate();

    /* Unreadable, not absent: exactly what a card that hiccups looks
     * like, and the case the old code treated as "the list is empty". */
    chmod(FAVOURITES_PATH, 0);
    bool lit = Favourites_Toggle("/Music/D.mp3");
    chmod(FAVOURITES_PATH, 0644);
    Favourites_Invalidate();
    int after = LikedPathCount();

    snprintf(buf, sizeof(buf), "%d paths before, %d after, heart %s",
             before, after, lit ? "LIT" : "stayed dark");
    Check("a like against an unreadable list changes nothing",
          before == 3 && after == 3 && !lit, buf);

    /* And a list longer than the cache can hold. Everything past the
     * limit is still on the card and must stay there: the cache holds a
     * fragment, so rewriting the file from it would delete the rest. */
    {
        FILE *f = fopen(FAVOURITES_PATH, "w");
        if (!f) { Check("the long fixture can be written", false, FAVOURITES_PATH); return; }
        fprintf(f, "#EXTM3U\n");
        for (int i = 0; i < 200; i++) fprintf(f, "/Music/t%03d.mp3\n", i);
        fclose(f);

        Favourites_Invalidate();
        int big = LikedPathCount();

        /* Unliking, not liking. Liking is already refused once the
         * cache is full, so it was never the way this lost anything.
         * Removing one is: the cache holds the first hundred and
         * twenty-eight, takes one out, and writes a hundred and
         * twenty-seven back over a file that had two hundred. */
        bool lit2 = Favourites_Toggle("/Music/t005.mp3");
        Favourites_Invalidate();
        int still = LikedPathCount();

        snprintf(buf, sizeof(buf), "%d paths, %d after unliking one, heart %s",
                 big, still, lit2 ? "still lit" : "went dark");
        Check("a list longer than the cache is never rewritten short",
              big == 200 && still == 200, buf);
    }

    /* A list that is genuinely absent is a different thing: the first
     * like has to create it. */
    remove(FAVOURITES_PATH);
    Favourites_Invalidate();
    bool made = Favourites_Toggle("/Music/First.mp3");
    Favourites_Invalidate();
    snprintf(buf, sizeof(buf), "heart %s, %d paths",
             made ? "lit" : "STAYED DARK", LikedPathCount());
    Check("but a list that does not exist yet is still created",
          made && LikedPathCount() == 1, buf);

    /* Left behind on purpose: the checks that follow look for a liked
     * list on the Faves shelf, and a test that tidies away the fixture
     * the next one needs fails somewhere else entirely. */
    Favourites_Invalidate();
}

static void TestLikedEditing(void)
{
    char buf[256];

    Favourites_Invalidate();
    remove(FAVOURITES_PATH);

    /* A path longer than a row can hold. Easy to reach, because the
     * liked list stores absolute paths. */
    static char deep[400] = "/Music";
    while (strlen(deep) < 160) strcat(deep, "/A Fairly Long Folder Name");
    strcat(deep, "/track.mp3");

    Favourites_Toggle("/Music/One.mp3");
    Favourites_Toggle(deep);
    Favourites_Toggle("/Music/Three.mp3");

    static FileBrowser lb;
    if (!FileBrowser_InitLocked(&lb, PLAYLIST_DIR)) {
        Check("the faves tab opens", false, "InitLocked failed");
        return;
    }

    int liked = -1;
    for (int i = 0; i < lb.count; i++) if (lb.entries[i].isLiked) liked = i;
    snprintf(buf, sizeof(buf), "at row %d of %d", liked, lb.count);
    Check("the liked list is on the faves tab, first", liked == 0, buf);
    if (liked < 0) return;

    /* Exactly how main.c opens it: the name is a pointer into the very
     * array the open is about to overwrite. That aliasing is what broke
     * this - the folder opened fine, but the name saved for later
     * editing came back as the ".." row written over the top of it, so
     * nothing could find the file again. */
    bool entered = FileBrowser_EnterPlaylist(&lb, lb.entries[liked].name);
    snprintf(buf, sizeof(buf), "playlistName = '%s'", lb.playlistName);
    Check("entering it remembers which list it is",
          entered && strcmp(lb.playlistName, FAVOURITES_NAME ".m3u") == 0, buf);

    snprintf(buf, sizeof(buf), "%d rows for 3 liked tracks", lb.count - 1);
    Check("the over-long path is not shown as a row", lb.count - 1 == 2, buf);

    /* And the minus button, with nothing marked: the row under the
     * cursor is marked and removed. */
    int before = LikedPathCount();
    FileBrowser_ClearMarks(&lb);
    lb.entries[1].marked = true;
    char removed[FB_NAME_LEN];
    snprintf(removed, sizeof(removed), "%s", lb.entries[1].name);
    bool ok = FileBrowser_RemoveMarkedFromPlaylist(&lb);

    Favourites_Invalidate();
    int after = LikedPathCount();
    snprintf(buf, sizeof(buf), "%s, %d paths before, %d after",
             ok ? "removed" : "DID NOTHING", before, after);
    Check("the minus button removes a track from the liked list",
          ok && !Favourites_Contains(removed), buf);

    /* The one that was never on screen must still be on the card. A
     * rewrite built from the rows deleted it, so one removal cost two
     * entries. */
    snprintf(buf, sizeof(buf), "%d paths left, expected %d", after, before - 1);
    Check("and takes exactly one, not the rows it could not show",
          after == before - 1 && Favourites_Contains(deep), buf);

    /* A line too long even for the read buffer must not come back as
     * two - the tail is short enough to look like a path, and used to
     * be listed as a row that played nothing and then written back in
     * place of the real line. */
    {
        static char huge[600] = "/Music";
        while (strlen(huge) < 420) strcat(huge, "/0123456789abcdefghijklmnopqrst");
        strcat(huge, "/x.mp3");

        char path[1024];
        snprintf(path, sizeof(path), "%s/split.m3u", PLAYLIST_DIR);
        FILE *f = fopen(path, "w");
        fprintf(f, "#EXTM3U\n%s\n/Music/Short.mp3\n", huge);
        fclose(f);

        static FileBrowser sb;
        FileBrowser_InitLocked(&sb, PLAYLIST_DIR);
        bool in = FileBrowser_EnterPlaylist(&sb, "split.m3u");
        snprintf(buf, sizeof(buf), "%d rows, expected 1", in ? sb.count - 1 : -1);
        Check("an over-long line is one line, not a row of its tail",
              in && sb.count - 1 == 1 &&
              strcmp(sb.entries[1].name, "/Music/Short.mp3") == 0, buf);

        /* And removing the short one leaves the long one whole. */
        FileBrowser_ClearMarks(&sb);
        sb.entries[1].marked = true;
        FileBrowser_RemoveMarkedFromPlaylist(&sb);

        f = fopen(path, "r");
        char back[700] = "";
        int lines = 0;
        bool intact = false;
        if (f) {
            while (fgets(back, sizeof(back), f)) {
                lines++;
                size_t n = strlen(back);
                while (n && (back[n-1] == '\n' || back[n-1] == '\r')) back[--n] = '\0';
                if (!strcmp(back, huge)) intact = true;
            }
            fclose(f);
        }
        snprintf(buf, sizeof(buf), "%d lines, long path %s",
                 lines, intact ? "whole" : "MANGLED");
        Check("and survives an edit to the line beside it", intact, buf);
        remove(path);
    }

    /* A track already in a list does not go in a second time.
     *
     * The heart could never do it - it toggles - but the picker reaches
     * the same file through a different door: mark a track in the file
     * list, press the add control, choose Liked. Liking it and then
     * adding it put it in twice, with two rows naming one file and no
     * way to tell them apart. */
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/dupes.m3u", PLAYLIST_DIR);
        remove(path);

        static FileBrowser mb;
        FileBrowser_Init(&mb, g_root);
        int marked = 0;
        for (int i = 0; i < mb.count && marked < 2; i++)
            if (!mb.entries[i].isDir && !mb.entries[i].isPlaylist) {
                mb.entries[i].marked = true;
                marked++;
            }

        bool first  = FileBrowser_AppendToPlaylistIn(&mb, PLAYLIST_DIR, "dupes.m3u");
        int afterOne = CountPathLines(path);

        /* The same marks, the same list, again. */
        bool second = FileBrowser_AppendToPlaylistIn(&mb, PLAYLIST_DIR, "dupes.m3u");
        int afterTwo = CountPathLines(path);

        snprintf(buf, sizeof(buf), "%d marked, %d lines after one add, %d after two",
                 marked, afterOne, afterTwo);
        Check("adding the same tracks twice does not list them twice",
              first && second && marked >= 1 &&
              afterOne == marked && afterTwo == marked, buf);

        /* And the other route to a duplicate: a marked folder and a
         * marked track inside that folder, in one write. Both resolve
         * to the same line, and nothing has been flushed to the card
         * when the second one is considered. */
        remove(path);
        char sub2[1024];
        snprintf(sub2, sizeof(sub2), "%s/dupdir", g_root);
        mkdir(sub2, 0755);
        char t1[1200];
        snprintf(t1, sizeof(t1), "%s/inside.mp3", sub2);
        FILE *tf = fopen(t1, "wb"); if (tf) { fputc('x', tf); fclose(tf); }

        static FileBrowser db;
        FileBrowser_Init(&db, g_root);
        for (int i = 0; i < db.count; i++)
            if (!strcmp(db.entries[i].name, "dupdir")) db.entries[i].marked = true;

        static FileBrowser ib;
        FileBrowser_Init(&ib, sub2);
        FileBrowser_AppendToPlaylistIn(&db, PLAYLIST_DIR, "dupes.m3u");
        int viaFolder = CountPathLines(path);
        FileBrowser_AppendToPlaylistIn(&ib, PLAYLIST_DIR, "dupes.m3u");

        for (int i = 0; i < ib.count; i++)
            if (!strcmp(ib.entries[i].name, "inside.mp3")) ib.entries[i].marked = true;
        FileBrowser_AppendToPlaylistIn(&ib, PLAYLIST_DIR, "dupes.m3u");
        int viaBoth = CountPathLines(path);

        snprintf(buf, sizeof(buf), "%d from the folder, %d after the track too",
                 viaFolder, viaBoth);
        Check("a folder and a track inside it do not both add the track",
              viaFolder == 1 && viaBoth == 1, buf);

        remove(path);
        remove(t1);
        rmdir(sub2);
    }

    Favourites_Invalidate();
}

static void TestFavourites(void)
{
    printf("\nfavourites\n");

    char buf[512];
    Favourites_Invalidate();

    const char *a = "/Music/Rozbeh/Baran.mp3";
    const char *b = "/Music/Other/Track 2.mp3";

    Check("nothing is a favourite to begin with",
          !Favourites_Contains(a) && Favourites_Count() == 0, NULL);

    Check("liking one reports it liked", Favourites_Toggle(a) == true, NULL);
    Check("and it is remembered",
          Favourites_Contains(a) && Favourites_Count() == 1, NULL);

    Check("a second is kept beside the first, not instead of it",
          Favourites_Toggle(b) && Favourites_Contains(a) &&
          Favourites_Contains(b) && Favourites_Count() == 2, NULL);

    /* Re-read from the card rather than from memory: the cache and the
     * file have to agree, and only a reload proves the write happened. */
    Favourites_Invalidate();
    snprintf(buf, sizeof(buf), "%d after reload", Favourites_Count());
    Check("they survive being read back off the card",
          Favourites_Contains(a) && Favourites_Contains(b) &&
          Favourites_Count() == 2, buf);

    Check("unliking reports it unliked", Favourites_Toggle(a) == false, NULL);
    Favourites_Invalidate();
    Check("and it is gone from the card too, with the other left alone",
          !Favourites_Contains(a) && Favourites_Contains(b) &&
          Favourites_Count() == 1, NULL);

    /* Still an M3U: a header line, then one path per line. If this stops
     * being true the list stops being something a computer can open,
     * which is half the reason for storing it this way. */
    {
        FILE *f = fopen(FAVOURITES_PATH, "r");
        int lines = 0, paths = 0;
        bool header = false;
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (line[0] == '#') { if (!lines) header = true; }
                else if (line[0] == '/') paths++;
                lines++;
            }
            fclose(f);
        }
        snprintf(buf, sizeof(buf), "%d lines, %d paths", lines, paths);
        Check("the file is still an M3U anything can read",
              f != NULL && header && paths == 1, buf);
    }

    /* The gate: with card writes off nothing is recorded, and the answer
     * says so rather than lighting a heart for a like that was never
     * saved. */
    g_blockWrites = true;
    Favourites_Invalidate();
    bool got = Favourites_Toggle(a);
    Favourites_Invalidate();
    snprintf(buf, sizeof(buf), "reported %s, card says %s",
             got ? "liked" : "not liked",
             Favourites_Contains(a) ? "liked" : "not liked");
    Check("with card writes off it does not claim to have saved",
          got == false && !Favourites_Contains(a), buf);
    g_blockWrites = false;
    Favourites_Invalidate();

    TestLikedEditing();
    TestLikedNeverShrinks();
    TestWritesNeverDestroy();
}

int main(int argc, char **argv)
{
    printf("playlists\n");

    snprintf(g_root, sizeof(g_root), "%s", argc > 1 ? argv[1] : "playlist_root");
    mkdir(g_root, 0755);

    /* Exactly what a desktop program writes: extended header, a title
     * line per track, CRLF endings. */
    Write("desktop.m3u",
          "#EXTM3U\r\n"
          "#EXTINF:214,Radiohead - Kid A\r\n"
          "02 - Kid A.mp3\r\n"
          "\r\n"
          "#EXTINF:-1,Another\r\n"
          "Album\\03 - The National Anthem.mp3\r\n");

    char path[1024];
    snprintf(path, sizeof(path), "%s/desktop.m3u", g_root);
    int n = ParseM3u(path);

    char buf[200];
    snprintf(buf, sizeof(buf), "%d entries: '%s', '%s'", n,
             n > 0 ? s_queue[0] : "", n > 1 ? s_queue[1] : "");
    Check("metadata, blanks and CRLF are all skipped", n == 2, buf);

    Check("no carriage return welded to the name",
          n > 0 && strchr(s_queue[0], '\r') == NULL, buf);

    Check("backslashes become separators",
          n > 1 && strcmp(s_queue[1], "Album/03 - The National Anthem.mp3") == 0, buf);

    /* An entry too long for a queue slot is dropped, not truncated: half
     * a path names nothing and would fail later with no clue why. */
    char big[FB_NAME_LEN * 2];
    memset(big, 'x', sizeof(big) - 6);
    memcpy(big + sizeof(big) - 6, ".mp3\n", 6);
    Write("toolong.m3u", big);
    snprintf(path, sizeof(path), "%s/toolong.m3u", g_root);
    n = ParseM3u(path);
    snprintf(buf, sizeof(buf), "%d entries from an over-long line", n);
    Check("an unusably long entry is dropped, not cut short", n == 0, buf);

    /* And the browser has to show playlists at all. */
    Write("mix.m3u", "#EXTM3U\n");
    Write("song.mp3", "x");
    FileBrowser fb;
    FileBrowser_Init(&fb, g_root);

    bool sawList = false, listFlagged = false, songFlagged = false;
    for (int i = 0; i < fb.count; i++) {
        if (!strcmp(fb.entries[i].name, "mix.m3u")) {
            sawList = true;
            listFlagged = fb.entries[i].isPlaylist;
        }
        if (!strcmp(fb.entries[i].name, "song.mp3"))
            songFlagged = fb.entries[i].isPlaylist;
    }
    snprintf(buf, sizeof(buf), "%d entries listed", fb.count);
    Check("playlists are listed and flagged as such",
          sawList && listFlagged && !songFlagged, buf);

    /* Marking takes tracks and folders, but not playlists - a playlist
     * of playlists has no obvious bottom - and not "..", which is the
     * way out rather than a folder.
     *
     * A folder is set up here so the rule has all three cases to answer
     * for; without one, "folders cannot be marked" and "folders can"
     * both pass. */
    char album[1024];
    snprintf(album, sizeof(album), "%s/Album", g_root);
    mkdir(album, 0777);

    FileBrowser_Init(&fb, g_root);
    FileBrowser_ClearMarks(&fb);
    int folders = 0, lists = 0, ups = 0;
    for (int i = 0; i < fb.count; i++) {
        fb.selected = i;
        FileBrowser_ToggleMark(&fb);
        if (!strcmp(fb.entries[i].name, "..")) ups++;
        else if (fb.entries[i].isDir)          folders++;
        else if (fb.entries[i].isPlaylist)     lists++;
    }
    int marked = FileBrowser_MarkedCount(&fb);
    snprintf(buf, sizeof(buf), "%d of %d took a mark; %d folders, %d lists, %d up",
             marked, fb.count, folders, lists, ups);
    Check("a folder can be marked, a playlist cannot",
          folders >= 1 && lists >= 1 && marked == fb.count - lists - ups, buf);

    /* Rescanning must not carry state across. The entry slots are reused
     * folder to folder, so anything a scan does not overwrite is
     * inherited - and the parent entry is written by hand rather than
     * from a directory read, which is exactly where that goes unnoticed. */
    for (int i = 0; i < fb.count; i++) fb.entries[i].marked = true;

    char sub[1024];
    snprintf(sub, sizeof(sub), "%s/Deeper", g_root);
    mkdir(sub, 0755);
    FileBrowser fb2;
    FileBrowser_Init(&fb2, sub);

    bool cleanParent = true;
    for (int i = 0; i < fb2.count; i++)
        if (!strcmp(fb2.entries[i].name, "..") &&
            (fb2.entries[i].marked || fb2.entries[i].isPlaylist))
            cleanParent = false;

    snprintf(buf, sizeof(buf), "%d entries in the subfolder", fb2.count);
    Check("the parent entry starts clean on every scan", cleanParent, buf);

    /* ---- editing ---------------------------------------------------
     *
     * A playlist opens like a folder, and the same marking then means
     * add and remove. What has to hold is that a rewrite keeps every
     * track it was not asked to drop - losing one silently is the only
     * failure here that destroys something the user made. */
    Write("edit.m3u",
          "#EXTM3U\n"
          "one.mp3\n"
          "two.mp3\n"
          "three.mp3\n"
          "four.mp3\n");

    FileBrowser fb3;
    FileBrowser_Init(&fb3, g_root);
    bool entered = FileBrowser_EnterPlaylist(&fb3, "edit.m3u");

    snprintf(buf, sizeof(buf), "%d rows including '..'", fb3.count);
    Check("a playlist opens like a folder", entered && fb3.count == 5, buf);
    Check("and the way out is the first row",
          fb3.count > 0 && fb3.entries[0].isDir &&
          !strcmp(fb3.entries[0].name, ".."), NULL);

    /* Drop the second and fourth tracks. */
    for (int i = 0; i < fb3.count; i++)
        if (!strcmp(fb3.entries[i].name, "two.mp3") ||
            !strcmp(fb3.entries[i].name, "four.mp3"))
            fb3.entries[i].marked = true;

    bool removed = FileBrowser_RemoveMarkedFromPlaylist(&fb3);

    bool kept1 = false, kept3 = false, gone2 = true, gone4 = true;
    for (int i = 0; i < fb3.count; i++) {
        const char *n2 = fb3.entries[i].name;
        if (!strcmp(n2, "one.mp3"))   kept1 = true;
        if (!strcmp(n2, "three.mp3")) kept3 = true;
        if (!strcmp(n2, "two.mp3"))   gone2 = false;
        if (!strcmp(n2, "four.mp3"))  gone4 = false;
    }
    snprintf(buf, sizeof(buf), "%d rows left", fb3.count);
    Check("removing marked entries keeps the rest",
          removed && kept1 && kept3 && gone2 && gone4, buf);

    /* Straight back off the card, not just out of memory. */
    snprintf(path, sizeof(path), "%s/edit.m3u", g_root);
    n = ParseM3u(path);
    snprintf(buf, sizeof(buf), "file now holds %d entries", n);
    Check("and the change reached the card", n == 2, buf);

    /* Appending must leave what is already there alone. */
    FileBrowser_LeavePlaylist(&fb3);
    for (int i = 0; i < fb3.count; i++)
        if (!strcmp(fb3.entries[i].name, "song.mp3")) fb3.entries[i].marked = true;

    bool appended = FileBrowser_AppendToPlaylistIn(&fb3, fb3.cwd, "edit.m3u");
    n = ParseM3u(path);
    snprintf(buf, sizeof(buf), "%d entries after appending one", n);
    Check("appending adds without disturbing the rest", appended && n == 3, buf);

    /* With card writing switched off, every one of these has to decline
     * and leave the file exactly as it was.
     *
     * The setting exists to take this program out of the picture while a
     * card is being tested, so a path that quietly kept writing would
     * make the whole exercise worthless - it would clear the app of a
     * corruption it was still causing. */
    {
        int before = ParseM3u(path);
        g_blockWrites = true;

        bool app2 = FileBrowser_AppendToPlaylistIn(&fb3, fb3.cwd, "edit.m3u");
        char savedTo[512];
        bool sav2 = FileBrowser_SavePlaylistIn(&fb3, g_root, "blocked.m3u",
                                              savedTo, sizeof(savedTo));
        bool del2 = FileBrowser_DeletePlaylist(&fb3, 0);
        bool rem2 = FileBrowser_RemoveMarkedFromPlaylist(&fb3);

        int after = ParseM3u(path);

        char blocked[512];
        snprintf(blocked, sizeof(blocked), "%s/blocked.m3u", g_root);
        bool noNewFile = (access(blocked, F_OK) != 0);

        g_blockWrites = false;

        snprintf(buf, sizeof(buf), "%d entries before, %d after%s",
                 before, after, noNewFile ? ", no new file" : ", A FILE APPEARED");
        Check("card writes off: every write path declines",
              !app2 && !sav2 && !del2 && !rem2 && before == after && noNewFile,
              buf);

        /* And it is a gate, not a one-way door. */
        bool app3 = FileBrowser_AppendToPlaylistIn(&fb3, fb3.cwd, "edit.m3u");
        snprintf(buf, sizeof(buf), "%d entries once allowed again",
                 ParseM3u(path));
        Check("and writing works again when it is switched back on",
              app3, buf);
    }

    /* Adding is allowed to wander into other folders to find tracks, and
     * that is where the paths go wrong quietly: a track from elsewhere
     * has no relative name, and one written as a bare filename simply
     * fails to open later with nothing to say why. */
    mkdir(sub, 0755);
    char subfile[1024];
    snprintf(subfile, sizeof(subfile), "%s/deep.mp3", sub);
    FILE *df = fopen(subfile, "wb"); if (df) { fputc('x', df); fclose(df); }

    FileBrowser fb4;
    FileBrowser_Init(&fb4, sub);
    for (int i = 0; i < fb4.count; i++)
        if (!strcmp(fb4.entries[i].name, "deep.mp3")) fb4.entries[i].marked = true;

    bool wandered = FileBrowser_AppendToPlaylistIn(&fb4, g_root, "edit.m3u");
    n = ParseM3u(path);        /* still the playlist in g_root */

    bool absolute = false;
    for (int i = 0; i < n; i++)
        if (strstr(s_queue[i], "deep.mp3") && s_queue[i][0] == '/') absolute = true;

    snprintf(buf, sizeof(buf), "%d entries, last '%s'", n, n ? s_queue[n-1] : "");
    Check("a track from another folder is written absolute",
          wandered && absolute, buf);

    /* And it went into the playlist's own folder, not the one being
     * browsed - otherwise there are now two playlists of the same name. */
    char stray[1024];
    snprintf(stray, sizeof(stray), "%s/edit.m3u", sub);
    FILE *sf = fopen(stray, "r");
    Check("and into the playlist's own folder", sf == NULL, stray);
    if (sf) fclose(sf);

    /* Saving somewhere other than the folder the tracks are in.
     *
     * The paths have to change with it: a list in /Playlists has no
     * relative way to reach the album it was built from, so a bare
     * filename would name a track that is not there. Getting this wrong
     * produces a playlist that looks right and plays nothing. */
    {
        char plDir[1024];
        snprintf(plDir, sizeof(plDir), "%s/Lists", g_root);

        FileBrowser fb5;
        FileBrowser_Init(&fb5, g_root);
        for (int i = 0; i < fb5.count; i++)
            if (!strcmp(fb5.entries[i].name, "song.mp3")) fb5.entries[i].marked = true;

        char made[FB_NAME_LEN];
        bool ok = FileBrowser_SavePlaylistIn(&fb5, plDir, "Elsewhere", made, sizeof(made));

        char plPath[1200];
        snprintf(plPath, sizeof(plPath), "%s/Elsewhere.m3u", plDir);
        int m = ParseM3u(plPath);

        snprintf(buf, sizeof(buf), "%s -> %d entries, '%s'", made, m,
                 m ? s_queue[0] : "");
        Check("a folder that did not exist is created", ok && m == 1, buf);
        Check("and its tracks are named absolutely",
              m == 1 && s_queue[0][0] == '/', buf);

        /* Saved beside the tracks, the names stay relative - which is
         * what keeps a playlist portable with its album. */
        FileBrowser fb6;
        FileBrowser_Init(&fb6, g_root);
        for (int i = 0; i < fb6.count; i++)
            if (!strcmp(fb6.entries[i].name, "song.mp3")) fb6.entries[i].marked = true;
        FileBrowser_SavePlaylistIn(&fb6, g_root, "Beside", made, sizeof(made));

        snprintf(plPath, sizeof(plPath), "%s/Beside.m3u", g_root);
        m = ParseM3u(plPath);
        snprintf(buf, sizeof(buf), "%d entries, '%s'", m, m ? s_queue[0] : "");
        Check("saved alongside, they stay relative",
              m == 1 && strcmp(s_queue[0], "song.mp3") == 0, buf);
    }

    /* A marked folder stands for the tracks in it.
     *
     * Building a list an album at a time was nine presses on a list nine
     * rows tall, and the mark was the only way to say anything at all.
     * The expansion happens where the file is written, so what is
     * checked here is the file: the right tracks, in the order the list
     * shows them, and nothing that is not audio. */
    {
        char album[1024];
        snprintf(album, sizeof(album), "%s/Whole", g_root);
        mkdir(album, 0777);

        /* Written out of order on purpose - a FAT directory hands its
         * entries back in creation order, and an album that arrives
         * shuffled is worse than one that will not add at all. */
        Write("Whole/03 - Third.mp3", "x");
        Write("Whole/01 - First.mp3", "x");
        Write("Whole/02 - Second.mp3", "x");
        Write("Whole/cover.jpg", "x");            /* not audio */
        Write("Whole/notes.m3u", "#EXTM3U\n");    /* not audio either */

        char deeper[1200];
        snprintf(deeper, sizeof(deeper), "%s/Deeper", album);
        mkdir(deeper, 0777);
        Write("Whole/Deeper/04 - Buried.mp3", "x");

        FileBrowser fbw;
        FileBrowser_Init(&fbw, g_root);
        FileBrowser_ClearMarks(&fbw);
        for (int i = 0; i < fbw.count; i++)
            if (!strcmp(fbw.entries[i].name, "Whole")) fbw.entries[i].marked = true;

        char made[FB_NAME_LEN];
        bool ok = FileBrowser_SavePlaylistIn(&fbw, g_root, "Album", made, sizeof(made));

        char plPath[1200];
        snprintf(plPath, sizeof(plPath), "%s/Album.m3u", g_root);
        int m = ParseM3u(plPath);

        snprintf(buf, sizeof(buf), "%d entries: %s | %s | %s", m,
                 m > 0 ? s_queue[0] : "-", m > 1 ? s_queue[1] : "-",
                 m > 2 ? s_queue[2] : "-");
        Check("one mark on a folder writes every track in it", ok && m == 3, buf);
        Check("in the order the list shows them, not readdir's",
              m == 3 &&
              !strcmp(s_queue[0], "Whole/01 - First.mp3") &&
              !strcmp(s_queue[1], "Whole/02 - Second.mp3") &&
              !strcmp(s_queue[2], "Whole/03 - Third.mp3"), buf);

        /* One level. A folder of folders would need a rule for the ones
         * with no audio in them, and "this album" is what the mark is
         * for. */
        bool buried = false;
        for (int i = 0; i < m; i++)
            if (strstr(s_queue[i], "Buried")) buried = true;
        Check("and stops at one level down", !buried, buf);

        /* Saved somewhere else, the same folder goes in absolute - a
         * list in the playlist folder has no relative way to reach it. */
        char plDir2[1024];
        snprintf(plDir2, sizeof(plDir2), "%s/Lists", g_root);
        FileBrowser fbw2;
        FileBrowser_Init(&fbw2, g_root);
        FileBrowser_ClearMarks(&fbw2);
        for (int i = 0; i < fbw2.count; i++)
            if (!strcmp(fbw2.entries[i].name, "Whole")) fbw2.entries[i].marked = true;
        FileBrowser_SavePlaylistIn(&fbw2, plDir2, "AlbumFar", made, sizeof(made));

        snprintf(plPath, sizeof(plPath), "%s/AlbumFar.m3u", plDir2);
        m = ParseM3u(plPath);
        snprintf(buf, sizeof(buf), "%d entries, first '%s'", m, m ? s_queue[0] : "-");
        Check("a folder saved elsewhere is named absolutely",
              m == 3 && s_queue[0][0] == '/', buf);
    }

    /* The folder the Faves tab is made of does not appear in Files.
     *
     * It has to live somewhere on the card, but showing it in the file
     * manager as well is a second door onto the same room - one that
     * lands the user in a locked browser they then have to climb out
     * of. */
    {
        mkdir(PLAYLIST_DIR, 0777);

        FileBrowser fbr;
        FileBrowser_Init(&fbr, g_root);

        bool sawIt = false;
        for (int i = 0; i < fbr.count; i++)
            if (!strcmp(fbr.entries[i].name, "dsxp_info")) sawIt = true;

        snprintf(buf, sizeof(buf), "%d rows at the root", fbr.count);
        Check("the app's folder is not a row in Files", !sawIt, buf);

        /* Only that one. A folder of the same name elsewhere on the card
         * is somebody else's and stays visible. */
        char elsewhere[1200];
        snprintf(elsewhere, sizeof(elsewhere), "%s/Deeper/dsxp_info", g_root);
        mkdir(elsewhere, 0777);

        char deeperDir[1024];
        snprintf(deeperDir, sizeof(deeperDir), "%s/Deeper", g_root);
        FileBrowser fbd;
        FileBrowser_Init(&fbd, deeperDir);

        bool sawTwin = false;
        for (int i = 0; i < fbd.count; i++)
            if (!strcmp(fbd.entries[i].name, "dsxp_info")) sawTwin = true;
        snprintf(buf, sizeof(buf), "%d rows one folder down", fbd.count);
        Check("one of the same name elsewhere is left alone", sawTwin, buf);
    }

    /* Adding to a list that already exists.
     *
     * The picker used to be built from the playlists in the folder being
     * browsed, and there are never any: marks are made in a music folder
     * and every list is saved to the playlist folder. So the only row it
     * ever offered was "New list", and the one thing a playlist is for -
     * putting another track in it - had no route at all short of opening
     * the list first. Both halves are checked: the lists are found where
     * they actually live, and the append lands in that folder rather
     * than beside the music. */
    {
        char lists[8][FB_NAME_LEN];

        /* A folder that is not there yet is empty, not an error - the
         * picker opens before anything has ever been saved. */
        int n = FileBrowser_ListPlaylistsIn("/nowhere/at/all", lists, 8);
        Check("a missing playlist folder lists nothing", n == 0, NULL);

        mkdir(PLAYLIST_DIR, 0777);
        char path[1024];
        static const char *const made[] = { "Zebra.m3u", "Apple.m3u",
                                            "Middle.M3U", "notalist.txt",
                                            "._Hidden.m3u" };
        for (unsigned i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
            snprintf(path, sizeof(path), "%s/%s", PLAYLIST_DIR, made[i]);
            FILE *f = fopen(path, "w");
            if (f) { fputs("#EXTM3U\n", f); fclose(f); }
        }

        n = FileBrowser_ListPlaylistsIn(PLAYLIST_DIR, lists, 8);
        snprintf(buf, sizeof(buf), "%d found: %s, %s, %s", n,
                 n > 0 ? lists[0] : "-", n > 1 ? lists[1] : "-",
                 n > 2 ? lists[2] : "-");
        Check("the playlists in the playlist folder are found",
              n == 3, buf);
        Check("sorted, with the non-lists and the dotfiles left out",
              n == 3 && !strcmp(lists[0], "Apple.m3u") &&
              !strcmp(lists[1], "Middle.M3U") &&
              !strcmp(lists[2], "Zebra.m3u"), buf);

        /* The marks are in the music folder; the list is not. The
         * tracks have to end up in the list's own file. */
        FileBrowser mus;
        FileBrowser_Init(&mus, g_root);
        int marked = 0;
        for (int i = 0; i < mus.count && marked < 2; i++) {
            if (mus.entries[i].isDir || mus.entries[i].isPlaylist) continue;
            mus.entries[i].marked = true;
            marked++;
        }

        snprintf(buf, sizeof(buf), "%d tracks marked", marked);
        Check("there is music in the folder to add", marked >= 1, buf);

        bool wrote = FileBrowser_AppendToPlaylistIn(&mus, PLAYLIST_DIR,
                                                    "Apple.m3u");
        Check("adding to an existing list succeeds", wrote, NULL);

        snprintf(path, sizeof(path), "%s/Apple.m3u", PLAYLIST_DIR);
        int lines = ParseM3u(path);
        snprintf(buf, sizeof(buf), "%d entries in the list", lines);
        Check("and the tracks are in the list's own file",
              lines == marked, buf);

        /* Absolute, because the list and the music are in different
         * folders - a bare filename there would open nothing. */
        bool absolute = (lines > 0 && s_queue[0][0] == '/');
        Check("written as paths, not bare names beside the list",
              absolute, lines > 0 ? s_queue[0] : "");

        /* Nothing was created next to the music. */
        snprintf(path, sizeof(path), "%s/Apple.m3u", g_root);
        FILE *stray = fopen(path, "r");
        Check("and no second list appears beside the music",
              stray == NULL, NULL);
        if (stray) fclose(stray);

        /* The gate every card write goes through covers this one too. */
        g_blockWrites = true;
        bool blocked = FileBrowser_AppendToPlaylistIn(&mus, PLAYLIST_DIR,
                                                      "Apple.m3u");
        g_blockWrites = false;
        Check("a blocked card refuses the append", !blocked, NULL);
    }

    /* Nothing the player writes may touch a file it did not make.
     *
     * Everything on the card except its own two kinds of file - the
     * playlists and dsxp.cfg - is somebody's music, and the player has
     * no business altering any of it. That is easy to believe from
     * reading the code and worth proving, because the failure would be
     * silent and permanent: a truncating open or a stray remove on the
     * wrong path destroys a file that is not backed up anywhere.
     *
     * So a folder is built with music, a text file, a picture and a
     * subfolder in it, every byte of it is recorded, every write the
     * player can perform is run over it, and then every file is compared
     * again. Only the .m3u files are allowed to have changed. */
    {
        char dir[1024], path[1200];
        snprintf(dir, sizeof(dir), "%s/Sandbox", g_root);
        mkdir(dir, 0777);

        static const struct { const char *name, *body; } kFiles[] = {
            { "01 track.mp3",   "\xff\xfb\x90\x00 pretend audio" },
            { "02 track.mp3",   "\xff\xfb\x90\x00 more audio"    },
            { "notes.txt",      "something the user wrote"        },
            { "cover.jpg",      "\xff\xd8\xff not really a jpeg"  },
            { "keep.m3u8",      "#EXTM3U\nsomething.mp3\n"         },
        };
        const int nFiles = (int)(sizeof(kFiles) / sizeof(kFiles[0]));

        for (int i = 0; i < nFiles; i++) {
            snprintf(path, sizeof(path), "%s/%s", dir, kFiles[i].name);
            FILE *f = fopen(path, "wb");
            if (f) { fputs(kFiles[i].body, f); fclose(f); }
        }
        char sub[1200];
        snprintf(sub, sizeof(sub), "%s/Album", dir);
        mkdir(sub, 0777);
        snprintf(path, sizeof(path), "%s/03 deep.mp3", sub);
        { FILE *f = fopen(path, "wb"); if (f) { fputs("\xff\xfb deep", f); fclose(f); } }

        /* A fingerprint of every file, before anything is asked of it. */
        unsigned long before[8]; long sizeBefore[8];
        for (int i = 0; i < nFiles; i++) {
            snprintf(path, sizeof(path), "%s/%s", dir, kFiles[i].name);
            before[i] = Fingerprint(path, &sizeBefore[i]);
        }

        /* Now every write the player knows how to do, aimed at this
         * folder: mark the tracks, save a list, add to it, take one out,
         * and delete it again. */
        FileBrowser sb;
        FileBrowser_Init(&sb, dir);
        for (int i = 0; i < sb.count; i++)
            if (!sb.entries[i].isDir && !sb.entries[i].isPlaylist)
                sb.entries[i].marked = true;

        char made[FB_NAME_LEN];
        FileBrowser_SavePlaylistIn(&sb, dir, "Mixtape", made, sizeof(made));
        FileBrowser_AppendToPlaylistIn(&sb, dir, "Mixtape.m3u");
        FileBrowser_AppendToPlaylistIn(&sb, dir, "keep.m3u8");

        FileBrowser_Refresh(&sb, NULL);
        if (FileBrowser_EnterPlaylist(&sb, "Mixtape.m3u")) {
            if (sb.count > 1) sb.entries[1].marked = true;
            FileBrowser_RemoveMarkedFromPlaylist(&sb);
            FileBrowser_LeavePlaylist(&sb);
        }

        /* And the delete, pointed at every row in turn - including the
         * ones that are not playlists, which is the case that matters. */
        FileBrowser_Refresh(&sb, NULL);
        for (int i = sb.count - 1; i >= 0; i--)
            FileBrowser_DeletePlaylist(&sb, i);

        /* Nothing that was not a playlist may have moved. */
        int changed = 0, missing = 0;
        const char *worst = "";
        for (int i = 0; i < nFiles; i++) {
            snprintf(path, sizeof(path), "%s/%s", dir, kFiles[i].name);
            long szNow = 0;
            unsigned long now = Fingerprint(path, &szNow);
            const bool isList = strstr(kFiles[i].name, ".m3u") != NULL;
            if (szNow < 0) {
                if (!isList) { missing++; worst = kFiles[i].name; }
            } else if (!isList &&
                       (now != before[i] || szNow != sizeBefore[i])) {
                changed++; worst = kFiles[i].name;
            }
        }
        snprintf(buf, sizeof(buf), "%d altered, %d deleted%s%s",
                 changed, missing, worst[0] ? ", worst: " : "", worst);
        Check("no file but a playlist is ever written or removed",
              changed == 0 && missing == 0, buf);

        /* The subfolder and the track inside it are untouched too - a
         * marked folder is expanded into the list, not moved. */
        snprintf(path, sizeof(path), "%s/03 deep.mp3", sub);
        long deepSz = 0; Fingerprint(path, &deepSz);
        snprintf(buf, sizeof(buf), "%s is %ld bytes", "Album/03 deep.mp3", deepSz);
        Check("a folder that was marked still has its music in it",
              deepSz > 0, buf);

        /* And the delete really can only take a playlist: the .m3u8 the
         * loop above pointed it at is gone, the rest are not. */
        snprintf(path, sizeof(path), "%s/keep.m3u8", dir);
        long listSz = 0; Fingerprint(path, &listSz);
        Check("and it does remove the playlists it is aimed at",
              listSz < 0, listSz < 0 ? "keep.m3u8 gone" : "still there");
    }

    /* Editing the liked list by hand must stick.
     *
     * Favourites answers the heart from a copy in memory and writes that
     * whole copy back whenever a heart is tapped. The list editor writes
     * the same file directly. Three of the paths that rewrite a playlist
     * did not tell Favourites to drop its copy - so removing tracks from
     * Liked through the editor left the copy holding them, and the next
     * heart tapped anywhere put every one of them back.
     *
     * This is the sequence that loses the edit, run against the real
     * thing: remove by hand, then like something else. */
    {
        mkdir(PLAYLIST_DIR, 0777);
        char liked[1024];
        snprintf(liked, sizeof(liked), "%s/%s.m3u", PLAYLIST_DIR, FAVOURITES_NAME);

        /* Three liked tracks, and the cache warmed by asking about one. */
        FILE *f = fopen(liked, "w");
        if (f) {
            fputs("#EXTM3U\n/Music/a.mp3\n/Music/b.mp3\n/Music/c.mp3\n", f);
            fclose(f);
        }
        Favourites_Invalidate();
        Check("the liked list starts with the track in it",
              Favourites_Contains("/Music/b.mp3"), NULL);

        /* Now take it out through the list editor, as the Faves tab does. */
        FileBrowser lb;
        memset(&lb, 0, sizeof(lb));
        snprintf(lb.cwd, FB_PATH_LEN, "%s", PLAYLIST_DIR);
        lb.locked = true;
        char name[FB_NAME_LEN];
        snprintf(name, sizeof(name), "%s.m3u", FAVOURITES_NAME);

        bool entered = FileBrowser_EnterPlaylist(&lb, name);
        int removed = 0;
        for (int i = 0; i < lb.count; i++)
            if (!lb.entries[i].isDir && strstr(lb.entries[i].name, "b.mp3")) {
                lb.entries[i].marked = true; removed++;
            }
        bool wrote = entered && removed == 1 &&
                     FileBrowser_RemoveMarkedFromPlaylist(&lb);
        snprintf(buf, sizeof(buf), "entered %d, marked %d, wrote %d",
                 entered, removed, wrote);
        Check("removing it through the list editor writes the file", wrote, buf);

        /* The heart must agree with the file straight away. */
        Check("and the heart stops claiming it is liked",
              !Favourites_Contains("/Music/b.mp3"), NULL);

        /* And liking something else must not resurrect it. */
        Favourites_Toggle("/Music/d.mp3");

        int sawB = 0, sawD = 0, lines = 0;
        f = fopen(liked, "r");
        if (f) {
            char ln[512];
            while (fgets(ln, sizeof(ln), f)) {
                if (ln[0] == '#' || ln[0] == '\n') continue;
                lines++;
                if (strstr(ln, "b.mp3")) sawB = 1;
                if (strstr(ln, "d.mp3")) sawD = 1;
            }
            fclose(f);
        }
        snprintf(buf, sizeof(buf), "%d entries, removed track %s, new track %s",
                 lines, sawB ? "CAME BACK" : "stayed gone", sawD ? "added" : "missing");
        Check("liking another track does not undo the edit",
              !sawB && sawD, buf);

        /* Put the card back as it was: the favourites tests below start
         * from "nothing is liked yet", and a list left lying here is
         * their starting state rather than this one's leftovers. */
        remove(liked);
        Favourites_Invalidate();
    }

    /* A write that never finishes must not destroy what was there.
     *
     * This is the failure everybody with a flashcart has heard of: the
     * card stops accepting writes, or the console goes off mid-save, and
     * what is left is a truncated file - because fopen(path,"w") throws
     * the old contents away before the first byte of the new ones is
     * written. Every file this program replaces is now staged through a
     * temporary and moved into place only once the data is on the card.
     *
     * What is checked is the property, at each of the three moments it
     * has to hold: while the new contents are being written, after an
     * abandoned write, and after a completed one. */
    {
        char target[1024];
        snprintf(target, sizeof(target), "%s/replace_me.m3u", g_root);

        FILE *f = fopen(target, "w");
        if (f) { fputs("#EXTM3U\n/Music/original.mp3\n", f); fclose(f); }
        long sizeWas = 0;
        unsigned long was = Fingerprint(target, &sizeWas);

        /* Part way through a replacement, the original is still there
         * and still whole. */
        DsxpWrite w;
        bool began = Dsxp_BeginWrite(&w, target);
        if (began) fputs("#EXTM3U\n/Music/replacement.mp3\n", w.f);

        long sizeMid = 0;
        unsigned long mid = Fingerprint(target, &sizeMid);
        snprintf(buf, sizeof(buf), "%ld bytes before, %ld during",
                 sizeWas, sizeMid);
        Check("the old file is untouched while the new one is written",
              began && mid == was && sizeMid == sizeWas, buf);

        /* Abandoned - as a card that has gone read-only would force -
         * and nothing has changed. */
        Dsxp_AbortWrite(&w);
        long sizeAfter = 0;
        unsigned long after = Fingerprint(target, &sizeAfter);
        snprintf(buf, sizeof(buf), "%ld bytes, %s", sizeAfter,
                 after == was ? "identical" : "CHANGED");
        Check("an abandoned write leaves the original exactly as it was",
              after == was && sizeAfter == sizeWas, buf);

        /* And the temporary does not stay behind on the card. */
        char tmp[1100];
        snprintf(tmp, sizeof(tmp), "%s.tmp", target);
        long tmpSize = 0; Fingerprint(tmp, &tmpSize);
        Check("and clears up the temporary it was using", tmpSize < 0, NULL);

        /* A completed one does replace it, and leaves no temporary. */
        if (Dsxp_BeginWrite(&w, target)) {
            fputs("#EXTM3U\n/Music/replacement.mp3\n", w.f);
            Dsxp_CommitWrite(&w, target);
        }
        long sizeNew = 0;
        unsigned long now = Fingerprint(target, &sizeNew);
        Fingerprint(tmp, &tmpSize);
        snprintf(buf, sizeof(buf), "%ld bytes, temporary %s", sizeNew,
                 tmpSize < 0 ? "gone" : "LEFT BEHIND");
        Check("a completed write replaces it and leaves nothing behind",
              now != was && sizeNew > 0 && tmpSize < 0, buf);

        remove(target);
    }

    printf("\n%s\n", g_fail ? "FAILURES" : "ALL PASSED (0 failures)");
    TestFavourites();
    TestLockedFolder();

    return g_fail ? 1 : 0;
}
