/*
 * Next step for a tank on foot on a land -> boat -> sea route
 * (test_pf_foot_boat_route.c).
 *
 * Regression for 20261103_095144_1 bot0 t=1385 (seed 330, Chew Toy 3): on
 * foot next to the boat it had left, the tank's route went back to the boat
 * and then by sea along the coast. The chain-rank pick matched route tiles by
 * x,y only, so it took coast tiles beside the SEA part of the route, which a
 * tank on foot cannot reach. The tank circled at (101,125) for minutes.
 *
 * Geometry: deep sea everywhere, a two-row land strip y=99..100,
 * x=96..110, and a boat at (96,101). Slate root (110,100) on foot,
 * destination (112,101) in the sea. The route walks west along y=100,
 * boards at (96,101) and sails east along y=101, right beside the land.
 * The tank stands on foot at (104,99), off the route. Its next step must
 * go west toward the boat, not east along the sea part.
 */

#include <stdio.h>
#include <string.h>

#include "brain_pathfinder.h"
#include "test_harness.h"

#define TT_GRASS     7
#define TT_BOAT      9
#define TT_DEEPSEA  10

#define PF_MAPSZ 256

#define SRC_X  110
#define SRC_Y  100
#define DST_X  112
#define DST_Y  101
#define BOAT_X  96
#define BOAT_Y 101
#define TANK_X 104
#define TANK_Y  99

static BYTE g_map[PF_MAPSZ * PF_MAPSZ];

int run_pf_foot_next_step_goes_to_boat(void) {
    memset(g_map, TT_DEEPSEA, sizeof(g_map));
    for (int y = 99; y <= 100; y++) {
        for (int x = 96; x <= 110; x++) g_map[y * PF_MAPSZ + x] = TT_GRASS;
    }
    g_map[BOAT_Y * PF_MAPSZ + BOAT_X] = TT_BOAT;

    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetMap(pf, g_map);
    /* The bot runs with both next-step rules on. */
    brainPathfinderSetConfig(pf, "nextstep_chain_veer", 1.0f);
    brainPathfinderSetConfig(pf, "nextstep_foot_sea_rule", 1.0f);

    brainPathfinderDijkstraStart(pf, /*slate*/0, /*tick*/1, SRC_X, SRC_Y,
                                 /*in_boat*/0, /*shells*/0, /*trees*/0,
                                 /*mines*/0, /*armour*/40,
                                 /*max_cost*/0.0f, /*exact*/0,
                                 /*danger_scale*/1.0f, /*kind*/1,
                                 /*allow_boat*/1);
    int done = 0;
    for (int i = 0; i < 200 && !done; i++) {
        done = brainPathfinderDijkstraStep(pf, 0, 2 + i, 100000);
    }
    UT_ASSERT_MSG(done, "dijkstra slate did not complete");

    /* The route must be the one the test is about: through the boat, then
     * along the sea row beside the land. */
    int px[512], py[512];
    int n = brainPathfinderDijkstraTracePath(pf, 0, DST_X, DST_Y, px, py, 512);
    UT_ASSERT_MSG(n > 0, "no traced path to the sea destination");
    int via_boat = 0, along_coast = 0;
    for (int i = 0; i < n; i++) {
        if (px[i] == BOAT_X && py[i] == BOAT_Y) via_boat = 1;
        if (px[i] == TANK_X + 1 && py[i] == BOAT_Y) along_coast = 1;
    }
    UT_ASSERT_MSG(via_boat, "route does not board at (%d,%d)", BOAT_X, BOAT_Y);
    UT_ASSERT_MSG(along_coast, "route does not sail along y=%d", BOAT_Y);

    int nx = -1, ny = -1;
    int ok = brainPathfinderDijkstraNextStep(pf, /*kind*/1, TANK_X, TANK_Y,
                                             DST_X, DST_Y, NULL, 0, 0.0f,
                                             /*tank_in_boat*/0, &nx, &ny);
    UT_ASSERT_MSG(ok, "no next step for the tank on foot");
    UT_ASSERT_MSG(nx < TANK_X,
                  "on foot at (%d,%d): next step (%d,%d) does not go west "
                  "toward the boat at (%d,%d)",
                  TANK_X, TANK_Y, nx, ny, BOAT_X, BOAT_Y);
    UT_ASSERT_MSG((g_map[ny * PF_MAPSZ + nx] & 0x0F) != TT_DEEPSEA,
                  "on foot: next step (%d,%d) is deep sea", nx, ny);

    brainPathfinderDestroy(pf);
    return 0;
}
