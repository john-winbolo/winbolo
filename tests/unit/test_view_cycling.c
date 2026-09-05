/*
 * Item-view cycling helpers (test_view_cycling.c).
 *
 * The client's pill view generalises to three kinds of item view. These cases
 * cover the two new sets of cycling helpers as pure logic — basesGetNextView /
 * basesMoveView over allied bases, playersGetNextAllyView /
 * playersMoveAllyView over allied live tanks — plus the per-tick upkeep
 * (viewportUpdateItemView) that drops a view once its item stops qualifying.
 *
 * Three more cover what a decay policy adds to that. The eligibility mask
 * (clientSimViewEligibleMask) turns the client's own proximity clocks into a
 * bit per item, and the cycling steps over an item whose bit is clear exactly
 * as it steps over one that does not qualify; under the other three policies
 * the mask is every bit, so those cycle as they always have. The last one is
 * the ally view's side of it: a tank the server is not sending arrives as a
 * hidden stub with its players entry zeroed, so the view centres on the square
 * the client last saw it on, and gives up on it once the stub run outlasts the
 * grace. The last one is the pill equivalent: a pill whose square the client
 * has not been told is current has stopped being solid, and must still be
 * cyclable.
 *
 * Every case drives ut_make_running_sim and pokes the GameSim directly (the
 * unittests profile permits T2-internal access). The bases cases set their own
 * base count and coordinates so the map's own layout does not decide the
 * answer; the ally cases write the last-known positions the client normally
 * gets from the snapshot render path. The two ClientSim cases need no server
 * at all — the mask and the grace read fields the test sets by hand.
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
#include "pillbox.h"
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

/* Every item selectable, which is what the eligibility mask reads as under
 * every policy but viewPolicyDecay. */
#define VC_ALL_ELIGIBLE (~(PlayerBitMap)0)

/* Lay out a known set of the local player's own pills, replacing whatever the
 * map shipped with. All alive, none carried, so only the eligibility mask
 * decides what the cycle finds. */
static void vc_set_pills(GameSim *gs, BYTE owner, BYTE count) {
    BYTE i;

    pillsSetNumPills(&gs->pb, count);
    for (i = 0; i < count; i++) {
        gs->pb->item[i].owner  = owner;
        gs->pb->item[i].armour = PILLBOX_15;
        gs->pb->item[i].inTank = FALSE;
        gs->pb->item[i].x      = (BYTE)(20 + i * 10);
        gs->pb->item[i].y      = 20;
    }
}

/* The per-tick upkeep with nothing but an alive mask to say — the shape every
 * case here used before the cycling took an inputs struct. */
static bool vc_update(ViewPort *vp, GameSim *gs, ScrollState *scroll,
                      PlayerBitMap alive) {
    ViewCycleInputs in;

    viewCycleInputsDefaults(&in);
    in.allyViewable = alive;
    return viewportUpdateItemView(vp, gs, scroll, &in);
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
    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, FALSE) == TRUE,
                  "a first allied base should be found");
    UT_ASSERT_MSG(mx == 40 && my == 40,
                  "first view should be our own base, got (%u,%u)", mx, my);

    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, TRUE) == TRUE,
                  "cycling from base 0 should find the next allied base");
    UT_ASSERT_MSG(mx == 80 && my == 40,
                  "the enemy and neutral bases should be skipped, got (%u,%u)",
                  mx, my);

    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, TRUE) == TRUE,
                  "cycling past the last allied base should wrap");
    UT_ASSERT_MSG(mx == 40 && my == 40,
                  "the wrap should land back on base 0, got (%u,%u)", mx, my);

    /* The ally's base changing hands leaves only our own in the cycle. */
    vc_place_base(gs, 2, 2, 80, 40);
    mx = 40; my = 40;
    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, TRUE) == TRUE,
                  "our own base should still be viewable");
    UT_ASSERT_MSG(mx == 40 && my == 40,
                  "the only allied base left is ours, got (%u,%u)", mx, my);

    /* Hand every base away: nothing qualifies at all. */
    vc_place_base(gs, 0, 2, 40, 40);
    mx = 0; my = 0;
    UT_ASSERT_MSG(basesGetNextView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, FALSE) == FALSE,
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
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, 1, 0) == TRUE,
                  "a base to the right should be found");
    UT_ASSERT_MSG(mx == 110 && my == 100,
                  "right should take the nearest base to the right, got (%u,%u)",
                  mx, my);

    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, 1, 0) == TRUE,
                  "a further base to the right should be found");
    UT_ASSERT_MSG(mx == 120 && my == 100,
                  "right again should step on, got (%u,%u)", mx, my);

    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, 1, 0) == FALSE,
                  "there is nothing further right");

    mx = 100; my = 100;
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, -1, 0) == TRUE,
                  "a base to the left should be found");
    UT_ASSERT_MSG(mx == 98 && my == 100,
                  "left should take the base to the left, got (%u,%u)", mx, my);

    mx = 100; my = 100;
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, 0, 1) == TRUE,
                  "a base below should be found");
    UT_ASSERT_MSG(mx == 100 && my == 110,
                  "down must not pick the nearer base above, got (%u,%u)",
                  mx, my);

    mx = 100; my = 100;
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, 0, -1) == TRUE,
                  "a base above should be found");
    UT_ASSERT_MSG(mx == 100 && my == 98,
                  "up should take the base above, got (%u,%u)", mx, my);

    /* An enemy base in the pressed direction is not a candidate. */
    vc_place_base(gs, 2, 3, 110, 100);
    vc_place_base(gs, 3, 3, 120, 100);
    mx = 100; my = 100;
    UT_ASSERT_MSG(basesMoveView(gs, &gs->bs, VC_ALL_ELIGIBLE, &mx, &my, 1, 0) == FALSE,
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

    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, FALSE) == TRUE,
                  "a first ally should be found");
    UT_ASSERT_MSG(target == 1, "own slot should be skipped, got %u", target);
    UT_ASSERT_MSG(mx == 100 && my == 50,
                  "the ally's last known square should come back, got (%u,%u)",
                  mx, my);

    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, TRUE) == TRUE,
                  "cycling from ally 1 should find the next ally");
    UT_ASSERT_MSG(target == 3, "the un-allied player 2 should be skipped, got %u",
                  target);

    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, TRUE) == TRUE,
                  "cycling past the last ally should wrap");
    UT_ASSERT_MSG(target == 1, "the wrap should land back on ally 1, got %u", target);

    /* An ally waiting out its death is not watchable. */
    gs->tanks[1]->deathWait = 10;
    alive = vc_alive_mask(gs);
    target = 0;
    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, FALSE) == TRUE,
                  "the surviving ally should still be found");
    UT_ASSERT_MSG(target == 3, "the dead ally should be skipped, got %u", target);
    gs->tanks[1]->deathWait = 0;

    /* Leaving the alliance empties the cycle. */
    playersLeaveAlliance(gs, &gs->plyrs, 0, 1, TRUE);
    playersLeaveAlliance(gs, &gs->plyrs, 0, 3, TRUE);
    alive = vc_alive_mask(gs);
    target = 0;
    UT_ASSERT_MSG(playersGetNextAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, FALSE) == FALSE,
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

    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, 1, 0) == TRUE,
                  "an ally to the right should be found");
    UT_ASSERT_MSG(target == 3, "right must not pick the nearer ally left, got %u",
                  target);
    UT_ASSERT_MSG(mx == 110 && my == 100,
                  "the chosen ally's square should come back, got (%u,%u)", mx, my);

    target = 1; mx = 100; my = 100;
    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, -1, 0) == TRUE,
                  "an ally to the left should be found");
    UT_ASSERT_MSG(target == 2, "left should take the ally to the left, got %u",
                  target);

    /* Move ally 3 below us: down must take it, not the nearer ally above. */
    vc_place_tank(gs, 3, 100, 110);
    target = 1; mx = 100; my = 100;
    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, 0, 1) == TRUE,
                  "an ally below should be found");
    UT_ASSERT_MSG(target == 3, "down must not pick the nearer ally above, got %u",
                  target);

    target = 1; mx = 100; my = 100;
    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, 0, -1) == TRUE,
                  "an ally above should be found");
    UT_ASSERT_MSG(target == 4, "up should take the ally above, got %u", target);

    /* With the only ally to the right dead, right finds nothing. */
    vc_place_tank(gs, 3, 110, 100);
    gs->tanks[3]->deathWait = 10;
    alive = vc_alive_mask(gs);
    target = 1; mx = 100; my = 100;
    UT_ASSERT_MSG(playersMoveAllyView(gs, alive, VC_ALL_ELIGIBLE, &target, &mx, &my, 1, 0) == FALSE,
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
    UT_ASSERT_MSG(vc_update(&vp, gs, &scroll, 0) == TRUE,
                  "the tank view should always survive the check");

    /* Base view on an allied base. */
    vc_set_bases(gs, 2);
    vc_place_base(gs, 0, 1, 80, 80);
    vc_place_base(gs, 1, 0, 40, 40);
    vp.viewKind   = VIEW_KIND_BASE;
    vp.viewTarget = 0;
    vp.viewX      = 80;
    vp.viewY      = 80;
    UT_ASSERT_MSG(vc_update(&vp, gs, &scroll, 0) == TRUE,
                  "an allied base should stay watchable");

    gs->bs->item[0].owner = 2;   /* captured by an enemy */
    UT_ASSERT_MSG(vc_update(&vp, gs, &scroll, 0) == FALSE,
                  "a captured base must drop the view");

    gs->bs->item[0].owner = NEUTRAL;
    UT_ASSERT_MSG(vc_update(&vp, gs, &scroll, 0) == FALSE,
                  "a neutral base must drop the view");

    /* Ally view follows its target. */
    vc_place_tank(gs, 0,  50,  50);
    vc_place_tank(gs, 1, 100, 100);
    PlayerBitMap alive = vc_alive_mask(gs);

    vp.viewKind   = VIEW_KIND_ALLY;
    vp.viewTarget = 1;
    vp.viewX      = 100;
    vp.viewY      = 100;
    UT_ASSERT_MSG(vc_update(&vp, gs, &scroll, alive) == TRUE,
                  "a live ally should stay watchable");

    vc_place_tank(gs, 1, 120, 130);
    UT_ASSERT_MSG(vc_update(&vp, gs, &scroll, alive) == TRUE,
                  "the ally view should survive the ally moving");
    UT_ASSERT_MSG(vp.viewX == 120 && vp.viewY == 130,
                  "the ally view should re-centre on the ally, got (%u,%u)",
                  vp.viewX, vp.viewY);

    gs->tanks[1]->deathWait = 10;
    UT_ASSERT_MSG(vc_update(&vp, gs, &scroll, vc_alive_mask(gs)) == FALSE,
                  "a dead ally must drop the view");
    gs->tanks[1]->deathWait = 0;

    playersLeaveAlliance(gs, &gs->plyrs, 0, 1, TRUE);
    UT_ASSERT_MSG(vc_update(&vp, gs, &scroll, vc_alive_mask(gs)) == FALSE,
                  "an ally who has left the alliance must drop the view");

    serverSimDestroy(sim);
    return 0;
}

/* 6. Under a decay policy the cycling takes an eligibility mask and steps over
 *    the items whose bit is clear: next-view skips an expired pill and wraps
 *    past it, the direction step refuses to land on it, re-approaching makes
 *    it selectable again, and a category with nothing left drops the view back
 *    to the tank. An all-bits mask — what every policy but decay produces —
 *    selects exactly what the cycle selected before there were any policies. */
int run_view_cycle_decay_skip(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    gs->viewPlayer = 0;

    /* Three of the player's own pills, ten squares apart along one row. */
    vc_set_pills(gs, 0, 3);
    BYTE p0x = gs->pb->item[0].x, p0y = gs->pb->item[0].y;
    BYTE p1x = gs->pb->item[1].x;
    BYTE p2x = gs->pb->item[2].x;

    BYTE mx = 0, my = 0;

    /* Every bit set: the cycle walks all three in index order and wraps. */
    UT_ASSERT_MSG(pillsGetNextView(gs, &gs->pb, VC_ALL_ELIGIBLE, &mx, &my,
                                   FALSE) == TRUE,
                  "the first pill should be found with every bit set");
    UT_ASSERT_MSG(mx == p0x && my == p0y,
                  "expected the first pill at (%u,%u), got (%u,%u)",
                  p0x, p0y, mx, my);
    UT_ASSERT_MSG(pillsGetNextView(gs, &gs->pb, VC_ALL_ELIGIBLE, &mx, &my,
                                   TRUE) == TRUE,
                  "the second pill should be found with every bit set");
    UT_ASSERT_MSG(mx == p1x, "expected the second pill at x=%u, got %u",
                  p1x, mx);

    /* Pill 1 has expired: the cycle steps straight over it to pill 2. */
    PlayerBitMap noMiddle = VC_ALL_ELIGIBLE & ~((PlayerBitMap)1 << 1);
    mx = p0x;
    my = p0y;
    UT_ASSERT_MSG(pillsGetNextView(gs, &gs->pb, noMiddle, &mx, &my, TRUE) ==
                      TRUE,
                  "a live pill should still be found past the expired one");
    UT_ASSERT_MSG(mx == p2x,
                  "the cycle should skip the expired pill to x=%u, got %u",
                  p2x, mx);

    /* And it wraps past it on the way round, back to pill 0. */
    UT_ASSERT_MSG(pillsGetNextView(gs, &gs->pb, noMiddle, &mx, &my, TRUE) ==
                      TRUE,
                  "the cycle should wrap with the expired pill skipped");
    UT_ASSERT_MSG(mx == p0x,
                  "the wrap should land on the first pill at x=%u, got %u",
                  p0x, mx);

    /* The direction step refuses the expired pill and takes the next one
     * along instead; with every bit set it takes the near one. */
    mx = p0x;
    my = p0y;
    UT_ASSERT_MSG(pillsMoveView(gs, &gs->pb, noMiddle, &mx, &my, 1, 0) == TRUE,
                  "a pill to the right should still be found");
    UT_ASSERT_MSG(mx == p2x,
                  "the step right should skip the expired pill to x=%u, got %u",
                  p2x, mx);
    mx = p0x;
    my = p0y;
    UT_ASSERT_MSG(pillsMoveView(gs, &gs->pb, VC_ALL_ELIGIBLE, &mx, &my, 1, 0) ==
                      TRUE,
                  "a pill to the right should be found with every bit set");
    UT_ASSERT_MSG(mx == p1x,
                  "the step right should take the nearest pill at x=%u, got %u",
                  p1x, mx);

    /* Re-approaching re-stamps the clock, so the bit comes back and the pill
     * is selectable again. */
    mx = p0x;
    my = p0y;
    UT_ASSERT_MSG(pillsMoveView(gs, &gs->pb, noMiddle | ((PlayerBitMap)1 << 1),
                                &mx, &my, 1, 0) == TRUE,
                  "the re-approached pill should be found again");
    UT_ASSERT_MSG(mx == p1x,
                  "the re-approached pill should be at x=%u, got %u", p1x, mx);

    /* A category with nothing left finds nothing at all. */
    mx = p0x;
    my = p0y;
    UT_ASSERT_MSG(pillsGetNextView(gs, &gs->pb, 0, &mx, &my, FALSE) == FALSE,
                  "a fully expired category should yield no pill");
    UT_ASSERT_MSG(pillsMoveView(gs, &gs->pb, 0, &mx, &my, 1, 0) == FALSE,
                  "a fully expired category should yield no pill in a "
                  "direction either");

    /* The same through the view machine: entering pill view with every pill
     * expired drops back to the tank, while an all-bits mask parks on one. */
    ViewPort vp;
    ScrollState scroll;
    memset(&vp, 0, sizeof(vp));
    memset(&scroll, 0, sizeof(scroll));

    ViewCycleInputs in;
    viewCycleInputsDefaults(&in);
    in.eligible[viewCategoryPill] = 0;
    vp.viewKind = VIEW_KIND_TANK;
    viewportPanInView(&vp, gs, &scroll, gs->tanks[0], VIEW_KIND_PILL, &in, 0, 0);
    UT_ASSERT_MSG(vp.viewKind == VIEW_KIND_TANK,
                  "an entirely expired category should leave the tank view, "
                  "got kind %u", vp.viewKind);

    viewCycleInputsDefaults(&in);
    vp.viewKind = VIEW_KIND_TANK;
    viewportPanInView(&vp, gs, &scroll, gs->tanks[0], VIEW_KIND_PILL, &in, 0, 0);
    UT_ASSERT_MSG(vp.viewKind == VIEW_KIND_PILL && vp.viewTarget == 0,
                  "an all-bits mask should park on the first pill, got kind "
                  "%u target %u", vp.viewKind, vp.viewTarget);

    /* The per-tick upkeep reads the same mask: the pill the camera is on
     * stops being watchable the moment its bit clears. */
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, &in) == TRUE,
                  "a live pill should stay watchable");
    in.eligible[viewCategoryPill] = VC_ALL_ELIGIBLE & ~(PlayerBitMap)1;
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, &in) == FALSE,
                  "an expired pill must drop the view");

    serverSimDestroy(sim);
    return 0;
}

/* 7. clientSimViewEligibleMask is the one place the per-item decay question is
 *    asked for cycling. Under always, key and off it answers "every item",
 *    whatever the clocks say — which is what keeps those three cycling as they
 *    did. Under decay it drops the items whose clocks have run out, and a
 *    re-stamped clock puts one back. */
int run_view_cycle_eligible_mask(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    UT_ASSERT_MSG(clientSimCreate(cs) == TRUE, "clientSimCreate failed");

    PlayerBitMap allPills = (PlayerBitMap)((1u << MAX_PILLS) - 1u);
    PlayerBitMap allTanks = (PlayerBitMap)((1u << MAX_TANKS) - 1u);

    /* Clocks that would fail every window test if one were applied: nobody
     * has ever been near anything. */
    cs->viewDecayTick = 5000;
    cs->viewDecaySecs[viewCategoryPill] = VIEW_DECAY_DEFAULT_SECS;
    cs->viewDecaySecs[viewCategoryAlly] = VIEW_DECAY_DEFAULT_SECS;

    cs->viewPolicy[viewCategoryPill] = viewPolicyAlways;
    UT_ASSERT_MSG(clientSimViewEligibleMask(cs, viewCategoryPill) == allPills,
                  "viewPolicyAlways should leave every pill selectable, got "
                  "0x%08x", (unsigned)clientSimViewEligibleMask(cs, viewCategoryPill));

    cs->viewPolicy[viewCategoryPill] = viewPolicyKey;
    UT_ASSERT_MSG(clientSimViewEligibleMask(cs, viewCategoryPill) == allPills,
                  "viewPolicyKey should leave every pill selectable, got "
                  "0x%08x", (unsigned)clientSimViewEligibleMask(cs, viewCategoryPill));

    cs->viewPolicy[viewCategoryPill] = viewPolicyOff;
    UT_ASSERT_MSG(clientSimViewEligibleMask(cs, viewCategoryPill) == allPills,
                  "viewPolicyOff should leave every pill selectable, got "
                  "0x%08x", (unsigned)clientSimViewEligibleMask(cs, viewCategoryPill));

    /* On decay the clocks decide. The window is the category's seconds at the
     * client's display rate; a stamp of 0 is a player who has never been near
     * the item at all. */
    uint32_t window = (uint32_t)VIEW_DECAY_DEFAULT_SECS *
                      CLIENT_VIEW_DECAY_TICKS_SEC;
    cs->viewPolicy[viewCategoryPill] = viewPolicyDecay;
    cs->pillNearTick[0] = cs->viewDecayTick;              /* right now */
    cs->pillNearTick[1] = 0;                              /* never been near */
    cs->pillNearTick[2] = cs->viewDecayTick - window - 1; /* just run out */
    cs->pillNearTick[3] = cs->viewDecayTick - window;     /* the last tick of it */

    PlayerBitMap mask = clientSimViewEligibleMask(cs, viewCategoryPill);
    UT_ASSERT_MSG((mask & 1u) != 0, "the pill stamped this tick should be selectable");
    UT_ASSERT_MSG((mask & 2u) == 0, "a pill never been near should not be selectable");
    UT_ASSERT_MSG((mask & 4u) == 0, "a pill past its window should not be selectable");
    UT_ASSERT_MSG((mask & 8u) != 0,
                  "a pill on the last tick of its window should be selectable");
    UT_ASSERT_MSG((mask & ~allPills) == 0,
                  "the mask should hold nothing past MAX_PILLS, got 0x%08x",
                  (unsigned)mask);

    /* Driving past it again re-stamps the clock and the pill comes back. */
    cs->pillNearTick[2] = cs->viewDecayTick;
    mask = clientSimViewEligibleMask(cs, viewCategoryPill);
    UT_ASSERT_MSG((mask & 4u) != 0,
                  "a re-approached pill should be selectable again");

    /* A category with every clock run out yields nothing. */
    memset(cs->pillNearTick, 0, sizeof(cs->pillNearTick));
    UT_ASSERT_MSG(clientSimViewEligibleMask(cs, viewCategoryPill) == 0,
                  "a fully expired category should leave no pill selectable");

    /* The ally category is the same question over MAX_TANKS bits, and it is
     * kept apart from the alive mask the region build takes. */
    cs->viewPolicy[viewCategoryAlly] = viewPolicyAlways;
    UT_ASSERT_MSG(clientSimViewEligibleMask(cs, viewCategoryAlly) == allTanks,
                  "viewPolicyAlways should leave every ally selectable");
    cs->viewPolicy[viewCategoryAlly] = viewPolicyDecay;
    cs->allyNearTick[2] = cs->viewDecayTick;
    UT_ASSERT_MSG(clientSimViewEligibleMask(cs, viewCategoryAlly) ==
                      ((PlayerBitMap)1 << 2),
                  "only the ally stamped this tick should be selectable");

    /* The inputs fill hands the viewport both masks, still apart. */
    ViewCycleInputs in;
    clientSimFillViewCycleInputs(cs, &in);
    UT_ASSERT_MSG(in.eligible[viewCategoryAlly] == ((PlayerBitMap)1 << 2),
                  "the fill should carry the ally eligibility mask through");
    UT_ASSERT_MSG(in.allyViewable == clientSimAllyViewMask(cs),
                  "the fill should carry the alive mask through unchanged");
    UT_ASSERT_MSG(in.allyLastMapX == cs->allyLastMapX &&
                      in.allyLastMapY == cs->allyLastMapY,
                  "the fill should carry the last known squares through");

    clientSimDestroy(cs);
    return 0;
}

/* 8. An ally the server is not sending arrives as a hidden stub, which zeroes
 *    its players entry. The ally view centres on the square the client last
 *    saw it on rather than the map origin, and gives up on it once the stub
 *    run outlasts the grace — a brief one, which is what entering the view
 *    under viewPolicyKey always costs, does not. */
int run_view_cycle_ally_stub(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    gs->viewPlayer = 0;
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);

    vc_place_tank(gs, 0, 50, 50);
    vc_place_tank(gs, 1, 100, 110);

    /* The client's own record of where slot 1 was last seen, and the zeroed
     * players entry a hidden stub leaves behind. */
    BYTE lastX[MAX_TANKS];
    BYTE lastY[MAX_TANKS];
    memset(lastX, 0, sizeof(lastX));
    memset(lastY, 0, sizeof(lastY));
    lastX[1] = 100;
    lastY[1] = 110;
    (*gs->plyrs).item[1].mapX = 0;
    (*gs->plyrs).item[1].mapY = 0;

    ViewCycleInputs in;
    viewCycleInputsDefaults(&in);
    in.allyViewable = vc_alive_mask(gs);
    in.allyLastMapX = lastX;
    in.allyLastMapY = lastY;

    ViewPort vp;
    ScrollState scroll;
    memset(&vp, 0, sizeof(vp));
    memset(&scroll, 0, sizeof(scroll));

    /* Entering ally view on the stubbed slot centres on the remembered
     * square, not the map origin. */
    vp.viewKind = VIEW_KIND_TANK;
    viewportPanInView(&vp, gs, &scroll, gs->tanks[0], VIEW_KIND_ALLY, &in, 0, 0);
    UT_ASSERT_MSG(vp.viewKind == VIEW_KIND_ALLY && vp.viewTarget == 1,
                  "the ally view should park on slot 1, got kind %u target %u",
                  vp.viewKind, vp.viewTarget);
    UT_ASSERT_MSG(vp.viewX == 100 && vp.viewY == 110,
                  "the ally view should centre on the last known square "
                  "(100,110), got (%u,%u)", vp.viewX, vp.viewY);

    /* The per-tick upkeep keeps it there rather than dragging it to (0,0). */
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, &in) == TRUE,
                  "a stubbed ally should stay watchable");
    UT_ASSERT_MSG(vp.viewX == 100 && vp.viewY == 110,
                  "the ally follow should hold the last known square, got "
                  "(%u,%u)", vp.viewX, vp.viewY);

    /* Without the record there is nothing to fall back to, which is the
     * origin the view used to land on. */
    ViewCycleInputs bare;
    viewCycleInputsDefaults(&bare);
    bare.allyViewable = in.allyViewable;
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, &bare) == TRUE,
                  "a stubbed ally should still be watchable with no record");
    UT_ASSERT_MSG(vp.viewX == 0 && vp.viewY == 0,
                  "with no record the view has only the zeroed entry, got "
                  "(%u,%u)", vp.viewX, vp.viewY);

    /* A real update puts the ally back in the players struct and the view
     * follows it there. */
    (*gs->plyrs).item[1].mapX = 120;
    (*gs->plyrs).item[1].mapY = 130;
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, &in) == TRUE,
                  "a re-appearing ally should stay watchable");
    UT_ASSERT_MSG(vp.viewX == 120 && vp.viewY == 130,
                  "the view should follow the ally back to (120,130), got "
                  "(%u,%u)", vp.viewX, vp.viewY);

    serverSimDestroy(sim);

    /* The grace itself, on a ClientSim with the clocks set by hand. */
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    UT_ASSERT_MSG(clientSimCreate(cs) == TRUE, "clientSimCreate failed");

    cs->viewDecayTick = 1000;
    cs->viewport.viewKind = VIEW_KIND_ALLY;
    cs->viewport.viewTarget = 3;
    cs->allySeenTick[3] = 1000;
    UT_ASSERT_MSG(clientSimAllyViewStubExpired(cs) == FALSE,
                  "an ally seen this tick should not have run out");

    /* A stub run inside the grace holds the view — this is the round trip a
     * viewPolicyKey claim costs before the server starts sending. */
    cs->viewDecayTick = 1000 + CLIENT_VIEW_ALLY_STUB_GRACE_TICKS;
    UT_ASSERT_MSG(clientSimAllyViewStubExpired(cs) == FALSE,
                  "a stub run inside the grace should hold the view");

    /* One tick past it, the view gives up. */
    cs->viewDecayTick = 1000 + CLIENT_VIEW_ALLY_STUB_GRACE_TICKS + 1;
    UT_ASSERT_MSG(clientSimAllyViewStubExpired(cs) == TRUE,
                  "a stub run past the grace should drop the view");

    /* A real update for the ally resets it wherever in the run it lands. */
    cs->allySeenTick[3] = cs->viewDecayTick;
    UT_ASSERT_MSG(clientSimAllyViewStubExpired(cs) == FALSE,
                  "an update for the watched ally should reset the grace");

    /* Leaving the view clears the timer, so entering again on the same ally
     * gets a full grace even though the slot has been stubbed all along. */
    cs->viewport.viewKind = VIEW_KIND_TANK;
    UT_ASSERT_MSG(clientSimAllyViewStubExpired(cs) == FALSE,
                  "the tank view has no ally to time out");
    cs->viewDecayTick += 10 * CLIENT_VIEW_ALLY_STUB_GRACE_TICKS;
    cs->viewport.viewKind = VIEW_KIND_ALLY;
    UT_ASSERT_MSG(clientSimAllyViewStubExpired(cs) == FALSE,
                  "re-entering the view should start the grace over");
    cs->viewDecayTick += CLIENT_VIEW_ALLY_STUB_GRACE_TICKS + 1;
    UT_ASSERT_MSG(clientSimAllyViewStubExpired(cs) == TRUE,
                  "the fresh grace should still run out");

    /* Moving to another ally restarts it too. */
    cs->viewport.viewTarget = 4;
    UT_ASSERT_MSG(clientSimAllyViewStubExpired(cs) == FALSE,
                  "moving to another ally should start the grace over");

    /* The round reset clears the record the grace reads. */
    clientSimResetViewDecay(cs);
    UT_ASSERT_MSG(cs->allySeenTick[3] == 0 && cs->allyLastMapX[3] == 0 &&
                      cs->allyLastMapY[3] == 0,
                  "the round reset should clear the last known squares");

    clientSimDestroy(cs);
    return 0;
}

/* 9. A pill whose square the client has not been told is current is still
 *    cyclable. The server grants the rect when the camera lands on it and the
 *    position corrects itself a tick later, so the cycle must reach a pill the
 *    movement code has already stopped treating as solid. */
int run_view_cycle_stale_pill(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    gs->viewPlayer = 0;

    /* Three of the player's own pills in a row; the middle one has moved out of
     * sight, so its square is the last one we were given. */
    vc_set_pills(gs, 0, 3);
    BYTE p0x = gs->pb->item[0].x, p0y = gs->pb->item[0].y;
    BYTE p1x = gs->pb->item[1].x, p1y = gs->pb->item[1].y;
    pillsSetPosState(&gs->pb, 1, PILL_SQUARE_REMEMBERED);

    /* The premise: gameplay has already let go of it. */
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, p1x, p1y) == FALSE,
                  "the stale pill should not be solid — the case proves "
                  "nothing otherwise");

    /* The cycle still walks onto it, in index order and by direction. */
    BYTE mx = p0x, my = p0y;
    UT_ASSERT_MSG(pillsGetNextView(gs, &gs->pb, VC_ALL_ELIGIBLE, &mx, &my,
                                   TRUE) == TRUE,
                  "cycling from pill 0 should find the stale pill");
    UT_ASSERT_MSG(mx == p1x && my == p1y,
                  "the cycle should land on the stale pill at (%u,%u), got "
                  "(%u,%u)", p1x, p1y, mx, my);

    mx = p0x; my = p0y;
    UT_ASSERT_MSG(pillsMoveView(gs, &gs->pb, VC_ALL_ELIGIBLE, &mx, &my, 1, 0) ==
                      TRUE,
                  "the step right should find the stale pill");
    UT_ASSERT_MSG(mx == p1x,
                  "the step right should take the stale pill at x=%u, got %u",
                  p1x, mx);

    /* And the view machine keeps a camera parked on it: the square still
     * resolves to a pill index, and the per-tick upkeep does not drop it. */
    UT_ASSERT_MSG(pillsCheckView(gs, &gs->pb, p1x, p1y) == TRUE,
                  "the stale pill should still pass the view check");

    ViewPort vp;
    ScrollState scroll;
    ViewCycleInputs in;
    memset(&vp, 0, sizeof(vp));
    memset(&scroll, 0, sizeof(scroll));
    viewCycleInputsDefaults(&in);

    vp.viewKind   = VIEW_KIND_PILL;
    vp.viewTarget = 1;
    vp.viewX      = p1x;
    vp.viewY      = p1y;
    UT_ASSERT_MSG(viewportUpdateItemView(&vp, gs, &scroll, &in) == TRUE,
                  "a stale pill must not drop the view");
    UT_ASSERT_MSG(vp.viewTarget == 1,
                  "the view should still report pill 1, got %u", vp.viewTarget);

    /* With every pill's square merely remembered — a player who has seen none
     * of them this round — entering pill view still parks on one. */
    pillsSetPosState(&gs->pb, 0, PILL_SQUARE_REMEMBERED);
    pillsSetPosState(&gs->pb, 2, PILL_SQUARE_REMEMBERED);
    vp.viewKind = VIEW_KIND_TANK;
    viewportPanInView(&vp, gs, &scroll, gs->tanks[0], VIEW_KIND_PILL, &in, 0, 0);
    UT_ASSERT_MSG(vp.viewKind == VIEW_KIND_PILL && vp.viewTarget == 0,
                  "entering pill view should park on the first pill, got kind "
                  "%u target %u", vp.viewKind, vp.viewTarget);

    serverSimDestroy(sim);
    return 0;
}
