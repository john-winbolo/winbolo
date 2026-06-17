/*
 * Adaptive input jitter buffer: grow on starvation, shrink on a calm link.
 *
 * Under network jitter the per-player input queue repeatedly empties when an
 * input was expected. Each such drain is the reliable "buffer too shallow"
 * signal, so the server deepens jitterTarget off drain events (not just off
 * consecutive in-filled stalls, which the re-fill `continue` skips past). A
 * calm link with a steadily full queue then shrinks the target back toward
 * JITTER_BUFFER_MIN after the stable interval, so there is no permanent
 * latency creep. The target never exceeds JITTER_BUFFER_MAX.
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
#include "server_sim_internal.h"   /* direct field access: jitterTarget etc. */
#include "input_packet.h"
#include "test_harness.h"

#define JBG_SLOT 0

/* Enqueue one input tick for slot 0 the way an arriving packet would. */
static void jbg_feed(ServerSim *sim, uint32_t tick, uint8_t buttons) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = JBG_SLOT;
    pkt.buttons   = buttons;
    serverSimApplyInput(sim, &pkt);
}

/* Establish the stream: two fresh inputs per serverSimTick over six frames,
 * feeding ticks 1..12 so the jitter buffer fills without ever stalling.
 * Leaves lastProcessedInput == 12, jitterTarget == JITTER_BUFFER_DEFAULT,
 * depth == 0; returns the next unused tick (13). */
static uint32_t jbg_establish(ServerSim *sim, uint8_t buttons) {
    uint32_t t = 1;
    int frame;
    for (frame = 0; frame < 6; frame++) {
        jbg_feed(sim, t++, buttons);
        jbg_feed(sim, t++, buttons);
        serverSimTick(sim);
    }
    return t;
}

/* Enqueue one input on a strictly increasing tick that is always ahead of the
 * server's lastProcessedInput. A drain triggers the stall-advance path, which
 * synthesises inputs and advances lastProcessedInput during the dry spell;
 * without this guard a free-running counter falls behind it and the next fed
 * input lands stale (dropped, never a fresh dequeue — so stable ticks never
 * accumulate and the buffer can't shrink). */
static void jbg_feed_fresh(ServerSim *sim, uint32_t *next, uint8_t buttons) {
    uint32_t lpi = sim->lastProcessedInput[JBG_SLOT];
    if (*next <= lpi) {
        *next = lpi + 1;
    }
    jbg_feed(sim, (*next)++, buttons);
}

/* One jitter-induced drain cycle: top the queue up to the current target
 * depth, then run enough half-steps to consume it completely. The queue
 * emptying with no fresh input is exactly one drain event. Feeding exactly
 * `target` keeps the fresh backlog at/below target + 1 so backlog catch-up
 * never fires and consumption stays one input per half-step. */
static void jbg_starve_cycle(ServerSim *sim, uint32_t *next, uint8_t buttons) {
    uint8_t target = sim->jitterTarget[JBG_SLOT];
    int i;
    for (i = 0; i < target; i++) {
        jbg_feed_fresh(sim, next, buttons);
    }
    /* target + 2 half-steps drains `target` inputs with margin. Once drained
     * the buffer re-enters filling mode and later empty half-steps take the
     * depth `continue`, so this registers exactly one drain — never more. */
    int t;
    for (t = 0; t < target + 2; t++) {
        serverSimTick(sim);
    }
}

/* Jitter grows the buffer; a calm link shrinks it back; it never exceeds MAX. */
int run_jitter_buffer_grow(void) {
    ServerSim *sim = ut_make_running_sim("Jitter");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    uint32_t next = jbg_establish(sim, INPUT_BTN_LEFT);  /* next tick == 13 */
    UT_ASSERT_MSG(sim->inputBufferFilled[JBG_SLOT], "stream not established");
    UT_ASSERT_MSG(sim->jitterTarget[JBG_SLOT] == JITTER_BUFFER_DEFAULT,
                  "establish left jitterTarget at %u, expected default %u",
                  sim->jitterTarget[JBG_SLOT], JITTER_BUFFER_DEFAULT);

    /* (a) Sustained jitter-induced drains deepen the buffer toward MAX. Each
     * cycle is one drain; the grow threshold is two drains, so a handful of
     * cycles climbs the target. Twelve cycles is comfortably past the eight
     * needed to walk DEFAULT(2) -> MAX(6). (c) is checked every cycle: the
     * target must never exceed MAX. */
    int cycle;
    for (cycle = 0; cycle < 12; cycle++) {
        jbg_starve_cycle(sim, &next, INPUT_BTN_LEFT);
        UT_ASSERT_MSG(sim->jitterTarget[JBG_SLOT] <= JITTER_BUFFER_MAX,
                      "jitterTarget %u exceeded MAX %u after cycle %d",
                      sim->jitterTarget[JBG_SLOT], JITTER_BUFFER_MAX, cycle);
    }
    UT_ASSERT_MSG(sim->jitterTarget[JBG_SLOT] > JITTER_BUFFER_DEFAULT,
                  "sustained jitter did not grow jitterTarget above default "
                  "(still %u)", sim->jitterTarget[JBG_SLOT]);
    UT_ASSERT_MSG(sim->jitterTarget[JBG_SLOT] == JITTER_BUFFER_MAX,
                  "sustained jitter did not drive jitterTarget to MAX %u "
                  "(reached %u)", JITTER_BUFFER_MAX, sim->jitterTarget[JBG_SLOT]);

    /* (b) A calm link with a steadily full queue shrinks the target back to
     * MIN. First re-prime with a full burst of fresh inputs so the very next
     * dequeue is fresh: that resets the dry-spell counter the grow drains left
     * high and stops the stall-advance path before it can race
     * lastProcessedInput past the feed. */
    int k;
    for (k = 0; k < JITTER_BUFFER_MAX; k++) {
        jbg_feed_fresh(sim, &next, INPUT_BTN_LEFT);
    }

    /* Then feed two fresh inputs per frame and never let the queue drain: each
     * successful dequeue is a stable tick, and JITTER_SHRINK_INTERVAL stable
     * ticks drop the target by one. Loop until it settles at MIN (the cap is
     * generous: ~50 frames per shrink, MAX->MIN is five shrinks). */
    int f;
    for (f = 0; f < 400 && sim->jitterTarget[JBG_SLOT] > JITTER_BUFFER_MIN; f++) {
        jbg_feed_fresh(sim, &next, INPUT_BTN_LEFT);
        jbg_feed_fresh(sim, &next, INPUT_BTN_LEFT);
        serverSimTick(sim);
        UT_ASSERT_MSG(sim->jitterTarget[JBG_SLOT] <= JITTER_BUFFER_MAX,
                      "jitterTarget %u exceeded MAX %u during shrink",
                      sim->jitterTarget[JBG_SLOT], JITTER_BUFFER_MAX);
    }
    UT_ASSERT_MSG(sim->jitterTarget[JBG_SLOT] == JITTER_BUFFER_MIN,
                  "calm link did not shrink jitterTarget back to MIN %u "
                  "(stuck at %u)", JITTER_BUFFER_MIN, sim->jitterTarget[JBG_SLOT]);

    serverSimDestroy(sim);
    return 0;
}
