/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The tablet UI's scenario panel square: a quarter of the screen's
 * shorter side, in the game view's top-right corner, inset by the
 * tablet's pad.
 *
 * The screens and views below are laid out the way the tablet frontend
 * centres its view: 15 tiles of 16 pixels at the largest whole zoom that
 * fits, with the rest split into gutters. Every expected number is worked
 * out by hand beside it.
 */
#include <stdbool.h>

#include "scenario_panel_slot.h"
#include "test_harness.h"

int run_scenario_panel_slot_rect(void) {
    float x, y, side;

    /* Landscape phone, 2340x1080: zoom 4 gives a 960 view at (690, 60).
       A quarter of 1080 is 270, which fits, so the square is at
       1650 - 20 - 270 = 1360 across and 60 + 20 = 80 down. */
    UT_ASSERT(scnPanelSlotRect(2340, 1080, 690.0f, 60.0f, 960.0f, 960.0f,
                               20.0f, &x, &y, &side));
    UT_ASSERT(side == 270.0f);
    UT_ASSERT(x == 1360.0f);
    UT_ASSERT(y == 80.0f);

    /* Landscape tablet, 2048x1536: zoom 6 gives a 1440 view at (304, 48).
       A quarter of 1536 is 384; 1744 - 9 - 384 = 1351 across, 48 + 9 = 57
       down. */
    UT_ASSERT(scnPanelSlotRect(2048, 1536, 304.0f, 48.0f, 1440.0f, 1440.0f,
                               9.0f, &x, &y, &side));
    UT_ASSERT(side == 384.0f);
    UT_ASSERT(x == 1351.0f);
    UT_ASSERT(y == 57.0f);

    /* A quarter of 1000 is 250, but a 200 view less 20 on each side leaves
       160, which is still above 128: the square shrinks to 160. */
    UT_ASSERT(scnPanelSlotRect(1600, 1000, 100.0f, 50.0f, 200.0f, 200.0f,
                               20.0f, &x, &y, &side));
    UT_ASSERT(side == 160.0f);
    UT_ASSERT(x == 120.0f);
    UT_ASSERT(y == 70.0f);

    /* A 150 view leaves 110 inside the pad, under 128: the square stops at
       128. The top and right keep their 20, which leaves 2 on the left and
       at the bottom: 300 - 20 - 128 = 152 across, 70 down, bottom at 198. */
    UT_ASSERT(scnPanelSlotRect(1600, 1000, 150.0f, 50.0f, 150.0f, 150.0f,
                               20.0f, &x, &y, &side));
    UT_ASSERT(side == 128.0f);
    UT_ASSERT(x == 152.0f);
    UT_ASSERT(y == 70.0f);

    /* A view smaller than 128 gets a square as big as the view, flush with
       it on every side. */
    UT_ASSERT(scnPanelSlotRect(1600, 1000, 150.0f, 50.0f, 100.0f, 100.0f,
                               20.0f, &x, &y, &side));
    UT_ASSERT(side == 100.0f);
    UT_ASSERT(x == 150.0f);
    UT_ASSERT(y == 50.0f);

    /* A quarter of the short side under 128 is kept as it is: the floor is
       for shrinking, not for growing. */
    UT_ASSERT(scnPanelSlotRect(480, 400, 120.0f, 80.0f, 240.0f, 240.0f,
                               4.0f, &x, &y, &side));
    UT_ASSERT(side == 100.0f);
    UT_ASSERT(x == 256.0f);
    UT_ASSERT(y == 84.0f);

    /* No view yet, before the first frame has laid one out: no square. */
    x = y = side = 1.0f;
    UT_ASSERT(!scnPanelSlotRect(2340, 1080, 0.0f, 0.0f, 0.0f, 0.0f, 20.0f,
                                &x, &y, &side));
    UT_ASSERT(x == 0.0f && y == 0.0f && side == 0.0f);

    return 0;
}
