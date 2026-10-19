#include "cardwrite.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

bool Dsxp_BeginWrite(DsxpWrite *w, const char *path)
{
    if (!w) return false;

    /* Emptied before anything can go wrong, not after.
     *
     * A DsxpWrite is a stack local at every call site, so until these
     * two lines run it holds whatever was on the stack. The checks
     * below used to come first, and a caller that aborted after one of
     * them failed would have handed remove() a path made of stack
     * rubbish. No caller does that today; the point is that none of
     * them should have to know not to. */
    w->f = NULL;
    w->tmp[0] = '\0';

    if (!path || !path[0]) return false;

    /* Beside the target, so the move at the end stays inside one
     * directory and one volume - a rename across either is a copy, and
     * a copy is the thing this exists to avoid. */
    if (snprintf(w->tmp, sizeof(w->tmp), "%s.tmp", path) >= (int)sizeof(w->tmp))
        return false;

    w->f = fopen(w->tmp, "w");
    return w->f != NULL;
}

bool Dsxp_CommitWrite(DsxpWrite *w, const char *path)
{
    if (!w || !w->f) return false;

    /* Everything out of the library, then everything out of the card's
     * own cache. Only the second of these makes the data survive the
     * power going off. */
    bool ok = (fflush(w->f) == 0);
    if (ok && fsync(fileno(w->f)) != 0) ok = false;
    if (fclose(w->f) != 0) ok = false;
    w->f = NULL;

    if (!ok) { remove(w->tmp); return false; }

    /* Over the top where the filesystem allows it, which is one
     * directory write and cannot leave the file missing. FatFs refuses a
     * rename onto a name that exists, so the fallback unlinks first -
     * that leaves a gap of two directory operations where the old file
     * is gone and the new one is not yet in place, which is far smaller
     * than the gap a truncating write leaves open for the whole of it,
     * and the complete new contents are sitting in the temporary the
     * entire time. */
    if (rename(w->tmp, path) == 0) return true;

    /* The fallback unlinks first, and only ever unlinks a plain file.
     *
     * remove() takes a directory as readily as a file, so a name that
     * happens to be a folder would be deleted by a routine whose whole
     * job is not to destroy things. Anything that is not a regular file
     * standing where the target should be is somebody else's, and the
     * write fails rather than clearing it out of the way. */
    struct stat st;
    if (stat(path, &st) == 0 && !S_ISREG(st.st_mode)) {
        remove(w->tmp);
        return false;
    }

    remove(path);
    if (rename(w->tmp, path) == 0) return true;

    /* The only branch in this file that can leave the card worse than
     * it found it: the unlink above succeeded and the rename after it
     * did not, so the target is gone and every byte of the replacement
     * is sitting in the temporary.
     *
     * So the temporary stays. A file called <name>.tmp beside where the
     * playlist used to be can be renamed back by hand; deleting it,
     * which is what this used to do, cannot be undone by anyone. The
     * name is cleared so that a caller's Dsxp_AbortWrite afterwards -
     * the ordinary response to a commit returning false - does not go
     * and remove the one surviving copy. */
    w->tmp[0] = '\0';
    return false;
}

void Dsxp_AbortWrite(DsxpWrite *w)
{
    if (!w) return;
    if (w->f) { fclose(w->f); w->f = NULL; }

    /* An empty name means Commit disowned the temporary because it is
     * the only copy of the data left - see the end of Dsxp_CommitWrite.
     * Aborting must not undo that. */
    if (w->tmp[0]) remove(w->tmp);
}

