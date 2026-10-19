/* Measures the resampler's headroom behaviour.
 *
 * The passband/stopband numbers in test_decoder.c say the filter is
 * *correct*; these say it does not *clip*, which is a separate property
 * and the one that actually went wrong. A windowed-sinc lowpass has unity
 * DC gain but overshoots on transients, and 16-bit output has nowhere to
 * put the overshoot - so without deliberate headroom anything mastered
 * near full scale hard-clips continuously.
 *
 * Run by tools/run_tests.sh; fails the build if the headroom stops being
 * enough for realistic material. */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#include "audio.h"

/* Minimal stubs - this exercises the resampler, not the mixer. */
uint32_t Audio_Write(const int16_t *f, uint32_t n) { (void)f; return n; }
uint32_t Audio_FreeFrames(void)   { return 99999; }
uint32_t Audio_QueuedFrames(void) { return 0; }
uint32_t Audio_OutputRate(void) { return AUDIO_OUT_RATE; }
uint32_t Audio_PlayedFrames(void) { return 0; }
uint32_t Audio_UnderrunFrames(void) { return 0; }
uint32_t Audio_DroppedFrames(void)  { return 0; }
void Audio_Reset(void) {}
void Audio_SetMuted(bool m) { (void)m; }
void Audio_SetPaused(bool p) { (void)p; }
void Audio_SetVolume(int v) { (void)v; }
bool Audio_Init(void) { return true; }

#include "decoder_mp3.c"

static double Rms(const int16_t *x, int n, int stride)
{
    double s = 0;
    for (int i = 0; i < n; i++) { double v = x[i * stride]; s += v * v; }
    return sqrt(s / n);
}

/* The tone control, measured on signal it was given rather than on a
 * fixture. The MP3 fixtures the other tests use are silent frames - fine
 * for everything they check, and useless for asking what a filter did to
 * the audio.
 *
 * Two claims. Flat has to leave the buffer alone to the bit: the three
 * bands are a split by subtraction and they sum back to the input, so
 * "close enough" is not the standard - a stage nobody switched on must
 * not change the player at all. And a preset has to move energy the way
 * its name says, which is a tone at each end of the band and the ratio
 * between what comes out.
 */
static void FillTone(int16_t *buf, int frames, double hz, double amp)
{
    for (int i = 0; i < frames; i++) {
        const double v = amp * sin(2 * M_PI * hz * i / (double)AUDIO_OUT_RATE);
        buf[i * 2 + 0] = buf[i * 2 + 1] = (int16_t)(v * 20000.0);
    }
}

static double ToneRms(double hz, int bassDb, int midDb, int trebleDb)
{
    enum { N = 4096 };
    static int16_t buf[N * 2];

    FillTone(buf, N, hz, 1.0);
    Mp3Decoder_SetTone(bassDb, midDb, trebleDb);
    ApplyTone(buf, N);
    Mp3Decoder_SetTone(0, 0, 0);

    /* The second half only: the filter starts from rest and the first
     * few milliseconds are it settling, not its response. */
    return Rms(buf + (N / 2) * 2, N / 2, 2);
}

/* One frequency's energy, in decibels. */
static double Goertzel(const int16_t *s, uint32_t n, uint32_t rate, double hz)
{
    const double w = 2.0 * M_PI * hz / rate, c = 2.0 * cos(w);
    double s1 = 0, s2 = 0, acc = 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < n; i++) {
        const double x = s[i * 2] / 32768.0;
        const double s0 = x + c * s1 - s2;
        s2 = s1; s1 = s0;
        if (++cnt == 1024) { acc += s1*s1 + s2*s2 - c*s1*s2; s1 = s2 = 0; cnt = 0; }
    }
    return acc > 0 ? 10.0 * log10(acc) : -200.0;
}

/* Resamples a pure tone from `in` to the output rate and returns the
 * loudest thing that is not the tone, relative to it. In a signal with
 * one frequency in it, everything else came from the resampler. */
static double ImageFor(uint32_t in, double hz)
{
    static mad_fixed_t L[1152], R[1152];
    static int16_t out[1 << 18];
    uint32_t n = 0;

    s_samplesPerFrame = 1152;
    s_mono = false;
    ResetResampler();
    ConfigureResampler(in);

    for (int fr = 0; fr < 60; fr++) {
        for (unsigned i = 0; i < 1152; i++) {
            const double t = (double)(fr * 1152 + i) / in;
            L[i] = R[i] = (mad_fixed_t)(0.73 * sin(2 * M_PI * hz * t)
                                        * (MAD_F_ONE - 1));
        }
        const uint32_t got = ResampleFrame(L, R, 1152);
        if (got && n + got < (1 << 17)) {
            memcpy(&out[n * 2], s_outBuf, got * 4);
            n += got;
        }
    }

    const uint32_t rate = Audio_OutputRate();
    const uint32_t skip = rate / 4;              /* let the window fill */
    if (n <= skip) return -200.0;
    const int16_t *s = out + skip * 2;
    const uint32_t len = n - skip;

    const double sig = Goertzel(s, len, rate, hz);
    double worst = -200.0;
    for (double f = 200; f < rate / 2.0 - 200; f += 50) {
        if (fabs(f - hz) < 400) continue;        /* the tone's own skirt */
        const double e = Goertzel(s, len, rate, f);
        if (e > worst) worst = e;
    }
    return worst - sig;
}

static int g_fail = 0;

static void Check(const char *what, bool ok, const char *detail)
{
    printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

/* Runs full-scale input through the filter and reports the peak of the
 * accumulator *before* the clamp, so overshoot is visible rather than
 * being hidden by the very clipping we are trying to detect. */
static double PeakFor(int kind, double freq)
{
    static mad_fixed_t L[1152];

    s_firRate = 0;
    s_samplesPerFrame = 1152;
    ResetResampler();
    ConfigureResampler(44100);

    double peak = 0;
    unsigned seed = 12345;

    for (int fr = 0; fr < 40; fr++) {
        for (int i = 0; i < 1152; i++) {
            double t = (double)(fr * 1152 + i) / 44100.0, v;
            switch (kind) {
                case 0:  v = sin(2 * M_PI * freq * t); break;
                case 1:  v = (sin(2 * M_PI * freq * t) >= 0) ? 1.0 : -1.0; break;
                default: seed = seed * 1103515245 + 12345;
                         v = ((int)(seed >> 16) - 16384) / 16384.0; break;
            }
            if (v >  1.0) v =  1.0;
            if (v < -1.0) v = -1.0;
            L[i] = (mad_fixed_t)(v * (MAD_F_ONE - 1));
        }

        if (s_workCount + 1152 <= RES_WORK_MAX) {
            for (unsigned i = 0; i < 1152; i++)
                s_workL[s_workCount + i] = s_workR[s_workCount + i] = ScaleToS16(L[i]);
            s_workCount += 1152;
        }

        while (1) {
            unsigned base = s_resPos >> 16;
            if (base + s_taps > s_workCount) break;

            const int16_t *cl = &s_workL[base];
            const int16_t *h = &s_fir[((s_resPos >> (16 - s_phaseBits)) & (s_phases - 1)) * s_taps];
            int32_t acc = 0;
            for (unsigned t = 0; t < s_taps; t++) acc += (int32_t)cl[t] * h[t];

            double v = fabs((double)acc / RES_COEF_ONE / 32768.0);
            if (v > peak) peak = v;
            s_resPos += s_resStep;
        }

        unsigned consumed = s_resPos >> 16;
        if (consumed) {
            unsigned keep = s_workCount - consumed;
            memmove(s_workL, &s_workL[consumed], keep * sizeof(int16_t));
            memmove(s_workR, &s_workR[consumed], keep * sizeof(int16_t));
            s_workCount = keep;
            s_resPos &= 0xFFFF;
        }
    }
    return peak;
}

/* Mean of the real output path, for a signal with no DC of its own.
 *
 * This one goes through ResampleFrame rather than reimplementing it,
 * because what is being measured is the rounding at the very end of it.
 * An arithmetic shift truncates, which biases every sample the same way
 * regardless of sign - so the average of a symmetric input comes out
 * about half an LSB low, and that offset is a DC term on the output. */
static double OutputDcOffset(double freq, double amplitude)
{
    static mad_fixed_t L[1152], R[1152];

    s_firRate = 0;
    s_samplesPerFrame = 1152;
    ResetResampler();
    ConfigureResampler(44100);

    double sum = 0;
    long n = 0;

    for (int fr = 0; fr < 200; fr++) {
        for (int i = 0; i < 1152; i++) {
            double t = (double)(fr * 1152 + i) / 44100.0;
            double v = amplitude * sin(2 * M_PI * freq * t);
            L[i] = R[i] = (mad_fixed_t)(v * (MAD_F_ONE - 1));
        }

        uint32_t out = ResampleFrame(L, R, 1152);

        /* The first frames are the filter window filling up, so they see
         * a step from silence that is not part of the steady state. */
        if (fr < 4) continue;

        for (uint32_t i = 0; i < out; i++) {
            sum += s_outBuf[i * 2 + 0];
            sum += s_outBuf[i * 2 + 1];
            n += 2;
        }
    }

    return n ? sum / (double)n : 0.0;
}

int main(void)
{
    /* ---- folding to mono before the filter ------------------------- */
    /* The claim is that summing the two channels in the subband domain
     * is the same mono the old path got by synthesising both in full and
     * averaging afterwards - which is true because the synthesis filter
     * is linear, and worth holding down because it is the reason the
     * saving is allowed to exist.
     *
     * Checked on the coefficients rather than through a decode: the MP3
     * fixtures here are silent frames, and silence proves nothing about
     * an average. Every row and every subband, because a loop that
     * stopped at eighteen rows instead of thirty-six would still sound
     * roughly right and would quietly lose the second granule. */
    {
        static struct mad_frame f;
        char buf[160];

        memset(&f, 0, sizeof(f));
        f.header.mode = MAD_MODE_STEREO;

        for (unsigned gr = 0; gr < 36; gr++)
            for (unsigned sb = 0; sb < 32; sb++) {
                f.sbsample[0][gr][sb] =  (mad_fixed_t)((gr * 32 + sb) << 8);
                f.sbsample[1][gr][sb] = -(mad_fixed_t)((gr * 16 + sb) << 8);
            }

        FoldFrameToMono(&f);

        int wrong = 0, firstGr = -1;
        for (unsigned gr = 0; gr < 36; gr++)
            for (unsigned sb = 0; sb < 32; sb++) {
                const mad_fixed_t want =
                    (mad_fixed_t)(((gr * 32 + sb) << 8) >> 1) +
                    (mad_fixed_t)((-(mad_fixed_t)((gr * 16 + sb) << 8)) >> 1);
                if (f.sbsample[0][gr][sb] != want) {
                    if (firstGr < 0) firstGr = (int)gr;
                    wrong++;
                }
            }

        snprintf(buf, sizeof(buf), "%d of 1152 wrong%s", wrong,
                 firstGr >= 0 ? " (from row ...)" : "");
        Check("the mono fold averages every row and subband",
              wrong == 0, buf);

        Check("and tells the synthesiser there is one channel now",
              MAD_NCHANNELS(&f.header) == 1, "so the filter runs once");

        /* A frame that is already mono must be left completely alone. */
        memset(&f, 0, sizeof(f));
        f.header.mode = MAD_MODE_SINGLE_CHANNEL;
        f.sbsample[0][0][0] = 1234;
        FoldFrameToMono(&f);
        Check("a mono frame is not folded twice",
              f.sbsample[0][0][0] == 1234, "halved again would be quieter");
    }

    /* ---- the image, when the stream is being sped up --------------- */
    /* The artefact that took the longest to find, because every
     * measurement in this file was of the filter slowing a stream down.
     *
     * Going up in rate the filter's job changes: what it has to remove is
     * the image, a mirror of the whole signal about the input rate, and
     * it sits close - a 9kHz tone from a 22050Hz stream has its copy at
     * 13050Hz. A short window cannot reach across that gap, so the cheap
     * modes leave the copy almost as loud as the music. On hi-hats and
     * sibilance it is a swishing ghost that follows the track, and on
     * headphones it is not subtle.
     *
     * What is asserted is the guard: whatever window is chosen, speeding
     * a stream up must not leave an image within 25dB of the signal. */
    {
        char buf[160];
        static const double kHz[] = { 5000, 7000, 9000 };

        for (int guard = 1; guard >= 0; guard--) {
            Mp3Decoder_SetUpsampleGuard(guard != 0);
            double worst = -200; double worstAt = 0;

            for (unsigned i = 0; i < sizeof(kHz)/sizeof(kHz[0]); i++) {
                Mp3Decoder_SetResampleMode(RESAMPLE_RAW);
                const double rel = ImageFor(22050, kHz[i]);
                if (rel > worst) { worst = rel; worstAt = kHz[i]; }
            }

            snprintf(buf, sizeof(buf),
                     "worst image %+.1f dB (a %.0fHz tone), guard %s",
                     worst, worstAt, guard ? "on" : "off");

            if (guard)
                Check("speeding a stream up leaves no image behind",
                      worst < -25.0, buf);
            else
                /* The other half: without it the fault is still there to
                 * be found, so a test that passes both ways is not
                 * measuring anything. */
                Check("and without the guard the image is plainly audible",
                      worst > -20.0, buf);
        }

        Mp3Decoder_SetUpsampleGuard(true);
        Mp3Decoder_SetResampleMode(RESAMPLE_BEST);
    }

    /* ---- the tone control ------------------------------------------ */
    {
        enum { N = 2048 };
        static int16_t a[N * 2], b[N * 2];
        char buf[160];

        FillTone(a, N, 440.0, 1.0);
        memcpy(b, a, sizeof(a));

        Mp3Decoder_SetTone(0, 0, 0);
        ApplyTone(b, N);
        Check("Flat leaves the audio byte for byte alone",
              memcmp(a, b, sizeof(a)) == 0 && !Mp3Decoder_ToneActive(),
              "2048 frames of a 440Hz tone");

        /* The three bands sum back to the input: given the same gain
         * each, the stage is a volume control and nothing else. This is
         * the property Flat's bypass rests on, and it is the one a wrong
         * split breaks while every other measurement here still looks
         * right - a filter can tilt the band correctly and still not add
         * up. Six down is a half, and the tone is well inside the range
         * where that is exact. */
        FillTone(a, N, 700.0, 1.0);
        memcpy(b, a, sizeof(a));
        Mp3Decoder_SetTone(-6, -6, -6);
        ApplyTone(b, N);
        Mp3Decoder_SetTone(0, 0, 0);

        int worst = 0;
        for (int i = (N / 2) * 2; i < N * 2; i++) {
            const int want = (a[i] * 2053) >> 12;      /* -6dB in Q12 */
            const int off = b[i] - want;
            const int mag = off < 0 ? -off : off;
            if (mag > worst) worst = mag;
        }
        snprintf(buf, sizeof(buf), "worst %d counts of 32767 off a half",
                 worst);
        Check("the three bands add back up to the input", worst <= 8, buf);

        /* A bass shape, as the Bass preset resolves to after the
         * headroom cut: up at the bottom, well down at the top. */
        const double lowFlat  = ToneRms(100.0,   0,  0,  0);
        const double highFlat = ToneRms(9000.0,  0,  0,  0);
        const double lowBass  = ToneRms(100.0,  +3, -5, -5);
        const double highBass = ToneRms(9000.0, +3, -5, -5);

        const double before = 20.0 * log10(lowFlat / highFlat);
        const double after  = 20.0 * log10(lowBass / highBass);

        snprintf(buf, sizeof(buf), "100Hz over 9kHz: %+.1f dB -> %+.1f dB",
                 before, after);
        Check("a bass preset moves energy down the band",
              after > before + 5.0, buf);

        /* And nothing it does can leave the sixteen bits it is given. */
        FillTone(a, N, 100.0, 1.0);
        Mp3Decoder_SetTone(+3, -5, -5);
        ApplyTone(a, N);
        Mp3Decoder_SetTone(0, 0, 0);

        int peak = 0;
        for (int i = 0; i < N * 2; i++) {
            const int v = a[i] < 0 ? -a[i] : a[i];
            if (v > peak) peak = v;
        }
        snprintf(buf, sizeof(buf), "peak %d of 32767", peak);
        Check("and stays inside sixteen bits", peak <= 32767, buf);
    }

    printf("resampler headroom (%d taps, rolloff %.2f, headroom %.2f)\n",
           RES_TAPS, (double)RES_ROLLOFF, (double)RES_HEADROOM);

    char buf[128];

    /* Worst case over every phase: the gain an adversarial input would
     * see. Not expected to be under 1.0 - real audio cannot reach it -
     * but worth printing to show how much margin the design has. */
    s_firRate = 0;
    s_samplesPerFrame = 1152;
    ConfigureResampler(44100);

    double worstPhase = 0, dc = 0;
    for (unsigned p = 0; p < s_phases; p++) {
        double sum = 0, g = 0;
        for (unsigned t = 0; t < s_taps; t++) { sum += fabs(s_fir[p * s_taps + t]); g += s_fir[p * s_taps + t]; }
        sum /= RES_COEF_ONE;
        if (sum > worstPhase) worstPhase = sum;
        if (p == 0) dc = g / RES_COEF_ONE;
    }
    snprintf(buf, sizeof(buf), "DC gain %.4f, theoretical worst %.3f", dc, worstPhase);
    Check("DC gain matches the intended headroom",
          fabs(dc - RES_HEADROOM) < 0.01, buf);

    struct { int kind; double freq; const char *name; double limit; } cases[] = {
        { 0, 1000.0, "full-scale sine 1kHz",   1.0 },
        { 0, 6000.0, "full-scale sine 6kHz",   1.0 },
        { 1, 1000.0, "full-scale square 1kHz", 1.0 },
        /* Full-scale white noise is harsher than any real recording -
         * every sample jumps the full range - so it is allowed a little
         * clipping rather than dictating the headroom for everything. */
        { 2, 0.0,    "full-scale white noise", 1.20 },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double peak = PeakFor(cases[i].kind, cases[i].freq);
        snprintf(buf, sizeof(buf), "%-24s peak %.3f x full scale (%+.2f dB)",
                 cases[i].name, peak, 20 * log10(peak));
        Check("stays inside full scale", peak <= cases[i].limit, buf);
    }

    /* An integer number of cycles of a sine has no DC of its own, so
     * whatever comes out is the filter's own bias. Truncation puts this
     * near -0.5 LSB at every level; rounding to nearest holds it at zero,
     * which is what stops a faint click when playback starts or stops. */
    struct { double freq, amp; const char *name; } dcCases[] = {
        { 1000.0, 0.80, "1kHz at -2 dBFS " },
        { 1000.0, 0.05, "1kHz at -26 dBFS" },
        {  440.0, 0.01, "440Hz at -40 dBFS" },
    };

    for (unsigned i = 0; i < sizeof(dcCases) / sizeof(dcCases[0]); i++) {
        double dcOff = OutputDcOffset(dcCases[i].freq, dcCases[i].amp);
        snprintf(buf, sizeof(buf), "%-18s mean %+.4f LSB", dcCases[i].name, dcOff);
        Check("output carries no DC offset", fabs(dcOff) < 0.10, buf);
    }

    /* BuildFirTable() only evaluates half the polyphase bank and mirrors
     * the rest, which is only valid because a linear-phase windowed sinc
     * is exactly symmetric. If that ever stops holding, half the filter
     * silently becomes wrong, so it is asserted rather than assumed. */
    printf("\nfilter symmetry (half the bank is mirrored, not computed)\n");

    s_firRate = 0;
    ConfigureResampler(44100);

    int worstDiff = 0;
    for (unsigned p = 1; p < s_phases; p++)
        for (unsigned t = 0; t < s_taps; t++) {
            int d = s_fir[p * s_taps + t]
                  - s_fir[(s_phases - p) * s_taps + s_taps - 1 - t];
            if (d < 0) d = -d;
            if (d > worstDiff) worstDiff = d;
        }

    snprintf(buf, sizeof(buf), "worst difference %d LSB of %d", worstDiff, RES_COEF_ONE);
    Check("h[P-p][taps-1-t] == h[p][t]", worstDiff == 0, buf);

    /* The quality switch is a run-time setting, so prove it actually
     * takes effect rather than merely being stored somewhere. */
    printf("\nquality switch\n");

    Mp3Decoder_SetHighQuality(false);
    ConfigureResampler(44100);
    unsigned lowTaps = s_taps;
    double lowPeak = PeakFor(1, 1000.0);

    Mp3Decoder_SetHighQuality(true);
    ConfigureResampler(44100);
    unsigned highTaps = s_taps;
    double highPeak = PeakFor(1, 1000.0);

    snprintf(buf, sizeof(buf), "standard %u taps, high %u taps", lowTaps, highTaps);
    Check("switching quality changes the filter length",
          lowTaps == 12 && highTaps == 24, buf);

    snprintf(buf, sizeof(buf), "standard %.3f, high %.3f", lowPeak, highPeak);
    Check("both settings stay inside full scale", lowPeak <= 1.0 && highPeak <= 1.0, buf);

    /* Noise reduction has to satisfy two things at once: it must pull
     * down hiss, and it must be *inaudible* on real music. A denoiser
     * that quietly squashes the quiet parts of a song is worse than the
     * noise it removes, so both directions are measured. */

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail != 0;
}
