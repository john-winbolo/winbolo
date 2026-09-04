/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          skin_source.c
 * Purpose:
 *   Reads skin assets out of a directory or a .wsf/.zip
 *   archive through one name index, tracks the active
 *   skin, and enumerates the skins installed on disk.
 *********************************************************/

#include "skin_source.h"

#include <SDL3/SDL.h>

#include "../../common/wb_log.h"
#include "../../steam/steam_wrapper.h"

#include "unzip.h"
#include "zip.h"

#if defined(__IPHONEOS__)
#include <dirent.h>
#endif

/* One indexed name. `key` is the normalised lookup name; a directory source
 * keeps the on-disk relative name in `rel` (original case, so the lookup
 * stays case-insensitive on a case-sensitive filesystem), a zip source keeps
 * the central-directory position and the uncompressed size. */
typedef struct SkinIndexEntry {
    char         *key;
    char         *rel;
    unz_file_pos  pos;
    uLong         size;
    int           next;   /* next entry in this bucket, -1 = end of chain */
} SkinIndexEntry;

struct SkinSource {
    bool            isZip;
    char            path[SKIN_PATH_MAX];   /* directory, or the archive file */
    unzFile         zip;                   /* NULL for a directory source */
    SkinIndexEntry *entries;
    int             count;
    int             cap;
    int            *buckets;
    int             bucketCount;           /* always a power of two */
    /* skin.ini, parsed on the first skinSourceReadIni and kept: the settings
     * tab asks for it every frame, and on a zip source each read is an
     * inflate. A source is immutable once open, so it never goes stale. */
    bool            iniLoaded;
    SkinInfo        ini;
    /* Never reused, unlike the address: a cache keyed on this cannot
     * mistake a source opened into a freed one's block for that source. */
    uint64_t        serial;
};

static char        s_activeId[SKIN_ID_MAX];
static char        s_requestedId[SKIN_ID_MAX];
static SkinSource *s_activeSource;
static uint64_t    s_nextSerial = 1;   /* 0 is reserved for "no source" */

/* ------------------------------------------------------------------ */
/* Name handling                                                       */
/* ------------------------------------------------------------------ */

/* Backslashes to forward slashes, ASCII lowercase, no leading "./" or "/". */
static void normaliseName(const char *in, char *out, size_t outLen) {
    size_t o = 0;
    if (outLen == 0) return;
    out[0] = '\0';
    if (!in) return;
    while (*in == '/' || *in == '\\' ||
           (in[0] == '.' && (in[1] == '/' || in[1] == '\\'))) {
        in += (in[0] == '.') ? 2 : 1;
    }
    for (; *in != '\0' && o + 1 < outLen; in++) {
        char c = *in;
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[o++] = c;
    }
    out[o] = '\0';
}

static unsigned int hashName(const char *s) {
    unsigned int h = 2166136261u;
    while (*s != '\0') {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* True for a name ending in .wsf or .zip. */
static bool hasArchiveExt(const char *name) {
    size_t len = name ? SDL_strlen(name) : 0;
    if (len < 5) return false;
    return SDL_strcasecmp(name + len - 4, ".wsf") == 0 ||
           SDL_strcasecmp(name + len - 4, ".zip") == 0;
}

/* True for anything a skin directory would hold besides an archive. */
static bool isSkinLikeName(const char *name) {
    size_t len = name ? SDL_strlen(name) : 0;
    if (len == 0) return false;
    if (SDL_strcasecmp(name, "skin.ini") == 0) return true;
    if (SDL_strcasecmp(name, "sounds") == 0) return true;
    if (len < 5) return false;
    return SDL_strcasecmp(name + len - 4, ".png") == 0 ||
           SDL_strcasecmp(name + len - 4, ".svg") == 0 ||
           SDL_strcasecmp(name + len - 4, ".bmp") == 0 ||
           SDL_strcasecmp(name + len - 4, ".wav") == 0;
}

/* Drops a trailing .wsf / .zip so foo.wsf and a foo/ directory share an id. */
static void stripArchiveExt(char *name) {
    size_t len = SDL_strlen(name);
    if (len >= 4) name[len - 4] = '\0';
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                   */
/* ------------------------------------------------------------------ */

/* Top-level names in dir. SDL_GlobDirectory doesn't see files inside the iOS
 * app bundle, so iOS reads the directory with opendir/readdir instead — the
 * same split gamefront.c's map scan and lang.c's language scan carry. The
 * "*" pattern stops at a path separator, so neither branch descends. */
static char **listDirectory(const char *dir, int *count) {
    *count = 0;
    if (!dir || !*dir) return NULL;

#if defined(__IPHONEOS__)
    {
        DIR *d = opendir(dir);
        char **arr;
        int cap = 16;
        struct dirent *ent;
        if (!d) return NULL;
        arr = (char **)SDL_calloc((size_t)cap, sizeof(*arr));
        if (!arr) {
            closedir(d);
            return NULL;
        }
        while ((ent = readdir(d)) != NULL) {
            if (SDL_strcmp(ent->d_name, ".") == 0 ||
                SDL_strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            if (*count >= cap) {
                int newCap = cap * 2;
                char **grown = (char **)SDL_realloc(
                    arr, (size_t)newCap * sizeof(*arr));
                if (!grown) break;
                arr = grown;
                cap = newCap;
            }
            arr[*count] = SDL_strdup(ent->d_name);
            if (!arr[*count]) break;
            (*count)++;
        }
        closedir(d);
        return arr;
    }
#else
    return SDL_GlobDirectory(dir, "*", 0, count);
#endif
}

static void freeDirectoryList(char **list, int count) {
#if defined(__IPHONEOS__)
    int i;
    if (!list) return;
    for (i = 0; i < count; i++) SDL_free(list[i]);
    SDL_free(list);
#else
    (void)count;
    SDL_free(list);
#endif
}

/* ------------------------------------------------------------------ */
/* Name index                                                          */
/* ------------------------------------------------------------------ */

static bool indexAdd(SkinSource *src, const char *key, const char *rel,
                     const unz_file_pos *pos, uLong size) {
    SkinIndexEntry *e;
    if (src->count == src->cap) {
        int newCap = src->cap ? src->cap * 2 : 32;
        SkinIndexEntry *grown = (SkinIndexEntry *)SDL_realloc(
            src->entries, (size_t)newCap * sizeof(*grown));
        if (!grown) return false;
        src->entries = grown;
        src->cap = newCap;
    }
    e = &src->entries[src->count];
    SDL_memset(e, 0, sizeof(*e));
    e->key = SDL_strdup(key);
    if (!e->key) return false;
    if (rel) {
        e->rel = SDL_strdup(rel);
        if (!e->rel) {
            SDL_free(e->key);
            return false;
        }
    }
    if (pos) e->pos = *pos;
    e->size = size;
    e->next = -1;
    src->count++;
    return true;
}

/* Chains every indexed name into a power-of-two bucket table. Built once,
 * after the whole listing is in, so lookups never touch the filesystem. */
static bool indexBuild(SkinSource *src) {
    int n = 16;
    int i;
    while (n < src->count * 2) n <<= 1;
    src->buckets = (int *)SDL_malloc((size_t)n * sizeof(int));
    if (!src->buckets) return false;
    for (i = 0; i < n; i++) src->buckets[i] = -1;
    src->bucketCount = n;
    /* Walk backwards so the head of each chain is the earliest entry: a
     * duplicated name resolves to the one the listing saw first. */
    for (i = src->count - 1; i >= 0; i--) {
        unsigned int b = hashName(src->entries[i].key) & (unsigned int)(n - 1);
        src->entries[i].next = src->buckets[b];
        src->buckets[b] = i;
    }
    return true;
}

static const SkinIndexEntry *indexFind(SkinSource *src, const char *relName) {
    char key[SKIN_PATH_MAX];
    unsigned int b;
    int i;
    if (!src || !src->buckets || src->count == 0) return NULL;
    normaliseName(relName, key, sizeof(key));
    if (key[0] == '\0') return NULL;
    b = hashName(key) & (unsigned int)(src->bucketCount - 1);
    for (i = src->buckets[b]; i >= 0; i = src->entries[i].next) {
        if (SDL_strcmp(src->entries[i].key, key) == 0) return &src->entries[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Opening                                                             */
/* ------------------------------------------------------------------ */

static SkinSource *sourceAlloc(bool isZip, const char *path) {
    SkinSource *src = (SkinSource *)SDL_calloc(1, sizeof(*src));
    if (!src) return NULL;
    src->isZip = isZip;
    SDL_strlcpy(src->path, path, sizeof(src->path));
    src->serial = s_nextSerial++;
    return src;
}

uint64_t skinSourceSerial(const SkinSource *src) {
    return src ? src->serial : 0;
}

/* If every indexed name sits under one top-level folder, drop that folder so
 * skinname/tiles.bmp is found as tiles.bmp. "sounds/" is left alone: a skin
 * whose only files are sound overrides is under it legitimately. */
static void stripCommonPrefix(SkinSource *src) {
    char prefix[SKIN_PATH_MAX];
    const char *slash;
    size_t plen;
    int i;
    if (src->count == 0) return;
    slash = SDL_strchr(src->entries[0].key, '/');
    if (!slash) return;
    plen = (size_t)(slash - src->entries[0].key) + 1;
    if (plen >= sizeof(prefix)) return;
    SDL_memcpy(prefix, src->entries[0].key, plen);
    prefix[plen] = '\0';
    if (SDL_strcmp(prefix, "sounds/") == 0) return;
    for (i = 0; i < src->count; i++) {
        if (SDL_strncmp(src->entries[i].key, prefix, plen) != 0) return;
        if (src->entries[i].key[plen] == '\0') return;
    }
    for (i = 0; i < src->count; i++) {
        char *k = src->entries[i].key;
        SDL_memmove(k, k + plen, SDL_strlen(k + plen) + 1);
    }
}

/* Walks the central directory once, recording each entry's name and position.
 * unzLocateFile is a linear scan of that same directory, so a read never
 * calls it — the index answers with a position unzGoToFilePos takes. */
static SkinSource *openZip(const char *path) {
    SkinSource *src;
    unzFile zf = unzOpen(path);
    int rc;
    if (!zf) return NULL;
    src = sourceAlloc(true, path);
    if (!src) {
        unzClose(zf);
        return NULL;
    }
    src->zip = zf;

    rc = unzGoToFirstFile(zf);
    while (rc == UNZ_OK) {
        unz_file_info fi;
        char name[SKIN_PATH_MAX];
        size_t nlen;
        /* minizip fills the name buffer without terminating it, so start
         * from zeroes and give it one byte less than the buffer holds. */
        SDL_memset(name, 0, sizeof(name));
        if (unzGetCurrentFileInfo(zf, &fi, name, sizeof(name) - 1,
                                  NULL, 0, NULL, 0) != UNZ_OK) {
            break;
        }
        nlen = SDL_strlen(name);
        if (nlen > 0 && name[nlen - 1] != '/' && name[nlen - 1] != '\\') {
            unz_file_pos pos;
            if (unzGetFilePos(zf, &pos) == UNZ_OK) {
                char key[SKIN_PATH_MAX];
                normaliseName(name, key, sizeof(key));
                if (key[0] != '\0') {
                    indexAdd(src, key, NULL, &pos, fi.uncompressed_size);
                }
            }
        }
        rc = unzGoToNextFile(zf);
    }

    stripCommonPrefix(src);
    if (!indexBuild(src)) {
        skinSourceClose(src);
        return NULL;
    }
    return src;
}

/* Indexes the top level plus the sounds/ subdirectory, the two listings a
 * skin layout can put files in. Both go into the same index the zip side
 * builds, so a lookup costs the same either way. */
static SkinSource *openDirectory(const char *path) {
    SkinSource *src;
    char **top;
    int topCount = 0;
    int archives = 0;
    int skinLike = 0;
    int archiveIdx = -1;
    int i;
    char soundsDir[SKIN_PATH_MAX];
    char **snd;
    int sndCount = 0;

    top = listDirectory(path, &topCount);
    if (!top) return NULL;

    for (i = 0; i < topCount; i++) {
        if (!top[i] || top[i][0] == '\0') continue;
        if (hasArchiveExt(top[i])) {
            archives++;
            archiveIdx = i;
        } else if (isSkinLikeName(top[i])) {
            skinLike++;
        }
    }

    /* A folder holding one archive and nothing else skin-like — a Workshop
     * install, or a .wsf dropped in a folder of its own — reads as that
     * archive. */
    if (archives == 1 && skinLike == 0) {
        char nested[SKIN_PATH_MAX];
        SDL_snprintf(nested, sizeof(nested), "%s/%s", path, top[archiveIdx]);
        freeDirectoryList(top, topCount);
        return openZip(nested);
    }

    src = sourceAlloc(false, path);
    if (!src) {
        freeDirectoryList(top, topCount);
        return NULL;
    }

    for (i = 0; i < topCount; i++) {
        char key[SKIN_PATH_MAX];
        if (!top[i] || top[i][0] == '\0') continue;
        normaliseName(top[i], key, sizeof(key));
        if (key[0] != '\0') indexAdd(src, key, top[i], NULL, 0);
    }
    freeDirectoryList(top, topCount);

    SDL_snprintf(soundsDir, sizeof(soundsDir), "%s/sounds", path);
    snd = listDirectory(soundsDir, &sndCount);
    if (snd) {
        for (i = 0; i < sndCount; i++) {
            char rel[SKIN_PATH_MAX];
            char key[SKIN_PATH_MAX];
            if (!snd[i] || snd[i][0] == '\0') continue;
            SDL_snprintf(rel, sizeof(rel), "sounds/%s", snd[i]);
            normaliseName(rel, key, sizeof(key));
            if (key[0] != '\0') indexAdd(src, key, rel, NULL, 0);
        }
        freeDirectoryList(snd, sndCount);
    }

    if (!indexBuild(src)) {
        skinSourceClose(src);
        return NULL;
    }
    return src;
}

SkinSource *skinSourceOpen(const char *path) {
    SDL_PathInfo info;
    if (!path || !*path) return NULL;
    if (!SDL_GetPathInfo(path, &info)) return NULL;
    if (info.type == SDL_PATHTYPE_DIRECTORY) return openDirectory(path);
    if (info.type == SDL_PATHTYPE_FILE && hasArchiveExt(path)) {
        return openZip(path);
    }
    return NULL;
}

void skinSourceClose(SkinSource *src) {
    int i;
    if (!src) return;
    for (i = 0; i < src->count; i++) {
        SDL_free(src->entries[i].key);
        SDL_free(src->entries[i].rel);
    }
    SDL_free(src->entries);
    SDL_free(src->buckets);
    if (src->zip) unzClose(src->zip);
    SDL_free(src);
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

bool skinSourceExists(SkinSource *src, const char *relName) {
    return indexFind(src, relName) != NULL;
}

bool skinSourceRead(SkinSource *src, const char *relName,
                    void **buf, size_t *len) {
    const SkinIndexEntry *e;
    if (!src || !relName || !buf || !len) return false;
    e = indexFind(src, relName);
    if (!e) return false;

    if (!src->isZip) {
        char full[SKIN_PATH_MAX * 2];
        size_t sz = 0;
        void *data;
        SDL_snprintf(full, sizeof(full), "%s/%s", src->path, e->rel);
        /* SDL_LoadFile allocates sz + 1 and NUL-terminates at [sz]. */
        data = SDL_LoadFile(full, &sz);
        if (!data) return false;
        *buf = data;
        *len = sz;
        return true;
    }

    {
        unz_file_pos pos = e->pos;
        size_t sz = (size_t)e->size;
        size_t got = 0;
        unsigned char *data;
        if (unzGoToFilePos(src->zip, &pos) != UNZ_OK) return false;
        if (unzOpenCurrentFile(src->zip) != UNZ_OK) return false;
        data = (unsigned char *)SDL_malloc(sz + 1);
        if (!data) {
            unzCloseCurrentFile(src->zip);
            return false;
        }
        while (got < sz) {
            int n = unzReadCurrentFile(src->zip, data + got,
                                       (unsigned int)(sz - got));
            if (n <= 0) break;
            got += (size_t)n;
        }
        unzCloseCurrentFile(src->zip);
        if (got != sz) {
            SDL_free(data);
            return false;
        }
        data[sz] = '\0';
        *buf = data;
        *len = sz;
        return true;
    }
}

bool skinSourceReadHead(SkinSource *src, const char *relName,
                        void *buf, size_t max, size_t *got) {
    const SkinIndexEntry *e;
    size_t n = 0;
    if (!src || !relName || !buf || !got || max == 0) return false;
    e = indexFind(src, relName);
    if (!e) return false;

    if (!src->isZip) {
        char full[SKIN_PATH_MAX * 2];
        SDL_IOStream *io;
        SDL_snprintf(full, sizeof(full), "%s/%s", src->path, e->rel);
        io = SDL_IOFromFile(full, "rb");
        if (!io) return false;
        n = SDL_ReadIO(io, buf, max);
        SDL_CloseIO(io);
        *got = n;
        return true;
    }

    {
        unz_file_pos pos = e->pos;
        unsigned char *p = (unsigned char *)buf;
        if (unzGoToFilePos(src->zip, &pos) != UNZ_OK) return false;
        if (unzOpenCurrentFile(src->zip) != UNZ_OK) return false;
        /* Inflate only as far as the caller asked; the rest of the entry
         * stays compressed. */
        while (n < max) {
            int r = unzReadCurrentFile(src->zip, p + n, (unsigned int)(max - n));
            if (r <= 0) break;
            n += (size_t)r;
        }
        unzCloseCurrentFile(src->zip);
        *got = n;
        return true;
    }
}

/* ------------------------------------------------------------------ */
/* skin.ini                                                            */
/* ------------------------------------------------------------------ */

/* One pass over the whole [Skin] section, picking up every key it carries.
 * Runs on the loaded buffer, which skinSourceRead leaves writable and
 * NUL-terminated. */
static void parseSkinIni(char *text, SkinInfo *out) {
    bool inSection = false;
    char *p = text;
    while (p != NULL && *p != '\0') {
        char *line = p;
        char *nl = SDL_strchr(p, '\n');
        char *eq;
        char *k;
        char *kend;
        char *v;
        char *vend;
        size_t len;

        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = NULL;
        }

        len = SDL_strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                           line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        while (*line == ' ' || *line == '\t') line++;

        if (line[0] == '[') {
            inSection = (SDL_strncasecmp(line, "[Skin]", 6) == 0);
            continue;
        }
        if (!inSection) continue;

        eq = SDL_strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        k = line;
        kend = eq - 1;
        while (kend > k && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';

        v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        vend = v + SDL_strlen(v);
        while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) *--vend = '\0';

        if (SDL_strcasecmp(k, "Name") == 0) {
            SDL_strlcpy(out->name, v, sizeof(out->name));
        } else if (SDL_strcasecmp(k, "Author") == 0) {
            SDL_strlcpy(out->author, v, sizeof(out->author));
        } else if (SDL_strcasecmp(k, "Notes") == 0) {
            SDL_strlcpy(out->notes, v, sizeof(out->notes));
        } else if (SDL_strcasecmp(k, "WorkshopId") == 0) {
            out->workshopId = (uint64_t)SDL_strtoull(v, NULL, 10);
        } else if (SDL_strcasecmp(k, "MaxPixelDensity") == 0) {
            out->maxPixelDensity = SDL_atoi(v);
        } else if (SDL_strcasecmp(k, "InGameRotate") == 0) {
            out->inGameRotate = SDL_atoi(v) ? 1 : 0;
        }
    }
}

void skinSourceReadIni(SkinSource *src, SkinInfo *out) {
    if (!out) return;
    SDL_memset(out, 0, sizeof(*out));
    if (!src) return;
    if (!src->iniLoaded) {
        void *buf = NULL;
        size_t len = 0;
        SDL_memset(&src->ini, 0, sizeof(src->ini));
        if (skinSourceRead(src, "skin.ini", &buf, &len)) {
            parseSkinIni((char *)buf, &src->ini);
            SDL_free(buf);
        }
        /* A missing or unreadable ini is an answer too: an empty SkinInfo,
         * and no reason to go looking again. */
        src->iniLoaded = true;
    }
    *out = src->ini;
}

/* ------------------------------------------------------------------ */
/* Writing a zip                                                       */
/* ------------------------------------------------------------------ */

static bool zipAddFile(zipFile zf, const char *srcPath, const char *entryName) {
    zip_fileinfo zi;
    size_t len = 0;
    void *data = SDL_LoadFile(srcPath, &len);
    bool ok = true;
    if (!data) return false;
    SDL_memset(&zi, 0, sizeof(zi));
    if (zipOpenNewFileInZip(zf, entryName, &zi, NULL, 0, NULL, 0, NULL,
                            Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK) {
        SDL_free(data);
        return false;
    }
    if (len > 0 &&
        zipWriteInFileInZip(zf, data, (unsigned int)len) != ZIP_OK) {
        ok = false;
    }
    if (zipCloseFileInZip(zf) != ZIP_OK) ok = false;
    SDL_free(data);
    return ok;
}

/* Adds every file directly inside dir. relPrefix names the folder inside the
 * skin ("sounds", or NULL for the top level); wrapFolder prefixes the whole
 * archive. A missing directory is only a failure when required. */
static bool zipAddLevel(zipFile zf, const char *dir, const char *relPrefix,
                        const char *wrapFolder, bool required) {
    char **list;
    int count = 0;
    int i;
    bool ok = true;

    list = listDirectory(dir, &count);
    if (!list) return !required;

    for (i = 0; i < count && ok; i++) {
        char full[SKIN_PATH_MAX * 2];
        char entry[SKIN_PATH_MAX * 2];
        SDL_PathInfo info;
        if (!list[i] || list[i][0] == '\0') continue;
        SDL_snprintf(full, sizeof(full), "%s/%s", dir, list[i]);
        if (!SDL_GetPathInfo(full, &info) || info.type != SDL_PATHTYPE_FILE) {
            continue;
        }
        if (wrapFolder && relPrefix) {
            SDL_snprintf(entry, sizeof(entry), "%s/%s/%s",
                         wrapFolder, relPrefix, list[i]);
        } else if (wrapFolder) {
            SDL_snprintf(entry, sizeof(entry), "%s/%s", wrapFolder, list[i]);
        } else if (relPrefix) {
            SDL_snprintf(entry, sizeof(entry), "%s/%s", relPrefix, list[i]);
        } else {
            SDL_snprintf(entry, sizeof(entry), "%s", list[i]);
        }
        ok = zipAddFile(zf, full, entry);
    }

    freeDirectoryList(list, count);
    return ok;
}

bool skinSourceZipDirectory(const char *dir, const char *outZip,
                            const char *wrapFolder) {
    char soundsDir[SKIN_PATH_MAX];
    zipFile zf;
    bool ok;

    if (!dir || !*dir || !outZip || !*outZip) return false;
    zf = zipOpen(outZip, APPEND_STATUS_CREATE);
    if (!zf) return false;

    ok = zipAddLevel(zf, dir, NULL, wrapFolder, true);
    if (ok) {
        SDL_snprintf(soundsDir, sizeof(soundsDir), "%s/sounds", dir);
        ok = zipAddLevel(zf, soundsDir, "sounds", wrapFolder, false);
    }
    if (zipClose(zf, NULL) != ZIP_OK) ok = false;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

typedef struct ScanState {
    SkinEntry *out;
    int        max;
    int        written;
    int        total;
    char     (*seen)[SKIN_ID_MAX];
    int        seenCount;
    int        seenCap;
} ScanState;

/* True when this id was already claimed by a higher-priority location. */
static bool scanSeen(ScanState *st, const char *id) {
    int i;
    for (i = 0; i < st->seenCount; i++) {
        if (SDL_strcmp(st->seen[i], id) == 0) return true;
    }
    if (st->seenCount == st->seenCap) {
        int newCap = st->seenCap ? st->seenCap * 2 : 32;
        char (*grown)[SKIN_ID_MAX] = (char (*)[SKIN_ID_MAX])SDL_realloc(
            st->seen, (size_t)newCap * SKIN_ID_MAX);
        if (!grown) return true;   /* out of memory: stop taking new ids */
        st->seen = grown;
        st->seenCap = newCap;
    }
    SDL_strlcpy(st->seen[st->seenCount], id, SKIN_ID_MAX);
    st->seenCount++;
    return false;
}

static void scanRecord(ScanState *st, const char *id, const char *base,
                       const char *path, SkinKind kind) {
    SkinEntry *e;
    SkinInfo info;
    SkinSource *src;

    st->total++;
    if (!st->out || st->written >= st->max) return;

    e = &st->out[st->written++];
    SDL_memset(e, 0, sizeof(*e));
    SDL_strlcpy(e->id, id, sizeof(e->id));
    SDL_strlcpy(e->path, path, sizeof(e->path));
    e->kind = kind;

    /* Display name comes from skin.ini when the skin carries one. Opening
     * every candidate to read it is the price of the scan. */
    SDL_memset(&info, 0, sizeof(info));
    src = skinSourceOpen(path);
    if (src) {
        skinSourceReadIni(src, &info);
        skinSourceClose(src);
    }
    SDL_strlcpy(e->displayName, info.name[0] ? info.name : base,
                sizeof(e->displayName));
}

/* A Workshop item the user is subscribed to whose files Steam has not
 * delivered yet. There is no folder to open, so there is no skin.ini to take
 * a name from and the published file id stands in for one. */
static void scanRecordPending(ScanState *st, const char *id,
                              const char *base) {
    SkinEntry *e;

    st->total++;
    if (!st->out || st->written >= st->max) return;

    e = &st->out[st->written++];
    SDL_memset(e, 0, sizeof(*e));
    SDL_strlcpy(e->id, id, sizeof(e->id));
    SDL_strlcpy(e->displayName, base, sizeof(e->displayName));
    e->kind = SKIN_KIND_WORKSHOP;
    e->pending = true;
}

/* Every subdirectory and every .wsf / .zip in dir is a skin candidate. The
 * id drops an archive's extension, so foo.wsf and a foo/ directory collide
 * and the location walked first keeps the id. */
static void scanDir(ScanState *st, const char *dir, SkinKind kind,
                    const char *prefix) {
    char **list;
    int count = 0;
    int i;

    list = listDirectory(dir, &count);
    if (!list) return;

    for (i = 0; i < count; i++) {
        char full[SKIN_PATH_MAX];
        char base[SKIN_NAME_MAX];
        char id[SKIN_ID_MAX];
        SDL_PathInfo info;
        const char *name = list[i];

        if (!name || name[0] == '\0' || name[0] == '.') continue;
        SDL_snprintf(full, sizeof(full), "%s/%s", dir, name);
        if (!SDL_GetPathInfo(full, &info)) continue;

        if (info.type == SDL_PATHTYPE_DIRECTORY) {
            SDL_strlcpy(base, name, sizeof(base));
        } else if (info.type == SDL_PATHTYPE_FILE && hasArchiveExt(name)) {
            SDL_strlcpy(base, name, sizeof(base));
            stripArchiveExt(base);
        } else {
            continue;
        }

        SDL_snprintf(id, sizeof(id), "%s%s", prefix, base);
        if (scanSeen(st, id)) continue;
        scanRecord(st, id, base, full, kind);
    }

    freeDirectoryList(list, count);
}

static int scanLocations(SkinEntry *out, int max, int *written) {
    ScanState st;
    char dir[SKIN_PATH_MAX];
    char *prefPath;
    const char *basePath;
    int total;

    SDL_memset(&st, 0, sizeof(st));
    st.out = out;
    st.max = max;

    /* 1. <prefpath>/skins — user-installed, and the one location writable on
     *    every platform. */
    prefPath = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefPath) {
        SDL_snprintf(dir, sizeof(dir), "%sskins", prefPath);
        scanDir(&st, dir, SKIN_KIND_USER, "user:");
        SDL_free(prefPath);
    }

    /* 2. Steam Workshop install folders — SKIN_KIND_WORKSHOP, ids of the form
     *    "workshop:<publishedfileid>". An item the user is subscribed to but
     *    Steam has not finished delivering has no folder yet, so it is listed
     *    as pending rather than left out of the picker entirely. */
    {
        int wsCount = steam_workshop_subscribed_count();
        int i;

        for (i = 0; i < wsCount; i++) {
            uint64_t wid = 0;
            bool installed;
            char folder[SKIN_PATH_MAX];
            char base[SKIN_NAME_MAX];
            char id[SKIN_ID_MAX];

            installed = steam_workshop_item(i, &wid, folder, sizeof(folder));
            if (wid == 0) continue;

            SDL_snprintf(base, sizeof(base), "%llu", (unsigned long long)wid);
            SDL_snprintf(id, sizeof(id), "workshop:%s", base);
            if (scanSeen(&st, id)) continue;

            if (installed) {
                scanRecord(&st, id, base, folder, SKIN_KIND_WORKSHOP);
            } else {
                scanRecordPending(&st, id, base);
                /* Nudge Steam to fetch the files. Only on the pass that
                 * writes rows: skinScanCount() walks these same locations
                 * with out == NULL purely to size the caller's buffer, and
                 * must not do anything observable. */
                if (st.out != NULL) steam_workshop_request_download(wid);
            }
        }
    }

    basePath = SDL_GetBasePath();
    if (basePath) {
        /* 3. <base>/data/skins — skins shipped with the game. */
        SDL_snprintf(dir, sizeof(dir), "%sdata/skins", basePath);
        scanDir(&st, dir, SKIN_KIND_BUILTIN, "builtin:");

        /* 4. <base>/skins — where 1.x users put their .wsf files. */
        SDL_snprintf(dir, sizeof(dir), "%sskins", basePath);
        scanDir(&st, dir, SKIN_KIND_USER, "user:");
    }

    total = st.total;
    if (written) *written = st.written;
    SDL_free(st.seen);
    return total;
}

int skinScanCount(void) {
    return scanLocations(NULL, 0, NULL);
}

int skinScan(SkinEntry *out, int max) {
    int written = 0;
    if (!out || max <= 0) return 0;
    scanLocations(out, max, &written);
    return written;
}

/* ------------------------------------------------------------------ */
/* Active skin                                                         */
/* ------------------------------------------------------------------ */

bool skinSetActive(const char *id) {
    int total;

    /* The choice is recorded whether or not the assets load, so a skin that
     * is not on disk right now — a Workshop item still downloading, a drive
     * not mounted — stays the saved preference and comes back when the files
     * do.  A caller retrying its last request may pass s_requestedId itself,
     * so only copy when it is not already the destination. */
    if (!id || id[0] == '\0' || SDL_strcasecmp(id, "default") == 0) {
        s_requestedId[0] = '\0';
    } else if (id != s_requestedId) {
        SDL_strlcpy(s_requestedId, id, sizeof(s_requestedId));
    }

    if (s_activeSource) {
        skinSourceClose(s_activeSource);
        s_activeSource = NULL;
    }
    s_activeId[0] = '\0';

    if (!id || id[0] == '\0' || SDL_strcasecmp(id, "default") == 0) {
        return true;
    }

    total = skinScanCount();
    if (total > 0) {
        SkinEntry *entries = (SkinEntry *)SDL_calloc((size_t)total,
                                                     sizeof(SkinEntry));
        if (entries) {
            int n = skinScan(entries, total);
            int i;
            for (i = 0; i < n; i++) {
                if (SDL_strcmp(entries[i].id, id) != 0) continue;
                s_activeSource = skinSourceOpen(entries[i].path);
                if (s_activeSource) {
                    SDL_strlcpy(s_activeId, id, sizeof(s_activeId));
                }
                break;
            }
            SDL_free(entries);
        }
    }

    if (!s_activeSource) {
        /* An id from another machine, a Workshop item still downloading, or
         * one whose files are offline: the built-in assets stand in and the
         * caller carries on, but the id stays the player's choice. */
        WB_LOG_DEBUG(WB_LOG_CAT_GUI,
                     "[Skin] '%s' did not resolve; using the built-in assets, "
                     "keeping it as the choice",
                     id);
        return false;
    }
    return true;
}

const char *skinGetActive(void) {
    return s_activeId;
}

const char *skinGetRequested(void) {
    return s_requestedId;
}

SkinSource *skinGetActiveSource(void) {
    return s_activeSource;
}
