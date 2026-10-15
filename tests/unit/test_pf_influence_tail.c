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
 *   - with EXPAND_DEEP_MARGIN set, no tile within that many king-moves of deep
 *     sea or of the map edge is claimed;
 *   - contact between tails produces a front line; no contact, no line;
 *   - with enemy_tail = 0 (constants.lua INFLUENCE_ENEMY_TAIL) only our cores
 *     grow: the enemy keeps its raw stamped disc, our tail is no longer
 *     cancelled, and the front line moves up against that disc.
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

/* Both sides grow (enemy_tail = 1): the pre-INFLUENCE_ENEMY_TAIL behaviour,
 * which is what every test below except the friendly-only one pins. */
static void rebuild_merge(BrainPathfinder *pf) {
    brainPathfinderRebuildInfluenceTail(pf, SEED_MIN, RADIUS, START, NSTEP, WSTEP, 0, 1);
    brainPathfinderMergeInfluenceTail(pf);
}

/* Same, with the deep-water margin (EXPAND_DEEP_MARGIN) active. */
static void rebuild_merge_margin(BrainPathfinder *pf, int margin) {
    brainPathfinderRebuildInfluenceTail(pf, SEED_MIN, RADIUS, START, NSTEP, WSTEP, margin, 1);
    brainPathfinderMergeInfluenceTail(pf);
}

/* Only our cores grow (enemy_tail = 0, constants.lua INFLUENCE_ENEMY_TAIL). */
static void rebuild_merge_friendly_only(BrainPathfinder *pf) {
    brainPathfinderRebuildInfluenceTail(pf, SEED_MIN, RADIUS, START, NSTEP, WSTEP, 0, 0);
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

int run_pf_tail_deep_margin_keeps_off_the_shore(void) {
    BrainPathfinder *pf = fresh_pf();
    const int MARGIN = 4;
    int y;
    UT_ASSERT(pf != NULL);
    /* A deep-sea column at x=97: with MARGIN=4 the tail may not claim
     * x=93..101 (x=102 is the first tile more than 4 king-moves clear). */
    for (y = 60; y <= 140; y++) g_map[y * PF_MAPSZ + 97] = TT_DEEPSEA;
    seed(pf, 105, 100, 100);
    rebuild_merge_margin(pf, MARGIN);
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 105, 100) == 100, "core overwritten");
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 103, 100) == expect(2),
                  "outside the margin: got %d want %d",
                  brainPathfinderInfluenceAt(pf, 103, 100), expect(2));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 102, 100) == expect(3),
                  "first tile clear of the margin: got %d want %d",
                  brainPathfinderInfluenceAt(pf, 102, 100), expect(3));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 101, 100) == 0,
                  "claimed a tile 4 from deep sea: %d",
                  brainPathfinderInfluenceAt(pf, 101, 100));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 98, 100) == 0, "claimed the shore tile");
    /* The map edge counts as deep sea too. */
    brainPathfinderClearInfluence(pf);
    seed(pf, 8, 100, 100);
    rebuild_merge_margin(pf, MARGIN);
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 4, 100) == expect(4),
                  "first tile clear of the edge margin: got %d want %d",
                  brainPathfinderInfluenceAt(pf, 4, 100), expect(4));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 3, 100) == 0,
                  "claimed a tile 4 from the map edge: %d",
                  brainPathfinderInfluenceAt(pf, 3, 100));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 0, 100) == 0, "claimed the map border");
    /* margin 0 is the old behaviour: the shore tile next to the sea is claimed
     * again (same pf, so this also proves the mask is not left over). */
    brainPathfinderClearInfluence(pf);
    seed(pf, 105, 100, 100);
    rebuild_merge_margin(pf, 0);
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 98, 100) == expect(7),
                  "margin 0 should claim the shore: got %d want %d",
                  brainPathfinderInfluenceAt(pf, 98, 100), expect(7));
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

    /* enemy_tail = 0: the same pair of cores, but only ours grows. Nothing
     * cancels our tail, so it runs all the way up to the enemy stamp and the
     * front line sits against the enemy's disc instead of midway. With cores
     * 10 apart that moves the line from x=104/105 to x=109/110 -- five tiles
     * of ground that used to read as theirs now reads as ours. */
    brainPathfinderClearInfluence(pf);
    seed(pf, 100, 100,  100);
    seed(pf, 110, 100, -100);
    rebuild_merge_friendly_only(pf);
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 105, 100) == expect(5),
                  "midpoint should be ours now: got %d want %d",
                  brainPathfinderInfluenceAt(pf, 105, 100), expect(5));
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 109, 100) == expect(9),
                  "tile next to the enemy core should be ours: got %d want %d",
                  brainPathfinderInfluenceAt(pf, 109, 100), expect(9));
    /* The enemy stamp itself is untouched -- Merge keeps the bigger magnitude,
     * so the enemy's footprint is intact; only its faint outer ring can lose. */
    UT_ASSERT_MSG(brainPathfinderInfluenceAt(pf, 110, 100) == -100,
                  "enemy stamp overwritten: %d", brainPathfinderInfluenceAt(pf, 110, 100));
    /* No tail cell is negative any more: the hostile pass did not run. */
    {
        int i;
        for (i = 0; i < PF_MAPSZ * PF_MAPSZ; i++)
            UT_ASSERT_MSG(pf->expand_grid[i] >= 0,
                          "hostile tail grew at idx %d: %d", i, pf->expand_grid[i]);
    }
    n = brainPathfinderFindFrontLine(pf, fx, fy, 4096);
    UT_ASSERT_MSG(n > 0, "friendly-only tail produced no front line");
    seen = 0;
    for (i = 0; i < n; i++)
        if (fy[i] == 100 && (fx[i] == 109 || fx[i] == 110)) seen = 1;
    UT_ASSERT_MSG(seen, "front line did not move up against the enemy core");
    brainPathfinderDestroy(pf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Own-ground distance (brainPathfinderBuildOwnDist): the Chebyshev      */
/* distance from every tile to the nearest tile with influence > 0,     */
/* clamped to a cap. GoalHunter's turtle far-base cost reads it.        */
/* ------------------------------------------------------------------ */

static int brute_own_dist(const int *sx, const int *sy, int n, int x, int y, int cap) {
    int i, best = cap;
    for (i = 0; i < n; i++) {
        int dx = sx[i] - x, dy = sy[i] - y, d;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        d = dx > dy ? dx : dy;
        if (d < best) best = d;
    }
    return best;
}

int run_pf_own_dist_matches_brute_force(void) {
    BrainPathfinder *pf = fresh_pf();
    int sx[24], sy[24], n = 0, i, x, y, got;
    unsigned int lcg = 12345u;
    UT_ASSERT(pf != NULL);
    /* 24 positive tiles (some on the map edge), plus negative and zero
     * tiles that must not count as ground. */
    for (i = 0; i < 24; i++) {
        lcg = lcg * 1103515245u + 12345u; x = (int)((lcg >> 16) & 255);
        lcg = lcg * 1103515245u + 12345u; y = (int)((lcg >> 16) & 255);
        if (i == 0) { x = 0; y = 0; }
        if (i == 1) { x = 255; y = 128; }
        seed(pf, x, y, 5 + i);
        sx[n] = x; sy[n] = y; n++;
    }
    for (i = 0; i < 40; i++) {
        lcg = lcg * 1103515245u + 12345u; x = (int)((lcg >> 16) & 255);
        lcg = lcg * 1103515245u + 12345u; y = (int)((lcg >> 16) & 255);
        if (pf->influence_grid[y * PF_MAPSZ + x] <= 0) seed(pf, x, y, -50);
    }
    got = brainPathfinderBuildOwnDist(pf, 64);
    UT_ASSERT_MSG(got == n, "source count %d, want %d", got, n);
    for (y = 0; y < PF_MAPSZ; y++) {
        for (x = 0; x < PF_MAPSZ; x++) {
            int want = brute_own_dist(sx, sy, n, x, y, 64);
            int have = brainPathfinderOwnDistAt(pf, x, y);
            UT_ASSERT_MSG(have == want, "(%d,%d): got %d want %d", x, y, have, want);
        }
    }
    brainPathfinderDestroy(pf);
    return 0;
}

int run_pf_own_dist_cap_empty_and_edges(void) {
    BrainPathfinder *pf = fresh_pf();
    UT_ASSERT(pf != NULL);
    /* No positive tile: count 0, every tile reads the cap. */
    seed(pf, 100, 100, -100);
    UT_ASSERT(brainPathfinderBuildOwnDist(pf, 32) == 0);
    UT_ASSERT(brainPathfinderOwnDistAt(pf, 100, 100) == 32);
    UT_ASSERT(brainPathfinderOwnDistAt(pf, 0, 255) == 32);
    /* Off the map reads 255. */
    UT_ASSERT(brainPathfinderOwnDistAt(pf, -1, 0) == 255);
    UT_ASSERT(brainPathfinderOwnDistAt(pf, 0, 256) == 255);
    /* One source in the far corner: the backward sweep carries it. */
    seed(pf, 255, 255, 1);
    UT_ASSERT(brainPathfinderBuildOwnDist(pf, 32) == 1);
    UT_ASSERT(brainPathfinderOwnDistAt(pf, 255, 255) == 0);
    UT_ASSERT_MSG(brainPathfinderOwnDistAt(pf, 250, 255) == 5, "got %d", brainPathfinderOwnDistAt(pf, 250, 255));
    UT_ASSERT_MSG(brainPathfinderOwnDistAt(pf, 245, 240) == 15, "diagonal: got %d", brainPathfinderOwnDistAt(pf, 245, 240));
    UT_ASSERT(brainPathfinderOwnDistAt(pf, 200, 255) == 32);   /* 55 away, clamped */
    /* The cap is clamped to 254. */
    UT_ASSERT(brainPathfinderBuildOwnDist(pf, 1000) == 1);
    UT_ASSERT_MSG(brainPathfinderOwnDistAt(pf, 0, 0) == 254, "got %d", brainPathfinderOwnDistAt(pf, 0, 0));
    UT_ASSERT(brainPathfinderOwnDistAt(pf, 5, 255) == 250);
    brainPathfinderDestroy(pf);
    return 0;
}
