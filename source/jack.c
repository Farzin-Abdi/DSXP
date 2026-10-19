#include "jack.h"

#if !defined(DSXP_HOSTTEST) && defined(DSXP_OWN_ARM7)
#include <nds.h>
#include <calico.h>
#include "speaker_pxi.h"
#endif

static int  s_guard;
static volatile bool s_plugged, s_unplugged;
static bool s_available, s_supported;

#if !defined(DSXP_HOSTTEST) && defined(DSXP_OWN_ARM7)

/* Once every two seconds, and only as a safety net.
 *
 * The socket no longer arrives this way: the ARM7 sends a message the
 * frame the pin moves, so acting on it costs nothing and happens within
 * a frame. Polling at three times a second used to be the only source,
 * which meant up to a third of a second of music out of the speaker
 * after the plug came out.
 *
 * What is left of it re-reads the state in case a notification is ever
 * missed, at an interval where the blocking round trip cannot matter. */
#define POLL_FRAMES 120

static int s_countdown;

void Dsxp_Arm7Wait(void);

static void Poll(void)
{
    /* Free after the first call, and it means no ordering between the
     * two subsystems that share this channel can ever hang the boot
     * again. */
    Dsxp_Arm7Wait();

    unsigned r = pxiSendAndReceive(SPK_PXI_CHANNEL, SPK_CMD_GET_JACK);

    /* Whether the console has the block at all is asked once: it cannot
     * change while the machine is running, and it is the expensive half. */
    static bool s_probed;
    if (!s_probed) {
        unsigned raw = pxiSendAndReceive(SPK_PXI_CHANNEL, SPK_CMD_DIAG_GPIO);
        s_supported = (r & JACK_TWL) || (raw != 0x0000 && raw != 0xFFFF);
        s_probed = true;
    }

    bool now = (r & JACK_PIN) != 0;

    /* The edge is recorded whether or not the pin is trusted yet: the
     * first unplug after boot is both the event worth acting on and the
     * observation that proves the pin is real.
     *
     * A pending edge from the notification is not cleared here. This runs
     * two seconds apart and the handler runs the frame the pin moves, so
     * the poll's view is the older one - overwriting with it would lose
     * exactly the event this exists to catch. */
    if (s_plugged && !now) s_unplugged = true;

    s_plugged   = now;
    s_available = s_supported;
}

void Dsxp_Arm7Wait(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    pxiWaitRemote(SPK_PXI_CHANNEL);
}

/* Runs in IRQ mode - see PxiHandlerFn - so it does the least possible:
 * records the new state and lets the frame loop act on it. Pausing
 * playback from here would mean touching the decoder and the mixer from
 * an interrupt. */
static void OnJackChanged(void *user, u32 data)
{
    (void)user;
    if ((data & 0xFF) != SPK_NOTIFY_JACK) return;

    bool now = ((data >> 8) & 1) != 0;
    if (s_plugged && !now) s_unplugged = true;
    s_plugged = now;
}

void Jack_Init(void)
{
    Dsxp_Arm7Wait();
    pxiSetHandler(SPK_NOTIFY_CHANNEL, OnJackChanged, NULL);
    Poll();
}

void Jack_MixerMute(bool muted)
{
    Dsxp_Arm7Wait();
    pxiSendAndReceive(SPK_PXI_CHANNEL,
                      SPK_CMD_MIXER_MUTE | SPK_ARG(muted ? 0 : 1));
}

void Jack_Tick(bool busy)
{
    if (s_guard == JACK_GUARD_OFF) return;   /* nothing acts on the answer */
    if (busy) return;                        /* the decoder needs the frame */

    if (--s_countdown > 0) return;
    s_countdown = POLL_FRAMES;
    Poll();
}


unsigned Jack_ReadReg(int bus, unsigned reg)
{
    Dsxp_Arm7Wait();
    return pxiSendAndReceive(SPK_PXI_CHANNEL,
                             SPK_CMD_READ_REG |
                             SPK_ARG(((unsigned)bus << 8) | (reg & 0xFF)));
}

const char *Jack_StatusText(void)
{
    if (!s_supported) return "not available (DSi only)";
    return s_plugged ? "IN" : "OUT";
}

#else

/* No ARM7 of ours to talk to - either this is a host test, or the ROM is
 * linked against calico's stock core, which never claims the PXI channel
 * Jack_Init would otherwise wait on for ever. */
void Dsxp_Arm7Wait(void) {}
void Jack_Init(void) {}
void Jack_Tick(bool busy) { (void)busy; }
void Jack_MixerMute(bool m) { (void)m; }
unsigned Jack_ReadReg(int b, unsigned r) { (void)b; (void)r; return 0xFF; }
const char *Jack_StatusText(void) { return "not available (DSi only)"; }

#endif

void Jack_SetGuard(int mode)
{
    if (mode < 0 || mode >= JACK_GUARD_MODES) mode = JACK_GUARD_OFF;
    if (!s_supported) mode = JACK_GUARD_OFF;   /* nothing to guard with */
    s_guard = mode;
    s_unplugged = false;      /* an edge belongs to the mode that saw it */

#if !defined(DSXP_HOSTTEST) && defined(DSXP_OWN_ARM7)
    /* Let the other CPU cut the sound itself when the socket opens. It
     * sees the pin a round trip earlier than anything here can. */
    Dsxp_Arm7Wait();
    pxiSendAndReceive(SPK_PXI_CHANNEL, SPK_CMD_JACK_AUTOMUTE
                      | SPK_ARG(mode == JACK_GUARD_PAUSE ? 1 : 0));
#endif
}

int  Jack_Guard(void)     { return s_guard; }
bool Jack_Plugged(void)   { return s_plugged; }
bool Jack_Available(void) { return s_available; }
bool Jack_Supported(void) { return s_supported; }

bool Jack_TakeUnplugged(void)
{
    bool v = s_unplugged;
    s_unplugged = false;
    return v;
}
