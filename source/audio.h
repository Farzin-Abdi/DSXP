#ifndef DSXP_AUDIO_H
#define DSXP_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* PCM output: a maxmod stream fed from a lock-free ring buffer.
 *
 * The decoder (main loop) is the only producer; maxmod's stream callback,
 * which runs on a maxmod-owned thread, is the only consumer. Both live on
 * the ARM9 - maxmod's own ARM7 half, inside calico's stock ds7 core, is
 * what actually drives the DS mixer, and it handles the cross-CPU cache
 * maintenance for its wave buffer itself.
 *
 * 32768Hz is not a style choice: it is the ceiling maxmod documents for
 * mm_stream.sampling_rate. Everything the decoder produces is resampled
 * to it (see decoder_mp3.c). */
#define AUDIO_OUT_RATE     32768

/* A second of slack, and the second half of it was earned the hard way.
 *
 * Half a second was sized for the thing most likely to interrupt playback
 * on real hardware - a slow SD read on a flashcart, not a slow decode -
 * and it absorbs a hitch that a 100ms buffer would turn into an audible
 * dropout. It is not enough for the other case.
 *
 * At 320 kbps the decoder runs at about 95% of real time, measured on
 * hardware with the readout on the Test page. That leaves a few percent
 * for everything else, and a few percent is not much of a margin - which
 * is what this buffer is for.
 *
 * A figure of fifteen percent stood here briefly and was wrong: the
 * clock behind it started after mad_synth_frame, so it timed the
 * resampler and called it the decoder. The two expensive stages, the
 * Huffman decode and the synthesis filter, were outside the measurement
 * entirely. The Test page splits the three now, so that particular
 * mistake cannot be made silently again.
 *
 * 875ms of margin between full and the boost threshold costs 64KB of
 * main RAM, which this has, and a buffer that large turns a small
 * misjudgement into a slow drift rather than a dropout. */
#define AUDIO_RING_FRAMES  32768

/* Brings up maxmod (no soundbank - we only ever stream) and opens the
 * output stream. Returns false if the stream could not be started. */
bool Audio_Init(void);

/* The rate the stream is actually running at, which is AUDIO_OUT_RATE
 * unless something lowered it.
 *
 * Lower is less work in two places at once: the resampler's cost is per
 * output sample, so producing fewer of them a second is proportionally
 * cheaper, and the mixer has fewer to chew through. It is also the only
 * way to make the resampler disappear altogether - at half rate a 44.1kHz
 * file comes out of libmad at 22050, and an output stream at 22050 means
 * there is nothing left to resample.
 *
 * What it costs is bandwidth, and at half rate that bill has already been
 * paid: half rate throws away everything above about eleven kilohertz, so
 * a 22050Hz output takes nothing further.
 *
 * Changing it closes and reopens the maxmod stream, which is audible as a
 * short break. */
uint32_t Audio_OutputRate(void);
bool     Audio_SetOutputRate(uint32_t rate);

/* Producer side. Returns the number of frames actually accepted, which
 * is less than `frames` when the ring is full. */
uint32_t Audio_Write(const int16_t *interleavedStereo, uint32_t frames);

uint32_t Audio_FreeFrames(void);
uint32_t Audio_QueuedFrames(void);

/* Copies up to `frames` of what is about to be played, starting at the
 * play cursor. Returns how many it copied. This is what a meter should
 * be drawn from: the decoder runs a ring's depth ahead of the output. */
uint32_t Audio_PeekPlaying(int16_t *out, uint32_t frames);

/* Drops everything still queued and restarts the played-frame counter.
 * Used on track change and after a seek. */
void Audio_Reset(void);

void Audio_SetPaused(bool paused);
void Audio_SetVolume(int volume0to127);

/* What it was last set to, 0..127. For the screen: the gramophone's
 * plinth carries a slider, and a slider that does not move is a
 * decoration pretending to be a control. */
int  Audio_Volume(void);

/* Extra output gain in whole dB, on top of the volume setting. Anything
 * above 0 can clip loud material - that is the caller's choice, and it is
 * how the resampler's deliberate headroom is handed back to anyone who
 * would rather have the loudness. */
void Audio_SetGainDb(int db);

/* Frames of real audio handed to the mixer since the last Audio_Reset().
 * Silence emitted while paused or underrunning is not counted, so this
 * tracks position within the decoded stream rather than wall-clock time
 * - which is exactly what the elapsed-time display wants. */
uint32_t Audio_PlayedFrames(void);

/* Diagnostics, reset by Audio_Reset() so they describe the current track.
 *
 * `Audio_UnderrunFrames` counts silence the mixer had to invent because
 * the decoder had not produced anything yet; `Audio_DroppedFrames` counts
 * decoded audio thrown away because the ring was full. Both are audible
 * as clicks, and both are otherwise invisible - without a counter there
 * is no way to tell a starved buffer apart from any other noise, so the
 * UI surfaces these when they are non-zero. */
uint32_t Audio_UnderrunFrames(void);

/* Zeroes the glitch counters without disturbing playback, so a
 * measurement can start from a known point. */
void Audio_ResetCounters(void);

/* Silences output without changing the pause state or disturbing the
 * ring. Unmuting fades back in over ~30ms. */
void Audio_SetMuted(bool muted);
uint32_t Audio_DroppedFrames(void);

#endif /* DSXP_AUDIO_H */
