/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          braintest_session_ticks.c
 *PURPOSE:       Tick range of each Load Session row (see the header).
 *********************************************************/
#include "braintest_session_ticks.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "brain_record_ticks.h"
#include "brain_record_walk.h"

#define SESSION_TICKS_MAX 4096

typedef struct {
    int64_t  size;
    int64_t  mtime;
    SessionTicksState state;
    bool     queued;          /* pending and not yet taken by the worker */
    uint32_t first, last;
    char     btrPath[600];
} TickRow;

static TickRow       g_rows[SESSION_TICKS_MAX];
static int           g_rowCount = 0;
static char          g_cachePath[600] = "";
static SDL_Mutex    *g_mutex = NULL;
static SDL_Condition *g_wake = NULL;
static SDL_Thread   *g_thread = NULL;
static SDL_AtomicInt g_stop;

/* Caller holds g_mutex (or no thread exists yet). */
static TickRow *findRow(int64_t size, int64_t mtime) {
    for (int i = 0; i < g_rowCount; i++) {
        if (g_rows[i].size == size && g_rows[i].mtime == mtime) return &g_rows[i];
    }
    return NULL;
}

/* Caller holds g_mutex (or no thread exists yet). When the table is full the
 * oldest row is reused; it is only a cache. */
static TickRow *addRow(int64_t size, int64_t mtime) {
    TickRow *r;
    if (g_rowCount < SESSION_TICKS_MAX) {
        r = &g_rows[g_rowCount++];
    } else {
        memmove(&g_rows[0], &g_rows[1], sizeof(TickRow) * (SESSION_TICKS_MAX - 1));
        r = &g_rows[SESSION_TICKS_MAX - 1];
    }
    memset(r, 0, sizeof *r);
    r->size = size;
    r->mtime = mtime;
    return r;
}

/* Rewrite the cache file with every known range. Caller holds g_mutex. */
static void saveCache(void) {
    if (!g_cachePath[0]) return;
    FILE *f = fopen(g_cachePath, "w");
    if (!f) return;
    fprintf(f, "# BrainTest session tick ranges: btr_bytes btr_mtime_ns first_tick last_tick\n");
    for (int i = 0; i < g_rowCount; i++) {
        const TickRow *r = &g_rows[i];
        if (r->state != SESSION_TICKS_HAVE) continue;
        fprintf(f, "%lld %lld %lu %lu\n", (long long)r->size, (long long)r->mtime,
                (unsigned long)r->first, (unsigned long)r->last);
    }
    fclose(f);
}

static void loadCache(void) {
    if (!g_cachePath[0]) return;
    FILE *f = fopen(g_cachePath, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        long long size = 0, mtime = 0;
        unsigned long first = 0, last = 0;
        if (line[0] == '#') continue;
        if (sscanf(line, "%lld %lld %lu %lu", &size, &mtime, &first, &last) != 4) continue;
        if (last < first) continue;
        TickRow *r = findRow(size, mtime);
        if (!r) r = addRow(size, mtime);
        r->state = SESSION_TICKS_HAVE;
        r->first = (uint32_t)first;
        r->last  = (uint32_t)last;
    }
    fclose(f);
}

static bool walkCancelled(void *ctx) {
    (void)ctx;
    return SDL_GetAtomicInt(&g_stop) != 0;
}

/* One worker, so at most one recording is read at a time: the reads are
 * disk and inflate bound, and the main thread keeps the frame rate. */
static int SDLCALL tickWorker(void *unused) {
    (void)unused;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    SDL_LockMutex(g_mutex);
    while (!SDL_GetAtomicInt(&g_stop)) {
        TickRow *job = NULL;
        for (int i = 0; i < g_rowCount; i++) {
            if (g_rows[i].queued) { job = &g_rows[i]; break; }
        }
        if (!job) {
            SDL_WaitCondition(g_wake, g_mutex);
            continue;
        }
        job->queued = false;
        char path[600];
        int64_t size = job->size, mtime = job->mtime;
        SDL_strlcpy(path, job->btrPath, sizeof path);
        SDL_UnlockMutex(g_mutex);

        uint32_t first = 0, last = 0;
        uint64_t frames = 0;
        Uint64 t0 = SDL_GetTicks();
        bool ok = brainRecWalkFileTicks(path, &first, &last, &frames, walkCancelled, NULL);
        Uint64 ms = SDL_GetTicks() - t0;

        SDL_LockMutex(g_mutex);
        if (SDL_GetAtomicInt(&g_stop)) break;   /* cancelled: leave it pending */
        /* The row may have moved (table full); find it again by key. */
        TickRow *r = findRow(size, mtime);
        if (r) {
            r->state = ok ? SESSION_TICKS_HAVE : SESSION_TICKS_FAILED;
            r->first = first;
            r->last  = last;
        }
        if (ok) {
            fprintf(stderr, "Load Session: read %s in %llu ms: ticks %lu..%lu (%llu frames)\n",
                    path, (unsigned long long)ms, (unsigned long)first,
                    (unsigned long)last, (unsigned long long)frames);
            saveCache();
        }
    }
    SDL_UnlockMutex(g_mutex);
    return 0;
}

void sessionTicksInit(const char *cachePath) {
    SDL_strlcpy(g_cachePath, cachePath ? cachePath : "", sizeof g_cachePath);
    if (!g_mutex) g_mutex = SDL_CreateMutex();
    if (!g_wake)  g_wake  = SDL_CreateCondition();
    SDL_SetAtomicInt(&g_stop, 0);
    SDL_LockMutex(g_mutex);
    g_rowCount = 0;
    loadCache();
    SDL_UnlockMutex(g_mutex);
}

SessionTicksState sessionTicksLookup(const char *dir, const char *btrPath,
                                     int64_t size, int64_t mtime,
                                     uint32_t *first, uint32_t *last) {
    SessionTicksState st;
    if (!g_mutex) sessionTicksInit(NULL);

    SDL_LockMutex(g_mutex);
    TickRow *r = findRow(size, mtime);
    if (!r) {
        uint32_t f = 0, l = 0;
        r = addRow(size, mtime);
        SDL_strlcpy(r->btrPath, btrPath, sizeof r->btrPath);
        if (brainRecTicksRead(dir, size, &f, &l)) {
            r->state = SESSION_TICKS_HAVE;
            r->first = f;
            r->last  = l;
        } else {
            /* A recording still being written has a new size on every scan.
             * Only its newest size is worth reading. */
            for (int i = 0; i < g_rowCount; i++) {
                TickRow *o = &g_rows[i];
                if (o != r && o->queued && strcmp(o->btrPath, btrPath) == 0) {
                    o->queued = false;
                    o->state  = SESSION_TICKS_FAILED;
                }
            }
            r->state  = SESSION_TICKS_PENDING;
            r->queued = true;
            if (!g_thread) {
                SDL_SetAtomicInt(&g_stop, 0);
                g_thread = SDL_CreateThread(tickWorker, "sessionticks", NULL);
                if (!g_thread) { r->state = SESSION_TICKS_FAILED; r->queued = false; }
            }
            SDL_SignalCondition(g_wake);
        }
    } else if (r->state == SESSION_TICKS_PENDING && !r->queued && !g_thread) {
        /* Left pending by a cancelled walk; the worker is gone. */
        r->state = SESSION_TICKS_FAILED;
    }
    st = r->state;
    if (st == SESSION_TICKS_HAVE) {
        if (first) *first = r->first;
        if (last)  *last  = r->last;
    }
    SDL_UnlockMutex(g_mutex);
    return st;
}

void sessionTicksSeconds(uint32_t first, uint32_t last,
                         int *startSec, int *endSec, int *lengthSec) {
    if (startSec)  *startSec  = (int)(first / BRAINREC_TICKS_PER_SEC);
    if (endSec)    *endSec    = (int)(last  / BRAINREC_TICKS_PER_SEC);
    if (lengthSec) *lengthSec = (int)((last >= first ? last - first : 0u) / BRAINREC_TICKS_PER_SEC);
}

void sessionTicksShutdown(void) {
    if (!g_mutex) return;
    SDL_SetAtomicInt(&g_stop, 1);
    SDL_LockMutex(g_mutex);
    SDL_SignalCondition(g_wake);
    SDL_UnlockMutex(g_mutex);
    if (g_thread) {
        SDL_WaitThread(g_thread, NULL);
        g_thread = NULL;
    }
}
