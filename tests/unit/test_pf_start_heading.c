/*
 * Dijkstra slate root heading (test_pf_start_heading.c).
 *
 * Regression for a recorded game, player 2, brain ticks 2147-2328: the slate
 * root has no arrival direction, so its first steps paid no turn cost. With
 * two routes of near equal cost the slate picked the step behind the tank,
 * and steering made a U-turn at speed 48 that became a wide loop. The
 * "start_heading" config key (GoalHunter PF_START_HEADING) gives the root the
 * tank heading, so its first steps pay the same turn cost as any other step.
 *
 * Geometry: deep sea everywhere and a one-square-wide grass ring, columns
 * x=100 and x=110 from y=95 to y=105, rows y=95 and y=105 from x=100 to
 * x=110. Root (100,100) on foot, destination (110,100). The way round by
 * the north and the way round by the south cost exactly the same.
 *
 *   - knob on, heading north: the first step is (100,99);
 *   - knob on, heading south: the first step is (100,101);
 *   - knob on, heading 16 (half way between north and north-east): north;
 *   - knob off (heading given or not), or knob on with no heading given:
 *     every cost and the traced route match a slate with no heading at all.
 */

#include <stdio.h>
#include <string.h>

#include "brain_pathfinder.h"
#include "test_harness.h"

#define TT_GRASS     7
#define TT_DEEPSEA  10

#define PF_MAPSZ 256

#define RING_X0 100
#define RING_X1 110
#define RING_Y0  95
#define RING_Y1 105

#define SRC_X 100
#define SRC_Y 100
#define DST_X 110
#define DST_Y 100

#define HEADING_NONE -1

static BYTE g_map[PF_MAPSZ * PF_MAPSZ];

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

/* Make a pathfinder on the ring map and run one slate to completion from
 * the root. knob: value of the "start_heading" config key, or -1 to leave
 * it unset. heading: tank direction 0..255, or HEADING_NONE for no call. */
static BrainPathfinder *make_slate(int knob, int heading) {
    build_ring_map();

    BrainPathfinder *pf = brainPathfinderCreate();
    if (pf == NULL) return NULL;
    brainPathfinderSetMap(pf, g_map);
    /* The bot runs with both next-step rules on. */
    brainPathfinderSetConfig(pf, "nextstep_chain_veer", 1.0f);
    brainPathfinderSetConfig(pf, "nextstep_foot_sea_rule", 1.0f);
    if (knob >= 0) brainPathfinderSetConfig(pf, "start_heading", (float)knob);

    brainPathfinderDijkstraStart(pf, /*slate*/0, /*tick*/1, SRC_X, SRC_Y,
                                 /*in_boat*/0, /*shells*/0, /*trees*/0,
                                 /*mines*/0, /*armour*/40,
                                 /*max_cost*/0.0f, /*exact*/0,
                                 /*danger_scale*/1.0f, /*kind*/1,
                                 /*allow_boat*/1);
    if (heading != HEADING_NONE) {
        brainPathfinderDijkstraSetSourceHeading(pf, 0, heading);
    }
    int done = 0;
    for (int i = 0; i < 200 && !done; i++) {
        done = brainPathfinderDijkstraStep(pf, 0, 2 + i, 100000);
    }
    if (!done) {
        brainPathfinderDestroy(pf);
        return NULL;
    }
    return pf;
}

/* First step of the traced route to the destination, and the next step a
 * tank standing on the root is handed. Returns 0 on success. */
static int first_steps(BrainPathfinder *pf, int *tx, int *ty, int *nx, int *ny) {
    int px[512], py[512];
    int n = brainPathfinderDijkstraTracePath(pf, 0, DST_X, DST_Y, px, py, 512);
    if (n < 2 || px[0] != SRC_X || py[0] != SRC_Y) return 1;
    *tx = px[1];
    *ty = py[1];
    if (!brainPathfinderDijkstraNextStep(pf, /*kind*/1, SRC_X, SRC_Y,
                                         DST_X, DST_Y, NULL, 0, 0.0f,
                                         /*tank_in_boat*/0, nx, ny)) {
        return 1;
    }
    return 0;
}

int run_pf_start_heading_first_step_goes_ahead(void) {
    static const struct { int heading; int want_dy; const char *name; } cases[] = {
        {   0, -1, "north" },
        { 128, +1, "south" },
        {  16, -1, "half way north/north-east" },
        { 112, +1, "half way south-east/south" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        BrainPathfinder *pf = make_slate(/*knob*/1, cases[i].heading);
        UT_ASSERT_MSG(pf != NULL, "dijkstra slate did not complete (%s)",
                      cases[i].name);
        int tx = -1, ty = -1, nx = -1, ny = -1;
        int rc = first_steps(pf, &tx, &ty, &nx, &ny);
        brainPathfinderDestroy(pf);
        UT_ASSERT_MSG(rc == 0, "no route or next step (%s)", cases[i].name);
        UT_ASSERT_MSG(tx == SRC_X && ty == SRC_Y + cases[i].want_dy,
                      "heading %d (%s): route starts (%d,%d), want (%d,%d)",
                      cases[i].heading, cases[i].name, tx, ty,
                      SRC_X, SRC_Y + cases[i].want_dy);
        UT_ASSERT_MSG(nx == SRC_X && ny == SRC_Y + cases[i].want_dy,
                      "heading %d (%s): next step (%d,%d), want (%d,%d)",
                      cases[i].heading, cases[i].name, nx, ny,
                      SRC_X, SRC_Y + cases[i].want_dy);
    }
    return 0;
}

int run_pf_start_heading_off_keeps_old_costs(void) {
    /* Reference: the key never set and no heading call (an ANCHOR brain). */
    BrainPathfinder *ref = make_slate(/*knob*/-1, HEADING_NONE);
    UT_ASSERT_MSG(ref != NULL, "reference slate did not complete");
    int rtx, rty, rnx, rny;
    UT_ASSERT_MSG(first_steps(ref, &rtx, &rty, &rnx, &rny) == 0,
                  "reference: no route or next step");

    static const struct { int knob; int heading; const char *name; } cases[] = {
        { 0,   0,            "key off, heading north" },
        { 0, 128,            "key off, heading south" },
        { 1, HEADING_NONE,   "key on, no heading given" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        BrainPathfinder *pf = make_slate(cases[i].knob, cases[i].heading);
        UT_ASSERT_MSG(pf != NULL, "slate did not complete (%s)", cases[i].name);
        int diffs = 0, fx = -1, fy = -1, fb = -1;
        for (int b = 0; b <= 1; b++) {
            for (int y = 0; y < PF_MAPSZ; y++) {
                for (int x = 0; x < PF_MAPSZ; x++) {
                    float a = brainPathfinderDijkstraCostAt(ref, 0, x, y, b);
                    float c = brainPathfinderDijkstraCostAt(pf, 0, x, y, b);
                    if (memcmp(&a, &c, sizeof(float)) != 0) {
                        if (diffs == 0) { fx = x; fy = y; fb = b; }
                        diffs++;
                    }
                }
            }
        }
        int tx, ty, nx, ny;
        int rc = first_steps(pf, &tx, &ty, &nx, &ny);
        brainPathfinderDestroy(pf);
        if (diffs != 0 || rc != 0 || tx != rtx || ty != rty
            || nx != rnx || ny != rny) {
            brainPathfinderDestroy(ref);
        }
        UT_ASSERT_MSG(diffs == 0,
                      "%s: %d costs differ from the old slate, first at "
                      "(%d,%d) boat=%d", cases[i].name, diffs, fx, fy, fb);
        UT_ASSERT_MSG(rc == 0, "%s: no route or next step", cases[i].name);
        UT_ASSERT_MSG(tx == rtx && ty == rty,
                      "%s: route starts (%d,%d), old slate (%d,%d)",
                      cases[i].name, tx, ty, rtx, rty);
        UT_ASSERT_MSG(nx == rnx && ny == rny,
                      "%s: next step (%d,%d), old slate (%d,%d)",
                      cases[i].name, nx, ny, rnx, rny);
    }
    brainPathfinderDestroy(ref);
    return 0;
}
