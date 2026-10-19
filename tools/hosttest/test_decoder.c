/* Host-side test for source/decoder_mp3.c.
 *
 * decoder_mp3.c is included rather than linked so the test can reach its
 * statics (the resampler in particular). audio.c is replaced by a stub
 * that models the mixer: it accepts frames into a queue and only removes
 * them when the test says playback has consumed them, which is what the
 * end-of-track drain logic depends on. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "audio.h"

/* ---- stub mixer -------------------------------------------------- */

#define CAP (1 << 22)
static int16_t g_captured[CAP * 2];
static uint32_t g_capturedFrames = 0;
static uint32_t g_queued = 0;
static uint32_t g_played = 0;
static bool g_paused = false;
static int g_volume = 127;

uint32_t Audio_Write(const int16_t *frames, uint32_t count)
{
    uint32_t space = Audio_FreeFrames();
    if (count > space) count = space;
    if (g_capturedFrames + count > CAP) { printf("capture overflow\n"); exit(1); }

    memcpy(&g_captured[g_capturedFrames * 2], frames, count * 4);
    g_capturedFrames += count;
    g_queued += count;
    return count;
}

uint32_t Audio_FreeFrames(void)   { return AUDIO_RING_FRAMES - g_queued; }
uint32_t Audio_QueuedFrames(void) { return g_queued; }

/* The stream's rate. Settable, because the filter's cutoff is derived
 * from the ratio between this and the file's rate - so a test that
 * cannot move it cannot check that the filter follows. */
static uint32_t g_outRate = AUDIO_OUT_RATE;
uint32_t Audio_OutputRate(void) { return g_outRate; }
uint32_t Audio_PlayedFrames(void) { return g_played; }
void Audio_Reset(void)            { g_queued = 0; g_played = 0; }
void Audio_SetMuted(bool m) { (void)m; }

/* The host ring never underruns - it is a counter, not a mixer - so
 * these are constant. Present so the resume test can assert that
 * playback comes back without new faults. */
uint32_t Audio_UnderrunFrames(void) { return 0; }
uint32_t Audio_DroppedFrames(void)  { return 0; }
void Audio_SetPaused(bool p)      { g_paused = p; }
void Audio_SetVolume(int v)       { g_volume = v; }
bool Audio_Init(void)             { return true; }

/* Pretend the mixer consumed `n` frames. */
static void Consume(uint32_t n)
{
    if (n > g_queued) n = g_queued;
    g_queued -= n;
    g_played += n;
}

#include "decoder_mp3.c"

/* ---- test scaffolding -------------------------------------------- */

static int g_fail = 0;

static void Check(const char *what, bool ok, const char *detail)
{
    printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

static void CheckNear(const char *what, long got, long want, long tol)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "got %ld, want %ld +/-%ld", got, want, tol);
    Check(what, labs(got - want) <= tol, buf);
}

/* Runs a whole file to completion the way main.c does: pump, then let the
 * "mixer" drain a frame's worth of audio each iteration. */
static uint32_t PlayToEnd(const char *path, int *outIterations, int maxIterations)
{
    g_capturedFrames = 0;
    if (!Mp3Decoder_Open(path)) { printf("  open failed: %s\n", path); g_fail++; return 0; }

    int iterations = 0;
    while (Mp3Decoder_Pump()) {
        Consume(AUDIO_OUT_RATE / 60);      /* one video frame of audio */
        if (++iterations > maxIterations) break;
    }

    *outIterations = iterations;
    uint32_t produced = g_capturedFrames;
    Mp3Decoder_Close();
    return produced;
}

static void TestPlainFile(void)
{
    printf("\nplain.mp3 (200 frames, no Xing, CBR 128k)\n");

    int iterations = 0;
    uint32_t frames = PlayToEnd("plain.mp3", &iterations, 100000);

    /* 200 MPEG frames * 1152 samples at 44100Hz, resampled to 32768Hz */
    long expected = (long)(200.0 * 1152 * AUDIO_OUT_RATE / 44100.0);
    CheckNear("output frame count matches resampled duration", frames, expected, 8);
    Check("terminates instead of spinning at EOF", iterations < 100000, NULL);

    Mp3Decoder_Open("plain.mp3");
    while (Mp3Decoder_Pump()) Consume(AUDIO_OUT_RATE / 60);
    CheckNear("duration estimate (CBR)", Mp3Decoder_TotalMs(), 5224, 60);
    Check("duration reported as exact for CBR", Mp3Decoder_DurationIsExact(), NULL);
    CheckNear("sample rate", Mp3Decoder_SampleRate(), 44100, 0);
    CheckNear("bitrate", Mp3Decoder_BitrateKbps(), 128, 0);
    Mp3Decoder_Close();
}

static void TestXingFile(void)
{
    printf("\nxing.mp3 (Xing header with frame count + TOC)\n");

    if (!Mp3Decoder_Open("xing.mp3")) { Check("open", false, NULL); return; }
    Mp3Decoder_Pump();

    Check("Xing header detected", s_hasVbrHeader, NULL);
    Check("TOC parsed", s_hasToc, NULL);
    CheckNear("Xing frame count", s_vbrFrames, 200, 0);
    CheckNear("duration from Xing", Mp3Decoder_TotalMs(), 5224, 30);
    Check("duration reported as exact", Mp3Decoder_DurationIsExact(), NULL);

    Mp3Decoder_Close();
}

static void TestId3Skip(void)
{
    printf("\nid3.mp3 (1010 byte ID3v2 tag in front)\n");

    if (!Mp3Decoder_Open("id3.mp3")) { Check("open", false, NULL); return; }
    Check("ID3v2 tag skipped", s_dataStart == 1010, NULL);
    Mp3Decoder_Pump();

    /* If the tag were counted as audio, the estimate would be ~63ms long. */
    CheckNear("duration excludes the tag bytes", Mp3Decoder_TotalMs(), 5224, 60);
    Mp3Decoder_Close();
}

static void TestSeek(void)
{
    printf("\nseeking\n");

    if (!Mp3Decoder_Open("xing.mp3")) { Check("open", false, NULL); return; }
    Mp3Decoder_Pump();

    uint32_t beforeSeek = Audio_PlayedFrames();
    Mp3Decoder_SeekToMs(2600);
    CheckNear("elapsed follows a seek", Mp3Decoder_ElapsedMs(), 2600, 1);

    /* The seek discards whatever was queued from the old position and
     * then refills from the new one, so the test is that the mixer's
     * position restarted - not that the buffer is empty, which it
     * deliberately is not any more. */
    (void)beforeSeek;
    Check("stale audio dropped and position reset", Audio_PlayedFrames() == 0, NULL);

    int iterations = 0;
    while (Mp3Decoder_Pump()) {
        Consume(AUDIO_OUT_RATE / 60);
        if (++iterations > 100000) break;
    }
    Check("still terminates after a mid-file seek", iterations < 100000, NULL);
    Mp3Decoder_Close();

    /* Seeking past the end must not wedge the decoder. */
    Mp3Decoder_Open("xing.mp3");
    Mp3Decoder_Pump();
    Mp3Decoder_SeekToMs(999999);
    iterations = 0;
    while (Mp3Decoder_Pump()) {
        Consume(AUDIO_OUT_RATE / 60);
        if (++iterations > 100000) break;
    }
    Check("seek past the end terminates", iterations < 100000, NULL);
    Mp3Decoder_Close();

    /* Seeking backwards from the start must clamp, not underflow. */
    Mp3Decoder_Open("xing.mp3");
    Mp3Decoder_Pump();
    Mp3Decoder_SeekBy(-999999);
    CheckNear("seek before the start clamps to zero", Mp3Decoder_ElapsedMs(), 0, 0);
    Mp3Decoder_Close();
}

static void TestDrainBeforeFinish(void)
{
    printf("\nend-of-track drain\n");

    if (!Mp3Decoder_Open("tiny.mp3")) { Check("open", false, NULL); return; }

    /* Decode everything, never consuming. Pump must keep reporting true
     * for as long as audio is still queued, or main.c would skip to the
     * next track before the tail had played. */
    bool stillPlaying = true;
    for (int i = 0; i < 200 && stillPlaying; i++)
        stillPlaying = Mp3Decoder_Pump();

    Check("still 'playing' while audio remains queued", Audio_QueuedFrames() > 0, NULL);
    Check("did not report finished early", stillPlaying, NULL);

    Consume(Audio_QueuedFrames());
    Check("reports finished once drained", Mp3Decoder_Pump() == false, NULL);

    Mp3Decoder_Close();
}

static void TestTruncatedAndGarbage(void)
{
    printf("\nmalformed input\n");

    FILE *f = fopen("garbage.mp3", "wb");
    for (int i = 0; i < 40000; i++) fputc((i * 37) & 0xFF, f);
    fclose(f);

    int iterations = 0;
    PlayToEnd("garbage.mp3", &iterations, 200000);
    Check("pure garbage terminates instead of hanging", iterations < 200000, NULL);

    /* A file cut off mid-frame: the classic way an EOF loop spins. */
    FILE *src = fopen("plain.mp3", "rb");
    FILE *dst = fopen("truncated.mp3", "wb");
    for (int i = 0; i < 5000; i++) fputc(fgetc(src), dst);
    fclose(src); fclose(dst);

    iterations = 0;
    PlayToEnd("truncated.mp3", &iterations, 200000);
    Check("truncated file terminates", iterations < 200000, NULL);

    Check("missing file fails cleanly", Mp3Decoder_Open("does_not_exist.mp3") == false, NULL);
}

/* ---- resampler measurement helpers -------------------------------- */

/* Magnitude of one frequency bin, by direct correlation. Avoids needing
 * to know the filter's group delay, since it measures both quadratures. */
static int WholeCycles(int n, double freq, double rate)
{
    int cycles = (int)(n * freq / rate);
    int len = (int)(cycles * rate / freq);
    return (len > 0 && len <= n) ? len : n;
}

static double BinMagnitude(const int16_t *x, int n, int stride, double freq, double rate)
{
    n = WholeCycles(n, freq, rate);
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        double t = 2.0 * M_PI * freq * i / rate;
        re += x[i * stride] * cos(t);
        im -= x[i * stride] * sin(t);
    }
    return 2.0 * sqrt(re * re + im * im) / n;
}

static double Rms(const int16_t *x, int n, int stride)
{
    double s = 0;
    for (int i = 0; i < n; i++) {
        double v = x[i * stride];
        s += v * v;
    }
    return sqrt(s / n);
}

/* A reference linear-interpolation resampler - the algorithm this used to
 * use - so the FIR's alias rejection can be quoted as an improvement over
 * something concrete rather than in the abstract. */
static int LinearResample(const int16_t *in, int inCount, double ratio,
                          int16_t *out, int outMax)
{
    int n = 0;
    double pos = 0;
    while (n < outMax) {
        int base = (int)pos;
        if (base + 1 >= inCount) break;
        double f = pos - base;
        out[n++] = (int16_t)(in[base] + (in[base + 1] - in[base]) * f);
        pos += ratio;
    }
    return n;
}

/* Feeds a pure tone through the real resampler and returns the output. */
static int RunResampler(double freqHz, uint32_t inRate, int16_t *out, int outMax)
{
    static mad_fixed_t left[1152], right[1152];

    s_samplesPerFrame = 1152;
    ResetResampler();
    ConfigureResampler(inRate);

    int total = 0;
    for (int fr = 0; fr < 60; fr++) {
        for (int i = 0; i < 1152; i++) {
            double t = (double)(fr * 1152 + i) / inRate;
            double v = sin(2.0 * M_PI * freqHz * t) * 0.8;
            left[i] = right[i] = (mad_fixed_t)(v * MAD_F_ONE);
        }

        uint32_t n = ResampleFrame(left, right, 1152);
        for (uint32_t k = 0; k < n && total < outMax; k++)
            out[total++] = s_outBuf[k * 2];
    }
    return total;
}

/* Lowering the output rate must narrow the filter.
 *
 * The cutoff comes from the ratio between the file's rate and the
 * stream's, so dropping the stream to 22050 should reject what a 32768
 * stream passes. The bank is cached, and the output rate was missing
 * from the cache key - harmless while the stream ran at one fixed rate,
 * and a silent no-op the moment a setting could change it. The filter
 * stayed as it was and the setting did nothing.
 *
 * Measured at 14kHz, which a 32768 stream carries and a 22050 one cannot
 * - its Nyquist is 11025. */
static void TestOutputRateFollows(void)
{
    printf("\noutput rate\n");

    static mad_fixed_t left[1152], right[1152];
    double peak[2];

    /* Put the resampler back the way it was found. Leaving it on a mode
     * of this test's choosing broke the quality check that runs next -
     * the same trap the mono test fell into. */
    const int wasMode = Mp3Decoder_ResampleMode();

    for (int c = 0; c < 2; c++) {
        g_outRate = c ? 22050 : 32768;

        Mp3Decoder_SetResampleMode(RESAMPLE_BEST);
        ResetResampler();
        ConfigureResampler(44100);

        int p = 0;
        for (int fr = 0; fr < 40; fr++) {
            for (int i = 0; i < 1152; i++) {
                double t = (double)(fr * 1152 + i) / 44100.0;
                double v = sin(2.0 * M_PI * 14000.0 * t) * 0.5;
                left[i] = right[i] = (mad_fixed_t)(v * MAD_F_ONE);
            }
            uint32_t n = ResampleFrame(left, right, 1152);
            if (fr < 8) continue;
            for (uint32_t k = 0; k < n; k++) {
                int a = s_outBuf[k * 2];
                if (a < 0) a = -a;
                if (a > p) p = a;
            }
        }
        peak[c] = p;
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "14kHz peak: %.0f at 32768, %.0f at 22050",
             peak[0], peak[1]);

    /* Well clear of the noise floor at the higher rate, and firmly gone
     * at the lower one. Any margin either side of that would pass with
     * the cache key broken, because both numbers would be the same. */
    Check("lowering the output rate narrows the filter",
          peak[0] > 4000 && peak[1] < peak[0] / 4, buf);

    g_outRate = AUDIO_OUT_RATE;
    Mp3Decoder_SetResampleMode(wasMode);
    ResetResampler();
    ConfigureResampler(44100);
}

/* Mono folds both channels into one before the filter runs.
 *
 * The saving is that the filter does one channel instead of two, which
 * is only real if the fold happens first - doing it after costs the same
 * and saves nothing. So what is checked is both halves: that the output
 * really is one signal on both sides, and that it is the average of the
 * two inputs rather than one of them thrown away.
 *
 * A left-only tone is the case that tells those apart. Keeping the left
 * channel gives full amplitude; averaging gives half. */
static void TestMonoFold(void)
{
    printf("\nmono fold\n");

    static mad_fixed_t left[1152], right[1152];
    char buf[160];

    /* Put the resampler back afterwards. Leaving it on Lite made the
     * quality test that runs next measure a four-tap filter against a
     * ten-kilohertz tone and call it a regression - a failure caused
     * entirely by the test before it. */
    const int wasMode = Mp3Decoder_ResampleMode();

    for (int pass = 0; pass < 2; pass++) {
        const bool mono = (pass == 1);
        Mp3Decoder_SetMono(mono);
        Mp3Decoder_SetResampleMode(RESAMPLE_LITE);
        ResetResampler();
        ConfigureResampler(44100);

        int peakL = 0, peakR = 0, worstGap = 0;

        for (int fr = 0; fr < 20; fr++) {
            for (int i = 0; i < 1152; i++) {
                double t = (double)(fr * 1152 + i) / 44100.0;
                double v = sin(2.0 * M_PI * 1000.0 * t) * 0.8;
                left[i]  = (mad_fixed_t)(v * MAD_F_ONE);
                right[i] = 0;                    /* silence on the right */
            }

            uint32_t n = ResampleFrame(left, right, 1152);
            for (uint32_t k = 0; k < n; k++) {
                int l = s_outBuf[k * 2], r = s_outBuf[k * 2 + 1];
                int gap = l > r ? l - r : r - l;
                if (gap > worstGap) worstGap = gap;
                if (l > peakL) peakL = l;
                if (r > peakR) peakR = r;
            }
        }

        snprintf(buf, sizeof(buf), "peak L %d R %d, widest L-R gap %d",
                 peakL, peakR, worstGap);

        if (mono) {
            Check("mono puts the same signal on both channels",
                  worstGap == 0 && peakR > 1000, buf);
            /* Half, because the right channel was silent and the two
             * were averaged. A decoder that simply used the left would
             * come back at full amplitude. */
            Check("and it is the average, not one channel kept",
                  peakL > 8000 && peakL < 16000, buf);
        } else {
            Check("stereo keeps the channels apart",
                  peakL > 16000 && peakR < 1000, buf);
        }
    }

    Mp3Decoder_SetMono(false);
    Mp3Decoder_SetResampleMode(wasMode);
    ResetResampler();
    ConfigureResampler(44100);
}

/* Reproduces the start-of-track sequence main.c/player.c perform, and
 * checks the mixer never sees an empty buffer. This is the bug that
 * showed up on hardware as a ~2000 frame underrun on every track: the
 * stream was unpaused before the decoder had produced anything. */
static void TestPrefill(void)
{
    printf("\nstart-of-track buffering\n");

    if (!Mp3Decoder_Open("plain.mp3")) { Check("open", false, NULL); return; }

    char buf[96];
    snprintf(buf, sizeof(buf), "%u frames queued before decoding", Audio_QueuedFrames());
    Check("buffer starts empty, as expected", Audio_QueuedFrames() == 0, buf);

    Mp3Decoder_Prefill();

    uint32_t queued = Audio_QueuedFrames();
    snprintf(buf, sizeof(buf), "%u frames (%.0f ms) ready before playback starts",
             queued, 1000.0 * queued / AUDIO_OUT_RATE);
    /* The mixer asks for up to one full maxmod buffer (2048) the instant
     * it is unpaused, so anything less than that still glitches. */
    Check("prefill covers the mixer's first request", queued >= 2048, buf);

    /* Now drain at the real rate and confirm the decoder keeps ahead. */
    uint32_t lowest = queued;
    for (int i = 0; i < 600; i++) {
        if (!Mp3Decoder_Pump()) break;
        Consume(AUDIO_OUT_RATE / 60);
        /* Only meaningful while there is still file left to decode - the
         * buffer is *supposed* to reach zero once the track ends. */
        if (s_decodeDone) break;
        if (Audio_QueuedFrames() < lowest) lowest = Audio_QueuedFrames();
    }
    snprintf(buf, sizeof(buf), "low-water mark %u frames", lowest);
    Check("decoder stays ahead of playback", lowest > 0, buf);

    Mp3Decoder_Close();

    /* Seeking empties the ring too, so it needs the same treatment. */
    Mp3Decoder_Open("xing.mp3");
    Mp3Decoder_Prefill();
    Mp3Decoder_SeekToMs(2000);
    snprintf(buf, sizeof(buf), "%u frames queued after seek", Audio_QueuedFrames());
    Check("a seek leaves the buffer primed too", Audio_QueuedFrames() >= 2048, buf);
    Mp3Decoder_Close();
}

/* The decoder throttles itself so the UI can breathe, but must abandon
 * that the moment the output buffer is running dry. Getting this backwards
 * is not a small bug: a catch-up path that cannot actually catch up means
 * a buffer that has drained never refills, and playback breaks up
 * permanently rather than briefly. */
static void TestDecodeCatchUp(void)
{
    printf("\ndecode scheduling\n");

    char buf[128];

    if (!Mp3Decoder_Open("plain.mp3")) { Check("open", false, NULL); return; }
    Mp3Decoder_Prefill();

    /* Healthy buffer: one call should do a little work and hand the frame
     * back, not decode everything it can. */
    g_capturedFrames = 0;
    Mp3Decoder_Pump();
    uint32_t whenHealthy = g_capturedFrames;

    snprintf(buf, sizeof(buf), "%u frames produced with a full buffer", whenHealthy);
    Check("throttles while the buffer is healthy", whenHealthy <= 2 * 900, buf);

    /* Now starve it and check one call recovers a meaningful amount. */
    Consume(Audio_QueuedFrames());
    g_capturedFrames = 0;
    Mp3Decoder_Pump();
    uint32_t whenStarved = g_capturedFrames;

    snprintf(buf, sizeof(buf), "%u frames produced with an empty buffer", whenStarved);
    Check("ignores the clock when the buffer is draining", whenStarved > 4 * 900, buf);

    snprintf(buf, sizeof(buf), "healthy %u vs starved %u frames", whenHealthy, whenStarved);
    Check("catch-up does much more work than the throttled case",
          whenStarved > whenHealthy * 3, buf);

    /* The main thread's backstop is deliberately a much smaller thing than
     * the pump above. Recovering a drained ring is the decode worker's job
     * - it raises itself above the UI to do it - and this exists only to
     * cover the worker not being inside the decoder at all. It is capped
     * so that it can never itself become the stall it replaced, so what is
     * checked here is that it does *some* work and stays small. */
    Consume(Audio_QueuedFrames());
    g_capturedFrames = 0;
    Mp3Decoder_PumpIfIdle();
    uint32_t viaBackstop = g_capturedFrames;

    snprintf(buf, sizeof(buf), "%u frames, versus the full pump's %u",
             viaBackstop, whenStarved);
    Check("backstop makes progress without becoming a stall",
          viaBackstop > 0 && viaBackstop <= 3 * 900, buf);

    Mp3Decoder_Close();
}

/* A scrub must not touch the file until it ends. That is the whole point
 * of it: an fseek walks the FAT chain, and doing one per step was what
 * made holding the seek button feel heavy. */
static void TestScrub(void)
{
    printf("\nscrubbing\n");

    char buf[140];
    if (!Mp3Decoder_Open("plain.mp3")) { Check("open", false, NULL); return; }
    Mp3Decoder_Prefill();

    uint32_t before = Audio_QueuedFrames();

    /* Positions are taken from the file's own length: the synthetic
     * tracks here are a few seconds long, and a scrub past the end
     * correctly clamps - which looks like the target not being recorded
     * if the test asks for somewhere the track does not reach. */
    uint32_t total = Mp3Decoder_TotalMs();
    uint32_t aim = total / 2;

    Mp3Decoder_SetScrubbing(true);
    g_capturedFrames = 0;
    for (int i = 0; i < 20; i++)
        Mp3Decoder_SeekToMs((total * (uint32_t)(i + 1)) / 40);

    uint32_t during = Audio_QueuedFrames();
    snprintf(buf, sizeof(buf), "%u frames queued before, %u after 20 steps",
             before, during);
    Check("twenty scrub steps leave the buffer alone", during == before, buf);

    snprintf(buf, sizeof(buf), "reports %lu ms, pointing at %lu of %lu",
             (unsigned long)Mp3Decoder_ElapsedMs(),
             (unsigned long)aim, (unsigned long)total);
    Check("position follows the scrub, not the mixer",
          Mp3Decoder_ElapsedMs() == aim, buf);

    /* Letting go hands the seek over but does not perform it yet. This
     * window is where the readout used to fall back to the old position
     * and then jump forward again - on hardware it is however long the
     * card takes, which is exactly long enough to see. */
    Mp3Decoder_SetScrubbing(false);

    snprintf(buf, sizeof(buf), "reports %lu ms with the seek still owed",
             (unsigned long)Mp3Decoder_ElapsedMs());
    Check("position holds while the seek is pending",
          Mp3Decoder_ElapsedMs() == aim, buf);

    /* Now perform it - the worker's job on hardware. */
    Mp3Decoder_RunPendingSeek();

    snprintf(buf, sizeof(buf), "reports %lu ms once it has landed",
             (unsigned long)Mp3Decoder_ElapsedMs());
    Check("and does not move when it lands", Mp3Decoder_ElapsedMs() == aim, buf);

    /* And it must come back with a real cushion, not a sliver: resuming
     * on 30ms of audio is what made playback stutter for about a second
     * after every seek. */
    Mp3Decoder_Prefill();
    snprintf(buf, sizeof(buf), "%u frames = %u ms of audio ready",
             Audio_QueuedFrames(), Audio_QueuedFrames() * 1000 / AUDIO_OUT_RATE);
    /* Measured in milliseconds of audio, not in fractions of the ring.
     * What this is guarding is that a seek does not resume on a sliver -
     * a property of how much sound is ready, which does not change when
     * the ring is resized. Written as half the ring it moved on its own
     * the moment the ring doubled, failing on an unchanged prefill that
     * had put up 313ms. */
    Check("playback resumes on a full buffer, not a sliver",
          Audio_QueuedFrames() >= (AUDIO_OUT_RATE / 4), buf);   /* 250ms */

    /* The fragment bug: a seek arriving before scrubbing is declared does
     * the full job - reset and prefill - and the mixer plays a slice of
     * the destination before the mute catches up. */
    uint32_t settled = Audio_QueuedFrames();
    Mp3Decoder_SetScrubbing(true);
    Mp3Decoder_SeekToMs(total / 4);
    snprintf(buf, sizeof(buf), "%u frames before, %u immediately after",
             settled, Audio_QueuedFrames());
    Check("the first seek of a scrub refills nothing",
          Audio_QueuedFrames() == settled, buf);
    Mp3Decoder_SetScrubbing(false);
    Mp3Decoder_RunPendingSeek();

    Mp3Decoder_Close();
}

static void TestResamplerQuality(void)
{
    printf("\nresampler audio quality (44100 -> 32768)\n");

    static int16_t out[200000];
    static int16_t lin[200000];
    static int16_t tone[200000];

    /* --- passband: a tone well inside the band must survive intact --- */
    struct { double f; const char *name; } passband[] = {
        { 440.0,  "440Hz" }, { 1000.0, "1kHz" }, { 5000.0, "5kHz" }, { 10000.0, "10kHz" },
    };

    for (unsigned i = 0; i < sizeof(passband) / sizeof(passband[0]); i++) {
        int n = RunResampler(passband[i].f, 44100, out, 200000);

        /* Skip the filter's warm-up before measuring. */
        const int skip = 512;
        int len = WholeCycles(n - skip, passband[i].f, AUDIO_OUT_RATE);
        double fund = BinMagnitude(out + skip, len, 1, passband[i].f, AUDIO_OUT_RATE);
        double total = Rms(out + skip, len, 1) * sqrt(2.0);
        double resid = (total > fund) ? sqrt(total * total - fund * fund) : 0.0;
        double thdn = (fund > 0) ? 20.0 * log10(resid / fund) : 0.0;

        char buf[128];
        snprintf(buf, sizeof(buf), "%s: fundamental %.0f, THD+N %.1f dB",
                 passband[i].name, fund, thdn);
        /* -45dB is roughly 0.6% distortion - inaudible on DS speakers, and
         * far better than the phase quantisation alone would give. */
        Check("passband tone reproduced cleanly", thdn < -45.0, buf);
    }

    /* --- stopband: content above the output Nyquist must be rejected ---
     * This is the whole point of the change. 20kHz cannot exist at 32768Hz
     * output; a resampler without a real lowpass folds it back to
     * |20000 - 32768| = 12.7kHz, right in the middle of the audible band. */
    /* Both filter lengths, because both can be selected and only one of
     * them used to be measured here.
     *
     * This test called neither Mp3Decoder_SetHighQuality nor anything
     * else that moves s_taps, so it ran at the 24-tap default and quoted
     * that figure - while the ROM ships the 12-tap filter. The number in
     * the report described a configuration nobody was running. */
    const double aliasFreq = 20000.0;

    /* Same tone through plain linear interpolation, as the baseline. */
    int toneN = 0;
    for (; toneN < 120000; toneN++)
        tone[toneN] = (int16_t)(sin(2.0 * M_PI * aliasFreq * toneN / 44100.0) * 0.8 * 32767);
    int linN = LinearResample(tone, toneN, 44100.0 / AUDIO_OUT_RATE, lin, 200000);
    double linLevel = Rms(lin + 512, linN - 512, 1) * RES_HEADROOM;

    double inputLevel = 0.8 * 32767 * RES_HEADROOM / sqrt(2.0);
    double linDb = 20.0 * log10(linLevel / inputLevel);

    static const struct { int mode; const char *name; double floorDb; } kModes[] = {
        /* The short filter is what ships, and its rejection is far weaker
         * than the long one's - about -25dB against -75dB. That is not a
         * pass mark being lowered to fit: it is the real figure, asserted
         * where it actually sits so that a regression still shows, and
         * printed so nobody reads the long filter's number and assumes it
         * applies to the default. */
        { RESAMPLE_FAST,  "Fast  ( 8 taps, to  9.8k)", -30.0 },
        { RESAMPLE_WIDE,  "Wide  (12 taps, to 14.7k)", -20.0 },
        { RESAMPLE_CLEAN, "Clean (12 taps, to 11.5k)", -45.0 },
        { RESAMPLE_BEST,  "Best  (24 taps, to 14.7k)", -60.0 },
    };

    for (unsigned f = 0; f < sizeof(kModes) / sizeof(kModes[0]); f++) {
        Mp3Decoder_SetResampleMode(kModes[f].mode);

        int n = RunResampler(aliasFreq, 44100, out, 200000);
        double firLevel = Rms(out + 512, n - 512, 1);
        double firDb = 20.0 * log10(firLevel / inputLevel);

        char buf[180];
        snprintf(buf, sizeof(buf), "%s: 20kHz folds back at %.1f dB (linear %.1f dB)",
                 kModes[f].name, firDb, linDb);
        Check("out-of-band content is rejected, not folded back",
              firDb < kModes[f].floorDb, buf);
        Check("clearly better than linear interpolation",
              (linDb - firDb) > 15.0, buf);
    }

    /* Left as the shipping default, so anything measured after this point
     * sees the filter the ROM actually uses. */
    Mp3Decoder_SetResampleMode(RESAMPLE_WIDE);

    /* Rejecting the stopband is only half the job - the filter also has to
     * leave the treble alone. 10kHz must come through essentially at full
     * level, or the cure is worse than the disease. */
    int tn = RunResampler(10000.0, 44100, out, 200000);
    double treble = BinMagnitude(out + 512, tn - 512, 1, 10000.0, AUDIO_OUT_RATE);
    double trebleDb = 20.0 * log10(treble / (0.8 * 32767.0 * RES_HEADROOM));
    char tbuf[128];
    snprintf(tbuf, sizeof(tbuf), "10kHz passes at %.2f dB (shipping filter)", trebleDb);
    Check("treble is preserved, not filtered away", trebleDb > -1.0, tbuf);
}

/* Drives the resampler directly with a synthetic signal - the silent test
 * files cannot show whether the interpolation itself is right. */
static void TestResampler(void)
{
    printf("\nresampler\n");

    static mad_fixed_t left[1152], right[1152];
    const double freq = 440.0;

    struct { uint32_t rate; const char *name; } cases[] = {
        { 44100, "44100 -> 32768" },
        { 48000, "48000 -> 32768" },
        { 32768, "32768 -> 32768 (unity)" },
        { 22050, "22050 -> 32768 (upsample)" },
        { 16000, "16000 -> 32768 (2x)" },
    };

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint32_t inRate = cases[c].rate;

        s_samplesPerFrame = 1152;
        ResetResampler();
        ConfigureResampler(inRate);

        uint32_t totalOut = 0;
        const int nFrames = 40;

        for (int fr = 0; fr < nFrames; fr++) {
            for (int i = 0; i < 1152; i++) {
                double t = (double)(fr * 1152 + i) / inRate;
                double v = sin(2.0 * M_PI * freq * t) * 0.5;
                left[i] = (mad_fixed_t)(v * MAD_F_ONE);
                right[i] = left[i];
            }

            uint32_t n = ResampleFrame(left, right, 1152);
            if (n > s_maxOutPerFrame) {
                printf("  !! produced %u frames, budget said %u\n", n, s_maxOutPerFrame);
                g_fail++;
            }

            totalOut += n;
        }

        long expected = (long)((double)nFrames * 1152 * AUDIO_OUT_RATE / inRate);
        char buf[128];

        snprintf(buf, sizeof(buf), "%s: %u frames, want ~%ld", cases[c].name, totalOut, expected);
        Check("output length matches the rate ratio", labs((long)totalOut - expected) <= 4, buf);

    }

    /* At 1:1 the filter is not a passthrough - it is still a lowpass, by
     * design - so the meaningful check is that a mid-band tone comes
     * through at full level rather than that the samples are identical. */
    static int16_t unity[200000];
    int un = RunResampler(1000.0, AUDIO_OUT_RATE, unity, 200000);
    double umag = BinMagnitude(unity + 512, un - 512, 1, 1000.0, AUDIO_OUT_RATE);
    double uwant = 0.8 * 32767.0 * RES_HEADROOM;

    char ubuf[128];
    snprintf(ubuf, sizeof(ubuf), "1kHz at 1:1 came through at %.0f, expected ~%.0f", umag, uwant);
    Check("unity rate preserves level", fabs(umag - uwant) / uwant < 0.05, ubuf);
}

/* Every mode must read back as itself.
 *
 * The getter used to reconstruct the mode from (taps, rolloff), which
 * cannot tell apart two modes that share a filter. Raw and Lite both came
 * back as Clean, so selecting either applied it and then reported it as
 * something else - the UI segment snapped back and the setting looked
 * frozen. A projection that loses information is only safe while nothing
 * collides in it, and nothing here guarded that. */
static void TestResampleModeRoundTrip(void)
{
    printf("resampler mode round-trip\n");

    static const char *const kNames[RESAMPLE_MODES] = {
        "Raw", "Lite", "Fast", "Wide", "Clean", "Best"
    };

    for (int m = 0; m < RESAMPLE_MODES; m++) {
        Mp3Decoder_SetResampleMode(m);
        int got = Mp3Decoder_ResampleMode();
        char detail[64];
        snprintf(detail, sizeof(detail), "set %s (%d), read back %d",
                 kNames[m], m, got);
        Check("mode reads back as itself", got == m, detail);
    }

    /* Setting the same mode twice must not disturb it: the early-out in
     * the setter compares the filter, not the mode. */
    Mp3Decoder_SetResampleMode(RESAMPLE_LITE);
    Mp3Decoder_SetResampleMode(RESAMPLE_LITE);
    Check("re-setting a mode keeps it",
          Mp3Decoder_ResampleMode() == RESAMPLE_LITE, NULL);

    /* Wide and Clean share a tap count and differ only in rolloff, which
     * is exactly the pair the old projection got right by accident. */
    Mp3Decoder_SetResampleMode(RESAMPLE_WIDE);
    Check("Wide survives its shared tap count",
          Mp3Decoder_ResampleMode() == RESAMPLE_WIDE, NULL);
    Mp3Decoder_SetResampleMode(RESAMPLE_CLEAN);
    Check("Clean survives its shared tap count",
          Mp3Decoder_ResampleMode() == RESAMPLE_CLEAN, NULL);
}

/* The resampler setting survives opening a file.
 *
 * Shared by construction - ConfigureResampler recomputes the phase step
 * and the coefficient bank, and the tap count comes from the quality
 * chosen - but "by construction" is what several things here were until
 * they were not, and a setting that silently reverted on every track
 * would look like the setting not working at all. */
static void TestResamplerSurvivesOpen(void)
{
    printf("\nresampler across tracks\n");

    static const char *kFiles[] = { "plain.mp3", "xing.mp3", "id3.mp3" };
    char buf[160];

    for (int mode = 0; mode < RESAMPLE_MODES; mode++) {
        Mp3Decoder_SetResampleMode(mode);
        unsigned wantTaps = s_taps;

        for (size_t i = 0; i < sizeof(kFiles) / sizeof(kFiles[0]); i++) {
            if (!Mp3Decoder_Open(kFiles[i])) continue;
            Mp3Decoder_Pump();

            if (s_taps != wantTaps || Mp3Decoder_ResampleMode() != mode) {
                snprintf(buf, sizeof(buf), "%s: mode %d taps %u, wanted %d/%u",
                         kFiles[i], Mp3Decoder_ResampleMode(), s_taps,
                         mode, wantTaps);
                Check("opening a file leaves the resampler alone", false, buf);
                Mp3Decoder_Close();
                return;
            }
            Mp3Decoder_Close();
        }
    }

    Check("opening a file leaves the resampler alone", true,
          "every mode, every file");
    Mp3Decoder_SetResampleMode(RESAMPLE_BEST);
}

/* Relief and the rate-cut row drive the same flag from two places.
 *
 * Relief engages on its own when the decoder cannot keep up and releases
 * itself when there is room again. The Diagnostics row sets the same
 * thing by hand. The two met badly: relief released by clearing the flag
 * outright, so a rate cut chosen while relief happened to be engaged was
 * undone a few seconds later - silently, with the row still showing what
 * had been picked.
 *
 * Only the setter is exercised here; engaging is the worker's decision
 * and there is no worker in this build. What is checked is the rule that
 * settles the conflict: whoever asked most recently owns it. */
static void TestReliefOwnership(void)
{
    printf("\nrate cut: relief against an explicit setting\n");
    char buf[128];

    if (!Mp3Decoder_Open("plain.mp3")) { Check("open", false, NULL); return; }

    /* Relief's own path sets the flag and keeps ownership. */
    s_reliefOn = false;
    ReliefSetHalfRate(true);
    s_reliefOn = true;
    Check("relief can reduce the rate without owning the row",
          s_halfRate && s_reliefOn, NULL);

    /* A rate cut chosen by hand, while relief is engaged, takes it. */
    Mp3Decoder_SetHalfRate(true);
    snprintf(buf, sizeof(buf), "half rate %s, relief %s",
             s_halfRate ? "on" : "off", s_reliefOn ? "still on" : "stood down");
    Check("an explicit setting takes the decision back from relief",
          s_halfRate && !s_reliefOn, buf);

    /* And because relief stood down, its release can no longer run and
     * undo what was chosen - which was the whole fault. */
    Check("so nothing later clears a rate cut the user asked for",
          s_halfRate, buf);

    /* Turning it off by hand is equally explicit. */
    Mp3Decoder_SetHalfRate(false);
    Check("and turning it off by hand is honoured too",
          !s_halfRate && !s_reliefOn, NULL);

    /* A new track starts at full quality whatever happened on the last. */
    s_reliefOn = true;
    ReliefSetHalfRate(true);
    Mp3Decoder_Open("plain.mp3");
    snprintf(buf, sizeof(buf), "half rate %s, relief %s after open",
             s_halfRate ? "on" : "off", s_reliefOn ? "on" : "off");
    Check("and a new track is not given the last one's bandwidth",
          !s_halfRate && !s_reliefOn, buf);

    /* Nor the stopwatch that decides it.
     *
     * The worker boosts itself when the ring runs low and engages relief
     * if half a second of boosting has not refilled it. s_boostSince is
     * when that boost started, and opening a track used to leave it
     * pointing at the previous one - so a skip during a boost handed the
     * next track a clock that had nearly run out, and it dropped to half
     * rate on a file that was never struggling. Every open empties the
     * ring, so the worker is boosted across almost every track change:
     * this is the ordinary case, not a corner.
     *
     * The flag is deliberately left alone - the ring really is empty and
     * really does need filling. Only the age carries no meaning. */
    s_workerBoosted = true;
    s_boostSince = 1;                   /* as if it began long ago */
    s_boostBlockUntil = (uint64_t)-1;   /* and left a cooldown behind */
    uint64_t before = s_boostSince;

    Mp3Decoder_Open("plain.mp3");

    snprintf(buf, sizeof(buf), "clock %s, cooldown %s, boost %s",
             s_boostSince != before ? "restarted" : "INHERITED",
             s_boostBlockUntil == 0 ? "cleared" : "KEPT",
             s_workerBoosted ? "left running" : "dropped");
    Check("nor the boost clock that would cut it on the next track",
          s_boostSince != before && s_boostBlockUntil == 0, buf);
    s_workerBoosted = false;

    Mp3Decoder_Close();
}

/* The two output effects, which are off unless somebody asks for them.
 *
 * Both were wrong when first written and measured that way: the
 * enhancer used x*|x|, which keeps the period and so boosts the
 * fundamental instead of making an octave - it doubled a 3kHz tone and
 * added nothing at 6kHz. What is checked here is the behaviour each one
 * is for, because both of them compile and run perfectly well while
 * doing the wrong thing. */
static double ShapeMag(const int16_t *s, int n, double hz)
{
    const double w = 2 * M_PI * hz / 32768.0;
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) { re += s[i*2] * cos(w*i); im += s[i*2] * sin(w*i); }
    return sqrt(re*re + im*im) / n;
}

static void ShapeFill(int16_t *b, int n, double hz, double amp)
{
    for (int i = 0; i < n; i++) {
        int16_t v = (int16_t)(amp * 32767.0 * sin(2 * M_PI * hz * i / 32768.0));
        b[i*2] = v; b[i*2+1] = v;
    }
}

static void TestOutputShaping(void)
{
    printf("\nnoise filter and enhance\n");
    enum { N = 8192 };
    static int16_t b[N*2], ref[N*2];
    char buf[160];
    const int stops = Mp3Decoder_ShapeStops();

    /* Off must be free, and free means untouched. */
    Mp3Decoder_SetNoiseFilter(0); Mp3Decoder_SetEnhance(0);
    ShapeFill(b, N, 1000.0, 0.5); memcpy(ref, b, sizeof(ref));
    ApplyShaping(b, N);
    Check("with both off the samples are not touched at all",
          memcmp(ref, b, sizeof(ref)) == 0, NULL);

    /* Loud music must survive every setting of the expander. */
    double worstMusic = 0; int worstAt = 0;
    for (int lv = 1; lv < stops; lv++) {
        Mp3Decoder_SetNoiseFilter(lv); Mp3Decoder_SetEnhance(0); ResetShaping();
        for (int k = 0; k < 8; k++) { ShapeFill(b, N, 1000.0, 0.5); ApplyShaping(b, N); }
        ShapeFill(ref, N, 1000.0, 0.5);
        double d = fabs(20*log10(ShapeMag(b,N,1000) / ShapeMag(ref,N,1000)));
        if (d > worstMusic) { worstMusic = d; worstAt = lv; }
    }
    snprintf(buf, sizeof(buf), "worst %.3f dB, at level %d", worstMusic, worstAt);
    Check("loud music is left alone at every noise-filter level",
          worstMusic < 0.1, buf);

    /* And quiet hiss on its own must come down, further each step. */
    double prev = 1e9; int notMonotonic = 0; double total = 0;
    for (int lv = 0; lv < stops; lv++) {
        Mp3Decoder_SetNoiseFilter(lv); Mp3Decoder_SetEnhance(0); ResetShaping();
        for (int k = 0; k < 8; k++) { ShapeFill(b, N, 12000.0, 0.002); ApplyShaping(b, N); }
        double m = ShapeMag(b, N, 12000);
        if (lv == 0) total = m;
        else if (m > prev + 0.01) notMonotonic++;
        prev = m;
    }
    snprintf(buf, sizeof(buf), "%.1f dB at the top setting, %d steps out of order",
             20*log10(prev/total), notMonotonic);
    Check("quiet hiss alone is expanded down, further at each step",
          notMonotonic == 0 && prev < total * 0.6, buf);

    /* The enhancer must add an octave, not amplify what is there. */
    Mp3Decoder_SetNoiseFilter(0);
    double fund0, harm0, fundN, harmN;
    Mp3Decoder_SetEnhance(0); ResetShaping();
    for (int k = 0; k < 4; k++) { ShapeFill(b, N, 3000.0, 0.5); ApplyShaping(b, N); }
    fund0 = ShapeMag(b, N, 3000); harm0 = ShapeMag(b, N, 6000);

    Mp3Decoder_SetEnhance(stops - 1); ResetShaping();
    for (int k = 0; k < 4; k++) { ShapeFill(b, N, 3000.0, 0.5); ApplyShaping(b, N); }
    fundN = ShapeMag(b, N, 3000); harmN = ShapeMag(b, N, 6000);

    snprintf(buf, sizeof(buf), "fundamental %+.2f dB, octave %.0f -> %.0f",
             20*log10(fundN/fund0), harm0, harmN);
    Check("enhance adds an octave up rather than more fundamental",
          harmN > 50 && fabs(20*log10(fundN/fund0)) < 0.5, buf);

    Mp3Decoder_SetNoiseFilter(0); Mp3Decoder_SetEnhance(0);
}

int main(void)
{
    printf("DSXP decoder tests\n==================\n");

    TestPlainFile();
    TestResamplerSurvivesOpen();
    TestXingFile();
    TestId3Skip();
    TestSeek();
    TestDrainBeforeFinish();
    TestTruncatedAndGarbage();
    TestPrefill();
    TestDecodeCatchUp();
    TestScrub();
    TestReliefOwnership();
    TestOutputShaping();
    TestResampler();
    TestMonoFold();
    TestOutputRateFollows();
    TestResamplerQuality();
    TestResampleModeRoundTrip();

    /* ---- ID3 tags ---------------------------------------------------- */

    /* The font is ASCII, so the interesting part is not "does it read a
     * tag" but what it does with text it cannot draw. Accented letters
     * have to fold to something readable rather than becoming a row of
     * question marks, and a title must never be silently cut short at the
     * first character the font lacks. */
    {
        static const struct { const char *file, *title, *artist, *why; } kT[] = {
            { "tag_latin1.mp3", "Bjork * Hyperballad", "Bjork",
              "Latin-1 accents fold to their base letter" },
            { "tag_utf16.mp3",  "Cafe - \"Bleu\".", "The Style Council",
              "UTF-16 dashes, curly quotes and ellipsis fold" },
            { "tag_art.mp3",    "Behind The Art", "Buried",
              "text frames found past 60KB of album art" },
            { "tag_v1.mp3",     "Old Tag Title", "Old Tag Artist",
              "ID3v1 used when there is no v2" },
            { "tag_both.mp3",   "From v2", "V1 Artist",
              "v2 title wins, v1 fills what v2 omitted" },
            { "tag_none.mp3",   "", "", "no tag reads as no tag" },
        };

        for (unsigned i = 0; i < sizeof(kT) / sizeof(kT[0]); i++) {
            bool opened = Mp3Decoder_Open(kT[i].file);
            const char *ti = opened ? Mp3Decoder_Title() : "";
            const char *ar = opened ? Mp3Decoder_Artist() : "";
            bool ok = opened && !strcmp(ti, kT[i].title)
                             && !strcmp(ar, kT[i].artist);
            char detail[192];
            snprintf(detail, sizeof(detail), "'%s' / '%s'", ti, ar);
            Check(kT[i].why, ok, detail);
            if (opened) Mp3Decoder_Close();
        }

        /* Persian, romanised.
         *
         * This is the case the fold table was extended for: a library
         * tagged in Persian used to show "?????????" on every line, so
         * one track could not be told from another. The expected strings
         * are the letter-by-letter spellings - not how a Persian speaker
         * would write the name in Latin, which no table can produce -
         * and the point is that they are distinct and readable.
         *
         * The title carries a zero-width non-joiner and the second
         * fixture a harakat; both must vanish rather than become
         * characters. */
        {
            static const struct { const char *file, *title, *artist, *why; } kFa[] = {
                { "tag_fa.mp3",   "nmydanm",  "daryvsh",
                  "Persian in UTF-8 is romanised, not blanked" },
                { "tag_fa16.mp3", "chharshnbh 14", "zndgy",
                  "Persian in UTF-16, with Persian digits" },
            };
            for (unsigned i = 0; i < sizeof(kFa) / sizeof(kFa[0]); i++) {
                bool op = Mp3Decoder_Open(kFa[i].file);
                const char *ti = op ? Mp3Decoder_Title() : "";
                const char *ar = op ? Mp3Decoder_Artist() : "";
                char dd[192];
                snprintf(dd, sizeof(dd), "'%s' / '%s'", ti, ar);
                Check(kFa[i].why,
                      op && !strcmp(ti, kFa[i].title) && !strcmp(ar, kFa[i].artist),
                      dd);
                if (op) Mp3Decoder_Close();
            }
        }

        /* Text the font cannot represent at all must still leave a
         * usable title rather than an empty one. */
        if (Mp3Decoder_Open("tag_utf8.mp3")) {
            char d[128];
            snprintf(d, sizeof(d), "'%s' / '%s'",
                     Mp3Decoder_Title(), Mp3Decoder_Artist());
            Check("unrenderable text degrades rather than vanishing",
                  strcmp(Mp3Decoder_Title(), "Senor") == 0, d);
            Mp3Decoder_Close();
        }
    }

    /* ---- a tag with a picture in it ----------------------------------- */

    /* Nothing reads the picture unless something on screen is going to
     * show it.
     *
     * Reading a cover off the card and decoding it both happen on the
     * thread that opens the track, holding the decoder lock, so with a
     * meter on the top screen that work bought nothing and still cost a
     * pause at the start of every track. This is that gate, checked
     * before it is opened - the default has to be off, or a build that
     * never asks pays anyway. */
    {
        char detail[128];
        if (Mp3Decoder_Open("art.mp3")) {
            int w = 0, h = 0;
            const uint16_t *px = Mp3Decoder_Art(&w, &h);
            snprintf(detail, sizeof(detail), "art %dx%d, title '%s'",
                     w, h, Mp3Decoder_Title());
            Check("with nothing showing it, the picture is not read",
                  px == NULL && w == 0 &&
                  !strcmp(Mp3Decoder_Title(), "Art Test"), detail);
            Mp3Decoder_Close();
        }
    }

    Mp3Decoder_SetWantArt(true);

    /* An APIC frame has to be walked past exactly, or the title after it
     * is never reached - it is large and it comes first. That was true
     * before anything displayed the picture, and it is still the first
     * thing to check. */
    {
        char detail[192];
        bool opened = Mp3Decoder_Open("art.mp3");
        Check("a picture frame is stepped over and the tags after it read",
              opened && !strcmp(Mp3Decoder_Title(), "Art Test")
                     && !strcmp(Mp3Decoder_Artist(), "The Artist"), NULL);

        /* And now the picture itself. The fixture is a gradient - red
         * rising left to right, green top to bottom, blue flat - so the
         * two far corners say whether it decoded, whether it is the
         * right way up, and whether the colour planes are in the right
         * order. A decoder with Cb and Cr crossed still produces a
         * picture, and a test that only asks "is there one" passes. */
        if (opened) {
            int w = 0, h = 0;
            const uint16_t *px = Mp3Decoder_Art(&w, &h);
            snprintf(detail, sizeof(detail), "%dx%d", w, h);
            Check("the cover is decoded to a thumbnail",
                  px && w > 0 && h > 0 && w <= 116 && h <= 116, detail);

            if (px && w > 1 && h > 1) {
                const uint16_t a = px[0], b = px[(h - 1) * w + (w - 1)];
                const int ar = (a >> 0) & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
                const int br = (b >> 0) & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
                snprintf(detail, sizeof(detail),
                         "first %d,%d,%d  last %d,%d,%d", ar, ag, ab, br, bg, bb);
                Check("and it is the picture that was in the file",
                      ar < 8 && ag < 8 && br > 23 && bg > 23 &&
                      ab > 10 && ab < 21 && bb > 10 && bb < 21, detail);
            }

            uint32_t gen = Mp3Decoder_ArtGeneration();
            Mp3Decoder_Close();

            /* A track with no picture must clear the last one, or the
             * card keeps showing the cover of the track before it. */
            if (Mp3Decoder_Open("tag_none.mp3")) {
                int nw = 0, nh = 0;
                const uint16_t *none = Mp3Decoder_Art(&nw, &nh);
                snprintf(detail, sizeof(detail), "generation %u -> %u",
                         (unsigned)gen, (unsigned)Mp3Decoder_ArtGeneration());
                Check("a track with no picture clears the last one",
                      none == NULL && nw == 0 &&
                      Mp3Decoder_ArtGeneration() != gen, detail);
                Mp3Decoder_Close();
            }
        }
    }

    /* The picture behind a UTF-16 description, which is the part of an
     * APIC frame easiest to walk a byte wrong: it ends in two zero
     * bytes, not one, and a parser that stops at the first lands inside
     * the JPEG's start marker.
     *
     * Two files, because one of them proves less than it looks. The
     * decoder has a second way in - a scan of the first few hundred
     * bytes for a start marker, there to rescue exactly this mistake -
     * so a short description is found whether the parse works or not.
     * The long one puts the picture past the end of that window, where
     * only the parse can reach it. */
    {
        static const struct { const char *file, *title, *why; } kDesc[] = {
            { "art_utf16.mp3", "Wide Desc",
              "a UTF-16 description is walked past to the picture" },
            { "art_longdesc.mp3", "Long Desc",
              "and one too long for the fallback scan to rescue" },
        };

        for (unsigned i = 0; i < sizeof(kDesc) / sizeof(kDesc[0]); i++) {
            char detail[160];
            if (!Mp3Decoder_Open(kDesc[i].file)) {
                Check(kDesc[i].why, false, "fixture missing");
                continue;
            }
            int w = 0, h = 0;
            const uint16_t *px = Mp3Decoder_Art(&w, &h);
            snprintf(detail, sizeof(detail), "%dx%d, title '%s'",
                     w, h, Mp3Decoder_Title());
            Check(kDesc[i].why,
                  px && w > 0 && !strcmp(Mp3Decoder_Title(), kDesc[i].title),
                  detail);
            Mp3Decoder_Close();
        }
    }

    /* The picture turns up in the end, whatever the fetcher was told.
     *
     * Two ways it could not, both fixed together and both worth holding
     * down. The fetcher stands down while the audio buffer is low, and
     * on a track the decoder cannot keep ahead of, the buffer never does
     * recover - so the wait had no end to it. And a decode that ran out
     * of its budget was marked done and never attempted again, so a
     * budget meant to bound how long you wait for a stand-in quietly
     * decided whether the cover ever appeared at all.
     *
     * The buffer here is pinned at nothing for the whole test, which is
     * the case the old code could not get out of, and the pace is the
     * one that stands down hardest. */
    {
        char detail[160];
        const int wasPace = 1;

        Mp3Decoder_SetArtPace(0);              /* patient */
        Mp3Decoder_SetArtBudgetMs(1000);
        g_queued = 0;

        /* First, on its own: a stand-down has to end even when the thing
         * it is waiting for never happens. */
        const uint32_t before = s_artSleptUs;
        const bool went = ArtYield();
        const uint32_t slept = s_artSleptUs - before;

        snprintf(detail, sizeof(detail), "gave way after %u us, then %s",
                 slept, went ? "carried on" : "stopped");
        Check("standing down for a buffer that never fills ends",
              slept == (uint32_t)ART_STAND_DOWN_US * ART_STAND_DOWN_MAX,
              detail);

        /* And end to end, with the budget the old code would have let
         * settle the matter. */
        if (Mp3Decoder_Open("art.mp3")) {
            int w = 0, h = 0;
            const uint16_t *px = Mp3Decoder_Art(&w, &h);
            snprintf(detail, sizeof(detail), "%dx%d with the buffer at zero "
                     "and a 1s budget", w, h);
            Check("the cover still arrives on a starved buffer",
                  px && w > 0, detail);
            Mp3Decoder_Close();
        } else {
            Check("the cover still arrives on a starved buffer", false,
                  "fixture missing");
        }

        Mp3Decoder_SetArtPace(wasPace);
        Mp3Decoder_SetArtBudgetMs(0);
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail != 0;
}
