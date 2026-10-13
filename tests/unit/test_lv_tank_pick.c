/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums behind a click on a tank on the log viewer's map
 * (lv_tank_pick.h).
 *
 *   lv_tank_pick_sums — that a point on a tank's picture picks that tank
 *       and one beside it picks none; that the box is never under 12 points
 *       a side, so a tank can be hit when the map is zoomed out, on a
 *       display of any pixel density; that of two tanks under the point the
 *       one with the nearer middle wins; that a tank that cannot be picked
 *       (dead) is passed over, also when it lies over a live one; and where
 *       the mark round the selected tank goes.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_tank_pick.h"
#include "test_harness.h"

int run_lv_tank_pick_sums(void) {
    LvTankPickBox t[3];
    LvTankMark m;

    /* One tank, squares of 32: the picture is 100..132 by 200..232. */
    t[0].x = 100.0f; t[0].y = 200.0f; t[0].slot = 4; t[0].pickable = true;
    UT_ASSERT_MSG(lvTankPick(t, 1, 116.0f, 216.0f, 32.0f, 1.0f) == 0, "the middle");
    UT_ASSERT_MSG(lvTankPick(t, 1, 100.0f, 200.0f, 32.0f, 1.0f) == 0, "the top left pixel");
    UT_ASSERT_MSG(lvTankPick(t, 1, 131.9f, 231.9f, 32.0f, 1.0f) == 0, "the last pixel");
    UT_ASSERT_MSG(lvTankPick(t, 1, 132.0f, 216.0f, 32.0f, 1.0f) == -1, "one past the right");
    UT_ASSERT_MSG(lvTankPick(t, 1, 99.9f, 216.0f, 32.0f, 1.0f) == -1, "one before the left");
    UT_ASSERT_MSG(lvTankPick(t, 1, 116.0f, 232.0f, 32.0f, 1.0f) == -1, "one past the bottom");
    UT_ASSERT_MSG(lvTankPick(t, 1, 116.0f, 199.9f, 32.0f, 1.0f) == -1, "one above the top");
    UT_ASSERT_MSG(lvTankPick(t, 0, 116.0f, 216.0f, 32.0f, 1.0f) == -1, "no tanks");
    UT_ASSERT_MSG(lvTankPick(NULL, 1, 116.0f, 216.0f, 32.0f, 1.0f) == -1, "no list");
    UT_ASSERT_MSG(lvTankPick(t, 1, 116.0f, 216.0f, 0.0f, 1.0f) == -1, "no square size");

    /* Zoomed out: squares of 8. The picture is 100..108; the box is 12 a
       side round its middle (104), so 98..110. */
    UT_ASSERT_MSG(lvTankPickBoxSide(8.0f, 1.0f) == 12.0f, "the least box");
    UT_ASSERT_MSG(lvTankPickBoxSide(32.0f, 1.0f) == 32.0f, "the picture when it is larger");
    UT_ASSERT_MSG(lvTankPick(t, 1, 98.0f, 204.0f, 8.0f, 1.0f) == 0, "2 px left of the picture");
    UT_ASSERT_MSG(lvTankPick(t, 1, 109.9f, 204.0f, 8.0f, 1.0f) == 0, "2 px right of it");
    UT_ASSERT_MSG(lvTankPick(t, 1, 97.9f, 204.0f, 8.0f, 1.0f) == -1, "outside the slack");
    UT_ASSERT_MSG(lvTankPick(t, 1, 110.0f, 204.0f, 8.0f, 1.0f) == -1, "outside the slack, right");
    /* The slack is in points: on a display of density 2 a square of 16
       pixels is 8 points, and the box is 24 pixels. */
    UT_ASSERT_MSG(lvTankPickBoxSide(16.0f, 2.0f) == 24.0f, "the least box at density 2");
    UT_ASSERT_MSG(lvTankPick(t, 1, 97.0f, 208.0f, 16.0f, 2.0f) == 0, "density 2: in the slack");
    UT_ASSERT_MSG(lvTankPick(t, 1, 95.9f, 208.0f, 16.0f, 2.0f) == -1, "density 2: outside");
    UT_ASSERT_MSG(lvTankPickBoxSide(16.0f, 0.0f) == 16.0f, "a density of 0 is 1");

    /* Two tanks that overlap: 100..132 and 120..152. A point on both goes
       to the nearer middle (116 and 136). */
    t[1].x = 120.0f; t[1].y = 200.0f; t[1].slot = 9; t[1].pickable = true;
    UT_ASSERT_MSG(lvTankPick(t, 2, 122.0f, 216.0f, 32.0f, 1.0f) == 0, "nearer the first");
    UT_ASSERT_MSG(lvTankPick(t, 2, 130.0f, 216.0f, 32.0f, 1.0f) == 1, "nearer the second");
    UT_ASSERT_MSG(lvTankPick(t, 2, 126.0f, 216.0f, 32.0f, 1.0f) == 0, "a tie: the earlier");
    UT_ASSERT_MSG(lvTankPick(t, 2, 140.0f, 216.0f, 32.0f, 1.0f) == 1, "on the second only");
    UT_ASSERT_MSG(lvTankPick(t, 2, 105.0f, 216.0f, 32.0f, 1.0f) == 0, "on the first only");

    /* A tank that cannot be picked is passed over, and does not hide the
       live one under it. */
    t[0].pickable = false;
    UT_ASSERT_MSG(lvTankPick(t, 2, 105.0f, 216.0f, 32.0f, 1.0f) == -1, "a dead tank alone");
    UT_ASSERT_MSG(lvTankPick(t, 2, 122.0f, 216.0f, 32.0f, 1.0f) == 1, "the live one under it");
    t[0].pickable = true;

    /* The mark: 2 points out from the picture, 1 thick, corners a third of
       the side. */
    m = lvTankMarkLayout(100.7f, 200.3f, 32.0f, 1.0f);
    UT_ASSERT_MSG(m.x == 97.0f && m.y == 197.0f, "the mark starts 3 px out (%g, %g)",
                  (double)m.x, (double)m.y);
    UT_ASSERT_MSG(m.side == 38.0f && m.thick == 1.0f && m.arm == 12.0f,
                  "the mark's size (%g, %g, %g)", (double)m.side, (double)m.thick, (double)m.arm);
    m = lvTankMarkLayout(100.0f, 200.0f, 32.0f, 2.0f);
    UT_ASSERT_MSG(m.x == 94.0f && m.thick == 2.0f && m.side == 44.0f,
                  "the mark at density 2 (%g, %g, %g)", (double)m.x, (double)m.thick,
                  (double)m.side);
    m = lvTankMarkLayout(10.0f, 10.0f, 12.0f, 1.0f);
    UT_ASSERT_MSG(m.arm == 6.0f && m.arm * 2.0f <= m.side, "the corners do not meet");
    return 0;
}
