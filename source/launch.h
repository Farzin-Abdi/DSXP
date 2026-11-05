#ifndef DSXP_LAUNCH_H
#define DSXP_LAUNCH_H

#include <stdbool.h>
#include <stddef.h>

/* Resolve one argv path without truncating it. Device-qualified paths keep
 * their card; relative paths use the launcher's initial working directory.
 * Dot segments are normalized without allowing a walk above the card root. */
bool Launch_ResolvePath(const char *argument, const char *workingDir,
                        char *out, size_t outSize);

#endif
