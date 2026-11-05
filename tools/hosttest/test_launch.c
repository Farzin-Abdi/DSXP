#include <stdio.h>
#include <string.h>
#include "launch.h"
#include "filebrowser.h"

static int failures;

static void CheckPath(const char *label, const char *arg, const char *cwd,
                      const char *expected)
{
    char out[FB_PATH_LEN];
    bool ok = Launch_ResolvePath(arg, cwd, out, sizeof(out));
    bool passed = expected ? ok && strcmp(out, expected) == 0 : !ok;
    printf("  [%s] %s\n", passed ? "PASS" : "FAIL", label);
    if (!passed) failures++;
}

int main(void)
{
    CheckPath("SD MP3 with spaces", "sd:/Music/Some Song.mp3", "/",
              "sd:/Music/Some Song.mp3");
    CheckPath("DLDI MP3", "fat:/Music/song.mp3", "sd:/",
              "fat:/Music/song.mp3");
    CheckPath("root MP3", "/song.mp3", "sd:/elsewhere", "/song.mp3");
    CheckPath("SD root", "sd:/", "/", "sd:/");
    CheckPath("DLDI root", "fat:/", "/", "fat:/");
    CheckPath("directory trailing slash", "sd:/Music/", "/", "sd:/Music");
    CheckPath("relative MP3 uses launch cwd", "song.mp3", "sd:/Music",
              "sd:/Music/song.mp3");
    CheckPath("relative DLDI path keeps its device", "../song.mp3", "fat:/Music",
              "fat:/song.mp3");
    CheckPath("dot directory", ".", "sd:/Music", "sd:/Music");
    CheckPath("collapse separators", "sd:////Music//song.mp3", "/",
              "sd:/Music/song.mp3");
    CheckPath("dot segments", "/a/./b/../c.mp3", "/", "/a/c.mp3");
    CheckPath("cannot walk above the SD root", "sd:/../../song.mp3", "/",
              "sd:/song.mp3");
    CheckPath("UTF-8 path is preserved", "sd:/Musik/Bj\303\266rk.mp3", "/",
              "sd:/Musik/Bj\303\266rk.mp3");
    CheckPath("missing argument", NULL, "/", NULL);
    CheckPath("empty argument", "", "/", NULL);
    CheckPath("unknown device refused", "nand:/song.mp3", "/", NULL);
    CheckPath("URL refused", "https://example.com/song.mp3", "/", NULL);
    CheckPath("relative path needs an absolute base", "song.mp3", "Music", NULL);
    CheckPath("relative path cannot guess a missing cwd", "song.mp3", NULL, NULL);
    CheckPath("colon in a filename refused", "/song:bad.mp3", "/", NULL);
    CheckPath("backslash path refused", "sd:\\Music\\song.mp3", "/", NULL);
    char longPath[FB_PATH_LEN + 10];
    memset(longPath, 'a', sizeof(longPath) - 1);
    longPath[sizeof(longPath) - 1] = '\0';
    CheckPath("overlong argument refused, never truncated", longPath, "/", NULL);
    char small[4] = "old";
    bool ok = !Launch_ResolvePath("sd:/song.mp3", "/", small, sizeof(small)) && !small[0];
    printf("  [%s] short output buffer refused\n", ok ? "PASS" : "FAIL");
    if (!ok) failures++;
    return failures ? 1 : 0;
}
