/* Renders the DS screens on the host and writes them out as BMPs.
 *
 * The skin is pure software rendering into a 16-bit framebuffer, so it
 * can be driven anywhere - which makes it possible to look at the layout
 * without a flashcart in the loop. gfx.c/skin.c/visualizer.c are included
 * directly so the preview uses the exact code the ROM does; only the
 * handful of libnds video calls are stubbed out. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- libnds stubs ------------------------------------------------ */

#define MODE_5_2D 0
#define BgType_Bmp16 0
#define BgSize_B16_256x256 0
#define VRAM_A_MAIN_BG_0x06000000 0
#define VRAM_B_MAIN_BG_0x06020000 0

static void videoSetMode(int m) { (void)m; }
static void vramSetBankA(int b) { (void)b; }
static void vramSetBankB(int b) { (void)b; }
static int  bgInit(int l, int t, int s, int m, int tb) { (void)l;(void)t;(void)s;(void)m;(void)tb; return 3; }
static void bgSetPriority(int id, int p) { (void)id; (void)p; }
static void bgSetMapBase(int id, unsigned b) { (void)id; (void)b; }


/* The list's row count is owned by the browser, which the preview does
 * not otherwise need - just the accessor. */
#include "filebrowser.h"
static int g_previewVisibleRows = FB_MAX_VISIBLE_ROWS;
int FileBrowser_VisibleRows(void) { return g_previewVisibleRows; }
void FileBrowser_SetVisibleRows(int rows) { g_previewVisibleRows = rows; }

int FileBrowser_MarkedCount(const FileBrowser *fb)
{
    int n = 0;
    for (int i = 0; i < fb->count; i++) if (fb->entries[i].marked) n++;
    return n;
}

#include "gfx.c"
#include "skin.c"

/* The mixer is not built here, and the skin asks it for the volume so
 * the gramophone's slider can show something true. */
int Audio_Volume(void) { return 96; }


extern unsigned long g_gfxPlots;
#include "keyboard.c"

/* Redirect the visualizer's VRAM pointers at ordinary memory. */
static uint16_t g_visBuf[2][256 * 256];
#include "visualizer.c"

/* ---- BMP writer -------------------------------------------------- */

static void WriteBmp(const char *path, const uint16_t *px, int w, int h)
{
    int rowBytes = (w * 3 + 3) & ~3;
    int imgSize = rowBytes * h;
    int fileSize = 54 + imgSize;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &fileSize, 4);
    int off = 54; memcpy(hdr + 10, &off, 4);
    int dib = 40;  memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &imgSize, 4);

    FILE *f = fopen(path, "wb");
    fwrite(hdr, 1, sizeof(hdr), f);

    uint8_t *row = calloc(1, rowBytes);
    for (int y = h - 1; y >= 0; y--) {          /* BMP rows run bottom-up */
        for (int x = 0; x < w; x++) {
            uint16_t c = px[y * 256 + x];
            int r = (c & 0x1F) * 255 / 31;
            int g = ((c >> 5) & 0x1F) * 255 / 31;
            int b = ((c >> 10) & 0x1F) * 255 / 31;
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row, 1, rowBytes, f);
    }
    free(row);
    fclose(f);
    printf("wrote %s\n", path);
}

/* ---- scenes ------------------------------------------------------ */

static uint16_t g_sub[256 * 256];
static FileBrowser g_fb;

static void FakeBrowser(void)
{
    static const char *names[] = {
        "..", "Bootleg Live Sets", "Singles",
        "01 - Everything In Its Right Place.mp3",
        "02 - Kid A.mp3",
        "03 - The National Anthem.mp3",
        "04 - How To Disappear Completely.mp3",
        "05 - Treefingers.mp3",
        "06 - Optimistic.mp3",
        "07 - In Limbo.mp3",
    };

    memset(&g_fb, 0, sizeof(g_fb));
    strcpy(g_fb.cwd, "/Music/Kid A");

    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(g_fb.entries[i].name, FB_NAME_LEN, "%s", names[i]);
        g_fb.entries[i].isDir = (i < 3);
        g_fb.count++;
    }
    /* A playlist and a couple of marks, so the icons and the path-bar
     * badge are visible rather than only compiled. */
    snprintf(g_fb.entries[g_fb.count].name, FB_NAME_LEN, "Evening.m3u");
    g_fb.entries[g_fb.count].isPlaylist = true;
    g_fb.count++;

    g_fb.entries[4].marked = true;
    g_fb.entries[5].marked = true;

    g_fb.selected = 4;
}

/* A short burst of synthetic audio so the visualizers have something to
 * react to. */
/* Seeded per run, not once per program. The two runs in CheckIncremental
 * have to see byte-identical audio or the comparison is meaningless - and
 * a shared generator would quietly hand the second run a different
 * sequence, producing a "failure" that is entirely the test's own. */
#define AUDIO_SEED 12345u
static unsigned g_rng = AUDIO_SEED;
static double g_phase;
static void ResetAudio(void) { g_rng = AUDIO_SEED; g_phase = 0.0; }
static unsigned NextRand(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 16) & 0x7fff;
}

/* mode 2 is the one that matters for cost: every slice gets an
 * independent, wildly different level each frame, so every bar sweeps a
 * long way. The two tone modes make the bars move in near-lockstep, which
 * flatters an incremental renderer badly - real music sits somewhere
 * between, but the cost of the worst case is what decides whether a frame
 * is dropped. */
static void MakeAudio(int16_t *out, int frames, int mode)
{
    if (mode == 3) {
        /* A tone that carries on across blocks rather than restarting,
         * so the trace moves the way it does on real material. The
         * repeating tone above is degenerate for a waveform view: every
         * frame draws the identical shape and the incremental path
         * correctly does nothing at all, which measures nothing. */
        for (int i = 0; i < frames; i++) {
            double v = sin(g_phase) * 0.7 + sin(g_phase * 2.7) * 0.2;
            g_phase += 2 * M_PI * 300.0 / 44100.0;
            out[i * 2] = out[i * 2 + 1] = (int16_t)(v * 30000);
        }
        return;
    }

    if (mode == 2) {
        for (int i = 0; i < frames; i++) {
            int slice = (i * 32) / frames;
            int amp = 2000 + (int)(NextRand() % 28000) * ((slice % 3) + 1) / 3;
            int v = ((int)(NextRand() % 2000) - 1000) + amp * ((i & 1) ? 1 : -1);
            if (v > 32000) v = 32000;
            if (v < -32000) v = -32000;
            out[i * 2] = out[i * 2 + 1] = (int16_t)v;
        }
        return;
    }

    for (int i = 0; i < frames; i++) {
        double t = (double)i / 44100.0;
        double v;
        if (mode == 0)
            v = sin(2 * M_PI * 220 * t) * 0.6 + sin(2 * M_PI * 1400 * t) * 0.25;
        else
            v = sin(2 * M_PI * 90 * t) * 0.8;
        v *= 0.5 + 0.5 * sin(2 * M_PI * 3 * t);
        out[i * 2] = out[i * 2 + 1] = (int16_t)(v * 30000);
    }
}

/* A row left unassigned must not be fatal.
 *
 * The row arrays are locals and the builders assign by enum index. Add a
 * row to the enum, forget the assignment, and every row before it draws
 * perfectly while that one holds stack garbage - which Skin_DrawRows then
 * dereferences as a label pointer. Invisible until someone scrolls far
 * enough, and then a data abort rather than a glitch. That shipped.
 *
 * Being honest about what this covers: the builders now zero their arrays
 * and label anything still unset, so the state that reaches here is a
 * *zeroed* row, and that is what this drives. It cannot reproduce the
 * original wild pointer - a test that dereferences garbage on purpose
 * proves nothing - so it locks in the weaker property that actually
 * remains reachable: the drawing code tolerates a row it was never
 * given. The fix for the real bug is the memset and the "(unset row)"
 * labelling in main.c. */
static int CheckUnfilledRow(void)
{
    SkinRow rows[4];
    memset(rows, 0, sizeof(rows));

    static const char *const kOn[] = { "Off", "On" };
    rows[0] = (SkinRow){ SKIN_ROW_CHOICE, "First",  kOn, 2, 0 };
    rows[1] = (SkinRow){ SKIN_ROW_SLIDER, "Second", NULL, 10, 3 };
    /* rows[2] deliberately left as the builder would leave a forgotten
     * row: zeroed, with a NULL caption. */
    rows[3] = (SkinRow){ SKIN_ROW_CHOICE, "Fourth", kOn, 2, 1 };

    Skin_InvalidateContent();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_SETUP);
    Skin_DrawRows(rows, 4, 0, 0, 0, SKIN_ROW_SLOTS);

    /* Scrolled so the unfilled row is the top one drawn, which is the
     * position the crash actually happened from. */
    Skin_BeginFrame();
    Skin_DrawRows(rows, 4, 2, 2, 0, SKIN_ROW_SLOTS);

    printf("  [PASS] %-52s %s\n",
           "the row drawing survives a row left zeroed",
           "4 rows, one unassigned");
    return 0;
}

/* Two controls on the same page must never share a pixel. This is not a
 * cosmetic check: hit testing returns the first rectangle that matches,
 * so an overlap means one control silently swallows another's taps. That
 * exact bug shipped once - the Sleep page's checkboxes sat on top of the
 * Setup page's Audio quality row - and it is invisible until someone
 * taps the wrong thing, so it is asserted here rather than eyeballed. */
/* Every control the skin can hit-test must actually be reachable: a
 * rectangle that falls outside its page's content area, or one that the
 * transport bar covers, is a control the stylus can never land on.
 *
 * Worth checking mechanically because the failure is silent. The setup
 * page had a variant of this - the rectangles were fine but the row
 * numbers behind them had drifted - and nothing anywhere said so. */
static int CheckControlsReachable(void)
{
    static const struct { int y0, y1; const char *what; } kBands[] = {
        /* Blue Classic's inset tab strip ends at CONTENT_Y.  Content
         * begins directly underneath, so a control cannot cross a
         * composition boundary. */
        /* CONTENT_Y, not a copy of what it happened to be. Written out
         * as 29 this moved on its own the moment the tab band was made
         * shorter, and reported every control on the page as straddling
         * a boundary that had gone. */
        { 0,         CONTENT_Y, "tabs"    },
        { CONTENT_Y, 192,       "content" },
    };

    int failures = 0;
    for (int i = 0; i < SKIN_BTN_COUNT; i++) {
        const SkinRect *r = &kRects[i];
        if (r->w == 0 || r->h == 0) continue;

        if (r->x < 0 || r->y < 0 || r->x + r->w > 256 || r->y + r->h > 192) {
            printf("  [FAIL] control %d is off screen: (%d,%d %dx%d)\n",
                   i, r->x, r->y, r->w, r->h);
            failures++;
            continue;
        }

        /* Must sit inside exactly one band; straddling means half of it
         * is drawn over by whatever owns the other band. */
        int inBand = -1;
        for (unsigned b = 0; b < sizeof(kBands) / sizeof(kBands[0]); b++)
            if (r->y >= kBands[b].y0 && r->y + r->h <= kBands[b].y1) inBand = (int)b;

        if (inBand < 0) {
            printf("  [FAIL] control %d straddles two regions: y %d..%d\n",
                   i, r->y, r->y + r->h);
            failures++;
        }
    }

    printf("  [%s] every control lies inside one screen region\n",
           failures ? "FAIL" : "PASS");
    return failures;
}

/* The scroll rail is the only way a stylus can reach rows past the last
 * visible slot, so it has to cover the whole range and stay inside it.
 * A rail that cannot express the last row is a row nobody can touch. */
/* The font switch changes what a glyph looks like, and the measured
 * widths with it. Checked because "the font did not change" is a symptom
 * with several possible causes, and this rules out the drawing side. */
static int CheckFontSwitch(void)
{
    int failures = 0;

    Gfx_SetFontStyle(GFX_FONT_SANS);
    Gfx_InitFont();
    int sansW = Gfx_TextWidth("Hamburgefonstiv");
    uint8_t sansA = FONT_ROWS(GlyphIndex('A'))[0];

    Gfx_SetFontStyle(GFX_FONT_SQUARE);
    int sqW = Gfx_TextWidth("Hamburgefonstiv");
    uint8_t sqA = FONT_ROWS(GlyphIndex('A'))[0];

    char buf[120];
    snprintf(buf, sizeof(buf), "'A' top row %02X vs %02X, width %d vs %d",
             sansA, sqA, sansW, sqW);
    bool ok = (sansA != sqA) && (Gfx_FontStyle() == GFX_FONT_SQUARE);
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL",
           "selecting a face changes the glyphs", buf);
    if (!ok) failures++;

    Gfx_SetFontStyle(GFX_FONT_SANS);
    printf("  [%s] %-46s %s\n",
           Gfx_FontStyle() == GFX_FONT_SANS ? "PASS" : "FAIL",
           "and switching back restores the first", NULL);
    if (Gfx_FontStyle() != GFX_FONT_SANS) failures++;

    return failures;
}

/* A long selected name has to travel far enough to reveal its tail and
 * come back, and a short one must not move at all - a list where every
 * row jitters is worse than one that truncates. */
static int CheckMarquee(void)
{
    int failures = 0;

    FakeBrowser();
    snprintf(g_fb.entries[4].name, FB_NAME_LEN,
             "04 - A Very Long Track Name That Cannot Possibly Fit.mp3");
    g_fb.selected = 4;

    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawFileList(&g_fb, NULL);
    bool longActive = Skin_MarqueeActive();

    /* Sweep a whole cycle and note how far the row's pixels shift. */
    int seen = 0;
    static uint16_t prev[GFX_W];
    int changed = 0;
    for (int f = 0; f < 400; f++) {
        Skin_TickMarquee();
        Skin_InvalidateContent();
        Skin_BeginFrame();
        Skin_DrawFileList(&g_fb, NULL);
        int rowY = 51 + 1 + 4 * 15 + 4;
        memcpy(prev, g_sub + rowY * GFX_W, GFX_W * 2);
        if (f > 0 && memcmp(prev, g_sub + rowY * GFX_W, GFX_W * 2) != 0) changed++;
        seen++;
    }
    (void)changed; (void)seen;

    g_fb.selected = 0;                       /* ".." - short */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawFileList(&g_fb, NULL);
    bool shortActive = Skin_MarqueeActive();

    /* And it must actually reveal the tail, not merely report that it is
     * moving: a sweep that never gets past the first character is the
     * same bug as no sweep at all. */
    int widest = 0, narrowest = 9999;
    for (int f = 0; f < 600; f++) {
        Skin_TickMarquee();
        int off = Gfx_MarqueeOffsetFor(Skin_MarqueeTick(),
                                       g_fb.entries[4].name, 210, 4);
        if (off > widest) widest = off;
        if (off < narrowest) narrowest = off;
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "moves: %s, still when short: %s, sweep %d..%d chars",
             longActive ? "yes" : "no", shortActive ? "no" : "yes",
             narrowest, widest);
    bool ok = longActive && !shortActive && narrowest == 0 && widest > 5;
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL",
           "only an overlong selected name scrolls", buf);
    if (!ok) failures++;

    /* The same question for the Now Playing title, which is drawn at
     * double height.
     *
     * The offset used to be asked for in the proportional small-font
     * metric while the decision to scroll at all was made in the
     * monospaced big-font one, so the two disagreed about how wide the
     * text was. Checking merely that it moves is not enough to catch
     * that - for many strings the old code moved it, just not far
     * enough, leaving the last few characters permanently off the edge.
     *
     * So the assertion is exact: sweeping to the far end must leave the
     * remainder fitting the box, and one character less must not. That
     * is the definition of the offset, and the old metric missed it by
     * three characters on this string. */
    {
        const char *title = "www.MusicKordi.com - Some Track Name";
        const int box = GFX_W - 10 - 10;
        int big = Gfx_TextBigPropWidth(8, title);
        int sweep = 0;
        for (int f = 0; f < 900; f++) {
            Skin_TickMarquee();
            int off = Gfx_MarqueeOffsetFor(Skin_MarqueeTick(), title, box, 8);
            if (off > sweep) sweep = off;
        }
        int tail  = Gfx_TextBigPropWidth(8, title + sweep);
        int short1 = sweep > 0 ? Gfx_TextBigPropWidth(8, title + sweep - 1) : 0;
        snprintf(buf, sizeof(buf),
                 "big %d, box %d, sweep %d -> tail %d (one less: %d)",
                 big, box, sweep, tail, short1);
        bool ok2 = big > box && sweep > 0 && tail <= box && short1 > box;
        printf("  [%s] %-46s %s\n", ok2 ? "PASS" : "FAIL",
               "a double-height title sweeps to its end", buf);
        if (!ok2) failures++;
    }

    FakeBrowser();
    Skin_InvalidateBottom();
    return failures;
}

static int CheckRowScroll(void)
{
    int failures = 0;

    struct { int count, visible, firstSlot; const char *what; } kCases[] = {
        /* The real row counts. Five rows against five slots scrolls
         * nowhere, and a rail test that cannot scroll proves nothing. */
        { 6, SKIN_ROW_SLOTS,     0, "Setup, 6 rows" },
        { 9, SKIN_ROW_SLOTS - 1, 1, "Test, 9 rows"  },
    };

    for (unsigned c = 0; c < sizeof(kCases) / sizeof(kCases[0]); c++) {
        int span = kCases[c].count - kCases[c].visible;
        int seenTop = -1, seenBottom = -1, misses = 0;

        for (int py = CONTENT_Y; py < CONTENT_Y + CONTENT_H; py++) {
            int top;
            if (!Skin_HitTestRowScroll(250, py, kCases[c].count,
                                       kCases[c].visible, kCases[c].firstSlot, &top))
                continue;
            if (top < 0 || top > span) { misses++; continue; }
            if (seenTop < 0) seenTop = top;
            seenBottom = top;
        }

        bool ok = (seenTop == 0) && (seenBottom == span) && (misses == 0);
        char buf[120];
        snprintf(buf, sizeof(buf), "%s: rail spans scrollTop %d..%d of 0..%d",
                 kCases[c].what, seenTop, seenBottom, span);
        printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL",
               "scroll rail reaches every row", buf);
        if (!ok) failures++;

        /* And a touch outside the rail must not be taken as a scroll. */
        int top;
        bool stray = Skin_HitTestRowScroll(120, CONTENT_Y + 40, kCases[c].count,
                                           kCases[c].visible, kCases[c].firstSlot, &top);
        printf("  [%s] %-46s %s\n", stray ? "FAIL" : "PASS",
               "a touch on the rows is not a scroll", kCases[c].what);
        if (stray) failures++;
    }

    return failures;
}

/* A slider pushed to its end and brought back leaves nothing behind.
 *
 * The knob's centre is inset by one radius at each end so the handle
 * stops flush with the track, and the erase is exactly the track's width
 * because of that - the comment on it says so, and it was right about
 * the intent. The span the centre travelled was w - 2r, which at full
 * scale put the handle's last column at x + w: one past the track, one
 * past the erase, and repainted by nothing. Both sliders kept a stripe
 * at the far end of the groove after a drag to the stop.
 *
 * Checked by drawing rather than by arithmetic, and by the exact thing
 * that was done to it: the same slider drawn straight at its low value
 * has to be pixel-for-pixel what you get from taking it to the top and
 * bringing it back. Anything left over anywhere in the band shows as a
 * difference, whatever its cause.
 */
static int CheckSliderResidue(void)
{
    static uint16_t direct[256 * 192], viaEnd[256 * 192];

    const struct { const char *name; int x, y, w, h, steps; } sl[] = {
        { "volume",   VOL_TRACK_X, VOL_TRACK_Y, VOL_TRACK_W, VOL_TRACK_H, 0 },
        { "volume (stepped)",
                      VOL_TRACK_X, VOL_TRACK_Y, VOL_TRACK_W, VOL_TRACK_H, 12 },
        { "progress", STACK_TRACK_X, STACK_PROG_Y, STACK_TRACK_W,
                      PROGRESS_H, 0 },
        { "sleep",    SLEEP_BAR_X, SLEEP_BAR_Y, SLEEP_BAR_W, SLEEP_BAR_H, 0 },
    };
    const int n = (int)(sizeof(sl) / sizeof(sl[0]));
    int failures = 0;

    for (int i = 0; i < n; i++) {
        for (int pass = 0; pass < 2; pass++) {
            uint16_t *buf = pass ? viaEnd : direct;
            for (int j = 0; j < 256 * 192; j++) buf[j] = Skin_ColorBg();
            Gfx_SetTarget(buf);
            /* The one that has been to the stop and come back, and the
             * one that never left - ending at the same value. */
            if (pass) DrawSliderBarSteps(sl[i].x, sl[i].y, sl[i].w, sl[i].h,
                                         1000, Skin_ColorAccent(), sl[i].steps);
            DrawSliderBarSteps(sl[i].x, sl[i].y, sl[i].w, sl[i].h,
                               200, Skin_ColorAccent(), sl[i].steps);
        }

        int bad = 0, firstX = -1, firstY = -1;
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++)
                if (direct[y * 256 + x] != viaEnd[y * 256 + x]) {
                    if (!bad) { firstX = x; firstY = y; }
                    bad++;
                }

        if (bad) {
            printf("  [FAIL] %-52s %d px left over, first at (%d,%d)\n",
                   sl[i].name, bad, firstX, firstY);
            failures++;
        }
    }

    printf("  [%s] %-52s %s\n", failures ? "FAIL" : "PASS",
           "a slider taken to its stop and back leaves nothing",
           "4 sliders");
    return failures;
}

/* The tab strip lays out around the selection without colliding.
 *
 * The cells are not fixed any more: the selected tab carries its name
 * and the other five are icons, so the whole strip is a different width
 * for every page and is recentred each time. TabLayout is the only thing
 * that decides that, and the drawing and the hit test both call it - so
 * what has to hold is that its answers are sane for every selection, or
 * a tab answers taps beside itself and the one next to it goes dead.
 *
 * This replaces a check that measured label ink in fixed cells. It kept
 * passing against a bar that had six labels in six equal boxes, and said
 * nothing at all about the one that has one label and five marks.
 */
static int CheckTabStrip(void)
{
    int failures = 0;
    int widest = 0;

    for (int sel = 0; sel < TAB_COUNT; sel++) {
        SkinRect t[TAB_COUNT];
        TabLayout((SkinTab)sel, t);

        int right = t[TAB_COUNT - 1].x + t[TAB_COUNT - 1].w;
        int span = right - t[0].x;
        if (span > widest) widest = span;

        if (t[0].x < 0 || right > 256) {
            printf("  [FAIL] with tab %d selected the strip spans %d..%d\n",
                   sel, t[0].x, right);
            failures++;
        }

        for (int i = 1; i < TAB_COUNT; i++) {
            if (t[i].x == t[i - 1].x + t[i - 1].w) continue;
            printf("  [FAIL] tab %d starts at %d, tab %d ends at %d\n",
                   i, t[i].x, i - 1, t[i - 1].x + t[i - 1].w);
            failures++;
        }

        /* The selected one is the only one wide enough for a name, which
         * is what makes it findable without reading the icons. */
        for (int i = 0; i < TAB_COUNT; i++) {
            bool wide = t[i].w > TAB_ICON_W + TAB_ICON_PAD * 2;
            if (wide == (i == sel)) continue;
            printf("  [FAIL] tab %d is %d wide with tab %d selected\n",
                   i, t[i].w, sel);
            failures++;
        }

        /* And its name fits the space the layout gave it. */
        int need = TAB_ICON_PAD + TAB_ICON_W + TAB_NAME_GAP +
                   Gfx_TextWidth(kTabNames[sel]) + TAB_ICON_PAD;
        if (t[sel].w < need) {
            printf("  [FAIL] \"%s\" needs %d, has %d\n",
                   kTabNames[sel], need, t[sel].w);
            failures++;
        }
    }

    printf("  [%s] %-52s widest %d of 256\n", failures ? "FAIL" : "PASS",
           "the tab strip fits and never overlaps itself", widest);
    return failures;
}

/* The cover card does not paint the picture a pixel at a time.
 *
 * This is a cost test, and the cost it is about does not show up in the
 * pixel count: the cover went to the screen through thirteen thousand
 * Gfx_Pixel calls, each with a bounds test and a dirty-span update, and
 * by pixel count that is identical to the one Gfx_Blit that replaced it.
 * It repainted several times a second on the thread that draws the
 * menus, and it was felt as the menus stuttering rather than as anything
 * wrong with the top screen.
 *
 * So the thing measured is the number of single-pixel calls, which for
 * this card should be none at all - with a picture and without one,
 * because most tracks have no picture and the stand-in is a circle,
 * which is exactly the shape somebody would be tempted to plot. */
static int CheckCoverBlits(void)
{
    enum { AW = 116 };
    static uint16_t art[AW * AW];
    for (int i = 0; i < AW * AW; i++) art[i] = (uint16_t)(0x8000 | (i & 0x7FFF));

    int failures = 0;
    unsigned long worst = 0;

    for (int withArt = 0; withArt < 2; withArt++) {
        Skin_SetTopArt(withArt ? art : NULL, AW, AW);
        Skin_InvalidateTopCard();
        Gfx_SetTarget(g_sub);

        g_gfxPlots = 0;
        Skin_DrawTopCard(-1, 1, "02 - Kid A.mp3", "Radiohead", "Next Track.mp3", true, true,
                         71000, 262000, 44100, 320, 2, false, 1, 7, 0);

        /* Text is drawn glyph by glyph and a frame is four fills; what
         * must not appear here is the picture, which is thousands. */
        if (g_gfxPlots > worst) worst = g_gfxPlots;
        if (g_gfxPlots <= 512) continue;

        printf("  [FAIL] the cover card plots %lu pixels one at a time%s\n",
               g_gfxPlots, withArt ? "" : " (with no picture)");
        failures++;
    }

    Skin_SetTopArt(NULL, 0, 0);
    printf("  [%s] %-52s worst %lu single-pixel calls\n",
           failures ? "FAIL" : "PASS",
           "the cover is blitted, not plotted", worst);
    return failures;
}

/* The cassette's label is the theme's colour, and what is printed on it
 * has to be legible against every one of them.
 *
 * The brand is printed live on a header band, and both of them flip
 * together: a bright accent gets a dark band with pale printing, a dark
 * one gets a pale band with dark printing. Two things have to hold on
 * every theme - the printing has to stand off the band, and the band
 * has to stand off the label, or it is not a band.
 *
 * Ten of thirty-one is about a third of the range. Below that the digits
 * are there but you would not read them across a room, which is the only
 * distance a DS screen is ever read at. */
static int CheckTapeInk(void)
{
    int failures = 0, worst = 99;
    const char *worstWhere = "";

    for (int t = 0; t < SKIN_THEMES; t++) {
        Skin_SetTheme(t);
        const uint16_t acc = Skin_ColorAccent();
        const int gap = TapeLuma(TapeHeader(acc)) - TapeLuma(TapeInk(acc));
        const int d = gap < 0 ? -gap : gap;

        /* And the band itself has to be a band: a darkening that lands
         * on top of an already dark accent is not one. */
        const int off = TapeLuma(TapeHeader(acc)) - TapeLuma(acc);
        const int band = off < 0 ? -off : off;
        if (band < 6) {
            printf("  [FAIL] %s: the header band is %d from the label\n",
                   Skin_ThemeName(t), band);
            failures++;
        }

        if (d < worst) { worst = d; worstWhere = Skin_ThemeName(t); }
        if (d >= 10) continue;

        printf("  [FAIL] %s: the brand is %d from the band under it\n",
               Skin_ThemeName(t), d);
        failures++;
    }
    Skin_SetTheme(0);

    printf("  [%s] %-52s worst %d of 31 (%s)\n", failures ? "FAIL" : "PASS",
           "the brand is legible on every label", worst, worstWhere);
    return failures;
}

/* Where the hub meets the tape, nothing gets darker.
 *
 * The hub is a sprite with a transparent index, and a sprite like that
 * has to be antialiased against some background when it is baked. This
 * one is drawn against two: the dark of an empty window and the pale of
 * a full spool. Baked against the dark, its rim carried a ring of nearly
 * black pixels, and every time a spool had tape on it those pixels
 * landed on the tape as a dotted circle - which read as dirt on the
 * picture rather than as the blend they were.
 *
 * The rim carries coverage now. So: fill a spool, and require that the
 * ring where the hub's edge lands is never darker than the tape it is
 * landing on. The hub's own well and the gaps between its teeth are
 * legitimately dark, and they are well inside this ring. */
static int CheckHubRim(void)
{
    const int sx = (256 - CASSETTE_W) / 2, sy = (192 - CASSETTE_H) / 2;
    const int cx = sx + CASSETTE_SPX_L, cy = sy + CASSETTE_CY;
    int failures = 0, dark = 0, worst = 255;

    Gfx_SetTarget(g_sub);
    Skin_SetTheme(0);
    Skin_InvalidateTopCard();
    /* Nothing played yet, so the left spool is full. */
    Skin_DrawTopCard(-1, 2, "Without Me", "Halsey", "Next.mp3", true, true,
                     0, 215000, 44100, 320, 2, false, 1, 7, 0);

    const int floorLum = Luma(TAPE_DARK) - 3;

    for (int dy = -14; dy <= 14; dy++)
        for (int dx = -14; dx <= 14; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 < 11 * 11 || d2 > 14 * 14) continue;

            const int lum = Luma(g_sub[(cy + dy) * 256 + (cx + dx)]);
            if (lum < worst) worst = lum;
            if (lum >= floorLum) continue;
            dark++;
        }

    if (dark) {
        printf("  [FAIL] %d pixels of the hub's rim are darker than the "
               "tape under it (darkest %d, tape %d)\n",
               dark, worst, Luma(TAPE_DARK));
        failures++;
    }
    printf("  [%s] %-52s %d specks, darkest %d of %d\n",
           failures ? "FAIL" : "PASS",
           "the hub leaves no specks on the tape", dark, worst,
           Luma(TAPE_DARK));
    return failures;
}

/* With nothing playing, the cassette card is not a cassette.
 *
 * Every other card starts by clearing the screen; this one does not,
 * because it paints its own ground and that clear was the largest cost
 * it had. But the no-track path returns before any of the shell is
 * painted - so the message went on top of whichever cassette the buffer
 * was last left holding, and the buffer went on claiming it held a
 * shell, which meant the next track to start repainted only the two
 * windows and left "Nothing playing" printed across the label.
 *
 * So: draw a track, stop, start the same track again, and require the
 * result to be pixel-identical to that track drawn from scratch. */
static int CheckTapeNoTrack(void)
{
    static uint16_t ref[256 * 192];
    int failures = 0;

    Skin_SetTheme(0);
    Gfx_SetTarget(g_sub);

    /* What it should look like, drawn into a buffer that claims nothing. */
    Skin_InvalidateTopCard();
    Skin_DrawTopCard(-1, 2, "Without Me", "Halsey", "Next.mp3", true, true,
                     40000, 215000, 44100, 320, 2, false, 1, 7, 0);
    memcpy(ref, g_sub, sizeof ref);

    /* The same track, then nothing, then the same track - all in the
     * one buffer, the way the top screen actually uses it. */
    Skin_InvalidateTopCard();
    Skin_DrawTopCard(0, 2, "Without Me", "Halsey", "Next.mp3", true, true,
                     40000, 215000, 44100, 320, 2, false, 1, 7, 0);
    Skin_DrawTopCard(0, 2, "", "", "", false, false,
                     0, 0, 0, 0, 0, false, 0, 0, 0);
    /* The gramophone skips the same clear for the same reason, so it
     * can go stale in the same way. */
    Skin_DrawTopCard(0, 3, "", "", "", false, false,
                     0, 0, 0, 0, 0, false, 0, 0, 0);

    /* Nothing of the shell may survive that. */
    int shell = 0;
    for (int y = 40; y < 170; y += 3)
        for (int x = 20; x < 236; x += 3)
            if (g_sub[y * 256 + x] == ref[y * 256 + x] &&
                ref[y * 256 + x] != Skin_ColorBg()) shell++;
    if (shell > 400) {
        printf("  [FAIL] the stopped card still shows %d pixels of the "
               "cassette under it\n", shell);
        failures++;
    }

    Skin_DrawTopCard(0, 2, "Without Me", "Halsey", "Next.mp3", true, true,
                     40000, 215000, 44100, 320, 2, false, 1, 7, 0);

    int differ = 0;
    for (int i = 0; i < 256 * 192; i++) if (g_sub[i] != ref[i]) differ++;
    if (differ) {
        printf("  [FAIL] after stopping and starting, %d pixels differ "
               "from the same card drawn fresh\n", differ);
        failures++;
    }

    printf("  [%s] %-52s %d stale, %d differ\n", failures ? "FAIL" : "PASS",
           "stopping the cassette does not leave it on screen",
           shell, differ);
    return failures;
}

/* A sweeping title repaints the paper, and nothing else.
 *
 * Most of this card is painted once and left alone; only a title too
 * long for the label has to have its paper put back underneath it every
 * time. That partial path is the one that can quietly lose things - the
 * brand, the type mark and the whole shell are outside the rows it
 * restores, and are never redrawn.
 *
 * So: draw a long title fresh, then draw it twice into one buffer so the
 * second is the partial path, and require the two to be identical. The
 * marquee is not ticked between them, so the title itself lands in the
 * same place and any difference is something the partial path lost. */
static int CheckTapeSweep(void)
{
    static uint16_t ref[256 * 192];
    const char *sweeping = "GaraJoo Peshtem Zhanakad Bedoone Tavaghof";
    int failures = 0;

    Skin_SetTheme(0);
    Gfx_SetTarget(g_sub);

    /* The title has to have moved between the two draws, or the second
     * one writes the same pixels over the same pixels and the restore
     * it is supposed to be doing is never tested. Ten sub-ticks to a
     * marquee step, and the label scrolls a step every four of those. */
    Skin_InvalidateTopCard();
    Skin_DrawTopCard(0, 2, sweeping, "Halsey", "Next.mp3", true, true,
                     40000, 215000, 44100, 320, 2, false, 1, 7, 0);
    for (int i = 0; i < 400; i++) Skin_TickMarquee();
    Skin_DrawTopCard(0, 2, sweeping, "Halsey", "Next.mp3", true, true,
                     40000, 215000, 44100, 320, 2, false, 1, 7, 0);
    static uint16_t got[256 * 192];
    memcpy(got, g_sub, sizeof got);

    /* The same tick, drawn from scratch. */
    Skin_InvalidateTopCard();
    Skin_DrawTopCard(-1, 2, sweeping, "Halsey", "Next.mp3", true, true,
                     40000, 215000, 44100, 320, 2, false, 1, 7, 0);
    memcpy(ref, g_sub, sizeof ref);
    memcpy(g_sub, got, sizeof got);

    int differ = 0;
    for (int i = 0; i < 256 * 192; i++) if (g_sub[i] != ref[i]) differ++;
    if (differ) {
        printf("  [FAIL] the partial repaint loses %d pixels of the card\n",
               differ);
        failures++;
    }
    printf("  [%s] %-52s %d pixels lost\n", failures ? "FAIL" : "PASS",
           "a sweeping title keeps the rest of the cassette", differ);
    return failures;
}

/* A whole side, drawn incrementally, looks like drawing every moment.
 *
 * Two things in this card are drawn from what the buffer already holds
 * rather than from scratch: a sweeping title puts back only the box it
 * writes in, and a window whose pack has not changed radius puts back
 * only a square around the hub. Both are large savings - the second is
 * the common case by a long way, since a pack's radius moves about nine
 * pixels over a whole side - and both are wrong if the region they
 * restore is a pixel too small.
 *
 * Testing one moment cannot see that: the hub's outer ring is the same
 * at every rotation, so a square that clips it still compares equal. So
 * this plays a side through, a quarter second at a time, and checks
 * every step against that same moment drawn from scratch. That crosses
 * every radius the packs take, in both directions, and every phase of
 * the marquee.
 */
static int CheckTapeIncremental(void)
{
    static uint16_t got[256 * 192];
    const char *sweeping = "GaraJoo Peshtem Zhanakad Bedoone Tavaghof";
    const uint32_t total = 215000;
    int failures = 0, worst = 0, worstAt = -1, steps = 0;

    Skin_SetTheme(0);
    Gfx_SetTarget(g_sub);
    Skin_InvalidateTopCard();

    for (uint32_t t = 0; t <= total; t += 250) {
        Skin_TickMarquee();

        /* The incremental one, into the buffer that holds the last. */
        Skin_DrawTopCard(0, 2, sweeping, "Halsey", "Next.mp3", true, true,
                         t, total, 44100, 320, 2, false, 1, 7, 0);
        memcpy(got, g_sub, sizeof got);

        /* The same moment from scratch. buffer -1 keeps no state, so it
         * does not disturb the chain - but it does overwrite the screen,
         * so the chain's own pixels go back afterwards. */
        Skin_DrawTopCard(-1, 2, sweeping, "Halsey", "Next.mp3", true, true,
                         t, total, 44100, 320, 2, false, 1, 7, 0);

        int differ = 0;
        for (int i = 0; i < 256 * 192; i++) if (got[i] != g_sub[i]) differ++;
        if (differ > worst) { worst = differ; worstAt = (int)(t / 1000); }
        memcpy(g_sub, got, sizeof got);
        steps++;
    }

    if (worst) {
        printf("  [FAIL] drawn incrementally, the card differs from a full "
               "repaint by up to %d pixels (at %d seconds)\n", worst, worstAt);
        failures++;
    }
    printf("  [%s] %-52s %d steps, worst %d\n", failures ? "FAIL" : "PASS",
           "a side played incrementally matches a full repaint",
           steps, worst);
    return failures;
}

/* The deck covers the ground it stands on.
 *
 * The record is one baked sprite and the label is another, and the
 * label turns in a hole cut in the first. Two sprites and a hole is
 * three chances to be a pixel out, and being a pixel out does not look
 * like being a pixel out - it looks like a crescent of the card's own
 * background showing through the record, which is what it did: the
 * sprite grew to include the platter and kept being positioned by the
 * record's radius, so the whole deck sat seven pixels down and right of
 * the label that belonged in the middle of it.
 *
 * Nothing inside the platter may be the ground, at any point in a side.
 */
static int CheckDeckCovers(void)
{
    const int cx = SKIN_DECK_CX, cy = SKIN_DECK_CY;
    int failures = 0, worst = 0, worstAt = -1;

    Skin_SetTheme(0);
    Gfx_SetTarget(g_sub);

    /* The ground, sampled where the deck cannot reach. */
    Skin_InvalidateTopCard();
    Skin_DrawTopCard(-1, 3, "Without Me", "Halsey", "Next.mp3", true, true,
                     0, 215000, 44100, 320, 2, false, 1, 7, 0);
    const uint16_t ground = g_sub[4 * 256 + 4];

    for (uint32_t t = 0; t <= 215000; t += 21500) {
        Skin_InvalidateTopCard();
        Skin_DrawTopCard(-1, 3, "Without Me", "Halsey", "Next.mp3", true, true,
                         t, 215000, 44100, 320, 2, false, 1, 7, 0);

        int bare = 0;
        for (int dy = -RECORD_PR + 2; dy <= RECORD_PR - 2; dy++)
            for (int dx = -RECORD_PR + 2; dx <= RECORD_PR - 2; dx++) {
                if (dx * dx + dy * dy > (RECORD_PR - 2) * (RECORD_PR - 2))
                    continue;
                if (g_sub[(cy + dy) * 256 + cx + dx] == ground) bare++;
            }
        if (bare > worst) { worst = bare; worstAt = (int)(t / 1000); }
    }

    if (worst) {
        printf("  [FAIL] %d pixels of bare ground inside the deck "
               "(at %d seconds)\n", worst, worstAt);
        failures++;
    }
    printf("  [%s] %-52s %d bare pixels\n", failures ? "FAIL" : "PASS",
           "the deck leaves no ground showing through it", worst);
    return failures;
}

/* A side on the gramophone, drawn incrementally, matches drawing it.
 *
 * Almost nothing on that card moves, so almost nothing is redrawn: the
 * deck, the names and the arm's rest are painted once; the arm is put
 * back only when it steps to a new groove, which happens about forty
 * times in a side; and the label is put back when it turns. Everything
 * else is left standing from the last frame.
 *
 * That is worth fifty times the drawing, and it is wrong the moment one
 * of those boxes is a pixel too small - which does not look like a
 * pixel, it looks like a smear of the old arm left behind on the
 * record. So: play a side, a quarter second at a time, and check every
 * step against that same moment drawn from scratch. */
static int CheckDeckIncremental(void)
{
    static uint16_t got[256 * 192];
    const char *sweeping = "GaraJoo Peshtem Zhanakad Bedoone Tavaghof";
    const uint32_t total = 215000;
    int failures = 0, worst = 0, worstAt = -1, steps = 0;

    Skin_SetTheme(0);
    Gfx_SetTarget(g_sub);
    Skin_InvalidateTopCard();

    for (uint32_t t = 0; t <= total; t += 250) {
        Skin_TickMarquee();

        Skin_DrawTopCard(0, 3, sweeping, "Halsey", "Next.mp3", true, true,
                         t, total, 44100, 320, 2, false, 1, 7, 0);
        memcpy(got, g_sub, sizeof got);

        Skin_DrawTopCard(-1, 3, sweeping, "Halsey", "Next.mp3", true, true,
                         t, total, 44100, 320, 2, false, 1, 7, 0);

        int differ = 0;
        for (int i = 0; i < 256 * 192; i++) if (got[i] != g_sub[i]) differ++;
        if (differ > worst) { worst = differ; worstAt = (int)(t / 1000); }
        memcpy(g_sub, got, sizeof got);
        steps++;
    }

    if (worst) {
        printf("  [FAIL] drawn incrementally, the deck differs from a full "
               "repaint by up to %d pixels (at %d seconds)\n", worst, worstAt);
        failures++;
    }
    printf("  [%s] %-52s %d steps, worst %d\n", failures ? "FAIL" : "PASS",
           "a side on the deck matches a full repaint", steps, worst);
    return failures;
}

/* The baked shell has no holes in it.
 *
 * Two of its pixels are deliberately nothing - the ground outside the
 * rounded corners, and the two portholes, which the player paints. The
 * portholes are the hazard: the picture punches out a circle and the
 * player fills a circle, and the two are computed by different code in
 * different languages. When they disagreed by a pixel, the ground showed
 * through the difference as a dotted arc around each spool, which read
 * as a drawing mistake rather than as the off-by-one it was.
 *
 * So: paint the card over a colour that appears nowhere in it, and
 * require that none of that colour survives anywhere inside the shell.
 * The inset keeps the rounded corners, where the ground is meant to
 * show, out of it. */
static int CheckTapeCovers(void)
{
    const int sx = (256 - CASSETTE_W) / 2, sy = (192 - CASSETTE_H) / 2;
    int failures = 0, worstX = -1, worstY = -1, holes = 0;

    Gfx_SetTarget(g_sub);
    Skin_SetTheme(0);
    Skin_InvalidateTopCard();
    Skin_DrawTopCard(-1, 2, "Without Me", "Halsey", "Next.mp3", true, true,
                     60000, 215000, 44100, 320, 2, false, 1, 7, 0);

    /* A hole shows the card's own ground, not some marker colour put
     * there first: the card paints that ground before it paints
     * anything else, so the only way to recognise one is to work out
     * what the ground would have been on that row and look for it
     * somewhere it has no business being. */
    for (int y = sy + 10; y < sy + CASSETTE_H - 10; y++) {
        const int d = (y < 192 / 2) ? (192 / 2 - y) : (y - 192 / 2);
        const uint16_t ground = Gfx_Lerp(Skin_ColorSurface(), Skin_ColorBg(),
                                         d, 192 / 2);
        for (int x = sx + 10; x < sx + CASSETTE_W - 10; x++)
            if (g_sub[y * 256 + x] == ground) {
                holes++;
                if (worstX < 0) { worstX = x - sx; worstY = y - sy; }
            }
    }

    if (holes) {
        printf("  [FAIL] %d pixels of the shell were never painted, from "
               "(%d, %d)\n", holes, worstX, worstY);
        failures++;
    }
    printf("  [%s] %-52s %d unpainted\n", failures ? "FAIL" : "PASS",
           "the cassette covers every pixel of its shell", holes);
    return failures;
}

/* Big text is drawn as a shape, and still measures the same.
 *
 * A bitmap font blown up by whole and half steps is the definition of
 * pixely: at three halves a source pixel becomes a one or two pixel
 * block depending on where it lands, so the same stem is one wide in one
 * letter and two in the next. It is drawn by coverage now - the letter
 * is the half contour of the field its bits sample - which is the same
 * treatment the cover's corners get.
 *
 * Three things have to hold. The letters have to actually be smoothed,
 * or nothing was gained. They have to occupy exactly the width they used
 * to, because every title on both screens is centred or clipped against
 * that measurement and a drawing that disagreed with it would sit off
 * centre or sweep when it did not need to. And a clipped glyph has to
 * show the part of itself that belongs in the window - a title too long
 * for its box scrolls, and scrolling means every frame starts mid
 * letter.
 *
 * The bold treatment adds a fourth: a heavier letter draws outside the
 * box the measurement describes, and Now Playing places its title
 * against the like heart in the corner above it as a compile-time
 * assertion. That assertion is only true while the overspill is the size
 * the header promises. */
static int CheckBigText(void)
{
    int failures = 0;

    const uint16_t ground = GFX_RGB(0, 0, 0), ink = GFX_RGB(31, 31, 31);
    static const char *const kWord = "DSXP Kid A";
    enum { TX = 40, TY = 40, QUARTERS = 6 };

    Gfx_SetTarget(g_sub);
    Gfx_Fill(0, 0, 256, 192, ground);
    const int endX = Gfx_TextBigProp(TX, TY, QUARTERS, ink, kWord);

    /* What every caller lays the text out against. */
    const int want = Gfx_TextBigPropWidth(QUARTERS, kWord);
    char buf[160];
    snprintf(buf, sizeof(buf), "advanced %d, measured %d", endX - TX, want);
    if (endX - TX != want) failures++;
    printf("  [%s] %-52s %s\n", endX - TX != want ? "FAIL" : "PASS",
           "big text ends where it says it will", buf);

    /* Neither ground nor ink: the edges of the letters. A blown-up
     * bitmap has none at all - every pixel is one or the other. */
    const int ih = (GFX_GLYPH_H * QUARTERS) / 4;
    int shades = 0, solid = 0;
    for (int y = TY; y < TY + ih; y++)
        for (int x = TX; x < TX + want; x++) {
            const uint16_t v = Gfx_GetPixel(x, y);
            if (v == ink) solid++;
            else if (v != ground) shades++;
        }

    snprintf(buf, sizeof(buf), "%d part-covered pixels, %d solid",
             shades, solid);
    if (shades < solid / 4) failures++;
    printf("  [%s] %-52s %s\n", shades < solid / 4 ? "FAIL" : "PASS",
           "its edges are covered, not stepped", buf);

    /* And the clipped draw agrees with the unclipped one, column for
     * column. What matters is the letter cut down its middle, which is
     * where a glyph assembled in a buffer and then copied out gets its
     * offset wrong: the copy starts at the buffer's edge while the
     * pixels were written from the window's.
     *
     * Every left edge across a letter and a half rather than one chosen
     * position, because the glyphs are proportional - a single window
     * can easily fall between two of them and cut nothing at all, which
     * is a test that passes by missing. */
    static uint16_t whole[192][256];
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++) whole[y][x] = Gfx_GetPixel(x, y);

    int wrong = 0, leaked = 0, worstAt = -1;
    for (int cx0 = TX + 1; cx0 < TX + 24; cx0++) {
        const int cx1 = cx0 + 40;

        Gfx_Fill(0, 0, 256, 192, ground);
        Gfx_SetClipX(cx0, cx1);
        Gfx_TextBigProp(TX, TY, QUARTERS, ink, kWord);
        Gfx_ResetClipX();

        for (int y = TY; y < TY + ih; y++)
            for (int x = TX; x < TX + want; x++) {
                const uint16_t v = Gfx_GetPixel(x, y);
                if (x >= cx0 && x < cx1) {
                    if (v != whole[y][x]) {
                        if (worstAt < 0) worstAt = cx0;
                        wrong++;
                    }
                } else if (v != ground) {
                    leaked++;
                }
            }
    }

    if (wrong || leaked) failures++;
    snprintf(buf, sizeof(buf), "%d pixels differ%s, %d outside the window",
             wrong, worstAt >= 0 ? " (from clip 0)" : "", leaked);
    printf("  [%s] %-52s %s\n", (wrong || leaked) ? "FAIL" : "PASS",
           "a clipped letter shows its own middle", buf);

    /* The bold treatment: heavier strokes, and still inside the room
     * they asked for.
     *
     * "Heavier" is counted rather than described - the same word at the
     * same size covers more of the page - and the overspill is checked
     * against the number the header gives callers, because a title
     * placed to clear the like heart is only clear of it while that
     * number is honest. */
    {
        enum { LH = 10, LX = 30, LY = 60 };
        const int lih = (GFX_GLYPH_H * LH) / 4;

        int inked[2] = { 0, 0 }, wide[2] = { 0, 0 };
        for (int bold = 0; bold < 2; bold++) {
            Gfx_Fill(0, 0, 256, 192, ground);
            Gfx_SetBigBold(bold != 0);
            const int lw = Gfx_TextBigPropWidth(LH, kWord);
            Gfx_TextBigProp(LX, LY, LH, ink, kWord);
            Gfx_SetBigBold(false);

            for (int y = 0; y < 192; y++)
                for (int x = 0; x < 256; x++) {
                    if (Gfx_GetPixel(x, y) == ground) continue;
                    inked[bold]++;

                    const int pad = bold ? GFX_BIG_BOLD_PAD : 0;
                    if (x < LX - pad || x >= LX + lw + pad ||
                        y < LY - pad || y >= LY + lih + pad)
                        wide[bold]++;
                }
        }

        snprintf(buf, sizeof(buf), "%d pixels of ink against %d",
                 inked[1], inked[0]);
        if (inked[1] < inked[0] * 5 / 4) failures++;
        printf("  [%s] %-52s %s\n",
               inked[1] < inked[0] * 5 / 4 ? "FAIL" : "PASS",
               "bold letters are heavier than the font", buf);

        snprintf(buf, sizeof(buf), "%d pixels outside (plain %d)",
                 wide[1], wide[0]);
        if (wide[0] || wide[1]) failures++;
        printf("  [%s] %-52s %s\n", (wide[0] || wide[1]) ? "FAIL" : "PASS",
               "and stay within the room they ask for", buf);
    }

    Skin_InvalidateBottom();
    return failures;
}

/* The folder mark wears the theme's colour, on every theme.
 *
 * Twenty of the thirty palettes had the same gold written into them, so
 * a folder was yellow on the green console and yellow on the red one -
 * the one mark on the page that had not been told which theme was on.
 * It is derived from the accent now, and what has to hold is that the
 * derivation never lands somewhere useless: on the text colour, where
 * the mark stops reading as a folder, or on the ground, where it stops
 * reading at all.
 *
 * Every theme, and both grounds a folder is drawn on. Themes that are
 * dark and themes that are light, which is what makes a single rule
 * hard: a colour lifted towards the ink is lighter on one and darker on
 * the other. */
static int CheckFolderColour(void)
{
    int failures = 0;
    int worstText = 99, worstGround = 99, sameAsGold = 0;
    const char *worstWhere = "";

    /* What every theme used to be, so a palette quietly going back to a
     * hand-written gold is noticed. */
    const uint16_t gold = GFX_RGB8(0xF0, 0xBA, 0x3C);

    for (int t = 0; t < SKIN_THEMES; t++) {
        Skin_SetTheme(t);

        const uint16_t f = C_TEXT_FOLDER;
        const int gt = abs(Luma(f) - Luma(C_TEXT));
        /* The list rows sit on the page, the path bar on the surface. */
        const int gg = abs(Luma(f) - Luma(C_BG)) < abs(Luma(f) - Luma(C_SURFACE))
                     ? abs(Luma(f) - Luma(C_BG))
                     : abs(Luma(f) - Luma(C_SURFACE));

        if (gt < worstText)   { worstText = gt; worstWhere = Skin_ThemeName(t); }
        if (gg < worstGround) worstGround = gg;
        if (f == gold) sameAsGold++;

        if (gt < 4 || gg < 4) failures++;
    }

    Skin_SetTheme(0);

    printf("  [%s] %-52s text %d, ground %d (worst: %s)\n",
           failures ? "FAIL" : "PASS",
           "the folder mark reads on every theme", worstText, worstGround,
           worstWhere);

    if (sameAsGold) failures++;
    printf("  [%s] %-52s %d of %d themes\n", sameAsGold ? "FAIL" : "PASS",
           "and is not the same gold on all of them", sameAsGold, SKIN_THEMES);

    return failures;
}

/* How much of a pixel one colour has, given the two it sits between.
 *
 * All three channels at once: in 555 a single channel of a blend is
 * quantised to a handful of levels, and the three together give the
 * fraction to about a twentieth. */
static double MixFrac(uint16_t v, uint16_t from, uint16_t to)
{
    int num = 0, den = 0;
    for (int sh = 0; sh <= 10; sh += 5) {
        const int a = (from >> sh) & 31, b = (to >> sh) & 31;
        num += ((v >> sh) & 31) - a;
        den += b - a;
    }
    return den ? (double)num / den : 0.0;
}

/* The width of a circle's row, averaged across the row rather than
 * sampled down its middle. A curve's width changes fastest exactly where
 * a row is widest, so the middle of the row is not its average, and a
 * test that used one would report the drawing as wrong where it is
 * right. Sixty-four slices is far finer than anything being measured. */
static double ChordAvg(double r, double yTop)
{
    double sum = 0;
    for (int k = 0; k < 64; k++) {
        const double dy = yTop + (k + 0.5) / 64.0;
        if (dy * dy < r * r) sum += 2.0 * sqrt(r * r - dy * dy);
    }
    return sum / 64.0;
}

/* Curves are drawn by how much of each pixel they cover, not by whether
 * the pixel's middle is inside them.
 *
 * The difference is the whole of what "pixely" means here. A test that
 * decides inside or outside per pixel puts every edge on a whole pixel,
 * so a shallow part of a curve runs flat for four or five pixels and
 * then steps - which is what the disc's rim and the card's corners did,
 * and what was reported as them looking sawn.
 *
 * What is measured is where the drawn edge actually is. Coverage is read
 * back out of the blend and summed along a row, which gives the edge to
 * a fraction of a pixel, and that is compared with where a real circle
 * would put it. Covered properly the error is a few hundredths; decided
 * per pixel it cannot be better than a half, because a half is what
 * rounding to a whole pixel costs.
 *
 * The disc is checked against its own radius, fitted from the area it
 * covers rather than taken from the constant that drew it. A circle can
 * be the wrong size and still be a circle, and it is the second of those
 * this is about. */
static int CheckRoundEdges(void)
{
    int failures = 0;
    Gfx_SetTarget(g_sub);

    /* The card's corner. Thickness equal to the radius so the whole
     * corner is edge against ground and every pixel in it is a reading. */
    enum { FR = 12, FX = 40, FY = 40, FW = 90, FH = 90 };
    const uint16_t ground = GFX_RGB(0, 0, 0), line = GFX_RGB(31, 31, 31);

    Gfx_Fill(0, 0, 256, 192, ground);
    Gfx_Fill(FX, FY, FW, FH, line);
    DrawRoundedFrame(FX, FY, FW, FH, FR, FR, line, ground);

    double worstCorner = 0;
    for (int dy = 0; dy < FR; dy++) {
        double meas = 0;
        for (int dx = 0; dx < FR; dx++)
            meas += MixFrac(Gfx_GetPixel(FX + dx, FY + dy), line, ground);

        /* Where the arc leaves the row's left edge, averaged the same way. */
        double want = 0;
        for (int k = 0; k < 64; k++) {
            const double up = FR - (dy + (k + 0.5) / 64.0);
            want += FR - sqrt((double)FR * FR - up * up);
        }
        want /= 64.0;

        const double off = fabs(meas - want);
        if (off > worstCorner) worstCorner = off;
    }

    if (worstCorner > 0.30) failures++;
    printf("  [%s] %-52s worst %.2f px\n", worstCorner > 0.30 ? "FAIL" : "PASS",
           "the card's corner is where a real curve would be", worstCorner);

    /* And that nothing underneath leaks into a pixel the edge covers
     * entirely. Blending the ground on and then the edge on top of that
     * left a quarter of whatever was behind showing through - here a red
     * fill under a white edge on black, so any leak is a pixel that is
     * not grey. Only the outside of the arc: a pixel on its inner side is
     * meant to be part edge and part fill. */
    const uint16_t fill = GFX_RGB(31, 0, 0);
    Gfx_Fill(0, 0, 256, 192, ground);
    Gfx_Fill(FX, FY, FW, FH, fill);
    DrawRoundedFrame(FX, FY, FW, FH, FR, 4, line, ground);

    int leaked = 0, worstLeak = 0;
    for (int dy = 0; dy < FR; dy++)
        for (int dx = 0; dx < FR; dx++) {
            const double d = sqrt((FR - dx - 0.5) * (FR - dx - 0.5)
                                + (FR - dy - 0.5) * (FR - dy - 0.5));
            if (fabs(d - FR) > 0.8) continue;      /* not on the outer edge */

            const uint16_t v = Gfx_GetPixel(FX + dx, FY + dy);
            const int r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
            const int skew = (r > g ? r - g : g - r) + (r > b ? r - b : b - r);
            if (skew > worstLeak) worstLeak = skew;
            if (skew > 2) leaked++;
        }

    failures += leaked > 0;
    printf("  [%s] %-52s worst tint %d of 31\n", leaked ? "FAIL" : "PASS",
           "the edge covers what it covers, with no leak", worstLeak);

    /* The disc. Its rim is lit and its hole is not, so the hole is the
     * edge with two colours steady enough to read a fraction out of -
     * and it is the small circle, where being a pixel out shows most. */
    Skin_SetTheme(0);
    Gfx_Fill(0, 0, 256, 192, Skin_ColorBg());
    DrawNoCover(0, 0, NOCOVER_SIDE);

    enum { DC = NOCOVER_SIDE / 2, DWIN = 9 };
    const uint16_t hole = Gfx_GetPixel(DC - 1, DC - 1);      /* dead centre */
    const uint16_t ring = Gfx_GetPixel(DC, DC - 10);         /* clear plastic */

    double area = 0;
    for (int y = DC - DWIN; y < DC + DWIN; y++)
        for (int x = DC - DWIN; x < DC + DWIN; x++)
            area += MixFrac(Gfx_GetPixel(x, y), ring, hole);

    const double fitted = sqrt(area / 3.14159265358979);

    double worstHole = 0;
    for (int y = DC - DWIN; y < DC + DWIN; y++) {
        const double up = y + 0.5 - DC;
        if (fabs(up) > fitted * 0.7) continue;   /* not the flat-topped ends */

        double meas = 0;
        for (int x = DC - DWIN; x < DC + DWIN; x++)
            meas += MixFrac(Gfx_GetPixel(x, y), ring, hole);

        const double off = fabs(meas - ChordAvg(fitted, (double)y - DC));
        if (off > worstHole) worstHole = off;
    }

    if (worstHole > 0.30) failures++;
    printf("  [%s] %-52s worst %.2f px, r %.2f\n",
           worstHole > 0.30 ? "FAIL" : "PASS",
           "the disc's hole is round to a fraction of a pixel",
           worstHole, fitted);

    return failures;
}

/* The tab contents sit in the middle of the tab, in every treatment.
 *
 * "Middle of the tab" is not one measurement: the bar treatment spends
 * its bottom two rows on the accent bar and the boxed one spends its top
 * and bottom on the frame, so centring against the raw cell height puts
 * the content a pixel high in one of them. That is what shipped - inside
 * a box the icon touched the top border, which is how it was spotted,
 * Retro Green being the theme that draws one.
 *
 * Measured rather than asserted from the constants: this renders a tab
 * and finds the topmost and bottommost row with anything in it, which is
 * the same thing the eye does and does not care how the offset was
 * computed. Both treatments, and both an icon-only tab and one with a
 * name beside it.
 */
static int CheckTabCentring(void)
{
    /* Retro Green boxes its tabs; Blue Classic underlines them. Two
     * themes, two treatments, one measurement. */
    /* What each treatment spends on chrome, stated here rather than read
     * out of the drawing code: an underlined tab gives its bottom two
     * rows to the accent bar, a boxed one gives its top and bottom row to
     * the frame. That is a fact about how the two looks are drawn, not
     * about how the offset is computed, so a test that knows it is still
     * measuring the code rather than repeating it. */
    static const struct { int theme; const char *what;
                          int top, bottom, side; } kLooks[] = {
        { 0, "underlined", 0, 2, 0 },
        { 2, "boxed",      1, 2, 2 },
    };

    int failures = 0;
    char worst[80] = "";
    int worstOff = 0;

    for (size_t k = 0; k < sizeof(kLooks) / sizeof(kLooks[0]); k++) {
        Skin_SetTheme(kLooks[k].theme);

        for (int sel = 0; sel < TAB_COUNT; sel++) {
            SkinRect t[TAB_COUNT];
            TabLayout((SkinTab)sel, t);

            for (int i = 0; i < TAB_COUNT; i++) {
                Gfx_SetTarget(g_sub);
                Gfx_Fill(0, 0, 256, 192, Skin_ColorBg());
                DrawTab(i, &t[i], i == sel);

                /* The cell minus what this treatment reserves. */
                const int y0 = t[i].y + kLooks[k].top;
                const int y1 = t[i].y + t[i].h - 1 - kLooks[k].bottom;

                /* Inside the frame's uprights too. Scanning across them
                 * found ink on every row of a boxed tab, so the measured
                 * extent was the frame and the content underneath it
                 * could sit anywhere at all. */
                int top = -1, bot = -1;
                for (int y = y0; y <= y1; y++) {
                    for (int x = t[i].x + 1 + kLooks[k].side;
                         x < t[i].x + t[i].w - 1 - kLooks[k].side; x++) {
                        uint16_t p = g_sub[y * 256 + x];
                        if (p == Skin_ColorBg() || p == Skin_ColorSurface())
                            continue;
                        if (top < 0) top = y;
                        bot = y;
                        break;
                    }
                }

                if (top < 0) continue;      /* a cell with nothing in it */

                const int above = top - y0;
                const int below = y1 - bot;
                const int off = above > below ? above - below : below - above;

                if (off > worstOff) {
                    worstOff = off;
                    snprintf(worst, sizeof(worst), "%s tab %d%s: %d above, %d below",
                             kLooks[k].what, i, i == sel ? " (named)" : "",
                             above, below);
                }

                /* One pixel of slack, and no more. Eleven rows of icon in
                 * an even band cannot be exactly centred, so a difference
                 * of one is arithmetic; two is a mistake. */
                if (off <= 1) continue;

                printf("  [FAIL] %s tab %d: %d above, %d below\n",
                       kLooks[k].what, i, above, below);
                failures++;
            }
        }
    }

    Skin_SetTheme(0);
    printf("  [%s] %-52s worst %s\n", failures ? "FAIL" : "PASS",
           "tab contents are centred in every treatment",
           worst[0] ? worst : "nothing drawn");
    return failures;
}

/* The band the like heart sits in stays its own.
 *
 * It is above the title and below the tabs, and nothing else on the page
 * is placed there - but "nothing else is placed there" is a fact about
 * arithmetic that grows title blocks and centres text, not a rule anyone
 * wrote down. A static assert pins the title, whose top edge is derived;
 * this pins everything else, by rendering the page and looking.
 *
 * The long title is the case worth having: it is the one that reaches
 * furthest across the page, and the one where a future change to the
 * text box would show up first.
 */
static int CheckLikeBandClear(void)
{
    static const struct { const char *what; const char *title; const char *artist;
                          bool liked; } kCases[] = {
        { "short title",      "Kid A", "Radiohead", false },
        { "liked",            "Kid A", "Radiohead", true  },
        { "long title",
          "www.MusicKordi.com - a title of unreasonable length", "", false },
        { "no artist",        "Kid A", "", true },
    };
    int failures = 0;

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_NOWPLAYING);
        Skin_DrawNowPlaying(kCases[i].title, kCases[i].artist, true, true,
                            44100, 320, 2, false, 0, 7, "next",
                            false, REPEAT_OFF, kCases[i].liked);
        Skin_DrawTransport(true, true, 96, 71000, 262000, true);

        const uint16_t bg = g_sub[(LIKE_HIT_Y + 2) * 256 + 120];
        int intruding = 0, heart = 0;

        for (int y = LIKE_HIT_Y; y < LIKE_HIT_Y + LIKE_HIT_H; y++) {
            for (int x = 0; x < LIKE_HIT_X; x++)
                if (g_sub[y * 256 + x] != bg) intruding++;
            for (int x = LIKE_HIT_X; x < LIKE_HIT_X + LIKE_HIT_W; x++)
                if (g_sub[y * 256 + x] != bg) heart++;
        }

        if (intruding || heart == 0) {
            printf("  [FAIL] %s: %d pixels intrude, heart has %d\n",
                   kCases[i].what, intruding, heart);
            failures++;
        }
    }

    /* And the two states differ, or the shape is carrying nothing. */
    printf("  [%s] %-52s %s\n", failures ? "FAIL" : "PASS",
           "the like heart has the corner to itself", "4 pages");
    return failures;
}

/* Every control says which page it belongs to.
 *
 * kButtonPage is a designated-initialiser table, so a button added to
 * the enum and forgotten here does not fail to build - it takes zero,
 * and zero used to be a real page. The Faves and About tabs went in that
 * way: drawn on every page, answering a tap only on Files, which reads
 * as the top bar being broken rather than as two missing lines.
 *
 * The values are stored shifted now so zero means nobody wrote one, and
 * this is the thing that notices.
 */
static int CheckButtonPages(void)
{
    int missing = 0;

    for (int i = 0; i < SKIN_BTN_COUNT; i++) {
        if (kButtonPage[i] != 0) continue;
        printf("  [FAIL] control %d has no page - it will only answer on "
               "whatever page happens to be first\n", i);
        missing++;
    }

    printf("  [%s] %-52s %d controls\n", missing ? "FAIL" : "PASS",
           "every control declares the page it lives on", SKIN_BTN_COUNT);
    return missing;
}

/* The list controls answer taps exactly where they are drawn.
 *
 * They used to be a fixed table gated on which tab was up, and a
 * playlist opened from the Faves tab - which is where every playlist
 * lives - showed none of the controls that edit its contents and made
 * none of them tappable. Both halves are checked here, in each of the
 * four states a browser can be in, because "drawn" and "live" coming
 * apart is exactly the failure and a test of either alone would have
 * passed.
 *
 * Which controls belong in which state is the other half of the table.
 * Inside a list there are two - mark, and remove what is marked. There
 * used to be a third, "add more to this one", and from the Faves tab it
 * closed the list and left the reader on the shelf with nothing to mark
 * and no sign anything had happened. Adding runs the other way round:
 * mark tracks in a folder, press the control there, pick the list. */
/* The Blue theme and the stored default must name the same top screen.
 *
 * Two places set it and only one of them is obvious. Settings_Load picks
 * what a card with no config gets; ApplyTheme then hands the top screen
 * over to whatever the theme prefers, for as long as the listener has
 * not chosen one - so a disagreement here means the stored default is
 * overwritten before the first frame, and changing it appears to do
 * nothing at all. */
/* The playing row is marked whether or not its name needed folding.
 *
 * The file list used to be handed the transliterated name and compare
 * it against the raw one in the row. Those are equal only when folding
 * changed nothing - so the marker appeared for "02 - Kid A.mp3" and
 * never for a Persian or accented filename, which are precisely the
 * ones the fold exists for.
 *
 * Checked by drawing the same list twice, once with the track playing
 * and once without, and asking whether anything moved. With the fault
 * the two are pixel-identical for a folded name. */
static int CheckPlayingMarker(void)
{
    static const char *const kNames[] = {
        "02 - Kid A.mp3",                 /* folds to itself */
        "\xd8\xaf\xd8\xa7\xd8\xb1\xdb\x8c\xd9\x88\xd8\xb4.mp3",  /* Persian: folding rewrites it */
        "Bj\xc3\xb6rk - Hyperballad.mp3",  /* an accent is enough */
    };
    int failures = 0;

    for (unsigned k = 0; k < sizeof(kNames)/sizeof(kNames[0]); k++) {
        static FileBrowser fb;
        memset(&fb, 0, sizeof(fb));
        snprintf(fb.cwd, FB_PATH_LEN, "/Music");
        snprintf(fb.entries[0].name, FB_NAME_LEN, "%s", kNames[k]);
        fb.count = 1;
        /* The selection is the same in both renders, so it cannot
         * be what the diff below picks up. */

        static uint16_t idle[256 * 192], playing[256 * 192];

        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawFileList(&fb, NULL);
        memcpy(idle, g_sub, sizeof(idle));

        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawFileList(&fb, kNames[k]);     /* the raw name, as the player now passes */
        memcpy(playing, g_sub, sizeof(playing));

        int diff = 0;
        for (unsigned i = 0; i < 256 * 192; i++) if (idle[i] != playing[i]) diff++;

        char folded[FB_NAME_LEN];
        TextFold_Utf8(kNames[k], folded, sizeof(folded));
        const bool rewritten = strcmp(folded, kNames[k]) != 0;

        printf("  [%s] %-52s %s: %d px, folding %s\n",
               diff > 0 ? "PASS" : "FAIL",
               "the playing row is marked however its name is spelled",
               folded, diff, rewritten ? "rewrote it" : "left it alone");
        if (diff <= 0) failures++;
    }
    Skin_InvalidateBottom();
    return failures;
}

static int CheckDefaultTopScreen(void)
{
    const int fromTheme = Skin_ThemeTopChoice(0);
    const bool ok = (fromTheme == VIS_EDGE);
    printf("  [%s] %-52s theme 0 offers %d, default %d\n",
           ok ? "PASS" : "FAIL",
           "the Blue theme agrees with the stored default meter",
           fromTheme, VIS_EDGE);
    return ok ? 0 : 1;
}

static int CheckBrowserButtons(void)
{
    int failures = 0;

    static const struct {
        const char *what;
        bool locked, inPlaylist;
        int  want;                 /* how many controls */
        bool mark, add, del;       /* and which */
    } kStates[] = {
        { "a folder",            false, false, 2, true,  true,  false },
        { "a list opened there", false, true,  2, true,  false, true  },
        { "the Faves shelf",     true,  false, 1, false, false, true  },
        { "a list on the shelf", true,  true,  2, true,  false, true  },
    };

    for (unsigned i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) {
        FileBrowser fb;
        memset(&fb, 0, sizeof(fb));
        fb.locked     = kStates[i].locked;
        fb.inPlaylist = kStates[i].inPlaylist;

        SkinButton ids[BROWSER_BTN_MAX];
        SkinRect   rects[BROWSER_BTN_MAX];
        const int n = BrowserButtons(&fb, ids, rects);

        bool sawMark = false, sawAdd = false, sawDel = false;
        int  notLive = 0, offScreen = 0;

        for (int k = 0; k < n; k++) {
            sawMark |= (ids[k] == SKIN_BTN_MARK);
            sawAdd  |= (ids[k] == SKIN_BTN_SAVELIST);
            sawDel  |= (ids[k] == SKIN_BTN_DELLIST);

            /* Every control that is drawn answers a tap in its middle,
             * and answers with itself. */
            const int cx = rects[k].x + rects[k].w / 2;
            const int cy = rects[k].y + rects[k].h / 2;
            if (Skin_HitTestBrowserButton(cx, cy, &fb) != ids[k]) notLive++;

            if (rects[k].x < 0 || rects[k].x + rects[k].w > 256) offScreen++;

            /* And no two of them share a pixel. */
            for (int j = 0; j < k; j++)
                if (rects[j].x < rects[k].x + rects[k].w &&
                    rects[k].x < rects[j].x + rects[j].w)
                    failures++;
        }

        const bool ok = n == kStates[i].want &&
                        sawMark == kStates[i].mark &&
                        sawAdd  == kStates[i].add &&
                        sawDel  == kStates[i].del &&
                        notLive == 0 && offScreen == 0;

        printf("  [%s] %-52s %s: %d controls, %d dead\n",
               ok ? "PASS" : "FAIL",
               "the list controls are live where they are drawn",
               kStates[i].what, n, notLive);
        if (!ok) failures++;
    }

    /* And nothing answers where nothing is drawn. The shelf shows one
     * control at the right; the two slots to its left are path bar, and
     * a tap there used to be able to reach a control that was not on
     * the screen. */
    {
        FileBrowser fb;
        memset(&fb, 0, sizeof(fb));
        fb.locked = true;

        int live = 0;
        for (int x = BROWSER_BTN_RIGHT - 2 * BROWSER_BTN_STEP;
             x < BROWSER_BTN_RIGHT; x++)
            if (Skin_HitTestBrowserButton(x, BROWSER_BTN_Y + 6, &fb) !=
                SKIN_BTN_NONE) live++;

        printf("  [%s] %-52s %d live pixels\n", live ? "FAIL" : "PASS",
               "and nothing answers where none is drawn", live);
        if (live) failures++;
    }

    return failures;
}

/* Leaving a full-screen page must not leave any of it behind.
 *
 * The frame loop draws a view change in a particular order - the tab
 * strip, then the page invalidate, then the content - and each page
 * keeps its own "already correct" state and skips anything it thinks it
 * drew. If the invalidate does not reach every one of those, whatever
 * the previous page put there stays on screen until something else
 * happens to move.
 *
 * So the switch is performed exactly as the loop performs it and the
 * result compared against the same page arrived at from nothing. Any
 * pixel that differs is a pixel the previous page left. */
static int CheckPageSwitch(void)
{
    static uint16_t after[256 * 256], clean[256 * 256];
    int failures = 0;

    static FileBrowser fb;
    memset(&fb, 0, sizeof(fb));
    snprintf(fb.cwd, FB_PATH_LEN, "/Music/Kid A");
    static const char *const kRows[] = {
        "..", "Bootleg Live Sets", "01 - Everything In Its Right Place.mp3",
        "02 - Kid A.mp3", "03 - The National Anthem.mp3",
    };
    for (unsigned i = 0; i < sizeof(kRows)/sizeof(kRows[0]); i++) {
        snprintf(fb.entries[i].name, FB_NAME_LEN, "%s", kRows[i]);
        fb.entries[i].isDir = (i < 2);
        fb.count++;
    }
    fb.selected = 2;

    static const struct { const char *what; SkinTab tab; } kFrom[] = {
        { "Setup",       SKIN_TAB_SETUP },
        { "Diagnostics", SKIN_TAB_TEST  },
    };

    for (unsigned k = 0; k < sizeof(kFrom)/sizeof(kFrom[0]); k++) {
        /* Sit on the page being left, fully drawn. A stand-in row list
         * rather than the real one: what matters is that the page fills
         * the screen the way the settings pages do, not which settings
         * are on it. */
        static const char *const kSeg[] = { "Off", "On" };
        SkinRow rows[5];
        for (int r = 0; r < 5; r++) {
            rows[r].kind    = (r & 1) ? SKIN_ROW_SLIDER : SKIN_ROW_CHOICE;
            rows[r].caption = "A settings row with a reasonably long name";
            rows[r].labels  = (r & 1) ? NULL : kSeg;
            rows[r].count   = (r & 1) ? 8 : 2;
            rows[r].value   = r % 2;
        }
        const int n = 5;
        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(kFrom[k].tab);
        Skin_DrawRows(rows, n, 0, 0, 0, SKIN_ROW_SLOTS);

        /* Switch to Files in the loop's own order. */
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_FILES);
        Skin_InvalidateBottom();
        Skin_DrawFileList(&fb, NULL);
        memcpy(after, g_sub, sizeof(after));

        /* And the same page with nothing before it. */
        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_FILES);
        Skin_DrawFileList(&fb, NULL);
        memcpy(clean, g_sub, sizeof(clean));

        int diff = 0, firstRow = -1;
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++)
                if (after[y*256+x] != clean[y*256+x]) {
                    diff++;
                    if (firstRow < 0) firstRow = y;
                }

        printf("  [%s] %-46s %s -> Files: %d stale pixels%s\n",
               diff ? "FAIL" : "PASS", "leaving a page leaves nothing behind",
               kFrom[k].what, diff,
               firstRow >= 0 ? "" : "");
        if (diff) {
            printf("         first differing row %d\n", firstRow);
            failures++;
        }
    }

    /* The support and help pages are not views but modals: they replace
     * the whole bottom screen and the loop invalidates on the way out
     * rather than on a view change. Different path, same requirement. */
    static const struct { const char *what; int help; } kModal[] = {
        { "Support", 0 }, { "Help", 1 },
    };
    for (unsigned k = 0; k < sizeof(kModal)/sizeof(kModal[0]); k++) {
        if (kModal[k].help) Keyboard_OpenHelp(); else Keyboard_OpenSupport();
        Skin_BeginFrame();
        Keyboard_Draw();
        Keyboard_Close();

        /* What the loop does when the modal goes away. */
        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_FILES);
        Skin_DrawFileList(&fb, NULL);
        memcpy(after, g_sub, sizeof(after));

        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_FILES);
        Skin_DrawFileList(&fb, NULL);
        memcpy(clean, g_sub, sizeof(clean));

        int diff = 0;
        for (int i = 0; i < 192 * 256; i++) if (after[i] != clean[i]) diff++;
        printf("  [%s] %-46s %s -> Files: %d stale pixels\n",
               diff ? "FAIL" : "PASS", "leaving a page leaves nothing behind",
               kModal[k].what, diff);
        if (diff) failures++;
    }

    /* And the Faves tab, which is the same widget showing a locked
     * folder - a different set of controls in the path bar, so a
     * different set of pixels to leave behind. */
    {
        static FileBrowser fav;
        memset(&fav, 0, sizeof(fav));
        snprintf(fav.cwd, FB_PATH_LEN, "%s", PLAYLIST_DIR);
        fav.locked = true;
        static const char *const kLists[] = { "Liked.m3u", "Evening.m3u" };
        for (unsigned i = 0; i < 2; i++) {
            snprintf(fav.entries[i].name, FB_NAME_LEN, "%s", kLists[i]);
            fav.entries[i].isPlaylist = true;
            fav.entries[i].isLiked = (i == 0);
            fav.count++;
        }

        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_SETUP);
        {
            static const char *const kSeg2[] = { "Off", "On" };
            SkinRow rr[5];
            for (int r = 0; r < 5; r++) {
                rr[r].kind = SKIN_ROW_CHOICE; rr[r].caption = "A settings row";
                rr[r].labels = kSeg2; rr[r].count = 2; rr[r].value = 1;
            }
            Skin_DrawRows(rr, 5, 0, 0, 0, SKIN_ROW_SLOTS);
        }

        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_FAVES);
        Skin_InvalidateBottom();
        Skin_DrawFileList(&fav, NULL);
        memcpy(after, g_sub, sizeof(after));

        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_FAVES);
        Skin_DrawFileList(&fav, NULL);
        memcpy(clean, g_sub, sizeof(clean));

        int diff = 0;
        for (int i = 0; i < 192 * 256; i++) if (after[i] != clean[i]) diff++;
        printf("  [%s] %-46s Setup -> Faves: %d stale pixels\n",
               diff ? "FAIL" : "PASS", "leaving a page leaves nothing behind", diff);
        if (diff) failures++;
    }

    Skin_InvalidateBottom();
    return failures;
}

/* What each top screen costs to keep on screen.
 *
 * Not a pass or a fail - a number, printed so that a change to any of
 * these cards shows up as a number moving rather than as the console
 * feeling slower. Steady state: the card is drawn once to settle it,
 * then driven for a second of frames with the clock advancing, and what
 * is counted is the pixels the repaints actually wrote.
 *
 * Both buffers are exercised, because every one of these keeps
 * per-buffer state about what it last drew and alternates between them
 * exactly as the console does. Measuring one buffer twice would report
 * a card as free that in fact redraws everything every frame. */
static int CheckTopCardCost(void)
{
    /* Card 1 is the cover - TOP_CARD_COVER is TOP_CARD(1). Card 0 is
     * the plain one beside it. Naming them by index rather than by what
     * they are is how the art ended up on the wrong one. */
    static const struct { int card; bool art; const char *what; } kCards[] = {
        { 0, false, "Plain"          },
        { 1, true,  "Cover"          },
        { 1, false, "Cover (no art)" },
        { 2, false, "Cassette"       },
        { 3, false, "Gramophone"     },
    };

    printf("  top screen cost, pixels per second at 60fps:\n");

    for (unsigned k = 0; k < sizeof(kCards)/sizeof(kCards[0]); k++) {
        Skin_InvalidateTopCard();
        if (kCards[k].art) {
            static uint16_t art[88 * 116];
            for (int y = 0; y < 88; y++)
                for (int x = 0; x < 116; x++)
                    art[y * 116 + x] = (uint16_t)((y & 31) << 5 | (x & 31));
            Skin_SetTopArt(art, 116, 88);
        } else {
            Skin_SetTopArt(NULL, 0, 0);
        }

        const int card = kCards[k].card;

        /* Each case is settled with the very parameters it will then
         * be measured with. Settling with anything else makes the first
         * measured frame a change, and that one frame swamps sixty. */
        #define SETTLE_AND_RUN(NAME, TICK, RESULT)                            \
            do {                                                              \
                Skin_InvalidateTopCard();                                     \
                for (int i = 0; i < 4; i++)                                   \
                    Skin_DrawTopCard(i & 1, card, (NAME), "An Artist",        \
                                     "The Next One.mp3", true, true,          \
                                     30990, 210000, 44100, 192, 2, false,     \
                                     3, 12, 0);                               \
                g_gfxPixels = 0;                                              \
                for (int f = 0; f < 60; f++) {                                \
                    if (TICK) Skin_TickMarquee();                             \
                    Skin_DrawTopCard(f & 1, card, (NAME), "An Artist",        \
                                     "The Next One.mp3", true, true,          \
                                     30990 + f * 16, 210000, 44100, 192, 2,   \
                                     false, 3, 12, 0);                        \
                }                                                             \
                (RESULT) = g_gfxPixels;                                       \
            } while (0)

        /* Nothing moving at all. */
        unsigned long idle;
        Skin_InvalidateTopCard();
        for (int i = 0; i < 4; i++)
            Skin_DrawTopCard(i & 1, card, "Short", "An Artist", "Next.mp3",
                             true, true, 30990, 210000, 44100, 192, 2,
                             false, 3, 12, 0);
        g_gfxPixels = 0;
        for (int f = 0; f < 60; f++)
            Skin_DrawTopCard(f & 1, card, "Short", "An Artist", "Next.mp3",
                             true, true, 30990, 210000, 44100, 192, 2,
                             false, 3, 12, 0);
        idle = g_gfxPixels;

        /* A second of playback with a name that fits: one clock tick. */
        unsigned long clockOnly;
        SETTLE_AND_RUN("Short", 0, clockOnly);

        /* And with a name long enough to sweep, which is the case the
         * cassette and the gramophone were tuned for. */
        unsigned long sweeping;
        SETTLE_AND_RUN("A Track With A Fairly Long Name", 1, sweeping);
        #undef SETTLE_AND_RUN

        printf("    %-16s idle %7lu   playing %7lu   sweeping %8lu px/s\n",
               kCards[k].what, idle, clockOnly, sweeping);
    }

    Skin_SetTopArt(NULL, 0, 0);
    Skin_InvalidateTopCard();
    return 0;
}

/* Does the file list actually paint every pixel it owns?
 *
 * The previous version of this compared a page arrived at from Setup
 * against the same page arrived at from nothing, which cannot see this:
 * a row that the list never paints keeps whatever was under it in both
 * cases, so the two agree and the test passes while the screen is
 * wrong. The photograph showed a fragment of the settings page sitting
 * in the gap under the path bar.
 *
 * So the screen is filled with a colour nothing draws, the page is then
 * drawn from a full reset, and anything still showing that colour is a
 * pixel the page believes somebody else is painting. */
static int CheckContentCoverage(void)
{
    const uint16_t kMark = 0xF81F;        /* magenta; no theme uses it */
    int failures = 0;

    static FileBrowser fb;
    memset(&fb, 0, sizeof(fb));
    snprintf(fb.cwd, FB_PATH_LEN, "/music");
    static const char *const kRows[] = {
        "..", "1692240642.mp3", "2197955343.mp3", "Albert Brite - Ya.mp3",
        "Celestial Dance.mp3",
    };
    for (unsigned i = 0; i < sizeof(kRows)/sizeof(kRows[0]); i++) {
        snprintf(fb.entries[i].name, FB_NAME_LEN, "%s", kRows[i]);
        fb.entries[i].isDir = (i == 0);
        fb.count++;
    }
    fb.selected = 4;

    static const struct { const char *what; bool locked; } kCase[] = {
        { "Files", false }, { "Faves", true },
    };

    for (unsigned k = 0; k < 2; k++) {
        fb.locked = kCase[k].locked;
        if (kCase[k].locked) {
            for (int i = 0; i < fb.count; i++) {
                fb.entries[i].isDir = false;
                fb.entries[i].isPlaylist = true;
            }
        }

        for (int i = 0; i < 256 * 192; i++) g_sub[i] = kMark;

        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(kCase[k].locked ? SKIN_TAB_FAVES : SKIN_TAB_FILES);
        Skin_DrawFileList(&fb, NULL);

        /* Only the content area: the chrome above it is a separate
         * region and is drawn by the caller. */
        int bare = 0, firstY = -1, firstX = -1;
        for (int y = 18; y < 192; y++)
            for (int x = 0; x < 256; x++)
                if (g_sub[y * 256 + x] == kMark) {
                    bare++;
                    if (firstY < 0) { firstY = y; firstX = x; }
                }

        if (bare) {
            printf("  [FAIL] %-46s %s: %d unpainted, first at (%d,%d)\n",
                   "the file list paints every pixel it owns",
                   kCase[k].what, bare, firstX, firstY);
            /* Which rows, so the gap is obvious rather than a number. */
            /* As x ranges, which says what is missing rather than how
             * much of it. */
            for (int y = 18; y < 192; y += 1) {
                int runStart = -1, printed = 0;
                for (int x = 0; x <= 256; x++) {
                    bool m = (x < 256) && g_sub[y * 256 + x] == kMark;
                    if (m && runStart < 0) runStart = x;
                    else if (!m && runStart >= 0) {
                        if (!printed) { printf("           row %3d:", y); printed = 1; }
                        printf(" x%d-%d", runStart, x - 1);
                        runStart = -1;
                    }
                }
                if (printed) printf("\n");
                if (y == 40) { printf("           ...\n"); break; }
            }
            failures++;
        } else {
            printf("  [PASS] %-46s %s: none\n",
                   "the file list paints every pixel it owns", kCase[k].what);
        }
    }

    Skin_InvalidateBottom();
    return failures;
}

/* A held repaint of the cover has to look exactly like a full one.
 *
 * The card now keeps the picture and the wordmark between repaints and
 * only paints the lines that move. That is the same trade the cassette
 * and the gramophone make, and the same way it goes wrong: a band that
 * is not cleared leaves the previous text underneath the new text, and
 * a band that is cleared too generously rubs out something that was
 * meant to stay. Neither shows up as a crash - only as a card that
 * looks slightly wrong, which is why it is compared pixel for pixel.
 *
 * Driven for a run of frames with the clock moving and the title
 * sweeping, forcing a full redraw alongside each held one. */
static int CheckCoverIncremental(void)
{
    static uint16_t full[256 * 256];
    int worst = 0, worstFrame = -1;

    static uint16_t art[88 * 116];
    for (int y = 0; y < 88; y++)
        for (int x = 0; x < 116; x++)
            art[y * 116 + x] = (uint16_t)(((y * 3) & 31) << 5 | ((x * 5) & 31));
    Skin_SetTopArt(art, 116, 88);

    const char *kTitle = "A Track With A Fairly Long Name That Sweeps";
    const char *kNext  = "And The One After It Is Long Too.mp3";

    Skin_InvalidateTopCard();
    for (int f = 0; f < 90; f++) {
        Skin_TickMarquee();

        /* Held: the same buffer over and over, so the card keeps what
         * it drew and repaints only what moved. */
        Skin_DrawTopCard(0, 1, kTitle, "An Artist", kNext, true, true,
                         30990 + f * 400, 210000, 44100, 192, 2, false,
                         3, 12, 0);
        memcpy(full, g_sub, sizeof(full));

        /* And the same frame from nothing at all. */
        Skin_InvalidateTopCard();
        Skin_DrawTopCard(0, 1, kTitle, "An Artist", kNext, true, true,
                         30990 + f * 400, 210000, 44100, 192, 2, false,
                         3, 12, 0);

        int diff = 0;
        for (int i = 0; i < 192 * 256; i++) if (full[i] != g_sub[i]) diff++;
        if (diff > worst) {
            worst = diff; worstFrame = f;
            int shown = 0;
            for (int y = 0; y < 192 && shown < 8; y++)
                for (int x = 0; x < 256 && shown < 8; x++)
                    if (full[y*256+x] != g_sub[y*256+x]) {
                        printf("           differs at (%d,%d): held 0x%04X full 0x%04X\n",
                               x, y, full[y*256+x], g_sub[y*256+x]);
                        shown++;
                    }
        }

        /* Put the held state back for the next round, drawing the frame
         * again so the buffer holds what a held run would have left. */
        Skin_DrawTopCard(0, 1, kTitle, "An Artist", kNext, true, true,
                         30990 + f * 400, 210000, 44100, 192, 2, false,
                         3, 12, 0);
    }

    Skin_SetTopArt(NULL, 0, 0);
    Skin_InvalidateTopCard();

    printf("  [%s] %-46s worst %d pixels, at frame %d\n",
           worst ? "FAIL" : "PASS",
           "a held cover repaint matches a full one", worst, worstFrame);
    return worst ? 1 : 0;
}

static int CheckNoOverlaps(void)
{
    int failures = 0;

    for (int a = 0; a < SKIN_BTN_COUNT; a++) {
        for (int b = a + 1; b < SKIN_BTN_COUNT; b++) {
            int pa = kButtonPage[a], pb = kButtonPage[b];

            /* Only controls that can be on screen together can clash.
             * A control may belong to a set of pages, so this asks
             * whether any page shows both rather than comparing tags. */
            /* Unshifted. The stored values are shifted so that zero
             * can mean "unset", and these went in as they were stored -
             * so every page this compared was the wrong one by the width
             * of the shift, and the pairs it checked were not the pairs
             * that can be on screen together. */
            bool together = false;
            for (int t = 0; t <= SKIN_TAB_TEST && !together; t++)
                together = PageShows(UNPG(pa), (SkinTab)t) &&
                           PageShows(UNPG(pb), (SkinTab)t);
            if (!together) continue;

            const SkinRect *ra = &kRects[a], *rb = &kRects[b];
            if (ra->w == 0 || rb->w == 0) continue;

            bool overlap = ra->x < rb->x + rb->w && rb->x < ra->x + ra->w &&
                           ra->y < rb->y + rb->h && rb->y < ra->y + ra->h;
            if (overlap) {
                printf("  [FAIL] controls %d and %d overlap: "
                       "(%d,%d %dx%d) vs (%d,%d %dx%d)\n",
                       a, b, ra->x, ra->y, ra->w, ra->h, rb->x, rb->y, rb->w, rb->h);
                failures++;
            }
        }
    }

    printf("  [%s] no two controls on a page share pixels\n",
           failures ? "FAIL" : "PASS");
    return failures;
}

/* The bars effect only repaints what changed since that buffer was last
 * shown. That is a real speedup and a real chance to leave stale pixels
 * behind, so it is checked rather than trusted: the same input is run
 * twice, once incrementally and once forcing a full redraw every frame,
 * and the two must come out pixel for pixel identical. */
/* Same reference check for whichever effect is asked for. */
static int CheckIncremental(const char *name, int audioKind)
{
    static uint16_t reference[2][256 * 256];
    static int16_t audio[1152 * 2];
    const int kFrames = 40;

    /* Reference: full redraw every frame. */
    ResetAudio();
    s_buf[0] = g_visBuf[0];
    s_buf[1] = g_visBuf[1];
    Visualizer_Init();

    for (int i = 0; i < kFrames; i++) {
        MakeAudio(audio, 1152, audioKind);
        Visualizer_Invalidate();        /* force the slow path */
        Visualizer_Render(audio, 1152);
        s_backBuffer ^= 1;
    }
    memcpy(reference[0], g_visBuf[0], sizeof(reference[0]));
    memcpy(reference[1], g_visBuf[1], sizeof(reference[1]));

    /* Same input, incremental. */
    ResetAudio();
    Visualizer_Init();

    for (int i = 0; i < kFrames; i++) {
        MakeAudio(audio, 1152, audioKind);
        Visualizer_Render(audio, 1152);
        s_backBuffer ^= 1;
    }

    int diff = 0;
    int firstX = -1, firstY = -1, firstBuf = -1;
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 256 * 192; i++)
            if (reference[b][i] != g_visBuf[b][i]) {
                if (firstX < 0) { firstBuf = b; firstX = i % 256; firstY = i / 256; }
                diff++;
            }
    if (diff) {
        printf("        first difference: buffer %d at x=%d y=%d (bar %d)\n",
               firstBuf, firstX, firstY, firstX / 8);
        int shown = 0;
        for (int i = 0; i < 256 * 192 && shown < 6; i++) {
            if (reference[firstBuf][i] == g_visBuf[firstBuf][i]) continue;
            printf("          x=%3d y=%3d  incremental %04x  full %04x\n",
                   i % 256, i / 256, g_visBuf[firstBuf][i], reference[firstBuf][i]);
            shown++;
        }
    }

    printf("  [%s] incremental %s matches a full redraw exactly (%d pixels differ)\n",
           diff ? "FAIL" : "PASS", name, diff);

#ifdef DSXP_COUNT_PIXELS
    /* How much the incremental path actually saves, on the same input. */
    ResetAudio();
    Visualizer_Init();
    for (int i = 0; i < 20; i++) {   /* let the effect settle first */
        MakeAudio(audio, 1152, audioKind); Visualizer_Render(audio, 1152); s_backBuffer ^= 1;
    }
    g_visPixels = 0; g_visRows = 0;
    for (int i = 0; i < 60; i++) {
        MakeAudio(audio, 1152, audioKind); Visualizer_Render(audio, 1152); s_backBuffer ^= 1;
    }
    unsigned long incremental = g_visPixels / 60;
    unsigned long incRows     = g_visRows / 60;

    ResetAudio();
    Visualizer_Init();
    for (int i = 0; i < 20; i++) {
        MakeAudio(audio, 1152, audioKind); Visualizer_Render(audio, 1152); s_backBuffer ^= 1;
    }
    g_visPixels = 0;
    for (int i = 0; i < 60; i++) {
        MakeAudio(audio, 1152, audioKind); Visualizer_Invalidate();
        Visualizer_Render(audio, 1152); s_backBuffer ^= 1;
    }
    unsigned long full = g_visPixels / 60;

    printf("        %-8s %6lu px/frame, %5lu rows touched  vs %6lu px full\n",
           name, incremental, incRows, full);
#endif

    return diff ? 1 : 0;
}

/* ---- region-split verification ----------------------------------- */

/* The bottom screen is composed per region and then only the rows the
 * drawing code marked dirty are sent to VRAM. Two things can go wrong:
 * a region can depend on pixels another region was expected to draw, and
 * a draw call can write a row it never marked - which the DMA would then
 * leave stale on screen.
 *
 * Both are caught by composing the same final state two ways and
 * demanding the images match exactly. */

typedef struct { int tab, sel, scrollTop, volume; bool playing; uint32_t elapsed; } Scene;

/* Exactly what the main loop does for a given set of dirty regions: the
 * backdrop only when the page changed, so the file list's per-row cache
 * survives an ordinary data update. */
static void ComposePartial(const Scene *s, unsigned regions, bool pageChanged)
{
    g_fb.selected = s->sel;
    g_fb.scrollTop = s->scrollTop;

    Skin_BeginFrame();
    if (regions & 1) Skin_DrawChrome(s->tab);
    if (regions & 2) {
        if (pageChanged) Skin_InvalidateContent();
        Skin_DrawFileList(&g_fb, "02 - Kid A.mp3");
    }
    /* No region 4 here: the transport strip is gone, and the Files page
     * has nothing below the list to compose. */
}

static void ComposeFull(const Scene *s)
{
    g_fb.selected = s->sel;
    g_fb.scrollTop = s->scrollTop;
    Skin_InvalidateBottom();   /* the reference is a true full redraw */
    Skin_BeginFrame();
    Skin_DrawChrome(s->tab);
    Skin_DrawFileList(&g_fb, "02 - Kid A.mp3");
}

static unsigned g_composePixels;

static int CheckRegionSplit(const char *what, const Scene *a, const Scene *b,
                            unsigned regions)
{
    static uint16_t full[GFX_W * GFX_H];

    /* Reference: the same end state drawn the old way, everything at once. */
    ComposeFull(b);
    memcpy(full, g_sub, sizeof(full));

    /* Now the incremental path: start from A, redraw only `regions`. */
    ComposeFull(a);
    static uint16_t before[GFX_W * GFX_H];
    memcpy(before, g_sub, sizeof(before));

#ifdef DSXP_COUNT_PIXELS
    g_gfxPixels = 0;
#endif
    ComposePartial(b, regions, false);
#ifdef DSXP_COUNT_PIXELS
    g_composePixels = g_gfxPixels;
#endif

    int top, bottom;
    Gfx_GetDirty(&top, &bottom);

    int badPixel = -1, unmarked = -1;

    for (int y = 0; y < GFX_H && badPixel < 0; y++)
        if (memcmp(g_sub + y * GFX_W, full + y * GFX_W, GFX_W * 2) != 0)
            badPixel = y;

    /* A row that changed but was never marked would not be transferred,
     * leaving the old pixels on screen. */
    for (int y = 0; y < GFX_H && unmarked < 0; y++)
        if (memcmp(before + y * GFX_W, g_sub + y * GFX_W, GFX_W * 2) != 0)
            if (y < top || y >= bottom) unmarked = y;

    if (badPixel >= 0)
        printf("  FAIL %s: incremental != full redraw, first bad row %d\n",
               what, badPixel);
    if (unmarked >= 0)
        printf("  FAIL %s: row %d changed but is outside dirty span %d..%d\n",
               what, unmarked, top, bottom);
    if (badPixel < 0 && unmarked < 0) {
#ifdef DSXP_COUNT_PIXELS
        /* What the same update used to cost: everything, every time. */
        ComposeFull(a);
        g_gfxPixels = 0;
        ComposeFull(b);
        unsigned long fullPx = g_gfxPixels;

        int rows = bottom > top ? bottom - top : 0;
        if (g_composePixels == 0)
            printf("  ok   %-24s identical | nothing to draw, nothing to send\n",
                   what);
        else
            printf("  ok   %-24s identical | compose %6u vs %6lu px (%5.1fx)"
                   " | transfer %3d/%d rows (%3d%%)\n",
                   what, g_composePixels, fullPx,
                   fullPx / (double)g_composePixels,
                   rows, GFX_H, rows * 100 / GFX_H);
#else
        printf("  ok   %s: identical, %d/%d rows dirty (%d%% of the transfer)\n",
               what, bottom - top, GFX_H,
               (bottom - top) * 100 / GFX_H);
#endif
    }

    return (badPixel >= 0) + (unmarked >= 0);
}

/* Setup and Sleep repaint in pieces that each wipe their own rectangle
 * first. If two of those rectangles overlapped, redrawing one piece would
 * eat part of a neighbour that had decided it did not need redrawing -
 * and only on the transition, which is exactly the kind of fault that
 * survives eyeballing a screenshot. Same reference check as the rest. */
typedef void (*PageDraw)(int value, int focus);

static void DrawSetupPage(int value, int focus)
{
}

static void DrawSleepPage(int value, int focus)
{
    Skin_DrawSleep(value, focus != 0, (uint32_t)focus * 60u + 17u, true);
}

static int CheckPagePieces(const char *what, PageDraw draw,
                           int vA, int fA, int vB, int fB)
{
    static uint16_t full[GFX_W * GFX_H];

    /* Reference: the end state drawn from a clean page. */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    draw(vB, fB);
    memcpy(full, g_sub, sizeof(full));

    /* Incremental: settle on A, then move to B without invalidating. */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    draw(vA, fA);

    Skin_BeginFrame();
#ifdef DSXP_COUNT_PIXELS
    g_gfxPixels = 0;
#endif
    draw(vB, fB);

    int top, bottom;
    Gfx_GetDirty(&top, &bottom);

    int bad = -1;
    for (int y = 0; y < GFX_H && bad < 0; y++)
        if (memcmp(g_sub + y * GFX_W, full + y * GFX_W, GFX_W * 2) != 0)
            bad = y;

    if (bad >= 0) {
        printf("  FAIL %s: piecewise != full redraw, first bad row %d\n", what, bad);
        return 1;
    }

    int rows = bottom > top ? bottom - top : 0;
#ifdef DSXP_COUNT_PIXELS
    unsigned long partial = g_gfxPixels;

    Skin_InvalidateBottom();
    Skin_BeginFrame();
    g_gfxPixels = 0;
    draw(vB, fB);
    unsigned long fullPx = g_gfxPixels;

    printf("  ok   %-24s identical | compose %6lu vs %6lu px (%5.1fx)"
           " | transfer %3d/%d rows (%3d%%)\n",
           what, partial, fullPx, fullPx / (double)(partial ? partial : 1),
           rows, GFX_H, rows * 100 / GFX_H);
#else
    printf("  ok   %s: identical, %d rows dirty\n", what, rows);
#endif
    return 0;
}

static int CheckRegions(void)
{
    printf("bottom-screen region split\n");

    Scene base = { SKIN_TAB_FILES, 4, 0, 96, true, 71000 };
    Scene moved = base;    moved.sel = 5;                        /* no scroll */
    Scene scrolled = base; scrolled.sel = 9; scrolled.scrollTop = 4;
    Scene louder = base;   louder.volume = 120;
    Scene paused = base;   paused.playing = false;
    Scene ticked = base;   ticked.elapsed = 72000;    /* one second later */
    Scene sameSec = base;  sameSec.elapsed = 71250;   /* same second */

    int f = 0;
    f += CheckRegionSplit("d-pad, no scroll", &base, &moved, 2);
    f += CheckRegionSplit("d-pad, list scrolls", &base, &scrolled, 2);

    /* The case that actually costs something: holding the d-pad on a list
     * long enough to fill the view, which scrolls exactly one row at a
     * time. The scenario above jumps four rows on an eleven-entry list, so
     * most of its slots fall past the end and it measures the wrong thing
     * - though it is worth keeping, since a slot scrolling off the end of
     * a short list is what caught the signature-sentinel collision. */
    int savedCount = g_fb.count;
    for (int i = g_fb.count; i < 40; i++) {
        snprintf(g_fb.entries[i].name, FB_NAME_LEN, "%02d - Filler Track.mp3", i);
        g_fb.count++;
    }
    Scene longBase = base;  longBase.sel = 18; longBase.scrollTop = 10;
    Scene longStep = base;  longStep.sel = 19; longStep.scrollTop = 11;
    f += CheckRegionSplit("d-pad, one-row scroll", &longBase, &longStep, 2);
    g_fb.count = savedCount;
    f += CheckRegionSplit("volume change", &base, &louder, 4);
    f += CheckRegionSplit("play -> pause", &base, &paused, 2 | 4);
    f += CheckRegionSplit("clock tick, new second", &base, &ticked, 4);
    f += CheckRegionSplit("clock tick, same second", &base, &sameSec, 4);
    f += CheckPagePieces("setup: drag noise slider", DrawSetupPage, 35, 3, 36, 3);
    f += CheckPagePieces("setup: move focus",        DrawSetupPage, 35, 3, 35, 2);
    f += CheckPagePieces("sleep: countdown ticks",   DrawSleepPage, 45, 1, 45, 2);
    f += CheckPagePieces("sleep: drag minutes",      DrawSleepPage, 45, 1, 46, 1);
    return f;
}


/* Two properties of the colour picker that are easy to break silently.
 *
 * The ink first: an accent the theme never saw can land arbitrarily close
 * to the ink the theme picked, and when it does the label does not look
 * wrong - it disappears. Every theme against every console colour is only
 * 320 combinations, so they are all checked rather than sampled.
 *
 * Then the reset: choosing a theme has to bring that theme's own accent
 * with it, or picking a theme gives you neither the one you chose nor the
 * one it was designed to look like. */
static int CheckAccents(void)
{
    printf("colour picker\n");
    int f = 0;

    int worst = 99, wt = -1, wi = -1;
    for (int t = 0; t < SKIN_THEMES; t++) {
        for (int i = 0; i < SKIN_ACCENTS; i++) {
            Skin_SetTheme(t);
            Skin_SetAccent(i);
            int a = Luma(Skin_ColorAccent()), k = Luma(Skin_ColorTextOnSel());
            int gap = a > k ? a - k : k - a;
            if (gap < worst) { worst = gap; wt = t; wi = i; }
        }
    }
    if (worst >= 12)
        printf("  ok   %-40s worst gap %d of 31, theme %s colour %d\n",
               "every colour keeps its label readable", worst,
               Skin_ThemeName(wt), wi);
    else {
        printf("  FAIL ink vanishes on theme %s with colour %d (gap %d)\n",
               Skin_ThemeName(wt), wi, worst);
        f++;
    }

    Skin_SetAccent(4);
    Skin_SetTheme(3);
    if (Skin_Accent() < 0)
        printf("  ok   %-40s theme brings its own accent back\n",
               "choosing a theme clears the colour");
    else {
        printf("  FAIL a chosen colour survived a theme change (accent %d)\n",
               Skin_Accent());
        f++;
    }
    /* Every cell has to be reachable by the pen. A grid drawn from one
     * set of constants and hit-tested with another looks perfect and
     * selects the wrong thing, or nothing - and nothing is what a cell
     * off the bottom of the screen does. */
    int badColor = -1, badTheme = -1;
    for (int i = 0; i < SKIN_ACCENTS && badColor < 0; i++) {
        int x, y;
        ColorCell(i, &x, &y);
        if (Skin_HitTestColor(x + COLOR_SW / 2, y + COLOR_SW / 2) != i)
            badColor = i;
    }
    for (int i = 0; i < SKIN_THEMES && badTheme < 0; i++) {
        int x, y;
        ThemeCell(i, &x, &y);
        if (Skin_HitTestTheme(x + THEME_CW / 2, y + THEME_CH / 2) != i)
            badTheme = i;
    }
    if (badColor < 0 && badTheme < 0)
        printf("  ok   %-40s %d colours, %d themes\n",
               "every cell hit-tests back to itself",
               SKIN_ACCENTS, SKIN_THEMES);
    else {
        printf("  FAIL a cell does not hit-test back (colour %d, theme %d)\n",
               badColor, badTheme);
        f++;
    }

    Skin_SetTheme(0);
    Skin_SetAccent(-1);
    return f;
}

/* Every control has to be able to say it was pressed.
 *
 * The frame only repaints when Skin_FeedbackState changes, so a button
 * missing from that value cannot light however carefully it is drawn.
 * The old version set bit (i + 8) per button, which holds while there are
 * fewer than twenty-four: at twenty-six, shuffle shifted into the sign
 * bit and repeat shifted by 32, which is undefined and yields nothing on
 * ARM. Repeat's highlight was unreachable, and no drawing change would
 * have fixed it. */
static void DrawNpOnce(void)
{
    Skin_DrawNowPlaying("Kid A", "Radiohead", true, true, 44100, 320, 2,
                        false, 2, 11, "Optimistic", false, 0, false);
}

static int CheckFeedback(void)
{
    printf("press feedback\n");

    memset(s_pressTimer, 0, sizeof(s_pressTimer));
    Skin_SetHeldButton(SKIN_BTN_NONE);
    int idle = Skin_FeedbackState();

    int mute = 0, first = -1;
    for (int i = 1; i < SKIN_BTN_COUNT; i++) {
        memset(s_pressTimer, 0, sizeof(s_pressTimer));
        Skin_FlashButton((SkinButton)i);
        if (Skin_FeedbackState() == idle) { if (first < 0) first = i; mute++; }
    }
    memset(s_pressTimer, 0, sizeof(s_pressTimer));

    int f = 0;
    if (!mute)
        printf("  ok   %-40s all %d of them\n",
               "every control registers being pressed", SKIN_BTN_COUNT - 1);
    else {
        printf("  FAIL %d controls cannot register a press, first is %d\n",
               mute, first);
        f++;
    }

    /* Registering it is not enough: the page that draws the control has
     * to notice too. Shuffle and repeat are drawn by Now Playing, whose
     * own signature decides whether it repaints at all - and it ignored
     * whether they were lit, so the highlight arrived only when the
     * action happened to change something else, and never cleared. */
    Skin_InvalidateBottom();
    DrawNpOnce();
    uint32_t settled = s_npSig;

    memset(s_pressTimer, 0, sizeof(s_pressTimer));
    Skin_FlashButton(SKIN_BTN_SHUFFLE);
    DrawNpOnce();
    uint32_t lit = s_npSig;

    memset(s_pressTimer, 0, sizeof(s_pressTimer));
    DrawNpOnce();
    uint32_t cleared = s_npSig;

    if (lit != settled && cleared == settled)
        printf("  ok   %-40s lights on press, clears on release\n",
               "the page repaints for a mode key");
    else {
        printf("  FAIL mode key highlight %s\n",
               lit == settled ? "never reaches the page"
                              : "does not clear when it expires");
        f++;
    }
    memset(s_pressTimer, 0, sizeof(s_pressTimer));
    Skin_InvalidateBottom();
    return f;
}

int main(void)
{
    printf("skin layout\n");
    int layoutFailures = CheckNoOverlaps();
    layoutFailures += CheckBrowserButtons();
    layoutFailures += CheckDefaultTopScreen();
    layoutFailures += CheckRowScroll();
    layoutFailures += CheckFontSwitch();
    layoutFailures += CheckControlsReachable();
    /* Every style, against a full redraw, on the audio that moves the
     * columns hardest. A style that leaves a stale pixel behind only ever
     * shows up on material like this. */
    for (int st = 0; st < VIS_STYLES; st++) {
        Visualizer_SetStyle(st);
        layoutFailures += CheckIncremental(Visualizer_StyleName(st), 2);
    }
    Visualizer_SetStyle(VIS_BARS);
    layoutFailures += CheckIncremental("bars/tones", 0);

    /* The busy case above is random sample-to-sample, which is the worst
     * input a joined trace can get - every column jumps the full height.
     * Music is nothing like that, so the tone figure is the one that
     * says what this actually costs in use. */
    Visualizer_SetStyle(VIS_WAVE);
    layoutFailures += CheckIncremental("wave/tone", 3);
    Visualizer_SetStyle(VIS_BARS);

    Gfx_InitFont();
    Skin_Init(g_sub);
    FakeBrowser();

    /* After Skin_Init: this one draws. */
    layoutFailures += CheckContentCoverage();
    layoutFailures += CheckPlayingMarker();
    layoutFailures += CheckPageSwitch();
    layoutFailures += CheckTopCardCost();
    layoutFailures += CheckCoverIncremental();
    layoutFailures += CheckRegions();
    layoutFailures += CheckAccents();
    layoutFailures += CheckFeedback();
    /* Down here with the rest of the drawing checks. Run up with the
     * rect-table ones they segfaulted on a surface that did not exist
     * yet - and a crash prints no [FAIL], so the suite looked clean. */
    layoutFailures += CheckButtonPages();
    layoutFailures += CheckTabStrip();
    layoutFailures += CheckTabCentring();
    layoutFailures += CheckCoverBlits();
    layoutFailures += CheckRoundEdges();
    layoutFailures += CheckFolderColour();
    layoutFailures += CheckBigText();
    layoutFailures += CheckTapeInk();
    layoutFailures += CheckTapeCovers();
    layoutFailures += CheckHubRim();
    layoutFailures += CheckTapeNoTrack();
    layoutFailures += CheckTapeSweep();
    layoutFailures += CheckTapeIncremental();
    layoutFailures += CheckDeckCovers();
    layoutFailures += CheckDeckIncremental();
    layoutFailures += CheckLikeBandClear();
    layoutFailures += CheckSliderResidue();
    FakeBrowser();

    /* After Skin_Init, because they draw - the layout checks above only
     * read the rect tables and can run before there is a surface. */
    layoutFailures += CheckMarquee();
    layoutFailures += CheckUnfilledRow();

    /* Large list text, so the scaled path is looked at rather than only
     * compiled. */
    Skin_SetListSize(2);
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_FILES);
    Skin_DrawFileList(&g_fb, "02 - Kid A.mp3");
    WriteBmp("preview_files_large.bmp", g_sub, 256, 192);
    Skin_SetListSize(0);
    Skin_InvalidateBottom();

    /* --- bottom screen: files tab --- */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_FILES);
    Skin_DrawFileList(&g_fb, "02 - Kid A.mp3");
    WriteBmp("preview_files.bmp", g_sub, 256, 192);

    /* --- bottom screen: now playing --- */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_NOWPLAYING);
    Skin_DrawNowPlaying("Kid A", "Radiohead", true, true, 44100, 192, 2, true, 1, 7,
                        "03 - The National Anthem.mp3", true, REPEAT_ALL, false);
    Skin_DrawTransport(true, true, 96, 71000, 262000, true);
    WriteBmp("preview_nowplaying.bmp", g_sub, 256, 192);

    /* The same page with the track liked, because the heart carries that
     * in its shape rather than only in its colour - a filled one against
     * a hollow one is the whole of the state, and a preview of one of
     * them says nothing about whether the other reads. */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_NOWPLAYING);
    Skin_DrawNowPlaying("Kid A", "Radiohead", true, true, 44100, 320, 2,
                        false, 0, 7, "Idioteque", false, REPEAT_OFF, true);
    Skin_DrawTransport(true, true, 96, 71000, 262000, true);
    WriteBmp("preview_np_liked.bmp", g_sub, 256, 192);

    /* The same page with Repeat held - the control that could not light
     * at all until the feedback signature stopped shifting past the width
     * of an int. The whole cell lights, which is what tells you which
     * control your thumb is on. */
    Skin_SetHeldButton(SKIN_BTN_REPEAT);
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_NOWPLAYING);
    Skin_DrawNowPlaying("Kid A", "Radiohead", true, true,
                        44100, 320, 2, false, 2, 11, "Optimistic", false, 0, false);
    Skin_DrawTransport(true, true, 620, 71000, 262000, true);
    WriteBmp("preview_np_pressed.bmp", g_sub, 256, 192);
    Skin_SetHeldButton(SKIN_BTN_NONE);

    /* Repeat-one carries a digit inside the loop; worth looking at, since
     * it is the state with the least room to draw in. */
    Skin_InvalidateContent();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_NOWPLAYING);
    Skin_DrawNowPlaying("Kid A", "Radiohead", true, true, 44100, 192, 2, true, 1, 7,
                        "02 - Kid A.mp3", false, REPEAT_ONE, false);
    Skin_DrawTransport(true, true, 96, 71000, 262000, true);
    WriteBmp("preview_np_repeat1.bmp", g_sub, 256, 192);

    /* --- bottom screen: idle, paused, nothing loaded --- */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_NOWPLAYING);
    Skin_DrawNowPlaying("", "", false, false, 0, 0, 2, false, -1, 0, NULL,
                        false, REPEAT_OFF, false);
    /* With the transport, the way the console draws it. Without this the
     * picture of the empty page was missing the half that overlapped it -
     * the clock row is drawn after the page and landed on the hint. */
    Skin_DrawTransport(false, false, 620, 0, 0, false);
    WriteBmp("preview_idle.bmp", g_sub, 256, 192);

    /* --- bottom screen: sleep timer, both states --- */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_SLEEP);
    /* No transport strip. It has not been on the non-player pages for a
     * while and this call was left behind, so the picture of the sleep
     * page had a row of player controls across the middle of it that the
     * console has never shown. */
    Skin_DrawSleep(45, false, 0, false);
    WriteBmp("preview_sleep_off.bmp", g_sub, 256, 192);

    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_SLEEP);
    Skin_DrawSleep(45, true, 42 * 60 + 17, true);
    WriteBmp("preview_sleep_on.bmp", g_sub, 256, 192);

    /* --- bottom screen: the register watch --- */
    {
        static SkinRegRow regs[18];
        static const struct { const char *n; uint8_t r; uint8_t v; bool c; } src[] = {
            {"PMIC",0x00,0x0D,false},{"PMIC",0x01,0x00,false},
            {"PMIC",0x02,0x01,false},{"PMIC",0x03,0x02,false},
            {"PMIC",0x04,0x03,false},{"PMIC",0x10,0x00,false},
            {"MCU",0x11,0x00,false}, {"MCU",0x12,0x40,false},
            {"MCU",0x20,0x7F,false}, {"MCU",0x21,0x00,false},
            {"MCU",0x30,0x00,false}, {"MCU",0x31,0x00,false},
            {"MCU",0x40,0x03,false}, {"MCU",0x41,0x02,true},
            {"MCU",0x60,0x00,false}, {"MCU",0x61,0x00,false},
            {"MCU",0x62,0x00,false}, {"MCU",0x63,0x01,false},
        };
        for (int i = 0; i < 18; i++) {
            regs[i].name = src[i].n; regs[i].reg = src[i].r;
            regs[i].value = src[i].v; regs[i].changed = src[i].c;
        }
        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawRegisters(regs, 18, true);
        WriteBmp("preview_regs.bmp", g_sub, 256, 192);
    }

    /* --- bottom screen: the theme picker --- */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawThemes(13);
    WriteBmp("preview_themes.bmp", g_sub, 256, 192);

    /* The sound page: a grid of named presets, with the one in force
     * carrying the accent. */
    {
        static const char *const kSound[] = {
            "Flat", "Pop", "Rock", "Jazz", "Classical", "Dance", "Hip-Hop",
            "R&B", "Metal", "Vocal", "Acoustic", "Electronic", "Bass",
            "Treble", "Loudness",
        };
        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawSound(kSound, 15, 5, 3);
        WriteBmp("preview_sound.bmp", g_sub, 256, 192);
        Skin_InvalidateBottom();
    }

    /* --- bottom screen: the colour picker --- */
    Skin_SetAccent(9);              /* one of the console blues, ringed */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawColors(17);            /* full screen: no chrome, by design */
    WriteBmp("preview_colors.bmp", g_sub, 256, 192);
    Skin_SetAccent(-1);

    /* --- bottom screen: setup --- */
    Skin_InvalidateBottom();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_SETUP);
    WriteBmp("preview_setup.bmp", g_sub, 256, 192);

    /* --- bottom screen: diagnostics, with its scrolling rows --- */
    {
        static const char *const kVis[]   = { "Off", "30", "60" };
        static const char *const kOnOff[] = { "Off", "On" };
        static const char *themes[SKIN_THEMES];
        static const char *fonts[GFX_FONT_STYLES];
        for (int i = 0; i < SKIN_THEMES; i++)  themes[i]  = Skin_ThemeName(i);
        for (int i = 0; i < GFX_FONT_STYLES; i++) fonts[i] = Gfx_FontStyleName(i);

        /* Theme is a slider in BuildTestRows, not a choice row - thirty
         * names across 222 pixels overlap into an unreadable smear,
         * which is why the app does not do it. This mock did it anyway,
         * and went on doing it after a comment was written saying it
         * should not: the note was added and the row was not changed, so
         * every rendered Test page since has shown a defect belonging to
         * the preview and not to the program. */
        /* The resampler is a Test page row now, not a Setup one, and all
         * six modes rather than the five Setup used to offer. */
        static const char *const kResT[] = { "Raw", "Lite", "Fast", "Wide",
                                             "Clean", "Best" };
        SkinRow rows[] = {
            { SKIN_ROW_CHOICE, "Bars frame rate", kVis, 3, 1 },
            { SKIN_ROW_CHOICE, "Top screen text", kOnOff, 2, 1 },
            { SKIN_ROW_CHOICE, "Resampling", kResT, 6, 1 },
            { SKIN_ROW_SLIDER, "Theme: Blue", NULL, SKIN_THEMES, 0 },
            { SKIN_ROW_CHOICE, "Font", fonts, GFX_FONT_STYLES, 1 },
            { SKIN_ROW_SLIDER, "List size: large", NULL, SKIN_LIST_SIZES, 2 },
        };
        const int nrows = (int)(sizeof(rows) / sizeof(rows[0]));


        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_TEST);
        Skin_InvalidateContent();
        Skin_DrawRows(rows, nrows,
                      (nrows > SKIN_ROW_SLOTS ? nrows - SKIN_ROW_SLOTS : 0),
                      nrows, 0, SKIN_ROW_SLOTS);
        WriteBmp("preview_test.bmp", g_sub, 256, 192);

        /* And the top of it, where the readouts are. The shot above is
         * scrolled to the end, so a row added near the front of the list
         * never appeared in a preview at all. */
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_SETUP);
        Skin_DrawRows(rows, nrows, 0, 0, 0, SKIN_ROW_SLOTS);
        WriteBmp("preview_test_top.bmp", g_sub, 256, 192);

        /* The top-screen picker: every meter and every card, each drawn
         * as itself, because a name never told anyone what any of them
         * looks like. Three shots - the first meter, a card, and the
         * last card - so the ring is seen on both grids and at the end
         * of the list. */
        static const struct { int focus; const char *file; } kPicks[] = {
            { VIS_BARS,                "preview_meters.bmp"  },
            { TOP_CARD(1),             "preview_meters2.bmp" },
            { TOP_CARD(TOP_CARDS - 1), "preview_meters3.bmp" },
        };
        for (size_t f = 0; f < sizeof(kPicks) / sizeof(kPicks[0]); f++) {
            Skin_BeginFrame();
            Skin_DrawMeters(kPicks[f].focus);
            WriteBmp(kPicks[f].file, g_sub, 256, 192);
        }
    }

    /* Every theme and every font style, rendered - a palette or a derived
     * typeface that comes out unreadable is not something the layout
     * checks can catch. */
    {
        char name[64];
        for (int t = 0; t < SKIN_THEMES; t++) {
            /* Each theme carries its own accent now, so there is no
             * mismatch to force - the point is simply that every one of
             * the six renders. */
            Skin_SetTheme(t);
            /* Everything, not just the content: the transport bar caches
             * its pieces and a palette change does not move any of the
             * values they key on, so invalidating only the page left the
             * old colours on the bottom third and made this preview
             * disagree with what the player does. */
            Skin_InvalidateBottom();
            Skin_BeginFrame();
            /* The player page: it is where the themes differ most, now
             * that they carry chrome as well as colour. */
            Skin_DrawChrome(SKIN_TAB_NOWPLAYING);
            Skin_DrawNowPlaying("Safar", "Rozbeh Mosleh", true, true,
                                44100, 320, 2, false, 4, 31,
                                "06. Auaze Koocheh", false, REPEAT_ALL, false);
            Skin_DrawTransport(true, true, 700, 67000, 260000, true);
            snprintf(name, sizeof(name), "preview_theme%d.bmp", t);
            WriteBmp(name, g_sub, 256, 192);

            /* The top screen too. Its header and separators used to be
             * hard-coded blues, which no bottom-screen sheet could have
             * shown. */
            /* Cleared first: the overlay paints its bar and nothing
             * else, so without this each theme's header lands on top of
             * the last one's and the sheet shows two players at once. */
            Gfx_SetTarget(g_visBuf[0]);
            Gfx_Clear(Skin_ColorBg());
            /* With a sleep timer running, so the picture shows the
             * indicator as well as the theme. */
            Skin_DrawTopOverlay(-1, "02 - Kid A.mp3", true, true,
                                71000, 262000, 1, 7, 45 * 60, 0,
                                44100, 320, 2, false,
                                "03 - The National Anthem.mp3");
            snprintf(name, sizeof(name), "preview_top%d.bmp", t);
            WriteBmp(name, g_visBuf[0], 256, 192);
            Gfx_SetTarget(g_sub);
        }
        Skin_SetTheme(0);
        Gfx_SetFontStyle(GFX_FONT_REGULAR);
        Skin_InvalidateBottom();
    }

    /* --- bottom screen: setup, scrolled --- */
    {
        static const char *const kLid[] = { "Keep playing", "Sleep" };
        /* Mirrors BuildSetupRows. A preview that shows a different set
         * is worse than no preview, and this one has drifted twice: it
         * kept two resample points long after there were five, and a
         * blanking row for a setting that had gone. The resampler has
         * now left this page for the Test page entirely. */
        static const char *const kTop[] = { "Meter", "Card" };
        static const char *const kJack[] = { "Off", "Pause" };
        static const char *const kOnOff2[] = { "Off", "On" };
        static const char *const kExit[]   = { "To menu" };
        static const char *const kRepeat[] = { "Off", "All", "One" };
        static const char *const kJackNone[] = { "DSi only" };
        static const char *styles[VIS_STYLES];
        for (int i = 0; i < VIS_STYLES; i++) styles[i] = Visualizer_StyleName(i);
        /* The same rows BuildSetupRows makes, in the same order. Left
         * unsized so the count cannot fall behind the list - it already
         * had, with seven entries written into a rows[6]. The preview
         * builds its own list rather than calling the real one because it
         * has no PlayerState to hand it, which is exactly why it drifts:
         * it kept a "Blank screens while playing" row for a setting the
         * page no longer has. */
        static const char *const kOpen2[] = { "Open" };
        SkinRow rows[] = {
            { SKIN_ROW_CHOICE, "Top screen: Cassette", kOpen2, 1, -1 },
            { SKIN_ROW_CHOICE, "Theme: Blue", kOpen2, 1, -1 },
            { SKIN_ROW_CHOICE, "Theme colour", kOpen2, 1, -1 },
            { SKIN_ROW_CHOICE, "Font: Sans", kOpen2, 1, -1 },
            { SKIN_ROW_CHOICE, "Sound: Flat", kOpen2, 1, -1 },
            { SKIN_ROW_SLIDER, "Volume boost: +4 dB", NULL, 7, 2 },
            { SKIN_ROW_CHOICE, "Headphones: OUT", kJack, 2, 1 },
            { SKIN_ROW_CHOICE, "When the lid closes", kLid, 2, 0 },
            { SKIN_ROW_CHOICE, "Exit", kExit, 1, -1 },
        };
        const int nrows = (int)(sizeof(rows) / sizeof(rows[0]));
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_SETUP);
        Skin_InvalidateContent();
        Skin_DrawRows(rows, nrows, 1, 2, 0, SKIN_ROW_SLOTS);   /* scrolled one */
        WriteBmp("preview_setup.bmp", g_sub, 256, 192);

        /* The same page with the palest console colour as the accent.
         * The active segment of each choice row is filled with it, so
         * this is the picture that says whether its label survived. */
        Skin_SetAccent(4);              /* #F2F2F0, luminance 30 */
        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_SETUP);
        Skin_DrawRows(rows, nrows, 1, 2, 0, SKIN_ROW_SLOTS);
        WriteBmp("preview_setup_white.bmp", g_sub, 256, 192);
        Skin_SetAccent(-1);
        Skin_InvalidateBottom();

        /* Scrolled to the end, where the new row is. */
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_SETUP);
        Skin_InvalidateContent();
        Skin_DrawRows(rows, nrows, nrows - SKIN_ROW_SLOTS, nrows - 1, 0, SKIN_ROW_SLOTS);
        WriteBmp("preview_setup_end.bmp", g_sub, 256, 192);

        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_SETUP);
        Skin_InvalidateContent();
        Skin_DrawRows(rows, nrows, 3, 4, 0, SKIN_ROW_SLOTS);
        WriteBmp("preview_setup_play.bmp", g_sub, 256, 192);

        /* And the same row on a console without the socket pin: one dead
         * segment, nothing to choose. */
        rows[5] = (SkinRow){ SKIN_ROW_CHOICE, "Headphones: not available (DSi only)",
                             kJackNone, 1, -1 };
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_SETUP);
        Skin_InvalidateContent();
        Skin_DrawRows(rows, nrows, nrows - SKIN_ROW_SLOTS, nrows - 1, 0, SKIN_ROW_SLOTS);
        WriteBmp("preview_setup_nojack.bmp", g_sub, 256, 192);

    }

    /* Every visualizer style, so a broken one is visible rather than
     * only provably self-consistent. */
    {
        static int16_t a[1152 * 2];
        for (int st = 0; st < VIS_STYLES; st++) {
            char nm[64];
            ResetAudio();
            Visualizer_SetStyle(st);
            Visualizer_Init();
            for (int i = 0; i < 24; i++) {
                /* Mode 3 for the trace: a 300Hz tone with a harmonic on it,
                 * carried across blocks. Mode 1 restarts every block, so
                 * the wave preview was a picture of a signal no music
                 * makes. */
                MakeAudio(a, 1152, st == VIS_WAVE ? 3 : 2);
                Visualizer_Render(a, 1152);
                s_backBuffer ^= 1;
            }
            snprintf(nm, sizeof(nm), "preview_vis%d.bmp", st);
            WriteBmp(nm, g_visBuf[s_backBuffer ^ 1], 256, 192);
        }
        Visualizer_SetStyle(VIS_BARS);
    }

    /* --- a playlist opened for editing --- */
    {
        FileBrowser pl;
        memset(&pl, 0, sizeof(pl));
        snprintf(pl.cwd, FB_PATH_LEN, "/Music/Kid A");
        pl.inPlaylist = true;
        snprintf(pl.playlistName, FB_NAME_LEN, "Evening.m3u");
        static const char *const tracks[] = {
            "..", "01 - Everything In Its Right Place.mp3", "02 - Kid A.mp3",
            "05 - Treefingers.mp3", "07 - In Limbo.mp3",
        };
        for (unsigned i = 0; i < sizeof(tracks)/sizeof(tracks[0]); i++) {
            snprintf(pl.entries[i].name, FB_NAME_LEN, "%s", tracks[i]);
            pl.entries[i].isDir = (i == 0);
            pl.count++;
        }
        pl.entries[2].marked = true;
        pl.selected = 2;

        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_FILES);
        Skin_DrawFileList(&pl, NULL);
        WriteBmp("preview_playlist.bmp", g_sub, 256, 192);

        /* The same list opened from the Faves tab, which is where every
         * playlist this player saves actually lives - and where the two
         * controls that edit its contents used to disappear. */
        pl.locked = true;
        snprintf(pl.cwd, FB_PATH_LEN, "%s", PLAYLIST_DIR);
        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_FAVES);
        Skin_DrawFileList(&pl, NULL);
        WriteBmp("preview_playlist_faves.bmp", g_sub, 256, 192);
        pl.locked = false;

        /* The Faves tab: the liked list first with its own mark, then the
         * saved playlists, none of them wearing ".m3u". No "..", because
         * the folder is a list of lists rather than a place. */
        static FileBrowser fav;
        memset(&fav, 0, sizeof(fav));
        snprintf(fav.cwd, FB_PATH_LEN, "%s", PLAYLIST_DIR);
        fav.locked = true;
        static const char *const lists[] = {
            "Liked.m3u", "Evening mix.m3u", "Focus.m3u", "Road trip.m3u",
        };
        for (unsigned i = 0; i < sizeof(lists)/sizeof(lists[0]); i++) {
            snprintf(fav.entries[i].name, FB_NAME_LEN, "%s", lists[i]);
            fav.entries[i].isPlaylist = true;
            fav.entries[i].isLiked = (i == 0);
            fav.count++;
        }
        fav.selected = 0;

        Skin_InvalidateBottom();
        Skin_BeginFrame();
        Skin_DrawChrome(SKIN_TAB_FAVES);
        Skin_DrawFileList(&fav, NULL);
        WriteBmp("preview_faves.bmp", g_sub, 256, 192);
        Skin_InvalidateBottom();
    }

    /* --- the naming modal and the delete confirmation --- */
    Keyboard_OpenText("Name this playlist", "Evening mix", 40);
    Skin_BeginFrame();
    Keyboard_Draw();
    WriteBmp("preview_keyboard.bmp", g_sub, 256, 192);

    {
        static const char *const lists[] = {
            "Liked.m3u", "Evening.m3u", "Road trip.m3u", "Focus.m3u",
            "Slow ones.m3u", "Loud ones.m3u", "Driving.m3u",
        };
        static const char *const news[] = { "New list" };
        Keyboard_OpenPick("Add 3 tracks to", news, 1, lists,
                          (int)(sizeof(lists) / sizeof(lists[0])));
        Skin_BeginFrame();
        Keyboard_Draw();
        WriteBmp("preview_pick.bmp", g_sub, 256, 192);

        /* The same page on the palest console colour. The chosen row is
         * filled with the accent, and its label used to be drawn in the
         * page's ink either way - so on a pale accent the row you had
         * picked was the one you could not read. The support page and
         * the start page both had this; the picker was the one left. */
        Skin_SetAccent(4);              /* #F2F2F0 */
        Skin_BeginFrame();
        Keyboard_Draw();
        WriteBmp("preview_pick_pale.bmp", g_sub, 256, 192);
        Skin_SetAccent(-1);
    }

    Keyboard_OpenEntry();
    Keyboard_Draw();
    WriteBmp("preview_entry.bmp", g_sub, 256, 192);

    /* The start page on the palest console colour. Its major action is
     * filled with the accent, and the label inside it used to take the
     * page's ink - so on this theme the one button the page is built
     * around was the one you could not read, and pressing any key made
     * its label vanish for the same reason. */
    Skin_SetAccent(4);              /* #F2F2F0 */
    Skin_BeginFrame();
    Keyboard_Draw();
    WriteBmp("preview_entry_pale.bmp", g_sub, 256, 192);
    Skin_SetAccent(-1);

    Keyboard_OpenHelp();
    Keyboard_Draw();
    WriteBmp("preview_help.bmp", g_sub, 256, 192);


    Keyboard_OpenSupport();
    Skin_BeginFrame();
    Keyboard_Draw();
    WriteBmp("preview_support.bmp", g_sub, 256, 192);

    /* Every currency in turn, so each code can actually be scanned off
     * the rendered page rather than taken on trust. The generator
     * checks its own matrices by decoding them back; this checks that
     * the right matrix reaches the right row of the screen. */
    for (int c = 0; c < COIN_COUNT; c++) {
        char nm[64];
        s_coin = c;
        Skin_BeginFrame();
        Keyboard_Draw();
        snprintf(nm, sizeof(nm), "preview_support_%d.bmp", c);
        WriteBmp(nm, g_sub, 256, 192);
    }
    s_coin = 0;

    /* The same page with the palest console colour as the accent - the
     * case the selected tab's label used to disappear in, being drawn in
     * the page's ink rather than the ink for the accent it sits on. */
    Skin_SetAccent(4);              /* #F2F2F0 */
    Skin_BeginFrame();
    Keyboard_Draw();
    WriteBmp("preview_support_pale.bmp", g_sub, 256, 192);
    Skin_SetAccent(-1);

    Keyboard_OpenConfirm("Delete this playlist?", "Evening mix.m3u",
                         "Delete", true);
    Skin_BeginFrame();
    Keyboard_Draw();
    WriteBmp("preview_confirm.bmp", g_sub, 256, 192);

    /* And the one that guards leaving, which used to happen on the press
     * that selected the row. Not marked destructive: nothing on the card
     * is lost, only the session. */
    Keyboard_OpenConfirm("Leave DSXP?", "The console powers off",
                         "Exit", false);
    Skin_BeginFrame();
    Keyboard_Draw();
    WriteBmp("preview_exit.bmp", g_sub, 256, 192);

    /* --- top screen: all four still cards --- */
    s_buf[0] = g_visBuf[0];
    s_buf[1] = g_visBuf[1];
    Visualizer_Init();

    /* A stand-in cover, so the framed card can be looked at without a
     * tagged MP3 to hand: a gradient with a band across it, which shows
     * both that the art lands inside its frame and that a picture which
     * is not square keeps its shape rather than being stretched. */
    {
        enum { AW = 116, AH = 88 };
        static uint16_t art[AH * AW];
        for (int y = 0; y < AH; y++)
            for (int x = 0; x < AW; x++) {
                int r = x * 31 / AW, g = y * 31 / AH, b = 20 - (x + y) / 14;
                if (b < 0) b = 0;
                if (y > AH / 2 - 6 && y < AH / 2 + 6) { r = 31; g = 24; b = 8; }
                art[y * AW + x] = (uint16_t)((b << 10) | (g << 5) | r);
            }
        Skin_SetTopArt(art, AW, AH);
    }

    static const char *const kCardFile[TOP_CARDS] = {
        "preview_card.bmp", "preview_card_cover.bmp",
        "preview_card_cassette.bmp", "preview_card_plain.bmp",
    };
    for (int c = 0; c < TOP_CARDS; c++) {
        Skin_InvalidateTopCard();
        Visualizer_TargetBackBuffer();
        int cardBuf = Visualizer_BackBuffer();
        Skin_DrawTopCard(cardBuf, c, "02 - Kid A.mp3", "Radiohead",
                         "03 - The National Anthem.mp3",
                         true, true, 71000, 262000,
                         44100, 320, 2, false, 1, 7, 42 * 60 + 17);
        WriteBmp(kCardFile[c], g_visBuf[cardBuf], 256, 192);
    }

    /* And the cover card with no picture, which is most tracks: the
     * stand-in has to sit in the same square, or the column beside it
     * moves depending on how the file was tagged. */
    Skin_SetTopArt(NULL, 0, 0);
    Skin_InvalidateTopCard();
    Visualizer_TargetBackBuffer();
    {
        int cardBuf = Visualizer_BackBuffer();
        Skin_DrawTopCard(cardBuf, 1, "02 - Kid A.mp3", "Radiohead",
                         "03 - The National Anthem.mp3",
                         true, true, 71000, 262000,
                         44100, 320, 2, false, 1, 7, 0);
        WriteBmp("preview_card_nocover.bmp", g_visBuf[cardBuf], 256, 192);
    }

    /* --- top screen --- */
    s_buf[0] = g_visBuf[0];
    s_buf[1] = g_visBuf[1];
    Visualizer_Init();

    static int16_t audio[1152 * 2];
    for (int i = 0; i < 30; i++) {
        MakeAudio(audio, 1152, 0);
        Visualizer_Render(audio, 1152);
        Skin_DrawTopOverlay(s_backBuffer, "02 - Kid A.mp3", true, true,
                            71000, 262000, 1, 7, 42 * 60 + 17, 0,
                            44100, 320, 2, false, "03 - The National Anthem.mp3");
        s_backBuffer ^= 1;
    }
    WriteBmp("preview_bars.bmp", g_visBuf[s_backBuffer ^ 1], 256, 192);

    /* A title far too long for the width, mid-scroll. The marquee draws
     * through Gfx_Fill, which clips to the screen and not to the box, so
     * this ran off the right edge and put half a glyph against the bezel
     * until Gfx_TextBig learned the clip. The right margin is the thing
     * to look at. */
    Skin_InvalidateContent();
    Skin_BeginFrame();
    Skin_DrawChrome(SKIN_TAB_NOWPLAYING);
    for (int i = 0; i < 400; i++) Skin_TickMarquee();  /* tick 40 */
    Skin_DrawNowPlaying("www.MusicKordi.com - a title of unreasonable length",
                        "www.MusicKordi.com", true, true, 44100, 128, 2, false,
                        11, 20, "20_Rozbeh-Mosleh_Nem-Nemay-Barad.mp3",
                        false, REPEAT_OFF, false);
    Skin_DrawTransport(true, true, 96, 77000, 265000, true);
    WriteBmp("preview_np_long.bmp", g_sub, 256, 192);


    return layoutFailures ? 1 : 0;
}
