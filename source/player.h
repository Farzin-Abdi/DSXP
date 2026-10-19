#ifndef DSXP_PLAYER_H
#define DSXP_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "filebrowser.h"

typedef enum {
    VIEW_FILES = 0,
    VIEW_FAVES,
    VIEW_NOWPLAYING,
    VIEW_SLEEP,
    VIEW_SETUP,
    VIEW_TEST,

    /* Reached from Setup and returns to it, like Test. It keeps the Setup
     * tab lit rather than claiming one of its own: it is a choice being
     * made inside Setup, not a place to be. */
    VIEW_COLORS,
    VIEW_THEMES,
    VIEW_FONTS,
    VIEW_METERS,
    VIEW_SOUND,
    VIEW_REGS,

    VIEW_COUNT
} PlayerView;

typedef struct {
    bool hasTrack;
    bool isPlaying;            /* false while paused; hasTrack stays true */
    int  volume;               /* 0..127 */
    /* The playing track's filename, transliterated for the font.
     *
     * Nothing draws it - the three display fields below are what reach
     * the screen - and nothing matches rows against it. It survives as
     * a cheap way to notice a track change between frames.
     *
     * It used to be the name as written on the card, and the comment
     * here said so: "it is matched against directory entries, so it has
     * to stay the name on the card". Then the transliteration was added
     * for a screen that needed it, the comment stayed, and the file list
     * went on matching rows against a string that no longer equalled any
     * filename it would ever see - so the playing row was marked for
     * ASCII names and for nothing else. The list asks
     * Player_CurrentRawName now, which says in its own name what it is. */
    char currentName[FB_NAME_LEN];

    /* What to put on screen. The tag title if the file has one, with the
     * artist in front of it, otherwise the filename. */
    char displayName[FB_NAME_LEN];

    /* The two halves separately, so the screen can size them differently.
     * `displayTitle` is the tag title or the filename; `displayArtist` is
     * empty when the file has no artist tag, and the layout closes up
     * around it rather than leaving a blank line. */
    char displayTitle[FB_NAME_LEN];
    char displayArtist[FB_NAME_LEN];
    PlayerView view;

    /* Set by Player_Update() when a track reaches its natural end, and
     * only then - never on a manual stop or pause. main.c clears it after
     * deciding whether to auto-advance. */
    bool trackJustFinished;

    /* Sleep timer. `sleepMinutes` is what the slider is set to and
     * survives the timer being cancelled, so re-arming keeps the last
     * choice; `sleepArmed` says whether it is counting down. */
    int      sleepMinutes;
    bool     sleepArmed;
    uint64_t sleepDeadline;    /* tick count, valid while armed */

    /* When set, expiry shuts the console down instead of merely stopping
     * playback. Kept separate from `sleepArmed` so the choice survives
     * cancelling and re-arming the timer. */
    bool     sleepPowerOff;

    /* Blank the screens after a stretch with no input. This is the only
     * way to get automatic blanking on hardware where the lid cannot be
     * detected - see the lid notes in README. */

    /* Raised for one iteration when the timer runs out, so main.c can
     * carry out the shutdown - powering off is a platform decision and
     * does not belong in the transport logic. */
    bool     sleepExpired;
} PlayerState;

#define PLAYER_SLEEP_MIN_MINUTES 1
#define PLAYER_SLEEP_MAX_MINUTES 120

void Player_Init(PlayerState *p);

/* Starts the file at `index` in `fb` and snapshots that folder's playable
 * files as the play queue. The queue is captured rather than re-read from
 * the browser so that next/previous and auto-advance keep working through
 * the rest of the album after the user browses off to another folder. */
bool Player_PlayFromBrowser(PlayerState *p, const FileBrowser *fb, int index);

/* Reopens `path` at `ms` and leaves it paused on the file list, so the
 * listener is offered their place back rather than having it played at
 * them. The track's folder becomes the queue, and `fb` is pointed at
 * that folder - it is borrowed as scratch for the directory read, so it
 * must be the browser the caller is about to show.
 *
 * False, harmlessly, for anything that no longer adds up: an empty
 * path, a folder that will not open, a file that has been moved or
 * deleted since. The player then simply starts idle, which is what it
 * did before there was a resume at all, and `fb` is left on a folder
 * that exists - untouched if the path was rejected before it was
 * needed, and otherwise put back on the root. */
bool Player_Resume(PlayerState *p, FileBrowser *fb,
                   const char *path, uint32_t ms);

void Player_TogglePlayPause(PlayerState *p);
void Player_Stop(PlayerState *p);
void Player_Next(PlayerState *p);
void Player_Prev(PlayerState *p);

/* ---- play order ----------------------------------------------------- */

typedef enum {
    REPEAT_OFF = 0,   /* stop at the end of the queue   */
    REPEAT_ALL,       /* wrap round to the beginning    */
    REPEAT_ONE,       /* replay this track              */
    REPEAT_MODES,
} RepeatMode;

/* Shuffle is a permutation of the queue, not a random pick each time.
 *
 * Picking at random would replay tracks before others had been heard once
 * and would make "previous" meaningless, because there would be no order
 * to step back through. A shuffled order gives every track exactly one
 * turn, a working history, and a fresh arrangement each time the queue is
 * exhausted. */
void Player_SetShuffle(PlayerState *p, bool on);
bool Player_Shuffle(void);

void Player_SetRepeat(int mode);
int  Player_Repeat(void);

/* Called when a track ends of its own accord, as opposed to the user
 * pressing next. The two differ: repeat-one replays the track here, but
 * must not trap the next button on the same track forever. */
void Player_TrackEnded(PlayerState *p);

void Player_SeekBy(PlayerState *p, int32_t deltaMs);
void Player_SeekToPermille(PlayerState *p, int permille);

void Player_AdjustVolume(PlayerState *p, int delta);
void Player_SetVolume(PlayerState *p, int volume0to127);

/* The track that will start when this one ends, or NULL at the end of the
 * queue. Borrowed pointer into the queue, valid until it is rebuilt. */
const char *Player_NextName(void);

/* The full path of the track playing, or NULL. Rebuilds the path each
 * call, so it is for the moment something is saved rather than for
 * polling - see Player_TrackGeneration. */
const char *Player_CurrentPath(void);

/* Increments whenever a track starts. The cheap way to notice a change:
 * comparing this is an integer compare, where asking for the path is a
 * snprintf and a string compare of a couple of hundred characters. */
uint32_t Player_TrackGeneration(void);

/* The playing track's filename as it is written on the card, not folded
 * down to the font's alphabet.
 *
 * The file list marks the playing row by matching it against this, and
 * the rows hold what the card holds - so the thing compared against
 * them has to be the same. `currentName` is the transliterated form,
 * which is right for drawing and wrong for comparing: it is equal to
 * the filename only when folding changed nothing, so the match
 * succeeded for ASCII names and silently failed for every Persian or
 * accented one. Those are exactly the files that need the fold. */
const char *Player_CurrentRawName(void);


/* Position in the play queue, for a "3 of 12" style readout. */
int Player_QueuePosition(void);
int Player_QueueLength(void);

/* Writes the browser's marked entries to a new "Playlist NN.m3u" in the
 * current folder and reports the name it chose. False if nothing is
 * marked or the card refused the write. */


/* Sleep timer. Setting the length does not start it; arming does. When it
 * expires playback stops, which is the whole point - it is for falling
 * asleep to an album without the battery being flat in the morning. */
void Player_SetSleepMinutes(PlayerState *p, int minutes);
void Player_SetSleepArmed(PlayerState *p, bool armed);
void Player_ToggleSleep(PlayerState *p);
void Player_ToggleSleepPowerOff(PlayerState *p);

/* Seconds left, or 0 when not armed. */
uint32_t Player_SleepRemainingSec(const PlayerState *p);

/* Pumps the decoder. Call once per main-loop iteration. */
void Player_Update(PlayerState *p);

#endif /* DSXP_PLAYER_H */
