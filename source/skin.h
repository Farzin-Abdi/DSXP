#ifndef DSXP_SKIN_H
#define DSXP_SKIN_H

#include <stdbool.h>
#include <stdint.h>

#include "filebrowser.h"

/* Bottom-screen chrome: a file list, a transport and a segmented
 * progress meter, drawn entirely in code. Every pixel here is computed -
 * there is no sampled artwork anywhere in this program, so there is
 * nothing in it that cannot be shipped.
 *
 * The surfaces are flat, which is a performance decision as much as a
 * visual one; skin.c explains why at the palette.
 *
 * Text is rendered into the same bitmap as everything else (see gfx.c),
 * not onto a console layer: a 256x256 16-bit background already occupies
 * the whole sub-engine BG address space, so there is nowhere for a tiled
 * text layer to live. */

typedef enum {
    SKIN_BTN_NONE = -1,
    SKIN_BTN_PREV = 0,
    SKIN_BTN_PLAYPAUSE,
    SKIN_BTN_NEXT,
    SKIN_BTN_VOL_DOWN,
    SKIN_BTN_VOL_UP,
    SKIN_BTN_TAB_FILES,
    SKIN_BTN_TAB_FAVES,
    SKIN_BTN_TAB_NOWPLAYING,
    SKIN_BTN_TAB_SLEEP,
    SKIN_BTN_TAB_SETUP,
    SKIN_BTN_TAB_ABOUT,
    SKIN_BTN_SLEEP_TOGGLE,
    SKIN_BTN_SLEEP_POWEROFF,
    /* Generic row slots shared by the two scrollable settings pages.
     * A slot is a position on screen, not a setting: which setting is in
     * it depends on how far the page is scrolled. */
    SKIN_BTN_ROW0,
    SKIN_BTN_ROW1,
    SKIN_BTN_ROW2,
    SKIN_BTN_ROW3,
    SKIN_BTN_ROW4,
    SKIN_BTN_MARK,
    SKIN_BTN_SAVELIST,
    SKIN_BTN_DELLIST,
    SKIN_BTN_TAB_TEST,
    SKIN_BTN_TEST_RESET,
    SKIN_BTN_SHUFFLE,
    SKIN_BTN_REPEAT,
    SKIN_BTN_LIKE,
    SKIN_BTN_COUNT
} SkinButton;

typedef enum {
    SKIN_TAB_FILES = 0,
    SKIN_TAB_FAVES,
    SKIN_TAB_NOWPLAYING,
    SKIN_TAB_SLEEP,
    SKIN_TAB_SETUP,
    SKIN_TAB_ABOUT,
    SKIN_TAB_TEST,
} SkinTab;

/* Look: a neutral ground picked separately from the accent, so any of
 * one works with any of the other. */
/* Six complete looks, each carrying its own accent. There is no
 * separate accent list: half the pairings a free choice allows are
 * wrong, and the themes are designed as whole palettes. */
#define SKIN_THEMES  30

/* The console's own colours, offered as the accent. */
#define SKIN_ACCENTS 32

int      Skin_AccentCount(void);
uint16_t Skin_AccentColor(int i);

/* -1 restores the theme's own accent. */
void     Skin_SetAccent(int index);
int      Skin_Accent(void);

/* The colour picker: a preview of the current accent and every choice
 * below it. `focus` is the d-pad cursor, -1 when the pen is driving. */
void Skin_DrawColors(int focus);

/* Which swatch is under the pen, or -1. */
int  Skin_HitTestColor(int px, int py);

/* The theme picker: every theme as a small card of its own colours.
 * `focus` is the d-pad cursor. */
/* One watched register, as the diagnostics page shows it. */
typedef struct {
    const char *name;    /* the bus it lives on */
    uint8_t     reg;
    uint8_t     value;
    bool        changed; /* moved since the page was opened or rebased */
} SkinRegRow;

void Skin_DrawRegisters(const SkinRegRow *rows, int count, bool anyChanged);

void Skin_DrawThemes(int focus);

/* The meter picker: every style drawn as a miniature of itself, on the
 * same full screen the theme and colour pages use. Returns -1 from the
 * hit test when the tap was not on a card. */
/* The top-screen chooser: every meter, every card, and off, on one page.
 * `focus` and the return value are TOP_* / VIS_* choice numbers. */
void Skin_DrawMeters(int focus);
int  Skin_HitTestMeter(int px, int py);

/* One d-pad step across that page, in the shape the page actually has -
 * two grids of different widths, so a single modulus would walk off one
 * and arrive somewhere that is not under where it left. */
int  Skin_TopPickStep(int from, int dx, int dy);
int  Skin_HitTestTheme(int px, int py);

/* The sound page: a grid of named presets. The names come from the
 * caller, because what a preset does is audio and belongs with the audio
 * rather than with the drawing. */
void Skin_DrawSound(const char *const *names, int count, int focus, int chosen);
int  Skin_HitTestSound(int px, int py, int count);
int  Skin_SoundSlots(void);
int  Skin_SoundCols(void);

void Skin_SetTheme(int theme);
int  Skin_Theme(void);
const char *Skin_ThemeName(int i);

/* The meter each look was drawn with; applied when the theme changes. */
/* What the top screen opens on in this look: a meter or one of the
 * cards, as a TOP_* choice. Each theme was drawn around one of them, and
 * the quieter looks were drawn around a card rather than a spectrum. */
int         Skin_ThemeTopChoice(int i);

/* The live palette, for the top screen. The visualizer builds its colour
 * ramps from these so a theme reaches both screens rather than stopping
 * at the hinge. */
uint16_t Skin_ColorBg(void);
uint16_t Skin_ColorSurface(void);
uint16_t Skin_ColorAccent(void);
uint16_t Skin_ColorText(void);
uint16_t Skin_ColorHairline(void);
uint16_t Skin_ColorRaised(void);
uint16_t Skin_ColorTrough(void);
uint16_t Skin_ColorTextDim(void);

/* The ink that lands on the accent - derived when the theme's own would
 * not be readable on the colour actually in use. */
uint16_t Skin_ColorTextOnSel(void);

/* `bitmap` is the surface the skin composes into. */
void Skin_Init(uint16_t *bitmap);

/* Retargets the drawing layer. Call before any of the Draw calls below.
 *
 * The three regions are drawn separately because they change for
 * different reasons: the titlebar only on a tab change, the content on
 * whatever its page shows, the transport on playback state.
 * Recomposing all three - and transferring the whole screen - because one
 * of them moved is most of what made scrolling feel heavy. */
void Skin_BeginFrame(void);

/* Titlebar and tab strip. */
void Skin_DrawChrome(SkinTab activeTab);

/* Page switch: makes the next page draw a full one. */
void Skin_InvalidateContent(void);

/* File-list size, four steps. Bigger rows mean fewer on screen, which is
 * the trade the list can make and the other pages cannot. */
#define SKIN_LIST_SIZES 4
void Skin_SetListSize(int size);
int  Skin_ListSize(void);
int  Skin_ListRowHeight(void);

/* Throws away every "this part is already correct" cache, so the next
 * draw repaints the bottom screen in full. For anything that writes over
 * the screen behind the skin's back. */
void Skin_InvalidateBottom(void);

/* `playingName` may be NULL; when it matches a row, that row gets a
 * playing marker. */
void Skin_DrawFileList(const FileBrowser *fb, const char *playingName);

/* The sleep-timer page: a 1..120 minute slider, a "power off when it
 * expires" checkbox, and an arm/cancel button. */
void Skin_DrawSleep(int minutes, bool armed, uint32_t remainingSec,
                    bool powerOff);

/* The setup page: lid behaviour, extra volume, noise reduction.
 *
 * Audio quality is deliberately not here. The long resampling filter cost
 * enough on a 320 kbps file to push the machine past its limit, and a
 * setting whose high end is unusable is not a setting - it is a trap. The
 * short filter is now the only one the player ships with. The Test page
 * can still switch it, for measurement, without persisting the choice.
 *
 * `focus` is the row the d-pad is on (0..2), or -1 for none. */
#define SKIN_ROW_SLOTS 5

/* One row of a scrollable settings page. */
typedef enum {
    SKIN_ROW_CHOICE,
    SKIN_ROW_SLIDER,
    SKIN_ROW_SWATCH,   /* colour blocks rather than names */
} SkinRowKind;

typedef struct {
    SkinRowKind kind;
    const char *caption;             /* choice rows: the label above */
    const char *const *labels;       /* choice rows */
    int         count;               /* choice rows: segments; sliders: max+1 */
    int         value;
} SkinRow;

/* Draws up to SKIN_ROW_SLOTS rows starting at `scrollTop`, plus a
 * scrollbar when there is more than fits. `focus` is an absolute row
 * index, not a slot. */
/* `firstSlot`/`slotCount` let a page keep the top slot for something of
 * its own - the Test page puts its measurements there - while everything
 * below scrolls exactly as it does on Setup. */
void Skin_DrawRows(const SkinRow *rows, int count, int scrollTop, int focus,
                   int firstSlot, int slotCount);

/* Which slot a point falls in, or -1. Add scrollTop to get the row. */
int Skin_HitTestRowSlot(int px, int py);

/* The scroll rail. Returns the row that should become the top one, so a
 * stylus can reach the rows past the last visible slot - which otherwise
 * only the d-pad could. */
bool Skin_HitTestRowScroll(int px, int py, int count, int visible,
                           int firstSlot, int *outTop);


/* Value picked from a slider row, or false if the touch missed it. */
bool Skin_HitTestSlider(SkinButton which, int px, int py, int maxValue, int *outValue);

/* Drawn over whatever the visualizer just rendered, so the top screen
 * shows what is playing rather than being decoration only. Draws into the
 * *current* Gfx target rather than the skin's own surface. */
/* `buffer` is which of the two top-screen buffers is being drawn into.
 * The overlay owns the strip above the visualizer outright, so nothing
 * else paints over it - which means it only has to redraw when what it
 * shows actually changes, and each buffer has to be tracked separately
 * because they alternate. */
/* The top bar carries what the player page used to list underneath its
 * controls: the format of the file, and what plays next. Both are read
 * once and then ignored, which is what a second screen is for. */
void Skin_DrawTopOverlay(int buffer,
                         const char *trackName, bool hasTrack, bool isPlaying,
                         uint32_t elapsedMs, uint32_t totalMs,
                         int queuePos, int queueLen, uint32_t sleepRemainingSec,
                         uint32_t glitchFrames,
                         uint32_t sampleRate, uint32_t bitrateKbps,
                         int channels, bool vbr, const char *nextName);

/* Forces both buffers' overlays to repaint. */
void Skin_InvalidateTopOverlay(void);

/* The top screen as a still card about the current track instead of a
 * moving meter. `card` picks which of the four - see TOP_CARD in
 * visualizer.h - and they differ in what they put on the screen rather
 * than in how they are drawn:
 *
 *   0 Now playing  everything: title, format, clock, progress, queue
 *   1 Cover        the embedded picture in a fixed frame, and the names
 *   2 Cassette     a tape whose spools carry the position
 *   3 Plain        the title and the artist, and nothing else at all
 *
 * The cheapest thing the top screen can show while still saying
 * something. None of them has per-frame animation - the only thing that
 * moves is the clock, so they repaint once a second and cost literally
 * nothing on the other fifty-nine frames. Returns true if it painted, so
 * the caller knows whether the buffers need flipping. */
bool Skin_DrawTopCard(int buffer, int card,
                      const char *trackName, const char *artist,
                      const char *nextName,
                      bool hasTrack, bool isPlaying,
                      uint32_t elapsedMs, uint32_t totalMs,
                      uint32_t sampleRate, uint32_t bitrateKbps, int channels,
                      bool vbr, int queuePos, int queueLen,
                      uint32_t sleepRemainingSec);

/* Where the gramophone's deck is centred.
 *
 * Here rather than inside the card because the test that checks the
 * deck covers its ground has to look in the same place, and the two
 * drifting apart looks exactly like the bug that test exists to catch. */
#define SKIN_DECK_CX 128
#define SKIN_DECK_CY 100

/* The font picker: a page of its own, the way the themes and the top
 * screen have one. Twelve faces, each drawn in itself - a list of names
 * tells you nothing about what a typeface looks like, which is the only
 * thing anybody picks one by. */
void Skin_DrawFonts(int focus);

/* One step around each picker's grid, in the file that lays the grid
 * out. The pages used to walk the themes and the colours from a column
 * count written out again in main.c, which is a second copy of a number
 * nothing checked against the first. */
int Skin_ThemePickStep(int from, int dx, int dy);
int Skin_ColorPickStep(int from, int dx, int dy);
int  Skin_FontPickStep(int from, int dx, int dy);
int  Skin_HitTestFont(int px, int py);

/* Forces both buffers' cards to repaint. */
void Skin_InvalidateTopCard(void);

/* The current track's cover, for the card that shows one.
 *
 * A borrowed pointer: whoever decodes the picture keeps it, and hands
 * this its address and size. NULL clears it, which is what a track with
 * no picture does. The generation counts the changes, and the card's
 * signature includes it - the art arrives from the worker thread after
 * the card has already been painted once. */
void Skin_SetTopArt(const uint16_t *px, int w, int h);
const uint16_t *Skin_TopArt(int *w, int *h);
uint32_t Skin_TopArtGeneration(void);

/* `art` is the embedded picture as a thumbnail, or NULL. */
void Skin_DrawNowPlaying(const char *title, const char *artist,
                         bool hasTrack, bool isPlaying,
                         uint32_t sampleRate, uint32_t bitrateKbps, int channels,
                         bool vbr, int queuePos, int queueLen,
                         const char *nextName, bool shuffle, int repeatMode,
                         bool liked);

/* Diagnostics page. Everything on it is either a live measurement or a
 * switch that takes one specific cost out of the frame, so a lag can be
 * narrowed down to the thing causing it instead of guessed at. */


/* The player controls, which live inside the Now Playing page. No
 * other page has them: a transport strip that follows you onto the file
 * list is a strip of buttons for a thing you are not looking at. */
void Skin_DrawTransport(bool isPlaying, bool hasTrack, int volumePermille,
                        uint32_t elapsedMs, uint32_t totalMs, bool durationExact);

/* Flashes a control as pressed for a few frames after it is tapped, so a
 * touch produces visible feedback the way a real button would. */
void Skin_FlashButton(SkinButton btn);

/* Half the usual flash, for a control whose action blocks afterwards.
 * The caller waits for Skin_ButtonLit to go false before doing the work,
 * so the highlight is never on screen while the console is stalled. */
void Skin_FlashButtonBrief(SkinButton btn);

/* Whether a control is currently drawn lit. */
bool Skin_ButtonLit(SkinButton btn);
/* Advances the press highlights by `periods` refreshes.
 *
 * Takes a count rather than assuming one per call because a frame that
 * blocks on the card lasts many refreshes, and the highlight is on screen
 * for all of them - so charging it one tick made those controls hold
 * their light far longer than the ones that never block. */
void Skin_TickFeedback(unsigned periods);

/* Advances the selected file row's sideways scroll. Call once a frame. */
void Skin_TickMarquee(void);
uint32_t Skin_MarqueeTick(void);
bool Skin_MarqueeActive(void);
bool Skin_NowPlayingScrolling(void);

/* Marks the control currently under the stylus, so it stays visibly
 * pressed for as long as it is held rather than only flashing. Pass
 * SKIN_BTN_NONE on release. */
void Skin_SetHeldButton(SkinButton btn);

/* A value that changes whenever the *visible* pressed state of anything
 * changes. Callers that only repaint on change must fold this into their
 * comparison: without it, the frame where a flash expires is not dirty,
 * so the last thing drawn stays on screen and the button appears stuck
 * down forever. */
int Skin_FeedbackState(void);

/* True while any press feedback is still animating. */

/* Hit testing shares the exact rectangles the drawing uses, so touch
 * targets cannot drift away from what is on screen. */
/* `activeTab` is required, not optional: controls that live on different
 * pages are allowed to occupy the same pixels, since they are never on
 * screen together. Without knowing which page is showing, the first
 * matching rectangle wins and a control from a hidden page shadows the
 * visible one. */
SkinButton Skin_HitTestButton(int px, int py, SkinTab activeTab);

/* Which segment of a multiple-choice row was tapped. */
/* The three list controls, which the tab-gated test above cannot cover.
 *
 * Which of them are on screen depends on what the browser is showing -
 * a folder has two, a playlist has three, the Faves tab at its top
 * level has one - so they are tested from the same layout they are
 * drawn from. Ask this before the general test on any browser page. */
SkinButton Skin_HitTestBrowserButton(int px, int py, const FileBrowser *fb);

bool Skin_HitTestChoice(SkinButton which, int px, int py, int count, int *outIndex);
int  Skin_HitTestFileRow(int px, int py);            /* -1 if not on the list */

/* The file list's scroll rail. */
bool Skin_HitTestListScroll(int px, int py, int count, int *outTop);
bool Skin_HitTestVolume(int px, int py, int *outPermille);
bool Skin_HitTestProgress(int px, int py, int *outPermille);
int  Skin_ProgressPermilleAt(int px);
int  Skin_SleepPresetStep(int minutes, int dir);
bool Skin_HitTestSleepBar(int px, int py, int *outMinutes);
bool Skin_HitTestSleepPreset(int px, int py, int *outMinutes);

#endif /* DSXP_SKIN_H */
