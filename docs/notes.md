# DSXP — implementation notes

Why the player is built the way it is: the decode path, the
screen, the hardware it talks to, and the measurements behind the
choices. None of this is needed to build or use DSXP — see the
[README](../README.md) for that.
### DS mode and DSi mode

DSXP asks for the DSi's 134MHz clock at start-up and reports which one it
got on the Test page. It matters more than anything else in this
program: an ARM9 at 67MHz is what a DS mode flashcart gives you, and
every decode budget doubles at 134.

A DS mode flashcart runs everything at 67MHz, and 320 kbps stereo MP3 is
the case that leaves the least headroom there — close enough to the edge
that the decoder carries machinery for it: a worker that raises its own
priority when the buffer runs low, and a relief path that halves the
sample rate rather than let the sound break up. At 134MHz none of that
has to fire.

To get it, DSXP has to be launched in DSi mode with SCFG unlocked -
Unlaunch, or TWiLight Menu++ from the SD card. A DS mode flashcart runs
the console at 67MHz and nothing in software can change that.

### Dependencies

1. **devkitARM, libnds, libfat, maxmod** via devkitPro's pacman:
   ```
   sudo dkp-pacman -S nds-dev
   ```
   Developed against devkitARM r68 / libnds 2.0.2 / calico 1.2.0 /
   maxmod 2.1.0.

2. **libmad**, which is not in devkitPro's portlibs, so
   `tools/build_libmad.sh` fetches 0.15.1b and cross-compiles it into
   `libs/libmad/`. It deliberately skips libmad's own `./configure`,
   which is old enough to pass `gcc` flags that no longer exist, and just
   compiles the eleven source files directly with `FPM_ARM` — the
   `smull`-based fixed-point path, built in ARM mode because that inline
   assembly will not assemble inside a Thumb-1 function.

   > **Licensing:** libmad is **GPL-2.0-or-later**, not LGPL. Linking it
   > makes a redistributed `dsxp.nds` GPL too. (An earlier version of
   > this README got that wrong.)

## Architecture

Almost everything is on the **ARM9**: MP3 decoding, the UI and the audio
stream callback. The ARM7 runs `arm7/`, which is devkitPro's stock core
reproduced call for call plus one PXI server — see *Headphones* below for
the one thing that needs it. maxmod's ARM7 half does the actual mixing, as
it would under the stock core.

```
ARM9 (source/)                          ARM7 (arm7/)
  libfat file browser                     maxmod mixer
  libmad decode ──► resample to 32768Hz     ▲
                    │                       │
                    ▼                       │
              ring buffer  ◄── maxmod stream callback
                                (runs on a maxmod-owned thread)
  skin + visualizer, drawn in software
```

Decoding runs through libmad's **low-level frame API**
(`mad_frame_decode`/`mad_synth_frame`) rather than `mad_decoder_run()`,
which decodes to completion in one call. `Mp3Decoder_Pump()` decodes a
bounded number of frames and returns, so the main loop keeps servicing
input and redrawing between decode bursts instead of stalling for the
length of a song.

Output is **resampled to 32768Hz**, which is not a style choice: that is
the ceiling maxmod documents for `mm_stream.sampling_rate`. Handing it
44100 does not get you 44100, it gets you a track playing at the wrong
speed.

Because that is a *downsample*, the resampler is a polyphase
windowed-sinc FIR (24 taps, 256 phases) rather than the linear
interpolation it started as. Linear interpolation is a poor
anti-aliasing filter, and everything in the source above the output's
16384Hz Nyquist folds back into the audible band instead of being
removed - a cymbal at 20kHz reappears at 12.7kHz as a whistle, which is
exactly the kind of small persistent noise that sits on top of otherwise
clean playback. Measured with `make test`:

| | linear | 24-tap FIR |
|---|---|---|
| 20kHz alias rejection | −4.6 dB | **−73.3 dB** |
| 10kHz passband | — | −0.01 dB |

The tap count was picked by sweeping it against those two numbers. The
inner multiply-accumulate loop is compiled in ARM mode rather than the
project's usual Thumb — GCC then emits `smlabb`, the ARM9E single-cycle
16x16+32 multiply-accumulate, which is exactly the shape of this loop.

Building the filter table is the one expensive piece of start-up work,
since the ARM9 has no FPU and every `sinf`/`cosf` is a software routine.
It is kept tolerable three ways: the table is cached by (rate, taps) so an
album pays for it once; it uses single precision, which is far more
accuracy than Q14 coefficients can hold; and **only half the polyphase
bank is evaluated**. A linear-phase windowed sinc satisfies
`h[P-p][taps-1-t] == h[p][t]` exactly — the tap distance for that pair is
the precise negative, and both sinc and Blackman are even — so the other
half is a copy. `make test` asserts that identity holds to 0 LSB, because
if it ever stopped holding, half the filter would silently be wrong.

### Output headroom

A windowed sinc has unity gain at DC but **overshoots on transients**,
and 16-bit output has nowhere to put the overshoot. Measured on
full-scale input (`tools/hosttest/measure_audio.c`):

| input | overshoot, no headroom |
|---|---|
| sine | 1.00x |
| square wave | 1.19x (+1.5 dB) |
| noise-like (a loud modern master) | 1.44x (+3.2 dB) |

All of that used to run straight into the int16 clamp, so anything
mastered near 0dBFS hard-clipped *continuously* — heard as a persistent
grainy edge on loud passages rather than as an occasional tick. The
resampler now carries 2.2 dB of headroom, which keeps every realistic
transient inside full scale. The cost is 2.2 dB of maximum loudness,
which the DS's own volume slider gives back.

### What `arm7/` is

Earlier versions shipped a hand-written ARM7 that relayed input through
the `IPC` transfer region, called `installSoundFIFO()` and opened the
maxmod stream on the ARM7 side. **libnds 2.0 replaced its own ARM7 runtime
with calico**, and `IPC`, `installSoundFIFO()` and `mailReadCheck()` are
all gone; that binary could not compile, let alone run, and it was
deleted rather than resurrected.

What is there now is a different thing. `arm7/source/main.c` is
devkitPro's own `templates/combined` ARM7 — the same calls in the same
order, bringing up NVRAM, the extended keypad, the RTC, power management,
block devices, touch, sound, mic, wireless and maxmod — plus one server
thread on `PxiChannel_User0`. Nothing is reimplemented. Reproducing the
stock core exactly is the whole safety argument: the audio path must not
regress, and the way to keep it identical is to change nothing about how
it is brought up.

`ds_rules` defaults `_ARM7_ELF` to calico's prebuilt `ds7_maine.elf`; the
top-level Makefile overrides it. Commenting those two lines out and
dropping `arm7` from the `$(BUILD)` prerequisites reverts to the stock
core — the ARM9 half still builds, and headphone detection reports
unavailable.

### Start-of-track buffering

`Mp3Decoder_Prefill()` decodes ~150ms ahead before the mixer is allowed to
start, and again after every seek. Without it the stream was unpaused
against a completely empty ring — the decoder's first `Pump()` does not
run until the next main-loop iteration, so the mixer immediately asked
for a full 2048-frame buffer and got silence. That showed up on hardware
as a ~2000 frame underrun on *every* track and every seek, which is
exactly one maxmod buffer's worth.

### Press feedback

The visible pressed state of a control is folded into the repaint
comparison (`Skin_FeedbackState()`). That is not a detail: without it, the
frame on which a press flash expires is not "dirty", so nothing repaints
and the pressed graphic drawn on the previous frame simply stays on
screen — the button looks stuck down permanently. A control also stays
down for as long as the stylus rests on it rather than only flashing, and
physical buttons flash their on-screen counterpart so both input methods
respond the same way.

### Keeping playback smooth

The UI slows down *only while music is playing*, and some of that is not
a bug to be fixed. Decoding one MPEG frame costs roughly 8-15ms on a
67MHz ARM9 with no FPU, and it cannot be interrupted part way -
`mad_frame_decode` either completes or it does not. A whole video frame
is 16.7ms. So whenever a decode lands, that frame is largely spent
before any drawing happens, and roughly every other frame has one.

That sets a floor on how smooth the UI can be while playing, and the only
levers on it are decoding less often (a bigger buffer does not help;
the *average* work is fixed) or decoding more cheaply - which is what the
**Audio quality: Standard** setting does, halving the resampler's share.

Everything else was avoidable, and was fixed:

- **Decode is budgeted by the clock, not by a frame count.** A fixed
  count cannot express the actual priority: set it high and a catch-up
  burst freezes the UI for a hundred milliseconds; set it low and there is
  no headroom left to recover from a stall, which risks the one thing that
  must never break. `Mp3Decoder_Pump()` now stops once it has spent
  ~2.5ms — *unless* the output buffer has fallen under a quarter full, in
  which case **the clock is ignored entirely** and it decodes until
  playback is safe again. Audio first, UI second, decided per call. One
  frame always gets decoded regardless, so a low frame rate can never
  stall decoding.

  The "ignored entirely" part is load-bearing and was got wrong once. An
  earlier version gave the catch-up case a *larger* time budget (12ms)
  rather than none. That cannot work: the deadline can only be checked
  between frames, and one frame decode already costs about as much as the
  whole generous budget — so it bought exactly one extra frame. A ring
  that had drained could never refill, and the underrun counter climbed
  forever instead of recovering. `make test` now measures both cases: 856
  output frames produced when the buffer is healthy, 10272 when it is
  empty.
- **Five `snprintf` calls every frame** in the top overlay, which is
  redrawn every frame because the visualizer repaints under it. Every one
  of those strings changes at most once a second, so they are now rebuilt
  only when the value behind them moves.
- **`Mp3Decoder_TotalMs()` did a 64-bit divide** by a non-power-of-two and
  was called several times a frame — for a value that is constant for the
  whole track. It is computed once, when the first frame reveals the
  sample rate.

- **The bars effect stopped going through the general-purpose fill.** A
  bar row is six pixels, and drawing each one with a clipping
  general-purpose `Gfx_Fill` meant the call overhead dwarfed the work:
  with 32 bars up to 132 rows tall, roughly **five thousand calls a
  frame**. Worse, the reflection faded each row with a `Gfx_Lerp`, which
  is three integer divisions, on a CPU with **no hardware divider** —
  about **2,500 divisions every frame**. The fade depends only on how far
  below the floor a row is, so it is now a table built once at startup,
  and bar rows are written straight into the buffer as six unrolled
  stores. This was the single largest cost in the loop, and it was in the
  *default* visualizer mode.
- **The visualizer only repaints what changed** — 768 pixel writes a
  frame instead of 56,886, measured by `make test`. **74x less.**

  It used to clear all 256x192 and redraw from scratch every frame. But a
  bar only moves a few pixels at a time, and the band its top edge sweeps
  across is the *only* part that changes colour: everything below was bar
  before and is bar now, everything above was backdrop and still is. Each
  buffer remembers what it is showing, so only that band, the peak marker
  and the moved part of the reflection get painted. The backdrop and floor
  line never change at all and are painted once per buffer.

  Incremental drawing is an easy way to leave stale pixels on screen, so
  it is not taken on trust: the test runs the same input twice, once
  incrementally and once forcing a full redraw every frame, and fails
  unless the two are pixel for pixel identical.

- **The now-playing overlay redraws only when it changes.** Its plate is
  256x28 - about 7000 pixels - and it was being repainted every frame
  purely because the visualizer used to clear the whole screen underneath
  it. The visualizer's backdrop now stops below the strip, so the overlay
  owns those rows outright and can skip frames where nothing it shows has
  moved. Everything on it changes at most once a second, so that is 7000
  pixels a second instead of 7000 a frame. Once the bars became cheap this
  was about 95% of the remaining top-screen cost.

- **There is one visualizer, not three.** A waveform and a plasma effect
  were dropped rather than left switchable. Plasma alone held a 48KB
  radial-distance table permanently resident and changed every pixel every
  frame, so nothing about it could ever be skipped — and the CPU it wanted
  came out of the same budget as MP3 decoding. Removing both freed **50KB
  of RAM** and left the one effect that can be drawn almost for free.
- **The visualizer yields when the loop is behind.** It is the largest
  fixed cost in the frame — every pixel of 256x192, every frame — and it
  is pure decoration. If the previous iteration overran its budget, this
  one skips it and leaves the last image up; the effect drops to half rate
  for a moment, which is nearly invisible on a music visualisation, and
  the milliseconds go to input and decoding instead. Never twice in a row,
  because a permanently half-rate visualizer would be worse than an
  occasional dropped frame.

Three earlier fixes are still doing their share:

- **`pmGetBatteryState()` was called every frame** for the battery icon.
  It is a *blocking* cross-CPU round trip (`pxiBeginReceive`,
  `pxiSendPacket`, `pxiEndReceive`) that waits on the ARM7's power
  thread — the same CPU mixing the audio. Sixty synchronous waits a
  second is exactly the kind of intermittent contention that makes
  playback tick. It is now polled once a second, which is plenty for a
  gauge the DS only reports two levels for.
- **Volume was applied as a step.** Changing a multiplier between one
  sample and the next is a discontinuity, and a discontinuity is a click.
  The callback now eases toward the target over a few milliseconds.
- **Decode ran once per frame**, so a long repaint could open a gap.
  `Player_Update()` is now called three times per iteration — before
  drawing, after the visualizer, and again before the loop parks on
  vblank. It is a no-op when the buffer is full, so the extra calls cost
  nothing when there is nothing to do.

### Blanking the screens

Three ways, because lid-close cannot be relied on (see below):

- **Automatically**, after 30 seconds with no input while something is
  playing. Controlled by "Blank screens when idle" on the Sleep tab, on
  by default. It only triggers during playback — going dark on someone
  reading the file list would be obnoxious.
- **Manually**, with the screen-off button in the titlebar.
- **On lid close**, where the hardware reports it.

Any button or touch brings the screens back.

> **What blanking does *not* do is switch the backlight off**, and on a
> DSi neither does lid-close work at all. Both are hard limits of running
> on calico's stock ARM7 core, and both were traced rather than guessed:
>
> - `IRQ_HINGE` is declared inside `#ifdef ARM7` in calico's `irq.h` —
>   the ARM9 has no hinge interrupt. The only route left is the extended
>   key word the ARM7 publishes at `0x02FFFF6C`, which does not carry the
>   lid on a DSi.
> - The ARM9 *does* get told the lid closed — `pmMainLoop` tests bit 3 of
>   its private `s_pmState` for exactly that — but when sleep is
>   disallowed it clears the bit and discards it, and `s_pmState` is a
>   static with no accessor. The only exposed consumer is sleep, which
>   halts both CPUs and therefore stops the music.
> - The backlight sits on the ARM7's SPI bus. The stock core has
>   `pmicWriteRegister`, but its PXI dispatch only handles battery,
>   NVRAM, mic amp, sleep-allowed, power LED and reset — nothing reaches
>   the backlight register.
>
> Getting a true "lid closed, lamp off, music playing" would mean owning
> the hinge and the PMIC on the ARM7 — which `arm7/` shows how to do, and
> which is the main reason that directory was kept rather than deleted.
> Unimplemented rather than impossible.

### Headphones: detecting the socket

On a **DSi** the console has a wire to the headphone socket. calico's pin
map names it: `T5`, aka `GPIO33[1]`, *"headphone connection state"*.
Measured with the readout page open:

```
headphones out   GPIO_CNT = 80D7    bit 5 low
headphones in    GPIO_CNT = 80F7    bit 5 high
```

Nothing else in the register moves. Bit 5 is the socket and high means
connected — confirmed on hardware rather than assumed.

The pin is in ARM7 I/O space, so this needs `arm7/` — DSXP's own ARM7
core — in place of calico's prebuilt `ds7_maine.elf`. It is on by default;
`make DSXP_OWN_ARM7=0` goes back to the stock core, and the ARM9 half then
compiles out entirely (`Jack_Init` would otherwise wait for ever on a PXI
channel the stock core never opens).

**Setup → `Headphones: IN/OUT` — Off / Pause.** It pauses rather than
mutes, so the transport shows a state the user recognises. On anything
without the pin the row reads `not available (DSi only)` and shows a
single unselected segment.

> The poll is a *blocking* round trip between the CPUs, and it is easy to
> make expensive. An early version ran it every four frames regardless of
> whether the feature was switched on — thirty main-loop stalls a second,
> and the visualizer stuttered exactly as it had before any of the
> performance work. It is now skipped entirely when no guard is set,
> skipped while the decoder is behind, and otherwise runs three times a
> second.

#### DS Phat and DS Lite: confirmed impossible

Not "not implemented" — impossible, and confirmed two independent ways.

**Measured.** The register scanner (Test → Register scanner) reads every
ARM7 I/O register from `0x04000000` to `0x040005FE`, plus the DSi GPIO
block, on every frame. It reports a register only if a bit was *invariably*
one value while the user declared the headphones out and *invariably* the
other while they declared them in — so a timer counting or a button being
pressed is rejected however busily it changes.

Run on a DSi it reports `04004C00 bits 0020` — the socket pin, which is
the positive control: the method demonstrably finds a real jack signal
when one exists. Run on a DS Lite it reports **nothing**. That is what
makes the negative result evidence rather than an absence of effort.

**Documented.** GBATEK independently accounts for every candidate:

| probe | what it actually is |
|---|---|
| TSC channel 0 (`T0`) | *"Temperature 0 (requires calibration, step 2.1mV per 1'C accuracy)"* |
| TSC channel 6 (`AUX`) | *"AUX Input (connected to Microphone in the NDS)"* |
| TSC channel 2 (`VBAT`) | *"Battery Voltage (not used, connected to GND in NDS, always 000h)"* |
| `RCNT_EXT` | X, Y, debug button, stylus PENIRQ, hinge |
| DSi `GPIO_CNT` | does not exist on these consoles |

No TSC channel connects to a headphone socket, and the socket's own
contacts are a mechanical switch that makes and breaks the speaker
connection without reporting to either CPU.

**`T0` looked promising during testing and was a red herring.** It moved
while the socket was being used — because it is a thermometer and a hand
was on the console. A calibration path was built on that observation and
then removed: keyed to temperature, it would have paused the music
whenever the console warmed up. Worth recording, because the measurement
was real and the conclusion drawn from it was wrong.

#### What else did not work

Two earlier approaches failed, both because a plausible-looking document
was taken as a hardware fact:

- **`PM_SOUND_MUTE`.** libnds documents it as *"mute the main speakers,
  headphone output will still work"*. Measured, the mute bit does nothing
  and the enable bit gates the entire analog path — speaker and jack
  together. There is no headphones-only mute on this console.
- **An acoustic detector**, correlating the built-in microphone against an
  envelope of what was sent to the DAC. The maths separated synthetic
  cases cleanly and it did not work on hardware. Not in the tree.
  (Fittingly, the microphone it used is TSC channel 6 — the one channel on
  that chip which *is* connected to something audio-related.)

Two diagnostic pages did that work — a live register readout and a
brute-force scanner that reported only bits which consistently
discriminated between the two states. They found GPIO bit 5 on a DSi
without having to trust the pin map, and found nothing on a DS Lite. Both
have been removed now that the question is settled; the measurements they
produced are recorded above and in `arm7/source/main.c`.

### Why there is no noise reduction

There was one: a high-band expander with a one-pole crossover, swept by a
single slider. It measured well and it is gone, because it ran **per
sample at 32768 Hz** on a CPU with no FPU, and at 320kbps the decoder has
no cycles to lend it.

That cost did not show up as bad audio. It showed up as the file list
stuttering while scrolling — the decoder falling behind, catching up, and
the frame loop paying for it. Which is why it took so long to find: the
symptom was in the UI and the cause was in the audio path, and the two
had never been connected.

It was found by measurement rather than reasoning, on hardware, by
switching it off.

### Picking up where you left off

The playing track and its position are written to the settings file a few
times a minute, and reopened on the next run — the folder is rescanned so
next and previous work immediately, exactly as if the track had been
opened from the browser.

It resumes **paused**, on the file list rather than Now Playing. A console
that starts making noise the moment it boots is startling, and the
transport is one button away. A path that no longer exists fails silently
and the player simply starts idle, which is what happens when the card has
been rearranged since.

The path is the only non-numeric value in `dsxp.cfg`, and it is parsed
before the integer parse rather than after: `/2 Unlimited/03.mp3` begins
with a digit, and `sscanf` would otherwise take the `2` and discard the
rest.

### Folders larger than the browser

`FB_MAX_ENTRIES` is 256. A folder with more than that shows the first 256
and **says so** in the path bar — `/Music  (first 256)`, in the accent
colour. It used to omit the rest silently, which is the worst way for a
file that is on the card to be missing from the list.

### Closing the lid

On models where the hinge bit does arrive (DS / DS Lite), shutting the
console does **not** suspend playback. calico's `pmMainLoop()`
sleeps on hinge-close by default, so `pmSetSleepAllowed(false)` turns that
off and the main loop handles the lid itself: both screens go black, all
input is ignored so nothing gets pressed in a pocket, and every pixel of
drawing work is skipped while decoding carries on.

> **The backlight itself stays on, and that is a platform limit, not an
> oversight.** The obvious call — `powerOff(PM_BACKLIGHT_TOP |
> PM_BACKLIGHT_BOTTOM)` — is a silent no-op on libnds 2.0: disassembling
> the ARM9 `powerOff` shows it testing bit 16 (`PM_ARM9_DIRECT`) and
> returning immediately for anything else, and the backlight bits are not
> ARM9-direct. The header still claims ARM7 bits get forwarded, but that
> forwarding is gone. The backlight is on the ARM7's SPI bus, which the
> ARM9 cannot reach. The stock ARM7 core does contain
> `pmicWriteRegister`, but its PXI command dispatch only handles battery,
> NVRAM, mic amp, sleep-allowed, power LED and reset — there is no command
> that reaches the PMIC's backlight register, so the ARM9 cannot ask for
> it either. Short of replacing that core with a custom ARM7, the
> strongest thing available is fading both 2D engines to black via
> `setBrightness(3, -16)` (the `MASTER_BRIGHT` registers), which is what
> this does. Expect a faint glow at the lid seam.
>
> The hinge is detected by sampling `KEY_LID` once at startup — when the
> lid is by definition open — and treating any change from that reading
> as "closed", rather than hardcoding a polarity. EXTKEYIN bit 7 is
> documented inconsistently, and calico passes it through uninverted while
> inverting the buttons either side of it, so this cannot be wrong about
> the sense; if the bit never changes it fails safe by leaving the screens
> alone instead of blanking at boot.

### Sleep timer

The third tab holds a 1–120 minute slider; the button under it arms and
cancels. The countdown is driven from calico's `tickGetCount()` rather
than a frame counter, so it does not run slow when the plasma visualizer
is up, and it keeps counting while paused. While it is armed the
remaining time also shows on the top screen.

Tick **"Turn the DS off when it ends"** and expiry shuts the console down
instead of just stopping playback. That is *not* done with libnds'
`systemShutDown()`, which is `powerOn(PM_SYSTEM_PWR)` and therefore hits
the same ARM9 guard that makes the backlight call a no-op. It uses
calico's documented route instead: `pmClearResetJumpTarget()` so exiting
does not bounce back to a homebrew menu, then `pmPrepareToReset()`, which
ends `pmMainLoop()` and lets calico power the console down on the way
out. **On a DSi this reboots rather than powers off** — that is calico's
documented DSi behaviour, not something the app can override.

## Layout of the source

| File | What it does |
|---|---|
| `main.c` | Screen setup, input handling, main loop |
| `audio.c` | maxmod stream + lock-free ring buffer |
| `decoder_mp3.c` | libmad driver, resampler, Xing/ID3 parsing, seeking |
| `player.c` | Transport state and the play queue |
| `filebrowser.c` | libfat directory listing |
| `skin.c` | Bottom-screen chrome and the top-screen overlay; drawing and hit testing |
| `visualizer.c` | Top-screen effects |
| `gfx.c` | Bitmap primitives and the text renderer |

### The typeface

`tools/make_font.py` draws DSXP's own 6x8 proportional face and emits
`source/font_data.h`. It replaced libnds' console font, which works but is
a chunky 2px-stem design built for a terminal; this one has 1px stems, a
real baseline and proper descenders, so it reads like small UI type
instead of like a console. It is also narrower, which is why a full
`01 - Everything In Its Right Place.mp3` now fits on a row that used to
truncate. Run the script with `--preview` to see it as ASCII art.

Drawing it rather than lifting one keeps the project owning everything it
ships, same reasoning as the skin artwork.

### There is no console layer

A 256x256 16-bit bitmap background occupies 128KB, which is the *entire*
sub-engine BG address space. A libnds console cannot coexist with one —
its map and tile data would land inside the pixels the skin is drawing,
and both would be corrupt. So `gfx.c` renders text into the bitmap
itself, blitting from libnds' own `default_font_bin` and measuring each
glyph at startup so text can be spaced proportionally. That also frees
the UI from the console's 8-pixel grid.

## Why the bottom screen only repaints when something changes

The top screen (visualizer) is double buffered across VRAM banks A and
B, so it can be redrawn in full every frame safely. The bottom screen
cannot be: real DS hardware has exactly one VRAM bank (C) that can drive
the sub engine's bitmap BG layer, so there is nowhere to put a second
128KB buffer for it. `main.c` tracks a small snapshot of everything the
skin's drawing depends on (view, selection, track, volume, duration,
stream info) and only repaints when that snapshot actually changes, plus
a throttled ~4Hz tick for the clock/progress bar while something is
playing and continuous redraw while the screen is being touched (so a
drag stays responsive). Skipping the other ~56 out of every 60 frames
when nothing changed is what keeps a single-buffered bitmap from tearing
visibly, and it's also a meaningful chunk of CPU headroom back for the
decoder on a 67MHz ARM9 with no FPU.

Repainting less often was not by itself enough, because it treated a
symptom. Two independent root causes had to be fixed as well.

**The screen blinked because the display could see half-drawn frames.**
The skin is composed in layers - background gradient, then panels, then
rows and text - so for most of a repaint's duration the background is
down but the content is not. Drawing that straight into a
single-buffered VRAM means the display controller scans out whatever
happens to be there, so *every* repaint flashed its own intermediate
state. That is not tearing and no amount of repainting less often fixes
it. The skin is now composed into a 96KB main-RAM backbuffer and DMA'd
to VRAM in one shot during vblank, so the display only ever sees
finished frames.

**Audio was noisy because maxmod's mixer thread could never preempt.**
`mm_stream.manual` is a union whose negative values mean "spawn the
stream thread at priority abs(value)"; passing zero asks for maxmod's
default, and disassembling `mmStreamOpen` shows that default is **29**
(`movge r2, #29`). calico's `MAIN_THREAD_PRIO` is **28**, and lower
numbers are higher priority - so the default puts the audio thread one
step *below* the main thread, and calico only round-robins between equal
priorities. The mixer was therefore only able to run when the main loop
happened to block in `swiWaitForVBlank`, and any frame that overran its
budget starved it into an underrun. That is why the noise tracked the
blinking so exactly: one long frame caused both. `audio.c` now asks for
priority `0x10` explicitly, so audio preempts drawing and decoding.

Two supporting optimisations came out of the same investigation. Every
gradient used to call `Gfx_Lerp` once per row, which does three integer
divisions - and this ARM9 has **no hardware divider**, so each is a slow
software routine. Instrumenting a host build showed a full repaint
making ~605 such calls *every single time*; since each gradient has a
fixed (top colour, bottom colour, height) triple, `skin.c` now caches
each ramp on first use, taking that to ~605 calls once and zero
thereafter. And `Gfx_Fill` - by far the hottest routine, since gradients
are just stacks of fills and the visualizer repaints every pixel every
frame - now writes 32-bit words carrying two pixels each instead of one
16-bit store at a time, halving the store count.

### The noise filter


It is a **downward expander**, not noise cancellation. Cancellation means
sampling ambient sound with a microphone and playing its inverse, and
nothing about this program can do that: the mic is not in the signal path,
and the output is buffered by roughly 150ms — cancellation needs latency
in the *microseconds*. Anything advertised here as "cancellation" would be
a lie.

What the expander does is lean on the level during passages quieter than
real music ever gets, which is exactly where tape hiss and low-bitrate
encoder noise are audible on their own. It is an expander rather than a
gate on purpose: a gate has one threshold and slams shut, so it chatters
on anything hovering near it and chops off reverb tails.

**It cannot remove noise sitting underneath music.** Nothing that fits in
this CPU budget can. The ten thresholds are spaced evenly *in dB*, because
loudness is perceived logarithmically and a linear ladder would bunch all
the useful settings at one end. Measured by `make test`:

| level | hiss at -54dBFS | quiet music (-40dBFS) |
|---|---|---|
| 10 | -4.6 dB | -0.0 dB |
| 30 | -10.7 dB | -0.0 dB |
| 50 | -16.8 dB | -0.9 dB |
| 70 | -22.7 dB | -7.0 dB |
| 100 | -31.5 dB | -16.2 dB |

Both the threshold *and* the deepest attenuation scale with the setting.
Holding the attenuation fixed made the top fifth of the slider useless:
past a point it was already clamped, so raising the setting further only
reached deeper into the music without removing any more hiss.

Normal-level music measures within **0.02 dB at every setting**, which is
the property that matters — a denoiser that audibly ducks a chorus is
worse than the hiss it removes. Up to about 40 it leaves quiet passages
alone entirely; above that it starts reaching into them, and near 100 it
will noticeably duck a fade-out. That is what the top of a range is for,
not a defect, and it is why off is the default.

> Controls on different pages are allowed to occupy the same pixels, so
> `Skin_HitTestButton()` takes the active tab. It is not optional: without
> it the first matching rectangle wins, and the Sleep page's checkboxes
> sat on top of the Setup page's Audio quality row — tapping "quality"
> silently toggled a hidden checkbox instead.
>
> Overlaps within a page are now caught mechanically. `make test` walks
> every pair of controls that can be on screen together and fails if their
> rectangles intersect, because hit testing returns the first match and an
> overlap means one control quietly eats another's taps — invisible until
> somebody taps the wrong thing.
