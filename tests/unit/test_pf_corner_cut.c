/*
 * Diagonal corner-cut rule consistency across the pathfinder's three
 * searchers (test_pf_corner_cut.c).
 *
 * Regression for 20260713_233008_1 bot1 t=11467 (map "Gothic Industrial"):
 * the nav Dijkstra planned (112,141)->(111,142) — a diagonal squeezing past
 * the halfbuilding at (111,141). A full-tile tank cannot cut that corner;
 * the bot ground against it at full throttle for ~110 ticks until stuck
 * recovery blacklisted the tile.
 *
 * Root cause: the corner-cut rule exists in three places and diverged. The
 * A* (brainPathfinderPathTo) was fixed to "on foot, EITHER cardinal corner
 * solid blocks the diagonal", but the precomputed edge cache feeding the
 * Dijkstra slates (brainPathfinderRebuildEdgeCosts) and the cost_to
 * expansion kept the old "BOTH corners solid" rule — so the nav Dijkstra
 * emitted diagonals the tank physically cannot drive and the click-A*
 * correctly refused.
 *
 * These tests rebuild the incident geometry and assert the invariant on
 * all three searchers' traced paths: no diagonal step may pass a solid
 * (building / halfbuilding) cardinal corner.
 */

#include <stdio.h>
#include <string.h>

#include "brain_pathfinder.h"
#include "test_harness.h"

#define TT_BUILDING  0
#define TT_ROAD      4
#define TT_FOREST    5
#define TT_RUBBLE    6
#define TT_GRASS     7
#define TT_HALFBUILD 8

#define PF_MAPSZ 256

/* Incident geometry: Gothic Industrial rows y=138..146, x=104..115.
 * 'T' marks the tank tile (road), 'B' the refuel base tile (road). */
static const char *kIncidentRows[] = {
    /* y=138 */ ".==========.",
    /* y=139 */ ".===hr=====.",
    /* y=140 */ ".===hh=====.",
    /* y=141 */ "F.===rrhT==.",
    /* y=142 */ "F..===rr===.",
    /* y=143 */ "FF.====hhh=.",
    /* y=144 */ "FF.=B===hh=.",
    /* y=145 */ "F.=========.",
    /* y=146 */ ".===...====.",
};
#define INCIDENT_X0   104
#define INCIDENT_Y0   138
#define INCIDENT_ROWS 9

#define SRC_X 112
#define SRC_Y 141
#define DST_X 108
#define DST_Y 144

static BYTE g_map[PF_MAPSZ * PF_MAPSZ];

static BYTE char_to_type(char c) {
    switch (c) {
    case '=': case 'T': case 'B': return TT_ROAD;
    case 'r': return TT_RUBBLE;
    case 'h': return TT_HALFBUILD;
    case '#': return TT_BUILDING;
    case 'F': return TT_FOREST;
    default:  return TT_GRASS;
    }
}

static void build_incident_map(void) {
    memset(g_map, TT_GRASS, sizeof(g_map));
    for (int r = 0; r < INCIDENT_ROWS; r++) {
        const char *row = kIncidentRows[r];
        for (int i = 0; row[i] != '\0'; i++) {
            g_map[(INCIDENT_Y0 + r) * PF_MAPSZ + (INCIDENT_X0 + i)] =
                char_to_type(row[i]);
        }
    }
}

static int tile_type(int x, int y) {
    return g_map[y * PF_MAPSZ + x] & 0x0F;
}

static int is_solid(int tt) {
    return tt == TT_BUILDING || tt == TT_HALFBUILD;
}

/* The invariant: an on-foot path may never take a diagonal step whose
 * either cardinal corner tile is solid. Also sanity-check contiguity.
 * Returns 0 if clean, 1 (with a diagnostic) on the first violation. */
static int check_no_corner_cut(const char *who,
                               const int *px, const int *py, int n) {
    for (int i = 1; i < n; i++) {
        int dx = px[i] - px[i - 1];
        int dy = py[i] - py[i - 1];
        if (dx < -1 || dx > 1 || dy < -1 || dy > 1 || (dx == 0 && dy == 0)) {
            fprintf(stderr, "FAIL %s: non-contiguous path step %d: "
                    "(%d,%d)->(%d,%d)\n",
                    who, i, px[i - 1], py[i - 1], px[i], py[i]);
            return 1;
        }
        if (dx != 0 && dy != 0) {
            int corner_a = tile_type(px[i - 1] + dx, py[i - 1]);
            int corner_b = tile_type(px[i - 1], py[i - 1] + dy);
            if (is_solid(corner_a) || is_solid(corner_b)) {
                fprintf(stderr, "FAIL %s: step %d (%d,%d)->(%d,%d) cuts a "
                        "solid corner (adjacent types %d / %d) — a full-tile "
                        "tank cannot make this move\n",
                        who, i, px[i - 1], py[i - 1], px[i], py[i],
                        corner_a, corner_b);
                return 1;
            }
        }
    }
    return 0;
}

/* Nav Dijkstra (the slate machinery GoalHunter's cpathfinder uses). This is
 * the path the bot actually drives; before the fix it cut the halfbuilding
 * corner at (111,141) exactly as in the field incident. */
int run_pf_dijkstra_no_solid_corner_cut(void) {
    build_incident_map();
    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetMap(pf, g_map);

    brainPathfinderDijkstraStart(pf, /*slate*/0, /*tick*/1, SRC_X, SRC_Y,
                                 /*in_boat*/0, /*shells*/30, /*trees*/20,
                                 /*mines*/0, /*armour*/40,
                                 /*max_cost*/0.0f, /*exact*/0,
                                 /*danger_scale*/1.0f, /*kind*/1,
                                 /*allow_boat*/0);
    int done = 0;
    for (int i = 0; i < 200 && !done; i++) {
        done = brainPathfinderDijkstraStep(pf, 0, 2 + i, 100000);
    }
    UT_ASSERT_MSG(done, "dijkstra slate did not complete");

    float c = brainPathfinderDijkstraCostAt(pf, 0, DST_X, DST_Y, 0);
    UT_ASSERT_MSG(c < 1e29f, "destination unreachable (cost=%g)", c);

    int px[512], py[512];
    int n = brainPathfinderDijkstraTracePath(pf, 0, DST_X, DST_Y, px, py, 512);
    UT_ASSERT_MSG(n > 0, "no traced path from dijkstra slate");

    int bad = check_no_corner_cut("dijkstra", px, py, n);
    brainPathfinderDestroy(pf);
    return bad;
}

/* A* (brainPathfinderPathTo) — the searcher behind BrainTest's click-a-path
 * debug tool. Already carried the fixed either-corner rule; pinned here so
 * the three searchers can't silently diverge again. */
int run_pf_astar_no_solid_corner_cut(void) {
    build_incident_map();
    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetMap(pf, g_map);

    int nx = -1, ny = -1;
    int rc = 0;
    for (int i = 0; i < 200; i++) {
        rc = brainPathfinderPathTo(pf, SRC_X, SRC_Y, DST_X, DST_Y,
                                   /*in_boat*/0, /*shells*/30, /*trees*/20,
                                   /*mines*/0, /*armour*/40,
                                   /*budget*/100000, &nx, &ny);
        if (rc != 0) break;
    }
    UT_ASSERT_MSG(rc == 1, "A* did not complete (rc=%d)", rc);

    int px[512], py[512];
    int n = brainPathfinderTracePath(pf, px, py, 512);
    UT_ASSERT_MSG(n > 0, "no traced path from A*");

    int bad = check_no_corner_cut("astar", px, py, n);
    brainPathfinderDestroy(pf);
    return bad;
}

/* cost_to (one-shot A* cost surface) — the third copy of the corner rule. */
int run_pf_costto_no_solid_corner_cut(void) {
    build_incident_map();
    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetMap(pf, g_map);

    float c = brainPathfinderCostTo(pf, SRC_X, SRC_Y, DST_X, DST_Y,
                                    /*in_boat*/0, /*shells*/30, /*trees*/20,
                                    /*mines*/0, /*armour*/40,
                                    /*budget*/2000000);
    UT_ASSERT_MSG(c < 1e29f, "cost_to found no route (cost=%g)", c);

    int px[512], py[512];
    int n = brainPathfinderTraceLastSearchPath(pf, DST_X, DST_Y, px, py, 512);
    UT_ASSERT_MSG(n > 0, "no traced path from cost_to");

    int bad = check_no_corner_cut("cost_to", px, py, n);
    brainPathfinderDestroy(pf);
    return bad;
}
