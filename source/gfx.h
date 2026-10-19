#ifndef DSXP_GFX_H
#define DSXP_GFX_H

#include <stdbool.h>
#include <stdint.h>

/* Drawing primitives for the two 16-bit bitmap backgrounds, plus a text
 * renderer.
 *
 * There is deliberately no libnds console anywhere in this program. A
 * 256x256 16-bit bitmap background occupies 128KB, which is the entire
 * sub-engine BG address space, so a tiled text layer physically cannot
 * coexist with one - its map and tile data would land inside the pixels
 * the skin is drawing. Rendering glyphs into the bitmap ourselves avoids
 * that, and along the way frees the UI from the console's 8-pixel grid.
 *
 * The glyphs come from libnds' own `default_font_bin` (a 1bpp 8x8 set),
 * measured at startup so text can be drawn proportionally. */

#define GFX_W 256
#define GFX_H 192

/* Bit 15 is the "opaque" flag for 16-bit bitmap backgrounds. libnds'
 * RGB15() leaves it clear, which makes the colour invisible rather than
 * black - an easy way to draw white text that never shows up. Every
 * colour in this program goes through these two macros instead. */
#define GFX_RGB(r, g, b)  ((uint16_t)(0x8000u | (uint16_t)(r) | ((uint16_t)(g) << 5) | ((uint16_t)(b) << 10)))
#define GFX_RGB8(r, g, b) GFX_RGB((r) >> 3, (g) >> 3, (b) >> 3)

/* Two typefaces, both drawn in tools/make_font.py. */
/* Twelve faces: four drawn glyph by glyph, eight cut from those.
 *
 * Five drawn and seven cut, now: Geometric is drawn to the other
 * convention entirely - circular bowls, a single-storey 'a' and 'g',
 * stems with nothing on the ends of them - because those are what the
 * eye reads as a different typeface at this size.
 *
 * Not a weight and a slope of each: that is twelve entries and four
 * designs, and it was tried and thrown away. The cut ones differ in
 * letterform - feet on the stems, or a narrower set. See
 * tools/make_font.py for what else was tried and why none of it
 * survives at six pixels by eight.
 *
 * The first four keep the numbers they always had, so a saved setting
 * still selects the face it selected before. */
enum { GFX_FONT_SANS = 0, GFX_FONT_SQUARE, GFX_FONT_NARROW, GFX_FONT_ROUND,
       GFX_FONT_ROMAN, GFX_FONT_SLAB, GFX_FONT_QUILL, GFX_FONT_ANTIQUE,
       GFX_FONT_COMPACT, GFX_FONT_GRID, GFX_FONT_STENCIL, GFX_FONT_GEOMETRIC,
       GFX_FONT_STYLES };
#define GFX_FONT_COLS 4
#define GFX_FONT_ROWS 3
#define GFX_FONT_REGULAR GFX_FONT_SANS

void Gfx_SetFontStyle(int style);
int  Gfx_FontStyle(void);
const char *Gfx_FontStyleName(int i);

/* Measures the font. Call once before any Gfx_Text* call. */
void Gfx_InitFont(void);

/* Pixels written and single-pixel calls made since they were last
 * zeroed. Counted with one add per drawing call - not per pixel - which
 * is cheap enough to leave in the ROM, and is the one figure for what
 * the screen costs that nothing else running on the machine can
 * inflate. The frame timer cannot say that: it is wall clock over the
 * loop body, so the mixer thread and a boosted decoder land in it too. */
#ifdef DSXP_COUNT_PIXELS
extern unsigned long g_gfxPixels;
extern unsigned long g_gfxPlots;
#endif

void      Gfx_SetTarget(uint16_t *bitmap);
uint16_t *Gfx_Target(void);

/* Row range touched since the last Gfx_ResetDirty(), so a caller that
 * composes into main RAM can transfer just those rows to VRAM instead of
 * the whole screen. `bottom` is exclusive; empty is bottom <= top. */
void Gfx_ResetDirty(void);
void Gfx_GetDirty(int *top, int *bottom);

/* The dirty rows as runs rather than as one span from the first to the
 * last. Start at row 0 and carry on from the end of each run:
 *
 *     int y0, y1;
 *     for (int at = 0; Gfx_NextDirtyRun(at, &y0, &y1); at = y1) { ... }
 *
 * Two changes at opposite ends of the screen are two runs here and were
 * one span before, which meant sending everything between them. */
bool Gfx_NextDirtyRun(int from, int *outY0, int *outY1);

void Gfx_Clear(uint16_t c);
void Gfx_Pixel(int x, int y, uint16_t c);

/* A block of pixels copied in. `srcStride` is in pixels, so the source
 * may be a window onto a larger image. Clipped like every other
 * primitive here, and the source is offset to match when it is. */
void Gfx_Blit(int x, int y, int w, int h, const uint16_t *src, int srcStride);
uint16_t Gfx_GetPixel(int x, int y);

/* Softens a rectangle by restoring its four corner pixels to whatever was
 * behind it. One pixel is enough to take the hard edge off at this size,
 * and sampling the existing pixel means it works over any background
 * without the caller having to know what it is. */
void Gfx_RoundCorners(int x, int y, int w, int h, const uint16_t saved[4]);
void Gfx_SaveCorners(int x, int y, int w, int h, uint16_t out[4]);
void Gfx_Fill(int x, int y, int w, int h, uint16_t c);
/* Shift a rectangle's contents within itself; negative dy moves them up.
 * The vacated band is left for the caller to redraw. */
void Gfx_ScrollRect(int x, int y, int w, int h, int dy);

/* A scroll that has been applied to the drawing buffer and not yet to the
 * screen.
 *
 * The pixels in a scrolled band do not change - they only move - so
 * sending them to the screen again is paying to transfer the whole band
 * in order to shift it by one row. The presenter can replay the same
 * shift in video memory instead, which costs no cache traffic at all,
 * and then only the newly exposed row has to go across.
 *
 * Returns false when there is nothing pending, which includes the case
 * where two scrolls happened between presents - those cannot be composed
 * this way, so the band is marked dirty and sent the long way instead. */
typedef struct {
    const uint16_t *target;
    int x, y, w, h, dy;
} GfxScroll;

bool Gfx_TakePendingScroll(GfxScroll *out);
void Gfx_HLine(int x, int y, int w, uint16_t c);
void Gfx_VLine(int x, int y, int h, uint16_t c);
void Gfx_Frame(int x, int y, int w, int h, uint16_t c);

/* A 1-bit icon: one byte a row, most significant bit leftmost, so an icon
 * is at most eight pixels wide. Zero bits are left alone rather than
 * painted as background, which is what lets an icon sit on a button face
 * or a tab without carrying a rectangle of its own around with it. */
void Gfx_Icon(int x, int y, int h, const uint8_t *rows, uint16_t c);
void Gfx_Icon16(int x, int y, int h, const uint16_t *rows, uint16_t c);
void Gfx_VGradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom);

/* Two-tone 1px border: `light` on the top/left edges, `dark` on the
 * bottom/right, or swapped when `sunken`. Every control in the skin is
 * built from this plus a gradient, which is what makes the whole UI read
 * as one surface rather than a pile of unrelated boxes. */
void Gfx_Bevel(int x, int y, int w, int h, uint16_t light, uint16_t dark, bool sunken);

uint16_t Gfx_Lerp(uint16_t a, uint16_t b, int num, int den);

#define GFX_GLYPH_H 8

/* Kept for the few places that want a fixed cell - the idle "Nothing"
 * heading - while the title uses the proportional pair below. */
#define GFX_BIG_ADV 6

/* Proportional text. Returns the x coordinate just past the last glyph. */
int  Gfx_Text(int x, int y, uint16_t c, const char *s);
/* Pass as the shadow colour to skip the shadow pass entirely. Zero is not
 * a drawable colour - every real one has bit 15 set - so it is free to
 * use as a sentinel. A flat, high-contrast surface does not need a
 * shadow to stay legible, and skipping it halves what text costs. */
#define GFX_NO_SHADOW ((uint16_t)0)

/* Same, with a 1px offset drop shadow underneath - keeps light text
 * legible where it crosses a light part of a gradient. */
int  Gfx_TextShadow(int x, int y, uint16_t c, uint16_t shadow, const char *s);
int  Gfx_TextWidth(const char *s);

/* Truncates with a trailing ".." when the string will not fit in
 * maxWidth pixels, which is the normal case for song filenames. */
void Gfx_TextClipped(int x, int y, int maxWidth, uint16_t c, uint16_t shadow, const char *s);

/* Fixed-advance text, each glyph centred in its cell. Used for the clock
 * so the digits do not jitter sideways as the seconds tick over. */
int  Gfx_TextMono(int x, int y, int advance, uint16_t c, uint16_t shadow, const char *s);
int  Gfx_TextMonoWidth(int advance, const char *s);

/* Fixed-advance text blown up by an integer factor, each pixel of the
 * glyph drawn as a scale x scale block. There is only one font in the
 * ROM, so this is how anything gets to be bigger than 6x8 - and since it
 * is whole pixels there is no filtering to pay for or to blur it. */
int  Gfx_TextBig(int x, int y, int scale, int advance, uint16_t c, const char *s);

/* Clip window for text, so a scrolling line can be drawn from outside
 * the area it is allowed to occupy. */
void Gfx_SetClipX(int x0, int x1);
void Gfx_ResetClipX(void);

/* Text that sweeps sideways, one character at a time, when it is too
 * long for `maxW`. Returns true while it is moving. `scale` is 1 or 2. */
bool Gfx_TextScroll(int x, int y, int maxW, uint16_t c, const char *s,
                    uint32_t tick, int scale);

/* The sweep position on its own, so it can be checked without drawing. */
int  Gfx_MarqueeOffsetFor(uint32_t tick, const char *s, int maxW, int scale);
int  Gfx_TextBigWidth(int scale, int advance, const char *s);
/* `quarters`, not a whole multiplier: 4 is single size, 8 double, 10 two
 * and a half, 9 two and a quarter. Gfx_TextScroll's scale is the same
 * unit, and anything at or under 4 there is drawn as ordinary text. */
int  Gfx_TextBigProp(int x, int y, int quarters, uint16_t c, const char *s);
int  Gfx_TextBigPropWidth(int quarters, const char *s);

/* Bold text reaches this many pixels outside the box its measurement
 * describes, on every side - the strokes are drawn past the font's own
 * hairline. A caller placing a bold line has to allow for it above and
 * below the y it passes; the width already includes it. */
#define GFX_BIG_BOLD_PAD 2

/* Draws big proportional text with heavier strokes than the font was
 * drawn with. Affects the measurement as well as the drawing, so set it
 * around both. */
void Gfx_SetBigBold(bool on);

/* Draws big text with hard edges rather than smoothed ones, for a line
 * that has to match unscaled text beside it - the smoothing reads as a
 * shadow when the two are compared. Affects drawing only. */
void Gfx_SetBigCrisp(bool on);

#endif /* DSXP_GFX_H */
