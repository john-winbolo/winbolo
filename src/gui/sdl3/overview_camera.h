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
 * Name:          overview_camera.h
 * Purpose:       Camera state and coordinate maths for the
 *                map overview: the zoom ladder, world <->
 *                screen conversion, cursor-anchored zoom,
 *                pan, follow-the-tank centring and the
 *                range of squares a view covers.
 *
 *                Plus the rule for which squares an entity
 *                may be drawn on, which is arithmetic over
 *                the same state and belongs with it.
 *
 *                Pure state in, numbers out — no ImGui and
 *                no SDL_Renderer, so it compiles into the
 *                unit-test binary (which links neither) and
 *                the drawing code is a thin consumer.
 *********************************************************/

#ifndef OVERVIEW_CAMERA_H
#define OVERVIEW_CAMERA_H

#include <stdbool.h>

/* windows.h — reached through WinSock2 — has to be seen at the default
 * packing: types.h below opens a #pragma pack(push, 4) and includes global.h
 * from inside it, and winnt.h static-asserts that the pack is default. The
 * rest of the client gets this for free by including SDL3 ahead of the bolo
 * headers; the camera has no SDL dependency to lean on. */
#ifdef _WIN32
#include <WinSock2.h>
#endif

#include "types.h"           /* MAP_ARRAY_SIZE */
#include "overview_types.h"  /* OverviewMap, OVERVIEW_F_LIVE */
#include "../tiles.h"        /* TILE_SIZE_X */

/* Screen pixels one map square occupies at 1x zoom. A square is drawn from
 * one 16x16 cell of the sprite sheet, so this is the sheet's cell size. */
#define OVERVIEW_TILE_PX TILE_SIZE_X

#ifdef __cplusplus
extern "C" {
#endif

/* Coordinate model
 * ----------------
 * Map square N spans [N, N+1) in world units, so the centre of square N is
 * N + 0.5. `cx`/`cy` are continuous map-square units — a fractional part is
 * a position inside a square, not a rounding error — and name the world
 * point the view is centred on.
 *
 * View pixels are measured from the top-left of the view, so the view centre
 * sits at (viewW * 0.5f, viewH * 0.5f). A pixel offset from that centre
 * converts to world units as
 *
 *     worldDelta = pixelDelta / (OVERVIEW_TILE_PX * zoomScale)
 */
typedef struct OverviewCamera {
    float cx, cy;    /* view centre, in map-square units (see above) */
    int   zoomIndex; /* index into the zoom ladder */
    bool  follow;    /* centre tracks the tank each frame */
} OverviewCamera;

/* 1x zoom, follow on, centred on the map. */
void overviewCameraInit(OverviewCamera *cam);

/* The ladder entry `zoomIndex` selects: 0.5x at the floor, 4x at the
 * ceiling, 1x in the middle. */
float overviewCameraZoomScale(const OverviewCamera *cam);

/* Number of ladder positions; valid zoomIndex values are 0..count-1. */
int overviewCameraZoomCount(void);

/* Snap the zoom to the ladder rung nearest `scale`, clamped to the ladder's
 * ends. Preferences store the zoom as a scale rather than a ladder index, so
 * a saved value keeps its meaning if the ladder ever gains a rung. */
void overviewCameraSetZoomScale(OverviewCamera *cam, float scale);

/* Top-left corner of map square (mapX,mapY) in view pixels. Fractional
 * coordinates are meaningful: pass mapX + 0.5f for the square's centre. */
void overviewCameraWorldToScreen(const OverviewCamera *cam, int viewW, int viewH,
                                 float mapX, float mapY, float *outSX, float *outSY);

/* The map square under a view pixel. Returns false when it falls outside
 * 0..MAP_ARRAY_SIZE-1, and still writes the out-of-range square so the
 * caller can see how far outside the map the pixel landed. */
bool overviewCameraScreenToWorld(const OverviewCamera *cam, int viewW, int viewH,
                                 float sx, float sy, int *outMapX, int *outMapY);

/* Step the zoom by `steps` ladder positions, keeping the world point under
 * (cursorX,cursorY) under it. Clamped to the ladder's ends. In follow mode
 * the cursor anchor is ignored — see the note at the implementation. */
void overviewCameraZoomAt(OverviewCamera *cam, int viewW, int viewH,
                          float cursorX, float cursorY, int steps);

/* Move the centre by a view-pixel delta and clear follow. A positive
 * dxPixels moves the centre right, so a drag handler passes the negated
 * mouse delta (dragging right pulls the map right, i.e. the centre left). */
void overviewCameraPan(OverviewCamera *cam, int viewW, int viewH,
                       float dxPixels, float dyPixels);

/* Centre on the tank's square and turn follow on. */
void overviewCameraCenterOnTank(OverviewCamera *cam, int viewW, int viewH,
                                int tankMX, int tankMY);

/* Re-centre on the tank without touching the flag. Does nothing unless
 * follow is on, so a caller can run it every frame unconditionally. */
void overviewCameraFollowTick(OverviewCamera *cam, int viewW, int viewH,
                              int tankMX, int tankMY);

/* Inclusive square range intersecting the view, clamped to
 * 0..MAP_ARRAY_SIZE-1. Returns false when no square is visible. */
bool overviewCameraVisibleRange(const OverviewCamera *cam, int viewW, int viewH,
                                int *outLeft, int *outTop,
                                int *outRight, int *outBottom);

/* Whether an entity standing on (mapX,mapY) may be drawn. Tanks, men and
 * shells appear only on squares the player can see this instant, so an enemy
 * the client has been told about in a corner it has walked away from stays
 * off the picture — the memory behind a frozen square is terrain, and nothing
 * that moves belongs on it. isSelf is the one exception: the local player's
 * own tank is drawn wherever it is. Off-map squares are never drawn, and a
 * NULL memory shows nothing but the player's own tank. */
bool overviewEntityIsVisible(const OverviewMap *om, int mapX, int mapY,
                             bool isSelf);

#ifdef __cplusplus
}
#endif

#endif /* OVERVIEW_CAMERA_H */
