/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums behind the log viewer's Tank panel (lv_tank_status.h).
 *
 *   lv_tank_status_sums — which tank the panel shows (the viewer's "self"
 *       player when that slot has a player in it, else none); and how tall
 *       a gauge is drawn: the game's 80 pixels for a tank that holds its
 *       cap, in proportion under that whatever the cap is, nothing for an
 *       empty tank or a cap of 0, and never taller than the gauge's well.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_tank_status.h"
#include "test_harness.h"

/* The game's numbers (gui/positions.h): a full gauge, and the well. */
#define FULL_PX 80
#define WELL_PX 85

int run_lv_tank_status_sums(void) {
    /* The selected tank. */
    UT_ASSERT_MSG(lvTankStatusSelected(0, 16, true) == 0, "slot 0 with a player");
    UT_ASSERT_MSG(lvTankStatusSelected(15, 16, true) == 15, "the last slot");
    UT_ASSERT_MSG(lvTankStatusSelected(3, 16, false) == -1, "slot with no player");
    UT_ASSERT_MSG(lvTankStatusSelected(16, 16, true) == -1, "past the last slot");
    UT_ASSERT_MSG(lvTankStatusSelected(255, 16, true) == -1, "the neutral owner");
    UT_ASSERT_MSG(lvTankStatusSelected(-1, 16, true) == -1, "below zero");

    /* The classic caps: 40 of each. */
    UT_ASSERT_MSG(lvTankGaugeHeight(40, 40, FULL_PX, WELL_PX) == 80.0f, "full is 80 pixels");
    UT_ASSERT_MSG(lvTankGaugeHeight(20, 40, FULL_PX, WELL_PX) == 40.0f, "half");
    UT_ASSERT_MSG(lvTankGaugeHeight(1, 40, FULL_PX, WELL_PX) == 2.0f, "one is two pixels");
    UT_ASSERT_MSG(lvTankGaugeHeight(0, 40, FULL_PX, WELL_PX) == 0.0f, "empty");

    /* A raised cap draws the same full gauge with a finer step. */
    UT_ASSERT_MSG(lvTankGaugeHeight(160, 160, FULL_PX, WELL_PX) == 80.0f, "full at a cap of 160");
    UT_ASSERT_MSG(lvTankGaugeHeight(1, 160, FULL_PX, WELL_PX) == 0.5f, "one of 160");

    /* A cap of 0 cannot be divided by. */
    UT_ASSERT_MSG(lvTankGaugeHeight(10, 0, FULL_PX, WELL_PX) == 0.0f, "cap of 0");
    UT_ASSERT_MSG(lvTankGaugeHeight(-1, 40, FULL_PX, WELL_PX) == 0.0f, "below zero");

    /* Over the cap: taller, but not out of the well. */
    UT_ASSERT_MSG(lvTankGaugeHeight(42, 40, FULL_PX, WELL_PX) == 84.0f, "a little over");
    UT_ASSERT_MSG(lvTankGaugeHeight(255, 40, FULL_PX, WELL_PX) == 85.0f, "cut at the well");

    return 0;
}
