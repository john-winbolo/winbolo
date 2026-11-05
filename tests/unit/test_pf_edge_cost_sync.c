/*
 * The Dijkstra edge cost grid follows the live map (test_pf_edge_cost_sync.c).
 *
 * Regression for 20261103_165532_1 player 2, brain ticks 6555-6762: the grid
 * was built once at Brain.open and marked stale only when the map POINTER
 * changed. The brain's map is one buffer changed in place, so squares the
 * bot had not seen at Brain.open stayed priced as "unknown" (cost 3, no walls,
 * no corner blocks). The route took the diagonal (120,131)->(121,130) between
 * a half wall (120,130) and a building (121,131), and the tank pushed against
 * the walls for about 120 ticks.
 *
 * brainPathfinderSyncEdgeCosts now compares the map with the bytes the grid
 * was built from and rebuilds the entries next to each changed square. These
 * tests check the two cases from the field, and that the per-square update
 * gives the same bytes as a full rebuild.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "brain_pathfinder.h"
#include "test_harness.h"

#define TT_BUILDING  0
#define TT_RIVER     1
#define TT_ROAD      4
#define TT_GRASS     7
#define TT_HALFBUILD 8
#define TT_BOAT      9
#define TT_DEEPSEA   10
#define TT_UNKNOWN   13

#define PF_MAPSZ    256
#define PF_GRID     (PF_MAPSZ * PF_MAPSZ)
#define PF_NODES    (PF_GRID * 2)
#define PF_ENTRIES  (PF_NODES * 8)
#define PF_INF      1e29f

/* Direction numbers, as DX8/DY8 in brain_pathfinder.c:
 * 0 N, 1 NE, 2 E, 3 SE, 4 S, 5 SW, 6 W, 7 NW. */
#define DIR_NE 1
#define DIR_E  2

static BYTE g_map[PF_GRID];

static float edge_at(const BrainPathfinder *pf, int x, int y, int boat, int d) {
    int node = (boat ? PF_GRID : 0) + y * PF_MAPSZ + x;
    return pf->edge_cost[(size_t)node * 8 + d];
}

/* The field case: the grid is built while the area is unknown, then the bot
 * sees the half wall and the building. The next Dijkstra start must refuse
 * the diagonal between them. Goes through brainPathfinderDijkstraStart, the
 * call the brain makes, so it checks the sync is wired in there. */
int run_pf_edge_sync_blocks_new_corner(void) {
    memset(g_map, TT_UNKNOWN, sizeof(g_map));
    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetMap(pf, g_map);
    brainPathfinderRebuildEdgeCosts(pf);   /* as Brain.open does */

    float before = edge_at(pf, 120, 131, 0, DIR_NE);
    UT_ASSERT_MSG(before < PF_INF, "unknown diagonal should be open (got %g)",
                  before);

    g_map[130 * PF_MAPSZ + 120] = TT_HALFBUILD;
    g_map[131 * PF_MAPSZ + 121] = TT_BUILDING;

    brainPathfinderDijkstraStart(pf, /*slate*/0, /*tick*/1, 120, 131,
                                 /*in_boat*/0, /*shells*/30, /*trees*/20,
                                 /*mines*/0, /*armour*/40,
                                 /*max_cost*/0.0f, /*exact*/0,
                                 /*danger_scale*/1.0f, /*kind*/1,
                                 /*allow_boat*/0);

    float after = edge_at(pf, 120, 131, 0, DIR_NE);
    brainPathfinderDestroy(pf);
    UT_ASSERT_MSG(after >= PF_INF,
                  "diagonal (120,131)->(121,130) between two walls still "
                  "open after sync (cost %g)", after);
    return 0;
}

/* A step onto a square that became a wall costs wall_shoot_cost on foot,
 * not the "unknown" cost the grid was built with. */
int run_pf_edge_sync_prices_new_wall(void) {
    memset(g_map, TT_UNKNOWN, sizeof(g_map));
    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetMap(pf, g_map);
    brainPathfinderSetConfig(pf, "wall_shoot_cost", 30.0f);
    brainPathfinderRebuildEdgeCosts(pf);

    float before = edge_at(pf, 119, 130, 0, DIR_E);
    UT_ASSERT_MSG(before == 3.0f, "unknown step should cost 3 (got %g)", before);

    g_map[130 * PF_MAPSZ + 120] = TT_HALFBUILD;
    int n = brainPathfinderSyncEdgeCosts(pf);
    float after = edge_at(pf, 119, 130, 0, DIR_E);
    /* A change of the mine bit alone is not a terrain change. */
    g_map[10 * PF_MAPSZ + 10] |= 0x80;
    int n_mine = brainPathfinderSyncEdgeCosts(pf);
    brainPathfinderDestroy(pf);

    UT_ASSERT_MSG(n == 1, "sync should report 1 changed square (got %d)", n);
    UT_ASSERT_MSG(after == 30.0f,
                  "step onto the new half wall should cost wall_shoot_cost "
                  "30 (got %g)", after);
    UT_ASSERT_MSG(n_mine == 0,
                  "mine bit alone should not count as a change (got %d)",
                  n_mine);
    return 0;
}

/* Fixed-seed generator so the test is the same every run. */
static uint32_t g_rng;
static uint32_t rng_next(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

/* The types that change edge costs in different ways: walls, water, boat,
 * deep sea, plain land, unknown. */
static BYTE random_type(void) {
    static const BYTE kTypes[] = {
        TT_BUILDING, TT_RIVER, TT_ROAD, TT_GRASS, TT_HALFBUILD,
        TT_BOAT, TT_DEEPSEA, TT_UNKNOWN, 2, 3, 5, 6, 11, 12
    };
    BYTE t = kTypes[rng_next() % (sizeof(kTypes) / sizeof(kTypes[0]))];
    if ((rng_next() & 7) == 0) t |= 0x80;   /* some squares carry a mine */
    return t;
}

/* After any set of changes, the per-square sync and a full rebuild give
 * byte-identical grids. Rounds cover: single squares, squares on the map
 * edges and corners, clusters (first view of an area), mine bit only, a
 * round with no change, and a round large enough to take the full-rebuild
 * fallback. */
int run_pf_edge_sync_matches_full_rebuild(void) {
    g_rng = 12345u;
    for (int i = 0; i < PF_GRID; i++) {
        g_map[i] = (rng_next() & 1) ? TT_UNKNOWN : random_type();
    }

    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetMap(pf, g_map);
    brainPathfinderSetConfig(pf, "wall_shoot_cost", 30.0f);
    brainPathfinderRebuildEdgeCosts(pf);

    /* The reference: a second pathfinder on the same map that does a full
     * rebuild every round. pf itself only syncs, so its changes add up
     * round after round. */
    BrainPathfinder *ref = brainPathfinderCreate();
    UT_ASSERT(ref != NULL);
    brainPathfinderSetMap(ref, g_map);
    brainPathfinderSetConfig(ref, "wall_shoot_cost", 30.0f);

    static const int kEdgeX[] = { 0, 255, 0, 255, 0, 255, 128, 128 };
    static const int kEdgeY[] = { 0, 0, 255, 255, 100, 200, 0, 255 };

    int fail = 0;
    for (int round = 0; round < 12 && !fail; round++) {
        int expect_full = 0;
        if (round == 0) {
            /* nothing changes */
        } else if (round == 1) {
            g_map[130 * PF_MAPSZ + 120] = TT_HALFBUILD;
        } else if (round == 2) {
            for (int k = 0; k < 8; k++) {
                g_map[kEdgeY[k] * PF_MAPSZ + kEdgeX[k]] = random_type();
            }
        } else if (round == 3) {
            for (int k = 0; k < 200; k++) {   /* mine bit only */
                g_map[rng_next() % PF_GRID] ^= 0x80;
            }
        } else if (round == 4 || round == 5) {
            /* a cluster, as when the bot first sees an area */
            int cx = 20 + (int)(rng_next() % 200), cy = 20 + (int)(rng_next() % 200);
            for (int y = cy - 7; y <= cy + 7; y++) {
                for (int x = cx - 7; x <= cx + 7; x++) {
                    g_map[y * PF_MAPSZ + x] = random_type();
                }
            }
        } else if (round == 6) {
            /* far more than the fallback count */
            for (int k = 0; k < 20000; k++) {
                g_map[rng_next() % PF_GRID] = random_type();
            }
            expect_full = 1;
        } else {
            int count = 1 + (int)(rng_next() % 300);
            for (int k = 0; k < count; k++) {
                g_map[rng_next() % PF_GRID] = random_type();
            }
        }

        int n = brainPathfinderSyncEdgeCosts(pf);
        if (expect_full && n != -1) {
            fprintf(stderr, "FAIL round %d: expected a full rebuild, got %d\n",
                    round, n);
            fail = 1;
            break;
        }
        if (round == 0 && n != 0) {
            fprintf(stderr, "FAIL round 0: nothing changed but sync "
                    "reported %d\n", n);
            fail = 1;
            break;
        }
        const float *synced = pf->edge_cost;
        brainPathfinderRebuildEdgeCosts(ref);
        if (memcmp(synced, ref->edge_cost, sizeof(float) * PF_ENTRIES) != 0) {
            for (int e = 0; e < PF_ENTRIES; e++) {
                if (memcmp(&synced[e], &ref->edge_cost[e], sizeof(float)) != 0) {
                    int node = e / 8;
                    fprintf(stderr, "FAIL round %d: entry (%d,%d) boat=%d d=%d "
                            "sync %g, full rebuild %g\n", round,
                            (node % PF_GRID) % PF_MAPSZ,
                            (node % PF_GRID) / PF_MAPSZ,
                            node >= PF_GRID, e % 8,
                            synced[e], ref->edge_cost[e]);
                    break;
                }
            }
            fail = 1;
        }
    }

    brainPathfinderDestroy(ref);
    brainPathfinderDestroy(pf);
    return fail;
}
