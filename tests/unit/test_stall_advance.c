/*
 * Stall-advance: a stall-substituted tick is a *processed* tick.
 *
 * When a player's input queue runs dry mid-stream the server substitutes
 * the held buttons AND advances lastProcessedInput past the substituted
 * tick. The real (late) input for that tick then arrives stale and its
 * movement is dropped instead of double-executing the held turn (the
 * gunsight-overshoot fix). The drop is not unconditional: while the slot is
 * in a stall-advance run the newest queued entry is taken instead, so the
 * stream restarts, and everything below it still drops. One-shot actions
 * (mine/build) commanded during a stall are never executed by the
 * substitute, so an entry below that newest tick is harvested off its drop
 * under a lastActionAppliedTick invariant and laid exactly once on the next
 * real input — never zero, never twice. The newest tick needs no harvest:
 * it is applied and lays as it goes.
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

    /* The first STALL_ADVANCE_DRY_TICKS dry half-steps wait without movement
     * (brief-trough handling), so the substitute only starts advancing
     * once the dry run passes the threshold. Prime two empty frames to
     * clear the wait window before measuring the steady +2-per-frame
     * advance below. */
    serverSimTick(sim);  /* dry 1,2: both wait */
    serverSimTick(sim);  /* dry 3,4: both wait */

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
     * They are all <= lastProcessedInput, and the slot is in a stall-advance
     * run, so the newest of them is taken: lastProcessedInput drops to
     * afterStall - 1 and the apply puts it straight back at afterStall.
     * Every older entry still drops as stale, and the frame's second
     * half-step finds the dry counter reset below the threshold, so it
     * waits. The net advance across the frame is zero. */
    uint32_t k, delivered = 0;
    for (k = before + 1; k <= afterStall; k++) {
        sa_feed(sim, k, INPUT_BTN_LEFT, 0, 0, 0, 0);
        delivered++;
    }
    uint32_t staleBefore = sim->statDroppedStaleInputs[SA_SLOT];
    uint32_t lpBefore = sim->lastProcessedInput[SA_SLOT];
    serverSimTick(sim);  /* drains the burst: all but the newest drop */
    UT_ASSERT_MSG(sim->statDroppedStaleInputs[SA_SLOT] ==
                      staleBefore + delivered - 1,
                  "expected %u stale drops, got %u", delivered - 1,
                  sim->statDroppedStaleInputs[SA_SLOT] - staleBefore);
    UT_ASSERT_MSG(sim->lastProcessedInput[SA_SLOT] == lpBefore,
                  "the late burst must leave lastProcessedInput where the "
                  "stall left it (lp moved by %u)",
                  sim->lastProcessedInput[SA_SLOT] - lpBefore);

    serverSimDestroy(sim);
    return 0;
}

/* 2. A mine commanded on a tick the stall substituted past is laid exactly
 *    once, when the late input for that tick is the one the rebase takes. */
int run_stall_mine_late_lays_once(void) {
    ServerSim *sim = ut_make_running_sim("MineLate");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL && gs->tanks[SA_SLOT] != NULL);
    tankSetMines(gs, &gs->tanks[SA_SLOT], 40);

    sa_establish(sim, INPUT_BTN_LEFT, 0);  /* lastProcessedInput = 12 */

    /* The withheld game (even) tick that carried LAY_MINE. */
    uint32_t mineTick = 14;

    /* Stall window substitutes past mineTick without ever laying it. The
     * first STALL_ADVANCE_DRY_TICKS dry half-steps wait, so run
     * empty frames until the substitute has advanced past the mine tick. */
    int sf;
    for (sf = 0; sf < 5 && sim->lastProcessedInput[SA_SLOT] < mineTick; sf++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(sim->lastProcessedInput[SA_SLOT] >= mineTick,
                  "stall did not advance past the mine tick");
    UT_ASSERT_MSG(sim->lastActionAppliedTick[SA_SLOT] == 0,
                  "substitutes must not touch the action marker");
    UT_ASSERT_MSG(sim->pendingHarvestActions[SA_SLOT] == 0,
                  "no pending action before the late mine arrives");

    /* Deliver the withheld ticks late as a burst (mineTick-1 then mineTick,
     * mirroring a redundancy retransmit). A lone stale input would not be
     * dequeued while the buffer is re-filling (depth < jitterTarget), so a
     * 2-input burst opens the gate. The slot is in a stall-advance run, so
     * the newest queued tick is taken: mineTick-1 still drops as stale and
     * mineTick applies for real, laying its mine as it goes. */
    sa_feed(sim, mineTick - 1, INPUT_BTN_LEFT, 0, 0, 0, 0);
    sa_feed(sim, mineTick, INPUT_BTN_LEFT, INPUT_ACTION_LAY_MINE, 0, 0, 0);
    BYTE minesBeforeLay = tankGetMines(&gs->tanks[SA_SLOT]);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->lastActionAppliedTick[SA_SLOT] == mineTick,
                  "the real apply must set the marker to the mine tick, got %u",
                  sim->lastActionAppliedTick[SA_SLOT]);
    UT_ASSERT_MSG(sim->pendingHarvestActions[SA_SLOT] == 0,
                  "an executed mine must not also be carried forward");

    /* The action layer laid exactly once (marker + pending prove it). The
     * physical inventory only decrements if the slot-0 spawn tile permits
     * a lay — a tank that spawns on a boat cannot lay — so assert it never
     * laid twice rather than asserting a fixed decrement. */
    BYTE minesAfterLay = tankGetMines(&gs->tanks[SA_SLOT]);
    UT_ASSERT_MSG((BYTE)(minesBeforeLay - minesAfterLay) <= 1,
                  "mine inventory dropped by more than one: %u -> %u",
                  minesBeforeLay, minesAfterLay);

    /* A redundant duplicate of the laid mine tick (stale, and no newer than
     * lastActionAppliedTick) must not lay a second one. (The robust
     * ignored-duplicate check is test 3; this confirms the live numbers.)
     * The queue emptied on the laying frame, so the buffer is re-filling:
     * feed jitterTarget copies or nothing is dequeued and the check proves
     * nothing. */
    uint8_t  jt = sim->jitterTarget[SA_SLOT];
    uint32_t d;
    for (d = 0; d < (uint32_t)jt; d++) {
        sa_feed(sim, mineTick, INPUT_BTN_LEFT, INPUT_ACTION_LAY_MINE, 0, 0, 0);
    }
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
    tankSetMines(gs, &gs->tanks[SA_SLOT], 40);

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

    /* Substitute past ticks 13 and 14. The first STALL_ADVANCE_DRY_TICKS
     * dry half-steps wait, so run empty frames until the
     * substitute has advanced past tick 14. */
    int sf;
    for (sf = 0; sf < 5 && sim->lastProcessedInput[SA_SLOT] < 14; sf++) {
        serverSimTick(sim);
    }
    UT_ASSERT(sim->lastProcessedInput[SA_SLOT] >= 14);
    UT_ASSERT(sim->lastActionAppliedTick[SA_SLOT] == 0);

    /* Deliver a late burst: the FIRE-only entry on tick 14, then a movement
     * on tick 15 (two inputs so the re-fill gate opens). 15 is above
     * lastProcessedInput, so the newest queued tick is not one the boundary
     * rebase can take and the slot is left where the stall put it. That
     * keeps 14 at or below lastProcessedInput, so it drops as stale and goes
     * through the harvest path, where fire is excluded: nothing is made
     * pending, but 14 > lastActionAppliedTick so the marker advances to 14.
     * 15 applies fresh and is a keys (odd) tick, which never moves the
     * marker. */
    sa_feed(sim, 14, 0, INPUT_ACTION_FIRE, 0, 0, 0);
    sa_feed(sim, 15, INPUT_BTN_LEFT, 0, 0, 0, 0);
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

/* 6. Regression guard: a dry spell no longer than STALL_ADVANCE_DRY_TICKS
 *    half-steps must NOT advance lastProcessedInput and must NOT eat the
 *    in-flight real inputs — the tank waits without advancing so the late
 *    inputs still apply fresh at their true ticks. This is the cadence
 *    trough the regression fix restores: the client batches two inputs per
 *    packet while the server consumes one per half-step, so the queue
 *    routinely drains for a half-step or two between packets without any
 *    loss, and substituting there would wrongly drop the real input. */
int run_stall_brief_trough_no_advance(void) {
    ServerSim *sim = ut_make_running_sim("Trough");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    uint32_t next = sa_establish(sim, INPUT_BTN_LEFT, 0);  /* lpi 12, next 13 */
    UT_ASSERT_MSG(sim->lastProcessedInput[SA_SLOT] > 0, "stream not established");

    /* A lone fresh input lands on the first half-step and resets the dry
     * counter, leaving the second half-step as the first dry half-step. */
    sa_feed(sim, next, INPUT_BTN_LEFT, 0, 0, 0, 0);  /* tick 13 */
    serverSimTick(sim);                              /* applies 13, then dry #1 */
    uint32_t troughBase = sim->lastProcessedInput[SA_SLOT];
    UT_ASSERT_MSG(troughBase == next, "lone fresh input did not apply");

    /* One more empty frame: dry #2 and dry #3, still within the four-tick
     * grace period, so the tank and ACK must still wait. */
    uint32_t staleBefore = sim->statDroppedStaleInputs[SA_SLOT];
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->lastProcessedInput[SA_SLOT] == troughBase,
                  "a brief dry run advanced lastProcessedInput "
                  "(%u -> %u) — a brief trough must wait, not substitute-advance",
                  troughBase, sim->lastProcessedInput[SA_SLOT]);

    /* Deliver the real inputs for the tick numbers the trough spanned.
     * Because the tank waited (lpi never advanced past troughBase),
     * these are all fresh (tick > lpi): they must apply, not drop stale. */
    uint32_t t;
    for (t = troughBase + 1; t <= troughBase + 3; t++) {
        sa_feed(sim, t, INPUT_BTN_LEFT, 0, 0, 0, 0);
    }
    int frames;
    for (frames = 0; frames < 4 &&
         sim->lastProcessedInput[SA_SLOT] < troughBase + 3; frames++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(sim->lastProcessedInput[SA_SLOT] >= troughBase + 3,
                  "delivered trough inputs did not all apply (lpi %u < %u)",
                  sim->lastProcessedInput[SA_SLOT], troughBase + 3);
    UT_ASSERT_MSG(sim->statDroppedStaleInputs[SA_SLOT] == staleBefore,
                  "brief-trough inputs were dropped as stale (%u new drops) — "
                  "the regression: substitutes ate real inputs",
                  sim->statDroppedStaleInputs[SA_SLOT] - staleBefore);

    serverSimDestroy(sim);
    return 0;
}

/* 7. A dry spell past the threshold still stall-advances (genuine loss):
 *    lastProcessedInput climbs during the dry run and the late inputs for
 *    the substituted ticks drop as stale. Confirms the regression fix did
 *    not disable stall-advance, only gate it. */
int run_stall_long_dry_advances(void) {
    ServerSim *sim = ut_make_running_sim("LongDry");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    sa_establish(sim, INPUT_BTN_LEFT, 0);  /* lpi 12 */
    uint32_t before = sim->lastProcessedInput[SA_SLOT];

    /* Dry spell well past the threshold: STALL_ADVANCE_DRY_TICKS + 4
     * half-steps. The first STALL_ADVANCE_DRY_TICKS wait, then every
     * further dry half-step substitutes-and-advances. Round up to whole
     * frames so the dry run comfortably clears the threshold. */
    int frames = (STALL_ADVANCE_DRY_TICKS + 4 + 1) / 2;
    int f;
    for (f = 0; f < frames; f++) {
        serverSimTick(sim);
    }
    uint32_t afterDry = sim->lastProcessedInput[SA_SLOT];
    UT_ASSERT_MSG(afterDry > before,
                  "long dry spell did not advance lastProcessedInput past the "
                  "threshold (still %u)", afterDry);

    /* Deliver the late real inputs for the substituted tick numbers. They
     * are all <= lastProcessedInput now, so genuine loss still drops them
     * as stale — the late movement does not re-execute. */
    uint32_t staleBefore = sim->statDroppedStaleInputs[SA_SLOT];
    uint32_t t;
    for (t = before + 1; t <= afterDry; t++) {
        sa_feed(sim, t, INPUT_BTN_LEFT, 0, 0, 0, 0);
    }
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->statDroppedStaleInputs[SA_SLOT] > staleBefore,
                  "late inputs for substituted ticks were not dropped as stale "
                  "— stall-advance failed to consume the ticks under real loss");

    serverSimDestroy(sim);
    return 0;
}
