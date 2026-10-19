#include "gfx.h"

#include <stdint.h>

#include <string.h>

/* The typeface is this project's own (tools/make_font.py), not libnds'
 * console font. That one works, but it is a chunky 2px-stem face built
 * for a terminal; this is a 1px-stem design with a real baseline and
 * descenders, which reads far more like the small UI type the skin is
 * imitating - and it means the look is not at the mercy of whatever
 * libnds ships.
 *
 * Bit 0 of each byte is the leftmost pixel of that row. */
#include "font_data.h"

#define FONT_FIRST DSXP_FONT_FIRST
#define FONT_LAST  DSXP_FONT_LAST
#define FONT_COUNT (FONT_LAST - FONT_FIRST + 1)
/* Two typefaces, both drawn glyph by glyph in tools/make_font.py, and
 * the choice between them is which table gets read.
 *
 * This started out as one face plus weights derived from it - bold by
 * smearing each row sideways, and a wider one by doubling its columns.
 * Neither is a typeface choice: a bold version of a face is the same
 * face, and on a 6x8 body it mostly fills in the counters and makes text
 * muddier. The doubled one was worse than useless, since a 6px glyph
 * needs 12 columns to stretch into and the cell holds 8, so most letters
 * simply lost their right-hand side. Two faces that differ in their
 * letterforms is the thing worth offering. */
#define FONT_ROWS(gi) (&g_dsxpFonts[s_fontFace][(gi) * 8])

/* Per-glyph ink extents, measured once at startup: `left` is the first
 * column that has any pixels, `width` how many columns are occupied.
 * Advancing by the measured width instead of a fixed cell is what turns
 * the fixed 8x8 cell font into a proportional one, which fits roughly a
 * third more characters per line - the difference between showing a song
 * filename and showing the first half of one. */
static uint8_t s_glyphLeft[FONT_COUNT];
static uint8_t s_glyphWidth[FONT_COUNT];
static bool    s_fontReady = false;

#define SPACE_WIDTH 3
/* Two, not one.
 *
 * The face has 1px stems, so a single blank column between glyphs is the
 * same width as the strokes either side of it - which reads as letters
 * touching rather than as spacing, and turns "rn" into something close
 * to "m". Two columns is the smallest gap that separates them at this
 * size. It costs about one pixel per character in line width, which the
 * clipped-text paths absorb. */
/* One pixel. Two was an overcorrection: the complaint it answered was
 * about the vertical gap between settings rows, and widening the letters
 * as well turned "hello" into "h e l l o". The glyphs are trimmed to
 * their ink, so one pixel is a real gap - it only reads as touching on
 * faces whose stems already sit hard against the cell edge, and none of
 * the four do. */
#define GLYPH_GAP   1

static uint16_t *s_target = NULL;

/* Rows written since the last reset. Tracking this costs two compares per
 * drawing call and saves transferring - and cache-flushing - the whole
 * 96KB of the bottom screen when only a strip of it moved. */
#ifdef DSXP_COUNT_PIXELS
unsigned long g_gfxPixels = 0;
unsigned long g_gfxPlots  = 0;
#define COUNT_GFX(n) (g_gfxPixels += (unsigned long)(n))
/* Separately from the pixel count: how many of them arrived one call at
 * a time. Two primitives can touch the same number of pixels and cost
 * wildly different amounts, and that difference is the whole of what a
 * per-pixel loop does wrong - the album cover went to the screen through
 * thirteen thousand Gfx_Pixel calls and looked, by pixel count, exactly
 * like the one Gfx_Blit that replaced it. */
#define COUNT_PLOT() (g_gfxPlots++)
#else
#define COUNT_GFX(n) ((void)0)
#define COUNT_PLOT()  ((void)0)
#endif

/* Which rows have been written since the last reset.
 *
 * A bit per row, rather than the topmost and bottommost written.
 *
 * The span was cheaper to keep and much more expensive to honour. A
 * scroll of the file list writes two rows of text near the bottom and
 * moves a scrollbar thumb near the top; between them lies most of the
 * screen, unchanged, and a span said all of it was dirty. Ninety
 * kilobytes of cache flush and DMA, twenty times a second while the
 * d-pad is held, to send about twelve kilobytes that had actually
 * changed - and the flush is worse than the transfer, because walking
 * that range evicts the decoder's tables from a data cache four
 * kilobytes deep.
 *
 * Two compares became a short loop over a bitmap, which is still
 * nothing next to the pixels being written. */
static uint32_t s_dirtyRows[(GFX_H + 31) / 32];
static int s_dirtyTop = GFX_H;
static int s_dirtyBottom = 0;

static inline void MarkDirty(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > GFX_H) y1 = GFX_H;
    if (y1 <= y0) return;

    if (y0 < s_dirtyTop) s_dirtyTop = y0;
    if (y1 > s_dirtyBottom) s_dirtyBottom = y1;

    for (int y = y0; y < y1; y++)
        s_dirtyRows[y >> 5] |= 1u << (y & 31);
}

/* The next run of dirty rows at or after `from`.
 *
 * Returns false when there are none left. `from` is a row index, so a
 * caller walks the screen with it: start at zero, and carry on from the
 * end of each run. */
bool Gfx_NextDirtyRun(int from, int *outY0, int *outY1)
{
    if (from < 0) from = 0;

    int y = from;
    while (y < GFX_H && !(s_dirtyRows[y >> 5] & (1u << (y & 31)))) y++;
    if (y >= GFX_H) return false;

    int end = y;
    while (end < GFX_H && (s_dirtyRows[end >> 5] & (1u << (end & 31)))) end++;

    if (outY0) *outY0 = y;
    if (outY1) *outY1 = end;
    return true;
}

void Gfx_ResetDirty(void)
{
    /* A pending shift is deliberately not dropped here.
     *
     * It is created and consumed inside the same frame - a scroll only
     * happens in the drawing block, and the presenter runs at the end of
     * any frame that drew - so there is never one left over. Clearing it
     * would turn a violation of that into a silent desync between the
     * drawing buffer and the screen, which is a corrupted list that
     * nothing puts right. Leaving it means the next scroll finds one
     * pending, gives up on the cheap path, and marks both bands dirty -
     * which resynchronises. */

    s_dirtyTop = GFX_H;
    s_dirtyBottom = 0;
    memset(s_dirtyRows, 0, sizeof(s_dirtyRows));
}

void Gfx_GetDirty(int *top, int *bottom)
{
    *top = s_dirtyTop;
    *bottom = s_dirtyBottom;
}

static int s_fontFace = 0;

/* Horizontal clip window for text.
 *
 * Scrolling text needs to be drawn starting left of where it is allowed
 * to appear, with the overhang thrown away rather than painted and then
 * covered up. Covering up is what the file list did, and it only worked
 * because the list happened to own the pixels either side; on the top
 * screen there is nothing to cover with. A window that DrawGlyph honours
 * makes the clipping the text renderer's job, where it belongs. */
static int s_clipX0 = 0, s_clipX1 = GFX_W;

void Gfx_SetClipX(int x0, int x1)
{
    s_clipX0 = (x0 < 0) ? 0 : x0;
    s_clipX1 = (x1 > GFX_W) ? GFX_W : x1;
}

void Gfx_ResetClipX(void) { s_clipX0 = 0; s_clipX1 = GFX_W; }

void Gfx_SetFontStyle(int style)
{
    if (style < 0 || style >= GFX_FONT_STYLES || style == s_fontFace) return;
    s_fontFace = style;
    /* Remeasure: the ink extents belong to the shapes, and a face drawn
     * to different widths that kept the old measurements would set text
     * with the wrong spacing. */
    Gfx_InitFont();
}

int Gfx_FontStyle(void) { return s_fontFace; }

const char *Gfx_FontStyleName(int i)
{
    static const char *const kNames[GFX_FONT_STYLES] = {
        "Sans", "Square", "Narrow", "Round",
        "Roman", "Slab", "Quill", "Antique",
        "Compact", "Grid", "Stencil", "Geometric"
    };
    return (i >= 0 && i < GFX_FONT_STYLES) ? kNames[i] : "?";
}

void Gfx_InitFont(void)
{
    for (int i = 0; i < FONT_COUNT; i++) {
        const uint8_t *rows = FONT_ROWS(i);
        uint8_t ink = 0;
        for (int r = 0; r < 8; r++)
            ink |= rows[r];

        if (ink == 0) { /* space and friends */
            s_glyphLeft[i]  = 0;
            s_glyphWidth[i] = SPACE_WIDTH;
            continue;
        }

        int first = 0;
        while (!(ink & (1 << first))) first++;
        int last = 7;
        while (!(ink & (1 << last))) last--;

        s_glyphLeft[i]  = (uint8_t)first;
        s_glyphWidth[i] = (uint8_t)(last - first + 1);
    }
    s_fontReady = true;
}

void Gfx_SetTarget(uint16_t *bitmap) { s_target = bitmap; }
uint16_t *Gfx_Target(void)           { return s_target; }

void Gfx_Pixel(int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)GFX_W && (unsigned)y < (unsigned)GFX_H) {
        s_target[y * GFX_W + x] = c;
        MarkDirty(y, y + 1);
        COUNT_GFX(1);
        COUNT_PLOT();
    }
}

uint16_t Gfx_GetPixel(int x, int y)
{
    if ((unsigned)x < (unsigned)GFX_W && (unsigned)y < (unsigned)GFX_H)
        return s_target[y * GFX_W + x];
    return 0;
}

void Gfx_SaveCorners(int x, int y, int w, int h, uint16_t out[4])
{
    out[0] = Gfx_GetPixel(x,         y);
    out[1] = Gfx_GetPixel(x + w - 1, y);
    out[2] = Gfx_GetPixel(x,         y + h - 1);
    out[3] = Gfx_GetPixel(x + w - 1, y + h - 1);
}

void Gfx_RoundCorners(int x, int y, int w, int h, const uint16_t saved[4])
{
    Gfx_Pixel(x,         y,         saved[0]);
    Gfx_Pixel(x + w - 1, y,         saved[1]);
    Gfx_Pixel(x,         y + h - 1, saved[2]);
    Gfx_Pixel(x + w - 1, y + h - 1, saved[3]);
}

/* Clips the rect to the screen once, up front, so the inner loops can
 * write straight to VRAM without a bounds test per pixel. */
static bool ClipRect(int *x, int *y, int *w, int *h)
{
    if (*w <= 0 || *h <= 0) return false;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > GFX_W) *w = GFX_W - *x;
    if (*y + *h > GFX_H) *h = GFX_H - *y;
    return *w > 0 && *h > 0;
}

/* A rectangle of pixels copied in, rather than set one at a time.
 *
 * The album cover was going to the screen through Gfx_Pixel, which is a
 * bounds test, a dirty-span update and a counter for every one of
 * twenty-three thousand pixels - and it was doing that several times a
 * second, on the thread that also draws the menus. That was the stutter:
 * not the decoding, which happens once when a track opens, but the
 * blitting, which happened over and over.
 *
 * Row at a time, and in 32-bit words where the alignment allows, for the
 * same reason Gfx_Fill does it: half a 32-bit bus is wasted on 16-bit
 * stores. `srcStride` is in pixels, so a caller can hand over a window
 * onto a larger image. */
/* In fast RAM.
 *
 * These three are what every surface in the app goes through, and they
 * are called from all over it, so they are exactly the code the
 * instruction cache keeps losing to whatever ran in between. Held in
 * ITCM they cost no fetch at all. The audio routines have first claim
 * on that memory and are checked by tools/check_itcm.sh; this is what
 * fits in what they leave.
 *
 * The attribute has to sit immediately before its function. Inserting
 * anything between the two silently decorates the next one along, which
 * has happened here before and is why the checker names each one. */
#ifdef ARM9
__attribute__((section(".itcm"), long_call, target("arm"), noinline))
#endif
void Gfx_Blit(int x, int y, int w, int h, const uint16_t *src, int srcStride)
{
    if (!src) return;

    const int ox = x, oy = y;
    if (!ClipRect(&x, &y, &w, &h)) return;

    MarkDirty(y, y + h);
    COUNT_GFX(w * h);

    /* Whatever the clip took off the top and left has to come off the
     * source too, or a picture pushed off the edge arrives shifted. */
    src += (y - oy) * srcStride + (x - ox);

    for (int r = 0; r < h; r++) {
        uint16_t *d = s_target + (size_t)(y + r) * GFX_W + x;
        const uint16_t *s2 = src + (size_t)r * srcStride;
        int n = w;

        if (((uintptr_t)d & 2) && n) { *d++ = *s2++; n--; }

        /* The source is only word-aligned when its stride and offset
         * happen to agree with the destination's, so it is read as two
         * quarters rather than assumed. */
        uint32_t *d32 = (uint32_t *)d;
        for (int i = 0; i < n / 2; i++)
            d32[i] = (uint32_t)s2[i * 2] | ((uint32_t)s2[i * 2 + 1] << 16);

        if (n & 1) d[n - 1] = s2[n - 1];
    }
}

#ifdef ARM9
__attribute__((section(".itcm"), long_call, target("arm"), noinline))
#endif
void Gfx_Fill(int x, int y, int w, int h, uint16_t c)
{
    if (!ClipRect(&x, &y, &w, &h)) return;

    MarkDirty(y, y + h);
    COUNT_GFX(w * h);

    /* Fills are the single hottest thing in this program - every
     * gradient is h separate full-width fills, and the visualizer
     * repaints the entire screen with them every frame. Writing pixels
     * one 16-bit store at a time wastes half of a 32-bit bus, so the
     * bulk of each row goes out as 32-bit words carrying two pixels,
     * with the odd leading/trailing pixel handled separately. */
    const uint32_t pair = ((uint32_t)c << 16) | c;

    /* A full-width rectangle is one unbroken run in memory, so it does not
     * need to be walked a row at a time: the row loop's setup, alignment
     * test and bounds check all disappear, and what is left is a single
     * linear burst the ARM9 can hold in a store-multiple. Full-width bands
     * are the backbone of the layout - every page background, every
     * separator, the transport plate - so this is the path most fills
     * actually take. */
    if (x == 0 && w == GFX_W) {
        uint32_t *words = (uint32_t *)&s_target[y * GFX_W];
        int count = (w * h) >> 1;

        /* Unrolled eight at a time: GCC turns this into stm instructions,
         * which write eight words per cycle-group instead of one. */
        int k = 0;
        for (; k + 8 <= count; k += 8) {
            words[k + 0] = pair; words[k + 1] = pair;
            words[k + 2] = pair; words[k + 3] = pair;
            words[k + 4] = pair; words[k + 5] = pair;
            words[k + 6] = pair; words[k + 7] = pair;
        }
        for (; k < count; k++) words[k] = pair;
        return;
    }

    for (int j = 0; j < h; j++) {
        uint16_t *row = &s_target[(y + j) * GFX_W + x];
        int i = 0;
        int remaining = w;

        /* Word stores must be 4-byte aligned; if this row starts on an
         * odd pixel, emit one 16-bit store to get there. */
        if (((uintptr_t)row & 2) && remaining > 0) {
            row[0] = c;
            i = 1;
            remaining--;
        }

        uint32_t *words = (uint32_t *)&row[i];
        int wordCount = remaining >> 1;
        for (int k = 0; k < wordCount; k++)
            words[k] = pair;

        if (remaining & 1)
            row[i + (wordCount << 1)] = c;
    }
}

void Gfx_Clear(uint16_t c)
{
    Gfx_Fill(0, 0, GFX_W, GFX_H, c);
}

/* Move a rectangle's own contents up or down inside itself.
 *
 * This exists for the file list. Holding the d-pad shifts the list by one
 * row, and the rows themselves are unchanged - but the per-slot signature
 * cache is keyed by slot, so a shift made every slot a miss and all nine
 * rows were recomposed. Measured at 40,705 pixels against 9,365 for a
 * plain selection move: four times the work, on exactly the frames that
 * already have a scroll to pay for.
 *
 * A shift is a copy. The rows that survive get memmove'd to their new
 * slots and only the newly exposed row is drawn. The pixels still have to
 * be transferred - they genuinely moved - but they no longer have to be
 * composed, and composing is where the glyph decoding and clipping is.
 *
 * `dy` is where the content goes: negative moves it up the screen, which
 * is what scrolling down the list does. The vacated band is left as it is
 * for the caller to draw over.
 */
static GfxScroll s_pendingScroll;

bool Gfx_TakePendingScroll(GfxScroll *out)
{
    if (!s_pendingScroll.target) return false;
    if (out) *out = s_pendingScroll;
    s_pendingScroll.target = NULL;
    return true;
}

void Gfx_ScrollRect(int x, int y, int w, int h, int dy)
{
    if (dy == 0) return;
    if (!ClipRect(&x, &y, &w, &h)) return;

    int dist = dy < 0 ? -dy : dy;
    if (dist >= h) return;                  /* nothing survives the move */

    int moved = h - dist;
    COUNT_GFX(w * moved);

    /* Recorded rather than marked dirty.
     *
     * Marking it dirty made every scrolled row travel to the screen
     * again: a cache flush over the band and a DMA of the same size,
     * twenty times a second while the d-pad is held, to move pixels that
     * had not changed. The presenter replays the shift in video memory
     * instead and only the exposed row is sent.
     *
     * Two scrolls between presents cannot be composed into one shift, so
     * the second gives up and marks both bands dirty - correct, and no
     * slower than this has always been. That only happens on a dropped
     * frame, which is when the cheap path matters least. */
    /* The shortcut only works on a band the DMA can actually address.
     *
     * The presenter replays this shift with dmaCopyWords, which is a
     * 32-bit transfer: it truncates the length to whole words and the
     * hardware ignores the low bits of the address. A band starting on
     * an odd pixel is therefore read from two bytes away from where it
     * was meant to be, and one of an odd width loses its last column -
     * with nothing reported, on a path that only runs while the d-pad
     * is held.
     *
     * Today's list is at x=4 and 238 wide, so both are even and this
     * never fires. That is luck rather than design, and the cost of
     * being wrong is garbled pixels nobody can trace back here. An
     * unaligned band simply takes the ordinary route instead: the
     * memmove below is already alignment-agnostic, and the rows are
     * marked dirty so the presenter sends them the long way. */
    if ((x & 1) || (w & 1)) {
        MarkDirty(y, y + h);
        if (s_pendingScroll.target) {
            MarkDirty(s_pendingScroll.y, s_pendingScroll.y + s_pendingScroll.h);
            s_pendingScroll.target = NULL;
        }
    } else if (s_pendingScroll.target) {
        MarkDirty(s_pendingScroll.y, s_pendingScroll.y + s_pendingScroll.h);
        MarkDirty(y, y + h);
        s_pendingScroll.target = NULL;
    } else {
        s_pendingScroll.target = s_target;
        s_pendingScroll.x = x; s_pendingScroll.y = y;
        s_pendingScroll.w = w; s_pendingScroll.h = h;
        s_pendingScroll.dy = dy;
    }

    /* Copied in the direction that keeps the source ahead of the
     * destination, so overlapping bands do not eat their own input. */
    if (dy < 0) {
        for (int r = 0; r < moved; r++)
            memmove(&s_target[(y + r) * GFX_W + x],
                    &s_target[(y + r + dist) * GFX_W + x],
                    (size_t)w * sizeof(uint16_t));
    } else {
        for (int r = moved - 1; r >= 0; r--)
            memmove(&s_target[(y + r + dist) * GFX_W + x],
                    &s_target[(y + r) * GFX_W + x],
                    (size_t)w * sizeof(uint16_t));
    }
}

void Gfx_HLine(int x, int y, int w, uint16_t c) { Gfx_Fill(x, y, w, 1, c); }
void Gfx_VLine(int x, int y, int h, uint16_t c) { Gfx_Fill(x, y, 1, h, c); }

void Gfx_Icon(int x, int y, int h, const uint8_t *rows, uint16_t c)
{
    for (int r = 0; r < h; r++) {
        uint8_t bits = rows[r];
        for (int b = 0; bits; b++, bits <<= 1)
            if (bits & 0x80)
                Gfx_Pixel(x + b, y + r, c);
    }
}

/* The same, for icons too wide to fit in a byte per row. Sixteen pixels
 * is enough for a shape with an arrowhead on it; eight is not, which is
 * why the shuffle and repeat glyphs used to be assembled from a dozen
 * hand-placed rectangles instead of drawn. */
void Gfx_Icon16(int x, int y, int h, const uint16_t *rows, uint16_t c)
{
    for (int r = 0; r < h; r++) {
        uint16_t bits = rows[r];
        for (int b = 0; bits; b++, bits <<= 1)
            if (bits & 0x8000)
                Gfx_Pixel(x + b, y + r, c);
    }
}

void Gfx_Frame(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    Gfx_HLine(x, y, w, c);
    Gfx_HLine(x, y + h - 1, w, c);
    Gfx_VLine(x, y, h, c);
    Gfx_VLine(x + w - 1, y, h, c);
}

#ifdef ARM9
__attribute__((section(".itcm"), long_call, target("arm"), noinline))
#endif
uint16_t Gfx_Lerp(uint16_t a, uint16_t b, int num, int den)
{
    if (den <= 0) return a;
    if (num < 0) num = 0;
    if (num > den) num = den;

    int ar = a & 0x1F, ag = (a >> 5) & 0x1F, ab = (a >> 10) & 0x1F;
    int br = b & 0x1F, bg = (b >> 5) & 0x1F, bb = (b >> 10) & 0x1F;

    return GFX_RGB(ar + (br - ar) * num / den,
                   ag + (bg - ag) * num / den,
                   ab + (bb - ab) * num / den);
}

void Gfx_VGradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
    if (h <= 0) return;
    for (int j = 0; j < h; j++)
        Gfx_Fill(x, y + j, w, 1, Gfx_Lerp(top, bottom, j, h - 1));
}

void Gfx_Bevel(int x, int y, int w, int h, uint16_t light, uint16_t dark, bool sunken)
{
    uint16_t tl = sunken ? dark : light;
    uint16_t br = sunken ? light : dark;

    Gfx_HLine(x, y, w, tl);
    Gfx_VLine(x, y, h, tl);
    Gfx_HLine(x, y + h - 1, w, br);
    Gfx_VLine(x + w - 1, y, h, br);
}

/* ------------------------------------------------------------------ */
/* text                                                               */
/* ------------------------------------------------------------------ */

static int GlyphIndex(unsigned char ch)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) return '?' - FONT_FIRST;
    return ch - FONT_FIRST;
}

/* Draws one glyph with its ink left-aligned at `x`, and returns its
 * width in pixels. */
static int DrawGlyph(int x, int y, uint16_t c, unsigned char ch)
{
    int gi = GlyphIndex(ch);
    int left = s_glyphLeft[gi];
    int width = s_glyphWidth[gi];

    /* Clip horizontally once for the whole glyph rather than testing
     * every pixel. */
    int col0 = 0, col1 = width;
    if (x < s_clipX0) col0 = s_clipX0 - x;
    if (x + col1 > s_clipX1) col1 = s_clipX1 - x;
    if (col0 >= col1) return width;

    const uint8_t *rows = FONT_ROWS(gi);

    /* Glyphs write straight into the target, so they mark their own rows
     * rather than going through Gfx_Fill. */
    int gy0 = y < 0 ? 0 : y;
    int gy1 = y + GFX_GLYPH_H;
    if (gy1 > GFX_H) gy1 = GFX_H;
    if (gy1 > gy0) MarkDirty(gy0, gy1);
    COUNT_GFX(GFX_GLYPH_H * 6);

    for (int r = 0; r < GFX_GLYPH_H; r++) {
        uint8_t bits = rows[r];
        if (!bits) continue;

        int py = y + r;
        if ((unsigned)py >= (unsigned)GFX_H) continue;

        uint16_t *dst = &s_target[py * GFX_W + x];
        for (int col = col0; col < col1; col++)
            if (bits & (1u << (left + col)))
                dst[col] = c;
    }
    return width;
}

int Gfx_TextWidth(const char *s)
{
    if (!s_fontReady || !s) return 0;

    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        w += s_glyphWidth[GlyphIndex(*p)] + GLYPH_GAP;

    return w > 0 ? w - GLYPH_GAP : 0;
}

int Gfx_Text(int x, int y, uint16_t c, const char *s)
{
    if (!s_fontReady || !s) return x;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (x >= GFX_W) break;
        x += DrawGlyph(x, y, c, *p) + GLYPH_GAP;
    }
    return x;
}

int Gfx_TextShadow(int x, int y, uint16_t c, uint16_t shadow, const char *s)
{
    if (shadow != GFX_NO_SHADOW)
        Gfx_Text(x + 1, y + 1, shadow, s);
    return Gfx_Text(x, y, c, s);
}

void Gfx_TextClipped(int x, int y, int maxWidth, uint16_t c, uint16_t shadow, const char *s)
{
    if (!s_fontReady || !s || maxWidth <= 0) return;

    if (Gfx_TextWidth(s) <= maxWidth) {
        Gfx_TextShadow(x, y, c, shadow, s);
        return;
    }

    /* Reserve room for the ".." marker before deciding how much of the
     * string fits, so the result is never wider than asked for. */
    int ellipsisW = Gfx_TextWidth("..");
    int budget = maxWidth - ellipsisW;

    char buf[80];
    size_t used = 0;
    int w = 0;

    for (const unsigned char *p = (const unsigned char *)s;
         *p && used < sizeof(buf) - 1; p++) {
        int gw = s_glyphWidth[GlyphIndex(*p)] + GLYPH_GAP;
        if (w + gw > budget) break;
        w += gw;
        buf[used++] = (char)*p;
    }
    buf[used] = '\0';

    int end = Gfx_TextShadow(x, y, c, shadow, buf);
    Gfx_TextShadow(end, y, c, shadow, "..");
}

/* Draws `s` inside [x, x+maxW), sweeping sideways when it does not fit.
 *
 * The sweep pauses at each end - without that the text is never still
 * long enough to start reading - and steps a whole character at a time
 * rather than a pixel, which is far easier to follow on a screen this
 * size and never leaves a half-glyph at the edge.
 *
 * Returns true if it is scrolling, so a caller that caches can know the
 * line needs redrawing next frame. */
/* How far into `s` the sweep has got at `tick`, or 0 if it fits. Split
 * out so the behaviour can be tested without a framebuffer. */
/* Text has to be measured the same way it is drawn.
 *
 * Gfx_Text is proportional - an 'i' is narrower than an 'm' - while
 * Gfx_TextBig is monospaced at `advance` cells per character. Mixing the
 * two is what stopped the Now Playing title from scrolling: Gfx_TextScroll
 * decided a title was too long using the big-font width, then asked for an
 * offset from a function measuring the small-font width, which found it fit
 * and returned zero. The title was clipped and perfectly still, and only
 * titles long enough to overflow both metrics ever moved - which is why it
 * looked like the marquee worked. */
static int MarqueeWidth(const char *s, int scale)
{
    return (scale > 4) ? Gfx_TextBigPropWidth(scale, s) : Gfx_TextWidth(s);
}

int Gfx_MarqueeOffsetFor(uint32_t tick, const char *s, int maxW, int scale)
{
    if (!s_fontReady || !s || maxW <= 0) return 0;
    if (MarqueeWidth(s, scale) <= maxW) return 0;

    int len = (int)strlen(s);
    int firstOff = 0;
    while (firstOff < len && MarqueeWidth(s + firstOff, scale) > maxW)
        firstOff++;

    /* Six steps at six a second is a one-second pause, which is about
     * how long it takes to notice a line has stopped and start reading
     * it. Shorter and it never feels still. */
    const uint32_t kHold = 6;
    uint32_t steps = (uint32_t)firstOff + 1;
    uint32_t cycle = kHold * 2 + steps * 2;
    uint32_t t = tick % cycle;

    int off;
    if (t < kHold)                     off = 0;
    else if (t < kHold + steps)        off = (int)(t - kHold);
    else if (t < kHold * 2 + steps)    off = firstOff;
    else                               off = firstOff - (int)(t - kHold * 2 - steps);

    if (off < 0) off = 0;
    if (off > firstOff) off = firstOff;
    return off;
}

bool Gfx_TextScroll(int x, int y, int maxW, uint16_t c, const char *s,
                    uint32_t tick, int scale)
{
    if (!s_fontReady || !s || maxW <= 0) return false;

    int full = (scale > 4) ? Gfx_TextBigPropWidth(scale, s) : Gfx_TextWidth(s);
    if (full <= maxW) {
        if (scale > 4) Gfx_TextBigProp(x, y, scale, c, s);
        else           Gfx_Text(x, y, c, s);
        return false;
    }

    int off = Gfx_MarqueeOffsetFor(tick, s, maxW, scale);

    Gfx_SetClipX(x, x + maxW);
    if (scale > 4) Gfx_TextBigProp(x, y, scale, c, s + off);
    else           Gfx_Text(x, y, c, s + off);
    Gfx_ResetClipX();
    return true;
}

/* Proportional big text: every glyph advances by its own measured width,
 * exactly as Gfx_Text does, scaled up.
 *
 * The monospaced version needs one cell wide enough for the widest glyph
 * it might ever hold, and neither choice works. Six - the width of the
 * tilde - costs every other letter a column of air and makes a title
 * read as a banner. Five - the width of seventy of the ninety-five -
 * means those seventy fill their cell edge to edge, so "Beat It." comes
 * out with its letters touching.
 *
 * Advancing per glyph gives both: a real gap after every letter, and
 * narrow ones taking the room they need instead of a full cell. */
/* `quarters` rather than a whole multiplier: 4 is single size, 8 is
 * double, 10 is two and a half.
 *
 * Whole steps are far too coarse for a title - the jump from double to
 * triple is a third bigger with nothing between them - and quarters turned
 * out to be coarse too: the one line on the page that answers "what is
 * playing" wants tuning finer than half a font apart. Quarters put four
 * usable sizes between double and triple.
 *
 * Every extent is computed as (n * quarters) / 4 from the glyph's own
 * origin, so the measure and the draw round identically. Scaling each
 * span separately and adding them up would not. */
#define BIG_QUARTERS_MAX 16

/* Heavier than the font was drawn.
 *
 * The font's strokes are one pixel, which at two and a half times is two
 * and a half - thin for the one line on the page that is the answer to
 * "what is playing". The field above measures how far inside the letter
 * a pixel is, so weight is just that measurement offset: push the
 * contour outward and the whole letter thickens evenly, curves and
 * diagonals included, without touching the font or the metrics it is
 * laid out by.
 *
 * In sixteenths of a pixel and scaled with the text, so the letters get
 * heavier as they get larger rather than the weight staying put and
 * looking like a different face at every size. Seven sixteenths of a
 * pixel per whole step: at the title's two and a half times that is a
 * stroke of one and a half where the font drew one - heavier than the
 * font, and still plainly the same letters.
 *
 * It has a ceiling. The field falls to a quarter of `quarters` outside the
 * letter and no further, so a weight at or past that never crosses zero
 * and the entire background inks over. A quarter of that ceiling is a
 * heavy face with a lot of room to spare. */
#define BIG_WEIGHT(q) ((q) * 7 / 8)

_Static_assert(BIG_WEIGHT(BIG_QUARTERS_MAX) * 8 < 4 * 16 * BIG_QUARTERS_MAX,
               "the weight reaches past the field that carries it - the "
               "page would ink over solid");

static bool s_bigBold = false;

/* Hard edges instead of smoothed ones.
 *
 * The coverage renderer exists because a blown-up bitmap font is lumpy,
 * and the shading it puts on an edge is what makes a scaled letter read
 * as the same shape rather than a staircase. Beside text that is *not*
 * scaled, though, that shading reads as a shadow - the eye compares the
 * two and sees a different colour rather than a smoother curve.
 *
 * So where a line has to match unscaled text sitting next to it, the
 * ramp is dropped and the contour is taken as a plain yes or no. The
 * letters are then made of whole pixels in exactly one colour, which is
 * what "the same as that other line" actually means. */
static bool s_bigCrisp = false;

void Gfx_SetBigCrisp(bool on) { s_bigCrisp = on; }

void Gfx_SetBigBold(bool on) { s_bigBold = on; }

/* A heavier letter is drawn past the box its measurement describes, so
 * the box it goes in - and the room between letters - has to allow for
 * it.
 *
 * One number for every size rather than one derived from the weight,
 * because a caller has to be able to lay out around it: the title on Now
 * Playing is placed to clear the like heart, and that placement is a
 * compile time assertion. */
static int BigPad(int quarters)
{
    (void)quarters;
    return s_bigBold ? GFX_BIG_BOLD_PAD : 0;
}

_Static_assert((BIG_WEIGHT(BIG_QUARTERS_MAX) + 15) / 16 <= GFX_BIG_BOLD_PAD,
               "bold text draws further outside its box than callers are "
               "told to expect");

int Gfx_TextBigPropWidth(int quarters, const char *s)
{
    if (!s_fontReady || !s) return 0;
    if (quarters > BIG_QUARTERS_MAX) quarters = BIG_QUARTERS_MAX;

    /* A heavier letter is wider than its ink box, so it needs more room
     * beside it or the strokes run together. Added here as well as in
     * the draw, because every title on both screens is centred or
     * clipped against this number. */
    const int extra = BigPad(quarters);

    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        w += ((s_glyphWidth[GlyphIndex(*p)] + GLYPH_GAP) * quarters) / 4 + extra;
    return w;
}

/* Blown up, a bitmap font stops being letters and becomes its pixels.
 *
 * The old way was to fill a rectangle per source pixel. At three quarters
 * that rectangle is one or two pixels wide depending on where the column
 * lands, so a stem a pixel thick came out one pixel wide in one letter
 * and two in the next, and every diagonal was a staircase with treads of
 * uneven depth. That is what "pixely" is: not the size of the pixels but
 * the fact that the same shape is drawn differently depending on where
 * it happens to sit.
 *
 * Drawn as a shape instead. The glyph's bits are read as samples of a
 * continuous field - bilinear between them - and the letter is the half
 * contour of that field, which is the same idea as the corners of the
 * cover frame: how much of each pixel the shape covers, rather than
 * whether the pixel's middle is inside it.
 *
 * Three things fall out of it, and the third is the one that matters:
 *
 *   Straight edges stay exactly where they were and stay hard. A column
 *   of set bits beside a column of clear ones puts the contour on the
 *   boundary between them, so a stem is not softened and the metrics do
 *   not move by so much as a pixel.
 *
 *   Diagonals and corners are cut, because the contour of a bilinear
 *   field rounds a lone corner rather than stepping around it.
 *
 *   A stem gets the width it is owed. One source pixel at three quarters
 *   is one and a half pixels of ink - a full one and a half-covered
 *   neighbour - wherever it lands, instead of one pixel here and two
 *   there.
 *
 * The ramp is steepened by the scale so the soft part stays the same
 * width however large the text is: an edge that faded over three pixels
 * at triple size would read as blur rather than as a letter.
 *
 * Half a pixel of it, not a whole one. At a whole pixel almost nothing
 * in a title reaches full ink - a stem one source pixel thick is one and
 * a half pixels wide at this size, and two half-pixel ramps eat all of
 * it - so the text came out evenly grey and looked washed rather than
 * smooth. At half, the stems are solid and only the corners and the
 * diagonals are shaded, which is where the stepping was.
 */
#define BIG_EDGE_HALVES 2      /* the soft part, in half destination pixels */
#define BIG_BUF_W      (8 * BIG_QUARTERS_MAX / 4 + 8)
#define BIG_BUF_H      (GFX_GLYPH_H * BIG_QUARTERS_MAX / 4 + 8)

/* 65536/n, so the one division per pixel becomes a multiply. There is no
 * divide instruction on this processor and the ramp below runs for every
 * pixel of every letter. */
static const uint16_t kBigRecip[BIG_QUARTERS_MAX + 1] = {
    0, 65535, 32768, 21845, 16384, 13107, 10923, 9362, 8192,
    7282, 6554, 5958, 5461, 5041, 4681, 4369, 4096
};

/* Where a destination pixel's centre falls in the glyph, as the source
 * pixel it sits in and how far across it is - in quarters of a source
 * pixel, doubled again so the half-pixel offset is a whole number.
 *
 * Computed per row and per column rather than per pixel: it depends only
 * on the position and the scale, and a glyph is a few hundred pixels
 * over twenty distinct coordinates. */
static void BigAxis(int start, int n, int quarters, int8_t *cell, uint8_t *frac)
{
    const int d = 2 * quarters;
    for (int i = 0; i < n; i++) {
        const int j = start + i;
        const int p = 4 * (2 * j + 1) - quarters;   /* (j + 1/2) * 4 - 2, in 4/quarters */
        int c = (p >= 0) ? p / d : -((-p + d - 1) / d);
        cell[i] = (int8_t)c;
        frac[i] = (uint8_t)(p - c * d);
    }
}

int Gfx_TextBigProp(int x, int y, int quarters, uint16_t c, const char *s)
{
    if (!s_fontReady || !s || quarters < 4) return x;
    if (quarters > BIG_QUARTERS_MAX) quarters = BIG_QUARTERS_MAX;

    static uint16_t buf[BIG_BUF_H * BIG_BUF_W];

    const int ih  = (GFX_GLYPH_H * quarters) / 4;
    const int d   = 2 * quarters;
    const int dd  = d * d;
    const int pad = BigPad(quarters);

    const int weight = s_bigBold ? BIG_WEIGHT(quarters) : 0;

    /* The rows and columns drawn, which for a heavier letter reach past
     * the glyph's ink box on every side. */
    const int bh = ih + 2 * pad;

    int8_t  cellY[BIG_BUF_H];
    uint8_t fracY[BIG_BUF_H];
    BigAxis(-pad, bh, quarters, cellY, fracY);

    const int cr = (c >> 0) & 31, cg = (c >> 5) & 31, cb = (c >> 10) & 31;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const int gi = GlyphIndex(*p);
        const uint8_t *rows = FONT_ROWS(gi);
        const int gw = s_glyphWidth[gi];
        const int left = s_glyphLeft[gi];
        const int cell = ((gw + GLYPH_GAP) * quarters) / 4 + pad;
        const int inkW = (gw * quarters) / 4;
        const int bw0  = inkW + 2 * pad;          /* what is actually drawn */

        if (inkW <= 0 || bw0 > BIG_BUF_W) { x += cell; continue; }
        if (x + inkW + pad <= s_clipX0) { x += cell; continue; }
        if (x - pad >= s_clipX1) break;

        /* The span of this glyph that survives the clip, which is what
         * gets read, blended and written back. */
        int bx0 = -pad, bx1 = inkW + pad;
        if (x + bx0 < s_clipX0) bx0 = s_clipX0 - x;
        if (x + bx1 > s_clipX1) bx1 = s_clipX1 - x;
        if (bx1 <= bx0) { x += cell; continue; }

        int8_t  cellX[BIG_BUF_W];
        uint8_t fracX[BIG_BUF_W];
        BigAxis(-pad, bw0, quarters, cellX, fracX);

        const int bw = bx1 - bx0;

        /* The rows the letter actually reaches. Most glyphs leave the
         * top or the bottom of the cell empty - a lower-case letter
         * leaves both - and those rows would be read, written back
         * unchanged and copied out again for nothing. */
        int ry0 = bh, ry1 = 0;
        for (int i = 0; i < bh; i++) {
            const int sy = cellY[i];
            const unsigned a = (sy >= 0 && sy < GFX_GLYPH_H) ? rows[sy] : 0u;
            const unsigned b = (sy + 1 < GFX_GLYPH_H && sy + 1 >= 0)
                             ? rows[sy + 1] : 0u;
            if (!(a | b)) continue;
            if (i < ry0) ry0 = i;
            ry1 = i + 1;
        }
        if (ry1 <= ry0) { x += cell; continue; }

        for (int iy = ry0; iy < ry1; iy++) {
            const int dy = iy - pad;
            const int sy = cellY[iy], vN = fracY[iy];
            /* The two source rows this destination row sits between. */
            const unsigned rowA = (sy >= 0 && sy < GFX_GLYPH_H)
                                ? (unsigned)(rows[sy] >> left) : 0u;
            const unsigned rowB = (sy + 1 >= 0 && sy + 1 < GFX_GLYPH_H)
                                ? (unsigned)(rows[sy + 1] >> left) : 0u;

            uint16_t *out = buf + (iy - ry0) * BIG_BUF_W;

            for (int i = 0; i < bw; i++) {
                const int dx = bx0 + i;
                const int sx = cellX[dx + pad], uN = fracX[dx + pad];

                const int in0 = (sx >= 0 && sx < gw);
                const int in1 = (sx + 1 >= 0 && sx + 1 < gw);
                const int b00 = in0 ? (int)((rowA >> sx) & 1u) : 0;
                const int b10 = in1 ? (int)((rowA >> (sx + 1)) & 1u) : 0;
                const int b01 = in0 ? (int)((rowB >> sx) & 1u) : 0;
                const int b11 = in1 ? (int)((rowB >> (sx + 1)) & 1u) : 0;

                const uint16_t under = Gfx_GetPixel(x + dx, y + dy);

                if (!(b00 | b10 | b01 | b11)) { out[i] = under; continue; }

                const int tN = b00 * (d - uN) * (d - vN)
                             + b10 * uN * (d - vN)
                             + b01 * (d - uN) * vN
                             + b11 * uN * vN;

                /* How far inside the letter this pixel is, in
                 * sixteenths of a pixel - negative outside it. Weight
                 * moves the letter's edge outward by shifting it.
                 *
                 * Half coverage at the contour, and a ramp either side
                 * steep enough that the soft part is about half a
                 * pixel. */
                const int in16 = (((2 * tN - dd) * kBigRecip[quarters]) >> 17);
                int cov = s_bigCrisp ? ((in16 + weight) >= 0 ? 16 : 0)
                                     : 8 + BIG_EDGE_HALVES * (in16 + weight);

                if (cov <= 0)  { out[i] = under; continue; }
                if (cov >= 16) { out[i] = c; continue; }

                const int ur = under & 31, ug = (under >> 5) & 31,
                          ub = (under >> 10) & 31;
                out[i] = GFX_RGB(ur + ((cr - ur) * cov) / 16,
                                 ug + ((cg - ug) * cov) / 16,
                                 ub + ((cb - ub) * cov) / 16);
            }
        }

        /* One blit per letter rather than a plot per pixel. The cover
         * card is drawn on the thread that draws the menus, and single
         * pixel calls on that path are what the picture was taken off. */
        Gfx_Blit(x + bx0, y + ry0 - pad, bw, ry1 - ry0, buf, BIG_BUF_W);
        x += cell;
    }
    return x;
}

int Gfx_TextBigWidth(int scale, int advance, const char *s)
{
    return s ? (int)strlen(s) * advance * scale : 0;
}

int Gfx_TextBig(int x, int y, int scale, int advance, uint16_t c, const char *s)
{
    if (!s_fontReady || !s || scale < 1) return x;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int gi = GlyphIndex(*p);
        const uint8_t *rows = FONT_ROWS(gi);
        int gw = s_glyphWidth[gi];

        /* The font is proportional: leading blank columns are trimmed and
         * s_glyphLeft says how many, so a glyph's first drawn column is
         * bit `left`, not bit 0. Reading from bit 0 renders a different
         * character - which is exactly what it did until this line. */
        int left = s_glyphLeft[gi];
        int gx = x + ((advance - gw) * scale) / 2;
        /* The tilde is wider than the cell. Centring it puts its ink
         * left of the pen, over the previous letter; left-aligning it
         * lets it lean into the next cell instead, which is the space
         * a following letter leaves blank anyway. */
        if (gx < x) gx = x;
        int gr = gx + gw * scale;

        /* The horizontal clip, which this used to ignore.
         *
         * Gfx_Text honours s_clipX0/s_clipX1 inside its own blitter, but
         * this draws through Gfx_Fill, and Gfx_Fill clips to the screen
         * and nothing else. So a marquee at double height obeyed its box
         * on the way in and then ran past it to the edge of the panel -
         * which is what put half an 'm' against the right bezel.
         *
         * Whole glyphs outside the box are skipped, and since text runs
         * left to right the first one past the right edge ends it. */
        if (gr <= s_clipX0) { x += advance * scale; continue; }
        if (gx >= s_clipX1) break;

        bool whole = (gx >= s_clipX0 && gr <= s_clipX1);

        for (int r = 0; r < GFX_GLYPH_H; r++) {
            uint8_t bits = rows[r];
            if (!bits) continue;                  /* whole row blank */
            for (int b = 0; b < gw; b++) {
                if (!(bits & (1u << (left + b)))) continue;
                int px = gx + b * scale, pw = scale;
                /* Only the one or two glyphs straddling an edge pay for
                 * the clamp; the rest take the same path as before. */
                if (!whole) {
                    if (px < s_clipX0) { pw -= s_clipX0 - px; px = s_clipX0; }
                    if (px + pw > s_clipX1) pw = s_clipX1 - px;
                    if (pw <= 0) continue;
                }
                Gfx_Fill(px, y + r * scale, pw, scale, c);
            }
        }
        x += advance * scale;
    }
    return x;
}

int Gfx_TextMonoWidth(int advance, const char *s)
{
    return s ? (int)strlen(s) * advance : 0;
}

int Gfx_TextMono(int x, int y, int advance, uint16_t c, uint16_t shadow, const char *s)
{
    if (!s_fontReady || !s) return x;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int gw = s_glyphWidth[GlyphIndex(*p)];
        int gx = x + (advance - gw) / 2;
        if (shadow != GFX_NO_SHADOW) DrawGlyph(gx + 1, y + 1, shadow, *p);
        DrawGlyph(gx, y, c, *p);
        x += advance;
    }
    return x;
}
