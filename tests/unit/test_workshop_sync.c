/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The Workshop sync, driven through workshopSyncRunWith with a source table
 * in place of the Steam wrapper. Each case builds the item folders Steam
 * would have unpacked, and the Workshop directory they are copied into,
 * under its own scratch path.
 *
 *   workshop_sync_classify            what a folder holds: a skin, one
 *                                     content file, or something else
 *   workshop_sync_copies_and_indexes  installed items are copied and indexed
 *                                     once, and only a changed one again
 *   workshop_sync_removes_unsubscribed
 *                                     an item no longer subscribed loses its
 *                                     file; one still downloading keeps its
 *                                     own, and a file the sync did not put
 *                                     there is left alone
 *   workshop_sync_unavailable_touches_nothing
 *                                     with the Workshop unavailable a pass
 *                                     changes nothing, index included
 *   workshop_sync_name_clash          two items with one file name: the
 *                                     lower id keeps it
 *   workshop_sync_renamed_content     an item whose file changed its name
 *                                     leaves the new file and not the old
 *   workshop_sync_bumps_script_dirs_gen
 *                                     a pass that copies tells the listings,
 *                                     and one that copies nothing does not
 *   workshop_sync_disabled_item_left_alone
 *                                     an item the player disabled in Steam
 *                                     is not asked for or counted pending,
 *                                     and keeps the file and row it had
 *   workshop_sync_recovers_bad_index  an index that does not parse is set
 *                                     aside, the files the sync copied are
 *                                     indexed again by their bytes, and a
 *                                     file that differs is still left alone
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cJSON.h"
#include "server_sim.h"     /* serverSimScriptDirsGen */
#include "workshop_sync.h"
#include "test_harness.h"

#define WSS_ITEMS_MAX 8

/* ── The source ───────────────────────────────────────────────────── */

typedef struct {
    uint64_t id;
    bool     installed;
    bool     disabled;   /* disabled locally: item answers false for it */
    char     folder[512];
} WssItem;

static WssItem  wssItems[WSS_ITEMS_MAX];
static int      wssCount;
static bool     wssAvailable;
static uint64_t wssRequested[WSS_ITEMS_MAX];
static int      wssRequestedCount;

static bool wssAvailableFn(void) { return wssAvailable; }

static int wssCountFn(void) { return wssCount; }

/* The wrapper's contract: both outs cleared, the id written whether or not
   the item is installed, and true only when it is installed and not
   disabled. */
static bool wssItemFn(int idx, uint64_t *id, char *folder, size_t folderSize) {
    if (id != NULL) *id = 0;
    if (folder != NULL && folderSize > 0) folder[0] = '\0';
    if (idx < 0 || idx >= wssCount || id == NULL) return false;
    *id = wssItems[idx].id;
    if (!wssItems[idx].installed || wssItems[idx].disabled) return false;
    SDL_strlcpy(folder, wssItems[idx].folder, folderSize);
    return true;
}

static void wssRequestFn(uint64_t id) {
    if (wssRequestedCount < WSS_ITEMS_MAX) {
        wssRequested[wssRequestedCount++] = id;
    }
}

static bool wssDisabledFn(uint64_t id) {
    int i;

    for (i = 0; i < wssCount; i++) {
        if (wssItems[i].id == id) return wssItems[i].disabled;
    }
    return false;
}

static const WorkshopSyncSource kWssSource = {
    wssAvailableFn, wssCountFn, wssItemFn, wssRequestFn, wssDisabledFn,
};

static void wssReset(void) {
    memset(wssItems, 0, sizeof(wssItems));
    wssCount          = 0;
    wssAvailable      = true;
    wssRequestedCount = 0;
}

/* ── Files ────────────────────────────────────────────────────────── */

static bool wssMakeDir(char *out, size_t outLen, const char *leaf) {
    return utScratchPath(out, outLen, leaf) && SDL_CreateDirectory(out);
}

static bool wssWrite(const char *dir, const char *name, const char *text) {
    char  path[1024];
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool wssExists(const char *dir, const char *name) {
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return SDL_GetPathInfo(path, NULL);
}

/* Whether dir/name holds exactly text. */
static bool wssHolds(const char *dir, const char *name, const char *text) {
    char   path[1024];
    size_t len = 0;
    void  *got;
    bool   same;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    got = SDL_LoadFile(path, &len);
    if (got == NULL) return false;
    same = len == strlen(text) && memcmp(got, text, len) == 0;
    SDL_free(got);
    return same;
}

/* An item on the source, its folder made under the scratch path. */
static bool wssAddItem(uint64_t id, bool installed, const char *leaf) {
    WssItem *it;

    if (wssCount >= WSS_ITEMS_MAX) return false;
    it            = &wssItems[wssCount++];
    it->id        = id;
    it->installed = installed;
    return wssMakeDir(it->folder, sizeof(it->folder), leaf);
}

/* The index's rows: how many, and whether one has this id (as a string)
   and this file. */
static int wssIndexRows(const char *dir, const char *id, const char *file,
                        bool *found) {
    char   path[1024];
    size_t len = 0;
    void  *text;
    cJSON *root;
    cJSON *it;
    int    n = 0;

    if (found != NULL) *found = false;
    snprintf(path, sizeof(path), "%s/%s", dir, WORKSHOP_SYNC_INDEX);
    text = SDL_LoadFile(path, &len);
    if (text == NULL) return -1;
    root = cJSON_Parse((const char *)text);
    SDL_free(text);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_ArrayForEach(it, root) {
        const cJSON *jid   = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON *jfile = cJSON_GetObjectItemCaseSensitive(it, "file");

        n++;
        if (found != NULL && id != NULL && cJSON_IsString(jid) &&
            cJSON_IsString(jfile) && strcmp(jid->valuestring, id) == 0 &&
            strcmp(jfile->valuestring, file) == 0) {
            *found = true;
        }
    }
    cJSON_Delete(root);
    return n;
}

static const char kWssLua[] =
    "scenario = { name = \"Synced\", api = 1, kind = \"mod\" }\n";

/* ── Cases ────────────────────────────────────────────────────────── */

int run_workshop_sync_classify(void) {
    char             dir[512];
    char             file[WORKSHOP_SYNC_FILE_MAX];
    const char      *why = NULL;
    WorkshopItemKind k;

    /* A skin by its ini, even with a script beside it. */
    UT_ASSERT(wssMakeDir(dir, sizeof(dir), "skin_ini"));
    UT_ASSERT(wssWrite(dir, "skin.ini", "[skin]\n"));
    UT_ASSERT(wssWrite(dir, "extra.lua", kWssLua));
    k = workshopSyncClassify(dir, file, sizeof(file), &why);
    UT_ASSERT_MSG(k == WORKSHOP_ITEM_SKIN, "skin.ini folder read as %d",
                  (int)k);

    /* A skin by its archive. */
    UT_ASSERT(wssMakeDir(dir, sizeof(dir), "skin_zip"));
    UT_ASSERT(wssWrite(dir, "Desert.zip", "PK"));
    k = workshopSyncClassify(dir, file, sizeof(file), &why);
    UT_ASSERT_MSG(k == WORKSHOP_ITEM_SKIN, ".zip folder read as %d", (int)k);

    /* One of each content kind, the name handed back. A file of another
       kind beside it does not count. */
    UT_ASSERT(wssMakeDir(dir, sizeof(dir), "one_lua"));
    UT_ASSERT(wssWrite(dir, "Mod.LUA", kWssLua));
    UT_ASSERT(wssWrite(dir, "readme.txt", "hello\n"));
    k = workshopSyncClassify(dir, file, sizeof(file), &why);
    UT_ASSERT_MSG(k == WORKSHOP_ITEM_CONTENT && strcmp(file, "Mod.LUA") == 0,
                  "one .lua read as %d '%s' (%s)", (int)k, file, why);

    UT_ASSERT(wssMakeDir(dir, sizeof(dir), "one_scenario"));
    UT_ASSERT(wssWrite(dir, "pack.scenario", "WBSC"));
    k = workshopSyncClassify(dir, file, sizeof(file), &why);
    UT_ASSERT_MSG(k == WORKSHOP_ITEM_CONTENT &&
                  strcmp(file, "pack.scenario") == 0,
                  "one .scenario read as %d '%s' (%s)", (int)k, file, why);

    UT_ASSERT(wssMakeDir(dir, sizeof(dir), "one_map"));
    UT_ASSERT(wssWrite(dir, "island.map", "BMAPBOLO"));
    k = workshopSyncClassify(dir, file, sizeof(file), &why);
    UT_ASSERT_MSG(k == WORKSHOP_ITEM_CONTENT && strcmp(file, "island.map") == 0,
                  "one .map read as %d '%s' (%s)", (int)k, file, why);

    /* Two scripts, and a script beside a map: which one is meant is not
       the sync's to guess. */
    UT_ASSERT(wssMakeDir(dir, sizeof(dir), "two_lua"));
    UT_ASSERT(wssWrite(dir, "a.lua", kWssLua));
    UT_ASSERT(wssWrite(dir, "b.lua", kWssLua));
    k = workshopSyncClassify(dir, file, sizeof(file), &why);
    UT_ASSERT_MSG(k == WORKSHOP_ITEM_OTHER, "two .lua read as %d", (int)k);
    UT_ASSERT(why != NULL && why[0] != '\0');
    UT_ASSERT(file[0] == '\0');

    UT_ASSERT(wssMakeDir(dir, sizeof(dir), "lua_map"));
    UT_ASSERT(wssWrite(dir, "a.lua", kWssLua));
    UT_ASSERT(wssWrite(dir, "a.map", "BMAPBOLO"));
    k = workshopSyncClassify(dir, file, sizeof(file), &why);
    UT_ASSERT_MSG(k == WORKSHOP_ITEM_OTHER, ".lua beside .map read as %d",
                  (int)k);

    /* Nothing in it. */
    UT_ASSERT(wssMakeDir(dir, sizeof(dir), "empty"));
    k = workshopSyncClassify(dir, file, sizeof(file), &why);
    UT_ASSERT_MSG(k == WORKSHOP_ITEM_OTHER, "empty folder read as %d",
                  (int)k);
    UT_ASSERT(why != NULL && why[0] != '\0');
    return 0;
}

int run_workshop_sync_copies_and_indexes(void) {
    WorkshopSyncReport rep;
    char               ws[512];
    bool               found;
    int                rows;

    wssReset();
    UT_ASSERT(utScratchPath(ws, sizeof(ws), "Workshop"));
    UT_ASSERT(wssAddItem(200, true, "items/200"));
    UT_ASSERT(wssWrite(wssItems[0].folder, "two.map", "BMAPBOLO two"));
    UT_ASSERT(wssAddItem(100, true, "items/100"));
    UT_ASSERT(wssWrite(wssItems[1].folder, "one.lua", kWssLua));

    /* The directory is made by the pass. */
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT_MSG(rep.copied == 2 && rep.removed == 0 && rep.pending == 0 &&
                  rep.skipped == 0, "first pass: %d copied %d removed "
                  "%d pending %d skipped", rep.copied, rep.removed,
                  rep.pending, rep.skipped);
    UT_ASSERT(wssHolds(ws, "one.lua", kWssLua));
    UT_ASSERT(wssHolds(ws, "two.map", "BMAPBOLO two"));

    rows = wssIndexRows(ws, "100", "one.lua", &found);
    UT_ASSERT_MSG(rows == 2, "the index holds %d rows, wanted 2", rows);
    UT_ASSERT_MSG(found, "no row for 100 / one.lua with the id as a string");
    (void)wssIndexRows(ws, "200", "two.map", &found);
    UT_ASSERT_MSG(found, "no row for 200 / two.map with the id as a string");

    /* Nothing changed, so nothing is copied. */
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT_MSG(rep.copied == 0, "a second pass copied %d", rep.copied);

    /* One source grows: that one is copied again, and only that one. */
    UT_ASSERT(wssWrite(wssItems[0].folder, "two.map",
                       "BMAPBOLO two, a longer version"));
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT_MSG(rep.copied == 1, "a changed source copied %d", rep.copied);
    UT_ASSERT(wssHolds(ws, "two.map", "BMAPBOLO two, a longer version"));
    UT_ASSERT(wssHolds(ws, "one.lua", kWssLua));
    return 0;
}

int run_workshop_sync_removes_unsubscribed(void) {
    WorkshopSyncReport rep;
    char               ws[512];
    bool               found;
    int                rows;

    wssReset();
    UT_ASSERT(utScratchPath(ws, sizeof(ws), "Workshop"));
    UT_ASSERT(wssAddItem(100, true, "items/100"));
    UT_ASSERT(wssWrite(wssItems[0].folder, "one.lua", kWssLua));
    UT_ASSERT(wssAddItem(200, true, "items/200"));
    UT_ASSERT(wssWrite(wssItems[1].folder, "two.lua", kWssLua));
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT(rep.copied == 2);

    /* A file of the player's own, which no row names. */
    UT_ASSERT(wssWrite(ws, "mine.lua", kWssLua));

    /* 100 unsubscribed; 200 still subscribed but downloading an update. */
    wssItems[0]           = wssItems[1];
    wssItems[0].installed = false;
    wssCount              = 1;

    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT_MSG(rep.removed == 1 && rep.pending == 1 && rep.copied == 0,
                  "%d removed %d pending %d copied", rep.removed, rep.pending,
                  rep.copied);
    UT_ASSERT_MSG(!wssExists(ws, "one.lua"), "the unsubscribed file is there");
    UT_ASSERT_MSG(wssExists(ws, "two.lua"),
                  "the file of an item still downloading was removed");
    UT_ASSERT_MSG(wssExists(ws, "mine.lua"),
                  "a file no row names was removed");
    UT_ASSERT_MSG(wssRequestedCount == 1 && wssRequested[0] == 200,
                  "download asked for %d time(s), first id %llu",
                  wssRequestedCount,
                  (unsigned long long)(wssRequestedCount > 0 ? wssRequested[0]
                                                             : 0));

    rows = wssIndexRows(ws, "200", "two.lua", &found);
    UT_ASSERT_MSG(rows == 1 && found, "the index holds %d rows (200 %s)",
                  rows, found ? "kept" : "missing");
    return 0;
}

int run_workshop_sync_unavailable_touches_nothing(void) {
    WorkshopSyncReport rep;
    char               ws[512];
    char               fresh[512];
    char               path[1024];
    size_t             beforeLen = 0;
    size_t             afterLen  = 0;
    void              *before;
    void              *after;
    bool               same;

    wssReset();
    UT_ASSERT(utScratchPath(ws, sizeof(ws), "Workshop"));
    UT_ASSERT(wssAddItem(100, true, "items/100"));
    UT_ASSERT(wssWrite(wssItems[0].folder, "one.lua", kWssLua));
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT(rep.copied == 1);

    snprintf(path, sizeof(path), "%s/%s", ws, WORKSHOP_SYNC_INDEX);
    before = SDL_LoadFile(path, &beforeLen);
    UT_ASSERT(before != NULL);

    /* Steam gone, and with it every subscription: a pass must not read
       that as the player having unsubscribed from everything. */
    wssAvailable = false;
    wssCount     = 0;
    UT_ASSERT(!workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT(rep.copied == 0 && rep.removed == 0 && rep.pending == 0 &&
              rep.skipped == 0);

    after = SDL_LoadFile(path, &afterLen);
    same  = after != NULL && afterLen == beforeLen &&
           memcmp(before, after, beforeLen) == 0;
    SDL_free(before);
    SDL_free(after);
    UT_ASSERT_MSG(same, "the index changed on a pass with no Workshop");
    UT_ASSERT_MSG(wssHolds(ws, "one.lua", kWssLua),
                  "the copied file changed on a pass with no Workshop");

    /* Nor is a directory that was not there made. */
    UT_ASSERT(utScratchPath(fresh, sizeof(fresh), "NotMade"));
    UT_ASSERT(!workshopSyncRunWith(&kWssSource, fresh, &rep));
    UT_ASSERT_MSG(!SDL_GetPathInfo(fresh, NULL),
                  "a pass with no Workshop made the directory");
    return 0;
}

int run_workshop_sync_name_clash(void) {
    WorkshopSyncReport rep;
    char               ws[512];
    bool               found;
    int                rows;

    wssReset();
    UT_ASSERT(utScratchPath(ws, sizeof(ws), "Workshop"));
    /* Listed higher id first, so the order Steam gives is not what
       decides. */
    UT_ASSERT(wssAddItem(300, true, "items/300"));
    UT_ASSERT(wssWrite(wssItems[0].folder, "same.lua", "-- from 300\n"));
    UT_ASSERT(wssAddItem(100, true, "items/100"));
    UT_ASSERT(wssWrite(wssItems[1].folder, "SAME.lua", "-- from 100\n"));

    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT_MSG(rep.copied == 1 && rep.skipped == 1,
                  "%d copied %d skipped", rep.copied, rep.skipped);
    UT_ASSERT_MSG(wssHolds(ws, "SAME.lua", "-- from 100\n"),
                  "the file is not the lower id's");

    rows = wssIndexRows(ws, "100", "SAME.lua", &found);
    UT_ASSERT_MSG(rows == 1 && found, "the index holds %d rows (100 %s)",
                  rows, found ? "there" : "missing");

    /* A second pass settles on the same answer rather than swapping. */
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT(rep.copied == 0 && rep.skipped == 1);
    UT_ASSERT(wssHolds(ws, "SAME.lua", "-- from 100\n"));
    return 0;
}

int run_workshop_sync_renamed_content(void) {
    WorkshopSyncReport rep;
    char               ws[512];
    char               path[1024];
    bool               found;
    int                rows;

    wssReset();
    UT_ASSERT(utScratchPath(ws, sizeof(ws), "Workshop"));
    UT_ASSERT(wssAddItem(100, true, "items/100"));
    UT_ASSERT(wssWrite(wssItems[0].folder, "a.lua", kWssLua));
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT(wssExists(ws, "a.lua"));

    /* The author renamed the file in an update. */
    snprintf(path, sizeof(path), "%s/a.lua", wssItems[0].folder);
    UT_ASSERT(SDL_RemovePath(path));
    UT_ASSERT(wssWrite(wssItems[0].folder, "b.lua", kWssLua));

    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT_MSG(rep.copied == 1, "%d copied", rep.copied);
    UT_ASSERT_MSG(wssExists(ws, "b.lua"), "the new file is not there");
    UT_ASSERT_MSG(!wssExists(ws, "a.lua"), "the old file is still there");

    rows = wssIndexRows(ws, "100", "b.lua", &found);
    UT_ASSERT_MSG(rows == 1 && found, "the index holds %d rows (b.lua %s)",
                  rows, found ? "there" : "missing");
    return 0;
}

int run_workshop_sync_bumps_script_dirs_gen(void) {
    WorkshopSyncReport rep;
    char               ws[512];
    uint32_t           gen;

    wssReset();
    UT_ASSERT(utScratchPath(ws, sizeof(ws), "Workshop"));
    UT_ASSERT(wssAddItem(100, true, "items/100"));
    UT_ASSERT(wssWrite(wssItems[0].folder, "one.lua", kWssLua));

    gen = serverSimScriptDirsGen();
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT(rep.copied == 1);
    UT_ASSERT_MSG(serverSimScriptDirsGen() != gen,
                  "a pass that copied left the listings' count where it was");

    gen = serverSimScriptDirsGen();
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT(rep.copied == 0 && rep.removed == 0);
    UT_ASSERT_MSG(serverSimScriptDirsGen() == gen,
                  "a pass that changed nothing moved the listings' count");
    return 0;
}

int run_workshop_sync_disabled_item_left_alone(void) {
    WorkshopSyncReport rep;
    char               ws[512];
    bool               found;
    int                rows;

    wssReset();
    UT_ASSERT(utScratchPath(ws, sizeof(ws), "Workshop"));
    UT_ASSERT(wssAddItem(100, true, "items/100"));
    UT_ASSERT(wssWrite(wssItems[0].folder, "one.lua", kWssLua));
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT(rep.copied == 1);

    /* The player disables 100 in Steam, and 200, disabled before it ever
       synced, is subscribed too. Both stay installed on disk. */
    wssItems[0].disabled = true;
    UT_ASSERT(wssAddItem(200, true, "items/200"));
    UT_ASSERT(wssWrite(wssItems[1].folder, "two.lua", kWssLua));
    wssItems[1].disabled = true;
    wssRequestedCount    = 0;

    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT_MSG(rep.pending == 0 && rep.copied == 0 && rep.removed == 0,
                  "%d pending %d copied %d removed", rep.pending, rep.copied,
                  rep.removed);
    UT_ASSERT_MSG(wssRequestedCount == 0,
                  "a download was asked for %d time(s), first id %llu",
                  wssRequestedCount,
                  (unsigned long long)(wssRequestedCount > 0 ? wssRequested[0]
                                                             : 0));
    UT_ASSERT_MSG(wssHolds(ws, "one.lua", kWssLua),
                  "the disabled item's synced file was removed or changed");
    UT_ASSERT_MSG(!wssExists(ws, "two.lua"),
                  "a disabled item that never synced was copied");

    rows = wssIndexRows(ws, "100", "one.lua", &found);
    UT_ASSERT_MSG(rows == 1 && found, "the index holds %d rows (100 %s)",
                  rows, found ? "kept" : "missing");
    return 0;
}

int run_workshop_sync_recovers_bad_index(void) {
    WorkshopSyncReport rep;
    char               ws[512];
    bool               found;
    int                rows;

    wssReset();
    UT_ASSERT(utScratchPath(ws, sizeof(ws), "Workshop"));
    UT_ASSERT(wssAddItem(100, true, "items/100"));
    UT_ASSERT(wssWrite(wssItems[0].folder, "one.lua", kWssLua));
    UT_ASSERT(wssAddItem(200, true, "items/200"));
    UT_ASSERT(wssWrite(wssItems[1].folder, "two.map", "BMAPBOLO two"));
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT(rep.copied == 2);

    /* The index is lost, and a third item names a file the player wrote by
       hand, with other bytes than the item's. */
    UT_ASSERT(wssWrite(ws, WORKSHOP_SYNC_INDEX, "this is not JSON {"));
    UT_ASSERT(wssAddItem(300, true, "items/300"));
    UT_ASSERT(wssWrite(wssItems[2].folder, "three.lua", "-- from 300\n"));
    UT_ASSERT(wssWrite(ws, "three.lua", "-- the player's own\n"));

    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT_MSG(rep.skipped == 1 && rep.removed == 0,
                  "%d skipped %d removed", rep.skipped, rep.removed);
    UT_ASSERT_MSG(wssExists(ws, WORKSHOP_SYNC_INDEX ".bad"),
                  "the index that did not parse was not set aside");

    rows = wssIndexRows(ws, "100", "one.lua", &found);
    UT_ASSERT_MSG(rows == 2, "the index holds %d rows, wanted 2", rows);
    UT_ASSERT_MSG(found, "no row for 100 / one.lua");
    (void)wssIndexRows(ws, "200", "two.map", &found);
    UT_ASSERT_MSG(found, "no row for 200 / two.map");
    UT_ASSERT(wssHolds(ws, "one.lua", kWssLua));
    UT_ASSERT(wssHolds(ws, "two.map", "BMAPBOLO two"));
    UT_ASSERT_MSG(wssHolds(ws, "three.lua", "-- the player's own\n"),
                  "a file the sync did not write was written over");

    /* Indexed again, the files follow their items: an update is copied. */
    UT_ASSERT(wssWrite(wssItems[0].folder, "one.lua", "-- one, updated\n"));
    UT_ASSERT(workshopSyncRunWith(&kWssSource, ws, &rep));
    UT_ASSERT_MSG(rep.copied == 1 && rep.skipped == 1,
                  "%d copied %d skipped", rep.copied, rep.skipped);
    UT_ASSERT(wssHolds(ws, "one.lua", "-- one, updated\n"));
    return 0;
}
