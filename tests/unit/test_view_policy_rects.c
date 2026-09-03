/*
 * Policy-driven viewport rects (test_view_policy_rects.c).
 *
 * serverSimBuildViewports builds each recipient's visibility set from the
 * server's per-category ViewPolicy: allied pillboxes, allied bases and allied
 * tanks each contribute a screen-sized rect under viewPolicyAlways, nothing
 * under viewPolicyOff and viewPolicyKey, and under viewPolicyDecay only while
 * the recipient's proximity clock for that item is unexpired. A client with no
 * tank keeps a rect at its last known position rather than seeing the whole
 * map.
 *
 * Every case drives ut_make_running_sim and pokes the GameSim directly (the
 * unittests profile permits T2-internal access), then reads the result back
 * through inAnyViewport.
 */

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* serverSimBuildViewports / ViewportRect / view state */
#include "game_sim.h"
#include "tank.h"
#include "pillbox.h"
#include "bases.h"
#include "players.h"
#include "view_policy.h"
#include "test_harness.h"

/* The world centre of a map square. */
static WORLD vp_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
}

/* Hand every pill and base to nobody, so only what a case sets can produce a
 * rect regardless of what the loaded map ships with. */
static void vp_clear_owners(GameSim *gs) {
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

/* Park a live tank on a map square. The death cases set deathWait afterwards. */
static void vp_place_tank(GameSim *gs, BYTE slot, BYTE mx, BYTE my) {
    tankSetWorld(gs, &gs->tanks[slot], vp_world(mx), vp_world(my), 0, false);
    gs->tanks[slot]->deathWait = 0;
}

/* Own a live pillbox at a map square. */
static void vp_place_pill(GameSim *gs, BYTE idx, BYTE owner, BYTE mx, BYTE my) {
    gs->pb->item[idx].owner  = owner;
    gs->pb->item[idx].armour = PILL_MAX_HEALTH;
    gs->pb->item[idx].inTank = FALSE;
    gs->pb->item[idx].x      = mx;
    gs->pb->item[idx].y      = my;
}

/* 1. Stock policies (pill always, base off, ally always) with nobody allied:
 *    the rect set is the tank screen plus each live owned pillbox, and a dead
 *    pillbox contributes nothing. */
int run_view_rects_default_baseline(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "Everard map has no pills (%u) — test needs one",
                  pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");

    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyAlways,
                  "default pill policy is not always (%d)",
                  (int)serverSimGetViewPolicy(sim, viewCategoryPill));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyOff,
                  "default base policy is not off (%d)",
                  (int)serverSimGetViewPolicy(sim, viewCategoryBase));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyAlways,
                  "default ally policy is not always (%d)",
                  (int)serverSimGetViewPolicy(sim, viewCategoryAlly));

    vp_clear_owners(gs);
    vp_place_tank(gs, 0, 50, 50);
    vp_place_pill(gs, 0, 0, 200, 200);

    ViewportRect vps[MAX_VIEWPORTS];
    int n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 2, "expected tank + one owned pill rect, got %d", n);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 50, 50), "tank square not covered");
    UT_ASSERT_MSG(inAnyViewport(vps, n, 200, 200), "owned pill square not covered");
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 125, 125),
                  "midpoint (125,125) should be outside both rects");

    /* A dead pillbox is not viewable, so it grants no rect. */
    gs->pb->item[0].armour = 0;
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 1, "dead pill should leave only the tank rect, got %d", n);
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 200, 200),
                  "dead pill square must not be covered");

    serverSimDestroy(sim);
    return 0;
}

/* 2. viewPolicyAlways for bases and allies: an allied base and an allied live
 *    tank are covered; an enemy base, a neutral base, an enemy tank and a dead
 *    ally are not. */
int run_view_rects_always_base_ally(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 2,
                  "Everard map has < 2 bases (%u) — test needs two",
                  basesGetNumBases(&gs->bs));
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL && gs->tanks[2] != NULL,
                  "slots 0/1/2 need tanks for positioning");

    vp_clear_owners(gs);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) == TRUE,
                  "players 0 and 1 should be allied for this test");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 2) != TRUE,
                  "players 0 and 2 must stay un-allied for the enemy cases");

    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyAlways,
                           VIEW_DECAY_DEFAULT_SECS);

    vp_place_tank(gs, 0, 50, 50);
    vp_place_tank(gs, 1, 200, 50);    /* ally */
    vp_place_tank(gs, 2, 50, 200);    /* enemy */
    gs->bs->item[0].owner = 1;  gs->bs->item[0].x = 200; gs->bs->item[0].y = 200;
    gs->bs->item[1].owner = 2;  gs->bs->item[1].x = 20;  gs->bs->item[1].y = 220;

    ViewportRect vps[MAX_VIEWPORTS];
    int n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 200, 200), "allied base square not covered");
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 20, 220),
                  "enemy base square must not be covered");
    UT_ASSERT_MSG(inAnyViewport(vps, n, 200, 50), "allied tank square not covered");
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 50, 200),
                  "enemy tank square must not be covered");

    /* A neutral base belongs to nobody, so it grants nothing either. */
    gs->bs->item[1].owner = NEUTRAL;
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 20, 220),
                  "neutral base square must not be covered");

    /* An ally waiting out its death does not show its surroundings. */
    gs->tanks[1]->deathWait = 10;
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 200, 50),
                  "dead ally square must not be covered");
    UT_ASSERT_MSG(inAnyViewport(vps, n, 200, 200),
                  "allied base square should still be covered");

    serverSimDestroy(sim);
    return 0;
}

/* 3. viewPolicyOff for all three categories: only the tank screen survives,
 *    with an allied pill, base and tank all present. */
int run_view_rects_off(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1 && basesGetNumBases(&gs->bs) >= 1,
                  "test needs at least one pill and one base");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");

    vp_clear_owners(gs);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);

    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);

    vp_place_tank(gs, 0, 50, 50);
    vp_place_tank(gs, 1, 200, 50);
    vp_place_pill(gs, 0, 0, 200, 200);
    gs->bs->item[0].owner = 1; gs->bs->item[0].x = 20; gs->bs->item[0].y = 220;

    ViewportRect vps[MAX_VIEWPORTS];
    int n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 1, "all categories off should leave one tank rect, got %d", n);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 50, 50), "tank square not covered");
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 200, 200), "owned pill square must not be covered");
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 20, 220), "allied base square must not be covered");
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 200, 50), "allied tank square must not be covered");

    serverSimDestroy(sim);
    return 0;
}

/* 4. viewPolicyDecay: an item is viewable only after the recipient has been
 *    within VIEW_DECAY_NEAR_TILES of it, stays viewable for decaySecs, expires
 *    one tick later, and re-arms on a second approach. Player leave and the
 *    round reset clear the clocks. */
int run_view_rects_decay(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "test needs at least one pill");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");

    const uint32_t armedTick = 1000;
    const uint32_t window = (uint32_t)VIEW_DECAY_MIN_SECS * GAME_NUMTOTALTICKS_SEC;

    vp_clear_owners(gs);
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyDecay, VIEW_DECAY_MIN_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);

    vp_place_tank(gs, 0, 50, 50);
    vp_place_pill(gs, 0, 0, 100, 100);

    ViewportRect vps[MAX_VIEWPORTS];
    int n;

    /* Never approached: the pill is owned and alive but grants no rect. */
    sim->tick = armedTick;
    serverSimUpdateViewDecay(sim);
    UT_ASSERT_MSG(sim->pillNearTick[0][0] == 0,
                  "a far pill should not be stamped (%u)",
                  (unsigned)sim->pillNearTick[0][0]);
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 100, 100),
                  "un-approached pill must not be covered under decay");

    /* Drive past it (5 squares away, inside VIEW_DECAY_NEAR_TILES), then leave. */
    vp_place_tank(gs, 0, 105, 105);
    serverSimUpdateViewDecay(sim);
    UT_ASSERT_MSG(sim->pillNearTick[0][0] == armedTick,
                  "approach should stamp the clock with the sim tick (%u)",
                  (unsigned)sim->pillNearTick[0][0]);
    vp_place_tank(gs, 0, 50, 50);

    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 100, 100),
                  "pill must be covered right after the approach");

    sim->tick = armedTick + window;
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 100, 100),
                  "pill must still be covered at the end of the window");

    sim->tick = armedTick + window + 1;
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 100, 100),
                  "pill must expire one tick past the window");

    /* A second approach re-arms it. */
    vp_place_tank(gs, 0, 105, 105);
    serverSimUpdateViewDecay(sim);
    vp_place_tank(gs, 0, 50, 50);
    UT_ASSERT_MSG(sim->pillNearTick[0][0] == sim->tick,
                  "re-approach should re-stamp the clock (%u vs %u)",
                  (unsigned)sim->pillNearTick[0][0], (unsigned)sim->tick);
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 100, 100),
                  "pill must be covered again after the second approach");

    /* Player leave clears the departing slot's clocks — its own row, and its
     * column in the ally clocks, where it was another player's target. */
    serverSimAddPlayer(sim, 1, "P1", false);
    UT_ASSERT_MSG(gs->tanks[1] != NULL, "slot-1 tank not valid for positioning");
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyDecay, VIEW_DECAY_MIN_SECS);
    /* Both tanks near each other and near the pill, so one update stamps the
     * ally clocks in both directions and slot 1's pill clock. */
    vp_place_tank(gs, 0, 105, 105);
    vp_place_tank(gs, 1, 108, 108);
    serverSimUpdateViewDecay(sim);
    UT_ASSERT_MSG(sim->allyNearTick[0][1] == sim->tick &&
                  sim->allyNearTick[1][0] == sim->tick,
                  "both ally clocks should be stamped (%u / %u)",
                  (unsigned)sim->allyNearTick[0][1],
                  (unsigned)sim->allyNearTick[1][0]);
    UT_ASSERT_MSG(sim->pillNearTick[1][0] != 0,
                  "slot 1 should have stamped the nearby pill");
    /* Give slot 1 a last known position too, so the leave path has one to drop. */
    (void)serverSimBuildViewports(sim, 1, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(sim->lastTankValid[1],
                  "building for slot 1 should record its position");

    serverSimRemovePlayer(sim, 1);
    UT_ASSERT_MSG(sim->allyNearTick[0][1] == 0,
                  "leaver must be cleared from every other slot's ally clocks (%u)",
                  (unsigned)sim->allyNearTick[0][1]);
    UT_ASSERT_MSG(sim->allyNearTick[1][0] == 0 && sim->pillNearTick[1][0] == 0,
                  "leaver's own clocks must be cleared (%u / %u)",
                  (unsigned)sim->allyNearTick[1][0],
                  (unsigned)sim->pillNearTick[1][0]);
    UT_ASSERT_MSG(!sim->lastTankValid[1],
                  "leaver's last known position must be dropped");

    /* The round reset clears every clock and the last known positions. */
    UT_ASSERT_MSG(sim->pillNearTick[0][0] != 0 && sim->lastTankValid[0],
                  "slot 0 should still hold view state before the reset");
    serverSimResetGameWorld(sim);
    UT_ASSERT_MSG(sim->pillNearTick[0][0] == 0,
                  "round reset must clear the pill clocks (%u)",
                  (unsigned)sim->pillNearTick[0][0]);
    UT_ASSERT_MSG(!sim->lastTankValid[0],
                  "round reset must clear the last known positions");

    serverSimDestroy(sim);
    return 0;
}

/* 5. A player whose tank has gone keeps one screen-sized rect where the tank
 *    last was, not the whole map, and a slot that never had a tank gets
 *    nothing. */
int run_view_rects_dead_player(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");

    vp_clear_owners(gs);
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    vp_place_tank(gs, 0, 50, 50);

    ViewportRect vps[MAX_VIEWPORTS];
    int n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 1, "expected one tank rect, got %d", n);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 50, 50), "tank square not covered");

    /* Tank gone: the view is held where it last was. Put it back afterwards so
     * serverSimDestroy tears it down as usual. */
    tank saved = gs->tanks[0];
    gs->tanks[0] = NULL;
    n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 1, "tankless player should get one frozen rect, got %d", n);
    UT_ASSERT_MSG(inAnyViewport(vps, n, 50, 50),
                  "frozen rect should still cover the last known square");
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 200, 200),
                  "a far corner must not be covered — the frozen rect is one screen");

    /* A slot that never had a tank has nothing to freeze. */
    n = serverSimBuildViewports(sim, 1, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 0, "a slot that never had a tank should get no rects, got %d", n);

    gs->tanks[0] = saved;
    serverSimDestroy(sim);
    return 0;
}
