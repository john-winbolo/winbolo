/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * How the log viewer's window is shared between the map and the sidebars
 * (lv_dock_fit.h), and the text the sidebar layout is saved as
 * (lv_dock_layout.h).
 *
 *   lv_dock_fit_sums — with no dock the squares across are the nearest whole
 *       number, as the window sizing has always worked them out; beside the
 *       dock a part square counts as a whole one, so the map reaches the
 *       dock; the count stays inside 1..255; and the dock's width is held
 *       between its smallest and two thirds of the window.
 *
 *   lv_dock_fit_margins — the four sidebars together: one that is off takes
 *       nothing; two that face each other leave the map a third of the
 *       window however much they ask for; the left and right ones do not
 *       change the top and bottom ones; a dragged edge stops where the
 *       facing sidebar's share ends; and the map's part of the window and
 *       the test for a point on it follow from the margins.
 *
 *   lv_dock_layout_sides — the options line is read back as written; a line
 *       that is cut short, has a damaged side or is not this format gives
 *       the defaults for what cannot be read and never a failure.
 *
 *   lv_dock_layout_tree — an arrangement is read back as written, with its
 *       splits, shares, tab order and the tab in front; text that is cut
 *       short, damaged, too deep or too big is refused whole.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lv_dock_fit.h"
#include "lv_dock_layout.h"
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

int run_lv_dock_fit_margins(void) {
    int wanted[LV_DOCK_SIDES];
    int showing[LV_DOCK_SIDES];
    LvDockMargins m;
    int x, y, w, h;

    /* A top or bottom sidebar's height: as asked inside the limits, raised
       to the smallest, cut to two thirds of the height under the menu bar. */
    UT_ASSERT_MSG(lvDockClampHeight(220, 700) == 220, "220 of 700: %d",
                  lvDockClampHeight(220, 700));
    UT_ASSERT_MSG(lvDockClampHeight(10, 700) == LV_DOCK_HEIGHT_MIN,
                  "10 of 700: %d", lvDockClampHeight(10, 700));
    UT_ASSERT_MSG(lvDockClampHeight(900, 600) == 400, "900 of 600: %d",
                  lvDockClampHeight(900, 600));
    UT_ASSERT_MSG(lvDockClampHeight(220, 120) == LV_DOCK_HEIGHT_MIN,
                  "220 of 120: %d", lvDockClampHeight(220, 120));
    UT_ASSERT_MSG(lvDockClampHeight(220, 60) == 60, "220 of 60: %d",
                  lvDockClampHeight(220, 60));
    UT_ASSERT_MSG(lvDockClampHeight(220, 0) == 0, "no height: %d",
                  lvDockClampHeight(220, 0));

    UT_ASSERT(lvDockSideIsWide(LV_DOCK_RIGHT) && lvDockSideIsWide(LV_DOCK_LEFT));
    UT_ASSERT(!lvDockSideIsWide(LV_DOCK_TOP) && !lvDockSideIsWide(LV_DOCK_BOTTOM));
    UT_ASSERT(lvDockSideMin(LV_DOCK_LEFT) == LV_DOCK_WIDTH_MIN);
    UT_ASSERT(lvDockSideMin(LV_DOCK_BOTTOM) == LV_DOCK_HEIGHT_MIN);
    UT_ASSERT(lvDockSideDefault(LV_DOCK_RIGHT) == LV_DOCK_WIDTH_DEFAULT);
    UT_ASSERT(lvDockSideDefault(LV_DOCK_TOP) == LV_DOCK_HEIGHT_DEFAULT);

    wanted[LV_DOCK_RIGHT] = 420;
    wanted[LV_DOCK_LEFT] = 420;
    wanted[LV_DOCK_TOP] = 220;
    wanted[LV_DOCK_BOTTOM] = 220;

    /* None showing: nothing is taken. */
    showing[LV_DOCK_RIGHT] = showing[LV_DOCK_LEFT] = 0;
    showing[LV_DOCK_TOP] = showing[LV_DOCK_BOTTOM] = 0;
    m = lvDockFitMargins(wanted, showing, 1280, 700);
    UT_ASSERT_MSG(m.left == 0 && m.top == 0 && m.right == 0 && m.bottom == 0,
                  "none showing: %d %d %d %d", m.left, m.top, m.right, m.bottom);

    /* Only the right one, as when it was the only one there was. */
    showing[LV_DOCK_RIGHT] = 1;
    m = lvDockFitMargins(wanted, showing, 1280, 700);
    UT_ASSERT_MSG(m.left == 0 && m.top == 0 && m.right == 420 && m.bottom == 0,
                  "right only: %d %d %d %d", m.left, m.top, m.right, m.bottom);

    /* All four in a window with room: each gets what it asked for, and the
       map is the rectangle between them under a 20 point menu bar. */
    showing[LV_DOCK_LEFT] = showing[LV_DOCK_TOP] = showing[LV_DOCK_BOTTOM] = 1;
    m = lvDockFitMargins(wanted, showing, 1280, 700);
    UT_ASSERT_MSG(m.left == 420 && m.top == 220 && m.right == 420 && m.bottom == 220,
                  "all four, room: %d %d %d %d", m.left, m.top, m.right, m.bottom);
    lvDockMapRect(&m, 1280, 720, 20, &x, &y, &w, &h);
    UT_ASSERT_MSG(x == 420 && y == 240 && w == 440 && h == 260,
                  "map rect: %d %d %d %d", x, y, w, h);

    /* All four in a small window: each pair is cut to two thirds of its
       length, so the map keeps a third each way. */
    m = lvDockFitMargins(wanted, showing, 600, 300);
    UT_ASSERT_MSG(m.left == 200 && m.right == 200, "small, across: %d %d",
                  m.left, m.right);
    UT_ASSERT_MSG(m.top == 100 && m.bottom == 100, "small, down: %d %d",
                  m.top, m.bottom);
    lvDockMapRect(&m, 600, 320, 20, &x, &y, &w, &h);
    UT_ASSERT_MSG(w == 200 && h == 100, "small map: %d by %d", w, h);

    /* Unequal asks give up in proportion: 300 and 600 of 900 become 200 and
       400. The top and bottom ones are not changed by it. */
    wanted[LV_DOCK_LEFT] = 300;
    wanted[LV_DOCK_RIGHT] = 600;
    m = lvDockFitMargins(wanted, showing, 900, 700);
    UT_ASSERT_MSG(m.left == 200 && m.right == 400, "in proportion: %d %d",
                  m.left, m.right);
    UT_ASSERT_MSG(m.top == 220 && m.bottom == 220, "down untouched: %d %d",
                  m.top, m.bottom);

    /* A window with no size takes nothing and still gives a map of one
       point, so nothing divides by zero. */
    m = lvDockFitMargins(wanted, showing, 0, 0);
    UT_ASSERT_MSG(m.left == 0 && m.top == 0 && m.right == 0 && m.bottom == 0,
                  "no window: %d %d %d %d", m.left, m.top, m.right, m.bottom);
    m.left = 500; m.right = 500; m.top = 400; m.bottom = 400;
    lvDockMapRect(&m, 600, 300, 20, &x, &y, &w, &h);
    UT_ASSERT_MSG(w == 1 && h == 1, "margins past the window: %d by %d", w, h);

    /* The pair sum on its own: one side off is left alone, as is a pair
       that fits. */
    {
        int a = 500, b = 0;
        lvDockFitPair(&a, &b, 600);
        UT_ASSERT_MSG(a == 500 && b == 0, "one of the pair off: %d %d", a, b);
        a = 100; b = 200;
        lvDockFitPair(&a, &b, 600);
        UT_ASSERT_MSG(a == 100 && b == 200, "pair fits: %d %d", a, b);
    }

    /* Dragging an edge: stops at what the facing sidebar leaves of the two
       thirds, never under the smallest size, and the smallest wins in a
       window too small for both. */
    UT_ASSERT_MSG(lvDockDragSize(LV_DOCK_LEFT, 900, 300, 1200) == 500,
                  "drag left past: %d", lvDockDragSize(LV_DOCK_LEFT, 900, 300, 1200));
    UT_ASSERT_MSG(lvDockDragSize(LV_DOCK_LEFT, 350, 300, 1200) == 350,
                  "drag left inside: %d", lvDockDragSize(LV_DOCK_LEFT, 350, 300, 1200));
    UT_ASSERT_MSG(lvDockDragSize(LV_DOCK_RIGHT, 50, 0, 1200) == LV_DOCK_WIDTH_MIN,
                  "drag right under: %d", lvDockDragSize(LV_DOCK_RIGHT, 50, 0, 1200));
    UT_ASSERT_MSG(lvDockDragSize(LV_DOCK_TOP, 10, 0, 600) == LV_DOCK_HEIGHT_MIN,
                  "drag top under: %d", lvDockDragSize(LV_DOCK_TOP, 10, 0, 600));
    UT_ASSERT_MSG(lvDockDragSize(LV_DOCK_BOTTOM, 900, 150, 600) == 250,
                  "drag bottom past: %d", lvDockDragSize(LV_DOCK_BOTTOM, 900, 150, 600));
    UT_ASSERT_MSG(lvDockDragSize(LV_DOCK_RIGHT, 500, 0, 240) == LV_DOCK_WIDTH_MIN,
                  "drag in a small window: %d", lvDockDragSize(LV_DOCK_RIGHT, 500, 0, 240));
    UT_ASSERT_MSG(lvDockDragSize(LV_DOCK_TOP, 500, 0, 0) == LV_DOCK_HEIGHT_MIN,
                  "drag with no window: %d", lvDockDragSize(LV_DOCK_TOP, 500, 0, 0));

    /* A point on the map: inside the margins and under the menu bar. The
       edges belong to the map on the left and top and to the sidebar on the
       right and bottom. */
    m.left = 100; m.top = 50; m.right = 200; m.bottom = 60;
    UT_ASSERT(!lvDockPointOnMap(&m, 1000, 800, 20, 99.0f, 100.0f));
    UT_ASSERT(lvDockPointOnMap(&m, 1000, 800, 20, 100.0f, 70.0f));
    UT_ASSERT(!lvDockPointOnMap(&m, 1000, 800, 20, 100.0f, 69.0f));
    UT_ASSERT(lvDockPointOnMap(&m, 1000, 800, 20, 799.0f, 100.0f));
    UT_ASSERT(!lvDockPointOnMap(&m, 1000, 800, 20, 800.0f, 100.0f));
    UT_ASSERT(lvDockPointOnMap(&m, 1000, 800, 20, 500.0f, 739.0f));
    UT_ASSERT(!lvDockPointOnMap(&m, 1000, 800, 20, 500.0f, 740.0f));
    /* With no sidebar the whole window under the menu bar is the map. */
    m.left = m.top = m.right = m.bottom = 0;
    UT_ASSERT(lvDockPointOnMap(&m, 1000, 800, 20, 0.0f, 20.0f));
    UT_ASSERT(!lvDockPointOnMap(&m, 1000, 800, 20, 0.0f, 19.0f));
    return 0;
}

int run_lv_dock_layout_sides(void) {
    LvDockSides in, out;
    char text[128];
    char small[8];
    int i, n;

    /* Nothing saved: the defaults, and the caller is told so. */
    UT_ASSERT(lvDockSidesParse("", &out) == 0);
    for (i = 0; i < LV_DOCK_SIDES; i++) {
        UT_ASSERT_MSG(out.enabled[i] == 0 && out.size[i] == lvDockSideDefault(i),
                      "default side %d: %d %d", i, out.enabled[i], out.size[i]);
    }
    UT_ASSERT(out.rightLaidOut == 0);
    UT_ASSERT(lvDockSidesParse(NULL, &out) == 0);

    /* Written and read back. */
    lvDockSidesDefault(&in);
    in.enabled[LV_DOCK_RIGHT] = 1;  in.size[LV_DOCK_RIGHT] = 500;
    in.enabled[LV_DOCK_TOP] = 1;    in.size[LV_DOCK_TOP] = 150;
    in.enabled[LV_DOCK_BOTTOM] = 1; in.size[LV_DOCK_BOTTOM] = 300;
    in.rightLaidOut = 1;
    n = lvDockSidesFormat(&in, text, sizeof(text));
    UT_ASSERT_MSG(n > 0 && strcmp(text, "v1 R=1,500 L=0,420 T=1,150 B=1,300 I=1") == 0,
                  "written: \"%s\"", text);
    UT_ASSERT(lvDockSidesParse(text, &out) == 1);
    UT_ASSERT_MSG(memcmp(&in, &out, sizeof(in)) == 0, "round trip of \"%s\"", text);

    /* A line cut short keeps what came through whole and defaults the rest. */
    UT_ASSERT(lvDockSidesParse("v1 R=1,500 L=1,", &out) == 1);
    UT_ASSERT_MSG(out.enabled[LV_DOCK_RIGHT] == 1 && out.size[LV_DOCK_RIGHT] == 500,
                  "cut short, right: %d %d", out.enabled[LV_DOCK_RIGHT],
                  out.size[LV_DOCK_RIGHT]);
    UT_ASSERT_MSG(out.enabled[LV_DOCK_LEFT] == 0 &&
                  out.size[LV_DOCK_LEFT] == LV_DOCK_WIDTH_DEFAULT,
                  "cut short, left: %d %d", out.enabled[LV_DOCK_LEFT],
                  out.size[LV_DOCK_LEFT]);
    UT_ASSERT(lvDockSidesParse("v1", &out) == 1);
    UT_ASSERT(out.enabled[LV_DOCK_RIGHT] == 0);

    /* Damaged sides: an on/off that is neither, a size that is not a number,
       a size with too many digits. Each keeps its default; the good side
       beside them is kept, and words that are not known are passed over. */
    UT_ASSERT(lvDockSidesParse("v1 R=2,400 Z=9 L=1,abc T=1,999999 junk B=1,300 I=7",
                               &out) == 1);
    UT_ASSERT_MSG(out.enabled[LV_DOCK_RIGHT] == 0 &&
                  out.size[LV_DOCK_RIGHT] == LV_DOCK_WIDTH_DEFAULT, "bad on/off");
    UT_ASSERT_MSG(out.enabled[LV_DOCK_LEFT] == 0 &&
                  out.size[LV_DOCK_LEFT] == LV_DOCK_WIDTH_DEFAULT, "bad size");
    UT_ASSERT_MSG(out.enabled[LV_DOCK_TOP] == 0 &&
                  out.size[LV_DOCK_TOP] == LV_DOCK_HEIGHT_DEFAULT, "long size");
    UT_ASSERT_MSG(out.enabled[LV_DOCK_BOTTOM] == 1 && out.size[LV_DOCK_BOTTOM] == 300,
                  "good side: %d %d", out.enabled[LV_DOCK_BOTTOM],
                  out.size[LV_DOCK_BOTTOM]);
    UT_ASSERT(out.rightLaidOut == 0);

    /* A size outside what a sidebar can be is brought inside. */
    UT_ASSERT(lvDockSidesParse("v1 R=1,99999 T=1,5", &out) == 1);
    UT_ASSERT_MSG(out.size[LV_DOCK_RIGHT] == LV_DOCK_SIZE_MAX, "huge: %d",
                  out.size[LV_DOCK_RIGHT]);
    UT_ASSERT_MSG(out.size[LV_DOCK_TOP] == LV_DOCK_HEIGHT_MIN, "tiny: %d",
                  out.size[LV_DOCK_TOP]);

    /* Not this format at all. */
    UT_ASSERT(lvDockSidesParse("v2 R=1,400", &out) == 0);
    UT_ASSERT(out.enabled[LV_DOCK_RIGHT] == 0);
    UT_ASSERT(lvDockSidesParse("v10 R=1,400", &out) == 0);
    UT_ASSERT(lvDockSidesParse("Yes", &out) == 0);
    UT_ASSERT(lvDockSidesParse("[Window][###controls]", &out) == 0);

    /* A buffer too small for the line gives nothing, not half a line. */
    UT_ASSERT(lvDockSidesFormat(&in, small, sizeof(small)) == -1);
    UT_ASSERT(small[0] == '\0');
    return 0;
}

/* "X500(" n times round a leaf: a tree n splits deep. */
static void lvDockTestNest(char *out, size_t size, int n) {
    int i;
    out[0] = '\0';
    for (i = 0; i < n; i++) strncat(out, "X500(", size - strlen(out) - 1);
    strncat(out, "L[a]", size - strlen(out) - 1);
    for (i = 0; i < n; i++) strncat(out, ")(L[b])", size - strlen(out) - 1);
}

/* A full tree of splits `levels` deep. */
static void lvDockTestFull(char *out, size_t size, int levels) {
    if (levels == 0) {
        strncat(out, "L[a]", size - strlen(out) - 1);
        return;
    }
    strncat(out, "Y500(", size - strlen(out) - 1);
    lvDockTestFull(out, size, levels - 1);
    strncat(out, ")(", size - strlen(out) - 1);
    lvDockTestFull(out, size, levels - 1);
    strncat(out, ")", size - strlen(out) - 1);
}

int run_lv_dock_layout_tree(void) {
    static LvDockTree tree;
    static char big[LV_DOCK_TREE_TEXT_MAX + 64];
    char text[LV_DOCK_TREE_TEXT_MAX];
    char small[8];
    size_t i;
    static const char *const broken[] = {
        "",
        "Y350(L[###a])",                /* the second part is missing */
        "Y350(L[###a])(L[###b]",        /* cut short */
        "Y350(L[###a])(L[###b]))",      /* text after the end */
        "Y(L[###a])(L[###b])",          /* no share */
        "L[###a,]",
        "L[,###a]",
        "L[###a|x]",
        "L[###a|]",
        "L[###a",
        "L###a]",
        "Q[###a]",
        "L[###a]junk",
        "L[a b]",
        "L[a,b,c,d,e,f,g,h,i]",         /* more tabs than a part holds */
        "L[abcdefghijklmnopqrstuvwxyz0123456]", /* a name too long */
    };

    /* Written and read back: a split, the shares, the tab order, the tab in
       front and the part that takes the slack. */
    {
        const char *layout = "Y350(L[###controls])(L*[###events,###comments|1])";
        UT_ASSERT(lvDockTreeParse(layout, &tree) == 1);
        UT_ASSERT_MSG(tree.count == 3, "parts: %d", tree.count);
        UT_ASSERT(tree.node[0].axis == 'Y' && tree.node[0].permille == 350);
        UT_ASSERT(tree.node[0].child[0] == 1 && tree.node[0].child[1] == 2);
        UT_ASSERT(tree.node[1].axis == 0 && tree.node[1].winCount == 1);
        UT_ASSERT(strcmp(tree.node[1].win[0], "###controls") == 0);
        UT_ASSERT(tree.node[1].selected == -1 && tree.node[1].central == 0);
        UT_ASSERT(tree.node[2].winCount == 2 && tree.node[2].central == 1);
        UT_ASSERT(strcmp(tree.node[2].win[1], "###comments") == 0);
        UT_ASSERT(tree.node[2].selected == 1);
        UT_ASSERT(lvDockTreeWindowCount(&tree) == 3);
        UT_ASSERT(lvDockTreeHasCentral(&tree, 0, 0) == 1);
        UT_ASSERT(lvDockTreeHasCentral(&tree, 1, 0) == 0);
        UT_ASSERT(lvDockTreeHasCentral(&tree, 2, 0) == 1);
        UT_ASSERT(lvDockTreeFormat(&tree, text, sizeof(text)) == (int)strlen(layout));
        UT_ASSERT_MSG(strcmp(text, layout) == 0, "written: \"%s\"", text);

        /* Too small a buffer gives nothing, not half a tree. */
        UT_ASSERT(lvDockTreeFormat(&tree, small, sizeof(small)) == -1);
        UT_ASSERT(small[0] == '\0');
    }

    /* Splits inside splits, both ways. */
    {
        const char *layout =
            "X250(Y500(L[###hud])(L[###gameinfo|0]))(X600(L*[])(L[###iteminfo,###lvscnpanel]))";
        UT_ASSERT(lvDockTreeParse(layout, &tree) == 1);
        UT_ASSERT_MSG(tree.count == 7, "parts: %d", tree.count);
        UT_ASSERT(lvDockTreeWindowCount(&tree) == 4);
        UT_ASSERT(lvDockTreeHasCentral(&tree, tree.node[0].child[0], 1) == 0);
        UT_ASSERT(lvDockTreeHasCentral(&tree, tree.node[0].child[1], 1) == 1);
        UT_ASSERT(lvDockTreeFormat(&tree, text, sizeof(text)) > 0);
        UT_ASSERT_MSG(strcmp(text, layout) == 0, "written: \"%s\"", text);
    }

    /* An empty sidebar. */
    UT_ASSERT(lvDockTreeParse("L*[]", &tree) == 1);
    UT_ASSERT(tree.count == 1 && tree.node[0].winCount == 0);
    UT_ASSERT(lvDockTreeWindowCount(&tree) == 0);
    UT_ASSERT(lvDockTreeFormat(&tree, text, sizeof(text)) == 4);
    UT_ASSERT(strcmp(text, "L*[]") == 0);

    /* A tab in front that is not there is no tab in front, and a share
       outside 1..999 is brought inside. */
    UT_ASSERT(lvDockTreeParse("L[###a|7]", &tree) == 1);
    UT_ASSERT(tree.node[0].selected == -1);
    UT_ASSERT(lvDockTreeFormat(&tree, text, sizeof(text)) > 0);
    UT_ASSERT_MSG(strcmp(text, "L[###a]") == 0, "written: \"%s\"", text);
    UT_ASSERT(lvDockTreeParse("X0(L[a])(L[b])", &tree) == 1);
    UT_ASSERT(tree.node[0].permille == 1);
    UT_ASSERT(lvDockTreeParse("X5000(L[a])(L[b])", &tree) == 1);
    UT_ASSERT(tree.node[0].permille == 999);

    /* Damaged text is refused whole, and leaves nothing behind. */
    UT_ASSERT(lvDockTreeParse(NULL, &tree) == 0);
    for (i = 0; i < sizeof(broken) / sizeof(broken[0]); i++) {
        tree.count = 5;
        UT_ASSERT_MSG(lvDockTreeParse(broken[i], &tree) == 0, "accepted \"%s\"",
                      broken[i]);
        UT_ASSERT_MSG(tree.count == 0, "left parts behind for \"%s\"", broken[i]);
    }

    /* As deep as allowed reads; one deeper does not. */
    lvDockTestNest(text, sizeof(text), LV_DOCK_TREE_DEPTH);
    UT_ASSERT_MSG(lvDockTreeParse(text, &tree) == 1, "depth %d refused",
                  LV_DOCK_TREE_DEPTH);
    UT_ASSERT(tree.count == 2 * LV_DOCK_TREE_DEPTH + 1);
    lvDockTestNest(text, sizeof(text), LV_DOCK_TREE_DEPTH + 1);
    UT_ASSERT_MSG(lvDockTreeParse(text, &tree) == 0, "depth %d accepted",
                  LV_DOCK_TREE_DEPTH + 1);

    /* More parts than a tree holds: 4 levels is 31 and fits, 5 is 63. */
    text[0] = '\0';
    lvDockTestFull(text, sizeof(text), 4);
    UT_ASSERT(lvDockTreeParse(text, &tree) == 1);
    UT_ASSERT_MSG(tree.count == LV_DOCK_TREE_NODES, "full tree: %d", tree.count);
    text[0] = '\0';
    lvDockTestFull(text, sizeof(text), 5);
    UT_ASSERT(lvDockTreeParse(text, &tree) == 0);

    /* Text longer than any arrangement is not read at all. */
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    big[0] = 'L';
    big[1] = '[';
    UT_ASSERT(lvDockTreeParse(big, &tree) == 0);

    /* A tree that cannot be written: empty, or with a name the text cannot
       hold. */
    tree.count = 0;
    UT_ASSERT(lvDockTreeFormat(&tree, text, sizeof(text)) == -1);
    UT_ASSERT(lvDockTreeParse("L[###a]", &tree) == 1);
    strcpy(tree.node[0].win[0], "a,b");
    UT_ASSERT(lvDockTreeFormat(&tree, text, sizeof(text)) == -1);
    UT_ASSERT(text[0] == '\0');
    return 0;
}
