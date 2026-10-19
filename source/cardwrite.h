#ifndef DSXP_CARDWRITE_H
#define DSXP_CARDWRITE_H

#include <stdbool.h>
#include <stdio.h>

/* Replacing a file on the card without being able to destroy it.
 *
 * fopen(path, "w") truncates before a single byte is written, so a write
 * that fails - a card that has gone read-only, a console switched off
 * part way through - leaves the old contents already gone and the new
 * ones never arrived. For the settings that costs some preferences. For
 * the liked list or a playlist it is the user's own work, and there is
 * no second copy of it anywhere on the card.
 *
 * So a replacement is staged. Begin opens a temporary beside the target,
 * the caller writes to that, and Commit pushes it to the card and moves
 * it into place. Until that last step the original is untouched and
 * complete; if anything goes wrong the caller Aborts and nothing has
 * changed at all.
 *
 * Commit flushes with fsync before the move rather than trusting fclose:
 * the whole point of moving it last is that the move happens after the
 * data is really on the card, and fclose only promises to hand it to the
 * filesystem.
 *
 * Its own file rather than part of the browser because three unrelated
 * modules replace files - the settings, the favourites and the playlists
 * - and none of them should have to link the other two to do it safely.
 */
#define DSXP_WRITE_PATH_MAX 640

typedef struct {
    FILE *f;
    char  tmp[DSXP_WRITE_PATH_MAX];
} DsxpWrite;

bool Dsxp_BeginWrite(DsxpWrite *w, const char *path);

/* Pushes the staged contents to the card and moves them into place.
 *
 * False means the original is still there and nothing was lost - with
 * one exception it cannot avoid. Where the filesystem refuses to rename
 * onto a name that already exists, the old file has to be unlinked
 * first, and if the rename then fails the target is gone. In that one
 * case the temporary is deliberately left on the card, holding the
 * complete new contents, and `w` is marked so that a following
 * Dsxp_AbortWrite will not remove it. */
bool Dsxp_CommitWrite(DsxpWrite *w, const char *path);

/* Throws the staged write away. Safe to call after a failed commit. */
void Dsxp_AbortWrite(DsxpWrite *w);

#endif /* DSXP_CARDWRITE_H */
