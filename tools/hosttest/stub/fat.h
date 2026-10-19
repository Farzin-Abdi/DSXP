/* Host stub. filebrowser.c includes <fat.h> for fatInitDefault, which is
 * the one libfat call it makes; everything else it does is plain POSIX
 * directory reading that works unchanged off-target. */
#ifndef DSXP_STUB_FAT_H
#define DSXP_STUB_FAT_H
#include <stdbool.h>

/* Drivable, because the interesting case is the one where it says no.
 * A card that will not mount is what the browser has to survive without
 * wiping itself, and there is no other way to ask for that on a host
 * where the "card" is a directory that is always there. */
static bool g_fatMountFails = false;
static inline bool fatInitDefault(void) { return !g_fatMountFails; }
#endif
