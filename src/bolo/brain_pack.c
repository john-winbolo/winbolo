/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Brain pack
 *Filename:      brain_pack.c
 *Purpose:
 *  The packed brain file stream between a server and a
 *  Bot Host, and the session folders of the Bot Host inside
 *  a client. See brain_pack.h.
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <zstd.h>

#include "brain_pack.h"
#include "machine_id.h"   /* machineRandomBytes */

BrainPackManifestChoice brainPackManifestChoice(bool capsSeen, uint8_t caps,
                                                uint32_t ticksSinceJoin) {
    if (capsSeen) {
        return (caps & BRAIN_PACK_CAP_PACKED) ? BRAIN_PACK_MANIFEST_V2
                                              : BRAIN_PACK_MANIFEST_V1;
    }
    return ticksSinceJoin >= BRAIN_PACK_CAPS_WAIT_TICKS ? BRAIN_PACK_MANIFEST_V1
                                                        : BRAIN_PACK_MANIFEST_WAIT;
}

/* ── Bitmaps ───────────────────────────────────────────────────────────── */

void brainPackBitSet(uint8_t *bits, int i) {
    if (i < 0 || i >= BRAIN_SYNC_MAX_FILES) return;
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

bool brainPackBitGet(const uint8_t *bits, int i) {
    if (i < 0 || i >= BRAIN_SYNC_MAX_FILES) return false;
    return (bits[i >> 3] & (1u << (i & 7))) != 0;
}

int brainPackBitCount(const uint8_t *bits, int n) {
    int i, c = 0;
    if (n > BRAIN_SYNC_MAX_FILES) n = BRAIN_SYNC_MAX_FILES;
    for (i = 0; i < n; i++) {
        if (brainPackBitGet(bits, i)) c++;
    }
    return c;
}

void brainPackBitsFromPlan(const BrainSyncPlan *plan, int b,
                           uint8_t bits[BRAIN_PACK_BITMAP_BYTES]) {
    int i;
    memset(bits, 0, BRAIN_PACK_BITMAP_BYTES);
    for (i = 0; i < plan->needCount; i++) {
        if ((int)(plan->need[i] >> 16) == b) {
            brainPackBitSet(bits, (int)(plan->need[i] & 0xffffu));
        }
    }
}

static void putU16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void putU32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t getU16(const uint8_t *p) {
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}
static uint32_t getU32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ── Writer ────────────────────────────────────────────────────────────── */

struct BrainPackWriter {
    ZSTD_CCtx *cctx;
    uint8_t   *out;
    size_t     outHead, outLen, outCap;
    bool       finished;
    bool       failed;
    uint64_t   raw, comp;
};

static bool writerRun(BrainPackWriter *w, const void *data, size_t len,
                      ZSTD_EndDirective mode) {
    ZSTD_inBuffer in;
    in.src = data != NULL ? data : (const void *)"";
    in.size = len;
    in.pos = 0;
    if (w->failed || w->finished) return false;
    for (;;) {
        ZSTD_outBuffer o;
        size_t r, want = ZSTD_CStreamOutSize();
        if (w->outHead > 0 && w->outHead == w->outLen) {
            w->outHead = w->outLen = 0;
        }
        if (w->outCap - w->outLen < want) {
            size_t ncap = w->outCap ? w->outCap * 2 : 2 * want;
            uint8_t *n;
            while (ncap - w->outLen < want) ncap *= 2;
            n = (uint8_t *)realloc(w->out, ncap);
            if (n == NULL) { w->failed = true; return false; }
            w->out = n;
            w->outCap = ncap;
        }
        o.dst = w->out + w->outLen;
        o.size = w->outCap - w->outLen;
        o.pos = 0;
        r = ZSTD_compressStream2(w->cctx, &o, &in, mode);
        if (ZSTD_isError(r)) { w->failed = true; return false; }
        w->outLen += o.pos;
        w->comp += o.pos;
        if (mode == ZSTD_e_end) {
            if (r == 0) break;
        } else if (in.pos == in.size) {
            break;
        }
    }
    w->raw += len;
    return true;
}

BrainPackWriter *brainPackWriterNew(int level, uint8_t brain, uint16_t count) {
    BrainPackWriter *w = (BrainPackWriter *)calloc(1, sizeof(*w));
    uint8_t hdr[6];
    if (w == NULL) return NULL;
    w->cctx = ZSTD_createCCtx();
    if (w->cctx == NULL ||
        ZSTD_isError(ZSTD_CCtx_setParameter(w->cctx, ZSTD_c_compressionLevel,
                                            level))) {
        brainPackWriterFree(w);
        return NULL;
    }
    hdr[0] = 'B';
    hdr[1] = 'P';
    hdr[2] = 1;
    hdr[3] = brain;
    putU16(hdr + 4, count);
    if (!writerRun(w, hdr, sizeof(hdr), ZSTD_e_continue)) {
        brainPackWriterFree(w);
        return NULL;
    }
    return w;
}

bool brainPackWriterBeginFile(BrainPackWriter *w, uint16_t file, uint32_t size) {
    uint8_t rec[6];
    putU16(rec, file);
    putU32(rec + 2, size);
    return w != NULL && writerRun(w, rec, sizeof(rec), ZSTD_e_continue);
}

bool brainPackWriterPut(BrainPackWriter *w, const void *data, size_t len) {
    return w != NULL && writerRun(w, data, len, ZSTD_e_continue);
}

bool brainPackWriterFinish(BrainPackWriter *w) {
    if (w == NULL || !writerRun(w, NULL, 0, ZSTD_e_end)) return false;
    w->finished = true;
    return true;
}

size_t brainPackWriterPending(const BrainPackWriter *w) {
    return w != NULL ? w->outLen - w->outHead : 0;
}

size_t brainPackWriterTake(BrainPackWriter *w, uint8_t *dst, size_t cap) {
    size_t n = brainPackWriterPending(w);
    if (n > cap) n = cap;
    if (n == 0) return 0;
    memcpy(dst, w->out + w->outHead, n);
    w->outHead += n;
    if (w->outHead == w->outLen) w->outHead = w->outLen = 0;
    return n;
}

bool brainPackWriterFinished(const BrainPackWriter *w) {
    return w != NULL && w->finished;
}

bool brainPackWriterDone(const BrainPackWriter *w) {
    return w != NULL && w->finished && brainPackWriterPending(w) == 0;
}

uint64_t brainPackWriterRawBytes(const BrainPackWriter *w) {
    return w != NULL ? w->raw : 0;
}

uint64_t brainPackWriterOutBytes(const BrainPackWriter *w) {
    return w != NULL ? w->comp : 0;
}

void brainPackWriterFree(BrainPackWriter *w) {
    if (w == NULL) return;
    ZSTD_freeCCtx(w->cctx);
    free(w->out);
    free(w);
}

/* ── Reader ────────────────────────────────────────────────────────────── */

enum { RD_HEADER, RD_RECORD, RD_BODY, RD_END };

struct BrainPackReader {
    ZSTD_DCtx            *dctx;
    const BrainSyncBrain *br;
    uint8_t               brain;
    uint8_t               bits[BRAIN_PACK_BITMAP_BYTES];
    int                   want;     /* files set in bits */
    int                   got;      /* files given to fn */
    int                   next;     /* the bit to look from */
    int                   state;
    uint8_t               hdr[8];
    size_t                hdrLen;
    int                   file;
    uint32_t              size, have;
    uint8_t              *body;
    bool                  frameEnded;
    BrainPackStatus       status;
};

BrainPackReader *brainPackReaderNew(const BrainSyncBrain *br, uint8_t brain,
                                    const uint8_t bits[BRAIN_PACK_BITMAP_BYTES]) {
    BrainPackReader *r;
    if (br == NULL || bits == NULL) return NULL;
    r = (BrainPackReader *)calloc(1, sizeof(*r));
    if (r == NULL) return NULL;
    r->dctx = ZSTD_createDCtx();
    if (r->dctx == NULL) { free(r); return NULL; }
    r->br = br;
    r->brain = brain;
    memcpy(r->bits, bits, sizeof(r->bits));
    r->want = brainPackBitCount(bits, br->fileCount);
    r->state = RD_HEADER;
    r->file = -1;
    r->status = BRAIN_PACK_MORE;
    return r;
}

/* The next file the stream must hold, or -1. */
static int readerNextFile(BrainPackReader *r) {
    int i;
    for (i = r->next; i < r->br->fileCount; i++) {
        if (brainPackBitGet(r->bits, i)) return i;
    }
    return -1;
}

static bool readerTake(BrainPackReader *r, const uint8_t *p, size_t n,
                       BrainPackFileFn fn, void *ctx) {
    size_t pos = 0;
    while (pos < n) {
        if (r->state == RD_END) return false;   /* bytes after the last file */
        if (r->state == RD_HEADER || r->state == RD_RECORD) {
            size_t need = 6 - r->hdrLen, k = n - pos;
            if (k > need) k = need;
            memcpy(r->hdr + r->hdrLen, p + pos, k);
            r->hdrLen += k;
            pos += k;
            if (r->hdrLen < 6) continue;
            r->hdrLen = 0;
            if (r->state == RD_HEADER) {
                if (r->hdr[0] != 'B' || r->hdr[1] != 'P' || r->hdr[2] != 1 ||
                    r->hdr[3] != r->brain || getU16(r->hdr + 4) != r->want) {
                    return false;
                }
                r->state = r->want > 0 ? RD_RECORD : RD_END;
                continue;
            }
            {
                int file = getU16(r->hdr), expect = readerNextFile(r);
                uint32_t size = getU32(r->hdr + 2);
                if (file != expect || size != r->br->files[file].size) {
                    return false;
                }
                r->file = file;
                r->size = size;
                r->have = 0;
                r->body = (uint8_t *)malloc(size > 0 ? size : 1);
                if (r->body == NULL) return false;
                r->state = RD_BODY;
            }
        }
        if (r->state == RD_BODY) {
            size_t k = n - pos;
            if (k > r->size - r->have) k = r->size - r->have;
            memcpy(r->body + r->have, p + pos, k);
            r->have += (uint32_t)k;
            pos += k;
            if (r->have < r->size) continue;
            if (fn != NULL) (void)fn(ctx, r->file, r->body, r->size);
            free(r->body);
            r->body = NULL;
            r->got++;
            r->next = r->file + 1;
            r->file = -1;
            r->state = r->got >= r->want ? RD_END : RD_RECORD;
        }
    }
    return true;
}

BrainPackStatus brainPackReaderFeed(BrainPackReader *r, const uint8_t *data,
                                    size_t len, BrainPackFileFn fn, void *ctx) {
    uint8_t buf[64 * 1024];
    ZSTD_inBuffer in;
    if (r == NULL) return BRAIN_PACK_BAD;
    if (r->status != BRAIN_PACK_MORE) {
        if (r->status == BRAIN_PACK_DONE && len > 0) r->status = BRAIN_PACK_BAD;
        return r->status;
    }
    in.src = data;
    in.size = len;
    in.pos = 0;
    while (in.pos < in.size) {
        ZSTD_outBuffer o;
        size_t z;
        if (r->frameEnded) { r->status = BRAIN_PACK_BAD; return r->status; }
        o.dst = buf;
        o.size = sizeof(buf);
        o.pos = 0;
        z = ZSTD_decompressStream(r->dctx, &o, &in);
        if (ZSTD_isError(z)) { r->status = BRAIN_PACK_BAD; return r->status; }
        if (!readerTake(r, buf, o.pos, fn, ctx)) {
            r->status = BRAIN_PACK_BAD;
            return r->status;
        }
        if (z == 0) r->frameEnded = true;
        /* A full buffer may hold more output for the same input. */
        while (o.pos == o.size && !r->frameEnded) {
            o.pos = 0;
            z = ZSTD_decompressStream(r->dctx, &o, &in);
            if (ZSTD_isError(z)) { r->status = BRAIN_PACK_BAD; return r->status; }
            if (!readerTake(r, buf, o.pos, fn, ctx)) {
                r->status = BRAIN_PACK_BAD;
                return r->status;
            }
            if (z == 0) r->frameEnded = true;
        }
    }
    if (r->frameEnded) {
        r->status = r->state == RD_END ? BRAIN_PACK_DONE : BRAIN_PACK_BAD;
    }
    return r->status;
}

int brainPackReaderGot(const BrainPackReader *r) {
    return r != NULL ? r->got : 0;
}

void brainPackReaderFree(BrainPackReader *r) {
    if (r == NULL) return;
    ZSTD_freeDCtx(r->dctx);
    free(r->body);
    free(r);
}

/* ── Session folders ───────────────────────────────────────────────────── */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

static const char kMarkerText[] = "WinBolo Bot Host session folder\n";

static bool isHex16(const char *s) {
    int i;
    for (i = 0; i < 16; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[16] == '\0';
}

/* Open and hold dir's lock file; -1 when another holder has it. Windows:
 * the file opened with no sharing, which no other open can get past.
 * Elsewhere: an flock, which another open of the file cannot take. */
static intptr_t lockTake(const char *dir) {
    char path[1100];
    if ((size_t)SDL_snprintf(path, sizeof(path), "%s/%s", dir,
                             BRAIN_SESSION_LOCK) >= sizeof(path)) {
        return -1;
    }
#ifdef _WIN32
    {
        wchar_t w[1100];
        HANDLE h;
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w,
                                (int)(sizeof(w) / sizeof(w[0]))) == 0) {
            return -1;
        }
        h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return -1;
        return (intptr_t)h;
    }
#else
    {
        int fd = open(path, O_RDWR | O_CREAT, 0600);
        if (fd < 0) return -1;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            return -1;
        }
        return (intptr_t)fd;
    }
#endif
}

static void lockRelease(intptr_t lock) {
    if (lock == -1) return;
#ifdef _WIN32
    CloseHandle((HANDLE)lock);
#else
    flock((int)lock, LOCK_UN);
    close((int)lock);
#endif
}

/* True when dir holds this code's marker. */
static bool hasMarker(const char *dir) {
    char path[1100];
    SDL_PathInfo pi;
    size_t sz = 0;
    void *data;
    bool ok;
    if ((size_t)SDL_snprintf(path, sizeof(path), "%s/%s", dir,
                             BRAIN_SESSION_MARKER) >= sizeof(path)) {
        return false;
    }
    if (!SDL_GetPathInfo(path, &pi) || pi.type != SDL_PATHTYPE_FILE ||
        pi.size != sizeof(kMarkerText) - 1) {
        return false;
    }
    data = SDL_LoadFile(path, &sz);
    ok = data != NULL && sz == sizeof(kMarkerText) - 1 &&
         memcmp(data, kMarkerText, sz) == 0;
    SDL_free(data);
    return ok;
}

/* True when dir is "<cacheRoot>/session/<16 hex>". */
static bool isSessionPath(const char *cacheRoot, const char *dir) {
    char base[1100];
    size_t n;
    if ((size_t)SDL_snprintf(base, sizeof(base), "%s/session/", cacheRoot)
        >= sizeof(base)) {
        return false;
    }
    n = strlen(base);
    return strncmp(dir, base, n) == 0 && isHex16(dir + n);
}

void brainSessionInit(BrainSession *s) {
    if (s == NULL) return;
    s->dir[0] = '\0';
    s->lock = -1;
}

bool brainSessionCreate(const char *cacheRoot, BrainSession *s) {
    uint8_t id[8];
    char hex[17], marker[1100];
    int i;
    if (cacheRoot == NULL || s == NULL) return false;
    brainSessionInit(s);
    if (!machineRandomBytes(id, sizeof(id))) {
        uint64_t t = SDL_GetPerformanceCounter() ^ SDL_GetTicksNS();
        for (i = 0; i < 8; i++) id[i] = (uint8_t)(t >> (i * 8));
    }
    for (i = 0; i < 8; i++) {
        static const char digits[] = "0123456789abcdef";
        hex[i * 2] = digits[id[i] >> 4];
        hex[i * 2 + 1] = digits[id[i] & 15];
    }
    hex[16] = '\0';
    if ((size_t)SDL_snprintf(s->dir, sizeof(s->dir), "%s/session/%s",
                             cacheRoot, hex) >= sizeof(s->dir) ||
        !SDL_CreateDirectory(s->dir)) {
        s->dir[0] = '\0';
        return false;
    }
    /* The lock before the marker: a sweep never finds a marked folder
     * whose lock is not yet held. */
    s->lock = lockTake(s->dir);
    SDL_snprintf(marker, sizeof(marker), "%s/%s", s->dir, BRAIN_SESSION_MARKER);
    if (s->lock == -1 ||
        !brainSyncWriteFile(marker, kMarkerText, sizeof(kMarkerText) - 1)) {
        lockRelease(s->lock);
        s->lock = -1;
        (void)brainSyncRemoveTree(s->dir);
        s->dir[0] = '\0';
        return false;
    }
    return true;
}

void brainSessionUnlock(BrainSession *s) {
    if (s == NULL) return;
    lockRelease(s->lock);
    s->lock = -1;
}

bool brainSessionEnd(const char *cacheRoot, BrainSession *s) {
    bool ok = true;
    if (s == NULL) return false;
    brainSessionUnlock(s);
    if (s->dir[0] != '\0') {
        SDL_PathInfo pi;
        if (cacheRoot == NULL || !isSessionPath(cacheRoot, s->dir)) {
            ok = false;
        } else if (SDL_GetPathInfo(s->dir, &pi)) {
            ok = hasMarker(s->dir) && brainSyncRemoveTree(s->dir);
        }
    }
    s->dir[0] = '\0';
    return ok;
}

typedef struct {
    char names[64][24];
    int  count;
} SessionNames;

static SDL_EnumerationResult SDLCALL sessionName(void *userdata,
                                                 const char *dirname,
                                                 const char *fname) {
    SessionNames *s = (SessionNames *)userdata;
    (void)dirname;
    if (strlen(fname) != 16 || !isHex16(fname)) return SDL_ENUM_CONTINUE;
    if (s->count >= (int)(sizeof(s->names) / sizeof(s->names[0]))) {
        return SDL_ENUM_SUCCESS;
    }
    SDL_strlcpy(s->names[s->count++], fname, sizeof(s->names[0]));
    return SDL_ENUM_CONTINUE;
}

int brainSessionSweep(const char *cacheRoot) {
    char base[1100];
    SessionNames *s;
    int i, removed = 0;
    if (cacheRoot == NULL || cacheRoot[0] == '\0') return 0;
    if ((size_t)SDL_snprintf(base, sizeof(base), "%s/session", cacheRoot)
        >= sizeof(base)) {
        return 0;
    }
    s = (SessionNames *)calloc(1, sizeof(*s));
    if (s == NULL) return 0;
    if (SDL_EnumerateDirectory(base, sessionName, s)) {
        for (i = 0; i < s->count; i++) {
            char dir[1200];
            SDL_PathInfo pi;
            intptr_t lock;
            SDL_snprintf(dir, sizeof(dir), "%s/%s", base, s->names[i]);
            if (!SDL_GetPathInfo(dir, &pi) ||
                pi.type != SDL_PATHTYPE_DIRECTORY || !hasMarker(dir)) {
                continue;
            }
            /* A running program holds the lock: the folder is in use. */
            lock = lockTake(dir);
            if (lock == -1) continue;
            /* Let go first: an open file cannot be removed on Windows. No
             * program takes up a folder it did not make, so none comes in
             * between. */
            lockRelease(lock);
            if (brainSyncRemoveTree(dir)) removed++;
        }
    }
    free(s);
    return removed;
}
