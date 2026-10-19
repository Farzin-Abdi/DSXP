#ifndef DSXP_JPEG_H
#define DSXP_JPEG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The largest decoded plane, in samples on a side.
 *
 * The scale is chosen per picture as the largest that still fits this,
 * so a small cover is decoded at half size and a large one further down.
 * It bounds the working memory - three planes of it in the worst case,
 * which is a file with no chroma subsampling - and the time, since the
 * inverse transform is per sample. */
#define JPEG_PLANE_MAX 320

/* Just enough JPEG to show a cover on the top screen.
 *
 * Baseline sequential only, which is what every tagger writes into an
 * APIC frame. Progressive files are refused rather than half-decoded -
 * they need the whole coefficient array in memory and several passes
 * over it, and this runs on a 67MHz ARM9 with the decoder already using
 * the CPU to keep audio flowing.
 *
 * Decoded at a reduced scale, and the scale is chosen per picture: an
 * n-point inverse transform over the top-left n by n coefficients of
 * each block gives the picture at n eighths of its size. Half size is
 * the usual answer, which for a 300-pixel cover is 150 - about what the
 * screen shows it at, so it arrives near enough pixel for pixel.
 *
 * This began as the DC coefficient alone, which is one eighth and needs
 * no transform at all. It is a correct image and it was too soft to look
 * at: a 300-pixel cover became 37 pixels and was then stretched back up.
 * The transform costs about a fifth of what the entropy decode already
 * costs, which turned out to be a price worth paying.
 *
 * The AC coefficients past the ones kept are still Huffman-decoded,
 * because a variable-length bitstream cannot be skipped through - they
 * are read and dropped.
 */

/* The largest source this will attempt, in pixels on a side.
 *
 * Set from measurement rather than taste. The AC coefficients still have
 * to be Huffman-decoded even though they are discarded, so the cost goes
 * with the number of blocks and with how much is in them. On a desktop
 * host a deliberately noisy 600-pixel cover decodes in 4.3ms and a
 * 1000-pixel one in 11.8ms; this processor is something like two orders
 * of magnitude slower, which puts those at roughly half a second and a
 * second and a half.
 *
 * Half a second, once, while the track is already opening, is a cost
 * worth paying for a cover. A second and a half is not. A thousand and
 * twenty-four is where that line falls, and a picture larger than it is
 * refused immediately rather than stalling the start of a track. */
#define JPEG_MAX_SIDE 1024

/* Decodes into `out`, which must hold boxW * boxH pixels.
 *
 * The result is fitted inside the box with its shape kept - a wide cover
 * comes back wide - and its real size is written to `outW`/`outH`. Only
 * that many pixels of `out` are touched, laid out `*outW` to the row.
 *
 * False if the data is not a baseline JPEG this understands, if it is
 * larger than JPEG_MAX_SIDE, or if the working memory could not be had.
 * Nothing is written to `out` in that case. */
bool Jpeg_Thumbnail(const uint8_t *data, size_t len,
                    uint16_t *out, int boxW, int boxH,
                    int *outW, int *outH);

/* Caps how much of each block is transformed: 1, 2 or 4 samples a side,
 * which is an eighth, a quarter or a half of the picture's size.
 *
 * The scale is normally chosen from the frame the picture is going into,
 * and this only lowers it. Lower is coarser and much cheaper - not
 * because the transform itself is large, but because the plane it writes
 * shrinks by the square, and on a processor with four kilobytes of data
 * cache the plane is what evicts the Huffman tables. */
void Jpeg_SetMaxScale(int n);

/* Called once per row of blocks while a picture is being decoded.
 *
 * A decode is one long stretch of arithmetic - the better part of a
 * second on this processor - and a thread holding a useful priority for
 * that long is a thread that has stopped sharing. The hook is where the
 * caller gets to look up, see whether anything more important has fallen
 * behind, and stand down for a moment if it has.
 *
 * Returning false abandons the decode: Jpeg_Thumbnail gives up and
 * reports failure, and nothing is written to the caller's buffer. That is
 * the only way to bound how long a picture can take, because the entropy
 * decode is proportional to how much data is in the file and there is no
 * shortening it - a large picture is simply a lot of bits to walk.
 *
 * NULL for none, which is what the tests use. */
void Jpeg_SetYieldHook(bool (*fn)(void));

#endif /* DSXP_JPEG_H */
