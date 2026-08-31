/*
 * Influence tail (test_pf_influence_tail.c).
 *
 * brainPathfinderRebuildInfluenceTail grows each side's stamped cores
 * outward over passable ground so the blank gaps between our pills and
 * bases read as ours, and the front line forms where the two tails meet.
 * These tests pin the contract the brain relies on (constants.lua EXPAND_*):
 *
 *   - reach: value start*(1 - d/(R+1)) from the core edge, 0 past R;
 *   - a wall (building) or deep sea blocks it; it does not claim the far side;
 *   - a neutral-pill zone triples the step cost (penetrates ~1/3 as far);
 *   - the value never exceeds start;
 *   - two tails meeting cancel, and an exact tie goes to the hostile side
 *     (-1) so FindFrontLine never sees a signless zero seam;
 *   - a stamped cell is never overwritten by the tail;
 *   - contact between tails produces a front line; no contact, no line.
 */

#include <stdio.h>
#include <string.h>

#include "brain_pathfinder.h"
#include "test_harness.h"

#define TT_BUILDING  0
#define TT_RIVER     1
#define TT_GRASS     7
#define TT_DEEPSEA  10

#define PF_MAPSZ 256
#define SEED_MIN 20
#define RADIUS   10
#define START    15
#define NSTEP    3
#define WSTEP    2

static BYTE g_map[PF_MAPSZ * PF_MAPSZ];

static BrainPathfinder *fresh_pf(void) {
    memset(g_map, TT_GRASS, sizeof(g_map));
    BrainPathfinder *pf = brainPathfinderCreate();
    if (!pf) return NULL;
    brainPathfinderSetMap(pf, g_map);
    brainPathfinderClearInfluence(pf);
    brainPathfinderClearNeutralZones(pf);
    return pf;
}

static void seed(BrainPathfinder *pf, int x, int y, int v) {
    pf->influence_grid[y * PF_MAPSZ + x] = (int16_t)v;
}

static void rebuild_merge(BrainPathfinder *pf) {
    brainPathfinderRebuildInfluenceTail(pf, SEED_MIN, RADIUS, START, NSTEP, WSTEP);
    brainPathfinderMergeInfluenceTail(pf);
}

/* Expected tail value at step distance d. */
static int expect(int d) {
    return (int)((float)START * (1.0f - (float)d / (float)(RADIUS + 1)));
}

int run_pf_tail_reaches_radius_and_stops(void) {
    BrainPathfinder *pf = fresh_pf();
    UT_ASSERT(pf != NULL);
    seed(pf, 100, 100, 100);
    rebuild_merge(pf);
    /* The stamp itself is untouched. */
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 100, 100) == 100,
                  "core overwritten: %d", brainPathfinderInfluenceAt(pf, 100, 100));
    UT_ASSERT(brainPathfinderInfluenceTailAt(pf, 100, 100) == 0);
    /* One step out, ten steps out, eleven steps out. */
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 101, 100) == expect(1),
                  "d=1: got %d want %d", brainPathfinderInfluenceAt(pf, 101, 100), expect(1));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 110, 100) == expect(10),
                  "d=10: got %d want %d", brainPathfinderInfluenceAt(pf, 110, 100), expect(10));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 111, 100) == 0,
                  "d=11 should be 0, got %d", brainPathfinderInfluenceAt(pf, 111, 100));
    /* 8-neighbour: a diagonal step is one step. */
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 108, 108) == expect(8),
                  "diag d=8: got %d want %d", brainPathfinderInfluenceAt(pf, 108, 108), expect(8));
    /* Never above START anywhere. */
    {
        int mx = 0, i;
        for (i = 0; i < PF_MAPSZ * PF_MAPSZ; i++) {
            int t = pf->expand_grid[i];
            if (t < 0) t = -t;
            if (t > mx) mx = t;
        }
        UT_ASSERT_MSG(mx <= START, "tail max %d exceeds START %d", mx, START);
    }
    brainPathfinderDestroy(pf);
    return 0;
}

int run_pf_tail_blocked_by_wall_and_sea(void) {
    BrainPathfinder *pf = fresh_pf();
    int y;
    UT_ASSERT(pf != NULL);
    /* Building column at x=103 and a deep-sea column at x=97, both long
     * enough that going around costs more than RADIUS steps. */
    for (y = 80; y <= 120; y++) {
        g_map[y * PF_MAPSZ + 103] = TT_BUILDING;
        g_map[y * PF_MAPSZ + 97]  = TT_DEEPSEA;
    }
    seed(pf, 100, 100, 100);
    rebuild_merge(pf);
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 102, 100) == expect(2), "inside the wall side");
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 103, 100) == 0, "the wall itself is not claimed");
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 104, 100) == 0,
                  "far side of the wall claimed: %d", brainPathfinderInfluenceAt(pf, 104, 100));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 96, 100) == 0,
                  "far side of the sea claimed: %d", brainPathfinderInfluenceAt(pf, 96, 100));
    brainPathfinderDestroy(pf);
    return 0;
}

int run_pf_tail_neutral_zone_slows(void) {
    BrainPathfinder *pf = fresh_pf();
    UT_ASSERT(pf != NULL);
    seed(pf, 100, 100, 100);
    /* A live neutral pill at (104,100): its 8-tile range covers everything
     * from x=96 to x=112 on this row, including the seed's neighbours. */
    brainPathfinderStampNeutralZone(pf, 104, 100, 8);
    rebuild_merge(pf);
    /* Each step east now costs 3: d=3, 6, 9, then 12 > RADIUS. */
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 101, 100) == expect(3),
                  "zone step 1: got %d want %d", brainPathfinderInfluenceAt(pf, 101, 100), expect(3));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 102, 100) == expect(6),
                  "zone step 2: got %d want %d", brainPathfinderInfluenceAt(pf, 102, 100), expect(6));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 103, 100) == expect(9),
                  "zone step 3: got %d want %d", brainPathfinderInfluenceAt(pf, 103, 100), expect(9));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 104, 100) == 0,
                  "the neutral pill's own tile claimed: %d", brainPathfinderInfluenceAt(pf, 104, 100));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 108, 100) == 0, "far side of the neutral pill claimed");
    brainPathfinderDestroy(pf);
    return 0;
}

int run_pf_tail_meeting_cancels_tie_to_hostile(void) {
    BrainPathfinder *pf = fresh_pf();
    UT_ASSERT(pf != NULL);
    seed(pf, 100, 100,  100);
    seed(pf, 110, 100, -100);
    rebuild_merge(pf);
    /* Midpoint: d=5 from each, exact tie -> hostile -1, never 0. */
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 105, 100) == -1,
                  "tie should read -1, got %d", brainPathfinderInfluenceAt(pf, 105, 100));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 104, 100) == expect(4) - expect(6),
                  "our side: got %d", brainPathfinderInfluenceAt(pf, 104, 100));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 106, 100) == expect(6) - expect(4),
                  "their side: got %d", brainPathfinderInfluenceAt(pf, 106, 100));
    brainPathfinderDestroy(pf);
    return 0;
}

int run_pf_tail_never_overwrites_a_stamp(void) {
    BrainPathfinder *pf = fresh_pf();
    UT_ASSERT(pf != NULL);
    seed(pf, 100, 100, 100);
    seed(pf, 101, 100, 30);   /* inside a stamp, stronger than the tail (and,
                                 being >= SEED_MIN, a core in its own right) */
    seed(pf, 102, 100, 5);    /* a stamp's faint edge, one step from that core */
    rebuild_merge(pf);
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 101, 100) == 30, "strong stamp lost to the tail");
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 102, 100) == expect(1),
                  "faint stamp edge should take the tail value %d, got %d",
                  expect(1), brainPathfinderInfluenceAt(pf, 102, 100));
    UT_ASSERT(brainPathfinderInfluenceTailAt(pf, 101, 100) == 0);
    UT_ASSERT(brainPathfinderInfluenceTailAt(pf, 102, 100) == expect(1));
    brainPathfinderDestroy(pf);
    return 0;
}

int run_pf_tail_contact_makes_a_front_line(void) {
    BrainPathfinder *pf = fresh_pf();
    int fx[4096], fy[4096], n, i, seen = 0;
    UT_ASSERT(pf != NULL);
    seed(pf, 100, 100,  100);
    seed(pf, 110, 100, -100);
    rebuild_merge(pf);
    n = brainPathfinderFindFrontLine(pf, fx, fy, 4096);
    UT_ASSERT_MSG(n > 0, "tails in contact produced no front line");
    for (i = 0; i < n; i++) {
        if (fy[i] == 100 && (fx[i] == 104 || fx[i] == 105)) seen = 1;
        /* No front cell may be a zero: the tie rule exists for this. */
        UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, fx[i], fy[i]) != 0,
                      "front cell (%d,%d) reads 0", fx[i], fy[i]);
    }
    UT_ASSERT_MSG(seen, "front line missing at the contact (104/105,100)");

    /* Too far apart: no contact, no line. */
    brainPathfinderClearInfluence(pf);
    seed(pf, 100, 100,  100);
    seed(pf, 140, 100, -100);
    rebuild_merge(pf);
    n = brainPathfinderFindFrontLine(pf, fx, fy, 4096);
    UT_ASSERT_MSG(n == 0, "no contact but %d front cells", n);
    brainPathfinderDestroy(pf);
    return 0;
}
