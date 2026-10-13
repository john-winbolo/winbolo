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
 *       Then the small pictures a small panel shows in place of the words:
 *       when it shows them, where they are in the game's picture, how wide
 *       one is drawn, and where the colon and the numbers of a line go.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lv_tank_status.h"
#include "test_harness.h"

/* The game's numbers, from the header the panel itself reads: a full gauge,
   and the well. The sums below state what those numbers are (80 and 85), so
   a change to the header shows here. */
#include "../../src/gui/positions.h"
#define FULL_PX BAR_TANK_FULL_PIXELS
#define WELL_PX STATUS_TANK_BARS_HEIGHT

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

    /* What the panel shows in the room it has. The well is 55 by 107 with 2
       for its edge: 57 by 109 at scale 1, 112 by 216 at scale 2. The block
       with words is 120 by 100, the block with pictures 80 by 100; 16
       between the well and a block beside it, 4 over a block under it. */
    {
        LvTankRoom base;
        LvTankRoom r;
        LvTankLayout l;
        base.availW = 0.0f; base.availH = 0.0f;
        base.wellW = 55; base.wellH = 107; base.wellEdge = 2.0f;
        base.maxScale = 4; base.wordsMinScale = 2;
        base.gapBeside = 16.0f; base.gapUnder = 4.0f;
        base.wordsW = 120.0f; base.wordsH = 100.0f;
        base.iconsW = 80.0f;  base.iconsH = 100.0f;

#define PICK(w, h) (r = base, r.availW = (w), r.availH = (h), l = lvTankLayoutPick(&r))
#define IS(t, b, s) (l.text == (t) && l.beside == (b) && l.scale == (s))
#define SAYS l.text, (int)l.beside, (double)l.scale

        /* Words beside the well at scale 2: 112 + 16 + 120 by 216. */
        PICK(248.0f, 216.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_WORDS, true, 2), "words beside, exact: text %d beside %d scale %g", SAYS);
        PICK(247.0f, 216.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_ICONS, true, 2), "one short across: text %d beside %d scale %g", SAYS);
        PICK(248.0f, 215.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_ICONS, true, 1), "one short down: text %d beside %d scale %g", SAYS);

        /* Words under the well at scale 2: 120 by 216 + 4 + 100. */
        PICK(120.0f, 320.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_WORDS, false, 2), "words under, exact: text %d beside %d scale %g", SAYS);
        PICK(119.0f, 320.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_ICONS, false, 2), "one short across: text %d beside %d scale %g", SAYS);
        PICK(120.0f, 319.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_ICONS, false, 1), "one short down: text %d beside %d scale %g", SAYS);

        /* Both places have the well at the same scale: beside. */
        PICK(248.0f, 320.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_WORDS, true, 2), "both fit: text %d beside %d scale %g", SAYS);
        /* Under leaves the well larger: under. */
        PICK(180.0f, 430.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_WORDS, false, 3), "under has the larger well: text %d beside %d scale %g", SAYS);
        /* Room to spare: the well stops at its largest. */
        PICK(2000.0f, 2000.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_WORDS, true, 4), "a large panel: text %d beside %d scale %g", SAYS);

        /* Pictures beside the well at scale 1: 57 + 16 + 80 by 109. */
        PICK(153.0f, 109.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_ICONS, true, 1), "pictures beside, exact: text %d beside %d scale %g", SAYS);
        PICK(152.0f, 109.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_NONE, false, 1), "one short across: text %d beside %d scale %g", SAYS);
        PICK(153.0f, 108.0f);
        UT_ASSERT_MSG(l.text == LV_TANK_TEXT_NONE && l.scale < 1.0f && l.scale > 0.99f, "one short down (the well too): text %d beside %d scale %g", SAYS);

        /* Pictures under the well at scale 1: 80 by 109 + 4 + 100. */
        PICK(80.0f, 213.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_ICONS, false, 1), "pictures under, exact: text %d beside %d scale %g", SAYS);
        PICK(79.0f, 213.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_NONE, false, 1), "one short across: text %d beside %d scale %g", SAYS);
        PICK(80.0f, 212.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_NONE, false, 1), "one short down: text %d beside %d scale %g", SAYS);

        /* A block taller than the well beside it needs its own height. */
        r = base; r.iconsH = 150.0f; r.availW = 153.0f; r.availH = 150.0f;
        l = lvTankLayoutPick(&r);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_ICONS, true, 1), "a tall block, exact: text %d beside %d scale %g", SAYS);
        r.availH = 149.0f;
        l = lvTankLayoutPick(&r);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_NONE, false, 1), "a tall block, one short: text %d beside %d scale %g", SAYS);

        /* No text: the well takes the room. */
        PICK(700.0f, 216.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_WORDS, true, 2), "wide and short, words fit: text %d beside %d scale %g", SAYS);
        PICK(130.0f, 220.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_ICONS, false, 1), "narrow: pictures under: text %d beside %d scale %g", SAYS);
        PICK(79.0f, 500.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_NONE, false, 1), "too narrow for any text: text %d beside %d scale %g", SAYS);
        r = base; r.iconsW = 0.0f; r.availW = 112.0f; r.availH = 216.0f;
        l = lvTankLayoutPick(&r);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_NONE, false, 2), "the well alone at scale 2: text %d beside %d scale %g", SAYS);
        /* A room smaller than the well at scale 1 (57 by 109): the well is
           shrunk to go into it, never under a quarter. */
        PICK(57.0f, 109.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_NONE, false, 1), "the well alone, exact: text %d beside %d scale %g", SAYS);
        PICK(57.0f, 108.0f);
        UT_ASSERT_MSG(l.text == LV_TANK_TEXT_NONE && l.scale < 1.0f &&
                      l.scale * 107.0f + 2.0f <= 108.0f && l.scale * 107.0f + 2.0f > 107.9f,
                      "one short down: the well fills the height: text %d beside %d scale %g", SAYS);
        PICK(56.0f, 109.0f);
        UT_ASSERT_MSG(l.text == LV_TANK_TEXT_NONE && l.scale < 1.0f &&
                      l.scale * 55.0f + 2.0f <= 56.0f && l.scale * 55.0f + 2.0f > 55.9f,
                      "one short across: the well fills the width: text %d beside %d scale %g", SAYS);
        PICK(700.0f, 98.0f);
        UT_ASSERT_MSG(l.text == LV_TANK_TEXT_NONE && l.scale < 1.0f && l.scale > 0.89f &&
                      l.scale * 107.0f + 2.0f <= 98.0f,
                      "wide and too short: text %d beside %d scale %g", SAYS);
        PICK(30.0f, 30.0f);
        UT_ASSERT_MSG(l.text == LV_TANK_TEXT_NONE && l.scale > 0.26f && l.scale < 0.27f &&
                      l.scale * 107.0f + 2.0f <= 30.0f && l.scale * 55.0f + 2.0f <= 30.0f,
                      "smaller than the well: text %d beside %d scale %g", SAYS);
        PICK(0.0f, 0.0f);
        UT_ASSERT_MSG(l.text == LV_TANK_TEXT_NONE && l.scale == LV_TANK_WELL_MIN_SCALE,
                      "no room: text %d beside %d scale %g", SAYS);

        /* No pictures to show: the words stay, down to the smallest well. */
        r = base; r.iconsW = 0.0f; r.availW = 193.0f; r.availH = 109.0f;
        l = lvTankLayoutPick(&r);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_WORDS, true, 1), "no pictures, words exact: text %d beside %d scale %g", SAYS);
        r.availW = 192.0f;
        l = lvTankLayoutPick(&r);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_NONE, false, 1), "no pictures, one short: text %d beside %d scale %g", SAYS);

        /* The same room gives the same answer every time. */
        PICK(153.0f, 109.0f);
        PICK(153.0f, 109.0f);
        UT_ASSERT_MSG(IS(LV_TANK_TEXT_ICONS, true, 1), "asked twice: text %d beside %d scale %g", SAYS);

#undef PICK
#undef IS
#undef SAYS
    }

    /* Where the pictures are in the game's background picture (515 by 325):
       7 by 8 at 444,246 / 456,246 / 468,246 / 480,246, so the cells with
       their one pixel of black start one left and one up. */
    {
        const int gaugeX[LV_TANK_GAUGE_COUNT] = {
            0, STATUS_TANK_MINES, STATUS_TANK_ARMOUR, STATUS_TANK_TREES
        };
        const int wantX[LV_TANK_GAUGE_COUNT] = { 443, 455, 467, 479 };
        int cellY = STATUS_TANK_BARS_TOP + STATUS_TANK_BARS_HEIGHT + LV_TANK_ICON_CELL_DY;
        int g;
        UT_ASSERT_MSG(cellY == 245, "the cells start at row %d (want 245)", cellY);
        UT_ASSERT_MSG(cellY + LV_TANK_ICON_CELL_H <= 259, "the cells reach the grey under the well");
        for (g = 0; g < LV_TANK_GAUGE_COUNT; g++) {
            int x = STATUS_TANK_SHELLS + gaugeX[g] + LV_TANK_ICON_CELL_DX;
            UT_ASSERT_MSG(x == wantX[g], "cell %d starts at column %d (want %d)", g, x, wantX[g]);
            if (g > 0) {
                UT_ASSERT_MSG(x >= wantX[g - 1] + LV_TANK_ICON_CELL_W, "cell %d is over cell %d", g, g - 1);
            }
        }
    }

    /* As tall as a line of text, in the cell's own shape (9 by 10). */
    UT_ASSERT_MSG(lvTankIconWidth(10.0f) == 9.0f, "at its own size");
    UT_ASSERT_MSG(lvTankIconWidth(20.0f) == 18.0f, "twice its size");
    UT_ASSERT_MSG(lvTankIconWidth(13.0f) == 12.0f, "a 13 pixel line: 11.7 to 12");
    UT_ASSERT_MSG(lvTankIconWidth(0.0f) == 1.0f, "never under one pixel");

    /* The line with no word: the colon and the numbers apart. */
    {
        bool colon = false;
        UT_ASSERT_MSG(strcmp(lvTankStockNumbers(": 12 / 40", &colon), "12 / 40") == 0 && colon,
                      "the English line");
        UT_ASSERT_MSG(strcmp(lvTankStockNumbers("  :0 / 40", &colon), "0 / 40") == 0 && colon,
                      "spaces before, none after");
        UT_ASSERT_MSG(strcmp(lvTankStockNumbers("12 / 40", &colon), "12 / 40") == 0 && !colon,
                      "no colon");
        UT_ASSERT_MSG(strcmp(lvTankStockNumbers("?", &colon), "?") == 0 && !colon,
                      "some other text is kept whole");
        UT_ASSERT_MSG(strcmp(lvTankStockNumbers(":", &colon), "") == 0 && colon, "only a colon");
        UT_ASSERT_MSG(strcmp(lvTankStockNumbers(NULL, &colon), "") == 0 && !colon, "no line");
        UT_ASSERT_MSG(strcmp(lvTankStockNumbers(": 1 / 2", NULL), "1 / 2") == 0, "no flag asked for");
    }

    /* The value after the word of a line. */
    {
        bool colon = false;
        UT_ASSERT_MSG(strcmp(lvTankAfterColon("Pillboxes carried: 2", &colon), "2") == 0 && colon,
                      "the English line");
        UT_ASSERT_MSG(strcmp(lvTankAfterColon("2 pillboxes", &colon), "2 pillboxes") == 0 && !colon,
                      "no colon: the whole line");
        UT_ASSERT_MSG(strcmp(lvTankAfterColon("a:b:c", &colon), "b:c") == 0 && colon,
                      "the first colon");
        UT_ASSERT_MSG(strcmp(lvTankAfterColon("word:", &colon), "") == 0 && colon, "nothing after");
        UT_ASSERT_MSG(strcmp(lvTankAfterColon(NULL, &colon), "") == 0 && !colon, "no line");
    }

    /* The pillbox's picture: whole multiples of its 12 pixels. */
    UT_ASSERT_MSG(lvTankPillIconSize(13.0f, 12) == 12.0f, "a 13 pixel line");
    UT_ASSERT_MSG(lvTankPillIconSize(24.0f, 12) == 24.0f, "a 24 pixel line");
    UT_ASSERT_MSG(lvTankPillIconSize(8.0f, 12) == 12.0f, "never under its own size");

    /* The numbers end under each other: start + width is the same for a
       short line and for the widest. */
    {
        float wide  = lvTankIconNumberX(12.0f, 4.0f, 3.0f, 60.0f, 60.0f);
        float narrow = lvTankIconNumberX(12.0f, 4.0f, 3.0f, 60.0f, 45.0f);
        UT_ASSERT_MSG(wide == 19.0f, "the widest starts after picture, colon and gap (%f)", (double)wide);
        UT_ASSERT_MSG(narrow == 34.0f, "a shorter one starts further in (%f)", (double)narrow);
        UT_ASSERT_MSG(wide + 60.0f == narrow + 45.0f, "the ends do not meet");
        UT_ASSERT_MSG(lvTankIconNumberX(12.0f, 4.0f, 3.0f, 40.0f, 50.0f) == 19.0f,
                      "numbers wider than the column are not pushed left");
    }

    return 0;
}
