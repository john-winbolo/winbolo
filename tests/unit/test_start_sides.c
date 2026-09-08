/*
 * Start side classification tests (test_start_sides.c).
 *
 * start_sides.h decides which side of the map a start is on from its
 * position inside the start bounding box, and whether a team with a chosen
 * side (or no side) may be placed on it. The lobby's compass labels and the
 * placement code both read the same mask, so these tests pin the geometry
 * directly against the header:
 *
 *   (1) the eight sectors and the centre band on a synthetic 0..200 box;
 *   (2) a box collapsed to one point still reads as centre;
 *   (3) eligibility: a team with no side stays off the sides other teams
 *       chose, while a side team is bound only by its own side.
 *
 * No sim is needed; every helper is a pure function of its arguments.
 */

#include <stdbool.h>
#include <stdio.h>

#include "start_sides.h"
#include "test_harness.h"

/* Synthetic box: centre (100,100), tolerance 25 per axis. */
#define K_MIN 0
#define K_MAX 200

/* (1) Sector table: one bit for a cardinal sector, two for a diagonal,
 *     none inside the centre band. */
int run_start_side_mask_sectors(void) {
    static const struct {
        int sx;
        int sy;
        BYTE want;
        const char *what;
    } k_cases[] = {
        { 180,  20, START_SIDE_BIT_N | START_SIDE_BIT_E, "NE start" },
        { 100,  10, START_SIDE_BIT_N,                    "due-north start" },
        { 100, 100, 0,                                   "bbox centre" },
        {  20, 180, START_SIDE_BIT_S | START_SIDE_BIT_W, "SW start" },
        { 200,  70, START_SIDE_BIT_E,                    "dx=100 dy=-30, east sector" },
    };
    size_t i;
    for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        BYTE got = startSideMaskFor(k_cases[i].sx, k_cases[i].sy,
                                    K_MIN, K_MIN, K_MAX, K_MAX);
        UT_ASSERT_MSG(got == k_cases[i].want,
                      "%s at (%d,%d): want mask 0x%02x, got 0x%02x",
                      k_cases[i].what, k_cases[i].sx, k_cases[i].sy,
                      (unsigned)k_cases[i].want, (unsigned)got);
    }
    return 0;
}

/* (2) Every start at one point: the box has no extent, the tolerance
 *     clamps to 1, and the start reads as centre. */
int run_start_side_mask_degenerate_bbox(void) {
    static const int k_pts[] = { 0, 50, 128, 255 };
    size_t i;
    for (i = 0; i < sizeof(k_pts) / sizeof(k_pts[0]); i++) {
        int p = k_pts[i];
        BYTE got = startSideMaskFor(p, p, p, p, p, p);
        UT_ASSERT_MSG(got == 0,
                      "start at (%d,%d) in a one-point bbox: want centre, got 0x%02x",
                      p, p, (unsigned)got);
        UT_ASSERT_MSG(startSideIsCentre(got),
                      "startSideIsCentre should agree for (%d,%d)", p, p);
    }
    return 0;
}

/* (3) Eligibility. A team with no side is kept off the sides other teams
 *     chose (closedMask); a side team ignores closedMask and takes what its
 *     own side accepts, centre starts included. */
int run_start_side_eligible_closed_mask(void) {
    static const BYTE k_closed[] = {
        0,
        START_SIDE_BIT_N,
        START_SIDE_BIT_N | START_SIDE_BIT_E | START_SIDE_BIT_S | START_SIDE_BIT_W,
    };
    size_t i;

    /* ANY team, north closed by someone else. */
    UT_ASSERT_MSG(!startSideEligible(START_SIDE_BIT_N, START_SIDE_ANY,
                                     START_SIDE_BIT_N),
                  "ANY team should reject an N start when N is closed");
    UT_ASSERT_MSG(!startSideEligible(START_SIDE_BIT_N | START_SIDE_BIT_E,
                                     START_SIDE_ANY, START_SIDE_BIT_N),
                  "ANY team should reject an N|E start when N is closed");
    UT_ASSERT_MSG(startSideEligible(START_SIDE_BIT_S | START_SIDE_BIT_E,
                                    START_SIDE_ANY, START_SIDE_BIT_N),
                  "ANY team should accept an S|E start when N is closed");
    UT_ASSERT_MSG(startSideEligible(0, START_SIDE_ANY, START_SIDE_BIT_N),
                  "ANY team should accept a centre start when N is closed");

    /* Side-N team: closedMask makes no difference. */
    for (i = 0; i < sizeof(k_closed) / sizeof(k_closed[0]); i++) {
        UT_ASSERT_MSG(startSideEligible(START_SIDE_BIT_N | START_SIDE_BIT_E,
                                        START_SIDE_N, k_closed[i]),
                      "side-N team should accept an N|E start with closed 0x%02x",
                      (unsigned)k_closed[i]);
        UT_ASSERT_MSG(startSideEligible(0, START_SIDE_N, k_closed[i]),
                      "side-N team should accept a centre start with closed 0x%02x",
                      (unsigned)k_closed[i]);
        UT_ASSERT_MSG(!startSideEligible(START_SIDE_BIT_S, START_SIDE_N,
                                         k_closed[i]),
                      "side-N team should reject an S start with closed 0x%02x",
                      (unsigned)k_closed[i]);
    }
    return 0;
}
