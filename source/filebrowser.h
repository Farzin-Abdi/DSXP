#ifndef DSXP_FILEBROWSER_H
#define DSXP_FILEBROWSER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cardwrite.h"
#include <stddef.h>

#define FB_MAX_ENTRIES   256
#define FB_NAME_LEN      128
#define FB_PATH_LEN      512
/* Most rows the list can show. The number actually shown depends on the
 * chosen text size, so this is the array bound, not the count. */
#define FB_MAX_VISIBLE_ROWS 9

/* How many rows the list is currently showing. Set by the skin when the
 * text size changes; the browser needs it to keep the selection on
 * screen, which is the one thing that breaks silently if the two
 * disagree. */
void FileBrowser_SetVisibleRows(int rows);
int  FileBrowser_VisibleRows(void);

typedef struct {
    char name[FB_NAME_LEN];
    bool isDir;
    bool isPlaylist;   /* .m3u - opens as a queue rather than as audio */
    bool isLiked;      /* the liked list: sorts first and draws a heart */
    bool marked;       /* picked out for the playlist being built */

    /* Which line of the playlist file this row came from, counting from
     * one. Zero for anything that did not come from a playlist.
     *
     * It exists so that editing a list can rewrite the file by dropping
     * named lines instead of writing the rows back out. The rows are
     * not the file: a path too long for `name`, or anything past
     * FB_MAX_ENTRIES, never becomes a row - and a rewrite built from
     * the rows deleted every one of those, so removing one track from
     * a long list silently took the invisible ones with it. */
    uint32_t srcLine;
} FbEntry;

typedef struct {
    char cwd[FB_PATH_LEN];
    FbEntry entries[FB_MAX_ENTRIES];
    int count;
    int selected;      /* index into entries[] */
    int scrollTop;     /* first visible row */
    bool truncated;    /* folder had more than FB_MAX_ENTRIES entries */

    /* One folder and nothing else: no "..", no subfolders, only the
     * playlists in it.
     *
     * The Faves tab is this folder, and it is a list of saved lists
     * rather than a place on the card - climbing out of it with ".." or
     * descending into something would land the user in the file manager
     * by way of a tab that is not the file manager. */
    bool locked;

    /* When set, the list is the contents of a playlist rather than of a
     * directory. Entering one instead of playing it is what makes a
     * playlist editable at all: the same list, the same marking, and the
     * same buttons then mean add and remove. */
    bool inPlaylist;
    char playlistName[FB_NAME_LEN];   /* the .m3u, inside cwd */
} FileBrowser;

/* Mounts the card filesystem (libfat) and lists `startPath`, e.g. "/".
 * Returns false if the card could not be mounted. */
bool FileBrowser_Init(FileBrowser *fb, const char *startPath);

/* Marks the selected entry for inclusion in a new playlist.
 *
 * A folder can be marked, and stands for every track in it - expanded,
 * one level deep and in the order the list shows them, when the playlist
 * is written. Playlists cannot be marked: a playlist of playlists is a
 * rabbit hole with no obvious bottom. Nor can ".." - it is the way out,
 * not a folder. */
void FileBrowser_ToggleMark(FileBrowser *fb);
int  FileBrowser_MarkedCount(const FileBrowser *fb);
void FileBrowser_ClearMarks(FileBrowser *fb);

/* True where the rows are tracks rather than saved lists.
 *
 * A folder is, and so is a playlist opened to show what is in it -
 * including one opened from the Faves tab, which stays locked while it
 * is open. Only the Faves tab at its top level is not: there every row
 * is a list, and there is nothing to mark or add.
 *
 * The controls that build and edit lists are drawn and hit-tested from
 * this rather than from which tab is showing. Gated on the tab, they
 * vanished the moment a playlist was opened from Faves - which is the
 * one place playlists live, so marking several tracks to remove them
 * together, and adding more to the list you are looking at, could not
 * be reached at all. */
static inline bool FileBrowser_RowsAreTracks(const FileBrowser *fb)
{
    return !fb->locked || fb->inPlaylist;
}

/* Shows the contents of a playlist in place of the folder. Leaving it -
 * via ".." - returns to the folder it lives in. */
bool FileBrowser_EnterPlaylist(FileBrowser *fb, const char *name);
void FileBrowser_LeavePlaylist(FileBrowser *fb);

/* Rewrites the open playlist without its marked entries, and reloads. */
bool FileBrowser_RemoveMarkedFromPlaylist(FileBrowser *fb);

/* Appends the marked entries to a playlist in `dir` rather than in the
 * folder being browsed.
 *
 * Adding to an existing list needs this, because a list and the music
 * that goes into it do not live in the same place: every list is in the
 * playlist folder and the tracks are wherever the user keeps them. The
 * marks still come from `fb`; only the destination is elsewhere. */
bool FileBrowser_AppendToPlaylistIn(const FileBrowser *fb, const char *dir,
                                    const char *listName);

/* Every playlist in `dir`, by name and in the order the browser would
 * show them. Returns how many were written, at most `max`.
 *
 * A scan of its own rather than a read of fb->entries: the playlists are
 * in the playlist folder and the browser is in a music folder, so the
 * entry table has none of them in it. */
int FileBrowser_ListPlaylistsIn(const char *dir, char names[][FB_NAME_LEN],
                                int max);

/* Writes the marked entries to `<dir>/<name>.m3u`, creating `dir` if it
 * is not there yet. */
bool FileBrowser_SavePlaylistIn(const FileBrowser *fb, const char *dir,
                                const char *name, char *outFile, size_t outLen);

/* Removes a playlist from the card. Only playlists: deleting music by
 * accident is not a mistake worth making reachable. */
bool FileBrowser_DeletePlaylist(const FileBrowser *fb, int index);

/* Everything this player keeps on the card lives under one folder.
 *
 * It used to be two things at the root - dsxp.cfg beside the music, and
 * a DSXP_Playlists folder next to it - which is two pieces of clutter in
 * the place people keep their own files. One folder is tidier, and it
 * also means the Files tab has exactly one name to hide rather than
 * having to know about the settings file as well.
 *
 * Overridable so the host tests can point it somewhere they are allowed
 * to write. The playlist folder derives from it, so a test that moves
 * the base moves both. */
#ifndef DSXP_DIR
#define DSXP_DIR "/dsxp_info"
#endif

#ifndef PLAYLIST_DIR
#define PLAYLIST_DIR DSXP_DIR "/playlists"
#endif

/* And the settings file, named here rather than in settings.c so that
 * the one place that knows the layout is the one that defines it. */
#define DSXP_CFG_PATH DSXP_DIR "/dsxp.cfg"

/* Where the same two things used to be, so that a card written by an
 * earlier build can be brought across once. */
#ifndef DSXP_LEGACY_CFG
#define DSXP_LEGACY_CFG "/dsxp.cfg"
#endif
#ifndef DSXP_LEGACY_PLAYLISTS
#define DSXP_LEGACY_PLAYLISTS "/DSXP_Playlists"
#endif

/* Moves a card laid out by an earlier build into DSXP_DIR: the settings
 * file and any playlists, and nothing else. Safe to call every start -
 * it does nothing once there is nothing left to move, and it never
 * overwrites a file already in the new place. */
void FileBrowser_MigrateAppFolder(void);

/* Re-reads the current directory: subfolders and .mp3 files only, "."
 * skipped, ".." synthesized at index 0 unless already at the root. If
 * `selectName` is non-NULL and present in the listing, it starts out
 * selected. */
/* Opens a folder as a list of playlists: no "..", no subfolders, no
 * loose audio. What the Faves tab shows. */
bool FileBrowser_InitLocked(FileBrowser *fb, const char *startPath);

void FileBrowser_Refresh(FileBrowser *fb, const char *selectName);

void FileBrowser_MoveSelection(FileBrowser *fb, int delta);
void FileBrowser_SetSelection(FileBrowser *fb, int index);

/* Descends into the selected folder (or back out of it via "..") and
 * returns false; returns true if the selection is a playable file. */
bool FileBrowser_EnterSelected(FileBrowser *fb);

/* True if the selected entry is a folder. */
bool FileBrowser_SelectedIsDir(const FileBrowser *fb);

/* Moves up one level. No-op at the root. */
void FileBrowser_GoUp(FileBrowser *fb);

#endif /* DSXP_FILEBROWSER_H */
