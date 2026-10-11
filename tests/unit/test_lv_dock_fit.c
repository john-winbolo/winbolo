/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * How the log viewer's window is shared between the map and the right-side
 * dock (lv_dock_fit.h).
 *
 *   lv_dock_fit_sums — with no dock the squares across are the nearest whole
 *       number, as the window sizing has always worked them out; beside the
 *       dock a part square counts as a whole one, so the map reaches the
 *       dock; the count stays inside 1..255; and the dock's width is held
 *       between its smallest and two thirds of the window.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_dock_fit.h"
#include "test_harness.h"

int run_lv_dock_fit_sums(void) {
    /* Nearest: 16 pixels a square, so 807 is 50 and 808 (half way) is 51. */
    UT_ASSERT_MSG(lvFitTilesNearest(800, 16.0f) == 50, "800/16 nearest: %d",
                  lvFitTilesNearest(800, 16.0f));
    UT_ASSERT_MSG(lvFitTilesNearest(807, 16.0f) == 50, "807/16 nearest: %d",
                  lvFitTilesNearest(807, 16.0f));
    UT_ASSERT_MSG(lvFitTilesNearest(808, 16.0f) == 51, "808/16 nearest: %d",
                  lvFitTilesNearest(808, 16.0f));

    /* Cover: an exact fit is not rounded up, one pixel over is. */
    UT_ASSERT_MSG(lvFitTilesCover(800, 16.0f) == 50, "800/16 cover: %d",
                  lvFitTilesCover(800, 16.0f));
    UT_ASSERT_MSG(lvFitTilesCover(801, 16.0f) == 51, "801/16 cover: %d",
                  lvFitTilesCover(801, 16.0f));
    /* A zoomed square that is not a whole number of pixels. */
    UT_ASSERT_MSG(lvFitTilesCover(100, 12.0f) == 9, "100/12 cover: %d",
                  lvFitTilesCover(100, 12.0f));

    /* The limits: never no squares, never more than the map has, and a
       square size of nothing does not divide by zero. */
    UT_ASSERT_MSG(lvFitTilesNearest(0, 16.0f) == 1, "empty area, nearest");
    UT_ASSERT_MSG(lvFitTilesCover(-40, 16.0f) == 1, "negative area, cover");
    UT_ASSERT_MSG(lvFitTilesCover(100000, 16.0f) == 255, "huge area, cover");
    UT_ASSERT_MSG(lvFitTilesNearest(100, 0.0f) == 100, "zero square size: %d",
                  lvFitTilesNearest(100, 0.0f));

    /* Beside the dock: nothing reserved is the nearest sum over the whole
       window; with 420 reserved of 1280 the map has 860, which 16 covers in
       54 squares (53.75 rounded up). */
    UT_ASSERT_MSG(lvFitTilesBesideDock(807, 0, 16.0f) == 50,
                  "no dock: %d", lvFitTilesBesideDock(807, 0, 16.0f));
    UT_ASSERT_MSG(lvFitTilesBesideDock(1280, 420, 16.0f) == 54,
                  "dock 420 of 1280: %d",
                  lvFitTilesBesideDock(1280, 420, 16.0f));
    /* A dock as wide as the window still leaves the one square. */
    UT_ASSERT_MSG(lvFitTilesBesideDock(400, 400, 16.0f) == 1,
                  "dock over the whole window: %d",
                  lvFitTilesBesideDock(400, 400, 16.0f));

    /* The dock's width: as asked inside the limits, raised to the smallest,
       cut to two thirds of the window. */
    UT_ASSERT_MSG(lvDockClampWidth(420, 1280) == 420, "420 of 1280: %d",
                  lvDockClampWidth(420, 1280));
    UT_ASSERT_MSG(lvDockClampWidth(10, 1280) == LV_DOCK_WIDTH_MIN,
                  "10 of 1280: %d", lvDockClampWidth(10, 1280));
    UT_ASSERT_MSG(lvDockClampWidth(2000, 1200) == 800, "2000 of 1200: %d",
                  lvDockClampWidth(2000, 1200));
    /* A window whose two thirds is under the smallest width: the smallest
       wins while it fits, and the dock never passes the window's own width. */
    UT_ASSERT_MSG(lvDockClampWidth(420, 300) == LV_DOCK_WIDTH_MIN,
                  "420 of 300: %d", lvDockClampWidth(420, 300));
    UT_ASSERT_MSG(lvDockClampWidth(420, 150) == 150, "420 of 150: %d",
                  lvDockClampWidth(420, 150));
    UT_ASSERT_MSG(lvDockClampWidth(420, 0) == 0, "no window: %d",
                  lvDockClampWidth(420, 0));
    return 0;
}
