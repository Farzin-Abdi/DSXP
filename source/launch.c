#include "launch.h"
#include "filebrowser.h"

#include <stdio.h>
#include <string.h>

static size_t RootLength(const char *path)
{
    if (strncmp(path, "sd:/", 4) == 0) return 4;
    if (strncmp(path, "fat:/", 5) == 0) return 5;
    return path[0] == '/' ? 1 : 0;
}

bool Launch_ResolvePath(const char *argument, const char *workingDir,
                        char *out, size_t outSize)
{
    if (!out || !outSize) return false;
    out[0] = '\0';
    if (!argument || !argument[0] || strchr(argument, '\\')) return false;

    char raw[FB_PATH_LEN];
    int len;
    if (RootLength(argument)) {
        len = snprintf(raw, sizeof(raw), "%s", argument);
    } else {
        if (!workingDir || !RootLength(workingDir) || strchr(argument, ':'))
            return false;
        len = snprintf(raw, sizeof(raw), "%s/%s", workingDir, argument);
    }
    if (len < 0 || (size_t)len >= sizeof(raw)) return false;

    const size_t root = RootLength(raw);
    /* Colons belong only to the recognized volume prefix. */
    if (strchr(raw + root, ':') || root >= outSize) return false;
    memcpy(out, raw, root);
    size_t used = root;
    out[used] = '\0';

    const char *part = raw + root;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        if (n == 2 && part[0] == '.' && part[1] == '.') {
            while (used > root && out[used - 1] != '/') used--;
            if (used > root) used--;
            out[used] = '\0';
        } else if (n && !(n == 1 && part[0] == '.')) {
            const bool separator = used > root;
            if (used + separator + n >= outSize) {
                out[0] = '\0';
                return false;
            }
            if (separator) out[used++] = '/';
            memcpy(out + used, part, n);
            used += n;
            out[used] = '\0';
        }
        if (!end) break;
        part = end + 1;
    }
    return true;
}
