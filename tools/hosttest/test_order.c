/* Shuffle and repeat: the play order, not the audio.
 *
 * These are the parts that are easy to get subtly wrong and hard to
 * notice on hardware. A shuffle that picks at random replays tracks while
 * others have not been heard; a repeat that treats "the track ended" and
 * "the user pressed next" as the same event traps the next button; a
 * shuffle that reshuffles on every step has no history to walk back
 * through. All three behave plausibly for a few minutes before anyone
 * spots the fault.
 *
 * player.c is driven directly, with the decoder stubbed out - what is
 * being tested is which entry gets asked for, and in what order.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nds.h"
#include "filebrowser.h"

/* ---- decoder stub ---------------------------------------------------- */

static char g_opened[FB_PATH_LEN + FB_NAME_LEN];
static int  g_openCount;
static int  g_seekZeroCount;

/* How far into the track the player thinks it is. Previous used to
 * behave differently either side of three seconds, so the tests have to
 * be able to say. */
static uint32_t g_elapsedMs;

/* Which track numbers this fixture pretends are missing - a file
 * deleted since the playlist was written, or a path the loader could
 * not make sense of. */
static int g_refuse[8];
static int g_refuseCount;

static bool Refused(const char *path)
{
    const char *slash = strrchr(path, '/');
    int n = slash ? atoi(slash + 1) : -1;
    for (int i = 0; i < g_refuseCount; i++) if (g_refuse[i] == n) return true;
    return false;
}

bool Mp3Decoder_Open(const char *path)
{
    if (Refused(path)) return false;
    snprintf(g_opened, sizeof(g_opened), "%s", path);
    g_openCount++;
    return true;
}
void Mp3Decoder_Close(void) {}
void Mp3Decoder_SeekToMs(uint32_t ms)
{ if (!ms) g_seekZeroCount++; g_elapsedMs = ms; }
void Mp3Decoder_SeekBy(int32_t d) { (void)d; }
uint32_t Mp3Decoder_ElapsedMs(void) { return g_elapsedMs; }
uint32_t Mp3Decoder_TotalMs(void) { return 100000; }
bool Mp3Decoder_AtEnd(void) { return false; }
bool Mp3Decoder_Pump(void) { return true; }
bool Mp3Decoder_PumpIfIdle(void) { return true; }
bool Mp3Decoder_Finished(void) { return false; }
void Mp3Decoder_Prefill(void) {}
/* The tags the fixture pretends to carry. Settable, because what the
 * three display strings are for is exactly the difference between a file
 * with tags and one without. */
static const char *g_tagTitle  = "";
static const char *g_tagArtist = "";
const char *Mp3Decoder_Title(void) { return g_tagTitle; }
const char *Mp3Decoder_Artist(void) { return g_tagArtist; }

void Audio_SetPaused(bool p) { (void)p; }
void Audio_Reset(void) {}
void Audio_SetVolume(int v) { (void)v; }
uint32_t Audio_QueuedFrames(void) { return 0; }
void Audio_SetMuted(bool m) { (void)m; }

/* Resume rescans the track's folder to rebuild the queue.
 *
 * Most of these tests drive the queue directly and want the scan to
 * fail, which is also the real behaviour when a saved path no longer
 * exists. The resume tests at the end turn it on, and then it lists a
 * real folder on disk - because Player_Resume asks the card whether the
 * file is still there before it touches the browser, and a folder that
 * only exists in memory would never get past that. */
static bool g_scanWorks = false;
static char g_scanDir[FB_PATH_LEN];
static int  g_scanCount = 4;

bool FileBrowser_Init(FileBrowser *fb, const char *path)
{
    if (!g_scanWorks) { (void)fb; (void)path; return false; }

    memset(fb, 0, sizeof(*fb));
    snprintf(fb->cwd, sizeof(fb->cwd), "%s", path);
    if (strcmp(path, g_scanDir) != 0) return true;   /* a real but empty folder */

    for (int i = 0; i < g_scanCount; i++) {
        snprintf(fb->entries[fb->count].name, FB_NAME_LEN, "%d.mp3", i);
        fb->count++;
    }
    return true;
}

int  FileBrowser_VisibleRows(void) { return FB_MAX_VISIBLE_ROWS; }
void FileBrowser_SetVisibleRows(int r) { (void)r; }
int  FileBrowser_MarkedCount(const FileBrowser *fb) { (void)fb; return 0; }

#include "player.c"

/* ---- harness --------------------------------------------------------- */

static int g_fail = 0;
static void Check(const char *what, bool ok, const char *detail)
{
    printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

#define N 8

static FileBrowser g_fb;
static PlayerState g_p;

static void Setup(int startIndex)
{
    memset(&g_fb, 0, sizeof(g_fb));
    snprintf(g_fb.cwd, sizeof(g_fb.cwd), "/music");
    for (int i = 0; i < N; i++) {
        snprintf(g_fb.entries[i].name, FB_NAME_LEN, "%d.mp3", i);
        g_fb.count++;
    }
    Player_Init(&g_p);
    Player_SetShuffle(&g_p, false);
    Player_SetRepeat(REPEAT_OFF);
    Player_PlayFromBrowser(&g_p, &g_fb, startIndex);
}

/* Which track index is playing, read back from the path the stub saw. */
static int Playing(void)
{
    const char *slash = strrchr(g_opened, '/');
    return slash ? atoi(slash + 1) : -1;
}

/* What the file list has to match the playing row against.
 *
 * currentName is folded down to the font's alphabet - that is what the
 * Now Playing screen draws - and the browser rows hold what is written
 * on the card. The two are the same string only when folding changed
 * nothing, so a list matching rows against currentName marked ASCII
 * filenames and silently skipped every Persian or accented one: exactly
 * the files the fold exists for.
 *
 * This is the half that proves it. The skin-side check lives in
 * preview.c; between them they pin both ends of the contract. */
static void TestPlayingName(void)
{
    char buf[256];
    printf("\nthe playing row's name\n");

    /* Persian, which folding rewrites. */
    static const char *kRaw = "\xd8\xaf\xd8\xa7\xd8\xb1\xdb\x8c\xd9\x88\xd8\xb4.mp3";

    static FileBrowser fb;
    memset(&fb, 0, sizeof(fb));
    snprintf(fb.cwd, sizeof(fb.cwd), "/music");
    snprintf(fb.entries[0].name, FB_NAME_LEN, "%s", kRaw);
    fb.count = 1;

    Player_Init(&g_p);
    g_refuseCount = 0;
    Player_PlayFromBrowser(&g_p, &fb, 0);

    const char *raw = Player_CurrentRawName();

    snprintf(buf, sizeof(buf), "raw '%s', drawn '%s'",
             raw ? raw : "(none)", g_p.currentName);
    Check("the raw name is what the card holds, not the folded one",
          raw && !strcmp(raw, kRaw), buf);

    /* And the two really are different here - otherwise the check above
     * would pass for a name that needed no folding at all and prove
     * nothing about the case that broke. */
    Check("and folding really did rewrite this one",
          strcmp(g_p.currentName, kRaw) != 0, buf);

    /* Which is the whole point: a list matching on the drawn name could
     * never have found this row. */
    Check("so matching rows on the drawn name could not have worked",
          strcmp(fb.entries[0].name, g_p.currentName) != 0, buf);
}

/* A track that will not open must not end the album.
 *
 * StepOrder handed the caller whatever the open said, and the callers
 * read false as "the queue is finished" - so one deleted file rewound
 * the whole queue and paused it, in the middle, with nothing on screen
 * to say which file was at fault or that anything was. */
static void TestMissingTracks(void)
{
    char buf[200];
    printf("\nmissing tracks\n");

    /* Track 4 has been deleted since the playlist was written. */
    g_refuseCount = 1; g_refuse[0] = 4;
    Setup(3);
    Player_TrackEnded(&g_p);
    snprintf(buf, sizeof(buf), "landed on %d, %s", Playing(),
             g_p.isPlaying ? "playing" : "PAUSED");
    Check("a track that will not open is stepped over",
          Playing() == 5 && g_p.isPlaying, buf);

    /* Several in a row, which is what a moved folder looks like. */
    g_refuseCount = 3; g_refuse[0] = 4; g_refuse[1] = 5; g_refuse[2] = 6;
    Setup(3);
    Player_TrackEnded(&g_p);
    snprintf(buf, sizeof(buf), "landed on %d, %s", Playing(),
             g_p.isPlaying ? "playing" : "PAUSED");
    Check("and so is a run of them", Playing() == 7 && g_p.isPlaying, buf);

    /* Pressing next walks over them too, not just auto-advance. */
    g_refuseCount = 2; g_refuse[0] = 1; g_refuse[1] = 2;
    Setup(0);
    Player_Next(&g_p);
    snprintf(buf, sizeof(buf), "next landed on %d", Playing());
    Check("the next button steps over them as well", Playing() == 3, buf);

    /* A queue in which nothing opens has to give up, not spin. */
    g_refuseCount = 8;
    for (int i = 0; i < 8; i++) g_refuse[i] = i;
    Setup(0);
    bool ended = true;
    Player_TrackEnded(&g_p);        /* must return rather than hang */
    Check("a queue where nothing opens gives up instead of spinning",
          ended && !g_p.isPlaying, "returned");

    g_refuseCount = 0;

    /* And the other way a queue entry comes out unopenable: a playlist
     * line too long for the loader's buffer. fgets hands the tail back
     * as a line of its own, and a tail short enough to look like a path
     * went into the queue. */
    {
        char dir[512], file[700];
        snprintf(dir, sizeof(dir), "m3u_root");
        mkdir(dir, 0755);
        snprintf(file, sizeof(file), "%s/long.m3u", dir);

        static char deep[400] = "/Music";
        while (strlen(deep) < 300) strcat(deep, "/a_fairly_long_folder_name");
        strcat(deep, "/track.mp3");

        FILE *f = fopen(file, "w");
        if (f) {
            fprintf(f, "#EXTM3U\n%s\n/Music/good.mp3\n", deep);
            fclose(f);
        }

        FileBrowser fb;
        memset(&fb, 0, sizeof(fb));
        snprintf(fb.cwd, sizeof(fb.cwd), "%s", dir);
        snprintf(fb.entries[0].name, FB_NAME_LEN, "long.m3u");
        fb.entries[0].isPlaylist = true;
        fb.count = 1;

        Player_Init(&g_p);
        Player_PlayFromBrowser(&g_p, &fb, 0);

        snprintf(buf, sizeof(buf), "%d queued, first '%s'",
                 Player_QueueLength(),
                 Player_QueueLength() ? s_queue[s_order[0]] : "(none)");
        Check("an over-long playlist line does not queue its tail",
              Player_QueueLength() == 1 &&
              !strcmp(s_queue[s_order[0]], "/Music/good.mp3"), buf);

        remove(file);
        rmdir(dir);
    }
}

/* Picking up where the last session stopped.
 *
 * This runs on every start and had no test over it at all. What it has
 * to do: find the track, make its folder the queue so next and previous
 * work, seek to the saved position, and leave the whole thing paused on
 * the file list rather than playing at somebody who has just switched
 * the console on. What it must never do: leave the browser pointing at
 * a folder that is not there. */
static void TestResume(void)
{
    char buf[256], dir[512], path[700];

    printf("\nresume\n");

    /* A real folder with real files, because the resume checks the card
     * before it touches anything. */
    snprintf(dir, sizeof(dir), "%s", "resume_root");
    mkdir(dir, 0755);
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/%d.mp3", dir, i);
        FILE *f = fopen(path, "wb");
        if (f) { fputc('x', f); fclose(f); }
    }
    snprintf(g_scanDir, sizeof(g_scanDir), "%s", dir);
    g_scanWorks = true;

    /* Nothing saved: nothing happens, and the browser is left alone. */
    {
        FileBrowser fb;
        memset(&fb, 0, sizeof(fb));
        snprintf(fb.cwd, sizeof(fb.cwd), "/elsewhere");
        Player_Init(&g_p);
        bool got = Player_Resume(&g_p, &fb, "", 0);
        Check("an empty saved path resumes nothing",
              !got && !strcmp(fb.cwd, "/elsewhere"), fb.cwd);
    }

    /* A track that is gone: refused, and the browser is still untouched
     * - the folder it names would open quite happily and leave the file
     * list sitting on a path that is not on the card. */
    {
        FileBrowser fb;
        memset(&fb, 0, sizeof(fb));
        snprintf(fb.cwd, sizeof(fb.cwd), "/elsewhere");
        Player_Init(&g_p);
        snprintf(path, sizeof(path), "%s/deleted.mp3", dir);
        bool got = Player_Resume(&g_p, &fb, path, 12000);
        Check("a track that has been deleted since is refused",
              !got && !strcmp(fb.cwd, "/elsewhere"), fb.cwd);
    }

    /* And the real thing. */
    {
        FileBrowser fb;
        memset(&fb, 0, sizeof(fb));
        Player_Init(&g_p);
        g_openCount = 0;
        snprintf(path, sizeof(path), "%s/2.mp3", dir);

        bool got = Player_Resume(&g_p, &fb, path, 71000);

        snprintf(buf, sizeof(buf), "opened '%s', %d in the queue, %s, view %d",
                 g_opened, Player_QueueLength(),
                 g_p.isPlaying ? "PLAYING" : "paused", (int)g_p.view);
        Check("resuming opens the track, paused, on the file list",
              got && !g_p.isPlaying && g_p.view == VIEW_FILES &&
              strstr(g_opened, "2.mp3") != NULL, buf);

        snprintf(buf, sizeof(buf), "elapsed %lu", (unsigned long)Mp3Decoder_ElapsedMs());
        Check("and seeks to where the last session stopped",
              Mp3Decoder_ElapsedMs() == 71000, buf);

        /* The folder becomes the queue, or next and previous are dead
         * until the user goes back to the browser themselves. */
        snprintf(buf, sizeof(buf), "%d tracks queued, browser at '%s'",
                 Player_QueueLength(), fb.cwd);
        Check("the track's folder becomes the queue",
              Player_QueueLength() == 4 && !strcmp(fb.cwd, dir), buf);

        /* And it really is a queue: next moves within it. */
        Player_Next(&g_p);
        snprintf(buf, sizeof(buf), "next opened '%s'", g_opened);
        Check("so next steps through the rest of the album",
              strstr(g_opened, "3.mp3") != NULL, buf);
    }

    g_scanWorks = false;
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/%d.mp3", dir, i);
        remove(path);
    }
    rmdir(dir);
}

int main(void)
{
    char buf[160];

    /* ---- sequential ------------------------------------------------- */
    Setup(0);
    bool seq = true;
    for (int i = 1; i < N; i++) {
        Player_Next(&g_p);
        if (Playing() != i) seq = false;
    }
    Check("without shuffle the queue plays in order", seq, NULL);

    /* Past the end with repeat off: back to the first track, paused.
     *
     * The assertion used to be "not playing" alone, which stayed true
     * when the behaviour changed from stopping to rewinding - it would
     * have passed just as happily if the player had landed anywhere, or
     * nowhere. Where it lands is the point: stopping closed the track and
     * left the "Nothing" page at the end of every album. */
    Player_Next(&g_p);
    {
        char b[64];
        snprintf(b, sizeof(b), "playing=%d, track %d", g_p.isPlaying, Playing());
        Check("past the end it rewinds to the first track and pauses",
              !g_p.isPlaying && g_p.hasTrack && Playing() == 0, b);
    }

    /* ---- repeat all ------------------------------------------------- */
    Setup(N - 1);
    Player_SetRepeat(REPEAT_ALL);
    Player_Next(&g_p);
    snprintf(buf, sizeof(buf), "wrapped to %d", Playing());
    Check("repeat-all wraps past the end", g_p.isPlaying && Playing() == 0, buf);

    /* ---- repeat one ------------------------------------------------- */
    Setup(3);
    Player_SetRepeat(REPEAT_ONE);
    int before = g_openCount;
    g_seekZeroCount = 0;
    Player_TrackEnded(&g_p);
    Check("repeat-one restarts the track it just finished",
          g_seekZeroCount == 1 && g_openCount == before, NULL);

    /* The distinction that matters: the user is not trapped on one track
     * just because the player would repeat it on its own. */
    Player_Next(&g_p);
    snprintf(buf, sizeof(buf), "moved to %d", Playing());
    Check("but the next button still moves on", Playing() == 4, buf);

    /* ---- shuffle: a permutation, not a lottery ---------------------- */
    Setup(0);
    Player_SetShuffle(&g_p, true);
    Player_SetRepeat(REPEAT_OFF);

    int seen[N] = { 0 };
    seen[Playing()]++;
    for (int i = 1; i < N; i++) { Player_Next(&g_p); seen[Playing()]++; }

    bool everyOnce = true;
    for (int i = 0; i < N; i++) if (seen[i] != 1) everyOnce = false;
    Check("shuffle plays every track exactly once", everyOnce,
          "8 tracks, 8 steps");

    /* ---- shuffle keeps the chosen track playing --------------------- */
    Setup(5);
    Check("the track picked in the browser is the one playing",
          Playing() == 5, NULL);
    Player_SetShuffle(&g_p, true);
    snprintf(buf, sizeof(buf), "still on %d", Playing());
    Check("turning shuffle on does not jump away from it",
          Playing() == 5, buf);

    /* ---- previous walks the shuffled order back --------------------- */
    Setup(0);
    Player_SetShuffle(&g_p, true);
    int a = Playing();
    Player_Next(&g_p);
    int b = Playing();
    Player_Prev(&g_p);
    snprintf(buf, sizeof(buf), "%d -> %d -> back to %d", a, b, Playing());
    Check("previous returns along the shuffled order",
          Playing() == a && a != b, buf);

    /* ---- the three names a track goes out under -------------------- */
    /* One line or two, and the difference matters on screen.
     *
     * The file list has a single row per track, so it wants the joined
     * form. The cover card has a line for the name and a line under it
     * for the singer - and it was being handed the joined form for the
     * first of them, so the singer appeared on both. Three strings, and
     * what has to hold is that the split ones are actually split. */
    {
        g_tagTitle = "Safar";
        g_tagArtist = "Rozbeh Mosleh";
        Setup(2);

        snprintf(buf, sizeof(buf), "name '%s' | title '%s' | artist '%s'",
                 g_p.displayName, g_p.displayTitle, g_p.displayArtist);
        Check("the title on its own carries no singer",
              !strcmp(g_p.displayTitle, "Safar") &&
              !strcmp(g_p.displayArtist, "Rozbeh Mosleh") &&
              !strcmp(g_p.displayName, "Rozbeh Mosleh - Safar"), buf);

        /* With no tags there is only the filename, and it has to be all
         * three - a card that showed nothing for the name because the
         * file had no title tag would be worse than one that repeats. */
        g_tagTitle = "";
        g_tagArtist = "";
        Setup(2);
        snprintf(buf, sizeof(buf), "name '%s' | title '%s'",
                 g_p.displayName, g_p.displayTitle);
        Check("an untagged file still has a name to show",
              !strcmp(g_p.displayTitle, "2.mp3") &&
              !strcmp(g_p.displayName, "2.mp3"), buf);
    }

    /* ---- a track picked from the middle of a real folder ------------ */
    /* The listing a folder actually produces: the climb-out row, then
     * the sub-folders, then the tracks. The queue is built from the
     * tracks alone, so the row the user touched and the place it lands
     * in the queue are two different numbers - and if they were ever
     * confused for each other, picking the fifth track would start the
     * wrong one and "previous" would walk back through the wrong list.
     *
     * Reported as: pick something in the middle of a folder and previous
     * will not reach the track before it, leaving no way back except
     * starting the folder again from the top. */
    {
        memset(&g_fb, 0, sizeof(g_fb));
        snprintf(g_fb.cwd, sizeof(g_fb.cwd), "/music");

        snprintf(g_fb.entries[0].name, FB_NAME_LEN, "..");
        g_fb.entries[0].isDir = true;
        snprintf(g_fb.entries[1].name, FB_NAME_LEN, "Bootlegs");
        g_fb.entries[1].isDir = true;
        snprintf(g_fb.entries[2].name, FB_NAME_LEN, "Singles");
        g_fb.entries[2].isDir = true;
        for (int i = 0; i < N; i++)
            snprintf(g_fb.entries[3 + i].name, FB_NAME_LEN, "%d.mp3", i);
        g_fb.count = 3 + N;

        Player_Init(&g_p);
        Player_SetShuffle(&g_p, false);
        Player_SetRepeat(REPEAT_OFF);
        g_elapsedMs = 0;

        /* Row 7 of the list, which is track 4 of the folder. */
        Player_PlayFromBrowser(&g_p, &g_fb, 7);
        snprintf(buf, sizeof(buf), "row 7 of the list started track %d",
                 Playing());
        Check("a row past the folders starts the track beside it",
              Playing() == 4, buf);

        /* From the start of a track, where previous steps rather than
         * restarting - which is what walking back through a queue
         * actually looks like. */
        g_elapsedMs = 0;
        Player_Prev(&g_p);
        const int back1 = Playing();
        g_elapsedMs = 0;
        Player_Prev(&g_p);
        snprintf(buf, sizeof(buf), "4 -> %d -> %d", back1, Playing());
        Check("and previous walks back from there",
              back1 == 3 && Playing() == 2, buf);
    }

    /* ---- previous: the start of the track, then the track before ----
     *
     * The reported fault, in the order it happens on the console: open a
     * folder, start a track, listen to it, press previous, press it
     * again.
     *
     * Previous used to restart the current track once three seconds had
     * played, and decide that by reading the elapsed time. A seek is
     * handed to the decoder thread and that figure does not fall until
     * the ring is refilled, so the second press read the same minute and
     * restarted again - a previous button that could never reach the
     * previous track while next always worked.
     *
     * So the second press is checked here with the elapsed time left
     * exactly where it was, which is the state the console is actually
     * in a moment after a seek. If it steps back with the clock still
     * reading a minute, it is not reading the clock. */
    Setup(3);
    Player_SetShuffle(&g_p, false);
    Player_SetRepeat(REPEAT_OFF);

    g_elapsedMs = 60000;
    g_seekZeroCount = 0;
    Player_Prev(&g_p);
    snprintf(buf, sizeof(buf), "a minute in: track %d, %d seeks to zero",
             Playing(), g_seekZeroCount);
    Check("previous returns to the start of the track first",
          Playing() == 3 && g_seekZeroCount == 1, buf);

    /* The clock has not moved, because on the console it would not have
     * yet. */
    Player_Prev(&g_p);
    snprintf(buf, sizeof(buf), "pressed again -> track %d", Playing());
    Check("and the next press steps back, clock or no clock",
          Playing() == 2, buf);

    g_elapsedMs = 60000;
    Player_Prev(&g_p);
    Player_Prev(&g_p);
    snprintf(buf, sizeof(buf), "-> %d", Playing());
    Check("and keeps stepping back, not just the once", Playing() == 1, buf);

    /* At the front of the queue with nothing behind it there is still
     * something sensible to do, and it is the thing the three-second
     * rule was for. */
    Setup(0);
    Player_SetRepeat(REPEAT_OFF);
    g_elapsedMs = 60000;
    g_seekZeroCount = 0;
    int opensBefore = g_openCount;
    Player_Prev(&g_p);
    snprintf(buf, sizeof(buf), "%d seeks to zero, %d tracks opened",
             g_seekZeroCount, g_openCount - opensBefore);
    Check("with nothing behind it, previous restarts the track",
          g_seekZeroCount == 1 && g_openCount == opensBefore, buf);

    /* Repeat-all wraps in both directions or it is not a loop. Next has
     * always wrapped off the end; previous refused to wrap off the
     * front, so the first track was a wall. */
    Setup(0);
    Player_SetShuffle(&g_p, false);
    Player_SetRepeat(REPEAT_ALL);
    g_elapsedMs = 0;
    Player_Prev(&g_p);
    snprintf(buf, sizeof(buf), "track 0, repeat all, previous -> %d", Playing());
    Check("previous wraps to the end the way next wraps to the start",
          Playing() == N - 1, buf);

    g_elapsedMs = 0;

    /* ---- a repeating shuffle is not the same shuffle on loop -------- */
    Setup(0);
    Player_SetShuffle(&g_p, true);
    Player_SetRepeat(REPEAT_ALL);
    int first[N], second[N];
    first[0] = Playing();
    for (int i = 1; i < N; i++) { Player_Next(&g_p); first[i] = Playing(); }
    Player_Next(&g_p);                      /* wraps, reshuffling */
    second[0] = Playing();
    for (int i = 1; i < N; i++) { Player_Next(&g_p); second[i] = Playing(); }

    bool identical = true;
    for (int i = 0; i < N; i++) if (first[i] != second[i]) identical = false;
    bool complete = true;
    int c2[N] = { 0 };
    for (int i = 0; i < N; i++) c2[second[i]]++;
    for (int i = 0; i < N; i++) if (c2[i] != 1) complete = false;
    snprintf(buf, sizeof(buf), "second pass %s, still a permutation: %s",
             identical ? "IDENTICAL" : "differs", complete ? "yes" : "no");
    Check("the second time round is reshuffled", !identical && complete, buf);

    /* ---- what the "up next" line promises --------------------------- */
    Setup(0);
    Player_SetShuffle(&g_p, false);
    Player_SetRepeat(REPEAT_OFF);
    const char *next = Player_NextName();
    Check("next-up names the following track", next && !strcmp(next, "1.mp3"),
          next ? next : "(none)");

    Setup(N - 1);
    Player_SetRepeat(REPEAT_OFF);
    Check("and nothing at the end of the queue",
          Player_NextName() == NULL, NULL);

    Setup(3);
    Player_SetRepeat(REPEAT_ONE);
    next = Player_NextName();
    Check("repeat-one names the current track as next",
          next && !strcmp(next, "3.mp3"), next ? next : "(none)");

    TestResume();
    TestMissingTracks();
    TestPlayingName();

    printf("\n%s\n", g_fail ? "FAILURES" : "ALL PASSED (0 failures)");
    return g_fail ? 1 : 0;
}
