/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          overview_camera.cpp
 * Purpose:       Implementation of the map overview's camera
 *                maths and its entity-visibility rule — see
 *                overview_camera.h. Arithmetic only: no ImGui,
 *                no SDL_Renderer, no state outside the
 *                OverviewCamera the caller owns.
 *********************************************************/

#include <cmath>

#include "overview_camera.h"

/* Discrete zoom ladder. 0.5x is the floor (8 px squares — a window smaller
 * than the 2048 px map pans rather than switching to colour blocks) and 4x
 * the ceiling (64 px squares). */
static const float kOverviewZoomSteps[] = {
    0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f
};
#define OVERVIEW_ZOOM_COUNT 7
#define OVERVIEW_ZOOM_1X    2 /* index of 1.0f */

static_assert((int)(sizeof(kOverviewZoomSteps) / sizeof(kOverviewZoomSteps[0]))
                  == OVERVIEW_ZOOM_COUNT,
              "kOverviewZoomSteps and OVERVIEW_ZOOM_COUNT disagree");
static_assert(OVERVIEW_ZOOM_1X < OVERVIEW_ZOOM_COUNT,
              "OVERVIEW_ZOOM_1X is off the ladder");

/* floorf / ceilf followed by a range-safe cast. A stale cursor position or a
 * degenerate view size can hand us a pixel millions of squares off the map,
 * and the float-to-int conversion is undefined once the value leaves int's
 * range; callers only ever need to know it was far outside. */
static int overviewFloorToInt(float v) {
    if (v < -1.0e6f) return -1000000;
    if (v >  1.0e6f) return  1000000;
    return (int)floorf(v);
}

static int overviewCeilToInt(float v) {
    if (v < -1.0e6f) return -1000000;
    if (v >  1.0e6f) return  1000000;
    return (int)ceilf(v);
}

/* View pixels one map square occupies at the camera's current zoom. Never
 * zero: the ladder's floor is 0.5x, so this bottoms out at 8. */
static float overviewTilePx(const OverviewCamera *cam) {
    return (float)OVERVIEW_TILE_PX * overviewCameraZoomScale(cam);
}

/* The one place camera state is brought back into range — every call that
 * moves or rezooms the camera ends here.
 *
 * The view spans cx +/- halfW square units, where halfW = viewW / (2 *
 * tilePx). The rule is that the view never leaves the map by more than half
 * a window. On the left the empty strip is halfW - cx and must not exceed
 * halfW, which gives cx >= 0; on the right the strip is
 * (cx + halfW) - MAP_ARRAY_SIZE and must not exceed halfW, which gives
 * cx <= MAP_ARRAY_SIZE. halfW cancels on both sides, so the clamp is the
 * same at every zoom and every window size and needs neither. Same
 * derivation in y. */
static void overviewCameraClamp(OverviewCamera *cam) {
    if (cam->cx < 0.0f) cam->cx = 0.0f;
    if (cam->cx > (float)MAP_ARRAY_SIZE) cam->cx = (float)MAP_ARRAY_SIZE;
    if (cam->cy < 0.0f) cam->cy = 0.0f;
    if (cam->cy > (float)MAP_ARRAY_SIZE) cam->cy = (float)MAP_ARRAY_SIZE;
    if (cam->zoomIndex < 0) cam->zoomIndex = 0;
    if (cam->zoomIndex >= OVERVIEW_ZOOM_COUNT) {
        cam->zoomIndex = OVERVIEW_ZOOM_COUNT - 1;
    }
}

void overviewCameraInit(OverviewCamera *cam) {
    if (!cam) return;
    cam->cx = (float)MAP_ARRAY_SIZE * 0.5f;
    cam->cy = (float)MAP_ARRAY_SIZE * 0.5f;
    cam->zoomIndex = OVERVIEW_ZOOM_1X;
    cam->follow = true;
    overviewCameraClamp(cam);
}

float overviewCameraZoomScale(const OverviewCamera *cam) {
    if (!cam) return kOverviewZoomSteps[OVERVIEW_ZOOM_1X];
    int i = cam->zoomIndex;
    if (i < 0) i = 0;
    if (i >= OVERVIEW_ZOOM_COUNT) i = OVERVIEW_ZOOM_COUNT - 1;
    return kOverviewZoomSteps[i];
}

int overviewCameraZoomCount(void) {
    return OVERVIEW_ZOOM_COUNT;
}

void overviewCameraWorldToScreen(const OverviewCamera *cam, int viewW, int viewH,
                                 float mapX, float mapY,
                                 float *outSX, float *outSY) {
    if (!cam) return;
    float tilePx = overviewTilePx(cam);
    if (outSX) *outSX = (mapX - cam->cx) * tilePx + (float)viewW * 0.5f;
    if (outSY) *outSY = (mapY - cam->cy) * tilePx + (float)viewH * 0.5f;
}

bool overviewCameraScreenToWorld(const OverviewCamera *cam, int viewW, int viewH,
                                 float sx, float sy, int *outMapX, int *outMapY) {
    if (!cam) return false;
    float tilePx = overviewTilePx(cam);
    float worldX = (sx - (float)viewW * 0.5f) / tilePx + cam->cx;
    float worldY = (sy - (float)viewH * 0.5f) / tilePx + cam->cy;
    int mx = overviewFloorToInt(worldX);
    int my = overviewFloorToInt(worldY);
    /* Written even when off the map, so an edge drag can read the
     * overshoot rather than just being told "outside". */
    if (outMapX) *outMapX = mx;
    if (outMapY) *outMapY = my;
    return mx >= 0 && mx < MAP_ARRAY_SIZE && my >= 0 && my < MAP_ARRAY_SIZE;
}

void overviewCameraZoomAt(OverviewCamera *cam, int viewW, int viewH,
                          float cursorX, float cursorY, int steps) {
    if (!cam) return;

    /* Both sides of the add are brought into range first: a ladder this short
     * saturates on any step of 7 or more either way, and clamping afterwards
     * would leave the sum itself free to overflow. */
    int base = cam->zoomIndex;
    if (base < 0) base = 0;
    if (base >= OVERVIEW_ZOOM_COUNT) base = OVERVIEW_ZOOM_COUNT - 1;
    if (steps < -OVERVIEW_ZOOM_COUNT) steps = -OVERVIEW_ZOOM_COUNT;
    if (steps >  OVERVIEW_ZOOM_COUNT) steps =  OVERVIEW_ZOOM_COUNT;

    int want = base + steps;
    if (want < 0) want = 0;
    if (want >= OVERVIEW_ZOOM_COUNT) want = OVERVIEW_ZOOM_COUNT - 1;

    if (cam->follow) {
        /* Follow re-centres on the tank every frame, so a cursor anchor set
         * here would be undone before it was ever drawn — the point the
         * cursor appeared to hold would move anyway. Change the zoom and
         * leave the centre to follow. A drag clears follow, and the anchor
         * is back in play from then on. */
        cam->zoomIndex = want;
        overviewCameraClamp(cam);
        return;
    }

    float offX = cursorX - (float)viewW * 0.5f;
    float offY = cursorY - (float)viewH * 0.5f;

    float oldTilePx = overviewTilePx(cam);
    float worldX = offX / oldTilePx + cam->cx;
    float worldY = offY / oldTilePx + cam->cy;

    cam->zoomIndex = want;

    /* Solve offX / newTilePx + cx' = worldX for the centre that leaves the
     * cursor's world point exactly where it was. */
    float newTilePx = overviewTilePx(cam);
    cam->cx = worldX - offX / newTilePx;
    cam->cy = worldY - offY / newTilePx;
    overviewCameraClamp(cam);
}

void overviewCameraPan(OverviewCamera *cam, int viewW, int viewH,
                       float dxPixels, float dyPixels) {
    if (!cam) return;
    /* The window size does not enter a relative move — the pixels-to-squares
     * factor is the zoom alone. Taken for symmetry with the rest of the
     * camera calls, which do need it. */
    (void)viewW;
    (void)viewH;
    float tilePx = overviewTilePx(cam);
    cam->cx += dxPixels / tilePx;
    cam->cy += dyPixels / tilePx;
    cam->follow = false;
    overviewCameraClamp(cam);
}

void overviewCameraCenterOnTank(OverviewCamera *cam, int viewW, int viewH,
                                int tankMX, int tankMY) {
    if (!cam) return;
    (void)viewW;
    (void)viewH;
    /* The centre of the tank's square, not its top-left corner. */
    cam->cx = (float)tankMX + 0.5f;
    cam->cy = (float)tankMY + 0.5f;
    cam->follow = true;
    overviewCameraClamp(cam);
}

void overviewCameraFollowTick(OverviewCamera *cam, int viewW, int viewH,
                              int tankMX, int tankMY) {
    if (!cam || !cam->follow) return;
    (void)viewW;
    (void)viewH;
    cam->cx = (float)tankMX + 0.5f;
    cam->cy = (float)tankMY + 0.5f;
    overviewCameraClamp(cam);
}

bool overviewCameraVisibleRange(const OverviewCamera *cam, int viewW, int viewH,
                                int *outLeft, int *outTop,
                                int *outRight, int *outBottom) {
    if (!cam || viewW <= 0 || viewH <= 0) return false;

    float tilePx = overviewTilePx(cam);
    float halfW = (float)viewW / (2.0f * tilePx);
    float halfH = (float)viewH / (2.0f * tilePx);

    /* Square N covers [N, N+1), so it is on screen when N + 1 > the view's
     * near edge and N < its far edge. The first such N is floor(near); the
     * last is ceil(far) - 1, which drops a square whose left edge lands
     * exactly on the far edge and therefore contributes no pixels. */
    int left   = overviewFloorToInt(cam->cx - halfW);
    int top    = overviewFloorToInt(cam->cy - halfH);
    int right  = overviewCeilToInt(cam->cx + halfW) - 1;
    int bottom = overviewCeilToInt(cam->cy + halfH) - 1;

    if (left < 0) left = 0;
    if (top  < 0) top  = 0;
    if (right  > MAP_ARRAY_SIZE - 1) right  = MAP_ARRAY_SIZE - 1;
    if (bottom > MAP_ARRAY_SIZE - 1) bottom = MAP_ARRAY_SIZE - 1;
    if (left > right || top > bottom) return false;

    if (outLeft)   *outLeft   = left;
    if (outTop)    *outTop    = top;
    if (outRight)  *outRight  = right;
    if (outBottom) *outBottom = bottom;
    return true;
}

bool overviewEntityIsVisible(const OverviewMap *om, int mapX, int mapY,
                             bool isSelf) {
    if (mapX < 0 || mapX >= MAP_ARRAY_SIZE ||
        mapY < 0 || mapY >= MAP_ARRAY_SIZE) {
        return false;
    }
    /* Ahead of the memory read on purpose: the player's own tank is drawn
     * even on the frame its square has not been stamped live yet. */
    if (isSelf) return true;
    if (!om) return false;
    return (om->flags[mapX][mapY] & OVERVIEW_F_LIVE) != 0;
}
