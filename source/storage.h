#ifndef DSXP_STORAGE_H
#define DSXP_STORAGE_H

#include <stdbool.h>

/* Mount once, including native DSi SD and DLDI devices supported by libfat. */
bool Storage_Init(void);

/* Select the card for root-relative browser, playlist and settings paths.
 * An explicit sd:/ or fat:/ path must use that card; otherwise prefer SD.
 * This changes the default filesystem device, not any files on the card. */
bool Storage_Select(const char *path);

/* Strip a supported device prefix after Storage_Select has succeeded. */
const char *Storage_LocalPath(const char *path);

#endif
