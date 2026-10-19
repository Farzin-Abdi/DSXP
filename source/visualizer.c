#include "visualizer.h"

#include "skin.h"

#include <nds.h>
#include <string.h>

#include "gfx.h"

#define SCREEN_W GFX_W
#define SCREEN_H GFX_H

/* The top strip is covered by the now-playing overlay (see
 * Skin_DrawTopOverlay), so the effect keeps clear of it rather than
 * drawing detail that is immediately painted over. */
/* Blue Classic reserves a real header above its meter.  The other looks
 * retain the compact overlay, so this is geometry owned by the selected
 * theme rather than a one-size-fits-none screen crop. */
#define VIS_TOP_MARGIN 32   /* equals TOP_HEADER_H in skin.c */

#define NUM_BARS VIS_NUM_BARS
#define BAR_STRIDE (SCREEN_W / NUM_BARS)   /* 8px: 6px bar + 2px gutter */
#define BAR_W (BAR_STRIDE - 2)

/* Bars stand on a floor line with a dimmed mirror image below it. */
#define FLOOR_Y      (SCREEN_H - 22)
/* Shortened so the foot of the screen is left clear for the overlay's
 * "next" line. The reflection is the cheapest thing here to give up:
 * it is decoration under the bars, and the rows it loses are the
 * faintest ones at the tail of the falloff. */
#define REFLECT_H    2

/* Bar rows are written as six unrolled stores, so the width is not free
 * to change without changing that loop too. */
_Static_assert(BAR_W == 6, "bar rows are written as six unrolled stores");

/* Two full 256x256x16 buffers, one in VRAM_A and one in VRAM_B. The two
 * banks are mapped back to back at 0x06000000, so the second buffer is
 * simply 128KB (bitmap base unit 8) into the same address space.
 *
 * These are variables rather than constants so the renderer can be driven
 * against ordinary memory by the host-side preview tool. */
#define BUF1_BASE 8

static uint16_t *s_buf[2] = {
    (uint16_t *)0x06000000,
    (uint16_t *)0x06020000,
};

static int s_bgId = -1;
static int s_backBuffer = 1;      /* index of the buffer being drawn into */

static uint16_t s_bgRow[SCREEN_H];         /* backdrop gradient, one colour per row */
static uint16_t s_vuColor[SCREEN_H];       /* bar colour by height: green -> red */
static uint16_t s_waveEdge[SCREEN_H];      /* the trace, half-way to the backdrop */
static uint16_t s_reflectColor[REFLECT_H]; /* mirrored bar, pre-faded per row */

/* Each bar is topped with a brighter cap, which is most of what makes a
 * level meter read as one. */
#define CAP_ROWS 2

/* How tall the tip is in Dots. Four rather than the cap's two: with no
 * column under it a two-row mark reads as a speck rather than as a
 * level, and four still costs a tenth of what a full column does. */
#define DOT_ROWS 4
static uint16_t s_capColor[SCREEN_H];

static int s_barHeight[NUM_BARS];
static int s_peak[NUM_BARS];
static int s_peakHold[NUM_BARS];

/* What each buffer currently shows, so only the difference is painted.
 * Per buffer, because they alternate: what the back buffer holds is two
 * frames old, not one. */
static bool s_bufDrawn[2];
static int  s_drawnH[2][NUM_BARS];
static int  s_drawnPeakY[2][NUM_BARS];
static int  s_drawnRefl[2][NUM_BARS];

/* Optional instrumentation: tools/hosttest measures how much the
 * incremental path actually saves. Compiled out of the ROM entirely. */
#ifdef DSXP_COUNT_PIXELS
unsigned long g_visPixels = 0;

/* Rows written, counted separately from pixels.
 *
 * Pixels are the wrong unit for this hardware. The frame buffer is row
 * major, so a run of pixels along a row shares a cache line while a run
 * down a column touches a new one every step, 512 bytes apart. A bar is
 * rectangles and a wave is columns, so the two can have similar pixel
 * counts and quite different costs - which is how the wave came to be the
 * style that stutters while the bars do not. This counts the line
 * fetches, which is what the console actually pays. */
unsigned long g_visRows = 0;

#define COUNT_PIXELS(n) (g_visPixels += (unsigned long)(n))
#define COUNT_ROW()     (g_visRows += 1)
#else
#define COUNT_PIXELS(n) ((void)0)
#define COUNT_ROW()     ((void)0)
#endif

static void BuildTables(void);
/* The rows each column last had ink in, per buffer, so the previous
 * trace can be erased exactly. A single y per column is not enough once
 * the columns are joined up: the segment between two samples covers a
 * span, and erasing only its endpoint leaves the rest behind. */
/* Columns per trace step. Two pixels wide, so a row of the trace is one
 * cache line's worth rather than one pixel of one. */
/* Columns per trace step, and the width the trace is drawn.
 *
 * Two columns wide, but only the first is the trace: the second is the
 * same colour half-way to the backdrop.
 *
 * One pixel was a hairline and two solid pixels were a rope, and there is
 * no integer between them - so the weight comes from the colour instead.
 * A bright core with a soft edge reads as about a pixel and a half, which
 * is the width neither of the whole-pixel options could be.
 *
 * The cost is unchanged: the pair still shares one cache line, which is
 * what stepping by two bought in the first place.
 *
 * The edge columns are written by a loop rather than by separate stores,
 * so a change to the step cannot leave the writes behind - WAVE_STEP is a
 * compile-time constant and it unrolls to the same code. */
#define WAVE_STEP 2

/* The tallest a single column of the trace may be. Twenty rows is well
 * past any jump music makes between neighbouring samples, so on real
 * material this never fires; what it removes is the full-height stripe
 * that a discontinuity would otherwise draw. */
#define WAVE_MAX_JOIN 20
_Static_assert(SCREEN_W % WAVE_STEP == 0,
               "the trace must tile the screen exactly or the last column "
               "is drawn narrow and never erased");

static int16_t s_waveTop[2][SCREEN_W], s_waveBot[2][SCREEN_W];
static bool s_waveDrawn[2];

void Visualizer_Invalidate(void)
{
    /* Rebuilds the colour tables too, not just the "buffer needs
     * repainting" flags. The theme feeds those tables, and this is the
     * call every theme change already makes - so rebuilding here is what
     * makes a new palette actually reach the meter rather than only the
     * next full clear. */
    BuildTables();
    s_bufDrawn[0] = s_bufDrawn[1] = false;
    s_waveDrawn[0] = s_waveDrawn[1] = false;
}

static void BuildTables(void)
{
    /* Backdrop: near-black at the top easing into the deep blue of the
     * bottom screen's page colour, so the two halves of the console look
     * like one program. Every improvement in this function is free at
     * runtime - these are lookup tables built once at startup, and the
     * per-frame code does nothing but index them. */
    for (int y = 0; y < SCREEN_H; y++) {
        int t = (y * 255) / (SCREEN_H - 1);
        /* The theme's own page colour, lifted slightly towards its
         * surface colour down the screen. Built from the palette rather
         * than from constants, so switching theme changes both screens
         * instead of stopping at the hinge. */
        s_bgRow[y] = Gfx_Lerp(Skin_ColorBg(), Skin_ColorSurface(), t, 255 * 2);
    }

    /* A soft pool of light behind the meter, brightest across the middle
     * of the bars and falling away above and below. It gives the bars
     * something to stand in front of instead of a flat field. */
    for (int y = VIS_TOP_MARGIN; y < FLOOR_Y; y++) {
        int span = FLOOR_Y - VIS_TOP_MARGIN;
        int d = (y - VIS_TOP_MARGIN) * 2 - span;      /* -span..+span */
        if (d < 0) d = -d;
        int glow = 22 - (22 * d) / span;              /* 22 at centre, 0 at edges */
        s_bgRow[y] = Gfx_Lerp(s_bgRow[y], Skin_ColorAccent(), glow, 255);
    }

    /* Faint horizontal rules, baked into the row table rather than
     * painted over the top of it.
     *
     * They used to be a second pass in DrawBackdrop, which meant two
     * different answers to "what colour is the background at row y": the
     * gradient, and the gradient with a rule on it. The incremental
     * repaint restores uncovered rows from s_bgRow, so every time a bar
     * shrank back past a rule it erased that segment of it for good, and
     * the grid slowly ate away wherever the bars had been. One table with
     * the rules already in it makes the two answers the same by
     * construction, so the bug cannot come back. */
    /* Every 32 rows rather than every 16, and fainter. The bars are the
     * subject; the rules are there to give them a scale to be read
     * against, and at twice the density they were competing with what
     * they were supposed to be measuring. */
    for (int y = VIS_TOP_MARGIN + 20; y < FLOOR_Y; y += 32)
        s_bgRow[y] = Gfx_Lerp(s_bgRow[y], Skin_ColorHairline(), 90, 255);

    /* Bar colour is chosen by how tall the bar is, not by which bar it is:
     * quiet bars stay green and loud ones run into red, which is the
     * level-meter convention people already know how to read. */
    for (int y = 0; y < SCREEN_H; y++) {
        int level = (y * 255) / (SCREEN_H - 1);
        int r, g, b;

        /* Teal at the bottom through green and amber to red at the top.
         * Starting cool rather than at full green gives the quiet part of
         * the meter somewhere to go, so low-level material still reads as
         * movement instead of a flat green block. */
        /* Blue at the floor, through cyan and green to amber and red at
         * the top. Starting on the accent colour rather than on teal ties
         * the meter to the rest of the interface, and gives the quiet end
         * a longer run of distinguishable colour - most music spends its
         * time down here, so that is where the resolution is worth
         * spending. */
        /* Mostly the theme's own accent, with warmth only at the very
         * top.
         *
         * This used to spend its whole lower two thirds crossing into a
         * bright green, which meant that whatever accent was chosen the
         * meter still read as a green meter - the thing that looked
         * "mixed with green". Now the accent holds for the first two
         * thirds, brightening rather than changing hue, and only the last
         * third warms through amber to red where a level meter has to. */
        uint16_t accent = Skin_ColorAccent();
        uint16_t bright = Gfx_Lerp(accent, GFX_RGB8(0xFF, 0xFF, 0xFF), 2, 5);

        uint16_t c;
        if (level < 160) {
            c = Gfx_Lerp(accent, bright, level, 160);
        } else if (level < 208) {
            c = Gfx_Lerp(bright, GFX_RGB8(0xFF, 0xC0, 0x30), level - 160, 48);
        } else {
            c = Gfx_Lerp(GFX_RGB8(0xFF, 0xC0, 0x30), GFX_RGB8(0xFF, 0x3A, 0x28),
                         level - 208, 47);
        }
        (void)r; (void)g; (void)b;
        s_vuColor[y] = c;
    }

    /* The cap is the bar's own colour lifted most of the way to white,
     * which reads as the tip catching the light rather than as a separate
     * white block sitting on top of it. */
    for (int y = 0; y < SCREEN_H; y++)
        s_capColor[y] = Gfx_Lerp(s_vuColor[y], Skin_ColorText(), 6, 8);

    /* The trace's soft edge: the same colour half-way to the backdrop
     * behind it. Indexed by screen row rather than by level, because what
     * it has to blend into is the backdrop at that row. */
    for (int y = 0; y < SCREEN_H; y++) {
        int level = FLOOR_Y - 1 - y;
        if (level < 0) level = 0;
        if (level >= SCREEN_H) level = SCREEN_H - 1;
        s_waveEdge[y] = Gfx_Lerp(s_vuColor[level], s_bgRow[y], 1, 2);
    }

    /* The reflection's fade depends only on how far below the floor a row
     * is, so the whole ramp is a function of that one index and is built
     * once. Doing it per row per bar meant a Gfx_Lerp - three integer
     * divisions, on a CPU with no divider - about 2500 divisions a frame. */
    for (int i = 0; i < REFLECT_H; i++) {
        int y = FLOOR_Y + 2 + i;
        if (y >= SCREEN_H) y = SCREEN_H - 1;
        /* Eased falloff rather than linear: a real reflection dies away
         * quickly near the surface and only lingers faintly after that,
         * where the straight ramp read as a second, upside-down bar.
         *
         * The curve is 1-(1-t)^2, which starts steep and flattens - the
         * opposite way round from t^2, which is what this was first
         * written as and which made the tail brighter rather than
         * dimmer. */
        const int den = REFLECT_H + 4;    /* fully background */
        const int nearest = 11;           /* dimmest the row next to the floor gets */
        int n = REFLECT_H - 1;
        int u = n - i;
        int fade = den - ((den - nearest) * u * u) / (n * n);
        s_reflectColor[i] = Gfx_Lerp(s_vuColor[i], s_bgRow[y], fade, REFLECT_H + 4);
    }
}

void Visualizer_Init(void)
{
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankB(VRAM_B_MAIN_BG_0x06020000);

    s_bgId = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    bgSetPriority(s_bgId, 3);

    memset(s_buf[0], 0, SCREEN_W * SCREEN_H * 2);
    memset(s_buf[1], 0, SCREEN_W * SCREEN_H * 2);

    s_backBuffer = 1;

    memset(s_barHeight, 0, sizeof(s_barHeight));
    memset(s_peak, 0, sizeof(s_peak));
    memset(s_peakHold, 0, sizeof(s_peakHold));

    Visualizer_Invalidate();
    BuildTables();
}

void Visualizer_TargetBackBuffer(void)
{
    Gfx_SetTarget(s_buf[s_backBuffer]);
}

int Visualizer_BackBuffer(void)
{
    return s_backBuffer;
}

void Visualizer_Present(void)
{
    if (s_bgId < 0) return;

    bgSetMapBase(s_bgId, s_backBuffer ? BUF1_BASE : 0);
    s_backBuffer ^= 1;
}

/* Starts below the overlay strip: those rows belong to
 * Skin_DrawTopOverlay, and leaving them alone is what lets the overlay
 * skip redrawing itself on frames where nothing it shows has changed. */
static void DrawBackdrop(void)
{
    COUNT_PIXELS(SCREEN_W * (SCREEN_H - VIS_TOP_MARGIN));

    /* Rules included - see BuildTables. */
    for (int y = VIS_TOP_MARGIN; y < SCREEN_H; y++)
        Gfx_Fill(0, y, SCREEN_W, 1, s_bgRow[y]);
}

/* Peak amplitude of one slice of the frame. A real FFT would be the
 * honest way to get a spectrum, but it would also eat the CPU budget the
 * MP3 decode needs; a peak envelope across 32 columns reads as a lively
 * music-reactive display for a fraction of the cost. */
static int SlicePeak(const int16_t *frames, int start, int count)
{
    int peak = 0;
    for (int i = 0; i < count; i++) {
        int s = frames[(start + i) * 2];
        int a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    return peak;
}

static int s_style = VIS_BARS;

void Visualizer_SetStyle(int style)
{
    if (style >= 0 && style < VIS_STYLES) s_style = style;
}

int Visualizer_Style(void) { return s_style; }

const char *Visualizer_StyleName(int i)
{
    static const char *const kNames[VIS_STYLES] = {
        /* "Wide" rather than "Pillars", from when these shared a row of
         * 222 pixels and 41 was too wide for a 44-pixel share. They have
         * a page of their own now and the constraint is gone, but the
         * name has been on screen long enough to keep. */
        "Bars", "Blocks", "Mirror", "Wave", "Wide",
        "Dots", "Comb", "Hang",
        "Steps", "Trail", "Edge", "Drop"
    };
    return (i >= 0 && i < VIS_STYLES) ? kNames[i] : "?";
}

const char *Top_ChoiceName(int choice)
{
    /* The cards are named for what they put on the screen rather than
     * for how they are drawn, because that is the only thing the person
     * choosing between them can see. */
    static const char *const kCards[TOP_CARDS] = {
        "Now playing", "Cover", "Cassette", "Gramophone"
    };

    if (TOP_IS_METER(choice)) return Visualizer_StyleName(choice);
    if (TOP_IS_CARD(choice))  return kCards[choice - VIS_STYLES];
    return "?";
}

/* True if row `y` of a column whose top edge is at `barTop` is lit.
 *
 * This one predicate is the whole difference between three of the five
 * styles. Everything around it - which rows changed, what gets repainted,
 * how the peak marker is restored - stays exactly as it was for bars, so
 * the styles inherit that logic rather than each growing their own copy
 * of it to get subtly wrong. */
#define VIS_CENTRE ((VIS_TOP_MARGIN + FLOOR_Y) / 2)

static inline bool ColumnLit(int y, int barTop)
{
    if (s_style == VIS_MIRROR) {
        /* Grows both ways from the middle instead of up from the floor,
         * so it is the one style where rows above barTop can be lit. */
        int half = (FLOOR_Y - barTop) / 2;
        int d = y - VIS_CENTRE;
        return (d < 0 ? -d : d) <= half;
    }

    if (s_style == VIS_DROP) {
        /* Dots, hanging: the tip of a column that grows down from the
         * ceiling, which is the one edge that moves. */
        int bottom = VIS_TOP_MARGIN + (FLOOR_Y - barTop);
        return y < bottom && y >= bottom - DOT_ROWS;
    }

    if (s_style == VIS_HANG) {
        /* The same spectrum, grown down from the ceiling instead of up
         * from the floor. One moving edge, like Bars, so it costs the
         * same - which the first version of this style did not: it drew
         * both a standing and a hanging column, and two edges that far
         * apart make the dirty span the whole column. 4480 rows a frame
         * against Bars' few hundred, for a second bar. */
        return y < VIS_TOP_MARGIN + (FLOOR_Y - barTop);
    }

    if (y < barTop) return false;

    switch (s_style) {
        case VIS_PILLARS:
            /* Solid all the way down, but only the middle four columns
             * of the six get drawn - handled by the caller's width, not
             * here - so it reads as a heavier, more separated bar. */
            return true;
        case VIS_BLOCKS:
            /* Three rows lit, one dark, counted from the floor so the
             * segments stay put as the column moves. */
            return ((FLOOR_Y - 1 - y) & 3) != 3;
        case VIS_COMB:
            /* One and one, from the floor for the same reason. Finer than
             * Blocks, and at this pitch the column reads as a texture
             * rather than as a stack. */
            return ((FLOOR_Y - 1 - y) & 1) == 0;
        case VIS_STEPS:
            /* Two and two: between Blocks' heavy segments and Comb's fine
             * texture, and the one that reads as a ladder. */
            return ((FLOOR_Y - 1 - y) & 3) < 2;
        case VIS_TRAIL: {
            /* The tip, then two dashes falling away below it - a column
             * caught mid-fade rather than a solid one. Measured down from
             * the top edge, so the trail moves with the level instead of
             * standing still against the floor. */
            int d = y - barTop;
            return d < DOT_ROWS ||
                   (d >= DOT_ROWS + 3 && d < DOT_ROWS + 6) ||
                   (d >= DOT_ROWS + 10 && d < DOT_ROWS + 12);
        }
        case VIS_EDGE:
            /* Lit all the way down, so the sides take their colour from
             * the same ramp; PaintColumn is what hollows it out. */
            return true;
        case VIS_DOTS:
            /* The tip alone. Everything below it stays backdrop, so a
             * quiet passage is a row of low sparks rather than a wall,
             * and it touches four rows a column instead of forty. */
            return y < barTop + DOT_ROWS;
        default:
            return true;
    }
}

/* Which rows a column may have changed in, given where its top edge was
 * and is. Bars sweep between the two edges; dots carry a three-row tip
 * that has to be cleared behind it; mirror moves at both ends at once,
 * so its span is measured out from the centre line instead.
 *
 * Getting this wrong does not show up as a wrong pixel now - it shows up
 * as a stale one that never gets repainted, which is why the incremental
 * check compares against a full redraw rather than trusting the rule. */
static inline void ColumnSpan(int barTop, int oldTop, int *lo, int *hi)
{
    if (s_style == VIS_MIRROR) {
        /* Out from the centre far enough to cover both edges, old and
         * new.
         *
         * This did repaint the whole column, because a tight span left
         * stale pixels the first time it was tried - but that was with
         * the peak marker still running, drawing white rows at a
         * floor-anchored position no mirror span would ever include.
         * With the marker and the reflection both off for this style,
         * the only thing that moves is the pair of edges, and they are
         * both a known distance from the centre. */
        int a = (FLOOR_Y - barTop) / 2;
        int b = (FLOOR_Y - oldTop) / 2;
        int half = (a > b ? a : b) + 1;
        *lo = VIS_CENTRE - half;
        *hi = VIS_CENTRE + half + 1;
        return;
    }

    if (s_style == VIS_DROP) {
        /* The same single edge Hang has, plus the tip's own depth. */
        int a = VIS_TOP_MARGIN + (FLOOR_Y - barTop);
        int b = VIS_TOP_MARGIN + (FLOOR_Y - oldTop);
        *lo = (a < b ? a : b) - DOT_ROWS - 1;
        *hi = (a > b ? a : b) + 1;
        if (*lo < VIS_TOP_MARGIN) *lo = VIS_TOP_MARGIN;
        return;
    }

    if (s_style == VIS_HANG) {
        /* The one edge, at the bottom of the hanging column, between
         * where it was and where it is. */
        int a = VIS_TOP_MARGIN + (FLOOR_Y - barTop);
        int b = VIS_TOP_MARGIN + (FLOOR_Y - oldTop);
        *lo = (a < b ? a : b) - CAP_ROWS;
        *hi = (a > b ? a : b) + 1;
        if (*lo < VIS_TOP_MARGIN) *lo = VIS_TOP_MARGIN;
        return;
    }

    *lo = barTop < oldTop ? barTop : oldTop;
    *hi = barTop > oldTop ? barTop : oldTop;
    /* How far below the lower edge anything can still be lit. Dots carry
     * a tip; Trail carries the tip and both dashes, and a span stopping
     * at the tip would leave the dashes behind as the column moved. */
    *hi += (s_style == VIS_TRAIL) ? (DOT_ROWS + 12)
         : (s_style == VIS_DOTS)  ? DOT_ROWS
                                  : CAP_ROWS;
}

/* Writes rows [y0,y1) of one bar's column, picking the right colour for
 * each: bar below its top edge, backdrop above it. */
static inline void PaintColumn(uint16_t *buf, int x0, int y0, int y1,
                               int barTop, int gridStep)
{
    (void)gridStep;

    if (y1 <= y0) return;
    COUNT_PIXELS((y1 - y0) * BAR_W);

    for (int y = y0; y < y1; y++) {
        COUNT_ROW();
        uint16_t c;
        if (ColumnLit(y, barTop)) {
            /* Mirror has no single top edge to highlight, so it takes its
             * colour from how far out from the centre the row is - which
             * keeps the same quiet-to-loud ramp the others use. */
            int level = (s_style == VIS_MIRROR)
                      ? (FLOOR_Y - 1 - VIS_CENTRE) + ((y > VIS_CENTRE) ? (y - VIS_CENTRE)
                                                                       : (VIS_CENTRE - y))
                      : (FLOOR_Y - 1 - y);
            if (level < 0) level = 0;
            if (level >= SCREEN_H) level = SCREEN_H - 1;

            /* The cap goes on the edge that moves.
             *
             * For every floor-anchored style that is barTop. For Hang it
             * is the bottom of the hanging column, and using barTop there
             * put a bright band at a position the dirty span had no
             * reason to include - 852 stale pixels a frame, left behind
             * as the band moved. */
            bool onEdge;
            if (s_style == VIS_HANG)
                onEdge = y >= VIS_TOP_MARGIN + (FLOOR_Y - barTop) - CAP_ROWS;
            else
                onEdge = (s_style != VIS_MIRROR) && y < barTop + CAP_ROWS;

            c = onEdge ? s_capColor[level] : s_vuColor[level];
        }
        else
            /* Straight from the row table, gridlines and all.
             *
             * This used to re-derive the rule here and paint it with the
             * raw hairline colour, while the backdrop painted the same
             * row with the hairline blended into the background. Two
             * answers for one row again - the same fault that ate the
             * grid before, in a second copy that survived the first fix.
             * It only stayed hidden because Mirror repainted whole
             * columns, which happened to overwrite every gridline row
             * with this version consistently. */
            c = s_bgRow[y];

        uint16_t *p = &buf[y * SCREEN_W + x0];
        if (s_style == VIS_EDGE && ColumnLit(y, barTop) &&
            y >= barTop + CAP_ROWS) {
            /* Hollow: the two outer pixels in the bar's colour and the
             * four between them left as backdrop. The cap above stays
             * solid, so it reads as an outline with a lid rather than as
             * two loose lines. */
            uint16_t e = s_bgRow[y];
            p[0] = c; p[1] = e; p[2] = e; p[3] = e; p[4] = e; p[5] = c;
        } else if (s_style == VIS_PILLARS && ColumnLit(y, barTop)) {
            /* Ink only in the middle, backdrop either side, so the
             * columns separate without changing the grid they sit on. */
            uint16_t e = ((y - VIS_TOP_MARGIN - 12) % gridStep == 0 &&
                          y > VIS_TOP_MARGIN + 12)
                       ? Skin_ColorHairline() : s_bgRow[y];
            p[0] = e; p[1] = c; p[2] = c; p[3] = c; p[4] = c; p[5] = e;
        } else {
            p[0] = c; p[1] = c; p[2] = c; p[3] = c; p[4] = c; p[5] = c;
        }
    }
}

static inline void PaintRow(uint16_t *buf, int x0, int y, uint16_t c)
{
    uint16_t *p = &buf[y * SCREEN_W + x0];
    p[0] = c; p[1] = c; p[2] = c; p[3] = c; p[4] = c; p[5] = c;
    COUNT_PIXELS(BAR_W);
    COUNT_ROW();
}

/* One y per screen column, per buffer: what the trace was last drawn at,
 * so the previous frame's line can be erased exactly rather than by
 * repainting the whole field. 1KB of state to avoid ~40,000 pixel writes
 * a frame. */
/* The waveform itself rather than its envelope: 256 columns of signed
 * sample, one dot each, joined to the previous column so the trace reads
 * as a line instead of a dotted cloud. */
static void RenderWave(uint16_t *buf, int bi,
                       const int16_t *frames, int frameCount)
{
    const int centre = VIS_CENTRE;
    const int amp = (FLOOR_Y - VIS_TOP_MARGIN) / 2 - 2;

    int prevY = centre;

    /* One sample per column, from a window the width of the screen -
     * not the whole buffer squeezed into it.
     *
     * This is what made the trace ugly and expensive at the same time.
     * Mapping five hundred and forty samples across two hundred and
     * fifty-six columns puts two samples in every column, so neighbouring
     * columns are two samples apart and jump accordingly: on anything
     * with treble in it the trace stops being a wave and becomes a dense
     * band. And every one of those jumps is drawn as a join, which is a
     * cache line for every row of it - the same steepness that made it
     * look wrong made it cost.
     *
     * Showing consecutive samples instead gives a curve that moves a row
     * or two per column, which is both what a waveform looks like and
     * almost nothing to draw.
     *
     * The window starts at a rising zero crossing where one can be found.
     * Without it the trace slides sideways from frame to frame by
     * whatever the phase happens to have advanced - technically correct
     * and unreadable, the same reason an oscilloscope has a trigger. */
    int start = 0;
    if (frameCount > SCREEN_W) {
        int limit = frameCount - SCREEN_W;
        for (int i = 1; i < limit; i++) {
            if (frames[(i - 1) * 2] < 0 && frames[i * 2] >= 0) { start = i; break; }
        }
    }

    for (int x = 0; x < SCREEN_W; x += WAVE_STEP) {
        int y;
        if (frameCount > 0) {
            int idx = (frameCount > SCREEN_W)
                    ? start + x
                    : (x * frameCount) / SCREEN_W;
            if (idx >= frameCount) idx = frameCount - 1;
            /* Left channel only. The two are near-identical on most
             * material and averaging them costs a second load per column
             * to make the trace slightly duller. */
            y = centre - ((int)frames[idx * 2] * amp) / 32768;
        } else {
            y = centre;
        }
        if (y < VIS_TOP_MARGIN) y = VIS_TOP_MARGIN;
        if (y >= FLOOR_Y) y = FLOOR_Y - 1;

        /* Joined to the column before it, so a steep edge draws as a
         * continuous stroke rather than two dots with a gap. Without this
         * the trace breaks up into a cloud on anything but a slow tone. */
        int top = (y < prevY) ? y : prevY;
        int bot = (y > prevY) ? y : prevY;
        prevY = y;

        /* The join is capped.
         *
         * Joining a column to the one before it is what stops the trace
         * breaking into a cloud, and on music the two are a few rows
         * apart so it costs almost nothing. On a sharp transient - or on
         * the noise the tests use, where every sample is independent -
         * the join is the full height of the field, and a column that
         * tall is a stripe rather than a trace: it carries no shape and
         * costs a cache line for every row of it.
         *
         * Truncated from the far end, so the current sample stays exactly
         * where it belongs and only the tail reaching back to the
         * previous one is shortened. */
        if (bot - top > WAVE_MAX_JOIN) {
            if (y <= top) bot = top + WAVE_MAX_JOIN;
            else          top = bot - WAVE_MAX_JOIN;
        }


        int oldTop = s_waveDrawn[bi] ? s_waveTop[bi][x] : -1;
        int oldBot = s_waveDrawn[bi] ? s_waveBot[bi][x] : -1;

        if (oldTop == top && oldBot == bot) continue;

        /* Only the rows that changed state.
         *
         * A trace column overlaps its previous self heavily from one
         * frame to the next, and a row that is trace in both frames is
         * already the right colour - the colour depends only on y. Wiping
         * the whole old span and repainting the whole new one was doing
         * that work twice for nothing, and it made this by far the most
         * expensive style rather than the cheapest. */
        if (oldTop < 0) {
            for (int yy = top; yy <= bot; yy++) {
                uint16_t *px = &buf[yy * SCREEN_W + x];
                px[0] = s_vuColor[FLOOR_Y - 1 - yy];
                for (int k = 1; k < WAVE_STEP; k++) px[k] = s_waveEdge[yy];
                COUNT_ROW();
            }
            COUNT_PIXELS((bot - top + 1) * WAVE_STEP);
        } else {
            /* Rows that were trace and are not any more. */
            for (int yy = oldTop; yy <= oldBot; yy++) {
                if (yy >= top && yy <= bot) { yy = bot; continue; }
                uint16_t *px = &buf[yy * SCREEN_W + x];
                uint16_t c = s_bgRow[yy];
                for (int k = 0; k < WAVE_STEP; k++) px[k] = c;
                COUNT_PIXELS(WAVE_STEP); COUNT_ROW();
            }
            /* Rows that are trace now and were not. */
            for (int yy = top; yy <= bot; yy++) {
                if (yy >= oldTop && yy <= oldBot) { yy = oldBot; continue; }
                uint16_t *px = &buf[yy * SCREEN_W + x];
                px[0] = s_vuColor[FLOOR_Y - 1 - yy];
                for (int k = 1; k < WAVE_STEP; k++) px[k] = s_waveEdge[yy];
                COUNT_PIXELS(WAVE_STEP); COUNT_ROW();
            }
        }

        s_waveTop[bi][x] = (int16_t)top;
        s_waveBot[bi][x] = (int16_t)bot;
    }
    s_waveDrawn[bi] = true;
}

void Visualizer_Render(const int16_t *interleavedFrames, int frameCount)
{
    if (!interleavedFrames) frameCount = 0;

    Gfx_SetTarget(s_buf[s_backBuffer]);

    const int maxH = FLOOR_Y - VIS_TOP_MARGIN;
    int chunk = (frameCount > 0) ? frameCount / NUM_BARS : 0;

    for (int b = 0; b < NUM_BARS; b++) {
        int target = 0;
        if (chunk > 0)
            target = (SlicePeak(interleavedFrames, b * chunk, chunk) * maxH) / 32768;

        /* Rise fast, fall slow: matching the attack to the decay makes the
         * display look sluggish on transients and twitchy on sustain. */
        if (target > s_barHeight[b])
            s_barHeight[b] = target;
        else
            s_barHeight[b] -= (s_barHeight[b] - target) / 4 + 1;

        if (s_barHeight[b] < 0) s_barHeight[b] = 0;

        if (s_barHeight[b] >= s_peak[b]) {
            s_peak[b] = s_barHeight[b];
            s_peakHold[b] = 20;
        } else if (s_peakHold[b] > 0) {
            s_peakHold[b]--;
        } else if (s_peak[b] > 0) {
            s_peak[b] -= 2;
        }
    }

    uint16_t *buf = Gfx_Target();
    int bi = s_backBuffer;

    if (s_style == VIS_WAVE) {
        if (!s_bufDrawn[bi]) {
            DrawBackdrop();
            s_waveDrawn[bi] = false;
            s_bufDrawn[bi] = true;
        }
        RenderWave(buf, bi, interleavedFrames, frameCount);
        return;
    }

    /* The backdrop and floor line never change, so they are painted once
     * per buffer and then left alone. */
    if (!s_bufDrawn[bi]) {
        DrawBackdrop();
        Gfx_Fill(0, FLOOR_Y, SCREEN_W, 1, Skin_ColorText());
        Gfx_Fill(0, FLOOR_Y + 1, SCREEN_W, 1, Skin_ColorHairline());

        for (int b = 0; b < NUM_BARS; b++) {
            s_drawnH[bi][b] = 0;
            s_drawnPeakY[bi][b] = -1;
            s_drawnRefl[bi][b] = 0;
        }
        s_bufDrawn[bi] = true;
    }

    const uint16_t white = Skin_ColorText();

    for (int b = 0; b < NUM_BARS; b++) {
        int x0 = b * BAR_STRIDE + 1;

        int h = s_barHeight[b];
        int barTop = FLOOR_Y - h;
        int oldTop = FLOOR_Y - s_drawnH[bi][b];

        int peakY = -1;
        if (s_style != VIS_MIRROR && s_style != VIS_HANG &&
            s_style != VIS_DROP && s_peak[b] > 0) {
            peakY = FLOOR_Y - 1 - s_peak[b];
            if (peakY < VIS_TOP_MARGIN) peakY = VIS_TOP_MARGIN;
        }
        int oldPeakY = s_drawnPeakY[bi][b];

        /* Only the band the bar's top edge swept across actually changed
         * colour. Everything below it was bar before and is bar now;
         * everything above was backdrop and still is. Repainting the whole
         * column cost several times this for no visible difference. */
        /* Extended past the old top edge because the cap rows there are
         * now ordinary bar and have to lose their highlight. */
        int lo, hi;
        ColumnSpan(barTop, oldTop, &lo, &hi);
        if (lo < VIS_TOP_MARGIN) lo = VIS_TOP_MARGIN;
        if (hi > FLOOR_Y) hi = FLOOR_Y;

        if (s_style == VIS_MIRROR) {
            /* Two bands, not the whole stretch between them: the middle
             * is lit before and after, and its colour depends only on the
             * row, so it is already correct. Only the two edges move. */
            int a = (FLOOR_Y - barTop) / 2, b2 = (FLOOR_Y - oldTop) / 2;
            int loHalf = (a < b2 ? a : b2), hiHalf = (a > b2 ? a : b2) + 1;

            int t0 = VIS_CENTRE - hiHalf, t1 = VIS_CENTRE - loHalf;
            if (t0 < VIS_TOP_MARGIN) t0 = VIS_TOP_MARGIN;
            if (t1 > FLOOR_Y) t1 = FLOOR_Y;
            PaintColumn(buf, x0, t0, t1, barTop, 16);

            int b0 = VIS_CENTRE + loHalf, b1 = VIS_CENTRE + hiHalf + 1;
            if (b0 < VIS_TOP_MARGIN) b0 = VIS_TOP_MARGIN;
            if (b1 > FLOOR_Y) b1 = FLOOR_Y;
            PaintColumn(buf, x0, b0, b1, barTop, 16);
        } else {
            PaintColumn(buf, x0, lo, hi, barTop, 16);
        }

        /* The old peak marker sat on top of whatever was underneath it, so
         * it has to be put back unless the sweep above already covered it. */
        if (oldPeakY >= 0 && oldPeakY != peakY) {
            for (int k = 0; k < 2; k++) {
                int y = oldPeakY + k;
                if (y < VIS_TOP_MARGIN || y >= FLOOR_Y) continue;
                if (y >= lo && y < hi) continue;      /* already repainted */
                PaintRow(buf, x0, y,
                         (y >= barTop)
                           ? ((y < barTop + CAP_ROWS) ? s_capColor[FLOOR_Y - 1 - y]
                                                      : s_vuColor[FLOOR_Y - 1 - y])
                           : s_bgRow[y]);
            }
        }

        if (peakY >= 0) {
            for (int k = 0; k < 2; k++) {
                int y = peakY + k;
                if (y >= FLOOR_Y) break;
                PaintRow(buf, x0, y, white);
            }
        }

        /* Reflection: the colour of each row is fixed, so only the rows
         * between the old and new height need touching. */
        int rh = (s_style == VIS_MIRROR || s_style == VIS_HANG ||
                  s_style == VIS_DROP)
               ? 0 : ((h < REFLECT_H) ? h : REFLECT_H);
        int oldRh = s_drawnRefl[bi][b];
        int rlo = rh < oldRh ? rh : oldRh;
        int rhi = rh > oldRh ? rh : oldRh;

        for (int i = rlo; i < rhi; i++) {
            int y = FLOOR_Y + 2 + i;
            if (y >= SCREEN_H) break;
            PaintRow(buf, x0, y, (i < rh) ? s_reflectColor[i] : s_bgRow[y]);
        }

        s_drawnH[bi][b] = h;
        s_drawnPeakY[bi][b] = peakY;
        s_drawnRefl[bi][b] = rh;
    }
}
