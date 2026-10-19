#!/bin/sh
#
# Cross-compiles libmad 0.15.1b into libs/libmad/{include,lib} for the DS.
#
# libmad's own 1998-vintage autotools setup does not survive a modern
# binutils/gcc (it passes -fforce-mem, which gcc removed in 4.3), so this
# skips ./configure entirely and compiles the eleven source files directly.
# The only thing configure would really have decided for us is which
# fixed-point implementation to use, and we know that answer on ARM:
# FPM_ARM, the smull-based one.
#
# Usage:  ./tools/build_libmad.sh          (run once, then `make`)
#
set -eu

MAD_VERSION=0.15.1b
MAD_TARBALL="libmad-${MAD_VERSION}.tar.gz"
MAD_URL="https://sourceforge.net/projects/mad/files/libmad/${MAD_VERSION}/${MAD_TARBALL}/download"

TOPDIR=$(cd "$(dirname "$0")/.." && pwd)
WORKDIR="${TOPDIR}/libs/build"
PREFIX="${TOPDIR}/libs/libmad"

: "${DEVKITARM:=/opt/devkitpro/devkitARM}"
CC="${DEVKITARM}/bin/arm-none-eabi-gcc"
AR="${DEVKITARM}/bin/arm-none-eabi-ar"
RANLIB="${DEVKITARM}/bin/arm-none-eabi-ranlib"

if [ ! -x "$CC" ]; then
    echo "error: $CC not found - set DEVKITARM to your devkitARM install" >&2
    exit 1
fi

# -marm (not thumb): the FPM_ARM fixed-point macros in fixed.h are inline
# ARM-mode asm (smull/smlal), which will not assemble inside a Thumb-1
# function. This is also the hot path of the whole decoder, so ARM mode is
# what we want here regardless.
#
# OPT_SPEED/OPT_SSO trade a little output precision for a lot of speed in
# the IMDCT and subband synthesis. On a 67MHz ARM9 with no FPU that trade
# is the difference between "plays" and "stutters", and the loss is well
# below what the DS's speakers resolve.
#
# SIZEOF_INT has to be defined here even though nothing obvious asks for
# it. libmad's fixed.h picks `int` or `long` for mad_fixed_t based on it,
# and the public mad.h ships with SIZEOF_INT hardcoded to 4 - so building
# the library without it makes the library and its own header disagree
# about the size of every sample in every struct. On ARM the two spellings
# happen to be the same width so it goes unnoticed; on an LP64 host it is
# an immediate crash. Pinning it keeps the two in step by construction.
CFLAGS="-O2 -marm -march=armv5te -mtune=arm946e-s -mthumb-interwork \
        -ffunction-sections -fdata-sections -fomit-frame-pointer \
        -DFPM_ARM -DOPT_SPEED -DOPT_SSO -DASO_IMDCT -DASO_INTERLEAVE1 \
        -DDSXP_ITCM_SYNTH -DDSXP_ITCM_L3 \
        -DNDEBUG -DHAVE_ASSERT_H \
        -DSIZEOF_INT=4 -DSIZEOF_LONG=4 -DSIZEOF_LONG_LONG=8 \
        -std=gnu89 -w"

SOURCES="version.c fixed.c bit.c timer.c stream.c frame.c synth.c \
         decoder.c layer12.c layer3.c huffman.c"

# ASO_INTERLEAVE1 and ASO_ZEROCHECK are the other two ARM options
# libmad ships, and neither changes a single output sample:
#
#   INTERLEAVE1 software-pipelines the windowing loop, loading the next
#   window coefficient while multiplying by the current one. The same
#   multiplications in the same order, arranged so the ARM's load and
#   multiply units are not waiting on each other. libmad's own configure
#   enables it for every arm*-* host.
#
# ASO_ZEROCHECK is deliberately NOT enabled, though it is bit-exact and
# was tried. It skips the alias-reduction butterfly when both inputs are
# zero - but it pays a branch on every iteration to do it, and libmad's
# own configure enables it for i?86 and mips and pointedly not for arm.
# On a core where the multiply path is cheap and a mispredicted branch is
# not, that omission reads as deliberate rather than accidental, and
# there is no measurement here that contradicts it.
#
# ASO_IMDCT swaps layer3.c's C long-window IMDCT for libmad's own
# hand-written ARM assembly version of the same transform. It is what
# ./configure --enable-aso would have selected for an arm host, and it is
# the single largest speedup available here: the long IMDCT is one of the
# hottest routines in Layer III, and the assembly one is written around
# smull and register-scheduled by hand in a way the compiler does not
# match from the C.
#
# Not a quality trade, unlike OPT_SSO above - it computes the same
# transform, just faster. The decoder test suite checks the output either
# way, so a regression here would show up as a change in the resampler
# and SNR figures rather than silently.
ASM_SOURCES="imdct_l_arm.S"

mkdir -p "$WORKDIR"
cd "$WORKDIR"

if [ ! -f "$MAD_TARBALL" ]; then
    echo ">> downloading libmad ${MAD_VERSION}"
    curl -fL -o "$MAD_TARBALL" "$MAD_URL"
fi

rm -rf "libmad-${MAD_VERSION}"
tar xzf "$MAD_TARBALL"
cd "libmad-${MAD_VERSION}"

# The mad.h shipped in the tarball was generated on the maintainer's x86
# box and has FPM_INTEL baked into it. Everything else in that header is
# platform-neutral, so retargeting it is a one-line substitution rather
# than a reason to run configure.
sed -i.bak 's/^# define FPM_INTEL$/# define FPM_ARM/' mad.h
grep -q '^# define FPM_ARM$' mad.h || {
    echo "error: could not retarget mad.h to FPM_ARM" >&2
    exit 1
}

# The synthesis filter goes in ITCM.
#
# It is the most expensive stage of decoding - the Test page's load split
# shows it as the largest of the three - and it is 8KB of code running
# thirty-eight times a second against a 44KB layer3.o competing for the
# same instruction cache. ITCM is tightly coupled, has no wait states,
# and has 28KB spare after the resampler.
#
# Only the two filter functions, not the whole file: mad_synth_frame
# itself is a dispatcher that runs once a frame and belongs with the rest
# of the code. Both are static, so the attribute goes on the definition
# line - the one carrying parameter names, which is what distinguishes it
# from the ASO_SYNTH forward declaration above that this build does not
# compile.
#
# The attribute goes on the same line rather than its own: BSD sed does
# not read \n in a replacement as a newline, and the first attempt at
# this quietly produced a literal backslash-n in the middle of a function
# signature. Same line is valid C and works in both seds.
#
# The count check below fails the build rather than letting a changed
# synth.c quietly produce a slower library.
echo ">> placing the synthesis filter in ITCM"
# Behind a macro rather than written on the functions directly, because
# the host test build compiles this same patched source - and a section
# named ".itcm", a long_call, and target("arm") are all meaningless on a
# desktop. The first attempt put the attribute on bare and broke every
# host test in the suite. The macro is empty unless this script's own
# define is present.
# The macro goes in at the top of the file rather than beside the window
# table: dct32 is placed below and is defined long before that table, so
# a definition further down would be invisible to it.
sed -i.bak 's|^# include "synth.h"|# include "synth.h"\
\
#ifdef DSXP_ITCM_SYNTH\
# define DSXP_SYNTH_ITCM __attribute__((section(".itcm"), long_call, target("arm")))\
#else\
# define DSXP_SYNTH_ITCM\
#endif|' synth.c

for fn in synth_full synth_half; do
    sed -i.bak "s|^void ${fn}(struct mad_synth \*synth,|DSXP_SYNTH_ITCM void ${fn}(struct mad_synth *synth,|" synth.c
done

# And the 32-point DCT the filter calls for every group of subbands.
#
# It was left out of the first pass and that was a mistake: synth_full is
# the dispatcher around it, dct32 is where the arithmetic happens, and at
# 3072 bytes it was the largest single thing still reaching into main RAM
# from inside a loop that had otherwise been made fast.
sed -i.bak 's|^void dct32(mad_fixed_t const in\[32\], unsigned int slot,|DSXP_SYNTH_ITCM void dct32(mad_fixed_t const in[32], unsigned int slot,|' synth.c

dct=$(grep -c '^DSXP_SYNTH_ITCM void dct32(' synth.c || true)
if [ "$dct" != "1" ]; then
    echo "error: expected the DCT to be placed in ITCM, found ${dct}" >&2
    echo "       synth.c has changed shape - check dct32's signature" >&2
    exit 1
fi

placed=$(grep -c 'DSXP_SYNTH_ITCM void synth_' synth.c || true)
if [ "$placed" != "2" ]; then
    echo "error: expected two ITCM placements in synth.c, found ${placed}" >&2
    echo "       synth.c has changed shape - check the function signatures" >&2
    exit 1
fi
grep -q 'define DSXP_SYNTH_ITCM __attribute__' synth.c || {
    echo "error: the ITCM macro did not get defined in synth.c" >&2
    exit 1
}

# The window table the synthesis filter reads, reachable through a
# pointer so the player can move it at run time.
#
# The filter's code is in ITCM above; its data is not, and the data is
# the half that hurts. One channel of filter state is 2KB and there are
# two of them, so the state alone fills the ARM946's 4KB data cache -
# and D, another 2.2KB read straight through the inner loop, has nowhere
# to live. The two evict each other for the length of every frame.
#
# ITCM has 27KB free and is single-cycle, so a copy of D placed there
# takes it out of that fight entirely. Same numbers, different address:
# nothing about the output changes.
#
# Through a pointer rather than by moving D itself, because the choice
# then costs one load per channel per frame instead of being welded in at
# build time - which is what lets it be a switch on the Test page rather
# than a rebuild, on a console where this kind of change has produced a
# black screen before.
echo ">> routing the synthesis window table through a pointer"
sed -i.bak 's|^# if defined(ASO_SYNTH)|mad_fixed_t const (*dsxp_Dbase)[32] = D;\
\
# if defined(ASO_SYNTH)|' synth.c
sed -i.bak 's|Dptr = \&D\[0\];|Dptr = dsxp_Dbase;|g' synth.c

routed=$(grep -c 'Dptr = dsxp_Dbase;' synth.c || true)
if [ "$routed" != "2" ]; then
    echo "error: expected two window-table reads in synth.c, found ${routed}" >&2
    echo "       synth.c has changed shape - check synth_full/synth_half" >&2
    exit 1
fi
grep -q '^mad_fixed_t const (\*dsxp_Dbase)\[32\] = D;' synth.c || {
    echo "error: the window-table pointer did not get defined in synth.c" >&2
    exit 1
}

# The Layer III decoder, in ITCM.
#
# Measured on the console, the bit decoding is the largest of the three
# stages - and III_decode is 7160 bytes of it, called for every granule
# of every frame, sitting in a 44KB object that shares an 8KB
# instruction cache with everything else the player does. The synthesis
# filter went into ITCM for exactly this reason and it worked; this is
# the same argument applied to the stage that turned out to be bigger.
#
# There is room: the filter and its window table use about 7KB of the
# 32KB, so this fits with fifteen to spare. If it ever stops fitting,
# the link fails rather than the console misbehaving.
#
# Not a quality trade of any kind - the same code at a different address.
# Unlike the window table this cannot be a switch on the Test page,
# because where a function lives is decided when it is linked.
echo ">> placing the Layer III decoder in ITCM"
sed -i.bak 's|^# include "layer3.h"|# include "layer3.h"\
\
#ifdef DSXP_ITCM_L3\
# define DSXP_L3_ITCM __attribute__((section(".itcm"), long_call, target("arm")))\
#else\
# define DSXP_L3_ITCM\
#endif|' layer3.c

sed -i.bak 's|^enum mad_error III_decode(struct mad_bitptr \*ptr, struct mad_frame \*frame,|DSXP_L3_ITCM enum mad_error III_decode(struct mad_bitptr *ptr, struct mad_frame *frame,|' layer3.c

l3placed=$(grep -c 'DSXP_L3_ITCM enum mad_error III_decode' layer3.c || true)
if [ "$l3placed" != "1" ]; then
    echo "error: expected one ITCM placement in layer3.c, found ${l3placed}" >&2
    echo "       layer3.c has changed shape - check III_decode's signature" >&2
    exit 1
fi
grep -q 'define DSXP_L3_ITCM __attribute__' layer3.c || {
    echo "error: the ITCM macro did not get defined in layer3.c" >&2
    exit 1
}

# The Huffman tables, and the rest of the hot Layer III code.
#
# The tables are 4646 bytes read for every coefficient of every granule,
# which makes them the hottest data in the stage the console measured as
# the largest. They are also small enough that all twenty of them fit
# with room to spare, so there is no hot/cold split to get wrong.
#
# The functions below III_decode go in for the same reason it did. None
# of this is a quality trade - it is the same code and the same numbers
# at a different address.
#
# If any of it ever stops fitting, the link fails with an ITCM overflow
# rather than the console misbehaving, which is the right way round.
echo ">> placing the Huffman tables in ITCM"
sed -i.bak 's|^union huffpair const hufftab|DSXP_L3_ITCM union huffpair const hufftab|' huffman.c
sed -i.bak 's|^union huffquad const hufftab|DSXP_L3_ITCM union huffquad const hufftab|' huffman.c
sed -i.bak 's|^union huffpair const \*const mad_huff_pair_table|DSXP_L3_ITCM union huffpair const *const mad_huff_pair_table|' huffman.c
sed -i.bak 's|^union huffquad const \*const mad_huff_quad_table|DSXP_L3_ITCM union huffquad const *const mad_huff_quad_table|' huffman.c
sed -i.bak 's|^# include "huffman.h"|# include "huffman.h"\
\
#ifdef DSXP_ITCM_L3\
# define DSXP_L3_ITCM __attribute__((section(".itcm")))\
#else\
# define DSXP_L3_ITCM\
#endif|' huffman.c

huff=$(grep -c '^DSXP_L3_ITCM union huff' huffman.c || true)
if [ "$huff" -lt 20 ]; then
    echo "error: expected at least twenty ITCM tables in huffman.c, found ${huff}" >&2
    exit 1
fi

echo ">> placing the rest of the hot Layer III code in ITCM"
# Each signature spelled out rather than matched by a pattern: BSD sed
# has no alternation in a basic regexp, and a clever pattern that
# silently matches nothing is exactly the failure the counts below exist
# to catch.
sed -i.bak 's|^int mad_layer_III(|DSXP_L3_ITCM int mad_layer_III(|' layer3.c
sed -i.bak 's|^void III_imdct_s(|DSXP_L3_ITCM void III_imdct_s(|' layer3.c
sed -i.bak 's|^mad_fixed_t III_requantize(|DSXP_L3_ITCM mad_fixed_t III_requantize(|' layer3.c
sed -i.bak 's|^void III_aliasreduce(|DSXP_L3_ITCM void III_aliasreduce(|' layer3.c
sed -i.bak 's|^void III_freqinver(|DSXP_L3_ITCM void III_freqinver(|' layer3.c

more=$(grep -c '^DSXP_L3_ITCM ' layer3.c || true)
if [ "$more" -lt 5 ]; then
    echo "error: expected five more ITCM placements in layer3.c, found ${more}" >&2
    echo "       layer3.c has changed shape - check the function signatures" >&2
    exit 1
fi

# The bit reader, which every Huffman symbol goes through.
#
# Three small functions and the header they share. Tiny next to
# everything above - a few hundred bytes - but called more often than
# any of it.
echo ">> placing the bit reader in ITCM"
sed -i.bak 's|^# include "bit.h"|# include "bit.h"\
\
#ifdef DSXP_ITCM_L3\
# define DSXP_L3_ITCM __attribute__((section(".itcm"), long_call, target("arm")))\
#else\
# define DSXP_L3_ITCM\
#endif|' bit.c
sed -i.bak 's|^unsigned long mad_bit_read(|DSXP_L3_ITCM unsigned long mad_bit_read(|' bit.c
sed -i.bak 's|^void mad_bit_skip(|DSXP_L3_ITCM void mad_bit_skip(|' bit.c

bits=$(grep -c '^DSXP_L3_ITCM ' bit.c || true)
if [ "$bits" != "2" ]; then
    echo "error: expected two ITCM placements in bit.c, found ${bits}" >&2
    exit 1
fi

echo ">> compiling libmad (FPM_ARM, arm mode)"
# shellcheck disable=SC2086
for src in $SOURCES; do
    $CC $CFLAGS -c "$src" -o "${src%.c}.o"
done

# shellcheck disable=SC2086
# Assembled into an object whose *name* carries .itcm, which is how the
# hand-written IMDCT gets into fast RAM without touching the assembly.
#
# devkitARM's linker script routes `*.itcm*(.text .stub .text.*)` there,
# matching on the file name rather than on a section attribute - and an
# attribute is not available here anyway, because this is assembly with
# no C declaration to hang one on. The routine is the long-window IMDCT,
# which libmad's own notes call one of the hottest in Layer III.
for src in $ASM_SOURCES; do
    echo ">> assembling $src into ITCM"
    $CC $CFLAGS -c "$src" -o "${src%.S}.itcm.o"
done

rm -f libmad.a
# shellcheck disable=SC2046
$AR rcs libmad.a $(echo "$SOURCES" | sed 's/\.c/.o/g') \
                 $(echo "$ASM_SOURCES" | sed 's/\.S/.itcm.o/g')
$RANLIB libmad.a

mkdir -p "${PREFIX}/include" "${PREFIX}/lib"
cp mad.h "${PREFIX}/include/mad.h"
cp libmad.a "${PREFIX}/lib/libmad.a"

echo ">> installed:"
echo "   ${PREFIX}/include/mad.h"
echo "   ${PREFIX}/lib/libmad.a"
