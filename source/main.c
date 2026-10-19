/* DSXP - an MP3 player for the Nintendo DS.
 *
 * Top screen: music-reactive visualizer.
 * Bottom screen: file browser and transport.
 *
 * MP3 decoding, the UI and the stream callback all live here, on the
 * ARM9. The ARM7 is ours as well: arm7/ replaces calico's prebuilt core
 * so that the DSi's headphone-socket pin can be read, and maxmod's half
 * of it does the actual mixing. See README.md and the Makefile. */

#include <nds.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, for matching playlist names */

#include "audio.h"
#include "decoder_mp3.h"
#include "jpeg.h"
#include "filebrowser.h"
#include "gfx.h"
#include "keyboard.h"
#include "player.h"
#include "favourites.h"
#include "settings.h"
#include "jack.h"
#include "speaker_pxi.h"   /* SPK_BUS_* for the register watch */
#include "skin.h"
#include "visualizer.h"

/* Holding L/R seeks in discrete hops rather than continuously: at 60
 * frames a second, seeking on every held frame would rip through a
 * three minute song in two seconds and make the control useless. */
/* Ten seconds a step, not three.
 *
 * Three meant crossing a five-minute track took a hundred presses, and
 * held down it still only moved twenty seconds a second. Ten covers a
 * long track in a reasonable sweep and is still fine enough to land on a
 * particular part of a song. */
#define SEEK_STEP_MS      10000
#define SEEK_REPEAT_FRAMES 9

/* The bottom screen is composed here, in main RAM, and then pushed to
 * VRAM in one shot during vblank (see PresentSubScreen).
 *
 * Drawing straight into VRAM is what was causing the visible blinking.
 * The skin is built up in layers - background gradient first, then
 * panels, then rows and text on top - so a repaint spends most of its
 * duration in a half-finished state where the background has been laid
 * down but the content has not. On a single-buffered screen the display
 * controller happily scans out whatever is there at that instant, so
 * every repaint flashed its own intermediate state. That is a different
 * problem from tearing, and it cannot be fixed by repainting less often
 * - only by never letting the display see a partial frame.
 *
 * 96KB of main RAM buys exactly that: the display only ever sees a
 * finished image. The DMA that delivers it moves 256x192x2 bytes in well
 * under the ~4.5ms vblank window, so the copy itself is never visible
 * either. */
/* 32-byte aligned so a partial DC_FlushRange of whole rows lands on cache
 * line boundaries. */
static uint16_t s_subBackbuf[GFX_W * GFX_H] __attribute__((aligned(32)));
static uint16_t *s_subVram = NULL;

/* Diagnostics switches. Not saved to the card on purpose - they exist to
 * take one cost out of the frame while a problem is being cornered, and
 * a machine that came back from a reboot with the visualizer silently
 * off would be a worse bug than the one being chased. */
/* 0 = bars off, 1 = bars at half rate, 2 = bars every frame.
 *
 * Half rate is the default. The bars' drawing is genuinely cheap - about
 * 1200 pixels a frame even when every bar is swinging its full height -
 * but at 320 kbps the machine is close enough to its limit that the few
 * percent it does cost is what tips the decoder into a catch-up burst,
 * and a catch-up burst is a stretch where nothing gets drawn at all.
 * Updating them thirty times a second looks the same and halves both the
 * analysis and the repaint. */
static int s_visMode = 1;

/* What the top screen shows. The card is the cheap option: it has no
 * per-frame animation, so on the fifty-nine frames a second where the
 * clock has not changed it costs nothing at all. */
/* What the top screen shows, as one number - see visualizer.h.
 *
 * It was two settings for a while: a mode, and separately which meter.
 * They were split because a single row of segments could not hold seven
 * names across 232 pixels without them overlapping. The answer to that
 * was never two settings, it was not putting them in a row of segments -
 * they live on a page of their own now, drawn rather than named, and one
 * number is enough again. */
static int s_topChoice = 0;
/* Set once the listener picks a top screen for themselves. Until then a
 * theme is free to bring its own along. */
static bool s_topChosen = false;      /* Bars */

static void InvalidateTopScreen(void);

/* The Faves tab: every saved playlist, and Liked among them.
 *
 * Favourites are an M3U in the playlist folder, so the tab has nothing
 * of its own to draw - but it is a list of lists rather than a place on
 * the card, so the folder is opened locked: no "..", no subfolders, no
 * loose audio. Without that the tab was the file manager wearing a
 * different name, and one tap on ".." left the user somewhere the tab
 * had not offered to take them.
 *
 * If the folder is not there yet - nothing liked, nothing saved - the
 * browser stays where it was rather than showing an empty page. */
/* Where the Files tab was standing when Faves took the browser.
 *
 * There is one browser, so opening Faves points it at the playlist
 * folder and locks it - and going back to Files left it there, locked,
 * showing playlists and refusing to climb out. The file manager appeared
 * to have lost the card. */
static char s_filesPath[FB_PATH_LEN] = "/";

static void OpenFavourites(PlayerState *player, FileBrowser *fb)
{
    /* Only a real folder is worth remembering. A browser that failed to
     * open has no cwd at all, and storing that sends Files to nowhere
     * the next time it is asked for - and keeps sending it there, since
     * nothing later overwrites it with anything better. */
    if (!fb->locked && fb->cwd[0])
        snprintf(s_filesPath, sizeof(s_filesPath), "%s", fb->cwd);

    if (FileBrowser_InitLocked(fb, PLAYLIST_DIR)) {
        player->view = VIEW_FAVES;
        Skin_InvalidateBottom();
    }
}

/* And back, to the folder Files was in rather than to the root. */
static void OpenFiles(PlayerState *player, FileBrowser *fb)
{
    /* Locked, or holding nothing at all.
     *
     * The second case is what a failed open leaves behind, and it used
     * to fall straight through this: not locked, so not re-opened, so
     * Files showed an empty folder that refreshing could not fix -
     * because there was no folder, only an empty cwd. */
    if ((fb->locked || fb->cwd[0] == '\0') &&
        !FileBrowser_Init(fb, s_filesPath))
        FileBrowser_Init(fb, "/");

    player->view = VIEW_FILES;
    Skin_InvalidateBottom();
}

/* The one place the top screen changes.
 *
 * A meter's choice number is its style number, so choosing a meter also
 * tells the visualizer which one - two calls that had to be made
 * together, and now cannot be made apart. */
/* Which of the cards shows the embedded picture. */
#define TOP_CARD_COVER TOP_CARD(1)

/* Whether the cover is fetched at all. On, because the card that shows
 * one is no use without it; here so the cost of it can be taken out of
 * the picture in one press when a track starts badly. */
static bool s_coverFetch = true;

static void SetTopChoice(int choice)
{
    if (choice < 0 || choice >= TOP_CHOICES) return;
    s_topChoice = choice;
    if (TOP_IS_METER(choice)) Visualizer_SetStyle(choice);

    /* Only the cover card looks at the picture, and reading one costs a
     * pause at the start of every track - on the UI thread, holding the
     * decoder lock. Nothing else on the top screen has any use for it,
     * so nothing else pays for it.
     *
     * Turning it on mid-track fetches the picture for what is already
     * playing. Without that, choosing the card would show the stand-in
     * until the next track started, which reads as the card not working
     * rather than as it not having looked yet. */
    const bool want = (choice == TOP_CARD_COVER) && s_coverFetch;
    Mp3Decoder_SetWantArt(want);
    if (want) Mp3Decoder_LoadArtNow();

    /* And the meter is the only thing that reads the decoded samples, so
     * anything else up there means the decoder can stop keeping them. */
    Mp3Decoder_SetWantSamples(TOP_IS_METER(choice));

    InvalidateTopScreen();
}
static bool s_diagOverlay = true;
static int  s_testFocus = 0;

/* The d-pad cursor on the colour picker. Seeded from the current accent
 * when the page opens, so the cursor starts on the colour in use rather
 * than in a corner. */
static int  s_colorFocus = 0;
static int  s_themeFocus = 0;
static int  s_themeEntry = 0;
static int  s_colorEntry = 0;
static int  s_fontFocus = 0;
static int  s_fontEntry = 0;

/* The cursor on the top-screen picker.
 *
 * The shape of that page lives in skin.c and the d-pad asks it for each
 * step - it is two grids of different widths now, and a column count
 * kept here as well would be a second opinion about a layout that only
 * one of the two can see. */
static int  s_meterFocus = 0;

/* What the meter was when the page opened, so B can put it back. The
 * cursor applies each style as it lands on it - a meter is judged by
 * watching it move, and the top screen is showing the real one behind
 * the page - so leaving without choosing has to undo that. */
static int  s_meterEntry = 0;

/* And whether anything had been chosen on the way in.
 *
 * Moving the cursor on that page sets s_topChosen, which is right - the
 * choice applies live, so moving is choosing. But B puts the choice and
 * the cursor back and used to leave the flag set, so backing out of a
 * page you had only looked at latched "the user picked this" forever.
 * From then on a theme no longer brought its own top screen, and
 * nothing on screen had changed to explain why. */
static bool s_topChosenEntry = false;

/* A track change asked for but not yet made: +1 next, -1 previous, 0 idle.
 *
 * Opening a track blocks for as long as the card takes - a few hundred
 * milliseconds - and it used to happen in the same frame as the press
 * that asked for it. The highlight was set first but could not reach the
 * screen until that frame finished, so it appeared after the track had
 * already changed and the control read as slow. Held over one frame, the
 * press is drawn and presented first and the work happens next, which is
 * the same total wait with the acknowledgement at the front of it. */
/* True once the ARM9 is running at the DSi's 134MHz rather than the DS's
 * 67MHz. Reported on the Test page, because it doubles every decode
 * budget shown beside it, and a percentage that halves between two
 * consoles has to say which one it came from. */
static bool s_twlSpeed = false;

static int  s_pendingTrackStep = 0;
static SkinButton s_pendingTrackBtn = SKIN_BTN_NONE;

/* Defined further down with the rest of the look-changing helpers; the
 * theme picker's key handler sits above it. */
static void ApplyTheme(int theme);
static void InvalidateTopScreen(void);

/* Mean frame work, so the readout is not one unlucky frame. Worst-case
 * alone cannot tell a setting that costs a little every frame from one
 * that costs a lot once. */

/* Frames that ran past two refreshes. The worst-case alone cannot say
 * whether a long frame was one event or a recurring stall, and those need
 * completely different answers: a handful means something one-off like a
 * track change, a steady stream means the player is dropping frames. */
static uint32_t s_diagTick = 0;

enum {
    /* What is left after the page was cut down.
     *
     * Eleven rows went, and each of them went one of two ways. Some were
     * settings that belong on Setup and were on both - the theme, the
     * list size. The rest were switches for things that measured better
     * on than off every time they were tried: the anti-image filter, the
     * filter table in fast RAM, the cover fetch, card writes. A switch
     * whose other position is always wrong is not a setting, it is a
     * question the code should have answered - so they are answered, and
     * the four measurements they were there to explain stayed.
     *
     * The order is: what it measures, then what changes it, then the
     * buttons. */
    TEST_ROW_RING = 0,
    TEST_ROW_GLITCH,
    TEST_ROW_LOAD,
    TEST_ROW_UI,
    TEST_ROW_RESAMPLE,
    TEST_ROW_RATECUT,
    TEST_ROW_NOISE,
    TEST_ROW_ENHANCE,
    TEST_ROW_CRC,
    TEST_ROW_SCROLL,
    TEST_ROW_ARTSCALE,
    TEST_ROW_ARTPACE,
    TEST_ROW_RESET,
    TEST_ROW_DEFAULTS,
    TEST_ROW_REGS,
    TEST_ROWS
};

/* Widest row list either page has, so one buffer serves both.
 *
 * Asserted rather than trusted, because it silently went wrong once:
 * adding rows to the Test page took it past this bound and every
 * BuildTestRows call then wrote one SkinRow past the end of a stack
 * array. Nothing crashed - it corrupted whatever local followed, which
 * is why the symptoms were "touch is odd" and "the font switch does
 * nothing" rather than anything pointing at the actual fault. */
/* Twelve rows on the Test page and eight on Setup; the assert beside
 * each list is what catches this being outgrown. */
/* The longer of the two pages, with room to add one. Both pages assert
 * against it, which is what caught this being 14 the moment the Test page
 * grew a fifteenth row - a silent overrun of a stack array otherwise, and
 * Skin_DrawRows dereferencing whatever was next to it as a label. */
/* Big enough for the longer of the two settings pages, which is Test -
 * it carries the readouts and the diagnostic levers as well as its own
 * settings. The static asserts below each builder are what caught this
 * being too small when the output-rate row was added, rather than a
 * stack overwrite at the far end of the array. */
#define SETTINGS_MAX_ROWS 28

/* Written by the transport handler, drained into Settings by the frame
 * loop - the handler does not own the config object. -1 means nothing
 * changed since the last drain. */
static int s_pendingShuffle = -1;
static int s_pendingRepeat  = -1;


/* The embedded picture for whatever is playing.
 *
 * Loaded a frame after the track starts rather than during the change:
 * reading tens of kilobytes off the card and Huffman-decoding it is not
 * something to add to the slowest moment in the program. The music
 * begins, then the picture appears. */

static int s_testScroll = 0;
static int s_setupScroll = 0;

/* Drag-to-scroll state for the settings pages.
 *
 * The rail on the right works, but it is not what anyone reaches for:
 * the instinct on a touchscreen is to drag the list itself, and a rail
 * you have to aim at is a poor substitute even when it is wide. So a
 * press on a choice row now starts a gesture rather than acting - if the
 * stylus moves, it scrolls; if it lifts without moving, the tap lands.
 * Sliders are excluded because a drag on one is already a drag on one. */
#define ROW_PITCH 27
#define DRAG_SLOP 4

static int  s_dragStartY = -1;
static int  s_dragStartScroll = 0;
static bool s_dragMoved = false;
static bool s_dragOnSlider = false;
static int  s_tapRow = -1, s_tapIndex = 0;

/* Bumped whenever any setting changes.
 *
 * The dirty check compares a snapshot of what is on screen, and the
 * settings pages show values that were never in it - meter style, theme,
 * font, top screen mode. Changing one with the d-pad therefore changed
 * nothing the comparison could see, so the page was not redrawn and the
 * new value only appeared once something else forced a repaint, like
 * moving to another row. A touch happened to work because the press
 * feedback marked the frame dirty on its own.
 *
 * One counter rather than a dozen more snapshot fields: every future
 * setting is covered by it without anyone having to remember. */
static uint32_t s_uiRevision = 0;

/* What the modal was opened to do.
 *
 * The answer used to be worked out when it closed, by looking at whether
 * anything was marked and whether a playlist was selected - which is
 * guessing, and it only gave the right answer because an empty name made
 * the save fail and fall through to the delete. Remembering the intent
 * is both correct and one line. */
typedef enum { PENDING_NONE = 0, PENDING_SAVE, PENDING_DELETE,
               PENDING_PICK, PENDING_UNLIST,
               PENDING_UNLIST_ONE, PENDING_EXIT } PendingAction;
static PendingAction s_pending = PENDING_NONE;
static int s_pendingIndex = -1;

/* The playlists the picker offers, and where each one lives.
 *
 * Copied rather than pointed at. They used to be borrowed pointers into
 * the browser's entry table - every playlist in the folder being browsed
 * - and that was a list that is always empty in the one place the picker
 * opens from: marks are made in a music folder, and every playlist is
 * saved to the playlist folder. So the picker only ever offered "New
 * list", and there was no way to add to a list that already existed
 * without going into it first. They are read from the playlist folder
 * now, which is where they are.
 *
 * `s_pickHere` says which of them came from the folder being browsed
 * instead - a list saved beside its music before every new one started
 * going to the playlist folder. The append has to write where the file
 * actually is or it creates a second list of the same name. */
#define PICK_MAX 48
static char s_pickStore[PICK_MAX][FB_NAME_LEN];
static bool s_pickHere[PICK_MAX];
static const char *s_pickNames[PICK_MAX];
static int s_pickCount = 0;

/* The two ways to make a new list, offered alongside the ones that exist.
 * Putting the destination in the choice costs no extra step: picking
 * where to save it and picking what to save it into are the same
 * question asked once. */
static const char *const kNewRows[] = {
    /* One destination, not two.
     *
     * Saving "here" put a list in whatever folder the marks came from,
     * which is a reasonable thing to want and no longer a coherent one:
     * the Faves tab is where lists are kept and played from, and a list
     * saved beside the album it was made from never appears there. It
     * would exist, be findable only by browsing to it, and look like the
     * save having failed.
     *
     * So every new list goes to the playlist folder, where the tab that
     * exists to show lists will show it. */
    "New list",
};
#define NEW_ROWS ((int)(sizeof(kNewRows) / sizeof(kNewRows[0])))

/* Where PENDING_SAVE will write. */
static char s_saveDir[FB_PATH_LEN];

/* Drag-to-scroll for the file list, the same shape as the settings
 * pages'. Row height is not a compile-time constant any more, so the
 * step comes from the skin. */
#define LIST_H_FOR_DRAG (Skin_ListRowHeight())
static int  s_listDragY = -1, s_listDragTop = 0, s_listTap = -1;
static bool s_listMoved = false;

/* Keeps the d-pad cursor inside what the stylus just scrolled to.
 *
 * Without it the two input methods disagree: scrolling with the pen
 * leaves the cursor wherever it was, and the next press of up or down
 * moves it from there - which scrolls the list straight back to where it
 * had been. The cursor has to follow the view, or the pen's scroll is
 * undone by the first button press after it. */
/* Set while the stylus is on the progress bar; combined with the L/R
 * buttons at the one place that tells the decoder a scrub is running. */
/* The slider is volume; boost is a row on the Setup page.
 *
 * They were one control for a while - 127 steps of hardware volume and
 * six more of gain past the end of it - on the reasoning that "louder"
 * is one question and should have one answer. In practice the six extra
 * steps were unlabelled, indistinguishable from the end of the travel,
 * and undiscoverable: there was nothing to say the boost existed or what
 * it was set to.
 *
 * Worse, they could not be separated once a row existed for the boost as
 * well. The slider drove gain from the volume, so dragging the volume
 * down cleared a boost set on the Setup page, and a boost set below full
 * volume was a state the slider had no way to show and startup wiped on
 * the next launch. One writer per setting removes all of that.
 *
 * Six steps of two decibels, because the row names its values and can
 * only name them if a step is a whole entry in that list. */
#define VOL_GAIN_STEPS       6
#define VOL_GAIN_DB_PER_STEP 2
#define VOL_GAIN_MAX_DB      (VOL_GAIN_STEPS * VOL_GAIN_DB_PER_STEP)
#define VOL_LEVEL_MAX        127

_Static_assert(VOL_GAIN_MAX_DB <= GAIN_MAX_DB, "the mixer cannot go that loud");

/* The emptiest the audio buffer has been, as a percentage.
 *
 * Sampled every frame and never raised except by Measurements on the
 * Test page: the interesting value is the worst one, and the worst one
 * is over in a frame or two. */
static unsigned long s_ringLowMark = 100;

/* Diagnostic levers, deliberately not saved to the card: they are for
 * finding out where the time goes on one particular file, not for living
 * with. Mono especially - it is half the sound field. */

/* CRC checking off by default: it costs a little on every frame and buys
 * error detection on a stream that is nearly always intact. */

/* The rate-cut ladder: how much of the rate has been given up, what that
 * means for libmad, and what the stream is opened at. See the row that
 * draws it for why these are one control and not two. */
/* RATE_CUT_STOPS lives in settings.h, with the rest of what the loader
 * needs in order to clamp a config file. The three tables below are
 * asserted against it rather than sized by hand. */

/* Off: the file's own rate, decoded in full.
 *
 * It was fifty - half rate - back when the decoder needed the help more
 * than the music needed the top of its band. Half rate throws away
 * everything above 11kHz and adds a little aliasing of its own just
 * below it, which is a real cost on headphones.
 *
 * What paid for moving it back is the work in tightly-coupled memory:
 * the synthesis filter, its window table, the Layer III decoder, the
 * Huffman tables, the DCT, the IMDCT and the bit reader are all in fast
 * RAM now, and between them they bought back enough of the processor to
 * run at the file's own rate. */
static const char *const kRateCutName[RATE_CUT_STOPS] =
    { "off", "25", "50 (half rate)", "75", "100" };
static const bool     kRateCutHalf[RATE_CUT_STOPS] =
    { false, false, true, true, true };
/* The middle stop used to leave the output at 32768 while the decoder
 * produced 22050, which meant upsampling - and upsampling is the one
 * direction the cheap resamplers cannot do: the image of the music sits
 * just above the decoder's rate and a short window cannot reach it. At
 * the default setting a tone at 9kHz came out with a copy of itself at
 * 13kHz only five decibels down, which is what a swishing ghost on
 * hi-hats and sibilance actually is.
 *
 * Leaving the output at the decoder's own rate makes the resampler a
 * pass-through: no image to remove, no filter to run, and the full band
 * the decoder produced. It is cheaper as well as cleaner, so there was
 * never anything to weigh. */
static const uint32_t kRateCutOut[RATE_CUT_STOPS] =
    { 32768, 22050, 22050, 22050, 16384 };

/* The tables are what the setting actually indexes, so they are what
 * the clamp has to match. Sized from RATE_CUT_STOPS above, and counted
 * here as well, because a declaration of [RATE_CUT_STOPS] with four
 * initialisers is legal C and leaves a zero on the end rather than an
 * error. */
_Static_assert(sizeof(kRateCutName) / sizeof(kRateCutName[0]) == RATE_CUT_STOPS &&
               sizeof(kRateCutHalf) / sizeof(kRateCutHalf[0]) == RATE_CUT_STOPS &&
               sizeof(kRateCutOut)  / sizeof(kRateCutOut[0])  == RATE_CUT_STOPS,
               "the rate-cut tables and the range settings.c clamps to "
               "have to be the same length");
/* Sound presets: three bands, in whole decibels either side of flat.
 *
 * Written as the shape somebody would describe - "more bass, less mid" -
 * and normalised below rather than pre-cut here, so the table reads as
 * the intent and the arithmetic that protects the output is in one
 * place and visible.
 *
 * That normalisation is the important part. Every one of these is a
 * boost somewhere, and a boost on audio that is already near full scale
 * is clipping. The output carries about two decibels of headroom, so the
 * loudest band of a preset is pulled back to three and the rest come
 * with it - the shape is kept exactly and the level goes down, which is
 * a preset that sounds like itself rather than one that crackles. Turn
 * it back up with the volume, or with the boost on this same page. */
#define TONE_MAX_BOOST_DB 3

static const struct { const char *name; int8_t bass, mid, treble; }
kTonePresets[] = {
    { "Flat",       0,  0,  0 },
    { "Pop",       +2, +3, +1 },
    { "Rock",      +5, -1, +4 },
    { "Jazz",      +3, -1, +3 },
    { "Classical", +3, -2, +4 },
    { "Dance",     +6, -2, +4 },
    { "Hip-Hop",   +7, -1, +2 },
    { "R&B",       +6, +1, +4 },
    { "Metal",     +4, +2, +5 },
    { "Vocal",     -3, +4, +1 },
    { "Acoustic",  +3, +1, +3 },
    { "Electronic",+5, -2, +4 },
    { "Bass",      +8,  0,  0 },
    { "Treble",     0,  0, +7 },
    { "Loudness",  +6, -3, +5 },
};
#define TONE_PRESETS ((int)(sizeof(kTonePresets) / sizeof(kTonePresets[0])))

static int s_tonePreset = 0;
static int s_soundFocus = 0;
static int s_soundEntry = 0;   /* what was in force on the way in */

/* Hands the chosen shape to the decoder, pulled back so its loudest band
 * sits inside the headroom the output has. */
static void ApplyTonePreset(int i)
{
    if (i < 0 || i >= TONE_PRESETS) i = 0;
    s_tonePreset = i;

    int b = kTonePresets[i].bass, m = kTonePresets[i].mid,
        t = kTonePresets[i].treble;

    int top = b > m ? b : m;
    if (t > top) top = t;

    const int cut = (top > TONE_MAX_BOOST_DB) ? top - TONE_MAX_BOOST_DB : 0;
    Mp3Decoder_SetTone(b - cut, m - cut, t - cut);
}

/* Four frames: a step that did not exist before.
 *
 * Three repeats fast enough that a held d-pad runs away down a long
 * list; five is deliberate enough to feel slow. Four is the one people
 * actually keep, so it is where the player starts. */
static const uint8_t kScrollFrames[SCROLL_RATE_STOPS] = { 3, 4, 5, 8 };
_Static_assert(sizeof(kScrollFrames) / sizeof(kScrollFrames[0]) ==
               SCROLL_RATE_STOPS,
               "and the same for the scroll ladder");

/* The whole transform, by default.
 *
 * A cover is shown 116 pixels wide, and the decoder stops growing the
 * scale the moment it has that many - so on a 500 pixel cover, which is
 * most of them, this setting costs nothing at all: a quarter scale
 * already clears 116 and that is where it stops. What it buys is the
 * small cover. At half, a 300 pixel picture decoded to 75 and was
 * stretched to fit, and stretching is the one thing that looks like a
 * bad picture rather than like a fast one.
 *
 * Half rather than full, in the end: full decodes a small cover at the
 * size it deserves, and it also spends the processor doing it at exactly
 * the moment a track is starting. Half is the setting that keeps the
 * cover quick, and quick is what was asked for first.
 *
 * The other stops stay, because "Last cover" above reports the decode in
 * milliseconds and the trade is worth being able to measure. The names
 * are the fraction of the picture each one keeps. */
static const uint8_t kArtScaleN[4] = { 1, 2, 4, 8 };

/* No limit. A cover that arrives late beats one that never arrives,
 * which is what a deadline used to deliver on a busy track. */

/* And eager: the picture takes the processor when it wants it.
 *
 * Balanced was the default while standing down could last forever, when
 * the safe setting was the one that got out of the way. It cannot now -
 * a stand-down is capped, and below an eighth of the ring the decoder
 * boosts itself above the fetcher whatever the fetcher was told - so the
 * cautious middle setting is buying a slower cover and not much else.
 *
 * The other two stay on the Test page. Eager is the fastest a cover can
 * arrive and it is the setting that made the file list stutter when it
 * was the only behaviour on offer, so the trade is there to be taken
 * back by anyone who would rather have the list smooth. */

/* What a frame of drawing and input costs, as a percentage of the frame
 * it has to fit in. Smoothed, plus the worst seen since Measurements was
 * last touched - the worst is what a held d-pad produces and it is over
 * before anybody can read it. */
static unsigned long s_uiPct = 0, s_uiPctPeak = 0, s_uiPx = 0;

static int VolumeLevel(const PlayerState *p, const Settings *cfg)
{
    (void)cfg;
    return p->volume;
}

static void SetVolumeLevel(PlayerState *p, Settings *cfg, int level)
{
    (void)cfg;
    if (level < 0) level = 0;
    if (level > VOL_LEVEL_MAX) level = VOL_LEVEL_MAX;
    Player_SetVolume(p, level);
}

static bool s_penScrubbing = false;

/* Set while the stylus has hold of the progress bar. Without it the bar
 * could be tapped but never dragged: the press-only gate below sits
 * between the volume's hit test and the progress bar's, so one tracked
 * the finger and the other fired once and stopped. */
static bool s_progressDrag = false;



/* The Test row asking for the register page. Opening it needs the player,
 * which the row handler is not given. */
static bool s_openRegs = false;

/* Set by the Test row, acted on by the loop - ApplyTestChoice has no
 * PlayerState to change the view with, which is the same reason the
 * registers page opens this way. */
static bool s_openMeters = false;

/* The registers the page watches.
 *
 * One is read per frame rather than all of them at once: each is a PXI
 * round trip that blocks this CPU until the ARM7 answers, and eighteen of
 * those in a row is a visible stall on the thread that also feeds the
 * decoder. Cycling means a full sweep costs one frame each and the page
 * still refreshes three times a second.
 *
 * The PMIC only. The MCU is where a DSi keeps its volume and backlight
 * and would have been the interesting half - but it is reached over I2C,
 * and reading that from a DS-mode ROM is the transaction the ARM7 file
 * warns about: it does not return, and it hung the console the first time
 * this page was opened. */
static const struct { const char *name; uint8_t bus, reg; } kRegWatch[] = {
    { "PMIC", SPK_BUS_PMIC, 0x00 }, { "PMIC", SPK_BUS_PMIC, 0x01 },
    { "PMIC", SPK_BUS_PMIC, 0x02 }, { "PMIC", SPK_BUS_PMIC, 0x03 },
    { "PMIC", SPK_BUS_PMIC, 0x04 }, { "PMIC", SPK_BUS_PMIC, 0x10 },
};
#define REG_WATCH_COUNT ((int)(sizeof(kRegWatch) / sizeof(kRegWatch[0])))

static uint8_t s_regValue[REG_WATCH_COUNT];
static uint8_t s_regBase[REG_WATCH_COUNT];
static bool    s_regPrimed = false;
static bool    s_regRebase = false;

/* Reads one register per call, cycling. Returns true if the page needs
 * repainting - which is only when a value actually moved, so a page of
 * still numbers costs nothing to keep on screen. */
static bool PollOneRegister(void)
{
    static int at = 0;

    uint8_t v = (uint8_t)Jack_ReadReg(kRegWatch[at].bus, kRegWatch[at].reg);
    bool changed = (!s_regPrimed) || (v != s_regValue[at]);

    s_regValue[at] = v;
    if (!s_regPrimed) s_regBase[at] = v;

    if (++at >= REG_WATCH_COUNT) { at = 0; s_regPrimed = true; }
    return changed;
}

/* How long the frame has been running over budget, and whether the meter
 * has dropped to half rate because of it. See the frame loop. */
/* How much of playback the decoder spends boosted. The meter runs at
 * half rate for exactly this fraction of the time, so a high number here
 * is the bars looking slow while the audio is perfect. */

static int  s_visPressure = 0;
static bool s_visHalfRate = false;

static void PullFocusIntoView(int *focus, int top, int visible)
{
    if (!focus) return;
    if (*focus < top) *focus = top;
    if (*focus > top + visible - 1) *focus = top + visible - 1;
}

/* The diagnostics settings, put into force.
 *
 * Called at startup and by that page's reset button. One function, so a
 * reset cannot land somewhere a fresh install would not - which is what
 * happened when the two lists were written out separately and one of
 * them was updated. */
static void ApplyDiagSettings(const Settings *cfg)
{
    Mp3Decoder_SetResampleMode(cfg->resampleMode);
    Mp3Decoder_SetNoiseFilter(cfg->noiseFilter);
    Mp3Decoder_SetEnhance(cfg->enhance);
    Mp3Decoder_SetHalfRate(kRateCutHalf[cfg->rateCut]);
    Audio_SetOutputRate(kRateCutOut[cfg->rateCut]);
    Mp3Decoder_RateChanged();
    Mp3Decoder_SetIgnoreCrc(cfg->ignoreCrc);
    Mp3Decoder_SetArtPace(cfg->artPace);
    Jpeg_SetMaxScale(kArtScaleN[cfg->artScale]);
    keysSetRepeat(15, kScrollFrames[cfg->scrollRate]);
}

#ifdef ARM9
static void MainStackUsed(uint32_t *deepest, uint32_t *untouched);
#endif

static int BuildTestRows(SkinRow *out, const Settings *cfg)
{
    (void)cfg;
    _Static_assert(TEST_ROWS <= SETTINGS_MAX_ROWS, "row buffer too small");

    /* Zeroed first. The array is a local, so a row added to the enum but
     * not assigned here leaves stack garbage that Skin_DrawRows will
     * happily dereference as a label pointer - which is a crash the
     * moment that row scrolls into view, and nothing before it. That
     * exact omission shipped once. */
    memset(out, 0, TEST_ROWS * sizeof(*out));

    static char sizeLabel[40];
    {
        static const char *const kNames[SKIN_LIST_SIZES] = {
            "compact", "roomy", "large", "largest"
        };
        snprintf(sizeLabel, sizeof(sizeLabel), "List size: %s",
                 kNames[Skin_ListSize()]);
    }
    static char themeLabel[40];


    /* The resampler, which is the one setting on either page that trades
     * sound quality for CPU. It lives here rather than in Setup because
     * this is the page for things that cost time.
     *
     * All six modes, including Raw and Lite - Setup used to stop at Clean
     * because the bottom two are blunt instruments, and on a page of
     * blunt instruments that is not a reason to hide them. */
    static const char *const kRes[] = { "None", "Raw", "Lite", "Fast",
                                        "Wide", "Clean", "Best" };
    _Static_assert(sizeof(kRes) / sizeof(kRes[0]) == RESAMPLE_MODES,
                   "a resampler mode has no name - the row would read off "
                   "the end of this list");
    int res = Mp3Decoder_ResampleMode();
    out[TEST_ROW_RESAMPLE] = (SkinRow){ SKIN_ROW_CHOICE, "Resampling",
                                        kRes, RESAMPLE_MODES,
                                        (res >= 0 && res < RESAMPLE_MODES)
                                            ? res : RESAMPLE_LITE };

    /* Off stops this program touching the card at all: no settings, no
     * playlists, no resume position. It is here to answer one question
     * that reading the code cannot - whether the corruption seen after a
     * power-off has anything to do with this app. If a card still
     * corrupts with this Off, nothing here is responsible.
     *
     * The choice survives a reboot: switching it off writes once to
     * record itself and then stops. */
    /* What became of the last cover.
     *
     * Not a setting - a readout, on the page for things that cost time.
     * A cover that is slow to appear can be a slow card, a large
     * picture, a format this cannot read, or a processor with nothing to
     * spare, and those are indistinguishable from the outside. Two
     * rounds of plausible fixes went to the wrong one of them before
     * this line existed. */
    static char artDetail[48];
    {
        Mp3ArtReport ar;
        Mp3Decoder_GetArtReport(&ar);
        const unsigned long kb = (unsigned long)(ar.bytes / 1024u);

        switch (ar.outcome) {
        case ART_TOO_BIG:
            snprintf(artDetail, sizeof(artDetail), "%luK - too big to read", kb);
            break;
        case ART_NOT_JPEG:
            snprintf(artDetail, sizeof(artDetail), "%luK - not a JPEG", kb);
            break;
        case ART_UNDECODABLE:
            snprintf(artDetail, sizeof(artDetail), "%luK - not baseline JPEG", kb);
            break;
        case ART_TOO_SLOW:
            snprintf(artDetail, sizeof(artDetail), "%luK - gave up at %lums", kb,
                     (unsigned long)ar.decodeMs);
            break;
        case ART_READ_FAILED:
            snprintf(artDetail, sizeof(artDetail), "%luK - read stopped at %lums",
                     kb, (unsigned long)ar.readMs);
            break;
        case ART_OK:
            snprintf(artDetail, sizeof(artDetail), "%luK   read %lums   decode %lums",
                     kb, (unsigned long)ar.readMs, (unsigned long)ar.decodeMs);
            break;
        default:
            snprintf(artDetail, sizeof(artDetail), "none in this file");
            break;
        }
    }

    /* value -1 so no segment lights up: this is a readout, and a
     * highlighted segment would read as a setting that is switched on.
     * The same spelling the headphone row uses when the socket cannot be
     * read - there is nothing here to choose. */



    /* The audio buffer, now and at its worst.
     *
     * The number that decides everything else on this page. Full means
     * the decoder is running ahead and there is time to spare; a low
     * mark near zero means it is not, and that whatever else the program
     * does has to come out of a margin that is not there.
     *
     * The low mark is the one to watch while holding the d-pad. It is
     * over in a frame or two, so by the time anybody looks at this page
     * the live figure has recovered and says nothing at all. */
    static char ringDetail[48];
    static const char *ringSeg[1] = { ringDetail };
    {
        uint32_t now = (Audio_QueuedFrames() * 100u) / AUDIO_RING_FRAMES;
        if (now > 100u) now = 100u;
        snprintf(ringDetail, sizeof(ringDetail), "%lu%% now,   %lu%% lowest",
                 (unsigned long)now, s_ringLowMark);
    }
    out[TEST_ROW_RING] = (SkinRow){ SKIN_ROW_CHOICE, "Audio buffer",
                                    ringSeg, 1, -1 };

    /* What the buffer running dry actually sounded like.
     *
     * The row above says how full the buffer is now and how empty it has
     * been, and neither answers the question anyone is really asking,
     * which is "did I hear that". The mixer knows: every time it is asked
     * for samples that are not there it pads with silence and counts the
     * frames, and those frames are the gap in the music. In milliseconds
     * because that is the unit a person hears in - a hundred frames means
     * nothing, three milliseconds is a tick.
     *
     * Dropped is the opposite fault and worth having beside it: audio the
     * decoder produced and the ring had no room for, which is not audible
     * as a gap but means the two ends are fighting.
     *
     * Both are cleared by Measurements below, and by the start of every
     * track - so this reads as "this track, so far". */
    static char glitchDetail[72];
    static const char *glitchSeg[1] = { glitchDetail };
    {
        const uint32_t rate = Audio_OutputRate() ? Audio_OutputRate() : 1;
        const uint32_t lostMs = (Audio_UnderrunFrames() * 1000u) / rate;

        /* Relief belongs on this line rather than a row of its own: it is
         * the same story as a glitch, told one stage earlier. A track
         * that reads "0 ms lost" with relief on did not skip - it went
         * dull for a moment so that it would not have to. Without it
         * here, the two are indistinguishable from the outside, which is
         * exactly the confusion this row exists to end. */
        const uint32_t relief = Mp3Decoder_ReliefCount();
        if (relief)
            snprintf(glitchDetail, sizeof(glitchDetail),
                     "%lu ms lost, %lu drop, eased %lux%s",
                     (unsigned long)lostMs,
                     (unsigned long)Audio_DroppedFrames(),
                     (unsigned long)relief,
                     Mp3Decoder_ReliefActive() ? " now" : "");
        else
            snprintf(glitchDetail, sizeof(glitchDetail),
                     "%lu ms lost,   %lu frames dropped",
                     (unsigned long)lostMs,
                     (unsigned long)Audio_DroppedFrames());
    }
    out[TEST_ROW_GLITCH] = (SkinRow){ SKIN_ROW_CHOICE, "Glitches",
                                      glitchSeg, 1, -1 };

    /* And what is emptying it: the decoder's share of real time. This is
     * the number every other lever on this page is trying to move. */
    static char loadDetail[48];
    static const char *loadSeg[1] = { loadDetail };
    {
        uint32_t mad = 0, synth = 0, res = 0;
        Mp3Decoder_LoadSplit(&mad, &synth, &res);
        snprintf(loadDetail, sizeof(loadDetail),
                 "%lu%%:  bits %lu  filter %lu  resamp %lu",
                 (unsigned long)(mad + synth + res),
                 (unsigned long)mad, (unsigned long)synth, (unsigned long)res);
    }
    out[TEST_ROW_LOAD] = (SkinRow){ SKIN_ROW_CHOICE, "Decoder load",
                                    loadSeg, 1, -1 };

    /* And the other side of the machine: what one frame of drawing and
     * input costs, against the frame it has to fit in. The decoder
     * figure above and this one are the whole budget between them, so
     * two small numbers here means the time is going somewhere neither
     * of them is looking. */
    static char uiDetail[44];
    static const char *uiSeg[1] = { uiDetail };
    {
#ifdef ARM9
        /* The stack high-water mark rides along here rather than taking
         * a row: it is a number wanted once, to settle whether the
         * decoder's filter state can move into DTCM. */
        uint32_t deep = 0, spare = 0;
        MainStackUsed(&deep, &spare);
        snprintf(uiDetail, sizeof(uiDetail), "%lu%% %lu%% %lupx stk%lu/%lu",
                 (unsigned long)s_uiPct, (unsigned long)s_uiPctPeak,
                 (unsigned long)s_uiPx,
                 (unsigned long)deep, (unsigned long)(deep + spare));
#else
        snprintf(uiDetail, sizeof(uiDetail), "%lu%% frame  %lu%% worst  %lu px",
                 (unsigned long)s_uiPct, (unsigned long)s_uiPctPeak,
                 (unsigned long)s_uiPx);
#endif
    }
    out[TEST_ROW_UI] = (SkinRow){ SKIN_ROW_CHOICE, "Screen load",
                                  uiSeg, 1, -1 };

    /* Two levers aimed at the same question: is the time going into the
     * audio or into the screen? Mono halves the resampler, which is the
     * decoder's most expensive loop; scroll step changes how often
     * holding the d-pad asks for a repaint. If the stutter follows one
     * of them, that says which. */
    /* The decoder's own levers, in the order of how much they save.
     *
     * Half rate is the large one and the loud one: the synthesis filter
     * is the most expensive stage of decoding and this takes its short
     * path, at the cost of everything above about eleven kilohertz.
     * Ignoring CRC is nearly free and gives up error detection. Mono
     * halves the resampler, which is the stage after. */
    /* One slider where there were two switches.
     *
     * The two levers were half rate, which halves the synthesis filter,
     * and the stream's rate, which is the only thing that reaches the
     * resampler. They are not independent in any way that matters: both
     * spend bandwidth to buy time, and the settings worth having are the
     * ones that spend it together. So they are five stops on one control,
     * and the number is how much of the rate has been given up.
     *
     *     0   everything at full rate
     *    25   the stream drops to 22050; the filter is untouched
     *    50   half rate - this is where the filter halves
     *    75   half rate into a 22050 stream, so on a 44.1kHz file the two
     *         rates meet and the resampler stops running at all
     *   100   the same, into 16384
     *
     * Monotonic in cost, which is the point of a slider: further right is
     * always cheaper and always duller, with no combination that is worse
     * at both. */
    static char rateLabel[40];
    snprintf(rateLabel, sizeof(rateLabel), "Rate cut: %s",
             cfg->rateCut == 0 ? "off" : kRateCutName[cfg->rateCut]);
    out[TEST_ROW_RATECUT] = (SkinRow){ SKIN_ROW_SLIDER, rateLabel,
                                       NULL, RATE_CUT_STOPS, cfg->rateCut };

    /* Two effects rather than measurements, and the only two rows on
     * this page that change how the music sounds on purpose.
     *
     * Both read "off" at zero and cost nothing there, which is where
     * they start: this console does not have processor to spare and
     * neither of these is something the player needs to work. The
     * numbers are strengths, not units - what they mean is best learned
     * by moving them while something plays, which is why they are
     * sliders and why the first one says what it is doing.
     *
     * The noise filter leans on the top of the band when there is
     * nothing up there but hiss, so it helps a recording with tape or
     * encoder noise under it and does nothing for a clean one. It cannot
     * do anything at all about a gap in the sound - that is the decoder
     * running out of audio, not noise in it, and no filter reaches it. */
    static char noiseLabel[48];
    {
        const int cut = Mp3Decoder_NoiseCutDb();
        if (cfg->noiseFilter == 0)
            snprintf(noiseLabel, sizeof(noiseLabel), "Noise filter: off");
        else if (cut < 0)
            snprintf(noiseLabel, sizeof(noiseLabel),
                     "Noise filter: %d, cutting %d dB", cfg->noiseFilter, -cut);
        else
            snprintf(noiseLabel, sizeof(noiseLabel),
                     "Noise filter: %d, nothing to cut", cfg->noiseFilter);
    }
    out[TEST_ROW_NOISE] = (SkinRow){ SKIN_ROW_SLIDER, noiseLabel, NULL,
                                     Mp3Decoder_ShapeStops(), cfg->noiseFilter };

    /* And the other way: the output is band limited - by the resampler's
     * window and by the console's own rate - so the top octave is simply
     * not there. This puts some of it back by making harmonics out of
     * the band below it, which is not the same as recovering what was
     * lost but is what the ear reads as air and detail. */
    static char enhLabel[48];
    if (cfg->enhance == 0)
        snprintf(enhLabel, sizeof(enhLabel), "Enhance: off");
    else
        snprintf(enhLabel, sizeof(enhLabel), "Enhance: %d", cfg->enhance);
    out[TEST_ROW_ENHANCE] = (SkinRow){ SKIN_ROW_SLIDER, enhLabel, NULL,
                                       Mp3Decoder_ShapeStops(), cfg->enhance };

    static const char *const kOnOff[] = { "Off", "On" };
    out[TEST_ROW_CRC] = (SkinRow){ SKIN_ROW_CHOICE, "Skip CRC checks",
                                   kOnOff, 2, cfg->ignoreCrc ? 1 : 0 };

    /* The two channels are summed before the synthesis filter rather than
     * after it, so this halves the filter as well as the resampling -
     * and the filter is the expensive stage. It is the way to run at
     * full rate for about what half rate costs: the treble is kept and
     * the stereo is what goes. */

    static const char *const kScroll[] = { "Fast", "Quick", "Steady",
                                           "Easy" };
    _Static_assert(sizeof(kScroll) / sizeof(kScroll[0]) ==
                   sizeof(kScrollFrames) / sizeof(kScrollFrames[0]),
                   "a scroll step has no name - the row would read off the "
                   "end of this list");
    out[TEST_ROW_SCROLL] = (SkinRow){ SKIN_ROW_CHOICE, "Scroll step",
                                      kScroll, 4, cfg->scrollRate };

    /* How much of each block of the cover is transformed. Coarser is
     * cheaper by far more than the transform itself costs: the plane it
     * writes shrinks by the square, and the plane is what pushes the
     * Huffman tables out of a four-kilobyte cache. Watch "Last cover"
     * above while changing it. */
    /* A way to take the cover out of the picture entirely, without
     * giving up the card that shows it.
     *
     * This is the one lever on the page that answers a question the
     * others cannot. A cover is read off the card and entropy-decoded on
     * a background thread while the track starts, and a large one - six
     * hundred pixels square is a hundred kilobytes and several seconds
     * of it - is doing that during exactly the opening of the track
     * somebody is listening to. If the noise at the start of a track
     * goes away with this off, it was never the decoder: it was the two
     * of them competing. Turning the top screen off would answer the
     * same question and lose the page as well. */

    static const char *const kArtScale[] = { "Eighth", "Quarter",
                                             "Half", "Full" };
    out[TEST_ROW_ARTSCALE] = (SkinRow){ SKIN_ROW_CHOICE, "Cover detail",
                                        kArtScale, 4, cfg->artScale };

    /* How hard the cover competes for the processor.
     *
     * This is the setting that moves the number. A cover's wall-clock
     * time is its work divided by the share it gets, and at 320 kbps the
     * decoder wants nearly all of it - so where the fetcher sits matters
     * more than anything about the decoding. Read "Last cover" after
     * each: Eager is the fastest and the one that can make the list
     * stutter, Patient never disturbs anything and is slow. */
    static const char *const kArtPace[] = { "Patient", "Balanced", "Eager" };
    out[TEST_ROW_ARTPACE] = (SkinRow){ SKIN_ROW_CHOICE, "Cover priority",
                                       kArtPace, 3, cfg->artPace };

    /* How long a cover may take before it is given up on. No limit by
     * default: a cover that arrives late is better than one that never
     * arrives, and the limit exists for anyone who would rather have the
     * disc than the wait. */


    /* The counters' reset, as a row like everything else on this page.
     * -1 lights no segment, so it reads as a button rather than as a
     * setting that is currently off. */
    static const char *const kReset[] = { "Reset counters" };
    out[TEST_ROW_RESET] = (SkinRow){ SKIN_ROW_CHOICE, "Measurements",
                                     kReset, 1, -1 };

    /* Everything on this page back to what it ships as.
     *
     * The page exists to be experimented with, and an experiment needs a
     * way back: without one the only route to a known state was to work
     * out what each default had been, one row at a time. It puts back
     * the settings, not the counters - the row above does those. */
    static const char *const kDefaults[] = { "Reset to defaults" };
    out[TEST_ROW_DEFAULTS] = (SkinRow){ SKIN_ROW_CHOICE, "All of the above",
                                        kDefaults, 1, -1 };

    /* Watch the hardware registers while something changes them. The
     * sleep button that used to sit here proved what it had to - that an
     * open lid cannot hold a sleep - and had no use after that. */
    static const char *const kRegs[] = { "Watch" };
    out[TEST_ROW_REGS] = (SkinRow){ SKIN_ROW_CHOICE, "Registers",
                                    kRegs, 1, -1 };
    snprintf(themeLabel, sizeof(themeLabel), "Theme: %s",
             Skin_ThemeName(Skin_Theme()));
    /* A slider, not ten segments. Ten names across 222 pixels is 22
     * pixels each - three characters - so the name goes in the caption
     * where there is room for it. */
    /* The font moved to Setup, where it is a page of twelve faces each
     * drawn in itself rather than a row of names that say nothing. */
    /* A slider rather than a strip of segments: four steps is enough
     * that named buttons stop being worth the width, and dragging a bar
     * is the natural way to try sizes until one looks right. */

    /* Say so, rather than leaving a gap. The memset above already turns a
     * forgotten row from stack garbage into something harmless, but
     * harmless and invisible is how the omission survived review in the
     * first place - it drew perfectly until someone scrolled far enough.
     * A row that announces itself is found in seconds. */


    /* A row in the enum with nothing assigned to it.
     *
     * This is a real failure, not a cosmetic one - the caption is a
     * pointer into a zeroed local, so without the fallback below
     * Skin_DrawRows dereferences NULL. The fallback keeps it drawable and
     * names it, which is how the resampler row was found missing after an
     * edit removed the measurement rows either side of it and took that
     * one with them.
     *
     * The count is what the caller sees, so a page that is short a row
     * still draws the rows it has. */
    for (int i = 0; i < TEST_ROWS; i++)
        if (!out[i].caption) out[i].caption = "(unset row)";

    return TEST_ROWS;
}

static void InitSubScreen(void)
{
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);

    int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    bgSetPriority(bg, 3);

    s_subVram = (uint16_t *)bgGetGfxPtr(bg);

    memset(s_subBackbuf, 0, sizeof(s_subBackbuf));
    dmaCopyWords(3, s_subBackbuf, s_subVram, sizeof(s_subBackbuf));
}

/* Must be called during vblank.
 *
 * Only the rows the compose step actually wrote get transferred. The
 * backbuffer and VRAM are identical everywhere else - they were left in
 * sync by the previous present - so copying the full 96KB every time was
 * paying for 192 rows to fix the 15 that moved. Flushing less matters as
 * much as copying less: DC_FlushRange over the whole buffer walks 3072
 * cache lines whatever changed. */
/* Replays a scroll that the drawing buffer has already had, in video
 * memory, so the moved pixels never have to travel from main RAM again.
 *
 * A row of the file list scrolling shifts a band of about sixty
 * kilobytes. Sent the ordinary way that is a cache flush over the band
 * and a DMA of the same size, twenty times a second while the d-pad is
 * held - to move pixels that had not changed, only relocated. Done here
 * it is a DMA inside video memory, which touches neither the cache nor
 * main RAM, and the only thing left to send is the row that was exposed.
 *
 * The flush is the part that hurt most. Video memory is not cached, but
 * the drawing buffer is, and walking a sixty-kilobyte range of it
 * evicts the decoder's tables from a data cache that is four kilobytes
 * in total - so the audio thread paid for the scroll twice, once in bus
 * traffic and again in every table lookup afterwards.
 *
 * Row at a time because the band is narrower than the screen: the
 * scrollbar rail sits outside it and must not move. */
static void ScrollVramLike(const GfxScroll *s)
{
    const int dist = s->dy < 0 ? -s->dy : s->dy;
    const int moved = s->h - dist;
    if (moved <= 0) return;

    const size_t bytes = (size_t)s->w * sizeof(uint16_t);

    if (s->dy < 0) {
        for (int r = 0; r < moved; r++)
            dmaCopyWords(3, s_subVram + (size_t)(s->y + r + dist) * GFX_W + s->x,
                            s_subVram + (size_t)(s->y + r)        * GFX_W + s->x,
                            bytes);
    } else {
        for (int r = moved - 1; r >= 0; r--)
            dmaCopyWords(3, s_subVram + (size_t)(s->y + r)        * GFX_W + s->x,
                            s_subVram + (size_t)(s->y + r + dist) * GFX_W + s->x,
                            bytes);
    }
}

static void PresentSubScreen(void)
{
    /* Before the early-out: a shift has to reach the screen even on a
     * frame where nothing else was drawn, or the buffer and the screen
     * disagree from then on. */
    GfxScroll scroll;
    if (Gfx_TakePendingScroll(&scroll) && scroll.target == s_subBackbuf)
        ScrollVramLike(&scroll);

    /* Run by run, not first-to-last.
     *
     * A scroll writes two rows of text at the bottom of the list and
     * moves a scrollbar thumb near the top, and everything between them
     * is untouched. Sent as one span that was most of the screen. */
    int y0, y1;
    for (int at = 0; Gfx_NextDirtyRun(at, &y0, &y1); at = y1) {
        const size_t offset = (size_t)y0 * GFX_W;
        const size_t words  = (size_t)(y1 - y0) * GFX_W * sizeof(uint16_t);

        /* DMA reads main RAM directly and knows nothing about the ARM9's
         * data cache, so the freshly drawn rows have to be written out of
         * the cache before the transfer or the display gets a mix of new
         * and stale pixels. */
        DC_FlushRange(s_subBackbuf + offset, words);
        dmaCopyWords(3, s_subBackbuf + offset, s_subVram + offset, words);
    }
}

/* Shuts the console down when the sleep timer expires with "turn the DS
 * off" ticked.
 *
 * Not systemShutDown(): that is powerOn(PM_SYSTEM_PWR), and PM_SYSTEM_PWR
 * is not PM_ARM9_DIRECT, so on libnds 2.0 it hits the same guard that
 * makes the backlight call a no-op - the ARM9 powerOn/powerOff both test
 * bit 16 and return immediately for anything else. The supported route is
 * calico's: clear any jump target so exiting does not just bounce back to
 * a homebrew menu, then ask the app to exit. pmMainLoop() then returns
 * false, main() returns, and calico powers the console off.
 *
 * On a DSi this reboots rather than powers off - that is calico's
 * documented behaviour for DSi mode, not something this can override. */
/* How much of the main thread's stack has ever been used.
 *
 * This exists to answer one question with a measurement instead of a
 * guess: the decoder's hottest data - libmad's four-kilobyte polyphase
 * filter state - sits in main RAM and is read and written per sample,
 * which is more than the four-kilobyte data cache can hold alongside
 * everything else. DTCM has no wait states and would fix that, but DTCM
 * is shared: the settings and buffers grow up from the bottom and this
 * stack grows down from the top, so anything moved in comes straight
 * off the stack.
 *
 * Nobody knows how much of that stack is actually used. The largest
 * frame in the whole binary is libmad's 7104 bytes and that runs on the
 * worker's own stack in main RAM, not here; the next largest anywhere is
 * about a kilobyte. So there is very likely room - but "very likely" is
 * not good enough to take memory away from a stack, because the symptom
 * of getting it wrong is a crash with no clue attached.
 *
 * So the unused part is painted at start-up and the deepest mark it has
 * ever reached is reported. Once that number is known from a real
 * session - menus, playlists, the keyboard, a modal or two - the trade
 * can be made on evidence.
 *
 * Painted from the end of the static data up to just under the current
 * stack pointer, so this function's own frame and everything below it
 * are left alone. */
#ifdef ARM9
#define DTCM_PAINT 0xC5C5C5C5u
extern char __dtcm_bss_end[];

static uint32_t s_stackSpan = 0;

static void PaintMainStack(void)
{
    uint32_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));

    uint32_t *p   = (uint32_t *)(void *)__dtcm_bss_end;
    uint32_t *end = (uint32_t *)((sp - 512) & ~3u);   /* clear of our own frame */

    if (end <= p) return;
    s_stackSpan = (uint32_t)((char *)end - (char *)p);
    while (p < end) *p++ = DTCM_PAINT;
}

/* The deepest it has been, in bytes, and how much was never touched. */
static void MainStackUsed(uint32_t *deepest, uint32_t *untouched)
{
    const uint32_t *p = (const uint32_t *)(const void *)__dtcm_bss_end;
    uint32_t clean = 0;

    while (clean < s_stackSpan && p[clean / 4] == DTCM_PAINT) clean += 4;

    *untouched = clean;
    *deepest   = (s_stackSpan > clean) ? (s_stackSpan - clean) : 0;
}
#endif

/* Records where playback got to, for the next session to offer back.
 *
 * The rule this follows is the whole reason it is safe to have at all:
 * it is called only at moments when nothing is playing. A card write
 * takes the main thread into libfat and down onto the card, and doing
 * that in the middle of a track is the single most likely thing to
 * interrupt audio on a flashcart - which is exactly why the previous
 * attempt at this, which wrote on every track change, had to go.
 *
 * There are three such moments and no others: playback stopping for any
 * reason (the pause button, the touch transport, the shoulders, the lid
 * with Sleep chosen, the sleep timer, the end of the queue), the sleep
 * timer powering the console off, and leaving the program. A session
 * spent listening straight through writes once, on the way out.
 *
 * Marking dirty rather than writing: the settle timer in settings.c
 * then folds a pause-play-pause into one write instead of three, and
 * the write itself goes through the same staged temporary-and-rename
 * every other write here uses, so an interrupted one cannot leave a
 * half-written config behind. */
/* Where leaving actually lands, which depends on how this was started.
 *
 * In one place because two things say it now: the row on the Setup page,
 * and the confirmation that row opens. Two copies would eventually
 * disagree, and the one the user reads before answering "yes" is the
 * one that has to be right. */
static const char *ExitDestination(void)
{
    if (pmHasResetJumpTarget()) return "Back to the menu";
    return systemIsTwlMode() ? "Back to the system menu"
                             : "The console powers off";
}

static void RememberPosition(Settings *s, const PlayerState *p)
{
    if (!p->hasTrack) return;

    const char *cur = Player_CurrentPath();
    if (!cur || !cur[0]) return;

    int ms = (int)Mp3Decoder_ElapsedMs();

    /* Already recorded. Worth the compare: the three callers overlap by
     * design - the sleep timer pauses playback and then powers off, and
     * leaving the program follows whatever stopped it - so the same
     * position is offered up two or three times in a row, and without
     * this each of those would be a separate trip to the card. */
    if (ms == s->resumeMs && !strcmp(s->resumePath, cur)) return;

    snprintf(s->resumePath, sizeof(s->resumePath), "%s", cur);
    s->resumeMs = ms;
    Settings_MarkDirty();
}

static void RequestPowerOff(PlayerState *player)
{
    Player_Stop(player);
    setBrightness(3, -16);

    pmClearResetJumpTarget();
    pmPrepareToReset();
}

/* The card is the one failure this program cannot work around, so it gets
 * a real screen rather than a silent hang. */
static void FatalScreen(uint16_t *bmp, const char *line1, const char *line2)
{
    Gfx_SetTarget(bmp);
    Gfx_VGradient(0, 0, GFX_W, GFX_H,
                  GFX_RGB8(0x0A, 0x24, 0x6A), GFX_RGB8(0x04, 0x0C, 0x24));

    Gfx_Fill(20, 70, GFX_W - 40, 52, GFX_RGB8(0x18, 0x2C, 0x54));
    Gfx_Bevel(20, 70, GFX_W - 40, 52,
              GFX_RGB8(0xBC, 0xD8, 0xFF), GFX_RGB8(0x05, 0x1E, 0x50), false);

    Gfx_TextShadow(32, 82, GFX_RGB8(0xFF, 0xFF, 0xFF), GFX_RGB8(0x04, 0x0C, 0x1C), line1);
    Gfx_TextShadow(32, 98, GFX_RGB8(0xA8, 0xC4, 0xE8), GFX_RGB8(0x04, 0x0C, 0x1C), line2);

    /* Drawn into the backbuffer like everything else, so it still has to
     * be pushed to VRAM or the error would never appear. */
    swiWaitForVBlank();
    PresentSubScreen();

    while (pmMainLoop())
        swiWaitForVBlank();
}

/* Real DS hardware has exactly one VRAM bank (C) that can drive the sub
 * screen's bitmap BG layer - bank D, which doubles up the main screen's
 * visualizer, can only be a *main*-engine BG, ARM7 work RAM, sprites or a
 * 3D texture slot on the sub side, never a sub BG (see VRAM_D_TYPE in
 * libnds' video.h). So unlike the top screen, the bottom screen cannot
 * be double buffered - there is nowhere to put a second 128KB buffer.
 *
 * That makes how *often* it is repainted matter a great deal. Rewriting
 * every gradient and bevel on it every single vblank writes into the
 * exact memory being scanned out to the LCD at that moment, and a full
 * repaint takes many scanlines' worth of time - so different lines of
 * the same displayed frame end up showing different "generations" of
 * the UI. Doing that 60 times a second is what reads as flicker, and
 * redrawing a mostly-static screen that often also spends CPU the
 * decoder needs, which is what turns into audible stutter. The fix for
 * both is the same: only repaint when the bottom screen would actually
 * look different, and do it right after entering vblank rather than at
 * an arbitrary point mid-scanout. */
typedef struct {
    bool     valid;
    PlayerView view;
    int      sleepMinutes;
    bool     sleepArmed;
    bool     sleepPowerOff;
    int      lidAction;
    int      gain;
    int      setupFocus;
    int      testFocus;
    int      colorFocus;
    int      themeFocus;
    /* And the meter picker's, which was missing for exactly as long as
     * it existed. Moving the cursor changed the style - the top screen
     * showed it - but nothing here saw a difference, so the bottom screen
     * was never marked dirty and the selection ring never moved. It read
     * as the page not answering the d-pad at all, which is the same
     * symptom the scroll offsets below caused before they were added. */
    int      meterFocus;
    int      theme;
    int      accent;
    /* Scroll offsets belong here as much as the focus does. They were
     * missing, and the effect was that dragging a settings page changed
     * the offset while nothing on screen moved: the comparison saw no
     * difference, so the frame was never marked dirty and the page was
     * never redrawn. It looked exactly like touch scrolling not working,
     * which is why it survived two attempts to fix the touch code. */
    int      setupScroll, testScroll;
    uint32_t uiRevision;
    /* Whole seconds of playback position.
     *
     * The transport used to be marked dirty by a timer running four
     * times a second, which is fine for a clock ticking forward but not
     * for a seek: the position changed instantly and the bar waited up
     * to a quarter of a second to show it. Position in the snapshot
     * means any change redraws on the frame it happens, and the timer is
     * no longer needed at all. */
    uint32_t elapsedSec;
    bool     modalOpen;
    uint32_t testTick;
    uint32_t marqueeTick;
    int      feedback;
    uint32_t sleepRemainingSec;
    char     cwd[FB_PATH_LEN];
    int      selected, scrollTop, count;
    bool     truncated;
    char     currentName[FB_NAME_LEN];
    char     displayName[FB_NAME_LEN];
    bool     hasTrack, isPlaying;
    int      volume;
    uint32_t totalMs;
    bool     durationExact;
    uint32_t sampleRate, bitrateKbps;
    int      channels;
    bool     vbr;
    int      queuePos, queueLen;
} UiSnapshot;

/* Files and Faves are the same browser, pointed at different folders.
 *
 * Everything the browser does - the d-pad, the marquee, tapping a row,
 * drawing the list - applies to both, so they are asked about by what
 * they are rather than by name. Written out as two comparisons in eight
 * places, one of them would eventually be missed and the Faves tab would
 * work in seven ways and not the eighth. */
static inline bool IsBrowserView(PlayerView v)
{
    return v == VIEW_FILES || v == VIEW_FAVES;
}

static void CaptureSnapshot(UiSnapshot *s, const PlayerState *player,
                            const FileBrowser *fb, const Settings *cfg,
                            int setupFocus)
{
    s->valid       = true;
    s->view        = player->view;
    s->sleepMinutes = player->sleepMinutes;
    s->sleepArmed   = player->sleepArmed;
    s->sleepPowerOff = player->sleepPowerOff;
    s->lidAction     = cfg->lidAction;
    s->gain          = cfg->gain;
    s->setupFocus    = setupFocus;
    s->testFocus     = s_testFocus;
    s->colorFocus    = s_colorFocus;
    s->themeFocus    = s_themeFocus;
    s->meterFocus    = s_meterFocus;
    s->theme         = Skin_Theme();
    s->accent        = Skin_Accent();
    s->setupScroll   = s_setupScroll;
    s->testScroll    = s_testScroll;
    s->uiRevision    = s_uiRevision;
    s->elapsedSec    = Mp3Decoder_ElapsedMs() / 1000;
    s->modalOpen     = Keyboard_IsOpen();
    /* Only when something is actually mid-sweep, so a page with names
     * that all fit is still only redrawn when it changes. */
    s->marqueeTick   = ((IsBrowserView(player->view) && Skin_MarqueeActive()) ||
                        (player->view == VIEW_NOWPLAYING && Skin_NowPlayingScrolling()))
                     ? Skin_MarqueeTick() : 0;
    /* The diagnostics numbers move every frame, so on that page - and
     * only on that page - the snapshot carries a counter that forces a
     * repaint at a readable rate rather than sixty times a second. */
    s->testTick      = (player->view == VIEW_TEST) ? (s_diagTick / 10) : 0;
    s->feedback      = Skin_FeedbackState();
    /* Only the whole-second value takes part in the dirty check, so the
     * countdown repaints once a second rather than every frame. */
    s->sleepRemainingSec = Player_SleepRemainingSec(player);
    strncpy(s->cwd, fb->cwd, sizeof(s->cwd) - 1);
    s->cwd[sizeof(s->cwd) - 1] = '\0';
    s->selected    = fb->selected;
    s->scrollTop   = fb->scrollTop;
    s->count       = fb->count;
    s->truncated   = fb->truncated;
    strncpy(s->currentName, player->currentName, sizeof(s->currentName) - 1);
    s->currentName[sizeof(s->currentName) - 1] = '\0';
    strncpy(s->displayName, player->displayName, sizeof(s->displayName) - 1);
    s->displayName[sizeof(s->displayName) - 1] = '\0';
    s->hasTrack    = player->hasTrack;
    s->isPlaying   = player->isPlaying;
    s->volume      = player->volume;
    s->totalMs     = Mp3Decoder_TotalMs();
    s->durationExact = Mp3Decoder_DurationIsExact();
    s->sampleRate  = Mp3Decoder_SampleRate();
    s->bitrateKbps = Mp3Decoder_BitrateKbps();
    s->channels    = Mp3Decoder_Channels();
    s->vbr         = Mp3Decoder_IsVbr();
    s->queuePos    = Player_QueuePosition();
    s->queueLen    = Player_QueueLength();
}

/* Which horizontal band of the skin a change belongs to. The bands do not
 * overlap - chrome is rows 0..34, content 35..142, transport 143..191 -
 * so each can be recomposed on its own, and the rows the others still
 * hold in the backbuffer stay valid. */
#define REGION_CHROME    (1u << 0)
#define REGION_CONTENT   (1u << 1)
#define REGION_TRANSPORT (1u << 2)
/* Set only when the page itself changes. The file list keeps a per-row
 * cache and repaints its own background, so handing it a fresh backdrop
 * on every data change would throw that cache away for nothing. */
#define REGION_PAGE      (1u << 3)
#define REGION_ALL       (REGION_CHROME | REGION_CONTENT | REGION_TRANSPORT)

/* Returns the set of regions that need recomposing.
 *
 * This is the whole point of the exercise: holding the d-pad in the file
 * list changes `selected` and `scrollTop` and nothing else, so it should
 * repaint the list and leave the titlebar, tabs and transport bar alone. */
static unsigned SnapshotRegions(const UiSnapshot *a, const UiSnapshot *b)
{
    if (!a->valid || !b->valid) return REGION_ALL | REGION_PAGE;

    /* A view change moves the tab highlight and swaps the page under it. */
    if (a->view != b->view) return REGION_ALL | REGION_PAGE;

    unsigned r = 0;


    /* A settings change repaints everything, not just the page it was
     * made on. Theme, accent and font reach the titlebar and the
     * transport bar as much as the content - and this counter lived in
     * the content block, so changing the look left the top and bottom
     * strips in the old palette until something else happened to redraw
     * them. Settings changes are rare; a full repaint is the cheap and
     * obviously-correct answer. */
    if (a->uiRevision != b->uiRevision || a->modalOpen != b->modalOpen)
        r |= REGION_ALL;

    /* Press feedback can be on a control in any band, and the pressed
     * control is not identified here, so redraw all three. Taps are rare
     * and brief; scrolling is what has to stay cheap. */
    if (a->feedback != b->feedback) r |= REGION_ALL;

    if (a->sleepMinutes != b->sleepMinutes ||
        a->sleepArmed != b->sleepArmed ||
        a->sleepPowerOff != b->sleepPowerOff ||
        a->lidAction != b->lidAction ||
        a->gain != b->gain ||
        a->setupFocus != b->setupFocus ||
        a->themeFocus != b->themeFocus ||
        a->theme != b->theme ||
        a->colorFocus != b->colorFocus ||
        a->meterFocus != b->meterFocus ||
        a->accent != b->accent ||
        a->testFocus != b->testFocus ||
        a->setupScroll != b->setupScroll ||
        a->testScroll != b->testScroll ||
        a->marqueeTick != b->marqueeTick ||
        a->testTick != b->testTick ||
        a->sleepRemainingSec != b->sleepRemainingSec ||
        strcmp(a->cwd, b->cwd) != 0 ||
        a->selected != b->selected ||
        a->scrollTop != b->scrollTop ||
        a->count != b->count ||
        a->truncated != b->truncated ||
        a->sampleRate != b->sampleRate ||
        a->bitrateKbps != b->bitrateKbps ||
        a->channels != b->channels ||
        a->vbr != b->vbr ||
        a->queuePos != b->queuePos ||
        a->queueLen != b->queueLen)
        r |= REGION_CONTENT;

    if (a->volume != b->volume ||
        a->totalMs != b->totalMs ||
        a->elapsedSec != b->elapsedSec ||
        a->durationExact != b->durationExact)
        r |= REGION_TRANSPORT;

    /* Shown in both bands: the list marks the playing file, the transport
     * bar draws the play/pause glyph. */
    if (strcmp(a->currentName, b->currentName) != 0 ||
        strcmp(a->displayName, b->displayName) != 0 ||
        a->hasTrack != b->hasTrack ||
        a->isPlaying != b->isPlaying)
        r |= REGION_CONTENT | REGION_TRANSPORT;

    return r;
}


/* Copies anything worth remembering out of the live state, and lets the
 * debounce in settings.c decide when it is worth a write. */
static void SyncSettings(Settings *cfg, const PlayerState *player)
{
    /* The skin owns the accent - the picker calls Skin_SetAccent from two
     * places, one of which has no config to write to - so it is copied
     * back here rather than at each of them. Compared first, so an
     * unchanged colour does not mark the file dirty every frame. */
    /* The skin owns both of these. The theme picker and the colour
     * picker each set them from two places - a tap and a key press - and
     * neither of those has a config to write to, so they are copied back
     * here instead of at four call sites. Compared first, so an unchanged
     * look does not mark the file dirty every frame.
     *
     * The theme was missing from this: the picker applied it and nothing
     * ever wrote it to cfg, so it looked right until the next start and
     * then came back as whatever had been saved before. */
    if (cfg->theme != Skin_Theme()) {
        cfg->theme = Skin_Theme();
        Settings_MarkDirty();
    }
    if (cfg->accent != Skin_Accent()) {
        cfg->accent = Skin_Accent();
        Settings_MarkDirty();
    }
    /* And the top screen, for the same reason: its picker applies the
     * choice directly so the screen shows it as the cursor moves, and
     * without this the file would never learn about it. */
    if (cfg->topChoice != s_topChoice || cfg->topChosen != s_topChosen) {
        cfg->topChoice = s_topChoice;
        cfg->topChosen = s_topChosen;
        Settings_MarkDirty();
    }
    /* And the sound preset, which its page applies as the cursor moves
     * for the same reason: it is chosen by ear. */
    if (cfg->tonePreset != s_tonePreset) {
        cfg->tonePreset = s_tonePreset;
        Settings_MarkDirty();
    }
    /* And the font. Its page applies the face as the cursor moves so it
     * can be read while it is being chosen, and nothing wrote it here -
     * so it looked right until the next start and then came back as
     * whatever had been saved before, which is exactly what the theme
     * used to do. */
    if (cfg->fontStyle != Gfx_FontStyle()) {
        cfg->fontStyle = Gfx_FontStyle();
        Settings_MarkDirty();
    }

    if (cfg->volume == player->volume &&
        cfg->sleepMinutes == player->sleepMinutes &&
        cfg->sleepPowerOff == player->sleepPowerOff)
        return;

    cfg->volume         = player->volume;
    cfg->sleepMinutes   = player->sleepMinutes;
    cfg->sleepPowerOff  = player->sleepPowerOff;
    Settings_MarkDirty();
}

static SkinTab TabForView(PlayerView view)
{
    return (view == VIEW_FILES)      ? SKIN_TAB_FILES
         : (view == VIEW_FAVES)      ? SKIN_TAB_FAVES
         : (view == VIEW_NOWPLAYING) ? SKIN_TAB_NOWPLAYING
         : (view == VIEW_SLEEP)      ? SKIN_TAB_SLEEP
         : (view == VIEW_TEST)       ? SKIN_TAB_TEST
                                     : SKIN_TAB_SETUP;
}

static void ActivateSelection(PlayerState *player, FileBrowser *fb)
{
    int sel = fb->selected;
    if (sel < 0 || sel >= fb->count) return;

    /* Inside a playlist, ".." leaves it and anything else plays from
     * that point. */
    if (fb->inPlaylist) {
        if (fb->entries[sel].isDir) FileBrowser_LeavePlaylist(fb);
        else                        Player_PlayFromBrowser(player, fb, sel);
        return;
    }

    /* A playlist is opened rather than played: opening shows what is in
     * it, and playing is then one more tap on any track. Going straight
     * to playback would leave no way to look inside one at all. */
    if (fb->entries[sel].isPlaylist) {
        FileBrowser_EnterPlaylist(fb, fb->entries[sel].name);
        return;
    }

    if (FileBrowser_EnterSelected(fb))
        Player_PlayFromBrowser(player, fb, fb->selected);
}

/* Setup row indices.
 *
 * Named, not numbered, because they are a contract between three places
 * that sit far apart in this file: the order Skin_DrawSetup draws the
 * rows in, the touch handlers that report which one was hit, and
 * ApplySetupChoice which decides what a row means. Removing the audio
 * quality row shifted every index below it, the two slider handlers kept
 * their old literals, and the result was that dragging "Extra volume"
 * changed the noise reduction while dragging "Noise reduction" did
 * nothing at all. SETUP_ROWS falls out of the enum so the d-pad arrays
 * cannot fall out of step either. */
enum {
    /* Grouped by what they are about, rather than by the order they
     * happened to be added in: what the screens look like, then what the
     * sound does, then the console itself. Diagnostics is not in the
     * group order because it is not normally there at all. */
    SETUP_ROW_TOP = 0,
    SETUP_ROW_THEME,
    SETUP_ROW_COLOR,
    SETUP_ROW_FONT,
    SETUP_ROW_SOUND,
    SETUP_ROW_GAIN,
    SETUP_ROW_JACK,
    SETUP_ROW_LID,
    SETUP_ROW_DIAG,
    SETUP_ROW_EXIT,
    SETUP_ROWS
};

/* Which setting each drawn row is.
 *
 * The rows are built into a list with Diagnostics left out until it is
 * unlocked, so a row's place in that list is not its identity - and the
 * thing that acts on a row is handed a place. Without this the first
 * press after unlocking acted on whatever had moved up into the slot. */
static unsigned char s_setupIds[SETUP_ROWS];

/* How many rows the page actually has. Seeded with the count it starts
 * with rather than zero: the d-pad reads this before the page has ever
 * been built, and a modulus by zero is not a fault anybody wants to go
 * looking for on hardware. */
static int s_setupCount = SETUP_ROWS - 1;   /* Diagnostics hidden */

/* Diagnostics is hidden until left, right, left, right is pressed on the
 * Setup page. It is a page of counters for finding out why something is
 * wrong, not a setting, and it was the one row on Setup nobody had a
 * reason to open.
 *
 * On the d-pad rather than Select, because Select means "back to what is
 * playing" on every page and taking that away from Setup cost more than
 * the unlock was worth. Left and right keep doing their own job as well;
 * the sequence is only watched for.
 *
 * s_diagSeq is how much of it has been seen: even means the next press
 * wanted is left, odd means right. */
static bool s_setupDeltaFromA = false;
static int  s_diagSeq = 0;
static bool s_diagShown = false;

/* The shoulders skip tracks, and both together is play-pause.
 *
 * A skip lands on release rather than on press: until the button comes
 * back up there is no way to know whether the other one was going to
 * join it, and firing the skip immediately would mean every attempt at
 * the combo also skipped a track on the way in. Releasing a moment later
 * is a small price for a chord that works.
 *
 * Its own function, apart from the rest of the input handling, because
 * it is the one piece that has to keep running with the lid shut.
 * Everything else on this console needs the screen - a d-pad press pages
 * a list you cannot see, a settings row changes a value you cannot read,
 * and a pocket full of stray presses should not be editing a playlist.
 * These three do not: skip, skip back, and both together for play-pause
 * are what a player needs while it is closed, and they are the controls
 * you can find by feel. So the input block stays gated on the lid and
 * this steps outside it. */
static void HandleShoulders(PlayerState *player, int held)
{
    static int  shoulders = 0;      /* what has been held this press */
    static bool comboUsed = false;

    int sh = held & (KEY_L | KEY_R);
    shoulders |= sh;

    if (sh == (KEY_L | KEY_R) && !comboUsed) {
        Skin_FlashButton(SKIN_BTN_PLAYPAUSE);
        Player_TogglePlayPause(player);
        comboUsed = true;
    }

    if (sh == 0) {
        if (!comboUsed) {
            if (shoulders & KEY_R) { Skin_FlashButtonBrief(SKIN_BTN_NEXT);
                                     s_pendingTrackStep = +1; s_pendingTrackBtn = SKIN_BTN_NEXT; }
            else if (shoulders & KEY_L) { Skin_FlashButtonBrief(SKIN_BTN_PREV);
                                     s_pendingTrackStep = -1; s_pendingTrackBtn = SKIN_BTN_PREV; }
        }
        shoulders = 0;
        comboUsed = false;
    }
}

/* Builds the Setup page's row list. Rebuilt each frame it is drawn
 * rather than kept: it is five entries of already-live values, and a
 * cached copy is one more thing that can go stale. */
static int BuildSetupRows(SkinRow *out, const Settings *cfg)
{
    _Static_assert(SETUP_ROWS <= SETTINGS_MAX_ROWS, "row buffer too small");

    memset(out, 0, SETUP_ROWS * sizeof(*out));

    static const char *const kLid[]  = { "Keep playing", "Sleep" };
    /* Every mode but Best, cheapest first. Best is the setting that made
     * high-bitrate files stutter and does not belong on the page people
     * set once and forget.
     *
     * Raw and Lite are here rather than hidden behind a diagnostic
     * because they exist to be chosen: the resampler runs on the decoder
     * thread, so picking a cheaper one is how a listener trades treble
     * for a decoder that keeps ahead of the ring. Five segments across
     * 222px is 44 each, which these labels clear comfortably. */

    out[SETUP_ROW_LID] = (SkinRow){ SKIN_ROW_CHOICE, "When the lid closes",
                                    kLid, 2, cfg->lidAction ? 1 : 0 };


    /* Also on the Sleep page, which is where it lived alone until now -
     * and where nobody found it, because that page reads as being about
     * the sleep timer. Both switches drive the same setting. */
    /* One segment, so tapping anywhere on the row opens the page. It is
     * a door rather than a setting, and the last row is where a door
     * belongs. */
    static const char *const kOpen[] = { "Open" };
    static char themeRow[40];
    snprintf(themeRow, sizeof(themeRow), "Theme: %s", Skin_ThemeName(Skin_Theme()));
    out[SETUP_ROW_THEME] = (SkinRow){ SKIN_ROW_CHOICE, themeRow, kOpen, 1, -1 };

    out[SETUP_ROW_COLOR] = (SkinRow){ SKIN_ROW_CHOICE, "Theme colour",
                                      kOpen, 1, -1 };

    /* The face in the caption, the way the theme and the top screen
     * carry theirs: the name is the one thing a row can say about a
     * typeface, and the page behind it shows the rest. */
    static char fontRow[40];
    snprintf(fontRow, sizeof(fontRow), "Font: %s",
             Gfx_FontStyleName(Gfx_FontStyle()));
    out[SETUP_ROW_FONT] = (SkinRow){ SKIN_ROW_CHOICE, fontRow, kOpen, 1, -1 };

    /* Named like the theme row, because it is the same kind of thing: a
     * door to a page of choices, with the current one on the label. */
    static char soundRow[40];
    snprintf(soundRow, sizeof(soundRow), "Sound: %s",
             kTonePresets[s_tonePreset].name);
    out[SETUP_ROW_SOUND] = (SkinRow){ SKIN_ROW_CHOICE, soundRow, kOpen, 1, -1 };

    out[SETUP_ROW_DIAG] = (SkinRow){ SKIN_ROW_CHOICE, "Diagnostics",
                                     kOpen, 1, -1 };

    /* Back to whatever launched this - the flashcart's menu, usually.
     *
     * Labelled by what will actually happen rather than by what was
     * asked for. Calico returns to a jump target if one is configured and
     * powers the console off if none is, so a cart that did not leave a
     * bootstub behind gets told that instead of being promised a menu
     * that is not there. */
    /* This row leaves the app, and only that.
     *
     * It calls pmPrepareToReset and never pmClearResetJumpTarget, which
     * is the whole difference between it and the sleep timer's power-off:
     * a jump target is what a launcher leaves behind so that exiting
     * lands back in its menu, and clearing it is what turns leaving into
     * a shutdown. This leaves it alone.
     *
     * Where it lands is not the app's to decide. Calico documents three
     * kinds of jump target - a homebrew menu's bootstub, a chainload
     * back into this same app, or GBA mode - and if none is configured
     * it powers the console off in DS mode and reboots it in DSi mode.
     * A flashcart kernel that launches a ROM directly leaves none, so
     * there is nothing to return to and no call this app can make that
     * would invent one.
     *
     * So the label says which of the three will happen rather than
     * promising a menu. Launch DSXP from a homebrew menu - TWiLight
     * Menu++, nds-hb-menu - and the first of them is what happens. */
    /* Not const-qualified, because it is written here: the destination
     * depends on how the program was launched and is not known until it
     * is asked for. The row borrows the pointer, so the array is static
     * and outlives this call. */
    static const char *kExitLabel[1];
    kExitLabel[0] = ExitDestination();
    out[SETUP_ROW_EXIT] = (SkinRow){ SKIN_ROW_CHOICE, "Exit app",
                                     kExitLabel, 1, -1 };

    /* One row, opening one page.
     *
     * There were two: a mode - meter, card, or off - and, under it, a
     * door to the meter styles. Between them they could not reach three
     * of the four cards, because the mode row had one segment for "card"
     * and no way to say which. The page behind this row draws all of
     * them, so the row only has to name what is chosen and open it. */
    static char topRow[40];
    snprintf(topRow, sizeof(topRow), "Top screen: %s", Top_ChoiceName(s_topChoice));
    out[SETUP_ROW_TOP] = (SkinRow){ SKIN_ROW_CHOICE, topRow, kOpen, 1, -1 };

    /* Boost, in whole decibels, on top of the volume.
     *
     * It has always been reachable by dragging the volume slider past
     * full, and that is still where it lives on the player - but the
     * slider gives it six unlabelled steps at the very end of its
     * travel, so there was no way to see what the boost was, or to set a
     * particular one, or to find out it existed. This row names the
     * values, and moves in the same steps the slider does, so the two
     * cannot show different numbers for one setting.
     *
     * Anything above zero can clip loud material. That is the point: it
     * hands back the headroom the resampler keeps, for a quiet recording
     * where the loudness matters more than the last dB of clean. */
    /* A slider, not seven segments.
     *
     * Seven labels across 222 pixels is 31 each, which is the narrowest
     * control on either page and the only one that does not match the
     * plain two-way switches beside it. It is a number on a range, which
     * is what the slider is for - and the value goes in the caption, the
     * way every other slider in this app carries its value. */
    static char gainLabel[40];
    if (cfg->gain > 0)
        snprintf(gainLabel, sizeof(gainLabel), "Volume boost: +%d dB", cfg->gain);
    else
        snprintf(gainLabel, sizeof(gainLabel), "Volume boost: off");
    out[SETUP_ROW_GAIN] = (SkinRow){ SKIN_ROW_SLIDER, gainLabel, NULL,
                                     VOL_GAIN_STEPS + 1,
                                     cfg->gain / VOL_GAIN_DB_PER_STEP };


    /* The caption is the live state of the socket, not a fixed label.
     * On a DS Lite it says so, which is the whole difference between a
     * setting that is missing and one that is present and inert. */
    static const char *const kJack[JACK_GUARD_MODES] = { "Off", "Pause" };
    static const char *const kJackNone[] = { "DSi only" };
    static char jackLabel[56];
    snprintf(jackLabel, sizeof(jackLabel), "Headphones: %s", Jack_StatusText());

    /* On a console without the socket pin the row is a single dead
     * segment rather than a two-way switch. There is nothing to choose,
     * and offering a choice that silently does nothing is worse than
     * saying so. */
    if (Jack_Supported())
        out[SETUP_ROW_JACK] = (SkinRow){ SKIN_ROW_CHOICE, jackLabel,
                                         kJack, JACK_GUARD_MODES,
                                         cfg->jackGuard };
    else
        /* value -1 so no segment is highlighted. Selecting the single
         * option would paint it in the accent colour, which reads as a
         * switch that is turned on - the opposite of what this row means.
         * Nothing is chosen here because there is nothing to choose. */
        out[SETUP_ROW_JACK] = (SkinRow){ SKIN_ROW_CHOICE, jackLabel,
                                         kJackNone, 1, -1 };

    for (int i = 0; i < SETUP_ROWS; i++)
        if (!out[i].caption) out[i].caption = "(unset row)";

    /* Compacted into display order, dropping Diagnostics while it is
     * hidden, and remembering which setting each place holds. */
    int n = 0;
    for (int i = 0; i < SETUP_ROWS; i++) {
        if (i == SETUP_ROW_DIAG && !s_diagShown) continue;
        if (n != i) out[n] = out[i];
        s_setupIds[n] = (unsigned char)i;
        n++;
    }
    s_setupCount = n;
    return n;
}

static void HandleButtons(PlayerState *player, FileBrowser *fb,
                          int down, int repeat, int held, int *seekCooldown,
                          int *setupFocus, int *setupDelta,
                          bool *outSeeking,
                          int *outShuffle, int *outRepeat)
{
    /* SELECT is the way home, from anywhere.
     *
     * A settings row, a full-screen picker, three folders deep in the
     * card - one press lands on the track that is playing. Every other
     * route back is a sequence: B out of a picker, START around the
     * tabs, and neither works from all of them.
     *
     * It returns rather than falling through, so the rest of the frame's
     * keys do not also act on the page being left. The two seek outputs
     * are settled first because the caller reads them either way. */
    if (down & KEY_SELECT) {
        if (player->view != VIEW_NOWPLAYING) {
            player->view = VIEW_NOWPLAYING;
            Skin_InvalidateBottom();
        }
        *outSeeking = false;
        *seekCooldown = 0;
        return;
    }

    if (IsBrowserView(player->view)) {
        if (repeat & KEY_UP)    FileBrowser_MoveSelection(fb, -1);
        if (repeat & KEY_DOWN)  FileBrowser_MoveSelection(fb, +1);
        if (repeat & KEY_LEFT)  FileBrowser_MoveSelection(fb, -FileBrowser_VisibleRows());
        if (repeat & KEY_RIGHT) FileBrowser_MoveSelection(fb, +FileBrowser_VisibleRows());

        if (down & KEY_A) ActivateSelection(player, fb);
        if (down & KEY_B) {
            /* Back means one step out of wherever you are.
             *
             * In a playlist that is the list you opened it from, which
             * for a locked browser is the Faves tab and not the folder
             * on the card.
             *
             * At the top of Faves it means nothing, and does nothing.
             * It used to leave for Files, on the reasoning that there was
             * nowhere further to go in this tab - but a tab is not
             * somewhere you climbed into, and backing out of one into
             * another reads as the app having changed its mind about
             * which page you asked for. Coming out of a list and pressing
             * B once more landed on the file manager, which is the
             * "returns to the main page" this was reported as. */
            if (fb->inPlaylist)   FileBrowser_LeavePlaylist(fb);
            else if (fb->locked)  { /* the top of Faves: nowhere to go */ }
            else                  FileBrowser_GoUp(fb);
        }
    } else if (player->view == VIEW_SLEEP) {
        /* Left/right nudge the timer by a minute, up/down by ten - the
         * slider covers two hours, so a one-minute step alone would be
         * tedious and a ten-minute step alone would be imprecise. */
        /* Left and right walk the chips, which is what the page shows;
         * up and down trim a minute at a time for the values between
         * them. It used to be one and ten against a slider that is no
         * longer there. */
        if (down & KEY_LEFT)
            Player_SetSleepMinutes(player, Skin_SleepPresetStep(player->sleepMinutes, -1));
        if (down & KEY_RIGHT)
            Player_SetSleepMinutes(player, Skin_SleepPresetStep(player->sleepMinutes, +1));
        if (repeat & KEY_DOWN)  Player_SetSleepMinutes(player, player->sleepMinutes - 1);
        if (repeat & KEY_UP)    Player_SetSleepMinutes(player, player->sleepMinutes + 1);

        if (down & KEY_A) Player_ToggleSleep(player);
        if (down & KEY_B) Player_TogglePlayPause(player);
        /* Y is "previous track" everywhere else, but on this page there is
         * no free button left and the checkbox needs a d-pad route. */
        if (down & KEY_Y) Player_ToggleSleepPowerOff(player);
    } else if (player->view == VIEW_REGS) {
        /* A rebases: takes the current values as the new baseline, so a
         * second change can be watched without leaving and coming back.
         * B returns to the page it was opened from. */
        if (down & KEY_A) s_regRebase = true;
        if (down & KEY_B) {
            player->view = VIEW_TEST;
            Skin_InvalidateBottom();
        }
    } else if (player->view == VIEW_THEMES) {
        /* The grid is laid out in the skin, so it is walked there too -
         * this page used to carry its own copy of the column count,
         * which is a second number nothing checked against the first. */
        const int wasTheme = s_themeFocus;
        int nextTheme = s_themeFocus;
        if (down & KEY_LEFT)  nextTheme = Skin_ThemePickStep(nextTheme, -1, 0);
        if (down & KEY_RIGHT) nextTheme = Skin_ThemePickStep(nextTheme, +1, 0);
        if (down & KEY_UP)    nextTheme = Skin_ThemePickStep(nextTheme, 0, -1);
        if (down & KEY_DOWN)  nextTheme = Skin_ThemePickStep(nextTheme, 0, +1);
        s_themeFocus = nextTheme;

        if (s_themeFocus != wasTheme) {
            ApplyTheme(s_themeFocus);
            s_uiRevision++;
            InvalidateTopScreen();
        }

        if (down & (KEY_A | KEY_B)) {
            if (down & KEY_B) {
                ApplyTheme(s_themeEntry);
                s_themeFocus = s_themeEntry;
                s_uiRevision++;
                InvalidateTopScreen();
            }
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
        }
    } else if (player->view == VIEW_FONTS) {
        /* Applied as the cursor moves, like the meters: a typeface is
         * judged by reading it, and the page behind this one is set in
         * whichever is selected. B puts back the one you came in with. */
        int next = s_fontFocus;
        if (down & KEY_LEFT)  next = Skin_FontPickStep(next, -1, 0);
        if (down & KEY_RIGHT) next = Skin_FontPickStep(next, +1, 0);
        if (down & KEY_UP)    next = Skin_FontPickStep(next, 0, -1);
        if (down & KEY_DOWN)  next = Skin_FontPickStep(next, 0, +1);

        if (next != s_fontFocus) {
            s_fontFocus = next;
            Gfx_SetFontStyle(next);
            s_uiRevision++;
            InvalidateTopScreen();
        }

        if (down & (KEY_A | KEY_B)) {
            if (down & KEY_B) {
                Gfx_SetFontStyle(s_fontEntry);
                s_fontFocus = s_fontEntry;
                s_uiRevision++;
                InvalidateTopScreen();
            }
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
        }
    } else if (player->view == VIEW_METERS) {
        /* Walked as a grid like the other two pickers, and the choice
         * applies as the cursor moves rather than on A: a meter is judged
         * by watching it, and the top screen is showing the real one
         * behind this page. B puts back whatever was set on the way in. */
        /* The page has two grids of different widths, so the step is
         * asked of the page rather than computed here from a modulus -
         * one of the two would be wrong, and it would be the one the
         * cursor leaves rather than the one it lands on. */
        int next = s_meterFocus;
        if (down & KEY_LEFT)  next = Skin_TopPickStep(next, -1, 0);
        if (down & KEY_RIGHT) next = Skin_TopPickStep(next, +1, 0);
        if (down & KEY_UP)    next = Skin_TopPickStep(next, 0, -1);
        if (down & KEY_DOWN)  next = Skin_TopPickStep(next, 0, +1);

        if (next != s_meterFocus) {
            s_meterFocus = next;
            s_topChosen = true;
            SetTopChoice(next);
        }

        if (down & KEY_A) {
            /* Already applied - the cursor did it. A just leaves. */
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
        } else if (down & KEY_B) {
            SetTopChoice(s_meterEntry);
            s_meterFocus = s_meterEntry;
            /* Including whether anything had been chosen at all: putting
             * the choice back and keeping the flag would leave the page
             * looking untouched while the themes had quietly stopped
             * bringing their own. */
            s_topChosen = s_topChosenEntry;
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
        }
    } else if (player->view == VIEW_SOUND) {
        /* Walked as a grid, and applied as the cursor moves rather than
         * on A - the whole point of the page is hearing the difference,
         * and a preset you have to commit to before you can hear it is a
         * page you have to visit fifteen times. B puts back whatever was
         * in force on the way in. */
        const int cols = Skin_SoundCols();
        const int rows = (TONE_PRESETS + cols - 1) / cols;
        int cx = s_soundFocus % cols, cy = s_soundFocus / cols;

        if (down & KEY_LEFT)  cx = (cx + cols - 1) % cols;
        if (down & KEY_RIGHT) cx = (cx + 1) % cols;
        if (down & KEY_UP)    cy = (cy + rows - 1) % rows;
        if (down & KEY_DOWN)  cy = (cy + 1) % rows;

        int at = cy * cols + cx;
        if (at >= TONE_PRESETS) at = TONE_PRESETS - 1;
        if (at != s_soundFocus) {
            s_soundFocus = at;
            ApplyTonePreset(at);
            s_uiRevision++;
            Skin_InvalidateBottom();
        }

        if (down & (KEY_A | KEY_B)) {
            if (down & KEY_B) {
                ApplyTonePreset(s_soundEntry);
                s_soundFocus = s_soundEntry;
            }
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
        }
    } else if (player->view == VIEW_COLORS) {
        /* A grid, so the d-pad walks it in two dimensions rather than
         * wrapping through thirty-two in a line. Left and right wrap
         * within the row; up and down wrap within the column. */
        const int wasColor = s_colorFocus;
        int nextColor = s_colorFocus;
        if (down & KEY_LEFT)  nextColor = Skin_ColorPickStep(nextColor, -1, 0);
        if (down & KEY_RIGHT) nextColor = Skin_ColorPickStep(nextColor, +1, 0);
        if (down & KEY_UP)    nextColor = Skin_ColorPickStep(nextColor, 0, -1);
        if (down & KEY_DOWN)  nextColor = Skin_ColorPickStep(nextColor, 0, +1);
        s_colorFocus = nextColor;

        /* Applied as the cursor moves, the way the meters, the fonts and
         * the sound presets are. A colour is chosen by looking at it on
         * the thing it colours, not at a swatch - and both screens are
         * behind this page showing the real one. */
        if (s_colorFocus != wasColor) {
            Skin_SetAccent(s_colorFocus);
            s_uiRevision++;
            InvalidateTopScreen();
        }

        /* A leaves it as it stands; B puts back the one you came in
         * with, which is what makes the page safe to open and look at. */
        if (down & (KEY_A | KEY_B)) {
            if (down & KEY_B) {
                Skin_SetAccent(s_colorEntry);
                s_colorFocus = s_colorEntry;
                s_uiRevision++;
                InvalidateTopScreen();
            }
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
        }
    } else if (player->view == VIEW_TEST || player->view == VIEW_SETUP) {
        /* Left, right, left, right on Setup puts Diagnostics on the page.
         *
         * Watched rather than intercepted: both keys still do what they
         * always did on a row, so the sequence costs nothing to anybody
         * not looking for it. Do it on a row whose left and right merely
         * move between two settings - the headphone switch is the
         * obvious one - and it lands back where it started. */
        if (player->view == VIEW_SETUP && !s_diagShown &&
            (down & (KEY_LEFT | KEY_RIGHT))) {
            const bool wantLeft = (s_diagSeq % 2) == 0;
            const bool got = (down & (wantLeft ? KEY_LEFT : KEY_RIGHT)) != 0;
            s_diagSeq = got ? s_diagSeq + 1 : ((down & KEY_LEFT) ? 1 : 0);
            if (s_diagSeq >= 4) {
                s_diagShown = true;
                Skin_InvalidateBottom();
            }
        }

        /* Both settings pages behave identically: up/down move the focus
         * and drag the scroll offset along with it, left and right step
         * between a setting's values, and A works the row - which for a
         * door is opening it. The row list differs; the navigation does
         * not. */
        bool test = (player->view == VIEW_TEST);

        /* The rows the page has, not the settings it could have.
         *
         * Diagnostics is left out of the list until it is unlocked, so
         * SETUP_ROWS is one more than what is drawn - and the d-pad
         * walked that extra place. It was a position with no row in it:
         * nothing highlighted, A doing nothing, one press of down to get
         * off it again, and a blank slot at the bottom of the page once
         * it had been scrolled there. The Test page has every row it
         * declares, so its count is the enum's. */
        int  rows = test ? TEST_ROWS : s_setupCount;
        if (rows < 1) rows = 1;
        int *focus = test ? &s_testFocus : setupFocus;
        int *scroll = test ? &s_testScroll : &s_setupScroll;

        if (down & KEY_UP)   *focus = (*focus + rows - 1) % rows;
        if (down & KEY_DOWN) *focus = (*focus + 1) % rows;

        /* Keep the focused row on screen. */
        if (*focus < *scroll) *scroll = *focus;
        int visible = SKIN_ROW_SLOTS;
        if (*focus >= *scroll + visible) *scroll = *focus - visible + 1;
        if (*scroll > rows - visible) *scroll = rows - visible;
        if (*scroll < 0) *scroll = 0;

        /* A and the arrows share one path to the row list, and they no
         * longer mean the same thing there: the arrows step between the
         * values of a setting, A works the row whatever it is. Which
         * key it was has to travel with the step, or excluding the
         * arrows from a door excludes A from it too - which is what
         * happened, and left the doors with no key at all. */
        if (down & (KEY_LEFT | KEY_RIGHT | KEY_A)) {
            *setupDelta = (down & KEY_LEFT) ? -1 : 1;
            s_setupDeltaFromA = (down & KEY_A) != 0;
        }

        if (down & KEY_B) {
            Skin_FlashButton(SKIN_BTN_PLAYPAUSE);
            Player_TogglePlayPause(player);
        }
    } else {
        if (repeat & KEY_UP)   Player_AdjustVolume(player, +8);
        if (repeat & KEY_DOWN) Player_AdjustVolume(player, -8);

        if (down & (KEY_A | KEY_B)) {
            Skin_FlashButton(SKIN_BTN_PLAYPAUSE);
            Player_TogglePlayPause(player);
        }
    }

    /* The tabbed pages, not every view there is.
     *
     * This cycled modulo VIEW_COUNT, which was right when the views and
     * the tabs were the same list. They are not any more: Test hangs off
     * Setup, and the colour and theme pickers are full-screen choosers
     * reached from it - so START walked into a picker with no way of
     * knowing it had, and left by cycling through the rest.
     *
     * Expressed as tabs rather than views, because About is not a view.
     * It is the support page, a modal over whatever was underneath, so a
     * list of views could not hold it and START skipped the one tab the
     * user could see sitting in the bar. Leaving it again is handled
     * where the modal takes input, since the modal owns the keypad while
     * it is up. */
    if (down & KEY_START) {
        static const SkinTab kTabs[] = {
            SKIN_TAB_FILES, SKIN_TAB_FAVES, SKIN_TAB_NOWPLAYING,
            SKIN_TAB_SLEEP, SKIN_TAB_SETUP, SKIN_TAB_ABOUT
        };
        const int n = (int)(sizeof(kTabs) / sizeof(kTabs[0]));

        const SkinTab here = TabForView(player->view);
        int at = 0;
        for (int i = 0; i < n; i++)
            if (kTabs[i] == here) { at = i; break; }

        /* Through the same calls the tab buttons use, not by assigning
         * the view.
         *
         * Files and Faves share one browser and each has to point it
         * somewhere and set or clear its lock. Set directly here, START
         * walked into Faves without repointing the browser and out of it
         * without unlocking - the same bug the tabs had, reached by the
         * other door. */
        switch (kTabs[(at + 1) % n]) {
            case SKIN_TAB_FILES:      OpenFiles(player, fb); break;
            case SKIN_TAB_FAVES:      OpenFavourites(player, fb); break;
            case SKIN_TAB_ABOUT:      Keyboard_OpenSupport(); break;
            case SKIN_TAB_SLEEP:      player->view = VIEW_SLEEP; break;
            case SKIN_TAB_SETUP:      player->view = VIEW_SETUP; break;
            default:                  player->view = VIEW_NOWPLAYING; break;
        }
    }


    /* Physical buttons flash their on-screen counterpart, so the two
     * input methods give the same feedback instead of the touchscreen
     * being the only one that visibly responds. */
    /* X and Y are next/previous everywhere - except on Now Playing,
     * where the shoulders already are, and where duplicating them wastes
     * the only two free buttons on the console.
     *
     * A button route matters here specifically: with the lid nearly shut
     * in a pocket the touchscreen is unreachable, and shuffle and repeat
     * would otherwise be the only controls that cannot be worked blind. */
    bool npButtons = (player->view == VIEW_NOWPLAYING);

    if ((down & KEY_X) && npButtons) {
        Player_SetRepeat((Player_Repeat() + 1) % REPEAT_MODES);
        Skin_FlashButton(SKIN_BTN_REPEAT);
        *outRepeat = Player_Repeat();
    } else if (down & KEY_X) {
        Skin_FlashButtonBrief(SKIN_BTN_NEXT);
        s_pendingTrackStep = +1; s_pendingTrackBtn = SKIN_BTN_NEXT;
    }

    HandleShoulders(player, held);

    /* Y is repurposed as a toggle on the two settings pages, so it must
     * not also skip a track there. */
    if ((down & KEY_Y) && npButtons) {
        Player_SetShuffle(player, !Player_Shuffle());
        Skin_FlashButton(SKIN_BTN_SHUFFLE);
        *outShuffle = Player_Shuffle() ? 1 : 0;
    } else if ((down & KEY_Y) && player->view != VIEW_SLEEP &&
               player->view != VIEW_SETUP && player->view != VIEW_TEST) {
        Skin_FlashButtonBrief(SKIN_BTN_PREV);
        s_pendingTrackStep = -1; s_pendingTrackBtn = SKIN_BTN_PREV;
    }

    /* Seek on the initial press, then at a fixed cadence while held. */
    int seekDir = 0;
    /* Seeking moved to the d-pad, and only on Now Playing.
     *
     * Left and right are spoken for everywhere else - paging the file
     * list, nudging the sleep timer, changing a settings row - and Now
     * Playing is the page you are on when you want to move through a
     * track anyway. The shoulder buttons became previous and next, which
     * is what a shoulder button does on every other music player. */
    if (player->view == VIEW_NOWPLAYING) {
        if (held & KEY_RIGHT) seekDir += 1;
        if (held & KEY_LEFT)  seekDir -= 1;
    }

    /* Whether a scrub is running is decided in the frame loop, where both
     * sources - these buttons and the progress bar - are known. */
    *outSeeking = (seekDir != 0);

    if (seekDir == 0) {
        *seekCooldown = 0;
    } else if (*seekCooldown == 0) {
        Player_SeekBy(player, seekDir * SEEK_STEP_MS);
        *seekCooldown = SEEK_REPEAT_FRAMES;
    } else {
        (*seekCooldown)--;
    }
}

/* Applies one Setup row's new value, including its side effects. */
/* Applies a palette everywhere it is cached.
 *
 * Skin_SetTheme only changes the colour table; the bottom screen caches
 * per-piece pixels and the visualizer builds its own gradient and bar
 * ramps from that table at init. Setting the theme without telling both
 * of them leaves whatever was there before.
 *
 * That shipped: startup applied the saved accent and told neither, so the
 * meter came back blue on every launch until something else happened to
 * invalidate it. The runtime path did all three calls and the startup
 * path did one, which is exactly the kind of duplication this exists to
 * remove. */
static void ApplyTheme(int theme)
{
    Skin_SetTheme(theme);
    /* Each look was drawn around a particular top screen - for some of
     * them a meter, for the quieter ones a card - so switching theme
     * brings it along.
     *
     * Only until the listener picks one, though. Handing it over every
     * time was tolerable while a theme was applied on A and no oftener;
     * now that the theme changes as the cursor moves, walking the grid
     * took the chosen top screen away a dozen times over - and put back
     * whatever the last theme touched happened to prefer. A choice made
     * on purpose outranks a default that came with a palette. */
    if (!s_topChosen) SetTopChoice(Skin_ThemeTopChoice(theme));
    InvalidateTopScreen();
    Skin_InvalidateBottom();
    InvalidateTopScreen();
}

static void ApplySetupChoice(Settings *cfg, PlayerState *player, int row, int index)
{
    /* `row` is a place in the drawn list; which setting that is depends
     * on whether Diagnostics is showing. */
    if (row >= 0 && row < s_setupCount) row = s_setupIds[row];

    (void)player;   /* no Setup row acts on the player any more */
    s_uiRevision++;

    switch (row) {
        case SETUP_ROW_LID:
            cfg->lidAction = index ? LID_SLEEP : LID_KEEP_PLAYING;
            pmSetSleepAllowed(cfg->lidAction == LID_SLEEP);
            break;
        case SETUP_ROW_THEME:
            player->view = VIEW_THEMES;
            s_themeFocus = s_themeEntry = Skin_Theme();
            Skin_InvalidateBottom();
            return;
        case SETUP_ROW_FONT:
            player->view = VIEW_FONTS;
            s_fontFocus = s_fontEntry = Gfx_FontStyle();
            Skin_InvalidateBottom();
            return;
        case SETUP_ROW_SOUND:
            s_soundFocus = s_soundEntry = s_tonePreset;
            player->view = VIEW_SOUND;
            Skin_InvalidateBottom();
            return;
        case SETUP_ROW_COLOR:
            player->view = VIEW_COLORS;
            s_colorFocus = s_colorEntry = Skin_Accent();
            Skin_InvalidateBottom();
            return;
        case SETUP_ROW_EXIT:
            /* Asked first, like every other thing here that cannot be
             * taken back.
             *
             * This used to leave on the press that selected the row -
             * one button, or one stray tap on the last row of a page
             * people open to change a theme, and the album, the queue
             * and the place in the folder were gone with no warning and
             * nothing on screen to say what had happened. Deleting a
             * playlist asks, and a playlist can be made again; this
             * cannot be undone at all once the program is out of
             * memory. The confirmation is the cheap half of that. */
            s_pending = PENDING_EXIT;
            Keyboard_OpenConfirm("Leave DSXP?", ExitDestination(),
                                 "Exit", false);
            return;
        case SETUP_ROW_DIAG:
            player->view = VIEW_TEST;
            Skin_InvalidateBottom();
            return;
        case SETUP_ROW_GAIN:
            /* Straight to the mixer as well as to the file: the whole
             * point of a boost row is hearing the difference while the
             * track is still playing. */
            cfg->gain = index * VOL_GAIN_DB_PER_STEP;
            Audio_SetGainDb(cfg->gain);
            break;
        case SETUP_ROW_TOP:
            s_openMeters = true;
            break;
        case SETUP_ROW_JACK:
            /* Forced off and unchangeable where the hardware cannot
             * report the socket, so a stale config file cannot leave the
             * setting switched on either. */
            cfg->jackGuard = Jack_Supported() ? index : JACK_GUARD_OFF;
            Jack_SetGuard(cfg->jackGuard);
            break;
        default:
            return;
    }
    Settings_MarkDirty();
}

/* Switching what the top screen shows leaves whatever the other mode
 * drew sitting in both buffers, so everything that caches per-buffer has
 * to be told to forget. */
static void InvalidateTopScreen(void);
static void InvalidateTopScreen(void)
{
    Visualizer_Invalidate();
    Skin_InvalidateTopOverlay();
    Skin_InvalidateTopCard();
}

/* The single place a diagnostics switch changes state. Both the stylus
 * and the d-pad come through here, so the two input routes cannot drift
 * apart. */
/* The single place a Test row changes state. Both the stylus and the
 * d-pad come through here, so the two input routes cannot drift apart -
 * which is exactly the bug the Setup page had. */
static void ApplyTestChoice(Settings *cfg, int row, int index)
{

    s_uiRevision++;

    switch (row) {
        case TEST_ROW_RATECUT:
            if (index < 0) index = 0;
            if (index >= RATE_CUT_STOPS) index = RATE_CUT_STOPS - 1;

            /* The filter first, then the stream, then the rebuild: the
             * resampler's ratio depends on both, so it is told once, at
             * the end, when both ends of it are settled. */
            cfg->rateCut = index;
            Mp3Decoder_SetHalfRate(kRateCutHalf[index]);
            Audio_SetOutputRate(kRateCutOut[index]);
            Mp3Decoder_RateChanged();
            break;

        case TEST_ROW_NOISE:
            cfg->noiseFilter = index;
            Mp3Decoder_SetNoiseFilter(index);
            break;

        case TEST_ROW_ENHANCE:
            cfg->enhance = index;
            Mp3Decoder_SetEnhance(index);
            break;

        case TEST_ROW_CRC:
            cfg->ignoreCrc = (index != 0);
            Mp3Decoder_SetIgnoreCrc(cfg->ignoreCrc);
            break;

        case TEST_ROW_ARTPACE:
            cfg->artPace = index;
            Mp3Decoder_SetArtPace(index);
            break;

        case TEST_ROW_ARTSCALE:
            cfg->artScale = index;
            Jpeg_SetMaxScale(kArtScaleN[index]);
            break;

        case TEST_ROW_SCROLL:
            /* Frames between repeats while a direction is held. Slower
             * is fewer repaints a second, which is less of the main
             * thread's time and less of the decoder's cache. */
            cfg->scrollRate = index;
            keysSetRepeat(15, kScrollFrames[index]);
            break;

        case TEST_ROW_RESAMPLE:
            /* Saved like the Setup row saves it, so a mode chosen here to
             * make something play is still chosen after a reboot. */
            cfg->resampleMode = (index >= 0 && index < RESAMPLE_MODES)
                                ? index : RESAMPLE_FAST;
            Mp3Decoder_SetResampleMode(cfg->resampleMode);
            break;
        case TEST_ROW_REGS:
            s_openRegs = true;
            break;
        case TEST_ROW_RESET:
            Audio_ResetCounters();
            Mp3Decoder_ResetCatchUpCount();
            s_ringLowMark = 100;
            s_uiPctPeak = 0;
            break;
        case TEST_ROW_DEFAULTS: {
            /* Straight from the same place a first run gets them, so
             * there is one list of defaults rather than two that can
             * disagree - and every one of them is applied here as well
             * as stored, because this page is used while something is
             * playing and a setting that only takes effect next boot is
             * not a reset. */
            /* Static for the same reason the browser below is: a
             * Settings now carries a 640-byte path, and this sits
             * several calls deep on a main stack that lives in DTCM
             * and is under 16KB for the whole program. It is written
             * in full by the Settings_Load on the next line, so
             * nothing carries over between presses. */
            static Settings fresh;
            Settings_Load(&fresh);

            cfg->resampleMode = fresh.resampleMode;
            cfg->rateCut      = fresh.rateCut;
            cfg->ignoreCrc    = fresh.ignoreCrc;
            cfg->scrollRate   = fresh.scrollRate;
            cfg->artScale     = fresh.artScale;
            cfg->artPace      = fresh.artPace;
            cfg->noiseFilter  = fresh.noiseFilter;
            cfg->enhance      = fresh.enhance;

            ApplyDiagSettings(cfg);
            break;
        }
        default: return;
    }
    Settings_MarkDirty();
}

static void HandleTouch(PlayerState *player, FileBrowser *fb, Settings *cfg,
                        int down, int held, const touchPosition *touch,
                        int *outSetupRow, int *outSetupIndex,
                        int *outSetupFocus)
{
    if (!(held & KEY_TOUCH)) { s_progressDrag = false; return; }

    int px = touch->px, py = touch->py;

    /* Both sliders belong to Now Playing and are checked only there.
     *
     * They used to sit in a strip along the bottom that no page drew
     * over, so testing them everywhere was harmless. They are in the
     * middle of the page now, and their track runs straight through the
     * file list - touching a row on Files was setting the volume, and a
     * row higher up was seeking. */
    const bool onPlayer = (player->view == VIEW_NOWPLAYING);

    /* Dragging the volume tracks the finger; everything else acts on the
     * initial press only, so a drag across the controls does not fire
     * three buttons on the way past. */
    int volPermille;
    if (onPlayer && Skin_HitTestVolume(px, py, &volPermille)) {
        SetVolumeLevel(player, cfg,
                       (volPermille * VOL_LEVEL_MAX + 500) / 1000);
        return;
    }

    /* The progress bar, on the same terms. It is grabbed on the press and
     * followed until release, so a drag that began on a transport button
     * cannot seek on its way past, and one that began on the bar is not
     * lost the moment the hand strays off it. */
    if (onPlayer) {
        int seekTo;
        if (down & KEY_TOUCH)
            s_progressDrag = Skin_HitTestProgress(px, py, &seekTo);

        if (s_progressDrag) {
            /* Declared before the seek, for the same reason the buttons
             * do it before theirs: it is what tells the decoder not to
             * pay for a full refill on every step of the drag. */
            s_penScrubbing = true;
            Mp3Decoder_SetScrubbing(true);
            Player_SeekToPermille(player, Skin_ProgressPermilleAt(px));
            return;
        }
    }

    /* The sleep page's two ways of setting a duration, both only while
     * that page is showing. The chips are exact values; the bar is for
     * the ones in between and tracks the finger the way the volume meter
     * does - but only while nothing is armed, because dragging a running
     * countdown would be setting the number the clock is already
     * deciding. */
    int minutes;
    if (player->view == VIEW_SLEEP) {
        if (Skin_HitTestSleepPreset(px, py, &minutes)) {
            Player_SetSleepMinutes(player, minutes);
            return;
        }
        if (!player->sleepArmed && Skin_HitTestSleepBar(px, py, &minutes)) {
            Player_SetSleepMinutes(player, minutes);
            return;
        }
    }

    /* The file list scrolls by dragging it, and by its rail. Both live
     * here with the other draggable controls so they follow the stylus
     * rather than acting once on the initial press.
     *
     * A tap still selects, and a second tap opens - so the drag has to be
     * told apart from the tap, exactly as on the settings pages: the row
     * is remembered on press and only acted on when the stylus lifts
     * without having moved. */
    if (IsBrowserView(player->view)) {
        int newTop;
        if (Skin_HitTestListScroll(px, py, fb->count, &newTop)) {
            fb->scrollTop = newTop;

            int vis = FileBrowser_VisibleRows();
            if (fb->selected < newTop) fb->selected = newTop;
            if (fb->selected > newTop + vis - 1) fb->selected = newTop + vis - 1;
            if (fb->selected >= fb->count) fb->selected = fb->count - 1;
            if (fb->selected < 0) fb->selected = 0;

            s_listDragY = -1;
            return;
        }

        int r = Skin_HitTestFileRow(px, py);

        /* A press that is not on the list does not start a drag and does
         * not get swallowed here.
         *
         * It did both, which meant that after any touch on the Files page
         * the block claimed every touch that followed - so the tabs
         * stopped responding and the only way off the page was a button.
         * The settings pages had the identical fault; this is the same
         * fix. */
        if ((down & KEY_TOUCH) && r >= 0) {
            s_listDragY = py;
            s_listDragTop = fb->scrollTop;
            s_listMoved = false;
            s_listTap = (fb->scrollTop + r < fb->count)
                      ? fb->scrollTop + r : -1;
        }

        if (s_listDragY >= 0 && (held & KEY_TOUCH)) {
            /* A list that already fits on the page cannot be scrolled, so
             * a stylus that wandered while pressing it is not a drag -
             * there is nothing to drag it to. Counting it as one threw
             * the tap away and left the row unopened, so the only way in
             * was the d-pad, and the shortest lists suffered worst: Faves
             * usually holds two or three rows, and a press that slipped
             * five pixels did nothing at all. */
            const bool scrollable = fb->count > FileBrowser_VisibleRows();

            int dy = s_listDragY - py;
            if (scrollable && (dy > DRAG_SLOP || dy < -DRAG_SLOP)) {
                s_listMoved = true;
                s_listTap = -1;
            }
            if (s_listMoved) {
                int rowH = (LIST_H_FOR_DRAG);
                int moved = (dy >= 0) ? (dy + rowH / 2) / rowH
                                      : -((-dy + rowH / 2) / rowH);
                int top = s_listDragTop + moved;
                int span = fb->count - FileBrowser_VisibleRows();
                if (span < 0) span = 0;
                if (top < 0) top = 0;
                if (top > span) top = span;
                fb->scrollTop = top;

                /* Same for the list: leave the selection behind and the
                 * next d-pad press scrolls straight back to it. */
                int sel = fb->selected;
                int vis = FileBrowser_VisibleRows();
                if (sel < top) sel = top;
                if (sel > top + vis - 1) sel = top + vis - 1;
                if (sel >= fb->count) sel = fb->count - 1;
                if (sel < 0) sel = 0;
                fb->selected = sel;
            }
            return;
        }
    }

    /* A tap on a swatch takes it and returns to Setup, which is the
     * whole interaction: there is nothing to confirm, and having chosen a
     * colour you want to see it on the page you came from. */
    if (player->view == VIEW_FONTS) {
        int hit = (down & KEY_TOUCH) ? Skin_HitTestFont(touch->px, touch->py) : -1;
        if (hit >= 0) {
            s_fontFocus = hit;
            Gfx_SetFontStyle(hit);
            s_uiRevision++;
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
            InvalidateTopScreen();
        }
        return;
    }

    if (player->view == VIEW_THEMES) {
        int hit = (down & KEY_TOUCH) ? Skin_HitTestTheme(touch->px, touch->py) : -1;
        if (hit >= 0) {
            ApplyTheme(hit);
            s_themeFocus = hit;
            s_uiRevision++;
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
            InvalidateTopScreen();
        }
        return;
    }

    /* A tap takes the preset and stays, rather than returning to Setup
     * like the swatch pages do. Choosing a sound is a thing you do by
     * ear, so the page you compare them on is the page you want to still
     * be on after each tap; B is how you leave. */
    if (player->view == VIEW_SOUND) {
        int hit = (down & KEY_TOUCH)
                ? Skin_HitTestSound(touch->px, touch->py, TONE_PRESETS) : -1;
        if (hit >= 0) {
            s_soundFocus = hit;
            ApplyTonePreset(hit);
            s_uiRevision++;
            Skin_InvalidateBottom();
        }
        return;
    }

    if (player->view == VIEW_METERS) {
        int hit = (down & KEY_TOUCH) ? Skin_HitTestMeter(touch->px, touch->py) : -1;
        if (hit >= 0) {
            s_meterFocus = hit;
            s_topChosen = true;
            SetTopChoice(hit);
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
            InvalidateTopScreen();
        }
        return;
    }

    if (player->view == VIEW_COLORS) {
        /* On the press only. This sits above the general press gate
         * because the pages below it want drags, and without the test a
         * stylus dragged onto a swatch from somewhere else would pick it. */
        int hit = (down & KEY_TOUCH) ? Skin_HitTestColor(touch->px, touch->py) : -1;
        if (hit >= 0) {
            Skin_SetAccent(hit);
            s_colorFocus = hit;
            s_uiRevision++;
            player->view = VIEW_SETUP;
            Skin_InvalidateBottom();
            InvalidateTopScreen();
        }
        return;
    }

    /* The settings rows, handled here with the other draggable controls
     * so a slider tracks a drag rather than only responding to the
     * initial touch.
     *
     * Slots are positions, rows are settings: what a slot means depends
     * on how far the page has scrolled, and that offset is added in one
     * place. */
    if (player->view == VIEW_SETUP || player->view == VIEW_TEST) {
        bool test = (player->view == VIEW_TEST);
        SkinRow rows[SETTINGS_MAX_ROWS];
        int count = test ? BuildTestRows(rows, cfg) : BuildSetupRows(rows, cfg);
        int scroll = test ? s_testScroll : s_setupScroll;

        /* Both pages now start at slot 0: the Test page's measurement
         * header is gone, and with it the reason its rows began one slot
         * down. */
        int firstSlot = 0;
        int visible = SKIN_ROW_SLOTS - firstSlot;

        /* The scroll rail first, and on held rather than only on the
         * initial press, so the thumb follows the stylus instead of
         * jumping once and stopping. */
        int newTop;
        if (Skin_HitTestRowScroll(px, py, count, visible, firstSlot, &newTop)) {
            if (test) s_testScroll = newTop;
            else      s_setupScroll = newTop;
            PullFocusIntoView(test ? &s_testFocus : outSetupFocus, newTop, visible);
            s_dragStartY = -1;
            return;
        }

        int slot = Skin_HitTestRowSlot(px, py);
        int row = (slot >= firstSlot) ? scroll + (slot - firstSlot) : -1;
        if (row >= count) row = -1;

        /* Only claim the touch if it is actually on the list, or if a
         * drag that started there is still running.
         *
         * This block used to return unconditionally, which meant that on
         * Setup and Test every touch was swallowed here - so the tabs,
         * the transport buttons and the screen-off control all stopped
         * responding on exactly the two pages you need the tabs from. */
        bool onRows = (row >= 0) || (s_dragStartY >= 0 && (held & KEY_TOUCH));
        if (!onRows) {
            if (!(held & KEY_TOUCH)) s_dragStartY = -1;
            goto not_rows;
        }

        if (down & KEY_TOUCH) {
            s_dragStartY = py;
            s_dragStartScroll = scroll;
            s_dragMoved = false;
            s_dragOnSlider = (row >= 0 && rows[row].kind == SKIN_ROW_SLIDER);
            s_tapRow = -1;

            /* Remembered, not applied: whether this press turns out to be
             * a tap or the start of a drag is not known yet. */
            if (row >= 0 && !s_dragOnSlider) {
                int index;
                SkinButton which = (SkinButton)(SKIN_BTN_ROW0 + slot);
                /* Swatches divide their row exactly as a choice strip
                 * does, so the same hit test picks them. */
                if (Skin_HitTestChoice(which, px, py, rows[row].count, &index)) {
                    s_tapRow = row;
                    s_tapIndex = index;
                }
            }
        }

        /* A slider tracks the stylus outright - that is what a slider is
         * for, and dragging one is never meant to scroll. */
        if (s_dragOnSlider && row >= 0 && rows[row].kind == SKIN_ROW_SLIDER) {
            int index;
            SkinButton which = (SkinButton)(SKIN_BTN_ROW0 + slot);
            if (Skin_HitTestSlider(which, px, py, rows[row].count - 1, &index)) {
                *outSetupRow = row;
                *outSetupIndex = index;
            }
            return;
        }

        if (s_dragStartY >= 0 && !s_dragOnSlider) {
            int dy = s_dragStartY - py;
            if (dy > DRAG_SLOP || dy < -DRAG_SLOP) {
                s_dragMoved = true;
                s_tapRow = -1;          /* it is a drag, not a tap */
            }
            if (s_dragMoved) {
                /* Rounded so the list follows the stylus rather than
                 * snapping only after a whole row of travel. */
                int moved = (dy >= 0) ? (dy + ROW_PITCH / 2) / ROW_PITCH
                                      : -((-dy + ROW_PITCH / 2) / ROW_PITCH);
                int top = s_dragStartScroll + moved;
                int span = count - visible;
                if (top < 0) top = 0;
                if (top > span) top = span;
                if (test) s_testScroll = top;
                else      s_setupScroll = top;
                PullFocusIntoView(test ? &s_testFocus : outSetupFocus, top, visible);
            }
        }
        return;
    }

not_rows:
    if (!(down & KEY_TOUCH)) return;

    /* The list controls first, and asked of the browser rather than of
     * the tab: which of them are on screen depends on what the list is
     * showing. The general test cannot know that, which is why they are
     * no longer in it. */
    SkinButton btn = SKIN_BTN_NONE;
    if (IsBrowserView(player->view))
        btn = Skin_HitTestBrowserButton(px, py, fb);
    if (btn == SKIN_BTN_NONE)
        btn = Skin_HitTestButton(px, py, TabForView(player->view));

    if (btn != SKIN_BTN_NONE) {
        Skin_FlashButton(btn);

        switch (btn) {
            case SKIN_BTN_PLAYPAUSE:      Player_TogglePlayPause(player); return;
            case SKIN_BTN_NEXT:
                s_pendingTrackStep = +1; s_pendingTrackBtn = SKIN_BTN_NEXT; return;

            /* Cycling rather than a menu: one tap is the whole
             * interaction, which is the point of them being here. */
            case SKIN_BTN_SHUFFLE:
                Player_SetShuffle(player, !Player_Shuffle());
                s_pendingShuffle = Player_Shuffle() ? 1 : 0;
                s_uiRevision++;
                return;
            case SKIN_BTN_REPEAT:
                Player_SetRepeat((Player_Repeat() + 1) % REPEAT_MODES);
                s_pendingRepeat = Player_Repeat();
                s_uiRevision++;
                return;
            case SKIN_BTN_LIKE: {
                /* The path the player is actually holding, not the name
                 * on screen: the display name may be a tag, and two
                 * tracks tagged the same are still two files. */
                const char *path = Player_CurrentPath();
                if (path) {
                    Favourites_Toggle(path);
                    s_uiRevision++;
                }
                return;
            }
            case SKIN_BTN_PREV:
                s_pendingTrackStep = -1; s_pendingTrackBtn = SKIN_BTN_PREV; return;
            case SKIN_BTN_VOL_DOWN:
                SetVolumeLevel(player, cfg, VolumeLevel(player, cfg) - 8);
                return;
            case SKIN_BTN_VOL_UP:
                SetVolumeLevel(player, cfg, VolumeLevel(player, cfg) + 8);
                return;
            case SKIN_BTN_TAB_FILES:      OpenFiles(player, fb); return;
            case SKIN_BTN_TAB_FAVES:      OpenFavourites(player, fb); return;
            case SKIN_BTN_TAB_NOWPLAYING: player->view = VIEW_NOWPLAYING; return;
            case SKIN_BTN_TAB_SLEEP:      player->view = VIEW_SLEEP; return;
            case SKIN_BTN_TAB_SETUP:      player->view = VIEW_SETUP; return;
            case SKIN_BTN_TAB_TEST:       player->view = VIEW_TEST; return;
            case SKIN_BTN_TAB_ABOUT:      Keyboard_OpenSupport(); return;

            case SKIN_BTN_MARK:
                FileBrowser_ToggleMark(fb);
                return;

            case SKIN_BTN_SAVELIST: {
                /* Only ever offered in a folder now - see BrowserButtons
                 * for why it is not inside a list. */
                if (FileBrowser_MarkedCount(fb) == 0) return;

                /* Every playlist in this folder, offered as a choice
                 * alongside making a new one.
                 *
                 * Choosing used to mean putting the cursor on the right
                 * playlist before pressing this - which works only if it
                 * happens to be in view, and is not something anyone
                 * would guess. A list of the actual options is both
                 * discoverable and the same number of taps. */
                s_pickCount = FileBrowser_ListPlaylistsIn(PLAYLIST_DIR,
                                                          s_pickStore, PICK_MAX);
                for (int i = 0; i < s_pickCount; i++) s_pickHere[i] = false;

                /* Lists that are sitting beside their music, from before
                 * every new one went to the playlist folder. Offered too,
                 * so an old list is not stranded - and skipped when the
                 * playlist folder already has one by that name, which is
                 * the same file seen twice. */
                for (int i = 0; i < fb->count && s_pickCount < PICK_MAX; i++) {
                    if (!fb->entries[i].isPlaylist) continue;

                    bool dup = false;
                    for (int j = 0; j < s_pickCount && !dup; j++)
                        dup = (strcasecmp(s_pickStore[j],
                                          fb->entries[i].name) == 0);
                    if (dup) continue;

                    snprintf(s_pickStore[s_pickCount], FB_NAME_LEN, "%s",
                             fb->entries[i].name);
                    s_pickHere[s_pickCount] = true;
                    s_pickCount++;
                }

                for (int i = 0; i < s_pickCount; i++)
                    s_pickNames[i] = s_pickStore[i];

                char msg[64];
                snprintf(msg, sizeof(msg), "Add %d track%s to",
                         FileBrowser_MarkedCount(fb),
                         FileBrowser_MarkedCount(fb) == 1 ? "" : "s");
                s_pending = PENDING_PICK;
                Keyboard_OpenPick(msg, kNewRows, NEW_ROWS,
                                  s_pickNames, s_pickCount);
                return;
            }

            case SKIN_BTN_DELLIST: {
                char msg[64];

                /* Inside a playlist the same button removes the marked
                 * tracks from it; outside, it deletes a playlist. Both
                 * are "get rid of what is selected", which is why they
                 * share the control rather than needing a fourth. */
                if (fb->inPlaylist && FileBrowser_MarkedCount(fb) > 0) {
                    s_pending = PENDING_UNLIST;
                    snprintf(msg, sizeof(msg), "Remove %d from this playlist?",
                             FileBrowser_MarkedCount(fb));
                    Keyboard_OpenConfirm(msg, fb->playlistName, "Remove", true);
                } else if (fb->inPlaylist &&
                           fb->selected > 0 && fb->selected < fb->count) {
                    /* Nothing marked, so it means the one under the
                     * cursor. Marking a single track just to remove it is
                     * two steps where one will do, and the button is
                     * otherwise dead in the common case. */
                    s_pending = PENDING_UNLIST_ONE;
                    s_pendingIndex = fb->selected;
                    Keyboard_OpenConfirm("Remove from this playlist?",
                                         fb->entries[fb->selected].name,
                                         "Remove", true);
                } else if (!fb->inPlaylist &&
                           fb->selected >= 0 && fb->selected < fb->count &&
                           fb->entries[fb->selected].isPlaylist) {
                    s_pending = PENDING_DELETE;
                    s_pendingIndex = fb->selected;
                    Keyboard_OpenConfirm("Delete this playlist?",
                                         fb->entries[fb->selected].name,
                                         "Delete", true);
                }
                return;
            }
            default: break;
        }



        /* Settings rows are not handled here: they are slots rather than
         * fixed controls, and the draggable path below owns them so a
         * slider tracks the stylus. */

        switch (btn) {
            case SKIN_BTN_SLEEP_TOGGLE:   Player_ToggleSleep(player); return;
            case SKIN_BTN_SLEEP_POWEROFF: Player_ToggleSleepPowerOff(player); return;
            default: break;
        }
    }

    if (!IsBrowserView(player->view)) return;

    /* Row taps are settled on release by the caller - see s_listTap -
     * so that dragging the list to scroll does not also select whatever
     * the drag started on. */
    (void)fb; (void)player;
}

/* The refresh rate, and the trap underneath it.
 *
 * The DS refreshes at 59.8261 Hz, not 60, so one idle frame lasts longer
 * than TICK_FREQ/60 ticks. Any budget written as TICK_FREQ/60 and compared
 * against a whole loop iteration is therefore over budget on every frame
 * forever, including frames that did nothing - which is precisely the bug
 * that kept the meter pinned at half rate from boot. The assert below is
 * here so that constant can never come back unnoticed. */
#define FRAME_PERIOD_TICKS ((uint32_t)(((uint64_t)TICK_FREQ * 10000u) / 598261u))
#define WORK_BUDGET_TICKS  (FRAME_PERIOD_TICKS - FRAME_PERIOD_TICKS / 4)

_Static_assert(FRAME_PERIOD_TICKS > (TICK_FREQ / 60),
               "an idle frame outlasts TICK_FREQ/60 - never budget frame "
               "work against it, and never measure work across the vblank wait");
_Static_assert(WORK_BUDGET_TICKS < FRAME_PERIOD_TICKS,
               "the work budget must leave headroom inside one refresh");

/* Ask for the DSi clock, and find out whether we got it.
 *
 * This is why a DSi plays AAC in its own music app and this player does
 * not: Nintendo's runs in DSi mode at 134MHz, and a game card or a DS
 * mode flashcart runs everything at the DS's 67MHz. FAAD2 needs about
 * 24960us per AAC frame, which is 117% of what a 48kHz frame lasts at
 * 67MHz and about 58% of it at 134.
 *
 * SCFG is only writable when whatever launched us left it unlocked -
 * Unlaunch and TWiLight Menu++ do, a DS mode flashcart cannot - so the
 * write is attempted and then read back rather than assumed. Locked, the
 * write is ignored and this returns false, which is the correct answer
 * and not an error.
 *
 * The timers are unaffected: calico's TICK_FREQ comes from the 33.51MHz
 * system bus, not from the CPU clock, so every budget and every
 * measurement in this program means the same thing at either speed. */
static bool RequestTwlSpeed(void)
{
    if (!systemIsTwlMode()) return false;

    REG_SCFG_CLK |= SCFG_CLK_CPU_134MHz;
    return (REG_SCFG_CLK & SCFG_CLK_CPU_134MHz) != 0;
}

int main(void)
{
    defaultExceptionHandler();

#ifdef ARM9
    /* Before anything goes deep, so what it paints is genuinely
     * untouched stack rather than whatever start-up has already used. */
    PaintMainStack();
#endif

    s_twlSpeed = RequestTwlSpeed();

    powerOn(POWER_ALL_2D);
    lcdMainOnTop();

    /* Sleep-on-lid is enabled or disabled from the saved setting once it
     * has been loaded (see below); until then, keep calico from sleeping
     * during start-up. */
    pmSetSleepAllowed(false);

    Gfx_InitFont();
    Visualizer_Init();

    InitSubScreen();
    Skin_Init(s_subBackbuf);

    Audio_Init();

    /* Decoding moves off the main thread here. Started after the mixer so
     * the worker never finds a ring that has not been set up yet. */
    Mp3Decoder_StartWorker();

    /* Static, not a local: a FileBrowser is ~33KB and the ARM9's main
     * stack lives in DTCM, which is under 16KB in total. As a local this
     * overruns the stack before the first frame is ever drawn. */
    static FileBrowser fb;
    if (!FileBrowser_Init(&fb, "/")) {
        FatalScreen(s_subBackbuf,
                    "Could not mount the card.",
                    "libfat init failed - check the flashcart.");

        /* FatalScreen holds the screen until pmMainLoop says the user
         * is leaving, and then returns - it is not noreturn, which it
         * reads as. Without this, start-up carried on past a card that
         * is not there: settings loaded from nothing, a worker started,
         * and a flush on the way out writing to a card that failed to
         * mount. There is nothing to run, so leave. */
        return 0;
    }

    PlayerState player;
    Player_Init(&player);

    /* Restore preferences before anything reads them. */
    /* Static, like the browser above: it holds a 640-byte resume path
     * now, and the ARM9's main stack is in DTCM. */
    static Settings settings;
    /* Anything an earlier build left at the root comes across first, so
     * the load below finds the settings where they now live rather than
     * starting from defaults and writing a second copy. */
    FileBrowser_MigrateAppFolder();

    Settings_Load(&settings);

    Player_SetVolume(&player, settings.volume);
    player.sleepMinutes  = settings.sleepMinutes;
    player.sleepPowerOff = settings.sleepPowerOff;
    Mp3Decoder_SetResampleMode(settings.resampleMode);
    Jack_Init();
    Jack_SetGuard(settings.jackGuard);
    Player_SetShuffle(&player, settings.shuffle);
    Player_SetRepeat(settings.repeatMode);

    /* The saved flag first: ApplyTheme below reads it to decide whether
     * the theme may bring its own top screen along, and without it a
     * deliberate choice would be overwritten on every start by whatever
     * the saved theme prefers - which is the same bug as changing theme
     * losing it, just moved to boot. */
    s_topChosen = settings.topChosen;
    SetTopChoice(settings.topChoice);
    ApplyTheme(settings.theme);
    /* After the theme, which sets the accent the theme came with - this
     * overrides it, so it has to run second or the theme would win. */
    Skin_SetAccent(settings.accent);
    Gfx_SetFontStyle(settings.fontStyle);
    Skin_SetListSize(settings.listScale);
    /* Boost is its own setting again, so a saved one is kept whatever the
     * volume is - the pass that used to zero it when the volume was
     * below full existed only because the slider had to be able to draw
     * it, and the slider no longer carries it.
     *
     * Clamped to what the row can express, and rounded onto its step, so
     * a config written by an older build lands on a value the row can
     * show rather than between two of them. */
    if (settings.gain > VOL_GAIN_MAX_DB) settings.gain = VOL_GAIN_MAX_DB;
    if (settings.gain < 0)               settings.gain = 0;
    settings.gain -= settings.gain % VOL_GAIN_DB_PER_STEP;
    Audio_SetGainDb(settings.gain);

    /* The decoder's starting settings, from the card and through the
     * same one function the diagnostics page's reset uses - so what the
     * rows say on the first frame is what is actually in force, and a
     * reset lands in exactly the state a fresh install would. */
    ApplyDiagSettings(&settings);

    /* Things that used to be switches on that page and are not any more,
     * because every time they were measured the answer was the same.
     *
     * The window table in fast RAM is bit-exact - the same numbers at a
     * faster address - so there was never a quality trade to weigh. The
     * anti-image guard only does anything when a stream is being sped
     * up, and when it is, not having it is audible. Neither is a
     * question a listener should be asked. */
    Mp3Decoder_SetFastWindow(true);
    Mp3Decoder_SetUpsampleGuard(true);
    Mp3Decoder_SetMono(false);
    Mp3Decoder_SetArtBudgetMs(0);          /* no limit */

    /* The sound preset, from the card. Flat by default, and Flat leaves
     * the tone stage switched off entirely. */
    if (settings.tonePreset < 0 || settings.tonePreset >= TONE_PRESETS)
        settings.tonePreset = 0;
    ApplyTonePreset(settings.tonePreset);

    /* The lid option is implemented by handing the hinge back to calico:
     * with sleep allowed it performs a real sleep, which is the only way
     * to get the backlight genuinely off - at the cost of stopping the
     * music, which is exactly the trade the setting describes. */
    pmSetSleepAllowed(settings.lidAction == LID_SLEEP);

    /* Pick up where the last session stopped.
     *
     * Last of the start-up work on purpose: this one opens a file and
     * seeks into it, so everything the decoder and the mixer are going
     * to be told has to be in force first, or the track would be
     * reopened under settings it was not started with.
     *
     * Failure is silent and harmless - the card may have been
     * rearranged since, in which case this is a stale path and the
     * player simply starts idle. On success the browser is left in that
     * track's folder, so the Files tab opens where the listener was
     * rather than back at the root. */
    if (Player_Resume(&player, &fb, settings.resumePath,
                      (uint32_t)settings.resumeMs) && fb.cwd[0])
        snprintf(s_filesPath, sizeof(s_filesPath), "%s", fb.cwd);

    /* ~0.25s before auto-repeat kicks in, then ~20 rows a second. The
     * previous 24/5 felt sluggish when walking a long folder. */
    /* From the same table the row reads, so the page and the keypad
     * agree on the first frame. It was a bare 3 here, which meant the
     * default above was whatever the row happened to say while the
     * d-pad repeated at a different speed entirely. */

    int seekCooldown = 0;
    int setupFocus = 0;

    /* One frame at 60Hz is ~8727 ticks. Anything past this and the loop is
     * already behind, so the next frame gives the visualizer up rather
     * than falling further behind. Never two frames running - a permanent
     * half-rate visualizer would be worse than an occasional dropped
     * one. */
    /* The DS refreshes at 59.8261 Hz, not 60.
     *
     * This mattered enormously. The budget used to be TICK_FREQ/60 = 8727
     * ticks, and it was compared against the whole loop iteration - which
     * spans swiWaitForVBlank() and is therefore, by definition, one frame
     * period. A completely idle frame measures 8753 ticks. So the test
     * "did this frame run over budget" was true on every frame, forever,
     * with the console doing nothing at all, and the meter has never once
     * run at full rate. Both the rule that skipped every other frame and
     * the rule that replaced it were reacting to a constant.
     *
     * So: measure the work, not the period, and compare it against a
     * period that is the one the hardware actually has. */
    uint32_t workTicks = 0;
    uint64_t frameStart = tickGetCount();

    bool lidClosed = false;

    /* Manual screen blanking, separate from the lid. Unlike lid-close
     * this wakes on the next button or touch, because the user is holding
     * the console and expects it back. */

    /* Frames of no input before the screens blank themselves. The lid
     * cannot be detected on every model, so this is what makes blanking
     * automatic there: shut the console, and a few seconds later it goes
     * dark on its own. Long enough not to interrupt watching the
     * visualizer, short enough to be worth having in a pocket. */

    /* Sample the hinge bit once now, while the lid is by definition open
     * - the user just launched this - and treat any departure from that
     * reading as "closed".
     *
     * Doing it this way instead of hardcoding a sense means the code
     * cannot be wrong about the polarity of EXTKEYIN bit 7, which is
     * documented inconsistently and which calico passes through
     * uninverted while inverting the buttons either side of it. It also
     * fails safe: if the bit never changes (say the ARM7 core is not
     * publishing it), lidClosed simply stays false and the screens are
     * left alone, rather than blanking at boot and never coming back. */
    scanKeys();
    const int lidOpenLevel = (keysHeld() & KEY_LID) ? 1 : 0;

    /* The start page, before anything else is on screen. It is a modal
     * like every other full-screen page, so opening it here is the whole
     * of the wiring: the loop already knows to hand input to the modal
     * and to repaint underneath it once it closes. */
    Keyboard_OpenEntry();
    UiSnapshot prevSnapshot;
    memset(&prevSnapshot, 0, sizeof(prevSnapshot));
    uint32_t frameCounter = 0;

    /* Elapsed time isn't part of the dirty comparison - at 1ms resolution
     * it would never sit still, which defeats the whole point. Instead
     * the clock/progress bar get their own deliberately coarse refresh: a
     * few times a second is imperceptibly different from every frame for
     * a number that counts in whole seconds, and costs a tiny fraction of
     * the CPU. */

    while (pmMainLoop()) {
        /* Last frame's track change, made now that last frame's highlight
         * has been drawn and presented. Opening a track blocks on the
         * card, so doing it where it was asked for meant the press could
         * not appear until the work was finished. One frame of delay puts
         * the acknowledgement first; the total wait is the same. */
        if (s_pendingTrackStep && !Skin_ButtonLit(s_pendingTrackBtn)) {
            int step = s_pendingTrackStep;
            s_pendingTrackStep = 0;
            s_pendingTrackBtn = SKIN_BTN_NONE;
            if (step > 0) Player_Next(&player);
            else          Player_Prev(&player);
        }

        scanKeys();
        int down   = keysDown();
        int repeat = keysDownRepeat();
        int held   = keysHeld();

        /* Lid handling. KEY_LID reads as held while the console is shut.
         *
         * Blanking is done with the 2D engines' master brightness rather
         * than by switching the backlight off, because on libnds 2.0 the
         * ARM9 cannot switch the backlight off at all: powerOff() ignores
         * every bit that is not PM_ARM9_DIRECT (it literally tests bit 16
         * and returns), and PM_BACKLIGHT_TOP/BOTTOM are not, so the call
         * that looks like it should do this is a no-op. The backlight
         * lives on the ARM7's SPI bus, which this CPU has no access to,
         * and calico's stock core exposes no message for it. Fading both
         * engines to black is the strongest thing an ARM9 can actually
         * do, and it is a real register write, not a silent nothing. */
        bool nowClosed = (((held & KEY_LID) ? 1 : 0) != lidOpenLevel);
        if (nowClosed != lidClosed) {
            lidClosed = nowClosed;
            setBrightness(3, lidClosed ? -16 : 0);   /* 3 = both screens */

            /* Shutting the lid with "Sleep" chosen pauses the music, and
             * opening it again leaves it paused.
             *
             * Calico sleeps the console on the hinge for that setting, so
             * the sound stopped anyway - but only because both cores were
             * halted, and it picked straight back up on the way out. That
             * makes shutting the lid a pause you cannot rely on and a
             * resume you did not ask for: put it away mid-track and it is
             * playing again the moment you look at it.
             *
             * Pausing here is deliberate on both edges. "Keep playing" is
             * untouched, because that setting says so. */
            if (lidClosed && settings.lidAction == LID_SLEEP && player.isPlaying)
                Player_TogglePlayPause(&player);
        }

        /* The shoulders keep working while the console is shut.
         *
         * The input block below is gated on the lid, which is right for
         * everything that needs the screen and wrong for the three
         * controls that do not: skip, skip back, and both together for
         * play-pause. Shut in a bag with "Keep playing" chosen, this was
         * a player with no controls at all - the music ran to the end of
         * the queue and there was no way to move within it without
         * opening the console.
         *
         * Only under "Keep playing". "Sleep" halts both cores on the
         * hinge, so nothing here runs at all under that setting, and the
         * lid is a pause rather than a mode. */
        if (lidClosed) HandleShoulders(&player, held);

        touchPosition touch;
        touchRead(&touch);

        Skin_SetHeldButton((held & KEY_TOUCH)
                           ? Skin_HitTestButton(touch.px, touch.py,
                                                TabForView(player.view))
                           : SKIN_BTN_NONE);

        /* START keeps cycling the tabs even while the support page is up,
         * because About is one of the tabs and that page is how About is
         * shown. Without this the cycle ran into About and stopped: the
         * modal owns the keypad, so the next press was never seen and the
         * only way on was to find Close.
         *
         * Only from the support page, and only START. Everything else the
         * modal is used for is a question waiting on an answer or a name
         * half typed, and neither should be walked out of by a button
         * that means "next tab".
         *
         * The press is taken out of `down` rather than jumping the rest
         * of the frame: the frame still has to pump the decoder and draw,
         * and skipping that to save one branch is how a button press
         * turns into an audible gap. Left in, it would be spent twice -
         * once leaving About and once advancing off Files. */
        if ((down & KEY_START) && Keyboard_Mode() == KB_SUPPORT) {
            Keyboard_Close();
            OpenFiles(&player, &fb);
            s_uiRevision++;
            down &= ~KEY_START;
        }

        /* And Select, which everywhere else means "back to what is
         * playing". The modal owned the keypad, so on this one page the
         * shortcut that works from every other page did nothing - and
         * this is a page of text with no reason to hold the keypad
         * against it. The same reasoning as START above; the same care
         * not to let it out of a question or a half-typed name. */
        if ((down & KEY_SELECT) && Keyboard_Mode() == KB_SUPPORT) {
            Keyboard_Close();
            player.view = VIEW_NOWPLAYING;
            Skin_InvalidateBottom();
            s_uiRevision++;
            down &= ~KEY_SELECT;
        }

        if (Keyboard_IsOpen()) {
            /* The modal owns the whole bottom screen while it is up, so
             * nothing else gets to look at this frame's input - a tap
             * meant for a key must not also reach a tab underneath it. */
            bool dirtyKb = Keyboard_Tick();
            dirtyKb |= Keyboard_HandleButtons(down);
            dirtyKb |= Keyboard_HandleTouch(touch.px, touch.py,
                                            (down & KEY_TOUCH) != 0,
                                            !(held & KEY_TOUCH));

            const char *typed;
            if (Keyboard_TakeAccepted(&typed)) {
                char saved[FB_NAME_LEN];

                if (s_pending == PENDING_SAVE) {
                    /* The playlist folder, always - and the fallback is
                     * that folder too, not wherever the browser happens
                     * to be standing.
                     *
                     * Nothing reaches here without s_saveDir set, so the
                     * old fallback was unreachable rather than wrong.
                     * But it was the one line in the program that could
                     * put a new file somewhere the user did not ask for,
                     * and the only thing keeping it from doing so was
                     * that no second caller had been added yet. */
                    const char *dir = s_saveDir[0] ? s_saveDir : PLAYLIST_DIR;
                    Favourites_Invalidate();
                    if (FileBrowser_SavePlaylistIn(&fb, dir, typed, saved, sizeof(saved))) {
                        /* Straight to Faves, landing on the new list.
                         *
                         * Every list goes to the playlist folder now, so
                         * rescanning the folder the marks came from and
                         * hunting for the file there finds nothing - and
                         * a save whose result is nowhere on screen is
                         * indistinguishable from one that failed. */
                        FileBrowser_ClearMarks(&fb);

                        /* Show it where it landed, on the tab that
                         * exists to show it - moving the browser there
                         * and leaving it unlocked left the Files tab
                         * standing in the playlist folder, which is
                         * neither where the user was nor where the list
                         * belongs. */
                        OpenFavourites(&player, &fb);
                        FileBrowser_Refresh(&fb, saved);
                    }
                    s_saveDir[0] = '\0';
                } else if (s_pending == PENDING_DELETE) {
                    /* The favourites list is one of the playlists in
                     * that folder, so anything that rewrites or deletes
                     * one may have been it. The cache is dropped rather
                     * than reasoned about - re-reading costs one file
                     * open and being wrong costs a heart that lies. */
                    Favourites_Invalidate();
                    if (FileBrowser_DeletePlaylist(&fb, s_pendingIndex))
                        FileBrowser_Refresh(&fb, NULL);
                } else if (s_pending == PENDING_PICK) {
                    int row = Keyboard_PickedRow();
                    int picked = row - NEW_ROWS;

                    if (picked < 0) {
                        /* A new list: the row says where it goes, so the
                         * only thing left to ask is what to call it. */
                        snprintf(s_saveDir, sizeof(s_saveDir), "%s",
                                 PLAYLIST_DIR);
                        s_pending = PENDING_SAVE;
                        Keyboard_OpenText("Name this playlist", "", 40);
                        dirtyKb = true;
                    } else if (picked < s_pickCount) {
                        /* Where the file is, which is the playlist folder
                         * unless this is one of the old ones sitting
                         * beside its music. */
                        const bool here = s_pickHere[picked];
                        const char *dir = here ? fb.cwd : PLAYLIST_DIR;

                        char opened[FB_NAME_LEN];
                        snprintf(opened, sizeof(opened), "%s",
                                 s_pickNames[picked]);

                        if (FileBrowser_AppendToPlaylistIn(&fb, dir, opened)) {
                            Favourites_Invalidate();
                            FileBrowser_ClearMarks(&fb);

                            /* The list is in the playlist folder and the
                             * browser is not, so opening it means going
                             * there first - the Faves tab is that folder,
                             * and it is where the list belongs anyway. */
                            if (!here) OpenFavourites(&player, &fb);

                            /* Into the list, so the additions are visible
                             * rather than taken on trust. */
                            FileBrowser_EnterPlaylist(&fb, opened);
                        }
                    }
                } else if (s_pending == PENDING_UNLIST) {
                    FileBrowser_RemoveMarkedFromPlaylist(&fb);
                } else if (s_pending == PENDING_UNLIST_ONE) {
                    if (s_pendingIndex > 0 && s_pendingIndex < fb.count) {
                        FileBrowser_ClearMarks(&fb);
                        fb.entries[s_pendingIndex].marked = true;
                        FileBrowser_RemoveMarkedFromPlaylist(&fb);
                    }
                } else if (s_pending == PENDING_EXIT) {
                    /* The jump target is deliberately left alone, which
                     * is the whole difference between leaving and the
                     * sleep timer's power-off: with one set, exiting
                     * goes back to whatever launched this.
                     *
                     * Nothing is torn down here. pmMainLoop returns
                     * false on the next pass and main() saves the
                     * position, flushes the settings and stops the
                     * player on its way out - the same path the power
                     * button takes. */
                    pmPrepareToReset();
                }

                /* The picker may have handed straight on to the naming
                 * modal, which owns the pending action from here. */
                if (!Keyboard_IsOpen()) s_pending = PENDING_NONE;
                dirtyKb = true;
            } else if (Keyboard_TakeCancelled()) {
                s_pending = PENDING_NONE;
                dirtyKb = true;
            }

            if (dirtyKb || !Keyboard_IsOpen()) s_uiRevision++;
        } else if (!lidClosed) {
            int setupDelta = 0;
            int touchedRow = -1, touchedIndex = 0;

            bool seeking = false;

            /* Scrubbing is declared before anything can seek, not after.
             *
             * It used to be set at the end of input handling, which meant
             * the first seek of a press ran while the decoder still
             * thought it was an ordinary one - so it did the full thing:
             * fseek, reset, and a prefill that filled the ring with audio
             * from the new position. The mixer runs on a higher priority
             * thread and played a slice of that before the mute landed a
             * moment later, and the whole seek was then redone on
             * release. That is the fragment heard before the silence:
             * a burst of the destination, thrown away and fetched again. */
            /* Follows the keys that now seek. Left behind on the
             * shoulders, this would arm a scrub for a track skip and
             * never arm one for an actual seek - muting the output and
             * standing the decoder down for the wrong button. */
            Mp3Decoder_SetScrubbing(
                (player.view == VIEW_NOWPLAYING &&
                 (held & (KEY_LEFT | KEY_RIGHT)) != 0) || s_penScrubbing);

            s_penScrubbing = false;
            HandleButtons(&player, &fb, down, repeat, held, &seekCooldown,
                          &setupFocus, &setupDelta,
                          &seeking, &s_pendingShuffle, &s_pendingRepeat);

            HandleTouch(&player, &fb, &settings, down, held, &touch,
                        &touchedRow, &touchedIndex, &setupFocus);

            /* Closing the scrub is safe to do here: by now both routes
             * have had their say, and ending one only sets flags. */
            if (!seeking && !s_penScrubbing) Mp3Decoder_SetScrubbing(false);

            /* A settings tap lands when the stylus lifts, not when it
             * lands: until then it might still turn into a scroll. */
            if (!(held & KEY_TOUCH)) {
                /* File list: a tap that never became a drag selects the
                 * row, and a second tap on the already-selected row opens
                 * it - the same two-step the d-pad has. */
                if (IsBrowserView(player.view) && s_listTap >= 0 && !s_listMoved) {
                    if (s_listTap == fb.selected) ActivateSelection(&player, &fb);
                    else                          FileBrowser_SetSelection(&fb, s_listTap);
                }
                s_listTap = -1;
                s_listDragY = -1;
                s_listMoved = false;

                if (s_tapRow >= 0 && !s_dragMoved) {
                    touchedRow = s_tapRow;
                    touchedIndex = s_tapIndex;
                }
                s_tapRow = -1;
                s_dragStartY = -1;
                s_dragMoved = false;
                s_dragOnSlider = false;
            }

            /* Both settings pages funnel through here. A touch names the
             * row and the value outright; the d-pad names a direction and
             * the row list says what the next value along is. Either way
             * exactly one function applies it, per page. */
            bool onTest = (player.view == VIEW_TEST);
            if (player.view == VIEW_SETUP || onTest) {
                SkinRow rows[SETTINGS_MAX_ROWS];
                int count = onTest ? BuildTestRows(rows, &settings)
                                   : BuildSetupRows(rows, &settings);
                int *focus = onTest ? &s_testFocus : &setupFocus;

                if (touchedRow >= 0 && touchedRow < count) {
                    *focus = touchedRow;
                    if (onTest) ApplyTestChoice(&settings, touchedRow, touchedIndex);
                    else        ApplySetupChoice(&settings, &player, touchedRow, touchedIndex);
                } else if (setupDelta != 0 && *focus < count &&
                           /* Left and right move between the values of a
                            * setting. A row with one value is not a
                            * setting - it is a door to a page, or a
                            * button, or a line of measurements with
                            * nothing to choose at all - and on those the
                            * arrows were acting as A. Sideways on a door
                            * does nothing now, the way sideways on a
                            * list item does; opening it is what A and a
                            * tap are for, and both still do. */
                           (s_setupDeltaFromA ||
                            !(rows[*focus].kind == SKIN_ROW_CHOICE &&
                              rows[*focus].count <= 1))) {
                    const SkinRow *r = &rows[*focus];
                    int n = r->count;

                    /* Sliders move a fraction of their range per press,
                     * not one unit. Noise reduction has 101 values, so a
                     * single step was 1% - about a third of a decibel,
                     * which is nothing you can hear. Pressing it a few
                     * times and concluding the setting does nothing is
                     * the correct reading of that behaviour. */
                    int step = 1;
                    if (r->kind == SKIN_ROW_SLIDER && n > 24) step = n / 20;

                    int next = r->value + setupDelta * step;
                    if (next < 0) next = 0;
                    if (next > n - 1) next = n - 1;
                    if (r->kind != SKIN_ROW_SLIDER)
                        next = (r->value + setupDelta + n) % n;
                    if (onTest) ApplyTestChoice(&settings, *focus, next);
                    else        ApplySetupChoice(&settings, &player, *focus, next);
                }
            }
        }

        /* Sleep timer, end-of-track detection, and the backstop pump that
         * only fires if the worker has somehow fallen behind. The decoding
         * itself is on its own thread. */
        Player_Update(&player);

        if (player.trackJustFinished) {
            player.trackJustFinished = false;
            /* Not Player_Next: a track running out and the user pressing
             * next are different events, and repeat-one distinguishes
             * them. */
            Player_TrackEnded(&player);
        }

        if (player.sleepExpired) {
            player.sleepExpired = false;
            if (player.sleepPowerOff) {
                RememberPosition(&settings, &player);
                Settings_Flush(&settings);
                RequestPowerOff(&player);
                continue;           /* pmMainLoop() ends the loop next time round */
            }

            /* Nothing further. The expiry has already paused
             * playback, and the screens are left alone.
             *
             * Blanking them used to happen here, and it is gone with the
             * rest of that feature: on a DSi it could not reach the
             * backlight, so it was a black screen over a running console
             * - something that looked like it saved power without saving
             * any - and it was one more state to have to get out of. The
             * tick below still ends the evening outright for anyone who
             * wants that. */
        }

        if (s_openMeters) {
            s_openMeters = false;
            s_meterFocus = s_topChoice;
            s_meterEntry = s_meterFocus;
            s_topChosenEntry = s_topChosen;
            player.view = VIEW_METERS;
            Skin_InvalidateBottom();
        }

        if (s_openRegs) {
            s_openRegs = false;
            s_regPrimed = false;          /* the baseline starts here */
            player.view = VIEW_REGS;
            Skin_InvalidateBottom();
        }

        if (s_regRebase) {
            s_regRebase = false;
            for (int i = 0; i < REG_WATCH_COUNT; i++) s_regBase[i] = s_regValue[i];
            Skin_InvalidateContent();
        }

        /* One register a frame while the page is up, and a repaint only
         * when a value moved. */
        if (player.view == VIEW_REGS && PollOneRegister())
            Skin_InvalidateContent();

        /* The socket, and what to do about it.
         *
         * Above the blanking short-circuit deliberately: catching the
         * headphones coming out matters most when the screen has gone
         * dark in a pocket, which is exactly when the code below would
         * otherwise have skipped this.
         *
         * A pause rather than a mute, so the transport shows a state the
         * user recognises and can undo with one button instead of silence
         * they cannot explain. */
        /* The DS mixer is opened once there is real audio to hear, and
         * closed the moment there is not.
         *
         * This is separate from pausing. Pausing stops handing audio over;
         * the mixer plays out what it already has regardless, which is the
         * few milliseconds that escaped through the speaker after the plug
         * came out, and the click at startup before the first callback had
         * filled anything. Cutting the DAC has neither problem.
         *
         * The ARM7 also does this by itself on an unplug when the guard is
         * armed, because it sees the pin a round trip before this loop
         * can. This is the path for everything else - starting, stopping,
         * plugging back in. */
        {
            static int mixerOpen = -1;
            int want = (player.isPlaying
                        && Audio_QueuedFrames() > 0) ? 1 : 0;
            if (want != mixerOpen) {
                mixerOpen = want;
                Jack_MixerMute(!want);
            }
        }


        Jack_Tick(Mp3Decoder_IsCatchingUp());

        if (Jack_Available() && player.isPlaying) {
            if (Jack_Guard() == JACK_GUARD_PAUSE && Jack_TakeUnplugged()) {
                Player_TogglePlayPause(&player);
                Skin_FlashButton(SKIN_BTN_PLAYPAUSE);
                s_uiRevision++;
            }
        }

        /* Kept above the blanking short-circuit below: a value changed
         * just before the screen went dark still has to reach the card. */

        /* Nothing here remembers where playback got to any more.
         *
         * It used to: the track and its position were written into
         * dsxp.cfg whenever the track changed or playback stopped, and
         * picked up again at start-up. One card write per track, landing
         * about three seconds into the new one - which is three seconds
         * after the decoder started a fresh file and while the ring is
         * still filling, and a card write is the single most likely
         * thing to interrupt audio on a flashcart.
         *
         * That was a real cost on every track boundary for a value read
         * exactly once, at start-up. The feature is gone rather than
         * made cheaper: the settings that matter are still saved, and
         * they are saved when they change rather than while music is
         * playing. */

        if (s_pendingShuffle >= 0) {
            settings.shuffle = (s_pendingShuffle != 0);
            s_pendingShuffle = -1;
            Settings_MarkDirty();
        }
        if (s_pendingRepeat >= 0) {
            settings.repeatMode = s_pendingRepeat;
            s_pendingRepeat = -1;
            Settings_MarkDirty();
        }

        SyncSettings(&settings, &player);
        /* Playback stopping is the signal, and the only one. Watching
         * the flag rather than patching the seven places that can clear
         * it - the pause button, the touch transport, the shoulder
         * combo, the lid, the sleep timer, the end of the queue - means
         * a new way to stop cannot be added without this noticing. */
        {
            static bool wasPlaying = false;
            if (wasPlaying && !player.isPlaying)
                RememberPosition(&settings, &player);
            wasPlaying = player.isPlaying;
        }

        Settings_Tick(&settings);

        /* Above the blanking short-circuit below, which returns to the
         * top of the loop: a flash set just before the screen went dark
         * was never ticked down and was still lit when it came back.
         *
         * Ticked by the time that passed, not once per iteration. A frame
         * that opens a track runs for a few hundred milliseconds, and the
         * highlight is on screen for every one of them - the console is
         * still showing the frame that drew it. Counting that as a single
         * tick made prev and next hold their light for the whole load and
         * then a further five frames, while play, which never blocks,
         * held it for five frames flat. Skipping the long frame outright,
         * which is what this did before, made that worse.
         *
         * Charged in refresh periods, every control lights for the same
         * length of time - apart from the stall itself, which is on
         * screen whether it is counted or not. */
        /* The frame's own cost, before anything else uses the number.
         * An eighth-weight average, because a single frame's figure
         * swings with whatever happened to repaint and says nothing. */
        {
            const unsigned long pct =
                ((unsigned long)workTicks * 100u) / FRAME_PERIOD_TICKS;
            s_uiPct = s_uiPct - (s_uiPct / 8) + (pct / 8);
            if (pct > s_uiPctPeak) s_uiPctPeak = pct;

#ifdef DSXP_COUNT_PIXELS
            /* And what the screen actually did, which the figure above
             * cannot tell you: that one is wall clock over the loop
             * body, so the mixer thread and a decoder that boosted past
             * the main thread are inside it as well. This one counts
             * only pixels written. */
            s_uiPx = s_uiPx - (s_uiPx / 8) + (g_gfxPixels / 8);
            g_gfxPixels = 0;
#endif
        }

        Skin_TickFeedback(workTicks / FRAME_PERIOD_TICKS);

        if (lidClosed) {
            /* Nothing is visible, so skip every pixel of work and just
             * keep the decoder fed. This is also what makes a blanked
             * screen genuinely cheaper on battery rather than merely
             * darker. */
            swiWaitForVBlank();
            continue;
        }

        /* From the play cursor, not from the decoder.
         *
         * The decoder runs ahead of the output by the ring's depth, so a
         * meter drawn from the newest decoded frame leads what you hear
         * by up to a second - which is exactly the lead that was
         * visible. Reading at the cursor shows the present. */
        /* One video frame's worth of audio, not two.
         *
         * At 32768Hz and 59.8261fps exactly 547.7 frames arrive between
         * one meter draw and the next, and this used to copy and scan
         * 1152 - so more than half of every frame's analysis was of
         * samples the previous frame had already analysed. Both the copy
         * out of the ring and the peak scan were a little over twice the
         * size they needed to be, every frame the meter drew.
         *
         * 544 is 32 bars x 17 samples, so the slice divides exactly and
         * the peak envelope now describes the audio that actually
         * arrived rather than a two-frame smear of it. */
        enum { VIS_ANALYSIS_FRAMES = VIS_NUM_BARS * 17 };
        static int16_t s_visPcm[VIS_ANALYSIS_FRAMES * 2];
        int frameCount = 0;
        const int16_t *frames = NULL;
        if (player.isPlaying) {
            frameCount = (int)Audio_PeekPlaying(s_visPcm, VIS_ANALYSIS_FRAMES);
            if (frameCount) frames = s_visPcm;
        }

        /* Nothing to show while scrubbing: the last decoded frame is from
         * wherever the track used to be, so holding it would freeze the
         * bars at a stale height and then snap. Feeding silence lets them
         * fall away and build back up as the new position decodes. */
        if (Mp3Decoder_IsScrubbing()) frameCount = 0;

        /* The top screen is double buffered (VRAM A/B), so redrawing it
         * in full every frame is normally the right thing.
         *
         * It is also the largest fixed cost in the loop - every pixel of
         * 256x192, every frame - and it is pure decoration. When the
         * previous frame overran its budget, this one is skipped and the
         * last image is left on screen: the visualizer drops to half rate
         * for a moment, which is close to invisible on a music
         * visualisation, and the reclaimed milliseconds go to input and
         * decoding, which are not. */
        /* Also dropped while the decoder is catching up. The visualizer is
         * the one part of the screen nobody is interacting with, and its
         * analysis and repaint are main-thread work competing with the
         * very decode that is short of time - so giving it up both speeds
         * the recovery and leaves what main does get for the half of the
         * screen the user is actually touching. */
        /* Under sustained pressure the meter halves its rate and holds
         * there, rather than dropping a frame the instant one runs long.
         * A steady every-other-frame cadence costs the decoder the same
         * time as irregular skipping but looks like a slower meter rather
         * than a stuttering one, and irregularity is what reads as lag:
         * the bars do not slow down, they hitch.
         *
         * Hysteresis either way, so one long frame does not switch it and
         * one quick frame does not switch it back.
         *
         * This is measured against workTicks - the time the frame spent
         * working - and not against the loop period. The period spans the
         * vblank wait, so it reports one refresh no matter how little work
         * the frame did, and the old comparison against TICK_FREQ/60 was
         * true on literally every frame including idle ones: the console
         * refreshes at 59.8261 Hz, so an empty frame measured 8753 ticks
         * against a budget of 8727. The meter was pinned at half rate from
         * boot and the pressure counter was pinned at its ceiling, which
         * is why halving it deliberately appeared to change so little. */
        /* A frame the decoder took time from is not a measurement of
         * how much work the screen did.
         *
         * workTicks is wall clock across the loop body - the comment on
         * the readout above says so - and a boosted worker runs at
         * WORKER_PRIO_URGENT, which is above this thread. So when the
         * ring dips and the worker preempts the UI, those frames measure
         * far over budget without the UI having done anything at all,
         * and the pressure counter read that as "the screen is too
         * expensive".
         *
         * The cost of believing it was not the boost itself - the line
         * below already halves the meter for exactly as long as the
         * worker is catching up, which is the right response and ends
         * when the boost does. It was that the pressure counter latched
         * on the same event and then had to drain all the way back to
         * zero, holding the meter at half rate for another tenth of a
         * second after the decoder had finished and the UI was idle.
         *
         * One dip, counted twice, the second one outlasting the cause.
         * At 320 kbps the ring sits near that threshold, so this is the
         * bitrate where it shows: bars that hitch about once a minute
         * while the sound stays perfect.
         *
         * Frames the worker did not touch are still measured exactly as
         * before, so a genuinely expensive screen - a cover card at
         * forty per cent - still brings the meter down. */
        {
            static uint32_t lastBoosts = 0;
            const uint32_t boosts = Mp3Decoder_CatchUpCount();

            /* Both halves matter: a boost still running at the end of
             * the frame, and one that began and finished inside it. */
            const bool preempted = Mp3Decoder_IsCatchingUp() ||
                                   boosts != lastBoosts;
            lastBoosts = boosts;

            if (!preempted) {
                if (workTicks > WORK_BUDGET_TICKS) {
                    if (s_visPressure < 8) s_visPressure++;
                } else if (s_visPressure > 0) {
                    s_visPressure--;
                }
            }
        }
        if (!s_visHalfRate && s_visPressure >= 4)      s_visHalfRate = true;
        else if (s_visHalfRate && s_visPressure == 0)  s_visHalfRate = false;

        bool skipVisualizer = !TOP_IS_METER(s_topChoice)
                            || (s_visMode == 0)
                            || (s_visMode == 1 && (frameCounter & 1))
                            || (s_visHalfRate && (frameCounter & 1))
                            /* Catch-up halves the meter. It used to stop
                             * it dead, and that was the freeze.
                             *
                             * The reasoning was that the decoder should
                             * have the CPU - but a boosted worker runs at
                             * WORKER_PRIO_URGENT, which is above the main
                             * thread, so it already takes the CPU by
                             * priority whenever it is runnable. The meter
                             * only ever ran in the slices the worker was
                             * not using, and suspending it handed the
                             * decoder almost nothing while costing the
                             * whole display.
                             *
                             * What it cost is worst on exactly the files
                             * that trigger it. Boost engages below a
                             * quarter of the ring and does not release
                             * until three eighths, so each dip freezes
                             * the bars until the decoder has rebuilt
                             * 2048 frames - 62.5ms of surplus - and at
                             * 320 kbps full stereo the ring sits near
                             * that threshold and dips every time the main
                             * thread takes work on, such as a list being
                             * scrolled. Audio stayed perfect throughout,
                             * which is why this read as "sound is fine,
                             * bars are not".
                             *
                             * Still suppressed entirely during a seek: a
                             * seek empties the ring by design, so it
                             * always triggers catch-up, and there is no
                             * audio to visualize until it refills. */
                            || (Mp3Decoder_IsCatchingUp()
                                && !Mp3Decoder_IsScrubbing()
                                && (frameCounter & 1));

        /* The buffer's low water mark, sampled every frame.
         *
         * Here rather than inside the audio module because what it
         * measures is the whole program's behaviour, not the mixer's:
         * whether anything the UI did this frame cost more than the
         * decoder had to give. */
        {
            const uint32_t pct = (Audio_QueuedFrames() * 100u) / AUDIO_RING_FRAMES;
            if (pct < s_ringLowMark) s_ringLowMark = pct;
        }

        /* The cover, handed to the skin when the decoder has a new one.
         *
         * Pulled here rather than pushed from the worker thread: the
         * decoder has no business knowing what a skin is, and this is
         * the same shape as the title and the artist, which are read the
         * same way. Only on a change, because handing it over is what
         * invalidates the card. */
        {
            static uint32_t seenArt = 0;
            uint32_t gen = Mp3Decoder_ArtGeneration();
            if (gen != seenArt) {
                seenArt = gen;
                int aw = 0, ah = 0;
                const uint16_t *px = Mp3Decoder_Art(&aw, &ah);
                Skin_SetTopArt(px, aw, ah);
            }
        }

        bool topDrew = false;

        if (TOP_IS_CARD(s_topChoice)) {
            /* Guarded by its own per-buffer signature, so this is a
             * couple of comparisons on a frame where nothing changed. */
            Visualizer_TargetBackBuffer();
            topDrew = Skin_DrawTopCard(Visualizer_BackBuffer(),
                                       s_topChoice - VIS_STYLES,
                                       /* The name of the track, not the
                                        * name of the track with the
                                        * singer in front of it: these
                                        * cards give the singer a line of
                                        * their own, so the joined form
                                        * printed them twice. */
                                       player.displayTitle,
                                       player.displayArtist,
                                       Player_NextName(),
                                       player.hasTrack,
                                       player.isPlaying,
                                       Mp3Decoder_ElapsedMs(), Mp3Decoder_TotalMs(),
                                       Mp3Decoder_SampleRate(), Mp3Decoder_BitrateKbps(),
                                       Mp3Decoder_Channels(), Mp3Decoder_IsVbr(),
                                       Player_QueuePosition(), Player_QueueLength(),
                                       Player_SleepRemainingSec(&player));
        } else if (!skipVisualizer) {
            Visualizer_Render(frames, frameCount);
            if (s_diagOverlay)
                Skin_DrawTopOverlay(Visualizer_BackBuffer(),
                                    player.displayName, player.hasTrack, player.isPlaying,
                                    Mp3Decoder_ElapsedMs(), Mp3Decoder_TotalMs(),
                                    Player_QueuePosition(), Player_QueueLength(),
                                    Player_SleepRemainingSec(&player),
                                    Audio_UnderrunFrames() + Audio_DroppedFrames(),
                                    Mp3Decoder_SampleRate(), Mp3Decoder_BitrateKbps(),
                                    Mp3Decoder_Channels(), Mp3Decoder_IsVbr(),
                                    Player_NextName());
            topDrew = true;
        }

        frameCounter++;

        UiSnapshot snapshot;
        CaptureSnapshot(&snapshot, &player, &fb, &settings, setupFocus);

        unsigned regions = SnapshotRegions(&snapshot, &prevSnapshot);

        /* A change in what is lit invalidates the page outright.
         *
         * SnapshotRegions already returns REGION_ALL for it, but that
         * only says the page may draw - each page then decides for itself
         * from its own signature, and a page whose signature does not
         * mention the pressed set will early-out and leave a highlight on
         * screen until something else moves. That is what kept repeat's
         * cover up until the next button was touched: the drawing was
         * right and the page simply never ran.
         *
         * Rather than require every page to remember to include it, the
         * one thing that is always true is stated here - if the lit set
         * changed, whatever is on screen is stale. It costs a full
         * content redraw on the press and another when it expires, which
         * is twice per tap on a page that is otherwise idle. */
        if (snapshot.feedback != prevSnapshot.feedback)
            Skin_InvalidateContent();

        /* Note what is deliberately absent: neither "the stylus is down"
         * nor "a control is showing pressed" forces a repaint any more.
         * Both used to, and both were redundant - the pressed set and
         * every value a drag can change are already in the snapshot, so
         * the comparison above catches a real change on the frame it
         * happens. Forcing on top of that just meant a slider drag
         * recomposed and re-transferred the whole screen sixty times a
         * second while the value under the stylus sat still. */
        bool dirty = regions != 0;

        /* Composing happens here, before the vblank wait, because it now
         * targets main RAM - it can take as long as it needs without the
         * display ever seeing it. Only the DMA afterwards is timing
         * sensitive. */
        /* Opening or closing the modal throws away every "this part is
         * already correct" cache.
         *
         * The modal paints over the whole bottom screen, but the skin's
         * pieces do not know that: the transport bar in particular keys
         * on volume and playback state, none of which changed while the
         * keyboard was up - so on the way back out it decided it was
         * already drawn and left the keyboard's pixels sitting where the
         * transport belongs. */
        {
            static bool wasModal = false;
            bool isModal = Keyboard_IsOpen();
            if (isModal != wasModal) {
                wasModal = isModal;
                Skin_InvalidateBottom();
            }
        }

        /* The bottom screen is never throttled.
         *
         * Four strategies were offered on the Test page and measured on
         * hardware; the honest result was that the stutter was not the
         * drawing at all - it was the noise reduction, which ran a
         * one-pole crossover and an expander per sample at 32768 Hz and
         * left the decoder nothing at 320kbps. With that gone the list
         * keeps up without help, so the help was removed too rather than
         * left as a knob nobody should have to think about. */
        if (dirty) {
            SkinTab tab = TabForView(player.view);

            Skin_BeginFrame();

            /* The modal replaces the page rather than sitting over it,
             * so none of the region machinery applies while it is up. */
            if (Keyboard_IsOpen()) {
                Keyboard_Draw();
                prevSnapshot = snapshot;
                goto composed;
            }

            /* The colour page owns the whole screen, tab bar included -
             * it is a chooser you are in and then out of, and the tabs
             * are no use while it is up. */
            if ((regions & REGION_CHROME) &&
                player.view != VIEW_COLORS && player.view != VIEW_THEMES &&
                player.view != VIEW_FONTS &&
                player.view != VIEW_METERS && player.view != VIEW_SOUND &&
                player.view != VIEW_REGS)
                Skin_DrawChrome(tab);

            /* Every page now owns its own background and keeps its own
             * "already correct" state, so all the loop has to say is when
             * the page underneath them changed. */
            /* The whole bottom, not just the content: leaving the
             * stacked player means the transport strip has to be painted
             * back in, and it only paints itself on a reset. */
            if (regions & REGION_PAGE)
                Skin_InvalidateBottom();

            /* Now Playing repaints its whole page, controls included,
             * so the two regions go out together. */
            if ((regions & REGION_CONTENT) && player.view == VIEW_NOWPLAYING)
                regions |= REGION_TRANSPORT;

            if (regions & REGION_CONTENT) {
                if (IsBrowserView(player.view)) {
                    /* The raw filename, not the drawn one - see
                     * Player_CurrentRawName for why. */
                    Skin_DrawFileList(&fb, player.hasTrack
                                           ? Player_CurrentRawName() : NULL);
                } else if (player.view == VIEW_SETUP || player.view == VIEW_TEST) {
                    /* Both settings pages are the same list widget with a
                     * different list in it. Test keeps the top slot for
                     * its measurements; Setup uses all four. */
                    bool onTest = (player.view == VIEW_TEST);
                    SkinRow rows[SETTINGS_MAX_ROWS];
                    int count = onTest ? BuildTestRows(rows, &settings)
                                       : BuildSetupRows(rows, &settings);

                    Skin_DrawRows(rows, count,
                                  onTest ? s_testScroll : s_setupScroll,
                                  onTest ? s_testFocus : setupFocus,
                                  0,
                                  SKIN_ROW_SLOTS);
                } else if (player.view == VIEW_REGS) {
                    SkinRegRow rows[REG_WATCH_COUNT];
                    bool any = false;
                    for (int i = 0; i < REG_WATCH_COUNT; i++) {
                        rows[i].name    = kRegWatch[i].name;
                        rows[i].reg     = kRegWatch[i].reg;
                        rows[i].value   = s_regValue[i];
                        rows[i].changed = s_regPrimed &&
                                          s_regValue[i] != s_regBase[i];
                        if (rows[i].changed) any = true;
                    }
                    Skin_DrawRegisters(rows, REG_WATCH_COUNT, any);
                } else if (player.view == VIEW_SOUND) {
                    static const char *names[TONE_PRESETS];
                    for (int i = 0; i < TONE_PRESETS; i++)
                        names[i] = kTonePresets[i].name;
                    Skin_DrawSound(names, TONE_PRESETS, s_soundFocus,
                                   s_tonePreset);
                } else if (player.view == VIEW_FONTS) {
                    Skin_DrawFonts(s_fontFocus);
                } else if (player.view == VIEW_THEMES) {
                    Skin_DrawThemes(s_themeFocus);
                } else if (player.view == VIEW_COLORS) {
                    Skin_DrawColors(s_colorFocus);
                } else if (player.view == VIEW_METERS) {
                    Skin_DrawMeters(s_meterFocus);
                } else if (player.view == VIEW_SLEEP) {
                    Skin_DrawSleep(player.sleepMinutes, player.sleepArmed,
                                   Player_SleepRemainingSec(&player),
                                   player.sleepPowerOff);
                } else {
                    /* Asked once per repaint rather than kept in the
                     * player: the list is cached in favourites.c, and a
                     * second copy of "is this liked" in the player would
                     * be a thing to keep in step for no gain. */
                    const char *cur = Player_CurrentPath();
                    Skin_DrawNowPlaying(player.displayTitle, player.displayArtist,
                                        player.hasTrack, player.isPlaying,
                                        Mp3Decoder_SampleRate(), Mp3Decoder_BitrateKbps(),
                                        Mp3Decoder_Channels(), Mp3Decoder_IsVbr(),
                                        Player_QueuePosition(), Player_QueueLength(),
                                        Player_NextName(),
                                        Player_Shuffle(), Player_Repeat(),
                                        Favourites_Contains(cur));
                }
            }

            if (regions & REGION_TRANSPORT)
                /* Only here. The controls belong to this page now - a
                 * transport strip that follows you onto the file list is
                 * a row of buttons for something you are not looking at,
                 * and the space is worth more to the list. */
                if (player.view == VIEW_NOWPLAYING) {
                    Skin_DrawTransport(player.isPlaying, player.hasTrack,
                                       (VolumeLevel(&player, &settings) * 1000)
                                           / VOL_LEVEL_MAX,
                                       Mp3Decoder_ElapsedMs(),
                                       Mp3Decoder_TotalMs(),
                                       Mp3Decoder_DurationIsExact());
                }

            prevSnapshot = snapshot;
composed: ;
        }

        /* Outside the dirty block, and after the drawing that used it:
         * these timers are what makes a pressed control pop back up, so
         * they have to advance on every frame. Ticking them only on
         * frames that happened to repaint was survivable while feedback
         * forced a repaint of its own; now that it does not, a flash
         * would otherwise never expire. */

        /* The marquee is time-based, so it has to advance every frame and
         * the file page has to be redrawn while it does. Only while a
         * long name is actually selected - Skin_MarqueeActive says so -
         * or the list would repaint sixty times a second for nothing. */
        /* Every frame, not only on the file page: the top screen and the
         * Now Playing page scroll long names too. */
        Skin_TickMarquee();

        /* Decoding happens on the worker thread now, so there is nothing
         * left here to schedule around: this call just checks the sleep
         * timer and whether the track ran out. The three-times-a-frame
         * pumping, and the guard that stopped a late one from overrunning
         * the vblank, both went with the work they were managing. */



        /* Everything above is this frame's work. Sampling here rather
         * than after the wait is the whole point: past this line the
         * clock is measuring the hardware's refresh, not our load. */
        workTicks = (uint32_t)(tickGetCount() - frameStart);

        swiWaitForVBlank();

        if (topDrew)
            Visualizer_Present();
        if (dirty)
            PresentSubScreen();

        /* The loop period is deliberately no longer measured. A frame
         * that comfortably makes its deadline and one that barely makes
         * it both span exactly one refresh, so as a load signal it is
         * worthless - which is how it came to read as permanently over
         * budget. workTicks above is the load. */
        frameStart = tickGetCount();
        s_diagTick++;

    }

    /* Before the flush, and before Player_Stop below clears the track
     * out from under it. This is the path both Exit and the power
     * button take, and it is the one that catches somebody who was
     * listening right up to the moment they put the console down -
     * every other save point is a stop that already happened.
     *
     * The output is silenced first, so that the rule the rest of this
     * follows holds here too: the card is never touched while there is
     * audio to interrupt. It costs the last few milliseconds of a track
     * that is about to stop anyway. */
    RememberPosition(&settings, &player);
    Audio_SetPaused(true);
    Settings_Flush(&settings);
    Player_Stop(&player);
    return 0;
}
