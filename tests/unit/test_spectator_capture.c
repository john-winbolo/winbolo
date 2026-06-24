/*
 * Spectator feed capture test — client side, end-to-end over loopback.
 *
 * Drives the real client transport connected as a tankless spectator (4.1)
 * against a real server whose spectator ring is recording, and proves the
 * client captures the feed: the BULK_KIND_SPEC_SEED blob and the ordered
 * BULK_KIND_SPEC_RECORD blobs that follow it land in the ClientSim spectator
 * feed, with the 9-byte record transport header stripped and arrival order
 * preserved. Drained via the clientSimSpectator* accessors.
 *
 * This also exercises the PACKET_CHANNEL emitter gate fix end-to-end: the seed
 * streams on CHANNEL_BULK, whose window only advances as the client acks. A
 * spectator that doesn't emit its acks would stall the transfer one window in
 * and never complete the seed — so clientSimSpectatorSeedReady becoming true is
 * itself proof the spectator now acks the bulk stream.
 *
 * Recording is added on top of the loopback server the way a real MP host gets
 * it: logStart (so logWriteTick feeds the ring) plus serverInstanceCreate-
 * SpectatorRing once the spectator cap is non-zero. specDelayTicks stays at its
 * default 0, so the seed targets the head keyframe and arms as soon as the ring
 * holds one.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAX_TANKS */
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "server_lifecycle.h"  /* serverInstanceCreateSpectatorRing, GetSpectatorRing */
#include "spectator_ring.h"    /* spectatorRingHeadSeq */
#include "log.h"               /* logCreate, logStart, logStop, logDestroy */
#include "test_harness.h"
#include "loopback_harness.h"

/* The handshake + a recorded keyframe + the seed transfer all complete well
 * inside this bound on a clean loopback path. */
#define SC_SEED_MAX     800
#define SC_RECORD_MAX   400
#define SC_MIN_RECORDS  2

static bool pred_seed_ready(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimSpectatorSeedReady(h->cs);
}

static bool pred_records(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimSpectatorRecordCount(h->cs) >= SC_MIN_RECORDS;
}

int run_spectator_capture(void) {
    LoopbackHarness h;
    const char *fname = "test_spectator_capture.wbv";
    SpectatorRing *ring;
    uint8_t *seed = NULL;
    uint32_t seedLen = 0;
    int seedAt, recAt, popped;
    uint32_t prevTick;

    UT_ASSERT_MSG(loopbackHarnessStartSpectator(&h, "SpecCap", /*seed*/ 1u),
                  "spectator harness start failed");

    /* Stand up a recording ring on top of the running server (maxSpectators is
     * already opened by the spectator-start helper). */
    remove(fname);
    logCreate();
    UT_ASSERT_MSG(logStart(fname, h.sim, 0, MAX_TANKS, FALSE) == TRUE,
                  "logStart failed");
    serverInstanceCreateSpectatorRing(h.sim);
    ring = serverInstanceGetSpectatorRing();
    UT_ASSERT_MSG(ring != NULL, "ring not created for maxSpectators > 0");

    /* Pump until the seed is fully received and captured. If the emitter gate
     * fix were missing, the bulk window would stall and this would never hold. */
    seedAt = loopbackHarnessPumpUntil(&h, SC_SEED_MAX, pred_seed_ready, NULL);
    fprintf(stderr, "  spectator capture: seed ready after %d pump(s) (cap %d)\n",
            seedAt, SC_SEED_MAX);
    if (seedAt < 0) {
        logStop();
        logDestroy();
        loopbackHarnessStop(&h);
        remove(fname);
        UT_FAIL("spectator seed never captured within %d pumps", SC_SEED_MAX);
    }

    /* The spectator must still be in SPECTATING (never promoted to a player). */
    UT_ASSERT_MSG(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_SPECTATING,
                  "spectator left SPECTATING during capture");

    /* Take the seed: a non-empty blob, ownership handed to us. */
    UT_ASSERT_MSG(clientSimSpectatorTakeSeed(h.cs, &seed, &seedLen),
                  "seed ready but take returned nothing");
    UT_ASSERT_MSG(seed != NULL && seedLen > 0, "captured seed blob is empty");
    free(seed);
    /* Taking it clears the feed's seed. */
    UT_ASSERT_MSG(!clientSimSpectatorSeedReady(h.cs),
                  "seed still marked ready after take");

    /* Pump on until forward records accumulate (the ring keeps recording, so
     * the feed streams records after the seed completes). */
    recAt = loopbackHarnessPumpUntil(&h, SC_RECORD_MAX, pred_records, NULL);
    fprintf(stderr, "  spectator capture: %u record(s) after %d pump(s) (cap %d)\n",
            (unsigned)clientSimSpectatorRecordCount(h.cs), recAt, SC_RECORD_MAX);
    if (recAt < 0) {
        logStop();
        logDestroy();
        loopbackHarnessStop(&h);
        remove(fname);
        UT_FAIL("fewer than %d forward records captured within %d pumps",
                SC_MIN_RECORDS, SC_RECORD_MAX);
    }

    /* Drain the records: arrival (ring-seq) order is preserved, so gameTick is
     * non-decreasing and advances overall; the header was stripped (a sane,
     * increasing gameTick can only come from parsing the 9-byte header off). */
    popped = 0;
    prevTick = 0;
    {
        ClientSpectatorRecord rec;
        uint32_t firstTick = 0, lastTick = 0;
        while (clientSimSpectatorPopRecord(h.cs, &rec)) {
            UT_ASSERT_MSG(rec.gameTick >= prevTick,
                          "records out of order: gameTick %u after %u",
                          (unsigned)rec.gameTick, (unsigned)prevTick);
            if (popped == 0) firstTick = rec.gameTick;
            lastTick = rec.gameTick;
            prevTick = rec.gameTick;
            free(rec.payload);   /* ownership passed to us */
            popped++;
        }
        UT_ASSERT_MSG(popped >= SC_MIN_RECORDS,
                      "drained %d records, expected >= %d", popped, SC_MIN_RECORDS);
        UT_ASSERT_MSG(lastTick > firstTick,
                      "record gameTick did not advance (first=%u last=%u)",
                      (unsigned)firstTick, (unsigned)lastTick);
        /* Queue is empty after draining. */
        UT_ASSERT_MSG(clientSimSpectatorRecordCount(h.cs) == 0,
                      "record count nonzero after full drain");
    }

    logStop();
    logDestroy();
    loopbackHarnessStop(&h);
    remove(fname);
    return 0;
}
