/* Round-trips settings.c through a real file: defaults, save, reload,
 * and rejection of a corrupt file. Persistence that silently loses your
 * volume is worse than none, so it is worth a test. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

/* This test drives the debounce, so it needs a clock it can advance -
 * hence its own rather than the frozen one in the stub header. */
#define DSXP_HOST_OWN_TICKS
#define TICK_FREQ 523656u
static uint64_t g_ticks = 0;
static uint64_t tickGetCount(void) { return g_ticks; }
#include "settings.c"

static int g_fail = 0;
static void Check(const char *what, bool ok, const char *detail)
{
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

int main(void)
{
    printf("settings persistence\n");
    remove(SETTINGS_PATH);

    Settings a;
    Settings_Load(&a);
    char buf[128];
    snprintf(buf, sizeof(buf), "volume %d, lid %d", a.volume, a.lidAction);
    Check("defaults when no file exists",
          a.volume == 96 &&
          a.lidAction == LID_KEEP_PLAYING, buf);

    /* What a card with no config gets, which is what every new user
     * gets. Pinned because these were chosen rather than inherited, and
     * because one of them is set in two places: the Blue theme hands
     * its own top screen over at startup, so skin.c's kTraits has to
     * agree with topChoice below or the value here never survives to
     * the first frame. */
    snprintf(buf, sizeof(buf), "theme %d, meter %d, gain %d, jack %d, lid %d",
             a.theme, a.topChoice, a.gain, a.jackGuard, a.lidAction);
    Check("a fresh card starts on the chosen defaults",
          a.theme == 0 &&                      /* Blue */
          a.topChoice == VIS_EDGE &&
          a.gain == 4 &&
          a.jackGuard == JACK_GUARD_PAUSE &&
          a.lidAction == LID_KEEP_PLAYING, buf);

    /* Change everything and make sure it survives a round trip. */
    a.volume = 42; a.lidAction = LID_SLEEP;
    a.sleepMinutes = 77; a.sleepPowerOff = true;

    /* The look and playback settings too. These were added one at a time
     * and none of them was ever checked here, so a mistyped key or a
     * missing argument would have lost your theme on the next boot and
     * nothing would have said so. */
    a.gain = 7;
    a.resampleMode = 2; a.topChoice = TOP_CARD(1);
    a.theme = 4; a.fontStyle = 9; a.listScale = 2;
    a.jackGuard = 1;
    a.accent = 11; a.tonePreset = 3; a.topChosen = true;
    a.shuffle = true; a.repeatMode = 2;

    /* The diagnostics page's settings, which used to be plain statics in
     * main.c and so went back to their defaults on every run. */
    a.rateCut = 3; a.ignoreCrc = false; a.scrollRate = 2;
    a.artScale = 1; a.artPace = 0;

    Settings_MarkDirty();
    Settings_Flush(&a);

    Settings b;
    Settings_Load(&b);

    /* Every field, by name, rather than a handful chosen by whoever last
     * touched this: a setting that is saved and not reloaded looks
     * exactly like one that was never saved, and the only way to tell is
     * to write one and read it back. */
    {
        const struct { const char *name; int wrote, read; } kF[] = {
            { "volume",     a.volume,       b.volume },
            { "gain",       a.gain,         b.gain },
            { "lid",        a.lidAction,    b.lidAction },
            { "jack",       a.jackGuard,    b.jackGuard },
            { "resample",   a.resampleMode, b.resampleMode },
            { "top screen", a.topChoice,    b.topChoice },
            { "top chosen", a.topChosen,    b.topChosen },
            { "theme",      a.theme,        b.theme },
            { "accent",     a.accent,       b.accent },
            { "font",       a.fontStyle,    b.fontStyle },
            { "list size",  a.listScale,    b.listScale },
            { "tone",       a.tonePreset,   b.tonePreset },
            { "rate cut",   a.rateCut,      b.rateCut },
            { "skip crc",   a.ignoreCrc,    b.ignoreCrc },
            { "scroll",     a.scrollRate,   b.scrollRate },
            { "art scale",  a.artScale,     b.artScale },
            { "art pace",   a.artPace,      b.artPace },
            { "shuffle",    a.shuffle,      b.shuffle },
            { "repeat",     a.repeatMode,   b.repeatMode },
            { "sleep",      a.sleepMinutes, b.sleepMinutes },
        };
        const int n = (int)(sizeof kF / sizeof kF[0]);
        int lost = 0;
        char which[64] = "";
        for (int i = 0; i < n; i++)
            if (kF[i].wrote != kF[i].read) {
                lost++;
                if (!which[0])
                    snprintf(which, sizeof(which), "%s: wrote %d read %d",
                             kF[i].name, kF[i].wrote, kF[i].read);
            }
        snprintf(buf, sizeof(buf), "%d of %d%s%s", n - lost, n,
                 which[0] ? " - " : "", which);
        Check("every setting survives a save and a reload", lost == 0, buf);
    }

    /* Card writes on its own, and put back afterwards.
     *
     * Not in the table above: turning it off closes the gate on every
     * write that follows, so a round trip that left it off would take
     * the rest of this file's checks with it - which is exactly what it
     * did when it was in there. */
    {
        Settings c;
        Settings_SetCardWrites(&a, false);
        Settings_Flush(&a);
        Settings_Load(&c);
        Check("card writes survives being turned off", !c.cardWrites,
              c.cardWrites ? "read back as on" : "read back as off");
        Settings_SetCardWrites(&a, true);
        Settings_Flush(&a);
    }

    /* The one value that is not a number, and so is not in the table
     * above: where playback had got to.
     *
     * This is the field that broke last time it existed. The reader's
     * line buffer was 64 bytes against a path field of 640, so a real
     * path was stored as its first 52 characters and the tail arrived
     * as a line of its own - which parsed as a key nobody had heard of
     * and was dropped. The resume then pointed at a file that did not
     * exist, silently, and only for people whose music was more than
     * two folders deep. Every short path round-tripped perfectly. */
    {
        /* As long as the field can hold, because the length is the
         * whole point. Built rather than typed so it cannot drift away
         * from the size of the thing it is testing. */
        char longPath[sizeof(a.resumePath)];
        size_t n = 0;
        n += (size_t)snprintf(longPath + n, sizeof(longPath) - n, "/Music");
        while (n < sizeof(longPath) - 20)
            n += (size_t)snprintf(longPath + n, sizeof(longPath) - n, "/folder");
        snprintf(longPath + n, sizeof(longPath) - n, "/track.mp3");

        Settings r;
        a.volume = 42;
        snprintf(a.resumePath, sizeof(a.resumePath), "%s", longPath);
        a.resumeMs = 1234567;
        Settings_MarkDirty();
        Settings_Flush(&a);
        Settings_Load(&r);

        snprintf(buf, sizeof(buf), "%d characters in, %d out",
                 (int)strlen(longPath), (int)strlen(r.resumePath));
        Check("a full-length resume path survives a round trip",
              !strcmp(r.resumePath, longPath), buf);

        snprintf(buf, sizeof(buf), "wrote %d, read %d", a.resumeMs, r.resumeMs);
        Check("the resume position survives with it",
              r.resumeMs == a.resumeMs, buf);

        /* The other half of the old fault: a line too long for the
         * buffer must be finished and thrown away, not spill into the
         * next key. Written by hand with a value after the path,
         * because the writer puts the path last and so could never
         * produce this ordering by itself. */
        FILE *h = fopen(SETTINGS_PATH, "w");
        fprintf(h, "resumepath=%s\nvolume=42\nresumems=99\n", longPath);
        fclose(h);

        Settings q;
        Settings_Load(&q);
        snprintf(buf, sizeof(buf), "volume %d, ms %d, path %d chars",
                 q.volume, q.resumeMs, (int)strlen(q.resumePath));
        Check("a long path does not swallow the keys after it",
              q.volume == 42 && q.resumeMs == 99 &&
              !strcmp(q.resumePath, longPath), buf);

        /* Nothing to resume must leave the key out altogether, so that
         * absent and blank are not two spellings of the same state. */
        a.resumePath[0] = '\0';
        a.resumeMs = 0;
        Settings_MarkDirty();
        Settings_Flush(&a);

        char whole[2048] = "";
        h = fopen(SETTINGS_PATH, "r");
        fread(whole, 1, sizeof(whole) - 1, h);
        fclose(h);
        Check("an empty position writes no resumepath line",
              strstr(whole, "resumepath=") == NULL,
              strstr(whole, "resumepath=") ? "the key is there" : "absent");

        /* A corrupt file must not hand the decoder a negative seek. */
        h = fopen(SETTINGS_PATH, "w");
        fputs("resumems=-5000\nresumepath=/a/b.mp3\n", h);
        fclose(h);
        Settings_Load(&q);
        snprintf(buf, sizeof(buf), "read back as %d", q.resumeMs);
        Check("a negative position falls back to the start", q.resumeMs == 0, buf);
    }


    /* A card that refuses the write must not be asked again on the next
     * frame.
     *
     * The settle timer has already expired by the time the write is
     * attempted, so a failure that left the dirty flag set brought
     * Settings_Tick straight back round - sixty times a second, each
     * time an fopen for writing, which truncates. Pointing SETTINGS_PATH
     * at a directory is the simplest way to make fopen fail reliably.
     */
    {
        Settings c = a;
        remove(SETTINGS_PATH);

        /* A directory where the file should be: fopen("w") cannot
         * succeed, and nothing else about the run is disturbed. */
        char blocker[512];
        snprintf(blocker, sizeof(blocker), "%s", SETTINGS_PATH);
        mkdir(blocker, 0777);

        Settings_MarkDirty();
        g_ticks += TICK_FREQ * 4;          /* past the settle time */

        /* Each refused attempt pushes the settle timer forward, so the
         * timer is the attempt counter - the test's clock does not move
         * on its own, so watching it here would have counted nothing and
         * passed whatever the code did. */
        /* Twenty seconds of frames: long enough for every retry a
         * settle period apart, and for it to give up. */
        const int frames = 20 * 60;
        int attempts = 0;
        for (int i = 0; i < frames; i++) {
            uint64_t was = s_dirtyAt;
            Settings_Tick(&c);
            if (s_dirtyAt != was) attempts++;
            g_ticks += TICK_FREQ / 60;     /* one frame */
        }

        snprintf(buf, sizeof(buf), "%d attempts over %d frames, dirty %s",
                 attempts, frames, s_dirty ? "still set" : "cleared");

        /* It must try - a setting should survive a card that was briefly
         * busy - and it must stop. Retrying on every frame, which is what
         * this replaced, would be twelve hundred. */
        Check("a refused write retries a few times, not every frame",
              attempts == SETTINGS_MAX_FAILS && !s_dirty, buf);

        rmdir(blocker);
    }

    /* Debounce: dirty but not settled yet must not write. */
    remove(SETTINGS_PATH);
    Settings_MarkDirty();
    g_ticks += TICK_FREQ;            /* 1s - under the 3s settle time */
    Settings_Tick(&a);
    Check("a just-changed value is not written yet", fopen(SETTINGS_PATH, "r") == NULL, NULL);

    g_ticks += TICK_FREQ * 5;
    Settings_Tick(&a);
    Check("it is written once it settles", fopen(SETTINGS_PATH, "r") != NULL, NULL);

    /* Every field in range after a file full of nonsense.
     *
     * One field at a time is how this was checked before, and it is why
     * two of them were wrong for a long time. rateCut and scrollRate
     * were bounded below and not above, so a config saying 99 indexed a
     * five-entry and a four-entry table with 99 - and one of those
     * tables holds const char * that go straight to snprintf. The file
     * says "safe to edit or delete" in its own first line.
     *
     * So: every value the loader is responsible for, both directions,
     * checked against the range its consumer actually indexes. A field
     * added later without a clamp shows up here rather than on a
     * flashcart. */
    {
        static const char *const kKeys[] = {
            "volume", "gain", "lid", "sleepminutes", "jack", "repeat",
            "resample5", "top", "theme", "accent", "font", "listsize",
            "tone", "ratecut", "scrollrate", "artscale", "artpace",
            "noisefilter", "enhance", "resumems",
        };
        const long kWild[] = { 99999L, -99999L, 2147483647L };

        for (unsigned pass = 0; pass < sizeof kWild / sizeof kWild[0]; pass++) {
            FILE *g = fopen(SETTINGS_PATH, "w");
            for (unsigned k = 0; k < sizeof kKeys / sizeof kKeys[0]; k++)
                fprintf(g, "%s=%ld\n", kKeys[k], kWild[pass]);
            fclose(g);

            Settings w;
            Settings_Load(&w);

            const struct { const char *name; bool ok; int got; } kR[] = {
                { "volume",      w.volume >= 0 && w.volume <= 127,        w.volume },
                { "gain",        w.gain >= 0 && w.gain <= GAIN_MAX_DB,    w.gain },
                { "lid",         w.lidAction == LID_KEEP_PLAYING ||
                                 w.lidAction == LID_SLEEP,                w.lidAction },
                { "sleep mins",  w.sleepMinutes >= PLAYER_SLEEP_MIN_MINUTES &&
                                 w.sleepMinutes <= PLAYER_SLEEP_MAX_MINUTES,
                                                                          w.sleepMinutes },
                { "jack",        w.jackGuard >= 0 &&
                                 w.jackGuard < JACK_GUARD_MODES,          w.jackGuard },
                { "repeat",      w.repeatMode >= 0 &&
                                 w.repeatMode < REPEAT_MODES,             w.repeatMode },
                { "resample",    w.resampleMode >= 0 &&
                                 w.resampleMode < RESAMPLE_MODES,         w.resampleMode },
                { "top screen",  w.topChoice >= 0 &&
                                 w.topChoice < TOP_CHOICES,               w.topChoice },
                { "theme",       w.theme >= 0 && w.theme < SKIN_THEMES,   w.theme },
                { "accent",      w.accent >= -1 &&
                                 w.accent < SKIN_ACCENTS,                 w.accent },
                { "font",        w.fontStyle >= 0 &&
                                 w.fontStyle < GFX_FONT_STYLES,           w.fontStyle },
                { "list size",   w.listScale >= 0 &&
                                 w.listScale < SKIN_LIST_SIZES,           w.listScale },
                { "rate cut",    w.rateCut >= 0 &&
                                 w.rateCut < RATE_CUT_STOPS,              w.rateCut },
                { "scroll",      w.scrollRate >= 0 &&
                                 w.scrollRate < SCROLL_RATE_STOPS,        w.scrollRate },
                { "art scale",   w.artScale >= 0 && w.artScale <= 3,      w.artScale },
                { "art pace",    w.artPace >= 0 && w.artPace <= 2,        w.artPace },
                { "noise",       w.noiseFilter >= 0 &&
                                 w.noiseFilter < MP3_SHAPE_STOPS,         w.noiseFilter },
                { "enhance",     w.enhance >= 0 &&
                                 w.enhance < MP3_SHAPE_STOPS,             w.enhance },
                { "resume ms",   w.resumeMs >= 0,                         w.resumeMs },
                /* The one the loader cannot finish on its own: the
                 * preset table lives with the audio and settings.c does
                 * not know how long it is, so main.c clamps it against
                 * TONE_PRESETS before applying it. Only the lower bound
                 * belongs to this file. */
                { "tone (lower bound only)", w.tonePreset >= 0,           w.tonePreset },
            };

            int bad = 0;
            char which[72] = "";
            for (unsigned i = 0; i < sizeof kR / sizeof kR[0]; i++)
                if (!kR[i].ok) {
                    bad++;
                    if (!which[0])
                        snprintf(which, sizeof(which), " - %s = %d",
                                 kR[i].name, kR[i].got);
                }

            snprintf(buf, sizeof(buf), "every key = %ld, %d out of range%s",
                     kWild[pass], bad, which);
            Check("a file full of nonsense leaves every field in range",
                  bad == 0, buf);
        }
    }

    /* Garbage must not produce a strange player. */
    FILE *f = fopen(SETTINGS_PATH, "w");
    fputs("volume=9999\nquality=42\nsleepminutes=0\nnonsense\n", f);
    fclose(f);

    Settings c;
    Settings_Load(&c);
    snprintf(buf, sizeof(buf), "volume %d, mins %d",
             c.volume, c.sleepMinutes);
    Check("out-of-range values fall back to defaults",
          c.volume == 96 &&
          c.sleepMinutes == 30, buf);


printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail != 0;
}
