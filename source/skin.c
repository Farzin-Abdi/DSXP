#include "skin.h"
#include "textfold.h"
#include "cassette.h"
#include "gramophone.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "gfx.h"
#include "player.h"       /* PLAYER_SLEEP_* range for the slider */
#include "settings.h"
#include "visualizer.h"    /* GAIN_MAX_DB */
#include "decoder_mp3.h"
#include "audio.h"       /* the volume, for the plinth's slider */

/* ---- the palette: computed values, nothing sampled ---- */
/* A flat palette, and that is the whole design.
 *
 * The old skin was built from vertical gradients, two-tone bevels, gloss
 * hairlines, rounded corners and drop-shadowed text. Every one of those
 * costs work that a 67MHz ARM9 with no divider has to find room for
 * between MPEG frames, and together they meant a full repaint wrote every
 * pixel on the screen more than twice - backdrop, then panel over it,
 * then a row over that, then text.
 *
 * Solid surfaces cost one write per pixel and nothing else. No ramp to
 * look up, no second pass for a shadow, no read-modify-write for a
 * corner. Where the old design separated things with a bevel, this one
 * uses a single hairline or a change of surface colour; where it drew
 * attention with a gloss highlight, this one uses the accent. It reads
 * better at 256x192 as well - a 47-pixel gradient on a DS panel is mostly
 * dither noise, and a 1px bevel is mostly guesswork. */
/* The palette lives in a struct rather than in constants, so it can be
 * swapped at runtime.
 *
 * Nothing in the drawing code changes shape for this: every C_ name below
 * still reads like a colour literal, it just resolves to a field instead
 * of an immediate. The cost is a load where there used to be a constant,
 * and colours are read once per fill rather than per pixel, so it does
 * not show up anywhere that matters. */
typedef struct {
    uint16_t bg, surface, raised, raisedHi, hairline, trough;
    uint16_t accent, accentDeep;
    uint16_t text, textDim, textOnSel, green;
    /* Derived in ApplyPalette, not written in the table: see there. */
    uint16_t textFolder;
} SkinPalette;

static SkinPalette s_pal;

#define C_BG           (s_pal.bg)
#define C_SURFACE      (s_pal.surface)
#define C_RAISED       (s_pal.raised)
#define C_RAISED_HI    (s_pal.raisedHi)
#define C_HAIRLINE     (s_pal.hairline)
#define C_TROUGH       (s_pal.trough)
#define C_ACCENT       (s_pal.accent)
#define C_ACCENT_DEEP  (s_pal.accentDeep)
#define C_TEXT         (s_pal.text)
#define C_TEXT_DIM     (s_pal.textDim)
#define C_TEXT_ON_SEL  (s_pal.textOnSel)
#define C_TEXT_FOLDER  (s_pal.textFolder)
/* Green survives in three places and nowhere else: a healthy battery, a
 * glitch-free buffer on the Test page, and the middle of the level
 * meter's ramp. All three mean something - full, fine, and not yet loud -
 * and a meter that ran accent-to-red would lose the reading everyone
 * already knows how to take.
 *
 * Everywhere else it was decoration: progress bars, slider fills, the
 * playing marker, the sleep moon. Those are the theme's job, and left
 * green they fought whichever accent was chosen - which is exactly the
 * "mixed with green" the palette looked like. */
#define C_GREEN        (s_pal.green)

/* What each theme changes besides its colours.
 *
 * The palettes alone were not the difference between these looks: one
 * frames the top screen and another leaves it bare, one boxes every
 * transport button and another draws the bare glyph, one steps its
 * volume in blocks like an LED meter. Those are the details that make
 * Retro Green read as a terminal rather than as a green version of the
 * blue one, and they belong in the theme with the colours. */
typedef struct {
    uint8_t handleR;     /* corner radius of a slider handle: 2 square, 5 round */
    uint8_t tabsUpper;   /* labels in capitals */
    uint8_t tabsBoxed;   /* a frame around every tab, not just an underline */
    uint8_t btnChrome;   /* 0 bare glyph, 1 boxed, 2 one strip behind the row */
    uint8_t volSteps;    /* 0 smooth, otherwise how many blocks */
    uint8_t topFramed;   /* a border around the visualizer */
    /* What the top screen shows in this look - a TOP_* choice, so it can
     * be one of the cards and not only a meter. Some of these themes are
     * quiet, typographic things that a bouncing spectrum fights; those
     * ones open on a card instead, which is the difference between a
     * theme being a palette and a theme being a look. */
    uint8_t topChoice;
} SkinTraits;

static const SkinTraits kTraits[] = {
    /* Blue Classic  */ { 2, 0, 0, 1, 0, 0, VIS_EDGE    },
    /* Minimal Dark  */ { 5, 1, 0, 0, 0, 0, TOP_CARD(3) },
    /* Retro Green   */ { 2, 0, 1, 1, 14, 0, VIS_BLOCKS },
    /* Amber         */ { 2, 0, 0, 0, 0, 0, VIS_BARS    },
    /* Neon Purple   */ { 2, 0, 0, 2, 0, 0, VIS_PILLARS },
    /* Crimson       */ { 2, 0, 0, 1, 0, 0, VIS_BARS    },
    /* Teal          */ { 2, 0, 0, 1, 0, 0, VIS_MIRROR  },
    /* Slate         */ { 5, 0, 0, 0, 0, 0, TOP_CARD(0) },
    /* Rose          */ { 2, 0, 0, 1, 0, 0, VIS_BARS    },
    /* Sand          */ { 2, 0, 0, 1, 0, 0, VIS_PILLARS },

    /* Twenty more, and they differ in chrome as well as in colour -
     * the handle shape, the tab treatment, whether the volume reads as
     * blocks, what the top screen opens on. A set that varied only in
     * hue would be one theme with a colour picker on it. */
    /* Indigo  */ { 2, 0, 0, 1,  0, 0, VIS_BARS    },
    /* Ice     */ { 5, 0, 0, 0,  0, 1, TOP_CARD(1) },
    /* Mint    */ { 2, 0, 0, 1,  0, 0, VIS_MIRROR  },
    /* Lime    */ { 2, 0, 1, 1, 12, 0, VIS_BLOCKS  },
    /* Gold    */ { 2, 1, 0, 2,  0, 0, VIS_PILLARS },
    /* Ember   */ { 5, 0, 0, 1,  0, 1, VIS_BARS    },
    /* Scarlet */ { 2, 0, 0, 0,  0, 0, VIS_BARS    },
    /* Magenta */ { 5, 1, 0, 2,  0, 0, VIS_PILLARS },
    /* Violet  */ { 2, 0, 1, 1,  0, 0, VIS_BLOCKS  },
    /* Steel   */ { 5, 0, 0, 0,  0, 0, TOP_CARD(2) },
    /* Olive   */ { 2, 0, 0, 1, 10, 0, VIS_BLOCKS  },
    /* Plum    */ { 2, 0, 0, 1,  0, 1, VIS_MIRROR  },
    /* Coral   */ { 5, 0, 0, 1,  0, 0, VIS_PILLARS },
    /* Navy    */ { 2, 1, 0, 1,  0, 0, VIS_BARS    },
    /* Forest  */ { 2, 0, 1, 0, 14, 0, VIS_BLOCKS  },
    /* Copper  */ { 2, 0, 0, 2,  0, 1, VIS_BARS    },
    /* Paper   */ { 2, 0, 0, 1,  0, 0, TOP_CARD(1) },
    /* Frost   */ { 5, 0, 0, 0,  0, 0, VIS_WAVE    },
    /* Sepia   */ { 2, 1, 1, 1,  0, 0, TOP_CARD(3) },
    /* Mono    */ { 5, 1, 0, 0,  0, 0, TOP_CARD(2) },
};

_Static_assert(sizeof(kTraits) / sizeof(kTraits[0]) == SKIN_THEMES,
               "a theme has no chrome traits - it would read off the end of "
               "the table and take its shape from whatever followed");

#define TR (kTraits[s_theme])

/* Six complete looks. Each carries its own accent rather than taking one
 * from a separate list: these are palettes, not grounds, and half the
 * pairings a free choice allows are wrong - a red accent on the green
 * phosphor theme is not a theme anyone wanted.
 *
 * Fields in order: bg, surface, raised, raisedHi, hairline, trough,
 * accent, accentDeep (derived, left 0), text, textDim, textOnSel,
 * green, textFolder (derived, left 0).
 *
 * textOnSel is the one to watch: it is the ink that lands on the accent,
 * so on Minimal Dark - whose accent is nearly white - it has to be dark
 * rather than the white every other theme uses. */
static const SkinPalette kThemes[SKIN_THEMES] = {
    /* Blue Classic: the blue-leaning near-black this has always been. */
    { GFX_RGB8(0x0D,0x15,0x20), GFX_RGB8(0x13,0x1E,0x2C), GFX_RGB8(0x1E,0x2C,0x40),
      GFX_RGB8(0x2A,0x3C,0x55), GFX_RGB8(0x24,0x34,0x4A), GFX_RGB8(0x1C,0x28,0x3C),
      GFX_RGB8(0x2E,0x8B,0xE8), 0,
      GFX_RGB8(0xFF,0xFF,0xFF), GFX_RGB8(0x7C,0x9A,0xB8), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4C,0xD1,0x64), 0 },

    /* Minimal Dark: no hue at all. The accent is the paper rather than
     * the ink, which is why textOnSel goes dark here and nowhere else. */
    { GFX_RGB8(0x0A,0x0A,0x0C), GFX_RGB8(0x10,0x10,0x13), GFX_RGB8(0x1A,0x1A,0x1E),
      GFX_RGB8(0x26,0x26,0x2C), GFX_RGB8(0x20,0x20,0x26), GFX_RGB8(0x05,0x05,0x06),
      GFX_RGB8(0xE6,0xE6,0xEA), 0,
      GFX_RGB8(0xFF,0xFF,0xFF), GFX_RGB8(0x8A,0x8A,0x92), GFX_RGB8(0x0A,0x0A,0x0C),
      GFX_RGB8(0xB0,0xB0,0xB8), 0 },

    /* Retro Green: phosphor. The text is green too, not white on green -
     * that is the whole of the effect. */
    { GFX_RGB8(0x02,0x0A,0x04), GFX_RGB8(0x06,0x14,0x0A), GFX_RGB8(0x0C,0x20,0x10),
      GFX_RGB8(0x14,0x30,0x18), GFX_RGB8(0x10,0x28,0x14), GFX_RGB8(0x01,0x06,0x03),
      GFX_RGB8(0x3F,0xD8,0x48), 0,
      GFX_RGB8(0x86,0xF0,0x90), GFX_RGB8(0x4A,0x98,0x50), GFX_RGB8(0x02,0x12,0x06),
      GFX_RGB8(0x3F,0xD8,0x48), 0 },

    /* Amber: the same idea one lamp warmer. */
    { GFX_RGB8(0x0A,0x07,0x03), GFX_RGB8(0x14,0x0E,0x04), GFX_RGB8(0x20,0x18,0x06),
      GFX_RGB8(0x2E,0x22,0x08), GFX_RGB8(0x28,0x1E,0x06), GFX_RGB8(0x05,0x03,0x01),
      GFX_RGB8(0xF0,0xA0,0x20), 0,
      GFX_RGB8(0xFF,0xC8,0x5A), GFX_RGB8(0xA0,0x78,0x30), GFX_RGB8(0x14,0x0A,0x00),
      GFX_RGB8(0x90,0xC0,0x40), 0 },

    /* Neon Purple. */
    { GFX_RGB8(0x0E,0x06,0x18), GFX_RGB8(0x17,0x0A,0x26), GFX_RGB8(0x24,0x10,0x38),
      GFX_RGB8(0x34,0x18,0x50), GFX_RGB8(0x2C,0x14,0x44), GFX_RGB8(0x07,0x03,0x10),
      GFX_RGB8(0xA8,0x45,0xE0), 0,
      GFX_RGB8(0xE4,0xBA,0xFF), GFX_RGB8(0x8A,0x5E,0xB0), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x7E,0xD8,0x90), 0 },

    /* Crimson. */
    { GFX_RGB8(0x14,0x0A,0x0C), GFX_RGB8(0x1E,0x10,0x12), GFX_RGB8(0x2C,0x18,0x1C),
      GFX_RGB8(0x40,0x22,0x28), GFX_RGB8(0x38,0x1E,0x22), GFX_RGB8(0x2A,0x14,0x18),
      GFX_RGB8(0xE0,0x38,0x4C), 0,
      GFX_RGB8(0xFF,0xEC,0xEE), GFX_RGB8(0xB0,0x80,0x88), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4C,0xD1,0x64), 0 },

    /* Teal. */
    { GFX_RGB8(0x08,0x18,0x1A), GFX_RGB8(0x0E,0x24,0x26), GFX_RGB8(0x16,0x34,0x38),
      GFX_RGB8(0x20,0x4A,0x50), GFX_RGB8(0x1C,0x40,0x46), GFX_RGB8(0x12,0x30,0x34),
      GFX_RGB8(0x20,0xC0,0xB0), 0,
      GFX_RGB8(0xE8,0xFA,0xF8), GFX_RGB8(0x78,0xA8,0xA4), GFX_RGB8(0x04,0x20,0x1E),
      GFX_RGB8(0x4C,0xD1,0x64), 0 },

    /* Slate: the one with no hue in its accent either, for when colour
     * itself is the distraction. */
    { GFX_RGB8(0x14,0x17,0x1C), GFX_RGB8(0x1E,0x22,0x2A), GFX_RGB8(0x2C,0x32,0x3C),
      GFX_RGB8(0x40,0x48,0x54), GFX_RGB8(0x38,0x3E,0x4A), GFX_RGB8(0x26,0x2C,0x36),
      GFX_RGB8(0x7C,0x93,0xB0), 0,
      GFX_RGB8(0xF0,0xF2,0xF6), GFX_RGB8(0x8A,0x93,0xA2), GFX_RGB8(0x14,0x17,0x1C),
      GFX_RGB8(0x6C,0xC8,0x88), 0 },

    /* Rose. */
    { GFX_RGB8(0x18,0x0E,0x14), GFX_RGB8(0x24,0x14,0x1E), GFX_RGB8(0x34,0x1E,0x2C),
      GFX_RGB8(0x4A,0x2C,0x3E), GFX_RGB8(0x42,0x26,0x36), GFX_RGB8(0x2C,0x18,0x26),
      GFX_RGB8(0xF0,0x60,0x90), 0,
      GFX_RGB8(0xFC,0xE8,0xF0), GFX_RGB8(0xB8,0x80,0x98), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4C,0xD1,0x64), 0 },

    /* Sand: the one light theme. A DS backlight is bright enough that a
     * pale ground is genuinely easier in daylight, which is the one case
     * every dark theme here loses. */
    { GFX_RGB8(0xE8,0xE0,0xD0), GFX_RGB8(0xD8,0xCF,0xBC), GFX_RGB8(0xC6,0xBA,0xA4),
      GFX_RGB8(0xB2,0xA4,0x8C), GFX_RGB8(0xBA,0xAE,0x98), GFX_RGB8(0xCF,0xC5,0xB0),
      GFX_RGB8(0xB0,0x60,0x10), 0,
      GFX_RGB8(0x24,0x1C,0x10), GFX_RGB8(0x6A,0x5C,0x46), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x2A,0x70,0x38), 0 },


    /* Indigo. */
    { GFX_RGB8(0x11,0x10,0x17), GFX_RGB8(0x1C,0x1A,0x24), GFX_RGB8(0x2B,0x28,0x36),
      GFX_RGB8(0x41,0x3E,0x4F), GFX_RGB8(0x38,0x35,0x45), GFX_RGB8(0x23,0x21,0x2E),
      GFX_RGB8(0x58,0x42,0xEB), 0,
      GFX_RGB8(0xEE,0xEC,0xFA), GFX_RGB8(0x8D,0x88,0xA8), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Ice. */
    { GFX_RGB8(0x11,0x15,0x17), GFX_RGB8(0x1B,0x21,0x24), GFX_RGB8(0x2A,0x33,0x36),
      GFX_RGB8(0x3F,0x4B,0x4F), GFX_RGB8(0x36,0x41,0x45), GFX_RGB8(0x22,0x2B,0x2E),
      GFX_RGB8(0x24,0xD0,0xF2), 0,
      GFX_RGB8(0xED,0xF7,0xFA), GFX_RGB8(0x8B,0xA1,0xA8), GFX_RGB8(0x0A,0x0A,0x0C),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Mint. */
    { GFX_RGB8(0x11,0x17,0x15), GFX_RGB8(0x1C,0x24,0x21), GFX_RGB8(0x2B,0x36,0x32),
      GFX_RGB8(0x41,0x4F,0x4A), GFX_RGB8(0x38,0x45,0x41), GFX_RGB8(0x24,0x2E,0x2A),
      GFX_RGB8(0x36,0xD9,0x9D), 0,
      GFX_RGB8(0xEF,0xFA,0xF6), GFX_RGB8(0x8E,0xA8,0x9F), GFX_RGB8(0x0A,0x0A,0x0C),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Lime. */
    { GFX_RGB8(0x14,0x17,0x12), GFX_RGB8(0x20,0x24,0x1D), GFX_RGB8(0x30,0x36,0x2C),
      GFX_RGB8(0x48,0x4F,0x42), GFX_RGB8(0x3E,0x45,0x39), GFX_RGB8(0x29,0x2E,0x25),
      GFX_RGB8(0x87,0xE0,0x22), 0,
      GFX_RGB8(0xF4,0xFA,0xF0), GFX_RGB8(0x9B,0xA8,0x91), GFX_RGB8(0x0A,0x0A,0x0C),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Gold. */
    { GFX_RGB8(0x17,0x16,0x11), GFX_RGB8(0x24,0x22,0x1C), GFX_RGB8(0x36,0x33,0x2B),
      GFX_RGB8(0x4F,0x4B,0x41), GFX_RGB8(0x45,0x42,0x38), GFX_RGB8(0x2E,0x2B,0x24),
      GFX_RGB8(0xF2,0xB5,0x0C), 0,
      GFX_RGB8(0xFA,0xF7,0xEF), GFX_RGB8(0xA8,0xA2,0x8E), GFX_RGB8(0x0A,0x0A,0x0C),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Ember. */
    { GFX_RGB8(0x17,0x13,0x10), GFX_RGB8(0x24,0x1D,0x1A), GFX_RGB8(0x36,0x2D,0x28),
      GFX_RGB8(0x4F,0x44,0x3E), GFX_RGB8(0x45,0x3B,0x35), GFX_RGB8(0x2E,0x26,0x21),
      GFX_RGB8(0xF2,0x61,0x18), 0,
      GFX_RGB8(0xFA,0xF1,0xEC), GFX_RGB8(0xA8,0x94,0x88), GFX_RGB8(0x0A,0x0A,0x0C),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Scarlet. */
    { GFX_RGB8(0x17,0x10,0x10), GFX_RGB8(0x24,0x1A,0x1A), GFX_RGB8(0x36,0x29,0x28),
      GFX_RGB8(0x4F,0x3E,0x3E), GFX_RGB8(0x45,0x35,0x35), GFX_RGB8(0x2E,0x22,0x21),
      GFX_RGB8(0xEB,0x2A,0x23), 0,
      GFX_RGB8(0xFA,0xED,0xEC), GFX_RGB8(0xA8,0x89,0x88), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Magenta. */
    { GFX_RGB8(0x17,0x11,0x15), GFX_RGB8(0x24,0x1B,0x21), GFX_RGB8(0x36,0x2A,0x32),
      GFX_RGB8(0x4F,0x3F,0x4A), GFX_RGB8(0x45,0x36,0x41), GFX_RGB8(0x2E,0x22,0x2A),
      GFX_RGB8(0xF0,0x30,0xB6), 0,
      GFX_RGB8(0xFA,0xED,0xF6), GFX_RGB8(0xA8,0x8B,0x9F), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Violet. */
    { GFX_RGB8(0x14,0x11,0x17), GFX_RGB8(0x20,0x1B,0x24), GFX_RGB8(0x31,0x2A,0x36),
      GFX_RGB8(0x48,0x3F,0x4F), GFX_RGB8(0x3F,0x36,0x45), GFX_RGB8(0x29,0x22,0x2E),
      GFX_RGB8(0xAE,0x46,0xEB), 0,
      GFX_RGB8(0xF5,0xED,0xFA), GFX_RGB8(0x9C,0x8B,0xA8), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Steel. */
    { GFX_RGB8(0x13,0x15,0x17), GFX_RGB8(0x1F,0x21,0x24), GFX_RGB8(0x2F,0x32,0x36),
      GFX_RGB8(0x46,0x4A,0x4F), GFX_RGB8(0x3D,0x40,0x45), GFX_RGB8(0x27,0x2A,0x2E),
      GFX_RGB8(0x70,0x9B,0xCC), 0,
      GFX_RGB8(0xF3,0xF6,0xFA), GFX_RGB8(0x98,0x9F,0xA8), GFX_RGB8(0x0A,0x0A,0x0C),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Olive. */
    { GFX_RGB8(0x16,0x17,0x13), GFX_RGB8(0x22,0x24,0x1D), GFX_RGB8(0x34,0x36,0x2D),
      GFX_RGB8(0x4D,0x4F,0x44), GFX_RGB8(0x43,0x45,0x3B), GFX_RGB8(0x2C,0x2E,0x26),
      GFX_RGB8(0xA6,0xB8,0x37), 0,
      GFX_RGB8(0xF8,0xFA,0xF1), GFX_RGB8(0xA4,0xA8,0x94), GFX_RGB8(0x0A,0x0A,0x0C),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Plum. */
    { GFX_RGB8(0x17,0x11,0x17), GFX_RGB8(0x24,0x1C,0x24), GFX_RGB8(0x36,0x2B,0x36),
      GFX_RGB8(0x4F,0x41,0x4F), GFX_RGB8(0x45,0x38,0x45), GFX_RGB8(0x2E,0x24,0x2E),
      GFX_RGB8(0xAD,0x4E,0xAD), 0,
      GFX_RGB8(0xFA,0xEF,0xFA), GFX_RGB8(0xA8,0x8E,0xA8), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Coral. */
    { GFX_RGB8(0x17,0x13,0x12), GFX_RGB8(0x24,0x1E,0x1C), GFX_RGB8(0x36,0x2D,0x2B),
      GFX_RGB8(0x4F,0x44,0x42), GFX_RGB8(0x45,0x3B,0x39), GFX_RGB8(0x2E,0x26,0x24),
      GFX_RGB8(0xFA,0x73,0x57), 0,
      GFX_RGB8(0xFA,0xF2,0xEF), GFX_RGB8(0xA8,0x95,0x90), GFX_RGB8(0x0A,0x0A,0x0C),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Navy. */
    { GFX_RGB8(0x0F,0x11,0x17), GFX_RGB8(0x19,0x1B,0x24), GFX_RGB8(0x27,0x2A,0x36),
      GFX_RGB8(0x3C,0x41,0x4F), GFX_RGB8(0x33,0x38,0x45), GFX_RGB8(0x20,0x23,0x2E),
      GFX_RGB8(0x28,0x52,0xC7), 0,
      GFX_RGB8(0xEB,0xEE,0xFA), GFX_RGB8(0x85,0x8D,0xA8), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Forest. */
    { GFX_RGB8(0x11,0x17,0x13), GFX_RGB8(0x1B,0x24,0x1E), GFX_RGB8(0x2A,0x36,0x2E),
      GFX_RGB8(0x3F,0x4F,0x45), GFX_RGB8(0x36,0x45,0x3C), GFX_RGB8(0x22,0x2E,0x27),
      GFX_RGB8(0x20,0x9E,0x46), 0,
      GFX_RGB8(0xED,0xFA,0xF2), GFX_RGB8(0x8B,0xA8,0x96), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Copper. */
    { GFX_RGB8(0x17,0x14,0x11), GFX_RGB8(0x24,0x1F,0x1B), GFX_RGB8(0x36,0x2F,0x2A),
      GFX_RGB8(0x4F,0x47,0x40), GFX_RGB8(0x45,0x3D,0x37), GFX_RGB8(0x2E,0x28,0x23),
      GFX_RGB8(0xBD,0x70,0x35), 0,
      GFX_RGB8(0xFA,0xF4,0xEE), GFX_RGB8(0xA8,0x99,0x8C), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x4F,0xD1,0x7B), 0 },
    /* Paper. */
    { GFX_RGB8(0xF2,0xF0,0xEB), GFX_RGB8(0xE0,0xDD,0xD6), GFX_RGB8(0xC9,0xC5,0xBC),
      GFX_RGB8(0xB2,0xAE,0xA4), GFX_RGB8(0xBA,0xB5,0xAC), GFX_RGB8(0xD6,0xD2,0xCB),
      GFX_RGB8(0xB8,0x5A,0x1C), 0,
      GFX_RGB8(0x29,0x27,0x22), GFX_RGB8(0x70,0x6C,0x63), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x1F,0x7A,0x3D), 0 },
    /* Frost. */
    { GFX_RGB8(0xEA,0xEF,0xF2), GFX_RGB8(0xD4,0xDB,0xE0), GFX_RGB8(0xBA,0xC3,0xC9),
      GFX_RGB8(0xA1,0xAB,0xB2), GFX_RGB8(0xA9,0xB3,0xBA), GFX_RGB8(0xC9,0xD1,0xD6),
      GFX_RGB8(0x10,0x63,0x9E), 0,
      GFX_RGB8(0x21,0x26,0x29), GFX_RGB8(0x60,0x6A,0x70), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x1F,0x7A,0x3D), 0 },
    /* Sepia. */
    { GFX_RGB8(0xF2,0xEC,0xE5), GFX_RGB8(0xE0,0xD7,0xCD), GFX_RGB8(0xC9,0xBE,0xB1),
      GFX_RGB8(0xB2,0xA6,0x97), GFX_RGB8(0xBA,0xAE,0xA0), GFX_RGB8(0xD6,0xCC,0xC1),
      GFX_RGB8(0x85,0x3A,0x1B), 0,
      GFX_RGB8(0x29,0x23,0x1C), GFX_RGB8(0x70,0x65,0x58), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x1F,0x7A,0x3D), 0 },
    /* Mono. */
    { GFX_RGB8(0xF2,0xF2,0xF2), GFX_RGB8(0xE0,0xE0,0xE0), GFX_RGB8(0xC9,0xC9,0xC9),
      GFX_RGB8(0xB2,0xB2,0xB2), GFX_RGB8(0xBA,0xBA,0xBA), GFX_RGB8(0xD6,0xD6,0xD6),
      GFX_RGB8(0x33,0x33,0x33), 0,
      GFX_RGB8(0x29,0x29,0x29), GFX_RGB8(0x70,0x70,0x70), GFX_RGB8(0xFF,0xFF,0xFF),
      GFX_RGB8(0x1F,0x7A,0x3D), 0 },
};


/* Asserted rather than trusted: a table that has grown past the count
 * beside it reads and writes off the end, and the last time that happened
 * here the symptom was unrelated controls misbehaving rather than
 * anything pointing at the table. */
_Static_assert(sizeof(kThemes) / sizeof(kThemes[0]) == SKIN_THEMES,
               "theme table and SKIN_THEMES disagree");

static int s_theme = 0;

/* The console's own colours, for the accent.
 *
 * Every shade a DS or DSi shipped in. They are here as a palette rather
 * than as a theme because a theme is a whole scheme - chrome, ground and
 * text together - and this is one colour inside it: the accent that marks
 * the selected row, fills the progress bar and lights the meter. Picking
 * a console colour recolours those without disturbing anything the theme
 * decided about legibility.
 *
 * -1 means the theme keeps its own accent, which is the default and what
 * every theme was designed around. */
static int s_accent = -1;

static const uint16_t kAccents[SKIN_ACCENTS] = {
    GFX_RGB8(0xA7, 0xA9, 0xAC), GFX_RGB8(0x00, 0x7B, 0xC3), GFX_RGB8(0xF4, 0xA6, 0xC1), GFX_RGB8(0xE8, 0xA0, 0xB8),
    GFX_RGB8(0xF2, 0xF2, 0xF0), GFX_RGB8(0x25, 0x25, 0x25), GFX_RGB8(0xB8, 0x20, 0x25), GFX_RGB8(0x00, 0x8C, 0x95),
    GFX_RGB8(0x8C, 0xC6, 0x3F), GFX_RGB8(0x8C, 0xC9, 0xE8), GFX_RGB8(0x17, 0x2C, 0x4F), GFX_RGB8(0xE8, 0x9B, 0xAE),
    GFX_RGB8(0xB7, 0x7B, 0x88), GFX_RGB8(0xA6, 0xA9, 0xAD), GFX_RGB8(0xB5, 0x1F, 0x2A), GFX_RGB8(0x00, 0x5B, 0x9A),
    GFX_RGB8(0x36, 0xB8, 0xC2), GFX_RGB8(0x75, 0xB8, 0x43), GFX_RGB8(0xF1, 0xF1, 0xED), GFX_RGB8(0x20, 0x20, 0x20),
    GFX_RGB8(0x83, 0xC7, 0xE8), GFX_RGB8(0xE6, 0x9A, 0xAF), GFX_RGB8(0x3E, 0x71, 0x8F), GFX_RGB8(0x4B, 0x86, 0xA8),
    GFX_RGB8(0x82, 0xB8, 0x3D), GFX_RGB8(0x4A, 0x34, 0x28), GFX_RGB8(0x6E, 0x1F, 0x2B), GFX_RGB8(0xF1, 0xF0, 0xE8),
    GFX_RGB8(0x25, 0x4A, 0x67), GFX_RGB8(0x5C, 0x8A, 0x45), GFX_RGB8(0xE5, 0xC9, 0x4A), GFX_RGB8(0xB5, 0x2A, 0x2F),
};

int      Skin_AccentCount(void)     { return SKIN_ACCENTS; }
uint16_t Skin_AccentColor(int i)
{
    return (i >= 0 && i < SKIN_ACCENTS) ? kAccents[i] : 0;
}
int      Skin_Accent(void)          { return s_accent; }

/* Perceived brightness of a colour, 0..31.
 *
 * The DS stores five bits a channel, so this works in those directly
 * rather than converting: the weights are the usual luma coefficients
 * scaled to 8 bits and shifted back down. */
static int Luma(uint16_t c)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r * 77 + g * 151 + b * 28) >> 8;
}

/* The ink a theme asked for, unless it cannot be read on that ground.
 *
 * Themes pick their own ink for the accent and all ten pick well - the
 * closest is 13 apart, and this leaves anything past 12 alone. What it
 * catches is an accent the theme never saw: pick #F2F2F0 off the colour
 * page, at luminance 30, and a theme whose ink is white puts white text
 * on white. The label does not look wrong, it disappears.
 *
 * It also fixes one that was always wrong. Minimal Dark's accent is
 * near-white by design, and the active segment of a choice row drew its
 * label in C_TEXT rather than the dark ink the theme had specified for
 * exactly that ground. */
static uint16_t ReadableInk(uint16_t ground, uint16_t preferred)
{
    int gl = Luma(ground), pl = Luma(preferred);
    int gap = gl > pl ? gl - pl : pl - gl;
    if (gap >= 12) return preferred;

    return (gl >= 16) ? GFX_RGB8(0x0A, 0x0A, 0x0C)
                      : GFX_RGB8(0xFF, 0xFF, 0xFF);
}

static void ApplyPalette(void)
{
    s_pal = kThemes[s_theme];

    /* Applied over the theme, not instead of it: everything else the
     * theme chose still holds. */
    if (s_accent >= 0 && s_accent < SKIN_ACCENTS)
        s_pal.accent = kAccents[s_accent];

    /* After the accent, whichever set it: the ink has to answer to the
     * colour actually in use. */
    s_pal.textOnSel = ReadableInk(s_pal.accent, s_pal.textOnSel);
    /* A darker companion for the pressed state, derived so a new accent
     * never needs a second colour picked by hand. */
    s_pal.accentDeep = Gfx_Lerp(s_pal.accent, GFX_RGB8(0, 0, 0), 1, 2);

    /* And the folder mark, in the theme's own colour.
     *
     * This was written out per theme and twenty of the thirty had the
     * same gold in them, so a folder was yellow on a green console and
     * yellow on a red one - the one thing on the page that had not
     * heard which theme was on. Derived, it cannot drift out of step
     * again, and an accent chosen on the Setup page takes the folders
     * with it.
     *
     * A third of the way from the accent towards the ink, so it belongs
     * to the list rather than shouting over it and is still plainly not
     * a file name. On a theme whose accent is already nearly the ink -
     * Minimal Dark, whose accent is the paper - that lands on the text
     * colour and the mark disappears into the row, so it goes the other
     * way instead, towards the ground. Five is about a sixth of the
     * luma range, which is the point where the two stop being
     * separable at this size. */
    const uint16_t lifted = Gfx_Lerp(s_pal.accent, s_pal.text, 1, 3);
    const int gap = Luma(lifted) - Luma(s_pal.text);

    s_pal.textFolder = (gap < 5 && gap > -5)
                     ? Gfx_Lerp(s_pal.accent, s_pal.bg, 1, 3)
                     : lifted;
}

void Skin_SetTheme(int theme)
{
    s_theme = (theme >= 0 && theme < SKIN_THEMES) ? theme : 0;

    /* A new theme brings its own accent with it. Carrying the old choice
     * across would mean picking a theme and not getting the thing it was
     * designed to look like - and the accent is the colour a theme is
     * recognised by, so it would not look like the one chosen either.
     * Startup restores a saved choice by calling Skin_SetAccent after
     * this, which is why the order there matters. */
    s_accent = -1;

    ApplyPalette();
}

/* Anything outside the table means "the theme's own", so a stored value
 * from a build with a different table cannot select a colour at random. */
void Skin_SetAccent(int index)
{
    s_accent = (index >= 0 && index < SKIN_ACCENTS) ? index : -1;
    ApplyPalette();
}

uint16_t Skin_ColorBg(void)       { return s_pal.bg; }
uint16_t Skin_ColorSurface(void)  { return s_pal.surface; }
uint16_t Skin_ColorAccent(void)   { return s_pal.accent; }
uint16_t Skin_ColorText(void)     { return s_pal.text; }
uint16_t Skin_ColorHairline(void) { return s_pal.hairline; }
uint16_t Skin_ColorRaised(void)   { return s_pal.raised; }
uint16_t Skin_ColorTrough(void)   { return s_pal.trough; }
uint16_t Skin_ColorTextDim(void)  { return s_pal.textDim; }
uint16_t Skin_ColorTextOnSel(void){ return s_pal.textOnSel; }

int Skin_Theme(void)  { return s_theme; }

int Skin_ThemeTopChoice(int i)
{
    return (i >= 0 && i < SKIN_THEMES) ? kTraits[i].topChoice : VIS_BARS;
}

const char *Skin_ThemeName(int i)
{
    /* Deliberately unsized. At [SKIN_THEMES] a short list still
     * compiles and quietly fills the rest with NULL, which reaches
     * the text blitter as a name. */
    static const char *const kNames[] = {
        "Blue", "Dark", "Green", "Amber", "Purple",
        "Crimson", "Teal", "Slate", "Rose", "Sand",
        "Indigo", "Ice", "Mint", "Lime", "Gold", "Ember", "Scarlet", "Magenta", "Violet", "Steel", "Olive", "Plum", "Coral", "Navy", "Forest", "Copper", "Paper", "Frost", "Sepia", "Mono"
    };
    _Static_assert(sizeof(kNames) / sizeof(kNames[0]) == SKIN_THEMES,
                   "a theme has no name");
    return (i >= 0 && i < SKIN_THEMES) ? kNames[i] : "?";
}


/* The old names the drawing code still speaks in, mapped onto the flat
 * set. Keeping them means the layout code did not have to be rewritten
 * alongside the look, so the two changes stayed reviewable apart. */
#define C_FRAME        C_HAIRLINE
#define C_TITLE_GLOSS  C_ACCENT
#define C_TITLE_TOP    C_SURFACE
#define C_TITLE_MID    C_SURFACE
#define C_TITLE_BOT    C_SURFACE
#define C_PANEL_TOP    C_BG
#define C_PANEL_BOT    C_BG
#define C_LIST_TOP     C_BG
#define C_LIST_BOT     C_BG
#define C_ROW_ALT      C_SURFACE
#define C_SEL_TOP      C_ACCENT
#define C_SEL_BOT      C_ACCENT
#define C_SHADOW       GFX_NO_SHADOW
#define C_BTN_GLOSS    C_RAISED_HI
#define C_BTN_TOP      C_RAISED
#define C_BTN_MID      C_RAISED
#define C_BTN_BOT      C_RAISED
#define C_BTN_DOWN_TOP C_ACCENT_DEEP
#define C_BTN_DOWN_BOT C_ACCENT_DEEP
#define C_EDGE_LIGHT   C_HAIRLINE
#define C_EDGE_DARK    C_HAIRLINE
#define C_TROUGH_TOP   C_TROUGH
#define C_TROUGH_BOT   C_TROUGH
#define C_GREEN_TOP    C_GREEN
#define C_GREEN_BOT    C_GREEN
#define C_BLUE_TOP     C_ACCENT
#define C_BLUE_BOT     C_ACCENT

/* ---- layout ---- */
#define TITLE_H       18
/* Blue Classic has a narrow inset around the complete lower screen.  The
 * navigation sits inside that bezel, rather than being pinned to the DS
 * screen edge.  The other looks use the same geometry, which also makes
 * their touch targets consistent. */
/* The top screen's bar. Named because the visualizer's top margin has
 * to equal it, and the frame is drawn before the function's local
 * copy exists. */
/* The top screen's header, band by band. The name row holds one line,
 * the block under it holds two, and each is only as tall as it needs.
 * TOP_HEADER_H must equal VIS_TOP_MARGIN or the meter paints under the
 * header or leaves a strip nothing owns. */
#define TOP_HEADER_H  32
#define TOP_LINE1_Y   5
#define TOP_LINE2_Y   19

/* Where the meter's floor sits, from visualizer.c: SCREEN_H - 30. */
#define TOP_FLOOR_Y   (GFX_H - 22)

#define TOP_BAR_H     TOP_HEADER_H

/* The tab band, and it is as short as it can be read at.
 *
 * It used to start seven rows down and stand nineteen tall, which put
 * eight rows of nothing above a label eight pixels high. Then three and
 * sixteen. Now none and sixteen: the band starts at the top edge, and
 * what is above a tab is the tab's own padding rather than a strip of
 * screen belonging to nothing.
 *
 * Those three rows were invisible in the sense that mattered least -
 * every theme paints them the same colour as the band, so there is no
 * seam to see - and perfectly visible as the gap they left above the
 * labels. They go to the page, like the eight before them: every page is
 * anchored to CONTENT_Y, including Now Playing, whose title band is
 * measured from it. */
#define TABS_Y        0
#define TABS_H        16
#define CONTENT_Y     18

/* Four tabs of equal width, laid out from one span.
 *
 * They used to be four hand-written widths - 50, 58, 48, 46 - starting at
 * 12, so the gaps between the labels came out different every time
 * because each label was centred in a box of a different size. Equal
 * shares of one span puts the same air on both sides of every label. The
 * span stops clear of the heart at 236. */
/* Six tabs, drawn as icons, with the selected one carrying its name.
 *
 * Six labels across 248 pixels fit, but only just - 41 each against a
 * widest label of 37 - and a row of six words is a lot of reading for
 * something the eye should land on. Icons are quicker to find and worse
 * to learn: a moon could be night mode, a clock could be a timer.
 *
 * So the tab you are on says what it is and the rest are marks. Nothing
 * has to be learned, because the name of wherever you are standing is
 * always on screen, and moving between them teaches the icons as you go.
 *
 * The strip is laid out around the selection rather than into fixed
 * cells: five icons and one icon-with-name is about 180 pixels, so it is
 * centred in the bar with room to spare. TabLayout is the single place
 * that decides this, and both the drawing and the hit test call it with
 * the same active tab - written twice they would disagree the moment a
 * label changed width, and a tab would answer taps beside itself. */
#define TAB_X0        4
#define TAB_SPAN      248
#define TAB_COUNT     6
#define TAB_W         (TAB_SPAN / TAB_COUNT)

#define TAB_ICON_W    13    /* the widest of them, which is the heart */
#define TAB_ICON_PAD   5
#define TAB_NAME_GAP   4

static const char *const kTabNames[TAB_COUNT] = {
    "Files", "Faves", "Playing", "Sleep", "Setup", "About"
};

/* Which tab lights up. Test is reached from Setup and has no tab of its
 * own, so it lights Setup's - otherwise the strip would show nothing
 * selected and the name would vanish from the one page that has no other
 * heading. */
static int TabActiveIndex(SkinTab active)
{
    if (active == SKIN_TAB_TEST) return (int)SKIN_TAB_SETUP;
    /* Compared as an int and only against the upper bound: SkinTab is an
     * unsigned enum here, so "< 0" is a comparison the compiler can
     * answer without looking - and warns about, rightly. */
    if ((unsigned)active >= (unsigned)TAB_COUNT) return (int)SKIN_TAB_FILES;
    return (int)active;
}


#define TAB_AT(i)     (TAB_X0 + (i) * TAB_W)

/* One plate behind the whole control row, its five controls spaced
 * across it in equal shares. Up here rather than beside the rest of the
 * Now Playing layout because the button rectangles below are cut from it. */
#define STACK_BOX_X     20
#define STACK_BOX_Y     110
#define STACK_BOX_W     216
#define STACK_BOX_H     40

/* Each transport control is its whole share of the plate, not a 24-pixel
 * square in the middle of one.
 *
 * They were centred glyphs with a rectangle the size of the glyph, so
 * four fifths of every cell looked like a button and did nothing: a tap
 * that landed anywhere but the middle was swallowed. It also meant the
 * pressed highlight was a small square floating in a large cell, which
 * is a poor answer to "which one did I just press". Cut from the plate,
 * the whole cell is the target and the whole cell lights up.
 *
 * The division is by fifths of the plate width so the cells tile it
 * exactly, with a pixel of air each side rather than a gap that has to
 * be kept in step by hand. */
/* Inset from the plate, not flush with it. Dividing the full width into
 * fifths put the first cell's left edge and the last cell's right edge
 * exactly on the plate's inner border, so shuffle and repeat lit right up
 * against it with no air either side while the three between them had
 * gaps. The cells are cut from the inset span instead, which gives the
 * two ends the same margin as everything else. */
#define TBTN_INSET  4
#define TBTN_SPAN   (STACK_BOX_W - 2 * TBTN_INSET)
/* The same four pixels vertically as horizontally. The plate's inside
 * runs from STACK_BOX_Y + 1 to STACK_BOX_Y + STACK_BOX_H - 1, so this
 * leaves four above and four below. It was three and three against a
 * side inset of four, which reads as a highlight sitting slightly proud
 * of its cell rather than as a cell with an even margin. */
#define TBTN_Y      (STACK_BOX_Y + 1 + TBTN_INSET)
#define TBTN_H      (STACK_BOX_H - 2 - 2 * TBTN_INSET)
/* The like heart, in the corner above the title.
 *
 * It sits in the band between the tab bar and the title block, which is
 * the one part of this page nothing else can reach into - the title's
 * top edge is computed from the space it needs, and the assert further
 * down pins the worst case. Horizontally it is clear of everything
 * because that band has nothing else in it at all. */
#define LIKE_X       234
#define LIKE_Y        20

/* The target is a good deal larger than the heart drawn in it.
 *
 * Twenty-six by fourteen was about the size of the glyph, and a stylus
 * does not land where it is aimed to within a pixel - so liking a track
 * took two or three tries. Thirty-six by twenty was better and still not
 * enough: this is the top corner of the screen, next to the hinge, which
 * is the hardest part of it to reach squarely. Forty-eight by twenty-one
 * is the whole band between the tab strip and the title, and it is as
 * large as it can be - the assertion below is what stops it growing into
 * the title, and it is one row from firing.
 *
 * Growing it downward also opens the gap under it, because the title's
 * top edge is computed from this and not from a number of its own: the
 * title cannot be pushed into the heart's target, whatever either of
 * them is changed to. */
#define LIKE_HIT_X   208
#define LIKE_HIT_Y   CONTENT_Y
#define LIKE_HIT_W    48
#define LIKE_HIT_H    21

#define TBTN_CELLS  5
#define TBTN_X(i)   (STACK_BOX_X + TBTN_INSET + (TBTN_SPAN * (i)) / TBTN_CELLS + 1)
#define TBTN_W(i)   ((TBTN_SPAN * ((i) + 1)) / TBTN_CELLS - \
                     (TBTN_SPAN * (i)) / TBTN_CELLS - 2)
#define CONTENT_H     (GFX_H - CONTENT_Y)
#define TRANSPORT_Y   143   /* only the Now Playing page has controls */

/* A path bar above the list: without it there is no way to tell which
 * folder you are looking at, which matters as soon as there is more than
 * one album on the card. */
#define PATH_X        4
#define PATH_Y        (CONTENT_Y + 1)
#define PATH_W        248
#define PATH_H        13

#define LIST_X        4
#define LIST_Y        (CONTENT_Y + 16)
#define LIST_W        238
/* Row height follows the chosen text size: one line of type plus its
 * padding, doubled when the type is. */
/* Settings rows: caption, then the control, then air.
 *
 * The rows are 27 apart and the caption is 8 tall, so the numbers below
 * are the whole layout. A setting is a caption and a control that belong
 * together, and what separates one setting from the next is the gap - so
 * the gap inside a row is one pixel and the gap between rows is six,
 * with the rule sitting in the middle of it.
 *
 * It used to be the other way round: two inside, five between, and a
 * slider that overhung its own rect by two more. */
#define ROW_CTRL_DY   9
#define ROW_CTRL_H    11
#define ROW_RULE_DY   26

#define ROW_H         (kListSizes[s_listSize].rowH)
/* +2 so the rows sit exactly inside the panel's 1px border rather than
 * having the last one clipped by it. */
#define LIST_H        (FileBrowser_VisibleRows() * ROW_H + 2)
#define SCROLL_X      246
#define SCROLL_W      6

/* sleep page */
/* The sleep page.
 *
 * The duration used to be a slider from one minute to a hundred and
 * twenty: a hundred and twenty values across two hundred pixels of
 * resistive screen, to pick a number people only ever choose five of.
 * Worse, it sat under a countdown showing a different number, so the page
 * carried two times and no clue which one you were setting.
 *
 * Presets instead - five chips, each an exact value and a target a thumb
 * can hit. The countdown becomes the one large thing on the page with a
 * bar beneath it showing how much has run, which is the question someone
 * opening this page is actually asking. The d-pad still trims by one and
 * by ten, so in-between values remain reachable and an old config still
 * works; its chip simply is not lit.
 *
 * Every position is measured from CONTENT_Y. Three of them used to be
 * measured from the top of the screen, so shortening the tab band moved
 * half the page and left the rest: the slider ended up inside the
 * countdown and the last control finished 64 pixels short of the bottom. */
#define SLEEP_PRESETS   5

/* Letter advance, not size - Gfx_TextBig takes (scale, advance) in that
 * order and the countdown has always been drawn at scale 2, sixteen
 * rows. Seven put a gap between digits wide enough to read as spaced
 * lettering; six closes it up so the time reads as one number. */
#define SLEEP_TIME_ADV  6
#define SLEEP_TIME_Y    (CONTENT_Y +  10)
#define SLEEP_BAR_X     16
#define SLEEP_BAR_W     224
/* The disc reaches about seven rows either side of the groove, so this
 * band is really fifteen tall rather than the eight it is drawn with -
 * spacing it as eight is what put it hard against the countdown. */
#define SLEEP_BAR_Y     (CONTENT_Y +  35)
#define SLEEP_BAR_H     PROGRESS_H
#define SLEEP_CHIP_Y    (CONTENT_Y +  58)
#define SLEEP_CHIP_H    26
#define SLEEP_CHIP_X0   10
#define SLEEP_CHIP_GAP  4
#define SLEEP_CHIP_W    ((236 - (SLEEP_PRESETS - 1) * SLEEP_CHIP_GAP) / SLEEP_PRESETS)
#define SLEEP_CHIP_X(i) (SLEEP_CHIP_X0 + (i) * (SLEEP_CHIP_W + SLEEP_CHIP_GAP))

_Static_assert(SLEEP_CHIP_X(SLEEP_PRESETS - 1) + SLEEP_CHIP_W <= GFX_W - 8,
               "the preset chips run off the right of the screen");

static const uint8_t kSleepPresets[SLEEP_PRESETS] = { 15, 30, 45, 60, 90 };

/* Defined further down with the other slider drawing; the sleep page's
 * elapsed bar is drawn with it so the two look like the same control. */
static void DrawSliderBar(int x, int y, int w, int h, int permille);

/* Elapsed, bar, total - on one line, so the number and the thing it
 * describes are read together instead of the eye travelling between a bar
 * and a pair of digits underneath it at opposite corners. The clocks are
 * monospaced and fixed-width, so the bar between them never moves as the
 * digits change. */
#define CLOCK_W       30            /* "-88:88" at 6px a digit */
#define PROGRESS_X    (6 + CLOCK_W + 4)
#define PROGRESS_Y    176
#define PROGRESS_W    (244 - 2 * (CLOCK_W + 4))
#define PROGRESS_H    8

#define VOL_TRACK_X   146
#define VOL_TRACK_Y   153
#define VOL_TRACK_W   82
#define VOL_TRACK_H   8

typedef struct { int x, y, w, h; } SkinRect;

static const SkinRect kRects[SKIN_BTN_COUNT] = {
    /* Stop is gone: pause already stops the sound and the only thing it
     * did beyond that was forget the position, which is not worth a
     * quarter of the transport row. Its width went to the volume, which
     * is now long enough to be aimed at rather than nudged.
     *
     * Twenty rows tall rather than twenty-four, which buys four pixels
     * of air above the row and keeps the glyphs the size they were. */
    /* Five cells of the same size at the same pitch, so the row reads as
     * one set of controls rather than three in the middle with two
     * strays at the ends. Shuffle and repeat are part of the transport
     * in the reference, not decorations beside it. */
    [SKIN_BTN_PREV]           = { TBTN_X(1), TBTN_Y, TBTN_W(1), TBTN_H },
    [SKIN_BTN_PLAYPAUSE]      = { TBTN_X(2), TBTN_Y, TBTN_W(2), TBTN_H },
    [SKIN_BTN_NEXT]           = { TBTN_X(3), TBTN_Y, TBTN_W(3), TBTN_H },
    /* Outside the volume track, so the track can be exactly as long as
     * the progress bar above it. */
    [SKIN_BTN_VOL_DOWN]       = {  16, 158,  16, 16 },
    [SKIN_BTN_VOL_UP]         = { 224, 158,  16, 16 },
    /* Five tabs now, so they are tighter than they were. Widths follow
     * the label lengths rather than being uniform, which keeps the two
     * longest ones from being the only cramped pair. */
    /* The tab row is the top of the screen now - there is no title bar
     * above it. A second bar saying the name of the program on every
     * page is eighteen pixels spent on something the user already knows,
     * and the reference does not have one.
     *
     * Files carries a folder mark, so it is a little wider than the
     * others. The two icon controls sit at the right-hand end of the
     * same row. */
    [SKIN_BTN_TAB_FILES]      = { TAB_AT(0), TABS_Y, TAB_W, TABS_H },
    [SKIN_BTN_TAB_FAVES]      = { TAB_AT(1), TABS_Y, TAB_W, TABS_H },
    [SKIN_BTN_TAB_NOWPLAYING] = { TAB_AT(2), TABS_Y, TAB_W, TABS_H },
    [SKIN_BTN_TAB_SLEEP]      = { TAB_AT(3), TABS_Y, TAB_W, TABS_H },
    [SKIN_BTN_TAB_SETUP]      = { TAB_AT(4), TABS_Y, TAB_W, TABS_H },
    [SKIN_BTN_TAB_ABOUT]      = { TAB_AT(5), TABS_Y, TAB_W, TABS_H },
    /* No rectangle: reached from a Setup row, left by tapping any tab. */
    [SKIN_BTN_TAB_TEST]       = {   0,      0,   0,      0 },

    /* Two small controls in the path bar. There is no spare console
     * button on the Files page - every one is already a transport or a
     * navigation key - so building a playlist has to be reachable by
     * stylus, and the path bar is the only strip on the page that is not
     * the list itself. */
    /* Shifted right by 28 when the battery gauge was removed, so the
     * right-hand controls end on the margin the gauge used to hold rather
     * than leaving a gap where it was.
     *
     * A heart in the titlebar, left of the screen-off control. Small,
     * out of the way, and on every page - it is not something to hunt
     * for, and not something that should ever be in the way either. */

    /* Shuffle and repeat sit with the music, not in Setup: they are
     * playback modes changed while listening, and their state has to be
     * readable at a glance without leaving the page. The band beside
     * "Up next" was the only empty space on Now Playing wide enough for
     * two targets a thumb can hit. */
    /* Flanking the transport: they belong with the controls they
     * modify, not beside a line of text. */
    [SKIN_BTN_SHUFFLE]        = { TBTN_X(0), TBTN_Y, TBTN_W(0), TBTN_H },
    [SKIN_BTN_REPEAT]         = { TBTN_X(4), TBTN_Y, TBTN_W(4), TBTN_H },
    /* Top right of the page, above the title and below the tabs.
     *
     * The glyph is eleven by nine and a rectangle that size cannot be
     * hit, so the target is padded to 26 by 14 - the same trick the
     * volume keys use. It stops at row 31 deliberately: see the assert
     * by TITLE_SCALE, which is what keeps it off the title. */
    [SKIN_BTN_LIKE]           = { LIKE_HIT_X, LIKE_HIT_Y,
                                  LIKE_HIT_W, LIKE_HIT_H },

    /* Mark and save sit hard against the right edge, where delete used
     * to be the third of them. Delete moved to Faves and left a hole at
     * the end of the row; the two that stayed slide up into it rather
     * than leaving a button-sized gap between save and the edge. */
    /* No rectangles. The three list controls are laid out by
     * BrowserButtons, because where they are - and whether they are
     * there at all - depends on what the list is showing rather than on
     * which tab is showing it. Entries here would be a second, stale
     * copy of that layout; the last two were the same rectangle twice,
     * which was only harmless because one of them was never drawn. */
    [SKIN_BTN_MARK]           = { 0, 0, 0, 0 },
    [SKIN_BTN_SAVELIST]       = { 0, 0, 0, 0 },
    [SKIN_BTN_DELLIST]        = { 0, 0, 0, 0 },

    /* No rectangle: the counter reset is a row on the page now.
     *
     * It used to be a 58x22 box at a fixed y=35 - never drawn, only
     * hit-tested - from when the diagnostics page was a block of readouts
     * with room beside them. The page has been a scrolling list of rows
     * for a while, so it had become an invisible target lying across
     * whichever row happened to be under it, taking that row's taps. A
     * zero rectangle is skipped by the hit test and by the layout
     * check. */
    [SKIN_BTN_TEST_RESET]     = { 0, 0, 0, 0 },
    /* Five pixels clear of the transport bar below and the status block
     * sits five clear of the tab strip above - the page had been pressed
     * hard against both. */
    /* 100x12 was the smallest target on any page, and it is the one
     * control here that does something. */
    [SKIN_BTN_SLEEP_TOGGLE]   = {  68, CONTENT_Y + 134, 120, 18 },
    /* Spans the label too, not just the 13px box - a tick box alone is
     * well under what a fingertip can reliably hit on this screen. */
    /* Four pixels between the checkboxes and six below the slider. They
     * were flush against each other and against the slider above, so the
     * page read as one dense block of text with no grouping at all. */
    /* Sixteen tall with three-pixel gaps, and clear of the slider knob
     * above. They are the two things on this page a stylus has to land
     * on, and at fourteen pressed up against the slider they were the
     * hardest targets in the program. */
    [SKIN_BTN_SLEEP_POWEROFF] = {  16, CONTENT_Y + 100, 210, 16 },
    /* Four rows between the tabs and the transport, 26px apart: caption
     * on top, control underneath, and a 2px gap so the focus outline of
     * one row cannot touch the caption of the next. */
    /* Four 26px slots fill the content area exactly, leaving the right
     * edge for a scrollbar. */
    /* 22 tall on a 27 pitch. The five pixels between them are the point:
     * at 24-on-26 the segment strip of one row all but touched the
     * caption of the next, and two adjacent rows read as one block. */
    /* 28 apart rather than 27. The last row used to stop seven pixels
     * short of the transport with nothing under it, so the spare was
     * spread between the rows where it separates one setting from the
     * next instead of pooling at the bottom where it does nothing. */
    /* Thirty-two apart, not twenty-eight.
     *
     * A row is a caption with a control under it, and at the old pitch
     * the next row's caption sat eight pixels below the control above
     * it - close enough that the two read as one block and it was not
     * obvious which label went with which strip. Five of these still fit
     * the content area with room to spare. */
    /* Four in from the left and 238 across - the file list's own
     * margins. They were 6 and 222, which left the rows sitting well
     * clear of the rail on the right and a wider gutter on that side
     * than on this one, so the page read as shifted rather than set. */
    [SKIN_BTN_ROW0]           = {   4, CONTENT_Y +  1, 238, 22 },
    [SKIN_BTN_ROW1]           = {   4, CONTENT_Y + 33, 238, 22 },
    [SKIN_BTN_ROW2]           = {   4, CONTENT_Y + 65, 238, 22 },
    [SKIN_BTN_ROW3]           = {   4, CONTENT_Y + 97, 238, 22 },
    [SKIN_BTN_ROW4]           = {   4, CONTENT_Y +129, 238, 22 },
    /* Not in the tab row - the reference has the heart there and
     * nothing else. It is a row on Setup instead. */
};

/* The controls live inside the Now Playing page, in the order every
 * phone player uses: what is playing, how far through it is, the
 * transport, the volume, and then the things read once. The progress bar
 * sits on the middle of the screen because that is where the thumb
 * rests. */
/* The progress bar runs the full content width with its clocks above
 * it; the volume has a speaker at each end and stops short of both. Two
 * widths, because one bar is labelled and the other is flanked. */
#define STACK_TRACK_X   20
#define STACK_VOL_X     38
#define STACK_VOL_W     180


#define STACK_TRACK_W   216
#define STACK_PROG_Y    90

/* Where the "nothing loaded" lines sit, and the row they must not reach.
 *
 * The transport draws its clocks eleven rows above the progress bar, and
 * it draws them after this page has returned - so nothing stops the two
 * overlapping except this arithmetic. */
#define NP_CLOCK_Y      (STACK_PROG_Y - 11)
#define NP_EMPTY_Y      (CONTENT_Y + 12)

_Static_assert(NP_EMPTY_Y + 22 + GFX_GLYPH_H <= NP_CLOCK_Y,
               "the empty-page text runs into the transport's clock row, "
               "which is drawn over it afterwards");
#define STACK_VOL_Y     162

static void ProgressGeom(int *x, int *y, int *w)
{ *x = STACK_TRACK_X; *y = STACK_PROG_Y; *w = STACK_TRACK_W; }

static void VolumeGeom(int *x, int *y, int *w)
{ *x = STACK_VOL_X; *y = STACK_VOL_Y; *w = STACK_VOL_W; }


/* Which page each control belongs to. Transport, tabs and the titlebar
 * are on every page; the rest exist only on their own, and are allowed to
 * overlap each other in space because they are never visible together. */
#define PAGE_ANY (-1)

/* "Either settings page". The row slots are shared between Setup and
 * Test, so they belong to a set of pages rather than to one - which the
 * single-page field could not express, and which the overlap check needs
 * to know or it flags every slot against every Sleep control. */
#define PAGE_SETTINGS (-2)

/* Never matched by the generic hit test. For controls whose presence
 * depends on more than which tab is up, and which are therefore tested
 * by whatever owns that decision. */
#define PAGE_NEVER (-3)

/* Stored shifted, so that "nobody wrote an entry" is a value this table
 * can be asked about.
 *
 * A designated initialiser leaves unlisted entries zero, and zero was
 * SKIN_TAB_FILES - a real page - so a button added to the enum and
 * forgotten here silently became "only hittable on the Files page". That
 * is what happened to the Faves and About tabs: they drew on every page
 * and answered a tap on one, which looks like touch being broken rather
 * than like a table missing two lines.
 *
 * PG() puts every real value above zero, so zero means unset and
 * CheckButtonPages can say so. The shift is four rather than three
 * because PAGE_NEVER is -3, and at three it landed on zero - which
 * would have made "this control is tested elsewhere" indistinguishable
 * from "nobody wrote an entry", and silenced the check for it. */
#define PG(p) ((p) + 4)
#define UNPG(v) ((v) - 4)

static const int kButtonPage[SKIN_BTN_COUNT] = {
    /* Now Playing only. They used to be PAGE_ANY because they lived in a
     * strip along the bottom of every page; now they sit in the middle
     * of this one, over ground the settings rows and the sleep slider
     * occupy on theirs. Left as PAGE_ANY, a tap on the middle of Setup
     * would have pressed play. */
    [SKIN_BTN_PREV]            = PG(SKIN_TAB_NOWPLAYING),
    [SKIN_BTN_PLAYPAUSE]       = PG(SKIN_TAB_NOWPLAYING),
    [SKIN_BTN_NEXT]            = PG(SKIN_TAB_NOWPLAYING),
    [SKIN_BTN_VOL_DOWN]        = PG(SKIN_TAB_NOWPLAYING),
    [SKIN_BTN_VOL_UP]          = PG(SKIN_TAB_NOWPLAYING),
    [SKIN_BTN_TAB_FILES]       = PG(PAGE_ANY),
    [SKIN_BTN_TAB_FAVES]       = PG(PAGE_ANY),
    [SKIN_BTN_TAB_NOWPLAYING]  = PG(PAGE_ANY),
    [SKIN_BTN_TAB_SLEEP]       = PG(PAGE_ANY),
    [SKIN_BTN_TAB_ABOUT]       = PG(PAGE_ANY),
    /* No rectangle, so it is never hit; the page is reached from Setup
     * and left by tapping any tab. */
    [SKIN_BTN_TAB_TEST]        = PG(PAGE_ANY),
    [SKIN_BTN_SHUFFLE]         = PG(SKIN_TAB_NOWPLAYING),
    [SKIN_BTN_REPEAT]          = PG(SKIN_TAB_NOWPLAYING),
    [SKIN_BTN_LIKE]            = PG(SKIN_TAB_NOWPLAYING),
    /* The three list controls are not in the generic hit test at all.
     *
     * Which of them are on screen, and where, depends on what the list
     * is showing rather than on which tab is showing it - a playlist
     * opened from Faves has tracks in it and wants all three, the tab
     * at its top level has lists in it and wants one. A page gate can
     * only see the tab, so it had Mark and Add live on Files and dead
     * everywhere else, including inside the playlists they exist to
     * edit. Skin_HitTestBrowserButton walks the same layout the drawing
     * does instead, so one cannot be live where the other is absent. */
    [SKIN_BTN_MARK]            = PG(PAGE_NEVER),
    [SKIN_BTN_SAVELIST]        = PG(PAGE_NEVER),
    [SKIN_BTN_DELLIST]         = PG(PAGE_NEVER),
    [SKIN_BTN_TEST_RESET]      = PG(SKIN_TAB_TEST),
    [SKIN_BTN_TAB_SETUP]       = PG(PAGE_ANY),
    [SKIN_BTN_SLEEP_TOGGLE]    = PG(SKIN_TAB_SLEEP),
    [SKIN_BTN_SLEEP_POWEROFF]  = PG(SKIN_TAB_SLEEP),
    /* Live on both scrollable pages, so PAGE_ANY - the pages are never
     * on screen together and each maps slots to its own rows. */
    /* Slot 0 is Setup-only: Test keeps that space for its measurements
     * and its Reset button, so the two never contend for it. */
    [SKIN_BTN_ROW0]            = PG(SKIN_TAB_SETUP),
    [SKIN_BTN_ROW1]            = PG(PAGE_SETTINGS),
    [SKIN_BTN_ROW2]            = PG(PAGE_SETTINGS),
    [SKIN_BTN_ROW3]            = PG(PAGE_SETTINGS),
    [SKIN_BTN_ROW4]            = PG(PAGE_SETTINGS),
};

static uint16_t *s_bmp = NULL;

/* Per-piece cache for the file list, so a redraw can skip whatever did
 * not change. Cleared whenever the content backdrop is repainted, which
 * is what a page switch does. */
static uint32_t TextHash(const char *s);   /* defined with the overlay code */

/* File-list text size, as four steps rather than two.
 *
 * Only the list scales. It is the surface anyone actually reads for any
 * length of time, and it is the one page whose layout can give up rows
 * to buy height - everything else on the bottom screen is a fixed
 * arrangement of controls that doubling the type would simply break.
 *
 * The middle steps change row height without changing the type, which is
 * worth having on its own: a taller row is an easier target for a stylus
 * even when the text in it is the same size. */
/* Row counts grew when the transport strip stopped following the file
 * list down the screen: the list runs to the bottom of the panel now
 * rather than stopping short of a band of buttons for a track nobody is
 * looking at on this page. */
static const struct { uint8_t rowH, scale, rows; } kListSizes[SKIN_LIST_SIZES] = {
    { 15, 1, 9 },
    { 18, 1, 7 },
    { 22, 2, 6 },
    { 26, 2, 5 },
};

/* Advances once per drawn frame; the selected row's marquee is a
 * function of it. */
static uint32_t s_marqueeTick = 0;

/* One step every ten frames - six characters a second.
 *
 * Two things set this. Reading: a character a frame, or even one every
 * four, goes past faster than anyone can follow, and a name that has
 * scrolled by before you have read it is no better than one that was
 * truncated. Cost: every cache that folds this tick in redraws when it
 * changes, so the rate is also how often the file list repaints while a
 * long name sits selected. Six a second reads comfortably and leaves the
 * other fifty-four frames alone. */
void Skin_TickMarquee(void)
{
    static uint8_t sub = 0;
    if (++sub >= 10) { sub = 0; s_marqueeTick++; }
}

/* Set by whichever surface last drew a name that did not fit. The frame
 * loop uses them to know a screen still needs redrawing even though
 * nothing else about it changed. */
static bool s_overlayScrolling = false;
static bool s_cardScrolling = false;
static bool s_npScrolling = false;

bool Skin_NowPlayingScrolling(void) { return s_npScrolling; }
uint32_t Skin_MarqueeTick(void) { return s_marqueeTick; }

/* True while the selected row's name is too long to fit, which is the
 * only case the list needs redrawing every frame for. */
static bool s_marqueeActive = false;
bool Skin_MarqueeActive(void) { return s_marqueeActive; }

static int s_listSize = 0;
static int s_listScale = 1;

void Skin_SetListSize(int size)
{
    if (size < 0) size = 0;
    if (size >= SKIN_LIST_SIZES) size = SKIN_LIST_SIZES - 1;

    s_listSize = size;
    s_listScale = kListSizes[size].scale;
    FileBrowser_SetVisibleRows(kListSizes[size].rows);
    Skin_InvalidateContent();
}

int Skin_ListSize(void) { return s_listSize; }
int Skin_ListRowHeight(void) { return ROW_H; }

static bool     s_contentReset = true;
static uint32_t s_pathSig = 0;
static uint32_t s_scrollSig = 0;
static uint32_t s_rowSig[FB_MAX_VISIBLE_ROWS];

/* The scrollTop s_rowSig was built at, so a shift can be told apart
 * from a genuine change of contents. -1 means the cache is not shiftable. */
static int s_rowScrollTop = -1;

/* "Contents unknown - redraw whatever is there."
 *
 * It cannot be 0. RowSignature returns 0 for a slot past the end of the
 * list and an odd value for every real row, so 0 is a genuine signature
 * meaning "this slot is empty" - and marking a shifted slot with it made
 * a row that had just scrolled off the end of a short list compare equal
 * to its own marker, skip its redraw and keep the stale pixels the shift
 * had left underneath. Any even non-zero value is one RowSignature can
 * never produce, which is exactly what this needs to be. */
#define ROWSIG_UNKNOWN 2u

/* Same idea for the transport bar. This one matters because the bar is
 * asked to redraw four times a second for as long as a track plays, so
 * that the clock stays honest - and repainting six buttons, two meters
 * and the readout to advance one digit was, after the file list, the
 * largest standing cost on the bottom screen. */
/* Setup rows, sleep blocks and the Now Playing page, same scheme. The
 * erase rectangles below are deliberately disjoint: a piece repaints
 * itself by wiping back to the panel gradient first, so if two of them
 * overlapped, redrawing one would eat part of a neighbour that had
 * decided it did not need redrawing. */
static uint32_t s_setupSig[3];
static uint32_t s_sleepTimerSig, s_sleepOptSig;
static uint32_t s_npSig;

static bool     s_transportReset = true;
static uint32_t s_btnSig[SKIN_BTN_VOL_UP + 1];
static uint32_t s_volSig = 0xFFFFFFFFu;
static uint32_t s_progSig = 0xFFFFFFFFu;
static uint32_t s_clockSig = 0xFFFFFFFFu;

/* Frames of "pressed" feedback left for each control. */
static uint8_t s_pressTimer[SKIN_BTN_COUNT];
/* How long a control stays lit after the finger leaves it.
 *
 * This has been six, forty-two and fourteen. Part of why fourteen still
 * read as long is that the two mode keys were not clearing at all - they
 * are drawn by the Now Playing page, whose signature ignored whether they
 * were lit, so their highlight stayed until something else repainted the
 * page. That is fixed separately; this is the timer alone. */
#define PRESS_FRAMES 5

/* Whatever the stylus is currently resting on. */
static SkinButton s_heldButton = SKIN_BTN_NONE;

/* ------------------------------------------------------------------ */
/* surfaces                                                           */
/* ------------------------------------------------------------------ */

/* Named for what it replaced. Every caller used to hand this two colours
 * and get a vertical ramp; now it takes the first and fills flat, so the
 * whole skin lost its gradients in one place rather than in ninety.
 *
 * What went with them: a 64-slot ramp cache holding 128 precomputed
 * colours each, about 17KB of RAM that existed only to keep Gfx_Lerp -
 * three software divisions a row, on a CPU with no divider - off the
 * repaint path. A flat fill needs neither. */
static inline void FastVGradient(int x, int y, int w, int h,
                                 uint16_t top, uint16_t bottom)
{
    (void)bottom;
    Gfx_Fill(x, y, w, h, top);
}

void Skin_Init(uint16_t *bitmap)
{
    ApplyPalette();
    s_bmp = bitmap;
    memset(s_pressTimer, 0, sizeof(s_pressTimer));
}

void Skin_FlashButton(SkinButton btn)
{
    if (btn > SKIN_BTN_NONE && btn < SKIN_BTN_COUNT)
        s_pressTimer[btn] = PRESS_FRAMES;
}

void Skin_FlashButtonBrief(SkinButton btn)
{
    if (btn > SKIN_BTN_NONE && btn < SKIN_BTN_COUNT)
        s_pressTimer[btn] = (PRESS_FRAMES + 1) / 2;
}

void Skin_TickFeedback(unsigned periods)
{
    if (periods == 0) periods = 1;
    if (periods > PRESS_FRAMES) periods = PRESS_FRAMES;

    for (int i = 0; i < SKIN_BTN_COUNT; i++)
        s_pressTimer[i] = (s_pressTimer[i] > periods)
                        ? (uint8_t)(s_pressTimer[i] - periods) : 0;
}

void Skin_SetHeldButton(SkinButton btn)
{
    s_heldButton = btn;
}

/* Held wins over the flash timer, so a control stays down for as long as
 * the stylus is on it and the brief flash only covers quick taps. */
static bool IsPressed(SkinButton b)
{
    return s_pressTimer[b] != 0 || s_heldButton == b;
}

bool Skin_ButtonLit(SkinButton btn)
{
    return btn > SKIN_BTN_NONE && btn < SKIN_BTN_COUNT && IsPressed(btn);
}

/* A value that changes whenever any control's lit state does.
 *
 * It used to set bit (i + 8) per pressed button, which fits while there
 * are fewer than twenty-four of them. There are twenty-six: shuffle at 23
 * shifted into the sign bit of a signed int, and repeat at 24 shifted by
 * 32, which is undefined and on ARM produces nothing at all. So repeat's
 * press never reached this signature, the frame never saw a change, and
 * the page never repainted - its highlight could not have appeared no
 * matter what the drawing code did.
 *
 * Hashed instead of packed, so it cannot run out of room whatever gets
 * added to the button list. */
int Skin_FeedbackState(void)
{
    uint32_t sig = (uint32_t)(s_heldButton + 1);
    for (int i = 0; i < SKIN_BTN_COUNT; i++)
        if (s_pressTimer[i]) sig = sig * 31u + (uint32_t)(i + 1);
    return (int)sig;
}


static bool PointInRect(int px, int py, const SkinRect *r)
{
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

/* ------------------------------------------------------------------ */
/* shared control shapes                                              */
/* ------------------------------------------------------------------ */

/* One glossy raised button: a vertical gradient split at the midpoint,
 * with a brighter top half, plus a light/dark 1px bevel. The split is
 * what reads as "glass" at this size; a single linear ramp looks flat. */
/* A control is a flat plate. Pressed swaps the surface for the accent
 * rather than pushing a bevel in - it is one fill either way, and at this
 * size a colour change is far easier to see than a one-pixel edge moving.
 *
 * Gone with the bevel: two half-height gradients, a gloss hairline, four
 * bevel lines, and the save/restore of four corner pixels that rounded it
 * off. Eleven operations and two reads per button, six buttons on the
 * transport bar alone, four times a second. */

/* Defined with the sliders; the top bar's progress groove wants the
 * same ends. */
static void CapEnds(int x, int y, int w, int h, uint16_t bg);

/* A rectangle with circular corners, which at a radius of half the
 * height is a pill. The inset per row comes from the same disc test the
 * slider handle uses, so the curve matches the one already on screen. */
/* Defined with the sliders; the transport's play key wants the same
 * shape and the same lighting. */
static void FillRoundedLit(int x, int y, int w, int h, int r,
                           uint16_t hi, uint16_t base);

static void FillRounded(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int dy = (j < r) ? (r - 1 - j)
               : (j >= h - r) ? (j - (h - r)) : -1;
        int inset = 0;
        if (dy >= 0) {
            int d = 0;
            while ((d + 1) * (d + 1) + dy * dy <= r * r) d++;
            inset = r - d;
        }
        if (w - 2 * inset > 0)
            Gfx_Fill(x + inset, y + j, w - 2 * inset, 1, c);
    }
}

static void DrawButtonFace(const SkinRect *r, bool pressed)
{
    Gfx_Fill(r->x, r->y, r->w, r->h, pressed ? C_ACCENT : C_RAISED);

    /* One hairline along the top edge keeps adjacent plates from merging
     * into a single block. */
    if (!pressed)
        Gfx_HLine(r->x, r->y, r->w, C_RAISED_HI);
}

/* Repaints part of a surface back to its own colour. It used to have to
 * reconstruct the right slice of a gradient, reading the same cached ramp
 * the full fill had used so the seam would not show; with flat surfaces
 * there is no slice and no seam - the colour is the same everywhere - and
 * the whole problem disappears. Kept as a named call because the pieces
 * that erase themselves read better for saying so. */
static void GradientRows(int x, int y, int w, int h, int gy, int gh,
                         uint16_t top, uint16_t bottom)
{
    (void)gy; (void)gh; (void)bottom;
    Gfx_Fill(x, y, w, h, top);
}

/* Wipes part of a page back to the panel gradient behind it, so one
 * piece of that page can be redrawn without the whole backdrop. */
static void EraseContent(int x, int y, int w, int h)
{
    /* Blue Classic's side rails belong to the bezel, not the page.  A
     * piecewise redraw (the sleep countdown is the frequent case) must
     * not paint over them while clearing its own content. */
    if (s_theme == 0) {
        if (x < 8) { w -= 8 - x; x = 8; }
        if (x + w > GFX_W - 8) w = GFX_W - 8 - x;
    }
    if (w <= 0 || h <= 0) return;
    GradientRows(x, y, w, h, CONTENT_Y, CONTENT_H, C_PANEL_TOP, C_PANEL_BOT);
}


/* Segmented meter fill. Drawn as discrete cells rather than one solid
 * bar because that is what makes a small change readable on a gauge
 * 244 pixels wide. */
/* Trough plus one solid bar.
 *
 * The old meter drew its fill as a row of four-pixel cells with a gap
 * between each, which meant a full progress bar was fifty separate
 * clipped fills instead of one. It looked like an equaliser; it read like
 * a wall of stripes. A continuous bar is both clearer and a single fill,
 * and the exact end position stays honest to the value. */
static void DrawMeter(int x, int y, int w, int h, int permille,
                      uint16_t top, uint16_t bottom)
{
    (void)bottom;
    Gfx_Fill(x, y, w, h, C_TROUGH);

    int innerX = x + 1, innerY = y + 1;
    int innerW = w - 2, innerH = h - 2;
    if (innerW <= 0 || innerH <= 0) return;

    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;

    int filled = (innerW * permille) / 1000;
    if (filled > 0)
        Gfx_Fill(innerX, innerY, filled, innerH, top);
}

/* ---- transport glyphs, drawn as shapes rather than bitmap assets ---- */

static void DrawTriangle(int cx, int cy, int size, bool pointingRight, uint16_t c)
{
    for (int row = -size; row <= size; row++) {
        int width = size - (row < 0 ? -row : row);
        for (int col = 0; col <= width; col++)
            Gfx_Pixel(pointingRight ? cx + col : cx - col, cy + row, c);
    }
}


/* ------------------------------------------------------------------ */
/* chrome                                                             */
/* ------------------------------------------------------------------ */

static void DrawNoteIcon(int x, int y, uint16_t c);

/* The like heart, in two states.
 *
 * Hollow when the track is not a favourite and solid when it is, which
 * is the one convention nobody has to be taught. Colour alone would not
 * do it: the dim/accent pair the other toggles use is legible when the
 * two are side by side and ambiguous when there is only one control and
 * no second state to compare it against.
 *
 * Eleven wide and nine tall - the same shape the support page's heart
 * uses, so the two read as the same mark. */
/* Thirteen by eleven: the heart plus the ring drawn around it.
 *
 * The border belongs to the heart, not to a rectangle behind it. A
 * rounded box made the corner look like a key, which it is - but what
 * needed to be easier to see was the heart, and a frame around it does
 * not make the shape inside any clearer. */
#define LIKE_W 13
#define LIKE_H 11

static const uint16_t kHeartSolid[LIKE_H] = {
    0x0000,  /* ............. */
    0x38E0,  /* ..###...###.. */
    0x7DF0,  /* .#####.#####. */
    0x7FF0,  /* .###########. */
    0x7FF0,  /* .###########. */
    0x3FE0,  /* ..#########.. */
    0x1FC0,  /* ...#######... */
    0x0F80,  /* ....#####.... */
    0x0700,  /* .....###..... */
    0x0200,  /* ......#...... */
    0x0000,  /* ............. */
};

/* The ring: every pixel just outside the shape. Drawn under the heart in
 * a contrasting tone, it is a border that follows the heart rather than
 * boxing it, and it is what makes an eleven pixel glyph read against a
 * dark page from arm's length. Computed from the body, so the two cannot
 * drift apart. */
static const uint16_t kHeartRing[LIKE_H] = {
    0x7DF0,  /* .#####.#####. */
    0xC718,  /* ##...###...## */
    0x8208,  /* #.....#.....# */
    0x8008,  /* #...........# */
    0x8008,  /* #...........# */
    0xC018,  /* ##.........## */
    0x6030,  /* .##.......##. */
    0x3060,  /* ..##.....##.. */
    0x18C0,  /* ...##...##... */
    0x0D80,  /* ....##.##.... */
    0x0700,  /* .....###..... */
};

/* Not liked: the heart's own edge, with the middle left out.
 *
 * This was the ring for a while, which was wrong twice over. The ring
 * sits one pixel outside the body, so the empty heart stood a pixel
 * taller and wider than the full one and the two states jumped as they
 * swapped; and the ring closes over the notch between the lobes, which
 * is the one feature that says heart rather than shield. Taking the
 * boundary from inside the body keeps both states on the same thirteen
 * by eleven footprint, and keeps the notch.
 *
 * Every pixel of kHeartSolid that touches a pixel kHeartSolid does not
 * have. Computed from the body, so the two cannot drift apart. */
static const uint16_t kHeartHollow[LIKE_H] = {
    0x0000,  /* ............. */
    0x38E0,  /* ..###...###.. */
    0x4510,  /* .#...#.#...#. */
    0x4210,  /* .#....#....#. */
    0x4010,  /* .#.........#. */
    0x2020,  /* ..#.......#.. */
    0x1040,  /* ...#.....#... */
    0x0880,  /* ....#...#.... */
    0x0500,  /* .....#.#..... */
    0x0200,  /* ......#...... */
    0x0000,  /* ............. */
};
/* The active tab is marked by a solid accent bar under it and brighter
 * text, which is unmistakable at a glance. The old treatment - a tab that
 * grew two pixels and lost its bottom border to merge with the panel -
 * needed three edge lines and a height change to say the same thing, and
 * said it more quietly. */
/* Set 3: a page, a heart, a disc, a clock, a gear, a person.
 *
 * Drawn from rectangles rather than stored as bitmaps because at eleven
 * pixels most of these are two or three rectangles, and the arithmetic
 * says what the shape is where a row of hex would not. */
static void TabIconFiles(int x, int y, uint16_t c, uint16_t bg)
{
    (void)bg;
    Gfx_Frame(x + 2, y, 9, 11, c);          /* the page */
    Gfx_Fill(x + 4, y + 3, 5, 1, c);        /* two lines of writing */
    Gfx_Fill(x + 4, y + 6, 5, 1, c);
}

static void TabIconFaves(int x, int y, uint16_t c, uint16_t bg)
{
    (void)bg;
    Gfx_Icon16(x, y, LIKE_H, kHeartSolid, c);
}

static void TabIconPlaying(int x, int y, uint16_t c, uint16_t bg)
{
    /* A disc, and round rather than square.
     *
     * Drawn as a rectangle it was a box with a dot in it, which is what
     * the clock beside it is too - two neighbouring tabs telling the
     * same shape apart by their contents at eleven pixels. Rounded, the
     * two read apart at a glance, which is the whole job of an icon.
     *
     * The hole is punched in the colour behind the tab, so it is a ring
     * and not a filled blob with a dark speck. */
    FillRounded(x + 1, y, 11, 11, 5, c);
    FillRounded(x + 4, y + 3, 5, 5, 2, bg);
    Gfx_Fill(x + 6, y + 5, 1, 1, c);
}

static void TabIconSleep(int x, int y, uint16_t c, uint16_t bg)
{
    (void)bg;
    Gfx_Frame(x + 1, y + 1, 11, 9, c);      /* the face */
    Gfx_Fill(x + 6, y + 3, 1, 3, c);        /* the hands, at four o'clock */
    Gfx_Fill(x + 6, y + 5, 3, 1, c);
}

static void TabIconSetup(int x, int y, uint16_t c, uint16_t bg)
{
    (void)bg;
    Gfx_Frame(x + 3, y + 2, 7, 7, c);       /* the ring */
    Gfx_Fill(x + 5, y,     3, 2, c);        /* four teeth */
    Gfx_Fill(x + 5, y + 9, 3, 2, c);
    Gfx_Fill(x + 1, y + 4, 2, 3, c);
    Gfx_Fill(x + 10, y + 4, 2, 3, c);
}

static void TabIconAbout(int x, int y, uint16_t c, uint16_t bg)
{
    (void)bg;

    /* A person, filled and rounded. Squared off it was a small box over
     * a larger box, which is a building, not somebody. */
    FillRounded(x + 4, y, 5, 5, 2, c);      /* head */
    FillRounded(x + 1, y + 6, 11, 5, 3, c); /* shoulders, round on top */
    Gfx_Fill(x + 1, y + 10, 11, 1, c);      /* and square where they end */
}

typedef void (*TabIcon)(int, int, uint16_t, uint16_t);
static const TabIcon kTabIcons[TAB_COUNT] = {
    TabIconFiles, TabIconFaves, TabIconPlaying,
    TabIconSleep, TabIconSetup, TabIconAbout,
};

static void TabLayout(SkinTab active, SkinRect *out)
{
    const int sel = TabActiveIndex(active);

    int w[TAB_COUNT], total = 0;
    for (int i = 0; i < TAB_COUNT; i++) {
        w[i] = TAB_ICON_W + TAB_ICON_PAD * 2;
        if (i == sel) w[i] += TAB_NAME_GAP + Gfx_TextWidth(kTabNames[i]);
        total += w[i];
    }

    int x = (GFX_W - total) / 2;
    if (x < 0) x = 0;

    for (int i = 0; i < TAB_COUNT; i++) {
        out[i].x = x;
        out[i].y = TABS_Y;
        out[i].w = w[i];
        out[i].h = TABS_H;
        x += w[i];
    }
}

/* One tab: its icon, and its name when it is the one you are on. */
static void DrawTab(int index, const SkinRect *r, bool active)
{
    const int barH = 2;

    Gfx_Fill(r->x, r->y, r->w, r->h - barH, active ? C_BG : C_SURFACE);
    Gfx_Fill(r->x, r->y + r->h - barH, r->w, barH, active ? C_ACCENT : C_SURFACE);

    if (TR.tabsBoxed) {
        Gfx_Fill(r->x, r->y, r->w, r->h, active ? C_BG : C_SURFACE);
        Gfx_Frame(r->x + 1, r->y, r->w - 2, r->h - 1,
                  active ? C_ACCENT : C_HAIRLINE);
    }

    const uint16_t ink = active ? C_TEXT : C_TEXT_DIM;

    /* The band the icon and the name are centred in - not the cell.
     *
     * Each treatment spends part of the cell on something that is not
     * content: the bar gives its bottom two rows to the accent bar, and
     * the box gives its top and bottom rows to the frame. One formula
     * served both, measured off the bar, so inside a box everything sat a
     * pixel high and the icon touched the top border. Barely visible
     * against a flat bar; unmistakable inside a frame, which is where it
     * was noticed - Retro Green and Lime draw one.
     *
     * Eleven rows of icon in fourteen, and eight of text in thirteen,
     * cannot both land dead centre. The odd pixel goes below in every
     * case, which is the convention the rest of the file follows. */
    const int bandY = TR.tabsBoxed ? r->y + 1 : r->y;
    const int bandH = TR.tabsBoxed ? r->h - 3 : r->h - barH;

    /* The icon sits against the left padding when a name follows it and
     * in the middle of the cell when it does not, so an unselected tab
     * is a mark centred in its own space rather than a mark shoved to
     * one side of it. */
    int iconX = active ? r->x + TAB_ICON_PAD
                       : r->x + (r->w - TAB_ICON_W) / 2;
    int iconY = bandY + (bandH - 11) / 2;

    kTabIcons[index](iconX, iconY, ink, active ? C_BG : C_SURFACE);

    if (active) {
        char up[16];
        const char *label = kTabNames[index];
        if (TR.tabsUpper) {
            size_t i = 0;
            for (; label[i] && i < sizeof(up) - 1; i++)
                up[i] = (label[i] >= 'a' && label[i] <= 'z')
                      ? (char)(label[i] - 32) : label[i];
            up[i] = '\0';
            label = up;
        }
        Gfx_Text(iconX + TAB_ICON_W + TAB_NAME_GAP,
                 bandY + (bandH - GFX_GLYPH_H) / 2, ink, label);
    }
}

/* The Files view's list panel (LIST_X..LIST_X+LIST_W, LIST_Y..LIST_Y+
 * LIST_H) sits almost exactly on top of the general content-area
 * background and is about to be drawn over it by DrawFileList() a few
 * calls later - painting the full-width gradient underneath it first is
 * pure waste. Only the margin actually left visible needs it: a strip
 * either side of the panel, and the 1px sliver above it where LIST_Y
 * sits below CONTENT_Y. The Now Playing view doesn't cover the content
 * area the same way (its art box is a small corner square), so it still
 * gets the plain full-width fill. */
/* One full-width flat band for every page.
 *
 * The Files page used to cut a hole here for the list panel, because the
 * panel painted a different gradient over it and filling twice was pure
 * waste. Flat surfaces make the hole pointless - the list sits on exactly
 * this colour - and a single full-width fill is the fastest shape there
 * is, one unbroken run of memory rather than three clipped rectangles. */
static void DrawContentBackdrop(SkinTab activeTab)
{
    (void)activeTab;

    /* The whole content area, on every theme.
     *
     * The first theme used to inset this by eight pixels a side and stop
     * six rows short of the bottom, which saved a little filling and
     * left a margin that nothing else on the file list paints - the path
     * bar starts at four and the list at four, so four pixels down each
     * edge belonged to no one. The settings pages do fill the full
     * width, so leaving one of those for Files left a sliver of the page
     * behind, down the left edge under the path bar, until something
     * else happened to cover it.
     *
     * Two pages disagreeing about who owns the edge of the screen is not
     * a saving worth having; both fill it now. */
    Gfx_Fill(0, CONTENT_Y, GFX_W, CONTENT_H, C_BG);
}

void Skin_BeginFrame(void)
{
    Gfx_SetTarget(s_bmp);
    Gfx_ResetDirty();
}

void Skin_InvalidateContent(void)
{
    s_contentReset = true;
    s_pathSig = s_scrollSig = 0;
    for (int i = 0; i < FB_MAX_VISIBLE_ROWS; i++) s_rowSig[i] = 0;
    s_rowScrollTop = -1;
    for (int i = 0; i < 3; i++) s_setupSig[i] = 0;
    s_sleepTimerSig = s_sleepOptSig = s_npSig = 0;
}

void Skin_InvalidateBottom(void)
{
    Skin_InvalidateContent();
    s_contentReset = true;
    s_transportReset = true;
    s_pathSig = s_scrollSig = 0;
    s_volSig = s_progSig = s_clockSig = 0xFFFFFFFFu;
    for (int i = 0; i < FB_MAX_VISIBLE_ROWS; i++) s_rowSig[i] = 0;
    s_rowScrollTop = -1;
    for (int i = 0; i <= SKIN_BTN_VOL_UP; i++) s_btnSig[i] = 0xFFFFFFFFu;
}

void Skin_DrawChrome(SkinTab activeTab)
{
    /* Down to CONTENT_Y, not just to the bottom of the tabs. The rows
     * between the tab band and the top of the page belonged to nothing
     * and stayed whatever the buffer was cleared to - a black stripe
     * under the tabs. */
    Gfx_Fill(0, 0, GFX_W, CONTENT_Y, C_SURFACE);
    FastVGradient(0, TABS_Y, GFX_W, TABS_H, C_TITLE_BOT, C_TITLE_BOT);
    SkinRect tabs[TAB_COUNT];
    TabLayout(activeTab, tabs);

    const int sel = TabActiveIndex(activeTab);
    for (int i = 0; i < TAB_COUNT; i++)
        DrawTab(i, &tabs[i], i == sel);
}

/* ------------------------------------------------------------------ */
/* content: file list                                                 */
/* ------------------------------------------------------------------ */

/* Where the thumb was left, so only the part that moves is repainted. */
static int s_thumbY = -1, s_thumbH = 0;

/* The rail is painted once; after that only the thumb moves.
 *
 * Repainting the whole rail on every scroll marked ninety-five rows
 * dirty - and the presenter flushes and transfers whole rows, because a
 * dirty region here is a span of rows and not a rectangle. So a control
 * six pixels wide was costing forty-eight kilobytes of cache flush and
 * DMA per scroll step, twenty times a second while the d-pad is held,
 * to move a thumb about two pixels. That was the largest single cost of
 * scrolling, and it was not the list.
 *
 * Erasing where it was and painting where it is leaves a dirty span of
 * the thumb plus the distance travelled - a couple of dozen rows instead
 * of ninety-five. */
static void DrawScrollbar(const FileBrowser *fb, bool reset)
{
    const int trackY = LIST_Y, trackH = LIST_H;

    if (reset) {
        Gfx_Fill(SCROLL_X, trackY, SCROLL_W, trackH, C_TROUGH);
        s_thumbY = -1;
    }

    if (fb->count <= FileBrowser_VisibleRows()) {
        /* A list that now fits has no thumb; take away the one it had
         * rather than leaving it stranded on the rail. */
        if (s_thumbY >= 0 && !reset)
            Gfx_Fill(SCROLL_X, s_thumbY, SCROLL_W, s_thumbH, C_TROUGH);
        s_thumbY = -1;
        return;
    }

    const int innerH = trackH - 2;
    int thumbH = (innerH * FileBrowser_VisibleRows()) / fb->count;
    if (thumbH < 24) thumbH = 24;      /* matches Skin_HitTestListScroll */

    const int span = fb->count - FileBrowser_VisibleRows();
    const int thumbY = trackY + 1 +
                       ((innerH - thumbH) * fb->scrollTop) / (span > 0 ? span : 1);

    if (!reset && thumbY == s_thumbY && thumbH == s_thumbH) return;

    if (s_thumbY >= 0 && !reset)
        Gfx_Fill(SCROLL_X, s_thumbY, SCROLL_W, s_thumbH, C_TROUGH);

    Gfx_Fill(SCROLL_X + 1, thumbY, SCROLL_W - 2, thumbH, C_ACCENT);
    s_thumbY = thumbY;
    s_thumbH = thumbH;
}

/* Small folder glyph so directories read as directories at a glance
 * without needing a "[DIR]" prefix eating into the filename. */
/* A five-pointed star at this size is a blob; this is the readable
 * version - a plus with its corners filled. */
/* A list with a note beside it, which is what a playlist is.
 *
 * Three bars on their own are the shorthand for any list at all, and
 * this program draws them for the path bar as well - so the row that
 * said "playlist" and the bar above it wore the same glyph, and neither
 * said anything about music. The note is the half that makes it a
 * playlist rather than a list, and it is the same shape the track rows
 * carry, so the icon reads as "a list of those".
 *
 * Eleven wide against the old nine, which the rows have room for: the
 * icon sits at LIST_X + 6 and the name starts at LIST_X + 20. The bars
 * lose four pixels of length to pay for it, and three bars still read
 * as three bars at five pixels. */
static void DrawListIcon(int x, int y, uint16_t c)
{
    Gfx_Fill(x, y + 1, 5, 2, c);
    Gfx_Fill(x, y + 4, 5, 2, c);
    Gfx_Fill(x, y + 7, 3, 2, c);

    /* A quarter note, not the eighth the track rows use: at this size
     * the flag is three pixels that only muddy it, and the two glyphs
     * are better off not being the same drawing at different widths. */
    Gfx_Fill(x + 9, y + 1, 2, 6, c);     /* stem */
    Gfx_Fill(x + 6, y + 6, 4, 3, c);     /* head */
}

/* Which of the three list controls are on screen, and where.
 *
 * Both the drawing and the hit test come through here, so a control
 * cannot answer a tap where it is not drawn, or be drawn where it
 * answers nothing. They used to be a fixed table gated on which tab was
 * up, which could not express the case that matters: a playlist opened
 * from the Faves tab has tracks in it, so Mark and Add belong there -
 * but the tab is Faves, so both were dead. Since every playlist lives
 * in that tab, marking several tracks to remove them together, and
 * adding to the list you are looking at, could not be reached at all.
 *
 * Right-aligned, so a folder's two sit exactly where they always have
 * and the third appears to their left only inside a list. */
#define BROWSER_BTN_W     22
#define BROWSER_BTN_H     13
#define BROWSER_BTN_Y     (CONTENT_Y + 2)
#define BROWSER_BTN_STEP  24
#define BROWSER_BTN_RIGHT 226
#define BROWSER_BTN_MAX   3

_Static_assert(BROWSER_BTN_RIGHT + BROWSER_BTN_W <= GFX_W,
               "the list controls run off the right edge");
_Static_assert(BROWSER_BTN_RIGHT - (BROWSER_BTN_MAX - 1) * BROWSER_BTN_STEP
               > PATH_X + 18, "a full row of list controls leaves no path bar");

static int BrowserButtons(const FileBrowser *fb, SkinButton *ids,
                          SkinRect *rects)
{
    SkinButton want[BROWSER_BTN_MAX];
    int n = 0;

    /* Where the rows are tracks, they can be marked. */
    if (FileBrowser_RowsAreTracks(fb))
        want[n++] = SKIN_BTN_MARK;

    /* And in a folder, what is marked can go into a list.
     *
     * Not inside a list. There was a button there that meant "go and
     * fetch more for this one", and from the Faves tab - which is where
     * every list this player saves actually lives - it left the
     * playlist and landed back on the list of lists, which is locked
     * and has no tracks in it to mark. Nothing on screen changed except
     * that the list the user was reading had closed. Adding to a list
     * is done the other way round: mark the tracks in the file list and
     * press this, which offers every playlist including Liked. */
    if (!fb->inPlaylist && FileBrowser_RowsAreTracks(fb))
        want[n++] = SKIN_BTN_SAVELIST;

    /* And the one that takes something away - a track out of the list
     * you are inside, or a whole list off the shelf. */
    if (fb->locked || fb->inPlaylist)
        want[n++] = SKIN_BTN_DELLIST;

    for (int i = 0; i < n; i++) {
        ids[i] = want[i];
        rects[i] = (SkinRect){
            BROWSER_BTN_RIGHT - (n - 1 - i) * BROWSER_BTN_STEP,
            BROWSER_BTN_Y, BROWSER_BTN_W, BROWSER_BTN_H
        };
    }
    return n;
}

static void DrawStar(int cx, int cy, uint16_t c)
{
    Gfx_Fill(cx - 4, cy - 1, 9, 3, c);
    Gfx_Fill(cx - 1, cy - 4, 3, 9, c);
    Gfx_Pixel(cx - 2, cy - 2, c); Gfx_Pixel(cx + 2, cy - 2, c);
    Gfx_Pixel(cx - 2, cy + 2, c); Gfx_Pixel(cx + 2, cy + 2, c);
}

static void DrawFolderIcon(int x, int y, uint16_t c)
{
    Gfx_Fill(x, y + 1, 4, 2, c);
    Gfx_Fill(x, y + 3, 10, 6, c);
}

static void DrawNoteIcon(int x, int y, uint16_t c)
{
    Gfx_Fill(x + 5, y, 2, 7, c);
    Gfx_Fill(x + 5, y, 4, 2, c);
    Gfx_Fill(x + 1, y + 6, 5, 3, c);
    Gfx_Pixel(x, y + 7, c);
}

/* Everything about one row that decides what its pixels look like. Two
 * rows with the same signature are the same picture, so the row can be
 * left alone. */
/* Whether this row is the track that is playing.
 *
 * Both the signature and the drawing ask, and they have to agree - a
 * row drawn with the marker but hashed without it repaints every frame,
 * and the other way round it never repaints at all.
 *
 * Compared on the last path component of each, because a playlist row
 * can carry a folder in front of its name while the queue holds the
 * same track written another way. Both sides are the raw name off the
 * card: matching against the transliterated one meant the marker
 * appeared for ASCII filenames and never for anything that needed
 * folding, which is the opposite of useful. */
static bool RowIsPlaying(const FbEntry *e, const char *playingRaw)
{
    if (!playingRaw || e->isDir) return false;

    const char *a = strrchr(e->name, '/');
    a = a ? a + 1 : e->name;
    return strcmp(a, playingRaw) == 0;
}

/* Keyed on what the row shows, deliberately not on which slot shows it,
 * so a signature stays valid when the row moves. */
static uint32_t RowSignature(const FileBrowser *fb, int i,
                             const char *playingName)
{
    if (i >= fb->count) return 0;   /* 0 means "empty slot" */

    const FbEntry *e = &fb->entries[i];
    bool selected = (i == fb->selected);
    bool isPlaying = RowIsPlaying(e, playingName);

    uint32_t h = TextHash(e->name);
    h = (h * 31u) + (uint32_t)(e->isDir ? 1 : 0);
    h = (h * 31u) + (uint32_t)(selected ? 1 : 0);
    h = (h * 31u) + (uint32_t)(isPlaying ? 1 : 0);
    /* No slot-parity term. It used to pick the alternating row tint, but
     * the tint went when the selected row became a solid accent block -
     * DrawListRow's background is now just selected ? C_ACCENT : C_BG, and
     * `row` survives in it only to work out the y coordinate. Leaving the
     * parity in the hash cost nothing visible and broke scroll-shifting:
     * an odd shift flips every slot's parity, so every signature missed
     * and all nine rows were recomposed on exactly the one-row scroll that
     * holding the d-pad produces. Keyed on contents alone, a row's
     * signature travels with the row. */
    h = (h * 31u) + (uint32_t)(e->marked ? 1 : 0);
    h = (h * 31u) + (uint32_t)(e->isPlaylist ? 1 : 0);
    h = (h * 31u) + (uint32_t)(e->isLiked ? 1 : 0);

    /* The scrolling row changes every few frames without any of the
     * above changing, so the tick has to be part of what makes it
     * different - otherwise the incremental path decides the row is
     * already correct and never redraws it, and the name simply sits
     * there. That is exactly what the first version of this did. */
    if (selected && s_marqueeActive)
        h = (h * 31u) + s_marqueeTick;

    return h | 1u;                          /* never collide with "empty" */
}

static void DrawListRow(const FileBrowser *fb, int i, int row,
                        const char *playingName)
{
    int y = LIST_Y + 1 + row * ROW_H;
    const int h = ROW_H;

    /* Rows are repainted individually now, so each one lays down its own
     * background rather than relying on the panel fill underneath it
     * still being untouched. */
    if (i >= fb->count) {
        Gfx_Fill(LIST_X, y, LIST_W, h, C_BG);
        return;
    }

    const FbEntry *e = &fb->entries[i];
    bool selected = (i == fb->selected);

    /* The selected row is a solid accent block with a brighter bar down
     * its leading edge. That is one fill and one line, against a gradient
     * plus the alternating tint the old list needed to stay readable -
     * and a block of colour is easier to find with your eye than a row
     * that is merely a slightly different shade of blue. */
    Gfx_Fill(LIST_X, y, LIST_W, h, selected ? C_ACCENT : C_BG);
    if (selected)
        Gfx_Fill(LIST_X, y, 2, h, C_TEXT_ON_SEL);

    int iconY = y + (h - 9 * s_listScale) / 2;
    int textY = y + (h - GFX_GLYPH_H * s_listScale) / 2;

    bool isPlaying = RowIsPlaying(e, playingName);

    if (e->isDir)
        DrawFolderIcon(LIST_X + 6, iconY, selected ? C_TEXT_ON_SEL : C_TEXT_FOLDER);
    else if (e->isLiked)
        /* The same heart the Now Playing button uses, so the thing that
         * fills this list and the list itself carry one mark. Solid,
         * because the hollow one means "not liked" and a list of liked
         * tracks is not that. */
        Gfx_Icon16(LIST_X + 4, iconY - 1, LIKE_H, kHeartSolid,
                   selected ? C_TEXT_ON_SEL : C_ACCENT);
    else if (e->isPlaylist)
        DrawListIcon(LIST_X + 6, iconY, selected ? C_TEXT_ON_SEL : C_ACCENT);
    else if (isPlaying)
        DrawTriangle(LIST_X + 7, iconY + 4, 4, true, selected ? C_TEXT_ON_SEL : C_ACCENT);
    else
        DrawNoteIcon(LIST_X + 6, iconY, selected ? C_TEXT_ON_SEL : C_TEXT_DIM);

    /* Marked tracks carry a star at the right-hand edge, where it cannot
     * be confused with the icon that says what the row is. */
    if (e->marked)
        DrawStar(LIST_X + LIST_W - 10, y + h / 2,
                 selected ? C_TEXT_ON_SEL : C_ACCENT);

    uint16_t ink = selected ? C_TEXT_ON_SEL
                 : (e->isDir ? C_TEXT_FOLDER : (isPlaying ? C_TEXT : C_TEXT_DIM));

    /* The selected row's name sweeps sideways when it does not fit, so a
     * long filename can be read in full without opening it. Only the
     * selected one moves: every row marqueeing at once would be a mess to
     * read and would repaint the whole list every frame instead of one
     * row of it.
     *
     * The clipping is the text renderer's job now. The first attempt drew
     * the line overhanging its row and then painted the panel back over
     * both ends, which only worked because the list owned those pixels -
     * and left the row's icon to be redrawn afterwards. */
    /* Transliterated for drawing, and only for drawing.
     *
     * The name on the card is what opens the file and what goes into a
     * playlist, so it is left alone; what reaches the font is a fold of
     * it. The font is ASCII 32..126, so a Persian folder name arrived as
     * a row of question marks - the decoder had known how to romanise
     * that text for ID3 tags since long before this, and the list simply
     * never asked. Done here rather than stored per entry because it
     * costs a switch per character on the rows that actually repaint,
     * against 32KB of table for all 256 of them whether shown or not. */
    char shown[FB_NAME_LEN];
    TextFold_Utf8(e->name, shown, sizeof(shown));

    /* A playlist shows its name, not its filename.
     *
     * ".m3u" is how the card knows what the file is and is no use to
     * anyone reading a list of their own playlists - and on the Faves
     * tab, where every row is one, it is four characters of noise
     * repeated down the page. Trimmed from the copy being drawn, so what
     * opens the file is still the name on the card. */
    if (e->isPlaylist) {
        size_t n = strlen(shown);
        if (n > 4 && !strcasecmp(shown + n - 4, ".m3u"))       shown[n - 4] = '\0';
        else if (n > 5 && !strcasecmp(shown + n - 5, ".m3u8")) shown[n - 5] = '\0';
    }

    if (selected) {
        bool moving = Gfx_TextScroll(LIST_X + 20, textY, LIST_W - 28, ink,
                                     shown, s_marqueeTick, s_listScale * 4);
        s_marqueeActive = moving;
        return;
    }

    /* At double size the clip window does the trimming.
     *
     * This used to copy the name and chop a character off the end in a
     * loop, remeasuring the whole string each time - quadratic in the
     * name length, on every row of every redraw, to produce the same
     * pixels the renderer can clip to directly. */
    if (s_listScale == 2) {
        Gfx_SetClipX(LIST_X + 20, LIST_X + 20 + LIST_W - 28);
        Gfx_TextBig(LIST_X + 20, textY, 2, 6, ink, shown);
        Gfx_ResetClipX();
    } else {
        Gfx_TextClipped(LIST_X + 20, textY, LIST_W - 28, ink, C_SHADOW,
                        shown);
    }
}

/* Holding the d-pad is the case this exists for. Moving the selection
 * within the visible window changes two rows out of six and nothing else
 * on the screen, but the list used to repaint the path bar, the panel,
 * the scrollbar and all six rows on every press - and then the whole
 * bottom screen was flushed and DMA'd to VRAM. Each piece now carries a
 * signature and redraws only when its own picture changes, which also
 * narrows the row span the present step has to transfer. */
void Skin_DrawFileList(const FileBrowser *fb, const char *playingName)
{
    bool reset = s_contentReset;
    s_contentReset = false;

    if (reset) DrawContentBackdrop(SKIN_TAB_FILES);

    /* Path bar: which folder this is, and how many entries are in it. */
    uint32_t pathSig = TextHash(fb->inPlaylist ? fb->playlistName : fb->cwd) * 31u +
                       (uint32_t)(fb->inPlaylist ? 3u : 0u) +
                       (uint32_t)(fb->locked ? 5u : 0u) +
                       (uint32_t)fb->count * 2u + (fb->truncated ? 1u : 0u) +
                       (uint32_t)FileBrowser_MarkedCount(fb) * 7919u +
                       (uint32_t)(IsPressed(SKIN_BTN_MARK) ? 1u : 0u) * 131u +
                       (uint32_t)(IsPressed(SKIN_BTN_SAVELIST) ? 1u : 0u) * 137u +
                       (uint32_t)(IsPressed(SKIN_BTN_DELLIST) ? 1u : 0u) * 139u +
                       (uint32_t)(fb->selected >= 0 && fb->selected < fb->count &&
                                  fb->entries[fb->selected].isPlaylist ? 1u : 0u) * 149u +
                       (uint32_t)(fb->selected > 0 ? 1u : 0u) * 151u;
    if (reset || pathSig != s_pathSig) {
        s_pathSig = pathSig;

        Gfx_Fill(PATH_X, PATH_Y, PATH_W, PATH_H, C_SURFACE);

        int marked = FileBrowser_MarkedCount(fb);

        /* The three on the right edit lists: mark a track, add to a
         * list, take something out of one. Which of them are there
         * comes from what the list is showing, not from which tab is
         * showing it - and from the same function the hit test uses, so
         * one cannot be drawn without answering or answer without being
         * drawn. */
        SkinButton bIds[BROWSER_BTN_MAX];
        SkinRect   bRects[BROWSER_BTN_MAX];
        const int  bCount = BrowserButtons(fb, bIds, bRects);

        for (int i = 0; i < bCount; i++) {
            const SkinRect *r = &bRects[i];
            DrawButtonFace(r, IsPressed(bIds[i]));

            const int cx = r->x + r->w / 2, cy = r->y + r->h / 2;

            if (bIds[i] == SKIN_BTN_MARK) {
                DrawStar(cx, cy, marked ? C_ACCENT : C_TEXT_DIM);

            } else if (bIds[i] == SKIN_BTN_SAVELIST) {
                /* It saves what is marked, so it counts them. */
                char badge[8];
                snprintf(badge, sizeof(badge), "%d", marked);
                Gfx_Text(r->x + (r->w - Gfx_TextWidth(badge)) / 2, r->y + 3,
                         marked ? C_TEXT : C_TEXT_DIM, badge);

            } else {
                /* Lit only when it has something to act on - it is the
                 * one control here that destroys something, and an
                 * always-live button for that is an invitation.
                 *
                 * Inside a list it acts on the marks, or on the track
                 * under the cursor when there are none, so it is dark
                 * only on the ".." row. Outside, it acts on the list
                 * under the cursor - and never on Liked, which is not a
                 * list the user made and would take every heart with
                 * it. */
                const bool live = fb->inPlaylist
                    ? (marked > 0 || fb->selected > 0)
                    : (fb->selected >= 0 && fb->selected < fb->count &&
                       fb->entries[fb->selected].isPlaylist &&
                       !fb->entries[fb->selected].isLiked);

                Gfx_Fill(r->x + 6, r->y + 6, 10, 2, live ? C_TEXT : C_TEXT_DIM);
            }
        }

        /* The bar runs to the leftmost of them, wherever that is. */
        const int pathW = (bCount ? bRects[0].x : GFX_W - 8) - PATH_X - 22;

        /* Inside a playlist the bar names the playlist rather than the
         * folder - otherwise nothing on screen says which list these
         * tracks belong to. */
        if (fb->inPlaylist) {
            DrawListIcon(PATH_X + 4, PATH_Y + 2, C_ACCENT);
            char shownPl[FB_NAME_LEN];
            TextFold_Utf8(fb->playlistName, shownPl, sizeof(shownPl));

            /* Without the extension, like the rows. The bar names the
             * list you are inside; ".m3u" says what the card calls the
             * file, which is not the same question. */
            {
                size_t n = strlen(shownPl);
                if (n > 4 && !strcasecmp(shownPl + n - 4, ".m3u"))
                    shownPl[n - 4] = '\0';
                else if (n > 5 && !strcasecmp(shownPl + n - 5, ".m3u8"))
                    shownPl[n - 5] = '\0';
            }
            Gfx_TextClipped(PATH_X + 18, PATH_Y + 3, pathW,
                            C_ACCENT, C_SHADOW, shownPl);
        } else if (fb->truncated) {
            /* A folder with more entries than the browser can hold used
             * to show the first FB_MAX_ENTRIES and simply omit the rest -
             * silently, with nothing on screen to say so. Files that are
             * on the card and not in the list is the worst kind of bug to
             * leave quiet, so the path bar says it in the accent colour
             * and gives the count that did fit. */
            char shownPath[FB_PATH_LEN], full[FB_PATH_LEN + 24];
            TextFold_Utf8(fb->cwd, shownPath, sizeof(shownPath));
            snprintf(full, sizeof(full), "%s  (first %d)", shownPath, fb->count);
            DrawFolderIcon(PATH_X + 4, PATH_Y + 2, C_ACCENT);
            Gfx_TextClipped(PATH_X + 18, PATH_Y + 3, pathW,
                            C_ACCENT, C_SHADOW, full);
        } else if (fb->locked) {
            /* Not the path. The Faves tab is a set of saved lists, and
             * "/dsxp_info/playlists" is where they happen to be kept - a fact
             * about the card that the tab is not about, and one that
             * invites the reading that this is a folder you are standing
             * in and could climb out of. */
            /* Two stacked rules rather than the three of a single list:
             * the bar names the shelf and the rows name the lists on it,
             * and drawing the same mark for both said they were the same
             * kind of thing. */
            Gfx_Fill(PATH_X + 4, PATH_Y + 3, 9, 2, C_ACCENT);
            Gfx_Fill(PATH_X + 6, PATH_Y + 7, 9, 2, C_ACCENT);
            Gfx_TextClipped(PATH_X + 18, PATH_Y + 3, pathW,
                            C_TEXT, C_SHADOW, "Saved lists");
        } else {
            /* The path is folded like the rows are - a Persian folder
             * name is Persian in the bar above the list as well as in
             * it, and half a fix reads worse than none. */
            char shownPath[FB_PATH_LEN];
            TextFold_Utf8(fb->cwd, shownPath, sizeof(shownPath));
            DrawFolderIcon(PATH_X + 4, PATH_Y + 2, C_TEXT_FOLDER);
            Gfx_TextClipped(PATH_X + 18, PATH_Y + 3, pathW,
                            C_TEXT, C_SHADOW, shownPath);
        }
    }

    /* No list panel to lay down: it was a different colour from the page
     * behind it, and now it is not. The backdrop already put C_BG here and
     * the rows paint over it, so filling it a second time was 22,000
     * pixels of pure overdraw on every page switch. */

    uint32_t scrollSig = (uint32_t)fb->scrollTop * 65537u + (uint32_t)fb->count + 1u;
    if (reset || scrollSig != s_scrollSig) {
        s_scrollSig = scrollSig;
        DrawScrollbar(fb, reset);
    }

    if (fb->count == 0) {
        if (reset)
            Gfx_TextShadow(LIST_X + 10, LIST_Y + 8, C_TEXT_DIM, C_SHADOW,
                           "No MP3 files in this folder.");
        for (int row = 0; row < FB_MAX_VISIBLE_ROWS; row++) s_rowSig[row] = 0;
        s_rowScrollTop = -1;
        return;
    }

    /* Scrolling moves rows, it does not change them.
     *
     * The signature cache is keyed by slot, so a shift of one row made
     * every slot a miss and all nine rows were recomposed for a list that
     * had merely slid. Shift the pixels and the signatures together and
     * the loop below finds only the newly exposed row - and whichever two
     * rows changed selection - still needing to be drawn. */
    int visible = FileBrowser_VisibleRows();
    if (!reset && s_rowScrollTop >= 0 && fb->scrollTop != s_rowScrollTop) {
        int d = fb->scrollTop - s_rowScrollTop;
        if (d > -visible && d < visible) {
            Gfx_ScrollRect(LIST_X, LIST_Y + 1, LIST_W, visible * ROW_H,
                           -d * ROW_H);
            if (d > 0) {
                for (int r = 0; r + d < visible; r++)
                    s_rowSig[r] = s_rowSig[r + d];
                for (int r = visible - d; r < visible; r++)
                    s_rowSig[r] = ROWSIG_UNKNOWN;
            } else {
                for (int r = visible - 1; r + d >= 0; r--)
                    s_rowSig[r] = s_rowSig[r + d];
                for (int r = 0; r < -d; r++)
                    s_rowSig[r] = ROWSIG_UNKNOWN;
            }
        }
    }
    s_rowScrollTop = fb->scrollTop;

    for (int row = 0; row < FileBrowser_VisibleRows(); row++) {
        int i = fb->scrollTop + row;
        uint32_t sig = RowSignature(fb, i, playingName);

        if (!reset && sig == s_rowSig[row]) continue;
        s_rowSig[row] = sig;

        DrawListRow(fb, i, row, playingName);
    }
}

/* ------------------------------------------------------------------ */
/* content: now playing                                               */
/* ------------------------------------------------------------------ */

/* Shuffle: two routes that cross and swap sides, each ending in an
 * arrowhead on the right.
 *
 * The straight stubs entering from the left are what stop this reading as
 * a letter X - the idea is two paths being exchanged, and a bare crossing
 * does not say that. Drawn with primitives because an 8x8 bitmap cannot
 * hold two diagonals and two arrowheads without them merging. */
/* Shuffle and repeat, as bitmaps rather than a dozen placed rectangles.
 *
 * The old pair were assembled from stubs and stacked fills, and at this
 * size that is drawing blind - the arrowheads came out as nubs and the
 * repeat glyph read as a box with a spike. These were drawn on a grid,
 * looked at, and adjusted, which is the only way to get a 16 by 14 icon
 * right.
 *
 * Shuffle is two paths that swap height and cross, each ending in a
 * head. Repeat is one loop broken at opposite corners, each break ending
 * in a head - which is what says "cycle" rather than "rectangle". */
#define ICON_W 16
#define ICON_H 16

/* Redrawn to sit in the middle of its cell. The first version ran from
 * column 0 to column 12 of sixteen, so the glyph hugged the left edge of
 * its button while the repeat icon beside it was centred - which is what
 * made the pair look misaligned rather than either one look wrong. */
static const uint16_t kIconShuffle[ICON_H] = {
    /* Drawn as one path and mirrored about the centre line, so the two
     * arrows cannot disagree. The head is wider than it is tall for the
     * same reason the first attempt failed: a tall head meets its own
     * mirror in the middle and the pair reads as one vertical bar. */
    0x0000, 0x0000, 0x0000, 0x000C, 0x7C1E, 0x7E3E, 0x036C, 0x01C0,
    0x01C0, 0x036C, 0x7E3E, 0x7C1E, 0x000C, 0x0000, 0x0000, 0x0000,
};

static const uint16_t kIconRepeat[ICON_H] = {
    0x0000, 0x0000, 0x3FF0, 0x3FF0, 0x3030, 0x3034, 0x3036, 0x303E,
    0x3036, 0x3034, 0x3030, 0x3FF0, 0x3FF0, 0x0000, 0x0000, 0x0000,
};

/* Repeat-one: the same loop with a digit in it.
 *
 * Sixteen rows rather than fourteen, which is the whole reason the digit
 * reads. At fourteen the interior is eight by six, a five-tall 1 has one
 * row of slack in total, and it fuses with the outline into a solid
 * block - which is exactly what the first attempt drew. Two more rows
 * make the interior eight by eight and the digit sits clear of it. */
static const uint16_t kIconRepeatOne[ICON_H] = {
    0x0000, 0x0000, 0x3FF0, 0x3FF0, 0x3330, 0x3734, 0x3336, 0x333E,
    0x3336, 0x3334, 0x37B0, 0x3FF0, 0x3FF0, 0x0000, 0x0000, 0x0000,
};

/* Transport glyphs, on the same 16x16 grid as the two mode icons.
 *
 * They were shapes before - a triangle helper, a couple of fills - and
 * the arithmetic centred their bounding boxes. That is not the same as
 * looking centred: a play triangle carries its mass at the base, so a
 * bounding box centred one sits visibly left in its button. These were
 * drawn out and the ink measured, and the play glyph is deliberately a
 * pixel right of centre for that reason.
 *
 * Prev and next are the same shape mirrored, with matching margins, so
 * the row reads as one set rather than five separate drawings. */
/* Volume ends for the minimal style, where a plated + and - would be the
 * only plates left on the row. A speaker says the same thing without
 * one, and says which end is which without relying on a sign that is two
 * pixels of difference at this size. */
static const uint16_t kGlyphSpkQuiet[ICON_H] = {
    0x0000, 0x0000, 0x0000, 0x0080, 0x0180, 0x0380, 0x1F80, 0x1F80,
    0x1F80, 0x0380, 0x0180, 0x0080, 0x0000, 0x0000, 0x0000, 0x0000,
};
/* Two clean strokes, a short one and a tall one. Sound waves as arcs
 * need a curve, and at four pixels of radius a curve is two pixels and a
 * corner - it came out looking like a zero beside the cone. Bars of
 * different heights say "more" without pretending to be anything. */
static const uint16_t kGlyphSpkLoud[ICON_H] = {
    0x0000, 0x0000, 0x0000, 0x0080, 0x0190, 0x0390, 0x1F94, 0x1F94,
    0x1F94, 0x0390, 0x0190, 0x0080, 0x0000, 0x0000, 0x0000, 0x0000,
};

static const uint16_t kGlyphPrev[ICON_H] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x3084, 0x318C, 0x339C, 0x37BC,
    0x3FFC, 0x37BC, 0x339C, 0x318C, 0x3084, 0x0000, 0x0000, 0x0000,
};
static const uint16_t kGlyphPlay[ICON_H] = {
    0x0000, 0x0000, 0x0400, 0x0600, 0x0700, 0x0780, 0x07C0, 0x07E0,
    0x07F0, 0x07E0, 0x07C0, 0x0780, 0x0700, 0x0600, 0x0400, 0x0000,
};
static const uint16_t kGlyphPause[ICON_H] = {
    0x0000, 0x0000, 0x1E78, 0x1E78, 0x1E78, 0x1E78, 0x1E78, 0x1E78,
    0x1E78, 0x1E78, 0x1E78, 0x1E78, 0x1E78, 0x1E78, 0x0000, 0x0000,
};
static const uint16_t kGlyphNext[ICON_H] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x210C, 0x318C, 0x39CC, 0x3DEC,
    0x3FFC, 0x3DEC, 0x39CC, 0x318C, 0x210C, 0x0000, 0x0000, 0x0000,
};


static void DrawShuffleIcon(int x, int y, uint16_t c)
{
    Gfx_Icon16(x, y, ICON_H, kIconShuffle, c);
}

static void DrawRepeatIcon(int x, int y, uint16_t c, bool one)
{
    Gfx_Icon16(x, y, ICON_H, one ? kIconRepeatOne : kIconRepeat, c);
}

/* Centred text, which only means anything when the text fits.
 *
 * A line too long for its box already occupies the whole box, so there is
 * no ambiguity and no jitter: short titles sit in the middle, long ones
 * fill the width and sweep. */
/* The largest scale the title fits at, so the page uses the room it has.
 *
 * Most track names are short and there is space for triple height now the
 * cover is gone; the ones that are not fall back rather than sweeping a
 * huge font past at six characters a second. The scale has to be chosen
 * before anything is drawn, because it decides how tall the block is and
 * so where the block starts. */
/* One size, always. Picking the largest that fits meant a short name
 * arrived a third taller than a long one, so the page changed shape
 * between tracks - and the big one was too big anyway. */
/* Two and a half. Double was small and triple was large. */
/* Two and a quarter times, and drawn heavy.
 *
 * In quarters, which is why a quarter step is available at all: halves
 * put the choice between two and a half - a shade too large - and two,
 * which is the size the file list uses and reads as a row rather than as
 * the answer to "what is playing". The weight is what carries it at this
 * size; the font's own strokes are a pixel and this thickens them by
 * about half of one, which is a different face rather than a larger. */
#define TITLE_SCALE 9

/* The title block's height, and the first row it may start on.
 *
 * It is centred in the band by eye - three fifths of the slack above it -
 * but never higher than the bottom of the like heart. The heart is in
 * the top right corner of the same band and the title is centred across
 * the whole width, so a long name would run underneath it.
 *
 * That used to be guaranteed by the block being short enough that the
 * three fifths always cleared the heart on its own, which is why the
 * title could not grow: at four times it starts six rows too high. The
 * floor says the thing that was actually meant, and the block is then
 * asserted to still fit between the heart and the clock row. */
/* Measured to the clock row rather than to a number near it. It was 82,
 * which is three rows past where the elapsed time starts - room the
 * title block did not use while it was short, and quietly took the
 * moment it grew: the artist came out sitting on the clock. */
#define NP_ARTIST_GAP  10
#define NP_TEXT_TOP_H  (NP_CLOCK_Y - CONTENT_Y)
#define NP_TITLE_BLOCK ((TITLE_SCALE * GFX_GLYPH_H) / 4 \
                        + NP_ARTIST_GAP + GFX_GLYPH_H)
#define NP_TITLE_MIN_Y (LIKE_HIT_Y + LIKE_HIT_H + GFX_BIG_BOLD_PAD)

_Static_assert(NP_TITLE_MIN_Y + NP_TITLE_BLOCK <= NP_CLOCK_Y,
               "the title and its artist do not fit between the like heart "
               "and the clock row");

/* The title's heavier strokes reach below its box too, and the artist
 * sits in that gap. */
_Static_assert(NP_ARTIST_GAP > GFX_BIG_BOLD_PAD,
               "the artist line runs into the bottom of the title");
_Static_assert(LIKE_Y >= CONTENT_Y && LIKE_X + LIKE_W <= GFX_W - 4,
               "the like heart is off the page or under the tab bar");

static int TitleScale(const char *s, int boxW)
{
    (void)s; (void)boxW;
    return TITLE_SCALE;
}

static bool DrawTitleCentred(int x, int boxW, int y, uint16_t c,
                             const char *s, uint32_t tick, int scale)
{
    int tw = Gfx_TextBigPropWidth(scale, s);
    if (tw <= boxW) {
        Gfx_TextBigProp(x + (boxW - tw) / 2, y, scale, c, s);
        return false;
    }
    return Gfx_TextScroll(x, y, boxW, c, s, tick, scale);
}

static void DrawLineCentred(int x, int boxW, int y, uint16_t c, const char *s)
{
    int tw = Gfx_TextWidth(s);
    if (tw <= boxW) Gfx_TextShadow(x + (boxW - tw) / 2, y, c, GFX_NO_SHADOW, s);
    else            Gfx_TextClipped(x, y, boxW, c, GFX_NO_SHADOW, s);
}


/* "13_Kholy.mp3" is a filename, and this line is answering what plays
 * next rather than what is on the card. The extension is the same on
 * every row and carries nothing; the name is what is being read. */
static void StripExtension(char *dst, size_t n, const char *s)
{
    snprintf(dst, n, "%s", s);
    char *dot = strrchr(dst, '.');
    if (dot && dot != dst && strlen(dot) <= 5) *dot = '\0';
}

static void DrawControlPlate(bool shuffle, int repeatMode)
{
    /* One plate behind the whole control row, drawn here because this
     * function owns the page and runs before Skin_DrawTransport puts the
     * other three controls on top of it. Each of those clears itself to
     * this surface, so none of them punches through. */
    FillRounded(STACK_BOX_X, STACK_BOX_Y, STACK_BOX_W, STACK_BOX_H,
                2, C_RAISED_HI);
    FillRounded(STACK_BOX_X + 1, STACK_BOX_Y + 1,
                STACK_BOX_W - 2, STACK_BOX_H - 2, 2, C_SURFACE);

    /* Shuffle and repeat are states, not actions, so they take the
     * accent when on and the dim text colour when off - never the plain
     * white prev and next use, which would make five identical controls
     * out of three actions and two switches. */
    const SkinRect *sh = &kRects[SKIN_BTN_SHUFFLE];
    const SkinRect *rp = &kRects[SKIN_BTN_REPEAT];

    /* Touched, they light their whole cell like prev and next do. They
     * are drawn here rather than in the transport loop, which is how they
     * came to be the only two controls on the plate with no pressed state
     * at all - the treatment lived in the loop they are not part of. */
    Gfx_Fill(sh->x, sh->y, sh->w, sh->h, C_SURFACE);
    if (IsPressed(SKIN_BTN_SHUFFLE))
        FillRounded(sh->x, sh->y, sh->w, sh->h, 2, C_ACCENT);
    DrawShuffleIcon(sh->x + (sh->w - ICON_W) / 2, sh->y + (sh->h - ICON_H) / 2,
                    IsPressed(SKIN_BTN_SHUFFLE) ? C_TEXT_ON_SEL
                                                : (shuffle ? C_ACCENT : C_TEXT_DIM));

    Gfx_Fill(rp->x, rp->y, rp->w, rp->h, C_SURFACE);
    if (IsPressed(SKIN_BTN_REPEAT))
        FillRounded(rp->x, rp->y, rp->w, rp->h, 2, C_ACCENT);
    DrawRepeatIcon(rp->x + (rp->w - ICON_W) / 2, rp->y + (rp->h - ICON_H) / 2,
                   IsPressed(SKIN_BTN_REPEAT) ? C_TEXT_ON_SEL
                       : (repeatMode != REPEAT_OFF ? C_ACCENT : C_TEXT_DIM),
                   repeatMode == REPEAT_ONE);

}

void Skin_DrawNowPlaying(const char *title, const char *artist,
                         bool hasTrack, bool isPlaying,
                         uint32_t sampleRate, uint32_t bitrateKbps, int channels,
                         bool vbr, int queuePos, int queueLen,
                         const char *nextName, bool shuffle, int repeatMode,
                         bool liked)
{
    /* The title is what this page is for, so it gets double-height text
     * and the whole width. Everything that was competing with it - the
     * sample rate, the bitrate, the channel count, the word "Playing" -
     * is either demoted to one dim line or dropped, because none of it is
     * what anyone came to this screen to read.
     *
     * Nothing shares the row with it. A cover lived here briefly and took
     * a third of the width to show sixty pixels of picture; the title is
     * what makes this screen useful on a moving console, and it gets the
     * whole line. */
    uint32_t sig = TextHash(title) * 31u + TextHash(artist);
    sig = sig * 31u + TextHash(nextName);
    sig = sig * 31u + sampleRate;
    sig = sig * 31u + bitrateKbps;
    sig = sig * 31u + (uint32_t)(channels * 4 + (vbr ? 2 : 0) + (hasTrack ? 1 : 0));
    sig = sig * 31u + (uint32_t)(queuePos * 2 + (isPlaying ? 1 : 0));
    sig = sig * 31u + (uint32_t)queueLen;
    sig = sig * 31u + (uint32_t)(repeatMode * 2 + (shuffle ? 1 : 0));

    /* Whether these two are lit, not just what they are set to.
     *
     * They are the only controls drawn by this page rather than by the
     * transport strip, and the strip keys its buttons on IsPressed while
     * this signature did not - so the page could early-out with a
     * highlight that had expired still on screen. It also made the
     * highlight arrive late: pressing shuffle only repainted because the
     * shuffle setting itself had changed, so the light appeared with the
     * action rather than with the finger, and pressing one that changed
     * nothing else showed nothing at all until something else moved. */
    sig = sig * 31u + (uint32_t)((IsPressed(SKIN_BTN_SHUFFLE) ? 2u : 0u) |
                                 (IsPressed(SKIN_BTN_REPEAT)  ? 1u : 0u) |
                                 (IsPressed(SKIN_BTN_LIKE)    ? 4u : 0u) |
                                 (liked                       ? 8u : 0u));
    sig |= 1u;

    bool reset = s_contentReset;
    s_contentReset = false;

    if (s_npScrolling) sig += s_marqueeTick * 131u;
    if (!reset && sig == s_npSig) return;
    s_npSig = sig;

    /* The whole page, every time.
     *
     * Erasing only the bands that hold text was cheaper and left a seam:
     * the strip between them is painted once and then only written to by
     * the controls, so anything that ever landed there off their edges
     * stayed. One fill is a few thousand 32-bit stores and removes the
     * whole class of problem; the controls are redrawn with it. */
    /* Every theme erases the whole page. Blue Classic used to spare the
     * margins a bezel occupied - eight columns at each side and six rows
     * at the foot - and with the bezel gone nothing painted them, so
     * whatever the last page left there stayed. */
    const int TEXT_TOP_H = NP_TEXT_TOP_H;

    Gfx_Fill(0, CONTENT_Y, GFX_W, GFX_H - CONTENT_Y, C_BG);

    /* That fill just wiped the controls, and Skin_DrawTransport redraws
     * a button only when that button's own state has changed - so
     * without this they stay erased until something happens to them.
     * Stopping cleared the page and the transport had no reason to
     * repaint, which is why the controls vanished. */
    s_transportReset = true;

    const int x = 10;

    if (!hasTrack) {
        /* Both lines centred, and both clear of the clock row.
         *
         * The hint was drawn at y=76 and stands eight tall, while the
         * transport puts its elapsed time at STACK_PROG_Y - 11 = 79 - so
         * the two were written over each other, which is the mess an
         * empty page showed. The transport is drawn by the caller after
         * this returns, so this page cannot see it and has to leave the
         * room; the assert below is what remembers that.
         *
         * Centred to match the real title, which is: an empty page that
         * lays its text out differently reads as a different screen. */
        const char *l1 = "Nothing";
        const char *l2 = "Pick a track on the Files tab.";

        Gfx_TextBig((GFX_W - Gfx_TextBigWidth(2, 7, l1)) / 2,
                    NP_EMPTY_Y, 2, 7, C_TEXT_DIM, l1);
        Gfx_Text((GFX_W - Gfx_TextWidth(l2)) / 2,
                 NP_EMPTY_Y + 22, C_TEXT_DIM, l2);

        /* The plate and the two mode keys, the same as when a track is
         * loaded. Returning before them left the caller's three transport
         * buttons standing on nothing, with shuffle and repeat missing
         * either side - half a control row, which looks like a drawing
         * fault rather than an empty player. */
        DrawControlPlate(shuffle, repeatMode);
        return;
    }

    const int textX = x;
    const int w = GFX_W - textX - 10;

    const bool hasArtist = (artist && artist[0]);
    const int titleScale = TitleScale(title, w);

    int blockH = (GFX_GLYPH_H * titleScale) / 4;
    if (hasArtist) blockH += NP_ARTIST_GAP + GFX_GLYPH_H;

    int slack = TEXT_TOP_H - blockH;
    if (slack < 0) slack = 0;
    /* One row lower than the share of the slack alone would put it, so
     * the title has air under the heart rather than sitting against it
     * even when there is room to spare. */
    int y = CONTENT_Y + (slack * 3) / 5 + 1;
    if (y < NP_TITLE_MIN_Y) y = NP_TITLE_MIN_Y;   /* clear of the heart */

    Gfx_SetBigBold(true);
    s_npScrolling = DrawTitleCentred(textX, w, y, C_TEXT, title,
                                     s_marqueeTick, titleScale);
    Gfx_SetBigBold(false);
    y += (GFX_GLYPH_H * titleScale) / 4;

    if (hasArtist) {
        y += NP_ARTIST_GAP;
        DrawLineCentred(textX, w, y, C_ACCENT, artist);
    }

    /* The heart, in the band above the title.
     *
     * Drawn here rather than on the control plate, where it started: it
     * is about the track and not about playback, and the plate is the
     * five things that move the music. The shape carries whether the
     * track is a favourite - hollow or solid - and the colour only says
     * whether a finger is on it, because a single control has no second
     * state beside it to make a colour difference legible on its own.
     *
     * The whole band is repainted first: this page is drawn on a
     * signature that includes the liked flag, so a change gets here, but
     * the previous heart's pixels are still underneath. */
    {
        const SkinRect *lk = &kRects[SKIN_BTN_LIKE];

        /* No box: the border is the heart's own ring.
         *
         * The dark ring goes down in both states, so the shape has an
         * edge against whatever is behind it either way. On top of it
         * goes the body - solid when liked, its own outline when not -
         * and both occupy the same thirteen by eleven, so the two states
         * are one shape filling and emptying rather than two marks of
         * different sizes swapping places. */
        Gfx_Fill(lk->x, lk->y, lk->w, lk->h, C_BG);

        /* The press lights the heart, not the target it is in.
         *
         * It used to fill the whole hit rectangle, which was about the
         * size of the heart and so looked like the heart lighting up.
         * The rectangle is nearly twice that now - it has to be, for a
         * stylus to land in it - and filling it drew a block in the
         * corner of the screen that is otherwise invisible. A touch
         * target is for touching, not for looking at. */
        if (IsPressed(SKIN_BTN_LIKE))
            FillRounded(LIKE_X - 2, LIKE_Y - 2, LIKE_W + 4, LIKE_H + 4,
                        3, C_ACCENT);

        const uint16_t ink = IsPressed(SKIN_BTN_LIKE) ? C_TEXT_ON_SEL
                           : (liked ? C_ACCENT : C_TEXT);

        if (!IsPressed(SKIN_BTN_LIKE))
            Gfx_Icon16(LIKE_X, LIKE_Y, LIKE_H, kHeartRing, C_SHADOW);

        Gfx_Icon16(LIKE_X, LIKE_Y, LIKE_H,
                   liked ? kHeartSolid : kHeartHollow, ink);
    }

    DrawControlPlate(shuffle, repeatMode);
}

/* ------------------------------------------------------------------ */
/* content: sleep timer                                               */
/* ------------------------------------------------------------------ */

/* A crescent moon, drawn as one filled disc with a second punched out of
 * it in the background colour. */

/* One option row: a tick box plus its label, the whole row being the
 * touch target. */
static void DrawCheckbox(SkinButton which, bool checked, const char *label)
{
    const SkinRect *r = &kRects[which];
    const int boxSize = 13;
    int boxY = r->y + (r->h - boxSize) / 2;

    Gfx_Fill(r->x, boxY, boxSize, boxSize, checked ? C_ACCENT : C_TROUGH);

    if (checked) {
        /* A tick drawn as two strokes rather than a glyph, so it stays
         * crisp at this size. */
        for (int i = 0; i < 4; i++) {
            Gfx_Pixel(r->x + 3 + i, boxY + 6 + i, C_TEXT_ON_SEL);
            Gfx_Pixel(r->x + 3 + i, boxY + 7 + i, C_TEXT_ON_SEL);
        }
        for (int i = 0; i < 6; i++) {
            Gfx_Pixel(r->x + 6 + i, boxY + 9 - i, C_TEXT_ON_SEL);
            Gfx_Pixel(r->x + 6 + i, boxY + 10 - i, C_TEXT_ON_SEL);
        }
    }

    Gfx_TextShadow(r->x + boxSize + 7, r->y + (r->h - GFX_GLYPH_H) / 2,
                   checked ? C_TEXT : C_TEXT_DIM, C_SHADOW, label);
}

/* The page splits where the drawing does: everything about the timer
 * itself above, the two options and the start button below. The countdown
 * reruns the top block once a second while armed, and the bottom block -
 * three bevelled controls - is skipped entirely for it. */
#define SLEEP_TIMER_Y   SLEEP_TIME_Y

/* The bottom of the timer block, derived from the knob rather than
 * written down beside it.
 *
 * The knob overhangs the slider track by three pixels at each end, so the
 * erase has to reach past the track or a sliver of the old one survives
 * every drag. Written out as a height of 52 it reached row 87 - and row
 * 87 is the first row of the power-off checkbox, which lives in the
 * options block and is only repainted when the options' own signature
 * changes. So a countdown tick, or a drag of the minutes, erased the top
 * of that checkbox and left it erased: one row, thirteen pixels wide,
 * for as long as the page stayed up.
 *
 * Derived, the two cannot drift apart again, and the assert below says so
 * if the layout ever moves. */
#define SLEEP_TIMER_BOTTOM (SLEEP_CHIP_Y + SLEEP_CHIP_H)
#define SLEEP_TIMER_H      (SLEEP_TIMER_BOTTOM - SLEEP_TIMER_Y)

/* Where the options actually start drawing - the first checkbox's own
 * rect, not a number three rows below it. At 89 the options erase did not
 * cover the checkbox it was erasing for, which was the same bug waiting
 * on the other side of the split. */
#define SLEEP_OPT_Y     (CONTENT_Y + 100)
#define SLEEP_OPT_H     (CONTENT_Y + CONTENT_H - SLEEP_OPT_Y)

_Static_assert(SLEEP_TIMER_BOTTOM <= SLEEP_OPT_Y,
               "the timer erase reaches into the options block, which is "
               "repainted only when its own signature changes - so whatever "
               "it erases there stays erased");

void Skin_DrawSleep(int minutes, bool armed, uint32_t remainingSec,
                    bool powerOff)
{
    char line[48];

    bool reset = s_contentReset;
    s_contentReset = false;

    if (reset) DrawContentBackdrop(SKIN_TAB_SLEEP);

    uint32_t timerSig = ((uint32_t)minutes << 1) + (armed ? 1u : 0u);
    if (armed) timerSig += (remainingSec + 1u) << 9;
    timerSig |= 1u;

    uint32_t optSig = (powerOff ? 1u : 0u) |
                      (armed ? 4u : 0u) |
                      (IsPressed(SKIN_BTN_SLEEP_TOGGLE) ? 8u : 0u) | 16u;

    if (!reset && timerSig == s_sleepTimerSig && optSig == s_sleepOptSig)
        return;

    if (reset || timerSig != s_sleepTimerSig) {
    s_sleepTimerSig = timerSig;
    if (!reset) EraseContent(0, SLEEP_TIMER_Y, GFX_W, SLEEP_TIMER_H);

    /* No "Sleep timer" heading: the tab above already says Sleep, and
     * the line was taking a row of a page that needed the room more than
     * it needed to repeat itself. */
    /* The countdown is the page. Centred and large, because someone
     * opening this is asking how long is left and everything else here is
     * setting up for that answer. Stopped, the same place shows the
     * duration that would be armed - one number in one position, rather
     * than two numbers competing. */
    if (armed) {
        uint32_t h = remainingSec / 3600;
        uint32_t m = (remainingSec / 60) % 60;
        uint32_t sec = remainingSec % 60;
        if (h > 0) snprintf(line, sizeof(line), "%lu:%02lu:%02lu",
                            (unsigned long)h, (unsigned long)m, (unsigned long)sec);
        else       snprintf(line, sizeof(line), "%lu:%02lu",
                            (unsigned long)m, (unsigned long)sec);
    } else {
        snprintf(line, sizeof(line), "%d min", minutes);
    }
    {
        int tw = Gfx_TextBigWidth(2, SLEEP_TIME_ADV, line);
        Gfx_TextBig((GFX_W - tw) / 2, SLEEP_TIME_Y, 2, SLEEP_TIME_ADV,
                    armed ? C_ACCENT : C_TEXT, line);
    }

    /* How much of the timer has run, drawn with the same slider the
     * progress and volume rows use - groove, fill and a disc on the
     * front of it. Written by hand it was a plain bar, which was the one
     * gauge on the player that did not look like the others.
     *
     * The disc marks the position; it is not a grab handle. Dragging a
     * countdown has nothing to mean - the number it would set is the one
     * the clock is already deciding - so the page does not offer it, and
     * a tap on this row does nothing rather than something surprising.
     *
     * The track is drawn whether or not anything is armed, so starting a
     * timer does not change the shape of the page under the reader. */
    {
        int permille;
        if (armed && minutes > 0) {
            /* Running: how much of it has gone. */
            uint32_t total = (uint32_t)minutes * 60u;
            uint32_t gone  = (remainingSec < total) ? total - remainingSec : total;
            permille = (int)(((uint64_t)gone * 1000u) / total);
        } else {
            /* Stopped: where the chosen duration sits in the range, and
             * draggable to set it.
             *
             * It used to sit flat at zero whenever nothing was armed,
             * which made it look broken - tapping 15 moved the chips and
             * left the bar exactly where it was, so the one control that
             * looks like it should follow along did not. The chips are
             * still the quick way to a round number; this is how to get
             * to the ones between them without the d-pad. */
            int span = PLAYER_SLEEP_MAX_MINUTES - PLAYER_SLEEP_MIN_MINUTES;
            permille = span > 0
                     ? ((minutes - PLAYER_SLEEP_MIN_MINUTES) * 1000) / span
                     : 0;
        }
        DrawSliderBar(SLEEP_BAR_X, SLEEP_BAR_Y, SLEEP_BAR_W, SLEEP_BAR_H,
                      permille);
    }

    /* Five durations, one tap each. The one in use is filled; a value
     * trimmed to something in between with the d-pad lights none of them,
     * which is the honest thing for it to show. */
    for (int i = 0; i < SLEEP_PRESETS; i++) {
        int x = SLEEP_CHIP_X(i);
        bool on = (minutes == (int)kSleepPresets[i]);

        FillRounded(x, SLEEP_CHIP_Y, SLEEP_CHIP_W, SLEEP_CHIP_H, 2,
                    on ? C_ACCENT : C_RAISED);
        if (!on)
            Gfx_HLine(x, SLEEP_CHIP_Y, SLEEP_CHIP_W, C_RAISED_HI);

        char n[8];
        snprintf(n, sizeof(n), "%u", (unsigned)kSleepPresets[i]);
        Gfx_Text(x + (SLEEP_CHIP_W - Gfx_TextWidth(n)) / 2, SLEEP_CHIP_Y + 4,
                 on ? C_TEXT_ON_SEL : C_TEXT, n);
        Gfx_Text(x + (SLEEP_CHIP_W - Gfx_TextWidth("min")) / 2,
                 SLEEP_CHIP_Y + SLEEP_CHIP_H - GFX_GLYPH_H - 4,
                 on ? C_TEXT_ON_SEL : C_TEXT_DIM, "min");
    }

    /* Range labels sit level with the value readout rather than under the
     * slider, which keeps the row below free for the option checkboxes. */
    /* No "1" and "120" at the ends. The value is printed above the bar
     * and the knob shows where in the range it sits, so the two numbers
     * were labelling something already legible and crowding the line
     * that mattered. */
    }

    if (reset || optSig != s_sleepOptSig) {
        s_sleepOptSig = optSig;
        if (!reset) EraseContent(0, SLEEP_OPT_Y, GFX_W, SLEEP_OPT_H);

        /* One tick. "Blank screens when idle" was never about the sleep
         * timer - it is about blanking during ordinary playback, and it
         * still lives on the Setup page where that belongs. Here it read
         * as a third thing the timer might do, beside two that it does.
         *
         * What the timer does is fixed now: it stops, and it turns the
         * screens off. This tick says whether to go further and shut the
         * console down. */
        DrawCheckbox(SKIN_BTN_SLEEP_POWEROFF, powerOff, "Power off at the end");

        const SkinRect *btn = &kRects[SKIN_BTN_SLEEP_TOGGLE];
        DrawButtonFace(btn, IsPressed(SKIN_BTN_SLEEP_TOGGLE));

        const char *label = armed ? "Cancel" : "Start";
        int lw = Gfx_TextWidth(label);
        Gfx_TextShadow(btn->x + (btn->w - lw) / 2,
                       btn->y + (btn->h - GFX_GLYPH_H) / 2,
                       IsPressed(SKIN_BTN_SLEEP_TOGGLE) ? C_TEXT_ON_SEL : C_TEXT,
                       C_SHADOW, label);
    }
}

/* ------------------------------------------------------------------ */
/* content: colour picker                                             */
/* ------------------------------------------------------------------ */

/* Eight across and four down covers the thirty-two exactly, with no
 * ragged last row and no scrolling - the whole palette is on screen at
 * once, which is the point of a colour picker. */
/* One step around a grid of `count` cells laid out `cols` wide.
 *
 * Shared by the four pages that are grids, because each of them was
 * doing this arithmetic itself - and two of them were doing it in
 * main.c, from a column count written out as a number beside the one in
 * this file. The build asserts that each grid holds its whole list, so a
 * grid that does not divide evenly cannot ship; nothing tied the two
 * copies of the width together, so a grid redrawn six wide instead of
 * eight would have left the d-pad stepping by the old number, which no
 * assert would have caught and nothing on screen would have explained.
 *
 * A short last row is handled rather than asserted away: stepping down
 * into a column it does not have lands on its last cell, and stepping
 * sideways along it wraps within the cells that are there. */
static int GridStep(int from, int count, int cols, int dx, int dy)
{
    if (count < 1 || cols < 1) return 0;
    if (from < 0 || from >= count) from = 0;

    const int rows = (count + cols - 1) / cols;
    int col = from % cols, row = from / cols;

    if (dy) {
        row = (row + dy % rows + rows) % rows;
        if (row * cols + col >= count) col = (count - 1) % cols;
    }

    if (dx) {
        int len = count - row * cols;
        if (len > cols) len = cols;
        col = (col + dx % len + len) % len;
    }

    const int at = row * cols + col;
    return (at >= 0 && at < count) ? at : from;
}

#define COLOR_COLS  8
#define COLOR_ROWS  4
#define COLOR_SW    28
#define COLOR_GAP   3
#define COLOR_GRID_W (COLOR_COLS * COLOR_SW + (COLOR_COLS - 1) * COLOR_GAP)
#define COLOR_X      ((GFX_W - COLOR_GRID_W) / 2)

/* The page takes the whole screen rather than sitting under the tab bar.
 * It is a chooser you are in and then out of, not a tab you browse, and
 * the tabs are no use while it is up - so the room goes to the swatches
 * and they get big enough to hit with a thumb. */
#define COLOR_TITLE_Y   6
#define COLOR_PREVIEW_Y 18
#define COLOR_PREVIEW_H 18
#define COLOR_GRID_Y    46
#define COLOR_HINT_Y    174

_Static_assert(COLOR_COLS * COLOR_ROWS == SKIN_ACCENTS,
               "the grid and the palette have to hold the same number");
_Static_assert(COLOR_GRID_Y + COLOR_ROWS * COLOR_SW +
               (COLOR_ROWS - 1) * COLOR_GAP + 3 <= COLOR_HINT_Y,
               "the swatch grid, with room for its selection ring, runs "
               "into the hint line below it");
_Static_assert(COLOR_X >= 3 && COLOR_X + COLOR_GRID_W + 3 <= GFX_W,
               "the selection ring falls off the side of the screen");

static void ColorCell(int i, int *x, int *y)
{
    *x = COLOR_X + (i % COLOR_COLS) * (COLOR_SW + COLOR_GAP);
    *y = COLOR_GRID_Y + (i / COLOR_COLS) * (COLOR_SW + COLOR_GAP);
}

int Skin_HitTestColor(int px, int py)
{
    for (int i = 0; i < SKIN_ACCENTS; i++) {
        int x, y;
        ColorCell(i, &x, &y);
        if (px >= x && px < x + COLOR_SW && py >= y && py < y + COLOR_SW)
            return i;
    }
    return -1;
}

void Skin_DrawColors(int focus)
{
    bool reset = s_contentReset;
    s_contentReset = false;

    /* No signature early-out, and the whole screen every time. The page
     * is thirty-two flat fills and a bar; there is nothing here worth the
     * machinery, and it is only up while somebody is choosing. */
    (void)reset;
    Gfx_Fill(0, 0, GFX_W, GFX_H, C_BG);

    Gfx_Text(COLOR_X, COLOR_TITLE_Y, C_TEXT, "Theme colour");

    /* The current accent, shown as itself rather than named. A swatch
     * answers "which one is it" at a glance; a name only helps if you
     * already know what the names look like. */
    Gfx_Fill(COLOR_X, COLOR_PREVIEW_Y, COLOR_GRID_W, COLOR_PREVIEW_H, C_ACCENT);
    Gfx_Frame(COLOR_X, COLOR_PREVIEW_Y, COLOR_GRID_W, COLOR_PREVIEW_H, C_HAIRLINE);

    for (int i = 0; i < SKIN_ACCENTS; i++) {
        int x, y;
        ColorCell(i, &x, &y);

        Gfx_Fill(x, y, COLOR_SW, COLOR_SW, Skin_AccentColor(i));

        /* Every swatch gets a hairline, or the pale ones dissolve into a
         * light theme's background and read as gaps in the grid. */
        Gfx_Frame(x, y, COLOR_SW, COLOR_SW, C_HAIRLINE);

        /* The one in use is ringed in the text colour, which is legible
         * against both grounds - an accent-coloured ring would vanish on
         * the very swatch it was marking. */
        if (i == Skin_Accent()) {
            Gfx_Frame(x - 2, y - 2, COLOR_SW + 4, COLOR_SW + 4, C_TEXT);
            Gfx_Frame(x - 1, y - 1, COLOR_SW + 2, COLOR_SW + 2, C_TEXT);
        }

        if (i == focus)
            Gfx_Frame(x - 3, y - 3, COLOR_SW + 6, COLOR_SW + 6, C_TEXT);
    }

    /* Says the way out. With the tab bar gone there is nothing else on
     * screen that does. */
    Gfx_Text(COLOR_X, COLOR_HINT_Y, C_TEXT_DIM,
             "Tap a colour, or A to choose, B to go back");
}

/* ------------------------------------------------------------------ */
/* content: meter picker                                              */
/* ------------------------------------------------------------------ */

/* Everything the top screen can be, on one page.
 *
 * Two shapes rather than one grid, because there are two kinds of thing
 * here. The meters are twelve variations on one idea and belong in a
 * block of equals - four across and three down, as they always were. The
 * cards are four different pages, plus off, and they sit in a row of
 * their own underneath with a rule between: a card shown as a
 * meter-shaped tile would read as a thirteenth meter.
 *
 * A row of names under a tab bar told you what the styles were called
 * and nothing about what they look like, which is the only thing anyone
 * chooses either of these by. */
#define METER_COLS  4
#define METER_ROWS  3
#define METER_CW    56
#define METER_CH    30
#define METER_GAP   3
#define METER_GRID_W (METER_COLS * METER_CW + (METER_COLS - 1) * METER_GAP)
#define METER_X      ((GFX_W - METER_GRID_W) / 2)

/* The cards, four across.
 *
 * Five, when there was an Off at the end of them. With Off gone the
 * fifth column stayed, and the four cards sat in it off to one side
 * with a cell of nothing beside them. */
#define CARD_COLS   TOP_CARDS
#define CARD_CW     44
#define CARD_CH     34
#define CARD_GAP    3
#define CARD_GRID_W (CARD_COLS * CARD_CW + (CARD_COLS - 1) * CARD_GAP)
#define CARD_X      ((GFX_W - CARD_GRID_W) / 2)

/* The cards above the meters, not below them.
 *
 * There are four cards and twelve meters, and the cards are the ones
 * that show what is playing - so they are what most people are looking
 * for on this page. Underneath three rows of meters they were the last
 * thing found rather than the first. */
#define METER_TITLE_Y   4
#define METER_NAME_Y   16
#define CARD_GRID_Y    30
#define CARD_RULE_Y    (CARD_GRID_Y + CARD_CH + 5)
#define METER_GRID_Y   (CARD_RULE_Y + 6)
#define METER_GRID_B   (METER_GRID_Y + METER_ROWS * METER_CH + \
                        (METER_ROWS - 1) * METER_GAP)
#define METER_HINT_Y   180

_Static_assert(METER_COLS * METER_ROWS == VIS_STYLES,
               "the grid and the style list have to hold the same number");
_Static_assert(METER_GRID_Y >= METER_NAME_Y + GFX_GLYPH_H + 3,
               "the meter grid, with room for its selection ring, runs into "
               "the name above it");
_Static_assert(CARD_GRID_Y + CARD_CH + 3 <= METER_HINT_Y,
               "the card row, with room for its selection ring, runs into "
               "the hint line below it");
_Static_assert(METER_X >= 3 && METER_X + METER_GRID_W + 3 <= GFX_W,
               "the selection ring falls off the side of the screen");
_Static_assert(CARD_X >= 3 && CARD_X + CARD_GRID_W + 3 <= GFX_W,
               "the card row's selection ring falls off the side");

/* One cell of either shape, by choice number. The two grids are laid out
 * here and nowhere else - drawing, hit testing and the d-pad all read
 * them from this, so none of the three can disagree about where a cell
 * is. */
static void TopCell(int i, int *x, int *y, int *w, int *h)
{
    if (TOP_IS_METER(i)) {
        *x = METER_X + (i % METER_COLS) * (METER_CW + METER_GAP);
        *y = METER_GRID_Y + (i / METER_COLS) * (METER_CH + METER_GAP);
        *w = METER_CW; *h = METER_CH;
        return;
    }

    const int col = i - VIS_STYLES;
    *x = CARD_X + col * (CARD_CW + CARD_GAP);
    *y = CARD_GRID_Y;
    *w = CARD_CW; *h = CARD_CH;
}

int Skin_HitTestMeter(int px, int py)
{
    for (int i = 0; i < TOP_CHOICES; i++) {
        int x, y, w, h;
        TopCell(i, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) return i;
    }
    return -1;
}

/* One step of the d-pad, in the shape the page actually has.
 *
 * The two grids are different widths, so a cursor that walked a single
 * modulus would leave the last meter and arrive somewhere that is not
 * under it. Moving between them keeps the horizontal position as closely
 * as the narrower row allows, which is what "down" looks like to the
 * person pressing it. */
int Skin_TopPickStep(int from, int dx, int dy)
{
    if (from < 0 || from >= TOP_CHOICES) return 0;

    if (dx) {
        if (TOP_IS_METER(from)) {
            const int row = from / METER_COLS, col = from % METER_COLS;
            return row * METER_COLS + (col + dx + METER_COLS) % METER_COLS;
        }
        const int col = from - VIS_STYLES;
        return VIS_STYLES + (col + dx + CARD_COLS) % CARD_COLS;
    }

    if (!dy) return from;

    if (TOP_IS_METER(from)) {
        const int row = from / METER_COLS, col = from % METER_COLS;
        const int next = row + dy;

        /* Off the bottom of the meters is the card row; off the top of it
         * wraps to the bottom of the meters, so the whole page is a loop
         * in both directions. */
        if (next >= METER_ROWS) return VIS_STYLES + (col * CARD_COLS) / METER_COLS;
        if (next < 0)           return VIS_STYLES + (col * CARD_COLS) / METER_COLS;
        return next * METER_COLS + col;
    }

    const int col = from - VIS_STYLES;
    const int mcol = (col * METER_COLS) / CARD_COLS;
    /* Up from the cards lands on the last row of meters; down wraps to
     * the first, so leaving the row is always possible either way. */
    return (dy > 0 ? 0 : (METER_ROWS - 1)) * METER_COLS + mcol;
}

/* A fixed set of levels for the miniatures.
 *
 * The same eight in every cell, so the cards differ by style and by
 * nothing else - drawn from live audio they would differ by whatever the
 * music was doing at the moment each was painted, and comparing them
 * would be comparing eight different moments. Chosen to have a tall
 * column, a short one and a couple in between, because a style is told
 * apart by what it does across a range and not at one height. */
static const uint8_t kMeterDemo[8] = { 5, 9, 3, 8, 6, 2, 7, 4 };

/* One cell: the style drawn small enough to fit, in its own colours.
 *
 * Not the real renderer. That one owns a whole screen, a backdrop
 * gradient, a reflection and a peak marker, and reaching into it for a
 * 56x46 thumbnail would mean either a second set of arguments through
 * every layer of it or a scratch buffer the size of the top screen. This
 * draws the shape each style makes, which is what the choice is between. */
static void DrawMeterCell(int cx, int cy, int style)
{
    const int inset = 4;
    const int gx = cx + inset, gy = cy + inset;
    const int gw = METER_CW - 2 * inset, gh = METER_CH - 2 * inset;
    const int cols = 8;
    const int cw = gw / cols;

    Gfx_Fill(cx, cy, METER_CW, METER_CH, C_BG);
    Gfx_Frame(cx, cy, METER_CW, METER_CH, C_HAIRLINE);

    for (int i = 0; i < cols; i++) {
        int x = gx + i * cw;
        int h = (kMeterDemo[i] * gh) / 10;
        if (h < 2) h = 2;

        int top = gy + gh - h;
        uint16_t ink = (kMeterDemo[i] >= 8) ? C_ACCENT : C_TEXT;

        switch (style) {
        case VIS_DOTS:
            Gfx_Fill(x, top, cw - 1, 2, ink);
            break;

        case VIS_COMB:
            for (int y = top; y < gy + gh; y += 2)
                Gfx_Fill(x, y, cw - 1, 1, ink);
            break;

        case VIS_BLOCKS:
            for (int y = top; y < gy + gh; y += 4)
                Gfx_Fill(x, y, cw - 1, 3, ink);
            break;

        case VIS_MIRROR: {
            int half = h / 2;
            if (half < 1) half = 1;
            Gfx_Fill(x, gy + gh / 2 - half, cw - 1, half * 2, ink);
            break;
        }

        case VIS_HANG:
            Gfx_Fill(x, gy, cw - 1, h, ink);
            break;

        case VIS_DROP:
            Gfx_Fill(x, gy + h - 2, cw - 1, 2, ink);
            break;

        case VIS_STEPS:
            for (int y = top; y < gy + gh; y += 4)
                Gfx_Fill(x, y, cw - 1, 2, ink);
            break;

        case VIS_TRAIL:
            Gfx_Fill(x, top, cw - 1, 2, ink);
            if (top + 5 < gy + gh) Gfx_Fill(x, top + 5, cw - 1, 2, ink);
            if (top + 9 < gy + gh) Gfx_Fill(x, top + 9, cw - 1, 1, ink);
            break;

        case VIS_EDGE:
            /* The lid and the two sides, which is the whole of it. */
            Gfx_Fill(x, top, cw - 1, 2, ink);
            Gfx_Fill(x, top, 1, h, ink);
            Gfx_Fill(x + cw - 2, top, 1, h, ink);
            break;

        case VIS_PILLARS:
            /* The gap is the point of this one, so the miniature keeps it
             * proportionally rather than at the real style's one pixel -
             * one pixel of six is invisible at this size. */
            if (cw >= 3) Gfx_Fill(x + 1, top, cw - 2, h, ink);
            break;

        case VIS_WAVE: {
            /* A trace rather than columns: the one style that is not a
             * spectrum at all, and a cell of bars would say the opposite
             * of what it is. */
            int mid = gy + gh / 2;
            int y = mid + ((i & 1) ? -h / 3 : h / 3);
            Gfx_Fill(x, y, cw, 2, ink);
            if (i > 0) {
                int py = mid + (((i - 1) & 1) ? -((kMeterDemo[i-1] * gh) / 10) / 3
                                              :  ((kMeterDemo[i-1] * gh) / 10) / 3);
                int lo = y < py ? y : py, hi = y > py ? y : py;
                Gfx_Fill(x, lo, 1, hi - lo + 2, ink);
            }
            break;
        }

        default:   /* VIS_BARS */
            Gfx_Fill(x, top, cw - 1, h, ink);
            break;
        }
    }
}

/* One card, in miniature.
 *
 * Each is drawn as the shape it puts on the screen rather than as a
 * label, for the same reason the meters are: what a card looks like is
 * the only thing anyone chooses between them by. The cover card shows
 * its frame empty here, because what goes in it depends on the track. */
static void DrawCardCell(int cx, int cy, int card)
{
    Gfx_Fill(cx, cy, CARD_CW, CARD_CH, C_BG);
    Gfx_Fill(cx, cy, CARD_CW, 2, C_ACCENT);      /* the band every card wears */

    const int ix = cx + 4, iw = CARD_CW - 8;

    switch (card) {
    case 0:   /* Now playing: title, artist, a clock and a progress bar */
        Gfx_Fill(ix, cy + 7,  iw - 6, 3, C_TEXT);
        Gfx_Fill(ix, cy + 13, iw - 16, 2, C_TEXT_DIM);
        Gfx_Fill(ix, cy + 22, iw, 2, C_SURFACE);
        Gfx_Fill(ix, cy + 22, iw / 3, 2, C_ACCENT);
        Gfx_Fill(ix, cy + 27, 8, 2, C_TEXT_DIM);
        break;

    case 1: {  /* Cover: the frame, at the size it always keeps */
        const int side = CARD_CH - 16;
        const int fx = cx + (CARD_CW - side) / 2;
        Gfx_Frame(fx, cy + 5, side, side, C_TEXT_DIM);
        Gfx_Frame(fx + 1, cy + 6, side - 2, side - 2, C_HAIRLINE);
        Gfx_Fill(ix, cy + CARD_CH - 7, iw - 8, 3, C_TEXT);
        break;
    }

    case 2: {  /* Cassette: the shell, its label, and the two spools */
        const int bw = CARD_CW - 10, bh = 21;
        const int bx = cx + 5, by = cy + 7;
        Gfx_Fill(bx, by, bw, bh, C_TEXT_DIM);
        Gfx_Fill(bx + 2, by + 2, bw - 4, 4, C_ACCENT);
        Gfx_Fill(bx + 2, by + 7, bw - 4, 4, C_TEXT);
        for (int i = 0; i < 2; i++)
            Gfx_Fill(bx + (i ? bw - 12 : 6), by + 13, 6, 5, C_BG);
        break;
    }

    default: {  /* Gramophone: the record, its label, and the arm */
        /* Half-widths of an eleven-pixel disc, row by row. A circle
         * drawn with a square root, in a function that runs before the
         * one this file keeps its square root in. */
        static const unsigned char kHalf[12] =
            { 11, 10, 10, 10, 10, 9, 9, 8, 7, 6, 4, 0 };
        static const unsigned char kLab[5] = { 4, 3, 3, 2, 0 };
        const int dcx = cx + CARD_CW / 2 - 3, dcy = cy + CARD_CH / 2 + 1;

        for (int dy = -11; dy <= 11; dy++) {
            const int w = kHalf[dy < 0 ? -dy : dy];
            if (w) Gfx_Fill(dcx - w, dcy + dy, w * 2 + 1, 1, C_TEXT_DIM);
        }
        for (int dy = -4; dy <= 4; dy++) {
            const int w = kLab[dy < 0 ? -dy : dy];
            if (w) Gfx_Fill(dcx - w, dcy + dy, w * 2 + 1, 1, C_ACCENT);
        }
        /* the arm, coming in from the top right corner */
        for (int i = 0; i < 12; i++)
            Gfx_Fill(cx + CARD_CW - 5 - i, cy + 5 + i, 2, 1, C_TEXT);
        break;
    }
    }

    Gfx_Frame(cx, cy, CARD_CW, CARD_CH, C_HAIRLINE);
}


void Skin_DrawMeters(int focus)
{
    s_contentReset = false;

    if (focus < 0 || focus >= TOP_CHOICES) focus = 0;

    Gfx_Fill(0, 0, GFX_W, GFX_H, C_BG);
    Gfx_Text(METER_X, METER_TITLE_Y, C_TEXT, "Top screen");
    Gfx_Text(METER_X, METER_NAME_Y, C_TEXT, Top_ChoiceName(focus));

    for (int i = 0; i < TOP_CHOICES; i++) {
        int x, y, w, h;
        TopCell(i, &x, &y, &w, &h);

        if (TOP_IS_METER(i)) DrawMeterCell(x, y, i);
        else                 DrawCardCell(x, y, i - VIS_STYLES);

        if (i == focus)
            Gfx_Frame(x - 2, y - 2, w + 4, h + 4, C_ACCENT);
    }

    /* A rule, so the meters and the cards read as two answers to the
     * question rather than one list that changes shape halfway down. */
    Gfx_Fill(METER_X, CARD_RULE_Y, METER_GRID_W, 1, C_HAIRLINE);

    Gfx_Text(METER_X, METER_HINT_Y, C_TEXT_DIM, "A choose    B back");
}

/* ------------------------------------------------------------------ */
/* content: sound picker                                              */
/* ------------------------------------------------------------------ */

/* Three across and five down, on the same full screen the theme and
 * colour pages use and for the same reason: it is a chooser you are in
 * and then out of.
 *
 * Named chips rather than a swatch grid, because unlike a theme there is
 * nothing to show - the difference between Jazz and Hip Hop is something
 * you hear. The one that is in force keeps the accent so the page
 * answers "which is on" without being asked. */
#define SOUND_COLS  3
#define SOUND_ROWS  5
#define SOUND_CW    76
#define SOUND_CH    22
#define SOUND_GAP   4
#define SOUND_GRID_W (SOUND_COLS * SOUND_CW + (SOUND_COLS - 1) * SOUND_GAP)
#define SOUND_X      ((GFX_W - SOUND_GRID_W) / 2)
#define SOUND_TITLE_Y   6
#define SOUND_GRID_Y    34
#define SOUND_HINT_Y    180

_Static_assert(SOUND_GRID_Y + SOUND_ROWS * SOUND_CH +
               (SOUND_ROWS - 1) * SOUND_GAP + 3 <= SOUND_HINT_Y,
               "the sound grid, with room for its selection ring, runs into "
               "the hint line below it");
_Static_assert(SOUND_X >= 3 && SOUND_X + SOUND_GRID_W + 3 <= GFX_W,
               "the selection ring falls off the side of the screen");

static void SoundCell(int i, int *x, int *y)
{
    *x = SOUND_X + (i % SOUND_COLS) * (SOUND_CW + SOUND_GAP);
    *y = SOUND_GRID_Y + (i / SOUND_COLS) * (SOUND_CH + SOUND_GAP);
}

int Skin_SoundSlots(void) { return SOUND_COLS * SOUND_ROWS; }
int Skin_SoundCols(void)  { return SOUND_COLS; }

int Skin_HitTestSound(int px, int py, int count)
{
    for (int i = 0; i < count && i < SOUND_COLS * SOUND_ROWS; i++) {
        int x, y;
        SoundCell(i, &x, &y);
        if (px >= x && px < x + SOUND_CW && py >= y && py < y + SOUND_CH)
            return i;
    }
    return -1;
}

void Skin_DrawSound(const char *const *names, int count, int focus, int chosen)
{
    s_contentReset = false;

    Gfx_Fill(0, 0, GFX_W, GFX_H, C_BG);
    Gfx_Text((GFX_W - Gfx_TextWidth("Sound")) / 2, SOUND_TITLE_Y,
             C_TEXT, "Sound");

    const char *sub = (chosen >= 0 && chosen < count) ? names[chosen] : "";
    Gfx_Text((GFX_W - Gfx_TextWidth(sub)) / 2, SOUND_TITLE_Y + 12,
             C_TEXT_DIM, sub);

    for (int i = 0; i < count && i < SOUND_COLS * SOUND_ROWS; i++) {
        int x, y;
        SoundCell(i, &x, &y);

        const bool on = (i == chosen);
        Gfx_Fill(x, y, SOUND_CW, SOUND_CH, on ? C_ACCENT : C_RAISED);

        const char *n = names[i];
        Gfx_TextClipped(x + (SOUND_CW - Gfx_TextWidth(n)) / 2,
                        y + (SOUND_CH - GFX_GLYPH_H) / 2, SOUND_CW - 6,
                        on ? C_TEXT_ON_SEL : C_TEXT, GFX_NO_SHADOW, n);

        if (i == focus)
            Gfx_Frame(x - 2, y - 2, SOUND_CW + 4, SOUND_CH + 4, C_ACCENT);
    }

    Gfx_Text(SOUND_X, SOUND_HINT_Y, C_TEXT_DIM, "A choose    B back");
}

/* ------------------------------------------------------------------ */
/* content: theme picker                                              */
/* ------------------------------------------------------------------ */

/* Six across and five down for thirty, on the same full screen the colour
 * page uses and for the same reason: it is a chooser you are in and then
 * out of, and thirty of anything does not fit under a tab bar. */
#define THEME_COLS  6
#define THEME_ROWS  5
#define THEME_CW    38
#define THEME_CH    24
#define THEME_GAP   3
#define THEME_GRID_W (THEME_COLS * THEME_CW + (THEME_COLS - 1) * THEME_GAP)
#define THEME_X      ((GFX_W - THEME_GRID_W) / 2)
#define THEME_TITLE_Y   6
#define THEME_NAME_Y    20
#define THEME_GRID_Y    40
#define THEME_HINT_Y    180

_Static_assert(THEME_COLS * THEME_ROWS == SKIN_THEMES,
               "the grid and the theme table have to hold the same number");
_Static_assert(THEME_GRID_Y + THEME_ROWS * THEME_CH +
               (THEME_ROWS - 1) * THEME_GAP + 3 <= THEME_HINT_Y,
               "the theme grid, with room for its selection ring, runs into "
               "the hint line below it");
_Static_assert(THEME_X >= 3 && THEME_X + THEME_GRID_W + 3 <= GFX_W,
               "the selection ring falls off the side of the screen");

static void ThemeCell(int i, int *x, int *y)
{
    *x = THEME_X + (i % THEME_COLS) * (THEME_CW + THEME_GAP);
    *y = THEME_GRID_Y + (i / THEME_COLS) * (THEME_CH + THEME_GAP);
}

/* ---- the font picker ------------------------------------------------
 *
 * Twelve cells, four across: a column is a family and a row is a cut -
 * plain, heavier, sloped. Each one is drawn in its own face, because a
 * list of names says nothing about what a typeface looks like and that
 * is the only thing anybody picks one by. The sample carries a cap, a
 * round lowercase, a descender and two digits, which between them is
 * most of what differs between these.
 */
/* FONT_GRID_ROWS, not FONT_ROWS: gfx.c already has a FONT_ROWS(gi) that
 * fetches a glyph's bitmap, and this file is included after it. */
#define FONT_COLS       GFX_FONT_COLS
#define FONT_GRID_ROWS  GFX_FONT_ROWS
#define FONT_CW     56
#define FONT_CH     34
#define FONT_GAP    4
#define FONT_GRID_W (FONT_COLS * FONT_CW + (FONT_COLS - 1) * FONT_GAP)
#define FONT_X      ((GFX_W - FONT_GRID_W) / 2)
#define FONT_TITLE_Y  6
#define FONT_NAME_Y  20
#define FONT_GRID_Y  48
#define FONT_HINT_Y  180

_Static_assert(FONT_COLS * FONT_GRID_ROWS == GFX_FONT_STYLES,
               "the grid and the face list have to hold the same number");

int Skin_ThemePickStep(int from, int dx, int dy)
{
    return GridStep(from, SKIN_THEMES, THEME_COLS, dx, dy);
}

int Skin_ColorPickStep(int from, int dx, int dy)
{
    return GridStep(from, SKIN_ACCENTS, COLOR_COLS, dx, dy);
}

static void FontCell(int i, int *x, int *y)
{
    *x = FONT_X + (i % FONT_COLS) * (FONT_CW + FONT_GAP);
    *y = FONT_GRID_Y + (i / FONT_COLS) * (FONT_CH + FONT_GAP);
}

int Skin_FontPickStep(int from, int dx, int dy)
{
    return GridStep(from, GFX_FONT_STYLES, FONT_COLS, dx, dy);
}

int Skin_HitTestFont(int px, int py)
{
    for (int i = 0; i < GFX_FONT_STYLES; i++) {
        int x, y;
        FontCell(i, &x, &y);
        if (px >= x && px < x + FONT_CW && py >= y && py < y + FONT_CH)
            return i;
    }
    return -1;
}

void Skin_DrawFonts(int focus)
{
    s_contentReset = false;

    if (focus < 0 || focus >= GFX_FONT_STYLES) focus = 0;

    const int was = Gfx_FontStyle();

    Gfx_Fill(0, 0, GFX_W, GFX_H, C_BG);
    Gfx_Text(FONT_X, FONT_TITLE_Y, C_TEXT, "Font");
    Gfx_Text(FONT_X, FONT_NAME_Y, C_TEXT, Gfx_FontStyleName(focus));

    for (int i = 0; i < GFX_FONT_STYLES; i++) {
        int x, y;
        FontCell(i, &x, &y);

        Gfx_Fill(x, y, FONT_CW, FONT_CH, i == focus ? C_SURFACE : C_BG);

        /* Drawn in the face it is offering, which means switching the
         * whole renderer twelve times to paint one page. It is a page
         * that appears when somebody asks for it and not otherwise, so
         * the remeasure that costs is not worth avoiding. */
        Gfx_SetFontStyle(i);
        const char *sample = "Ragu 42";
        const int tw = Gfx_TextWidth(sample);
        Gfx_Text(x + (FONT_CW - tw) / 2, y + 6,
                 i == focus ? C_TEXT : C_TEXT_DIM, sample);
        const char *second = "bcdehk";
        const int sw = Gfx_TextWidth(second);
        Gfx_Text(x + (FONT_CW - sw) / 2, y + 19,
                 i == focus ? C_TEXT : C_TEXT_DIM, second);

        Gfx_SetFontStyle(was);
        if (i == focus) Gfx_Frame(x - 2, y - 2, FONT_CW + 4, FONT_CH + 4, C_ACCENT);
        else            Gfx_Frame(x, y, FONT_CW, FONT_CH, C_HAIRLINE);
    }

    Gfx_Text(FONT_X, FONT_HINT_Y, C_TEXT_DIM, "A choose    B back");
}

int Skin_HitTestTheme(int px, int py)
{
    for (int i = 0; i < SKIN_THEMES; i++) {
        int x, y;
        ThemeCell(i, &x, &y);
        if (px >= x && px < x + THEME_CW && py >= y && py < y + THEME_CH)
            return i;
    }
    return -1;
}

void Skin_DrawThemes(int focus)
{
    s_contentReset = false;

    if (focus < 0 || focus >= SKIN_THEMES) focus = 0;

    Gfx_Fill(0, 0, GFX_W, GFX_H, C_BG);
    Gfx_Text(THEME_X, THEME_TITLE_Y, C_TEXT, "Theme");

    /* The cursor's theme named in full, because a card this size can show
     * what a theme looks like but not what it is called. */
    Gfx_Text(THEME_X, THEME_NAME_Y, C_TEXT, Skin_ThemeName(focus));

    for (int i = 0; i < SKIN_THEMES; i++) {
        int x, y;
        ThemeCell(i, &x, &y);

        /* Each card is the theme in miniature: its ground, two strokes in
         * its ink and a band of its accent. Those three are what tells
         * one theme from another at a glance - a single swatch of the
         * accent would make Blue and Navy the same card. */
        const SkinPalette *t = &kThemes[i];

        Gfx_Fill(x, y, THEME_CW, THEME_CH, t->bg);
        Gfx_Fill(x + 1, y + THEME_CH - 8, THEME_CW - 2, 6, t->surface);
        Gfx_Fill(x + 3, y + 5,  THEME_CW - 12, 2, t->text);
        Gfx_Fill(x + 3, y + 10, THEME_CW - 20, 2, t->textDim);
        Gfx_Fill(x + 3, y + THEME_CH - 7, THEME_CW - 6, 4, t->accent);

        Gfx_Frame(x, y, THEME_CW, THEME_CH, C_HAIRLINE);

        /* Ringed in this theme's own text colour, which is legible on
         * whatever ground the page happens to be drawn on. */
        if (i == s_theme) {
            Gfx_Frame(x - 2, y - 2, THEME_CW + 4, THEME_CH + 4, C_TEXT);
            Gfx_Frame(x - 1, y - 1, THEME_CW + 2, THEME_CH + 2, C_TEXT);
        }
        if (i == focus)
            Gfx_Frame(x - 3, y - 3, THEME_CW + 6, THEME_CH + 6, C_TEXT);
    }

    Gfx_Text(THEME_X, THEME_HINT_Y, C_TEXT_DIM,
             "Tap a theme, or A to choose, B to go back");
}

/* ------------------------------------------------------------------ */
/* content: register watch                                            */
/* ------------------------------------------------------------------ */

/* Every hardware fact in this program was measured by watching a register
 * while something changed - which PMIC bits gate the speaker, whether the
 * headphone pin is wired, whether the lamp bits reach a DSi. Each of
 * those cost a build. This is that loop made part of the player: open it,
 * change the thing, and read off what moved.
 *
 * Two columns of name and value, with anything that has moved since the
 * page opened drawn in the accent. */
#define REG_COLS      2
#define REG_ROW_H     13
#define REG_TOP       (CONTENT_Y + 22)
#define REG_COL_W     ((GFX_W - 16) / REG_COLS)

void Skin_DrawRegisters(const SkinRegRow *rows, int count, bool anyChanged)
{
    s_contentReset = false;

    Gfx_Fill(0, 0, GFX_W, GFX_H, C_BG);
    Gfx_Text(8, CONTENT_Y + 2, C_TEXT, "Registers");
    Gfx_Text(8 + Gfx_TextWidth("Registers") + 10, CONTENT_Y + 2,
             anyChanged ? C_ACCENT : C_TEXT_DIM,
             anyChanged ? "something moved" : "watching");

    for (int i = 0; i < count; i++) {
        int col = i % REG_COLS;
        int row = i / REG_COLS;
        int x = 8 + col * REG_COL_W;
        int y = REG_TOP + row * REG_ROW_H;
        if (y + GFX_GLYPH_H > GFX_H - 12) break;

        char line[32];
        snprintf(line, sizeof(line), "%s %02X", rows[i].name, rows[i].reg);
        Gfx_Text(x, y, rows[i].changed ? C_ACCENT : C_TEXT_DIM, line);

        char val[8];
        snprintf(val, sizeof(val), "%02X", rows[i].value & 0xFF);
        Gfx_Text(x + REG_COL_W - Gfx_TextWidth(val) - 12, y,
                 rows[i].changed ? C_ACCENT : C_TEXT, val);
    }

    Gfx_Text(8, GFX_H - 11, C_TEXT_DIM, "A rebases, B goes back");
}

/* ------------------------------------------------------------------ */
/* content: setup                                                     */
/* ------------------------------------------------------------------ */

/* A labelled row of choices: caption on the left, the options as a strip
 * of segments with the active one lit. Reads better than a checkbox when
 * neither option is really "off", and scales to more than two. */
static void DrawChoiceRow(SkinButton which, const char *caption,
                          const char *const *labels, int count, int active,
                          bool focused)
{
    const SkinRect *r = &kRects[which];

    Gfx_Text(r->x, r->y, focused ? C_TEXT : C_TEXT_DIM, caption);

    /* ROW_CTRL_DY/H rather than numbers here and different numbers in the
     * slider: the two drifted apart, and the slider's track ran to
     * r->y + 24 - past the bottom of its own rect and straight into the
     * hairline that separates it from the next row. */
    int y = r->y + ROW_CTRL_DY;
    int h = ROW_CTRL_H;

    for (int i = 0; i < count; i++) {
        int x0 = r->x + (r->w * i) / count;
        int x1 = r->x + (r->w * (i + 1)) / count;
        SkinRect seg = { x0, y, x1 - x0, h };

        DrawButtonFace(&seg, i == active);

        Gfx_TextShadow(seg.x + (seg.w - Gfx_TextWidth(labels[i])) / 2,
                       seg.y + (seg.h - GFX_GLYPH_H) / 2,
                       /* The active segment's face is filled with the
                        * accent, so its label is the ink meant for that
                        * ground - not C_TEXT, which is the ink for the
                        * page behind it. */
                       i == active ? C_TEXT_ON_SEL : C_TEXT_DIM,
                       C_SHADOW, labels[i]);
    }

    /* The focused row gets a bright outline so the d-pad has somewhere
     * visible to be. Hugged tight to the control: a 2px inset reached up
     * into the caption of the row below it. */
    if (focused)
        Gfx_Frame(r->x - 2, y - 1, r->w + 4, h + 2, C_ACCENT);
}

/* A labelled slider row: caption with its current value on the left,
 * track underneath. Used wherever a setting has more steps than can be
 * shown as tappable segments - ten segments across 228px would be 22px
 * each, and a hundred is obviously hopeless. */
static void DrawSliderRow(SkinButton which, const char *caption,
                          int value, int maxValue, bool focused)
{
    const SkinRect *r = &kRects[which];

    Gfx_Text(r->x, r->y, focused ? C_TEXT : C_TEXT_DIM, caption);

    int trackY = r->y + ROW_CTRL_DY;
    int permille = (maxValue > 0) ? (value * 1000) / maxValue : 0;

    DrawMeter(r->x, trackY, r->w, ROW_CTRL_H, permille,
              C_ACCENT, C_ACCENT);

    /* Both of these are derived from the track rather than carrying
     * their own numbers. They used to be trackY - 2 by 15 and trackY - 3
     * by 17, tuned by hand against a track that started three pixels
     * lower - so moving the track dragged the knob and the focus outline
     * up into the caption, and the row above's label came out with a
     * line struck through it. */
    int knobX = r->x + 1 + ((r->w - 2 - 6) * permille) / 1000;
    Gfx_Fill(knobX, trackY - 1, 7, ROW_CTRL_H + 2, C_TEXT_ON_SEL);

    if (focused)
        Gfx_Frame(r->x - 2, trackY - 1, r->w + 4, ROW_CTRL_H + 2, C_ACCENT);
}


/* Dragging the noise-reduction slider moves a value with a hundred steps,
 * so it changes on very nearly every frame of the drag. Redrawing one row
 * instead of the page is the difference between that being comfortable
 * and being the reason the decoder misses a frame. */
/* ------------------------------------------------------------------ */
/* scrollable settings rows                                           */
/* ------------------------------------------------------------------ */

/* The same rail the file list has, in the same place.
 *
 * It drew ten pixels wide at x=236 while the list's draws six at 246,
 * so the scrollbar changed size and moved sideways depending on which
 * page you were on. One of the two had to be wrong, and it was this
 * one: a rail is a rail. */
#define ROW_SCROLL_X SCROLL_X
#define ROW_SCROLL_W SCROLL_W

/* Drawn six pixels wide and touched across eighteen.
 *
 * A stylus does not reliably land inside a narrow target, and this is
 * the only way to reach the rows past the last visible slot without the
 * d-pad - so a miss here is not a cosmetic annoyance, it is a page the
 * user cannot get to. The touch area is not the rail and never was.
 *
 * The same eighteen the file list gives its own rail, and it overlaps
 * the last few pixels of a row there too: a tap that close to the edge
 * is far more likely to be aimed at the rail than at a control whose
 * middle is two hundred pixels away. */
#define ROW_SCROLL_HIT_X (SCROLL_X - 12)

/* Track geometry, shared by the drawing and the hit test so the thumb
 * cannot end up somewhere the touch does not agree with. */
static void RowTrack(int firstSlot, int *y, int *h)
{
    int y0 = kRects[SKIN_BTN_ROW0 + firstSlot].y - 2;
    *y = y0 + 2;
    *h = CONTENT_Y + CONTENT_H - *y - 2;
}

bool Skin_HitTestRowScroll(int px, int py, int count, int visible,
                           int firstSlot, int *outTop)
{
    if (px < ROW_SCROLL_HIT_X) return false;
    if (count <= visible) return false;

    int trackY, trackH;
    RowTrack(firstSlot, &trackY, &trackH);
    if (py < trackY || py >= trackY + trackH) return false;

    int thumbH = (trackH * visible) / count;
    if (thumbH < 24) thumbH = 24;      /* never smaller than a fingertip */

    /* The thumb centres on the stylus rather than its top edge following
     * it, which is what makes dragging feel like moving the thumb instead
     * of dragging a point some distance above it. */
    int span = count - visible;
    int travel = trackH - thumbH;
    int rel = py - trackY - thumbH / 2;
    if (rel < 0) rel = 0;
    if (rel > travel) rel = travel;

    *outTop = (travel > 0) ? (rel * span + travel / 2) / travel : 0;
    if (*outTop < 0) *outTop = 0;
    if (*outTop > span) *outTop = span;
    return true;
}

/* The file list's own scroll rail, and the same widened target the
 * settings pages use - the drawn bar is six pixels and a stylus does not
 * land on that. Returns the row that should become the top one. */
bool Skin_HitTestListScroll(int px, int py, int count, int *outTop)
{
    int visible = FileBrowser_VisibleRows();
    if (px < SCROLL_X - 12 || count <= visible) return false;
    if (py < LIST_Y || py >= LIST_Y + LIST_H) return false;

    int innerH = LIST_H - 2;
    int thumbH = (innerH * visible) / count;
    if (thumbH < 24) thumbH = 24;

    int span = count - visible;
    int travel = innerH - thumbH;
    int rel = py - LIST_Y - 1 - thumbH / 2;
    if (rel < 0) rel = 0;
    if (rel > travel) rel = travel;

    *outTop = (travel > 0) ? (rel * span + travel / 2) / travel : 0;
    if (*outTop < 0) *outTop = 0;
    if (*outTop > span) *outTop = span;
    return true;
}

int Skin_HitTestRowSlot(int px, int py)
{
    for (int i = 0; i < SKIN_ROW_SLOTS; i++) {
        const SkinRect *r = &kRects[SKIN_BTN_ROW0 + i];
        if (px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h)
            return i;
    }
    return -1;
}

/* Both settings pages outgrew the content area, so both scroll, and they
 * do it through the same code rather than each growing its own. A slot is
 * a place on screen; which row is in it depends on scrollTop, and hit
 * testing returns the slot for the caller to add its own offset to. That
 * is the whole mechanism - there is no per-page geometry left to drift.
 *
 * Repainted whole on any change rather than per row. The pages are small,
 * they are not on screen while music is being browsed, and the alternative
 * is another per-row signature cache to keep in step with a scroll
 * offset. */
void Skin_DrawRows(const SkinRow *rows, int count, int scrollTop, int focus,
                   int firstSlot, int slotCount)
{
    s_contentReset = false;

    /* Only the part this call owns: a page that reserved the top slot has
     * already drawn there, and wiping the whole content area would take
     * it back out again. */
    int y0 = kRects[SKIN_BTN_ROW0 + firstSlot].y - 2;
    Gfx_Fill(0, y0, GFX_W, CONTENT_Y + CONTENT_H - y0, C_BG);

    /* A page with fewer rows than slots has nothing to scroll, and a
     * caller computing count - SKIN_ROW_SLOTS then hands this a negative
     * top. rows[-1] is a wild caption pointer and a crash in the text
     * blitter, a long way from the arithmetic that caused it. */
    if (scrollTop < 0) scrollTop = 0;

    for (int i = 0; i < slotCount; i++) {
        int slot = firstSlot + i;
        int idx = scrollTop + i;
        if (idx < 0 || idx >= count) break;

        const SkinRow *row = &rows[idx];

        /* A row with no caption was never filled in. Skipping it leaves a
         * gap on screen, which is wrong but visible and harmless; drawing
         * it means dereferencing whatever `labels` happens to point at,
         * which is a crash. The callers zero their arrays so this is
         * reachable only through a genuine omission - and an omission
         * should cost a blank row, not the program. */
        if (!row->caption) continue;

        SkinButton which = (SkinButton)(SKIN_BTN_ROW0 + slot);

        if (row->kind == SKIN_ROW_CHOICE)

            DrawChoiceRow(which, row->caption, row->labels, row->count,
                          row->value, focus == idx);
        else
            DrawSliderRow(which, row->caption, row->value, row->count - 1,
                          focus == idx);

        /* A hairline in the gap below each row but the last.
         *
         * Spacing alone was not enough to separate them: a caption sits
         * directly under the control above it, so without a rule the two
         * read as one block and it is genuinely unclear which label goes
         * with which strip. The line says where one setting ends. */
        const SkinRect *rr = &kRects[which];
        if (i + 1 < slotCount && idx + 1 < count)
            Gfx_Fill(rr->x, rr->y + ROW_RULE_DY, rr->w, 1, C_HAIRLINE);
    }

    /* Scrollbar only when there is something off screen, so a page that
     * happens to fit does not carry a permanently full rail. */
    if (count > slotCount) {
        int trackY, trackH;
        RowTrack(firstSlot, &trackY, &trackH);
        Gfx_Fill(ROW_SCROLL_X, trackY, ROW_SCROLL_W, trackH, C_TROUGH);

        int thumbH = (trackH * slotCount) / count;
        if (thumbH < 24) thumbH = 24;
        int span = count - slotCount;
        int thumbY = trackY + ((trackH - thumbH) * scrollTop) / (span > 0 ? span : 1);
        Gfx_Fill(ROW_SCROLL_X + 1, thumbY, ROW_SCROLL_W - 2, thumbH, C_ACCENT);
    }
}

bool Skin_HitTestSlider(SkinButton which, int px, int py, int maxValue, int *outValue)
{
    if (which <= SKIN_BTN_NONE || which >= SKIN_BTN_COUNT) return false;

    const SkinRect *r = &kRects[which];

    /* Starts below the caption so a tap on the label does not jump the
     * slider to wherever the text happened to be. */
    if (px < r->x - 6 || px > r->x + r->w + 6) return false;
    if (py < r->y + ROW_CTRL_DY || py > r->y + ROW_CTRL_DY + ROW_CTRL_H)
        return false;

    int rel = px - r->x;
    if (rel < 0) rel = 0;
    if (rel > r->w) rel = r->w;

    *outValue = (rel * maxValue + r->w / 2) / r->w;
    return true;
}

/* ------------------------------------------------------------------ */
/* top screen overlay                                                 */
/* ------------------------------------------------------------------ */

/* Hashes the text, not the pointer. The track name lives in a fixed
 * buffer inside PlayerState, so its address never changes - keying off
 * that would have meant a new track sometimes kept the old title on
 * screen. */
static uint32_t TextHash(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s) return h;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static uint32_t s_overlaySig[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };

static void FormatTime(char *out, size_t len, uint32_t ms);   /* below */

static uint32_t s_cardSig[2] = { 0, 0 };

/* What static content each of the two top-screen buffers is holding, so
 * a repaint knows whether it may touch only the parts that moved. */
static uint32_t s_tapeHeld[2];

/* What radius of tape each buffer's two spools already have on them.
 *
 * Wound tape is concentric, so while the pack keeps its radius the only
 * thing in a window that changes is the hub turning - and the hub is a
 * quarter of the window's area. The pack's radius moves about nine
 * pixels over a whole side, so this is the common case by a long way,
 * and it is worth knowing when it holds. -1 is "nothing known". */
static int s_tapeSpoolR[2][2];

/* And the gramophone's, for the same reason.
 *
 * Almost nothing on that card moves. The deck is baked, the names are
 * printed once, and of the two things that do move one of them - the
 * arm - moves about forty times in a whole side. So each buffer
 * remembers what it is holding: the static content, which groove the
 * arm is standing in, and which of the label's twelve rotations it has.
 * A repaint that only turned the label puts back the label. */
/* What the cover card last drew into each buffer, as a signature of the
 * half of it that does not move: the picture, the wordmark, and the
 * facts about the file. When that matches, the picture is already on
 * screen and only the lines that change need painting again.
 *
 * Without it the card cleared all fifty thousand pixels and re-blitted
 * the cover on every repaint - and a sweeping title asks for a repaint
 * about fifteen times a second, so a still picture was being drawn
 * fifteen times a second to move some text beside it. The cassette and
 * the gramophone were given this treatment and the cover was not. */
static uint32_t s_coverHeld[2];

static uint32_t s_gramHeld[2];
static int      s_gramArmR[2];
static int      s_gramSpin[2];
static int      s_gramTitle[2];

void Skin_InvalidateTopCard(void)
{
    s_cardSig[0] = s_cardSig[1] = 0;
    /* And the cassette forgets what it left in each buffer: a repaint
     * that only touches the window is safe on a buffer holding the rest
     * of that picture, and after this nothing may be assumed. */
    s_tapeHeld[0] = s_tapeHeld[1] = 0;
    s_tapeSpoolR[0][0] = s_tapeSpoolR[0][1] = -1;
    s_tapeSpoolR[1][0] = s_tapeSpoolR[1][1] = -1;
    s_coverHeld[0] = s_coverHeld[1] = 0;
    s_gramHeld[0] = s_gramHeld[1] = 0;
    s_gramArmR[0] = s_gramArmR[1] = -1;
    s_gramSpin[0] = s_gramSpin[1] = -1;
    s_gramTitle[0] = s_gramTitle[1] = -1;
}

/* The current track's cover, as a thumbnail somebody else owns.
 *
 * Borrowed rather than copied: the decoder holds the pixels, and the card
 * only needs to know where they are and whether they have changed since
 * it was last painted. The generation is what puts the art into the
 * card's signature - the picture is decoded on the worker thread while
 * the track opens, which is after the card has already been drawn once,
 * so without it the frame would stay empty until the clock next ticked. */
static const uint16_t *s_topArt = NULL;
static int s_topArtW = 0, s_topArtH = 0;
static uint32_t s_topArtGen = 0;

void Skin_SetTopArt(const uint16_t *px, int w, int h)
{
    s_topArt  = px;
    s_topArtW = px ? w : 0;
    s_topArtH = px ? h : 0;
    s_topArtGen++;
}

const uint16_t *Skin_TopArt(int *w, int *h)
{
    if (w) *w = s_topArtW;
    if (h) *h = s_topArtH;
    return s_topArt;
}

uint32_t Skin_TopArtGeneration(void) { return s_topArtGen; }

/* The margin every card keeps at the sides, so their contents line up
 * with each other when you switch between them. */
#define CARD_M 12

/* The corner radius of the frame the cover sits in. About a tenth of the
 * square, which is the proportion that reads as deliberate rather than
 * as a rectangle with the corners knocked off. */
#define CARD_ART_RADIUS 12

/* The progress bar, which three of the four cards want and all three
 * want in the same place. */
static void CardProgress(int y, int h, uint32_t elapsedMs, uint32_t totalMs)
{
    const int barX = CARD_M, barW = 256 - 2 * CARD_M;

    Gfx_Fill(barX, y, barW, h, C_TROUGH);
    if (totalMs == 0) return;

    uint32_t filled = (uint32_t)(((uint64_t)elapsedMs * (uint32_t)(barW - 2)) / totalMs);
    if (filled > (uint32_t)(barW - 2)) filled = (uint32_t)(barW - 2);
    if (filled > 0) Gfx_Fill(barX + 1, y + 1, (int)filled, h - 2, C_ACCENT);
}

/* A line across the card, centred, sweeping sideways when it does not
 * fit.
 *
 * Every line on every card goes through this. They used to be centred
 * and left alone, so a title too long for the screen was simply cut off
 * and sat there - on a card whose entire job is to say which track is
 * playing, with nothing else moving on the screen to suggest anything
 * was missing.
 *
 * `halves` is the size in halves, the unit the big font and the marquee
 * share: 2 is plain, 4 double, 6 triple. A line that sweeps puts the
 * card into the moving state, which is what keeps it being repainted -
 * the cards otherwise redraw once a second and would show a marquee
 * frozen between steps. */
static void CardLine(int y, int halves, uint16_t ink, const char *text)
{
    if (!text || !text[0]) return;

    if (DrawTitleCentred(CARD_M, 256 - 2 * CARD_M, y, ink,
                         text, s_marqueeTick, halves))
        s_cardScrolling = true;
}

/* The stand-in cover, for a track that carries no picture.
 *
 * A record, drawn from the theme's own colours. It has to be something:
 * an empty square reads as a card that failed rather than as a track
 * without a cover, and it has to be the same size and in the same place
 * as a real one or the column beside it shifts depending on how the
 * track was tagged.
 *
 * A CD rather than a record, which is what it was first: at this size
 * the grooves read as a target, and half the point of the shape is that
 * it says "this is where a cover would be" at a glance. */
/* Integer square root, for laying the disc out by radius rather than by
 * its square. Built once per palette, so the loop below it is not worth
 * avoiding. */
static int Isqrt(int v)
{
    if (v <= 0) return 0;
    /* A power of four, and the largest the caller can need: this handles
     * anything under 4^10. The disc measures in eighths of a pixel, so
     * its squared distances are sixty-four times what a whole-pixel
     * radius would give, and 1 << 14 was not enough for them. */
    int r = 0, b = 1 << 18;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else            { r >>= 1; }
        b >>= 2;
    }
    return r;
}

#define NOCOVER_SIDE 116
static uint16_t s_noCover[NOCOVER_SIDE * NOCOVER_SIDE];
static uint16_t s_noCoverKey[6] = { 0, 0, 0, 0, 0, 0 };

/* Which of the disc's five bands a point falls in, given its squared
 * distance from the centre and the squared radii that separate them.
 * Outwards in: the square behind the disc, the lit face, the ring where
 * the recorded area starts, the clear plastic, and the hole. */
static int DiscBand(int d2, const int *r2)
{
    if (d2 > r2[0]) return 0;
    if (d2 > r2[1]) return 1;
    if (d2 > r2[2]) return 2;
    if (d2 > r2[3]) return 3;
    return 4;
}

/* Built once per palette, then blitted like a real cover.
 *
 * It is drawn a pixel at a time - it is a circle, and there is no
 * cheaper honest way - but that only happens when the colours change,
 * not every time the card repaints. Plotting it live would put fifteen
 * thousand Gfx_Pixel calls back on the path this card was just taken
 * off, and most tracks have no picture, so it would be the common case
 * rather than the rare one. */
static void BuildNoCover(void)
{
    const uint16_t key[6] = { C_SURFACE, C_ACCENT, C_HAIRLINE, C_BG,
                              C_TEXT, C_TEXT_DIM };
    if (!memcmp(key, s_noCoverKey, sizeof(key))) return;
    memcpy(s_noCoverKey, key, sizeof(key));

    const int side = NOCOVER_SIDE;
    const int c0 = side / 2, rad = (side * 2) / 5;

    /* A compact disc, by its radii. The proportions are the real ones -
     * a 120mm disc has a 15mm hole and its recorded area starts at 46mm
     * - because that ratio is most of what makes a disc read as a CD
     * rather than as a record: the hole is large and there is a wide
     * band of clear plastic around it.
     *
     * Everything here is in eighths of a pixel. Whole pixels put every
     * radius on an integer, which is how the rim came to look sawn: the
     * hole landed on six pixels flat and the recorded area's two edges,
     * two pixels apart, rounded to the same place at some angles and not
     * at others. */
    /* No separate rim ring. One drawn a pixel inside the edge lands
     * exactly on the boundary at the four compass points and fractionally
     * inside it everywhere else, so it showed as four little spurs and
     * nothing in between. The lit face is bright against the square
     * behind it, which is edge enough. */
    const int kR[4] = {
        rad * 8,                    /* the rim */
        rad * 8 * 36 / 100,         /* where the recorded area starts */
        rad * 8 * 31 / 100,         /* and its inner edge */
        rad * 8 * 13 / 100,         /* the hole */
    };
    int kR2[4];
    for (int i = 0; i < 4; i++) kR2[i] = kR[i] * kR[i];

    /* The sheen, without a single trigonometric function.
     *
     * A disc's shine is two opposed lobes that sweep round as it turns,
     * which is cos(2t) - and cos(2t) is (dx^2 - dy^2)/r^2, with
     * sin(2t) as 2.dx.dy/r^2. Both fall straight out of coordinates
     * already in hand. Rotating the pair by 45 degrees puts the
     * highlight off the axes, where it looks like light rather than
     * like alignment.
     *
     * This replaces a stripe: two flat tones meeting on a straight line
     * across the middle, which read as a pie chart. The difference is
     * not the highlight, it is that this one has no edges. */
    const int COS45 = 181;   /* 0.707 in 256ths */

    const uint16_t clear = Gfx_Lerp(C_SURFACE, C_TEXT_DIM, 1, 3);

    /* Sub-sample centres, as offsets from a pixel's centre in eighths:
     * a four by four grid inside the pixel. */
    static const int kOff[4] = { -3, -1, 1, 3 };

    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            /* The pixel's centre, not its corner. A square 116 across is
             * centred on 57.5, and measuring from 58 left the disc half a
             * pixel off in both directions - which is exactly the sort of
             * thing that reads as a wobble rather than as an offset. */
            const int ex = (x - c0) * 8 + 4, ey = (y - c0) * 8 + 4;
            const int d2 = ex * ex + ey * ey;
            const int r8 = Isqrt(d2);

            /* The lit face's colour here. Only wanted from just inside
             * the recorded area outwards, which is also what keeps the
             * divides below away from a radius of nothing. */
            uint16_t face = C_HAIRLINE;
            if (r8 > kR[2] - 16) {
                const int c2 = ((ex * ex - ey * ey) / 8 * 256) / (d2 / 8);
                const int s2 = ((2 * ex * ey)       / 8 * 256) / (d2 / 8);

                /* A third of the range, not all of it. At full amplitude
                 * the two lobes reach both ends of the ramp and the disc
                 * becomes four hard blobs; a third is a sheen. */
                int shade = 118 + ((c2 * COS45 + s2 * COS45) / 256) / 3;

                /* Brighter towards the rim, the way a disc catches light
                 * across its face rather than evenly. */
                shade += (r8 * 44) / kR[0];

                /* And the track spiral, as fine even rings. A triangle
                 * rather than a step: stepped, every ring was its own
                 * hard edge, and sixty hard edges are sixty chances to
                 * look sawn. Spaced by radius rather than by its square,
                 * too - squared, the rings were inches apart at the
                 * centre and packed at the rim, and where they crossed
                 * the sheen they made a moire that read as a
                 * fingerprint. */
                const int t = r8 & 15;
                shade += ((t < 8 ? t : 16 - t) * 10) / 8 - 5;

                if (shade < 0)   shade = 0;
                if (shade > 255) shade = 255;
                face = Gfx_Lerp(C_HAIRLINE, C_TEXT, shade, 255);
            }

            const uint16_t pal[5] = { C_SURFACE, face, C_TEXT_DIM,
                                      clear, C_BG };

            /* Away from every edge, one sample says it all. */
            int near = 0;
            for (int i = 0; i < 4; i++) {
                const int off = r8 - kR[i];
                if (off > -12 && off < 12) { near = 1; break; }
            }
            if (!near) {
                s_noCover[y * side + x] = pal[DiscBand(d2, kR2)];
                continue;
            }

            /* On one, count how much of the pixel each side gets.
             *
             * Sixteen sub-samples, each asked which band it lands in, and
             * the answer is the average of the sixteen colours. This is
             * coverage rather than an estimate of it, and it costs
             * nothing worth naming because only the thousand-odd pixels
             * that actually straddle an edge take this path. */
            int n[5] = { 0, 0, 0, 0, 0 };
            for (int sy = 0; sy < 4; sy++) {
                const int py = ey + kOff[sy];
                for (int sx = 0; sx < 4; sx++) {
                    const int px = ex + kOff[sx];
                    n[DiscBand(px * px + py * py, kR2)]++;
                }
            }

            int rr = 0, gg = 0, bb = 0;
            for (int i = 0; i < 5; i++) {
                if (!n[i]) continue;
                rr += (pal[i]        & 31) * n[i];
                gg += ((pal[i] >> 5)  & 31) * n[i];
                bb += ((pal[i] >> 10) & 31) * n[i];
            }
            s_noCover[y * side + x] = GFX_RGB(rr / 16, gg / 16, bb / 16);
        }
}

/* The stand-in cover, for a track that carries no picture.
 *
 * A disc, in the theme's own colours. It has to be something: an empty
 * square reads as a card that failed rather than as a track without a
 * cover, and it has to be the same size and in the same place as a real
 * one or the column beside it shifts depending on how the track happens
 * to be tagged.
 *
 * Concentric rings rather than a flat disc - at this size a plain circle
 * with a dot in it is a target, and the grooves are the whole of what
 * says record. */
static void DrawNoCover(int x, int y, int side)
{
    BuildNoCover();

    if (side == NOCOVER_SIDE) {
        Gfx_Blit(x, y, side, side, s_noCover, NOCOVER_SIDE);
        return;
    }

    /* A card asking for a different square gets the ground and no
     * record, rather than a stretched one. Nothing does today; this is
     * so that a card that starts to would look wrong rather than
     * silently smeared. */
    Gfx_Fill(x, y, side, side, C_SURFACE);
}

/* A rounded rectangle in outline, with the corners of whatever is inside
 * it taken off to match, and both edges smoothed.
 *
 * Three things happen in the corner pass, and all three are needed.
 *
 * The outline alone is not enough: the picture inside is square, so its
 * corners poke through the curve and read as a mistake rather than a
 * frame. What falls outside the arc is taken back to the page's ground.
 *
 * And a curve of this size drawn by a yes-or-no distance test is visibly
 * stepped - twelve pixels of radius is not many, and every step is a
 * whole pixel of error at the one place the eye is drawn to. So the
 * distance is measured in sixteenths and the two edges are blended
 * rather than switched: a pixel the arc half covers comes out half way
 * between the frame and what is behind it. It costs a square root and
 * two blends per pixel over four boxes of a hundred and forty-four, on
 * a card that repaints once a second.
 *
 * Reading what is behind each pixel is what makes the blend right in
 * both places at once - over the picture at the top of the arc and over
 * the page below it - without this needing to know which is which. */
static void DrawRoundedFrame(int x, int y, int w, int h, int r, int t,
                             uint16_t c, uint16_t outside)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 2) { Gfx_Frame(x, y, w, h, c); return; }

    /* The straight runs, which stop where the corners begin. */
    Gfx_Fill(x + r,     y,         w - 2 * r, t, c);
    Gfx_Fill(x + r,     y + h - t, w - 2 * r, t, c);
    Gfx_Fill(x,         y + r,     t, h - 2 * r, c);
    Gfx_Fill(x + w - t, y + r,     t, h - 2 * r, c);

    /* Sixteen samples a pixel, and not a square root among them.
     *
     * The first version of this measured the distance from the pixel's
     * centre to the ideal circle and blended by that. It is an
     * approximation of coverage and it shows at the shallow parts of the
     * arc, where a pixel's centre can be well outside the curve while
     * most of the pixel is inside it.
     *
     * Counting is exact and cheaper. Each pixel is divided into a four by
     * four grid, each sub-sample is asked which side of the two radii it
     * falls on, and the answer is how many landed where. Comparing
     * squared distances against squared radii keeps all of it in
     * integers - the square root the old version needed per pixel is
     * gone entirely.
     *
     * Positions are in eighths of a pixel so the sub-sample centres -
     * one, three, five and seven eighths - are whole numbers. */
    const int rOut2 = (r * 8) * (r * 8);
    const int rIn2  = ((r - t) * 8) * ((r - t) * 8);

    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int ring = 0, erase = 0;

            for (int sy = 0; sy < 4; sy++) {
                const int ey = r * 8 - (dy * 8 + 2 * sy + 1);
                for (int sx = 0; sx < 4; sx++) {
                    const int ex = r * 8 - (dx * 8 + 2 * sx + 1);
                    const int d2 = ex * ex + ey * ey;

                    if (d2 > rOut2)      erase++;
                    else if (d2 >= rIn2) ring++;
                }
            }

            if (!ring && !erase) continue;         /* well inside: leave it */

            static const int kX[4] = { 0, 1, 0, 1 }, kY[4] = { 0, 0, 1, 1 };
            for (int q = 0; q < 4; q++) {
                const int px = kX[q] ? x + w - 1 - dx : x + dx;
                const int py = kY[q] ? y + h - 1 - dy : y + dy;

                /* One weighted average, not two blends in a row.
                 *
                 * Applied in sequence, the second blend acts on the
                 * result of the first, so a pixel that is half outside
                 * and half edge came out as a quarter of what was
                 * underneath it - the picture leaking into a pixel that
                 * should have been edge and ground only. Averaging the
                 * three by their sub-sample counts gives each exactly
                 * the share of the pixel it covers. */
                const uint16_t v = Gfx_GetPixel(px, py);
                const int keep = 16 - erase - ring;

                const int rr = (v & 31) * keep
                             + (outside & 31) * erase + (c & 31) * ring;
                const int gg = ((v >> 5) & 31) * keep
                             + ((outside >> 5) & 31) * erase
                             + ((c >> 5) & 31) * ring;
                const int bb = ((v >> 10) & 31) * keep
                             + ((outside >> 10) & 31) * erase
                             + ((c >> 10) & 31) * ring;

                Gfx_Pixel(px, py, GFX_RGB(rr / 16, gg / 16, bb / 16));
            }
        }
    }
}

/* The cassette's own colours, for the parts drawn live.
 *
 * Fixed rather than taken from the palette, for the same reason the
 * baked shell is: tape is brown and a hub is grey, and tinting those to
 * whatever the theme happens to be turns an object back into a widget. */
/* Wound tape, and the window it is wound in.
 *
 * Light on dark, which is the way this is drawn rather than the way it
 * is lit: the window behind the spools is the dark of the inside of the
 * shell, and what a spool has on it is the thing that shows. So a full
 * spool is a pale disc and an empty one is a hole, and the two are as
 * far apart as the picture has room for - which is the whole of what
 * this card has to say. */
#define TAPE_DARK  GFX_RGB8(0x8C, 0x72, 0x52)
#define TAPE_EDGE  GFX_RGB8(0xAB, 0x8D, 0x68)
#define WINDOW_BG  TAPE_C_BAND

/* The shell's palette, with the label's colour filled in.
 *
 * The baked picture does not store the label as a colour. Its pixels
 * are steps along three ramps that all meet at the label - shell to
 * label round its outline, label to paper round the strip, label to
 * grey round the window - and the far ends of those are fixed while the
 * label end is whatever the selected theme is. So the picture is one
 * picture and the printing on it is the theme's, which is the only way
 * a baked object can take part in a colour scheme at all.
 *
 * Rebuilt when the accent changes and not otherwise: twenty-eight
 * lerps, once a theme. */
static uint16_t s_tapePal[sizeof kCassettePal / sizeof kCassettePal[0]];
static uint16_t s_tapePalFor = 0;
static bool     s_tapePalSet = false;

/* Which of the label's two inks to print on it.
 *
 * A theme's accent can be anything from near-black to a pale yellow, and
 * the printing has to be legible on all of them, so the choice is made
 * from the accent's own brightness against the midpoint of the two inks
 * rather than from a number picked by eye. Whichever is chosen, it is at
 * least a third of the range away from the ground it sits on. */
static int TapeLuma(uint16_t c)
{
    return ((c & 31) * 2 + ((c >> 5) & 31) * 5 + ((c >> 10) & 31)) / 8;
}

/* Which way the header band across the top of the label prints.
 *
 * Dark on a label bright enough to carry it, light on one that is not.
 * A quarter of the themes have an accent darker than the window itself,
 * and on those a darker band is no band at all - so the band flips, and
 * the brand printed on it flips with it. Both from the same test, which
 * is the only thing that keeps them agreeing. */
static bool TapeBandIsDark(uint16_t acc) { return TapeLuma(acc) > 14; }

static uint16_t TapeHeader(uint16_t acc)
{
    return TapeBandIsDark(acc) ? Gfx_Lerp(acc, TAPE_C_SHELL, 2, 3)
                               : Gfx_Lerp(acc, TAPE_C_PAPER, 1, 2);
}

static uint16_t TapeInk(uint16_t acc)
{
    return TapeBandIsDark(acc) ? TAPE_C_PAPER : TAPE_C_INK;
}

static void TapePalette(void)
{
    const uint16_t acc = C_ACCENT;
    if (s_tapePalSet && s_tapePalFor == acc) return;

    for (unsigned i = 0; i < sizeof s_tapePal / sizeof s_tapePal[0]; i++)
        s_tapePal[i] = kCassettePal[i];

    for (int i = 0; i < TAPE_N_SHELL; i++)
        s_tapePal[TAPE_RAMP_SHELL + i] =
            Gfx_Lerp(TAPE_C_SHELL, acc, i + 1, TAPE_N_SHELL);
    for (int i = 0; i < TAPE_N_PAPER; i++)
        s_tapePal[TAPE_RAMP_PAPER + i] =
            Gfx_Lerp(acc, TAPE_C_PAPER, i + 1, TAPE_N_PAPER);
    for (int i = 0; i < TAPE_N_BAND; i++)
        s_tapePal[TAPE_RAMP_BAND + i] =
            Gfx_Lerp(acc, TAPE_C_BAND, i + 1, TAPE_N_BAND);
    {
        const uint16_t far = TapeHeader(acc);
        for (int i = 0; i < TAPE_N_HDR; i++)
            s_tapePal[TAPE_RAMP_HDR + i] =
                Gfx_Lerp(acc, far, i + 1, TAPE_N_HDR);
    }

    s_tapePalFor = acc;
    s_tapePalSet = true;
}

/* One spool: the tape wound on it, and the hub it is wound around.
 *
 * `turn` is where the hub has rotated to. Only the hub can show it -
 * wound tape is concentric and looks identical however far round it has
 * gone, which is exactly what a real one does. */
/* One spool, composed into the caller's row buffer rather than plotted.
 *
 * Every pixel of this used to go through Gfx_Pixel, which is a bounds
 * test and a dirty-row mark apiece - four thousand of them a repaint,
 * and the repaint happens every time the clock moves. Written into a
 * buffer the caller then blits, the whole window costs one transfer a
 * row instead.
 *
 * `buf` is the window's rows; (bx, by) is where this spool's centre
 * sits inside it. */
/* Isqrt handles values under 4^10; the largest this asks it for is the
 * corner of the window measured in sixteenths. Stated here because the
 * two live far apart and the failure would be silent. */
typedef char TapeIsqrtFits[
    ((CASSETTE_WINR + 1) * (CASSETTE_WINR + 1) * 2 * 256 < (1 << 20)) ? 1 : -1];

static void DrawSpool(uint16_t *buf, int stride, int bw, int bh,
                      int bx, int by, int rTape, int turn)
{
    const int rOut16 = rTape * 16;
    const int rIn = HUB_D / 2;
    const int i2 = rIn * rIn;

    for (int dy = -rTape - 1; dy <= rTape + 1; dy++) {
        const int py = by + dy;
        if (py < 0 || py >= bh) continue;
        uint16_t *row = buf + py * stride;

        for (int dx = -rTape - 1; dx <= rTape + 1; dx++) {
            const int px = bx + dx;
            if (px < 0 || px >= bw) continue;

            const int d2 = dx * dx + dy * dy;
            if (d2 < i2) continue;                   /* the hub covers it */

            const int d16 = Isqrt(d2 * 256);
            const int over = d16 - rOut16;
            if (over > 8) continue;

            /* Flat, with one lighter ring at the rim. Concentric
             * banding is what wound tape really looks like and it was
             * the wrong answer here: this picture is drawn, not
             * photographed, and a drawn object holds its colour. */
            const uint16_t c = (rOut16 - d16 < 32 || d16 - rIn * 16 < 24)
                             ? TAPE_EDGE : TAPE_DARK;

            /* The rim measured in sixteenths and blended, so the edge of
             * the pack is a curve rather than a staircase. */
            row[px] = (over <= -8) ? c : Gfx_Lerp(row[px], c, 8 - over, 16);
        }
    }

    const unsigned char *f =
        &kHub[((unsigned)turn % HUB_FRAMES) * HUB_D * HUB_D];
    for (int y = 0; y < HUB_D; y++) {
        const int py = by - HUB_D / 2 + y;
        if (py < 0 || py >= bh) continue;
        uint16_t *row = buf + py * stride;
        for (int x = 0; x < HUB_D; x++) {
            const int px = bx - HUB_D / 2 + x;
            if (px < 0 || px >= bw) continue;
            const unsigned char v = f[y * HUB_D + x];
            if (!v) continue;
            /* The rim is coverage rather than colour, so it meets tape
             * and empty window equally well - see make_cassette.py. */
            row[px] = (v <= HUB_EDGE)
                    ? Gfx_Lerp(row[px], kHubPal[v], v, HUB_EDGE + 1)
                    : kHubPal[v];
        }
    }
}

/* ---- the gramophone's pieces ---------------------------------------
 *
 * The record is baked and fixed; the label is baked but takes the
 * theme's colour, the same trick the cassette's label uses - its pixels
 * are not colours but a rim coverage, the accent, and an ink. The arm
 * is drawn, because it moves a little every second and a sprite sheet
 * of every angle it can take would be a hundred kilobytes.
 */
static uint16_t s_labPal[LABEL_PAL_N];
static uint16_t s_backPal[BACK_PAL_N];
static uint16_t s_labPalFor = 0;
static bool     s_labPalSet = false;

static void GramPalette(void)
{
    const uint16_t acc = C_ACCENT;
    if (s_labPalSet && s_labPalFor == acc) return;

    const uint16_t ink = TapeInk(acc);      /* the same choice the tape makes */

    s_labPal[0] = 0;
    for (int i = 0; i < LABEL_EDGE; i++)
        s_labPal[1 + i] = acc;              /* coverage, blended at use */
    s_labPal[LABEL_FACE] = acc;
    for (int i = 0; i < LABEL_INK_N; i++)
        s_labPal[LABEL_INK + i] =
            Gfx_Lerp(acc, ink, i + 1, LABEL_INK_N);

    /* The spindle, baked into every frame of the label because it stands
     * still while the label turns. Fixed tones: it is a steel pin. */
    s_labPal[LABEL_PIN + 0] = GFX_RGB8(0x1A, 0x1A, 0x1E);
    s_labPal[LABEL_PIN + 1] = GFX_RGB8(0xB4, 0xB6, 0xC0);
    s_labPal[LABEL_PIN + 2] = GFX_RGB8(0x70, 0x72, 0x7C);

    /* And the plinth takes a fifth of the theme's colour.
     *
     * The grain only - the metal on it is left alone, because tinting
     * the controls too turns the whole picture one colour and the wood
     * stops being wood.
     *
     * A seventh. A fifth was enough to tell a blue theme from a green
     * one across the room and also enough to make the green one read as
     * painted rather than stained, which is a different object.
     *
     * Wood first in the palette, metal after: the generator lays it out
     * that way so this loop can be a single bound rather than a test
     * per entry. */
    for (int i = 0; i < BACK_PAL_N; i++)
        s_backPal[i] = (i < BACK_WOOD_N)
                     ? Gfx_Lerp(kBackPal[i], acc, 1, 7)
                     : kBackPal[i];

    s_labPalFor = acc;
    s_labPalSet = true;
}

/* A disc with a soft edge, for the small round parts of the arm.
 *
 * Measured in sixteenths and blended, the same way the tape pack's rim
 * is, because a circle this size tested with whole pixels is four
 * visible steps and reads as a cog. */
/* Not in fast RAM, and deliberately.
 *
 * These two work a pixel at a time, which is the usual reason to put
 * something there - but they draw the arm, and the arm is redrawn about
 * forty times in a whole side of a record. The label's blit runs
 * fourteen times a second. With the audio routines holding first claim
 * on this memory there was room for one or the other, and the one that
 * runs three thousand times as often won. */
static void DrawDisc(int cx, int cy, int r16, uint16_t c)
{
    const int r = (r16 + 15) >> 4;
    for (int dy = -r - 1; dy <= r + 1; dy++)
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            const int d16 = Isqrt((dx * dx + dy * dy) * 256);
            const int over = d16 - r16;
            if (over > 8) continue;
            const int x = cx + dx, y = cy + dy;
            Gfx_Pixel(x, y, (over <= -8)
                          ? c
                          : Gfx_Lerp(Gfx_GetPixel(x, y), c, 8 - over, 16));
        }
}

/* The card's ground, which is a gradient rather than a flat fill:
 * lightest across the middle where the deck sits, falling away above and
 * below, so the record is lit rather than pasted on. */
/* The plinth, from the baked picture.
 *
 * Everything moulded into it - the well the platter turns in, the
 * recesses the controls sit in, the hinge blocks - is in here rather
 * than painted over the top, which is what lets any piece of the card be
 * put back without also having to know what furniture was standing
 * there. The arm's counterweight swings out over the right-hand edge of
 * the plinth; painted over the top, that edge was rubbed out the first
 * time the arm moved. */
static uint16_t GramBack(int x, int y)
{
    if (x < 0 || x >= BACK_W || y < 0 || y >= BACK_H) return C_BG;
    return s_backPal[kBack[y * BACK_W + x]];
}

/* Puts back a box of the card: ground, and the deck where the deck is.
 *
 * One routine for the whole picture and for the small pieces of it that
 * a repaint disturbs - the arm's old position, and whatever the label
 * sits under. Composed a row at a time and blitted, never plotted.
 *
 * The deck is centred on the sprite, not on the record: the picture is
 * the platter, which is wider than the disc printed on it. Offsetting by
 * the record's radius put the whole deck seven pixels down and right of
 * the label, and the label's hole went with it - which showed as a
 * crescent of bare ground on one side of the label and another at the
 * platter's opposite edge. */
static void BlitDeckBox(int cx, int cy, int bx0, int by0, int bx1, int by1)
{
    static uint16_t row[256];
    const int x0 = cx - RECORD_D / 2, y0 = cy - RECORD_D / 2;

    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > 256) bx1 = 256;
    if (by1 > 192) by1 = 192;
    if (bx1 <= bx0 || by1 <= by0) return;

    for (int y = by0; y < by1; y++) {
        const int sy = y - y0;
        const unsigned char *src = (sy >= 0 && sy < RECORD_D)
                                 ? &kRecord[sy * RECORD_D] : NULL;

        for (int x = bx0; x < bx1; x++) {
            const int sx = x - x0;
            const uint16_t g = GramBack(x, y);
            const unsigned char v = (src && sx >= 0 && sx < RECORD_D)
                                  ? src[sx] : 0;
            row[x - bx0] = !v ? g
                         : (v <= RECORD_EDGE)
                           ? Gfx_Lerp(g, RECORD_C_RIM, v, RECORD_EDGE + 1)
                           : kRecordPal[v];
        }
        Gfx_Blit(bx0, y, bx1 - bx0, 1, row, bx1 - bx0);
    }
}

/* The furniture around the deck: the controls a turntable carries on its
 * plinth, and the plinth's own edge.
 *
 * All of it static, so it is painted once with the ground and never
 * touched again - which is why it can be as detailed as it likes. The
 * only live part is the lamp, and whether the deck is running is in the
 * card's signature, so it repaints when that changes.
 *
 * Kept clear of the boxes the arm and the title put back. Nothing
 * enforces that by construction, but the test that plays a side through
 * and compares every frame against a full repaint would fail the moment
 * one of those boxes reached over here. */
/* What the deck holds at one pixel, from the picture rather than from
 * the screen. */
static uint16_t DeckPixel(int cx, int cy, int x, int y)
{
    const int sx = x - (cx - RECORD_D / 2), sy = y - (cy - RECORD_D / 2);
    if (sx < 0 || sx >= RECORD_D || sy < 0 || sy >= RECORD_D)
        return GramBack(x, y);
    const unsigned char v = kRecord[sy * RECORD_D + sx];
    if (!v) return GramBack(x, y);
    if (v <= RECORD_EDGE)
        return Gfx_Lerp(GramBack(x, y), RECORD_C_RIM, v, RECORD_EDGE + 1);
    return kRecordPal[v];
}

/* The label, at whichever of its twelve rotations.
 *
 * In fast RAM: with the spindle baked into it and the title only
 * redrawn when it moves, this blit is the whole of what the gramophone
 * costs on a quiet repaint, and it is reached from a function far too
 * large to put there itself. */
#ifdef ARM9
__attribute__((section(".itcm"), long_call, target("arm"), noinline))
#endif
static void DrawLabel(int cx, int cy, int turn)
{
    static uint16_t row[LABEL_D];
    const unsigned char *f =
        &kLabel[((unsigned)turn % LABEL_FRAMES) * LABEL_D * LABEL_D];
    const int x0 = cx - LABEL_D / 2, y0 = cy - LABEL_D / 2;

    for (int y = 0; y < LABEL_D; y++) {
        const unsigned char *src = &f[y * LABEL_D];
        int x = 0;
        while (x < LABEL_D) {
            while (x < LABEL_D && !src[x]) x++;
            int run = 0;
            while (x + run < LABEL_D && src[x + run]) {
                const unsigned char v = src[x + run];
                /* The rim blends with the deck, taken from the baked
                 * picture rather than read back off the screen.
                 *
                 * Read off the screen it blends with whatever is there,
                 * which after the first rotation is its own previous
                 * rim - so the edge compounds a shade every turn, and
                 * the only cure is putting a square of deck back first.
                 * Taken from the picture, there is nothing to put back:
                 * what lies under a label is the same ring of vinyl at
                 * every rotation, because vinyl is concentric. */
                row[run] = (v <= LABEL_EDGE)
                         ? Gfx_Lerp(DeckPixel(cx, cy, x0 + x + run, y0 + y),
                                    s_labPal[LABEL_FACE], v, LABEL_EDGE + 1)
                         : s_labPal[v];
                run++;
            }
            if (run) Gfx_Blit(x0 + x, y0 + y, run, 1, row, run);
            x += run;
        }
    }
}

/* A line with coverage, which is all the arm is.
 *
 * Stepped along its longer axis and blended into the two pixels either
 * side of where it actually falls, so a shallow angle does not come out
 * as a staircase - the one thing that would give away that this is
 * drawn rather than photographed. */
static void DrawThickLine(int x0, int y0, int x1, int y1,
                          int halfW16, uint16_t c)
{
    const int dx = x1 - x0, dy = y1 - y0;
    const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    const int steps = (adx > ady ? adx : ady);
    if (steps <= 0) return;

    for (int i = 0; i <= steps; i++) {
        const int fx16 = x0 * 16 + (dx * 16 * i) / steps;
        const int fy16 = y0 * 16 + (dy * 16 * i) / steps;

        if (adx > ady) {
            const int x = fx16 >> 4;
            for (int o = -((halfW16 + 31) >> 4); o <= ((halfW16 + 31) >> 4); o++) {
                const int y = (fy16 >> 4) + o;
                int d = (fy16 - (y * 16 + 8));
                if (d < 0) d = -d;
                const int cov = halfW16 + 8 - d;
                if (cov <= 0) continue;
                Gfx_Pixel(x, y, Gfx_Lerp(Gfx_GetPixel(x, y), c,
                                         cov > 16 ? 16 : cov, 16));
            }
        } else {
            const int y = fy16 >> 4;
            for (int o = -((halfW16 + 31) >> 4); o <= ((halfW16 + 31) >> 4); o++) {
                const int x = (fx16 >> 4) + o;
                int d = (fx16 - (x * 16 + 8));
                if (d < 0) d = -d;
                const int cov = halfW16 + 8 - d;
                if (cov <= 0) continue;
                Gfx_Pixel(x, y, Gfx_Lerp(Gfx_GetPixel(x, y), c,
                                         cov > 16 ? 16 : cov, 16));
            }
        }
    }
}

/* The arm: a pivot, a tube, and a headshell with the stylus under it.
 *
 * The stylus sits where the two circles meet - the one the arm's length
 * draws around the pivot, and the one the groove draws around the
 * spindle. That is how a real arm tracks, and it is why the headshell
 * ends up at a slight angle to the arm rather than square to it. */
/* Where the stylus sits for a given groove.
 *
 * The meeting of two circles: the one the arm's length draws around its
 * pivot, and the one the groove draws around the spindle. That is how a
 * real arm tracks, and it is why the headshell ends up at a slight angle
 * to the arm rather than square to it.
 *
 * Wanted twice - once to draw the arm, once to work out the box it is
 * in - so it is solved in one place. */
static void ArmStylus(int px, int py, int cx, int cy, int rs, int armLen,
                      int *outX, int *outY)
{
    const int vx = px - cx, vy = py - cy;
    const int d2 = vx * vx + vy * vy;
    const int d  = Isqrt(d2);
    if (d <= 0) { *outX = px; *outY = py; return; }

    const int a = (d2 + rs * rs - armLen * armLen) / (2 * d);
    int h2 = rs * rs - a * a;
    if (h2 < 0) h2 = 0;
    const int h = Isqrt(h2);

    /* The mid point along the line, then off it by h - the side that
     * puts the stylus below the pivot, which is the way a deck is laid
     * out for a right-handed arm. */
    const int mx = cx + (vx * a) / d, my = cy + (vy * a) / d;
    *outX = mx - (vy * h) / d;
    *outY = my + (vx * h) / d;
}

/* The box the arm and everything on it occupies, for putting back. */
static void ArmBox(int px, int py, int sx, int sy, int *x0, int *y0,
                   int *x1, int *y1)
{
    const int bx = px + (px - sx) / 4, by = py + (py - sy) / 4;
    const int lo = 10, hi = 11;        /* the tube's width, and the shell */
    *x0 = (sx < bx ? sx : bx) - lo;
    *y0 = (sy < by ? sy : by) - lo;
    *x1 = (sx > bx ? sx : bx) + hi;
    *y1 = (sy > by ? sy : by) + hi;

    /* and the rest it stands beside, which is drawn with it */
    if (px - 5 < *x0)      *x0 = px - 5;
    if (px + 9 > *x1)      *x1 = px + 9;
    if (py + 38 < *y0)     *y0 = py + 38;
    if (py + 56 > *y1)     *y1 = py + 56;
}

static void DrawArm(int px, int py, int cx, int cy, int rs, int armLen,
                    bool isPlaying)
{
    const int ARM_TUBE  = GFX_RGB8(0xC8, 0xC9, 0xD2);
    const int ARM_DARK  = GFX_RGB8(0x5A, 0x5C, 0x66);
    const int ARM_SHELL = GFX_RGB8(0x27, 0x27, 0x2C);

    int sx, sy;
    ArmStylus(px, py, cx, cy, rs, armLen, &sx, &sy);

    /* The rest the arm sits in when it is not playing: a post with a
     * cradle on top, which is what it is - a single dot read as a
     * screw somebody had left in the plinth.
     *
     * Drawn here rather than with the other static furniture because the
     * arm's own box reaches over it; put back once and never again, it
     * was rubbed out the first time the arm stepped a groove. */
    {
        const int rx = px + 2, ry = py + 46;
        Gfx_Fill(rx - 2, ry - 1, 5, 9, GFX_RGB8(0x2A, 0x2B, 0x31));
        Gfx_Fill(rx - 1, ry, 3, 8, GFX_RGB8(0x5E, 0x60, 0x6A));
        Gfx_Fill(rx - 1, ry, 1, 8, GFX_RGB8(0x84, 0x86, 0x92));
        /* the cradle, a U the tube drops into */
        Gfx_Fill(rx - 5, ry - 6, 11, 5, GFX_RGB8(0x2A, 0x2B, 0x31));
        Gfx_Fill(rx - 4, ry - 5, 9, 3, GFX_RGB8(0x6E, 0x70, 0x7A));
        Gfx_Fill(rx - 2, ry - 5, 5, 3, GFX_RGB8(0x1C, 0x1C, 0x21));
        Gfx_Fill(rx - 4, ry - 5, 9, 1, GFX_RGB8(0x96, 0x98, 0xA4));
    }

    /* The counterweight end, behind the pivot. */
    const int bx = px + (px - sx) / 4, by = py + (py - sy) / 4;

    /* The tube: drawn dark and wide, then light and narrow inside it, so
     * it has an edge. One flat line at this size reads as a scratch. */
    DrawThickLine(bx, by, sx, sy, 28, ARM_DARK);
    DrawThickLine(bx, by, sx, sy, 12, ARM_TUBE);

    /* The headshell: a block across the end of the tube, not along it,
     * with the cartridge under its front edge. Set at a slight angle to
     * the arm, which is how a headshell is actually mounted and what
     * stops this reading as a stick with a lump on it. */
    {
        const int L = armLen ? armLen : 1;
        const int b1x = sx + ((px - sx) * 13) / L, b1y = sy + ((py - sy) * 13) / L;
        const int b2x = sx + ((px - sx) * 11) / L, b2y = sy + ((py - sy) * 11) / L;
        const int tipx = sx + ((px - sx) * 2) / L, tipy = sy + ((py - sy) * 2) / L;

        DrawThickLine(b1x, b1y, tipx, tipy, 56, ARM_SHELL);
        DrawThickLine(b2x, b2y, tipx, tipy, 40, ARM_TUBE);
        DrawThickLine(tipx, tipy, sx, sy, 16, ARM_SHELL);
    }

    /* The pivot: a bearing cup with the arm standing out of it. */
    DrawDisc(px, py, 7 * 16, ARM_SHELL);
    DrawDisc(px, py, 6 * 16, ARM_DARK);
    DrawDisc(px, py, 3 * 16 + 8, ARM_TUBE);

    /* The counterweight: a drum on the short end, which is the thing
     * that makes an arm look balanced rather than snapped off. */
    DrawDisc(bx, by, 6 * 16 + 8, ARM_SHELL);
    DrawDisc(bx, by, 5 * 16 + 8, ARM_DARK);
    DrawDisc(bx, by, 2 * 16 + 8, isPlaying ? ARM_TUBE : ARM_SHELL);
}

/* Puts back rows y0..y1 of the baked shell.
 *
 * The whole point of having it as a picture is that any part of it can
 * be restored without redrawing the rest - which is what lets the label
 * be repainted for a sweeping title while the ribs, screws and bevel
 * around it are left alone. */
static void BlitShellBox(int sx, int sy, int x0, int y0, int x1, int y1)
{
    static uint16_t rowbuf[CASSETTE_W];
    TapePalette();
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CASSETTE_W) x1 = CASSETTE_W;
    if (y1 > CASSETTE_H) y1 = CASSETTE_H;

    for (int y = y0; y < y1; y++) {
        const unsigned char *row = &kCassette[y * CASSETTE_W];
        int x = x0;
        while (x < x1) {
            while (x < x1 && !row[x]) x++;
            int run = 0;
            while (x + run < x1 && row[x + run]) {
                rowbuf[run] = s_tapePal[row[x + run]];
                run++;
            }
            if (run) Gfx_Blit(sx + x, sy + y, run, 1, rowbuf, run);
            x += run;
        }
    }
}

static void BlitShellRows(int sx, int sy, int y0, int y1)
{
    BlitShellBox(sx, sy, 0, y0, CASSETTE_W, y1);
}


/* The cover, at one size whatever the picture is.
 *
 * A frame that grew and shrank with the art would move the column beside
 * it from track to track, which is exactly what a still card exists not
 * to do. So the square is fixed and the art is fitted inside it,
 * centred, with the ground showing through wherever the shape does not
 * match - a wide cover keeps its shape rather than being stretched.
 *
 * Blitted, not plotted. This went to the screen a pixel at a time
 * through Gfx_Pixel, which is a bounds test and a dirty-span update per
 * pixel: fifteen thousand calls, several times a second, on the thread
 * that also draws the menus. That was the stutter. */
static void CardArt(int x, int y, int side)
{
    int aw = 0, ah = 0;
    const uint16_t *px = Skin_TopArt(&aw, &ah);

    if (!px || aw <= 0 || ah <= 0) {
        DrawNoCover(x, y, side);
        DrawRoundedFrame(x - 2, y - 2, side + 4, side + 4,
                         CARD_ART_RADIUS, 2, C_ACCENT, C_BG);
        return;
    }

    if (aw > side) aw = side;
    if (ah > side) ah = side;

    /* The frame goes round the picture, not round the square it sits in.
     *
     * Framing the square meant a cover that is not square showed a band
     * of panel colour between itself and its own border - fourteen pixels
     * of it top and bottom on a four-by-three cover, which reads as a
     * heavy mount rather than as a picture. Round the picture there is
     * one pixel of edge whatever shape arrives.
     *
     * What the picture does not fill takes the page's own ground, so it
     * disappears rather than becoming a visible surround. */
    const int ox = x + (side - aw) / 2, oy = y + (side - ah) / 2;

    if (oy > y)              Gfx_Fill(x, y, side, oy - y, C_BG);
    if (oy + ah < y + side)  Gfx_Fill(x, oy + ah, side, y + side - oy - ah, C_BG);
    if (ox > x)              Gfx_Fill(x, oy, ox - x, ah, C_BG);
    if (ox + aw < x + side)  Gfx_Fill(ox + aw, oy, x + side - ox - aw, ah, C_BG);

    Gfx_Blit(ox, oy, aw, ah, px, aw);

    /* The theme's own colour, two pixels thick, and rounded. A cover is
     * the brightest thing on the screen and a single grey line round it
     * read as the edge of a window; this reads as a frame the picture
     * has been put in. */
    DrawRoundedFrame(ox - 2, oy - 2, aw + 4, ah + 4,
                     CARD_ART_RADIUS, 2, C_ACCENT, C_BG);
}

bool Skin_DrawTopCard(int buffer, int card,
                      const char *trackName, const char *artist,
                      const char *nextName,
                      bool hasTrack, bool isPlaying,
                      uint32_t elapsedMs, uint32_t totalMs,
                      uint32_t sampleRate, uint32_t bitrateKbps, int channels,
                      bool vbr, int queuePos, int queueLen,
                      uint32_t sleepRemainingSec)
{
    const int W = 256, H = 192;

    /* Whole seconds, not milliseconds: the clock is the only thing here
     * that moves, and it moves once a second. Everything else in the
     * signature changes only on a track change. */
    uint32_t sec = elapsedMs / 1000;
    uint32_t sig = TextHash(trackName) * 31u
                 + TextHash(artist) * 37u
                 + TextHash(nextName) * 41u
                 + (uint32_t)card * 1009u
                 + sec * 7u + (totalMs / 1000) * 13u
                 /* The cassette's reels turn, so it asks to be repainted
                  * four times a second rather than once. It can afford
                  * to: everything but the window and the counter is left
                  * alone on a repaint. */
                 + ((card == 2 && isPlaying) ? (elapsedMs / 250u) * 7919u : 0u)
                 /* And the gramophone's label turns, at 33rpm. */
                 + ((card == 3 && isPlaying) ? (elapsedMs / 150u) * 6247u : 0u)
                 + (uint32_t)(hasTrack ? 1 : 0) + (uint32_t)(isPlaying ? 2 : 0)
                 + bitrateKbps * 3u + sampleRate
                 + (uint32_t)(queuePos * 101 + queueLen)
                 + sleepRemainingSec * 17u
                 /* The art arrives after the card has already been drawn
                  * once - it is decoded on the worker thread while the
                  * track opens - so its generation is in the signature or
                  * the frame stays empty until the clock ticks. */
                 + Skin_TopArtGeneration() * 8191u;
    if (s_cardScrolling) sig += s_marqueeTick * 131u;
    sig |= 1u;   /* never collide with the "invalidated" marker */

    if (buffer >= 0 && buffer < 2) {
        if (s_cardSig[buffer] == sig) return false;
        s_cardSig[buffer] = sig;
    }

    /* Reset before any line is drawn, because each of them may set it. */
    s_cardScrolling = false;

    /* Any other card painting into this buffer destroys whatever the
     * cassette left there, so the next cassette paint has to be a whole
     * one. Without this, switching away and back showed the old card
     * with only the tape window redrawn over it. */
    if (buffer >= 0 && buffer < 2) {
        if (card != 1) s_coverHeld[buffer] = 0;
        if (card != 2) s_tapeHeld[buffer] = 0;
        if (card != 3) s_gramHeld[buffer] = 0;
    }

    /* Every card but two starts from a cleared screen with the accent
     * band across the top.
     *
     * The cassette and the gramophone are the exceptions: each paints
     * its own lit ground over the whole screen, so this clear was fifty
     * thousand pixels written and then immediately covered - on every
     * repaint, several times a second. It was the single largest cost
     * either of them had, and on the gramophone it survived the whole
     * of that card's optimisation pass because it happens before the
     * card is even reached. */
    /* Everything about the cover card that does not move. The clock,
     * the progress bar and the two sweeping lines are deliberately
     * absent: those are what a held repaint exists to redraw. */
    uint32_t coverStatic = TextHash(trackName) * 31u
                         + TextHash(artist) * 37u
                         + Skin_TopArtGeneration() * 8191u
                         + sampleRate + bitrateKbps * 3u
                         + (uint32_t)channels * 71u + (vbr ? 13u : 0u)
                         + (uint32_t)(queuePos * 101 + queueLen)
                         + (uint32_t)(hasTrack ? 1 : 0);
    coverStatic |= 1u;

    const bool coverHeld = (card == 1 && hasTrack &&
                            buffer >= 0 && buffer < 2 &&
                            s_coverHeld[buffer] == coverStatic);

    if (card != 2 && card != 3 && !coverHeld) {
        Gfx_Fill(0, 0, W, H, C_BG);

        /* A single accent band across the top carries the app identity
         * so the card does not read as a blank screen with text on it. */
        Gfx_Fill(0, 0, W, 3, C_ACCENT);
    }

    if (!hasTrack) {
        /* The cassette skipped that clear because it paints its own
         * ground - but on this path there is no cassette to paint it,
         * so the message would go on top of whatever the buffer still
         * held, which is the last cassette it drew. And the buffer stops
         * being one that holds a shell, or the next track to start would
         * repaint only the windows and leave this message on the label.
         */
        if (card == 2 || card == 3) {
            Gfx_Fill(0, 0, W, H, C_BG);
            Gfx_Fill(0, 0, W, 3, C_ACCENT);
            if (buffer >= 0 && buffer < 2) {
                s_tapeHeld[buffer] = 0;
                s_gramHeld[buffer] = 0;
            }
        }

        const char *msg = "Nothing playing";
        Gfx_TextBig(( W - Gfx_TextBigWidth(2, 7, msg)) / 2, 84, 2, 7, C_TEXT_DIM, msg);
        return true;
    }

    char line[64], elapsed[16], total[16];
    FormatTime(elapsed, sizeof(elapsed), elapsedMs);
    FormatTime(total, sizeof(total), totalMs);

    /* ---- Cover -----------------------------------------------------
     *
     * A record sleeve: the cover square on the left, and everything the
     * card has to say in a column beside it.
     *
     * The first version of this put the names in a bar across the top
     * with the picture underneath. It read as a picture with a caption
     * stuck above it - the two halves were stacked, so the eye went
     * through the text on its way to the cover and the cover had the
     * whole width to be small in. Side by side, the square is the
     * subject and the column is what is written on the back of it.
     *
     * The column is narrow, which is the price of a square cover on a
     * screen this shape. Anything too wide for it sweeps. */
    if (card == 1) {
        /* The square is 116 rather than a rounder number because the
         * column has to hold a title. At 124 it was 99 pixels wide, and
         * "02 - Kid A.mp3" at three halves is 108 - so an ordinary
         * filename swept, every second, forever. Eight pixels off the
         * cover buys the column enough to hold one still. */
        /* Twelve from the edge, not eight. On the console the picture
         * sat almost against the bezel while the column beside it had
         * room to spare - the card looked pushed into the corner rather
         * than laid out. The four pixels come off the column, which the
         * title can afford. */
        const int side = 116, ax = 12, ay = 32;
        /* A character's width clear of the cover, not half of one. At
         * ten the first letter of the title sat almost against the
         * picture's edge and the two read as one block; sixteen is the
         * gap the eye expects between a picture and the words about it.
         * The column loses those six pixels, which the title can afford
         * now that it carries the track's name and not the singer's as
         * well. */
        const int cx = ax + side + 16;               /* the column */
        const int cw = W - cx - 8;

        if (!coverHeld) Gfx_Fill(0, 0, W, 3, C_ACCENT);

        /* The player's own name, in the band left over above the cover.
         * The title sits to the right of it and the cover starts below,
         * so this strip was empty on every track. Centred in what it has
         * rather than sitting on the accent band. */
        if (!coverHeld) {
            const int bandTop = 3, bandH = ay - 3 - 2;

            /* The round face, whatever the reader chose for lists and
             * settings.
             *
             * A wordmark is not body text: it should look the same on
             * every theme and at every font setting, because it is the
             * one thing on the screen that is a name rather than a
             * label. Round of the four - set side by side at this size,
             * Narrow is cramped and Square is barely distinguishable
             * from the default, while Round has letterforms of its own.
             *
             * The face is put back immediately, so nothing else on the
             * card is affected by having borrowed it. */
            const int wasFont = Gfx_FontStyle();
            Gfx_SetFontStyle(GFX_FONT_ROUND);

            /* Over the middle of the cover, not against the left edge.
             * The band belongs to the picture below it - the column
             * starts to the right of both - so the name reads as the
             * picture's heading rather than as something pushed into a
             * corner. The height is untouched: it is still centred in
             * the band it was given. */
            /* At its own size, not blown up, and the word in the page's
             * ink rather than the accent.
             *
             * Scaled up it was the loudest thing on a card whose subject
             * is the picture and the track - a name that belongs in the
             * corner was competing with both. Left at the size the font
             * was drawn it is crisp rather than interpolated, and the
             * note keeps the accent so the mark still reads as a mark. */
            const int markW = 11 + Gfx_TextWidth("DSXP");
            const int markX = ax + (side - markW) / 2;

            DrawNoteIcon(markX, bandTop + (bandH - 9) / 2, C_ACCENT);
            Gfx_Text(markX + 11, bandTop + (bandH - GFX_GLYPH_H) / 2,
                     C_TEXT, "DSXP");

            Gfx_SetFontStyle(wasFont);
        }

        /* The picture, which is the expensive one and the one that has
         * no reason to be drawn twice for the same track. */
        if (!coverHeld) CardArt(ax, ay, side);
        else {
            /* The lines that do move, cleared to their own ground
             * first. Only these bands - the picture beside them and the
             * wordmark above are already right. */
            Gfx_Fill(cx, ay, cw, 40, C_BG);          /* title and artist */
            Gfx_Fill(cx, ay + 86, cw, 30, C_BG);     /* bar and the times */
        }

        /* Half again, not double. The column is what is left of 256
         * pixels after a square cover, and at double size that is eight
         * characters - so every title but the shortest swept, all the
         * time. At three halves it is a dozen, and most of them sit
         * still. */
        /* Twice the size of everything else on the card.
         *
         * Eight quarters is the one scale above single size that needs
         * no interpolation at all: every pixel of the font becomes a
         * clean two by two block, so cut crisp this is the wordmark's
         * letters exactly, doubled. No other size can say that - five
         * and six have to decide what to do with a half pixel.
         *
         * It costs sweeping, and at this size it costs a lot of it:
         * about eight characters clear the column, so most names move.
         * That is the trade for a name readable across a room. */
        /* And in the wordmark's face, not the reader's.
         *
         * The rest of the card follows whatever face is chosen in Setup,
         * because the rest of the card is labels and figures and those
         * should read the way the reader asked for. These two are the
         * card's own voice - the name of the player and the name of the
         * track - and they are set in the same face for the same reason
         * a heading and a masthead match on a page.
         *
         * Saved and put back around the draw, so nothing after it
         * inherits a face it did not ask for. */
        {
            /* Hard edges as well as the same face, because the wordmark
             * above is drawn at its own size and is therefore made of
             * whole pixels in one colour. Smoothed beside it, the name's
             * shaded edges read as a shadow rather than as a curve - the
             * eye compares the two lines and sees a different ink. */
            const int wasTitleFont = Gfx_FontStyle();
            Gfx_SetFontStyle(GFX_FONT_ROUND);
            Gfx_SetBigCrisp(true);

            if (DrawTitleCentred(cx, cw, ay + 2, C_TEXT, trackName,
                                 s_marqueeTick, 8))
                s_cardScrolling = true;

            Gfx_SetBigCrisp(false);
            Gfx_SetFontStyle(wasTitleFont);
        }
        /* Two rows lower than it was: the name above is sixteen rows
         * tall now rather than ten, and at the old position the two
         * lines were a pixel apart. */
        if (DrawTitleCentred(cx, cw, ay + 22, C_ACCENT, artist, s_marqueeTick, 4))
            s_cardScrolling = true;

        /* Two lines rather than three. It is the least important block on
         * the card and it was taking as much height as the names above
         * it; the rate and the channel count sit together happily. */
        snprintf(line, sizeof(line), "%lu kbps%s", (unsigned long)bitrateKbps,
                 vbr ? " VBR" : "");
        if (!coverHeld) Gfx_Text(cx, ay + 42, C_TEXT_DIM, line);
        snprintf(line, sizeof(line), "%lu Hz %s", (unsigned long)sampleRate,
                 channels == 1 ? "Mono" : "Stereo");
        if (!coverHeld) Gfx_Text(cx, ay + 53, C_TEXT_DIM, line);

        /* Where in the queue this is, with the rest of the facts about
         * the track rather than stranded under the cover on its own. */
        if (queueLen > 0) {
            snprintf(line, sizeof(line), "Track %d of %d", queuePos + 1, queueLen);
            if (!coverHeld) Gfx_Text(cx, ay + 64, C_TEXT_DIM, line);
        }

        Gfx_Fill(cx, ay + 90, cw, 5, C_TROUGH);
        if (totalMs > 0) {
            /* From whole seconds, like the signature above.
             *
             * It used to come from the millisecond figure while the
             * signature that decides whether to repaint at all came from
             * the second - so the bar could move a pixel without the
             * card noticing, and what was on screen then depended on
             * which millisecond the last repaint happened to land on. A
             * card whose picture is not a function of its signature
             * cannot be repainted in parts, because there is no way to
             * know which parts are stale. */
            uint32_t f = (uint32_t)(((uint64_t)sec * (uint32_t)(cw - 1) * 1000u)
                                    / totalMs);
            if (f > (uint32_t)(cw - 1)) f = (uint32_t)(cw - 1);
            if (f > 0) Gfx_Fill(cx, ay + 90, (int)f, 5, C_ACCENT);
        }
        Gfx_Text(cx, ay + 100, C_TEXT_DIM, elapsed);
        Gfx_Text(cx + cw - Gfx_TextWidth(total), ay + 100, C_TEXT_DIM, total);

        if (sleepRemainingSec > 0) {
            snprintf(line, sizeof(line), "sleep %lu:%02lu",
                     (unsigned long)(sleepRemainingSec / 60),
                     (unsigned long)(sleepRemainingSec % 60));
            Gfx_Text(cx, ay + 112, C_ACCENT, line);
        }

        /* What is coming next, on the strip below the cover that was
         * otherwise empty. A card is the screen you leave on while the
         * music plays, and the one thing it could not tell you was what
         * happens when this track ends.
         *
         * Marked with the same triangle the file list uses for the
         * playing row, so the two screens agree about what that shape
         * means, and set in the theme's colour because it is the line
         * about the future rather than the present. */
        if (nextName && nextName[0]) {
            /* Centred in what is left below the cover, rather than a
             * fixed distance under it - the strip is forty-two rows and
             * the line was sitting in the top third of it. */
            const int artBottom = ay + side + 2;
            const int ny = artBottom + (H - artBottom - GFX_GLYPH_H) / 2;

            /* This line sweeps too, so on a held repaint it gets its own
             * strip cleared - the rest of the space under the cover has
             * nothing in it and does not need touching. */
            if (coverHeld) Gfx_Fill(0, ny - 2, W, GFX_GLYPH_H + 4, C_BG);

            /* Marker and label in the theme's colour, the name in the
             * page's own ink. The whole line was accent, which made the
             * name of a track that is not playing yet as loud as the one
             * that is - and left nothing saying what the line was for. */
            DrawTriangle(ax + 3, ny + 4, 4, true, C_ACCENT);
            Gfx_Text(ax + 12, ny, C_ACCENT, "Next");

            const int nx = ax + 12 + Gfx_TextWidth("Next") + 6;
            const int nw = W - nx - 10;

            char shown[FB_NAME_LEN];
            TextFold_Utf8(nextName, shown, sizeof(shown));
            if (Gfx_TextScroll(nx, ny, nw, C_TEXT, shown, s_marqueeTick, 4))
                s_cardScrolling = true;
        }

        /* What is now on screen, so the next repaint knows the picture
         * and the wordmark are already there. */
        if (buffer >= 0 && buffer < 2) s_coverHeld[buffer] = coverStatic;
        return true;
    }

    /* ---- Cassette --------------------------------------------------
     *
     * A tape deck's window, which is the one picture that says "this is
     * playing" without a single moving bar.
     *
     * The spools carry the position: the left one starts fat and thins
     * as the track runs, the right starts bare and fills. So the card
     * reports where you are the way a cassette does - by how much is
     * left on each side - and no progress bar is drawn at all, because
     * the picture already is one.
     *
     * And the hubs turn. The card is already redrawn when the second
     * changes, so the teeth step round with it and cost nothing extra;
     * a real hub spins far faster than that, but what is wanted here is
     * the fact of turning rather than the rate. Stopped, it stops.
     */
    if (card == 2) {
        /* Only what moved.
         *
         * The shell, the frame, the ground and the artist do not change
         * while a track plays; the label's sweeping title, the spools
         * and the counter do. Redrawing all of it four times a second
         * was costing two fifths of the drawing budget, and almost all
         * of that was spent putting back pixels that were already
         * correct.
         *
         * Each of the two buffers remembers what static content it
         * holds, because the top screen alternates between them and a
         * partial repaint is only safe on a buffer that already has the
         * rest of the picture in it. */
        const int cw2 = CASSETTE_W;
        /* Centred both ways. The artist used to sit above it and does
         * not any more, so the shell is the whole of the card and takes
         * the width the frame leaves it; the height follows from the
         * proportions of the object, not from what is left over. */
        const int cx2 = (W - cw2) / 2, cy2 = (H - CASSETTE_H) / 2;

        /* Never zero: that is the value the buffer is marked with when
         * its contents can no longer be trusted. */
        const uint32_t held = 1u | (TextHash(trackName) * 31u
                            + (uint32_t)Skin_Theme() * 1009u
                            + (uint32_t)(totalMs / 1000u) * 7u
                            + (uint32_t)(hasTrack ? 1 : 0));

        const bool whole = (buffer < 0 || buffer > 1) ||
                           s_tapeHeld[buffer] != held;
        if (buffer >= 0 && buffer < 2) s_tapeHeld[buffer] = held;

        if (whole) {
            /* A ground that is not flat: lightest across the middle
             * where the shell sits, falling away above and below, so the
             * cassette is lit rather than pasted on. */
            for (int y = 0; y < H; y++) {
                const int d = (y < H / 2) ? (H / 2 - y) : (y - H / 2);
                Gfx_Fill(0, y, W, 1, Gfx_Lerp(C_SURFACE, C_BG, d, H / 2));
            }

            /* Four corner marks and no frame between them.
             *
             * Each one is an L that opens toward the middle of the
             * screen, so the four of them bracket the shell. All four
             * used to be drawn with the arms going right and down,
             * which is only correct at the top left - the other three
             * pointed out of the screen.
             *
             * They sit clear of the shell rather than on it: the shell
             * is blitted after this, and an arm that reached under it
             * would simply be covered. */
            for (int i = 0; i < 4; i++) {
                const bool right = (i & 1) != 0, low = (i & 2) != 0;
                const int cx = right ? W - 5 : 4;
                const int cy = low   ? H - 5 : 4;
                Gfx_Fill(right ? cx - 3 : cx, cy, 4, 1, C_ACCENT);
                Gfx_Fill(cx, low ? cy - 3 : cy, 1, 4, C_ACCENT);
            }

            BlitShellRows(cx2, cy2, 0, CASSETTE_H);
        }

        /* The label is only touched when the title is actually moving.
         *
         * A name that fits is painted once and then stays painted, so on
         * most tracks a repaint is the window and the counter and
         * nothing else. Only a sweeping one needs its paper put back
         * underneath it every time. */
        const int labelW = CASSETTE_STRIP_X1 - CASSETTE_TITLE_X - 4;
        const bool sweeping = Gfx_TextWidth(trackName) > labelW;

        /* The tape's length, printed on the label the way a real one
         * carries it. Not a counter and not the track's time - a C60 is
         * a C60 whatever is on it - so it never moves, and it is the
         * one printed marking that has to know the label's colour. */
        if (whole) {
            /* The brand, printed on the header band in whichever ink
             * that band is printing. */
            const uint16_t ink = TapeInk(C_ACCENT);
            Gfx_Text(cx2 + CASSETTE_BRAND_X, cy2 + CASSETTE_BRAND_Y,
                     ink, "DSXP");
            /* The mark in the type box, in the band's own ink for the
             * same reason - it is printed on the same band. */
            Gfx_Fill(cx2 + CASSETTE_TYPE_X, cy2 + CASSETTE_TYPE_Y,
                     CASSETTE_TYPE_W, 2, ink);
        }

        if (whole || sweeping) {
            /* Only the box the title is in. The badge, the ruled lines
             * and the paper either side of the writing never change, and
             * putting all of them back was four fifths of what a
             * sweeping repaint cost. */
            if (!whole)
                BlitShellBox(cx2, cy2,
                             CASSETTE_TITLE_X - 1, CASSETTE_STRIP_Y + 1,
                             CASSETTE_TITLE_X + labelW + 1,
                             CASSETTE_STRIP_Y + 2 + GFX_GLYPH_H + 2);

            /* The track's name, written on the paper where it belongs,
             * in ink rather than in the theme's text colour - a white
             * strip carrying the page's colour would be the one part of
             * the shell that did not believe it was a cassette. */
            const int wasFont = Gfx_FontStyle();
            Gfx_SetFontStyle(GFX_FONT_ROUND);
            if (Gfx_TextScroll(cx2 + CASSETTE_TITLE_X, cy2 + CASSETTE_STRIP_Y + 2,
                               labelW, TAPE_C_INK, trackName, s_marqueeTick, 4))
                s_cardScrolling = true;
            Gfx_SetFontStyle(wasFont);
        }

        /* ---- the two windows ----
         *
         * A porthole over each hub rather than one long slot. Nothing
         * else is visible through a closed shell, so how full each
         * circle is becomes the whole of what the picture says - which
         * is the point of drawing a cassette rather than a bar.
         *
         * Each is built in its own small square buffer and blitted, so
         * a repaint touches two circles and nothing else. */
        {
            enum { WINR = CASSETTE_WINR, WIND = WINR * 2 + 1 };
            static uint16_t win[WIND][WIND];

            const int rFull = CASSETTE_WINR - 2, rCore = HUB_D / 2;
            int done = (totalMs > 0)
                     ? (int)(((uint64_t)elapsedMs * 1000u) / totalMs) : 0;
            if (done < 0) done = 0;
            if (done > 1000) done = 1000;

            /* Wound tape goes as the square root of what is on the spool
             * - area, not radius - which is why a cassette's reels look
             * even for most of a side and then move quickly at the end. */
            const int aFull = rFull * rFull - rCore * rCore;
            const int rLeft  = Isqrt(rCore * rCore + (aFull * (1000 - done)) / 1000);
            const int rRight = Isqrt(rCore * rCore + (aFull * done) / 1000);

            /* Four steps a second. The windows are all that is repainted,
             * so it can be asked for often enough to look like turning
             * rather than like a clock ticking. */
            const int turn = isPlaying ? (int)(elapsedMs / 250u) : 0;

            /* A square around the hub is enough while the pack keeps
             * its radius, and the pack keeps its radius for twenty
             * seconds at a time. The square's furthest corner is at
             * HUBSQ * 1.414 from the centre, which is well inside the
             * porthole, so nothing outside the hole can be touched by
             * it and the blit needs no circular clip. */
            enum { HUBSQ = HUB_D / 2 + 1 };
            const int rSide[2] = { rLeft, rRight };
            const bool tracked = (buffer >= 0 && buffer < 2);

            for (int side = 0; side < 2; side++) {
                const bool hubOnly = tracked && !whole &&
                                     s_tapeSpoolR[buffer][side] == rSide[side];
                const int r = hubOnly ? HUBSQ : WINR;
                const int d = r * 2 + 1;
                const int cxw = cx2 + (side ? CASSETTE_SPX_R : CASSETTE_SPX_L);
                const int cyw = cy2 + CASSETTE_CY;

                for (int y = 0; y < d; y++)
                    for (int x = 0; x < d; x++) win[y][x] = WINDOW_BG;

                DrawSpool(&win[0][0], WIND, d, d, r, r,
                          rSide[side], side ? -turn : turn);

                if (hubOnly) {
                    for (int y = 0; y < d; y++)
                        Gfx_Blit(cxw - r, cyw - r + y, d, 1, &win[y][0], d);
                } else {
                    /* Only what shows through the hole. The shell around
                     * it is already correct, so anything outside the
                     * circle is left exactly as the baked picture put
                     * it. */
                    for (int y = 0; y < d; y++) {
                        const int dy = y - r;
                        const int half = Isqrt(r * r - dy * dy);
                        if (half <= 0) continue;
                        Gfx_Blit(cxw - half, cyw - r + y, half * 2, 1,
                                 &win[y][r - half], half * 2);
                    }
                }

                if (tracked) s_tapeSpoolR[buffer][side] = rSide[side];
            }
        }

        return true;
    }

    /* ---- Gramophone ------------------------------------------------
     *
     * A record on a deck, and the arm creeping in across it.
     *
     * The same idea as the cassette and the same reason for it: the
     * position is in the picture rather than beside it. Where the arm
     * sits is how far through you are, which is how you read a record
     * across a room without going to look at it.
     *
     * What turns is the label, because on a real record that is the
     * only part that can. The grooves are concentric and the light in
     * the room does not go round with the disc, so both of those are
     * baked once and never touched again. */
    if (card == 3) {
        /* Centred across, and low enough to leave the track's name a
         * line of its own along the top. */
        const int rcx = SKIN_DECK_CX, rcy = SKIN_DECK_CY;   /* the spindle */
        const int px = 227, py = 55;                /* the arm's pivot */
        const int armLen = 100;

        GramPalette();

        /* Where the stylus is: the outer groove at the start, the inner
         * one at the end. Linear in radius rather than in area - a
         * record's groove pitch is constant, which is exactly why a
         * side's worth of music fills a predictable band of it. */
        int done = (totalMs > 0)
                 ? (int)(((uint64_t)elapsedMs * 1000u) / totalMs) : 0;
        if (done < 0) done = 0;
        if (done > 1000) done = 1000;

        const int rOuter = RECORD_R - 5, rInner = RECORD_LABR + 4;
        const int rs = rOuter - ((rOuter - rInner) * done) / 1000;

        /* Twelve frames of thirty degrees, one every 150ms: a turn every
         * 1.8 seconds, which is thirty-three and a third revolutions a
         * minute. The number is not decoration - a record turning at
         * visibly the wrong speed is the sort of thing that nags at you
         * without your being able to say why. */
        const int spin = isPlaying ? (int)(elapsedMs / 150u) : 0;

        /* What each buffer is holding.
         *
         * Everything on this card except the label and the arm is
         * painted once and left alone - and the arm moves about forty
         * times in a whole side, so on the overwhelming majority of
         * repaints the only thing that has changed is which way round
         * the label is. Redrawing the ground, the deck, the names and
         * the arm to show that was a million and a half pixels a second
         * and almost all of it was putting back pixels that were
         * already right. */
        const bool tracked = (buffer >= 0 && buffer < 2);
        const uint32_t held = 1u | (TextHash(trackName) * 31u
                            + (uint32_t)Skin_Theme() * 1009u
                            + (uint32_t)Audio_Volume() * 131u
                            + (uint32_t)(hasTrack ? 1 : 0)
                            + (uint32_t)(isPlaying ? 2 : 0));
        const bool whole = !tracked || s_gramHeld[buffer] != held;

        const int titleW = W - 24;
        const bool sweeping = Gfx_TextWidth(trackName) > titleW;

        /* Where the name has scrolled to, rather than what tick it is.
         *
         * The marquee steps six times a second and the label turns not
         * quite seven, so on most repaints only one of the two has
         * moved - and the name was being lifted and put back on every
         * one of them regardless. It also holds still for a second at
         * each end of its travel, and those frames were redrawing it
         * into exactly the same place. */
        const int titleOff = sweeping
            ? Gfx_MarqueeOffsetFor(s_marqueeTick, trackName, titleW, 4) : 0;

        /* Where the arm is, and the box it is in. */
        int sx, sy, ax0, ay0, ax1, ay1;
        ArmStylus(px, py, rcx, rcy, rs, armLen, &sx, &sy);
        ArmBox(px, py, sx, sy, &ax0, &ay0, &ax1, &ay1);

        if (whole) {
            BlitDeckBox(rcx, rcy, 0, 0, W, H);

            /* The plinth carries its own moulding. What is left to
             * print on it is what the moulding cannot say: which speed
             * is selected, whether the deck is running, and whose it is.
             *
             * It is running at thirty-three - the label turns once every
             * 1.8 seconds, which is where that number comes from. */
            /* Fixed inks, not the theme's. These are printed on a
             * wooden box, and the theme's text colour is chosen to sit
             * on the theme's own ground - put it on walnut and it reads
             * as a sticker somebody left there. The theme keeps the two
             * things that are actually lit. */
            const uint16_t PRINT_ON  = GFX_RGB8(0xEC, 0xE6, 0xD6);
            const uint16_t PRINT_OFF = GFX_RGB8(0x8E, 0x8A, 0x84);
            const uint16_t BADGE     = GFX_RGB8(0xC6, 0xA5, 0x62);

            const int wasFont = Gfx_FontStyle();
            Gfx_SetFontStyle(GFX_FONT_NARROW);
            for (int i = 0; i < 2; i++) {
                const int py2 = 52 + i * 24;
                const bool on = (i == 0);
                const char *n = on ? "33" : "45";
                if (on) Gfx_Fill(11, py2 + 3, 28, 2, C_ACCENT);
                Gfx_Text(8 + (34 - Gfx_TextWidth(n)) / 2, py2 + 8,
                         on ? PRINT_ON : PRINT_OFF, n);
            }

            /* The slider is the volume.
             *
             * It was a knob baked in the middle of its travel, which is
             * a decoration pretending to be a control - and the top
             * screen has no touch on this console, so nothing here can
             * ever be one. Made to show something true it is worth its
             * place: it moves when the volume does. */
            {
                const int vol = Audio_Volume();
                const int ky = 161 - (vol * 20) / 127;
                Gfx_Fill(17, ky - 1, 17, 10, GFX_RGB8(0x1A, 0x1A, 0x1E));
                Gfx_Fill(18, ky, 15, 8, GFX_RGB8(0x5E, 0x60, 0x6A));
                Gfx_Fill(18, ky, 15, 1, GFX_RGB8(0xA0, 0xA2, 0xAE));
                Gfx_Fill(22, ky + 3, 7, 2, GFX_RGB8(0x16, 0x16, 0x1A));
            }

            /* The lamp: the one thing on the plinth that changes. */
            if (isPlaying) {
                Gfx_Fill(16, 114, 18, 6, C_ACCENT);
                Gfx_Fill(17, 115, 16, 4, C_TEXT);
            } else {
                Gfx_Fill(16, 114, 18, 6, GFX_RGB8(0x2A, 0x2A, 0x30));
            }

            /* The badge, in brass the way one is screwed to a plinth -
             * clear of the corner screw, which it was sitting on. */
            Gfx_Text(25, 179, GFX_RGB8(0x3A, 0x2A, 0x14), "DSXP");
            Gfx_Text(24, 178, BADGE, "DSXP");
            Gfx_SetFontStyle(wasFont);
        }

        /* The arm, only when it has actually moved off the groove it was
         * standing in. Its old position and its new one both have to be
         * put back, so one box covering the pair of them. */
        /* What has to be put back, and in what order.
         *
         * The arm lies on top of the record, so it is drawn last; but
         * its box and the label's overlap near the end of a side, so
         * whichever of them is being redrawn can take a bite out of the
         * other. Both boxes are restored first, then the label, then the
         * arm on top of it - which is also the order the real objects
         * are stacked in. */
        const int lx0 = rcx - LABEL_D / 2 - 1, ly0 = rcy - LABEL_D / 2 - 1;
        const int lx1 = rcx + LABEL_D / 2 + 2, ly1 = rcy + LABEL_D / 2 + 2;

        const bool armMoved  = whole || !tracked || s_gramArmR[buffer] != rs;
        const bool needLabel = armMoved || s_gramSpin[buffer] != spin;
        const bool overlap   = !(ax1 <= lx0 || ax0 >= lx1 ||
                                 ay1 <= ly0 || ay0 >= ly1);
        const bool needArm   = armMoved || (needLabel && overlap);

        if (!whole) {
            /* Whenever the arm is going to be drawn, not only when it
             * moved. Its tube is drawn as coverage, so laid over the arm
             * already standing there it blends with itself and comes out
             * a shade heavier every time. */
            if (needArm) {
                /* Both positions: the one being vacated and the one
                 * being taken. The old box is remembered as the groove
                 * it was standing in, which is the only thing that would
                 * say where it was. */
                int ox0 = ax0, oy0 = ay0, ox1 = ax1, oy1 = ay1;
                if (tracked && s_gramArmR[buffer] >= 0) {
                    int ox, oy, bx0, by0, bx1, by1;
                    ArmStylus(px, py, rcx, rcy, s_gramArmR[buffer], armLen,
                              &ox, &oy);
                    ArmBox(px, py, ox, oy, &bx0, &by0, &bx1, &by1);
                    if (bx0 < ox0) ox0 = bx0;
                    if (by0 < oy0) oy0 = by0;
                    if (bx1 > ox1) ox1 = bx1;
                    if (by1 > oy1) oy1 = by1;
                }
                BlitDeckBox(rcx, rcy, ox0, oy0, ox1, oy1);
            }
        }

        if (needLabel) {
            /* The spindle comes with it: baked into every frame of the
             * label rather than plotted on top, which is what took the
             * last per-pixel work off this card's common path. */
            DrawLabel(rcx, rcy, spin);
            if (tracked) s_gramSpin[buffer] = spin;
        }

        if (needArm) {
            DrawArm(px, py, rcx, rcy, rs, armLen, isPlaying);
            if (tracked) s_gramArmR[buffer] = rs;
        }

        /* The track's name along the top, centred when it fits and
         * sweeping when it does not - and only then does it need its
         * own strip of ground back underneath it. */
        /* Said every frame it is moving, not only on the frames it is
         * redrawn - the loop stops asking for repaints the moment this
         * goes quiet, and a marquee that stops being asked for stops. */
        if (sweeping) s_cardScrolling = true;

        if (whole || (sweeping && (!tracked || s_gramTitle[buffer] != titleOff))) {
            if (!whole) BlitDeckBox(rcx, rcy, 11, 4, 13 + titleW, 14);

            const int wasFont = Gfx_FontStyle();
            Gfx_SetFontStyle(GFX_FONT_ROUND);
            /* On its own shadow: walnut is a busy ground and the
             * accent is whatever the theme says it is, so on some of
             * them the name would sit in the grain rather than on it. */
            const uint16_t sh = GFX_RGB8(0x2A, 0x1C, 0x10);
            const int tw = Gfx_TextWidth(trackName);
            if (tw > titleW) {
                Gfx_TextScroll(13, 6, titleW, sh,
                               trackName, s_marqueeTick, 4);
                Gfx_TextScroll(12, 5, titleW, C_ACCENT,
                               trackName, s_marqueeTick, 4);
            } else {
                Gfx_Text((W - tw) / 2 + 1, 6, sh, trackName);
                Gfx_Text((W - tw) / 2, 5, C_ACCENT, trackName);
            }
            Gfx_SetFontStyle(wasFont);
            if (tracked) s_gramTitle[buffer] = titleOff;
        }

        if (tracked) s_gramHeld[buffer] = held;
        return true;
    }

    /* ---- Now playing -----------------------------------------------
     *
     * Everything the player knows, which is what makes it the one to
     * pick when the top screen is the only screen being looked at.
     *
     * The big clock that used to sit in the middle of this is gone. It
     * was the same number at nearly the same size as the clock
     * beside it in the picker, and two cards whose difference is how
     * much surrounds an identical clock are not two cards. The time is
     * still here, at the ends of the progress bar where it belongs
     * beside the thing it measures. */
    if (isPlaying) {
        DrawTriangle(15, 22, 6, true, C_ACCENT);
    } else {
        Gfx_Fill(13, 16, 3, 12, C_TEXT_DIM);
        Gfx_Fill(19, 16, 3, 12, C_TEXT_DIM);
    }

    {
        /* The title takes the width left beside the transport glyph, so
         * the two never overlap however long the name is. */
        const int tx = 30, tw = W - tx - CARD_M;
        if (DrawTitleCentred(tx, tw, 16, C_TEXT, trackName, s_marqueeTick, 8))
            s_cardScrolling = true;
    }

    CardLine(40, 2, C_TEXT, artist);

    snprintf(line, sizeof(line), "%lu kbps%s   %lu Hz   %s",
             (unsigned long)bitrateKbps, vbr ? " VBR" : "",
             (unsigned long)sampleRate, channels == 1 ? "Mono" : "Stereo");
    CardLine(54, 2, C_TEXT_DIM, line);

    /* A plate for the two numbers that move, so the eye has one place to
     * look for them - the same device the file list uses for a selected
     * row, an accent rule down the leading edge. */
    const int plateY = 76, plateH = 34;
    Gfx_Fill(0, plateY, W, plateH, C_SURFACE);
    Gfx_Fill(0, plateY, 3, plateH, C_ACCENT);

    /* Proportional, not the monospaced clock face. Set on the seven-wide
     * cell the big clock uses, "1:11 / 4:22" comes out with a gap either
     * side of the slash wide enough to read as three separate things. */
    snprintf(line, sizeof(line), "%s / %s", elapsed, total);
    Gfx_TextBigProp((W - Gfx_TextBigPropWidth(12, line)) / 2, plateY + 5, 12,
                    isPlaying ? C_TEXT : C_TEXT_DIM, line);

    CardProgress(126, 6, elapsedMs, totalMs);

    if (totalMs > elapsedMs) {
        char rem[16];
        FormatTime(rem, sizeof(rem), totalMs - elapsedMs);
        snprintf(line, sizeof(line), "-%s", rem);
        Gfx_Text(W - CARD_M - Gfx_TextWidth(line), 138, C_TEXT_DIM, line);
    }
    Gfx_Text(CARD_M, 138, C_TEXT_DIM, elapsed);

    if (queueLen > 0) {
        snprintf(line, sizeof(line), "Track %d of %d", queuePos + 1, queueLen);
        Gfx_Text(CARD_M, 158, C_TEXT_DIM, line);
    }

    if (sleepRemainingSec > 0) {
        snprintf(line, sizeof(line), "sleep %lu:%02lu",
                 (unsigned long)(sleepRemainingSec / 60),
                 (unsigned long)(sleepRemainingSec % 60));
        Gfx_Text(W - CARD_M - Gfx_TextWidth(line), 158, C_ACCENT, line);
    }

    return true;
}

/* The Blue Classic top LCD is a second, smaller player panel.  It is
 * intentionally separate from the neutral overlay below: its header is
 * denser, the spectrum is boxed in, and the "Next" line stays inside the
 * same bezel instead of being an unrelated caption at the screen edge. */

static void DrawBlueClassicTopOverlay(int buffer,
                                      const char *trackName, bool hasTrack,
                                      bool isPlaying, uint32_t elapsedMs,
                                      int queuePos, int queueLen,
                                      uint32_t sampleRate,
                                      uint32_t bitrateKbps, int channels,
                                      bool vbr, const char *nextName,
                                      uint32_t sleepRemainingSec)
{
    uint32_t sig = (elapsedMs / 1000) * 7u
                 ^ TextHash(trackName) ^ TextHash(nextName)
                 ^ ((uint32_t)queuePos << 12) ^ ((uint32_t)queueLen << 18)
                 ^ bitrateKbps * 31u ^ sampleRate
                 ^ ((uint32_t)channels << 4) ^ ((uint32_t)vbr << 8)
                 ^ ((uint32_t)hasTrack << 30) ^ ((uint32_t)isPlaying << 31)
                 /* Minutes, not seconds: the strip is redrawn when this
                  * changes and once a minute is often enough for a
                  * timer measured in them. */
                 ^ ((sleepRemainingSec / 60u) * 40503u);
    if (s_overlayScrolling) sig += s_marqueeTick * 131u;

    /* Two separators, renewed each frame because the bar renderer owns
     * the field between them and repaints over them. The bezel that used
     * to sit outside these is gone: three nested lines around a 256x192
     * panel spend six pixels of every edge on decoration. */
    /* Only the footer is redrawn every frame - the visualizer's
     * reflection reaches into it. The rule under the header used to be
     * here too, and the header gradient is painted further down this
     * function, so once a second the gradient erased the rule and the
     * next frame put it back: a line that blinked in time with the
     * clock. It is drawn with the header now.
     *
     * The rule that sat at 164 is gone entirely. The visualizer draws
     * its own floor at 162 and 163, so that was a third line under two
     * that were already there. */

    /* Visualizer reflection can reach this row.  Repaint this small
     * footer every frame so its text never flickers behind a falling bar. */
    if (buffer >= 0 && buffer < 2) {
        if (s_overlaySig[buffer] == sig) return;
        s_overlaySig[buffer] = sig;
    }

    s_overlayScrolling = false;

    /* Drawn with the rest of the page, not on every frame.
     *
     * It used to be renewed sixty times a second because the meter's
     * reflection reached into it. The reflection is two rows now and
     * stops above this strip, so the fill and the text were 4600 pixels
     * and a blit per frame for a line that changes once a track - on the
     * CPU that is also decoding the audio, which is where the stutter
     * came from with the top-screen text switched on.
* The strip between the meter's floor and the bottom of the screen,
     * with the text centred in it. It used to start at 167 and be
     * measured fifteen tall, which is neither where the floor is nor
     * where the screen ends, so the line sat high in a band whose real
     * height nothing had computed. */
    const int footY = TOP_FLOOR_Y + 4;
    const int footH = GFX_H - footY;
    Gfx_Fill(0, footY, GFX_W, footH, C_BG);
    Gfx_HLine(0, footY, GFX_W, C_HAIRLINE);
    const int footTextY = footY + (footH - GFX_GLYPH_H) / 2;
    /* The sleep timer, where it can be seen without leaving the music.
     * It used to show only on the Card top screen, so anyone running the
     * meter had no sign a timer was counting at all. */
    int footRight = GFX_W - 8;
    if (sleepRemainingSec > 0) {
        /* Read the same way as the line beside it: the label in the
         * theme's colour saying what this is, the value in the reading
         * colour saying what it holds. The moon keeps the label short -
         * without it the word has to carry the meaning on its own and
         * takes half again as much of a strip the track name is already
         * using. */
        char left[16];
        snprintf(left, sizeof(left), "%lum",
                 (unsigned long)((sleepRemainingSec + 59u) / 60u));

        footRight -= Gfx_TextWidth(left);
        Gfx_Text(footRight, footTextY, C_TEXT_DIM, left);

        /* The same dot the header puts between kbps and kHz, and the
         * same ten pixels of advance - one separator on this screen, not
         * one per strip. */
        footRight -= 10;
        Gfx_Fill(footRight + 4, footTextY + 3, 2, 2, C_TEXT_DIM);

        footRight -= Gfx_TextWidth("Sleep");
        Gfx_Text(footRight, footTextY, C_ACCENT, "Sleep");
        footRight -= 8;
    }

    if (nextName && nextName[0]) {
        /* The label in the theme's colour and the name in the reading
         * colour: one says what the line is and the other says what it
         * holds, and two tones separate them without a second row. */
        char shown[96];
        StripExtension(shown, sizeof(shown), nextName);

        Gfx_Text(10, footTextY, C_ACCENT, "Next");
        int nx = 10 + Gfx_TextWidth("Next");
        Gfx_Fill(nx + 4, footTextY + 3, 2, 2, C_TEXT_DIM);
        nx += 10;
        int room = footRight - nx;
        if (Gfx_TextWidth(shown) <= room)
            Gfx_Text(nx, footTextY, C_TEXT_DIM, shown);
        else
            s_overlayScrolling = Gfx_TextScroll(nx, footTextY, room, C_TEXT_DIM,
                                                shown, s_marqueeTick, 4);
    }


    /* From the palette, not from a pair of blues. Hard-coded values here
     * were invisible on Blue Classic and wrong on every other theme -
     * a blue header over a green phosphor page reads as two programs
     * sharing a screen. */
    /* Down to the visualizer's own margin, not five rows short of it.
     * VIS_TOP_MARGIN is 76 for this theme; a header ending at 71 left a
     * band that neither the header nor the meter ever painted, so it
     * held whatever the last theme drew there. */
    /* One band, not two.
     *
     * The row above this carried the program's name, a PLAY/PAUSE word
     * and a battery - three things nobody reads while a track plays,
     * costing twenty rows of the screen that shows the music. The name
     * moves down in front of the track, where it says which program this
     * is without a row of its own, and the rest is gone.
     *
     * TOP_HEADER_H must equal VIS_TOP_MARGIN or the meter paints under
     * the header or leaves a strip nothing owns. */
    Gfx_VGradient(0, 0, GFX_W, TOP_HEADER_H, C_SURFACE, C_BG);
    Gfx_HLine(0, TOP_HEADER_H - 1, GFX_W, C_HAIRLINE);

    /* The mark goes against the right edge with the counter beneath it,
     * so the two things that are not the track share a column and the
     * track gets the whole of the left. */
    const int markW = 13 + Gfx_TextWidth("DSXP");
    const int markX = GFX_W - 6 - markW;
    DrawNoteIcon(markX, TOP_LINE1_Y, C_ACCENT);
    Gfx_Text(markX + 13, TOP_LINE1_Y, C_TEXT_DIM, "DSXP");

    const int colX = 6;

    if (!hasTrack) {
        Gfx_Text(colX, TOP_LINE1_Y, C_TEXT_DIM, "Nothing playing");
        return;
    }

    if (isPlaying)
        DrawTriangle(colX + 3, TOP_LINE1_Y + 4, 4, true, C_ACCENT);
    else {
        Gfx_Fill(colX, TOP_LINE1_Y, 3, 8, C_TEXT_DIM);
        Gfx_Fill(colX + 5, TOP_LINE1_Y, 3, 8, C_TEXT_DIM);
    }

    /* The counter is measured first so the name knows what is left. */
    char queue[32];
    if (queueLen > 0) snprintf(queue, sizeof(queue), "%d/%d", queuePos + 1, queueLen);
    else              snprintf(queue, sizeof(queue), "--/--");
    const int qw = Gfx_TextWidth(queue);
    Gfx_Text(GFX_W - 6 - qw, TOP_LINE2_Y, C_TEXT_DIM, queue);

    char title[128];
    StripExtension(title, sizeof(title), trackName ? trackName : "");
    const int nameX = colX + 12;
    if (Gfx_TextScroll(nameX, TOP_LINE1_Y, markX - nameX - 8,
                       C_TEXT, title, s_marqueeTick, 4))
        s_overlayScrolling = true;

    /* Three facts with a dot between them, indented a character from
     * the track above.
     *
     * The font is ASCII 32..126 and has no middle dot, so it is drawn
     * rather than typed - two pixels on the text's own centre line. A
     * hyphen or a slash would have read as part of a value. */
    char parts[3][28];
    int np = 0;
    snprintf(parts[np++], sizeof(parts[0]), "%lu kbps%s",
             (unsigned long)bitrateKbps, vbr ? " VBR" : "");
    snprintf(parts[np++], sizeof(parts[0]), "%lu.%lu kHz",
             (unsigned long)(sampleRate / 1000),
             (unsigned long)((sampleRate % 1000) / 100));
    snprintf(parts[np++], sizeof(parts[0]), "%s",
             channels == 1 ? "Mono" : "Stereo");

    int fx = colX + 6;
    for (int i = 0; i < np; i++) {
        if (i) {
            Gfx_Fill(fx + 4, TOP_LINE2_Y + 3, 2, 2, C_TEXT_DIM);
            fx += 10;
        }
        Gfx_Text(fx, TOP_LINE2_Y, C_ACCENT, parts[i]);
        fx += Gfx_TextWidth(parts[i]);
    }
}

void Skin_InvalidateTopOverlay(void)
{
    s_overlaySig[0] = s_overlaySig[1] = 0xFFFFFFFFu;
}

/* One layout for every theme.
 *
 * There were two - a three-row header for Blue Classic and a two-row one
 * for the rest - so the top screen did not merely recolour between
 * themes, it changed shape. A theme is a palette; the page under it
 * should be the same page. */
void Skin_DrawTopOverlay(int buffer,
                         const char *trackName, bool hasTrack, bool isPlaying,
                         uint32_t elapsedMs, uint32_t totalMs,
                         int queuePos, int queueLen, uint32_t sleepRemainingSec,
                         uint32_t glitchFrames,
                         uint32_t sampleRate, uint32_t bitrateKbps,
                         int channels, bool vbr, const char *nextName)
{
    (void)totalMs; (void)glitchFrames;
    DrawBlueClassicTopOverlay(buffer, trackName, hasTrack, isPlaying,
                              elapsedMs, queuePos, queueLen, sampleRate,
                              bitrateKbps, channels, vbr, nextName,
                              sleepRemainingSec);
}

/* ------------------------------------------------------------------ */
/* transport                                                          */
/* ------------------------------------------------------------------ */

static void FormatTime(char *out, size_t len, uint32_t ms)
{
    uint32_t sec = ms / 1000;
    snprintf(out, len, "%lu:%02lu",
             (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

/* ------------------------------------------------------------------ */
/* diagnostics page                                                   */
/* ------------------------------------------------------------------ */


/* Whole page, redrawn on any change. The numbers move constantly, so
 * there is nothing here worth caching piecewise - unlike the file list,
 * where most of the screen holds still. */
/* Panel the transport bar sits on, as a gradient span - so a single
 * piece of it can be erased back to the right colours. */
#define TP_Y   (TRANSPORT_Y + 1)
#define TP_H   (GFX_H - TRANSPORT_Y - 1)

static void DrawTransportButton(SkinButton b, bool isPlaying, bool hasTrack)
{
    const SkinRect *r = &kRects[b];

    /* The five controls stand on the plate, the two volume keys on the
     * page. Each clears its own rectangle to whichever ground it is on,
     * so a glyph that changes shape leaves nothing behind and neither
     * punches a hole in the plate under it. */
    bool volKey = (b == SKIN_BTN_VOL_DOWN || b == SKIN_BTN_VOL_UP);
    Gfx_Fill(r->x, r->y, r->w, r->h, volKey ? C_BG : C_SURFACE);

    /* Every key looks the same until it is touched, the play key
     * included. It used to carry a filled accent plate at all times, so
     * the one control whose state matters was shouting it whether it was
     * playing or stopped - and a press could only darken a plate that was
     * already the accent, which is a poor signal.
     *
     * The state moved into the glyph instead: the play or pause mark
     * takes the accent while sound is coming out and the ordinary ink
     * when it is not. */
    if (IsPressed(b))
        FillRounded(r->x, r->y, r->w, r->h, 2, C_ACCENT);

    uint16_t ink = hasTrack ? C_TEXT : C_TEXT_DIM;
    if (b == SKIN_BTN_PLAYPAUSE && isPlaying) ink = C_ACCENT;
    if (IsPressed(b)) ink = C_TEXT_ON_SEL;

    const int gx = r->x + (r->w - ICON_W) / 2;
    const int gy = r->y + (r->h - ICON_H) / 2;

    switch (b) {
    case SKIN_BTN_PREV:  Gfx_Icon16(gx, gy, ICON_H, kGlyphPrev, ink); break;
    case SKIN_BTN_NEXT:  Gfx_Icon16(gx, gy, ICON_H, kGlyphNext, ink); break;

    case SKIN_BTN_PLAYPAUSE:
        Gfx_Icon16(gx, gy, ICON_H,
                   isPlaying ? kGlyphPause : kGlyphPlay, ink);
        break;

    /* Volume keys are always live, so they keep full-strength ink even
     * with nothing loaded. Two strokes rather than a glyph: a plus and a
     * minus are the one pair where a bitmap would only be a slower way
     * of drawing two rectangles. */
    case SKIN_BTN_VOL_DOWN:
        if (true)
            Gfx_Icon16(r->x, r->y, ICON_H, kGlyphSpkQuiet, C_TEXT);
        else
            Gfx_Fill(r->x + 3, r->y + 7, 10, 2, C_TEXT);
        break;

    case SKIN_BTN_VOL_UP:
        if (true) {
            Gfx_Icon16(r->x, r->y, ICON_H, kGlyphSpkLoud, C_TEXT);
        } else {
            Gfx_Fill(r->x + 3, r->y + 7, 10, 2, C_TEXT);
            Gfx_Fill(r->x + 7, r->y + 3, 2, 10, C_TEXT);
        }
        break;

    default:
        break;
    }
}

/* A slim groove with a round handle riding on it.
 *
 * The handle is the whole message - it is what says the thing can be
 * taken hold of - so it gets to be the tall part and the track does not.
 * A groove of the same height as its handle reads as a bar that happens
 * to have a mark on it; a thin groove with a circle on top reads as a
 * control, and it is the shape every slider anyone has used already is.
 *
 * The circle's centre travels between x + r and x + w - r rather than
 * across the whole width, because a circle centred on the very end would
 * hang half off the track. That inset is why the position is not simply
 * px: the number the finger asks for and the number the knob is drawn at
 * have to come from the same place, or the handle lags the stylus at the
 * ends - so both come from the pair below. */
#define SLIDER_KNOB_R  6
#define SLIDER_TRACK_H 5



/* The distance the knob's centre travels, which is one column short of
 * the track less a radius at each end.
 *
 * Less the two radii alone put the centre at x + w - r at full scale and
 * the handle's rightmost column at x + w - one past the track, and one
 * past the erase, which is exactly the track's width for the reason the
 * comment on it gives. Nothing ever repainted that column, so pushing
 * either slider to its end and bringing it back left a stripe standing
 * at the far end of the groove.
 *
 * A span of n pixels runs from 0 to n - 1. Both the drawing and the
 * touch mapping read this, so they stay the same arithmetic. */
static int SliderSpan(int w)
{
    int span = w - 2 * SLIDER_KNOB_R - 1;
    return span < 1 ? 1 : span;
}

static int SliderKnobX(int x, int w, int permille)
{
    return x + SLIDER_KNOB_R + (SliderSpan(w) * permille) / 1000;
}

static int SliderPermilleAt(int x, int w, int px)
{
    int span = SliderSpan(w);
    int rel = px - (x + SLIDER_KNOB_R);
    if (rel < 0) rel = 0;
    if (rel > span) rel = span;
    return (rel * 1000) / span;
}

/* Knocks the corners off a short horizontal bar, which at four pixels
 * tall is the whole of "rounded ends". */
static void CapEnds(int x, int y, int w, int h, uint16_t bg)
{
    if (w < 2 || h < 2) return;
    Gfx_Pixel(x,         y,         bg);
    Gfx_Pixel(x + w - 1, y,         bg);
    Gfx_Pixel(x,         y + h - 1, bg);
    Gfx_Pixel(x + w - 1, y + h - 1, bg);
}

/* The lit face of a rounded square, brighter over the top third - the
 * same treatment the disc had, on the shape that replaced it. */
static void FillRoundedLit(int x, int y, int w, int h, int r,
                           uint16_t hi, uint16_t base)
{
    if (r * 2 > h) r = h / 2;
    const int split = h / 3;
    for (int j = 0; j < h; j++) {
        int dy = (j < r) ? (r - 1 - j)
               : (j >= h - r) ? (j - (h - r)) : -1;
        int inset = 0;
        if (dy >= 0) {
            int d = 0;
            while ((d + 1) * (d + 1) + dy * dy <= r * r) d++;
            inset = r - d;
        }
        if (w - 2 * inset > 0)
            Gfx_Fill(x + inset, y + j, w - 2 * inset, 1,
                     j < split ? hi : base);
    }
}

static void DrawSliderBarIn(int x, int y, int w, int h, int permille,
                            uint16_t fill);
static void DrawSliderBarSteps(int x, int y, int w, int h, int permille,
                               uint16_t fill, int steps);

static void DrawSliderBar(int x, int y, int w, int h, int permille)
{
    DrawSliderBarIn(x, y, w, h, permille, C_ACCENT);
}

/* `fill` because the stacked layout puts the two sliders directly above
 * one another at the same length, and two identical bars read as one
 * control drawn twice. The volume takes a muted tone: it is the one you
 * set and leave, and the progress is the one you watch. */
static void DrawSliderBarIn(int x, int y, int w, int h, int permille,
                            uint16_t fill)
{
    DrawSliderBarSteps(x, y, w, h, permille, fill, 0);
}

static void DrawSliderBarSteps(int x, int y, int w, int h, int permille,
                               uint16_t fill, int steps)
{
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;

    /* One centre row, and an odd height either side of it.
     *
     * The groove was four rows and the disc eleven, so there was no row
     * they could share: the disc's centre landed half a pixel below the
     * middle of the track and the handle sat visibly low on it. Three
     * and thirteen both have a middle, and it is the same one.
     *
     * Three rows of groove under a fifteen-pixel handle: the track is
     * the thing being pointed at and the handle is the thing being held,
     * so the handle is what gets the size. */
    const int cy = y + h / 2;                          /* the centre row */
    const int ty = cy - SLIDER_TRACK_H / 2;            /* groove */

    /* Taller than the band it nominally occupies, so the erase reaches
     * past it vertically or a dragged knob leaves a trail. Not
     * horizontally: the centre is inset by exactly one radius at each
     * end, so the circle stops flush with the track and never needs the
     * space beyond it. Erasing there anyway took two columns out of the
     * face of the volume + button, which only repaints when its own
     * state changes and so kept the notch. */
    Gfx_Fill(x, y - 3, w, h + 8, C_BG);

    Gfx_Fill(x, ty, w, SLIDER_TRACK_H, C_TROUGH);
    CapEnds(x, ty, w, SLIDER_TRACK_H, C_SURFACE);

    int kx = SliderKnobX(x, w, permille);
    int filled = kx - x;
    if (filled > 0 && steps > 0) {
        /* Blocks rather than a bar: an LED meter reads as a count, and
         * the count is the point of the look. */
        int cell = w / steps;
        if (cell < 2) cell = 2;

        int bx = x;
        for (; bx + cell - 1 <= x + filled; bx += cell)
            Gfx_Fill(bx, ty, cell - 1, SLIDER_TRACK_H, fill);

        /* And the part of a block at the end.
         *
         * Without it a block only appeared once a whole cell was filled,
         * and a cell here is twelve pixels of a hundred and eighty - so
         * on the one theme that draws this bar in blocks, turning the
         * volume up a step moved the handle and changed nothing else.
         * Two steps did nothing, then the third lit a whole block. The
         * bar was telling you about its own cell size rather than about
         * the volume. */
        int rest = (x + filled) - bx;
        if (rest > cell - 1) rest = cell - 1;
        if (rest > 0) Gfx_Fill(bx, ty, rest, SLIDER_TRACK_H, fill);
    } else if (filled > 0) {
        Gfx_Fill(x, ty, filled, SLIDER_TRACK_H, fill);
        Gfx_Pixel(x, ty, C_SURFACE);
        Gfx_Pixel(x, ty + SLIDER_TRACK_H - 1, C_SURFACE);
    }

    /* Three discs: a shadow, a rim, and the face.
     *
     * The shadow sits two rows lower and shows as a crescent under the
     * handle, which is what lifts it off the track rather than letting
     * it read as a hole cut in one. It is offset only downward - a
     * sideways offset would put ink past the end of the track, and the
     * erase deliberately stops there.
     *
     * The rim is one pixel of a tone halfway between the face and the
     * shadow. At fifteen pixels a hard edge still shows its stair-steps;
     * a middle tone along them is the nearest thing to antialiasing
     * available without reading back what is already on the screen, and
     * it is what makes the circle look round rather than notched. */
    /* The handle takes the accent, which is what every player that is
     * not this one does: the filled part behind it is the same colour,
     * so the handle reads as the leading edge of the fill rather than a
     * separate object parked on top of it, and the only edge that has to
     * carry is the one facing the empty track.
     *
     * That edge is why the rim is dark rather than light - against the
     * trough a light rim would halo, and against the fill it would cut
     * the handle off from what it belongs to. */
    /* A rounded square, not a circle.
     *
     * A circle of this size has four rows near each pole where the width
     * has to change by an odd amount, and whichever way that is rounded
     * one of them comes out a pixel short - which is the notch that was
     * visible on the handle and on the mode buttons. A rounded square
     * has straight sides and four small identical arcs, so there is
     * nothing to get wrong and nothing to look wrong. */
    const int ks = SLIDER_KNOB_R * 2 + 1;
    const int kxo = kx - SLIDER_KNOB_R, kyo = cy - SLIDER_KNOB_R;

    const int kr = TR.handleR;
    FillRounded(kxo, kyo + 1, ks, ks, kr, C_SHADOW);
    FillRounded(kxo, kyo, ks, ks, kr, Gfx_Lerp(fill, C_SHADOW, 1, 2));
    FillRoundedLit(kxo + 1, kyo + 1, ks - 2, ks - 2, kr > 2 ? kr - 1 : 2,
                   Gfx_Lerp(fill, GFX_RGB8(255, 255, 255), 2, 5), fill);
}

void Skin_DrawTransport(bool isPlaying, bool hasTrack, int volumePermille,
                        uint32_t elapsedMs, uint32_t totalMs, bool durationExact)
{
    bool reset = s_transportReset;
    s_transportReset = false;

    for (int i = SKIN_BTN_PREV; i <= SKIN_BTN_VOL_UP; i++) {
        SkinButton b = (SkinButton)i;
        uint32_t sig = (IsPressed(b) ? 1u : 0u) | (hasTrack ? 2u : 0u);
        if (b == SKIN_BTN_PLAYPAUSE && isPlaying) sig |= 4u;

        if (!reset && sig == s_btnSig[i]) continue;
        s_btnSig[i] = sig;

        DrawTransportButton(b, isPlaying, hasTrack);
    }

    /* Keyed on the raw setting, not the permille it maps to: the divide
     * is only worth doing on a frame that is going to repaint. */
    if (reset || (uint32_t)volumePermille != s_volSig) {
        s_volSig = (uint32_t)volumePermille;
        int vx, vy, vw;
        VolumeGeom(&vx, &vy, &vw);
        DrawSliderBarSteps(vx, vy, vw, VOL_TRACK_H, volumePermille,
                           Gfx_Lerp(C_ACCENT, C_TEXT, 2, 5), TR.volSteps);
    }

    /* The progress bar and the clock both move once a second and no
     * faster - the bar is a few hundred pixels wide across a track of
     * minutes - so keying them on whole seconds costs nothing visible and
     * turns three of every four ticks into no work at all, including the
     * 64-bit divide behind the bar. */
    int gx, gy, gw;
    ProgressGeom(&gx, &gy, &gw);

    uint32_t elapsedSec = elapsedMs / 1000;
    uint32_t timeSig = elapsedSec * 2u + (durationExact ? 1u : 0u);

    if (reset || timeSig != s_progSig || totalMs != s_clockSig) {
        int permille = (totalMs > 0)
                     ? (int)(((uint64_t)elapsedMs * 1000) / totalMs) : 0;
        DrawSliderBar(gx, gy, gw, PROGRESS_H, permille);

        char elapsed[16], total[16];
        FormatTime(elapsed, sizeof(elapsed), elapsedMs);

        const int digitW = 6;
        const uint16_t ground = C_BG;

        /* Only the two clock plates, not the strip between them - the bar
         * lives there now and is drawn before this runs, so a full-width
         * erase painted over it and left the row looking like two times
         * with nothing in between.
         *
         * The erase is still needed: without the full-bar repaint nothing
         * else covers the old digits, and "10:00" following "9:59" would
         * leave its tail behind. */
        /* Tucked against the ends of the track rather than pinned to the
         * screen edges. Elapsed is four characters and the remaining is
         * five, so fixed margins left fourteen pixels of air on one side
         * and eight on the other for no reason anyone could see. */
        /* Above the bar and flush with its ends, not beside it.
         *
         * Alongside, the two clocks take the width the bar wants and the
         * row has to be read left to right to find where the track even
         * begins. Above, the bar runs the full content width and the
         * numbers label its ends. */
        const int labelY = gy - 11;
        Gfx_Fill(0, labelY, GFX_W, GFX_GLYPH_H, ground);

        Gfx_TextMono(gx, labelY, digitW, C_TEXT_DIM, C_SHADOW, elapsed);

        /* Minimal and Soft count down instead of stating the length -
         * which is the number anyone reading a clock mid-track actually
         * wants, and what the players this borrows from show. */
        bool remaining = (true);

        if (totalMs > 0) {
            /* A "~" in front of the total is the honest way to show a
             * runtime derived from the first frame's bitrate on a VBR file
             * with no Xing header - it can be minutes out, and silently
             * rounding it off would make the progress bar look broken
             * instead. */
            char label[20];
            uint32_t shown = remaining
                           ? (totalMs > elapsedMs ? totalMs - elapsedMs : 0)
                           : totalMs;
            FormatTime(total, sizeof(total), shown);
            snprintf(label, sizeof(label), "%s%s%s",
                     durationExact ? "" : "~", remaining ? "-" : "", total);

            int lw = Gfx_TextMonoWidth(digitW, label);
            Gfx_TextMono(gx + gw - lw, labelY, digitW,
                         C_TEXT_DIM, C_SHADOW, label);
        }

        s_progSig = timeSig;
        s_clockSig = totalMs;
    }
}

/* ------------------------------------------------------------------ */
/* hit testing                                                        */
/* ------------------------------------------------------------------ */

/* True if a control tagged `page` can be on screen while `tab` shows. */
static bool PageShows(int page, SkinTab tab)
{
    if (page == PAGE_ANY) return true;
    if (page == PAGE_SETTINGS)
        return tab == SKIN_TAB_SETUP || tab == SKIN_TAB_TEST;
    return page == (int)tab;
}

SkinButton Skin_HitTestBrowserButton(int px, int py, const FileBrowser *fb)
{
    SkinButton ids[BROWSER_BTN_MAX];
    SkinRect   rects[BROWSER_BTN_MAX];
    const int  n = BrowserButtons(fb, ids, rects);

    for (int i = 0; i < n; i++)
        if (PointInRect(px, py, &rects[i])) return ids[i];

    return SKIN_BTN_NONE;
}

SkinButton Skin_HitTestButton(int px, int py, SkinTab activeTab)
{
    /* The strip moves with the selection, so its rectangles come from
     * the same function that drew them rather than from the table. */
    SkinRect tabs[TAB_COUNT];
    TabLayout(activeTab, tabs);

    for (int i = 0; i < SKIN_BTN_COUNT; i++) {
        if (!PageShows(UNPG(kButtonPage[i]), activeTab))
            continue;

        if (i >= SKIN_BTN_TAB_FILES && i < SKIN_BTN_TAB_FILES + TAB_COUNT) {
            if (PointInRect(px, py, &tabs[i - SKIN_BTN_TAB_FILES]))
                return (SkinButton)i;
            continue;
        }

        if (PointInRect(px, py, &kRects[i]))
            return (SkinButton)i;
    }
    return SKIN_BTN_NONE;
}

bool Skin_HitTestChoice(SkinButton which, int px, int py, int count, int *outIndex)
{
    if (which <= SKIN_BTN_NONE || which >= SKIN_BTN_COUNT || count <= 0) return false;

    const SkinRect *r = &kRects[which];
    if (!PointInRect(px, py, r)) return false;

    int rel = px - r->x;
    if (rel < 0) rel = 0;
    if (rel >= r->w) rel = r->w - 1;

    *outIndex = (rel * count) / r->w;
    if (*outIndex >= count) *outIndex = count - 1;
    return true;
}

int Skin_HitTestFileRow(int px, int py)
{
    if (px < LIST_X || px >= LIST_X + LIST_W) return -1;
    if (py < LIST_Y + 1 || py >= LIST_Y + 1 + FileBrowser_VisibleRows() * ROW_H) return -1;

    return (py - LIST_Y - 1) / ROW_H;
}

/* The meters are deliberately given a few pixels of slack vertically:
 * an 8px tall target is below what a fingertip can reliably hit on a
 * 3-inch screen, and being generous costs nothing here because nothing
 * else is nearby. */
#define TOUCH_SLACK 5

bool Skin_HitTestVolume(int px, int py, int *outPermille)
{
    int x, y, w;
    VolumeGeom(&x, &y, &w);
    if (px < x - 2 || px > x + w + 2) return false;
    if (py < y - TOUCH_SLACK || py > y + VOL_TRACK_H + TOUCH_SLACK) return false;

    *outPermille = SliderPermilleAt(x, w, px);
    return true;
}

/* Which preset chip is under the pen, as minutes, or false.
 *
 * The whole chip is the target. The slider this replaced asked for one of
 * a hundred and twenty positions along two hundred pixels, where missing
 * by three pixels meant missing by two minutes. */
/* The next preset along from `minutes`, in the direction `dir`.
 *
 * Stepping the chips is what left and right should do now that the chips
 * are what is on screen - the d-pad ought to move through the thing the
 * page is showing rather than through a range it no longer draws. A value
 * trimmed to somewhere in between lands on the neighbour it is heading
 * towards, so one press always reaches a lit chip. */
int Skin_SleepPresetStep(int minutes, int dir)
{
    if (dir > 0) {
        for (int i = 0; i < SLEEP_PRESETS; i++)
            if ((int)kSleepPresets[i] > minutes) return kSleepPresets[i];
        return kSleepPresets[SLEEP_PRESETS - 1];
    }
    for (int i = SLEEP_PRESETS - 1; i >= 0; i--)
        if ((int)kSleepPresets[i] < minutes) return kSleepPresets[i];
    return kSleepPresets[0];
}

/* The duration bar, as minutes. Only meaningful while nothing is armed -
 * the caller decides that, because dragging a running countdown would be
 * setting the number the clock is already deciding. */
bool Skin_HitTestSleepBar(int px, int py, int *outMinutes)
{
    if (px < SLEEP_BAR_X - 6 || px > SLEEP_BAR_X + SLEEP_BAR_W + 6) return false;
    if (py < SLEEP_BAR_Y - 10 || py > SLEEP_BAR_Y + SLEEP_BAR_H + 10) return false;

    int permille = SliderPermilleAt(SLEEP_BAR_X, SLEEP_BAR_W, px);
    int span = PLAYER_SLEEP_MAX_MINUTES - PLAYER_SLEEP_MIN_MINUTES;

    *outMinutes = PLAYER_SLEEP_MIN_MINUTES + (permille * span + 500) / 1000;
    return true;
}

bool Skin_HitTestSleepPreset(int px, int py, int *outMinutes)
{
    if (py < SLEEP_CHIP_Y || py >= SLEEP_CHIP_Y + SLEEP_CHIP_H) return false;

    for (int i = 0; i < SLEEP_PRESETS; i++) {
        int x = SLEEP_CHIP_X(i);
        if (px >= x && px < x + SLEEP_CHIP_W) {
            *outMinutes = kSleepPresets[i];
            return true;
        }
    }
    return false;
}

/* Once the stylus has hold of the bar it keeps it, however far up or
 * down the hand wanders - which is what "grab and drag" means and what
 * a hit test repeated every frame cannot express. The caller latches on
 * the press and asks this for the position after that. */
int Skin_ProgressPermilleAt(int px)
{
    int x, y, w;
    ProgressGeom(&x, &y, &w);
    return SliderPermilleAt(x, w, px);
}

bool Skin_HitTestProgress(int px, int py, int *outPermille)
{
    int x, y, w;
    ProgressGeom(&x, &y, &w);
    if (px < x - 2 || px > x + w + 2) return false;
    if (py < y - TOUCH_SLACK || py > y + PROGRESS_H + TOUCH_SLACK) return false;

    *outPermille = SliderPermilleAt(x, w, px);
    return true;
}
