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
 * run_util_spiral_rings      — both orders, sixty rings out, from a
 *                              centre in the map and one by its corner
 *
 * The first two cases list the centre and the first two rings, as offsets
 * from the centre, and check the ring number of each square. The third
 * checks what makes a ring without listing it: it begins on the right
 * square, goes east from there, moves one square at a time, stays on its
 * ring, visits no square twice and has 8 * ring squares.
 */

#include <stdlib.h>
#include <string.h>

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

#define SPIRAL_RINGS 60
#define SPIRAL_SPAN (2 * SPIRAL_RINGS + 1)

static int spiral_rings_check(utilSpiralStartAt startAt, int cx, int cy) {
    static unsigned char seen[SPIRAL_SPAN][SPIRAL_SPAN];
    utilSpiralCursor s;
    int ring;
    int i;
    int dx, dy;
    int prevX, prevY;
    int wantX;

    utilSpiralStart(&s, cx, cy, startAt);
    UT_ASSERT(s.x == cx && s.y == cy && utilSpiralRing(&s) == 0);
    utilSpiralStep(&s);
    for (ring = 1; ring <= SPIRAL_RINGS; ring++) {
        memset(seen, 0, sizeof(seen));
        /* Where the ring begins: due north of the centre, or the corner. */
        wantX = startAt == UTIL_SPIRAL_NORTH_WEST ? -ring : 0;
        UT_ASSERT_MSG(s.x - cx == wantX && s.y - cy == -ring,
                      "ring %d begins on (%d,%d), wanted (%d,%d)", ring,
                      s.x - cx, s.y - cy, wantX, -ring);
        prevX = s.x;
        prevY = s.y;
        for (i = 0; i < 8 * ring; i++) {
            dx = s.x - cx;
            dy = s.y - cy;
            UT_ASSERT_MSG(utilSpiralRing(&s) == ring,
                          "square %d of ring %d is on ring %d", i, ring,
                          utilSpiralRing(&s));
            if (i == 1) {
                /* Clockwise: the second square is east of the first. */
                UT_ASSERT_MSG(s.x == prevX + 1 && s.y == prevY,
                              "ring %d goes to (%d,%d) from its first square",
                              ring, dx, dy);
            } else if (i > 1) {
                UT_ASSERT_MSG(abs(s.x - prevX) + abs(s.y - prevY) == 1,
                              "ring %d steps from (%d,%d) to (%d,%d)", ring,
                              prevX - cx, prevY - cy, dx, dy);
            }
            UT_ASSERT_MSG(!seen[dy + SPIRAL_RINGS][dx + SPIRAL_RINGS],
                          "ring %d visits (%d,%d) twice", ring, dx, dy);
            seen[dy + SPIRAL_RINGS][dx + SPIRAL_RINGS] = 1;
            prevX = s.x;
            prevY = s.y;
            utilSpiralStep(&s);
        }
    }
    UT_ASSERT_MSG(utilSpiralRing(&s) == SPIRAL_RINGS + 1,
                  "after ring %d the cursor is on ring %d", SPIRAL_RINGS,
                  utilSpiralRing(&s));
    return 0;
}

int run_util_spiral_rings(void) {
    /* The second centre is by the map's corner, so most of each ring is off
     * the map, at squares below zero. */
    if (spiral_rings_check(UTIL_SPIRAL_NORTH, SPIRAL_CX, SPIRAL_CY) != 0 ||
        spiral_rings_check(UTIL_SPIRAL_NORTH_WEST, SPIRAL_CX, SPIRAL_CY) != 0 ||
        spiral_rings_check(UTIL_SPIRAL_NORTH, 2, 3) != 0 ||
        spiral_rings_check(UTIL_SPIRAL_NORTH_WEST, 2, 3) != 0) {
        return 1;
    }
    return 0;
}
