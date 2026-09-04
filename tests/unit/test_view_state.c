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
