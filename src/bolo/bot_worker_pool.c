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
 *Name:          Bot Worker Pool
 *Filename:      bot_worker_pool.c
 *Author:        John Morrison
 *Purpose:
 *  Fixed-size SDL3-backed worker thread pool for parallel
 *  per-bot jobs. Process-wide singleton, single producer
 *  (the main thread). Pre-work for the upcoming parallel
 *  brain-tick path; no caller exists yet.
 *
 *  Queueing model — per-worker semaphore pairs:
 *    Each worker owns one go semaphore, one done semaphore,
 *    and a single job slot. The producer dispatches by
 *    writing the slot then signalling that worker's go.
 *    On queue wrap (count > workerCount) the producer
 *    waits on that specific worker's done before reusing
 *    its slot.
 *
 *    Picked over a shared queue with a single done sem
 *    because per-worker ownership makes "this slot is
 *    free" trivially provable from the matching done-wait,
 *    with no ambiguity about which worker actually freed
 *    it. A generic done signal only tells you SOME worker
 *    finished, so reusing a slot after one such signal
 *    can race a still-running worker. Per-worker pairs
 *    sidestep this; bookkeeping stays O(workerCount).
 *********************************************************/

#include "bot_worker_pool.h"

#include <SDL3/SDL.h>

#include "../common/wb_log.h"

#if defined(__EMSCRIPTEN__)

/* WASM is single-threaded under the build configurations we ship; the
 * pool degrades to a no-op shell that runs every job inline on the
 * calling thread. The four entry points keep the same signatures so
 * callers compile unchanged. */

bool botWorkerPoolCreate(int workerCount) {
    (void)workerCount;
    return true;
}

void botWorkerPoolDestroy(void) {
}

void botWorkerPoolRun(const int *botIndices, int count,
                      BotJobFn jobFn, void *userData) {
    for (int i = 0; i < count; i++) {
        jobFn(botIndices[i], userData);
    }
}

int botWorkerPoolGetSize(void) {
    return 0;
}

#else /* !__EMSCRIPTEN__ */

typedef struct BotWorker {
    SDL_Thread    *thread;
    SDL_Semaphore *go;
    SDL_Semaphore *done;
    int            id;
    /* Job slot. Producer writes these before SDL_SignalSemaphore(go);
     * worker reads them after SDL_WaitSemaphore(go). The signal/wait
     * pair is a release/acquire, so no explicit barriers are needed. */
    BotJobFn       jobFn;
    void          *userData;
    int            botIndex;
} BotWorker;

static struct {
    BotWorker    *workers;
    int           workerCount;
    SDL_AtomicInt quit;
} g_pool;

static int workerThreadFn(void *data) {
    BotWorker *w = (BotWorker *)data;
    char name[32];
    SDL_snprintf(name, sizeof(name), "botpool-%d", w->id);
    SDL_SetCurrentThreadName(name);

    for (;;) {
        SDL_WaitSemaphore(w->go);
        if (SDL_GetAtomicInt(&g_pool.quit)) {
            return 0;
        }
        w->jobFn(w->botIndex, w->userData);
        SDL_SignalSemaphore(w->done);
    }
}

/* Tear down whatever exists. `threadsCreated` is the number of entries
 * in g_pool.workers[] whose `thread` field has a live SDL_Thread that
 * needs to be signalled to quit and joined. Semaphores are destroyed
 * for every slot whose go/done pointers are non-NULL, regardless of
 * whether the corresponding thread was ever created. Safe to call from
 * the failure path of botWorkerPoolCreate (where some semaphores or
 * threads may never have been created) and from botWorkerPoolDestroy
 * (where everything was created). */
static void poolTeardown(int threadsCreated) {
    if (g_pool.workers == NULL) {
        return;
    }
    SDL_SetAtomicInt(&g_pool.quit, 1);
    for (int i = 0; i < threadsCreated; i++) {
        if (g_pool.workers[i].go != NULL) {
            SDL_SignalSemaphore(g_pool.workers[i].go);
        }
    }
    for (int i = 0; i < threadsCreated; i++) {
        if (g_pool.workers[i].thread != NULL) {
            SDL_WaitThread(g_pool.workers[i].thread, NULL);
        }
    }
    for (int i = 0; i < g_pool.workerCount; i++) {
        if (g_pool.workers[i].go != NULL) {
            SDL_DestroySemaphore(g_pool.workers[i].go);
        }
        if (g_pool.workers[i].done != NULL) {
            SDL_DestroySemaphore(g_pool.workers[i].done);
        }
    }
    SDL_free(g_pool.workers);
    g_pool.workers = NULL;
    g_pool.workerCount = 0;
    SDL_SetAtomicInt(&g_pool.quit, 0);
}

bool botWorkerPoolCreate(int workerCount) {
    if (g_pool.workers != NULL) {
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM,
                    "bot worker pool already created (size %d); ignoring",
                    g_pool.workerCount);
        return true;
    }
    if (workerCount <= 0) {
        return true;
    }
    if (workerCount > MAX_TANKS - 1) {
        workerCount = MAX_TANKS - 1;
    }

    g_pool.workers = (BotWorker *)SDL_calloc((size_t)workerCount,
                                             sizeof(BotWorker));
    if (g_pool.workers == NULL) {
        WB_LOG_ERROR(WB_LOG_CAT_PLATFORM,
                     "bot worker pool: workers alloc failed for %d workers",
                     workerCount);
        return false;
    }
    g_pool.workerCount = workerCount;
    SDL_SetAtomicInt(&g_pool.quit, 0);

    /* Allocate every semaphore first; on failure poolTeardown can wipe
     * them in one pass without needing to know which were created. */
    for (int i = 0; i < workerCount; i++) {
        g_pool.workers[i].id   = i;
        g_pool.workers[i].go   = SDL_CreateSemaphore(0);
        g_pool.workers[i].done = SDL_CreateSemaphore(0);
        if (g_pool.workers[i].go == NULL ||
            g_pool.workers[i].done == NULL) {
            WB_LOG_ERROR(WB_LOG_CAT_PLATFORM,
                         "bot worker pool: semaphore create failed at "
                         "worker %d", i);
            poolTeardown(0);
            return false;
        }
    }

    /* Threads come last so any partial failure can join only what was
     * actually created. */
    for (int i = 0; i < workerCount; i++) {
        char name[32];
        SDL_snprintf(name, sizeof(name), "botpool-%d", i);
        g_pool.workers[i].thread =
            SDL_CreateThread(workerThreadFn, name, &g_pool.workers[i]);
        if (g_pool.workers[i].thread == NULL) {
            WB_LOG_ERROR(WB_LOG_CAT_PLATFORM,
                         "bot worker pool: thread create failed at "
                         "worker %d (created %d of %d)",
                         i, i, workerCount);
            poolTeardown(i);
            return false;
        }
    }

    WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
                "bot worker pool: %d workers ready", workerCount);
    return true;
}

void botWorkerPoolDestroy(void) {
    if (g_pool.workers == NULL) {
        return;
    }
    poolTeardown(g_pool.workerCount);
}

void botWorkerPoolRun(const int *botIndices, int count,
                      BotJobFn jobFn, void *userData) {
    if (g_pool.workerCount == 0 || count <= 1) {
        for (int i = 0; i < count; i++) {
            jobFn(botIndices[i], userData);
        }
        return;
    }

    const int N = g_pool.workerCount;
    const int toDispatch = count - 1;

    /* Push toDispatch jobs into per-worker slots round-robin. On wrap
     * we wait on the specific slot's done semaphore — that is what
     * makes "the slot is free" provable; a generic done signal would
     * only tell us SOME worker finished. */
    for (int i = 0; i < toDispatch; i++) {
        BotWorker *w = &g_pool.workers[i % N];
        if (i >= N) {
            SDL_WaitSemaphore(w->done);
        }
        w->jobFn    = jobFn;
        w->userData = userData;
        w->botIndex = botIndices[i];
        SDL_SignalSemaphore(w->go);
    }

    /* Producer runs the last job inline so it isn't idle on the wait;
     * with workerCount = N-1 plus the producer this delivers honest
     * N-way parallelism. */
    jobFn(botIndices[count - 1], userData);

    /* Drain the remaining done signals. After the dispatch loop above,
     * every worker that ran any jobs has exactly one outstanding done
     * (later wraps absorbed all earlier ones). When toDispatch < N,
     * only the first toDispatch workers ran anything. */
    const int outstanding = (toDispatch < N) ? toDispatch : N;
    for (int i = 0; i < outstanding; i++) {
        SDL_WaitSemaphore(g_pool.workers[i].done);
    }
}

int botWorkerPoolGetSize(void) {
    return g_pool.workerCount;
}

#endif /* __EMSCRIPTEN__ */
