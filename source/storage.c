#include "storage.h"

#include <fat.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool s_mounted;

bool Storage_Init(void)
{
    if (!s_mounted) s_mounted = fatInitDefault();
    return s_mounted;
}

static bool SelectRoot(const char *root)
{
    struct stat st;
    return stat(root, &st) == 0 && S_ISDIR(st.st_mode) && chdir(root) == 0;
}

bool Storage_Select(const char *path)
{
    if (!Storage_Init()) return false;

    if (path && strncmp(path, "sd:/", 4) == 0)
        return SelectRoot("sd:/");
    if (path && strncmp(path, "fat:/", 5) == 0)
        return SelectRoot("fat:/");
    /* Never silently reinterpret an unknown device as another card. */
    if (path && strchr(path, ':')) return false;

    return SelectRoot("sd:/") || SelectRoot("fat:/");
}

const char *Storage_LocalPath(const char *path)
{
    if (!path) return NULL;
    if (strncmp(path, "sd:/", 4) == 0) return path + 3;
    if (strncmp(path, "fat:/", 5) == 0) return path + 4;
    return path;
}
