#include "favourites.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "filebrowser.h"
#include "settings.h"

/* The list, in memory.
 *
 * Held rather than re-read because the heart's state is asked for on
 * every frame the transport plate repaints, and answering that from the
 * card would mean an fopen per frame on a device where a read is the
 * expensive thing. The file is the truth; this is a copy that is dropped
 * whenever anything might have changed it.
 *
 * A fixed table rather than a growing one: 128 favourites at a full path
 * each is 32KB, which is affordable, and the alternative is a malloc in a
 * program that otherwise has none. Past the limit the list stops
 * accepting rather than forgetting the oldest - silently dropping
 * something the user asked to keep is worse than saying no. */
#define FAV_MAX 128

static char  s_fav[FAV_MAX][FB_PATH_LEN];
static int   s_count = 0;
static bool  s_loaded = false;

/* Set when the file held more paths than this cache can keep, so what
 * is in memory is only part of what is on the card. */
static bool  s_partial = false;

/* Reads the list, and says whether it managed to.
 *
 * s_loaded means "memory agrees with the card", and it is set at the
 * end, once that is true. It used to be set on the first line of this
 * function, before the file had even been opened - so a read that
 * failed for any reason left the cache saying "loaded, and empty". The
 * next heart tapped anywhere then appended one path to that empty list
 * and wrote the whole file back, and every other like was gone. One
 * tap, no warning, no way back.
 *
 * A file that is genuinely not there is a different thing from a file
 * that could not be read: the first is an empty list and liking a song
 * should create it, the second is a list this program must not touch.
 * They are told apart by asking the card whether the file exists. */
static void Load(void)
{
    if (s_loaded) return;
    s_count = 0;
    s_partial = false;

    FILE *f = fopen(FAVOURITES_PATH, "r");
    if (!f) {
        struct stat st;
        /* Not there at all: an empty list is the right answer, and the
         * first like will write it. Anything else - there but
         * unreadable - leaves the cache unloaded, and everything that
         * would write refuses. */
        if (stat(FAVOURITES_PATH, &st) != 0) s_loaded = true;
        return;
    }

    char line[FB_PATH_LEN];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);

        /* A line too long for the buffer is finished and discarded as
         * one line rather than coming back as two, the second of which
         * is short enough to look like a path of its own. */
        bool whole = (n == 0 || line[n - 1] == '\n');
        if (!whole) { int c; while ((c = fgetc(f)) != EOF && c != '\n') { } }

        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        /* #EXTM3U and #EXTINF are comments, and a blank line is nothing.
         * Everything else is a path. */
        if (!n || line[0] == '#') continue;

        /* Counted even when there is no room for it. Stopping the loop
         * here would make the file look shorter than it is, and Save
         * below would then write that shorter list back over it. */
        if (!whole || s_count >= FAV_MAX) { s_partial = true; continue; }

        memcpy(s_fav[s_count], line, n + 1);
        s_count++;
    }
    fclose(f);
    s_loaded = true;
}

static bool Save(void)
{
    /* Nothing is written while card writes are off. The caller is told
     * the state did not change, so the heart does not light for something
     * that was not recorded. */
    if (Settings_CardWritesBlocked()) return false;

    /* Never write back less than was read.
     *
     * This function rewrites the whole file from memory, so it may only
     * run when memory holds the whole file. If the read failed, or the
     * list is longer than this cache, what is here is a fragment - and
     * writing a fragment over the card is how a hundred likes become
     * one. Refusing costs the user a like; not refusing costs them the
     * list. */
    if (!s_loaded || s_partial) return false;

    /* The parent first: the playlist folder is inside the app's
     * folder now, and mkdir does not make a path. */
    mkdir(DSXP_DIR, 0777);
    mkdir(PLAYLIST_DIR, 0777);

    /* Staged, because this file is the user's own list and rewriting it
     * in place means a failed write destroys it. */
    DsxpWrite w;
    if (!Dsxp_BeginWrite(&w, FAVOURITES_PATH)) return false;

    fprintf(w.f, "#EXTM3U\n");
    for (int i = 0; i < s_count; i++)
        fprintf(w.f, "%s\n", s_fav[i]);

    if (ferror(w.f)) { Dsxp_AbortWrite(&w); return false; }
    return Dsxp_CommitWrite(&w, FAVOURITES_PATH);
}

static int IndexOf(const char *path)
{
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_fav[i], path) == 0) return i;
    return -1;
}

bool Favourites_Contains(const char *path)
{
    if (!path || !path[0]) return false;
    Load();
    /* An unreadable list is not an empty one, but there is nothing
     * better to draw than an unlit heart. */
    return s_loaded && IndexOf(path) >= 0;
}

bool Favourites_Toggle(const char *path)
{
    if (!path || !path[0]) return false;
    Load();

    /* The list could not be read, so this program does not know what is
     * in it and has no business replacing it. */
    if (!s_loaded) return false;

    int at = IndexOf(path);

    if (at >= 0) {
        /* Closing the gap rather than leaving a hole, so the file stays a
         * plain list of paths that anything else can read. */
        for (int i = at; i + 1 < s_count; i++)
            memcpy(s_fav[i], s_fav[i + 1], sizeof(s_fav[i]));
        s_count--;

        if (!Save()) { s_loaded = false; return true; }
        return false;
    }

    if (s_count >= FAV_MAX) return false;
    if (strlen(path) >= FB_PATH_LEN) return false;

    snprintf(s_fav[s_count], sizeof(s_fav[s_count]), "%s", path);
    s_count++;

    /* If the write failed the card does not agree with memory, so the
     * cache is dropped and the answer is what the card will say next
     * time - which is "not a favourite". */
    if (!Save()) { s_loaded = false; return false; }
    return true;
}

void Favourites_Invalidate(void) { s_loaded = false; }

int Favourites_Count(void)
{
    Load();
    return s_loaded ? s_count : 0;
}
