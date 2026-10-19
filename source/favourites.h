/* Liked tracks, kept as a playlist.
 *
 * A favourite is a line in an M3U in PLAYLIST_DIR, which means the
 * feature needs no new file format, no new parser, and no second place on
 * the card for this program's data - the folder was already there and the
 * Faves tab is that folder listed. It also means a favourites list can be
 * copied to a computer and opened in anything.
 *
 * Paths are absolute and compared as written. Two names for one file - a
 * relative path and an absolute one - would both be storable and would
 * not match each other, so everything that goes in here comes from the
 * same place the player queues from.
 */
#ifndef DSXP_FAVOURITES_H
#define DSXP_FAVOURITES_H

#include <stdbool.h>

/* The list itself, inside the folder playlists already live in. */
/* "Liked", because that is the word on the button that fills it and the
 * name the Faves tab shows it under - the file and the feature should
 * not need translating between. */
#define FAVOURITES_NAME "Liked"

#define FAVOURITES_PATH PLAYLIST_DIR "/" FAVOURITES_NAME ".m3u"

/* Whether `path` is in the list. Cheap enough to call while drawing: the
 * list is read once and cached, and only re-read when it is written. */
bool Favourites_Contains(const char *path);

/* Adds or removes `path`, and returns what it is now. Writing is
 * suppressed when card writes are off, in which case the answer is
 * whatever it already was. */
bool Favourites_Toggle(const char *path);

/* Drops the cache, so the next question re-reads the card. Called when
 * something else may have changed the file - a playlist edited in the
 * browser, or the card write setting being turned back on. */
void Favourites_Invalidate(void);

/* How many are in the list, for the tab to say so. */
int Favourites_Count(void);

#endif /* DSXP_FAVOURITES_H */
