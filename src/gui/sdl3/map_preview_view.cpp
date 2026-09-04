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
 * Name:          map_preview_view.cpp
 * Purpose:       Implementation of the reusable map preview
 *                widget — see map_preview_view.h. Lifted
 *                wholesale from the original
 *                map_preview_popup.cpp (whose file-static
 *                state is now folded into a per-instance
 *                struct so two views can coexist — e.g.
 *                the lobby inline preview and the modal
 *                popup).
 *********************************************************/

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <SDL3/SDL.h>

#include "imgui.h"

extern "C" {
#include "global.h"
#include "client_mappreview.h"
#include "screencalc.h"
#include "tilenum.h"
#include "../tiles.h"
#include "map_preview_view.h"
#include "macos_pinch.h"
#include "sprite_positions.h"
#include "minimap_render.h"

/* From tileloader.h */
extern SDL_Surface *tileLoaderBuildSheet(int tileSize);
/* From sdl3draw.h */
extern unsigned int sdl3DrawGetTilesGeneration(void);
}

/* Zoom steps. The four sub-0.5 entries put the widget into "minimap
 * mode" where each tile is painted as a single coloured rect instead
 * of a full sprite — at 0.0625× the whole 256-tile map fits in 256
 * pixels (1 pixel per tile). 0.5×+ uses the existing tile-sprite
 * renderer. */
/* Discrete zoom ladder. Sub-1x steps map to specific tile pixel sizes
 * after ImGui's display-time downscale of the 1x sprite render:
 *   0.75x → ~12 px/tile (sprite mode)
 *   0.50x → ~ 8 px/tile (sprite mode)
 *   0.20x → minimap-colour mode (sprites at that scale are noise)
 *   0.10x → minimap-colour mode
 * Above 1x is integer multiples so pixel art stays crisp. */
static const float kZoomSteps[] = {
    0.1f, 0.2f, 0.5f, 0.75f,
    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
    9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f
};
#define ZOOM_STEP_COUNT 20
#define ZOOM_STEP_1X    4     /* index of 1.0f */
#define ZOOM_MINIMAP_MAX 0.33f /* < this: minimap-colour mode (catches 0.2, 0.1) */

/* Boat sprite atlas coords for start position overlays. */
static const int kBoatAtlasX[16] = {
    TANK_SELFBOAT_0_X,  TANK_SELFBOAT_1_X,  TANK_SELFBOAT_2_X,  TANK_SELFBOAT_3_X,
    TANK_SELFBOAT_4_X,  TANK_SELFBOAT_5_X,  TANK_SELFBOAT_6_X,  TANK_SELFBOAT_7_X,
    TANK_SELFBOAT_8_X,  TANK_SELFBOAT_9_X,  TANK_SELFBOAT_10_X, TANK_SELFBOAT_11_X,
    TANK_SELFBOAT_12_X, TANK_SELFBOAT_13_X, TANK_SELFBOAT_14_X, TANK_SELFBOAT_15_X
};
static const int kBoatAtlasY[16] = {
    TANK_SELFBOAT_0_Y,  TANK_SELFBOAT_1_Y,  TANK_SELFBOAT_2_Y,  TANK_SELFBOAT_3_Y,
    TANK_SELFBOAT_4_Y,  TANK_SELFBOAT_5_Y,  TANK_SELFBOAT_6_Y,  TANK_SELFBOAT_7_Y,
    TANK_SELFBOAT_8_Y,  TANK_SELFBOAT_9_Y,  TANK_SELFBOAT_10_Y, TANK_SELFBOAT_11_Y,
    TANK_SELFBOAT_12_Y, TANK_SELFBOAT_13_Y, TANK_SELFBOAT_14_Y, TANK_SELFBOAT_15_Y
};
/* Ally (green) and enemy (red) boat variants — selected per start ownership
 * so claimed starts render in the in-game allegiance colours. */
static const int kGoodBoatAtlasX[16] = {
    TANK_GOODBOAT_0_X,  TANK_GOODBOAT_1_X,  TANK_GOODBOAT_2_X,  TANK_GOODBOAT_3_X,
    TANK_GOODBOAT_4_X,  TANK_GOODBOAT_5_X,  TANK_GOODBOAT_6_X,  TANK_GOODBOAT_7_X,
    TANK_GOODBOAT_8_X,  TANK_GOODBOAT_9_X,  TANK_GOODBOAT_10_X, TANK_GOODBOAT_11_X,
    TANK_GOODBOAT_12_X, TANK_GOODBOAT_13_X, TANK_GOODBOAT_14_X, TANK_GOODBOAT_15_X
};
static const int kGoodBoatAtlasY[16] = {
    TANK_GOODBOAT_0_Y,  TANK_GOODBOAT_1_Y,  TANK_GOODBOAT_2_Y,  TANK_GOODBOAT_3_Y,
    TANK_GOODBOAT_4_Y,  TANK_GOODBOAT_5_Y,  TANK_GOODBOAT_6_Y,  TANK_GOODBOAT_7_Y,
    TANK_GOODBOAT_8_Y,  TANK_GOODBOAT_9_Y,  TANK_GOODBOAT_10_Y, TANK_GOODBOAT_11_Y,
    TANK_GOODBOAT_12_Y, TANK_GOODBOAT_13_Y, TANK_GOODBOAT_14_Y, TANK_GOODBOAT_15_Y
};
static const int kEvilBoatAtlasX[16] = {
    TANK_EVILBOAT_0_X,  TANK_EVILBOAT_1_X,  TANK_EVILBOAT_2_X,  TANK_EVILBOAT_3_X,
    TANK_EVILBOAT_4_X,  TANK_EVILBOAT_5_X,  TANK_EVILBOAT_6_X,  TANK_EVILBOAT_7_X,
    TANK_EVILBOAT_8_X,  TANK_EVILBOAT_9_X,  TANK_EVILBOAT_10_X, TANK_EVILBOAT_11_X,
    TANK_EVILBOAT_12_X, TANK_EVILBOAT_13_X, TANK_EVILBOAT_14_X, TANK_EVILBOAT_15_X
};
static const int kEvilBoatAtlasY[16] = {
    TANK_EVILBOAT_0_Y,  TANK_EVILBOAT_1_Y,  TANK_EVILBOAT_2_Y,  TANK_EVILBOAT_3_Y,
    TANK_EVILBOAT_4_Y,  TANK_EVILBOAT_5_Y,  TANK_EVILBOAT_6_Y,  TANK_EVILBOAT_7_Y,
    TANK_EVILBOAT_8_Y,  TANK_EVILBOAT_9_Y,  TANK_EVILBOAT_10_Y, TANK_EVILBOAT_11_Y,
    TANK_EVILBOAT_12_Y, TANK_EVILBOAT_13_Y, TANK_EVILBOAT_14_Y, TANK_EVILBOAT_15_Y
};

struct MapPreviewView {
    /* Source — either compressed buffer or a file path. The latter
     * wins on parse if both are set (callers pick one). */
    BYTE  *compressedData;
    int    compressedLen;
    char  *filePath;

    /* Parsed map data — built lazily inside RenderOffscreen so the
     * caller can Load* outside a render frame cheaply. */
    MapPreview *preview;
    bool        dataLoaded;

    /* Tile atlas — shared per-widget; rebuilt at the requested tile
     * size on first render, and again whenever a skin change bumps the
     * atlas generation under us. */
    SDL_Texture *tilesTex;
    unsigned int tilesGeneration;

    /* Offscreen render target. Sized at RenderOffscreen time to match
     * the requested viewport (oversampled when zoom < 1 so the
     * destination Image can downscale for crisp output). */
    /* Double-buffered: one is being rendered into while the other is the one
     * the caller was handed to draw. Rendering into the texture ImGui is
     * about to sample means the GPU can be reading it while SDL writes it,
     * which shows up in motion as ghosting between the old and new camera —
     * exactly what the lobby's own preview rebuild avoids by building the new
     * texture before releasing the old ("Build the NEW texture before
     * destroying the OLD one", imgui_lobby.cpp). Same shape, per frame:
     * render into the back buffer, publish it, flip. */
    SDL_Texture *offscreen[2];
    int          offscreenBack;   /* index currently rendered into */
    int          offscreenW;
    int          offscreenH;

    /* Intermediate scratch target used in sub-1x sprite mode for one
     * manual 2:1 bilinear downsample pass — chained with the final
     * ImGui Image-time downsample, this gives ~16-sample averaging
     * per output pixel vs the 4-sample bilinear of a single pass. */
    SDL_Texture *scratch;
    int          scratchW;
    int          scratchH;

    /* Pre-rendered map atlas: every visible terrain + mine overlay
     * tile of the parsed map, laid out at native 16 px/tile into
     * one big texture. Built once when the map data parses (or when
     * a Load* invalidates it), then any visible-region blit is a
     * single SDL_RenderTexture(atlas, srcSubRect, dst) instead of
     * the previous ~hundreds of per-tile blits. NEAREST scale mode
     * so zoom>=1 upscale stays crisp.
     *
     * Size = (256 tiles × 16 px) per side = 4096 × 4096 RGBA8 →
     * 64 MB per view. Acceptable for the lobby + popup since only
     * one or two MapPreviewViews are alive at once. */
    SDL_Texture *mapAtlas;
    int          mapAtlasTiles;        /* edge length in tiles (always 256) */
    int          mapAtlasTilePx;       /* pixels per tile (16 = native) */

    /* Pre-downscaled map atlas, used by the sub-1x sprite path. Same
     * 4096×4096 RGBA8 backing texture as mapAtlas — allocated ONCE
     * when the map parses and reused thereafter. On each zoom change
     * we LINEAR-downscale mapAtlas into the (0,0,scaledEdge,scaledEdge)
     * sub-rect of this texture; subsequent frames blit a visible
     * sub-rect 1:1 to the offscreen. The big win vs the old
     * "oversample → bilinear downscale at view time" path is pan
     * stability: the bilinear pattern is baked in once per zoom, so
     * every output pixel always reads the same source pixel and the
     * map doesn't shimmer / drift as the camera moves. */
    SDL_Texture *mapAtlasScaled;
    int          mapAtlasScaledEdge;   /* pixels per side baked at scaledZoom */
    float        mapAtlasScaledZoom;   /* zoom level currently baked in, or 0 */
    /* Which texture should ImGui display (offscreen for non-sub-1x,
     * scratch for sub-1x sprite mode). */
    SDL_Texture *displayTex;

    /* Viewport size cache so we can detect resizes between calls. */
    int          lastViewW;
    int          lastViewH;

    /* View state. */
    int   zoomIndex;
    float zoomLevel;
    WORLD centerX;
    WORLD centerY;

    /* Auto-fit happens the first frame after data parses. */
    bool autoFitDone;

    /* macOS pinch accumulator. */
    float pinchAccum;

    /* Sub-unit remainder carried between pan steps — see viewPanBy. */
    float panResidX;
    float panResidY;

    /* Pan-drag ownership, latched at the press. panDragChecked marks that
     * this press has already been adjudicated, so the decision is made once
     * rather than re-made every frame. See mapPreviewViewHandleInput. */
    bool panDragActive;
    bool panDragChecked;

    /* Transform state snapshot — captured each time viewRenderStarts runs,
     * so world<->screen inverts the same math the starts were drawn with.
     * Valid only in sprite mode (the minimap path doesn't draw starts). */
    bool   startsTransformValid;
    int    startsScreenW;
    int    startsScreenH;
    float  startsTileScale;
    WORLD  startsCenterX;
    WORLD  startsCenterY;

    /* Per-start ownership for colouring (0-based, start index i+1):
     * 0=unclaimed, 1=self, 2=ally, 3=enemy. Set per-frame by the lobby via
     * mapPreviewViewSetStartOwners; selects the boat sprite (sprite zoom)
     * and the dot colour (minimap zoom). */
    uint8_t startOwners[16];   /* MAX_STARTS */
    int     startOwnerCount;
};

/* Ownership code (0-based start index) -> boat atlas table + alpha. Self uses
 * the normal (black) self boat; ally the green boat; enemy the red boat;
 * an unclaimed start uses the self boat at 50% alpha so it reads as "not yet
 * taken". When there's no ownership info (e.g. the map chooser), everything
 * is the opaque self boat. */
static void boatStyleForOwner(const MapPreviewView *v, int startIdx1,
                              const int **outX, const int **outY, Uint8 *outA) {
    *outX = kBoatAtlasX; *outY = kBoatAtlasY;
    *outA = 255;
    int k = startIdx1 - 1;
    if (k < 0 || k >= v->startOwnerCount) return;   /* no info -> opaque self */
    switch (v->startOwners[k]) {
        case 1: break;                                                  /* self  black */
        case 2: *outX = kGoodBoatAtlasX; *outY = kGoodBoatAtlasY; break; /* ally  green */
        case 3: *outX = kEvilBoatAtlasX; *outY = kEvilBoatAtlasY; break; /* enemy red */
        default: *outA = 128; break;                  /* unclaimed -> 50% transparent */
    }
}

/* WORLD units per map square (1 << TANK_SHIFT_MAPSIZE). */
#define VIEW_WORLD_PER_TILE 256.0f

/* Camera centre in atlas pixels (tileSize px per map square), keeping the
 * sub-pixel fraction. This was ((int)centre * tileSize) >> 8, which snapped
 * the camera onto whole atlas pixels — one atlas pixel is `zoom` screen
 * pixels, so at 16x the view could only move in 16-pixel jumps and panning
 * read as stepped. The minimap path (viewRenderMinimapToOffscreen) already
 * did this in float; this brings the sprite path in line. */
static inline float viewCamPixelsF(WORLD centre, int tileSize) {
    return (float)centre * (float)tileSize / VIEW_WORLD_PER_TILE;
}

/* Move the camera by a fractional number of WORLD units, carrying the
 * remainder into the next call. Mouse deltas convert to fractional units and
 * the old code truncated each frame's step, so a slow drag lost every
 * sub-unit move and a fast one fell progressively behind the cursor. The
 * remainder is dropped at the clamps so it can't build up an invisible debt
 * the user has to drag back out. */
static void viewPanBy(MapPreviewView *v, float dxWorld, float dyWorld) {
    float wantX = (float)v->centerX + dxWorld + v->panResidX;
    float wantY = (float)v->centerY + dyWorld + v->panResidY;
    float newX  = floorf(wantX);
    float newY  = floorf(wantY);
    v->panResidX = wantX - newX;
    v->panResidY = wantY - newY;
    if (newX < 0.0f)     { newX = 0.0f;     v->panResidX = 0.0f; }
    if (newX > 65280.0f) { newX = 65280.0f; v->panResidX = 0.0f; }
    if (newY < 0.0f)     { newY = 0.0f;     v->panResidY = 0.0f; }
    if (newY > 65280.0f) { newY = 65280.0f; v->panResidY = 0.0f; }
    v->centerX = (WORLD)newX;
    v->centerY = (WORLD)newY;
}

/* WORLD units spanned by one displayed pixel at the given zoom — one map
 * square is TILE_SIZE_X * zoom pixels on screen and 256 WORLD units. */
static inline float viewPixelsToWorld(float zoom) {
    if (zoom <= 0.0f) zoom = 1.0f;
    return VIEW_WORLD_PER_TILE / ((float)TILE_SIZE_X * zoom);
}

/* Keep the map point under the cursor pinned across a zoom step, so zooming
 * reads as moving toward what you are looking at rather than re-centring.
 *
 * The item rect is the pan InvisibleButton that every caller submits
 * immediately before mapPreviewViewHandleInput (see the header note), so it
 * is this frame's displayed map rect. If a caller ever breaks that ordering
 * the offsets go to zero and zoom falls back to centre-anchored. */
static void viewZoomAnchorAtCursor(MapPreviewView *v, float oldZoom) {
    ImVec2 rectMin  = ImGui::GetItemRectMin();
    ImVec2 rectSize = ImGui::GetItemRectSize();
    if (rectSize.x < 1.0f || rectSize.y < 1.0f) return;
    ImVec2 mp = ImGui::GetMousePos();
    float offX = mp.x - (rectMin.x + rectSize.x * 0.5f);
    float offY = mp.y - (rectMin.y + rectSize.y * 0.5f);
    /* The cursor sits at centre + off * pixelsToWorld(zoom); solving for the
     * centre that keeps that product constant gives the delta below. */
    float delta = viewPixelsToWorld(oldZoom) - viewPixelsToWorld(v->zoomLevel);
    viewPanBy(v, offX * delta, offY * delta);
}

/* ── Adjacency-aware tile calculation (lifted verbatim) ──────────── */

static BYTE viewNeighbour(MapPreviewView *v, BYTE nx, BYTE ny) {
    if (clientMapPreviewIsBase(v->preview, nx, ny)) return ROAD;
    BYTE t = clientMapPreviewGetTerrain(v->preview, nx, ny);
    if (t >= MINE_START && t <= MINE_END) return (BYTE)(t - MINE_SUBTRACT);
    return t;
}

static BYTE viewCalcTile(MapPreviewView *v, BYTE xValue, BYTE yValue) {
    if (clientMapPreviewIsPill(v->preview, xValue, yValue)) {
        static const BYTE pillTileForArmour[16] = {
            PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
            PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
            PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
            PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
        };
        BYTE armour = clientMapPreviewGetPillArmourAt(v->preview, xValue, yValue);
        if (armour <= 15) return pillTileForArmour[armour];
        return PILL_EVIL_15;
    }
    if (clientMapPreviewIsBase(v->preview, xValue, yValue)) return BASE_NEUTRAL;
    if (clientMapPreviewIsStart(v->preview, xValue, yValue)) return DEEP_SEA_SOLID;

    BYTE currentPos = clientMapPreviewGetTerrain(v->preview, xValue, yValue);
    if (currentPos >= MINE_START && currentPos <= MINE_END)
        currentPos = (BYTE)(currentPos - MINE_SUBTRACT);

    BYTE aboveLeft  = viewNeighbour(v, (BYTE)(xValue-1), (BYTE)(yValue-1));
    BYTE above      = viewNeighbour(v, xValue,            (BYTE)(yValue-1));
    BYTE aboveRight = viewNeighbour(v, (BYTE)(xValue+1), (BYTE)(yValue-1));
    BYTE leftPos    = viewNeighbour(v, (BYTE)(xValue-1), yValue);
    BYTE rightPos   = viewNeighbour(v, (BYTE)(xValue+1), yValue);
    BYTE belowLeft  = viewNeighbour(v, (BYTE)(xValue-1), (BYTE)(yValue+1));
    BYTE below      = viewNeighbour(v, xValue,            (BYTE)(yValue+1));
    BYTE belowRight = viewNeighbour(v, (BYTE)(xValue+1), (BYTE)(yValue+1));

    switch (currentPos) {
    case ROAD:     return screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case BUILDING: return screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case FOREST:   return screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case RIVER:    return screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case DEEP_SEA: return screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case BOAT:     return screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case CRATER:   return screenCalcCrater(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    default:       return currentPos;
    }
}

/* ── Rendering ───────────────────────────────────────────────────── */

static void viewRenderStarts(MapPreviewView *v, SDL_Renderer *renderer,
                             int screenW, int screenH, float tileScale) {
    int tileSize = TILE_SIZE_X;
    float scaledTileF = (float)tileSize * tileScale;
    if (scaledTileF < 1.0f) scaledTileF = 1.0f;

    float centerPXf = viewCamPixelsF(v->centerX, tileSize);
    float centerPYf = viewCamPixelsF(v->centerY, tileSize);
    float halfX = (float)screenW / (2.0f * tileScale);
    float halfY = (float)screenH / (2.0f * tileScale);
    float camPXf = centerPXf - halfX;
    float camPYf = centerPYf - halfY;

    /* Snapshot the exact transform inputs so world<->screen helpers
     * invert this frame's math even if pan/zoom changes before they run. */
    v->startsScreenW = screenW;
    v->startsScreenH = screenH;
    v->startsTileScale = tileScale;
    v->startsCenterX = v->centerX;
    v->startsCenterY = v->centerY;
    v->startsTransformValid = true;

    BYTE numStarts = clientMapPreviewGetStartCount(v->preview);
    for (BYTE i = 1; i <= numStarts; i++) {
        BYTE sx, sy, sdir;
        if (!clientMapPreviewGetStart(v->preview, i, &sx, &sy, &sdir)) continue;
        float dx = ((float)((int)sx * tileSize) - camPXf) * tileScale;
        float dy = ((float)((int)sy * tileSize) - camPYf) * tileScale;
        if (dx + scaledTileF < 0 || dx > screenW ||
            dy + scaledTileF < 0 || dy > screenH) continue;
        int dir = sdir;
        const int *batX, *batY;
        Uint8 ba;
        boatStyleForOwner(v, i, &batX, &batY, &ba);   /* self black / ally green
                                                       * / enemy red / free 50% */
        SDL_FRect src = {
            (float)batX[dir], (float)batY[dir],
            (float)tileSize, (float)tileSize
        };
        SDL_FRect dest = { dx, dy, scaledTileF, scaledTileF };
        SDL_SetTextureAlphaMod(v->tilesTex, ba);
        SDL_RenderTexture(renderer, v->tilesTex, &src, &dest);
    }

    SDL_SetTextureAlphaMod(v->tilesTex, 255);
}

extern "C" bool mapPreviewViewWorldToScreen(const MapPreviewView *v,
                                            int mapSqX, int mapSqY,
                                            float *outX, float *outY) {
    if (!v || !v->startsTransformValid) return false;
    int tileSize = TILE_SIZE_X;
    float centerPXf = viewCamPixelsF(v->startsCenterX, tileSize);
    float centerPYf = viewCamPixelsF(v->startsCenterY, tileSize);
    float halfX = (float)v->startsScreenW / (2.0f * v->startsTileScale);
    float halfY = (float)v->startsScreenH / (2.0f * v->startsTileScale);
    float camPXf = centerPXf - halfX;
    float camPYf = centerPYf - halfY;
    if (outX) *outX = ((float)(mapSqX * tileSize) - camPXf) * v->startsTileScale;
    if (outY) *outY = ((float)(mapSqY * tileSize) - camPYf) * v->startsTileScale;
    return true;
}

extern "C" bool mapPreviewViewScreenToWorld(const MapPreviewView *v,
                                            float sx, float sy,
                                            int *outMapSqX, int *outMapSqY) {
    if (!v || !v->startsTransformValid) return false;
    int tileSize = TILE_SIZE_X;
    float centerPXf = viewCamPixelsF(v->startsCenterX, tileSize);
    float centerPYf = viewCamPixelsF(v->startsCenterY, tileSize);
    float halfX = (float)v->startsScreenW / (2.0f * v->startsTileScale);
    float halfY = (float)v->startsScreenH / (2.0f * v->startsTileScale);
    float camPXf = centerPXf - halfX;
    float camPYf = centerPYf - halfY;
    float tileXf = (sx / v->startsTileScale + camPXf) / (float)tileSize;
    float tileYf = (sy / v->startsTileScale + camPYf) / (float)tileSize;
    int mx = (int)floorf(tileXf);
    int my = (int)floorf(tileYf);
    if (mx < 0 || mx > 255 || my < 0 || my > 255) return false;
    if (outMapSqX) *outMapSqX = mx;
    if (outMapSqY) *outMapSqY = my;
    return true;
}

/* Minimap-colour rendering — one coloured rect per map tile, sized to
 * whatever fits the current sub-0.5 zoom. Replaces tile-sprite drawing
 * for the far-zoomed-out view. Includes pill/base/start dots and the
 * border-zone darkening that minimapRenderPixels does. */
static void viewRenderMinimapToOffscreen(MapPreviewView *v,
                                         SDL_Renderer *renderer,
                                         int screenW, int screenH) {
    /* Minimap mode doesn't draw starts, so any cached sprite-mode
     * transform is stale here — invalidate it. */
    v->startsTransformValid = false;
    /* tilePx ≥ 1; at 0.0625× it's 1 (16 game px × 0.0625 = 1). */
    float tilePxF = (float)TILE_SIZE_X * v->zoomLevel;
    int tilePx = (int)tilePxF;
    if (tilePx < 1) tilePx = 1;

    int tileSizeWU = 256; /* WORLD units per tile (= 1 << TANK_SHIFT_MAPSIZE) */
    /* Camera centre in pixels at current zoom. */
    float centerPxF = ((float)v->centerX / (float)tileSizeWU) * tilePxF;
    float centerPyF = ((float)v->centerY / (float)tileSizeWU) * tilePxF;
    float camPxF = centerPxF - (float)screenW * 0.5f;
    float camPyF = centerPyF - (float)screenH * 0.5f;

    /* First / last visible tile (inclusive). Pad by one so partial
     * tiles at the edges still draw. */
    int firstMX = (int)floorf(camPxF / tilePxF) - 1;
    int firstMY = (int)floorf(camPyF / tilePxF) - 1;
    int lastMX  = (int)floorf((camPxF + (float)screenW) / tilePxF) + 1;
    int lastMY  = (int)floorf((camPyF + (float)screenH) / tilePxF) + 1;
    if (firstMX < 0) firstMX = 0;
    if (firstMY < 0) firstMY = 0;
    if (lastMX > 255) lastMX = 255;
    if (lastMY > 255) lastMY = 255;

    for (int my = firstMY; my <= lastMY; my++) {
        for (int mx = firstMX; mx <= lastMX; mx++) {
            BYTE raw = clientMapPreviewGetTerrain(v->preview,
                                                   (BYTE)mx, (BYTE)my);
            BYTE terrain = raw;
            bool isMined = false;
            if (raw >= MINE_START && raw <= MINE_END) {
                isMined = true;
                terrain = (BYTE)(raw - MINE_SUBTRACT);
            }
            uint8_t cr, cg, cb;
            minimapTerrainColor(terrain, &cr, &cg, &cb);
            if (isMined) {
                cr = (uint8_t)((float)cr * 0.8f);
                cg = (uint8_t)((float)cg * 0.8f);
                cb = (uint8_t)((float)cb * 0.8f);
            }
            /* Darken the mine-border ring the game auto-mines so the
             * border reads even at minimap zoom. */
            if (mx <= MAP_MINE_EDGE_LEFT || mx >= MAP_MINE_EDGE_RIGHT ||
                my <= MAP_MINE_EDGE_TOP  || my >= MAP_MINE_EDGE_BOTTOM) {
                cr = (uint8_t)((float)cr * 0.5f);
                cg = (uint8_t)((float)cg * 0.5f);
                cb = (uint8_t)((float)cb * 0.5f);
            }

            float dx = (float)mx * tilePxF - camPxF;
            float dy = (float)my * tilePxF - camPyF;
            SDL_FRect dest = { dx, dy, tilePxF + 0.5f, tilePxF + 0.5f };
            SDL_SetRenderDrawColor(renderer, cr, cg, cb, 255);
            SDL_RenderFillRect(renderer, &dest);
        }
    }

    /* Object dots — pill (red), base (white), start (yellow). Size
     * scales with tilePx so they stay legible. */
    int dotSize = tilePx;
    if (dotSize < 2) dotSize = 2;
    if (dotSize > 4 && tilePx <= 4) dotSize = 4;

    BYTE numPills = clientMapPreviewGetPillCount(v->preview);
    for (BYTE i = 1; i <= numPills; i++) {
        BYTE px, py;
        if (!clientMapPreviewGetPill(v->preview, i, &px, &py, NULL, NULL)) continue;
        float dx = (float)px * tilePxF - camPxF;
        float dy = (float)py * tilePxF - camPyF;
        SDL_FRect dot = { dx, dy, (float)dotSize, (float)dotSize };
        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        SDL_RenderFillRect(renderer, &dot);
    }
    BYTE numBases = clientMapPreviewGetBaseCount(v->preview);
    for (BYTE i = 1; i <= numBases; i++) {
        BYTE bx, by;
        if (!clientMapPreviewGetBase(v->preview, i, &bx, &by, NULL)) continue;
        float dx = (float)bx * tilePxF - camPxF;
        float dy = (float)by * tilePxF - camPyF;
        SDL_FRect dot = { dx, dy, (float)dotSize, (float)dotSize };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderFillRect(renderer, &dot);
    }
    BYTE numStarts = clientMapPreviewGetStartCount(v->preview);
    for (BYTE i = 1; i <= numStarts; i++) {
        BYTE sx, sy;
        if (!clientMapPreviewGetStart(v->preview, i, &sx, &sy, NULL)) continue;
        float dx = (float)sx * tilePxF - camPxF;
        float dy = (float)sy * tilePxF - camPyF;
        SDL_FRect dot = { dx, dy, (float)dotSize, (float)dotSize };
        /* Colour the dot by ownership (matches the minimap preview): self
         * black, ally green, enemy red, unclaimed yellow. */
        Uint8 r = 255, g = 255, b = 0;
        int k = (int)i - 1;
        if (k >= 0 && k < v->startOwnerCount) {
            switch (v->startOwners[k]) {
                case 1: r = 0;   g = 110; b = 0;   break; /* self  dark green */
                case 2: r = 0;   g = 210; b = 0;   break; /* ally  green */
                case 3: r = 230; g = 50;  b = 50;  break; /* enemy red */
                default: break;                            /* free  yellow */
            }
        }
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_RenderFillRect(renderer, &dot);
    }
}

static void viewRenderTilesToOffscreen(MapPreviewView *v,
                                       SDL_Renderer *renderer,
                                       int screenW, int screenH,
                                       float tileScale) {
    /* `tileScale` is "offscreen pixels per game pixel" — i.e. how big
     * each game tile appears in this offscreen. Derived by the caller
     * from the *actual* post-clamp offscreen size so the apparent
     * zoom on display matches v->zoomLevel even when ofsW is capped
     * at 4096. Float-valued because clamps produce non-integer values
     * (e.g. ofsW=4096 at zoom 0.9 / viewW=1500 → tileScale ≈ 2.46). */
    int tileSize = TILE_SIZE_X;
    float scaledTileF = (float)tileSize * tileScale;
    if (scaledTileF < 1.0f) scaledTileF = 1.0f;

    float centerPXf = viewCamPixelsF(v->centerX, tileSize);
    float centerPYf = viewCamPixelsF(v->centerY, tileSize);
    /* Camera half-span in tile-pixel units (16 per tile, scale
     * independent). The offscreen carries scaledTileF pixels per
     * tile so half-span = screenW / (2 * scaledTileF) tiles, and
     * each tile = tileSize tile-pixels. */
    float halfX = (float)screenW / (2.0f * tileScale);
    float halfY = (float)screenH / (2.0f * tileScale);
    float camPXf = centerPXf - halfX;
    float camPYf = centerPYf - halfY;

    int camMX = (int)floorf(camPXf / (float)tileSize);
    int camMY = (int)floorf(camPYf / (float)tileSize);
    float edgeX = (camPXf - (float)(camMX * tileSize)) * tileScale;
    float edgeY = (camPYf - (float)(camMY * tileSize)) * tileScale;

    /* Scaled-atlas fast path: when zoom is sub-1x AND we have a
     * pre-downscaled cache, the offscreen is viewW×viewH (not
     * oversampled) and we just blit a sub-rect of the cache 1:1.
     * The cache itself was LINEAR-downscaled once at this zoom by
     * the caller (RenderOffscreen), so panning never re-runs the
     * filter — every output pixel always samples the same cached
     * source pixel. NEAREST scale mode on the cache so the 1:1
     * blit is a literal copy. */
    if (v->mapAtlasScaled && v->mapAtlasScaledEdge > 0 &&
        v->zoomLevel < 1.0f &&
        v->zoomLevel >= ZOOM_MINIMAP_MAX) {
        int scaledEdge = v->mapAtlasScaledEdge;
        /* Camera top-left in scaled-atlas pixel coords. camPXf was
         * computed above in native (16 px / tile) atlas pixels, so
         * scale by v->zoomLevel. */
        float camScaledX = camPXf * v->zoomLevel;
        float camScaledY = camPYf * v->zoomLevel;

        SDL_SetRenderDrawColor(renderer, 0, 0, 64, 255);
        SDL_RenderClear(renderer);

        float clipL = (camScaledX < 0.0f) ? -camScaledX : 0.0f;
        float clipT = (camScaledY < 0.0f) ? -camScaledY : 0.0f;
        float clipR = (camScaledX + (float)screenW > (float)scaledEdge)
                      ? (camScaledX + (float)screenW - (float)scaledEdge) : 0.0f;
        float clipB = (camScaledY + (float)screenH > (float)scaledEdge)
                      ? (camScaledY + (float)screenH - (float)scaledEdge) : 0.0f;
        SDL_FRect src = {
            camScaledX + clipL, camScaledY + clipT,
            (float)screenW - clipL - clipR,
            (float)screenH - clipT - clipB
        };
        SDL_FRect dst = {
            clipL, clipT,
            (float)screenW - clipL - clipR,
            (float)screenH - clipT - clipB
        };
        if (src.w > 0.0f && src.h > 0.0f) {
            SDL_RenderTexture(renderer, v->mapAtlasScaled, &src, &dst);
        }

        viewRenderStarts(v, renderer, screenW, screenH, tileScale);
        return;
    }

    /* Atlas fast path: pre-rendered map at 16 px/tile means the
     * visible region is one src sub-rect and the whole panel is one
     * SDL_RenderTexture call (vs the ~thousands per frame the old
     * loop did). Atlas is NEAREST so upscaling at zoom >= 1 stays
     * crisp; at sub-1x with tileScale ≈ 1 the src and dst are the
     * same size for the visible window so no scaling either. */
    if (v->mapAtlas) {
        int atlasPx = v->mapAtlasTiles * v->mapAtlasTilePx;
        /* Visible source rect in atlas pixels. camPXf/camPYf are
         * already in 16-px tile-space (atlas's native), so we can
         * use them directly. Width / height = view in tile-pixels
         * (screen / tileScale). Clamp to atlas bounds; if the
         * camera shows off-map area we'll handle it via the clear
         * colour below. */
        float srcX = camPXf;
        float srcY = camPYf;
        float srcW = (float)screenW / tileScale;
        float srcH = (float)screenH / tileScale;

        /* Clear with sea colour so off-map regions read like deep
         * sea even when the camera's been panned past the edge. */
        SDL_SetRenderDrawColor(renderer, 0, 0, 64, 255);
        SDL_RenderClear(renderer);

        /* Compute the in-atlas intersection and the matching dst
         * sub-rect so panning past the edge doesn't sample garbage. */
        float clipL = (srcX < 0.0f) ? -srcX : 0.0f;
        float clipT = (srcY < 0.0f) ? -srcY : 0.0f;
        float clipR = (srcX + srcW > (float)atlasPx)
                      ? (srcX + srcW - (float)atlasPx) : 0.0f;
        float clipB = (srcY + srcH > (float)atlasPx)
                      ? (srcY + srcH - (float)atlasPx) : 0.0f;
        SDL_FRect src = {
            srcX + clipL, srcY + clipT,
            srcW - clipL - clipR,
            srcH - clipT - clipB
        };
        SDL_FRect dst = {
            clipL * tileScale,
            clipT * tileScale,
            (srcW - clipL - clipR) * tileScale,
            (srcH - clipT - clipB) * tileScale
        };
        if (src.w > 0.0f && src.h > 0.0f) {
            SDL_RenderTexture(renderer, v->mapAtlas, &src, &dst);
        }

        viewRenderStarts(v, renderer, screenW, screenH, tileScale);
        return;
    }

    /* Fallback path (atlas not built yet — e.g. first frame after
     * data parse): per-tile rasterisation. Same code as before. */
    int tilesW = (int)((float)screenW / scaledTileF) + 3;
    int tilesH = (int)((float)screenH / scaledTileF) + 3;
    if (tilesW > 256) tilesW = 256;
    if (tilesH > 256) tilesH = 256;

    for (int x = 0; x < tilesW; x++) {
        for (int y = 0; y < tilesH; y++) {
            int mapX = camMX + x;
            int mapY = camMY + y;
            BYTE tileNum;
            if (mapX < 0 || mapX > 255 || mapY < 0 || mapY > 255) {
                tileNum = DEEP_SEA_SOLID;
            } else {
                tileNum = viewCalcTile(v, (BYTE)mapX, (BYTE)mapY);
            }
            SDL_FRect src = {
                (float)(mapViewPosX[tileNum]),
                (float)(mapViewPosY[tileNum]),
                (float)tileSize, (float)tileSize
            };
            SDL_FRect dest = {
                (float)x * scaledTileF - edgeX,
                (float)y * scaledTileF - edgeY,
                scaledTileF, scaledTileF
            };
            SDL_RenderTexture(renderer, v->tilesTex, &src, &dest);
        }
    }

    SDL_SetTextureAlphaMod(v->tilesTex, 180);
    for (int x = 0; x < tilesW; x++) {
        for (int y = 0; y < tilesH; y++) {
            int mapX = camMX + x;
            int mapY = camMY + y;
            if (mapX < 0 || mapX > 255 || mapY < 0 || mapY > 255) continue;
            bool mined = false;
            BYTE raw = clientMapPreviewGetTerrain(v->preview, (BYTE)mapX, (BYTE)mapY);
            if (raw >= MINE_START && raw <= MINE_END) {
                mined = true;
            } else if (mapX <= MAP_MINE_EDGE_LEFT || mapX >= MAP_MINE_EDGE_RIGHT ||
                       mapY <= MAP_MINE_EDGE_TOP  || mapY >= MAP_MINE_EDGE_BOTTOM) {
                mined = true;
            }
            if (mined) {
                SDL_FRect mineSrc = {
                    (float)MINE_X, (float)MINE_Y,
                    (float)tileSize, (float)tileSize
                };
                SDL_FRect dest = {
                    (float)x * scaledTileF - edgeX,
                    (float)y * scaledTileF - edgeY,
                    scaledTileF, scaledTileF
                };
                SDL_RenderTexture(renderer, v->tilesTex, &mineSrc, &dest);
            }
        }
    }
    SDL_SetTextureAlphaMod(v->tilesTex, 255);

    viewRenderStarts(v, renderer, screenW, screenH, tileScale);
}

/* ── Map data lifecycle ──────────────────────────────────────────── */

static void viewFreeMapData(MapPreviewView *v) {
    for (int i = 0; i < 2; i++) {
        if (v->offscreen[i]) {
            SDL_DestroyTexture(v->offscreen[i]);
            v->offscreen[i] = NULL;
        }
    }
    v->offscreenBack = 0;
    v->offscreenW = 0;
    v->offscreenH = 0;
    if (v->scratch) { SDL_DestroyTexture(v->scratch); v->scratch = NULL; }
    v->scratchW = 0;
    v->scratchH = 0;
    if (v->mapAtlas) { SDL_DestroyTexture(v->mapAtlas); v->mapAtlas = NULL; }
    v->mapAtlasTiles = 0;
    v->mapAtlasTilePx = 0;
    if (v->mapAtlasScaled) {
        SDL_DestroyTexture(v->mapAtlasScaled);
        v->mapAtlasScaled = NULL;
    }
    v->mapAtlasScaledEdge = 0;
    v->mapAtlasScaledZoom = 0.0f;
    v->displayTex = NULL;
    if (v->dataLoaded && v->preview) {
        clientMapPreviewDestroy(v->preview);
        v->preview = NULL;
        v->dataLoaded = false;
    }
    v->autoFitDone = false;
}

/* Release the baked atlas and its downscale cache so viewEnsureMapAtlas
 * bakes them again. Unlike viewFreeMapData this keeps the parsed map, the
 * camera and the auto-fit, so a caller that only wants new art does not
 * pay for a reparse or lose the user's pan/zoom. */
static void viewDropMapAtlas(MapPreviewView *v) {
    if (v->mapAtlas) { SDL_DestroyTexture(v->mapAtlas); v->mapAtlas = NULL; }
    v->mapAtlasTiles  = 0;
    v->mapAtlasTilePx = 0;
    if (v->mapAtlasScaled) {
        SDL_DestroyTexture(v->mapAtlasScaled);
        v->mapAtlasScaled = NULL;
    }
    v->mapAtlasScaledEdge = 0;
    v->mapAtlasScaledZoom = 0.0f;
}

/* Build the per-view map atlas. Called once after the map parses
 * (and reset whenever Load* invalidates v->preview). All terrain
 * tiles + mine overlay are baked in; start positions stay as
 * dynamic overlays so they can be toggled / styled separately. */
static void viewEnsureMapAtlas(MapPreviewView *v, SDL_Renderer *renderer) {
    if (!v || !v->preview || !v->dataLoaded || !v->tilesTex) return;
    if (v->mapAtlas) return;

    const int tilesPerSide = 256;
    const int tilePx       = TILE_SIZE_X;
    const int atlasPx      = tilesPerSide * tilePx;

    v->mapAtlas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                     SDL_TEXTUREACCESS_TARGET,
                                     atlasPx, atlasPx);
    if (!v->mapAtlas) return;
    SDL_SetTextureScaleMode(v->mapAtlas, SDL_SCALEMODE_NEAREST);
    v->mapAtlasTiles  = tilesPerSide;
    v->mapAtlasTilePx = tilePx;

    /* Companion downscale cache, same backing size — we redraw into
     * its top-left sub-rect on every zoom change instead of creating
     * a new texture, so the GPU never sees an allocate / free cycle
     * mid-frame. The size of the in-use sub-rect is tracked in
     * mapAtlasScaledEdge. */
    v->mapAtlasScaled = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                            SDL_TEXTUREACCESS_TARGET,
                                            atlasPx, atlasPx);
    if (v->mapAtlasScaled) {
        SDL_SetTextureScaleMode(v->mapAtlasScaled, SDL_SCALEMODE_NEAREST);
    }
    v->mapAtlasScaledEdge = 0;
    v->mapAtlasScaledZoom = 0.0f;

    SDL_SetTextureScaleMode(v->tilesTex, SDL_SCALEMODE_NEAREST);
    SDL_SetRenderTarget(renderer, v->mapAtlas);
    SDL_SetRenderDrawColor(renderer, 0, 0, 64, 255);
    SDL_RenderClear(renderer);

    /* Terrain pass. */
    for (int mapY = 0; mapY < tilesPerSide; mapY++) {
        for (int mapX = 0; mapX < tilesPerSide; mapX++) {
            BYTE tileNum = viewCalcTile(v, (BYTE)mapX, (BYTE)mapY);
            SDL_FRect src = {
                (float)mapViewPosX[tileNum],
                (float)mapViewPosY[tileNum],
                (float)tilePx, (float)tilePx
            };
            SDL_FRect dst = {
                (float)(mapX * tilePx),
                (float)(mapY * tilePx),
                (float)tilePx, (float)tilePx
            };
            SDL_RenderTexture(renderer, v->tilesTex, &src, &dst);
        }
    }

    /* Mine-overlay pass: translucent mine sprite on top of mined
     * tiles AND the auto-mined border zone, matching the live
     * renderer in viewRenderTilesToOffscreen. */
    SDL_SetTextureAlphaMod(v->tilesTex, 180);
    for (int mapY = 0; mapY < tilesPerSide; mapY++) {
        for (int mapX = 0; mapX < tilesPerSide; mapX++) {
            bool mined = false;
            BYTE raw = clientMapPreviewGetTerrain(v->preview,
                                                   (BYTE)mapX, (BYTE)mapY);
            if (raw >= MINE_START && raw <= MINE_END) {
                mined = true;
            } else if (mapX <= MAP_MINE_EDGE_LEFT ||
                       mapX >= MAP_MINE_EDGE_RIGHT ||
                       mapY <= MAP_MINE_EDGE_TOP  ||
                       mapY >= MAP_MINE_EDGE_BOTTOM) {
                mined = true;
            }
            if (!mined) continue;
            SDL_FRect src = {
                (float)MINE_X, (float)MINE_Y,
                (float)tilePx, (float)tilePx
            };
            SDL_FRect dst = {
                (float)(mapX * tilePx),
                (float)(mapY * tilePx),
                (float)tilePx, (float)tilePx
            };
            SDL_RenderTexture(renderer, v->tilesTex, &src, &dst);
        }
    }
    SDL_SetTextureAlphaMod(v->tilesTex, 255);

    SDL_SetRenderTarget(renderer, NULL);
}

static void viewAutoFitZoom(MapPreviewView *v, int viewW, int viewH) {
    if (!v->dataLoaded || !v->preview) return;
    int minX = 255, minY = 255, maxX = 0, maxY = 0;
    for (int y = 0; y < 256; y++) {
        for (int x = 0; x < 256; x++) {
            BYTE t = clientMapPreviewGetTerrain(v->preview, (BYTE)x, (BYTE)y);
            if (t != DEEP_SEA) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    {
        BYTE n = clientMapPreviewGetPillCount(v->preview);
        for (BYTE i = 1; i <= n; i++) {
            BYTE px, py;
            if (!clientMapPreviewGetPill(v->preview, i, &px, &py, NULL, NULL)) continue;
            if (px < minX) minX = px; if (px > maxX) maxX = px;
            if (py < minY) minY = py; if (py > maxY) maxY = py;
        }
    }
    {
        BYTE n = clientMapPreviewGetBaseCount(v->preview);
        for (BYTE i = 1; i <= n; i++) {
            BYTE bx, by;
            if (!clientMapPreviewGetBase(v->preview, i, &bx, &by, NULL)) continue;
            if (bx < minX) minX = bx; if (bx > maxX) maxX = bx;
            if (by < minY) minY = by; if (by > maxY) maxY = by;
        }
    }
    {
        BYTE n = clientMapPreviewGetStartCount(v->preview);
        for (BYTE i = 1; i <= n; i++) {
            BYTE sx, sy;
            if (!clientMapPreviewGetStart(v->preview, i, &sx, &sy, NULL)) continue;
            if (sx < minX) minX = sx; if (sx > maxX) maxX = sx;
            if (sy < minY) minY = sy; if (sy > maxY) maxY = sy;
        }
    }

    if (maxX < minX || maxY < minY) {
        v->zoomIndex = ZOOM_STEP_1X;
        v->zoomLevel = 1.0f;
        return;
    }

    minX -= 2; minY -= 2; maxX += 2; maxY += 2;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > 255) maxX = 255;
    if (maxY > 255) maxY = 255;

    v->centerX = ((minX + maxX) / 2) << 8;
    v->centerY = ((minY + maxY) / 2) << 8;

    int contentW = (maxX - minX + 1) * TILE_SIZE_X;
    int contentH = (maxY - minY + 1) * TILE_SIZE_Y;

    float zoomW = (float)viewW / (float)contentW;
    float zoomH = (float)viewH / (float)contentH;
    float idealZoom = (zoomW < zoomH) ? zoomW : zoomH;
    /* Lower bound is the first zoom step (0.0625 = 1 px per tile). */
    if (idealZoom < kZoomSteps[0]) idealZoom = kZoomSteps[0];

    v->zoomIndex = 0;
    for (int i = 0; i < ZOOM_STEP_COUNT; i++) {
        if (kZoomSteps[i] <= idealZoom) v->zoomIndex = i;
        else                            break;
    }
    v->zoomLevel = kZoomSteps[v->zoomIndex];
}

static void viewEnsureParsed(MapPreviewView *v, SDL_Renderer *renderer) {
    if (!v->compressedData && !v->filePath) return;

    /* Ahead of the dataLoaded early-out: a skin change rebuilds the tile
     * atlas in place, and the widget can be on screen while it happens (the
     * lobby preview sits behind the settings dialog). Whether our sheet is
     * current has nothing to do with whether the map parsed, so check it
     * every frame — a pointer and an int compare when nothing has moved. */
    unsigned int gen = sdl3DrawGetTilesGeneration();
    if (!v->tilesTex || v->tilesGeneration != gen) {
        SDL_Surface *sheet = tileLoaderBuildSheet(16);
        if (sheet) {
            /* Only the generation path has an old texture to release, and
             * it is still live: the renderer has not changed under us. */
            bool hadSheet = (v->tilesTex != NULL);
            if (v->tilesTex) SDL_DestroyTexture(v->tilesTex);
            v->tilesTex = SDL_CreateTextureFromSurface(renderer, sheet);
            SDL_SetTextureScaleMode(v->tilesTex, SDL_SCALEMODE_NEAREST);
            SDL_DestroySurface(sheet);
            v->tilesGeneration = gen;
            /* The terrain is baked into the map atlas from the sheet that
             * was live when it was built, so a new sheet has to rebake it.
             * Without this only the start-position overlays, which blit
             * from tilesTex every frame, would show the new art. */
            if (hadSheet) viewDropMapAtlas(v);
        }
    }

    if (v->dataLoaded) return;

    if (v->compressedData) {
        v->preview = clientMapPreviewLoadFromBuffer(v->compressedData,
                                                    v->compressedLen);
    } else if (v->filePath) {
        v->preview = clientMapPreviewLoadFromFile(v->filePath);
        if (!v->preview) {
            /* iOS/macOS bundle fallback — copy to a writable temp
             * file the parser can open. */
            size_t fileSize = 0;
            void *fileData = SDL_LoadFile(v->filePath, &fileSize);
            if (fileData && fileSize > 0) {
                char *tmpDir = SDL_GetPrefPath("WinBolo", "WinBolo");
                char tmpPath[512];
                SDL_snprintf(tmpPath, sizeof(tmpPath),
                             "%s_view_temp.map", tmpDir ? tmpDir : "");
                SDL_free(tmpDir);
                FILE *fp = fopen(tmpPath, "wb");
                if (fp) {
                    fwrite(fileData, 1, fileSize, fp);
                    fclose(fp);
                    v->preview = clientMapPreviewLoadFromFile(tmpPath);
                    remove(tmpPath);
                }
            }
            SDL_free(fileData);
        }
    }
    v->dataLoaded = (v->preview != NULL);
}

/* ── Public API ──────────────────────────────────────────────────── */

extern "C" MapPreviewView *mapPreviewViewCreate(void) {
    MapPreviewView *v = (MapPreviewView *)SDL_calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->zoomIndex = ZOOM_STEP_1X;
    v->zoomLevel = 1.0f;
    v->centerX   = 128 << 8;
    v->centerY   = 128 << 8;
    v->startsTransformValid = false;
    return v;
}

extern "C" void mapPreviewViewDestroy(MapPreviewView *v) {
    if (!v) return;
    viewFreeMapData(v);
    if (v->compressedData) { SDL_free(v->compressedData); v->compressedData = NULL; }
    if (v->filePath)       { SDL_free(v->filePath);       v->filePath       = NULL; }
    if (v->tilesTex)       { SDL_DestroyTexture(v->tilesTex); v->tilesTex   = NULL; }
    if (v->scratch)        { SDL_DestroyTexture(v->scratch); v->scratch     = NULL; }
    SDL_free(v);
}

extern "C" bool mapPreviewViewLoadCompressed(MapPreviewView *v,
                                              const BYTE *data, int len) {
    if (!v || !data || len <= 0) return false;
    if (v->compressedData) { SDL_free(v->compressedData); v->compressedData = NULL; v->compressedLen = 0; }
    if (v->filePath)       { SDL_free(v->filePath);       v->filePath = NULL; }
    v->compressedData = (BYTE *)SDL_malloc(len);
    if (!v->compressedData) return false;
    SDL_memcpy(v->compressedData, data, len);
    v->compressedLen = len;
    viewFreeMapData(v);
    return true;
}

extern "C" bool mapPreviewViewLoadCompressedKeepCamera(MapPreviewView *v,
                                                        const BYTE *data, int len) {
    if (!v) return false;
    /* Snapshot before the reload — LoadCompressed → viewFreeMapData
     * flips autoFitDone back to false, which would re-fit on the
     * next render and lose the user's pan/zoom. */
    int   savedZoomIdx = v->zoomIndex;
    float savedZoom    = v->zoomLevel;
    WORLD savedCx      = v->centerX;
    WORLD savedCy      = v->centerY;
    bool  hadFit       = v->autoFitDone;
    if (!mapPreviewViewLoadCompressed(v, data, len)) return false;
    if (hadFit) {
        v->zoomIndex   = savedZoomIdx;
        v->zoomLevel   = savedZoom;
        v->centerX     = savedCx;
        v->centerY     = savedCy;
        v->autoFitDone = true;
    }
    return true;
}

extern "C" bool mapPreviewViewLoadFile(MapPreviewView *v, const char *path) {
    if (!v || !path || path[0] == '\0') return false;
    if (v->compressedData) { SDL_free(v->compressedData); v->compressedData = NULL; v->compressedLen = 0; }
    if (v->filePath)       { SDL_free(v->filePath);       v->filePath = NULL; }
    v->filePath = SDL_strdup(path);
    if (!v->filePath) return false;
    viewFreeMapData(v);
    return true;
}

extern "C" void mapPreviewViewSetInitialBounds(MapPreviewView *v,
                                                int minX, int minY,
                                                int maxX, int maxY) {
    if (!v) return;
    v->zoomIndex   = ZOOM_STEP_1X + 1;
    v->zoomLevel   = 2.0f;
    v->centerX     = ((minX + maxX) / 2) << 8;
    v->centerY     = ((minY + maxY) / 2) << 8;
    v->autoFitDone = false;
}

extern "C" void mapPreviewViewRenderOffscreen(MapPreviewView *v,
                                               SDL_Renderer *renderer,
                                               int viewW, int viewH) {
    if (!v || !renderer || viewW < 1 || viewH < 1) return;

    viewEnsureParsed(v, renderer);
    if (!v->dataLoaded || !v->tilesTex) return;
    /* Build the per-map atlas the first frame after parse — turns
     * the per-frame tile loop into a single visible-region blit. */
    viewEnsureMapAtlas(v, renderer);

    /* Auto-fit zoom on first frame after data is loaded. */
    if (!v->autoFitDone) {
        viewAutoFitZoom(v, viewW, viewH);
        v->autoFitDone = true;
    }

    /* Detect viewport resize. */
    if (viewW != v->lastViewW || viewH != v->lastViewH) {
        v->lastViewW = viewW;
        v->lastViewH = viewH;
        for (int i = 0; i < 2; i++) {
            if (v->offscreen[i]) {
                SDL_DestroyTexture(v->offscreen[i]);
                v->offscreen[i] = NULL;
            }
        }
        v->displayTex = NULL;
        v->offscreenW = 0;
        v->offscreenH = 0;
    }

    /* Sub-1x zoom in sprite mode: render tiles at native 1x scale
     * (16 px each) into an offscreen sized at viewW / zoom — i.e.
     * just big enough that the visible tile count matches what zoom
     * implies. ImGui's display-time bilinear then downscales the
     * whole offscreen to viewW. One filter pass, no manual
     * downsample, no oversampled atlas. Simpler and the tiles stay
     * crisp at 1x source resolution.
     *
     * Clamp PROPORTIONALLY at 4096² so texture aspect always matches
     * the display rect (independent-axis clamping caused stretch).
     * Above the cap the apparent zoom degrades smoothly — fewer
     * visible tiles than zoom-level implies, but no aspect distortion. */
    /* When the scaled-atlas pre-downscale cache is ready, sub-1x
     * sprite mode no longer needs an oversampled offscreen: we just
     * blit a viewW×viewH sub-rect of the cache 1:1. Keeps pan stable
     * (sampling pattern baked at cache build time). If the cache
     * doesn't exist yet (atlas not built or first frame after Load*)
     * we fall through to the old oversample-then-bilinear path. */
    bool subOneSprite = (v->zoomLevel < 1.0f &&
                         v->zoomLevel >= ZOOM_MINIMAP_MAX);
    bool useScaledCache = (subOneSprite && v->mapAtlasScaled && v->mapAtlas);

    int ofsW = viewW, ofsH = viewH;
    if (subOneSprite && !useScaledCache) {
        float scale = 1.0f / v->zoomLevel;
        float fW = (float)viewW * scale;
        float fH = (float)viewH * scale;
        float largest = (fW > fH) ? fW : fH;
        if (largest > 4096.0f) {
            float cap = 4096.0f / largest;
            fW *= cap;
            fH *= cap;
        }
        ofsW = (int)fW;
        ofsH = (int)fH;
        if (ofsW < 1) ofsW = 1;
        if (ofsH < 1) ofsH = 1;
    }

    /* Rebuild the pre-downscaled cache lazily, before binding the
     * offscreen as the next render target. One LINEAR-sampled blit
     * of the full mapAtlas into a (scaledEdge × scaledEdge) sub-rect
     * of mapAtlasScaled — sub-rect addressing means the texture
     * itself is allocated once and never freed mid-frame. */
    if (useScaledCache) {
        int atlasPx = v->mapAtlasTiles * v->mapAtlasTilePx;
        int scaledEdge = (int)((float)atlasPx * v->zoomLevel + 0.5f);
        if (scaledEdge < 1) scaledEdge = 1;
        if (scaledEdge > atlasPx) scaledEdge = atlasPx;
        if (v->mapAtlasScaledZoom != v->zoomLevel ||
            v->mapAtlasScaledEdge != scaledEdge) {
            SDL_SetTextureScaleMode(v->mapAtlas, SDL_SCALEMODE_LINEAR);
            SDL_SetRenderTarget(renderer, v->mapAtlasScaled);
            SDL_SetRenderDrawColor(renderer, 0, 0, 64, 255);
            SDL_RenderClear(renderer);
            SDL_FRect dst = { 0.0f, 0.0f,
                              (float)scaledEdge, (float)scaledEdge };
            SDL_RenderTexture(renderer, v->mapAtlas, NULL, &dst);
            SDL_SetRenderTarget(renderer, NULL);
            SDL_SetTextureScaleMode(v->mapAtlas, SDL_SCALEMODE_NEAREST);
            v->mapAtlasScaledEdge = scaledEdge;
            v->mapAtlasScaledZoom = v->zoomLevel;
        }
    }

    /* Display-time filtering for the offscreen. LINEAR belongs ONLY to the
     * oversampled sub-1x fallback (ofsW > viewW), where ImGui does the
     * downscale at draw time and the 2x2 average is what keeps identical
     * tiles looking identical across the map.
     *
     * Every other path — zoom >= 1, the pre-downscaled sub-1x cache, and
     * minimap mode — builds the offscreen at the view size and ImGui draws
     * it at ~1:1. The Image rect is a float and the offscreen size an int,
     * so that "1:1" is routinely off by a fraction of a pixel, and LINEAR
     * then re-blended every pixel of already-crisp art: that sub-pixel
     * resample is what read as "the zoom is really blurry". NEAREST costs
     * at most a 1px snap when the rect lands on a half pixel. */
    SDL_ScaleMode ofsScaleMode = (ofsW > viewW) ? SDL_SCALEMODE_LINEAR
                                                : SDL_SCALEMODE_NEAREST;

    if (!v->offscreen[0] || !v->offscreen[1] ||
        v->offscreenW != ofsW || v->offscreenH != ofsH) {
        for (int i = 0; i < 2; i++) {
            if (v->offscreen[i]) SDL_DestroyTexture(v->offscreen[i]);
            v->offscreen[i] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                                SDL_TEXTUREACCESS_TARGET,
                                                ofsW, ofsH);
            if (v->offscreen[i]) {
                SDL_SetTextureScaleMode(v->offscreen[i], ofsScaleMode);
            }
        }
        v->displayTex    = NULL;
        v->offscreenBack = 0;
        v->offscreenW    = ofsW;
        v->offscreenH    = ofsH;
    }

    SDL_Texture *back = v->offscreen[v->offscreenBack];
    if (back) {
        /* Re-assert scale modes EVERY frame (ofsScaleMode above picks
         * LINEAR only for the oversampled sub-1x fallback, where NEAREST at
         * non-integer downscale ratios — 0.75x is 1.33:1 — drifts the
         * per-tile sampling phase and makes identical grass tiles look
         * different across the map). The atlas stays NEAREST so pixel art
         * at zoom >= 1, where only the atlas blit upscales, keeps its crisp
         * tile edges. */
        SDL_SetTextureScaleMode(back, ofsScaleMode);
        if (v->tilesTex) {
            SDL_SetTextureScaleMode(v->tilesTex, SDL_SCALEMODE_NEAREST);
        }

        SDL_SetRenderTarget(renderer, back);
        SDL_SetRenderDrawColor(renderer, 0, 0, 64, 255);
        SDL_RenderClear(renderer);
        /* Below 0.33× game scale, switch to minimap-colour mode —
         * tile sprites at < ~5 px per tile look like noise, so swap
         * in the per-tile colour rep used by the dedicated 256×256
         * minimap. Above that, fall through to the sprite renderer. */
        if (v->zoomLevel < ZOOM_MINIMAP_MAX) {
            viewRenderMinimapToOffscreen(v, renderer, ofsW, ofsH);
        } else {
            /* tileScale = "tile pixels in offscreen per game pixel".
             * Derived from the *actual* (post-clamp) ofsW so the
             * displayed zoom matches v->zoomLevel even when the
             * offscreen had to be capped at 4096. At zoom >= 1 this
             * equals floor(zoom); at sub-1x (where ofsW = viewW/zoom)
             * it works out to 1.0 — tiles at native 16 px each in
             * the offscreen. */
            float tileScale =
                (float)ofsW * v->zoomLevel / (float)viewW;
            if (tileScale < 0.5f) tileScale = 0.5f;
            viewRenderTilesToOffscreen(v, renderer, ofsW, ofsH, tileScale);
        }
        SDL_SetRenderTarget(renderer, NULL);
        /* Publish the finished buffer and flip. The caller's next
         * mapPreviewViewGetTexture hands out this one, and the following
         * frame renders into the other, so nothing is ever drawn from a
         * texture SDL is mid-write into. */
        v->displayTex    = back;
        v->offscreenBack ^= 1;
    }
}

extern "C" SDL_Texture *mapPreviewViewGetTexture(MapPreviewView *v) {
    if (!v) return NULL;
    /* displayTex is the most recently completed offscreen (see the
     * double-buffer note on the struct); NULL until the first render. */
    return v->displayTex;
}

extern "C" void mapPreviewViewGetTextureSize(const MapPreviewView *v,
                                              int *outW, int *outH) {
    if (outW) *outW = v ? v->offscreenW : 0;
    if (outH) *outH = v ? v->offscreenH : 0;
}

extern "C" bool mapPreviewViewIsReady(const MapPreviewView *v) {
    return v && v->dataLoaded;
}

extern "C" BYTE mapPreviewViewGetStartCount(const MapPreviewView *v) {
    if (!v || !v->preview) return 0;
    return clientMapPreviewGetStartCount(v->preview);
}

extern "C" bool mapPreviewViewGetStart(const MapPreviewView *v, BYTE i,
                                       BYTE *outX, BYTE *outY) {
    if (!v || !v->preview) return false;
    BYTE dir;
    return clientMapPreviewGetStart(v->preview, i, outX, outY, &dir);
}

extern "C" void mapPreviewViewSetStartOwners(MapPreviewView *v,
                                             const uint8_t *owners, int count) {
    if (!v) return;
    if (!owners || count <= 0) { v->startOwnerCount = 0; return; }
    if (count > 16) count = 16;   /* MAX_STARTS */
    memcpy(v->startOwners, owners, (size_t)count);
    v->startOwnerCount = count;
}

extern "C" void mapPreviewViewCenterOnMapSquare(MapPreviewView *v,
                                                int mapSqX, int mapSqY,
                                                float minZoom) {
    if (!v) return;
    if (mapSqX < 0) mapSqX = 0;
    if (mapSqX > 255) mapSqX = 255;
    if (mapSqY < 0) mapSqY = 0;
    if (mapSqY > 255) mapSqY = 255;
    /* +128 puts the camera on the square's centre rather than its corner. */
    v->centerX = (WORLD)((mapSqX << 8) + 128);
    v->centerY = (WORLD)((mapSqY << 8) + 128);
    v->panResidX = 0.0f;
    v->panResidY = 0.0f;
    if (minZoom > 0.0f && v->zoomLevel < minZoom) {
        for (int i = 0; i < ZOOM_STEP_COUNT; i++) {
            if (kZoomSteps[i] >= minZoom) {
                v->zoomIndex = i;
                v->zoomLevel = kZoomSteps[i];
                break;
            }
        }
    }
    /* Claim the auto-fit so the first render after a fresh load can't
     * re-centre on the whole map and throw the requested focus away. */
    v->autoFitDone = true;
}

extern "C" bool mapPreviewViewWantsNearestSampling(const MapPreviewView *v) {
    if (!v) return true;
    /* Oversampled only on the sub-1x fallback path, where the offscreen is
     * built larger than the view and the caller's draw does the downscale. */
    return v->offscreenW <= v->lastViewW;
}

extern "C" float mapPreviewViewGetZoom(const MapPreviewView *v) {
    return v ? v->zoomLevel : 1.0f;
}

extern "C" float mapPreviewViewZoomIn(MapPreviewView *v) {
    if (!v) return 1.0f;
    if (v->zoomIndex < ZOOM_STEP_COUNT - 1) v->zoomIndex++;
    v->zoomLevel = kZoomSteps[v->zoomIndex];
    return v->zoomLevel;
}

extern "C" float mapPreviewViewZoomOut(MapPreviewView *v) {
    if (!v) return 1.0f;
    if (v->zoomIndex > 0) v->zoomIndex--;
    v->zoomLevel = kZoomSteps[v->zoomIndex];
    return v->zoomLevel;
}

extern "C" void mapPreviewViewHandleInput(MapPreviewView *v, bool hovered,
                                           const MapPreviewInputOpts *opts) {
    if (!v) return;
    MapPreviewInputOpts defaultOpts = { true, true, true, false };
    if (!opts) opts = &defaultOpts;
    ImGuiIO &io = ImGui::GetIO();

    /* Who owns this press is decided ONCE, on its first frame, and holds
     * until the button comes up.
     *
     * It used to be re-decided every frame from opts->dragPan and `hovered`,
     * and both flip mid-drag: the popup recomputes dragPan from "is the
     * cursor over a grabbable start marker" (startPickerWantsDrag), and
     * `hovered` blips off at the rect edge. So sweeping the cursor across the
     * map froze the camera for every frame it passed within a marker's hit
     * radius — 2.5 tiles, or 40 px, whichever is larger — and those frames'
     * deltas were dropped outright, never caught up. That is the periodic
     * "sticks, then is a tile out" pan: the stall repeats at the marker
     * spacing and the camera ends a sweep many tiles behind the cursor.
     *
     * Latching at the press keeps the popup's start-grab veto working (a
     * press that begins on a movable marker still never becomes a pan) while
     * making a pan that has started immune to whatever it later sweeps over.
     * The item is the pan InvisibleButton the caller submits immediately
     * before this call, so IsItemActive() is exactly "this press is mine". */
    bool pressHeld = ImGui::IsItemActive();
    if (!pressHeld) {
        v->panDragActive  = false;
        v->panDragChecked = false;
        /* No partial step to carry between drags — a stale remainder would
         * show up as a jump on the next press. */
        v->panResidX = 0.0f;
        v->panResidY = 0.0f;
    } else if (!v->panDragChecked) {
        v->panDragChecked = true;
        v->panDragActive  = opts->dragPan;
    }

    /* Deliberately not gated on `hovered`: once the pan owns the press it
     * keeps tracking the cursor even when the drag leaves the image rect,
     * which is what every other pannable view does. */
    if (v->panDragActive) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        viewPanBy(v, -viewPixelsToWorld(v->zoomLevel) * io.MouseDelta.x,
                     -viewPixelsToWorld(v->zoomLevel) * io.MouseDelta.y);
    }

    if (hovered) {
        if (opts->dragPan && !v->panDragActive) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (opts->wheelZoom && io.MouseWheel != 0) {
            int prevIndex = v->zoomIndex;
            if (io.MouseWheel > 0 && v->zoomIndex < ZOOM_STEP_COUNT - 1)
                v->zoomIndex++;
            else if (io.MouseWheel < 0 && v->zoomIndex > 0)
                v->zoomIndex--;
            if (v->zoomIndex != prevIndex) {
                float oldZoom = v->zoomLevel;
                v->zoomLevel  = kZoomSteps[v->zoomIndex];
                viewZoomAnchorAtCursor(v, oldZoom);
            }
        }
    }

    if (opts->pinchZoom) {
        float pinch = macOSPinchZoomConsume();
        if (pinch != 0.0f) {
            v->pinchAccum += pinch;
            while (v->pinchAccum > 0.15f) {
                if (v->zoomIndex < ZOOM_STEP_COUNT - 1) v->zoomIndex++;
                v->pinchAccum -= 0.15f;
            }
            while (v->pinchAccum < -0.15f) {
                if (v->zoomIndex > 0) v->zoomIndex--;
                v->pinchAccum += 0.15f;
            }
            v->zoomLevel = kZoomSteps[v->zoomIndex];
        }
    }

    if (opts->arrowPan) {
        float panSpeed = 512.0f / v->zoomLevel;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) {
            int newCX = (int)v->centerX - (int)panSpeed;
            v->centerX = (WORLD)(newCX < 0 ? 0 : newCX);
        }
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
            int newCX = (int)v->centerX + (int)panSpeed;
            v->centerX = (WORLD)(newCX > 65280 ? 65280 : newCX);
        }
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) {
            int newCY = (int)v->centerY - (int)panSpeed;
            v->centerY = (WORLD)(newCY < 0 ? 0 : newCY);
        }
        if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) {
            int newCY = (int)v->centerY + (int)panSpeed;
            v->centerY = (WORLD)(newCY > 65280 ? 65280 : newCY);
        }
    }
}
