/*
 * Regression: the spectator ring must record on every game tick whenever a
 * ring is registered, independent of whether a .wbv log is being written.
 *
 * The real-sim cases in test_spectator_ring.c bring the world up with logStart
 * (sr_bringup), which is exactly what hid the production bug: on a normal server
 * nobody calls logStart, so logWriteTick used to early-out and the ring stayed
 * empty — a connecting spectator never got a seed and hung at "connecting".
 *
 * This test stands the sim up the way a real (non-recording) server does:
 * register a ring via logSetSpectatorRing and drive serverSimTick (the path that
 * calls logWriteTick) WITHOUT ever calling logStart and without opening a .wbv
 * file. It then asserts the ring populated and a delay-0 seek returns a seed —
 * the keyframe a connecting spectator needs — all while logIsRecording() proves
 * the .wbv writer was off the whole time.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"          /* MAX_TANKS */
#include "server_sim.h"      /* serverSimTick / ApplyInput / GetTick / Destroy */
#include "log.h"             /* logCreate, logDestroy, logIsRecording */
#include "log_internal.h"    /* logSetSpectatorRing */
#include "spectator_ring.h"  /* spectatorRingCreate / SeekDelayed / CursorKeyframe */
#include "test_harness.h"

#define SRN_SLOT     0
#define SRN_CADENCE  8        /* keyframe every 8 game ticks within a segment */

/* One zero-button input frame: keeps the input stream established while
   commanding no motion, matching the cadence the other ring tests use. */
static void srn_tick_idle(ServerSim *sim, uint32_t *it) {
    InputPacket pkt;
    int n;
    for (n = 0; n < 2; n++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick      = (*it)++;
        pkt.playerNum = SRN_SLOT;
        pkt.buttons   = 0;
        serverSimApplyInput(sim, &pkt);
    }
    serverSimTick(sim);
}

int run_spectator_ring_nolog(void) {
    ServerSim *sim;
    SpectatorRing *ring;
    SpectatorRingCursor cur;
    const uint8_t *seed;
    int seedLen;
    uint32_t head0, head1;
    uint32_t it = 1;
    int i;

    sim = ut_make_running_sim("NoLogSpec");
    UT_ASSERT_MSG(sim != NULL, "running sim bringup failed");

    /* Initialise the log subsystem but deliberately DO NOT call logStart: no
       .wbv file is opened (logStart is the only thing that zipOpens one), and
       the .wbv writer stays off for the whole test. */
    logCreate();
    UT_ASSERT_MSG(logIsRecording() == FALSE,
                  "no .wbv recording should be active without logStart");

    /* Register the ring the way a normal server does at instance startup. */
    ring = spectatorRingCreate(SRN_CADENCE, 100000);
    UT_ASSERT(ring != NULL);
    logSetSpectatorRing(ring, sim);

    /* Drive normal game ticks. Each serverSimTick calls logWriteTick, which must
       now record one ring tick even though no .wbv log is running. The first
       record opens a segment with a keyframe. */
    srn_tick_idle(sim, &it);
    UT_ASSERT_MSG(spectatorRingSegmentCount(ring) >= 1,
                  "ring recorded no segment under live ticking (no logStart)");
    head0 = spectatorRingHeadSeq(ring);

    for (i = 0; i < 8; i++) {
        srn_tick_idle(sim, &it);
    }
    head1 = spectatorRingHeadSeq(ring);
    UT_ASSERT_MSG(head1 > head0,
                  "ring head did not advance across ticks (head %u -> %u)",
                  (unsigned)head0, (unsigned)head1);

    /* The .wbv writer was off the entire time. */
    UT_ASSERT_MSG(logIsRecording() == FALSE,
                  "logStart must not have been triggered by the ring path");

    /* A delay-0 seek returns a seed: this is exactly what a connecting spectator
       at specdelay 0 needs to start its view. The seed is a non-empty keyframe
       blob (the [u32 bodyLen][body][u32 ctrlLen][ctrl] keyframe layout). */
    UT_ASSERT_MSG(spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK,
                  "delay-0 seek found no seed in a ring-only (no .wbv) recording");
    seed = spectatorRingCursorKeyframe(&cur, &seedLen, NULL);
    UT_ASSERT_MSG(seed != NULL && seedLen > 0,
                  "delay-0 seed keyframe is empty (len %d)", seedLen);

    logSetSpectatorRing(NULL, NULL);
    spectatorRingDestroy(ring);
    logDestroy();
    serverSimDestroy(sim);
    return 0;
}

/* The number of log_ServerTick records in one ring event payload: a run of
   [type][u16 big-endian length][payload] frames. */
static int srn_count_anchors(const uint8_t *p, int len) {
    int off   = 0;
    int found = 0;

    while (off + 3 <= len) {
        int evLen = ((int)p[off + 1] << 8) | (int)p[off + 2];
        if (p[off] == log_ServerTick) {
            found++;
        }
        off += 3 + evLen;
    }
    return found;
}

/* A server that is not recording still gives a live spectator a
   log_ServerTick every FULL_SYNC_INTERVAL ticks. With no .wbv open no snapshot
   is ever noted, so before the interval test the feed carried the round's
   first anchor and nothing after it. The cadence is set past the end of the
   run so the first record is the ring's only keyframe, and no interval's
   events are replaced by one. */
int run_spectator_ring_nolog_tick_anchors(void) {
    ServerSim     *sim;
    SpectatorRing *ring;
    uint32_t       it = 1;
    uint32_t       seq;
    uint32_t       head;
    int            anchors = 0;
    int            i;

    sim = ut_make_running_sim("NoLogAnchors");
    UT_ASSERT_MSG(sim != NULL, "running sim bringup failed");
    logCreate();
    UT_ASSERT_MSG(logIsRecording() == FALSE,
                  "no .wbv recording should be active without logStart");
    ring = spectatorRingCreate(100000, 100000);
    UT_ASSERT(ring != NULL);
    logSetSpectatorRing(ring, sim);

    /* Two game ticks a call, so this passes four intervals. */
    for (i = 0; i < 600; i++) {
        srn_tick_idle(sim, &it);
    }

    head = spectatorRingHeadSeq(ring);
    for (seq = spectatorRingOldestSeq(ring); seq <= head; seq++) {
        bool           key = false;
        const uint8_t *payload = NULL;
        int            len = 0;
        if (spectatorRingRecordAt(ring, seq, &key, &payload, &len, NULL,
                                  NULL) &&
            !key && payload != NULL) {
            anchors += srn_count_anchors(payload, len);
        }
    }

    logSetSpectatorRing(NULL, NULL);
    spectatorRingDestroy(ring);
    logDestroy();
    serverSimDestroy(sim);

    UT_ASSERT_MSG(anchors >= 3,
                  "a feed from a server that is not recording carried %d "
                  "log_ServerTick records over four sync intervals (want at "
                  "least 3)", anchors);
    return 0;
}
