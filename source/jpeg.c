#include "jpeg.h"

#include <stdlib.h>
#include <string.h>

/* ---- the pieces a baseline decode needs ---------------------------- */

#define MAX_COMPS 3

typedef struct {
    /* The canonical form from the spec's DECODE procedure: for each code
     * length, the smallest and largest code of that length and where its
     * values start in `val`. */
    int32_t  mincode[17];
    int32_t  maxcode[17];   /* -1 where no code of that length exists */
    int32_t  valptr[17];
    uint8_t  val[256];

    /* And a shortcut past it.
     *
     * Every code of eight bits or fewer is entered here under every
     * eight-bit string that starts with it, so the common case is one
     * array read instead of a loop that walks a bit at a time. It
     * matters: a 300-pixel cover is some eight thousand blocks and each
     * block is a dozen or more of these, on a processor that has to have
     * finished before the track it belongs to starts playing. Codes
     * longer than eight bits fall through to the loop, and they are the
     * rare ones by construction - Huffman gives the short codes to the
     * symbols that occur most.
     *
     * High byte is the code's length, low byte the symbol; zero means no
     * code that short matches. */
    uint16_t fast[256];
    bool     present;
} Huff;

typedef struct {
    uint8_t id, h, v, tq;
    uint8_t td, ta;
    int     bw, bh;         /* blocks across and down, MCU-padded */
    int     pw, ph;         /* and the same in samples: blocks times n */
    int     dcPred;
    uint8_t *plane;         /* one byte per sample, level-shifted */
} Comp;

typedef struct {
    const uint8_t *p, *end;
    /* Bits are held left-aligned in a 32-bit accumulator and refilled a
     * byte at a time, so peeking eight of them - which is what the
     * Huffman shortcut wants - is a shift and a mask rather than eight
     * calls. */
    uint32_t buf;
    int      nbits;
    bool     hitMarker;     /* a marker was reached: the segment is over */
} BitReader;

typedef struct {
    uint16_t qt[4][64];
    Huff     hdc[4], hac[4];
    Comp     comp[MAX_COMPS];
    int      ncomp;
    int      width, height;
    int      hmax, vmax;
    int      mcusX, mcusY;
    int      n;             /* samples decoded per block edge: 1, 2 or 4 */
    int      boxW, boxH;    /* what it is being decoded for */
    bool     abandoned;     /* the caller's hook asked us to stop */
    int      restart;       /* MCUs between RSTn markers, 0 for none */
    BitReader br;
} Jpeg;

/* The zigzag order, needed only to know which coefficient is the DC and
 * to walk past the rest. */
/* See Jpeg_SetMaxScale. Eight is the whole transform - "as much as
 * the frame asks for", with nothing held back. */
static int s_maxScale = 8;

static bool (*s_yield)(void) = NULL;

void Jpeg_SetYieldHook(bool (*fn)(void)) { s_yield = fn; }

void Jpeg_SetMaxScale(int n)
{
    s_maxScale = (n >= 8) ? 8 : (n >= 4) ? 4 : (n >= 2) ? 2 : 1;
}

static const uint8_t kZigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/* ---- bit reading --------------------------------------------------- */

/* Entropy-coded data has 0xFF stuffed with a following 0x00, and any
 * other 0xFF is the marker that ends the segment. Reaching one is not an
 * error - restart markers are expected, and EOI ends the scan - so the
 * reader sets a flag and feeds zero bits from then on rather than
 * failing, and the caller decides what to do about it. */
static void Fill(BitReader *br, int want)
{
    /* Three bytes at a stroke when none of them is 0xFF.
     *
     * That is almost always: 0xFF is the only byte the stuffing rule
     * cares about, and entropy-coded data is mostly not 0xFF. Taken one
     * at a time this loop ran three times per refill with a branch and a
     * shift each, and refills are the innermost thing in the decoder -
     * every Huffman symbol and every magnitude comes through here, some
     * twenty thousand blocks deep for a cover.
     *
     * Three and not four because the accumulator is 32 bits and has to
     * keep the eight it may already be holding. */
    if (br->nbits <= 8 && !br->hitMarker && br->p + 3 <= br->end) {
        const uint8_t *p = br->p;
        if (p[0] != 0xFF && p[1] != 0xFF && p[2] != 0xFF) {
            br->buf = (br->buf << 24) |
                      ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            br->nbits += 24;
            br->p += 3;
            if (br->nbits >= want) return;
        }
    }

    while (br->nbits < want) {
        uint8_t b = 0;

        if (br->hitMarker || br->p >= br->end) {
            br->hitMarker = true;          /* zeroes, for as long as asked */
        } else {
            b = *br->p++;
            if (b == 0xFF) {
                uint8_t m = (br->p < br->end) ? *br->p : 0xD9;
                if (m == 0x00) {
                    br->p++;               /* a stuffed 0xFF, and a real byte */
                } else {
                    br->hitMarker = true;
                    br->p--;               /* leave the marker for the caller */
                    b = 0;
                }
            }
        }

        br->buf = (br->buf << 8) | b;
        br->nbits += 8;
    }
}

static uint32_t Peek(BitReader *br, int n)
{
    Fill(br, n);
    return (br->buf >> (br->nbits - n)) & ((1u << n) - 1u);
}

static void Drop(BitReader *br, int n) { br->nbits -= n; }

static int Receive(BitReader *br, int n)
{
    if (n <= 0) return 0;
    Fill(br, n);
    br->nbits -= n;
    return (int)((br->buf >> br->nbits) & ((1u << n) - 1u));
}

/* The spec's EXTEND: an n-bit magnitude whose top bit is clear is
 * negative, and the value is biased by the range. */
static int Extend(int v, int n)
{
    if (n == 0) return 0;
    return (v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

static int HuffDecode(BitReader *br, const Huff *h)
{
    const uint32_t look = Peek(br, 8);
    const uint16_t hit = h->fast[look];
    if (hit) {
        Drop(br, hit >> 8);
        return hit & 0xFF;
    }

    /* Longer than eight bits, so the slow walk - starting from the eight
     * already peeked rather than from nothing. */
    int code = (int)look;
    Drop(br, 8);
    for (int len = 9; len <= 16; len++) {
        code = (code << 1) | (int)Receive(br, 1);
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len])
            return h->val[h->valptr[len] + (code - h->mincode[len])];
    }
    return 0;   /* a corrupt table or stream: a zero is the harmless answer */
}

/* ---- header parsing ------------------------------------------------ */

static bool BuildHuff(Huff *h, const uint8_t *counts, const uint8_t *vals,
                      int nvals)
{
    if (nvals < 0 || nvals > 256) return false;

    /* The counts have to describe a real prefix code before any of them
     * is used to index anything.
     *
     * Nothing checked that, and the fast table below is written at
     * `code << (8 - len)`: for a table that is a prefix code, the
     * largest code of a given length cannot exceed 2^len - 1, so that
     * index cannot exceed 255 and the 256-entry table is exactly the
     * right size. For a table that is not, the index is not bounded at
     * all - sixteen bytes in a file saying "255 codes, each one bit
     * long" passed the total-values check above and sent it past 32,000,
     * writing tens of kilobytes off the end of the heap block this
     * struct lives in. The data comes out of an MP3's cover art, so it
     * is whatever somebody's tagger, or a half-finished download, put
     * there.
     *
     * The test is the one the spec's own table-building procedure
     * implies: after the codes of each length are allocated, the running
     * code must still fit in that many bits. Equality is the complete
     * code and is fine; anything more is over-subscribed. */
    {
        int code = 0, total = 0;
        for (int len = 1; len <= 16; len++) {
            code  += counts[len - 1];
            total += counts[len - 1];
            if (code > (1 << len)) return false;
            code <<= 1;
        }
        /* And the values really are as many as the caller counted, so
         * that walking `val` stays inside what was copied into it. */
        if (total != nvals) return false;
    }

    memcpy(h->val, vals, (size_t)nvals);

    memset(h->fast, 0, sizeof(h->fast));

    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len]  = k;
        h->mincode[len] = code;

        /* Every eight-bit string beginning with this code maps to its
         * symbol, so one lookup answers for all of them. */
        if (len <= 8) {
            for (int n = 0; n < counts[len - 1]; n++) {
                const int c = code + n;
                const int lo = c << (8 - len);
                for (int f = 0; f < (1 << (8 - len)); f++)
                    h->fast[lo + f] = (uint16_t)((len << 8) | vals[k + n]);
            }
        }

        code += counts[len - 1];
        k    += counts[len - 1];
        h->maxcode[len] = counts[len - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h->present = true;
    return true;
}

static uint16_t Be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* ---- the reduced inverse transform ---------------------------------- */

/* An n-point inverse DCT over the top-left n by n coefficients gives the
 * picture at n eighths of its size. n = 1 is the DC alone - the mean of
 * the block, and the cheapest correct answer; n = 4 is half size and
 * carries real detail.
 *
 * The scaling is the same at every n, which is the part worth stating.
 * The n-point 2-D inverse carries a factor of 2/n, and matching the
 * eight-point level needs another n/8, and those multiply out to a
 * quarter whatever n is. So one constant serves all three, and the
 * one-point case reduces to F/8 - which is the DC formula, arrived at
 * from the general one rather than asserted beside it.
 *
 * Tables are C(u) cos((2x+1) u pi / 2n) in Q12, generated rather than
 * typed: sixteen cosines written by hand is a table with one wrong
 * entry in it, and a wrong entry looks like a picture. */
static const int kIdct1[1] = { 2896 };
static const int kIdct2[4] = { 2896, 2896, 2896, -2896 };
static const int kIdct4[16] = {
    2896,  3784,  2896,  1567,
    2896,  1567, -2896, -3784,
    2896, -1567, -2896,  3784,
    2896, -3784,  2896, -1567,
};
/* And the whole transform, for a cover too small to have detail to
 * spare. Eight is the ordinary IDCT rather than a reduced one: it costs
 * eight times what four does, but only a picture under about 230 pixels
 * ever asks for it, and for those the alternative is decoding at half
 * and then stretching the result - which is the one case where the scale
 * chosen is visible as softness rather than as speed. */
static const int kIdct8[64] = {
      2896,   4017,   3784,   3406,   2896,   2276,   1567,    799,
      2896,   3406,   1567,   -799,  -2896,  -4017,  -3784,  -2276,
      2896,   2276,  -1567,  -4017,  -2896,    799,   3784,   3406,
      2896,    799,  -3784,  -2276,   2896,   3406,  -1567,  -4017,
      2896,   -799,  -3784,   2276,   2896,  -3406,  -1567,   4017,
      2896,  -2276,  -1567,   4017,  -2896,   -799,   3784,  -3406,
      2896,  -3406,   1567,    799,  -2896,   4017,  -3784,   2276,
      2896,  -4017,   3784,  -3406,   2896,  -2276,   1567,   -799,
};

static void IdctN(const int *F, int n, uint8_t *out, int stride)
{
    const int *T = (n == 8) ? kIdct8 : (n == 4) ? kIdct4
                 : (n == 2) ? kIdct2 : kIdct1;
    int tmp[64];

    /* Rows first, then columns. Separable, so it is 2 n^3 multiplies
     * rather than n^4 - at n = 4 that is 128 against 256, on a processor
     * where this runs eight thousand times for one cover. */
    for (int v = 0; v < n; v++)
        for (int x = 0; x < n; x++) {
            int acc = 0;
            for (int u = 0; u < n; u++) acc += T[x * n + u] * F[v * n + u];
            tmp[v * n + x] = (acc + 2048) >> 12;
        }

    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int acc = 0;
            for (int v = 0; v < n; v++) acc += T[y * n + v] * tmp[v * n + x];

            int s = (((acc + 2048) >> 12) + 2) >> 2;   /* the quarter */
            s += 128;                                  /* the level shift */
            if (s < 0)   s = 0;
            if (s > 255) s = 255;
            out[y * stride + x] = (uint8_t)s;
        }
}

/* ---- the scan ------------------------------------------------------ */

static void DecodeBlock(Jpeg *j, Comp *c, int bx, int by)
{
    const Huff *dc = &j->hdc[c->td];
    const Huff *ac = &j->hac[c->ta];
    const uint16_t *q = j->qt[c->tq];
    const int n = j->n;

    int F[64];
    memset(F, 0, sizeof(F));

    int t = HuffDecode(&j->br, dc);

    /* The category is a count of bits, and it arrives as a symbol out of
     * a Huffman table the file supplied - so it is any byte at all,
     * while only nought to eleven mean anything for eight-bit samples.
     *
     * Used as written it became a shift of up to 255 places, which is
     * undefined, and then a coefficient built from whatever that
     * produced - which is where the arithmetic downstream of it went.
     * A file saying otherwise is corrupt: the block is left at its
     * midpoint and the scan stops, rather than carrying a wrong DC
     * through every block after it, since the predictor is cumulative.
     *
     * The AC categories need no such guard - they are the low four bits
     * of their symbol, so the mask already bounds them. */
    if (t > 11) { j->br.hitMarker = true; t = 0; }

    int diff = t ? Extend(Receive(&j->br, t), t) : 0;
    c->dcPred += diff;
    F[0] = c->dcPred * (int)q[0];

    /* Every AC coefficient is decoded, because a variable-length
     * bitstream cannot be skipped through - but only the top-left n by n
     * of them are kept. The rest are the detail this scale does not
     * carry, and reading them is the price of finding where the next
     * block starts. */
    for (int k = 1; k < 64; ) {
        int rs = HuffDecode(&j->br, ac);
        int s = rs & 15, r = rs >> 4;

        if (s == 0) {
            if (r != 15) break;      /* end of block */
            k += 16;                 /* a run of sixteen zeroes */
            continue;
        }

        k += r;
        if (k > 63) break;

        int val = Extend(Receive(&j->br, s), s);
        const int nat = kZigzag[k];
        const int row = nat >> 3, col = nat & 7;
        if (row < n && col < n) F[row * n + col] = val * (int)q[nat];
        k++;

        if (j->br.hitMarker) break;
    }

    if (bx < c->bw && by < c->bh)
        IdctN(F, n, c->plane + (by * n) * c->pw + bx * n, c->pw);
}

static void ResetPredictors(Jpeg *j)
{
    for (int i = 0; i < j->ncomp; i++) j->comp[i].dcPred = 0;
    j->br.nbits = 0;
    j->br.buf = 0;
    j->br.hitMarker = false;
}

static void DecodeScan(Jpeg *j)
{
    int sinceRestart = 0;

    for (int my = 0; my < j->mcusY; my++) {
        for (int mx = 0; mx < j->mcusX; mx++) {
            if (j->restart && sinceRestart == j->restart) {
                /* Skip to and over the RSTn marker, then start again with
                 * cleared predictors - which is the entire point of one. */
                while (j->br.p + 1 < j->br.end &&
                       !(j->br.p[0] == 0xFF && j->br.p[1] >= 0xD0 && j->br.p[1] <= 0xD7))
                    j->br.p++;
                if (j->br.p + 1 < j->br.end) j->br.p += 2;
                ResetPredictors(j);
                sinceRestart = 0;
            }

            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->comp[i];
                for (int v = 0; v < c->v; v++)
                    for (int h = 0; h < c->h; h++)
                        DecodeBlock(j, c, mx * c->h + h, my * c->v + v);
            }
            sinceRestart++;

            /* A truncated file stops here rather than reading past the
             * end for however many MCUs the header claimed. What has
             * been decoded so far is still shown. */
            if (j->br.hitMarker && j->br.p >= j->br.end) return;
        }

        /* Once a row, not once a block: a row is a few hundred blocks and
         * a handful of milliseconds, which is often enough to notice
         * something else needs the processor, or that this has gone on
         * long enough, and rare enough to cost nothing. */
        if (s_yield && !s_yield()) { j->abandoned = true; return; }
    }
}

/* ---- putting it together ------------------------------------------- */

static uint16_t Rgb555(int r, int g, int b)
{
    if (r < 0)   r = 0;
    if (r > 255) r = 255;
    if (g < 0)   g = 0;
    if (g > 255) g = 255;
    if (b < 0)   b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(0x8000u | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

static void FreePlanes(Jpeg *j)
{
    for (int i = 0; i < MAX_COMPS; i++) {
        free(j->comp[i].plane);
        j->comp[i].plane = NULL;
    }
}

bool Jpeg_Thumbnail(const uint8_t *data, size_t len,
                    uint16_t *out, int boxW, int boxH,
                    int *outW, int *outH)
{
    if (!data || !out || len < 4 || boxW <= 0 || boxH <= 0) return false;
    if (data[0] != 0xFF || data[1] != 0xD8) return false;   /* no SOI */

    /* Heap, not stack: this runs on the decoder's worker thread, whose
     * stack is 16K, and the tables alone are more than a third of it. */
    Jpeg *j = calloc(1, sizeof(*j));
    if (!j) return false;
    j->boxW = boxW;
    j->boxH = boxH;

    const uint8_t *p = data + 2, *end = data + len;
    bool haveFrame = false, ok = false;

    while (p + 4 <= end) {
        if (*p != 0xFF) { p++; continue; }       /* resync on padding */
        uint8_t m = p[1];
        p += 2;
        if (m == 0xFF) { p--; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        if (m == 0xD9) break;                    /* EOI */

        if (p + 2 > end) break;
        int seg = Be16(p);
        if (seg < 2 || p + seg > end) break;
        const uint8_t *sp = p + 2, *sEnd = p + seg;

        switch (m) {
        case 0xDB:                               /* DQT */
            while (sp < sEnd) {
                int prec = *sp >> 4, id = *sp & 15;
                sp++;
                if (id > 3) goto done;
                for (int k = 0; k < 64; k++) {
                    if (sp >= sEnd) goto done;
                    if (prec) { j->qt[id][kZigzag[k]] = Be16(sp); sp += 2; }
                    else      { j->qt[id][kZigzag[k]] = *sp++; }
                }
            }
            break;

        case 0xC4:                               /* DHT */
            while (sp + 17 <= sEnd) {
                int cls = *sp >> 4, id = *sp & 15;
                sp++;
                if (id > 3) goto done;

                int nvals = 0;
                for (int k = 0; k < 16; k++) nvals += sp[k];
                if (sp + 16 + nvals > sEnd) goto done;

                Huff *h = cls ? &j->hac[id] : &j->hdc[id];
                if (!BuildHuff(h, sp, sp + 16, nvals)) goto done;
                sp += 16 + nvals;
            }
            break;

        case 0xDD:                               /* DRI */
            if (sp + 2 <= sEnd) j->restart = Be16(sp);
            break;

        /* Baseline only. C1 is extended sequential, which decodes the
         * same way at eight bits; everything else - progressive,
         * arithmetic, lossless, hierarchical - is refused rather than
         * producing something that looks like a decode and is not. */
        case 0xC0: case 0xC1: {
            if (sp + 6 > sEnd) goto done;
            if (sp[0] != 8) goto done;           /* eight-bit samples only */
            j->height = Be16(sp + 1);
            j->width  = Be16(sp + 3);
            j->ncomp  = sp[5];
            sp += 6;

            if (j->ncomp < 1 || j->ncomp > MAX_COMPS) goto done;
            if (j->width  < 1 || j->width  > JPEG_MAX_SIDE) goto done;
            if (j->height < 1 || j->height > JPEG_MAX_SIDE) goto done;
            if (sp + 3 * j->ncomp > sEnd) goto done;

            for (int i = 0; i < j->ncomp; i++) {
                j->comp[i].id = sp[0];
                j->comp[i].h  = sp[1] >> 4;
                j->comp[i].v  = sp[1] & 15;
                j->comp[i].tq = sp[2];
                if (j->comp[i].h < 1 || j->comp[i].h > 4) goto done;
                if (j->comp[i].v < 1 || j->comp[i].v > 4) goto done;
                if (j->comp[i].tq > 3) goto done;
                if (j->comp[i].h > j->hmax) j->hmax = j->comp[i].h;
                if (j->comp[i].v > j->vmax) j->vmax = j->comp[i].v;
                sp += 3;
            }

            j->mcusX = (j->width  + j->hmax * 8 - 1) / (j->hmax * 8);
            j->mcusY = (j->height + j->vmax * 8 - 1) / (j->vmax * 8);

            /* The smallest scale that still fills the frame it is for.
             *
             * Decoding larger than that is work thrown away twice over:
             * every extra sample is transformed, stored, read back and
             * then averaged out of existence by the fit at the end. A
             * 600-pixel cover going into a 116-pixel frame needs an
             * eighth of nothing and a quarter of its width - a quarter
             * is n = 2, and it was being decoded at n = 4, which is four
             * times the transform and four times the memory for a
             * picture that comes out identical.
             *
             * Measured against the fitted size rather than the box,
             * because a cover that is not square touches only one edge
             * of the box and the other dimension needs correspondingly
             * less.
             *
             * Never below what the frame needs, so a small cover is
             * still decoded as large as it goes rather than being
             * blown up further than it has to be. */
            {
                int fitW, fitH;
                if (j->width * j->boxH >= j->height * j->boxW) {
                    fitW = j->boxW;
                    fitH = j->height * j->boxW / j->width;
                } else {
                    fitH = j->boxH;
                    fitW = j->width * j->boxH / j->height;
                }

                j->n = 1;
                while (j->n < s_maxScale &&
                       ((j->width  * j->n) / 8 < fitW ||
                        (j->height * j->n) / 8 < fitH))
                    j->n <<= 1;
            }

            /* And never so large that the planes stop being small. */
            while (j->n > 1 &&
                   (j->mcusX * j->hmax * j->n > JPEG_PLANE_MAX ||
                    j->mcusY * j->vmax * j->n > JPEG_PLANE_MAX))
                j->n >>= 1;

            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->comp[i];
                c->bw = j->mcusX * c->h;
                c->bh = j->mcusY * c->v;
                c->pw = c->bw * j->n;
                c->ph = c->bh * j->n;
                c->plane = calloc((size_t)c->pw * c->ph, 1);
                if (!c->plane) goto done;
            }
            haveFrame = true;
            break;
        }

        case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            goto done;                            /* not baseline */

        case 0xDA: {                              /* SOS */
            if (!haveFrame || sp >= sEnd) goto done;
            int ns = *sp++;
            if (ns != j->ncomp) goto done;        /* single-scan baseline */
            if (sp + 2 * ns > sEnd) goto done;

            for (int k = 0; k < ns; k++) {
                /* Both table numbers are four bits wide in the file and
                 * there are four tables of each kind, so twelve of the
                 * sixteen values a file can put here are out of range.
                 * They were used as written - the same check three
                 * lines of SOF away guards the quantiser number, and
                 * these two simply never got it - and indexing hdc[15]
                 * reads thousands of bytes past the end of the struct
                 * and then decodes against whatever is there. */
                if ((sp[1] >> 4) > 3 || (sp[1] & 15) > 3) goto done;

                for (int i = 0; i < j->ncomp; i++)
                    if (j->comp[i].id == sp[0]) {
                        j->comp[i].td = sp[1] >> 4;
                        j->comp[i].ta = sp[1] & 15;
                    }
                sp += 2;
            }
            sp += 3;                              /* Ss, Se, Ah/Al */

            /* And every table the scan names was actually defined.
             *
             * An undefined one is all zeroes, which decodes to a flat
             * grey rather than to anything - a picture that is wrong
             * rather than a file that is refused. The flag was being set
             * and never read. */
            for (int i = 0; i < j->ncomp; i++)
                if (!j->hdc[j->comp[i].td].present ||
                    !j->hac[j->comp[i].ta].present) goto done;

            j->br.p = sEnd;
            j->br.end = end;
            ResetPredictors(j);
            DecodeScan(j);
            ok = true;
            goto done;
        }

        default: break;                           /* APPn, COM, anything */
        }

        p = sEnd;
    }

done:
    /* Abandoned counts as failure, and deliberately so: half a picture
     * is worse than none, and the caller has a stand-in to show. */
    if (!ok || !haveFrame || j->abandoned) {
        FreePlanes(j); free(j); return false;
    }

    /* The decoded picture is the luma plane. Its real extent is the
     * picture's size scaled by n eighths and rounded up, which is
     * smaller than the MCU-padded plane - the padding blocks hold
     * whatever the encoder put there and are not part of the picture. */
    const Comp *y = &j->comp[0];
    const int sw = (j->width  * j->n + 7) / 8;
    const int sh = (j->height * j->n + 7) / 8;

    int w, h;
    if (sw * boxH >= sh * boxW) { w = boxW; h = sh * boxW / sw; }
    else                        { h = boxH; w = sw * boxH / sh; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    for (int oy = 0; oy < h; oy++) {
        /* The source rows this output row stands for. Averaged rather
         * than sampled: the decoded picture is usually larger than the
         * frame it goes in, and taking one pixel out of every three
         * turns a photograph into a field of speckle. */
        int y0 = oy * sh / h, y1 = (oy + 1) * sh / h;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh)  y1 = sh;

        for (int ox = 0; ox < w; ox++) {
            int x0 = ox * sw / w, x1 = (ox + 1) * sw / w;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw)  x1 = sw;

            int sum = 0, count = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint8_t *row = y->plane +
                    (sy < y->ph ? sy : y->ph - 1) * y->pw;
                for (int sx = x0; sx < x1; sx++) {
                    sum += row[sx < y->pw ? sx : y->pw - 1];
                    count++;
                }
            }
            int Y = count ? sum / count : 0;

            int cb = 128, cr = 128;
            if (j->ncomp == 3) {
                /* Chroma is its own, coarser grid when the file is
                 * subsampled, so it is sampled by proportion rather than
                 * by the luma index - which would land in the wrong half
                 * of the picture on a 4:2:0 file. Not averaged: the eye
                 * does not resolve colour at this scale, and it would
                 * double the cost of the loop that matters least. */
                const int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
                for (int i = 1; i < 3; i++) {
                    const Comp *c = &j->comp[i];
                    int cx = mx * c->h / j->hmax, cy = my * c->v / j->vmax;
                    if (cx >= c->pw) cx = c->pw - 1;
                    if (cy >= c->ph) cy = c->ph - 1;
                    int v = c->plane[cy * c->pw + cx];
                    if (i == 1) cb = v; else cr = v;
                }
            }

            const int d = cb - 128, e = cr - 128;
            out[oy * w + ox] = Rgb555(Y + ((91881 * e) >> 16),
                                      Y - ((22554 * d + 46802 * e) >> 16),
                                      Y + ((116130 * d) >> 16));
        }
    }

    if (outW) *outW = w;
    if (outH) *outH = h;

    FreePlanes(j);
    free(j);
    return true;
}
