/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * test_lv_base_near.c - the base the log viewer's Tank panel shows under the
 * selected tank's gauges (lv_base_near.h): picked as the game's
 * basesGetClosestForPlayer picks it, and its bars as long as the game draws
 * them.
 */

#include <stdbool.h>

#include "lv_base_near.h"
#include "test_harness.h"

/* The world place of the middle of map square n. */
#define BN_MID(n) ((long)(n) * 256 + 128)

/* The nearest neutral or allied base on the map, strictly nearer than the
   range; an enemy's, or one off the map, is passed over even when nearer. */
int run_lv_base_near_pick(void) {
    LvBaseNearItem items[4] = {
        { true, true, 100, 100 },   /* 0: friendly, 3 squares away */
        { true, false, 98, 100 },   /* 1: an enemy's, 1 square away */
        { false, true, 97, 100 },   /* 2: not on the map, at the tank */
        { true, true, 101, 100 },   /* 3: friendly, 4 squares away */
    };
    const long tx = BN_MID(97), ty = BN_MID(100);

    UT_ASSERT(lvBaseNearPick(items, 4, tx, ty, 1792) == 0);
    /* With the near one gone, the next friendly one. */
    items[0].friendly = false;
    UT_ASSERT(lvBaseNearPick(items, 4, tx, ty, 1792) == 3);
    /* None friendly and on the map: none. */
    items[3].active = false;
    UT_ASSERT(lvBaseNearPick(items, 4, tx, ty, 1792) == LV_BASE_NEAR_NONE);
    UT_ASSERT(lvBaseNearPick(items, 0, tx, ty, 1792) == LV_BASE_NEAR_NONE);
    return 0;
}

/* The range is strict, as the game's is: a base at exactly the range is not
   shown, one a world unit nearer is. The distance is a straight line, and of
   two at the same distance the lower number wins. A range of 0 shows none. */
int run_lv_base_near_range(void) {
    LvBaseNearItem one[1] = { { true, true, 100, 100 } };
    LvBaseNearItem two[2] = { { true, true, 103, 100 }, { true, true, 97, 100 } };
    const long bx = BN_MID(100), by = BN_MID(100);

    /* 1792 = 7 squares, the classic BASE_STATUS_RANGE. */
    UT_ASSERT(lvBaseNearPick(one, 1, bx + 1792, by, 1792) == LV_BASE_NEAR_NONE);
    UT_ASSERT(lvBaseNearPick(one, 1, bx + 1791, by, 1792) == 0);
    UT_ASSERT(lvBaseNearPick(one, 1, bx, by - 1791, 1792) == 0);
    /* 1200 and 1300 across and down: 1769 in a straight line, in range,
       though each is under the range alone and their sum is not. */
    UT_ASSERT(lvBaseNearPick(one, 1, bx + 1200, by + 1300, 1792) == 0);
    /* 1300 and 1300: 1838, out of range. */
    UT_ASSERT(lvBaseNearPick(one, 1, bx + 1300, by + 1300, 1792) == LV_BASE_NEAR_NONE);
    UT_ASSERT(lvBaseNearPick(one, 1, bx, by, 0) == LV_BASE_NEAR_NONE);
    /* Three squares each way: the first wins. */
    UT_ASSERT(lvBaseNearPick(two, 2, bx, by, 1792) == 0);
    /* A 16th of a square nearer the second: the second. */
    UT_ASSERT(lvBaseNearPick(two, 2, bx - 16, by, 1792) == 1);
    return 0;
}

/* The bars: 60 pixels at the cap and in proportion under it; empty at 0, for
   a cap of 0, and never past the end of the well. */
int run_lv_base_near_bars(void) {
    UT_ASSERT(lvBaseBarWidth(90, 90, 60, 60) == 60.0f);
    UT_ASSERT(lvBaseBarWidth(45, 90, 60, 60) == 30.0f);
    UT_ASSERT(lvBaseBarWidth(0, 90, 60, 60) == 0.0f);
    UT_ASSERT(lvBaseBarWidth(10, 0, 60, 60) == 0.0f);
    UT_ASSERT(lvBaseBarWidth(200, 90, 60, 60) == 60.0f);
    UT_ASSERT(lvBaseBarWidth(20, 40, 60, 60) == 30.0f);
    return 0;
}
