/*
 * Backlog catch-up: bleed a standing input queue at +1 input per sub-tick.
 *
 * After a jitter spike the queue can settle at a depth above jitterTarget
 * because consumption is normally one input per sub-tick — every input
 * thereafter waits longer than it should and input latency ratchets up for
 * the session. The catch-up path dequeues and applies one *additional* fresh
 * input in the same sub-tick whenever post-dequeue depth exceeds
 * jitterTarget + 1, so a burst drains at double speed (cap: 2 applies per
 * player per sub-tick, never an unbounded drain). statCatchupTicks counts
 * each extra apply over the per-second [netstat] window.
 *
 * Drives serverSimApplyInput + serverSimTick directly on ut_make_running_sim
 * (slot 0) and reads T2 state off the ServerSim struct (the unittests profile
 * permits internal access). serverSimTick runs two half-steps per call.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* direct field access: lastProcessedInput etc. */
#include "input_packet.h"
#include "test_harness.h"

#define IC_SLOT 0

/* Enqueue one input tick for slot 0 the way an arriving packet would. */
static void ic_feed(ServerSim *sim, uint32_t tick, uint8_t buttons) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = IC_SLOT;
    pkt.buttons   = buttons;
    serverSimApplyInput(sim, &pkt);
}

/* Current queue depth for slot 0 (free-running counters, wrap-safe). */
static uint8_t ic_depth(ServerSim *sim) {
    return (uint8_t)(sim->inputQueueHead[IC_SLOT] - sim->inputQueueTail[IC_SLOT]);
}

/* Establish the stream: two fresh inputs per serverSimTick over six frames,
 * feeding ticks 1..12 so the jitter buffer fills without ever stalling.
 * Leaves lastProcessedInput == 12, jitterTarget == 2, depth == 0; returns
 * the next unused tick (13). */
static uint32_t ic_establish(ServerSim *sim, uint8_t buttons) {
    uint32_t t = 1;
    int frame;
    for (frame = 0; frame < 6; frame++) {
        ic_feed(sim, t++, buttons);
        ic_feed(sim, t++, buttons);
        serverSimTick(sim);
    }
    return t;
}

/* Backlog catch-up: steady state never catches up; a burst bleeds at +1 per
 * sub-tick (capped at 4 applies per tick) and the queue returns to target.
 *
 * The whole run stays under 100 sub-ticks (establish 12 + steady 6 + bleed
 * 10 = 28), so neither the per-second [netstat] reset (sim->tick % 100) nor a
 * jitter-buffer shrink (JITTER_SHRINK_INTERVAL == 100 stable ticks) fires
 * mid-test — jitterTarget stays 2 throughout and statCatchupTicks isn't
 * zeroed before the final assert. */
int run_input_catchup(void) {
    ServerSim *sim = ut_make_running_sim("Catchup");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    uint32_t next = ic_establish(sim, INPUT_BTN_LEFT);  /* next tick == 13 */
    UT_ASSERT_MSG(sim->inputBufferFilled[IC_SLOT], "stream not established");
    UT_ASSERT_MSG(sim->lastProcessedInput[IC_SLOT] == 12,
                  "establish left lastProcessedInput at %u, expected 12",
                  sim->lastProcessedInput[IC_SLOT]);
    UT_ASSERT_MSG(sim->jitterTarget[IC_SLOT] == 2,
                  "establish left jitterTarget at %u, expected 2",
                  sim->jitterTarget[IC_SLOT]);
    uint8_t target = sim->jitterTarget[IC_SLOT];

    /* Steady state never catches up: at 2 fresh inputs per frame the queue is
     * consumed one-per-sub-tick and never exceeds target + 1, so the depth
     * gate (depth > target + 1) never opens. */
    int frame;
    for (frame = 0; frame < 3; frame++) {
        ic_feed(sim, next++, INPUT_BTN_LEFT);
        ic_feed(sim, next++, INPUT_BTN_LEFT);
        serverSimTick(sim);
        UT_ASSERT_MSG(ic_depth(sim) <= target,
                      "steady-state depth %u exceeded target %u",
                      ic_depth(sim), target);
    }
    UT_ASSERT_MSG(sim->statCatchupTicks[IC_SLOT] == 0,
                  "steady state triggered %u catch-ups, expected 0",
                  sim->statCatchupTicks[IC_SLOT]);
    UT_ASSERT_MSG(sim->lastProcessedInput[IC_SLOT] == 18,
                  "steady state left lastProcessedInput at %u, expected 18",
                  sim->lastProcessedInput[IC_SLOT]);  /* next == 19 */

    /* Jitter-spike backlog: burst-enqueue 8 consecutive fresh ticks (19..26).
     * Depth jumps to 8; the 16-deep queue holds it. */
    int i;
    for (i = 0; i < 8; i++) {
        ic_feed(sim, next++, INPUT_BTN_LEFT);
    }
    UT_ASSERT_MSG(ic_depth(sim) == 8, "burst left depth %u, expected 8",
                  ic_depth(sim));

    /* Bleed: continue 2-fresh-per-frame supply. With catch-up, a sub-tick
     * applies a second fresh input whenever post-dequeue depth > target + 1,
     * so a deeply-backlogged frame advances lastProcessedInput by 4 (2 normal
     * + 2 catch-up) — and never by more than 4, the structural cap (two
     * sub-ticks, one extra apply each). A frame advances by exactly 4 only
     * while the queue is deep enough for both sub-ticks to catch up; both
     * dequeues happen post-feed, so that needs depth-after-feed >= 7
     * (= 2*target + 3). As the backlog bleeds off, the per-frame advance
     * tapers (4, 4, 3, 3, 2) back to the steady 2.
     *
     * Five bleed frames (10 sub-ticks) drain the burst. Conservation gives
     * the catch-up total directly: inputs consumed = startBurstDepth(8) +
     * fed-during-bleed(10) - endDepth(2) = 16; one normal apply per sub-tick
     * = 10; so extra catch-up applies = 16 - 10 = 6. */
    uint32_t catchupBefore = sim->statCatchupTicks[IC_SLOT];
    for (frame = 0; frame < 5; frame++) {
        uint32_t lpiBefore = sim->lastProcessedInput[IC_SLOT];
        ic_feed(sim, next++, INPUT_BTN_LEFT);
        ic_feed(sim, next++, INPUT_BTN_LEFT);
        uint8_t depthAfterFeed = ic_depth(sim);
        serverSimTick(sim);
        uint32_t delta = sim->lastProcessedInput[IC_SLOT] - lpiBefore;

        UT_ASSERT_MSG(delta <= 4,
                      "frame %d advanced lastProcessedInput by %u — exceeds "
                      "the 2-applies-per-sub-tick cap of 4", frame, delta);
        if (depthAfterFeed >= (uint8_t)(2 * target + 3)) {
            UT_ASSERT_MSG(delta == 4,
                          "deeply-backlogged frame %d (depth %u) advanced by "
                          "%u, expected exactly 4 (+1 catch-up per sub-tick)",
                          frame, depthAfterFeed, delta);
        }
    }

    /* Depth returns to target: the burst has bled off and the queue sits back
     * at the steady jitter depth, so lastProcessedInput is caught up to within
     * the buffer of the newest fed tick. */
    UT_ASSERT_MSG(ic_depth(sim) <= (uint8_t)(target + 1),
                  "post-bleed depth %u still above target + 1 (%u)",
                  ic_depth(sim), (uint8_t)(target + 1));
    uint32_t newestFed = next - 1;
    UT_ASSERT_MSG(newestFed - sim->lastProcessedInput[IC_SLOT]
                      <= (uint32_t)(target + 1),
                  "lastProcessedInput %u did not catch up to newest fed tick "
                  "%u within the buffer", sim->lastProcessedInput[IC_SLOT],
                  newestFed);

    /* Exactly six extra applies bled the 8-deep burst (derivation above). */
    UT_ASSERT_MSG(sim->statCatchupTicks[IC_SLOT] - catchupBefore == 6,
                  "expected 6 catch-up applies, got %u",
                  sim->statCatchupTicks[IC_SLOT] - catchupBefore);

    serverSimDestroy(sim);
    return 0;
}

/* Catch-up gates on the fresh backlog (newest queued tick minus
 * lastProcessedInput), not raw head-tail depth. Input redundancy resends
 * each tick in several packets and intake does not dedup unprocessed ticks,
 * so the same fresh tick sits in the queue multiple times — raw depth is
 * inflated but the real backlog is small. The catch-up gate must not fire
 * on that, or a packet's two fresh inputs get applied in one half-step and
 * the server's angle jumps ahead of the client's prediction.
 *
 * The fresh-backlog helper is file-static and unreachable from here, so we
 * assert the observable consequence (statCatchupTicks) rather than the
 * helper directly. */
int run_catchup_ignores_redundant_duplicates(void) {
    ServerSim *sim = ut_make_running_sim("Dupes");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    ic_establish(sim, INPUT_BTN_LEFT);  /* lastProcessedInput == 12 */
    UT_ASSERT_MSG(sim->lastProcessedInput[IC_SLOT] == 12,
                  "establish left lastProcessedInput at %u, expected 12",
                  sim->lastProcessedInput[IC_SLOT]);
    UT_ASSERT_MSG(sim->jitterTarget[IC_SLOT] == 2,
                  "establish left jitterTarget at %u, expected 2",
                  sim->jitterTarget[IC_SLOT]);
    uint8_t target = sim->jitterTarget[IC_SLOT];

    /* Redundancy: the same two fresh ticks (13, 14) resent across four
     * packets each. Intake does not dedup unprocessed ticks, so all eight
     * copies enqueue — raw depth climbs to 8 while only two ticks (13, 14)
     * are genuinely fresh, so the fresh backlog is just 2. */
    int r;
    for (r = 0; r < 4; r++) {
        ic_feed(sim, 13, INPUT_BTN_LEFT);
        ic_feed(sim, 14, INPUT_BTN_LEFT);
    }

    /* Precondition: raw depth is inflated past the gate the old code used —
     * the duplicate condition the bug tripped on is present. */
    UT_ASSERT_MSG(ic_depth(sim) > (uint8_t)(target + 1),
                  "duplicate burst did not inflate raw depth (%u <= %u) — "
                  "test precondition not met", ic_depth(sim),
                  (uint8_t)(target + 1));

    /* Two frames process 13 and 14 and drop their six redundant copies as
     * stale. Catch-up gates on fresh backlog (max 2 <= target + 1 = 3), so
     * it must NOT fire despite the inflated raw depth. */
    uint32_t staleBefore = sim->statDroppedStaleInputs[IC_SLOT];
    serverSimTick(sim);  /* applies 13 then 14 */
    serverSimTick(sim);  /* drops the six stale duplicates */
    UT_ASSERT_MSG(sim->statCatchupTicks[IC_SLOT] == 0,
                  "catch-up fired %u times on a queue inflated only by "
                  "redundant duplicates", sim->statCatchupTicks[IC_SLOT]);
    UT_ASSERT_MSG(sim->lastProcessedInput[IC_SLOT] == 14,
                  "both fresh ticks not processed once (lpi %u, expected 14)",
                  sim->lastProcessedInput[IC_SLOT]);
    UT_ASSERT_MSG(sim->statDroppedStaleInputs[IC_SLOT] > staleBefore,
                  "redundant duplicates were not dropped as stale");

    /* Positive control: a genuine multi-tick fresh backlog still fires
     * catch-up. Burst-enqueue eight distinct fresh ticks (15..22) — the
     * fresh backlog is 8 > target + 1 — and drain. */
    uint32_t catchupBefore = sim->statCatchupTicks[IC_SLOT];
    uint32_t next = sim->lastProcessedInput[IC_SLOT] + 1;  /* 15 */
    int i;
    for (i = 0; i < 8; i++) {
        ic_feed(sim, next++, INPUT_BTN_LEFT);
    }
    int f;
    for (f = 0; f < 3; f++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(sim->statCatchupTicks[IC_SLOT] > catchupBefore,
                  "catch-up did not fire on a genuine 8-tick fresh backlog");

    serverSimDestroy(sim);
    return 0;
}
