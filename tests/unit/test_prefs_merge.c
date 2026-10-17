/*
 * Tests for merge-on-save in src/common/prefs.c: the mode the standalone
 * log viewer and the game both turn on, because the two may run side by
 * side and keep the same WinBolo.json.
 *
 * prefs.c holds one document per process, so the viewer is prefs.c itself
 * and "the game" is a second document here that does what the game's
 * prefs.c does: it loads the file once when the game starts and keeps that
 * document; each write reads the file again, puts onto it only the key the
 * game changed and keeps the result (gameStart, gameSet).
 *
 *   - prefs_merge_game_then_viewer: the game writes after the viewer
 *                                   loaded; the viewer's save keeps the
 *                                   game's change and lands its own, and
 *                                   the viewer then reads the game's value
 *   - prefs_merge_viewer_then_game: the reverse order, then a second
 *                                   viewer save; nothing is lost
 *   - prefs_merge_fallback:         file gone, or no longer JSON: the
 *                                   document in memory is written whole
 *   - prefs_merge_one_write:        sets do not write; one flush writes
 *                                   them all; with the mode off a set
 *                                   writes at once, as before
 *   - prefs_merge_reset_wins:       a key set again to the value in memory
 *                                   still goes over what the game wrote
 *   - prefs_merge_idle_no_write:    a viewer that changes nothing writes
 *                                   nothing
 *   - prefs_merge_missing_retry:    file gone for a moment: the first flush
 *                                   waits; the file back = a merge, still
 *                                   gone = the whole document; shutdown
 *                                   does both tries
 *
 * Portable on purpose (stdio only): these run on Windows too.
 */

#include "test_harness.h"

#include <stdio.h>
#include <string.h>

#include "common/prefs.h"
#include "common/prefs_doc.h"

/* Read [section]/key straight from the file. */
static void diskGet(const char *path, const char *section, const char *key,
                    char *out, size_t outSize) {
    PrefsDoc *doc = prefsDocLoad(path);
    out[0] = '\0';
    if (doc) {
        prefsDocGetString(doc, section, key, "<absent>", out, outSize);
        prefsDocFree(doc);
    } else {
        snprintf(out, outSize, "%s", "<unreadable>");
    }
}

/* The game's document: loaded once, when the game starts. */
static PrefsDoc *s_game = NULL;

static int gameStart(const char *path) {
    prefsDocFree(s_game);
    s_game = prefsDocLoad(path);
    return s_game != NULL;
}

/* The game changes one key and writes, with merge-on-save on: a fresh read
 * of the file, only this key put onto it, and the result kept as the
 * game's document. It does not load the file again before the change. */
static int gameSet(const char *path, const char *section, const char *key,
                   const char *value) {
    PrefsDoc *disk;
    int existed = 0;
    int ok;
    if (!s_game || !prefsDocSetString(s_game, section, key, value)) return 0;
    disk = prefsDocLoadFile(path, &existed);
    if (!disk) {
        /* No file, or not JSON: the whole document, as prefs.c does. */
        return prefsDocSave(s_game, path);
    }
    ok = prefsDocCopyKey(disk, s_game, section, key) && prefsDocSave(disk, path);
    if (ok) {
        prefsDocFree(s_game);
        s_game = disk;
    } else {
        prefsDocFree(disk);
    }
    return ok;
}

/* A file both programs start from. */
static int seedFile(const char *path) {
    PrefsDoc *doc = prefsDocNew();
    int ok;
    if (!doc) return 0;
    ok = prefsDocSetString(doc, "SETTINGS", "Player Name", "Before") &&
         prefsDocSetString(doc, "LOGVIEWER", "Window.X", "10") &&
         prefsDocSetString(doc, "LOGVIEWER", "Dock.Ini", "old layout") &&
         prefsDocSave(doc, path);
    prefsDocFree(doc);
    return ok;
}

static void endTest(const char *path) {
    char corrupt[1100];
    prefsShutdown();
    prefsSetMergeOnSave(false);
    prefsDocFree(s_game);
    s_game = NULL;
    remove(path);
    snprintf(corrupt, sizeof(corrupt), "%s.corrupt", path);
    remove(corrupt);
}

int run_prefs_merge_game_then_viewer(void) {
    char path[1024];
    char got[256];

    UT_ASSERT(utScratchPath(path, sizeof(path), "WinBolo.json"));
    remove(path);
    UT_ASSERT(seedFile(path));

    prefsInit(path);
    prefsSetMergeOnSave(true);
    UT_ASSERT(gameStart(path));

    /* The game changes its own key and one viewer key the viewer does
     * not change afterwards. */
    UT_ASSERT(gameSet(path, "SETTINGS", "Player Name", "Game"));
    UT_ASSERT(gameSet(path, "KEYS", "Forward", "W"));

    /* The viewer still holds what it loaded. */
    prefsGetString("SETTINGS", "Player Name", "", got, sizeof(got));
    UT_ASSERT(strcmp(got, "Before") == 0);

    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "250") == 1);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Dock.Ini", "new layout") == 1);
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT(!prefsIsDirty());

    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Game") == 0, "game's change lost: %s", got);
    diskGet(path, "KEYS", "Forward", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "W") == 0, "game's new section lost: %s", got);
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "250") == 0, "viewer's change lost: %s", got);
    diskGet(path, "LOGVIEWER", "Dock.Ini", got, sizeof(got));
    UT_ASSERT(strcmp(got, "new layout") == 0);

    /* After the save the viewer reads what the game wrote. */
    prefsGetString("SETTINGS", "Player Name", "", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Game") == 0, "viewer copy stale: %s", got);
    prefsGetString("LOGVIEWER", "Window.X", "", got, sizeof(got));
    UT_ASSERT(strcmp(got, "250") == 0);

    endTest(path);
    return 0;
}

int run_prefs_merge_viewer_then_game(void) {
    char path[1024];
    char got[256];

    UT_ASSERT(utScratchPath(path, sizeof(path), "WinBolo.json"));
    remove(path);
    UT_ASSERT(seedFile(path));

    prefsInit(path);
    prefsSetMergeOnSave(true);
    UT_ASSERT(gameStart(path));

    /* Viewer first. */
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "250") == 1);
    UT_ASSERT(prefsFlush() == 1);

    /* Then the game, which loaded the file before the viewer's write (it
     * was running all the time). Its writes must not take the viewer's
     * change away. It also changes a viewer key (the viewer inside the
     * game does this). */
    UT_ASSERT(gameSet(path, "SETTINGS", "Player Name", "Game"));
    UT_ASSERT(gameSet(path, "LOGVIEWER", "Speed", "7"));

    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "250") == 0, "viewer's change lost: %s", got);
    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT(strcmp(got, "Game") == 0);

    /* A second viewer save, of another key, loses nothing: not the game's
     * keys, not the viewer key the game changed, not its own first key. */
    UT_ASSERT(prefsSetString("LOGVIEWER", "Dock.Ini", "new layout") == 1);
    UT_ASSERT(prefsFlush() == 1);

    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Game") == 0, "game's change lost: %s", got);
    diskGet(path, "LOGVIEWER", "Speed", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "7") == 0, "untouched viewer key lost: %s", got);
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT(strcmp(got, "250") == 0);
    diskGet(path, "LOGVIEWER", "Dock.Ini", got, sizeof(got));
    UT_ASSERT(strcmp(got, "new layout") == 0);

    /* A key outside the viewer's section that the viewer does set (the
     * shared network code writes [WINBOLO.NET]) is carried the same way. */
    UT_ASSERT(gameSet(path, "SETTINGS", "Player Name", "Game2"));
    UT_ASSERT(prefsSetString("WINBOLO.NET", "Host", "wbn.example") == 1);
    UT_ASSERT(prefsFlush() == 1);
    diskGet(path, "WINBOLO.NET", "Host", got, sizeof(got));
    UT_ASSERT(strcmp(got, "wbn.example") == 0);
    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Game2") == 0, "game's change lost: %s", got);

    endTest(path);
    return 0;
}

int run_prefs_merge_fallback(void) {
    char path[1024];
    char got[256];
    FILE *fp;

    UT_ASSERT(utScratchPath(path, sizeof(path), "WinBolo.json"));
    remove(path);
    UT_ASSERT(seedFile(path));

    prefsInit(path);
    prefsSetMergeOnSave(true);
    UT_ASSERT(gameStart(path));

    /* The file is gone, and stays gone: the first flush waits, the second
     * writes the whole document in memory. */
    UT_ASSERT(remove(path) == 0);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "250") == 1);
    UT_ASSERT(prefsFlush() == 0);
    UT_ASSERT(prefsIsDirty());
    UT_ASSERT(prefsFlush() == 1);
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "250") == 0, "after missing file: %s", got);
    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Before") == 0, "after missing file: %s", got);

    /* The file is no longer JSON: the same. */
    fp = fopen(path, "wb");
    UT_ASSERT(fp != NULL);
    fputs("{ this is not json", fp);
    fclose(fp);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "300") == 1);
    UT_ASSERT(prefsFlush() == 1);
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "300") == 0, "after corrupt file: %s", got);
    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Before") == 0, "after corrupt file: %s", got);

    /* And the merge works again on the next save. */
    UT_ASSERT(gameSet(path, "SETTINGS", "Player Name", "Game"));
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "350") == 1);
    UT_ASSERT(prefsFlush() == 1);
    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Game") == 0, "merge after fallback: %s", got);

    endTest(path);
    return 0;
}

int run_prefs_merge_one_write(void) {
    char path[1024];
    char got[256];

    UT_ASSERT(utScratchPath(path, sizeof(path), "WinBolo.json"));
    remove(path);
    UT_ASSERT(seedFile(path));

    prefsInit(path);
    UT_ASSERT(!prefsMergeOnSave());
    prefsSetMergeOnSave(true);
    UT_ASSERT(prefsMergeOnSave());
    UT_ASSERT(gameStart(path));

    /* Sets do not reach the file ... */
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "1") == 1);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.Y", "2") == 1);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.W", "3") == 1);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.H", "4") == 1);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Dock.Ini", "five") == 1);
    UT_ASSERT(prefsIsDirty());
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "10") == 0, "a set wrote the file: %s", got);
    diskGet(path, "LOGVIEWER", "Window.H", got, sizeof(got));
    UT_ASSERT(strcmp(got, "<absent>") == 0);

    /* ... until the one flush, which carries all of them. */
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT(!prefsIsDirty());
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT(strcmp(got, "1") == 0);
    diskGet(path, "LOGVIEWER", "Window.Y", got, sizeof(got));
    UT_ASSERT(strcmp(got, "2") == 0);
    diskGet(path, "LOGVIEWER", "Window.W", got, sizeof(got));
    UT_ASSERT(strcmp(got, "3") == 0);
    diskGet(path, "LOGVIEWER", "Window.H", got, sizeof(got));
    UT_ASSERT(strcmp(got, "4") == 0);
    diskGet(path, "LOGVIEWER", "Dock.Ini", got, sizeof(got));
    UT_ASSERT(strcmp(got, "five") == 0);

    /* A flush with nothing changed does not write: the game's later
     * change is still there afterwards. */
    UT_ASSERT(gameSet(path, "LOGVIEWER", "Window.X", "77"));
    UT_ASSERT(prefsFlush() == 1);
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "77") == 0, "idle flush wrote: %s", got);

    /* Shutdown writes what is still pending. */
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.Y", "88") == 1);
    prefsShutdown();
    diskGet(path, "LOGVIEWER", "Window.Y", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "88") == 0, "shutdown did not write: %s", got);
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "77") == 0, "shutdown lost a key: %s", got);

    /* Mode off (every other program): a set writes at once, whole
     * document, as it always did. */
    prefsSetMergeOnSave(false);
    prefsInit(path);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "5") == 1);
    UT_ASSERT(!prefsIsDirty());
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "5") == 0, "immediate mode changed: %s", got);

    endTest(path);
    return 0;
}

/* The size of the file, or -1. A write through prefsDocSave puts the
 * document back in its own form, so bytes added by hand after the JSON
 * are gone after any write: the size tells a write from no write. */
static long diskSize(const char *path) {
    FILE *fp = fopen(path, "rb");
    long size = -1;
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) size = ftell(fp);
        fclose(fp);
    }
    return size;
}

/* Add white space after the JSON: still the same document to a reader. */
static int diskMark(const char *path) {
    FILE *fp = fopen(path, "ab");
    if (!fp) return 0;
    fputs("\n\n\n\n", fp);
    return fclose(fp) == 0;
}

/* The game changes a viewer key in the file. The viewer then sets that key
 * to the value its own copy still holds. The viewer's value must reach the
 * file: the set is not dropped as "nothing changed". */
int run_prefs_merge_reset_wins(void) {
    char path[1024];
    char got[256];

    UT_ASSERT(utScratchPath(path, sizeof(path), "WinBolo.json"));
    remove(path);
    UT_ASSERT(seedFile(path));

    prefsInit(path);
    prefsSetMergeOnSave(true);
    UT_ASSERT(gameStart(path));

    UT_ASSERT(gameSet(path, "LOGVIEWER", "Window.X", "999"));
    UT_ASSERT(gameSet(path, "SETTINGS", "Player Name", "Game"));

    /* "10" is what the viewer loaded and still holds. */
    prefsGetString("LOGVIEWER", "Window.X", "", got, sizeof(got));
    UT_ASSERT(strcmp(got, "10") == 0);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "10") == 1);
    UT_ASSERT(prefsIsDirty());
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT(!prefsIsDirty());

    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "10") == 0, "the viewer's set was dropped: %s", got);
    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Game") == 0, "game's change lost: %s", got);

    /* A set of the value already held in a section that syncs is no change
     * for the cloud copy. */
    UT_ASSERT(!prefsSyncDirty());
    prefsGetString("SETTINGS", "Player Name", "", got, sizeof(got));
    UT_ASSERT(strcmp(got, "Game") == 0);
    UT_ASSERT(prefsSetString("SETTINGS", "Player Name", "Game") == 1);
    UT_ASSERT(!prefsSyncDirty());
    UT_ASSERT(prefsFlush() == 1);

    endTest(path);
    return 0;
}

/* A viewer that changes nothing writes nothing: not on a flush with no set,
 * and not on a save that sets every key again to the value it has. */
int run_prefs_merge_idle_no_write(void) {
    char path[1024];
    char got[256];
    long marked;

    UT_ASSERT(utScratchPath(path, sizeof(path), "WinBolo.json"));
    remove(path);
    UT_ASSERT(seedFile(path));

    prefsInit(path);
    prefsSetMergeOnSave(true);
    UT_ASSERT(gameStart(path));
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "250") == 1);
    UT_ASSERT(prefsFlush() == 1);

    UT_ASSERT(diskMark(path));
    marked = diskSize(path);
    UT_ASSERT(marked > 0);

    /* Flushes with no set, as the viewer does at the end of every frame. */
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT_MSG(diskSize(path) == marked, "an idle flush wrote the file");

    /* The save routine: every key again, nothing moved. */
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "250") == 1);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Dock.Ini", "old layout") == 1);
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT(!prefsIsDirty());
    UT_ASSERT_MSG(diskSize(path) == marked, "a save that changed nothing wrote the file");
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT_MSG(diskSize(path) == marked, "the flush after it wrote the file");

    /* One key really moved: one write, and the mark is gone. */
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "250") == 1);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Dock.Ini", "new layout") == 1);
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT_MSG(diskSize(path) != marked, "a real change was not written");
    diskGet(path, "LOGVIEWER", "Dock.Ini", got, sizeof(got));
    UT_ASSERT(strcmp(got, "new layout") == 0);

    endTest(path);
    return 0;
}

/* The file is gone for a moment (the other program is between taking the
 * old file away and putting the new one in place). The viewer's first flush
 * must not write its whole document over what is about to appear. */
int run_prefs_merge_missing_retry(void) {
    char path[1024];
    char got[256];
    PrefsDoc *game;

    UT_ASSERT(utScratchPath(path, sizeof(path), "WinBolo.json"));
    remove(path);
    UT_ASSERT(seedFile(path));

    prefsInit(path);
    prefsSetMergeOnSave(true);
    UT_ASSERT(gameStart(path));

    /* The game, part one: it has its new document and takes the file away. */
    game = prefsDocLoad(path);
    UT_ASSERT(game != NULL);
    UT_ASSERT(prefsDocSetString(game, "SETTINGS", "Player Name", "Game"));
    UT_ASSERT(remove(path) == 0);

    /* The viewer saves in that moment: not written, still pending. */
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "250") == 1);
    UT_ASSERT(prefsFlush() == 0);
    UT_ASSERT(prefsIsDirty());
    UT_ASSERT_MSG(diskSize(path) == -1, "the first flush wrote a file");

    /* The game, part two: its file arrives. */
    UT_ASSERT(prefsDocSave(game, path));
    prefsDocFree(game);

    /* The viewer's next flush is a merge: both changes are in the file. */
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT(!prefsIsDirty());
    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Game") == 0, "game's change lost: %s", got);
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "250") == 0, "viewer's change lost: %s", got);

    /* The wait is one flush each time the file goes, not once for good. */
    UT_ASSERT(remove(path) == 0);
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "300") == 1);
    UT_ASSERT(prefsFlush() == 0);
    UT_ASSERT_MSG(diskSize(path) == -1, "the first flush wrote a file");
    /* Still gone on the next flush: really gone, so the whole document. */
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT(!prefsIsDirty());
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "300") == 0, "after missing file: %s", got);
    diskGet(path, "SETTINGS", "Player Name", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "Game") == 0, "after missing file: %s", got);

    /* A flush with nothing pending and no file writes nothing and is ok. */
    UT_ASSERT(remove(path) == 0);
    UT_ASSERT(prefsFlush() == 1);
    UT_ASSERT(diskSize(path) == -1);

    /* Shutdown has no later flush: it does both tries itself. */
    UT_ASSERT(prefsSetString("LOGVIEWER", "Window.X", "350") == 1);
    prefsShutdown();
    diskGet(path, "LOGVIEWER", "Window.X", got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "350") == 0, "shutdown did not write: %s", got);

    endTest(path);
    return 0;
}
