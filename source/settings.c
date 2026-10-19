#include "settings.h"
#include "jack.h"

#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "decoder_mp3.h"
#include "player.h"

/* Overridable so the host test can round-trip through a temp file. On
 * the card it sits in the app's own folder rather than at the root; see
 * DSXP_DIR in filebrowser.h. */
#ifndef SETTINGS_PATH
#define SETTINGS_PATH DSXP_CFG_PATH
#endif

/* Debounce before committing to the card. Volume is changed by dragging a
 * slider, which produces a new value every frame; writing each one would
 * pound the filesystem and hitch playback for no benefit. */
#define SETTINGS_SETTLE_TICKS (TICK_FREQ * 3)

static bool     s_dirty = false;
static uint64_t s_dirtyAt = 0;

/* Consecutive failed writes.
 *
 * A card that will not take the settings file is not going to start
 * taking it because it was asked again immediately. After this many
 * tries, a settle period apart, the change is dropped and the card is
 * left alone until something else changes. */
#define SETTINGS_MAX_FAILS 4
static int s_writeFails = 0;

static bool s_blockWrites = false;

bool Settings_CardWritesBlocked(void) { return s_blockWrites; }

void Settings_MarkDirty(void)
{
    s_dirty = true;
    s_dirtyAt = tickGetCount();
}

/* What an older config said about the top screen, held until the whole
 * file has been read: the two keys can arrive in either order, and the
 * merged value depends on both. */
static bool s_sawTop = false, s_sawOld = false;
static int  s_oldMode = 0, s_oldMeter = 0;

static void ApplyLine(Settings *s, const char *key, int value)
{
    if      (!strcmp(key, "volume"))       s->volume = value;
    else if (!strcmp(key, "lid"))          s->lidAction = value;
    else if (!strcmp(key, "sleepminutes")) s->sleepMinutes = value;
    else if (!strcmp(key, "sleeppoweroff"))s->sleepPowerOff = value != 0;
    else if (!strcmp(key, "gain"))         s->gain = value;
    else if (!strcmp(key, "jack"))         s->jackGuard = value;
    else if (!strcmp(key, "shuffle"))      s->shuffle = value != 0;
    else if (!strcmp(key, "repeat"))       s->repeatMode = value;
    /* Two cheaper modes were inserted below Fast, so every mode's number
     * moved up by two. A file written before that says "resample" and
     * means the old numbering; one written since says "resample2". Read
     * either, shift the old one, and a config from an earlier build keeps
     * selecting the resampler it always did instead of silently becoming
     * two steps cheaper. */
    else if (!strcmp(key, "cardwrites"))   s->cardWrites = value != 0;
    else if (!strcmp(key, "topchosen"))    s->topChosen = value != 0;
    else if (!strcmp(key, "ratecut"))      s->rateCut = value;
    else if (!strcmp(key, "skipcrc"))      s->ignoreCrc = value != 0;
    else if (!strcmp(key, "scrollrate"))   s->scrollRate = value;
    else if (!strcmp(key, "artscale"))     s->artScale = value;
    else if (!strcmp(key, "artpace"))      s->artPace = value;
    else if (!strcmp(key, "noisefilter"))  s->noiseFilter = value;
    else if (!strcmp(key, "enhance"))      s->enhance = value;
    else if (!strcmp(key, "resumems"))     s->resumeMs = value;

    /* resample5. The key gets a new number whenever the default changes,
     * so a config written by an older build falls back to it rather than
     * carrying the old choice across and making the new default look
     * broken. It has moved three times now: to Lite when the setting went
     * from Setup to the Test page, to Raw once the rate-cut ladder made
     * the filter's cost the thing worth cutting first, and to five when a
     * cheaper mode was added below Raw.
     *
     * That last move is the one that had to happen whether the default
     * changed or not: every mode's number shifted up by one, so a file
     * saying "0" meant Raw when it was written and would mean None if it
     * were read today. */
    else if (!strcmp(key, "resample5"))    s->resampleMode = value;
    /* "top" is the merged setting. "topmode" and "meter" are what an
     * older build wrote - a mode and a style, separately - and are read
     * so that a card already in use keeps showing what it showed. They
     * are folded into the new value once both have been seen, in
     * MergeOldTop below, because either can come first in the file. */
    else if (!strcmp(key, "top"))          { s->topChoice = value; s_sawTop = true; }
    else if (!strcmp(key, "topmode"))      { s_oldMode = value;  s_sawOld = true; }
    else if (!strcmp(key, "meter"))        { s_oldMeter = value; s_sawOld = true; }
    else if (!strcmp(key, "theme"))        s->theme = value;
    else if (!strcmp(key, "accent"))       s->accent = value;
    else if (!strcmp(key, "font"))         s->fontStyle = value;
    else if (!strcmp(key, "listsize"))     s->listScale = value;
    else if (!strcmp(key, "tone"))         s->tonePreset = value;
}

/* Anything out of range is discarded rather than clamped-and-kept: a
 * corrupt file should fall back to a sane player, not a strange one. */
static void Sanitise(Settings *s)
{
    if (s->volume < 0 || s->volume > 127) s->volume = 96;
    if (s->lidAction != LID_KEEP_PLAYING && s->lidAction != LID_SLEEP)
        s->lidAction = LID_KEEP_PLAYING;
    if (s->sleepMinutes < PLAYER_SLEEP_MIN_MINUTES ||
        s->sleepMinutes > PLAYER_SLEEP_MAX_MINUTES)
        s->sleepMinutes = 30;
    if (s->gain < 0 || s->gain > GAIN_MAX_DB) s->gain = 0;
    /* A config written before "Require" was removed, or copied from a DSi
     * to a DS Lite, must not leave the guard set to something the build
     * no longer has or the console cannot honour. */
    if (s->jackGuard < 0 || s->jackGuard >= JACK_GUARD_MODES) s->jackGuard = 0;
    if (s->repeatMode < 0 || s->repeatMode >= REPEAT_MODES) s->repeatMode = REPEAT_OFF;

    /* Defaults chosen to be the cheapest sensible thing, so a card that
     * has never seen this player starts on the settings least likely to
     * stutter. */
    if (s->resampleMode < 0 || s->resampleMode >= RESAMPLE_MODES) s->resampleMode = RESAMPLE_RAW;
    /* A card written before a style was removed can name one that no
     * longer exists, so this clamps rather than trusting the file. */
    if (s->topChoice < 0 || s->topChoice >= TOP_CHOICES) s->topChoice = 0;
    if (s->theme < 0 || s->theme >= SKIN_THEMES)   s->theme = 0;
    /* -1 is a value, not an absence: it means "whichever accent the
     * theme came with". So the floor is -1 and not 0.
     *
     * Skin_SetAccent clamps this as well, and did so while nothing here
     * did - which is fine until something other than the skin reads the
     * number. Checked where the file is read, like everything else, so
     * the guarantee belongs to the loader rather than to one consumer
     * that happens to be careful. */
    if (s->accent < -1 || s->accent >= SKIN_ACCENTS) s->accent = -1;
    if (s->fontStyle < 0 || s->fontStyle >= GFX_FONT_STYLES) s->fontStyle = 0;
    if (s->listScale < 0 || s->listScale >= SKIN_LIST_SIZES) s->listScale = 0;
    /* Only the lower bound here: the preset table lives with the audio
     * and this file does not know how long it is. The player clamps it
     * against its own table when it applies it. */
    if (s->tonePreset < 0) s->tonePreset = 0;

    /* Both bounds, not just the lower one. These two index tables of
     * five and four entries respectively, and one of those tables holds
     * pointers that are dereferenced as strings - so a number past the
     * end is not a strange setting, it is a read from wherever the next
     * bytes of the binary happen to be. */
    if (s->rateCut    < 0 || s->rateCut    >= RATE_CUT_STOPS)    s->rateCut    = 0;
    if (s->scrollRate < 0 || s->scrollRate >= SCROLL_RATE_STOPS) s->scrollRate = 1;
    if (s->artScale   < 0 || s->artScale > 3) s->artScale = 2;
    if (s->artPace    < 0 || s->artPace  > 2) s->artPace  = 2;
    /* Clamped against the decoder's own count, so a file naming a level
     * this build does not have falls back to off rather than indexing
     * past the table. */
    if (s->noiseFilter < 0 || s->noiseFilter >= MP3_SHAPE_STOPS)
        s->noiseFilter = 0;
    if (s->enhance     < 0 || s->enhance     >= MP3_SHAPE_STOPS)
        s->enhance = 0;

    /* A negative position is a corrupt file, not a seek target. Dropped
     * to the start of the track rather than dropping the track: the
     * path is the part worth keeping, and nobody minds beginning an
     * album again from its first second. */
    if (s->resumeMs < 0) s->resumeMs = 0;
}

void Settings_Load(Settings *out)
{
    out->volume         = 96;
    out->lidAction      = LID_KEEP_PLAYING;
    out->sleepMinutes   = 30;
    out->sleepPowerOff  = false;
    /* Two decibels, not four.
     *
     * The boost exists to give back the headroom the resampler keeps -
     * that headroom is 2.16dB, and the gain ladder steps in twos, so two
     * is what repays it. Four spends it and then overspends: measured on
     * a real track, +2dB peaks at 0.934 of full scale and clips nothing,
     * while +4dB peaks at 1.167 and hard-clipped about one sample in a
     * thousand, continuously, on every loud passage. That is not heard
     * as loudness, it is heard as grit.
     *
     * Four is the default now, chosen deliberately and on the strength
     * of that last paragraph: the mixer rounds peaks off with a
     * quadratic knee that meets full scale with zero slope, so the
     * sample that used to be sheared into grit is now bent instead. The
     * loudness is worth more than the 2.16dB of headroom on a console
     * played through earbuds on a bus.
     *
     * What it costs is honest and worth writing down: +4dB peaks at
     * 1.167 of full scale on real material, so the knee is working on
     * roughly one sample in a thousand through loud passages rather than
     * never. That is a rounding, not a clip - but it is not nothing, and
     * anyone chasing absolute transparency should put this back to 2. */
    out->gain           = 4;
    /* Pause when the headphones come out, rather than carry on into the
     * speaker. On a DS or DS Lite the socket cannot be read at all, and
     * there the guard simply never fires - Jack_Available gates it - so
     * this costs those consoles nothing. */
    out->jackGuard      = JACK_GUARD_PAUSE;
    out->shuffle        = false;
    out->repeatMode     = REPEAT_OFF;
    /* Raw: two-point linear interpolation and no filter at all.
     *
     * Measured, the four-tap filter that used to be the default is
     * already down 2dB at 8kHz and 6dB at 14kHz - so it was spending real
     * time to produce a response that is not obviously better than
     * linear, on a processor that has none to spare at 320 kbps. Anyone
     * who wants the long filter can have it; it is a preference now
     * rather than a default. */
    /* Fast: the shortest filter that is a filter.
     *
     * Raw was the default on the argument that its two-point
     * interpolation measures about as well as a four-tap filter, which
     * is true and is also the whole of the case for it. Fast costs a few
     * percent of one core and stops the imaging that Raw leaves when a
     * stream has to be resampled upward, which is audible on 22kHz
     * material as a whistle over the top of it. */
    out->resampleMode   = RESAMPLE_FAST;
    /* Edge: the columns as outlines rather than solids.
     *
     * Set in two places and it has to be, which is worth knowing before
     * changing it again. This is what a card with no config gets; but
     * ApplyTheme hands the top screen over to whatever the theme
     * prefers until the listener picks one themselves, so the Blue
     * theme's own entry in kTraits (skin.c) has to agree or this value
     * is overwritten before the first frame. */
    out->topChoice      = VIS_EDGE;
    out->topChosen      = false;  /* until somebody picks one */
    out->theme          = 0;
    out->accent         = -1;   /* the theme's own, until chosen */
    out->fontStyle      = 0;
    out->listScale      = 0;
    out->tonePreset     = 0;      /* Flat: the stage is not run at all */
    out->cardWrites     = true;   /* writing is the normal state */

    /* The diagnostics page's defaults, which are also what its reset
     * button puts back. Every one of them is the setting that measured
     * best on hardware rather than the one that is easiest to explain. */
    out->rateCut        = 0;      /* full rate */
    out->ignoreCrc      = true;   /* nearly free, and nothing acts on a fail */
    out->scrollRate     = 1;      /* Quick */
    out->artScale       = 2;      /* Half */
    out->artPace        = 2;      /* Eager */
    out->noiseFilter    = 0;      /* off */
    out->enhance        = 0;      /* off */

    /* Nothing to carry over until a session records something. */
    out->resumePath[0]  = '\0';
    out->resumeMs       = 0;

    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;

    /* One value in this file is text - the resume path - and the rest
     * are a short key and a few digits. The buffer is sized from the
     * field itself rather than from a number that looks big enough:
     * when this was sixty-four bytes against a path field of 640, the
     * front of a long path was stored and the tail arrived as a line of
     * its own, so a key called "sic/album/track.mp3" was parsed and the
     * resume silently pointed at nothing.
     *
     * Static rather than automatic. The ARM9's main stack lives in DTCM
     * and is under 16KB for the whole program; 672 bytes of it for a
     * function that runs at startup is not a trade worth making, and
     * the file browser beside it is static for the same reason. */
    static char line[sizeof(out->resumePath) + 32];
    while (fgets(line, sizeof(line), f)) {
        /* A line too long for even that is a corrupt or foreign file.
         * The rest of it is thrown away rather than read as the next
         * key=value, which is the half of the old fault that turned one
         * bad line into two. */
        size_t len = strlen(line);
        if (len && line[len - 1] != '\n') {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') { }
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        /* The one value that is text rather than a number, so it is
         * taken here instead of going through ApplyLine. Its newline is
         * trimmed, and a carriage return with it: the file is meant to
         * be editable by hand, and a copy that has been through a
         * desktop text editor arrives with CRLF endings. A path with a
         * '\r' welded to the end of it opens nothing. */
        if (!strcmp(line, "resumepath")) {
            char *v = eq + 1;
            size_t n = strlen(v);
            while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r'))
                v[--n] = '\0';
            snprintf(out->resumePath, sizeof(out->resumePath), "%s", v);
            continue;
        }

        int value = 0;
        if (sscanf(eq + 1, "%d", &value) == 1)
            ApplyLine(out, line, value);
    }
    fclose(f);

    /* Latched from the file, so a card left under test stays untouched
     * across a reboot rather than quietly allowing writes again. */
    s_blockWrites = !out->cardWrites;

    /* An older config, with a mode and a style instead of one choice.
     *
     * Only when the new key is absent: a file written by this build has
     * both, because the old keys are still emitted for a build the user
     * might go back to, and the new one is the authority.
     *
     * Mode 0 was "meter", and the meter's number is now the choice
     * itself, so it carries straight across. Mode 1 was "card", of which
     * there was one - the first. Mode 2 was off, which no longer exists:
     * a top screen that draws nothing is a top screen showing the last
     * thing it drew, which is not a setting anybody chose on purpose. It
     * lands on the first card. */
    if (!s_sawTop && s_sawOld) {
        if (s_oldMeter < 0 || s_oldMeter >= VIS_STYLES) s_oldMeter = 0;
        out->topChoice = (s_oldMode >= 1) ? TOP_CARD(0) : s_oldMeter;
    }
    s_sawTop = s_sawOld = false;

    Sanitise(out);
    s_dirty = false;
}

static void Write(const Settings *s)
{
    /* The one gate every card write in this program passes through. */
    if (s_blockWrites) { s_dirty = false; return; }

    /* Made on demand. The settings file is no longer at the root, so
     * the first write on a fresh card has a folder to create first -
     * and mkdir failing because it is already there is the normal case,
     * not an error. */
    mkdir(DSXP_DIR, 0777);

    /* Staged like every other write this program makes: the temporary
     * takes the truncation, and the real file is only replaced once the
     * new contents are on the card. The comment below is about how often
     * to retry; this is about what a failed attempt costs, which used to
     * be the whole file. */
    DsxpWrite w;
    if (!Dsxp_BeginWrite(&w, SETTINGS_PATH)) {
        /* Not straight back round.
         *
         * The settle timer has already expired by the time this runs, so
         * leaving the dirty flag set and returning meant Settings_Tick
         * came back on the very next frame - and the one after that,
         * sixty times a second, for as long as the card refused. Every
         * attempt is an fopen for writing, which truncates. A card that
         * was merely busy got hammered; a card that was in trouble got
         * hammered while it was in trouble, on the main thread, with
         * every attempt blocking on card I/O.
         *
         * The write is still retried - a setting should survive a card
         * that was briefly unavailable - but a settle period apart like
         * any other, and abandoned after a few tries. */
        s_dirtyAt = tickGetCount();
        if (++s_writeFails >= SETTINGS_MAX_FAILS) {
            s_writeFails = 0;
            s_dirty = false;
        }
        return;
    }

    fprintf(w.f,
            "# DSXP settings - safe to edit or delete\n"
            "volume=%d\n"
            "lid=%d\n"
            "sleepminutes=%d\n"
            "sleeppoweroff=%d\n"
            "gain=%d\n"
            "cardwrites=%d\n"
            "resample5=%d\n"
            "top=%d\n"
            /* Still written, and never read back when "top" is present:
             * a user who puts an older build on the card should find the
             * top screen where they left it rather than reset. */
            "topmode=%d\n"
            "meter=%d\n"
            "theme=%d\n"
            "accent=%d\n"
            "font=%d\n"
            "jack=%d\n"
            "shuffle=%d\n"
            "repeat=%d\n"

            "listsize=%d\n"
            "tone=%d\n"
            "topchosen=%d\n"
            "ratecut=%d\n"
            "skipcrc=%d\n"
            "scrollrate=%d\n"
            "artscale=%d\n"
            "artpace=%d\n"
            "noisefilter=%d\n"
            "enhance=%d\n"
            "resumems=%d\n",

            s->volume, s->lidAction,
            s->sleepMinutes, s->sleepPowerOff ? 1 : 0, s->gain,
            s->cardWrites ? 1 : 0,
            s->resampleMode, s->topChoice,
            TOP_IS_METER(s->topChoice) ? 0 : 1,
            TOP_IS_METER(s->topChoice) ? s->topChoice : 0,
            s->theme, s->accent, s->fontStyle, s->jackGuard,
            s->shuffle ? 1 : 0, s->repeatMode,
            s->listScale, s->tonePreset,
            s->topChosen ? 1 : 0,
            s->rateCut, s->ignoreCrc ? 1 : 0,
            s->scrollRate, s->artScale, s->artPace,
            s->noiseFilter, s->enhance, s->resumeMs);

    /* Last, and only when there is one.
     *
     * Last because it is the only line whose length is not bounded by a
     * handful of digits, so a card that runs out of room part way
     * through truncates this and nothing else - and a missing resume
     * path costs the listener their place, where a missing volume or
     * theme would change the player under them. Omitted entirely when
     * empty, so the key being absent and the key being blank are not
     * two spellings of the same thing. */
    if (s->resumePath[0])
        fprintf(w.f, "resumepath=%s\n", s->resumePath);

    /* Commit is where the write actually reaches the card - it flushes,
     * syncs and only then moves the temporary over the real file - so
     * its result is the one that says whether this worked. Ignoring it
     * meant a failed flush was recorded as a successful save. */
    /* One test of the stream, not two.
     *
     * This used to abort on the first ferror and then ask ferror again
     * on the next line - by which point Dsxp_AbortWrite had closed the
     * handle and set it to NULL, and newlib's ferror is a macro that
     * dereferences its argument rather than a function that checks it.
     * Reading a stream that has been closed is undefined whatever the
     * platform does with it, and the path it happens on is the one
     * where the card is already failing. The short-circuit here means
     * a stream error skips the commit, and the abort happens once,
     * after both questions have been asked. */
    if (ferror(w.f) || !Dsxp_CommitWrite(&w, SETTINGS_PATH)) {
        Dsxp_AbortWrite(&w);
        s_dirtyAt = tickGetCount();
        if (++s_writeFails >= SETTINGS_MAX_FAILS) {
            s_writeFails = 0;
            s_dirty = false;
        }
        return;
    }

    s_writeFails = 0;
    s_dirty = false;
}

void Settings_Tick(const Settings *s)
{
    if (!s_dirty) return;
    if (tickGetCount() - s_dirtyAt < SETTINGS_SETTLE_TICKS) return;

    Write(s);
}

void Settings_SetCardWrites(Settings *s, bool allow)
{
    if (allow) {
        /* Clear the block first, or the write recording it is refused. */
        s_blockWrites = false;
        s->cardWrites = true;
        Settings_MarkDirty();
        return;
    }

    /* One last write to record the choice, then the gate closes. Written
     * outright rather than marked dirty: a deferred write would be the
     * first thing the new block refuses. */
    s->cardWrites = false;
    Write(s);
    s_blockWrites = true;
}

void Settings_Flush(const Settings *s)
{
    if (s_dirty) Write(s);
}
