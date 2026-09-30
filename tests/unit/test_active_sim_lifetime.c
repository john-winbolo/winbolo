/*
 * Lifetime and per-thread correctness of serverSimGetActive().
 *
 * `activeSim` (server_sim.c) is THREAD_LOCAL, so every thread carries
 * its own cached "sim currently being operated on". Two properties have
 * to hold for the callers that recover a sim through it — chiefly
 * transportUdpServerGetPlayerName / GetClientCountryCode / GetClientType,
 * which take no sim argument:
 *
 *   1. A thread that ticks a sim has its slot pointing at that sim, in
 *      EVERY server state — not just serverStateRunning. The lobby tick
 *      runs deferred work (logWriteTick -> the dedicated-log pre-tick
 *      drain) that calls serverSimGetActive().
 *
 *   2. A slot must never outlive the sim it names. serverSimDestroy runs
 *      on whichever thread tears the server down (the main thread, from
 *      gameFrontShutdownServer), and it can only clear its OWN slot —
 *      every other thread keeps a dangling pointer.
 *
 * Both were violated together in the wild: SDL's timer thread ticked a
 * hosted server, the main thread destroyed it, a second server was
 * started in the same process, and the first lobby tick of that second
 * server ran the log drain -> handleLobbyEnter ->
 * transportUdpServerGetPlayerName -> serverSimGetActive(), which handed
 * back the freed first sim. botManagerIsBot then read through it:
 * EXCEPTION_ACCESS_VIOLATION_READ at sim+0x859.
 *
 * These tests fail on the pre-fix tree — the first because the lobby
 * branch of simRunHalfStep returns before the assignment, the second
 * because the cross-thread destroy leaves the worker's slot stale.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "everard_map.h"
#include "threads.h"
#include "test_harness.h"

/* ------------------------------------------------------------------ */
/* 1. The ticking thread's slot is armed during a LOBBY tick.          */
/* ------------------------------------------------------------------ */

typedef struct {
    ServerSim    *sim;
    SDL_AtomicInt preWasNull;   /* fresh thread starts with an empty slot */
    SDL_AtomicInt postIsSim;    /* the lobby tick armed it */
} LobbyTickArgs;

static int SDLCALL lobby_tick_fn(void *opaque) {
    LobbyTickArgs *args = (LobbyTickArgs *)opaque;

    /* A thread that has never touched a sim must start empty; this is
     * the SDL timer thread's state on the first tick of a process. */
    SDL_SetAtomicInt(&args->preWasNull, serverSimGetActive() == NULL ? 1 : 0);

    threadsWaitForMutex();
    serverSimTick(args->sim);
    threadsReleaseMutex();

    /* Post-condition: the thread that just ticked this sim can recover
     * it. Everything logWriteTick defers into the pre-tick hook runs on
     * this thread and reaches for the sim exactly this way. */
    SDL_SetAtomicInt(&args->postIsSim,
                     serverSimGetActive() == args->sim ? 1 : 0);
    return 0;
}

int run_active_sim_armed_on_lobby_tick(void) {
    BYTE emap[6000] = E_MAP;

    if (!threadsCreate(TRUE)) {
        UT_FAIL("threadsCreate failed");
    }

    /* A freshly created sim sits in serverStateLobby — the state the
     * host/SP startup path ticks in before the countdown. */
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "expected a lobby-state sim, got state %d",
                  (int)serverSimGetState(sim));

    LobbyTickArgs args;
    args.sim = sim;
    SDL_SetAtomicInt(&args.preWasNull, 0);
    SDL_SetAtomicInt(&args.postIsSim, 0);

    SDL_Thread *ticker = SDL_CreateThread(lobby_tick_fn, "lobby-ticker", &args);
    UT_ASSERT(ticker != NULL);
    SDL_WaitThread(ticker, NULL);

    int pre  = SDL_GetAtomicInt(&args.preWasNull);
    int post = SDL_GetAtomicInt(&args.postIsSim);

    serverSimDestroy(sim);
    threadsDestroy();

    UT_ASSERT_MSG(pre == 1,
                  "worker thread did not start with an empty activeSim slot");
    UT_ASSERT_MSG(post == 1,
                  "serverSimGetActive() != the sim just ticked — the lobby "
                  "branch of simRunHalfStep returns before activeSim is set, "
                  "so the log drain it runs recovers the wrong sim");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 2. A slot never outlives the sim it names.                          */
/* ------------------------------------------------------------------ */

typedef struct {
    ServerSim    *sim;
    SDL_AtomicInt armed;        /* worker: my slot now names sim */
    SDL_AtomicInt destroyed;    /* main:   sim has been freed */
    SDL_AtomicInt armedIsSim;
    SDL_AtomicInt afterIsNull;
} DanglingArgs;

static int SDLCALL dangling_fn(void *opaque) {
    DanglingArgs *args = (DanglingArgs *)opaque;

    /* Tick the running sim so this thread's slot names it — exactly what
     * the SDL timer thread does for a hosted server. */
    threadsWaitForMutex();
    serverSimTick(args->sim);
    threadsReleaseMutex();
    SDL_SetAtomicInt(&args->armedIsSim,
                     serverSimGetActive() == args->sim ? 1 : 0);

    SDL_SetAtomicInt(&args->armed, 1);
    while (SDL_GetAtomicInt(&args->destroyed) == 0) {
        SDL_Delay(1);
    }

    /* The sim is gone. Compare the pointer only — dereferencing is the
     * very bug under test. A stale non-NULL answer here is what the
     * crash reduced to. */
    SDL_SetAtomicInt(&args->afterIsNull,
                     serverSimGetActive() == NULL ? 1 : 0);
    return 0;
}

int run_active_sim_cleared_on_cross_thread_destroy(void) {
    if (!threadsCreate(TRUE)) {
        UT_FAIL("threadsCreate failed");
    }

    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    DanglingArgs args;
    args.sim = sim;
    SDL_SetAtomicInt(&args.armed, 0);
    SDL_SetAtomicInt(&args.destroyed, 0);
    SDL_SetAtomicInt(&args.armedIsSim, 0);
    SDL_SetAtomicInt(&args.afterIsNull, 0);

    SDL_Thread *worker = SDL_CreateThread(dangling_fn, "sim-ticker", &args);
    UT_ASSERT(worker != NULL);

    while (SDL_GetAtomicInt(&args.armed) == 0) {
        SDL_Delay(1);
    }

    /* Teardown from a different thread than the one that ticked, which
     * is the real arrangement: gameFrontShutdownServer on the main
     * thread, ticks on SDL's timer thread. */
    threadsWaitForMutex();
    serverSimDestroy(sim);
    threadsReleaseMutex();
    SDL_SetAtomicInt(&args.destroyed, 1);

    SDL_WaitThread(worker, NULL);

    int armedIsSim  = SDL_GetAtomicInt(&args.armedIsSim);
    int afterIsNull = SDL_GetAtomicInt(&args.afterIsNull);

    threadsDestroy();

    UT_ASSERT_MSG(armedIsSim == 1,
                  "worker's activeSim slot was not armed by serverSimTick");
    UT_ASSERT_MSG(afterIsNull == 1,
                  "serverSimGetActive() still returns the destroyed sim on a "
                  "thread other than the destroying one — every caller that "
                  "recovers a sim this way reads freed memory");
    return 0;
}
