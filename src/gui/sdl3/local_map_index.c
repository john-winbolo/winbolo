/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          local_map_index.c
 * Purpose:       Finds a .map file on this machine by the
 *                md5 of its bytes. See local_map_index.h.
 *********************************************************/

#include "local_map_index.h"

#include <string.h>

#include <SDL3/SDL.h>

#include "../../common/md5.h"

/* What every .map file starts with. bolo_map.h has the same text as
 * MAP_HEADER, in a header this folder may not include. */
static const char kMapMagic[] = "BMAPBOLO";
#define MAP_MAGIC_LEN (sizeof(kMapMagic) - 1)

typedef struct {
    char    *path;       /* root + '/' + path below the root */
    Uint64   size;
    SDL_Time modified;
    char     md5[33];    /* lowercase hex; "" for a file that is not a map */
    bool     seen;       /* met by the scan in progress */
} LocalMapEntry;

struct LocalMapIndex {
    LocalMapEntry *entries;
    int            count;
    int            cap;
};

LocalMapIndex *localMapIndexCreate(void) {
    return (LocalMapIndex *)SDL_calloc(1, sizeof(LocalMapIndex));
}

void localMapIndexDestroy(LocalMapIndex *idx) {
    int i;
    if (idx == NULL) {
        return;
    }
    for (i = 0; i < idx->count; i++) {
        SDL_free(idx->entries[i].path);
    }
    SDL_free(idx->entries);
    SDL_free(idx);
}

static bool hasMapExtension(const char *name) {
    size_t len = SDL_strlen(name);
    return len > 4 && SDL_strcasecmp(name + len - 4, ".map") == 0;
}

/* Hashes the file at `path` into out, or leaves out empty when the file
 * cannot be read or does not start with the map header. */
static void hashMapFile(const char *path, char out[33]) {
    size_t  len = 0;
    void   *data = SDL_LoadFile(path, &len);
    uint8_t digest[16];

    out[0] = '\0';
    if (data == NULL) {
        return;
    }
    if (len >= MAP_MAGIC_LEN && len <= LOCAL_MAP_INDEX_MAX_FILE &&
        memcmp(data, kMapMagic, MAP_MAGIC_LEN) == 0) {
        md5Compute(data, len, digest);
        md5ToHex(digest, out);
    }
    SDL_free(data);
}

static LocalMapEntry *findByPath(LocalMapIndex *idx, const char *path) {
    int i;
    for (i = 0; i < idx->count; i++) {
        if (SDL_strcmp(idx->entries[i].path, path) == 0) {
            return &idx->entries[i];
        }
    }
    return NULL;
}

/* Appends an entry that owns `path`. False (path not taken) when out of
 * memory. */
static bool appendEntry(LocalMapIndex *idx, char *path, Uint64 size,
                        SDL_Time modified) {
    LocalMapEntry *e;
    if (idx->count == idx->cap) {
        int            newCap = idx->cap == 0 ? 128 : idx->cap * 2;
        LocalMapEntry *grown = (LocalMapEntry *)SDL_realloc(
            idx->entries, (size_t)newCap * sizeof(LocalMapEntry));
        if (grown == NULL) {
            return false;
        }
        idx->entries = grown;
        idx->cap = newCap;
    }
    e = &idx->entries[idx->count++];
    e->path = path;
    e->size = size;
    e->modified = modified;
    e->md5[0] = '\0';
    e->seen = true;
    return true;
}

/* Scans one root. Returns how many files it hashed. */
static int scanRoot(LocalMapIndex *idx, const char *root) {
    int    hashed = 0;
    int    n = 0;
    int    i;
    char **list;

    if (root == NULL || root[0] == '\0') {
        return 0;
    }
    /* A NULL pattern lists every file and folder below the root, each as a
     * path relative to it. */
    list = SDL_GlobDirectory(root, NULL, 0, &n);
    if (list == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        SDL_PathInfo   info;
        char          *path = NULL;
        LocalMapEntry *e;

        if (!hasMapExtension(list[i])) {
            continue;
        }
        if (SDL_asprintf(&path, "%s/%s", root, list[i]) < 0 || path == NULL) {
            continue;
        }
        if (!SDL_GetPathInfo(path, &info) ||
            info.type != SDL_PATHTYPE_FILE ||
            info.size > LOCAL_MAP_INDEX_MAX_FILE) {
            SDL_free(path);
            continue;
        }
        e = findByPath(idx, path);
        if (e != NULL) {
            SDL_free(path);
            e->seen = true;
            if (e->size == info.size && e->modified == info.modify_time) {
                continue;  /* unchanged: keep the md5 it has */
            }
            e->size = info.size;
            e->modified = info.modify_time;
        } else {
            if (!appendEntry(idx, path, info.size, info.modify_time)) {
                SDL_free(path);
                continue;
            }
            e = &idx->entries[idx->count - 1];
        }
        hashMapFile(e->path, e->md5);
        hashed++;
    }
    SDL_free(list);
    return hashed;
}

int localMapIndexScan(LocalMapIndex *idx, const char *const *roots,
                      int rootCount) {
    int hashed = 0;
    int kept = 0;
    int i;

    if (idx == NULL) {
        return 0;
    }
    for (i = 0; i < idx->count; i++) {
        idx->entries[i].seen = false;
    }
    for (i = 0; roots != NULL && i < rootCount; i++) {
        hashed += scanRoot(idx, roots[i]);
    }
    /* Drop what no root holds any more, keeping the order of the rest: a
     * file from an earlier root stays ahead of its copy in a later one. */
    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].seen) {
            if (kept != i) {
                idx->entries[kept] = idx->entries[i];
            }
            kept++;
        } else {
            SDL_free(idx->entries[i].path);
        }
    }
    idx->count = kept;
    return hashed;
}

int localMapIndexCount(const LocalMapIndex *idx) {
    return idx != NULL ? idx->count : 0;
}

const char *localMapIndexFind(const LocalMapIndex *idx, const char *md5Hex) {
    int i;
    if (idx == NULL || md5Hex == NULL || SDL_strlen(md5Hex) != 32) {
        return NULL;
    }
    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].md5[0] != '\0' &&
            SDL_strcasecmp(idx->entries[i].md5, md5Hex) == 0) {
            return idx->entries[i].path;
        }
    }
    return NULL;
}

bool localMapIndexLoad(const LocalMapIndex *idx, const char *md5Hex,
                       uint8_t **bytesOut, size_t *lenOut) {
    const char *path;
    void       *data;
    size_t      len = 0;
    uint8_t     digest[16];
    char        hex[33];

    if (bytesOut != NULL) {
        *bytesOut = NULL;
    }
    if (lenOut != NULL) {
        *lenOut = 0;
    }
    if (bytesOut == NULL || lenOut == NULL) {
        return false;
    }
    path = localMapIndexFind(idx, md5Hex);
    if (path == NULL) {
        return false;
    }
    data = SDL_LoadFile(path, &len);
    if (data == NULL) {
        return false;
    }
    if (len > LOCAL_MAP_INDEX_MAX_FILE) {
        SDL_free(data);
        return false;
    }
    md5Compute(data, len, digest);
    md5ToHex(digest, hex);
    if (SDL_strcasecmp(hex, md5Hex) != 0) {
        SDL_free(data);
        return false;
    }
    *bytesOut = (uint8_t *)data;
    *lenOut = len;
    return true;
}
