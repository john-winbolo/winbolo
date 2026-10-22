/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums behind the bars the log viewer draws over each tank
 * (lv_tank_bars.h).
 *
 *   lv_tank_bars_sums — that a bar is as wide as one map square and an
 *       eighth of one high, never under 2 pixels nor over 5; that the
 *       shells bar sits one pixel above the tank and the armour bar one
 *       pixel above that, on whole pixels, wherever the tank is; that no
 *       bars are drawn when a square is under 12 pixels; how much of a bar
 *       is filled (nothing for a cap of 0, at least a pixel for a tank that
 *       holds anything, the whole bar only for a full one); and that armour
 *       is green over half, yellow down to a quarter and red under that.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_tank_bars.h"
#include "test_harness.h"

int run_lv_tank_bars_sums(void) {
    LvTankBarsLayout l;
    unsigned char r, g, b;

    /* The height of a bar at each size of square the viewer's zoom gives. */
    UT_ASSERT_MSG(lvTankBarThickness(8.0f, 1.0f) == 2.0f, "8 px square: the least");
    UT_ASSERT_MSG(lvTankBarThickness(12.8f, 1.0f) == 2.0f, "12.8 px square");
    UT_ASSERT_MSG(lvTankBarThickness(16.0f, 1.0f) == 2.0f, "16 px square: an eighth");
    UT_ASSERT_MSG(lvTankBarThickness(32.0f, 1.0f) == 4.0f, "32 px square: an eighth");
    UT_ASSERT_MSG(lvTankBarThickness(48.0f, 1.0f) == 5.0f, "48 px square: the most");
    UT_ASSERT_MSG(lvTankBarThickness(64.0f, 1.0f) == 5.0f, "64 px square: the most");

    /* A tank at a whole place, squares of 32. */
    l = lvTankBarsLayout(100.0f, 200.0f, 32.0f, 1.0f);
    UT_ASSERT_MSG(l.visible, "32 px squares have bars");
    UT_ASSERT_MSG(l.shells.x == 100.0f && l.armour.x == 100.0f, "start at the tank's left");
    UT_ASSERT_MSG(l.shells.w == 32.0f && l.armour.w == 32.0f, "one square wide");
    UT_ASSERT_MSG(l.shells.h == 4.0f && l.armour.h == 4.0f, "an eighth high");
    UT_ASSERT_MSG(l.shells.y + l.shells.h == 199.0f, "shells one pixel above the tank");
    UT_ASSERT_MSG(l.armour.y + l.armour.h == l.shells.y - 1.0f, "armour one pixel above shells");
    UT_ASSERT_MSG(l.armour.y < l.shells.y, "armour on top");

    /* A tank part way through a pixel: the bars are on whole pixels, and
       move with the tank. */
    l = lvTankBarsLayout(100.7f, 200.3f, 16.0f, 1.0f);
    UT_ASSERT_MSG(l.shells.x == 100.0f && l.shells.y == 197.0f, "floored");
    UT_ASSERT_MSG(l.armour.y == 194.0f && l.armour.h == 2.0f && l.armour.w == 16.0f,
                  "16 px squares");
    l = lvTankBarsLayout(101.2f, 200.3f, 16.0f, 1.0f);
    UT_ASSERT_MSG(l.shells.x == 101.0f && l.armour.x == 101.0f, "follows the tank");

    /* A square that is not a whole number of pixels: the nearest. */
    l = lvTankBarsLayout(0.0f, 50.0f, 12.8f, 1.0f);
    UT_ASSERT_MSG(l.visible && l.shells.w == 13.0f, "12.8 px squares");
    l = lvTankBarsLayout(0.0f, 50.0f, 14.4f, 1.0f);
    UT_ASSERT_MSG(l.visible && l.shells.w == 14.0f, "14.4 px squares");

    /* The cutoff. */
    UT_ASSERT_MSG(lvTankBarsLayout(0.0f, 50.0f, 12.0f, 1.0f).visible, "12 px squares have bars");
    UT_ASSERT_MSG(!lvTankBarsLayout(0.0f, 50.0f, 11.2f, 1.0f).visible, "11.2 px squares have none");
    UT_ASSERT_MSG(!lvTankBarsLayout(0.0f, 50.0f, 8.0f, 1.0f).visible, "8 px squares have none");

    /* A pixel density of 2: the sizes are in points, so the bars and the
       gaps are twice the pixels, the width is still exactly one square, and
       the cutoff is at the same zoom step (12 points, 24 pixels). */
    UT_ASSERT_MSG(lvTankBarThickness(32.0f, 2.0f) == 4.0f, "16 pt square at 2x: 2 pt, 4 px");
    UT_ASSERT_MSG(lvTankBarThickness(64.0f, 2.0f) == 8.0f, "32 pt square at 2x: 4 pt, 8 px");
    UT_ASSERT_MSG(lvTankBarThickness(128.0f, 2.0f) == 10.0f, "64 pt square at 2x: the most, 10 px");
    l = lvTankBarsLayout(100.0f, 200.0f, 64.0f, 2.0f);
    UT_ASSERT_MSG(l.visible, "32 pt squares at 2x have bars");
    UT_ASSERT_MSG(l.shells.w == 64.0f && l.armour.w == 64.0f, "one square wide at 2x");
    UT_ASSERT_MSG(l.shells.h == 8.0f && l.armour.h == 8.0f, "twice the pixels high");
    UT_ASSERT_MSG(l.shells.y + l.shells.h == 198.0f, "shells one point above the tank");
    UT_ASSERT_MSG(l.armour.y + l.armour.h == l.shells.y - 2.0f, "armour one point above shells");
    UT_ASSERT_MSG(lvTankBarsLayout(0.0f, 50.0f, 24.0f, 2.0f).visible, "12 pt squares at 2x have bars");
    UT_ASSERT_MSG(!lvTankBarsLayout(0.0f, 50.0f, 22.4f, 2.0f).visible, "11.2 pt squares at 2x have none");
    UT_ASSERT_MSG(!lvTankBarsLayout(0.0f, 50.0f, 16.0f, 2.0f).visible,
                  "8 pt squares at 2x have none, though they are 16 px");
    /* A density that is not a whole number: whole pixels, never none. */
    UT_ASSERT_MSG(lvTankBarThickness(24.0f, 1.5f) == 3.0f, "16 pt square at 1.5x: 2 pt, 3 px");
    l = lvTankBarsLayout(10.0f, 100.0f, 24.0f, 1.5f);
    UT_ASSERT_MSG(l.shells.y + l.shells.h == 98.0f, "a 1 pt gap at 1.5x is 2 px");
    /* A density of 0 is treated as 1. */
    UT_ASSERT_MSG(lvTankBarThickness(16.0f, 0.0f) == 2.0f, "density 0 reads as 1");

    /* How full. */
    UT_ASSERT_MSG(lvTankBarFraction(40, 40) == 1.0f, "full");
    UT_ASSERT_MSG(lvTankBarFraction(10, 40) == 0.25f, "a quarter");
    UT_ASSERT_MSG(lvTankBarFraction(0, 40) == 0.0f, "empty");
    UT_ASSERT_MSG(lvTankBarFraction(50, 40) == 1.0f, "over the cap is full");
    UT_ASSERT_MSG(lvTankBarFraction(10, 0) == 0.0f, "cap of 0");
    UT_ASSERT_MSG(lvTankBarFraction(-3, 40) == 0.0f, "below zero");

    UT_ASSERT_MSG(lvTankBarFillWidth(32.0f, 40, 40) == 32.0f, "full bar");
    UT_ASSERT_MSG(lvTankBarFillWidth(32.0f, 20, 40) == 16.0f, "half bar");
    UT_ASSERT_MSG(lvTankBarFillWidth(32.0f, 0, 40) == 0.0f, "empty bar");
    UT_ASSERT_MSG(lvTankBarFillWidth(16.0f, 1, 255) == 1.0f, "anything shows a pixel");
    UT_ASSERT_MSG(lvTankBarFillWidth(16.0f, 254, 255) == 15.0f, "only full fills the bar");
    UT_ASSERT_MSG(lvTankBarFillWidth(16.0f, 9, 0) == 0.0f, "cap of 0 is empty");
    UT_ASSERT_MSG(lvTankBarFillWidth(0.0f, 9, 40) == 0.0f, "no bar");

    /* Armour: green over half, yellow down to a quarter, red at and under. */
    lvTankBarArmourColour(1.0f, &r, &g, &b);
    UT_ASSERT_MSG(g > r && g > b, "full is green");
    lvTankBarArmourColour(0.51f, &r, &g, &b);
    UT_ASSERT_MSG(g > r && g > b, "just over half is green");
    lvTankBarArmourColour(0.5f, &r, &g, &b);
    UT_ASSERT_MSG(r > 200 && g > 200 && b < 100, "half is yellow");
    lvTankBarArmourColour(0.26f, &r, &g, &b);
    UT_ASSERT_MSG(r > 200 && g > 200 && b < 100, "just over a quarter is yellow");
    lvTankBarArmourColour(0.25f, &r, &g, &b);
    UT_ASSERT_MSG(r > 200 && g < 100 && b < 100, "a quarter is red");
    lvTankBarArmourColour(0.0f, &r, &g, &b);
    UT_ASSERT_MSG(r > 200 && g < 100 && b < 100, "empty is red");

    /* The shells colour is none of the three. */
    UT_ASSERT_MSG(LV_TANK_BARS_SHELLS_B > LV_TANK_BARS_SHELLS_R &&
                  LV_TANK_BARS_SHELLS_B > LV_TANK_BARS_SHELLS_G, "shells are blue");

    return 0;
}
