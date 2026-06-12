/*
 * Exact lag-compensation viewTick (test_viewtick_rewind.c).
 *
 * Two surfaces:
 *
 *   1. serverSimComputeLagCompTicks — the pure rewind math. When the client
 *      stamps a known viewTick, the server rewinds the real view age
 *      (simTick - viewTick) server ticks / 2 = posHistory entries; viewTick==0
 *      or a viewTick ahead of the server falls back to the old ping estimate;
 *      and the result clamps to LAG_COMP_MAX_TICKS.
 *
 *   2. clientSimGetViewTick — the client stamps the *displayed* frame, which is
 *      the second-newest applied snapshot's serverTick, and reports 0 until two
 *      snapshots have been applied.
 *
 * Always built — the math is sim-independent and the client case drives a bare
 * ClientSim with empty snapshots (no transport).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"     /* serverSimComputeLagCompTicks, LAG_COMP_MAX_TICKS */
#include "interpolation.h"           /* INTERP_BUFFER_MS */
#include "client_sim.h"
#include "client_net.h"              /* clientSimGetViewTick */
#include "input_packet.h"
#include "test_harness.h"

/* The pre-viewTick ping-based estimate, recomputed here so the fallback
 * assertions pin the helper to the exact formula it replaced. */
static uint8_t ping_fallback(uint16_t pingMs) {
    uint32_t delayMs = ((uint32_t)pingMs / 2) + INTERP_BUFFER_MS;
    return (uint8_t)(delayMs / 20);
}

int run_viewtick_rewind(void) {
    /* viewTick a known age behind simTick → (simTick - viewTick) / 2 history
     * entries, independent of ping. 6 ticks behind → 3. */
    UT_ASSERT(serverSimComputeLagCompTicks(100, 94, 0) == 3);
    UT_ASSERT(serverSimComputeLagCompTicks(100, 94, 9999) == 3);

    /* viewTick == 0 → ping fallback equals the old formula. */
    UT_ASSERT(serverSimComputeLagCompTicks(100, 0, 100) == ping_fallback(100));
    UT_ASSERT(serverSimComputeLagCompTicks(100, 0, 200) == ping_fallback(200));
    UT_ASSERT(ping_fallback(100) == 3);   /* (50+20)/20 */
    UT_ASSERT(ping_fallback(200) == 6);   /* (100+20)/20 */

    /* viewTick ahead of the server (stale/garbage) → ping fallback, never a
     * huge or negative rewind. */
    UT_ASSERT(serverSimComputeLagCompTicks(50, 100, 100) == ping_fallback(100));

    /* A large real view age clamps to the cap. */
    UT_ASSERT(serverSimComputeLagCompTicks(1000, 2, 0) == LAG_COMP_MAX_TICKS);
    UT_ASSERT(LAG_COMP_MAX_TICKS == 14);

    return 0;
}

/* Apply an otherwise-empty snapshot carrying just a header serverTick. */
static void apply_empty_snapshot(ClientSim *cs, uint32_t serverTick) {
    SnapshotHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.serverTick = serverTick;
    clientSimSyncFromSnapshot(cs, &hdr,
                              NULL, 0,   /* tanks */
                              NULL, 0,   /* shells */
                              NULL, 0,   /* tk explosions */
                              NULL, 0,   /* bases */
                              NULL, 0,   /* pills */
                              NULL, 0,   /* events */
                              0);        /* playerNum */
}

int run_viewtick_displayed_tick(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    clientSimCreate(cs);

    /* Before any snapshot: unknown. */
    UT_ASSERT_MSG(clientSimGetViewTick(cs) == 0,
                  "viewTick should be 0 before any snapshot, got %u",
                  (unsigned)clientSimGetViewTick(cs));

    /* One applied snapshot: still 0 (need two to know the displayed frame). */
    apply_empty_snapshot(cs, 1000);
    UT_ASSERT_MSG(clientSimGetViewTick(cs) == 0,
                  "viewTick should be 0 after one snapshot, got %u",
                  (unsigned)clientSimGetViewTick(cs));

    /* Second applied snapshot: the displayed frame is the second-newest =
     * the first snapshot's serverTick. */
    apply_empty_snapshot(cs, 1002);
    UT_ASSERT_MSG(clientSimGetViewTick(cs) == 1000,
                  "viewTick should be the second-newest applied tick (1000), got %u",
                  (unsigned)clientSimGetViewTick(cs));

    /* A third keeps tracking the second-newest. */
    apply_empty_snapshot(cs, 1004);
    UT_ASSERT_MSG(clientSimGetViewTick(cs) == 1002,
                  "viewTick should advance to 1002, got %u",
                  (unsigned)clientSimGetViewTick(cs));

    clientSimDestroy(cs);
    return 0;
}
