/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A sidebar of the log viewer whose unused part is transparent
 * (lv_dock_clear.h).
 *
 *   lv_dock_clear_layout — the panels are packed from the start with a grip
 *       after each and the rest is clear; when they do not fit the last
 *       gives up length first, down to its least; when not even the least
 *       lengths fit all share the room; a drag of a grip changes one panel
 *       and stops where the later panels are at their least; the first
 *       length of a panel is its content's, and a long list keeps its own.
 *
 *   lv_dock_clear_rects — the same plan as rectangles in each of the four
 *       sidebars: down the left and right ones, across the top and bottom
 *       ones (where a panel's far edge is its right edge), the panels beside
 *       the sidebar's own grip, the clear part to the end.
 *
 *   lv_dock_clear_map — the map goes under a clear sidebar and stops at a
 *       solid one; a point in the clear part is on the map and a point on
 *       the panels is not; the middle of the visible map does not move when
 *       the option is turned on; the scroll that keeps the map in place.
 *
 *   lv_dock_clear_prefs — the option of each sidebar and the lengths of its
 *       panels are read back as written; damaged text gives the defaults.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lv_dock_clear.h"
#include "test_harness.h"

int run_lv_dock_clear_layout(void) {
    LvDockClearPlan p;
    int wanted[4] = { 120, 96, 200, 150 };
    int min[4] = { 48, 48, 48, 48 };
    int i, extent;

    /* They fit: each as asked, a grip of 4 after each, the rest clear. */
    lvDockClearLayout(wanted, min, 4, 900, 4, &p);
    UT_ASSERT_MSG(p.count == 4, "four panels");
    UT_ASSERT_MSG(p.pos[0] == 0 && p.len[0] == 120, "the first at the start");
    UT_ASSERT_MSG(p.pos[1] == 124 && p.len[1] == 96, "the second after the first's grip");
    UT_ASSERT_MSG(p.pos[2] == 224 && p.pos[3] == 428, "no room between panels");
    UT_ASSERT_MSG(p.used == 582, "120 + 96 + 200 + 150 + 4 grips");
    UT_ASSERT_MSG(p.clearLen == 318, "the rest is clear");

    /* Exactly full: nothing clear, nothing changed. */
    lvDockClearLayout(wanted, min, 4, 582, 4, &p);
    UT_ASSERT_MSG(p.len[3] == 150 && p.clearLen == 0, "exactly full");

    /* 100 short: the last panel gives it up, the others keep theirs. */
    lvDockClearLayout(wanted, min, 4, 482, 4, &p);
    UT_ASSERT_MSG(p.len[0] == 120 && p.len[1] == 96 && p.len[2] == 200, "the first three keep their length");
    UT_ASSERT_MSG(p.len[3] == 50 && p.clearLen == 0, "the last is shorter");

    /* 200 short: the last down to its least, then the one before it. */
    lvDockClearLayout(wanted, min, 4, 382, 4, &p);
    UT_ASSERT_MSG(p.len[3] == 48, "the last at its least length");
    UT_ASSERT_MSG(p.len[2] == 102 && p.len[1] == 96 && p.len[0] == 120, "then the third");
    UT_ASSERT_MSG(p.used == 382, "all of the sidebar");

    /* Not even 4 x 48 + 16 fit: all share, and all of it is used. */
    lvDockClearLayout(wanted, min, 4, 116, 4, &p);
    UT_ASSERT_MSG(p.len[0] == 25 && p.len[1] == 25 && p.len[2] == 25 && p.len[3] == 25, "equal least lengths: equal shares");
    UT_ASSERT_MSG(p.used == 116 && p.clearLen == 0, "nothing past the end");

    /* A length under the least is brought up to it. */
    {
        int small[2] = { 10, 0 };
        lvDockClearLayout(small, min, 2, 500, 4, &p);
        UT_ASSERT_MSG(p.len[0] == 48 && p.len[1] == 48, "never under the least length");
    }

    /* No panel: all clear. */
    lvDockClearLayout(wanted, min, 0, 500, 4, &p);
    UT_ASSERT_MSG(p.count == 0 && p.used == 0 && p.clearLen == 500, "an empty sidebar is all clear");

    /* Every length of sidebar: in order, no overlap, never past the end
       while the least lengths fit. */
    for (extent = 0; extent <= 1000; extent++) {
        int at = 0;
        lvDockClearLayout(wanted, min, 4, extent, 4, &p);
        for (i = 0; i < 4; i++) {
            UT_ASSERT_MSG(p.pos[i] == at, "each starts where the last grip ended");
            UT_ASSERT_MSG(p.len[i] >= 1, "a panel has a length");
            at += p.len[i] + 4;
        }
        if (extent >= 4 * 48 + 16) {
            UT_ASSERT_MSG(p.used <= extent, "inside the sidebar");
            UT_ASSERT_MSG(p.used + p.clearLen == extent, "panels and clear part are all of it");
            for (i = 0; i < 4; i++) {
                UT_ASSERT_MSG(p.len[i] >= 48, "at its least length or more");
            }
        }
    }

    /* A drag of the second panel's grip. */
    lvDockClearLayout(wanted, min, 4, 900, 4, &p);
    UT_ASSERT_MSG(lvDockClearDragLen(&p, min, 1, 300, 900, 4) == 300, "as asked while the rest fits");
    UT_ASSERT_MSG(lvDockClearDragLen(&p, min, 1, 10, 900, 4) == 48, "not under its least");
    /* 900 - 16 - 120 before it - 2 x 48 after it = 668. */
    UT_ASSERT_MSG(lvDockClearDragLen(&p, min, 1, 5000, 900, 4) == 668, "stops where the later panels are at their least");
    /* The last panel: up to the end of the sidebar. 900 - 16 - 120 - 96 - 200. */
    UT_ASSERT_MSG(lvDockClearDragLen(&p, min, 3, 5000, 900, 4) == 468, "the last up to the end");
    {
        int w2[4] = { 120, 668, 200, 150 };
        lvDockClearLayout(w2, min, 4, 900, 4, &p);
        UT_ASSERT_MSG(p.len[1] == 668 && p.len[2] == 48 && p.len[3] == 48, "that drag fits exactly");
        UT_ASSERT_MSG(p.used == 900, "to the end");
    }

    /* The first length: the content's when that is shorter. */
    UT_ASSERT_MSG(lvDockClearFirstLen(300, 118, 48, 0) == 118, "a short panel stops at its content");
    UT_ASSERT_MSG(lvDockClearFirstLen(300, 2400, 48, 0) == 300, "a long list keeps its length");
    UT_ASSERT_MSG(lvDockClearFirstLen(300, 0, 48, 0) == 300, "content not known: as it is");
    UT_ASSERT_MSG(lvDockClearFirstLen(300, 20, 48, 0) == 48, "never under the least");
    UT_ASSERT_MSG(lvDockClearFirstLen(0, 90, 48, 0) == 90, "no length yet: the content's");
    /* A panel that shows a fixed amount gets all of it. */
    UT_ASSERT_MSG(lvDockClearFirstLen(266, 394, 48, 1) == 394, "a fixed panel grows to its content");
    UT_ASSERT_MSG(lvDockClearFirstLen(266, 142, 48, 1) == 142, "and shrinks to it");
    UT_ASSERT_MSG(lvDockClearFirstLen(266, 0, 48, 1) == 266, "fixed, content not known: as it is");
    /* A double click. */
    UT_ASSERT_MSG(lvDockClearSnapLen(300, 118, 48) == 118, "snap to the content");
    UT_ASSERT_MSG(lvDockClearSnapLen(100, 400, 48) == 400, "also when that is longer");
    UT_ASSERT_MSG(lvDockClearSnapLen(100, 0, 48) == 100, "not known: no change");

    UT_ASSERT_MSG(lvDockClearMin(LV_DOCK_LEFT) == 48 && lvDockClearMin(LV_DOCK_RIGHT) == 48, "least height");
    UT_ASSERT_MSG(lvDockClearMin(LV_DOCK_TOP) == 110 && lvDockClearMin(LV_DOCK_BOTTOM) == 110, "least width");
    return 0;
}

static bool dc_rect_is(LvDockRect r, int x, int y, int w, int h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

int run_lv_dock_clear_rects(void) {
    LvDockClearPlan p;
    int wanted[2] = { 100, 200 };
    int minDown[2] = { 48, 48 };
    int minAcross[2] = { 110, 110 };
    LvDockRect strip, r;

    UT_ASSERT_MSG(lvDockClearIsDown(LV_DOCK_LEFT) && lvDockClearIsDown(LV_DOCK_RIGHT), "left and right: down");
    UT_ASSERT_MSG(!lvDockClearIsDown(LV_DOCK_TOP) && !lvDockClearIsDown(LV_DOCK_BOTTOM), "top and bottom: across");

    /* The right sidebar: 300 wide at x 900, 700 high under a menu bar of
       20. Its own grip (6) is on its left edge. */
    strip.x = 900; strip.y = 20; strip.w = 300; strip.h = 700;
    lvDockClearLayout(wanted, minDown, 2, strip.h, 4, &p);
    r = lvDockClearPanelRect(LV_DOCK_RIGHT, strip, 6, &p, 0);
    UT_ASSERT_MSG(dc_rect_is(r, 906, 20, 294, 100), "right: the first panel at the top");
    r = lvDockClearGripRect(LV_DOCK_RIGHT, strip, 4, &p, 0);
    UT_ASSERT_MSG(dc_rect_is(r, 900, 120, 300, 4), "right: its grip is its bottom edge");
    r = lvDockClearPanelRect(LV_DOCK_RIGHT, strip, 6, &p, 1);
    UT_ASSERT_MSG(dc_rect_is(r, 906, 124, 294, 200), "right: the second under it");
    r = lvDockClearUsedRect(LV_DOCK_RIGHT, strip, &p);
    UT_ASSERT_MSG(dc_rect_is(r, 900, 20, 300, 308), "right: what the panels take");
    r = lvDockClearClearRect(LV_DOCK_RIGHT, strip, &p);
    UT_ASSERT_MSG(dc_rect_is(r, 900, 328, 300, 392), "right: clear from there to the bottom");

    /* The left sidebar: its own grip is on its right edge. */
    strip.x = 0;
    r = lvDockClearPanelRect(LV_DOCK_LEFT, strip, 6, &p, 1);
    UT_ASSERT_MSG(dc_rect_is(r, 0, 124, 294, 200), "left: the panels keep off the right edge");
    r = lvDockClearClearRect(LV_DOCK_LEFT, strip, &p);
    UT_ASSERT_MSG(dc_rect_is(r, 0, 328, 300, 392), "left: clear to the bottom");

    /* The bottom sidebar: 800 wide from x 200, 180 high at y 540. Packed
       from the left; a panel's far edge is its right edge; its own grip is
       on its top edge. */
    strip.x = 200; strip.y = 540; strip.w = 800; strip.h = 180;
    lvDockClearLayout(wanted, minAcross, 2, strip.w, 4, &p);
    UT_ASSERT_MSG(p.len[0] == 110, "across: the least width");
    r = lvDockClearPanelRect(LV_DOCK_BOTTOM, strip, 6, &p, 0);
    UT_ASSERT_MSG(dc_rect_is(r, 200, 546, 110, 174), "bottom: the first panel at the left");
    r = lvDockClearGripRect(LV_DOCK_BOTTOM, strip, 4, &p, 0);
    UT_ASSERT_MSG(dc_rect_is(r, 310, 540, 4, 180), "bottom: its grip is its right edge");
    r = lvDockClearPanelRect(LV_DOCK_BOTTOM, strip, 6, &p, 1);
    UT_ASSERT_MSG(dc_rect_is(r, 314, 546, 200, 174), "bottom: the second to its right");
    r = lvDockClearClearRect(LV_DOCK_BOTTOM, strip, &p);
    UT_ASSERT_MSG(dc_rect_is(r, 518, 540, 482, 180), "bottom: clear from there to the right");

    /* The top sidebar: its own grip is on its bottom edge. */
    strip.y = 20;
    r = lvDockClearPanelRect(LV_DOCK_TOP, strip, 6, &p, 0);
    UT_ASSERT_MSG(dc_rect_is(r, 200, 20, 110, 174), "top: the panels keep off the bottom edge");
    r = lvDockClearUsedRect(LV_DOCK_TOP, strip, &p);
    UT_ASSERT_MSG(dc_rect_is(r, 200, 20, 318, 180), "top: what the panels take");
    r = lvDockClearClearRect(LV_DOCK_TOP, strip, &p);
    UT_ASSERT_MSG(dc_rect_is(r, 518, 20, 482, 180), "top: clear to the right");

    /* Panels that fill the sidebar: no clear part. */
    {
        int big[2] = { 500, 500 };
        lvDockClearLayout(big, minAcross, 2, strip.w, 4, &p);
        r = lvDockClearClearRect(LV_DOCK_TOP, strip, &p);
        UT_ASSERT_MSG(r.w == 0, "full: nothing clear");
        r = lvDockClearUsedRect(LV_DOCK_TOP, strip, &p);
        UT_ASSERT_MSG(dc_rect_is(r, 200, 20, 800, 180), "full: all of the strip");
    }
    return 0;
}

int run_lv_dock_clear_map(void) {
    LvDockMargins m = { 280, 0, 300, 180 }; /* left, top, right, bottom */
    LvDockMargins map;
    int clear[LV_DOCK_SIDES] = { 0, 0, 0, 0 };
    LvDockRect used[LV_DOCK_SIDES];
    int i;

    memset(used, 0, sizeof(used));

    /* Option off everywhere: the map is bounded as it always was. */
    map = lvDockClearOpenMargins(&m, clear);
    UT_ASSERT_MSG(map.left == 280 && map.top == 0 && map.right == 300 && map.bottom == 180,
                  "open: with no clear sidebar, every margin");
    map = lvDockClearMapMargins(&m, clear);
    UT_ASSERT_MSG(map.left == 280 && map.right == 300 && map.bottom == 180, "solid sidebars bound the map");
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 600.0f, 300.0f) == 1, "the middle is map");
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 100.0f, 600.0f) == 0, "a solid sidebar is not");

    /* The left sidebar clear: the map goes under it. Its panels take the
       top 328 of it. */
    clear[LV_DOCK_LEFT] = 1;
    used[LV_DOCK_LEFT].x = 0; used[LV_DOCK_LEFT].y = 20;
    used[LV_DOCK_LEFT].w = 280; used[LV_DOCK_LEFT].h = 328;
    map = lvDockClearMapMargins(&m, clear);
    UT_ASSERT_MSG(map.left == 0 && map.right == 300, "only the clear one lets the map under");
    UT_ASSERT_MSG(map.top == 0 && map.bottom == 0, "a clear left sidebar: the map reaches the window's bottom");
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 600.0f, 600.0f) == 0, "the solid bottom sidebar is not map");
    {
        /* A clear top or bottom sidebar alone does not take the left and
           right ones' bounds away. */
        int onlyTop[LV_DOCK_SIDES] = { 0, 0, 0, 0 };
        LvDockMargins mm = { 280, 90, 300, 180 };
        onlyTop[LV_DOCK_TOP] = 1;
        map = lvDockClearMapMargins(&mm, onlyTop);
        UT_ASSERT_MSG(map.left == 280 && map.top == 0 && map.right == 300 && map.bottom == 180, "a clear top sidebar frees only the top");
    }
    {
        /* What the solid sidebars take: the clear left one takes nothing,
           and the solid bottom one keeps its margin though the map is drawn
           under it. */
        LvDockMargins open = lvDockClearOpenMargins(&m, clear);
        UT_ASSERT_MSG(open.left == 0 && open.top == 0 && open.right == 300 && open.bottom == 180,
                      "open: only the clear sidebar gives its strip up");
        /* The middle of the window less these is on the map. */
        UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20,
                                            (float)open.left + (float)(1200 - open.left - open.right) * 0.5f,
                                            (float)(20 + open.top) + (float)(720 - 20 - open.top - open.bottom) * 0.5f) == 1,
                      "open: its middle shows");
    }
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 100.0f, 600.0f) == 1, "the clear part is map");
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 100.0f, 100.0f) == 0, "a panel is not");
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 100.0f, 347.0f) == 0, "nor its last grip");
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 100.0f, 348.0f) == 1, "the first point under it is");
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 100.0f, 10.0f) == 0, "the menu bar is not");
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 1000.0f, 600.0f) == 0, "the solid right sidebar is not");

    /* The bottom one clear too; its panels take its left 318. */
    clear[LV_DOCK_BOTTOM] = 1;
    used[LV_DOCK_BOTTOM].x = 280; used[LV_DOCK_BOTTOM].y = 540;
    used[LV_DOCK_BOTTOM].w = 318; used[LV_DOCK_BOTTOM].h = 180;
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 300.0f, 600.0f) == 0, "bottom: a panel");
    UT_ASSERT_MSG(lvDockClearPointOnMap(&m, clear, used, 1200, 720, 20, 700.0f, 600.0f) == 1, "bottom: to the right of the panels");

    /* The middle of the visible map. A view 1200 map pixels long with 280
       of it under the left sidebar and 300 under the right one: the middle
       of the 620 between them. */
    UT_ASSERT_MSG(lvDockClearViewCentre(1200, 0, 0) == 600, "no sidebar: half the view");
    UT_ASSERT_MSG(lvDockClearViewCentre(1200, 280, 300) == 590, "between the sidebars");
    UT_ASSERT_MSG(lvDockClearViewCentre(1200, 280, 0) == 740, "one at the start");
    UT_ASSERT_MSG(lvDockClearViewCentre(100, 60, 60) == 50, "nothing left between them: the whole view");
    /* The option does not move the middle on the screen: with it off the
       map starts at 280 and is 620 long, with it on it starts at 0. */
    UT_ASSERT_MSG(280 + lvDockClearViewCentre(620, 0, 0) == lvDockClearViewCentre(1200, 280, 300),
                  "the same point of the window, on and off");
    for (i = 0; i <= 400; i++) {
        UT_ASSERT_MSG(i + lvDockClearViewCentre(1000 - i, 0, 0) == lvDockClearViewCentre(1000, i, 0),
                      "at every size of sidebar");
    }

    /* The scroll that keeps the map where it is when the option is turned
       on: the map now starts 280 points further left, so its scroll goes
       back by 280 points of map. */
    UT_ASSERT_MSG(lvDockClearScrollShift(280, 0, 1.0f) == -280, "on, zoom 1");
    UT_ASSERT_MSG(lvDockClearScrollShift(0, 280, 1.0f) == 280, "off again");
    UT_ASSERT_MSG(lvDockClearScrollShift(280, 0, 2.0f) == -140, "zoom 2: half as many map pixels");
    UT_ASSERT_MSG(lvDockClearScrollShift(280, 0, 0.5f) == -560, "zoom 0.5");
    UT_ASSERT_MSG(lvDockClearScrollShift(100, 100, 1.0f) == 0, "no change, no move");
    return 0;
}

int run_lv_dock_clear_prefs(void) {
    int clear[LV_DOCK_SIDES], back[LV_DOCK_SIDES];
    int len[LV_DOCK_CLEAR_PANELS], lenBack[LV_DOCK_CLEAR_PANELS];
    char text[256];
    char tiny[8];
    int i;

    /* The option of each sidebar. */
    clear[LV_DOCK_RIGHT] = 0; clear[LV_DOCK_LEFT] = 1;
    clear[LV_DOCK_TOP] = 0; clear[LV_DOCK_BOTTOM] = 1;
    UT_ASSERT_MSG(lvDockClearFlagsFormat(clear, text, sizeof(text)) > 0, "the options fit");
    UT_ASSERT_MSG(strcmp(text, "v1 R=0 L=1 T=0 B=1") == 0, "one line");
    UT_ASSERT_MSG(lvDockClearFlagsParse(text, back) == 1, "read back");
    for (i = 0; i < LV_DOCK_SIDES; i++) {
        UT_ASSERT_MSG(back[i] == clear[i], "as written");
    }
    UT_ASSERT_MSG(lvDockClearFlagsParse("", back) == 0 && back[LV_DOCK_LEFT] == 0, "none saved: off");
    UT_ASSERT_MSG(lvDockClearFlagsParse(NULL, back) == 0, "no text: off");
    UT_ASSERT_MSG(lvDockClearFlagsParse("Yes", back) == 0 && back[LV_DOCK_RIGHT] == 0, "another format: off");
    UT_ASSERT_MSG(lvDockClearFlagsParse("v1 R=7 L=1 Q=1 T=", back) == 1, "a damaged line still reads");
    UT_ASSERT_MSG(back[LV_DOCK_RIGHT] == 0 && back[LV_DOCK_LEFT] == 1 && back[LV_DOCK_TOP] == 0, "only what reads correctly");
    UT_ASSERT_MSG(lvDockClearFlagsFormat(clear, tiny, sizeof(tiny)) == -1 && tiny[0] == '\0', "too small a buffer");

    /* The lengths of one sidebar's panels. */
    memset(len, 0, sizeof(len));
    len[lvDockClearPanelIndex("###controls")] = 118;
    len[lvDockClearPanelIndex("###hud")] = 96;
    len[lvDockClearPanelIndex("###events")] = 420;
    UT_ASSERT_MSG(lvDockClearLensFormat(len, text, sizeof(text)) > 0, "the lengths fit");
    UT_ASSERT_MSG(strcmp(text, "v1 Controls=118 HUD=96 Events=420") == 0, "only the panels that have one");
    UT_ASSERT_MSG(lvDockClearLensParse(text, lenBack) == 1, "read back");
    for (i = 0; i < LV_DOCK_CLEAR_PANELS; i++) {
        UT_ASSERT_MSG(lenBack[i] == len[i], "as written");
    }
    /* Every panel with a length. */
    for (i = 0; i < LV_DOCK_CLEAR_PANELS; i++) len[i] = 50 + i * 37;
    UT_ASSERT_MSG(lvDockClearLensFormat(len, text, sizeof(text)) > 0, "all nine fit");
    UT_ASSERT_MSG(lvDockClearLensParse(text, lenBack) == 1, "read back");
    for (i = 0; i < LV_DOCK_CLEAR_PANELS; i++) {
        UT_ASSERT_MSG(lenBack[i] == len[i], "every panel as written");
    }
    UT_ASSERT_MSG(lvDockClearLensParse("", lenBack) == 0 && lenBack[0] == 0, "none saved: no length");
    UT_ASSERT_MSG(lvDockClearLensParse("v1 Tank=0 HUD=99999 Controls=abc Nothing=5 Events=300 Overview=", lenBack) == 1,
                  "a damaged line still reads");
    UT_ASSERT_MSG(lenBack[lvDockClearPanelIndex("###events")] == 300, "the good one is kept");
    UT_ASSERT_MSG(lenBack[lvDockClearPanelIndex("###tank")] == 0, "0 is no length");
    UT_ASSERT_MSG(lenBack[lvDockClearPanelIndex("###hud")] == 0, "too big is not believed");
    UT_ASSERT_MSG(lenBack[lvDockClearPanelIndex("###controls")] == 0, "not a number");
    UT_ASSERT_MSG(lenBack[lvDockClearPanelIndex("###overview")] == 0, "cut short");
    /* A name that starts like another is not that one. */
    UT_ASSERT_MSG(lvDockClearLensParse("v1 Event=77 Eventss=78", lenBack) == 1 &&
                      lenBack[lvDockClearPanelIndex("###events")] == 0, "the whole name has to match");
    UT_ASSERT_MSG(lvDockClearLensFormat(len, tiny, sizeof(tiny)) == -1 && tiny[0] == '\0', "too small a buffer");

    UT_ASSERT_MSG(lvDockClearPanelIndex("###tank") == 3, "a panel by its ID");
    UT_ASSERT_MSG(lvDockClearPanelIndex("###nothing") == -1 && lvDockClearPanelIndex(NULL) == -1, "not a panel");
    return 0;
}
