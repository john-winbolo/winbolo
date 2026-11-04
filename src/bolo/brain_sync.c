/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Brain sync
 *Filename:      brain_sync.c
 *Purpose:
 *  Brain manifests for the Bot Host: listing a brain's
 *  files, the manifest wire form, the path checks, and the
 *  Bot Host's per-version cache. See brain_sync.h.
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <zlib.h>

#include "brain_sync.h"
#include "brain_list_internal.h"   /* brainListScan, BRAIN_LIST_PATH_LEN */

/* ── Hash and checks ───────────────────────────────────────────────────── */

uint64_t brainSyncHash(uint64_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t hashU32(uint64_t h, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)v;
    return brainSyncHash(h, b, 4);
}

static uint64_t hashU64(uint64_t h, uint64_t v) {
    h = hashU32(h, (uint32_t)(v >> 32));
    return hashU32(h, (uint32_t)v);
}

/* True when part (len bytes, no '/') is a Windows device name, with or
 * without an extension: CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$,
 * CONOUT$. */
static bool partIsDeviceName(const char *part, size_t len) {
    static const char *const names[] = {
        "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"
    };
    char base[16];
    size_t n = 0, i;
    while (n < len && part[n] != '.') n++;
    if (n == 0 || n >= sizeof(base)) return false;
    for (i = 0; i < n; i++) {
        char c = part[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        base[i] = c;
    }
    base[n] = '\0';
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(base, names[i]) == 0) return true;
    }
    if (n == 4 && (strncmp(base, "COM", 3) == 0 || strncmp(base, "LPT", 3) == 0) &&
        base[3] >= '0' && base[3] <= '9') {
        return true;
    }
    return false;
}

static bool partSafe(const char *part, size_t len) {
    size_t i;
    if (len == 0) return false;
    if (part[0] == '.') return false;          /* ".", "..", hidden names */
    if (part[len - 1] == '.' || part[len - 1] == ' ') return false;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)part[i];
        if (c < 0x20 || c > 0x7e) return false;
        if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || c == '/') {
            return false;
        }
    }
    return !partIsDeviceName(part, len);
}

bool brainSyncPathSafe(const char *rel) {
    size_t len, start = 0, i;
    if (rel == NULL) return false;
    len = strlen(rel);
    if (len == 0 || len >= BRAIN_SYNC_PATH_MAX) return false;
    if (rel[0] == '/') return false;
    for (i = 0; i <= len; i++) {
        if (i == len || rel[i] == '/') {
            if (!partSafe(rel + start, i - start)) return false;
            start = i + 1;
        }
    }
    return true;
}

bool brainSyncNameSafe(const char *name) {
    size_t len;
    if (name == NULL) return false;
    len = strlen(name);
    if (len == 0 || len >= BRAIN_LIST_NAME_LEN) return false;
    return partSafe(name, len);
}

void brainSyncVersionHex(uint64_t version, char out[17]) {
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 15; i >= 0; i--) {
        out[i] = digits[version & 0xf];
        version >>= 4;
    }
    out[16] = '\0';
}

/* ── Versions ──────────────────────────────────────────────────────────── */

static uint64_t brainVersionOf(const BrainSyncBrain *b) {
    uint64_t h = BRAIN_SYNC_HASH_SEED;
    int i;
    for (i = 0; i < b->fileCount; i++) {
        const BrainSyncFile *f = &b->files[i];
        h = brainSyncHash(h, f->path, strlen(f->path) + 1);
        h = hashU32(h, f->size);
        h = hashU64(h, f->hash);
    }
    return h;
}

void brainSyncComputeVersions(BrainSyncManifest *m) {
    uint64_t h = BRAIN_SYNC_HASH_SEED;
    uint8_t flags = m->debugMode ? 1 : 0;
    int b;
    h = brainSyncHash(h, &flags, 1);
    for (b = 0; b < m->brainCount; b++) {
        BrainSyncBrain *br = &m->brains[b];
        int i;
        br->version = brainVersionOf(br);
        br->totalBytes = 0;
        for (i = 0; i < br->fileCount; i++) br->totalBytes += br->files[i].size;
        h = brainSyncHash(h, br->name, strlen(br->name) + 1);
        h = hashU64(h, br->version);
    }
    m->version = h;
}

void brainSyncFree(BrainSyncManifest *m) {
    int b;
    if (m == NULL) return;
    for (b = 0; b < BRAIN_SYNC_MAX_BRAINS; b++) {
        free(m->brains[b].files);
    }
    memset(m, 0, sizeof(*m));
}

const BrainSyncBrain *brainSyncFindBrain(const BrainSyncManifest *m,
                                         const char *name) {
    int b;
    if (m == NULL || name == NULL) return NULL;
    for (b = 0; b < m->brainCount; b++) {
        if (strcmp(m->brains[b].name, name) == 0) return &m->brains[b];
    }
    return NULL;
}

/* ── Listing a brain's files ───────────────────────────────────────────── */

typedef struct {
    char names[256][BRAIN_SYNC_PATH_MAX];
    int  count;
    bool overflow;
} DirNames;

static SDL_EnumerationResult SDLCALL collectName(void *userdata,
                                                 const char *dirname,
                                                 const char *fname) {
    DirNames *dn = (DirNames *)userdata;
    (void)dirname;
    if (dn->count >= (int)(sizeof(dn->names) / sizeof(dn->names[0]))) {
        dn->overflow = true;
        return SDL_ENUM_SUCCESS;
    }
    SDL_strlcpy(dn->names[dn->count], fname, sizeof(dn->names[0]));
    dn->count++;
    return SDL_ENUM_CONTINUE;
}

typedef struct {
    BrainSyncBrain       *b;
    const BrainSyncBrain *prev;
    int                   cap;
    uint64_t              total;
    bool                  failed;
} Collect;

static const BrainSyncFile *prevFile(const BrainSyncBrain *prev,
                                     const char *rel) {
    int i;
    if (prev == NULL) return NULL;
    for (i = 0; i < prev->fileCount; i++) {
        if (strcmp(prev->files[i].path, rel) == 0) return &prev->files[i];
    }
    return NULL;
}

static void addFile(Collect *c, const char *full, const char *rel,
                    const SDL_PathInfo *info) {
    BrainSyncBrain *b = c->b;
    BrainSyncFile *f;
    const BrainSyncFile *old;

    if (b->fileCount >= BRAIN_SYNC_MAX_FILES ||
        info->size > BRAIN_SYNC_FILE_MAX_BYTES) {
        fprintf(stderr, "[brainsync] brain \"%s\": too many files or \"%s\" "
                        "is too large\n", b->name, rel);
        c->failed = true;
        return;
    }
    c->total += info->size;
    if (c->total > BRAIN_SYNC_TOTAL_MAX_BYTES) {
        fprintf(stderr, "[brainsync] brain \"%s\" is too large\n", b->name);
        c->failed = true;
        return;
    }
    if (b->fileCount >= c->cap) {
        int ncap = c->cap ? c->cap * 2 : 64;
        BrainSyncFile *nf;
        if (ncap > BRAIN_SYNC_MAX_FILES) ncap = BRAIN_SYNC_MAX_FILES;
        nf = (BrainSyncFile *)realloc(b->files, sizeof(*nf) * (size_t)ncap);
        if (nf == NULL) { c->failed = true; return; }
        b->files = nf;
        c->cap = ncap;
    }
    f = &b->files[b->fileCount];
    memset(f, 0, sizeof(*f));
    SDL_strlcpy(f->path, rel, sizeof(f->path));
    f->size = (uint32_t)info->size;
    f->mtime = (int64_t)info->modify_time;
    old = prevFile(c->prev, rel);
    if (old != NULL && old->size == f->size && old->mtime == f->mtime &&
        f->mtime != 0) {
        f->hash = old->hash;
    } else {
        size_t sz = 0;
        void *data = SDL_LoadFile(full, &sz);
        if (data == NULL && f->size != 0) {
            fprintf(stderr, "[brainsync] cannot read \"%s\"\n", full);
            c->failed = true;
            return;
        }
        f->size = (uint32_t)sz;
        f->hash = brainSyncHash(BRAIN_SYNC_HASH_SEED, data, sz);
        SDL_free(data);
    }
    b->fileCount++;
}

/* Walk dir/relDir. recurse: go into sub-folders. skipOptLua: leave out
 * .lua files whose name opt/ holds too (the top level of an opt brain). */
static void walk(Collect *c, const char *dir, const char *relDir,
                 bool recurse, bool skipOptLua, int depth) {
    DirNames *dn;
    char full[1024];
    int i;

    if (c->failed || depth > 8) return;
    dn = (DirNames *)calloc(1, sizeof(*dn));
    if (dn == NULL) { c->failed = true; return; }
    if (relDir[0] != '\0') {
        SDL_snprintf(full, sizeof(full), "%s/%s", dir, relDir);
    } else {
        SDL_strlcpy(full, dir, sizeof(full));
    }
    if (!SDL_EnumerateDirectory(full, collectName, dn)) {
        free(dn);
        c->failed = true;
        return;
    }
    if (dn->overflow) {
        fprintf(stderr, "[brainsync] folder \"%s\" has too many entries\n",
                full);
        free(dn);
        c->failed = true;
        return;
    }
    for (i = 0; i < dn->count && !c->failed; i++) {
        const char *name = dn->names[i];
        char rel[BRAIN_SYNC_PATH_MAX];
        char path[1024];
        SDL_PathInfo info;
        size_t nl = strlen(name);

        if (name[0] == '.') continue;
        if (relDir[0] != '\0') {
            if ((size_t)SDL_snprintf(rel, sizeof(rel), "%s/%s", relDir, name)
                >= sizeof(rel)) {
                fprintf(stderr, "[brainsync] path too long under \"%s\"\n", full);
                continue;
            }
        } else {
            SDL_strlcpy(rel, name, sizeof(rel));
        }
        SDL_snprintf(path, sizeof(path), "%s/%s", dir, rel);
        if (!SDL_GetPathInfo(path, &info)) continue;
        if (!brainSyncPathSafe(rel)) {
            fprintf(stderr, "[brainsync] brain \"%s\": \"%s\" left out (its "
                            "name is not one a Bot Host can take)\n",
                    c->b->name, rel);
            continue;
        }
        if (info.type == SDL_PATHTYPE_DIRECTORY) {
            if (recurse) walk(c, dir, rel, true, false, depth + 1);
            continue;
        }
        if (info.type != SDL_PATHTYPE_FILE) continue;
        if (skipOptLua && nl > 4 && SDL_strcasecmp(name + nl - 4, ".lua") == 0) {
            char optPath[1024];
            SDL_PathInfo oi;
            SDL_snprintf(optPath, sizeof(optPath), "%s/opt/%s", dir, name);
            if (SDL_GetPathInfo(optPath, &oi) && oi.type == SDL_PATHTYPE_FILE) {
                continue;
            }
        }
        addFile(c, path, rel, &info);
    }
    free(dn);
}

static int fileCmp(const void *a, const void *b) {
    return strcmp(((const BrainSyncFile *)a)->path,
                  ((const BrainSyncFile *)b)->path);
}

bool brainSyncCollectBrain(BrainSyncBrain *b, const char *name,
                           const char *dir, bool debugMode,
                           const BrainSyncBrain *prev) {
    Collect c;
    char optInit[1024];
    SDL_PathInfo info;
    bool useOpt = false;

    memset(b, 0, sizeof(*b));
    if (!brainSyncNameSafe(name)) return false;
    SDL_strlcpy(b->name, name, sizeof(b->name));
    SDL_strlcpy(b->dir, dir, sizeof(b->dir));
    memset(&c, 0, sizeof(c));
    c.b = b;
    c.prev = prev;

    if (!debugMode) {
        SDL_snprintf(optInit, sizeof(optInit), "%s/opt/init.lua", dir);
        useOpt = SDL_GetPathInfo(optInit, &info) &&
                 info.type == SDL_PATHTYPE_FILE;
    }
    if (useOpt) {
        walk(&c, dir, "opt", true, false, 1);
        walk(&c, dir, "", false, true, 0);
    } else {
        walk(&c, dir, "", true, false, 0);
    }
    if (c.failed || b->fileCount == 0) {
        free(b->files);
        memset(b, 0, sizeof(*b));
        return false;
    }
    qsort(b->files, (size_t)b->fileCount, sizeof(b->files[0]), fileCmp);
    b->version = brainVersionOf(b);
    b->totalBytes = (uint32_t)c.total;
    return true;
}

void brainSyncBuildManifest(BrainSyncManifest *out, bool debugMode,
                            const BrainSyncManifest *prev) {
    BrainList list;
    char (*paths)[BRAIN_LIST_PATH_LEN];
    int i;

    memset(out, 0, sizeof(*out));
    out->debugMode = debugMode;
    paths = (char (*)[BRAIN_LIST_PATH_LEN])calloc(BRAIN_LIST_MAX,
                                                  BRAIN_LIST_PATH_LEN);
    if (paths == NULL) return;
    brainListScan(&list, paths);
    for (i = 0; i < list.count && out->brainCount < BRAIN_SYNC_MAX_BRAINS; i++) {
        char dir[BRAIN_LIST_PATH_LEN];
        size_t n;
        SDL_strlcpy(dir, paths[i], sizeof(dir));
        n = strlen(dir);
        while (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\') n--;
        if (n == 0) continue;
        dir[n - 1] = '\0';   /* drop the separator before init.lua */
        if (brainSyncCollectBrain(&out->brains[out->brainCount],
                                  list.entries[i].name, dir, debugMode,
                                  brainSyncFindBrain(prev,
                                                     list.entries[i].name))) {
            out->brainCount++;
        }
    }
    free(paths);
    brainSyncComputeVersions(out);
}

/* ── Wire form ─────────────────────────────────────────────────────────── */

static void putU16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void putU32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void putU64(uint8_t *p, uint64_t v) {
    putU32(p, (uint32_t)(v >> 32));
    putU32(p + 4, (uint32_t)v);
}
static uint16_t getU16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t getU32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t getU64(const uint8_t *p) {
    return ((uint64_t)getU32(p) << 32) | getU32(p + 4);
}


uint8_t *brainSyncManifestEncode(const BrainSyncManifest *m, size_t *outLen) {
    size_t len = 3 + 1 + 1 + 8, pos = 0;
    uint8_t *buf;
    int b, i;

    if (m == NULL || outLen == NULL) return NULL;
    for (b = 0; b < m->brainCount; b++) {
        const BrainSyncBrain *br = &m->brains[b];
        len += 1 + strlen(br->name) + 8 + 2;
        for (i = 0; i < br->fileCount; i++) {
            len += 1 + strlen(br->files[i].path) + 4 + 8;
        }
    }
    if (len > BRAIN_SYNC_MANIFEST_MAX) return NULL;
    buf = (uint8_t *)malloc(len);
    if (buf == NULL) return NULL;
    buf[pos++] = 'B';
    buf[pos++] = 'S';
    buf[pos++] = 1;
    buf[pos++] = m->debugMode ? 1 : 0;
    buf[pos++] = (uint8_t)m->brainCount;
    putU64(buf + pos, m->version);
    pos += 8;
    for (b = 0; b < m->brainCount; b++) {
        const BrainSyncBrain *br = &m->brains[b];
        size_t nl = strlen(br->name);
        buf[pos++] = (uint8_t)nl;
        memcpy(buf + pos, br->name, nl);
        pos += nl;
        putU64(buf + pos, br->version);
        pos += 8;
        putU16(buf + pos, (uint16_t)br->fileCount);
        pos += 2;
        for (i = 0; i < br->fileCount; i++) {
            const BrainSyncFile *f = &br->files[i];
            size_t pl = strlen(f->path);
            buf[pos++] = (uint8_t)pl;
            memcpy(buf + pos, f->path, pl);
            pos += pl;
            putU32(buf + pos, f->size);
            pos += 4;
            putU64(buf + pos, f->hash);
            pos += 8;
        }
    }
    *outLen = pos;
    return buf;
}

bool brainSyncManifestDecode(const uint8_t *buf, size_t len,
                             BrainSyncManifest *out) {
    size_t pos = 0;
    uint64_t wantVersion, total = 0;
    int b, i, count;

    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (buf == NULL || len < 13 || len > BRAIN_SYNC_MANIFEST_MAX) return false;
    if (buf[0] != 'B' || buf[1] != 'S' || buf[2] != 1) return false;
    if (buf[3] & ~1u) return false;
    out->debugMode = (buf[3] & 1) != 0;
    count = buf[4];
    if (count > BRAIN_SYNC_MAX_BRAINS) return false;
    wantVersion = getU64(buf + 5);
    pos = 13;
    for (b = 0; b < count; b++) {
        BrainSyncBrain *br = &out->brains[b];
        uint64_t brainVersion;
        size_t nl;
        int files;

        out->brainCount = b + 1;
        if (pos + 1 > len) goto bad;
        nl = buf[pos++];
        if (nl == 0 || nl >= sizeof(br->name) || pos + nl + 10 > len) goto bad;
        memcpy(br->name, buf + pos, nl);
        br->name[nl] = '\0';
        pos += nl;
        if (strlen(br->name) != nl || !brainSyncNameSafe(br->name)) goto bad;
        for (i = 0; i < b; i++) {
            if (strcmp(out->brains[i].name, br->name) == 0) goto bad;
        }
        brainVersion = getU64(buf + pos);
        pos += 8;
        files = getU16(buf + pos);
        pos += 2;
        if (files == 0 || files > BRAIN_SYNC_MAX_FILES) goto bad;
        br->files = (BrainSyncFile *)calloc((size_t)files, sizeof(BrainSyncFile));
        if (br->files == NULL) goto bad;
        for (i = 0; i < files; i++) {
            BrainSyncFile *f = &br->files[i];
            size_t pl;
            if (pos + 1 > len) goto bad;
            pl = buf[pos++];
            if (pl == 0 || pl >= sizeof(f->path) || pos + pl + 12 > len) goto bad;
            memcpy(f->path, buf + pos, pl);
            f->path[pl] = '\0';
            pos += pl;
            if (strlen(f->path) != pl || !brainSyncPathSafe(f->path)) goto bad;
            if (i > 0 && strcmp(br->files[i - 1].path, f->path) >= 0) goto bad;
            f->size = getU32(buf + pos);
            pos += 4;
            f->hash = getU64(buf + pos);
            pos += 8;
            if (f->size > BRAIN_SYNC_FILE_MAX_BYTES) goto bad;
            total += f->size;
            if (total > BRAIN_SYNC_TOTAL_MAX_BYTES) goto bad;
            br->fileCount = i + 1;
        }
        if (brainVersionOf(br) != brainVersion) goto bad;
    }
    if (pos != len) goto bad;
    brainSyncComputeVersions(out);
    if (out->version != wantVersion) goto bad;
    return true;

bad:
    brainSyncFree(out);
    return false;
}

/* ── Cache ─────────────────────────────────────────────────────────────── */

bool brainSyncCacheDir(char *out, size_t outSz, const char *root,
                       const char *brain, uint64_t version) {
    char hex[17];
    if (out == NULL || root == NULL || !brainSyncNameSafe(brain)) return false;
    brainSyncVersionHex(version, hex);
    return (size_t)SDL_snprintf(out, outSz, "%s/%s/%s", root, brain, hex) < outSz;
}

bool brainSyncCacheFile(char *out, size_t outSz, const char *root,
                        const char *brain, uint64_t version, const char *rel) {
    char dir[1024];
    if (!brainSyncPathSafe(rel)) return false;
    if (!brainSyncCacheDir(dir, sizeof(dir), root, brain, version)) return false;
    return (size_t)SDL_snprintf(out, outSz, "%s/%s", dir, rel) < outSz;
}

bool brainSyncFileMatches(const char *path, uint32_t size, uint64_t hash) {
    SDL_PathInfo info;
    size_t sz = 0;
    void *data;
    bool ok;
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE ||
        info.size != size) {
        return false;
    }
    data = SDL_LoadFile(path, &sz);
    if (data == NULL) return size == 0 && sz == 0 && info.size == 0;
    ok = (sz == size) && brainSyncHash(BRAIN_SYNC_HASH_SEED, data, sz) == hash;
    SDL_free(data);
    return ok;
}

bool brainSyncWriteFile(const char *path, const void *data, size_t len) {
    char dir[1024];
    char part[1100];
    size_t n;
    SDL_IOStream *io;
    bool ok;

    SDL_strlcpy(dir, path, sizeof(dir));
    n = strlen(dir);
    while (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\') n--;
    if (n > 0) {
        dir[n - 1] = '\0';
        if (!SDL_CreateDirectory(dir)) return false;
    }
    SDL_snprintf(part, sizeof(part), "%s.part", path);
    io = SDL_IOFromFile(part, "wb");
    if (io == NULL) return false;
    ok = (len == 0) || SDL_WriteIO(io, data, len) == len;
    if (!SDL_CloseIO(io)) ok = false;
    if (ok) {
        (void)SDL_RemovePath(path);
        ok = SDL_RenamePath(part, path);
    }
    if (!ok) (void)SDL_RemovePath(part);
    return ok;
}

/* Look through the other version folders of a brain for a copy of rel with
 * this size and hash, and copy it in to target. */
static bool copyFromSibling(const char *root, const BrainSyncBrain *br,
                            const BrainSyncFile *f, const char *target) {
    DirNames *dn;
    char brainDir[1024], hex[17];
    bool done = false;
    int i;

    if ((size_t)SDL_snprintf(brainDir, sizeof(brainDir), "%s/%s", root,
                             br->name) >= sizeof(brainDir)) {
        return false;
    }
    dn = (DirNames *)calloc(1, sizeof(*dn));
    if (dn == NULL) return false;
    if (!SDL_EnumerateDirectory(brainDir, collectName, dn)) {
        free(dn);
        return false;
    }
    brainSyncVersionHex(br->version, hex);
    for (i = 0; i < dn->count && !done; i++) {
        char cand[1400];
        size_t sz = 0;
        void *data;
        if (strcmp(dn->names[i], hex) == 0) continue;
        if (!brainSyncNameSafe(dn->names[i])) continue;
        if ((size_t)SDL_snprintf(cand, sizeof(cand), "%s/%s/%s", brainDir,
                                 dn->names[i], f->path) >= sizeof(cand)) {
            continue;
        }
        if (!brainSyncFileMatches(cand, f->size, f->hash)) continue;
        data = SDL_LoadFile(cand, &sz);
        if (data == NULL && f->size != 0) continue;
        done = brainSyncWriteFile(target, data, sz);
        SDL_free(data);
    }
    free(dn);
    return done;
}

bool brainSyncPlanCache(const BrainSyncManifest *m, const char *root,
                        BrainSyncPlan *plan) {
    int b, i, total = 0;

    memset(plan, 0, sizeof(*plan));
    for (b = 0; b < m->brainCount; b++) total += m->brains[b].fileCount;
    plan->need = (uint32_t *)calloc((size_t)(total > 0 ? total : 1),
                                    sizeof(uint32_t));
    if (plan->need == NULL) return false;
    for (b = 0; b < m->brainCount; b++) {
        const BrainSyncBrain *br = &m->brains[b];
        for (i = 0; i < br->fileCount; i++) {
            const BrainSyncFile *f = &br->files[i];
            char target[1400];
            if (!brainSyncCacheFile(target, sizeof(target), root, br->name,
                                    br->version, f->path)) {
                free(plan->need);
                memset(plan, 0, sizeof(*plan));
                return false;
            }
            if (brainSyncFileMatches(target, f->size, f->hash)) {
                plan->haveCount++;
            } else if (copyFromSibling(root, br, f, target)) {
                plan->copyCount++;
            } else {
                plan->need[plan->needCount++] = ((uint32_t)b << 16) | (uint32_t)i;
                plan->needBytes += f->size;
            }
        }
    }
    return true;
}

/* ── Chunks ────────────────────────────────────────────────────────────── */

uint8_t *brainSyncChunkPack(const uint8_t *raw, uint32_t rawLen,
                            uint32_t *outLen) {
    uLongf zLen = compressBound(rawLen);
    uint8_t *buf = (uint8_t *)malloc(4 + (size_t)zLen);
    if (buf == NULL) return NULL;
    putU32(buf, rawLen);
    if (compress2(buf + 4, &zLen, raw, rawLen, Z_DEFAULT_COMPRESSION) != Z_OK) {
        free(buf);
        return NULL;
    }
    *outLen = (uint32_t)(4 + zLen);
    return buf;
}

int64_t brainSyncChunkUnpack(const uint8_t *buf, uint32_t len,
                             uint8_t *dst, uint32_t dstCap) {
    uint32_t rawLen;
    uLongf outLen;
    if (buf == NULL || len < 4) return -1;
    rawLen = getU32(buf);
    if (rawLen > dstCap || rawLen > BRAIN_SYNC_CHUNK_BYTES) return -1;
    outLen = rawLen;
    if (rawLen == 0) return 0;
    if (uncompress(dst, &outLen, buf + 4, len - 4) != Z_OK) return -1;
    if (outLen != rawLen) return -1;
    return (int64_t)rawLen;
}
