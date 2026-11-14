/*
 * Route keep: price the old route again (test_pf_route_keep.c).
 *
 * GoalHunter PF_REROUTE_KEEP_MARGIN / PF_REROUTE_KEEP_FRAC. The slates are
 * rooted at the tank, and each new slate could pick a different first step
 * from a route of near equal cost, so the tank turned from one to the other
 * (debug session 20261103_165532_1_v3_anchor_helm_s205, player 2, brain
 * ticks 1524-2266). The brain saves the route it follows
 * (brainPathfinderDijkstraRouteSave) and, when a new slate wants another
 * first step, asks brainPathfinderDijkstraKeepCheck for the new route cost
 * and the cost of keeping the old step. The margin decision is in the brain.
 *
 *   pf_route_keep_reprice_matches_slate: on a mixed map, the saved route
 *     priced again from any of its squares equals the slate's own cost from
 *     that square to the destination; more danger on a route square adds
 *     exactly its slate price; a square off the route gives COST_INF.
 *   pf_route_keep_check_values: on the ring map of test_pf_start_heading.c
 *     (two ways round of equal cost), a new slate that turns the tank south
 *     gives g_keep - g_new = 8, the full-reverse turn cost of the north way;
 *     and each reason not to keep returns its own code.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "brain_pathfinder.h"
#include "test_harness.h"

#define TT_ROAD      4
#define TT_FOREST    5
#define TT_GRASS     7
#define TT_DEEPSEA  10

#define PF_MAPSZ 256
#define KIND     1
#define INF_CUT  1e29f

static BYTE g_map[PF_MAPSZ * PF_MAPSZ];

static int run_slate(BrainPathfinder *pf, int slate, uint32_t tick,
                     int sx, int sy, float max_cost, int heading) {
    brainPathfinderDijkstraStart(pf, slate, tick, sx, sy,
                                 /*in_boat*/0, /*shells*/0, /*trees*/0,
                                 /*mines*/0, /*armour*/40,
                                 max_cost, /*exact*/0,
                                 /*danger_scale*/1.0f, KIND,
                                 /*allow_boat*/1);
    if (heading >= 0) brainPathfinderDijkstraSetSourceHeading(pf, slate, heading);
    int done = 0;
    for (int i = 0; i < 200 && !done; i++) {
        done = brainPathfinderDijkstraStep(pf, slate, tick + 1 + (uint32_t)i, 100000);
    }
    return done;
}

static float min_layer(BrainPathfinder *pf, int slate, int x, int y) {
    float a = brainPathfinderDijkstraCostAt(pf, slate, x, y, 0);
    float b = brainPathfinderDijkstraCostAt(pf, slate, x, y, 1);
    return a < b ? a : b;
}

/* ---- reprice equals the slate cost ------------------------------------ */

/* A grass field 90..120 x 90..120 with a forest band and a road. */
static void build_field_map(void) {
    memset(g_map, TT_DEEPSEA, sizeof(g_map));
    for (int y = 90; y <= 120; y++) {
        for (int x = 90; x <= 120; x++) {
            BYTE t = TT_GRASS;
            if (x >= 100 && x <= 103 && y >= 92 && y <= 116) t = TT_FOREST;
            if (y == 110) t = TT_ROAD;
            g_map[y * PF_MAPSZ + x] = t;
        }
    }
}

int run_pf_route_keep_reprice_matches_slate(void) {
    build_field_map();
    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT_MSG(pf != NULL, "no pathfinder");
    brainPathfinderSetMap(pf, g_map);
    brainPathfinderSetConfig(pf, "nextstep_chain_veer", 1.0f);
    brainPathfinderSetConfig(pf, "nextstep_foot_sea_rule", 1.0f);
    /* Some danger on the field, so the route pays more than terrain. */
    for (int y = 95; y <= 105; y++) {
        for (int x = 104; x <= 110; x++) {
            brainPathfinderSetDanger(pf, x, y, (float)(3 + (x + y) % 5));
        }
    }
    const int SX = 92, SY = 100, DX = 116, DY = 103;
    if (!run_slate(pf, 0, 1, SX, SY, 0.0f, -1)) {
        brainPathfinderDestroy(pf);
        UT_FAIL("slate did not complete");
    }
    int nx = -1, ny = -1;
    int ok = brainPathfinderDijkstraNextStep(pf, KIND, SX, SY, DX, DY, NULL, 0,
                                             0.0f, 0, &nx, &ny);
    int saved = ok ? brainPathfinderDijkstraRouteSave(pf) : 0;
    if (!ok || saved < 3) {
        brainPathfinderDestroy(pf);
        UT_FAIL("no next step or route (ok=%d len=%d)", ok, saved);
    }

    int px[512], py[512];
    int n = brainPathfinderDijkstraTracePath(pf, 0, DX, DY, px, py, 512);
    float g_dst = min_layer(pf, 0, DX, DY);
    int bad = -1;
    float want_bad = 0.0f, got_bad = 0.0f;
    for (int i = 0; i < n && bad < 0; i++) {
        float g_i = (i == 0) ? 0.0f : min_layer(pf, 0, px[i], py[i]);
        float want = g_dst - g_i;
        float got = brainPathfinderDijkstraRouteReprice(pf, px[i], py[i]);
        if (fabsf(got - want) > 1e-3f * (1.0f + want)) {
            bad = i; want_bad = want; got_bad = got;
        }
    }
    if (bad >= 0) {
        brainPathfinderDestroy(pf);
        UT_FAIL("square %d (%d,%d) of %d: repriced %.4f, slate %.4f",
                bad, px[bad], py[bad], n, got_bad, want_bad);
    }

    /* More danger on one route square after the slate: the repriced cost
     * goes up by danger * 16/speed * diagonal factor of the step onto it. */
    int m = n / 2;
    float before = brainPathfinderDijkstraRouteReprice(pf, SX, SY);
    BYTE t = g_map[py[m] * PF_MAPSZ + px[m]];
    float speed = pf->terrain_speed_table[t & 0x0F];
    if (speed < 0.1f) speed = 0.1f;
    float dmul = (px[m] != px[m - 1] && py[m] != py[m - 1]) ? 1.41f : 1.0f;
    float old_d = brainPathfinderDangerAt(pf, px[m], py[m]);
    brainPathfinderSetDanger(pf, px[m], py[m], old_d + 40.0f);
    float after = brainPathfinderDijkstraRouteReprice(pf, SX, SY);
    float want_add = 40.0f * (16.0f / speed) * dmul;

    /* A square the route does not pass. */
    float off = brainPathfinderDijkstraRouteReprice(pf, 90, 120);
    brainPathfinderDestroy(pf);
    UT_ASSERT_MSG(fabsf((after - before) - want_add) < 1e-2f,
                  "danger +40 on (%d,%d): cost %.3f -> %.3f, want +%.3f",
                  px[m], py[m], before, after, want_add);
    UT_ASSERT_MSG(off >= INF_CUT, "square off the route repriced to %.3f", off);
    return 0;
}

/* ---- keep check on the ring ------------------------------------------- */

#define RING_X0 100
#define RING_X1 110
#define RING_Y0  95
#define RING_Y1 105
#define SRC_X 100
#define SRC_Y 100
#define DST_X 110
#define DST_Y 100

static void build_ring_map(void) {
    memset(g_map, TT_DEEPSEA, sizeof(g_map));
    for (int y = RING_Y0; y <= RING_Y1; y++) {
        g_map[y * PF_MAPSZ + RING_X0] = TT_GRASS;
        g_map[y * PF_MAPSZ + RING_X1] = TT_GRASS;
    }
    for (int x = RING_X0; x <= RING_X1; x++) {
        g_map[RING_Y0 * PF_MAPSZ + x] = TT_GRASS;
        g_map[RING_Y1 * PF_MAPSZ + x] = TT_GRASS;
    }
}

int run_pf_route_keep_check_values(void) {
    build_ring_map();
    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT_MSG(pf != NULL, "no pathfinder");
    brainPathfinderSetMap(pf, g_map);
    brainPathfinderSetConfig(pf, "nextstep_chain_veer", 1.0f);
    brainPathfinderSetConfig(pf, "nextstep_foot_sea_rule", 1.0f);
    brainPathfinderSetConfig(pf, "start_heading", 1.0f);

    /* Old slate: heading north, the tank follows the north way. */
    int nx = -1, ny = -1;
    int ok = run_slate(pf, 0, 1, SRC_X, SRC_Y, 0.0f, 0)
          && brainPathfinderDijkstraNextStep(pf, KIND, SRC_X, SRC_Y, DST_X, DST_Y,
                                             NULL, 0, 0.0f, 0, &nx, &ny)
          && brainPathfinderDijkstraRouteSave(pf) > 0;
    if (!ok || nx != SRC_X || ny != SRC_Y - 1) {
        brainPathfinderDestroy(pf);
        UT_FAIL("old slate: ok=%d next (%d,%d), want (%d,%d)", ok, nx, ny,
                SRC_X, SRC_Y - 1);
    }
    const int OX = SRC_X, OY = SRC_Y - 1;

    /* New slate on the same square, heading south: it wants the south way. */
    ok = run_slate(pf, 0, 300, SRC_X, SRC_Y, 0.0f, 128)
      && brainPathfinderDijkstraNextStep(pf, KIND, SRC_X, SRC_Y, DST_X, DST_Y,
                                         NULL, 0, 0.0f, 0, &nx, &ny);
    if (!ok || nx != SRC_X || ny != SRC_Y + 1) {
        brainPathfinderDestroy(pf);
        UT_FAIL("new slate: ok=%d next (%d,%d), want (%d,%d)", ok, nx, ny,
                SRC_X, SRC_Y + 1);
    }
    float g_new = -1.0f, g_keep = -1.0f;
    int rc = brainPathfinderDijkstraKeepCheck(pf, SRC_X, SRC_Y, OX, OY,
                                              DST_X, DST_Y, &g_new, &g_keep);
    float g_dst = min_layer(pf, 0, DST_X, DST_Y);
    int rc_dest = brainPathfinderDijkstraKeepCheck(pf, SRC_X, SRC_Y, OX, OY,
                                                   DST_X, DST_Y + 1, NULL, NULL);
    int rc_off = brainPathfinderDijkstraKeepCheck(pf, SRC_X, SRC_Y, SRC_X + 1,
                                                  RING_Y1, DST_X, DST_Y, NULL, NULL);
    int rc_sea = brainPathfinderDijkstraKeepCheck(pf, SRC_X, SRC_Y, SRC_X - 1,
                                                  SRC_Y - 1, DST_X, DST_Y, NULL, NULL);
    brainPathfinderSetOverlay(pf, OX, OY, 32767.0f);
    int rc_ovl = brainPathfinderDijkstraKeepCheck(pf, SRC_X, SRC_Y, OX, OY,
                                                  DST_X, DST_Y, NULL, NULL);
    brainPathfinderSetOverlay(pf, OX, OY, 0.0f);

    /* A newer slate that cannot reach the destination: the old one answers
     * as a fallback, and a fallback answer is never compared. */
    int done1 = run_slate(pf, 1, 600, SRC_X, SRC_Y, 3.0f, 128);
    int ok2 = brainPathfinderDijkstraNextStep(pf, KIND, SRC_X, SRC_Y, DST_X, DST_Y,
                                              NULL, 0, 0.0f, 0, &nx, &ny);
    int rc_backup = brainPathfinderDijkstraKeepCheck(pf, SRC_X, SRC_Y, OX, OY,
                                                     DST_X, DST_Y, NULL, NULL);
    brainPathfinderDestroy(pf);

    UT_ASSERT_MSG(rc == 0, "keep check rc=%d, want 0", rc);
    UT_ASSERT_MSG(fabsf(g_new - g_dst) < 1e-3f, "g_new %.3f, slate %.3f",
                  g_new, g_dst);
    /* turn_cost 2 per 45 degrees, full reverse = 4 x 2 = 8. */
    UT_ASSERT_MSG(fabsf((g_keep - g_new) - 8.0f) < 1e-2f,
                  "g_keep %.3f - g_new %.3f = %.3f, want 8", g_keep, g_new,
                  g_keep - g_new);
    UT_ASSERT_MSG(rc_dest == 5, "other destination: rc=%d, want 5", rc_dest);
    UT_ASSERT_MSG(rc_off == 5, "old step off the saved route: rc=%d, want 5", rc_off);
    UT_ASSERT_MSG(rc_sea == 4, "old step on deep sea: rc=%d, want 4", rc_sea);
    UT_ASSERT_MSG(rc_ovl == 3, "old step with overlay 32767: rc=%d, want 3", rc_ovl);
    UT_ASSERT_MSG(done1 && ok2, "fallback slate set-up failed (%d,%d)", done1, ok2);
    UT_ASSERT_MSG(rc_backup == 2, "older slate answered: rc=%d, want 2", rc_backup);
    return 0;
}
