/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * test_lv_first_view.c - where the log viewer's map is when a log is first
 * shown (lv_first_view.h): the box round everything that is not deep sea,
 * and the zoom step the box is first shown at.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "lv_event_flash.h"
#include "lv_first_view.h"
#include "test_harness.h"

#define FV_SEA 0xFF
#define FV_GRASS 7
/* A mined square of grass: the game keeps it as the terrain plus 8. */
#define FV_MINED_GRASS 15

/* The viewer's zoom steps, smallest first. */
static const float fvSteps[] = { 0.5f, 0.6f, 0.7f, 0.8f, 0.9f,
                                 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
#define FV_STEPS ((int)(sizeof(fvSteps) / sizeof(fvSteps[0])))
#define FV_1X 5
#define FV_15X 6
#define FV_2X 7
#define FV_3X 8

static unsigned char *fv_sea(void) {
    unsigned char *t = (unsigned char *)malloc(LV_LAND_MAP_SIZE * LV_LAND_MAP_SIZE);
    if (t != NULL) memset(t, FV_SEA, LV_LAND_MAP_SIZE * LV_LAND_MAP_SIZE);
    return t;
}

static void fv_fill(unsigned char *t, int x0, int y0, int x1, int y1, unsigned char v) {
    int x, y;
    for (x = x0; x <= x1; x++) {
        for (y = y0; y <= y1; y++) t[x * LV_LAND_MAP_SIZE + y] = v;
    }
}

int run_lv_first_view_box(void) {
    unsigned char *t = fv_sea();
    LvLandBox box;

    UT_ASSERT(t != NULL);

    /* All sea, nothing on it: no box. */
    lvLandBoxClear(&box);
    lvLandBoxAddTerrain(&box, t, FV_SEA);
    UT_ASSERT_MSG(box.has == 0, "all sea: no box");
    UT_ASSERT(lvLandBoxWidth(&box) == 0 && lvLandBoxHeight(&box) == 0);
    /* No map at all is the same. */
    lvLandBoxAddTerrain(&box, NULL, FV_SEA);
    UT_ASSERT(box.has == 0);

    /* An island in a corner of the world: columns 30..49, rows 200..214. */
    fv_fill(t, 30, 200, 49, 214, FV_GRASS);
    lvLandBoxClear(&box);
    lvLandBoxAddTerrain(&box, t, FV_SEA);
    UT_ASSERT_MSG(box.has == 1 && box.x0 == 30 && box.x1 == 49 && box.y0 == 200 && box.y1 == 214,
                  "island: %d,%d to %d,%d", box.x0, box.y0, box.x1, box.y1);
    UT_ASSERT(lvLandBoxWidth(&box) == 20 && lvLandBoxHeight(&box) == 15);
    /* The middle: the line after column 39 (40 * 16), and the middle of row
       207 (207.5 * 16). */
    UT_ASSERT(lvLandBoxMidPx(box.x0, box.x1, 16) == 640.0f);
    UT_ASSERT(lvLandBoxMidPx(box.y0, box.y1, 16) == 3320.0f);

    /* A mined square of land is land; it makes the box bigger. */
    t[60 * LV_LAND_MAP_SIZE + 205] = FV_MINED_GRASS;
    lvLandBoxClear(&box);
    lvLandBoxAddTerrain(&box, t, FV_SEA);
    UT_ASSERT_MSG(box.x1 == 60 && box.y0 == 200, "a mined square of land is in the box");
    t[60 * LV_LAND_MAP_SIZE + 205] = FV_SEA;

    /* Pillboxes, a base and a start outside the land make the box bigger:
       a pillbox on the sea to the right, a base above, a start below left. */
    lvLandBoxClear(&box);
    lvLandBoxAddTerrain(&box, t, FV_SEA);
    lvLandBoxAdd(&box, 90, 210);
    lvLandBoxAdd(&box, 40, 180);
    lvLandBoxAdd(&box, 25, 220);
    UT_ASSERT_MSG(box.x0 == 25 && box.x1 == 90 && box.y0 == 180 && box.y1 == 220,
                  "items outside the land: %d,%d to %d,%d", box.x0, box.y0, box.x1, box.y1);
    /* One inside changes nothing, and one that is not on the map is passed
       over. */
    lvLandBoxAdd(&box, 40, 205);
    lvLandBoxAdd(&box, 256, 10);
    lvLandBoxAdd(&box, -1, 10);
    lvLandBoxAdd(&box, 10, 300);
    UT_ASSERT(box.x0 == 25 && box.x1 == 90 && box.y0 == 180 && box.y1 == 220);

    /* All sea with one pillbox: the box is its square. */
    memset(t, FV_SEA, LV_LAND_MAP_SIZE * LV_LAND_MAP_SIZE);
    lvLandBoxClear(&box);
    lvLandBoxAddTerrain(&box, t, FV_SEA);
    lvLandBoxAdd(&box, 126, 126);
    UT_ASSERT(box.has == 1 && lvLandBoxWidth(&box) == 1 && lvLandBoxHeight(&box) == 1);
    UT_ASSERT(lvLandBoxMidPx(box.x0, box.x1, 16) == 126.5f * 16.0f);

    /* The first and last squares of the map. */
    t[0] = FV_GRASS;
    t[255 * LV_LAND_MAP_SIZE + 255] = FV_GRASS;
    lvLandBoxClear(&box);
    lvLandBoxAddTerrain(&box, t, FV_SEA);
    UT_ASSERT(box.x0 == 0 && box.y0 == 0 && box.x1 == 255 && box.y1 == 255);
    UT_ASSERT(lvLandBoxWidth(&box) == 256);

    free(t);
    return 0;
}

int run_lv_first_view_zoom(void) {
    /* The map that shows is 980 by 780 points: at 1x that is 61 by 48
       squares. */
    const float visW = 980.0f, visH = 780.0f;

    /* The box goes in at the zoom the viewer has: the zoom is kept, at any
       step, and never goes up. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_1X, 20, 15, visW, visH, 16) == FV_1X);
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, 0, 20, 15, visW, visH, 16) == 0);
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, 3, 20, 15, visW, visH, 16) == 3);
    /* It is kept also when the box only just goes in, with no room round
       it: 61 squares are 976 points. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_1X, 61, 48, visW, visH, 16) == FV_1X);

    /* One square too wide for 1x. With a square of room on each side it is
       64 squares: 1024 map pixels, 921.6 points at 0.9x. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_1X, 62, 40, visW, visH, 16) == 4);
    /* 70 by 40: 72 * 16 = 1152 map pixels. 0.9x is 1036.8 points, too many;
       0.8x is 921.6. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_1X, 70, 40, visW, visH, 16) == 3);
    /* The way down decides too: 40 by 70 is 72 * 16 = 1152 map pixels down,
       576 points at 0.5x and 691.2 at 0.6x, 806.4 at 0.7x. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_1X, 40, 70, visW, visH, 16) == 1);
    /* From 3x: 30 by 20 does not go in (1440 points); at 2x with its room it
       is 32 * 16 * 2 = 1024, too wide; at 1.5x it is 32 * 24 = 768 across
       and 22 * 24 = 528 down, and goes in. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_3X, 30, 20, visW, visH, 16) == FV_15X);
    /* From 2x: 40 by 20 with its room is 42 * 32 = 1344 points across, too
       wide, and at 1.5x 42 * 24 = 1008, still too wide; at 1x it goes in. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_2X, 40, 20, visW, visH, 16) == FV_1X);
    /* From 2x: 28 by 20 with its room is 30 * 32 = 960 points across and
       22 * 32 = 704 down. It does not go in bare at 3x but the viewer is at
       2x, where it does: kept. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_2X, 28, 20, visW, visH, 16) == FV_2X);
    /* The 1.5x step is kept the same way: 40 by 30 is 960 by 720 points. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_15X, 40, 30, visW, visH, 16) == FV_15X);

    /* A box bigger than the view at the smallest step: the smallest step.
       150 by 150 is 1200 points at 0.5x. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_1X, 150, 150, visW, visH, 16) == 0);
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, 0, 150, 150, visW, visH, 16) == 0);
    /* The box goes in at 0.5x bare but not with its room: still the
       smallest step, as there is none under it. 122 squares are 976 points. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_1X, 122, 40, visW, visH, 16) == 0);

    /* No box (all sea): the zoom is kept. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_1X, 0, 0, visW, visH, 16) == FV_1X);
    /* A step that is not one of the viewer's is pulled in, and no table is
       step 0. */
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, 99, 20, 15, visW, visH, 16) == FV_STEPS - 1 ||
              lvFirstViewZoomStep(fvSteps, FV_STEPS, 99, 20, 15, visW, visH, 16) < FV_STEPS);
    UT_ASSERT(lvFirstViewZoomStep(fvSteps, FV_STEPS, -3, 20, 15, visW, visH, 16) == 0);
    UT_ASSERT(lvFirstViewZoomStep(NULL, 0, 2, 20, 15, visW, visH, 16) == 0);
    return 0;
}

/* The whole first view: the island in the corner, in a window 1280 by 800
   with a 20 point menu bar and a solid sidebar 300 points wide on the
   right. The view is the 980 by 780 points beside the sidebar. */
int run_lv_first_view_place(void) {
    unsigned char *t = fv_sea();
    LvLandBox box;
    int step, shownX, shownY, maxX, maxY, sx, sy;
    float zoom, midX, midY;

    UT_ASSERT(t != NULL);
    fv_fill(t, 100, 105, 149, 144, FV_GRASS);
    lvLandBoxClear(&box);
    lvLandBoxAddTerrain(&box, t, FV_SEA);
    free(t);

    /* 50 by 40 squares go in at 1x (800 by 640 points). */
    step = lvFirstViewZoomStep(fvSteps, FV_STEPS, FV_1X, lvLandBoxWidth(&box), lvLandBoxHeight(&box),
                               980.0f, 780.0f, 16);
    UT_ASSERT(step == FV_1X);
    zoom = fvSteps[step];
    shownX = (int)(980.0f / (16.0f * zoom)) + 1;
    shownY = (int)(780.0f / (16.0f * zoom)) + 1;
    maxX = (255 - shownX) * 16;
    maxY = (255 - shownY) * 16;
    sx = lvViewCentreScrollPx(lvLandBoxMidPx(box.x0, box.x1, 16), 0.0f, 980.0f, zoom, maxX);
    sy = lvViewCentreScrollPx(lvLandBoxMidPx(box.y0, box.y1, 16), 0.0f, 780.0f, zoom, maxY);
    /* The middle of the box, 2000 and 2000, less half of what shows. */
    UT_ASSERT_MSG(sx == 2000 - 490 && sy == 2000 - 390, "scroll %d,%d", sx, sy);
    /* The whole box is in the view, and its middle is the view's middle. */
    midX = (lvLandBoxMidPx(box.x0, box.x1, 16) - (float)sx) * zoom;
    midY = (lvLandBoxMidPx(box.y0, box.y1, 16) - (float)sy) * zoom;
    UT_ASSERT(midX == 490.0f && midY == 390.0f);
    UT_ASSERT((float)(box.x0 * 16 - sx) * zoom >= 0.0f);
    UT_ASSERT((float)((box.x1 + 1) * 16 - sx) * zoom <= 980.0f);
    UT_ASSERT((float)(box.y0 * 16 - sy) * zoom >= 0.0f);
    UT_ASSERT((float)((box.y1 + 1) * 16 - sy) * zoom <= 780.0f);

    /* Where the load used to open: the view's corner at square 127,127. The
       box's left edge was 27 squares off the view. */
    UT_ASSERT(127 * 16 > box.x0 * 16);
    return 0;
}
