#ifndef DSXP_VISUALIZER_H
#define DSXP_VISUALIZER_H

#include <stdint.h>

/* Top-screen level meter: 32 amplitude bars on a lit floor line, with a
 * dimmed reflection below it and peak markers that hang and fall.
 *
 * There used to be a waveform and a plasma effect to cycle through as
 * well. They were dropped rather than kept switched off: the plasma alone
 * held a 48KB radial-distance table permanently resident, every pixel of
 * it changed every frame so nothing could be skipped, and the CPU it took
 * came out of the same budget as MP3 decoding. One effect that is always
 * cheap is worth more here than three that are not.
 *
 * Sets up the top screen as a double-buffered 16-bit bitmap across VRAM
 * banks A and B. */
void Visualizer_Init(void);

/* Draws into the back buffer, repainting only what changed since this
 * buffer was last shown. Pass count = 0 when nothing is playing and the
 * bars fall away rather than freezing on stale audio. */
/* How many columns the meter has. Public because the caller sizes its
 * analysis window to a whole number of slices, and a mismatch would leave
 * a ragged last bar with no way to notice. */
#define VIS_NUM_BARS 32

void Visualizer_Render(const int16_t *interleavedFrames, int frameCount);

/* Forces the next render to repaint in full. Needed after anything else
 * has drawn over the top screen. */
void Visualizer_Invalidate(void);

/* Which buffer is currently being drawn into. The overlay needs it to
 * track what each buffer already shows. */
/* Drawing styles. They all share the same analysis - the per-column
 * levels are computed once whichever is selected - so they differ only
 * in how those levels are painted. Cost varies with how many rows each
 * one actually touches rather than with the work behind it: Dots paints
 * a tip and nothing else, and is the cheapest thing here. */
enum {
    VIS_BARS = 0,
    VIS_BLOCKS,      /* the same columns, broken into lit segments */
    VIS_MIRROR,      /* growing out from a centre line, both ways */
    VIS_WAVE,        /* the waveform itself, not its envelope */
    VIS_PILLARS,     /* wide columns with a gap, no cap or reflection */
    VIS_DOTS,        /* the tip alone, floating where the column would end */
    VIS_COMB,        /* one row lit, one dark - a finer Blocks */
    VIS_HANG,        /* the spectrum grown downward from the ceiling */
    VIS_STEPS,       /* two rows lit, two dark - a ladder */
    VIS_TRAIL,       /* the tip, and two dashes falling away below it */
    VIS_EDGE,        /* the column as an outline rather than a solid */
    VIS_DROP,        /* Dots, hanging from the ceiling */
    VIS_STYLES
};

/* What the top screen shows, as one number.
 *
 * It used to be two settings: a mode - meter, card, or off - and, apart
 * from it, which meter. Two rows on the Setup page for one question,
 * and the row that named the mode was the only way to reach the card, of
 * which there was exactly one.
 *
 * One list now, and one picker showing all of it: the meters, then the
 * cards. A meter's choice number is its style number, so the
 * twelve that already existed keep the values they had and a saved
 * config still selects the same meter.
 *
 * The cards are laid out in skin.c, which is where every other card-like
 * thing on either screen is drawn; the meters are the visualizer's. The
 * split is the same as it always was - only the numbering is shared. */
#define TOP_CARDS    4
#define TOP_CARD(n)  (VIS_STYLES + (n))
#define TOP_IS_CARD(c) ((c) >= VIS_STYLES && (c) < VIS_STYLES + TOP_CARDS)
#define TOP_IS_METER(c) ((c) >= 0 && (c) < VIS_STYLES)
#define TOP_CHOICES  (VIS_STYLES + TOP_CARDS)

/* The name of any of them, meter or card, for the picker and for the
 * Setup row that opens it. */
const char *Top_ChoiceName(int choice);

void Visualizer_SetStyle(int style);
int  Visualizer_Style(void);
const char *Visualizer_StyleName(int i);

int Visualizer_BackBuffer(void);

/* Retargets the drawing layer at the top screen's back buffer, for
 * anything that wants to paint the top screen instead of the bars. */
void Visualizer_TargetBackBuffer(void);

/* Flips the buffers. Call immediately after the vblank wait. */
void Visualizer_Present(void);

#endif /* DSXP_VISUALIZER_H */
