#!/bin/sh
#
# Host-side checks for the parts of DSXP that are not DS-specific:
#
#   test_decoder  - drives decoder_mp3.c against synthetic MPEG files and
#                   checks duration, seeking, end-of-file behaviour and the
#                   resampler against a reference sine
#   measure_audio - checks the resampler's headroom, so full-scale material
#                   cannot silently go back to hard-clipping
#   test_settings - round-trips the saved preferences file
#   test_browser  - what the file list shows, and what it hides
#   test_order    - shuffle and repeat: the play order, with the decoder
#                   stubbed out
#   preview       - renders the skin and the visualizers to BMPs so the
#                   layout can be looked at without a flashcart
#
# Neither needs a DS, but both compile the same source files the ROM does.
#
set -eu

TOPDIR=$(cd "$(dirname "$0")/.." && pwd)
SRC="${TOPDIR}/source"
WORK="${TOPDIR}/tools/hosttest"
BUILD="${WORK}/build"

: "${DEVKITARM:=/opt/devkitpro/devkitARM}"
: "${DEVKITPRO:=/opt/devkitpro}"
CC=${CC:-cc}

mkdir -p "$BUILD"
cd "$BUILD"

# --- libmad, built for the host -------------------------------------
#
# Same sources as the DS build, but with the portable fixed-point path.
# SIZEOF_INT must match what mad.h hardcodes or the library and its own
# header disagree about the width of mad_fixed_t (see build_libmad.sh).
MADSRC="${TOPDIR}/libs/build/libmad-0.15.1b"
if [ ! -d "$MADSRC" ]; then
    echo "error: run ./tools/build_libmad.sh first (it leaves the unpacked" >&2
    echo "       libmad sources in libs/build/ for this script to reuse)" >&2
    exit 1
fi

if [ ! -f libmad_host.a ]; then
    echo ">> building libmad for the host"
    for s in version fixed bit timer stream frame synth decoder layer12 layer3 huffman; do
        $CC -O1 -w -DFPM_64BIT -DNDEBUG -DHAVE_ASSERT_H \
            -DSIZEOF_INT=4 -DSIZEOF_LONG_LONG=8 \
            -c "${MADSRC}/${s}.c" -o "${s}.o"
    done
    ar rcs libmad_host.a version.o fixed.o bit.o timer.o stream.o frame.o \
                         synth.o decoder.o layer12.o layer3.o huffman.o
    sed 's/^# define FPM_INTEL$/# define FPM_64BIT/' "${MADSRC}/mad.h" > mad.h
fi

# --- test inputs -----------------------------------------------------
python3 "${WORK}/make_mp3.py" "$BUILD" > /dev/null

# --- build and run ---------------------------------------------------
# Rebuilt every run, never conditionally.
#
# The compile lines below were once deleted by an edit that was meant to
# remove some libraries from them, and the script went on running the
# binaries already in build/ - reporting the same pass count whatever the
# sources said, for as long as nobody looked at a timestamp. Anything
# added here must build, not just exist.
echo ">> building tests"
$CC -O1 -g -Wall -I. -I"${WORK}/stub" -I"$SRC" "${WORK}/test_decoder.c" "${SRC}/textfold.c" "${SRC}/jpeg.c" libmad_host.a -lm -o test_decoder
$CC -O1 -g -w -I. -I"${WORK}/stub" -I"$SRC" "${WORK}/measure_audio.c" "${SRC}/textfold.c" "${SRC}/jpeg.c" libmad_host.a -lm -o measure_audio
$CC -O1 -g -w -DDSXP_COUNT_PIXELS -DDSXP_HOSTTEST -I. -I"${WORK}/stub" -I"$SRC" "${WORK}/preview.c" "${SRC}/textfold.c" -lm -o preview
$CC -O1 -g -w -I"${WORK}/stub" -I"$SRC" \
    -DSETTINGS_PATH="\"${BUILD}/dsxp_test.cfg\"" \
    "${WORK}/test_settings.c" "${SRC}/textfold.c" "${SRC}/cardwrite.c" -o test_settings
$CC -O1 -g -w -I"${WORK}/stub" -I"$SRC" "${WORK}/test_browser.c" "${SRC}/textfold.c" "${SRC}/cardwrite.c" -o test_browser
# cardwrite.c is compiled into this one rather than linked, because it
# shims rename() to reach the fallback that only FatFs ever takes.
$CC -O1 -g -w -I"${WORK}/stub" -I"$SRC" "${WORK}/test_cardwrite.c" -o test_cardwrite
$CC -O1 -g -w -I. -I"${WORK}/stub" -I"$SRC" "${WORK}/test_order.c" "${SRC}/textfold.c" -lm -o test_order
$CC -O1 -g -w -I"${WORK}/stub" -I"$SRC" \
    -DDSXP_DIR="\"${BUILD}/playlist_root/dsxp_info\"" \
    "${WORK}/test_playlist.c" "${SRC}/textfold.c" "${SRC}/cardwrite.c" -o test_playlist
$CC -O1 -g -w -DDSXP_HOSTTEST -I. -I"${WORK}/stub" -I"$SRC" "${WORK}/test_keyboard.c" "${SRC}/textfold.c" -lm -o test_keyboard
# The JPEG decoder, with the sanitizers on. It is the one thing here that
# walks a bitstream from a file it did not write, so an overrun is the
# failure mode that matters and the one a passing picture would hide.
$CC -O1 -g -Wall -fsanitize=address,undefined -I. -I"${WORK}/stub" -I"$SRC" "${WORK}/test_jpeg.c" -o test_jpeg

echo
./test_decoder
status=$?


echo
./measure_audio || status=1

echo
./test_settings || status=1

echo
rm -rf "${BUILD}/browser_root"
./test_browser "${BUILD}/browser_root" || status=1

echo
./test_order || status=1

echo
rm -rf "${BUILD}/cardwrite_root"
./test_cardwrite "${BUILD}/cardwrite_root" || status=1

echo
rm -rf "${BUILD}/playlist_root"
./test_playlist "${BUILD}/playlist_root" || status=1

echo
./test_keyboard || status=1

echo
./test_jpeg "${WORK}/jpeg" || status=1

echo
./preview || status=1

# --- checks on the built ROM, if there is one ------------------------
#
# Both of these look at build output rather than at source, and both
# skip when the DS build has not been run. They are here because each
# guards something that silently stopped happening once already: the
# icon was not reaching the ROM at all, and the hot routines had drifted
# out of fast RAM. Neither failure shows up in any other test, and
# neither script was run by anything until now.
echo
python3 "${TOPDIR}/tools/check_icon.py" "${TOPDIR}/dsxp.nds" "${TOPDIR}/gfx/icon.bmp" || status=1
sh "${TOPDIR}/tools/check_itcm.sh" "${TOPDIR}/dsxp.elf" || status=1

echo
echo "previews written to ${BUILD}/preview_*.bmp"
exit $status
