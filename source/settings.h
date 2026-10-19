#ifndef DSXP_SETTINGS_H
#define DSXP_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "decoder_mp3.h"
#include "filebrowser.h"
#include "gfx.h"
#include "skin.h"
#include "visualizer.h"

/* Persisted preferences, stored as a small key=value text file at the
 * card root so it can be inspected or edited by hand.
 *
 * Writes are debounced rather than immediate: dragging the volume slider
 * changes the value on every frame, and committing each of those to the
 * card would hammer the filesystem and hitch playback. */

typedef enum {
    LID_KEEP_PLAYING = 0,   /* default: screens blank, music continues */
    LID_SLEEP,              /* hand the lid back to calico: true sleep */
} LidAction;

typedef enum {
    AUDIO_QUALITY_STANDARD = 0, /* shorter resampling filter, less CPU */
    AUDIO_QUALITY_HIGH,         /* default */
} AudioQuality;

/* Output gain in whole dB, applied after the resampler. The resampler
 * keeps 2.2dB of headroom so transients cannot clip, so +2 hands exactly
 * that back; everything above pushes past unity and will clip loud
 * material, which is the caller's choice to make. */
#define GAIN_MAX_DB 20

/* How many stops the two diagnostics ladders have.
 *
 * Here, rather than beside the tables they index in main.c, because
 * this is the file that records what range a setting may hold -
 * GAIN_MAX_DB above does the same job - and the loader has to be able
 * to clamp against them. While these lived only in main.c it could not,
 * so Sanitise bounded both of these below and neither above, and a
 * config file saying "ratecut=99" indexed a five-entry table with 99.
 * One of those entries is a const char * that goes straight to
 * snprintf, which is a wild pointer dereference out of a text file we
 * invite people to edit.
 *
 * main.c static-asserts its tables against these, so the two cannot
 * drift apart in the other direction either. */
#define RATE_CUT_STOPS    5
#define SCROLL_RATE_STOPS 4

typedef struct {
    int  volume;            /* 0..127 */
    int  gain;              /* 0..GAIN_MAX_DB */
    int  lidAction;         /* LidAction */
    int  sleepMinutes;
    bool sleepPowerOff;
    int  resampleMode;      /* RESAMPLE_* */
    /* What the top screen shows: a meter, one of the cards, or nothing.
     * One number from the list in visualizer.h, where it used to be a
     * mode and a style that had to agree with each other. */
    int  topChoice;         /* TOP_* / VIS_* */
    /* Whether that choice was made by the listener rather than handed
     * over by a theme. Each look was drawn around a particular top
     * screen and brings it along - but only until somebody picks one
     * themselves, after which changing the look must not take it back. */
    bool topChosen;
    int  theme, accent;     /* look */
    int  fontStyle;
    int  listScale;         /* 1 or 2 */
    /* Which sound preset is in force. An index into the player's own
     * table, clamped on load - a file from a build with a longer list
     * must not select past the end of a shorter one. */
    int  tonePreset;
    int  jackGuard;         /* JACK_GUARD_* - headphone socket        */
    bool shuffle;
    int  repeatMode;        /* REPEAT_* */

    /* Whether this program is allowed to write to the card at all.
     *
     * A diagnostic, and a blunt one on purpose. Cards have turned up
     * corrupted after a power-off and the write path here checks out -
     * fclose syncs through to the card, and the decoder and UI threads
     * are locked against each other - so the app cannot be shown to be
     * at fault by reading it. Turning every write off can: if a card
     * still corrupts with this clear, nothing this program does is
     * responsible, and the question moves to the flashcart. */
    bool cardWrites;

    /* The diagnostics page's own settings.
     *
     * They were plain statics in main.c, which meant every one of them
     * went back to its default on the next run - so the page could be
     * used to find a setting that helped and then could not keep it.
     * They are settings, so they live with the settings. */
    int  rateCut;           /* 0..RATE_CUT_STOPS-1, 0 is full rate */
    bool ignoreCrc;
    int  scrollRate;        /* index into the player's step table */
    int  artScale;          /* how much of a cover block is transformed */
    int  artPace;

    /* Two opt-in output effects, both zero (off) by default. They live
     * on the Test page because both cost processor. */
    int  noiseFilter;
    int  enhance;           /* how hard a cover competes for the CPU */

    /* Where playback had got to when the console was last put down, so
     * an album or an audiobook is not started again from zero. Empty
     * when there is nothing to carry over.
     *
     * This was here once before and was taken out, because it was
     * written on every track change - a card write in the middle of
     * playback, which is the single most likely thing to interrupt audio
     * on a flashcart. It is back on one condition, enforced by the
     * caller rather than here: the position is only ever recorded at a
     * moment when nothing is playing. A write while paused cannot
     * interrupt anything, and there are a handful of them in a session
     * rather than one per track.
     *
     * Sized to hold whatever Player_CurrentPath can return. The reader
     * in settings.c has to be sized against this and not guessed at;
     * the last time they disagreed the path came back in two pieces. */
    char resumePath[FB_PATH_LEN + FB_NAME_LEN];
    int  resumeMs;

} Settings;

/* Fills `out` with defaults, then overlays whatever the file contains.
 * Never fails: a missing or malformed file just leaves the defaults. */
void Settings_Load(Settings *out);

/* Turns card writing on or off, and makes the choice stick.
 *
 * Disabling performs one last write to record it, so a card that is being
 * tested stays untouched across reboots instead of quietly allowing
 * writes again at the next start. Enabling clears the block first, so the
 * write that records it is permitted. */
void Settings_SetCardWrites(Settings *s, bool allow);

/* True when writes are blocked. Anything that touches the card must ask
 * first - this is not confined to the settings file. */
bool Settings_CardWritesBlocked(void);

/* Records that something changed. The actual write happens later, from
 * Settings_Tick(). */
void Settings_MarkDirty(void);

/* Call once per frame. Writes the file if it is dirty and has been
 * settled for a moment. */
void Settings_Tick(const Settings *s);

/* Writes immediately if dirty - for shutdown paths, where there is no
 * later. */
void Settings_Flush(const Settings *s);

#endif /* DSXP_SETTINGS_H */
