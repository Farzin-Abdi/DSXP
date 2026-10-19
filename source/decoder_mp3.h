#ifndef DSXP_DECODER_MP3_H
#define DSXP_DECODER_MP3_H

#include <stdbool.h>
#include <stdint.h>

/* Cooperative MP3 decoder. Nothing here blocks for long: call
 * Mp3Decoder_Pump() once per main-loop iteration and it decodes only as
 * many MPEG frames as the output buffer has room for, then returns, so
 * input and drawing stay responsive. That is why this drives libmad
 * through its low-level frame API rather than mad_decoder_run(), which
 * decodes to completion in a single call.
 *
 * Output is resampled to AUDIO_OUT_RATE and pushed into audio.c. */

bool Mp3Decoder_Open(const char *path);
void Mp3Decoder_Close(void);
bool Mp3Decoder_IsOpen(void);

/* Returns false once the file has been fully decoded *and* the queued
 * audio has finished playing - not merely when the last byte was read,
 * which would cut off the final half second still sitting in the ring. */
bool Mp3Decoder_Pump(void);

/* Upper bound on frames decoded by one Mp3Decoder_Pump call. Also the
 * ceiling on how long that call can hold the decoder lock. */
#define MP3_MAX_FRAMES_PER_PUMP 12

/* Same as Mp3Decoder_Pump, but returns immediately instead of waiting if
 * the decode worker is mid-frame. For the main thread, which must never
 * block on the decoder. */
bool Mp3Decoder_PumpIfIdle(void);

/* Starts the background decode worker (ARM9 build only; a no-op on the
 * host, where the tests drive Mp3Decoder_Pump directly).
 *
 * Decoding one MPEG frame takes 8-15ms and mad_frame_decode cannot be
 * stopped partway, so calling it from the main loop meant every video
 * frame that contained a decode blew its 16.7ms budget - which is why
 * browsing went sluggish exactly when a track was playing. On a thread
 * below the main one, the same work happens in the time the main thread
 * spends blocked on vblank, and the UI is never held up by it. */
void Mp3Decoder_StartWorker(void);

/* True once the track has been decoded to its end *and* the queued audio
 * has finished playing. Replaces the return value of Mp3Decoder_Pump for
 * callers that no longer pump themselves. */
bool Mp3Decoder_Finished(void);

/* True while the decode worker has raised itself above the UI to refill a
 * buffer that was running low. The caller's cue to stop spending main's
 * time on anything it can do without. */
/* True while the decoder is running at reduced bandwidth because it
 * could not keep up at full. Self-engaging and self-releasing; the count
 * is how many times it has had to. */
bool     Mp3Decoder_ReliefActive(void);
uint32_t Mp3Decoder_ReliefCount(void);

bool Mp3Decoder_IsCatchingUp(void);

/* How many times the worker has had to raise itself above the UI. Each
 * one is a stretch where drawing was not scheduled, so this is the
 * number that explains a stuttering top screen. */
uint32_t Mp3Decoder_CatchUpCount(void);
void     Mp3Decoder_ResetCatchUpCount(void);


/* Resampler operating points.
 *
 * Wide and Clean cost exactly the same - same filter length, different
 * coefficients - and differ only in where the passband ends. Best is the
 * long filter, which is roughly twice the arithmetic.
 *
 * Raw and Lite sit below Fast because the resampler runs on the decoder
 * thread, so its cost is decode cost: cheaper here means the ring drains
 * more slowly, the worker is boosted less often, and the meter spends
 * less of its time halved. Raw does no filtering at all - two multiplies
 * per output sample against Fast's sixteen - and aliases audibly on
 * treble; Lite is a real polyphase filter with half of Fast's taps. */
/* Ordered by cost, cheapest first, which is also the order they appear
 * on the Test page. */
enum {
    RESAMPLE_NONE = 0,   /*  nothing: the nearest sample, no arithmetic */
    RESAMPLE_RAW,        /*  no filter, 2-point linear interpolation */
    RESAMPLE_LITE,       /*  4 taps to  8.2kHz */
    RESAMPLE_FAST,       /*  8 taps to  9.8kHz, 20kHz alias at -34 dB */
    RESAMPLE_WIDE,       /* 12 taps to 14.7kHz, 20kHz alias at -25 dB */
    RESAMPLE_CLEAN,      /* 12 taps to 11.5kHz, 20kHz alias at -52 dB */
    RESAMPLE_BEST,       /* 24 taps to 14.7kHz, 20kHz alias at -73 dB */
    RESAMPLE_MODES
};

void Mp3Decoder_SetResampleMode(int mode);
int  Mp3Decoder_ResampleMode(void);

/* Time one MPEG frame took to decode, last and worst, in microseconds.
 * One frame is 26.1ms of audio at 44.1kHz, so anything approaching that
 * means the decoder is at its limit and no amount of scheduling will
 * help. */








/* How long the last seek's card access took. The only part of a seek
 * whose cost depends on the flashcart rather than on this code. */
uint32_t Mp3Decoder_SeekUs(void);

/* Position within the track, derived from frames the mixer has actually
 * consumed, so it neither runs ahead of the audio by the buffer depth
 * nor advances while paused or underrunning. */
uint32_t Mp3Decoder_ElapsedMs(void);

/* Decodes ahead until the output buffer holds a useful cushion, or the
 * track ends. Call after opening or seeking, *before* letting the mixer
 * run: starting playback against an empty buffer means the stream
 * callback fires before the decoder has produced anything and the track
 * opens with an audible burst of silence. */
void Mp3Decoder_Prefill(void);


/* Switches the resampling filter between a short (cheaper) and a long
 * (better alias rejection) window. Rebuilds the filter table, so call it
 * from a settings change rather than per frame. */
void Mp3Decoder_SetHighQuality(bool high);



/* Track length. Exact when the file carries a Xing/VBRI header (which is
 * most VBR files); otherwise estimated from the first frame's bitrate,
 * which is exact for CBR and approximate for headerless VBR.
 * Mp3Decoder_DurationIsExact() says which one you got. */
uint32_t Mp3Decoder_TotalMs(void);
bool     Mp3Decoder_DurationIsExact(void);

uint32_t Mp3Decoder_SampleRate(void);
uint32_t Mp3Decoder_BitrateKbps(void);
int      Mp3Decoder_Channels(void);
bool     Mp3Decoder_IsVbr(void);

void Mp3Decoder_SeekToMs(uint32_t ms);

/* While true, a seek does not stop to refill the ring - the worker does
 * it. Set for as long as a seek control is held so repeated seeks stay
 * responsive; clearing it tops the buffer up once. */
void Mp3Decoder_SetScrubbing(bool on);
bool Mp3Decoder_IsScrubbing(void);

/* Runs the seek a finished scrub left behind. The decode worker calls
 * this; builds without one must call it themselves. */
void Mp3Decoder_RunPendingSeek(void);
void Mp3Decoder_SeekBy(int32_t deltaMs);

/* Interleaved stereo PCM from the most recently decoded MPEG frame, at
 * the file's own sample rate. The visualizer reads this rather than
 * racing the mixer for the contents of the ring buffer. Valid until the
 * next Mp3Decoder_Pump(). */
const int16_t *Mp3Decoder_LastFrameSamples(int *outFrameCount);

/* ID3 title and artist, or empty strings when the file has no usable tag.
 * Borrowed pointers, valid until the next Mp3Decoder_Open.
 *
 * Album is deliberately not read: there is nowhere on either screen it
 * would go, and a parsed field nobody displays is work done every time a
 * track opens for no one's benefit. */
const char *Mp3Decoder_Title(void);
const char *Mp3Decoder_Artist(void);

/* The embedded picture, decoded to a thumbnail at most 96 pixels on a
 * side, or NULL when the file has none, when it is not a baseline JPEG,
 * or when it is too large to be worth the pause at track start.
 *
 * A borrowed pointer, valid until the next Mp3Decoder_Open. The
 * generation counts every change including the clear, so a caller that
 * caches the pixels knows when to look again - and it changes even when
 * two tracks happen to produce a picture of the same size. */
const uint16_t *Mp3Decoder_Art(int *w, int *h);
uint32_t Mp3Decoder_ArtGeneration(void);

/* Whether anything on screen intends to look at the picture.
 *
 * Reading a cover off the card and decoding it both happen on the thread
 * that opens the track, holding the decoder lock - so with a meter on
 * the top screen that work bought nothing and still cost a pause at the
 * start of every track. Off unless something asks. */
void Mp3Decoder_SetWantArt(bool want);

/* Asks for the picture of a track that is already playing, for the case
 * where the cover was turned on after it started. Returns at once. */
bool Mp3Decoder_LoadArtNow(void);

/* What became of the last cover the player went looking for.
 *
 * Kept because guessing at this from a desk has already cost two rounds
 * of plausible fixes that were not the problem: a slow cover can be a
 * slow card, a large picture, a format this cannot read, or simply a
 * processor with nothing to spare. Those look identical from the outside
 * and completely different from here. The Test page shows it. */
/* A three-band tone control, in whole decibels either side of flat.
 * All three zero turns the stage off entirely rather than running it at
 * unity, so the ordinary path is untouched by the feature existing. */
void Mp3Decoder_SetTone(int bassDb, int midDb, int trebleDb);

/* Whether the resampler refuses the cheapest windows when it is speeding
 * the stream up rather than slowing it down. On; off exists so the
 * artefact it prevents can be heard. */
/* How much is pulled off the card per refill, in bytes. Fewer, larger
 * reads mean fewer bursts of card activity - which is audible on a
 * flashcart through headphones, and is not an audio problem at all. */
/* A first-order lowpass across the top of the band, in Hz; zero is off
 * and is a genuine bypass. Blunt on purpose - it takes cymbals down with
 * whatever else is up there. */
/* Two opt-in output effects, both off at zero and both costing nothing
 * while they are. The noise filter is a downward expander on the top of
 * the band, for material with hiss under it; the enhancer synthesises
 * harmonics to put back the brightness the band limit takes away.
 *
 * Levels run 0 to ShapeStops()-1. NoiseCutDb reports what the expander
 * is doing at this moment, so a row can show whether it is earning its
 * processor. */
/* How many positions each slider has, zero (off) included. A macro so
 * that settings.c can clamp a stored level without having to link the
 * decoder in just to ask. */
#define MP3_SHAPE_STOPS 7

void Mp3Decoder_SetNoiseFilter(int level);
void Mp3Decoder_SetEnhance(int level);
int  Mp3Decoder_NoiseFilter(void);
int  Mp3Decoder_Enhance(void);
int  Mp3Decoder_ShapeStops(void);
int  Mp3Decoder_NoiseCutDb(void);

void     Mp3Decoder_SetTrebleCut(unsigned hz);
unsigned Mp3Decoder_TrebleCut(void);

void     Mp3Decoder_SetReadChunk(unsigned bytes);
unsigned Mp3Decoder_ReadChunk(void);

/* Keeps the synthesis filter's window table in tightly-coupled RAM.
 * Bit-exact - it changes where the numbers live, not what they are. */
void Mp3Decoder_SetFastWindow(bool on);
bool Mp3Decoder_FastWindow(void);

void Mp3Decoder_SetUpsampleGuard(bool on);
bool Mp3Decoder_UpsampleGuard(void);
bool Mp3Decoder_ToneActive(void);

typedef enum {
    ART_NONE = 0,      /* the file has no picture frame */
    ART_TOO_BIG,       /* larger than this will read off a card */
    ART_READ_FAILED,   /* the card did not give it up */
    ART_NOT_JPEG,      /* a PNG, most likely - this reads only JPEG */
    ART_UNDECODABLE,   /* JPEG, but progressive or otherwise not baseline */
    ART_TOO_SLOW,      /* gave up: it would not have finished in the budget */
    ART_OK,
} Mp3ArtOutcome;

typedef struct {
    Mp3ArtOutcome outcome;
    uint32_t bytes;      /* the picture as stored in the tag */
    uint32_t readMs;     /* off the card */
    uint32_t decodeMs;   /* and through the decoder */
} Mp3ArtReport;

void Mp3Decoder_GetArtReport(Mp3ArtReport *out);

/* Fold both channels into one before the resampler runs.
 *
 * The resampler does both channels in the same loop and is about a fifth
 * of the cost of playing anything, so this halves the most expensive
 * loop the decoder has. Offered as a diagnostic, not as a preference:
 * if a stutter goes away with it on, the resampler is where the time is
 * going. */
void Mp3Decoder_SetMono(bool mono);

/* The two decoding options libmad implements.
 *
 * Half rate takes the synthesis filter's short path: half as many
 * samples at half the rate, so the most expensive stage of decoding
 * halves and the resampler gets half as much to do. It costs everything
 * above about eleven kilohertz. Ignoring CRC saves a little and gives up
 * error detection. Both take effect on the next frame. */
/* Whether the last frame's samples are kept for the meter. Only the
 * meter reads them; with a card or nothing on the top screen, converting
 * them is work done for no one. */
void Mp3Decoder_SetWantSamples(bool want);

/* Tells the decoder the output stream's rate has changed, so the
 * resampler can be rebuilt for the new ratio. */
void Mp3Decoder_RateChanged(void);

/* How long a cover may take to decode before it is abandoned, in
 * milliseconds; zero for no limit.
 *
 * There is no way to make a large picture quick - the entropy decode
 * walks every bit the encoder wrote, and the scale only governs the
 * transform, which is a twentieth of the cost. So the choice is to wait
 * or to stop, and this is where that is decided. */
void Mp3Decoder_SetArtBudgetMs(uint32_t ms);

/* How hard the cover fetcher competes: 0 patient, 1 balanced, 2 eager.
 *
 * The wall-clock time a cover takes is its work divided by the share of
 * the processor it gets, and at 320 kbps the decoder wants nearly all of
 * it - so this moves the number far more than anything about the
 * decoding does. Eager is the fastest and the one that can make the list
 * stutter; patient never disturbs anything and can take most of a
 * minute. */
void Mp3Decoder_SetArtPace(int pace);

void Mp3Decoder_SetHalfRate(bool on);
void Mp3Decoder_SetIgnoreCrc(bool on);

/* The decoder's share of real time, averaged over half a second of
 * output. A hundred means it is exactly keeping up and has nothing to
 * lend anything else. */
uint32_t Mp3Decoder_LoadPercent(void);

/* And where it goes: Huffman and requantisation, the polyphase synthesis
 * filter, and the resampler. Each as a percentage of real time, so they
 * sum to LoadPercent. Split because "the decoder is expensive" is not
 * something anyone can act on and "the synthesis filter is two thirds of
 * it" is. */
void Mp3Decoder_LoadSplit(uint32_t *mad, uint32_t *synth, uint32_t *resample);

#endif /* DSXP_DECODER_MP3_H */
