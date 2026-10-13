/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums behind the options every panel of the log viewer has
 * (lv_panel_opts.h).
 *
 *   lv_panel_opts_sums — the name of a panel's option in the preferences,
 *       for every panel and for a window that is not one; where the gear
 *       goes in a title bar, with and without a close button, inside the
 *       border of a bar of tabs, and in a bar too narrow for it; where it
 *       goes in the corner of a panel with no bar.
 *
 *   lv_panel_backing_sums — which parts of a transparent panel get a
 *       backing: that one box is one block with the padding round it; that
 *       two boxes a small gap apart are one block and two a large gap apart
 *       are two; that a block is as wide as its widest row; that a box with
 *       no area or outside the panel adds nothing; that with more blocks
 *       than there is room for the last one takes the rest; and that a
 *       panel taller than the rows there are still gets its blocks.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lv_panel_opts.h"
#include "test_harness.h"

int run_lv_panel_opts_sums(void) {
    char        key[64];
    char        tiny[8];
    LvPanelGear g;
    int         i;

    UT_ASSERT_MSG(lvPanelOptIndex("###tank") == 3, "the Tank panel's index");
    UT_ASSERT_MSG(lvPanelOptIndex("###controls") == 0, "the first panel");
    UT_ASSERT_MSG(lvPanelOptIndex("###comments") == LV_PANEL_OPT_COUNT - 1, "the last panel");
    UT_ASSERT_MSG(lvPanelOptIndex("###lv_dock_host") == -1, "a window that is not a panel");
    UT_ASSERT_MSG(lvPanelOptIndex("Tank###tank") == -1, "the ID alone, not the title");
    UT_ASSERT_MSG(lvPanelOptIndex(NULL) == -1, "no name");

    UT_ASSERT_MSG(lvPanelOptKey(3, key, sizeof(key)) &&
                      strcmp(key, "Window.Tank.Transparent") == 0,
                  "the Tank panel's key (%s)", key);
    UT_ASSERT_MSG(lvPanelOptKey(lvPanelOptIndex("###gameinfo"), key, sizeof(key)) &&
                      strcmp(key, "Window.GameInformation.Transparent") == 0,
                  "the Game Information panel's key (%s)", key);
    UT_ASSERT_MSG(lvPanelOptKey(lvPanelOptIndex("###lvscnpanel"), key, sizeof(key)) &&
                      strcmp(key, "Window.ScenarioPanel.Transparent") == 0,
                  "the scenario panel's key (%s)", key);
    /* Every panel has a key, and no two the same. */
    for (i = 0; i < LV_PANEL_OPT_COUNT; i++) {
        char other[64];
        int  j;
        UT_ASSERT_MSG(lvPanelOptKey(i, key, sizeof(key)) && key[0] != '\0', "panel %d has a key", i);
        for (j = 0; j < i; j++) {
            lvPanelOptKey(j, other, sizeof(other));
            UT_ASSERT_MSG(strcmp(key, other) != 0, "panels %d and %d share a key", i, j);
        }
    }
    strcpy(key, "x");
    UT_ASSERT_MSG(!lvPanelOptKey(-1, key, sizeof(key)) && key[0] == '\0', "no key for no panel");
    UT_ASSERT_MSG(!lvPanelOptKey(LV_PANEL_OPT_COUNT, key, sizeof(key)), "no key past the end");
    UT_ASSERT_MSG(!lvPanelOptKey(3, tiny, sizeof(tiny)) && tiny[0] == '\0',
                  "a buffer too small gets no half key");
    UT_ASSERT_MSG(!lvPanelOptKey(3, NULL, 0), "no buffer");

    /* A title bar from 100 to 400, at y 50: font 13, padding 4 by 3, inner
       spacing 4. The close button is 383..396; the gear 366..379. */
    g = lvPanelGearInBar(100.0f, 50.0f, 400.0f, 13.0f, 4.0f, 3.0f, 4.0f, 0.0f, true);
    UT_ASSERT_MSG(g.x == 366.0f && g.y == 53.0f && g.side == 13.0f,
                  "beside the close button (%g, %g, %g)", (double)g.x, (double)g.y, (double)g.side);
    UT_ASSERT_MSG(g.x + g.side + 4.0f == 400.0f - 4.0f - 13.0f, "one inner spacing left of it");
    /* No close button: the gear is where it would be. */
    g = lvPanelGearInBar(100.0f, 50.0f, 400.0f, 13.0f, 4.0f, 3.0f, 4.0f, 0.0f, false);
    UT_ASSERT_MSG(g.x == 383.0f, "in the close button's place (%g)", (double)g.x);
    /* A bar of tabs is drawn inside the window's border. */
    g = lvPanelGearInBar(100.0f, 50.0f, 400.0f, 13.0f, 4.0f, 3.0f, 4.0f, 1.0f, true);
    UT_ASSERT_MSG(g.x == 365.0f, "inside the border (%g)", (double)g.x);
    /* A bar too narrow: the gear does not leave it at the left. */
    g = lvPanelGearInBar(100.0f, 50.0f, 120.0f, 13.0f, 4.0f, 3.0f, 4.0f, 0.0f, true);
    UT_ASSERT_MSG(g.x == 100.0f, "a narrow bar (%g)", (double)g.x);
    g = lvPanelGearInBar(100.0f, 50.0f, 120.0f, 13.0f, 4.0f, 3.0f, 4.0f, 1.0f, true);
    UT_ASSERT_MSG(g.x == 101.0f, "a narrow bar of tabs (%g)", (double)g.x);

    /* The corner: an inside of 100..300 by 60..260, a margin of 2. */
    g = lvPanelGearInCorner(100.0f, 60.0f, 300.0f, 260.0f, 13.0f, 2.0f);
    UT_ASSERT_MSG(g.x == 285.0f && g.y == 245.0f && g.side == 13.0f,
                  "the bottom right corner (%g, %g)", (double)g.x, (double)g.y);
    g = lvPanelGearInCorner(100.0f, 60.0f, 110.0f, 66.0f, 13.0f, 2.0f);
    UT_ASSERT_MSG(g.x == 100.0f && g.y == 60.0f, "a panel smaller than the gear");
    return 0;
}

/* Static: 8 KB of rows is a lot for a test's stack. */
static LvBacking s_b;

int run_lv_panel_backing_sums(void) {
    LvBackingRect r[4];
    int           n;

    /* A panel from y 100, 200 high: 100 rows of 2. */
    lvBackingReset(&s_b, 100.0f, 200.0f);
    UT_ASSERT_MSG(s_b.rows == 100 && s_b.rowH == 2.0f, "the rows (%d)", s_b.rows);
    UT_ASSERT_MSG(lvBackingBlocks(&s_b, 3.0f, r, 4) == 0, "nothing drawn, no block");

    /* One box: one block, with the padding round it. */
    lvBackingAdd(&s_b, 20.0f, 110.0f, 80.0f, 124.0f);
    n = lvBackingBlocks(&s_b, 3.0f, r, 4);
    UT_ASSERT_MSG(n == 1, "one box, one block (%d)", n);
    UT_ASSERT_MSG(r[0].x0 == 17.0f && r[0].x1 == 83.0f && r[0].y0 == 107.0f && r[0].y1 == 127.0f,
                  "the block (%g %g %g %g)", (double)r[0].x0, (double)r[0].y0, (double)r[0].x1,
                  (double)r[0].y1);

    /* A second box 4 under the first (a line of text under a line): the
       same block, as wide as the wider of the two. */
    lvBackingAdd(&s_b, 20.0f, 128.0f, 150.0f, 142.0f);
    n = lvBackingBlocks(&s_b, 3.0f, r, 4);
    UT_ASSERT_MSG(n == 1, "a small gap is one block (%d)", n);
    UT_ASSERT_MSG(r[0].x1 == 153.0f && r[0].y1 == 145.0f, "the widest row (%g, %g)",
                  (double)r[0].x1, (double)r[0].y1);

    /* A gap of exactly twice the padding still joins; one row more does
       not. */
    lvBackingAdd(&s_b, 30.0f, 148.0f, 40.0f, 150.0f);
    UT_ASSERT_MSG(lvBackingBlocks(&s_b, 3.0f, r, 4) == 1, "a gap of 6 joins");
    lvBackingAdd(&s_b, 200.0f, 158.0f, 260.0f, 170.0f);
    n = lvBackingBlocks(&s_b, 3.0f, r, 4);
    UT_ASSERT_MSG(n == 2, "a gap of 8 parts (%d)", n);
    UT_ASSERT_MSG(r[0].y1 == 153.0f && r[1].y0 == 155.0f, "the two do not overlap (%g, %g)",
                  (double)r[0].y1, (double)r[1].y0);
    UT_ASSERT_MSG(r[1].x0 == 197.0f && r[1].x1 == 263.0f, "the second is as wide as its own rows");

    /* Nothing from a box with no area, or outside the panel. */
    lvBackingAdd(&s_b, 0.0f, 200.0f, 0.0f, 220.0f);
    lvBackingAdd(&s_b, 0.0f, 200.0f, 50.0f, 200.0f);
    lvBackingAdd(&s_b, 0.0f, 20.0f, 50.0f, 100.0f);
    lvBackingAdd(&s_b, 0.0f, 300.0f, 50.0f, 400.0f);
    UT_ASSERT_MSG(lvBackingBlocks(&s_b, 3.0f, r, 4) == 2, "nothing from an empty box");

    /* A box that reaches over the panel's edges is cut to the panel. */
    lvBackingReset(&s_b, 100.0f, 200.0f);
    lvBackingAdd(&s_b, 10.0f, 50.0f, 20.0f, 500.0f);
    n = lvBackingBlocks(&s_b, 0.0f, r, 4);
    UT_ASSERT_MSG(n == 1 && r[0].y0 == 100.0f && r[0].y1 == 300.0f, "cut to the panel (%g, %g)",
                  (double)r[0].y0, (double)r[0].y1);

    /* A box that ends on a row's top edge does not use that row. */
    lvBackingReset(&s_b, 100.0f, 200.0f);
    lvBackingAdd(&s_b, 10.0f, 100.0f, 20.0f, 104.0f);
    n = lvBackingBlocks(&s_b, 0.0f, r, 4);
    UT_ASSERT_MSG(n == 1 && r[0].y1 == 104.0f, "ends on a row edge (%g)", (double)r[0].y1);

    /* Five blocks and room for two: the second takes all that is left. */
    lvBackingReset(&s_b, 0.0f, 400.0f);
    lvBackingAdd(&s_b, 0.0f, 10.0f, 10.0f, 20.0f);
    lvBackingAdd(&s_b, 50.0f, 60.0f, 60.0f, 70.0f);
    lvBackingAdd(&s_b, 100.0f, 110.0f, 110.0f, 120.0f);
    lvBackingAdd(&s_b, 150.0f, 160.0f, 160.0f, 170.0f);
    lvBackingAdd(&s_b, 200.0f, 210.0f, 210.0f, 220.0f);
    UT_ASSERT_MSG(lvBackingBlocks(&s_b, 3.0f, r, 4) == 4, "room for four of five");
    n = lvBackingBlocks(&s_b, 3.0f, r, 2);
    UT_ASSERT_MSG(n == 2, "room for two (%d)", n);
    UT_ASSERT_MSG(r[1].y0 == 57.0f && r[1].y1 == 223.0f && r[1].x0 == 47.0f && r[1].x1 == 213.0f,
                  "the last takes the rest (%g %g %g %g)", (double)r[1].x0, (double)r[1].y0,
                  (double)r[1].x1, (double)r[1].y1);
    UT_ASSERT_MSG(lvBackingBlocks(&s_b, 3.0f, r, 0) == 0, "no room at all");
    UT_ASSERT_MSG(lvBackingBlocks(&s_b, 3.0f, NULL, 4) == 0, "nowhere to write");

    /* A panel taller than 1024 rows of 2: taller rows, and the block is
       still where the box is, to within a row. */
    lvBackingReset(&s_b, 0.0f, 4096.0f);
    UT_ASSERT_MSG(s_b.rows == LV_BACKING_ROWS && s_b.rowH == 4.0f, "taller rows (%g)",
                  (double)s_b.rowH);
    lvBackingAdd(&s_b, 5.0f, 4001.0f, 9.0f, 4003.0f);
    n = lvBackingBlocks(&s_b, 0.0f, r, 4);
    UT_ASSERT_MSG(n == 1 && r[0].y0 == 4000.0f && r[0].y1 == 4004.0f, "a tall panel (%g, %g)",
                  (double)r[0].y0, (double)r[0].y1);

    /* No height: no rows, nothing to add to, no block. */
    lvBackingReset(&s_b, 0.0f, 0.0f);
    lvBackingAdd(&s_b, 0.0f, 0.0f, 10.0f, 10.0f);
    UT_ASSERT_MSG(lvBackingBlocks(&s_b, 3.0f, r, 4) == 0, "no height");
    lvBackingReset(&s_b, 0.0f, -5.0f);
    UT_ASSERT_MSG(s_b.rows == 0, "a negative height is none");
    return 0;
}
