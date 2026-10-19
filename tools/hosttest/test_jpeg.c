/* The JPEG decoder, against files an actual encoder produced.
 *
 * Written from the spec, which is exactly the kind of code that looks
 * right and decodes to noise: a Huffman table built one entry out, a
 * quantiser read in the wrong order, chroma sampled from the luma grid.
 * None of that shows up as a failure - it shows up as a picture, and
 * only if somebody looks at it.
 *
 * So these check the colours that come back. The fixtures are flat
 * quadrants, which survive one-eighth scaling exactly: whatever the DC
 * of a block in the red quadrant is, it is red. A decoder with its
 * chroma planes crossed gives blue where red should be and passes any
 * test that only asks whether it decoded something.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "jpeg.c"

static int g_fail = 0;
static void Check(const char *what, bool ok, const char *detail)
{
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

static uint8_t *Slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static void Unpack(uint16_t p, int *r, int *g, int *b)
{
    *r = ((p >> 0)  & 31) * 255 / 31;
    *g = ((p >> 5)  & 31) * 255 / 31;
    *b = ((p >> 10) & 31) * 255 / 31;
}

/* Within tolerance of a colour. Generous, because the fixture went
 * through a lossy encoder and comes back as block averages - the test is
 * "is this red", not "is this exactly 220,40,40". */
static bool Near(uint16_t px, int wr, int wg, int wb, int slack)
{
    int r, g, b;
    Unpack(px, &r, &g, &b);
    return abs(r - wr) <= slack && abs(g - wg) <= slack && abs(b - wb) <= slack;
}

static const char *g_dir;

static uint8_t *Fixture(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    return Slurp(path, len);
}

int main(int argc, char **argv)
{
    g_dir = argc > 1 ? argv[1] : ".";
    printf("jpeg\n");

    static uint16_t out[128 * 128];
    char buf[200];

    /* Four flat quadrants: red, green, blue, yellow, clockwise from the
     * top left. Every one of them is checked, because a decoder that has
     * Cb and Cr the wrong way round gets two of the four right. */
    {
        size_t len = 0;
        uint8_t *d = Fixture("quad.jpg", &len);
        if (!d) { Check("the quadrant fixture is there", false, "quad.jpg"); return 1; }

        int w = 0, h = 0;
        bool ok = Jpeg_Thumbnail(d, len, out, 96, 96, &w, &h);
        snprintf(buf, sizeof(buf), "%dx%d", w, h);
        Check("a baseline JPEG decodes", ok, buf);
        Check("a square source stays square", ok && w == 96 && h == 96, buf);

        if (ok) {
            const uint16_t tl = out[(h / 4) * w + w / 4];
            const uint16_t tr = out[(h / 4) * w + 3 * w / 4];
            const uint16_t bl = out[(3 * h / 4) * w + w / 4];
            const uint16_t br = out[(3 * h / 4) * w + 3 * w / 4];

            int r, g, b;
            Unpack(tl, &r, &g, &b);
            snprintf(buf, sizeof(buf), "top left %d,%d,%d", r, g, b);
            Check("the red quadrant comes back red", Near(tl, 220, 40, 40, 40), buf);

            Unpack(tr, &r, &g, &b);
            snprintf(buf, sizeof(buf), "top right %d,%d,%d", r, g, b);
            Check("the green quadrant comes back green", Near(tr, 40, 200, 60, 40), buf);

            Unpack(bl, &r, &g, &b);
            snprintf(buf, sizeof(buf), "bottom left %d,%d,%d", r, g, b);
            Check("the blue quadrant comes back blue", Near(bl, 50, 60, 220, 40), buf);

            Unpack(br, &r, &g, &b);
            snprintf(buf, sizeof(buf), "bottom right %d,%d,%d", r, g, b);
            Check("the yellow quadrant comes back yellow", Near(br, 230, 220, 40, 40), buf);
        }
        free(d);
    }

    /* Twice as wide as it is tall. The frame it goes in is square, so a
     * decoder that ignores the shape would hand back a square and the
     * cover would be stretched. */
    {
        size_t len = 0;
        uint8_t *d = Fixture("wide.jpg", &len);
        int w = 0, h = 0;
        bool ok = d && Jpeg_Thumbnail(d, len, out, 96, 96, &w, &h);
        snprintf(buf, sizeof(buf), "320x160 -> %dx%d", w, h);
        Check("a wide source keeps its shape", ok && w == 96 && h == 48, buf);
        free(d);
    }

    /* A gradient, which is the case the flat fixtures cannot catch: it
     * has AC energy in every block, so the bitstream only stays in step
     * if the AC coefficients are being walked past correctly. Get that
     * wrong and the image shears - the last row is where it shows. */
    {
        size_t len = 0;
        uint8_t *d = Fixture("grad.jpg", &len);
        int w = 0, h = 0;
        bool ok = d && Jpeg_Thumbnail(d, len, out, 64, 64, &w, &h);
        Check("a gradient decodes", ok, NULL);

        if (ok) {
            /* Red rises with x and green with y, so the corners are
             * known and the far one is the one that only comes out right
             * if the whole scan stayed in step. */
            int r0, g0, b0, r1, g1, b1;
            Unpack(out[0], &r0, &g0, &b0);
            Unpack(out[(h - 1) * w + (w - 1)], &r1, &g1, &b1);
            snprintf(buf, sizeof(buf), "first %d,%d,%d  last %d,%d,%d",
                     r0, g0, b0, r1, g1, b1);
            Check("it runs dark to light across the whole scan",
                  r0 < 60 && g0 < 60 && r1 > 195 && g1 > 195, buf);
        }
        free(d);
    }

    /* Detail finer than a block, which is the whole reason there is an
     * inverse transform here at all.
     *
     * The fixture is a four-pixel checkerboard, so every eight by eight
     * block contains a two by two checker whose mean is flat grey. A
     * decode that keeps only the DC coefficient returns that mean and
     * nothing else - a uniform grey field - and passes every other test
     * in this file, because the colours and the shape are all still
     * right. Only this one can tell the two apart.
     *
     * Decoded into a box big enough to hold the half-size image without
     * averaging it back away: at 128 the source's 256 pixels come down
     * by two, and a checker of four survives that. */
    {
        size_t len = 0;
        uint8_t *d = Fixture("checker.jpg", &len);
        int w = 0, h = 0;
        bool ok = d && Jpeg_Thumbnail(d, len, out, 128, 128, &w, &h);
        Check("a checkerboard decodes", ok, NULL);

        if (ok) {
            /* How far the picture actually swings. A DC-only decode of
             * this fixture is flat, so the spread between its darkest
             * and lightest pixel is nearly nothing; a real transform
             * keeps most of the original 20-to-235. */
            int lo = 255, hi = 0;
            for (int i = 0; i < w * h; i++) {
                int r, g, b;
                Unpack(out[i], &r, &g, &b);
                if (g < lo) lo = g;
                if (g > hi) hi = g;
            }
            snprintf(buf, sizeof(buf), "%dx%d, luma spans %d..%d", w, h, lo, hi);
            Check("and keeps detail finer than one block", hi - lo > 120, buf);
        }
        free(d);
    }

    /* The four transform tables, against the formula they are the
     * cosines of.
     *
     * These are the one part of the decoder a picture cannot check. A
     * wrong entry moves one column of every block by however wrong it
     * is, and the high-order ones barely move anything at all on a
     * fixture without detail at that frequency - so a swapped pair sails
     * through a decode that is then compared against another decode,
     * because neither picture had much to say at u = 7.
     *
     * Written out rather than generated at build time, so what is
     * checked here is the arithmetic in the file: C(u) cos((2x+1) u pi /
     * 2n) in Q12, with C(0) one over root two. Sixty-four of those typed
     * by hand is a table with an entry in it that is nearly right. */
    {
        static const struct { const int *t; int n; const char *what; } kT[] = {
            { kIdct1, 1, "one" }, { kIdct2, 2, "two" },
            { kIdct4, 4, "four" }, { kIdct8, 8, "eight" },
        };

        int wrong = 0, worst = 0;
        const char *where = "";
        for (size_t i = 0; i < sizeof(kT) / sizeof(kT[0]); i++) {
            const int n = kT[i].n;
            for (int x = 0; x < n; x++)
                for (int u = 0; u < n; u++) {
                    const double c = u ? 1.0 : 1.0 / sqrt(2.0);
                    const double v = 4096.0 * c *
                        cos((2 * x + 1) * u * M_PI / (2.0 * n));
                    const int want = (int)lround(v);
                    const int off = abs(kT[i].t[x * n + u] - want);
                    if (off > worst) { worst = off; where = kT[i].what; }
                    if (off) wrong++;
                }
        }

        snprintf(buf, sizeof(buf), "%d of 85 entries off, worst %d in the %s",
                 wrong, worst, worst ? where : "-");
        Check("the transform tables are the cosines they claim",
              wrong == 0, buf);
    }

    /* The whole transform, for a picture with no detail to spare.
     *
     * The decoder grows its scale until the picture is at least as large
     * as the frame it is going into and then stops, so a big cover never
     * reaches this path and never pays for it. A small one does: at
     * quarter scale a 300 pixel cover decoded to 75 and was stretched to
     * 116, which is the one case where the scale chosen shows as a soft
     * picture rather than as a quick one.
     *
     * Eight is also the one table that cannot be checked by looking at
     * the colours. Every reduced scale is exact on a flat block and this
     * one is not used unless there is detail, so the fixture is the
     * checkerboard and what is measured is whether the light squares
     * come back light and in the right places. A table with one entry
     * wrong still averages to the same block mean - the basis functions
     * sum to zero across a block whatever their values - so a test that
     * compared means would pass on a broken transform. This compares
     * each pixel against where the checker should be. */
    {
        static uint16_t big[256 * 256];
        size_t len = 0;
        uint8_t *d = Fixture("checker.jpg", &len);
        int w = 0, h = 0;

        Jpeg_SetMaxScale(8);
        bool ok = d && Jpeg_Thumbnail(d, len, big, 256, 256, &w, &h);

        snprintf(buf, sizeof(buf), "%dx%d out of a 256 source", w, h);
        Check("a small picture decodes at full scale", ok && w == 256, buf);

        /* Checked against the scale below it rather than against a
         * description of the fixture.
         *
         * Both transforms are approximating the same picture, so the
         * whole one averaged two-to-one has to land on the quarter one -
         * and the quarter table is the one the colour tests above
         * already stand on. This is strict where the obvious test is
         * not: a single wrong entry moves one column of every block,
         * which is an eighth of the picture, and an eighth of the
         * picture is invisible in an average over the whole of it. */
        static uint16_t ref[128 * 128];
        int rw = 0, rh = 0;
        Jpeg_SetMaxScale(4);
        const bool refOk = d && Jpeg_Thumbnail(d, len, ref, 128, 128, &rw, &rh);

        if (ok && w == 256 && refOk && rw == 128) {
            long sum = 0;
            int worst = 0;
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) {
                    int acc = 0;
                    for (int q = 0; q < 4; q++) {
                        int r, g, b;
                        Unpack(big[(y * 2 + (q >> 1)) * w + x * 2 + (q & 1)],
                               &r, &g, &b);
                        acc += g;
                    }
                    int r, g, b;
                    Unpack(ref[y * rw + x], &r, &g, &b);

                    const int off = abs(acc / 4 - g);
                    sum += off;
                    if (off > worst) worst = off;
                }

            const int mean = (int)(sum / (rw * rh));
            snprintf(buf, sizeof(buf), "mean %d, worst %d of 255", mean, worst);
            Check("and agrees with the scale below it", mean < 12, buf);
        } else {
            Check("and agrees with the scale below it", false, "no reference");
        }

        /* Left as the decoder found it - the tests after this one are
         * about other things and must not inherit a scale. */
        Jpeg_SetMaxScale(8);
        free(d);
    }

    /* Things that are not a baseline JPEG are refused rather than
     * half-decoded. A truncated file is the interesting one: it has a
     * valid header and no scan to speak of, and must not read past the
     * end of the buffer chasing the MCU count the header promised. */
    {
        Check("a buffer that is not a JPEG is refused",
              !Jpeg_Thumbnail((const uint8_t *)"not a jpeg at all", 17,
                              out, 96, 96, NULL, NULL), NULL);

        size_t len = 0;
        uint8_t *d = Fixture("quad.jpg", &len);
        if (d) {
            int w = 0, h = 0;
            /* Half the file. It should either refuse or come back with
             * whatever it managed - what it must not do is run off the
             * end, which is what the sanitizer build is here to catch. */
            bool ran = Jpeg_Thumbnail(d, len / 2, out, 96, 96, &w, &h);
            snprintf(buf, sizeof(buf), "%d bytes of %d -> %s",
                     (int)(len / 2), (int)len, ran ? "partial image" : "refused");
            Check("a truncated file does not run off the end", true, buf);
            free(d);
        }
    }

    /* The two ways a header could aim the decoder outside its own
     * memory, each as the exact bytes that did it.
     *
     * Cover art comes out of an MP3 off the card, so every number in it
     * is somebody else's. These two were used as written. */
    {
        /* A Huffman table whose code lengths are not a prefix code: 255
         * codes said to be one bit long. The total number of values is
         * 255, so the only check there was passed, and the fast-lookup
         * table was then written at an index built from the code - tens
         * of kilobytes past the end of the heap block the decoder state
         * lives in. */
        uint8_t dht[512]; size_t n = 0;
        dht[n++] = 0xFF; dht[n++] = 0xD8;
        dht[n++] = 0xFF; dht[n++] = 0xC4;
        size_t segAt = n; n += 2;
        dht[n++] = 0x00;                       /* class 0, table 0 */
        dht[n++] = 255;                        /* 255 one-bit codes */
        for (int i = 1; i < 16; i++) dht[n++] = 0;
        for (int i = 0; i < 255; i++) dht[n++] = (uint8_t)i;
        const int seg = (int)(n - segAt);
        dht[segAt] = (uint8_t)(seg >> 8); dht[segAt + 1] = (uint8_t)seg;
        dht[n++] = 0xFF; dht[n++] = 0xD9;

        int w = 0, h = 0;
        bool ran = Jpeg_Thumbnail(dht, n, out, 96, 96, &w, &h);
        Check("an impossible Huffman table is refused, not built", !ran, NULL);
    }

    {
        /* A scan naming Huffman table 15, of which there are four. The
         * quantiser number three lines away in the frame header is
         * range-checked; these two were not, and hdc[15] is thousands of
         * bytes past the struct. */
        size_t len = 0;
        uint8_t *d = Fixture("quad.jpg", &len);
        if (d) {
            int patched = -1;
            for (size_t i = 0; i + 5 < len; i++) {
                if (d[i] != 0xFF || d[i + 1] != 0xDA) continue;
                d[i + 5 + 1] = 0xFF;      /* first component: td = ta = 15 */
                patched = (int)i;
                break;
            }
            int w = 0, h = 0;
            bool ran = Jpeg_Thumbnail(d, len, out, 96, 96, &w, &h);
            snprintf(buf, sizeof(buf), "scan at %d", patched);
            Check("a scan naming a table that cannot exist is refused",
                  patched >= 0 && !ran, buf);
            free(d);
        }
    }

    /* And the class rather than the two instances.
     *
     * Every byte of a real file, flipped in turn across a fixed sweep,
     * and every result decoded. Nothing is asserted about the pictures -
     * a mangled file may decode to anything or to nothing, and both are
     * correct answers. What is being checked is that none of it reads or
     * writes outside its own memory, which this harness is built with
     * the address and undefined-behaviour sanitizers to notice.
     *
     * Deterministic, so a failure is reproducible: the same seed walks
     * the same bytes with the same values every run. */
    {
        static const char *const kFixtures[] = {
            "quad.jpg", "grad.jpg", "checker.jpg", "wide.jpg",
        };
        const int kTrials = 2000;
        int decoded = 0, refused = 0, files = 0;

        for (unsigned fx = 0; fx < sizeof(kFixtures) / sizeof(kFixtures[0]); fx++) {
        size_t len = 0;
        uint8_t *src = Fixture(kFixtures[fx], &len);

        if (src && len > 64) {
            files++;
            uint8_t *d = malloc(len);
            uint32_t rng = 0x5EED1234u + fx * 7919u;

            for (int trial = 0; trial < kTrials; trial++) {
                memcpy(d, src, len);

                /* One to four bytes, weighted towards the header, which
                 * is where the numbers that index things live. */
                const int hits = 1 + (int)(rng % 4);
                for (int k = 0; k < hits; k++) {
                    rng = rng * 1664525u + 1013904223u;
                    size_t at = (rng >> 8) % ((trial & 1) ? len : (len / 4 + 1));
                    rng = rng * 1664525u + 1013904223u;
                    d[at] = (uint8_t)(rng >> 16);
                }

                int w = 0, h = 0;
                if (Jpeg_Thumbnail(d, len, out, 96, 96, &w, &h)) decoded++;
                else                                             refused++;

                rng = rng * 1664525u + 1013904223u;
            }
            free(d);
            free(src);
        }
        }

        snprintf(buf, sizeof(buf), "%d files, %d decoded, %d refused",
                 files, decoded, refused);
        Check("mangled files stay inside their own memory",
              files == 4 && decoded + refused == files * kTrials, buf);
    }

    printf("\n%s\n", g_fail ? "FAILURES" : "ALL PASSED (0 failures)");
    return g_fail ? 1 : 0;
}
