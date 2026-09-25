/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          workshop_sync.c
 * Purpose:       The Workshop sync: see workshop_sync.h.
 *
 *                A pass reads what the player is subscribed
 *                to, copies the one content file of each
 *                installed item into the Workshop directory,
 *                and removes the files of items no longer
 *                subscribed. workshop.json records which file
 *                came from which item, with the source's size
 *                and modify time, so a pass copies only what
 *                changed and removes only what it put there.
 *
 *                Nothing is done at all when the Workshop is
 *                unavailable: a machine that is offline, or
 *                running without Steam, would otherwise read
 *                as subscribed to nothing and lose every file.
 *********************************************************/

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cJSON.h"
#include "../../common/wb_log.h"
#include "../../scenario/scenario_host.h"   /* scenarioHostWorkshopDir */
#include "../../steam/steam_wrapper.h"
#include "scenario_manifest_json.h"          /* scnManifestParseId */
#include "server_sim.h"                      /* serverSimNoteScriptDirsChanged */
#include "wire_limits.h"                     /* the upload caps */
#include "workshop_sync.h"

/* The longest install folder path an item is read from. */
#define WS_FOLDER_MAX 1024

/* Every path this file builds: a directory, a separator and a file name. */
#define WS_PATH_MAX (WS_FOLDER_MAX + WORKSHOP_SYNC_FILE_MAX + 16)

/* What a copy is written through before it takes its own name. Dotted, so
   no listing offers it while it is there. */
#define WS_TEMP_PREFIX ".sync-"

/* ── Names ─────────────────────────────────────────────────────────── */

static bool wsHasExt(const char *name, const char *ext) {
    size_t n = strlen(name);
    size_t e = strlen(ext);

    return n > e && SDL_strcasecmp(name + n - e, ext) == 0;
}

static bool wsIsContentName(const char *name) {
    return wsHasExt(name, ".map") || wsHasExt(name, ".scenario") ||
           wsHasExt(name, ".lua");
}

/* A bare file name this directory may hold: no separator, no drive colon,
   no leading dot, short enough for a listing row, and not the index. An
   index row naming anything else is ignored, so a hand-edited index can
   never make a pass remove a file outside the directory. */
static bool wsNameOk(const char *name) {
    if (name == NULL || name[0] == '\0' || name[0] == '.') return false;
    if (strlen(name) >= WORKSHOP_SYNC_FILE_MAX) return false;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
        strchr(name, ':') != NULL) {
        return false;
    }
    return SDL_strcasecmp(name, WORKSHOP_SYNC_INDEX) != 0;
}

/* The most a content file may weigh: what a server would take in an upload
   of the same kind, since a file bigger than that could not be sent to one
   anyway. */
static size_t wsCap(const char *name) {
    return wsHasExt(name, ".map") ? (size_t)LOBBY_MAP_UPLOAD_MAX_BYTES
                                  : (size_t)LOBBY_PACKAGE_UPLOAD_MAX_BYTES;
}

/* ── What a folder holds ──────────────────────────────────────────── */

WorkshopItemKind workshopSyncClassify(const char *folder, char *file,
                                      size_t fileSize, const char **why) {
    char           **names;
    char             found[WORKSHOP_SYNC_FILE_MAX];
    bool             skin    = false;
    bool             tooLong = false;
    int              content = 0;
    int              count   = 0;
    int              i;
    WorkshopItemKind kind;

    if (file != NULL && fileSize > 0) file[0] = '\0';
    if (why != NULL) *why = "";
    found[0] = '\0';

    if (folder == NULL || folder[0] == '\0') {
        if (why != NULL) *why = "it has no install folder";
        return WORKSHOP_ITEM_OTHER;
    }
    /* "*" and not NULL: a NULL pattern walks the subdirectories too. */
    names = SDL_GlobDirectory(folder, "*", 0, &count);
    if (names == NULL) {
        if (why != NULL) *why = "its install folder could not be read";
        return WORKSHOP_ITEM_OTHER;
    }
    for (i = 0; i < count; i++) {
        const char  *name = names[i];
        char         path[WS_PATH_MAX];
        SDL_PathInfo info;

        /* Dot files are the file system's, not the author's: a ._x.lua
           beside x.lua must not read as a second script. */
        if (name == NULL || name[0] == '\0' || name[0] == '.' ||
            strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
            continue;
        }
        if (SDL_strcasecmp(name, "skin.ini") == 0 || wsHasExt(name, ".wsf") ||
            wsHasExt(name, ".zip")) {
            skin = true;
            continue;
        }
        if (!wsIsContentName(name)) continue;
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", folder, name) >=
                sizeof(path) ||
            !SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) {
            continue;
        }
        content++;
        if (content == 1) {
            tooLong = strlen(name) >= sizeof(found);
            SDL_strlcpy(found, name, sizeof(found));
        }
    }
    SDL_free(names);

    if (skin) {
        kind = WORKSHOP_ITEM_SKIN;
    } else if (content == 0) {
        kind = WORKSHOP_ITEM_OTHER;
        if (why != NULL) *why = "it holds no .map, .scenario or .lua file";
    } else if (content > 1) {
        kind = WORKSHOP_ITEM_OTHER;
        if (why != NULL) {
            *why = "it holds more than one .map, .scenario or .lua file";
        }
    } else if (tooLong || !wsNameOk(found)) {
        kind = WORKSHOP_ITEM_OTHER;
        if (why != NULL) *why = "its file name is too long or not usable";
    } else {
        kind = WORKSHOP_ITEM_CONTENT;
        if (file != NULL && fileSize > 0) {
            SDL_strlcpy(file, found, fileSize);
        }
    }
    return kind;
}

/* ── The index ────────────────────────────────────────────────────── */

typedef struct {
    uint64_t id;
    char     file[WORKSHOP_SYNC_FILE_MAX];
    int64_t  size;      /* the source file's, in bytes */
    int64_t  mtime;     /* the source file's, in milliseconds since the
                           epoch: a JSON number holds that exactly, where
                           SDL's nanoseconds would lose their last digits */
} WsRow;

typedef struct {
    WsRow *rows;
    int    count;
    int    cap;
} WsIndex;

static WsRow *wsIndexFind(WsIndex *ix, uint64_t id) {
    int i;

    for (i = 0; i < ix->count; i++) {
        if (ix->rows[i].id == id) return &ix->rows[i];
    }
    return NULL;
}

static WsRow *wsIndexAdd(WsIndex *ix, uint64_t id) {
    WsRow *row;

    if (ix->count == ix->cap) {
        int    cap  = (ix->cap > 0) ? ix->cap * 2 : 16;
        WsRow *more = (WsRow *)realloc(ix->rows, (size_t)cap * sizeof(*more));

        if (more == NULL) return NULL;
        ix->rows = more;
        ix->cap  = cap;
    }
    row = &ix->rows[ix->count++];
    memset(row, 0, sizeof(*row));
    row->id = id;
    return row;
}

static void wsIndexDrop(WsIndex *ix, int i) {
    if (i < 0 || i >= ix->count) return;
    if (i + 1 < ix->count) {
        memmove(&ix->rows[i], &ix->rows[i + 1],
                (size_t)(ix->count - i - 1) * sizeof(ix->rows[0]));
    }
    ix->count--;
}

static bool wsPath(char *out, size_t outLen, const char *dir,
                   const char *prefix, const char *name) {
    return (size_t)snprintf(out, outLen, "%s/%s%s", dir, prefix, name) <
           outLen;
}

/* dir/workshop.json into ix. A missing index is an empty one and says
   nothing; one that cannot be read or parsed is empty too, with one line
   in the log. A row that is not whole, or names a file this directory may
   not hold, is left out. */
static void wsIndexLoad(const char *dir, WsIndex *ix) {
    char         path[WS_PATH_MAX];
    SDL_PathInfo info;
    void        *data;
    size_t       len = 0;
    cJSON       *root;
    cJSON       *it;

    if (!wsPath(path, sizeof(path), dir, "", WORKSHOP_SYNC_INDEX) ||
        !SDL_GetPathInfo(path, &info)) {
        return;
    }
    /* SDL_LoadFile ends what it read with a terminator, so it parses as a
       string. */
    data = SDL_LoadFile(path, &len);
    if (data == NULL) {
        WB_LOG_WARN(WB_LOG_CAT_CLIENT, "workshop: %s could not be read; "
                    "starting from an empty index", path);
        return;
    }
    root = cJSON_Parse((const char *)data);
    SDL_free(data);
    if (!cJSON_IsArray(root)) {
        WB_LOG_WARN(WB_LOG_CAT_CLIENT, "workshop: %s is not a JSON array; "
                    "starting from an empty index", path);
        cJSON_Delete(root);
        return;
    }
    cJSON_ArrayForEach(it, root) {
        const cJSON *jid   = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON *jfile = cJSON_GetObjectItemCaseSensitive(it, "file");
        const cJSON *jsize = cJSON_GetObjectItemCaseSensitive(it, "size");
        const cJSON *jtime = cJSON_GetObjectItemCaseSensitive(it, "mtime");
        uint64_t     id    = 0;
        WsRow       *row;

        if (!cJSON_IsString(jid) || !cJSON_IsString(jfile) ||
            !cJSON_IsNumber(jsize) || !cJSON_IsNumber(jtime) ||
            !scnManifestParseId(jid->valuestring, &id) || id == 0 ||
            !wsNameOk(jfile->valuestring) ||
            !wsIsContentName(jfile->valuestring) ||
            wsIndexFind(ix, id) != NULL) {
            continue;
        }
        row = wsIndexAdd(ix, id);
        if (row == NULL) break;
        SDL_strlcpy(row->file, jfile->valuestring, sizeof(row->file));
        row->size  = (int64_t)jsize->valuedouble;
        row->mtime = (int64_t)jtime->valuedouble;
    }
    cJSON_Delete(root);
}

/* ix as dir/workshop.json, written to a temporary file and renamed over the
   old one, so a failed write leaves the last index whole. */
static bool wsIndexWrite(const char *dir, const WsIndex *ix) {
    char          path[WS_PATH_MAX];
    char          tmp[WS_PATH_MAX];
    cJSON        *root;
    char         *text;
    SDL_IOStream *io;
    size_t        len;
    bool          ok;
    int           i;

    if (!wsPath(path, sizeof(path), dir, "", WORKSHOP_SYNC_INDEX) ||
        !wsPath(tmp, sizeof(tmp), dir, "", WORKSHOP_SYNC_INDEX ".tmp")) {
        return false;
    }
    root = cJSON_CreateArray();
    if (root == NULL) return false;
    for (i = 0; i < ix->count; i++) {
        const WsRow *row = &ix->rows[i];
        cJSON       *obj = cJSON_CreateObject();
        char         id[32];

        if (obj == NULL) {
            cJSON_Delete(root);
            return false;
        }
        cJSON_AddItemToArray(root, obj);
        snprintf(id, sizeof(id), "%" PRIu64, row->id);
        if (cJSON_AddStringToObject(obj, "id", id) == NULL ||
            cJSON_AddStringToObject(obj, "file", row->file) == NULL ||
            cJSON_AddNumberToObject(obj, "size", (double)row->size) == NULL ||
            cJSON_AddNumberToObject(obj, "mtime", (double)row->mtime) ==
                NULL) {
            cJSON_Delete(root);
            return false;
        }
    }
    text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text == NULL) return false;

    len = strlen(text);
    io  = SDL_IOFromFile(tmp, "wb");
    ok  = io != NULL;
    if (ok) {
        ok = SDL_WriteIO(io, text, len) == len;
        if (!SDL_CloseIO(io)) ok = false;
    }
    cJSON_free(text);
    if (!ok || !SDL_RenamePath(tmp, path)) {
        SDL_RemovePath(tmp);
        WB_LOG_WARN(WB_LOG_CAT_CLIENT, "workshop: %s could not be written",
                    path);
        return false;
    }
    return true;
}

/* ── One file in, one file out ────────────────────────────────────── */

/* from into dir/name, read through memory the way the skin publish copies a
   file, written to a dotted temporary name and renamed onto name. A failure
   leaves nothing behind and whatever dir/name held before. */
static bool wsCopyIn(const char *from, const char *dir, const char *name) {
    char          tmp[WS_PATH_MAX];
    char          dest[WS_PATH_MAX];
    size_t        len  = 0;
    void         *data;
    SDL_IOStream *io;
    bool          ok;

    if (!wsPath(tmp, sizeof(tmp), dir, WS_TEMP_PREFIX, name) ||
        !wsPath(dest, sizeof(dest), dir, "", name)) {
        return false;
    }
    data = SDL_LoadFile(from, &len);
    if (data == NULL) return false;
    /* Asked again of what was read: the file can grow between the size
       check and the read. */
    if (len > wsCap(name)) {
        SDL_free(data);
        return false;
    }
    io = SDL_IOFromFile(tmp, "wb");
    if (io == NULL) {
        SDL_free(data);
        return false;
    }
    ok = (len == 0 || SDL_WriteIO(io, data, len) == len);
    if (!SDL_CloseIO(io)) ok = false;
    SDL_free(data);
    if (!ok || !SDL_RenamePath(tmp, dest)) {
        SDL_RemovePath(tmp);
        return false;
    }
    return true;
}

/* dir/name gone. True when it is not there afterwards, which includes a
   file somebody else removed first. */
static bool wsRemove(const char *dir, const char *name) {
    char         path[WS_PATH_MAX];
    SDL_PathInfo info;

    if (!wsNameOk(name) || !wsPath(path, sizeof(path), dir, "", name)) {
        return false;
    }
    if (!SDL_GetPathInfo(path, &info)) return true;
    return SDL_RemovePath(path);
}

/* ── A pass ───────────────────────────────────────────────────────── */

typedef struct {
    uint64_t id;
    char     folder[WS_FOLDER_MAX];
} WsItem;

static int wsItemCmp(const void *a, const void *b) {
    uint64_t ia = ((const WsItem *)a)->id;
    uint64_t ib = ((const WsItem *)b)->id;

    return (ia < ib) ? -1 : (ia > ib) ? 1 : 0;
}

static bool wsIdIn(const uint64_t *ids, int count, uint64_t id) {
    int i;

    for (i = 0; i < count; i++) {
        if (ids[i] == id) return true;
    }
    return false;
}

static bool wsNameIn(char (*names)[WORKSHOP_SYNC_FILE_MAX], int count,
                     const char *name) {
    int i;

    for (i = 0; i < count; i++) {
        if (SDL_strcasecmp(names[i], name) == 0) return true;
    }
    return false;
}

/* The files in dir that no index row names: somebody else's, which a pass
   never writes over or removes. Collected once, before anything is copied,
   and kept as file names in *out; the caller frees it. */
static int wsStrays(const char *dir, const WsIndex *ix,
                    char (**out)[WORKSHOP_SYNC_FILE_MAX]) {
    char **names;
    char (*strays)[WORKSHOP_SYNC_FILE_MAX] = NULL;
    int    count = 0;
    int    n     = 0;
    int    i;

    *out  = NULL;
    names = SDL_GlobDirectory(dir, "*", 0, &count);
    if (names == NULL) return 0;
    if (count > 0) {
        strays = calloc((size_t)count, sizeof(*strays));
    }
    for (i = 0; i < count && strays != NULL; i++) {
        bool named = false;
        int  r;

        if (!wsNameOk(names[i])) continue;
        for (r = 0; r < ix->count; r++) {
            if (SDL_strcasecmp(ix->rows[r].file, names[i]) == 0) {
                named = true;
                break;
            }
        }
        if (!named) SDL_strlcpy(strays[n++], names[i], sizeof(strays[0]));
    }
    SDL_free(names);
    *out = strays;
    return n;
}

bool workshopSyncRunWith(const WorkshopSyncSource *src, const char *dir,
                         WorkshopSyncReport *rep) {
    WorkshopSyncReport  local;
    WsIndex             ix      = { NULL, 0, 0 };
    WsItem             *items   = NULL;
    uint64_t           *subs    = NULL;   /* every subscribed id this pass */
    char              (*claimed)[WORKSHOP_SYNC_FILE_MAX] = NULL;
    char              (*strays)[WORKSHOP_SYNC_FILE_MAX]  = NULL;
    int                 total;
    int                 installed = 0;
    int                 nSubs     = 0;
    int                 nClaimed  = 0;
    int                 nStrays;
    bool                changed   = false;
    int                 i;

    if (rep == NULL) rep = &local;
    memset(rep, 0, sizeof(*rep));
    if (src == NULL || src->available == NULL || src->count == NULL ||
        src->item == NULL || dir == NULL || dir[0] == '\0') {
        return false;
    }
    /* Before anything else, and nothing else if it says no. */
    if (!src->available()) return false;
    if (!SDL_CreateDirectory(dir)) {
        WB_LOG_WARN(WB_LOG_CAT_CLIENT, "workshop: %s could not be made", dir);
        return false;
    }

    wsIndexLoad(dir, &ix);
    nStrays = wsStrays(dir, &ix, &strays);

    total = src->count();
    if (total < 0) total = 0;
    if (total > 0) {
        items   = (WsItem *)calloc((size_t)total, sizeof(*items));
        subs    = (uint64_t *)calloc((size_t)total, sizeof(*subs));
        claimed = calloc((size_t)total, sizeof(*claimed));
        if (items == NULL || subs == NULL || claimed == NULL) {
            /* Nothing is removed on a pass that could not see every
               subscription. */
            free(items);
            free(subs);
            free(claimed);
            free(strays);
            free(ix.rows);
            return false;
        }
    }

    /* What is subscribed, and which of it is installed. */
    for (i = 0; i < total; i++) {
        uint64_t id = 0;
        char     folder[WS_FOLDER_MAX];

        folder[0] = '\0';
        if (src->item(i, &id, folder, sizeof(folder))) {
            if (id == 0) continue;
            items[installed].id = id;
            SDL_strlcpy(items[installed].folder, folder,
                        sizeof(items[installed].folder));
            installed++;
            subs[nSubs++] = id;
        } else if (id != 0) {
            /* Still downloading: its row and file stay as they are until
               it arrives. */
            if (src->requestDownload != NULL) src->requestDownload(id);
            subs[nSubs++] = id;
            rep->pending++;
        }
    }

    /* The lower id wins a file name, so which item a clash keeps does not
       depend on the order Steam lists them in. */
    if (installed > 1) {
        qsort(items, (size_t)installed, sizeof(items[0]), wsItemCmp);
    }

    for (i = 0; i < installed; i++) {
        const WsItem    *item = &items[i];
        char             name[WORKSHOP_SYNC_FILE_MAX];
        char             from[WS_PATH_MAX];
        const char      *why = NULL;
        SDL_PathInfo     info;
        WsRow           *row;
        WorkshopItemKind kind;
        int64_t          mtime;
        bool             copy;
        int              r;

        kind = workshopSyncClassify(item->folder, name, sizeof(name), &why);
        if (kind == WORKSHOP_ITEM_SKIN) {
            /* The skin picker reads it where Steam put it. */
            rep->skipped++;
            continue;
        }
        if (kind != WORKSHOP_ITEM_CONTENT) {
            WB_LOG_INFO(WB_LOG_CAT_CLIENT, "workshop: item %" PRIu64
                        " skipped: %s", item->id, why);
            rep->skipped++;
            continue;
        }
        if (wsNameIn(claimed, nClaimed, name)) {
            WB_LOG_INFO(WB_LOG_CAT_CLIENT, "workshop: item %" PRIu64
                        " skipped: a lower item already holds %s",
                        item->id, name);
            rep->skipped++;
            continue;
        }
        if (wsNameIn(strays, nStrays, name)) {
            WB_LOG_INFO(WB_LOG_CAT_CLIENT, "workshop: item %" PRIu64
                        " skipped: %s is already in the directory and was "
                        "not put there by the sync", item->id, name);
            rep->skipped++;
            continue;
        }
        SDL_strlcpy(claimed[nClaimed++], name, sizeof(claimed[0]));

        if (!wsPath(from, sizeof(from), item->folder, "", name) ||
            !SDL_GetPathInfo(from, &info) ||
            info.type != SDL_PATHTYPE_FILE) {
            WB_LOG_INFO(WB_LOG_CAT_CLIENT, "workshop: item %" PRIu64
                        " skipped: %s could not be read", item->id, name);
            rep->skipped++;
            continue;
        }
        if ((uint64_t)info.size > (uint64_t)wsCap(name)) {
            WB_LOG_INFO(WB_LOG_CAT_CLIENT, "workshop: item %" PRIu64
                        " skipped: %s is %" PRIu64 " bytes, over the %lu a "
                        "server takes", item->id, name, (uint64_t)info.size,
                        (unsigned long)wsCap(name));
            rep->skipped++;
            continue;
        }
        mtime = (int64_t)SDL_NS_TO_MS(info.modify_time);

        /* Another item's row naming this file loses it: the file is about to
           be this item's, so that row must not remove it later. */
        copy = false;
        for (r = 0; r < ix.count;) {
            if (ix.rows[r].id != item->id &&
                SDL_strcasecmp(ix.rows[r].file, name) == 0) {
                wsIndexDrop(&ix, r);
                changed = true;
                copy    = true;
                continue;
            }
            r++;
        }

        row = wsIndexFind(&ix, item->id);
        if (row != NULL && strcmp(row->file, name) != 0) {
            /* The item's file changed its name: the old one goes first, and
               its row with it, so a failed copy leaves no row naming a file
               that is gone. */
            if (wsRemove(dir, row->file)) {
                serverSimNoteScriptDirsChanged();
                wsIndexDrop(&ix, (int)(row - ix.rows));
                changed = true;
                row     = NULL;
            } else {
                WB_LOG_INFO(WB_LOG_CAT_CLIENT, "workshop: item %" PRIu64
                            " kept: its old file %s could not be removed",
                            item->id, row->file);
                rep->skipped++;
                continue;
            }
        }
        if (row == NULL || row->size != (int64_t)info.size ||
            row->mtime != mtime) {
            copy = true;
        }
        if (!copy) {
            /* A file the player removed by hand comes back. */
            char         dest[WS_PATH_MAX];
            SDL_PathInfo have;

            copy = !wsPath(dest, sizeof(dest), dir, "", name) ||
                   !SDL_GetPathInfo(dest, &have);
        }
        if (!copy) continue;

        if (!wsCopyIn(from, dir, name)) {
            WB_LOG_INFO(WB_LOG_CAT_CLIENT, "workshop: item %" PRIu64
                        " skipped: %s could not be copied", item->id, name);
            rep->skipped++;
            continue;
        }
        serverSimNoteScriptDirsChanged();
        if (row == NULL) row = wsIndexAdd(&ix, item->id);
        if (row != NULL) {
            SDL_strlcpy(row->file, name, sizeof(row->file));
            row->size  = (int64_t)info.size;
            row->mtime = mtime;
        }
        changed = true;
        rep->copied++;
    }

    /* A row whose item is no longer subscribed takes its file with it. A
       file that will not go keeps its row, so the next pass tries again. */
    for (i = 0; i < ix.count;) {
        if (wsIdIn(subs, nSubs, ix.rows[i].id)) {
            i++;
            continue;
        }
        if (!wsRemove(dir, ix.rows[i].file)) {
            WB_LOG_INFO(WB_LOG_CAT_CLIENT, "workshop: %s could not be removed",
                        ix.rows[i].file);
            i++;
            continue;
        }
        serverSimNoteScriptDirsChanged();
        wsIndexDrop(&ix, i);
        changed = true;
        rep->removed++;
    }

    if (changed) {
        (void)wsIndexWrite(dir, &ix);
    }

    free(items);
    free(subs);
    free(claimed);
    free(strays);
    free(ix.rows);
    return true;
}

/* ── With Steam ───────────────────────────────────────────────────── */

static uint32_t s_wsGeneration;

void workshopSyncRun(void) {
    static const WorkshopSyncSource steam = {
        steam_workshop_available,
        steam_workshop_subscribed_count,
        steam_workshop_item,
        steam_workshop_request_download,
    };
    WorkshopSyncReport rep;
    char               dir[WS_FOLDER_MAX];

    /* Asked here as well as in the pass so a build with no Steam names no
       directory and logs nothing. */
    if (!steam_workshop_available()) return;
    if (!scenarioHostWorkshopDir(dir, sizeof(dir))) {
        WB_LOG_WARN(WB_LOG_CAT_CLIENT, "workshop: no directory to copy "
                    "subscribed items to");
        return;
    }
    if (!workshopSyncRunWith(&steam, dir, &rep)) return;
    WB_LOG_INFO(WB_LOG_CAT_CLIENT, "workshop: %d copied, %d removed, "
                "%d downloading, %d skipped", rep.copied, rep.removed,
                rep.pending, rep.skipped);
}

void workshopSyncPoll(void) {
    if (!steam_workshop_consume_installed_event()) return;
    workshopSyncRun();
    s_wsGeneration++;
}

uint32_t workshopSyncGeneration(void) {
    return s_wsGeneration;
}

/* ── The index, for a list ────────────────────────────────────────── */

static int wsRowIdCmp(const void *a, const void *b) {
    uint64_t ia = ((const WsRow *)a)->id;
    uint64_t ib = ((const WsRow *)b)->id;

    return (ia > ib) - (ia < ib);
}

int workshopSyncIndexRows(WorkshopSyncRow *out, int max) {
    char    dir[WS_FOLDER_MAX];
    WsIndex ix;
    int     n = 0;
    int     i;

    if (out == NULL || max <= 0) return 0;
    if (!scenarioHostWorkshopDir(dir, sizeof(dir))) return 0;

    memset(&ix, 0, sizeof(ix));
    wsIndexLoad(dir, &ix);
    if (ix.count > 1) {
        qsort(ix.rows, (size_t)ix.count, sizeof(ix.rows[0]), wsRowIdCmp);
    }
    for (i = 0; i < ix.count && n < max; i++) {
        out[n].id = ix.rows[i].id;
        SDL_strlcpy(out[n].file, ix.rows[i].file, sizeof(out[n].file));
        n++;
    }
    free(ix.rows);
    return n;
}
