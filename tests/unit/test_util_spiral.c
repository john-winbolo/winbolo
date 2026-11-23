/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */


/*
 * The spiral cursor in src/bolo/util.c (utilSpiralCursor). Both start
 * corners walk the centre and then square rings of growing radius,
 * clockwise. They differ only in the first square of a ring.
 *
 * run_util_spiral_north      — UTIL_SPIRAL_NORTH, the pill drop order:
 *                              each ring starts due north of the centre
 * run_util_spiral_north_west — UTIL_SPIRAL_NORTH_WEST, the start scatter
 *                              order: each ring starts on its north-west
 *                              corner
 *
 * Each case lists the centre and the first two rings, as offsets from the
 * centre, and checks the ring number of each square.
 */

#include <stdlib.h>

#include "global.h"
#include "util.h"
#include "test_harness.h"

#define SPIRAL_CX 100
#define SPIRAL_CY 50
#define SPIRAL_SQUARES 25 /* The centre, 8 in ring one, 16 in ring two */

static int spiral_check(utilSpiralStartAt startAt,
                        const int want[SPIRAL_SQUARES][2]) {
    utilSpiralCursor s;
    int i;
    int ring;
    int dx, dy;

    utilSpiralStart(&s, SPIRAL_CX, SPIRAL_CY, startAt);
    for (i = 0; i < SPIRAL_SQUARES; i++) {
        dx = s.x - SPIRAL_CX;
        dy = s.y - SPIRAL_CY;
        UT_ASSERT_MSG(dx == want[i][0] && dy == want[i][1],
                      "square %d is (%d,%d), wanted (%d,%d)",
                      i, dx, dy, want[i][0], want[i][1]);
        ring = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        UT_ASSERT_MSG(utilSpiralRing(&s) == ring,
                      "square %d is on ring %d, wanted %d",
                      i, utilSpiralRing(&s), ring);
        utilSpiralStep(&s);
    }
    /* The next square starts ring three. */
    UT_ASSERT_MSG(utilSpiralRing(&s) == 3, "after ring two the cursor is on "
                  "ring %d", utilSpiralRing(&s));
    return 0;
}

int run_util_spiral_north(void) {
    static const int want[SPIRAL_SQUARES][2] = {
        { 0,  0},
        { 0, -1}, { 1, -1}, { 1,  0}, { 1,  1},
        { 0,  1}, {-1,  1}, {-1,  0}, {-1, -1},
        { 0, -2}, { 1, -2}, { 2, -2}, { 2, -1},
        { 2,  0}, { 2,  1}, { 2,  2}, { 1,  2},
        { 0,  2}, {-1,  2}, {-2,  2}, {-2,  1},
        {-2,  0}, {-2, -1}, {-2, -2}, {-1, -2},
    };
    return spiral_check(UTIL_SPIRAL_NORTH, want);
}

int run_util_spiral_north_west(void) {
    static const int want[SPIRAL_SQUARES][2] = {
        { 0,  0},
        {-1, -1}, { 0, -1}, { 1, -1}, { 1,  0},
        { 1,  1}, { 0,  1}, {-1,  1}, {-1,  0},
        {-2, -2}, {-1, -2}, { 0, -2}, { 1, -2},
        { 2, -2}, { 2, -1}, { 2,  0}, { 2,  1},
        { 2,  2}, { 1,  2}, { 0,  2}, {-1,  2},
        {-2,  2}, {-2,  1}, {-2,  0}, {-2, -1},
    };
    return spiral_check(UTIL_SPIRAL_NORTH_WEST, want);
}
