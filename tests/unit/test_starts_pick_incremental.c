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
 *   (c) when every start is taken, it returns MAX_STARTS.
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
