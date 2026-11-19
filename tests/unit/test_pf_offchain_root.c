/*
 * Off-chain next step must not turn back to a stale slate root
 * (test_pf_offchain_root.c).
 *
 * A slate restarts on the tank's square only every few ticks. When the tank
 * is on a square that is not on the traced chain, the next-step fallbacks
 * picked a neighbour by g (cost from the slate root), so the root (g = 0)
 * won and the tank turned back (debug session
 * 20261103_165532_1_v3_anchor_helm_s205, player 2, brain ticks 1663, 1703,
 * 1860, 1880, 2146-2147). The fallback pick is now checked against step cost
 * plus cost to go along the chain. This is a bug fix with no config key.
 *
 *   pf_offchain_lowest_g_not_root: chain veer off, tank next to the root and
 *     next to the chain square ahead. The lowest-g fallback picked the root;
 *     now the chain square ahead.
 *   pf_offchain_chain_rank_not_root: chain veer on, the root has danger and
 *     the square ahead has some danger, so the chain-rank hazard bound refuses
 *     the square ahead and the pick was the root. Going ahead is cheaper, so
 *     the square ahead is now the pick.
 *   pf_offchain_dead_end_keeps_root: a dead end beside the root; the root is
 *     the only way out and is still returned (veer on and off).
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "brain_pathfinder.h"
#include "test_harness.h"

#define TT_GRASS     7
#define TT_DEEPSEA  10

#define PF_MAPSZ 256
#define KIND     1

#define ROOT_X 100
#define ROOT_Y 100
#define DST_X  110
#define DST_Y  100

static BYTE g_map[PF_MAPSZ * PF_MAPSZ];

static int run_slate(BrainPathfinder *pf, int sx, int sy) {
    brainPathfinderDijkstraStart(pf, 0, 1, sx, sy,
                                 /*in_boat*/0, /*shells*/0, /*trees*/0,
                                 /*mines*/0, /*armour*/40,
                                 /*max_cost*/0.0f, /*exact*/0,
                                 /*danger_scale*/1.0f, KIND,
                                 /*allow_boat*/1);
    int done = 0;
    for (int i = 0; i < 200 && !done; i++) {
        done = brainPathfinderDijkstraStep(pf, 0, 2 + (uint32_t)i, 100000);
    }
    return done;
}

static float min_layer(BrainPathfinder *pf, int x, int y) {
    float a = brainPathfinderDijkstraCostAt(pf, 0, x, y, 0);
    float b = brainPathfinderDijkstraCostAt(pf, 0, x, y, 1);
    return a < b ? a : b;
}

static void build_field_map(void) {
    memset(g_map, TT_DEEPSEA, sizeof(g_map));
    for (int y = 90; y <= 110; y++) {
        for (int x = 90; x <= 120; x++) g_map[y * PF_MAPSZ + x] = TT_GRASS;
    }
}

static BrainPathfinder *make_pf(float chain_veer) {
    BrainPathfinder *pf = brainPathfinderCreate();
    if (!pf) return NULL;
    brainPathfinderSetMap(pf, g_map);
    brainPathfinderSetConfig(pf, "nextstep_chain_veer", chain_veer);
    brainPathfinderSetConfig(pf, "nextstep_foot_sea_rule", 1.0f);
    return pf;
}

int run_pf_offchain_lowest_g_not_root(void) {
    build_field_map();
    BrainPathfinder *pf = make_pf(0.0f);
    UT_ASSERT_MSG(pf != NULL, "no pathfinder");
    if (!run_slate(pf, ROOT_X, ROOT_Y)) {
        brainPathfinderDestroy(pf);
        UT_FAIL("slate did not complete");
    }
    /* The tank has moved to the square south of the root, off the chain. */
    int nx = -1, ny = -1;
    int ok = brainPathfinderDijkstraNextStep(pf, KIND, ROOT_X, ROOT_Y + 1,
                                             DST_X, DST_Y, NULL, 0, 0.0f, 0,
                                             &nx, &ny);
    brainPathfinderDestroy(pf);
    UT_ASSERT_MSG(ok == 1, "no next step");
    UT_ASSERT_MSG(!(nx == ROOT_X && ny == ROOT_Y),
                  "next step is the stale root (%d,%d)", nx, ny);
    UT_ASSERT_MSG(nx == ROOT_X + 1 && ny == ROOT_Y,
                  "next (%d,%d), want (%d,%d)", nx, ny, ROOT_X + 1, ROOT_Y);
    return 0;
}

int run_pf_offchain_chain_rank_not_root(void) {
    build_field_map();
    BrainPathfinder *pf = make_pf(1.0f);
    UT_ASSERT_MSG(pf != NULL, "no pathfinder");
    if (!run_slate(pf, ROOT_X, ROOT_Y)) {
        brainPathfinderDestroy(pf);
        UT_FAIL("slate did not complete");
    }
    /* g of the first chain square = cost of one plain grass step. */
    float g1 = min_layer(pf, ROOT_X + 1, ROOT_Y);
    float speed = pf->terrain_speed_table[TT_GRASS];
    if (speed < 0.1f) speed = 0.1f;
    /* Hazard = danger * 16 / speed. Square ahead (100,101): 2 x g1, more
     * than the hazard bound allows (0 + one grass step). Root: 4 x g1. */
    const int AX = ROOT_X, AY = ROOT_Y + 1;
    brainPathfinderSetDanger(pf, AX, AY, 2.0f * g1 * speed / 16.0f);
    brainPathfinderSetDanger(pf, ROOT_X, ROOT_Y, 4.0f * g1 * speed / 16.0f);

    /* The tank is south-west of the root, off the chain. */
    int nx = -1, ny = -1;
    int ok = brainPathfinderDijkstraNextStep(pf, KIND, ROOT_X - 1, ROOT_Y + 1,
                                             DST_X, DST_Y, NULL, 0, 0.0f, 0,
                                             &nx, &ny);
    brainPathfinderDestroy(pf);
    UT_ASSERT_MSG(g1 > 0.0f && g1 < 1e29f, "bad g1 %.3f", g1);
    UT_ASSERT_MSG(ok == 1, "no next step");
    UT_ASSERT_MSG(!(nx == ROOT_X && ny == ROOT_Y),
                  "next step is the stale root (%d,%d)", nx, ny);
    UT_ASSERT_MSG(nx == AX && ny == AY, "next (%d,%d), want (%d,%d)",
                  nx, ny, AX, AY);
    return 0;
}

int run_pf_offchain_dead_end_keeps_root(void) {
    /* A one-square corridor east from the root, and one pocket square south
     * of the root. From the pocket, the root is the only legal step. */
    memset(g_map, TT_DEEPSEA, sizeof(g_map));
    for (int x = ROOT_X; x <= DST_X; x++) g_map[ROOT_Y * PF_MAPSZ + x] = TT_GRASS;
    g_map[(ROOT_Y + 1) * PF_MAPSZ + ROOT_X] = TT_GRASS;
    for (int veer = 0; veer <= 1; veer++) {
        BrainPathfinder *pf = make_pf((float)veer);
        UT_ASSERT_MSG(pf != NULL, "no pathfinder");
        if (!run_slate(pf, ROOT_X, ROOT_Y)) {
            brainPathfinderDestroy(pf);
            UT_FAIL("slate did not complete (veer %d)", veer);
        }
        int nx = -1, ny = -1;
        int ok = brainPathfinderDijkstraNextStep(pf, KIND, ROOT_X, ROOT_Y + 1,
                                                 DST_X, DST_Y, NULL, 0, 0.0f, 0,
                                                 &nx, &ny);
        brainPathfinderDestroy(pf);
        UT_ASSERT_MSG(ok == 1, "no next step (veer %d)", veer);
        UT_ASSERT_MSG(nx == ROOT_X && ny == ROOT_Y,
                      "veer %d: next (%d,%d), want the root (%d,%d)",
                      veer, nx, ny, ROOT_X, ROOT_Y);
    }
    return 0;
}
