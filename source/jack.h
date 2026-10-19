#ifndef DSXP_JACK_H
#define DSXP_JACK_H

#include <stdbool.h>
#include <stdint.h>

/* The headphone socket, on consoles that have a wire to it.
 *
 * A DSi does: GPIO33[1], which calico's pin map documents as "headphone
 * connection state". Measured with the socket in and out, only bit 5 of
 * GPIO_CNT moves (80D7 -> 80F7), and high means connected - confirmed on
 * hardware rather than assumed.
 *
 * A DS Phat or DS Lite has no such wire, and none can be conjured. That
 * was settled four ways: a scan of every ARM7 I/O register found the pin
 * on a DSi and nothing on a DS Lite; GBATEK accounts for every other
 * candidate as a thermometer or a microphone; no DS homebrew implements
 * it; and a hardware modder had to solder to the socket switch because no
 * signal reaches either CPU.
 *
 * The pin is in ARM7 I/O space, so all of this needs DSXP's own ARM7 core
 * (build with DSXP_OWN_ARM7=1). Without it every call here is a stub and
 * the setting reports itself unavailable - importantly including
 * Jack_Init, which would otherwise wait forever on a PXI channel that
 * calico's stock core never opens. */

typedef enum {
    JACK_GUARD_OFF = 0,   /* ignore the socket                  */
    JACK_GUARD_PAUSE,     /* pause when the headphones come out */
    JACK_GUARD_MODES,
} JackGuard;

/* Blocks until DSXP's ARM7 has claimed the shared PXI channel, once.
 *
 * Everything that talks to that core must call this before its first
 * message. Sending into a mailbox the other side has not prepared yet
 * blocks for ever waiting for a reply - which is not a feature failing
 * quietly, it is the console never finishing boot. That shipped once,
 * when the battery read was initialised one line before the wait.
 *
 * Idempotent, and a no-op when the ROM is linked against calico's stock
 * core, which never claims the channel at all. */
void Dsxp_Arm7Wait(void);

void Jack_Init(void);

/* Opens or silences the DS mixer itself, on the ARM7.
 *
 * Distinct from pausing: pausing stops new audio being handed over, while
 * this stops the DAC. Audio already inside the mixer plays out through a
 * pause and does not survive this. */
void Jack_MixerMute(bool muted);


/* One hardware register, for the diagnostics page. bus is SPK_BUS_PMIC
 * or SPK_BUS_MCU. */
unsigned Jack_ReadReg(int bus, unsigned reg);

/* Poll. A blocking round trip to the other CPU, so it is skipped
 * entirely when no guard is set, skipped again while `busy` - pass what
 * the frame loop knows about the decoder being behind - and otherwise
 * rate-limited to a few times a second. Call it every frame. */
void Jack_Tick(bool busy);

void Jack_SetGuard(int mode);
int  Jack_Guard(void);

bool Jack_Plugged(void);

/* Set for one frame when the headphones are pulled out. */
bool Jack_TakeUnplugged(void);

/* Whether this console has the hardware, and whether the guard should
 * act. Support is decided by what the register reads - an unmapped
 * address reads all-zeros or all-ones, a real GPIO block reads neither -
 * and never by what mode the console reports: a DS-mode ROM on a
 * flashcart says NTR mode even on a DSi, and gating on that is what kept
 * this feature dead for three builds. */
bool Jack_Supported(void);
bool Jack_Available(void);

/* A short live description for the settings caption. */
const char *Jack_StatusText(void);

#endif /* DSXP_JACK_H */
