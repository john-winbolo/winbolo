/*
 * Client-reported view state (test_view_state.c).
 *
 * A client tells the server which view it is in with CMD_VIEW_STATE. The
 * server stores the claim per slot and, for a category set to viewPolicyKey,
 * grants exactly one rect — the claimed item's — while that item still
 * qualifies, in place of the recipient's own tank screen rather than as well
 * as it: under key the player sees one thing at a time, so the ground round
 * their tank stops being sent while they watch something else. A claim the
 * server cannot honour is never an error: the
 * dispatcher degrades an out-of-range one to the tank view on the spot, and
 * serverSimValidateViewTargets puts a slot back on the tank view once its
 * target stops qualifying, whereupon the rect simply stops being built.
 *
 * Every case drives ut_make_running_sim and pokes the GameSim directly (the
 * unittests profile permits T2-internal access), sends claims through
 * serverSimApplyCommand, and reads the result back through inAnyViewport.
 */

#include <string.h>

#include "global.h"
#include "client_command.h"
#include "control_event.h"          /* ControlEvent / CTRL_VIEW_TARGET */
#include "server_sim.h"
#include "server_sim_internal.h"   /* serverSimBuildViewports / ViewportRect / view state */
#include "game_sim.h"
#include "tank.h"
#include "pillbox.h"
#include "bases.h"
#include "players.h"
#include "threads.h"
#include "view_policy.h"
#include "test_harness.h"

/* The world centre of a map square. */
static WORLD vs_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
}

/* Hand every pill and base to nobody, so only what a case sets can produce a
 * rect regardless of what the loaded map ships with. */
static void vs_clear_owners(GameSim *gs) {
    BYTE n;
    BYTE i;

    n = pillsGetNumPills(&gs->pb);
    for (i = 0; i < n; i++) {
        gs->pb->item[i].owner  = NEUTRAL;
        gs->pb->item[i].inTank = FALSE;
    }
    n = basesGetNumBases(&gs->bs);
    for (i = 0; i < n; i++) {
        gs->bs->item[i].owner = NEUTRAL;
    }
}

/* Park a live tank on a map square. */
static void vs_place_tank(GameSim *gs, BYTE slot, BYTE mx, BYTE my) {
    tankSetWorld(gs, &gs->tanks[slot], vs_world(mx), vs_world(my), 0, false);
    gs->tanks[slot]->deathWait = 0;
}

/* Own a live pillbox at a map square. */
static void vs_place_pill(GameSim *gs, BYTE idx, BYTE owner, BYTE mx, BYTE my) {
    gs->pb->item[idx].owner  = owner;
    gs->pb->item[idx].armour = PILL_MAX_HEALTH;
    gs->pb->item[idx].inTank = FALSE;
    gs->pb->item[idx].x      = mx;
    gs->pb->item[idx].y      = my;
}

/* Send one CMD_VIEW_STATE as `senderSlot`, holding the threads mutex the
 * dispatcher asserts on. */
static CmdResult vs_send_view(ServerSim *sim, int senderSlot,
                              uint8_t kind, uint8_t target) {
    ClientCommand cmd;
    CmdResult r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_VIEW_STATE;
    cmd.cmdSeq = 1;
    cmd.u.viewState.kind   = kind;
    cmd.u.viewState.target = target;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Send one CMD_VIEW_CYCLE as `senderSlot`, holding the threads mutex the
 * dispatcher asserts on. */
static CmdResult vs_send_cycle(ServerSim *sim, int senderSlot, uint8_t kind,
                               uint8_t direction, uint8_t from) {
    ClientCommand cmd;
    CmdResult r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_VIEW_CYCLE;
    cmd.cmdSeq = 1;
    cmd.u.viewCycle.kind      = kind;
    cmd.u.viewCycle.direction = direction;
    cmd.u.viewCycle.from      = from;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Keeps the CTRL_VIEW_TARGET answers a case drives, and only those. */
typedef struct {
    int count;
    ControlEvent last;
} VsViewTargetCapture;

static void vs_capture_view_target(void *ctx, const ControlEvent *evt) {
    VsViewTargetCapture *c = (VsViewTargetCapture *)ctx;
    if (evt->type == CTRL_VIEW_TARGET) {
        c->count++;
        c->last = *evt;
    }
}

/* Send a claim the server must not honour and require the three things that
 * make it harmless: it is accepted, the slot ends up back on the tank view,
 * and the recipient gets no rect beyond its own tank screen. */
static int vs_expect_degraded(ServerSim *sim, uint8_t kind, uint8_t target,
                              const char *what) {
    ViewportRect vps[MAX_VIEWPORTS];
    int n;

    UT_ASSERT_MSG(vs_send_view(sim, 0, kind, target) == CMD_OK,
                  "%s should be accepted, not rejected", what);
    serverSimValidateViewTargets(sim);
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_TANK,
                  "%s should leave the slot on the tank view, got kind %u",
                  what, (unsigned)sim->viewKind[0]);
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 1, "%s should grant no rect beyond the tank screen, got %d",
                  what, n);
    return 0;
}

/* 1. Under viewPolicyKey a category grants nothing until the client says it is
 *    viewing an item, then exactly one rect — the claimed one. */
int run_view_state_key_grants_rect(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "Everard map has no pills (%u) — test needs one",
                  pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");

    vs_clear_owners(gs);
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyKey, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);

    vs_place_tank(gs, 0, 50, 50);
    vs_place_pill(gs, 0, 0, 200, 200);

    ViewportRect vps[MAX_VIEWPORTS];
    int n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 1, "no claim should leave only the tank rect, got %d", n);
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 200, 200),
                  "an unclaimed pill must not be covered under key");

    UT_ASSERT_MSG(vs_send_view(sim, 0, VIEW_KIND_PILL, 0) == CMD_OK,
                  "a valid pill claim should be accepted");
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_PILL && sim->viewTarget[0] == 0,
                  "claim should be stored (kind %u target %u)",
                  (unsigned)sim->viewKind[0], (unsigned)sim->viewTarget[0]);

    /* One view at a time: the claimed pill's rect is the whole set while the
     * claim stands, and the tank's own screen goes with it. */
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 1, "claim should leave the pill rect on its own, got %d", n);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 200, 200), "claimed pill square not covered");
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 50, 50),
                  "the tank square must not be covered while a key view stands");

    /* The validator leaves a claim that still qualifies alone. */
    serverSimValidateViewTargets(sim);
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_PILL,
                  "a qualifying claim must survive the validator");

    /* The frozen rect a tankless slot falls back on does not creep in behind
     * the key view either. Put the tank back afterwards so serverSimDestroy
     * tears it down as usual. */
    {
        tank saved = gs->tanks[0];
        gs->tanks[0] = NULL;
        n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
        UT_ASSERT_MSG(n == 1, "a tankless slot on a key view should still get "
                      "the pill rect on its own, got %d", n);
        UT_ASSERT_MSG(!inAnyViewport(vps, n, 50, 50),
                      "the last known tank square must not be covered while a "
                      "key view stands");
        gs->tanks[0] = saved;
    }

    /* Back to the tank view: the pill's rect goes and the tank's comes back. */
    UT_ASSERT_MSG(vs_send_view(sim, 0, VIEW_KIND_TANK, 0) == CMD_OK,
                  "the tank-view claim should be accepted");
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 1, "back on the tank view should leave one rect, got %d", n);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 50, 50),
                  "the tank square should be covered again");
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 200, 200),
                  "the pill square must not be covered once the claim is dropped");

    serverSimDestroy(sim);
    return 0;
}

/* 2. Every claim the server cannot honour is accepted and degraded to the tank
 *    view instead of rejected, and grants no rect. */
int run_view_state_bad_claims_degrade(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1 && basesGetNumBases(&gs->bs) >= 1,
                  "test needs at least one pill and one base");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "players 0 and 1 must stay un-allied for the enemy cases");

    vs_clear_owners(gs);
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyKey, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyKey, VIEW_DECAY_DEFAULT_SECS);

    vs_place_tank(gs, 0, 50, 50);
    vs_place_tank(gs, 1, 200, 50);

    /* An enemy's pillbox. */
    vs_place_pill(gs, 0, 1, 200, 200);
    UT_ASSERT(vs_expect_degraded(sim, VIEW_KIND_PILL, 0, "an enemy pill claim") == 0);

    /* A dead pillbox. */
    vs_place_pill(gs, 0, 0, 200, 200);
    gs->pb->item[0].armour = 0;
    UT_ASSERT(vs_expect_degraded(sim, VIEW_KIND_PILL, 0, "a dead pill claim") == 0);

    /* A pillbox someone is carrying. */
    vs_place_pill(gs, 0, 0, 200, 200);
    gs->pb->item[0].inTank = TRUE;
    UT_ASSERT(vs_expect_degraded(sim, VIEW_KIND_PILL, 0, "a carried pill claim") == 0);

    /* A pill index the map does not have. */
    vs_place_pill(gs, 0, 0, 200, 200);
    UT_ASSERT(vs_expect_degraded(sim, VIEW_KIND_PILL, MAX_PILLS,
                                 "an out-of-range pill claim") == 0);

    /* A kind this server does not know. */
    UT_ASSERT(vs_expect_degraded(sim, 9, 0, "an unknown view kind") == 0);

    /* An ally claim naming the sender itself. */
    UT_ASSERT(vs_expect_degraded(sim, VIEW_KIND_ALLY, 0,
                                 "an ally claim on the sender's own slot") == 0);

    /* A base whose category is switched off: in range and allied, so the
     * dispatcher stores it, and the validator drops it on the policy. */
    gs->bs->item[0].owner = 0;
    gs->bs->item[0].x     = 20;
    gs->bs->item[0].y     = 220;
    UT_ASSERT(vs_expect_degraded(sim, VIEW_KIND_BASE, 0,
                                 "a base claim while the category is off") == 0);

    serverSimDestroy(sim);
    return 0;
}

/* 3. A claim that was good stops being good: once the target no longer
 *    qualifies the validator puts the slot back on the tank view and the rect
 *    is gone, with no event sent either way. */
int run_view_state_invalidation(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "test needs at least one pill");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");

    vs_clear_owners(gs);
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyKey, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyKey, VIEW_DECAY_DEFAULT_SECS);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) == TRUE,
                  "players 0 and 1 should be allied for this test");

    vs_place_tank(gs, 0, 50, 50);
    vs_place_tank(gs, 1, 200, 50);
    vs_place_pill(gs, 0, 0, 200, 200);

    ViewportRect vps[MAX_VIEWPORTS];
    int n;

    /* Pill: valid claim, then the pill dies. */
    UT_ASSERT_MSG(vs_send_view(sim, 0, VIEW_KIND_PILL, 0) == CMD_OK,
                  "a valid pill claim should be accepted");
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 200, 200), "claimed pill square not covered");

    gs->pb->item[0].armour = 0;
    serverSimValidateViewTargets(sim);
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_TANK,
                  "a dead pill must reset the claim, got kind %u",
                  (unsigned)sim->viewKind[0]);
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 200, 200),
                  "the dead pill's square must not be covered");

    /* Ally: valid claim, then the ally starts waiting out a death. */
    UT_ASSERT_MSG(vs_send_view(sim, 0, VIEW_KIND_ALLY, 1) == CMD_OK,
                  "a valid ally claim should be accepted");
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 200, 50), "claimed ally square not covered");

    gs->tanks[1]->deathWait = 10;
    serverSimValidateViewTargets(sim);
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_TANK,
                  "a dead ally must reset the claim, got kind %u",
                  (unsigned)sim->viewKind[0]);
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 200, 50),
                  "the dead ally's square must not be covered");

    serverSimDestroy(sim);
    return 0;
}

/* 4. Stored claims do not outlive the thing they name: a target that leaves
 *    resets whoever was viewing through it, and the round reset clears the
 *    lot. */
int run_view_state_lifecycle(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "test needs at least one pill");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");

    vs_clear_owners(gs);
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyKey, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyKey, VIEW_DECAY_DEFAULT_SECS);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);

    vs_place_tank(gs, 0, 50, 50);
    vs_place_tank(gs, 1, 200, 50);
    vs_place_pill(gs, 0, 0, 200, 200);

    /* The player being viewed through leaves. */
    UT_ASSERT_MSG(vs_send_view(sim, 0, VIEW_KIND_ALLY, 1) == CMD_OK,
                  "a valid ally claim should be accepted");
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_ALLY && sim->viewTarget[0] == 1,
                  "ally claim should be stored (kind %u target %u)",
                  (unsigned)sim->viewKind[0], (unsigned)sim->viewTarget[0]);

    serverSimRemovePlayer(sim, 1);
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_TANK,
                  "the viewer must be reset when its target leaves, got kind %u",
                  (unsigned)sim->viewKind[0]);
    UT_ASSERT_MSG(sim->viewKind[1] == VIEW_KIND_TANK,
                  "the leaver's own claim must be cleared, got kind %u",
                  (unsigned)sim->viewKind[1]);

    /* The round reset clears whatever is stored. */
    UT_ASSERT_MSG(vs_send_view(sim, 0, VIEW_KIND_PILL, 0) == CMD_OK,
                  "a valid pill claim should be accepted");
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_PILL,
                  "pill claim should be stored before the reset");

    serverSimResetGameWorld(sim);
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_TANK && sim->viewTarget[0] == 0,
                  "round reset must clear the reported views (kind %u target %u)",
                  (unsigned)sim->viewKind[0], (unsigned)sim->viewTarget[0]);

    serverSimDestroy(sim);
    return 0;
}

/* 5. The picker walks the allies the recipient may watch, in slot order: next
 *    steps up, previous steps down, both wrap, and a dead ally, an un-allied
 *    player and the sender itself are never offered. */
int run_view_cycle_pick_order(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);
    serverSimAddPlayer(sim, 3, "P3", false);
    serverSimAddPlayer(sim, 4, "P4", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    for (int s = 0; s < 5; s++) {
        UT_ASSERT_MSG(gs->tanks[s] != NULL, "slot %d needs a tank", s);
    }

    vs_clear_owners(gs);
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyAlways, VIEW_DECAY_DEFAULT_SECS);

    /* Slots 1, 2 and 4 are allied with the sender; slot 3 is not. */
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 2, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 4, TRUE);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) == TRUE &&
                  playersIsAllie(&gs->plyrs, 0, 2) == TRUE &&
                  playersIsAllie(&gs->plyrs, 0, 4) == TRUE,
                  "slots 1, 2 and 4 should be allied with the sender");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 3) != TRUE,
                  "slot 3 must stay un-allied for the skip case");

    vs_place_tank(gs, 0, 50, 50);
    vs_place_tank(gs, 1, 60, 50);
    vs_place_tank(gs, 2, 70, 50);
    vs_place_tank(gs, 3, 80, 50);
    vs_place_tank(gs, 4, 90, 50);

    BYTE t = 0xEE, mx = 0xEE, my = 0xEE;

    /* No `from`: the lowest watchable slot, with its current square. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT,
                                    VIEW_CYCLE_FROM_NONE, &t, &mx, &my),
                  "next from nowhere should find an ally");
    UT_ASSERT_MSG(t == 1, "next from nowhere should pick slot 1, got %u",
                  (unsigned)t);
    UT_ASSERT_MSG(mx == 60 && my == 50,
                  "the pick's square should be (60,50), got (%u,%u)",
                  (unsigned)mx, (unsigned)my);

    /* Next steps up in slot order. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT, 1, &t, &mx, &my),
                  "next from slot 1 should find an ally");
    UT_ASSERT_MSG(t == 2, "next from slot 1 should pick slot 2, got %u",
                  (unsigned)t);

    /* The un-allied slot 3 is stepped over. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT, 2, &t, &mx, &my),
                  "next from slot 2 should find an ally");
    UT_ASSERT_MSG(t == 4, "the un-allied slot 3 should be skipped, got %u",
                  (unsigned)t);

    /* The cycle wraps at the top. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT, 4, &t, &mx, &my),
                  "next from the highest ally should find an ally");
    UT_ASSERT_MSG(t == 1, "next from slot 4 should wrap to slot 1, got %u",
                  (unsigned)t);

    /* Previous steps down. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_PREV, 4, &t, &mx, &my),
                  "previous from slot 4 should find an ally");
    UT_ASSERT_MSG(t == 2, "previous from slot 4 should pick slot 2, got %u",
                  (unsigned)t);

    /* Previous from the lowest ally wraps past the sender's own slot rather
     * than offering it. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_PREV, 1, &t, &mx, &my),
                  "previous from slot 1 should find an ally");
    UT_ASSERT_MSG(t == 4, "previous from slot 1 should wrap to slot 4, got %u",
                  (unsigned)t);

    /* Previous with no `from` starts at the top end. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_PREV,
                                    VIEW_CYCLE_FROM_NONE, &t, &mx, &my),
                  "previous from nowhere should find an ally");
    UT_ASSERT_MSG(t == 4, "previous from nowhere should pick slot 4, got %u",
                  (unsigned)t);

    /* An ally waiting out a death is not offered. */
    gs->tanks[2]->deathWait = 10;
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT, 1, &t, &mx, &my),
                  "next from slot 1 should still find an ally");
    UT_ASSERT_MSG(t == 4, "the dead slot 2 should be skipped, got %u",
                  (unsigned)t);

    /* With one watchable ally left, next from it comes back to it instead of
     * reporting nothing to watch. */
    gs->tanks[4]->deathWait = 10;
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT, 1, &t, &mx, &my),
                  "the lone ally should still be found");
    UT_ASSERT_MSG(t == 1, "the lone ally should be offered again, got %u",
                  (unsigned)t);

    /* Nothing watchable at all. */
    gs->tanks[1]->deathWait = 10;
    t = 0xEE; mx = 0xEE; my = 0xEE;
    UT_ASSERT_MSG(!serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT,
                                     VIEW_CYCLE_FROM_NONE, &t, &mx, &my),
                  "no watchable ally should report nothing to watch");
    UT_ASSERT_MSG(t == 0xEE && mx == 0xEE && my == 0xEE,
                  "a not-found pick must not write its outputs (%u,%u,%u)",
                  (unsigned)t, (unsigned)mx, (unsigned)my);

    serverSimDestroy(sim);
    return 0;
}

/* 6. The view policies decide what the picker may offer, and the dispatcher
 *    answers every ally request — including the one with nothing to watch —
 *    with a CTRL_VIEW_TARGET that carries the request's `from` back. */
int run_view_cycle_pick_policy(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");

    vs_clear_owners(gs);
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) == TRUE,
                  "players 0 and 1 should be allied for this test");

    vs_place_tank(gs, 0, 50, 50);
    vs_place_tank(gs, 1, 200, 50);

    BYTE t = 0xEE, mx = 0xEE, my = 0xEE;

    /* Off: a live allied tank is still nothing to watch. */
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    UT_ASSERT_MSG(!serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT,
                                     VIEW_CYCLE_FROM_NONE, &t, &mx, &my),
                  "viewPolicyOff should offer no ally");

    /* Decay: an ally the recipient has never been near has a proximity clock
     * of 0 and is outside the window, so it is not offered; one stamped
     * inside the window is. This is the pair the picker exists to get right —
     * a pick outside the window would be dropped on the next validate pass. */
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyDecay, VIEW_DECAY_DEFAULT_SECS);
    sim->tick = 100000;
    sim->allyNearTick[0][1] = 0;
    UT_ASSERT_MSG(!serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT,
                                     VIEW_CYCLE_FROM_NONE, &t, &mx, &my),
                  "an ally with no proximity clock should not be offered");
    sim->allyNearTick[0][1] =
        sim->tick - ((uint32_t)VIEW_DECAY_DEFAULT_SECS * GAME_NUMTOTALTICKS_SEC) / 2;
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_NEXT,
                                    VIEW_CYCLE_FROM_NONE, &t, &mx, &my),
                  "an ally stamped inside the decay window should be offered");
    UT_ASSERT_MSG(t == 1, "the only ally is slot 1, got %u", (unsigned)t);

    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyAlways, VIEW_DECAY_DEFAULT_SECS);

    /* Subscribe after the joins and the alliance so the sync replay does not
     * land in the capture. */
    VsViewTargetCapture cap;
    memset(&cap, 0, sizeof(cap));
    SubscriberHandle h =
        serverSimRegisterSubscriber(sim, vs_capture_view_target, &cap);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    UT_ASSERT_MSG(cap.count == 0,
                  "nothing should have answered yet, got %d", cap.count);

    UT_ASSERT_MSG(vs_send_cycle(sim, 0, VIEW_KIND_ALLY, VIEW_CYCLE_NEXT,
                                VIEW_CYCLE_FROM_NONE) == CMD_OK,
                  "a view-cycle request should be accepted");
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_ALLY && sim->viewTarget[0] == 1,
                  "the pick should be stored (kind %u target %u)",
                  (unsigned)sim->viewKind[0], (unsigned)sim->viewTarget[0]);
    UT_ASSERT_MSG(cap.count == 1,
                  "one CTRL_VIEW_TARGET expected, got %d", cap.count);
    UT_ASSERT_MSG(cap.last.u.viewTarget.origSlot == 0,
                  "the answer should be addressed to slot 0, got %u",
                  (unsigned)cap.last.u.viewTarget.origSlot);
    UT_ASSERT_MSG(cap.last.u.viewTarget.found == 1,
                  "the answer should report a find, got found %u",
                  (unsigned)cap.last.u.viewTarget.found);
    UT_ASSERT_MSG(cap.last.u.viewTarget.kind == VIEW_KIND_ALLY &&
                  cap.last.u.viewTarget.target == 1,
                  "the answer should name ally 1 (kind %u target %u)",
                  (unsigned)cap.last.u.viewTarget.kind,
                  (unsigned)cap.last.u.viewTarget.target);
    UT_ASSERT_MSG(cap.last.u.viewTarget.mapX == 200 &&
                  cap.last.u.viewTarget.mapY == 50,
                  "the answer should carry the ally's square (200,50), got (%u,%u)",
                  (unsigned)cap.last.u.viewTarget.mapX,
                  (unsigned)cap.last.u.viewTarget.mapY);
    UT_ASSERT_MSG(cap.last.u.viewTarget.fromEcho == VIEW_CYCLE_FROM_NONE,
                  "the request's `from` should come back, got %u",
                  (unsigned)cap.last.u.viewTarget.fromEcho);

    /* Nothing to watch is answered, not rejected, and leaves the stored view
     * alone. */
    gs->tanks[1]->deathWait = 10;
    UT_ASSERT_MSG(vs_send_cycle(sim, 0, VIEW_KIND_ALLY, VIEW_CYCLE_NEXT, 1) == CMD_OK,
                  "a request with nothing to watch should still be accepted");
    UT_ASSERT_MSG(cap.count == 2,
                  "a second CTRL_VIEW_TARGET expected, got %d", cap.count);
    UT_ASSERT_MSG(cap.last.u.viewTarget.found == 0,
                  "the answer should report nothing found, got found %u",
                  (unsigned)cap.last.u.viewTarget.found);
    UT_ASSERT_MSG(cap.last.u.viewTarget.fromEcho == 1,
                  "the request's `from` should come back, got %u",
                  (unsigned)cap.last.u.viewTarget.fromEcho);
    UT_ASSERT_MSG(sim->viewKind[0] == VIEW_KIND_ALLY && sim->viewTarget[0] == 1,
                  "a not-found answer must leave the stored view alone "
                  "(kind %u target %u)",
                  (unsigned)sim->viewKind[0], (unsigned)sim->viewTarget[0]);

    /* Pill and base selection stays client-side: the request is accepted and
     * answered with nothing. */
    UT_ASSERT_MSG(vs_send_cycle(sim, 0, VIEW_KIND_PILL, VIEW_CYCLE_NEXT,
                                VIEW_CYCLE_FROM_NONE) == CMD_OK,
                  "a non-ally view-cycle request should be accepted");
    UT_ASSERT_MSG(cap.count == 2,
                  "a non-ally kind must publish nothing, got %d answers",
                  cap.count);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* 7. The four scroll directions. Each press compares one coordinate only and
 *    compares it strictly, and the nearest match in that direction wins.
 *
 *    The allies stand in a cross round the origin (slot 1), so a press on
 *    either axis has one unambiguous answer and a comparison that read the
 *    other coordinate would name a different slot. Slot 7 shares the origin's
 *    own square: were any comparison not strict it would be the nearest match
 *    in every direction, so all four checks below also pin the strictness. */
int run_view_cycle_pick_direction(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);
    serverSimAddPlayer(sim, 3, "P3", false);
    serverSimAddPlayer(sim, 4, "P4", false);
    serverSimAddPlayer(sim, 5, "P5", false);
    serverSimAddPlayer(sim, 6, "P6", false);
    serverSimAddPlayer(sim, 7, "P7", false);
    serverSimAddPlayer(sim, 8, "P8", false);
    serverSimAddPlayer(sim, 9, "P9", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    for (int s = 0; s < 10; s++) {
        UT_ASSERT_MSG(gs->tanks[s] != NULL, "slot %d needs a tank", s);
    }

    vs_clear_owners(gs);
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyAlways, VIEW_DECAY_DEFAULT_SECS);

    /* Everyone but slot 8 is allied with the sender. */
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 2, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 3, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 4, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 5, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 6, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 7, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 9, TRUE);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 8) != TRUE,
                  "slot 8 must stay un-allied for the skip case");

    /* The cross round the origin at (100,100): slot 2 to its left, slot 3
     * further left again, slot 4 right, slot 5 above, slot 6 below. Slot 7
     * stands on the origin's own square. Slot 8 is a closer right-hand
     * neighbour the sender is not allied with, and slot 9 a closer neighbour
     * below that is waiting out a death. The sender's own tank sits on the
     * origin's row well to the left, where only the rule that never offers
     * the sender keeps it out of a left press. */
    vs_place_tank(gs, 0,  10, 100);
    vs_place_tank(gs, 1, 100, 100);
    vs_place_tank(gs, 2,  90, 100);
    vs_place_tank(gs, 3,  80, 100);
    vs_place_tank(gs, 4, 110, 100);
    vs_place_tank(gs, 5, 100,  90);
    vs_place_tank(gs, 6, 100, 110);
    vs_place_tank(gs, 7, 100, 100);
    vs_place_tank(gs, 8, 105, 100);
    vs_place_tank(gs, 9, 100, 105);
    gs->tanks[9]->deathWait = 10;

    BYTE t = 0xEE, mx = 0xEE, my = 0xEE;

    /* Left: the nearer of the two allies on the origin's row, not the further
     * one and not a neighbour above or below. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_LEFT, 1, &t, &mx, &my),
                  "left from the origin should find an ally");
    UT_ASSERT_MSG(t == 2, "left should pick the nearer slot 2, got %u",
                  (unsigned)t);
    UT_ASSERT_MSG(mx == 90 && my == 100,
                  "left's square should be (90,100), got (%u,%u)",
                  (unsigned)mx, (unsigned)my);

    /* Right: the ally with the larger X. Slot 8 lies that way and is closer,
     * but the sender is not allied with it, so it is passed over. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_RIGHT, 1, &t, &mx, &my),
                  "right from the origin should find an ally");
    UT_ASSERT_MSG(t == 4, "right should pick slot 4, got %u", (unsigned)t);
    UT_ASSERT_MSG(mx == 110 && my == 100,
                  "right's square should be (110,100), got (%u,%u)",
                  (unsigned)mx, (unsigned)my);

    /* Up: the ally with the smaller Y, not either of the neighbours sharing
     * the origin's row. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_UP, 1, &t, &mx, &my),
                  "up from the origin should find an ally");
    UT_ASSERT_MSG(t == 5, "up should pick slot 5, got %u", (unsigned)t);
    UT_ASSERT_MSG(mx == 100 && my == 90,
                  "up's square should be (100,90), got (%u,%u)",
                  (unsigned)mx, (unsigned)my);

    /* Down: the ally with the larger Y. Slot 9 lies that way and is closer,
     * but it is waiting out a death, so it is passed over. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_DOWN, 1, &t, &mx, &my),
                  "down from the origin should find an ally");
    UT_ASSERT_MSG(t == 6, "down should pick slot 6, got %u", (unsigned)t);
    UT_ASSERT_MSG(mx == 100 && my == 110,
                  "down's square should be (100,110), got (%u,%u)",
                  (unsigned)mx, (unsigned)my);

    /* A direction with no origin has nothing to measure from, so it steps
     * from the start instead and answers with the lowest watchable slot. */
    UT_ASSERT_MSG(serverSimPickAlly(sim, 0, VIEW_CYCLE_LEFT,
                                    VIEW_CYCLE_FROM_NONE, &t, &mx, &my),
                  "left with no origin should still find an ally");
    UT_ASSERT_MSG(t == 1, "left with no origin should pick slot 1, got %u",
                  (unsigned)t);

    /* Nothing to the left once both row neighbours are waiting out a death.
     * The sender's own tank is over there and is still not offered. */
    gs->tanks[2]->deathWait = 10;
    gs->tanks[3]->deathWait = 10;
    t = 0xEE; mx = 0xEE; my = 0xEE;
    UT_ASSERT_MSG(!serverSimPickAlly(sim, 0, VIEW_CYCLE_LEFT, 1, &t, &mx, &my),
                  "nothing to the left should report nothing to watch");
    UT_ASSERT_MSG(t == 0xEE && mx == 0xEE && my == 0xEE,
                  "a not-found pick must not write its outputs (%u,%u,%u)",
                  (unsigned)t, (unsigned)mx, (unsigned)my);

    serverSimDestroy(sim);
    return 0;
}
