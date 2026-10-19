/* DSXP's ARM7 core.
 *
 * It exists to read one pin. On a DSi the headphone socket is wired to
 * GPIO33[1] - `T5` in calico's pin map, "headphone connection state" -
 * and that register lives at 0x04004C00, in ARM7 I/O space, where the
 * ARM9 cannot reach it. Everything the player does with headphones goes
 * through the PXI server below.
 *
 * MEASURED, with Test -> Hardware readout open:
 *
 *   headphones out   GPIO_CNT = 80D7   bit 5 low
 *   headphones in    GPIO_CNT = 80F7   bit 5 high
 *
 * Nothing else in the register moved.
 *
 * The PMIC sound bits were tried and removed. The measurements are kept
 * so nobody writes that code again:
 *
 *   enable=1 mute=0  (calico's "on")    speaker plays,  jack plays
 *   enable=1 mute=1  (what libnds says) speaker plays,  jack plays
 *   enable=0 mute=1  (calico's "off")   speaker silent, jack SILENT
 *   enable=0 mute=0                     speaker silent, jack SILENT
 *
 * libnds documents bit 1 as "mute the main speakers, headphone output
 * will still work". It does nothing, and bit 0 gates the whole analog
 * path. There is no speaker-only mute on this hardware - which is why
 * headphones-only is implemented by detecting the socket and pausing,
 * not by muting anything.
 *
 * Everything below main() is devkitPro's `templates/combined` ARM7,
 * reproduced call for call and in the same order, plus one server thread.
 * That is deliberate and it is the whole safety argument: the audio path
 * must not regress, and the way to keep it identical is to change nothing
 * about how it is brought up.
 *
 * To go back to calico's stock core: comment out the two _ARM7_ELF lines
 * in the top-level Makefile and drop `arm7` from the $(BUILD)
 * prerequisites. The ARM9 half still builds; headphone detection reports
 * unavailable and its setting says so.
 */
#include <calico.h>
#include <nds.h>
#include <maxmod7.h>

#include "../../source/speaker_pxi.h"

/* Management structure and stack space for the PXI server thread. */
static Thread s_serverThread;
alignas(8) static u8 s_serverStack[1024];

static u8 s_jackSeen;

/* The mixer starts silent and the ARM9 opens it once real audio is
 * flowing, so the wave buffer's initial contents never reach the DAC. */
static bool s_mixerMuted = true;
static bool s_jackAutoMute;


/* The PMIC registers the diagnostics page watches, sampled one per frame
 * and handed back from here.
 *
 * The MCU is deliberately absent. A DSi keeps its backlight level there,
 * over I2C, and reading it from a DS-mode ROM is the transaction this
 * file already warns about - it does not return, and it took the console
 * with it when the register page tried. SPI is the bus this hardware has
 * in every model. */
static const u8 kPmicRegs[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x10 };
#define PMIC_REG_COUNT ((unsigned)(sizeof(kPmicRegs) / sizeof(kPmicRegs[0])))
static u8 s_pmicCache[PMIC_REG_COUNT];

static void SamplePmic(void)
{
    static unsigned at = 0;
    s_pmicCache[at] = pmicReadRegister((PmicRegister)kPmicRegs[at]);
    if (++at >= PMIC_REG_COUNT) at = 0;
}



/* Sampled every VBlank rather than only when asked, so a plug and unplug
 * between two glances at the screen still registers. */
static u16 s_gpioLast, s_gpioSeen0, s_gpioSeen1, s_gpioEdges;
static bool s_gpioPrimed;

static void SampleGpio(void)
{
    /* Stops sampling on hardware that has no such register.
     *
     * GPIO_CNT was added by the DSi; on a DS or DS Lite the address is
     * unmapped and reads open bus. Harmless, but it is sixty reads a
     * second for a console that can never answer, and this whole binary
     * already exists for a feature only a DSi has - the least it can do
     * is stop charging the others for it.
     *
     * The test is what the register reads, not what mode the console
     * reports. Gating on systemIsTwlMode() is what kept the headphone
     * feature dead for three builds: a DS-mode ROM on a flashcart says
     * NTR mode even on a DSi, while that register reads perfectly. */
    static int s_probeState;        /* 0 unknown, 1 present, 2 absent */
    if (s_probeState == 2) return;

    u16 v = REG_GPIO_CNT;

    if (s_probeState == 0) {
        if (v == 0x0000 || v == 0xFFFF) { s_probeState = 2; return; }
        s_probeState = 1;
    }

    if (s_gpioPrimed && v != s_gpioLast) s_gpioEdges++;
    s_gpioPrimed = true;
    s_gpioLast = v;
    s_gpioSeen0 |= (u16)~v;          /* pins observed low  */
    s_gpioSeen1 |= v;                /* pins observed high */

    /* Tracked here rather than in the query, so it accumulates from boot
     * whether or not anyone is asking. Otherwise the very first unplug -
     * which is both the event to act on and the thing that proves the pin
     * - would arrive before the pin was trusted, and be discarded. */
    s_jackSeen |= (v & GPIO_CNT_PIN(GPIO_PIN_HEADPHONE_CONNECT)) ? 2 : 1;

    /* And tell the other CPU the moment it moves, rather than waiting to
     * be asked. This is what makes the music stop when the plug comes out
     * instead of a third of a second later. */
    {
        static int last = -1;
        int now = (v & GPIO_CNT_PIN(GPIO_PIN_HEADPHONE_CONNECT)) ? 1 : 0;
        if (now != last) {
            /* Silence first, tell second. A round trip cannot beat the
             * CPU that just saw the pin, and the point of this feature is
             * that nothing comes out of the speaker at all. */
            if (!now && s_jackAutoMute && !s_mixerMuted) {
                soundSetMixerVolume(0);
                s_mixerMuted = true;
            }
            last = now;
            pxiSend(SPK_NOTIFY_CHANNEL, SPK_NOTIFY_JACK | (now << 8));
        }
    }
}

static int ServerThreadMain(void *arg)
{
    (void)arg;

    Mailbox mb;
    u32 slots[4];
    mailboxPrepare(&mb, slots, sizeof(slots) / 4);
    pxiSetMailbox(SPK_PXI_CHANNEL, &mb);

    for (;;) {
        u32 msg = mailboxRecv(&mb);
        u32 retval = 0;

        switch (msg & SPK_CMD_MASK) {
            case SPK_CMD_GET_JACK: {
                /* Deliberately NOT gated on systemIsTwlMode().
                 *
                 * It was, and that alone stopped this working. A DS-mode
                 * ROM launched from a flashcart reports NTR mode even on
                 * a DSi, so the reply was zero and the feature sat inert
                 * - while the readout page, which samples the register
                 * unconditionally, showed it changing perfectly. The
                 * register was readable the whole time; the gate was
                 * asking a question whose answer did not matter.
                 *
                 * What matters is whether the pin moves. s_jackSeen
                 * records both states from boot, and a pin observed high
                 * and low is a pin that is genuinely wired to something -
                 * which is true regardless of what mode the console
                 * thinks it is in, and false on hardware that has no such
                 * wire at all. */
                if (s_gpioLast & GPIO_CNT_PIN(GPIO_PIN_HEADPHONE_CONNECT))
                    retval |= JACK_PIN;
                if (systemIsTwlMode()) retval |= JACK_TWL;
                if (s_jackSeen == 3)   retval |= JACK_PROVEN;
                break;
            }

            case SPK_CMD_DIAG_GPIO:   retval = s_gpioLast; break;

            case SPK_CMD_MIXER_MUTE: {
                bool on = ((msg & SPK_ARG_MASK) >> SPK_ARG_SHIFT) != 0;
                s_mixerMuted = !on;
                soundSetMixerVolume(on ? 0x7F : 0);
                break;
            }

            case SPK_CMD_JACK_AUTOMUTE:
                s_jackAutoMute = ((msg & SPK_ARG_MASK) >> SPK_ARG_SHIFT) != 0;
                break;

            case SPK_CMD_READ_REG: {
                /* From the cache, never off the bus - see the note below.
                 * A read done here blocks the ARM9 until it returns, and
                 * a DS-mode ROM reaching for DSi I2C is exactly the read
                 * that does not. That hung the console. */
                unsigned arg = (msg & SPK_ARG_MASK) >> SPK_ARG_SHIFT;
                unsigned reg = arg & 0xFF;

                retval = 0xFF;
                for (unsigned i = 0; i < PMIC_REG_COUNT; i++)
                    if (kPmicRegs[i] == reg) { retval = s_pmicCache[i]; break; }
                break;
            }


            /* Handed back from a cache, never read here.
             *
             * The bus transaction happens on this CPU's own loop instead.
             * Doing it inside the handler meant the ARM9 sat blocked in
             * pxiSendAndReceive for as long as the read took - and if the
             * read never returned, which is what a DS-mode ROM reaching
             * for DSi I2C can do, the console never finished booting.
             *
             * A reply that is merely stale cannot hang anyone. */


            default:
                break;
        }

        pxiReply(SPK_PXI_CHANNEL, retval);
    }

    return 0;
}

int main(void)
{
    // Read settings from NVRAM
    envReadNvramSettings();

    // Set up extended keypad server (X/Y/hinge)
    keypadStartExtServer();

    // Configure and enable VBlank interrupt
    lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
    irqEnable(IRQ_VBLANK);

    // Set up RTC
    rtcInit();
    rtcSyncTime();

    // Initialize power management
    pmInit();

    // Set up block device peripherals
    blkInit();

    // Set up touch screen driver
    touchInit();
    touchStartServer(80, MAIN_THREAD_PRIO);

    // Set up sound and mic driver
    soundStartServer(MAIN_THREAD_PRIO-0x10);

    /* Silent until the ARM9 says otherwise. The mixer is enabled here but
     * has nothing to play yet, and whatever the wave buffer happens to
     * contain would otherwise be audible for the moment before the first
     * stream callback fills it. */
    soundSetMixerVolume(0);
    micStartServer(MAIN_THREAD_PRIO-0x18);

    // Set up wireless manager
    wlmgrStartServer(MAIN_THREAD_PRIO-8);

    // Set up Maxmod
    mmInstall(MAIN_THREAD_PRIO+1);

    /* The one addition. Lowest priority of anything here: a mute is a
     * user preference arriving a frame late at worst, and it must never
     * be in front of the mixer. */
    threadPrepare(&s_serverThread, ServerThreadMain, NULL,
                  &s_serverStack[sizeof(s_serverStack)], MAIN_THREAD_PRIO);
    threadStart(&s_serverThread);

    // Keep the ARM7 mostly idle
    while (pmMainLoop()) {
        threadWaitForVBlank();
        SampleGpio();
        SamplePmic();

        /* The battery, about once a second, and on this thread on
         * purpose: the PXI server answers from the cache above, so a slow
         * or unanswered bus transaction delays a number on a settings
         * page instead of stalling the other CPU. */
    }

    return 0;
}
