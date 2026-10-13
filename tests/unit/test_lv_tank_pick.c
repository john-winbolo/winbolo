/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums behind a click on a tank on the log viewer's map
 * (lv_tank_pick.h).
 *
 *   lv_tank_pick_sums — that a point on a tank's picture picks that tank
 *       and one beside it picks none; that the box is never under 12 points
 *       a side, so a tank can be hit when the map is zoomed out, on a
 *       display of any pixel density; that of two tanks under the point the
 *       one with the nearer middle wins; that a tank that cannot be picked
 *       (dead) is passed over, also when it lies over a live one; and where
 *       the mark round the selected tank goes.
 *
 *   lv_name_header_sums — the line that names a player at the top of the
 *       Tank and HUD panels (lv_name_header.h): that the X button is at the
 *       right end; that a name that goes in is whole and one that does not
 *       is cut to the room left, never under the button; and that in a line
 *       too narrow for anything the button still has its place after the
 *       stripe.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_tank_pick.h"
#include "lv_name_header.h"
#include "test_harness.h"

int run_lv_tank_pick_sums(void) {
    LvTankPickBox t[3];
    LvTankMark m;

    /* One tank, squares of 32: the picture is 100..132 by 200..232. */
    t[0].x = 100.0f; t[0].y = 200.0f; t[0].slot = 4; t[0].pickable = true;
    UT_ASSERT_MSG(lvTankPick(t, 1, 116.0f, 216.0f, 32.0f, 1.0f) == 0, "the middle");
    UT_ASSERT_MSG(lvTankPick(t, 1, 100.0f, 200.0f, 32.0f, 1.0f) == 0, "the top left pixel");
    UT_ASSERT_MSG(lvTankPick(t, 1, 131.9f, 231.9f, 32.0f, 1.0f) == 0, "the last pixel");
    UT_ASSERT_MSG(lvTankPick(t, 1, 132.0f, 216.0f, 32.0f, 1.0f) == -1, "one past the right");
    UT_ASSERT_MSG(lvTankPick(t, 1, 99.9f, 216.0f, 32.0f, 1.0f) == -1, "one before the left");
    UT_ASSERT_MSG(lvTankPick(t, 1, 116.0f, 232.0f, 32.0f, 1.0f) == -1, "one past the bottom");
    UT_ASSERT_MSG(lvTankPick(t, 1, 116.0f, 199.9f, 32.0f, 1.0f) == -1, "one above the top");
    UT_ASSERT_MSG(lvTankPick(t, 0, 116.0f, 216.0f, 32.0f, 1.0f) == -1, "no tanks");
    UT_ASSERT_MSG(lvTankPick(NULL, 1, 116.0f, 216.0f, 32.0f, 1.0f) == -1, "no list");
    UT_ASSERT_MSG(lvTankPick(t, 1, 116.0f, 216.0f, 0.0f, 1.0f) == -1, "no square size");

    /* Zoomed out: squares of 8. The picture is 100..108; the box is 12 a
       side round its middle (104), so 98..110. */
    UT_ASSERT_MSG(lvTankPickBoxSide(8.0f, 1.0f) == 12.0f, "the least box");
    UT_ASSERT_MSG(lvTankPickBoxSide(32.0f, 1.0f) == 32.0f, "the picture when it is larger");
    UT_ASSERT_MSG(lvTankPick(t, 1, 98.0f, 204.0f, 8.0f, 1.0f) == 0, "2 px left of the picture");
    UT_ASSERT_MSG(lvTankPick(t, 1, 109.9f, 204.0f, 8.0f, 1.0f) == 0, "2 px right of it");
    UT_ASSERT_MSG(lvTankPick(t, 1, 97.9f, 204.0f, 8.0f, 1.0f) == -1, "outside the slack");
    UT_ASSERT_MSG(lvTankPick(t, 1, 110.0f, 204.0f, 8.0f, 1.0f) == -1, "outside the slack, right");
    /* The slack is in points: on a display of density 2 a square of 16
       pixels is 8 points, and the box is 24 pixels. */
    UT_ASSERT_MSG(lvTankPickBoxSide(16.0f, 2.0f) == 24.0f, "the least box at density 2");
    UT_ASSERT_MSG(lvTankPick(t, 1, 97.0f, 208.0f, 16.0f, 2.0f) == 0, "density 2: in the slack");
    UT_ASSERT_MSG(lvTankPick(t, 1, 95.9f, 208.0f, 16.0f, 2.0f) == -1, "density 2: outside");
    UT_ASSERT_MSG(lvTankPickBoxSide(16.0f, 0.0f) == 16.0f, "a density of 0 is 1");

    /* Two tanks that overlap: 100..132 and 120..152. A point on both goes
       to the nearer middle (116 and 136). */
    t[1].x = 120.0f; t[1].y = 200.0f; t[1].slot = 9; t[1].pickable = true;
    UT_ASSERT_MSG(lvTankPick(t, 2, 122.0f, 216.0f, 32.0f, 1.0f) == 0, "nearer the first");
    UT_ASSERT_MSG(lvTankPick(t, 2, 130.0f, 216.0f, 32.0f, 1.0f) == 1, "nearer the second");
    UT_ASSERT_MSG(lvTankPick(t, 2, 126.0f, 216.0f, 32.0f, 1.0f) == 0, "a tie: the earlier");
    UT_ASSERT_MSG(lvTankPick(t, 2, 140.0f, 216.0f, 32.0f, 1.0f) == 1, "on the second only");
    UT_ASSERT_MSG(lvTankPick(t, 2, 105.0f, 216.0f, 32.0f, 1.0f) == 0, "on the first only");

    /* A tank that cannot be picked is passed over, and does not hide the
       live one under it. */
    t[0].pickable = false;
    UT_ASSERT_MSG(lvTankPick(t, 2, 105.0f, 216.0f, 32.0f, 1.0f) == -1, "a dead tank alone");
    UT_ASSERT_MSG(lvTankPick(t, 2, 122.0f, 216.0f, 32.0f, 1.0f) == 1, "the live one under it");
    t[0].pickable = true;

    /* The mark: 2 points out from the picture, 1 thick, corners a third of
       the side. */
    m = lvTankMarkLayout(100.7f, 200.3f, 32.0f, 1.0f);
    UT_ASSERT_MSG(m.x == 97.0f && m.y == 197.0f, "the mark starts 3 px out (%g, %g)",
                  (double)m.x, (double)m.y);
    UT_ASSERT_MSG(m.side == 38.0f && m.thick == 1.0f && m.arm == 12.0f,
                  "the mark's size (%g, %g, %g)", (double)m.side, (double)m.thick, (double)m.arm);
    m = lvTankMarkLayout(100.0f, 200.0f, 32.0f, 2.0f);
    UT_ASSERT_MSG(m.x == 94.0f && m.thick == 2.0f && m.side == 44.0f,
                  "the mark at density 2 (%g, %g, %g)", (double)m.x, (double)m.thick,
                  (double)m.side);
    m = lvTankMarkLayout(10.0f, 10.0f, 12.0f, 1.0f);
    UT_ASSERT_MSG(m.arm == 6.0f && m.arm * 2.0f <= m.side, "the corners do not meet");
    return 0;
}

int run_lv_name_header_sums(void) {
    LvNameHeader h;

    /* A line of 200: lead 8, a name of 60, a button of 13, a gap of 4. */
    h = lvNameHeaderLayout(200.0f, 8.0f, 60.0f, 13.0f, 4.0f);
    UT_ASSERT_MSG(h.nameX == 8.0f, "the name starts after the lead");
    UT_ASSERT_MSG(h.closeX == 187.0f, "the button ends at the right end");
    UT_ASSERT_MSG(!h.nameCut && h.nameW == 60.0f, "a name that goes in is whole");

    /* The name has exactly the room: lead 8 + 175 + gap 4 + 13 = 200. */
    h = lvNameHeaderLayout(200.0f, 8.0f, 175.0f, 13.0f, 4.0f);
    UT_ASSERT_MSG(!h.nameCut && h.nameW == 175.0f, "an exact fit is whole");
    h = lvNameHeaderLayout(200.0f, 8.0f, 176.0f, 13.0f, 4.0f);
    UT_ASSERT_MSG(h.nameCut && h.nameW == 175.0f, "one over is cut to the room");
    UT_ASSERT_MSG(h.nameX + h.nameW + 4.0f <= h.closeX, "the name ends a gap before the button");

    /* A narrow line: the name gives way, the button does not. */
    h = lvNameHeaderLayout(94.0f, 30.0f, 300.0f, 13.0f, 4.0f);
    UT_ASSERT_MSG(h.closeX == 81.0f && h.nameCut && h.nameW == 47.0f,
                  "94 wide (%g, %g)", (double)h.closeX, (double)h.nameW);
    UT_ASSERT_MSG(h.nameX + h.nameW <= h.closeX, "no overlap at 94");

    /* Too narrow for the lead and the button: the button straight after the
       lead, the name with no room. */
    h = lvNameHeaderLayout(20.0f, 30.0f, 300.0f, 13.0f, 4.0f);
    UT_ASSERT_MSG(h.closeX == 30.0f && h.nameW == 0.0f && h.nameCut, "20 wide");
    h = lvNameHeaderLayout(-5.0f, 8.0f, 10.0f, 13.0f, 4.0f);
    UT_ASSERT_MSG(h.closeX == 8.0f && h.nameW == 0.0f, "no width at all");

    /* An empty name is not "cut". */
    h = lvNameHeaderLayout(200.0f, 8.0f, 0.0f, 13.0f, 4.0f);
    UT_ASSERT_MSG(!h.nameCut && h.nameW == 0.0f, "an empty name");
    return 0;
}

/* A double click on a tank on the map: when the click turns that tank's
   view on (the eye of the HUD panel). */
/* One click as the viewer runs it. mode 1 is Select Team mode, where a
   single click on a tank moves a view that is on to that tank; in mode 0
   (Information) a single click leaves the view alone. */
static void tp_click(int mode, int count, int tank, int *view, int *prev, int *before,
                     int *changes) {
    int was = *view;
    LvMapClickView v;
    int prevTank = (count >= 2) ? *prev : -1;
    *prev = tank;
    if ((count % 2) != 0) *before = *view;
    if (mode == 1 && tank >= 0 && *view >= 0) *view = tank;
    v = lvMapClickViewsTank(count, tank, prevTank, *before);
    if (v == LV_MAP_VIEW_OFF) *view = -1;
    if (v == LV_MAP_VIEW_ON) *view = tank;
    if (*view != was) (*changes)++;
}

int run_lv_map_double_click(void) {
    int mode;

    /* One click selects and no more. */
    UT_ASSERT_MSG(lvMapClickViewsTank(1, 3, -1, -1) == LV_MAP_VIEW_KEEP, "a single click on a tank");
    UT_ASSERT_MSG(lvMapClickViewsTank(1, 3, 3, -1) == LV_MAP_VIEW_KEEP,
                  "a slow second click is a single click");
    UT_ASSERT_MSG(lvMapClickViewsTank(1, 3, 3, 3) == LV_MAP_VIEW_KEEP,
                  "a single click on the viewed tank");
    UT_ASSERT_MSG(lvMapClickViewsTank(0, 3, 3, -1) == LV_MAP_VIEW_KEEP, "no click counted");

    /* Case 1: a double click on a tank with no view on: its view comes on. */
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 3, 3, -1) == LV_MAP_VIEW_ON, "a double click, all teams shown");
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 0, 0, -1) == LV_MAP_VIEW_ON, "slot 0 is a tank");
    /* Case 2: on another tank while a view is on: the view goes to it. */
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 3, 3, 5) == LV_MAP_VIEW_ON, "from the view of another player");
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 15, 15, 0) == LV_MAP_VIEW_ON, "the last slot");
    /* Case 3: on the tank whose view is on: off, back to all teams. */
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 3, 3, 3) == LV_MAP_VIEW_OFF, "a double click on the viewed tank");
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 0, 0, 0) == LV_MAP_VIEW_OFF, "slot 0, viewed");

    /* A third click is the first of a new pair: nothing. A fourth is the
       second of that pair. */
    UT_ASSERT_MSG(lvMapClickViewsTank(3, 3, 3, 3) == LV_MAP_VIEW_KEEP, "a third click, view on");
    UT_ASSERT_MSG(lvMapClickViewsTank(3, 3, 3, -1) == LV_MAP_VIEW_KEEP, "a third click, view off");
    UT_ASSERT_MSG(lvMapClickViewsTank(4, 3, 3, 3) == LV_MAP_VIEW_OFF, "a fourth, view on before the third");
    UT_ASSERT_MSG(lvMapClickViewsTank(4, 3, 3, -1) == LV_MAP_VIEW_ON, "a fourth, view off before the third");

    /* Not on a tank: the map, a pillbox, a base. Nothing new. */
    UT_ASSERT_MSG(lvMapClickViewsTank(2, -1, -1, -1) == LV_MAP_VIEW_KEEP, "a double click on the map");
    UT_ASSERT_MSG(lvMapClickViewsTank(2, -1, -1, 3) == LV_MAP_VIEW_KEEP, "on the map, a view on: it stays");
    UT_ASSERT_MSG(lvMapClickViewsTank(2, -1, 3, -1) == LV_MAP_VIEW_KEEP, "tank, then the map");

    /* Both clicks on the same tank. */
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 3, -1, -1) == LV_MAP_VIEW_KEEP,
                  "the map, then a tank that drove under");
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 3, 4, -1) == LV_MAP_VIEW_KEEP, "one tank, then another");
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 3, 4, 4) == LV_MAP_VIEW_KEEP,
                  "one tank, then another, the first one viewed");
    UT_ASSERT_MSG(lvMapClickViewsTank(2, 3, 4, 3) == LV_MAP_VIEW_KEEP,
                  "one tank, then the viewed one");

    /* The whole double click as the viewer runs it, in both modes: where
       the view ends, and that the second click is the one that decides. */
    for (mode = 0; mode <= 1; mode++) {
        int view, prev, before, changes;

        /* No view on: on for the tank, changed once. */
        view = -1; prev = -1; before = -1; changes = 0;
        tp_click(mode, 1, 7, &view, &prev, &before, &changes);
        UT_ASSERT_MSG(view == -1, "mode %d: the first click alone leaves all teams", mode);
        tp_click(mode, 2, 7, &view, &prev, &before, &changes);
        UT_ASSERT_MSG(view == 7 && changes == 1, "mode %d: off, double click: on (%d, %d)", mode, view, changes);

        /* The view of this tank on: off, changed once, and not by the
           first click. */
        view = 7; prev = -1; before = -1; changes = 0;
        tp_click(mode, 1, 7, &view, &prev, &before, &changes);
        UT_ASSERT_MSG(view == 7 && changes == 0, "mode %d: the first click alone keeps the view", mode);
        tp_click(mode, 2, 7, &view, &prev, &before, &changes);
        UT_ASSERT_MSG(view == -1 && changes == 1, "mode %d: on, double click: off (%d, %d)", mode, view, changes);

        /* The view of another tank on: it ends on this tank. In Select Team
           mode the first click has moved it there already; the second must
           not take that for "its view was on" and turn it off. */
        view = 2; prev = -1; before = -1; changes = 0;
        tp_click(mode, 1, 7, &view, &prev, &before, &changes);
        UT_ASSERT_MSG(view == (mode == 1 ? 7 : 2), "mode %d: the first click (%d)", mode, view);
        tp_click(mode, 2, 7, &view, &prev, &before, &changes);
        UT_ASSERT_MSG(view == 7 && changes == 1, "mode %d: other view, double click: this tank (%d, %d)",
                      mode, view, changes);

        /* A triple click is a double click and one click more. */
        view = -1; prev = -1; before = -1; changes = 0;
        tp_click(mode, 1, 7, &view, &prev, &before, &changes);
        tp_click(mode, 2, 7, &view, &prev, &before, &changes);
        tp_click(mode, 3, 7, &view, &prev, &before, &changes);
        UT_ASSERT_MSG(view == 7 && changes == 1, "mode %d: three clicks (%d, %d)", mode, view, changes);
        /* Two double clicks in one row: on, then off again. */
        tp_click(mode, 4, 7, &view, &prev, &before, &changes);
        UT_ASSERT_MSG(view == -1 && changes == 2, "mode %d: four clicks (%d, %d)", mode, view, changes);
    }
    return 0;
}
