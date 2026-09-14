/*
 * Policy-driven viewport rects (test_view_policy_rects.c).
 *
 * serverSimBuildViewports builds each recipient's visibility set from the
 * server's per-category ViewPolicy: allied pillboxes, allied bases and allied
 * tanks each contribute a screen-sized rect under viewPolicyAlways, nothing by
 * sweep under viewPolicyOff and viewPolicyKey, and under viewPolicyDecay only
 * while the recipient's proximity clock for that item is unexpired. A client
 * with no tank keeps a rect at its last known position rather than seeing the
 * whole map. What a claimed key view adds on top is test_view_state.c's.
 *
 * The same rects decide which pill squares a recipient is shown. A pill inside
 * one of them reports its real square with the position-current bit set; one
 * outside every rect reports the square that recipient was last given, bit
 * clear, while its owner, armour and in-tank flag stay real and keep arriving.
 * Cases 6 to 9 read that back out of a built snapshot and out of the
 * EVENT_PILL_UPDATE the builder reshapes, including the advantage-brain
 * exemption.
 *
 * Every case drives ut_make_running_sim and pokes the GameSim directly (the
 * unittests profile permits T2-internal access), then reads the result back
 * through inAnyViewport.
 */

#include <string.h>

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
    gs->pb->item[idx].armour = PILLS_MAX_ARMOUR;
    gs->pb->item[idx].inTank = FALSE;
    gs->pb->item[idx].x      = mx;
    gs->pb->item[idx].y      = my;
}

/* 1. Pills always, base off, ally always, with nobody allied: the rect set is
 *    the tank screen plus each live owned pillbox, and a dead pillbox
 *    contributes nothing. The case is about what "always" grants, so it sets
 *    the policies rather than leaning on what a fresh sim holds. */
int run_view_rects_default_baseline(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "Everard map has no pills (%u) — test needs one",
                  pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");

    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyAlways,
                           VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff,
                           VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyAlways,
                           VIEW_DECAY_DEFAULT_SECS);
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyAlways,
                  "pill policy is not always (%d)",
                  (int)serverSimGetViewPolicy(sim, viewCategoryPill));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyOff,
                  "base policy is not off (%d)",
                  (int)serverSimGetViewPolicy(sim, viewCategoryBase));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyAlways,
                  "ally policy is not always (%d)",
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
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyAlways,
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

/* Force the full sync that carries the pill block and build one recipient's
 * snapshot. The tank, shell, explosion and base arrays are scratch no case
 * reads, so they live here rather than in each of them. */
static void vp_build_snapshot(ServerSim *sim, BYTE slot, SnapshotHeader *hdr,
                              PillSnapshot *pillsOut, GameEvent *eventsOut) {
    static TankSnapshot tk[MAX_TANKS];
    static ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    static TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    static BaseSnapshot bo[MAX_SNAPSHOT_BASES];

    memset(hdr, 0, sizeof(*hdr));
    sim->lastFullSyncTick[slot] = 0;
    serverSimBuildSnapshot(sim, slot, hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           pillsOut, MAX_SNAPSHOT_PILLS, eventsOut,
                           MAX_SNAPSHOT_EVENTS, false);
}

/* Where a built snapshot carries the EVENT_PILL_UPDATE for pill p, or -1 when
 * it carries none. */
static int vp_find_pill_event(const GameEvent *ev, int count, BYTE p) {
    int i;
    for (i = 0; i < count; i++) {
        if (ev[i].type == EVENT_PILL_UPDATE && ev[i].data[0] == p) return i;
    }
    return -1;
}

/* 6. A pill inside the recipient's rects reports its real square with the
 *    position-current bit set; one outside every rect reports the bit clear and
 *    the square that recipient was last given, while its owner, armour and
 *    in-tank flag stay live and keep arriving. */
int run_view_pill_pos_current(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 2,
                  "the map carries %u pills; this case needs two",
                  (unsigned)pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");

    vp_clear_owners(gs);
    vp_place_tank(gs, 0, 50, 50);
    vp_place_tank(gs, 1, 200, 50);
    vp_place_pill(gs, 0, NEUTRAL, 55, 55);
    vp_place_pill(gs, 1, NEUTRAL, 60, 60);
    serverSimPillShadowTick(sim);

    /* Pill 1 leaves slot 0's screen. Its record keeps 60,60 — the last square
     * slot 0 was given — while the live pill stands at 200,200. */
    gs->pb->item[1].x = 200;
    gs->pb->item[1].y = 200;
    serverSimPillShadowTick(sim);

    SnapshotHeader hdr;
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    vp_build_snapshot(sim, 0, &hdr, po, ev);
    UT_ASSERT_MSG(hdr.pillCount >= 2,
                  "the full sync carried %u pill entries, this case needs two",
                  (unsigned)hdr.pillCount);
    UT_ASSERT_MSG(po[0].x == 55 && po[0].y == 55,
                  "the pill in view reports %u,%u, not its real square 55,55",
                  (unsigned)po[0].x, (unsigned)po[0].y);
    UT_ASSERT_MSG(pillPosCurrentFromByte(po[0].pillFlags),
                  "the pill in view is not marked position-current");
    UT_ASSERT_MSG(po[1].x == 60 && po[1].y == 60,
                  "the pill out of view reports %u,%u, not the square slot 0 "
                  "was last given, 60,60",
                  (unsigned)po[1].x, (unsigned)po[1].y);
    UT_ASSERT_MSG(!pillPosCurrentFromByte(po[1].pillFlags),
                  "the pill out of view must not be marked position-current");

    /* Everything but the square is public and keeps updating while the pill is
     * unseen: someone picks it up damaged and slot 0 is told all of it. */
    gs->pb->item[1].owner  = 1;
    gs->pb->item[1].armour = 7;
    gs->pb->item[1].inTank = TRUE;
    serverSimPillShadowTick(sim);
    vp_build_snapshot(sim, 0, &hdr, po, ev);
    UT_ASSERT_MSG(po[1].owner == 1,
                  "the unseen pill's owner reports %u, not 1",
                  (unsigned)po[1].owner);
    UT_ASSERT_MSG(po[1].armour == 7,
                  "the unseen pill's armour reports %u, not 7",
                  (unsigned)po[1].armour);
    UT_ASSERT_MSG(pillInTankFromByte(po[1].pillFlags),
                  "the unseen pill's in-tank flag did not arrive");
    UT_ASSERT_MSG(po[1].x == 60 && po[1].y == 60 &&
                      !pillPosCurrentFromByte(po[1].pillFlags),
                  "the unseen pill's square leaked as %u,%u",
                  (unsigned)po[1].x, (unsigned)po[1].y);

    serverSimDestroy(sim);
    return 0;
}

/* 7. A pill that moves while nobody is watching keeps reporting the square it
 *    was last seen on; bring the recipient's screen over its new square and the
 *    real one arrives, marked current. */
int run_view_pill_pos_reveal(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "this case needs one pill");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");

    vp_clear_owners(gs);
    vp_place_tank(gs, 0, 50, 50);
    vp_place_pill(gs, 0, NEUTRAL, 55, 55);
    serverSimPillShadowTick(sim);

    gs->pb->item[0].x = 200;
    gs->pb->item[0].y = 200;
    serverSimPillShadowTick(sim);

    SnapshotHeader hdr;
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    vp_build_snapshot(sim, 0, &hdr, po, ev);
    UT_ASSERT_MSG(po[0].x == 55 && po[0].y == 55 &&
                      !pillPosCurrentFromByte(po[0].pillFlags),
                  "a pill that moved unseen reports %u,%u — it should still be "
                  "the withheld 55,55", (unsigned)po[0].x, (unsigned)po[0].y);

    /* Drive over there: the square enters the recipient's screen and corrects
     * itself, exactly as a stale tile does. */
    vp_place_tank(gs, 0, 200, 200);
    serverSimPillShadowTick(sim);
    UT_ASSERT_MSG(sim->clientKnownPillX[0][0] == 200 &&
                      sim->clientKnownPillY[0][0] == 200,
                  "the record still holds %u,%u after the pill came into view",
                  (unsigned)sim->clientKnownPillX[0][0],
                  (unsigned)sim->clientKnownPillY[0][0]);

    vp_build_snapshot(sim, 0, &hdr, po, ev);
    UT_ASSERT_MSG(po[0].x == 200 && po[0].y == 200,
                  "the pill now in view reports %u,%u, not 200,200",
                  (unsigned)po[0].x, (unsigned)po[0].y);
    UT_ASSERT_MSG(pillPosCurrentFromByte(po[0].pillFlags),
                  "the pill now in view is not marked position-current");

    serverSimDestroy(sim);
    return 0;
}

/* 8. EVENT_PILL_UPDATE through the builder's per-recipient filter: the
 *    recipient who can see the pill gets the real square marked current, the
 *    one who cannot gets its own square with the bit clear — and still gets the
 *    event, because it is the only carrier for armour, owner and the in-tank
 *    flag between full syncs. */
int run_view_pill_update_event_fogged(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "this case needs one pill");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");

    vp_clear_owners(gs);
    vp_place_tank(gs, 0, 50, 50);
    vp_place_tank(gs, 1, 200, 200);
    vp_place_pill(gs, 0, NEUTRAL, 205, 205);   /* slot 1 sees it, slot 0 does not */
    serverSimPillShadowTick(sim);

    /* It is carried onto slot 0's screen and drops damaged. Slot 0 is shown the
     * new square; slot 1 keeps the one it was given. */
    gs->pb->item[0].x      = 55;
    gs->pb->item[0].y      = 55;
    gs->pb->item[0].armour = 9;
    serverSimPillShadowTick(sim);

    GameEvent move;
    move.type = EVENT_PILL_UPDATE;
    memset(move.data, 0, sizeof(move.data));
    move.data[0] = 0;
    move.data[1] = 55;
    move.data[2] = 55;
    move.data[3] = gs->pb->item[0].owner;
    move.data[4] = pillSetInTank(0, false);
    move.data[5] = 9;
    serverSimAddEvent(sim, &move);

    SnapshotHeader hdr;
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];
    int idx;

    vp_build_snapshot(sim, 0, &hdr, po, ev);
    idx = vp_find_pill_event(ev, hdr.reliableEventCount, 0);
    UT_ASSERT_MSG(idx >= 0, "the watching recipient was sent no pill event");
    UT_ASSERT_MSG(ev[idx].data[1] == 55 && ev[idx].data[2] == 55,
                  "the watching recipient's event carries %u,%u, not 55,55",
                  (unsigned)ev[idx].data[1], (unsigned)ev[idx].data[2]);
    UT_ASSERT_MSG(pillPosCurrentFromByte(ev[idx].data[4]),
                  "the watching recipient's event is not marked position-current");

    vp_build_snapshot(sim, 1, &hdr, po, ev);
    idx = vp_find_pill_event(ev, hdr.reliableEventCount, 0);
    UT_ASSERT_MSG(idx >= 0,
                  "the event was dropped for the recipient that cannot see the "
                  "pill — it carries armour and owner too");
    UT_ASSERT_MSG(ev[idx].data[1] == 205 && ev[idx].data[2] == 205,
                  "the unseeing recipient's event carries %u,%u, not the square "
                  "it was last given, 205,205",
                  (unsigned)ev[idx].data[1], (unsigned)ev[idx].data[2]);
    UT_ASSERT_MSG(!pillPosCurrentFromByte(ev[idx].data[4]),
                  "the unseeing recipient's event claims a current position");
    UT_ASSERT_MSG(ev[idx].data[5] == 9,
                  "the unseeing recipient's event lost the armour (%u, not 9)",
                  (unsigned)ev[idx].data[5]);
    UT_ASSERT_MSG(ev[idx].data[3] == gs->pb->item[0].owner,
                  "the unseeing recipient's event lost the owner");

    serverSimDestroy(sim);
    return 0;
}

/* 9. The advantage settings promise a brain the location of every pillbox on
 *    the map, and that promise is served out of the bot's own client data — so
 *    an aiYesAdvantage or aiFull bot is sent every pill's real square whatever
 *    its rects hold. A plain computer player is fogged like a human. */
int run_view_pill_pos_bot_advantage(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "Bot", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "this case needs one pill");
    UT_ASSERT_MSG(gs->tanks[1] != NULL, "slot-1 tank not valid for positioning");

    /* A real bot needs a brain file. serverSimIsBot answers yes to a live bot
     * pool entry or to a roster seat marked as a bot; the pool flag is the
     * half set here, and the exemption reads the .ai beside it. */
    sim->botMgr.bots[1].active = true;
    sim->botMgr.bots[1].ai     = aiYes;
    UT_ASSERT_MSG(serverSimIsBot(sim, 1), "slot 1 is not seen as a bot");

    vp_clear_owners(gs);
    vp_place_tank(gs, 0, 50, 50);
    vp_place_tank(gs, 1, 50, 50);
    vp_place_pill(gs, 0, NEUTRAL, 55, 55);
    serverSimPillShadowTick(sim);

    gs->pb->item[0].x = 200;
    gs->pb->item[0].y = 200;
    serverSimPillShadowTick(sim);

    SnapshotHeader hdr;
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    vp_build_snapshot(sim, 1, &hdr, po, ev);
    UT_ASSERT_MSG(po[0].x == 55 && po[0].y == 55 &&
                      !pillPosCurrentFromByte(po[0].pillFlags),
                  "a plain computer player was shown %u,%u — it should get the "
                  "same fog a human does", (unsigned)po[0].x, (unsigned)po[0].y);

    sim->botMgr.bots[1].ai = aiYesAdvantage;
    serverSimPillShadowTick(sim);
    vp_build_snapshot(sim, 1, &hdr, po, ev);
    UT_ASSERT_MSG(po[0].x == 200 && po[0].y == 200,
                  "an advantage bot was shown %u,%u, not the pill's real square "
                  "200,200", (unsigned)po[0].x, (unsigned)po[0].y);
    UT_ASSERT_MSG(pillPosCurrentFromByte(po[0].pillFlags),
                  "an advantage bot's pill is not marked position-current");

    gs->pb->item[0].x = 20;
    gs->pb->item[0].y = 220;
    sim->botMgr.bots[1].ai = aiFull;
    serverSimPillShadowTick(sim);
    vp_build_snapshot(sim, 1, &hdr, po, ev);
    UT_ASSERT_MSG(po[0].x == 20 && po[0].y == 220 &&
                      pillPosCurrentFromByte(po[0].pillFlags),
                  "an aiFull bot was shown %u,%u, not 20,220",
                  (unsigned)po[0].x, (unsigned)po[0].y);

    sim->botMgr.bots[1].active = false;
    serverSimDestroy(sim);
    return 0;
}
