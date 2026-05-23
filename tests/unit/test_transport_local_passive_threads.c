/*
 * Concurrent writer/reader on a passive transport_local.
 *
 * Models the unification target: the host-style timer thread drives
 * transportTick (which under passive ownership drains the delay queue
 * into the server's input queue and ticks the server), while the
 * main-thread input path enqueues via transportSendInput without
 * any external locking — the contract the passive transport's
 * planned self-lock is meant to satisfy.
 *
 * Today the passive transport does NOT self-lock, so the
 * writer / reader race on its delay queue indices. The test is
 * structured so that adding the self-lock makes it stop tripping
 * TSan/ASan and makes the lastProcessedInput tally land at exactly N.
 * Without the lock, the queue race may lose or duplicate inputs
 * (which surfaces as a lastProcessedInput < N at the end).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "transport.h"
#include "server_sim.h"
#include "input_packet.h"
#include "threads.h"
#include "test_harness.h"

#define EXPECTED_DRAINED 200   /* highest tick we expect the server to see */
/* The passive transport's delay queue only releases an entry when
 * its internal count exceeds delay_ticks (delay=1 here), so the last
 * input written always lingers behind in transport_local. Push one
 * extra to flush the EXPECTED_DRAINED-th one through. */
#define WRITES (EXPECTED_DRAINED + 1)
#define WRITER_THROTTLE_NS 50000   /* 50 microseconds */

typedef struct {
    Transport   *transport;
    ServerSim   *sim;
    SDL_AtomicInt done;
} ReaderArgs;

typedef struct {
    Transport *transport;
} WriterArgs;

static int SDLCALL writer_fn(void *opaque) {
    WriterArgs *args = (WriterArgs *)opaque;
    uint32_t i;
    for (i = 1; i <= WRITES; i++) {
        InputPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick = i;
        pkt.playerNum = 0;
        /* No buttons / no actions: warmup-shaped input that won't
         * fire shells; we only care about queue mechanics. */
        args->transport->sendInput(args->transport->ctx, &pkt);
        SDL_DelayNS(WRITER_THROTTLE_NS);
    }
    return 0;
}

static int SDLCALL reader_fn(void *opaque) {
    ReaderArgs *args = (ReaderArgs *)opaque;
    while (SDL_GetAtomicInt(&args->done) == 0) {
        /* The reader is what the post-unification timer thread is
         * modelled on: it holds threadsMutex around the tick path
         * so server-internal state mutations don't race other
         * mutex-respecting readers. The passive transport's own
         * queue is the one the test is stressing. */
        threadsWaitForMutex();
        args->transport->tick(args->transport->ctx);
        serverSimTick(args->sim);
        threadsReleaseMutex();
    }
    return 0;
}

int run_transport_local_passive_threads(void) {
    if (!threadsCreate(TRUE)) {
        UT_FAIL("threadsCreate failed");
    }

    ServerSim *sim = ut_make_running_sim("Writer");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    /* Pass NULL for cs — this test exercises the raw transport without
     * a bound ClientSim, so the per-tick snapshot-apply hook stays off. */
    Transport transport = transportLocalCreatePassive(sim, NULL, 0);
    /* delay_ticks = 1 forces inputs through the internal delay queue,
     * which is the actual structure the planned self-lock protects. */
    transportLocalSetDelay(&transport, 20);

    WriterArgs wargs;
    wargs.transport = &transport;

    ReaderArgs rargs;
    rargs.transport = &transport;
    rargs.sim = sim;
    SDL_SetAtomicInt(&rargs.done, 0);

    SDL_Thread *writer = SDL_CreateThread(writer_fn, "passive-writer", &wargs);
    SDL_Thread *reader = SDL_CreateThread(reader_fn, "passive-reader", &rargs);
    UT_ASSERT_MSG(writer != NULL && reader != NULL,
                  "SDL_CreateThread returned NULL");

    SDL_WaitThread(writer, NULL);

    /* After the writer finishes, drain whatever's left so the reader
     * has a chance to observe every input before we stop it. */
    SDL_Delay(50);
    SDL_SetAtomicInt(&rargs.done, 1);
    SDL_WaitThread(reader, NULL);

    /* Final drain on the test thread: serverSimTick consumes one
     * input from sim's queue per call, and lastProcessedInput tracks
     * the highest tick consumed. Bail out once it stabilises. */
    uint32_t last_seen = serverSimGetLastProcessedInput(sim, 0);
    uint32_t stable_iters = 0;
    int drain_iters;
    for (drain_iters = 0; drain_iters < 4000; drain_iters++) {
        threadsWaitForMutex();
        transport.tick(transport.ctx);
        serverSimTick(sim);
        threadsReleaseMutex();
        uint32_t now = serverSimGetLastProcessedInput(sim, 0);
        if (now == last_seen) {
            stable_iters++;
            if (stable_iters >= 50) break;
        } else {
            stable_iters = 0;
            last_seen = now;
        }
    }

    uint32_t observed = serverSimGetLastProcessedInput(sim, 0);
    fprintf(stderr,
            "  WRITES=%u EXPECTED_DRAINED=%u observed=%u drain_iters=%d\n",
            (unsigned)WRITES, (unsigned)EXPECTED_DRAINED,
            (unsigned)observed, drain_iters);

    transportLocalDestroy(&transport);
    serverSimDestroy(sim);
    threadsDestroy();

    /* Strong invariant: every writer-issued input below the queue's
     * residual high-water-mark is dequeued exactly once and observed
     * by the server tick, so the highest processed tick equals
     * EXPECTED_DRAINED. Without the passive transport's self-lock
     * (added by the §3.3 change), a torn write on the queue indices
     * can drop or duplicate an input, which surfaces here as
     * lastProcessedInput < EXPECTED_DRAINED — TSan should flag the
     * underlying race in the same run. */
    UT_ASSERT_MSG(observed == EXPECTED_DRAINED,
                  "EXPECTED_DRAINED=%u observed=%u — torn write or lost input",
                  (unsigned)EXPECTED_DRAINED, (unsigned)observed);

    return 0;
}
