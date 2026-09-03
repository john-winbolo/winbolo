/*
 * Item-view cycling helpers (test_view_cycling.c).
 *
 * The client's pill view generalises to three kinds of item view. These cases
 * cover the two new sets of cycling helpers as pure logic — basesGetNextView /
 * basesMoveView over allied bases, playersGetNextAllyView /
 * playersMoveAllyView over allied live tanks — plus the per-tick upkeep
 * (viewportUpdateItemView) that drops a view once its item stops qualifying.
 *
 * Every case drives ut_make_running_sim and pokes the GameSim directly (the
 * unittests profile permits T2-internal access). The bases cases set their own
 * base count and coordinates so the map's own layout does not decide the
 * answer; the ally cases write the last-known positions the client normally
 * gets from the snapshot render path.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "client_command.h"        /* VIEW_KIND_* */
#include "client_sim_internal.h"   /* struct ViewPort */
#include "viewport.h"
#include "scroll.h"
#include "tank.h"
#include "bases.h"
#include "players.h"
#include "test_harness.h"

/* The world centre of a map square. */
static WORLD vc_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
}

/* Lay out a known set of bases, replacing whatever the map shipped with. */
static void vc_set_bases(GameSim *gs, BYTE count) {
    BYTE i;

    basesSetNumBases(&gs->bs, count);
    for (i = 0; i < count; i++) {
        gs->bs->item[i].owner  = NEUTRAL;
        gs->bs->item[i].x      = 0;
        gs->bs->item[i].y      = 0;
        gs->bs->item[i].armour = BASE_FULL_ARMOUR;
    }
}

static void vc_place_base(GameSim *gs, BYTE idx, BYTE owner, BYTE mx, BYTE my) {
    gs->bs->item[idx].owner = owner;
    gs->bs->item[idx].x     = mx;
    gs->bs->item[idx].y     = my;
}

/* Park a live tank on a map square and record it as the slot's last known
 * position — the client reads the players struct, the server the tank. */
static void vc_place_tank(GameSim *gs, BYTE slot, BYTE mx, BYTE my) {
    tankSetWorld(gs, &gs->tanks[slot], vc_world(mx), vc_world(my), 0, false);
    gs->tanks[slot]->deathWait = 0;
    (*gs->plyrs).item[slot].mapX = mx;
    (*gs->plyrs).item[slot].mapY = my;
}

/* The alive-tank mask the ally helpers take, built the way a caller holding
 * real tanks builds it (the client builds the same thing from interp). */
static PlayerBitMap vc_alive_mask(GameSim *gs) {
    PlayerBitMap mask = 0;
    BYTE i;

    for (i = 0; i < MAX_TANKS; i++) {
        if (gs->tanks[i] != NULL && tankGetDeathWait(&gs->tanks[i]) == 0) {
            mask |= (PlayerBitMap)1 << i;
        }
    }
    return mask;
}

/* 1. basesGetNextView walks the allied bases in index order and wraps, skips
 *    enemy and neutral bases, and reports FALSE when none qualify. */
int run_view_cycle_bases(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    gs->viewPlayer = 0;

    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) == TRUE,
                  "players 0 and 1 should be allied for this test");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 2) != TRUE,
                  "players 0 and 2 must stay un-allied for the enemy case");

    /* Four bases: ours, an enemy's, an ally's, and a neutral one. */
    vc_set_bases(gs, 4);
    vc_place_base(gs, 0, 0,       40, 40);
    vc_place_base(gs, 1, 2,       60, 40);
    vc_place_base(gs, 2, 1,       80, 40);
    vc_place_base(gs, 3, NEUTRAL, 100, 40);

    BYTE mx = 0, my = 0;
    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, &mx, &my, FALSE) == TRUE,
                  "a first allied base should be found");
    UT_ASSERT_MSG(mx == 40 && my == 40,
                  "first view should be our own base, got (%u,%u)", mx, my);

    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, &mx, &my, TRUE) == TRUE,
                  "cycling from base 0 should find the next allied base");
    UT_ASSERT_MSG(mx == 80 && my == 40,
                  "the enemy and neutral bases should be skipped, got (%u,%u)",
                  mx, my);

    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, &mx, &my, TRUE) == TRUE,
                  "cycling past the last allied base should wrap");
    UT_ASSERT_MSG(mx == 40 && my == 40,
                  "the wrap should land back on base 0, got (%u,%u)", mx, my);

    /* The ally's base changing hands leaves only our own in the cycle. */
    vc_place_base(gs, 2, 2, 80, 40);
    mx = 40; my = 40;
    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, &mx, &my, TRUE) == TRUE,
                  "our own base should still be viewable");
    UT_ASSERT_MSG(mx == 40 && my == 40,
                  "the only allied base left is ours, got (%u,%u)", mx, my);

    /* Hand every base away: nothing qualifies at all. */
    vc_place_base(gs, 0, 2, 40, 40);
    mx = 0; my = 0;
    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, &mx, &my, FALSE) == FALSE,
                  "no allied base should report FALSE");

    serverSimDestroy(sim);
    return 0;
}

/* 2. basesMoveView takes the nearest qualifying base in the pressed direction,
 *    on both axes, and never one that is merely nearer the other way — the
 *    guard against the loose-parenthesis direction filter. */
int run_view_cycle_base_direction(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    gs->viewPlayer = 0;

    /* All ours, laid out around the base we start on at (100,100):
     *  1 is 2 squares left (the decoy — nearest of all, wrong way for a
     *    rightward press), 2 is 10 right, 3 is 20 right,
     *  4 is 2 squares up (decoy for a downward press), 5 is 10 down. */
    vc_set_bases(gs, 6);
    vc_place_base(gs, 0, 0, 100, 100);
    vc_place_base(gs, 1, 0,  98, 100);
    vc_place_base(gs, 2, 0, 110, 100);
    vc_place_base(gs, 3, 0, 120, 100);
    vc_place_base(gs, 4, 0, 100,  98);
    vc_place_base(gs, 5, 0, 100, 110);

    BYTE mx = 100, my = 100;
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, &mx, &my, 1, 0) == TRUE,
                  "a base to the right should be found");
    UT_ASSERT_MSG(mx == 110 && my == 100,
                  "right should take the nearest base to the right, got (%u,%u)",
                  mx, my);

    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, &mx, &my, 1, 0) == TRUE,
                  "a further base to the right should be found");
    UT_ASSERT_MSG(mx == 120 && my == 100,
                  "right again should step on, got (%u,%u)", mx, my);

    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, &mx, &my, 1, 0) == FALSE,
                  "there is nothing further right");

    mx = 100; my = 100;
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, &mx, &my, -1, 0) == TRUE,
                  "a base to the left should be found");
    UT_ASSERT_MSG(mx == 98 && my == 100,
                  "left should take the base to the left, got (%u,%u)", mx, my);

    mx = 100; my = 100;
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, &mx, &my, 0, 1) == TRUE,
                  "a base below should be found");
    UT_ASSERT_MSG(mx == 100 && my == 110,
                  "down must not pick the nearer base above, got (%u,%u)",
                  mx, my);

    mx = 100; my = 100;
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, &mx, &my, 0, -1) == TRUE,
                  "a base above should be found");
    UT_ASSERT_MSG(mx == 100 && my == 98,
                  "up should take the base above, got (%u,%u)", mx, my);

    /* An enemy base in the pressed direction is not a candidate. */
    vc_place_base(gs, 2, 3, 110, 100);
    vc_place_base(gs, 3, 3, 120, 100);
    mx = 100; my = 100;
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, &mx, &my, 1, 0) == FALSE,
                  "only enemy bases to the right should report FALSE");

    serverSimDestroy(sim);
    return 0;
}

/* 3. playersGetNextAllyView walks the allied live tanks and wraps, skipping
 *    ourselves, a dead ally and a player we are not allied to. */
int run_view_cycle_allies(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);
    serverSimAddPlayer(sim, 3, "P3", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL &&
                  gs->tanks[2] != NULL && gs->tanks[3] != NULL,
                  "slots 0..3 need tanks for positioning");
    gs->viewPlayer = 0;

    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 3, TRUE);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) == TRUE &&
                  playersIsAllie(&gs->plyrs, 0, 3) == TRUE,
                  "players 1 and 3 should be allied to player 0");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 2) != TRUE,
                  "player 2 must stay un-allied for the enemy case");

    vc_place_tank(gs, 0,  50,  50);
    vc_place_tank(gs, 1, 100,  50);
    vc_place_tank(gs, 2, 150,  50);
    vc_place_tank(gs, 3, 200,  50);

    PlayerBitMap alive = vc_alive_mask(gs);
    BYTE target = 0, mx = 0, my = 0;

    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, &target, &mx, &my, FALSE) == TRUE,
                  "a first ally should be found");
    UT_ASSERT_MSG(target == 1, "own slot should be skipped, got %u", target);
    UT_ASSERT_MSG(mx == 100 && my == 50,
                  "the ally's last known square should come back, got (%u,%u)",
                  mx, my);

    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, &target, &mx, &my, TRUE) == TRUE,
                  "cycling from ally 1 should find the next ally");
    UT_ASSERT_MSG(target == 3, "the un-allied player 2 should be skipped, got %u",
                  target);

    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, &target, &mx, &my, TRUE) == TRUE,
                  "cycling past the last ally should wrap");
    UT_ASSERT_MSG(target == 1, "the wrap should land back on ally 1, got %u", target);

    /* An ally waiting out its death is not watchable. */
    gs->tanks[1]->deathWait = 10;
    alive = vc_alive_mask(gs);
    target = 0;
    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, &target, &mx, &my, FALSE) == TRUE,
                  "the surviving ally should still be found");
    UT_ASSERT_MSG(target == 3, "the dead ally should be skipped, got %u", target);
    gs->tanks[1]->deathWait = 0;

    /* Leaving the alliance empties the cycle. */
    playersLeaveAlliance(gs, &gs->plyrs, 0, 1, TRUE);
    playersLeaveAlliance(gs, &gs->plyrs, 0, 3, TRUE);
    alive = vc_alive_mask(gs);
    target = 0;
    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, &target, &mx, &my, FALSE) == FALSE,
                  "no allies should report FALSE");

    serverSimDestroy(sim);
    return 0;
}

/* 4. playersMoveAllyView takes the nearest ally in the pressed direction, on
 *    both axes, with a decoy nearer the other way. */
int run_view_cycle_ally_direction(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);
    serverSimAddPlayer(sim, 3, "P3", false);
    serverSimAddPlayer(sim, 4, "P4", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[4] != NULL, "slots 0..4 need tanks for positioning");
    gs->viewPlayer = 0;

    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 2, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 3, TRUE);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 4, TRUE);

    /* Watching ally 1 at (100,100): 2 is 2 squares left (the decoy), 3 is 10
     * right, 4 is 2 squares up (decoy for a downward press). */
    vc_place_tank(gs, 0,  10,  10);
    vc_place_tank(gs, 1, 100, 100);
    vc_place_tank(gs, 2,  98, 100);
    vc_place_tank(gs, 3, 110, 100);
    vc_place_tank(gs, 4, 100,  98);

    PlayerBitMap alive = vc_alive_mask(gs);
    BYTE target = 1, mx = 100, my = 100;

    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, &target, &mx, &my, 1, 0) == TRUE,
                  "an ally to the right should be found");
    UT_ASSERT_MSG(target == 3, "right must not pick the nearer ally left, got %u",
                  target);
    UT_ASSERT_MSG(mx == 110 && my == 100,
                  "the chosen ally's square should come back, got (%u,%u)", mx, my);

    target = 1; mx = 100; my = 100;
    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, &target, &mx, &my, -1, 0) == TRUE,
                  "an ally to the left should be found");
    UT_ASSERT_MSG(target == 2, "left should take the ally to the left, got %u",
                  target);

    /* Move ally 3 below us: down must take it, not the nearer ally above. */
    vc_place_tank(gs, 3, 100, 110);
    target = 1; mx = 100; my = 100;
    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, &target, &mx, &my, 0, 1) == TRUE,
                  "an ally below should be found");
    UT_ASSERT_MSG(target == 3, "down must not pick the nearer ally above, got %u",
                  target);

    target = 1; mx = 100; my = 100;
    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, &target, &mx, &my, 0, -1) == TRUE,
                  "an ally above should be found");
    UT_ASSERT_MSG(target == 4, "up should take the ally above, got %u", target);

    /* With the only ally to the right dead, right finds nothing. */
    vc_place_tank(gs, 3, 110, 100);
    gs->tanks[3]->deathWait = 10;
    alive = vc_alive_mask(gs);
    target = 1; mx = 100; my = 100;
    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, &target, &mx, &my, 1, 0) == FALSE,
                  "only a dead ally to the right should report FALSE");

    serverSimDestroy(sim);
    return 0;
}

/* 5. viewportUpdateItemView is the per-tick upkeep behind the view exits: a
 *    base view survives while the base stays allied and drops when it is
 *    captured or goes neutral; an ally view follows its target as it drives
 *    and drops when the ally dies or leaves the alliance. */
int run_view_cycle_exits(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    gs->viewPlayer = 0;
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);

    ViewPort vp;
    ScrollState scroll;
    memset(&vp, 0, sizeof(vp));
    memset(&scroll, 0, sizeof(scroll));

    /* Tank view has nothing to check. */
    vp.viewKind = VIEW_KIND_TANK;
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, 0) == TRUE,
                  "the tank view should always survive the check");

    /* Base view on an allied base. */
    vc_set_bases(gs, 2);
    vc_place_base(gs, 0, 1, 80, 80);
    vc_place_base(gs, 1, 0, 40, 40);
    vp.viewKind   = VIEW_KIND_BASE;
    vp.viewTarget = 0;
    vp.viewX      = 80;
    vp.viewY      = 80;
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, 0) == TRUE,
                  "an allied base should stay watchable");

    gs->bs->item[0].owner = 2;   /* captured by an enemy */
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, 0) == FALSE,
                  "a captured base must drop the view");

    gs->bs->item[0].owner = NEUTRAL;
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, 0) == FALSE,
                  "a neutral base must drop the view");

    /* Ally view follows its target. */
    vc_place_tank(gs, 0,  50,  50);
    vc_place_tank(gs, 1, 100, 100);
    PlayerBitMap alive = vc_alive_mask(gs);

    vp.viewKind   = VIEW_KIND_ALLY;
    vp.viewTarget = 1;
    vp.viewX      = 100;
    vp.viewY      = 100;
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, alive) == TRUE,
                  "a live ally should stay watchable");

    vc_place_tank(gs, 1, 120, 130);
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, alive) == TRUE,
                  "the ally view should survive the ally moving");
    UT_ASSERT_MSG(vp.viewX == 120 && vp.viewY == 130,
                  "the ally view should re-centre on the ally, got (%u,%u)",
                  vp.viewX, vp.viewY);

    gs->tanks[1]->deathWait = 10;
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, vc_alive_mask(gs)) == FALSE,
                  "a dead ally must drop the view");
    gs->tanks[1]->deathWait = 0;

    playersLeaveAlliance(gs, &gs->plyrs, 0, 1, TRUE);
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, vc_alive_mask(gs)) == FALSE,
                  "an ally who has left the alliance must drop the view");

    serverSimDestroy(sim);
    return 0;
}
