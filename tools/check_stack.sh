#!/bin/sh
#
# The decoder worker's stack against what the codecs actually need.
#
# This is checked mechanically because the failure mode is silent: a
# codec whose frame does not fit writes past the end of the stack and the
# symptom is a format that does not play, with nothing to say why. That
# is what a 40KB FAAD2 frame did to a 16KB stack.
#
set -eu
TOPDIR=$(cd "$(dirname "$0")/.." && pwd)
: "${DEVKITARM:=/opt/devkitpro/devkitARM}"
OD="${DEVKITARM}/bin/arm-none-eabi-objdump"

declared=$(grep -oE 's_workerStack\[[0-9]+ \* 1024\]' "${TOPDIR}/source/decoder_mp3.c" \
           | grep -oE '^s_workerStack\[[0-9]+' | grep -oE '[0-9]+$')
declared=$((declared * 1024))

worst=0; who=""
for lib in "${TOPDIR}"/libs/*/lib/*.a; do
    [ -f "$lib" ] || continue
    m=$("$OD" -d "$lib" 2>/dev/null \
        | grep -oE 'sub[[:space:]]+sp, sp, #[0-9]+' \
        | grep -oE '[0-9]+$' | sort -rn | head -1)
    [ -n "${m:-}" ] || continue
    echo "  $(basename "$lib"): largest frame ${m}"
    if [ "$m" -gt "$worst" ]; then worst=$m; who=$(basename "$lib"); fi
done

echo "  worker stack: ${declared} bytes, worst frame ${worst} (${who})"

# The main thread's stack, for contrast. It lives in DTCM - __sp_usr down
# to the end of .dtcm.bss - and is far smaller than any codec needs,
# which is why decoding happens on the worker and nowhere else. Printed
# rather than asserted: it is calico's number, not this project's.
ELF="${TOPDIR}/dsxp.elf"
if [ -f "$ELF" ]; then
    NM="${DEVKITARM}/bin/arm-none-eabi-nm"
    top=$("$NM" "$ELF" 2>/dev/null | awk '$3=="__sp_usr"{print $1}')
    bot=$("$NM" "$ELF" 2>/dev/null | awk '$3=="__dtcm_bss_end"{print $1}')
    if [ -n "$top" ] && [ -n "$bot" ]; then
        room=$(( 0x$top - 0x$bot ))
        echo "  main thread stack in DTCM: ${room} bytes - codecs must not run here"
    fi
fi

# Room for the call chain above and below the deepest frame, not just the
# frame itself.
need=$((worst * 3 / 2))
if [ "$declared" -lt "$need" ]; then
    echo "  [FAIL] worker stack ${declared} leaves too little over ${worst}" >&2
    exit 1
fi
echo "  [PASS] the decoder worker's stack fits the hungriest codec"
