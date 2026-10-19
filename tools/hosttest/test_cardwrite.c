/* The fallback path in cardwrite.c, which no other test can reach.
 *
 * Dsxp_CommitWrite tries to rename the temporary over the target. Where
 * the filesystem allows that it is one directory write and the target
 * can never be missing. FatFs - which is what is actually under this on
 * a flashcart - refuses to rename onto a name that exists, so there is
 * a fallback that unlinks the target first and renames again.
 *
 * That fallback is the only code in this program that can leave the card
 * worse than it found it, and on a POSIX host it never runs: rename()
 * replaces an existing file, so the first attempt always succeeds. The
 * branch that matters on the real device was therefore the one branch
 * with no test over it, which is how it came to delete the temporary
 * holding the only remaining copy of the data.
 *
 * So rename() is shimmed here and made to fail on demand. Everything
 * else - the files, the unlink, the ordering - is real. */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* How many of the next rename() calls must fail. */
static int s_renameFails = 0;

static int FakeRename(const char *from, const char *to)
{
    if (s_renameFails > 0) { s_renameFails--; errno = EXDEV; return -1; }
    return rename(from, to);
}

/* Only cardwrite.c's own calls are redirected: every header it needs has
 * already been included above, so the macro cannot reach a declaration. */
#define rename FakeRename
#include "cardwrite.c"
#undef rename

static int g_fail = 0;
static void Check(const char *what, bool ok, const char *detail)
{
    printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

static bool Exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static bool Holds(const char *p, const char *want)
{
    char got[256] = "";
    FILE *f = fopen(p, "r");
    if (!f) return false;
    size_t n = fread(got, 1, sizeof(got) - 1, f);
    fclose(f);
    got[n] = '\0';
    return strcmp(got, want) == 0;
}

static void Put(const char *p, const char *text)
{
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

#define OLD "#EXTM3U\n/Music/original.mp3\n"
#define NEW "#EXTM3U\n/Music/replacement.mp3\n"

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    char target[512], tmp[600];
    snprintf(target, sizeof(target), "%s/staged.m3u", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", target);

    printf("staged writes: the FatFs fallback\n");
    mkdir(dir, 0777);

    /* The fallback itself: one rename refused, so the target is unlinked
     * and the second attempt succeeds. This is the ordinary path on a
     * flashcart and it had never been exercised anywhere. */
    {
        remove(target); remove(tmp);
        Put(target, OLD);

        DsxpWrite w;
        bool began = Dsxp_BeginWrite(&w, target);
        if (began) fputs(NEW, w.f);

        s_renameFails = 1;
        bool ok = Dsxp_CommitWrite(&w, target);

        char buf[128];
        snprintf(buf, sizeof(buf), "commit %s, temporary %s",
                 ok ? "succeeded" : "FAILED", Exists(tmp) ? "LEFT" : "gone");
        Check("a refused rename falls back to unlink-then-rename",
              began && ok && Holds(target, NEW) && !Exists(tmp), buf);
    }

    /* And the branch that can lose data: the unlink succeeds and the
     * rename after it fails anyway, so the target is gone and the
     * temporary is the only copy of the new contents in existence.
     *
     * It used to be deleted right here, which turned a failed save into
     * a playlist that no longer exists in any form. It has to survive,
     * so that a file called <name>.tmp can be renamed back by hand. */
    {
        remove(target); remove(tmp);
        Put(target, OLD);

        DsxpWrite w;
        bool began = Dsxp_BeginWrite(&w, target);
        if (began) fputs(NEW, w.f);

        s_renameFails = 2;                /* both attempts refused */
        bool ok = Dsxp_CommitWrite(&w, target);
        s_renameFails = 0;

        char buf[128];
        snprintf(buf, sizeof(buf), "commit %s, target %s, temporary %s",
                 ok ? "SUCCEEDED" : "failed",
                 Exists(target) ? "there" : "gone",
                 Exists(tmp) ? "kept" : "DELETED");
        Check("a failed rename keeps the only copy of the data",
              began && !ok && Exists(tmp) && Holds(tmp, NEW), buf);

        /* And the caller's usual response to a false commit must not
         * undo that. This is the half the disowned name protects. */
        Dsxp_AbortWrite(&w);
        snprintf(buf, sizeof(buf), "%s", Exists(tmp) ? "still there" : "DELETED");
        Check("and aborting afterwards does not remove it",
              Exists(tmp) && Holds(tmp, NEW), buf);

        remove(tmp);
    }

    /* An abort before any of that is the opposite case: nothing was
     * lost, so the temporary is litter and must go. */
    {
        remove(target); remove(tmp);
        Put(target, OLD);

        DsxpWrite w;
        if (Dsxp_BeginWrite(&w, target)) {
            fputs(NEW, w.f);
            Dsxp_AbortWrite(&w);
        }
        Check("an ordinary abort still clears its temporary up",
              !Exists(tmp) && Holds(target, OLD),
              Exists(tmp) ? "LEFT BEHIND" : "gone, original intact");
    }

    /* Something that is not a regular file standing where the target
     * should be belongs to somebody else, and is not cleared out of the
     * way however many renames fail. */
    {
        remove(target); remove(tmp);
        mkdir(target, 0777);

        DsxpWrite w;
        bool began = Dsxp_BeginWrite(&w, target);
        if (began) fputs(NEW, w.f);

        s_renameFails = 2;
        bool ok = Dsxp_CommitWrite(&w, target);
        s_renameFails = 0;

        struct stat st;
        bool stillDir = (stat(target, &st) == 0 && S_ISDIR(st.st_mode));
        Check("a directory in the way is refused, never deleted",
              began && !ok && stillDir,
              stillDir ? "still a directory" : "DESTROYED");

        remove(tmp);
        rmdir(target);
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
