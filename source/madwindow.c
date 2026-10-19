/* The synthesis filter's window table, in tightly-coupled RAM.
 *
 * A file of its own for one reason: decoder_mp3.c already puts code in
 * the .itcm section, and a single translation unit cannot emit that
 * section as both executable and writable - GCC refuses with a section
 * type conflict. The linker is happy to concatenate one object's code
 * and another's data under the same name, so the buffer moves here and
 * nothing else has to change.
 *
 * Why it is worth the file: libmad's filter state is 2KB a channel and
 * there are two, which is the whole of the ARM946's 4KB data cache. The
 * window table is another 2.2KB read through the innermost loop, so for
 * the length of every frame the two evict each other. ITCM is
 * single-cycle and has 27KB free.
 *
 * Filled at run time from libmad's own copy rather than duplicated as
 * source - there is exactly one set of correct values and they live in
 * D.dat, inside the library's build tree. */
#include <string.h>

#ifdef ARM9

#include "mad.h"

/* Not const: it is written once, at the moment the setting is turned on. */
mad_fixed_t dsxp_windowItcm[17][32] __attribute__((section(".itcm")));

/* Copied from wherever the caller says rather than from D directly.
 *
 * D is a file-scope const in libmad's synth.c and the compiler gives it
 * local linkage, so it cannot be named from another object at all - but
 * the pointer the player already reads it through holds its address, and
 * that is enough. */
void Mp3Window_Load(const void *src)
{
    memcpy(dsxp_windowItcm, src, sizeof(dsxp_windowItcm));
}

#endif
