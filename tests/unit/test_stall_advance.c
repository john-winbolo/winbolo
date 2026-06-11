/*
 * Stall-advance: a stall-substituted tick is a *processed* tick.
 *
 * When a player's input queue runs dry mid-stream the server substitutes
 * the held buttons AND advances lastProcessedInput past the substituted
 * tick. The real (late) input for that tick then arrives stale and its
 * movement is dropped instead of double-executing the held turn (the
 * gunsight-overshoot fix). One-shot actions (mine/build) commanded during
 * a stall are never executed by the substitute, so they are harvested off
 * the dropped stale entry under a lastActionAppliedTick invariant and laid
 * exactly once on the next real input — never zero, never twice.
 *
 * These cases drive serverSimApplyInput + serverSimTick directly on
 * ut_make_running_sim (slot 0) and read T2 state off the ServerSim struct
 * (the unittests profile permits internal access). serverSimTick runs two
 * half-steps per call, so each empty frame substitutes two ticks.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* direct field access: lastProcessedInput etc. */
#include "game_sim.h"              /* GameSim.tanks[] */
#include "tank.h"                  /* tankSetMines / tankGetMines */
#include "input_packet.h"
#include "test_harness.h"

#define SA_SLOT 0

/* Enqueue one input tick for slot 0 the way an arriving packet would. */
static void sa_feed(ServerSim *sim, uint32_t tick, uint8_t buttons,
                    uint8_t actions, uint8_t buildAction,
                    uint8_t bx, uint8_t by) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick        = tick;
    pkt.playerNum   = SA_SLOT;
    pkt.buttons     = buttons;
    pkt.actions     = actions;
    pkt.buildAction = buildAction;
    pkt.buildX      = bx;
    pkt.buildY      = by;
    serverSimApplyInput(sim, &pkt);
}

/* Establish the stream: feed two fresh inputs per serverSimTick (the
 * redundancy test's cadence) so the queue never stalls and the jitter
 * buffer fills. After this, inputBufferFilled[0] is set and
 * lastProcessedInput[0] has advanced past 0. Feeds ticks 1..12, leaving
 * lastProcessedInput[0] == 12; returns the next unused tick (13). */
static uint32_t sa_establish(ServerSim *sim, uint8_t buttons, uint8_t actions) {
    uint32_t t = 1;
    int frame;
    for (frame = 0; frame < 6; frame++) {
        sa_feed(sim, t++, buttons, actions, 0, 0, 0);
        sa_feed(sim, t++, buttons, actions, 0, 0, 0);
        serverSimTick(sim);
    }
    return t;
}

/* 1. A stall-substituted tick advances lastProcessedInput, and the late
 *    input for that tick then drops as stale. */
int run_stall_advances_processed_tick(void) {
    ServerSim *sim = ut_make_running_sim("Staller");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    sa_establish(sim, INPUT_BTN_LEFT, 0);
    UT_ASSERT_MSG(sim->inputBufferFilled[SA_SLOT], "stream not established");
    UT_ASSERT_MSG(sim->lastProcessedInput[SA_SLOT] > 0, "lastProcessedInput still 0");

    /* Empty frames: each serverSimTick is two half-steps, each of which
     * substitutes one held tick, so lastProcessedInput climbs by 2. */
    uint32_t before = sim->lastProcessedInput[SA_SLOT];
    int n = 5, i;
    for (i = 0; i < n; i++) {
        uint32_t pre = sim->lastProcessedInput[SA_SLOT];
        serverSimTick(sim);
        UT_ASSERT_MSG(sim->lastProcessedInput[SA_SLOT] == pre + 2,
                      "empty frame advanced lastProcessedInput by %u, expected 2",
                      sim->lastProcessedInput[SA_SLOT] - pre);
    }
    uint32_t afterStall = sim->lastProcessedInput[SA_SLOT];
    UT_ASSERT(afterStall == before + (uint32_t)(2 * n));

    /* Deliver the late inputs for exactly the substituted tick numbers.
     * They are all <= lastProcessedInput, so they drop as stale and never
     * move lastProcessedInput forward (only the two substitutes of the
     * draining frame do). */
    uint32_t k, delivered = 0;
    for (k = before + 1; k <= afterStall; k++) {
        sa_feed(sim, k, INPUT_BTN_LEFT, 0, 0, 0, 0);
        delivered++;
    }
    uint32_t staleBefore = sim->statDroppedStaleInputs[SA_SLOT];
    uint32_t lpBefore = sim->lastProcessedInput[SA_SLOT];
    serverSimTick(sim);  /* drains all stale entries in the first half-step */
    UT_ASSERT_MSG(sim->statDroppedStaleInputs[SA_SLOT] == staleBefore + delivered,
                  "expected %u stale drops, got %u", delivered,
                  sim->statDroppedStaleInputs[SA_SLOT] - staleBefore);
    UT_ASSERT_MSG(sim->lastProcessedInput[SA_SLOT] == lpBefore + 2,
                  "late stale inputs must not advance lastProcessedInput "
                  "beyond the two substitutes (lp moved by %u)",
                  sim->lastProcessedInput[SA_SLOT] - lpBefore);

    serverSimDestroy(sim);
    return 0;
}

/* 2. A mine commanded during a stall, dropped, then delivered late, is
 *    harvested and laid exactly once on the next real input. */
int run_stall_mine_late_lays_once(void) {
    ServerSim *sim = ut_make_running_sim("MineLate");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL && gs->tanks[SA_SLOT] != NULL);
    tankSetMines(&gs->tanks[SA_SLOT], 40);

    sa_establish(sim, INPUT_BTN_LEFT, 0);  /* lastProcessedInput = 12 */

    /* The withheld game (even) tick that carried LAY_MINE. */
    uint32_t mineTick = 14;

    /* Stall window substitutes past mineTick without ever laying it. */
    serverSimTick(sim);  /* 12 -> 14 */
    serverSimTick(sim);  /* 14 -> 16 */
    UT_ASSERT_MSG(sim->lastProcessedInput[SA_SLOT] >= mineTick,
                  "stall did not advance past the mine tick");
    UT_ASSERT_MSG(sim->lastActionAppliedTick[SA_SLOT] == 0,
                  "substitutes must not touch the action marker");
    UT_ASSERT_MSG(sim->pendingHarvestActions[SA_SLOT] == 0,
                  "no pending action before the late mine arrives");

    /* Deliver the withheld ticks late as a burst (mineTick-1 then mineTick,
     * mirroring a redundancy retransmit). A lone stale input would not be
     * dequeued while the buffer is re-filling (depth < jitterTarget), so a
     * 2-input burst opens the gate; both drain as stale, the mine on the
     * higher tick is harvested last, leaving the marker on the mine tick. */
    sa_feed(sim, mineTick - 1, INPUT_BTN_LEFT, 0, 0, 0, 0);
    sa_feed(sim, mineTick, INPUT_BTN_LEFT, INPUT_ACTION_LAY_MINE, 0, 0, 0);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->lastActionAppliedTick[SA_SLOT] == mineTick,
                  "harvest must set the marker to the mine tick, got %u",
                  sim->lastActionAppliedTick[SA_SLOT]);
    UT_ASSERT_MSG(sim->pendingHarvestActions[SA_SLOT] & INPUT_ACTION_LAY_MINE,
                  "the mine bit must be pending after harvest");
    BYTE minesBeforeLay = tankGetMines(&gs->tanks[SA_SLOT]);

    /* Next real input on a game (even) tick folds the pending mine and lays
     * it exactly once. The late-burst frame's stalls grew jitterTarget (it
     * can reach 3), so a 2-input pair no longer reopens the re-fill gate
     * (depth < target → both half-steps substitute and the fed tick goes
     * stale). Read the live target and lastProcessedInput, then feed a burst
     * that exceeds the target starting just past lastProcessedInput — that
     * reopens the gate and guarantees an even (game) tick is among those
     * applied, so the fold runs. */
    uint32_t lpi = sim->lastProcessedInput[SA_SLOT];
    uint8_t  jt  = sim->jitterTarget[SA_SLOT];
    uint32_t burst = (uint32_t)jt + 2;  /* > target, spans both parities */
    uint32_t w;
    for (w = 1; w <= burst; w++) {
        sa_feed(sim, lpi + w, INPUT_BTN_LEFT, 0, 0, 0, 0);
    }
    int frames;
    for (frames = 0; frames < 8 &&
         (sim->pendingHarvestActions[SA_SLOT] & INPUT_ACTION_LAY_MINE); frames++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(!(sim->pendingHarvestActions[SA_SLOT] & INPUT_ACTION_LAY_MINE),
                  "pending mine must clear after folding into a real game input");
    /* Read the laying tick off the marker rather than predicting it: it must
     * be a real, applied game (even) input past the mine tick. */
    uint32_t layTick = sim->lastActionAppliedTick[SA_SLOT];
    UT_ASSERT_MSG(layTick > mineTick && (layTick % 2) == 0,
                  "marker must be the real game tick that laid the mine, got %u",
                  layTick);

    /* The action layer laid exactly once (marker + pending prove it). The
     * physical inventory only decrements if the slot-0 spawn tile permits
     * a lay — a tank that spawns on a boat cannot lay — so assert it never
     * laid twice rather than asserting a fixed decrement. */
    BYTE minesAfterLay = tankGetMines(&gs->tanks[SA_SLOT]);
    UT_ASSERT_MSG((BYTE)(minesBeforeLay - minesAfterLay) <= 1,
                  "mine inventory dropped by more than one: %u -> %u",
                  minesBeforeLay, minesAfterLay);

    /* A redundant duplicate of the harvested mine tick and of the laying
     * tick (both now stale) must not lay a second mine. (The robust
     * ignored-duplicate check is test 3; this confirms the live numbers.) */
    sa_feed(sim, mineTick, INPUT_BTN_LEFT, INPUT_ACTION_LAY_MINE, 0, 0, 0);
    sa_feed(sim, layTick, INPUT_BTN_LEFT, 0, 0, 0, 0);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->pendingHarvestActions[SA_SLOT] == 0,
                  "redundant duplicates must not create pending state");
    UT_ASSERT_MSG(tankGetMines(&gs->tanks[SA_SLOT]) == minesAfterLay,
                  "no second lay from redundant duplicates");

    serverSimDestroy(sim);
    return 0;
}

/* 3. A redundant duplicate of an already-applied mine is ignored, not
 *    re-laid. */
int run_stall_mine_duplicate_not_relaid(void) {
    ServerSim *sim = ut_make_running_sim("MineDup");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL && gs->tanks[SA_SLOT] != NULL);
    tankSetMines(&gs->tanks[SA_SLOT], 40);

    sa_establish(sim, INPUT_BTN_LEFT, 0);  /* lastProcessedInput = 12 */

    /* Apply a real input that lays a mine on a game (even) tick. Feed 13
     * (keys) + 14 (game, mine) as a fresh pair. */
    sa_feed(sim, 13, INPUT_BTN_LEFT, 0, 0, 0, 0);
    sa_feed(sim, 14, INPUT_BTN_LEFT, INPUT_ACTION_LAY_MINE, 0, 0, 0);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->lastActionAppliedTick[SA_SLOT] == 14,
                  "real mine apply must set marker to 14, got %u",
                  sim->lastActionAppliedTick[SA_SLOT]);
    UT_ASSERT_MSG(sim->pendingHarvestActions[SA_SLOT] == 0, "no pending after real lay");
    BYTE minesAfter = tankGetMines(&gs->tanks[SA_SLOT]);

    /* Enqueue a redundant duplicate of the already-applied mine tick. It
     * is stale (14 <= lastProcessedInput) AND 14 <= lastActionAppliedTick,
     * so it is an ordinary duplicate: ignored, not harvested. */
    sa_feed(sim, 14, INPUT_BTN_LEFT, INPUT_ACTION_LAY_MINE, 0, 0, 0);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->pendingHarvestActions[SA_SLOT] == 0,
                  "duplicate of an applied mine must not create pending state");
    UT_ASSERT_MSG(sim->lastActionAppliedTick[SA_SLOT] == 14,
                  "marker must be unchanged by the duplicate, got %u",
                  sim->lastActionAppliedTick[SA_SLOT]);
    UT_ASSERT_MSG(tankGetMines(&gs->tanks[SA_SLOT]) == minesAfter,
                  "duplicate must not lay a second mine");

    serverSimDestroy(sim);
    return 0;
}

/* 4. Fire is excluded from carry-forward: a stale-dropped fire-only entry
 *    creates no pending state, but the marker still advances to its tick. */
int run_stall_fire_not_harvested(void) {
    ServerSim *sim = ut_make_running_sim("FireDrop");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    sa_establish(sim, INPUT_BTN_LEFT, 0);  /* lastProcessedInput = 12 */

    /* Substitute past ticks 13 and 14. */
    serverSimTick(sim);  /* 12 -> 14 */
    UT_ASSERT(sim->lastProcessedInput[SA_SLOT] >= 14);
    UT_ASSERT(sim->lastActionAppliedTick[SA_SLOT] == 0);

    /* Deliver a late stale burst: a movement on tick 13 then a FIRE-only
     * entry on tick 14 (the burst opens the re-fill gate; the fire is the
     * higher tick so it lands last). Both drop; fire is never harvested,
     * but 14 > lastActionAppliedTick so the marker advances to 14 with no
     * pending state created. */
    sa_feed(sim, 13, INPUT_BTN_LEFT, 0, 0, 0, 0);
    sa_feed(sim, 14, 0, INPUT_ACTION_FIRE, 0, 0, 0);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->pendingHarvestActions[SA_SLOT] == 0,
                  "fire must not be harvested into pending actions");
    UT_ASSERT_MSG(sim->pendingHarvestBuildAction[SA_SLOT] == 0,
                  "no pending build from a fire-only entry");
    UT_ASSERT_MSG(sim->lastActionAppliedTick[SA_SLOT] == 14,
                  "marker must advance to the dropped fire tick, got %u",
                  sim->lastActionAppliedTick[SA_SLOT]);

    serverSimDestroy(sim);
    return 0;
}

/* 5. Substitutes never fire: with FIRE held in the input stream, the live
 *    shell count never grows during a pure-stall window. */
int run_stall_never_fires(void) {
    ServerSim *sim = ut_make_running_sim("NoFire");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    /* Establish with FIRE asserted every input so real game ticks shoot
     * (reload-gated). Substitutes carry actions = 0, so they cannot. */
    sa_establish(sim, 0, INPUT_ACTION_FIRE);

    ShellRender shells[MAX_SNAPSHOT_SHELLS];
    int prev = serverSimGetShellSnapshot(sim, shells, MAX_SNAPSHOT_SHELLS);

    /* Pure-stall window: many empty frames. A substitute that wrongly
     * fired would raise the shell count; in-flight shells may only expire,
     * so the count must be non-increasing throughout. */
    int i;
    for (i = 0; i < 20; i++) {
        serverSimTick(sim);
        int now = serverSimGetShellSnapshot(sim, shells, MAX_SNAPSHOT_SHELLS);
        UT_ASSERT_MSG(now <= prev,
                      "shell count rose during a pure stall (%d -> %d) — "
                      "a substitute fired", prev, now);
        prev = now;
    }

    serverSimDestroy(sim);
    return 0;
}
