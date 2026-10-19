#include "filebrowser.h"
#include "favourites.h"
#include "settings.h"

#include <ctype.h>
#include <dirent.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>   /* rmdir, for the one-time move out of the root */

static int s_visibleRows = FB_MAX_VISIBLE_ROWS;

void FileBrowser_ToggleMark(FileBrowser *fb)
{
    if (fb->selected < 0 || fb->selected >= fb->count) return;

    FbEntry *e = &fb->entries[fb->selected];

    /* A folder can be marked; it stands for every track in it.
     *
     * Adding an album a track at a time was nine presses for nine
     * tracks, on a list that only shows nine rows - the common case for
     * building a list is "this whole folder", and there was no way to
     * say it. The expansion happens where the list is written, so the
     * marks stay a selection and do not become a snapshot that goes
     * stale if the folder changes underneath them.
     *
     * ".." is not a folder here, it is the way out, and a playlist still
     * cannot be marked - a playlist of playlists is a rabbit hole with
     * no obvious bottom. */
    if (e->isPlaylist) return;
    if (e->isDir && strcmp(e->name, "..") == 0) return;

    e->marked = !e->marked;
}

int FileBrowser_MarkedCount(const FileBrowser *fb)
{
    int n = 0;
    for (int i = 0; i < fb->count; i++)
        if (fb->entries[i].marked) n++;
    return n;
}

void FileBrowser_ClearMarks(FileBrowser *fb)
{
    for (int i = 0; i < fb->count; i++) fb->entries[i].marked = false;
}

/* ---- playlists as folders ---------------------------------------- */

static void JoinInto(char *out, size_t n, const char *dir, const char *name)
{
    size_t len = strlen(dir);
    bool slash = (len > 0 && dir[len - 1] == '/');
    snprintf(out, n, "%s%s%s", dir, slash ? "" : "/", name);
}

bool FileBrowser_EnterPlaylist(FileBrowser *fb, const char *name)
{
    /* The name is copied before anything in `fb` is touched, because
     * the obvious way to call this is with a pointer into fb->entries -
     * that is where the name the user just tapped lives - and the first
     * thing the fill below does is overwrite entries[0] with the ".."
     * row, then entries[1..] with the lines of the file.
     *
     * So `name` was a dangling view of memory this function had already
     * reused. The folder was still opened correctly, because that
     * happens first; it was the playlistName saved at the very end that
     * came back wrong - and everything that edits a list works from
     * that name. For a playlist at row 0, which the liked list always
     * is, it read back as ".." every single time, and so removing a
     * track from it could never find the file to rewrite.
     *
     * Fixed here rather than at the call site: "do not hand me a
     * pointer into the structure you are handing me" is not a
     * precondition a caller should have to know about. */
    char wanted[FB_NAME_LEN];
    snprintf(wanted, sizeof(wanted), "%s", name);
    name = wanted;

    char path[FB_PATH_LEN + FB_NAME_LEN];
    JoinInto(path, sizeof(path), fb->cwd, name);

    FILE *f = fopen(path, "r");
    if (!f) return false;

    fb->count = 0;
    fb->selected = 0;
    fb->scrollTop = 0;
    fb->truncated = false;

    /* ".." leaves the playlist rather than the folder, so there is
     * always a way back out of a list that turned out to be the wrong
     * one. */
    FbEntry *up = &fb->entries[fb->count++];
    memset(up, 0, sizeof(*up));
    strcpy(up->name, "..");
    up->isDir = true;

    char line[FB_NAME_LEN * 2];
    uint32_t lineNo = 0;

    while (fb->count < FB_MAX_ENTRIES && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);

        /* A line too long for the buffer is finished and thrown away
         * whole, rather than coming back as a second line.
         *
         * Without this, a 300-character path arrived as 255 characters
         * - dropped for being too long - followed by a 45-character
         * remainder, which is short enough to be accepted and so became
         * a row naming the tail end of somebody's path. It played
         * nothing, and a rewrite wrote it back in place of the real
         * line. One line of the file is one line here. */
        bool whole = (n == 0 || line[n - 1] == '\n');
        if (!whole) { int c; while ((c = fgetc(f)) != EOF && c != '\n') { } }

        lineNo++;

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        for (size_t i = 0; i < n; i++) if (line[i] == '\\') line[i] = '/';
        if (!whole || n >= FB_NAME_LEN) continue;

        FbEntry *e = &fb->entries[fb->count++];
        memset(e, 0, sizeof(*e));
        memcpy(e->name, line, n + 1);
        e->srcLine = lineNo;
    }
    fclose(f);

    fb->inPlaylist = true;
    snprintf(fb->playlistName, sizeof(fb->playlistName), "%s", name);
    fb->selected = (fb->count > 1) ? 1 : 0;
    return true;
}

void FileBrowser_LeavePlaylist(FileBrowser *fb)
{
    char was[FB_NAME_LEN];
    snprintf(was, sizeof(was), "%s", fb->playlistName);

    fb->inPlaylist = false;
    fb->playlistName[0] = '\0';
    FileBrowser_Refresh(fb, was);      /* land back on the playlist itself */
}

/* Whether line `lineNo` of the playlist file belongs to a marked row. */
static bool LineIsMarked(const FileBrowser *fb, uint32_t lineNo)
{
    if (lineNo == 0) return false;
    for (int i = 0; i < fb->count; i++)
        if (fb->entries[i].marked && fb->entries[i].srcLine == lineNo)
            return true;
    return false;
}

/* Copies the playlist back out with the marked rows' lines left out.
 *
 * A copy, deliberately, rather than the rows written back. The rows are
 * only the part of the file this browser could show: a path longer than
 * FB_NAME_LEN never became one, nor did anything past FB_MAX_ENTRIES,
 * nor a comment line somebody's desktop player put there. Rebuilding
 * the file from the rows deleted every one of those, so removing a
 * single track from a long list quietly took the invisible entries with
 * it - measured at one removal costing a 172-character path that was on
 * the card the whole time and simply never on screen.
 *
 * So the original is read line by line and written through unchanged,
 * except for the numbered lines the marked rows came from. Anything
 * this code did not understand survives exactly as it was, which is the
 * only honest thing to do with a file somebody else may have written. */
static bool RewritePlaylist(FileBrowser *fb, bool dropMarked)
{
    /* Every path that writes to the card asks first; see cardWrites. */
    if (Settings_CardWritesBlocked()) return false;

    char path[FB_PATH_LEN + FB_NAME_LEN];
    JoinInto(path, sizeof(path), fb->cwd, fb->playlistName);

    FILE *in = fopen(path, "r");
    if (!in) return false;

    DsxpWrite w;
    if (!Dsxp_BeginWrite(&w, path)) { fclose(in); return false; }

    char line[FB_NAME_LEN * 2];
    uint32_t lineNo = 0;
    bool tail = false;          /* inside a line too long for the buffer */

    while (fgets(line, sizeof(line), in)) {
        size_t n = strlen(line);
        bool whole = (n == 0 || line[n - 1] == '\n');

        /* The rest of an over-long line. It cannot have produced a row,
         * so it is never dropped - copy it and keep going. */
        if (tail) { fputs(line, w.f); tail = !whole; continue; }

        lineNo++;
        if (!whole) { fputs(line, w.f); tail = true; continue; }

        if (dropMarked && LineIsMarked(fb, lineNo)) continue;
        fputs(line, w.f);
    }
    fclose(in);

    if (ferror(w.f)) { Dsxp_AbortWrite(&w); return false; }
    if (!Dsxp_CommitWrite(&w, path)) return false;

    /* The liked list is one of these files, so any rewrite might have
     * been it. See the note on FileBrowser_AppendToPlaylistIn. */
    Favourites_Invalidate();
    return true;
}

bool FileBrowser_RemoveMarkedFromPlaylist(FileBrowser *fb)
{
    if (!fb->inPlaylist) return false;
    if (FileBrowser_MarkedCount(fb) == 0) return false;

    if (!RewritePlaylist(fb, true)) return false;

    char name[FB_NAME_LEN];
    snprintf(name, sizeof(name), "%s", fb->playlistName);
    return FileBrowser_EnterPlaylist(fb, name);
}

static bool HasAudioExtension(const char *name);

/* Joins two path pieces, or refuses.
 *
 * JoinInto truncates, which is right where the pieces are struct fields
 * and the buffer is sized from the same constants they are. The folder
 * expansion below is a level deeper than that - cwd, then the folder,
 * then the track - so a path can genuinely be too long to write, and a
 * truncated one is a line in a playlist that points at the wrong file or
 * at nothing. Refusing drops the track; truncating would keep it and
 * make it a broken entry the user has to work out for themselves. */
static bool JoinChecked(char *out, size_t n, const char *a, const char *b)
{
    const size_t la = strlen(a), lb = strlen(b);
    const bool slash = (la > 0 && a[la - 1] == '/');

    if (la + (slash ? 0u : 1u) + lb + 1u > n) return false;

    memcpy(out, a, la);
    size_t at = la;
    if (!slash) out[at++] = '/';
    memcpy(out + at, b, lb + 1);
    return true;
}

/* Names collected from one folder while it is being written out.
 *
 * Static because 32K is not a stack, and safe to share because every
 * caller is the UI thread finishing a playlist - the decoder never comes
 * through here. */
static char s_folderNames[FB_MAX_ENTRIES][FB_NAME_LEN];

static int CompareNames(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

/* One marked folder's tracks, written into an open playlist.
 *
 * Collected and sorted rather than written in the order readdir hands
 * them over: a FAT directory comes back in the order its entries were
 * created, which for an album copied by a program that walked its own
 * list is not track order. The browser sorts what it shows, and a folder
 * added to a list should arrive in the order the user saw.
 *
 * One level deep. A folder of folders would need a rule for what to do
 * about the ones with no audio in them, and "the album I am looking at"
 * is what the mark is for.
 *
 * `relPrefix` is the folder's own name when the playlist sits beside it,
 * so the lines stay relative and the list stays portable; NULL writes
 * them absolute. */
/* What is already in the playlist being written to, so that adding a
 * track that is in it does not put it in twice.
 *
 * Held as hashes of the path lines rather than the lines themselves: a
 * playlist file has no length limit and this program has no allocator.
 * A hash that matches is confirmed against the file before anything is
 * skipped, so a collision costs one re-read and can never lose a track
 * the user asked for.
 *
 * It also takes the hash of every line this write adds, which is what
 * catches the other way of getting a duplicate: marking a folder and
 * then also marking a track inside it. */
#define SEEN_MAX 1024
static uint64_t s_seen[SEEN_MAX];
static int      s_seenCount;
static bool     s_seenOverflow;     /* longer than the table: stop deduping */

/* Sixty-four bits, and a match is taken at face value.
 *
 * The first version kept 32-bit hashes and confirmed a match by
 * reopening the playlist to look for the line - a second handle on a
 * file this code already had open for writing. On FatFs that is not
 * merely untidy: two handles to one file, one of them written through,
 * is how a directory entry gets written back stale, and the cost of
 * that lands on every file in the folder rather than on this one.
 *
 * It is not worth a re-read to confirm what sixty-four bits settle.
 * Across a thousand lines the chance that two different paths collide
 * is about one in forty million million. */
static uint64_t HashLine(const char *s)
{
    uint64_t h = 1469598103934665603ull;        /* FNV-1a, 64-bit */
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}

/* Reads the lines already in `path`. An absent file is not an error -
 * that is simply an empty list, which is the state save-as-new starts
 * from. */
static void SeenLoad(const char *path)
{
    s_seenCount = 0;
    s_seenOverflow = false;
    if (!path || !path[0]) return;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[FB_PATH_LEN + FB_NAME_LEN];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!n || line[0] == '#') continue;

        if (s_seenCount >= SEEN_MAX) { s_seenOverflow = true; break; }
        s_seen[s_seenCount++] = HashLine(line);
    }
    fclose(f);
}

/* Writes one playlist line unless it is already in the list.
 *
 * Every line this call writes goes into the table alongside the ones
 * that were already in the file, which is what catches the second way
 * of getting a duplicate: a marked folder and a marked track inside
 * that folder resolve to the same path.
 *
 * Nothing here reads the file. The caller has it open for writing, and
 * opening it again to check would be two handles on one file. */
static void WriteEntryOnce(FILE *f, const char *name, const char *entry)
{
    const uint64_t h = HashLine(entry);

    for (int i = 0; i < s_seenCount; i++)
        if (s_seen[i] == h) return;

    if (s_seenCount < SEEN_MAX) s_seen[s_seenCount++] = h;
    else                        s_seenOverflow = true;

    fprintf(f, "#EXTINF:-1,%s\n%s\n", name, entry);
}

static void WriteFolderTracks(FILE *f, const char *folder,
                              const char *relPrefix)
{
    DIR *d = opendir(folder);
    if (!d) return;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < FB_MAX_ENTRIES) {
        /* Dotfiles for the same reason the scan skips them: a Mac leaves
         * "._track.mp3" beside every file, and it ends in .mp3. */
        if (ent->d_name[0] == '.') continue;
        if (!HasAudioExtension(ent->d_name)) continue;
        snprintf(s_folderNames[n], FB_NAME_LEN, "%s", ent->d_name);
        n++;
    }
    closedir(d);

    qsort(s_folderNames, (size_t)n, FB_NAME_LEN, CompareNames);

    for (int i = 0; i < n; i++) {
        /* A folder deep enough to fill FB_PATH_LEN, plus the folder's own
         * name, plus a track's: the one line here that is two names below
         * cwd rather than one. */
        char entry[FB_PATH_LEN + 2 * FB_NAME_LEN];
        if (!JoinChecked(entry, sizeof(entry),
                         relPrefix ? relPrefix : folder, s_folderNames[i]))
            continue;

        WriteEntryOnce(f, s_folderNames[i], entry);
    }
}

/* Every marked entry of `fb`, written into an open playlist that lives
 * in `dir`.
 *
 * Shared by save-as-new and append: the two used to carry a copy of this
 * loop each, and only one of them would have learned that a mark can now
 * be a folder.
 *
 * A track from the playlist's own folder is written by name, which keeps
 * the list portable. One from anywhere else has no relative name, so it
 * goes in absolute - a wrong relative path would simply fail to open
 * later with nothing to say why. */
static void WriteMarkedEntries(FILE *f, const FileBrowser *fb, const char *dir)
{
    const bool sameDir = (strcmp(dir, fb->cwd) == 0);

    for (int i = 0; i < fb->count; i++) {
        const FbEntry *e = &fb->entries[i];
        if (!e->marked) continue;

        if (e->isDir) {
            char folder[FB_PATH_LEN + FB_NAME_LEN];
            if (!JoinChecked(folder, sizeof(folder), fb->cwd, e->name))
                continue;
            WriteFolderTracks(f, folder, sameDir ? e->name : NULL);
            continue;
        }

        char entry[FB_PATH_LEN + FB_NAME_LEN];
        if (sameDir) snprintf(entry, sizeof(entry), "%s", e->name);
        else         JoinInto(entry, sizeof(entry), fb->cwd, e->name);

        WriteEntryOnce(f, e->name, entry);
    }
}

bool FileBrowser_AppendToPlaylistIn(const FileBrowser *fb, const char *dir,
                                    const char *listName)
{
    /* Every path that writes to the card asks first; see cardWrites. */
    if (Settings_CardWritesBlocked()) return false;

    if (FileBrowser_MarkedCount(fb) == 0) return false;
    if (!dir || !dir[0] || !listName || !listName[0]) return false;

    char path[FB_PATH_LEN + FB_NAME_LEN];
    JoinInto(path, sizeof(path), dir, listName);

    /* What is in there already, so that adding a track twice adds it
     * once. The heart could never duplicate a like - it toggles - but
     * the picker reaches the same files through here, so liking a track
     * and then adding it to Liked from the file list put it in the list
     * two times with nothing to say so. */
    SeenLoad(path);

    /* Staged, like every other write here.
     *
     * This was the one that was not: it opened the list with "a" and
     * wrote onto the end of it. Append does not truncate, so the old
     * contents were never at risk - but a console that stops partway
     * leaves half a line welded to the file, and half a path is an
     * entry that opens nothing and that the next rewrite carries
     * forward. There was no fsync either, so the card could be told
     * about a longer file than it had actually been given.
     *
     * So the original is copied into the temporary byte for byte, the
     * new lines go after it, and the whole thing is moved into place
     * once it is really on the card. Copying rather than reformatting
     * keeps the property the old comment was about: whatever else is in
     * the file is the user's and comes through untouched.
     *
     * The read handle is on `path` and the write handle is on
     * `path.tmp` - two different files, which is the whole point. */
    DsxpWrite w;
    if (!Dsxp_BeginWrite(&w, path)) return false;

    char last = '\n';
    FILE *in = fopen(path, "r");
    if (in) {
        char chunk[512];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) {
            if (fwrite(chunk, 1, n, w.f) != n) break;
            last = chunk[n - 1];
        }
        fclose(in);
    }

    /* A list whose last line has no newline on it would otherwise have
     * the first new entry joined onto the end of it. */
    if (last != '\n') fputc('\n', w.f);

    WriteMarkedEntries(w.f, fb, dir);

    if (ferror(w.f) || !Dsxp_CommitWrite(&w, path)) {
        Dsxp_AbortWrite(&w);
        return false;
    }

    /* Here rather than in the callers, because the callers forgot.
     *
     * Favourites keeps the liked list in memory and answers the heart
     * from there; the file is the truth and that copy is dropped
     * whenever anything might have changed it. Three of the paths that
     * rewrite a playlist did not drop it, and the liked list is a
     * playlist - so editing it through the list editor left the copy
     * holding tracks the file no longer had, and the next heart tapped
     * anywhere wrote that copy back over the file. Every track removed
     * by hand came back.
     *
     * Whether this particular file was the liked one is not worth
     * working out: dropping the copy costs one file open next time it
     * is asked for, and being wrong costs the user's edit. */
    Favourites_Invalidate();
    return true;
}

/* Writing and removing playlist files lives here with the reading, not
 * in the player: none of it is about playback, and keeping it beside
 * FileBrowser_EnterPlaylist means the rules for what a path in a list
 * means are written once. */
bool FileBrowser_SavePlaylistIn(const FileBrowser *fb, const char *dir,
                           const char *name, char *outFile, size_t outLen)
{
    /* Every path that writes to the card asks first; see cardWrites. */
    if (Settings_CardWritesBlocked()) return false;

    if (FileBrowser_MarkedCount(fb) == 0) return false;
    if (!name || !name[0]) return false;

    /* Made on demand: a "save it in Playlists" option that fails because
     * Playlists does not exist yet is a worse answer than just creating
     * it. mkdir failing because it is already there is the normal case,
     * not an error. */
    mkdir(DSXP_DIR, 0777);
    mkdir(dir, 0777);

    char file[FB_NAME_LEN];
    snprintf(file, sizeof(file), "%s.m3u", name);

    char path[FB_PATH_LEN + FB_NAME_LEN];
    JoinInto(path, sizeof(path), dir, file);

    /* A name that is already taken is added to, never written over.
     *
     * This used to replace it. Typing the name of a list you already
     * have - which is the obvious thing to do when you mean "put these
     * in my Focus list" - threw that list away and left only the tracks
     * that happened to be marked, with nothing asked and nothing said.
     * Typing "Liked" did it to the liked list.
     *
     * Adding is what the name was reaching for, the duplicate gate
     * means a track already in there is not listed twice, and the
     * caller shows the list afterwards so the result is on screen
     * rather than taken on trust. */
    {
        struct stat st;
        if (stat(path, &st) == 0) {
            if (!FileBrowser_AppendToPlaylistIn(fb, dir, file)) return false;
            if (outFile && outLen) snprintf(outFile, outLen, "%s", file);
            return true;
        }
    }

    DsxpWrite w;
    if (!Dsxp_BeginWrite(&w, path)) return false;
    FILE *f = w.f;

    /* A new file, so nothing is in it - but a marked folder and a marked
     * track inside that folder are two routes to the same line, and the
     * gate catches that too. The name it is told to watch is the
     * temporary, which is what is actually being written. */
    SeenLoad(w.tmp);

    /* The extended header is what tells desktop players the '#' lines are
     * titles rather than comments; harmless to anything that does not
     * care. */
    fprintf(f, "#EXTM3U\n");
    WriteMarkedEntries(f, fb, dir);

    bool ok = !ferror(f) && Dsxp_CommitWrite(&w, path);
    if (!ok) Dsxp_AbortWrite(&w);
    if (ok && outFile && outLen) snprintf(outFile, outLen, "%s", file);

    /* A new list can be named over an existing one, and the liked list
     * is an existing one. */
    if (ok) Favourites_Invalidate();
    return ok;
}

bool FileBrowser_DeletePlaylist(const FileBrowser *fb, int index)
{
    /* Every path that writes to the card asks first; see cardWrites. */
    if (Settings_CardWritesBlocked()) return false;

    if (index < 0 || index >= fb->count) return false;
    if (!fb->entries[index].isPlaylist) return false;

    char path[FB_PATH_LEN + FB_NAME_LEN];
    JoinInto(path, sizeof(path), fb->cwd, fb->entries[index].name);

    /* Only ever a plain file, asked of the card rather than of the row.
     *
     * isPlaylist comes from a directory scan that may be minutes old -
     * the card can be changed under a browser that is just sitting
     * there - and remove() takes a directory as readily as a file. The
     * staged-write path learned this already; this one is the other
     * place that deletes something, and it was going on what the list
     * last saw. */
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    if (remove(path) != 0) return false;
    Favourites_Invalidate();
    return true;
}

void FileBrowser_SetVisibleRows(int rows)
{
    if (rows < 1) rows = 1;
    if (rows > FB_MAX_VISIBLE_ROWS) rows = FB_MAX_VISIBLE_ROWS;
    s_visibleRows = rows;
}

int FileBrowser_VisibleRows(void) { return s_visibleRows; }

/* True for the playlist container this player understands. M3U is the
 * one worth supporting: it is a plain list of paths, every desktop music
 * program writes it, and it needs no parser worth the name. */
static bool HasM3uExtension(const char *name)
{
    size_t len = strlen(name);
    if (len >= 4 && !strcasecmp(name + len - 4, ".m3u"))  return true;
    if (len >= 5 && !strcasecmp(name + len - 5, ".m3u8")) return true;
    return false;
}

int FileBrowser_ListPlaylistsIn(const char *dir, char names[][FB_NAME_LEN],
                                int max)
{
    if (!dir || !dir[0] || max <= 0) return 0;

    DIR *d = opendir(dir);
    if (!d) return 0;          /* the folder need not exist yet */

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max) {
        /* Dotfiles for the same reason every other scan skips them: a
         * Mac leaves "._Evening.m3u" beside every file, and it ends in
         * .m3u. */
        if (ent->d_name[0] == '.') continue;
        if (!HasM3uExtension(ent->d_name)) continue;

        /* Skipped rather than truncated. A name cut to fit is a name
         * that opens nothing, and offering it in the picker would be
         * offering a list that cannot be added to. */
        if (strlen(ent->d_name) >= FB_NAME_LEN) continue;

        memcpy(names[n], ent->d_name, strlen(ent->d_name) + 1);
        n++;
    }
    closedir(d);

    /* Sorted, because a FAT directory comes back in creation order and a
     * list of lists that reorders itself as lists are made is one the
     * user cannot learn the shape of. */
    qsort(names, (size_t)n, FB_NAME_LEN, CompareNames);
    return n;
}

/* The extensions the decoder can open. A table of one, because it is a
 * table that four other formats passed through and might again - the
 * shape is worth keeping even while MP3 is the only entry. */
static const char *const kAudioExt[] = { ".mp3" };

static bool HasAudioExtension(const char *name)
{
    size_t len = strlen(name);

    for (size_t e = 0; e < sizeof(kAudioExt) / sizeof(kAudioExt[0]); e++) {
        size_t elen = strlen(kAudioExt[e]);
        if (len <= elen) continue;
        if (!strcasecmp(name + len - elen, kAudioExt[e])) return true;
    }
    return false;
}

static int CompareEntries(const void *a, const void *b)
{
    const FbEntry *ea = (const FbEntry *)a;
    const FbEntry *eb = (const FbEntry *)b;

    /* The liked list first, always. It is the one row in the Faves tab
     * the user did not make and cannot rename, and burying it among the
     * playlists in alphabetical order means where it sits depends on what
     * else they have saved. */
    if (ea->isLiked != eb->isLiked) return ea->isLiked ? -1 : 1;

    if (ea->isDir != eb->isDir) return ea->isDir ? -1 : 1;  /* folders first */
    return strcasecmp(ea->name, eb->name);
}

static void ScrollToSelection(FileBrowser *fb)
{
    if (fb->selected < fb->scrollTop)
        fb->scrollTop = fb->selected;
    if (fb->selected >= fb->scrollTop + s_visibleRows)
        fb->scrollTop = fb->selected - s_visibleRows + 1;

    int maxTop = fb->count - s_visibleRows;
    if (maxTop < 0) maxTop = 0;
    if (fb->scrollTop > maxTop) fb->scrollTop = maxTop;
    if (fb->scrollTop < 0) fb->scrollTop = 0;
}

static bool IsAtRoot(const FileBrowser *fb)
{
    return fb->cwd[0] == '/' && fb->cwd[1] == '\0';
}

/* Is this entry the folder the Faves tab is made of?
 *
 * The lists have to live somewhere on the card, but the folder they live
 * in is a tab now, and showing it in Files as well is a second door onto
 * the same room - one that lands the user in a locked browser they then
 * have to climb out of.
 *
 * Matched against the whole path and not just the name, so a folder that
 * happens to share the name somewhere else on the card is left alone,
 * and read from PLAYLIST_DIR at runtime so an override still works. */
static bool IsPlaylistFolder(const FileBrowser *fb, const char *name)
{
    /* The app's whole folder, not just the playlists inside it. The
     * settings file lives there too now, and neither is something
     * anybody browsing their music wants to walk into. */
    static const char kDir[] = DSXP_DIR;
    const char *slash = strrchr(kDir, '/');
    const char *base  = slash ? slash + 1 : kDir;

    if (strcmp(name, base) != 0) return false;

    const size_t parentLen = slash ? (size_t)(slash - kDir) : 0;
    if (parentLen == 0) return IsAtRoot(fb);
    return strlen(fb->cwd) == parentLen &&
           strncmp(fb->cwd, kDir, parentLen) == 0;
}

/* The card, mounted once rather than once per call.
 *
 * fatInitDefault runs libfat's whole discovery and mount sequence, and
 * mounting a name that is already mounted does not succeed - so the
 * second call and every call after it can report failure for a card that
 * is present and working perfectly well. Init runs every time a tab
 * repoints the browser, which put that failure one tap away. */
static bool CardMounted(void)
{
    static bool mounted = false;
    if (!mounted) mounted = fatInitDefault();
    return mounted;
}

/* Brings a card written by an earlier build into the app's folder.
 *
 * Only two kinds of thing move: the settings file from the root, and
 * .m3u files out of the old playlist folder. Music is never touched,
 * nothing already in the new place is overwritten, and a file that
 * refuses to move is left exactly where it is rather than being deleted
 * and rewritten - a half-finished move must not be able to lose a
 * playlist somebody made.
 *
 * Safe to call on every start: once the old locations are empty it does
 * nothing but two mkdirs that fail harmlessly. */
void FileBrowser_MigrateAppFolder(void)
{
    if (Settings_CardWritesBlocked()) return;
    if (!CardMounted()) return;

    mkdir(DSXP_DIR, 0777);
    mkdir(PLAYLIST_DIR, 0777);

    /* The settings file, if the new one is not already there. */
    {
        struct stat st;
        if (stat(DSXP_CFG_PATH, &st) != 0 &&
            stat(DSXP_LEGACY_CFG, &st) == 0)
            rename(DSXP_LEGACY_CFG, DSXP_CFG_PATH);
    }

    /* And the playlists. Names are read into a list before anything is
     * moved: renaming while walking the same directory is the kind of
     * thing that works on one filesystem and skips entries on another. */
    DIR *d = opendir(DSXP_LEGACY_PLAYLISTS);
    if (!d) return;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < FB_MAX_ENTRIES) {
        if (ent->d_name[0] == '.') continue;
        if (!HasM3uExtension(ent->d_name)) continue;
        if (strlen(ent->d_name) >= FB_NAME_LEN) continue;
        memcpy(s_folderNames[n], ent->d_name, strlen(ent->d_name) + 1);
        n++;
    }
    closedir(d);

    for (int i = 0; i < n; i++) {
        /* The precision is not decoration: the name is bounded by the
         * scan above, but only saying so here lets the compiler see it. */
        char from[FB_PATH_LEN + FB_NAME_LEN], to[FB_PATH_LEN + FB_NAME_LEN];
        snprintf(from, sizeof(from), "%s/%.*s", DSXP_LEGACY_PLAYLISTS,
                 FB_NAME_LEN - 1, s_folderNames[i]);
        snprintf(to,   sizeof(to),   "%s/%.*s", PLAYLIST_DIR,
                 FB_NAME_LEN - 1, s_folderNames[i]);

        struct stat st;
        if (stat(to, &st) == 0) continue;      /* already there; leave both */
        rename(from, to);
    }

    /* And the old folder, but only if everything did move out of it:
     * rmdir refuses a directory with anything left in it, which is
     * exactly the behaviour wanted. */
    rmdir(DSXP_LEGACY_PLAYLISTS);
}

bool FileBrowser_Init(FileBrowser *fb, const char *startPath)
{
    /* Mount first, wipe second.
     *
     * The other way round, a failed open left the caller holding a
     * browser with no cwd and no rows: "No MP3 files in this folder"
     * over a folder that was never opened, and a Refresh that opened ""
     * and failed the same way. The browser is only cleared once there is
     * a card to fill it from. */
    if (!CardMounted()) return false;

    memset(fb, 0, sizeof(*fb));

    strncpy(fb->cwd, startPath, sizeof(fb->cwd) - 1);
    fb->cwd[sizeof(fb->cwd) - 1] = '\0';
    if (fb->cwd[0] == '\0') strcpy(fb->cwd, "/");

    FileBrowser_Refresh(fb, NULL);
    return true;
}

/* The same, for a folder that is a list rather than a place.
 *
 * Init clears the struct before it scans, so the flag cannot be set
 * beforehand - the folder is scanned twice instead. PLAYLIST_DIR holds a
 * handful of files, and paying one extra readdir once per visit to the
 * tab is cheaper than threading a parameter through every caller of a
 * function that has one job. */
bool FileBrowser_InitLocked(FileBrowser *fb, const char *startPath)
{
    if (!FileBrowser_Init(fb, startPath)) return false;
    fb->locked = true;
    FileBrowser_Refresh(fb, NULL);
    return true;
}

void FileBrowser_Refresh(FileBrowser *fb, const char *selectName)
{
    fb->count = 0;
    fb->selected = 0;
    fb->scrollTop = 0;
    fb->truncated = false;

    if (!IsAtRoot(fb) && !fb->locked) {
        /* Cleared, not just named: this slot holds whatever the previous
         * folder left in it, and every field that is not overwritten
         * carries over - so ".." could arrive already marked, or flagged
         * as a playlist, from a scan two folders ago. */
        FbEntry *up = &fb->entries[fb->count++];
        memset(up, 0, sizeof(*up));
        strcpy(up->name, "..");
        up->isDir = true;
    }

    DIR *dir = opendir(fb->cwd);
    if (!dir) return;   /* keeps ".." so the user can still get out */

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Anything starting with a dot is skipped.
         *
         * This is not tidiness, it is correctness. Copying music to a FAT
         * card from a Mac leaves an AppleDouble sidecar beside every
         * file - "1_x.mp3" arrives with "._1_x.mp3" next to it, holding
         * the resource fork and Finder metadata. It ends in .mp3, so the
         * extension check waves it straight through, and it appears in
         * the list as a track that cannot be played: the contents are not
         * MPEG audio at all.
         *
         * The same rule takes care of the rest of what desktop systems
         * scatter over a card - .Trashes, .Spotlight-V100, .fseventsd -
         * none of which anyone wants to scroll past to reach an album. */
        if (ent->d_name[0] == '.')
            continue;

        bool isDir;
        if (ent->d_type == DT_DIR) {
            isDir = true;
        } else if (ent->d_type == DT_UNKNOWN) {
            /* Not every filesystem driver fills d_type in, so fall back to
             * asking rather than mistaking every folder for a file. */
            char probe[FB_PATH_LEN + NAME_MAX + 2];
            size_t n = strlen(fb->cwd);
            snprintf(probe, sizeof(probe), "%s%s%s", fb->cwd,
                     (n && fb->cwd[n - 1] == '/') ? "" : "/", ent->d_name);

            struct stat st;
            isDir = (stat(probe, &st) == 0) && S_ISDIR(st.st_mode);
        } else {
            isDir = false;
        }

        if (isDir && !fb->locked && IsPlaylistFolder(fb, ent->d_name))
            continue;

        bool isPlaylist = !isDir && HasM3uExtension(ent->d_name);
        /* A locked folder lists its playlists and nothing else - no
         * subfolders to descend into, and no loose audio either. */
        if (fb->locked && (isDir || !isPlaylist)) continue;

        if (!isDir && !isPlaylist && !HasAudioExtension(ent->d_name))
            continue;

        if (fb->count >= FB_MAX_ENTRIES) {
            fb->truncated = true;
            break;
        }

        FbEntry *e = &fb->entries[fb->count++];
        /* snprintf rather than strncpy: names longer than the row can show
         * are truncated on purpose, and this is the spelling that says so
         * without leaving the result unterminated. */
        snprintf(e->name, sizeof(e->name), "%s", ent->d_name);
        e->isPlaylist = isPlaylist;
        /* Recognised by name, which is the only thing a directory scan
         * has to go on. It is the same name favourites.c writes, from
         * the same macro, so the two cannot disagree about which file
         * this is. */
        e->isLiked = isPlaylist &&
                     strcasecmp(ent->d_name, FAVOURITES_NAME ".m3u") == 0;
        e->marked = false;
        e->isDir = isDir;
    }
    closedir(dir);

    /* ".." is pinned at index 0 rather than left to sort there. It would
     * sort first anyway, but relying on that makes the collation order
     * load-bearing for navigation. */
    int sortStart = (fb->count > 0 && strcmp(fb->entries[0].name, "..") == 0) ? 1 : 0;
    if (fb->count > sortStart)
        qsort(fb->entries + sortStart, (size_t)(fb->count - sortStart),
              sizeof(FbEntry), CompareEntries);

    if (selectName) {
        for (int i = 0; i < fb->count; i++) {
            if (strcmp(fb->entries[i].name, selectName) == 0) {
                fb->selected = i;
                break;
            }
        }
        ScrollToSelection(fb);
    }
}

void FileBrowser_SetSelection(FileBrowser *fb, int index)
{
    if (fb->count == 0) return;

    if (index < 0) index = 0;
    if (index >= fb->count) index = fb->count - 1;

    fb->selected = index;
    ScrollToSelection(fb);
}

void FileBrowser_MoveSelection(FileBrowser *fb, int delta)
{
    if (fb->count == 0) return;

    /* Wrapping at the ends makes reaching the last track of a long folder
     * one press instead of a hundred. */
    int next = fb->selected + delta;
    if (next < 0) next = fb->count - 1;
    if (next >= fb->count) next = 0;

    FileBrowser_SetSelection(fb, next);
}

bool FileBrowser_SelectedIsDir(const FileBrowser *fb)
{
    if (fb->count == 0) return false;
    return fb->entries[fb->selected].isDir;
}

void FileBrowser_GoUp(FileBrowser *fb)
{
    if (IsAtRoot(fb)) return;

    char *slash = strrchr(fb->cwd, '/');
    if (!slash) return;

    /* Remember which folder we came out of so it starts out highlighted -
     * otherwise backing out of a folder dumps the cursor at the top of a
     * long list every time. */
    char cameFrom[FB_NAME_LEN];
    strncpy(cameFrom, slash + 1, sizeof(cameFrom) - 1);
    cameFrom[sizeof(cameFrom) - 1] = '\0';

    if (slash == fb->cwd)
        fb->cwd[1] = '\0';      /* back to "/" */
    else
        *slash = '\0';

    FileBrowser_Refresh(fb, cameFrom[0] ? cameFrom : NULL);
}

bool FileBrowser_EnterSelected(FileBrowser *fb)
{
    if (fb->count == 0) return false;

    const FbEntry *e = &fb->entries[fb->selected];
    if (!e->isDir) return true;

    if (strcmp(e->name, "..") == 0) {
        FileBrowser_GoUp(fb);
        return false;
    }

    size_t n = strlen(fb->cwd);
    size_t nameLen = strlen(e->name);
    bool needSlash = (n == 0) || (fb->cwd[n - 1] != '/');

    if (n + (needSlash ? 1u : 0u) + nameLen + 1 > sizeof(fb->cwd))
        return false;   /* path too deep to represent - stay put */

    if (needSlash) fb->cwd[n++] = '/';
    memcpy(fb->cwd + n, e->name, nameLen + 1);

    FileBrowser_Refresh(fb, NULL);
    return false;
}
