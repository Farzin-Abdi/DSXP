#ifndef DSXP_SPEAKER_PXI_H
#define DSXP_SPEAKER_PXI_H

/* The wire format between the two CPUs, shared by both so the command
 * numbers cannot drift apart.
 *
 * PxiChannel_User0 is the channel calico reserves for applications. The
 * immediate is 26 bits (pxiMakePacket shifts it up by six to make room
 * for the channel and direction), which is why the argument sits at bit
 * 8 rather than anywhere higher. */
#define SPK_PXI_CHANNEL   PxiChannel_User0

/* A second channel, ARM7 to ARM9 only, for the socket changing state.
 *
 * Polling cannot serve this. The interval sets the delay before the music
 * stops, and at three polls a second that is up to a third of a second of
 * audio out of the speaker after the headphones come out - audible, and
 * exactly what the feature exists to prevent. Polling fast enough to fix
 * it means fifteen or more blocking round trips a second, which is what
 * made the visualizer stutter.
 *
 * So the CPU that can see the pin every frame says when it changes, and
 * the other one stops doing arithmetic about it. Idle cost is nothing;
 * latency is one frame. */
#define SPK_NOTIFY_CHANNEL PxiChannel_User1
#define SPK_NOTIFY_JACK    0x01   /* low bit carries the new pin state */

/* Silence the DS mixer itself, on the CPU that owns it.
 *
 * Pausing on the ARM9 only stops new audio being handed over; whatever
 * has already reached the mixer plays out regardless, which is the few
 * milliseconds that still escaped through the speaker after the plug came
 * out. REG_SOUNDCNTVOL is the hardware master volume - writing zero stops
 * sound coming out of the DAC that instant, buffered or not.
 *
 * Also used at startup, where the mixer would otherwise briefly play
 * whatever the wave buffer contained before the first callback filled it.
 *
 * arg 0 silences, 1 restores. */
#define SPK_CMD_MIXER_MUTE 0x14

/* Whether the ARM7 should silence the mixer by itself the instant the
 * socket opens, without waiting to be told. That is the only way to get
 * the delay to nothing: a round trip cannot beat the CPU that saw it. */
#define SPK_CMD_JACK_AUTOMUTE 0x15


/* Read one hardware register, so a change can be watched rather than
 * guessed at.
 *
 * Every fact about this console's power hardware has had to be measured -
 * which PMIC bits gate the speaker, whether the headphone pin is wired,
 * whether the lamp bits reach a DSi's backlight - and each time the
 * quickest answer was to watch the register while the state changed. This
 * makes that a thing the player can do instead of a thing that needs a
 * new build.
 *
 * The argument is (bus << 8) | reg: bus 0 is the PMIC over SPI, bus 1 is
 * the MCU over I2C. The reply is the byte, or 0xFF00 if the bus is not
 * one this console has. */
#define SPK_CMD_READ_REG  0x17

#define SPK_BUS_PMIC      0
#define SPK_BUS_MCU       1

#define SPK_CMD_MASK      0xFF
#define SPK_ARG_SHIFT     8
#define SPK_ARG_MASK      0x3FFF00

/* Builds the argument half of a message. The immediate is 26 bits - the
 * channel and direction occupy the low six - so an argument lives at bit
 * 8 and has 14 bits, which is ample for a field index. */
#define SPK_ARG(x)        (((unsigned)(x) << SPK_ARG_SHIFT) & SPK_ARG_MASK)

/* The PMIC's speaker-mute bits are deliberately absent.
 *
 * libnds documents PM_SOUND_MUTE as "mute the main speakers, headphone
 * output will still work". Measured on hardware, the mute bit does
 * nothing at all and the enable bit gates the whole analog path - speaker
 * and jack together:
 *
 *   enable=1 mute=0   speaker plays,  jack plays
 *   enable=1 mute=1   speaker plays,  jack plays
 *   enable=0 mute=1   speaker silent, jack SILENT
 *   enable=0 mute=0   speaker silent, jack SILENT
 *
 * There is no speaker-only mute on this hardware, which is why
 * headphones-only is implemented by detecting the socket and pausing.
 * The code that drove those bits was removed once it was proved useless;
 * the measurements are kept so nobody writes it again.
 */

/* The headphone socket, on DSi only.
 *
 * A DSi has a pin wired to the socket - GPIO33[1], `T5` in calico's map,
 * documented there as "headphone connection state". It is a real signal,
 * not an inference, and it is the thing that made every previous attempt
 * at this unnecessary. It is also DSi-only and ARM7-only: those GPIOs
 * were added by the DSi and live at 0x04004C00, in ARM7 I/O space.
 *
 * Reply:
 *   bit 0  the pin, as read
 *   bit 1  TWL mode - if clear, the register is not ours to read and
 *          bit 0 means nothing
 *   bit 2  set once the pin has been seen in both states, which is the
 *          only proof that it is wired to anything at all */
#define SPK_CMD_GET_JACK  0x03

#define JACK_PIN          (1u<<0)
#define JACK_TWL          (1u<<1)
#define JACK_PROVEN       (1u<<2)

/* One raw read, used once at startup to decide whether this console has
 * the GPIO block at all. */
#define SPK_CMD_DIAG_GPIO   0x11

#endif /* DSXP_SPEAKER_PXI_H */
