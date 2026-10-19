#!/bin/sh
# Checks that the hot routines actually ended up in tightly-coupled RAM.
#
# This exists because they silently did not. The ITCM attribute in
# decoder_mp3.c sits on its own line above the function it decorates, and
# inserting anything between the two moves it onto whatever follows -
# which is still valid C, still links, still plays. It had drifted onto
# the file-refill routine, so the resampler's innermost loop had been
# running out of main RAM for some time with nothing to say so.
#
# A placement that quietly stops happening costs performance and nothing
# else, which is the kind of regression no test catches by accident.
set -e

ELF="${1:-dsxp.elf}"
NM="${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi-nm"

[ -f "$ELF" ] || { echo "  [SKIP] no $ELF to check"; exit 0; }

# ITCM is mapped at 0x01ff8000 on this console.
placed=$("$NM" "$ELF" | awk '$1 ~ /^01ff/ { print $3 }')

# The audio routines have first claim on this memory; the three drawing
# ones are what fits in what is left, and are named here so that running
# out shows up as this failing rather than as the screen quietly getting
# slower.
want="ResampleFrame dct32 synth_full synth_half III_decode III_requantize
      mad_bit_read hufftab24 dsxp_windowItcm OnStreamRequest
      Gfx_Fill Gfx_Blit Gfx_Lerp DrawLabel"

missing=""
for sym in $want; do
    echo "$placed" | grep -q "^${sym}" || missing="$missing $sym"
done

if [ -n "$missing" ]; then
    echo "  [FAIL] not in fast RAM:$missing" >&2
    exit 1
fi

used=$("${NM%nm}size" -A "$ELF" | awk '/^\.itcm/ { print $2 }')

# 0x7fe0, not 0x8000: calico's linker script gives the region ITCM minus
# the thirty-two bytes of exception vectors that sit at its base
# (ds9.ld: itcm : ORIGIN = 0x1ff8020, LENGTH = 0x7fe0). Measuring against
# the round number overstated the space left by those same thirty-two
# bytes, which matters now that what is left is smaller than that gap.
ITCM_BYTES=32736
free=$((ITCM_BYTES - used))

# How close it is, as well as what is in it. Overflowing is a link error
# and looks after itself; running it to within a few dozen bytes is the
# state where the next placement fails for a reason that has nothing to
# do with the change that triggered it, and that is worth seeing coming.
if [ "$free" -lt 64 ]; then
    echo "  [WARN] fast RAM is nearly full: ${used} of ${ITCM_BYTES} (${free} bytes left)"
else
    echo "  [PASS] the hot routines are in fast RAM (${used} of ${ITCM_BYTES} bytes used, ${free} free)"
fi
