/*
 * Incremental start-picker tests (test_starts_pick_incremental.c).
 *
 * startsPickIncremental reserves one free start for a single joiner,
 * sharing startsMapDistance and the deep-sea validity guard with
 * startsAssignBatch. These tests pin the two selection modes and the
 * exhausted-pool fallback:
 *
 *   (a) with teammate reservations, the free start nearest a teammate
 *       wins (cluster);
 *   (b) with none, the free start farthest from every taken start wins
 *       (farthest-first);
 *   (c) when every start is taken, it returns MAX_STARTS;
 *   (d) a joiner whose team chose a side spreads across it instead,
 *       taking the start farthest from its teammates, while the same
 *       layout with no side still clusters.
 *
 * A starts structure is built directly on a running sim's map, with the
 * chosen start squares forced to deep sea (no mine) so the validity
 * guard accepts them and the distance math drives the result.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"     /* mapSetPos */
#include "starts.h"       /* startsPickIncremental */
#include "start_sides.h"  /* START_SIDE_ANY */
#include "server_sim.h"   /* ut_make_running_sim, serverSimGetGameSim */
#include "test_harness.h"

/* Start layout shared by all three cases. Distances are Chebyshev
 * (startsMapDistance), so from start 0 at (100,100):
 *   1 (102,100) = 2, 2 (200,200) = 100, 3 (130,100) = 30, 4 (50,50) = 50 */
static const BYTE k_sx[5] = {100, 102, 200, 130, 50};
static const BYTE k_sy[5] = {100, 100, 200, 100, 50};
#define K_NUM_STARTS 5

/* Build the 5-start layout on the sim's map, forcing each square to deep
 * sea with no mine so startsIsValidSquare accepts it. */
static void build_starts(GameSim *gs) {
    int i;
    for (i = 0; i < K_NUM_STARTS; i++) {
        mapSetPos(gs, &gs->mp, k_sx[i], k_sy[i], DEEP_SEA, FALSE, TRUE);
        gs->ss->item[i].x = k_sx[i];
        gs->ss->item[i].y = k_sy[i];
        gs->ss->item[i].dir = 0;
    }
    startsSetNumStarts(&gs->ss, K_NUM_STARTS);
}

/* (a) Cluster: a teammate holds start 0; the nearest free start (1) wins. */
int run_starts_pick_cluster_nearest_teammate(void) {
    ServerSim *sim = ut_make_running_sim("Cluster");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_starts(gs);

    bool taken[MAX_STARTS] = {false};
    taken[0] = true;                 /* teammate sits on start 0 */
    BYTE teammates[1] = {0};         /* 0-based teammate reservation */
    BYTE picked = startsPickIncremental(gs, &gs->ss, taken, teammates, 1,
                                        START_SIDE_ANY, 0);

    UT_ASSERT_MSG(picked == 1,
                  "cluster should pick start 1 (nearest to teammate), got %u",
                  (unsigned)picked);
    serverSimDestroy(sim);
    return 0;
}

/* (b) Farthest-first: no teammates; the free start farthest from the one
 *     taken start (2, at distance 100) wins. */
int run_starts_pick_farthest_when_solo(void) {
    ServerSim *sim = ut_make_running_sim("Solo");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_starts(gs);

    bool taken[MAX_STARTS] = {false};
    taken[0] = true;                 /* only start 0 is taken */
    BYTE picked = startsPickIncremental(gs, &gs->ss, taken, NULL, 0,
                                        START_SIDE_ANY, 0);

    UT_ASSERT_MSG(picked == 2,
                  "farthest-first should pick start 2 (max distance), got %u",
                  (unsigned)picked);
    serverSimDestroy(sim);
    return 0;
}

/* (c) Exhausted: every start taken yields MAX_STARTS (no reservation). */
int run_starts_pick_none_when_all_taken(void) {
    ServerSim *sim = ut_make_running_sim("Full");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_starts(gs);

    bool taken[MAX_STARTS];
    int i;
    for (i = 0; i < MAX_STARTS; i++) taken[i] = true;
    BYTE picked = startsPickIncremental(gs, &gs->ss, taken, NULL, 0,
                                        START_SIDE_ANY, 0);

    UT_ASSERT_MSG(picked == MAX_STARTS,
                  "all-taken should return MAX_STARTS (%u), got %u",
                  (unsigned)MAX_STARTS, (unsigned)picked);
    serverSimDestroy(sim);
    return 0;
}

/* Corner layout for the side cases: four clusters of four, one per
 * corner, bbox 40..210 on both axes so every start is a diagonal
 * (north-west is N|W and so on) and a north team has two corners to
 * choose between. */
static const BYTE k_cx[16] = {  40,  50,  40,  50, 200, 210, 200, 210,
                                40,  50,  40,  50, 200, 210, 200, 210 };
static const BYTE k_cy[16] = {  40,  40,  50,  50,  40,  40,  50,  50,
                               200, 200, 210, 210, 200, 200, 210, 210 };

static void build_corner_starts(GameSim *gs) {
    int i;
    gs->pb->numPills = 0;
    gs->bs->numBases = 0;
    for (i = 0; i < 16; i++) {
        mapSetPos(gs, &gs->mp, k_cx[i], k_cy[i], DEEP_SEA, FALSE, TRUE);
        gs->ss->item[i].x = k_cx[i];
        gs->ss->item[i].y = k_cy[i];
        gs->ss->item[i].dir = 0;
    }
    startsSetNumStarts(&gs->ss, 16);
}

/* The side mask the picker itself computes for a start. */
static BYTE corner_mask(GameSim *gs, BYTE idx) {
    int leftPos;
    int rightPos;
    int topPos;
    int bottomPos;
    startsGetMaxs(&gs->ss, &leftPos, &rightPos, &topPos, &bottomPos);
    return startSideMaskFor(gs->ss->item[idx].x, gs->ss->item[idx].y,
                            leftPos, topPos, rightPos, bottomPos);
}

/* (d) A team-mate holds the north-west corner start 0 and the joiner's
 *     team has chosen north against a south team. The joiner takes a
 *     north-east start: on its own side, and as far from the team-mate as
 *     that side goes. With no side the same call still clusters onto the
 *     start next door, which is what a team that never asked for a side
 *     has always got. */
int run_starts_pick_spreads_on_side(void) {
    ServerSim *sim = ut_make_running_sim("SideSpreadPick");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_corner_starts(gs);

    bool taken[MAX_STARTS] = {false};
    taken[0] = true;                 /* team-mate on the north-west start 0 */
    BYTE teammates[1] = {0};
    BYTE picked = startsPickIncremental(gs, &gs->ss, taken, teammates, 1,
                                        START_SIDE_N, START_SIDE_BIT_S);

    UT_ASSERT_MSG(picked < 16, "side pick came back unplaced (%u)", (unsigned)picked);
    UT_ASSERT_MSG((corner_mask(gs, picked) & START_SIDE_BIT_N) != 0,
                  "side N pick landed on start %u, not a north one", (unsigned)picked);
    UT_ASSERT_MSG((corner_mask(gs, picked) & START_SIDE_BIT_E) != 0,
                  "side N pick landed on start %u, in the team-mate's own north-west corner",
                  (unsigned)picked);

    /* The team never chose a side, but the east team's choice leaves it
       only the west, so it spreads over that: from a team-mate in the
       north-west it takes a south-west start, not the one next door. This
       is the case a real game hit first — one team picks a side and the
       other is left alone, which is the ordinary way a lobby is set up. */
    picked = startsPickIncremental(gs, &gs->ss, taken, teammates, 1,
                                   START_SIDE_ANY, START_SIDE_BIT_E);
    UT_ASSERT_MSG(picked < 16, "confined pick came back unplaced (%u)", (unsigned)picked);
    UT_ASSERT_MSG((corner_mask(gs, picked) & START_SIDE_BIT_E) == 0,
                  "a team kept off the east landed on east start %u", (unsigned)picked);
    UT_ASSERT_MSG((corner_mask(gs, picked) & START_SIDE_BIT_S) != 0,
                  "confined pick landed on start %u, in the team-mate's own north-west corner",
                  (unsigned)picked);

    /* Nobody chose a side anywhere: the pick still clusters onto the start
       next door, which is what a team in an ordinary game has always had. */
    picked = startsPickIncremental(gs, &gs->ss, taken, teammates, 1,
                                   START_SIDE_ANY, 0);
    UT_ASSERT_MSG(picked == 1,
                  "with no side in play the pick should cluster onto start 1, got %u",
                  (unsigned)picked);
    serverSimDestroy(sim);
    return 0;
}

/* Ring layout: sixteen starts around the edge, two each of W, NW, N, NE,
 * E, SE, S and SW — the shape of Everard Island. bbox x 76..184,
 * y 100..156. */
static const BYTE k_rx[16] = {  76,  76,  92, 108, 124, 140, 156, 172,
                               184, 184, 172, 156, 140, 124, 108,  92 };
static const BYTE k_ry[16] = { 140, 124, 100, 100, 100, 100, 100, 100,
                               124, 140, 156, 156, 156, 156, 156, 156 };

static void build_ring_starts(GameSim *gs) {
    int i;
    gs->pb->numPills = 0;
    gs->bs->numBases = 0;
    for (i = 0; i < 16; i++) {
        mapSetPos(gs, &gs->mp, k_rx[i], k_ry[i], DEEP_SEA, FALSE, TRUE);
        gs->ss->item[i].x = k_rx[i];
        gs->ss->item[i].y = k_ry[i];
        gs->ss->item[i].dir = 0;
    }
    startsSetNumStarts(&gs->ss, 16);
}

/* (e) On a map whose starts ring the island, a team kept off the east is
 *     not thereby on the west: the rest of the ring runs north and south
 *     and comes back to the east at both ends. Both picks of a team with
 *     no side take the far side of the map, so neither lands on a
 *     due-north start beside the east team. */
int run_starts_pick_unsided_takes_far_side(void) {
    ServerSim *sim = ut_make_running_sim("RingFarSide");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_ring_starts(gs);

    bool taken[MAX_STARTS] = {false};
    BYTE mates[2];
    BYTE first;
    BYTE second;

    /* The east team holds its two starts: index 6 (156,100) is NE and
       index 10 (172,156) is SE. */
    taken[6] = true;
    taken[10] = true;

    first = startsPickIncremental(gs, &gs->ss, taken, NULL, 0,
                                  START_SIDE_ANY, START_SIDE_BIT_E);
    UT_ASSERT_MSG(first < 16, "first confined pick came back unplaced (%u)", (unsigned)first);
    UT_ASSERT_MSG((corner_mask(gs, first) & START_SIDE_BIT_W) != 0,
                  "first confined pick landed on start %u, off the east but not on the west",
                  (unsigned)first);
    taken[first] = true;
    mates[0] = first;

    second = startsPickIncremental(gs, &gs->ss, taken, mates, 1,
                                   START_SIDE_ANY, START_SIDE_BIT_E);
    UT_ASSERT_MSG(second < 16, "second confined pick came back unplaced (%u)", (unsigned)second);
    UT_ASSERT_MSG((corner_mask(gs, second) & START_SIDE_BIT_W) != 0,
                  "second confined pick landed on start %u, off the east but not on the west",
                  (unsigned)second);
    UT_ASSERT_MSG(second != first,
                  "both confined picks took start %u", (unsigned)first);
    serverSimDestroy(sim);
    return 0;
}
