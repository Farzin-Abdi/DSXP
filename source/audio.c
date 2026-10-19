#include "audio.h"

#include <nds.h>
#include <maxmod9.h>
#include <string.h>

/* Priority for maxmod's stream-filling thread. Numerically lower is
 * higher priority in calico, and the main thread sits at
 * MAIN_THREAD_PRIO (0x1c/28), so this has to be below that number to be
 * able to preempt drawing and MP3 decode. 0x10 leaves plenty of room
 * above it for calico's own system threads. */
#define AUDIO_THREAD_PRIO 0x10

/* Single-producer / single-consumer ring of interleaved 16-bit stereo
 * frames. The producer is the main loop; the consumer is the stream
 * callback, which maxmod runs on a thread of its own. */
static int16_t s_ring[AUDIO_RING_FRAMES * 2];
static volatile uint32_t s_writeIndex = 0;   /* frames, free-running, masked on use */
static volatile uint32_t s_readIndex  = 0;
static volatile uint32_t s_playedFrames = 0;
static volatile uint32_t s_underrunFrames = 0;
static volatile uint32_t s_droppedFrames = 0;

static volatile bool s_paused = true;

/* Volume is ramped rather than applied instantly. Changing a multiplier
 * between one sample and the next is a step discontinuity, and a step in
 * a waveform is a click - which is what you hear when nudging the volume
 * or dragging the slider. `s_volumeTarget` is written by the UI thread;
 * `s_volumeCurrent` belongs to the callback and walks toward it. */
static volatile int s_volumeTarget = 128;    /* 0..128, 128 == unity */
static int s_volumeCurrent = 128;

/* Silences the output without touching the ring or the pause state.
 *
 * Held for the length of a seek. A scrub resets the ring on every step,
 * so what actually reached the speaker was a run of half-filled buffers
 * and underruns - fragments of the old position, clicks between them.
 * Clean silence is what a seek is supposed to sound like, and it is also
 * the only version that leaves the buffer intact for the moment the user
 * lets go. */
static volatile bool s_muted = false;
static int s_volume0to127 = 127;
static int s_gainNum = 16;                   /* gain as s_gainNum/16 */
static bool s_streamOpen = false;

/* The rate the stream is open at. AUDIO_OUT_RATE is the default and the
 * ceiling maxmod documents; nothing may exceed it. */
static uint32_t s_outRate = AUDIO_OUT_RATE;

uint32_t Audio_OutputRate(void) { return s_outRate; }

#define RING_MASK (AUDIO_RING_FRAMES - 1)

/* The mask is the whole indexing scheme, and it only works on a power
 * of two. Nothing said so: the size is a plain number in audio.h that
 * looks like it could be tuned down to save a few kilobytes, and the
 * first thing a value like 30000 would do is wrap the write index into
 * the middle of the buffer and shred the audio in a way nothing points
 * back here from. */
_Static_assert((AUDIO_RING_FRAMES & (AUDIO_RING_FRAMES - 1)) == 0,
               "AUDIO_RING_FRAMES must be a power of two - RING_MASK "
               "indexes the ring with it");

/* Free-running indices masked only at access time: the difference is
 * then always the true fill level, so the buffer can hold all
 * AUDIO_RING_FRAMES frames instead of losing one to the
 * full/empty ambiguity. */
static inline uint32_t QueuedUnsafe(void) { return s_writeIndex - s_readIndex; }

uint32_t Audio_QueuedFrames(void) { return QueuedUnsafe(); }

/* What is about to come out of the speakers, rather than what was just
 * decoded.
 *
 * The decoder runs ahead of the output by however deep the ring is - up
 * to a second of it - so a meter fed from the newest decoded frame shows
 * the future. Reading from the play cursor instead shows the present.
 *
 * The mixer advances that cursor from an interrupt while this copies, so
 * the tail of a long read can be a few frames stale. For a level meter
 * that is invisible, and it is the reason this takes no lock: blocking
 * the decoder to draw a bar would be a far worse trade. */
uint32_t Audio_PeekPlaying(int16_t *out, uint32_t frames)
{
    uint32_t queued = QueuedUnsafe();
    if (frames > queued) frames = queued;
    if (!frames) return 0;

    uint32_t pos = s_readIndex & RING_MASK;
    uint32_t first = AUDIO_RING_FRAMES - pos;
    if (first > frames) first = frames;

    memcpy(out, &s_ring[pos * 2], first * 2 * sizeof(int16_t));
    if (first < frames)
        memcpy(out + first * 2, s_ring, (frames - first) * 2 * sizeof(int16_t));
    return frames;
}
uint32_t Audio_FreeFrames(void)   { return AUDIO_RING_FRAMES - QueuedUnsafe(); }
uint32_t Audio_PlayedFrames(void)   { return s_playedFrames; }
void Audio_SetMuted(bool muted)
{
    if (muted == s_muted) return;

    /* Coming back, restart the ramp from zero so the first buffer eases
     * in over about thirty milliseconds instead of arriving as a step -
     * a step into full-scale audio is an audible click. */
    if (!muted) s_volumeCurrent = 0;
    s_muted = muted;
}

void Audio_ResetCounters(void)
{
    s_underrunFrames = 0;
    s_droppedFrames = 0;
}

uint32_t Audio_UnderrunFrames(void) { return s_underrunFrames; }
uint32_t Audio_DroppedFrames(void)  { return s_droppedFrames; }

uint32_t Audio_Write(const int16_t *interleavedStereo, uint32_t frames)
{
    uint32_t space = Audio_FreeFrames();
    if (frames > space) {
        s_droppedFrames += frames - space;
        frames = space;
    }

    uint32_t head = s_writeIndex & RING_MASK;
    uint32_t firstRun = AUDIO_RING_FRAMES - head;
    if (firstRun > frames) firstRun = frames;

    memcpy(&s_ring[head * 2], interleavedStereo, firstRun * 2 * sizeof(int16_t));
    if (frames > firstRun)
        memcpy(&s_ring[0], interleavedStereo + firstRun * 2,
               (frames - firstRun) * 2 * sizeof(int16_t));

    /* Publish the samples before the index that advertises them. Same
     * core, so this is a compiler barrier rather than a cache one. */
    __asm__ volatile("" ::: "memory");
    s_writeIndex += frames;

    return frames;
}

void Audio_Reset(void)
{
    /* Both indices move here, and the consumer owns one of them, so this
     * has to be atomic with respect to the callback. The callback runs on
     * a maxmod thread, and calico only ever switches threads out of an
     * interrupt, so masking interrupts is enough to exclude it. Without
     * this, a callback landing mid-reset writes back its own advanced
     * readIndex afterwards and the ring comes back holding half a second
     * of the previous track. */
    int oldIME = enterCriticalSection();
    s_readIndex = s_writeIndex;
    s_playedFrames = 0;
    s_underrunFrames = 0;
    s_droppedFrames = 0;
    leaveCriticalSection(oldIME);
}

void Audio_SetPaused(bool paused) { s_paused = paused; }

/* 16 * 10^(dB/20), so the gain multiply stays integer. Tabulated because
 * a pow() per volume change is silly for 21 fixed values. */
static const int kGainNum[] = {
     16,  18,  20,  23,  25,  28,  32,  36,  40,  45,  51,
     57,  64,  71,  80,  90, 101, 113, 127, 143, 160
};

/* Rounds a peak off instead of shearing it.
 *
 * Above unity the mixer had a hard clamp, which is the one thing a
 * player must not do: a sheared peak is a step discontinuity, and a step
 * is odd harmonics all the way up the band. That is heard as grit rather
 * than as loudness, and at the gain this shipped with it was happening
 * to about one sample in a thousand on ordinary material - continuously,
 * on every loud passage. The ReplayGain specification says the same
 * thing in one line: a player should limit, not clip.
 *
 * A quadratic knee rather than a real limiter, because this runs inside
 * the mixer callback on a 67MHz core and a look-ahead limiter needs both
 * a delay line and a gain envelope. Below the knee nothing is touched at
 * all; above it the curve bends over and meets full scale with zero
 * slope, so there is no corner anywhere for harmonics to come from.
 *
 *   y = x - (x - KNEE)^2 / (4 * (CEIL - KNEE))
 *
 * The constants are chosen so that divisor is a power of two: the knee
 * at three quarters of full scale leaves 8192 below the ceiling, four
 * times which is exactly 1 << 15. So the whole thing is a subtract, a
 * multiply and a shift, and it only runs on the boost path - attenuating
 * cannot leave the range it came from, and that branch stays bit for bit
 * what it was. */
#define SOFT_KNEE  24576                        /* 0.75 of full scale */
#define SOFT_SHIFT 15                           /* 4 * (32768 - 24576) */
#define SOFT_CEIL  (SOFT_KNEE + (1 << (SOFT_SHIFT - 1)))

_Static_assert((1 << SOFT_SHIFT) == 4 * (32768 - SOFT_KNEE),
               "the knee and the shift have to describe the same curve");

static inline int16_t SoftClip(int32_t v)
{
    int32_t a = (v < 0) ? -v : v;

    if (a <= SOFT_KNEE) return (int16_t)v;
    if (a >= SOFT_CEIL) return (v < 0) ? -32768 : 32767;

    const int32_t over = a - SOFT_KNEE;
    a -= (over * over) >> SOFT_SHIFT;
    if (a > 32767) a = 32767;

    return (int16_t)((v < 0) ? -a : a);
}

static void RecomputeVolume(void)
{
    int v = (s_volume0to127 == 127) ? 128 : s_volume0to127;
    v = (v * s_gainNum) / 16;
    /* Bounded only so the per-sample multiply cannot overflow; the
     * output itself is clamped, so pushing past unity just clips. */
    if (v > 2048) v = 2048;
    s_volumeTarget = v;
}

int Audio_Volume(void) { return s_volume0to127; }

void Audio_SetGainDb(int db)
{
    if (db < 0) db = 0;
    if (db > (int)(sizeof(kGainNum) / sizeof(kGainNum[0])) - 1)
        db = (int)(sizeof(kGainNum) / sizeof(kGainNum[0])) - 1;

    s_gainNum = kGainNum[db];
    RecomputeVolume();
}

void Audio_SetVolume(int volume0to127)
{
    if (volume0to127 < 0) volume0to127 = 0;
    if (volume0to127 > 127) volume0to127 = 127;

    /* Scale to 0..128 so the top of the range is an exact >>7 and full
     * volume is bit-for-bit unity instead of 127/128 of it. Shifting
     * matters here: a divide would otherwise run twice per frame,
     * thousands of times a second. */
    s_volume0to127 = volume0to127;
    RecomputeVolume();
}

/* In fast RAM.
 *
 * This is the one routine in the program that is never allowed to be
 * late: the mixer calls it to fill the next buffer, and whatever it does
 * not deliver in time is a gap in the sound. It also does real
 * per-sample work - the volume multiply and the soft clip run over every
 * sample at 32768 frames a second, both channels - so it is not merely a
 * memcpy that happens to be urgent.
 *
 * Out of main RAM it competed for the instruction cache with the drawing
 * and the decoder, which between them touch far more code than the cache
 * holds - so the one function that must not stall was being fetched cold
 * on buffers where the UI had been busy. ITCM has no wait states and is
 * not shared with anything, so it cannot be evicted by either. */
#ifdef ARM9
__attribute__((section(".itcm"), long_call, target("arm"), noinline))
#endif
static mm_word OnStreamRequest(mm_word length, mm_addr dest, mm_stream_formats format)
{
    (void)format;
    int16_t *out = (int16_t *)dest;

    uint32_t want = length;
    uint32_t got = 0;

    bool silent = s_paused || s_muted;

    if (!silent) {
        uint32_t avail = QueuedUnsafe();
        got = want < avail ? want : avail;

        uint32_t tail = s_readIndex & RING_MASK;
        uint32_t firstRun = AUDIO_RING_FRAMES - tail;
        if (firstRun > got) firstRun = got;

        memcpy(out, &s_ring[tail * 2], firstRun * 2 * sizeof(int16_t));
        if (got > firstRun)
            memcpy(out + firstRun * 2, &s_ring[0],
                   (got - firstRun) * 2 * sizeof(int16_t));

        __asm__ volatile("" ::: "memory");
        s_readIndex += got;
        s_playedFrames += got;
    }

    /* Underrun (or paused): pad with silence rather than replaying stale
     * samples. The padding is deliberately not counted as played, so the
     * elapsed clock tracks decoded audio instead of drifting ahead of it
     * every time the card stalls. */
    if (got < want) {
        /* Only counted while actually playing: a paused or stopped player
         * emits silence on purpose, and that is not a fault. */
        /* A mute is deliberate silence, like a pause - counting it would
         * fill the glitch readout with faults that never happened. */
        if (!silent) s_underrunFrames += want - got;
        memset(out + got * 2, 0, (want - got) * 2 * sizeof(int16_t));
    }

    /* Apply volume across the real samples, easing toward the target one
     * step every few frames so a change glides in over a handful of
     * milliseconds instead of snapping. Rounded, not truncated: a plain
     * >>7 biases toward zero, which is a correlated error and sounds like
     * grit on quiet passages rather than like dither. */
    int vol = s_volumeCurrent;
    const int target = s_volumeTarget;

    /* Whether anything in this buffer can reach the knee at all.
     *
     * Attenuating cannot leave the range it came from, so below unity
     * the samples are what they always were - the steady path says so
     * and this keeps the ramp saying it too. Without it, whether a peak
     * got rounded would depend on whether a volume change happened to be
     * in flight, which is a rule nobody could hear but nobody could
     * state either. */
    const bool canBoost = (vol > 128) || (target > 128);

    if (vol != target) {
        /* Ramping: the general path, one step every eight frames so a
         * volume change glides in over a few milliseconds. */
        for (uint32_t i = 0; i < got; i++) {
            if ((i & 7) == 0 && vol != target)
                vol += (target > vol) ? 1 : -1;

            /* Shaped, not sheared: gain settings above 0 dB deliberately
             * push past unity, and what happens at the ceiling is the
             * difference between loud and noisy. */
            const int32_t l = (out[i * 2 + 0] * vol + 64) >> 7;
            const int32_t r = (out[i * 2 + 1] * vol + 64) >> 7;
            out[i * 2 + 0] = canBoost ? SoftClip(l) : (int16_t)l;
            out[i * 2 + 1] = canBoost ? SoftClip(r) : (int16_t)r;
        }

        /* Keep converging even across buffers that were entirely silence,
         * so the ramp cannot stall while paused. */
        if (got == 0 && vol != target)
            vol += (target > vol) ? 1 : -1;

    } else if (vol < 128) {
        /* Steady and attenuating, which is where this spends almost all
         * of its life: the ramp lasts a few milliseconds after a volume
         * change and unity only happens at maximum.
         *
         * Neither the ramp test nor the clamps are needed here. Scaling
         * down cannot leave the range it came from - a full-scale sample
         * times 128 is exactly full scale - so the four comparisons per
         * frame were checking for something arithmetically impossible,
         * 32768 times a second. */
        for (uint32_t i = 0; i < got * 2; i++)
            out[i] = (int16_t)((out[i] * vol + 64) >> 7);

    } else if (vol > 128) {
        /* Boosting past unity, so clipping is real and has to be caught -
         * but the ramp test still does not. */
        for (uint32_t i = 0; i < got * 2; i++)
            out[i] = SoftClip((out[i] * vol + 64) >> 7);
    }
    /* vol == 128 exactly: unity, nothing to do at all. */

    s_volumeCurrent = vol;

    return length;
}

/* Reopens the stream at a different rate.
 *
 * maxmod fixes the rate when the stream is opened, so there is no way to
 * change it in place - the stream is closed and opened again. That is
 * audible as a short break, which is the honest cost of the setting and
 * why it lives on the Test page rather than in Setup.
 *
 * The ring is emptied rather than carried across: what is in it was
 * produced for the old rate and would play at the wrong speed. The
 * decoder is told separately to rebuild its filter, because the ratio it
 * was set up for has just changed underneath it. */
bool Audio_SetOutputRate(uint32_t rate)
{
    if (rate == 0 || rate > AUDIO_OUT_RATE) rate = AUDIO_OUT_RATE;
    if (rate == s_outRate) return true;

    const bool wasPaused = s_paused;
    s_paused = true;

    if (s_streamOpen) {
        mmStreamClose();
        s_streamOpen = false;
    }

    s_outRate = rate;
    Audio_Reset();

    if (!Audio_Init()) return false;

    s_paused = wasPaused;
    return true;
}

bool Audio_Init(void)
{
    if (s_streamOpen) return true;

    /* No soundbank: mod_count/samp_count of zero is the documented way to
     * bring maxmod up for streaming only (see devkitPro's own maxmod
     * streaming example). */
    /* fifo_channel is left zeroed: maxmod 2.x on calico talks to its ARM7
     * half over PXI and no longer reads that field. */
    mm_ds_system sys;
    memset(&sys, 0, sizeof(sys));
    sys.mod_count  = 0;
    sys.samp_count = 0;
    sys.mem_bank   = 0;
    mmInit(&sys);

    mm_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.sampling_rate    = s_outRate;
    stream.buffer_length    = 2048;
    stream.callback         = OnStreamRequest;
    stream.format           = MM_STREAM_16BIT_STEREO;
    stream.thread_stack_size = 0;      /* default stack */

    /* maxmod refills the stream from a thread of its own rather than the
     * main loop, so audio continuity is not tied to the frame rate.
     *
     * The priority is set explicitly, and that detail is load-bearing.
     * This field is a union: a *positive* value means "no thread, call
     * mmStreamUpdate() yourself", zero means "make a thread at the
     * default priority", and a negative value means "make a thread at
     * priority abs(value)". The default it picks for the zero case is 29
     * - and calico's MAIN_THREAD_PRIO is 28, where a *lower* number means
     * higher priority. So asking for the default hands the audio thread a
     * priority one step BELOW the main thread, and calico only
     * round-robins between equal priorities: a lower-priority thread
     * cannot preempt at all. The mixer would then only ever get to run
     * when the main loop happened to block in swiWaitForVBlank, and every
     * frame that overran its budget would starve it into an underrun.
     * Naming a priority above the main thread is what lets audio
     * interrupt drawing instead of queueing behind it. */
    stream.minus_thread_prio = -AUDIO_THREAD_PRIO;

    mmStreamOpen(&stream);

    s_streamOpen = true;
    return true;
}
