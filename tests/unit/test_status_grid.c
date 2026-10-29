/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Where slot n of a status panel sits (src/gui/status_grid.h), the one sum
 * the game's renderer and the web Log Viewer both place their icons with.
 *
 *   status_grid_slots — each of the sixteen slots of the bases, pillboxes
 *       and tanks panels is where the explicit STATUS_*_n_X/Y macros of
 *       positions.h put it: six across the first row, two and two with a gap
 *       in the middle of the second, six across the third; the three panels
 *       share the grid; and a caller that wants only one axis may pass NULL
 *       for the other.
 */

#include <stddef.h>

#include "gui/positions.h"
#include "gui/tiles.h"
#include "gui/status_grid.h"
#include "test_harness.h"

typedef struct {
    int x, y;
} GridPoint;

#define SLOT(panel, n) { STATUS_##panel##_##n##_X, STATUS_##panel##_##n##_Y }
#define PANEL_SLOTS(panel)                                                  \
    {                                                                       \
        SLOT(panel, 1),  SLOT(panel, 2),  SLOT(panel, 3),  SLOT(panel, 4),  \
        SLOT(panel, 5),  SLOT(panel, 6),  SLOT(panel, 7),  SLOT(panel, 8),  \
        SLOT(panel, 9),  SLOT(panel, 10), SLOT(panel, 11), SLOT(panel, 12), \
        SLOT(panel, 13), SLOT(panel, 14), SLOT(panel, 15), SLOT(panel, 16), \
    }

int run_status_grid_slots(void) {
    static const GridPoint bases[16] = PANEL_SLOTS(BASE);
    static const GridPoint pills[16] = PANEL_SLOTS(PILLBOX);
    static const GridPoint tanks[16] = PANEL_SLOTS(TANKS);
    int n;

    for (n = 1; n <= 16; n++) {
        int x = -1, y = -1;
        statusGridItemPos(n, &x, &y);
        UT_ASSERT_MSG(x == bases[n - 1].x && y == bases[n - 1].y,
                      "base slot %d: grid %d,%d, positions.h %d,%d",
                      n, x, y, bases[n - 1].x, bases[n - 1].y);
        UT_ASSERT_MSG(x == pills[n - 1].x && y == pills[n - 1].y,
                      "pillbox slot %d: grid %d,%d, positions.h %d,%d",
                      n, x, y, pills[n - 1].x, pills[n - 1].y);
        UT_ASSERT_MSG(x == tanks[n - 1].x && y == tanks[n - 1].y,
                      "tank slot %d: grid %d,%d, positions.h %d,%d",
                      n, x, y, tanks[n - 1].x, tanks[n - 1].y);
    }

    /* The shape of the grid, in slots rather than pixels: the first row is
       six slots and the second starts under the first; the second row's
       third slot sits under the fifth of the first, leaving the gap for the
       centre icon; the third row starts under the first again. */
    {
        int x1, y1, x6, y6, x7, y7, x9, y9, x11, y11;
        statusGridItemPos(1, &x1, &y1);
        statusGridItemPos(6, &x6, &y6);
        statusGridItemPos(7, &x7, &y7);
        statusGridItemPos(9, &x9, &y9);
        statusGridItemPos(11, &x11, &y11);
        UT_ASSERT_MSG(y6 == y1 && x6 > x1, "slots 1 to 6 are one row");
        UT_ASSERT_MSG(x7 == x1 && y7 > y1, "slot 7 starts the second row");
        UT_ASSERT_MSG(y9 == y7 && x9 == STATUS_BASE_5_X,
                      "slot 9 is under slot 5: %d, %d", x9, STATUS_BASE_5_X);
        UT_ASSERT_MSG(x11 == x1 && y11 > y7, "slot 11 starts the third row");
    }

    /* One axis at a time. */
    {
        int x = -1, y = -1;
        statusGridItemPos(16, &x, NULL);
        UT_ASSERT_MSG(x == STATUS_BASE_16_X, "x alone: %d", x);
        statusGridItemPos(16, NULL, &y);
        UT_ASSERT_MSG(y == STATUS_BASE_16_Y, "y alone: %d", y);
    }
    return 0;
}
