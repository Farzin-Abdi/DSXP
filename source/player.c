#include "player.h"
#include "textfold.h"

#include <nds.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "audio.h"
#include "decoder_mp3.h"

/* Snapshot of the folder that playback started from. Holding our own copy
 * costs ~40KB of a 4MB machine and buys the property that browsing to
 * another folder mid-album does not silently stop the queue from
 * advancing. */
#define PLAYLIST_MAX FB_MAX_ENTRIES

static char s_queue[PLAYLIST_MAX][FB_NAME_LEN];
static int  s_queueCount = 0;
static int  s_queuePos   = -1;
static char s_queueDir[FB_PATH_LEN];

/* The order the queue is played in: s_order[n] is the queue index that
 * comes n-th. Sequential play is the identity permutation, so next and
 * previous walk this array in both modes and there is only one traversal
 * to get right rather than two. */
static int  s_order[PLAYLIST_MAX];
static int  s_orderPos = -1;

/* Bumped every time a track starts. Lets the frame loop notice a change
 * with an integer compare instead of rebuilding and comparing a path. */
static uint32_t s_trackGen;
static bool s_shuffle;
static int  s_repeat = REPEAT_OFF;

/* xorshift, seeded from the clock. The DS has no entropy to speak of, but
 * a shuffle that produced the same order every boot would be noticed
 * immediately, and the tick counter at the moment of the first shuffle is
 * enough to prevent that. */
static uint32_t s_rng;

static uint32_t NextRandom(void)
{
    if (!s_rng) s_rng = (uint32_t)tickGetCount() | 1u;
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int OrderIndexOf(int queueIdx)
{
    for (int i = 0; i < s_queueCount; i++)
        if (s_order[i] == queueIdx) return i;
    return -1;
}

/* Fisher-Yates over the whole queue, then the currently playing track is
 * swapped to the front.
 *
 * Keeping the current track first is what makes turning shuffle on
 * mid-album feel right: the song you are listening to keeps playing and
 * everything after it is rearranged, rather than the player jumping
 * somewhere else the moment the setting changes. */
static void BuildOrder(bool keepCurrent)
{
    int current = (s_queuePos >= 0) ? s_queuePos : -1;

    for (int i = 0; i < s_queueCount; i++) s_order[i] = i;

    if (s_shuffle) {
        for (int i = s_queueCount - 1; i > 0; i--) {
            int j = (int)(NextRandom() % (uint32_t)(i + 1));
            int t = s_order[i]; s_order[i] = s_order[j]; s_order[j] = t;
        }
        if (keepCurrent && current >= 0) {
            int at = OrderIndexOf(current);
            if (at > 0) { s_order[at] = s_order[0]; s_order[0] = current; }
        }
    }

    s_orderPos = (current >= 0) ? OrderIndexOf(current) : -1;
}

static void JoinPath(char *out, size_t outLen, const char *dir, const char *name)
{
    /* An entry that already names a full path is used as it stands.
     * Playlists may hold either, and a track picked from a folder other
     * than the playlist's own is written absolute precisely because
     * there is no relative name for it. */
    if (name[0] == '/') {
        snprintf(out, outLen, "%s", name);
        return;
    }

    size_t dirLen = strlen(dir);
    bool needSlash = (dirLen == 0) || (dir[dirLen - 1] != '/');
    snprintf(out, outLen, "%s%s%s", dir, needSlash ? "/" : "", name);
}

void Player_Init(PlayerState *p)
{
    memset(p, 0, sizeof(*p));
    p->volume = 96;
    p->view = VIEW_FILES;
    p->sleepMinutes = 30;      /* a sensible default if the user just arms it */

    s_queueCount = 0;
    s_queuePos = -1;
    s_orderPos = -1;
    s_queueDir[0] = '\0';

    Audio_SetVolume(p->volume);
    Audio_SetPaused(true);
}

/* Set while a track is being opened that must not be heard.
 *
 * Resuming opens the last track, seeks into it and leaves it paused -
 * and every one of those steps fills the ring, which the mixer plays the
 * moment it is unpaused. StartQueueEntry unpauses at its end, so the
 * seek that followed prefilled into a running mixer: about an eighth of
 * a second of the track's opening, then another of the resumed
 * position, every time the app started.
 *
 * A flag rather than an argument threaded through four call sites and
 * two public functions, all of which want the ordinary behaviour. */
static bool s_openSilent = false;

static bool StartQueueEntry(PlayerState *p, int pos)
{
    if (pos < 0 || pos >= s_queueCount) return false;

    char path[FB_PATH_LEN + FB_NAME_LEN];
    JoinPath(path, sizeof(path), s_queueDir, s_queue[pos]);

    /* Silence the output before tearing the decoder down, so the ring is
     * not still being drained into the mixer while it is reset. */
    Audio_SetPaused(true);

    if (!Mp3Decoder_Open(path)) {
        p->hasTrack = false;
        p->isPlaying = false;
        return false;
    }

    s_queuePos = pos;
    s_trackGen++;
    /* Just the filename on screen: a playlist entry may carry a folder
     * in front of it, and the folder is not what anyone is reading. */
    const char *base = strrchr(s_queue[pos], '/');
    base = base ? base + 1 : s_queue[pos];

    /* Transliterated, like the file list does it. Tags already came
     * through this on the way out of the decoder; a filename used in
     * their place had not, so a Persian file with no tags was a title of
     * question marks on the one screen that shows nothing else. */
    static char shownBuf[FB_NAME_LEN];
    TextFold_Utf8(base, shownBuf, sizeof(shownBuf));
    const char *shown = shownBuf;

    strncpy(p->currentName, shown, sizeof(p->currentName) - 1);
    p->currentName[sizeof(p->currentName) - 1] = '\0';

    /* Tags win over the filename when there are any, because "03.mp3" is
     * not a track name. The artist is included only when it is present
     * and the title is too - "Artist - " in front of a filename would be
     * a half-labelled row. */
    const char *title  = Mp3Decoder_Title();
    const char *artist = Mp3Decoder_Artist();

    snprintf(p->displayTitle, sizeof(p->displayTitle), "%s",
             title[0] ? title : shown);
    snprintf(p->displayArtist, sizeof(p->displayArtist), "%s", artist);

    /* The joined form is still what the file list and the top screen
     * show, where there is only one line to put it on. */
    if (title[0] && artist[0])
        snprintf(p->displayName, sizeof(p->displayName), "%s - %s", artist, title);
    else
        snprintf(p->displayName, sizeof(p->displayName), "%s",
                 title[0] ? title : shown);

    p->hasTrack = true;
    p->isPlaying = !s_openSilent;
    p->trackJustFinished = false;

    /* Build up a cushion before the mixer is allowed to start, or it
     * spends the first buffer's worth of the track playing silence. */
    Mp3Decoder_Prefill();

    Audio_SetVolume(p->volume);
    Audio_SetPaused(s_openSilent);
    return true;
}

/* Reads an M3U into the queue.
 *
 * The format is a list of paths, one per line, with '#' lines carrying
 * metadata this player has no use for. Paths are taken relative to the
 * playlist's own folder, which is what every writer assumes and what
 * makes a playlist survive being copied to a card along with its music.
 * Absolute paths are honoured too, since some writers emit those. */
static int LoadM3u(const char *dir, const char *name)
{
    char path[FB_PATH_LEN + FB_NAME_LEN];
    JoinPath(path, sizeof(path), dir, name);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    strncpy(s_queueDir, dir, sizeof(s_queueDir) - 1);
    s_queueDir[sizeof(s_queueDir) - 1] = '\0';

    s_queueCount = 0;
    char line[FB_NAME_LEN * 2];

    while (s_queueCount < PLAYLIST_MAX && fgets(line, sizeof(line), f)) {
        /* Trim the newline and any stray carriage return: playlists
         * written on a desktop usually arrive with CRLF endings, and a
         * filename with a '\r' welded to it opens nothing. */
        size_t n = strlen(line);
        const bool whole = (n == 0 || line[n - 1] == '\n');
        if (!whole) { int ch; while ((ch = fgetc(f)) != EOF && ch != '\n') { } }

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';

        /* A line too long for the buffer is finished and thrown away
         * whole.
         *
         * fgets stops at 255 characters and hands the rest back as a
         * line of its own, so a 300-character path arrived as a head
         * too long to queue - correctly skipped - followed by a tail
         * short enough to look like a path. That tail went into the
         * queue, and a queue entry that opens nothing does not merely
         * get skipped: it used to stop the album dead. */
        if (!whole) continue;

        if (n == 0 || line[0] == '#') continue;

        /* Backslashes appear in playlists written on Windows. */
        for (size_t i = 0; i < n; i++)
            if (line[i] == '\\') line[i] = '/';

        /* Anything longer than a queue entry holds is skipped rather
         * than truncated: half a path names a file that does not exist,
         * and silently queueing it would just fail later with no clue
         * why. */
        if (n >= FB_NAME_LEN) continue;

        memcpy(s_queue[s_queueCount], line, n + 1);
        s_queueCount++;
    }

    fclose(f);
    return s_queueCount;
}

bool Player_PlayFromBrowser(PlayerState *p, const FileBrowser *fb, int index)
{
    if (index < 0 || index >= fb->count) return false;
    if (fb->entries[index].isDir) return false;

    /* Opening a playlist loads it as the queue rather than trying to
     * decode it. */
    if (fb->entries[index].isPlaylist) {
        if (LoadM3u(fb->cwd, fb->entries[index].name) == 0) return false;
        if (StartQueueEntry(p, 0)) {
            BuildOrder(true);
            p->view = VIEW_NOWPLAYING;
            return true;
        }
        return false;
    }

    strncpy(s_queueDir, fb->cwd, sizeof(s_queueDir) - 1);
    s_queueDir[sizeof(s_queueDir) - 1] = '\0';

    s_queueCount = 0;
    int startPos = 0;

    for (int i = 0; i < fb->count && s_queueCount < PLAYLIST_MAX; i++) {
        if (fb->entries[i].isDir || fb->entries[i].isPlaylist) continue;
        if (i == index) startPos = s_queueCount;
        strncpy(s_queue[s_queueCount], fb->entries[i].name, FB_NAME_LEN - 1);
        s_queue[s_queueCount][FB_NAME_LEN - 1] = '\0';
        s_queueCount++;
    }

    if (s_queueCount == 0) return false;

    if (StartQueueEntry(p, startPos)) {
        /* The order is built after the track starts, so the track the
         * user actually picked is the one shuffle keeps in front. */
        BuildOrder(true);
        p->view = VIEW_NOWPLAYING;
        return true;
    }
    return false;
}

bool Player_Resume(PlayerState *p, FileBrowser *fb,
                   const char *path, uint32_t ms)
{
    if (!path || !path[0]) return false;

    /* Split the path so the folder can become the queue: resuming into a
     * queue of one would leave next and previous dead until the user went
     * back to the browser, which is not what "carry on where I was"
     * means.
     *
     * A track that was reached through a playlist comes back as its own
     * folder rather than as that playlist. The alternative is storing
     * which playlist it was and reloading that, and a playlist can be
     * edited or deleted between sessions - so the folder, which is
     * where the file actually is, is the thing more likely to still be
     * true tomorrow. */
    char dir[FB_PATH_LEN];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return false;

    size_t dirLen = (size_t)(slash - path);
    if (dirLen >= sizeof(dir)) return false;
    memcpy(dir, path, dirLen);
    dir[dirLen] = '\0';

    /* Is it still there, before the browser is disturbed.
     *
     * This matters more than it looks. FileBrowser_Init only fails when
     * the card is not mounted - a folder that does not exist is opened
     * quite happily, and leaves the browser sitting on a dead path with
     * no rows. So without this check, a track deleted or renamed
     * between sessions meant starting the program to an empty file list
     * at a folder that is not on the card, which looks far more like a
     * broken card than a stale bookmark. */
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    /* The caller's browser is borrowed as scratch rather than keeping a
     * second one. A FileBrowser is ~33KB - it holds 256 entries - and a
     * static here meant that much RAM permanently reserved for a function
     * that runs once, at startup, while the real browser sat beside it
     * holding the same kind of data.
     *
     * It is left showing the resumed track's folder, which is where the
     * user would want to be anyway. */
    FileBrowser *scratch = fb;
    if (!FileBrowser_Init(scratch, dir)) return false;

    for (int i = 0; i < scratch->count; i++) {
        if (scratch->entries[i].isDir || scratch->entries[i].isPlaylist) continue;
        if (strcmp(scratch->entries[i].name, slash + 1) != 0) continue;

        /* Silent for the whole of it - the open, the prefill and the
         * seek - rather than paused after the fact. Pausing afterwards
         * still leaves the mixer running across the seek, which is where
         * the fragment of music at start-up came from. */
        s_openSilent = true;
        bool ok = Player_PlayFromBrowser(p, scratch, i);
        if (ok && ms) Mp3Decoder_SeekToMs(ms);
        s_openSilent = false;

        if (!ok) return false;

        /* On the file list rather than Now Playing: resuming should offer
         * to continue, not take over. */
        p->isPlaying = false;
        Audio_SetPaused(true);
        p->view = VIEW_FILES;
        return true;
    }

    /* The file passed the check above and the folder still did not
     * offer it - a name too long for a row, or a card that changed
     * under us between the two. The browser has been pointed somewhere
     * by now either way, so it is put back on the root rather than left
     * wherever this got to. */
    FileBrowser_Init(scratch, "/");
    return false;
}

void Player_TogglePlayPause(PlayerState *p)
{
    if (!p->hasTrack) return;

    p->isPlaying = !p->isPlaying;
    Audio_SetPaused(!p->isPlaying);
}

void Player_Stop(PlayerState *p)
{
    Audio_SetPaused(true);
    Audio_Reset();
    Mp3Decoder_Close();

    p->hasTrack = false;
    p->isPlaying = false;
    p->trackJustFinished = false;
    p->currentName[0] = '\0';
}

/* Steps `delta` places through the play order. `wrap` says what happens
 * at the ends: the user pressing next gets to wrap when repeat-all is on,
 * and a track finishing naturally follows the same rule. */
static bool StepOrder(PlayerState *p, int delta, bool wrap)
{
    if (s_queueCount <= 0) return false;

    /* No position yet: the first step lands on an end of the order
     * rather than one place past it.
     *
     * Planting a synthetic zero and adding delta meant "next" would start
     * at order[1], skipping the first track, and "previous" would ask for
     * order[-1] and stop. Not reachable through the public API today -
     * s_queuePos is only negative before anything has played, and
     * BuildOrder takes the order position from it - so this is a guard on
     * an invariant rather than a fix for an observed fault. It is written
     * down because the arithmetic was wrong either way. */
    if (s_orderPos < 0) {
        BuildOrder(false);
        s_orderPos = (delta >= 0) ? 0 : s_queueCount - 1;
        if (StartQueueEntry(p, s_order[s_orderPos])) return true;
        /* Would not open - fall into the walk below and try the next. */
    }

    /* A track that will not open is stepped over, not stopped on.
     *
     * This used to hand the caller whatever StartQueueEntry said, and
     * the callers read false as "the queue is finished": one track in a
     * playlist that had been deleted, renamed, or written as a path
     * this build cannot parse would rewind the whole queue and pause
     * it. The album stopped in the middle with nothing on screen to say
     * which file was at fault or that anything was.
     *
     * Bounded by the length of the queue, so a playlist in which
     * nothing at all opens gives up after one pass instead of spinning
     * through it forever. */
    for (int tries = 0; tries < s_queueCount; tries++) {
        int next = s_orderPos + delta;

        if (next >= s_queueCount) {
            if (!wrap) return false;
            /* A fresh permutation each time round, so a repeating
             * shuffle is not the same shuffle on loop. */
            if (s_shuffle) BuildOrder(false);
            next = 0;
        } else if (next < 0) {
            if (!wrap) return false;
            next = s_queueCount - 1;
        }

        s_orderPos = next;
        if (StartQueueEntry(p, s_order[next])) return true;
    }

    return false;
}

/* Off the end of the queue with repeat off: back to the first track, and
 * paused there.
 *
 * Stopping was what happened before, and it closed the track and cleared
 * the queue position - so the last song of an album finishing left the
 * player on the "Nothing" page with the evening's listening gone, and
 * getting back meant walking the file browser again. Rewinding leaves it
 * exactly where somebody would want to start from, with one button
 * between them and it.
 *
 * Paused rather than playing, because a queue that has run out has said
 * what it wants to do: nothing, until asked. */
static void RewindToStartPaused(PlayerState *p)
{
    if (s_queueCount <= 0) { Player_Stop(p); return; }

    if (s_orderPos < 0) BuildOrder(false);
    s_orderPos = 0;

    if (!StartQueueEntry(p, s_order[0])) { Player_Stop(p); return; }

    p->isPlaying = false;
    Audio_SetPaused(true);
}

void Player_Next(PlayerState *p)
{
    /* Repeat-one does not trap this button: it governs what happens when
     * a track runs out, not what the user is allowed to ask for. */
    if (!StepOrder(p, +1, s_repeat != REPEAT_OFF))
        RewindToStartPaused(p);
}

void Player_TrackEnded(PlayerState *p)
{
    if (s_repeat == REPEAT_ONE) {
        Mp3Decoder_SeekToMs(0);
        return;
    }
    if (!StepOrder(p, +1, s_repeat == REPEAT_ALL))
        RewindToStartPaused(p);
}

void Player_SetShuffle(PlayerState *p, bool on)
{
    (void)p;
    if (on == s_shuffle) return;
    s_shuffle = on;
    BuildOrder(true);           /* whatever is playing keeps playing */
}

bool Player_Shuffle(void) { return s_shuffle; }

void Player_SetRepeat(int mode)
{
    s_repeat = (mode >= 0 && mode < REPEAT_MODES) ? mode : REPEAT_OFF;
}

int Player_Repeat(void) { return s_repeat; }

/* Whether previous last restarted a track rather than stepping back,
 * and when. Both: a bare timestamp of zero reads as "a moment ago" for
 * the first two seconds after the console starts, which is long enough
 * for the first press of the session to do the wrong thing. */
static bool     s_prevRestarted = false;
static uint64_t s_prevRestartAt = 0;

void Player_Prev(PlayerState *p)
{
    /* Back to the start, then back a track - the way every other player
     * behaves, and the way this one used to before it was made to always
     * step.
     *
     * Always stepping was itself a fix, for a real complaint: previous
     * never moved. But the diagnosis was wrong. The old code asked how
     * far into the track it was, restarted if that was over three
     * seconds, and expected the next press to see nearly zero and step
     * back. What it actually saw was the old position: a seek is handed
     * to the decoder thread and the elapsed figure does not fall until
     * the ring has been refilled, so a second press a moment later read
     * the same two minutes and restarted again. Next always moved and
     * previous never did.
     *
     * So the second press does not ask where it is. It asks whether the
     * previous press restarted, which is a fact this function knows for
     * certain because it is the one that did it.
     *
     * The wrap follows the same rule as next. Without that, repeat-all
     * wrapped forward off the end of the queue but refused to wrap
     * backward off the front, which is the other half of the same
     * complaint. */
    const uint64_t now = tickGetCount();
    const bool justRestarted = s_prevRestarted &&
                               (now - s_prevRestartAt) < (uint64_t)TICK_FREQ * 2;

    if (p->hasTrack && !justRestarted && Mp3Decoder_ElapsedMs() > 3000) {
        Mp3Decoder_SeekToMs(0);
        s_prevRestarted = true;
        s_prevRestartAt = now;
        return;
    }

    s_prevRestarted = false;
    if (StepOrder(p, -1, s_repeat != REPEAT_OFF)) return;

    if (p->hasTrack) Mp3Decoder_SeekToMs(0);
}

void Player_SeekBy(PlayerState *p, int32_t deltaMs)
{
    if (!p->hasTrack) return;
    Mp3Decoder_SeekBy(deltaMs);
}

void Player_SeekToPermille(PlayerState *p, int permille)
{
    if (!p->hasTrack) return;

    uint32_t total = Mp3Decoder_TotalMs();
    if (total == 0) return;

    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;

    Mp3Decoder_SeekToMs((uint32_t)(((uint64_t)total * permille) / 1000));
}

void Player_SetVolume(PlayerState *p, int volume0to127)
{
    if (volume0to127 < 0) volume0to127 = 0;
    if (volume0to127 > 127) volume0to127 = 127;

    p->volume = volume0to127;
    Audio_SetVolume(p->volume);
}

void Player_AdjustVolume(PlayerState *p, int delta)
{
    Player_SetVolume(p, p->volume + delta);
}

const char *Player_NextName(void)
{
    /* Follows the play order, not the queue. In shuffle this line is the
     * only way to know what is coming, and a "next up" that named the
     * following file on the card would simply be wrong. */
    if (s_orderPos < 0 || s_queueCount <= 0) return NULL;

    const char *raw;

    if (s_repeat == REPEAT_ONE) {
        raw = s_queue[s_order[s_orderPos]];
    } else {
        int next = s_orderPos + 1;
        if (next >= s_queueCount) {
            if (s_repeat != REPEAT_ALL) return NULL;
            /* Wrapping is real, but the order is reshuffled on the wrap,
             * so naming a track here would be a guess. */
            if (s_shuffle) return NULL;
            next = 0;
        }
        raw = s_queue[s_order[next]];
    }

    /* Transliterated here rather than by whoever draws it.
     *
     * The track that is playing goes through this in StartQueueEntry, so
     * a Persian filename reads on the Now Playing screen. This one did
     * not, and there are two places that draw it - one of them folded it
     * and one did not, so the same name was readable on one top screen
     * and a row of question marks on another. Doing it at the source is
     * one place instead of two, and the next caller gets it for free.
     *
     * A playlist line can carry a folder in front of it, and the folder
     * is not what anyone is reading. */
    const char *base = strrchr(raw, '/');
    base = base ? base + 1 : raw;

    static char shown[FB_NAME_LEN];
    TextFold_Utf8(base, shown, sizeof(shown));
    return shown;
}

uint32_t Player_TrackGeneration(void) { return s_trackGen; }

const char *Player_CurrentRawName(void)
{
    if (s_queuePos < 0 || s_queuePos >= s_queueCount) return NULL;

    /* A playlist line can carry a folder in front of it; the file list
     * shows rows by their own name, so the comparison is between the
     * last components of each. */
    const char *raw = s_queue[s_queuePos];
    const char *slash = strrchr(raw, '/');
    return slash ? slash + 1 : raw;
}

const char *Player_CurrentPath(void)
{
    static char path[FB_PATH_LEN + FB_NAME_LEN];
    if (s_queuePos < 0 || s_queuePos >= s_queueCount) return NULL;
    JoinPath(path, sizeof(path), s_queueDir, s_queue[s_queuePos]);
    return path;
}

int Player_QueuePosition(void) { return s_orderPos >= 0 ? s_orderPos : s_queuePos; }
int Player_QueueLength(void)   { return s_queueCount; }

/* ------------------------------------------------------------------ */
/* sleep timer                                                        */
/* ------------------------------------------------------------------ */

void Player_SetSleepMinutes(PlayerState *p, int minutes)
{
    if (minutes < PLAYER_SLEEP_MIN_MINUTES) minutes = PLAYER_SLEEP_MIN_MINUTES;
    if (minutes > PLAYER_SLEEP_MAX_MINUTES) minutes = PLAYER_SLEEP_MAX_MINUTES;

    p->sleepMinutes = minutes;

    /* Dragging the slider while it is already running restarts the
     * countdown from the new value, which is what "set it to 30" means. */
    if (p->sleepArmed)
        p->sleepDeadline = tickGetCount() + (uint64_t)minutes * 60 * TICK_FREQ;
}

void Player_SetSleepArmed(PlayerState *p, bool armed)
{
    p->sleepArmed = armed;
    if (armed)
        p->sleepDeadline = tickGetCount() + (uint64_t)p->sleepMinutes * 60 * TICK_FREQ;
}

void Player_ToggleSleep(PlayerState *p)
{
    Player_SetSleepArmed(p, !p->sleepArmed);
}

void Player_ToggleSleepPowerOff(PlayerState *p)
{
    p->sleepPowerOff = !p->sleepPowerOff;
}

uint32_t Player_SleepRemainingSec(const PlayerState *p)
{
    if (!p->sleepArmed) return 0;

    uint64_t now = tickGetCount();
    if (now >= p->sleepDeadline) return 0;

    /* Deliberately a real clock rather than a frame count: the frame rate
     * varies with what is on screen, and a timer that runs slow whenever
     * the plasma visualizer is up would be a strange thing to ship. */
    return (uint32_t)((p->sleepDeadline - now) / TICK_FREQ);
}

void Player_Update(PlayerState *p)
{
    /* Checked before the early-out below so the timer still fires while
     * paused - otherwise pausing would quietly suspend it forever. */
    if (p->sleepArmed && tickGetCount() >= p->sleepDeadline) {
        p->sleepArmed = false;
        p->sleepExpired = true;

        /* Paused, not stopped. Stopping closed the track and emptied the
         * queue position, so the player came back to the "Nothing" page
         * with the evening's listening thrown away - and a sleep timer is
         * not a request to forget where you were. Pausing leaves the
         * track loaded and the position intact, so waking up is one
         * button. */
        if (p->isPlaying) {
            p->isPlaying = false;
            Audio_SetPaused(true);
        }
        return;
    }

    if (!p->hasTrack || !p->isPlaying) return;

    /* The worker thread does the decoding now, in the time the main
     * thread spends blocked on vblank. All that is left here is noticing
     * when it has run the track out.
     *
     * The synchronous pump stays as a backstop, and only fires if the ring
     * has fallen below an eighth - about 60ms of audio. That should never
     * happen: the worker has a whole frame of idle CPU to keep 500ms of
     * buffer topped up. But if some future change ever does starve it,
     * this degrades to exactly the behaviour that was here before the
     * thread existed - a stutter in the UI - rather than to a dropout in
     * the audio, and audio is the one that must not break. */
    if (Audio_QueuedFrames() < (AUDIO_RING_FRAMES / 8))
        Mp3Decoder_PumpIfIdle();

    if (Mp3Decoder_Finished()) {
        p->isPlaying = false;
        p->trackJustFinished = true;
    }
}
