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
 * than the 2048 px map pans rather than zooming out further) and 4x the
 * ceiling (64 px squares). The two rungs under 1x are where the view stops
 * drawing sprites and fills each square with its map colour instead. */
static const float kOverviewZoomSteps[] = {
    0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f
};
#define OVERVIEW_ZOOM_COUNT 7
#define OVERVIEW_ZOOM_1X    2 /* index of 1.0f */
#define OVERVIEW_ZOOM_2X    4 /* index of 2.0f — where a fresh camera starts */

static_assert((int)(sizeof(kOverviewZoomSteps) / sizeof(kOverviewZoomSteps[0]))
                  == OVERVIEW_ZOOM_COUNT,
              "kOverviewZoomSteps and OVERVIEW_ZOOM_COUNT disagree");
static_assert(OVERVIEW_ZOOM_1X < OVERVIEW_ZOOM_COUNT,
              "OVERVIEW_ZOOM_1X is off the ladder");
static_assert(OVERVIEW_ZOOM_2X < OVERVIEW_ZOOM_COUNT,
              "OVERVIEW_ZOOM_2X is off the ladder");

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
    cam->zoomIndex = OVERVIEW_ZOOM_2X;
    cam->follow = true;
    cam->scrolling = false;
    cam->scrollFromX = cam->cx;
    cam->scrollFromY = cam->cy;
    cam->scrollToX = cam->cx;
    cam->scrollToY = cam->cy;
    cam->scrollElapsedMs = 0.0f;
    cam->insetL = 0.0f;
    cam->insetT = 0.0f;
    cam->insetR = 0.0f;
    cam->insetB = 0.0f;
    overviewCameraClamp(cam);
}

void overviewCameraSetInsets(OverviewCamera *cam, float left, float top,
                             float right, float bottom) {
    if (!cam) return;
    /* A negative inset would push a point off the edge it is meant to be kept
     * inside, and a host that has no panel on an edge passes zero for it. */
    cam->insetL = (left   > 0.0f) ? left   : 0.0f;
    cam->insetT = (top    > 0.0f) ? top    : 0.0f;
    cam->insetR = (right  > 0.0f) ? right  : 0.0f;
    cam->insetB = (bottom > 0.0f) ? bottom : 0.0f;
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

void overviewCameraSetZoomScale(OverviewCamera *cam, float scale) {
    if (!cam) return;
    /* Nearest rung by absolute difference. The ladder runs 0.5x to 4x, so a
     * scale outside it is nearest to whichever end it passed and the clamp
     * falls out of the same walk — nothing below or above needs its own
     * case. */
    int best = 0;
    float bestDelta = fabsf(kOverviewZoomSteps[0] - scale);
    for (int i = 1; i < OVERVIEW_ZOOM_COUNT; i++) {
        float delta = fabsf(kOverviewZoomSteps[i] - scale);
        if (delta < bestDelta) {
            bestDelta = delta;
            best = i;
        }
    }
    cam->zoomIndex = best;
    overviewCameraClamp(cam);
}

/* View pixel of map square 0,0's top-left corner, snapped to a whole pixel.
 * The camera is continuous, so the corner can land on a fraction; rounding it
 * once here, and adding whole tiles to it everywhere else, is what keeps every
 * consumer on the same grid. Rounding per square would not: neighbours can
 * fall on opposite sides of a half-pixel and either leave a gap or overlap by
 * one, and a sprite placed from the unrounded corner can straddle two snapped
 * squares. tilePx is a whole number at every rung and mapX * tilePx is at most
 * 16384, so origin + mapX * tilePx is exact in float and squares abut. */
static void overviewOriginPx(const OverviewCamera *cam, int viewW, int viewH,
                             float *outOX, float *outOY) {
    float tilePx = overviewTilePx(cam);
    *outOX = roundf(-cam->cx * tilePx + (float)viewW * 0.5f);
    *outOY = roundf(-cam->cy * tilePx + (float)viewH * 0.5f);
}

void overviewCameraWorldToScreen(const OverviewCamera *cam, int viewW, int viewH,
                                 float mapX, float mapY,
                                 float *outSX, float *outSY) {
    if (!cam) return;
    float tilePx = overviewTilePx(cam);
    float ox = 0.0f, oy = 0.0f;
    overviewOriginPx(cam, viewW, viewH, &ox, &oy);
    if (outSX) *outSX = ox + mapX * tilePx;
    if (outSY) *outSY = oy + mapY * tilePx;
}

bool overviewCameraScreenToWorld(const OverviewCamera *cam, int viewW, int viewH,
                                 float sx, float sy, int *outMapX, int *outMapY) {
    if (!cam) return false;
    /* The inverse of overviewCameraWorldToScreen, from the same snapped
     * origin, so the square under a pixel is the square drawn there. */
    float tilePx = overviewTilePx(cam);
    float ox = 0.0f, oy = 0.0f;
    overviewOriginPx(cam, viewW, viewH, &ox, &oy);
    float worldX = (sx - ox) / tilePx;
    float worldY = (sy - oy) / tilePx;
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

    /* A scroll's target centre was worked out for the old rung and means
     * something else after a rung change, so the rezoom takes the camera. */
    overviewCameraScrollCancel(cam);

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
    /* A hand on the camera takes it over, the same way a pan clears follow. */
    overviewCameraScrollCancel(cam);
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
                                float tankMapX, float tankMapY) {
    if (!cam) return;
    /* This jumps and claims follow; a scroll still running would pull the
     * centre straight back off the tank. */
    overviewCameraScrollCancel(cam);
    (void)viewW;
    (void)viewH;
    cam->cx = tankMapX;
    cam->cy = tankMapY;
    cam->follow = true;
    overviewCameraClamp(cam);
}

void overviewCameraFollowTick(OverviewCamera *cam, int viewW, int viewH,
                              float tankMapX, float tankMapY) {
    if (!cam || !cam->follow) return;
    /* No scroll cancel here, unlike the calls a player drives: follow and a
     * scroll are meant to run together, with the caller holding this tick off
     * while the scroll finishes. */
    (void)viewW;
    (void)viewH;
    cam->cx = tankMapX;
    cam->cy = tankMapY;
    overviewCameraClamp(cam);
}

bool overviewCameraCentreToShow(const OverviewCamera *cam, int viewW, int viewH,
                                float pointX, float pointY,
                                float *outCx, float *outCy) {
    if (!cam) return false;

    /* A view with no pixels has no inside to bring anything into, so the
     * answer is the centre the camera is already on and no move to make. */
    float wantX = cam->cx;
    float wantY = cam->cy;
    if (viewW <= 0 || viewH <= 0) {
        if (outCx) *outCx = wantX;
        if (outCy) *outCy = wantY;
        return false;
    }

    float tilePx = overviewTilePx(cam);

    /* The band of view pixels the point is allowed to land in: the view, less
     * whatever the panels cover, less the margin off each of those edges. A
     * point at pixel p sits at world pointX when the centre is
     * pointX + (view/2 - p) / tilePx, so a band of pixels is a band of
     * centres — the near pixel edge gives the far centre, which is why the two
     * come out swapped. */
    float nearX = cam->insetL + OVERVIEW_EDGE_MARGIN * tilePx;
    float farX  = (float)viewW - cam->insetR - OVERVIEW_EDGE_MARGIN * tilePx;
    float nearY = cam->insetT + OVERVIEW_EDGE_MARGIN * tilePx;
    float farY  = (float)viewH - cam->insetB - OVERVIEW_EDGE_MARGIN * tilePx;

    float midW = (float)viewW * 0.5f;
    float midH = (float)viewH * 0.5f;

    /* The two edges of an axis ask for opposite things. A band with nothing
     * left in it — the panels and the margins between them covering the view —
     * has no centre that satisfies either edge, so the point goes in the middle
     * of what the panels left: the nearest thing to what was asked, and the
     * only answer that does not favour one edge. */
    if (nearX >= farX) {
        wantX = pointX + (midW - (nearX + farX) * 0.5f) / tilePx;
    } else {
        float lowest  = pointX + (midW - farX) / tilePx;
        float highest = pointX + (midW - nearX) / tilePx;
        if (wantX < lowest)  wantX = lowest;
        if (wantX > highest) wantX = highest;
    }

    if (nearY >= farY) {
        wantY = pointY + (midH - (nearY + farY) * 0.5f) / tilePx;
    } else {
        float lowest  = pointY + (midH - farY) / tilePx;
        float highest = pointY + (midH - nearY) / tilePx;
        if (wantY < lowest)  wantY = lowest;
        if (wantY > highest) wantY = highest;
    }

    /* Put the answer through the clamp on a copy, so the caller is handed a
     * centre the camera would accept and the camera itself is untouched. The
     * clamp can only pull the centre back towards the map, and the point is on
     * the map, so it cannot undo what was just worked out. */
    OverviewCamera probe = *cam;
    probe.cx = wantX;
    probe.cy = wantY;
    overviewCameraClamp(&probe);

    if (outCx) *outCx = probe.cx;
    if (outCy) *outCy = probe.cy;
    return probe.cx != cam->cx || probe.cy != cam->cy;
}

void overviewCameraKeepOnScreen(OverviewCamera *cam, int viewW, int viewH,
                                float pointX, float pointY) {
    if (!cam || viewW <= 0 || viewH <= 0) return;

    float wantX = cam->cx;
    float wantY = cam->cy;
    overviewCameraCentreToShow(cam, viewW, viewH, pointX, pointY,
                               &wantX, &wantY);
    cam->cx = wantX;
    cam->cy = wantY;
    overviewCameraClamp(cam);
}

bool overviewCameraScrollTo(OverviewCamera *cam, int viewW, int viewH,
                            float targetCx, float targetCy) {
    if (!cam || viewW <= 0 || viewH <= 0) return false;

    /* Through the clamp on a copy, the way overviewCameraCentreToShow does it,
     * so the scroll is aimed at a centre the camera would accept rather than
     * at one the landing tick would pull back off. */
    OverviewCamera probe = *cam;
    probe.cx = targetCx;
    probe.cy = targetCy;
    overviewCameraClamp(&probe);

    if (probe.cx == cam->cx && probe.cy == cam->cy) {
        /* Already on the centre being asked for. Anything in flight was aimed
         * elsewhere, so it stops here rather than carrying on to a centre
         * nobody is asking for any more. */
        overviewCameraScrollCancel(cam);
        return false;
    }

    /* The start is wherever the centre is now, which mid-scroll is the
     * animated position rather than where the last scroll began. That is what
     * makes a re-aim carry on instead of jumping back. */
    cam->scrollFromX = cam->cx;
    cam->scrollFromY = cam->cy;
    cam->scrollToX = probe.cx;
    cam->scrollToY = probe.cy;
    cam->scrollElapsedMs = 0.0f;
    cam->scrolling = true;
    return true;
}

bool overviewCameraScrollToShow(OverviewCamera *cam, int viewW, int viewH,
                                float pointX, float pointY) {
    /* Ahead of the centre work, so a degenerate view is turned away without
     * touching a scroll in flight. */
    if (!cam || viewW <= 0 || viewH <= 0) return false;

    float toX = cam->cx;
    float toY = cam->cy;
    if (!overviewCameraCentreToShow(cam, viewW, viewH, pointX, pointY,
                                    &toX, &toY)) {
        /* Already on screen with room to spare. Anything in flight was aimed
         * elsewhere, so it stops here rather than carrying on to a centre
         * nobody is asking for any more. */
        overviewCameraScrollCancel(cam);
        return false;
    }

    /* The least move, handed to the same scroll a centring uses. That centre
     * is already clamped and already differs from the one the camera is on,
     * so the call below cannot answer false here. */
    return overviewCameraScrollTo(cam, viewW, viewH, toX, toY);
}

bool overviewCameraScrollTick(OverviewCamera *cam, float dtMs) {
    if (!cam || !cam->scrolling) return false;

    /* A frame that took no time still belongs to the scroll; it just does not
     * move it on. */
    if (dtMs > 0.0f) cam->scrollElapsedMs += dtMs;
    if (cam->scrollElapsedMs > OVERVIEW_SCROLL_MS) {
        cam->scrollElapsedMs = OVERVIEW_SCROLL_MS;
    }

    if (cam->scrollElapsedMs >= OVERVIEW_SCROLL_MS) {
        /* The end is assigned rather than interpolated: smoothstep at t = 1 is
         * 1 exactly on paper, but the multiplies need not land there, and the
         * camera has to finish on the centre it was aimed at. */
        cam->cx = cam->scrollToX;
        cam->cy = cam->scrollToY;
        cam->scrolling = false;
    } else {
        /* Smoothstep: starts and ends at rest, and never leaves 0..1, so the
         * centre cannot run past the target and come back. */
        float t = cam->scrollElapsedMs / OVERVIEW_SCROLL_MS;
        float f = t * t * (3.0f - 2.0f * t);
        cam->cx = cam->scrollFromX + (cam->scrollToX - cam->scrollFromX) * f;
        cam->cy = cam->scrollFromY + (cam->scrollToY - cam->scrollFromY) * f;
    }

    /* Both ends are clamped centres and the path between them is a weighted
     * average of the two, so this has nothing to do on a scroll started from a
     * camera in range. It is here to keep every centre move ending in the same
     * place. */
    overviewCameraClamp(cam);

    /* True for the tick that finished it as well, so the frame that lands the
     * camera is still the scroll's and not the caller's. */
    return true;
}

bool overviewCameraIsScrolling(const OverviewCamera *cam) {
    if (!cam) return false;
    return cam->scrolling;
}

void overviewCameraScrollCancel(OverviewCamera *cam) {
    if (!cam) return;
    /* The centre is left where the animation reached. */
    cam->scrolling = false;
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

bool overviewEntityIsVisible(const OverviewMap *om, int mapX, int mapY) {
    if (mapX < 0 || mapX >= MAP_ARRAY_SIZE ||
        mapY < 0 || mapY >= MAP_ARRAY_SIZE) {
        return false;
    }
    if (!om) return false;
    return (om->flags[mapX][mapY] & OVERVIEW_F_SIGHT) != 0;
}
