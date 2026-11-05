#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fat.h>

static bool hasSd, hasFat, failChdir;
static char selected[16];
static int TestStat(const char *path, struct stat *st)
{
    if ((!strcmp(path, "sd:/") && hasSd) || (!strcmp(path, "fat:/") && hasFat)) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR;
        return 0;
    }
    return -1;
}
static int TestChdir(const char *path)
{
    if (failChdir) return -1;
    snprintf(selected, sizeof(selected), "%s", path);
    return 0;
}
#define stat(path, st) TestStat(path, st)
#define chdir(path) TestChdir(path)
#include "storage.c"
#undef stat
#undef chdir

static int failures;
static void Check(const char *label, bool ok)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

int main(void)
{
    g_fatMountFails = true;
    Check("mount failure is reported", !Storage_Init());
    g_fatMountFails = false;
    Check("a failed mount can be retried", Storage_Init());
    int calls = g_fatMountCalls;
    Check("an existing mount is reused", Storage_Init() && g_fatMountCalls == calls);
    hasSd = hasFat = true;
    Check("prefer native SD when both are mounted",
          Storage_Select(NULL) && !strcmp(selected, "sd:/"));
    Check("explicit DLDI argument selects DLDI",
          Storage_Select("fat:/Music/song.mp3") && !strcmp(selected, "fat:/"));
    Check("explicit SD argument selects SD",
          Storage_Select("sd:/Music/song.mp3") && !strcmp(selected, "sd:/"));
    hasSd = false;
    Check("fall back to DLDI when native SD is absent",
          Storage_Select("/Music") && !strcmp(selected, "fat:/"));
    Check("an unavailable explicit SD card never opens a different card",
          !Storage_Select("sd:/Music/song.mp3") && !strcmp(selected, "fat:/"));
    Check("unknown device refused", !Storage_Select("nand:/song.mp3"));
    hasSd = true; hasFat = false;
    Check("native-SD-only setup works", Storage_Select(NULL) && !strcmp(selected, "sd:/"));
    Check("an unavailable explicit DLDI card is refused", !Storage_Select("fat:/song.mp3"));
    hasSd = false;
    Check("no usable volumes is reported", !Storage_Select(NULL));
    hasSd = true; failChdir = true;
    Check("an inaccessible root is reported", !Storage_Select("sd:/"));
    Check("SD prefix strips to root-relative path",
          !strcmp(Storage_LocalPath("sd:/Music/song.mp3"), "/Music/song.mp3"));
    Check("DLDI prefix strips to root-relative path",
          !strcmp(Storage_LocalPath("fat:/song.mp3"), "/song.mp3"));
    Check("plain path stays unchanged", !strcmp(Storage_LocalPath("/Music"), "/Music"));
    Check("null path is harmless", Storage_LocalPath(NULL) == NULL);
    return failures ? 1 : 0;
}
