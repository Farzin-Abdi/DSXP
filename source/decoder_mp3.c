#include "decoder_mp3.h"
#include "jpeg.h"

#include <nds.h>

#include <mad.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"

/* Input is read in 8KB bites. libmad also wants MAD_BUFFER_GUARD zero
 * bytes past the end of whatever we hand it, so its bit reader can look
 * ahead across the final frame boundary without a bounds check on every
 * bit - hence the oversized array. Skipping that is a real, if rare,
 * crash rather than a style point. */
/* Deliberately small, and it must stay that way.
 *
 * The obvious optimisation here is a bigger buffer: a 320 kbps frame is
 * 1044 bytes against 417 at 128 kbps, so this empties two and a half
 * times as often on high-bitrate files, and every refill is a card read
 * through DLDI - the slowest thing the decoder does. Going to 32KB does
 * cut the number of reads to a quarter.
 *
 * It also breaks playback, and the reason is worth keeping written down.
 * The refill happens inside the decoder lock and produces no audio while
 * it runs, so the read time is dead air the ring has to cover. DLDI reads
 * run somewhere around 100-500 KB/s depending on the card, which puts a
 * 32KB read at anywhere up to ~300ms against a ring that holds 500ms.
 * Fewer, larger stalls is the wrong trade when the buffer has to absorb
 * each one whole: 8KB stalls are comfortably swallowed, 32KB ones are not
 * and the mixer starves. Read frequency is not the constraint - worst-case
 * stall length is. */
/* The most the file buffer can hold. How much of it is actually asked
 * for per refill is a run-time setting - see Mp3Decoder_SetReadChunk. */
#define INPUT_BUFFER_SIZE (64 * 1024)

/* Eight, which is what the paragraph above concludes.
 *
 * It said 32KB "also breaks playback" and then defined the default as
 * 32KB, which is the state this shipped in. The runtime setter below
 * once had a row on the Test page and the row was removed; the constant
 * it had been overriding was left behind at the wrong value, so nothing
 * called the setter any more and every refill read four times what the
 * reasoning allows. At 249kbps that is a stall of up to a third of a
 * second arriving about once per second of audio - which is both the
 * starvation the comment predicts and, at that cadence, exactly what it
 * sounds like. */
#define INPUT_CHUNK_DEFAULT (8 * 1024)

/* How much of it a refill asks for; see Mp3Decoder_SetReadChunk. */
static size_t s_readChunk = INPUT_CHUNK_DEFAULT;

/* Worst case output frames from one MPEG frame after resampling: 1152
 * input samples (layer II) from an 8kHz MPEG 2.5 stream stretch to
 * 1152 * 32768/8000 = 4719 frames. Everything realistic is far smaller,
 * but the buffer has to survive the pathological case. */
#define MAX_OUT_FRAMES 4800

/* Max samples one decoded MPEG frame can produce per channel. libmad
 * hardcodes this as the size of mad_pcm.samples[]; it does not export a
 * constant for it. */
#define MAD_MAX_SAMPLES 1152

static struct mad_stream s_stream;
static struct mad_frame  s_frame;
static struct mad_synth  s_synth;

static unsigned char s_inputBuf[INPUT_BUFFER_SIZE + MAD_BUFFER_GUARD];
static FILE *s_file = NULL;
/* ---- decode worker -------------------------------------------------
 *
 * Everything that touches decoder state runs under s_lock. There are only
 * two parties: the worker thread, which holds it for the length of one
 * pump, and the main thread, which takes it to open, seek or close. The
 * ring buffer between the decoder and maxmod is untouched by this - it
 * was already single-producer/single-consumer and still is, the producer
 * has simply moved off the main thread. */
/* Absolute frame counts, not fractions of the ring.
 *
 * As fractions they tracked the ring size, so doubling the ring would
 * have doubled how much surplus a boost has to rebuild before it stands
 * down - making every episode twice as long to buy episodes that are
 * rarer. The point of the larger ring is more margin before boosting, not
 * a longer boost, and these are the levels that behaved well at 125ms and
 * 187ms of queued audio. */
#define BOOST_ON_BELOW  4096u   /* 125ms at 32768Hz */
#define BOOST_OFF_ABOVE 6144u   /* 187ms */

/* A refill has to fit in the audio that is left when things are already
 * going badly.
 *
 * The read happens under the decoder lock and produces nothing while it
 * runs, so its length is dead air the ring has to cover. The moment that
 * matters is not the comfortable one - it is when the ring has fallen to
 * the boost mark and the next refill still has to happen. If one read
 * can outlast what is left at that point, the mixer starves however the
 * priorities are arranged.
 *
 * Checked against the slowest DLDI speed the note at the top of this
 * file cites, because the fast cards were never the problem. Written as
 * an assertion so that raising the chunk size fails here, next to the
 * reasoning, rather than on somebody's card. */
#define SLOWEST_CARD_BYTES_PER_SEC (100u * 1024u)
_Static_assert(
    (INPUT_CHUNK_DEFAULT * 1000ull) / SLOWEST_CARD_BYTES_PER_SEC
        <= (BOOST_ON_BELOW * 1000ull) / AUDIO_OUT_RATE,
    "one refill can last longer than the audio left when the decoder "
    "boosts - the mixer will starve on a slow card");

/* How long the worker may hold priority over the UI before standing down
 * whether the ring recovered or not, and how long it must then stay down.
 *
 * The boost is a latch: on below 4096 frames, off above 6144. That works
 * when the decoder is faster than realtime and the ring is merely behind
 * - it catches up, crosses the upper mark, and lets go. It does not work
 * when the decoder is slower than realtime, because then the ring never
 * reaches the upper mark, the worker never lets go, and it sits above the
 * main thread forever. The console stops drawing and stops reading
 * buttons: the music you cannot keep up with is also the music you cannot
 * pause.
 *
 * A decoder that cannot keep up should stutter, not take the console with
 * it. So the boost is capped in time as well as by the ring, and the
 * cooldown after it is long enough for the main thread to actually do a
 * frame's work rather than being immediately preempted again. */
#define BOOST_MAX_TICKS   (TICK_FREQ / 2)      /* half a second held */
#define BOOST_COOLDOWN    (TICK_FREQ / 20)     /* 50ms yielded */

/* Relief: what to do about the stutter the cap above accepts.
 *
 * Standing down is right - a console that cannot be paused is worse than
 * one that skips - but on its own it only decides who gets the processor,
 * and the problem at that point is that there is not enough of it. So the
 * cap fires, the ring is still empty, fifty milliseconds go by
 * unprotected, the ring drains further, and the cap fires again: a cycle
 * of about half a second that is heard as a rasp repeating roughly twice
 * a second, for as long as the passage lasts.
 *
 * Measured, that is not a hypothetical. A 48kHz file holding 249kbps
 * across five seconds costs about 1.46 times a 44.1kHz CBR 192 that
 * plays perfectly - and half rate takes it to 0.66 of its own cost,
 * which is below the file that works. So the answer to "there is not
 * enough processor" is to need less of it, and only while it is true.
 *
 * Released on a real surplus held for several seconds, not the moment
 * the ring recovers: coming straight back up would re-enter the same
 * shortfall and swap one rasp for a wobble between two bandwidths. The
 * rate change costs a filter rebuild at each end, which is the other
 * reason not to do it often. */
#define RELIEF_RELEASE_TICKS (TICK_FREQ * 4)

/* And the same pair for a decoder that is not merely behind but slower
 * than the audio it produces.
 *
 * Boosting exists to let the decoder catch up. If a step costs more than
 * the audio it made, there is nothing to catch up to - the ring will
 * never reach the upper mark however long the worker holds priority, so
 * holding it only takes the console away from the person using it while
 * the sound breaks up anyway.
 *
 * Half and half rather than nine parts to one. The audio is going to
 * stutter either way; what this decides is whether the buttons still
 * work while it does. */
#define BOOST_LOST_HOLD   (TICK_FREQ / 10)     /* 100ms held */
#define BOOST_LOST_YIELD  (TICK_FREQ / 10)     /* 100ms yielded */
_Static_assert(BOOST_OFF_ABOVE < AUDIO_RING_FRAMES,
               "the boost could never stand down");

/* Also absolute, and for the same reason: how much has to be buffered
 * before a track starts and before a resume unmutes is a question about
 * milliseconds of audio, not about what fraction of the ring that is. */
#define PREFILL_FRAMES       4096u   /* 125ms */
#define RESUME_READY_FRAMES  8192u   /* 250ms */

/* Set while the decoder is being given less to do because it could not
 * keep up. Separate from s_halfRate, which is the user's own setting:
 * relief never engages on top of a rate cut that is already in force,
 * and never takes one away.
 *
 * Outside the worker's #ifdef even though only the worker engages it:
 * Mp3Decoder_Open clears it and the Test page reads it, and both of
 * those are built on the host where there is no worker at all. */
static bool     s_reliefOn = false;
static uint32_t s_reliefCount = 0;
static uint64_t s_reliefEasySince = 0;

/* The worker's priority boost, and the clock that turns a boost which
 * is not working into relief.
 *
 * Out here for the same reason as the three above, and it is the same
 * reason: Mp3Decoder_Open resets them, and Open is built on the host
 * where there is no worker. Left inside the worker's #ifdef, the reset
 * could not be written at all - which is how the clock came to survive
 * a track change and cut the next one's bandwidth. */
static bool     s_workerBoosted = false;
static uint32_t s_boostCount = 0;
static uint64_t s_boostSince = 0;     /* when the current boost started */
static uint64_t s_boostBlockUntil = 0;/* no boosting before this tick */

#ifdef ARM9
#include <calico.h>
static Mutex   s_lock;
static Thread  s_worker;
static bool    s_workerRunning = false;

/* The worker's two priorities.
 *
 * Below the UI while the buffer is comfortable, so decoding fills the
 * time the main thread spends blocked on vblank and never delays a
 * frame. Above the UI once the buffer is genuinely at risk, so it
 * preempts drawing and recovers.
 *
 * This replaces the main thread pitching in with a decode of its own,
 * which is how the buffer used to be defended. That worked - audio always
 * won - but it won by blocking the UI on the decoder lock for as long as
 * a twelve-frame catch-up burst, and simply removing it left decoding
 * with whatever CPU the UI happened not to use. At 320 kbps that is not
 * enough, and the shortfall never recovers. Priority says the same thing
 * the blocking backstop said - audio outranks drawing when it has to -
 * without anybody waiting on a lock to hear it.
 *
 * The mixer thread stays above both, so it can never be starved by
 * either. */
#define WORKER_PRIO_IDLE   (MAIN_THREAD_PRIO + 4)
#define WORKER_PRIO_URGENT (MAIN_THREAD_PRIO - 1)

/* Boost when a quarter of the ring is left (~125ms of audio), release at
 * three eighths (~190ms). A single threshold would flip priority every
 * few milliseconds around the boundary; a much wider band would be worse
 * in the other direction, because the whole boosted stretch is time the
 * UI is not being scheduled. Recovering 62ms of audio is about three
 * MPEG frames, so a burst is short enough to read as a hitch rather than
 * a freeze. */

/* 16KB, which is libmad and a margin.
 *
 * It was 64KB while four other codecs were here, sized from the largest
 * stack frame each of them had: FAAD2 needed 40960 bytes in one function
 * and dr_flac 16384, against libmad's 7104. With those gone the number
 * that matters is libmad's again.
 *
 * The decode still happens on this thread rather than the main one. That
 * is not about libmad - 7104 fits the 13800 bytes DTCM leaves below
 * __sp_usr - but the main thread's stack is DTCM, and a decoder is the
 * wrong thing to run on a stack that small even when it fits today. */
alignas(8) static uint8_t s_workerStack[16 * 1024];

#define DEC_LOCK()   mutexLock(&s_lock)
#define DEC_UNLOCK() mutexUnlock(&s_lock)
#else
/* No threads on the host, so there is nothing to serialise - but there
 * is something to check. Calico's mutex is not recursive, so a path that
 * takes this lock while already holding it hangs the console and does it
 * nowhere near the line that caused it. The host cannot reproduce that,
 * and it can notice it: the depth is counted, and going above one is a
 * failure at the call site rather than a lock-up on hardware.
 *
 * Worth having because the set of functions that take the lock just
 * grew, and the ones it grew by are called from the UI thread with no
 * obvious sign of what else might be holding it. */
static int s_lockDepth = 0;
static void DecLockHost(const char *where)
{
    if (++s_lockDepth > 1) {
        fprintf(stderr, "decoder lock taken recursively at %s - "
                        "this deadlocks on hardware\n", where);
        abort();
    }
}
#define DEC_LOCK()   DecLockHost(__func__)
#define DEC_UNLOCK() (--s_lockDepth)
#endif

static bool  s_finished = false;
static bool     s_scrubbing = false;
static uint32_t s_scrubTargetMs = 0;
static bool     s_committing = false;

/* Set when a scrub ends: the worker owes a seek, and the output stays
 * muted until the ring has recovered from it. */
static volatile bool s_seekPending = false;
static volatile bool s_resuming = false;
static uint32_t s_seekUs = 0;

static void SeekLocked(uint32_t ms);

static bool  s_opened = false;

static long s_fileSize   = 0;
static long s_dataStart  = 0;   /* first byte after any ID3v2 tag */

/* What the tags said, or empty. Shown in place of the filename when
 * present - a file called "03.mp3" is not a track name. */
static char s_title[64], s_artist[64];

/* The embedded cover, decoded to a thumbnail.
 *
 * Kept here rather than handed out as bytes because this is where the
 * tag is already being walked. The UI collects the result the same way
 * it collects the title.
 *
 * A hundred and sixteen on a side, which is the square the card gives it
 * beside the column of details. If the card and this ever disagree the
 * picture is simply centred in the space with a margin, which is a
 * cosmetic mismatch rather than an overrun.
 *
 * Written only by the fetch below, and only ever while s_artW is zero -
 * which is published before any byte of it is touched, so the UI is
 * showing the stand-in rather than reading a buffer being filled in
 * underneath it. That is what makes one buffer enough. */
#define ART_BOX 116
static uint16_t s_art[ART_BOX * ART_BOX];

/* volatile because these are the handoff, not just numbers.
 *
 * The rule above - the fetcher writes the buffer only while s_artW is
 * zero, and zero is published before a byte of it is touched - is a
 * rule about what the other thread can observe, and it only holds if
 * the compiler is made to actually load and store these where the code
 * says to. Without it the UI is entitled to read s_artW once and keep
 * the answer, which is the one thing the rule forbids. */
static volatile int      s_artW = 0, s_artH = 0;
static volatile uint32_t s_artGen = 0;

/* Where this track's picture is, found while the tag was walked. */
static char s_artPath[512];
static long s_artOffset = 0, s_artSize = 0;
static bool s_artV22 = false;

/* The request, and what has been answered.
 *
 * A number rather than a flag, so a fetch that is still running when the
 * next track starts can see that it has been overtaken and throw its
 * work away instead of publishing a cover for a track that has already
 * finished. Skipping through a folder with L and R does exactly that. */
static volatile uint32_t s_artSeq = 0, s_artDone = 0;

/* The largest picture worth reading off a card.
 *
 * Cover art in the wild runs from twenty kilobytes to about two hundred;
 * past this it is a scan of a gatefold that somebody embedded by
 * accident, and reading it costs a visible pause at the start of every
 * track for a picture that will be shown ninety-six pixels wide. */
#define ART_MAX_BYTES (320u * 1024u)

static void ReadArtFrame(const unsigned char *body, size_t n, bool v22);

/* Reads the recorded picture off the card and decodes it.
 *
 * Everything about this is arranged so that it never delays anything
 * else. The card is read in chunks so the sequence number can be checked
 * between them, and the decode itself is pure arithmetic on a heap
 * buffer.
 *
 * `seq` is checked before it starts, between chunks and again before
 * publishing. A track skipped away from stops being fetched partway
 * through rather than finishing and putting the wrong cover on screen.
 *
 * No lock is held for any of it - see the reasoning at the read below.
 * This used to say the opposite: that the decoder's lock was taken
 * around each chunk, and that this was what kept two threads out of
 * libfat at once. The read stopped taking it deliberately and correctly
 * - libfat has its own lock, and taking the decoder's as well was what
 * made a cover take twenty seconds to appear - but this paragraph was
 * left behind stating the safety argument backwards, which is worse than
 * having none. */
/* Eight kilobytes, and the size still matters even though this thread
 * holds no lock of its own.
 *
 * libfat takes its own lock for the duration of a read, so while this
 * one runs the decoder cannot touch the card either - and a decoder that
 * cannot read is a ring that drains, which ends in the worker preempting
 * the UI to catch up. A card managing 300KB a second is about thirty
 * milliseconds over eight kilobytes, which the ring absorbs; it is a
 * fifth of a second over sixty-four, which it does not.
 *
 * Chunking also lets the sequence number be checked between reads, so a
 * track skipped away from stops being fetched partway through. */
#define ART_CHUNK 8192

static volatile Mp3ArtReport s_artReport;

void Mp3Decoder_GetArtReport(Mp3ArtReport *out)
{
    if (!out) return;
    out->outcome  = s_artReport.outcome;
    out->bytes    = s_artReport.bytes;
    out->readMs   = s_artReport.readMs;
    out->decodeMs = s_artReport.decodeMs;
}

static uint32_t ElapsedMsSince(uint64_t t0)
{
    const uint64_t now = tickGetCount();
    return (uint32_t)(((now - t0) * 1000u) / TICK_FREQ);
}

/* Stands the cover decode down whenever the audio buffer is not
 * comfortable.
 *
 * This is what makes it safe for the fetcher to hold a priority above
 * the decoder at rest. Without it, the cover simply took the processor
 * for the eight hundred milliseconds it needed and the ring drained -
 * which is precisely the stutter this thread caused once already.
 *
 * With it, the cover runs on genuine surplus: full speed while the
 * buffer is above half, and out of the way the moment it is not. The
 * decoder is higher priority than this thread only when boosted, so
 * sleeping here is what actually hands it the time. */
/* How hard the fetcher competes: 0 patient, 1 balanced, 2 eager.
 * Declared here because the yield below reads it and the thread that
 * acts on it is further down. Eager by default, which is what main.c
 * applies at startup too - the two agree so a build that never called
 * the setter still behaves the way the Test page says it does. */
static int s_artPace = 2;

/* When this decode started, and how long it may take. */
static uint64_t s_artDeadline = 0;
static uint32_t s_artBudgetMs = 1000;

void Mp3Decoder_SetArtBudgetMs(uint32_t ms) { s_artBudgetMs = ms; }

/* Standing down is a pause, not a surrender.
 *
 * Two thousand microseconds at a time, and never more than this many in
 * one go. The cap is the whole point: the condition being waited on is
 * the audio buffer, and on a track the decoder cannot keep ahead of, the
 * buffer never does come back - so a wait with no end to it is a cover
 * that never loads at all, which is the one outcome worth ruling out.
 *
 * Pushing on anyway is safe because the priorities already cover the
 * case this was guarding: below an eighth of the ring the decoder boosts
 * itself above the fetcher and takes the processor whether the fetcher
 * yields or not. This stand-down is the softer, earlier measure, and a
 * soft measure should not be able to stop the picture arriving.
 *
 * Twenty-five stand-downs of two milliseconds is fifty per row of
 * blocks, so a cover of forty rows waits at most two seconds even if the
 * buffer is flat on the floor for the whole decode. */
#define ART_STAND_DOWN_US  2000
#define ART_STAND_DOWN_MAX 25

#ifdef ARM9
#define ART_SLEEP(us) threadSleep(us)
#else
/* No threads on the host, so it counts what it would have slept. The
 * loop below is then the same loop in both builds, which is the only way
 * a test can watch it stop waiting. */
static uint32_t s_artSleptUs = 0;
#define ART_SLEEP(us) (s_artSleptUs += (us))
#endif

static bool ArtYield(void)
{
    /* Out of the way while the buffer is low. A quarter, not a half: the
     * decoder boosts above everything at an eighth of the ring, so a
     * quarter leaves twice that much runway - and standing down at a
     * half meant the cover spent most of a freshly started track asleep
     * waiting for a buffer that had not filled yet, which is exactly
     * when it is wanted.
     *
     * Eager skips this entirely. It is the fastest the cover can arrive
     * and it is why the list used to stutter, which is the trade being
     * offered rather than hidden. */
    int waited = 0;
    while (s_artPace < 2 && Audio_QueuedFrames() < (AUDIO_RING_FRAMES / 4)) {
        if (waited >= ART_STAND_DOWN_MAX) break;      /* a row anyway */
        ART_SLEEP(ART_STAND_DOWN_US);
        waited++;
        if (s_artDeadline && tickGetCount() > s_artDeadline) break;
    }

    /* And give up on this attempt if it has gone on too long.
     *
     * There is no way to make a large picture quick. The entropy decode
     * walks every bit the encoder wrote and that is proportional to the
     * file, so a cover twice the size takes twice as long whatever scale
     * it is decoded at - the scale only governs the transform, which is a
     * twentieth of the cost. The choice is therefore to wait or to stop,
     * and a stand-in on screen now beats the real cover in four seconds.
     *
     * This attempt, not the picture: a decode stopped here is tried
     * again with no deadline at all, so the budget buys a quick stand-in
     * rather than a cover that never arrives.
     *
     * Reported as its own outcome, so the Test page says "too slow"
     * rather than leaving it looking like a file with no picture. */
    if (s_artDeadline && tickGetCount() > s_artDeadline) return false;
    return true;
}

static void FetchArt(uint32_t seq, int attempt)
{
    /* Everything the fetch needs, taken once. The next track may
     * overwrite these while this is still running, and the seq check
     * below is what stops the result being published - but the parse
     * must not be steered by a different track's numbers on its way to
     * being thrown away. */
    const long size = s_artSize, at = s_artOffset;
    const bool v22 = s_artV22;
    if (size <= 0 || !s_artPath[0]) return;

    s_artReport.bytes = (uint32_t)size;
    s_artReport.readMs = s_artReport.decodeMs = 0;
    s_artReport.outcome = ART_READ_FAILED;

    unsigned char *body = malloc((size_t)size);
    if (!body) return;

    FILE *f = fopen(s_artPath, "rb");
    if (!f) { free(body); return; }

    const uint64_t readAt = tickGetCount();

    /* Read without the decoder's lock.
     *
     * Taking it was the reason a cover could take twenty seconds to
     * appear. It is not the card's lock - libfat has its own, and it is
     * real: ff_mutex_take goes to __libc_lock_acquire and from there to
     * calico's mutexLock, so the volume is already serialised per
     * operation. Taking the decoder's lock as well meant every eight
     * kilobytes of picture had to win a race against the pump, which
     * holds that lock for long stretches, and this is the lowest
     * priority thread in the program - so it lost, over and over.
     *
     * Serialising against the pump was never buying safety. It was
     * buying a queue, at the back of which sat the only thread that had
     * nothing else to do.
     *
     * Seek once, then read forward. Seeking to an absolute offset before
     * every chunk walks the file's cluster chain from the beginning each
     * time, so the twentieth chunk cost twenty times the walk of the
     * first. */
    const bool placed = (fseek(f, at, SEEK_SET) == 0);

    long got = 0;
    while (placed && got < size && seq == s_artSeq) {
        long want = size - got;
        if (want > ART_CHUNK) want = ART_CHUNK;

        size_t n = fread(body + got, 1, (size_t)want, f);
        if (n == 0) break;
        got += (long)n;
    }

    fclose(f);
    s_artReport.readMs = ElapsedMsSince(readAt);

    if (got == size && seq == s_artSeq) {
        /* The budget is for the first attempt only. Anything that gave
         * up against it is tried again with no clock on it at all,
         * which is what makes the setting a deadline for the stand-in
         * rather than a limit on whether the cover ever appears. */
        s_artDeadline = (s_artBudgetMs && attempt == 0)
                      ? tickGetCount() + ((uint64_t)s_artBudgetMs * TICK_FREQ) / 1000u
                      : 0;

        Jpeg_SetYieldHook(ArtYield);
        ReadArtFrame(body, (size_t)size, v22);
        Jpeg_SetYieldHook(NULL);

        if (s_artW == 0 && s_artReport.outcome == ART_UNDECODABLE &&
            s_artDeadline && tickGetCount() > s_artDeadline)
            s_artReport.outcome = ART_TOO_SLOW;

        s_artDeadline = 0;
    }

    free(body);
}

/* Whether an outcome is worth another go.
 *
 * Two of them are: a read the card refused, and a decode that ran out of
 * budget. Both are about the moment rather than the file, and both leave
 * a track that plainly has a picture showing the stand-in disc instead.
 *
 * The rest are not. A file with no picture frame, a picture that is a
 * PNG, a JPEG that is progressive - none of those become anything else
 * on a second reading, and retrying them would be a background thread
 * reading the same bytes off the card for the length of the track. */
static bool ArtWorthRetry(void)
{
    return s_artReport.outcome == ART_READ_FAILED ||
           s_artReport.outcome == ART_TOO_SLOW;
}

/* Enough to get past a card that was busy, and few enough that a picture
 * which genuinely will not decode is not read three times over. */
#define ART_ATTEMPTS 3

/* The picture, and what it takes to actually get it.
 *
 * This is the guarantee, in one place because it is worth being able to
 * point at: a track whose picture can be decoded ends up with it on
 * screen, whatever pace the fetcher is set to and whatever budget was
 * put on the first attempt. The budget shortens the wait for a stand-in;
 * it does not decide whether the cover ever appears.
 *
 * Shared by both builds rather than living in the thread, so the tests
 * run the same policy the console does.
 */
static void FetchArtWithRetries(uint32_t seq)
{
    for (int attempt = 0; attempt < ART_ATTEMPTS; attempt++) {
        FetchArt(seq, attempt);

        /* The picture arrived, the track moved on underneath this, or
         * there is nothing here that another read would change. */
        if (s_artW || seq != s_artSeq || !ArtWorthRetry()) return;

#ifdef ARM9
        /* A moment before trying again, so a card that is busy is not
         * asked twice in the same breath. */
        threadSleep(200000);
#endif
    }
}

#ifdef ARM9
/* Below the decoder's idle priority, which is itself below the UI.
 *
 * So the order of precedence is: the mixer, then audio decoding when it
 * is short, then drawing, then - with whatever is left over - the
 * cover. Which is the right order: nobody has ever wanted a picture
 * more than they wanted the music to keep playing. */
/* Above the decoder at rest, and it stands down when the buffer says to.
 *
 * This has been moved twice. Below the decoder it was safe and far too
 * slow: at 320 kbps the decoder wants nearly the whole processor, so a
 * cover needing eight hundred milliseconds of it waited the better part
 * of a minute. Above the decoder it was fast and caused a stutter, for
 * exactly the reason it was fast - it held the processor for the whole
 * decode and the ring drained.
 *
 * What was missing both times was a way to give the time back partway
 * through. ArtYield is that: the decode checks the audio buffer once per
 * row of blocks and sleeps while it is below half. So the cover runs at
 * full speed on genuine surplus and steps aside the instant there is
 * none, which is the behaviour that was wanted from the start and that
 * neither priority alone could produce. */
#define ART_PRIO (MAIN_THREAD_PRIO + 2)

/* Three places the fetcher can sit, so the trade can be measured rather
 * than argued about. The wall-clock time a cover takes is its work
 * divided by the share of the processor it gets, and at 320 kbps the
 * decoder wants nearly all of it - so this is the setting that moves the
 * number, far more than anything about the decoding itself.
 *
 *   Patient   below the decoder at rest. Never disturbs anything and can
 *             take most of a minute on a busy track.
 *   Balanced  above the decoder at rest, standing down whenever the
 *             audio buffer drops below a quarter.
 *   Eager     the same priority and no standing down. Fastest by a wide
 *             margin, and the setting that made the list stutter when it
 *             was the only behaviour on offer.
 */
#define ART_PRIO_PATIENT  (MAIN_THREAD_PRIO + 8)
#define ART_PRIO_ACTIVE   (MAIN_THREAD_PRIO + 2)

_Static_assert(ART_PRIO > MAIN_THREAD_PRIO && ART_PRIO < WORKER_PRIO_IDLE,
               "the cover fetcher belongs between the UI and the decoder at "
               "rest - a larger number is a lower priority here, so this "
               "reads as: below the UI, above the decoder when it is idle");

static Thread s_artThread;
static bool   s_artRunning = false;
alignas(8) static uint8_t s_artStack[8 * 1024];

static int ArtWorker(void *arg)
{
    (void)arg;

    for (;;) {
        const uint32_t seq = s_artSeq;

        /* Five milliseconds, not fifty. Nothing here is urgent, but
         * fifty was up to three frames of doing nothing before the fetch
         * even started, added to a wait the user was already calling
         * late. The poll itself is a compare. */
        if (seq == s_artDone) { threadSleep(5000); continue; }

        FetchArtWithRetries(seq);
        s_artDone = seq;
    }
    return 0;
}
#endif

/* Out here rather than beside the thread it steers: the pace is what
 * the stand-down above reads, and a host build that could not set it
 * could not test the stand-down. */
void Mp3Decoder_SetArtPace(int pace)
{
    if (pace < 0) pace = 0;
    if (pace > 2) pace = 2;
    s_artPace = pace;

#ifdef ARM9
    if (s_artRunning)
        threadSetPrio(&s_artThread,
                      pace == 0 ? ART_PRIO_PATIENT : ART_PRIO_ACTIVE);
#endif
}

/* Asks for this track's picture. Returns at once; the cover turns up
 * when it turns up. */
static void RequestArt(void)
{
    if (s_artSize <= 0) return;
    s_artSeq++;

#ifndef ARM9
    /* No threads on the host, so the tests get it synchronously - the
     * only difference between the two builds, and it is here rather than
     * inside the fetch so that the fetch is the same code either way.
     * Retries included: they are the guarantee, not a detail of the
     * thread. */
    FetchArtWithRetries(s_artSeq);
    s_artDone = s_artSeq;
#endif
}

const uint16_t *Mp3Decoder_Art(int *w, int *h)
{
    if (w) *w = s_artW;
    if (h) *h = s_artH;
    return s_artW ? s_art : NULL;
}

uint32_t Mp3Decoder_ArtGeneration(void) { return s_artGen; }

/* Whether anything on screen is going to look at the picture.
 *
 * Reading it is not free and neither is decoding it, and both happen on
 * the thread that opens the track - which is the UI thread, holding the
 * decoder lock. With a meter on the top screen that work bought
 * absolutely nothing and still cost a pause at the start of every track.
 *
 * Off by default, so a build that forgets to ask never pays. */
static bool s_wantArt = false;

void Mp3Decoder_SetWantArt(bool want) { s_wantArt = want; }

static void ClearArt(void)
{
    s_artW = s_artH = 0;
    s_artGen++;
}

/* Finds the picture inside an APIC frame body and decodes it.
 *
 * The body is a header of variable length - an encoding byte, a MIME
 * string, a picture type, and a description in whichever encoding the
 * first byte named - followed by the file itself. Parsing that exactly
 * means knowing how a UTF-16 description terminates, which taggers do
 * not always agree on, and a description parsed a byte short leaves the
 * decoder pointed into the middle of a JPEG header.
 *
 * So it is parsed, and then checked: if the computed offset is not
 * sitting on a JPEG's start marker, the first few hundred bytes are
 * scanned for one instead. Either way the decoder is handed something
 * that begins where a JPEG begins. */
static void ReadArtFrame(const unsigned char *body, size_t n, bool v22)
{
    if (n < 16) return;

    size_t at = 1;                       /* past the encoding byte */
    const unsigned char enc = body[0];

    if (v22) {
        at += 3;                         /* a three-character format tag */
    } else {
        while (at < n && body[at]) at++; /* the MIME string */
        at++;
    }
    if (at >= n) return;
    at++;                                /* the picture type */

    /* The description. Two-byte terminated for the UTF-16 encodings. */
    if (enc == 1 || enc == 2) {
        while (at + 1 < n && (body[at] || body[at + 1])) at += 2;
        at += 2;
    } else {
        while (at < n && body[at]) at++;
        at++;
    }

    const size_t limit = (n < 512) ? n : 512;
    if (at + 3 >= n ||
        !(body[at] == 0xFF && body[at + 1] == 0xD8 && body[at + 2] == 0xFF)) {
        size_t scan = 0;
        for (; scan + 3 < limit; scan++)
            if (body[scan] == 0xFF && body[scan + 1] == 0xD8 &&
                body[scan + 2] == 0xFF) break;
        if (scan + 3 >= limit) {
            /* No JPEG start marker anywhere near the front of the frame.
             * A PNG cover is the usual reason, and there are plenty of
             * them: several taggers write PNG by default. Recorded
             * rather than passed over in silence, because from the
             * outside it is indistinguishable from a cover that is
             * merely taking its time. */
            s_artReport.outcome = ART_NOT_JPEG;
            return;
        }
        at = scan;
    }

    const uint64_t decodeAt = tickGetCount();

    int w = 0, h = 0;
    if (Jpeg_Thumbnail(body + at, n - at, s_art, ART_BOX, ART_BOX, &w, &h)) {
        s_artW = w;
        s_artH = h;
        s_artGen++;
        s_artReport.outcome = ART_OK;
    } else {
        s_artReport.outcome = ART_UNDECODABLE;
    }
    s_artReport.decodeMs = ElapsedMsSince(decodeAt);
}

/* Where the embedded picture lives in the file, not the picture itself.
 * Art runs to tens of kilobytes and only one track's is ever wanted, so
 * it is located during the tag walk and read on demand. */
static bool s_inputEof   = false;
static size_t s_prevRemaining = (size_t)-1;
static bool s_decodeDone = false;
static bool s_sawFirstFrame = false;

/* Whether libmad has run out of bytes and needs another RefillInput().
 * This is tracked here rather than read back out of s_stream.error
 * because libmad only ever *sets* that field - it is never cleared on a
 * successful decode, so a single MAD_ERROR_BUFLEN would otherwise look
 * like a permanent request to refill on every frame from then on. */
static bool s_needRefill = true;

/* stream properties, filled in from the first decoded frame */
static uint32_t s_sampleRate = 0;
static uint32_t s_bitrate    = 0;   /* bits per second */
static int      s_channels   = 2;
static unsigned s_samplesPerFrame = 1152;

/* Xing/Info/VBRI header contents, when the file has one */
static bool     s_hasVbrHeader = false;
static bool     s_isVbr        = false;
static uint32_t s_vbrFrames    = 0;
static uint32_t s_vbrBytes     = 0;
static bool     s_hasToc       = false;
static uint8_t  s_toc[100];

/* playback clock: position of the start of the queued audio, plus what
 * the mixer has consumed since */
static uint32_t s_baseMs = 0;

/* Track length, worked out once. The calculation is a 64-bit divide by a
 * value that is not a power of two, and the UI asks for it several times
 * every frame - it is constant for the whole track, so it is computed
 * when the first frame reveals the rate and cached from then on. */
static uint32_t s_totalMs = 0;

static void ComputeTotalMs(void);

/* ---- resampler (polyphase windowed-sinc FIR, input rate -> AUDIO_OUT_RATE) --
 *
 * This used to be linear interpolation, which is cheap but is a very poor
 * anti-aliasing filter. Going from 44100 to 32768 is a *downsample*:
 * everything in the source above the output's 16384Hz Nyquist has to be
 * removed first, or it folds back down into the audible band as
 * inharmonic tones. Linear interpolation barely attenuates that content
 * at all - a cymbal at 20kHz lands back at 12.7kHz as a whistle - which
 * is exactly the sort of "small noise" that sits on top of otherwise
 * clean playback.
 *
 * A short windowed-sinc lowpass, evaluated as a polyphase bank so no
 * work is wasted on samples that get discarded, fixes that for a few
 * percent of CPU. See tools/hosttest for the measured alias rejection. */
/* Tap count and cutoff margin are overridable so tools/hosttest can sweep
 * them; the defaults here are the measured sweet spot. More taps means a
 * steeper filter and better alias rejection, at a linear cost in CPU. */
/* Longest filter the tables are sized for. The tap count actually used
 * is a run-time value (s_taps) so the quality setting can be switched on
 * the console and compared by ear, which is the only way to judge it on
 * real hardware and real headphones. */
#ifndef RES_TAPS
#define RES_TAPS   24
#endif
#define RES_TAPS_MAX RES_TAPS
#define RES_TAPS_LOW 12
#define RES_TAPS_FAST 8
#define RES_TAPS_LITE 4
/* Not a tap count: the marker for the linear path, which uses no
 * coefficient bank at all. Kept in s_taps so one variable still selects
 * the whole resampler. */
#define RES_TAPS_NONE 1
#define RES_TAPS_RAW  2
_Static_assert(RES_TAPS_MAX % 4 == 0 && RES_TAPS_LOW % 4 == 0 &&
               RES_TAPS_FAST % 4 == 0 && RES_TAPS_LITE % 4 == 0,
               "the resampler inner loop is unrolled four taps at a time");
#ifndef RES_ROLLOFF
#define RES_ROLLOFF 0.90
#endif
/* Phase count, as a power of two so the index is a mask.
 *
 * The other dimension of the coefficient table, and the one that was
 * never a knob. At 8 taps the bank is RES_PHASES * taps * 2 bytes, which
 * at 256 phases is exactly 4096 - the whole of the ARM9's data cache,
 * leaving none of it for the decoder that has to share it. Halving the
 * phases halves the table for the same filter and the same alias
 * rejection; what it costs is resolution in where each output sample is
 * interpolated from, which shows up as noise rather than as aliasing.
 *
 * Overridable so tools/hosttest can sweep it, like RES_TAPS above. */
#ifndef RES_PHASE_BITS
#define RES_PHASE_BITS 8
#endif
#define RES_PHASES (1 << RES_PHASE_BITS)

/* Phases actually in use, chosen per mode.
 *
 * Aliasing is set by the filter; phase count only sets how finely the
 * interpolation point is resolved, so fewer phases cost noise and not one
 * decibel of alias rejection. Measured at 8 taps, 44100 -> 32768: the
 * 20kHz fold stays at -34.2dB from 256 phases down to 32, while THD+N at
 * 10kHz goes -54.9, -43.8, -37.8 dB.
 *
 * That makes it the right knob for the modes that exist to be cheap. Fast
 * at 256 phases is a 4096-byte bank - the whole data cache, leaving none
 * of it for the decoder it shares with; at 64 it is 1024, and still far
 * cleaner than dropping to no filter at all, which measures -20.4dB THD+N
 * at 10kHz. The quality modes keep every phase. */
static unsigned s_phases    = RES_PHASES;
static unsigned s_phaseBits = RES_PHASE_BITS;

/* The fraction in s_resPos is Q16, so this is what shifts its top
 * RES_PHASE_BITS down to the phase index. It was hardcoded to 8, which
 * silently assumed 256 phases and would have quietly read the wrong row
 * had anyone changed the count. */
#define RES_PHASE_SHIFT (16 - RES_PHASE_BITS)

/* Output headroom, as a linear gain applied to every coefficient.
 *
 * A windowed-sinc lowpass has unity gain at DC but overshoots on
 * transients - the same Gibbs ringing that makes the step response
 * wobble. Measured on full-scale input (tools/hosttest/overshoot.c): a
 * sine does not overshoot at all, a square wave reaches 1.19x full
 * scale, and noise-like material - which is what a loud modern master
 * looks like - reaches 1.44x. All of that used to run straight into the
 * int16 clamp, so anything mastered near 0dBFS was hard-clipping
 * continuously. That is heard as a persistent grainy edge on loud
 * passages, not as an occasional tick.
 *
 * 16-bit fixed point cannot represent above full scale, so the choice is
 * to clip or to make room. This makes room: -2.2dB, enough to absorb
 * every transient short of the pathological noise case. The cost is 2.2dB
 * of maximum loudness, which the DS's own volume slider gives back. */
#ifndef RES_HEADROOM
#define RES_HEADROOM 0.78
#endif
#define RES_COEF_BITS 14
#define RES_COEF_ONE  (1 << RES_COEF_BITS)

/* The same headroom the FIR bakes into its coefficients, as a multiplier
 * the linear path can apply directly.
 *
 * Linear interpolation cannot overshoot - every output is a weighted
 * average of two inputs, so it is bounded by them - and needs no headroom
 * of its own. It is applied anyway so that switching modes changes the
 * sound and not the volume: without it Raw would come out 2.2dB louder
 * than every other setting, which is heard as "better" whatever it does
 * to the treble, and makes the comparison useless. */
#define RES_LINEAR_GAIN ((int32_t)(RES_HEADROOM * RES_COEF_ONE))

/* Flat and packed to the tap count actually in use, not to the longest
 * one the array could hold.
 *
 * It was [RES_PHASES][RES_TAPS_MAX], so every row was 48 bytes wide no
 * matter how many of them a mode read. A four-tap filter's 2KB of
 * coefficients sat spread across 12KB, touched one row at a time at a
 * phase index that jumps pseudo-randomly through all 256 of them - and
 * the ARM9's data cache is 4KB. Fewer taps bought less arithmetic and not
 * one byte less cache pressure, which is most of why the quality setting
 * has mattered less on hardware than the tap counts suggest, and why a
 * host with megabytes of cache cannot see the difference at all.
 *
 * Packed, the table is 256 * taps * 2 bytes: 12KB at Best, 4KB at Fast,
 * 2KB at Lite - which fits the cache whole - and nothing at Raw. The row
 * address is phase * TAPS, and TAPS is a compile-time constant in each
 * instantiation of the inner loop, so the multiply folds to a shift. */
/* The bank lives in DTCM whenever it fits there.
 *
 * DTCM is 16KB of tightly-coupled RAM wired straight to the ARM9: single
 * cycle, no wait states, and - the point - it is not the cache, so a
 * table held there costs the decoder nothing at all. Calico's linker
 * script leaves 0x3e80 of it for data, of which the build was using 152
 * bytes.
 *
 * 2KB is reserved, which at 64 phases holds both cheap modes - Lite at
 * 512 bytes, Fast at 1024. The quality modes ask for 6KB and 12KB, do not
 * fit, and stay in main RAM where they were: those are the settings
 * chosen for quality over headroom.
 *
 * The test is the bank's size and not the mode, so any combination of
 * taps and phases that fits gets DTCM.
 *
 * Reintroduced alone. This and putting the inner loop in ITCM went out as
 * one commit, that build did not boot, and one commit changing two
 * separate things about where code and data live at startup cannot say
 * which of them did it. */
#define FIR_DTCM_BYTES 2048

#ifdef ARM9
#define FIR_DTCM DTCM_BSS
#else
#define FIR_DTCM                    /* the host has no TCM; plain RAM */
#endif

static int16_t s_firDtcm[FIR_DTCM_BYTES / sizeof(int16_t)]
    FIR_DTCM __attribute__((aligned(4)));
static int16_t s_firMain[RES_PHASES * RES_TAPS_MAX] __attribute__((aligned(4)));

/* Points at whichever of the two the current filter was built into. */
static int16_t *s_fir = s_firMain;
static uint32_t s_firRate = 0;      /* input rate the table was built for, 0 = none */
static uint32_t s_firOutRate = 0;   /* and the output rate, which sets the cutoff */
static unsigned s_taps = RES_TAPS_MAX;

/* What the chosen mode asked for, which is not always what runs.
 *
 * The filter has to answer to the ratio as well as to the setting: a
 * window that is merely cheap when the stream is being slowed down is
 * broken when it is being sped up. Kept apart from s_taps so that
 * choosing a mode compares against the mode, not against whatever the
 * current ratio made of the last one. */
static unsigned s_tapsWanted = RES_TAPS_MAX;
static float    s_rolloffWanted = 0.90f;

/* On. See ApplyUpsampleGuard - off is there to hear what it prevents. */
static bool s_upsampleGuard = true;

/* The selected mode, remembered rather than reconstructed.
 *
 * It used to be derived from (s_taps, s_rolloff) on the way out, which
 * only works while every mode has a distinct filter - and Raw and Lite do
 * not differ from the others in any way that projection could see. Both
 * came back as Clean, so choosing either applied it correctly and then
 * immediately reported it as something else: the segment snapped back and
 * the setting looked like it could not be changed at all.
 *
 * Initialised to match s_taps above, so the two never disagree even
 * before a mode has been set. */
static int s_resampleMode = RESAMPLE_BEST;

/* How close to the output Nyquist the passband reaches.
 *
 * The second knob on the filter, and the cheaper one. Taps buy a steeper
 * transition; rolloff buys a wider one to be steep across. A short filter
 * cannot be made steep, so pulling the passband in is the only way to get
 * real stopband rejection out of it - and it costs nothing at runtime,
 * because it only changes the coefficients baked once at startup.
 *
 * Measured at 12 taps, 44100 -> 32768:
 *     0.90   passband to 14.7kHz   20kHz folds back at -25 dB
 *     0.70   passband to 11.5kHz   20kHz folds back at -52 dB
 * 27dB of artifact for treble the DS speakers do not reproduce anyway -
 * though on headphones it is a real trade, which is why it is a switch
 * and not a decision made here. */
static float s_rolloff = (float)RES_ROLLOFF;
static unsigned s_firTaps = 0;      /* tap count the table was built for */
static unsigned s_firPhases = 0;    /* phase count the table was built for */

static uint32_t s_resStep = 0;      /* Q16 input samples per output sample */
static uint32_t s_resPos = 0;       /* Q16 position of next output within the work buffer */

/* Work buffer: leftover tail from the previous frame (always fewer than
 * RES_TAPS samples) followed by the frame just decoded. */
#define RES_WORK_MAX (RES_TAPS_MAX + MAD_MAX_SAMPLES + 8)
static int16_t  s_workL[RES_WORK_MAX];
static int16_t  s_workR[RES_WORK_MAX];
static unsigned s_workCount = 0;

static unsigned s_maxOutPerFrame = MAX_OUT_FRAMES;

static int16_t s_outBuf[MAX_OUT_FRAMES * 2];

/* ---- noise reduction (downward expander) ---------------------------
 *
 * Deliberately an expander rather than a gate. A gate has one threshold
 * and slams shut, which "chatters" on anything hovering near it and
 * chops off reverb tails; an expander leans on the level progressively,
 * so quiet music just gets quieter instead of disappearing.
 *
 * Above the threshold nothing happens at all - this must be inaudible on
 * normal material or it is worse than the noise it removes. Below it the
 * gain falls roughly 2dB per 1dB of level, bottoming out at a floor
 * rather than full silence, because a passage that mutes completely
 * sounds more broken than one that is merely quiet.
 *
 * Thresholds are picked from where hiss actually lives: music peaks near
 * 0dBFS, while tape and encoder noise sit around -60..-45dBFS. */



/* most recent decoded frame, kept at the file's own rate for the visualizer */
static int16_t s_lastFrame[MAD_MAX_SAMPLES * 2];
static int     s_lastFrameCount = 0;

/* ------------------------------------------------------------------ */

static inline int16_t ScaleToS16(mad_fixed_t sample)
{
    /* libmad's own round/clip/quantize sequence, straight out of its
     * example decoder. */
    sample += (1L << (MAD_F_FRACBITS - 16));

    if (sample >= MAD_F_ONE)
        sample = MAD_F_ONE - 1;
    else if (sample < -MAD_F_ONE)
        sample = -MAD_F_ONE;

    return (int16_t)(sample >> (MAD_F_FRACBITS + 1 - 16));
}

static uint32_t ReadBE32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void ReadId3v2(FILE *f, long tagEnd, unsigned char ver);
static void ReadId3v1(FILE *f);

/* ID3v2 tags sit in front of the audio and can be a hundred kilobytes
 * once album art is involved. libmad would resync past one on its own,
 * but skipping it explicitly keeps that cost off the first Pump() and,
 * more importantly, keeps the tag out of the byte<->time arithmetic that
 * duration and seeking are built on. */
static long SkipId3v2(FILE *f)
{
    unsigned char h[10];

    if (fread(h, 1, sizeof(h), f) != sizeof(h)) {
        fseek(f, 0, SEEK_SET);
        return 0;
    }

    bool looksLikeId3 = memcmp(h, "ID3", 3) == 0 && h[3] != 0xFF && h[4] != 0xFF &&
                        !((h[6] | h[7] | h[8] | h[9]) & 0x80); /* syncsafe size */
    if (!looksLikeId3) {
        fseek(f, 0, SEEK_SET);
        return 0;
    }

    long size = ((long)h[6] << 21) | ((long)h[7] << 14) |
                ((long)h[8] << 7)  | (long)h[9];
    long total = 10 + size;
    if (h[5] & 0x10) total += 10;   /* footer present */

    if (total > s_fileSize) total = s_fileSize;

    ReadId3v2(f, total, h[3]);

    fseek(f, total, SEEK_SET);
    return total;
}

/* ---- ID3 text tags -------------------------------------------------- */

/* The transliteration lives in textfold.c now - the file browser needs
 * the same table for filenames, and two copies of it would drift. */
#include "textfold.h"

/* Decodes one ID3v2 text frame body into `out`. `enc` is the frame's
 * leading encoding byte. */
static void DecodeTextFrame(const unsigned char *p, size_t n,
                            char *out, size_t cap)
{
    if (!n) { out[0] = '\0'; return; }

    unsigned enc = p[0];
    p++; n--;
    size_t len = 0;
    out[0] = '\0';

    if (enc == 1 || enc == 2) {                 /* UTF-16, with or without BOM */
        bool big = (enc == 2);
        if (enc == 1 && n >= 2) {
            if (p[0] == 0xFF && p[1] == 0xFE)      { big = false; p += 2; n -= 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { big = true;  p += 2; n -= 2; }
        }
        for (size_t i = 0; i + 1 < n; i += 2) {
            unsigned cp = big ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
            if (!cp) break;
            TextFold_Append(out, cap, &len, cp);
        }
    } else if (enc == 3) {                      /* UTF-8 */
        for (size_t i = 0; i < n && p[i]; ) {
            unsigned cp = p[i];
            int extra = (cp >= 0xF0) ? 3 : (cp >= 0xE0) ? 2 : (cp >= 0xC0) ? 1 : 0;
            if (extra) {
                cp &= (0x3F >> extra);
                for (int k = 0; k < extra && i + 1 + k < n; k++)
                    cp = (cp << 6) | (p[i + 1 + k] & 0x3F);
            }
            i += 1 + extra;
            TextFold_Append(out, cap, &len, cp);
        }
    } else {                                    /* ISO-8859-1 */
        for (size_t i = 0; i < n && p[i]; i++)
            TextFold_Append(out, cap, &len, p[i]);
    }

    while (len && out[len - 1] == ' ') out[--len] = '\0';
}

/* Walks the frames of an ID3v2 tag, reading only the three that are worth
 * showing and seeking over everything else.
 *
 * Seeking rather than reading matters: embedded album art is routinely
 * larger than this program's entire heap, and it usually sits before the
 * text frames rather than after. Reading the whole tag to find a title
 * would be both slow and impossible. */
static void ReadId3v2(FILE *f, long tagEnd, unsigned char ver)
{
    const int idLen = (ver == 2) ? 3 : 4;
    const int hdrLen = (ver == 2) ? 6 : 10;

    fseek(f, 10, SEEK_SET);

    for (int guard = 0; guard < 128; guard++) {
        unsigned char h[10];
        long here = ftell(f);
        if (here + hdrLen > tagEnd) break;
        if (fread(h, 1, (size_t)hdrLen, f) != (size_t)hdrLen) break;
        if (h[0] == 0) break;                    /* padding */

        long size;
        if (ver == 2) {
            size = ((long)h[3] << 16) | ((long)h[4] << 8) | h[5];
        } else if (ver == 4) {
            /* 2.4 sizes are syncsafe. Some taggers write 2.3-style sizes
             * into a 2.4 tag anyway; if the syncsafe reading does not land
             * on a plausible next frame, fall back to the flat one. */
            size = ((long)h[4] << 21) | ((long)h[5] << 14) |
                   ((long)h[6] << 7)  | h[7];
            if ((h[4] | h[5] | h[6] | h[7]) & 0x80)
                size = ((long)h[4] << 24) | ((long)h[5] << 16) |
                       ((long)h[6] << 8)  | h[7];
        } else {
            size = ((long)h[4] << 24) | ((long)h[5] << 16) |
                   ((long)h[6] << 8)  | h[7];
        }

        if (size <= 0 || here + hdrLen + size > tagEnd) break;

        /* The attached picture, which is the cover.
         *
         * Read only when nothing has been decoded yet, so a file with
         * several - a front cover, a back, an artist photo - costs one
         * decode rather than one per picture, and the first is the one
         * kept. Everything past the cap is stepped over instead.
         *
         * Either way the frame has to be walked past exactly, or every
         * tag after it - which is usually the title - is never reached.
         * That is why the seek happens whatever the outcome. */
        /* The attached picture, which is the cover.
         *
         * Noted, not read. Where it is costs two longs to remember and
         * nothing to find, since the frame is being walked anyway;
         * reading it costs a few hundred kilobytes off the card, and
         * this walk happens on the thread that opens the track, holding
         * the decoder lock. Fetching it here was a pause before every
         * track started - for a picture that is wanted after the music
         * is playing, if at all.
         *
         * Recorded whether or not anything wants it, so that turning the
         * cover card on mid-track has somewhere to look without walking
         * the tag a second time.
         *
         * The first one only: a file with a front cover, a back and an
         * artist photo should cost one fetch, and the front is the one
         * that comes first. Either way the frame is stepped over
         * exactly, or every tag after it - usually the title - is never
         * reached. */
        if (!memcmp(h, idLen == 3 ? "PIC" : "APIC", (size_t)idLen)) {
            if (s_artSize == 0 && size > 0) {
                if ((unsigned long)size <= ART_MAX_BYTES) {
                    s_artOffset = here + hdrLen;
                    s_artSize   = size;
                    s_artV22    = (idLen == 3);
                } else {
                    s_artReport.outcome = ART_TOO_BIG;
                    s_artReport.bytes   = (uint32_t)size;
                }
            }
            fseek(f, here + hdrLen + size, SEEK_SET);
            continue;
        }

        char *dst = NULL;
        size_t cap = 0;
        if (!memcmp(h, idLen == 3 ? "TT2" : "TIT2", (size_t)idLen)) {
            dst = s_title;  cap = sizeof(s_title);
        } else if (!memcmp(h, idLen == 3 ? "TP1" : "TPE1", (size_t)idLen)) {
            dst = s_artist; cap = sizeof(s_artist);
        }

        if (dst && size < 1024) {
            unsigned char buf[1024];
            if (fread(buf, 1, (size_t)size, f) == (size_t)size)
                DecodeTextFrame(buf, (size_t)size, dst, cap);
            else break;
        } else {
            fseek(f, size, SEEK_CUR);
        }
    }
}

/* ID3v1 lives in the last 128 bytes and is the only tag a surprising
 * number of files have. Used to fill whatever v2 did not provide, rather
 * than to override it - v1 is fixed-width, so its fields are truncated at
 * 30 characters and are the poorer source when both exist. */
static void ReadId3v1(FILE *f)
{
    if (s_fileSize < 128) return;
    if (s_title[0] && s_artist[0]) return;

    unsigned char t[128];
    long save = ftell(f);
    fseek(f, s_fileSize - 128, SEEK_SET);
    bool ok = fread(t, 1, sizeof(t), f) == sizeof(t) && !memcmp(t, "TAG", 3);
    fseek(f, save, SEEK_SET);
    if (!ok) return;

    struct { char *dst; size_t cap; int off; } kF[2] = {
        { s_title,  sizeof(s_title),  3  },
        { s_artist, sizeof(s_artist), 33 },
    };
    for (int i = 0; i < 2; i++) {
        if (kF[i].dst[0]) continue;
        size_t len = 0;
        kF[i].dst[0] = '\0';
        for (int k = 0; k < 30; k++) {
            unsigned char ch = t[kF[i].off + k];
            if (!ch) break;
            TextFold_Append(kF[i].dst, kF[i].cap, &len, ch);
        }
        while (len && kF[i].dst[len - 1] == ' ') kF[i].dst[--len] = '\0';
    }
}

/* Asks for the picture of a track that is already playing.
 *
 * Choosing the cover card while something is playing has to show that
 * track's cover, not the next one's. Where the picture is was recorded
 * when the tag was walked, whether or not anything wanted it at the
 * time, so this is only the request - no card is read on this thread and
 * nothing waits. */
bool Mp3Decoder_LoadArtNow(void)
{
    if (!s_opened) return false;
    if (s_artW) return true;                 /* already have it */

    RequestArt();
    return s_artSize > 0;
}

/* Looks for a Xing/Info (LAME et al) or VBRI (Fraunhofer) header inside
 * the first decoded frame. Without one, a VBR file's duration can only
 * be guessed from the first frame's bitrate - which is how "3:57" turns
 * into "6:12" on a file whose quiet intro encoded cheaply. */
static void ParseVbrHeader(const unsigned char *frame, size_t len,
                           const struct mad_header *h)
{
    if (len < 48) return;

    bool mpeg1 = (h->samplerate >= 32000);
    bool mono  = (h->mode == MAD_MODE_SINGLE_CHANNEL);
    size_t sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    size_t off = 4 + sideInfo;

    /* VBRI always sits at a fixed offset instead of after the side info. */
    if (len >= 36 + 26 && memcmp(frame + 36, "VBRI", 4) == 0) {
        const unsigned char *p = frame + 36;
        s_vbrBytes  = ReadBE32(p + 10);
        s_vbrFrames = ReadBE32(p + 14);
        s_hasVbrHeader = s_vbrFrames > 0;
        s_isVbr = true;
        return;
    }

    if (off + 8 > len) return;

    const unsigned char *p = frame + off;
    bool isXing = memcmp(p, "Xing", 4) == 0;
    bool isInfo = memcmp(p, "Info", 4) == 0;
    if (!isXing && !isInfo) return;

    /* "Xing" means the encoder was in VBR mode, "Info" means CBR/ABR with
     * the same header layout. */
    s_isVbr = isXing;
    p += 4;

    uint32_t flags = ReadBE32(p);
    p += 4;
    const unsigned char *end = frame + len;

    if (flags & 0x1) {
        if (p + 4 > end) return;
        s_vbrFrames = ReadBE32(p);
        p += 4;
    }
    if (flags & 0x2) {
        if (p + 4 > end) return;
        s_vbrBytes = ReadBE32(p);
        p += 4;
    }
    if (flags & 0x4) {
        if (p + 100 > end) return;
        memcpy(s_toc, p, 100);
        s_hasToc = true;
        p += 100;
    }

    s_hasVbrHeader = s_vbrFrames > 0;
}

/* Fold both channels into one before the filter runs.
 *
 * Not a taste setting - a lever for finding out where the time goes. The
 * resampler is about a fifth of the cost of playing anything and it does
 * both channels in the same loop, so this halves the most expensive loop
 * the decoder has. If the stutter goes away with it on, the resampler is
 * where the time is. */
static bool s_mono = false;

/* Where the decoding time actually goes, in three parts.
 *
 * The first version of this started its clock after mad_synth_frame and
 * so measured the resampler alone - and reported fifteen percent, which
 * was then quoted as the whole decoder's load. It was not; it was the
 * cheapest third of it. Splitting the measurement is the point: "the
 * decoder is expensive" is not something anyone can act on, and "the
 * synthesis filter is two thirds of it" is.
 *
 * Huffman and requantisation, then the polyphase synthesis filter, then
 * the resampler - the three stages a frame passes through, each timed
 * where it happens, all averaged over half a second of output. */
static uint64_t s_madTicks = 0, s_synthTicks = 0, s_resTicks = 0;
static uint32_t s_loadFrames = 0;
static uint32_t s_madPct = 0, s_synthPct = 0, s_resPct = 0;

uint32_t Mp3Decoder_LoadPercent(void) { return s_madPct + s_synthPct + s_resPct; }

void Mp3Decoder_LoadSplit(uint32_t *mad, uint32_t *synth, uint32_t *resample)
{
    if (mad)      *mad      = s_madPct;
    if (synth)    *synth    = s_synthPct;
    if (resample) *resample = s_resPct;
}

void Mp3Decoder_SetMono(bool mono) { s_mono = mono; }

/* The synthesis filter's window table, moved into tightly-coupled RAM.
 *
 * libmad reads D through a pointer now (see tools/build_libmad.sh), so
 * pointing it at a copy in ITCM is the whole of this: the same 2176
 * bytes, the same values, at an address the processor reaches in one
 * cycle without going through a cache it cannot fit in.
 *
 * Why it should matter here and nowhere else: one channel of filter
 * state is 2KB, there are two, and the ARM946 has 4KB of data cache. The
 * state alone fills it. D is another 2.2KB read straight through the
 * innermost loop, so for the length of every frame the two evict each
 * other - and the filter is the most expensive of the three stages. This
 * does not make the arithmetic cheaper; it stops the data getting in its
 * own way.
 *
 * Bit-exact by construction, which is why it is safe to leave on. It is
 * a switch anyway because moving things into TCM on this console has
 * produced a black screen before, and a switch is cheaper to reason
 * about at two in the morning than a rebuild. */
/* libmad reads its window table through this; see tools/build_libmad.sh.
 * The table itself has local linkage inside the library and cannot be
 * named here, which is why the original address is captured from the
 * pointer rather than taken from the array. */
extern mad_fixed_t const (*dsxp_Dbase)[32];

#ifdef ARM9
/* The buffer lives in madwindow.c; see the note there for why it cannot
 * live in this file. */
extern mad_fixed_t dsxp_windowItcm[17][32];
void Mp3Window_Load(const void *src);

static bool s_fastWindow = false;
static mad_fixed_t const (*s_windowMain)[32] = NULL;

void Mp3Decoder_SetFastWindow(bool on)
{
    DEC_LOCK();
    /* Whatever it pointed at before anything moved it. */
    if (!s_windowMain) s_windowMain = dsxp_Dbase;

    if (on) {
        Mp3Window_Load(s_windowMain);
        dsxp_Dbase = (mad_fixed_t const (*)[32])dsxp_windowItcm;
    } else {
        dsxp_Dbase = s_windowMain;
    }
    s_fastWindow = on;
    DEC_UNLOCK();
}

bool Mp3Decoder_FastWindow(void) { return s_fastWindow; }
#else
/* No tightly-coupled memory on the host, so there is nowhere to put it
 * and nothing to measure. The setting still exists so the tests and the
 * page can talk about it. */
static bool s_fastWindow = false;
void Mp3Decoder_SetFastWindow(bool on) { s_fastWindow = on; }
bool Mp3Decoder_FastWindow(void) { return s_fastWindow; }
#endif

/* Folds a stereo frame to one channel before the synthesis filter runs.
 *
 * The filter is linear, and that is the whole of why this works: the
 * synthesis of (L + R) is the sum of the syntheses, so adding the two
 * channels in the subband domain - thirty-six rows of thirty-two
 * coefficients, a thousand adds - produces exactly the mono the old path
 * produced by synthesising both channels in full and averaging the
 * output. The difference is what it costs.
 *
 * And the filter is the expensive stage. libmad's own notes put it above
 * the bit decoding, and this build already moved it into ITCM for that
 * reason. Halving it is worth more than anything else on this page.
 *
 * So this is the answer to "full rate without paying for it": the top of
 * the band is kept - every subband is still decoded and still
 * synthesised - and what is given up is the stereo image rather than the
 * treble. Half rate gives up the opposite. Which trade is better is a
 * matter of taste and headphones, which is why both are offered.
 *
 * The header is rewritten as well as the samples, because the number of
 * channels the synthesiser runs is read from it. */
static void FoldFrameToMono(struct mad_frame *frame)
{
    if (MAD_NCHANNELS(&frame->header) < 2) return;

    for (unsigned gr = 0; gr < 36; gr++)
        for (unsigned sb = 0; sb < 32; sb++)
            frame->sbsample[0][gr][sb] =
                (frame->sbsample[0][gr][sb] >> 1) +
                (frame->sbsample[1][gr][sb] >> 1);

    frame->header.mode = MAD_MODE_SINGLE_CHANNEL;
}

/* Whether anything is going to read the last frame's samples.
 *
 * Only the meter does. With a card or nothing on the top screen the copy
 * that feeds it is work done for no one - and because it happens inside
 * the resampler's clock it was being reported as resampler cost, which
 * is the kind of thing that sends somebody optimising the wrong loop. */
static bool s_wantSamples = true;

void Mp3Decoder_SetWantSamples(bool want) { s_wantSamples = want; }

/* The two options libmad actually implements.
 *
 * Half rate is the large one: it takes the synthesis filter's short
 * path, which produces half as many samples at half the rate, so the
 * most expensive stage of decoding halves and the resampler is handed
 * half as much to chew on. What it costs is everything above about
 * eleven kilohertz - a real loss on cymbals, and the reason it is a
 * switch rather than the default.
 *
 * Ignoring CRC saves a little and gives up error detection on a stream
 * that is nearly always intact. The other options in mad.h - left
 * channel, right channel, single channel - sit inside an "#if 0 not yet
 * implemented" and do nothing whatever.
 *
 * Held here and pushed into the stream, because mad_frame_decode copies
 * the stream's options onto every frame as it goes - so a change lands
 * on the next frame without reopening anything. */
static bool s_halfRate = false, s_ignoreCrc = false;

static int MadOptions(void)
{
    return (s_halfRate  ? MAD_OPTION_HALFSAMPLERATE : 0) |
           (s_ignoreCrc ? MAD_OPTION_IGNORECRC : 0);
}

static void ApplyMadOptions(void)
{
    DEC_LOCK();
    if (s_opened) mad_stream_options(&s_stream, MadOptions());
    DEC_UNLOCK();
}

/* Relief's own half-rate changes, which leave it owning the setting. */
static void ReliefSetHalfRate(bool on) { s_halfRate = on; ApplyMadOptions(); }

void Mp3Decoder_SetHalfRate(bool on)
{
    /* An explicit setting takes the decision back.
     *
     * Relief and the rate-cut row drive the same flag from two different
     * places, and relief used to release by clearing it outright - so a
     * rate cut chosen by hand while relief happened to be engaged was
     * undone a few seconds later, with the row still reading what the
     * user had picked. Standing relief down here means the last word
     * belongs to whoever asked most recently, and that the row and the
     * decoder cannot disagree. */
    s_reliefOn = false;
    s_reliefEasySince = 0;

    s_halfRate = on;
    ApplyMadOptions();
}
void Mp3Decoder_SetIgnoreCrc(bool on) { s_ignoreCrc = on; ApplyMadOptions(); }

static void ResetShaping(void);

static void ResetResampler(void)
{
    s_resPos = 0;
    ResetShaping();

    /* Start with the history primed to silence rather than to whatever
     * the previous track left behind, so a seek or a track change cannot
     * ring the filter with stale samples. */
    memset(s_workL, 0, sizeof(int16_t) * s_taps);
    memset(s_workR, 0, sizeof(int16_t) * s_taps);
    s_workCount = s_taps;
}

static float Sincf(float x)
{
    if (x > -1e-6f && x < 1e-6f) return 1.0f;
    float pix = (float)M_PI * x;
    return sinf(pix) / pix;
}

/* Builds the polyphase bank for one input rate.
 *
 * This is the one genuinely expensive piece of start-up work, and the
 * ARM9 has no FPU, so every sinf/cosf is a software routine costing
 * thousands of cycles. Two things keep it to something you would not
 * notice rather than half a second of frozen console:
 *
 *  - single precision, which is roughly twice the speed of double here
 *    and far more accuracy than Q14 coefficients can hold anyway;
 *  - and the filter's own symmetry. A linear-phase windowed sinc obeys
 *    h[P-p][taps-1-t] == h[p][t] exactly, because the tap distance for
 *    that pair is the exact negative and both sinc and Blackman are even
 *    functions. So only half the bank has to be evaluated; the rest is a
 *    copy. (tools/hosttest checks this identity holds to 0 LSB.)
 *
 * The result is cached by (input rate, output rate, taps, phases), so an
 * album of 44.1kHz files pays for it once, on the first track.
 *
 * The output rate belongs in that key and was missing from it. It was
 * harmless while the stream ran at one fixed rate, and became a real
 * fault the moment the rate-cut slider could change it: the cutoff is
 * derived from the ratio between the two rates, so lowering the output
 * rate should narrow the filter - and instead the old bank was kept and
 * the setting did nothing at all. A response measurement caught it,
 * reporting two different stops as bit-identical. */
static void BuildFirTable(uint32_t inputRate)
{
    const uint32_t outRate = Audio_OutputRate();

    if (s_firRate == inputRate && s_firOutRate == outRate &&
        s_firTaps == s_taps && s_firPhases == s_phases) return;

    /* The linear path reads no coefficients. The cache is still marked as
     * built, or this would recompute nothing on every single frame. */
    if (s_taps < RES_TAPS_LITE) {
        s_firRate = inputRate;
        s_firOutRate = outRate;
        s_firTaps = s_taps;
        s_firPhases = s_phases;
        return;
    }

    /* Pick the home before writing a single coefficient, so the build
     * below and every later read agree about where the bank is. */
    s_fir = ((size_t)s_phases * s_taps * sizeof(int16_t) <= sizeof(s_firDtcm))
          ? s_firDtcm : s_firMain;

    /* Cut off below whichever Nyquist is lower - the output's when
     * downsampling, the input's when not - with a margin for the
     * transition band a 12-tap filter cannot make steep. */
    float limit = (inputRate <= Audio_OutputRate())
                ? 0.5f
                : 0.5f * (float)Audio_OutputRate() / (float)inputRate;
    float fc = limit * s_rolloff;

    const int taps = (int)s_taps;

    /* Only the first half of the bank is evaluated; see the note above. */
    const int phases = (int)s_phases;

    for (int p = 0; p <= phases / 2; p++) {
        float frac = (float)p / (float)phases;
        float coef[RES_TAPS_MAX];
        float sum = 0.0f;

        for (int t = 0; t < taps; t++) {
            /* Distance from this tap to the reconstruction point, which
             * sits (RES_TAPS/2 - 1) + frac samples into the window. The
             * resulting constant group delay is harmless. */
            float d = (float)(taps / 2 - 1) + frac - (float)t;
            float a = (float)M_PI * d / (float)(taps / 2);

            float w = 0.42f + 0.50f * cosf(a) + 0.08f * cosf(2.0f * a);  /* Blackman */

            coef[t] = 2.0f * fc * Sincf(2.0f * fc * d) * w;
            sum += coef[t];
        }

        /* Normalise each phase to the same DC gain. Without this the
         * phases differ slightly in level and the output picks up a buzz
         * at the phase-cycle rate. The headroom factor rides along here
         * so it costs nothing at run time. */
        for (int t = 0; t < taps; t++) {
            float v = (sum != 0.0f) ? (coef[t] / sum) * (float)RES_HEADROOM : 0.0f;
            int q = (int)((v * RES_COEF_ONE) + (v >= 0 ? 0.5f : -0.5f));
            if (q > 32767) q = 32767;
            if (q < -32768) q = -32768;
            s_fir[p * taps + t] = (int16_t)q;
        }
    }

    /* Mirror the half that was computed onto the half that was not. */
    for (int p = phases / 2 + 1; p < phases; p++)
        for (int t = 0; t < taps; t++)
            s_fir[p * taps + t] = s_fir[(phases - p) * taps + taps - 1 - t];

    s_firRate = inputRate;
    s_firOutRate = outRate;
    s_firTaps = s_taps;
    s_firPhases = s_phases;
}

/* The rate the resampler is currently set up for.
 *
 * Kept because the rate libmad actually produces is not always the one
 * in the frame header: at half rate it is halved, and that can be
 * switched on and off while a track is playing. Reconfiguring when the
 * two disagree is what makes the switch take effect - without it the
 * filter keeps stepping at the old ratio and the music comes out at the
 * wrong speed. */
static uint32_t s_resInRate = 0;

static void ConfigureResampler(uint32_t inputRate);

/* Two taps cannot upsample, and four are not much better.
 *
 * Going up in rate, the thing the filter has to remove is the image: a
 * mirror of the whole signal sitting just above the *input* rate. Coming
 * down it has to remove aliases instead, and those are far away and easy
 * - which is why the cheap windows have always measured fine and why
 * nothing caught this. Measured on a tone at 22050 into 32768, which is
 * exactly what the rate cut at fifty does:
 *
 *      tone      Raw     Lite     Fast     Best
 *    5000Hz    -26dB    -32dB    -32dB    -32dB
 *    7000Hz    -11dB    -23dB    -32dB    -32dB
 *    9000Hz     -5dB    -11dB    -32dB    -32dB      (-32 is the floor)
 *
 * Five decibels down is not an artefact, it is a second copy of the
 * music an octave and a half up. On hi-hats and sibilance - which live
 * exactly where it is worst - it is a swishing ghost that follows the
 * track, obvious on headphones and inaudible on the console's speakers,
 * which is why it survived this long.
 *
 * Eight taps remove the ghost and take the treble with it - the window
 * is too short to separate a tone at 9kHz from its image at 13kHz, so
 * the passband has to be pulled down to 6kHz to get the stopband there.
 * Measured against the same tone, signal level by frequency:
 *
 *                 2kHz     6kHz     8kHz    9.5kHz
 *      8 taps    -0.2dB   -4.5dB  -10.5dB  -17.6dB
 *     24 taps     0.0dB    0.0dB    0.0dB   -3.2dB
 *
 * So it is the long window or nothing: twenty-four taps is what it takes
 * to keep the music and drop its reflection. That is expensive, and the
 * answer to the expense is not to upsample in the first place - the rate
 * cut no longer asks for it (see kRateCutOut in main.c, where the middle
 * stop now leaves the output at the decoder's own rate and the resampler
 * becomes a pass-through). What is left for this to catch is the file
 * that arrives below the output rate on its own - a 22kHz or 24kHz MP3 -
 * where there is no choice about upsampling, and where the decode is
 * cheap enough to afford the filter. */
static void ApplyUpsampleGuard(uint32_t inputRate)
{
    unsigned taps = s_tapsWanted;
    float roll = s_rolloffWanted;

    if (s_upsampleGuard && inputRate != 0 &&
        inputRate < Audio_OutputRate() && taps < RES_TAPS_MAX) {
        taps = RES_TAPS_MAX;
        roll = 0.90f;
    }

    if (taps == s_taps && roll == s_rolloff) return;

    s_taps = taps;
    s_rolloff = roll;
    s_firRate = 0;                      /* the bank no longer matches */
}

void Mp3Decoder_SetUpsampleGuard(bool on)
{
    if (on == s_upsampleGuard) return;
    s_upsampleGuard = on;
    DEC_LOCK();
    if (s_opened) {
        ConfigureResampler(s_resInRate ? s_resInRate : s_sampleRate);
        ResetResampler();
    }
    DEC_UNLOCK();
}

bool Mp3Decoder_UpsampleGuard(void) { return s_upsampleGuard; }

static void ConfigureResampler(uint32_t inputRate)
{
    if (inputRate == 0) inputRate = Audio_OutputRate();
    s_resInRate = inputRate;

    /* Before the step and the bank, both of which are derived from the
     * window this picks. */
    ApplyUpsampleGuard(inputRate);

    s_resStep = (uint32_t)(((uint64_t)inputRate << 16) / Audio_OutputRate());
    if (s_resStep == 0) s_resStep = 1;

    /* A pass-through has no filter window, so the silence ResetResampler
     * primes as history is not history - it is twenty-four samples of
     * silence that would be played, once, at the head of the stream. The
     * test measured it exactly: RES_TAPS_MAX frames more than the ratio
     * allows. */
    if (s_resStep == (1u << 16)) {
        s_workCount = 0;
        s_resPos = 0;
    }

    BuildFirTable(inputRate);

    /* How many output frames one MPEG frame can turn into. Pump() uses
     * this to decide whether the ring has room before committing to a
     * decode, so it must be an over-estimate, never an under-estimate: if
     * more comes out than the ring can take, Audio_Write silently drops
     * the excess and that is an audible glitch. The slack covers the
     * fractional phase carried across the frame boundary and the filter
     * tail held back in the work buffer. */
    uint64_t maxOut = ((uint64_t)s_samplesPerFrame * Audio_OutputRate()) / inputRate + RES_TAPS_MAX + 8;
    s_maxOutPerFrame = (maxOut > MAX_OUT_FRAMES) ? MAX_OUT_FRAMES : (unsigned)maxOut;
}

static inline int16_t ClampS16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* The stream's rate changed underneath the resampler.
 *
 * Everything about the filter - its step, its coefficients, how many
 * outputs a frame can produce - is derived from the ratio between the
 * file's rate and the stream's. Rebuilding is not optional: left alone
 * it would keep stepping at the old ratio and the music would play at
 * the wrong speed. */
void Mp3Decoder_RateChanged(void)
{
    DEC_LOCK();
    if (s_opened) {
        ResetResampler();
        ConfigureResampler(s_resInRate ? s_resInRate : s_sampleRate);
    }
    DEC_UNLOCK();
}

/* Resamples one decoded MPEG frame from the file's rate to
 * AUDIO_OUT_RATE. maxmod caps a stream at 32768Hz, so a 44.1kHz file
 * genuinely has to be resampled - handing 44100 to mmStreamOpen() just
 * plays it at the wrong speed.
 *
 * Built in ARM mode rather than the Thumb the rest of the project uses.
 * This is the one genuinely hot loop in the program - RES_TAPS
 * multiply-accumulates per output sample, times two channels, 32768
 * times a second - and ARM mode gets a real three-operand multiply and
 * better addressing instead of Thumb-1's cramped two-operand forms. */
/* The multiply-accumulate loop, with the tap count arriving as a compile
 * time constant.
 *
 * It used to run off `s_taps`, a variable, and that one detail was
 * costing most of what this loop spends. A runtime trip count means the
 * compiler cannot unroll: every tap pays an increment, a compare, a
 * branch and three separately indexed loads. With the count constant it
 * unrolls the whole thing, folds the indices into load offsets, and
 * schedules the multiplies against them - and the coefficient load is
 * shared between the two channels either way.
 *
 * Called twice below with the two lengths the quality switch can select,
 * so both come out fully unrolled. always_inline rather than a hint:
 * inlining is the entire point, and a compiler that declined would
 * silently give back the speedup. */
__attribute__((always_inline))
static inline uint32_t ResampleRunTaps(int TAPS, bool mono)
{
    uint32_t outCount = 0;

    /* An output can only be produced once every tap it needs is present. */
    while (outCount < MAX_OUT_FRAMES) {
        unsigned base = s_resPos >> 16;
        if (base + (unsigned)TAPS > s_workCount) break;

        const int16_t *cl = &s_workL[base];
        const int16_t *cr = &s_workR[base];
        const int16_t *h =
            &s_fir[((s_resPos >> (16 - s_phaseBits)) & (s_phases - 1)) * TAPS];

        /* Seeded with half a coefficient step so the shift below rounds to
         * nearest instead of truncating.
         *
         * An arithmetic shift always rounds down, which is not a symmetric
         * error: it biases every sample by about -0.5 LSB regardless of
         * sign, and a constant offset on the output is a DC term the
         * speaker has to sit against, audible as a small click when
         * playback starts or stops. Pre-adding half the step centres the
         * error on zero and halves its average magnitude, for one add per
         * sample against the taps multiply-accumulates already here. */
        int32_t accL = RES_COEF_ONE / 2, accR = RES_COEF_ONE / 2;

        /* Unrolled by hand rather than left to the compiler. -O2 does
         * not turn on -funroll-loops, so a constant trip count alone got
         * the indices folded but still paid a compare and a branch per
         * tap. Four at a time is what the register file holds comfortably
         * with both channels live, and both selectable lengths are
         * multiples of four.
         *
         * The mono path runs one channel instead of two, which is half
         * the multiply-accumulates in the most expensive loop the
         * decoder has. The branch is per output sample, not per tap. */
        if (mono) {
            for (int t = 0; t < TAPS; t += 4) {
                accL += (int32_t)cl[t + 0] * (int32_t)h[t + 0]
                      + (int32_t)cl[t + 1] * (int32_t)h[t + 1]
                      + (int32_t)cl[t + 2] * (int32_t)h[t + 2]
                      + (int32_t)cl[t + 3] * (int32_t)h[t + 3];
            }
            accR = accL;
        } else {
            for (int t = 0; t < TAPS; t += 4) {
                int32_t c0 = h[t + 0], c1 = h[t + 1];
                int32_t c2 = h[t + 2], c3 = h[t + 3];

                accL += (int32_t)cl[t + 0] * c0 + (int32_t)cl[t + 1] * c1
                      + (int32_t)cl[t + 2] * c2 + (int32_t)cl[t + 3] * c3;
                accR += (int32_t)cr[t + 0] * c0 + (int32_t)cr[t + 1] * c1
                      + (int32_t)cr[t + 2] * c2 + (int32_t)cr[t + 3] * c3;
            }
        }

        s_outBuf[outCount * 2 + 0] = ClampS16(accL >> RES_COEF_BITS);
        s_outBuf[outCount * 2 + 1] = ClampS16(accR >> RES_COEF_BITS);
        outCount++;

        s_resPos += s_resStep;
    }

    return outCount;
}

/* The no-filter path: straight two-point linear interpolation.
 *
 * Two multiplies per output sample per channel against Fast's eight, and
 * it exists for one reason - the resampler runs on the decoder thread, so
 * every cycle saved here is a cycle the decoder is not short of. At 320
 * kbps full stereo that is the difference between the ring holding its
 * level and dipping below the boost threshold.
 *
 * What it gives up is real. Linear interpolation is a very poor lowpass:
 * its response droops across the top of the band and it folds everything
 * above Nyquist back with only about 6dB of rejection, so 44.1kHz
 * material aliases audibly on cymbals and sibilance. That is the trade,
 * and it is offered as a setting rather than made here.
 *
 * No clamp on the output: |sample| <= 32768 and the gain is below unity,
 * so the result cannot leave int16. */
/* The cheapest thing that can change a rate at all: take the nearest
 * sample and take nothing else.
 *
 * No multiply, no window, no coefficients - a step through the input and
 * a pair of copies. On this processor that is the difference between a
 * stage that costs a fifth of the decoder and one that barely registers.
 *
 * What it gives up is everything a filter was doing. Dropping samples on
 * an uneven ratio puts a jitter on the waveform, and jitter is
 * broadband: it does not fold one tone to one other place the way a
 * proper alias does, it spreads a little noise everywhere. On a
 * 44100 to 32768 downsample about one output sample in four is a
 * repeat, so the error is at a quarter of the rate and lands high.
 *
 * Offered because the measurement on the console said the resampler was
 * a fifth of the budget, and a fifth is worth being able to spend
 * somewhere else if the ears say it is affordable. */
static uint32_t ResampleRunNearest(void)
{
    uint32_t outCount = 0;

    while (outCount < MAX_OUT_FRAMES) {
        const unsigned base = s_resPos >> 16;
        if (base + 1 > s_workCount) break;

        /* The same headroom every other path applies, so switching
         * between them is not also a change in level. */
        const int32_t l = ((int32_t)s_workL[base] * RES_LINEAR_GAIN) >> RES_COEF_BITS;
        const int32_t r = s_mono ? l
                        : (((int32_t)s_workR[base] * RES_LINEAR_GAIN) >> RES_COEF_BITS);

        s_outBuf[outCount * 2 + 0] = (int16_t)l;
        s_outBuf[outCount * 2 + 1] = (int16_t)r;
        outCount++;

        s_resPos += s_resStep;
    }

    return outCount;
}

static uint32_t ResampleRunLinear(void)
{
    uint32_t outCount = 0;

    while (outCount < MAX_OUT_FRAMES) {
        unsigned base = s_resPos >> 16;
        if (base + 2 > s_workCount) break;

        int32_t f  = (int32_t)(s_resPos & 0xFFFF);
        int32_t l0 = s_workL[base], l1 = s_workL[base + 1];
        int32_t r0 = s_workR[base], r1 = s_workR[base + 1];

        int32_t l = l0 + (((l1 - l0) * f) >> 16);
        int32_t r = r0 + (((r1 - r0) * f) >> 16);

        s_outBuf[outCount * 2 + 0] =
            (int16_t)((l * RES_LINEAR_GAIN) >> RES_COEF_BITS);
        s_outBuf[outCount * 2 + 1] =
            (int16_t)((r * RES_LINEAR_GAIN) >> RES_COEF_BITS);
        outCount++;

        s_resPos += s_resStep;
    }

    return outCount;
}

#define ResampleRun(n) ResampleRunTaps(n, s_mono)

/* Slides whatever libmad has not consumed to the front and tops the
 * buffer up. Returns false when the input is genuinely exhausted.
 *
 * The "no progress" check is what makes end-of-file terminate: at EOF
 * with a partial frame still in the buffer, fread returns 0 forever, and
 * a refill that keeps handing libmad the same unchanged tail would spin
 * for as long as the track is left playing. */
static bool RefillInput(void)
{
    size_t remaining = 0;

    if (s_stream.next_frame != NULL) {
        remaining = (size_t)(s_stream.bufend - s_stream.next_frame);
        if (remaining > INPUT_BUFFER_SIZE) remaining = INPUT_BUFFER_SIZE;
        memmove(s_inputBuf, s_stream.next_frame, remaining);
    }

    /* A full buffer that libmad consumed nothing out of means it is stuck
     * on something it cannot sync to (a free-format frame, or damage).
     * Throwing the block away guarantees the read position keeps moving
     * instead of the same bytes being re-offered forever. */
    /* Against the chunk in force, not the buffer: the buffer is sized for
     * the largest chunk anyone can select and is mostly unused at the
     * smaller ones. */
    if (remaining >= s_readChunk)
        remaining = 0;

    size_t want = s_readChunk - remaining;
    size_t got = fread(s_inputBuf + remaining, 1, want, s_file);

    if (got == 0) {
        /* Nothing new, and the leftovers are the same leftovers as last
         * time: libmad cannot make progress and never will. */
        if (remaining == 0 || remaining == s_prevRemaining) {
            s_inputEof = true;
            return false;
        }
        s_inputEof = true;
    }

    s_prevRemaining = remaining;

    size_t len = remaining + got;

    /* The guard bytes are only *counted* at end of file. Mid-stream they
     * must stay outside the length or libmad would try to decode eight
     * zero bytes as if they were audio; at EOF including them is what
     * lets it finish the final frame. This matches libmad's own
     * minimad.c. */
    if (s_inputEof) {
        memset(s_inputBuf + len, 0, MAD_BUFFER_GUARD);
        len += MAD_BUFFER_GUARD;
    }

    mad_stream_buffer(&s_stream, s_inputBuf, len);
    return true;
}

/* How much is pulled off the card per refill.
 *
 * Eight kilobytes at 192kbps is three reads a second, and on a flashcart
 * a card read is not silent: the SD lines sit next to the headphone amp
 * and each burst couples into it. Three a second is a repeating tick you
 * can hear on headphones and never on the console's speakers - which is
 * the shape of the complaint that led here.
 *
 * Reading more at once does not move fewer bytes, it moves them in fewer
 * bursts: thirty-two kilobytes is one read every one and a third
 * seconds at the same bitrate. Whether that is quieter is a question for
 * ears rather than for arithmetic, which is why it is a setting.
 *
 * The cost is a longer stall per read, absorbed by a ring holding a
 * second of audio, and the memory - which is static and already
 * reserved. */

void Mp3Decoder_SetReadChunk(unsigned bytes)
{
    if (bytes < 4096) bytes = 4096;
    if (bytes > INPUT_BUFFER_SIZE) bytes = INPUT_BUFFER_SIZE;
    DEC_LOCK();
    s_readChunk = bytes;
    DEC_UNLOCK();
}

unsigned Mp3Decoder_ReadChunk(void) { return (unsigned)s_readChunk; }

/* ---- tone ---------------------------------------------------------
 *
 * Three bands, split by two one-pole lowpasses.
 *
 * A proper parametric equaliser is four biquads a channel and this
 * processor is already spending most of itself decoding; a one-pole is
 * one multiply and one add, and three bands out of two of them is enough
 * to be the difference between a preset called Jazz and one called Hip
 * Hop. Which is what the setting is for - it is a tone control with
 * names on it, not a mastering tool.
 *
 * Split by subtraction rather than by three separate filters:
 *
 *   bass   = lp(200Hz)
 *   mid    = lp(3kHz) - lp(200Hz)
 *   treble = x - lp(3kHz)
 *
 * The three sum back to x exactly, so at unity gain the output is the
 * input to the bit - which is why Flat can bypass the whole stage and be
 * provably the same audio rather than nearly the same.
 *
 * The corner coefficients depend on the output rate, which is a setting
 * on the Test page, so they are looked up rather than fixed. */
static const struct { uint32_t rate; int32_t a1, a2; } kToneCoef[] = {
    { 32768,  1233, 14334 },
    { 22050,  1815, 18830 },
    { 16384,  2419, 22397 },
};

/* Gains in twelfths of a bit - 4096 is unity - and the filter's memory.
 * Off by default, and Flat turns it off rather than running it with
 * nothing to do. */
static bool    s_toneOn = false;
static int32_t s_toneGB = 4096, s_toneGM = 4096, s_toneGT = 4096;
static int32_t s_toneA1 = 1233, s_toneA2 = 14334;
static uint32_t s_toneRate = 0;
static int32_t s_lp1L, s_lp1R, s_lp2L, s_lp2R;

/* -12dB to +12dB in Q12. Written out rather than computed: there is no
 * floating point on this processor worth spending on twenty-five
 * constants. */
static const uint16_t kToneGain[25] = {
     1029,  1154,  1295,  1453,  1631,  1830,  2053,  2303,  2584,
     2900,  3254,  3651,  4096,  4596,  5157,  5786,  6492,  7284,
     8173,  9170, 10289, 11544, 12953, 14533, 16306,
};

static int32_t ToneGain(int db)
{
    if (db < -12) db = -12;
    if (db >  12) db =  12;
    return kToneGain[db + 12];
}

/* Under the lock, like the resampler beside it: these are the tone
 * stage's gains and its filter history, and the pump reads all of them
 * while it runs. Set from the UI thread without it, a preset change
 * landing mid-block gave one channel the old gain and the other the new,
 * over a history that had been zeroed underneath both - a click rather
 * than the smooth change it looks like from the Sound page.
 *
 * Bounded where the resampler's race was not: no pointer moves and no
 * array grows, so the worst of it was always audible rather than out of
 * bounds. Locking it is still the cheaper answer than reasoning about
 * which of the six writes can be torn. */
void Mp3Decoder_SetTone(int bassDb, int midDb, int trebleDb)
{
    DEC_LOCK();

    s_toneGB = ToneGain(bassDb);
    s_toneGM = ToneGain(midDb);
    s_toneGT = ToneGain(trebleDb);
    s_toneOn = (bassDb || midDb || trebleDb);

    s_lp1L = s_lp1R = s_lp2L = s_lp2R = 0;
    s_toneRate = 0;                     /* re-read the rate on the next block */

    DEC_UNLOCK();
}

bool Mp3Decoder_ToneActive(void) { return s_toneOn; }

/* A plain lowpass across the top of the band.
 *
 * Not a tone control - that is the Sound page, and this composes with
 * it. This is for the case where something high and thin is riding on
 * the music and the cause is not in the audio at all: interference
 * picked up after the samples were correct, a decoder artefact that
 * cannot be tuned out, a source that was always hissy. A first-order
 * lowpass will not remove any of those cleanly, and it does not pretend
 * to - it takes the whole top of the band down, noise and cymbals
 * together, and whether that trade is worth it is something only the
 * person listening can say.
 *
 * One pole, the same shape the tone stage uses, so it costs a multiply
 * and an add per sample per channel and nothing else. Off is a genuine
 * bypass. */
static int32_t s_cutA = 0;          /* Q15; zero means off */
static uint32_t s_cutHz = 0, s_cutRate = 0;
static int32_t s_cutL, s_cutR;

/* ---- output shaping: the noise filter and the enhancer -------------
 *
 * Two opt-in effects that share one crossover, which is why they are
 * written together: the band split is most of the per-sample cost and
 * doing it once serves both. Both are off by default and the whole stage
 * returns immediately when they are, so a player nobody has switched
 * them on in pays nothing at all.
 *
 * They are on the Test page rather than in Setup because both cost
 * processor on a machine that has been measured not to have much spare,
 * and because what they do is a matter of taste rather than of the
 * player working properly.
 *
 * The crossover is a single pole at about 3.6kHz - a shift rather than a
 * multiply, so it is two adds. That is gentle for a filter but it is the
 * right shape here: neither effect wants a clean split, they want to
 * know roughly which half of the spectrum they are looking at.
 */
#define SHAPE_STOPS MP3_SHAPE_STOPS

/* Where the expander stops leaning, as a sixteen-bit amplitude.
 *
 * Music peaks near full scale; tape and encoder noise sit around -66 to
 * -36dBFS, which is the range these cover. Below the threshold the gain
 * falls one for one with the level, so a sound 10dB under comes out
 * 20dB under - a 2:1 downward expander rather than a gate, because a
 * gate chatters on anything hovering near its threshold and chops the
 * tails off reverb. */
static const int32_t kNoiseThreshold[SHAPE_STOPS] =
    { 0, 16, 33, 65, 130, 260, 519 };

/* And how much synthesised harmonic the enhancer mixes back, in Q16. */
static const int32_t kEnhanceAmount[SHAPE_STOPS] =
    { 0, 6554, 13107, 19661, 26214, 39322, 52429 };

static int     s_nfLevel = 0;          /* 0 = off */
static int     s_exLevel = 0;          /* 0 = off */
static int32_t s_nfRecip = 0;          /* 1/threshold, Q24, from the level */

/* Per-channel filter state. Reset wherever the resampler is, because the
 * same moments matter: a seek or a new track must not carry the previous
 * audio's envelope into the first block of the next. */
static int32_t s_shLp[2], s_shLp2[2], s_nfEnv[2], s_exDc[2];

/* What the expander is currently doing, smoothed, so the row can show
 * it. Q16, 65536 meaning it is not pulling anything down. */
static int32_t s_nfShown = 65536;

static void ResetShaping(void)
{
    s_shLp[0] = s_shLp[1] = 0;
    s_shLp2[0] = s_shLp2[1] = 0;
    s_nfEnv[0] = s_nfEnv[1] = 0;
    s_exDc[0] = s_exDc[1] = 0;
    s_nfShown = 65536;
}

void Mp3Decoder_SetNoiseFilter(int level)
{
    if (level < 0) level = 0;
    if (level >= SHAPE_STOPS) level = SHAPE_STOPS - 1;

    DEC_LOCK();
    s_nfLevel = level;
    /* The reciprocal once here rather than a divide per sample: the
     * ARM9 has no divider and this is the only division the stage would
     * otherwise need. Q24 so that a threshold-sized envelope comes back
     * out as exactly unity in Q16. */
    s_nfRecip = level ? ((int32_t)1 << 24) / kNoiseThreshold[level] : 0;
    if (!level) s_nfShown = 65536;
    DEC_UNLOCK();
}

void Mp3Decoder_SetEnhance(int level)
{
    if (level < 0) level = 0;
    if (level >= SHAPE_STOPS) level = SHAPE_STOPS - 1;
    DEC_LOCK();
    s_exLevel = level;
    DEC_UNLOCK();
}

int Mp3Decoder_NoiseFilter(void) { return s_nfLevel; }
int Mp3Decoder_Enhance(void)     { return s_exLevel; }
int Mp3Decoder_ShapeStops(void)  { return SHAPE_STOPS; }

/* How far the expander is pulling the top end down right now, in whole
 * decibels, so the row can say whether it is doing anything. Six per
 * halving, interpolated across the last one. */
int Mp3Decoder_NoiseCutDb(void)
{
    int32_t g = s_nfShown;
    if (!s_nfLevel || g >= 65536) return 0;

    int db = 0;
    while (g < 32768 && db > -60) { g <<= 1; db -= 6; }
    db -= (int)((6 * (65536 - g)) / 32768);
    return db < -60 ? -60 : db;
}

/* Both effects over one block of interleaved stereo.
 *
 * Costed to be skippable: the test at the top is the whole price when
 * neither is on, and neither is on unless somebody went to the Test page
 * and moved a slider. */
static void ApplyShaping(int16_t *buf, uint32_t frames)
{
    if ((!s_nfLevel && !s_exLevel) || frames == 0) return;

    const int32_t thr    = kNoiseThreshold[s_nfLevel];
    const int32_t recip  = s_nfRecip;
    const int32_t amount = kEnhanceAmount[s_exLevel];
    int32_t worst = 65536;

    for (unsigned c = 0; c < 2; c++) {
        int32_t lp = s_shLp[c], lp2 = s_shLp2[c], env = s_nfEnv[c];
        int32_t dc = s_exDc[c];

        for (uint32_t i = 0; i < frames; i++) {
            const int32_t x = buf[i * 2 + c];

            /* The split, and a second, slower pole that leaves a middle
             * band between them for the enhancer to work from. */
            lp  += (x  - lp)  >> 1;      /* ~3.6kHz */
            lp2 += (lp - lp2) >> 4;      /* ~340Hz  */

            int32_t hi = x - lp;
            if (hi >  32767) hi =  32767;
            if (hi < -32767) hi = -32767;

            if (s_nfLevel) {
                const int32_t a = hi < 0 ? -hi : hi;

                /* Fast up, slow down: the attack has to catch a cymbal
                 * before it is over, the release has to not breathe. */
                env += (a > env) ? ((a - env) >> 3) : ((a - env) >> 11);

                int32_t e = env > thr ? thr : env;
                int32_t g = (e * recip) >> 8;        /* Q16, <= 65536 */

                /* A floor rather than silence: a passage that mutes
                 * completely sounds more broken than one that is merely
                 * quiet. */
                if (g < 6554) g = 6554;              /* -20dB */
                if (g > 65536) g = 65536;
                if (g < worst) worst = g;

                hi = (hi * g) >> 16;
            }

            if (s_exLevel) {
                /* Harmonics made from the middle band, so that what they
                 * add lands in the top one.
                 *
                 * A true square, not the signed one. x*|x| keeps the
                 * sign and so keeps the period, which makes odd
                 * harmonics and - overwhelmingly - more of the
                 * fundamental: measured, it simply turned the slider
                 * into a midrange volume control, doubling a 3kHz tone
                 * while adding nothing at 6kHz. Squaring halves the
                 * period, which is what puts energy an octave up.
                 *
                 * It also makes the result positive everywhere, so it
                 * carries a large DC term that has to come back off or
                 * the effect is an offset rather than a harmonic. A slow
                 * pole tracks it. */
                const int32_t mid = lp - lp2;
                const int32_t sq  = (mid * mid) >> 15;
                dc += (sq - dc) >> 6;
                hi += ((sq - dc) * amount) >> 16;
            }

            int32_t y = lp + hi;
            if (y >  32767) y =  32767;
            if (y < -32768) y = -32768;
            buf[i * 2 + c] = (int16_t)y;
        }

        s_shLp[c] = lp; s_shLp2[c] = lp2; s_nfEnv[c] = env; s_exDc[c] = dc;
    }

    /* Eased toward what the block actually did, so the readout settles
     * instead of flickering between blocks. */
    s_nfShown += (worst - s_nfShown) >> 2;
}

void Mp3Decoder_SetTrebleCut(unsigned hz)
{
    DEC_LOCK();                     /* the pump reads all three */
    s_cutHz = hz;
    s_cutRate = 0;                  /* recompute against the live rate */
    s_cutL = s_cutR = 0;
    DEC_UNLOCK();
}

unsigned Mp3Decoder_TrebleCut(void) { return s_cutHz; }

static void ApplyTrebleCut(int16_t *buf, uint32_t frames)
{
    if (s_cutHz == 0 || frames == 0) return;

    const uint32_t rate = Audio_OutputRate();
    if (rate != s_cutRate) {
        s_cutRate = rate;
        /* a = 1 - exp(-2*pi*fc/fs), near enough as 2*pi*fc/fs for the
         * corners on offer, which are all well under a fifth of the
         * rate. Clamped so a silly pairing cannot make it unstable. */
        int64_t a = ((int64_t)s_cutHz * 411774) / (int64_t)rate;  /* 2pi in Q16 */
        a = (a * 32768) >> 16;
        if (a > 32768) a = 32768;
        if (a < 1) a = 1;
        s_cutA = (int32_t)a;
    }

    const int32_t a = s_cutA;
    int32_t l = s_cutL, r = s_cutR;

    for (uint32_t i = 0; i < frames; i++) {
        l += ((buf[i * 2 + 0] - l) * a) >> 15;
        r += ((buf[i * 2 + 1] - r) * a) >> 15;
        buf[i * 2 + 0] = ClampS16(l);
        buf[i * 2 + 1] = ClampS16(r);
    }

    s_cutL = l; s_cutR = r;
}

/* Shapes one block of finished audio, in place.
 *
 * After the resampler, so it runs once per output sample whatever the
 * rate ladder is doing, and before the ring, so nothing downstream has
 * to know about it. */
static void ApplyTone(int16_t *buf, uint32_t frames)
{
    if (!s_toneOn || frames == 0) return;

    const uint32_t rate = Audio_OutputRate();
    if (rate != s_toneRate) {
        s_toneRate = rate;
        s_toneA1 = kToneCoef[0].a1;
        s_toneA2 = kToneCoef[0].a2;
        for (unsigned i = 0; i < sizeof(kToneCoef) / sizeof(kToneCoef[0]); i++)
            if (kToneCoef[i].rate == rate) {
                s_toneA1 = kToneCoef[i].a1;
                s_toneA2 = kToneCoef[i].a2;
                break;
            }
    }

    const int32_t a1 = s_toneA1, a2 = s_toneA2;
    const int32_t gb = s_toneGB, gm = s_toneGM, gt = s_toneGT;
    int32_t lp1L = s_lp1L, lp1R = s_lp1R, lp2L = s_lp2L, lp2R = s_lp2R;

    for (uint32_t i = 0; i < frames; i++) {
        const int32_t xl = buf[i * 2 + 0], xr = buf[i * 2 + 1];

        lp1L += ((xl - lp1L) * a1) >> 15;
        lp2L += ((xl - lp2L) * a2) >> 15;
        lp1R += ((xr - lp1R) * a1) >> 15;
        lp2R += ((xr - lp2R) * a2) >> 15;

        const int32_t yl = (lp1L * gb + (lp2L - lp1L) * gm
                            + (xl - lp2L) * gt) >> 12;
        const int32_t yr = (lp1R * gb + (lp2R - lp1R) * gm
                            + (xr - lp2R) * gt) >> 12;

        buf[i * 2 + 0] = ClampS16(yl);
        buf[i * 2 + 1] = ClampS16(yr);
    }

    s_lp1L = lp1L; s_lp1R = lp1R; s_lp2L = lp2L; s_lp2R = lp2R;
}

#ifdef ARM9
/* noinline as well as target("arm"): inlined into its Thumb caller, the
 * instruction-set attribute is simply dropped.
 *
 * And in ITCM, for the reason the coefficients are in DTCM. This is the
 * hottest loop in the program; held in tightly-coupled memory it fetches
 * in one cycle and stops competing with libmad for an 8KB instruction
 * cache - which is decode throughput, not drawing time. ITCM is 32KB and
 * the build was using two.
 *
 * This and the DTCM bank went out together once and that build showed a
 * black screen. DTCM has since gone in alone and boots, so this is the
 * remaining half and it goes in alone too. If a build carrying this one
 * fails to boot, this commit is the whole of it - revert it and the
 * player is back to a known-good state with the DTCM half intact. */
__attribute__((section(".itcm"), long_call, target("arm"), noinline))
#endif
static uint32_t ResampleFrame(const mad_fixed_t *left, const mad_fixed_t *right,
                              unsigned length)
{
    const int taps = (int)s_taps;

    if (length > MAD_MAX_SAMPLES) length = MAD_MAX_SAMPLES;

    /* Append the new samples after whatever tail the previous frame left
     * behind, so the filter window can straddle the frame boundary. */
    if (s_workCount + length > RES_WORK_MAX)
        length = RES_WORK_MAX - s_workCount;

    /* Mono is folded here, before the filter, so the filter has one
     * channel to do rather than two. Folding it afterwards would save
     * nothing - the expensive part is already done by then. */
    if (s_mono) {
        for (unsigned i = 0; i < length; i++) {
            const int32_t l = ScaleToS16(left[i]);
            const int32_t r = right ? ScaleToS16(right[i]) : l;
            s_workL[s_workCount + i] = (int16_t)((l + r) / 2);
            s_workR[s_workCount + i] = s_workL[s_workCount + i];
        }
    } else {
        for (unsigned i = 0; i < length; i++) {
            int16_t l = ScaleToS16(left[i]);
            s_workL[s_workCount + i] = l;
            s_workR[s_workCount + i] = right ? ScaleToS16(right[i]) : l;
        }
    }
    s_workCount += length;

    /* Nothing to resample.
     *
     * When the stream runs at the rate libmad is producing - half rate
     * on a 44.1kHz file into a 22050Hz stream, say - the filter would be
     * computing a weighted average of samples in order to arrive back at
     * the sample it started from. The whole stage disappears instead,
     * which is the largest saving available to it by a wide margin. */
    uint32_t outCount;

    if (s_resStep == (1u << 16)) {
        uint32_t out = s_workCount;
        if (out > MAX_OUT_FRAMES) out = MAX_OUT_FRAMES;

        /* The same headroom every other path applies, for the same
         * reason: a pass-through cannot overshoot and does not need it,
         * but without it this one setting would come out 2.2dB louder
         * than the rest, and louder is heard as better whatever it costs.
         * The test caught exactly that. */
        for (uint32_t i = 0; i < out; i++) {
            const int32_t l = ((int32_t)s_workL[i] * RES_LINEAR_GAIN) >> RES_COEF_BITS;
            const int32_t r = s_mono ? l
                            : (((int32_t)s_workR[i] * RES_LINEAR_GAIN) >> RES_COEF_BITS);
            s_outBuf[i * 2 + 0] = (int16_t)l;
            s_outBuf[i * 2 + 1] = (int16_t)r;
        }

        /* Position advanced as the filter would have advanced it, so the
         * trim below consumes exactly what was taken. Returning from here
         * instead would leave the work buffer never emptied, and one
         * frame later it would be full and accepting nothing. */
        s_resPos += (uint32_t)out << 16;
        outCount = out;
    } else {
        outCount = (taps == RES_TAPS_NONE) ? ResampleRunNearest()
                 : (taps == RES_TAPS_RAW)  ? ResampleRunLinear()
                 : (taps == RES_TAPS_LITE) ? ResampleRun(RES_TAPS_LITE)
                 : (taps == RES_TAPS_FAST) ? ResampleRun(RES_TAPS_FAST)
                 : (taps == RES_TAPS_LOW)  ? ResampleRun(RES_TAPS_LOW)
                                           : ResampleRun(RES_TAPS_MAX);
    }

    /* Drop everything the filter window has moved past, keeping the few
     * samples the next window still needs.
     *
     * Clamped, because the position can legitimately end up past the end
     * of the buffer. The linear path stops when base + 2 exceeds what it
     * has, so base can be workCount - 2 with a fraction just under one -
     * and it has already added the step by then. Above 65536Hz in the
     * source the step exceeds 2, so that lands beyond workCount: at 96kHz
     * it is 2.93, and consumed came out one greater than the count.
     *
     * `keep` is unsigned, so one too many became four billion and the
     * memmove below read the whole address space. The FIR paths never hit
     * this - their guard reserves `taps` samples, and four is already
     * more than the largest step - so it took a 96kHz file played on the
     * Raw setting to find, which is exactly the pair in the test now.
     *
     * The subtraction replaces `s_resPos &= 0xFFFF` and is the same thing
     * whenever consumed was not clamped. Where it was, it keeps the
     * overshoot instead of discarding it, so the next window resumes at
     * the position the step actually reached rather than a fraction of a
     * sample behind it. */
    unsigned consumed = s_resPos >> 16;
    if (consumed > s_workCount) consumed = s_workCount;

    if (consumed > 0) {
        unsigned keep = s_workCount - consumed;
        memmove(s_workL, &s_workL[consumed], keep * sizeof(int16_t));
        memmove(s_workR, &s_workR[consumed], keep * sizeof(int16_t));
        s_workCount = keep;
        s_resPos -= (uint32_t)consumed << 16;
    }

    return outCount;
}


/* ------------------------------------------------------------------ */

/* Control operations all serialise against the worker. Each may block the
 * caller for as long as one MPEG frame - the worker releases the lock
 * between frames, never inside one - which happens only on a track change
 * or a seek, and is invisible next to the disc access those involve. */
const char *Mp3Decoder_Title(void)  { return s_title; }
const char *Mp3Decoder_Artist(void) { return s_artist; }

bool Mp3Decoder_Open(const char *path)
{
    Mp3Decoder_Close();

    DEC_LOCK();
    s_finished = false;

    s_file = fopen(path, "rb");
    if (!s_file) { DEC_UNLOCK(); return false; }

    fseek(s_file, 0, SEEK_END);
    s_fileSize = ftell(s_file);
    fseek(s_file, 0, SEEK_SET);

    if (s_fileSize <= 0) {
        fclose(s_file);
        s_file = NULL;
        DEC_UNLOCK();
        return false;
    }

    /* A new track starts at full quality. Relief is a response to this
     * passage being too much, not a verdict on the next file - and a
     * track opened after a hard one should not inherit its bandwidth. */
    if (s_reliefOn) {
        s_reliefOn = false;
        s_reliefEasySince = 0;
        s_halfRate = false;
    }

    /* And the clock that decides relief is reset with it, which it was
     * not.
     *
     * The worker boosts itself when the ring runs low and stands down
     * again - engaging relief - if half a second of boosting has not
     * refilled it. s_boostSince is when that boost began, and opening a
     * track left it pointing at the previous one. Every open empties the
     * ring, so the worker is almost always boosted across a track
     * change: skip twice in a row and the second track inherits a
     * stopwatch that is already most of the way through. It then trips
     * the stand-down on a file that was never struggling, drops to half
     * rate - everything above about 11kHz gone - and stays there for the
     * four seconds relief takes to release.
     *
     * Skipping through tracks is an ordinary thing to do, and this made
     * it cost audible bandwidth for no reason. The boost itself is left
     * running: the ring really is empty and the new track really does
     * need filling. It is the age of the boost that must not carry
     * over, and the cooldown from the old track's stand-down with it. */
    s_boostSince = tickGetCount();
    s_boostBlockUntil = 0;
    /* Counted per track, like the glitches it is shown beside: the row
     * says "this track, so far" and it has to mean that for both. */
    s_reliefCount = 0;

    s_title[0] = s_artist[0] = '\0';
    /* Before the tag is walked, so a track with no picture clears the one
     * the last track left in the frame rather than keeping it - and so
     * that anything fetching the previous track's cover sees the buffer
     * go unowned before it is written again. */
    ClearArt();
    s_artReport.outcome = ART_NONE;
    s_artReport.bytes = s_artReport.readMs = s_artReport.decodeMs = 0;
    s_artOffset = s_artSize = 0;
    s_artV22 = false;
    snprintf(s_artPath, sizeof(s_artPath), "%s", path);

    /* Sniffed from the file, not from the extension.
     *
     * The browser already decides what to list by extension, and that is
     * the right place for it - but what to *decode* has to come from the
     * bytes, or a mis-named file is played as noise through a speaker
     * somebody is wearing. */
    s_dataStart = SkipId3v2(s_file);
    ReadId3v1(s_file);          /* fills whatever v2 did not */

    /* The tag walk noted where the picture is; fetching it is somebody
     * else's job and happens after this returns, so opening a track
     * costs nothing for a cover however large it is. */
    if (s_wantArt) RequestArt();

    fseek(s_file, s_dataStart, SEEK_SET);

    mad_stream_init(&s_stream);
    /* Whatever was chosen on the Test page survives this. */
    mad_stream_options(&s_stream, MadOptions());
    mad_frame_init(&s_frame);
    mad_synth_init(&s_synth);

    s_opened        = true;
    s_inputEof      = false;
    s_decodeDone    = false;
    s_sawFirstFrame = false;
    s_prevRemaining = (size_t)-1;
    s_needRefill    = true;

    s_sampleRate      = 0;
    s_bitrate         = 0;
    s_channels        = 2;
    s_samplesPerFrame = 1152;

    s_hasVbrHeader = false;
    s_isVbr        = false;
    s_vbrFrames    = 0;
    s_vbrBytes     = 0;
    s_hasToc       = false;

    s_baseMs         = 0;
    s_totalMs        = 0;
    s_lastFrameCount = 0;
    s_maxOutPerFrame = MAX_OUT_FRAMES;

    /* The resampler is configured from the first decoded frame, once the
     * real sample rate is known - configuring it here for a guessed rate
     * would build a filter table and then throw it away on every track. */
    ResetResampler();
    Audio_Reset();

    DEC_UNLOCK();
    return true;
}

void Mp3Decoder_Close(void)
{
    DEC_LOCK();
    s_finished = false;
    if (!s_opened) { DEC_UNLOCK(); return; }

    mad_synth_finish(&s_synth);
    mad_frame_finish(&s_frame);
    mad_stream_finish(&s_stream);

    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }

    s_opened = false;
    s_lastFrameCount = 0;

    DEC_UNLOCK();
}

bool Mp3Decoder_IsOpen(void) { return s_opened; }

static bool PumpLocked(int maxFrames)
{
    if (!s_opened) return false;

    /* Two separate budgets: how many frames we are willing to decode in
     * one call, and how many times round the loop we will go trying. The
     * second one matters because a refill or a corrupt frame costs an
     * iteration without producing audio, and without a cap a damaged file
     * could hold the main loop indefinitely. */
    /* How much work this call may do is decided by the clock and by how
     * much audio is left, not by a fixed frame count.
     *
     * A fixed count cannot express the actual priority. Set it high and a
     * catch-up burst freezes the UI for a hundred milliseconds; set it low
     * and there is no headroom left to recover from a stall, which risks
     * the one thing that must never break. A budget does both: while the
     * buffer is healthy, stop as soon as the time is spent and let the UI
     * have the frame; when the buffer is running dry, ignore the clock and
     * decode until it is safe again. Audio first, UI second, decided per
     * call instead of guessed once. */
    const uint32_t kBudgetNormalUs = 2500;

    /* Below a quarter of the ring - about 125ms of audio left - catching
     * up matters more than the frame rate, and the clock is ignored
     * entirely until the buffer is healthy again.
     *
     * An earlier version gave this case a larger time budget instead of
     * no budget, which does not work: the deadline can only be checked
     * between frames, and one frame already costs about as long as the
     * whole "generous" budget. It bought a single extra frame, so a ring
     * that had drained could never refill and the underrun counter simply
     * climbed forever. A catch-up path has to be able to run long. */
    bool urgent = Audio_QueuedFrames() < BOOST_ON_BELOW;

    uint64_t deadline = tickGetCount() +
                        ((uint64_t)TICK_FREQ * kBudgetNormalUs) / 1000000u;

    /* How many frames one call may decode is the caller's to choose,
     * because it decides how long the decoder lock is held. */
    const int kMaxAttempts = 32;

    int decoded = 0;
    int attempts = 0;

    while (!s_decodeDone && decoded < maxFrames && attempts < kMaxAttempts) {
        attempts++;

        if (Audio_FreeFrames() < s_maxOutPerFrame)
            break;

        /* Checked after the first frame, never before it: one frame per
         * call has to happen regardless or a slow frame rate could stall
         * decoding entirely. Skipped completely while catching up. */
        if (!urgent && decoded > 0 && tickGetCount() >= deadline)
            break;

        if (s_needRefill) {
            if (!RefillInput()) {
                s_decodeDone = true;
                break;
            }
            s_needRefill = false;
        }

        const uint64_t tDecode0 = tickGetCount();

        if (mad_frame_decode(&s_frame, &s_stream) == -1) {
            if (s_stream.error == MAD_ERROR_BUFLEN) {
                s_needRefill = true;
                continue;                       /* need more bytes */
            }
            if (MAD_RECOVERABLE(s_stream.error))
                continue;                       /* libmad has resynced */
            s_decodeDone = true;                /* unrecoverable */
            break;
        }

        bool isHeaderFrame = false;

        const uint64_t tSynth0 = tickGetCount();
        s_madTicks += tSynth0 - tDecode0;

        if (!s_sawFirstFrame) {
            s_sawFirstFrame = true;
            s_sampleRate = s_frame.header.samplerate;
            s_bitrate    = (uint32_t)s_frame.header.bitrate;
            s_channels   = (s_frame.header.mode == MAD_MODE_SINGLE_CHANNEL) ? 1 : 2;

            if (s_mono) FoldFrameToMono(&s_frame);
            mad_synth_frame(&s_synth, &s_frame);
            s_samplesPerFrame = s_synth.pcm.length ? s_synth.pcm.length : 1152;

            if (s_stream.this_frame && s_stream.next_frame > s_stream.this_frame) {
                size_t frameLen = (size_t)(s_stream.next_frame - s_stream.this_frame);
                ParseVbrHeader(s_stream.this_frame, frameLen, &s_frame.header);
            }

            ConfigureResampler(s_sampleRate);
            ComputeTotalMs();

            /* A Xing/VBRI frame carries no music - it is a silent frame
             * whose payload is the header itself - so it is parsed and
             * then dropped rather than played. */
            isHeaderFrame = s_hasVbrHeader || s_hasToc;
        } else {
            if (s_mono) FoldFrameToMono(&s_frame);
            mad_synth_frame(&s_synth, &s_frame);
        }

        const uint64_t tRes0 = tickGetCount();
        s_synthTicks += tRes0 - tSynth0;

        struct mad_pcm *pcm = &s_synth.pcm;

        if (isHeaderFrame || pcm->length == 0)
            continue;

        /* Follow what libmad actually produced rather than what the
         * header said. Half rate halves it, and it can be turned on
         * while a track is playing. */
        if (pcm->samplerate && pcm->samplerate != s_resInRate)
            ConfigureResampler(pcm->samplerate);

        const mad_fixed_t *left  = pcm->samples[0];
        const mad_fixed_t *right = (pcm->channels == 2) ? pcm->samples[1] : NULL;

        unsigned n = pcm->length;
        if (n > MAD_MAX_SAMPLES) n = MAD_MAX_SAMPLES;

        /* The visualizer's copy, and only when something is going to
         * look at it.
         *
         * Eleven hundred and fifty-two samples converted twice over,
         * thirty-eight times a second, for a meter that may not be on
         * screen at all - with a card or nothing up there it was pure
         * waste, and it was being counted as resampler time because it
         * sits inside that stage's clock. */
        if (s_wantSamples) {
            for (unsigned i = 0; i < n; i++) {
                int16_t l = ScaleToS16(left[i]);
                s_lastFrame[i * 2 + 0] = l;
                s_lastFrame[i * 2 + 1] = right ? ScaleToS16(right[i]) : l;
            }
            s_lastFrameCount = (int)n;
        } else {
            s_lastFrameCount = 0;
        }

        uint32_t outFrames = ResampleFrame(left, right, n);
        if (outFrames) {
            ApplyTone(s_outBuf, outFrames);
            ApplyTrebleCut(s_outBuf, outFrames);
            ApplyShaping(s_outBuf, outFrames);
            Audio_Write(s_outBuf, outFrames);
        }

        /* How much of real time this is costing.
         *
         * Ticks spent turning bytes into audio against the duration of
         * the audio that came out. Below a hundred there is headroom;
         * at a hundred there is none, and everything else the program
         * does has to come out of the ring - which is the whole story of
         * why scrolling seizes at 320 kbps and not at 192.
         *
         * Averaged over a windowful rather than reported per frame: one
         * frame is 26ms of audio and a handful of milliseconds of noise
         * either way, and a number that jumps between 60 and 140 tells
         * nobody anything. */
        s_resTicks   += tickGetCount() - tRes0;
        s_loadFrames += outFrames;

        if (s_loadFrames >= Audio_OutputRate() / 2) {  /* half a second */
            const uint64_t realTicks =
                ((uint64_t)s_loadFrames * TICK_FREQ) / Audio_OutputRate();
            if (realTicks) {
                s_madPct   = (uint32_t)((s_madTicks   * 100u) / realTicks);
                s_synthPct = (uint32_t)((s_synthTicks * 100u) / realTicks);
                s_resPct   = (uint32_t)((s_resTicks   * 100u) / realTicks);
            }
            s_madTicks = s_synthTicks = s_resTicks = 0;
            s_loadFrames = 0;
        }

        decoded++;
    }

    /* Only report the track as over once the mixer has drained what was
     * already queued, otherwise the last half second of every song gets
     * cut off by the auto-advance. */
    if (s_decodeDone && Audio_QueuedFrames() == 0)
        return false;

    return true;
}

bool Mp3Decoder_Pump(void)
{
    DEC_LOCK();
    bool more = PumpLocked(MP3_MAX_FRAMES_PER_PUMP);
    DEC_UNLOCK();
    if (!more) s_finished = true;
    return more;
}

/* The main thread's backstop, and the reason it can no longer stall the
 * UI: if the worker already holds the lock then it is doing exactly this
 * work right now, and waiting behind it adds nothing but a frozen screen.
 *
 * That stall was real and it had a precise trigger. Below a quarter of
 * the ring the pump enters catch-up mode, where it ignores the clock and
 * decodes up to twelve frames in one go. At 128 kbps the ring never sinks
 * that far, so it never fired. At 320 kbps it does - and twelve frames at
 * ~18ms each is 216ms with the lock held, which the main thread then sat
 * behind. Hence a slowdown that only ever showed up on high-bitrate
 * files. */
/* On the console this no longer decodes anything, and that is the point.
 *
 * The main thread's stack is the 13.5KB that fits in DTCM below
 * __sp_usr, and a codec does not fit in it. FAAD2's channel_pair_element
 * takes 24320 bytes on its own and dr_flac has a 16384 frame, against
 * libmad's 7104 - so MP3 fitted, which is why this stood for as long as
 * MP3 was the only format. AAC ran off the bottom of DTCM and took the
 * console down with a data abort at 0x02feca70, which is __sp_usr less
 * about 29KB: the stack simply kept going past the end of DTCM.
 *
 * The worker's stack is 64KB, sized from those same measurements, so
 * decoding belongs there and only there. This was already described as
 * an opportunistic top-up rather than something playback depends on -
 * recovery is the boosted worker's job - so there is nothing to replace
 * it with. End of track is still noticed by the worker, which sets
 * s_finished once the ring has drained. */
bool Mp3Decoder_PumpIfIdle(void)
{
#ifdef ARM9
    return s_opened && !s_finished;
#else
    /* The host has no threads, so the tests drive the decoder from here
     * and this stays what it was. */
    bool more = PumpLocked(2);
    if (!more) s_finished = true;
    return more;
#endif
}

/* Whether the decoder is currently being given less to do, and how many
 * times that has happened. On the Test page, because "the sound went
 * dull for a moment" and "the sound broke up" are different faults and
 * the difference is not guessable from the outside. */
bool     Mp3Decoder_ReliefActive(void) { return s_reliefOn; }
uint32_t Mp3Decoder_ReliefCount(void)  { return s_reliefCount; }

bool Mp3Decoder_IsCatchingUp(void)
{
#ifdef ARM9
    return s_workerBoosted;
#else
    return false;
#endif
}

uint32_t Mp3Decoder_CatchUpCount(void)
{
#ifdef ARM9
    return s_boostCount;
#else
    return 0;
#endif
}

void Mp3Decoder_ResetCatchUpCount(void)
{
#ifdef ARM9
    s_boostCount = 0;
#endif
}

uint32_t Mp3Decoder_SeekUs(void)
{
#ifdef ARM9
    return s_seekUs;
#else
    return 0;
#endif
}


bool Mp3Decoder_Finished(void)
{
    return s_finished;
}

#ifdef ARM9
static int DecodeWorker(void *arg)
{
    (void)arg;

    for (;;) {
        bool worked = false;

        /* Cheap, unlocked, and checked every pass: the decision only
         * needs the ring's fill level, which the mixer publishes. */
        uint32_t queued = Audio_QueuedFrames();

        /* Never boosted while scrubbing.
         *
         * A seek empties the ring, so the buffer is always "at risk"
         * during one and the worker would sit above the UI for as long as
         * the button was held - preempting the very drawing the user is
         * watching to see where they have got to. There is no audio to
         * protect mid-scrub: it has already been thrown away and will be
         * refilled when they let go. */
        if (s_scrubbing || s_resuming) {
            if (s_workerBoosted) {
                s_workerBoosted = false;
                threadSetPrio(&s_worker, WORKER_PRIO_IDLE);
            }
        } else if (!s_workerBoosted && queued < BOOST_ON_BELOW &&
                   tickGetCount() >= s_boostBlockUntil) {
            s_workerBoosted = true;
            s_boostCount++;
            s_boostSince = tickGetCount();
            threadSetPrio(&s_worker, WORKER_PRIO_URGENT);
        } else if (s_workerBoosted && queued > BOOST_OFF_ABOVE) {
            s_workerBoosted = false;
            threadSetPrio(&s_worker, WORKER_PRIO_IDLE);
        } else if (s_workerBoosted &&
                   tickGetCount() - s_boostSince > BOOST_MAX_TICKS) {
            /* Held for half a second and the ring is still below the mark,
             * which means the decoder is not merely behind - it is slower
             * than the audio it is producing. Standing down here trades a
             * gap in the sound for a console that answers its buttons. */
            s_workerBoosted = false;
            s_boostBlockUntil = tickGetCount() + BOOST_COOLDOWN;
            threadSetPrio(&s_worker, WORKER_PRIO_IDLE);

            /* And make the work smaller, rather than only deciding who
             * gets to do it. Standing down without this is the cycle
             * described at RELIEF_RELEASE_TICKS: the gap in the sound
             * comes back every half second until the passage ends. */
            if (!s_reliefOn && !s_halfRate) {
                s_reliefOn = true;
                s_reliefCount++;
                ReliefSetHalfRate(true);
            }
        }

        /* Back up again once there is genuinely room, held long enough
         * that recovering is not itself the thing that starts the next
         * shortfall. */
        if (s_reliefOn) {
            if (queued > BOOST_OFF_ABOVE * 2) {
                if (!s_reliefEasySince) s_reliefEasySince = tickGetCount();
                else if (tickGetCount() - s_reliefEasySince > RELIEF_RELEASE_TICKS) {
                    s_reliefOn = false;
                    s_reliefEasySince = 0;
                    ReliefSetHalfRate(false);
                }
            } else {
                s_reliefEasySince = 0;
            }
        }

        /* Nothing to decode during a scrub.
         *
         * Every seek step resets the ring, so anything produced between
         * steps is thrown away unheard - and producing it means holding
         * the decoder lock for a whole MPEG frame, which is time the main
         * thread spends waiting instead of drawing. Standing down for the
         * length of the scrub gives the UI the CPU and the lock both. */
        if (s_scrubbing) {
            threadSleep(2000);
            continue;
        }

        /* The seek a scrub left behind, done here so the main thread
         * never waits for a card access. */
        Mp3Decoder_RunPendingSeek();

        /* Back to audible only once there is enough to play from without
         * immediately running dry again. */
        /* Half the ring - about 250ms - before sound comes back.
         *
         * This was a sixteenth, on the reasoning that a shorter wait
         * meant less silence. That was exactly backwards. A muted mixer
         * does not consume, so the ring refills at full decode speed and
         * waiting for a real cushion costs almost no wall time at all -
         * whereas unmuting at 30ms handed the mixer a buffer it drained
         * in 30ms, and playback then spent a second underrunning its way
         * back up. Those underruns are also what put a climbing "buffer"
         * count on the top screen after every seek.
         *
         * Silence for a moment is clean. Audio that stutters for a second
         * is not, and it costs more of both. */
        if (s_resuming && Audio_QueuedFrames() >= RESUME_READY_FRAMES) {
            s_resuming = false;
            Audio_SetMuted(false);
        }

        DEC_LOCK();
        if (s_opened && !s_finished) {
            if (s_decodeDone) {
                /* Last bytes are decoded; all that is left is to notice
                 * when the ring finally empties. Deliberately not a pump:
                 * pumping here would return "still playing" without doing
                 * any work, and the loop would spin at full tilt for the
                 * closing half second of every track. */
                if (Audio_QueuedFrames() == 0) s_finished = true;
            } else if (Audio_FreeFrames() >= s_maxOutPerFrame) {
                /* One frame per acquisition, then straight back round the
                 * loop. Throughput is unchanged - the re-lock is free when
                 * nobody is contending - but the longest the main thread
                 * can ever wait for the decoder drops from a twelve-frame
                 * catch-up burst to a single frame. */
                if (!PumpLocked(1)) s_finished = true;
                worked = true;

            }
        }
        DEC_UNLOCK();

        /* Park when idle instead of spinning. Sleeping also releases the
         * CPU on the rare occasion the main thread is waiting on the lock,
         * and 2ms is far shorter than the ~500ms the ring holds, so the
         * buffer cannot drain during a nap. */
        if (!worked)
            threadSleep(2000);
    }

    return 0;   /* not reached: the worker lives as long as the program */
}

void Mp3Decoder_StartWorker(void)
{
    if (s_workerRunning) return;
    s_workerRunning = true;

    /* Mutex is a plain zero-initialised object in calico; the static
     * above is already in that state. */

    /* Starts below the main thread; DecodeWorker raises and lowers itself
     * from there according to how much audio is left in the ring. */
    threadPrepare(&s_worker, DecodeWorker, NULL,
                  &s_workerStack[sizeof(s_workerStack)], WORKER_PRIO_IDLE);
    threadStart(&s_worker);

    /* And the cover fetcher, below everything, started here so there is
     * one place that brings the decoder's threads up. */
    if (!s_artRunning) {
        s_artRunning = true;
        threadPrepare(&s_artThread, ArtWorker, NULL,
                      &s_artStack[sizeof(s_artStack)],
                      s_artPace == 0 ? ART_PRIO_PATIENT : ART_PRIO_ACTIVE);
        threadStart(&s_artThread);
    }
}
#else
void Mp3Decoder_StartWorker(void) {}
#endif

/* What the user chose, which is a ceiling rather than an instruction.
 *
 * The resampler is about a fifth of the total cost of playing anything -
 * measured at 23% for AAC, 20% for Vorbis, 22% for MP3, comparing two
 * taps against twenty-four - and that fifth is the difference between a
 * format that keeps up on this CPU and one that does not. So when the
 * decoder cannot keep up the filter comes down a step, and when it has
 * room again it goes back up, never past what was asked for. */
static void ApplyResampleMode(int mode)
{
    unsigned wantTaps;
    float wantRolloff;

    /* Six phase bits for the cheap modes, eight for the rest. The bank is
     * phases * taps * 2 bytes, so this is what actually makes Fast and
     * Lite small enough to share a 4KB cache with the decoder - 1024 and
     * 512 bytes against 4096 - and it costs noise rather than aliasing. */
    unsigned wantBits = (mode == RESAMPLE_RAW || mode == RESAMPLE_LITE ||
                         mode == RESAMPLE_FAST) ? 6 : RES_PHASE_BITS;

    switch (mode) {
        /* Eight taps only earns its place with the passband pulled well
         * in. At the usual 0.90 it rejects a 20kHz tone by just 17dB,
         * which is worse than anything else on offer for a saving under
         * one percent - the wrong trade in both directions. At 0.60 the
         * same eight taps reach -34dB, better than the twelve-tap default
         * manages, and cost less. What it gives up is treble: everything
         * above about 10kHz starts coming down. */
        /* Rolloff is meaningless for the linear path - it builds no
         * coefficients - but it still has to differ from every other
         * mode's pair, or SetResampleMode's "nothing changed" early-out
         * would decline to switch between two modes that share taps. */
        /* Nothing at all: the nearest sample, no arithmetic. Its rolloff
         * has to differ from every other mode's for the same reason the
         * linear path's does - the early-out below compares the pair. */
        case RESAMPLE_NONE:  wantTaps = RES_TAPS_NONE; wantRolloff = -1.0f; break;
        case RESAMPLE_RAW:   wantTaps = RES_TAPS_RAW;  wantRolloff = 0.0f;  break;

        /* Four taps cannot be made steep, so the passband comes in
         * further still than Fast's - the same trade as the comment
         * above, one step harder. */
        case RESAMPLE_LITE:  wantTaps = RES_TAPS_LITE; wantRolloff = 0.50f; break;
        case RESAMPLE_FAST:  wantTaps = RES_TAPS_FAST; wantRolloff = 0.60f; break;
        case RESAMPLE_CLEAN: wantTaps = RES_TAPS_LOW;  wantRolloff = 0.70f; break;
        case RESAMPLE_BEST:  wantTaps = RES_TAPS_MAX;  wantRolloff = 0.90f; break;
        default:             wantTaps = RES_TAPS_LOW;  wantRolloff = 0.90f; break;
    }

    /* Recorded before the early-out below, which compares the filter and
     * not the mode. */
    /* The reported mode follows the ceiling, not this - what is asked for
     * is what Setup should keep showing, even while the filter has been
     * stepped down under it. */
    s_resampleMode = (mode >= 0 && mode < RESAMPLE_MODES) ? mode : RESAMPLE_WIDE;

    if (wantBits > RES_PHASE_BITS) wantBits = RES_PHASE_BITS;

    /* Everything past here is the filter itself, so it runs under the
     * lock - the same lock, around the same two calls, that
     * Mp3Decoder_RateChanged and Mp3Decoder_SetUpsampleGuard already
     * take. This one did not, and it is the one that changes the shape
     * of the bank rather than just its contents.
     *
     * The pump indexes the bank with the live s_phaseBits, s_phases and
     * tap count, while which of the two buffers s_fir points at is only
     * decided later, inside BuildFirTable. So between publishing the new
     * numbers and repointing the pointer the two disagreed, and a worker
     * that preempted in that window read the bank with the new geometry
     * out of the old buffer: stepping up from Fast to Best, that is 256
     * phases of 24 taps indexed into the 2KB DTCM buffer, ten kilobytes
     * past its end and into the same DTCM the main thread's stack lives
     * in, used as filter coefficients. Nothing is written there, so
     * nothing is corrupted - it comes out as noise, at whatever the
     * volume happens to be.
     *
     * And the worker is not a background thread at the moment it
     * matters: it boosts above the UI whenever the ring runs low, which
     * is exactly when somebody is on the Diagnostics page changing the
     * resampler.
     *
     * The early-out is inside too: it compares s_phases, which is the
     * shared state this is protecting.
     *
     * Not called with the lock held. Its one caller is the public
     * setter, and calico's mutex is not recursive. */
    DEC_LOCK();

    if (wantTaps == s_tapsWanted && wantRolloff == s_rolloffWanted &&
        (1u << wantBits) == s_phases) { DEC_UNLOCK(); return; }

    s_tapsWanted = wantTaps;
    s_rolloffWanted = wantRolloff;
    s_taps = wantTaps;
    s_rolloff = wantRolloff;
    s_phaseBits = wantBits;
    s_phases = 1u << wantBits;

    /* Force a rebuild, and restart the filter history: the window length
     * just changed, so the samples held over from the old one no longer
     * line up with it. */
    s_firRate = 0;
    if (s_opened) {
        ConfigureResampler(s_sampleRate ? s_sampleRate : Audio_OutputRate());
        ResetResampler();
    }

    DEC_UNLOCK();
}

void Mp3Decoder_SetResampleMode(int mode)
{
    ApplyResampleMode(mode);
}

void Mp3Decoder_SetHighQuality(bool high)
{
    Mp3Decoder_SetResampleMode(high ? RESAMPLE_BEST : RESAMPLE_WIDE);
}

int Mp3Decoder_ResampleMode(void)
{
    return s_resampleMode;
}

/* Applies the expander in place over `frames` interleaved stereo frames.
 *
 * It works on the high band only, and that is the whole design rather
 * than a refinement of it.
 *
 * A plain downward expander decides by level: below a threshold, turn it
 * down. But hiss and a quiet passage of music are both quiet, so it has
 * no way to tell them apart - and the measurements said so plainly. Past
 * the middle of its range it was pulling a quiet 440Hz tone down almost
 * as hard as the noise, thirty-four decibels at the top, which does not
 * remove hiss from music so much as remove the music.
 *
 * Frequency is the thing that separates them. Tape hiss and encoder
 * noise sit high; the body of almost any music sits low and mid. So the
 * signal is split, the expander is given only the high half, and the low
 * half passes through untouched. A quiet bass line or a sustained note
 * keeps every decibel it had, while hiss - which is nearly all high band
 * - gets ducked. This is what analogue noise reduction did for the same
 * reason, and it costs one pole per channel. */
void Mp3Decoder_Prefill(void)
{
    if (!s_opened) return;

    /* A quarter of the ring is ~125ms - enough that the mixer is never
     * chasing the decoder from a standing start, but short enough that
     * the pause before a track begins is not noticeable. The guard stops
     * this spinning on a track shorter than the target. */
    const uint32_t target = PREFILL_FRAMES;

#ifdef ARM9
    /* Waited for, not done here - see Mp3Decoder_PumpIfIdle for why the
     * main thread must not run a codec.
     *
     * Slept rather than yielded. The worker sits below this thread in
     * priority and calico only round-robins between equals, so a yield
     * hands control straight back and spins without the worker ever
     * running. A millisecond at a time lets it in; the guard caps the
     * wait at four hundred of them, after which playback starts with
     * whatever there is rather than not starting at all. */
    for (int guard = 0; guard < 400; guard++) {
        if (Audio_QueuedFrames() >= target) break;
        if (s_decodeDone || s_finished) break;
        threadSleep(1000);
    }
#else
    for (int guard = 0; guard < 400; guard++) {
        if (Audio_QueuedFrames() >= target) break;
        if (!Mp3Decoder_Pump()) break;
        if (s_decodeDone) break;
    }
#endif
}

uint32_t Mp3Decoder_ElapsedMs(void)
{
    if (!s_opened) return 0;

    /* Wherever the user has pointed, for as long as the file has not
     * caught up with them.
     *
     * That means until the worker has actually performed the seek, not
     * just until the button is released. Reporting the mixer's position
     * in between means reporting the old one - so the readout jumped back
     * to where the track had been and then forward again a moment later,
     * on both screens, every time. */
    if (s_scrubbing || s_seekPending) return s_scrubTargetMs;
    return s_baseMs + (uint32_t)(((uint64_t)Audio_PlayedFrames() * 1000) / Audio_OutputRate());
}

static void ComputeTotalMs(void)
{
    if (s_hasVbrHeader && s_sampleRate) {
        s_totalMs = (uint32_t)(((uint64_t)s_vbrFrames * s_samplesPerFrame * 1000) / s_sampleRate);
        return;
    }

    if (s_bitrate == 0) { s_totalMs = 0; return; }

    uint64_t audioBytes = (uint64_t)(s_fileSize - s_dataStart);
    s_totalMs = (uint32_t)((audioBytes * 8 * 1000) / s_bitrate);
}

uint32_t Mp3Decoder_TotalMs(void)
{
    return s_opened ? s_totalMs : 0;
}

/* A WAV's length is a byte count divided by a byte rate, which is not an
 * estimate at all. */
bool     Mp3Decoder_DurationIsExact(void) { return s_hasVbrHeader || !s_isVbr; }
uint32_t Mp3Decoder_SampleRate(void)      { return s_sampleRate; }
uint32_t Mp3Decoder_BitrateKbps(void)     { return s_bitrate / 1000; }
int      Mp3Decoder_Channels(void)        { return s_channels; }
bool     Mp3Decoder_IsVbr(void)           { return s_isVbr; }

const int16_t *Mp3Decoder_LastFrameSamples(int *outFrameCount)
{
    *outFrameCount = s_lastFrameCount;
    return s_lastFrame;
}

/* Maps a time to a byte offset. With a Xing table of contents this is
 * accurate on VBR files too; without one it falls back to the first
 * frame's bitrate, which is exact for CBR and a guess otherwise. */
static long ByteOffsetForMs(uint32_t ms)
{
    uint32_t total = s_totalMs;

    if (s_hasToc && s_vbrBytes > 0 && total > 0) {
        if (ms > total) ms = total;

        /* Percentage position, interpolated between the two surrounding
         * TOC entries rather than snapped to the nearest whole percent
         * (each entry covers ~2.4s of a four minute track). */
        uint32_t permille = (uint32_t)(((uint64_t)ms * 1000) / total);
        if (permille > 999) permille = 999;

        int a = (int)(permille / 10);
        int b = (a < 99) ? a + 1 : 99;
        uint32_t frac = permille % 10;

        uint32_t va = s_toc[a];
        uint32_t vb = s_toc[b];
        uint32_t v = va + ((vb - va) * frac) / 10;

        return s_dataStart + (long)(((uint64_t)v * s_vbrBytes) / 256);
    }

    if (s_bitrate == 0) return s_dataStart;

    return s_dataStart + (long)(((uint64_t)ms * s_bitrate) / 8000);
}

/* The seek itself, with the lock already held and no refilling. */
static void SeekLocked(uint32_t ms)
{
    if (!s_opened || !s_file) return;

    uint32_t total = Mp3Decoder_TotalMs();
    if (total && ms > total) ms = total;

    long target = ByteOffsetForMs(ms);
    if (target < s_dataStart) target = s_dataStart;
    if (target > s_fileSize)  target = s_fileSize;

    /* Negative means the seek has already happened somewhere that owns
     * the handle; moving it here would undo that. */
    if (target >= 0 && fseek(s_file, target, SEEK_SET) != 0) return;

    mad_stream_finish(&s_stream);
    mad_stream_init(&s_stream);
    /* Whatever was chosen on the Test page survives this. */
    mad_stream_options(&s_stream, MadOptions());
    mad_frame_mute(&s_frame);
    mad_synth_mute(&s_synth);

    s_inputEof      = false;
    s_decodeDone    = false;
    s_prevRemaining = (size_t)-1;
    s_needRefill    = true;
    s_lastFrameCount = 0;

    ResetResampler();
    /* The filters' memory is a couple of milliseconds of the audio they
     * just saw. Carried across a seek it is a click. */
    s_lp1L = s_lp1R = s_lp2L = s_lp2R = 0;
    s_cutL = s_cutR = 0;
    Audio_Reset();
    s_baseMs = ms;
    s_finished = false;
}

void Mp3Decoder_SeekToMs(uint32_t ms)
{
    /* While a scrub is running this only records where the user is
     * pointing - the file is not touched at all.
     *
     * Repositioning means an fseek, and on FAT that walks the cluster
     * chain from wherever it is to wherever it needs to be. Doing that
     * ten times a second to land on positions the user is about to leave
     * was the last real cost in a scrub, and by far the largest: every
     * other part of a seek is arithmetic and a few resets. The commit
     * happens once, when they let go. */
    if (s_scrubbing) {
        uint32_t total = Mp3Decoder_TotalMs();
        if (total && ms > total) ms = total;
        s_scrubTargetMs = ms;
        return;
    }

    DEC_LOCK();
    s_finished = false;
    if (!s_opened || !s_file) { DEC_UNLOCK(); return; }

    /* The one implementation, not a second copy of it.
     *
     * This used to repeat SeekLocked's body inline - the clamp, the byte
     * offset, the fseek, the four mad resets, the six flags. Two copies
     * of a seek is two places to teach anything new, and when a format
     * arrived whose position is a byte offset rather than an estimate,
     * only one of them learned: RunPendingSeek jumped correctly and this
     * path, which is the one every ordinary seek takes, did not. */
    SeekLocked(ms);

    DEC_UNLOCK();

    /* Outside the lock, and it has to be: Prefill drives Mp3Decoder_Pump,
     * which takes the same lock, and calico's Mutex is not recursive - a
     * seek would have deadlocked the moment it got here.
     *
     * Skipped entirely while scrubbing. One prefill is up to twelve MPEG
     * frames, around 180ms of decoding, and a held seek asks for another
     * every 150ms - so the main thread never gets back to drawing and the
     * position readout crawls. During a scrub the worker refills instead;
     * it raises itself above the UI when the ring runs low, so it
     * recovers on its own without blocking anything. */
    /* Not after a scrub commit: SetScrubbing follows this with the
     * lighter settle instead. */
    if (!s_scrubbing && !s_committing) Mp3Decoder_Prefill();
}

bool Mp3Decoder_IsScrubbing(void) { return s_scrubbing; }

/* Performs the seek a finished scrub left behind, if there is one.
 *
 * Called by the worker on hardware and directly by the tests, so both
 * exercise the same path - including the window between the button being
 * released and the file actually moving, which is where the position
 * readout used to fall back to the old value. */
void Mp3Decoder_RunPendingSeek(void)
{
    if (!s_seekPending) return;

#ifdef ARM9
    /* Timed, because this is the one part of a seek that happens on the
     * card and the one part whose cost cannot be reasoned about from
     * here - an fseek walks the FAT chain, and how long that takes
     * depends entirely on the flashcart. */
    uint64_t t0 = tickGetCount();
#endif
    DEC_LOCK();
    SeekLocked(s_scrubTargetMs);
    s_seekPending = false;
    DEC_UNLOCK();
#ifdef ARM9
    s_seekUs = (uint32_t)(((tickGetCount() - t0) * 1000000u) / TICK_FREQ);
#endif
}

void Mp3Decoder_SetScrubbing(bool on)
{
    if (on == s_scrubbing) return;

    if (on) {
        /* Start from where playback actually is, so the first nudge moves
         * from the current position rather than from zero. */
        s_scrubTargetMs = Mp3Decoder_ElapsedMs();
    }

    s_scrubbing = on;

    /* Muted on the way in; the worker unmutes once it has refilled, so
     * playback resumes at full buffer rather than from a scrape. */
    if (on) Audio_SetMuted(true);
    /* Letting go is where the single real seek happens, followed by the
     * top-up that keeps the first moment of playback from being fed out
     * of an empty ring. */
    if (!on && s_opened) {
        /* Handed to the worker rather than done here.
         *
         * Both halves of finishing a seek - the fseek, which walks the FAT
         * chain, and decoding enough to play from - were being done on the
         * main thread while it had a screen to keep drawing. Neither has
         * to be: the worker can do both, and the output stays muted until
         * it has caught up, so what the user hears is a clean gap rather
         * than the buffer being scraped. Nothing about the seek is on the
         * drawing thread's critical path any more. */
#ifdef ARM9
        s_seekPending = true;
        s_resuming = true;
#else
        /* No worker in this build, so the caller runs it - which is also
         * what lets a test look at the state in between. */
        s_seekPending = true;
        s_resuming = true;
#endif
    }
}

void Mp3Decoder_SeekBy(int32_t deltaMs)
{
    if (!s_opened) return;

    int64_t target = (int64_t)Mp3Decoder_ElapsedMs() + deltaMs;
    if (target < 0) target = 0;

    Mp3Decoder_SeekToMs((uint32_t)target);
}
