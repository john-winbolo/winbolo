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
#include "machine_id.h"            /* machineSha256 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

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

bool brainSyncIsTextPath(const char *path) {
    static const char *const exts[] = { ".lua", ".txt", ".json" };
    const char *dot, *slash, *back;
    size_t i;
    if (path == NULL) return false;
    dot = strrchr(path, '.');
    slash = strrchr(path, '/');
    back = strrchr(path, '\\');
    if (back != NULL && (slash == NULL || back > slash)) slash = back;
    if (dot == NULL || (slash != NULL && dot < slash)) return false;
    for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (SDL_strcasecmp(dot, exts[i]) == 0) return true;
    }
    return false;
}

size_t brainSyncNormalizeText(void *data, size_t len) {
    uint8_t *p = (uint8_t *)data;
    size_t in = 0, out = 0;
    if (p == NULL) return 0;
    while (in < len) {
        if (p[in] == '\r') {
            size_t run = in;
            while (run < len && p[run] == '\r') run++;
            if (run < len && p[run] == '\n') {
                in = run;                          /* CRs before a LF go */
            } else {
                while (in < run) p[out++] = p[in++];   /* lone CRs stay */
            }
            continue;
        }
        p[out++] = p[in++];
    }
    return out;
}

void *brainSyncLoadFile(const char *path, size_t *len) {
    size_t sz = 0;
    void *data = SDL_LoadFile(path, &sz);
    if (data != NULL && brainSyncIsTextPath(path)) {
        sz = brainSyncNormalizeText(data, sz);
        ((uint8_t *)data)[sz] = 0;   /* SDL_LoadFile leaves room for a NUL */
    }
    if (len != NULL) *len = data != NULL ? sz : 0;
    return data;
}

/* True when a file of onDisk bytes at path may read as size bytes: the same
 * size, or no smaller for a text file, whose CRs before a LF are taken out. */
static bool diskSizeFits(const char *path, uint64_t onDisk, uint32_t size) {
    if (brainSyncIsTextPath(path)) return onDisk >= size;
    return onDisk == size;
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
    f->rawSize = info->size;
    f->mtime = (int64_t)info->modify_time;
    old = prevFile(c->prev, rel);
    if (old != NULL && old->rawSize == f->rawSize && old->mtime == f->mtime &&
        f->mtime != 0) {
        f->size = old->size;
        f->hash = old->hash;
        memcpy(f->sha, old->sha, sizeof(f->sha));
    } else {
        size_t sz = 0;
        void *data = brainSyncLoadFile(full, &sz);
        if (data == NULL && f->size != 0) {
            fprintf(stderr, "[brainsync] cannot read \"%s\"\n", full);
            c->failed = true;
            return;
        }
        f->size = (uint32_t)sz;
        f->hash = brainSyncHash(BRAIN_SYNC_HASH_SEED, data, sz);
        machineSha256(data != NULL ? data : (const void *)"", sz, f->sha);
        SDL_free(data);
    }
    /* The brain's bytes: a text file counts with its CRs taken out. */
    c->total += f->size;
    if (c->total > BRAIN_SYNC_TOTAL_MAX_BYTES) {
        fprintf(stderr, "[brainsync] brain \"%s\" is too large\n", b->name);
        c->failed = true;
        return;
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
        } else if (SDL_strlcpy(rel, name, sizeof(rel)) >= sizeof(rel)) {
            fprintf(stderr, "[brainsync] path too long under \"%s\"\n", full);
            continue;
        }
        /* A cut path names some other file: leave it out. */
        if ((size_t)SDL_snprintf(path, sizeof(path), "%s/%s", dir, rel)
            >= sizeof(path)) {
            fprintf(stderr, "[brainsync] path too long under \"%s\"\n", full);
            continue;
        }
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
            if ((size_t)SDL_snprintf(optPath, sizeof(optPath), "%s/opt/%s",
                                     dir, name) < sizeof(optPath) &&
                SDL_GetPathInfo(optPath, &oi) && oi.type == SDL_PATHTYPE_FILE) {
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
    out->haveSha = true;
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


/* The FNV-1a hash of every file's sha, the v2 trailer. */
static uint64_t shaCheckOf(const BrainSyncManifest *m) {
    uint64_t h = BRAIN_SYNC_HASH_SEED;
    int b, i;
    for (b = 0; b < m->brainCount; b++) {
        for (i = 0; i < m->brains[b].fileCount; i++) {
            h = brainSyncHash(h, m->brains[b].files[i].sha, BRAIN_SYNC_SHA_LEN);
        }
    }
    return h;
}

static uint8_t *manifestEncodeAs(const BrainSyncManifest *m, size_t *outLen,
                                 int form) {
    size_t len = 3 + 1 + 1 + 8, pos = 0;
    size_t shaLen = form == 2 ? BRAIN_SYNC_SHA_LEN : 0;
    uint8_t *buf;
    int b, i;

    if (m == NULL || outLen == NULL) return NULL;
    for (b = 0; b < m->brainCount; b++) {
        const BrainSyncBrain *br = &m->brains[b];
        len += 1 + strlen(br->name) + 8 + 2;
        for (i = 0; i < br->fileCount; i++) {
            len += 1 + strlen(br->files[i].path) + 4 + 8 + shaLen;
        }
    }
    if (form == 2) len += 8;
    if (len > BRAIN_SYNC_MANIFEST_MAX) return NULL;
    buf = (uint8_t *)malloc(len);
    if (buf == NULL) return NULL;
    buf[pos++] = 'B';
    buf[pos++] = 'S';
    buf[pos++] = (uint8_t)form;
    buf[pos++] = (uint8_t)((m->debugMode ? 1 : 0) | (m->noCode ? 2 : 0));
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
            if (shaLen > 0) {
                memcpy(buf + pos, f->sha, shaLen);
                pos += shaLen;
            }
        }
    }
    if (form == 2) {
        putU64(buf + pos, shaCheckOf(m));
        pos += 8;
    }
    *outLen = pos;
    return buf;
}

uint8_t *brainSyncManifestEncode(const BrainSyncManifest *m, size_t *outLen) {
    return manifestEncodeAs(m, outLen, 1);
}

uint8_t *brainSyncManifestEncodeV2(const BrainSyncManifest *m, size_t *outLen) {
    return manifestEncodeAs(m, outLen, 2);
}

bool brainSyncManifestDecode(const uint8_t *buf, size_t len,
                             BrainSyncManifest *out) {
    size_t pos = 0, shaLen, end;
    uint64_t wantVersion, total = 0;
    int b, i, count;

    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (buf == NULL || len < 13 || len > BRAIN_SYNC_MANIFEST_MAX) return false;
    if (buf[0] != 'B' || buf[1] != 'S' || (buf[2] != 1 && buf[2] != 2)) {
        return false;
    }
    if (buf[3] & ~3u) return false;
    shaLen = buf[2] == 2 ? BRAIN_SYNC_SHA_LEN : 0;
    /* The v2 trailer is not part of the brain list. */
    if (buf[2] == 2 && len < 13 + 8) return false;
    end = buf[2] == 2 ? len - 8 : len;
    len = end;
    out->debugMode = (buf[3] & 1) != 0;
    out->noCode = (buf[3] & 2) != 0;
    out->haveSha = buf[2] == 2;
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
            if (pl == 0 || pl >= sizeof(f->path) || pos + pl + 12 + shaLen > len) {
                goto bad;
            }
            memcpy(f->path, buf + pos, pl);
            f->path[pl] = '\0';
            pos += pl;
            if (strlen(f->path) != pl || !brainSyncPathSafe(f->path)) goto bad;
            if (i > 0 && strcmp(br->files[i - 1].path, f->path) >= 0) goto bad;
            f->size = getU32(buf + pos);
            pos += 4;
            f->hash = getU64(buf + pos);
            pos += 8;
            if (shaLen > 0) {
                memcpy(f->sha, buf + pos, shaLen);
                pos += shaLen;
            }
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
    if (out->haveSha && getU64(buf + end) != shaCheckOf(out)) goto bad;
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
        !diskSizeFits(path, info.size, size)) {
        return false;
    }
    data = brainSyncLoadFile(path, &sz);
    if (data == NULL) return size == 0 && sz == 0 && info.size == 0;
    ok = (sz == size) && brainSyncHash(BRAIN_SYNC_HASH_SEED, data, sz) == hash;
    SDL_free(data);
    return ok;
}

bool brainSyncDataIs(const BrainSyncFile *f, const void *data, size_t len) {
    uint8_t sha[BRAIN_SYNC_SHA_LEN];
    if (f == NULL || len != f->size || (data == NULL && len != 0)) return false;
    if (brainSyncHash(BRAIN_SYNC_HASH_SEED, data, len) != f->hash) return false;
    machineSha256(data != NULL ? data : (const void *)"", len, sha);
    return memcmp(sha, f->sha, sizeof(sha)) == 0;
}

bool brainSyncFileIs(const char *path, const BrainSyncFile *f, bool useSha) {
    SDL_PathInfo info;
    size_t sz = 0;
    void *data;
    bool ok;
    if (!useSha) return brainSyncFileMatches(path, f->size, f->hash);
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE ||
        !diskSizeFits(path, info.size, f->size)) {
        return false;
    }
    data = brainSyncLoadFile(path, &sz);
    if (data == NULL && f->size != 0) return false;
    ok = brainSyncDataIs(f, data, sz);
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

/* Copy the file at src to target when it is f. src is only read. */
static bool copyIfSame(const char *src, const BrainSyncFile *f, bool useSha,
                       const char *target) {
    size_t sz = 0;
    void *data;
    bool done;
    if (!brainSyncFileIs(src, f, useSha)) return false;
    /* The copy holds the LF bytes that the hashes are of. */
    data = brainSyncLoadFile(src, &sz);
    if (data == NULL && f->size != 0) return false;
    /* Checked again on the bytes read, in case the file changed between. */
    done = (useSha ? brainSyncDataIs(f, data, sz)
                   : (sz == f->size &&
                      brainSyncHash(BRAIN_SYNC_HASH_SEED, data, sz) == f->hash)) &&
           brainSyncWriteFile(target, data != NULL ? data : "", sz);
    SDL_free(data);
    return done;
}

/* Look through the other version folders of a brain for a copy of rel with
 * this size and hash (and sha when useSha), and copy it in to target. */
static bool copyFromSiblingAs(const char *root, const BrainSyncBrain *br,
                              const BrainSyncFile *f, const char *target,
                              bool useSha) {
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
        if (strcmp(dn->names[i], hex) == 0) continue;
        if (!brainSyncNameSafe(dn->names[i])) continue;
        /* Only version folders: not a stage folder ("<hex>.tmp"). */
        if (strlen(dn->names[i]) != 16) continue;
        if ((size_t)SDL_snprintf(cand, sizeof(cand), "%s/%s/%s", brainDir,
                                 dn->names[i], f->path) >= sizeof(cand)) {
            continue;
        }
        done = copyIfSame(cand, f, useSha, target);
    }
    free(dn);
    return done;
}

static bool copyFromSibling(const char *root, const BrainSyncBrain *br,
                            const BrainSyncFile *f, const char *target) {
    return copyFromSiblingAs(root, br, f, target, false);
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

/* ── Packed plan and the stage folder ──────────────────────────────────── */

bool brainSyncStageDir(char *out, size_t outSz, const char *root,
                       const char *brain, uint64_t version) {
    char dir[1024];
    if (!brainSyncCacheDir(dir, sizeof(dir), root, brain, version)) return false;
    return (size_t)SDL_snprintf(out, outSz, "%s.tmp", dir) < outSz;
}

/* True when a path is a link or junction, which a tree removal must not
 * go through. */
static bool pathIsLink(const char *path) {
#ifdef _WIN32
    wchar_t w[1100];
    DWORD a;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w,
                            (int)(sizeof(w) / sizeof(w[0]))) == 0) {
        return true;   /* not a path it can check: leave it */
    }
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

typedef struct {
    const char *dir;
    bool        ok;
} RemoveCtx;

static SDL_EnumerationResult SDLCALL removeEntry(void *userdata,
                                                 const char *dirname,
                                                 const char *fname) {
    RemoveCtx *r = (RemoveCtx *)userdata;
    char path[1400];
    (void)dirname;
    if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) {
        return SDL_ENUM_CONTINUE;
    }
    if ((size_t)SDL_snprintf(path, sizeof(path), "%s/%s", r->dir, fname)
        >= sizeof(path)) {
        r->ok = false;
        return SDL_ENUM_CONTINUE;
    }
    if (!brainSyncRemoveTree(path)) r->ok = false;
    return SDL_ENUM_CONTINUE;
}

bool brainSyncRemoveTree(const char *path) {
    SDL_PathInfo pi;
    RemoveCtx r;
    if (path == NULL || path[0] == '\0') return false;
    if (!SDL_GetPathInfo(path, &pi)) return true;   /* nothing there */
    if (pathIsLink(path)) return false;
    if (pi.type != SDL_PATHTYPE_DIRECTORY) return SDL_RemovePath(path);
    r.dir = path;
    r.ok = true;
    if (!SDL_EnumerateDirectory(path, removeEntry, &r)) r.ok = false;
    if (!r.ok) return false;
    return SDL_RemovePath(path);
}

/* True when the install folder's copy of br is br, file for file. */
static bool installIsSame(const char *installRoot, const BrainSyncBrain *br) {
    BrainSyncBrain local;
    char dir[1024];
    bool same;
    int i;
    if ((size_t)SDL_snprintf(dir, sizeof(dir), "%s/%s", installRoot, br->name)
        >= sizeof(dir)) {
        return false;
    }
    if (!brainSyncCollectBrain(&local, br->name, dir, false, NULL)) return false;
    same = local.version == br->version && local.fileCount == br->fileCount;
    for (i = 0; same && i < br->fileCount; i++) {
        same = memcmp(local.files[i].sha, br->files[i].sha,
                      BRAIN_SYNC_SHA_LEN) == 0;
    }
    free(local.files);
    return same;
}

bool brainSyncInstallMatches(const char *installRoot,
                             const BrainSyncBrain *br) {
    if (installRoot == NULL || br == NULL || br->fileCount <= 0) return false;
    return installIsSame(installRoot, br);
}

/* ── The files a build shipped ─────────────────────────────────────────── */

/* The entry of table for brain-relative path rel of brain name, or NULL. */
static const BrainSyncShippedFile *shippedFind(
    const BrainSyncShippedFile *table, int count, const char *name,
    size_t nameLen, const char *rel) {
    int i;
    for (i = 0; i < count; i++) {
        const char *p = table[i].path;
        if (p != NULL && strncmp(p, name, nameLen) == 0 && p[nameLen] == '/' &&
            strcmp(p + nameLen + 1, rel) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

/* True when the shipped file rel is one that brainSyncCollectBrain lists:
 * the same rule as walk(). useOpt: the table has opt/init.lua. */
static bool shippedListed(const BrainSyncShippedFile *table, int count,
                          const char *name, size_t nameLen, const char *rel,
                          bool useOpt) {
    char opt[BRAIN_SYNC_PATH_MAX + 8];
    size_t n = strlen(rel);
    if (!brainSyncPathSafe(rel)) return false;
    if (!useOpt) return true;
    if (strncmp(rel, "opt/", 4) == 0) return true;
    if (strchr(rel, '/') != NULL) return false;   /* another sub-folder */
    if (n > 4 && SDL_strcasecmp(rel + n - 4, ".lua") == 0) {
        SDL_snprintf(opt, sizeof(opt), "opt/%s", rel);
        if (shippedFind(table, count, name, nameLen, opt) != NULL) {
            return false;   /* opt/ has its own copy */
        }
    }
    return true;
}

static void shaHex(const uint8_t sha[BRAIN_SYNC_SHA_LEN], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < BRAIN_SYNC_SHA_LEN; i++) {
        out[i * 2] = digits[sha[i] >> 4];
        out[i * 2 + 1] = digits[sha[i] & 0x0f];
    }
    out[64] = '\0';
}

BrainSyncShippedResult brainSyncShippedCheck(const BrainSyncShippedFile *table,
                                             int count,
                                             const char *installRoot,
                                             const char *name,
                                             char *file, size_t fileSz) {
    BrainSyncBrain local;
    BrainSyncShippedResult r = BRAIN_SYNC_SHIPPED_OK;
    char dir[1024];
    size_t nameLen;
    bool useOpt, any = false;
    int i;

    if (file != NULL && fileSz > 0) file[0] = '\0';
    if (table == NULL || count <= 0 || name == NULL || name[0] == '\0') {
        return BRAIN_SYNC_SHIPPED_NOT_SHIPPED;
    }
    nameLen = strlen(name);
    for (i = 0; i < count && !any; i++) {
        any = table[i].path != NULL &&
              strncmp(table[i].path, name, nameLen) == 0 &&
              table[i].path[nameLen] == '/';
    }
    if (!any) return BRAIN_SYNC_SHIPPED_NOT_SHIPPED;
    useOpt = shippedFind(table, count, name, nameLen, "opt/init.lua") != NULL;

    if (installRoot == NULL ||
        (size_t)SDL_snprintf(dir, sizeof(dir), "%s/%s", installRoot, name)
        >= sizeof(dir) ||
        !brainSyncCollectBrain(&local, name, dir, false, NULL)) {
        return BRAIN_SYNC_SHIPPED_UNREADABLE;
    }

    /* Every file on disk was shipped, with the same bytes. */
    for (i = 0; i < local.fileCount && r == BRAIN_SYNC_SHIPPED_OK; i++) {
        const BrainSyncFile *f = &local.files[i];
        const BrainSyncShippedFile *s =
            shippedFind(table, count, name, nameLen, f->path);
        if (s == NULL ||
            !shippedListed(table, count, name, nameLen, f->path, useOpt)) {
            r = BRAIN_SYNC_SHIPPED_EXTRA;
        } else {
            char hex[65];
            shaHex(f->sha, hex);
            if (s->sha256 == NULL || SDL_strcasecmp(hex, s->sha256) != 0) {
                r = BRAIN_SYNC_SHIPPED_CHANGED;
            }
        }
        if (r != BRAIN_SYNC_SHIPPED_OK && file != NULL) {
            SDL_strlcpy(file, f->path, fileSz);
        }
    }

    /* Every shipped file the brain runs is on disk. */
    for (i = 0; i < count && r == BRAIN_SYNC_SHIPPED_OK; i++) {
        const char *p = table[i].path;
        const char *rel;
        int j;
        bool found = false;
        if (p == NULL || strncmp(p, name, nameLen) != 0 || p[nameLen] != '/') {
            continue;
        }
        rel = p + nameLen + 1;
        if (!shippedListed(table, count, name, nameLen, rel, useOpt)) continue;
        for (j = 0; j < local.fileCount && !found; j++) {
            found = strcmp(local.files[j].path, rel) == 0;
        }
        if (!found) {
            r = BRAIN_SYNC_SHIPPED_MISSING;
            if (file != NULL) SDL_strlcpy(file, rel, fileSz);
        }
    }
    free(local.files);
    return r;
}

const char *brainSyncShippedResultText(BrainSyncShippedResult r) {
    switch (r) {
    case BRAIN_SYNC_SHIPPED_OK:          return "the same as shipped";
    case BRAIN_SYNC_SHIPPED_NOT_SHIPPED: return "not a brain this build shipped";
    case BRAIN_SYNC_SHIPPED_UNREADABLE:  return "not readable";
    case BRAIN_SYNC_SHIPPED_MISSING:     return "missing";
    case BRAIN_SYNC_SHIPPED_CHANGED:     return "changed since it shipped";
    case BRAIN_SYNC_SHIPPED_EXTRA:       return "a file this build did not ship";
    }
    return "?";
}

bool brainSyncPlanPacked(const BrainSyncManifest *m, const char *root,
                         const char *installRoot, BrainSyncPlan *plan) {
    int b, i, total = 0;

    memset(plan, 0, sizeof(*plan));
    for (b = 0; b < m->brainCount; b++) total += m->brains[b].fileCount;
    plan->need = (uint32_t *)calloc((size_t)(total > 0 ? total : 1),
                                    sizeof(uint32_t));
    if (plan->need == NULL) return false;
    for (b = 0; b < m->brainCount; b++) {
        const BrainSyncBrain *br = &m->brains[b];
        char stage[1100];
        int need0 = plan->needCount, staged = 0;

        /* Run in place only outside debug mode: a debug brain may write
         * files into its own folder, and the install is never written. */
        if (installRoot != NULL && m->haveSha && !m->debugMode &&
            installIsSame(installRoot, br)) {
            plan->inPlace[b] = true;
            plan->installCount += br->fileCount;
            plan->installOf[b] = br->fileCount;
            continue;
        }
        if (!brainSyncStageDir(stage, sizeof(stage), root, br->name,
                               br->version)) {
            goto refused;
        }
        (void)brainSyncRemoveTree(stage);
        for (i = 0; i < br->fileCount; i++) {
            const BrainSyncFile *f = &br->files[i];
            char target[1400], staging[1400], inst[1400];
            if (!brainSyncCacheFile(target, sizeof(target), root, br->name,
                                    br->version, f->path) ||
                (size_t)SDL_snprintf(staging, sizeof(staging), "%s/%s", stage,
                                     f->path) >= sizeof(staging)) {
                goto refused;
            }
            if (brainSyncFileIs(target, f, m->haveSha)) {
                plan->haveCount++;
                plan->cacheOf[b]++;
            } else if (copyFromSiblingAs(root, br, f, staging, m->haveSha)) {
                plan->copyCount++;
                plan->cacheOf[b]++;
                staged++;
            } else if (installRoot != NULL && m->haveSha &&
                       (size_t)SDL_snprintf(inst, sizeof(inst), "%s/%s/%s",
                                            installRoot, br->name, f->path)
                           < sizeof(inst) &&
                       copyIfSame(inst, f, true, staging)) {
                plan->installCount++;
                plan->installOf[b]++;
                staged++;
            } else {
                plan->need[plan->needCount++] = ((uint32_t)b << 16) | (uint32_t)i;
                plan->needBytes += f->size;
            }
        }
        plan->staged[b] = staged > 0 || plan->needCount > need0;
    }
    return true;

refused:
    free(plan->need);
    memset(plan, 0, sizeof(*plan));
    return false;
}

bool brainSyncCommitStage(const char *root, const BrainSyncBrain *br) {
    char stage[1100], ver[1100];
    SDL_PathInfo pi;
    bool ok = true;
    int i;

    if (!brainSyncStageDir(stage, sizeof(stage), root, br->name, br->version) ||
        !brainSyncCacheDir(ver, sizeof(ver), root, br->name, br->version)) {
        return false;
    }
    if (!SDL_GetPathInfo(stage, &pi)) return true;
    if (!SDL_GetPathInfo(ver, &pi)) {
        /* No version folder yet: the whole folder at once. */
        if (SDL_RenamePath(stage, ver)) return true;
    }
    for (i = 0; i < br->fileCount; i++) {
        char from[1400], to[1400], dir[1400];
        size_t n;
        if ((size_t)SDL_snprintf(from, sizeof(from), "%s/%s", stage,
                                 br->files[i].path) >= sizeof(from) ||
            (size_t)SDL_snprintf(to, sizeof(to), "%s/%s", ver,
                                 br->files[i].path) >= sizeof(to)) {
            ok = false;
            continue;
        }
        if (!SDL_GetPathInfo(from, &pi)) continue;
        SDL_strlcpy(dir, to, sizeof(dir));
        n = strlen(dir);
        while (n > 0 && dir[n - 1] != '/') n--;
        if (n > 0) {
            dir[n - 1] = '\0';
            (void)SDL_CreateDirectory(dir);
        }
        (void)SDL_RemovePath(to);
        if (!SDL_RenamePath(from, to)) ok = false;
    }
    if (!brainSyncRemoveTree(stage)) ok = false;
    return ok;
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
