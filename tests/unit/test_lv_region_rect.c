/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Where the log viewer draws a declared region (lv_region_rect.h).
 *
 *   lv_region_rect_placement — on the overview's 1x target, a region in the
 *       last column and one hanging off the left edge are drawn and one that
 *       only touches the left edge is not; a region past the view and an
 *       empty one are not drawn; and on the game view at 2x, with a
 *       sub-square pan, a region lands one square in from the viewport's
 *       offset, moved back by the pan, at twice the size.
 *
 * Every number here is a whole number of pixels, so the floats compare
 * exactly.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_region_rect.h"
#include "test_harness.h"

/* One rectangle against the one it should be. */
static bool lvrrSame(const LvRegionRect *got, float x, float y, float w,
                     float h) {
    return got->x == x && got->y == y && got->w == w && got->h == h;
}

int run_lv_region_rect_placement(void) {
    LvRegionView overview;
    LvRegionView gameView;
    LvRegionRect rect;

    /* The overview: viewport offset (100, 120), 20 by 15 squares shown and
       the render target one square wider and taller, 16 pixels a square. */
    overview.mapX  = -100.0f * 16.0f;
    overview.mapY  = -120.0f * 16.0f;
    overview.tileW = 16.0f;
    overview.tileH = 16.0f;
    overview.viewX = 0.0f;
    overview.viewY = 0.0f;
    overview.viewW = 21.0f * 16.0f;
    overview.viewH = 16.0f * 16.0f;

    /* In the last column, running off the right. */
    UT_ASSERT_MSG(lvRegionScreenRect(&overview, 120, 120, 4, 8, &rect),
                  "a region in the last column was not drawn");
    UT_ASSERT_MSG(lvrrSame(&rect, 320.0f, 0.0f, 64.0f, 128.0f),
                  "last column: got (%g, %g, %g, %g)",
                  rect.x, rect.y, rect.w, rect.h);

    /* Half off the left edge. */
    UT_ASSERT_MSG(lvRegionScreenRect(&overview, 98, 121, 4, 2, &rect),
                  "a region across the left edge was not drawn");
    UT_ASSERT_MSG(lvrrSame(&rect, -32.0f, 16.0f, 64.0f, 32.0f),
                  "left edge: got (%g, %g, %g, %g)",
                  rect.x, rect.y, rect.w, rect.h);

    /* Ending exactly where the view starts. */
    UT_ASSERT_MSG(!lvRegionScreenRect(&overview, 96, 121, 4, 2, &rect),
                  "a region that only touches the left edge was drawn");

    /* Wholly off-screen, to the right and above. */
    UT_ASSERT_MSG(!lvRegionScreenRect(&overview, 151, 124, 4, 8, &rect),
                  "a region right of the view was drawn");
    UT_ASSERT_MSG(!lvRegionScreenRect(&overview, 101, 100, 4, 8, &rect),
                  "a region above the view was drawn");

    /* No size. */
    UT_ASSERT_MSG(!lvRegionScreenRect(&overview, 105, 125, 0, 8, &rect),
                  "an empty region was drawn");

    /* The game view at 2x: the view starts at (20, 40) and shows 15 by 15
       squares of 32 pixels from square xOffset+1, yOffset+1, panned 6 and 10
       pixels on. */
    gameView.mapX  = 20.0f - 6.0f - (100.0f + 1.0f) * 32.0f;
    gameView.mapY  = 40.0f - 10.0f - (120.0f + 1.0f) * 32.0f;
    gameView.tileW = 32.0f;
    gameView.tileH = 32.0f;
    gameView.viewX = 20.0f;
    gameView.viewY = 40.0f;
    gameView.viewW = 15.0f * 32.0f;
    gameView.viewH = 15.0f * 32.0f;

    UT_ASSERT_MSG(lvRegionScreenRect(&gameView, 101, 124, 4, 8, &rect),
                  "a region in the 2x game view was not drawn");
    UT_ASSERT_MSG(lvrrSame(&rect, 14.0f, 126.0f, 128.0f, 256.0f),
                  "2x: got (%g, %g, %g, %g)",
                  rect.x, rect.y, rect.w, rect.h);

    /* The same region seen from further along the map is off the view. */
    gameView.mapX -= 20.0f * 32.0f;
    UT_ASSERT_MSG(!lvRegionScreenRect(&gameView, 101, 124, 4, 8, &rect),
                  "a region left of the 2x game view was drawn");
    return 0;
}
