/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The number of each pillbox the log viewer shows on its map and in its
 * tooltips (lv_pill_numbers.h).
 *
 *   lv_pill_numbers_sums — the number of a stored pillbox, which pillboxes
 *       have a number on the map, the view square a pillbox stands in, the
 *       map square under the mouse, and a number as text.
 *   lv_pill_numbers_list — a list of numbers as bits and as text, cut short
 *       when there is no room.
 *   lv_pill_numbers_layout — where the number goes beside its pillbox at
 *       small, usual and large zooms and on a display of twice the density.
 *   lv_pill_numbers_overview — the Overview panel notes the number of each
 *       pillbox in the row it counts it in (lv_overview.h).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lv_overview.h"
#include "lv_pill_numbers.h"
#include "test_harness.h"

static bool pn_close(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < 0.001f;
}

int run_lv_pill_numbers_sums(void) {
    char text[LV_PILL_NUMBER_TEXT_MAX];
    char small[2];
    int vx = -1, vy = -1, mx = -1, my = -1, n;

    /* The game's list counts from 1; the stores from 0. */
    UT_ASSERT(lvPillNumber(0) == 1);
    UT_ASSERT(lvPillNumber(6) == 7);
    UT_ASSERT(lvPillNumber(15) == 16);

    /* A number only for a pillbox standing on the map: not for one a tank
       carries, and not for one the round took off the map. */
    UT_ASSERT(lvPillLabelShown(true, false));
    UT_ASSERT(!lvPillLabelShown(true, true));
    UT_ASSERT(!lvPillLabelShown(false, false));
    UT_ASSERT(!lvPillLabelShown(false, true));

    /* The view square: the view's top left square is 0, 0, and the margin
       column and row (lastX, lastY) count as in the view. */
    UT_ASSERT(lvPillViewSquare(100, 50, 90, 40, 20, 15, &vx, &vy) && vx == 10 && vy == 10);
    UT_ASSERT(lvPillViewSquare(90, 40, 90, 40, 20, 15, &vx, &vy) && vx == 0 && vy == 0);
    UT_ASSERT(lvPillViewSquare(110, 55, 90, 40, 20, 15, &vx, &vy) && vx == 20 && vy == 15);
    UT_ASSERT_MSG(!lvPillViewSquare(111, 50, 90, 40, 20, 15, &vx, &vy), "past the margin");
    UT_ASSERT_MSG(!lvPillViewSquare(100, 56, 90, 40, 20, 15, &vx, &vy), "under the margin");
    UT_ASSERT_MSG(!lvPillViewSquare(89, 50, 90, 40, 20, 15, &vx, &vy), "left of the view");
    UT_ASSERT_MSG(!lvPillViewSquare(100, 39, 90, 40, 20, 15, &vx, &vy), "above the view");
    UT_ASSERT(lvPillViewSquare(100, 50, 90, 40, 20, 15, NULL, NULL));

    /* The square under the mouse, as a click on the map picks it: whole
       squares of the picture from the view's top left. */
    UT_ASSERT(lvPillMapSquareAt(0, 0, 16, 90, 40, &mx, &my) && mx == 90 && my == 40);
    UT_ASSERT(lvPillMapSquareAt(15, 15, 16, 90, 40, &mx, &my) && mx == 90 && my == 40);
    UT_ASSERT(lvPillMapSquareAt(16, 31, 16, 90, 40, &mx, &my) && mx == 91 && my == 41);
    UT_ASSERT(lvPillMapSquareAt(165, 170, 16, 90, 40, &mx, &my) && mx == 100 && my == 50);
    /* The map's numbers go round at 256, as the click's do. */
    UT_ASSERT(lvPillMapSquareAt(16 * 10, 0, 16, 250, 0, &mx, &my) && mx == 4 && my == 0);
    UT_ASSERT_MSG(!lvPillMapSquareAt(-1, 0, 16, 90, 40, &mx, &my), "left of the picture");
    UT_ASSERT_MSG(!lvPillMapSquareAt(0, -1, 16, 90, 40, &mx, &my), "above the picture");
    UT_ASSERT_MSG(!lvPillMapSquareAt(0, 0, 0, 90, 40, &mx, &my), "no square size");

    /* A number as text; nothing for one no pillbox has. */
    n = lvPillNumberText(7, text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "7") == 0 && n == 1, "7: \"%s\" %d", text, n);
    n = lvPillNumberText(16, text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "16") == 0 && n == 2, "16: \"%s\" %d", text, n);
    strcpy(text, "x");
    UT_ASSERT(lvPillNumberText(0, text, sizeof(text)) == 0 && text[0] == '\0');
    strcpy(text, "x");
    UT_ASSERT(lvPillNumberText(-3, text, sizeof(text)) == 0 && text[0] == '\0');
    UT_ASSERT(lvPillNumberText(7, NULL, 4) == 0);
    UT_ASSERT(lvPillNumberText(7, text, 0) == 0);
    n = lvPillNumberText(16, small, sizeof(small));
    UT_ASSERT_MSG(n == 1 && strcmp(small, "1") == 0, "cut short: \"%s\" %d", small, n);
    /* Every character of a number has a picture: the text is as wide in
       cells as its digits. */
    UT_ASSERT(lvBuilderTextCells("7") == 3);
    UT_ASSERT(lvBuilderTextCells("16") == 7);
    return 0;
}

int run_lv_pill_numbers_list(void) {
    char text[LV_PILL_NUMBER_LIST_MAX];
    char small[8];
    uint32_t all = 0;
    int number, n;

    UT_ASSERT(lvPillNumberBit(1) == 1u);
    UT_ASSERT(lvPillNumberBit(16) == 0x8000u);
    UT_ASSERT(lvPillNumberBit(0) == 0u);
    UT_ASSERT(lvPillNumberBit(17) == 0u);

    n = lvPillNumberListText(lvPillNumberBit(7) | lvPillNumberBit(1) | lvPillNumberBit(4),
                             text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "1, 4, 7") == 0 && n == 7, "\"%s\" %d", text, n);
    n = lvPillNumberListText(lvPillNumberBit(12), text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "12") == 0 && n == 2, "one: \"%s\"", text);
    strcpy(text, "x");
    n = lvPillNumberListText(0, text, sizeof(text));
    UT_ASSERT_MSG(text[0] == '\0' && n == 0, "none: \"%s\"", text);

    /* All sixteen fit the list's room. */
    for (number = 1; number <= LV_PILL_NUMBERS_MAX; number++) all |= lvPillNumberBit(number);
    n = lvPillNumberListText(all, text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16") == 0 &&
                  n == (int)strlen(text), "all: \"%s\"", text);

    /* Too little room: the list stops at the last whole number. */
    n = lvPillNumberListText(all, small, sizeof(small));
    UT_ASSERT_MSG(strcmp(small, "1, 2, 3") == 0 && n == 7, "cut short: \"%s\" %d", small, n);
    n = lvPillNumberListText(lvPillNumberBit(16), small, 2);
    UT_ASSERT_MSG(small[0] == '\0' && n == 0, "no room for one: \"%s\"", small);
    UT_ASSERT(lvPillNumberListText(all, NULL, 8) == 0);
    UT_ASSERT(lvPillNumberListText(all, text, 0) == 0);
    return 0;
}

int run_lv_pill_numbers_layout(void) {
    LvPillLabelLayout l;

    /* The usual zoom, 16 pixels a square: one pixel a cell. "7" is 3 by 5
       cells and stands 3 by 4 pixels into the square's bottom right corner
       (no more than a quarter of the square, and never more than itself). */
    l = lvPillLabelLayout(100.0f, 200.0f, 16.0f, 1.0f, 3);
    UT_ASSERT(l.visible);
    UT_ASSERT(pn_close(l.cell, 1.0f));
    UT_ASSERT(pn_close(l.w, 3.0f) && pn_close(l.h, 5.0f));
    UT_ASSERT_MSG(pn_close(l.x, 113.0f) && pn_close(l.y, 212.0f), "16 px: %g, %g", l.x, l.y);

    /* "16" is 7 cells wide: a quarter of the square in, the rest outside. */
    l = lvPillLabelLayout(100.0f, 200.0f, 16.0f, 1.0f, 7);
    UT_ASSERT(pn_close(l.w, 7.0f));
    UT_ASSERT_MSG(pn_close(l.x, 112.0f) && pn_close(l.y, 212.0f), "16 px, two digits: %g, %g",
                  l.x, l.y);

    /* Zoomed out to 8 pixels a square: the digits keep their size, so they
       still read, and only 2 pixels of them are over the square. */
    l = lvPillLabelLayout(40.0f, 80.0f, 8.0f, 1.0f, 7);
    UT_ASSERT(pn_close(l.cell, 1.0f));
    UT_ASSERT_MSG(pn_close(l.x, 46.0f) && pn_close(l.y, 86.0f), "8 px: %g, %g", l.x, l.y);

    /* Zoomed in to 64 pixels a square: two pixels a cell, and a quarter of
       the square is more than the number, so it is all inside the corner. */
    l = lvPillLabelLayout(0.0f, 0.0f, 64.0f, 1.0f, 7);
    UT_ASSERT(pn_close(l.cell, 2.0f));
    UT_ASSERT(pn_close(l.w, 14.0f) && pn_close(l.h, 10.0f));
    UT_ASSERT_MSG(pn_close(l.x, 50.0f) && pn_close(l.y, 54.0f), "64 px: %g, %g", l.x, l.y);
    UT_ASSERT(pn_close(l.x + l.w, 64.0f) && pn_close(l.y + l.h, 64.0f));

    /* A display of twice the density: a 16 point square is 32 pixels, and a
       cell is still one point, two pixels. */
    l = lvPillLabelLayout(0.0f, 0.0f, 32.0f, 2.0f, 3);
    UT_ASSERT(pn_close(l.cell, 2.0f));
    UT_ASSERT_MSG(pn_close(l.x, 26.0f) && pn_close(l.y, 24.0f), "2x: %g, %g", l.x, l.y);

    /* Whole pixels from a square that does not start on one. */
    l = lvPillLabelLayout(10.6f, 20.4f, 16.0f, 1.0f, 3);
    UT_ASSERT_MSG(pn_close(l.x, 23.0f) && pn_close(l.y, 32.0f), "whole: %g, %g", l.x, l.y);

    /* No text, no number. */
    l = lvPillLabelLayout(0.0f, 0.0f, 16.0f, 1.0f, 0);
    UT_ASSERT(!l.visible);
    return 0;
}

int run_lv_pill_numbers_overview(void) {
    LvOverview o;

    lvOverviewInit(&o);
    UT_ASSERT(lvOverviewAddPlayer(&o, 2, 5, "Rider"));
    UT_ASSERT(lvOverviewAddPlayer(&o, 4, 1, "ErYan"));
    lvOverviewCountPill(&o, true, 2, 1);
    lvOverviewCountPill(&o, true, 2, 7);
    lvOverviewCountPill(&o, true, 4, 3);
    /* Nobody owns it, and an owner whose team has no row: both neutral. */
    lvOverviewCountPill(&o, false, 0, 16);
    lvOverviewCountPill(&o, true, 9, 5);
    /* A number no pillbox has is counted and not noted. */
    lvOverviewCountPill(&o, true, 4, 0);

    UT_ASSERT(o.totalPills == 6);
    UT_ASSERT(o.rows[0].team == 2 && o.rows[0].pills == 2);
    UT_ASSERT(o.rows[0].pillNumbers == (lvPillNumberBit(1) | lvPillNumberBit(7)));
    UT_ASSERT(o.rows[1].team == 4 && o.rows[1].pills == 2);
    UT_ASSERT(o.rows[1].pillNumbers == lvPillNumberBit(3));
    UT_ASSERT(o.neutralPills == 2);
    UT_ASSERT(o.neutralPillNumbers == (lvPillNumberBit(16) | lvPillNumberBit(5)));
    /* Bases have no numbers to note. */
    lvOverviewCount(&o, true, true, 2);
    UT_ASSERT(o.rows[0].bases == 1 && o.rows[0].pillNumbers == (lvPillNumberBit(1) | lvPillNumberBit(7)));
    return 0;
}
