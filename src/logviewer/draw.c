/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
*Name:          Draw
*Filename:      draw.c
*Author:        John Morrison
*Creation Date: 13/12/98
*Last Modified:  2024
*Purpose:
*  System Specific Drawing routines (Uses SDL3)
*********************************************************/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lv_global.h"
#include "lv_screenlgm.h"
#include "tiles.h"
#include "lv_tilenum.h"
#include "positions.h"
#include "draw.h"
#include "logviewer.h"
#include "lv_region_rect.h"
#include "game_view.h"
#include "imgui/imgui_main_menu.h"
#include "../gui/sdl3/sdl_bmp.h"
#include "../gui/sdl3/sprite_positions.h"
#include "../gui/sdl3/tileloader.h"
#include "../gui/sdl3/map_colours.h"   /* the zoomed-out ground colours */
#include "../gui/sdl3/gfx_settings.h"  /* the simplified view setting */
#include "../gui/sdl3/map_markers.h"   /* the shapes a thing becomes when small */
#include "../third_party/stb/stb_image.h"

/* Must be included after global.h to avoid bool type conflict */
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#define TEAM_IMAGE_SIZE_X (16 * 16)
#define TEAM_IMAGE_SIZE_Y (17 * 16)
#define ITEM_IMAGE_SIZE_X (17 * 16)
#define ITEM_IMAGE_SIZE_Y (17 * 16)

/* SDL3 Renderer and Window */
static SDL_Window *sdlWindow = NULL;
static SDL_Renderer *sdlRenderer = NULL;
static int sdlInitialized = 0;  /* Track if SDL is already initialized */
static bool ownsWindow = TRUE;  /* TRUE in standalone, FALSE when embedded */

/* Sprite Textures (loaded once at startup) */
static SDL_Texture *textureTiles = NULL;
static SDL_Texture *textureTanks = NULL;
static SDL_Texture *textureBoats = NULL;
static SDL_Texture *textureItems = NULL;

/* Render Target (back buffer equivalent) */
static SDL_Texture *textureTarget = NULL;
static int targetWidth = 0;
static int targetHeight = 0;

/* Font for labels */
static TTF_Font *labelFont = NULL;

/* Tank names. The render target is 1x pixel art that the blit magnifies, so a
 * name drawn into it comes out blocky above 1x and smeared below. Instead
 * lv_drawTanks records each name and where it goes in target pixels, and the
 * names are drawn over the blit at the blit's scale: by lvDrawBlitTargetToWindow
 * here, and by the host through lvEmbedTankLabel when embedded. The list is
 * rebuilt only when the target is, so it always describes what the target
 * holds. A GIF export reads the target back, so it asks for the names in the
 * target instead (lv_drawSetTankLabelsInTarget). */
#define LV_TANK_LABEL_PX      13   /* the game's label face, gFontMsg, at 1x */
#define LV_TANK_LABEL_MIN_PX  8    /* below this the name cannot be read */
#define LV_TANK_LABEL_GREY    200  /* the game's TANK_LABEL_NAME_GREY */
typedef struct {
    char name[PLAYER_NAME_LEN];
    int  x, y;                     /* top-left, in render-target pixels */
    int  tex;                      /* s_tankLabelTex slot, -1 for none;
                                      set by lv_drawTankLabelCount */
} LvTankLabelPos;
/* One rasterized name. Found by name, not by list position, so a name keeps
 * its texture when a tank entering or leaving view shifts the list. */
typedef struct {
    char         name[PLAYER_NAME_LEN];
    SDL_Texture *tex;              /* white; tinted at draw time */
    int          w, h;
} LvTankLabelTex;
static LvTankLabelPos s_tankLabelPos[MAX_TANKS];
static int            s_tankLabelCount = 0;
static LvTankLabelTex s_tankLabelTex[MAX_TANKS];
static TTF_Font      *s_tankLabelFont = NULL;
static int            s_tankLabelFontPx = 0;
static bool           s_tankLabelsInTarget = FALSE;

/* Used for storing time */
static uint32_t g_dwFrameTotal = 0;

/* Stepped zoom table — same values as the map editor (mapeditor.c).
 * Below 1.0 the texture target grows so we can show more tiles, then
 * the blit scales it down; at and above 1.0 the texture target stays
 * sized to the visible tile count and the blit scales it up. */
static const float g_zoomSteps[] = {
    0.5f, 0.6f, 0.7f, 0.8f, 0.9f,
    1.0f, 2.0f, 3.0f, 4.0f
};
#define ZOOM_STEP_COUNT ((int)(sizeof(g_zoomSteps) / sizeof(g_zoomSteps[0])))
#define ZOOM_STEP_1X    5

static int   g_zoomStepIndex = ZOOM_STEP_1X;
static float g_zoomLevel     = 1.0f;

/* Whether the last frame drew map colours instead of tile sprites. The
 * per-square cache below keys on the tile number alone, so a square whose
 * tile has not changed is left alone - which would leave half the map in the
 * old style when the answer flips. A zoom change already asks for a full
 * redraw; this covers any other way the answer can change. */
static bool  g_lastSimple    = false;

/* The simplified view. Tiles are drawn at their native 16 px into the render
 * target and the blit downscales the lot, so below 1x a square lands on the
 * window as eight to fourteen pixels and the sprite in it is a smudge: the
 * ground goes to one map colour a square and the things standing on it to
 * marker shapes. The setting is the game's, out of the prefs document both
 * apps share (loadPreferences).
 *
 * Asked here by the ground pass and the tank pass alike, so the two cannot
 * end up in different styles on the same frame. */
static bool lvSimpleView(void) {
    return gfxGetSimplifiedZoomOut() && g_zoomLevel < 1.0f;
}

/* A pillbox or base met during the ground pass, held back so its marker goes
 * on after the ground: a marker runs a little past its square, and the next
 * square's fill would clip it. The map holds at most MAX_PILLS + MAX_BASES of
 * them and the viewport shows a window onto that, so the list cannot fill -
 * but it is flushed and restarted if it ever does rather than trusted. */
typedef struct {
    int        mx, my;
    bool       isBase;
    SDL_FColor colour;
} LvItemMarker;

#define LV_ITEM_MARKER_MAX 32

/* The blend mode belongs here rather than at the call sites: a marker's
 * outline layer is translucent, and drawn without it the outline comes out
 * opaque black and the markers gain a hard border. map_markers.h asks a
 * caller drawing a run of markers to set it once round the lot, and this is
 * that caller - both the overflow flush inside the ground pass and the batch
 * after it come through here, so neither can be the one that forgets. */
static void lvDrawItemMarkers(const LvItemMarker *hits, int count,
                              BYTE zoomFactor) {
    SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
    int i;

    if (count <= 0) return;
    SDL_GetRenderDrawBlendMode(sdlRenderer, &oldBlend);
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);
    for (i = 0; i < count; i++) {
        float side = (float)(zoomFactor * TILE_SIZE_X);
        float cx = (float)(zoomFactor * (hits[i].mx * TILE_SIZE_X)) + side / 2.0f;
        float cy = (float)(zoomFactor * (hits[i].my * TILE_SIZE_Y)) + side / 2.0f;
        /* The same fractions of a square the overview uses, so an item is the
           same size relative to its square in both. */
        if (hits[i].isBase) {
            mapMarkerBase(sdlRenderer, cx, cy,
                          SDL_max(2.5f, side * 0.42f), hits[i].colour);
        } else {
            mapMarkerPill(sdlRenderer, cx, cy,
                          SDL_max(2.0f, side * 0.36f), hits[i].colour);
        }
    }
    SDL_SetRenderDrawBlendMode(sdlRenderer, oldBlend);
}

/* --- Scenario map markers ---------------------------------------------
 * The game draws these through scnMarkerDraw (src/gui/sdl3/scenario_marker.c),
 * whose header brings in the game's screentank.h; its screenTanks is not the
 * viewer's, so no viewer file can include it. This is the same drawing, with
 * the same numbers, over the same palette. A change to how the game's marker
 * looks is a change here too. */

/* The scenario palette, from scenario_panel_draw.cpp. Declared here rather
   than through scenario_panel_draw.h: that header's timer needs the game's
   global.h, and lv_global.h shares its include guard, so in this file the
   game's one is never read. */
bool scnPanelColourRGBA(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b,
                        uint8_t *a);

/* How much of the colour shows at the bottom and the top of a breath, and one
   breath in milliseconds. */
#define LV_SCN_MARKER_ALPHA_LOW  0.45f
#define LV_SCN_MARKER_ALPHA_HIGH 0.95f
#define LV_SCN_MARKER_BREATH_MS  1600.0f

/* The outline's thickness, and the pointer's height, width and gap above the
   square, as fractions of a map square. */
#define LV_SCN_MARKER_STROKE    0.10f
#define LV_SCN_MARKER_POINT_H   0.38f
#define LV_SCN_MARKER_POINT_W   0.30f
#define LV_SCN_MARKER_POINT_GAP 0.12f

void lv_drawScnMarker(SDL_Renderer *renderer, BYTE colour,
                      float cx, float cy, float tileW, float tileH,
                      uint32_t nowMs) {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
    float phase, breath, halfW, halfH, stroke, height, halfPoint, tipY;
    Uint8 ca;
    int steps, rows, i;

    if (renderer == NULL || tileW <= 0.0f || tileH <= 0.0f) return;
    if (!scnPanelColourRGBA(colour, &r, &g, &b, &a)) return;

    /* A cosine, so the turn at each end breathes rather than ticks. */
    phase  = fmodf((float)nowMs, LV_SCN_MARKER_BREATH_MS) /
             LV_SCN_MARKER_BREATH_MS;
    breath = 0.5f - 0.5f * cosf(phase * 6.2831853f);
    ca = (Uint8)((float)a * (LV_SCN_MARKER_ALPHA_LOW +
                 (LV_SCN_MARKER_ALPHA_HIGH - LV_SCN_MARKER_ALPHA_LOW) * breath));

    SDL_GetRenderDrawBlendMode(renderer, &oldBlend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, r, g, b, ca);

    /* The outline, as nested one-pixel rects: SDL has no stroke width. */
    halfW  = tileW * 0.5f;
    halfH  = tileH * 0.5f;
    stroke = tileH * LV_SCN_MARKER_STROKE;
    steps  = (int)(stroke + 0.5f);
    if (steps < 1) steps = 1;
    for (i = 0; i < steps; i++) {
        SDL_FRect rect;
        rect.x = cx - halfW + (float)i;
        rect.y = cy - halfH + (float)i;
        rect.w = tileW - (float)(2 * i);
        rect.h = tileH - (float)(2 * i);
        if (rect.w <= 0.0f || rect.h <= 0.0f) break;
        SDL_RenderRect(renderer, &rect);
    }

    /* The pointer above it, tip down, filled a row at a time. */
    height    = tileH * LV_SCN_MARKER_POINT_H;
    halfPoint = tileW * LV_SCN_MARKER_POINT_W * 0.5f;
    tipY      = cy - halfH - tileH * LV_SCN_MARKER_POINT_GAP;
    rows      = (int)(height + 0.5f);
    if (rows < 1) rows = 1;
    for (i = 0; i < rows; i++) {
        float t = (float)i / (float)rows;
        float w = halfPoint * (1.0f - t);
        float y = tipY - height + (float)i;
        SDL_RenderLine(renderer, cx - w, y, cx + w, y);
    }

    SDL_SetRenderDrawBlendMode(renderer, oldBlend);
}

/* --- Declared regions -------------------------------------------------
 * The regions a recording's scripts.json declares, outlined with their names
 * while Options -> Regions is on. The colour and stroke are in
 * lv_region_rect.h, beside the placement both views share. */

SDL_Texture *lv_drawRegionName(SDL_Renderer *renderer, TTF_Font *font,
                               const char *name, int *outW, int *outH) {
    SDL_Color colour = { LV_REGION_COLOUR_R, LV_REGION_COLOUR_G,
                         LV_REGION_COLOUR_B, 255 };
    SDL_Surface *surface;
    SDL_Texture *texture;

    if (outW != NULL) *outW = 0;
    if (outH != NULL) *outH = 0;
    if (renderer == NULL || font == NULL || name == NULL || name[0] == '\0') {
        return NULL;
    }
    surface = TTF_RenderText_Blended(font, name, 0, colour);
    if (surface == NULL) return NULL;
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture != NULL) {
        if (outW != NULL) *outW = surface->w;
        if (outH != NULL) *outH = surface->h;
    }
    SDL_DestroySurface(surface);
    return texture;
}

void lv_drawRegion(SDL_Renderer *renderer, float x, float y, float w, float h,
                   float tileH, SDL_Texture *name, int nameW, int nameH) {
    SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
    int steps, i;

    if (renderer == NULL || w <= 0.0f || h <= 0.0f) return;

    /* The outline, as nested one-pixel rects inside the region's squares:
       SDL has no stroke width. */
    steps = (int)(tileH * LV_REGION_STROKE + 0.5f);
    if (steps < 1) steps = 1;
    SDL_GetRenderDrawBlendMode(renderer, &oldBlend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, LV_REGION_COLOUR_R, LV_REGION_COLOUR_G,
                           LV_REGION_COLOUR_B, 255);
    for (i = 0; i < steps; i++) {
        SDL_FRect rect;
        rect.x = x + (float)i;
        rect.y = y + (float)i;
        rect.w = w - (float)(2 * i);
        rect.h = h - (float)(2 * i);
        if (rect.w <= 0.0f || rect.h <= 0.0f) break;
        SDL_RenderRect(renderer, &rect);
    }

    /* The name just inside the corner, on a dark box so it reads over any
       ground. A long name runs past a narrow region rather than being cut to
       nothing; the view's own edge is what clips it. */
    if (name != NULL && nameW > 0 && nameH > 0) {
        SDL_FRect back, dst;
        dst.x = x + (float)steps + 1.0f;
        dst.y = y + (float)steps;
        dst.w = (float)nameW;
        dst.h = (float)nameH;
        back.x = dst.x - 1.0f;
        back.y = dst.y;
        back.w = dst.w + 2.0f;
        back.h = dst.h;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, LV_REGION_LABEL_BACK);
        SDL_RenderFillRect(renderer, &back);
        SDL_RenderTexture(renderer, name, NULL, &dst);
    }
    SDL_SetRenderDrawBlendMode(renderer, oldBlend);
}

/* Embed mode: a host that already owns an ImGui frame draws the world
 * texture itself, so the two framebuffer blits must not run, the viewer's
 * menu bar does not exist, and the tile grid sizes against the host's
 * image rect instead of the window. */
static int g_embedded   = 0;
static int g_embedViewW = 0;
static int g_embedViewH = 0;


/* Last drawn map positions for dirty rect optimization.
 * Sized for the maximum map size (255 tiles in each direction). */
int lv_drawLast[256][256];

/* Access team colours and useTeamColours through LogViewerState */

/* Function prototypes */
BYTE lv_screenGetPillTeam(BYTE x, BYTE y, BYTE *pillHealth);
BYTE lv_screenGetBaseTeam(BYTE x, BYTE y);
extern void lv_windowNeedRedraw(void);

BYTE lv_windowGetZoomFactor(void) { return 1; }

void lv_drawDirtyScreen(void) {
    int count, count2;
    for (count = 0; count < 256; count++) {
        for (count2 = 0; count2 < 256; count2++) {
            lv_drawLast[count][count2] = 10000;
        }
    }
}

float lv_drawGetZoomLevel(void) {
    return g_zoomLevel;
}

void lv_drawSetEmbedded(int enabled) {
    g_embedded = enabled ? 1 : 0;
}

void lv_drawSetEmbedViewport(int w, int h) {
    g_embedViewW = w;
    g_embedViewH = h;
}

void lv_drawGetGameTargetSize(int *outW, int *outH) {
    if (outW != NULL) *outW = targetWidth;
    if (outH != NULL) *outH = targetHeight;
}

/* Vertical offset the game area starts at inside the viewer's own window.
 * Embedded there is no viewer menu bar, and lv_imgui_get_menu_bar_height()
 * would read — and permanently cache — the host's frame height instead.
 *
 * Floored to a whole pixel: ImGui derives the height from the font size and
 * frame padding, so off macOS (where the native NSMenu makes it 0) it lands
 * on fractions. The blit origin has to be an integer or every pixel of the
 * world sits on a half-texel and gets resampled. Floor rather than round so
 * this matches the (int) casts the window-sizing paths in logviewer.c and
 * lv_drawApplyZoomStep already apply to the same number. */
static float lvDrawMenuBarOffset(void) {
    if (g_embedded) return 0.0f;
    return SDL_floorf(lv_imgui_get_menu_bar_height());
}

/* Renderer pixels per window point.
 *
 * The window carries SDL_WINDOW_HIGH_PIXEL_DENSITY, so on a Retina display the
 * framebuffer is larger than the window's point size. Everything that reasons
 * in points stays in points — the tile-count fit against the window, the zoom
 * anchor, the mouse-to-texture conversions — and only what is painted straight
 * to the framebuffer scales by this. Embedded, the host owns the window and
 * does its own blitting, so nothing here should scale. */
static float lvDrawPixelScale(void) {
    float d;
    if (g_embedded || sdlWindow == NULL) return 1.0f;
    d = SDL_GetWindowPixelDensity(sdlWindow);
    return (d > 0.0f) ? d : 1.0f;
}

static void lvDrawTankLabelsOverBlit(const SDL_FRect *src, const SDL_FRect *dst);

/* Paint the world render target into the viewer's own window.
 *
 * Sampling: at zoom >= 1 the blit is a whole-number magnification of pixel
 * art, so NEAREST keeps the tile edges hard. The target is created without a
 * scale mode and SDL3's renderer default is LINEAR, which is what smeared
 * every zoom step. Below 1x the blit is a downscale at a non-integer ratio
 * (0.75x is 1.33:1) where NEAREST drifts the per-tile sampling phase and
 * makes identical grass tiles look different across the map, so LINEAR's 2x2
 * average is the right filter there. map_preview_view.cpp splits the same way
 * for the same reason.
 *
 * Set per blit, not once at creation: the target is recreated on resize
 * (lv_drawResizeTarget) and the correct mode depends on the current zoom. */
static void lvDrawBlitTargetToWindow(void) {
    LogViewerState *lv = lv_screenGetState();
    BYTE zoomFactor = lv_windowGetZoomFactor();
    float pxScale = lvDrawPixelScale();
    /* The magnification actually applied to the framebuffer. The user picks
     * g_zoomLevel against a window measured in points; the density folds in on
     * top so the world keeps its physical size and gains pixels instead of
     * being stretched by the compositor. */
    float blitZoom = g_zoomLevel * pxScale;
    SDL_FRect srcRect, dstRect;

    /* Sub-tile pan: the texture target is sized (sizeX+1, sizeY+1) tiles and
     * gets painted with a 1-tile margin on every edge. The srcRect picks the
     * visible (sizeX, sizeY)-tile slice starting at the sub-pixel offset, so
     * the leading edge reveals the margin instead of empty pixels. */
    srcRect.x = (float)lv->subPxX;
    srcRect.y = (float)lv->subPxY;
    srcRect.w = (float)(lv_screenGetSizeX() * TILE_SIZE_X);
    srcRect.h = (float)(lv_screenGetSizeY() * TILE_SIZE_Y);

    /* SDL expects client-area coordinates (0,0), not screen coordinates, and
     * the menu bar offset leaves the top strip to ImGui — which renders its own
     * geometry at the framebuffer scale, so that offset converts to pixels too.
     * Floored after scaling: the origin has to be a whole pixel or every pixel
     * of the world sits on a half-texel. The texture target stays at native
     * (1x) tile resolution. */
    dstRect.x = 0.0f;
    dstRect.y = SDL_floorf(lvDrawMenuBarOffset() * pxScale);
    dstRect.w = (float)(zoomFactor * lv_screenGetSizeX() * TILE_SIZE_X) * blitZoom;
    dstRect.h = (float)(zoomFactor * lv_screenGetSizeY() * TILE_SIZE_Y) * blitZoom;

    SDL_SetTextureScaleMode(textureTarget,
                            (blitZoom >= 1.0f) ? SDL_SCALEMODE_NEAREST
                                               : SDL_SCALEMODE_LINEAR);
    SDL_RenderTexture(sdlRenderer, textureTarget, &srcRect, &dstRect);
    if (!lv->gameView) {
        lvDrawTankLabelsOverBlit(&srcRect, &dstRect);
    }
}

/* Apply a stepped zoom change with the map tile under (mouseScreenX,
 * mouseScreenY) anchored — that tile stays under the cursor. The screen
 * coordinates are in window pixels (the same space SDL events report),
 * so we subtract the ImGui menu bar height to reach the game area. */
static void lv_drawApplyZoomStep(int newStepIndex, int mouseScreenX, int mouseScreenY) {
    float oldZoom, newZoom;
    int   windowW = 0, windowH = 0, gameH;
    int   menuH;
    BYTE  oldOffX = 0, oldOffY = 0;
    int   curTilesX, curTilesY;
    float gx, gy;
    float oldEffective, newEffective;
    float anchorTileX, anchorTileY;
    int   newTilesX, newTilesY;
    int   maxOffX, maxOffY, newOffX, newOffY;
    float newOffXf, newOffYf;
    BYTE  zf;
    float maxGx, maxGy;
    int   curTexW, curTexH;
    int   gameW;

    if (newStepIndex < 0) newStepIndex = 0;
    if (newStepIndex >= ZOOM_STEP_COUNT) newStepIndex = ZOOM_STEP_COUNT - 1;
    if (newStepIndex == g_zoomStepIndex) return;

    oldZoom = g_zoomLevel;
    newZoom = g_zoomSteps[newStepIndex];

    if (g_embedded) {
        /* Size against the host's image rect — the window belongs to the
         * host and is far larger than the area the reel occupies. */
        windowW = g_embedViewW;
        windowH = g_embedViewH;
    } else if (sdlWindow != NULL) {
        SDL_GetWindowSize(sdlWindow, &windowW, &windowH);
    }
    if (windowW < 1) windowW = TILE_SIZE_X;
    if (windowH < 1) windowH = TILE_SIZE_Y;
    menuH = (int)lvDrawMenuBarOffset();
    gameH = windowH - menuH;
    if (gameH < 1) gameH = 1;

    /* Compute map tile under cursor at the OLD zoom. The blit width is
     * curTilesX * TILE_SIZE_X * oldZoom, and the cursor screen X covers
     * that same range, so each on-screen pixel == 1/oldZoom texture
     * pixels. We clamp to the rendered game extents so the anchor stays
     * within the loaded map even when the cursor sits outside it. */
    zf = lv_windowGetZoomFactor();
    curTilesX = lv_screenGetSizeX();
    curTilesY = lv_screenGetSizeY();
    lv_screenGetOffsets(&oldOffX, &oldOffY);
    int oldSubX = 0, oldSubY = 0;
    lv_screenGetSubOffset(&oldSubX, &oldSubY);

    oldEffective = (float)(zf * TILE_SIZE_X) * oldZoom;
    if (oldEffective <= 0.0f) oldEffective = (float)TILE_SIZE_X;

    gx = (float)mouseScreenX;
    gy = (float)mouseScreenY - (float)menuH;
    if (gx < 0.0f) gx = 0.0f;
    if (gy < 0.0f) gy = 0.0f;

    /* Use the actual rendered area (texture target * oldZoom) to clamp. */
    curTexW = targetWidth;
    curTexH = targetHeight;
    if (curTexW < 1) curTexW = curTilesX * TILE_SIZE_X;
    if (curTexH < 1) curTexH = curTilesY * TILE_SIZE_Y;
    maxGx = (float)curTexW * oldZoom;
    maxGy = (float)curTexH * oldZoom;
    if (gx > maxGx) gx = maxGx;
    if (gy > maxGy) gy = maxGy;

    /* Include the current sub-tile pan so the cursor anchors on the same
     * map pixel even when a partial drag is in flight. */
    anchorTileX = (float)oldOffX + (float)oldSubX / (float)TILE_SIZE_X + gx / oldEffective;
    anchorTileY = (float)oldOffY + (float)oldSubY / (float)TILE_SIZE_Y + gy / oldEffective;

    /* Compute new viewport tile count so the blit fills (approximately)
     * the available window area at the new zoom. Use the same nearest-
     * tile rounding the resize handler uses, just in zoomed pixel space. */
    gameW = windowW;
    newEffective = (float)(zf * TILE_SIZE_X) * newZoom;
    if (newEffective <= 0.0f) newEffective = (float)TILE_SIZE_X;
    newTilesX = (int)(((float)gameW + newEffective * 0.5f) / newEffective);
    newTilesY = (int)(((float)gameH + newEffective * 0.5f) / newEffective);
    if (newTilesX < 1) newTilesX = 1;
    if (newTilesY < 1) newTilesY = 1;
    if (newTilesX > 255) newTilesX = 255;
    if (newTilesY > 255) newTilesY = 255;

    /* Place the anchor tile back under the cursor at the new zoom. */
    newOffXf = anchorTileX - gx / newEffective;
    newOffYf = anchorTileY - gy / newEffective;

    maxOffX = 255 - newTilesX;
    maxOffY = 255 - newTilesY;
    if (maxOffX < 0) maxOffX = 0;
    if (maxOffY < 0) maxOffY = 0;
    newOffX = (int)(newOffXf + 0.5f);
    newOffY = (int)(newOffYf + 0.5f);
    if (newOffX < 0) newOffX = 0;
    if (newOffY < 0) newOffY = 0;
    if (newOffX > maxOffX) newOffX = maxOffX;
    if (newOffY > maxOffY) newOffY = maxOffY;

    /* Commit. lv_screenSetSizeX/Y reallocate the screen buffer and call
     * lv_drawResizeRenderTarget(); after that we set the new offset and
     * request a full redraw so the texture target gets repopulated. */
    g_zoomStepIndex = newStepIndex;
    g_zoomLevel = newZoom;

    if (newTilesX != curTilesX) lv_screenSetSizeX((BYTE)newTilesX);
    if (newTilesY != curTilesY) lv_screenSetSizeY((BYTE)newTilesY);

    lv_screenSetOffset((BYTE)newOffX, (BYTE)newOffY);
    /* Zoom anchors on the cursor's tile, not its sub-tile pixel; reset
     * sub-pan so the new viewport doesn't carry an offset from the
     * pre-zoom sub-pixel position. */
    lv_screenSetSubOffset(0, 0);
    lv_drawDirtyScreen();
    lv_windowNeedRedraw();
}

void lv_drawZoomIn(int mouseScreenX, int mouseScreenY) {
    lv_drawApplyZoomStep(g_zoomStepIndex + 1, mouseScreenX, mouseScreenY);
}

void lv_drawZoomOut(int mouseScreenX, int mouseScreenY) {
    lv_drawApplyZoomStep(g_zoomStepIndex - 1, mouseScreenX, mouseScreenY);
}

int lv_drawGetZoomStepIndex(void) {
    return g_zoomStepIndex;
}

int lv_drawGetZoomStepCount(void) {
    return ZOOM_STEP_COUNT;
}

float lv_drawGetZoomStepValue(int index) {
    if (index < 0 || index >= ZOOM_STEP_COUNT) return 1.0f;
    return g_zoomSteps[index];
}

void lv_drawSetZoomStep(int stepIndex, int mouseScreenX, int mouseScreenY) {
    lv_drawApplyZoomStep(stepIndex, mouseScreenX, mouseScreenY);
}

/* Set the zoom step and nothing else: no viewport resize, no cursor anchoring,
 * no screen-state writes. The step is a process-wide static, so the embedded
 * reel saves what it found, forces its own, and puts the original back rather
 * than leaving a later full-window session opening at the reel's zoom. The
 * anchored path is the wrong tool for that: it would resize the tile grid to
 * the reel's rect, and at restore time it would also be unsafe — it writes
 * through decoder state the embed has already destroyed. */
void lv_drawSetZoomStepIndexRaw(int index) {
    if (index < 0) index = 0;
    if (index >= ZOOM_STEP_COUNT) index = ZOOM_STEP_COUNT - 1;
    g_zoomStepIndex = index;
    g_zoomLevel = g_zoomSteps[index];
}

/* Build the unified tile atlas (SVG/PNG/BMP combined sheet) at scale 1.
 * Returns NULL on failure. The log viewer always uses scale 1 — the
 * blit-time scaling is in the texture target, not the source atlas. */
static SDL_Texture *buildTileAtlas(SDL_Renderer *renderer) {
    SDL_Surface *sheet = tileLoaderBuildSheet(TILE_SIZE_X);
    if (!sheet) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, sheet);
    SDL_DestroySurface(sheet);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    return tex;
}

/*********************************************************
*NAME:          lv_drawSetupWithHandles
*PURPOSE:
*  Sets up drawing using externally-provided window/renderer
*  (embedded mode). Does NOT create SDL window/renderer.
*  Loads textures, render target, and font.
*RETURNS:
*  TRUE on success, FALSE on failure
*********************************************************/
BYTE lv_drawSetupWithHandles(SDL_Window *window, SDL_Renderer *renderer) {
    lv_drawDirtyScreen();

    sdlWindow = window;
    sdlRenderer = renderer;
    ownsWindow = FALSE;

    /* +1 tile of margin in each dimension. The blit clips the margin
     * with a srcRect so the visible window is sizeX*TILE_SIZE_X by
     * sizeY*TILE_SIZE_Y; the margin only fills in the leading-edge
     * pixels revealed by sub-tile scrolling. */
    targetWidth  = (lv_screenGetSizeX() + 1) * TILE_SIZE_X;
    targetHeight = (lv_screenGetSizeY() + 1) * TILE_SIZE_Y;
    textureTarget = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET, targetWidth, targetHeight);
    if (!textureTarget) {
        return FALSE;
    }
    /* Tile background uses the unified atlas (SVG/PNG with skin.bmp
     * fallback) shared with the game and map editor. Tank, boat, and
     * pill/base sprites stay on their own BMPs because they use the
     * 16-row team-colour palette indexed via lv->tc[], which the
     * unified atlas does not provide. */
    textureTiles = buildTileAtlas(sdlRenderer);
    {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";
        char bmpPath[1024];

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/tanks.bmp", basePath);
        textureTanks = sdlLoadBmpSheetAsTexture(sdlRenderer, bmpPath,
                                              TILE_SIZE_X, TILE_SIZE_Y);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/boats.bmp", basePath);
        textureBoats = sdlLoadBmpSheetAsTexture(sdlRenderer, bmpPath,
                                              TILE_SIZE_X, TILE_SIZE_Y);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/items.bmp", basePath);
        textureItems = sdlLoadBmpSheetAsTexture(sdlRenderer, bmpPath,
                                              TILE_SIZE_X, TILE_SIZE_Y);
    }

    if (!textureTiles || !textureTanks || !textureBoats || !textureItems) {
        return FALSE;
    }
    {
        const char *fontBase = SDL_GetBasePath();
        if (!fontBase) fontBase = "./";
        char fontPath[1024];
        SDL_snprintf(fontPath, sizeof(fontPath), "%sdata/fonts/SarasaMonoSlabJ-Regular.ttf", fontBase);
        labelFont = TTF_OpenFont(fontPath, 10);
    }

    mapViewInit();
    return TRUE;
}

BYTE lv_drawSetup(void) {
    int width, height;

    ownsWindow = TRUE;
    lv_drawDirtyScreen();

    /* If already initialized, skip recreation entirely.
     * ImGui depends on the SDL window/renderer staying valid. */
    if (sdlRenderer != NULL && sdlWindow != NULL) {

        return TRUE;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error initializing SDL video", NULL);
        return FALSE;
    }
    if (!TTF_Init()) {
        SDL_Quit();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error initializing SDL_ttf", NULL);
        return FALSE;
    }
    /* Calculate window dimensions from screen tile size + menu bar space */
    #define IMGUI_MENU_BAR_HEIGHT 25
    width  = lv_screenGetSizeX() * TILE_SIZE_X;
    height = lv_screenGetSizeY() * TILE_SIZE_Y + IMGUI_MENU_BAR_HEIGHT;

    /* HIGH_PIXEL_DENSITY: without it SDL pins the layer's contentsScale to 1
     * (SDL_cocoametalview.m), so on a Retina display the whole window renders at
     * point resolution and the compositor bilinear-upscales it — menu bar,
     * dialogs and world alike. ImGui reads the density itself and rasterizes
     * its text to match; the world blit scales by lvDrawPixelScale(). */
    sdlWindow = SDL_CreateWindow("WinBolo Log Viewer", width, height,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (sdlWindow == NULL) {
        TTF_Quit(); SDL_Quit();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error creating SDL window", NULL);
        return FALSE;
    }
    /* Set window icon from data/icons/logviewerr-icon.png (used on Linux/Windows
       taskbar; macOS uses the .icns bundle). */
    {
        SDL_IOStream *io = SDL_IOFromFile("data/icons/logviewerr-icon.png", "rb");
        if (!io) {
            const char *basePath = SDL_GetBasePath();
            if (!basePath) basePath = "./";
            char iconPath[1024];
            SDL_snprintf(iconPath, sizeof(iconPath), "%sdata/icons/logviewerr-icon.png", basePath);
            io = SDL_IOFromFile(iconPath, "rb");
        }
        if (io) {
            Sint64 fileSize = SDL_GetIOSize(io);
            if (fileSize > 0) {
                unsigned char *buf = (unsigned char *)SDL_malloc((size_t)fileSize);
                if (buf) {
                    SDL_ReadIO(io, buf, (size_t)fileSize);
                    int iw, ih, ch;
                    unsigned char *pixels = stbi_load_from_memory(buf, (int)fileSize, &iw, &ih, &ch, 4);
                    SDL_free(buf);
                    if (pixels) {
                        SDL_Surface *icon = SDL_CreateSurfaceFrom(iw, ih, SDL_PIXELFORMAT_RGBA32, pixels, iw * 4);
                        if (icon) {
                            SDL_SetWindowIcon(sdlWindow, icon);
                            SDL_DestroySurface(icon);
                        }
                        stbi_image_free(pixels);
                    }
                }
            }
            SDL_CloseIO(io);
        }
    }

    sdlRenderer = SDL_CreateRenderer(sdlWindow, NULL);
    if (!sdlRenderer) {
        SDL_DestroyWindow(sdlWindow); TTF_Quit(); SDL_Quit();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error creating SDL renderer", NULL);
        return FALSE;
    }
    SDL_SetRenderVSync(sdlRenderer, 1);

    /* See lv_drawSetupWithHandles for the +1 margin rationale. */
    targetWidth  = (lv_screenGetSizeX() + 1) * TILE_SIZE_X;
    targetHeight = (lv_screenGetSizeY() + 1) * TILE_SIZE_Y;
    textureTarget = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET, targetWidth, targetHeight);
    if (!textureTarget) {
        lv_drawCleanup();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error creating render target", NULL);
        return FALSE;
    }
    /* See lv_drawSetupWithHandles for the tile-vs-tank/items split. */
    textureTiles = buildTileAtlas(sdlRenderer);
    {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";
        char bmpPath[1024];

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/tanks.bmp", basePath);
        textureTanks = sdlLoadBmpSheetAsTexture(sdlRenderer, bmpPath,
                                              TILE_SIZE_X, TILE_SIZE_Y);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/boats.bmp", basePath);
        textureBoats = sdlLoadBmpSheetAsTexture(sdlRenderer, bmpPath,
                                              TILE_SIZE_X, TILE_SIZE_Y);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/items.bmp", basePath);
        textureItems = sdlLoadBmpSheetAsTexture(sdlRenderer, bmpPath,
                                              TILE_SIZE_X, TILE_SIZE_Y);
    }

    if (!textureTiles || !textureTanks || !textureBoats || !textureItems) {

        lv_drawCleanup();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error loading sprite bitmaps from data/ directory", NULL);
        return FALSE;
    }
    {
        const char *fontBase = SDL_GetBasePath();
        if (!fontBase) fontBase = "./";
        char fontPath[1024];
        SDL_snprintf(fontPath, sizeof(fontPath), "%sdata/fonts/SarasaMonoSlabJ-Regular.ttf", fontBase);
        labelFont = TTF_OpenFont(fontPath, 10);
    }

    mapViewInit();
    return TRUE;
}

static void lvFlushRegionNames(void);
static void lvFlushTankLabels(void);

void lv_drawCleanup(void) {
    /* First, while the renderer the names were made on and the font they
       were made from are both still there. */
    lvFlushRegionNames();
    lvFlushTankLabels();
    s_tankLabelCount = 0;
    /* A GIF export that was still running when the session ended must not
       leave the next session drawing its names into the target. */
    s_tankLabelsInTarget = FALSE;
    if (textureTiles) { SDL_DestroyTexture(textureTiles); textureTiles = NULL; }
    if (textureTanks) { SDL_DestroyTexture(textureTanks); textureTanks = NULL; }
    if (textureBoats) { SDL_DestroyTexture(textureBoats); textureBoats = NULL; }
    if (textureItems) { SDL_DestroyTexture(textureItems); textureItems = NULL; }
    if (textureTarget) { SDL_DestroyTexture(textureTarget); textureTarget = NULL; }
    if (labelFont) { TTF_CloseFont(labelFont); labelFont = NULL; }
    if (ownsWindow == TRUE) {
        /* Only destroy renderer/window and quit SDL if we created them */
        if (sdlRenderer) { SDL_DestroyRenderer(sdlRenderer); sdlRenderer = NULL; }
        if (sdlWindow) { SDL_DestroyWindow(sdlWindow); sdlWindow = NULL; }
        TTF_Quit();
        /* Quit SDL video subsystem only (audio is used by Sound.c) */
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    } else {
        /* Embedded mode: don't destroy the main app's window/renderer */
        sdlRenderer = NULL;
        sdlWindow = NULL;
    }
    sdlInitialized = 0;  /* Reset initialization flag */
}

/*********************************************************
*NAME:          lv_drawResizeRenderTarget
*PURPOSE:
*  Recreates the render target texture when screen size changes.
*  This is critical for correct mouse coordinate mapping -
*  if the texture size doesn't match the screen size, SDL
*  will scale the texture, causing coordinate drift.
*********************************************************/
void lv_drawResizeRenderTarget(void) {
    int newWidth = (lv_screenGetSizeX() + 1) * TILE_SIZE_X;
    int newHeight = (lv_screenGetSizeY() + 1) * TILE_SIZE_Y;
    
    if (sdlRenderer == NULL) return;
    
    /* Check if size actually changed */
    if (newWidth == targetWidth && newHeight == targetHeight) {
        return;
    }
    
    /* Destroy old texture */
    if (textureTarget) {
        SDL_DestroyTexture(textureTarget);
        textureTarget = NULL;
    }
    
    /* Create new texture with correct size */
    targetWidth = newWidth;
    targetHeight = newHeight;
    textureTarget = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_TARGET, targetWidth, targetHeight);
    if (!textureTarget) {
        return;
    }
    
    /* Mark screen as dirty to force redraw */
    lv_drawDirtyScreen();
}

int lv_drawGetTargetWidth(void) { return targetWidth; }
int lv_drawGetTargetHeight(void) { return targetHeight; }

/* Static splash texture - loaded once and cached */
static SDL_Texture *textureSplash = NULL;
static int splashWidth = 800;
static int splashHeight = 600;

/*********************************************************
*NAME:          lv_drawSplashForImGui
*PURPOSE:
*  Draws the splash screen for the ImGui rendering loop.
*  This function clears the screen and draws the splash,
*  but does NOT present. ImGui will present after drawing UI.
*
*  Called from the main loop BEFORE ImGui renders.
*********************************************************/
void lv_drawSplashForImGui(void) {
    int x = 0, y = 0;
    SDL_FRect dstRect;
    int windowWidth, windowHeight;
    
    /* Ensure we're rendering to the screen, not to a texture target */
    SDL_SetRenderTarget(sdlRenderer, NULL);

    /* Centre in framebuffer pixels, and scale the image by the same density so
     * it keeps its physical size rather than shrinking to a quarter of the
     * window on a Retina display. */
    float pxScale = lvDrawPixelScale();
    SDL_GetRenderOutputSize(sdlRenderer, &windowWidth, &windowHeight);
    int splashW = (int)(splashWidth * pxScale);
    int splashH = (int)(splashHeight * pxScale);

    /* Clear the screen first */
    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
    
    /* Load splash texture if not already loaded */
    if (textureSplash == NULL) {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";
        char splashPath[1024];
        SDL_snprintf(splashPath, sizeof(splashPath), "%sdata/splash.bmp", basePath);
        textureSplash = sdlLoadBmpAsTexture(sdlRenderer, splashPath);
    }
    
    if (textureSplash) {
        /* Center the splash image */
        if (windowWidth > splashW) x = (windowWidth - splashW) / 2;
        if (windowHeight > splashH) y = (windowHeight - splashH) / 2;

        dstRect.x = (float)x;
        dstRect.y = (float)y;
        dstRect.w = (float)splashW;
        dstRect.h = (float)splashH;
        SDL_RenderTexture(sdlRenderer, textureSplash, NULL, &dstRect);
    }
    
    /* NOTE: Do NOT call SDL_RenderPresent here - ImGui will present after drawing UI */
}

/*********************************************************
*NAME:          lv_drawCleanupSplash
*PURPOSE:
*  Cleans up the cached splash texture
*********************************************************/
void lv_drawCleanupSplash(void) {
    if (textureSplash) {
        SDL_DestroyTexture(textureSplash);
        textureSplash = NULL;
    }
}

static void drawRenderTexture(SDL_Texture *texture, int srcX, int srcY, int srcW, int srcH, int dstX, int dstY) {
    SDL_FRect srcRect = { (float)srcX, (float)srcY, (float)srcW, (float)srcH };
    SDL_FRect dstRect = { (float)dstX, (float)dstY, (float)srcW, (float)srcH };
    SDL_RenderTexture(sdlRenderer, texture, &srcRect, &dstRect);
}

/* The markers the followed player would see, into the overview's render
 * target, whose square (0, 0) is the viewport's top-left. A marker's square
 * and the one above it, where the pointer sits, are marked for redraw the
 * way a tank's are, so the next frame repaints the ground under them before
 * drawing them again and a marker that has moved or gone leaves nothing. The
 * target is only redrawn when the replay moves or the view does, so a paused
 * replay holds its markers still. */
static void lvDrawScnMarkers(BYTE zoomFactor) {
    LogViewerState *lv = lv_screenGetState();
    float side = (float)(zoomFactor * TILE_SIZE_X);
    uint32_t now = (uint32_t)SDL_GetTicks();
    BYTE id;

    for (id = 0; id < SCN_MARKERS_MAX; id++) {
        BYTE mx, my, colour;
        int sx, sy;

        if (!lv_screenMarkerPlace(id, &mx, &my, &colour)) continue;
        sx = (int)mx - (int)lv->xOffset;
        sy = (int)my - (int)lv->yOffset;
        if (sx < 0 || sy < 0 || sx > lv_screenGetSizeX() ||
            sy > lv_screenGetSizeY()) {
            continue;
        }
        lv_drawScnMarker(sdlRenderer, colour,
                         (float)sx * side + side / 2.0f,
                         (float)sy * side + side / 2.0f, side, side, now);
        lv_drawLast[sx][sy] = 10000;
        if (sy > 0) lv_drawLast[sx][sy - 1] = 10000;
    }
}

/* Mark the viewport squares from (x0, y0) to (x1, y1) inclusive for redraw,
 * clipped to the render target's sizeX+1 by sizeY+1 squares. */
static void lvMarkSquares(int x0, int y0, int x1, int y1) {
    int lastX = (int)lv_screenGetSizeX();
    int lastY = (int)lv_screenGetSizeY();
    int x, y;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > lastX) x1 = lastX;
    if (y1 > lastY) y1 = lastY;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            lv_drawLast[x][y] = 10000;
        }
    }
}

/* The declared regions' names as the overview draws them, rendered once and
 * kept: the overview repaints often while playing, and a region's name does
 * not change. An entry is good while its name matches the region at its
 * index. The overview draws with one font at one size whatever the zoom, so
 * the name is the whole key. Every entry is freed by lv_drawCleanup, which is
 * where the font and the renderer they were made with go; a recording loaded
 * or closed in between leaves entries whose names still describe the
 * textures they hold, so they are kept for a region of the same name. */
typedef struct {
    SDL_Texture *tex;
    int          w, h;
    char         name[LV_SCRIPTS_REGION_NAME_LEN];
} LvRegionName;

static LvRegionName lvRegionNames[LV_SCRIPTS_REGIONS_MAX];

static void lvFlushRegionNames(void) {
    int i;

    for (i = 0; i < LV_SCRIPTS_REGIONS_MAX; i++) {
        if (lvRegionNames[i].tex != NULL) {
            SDL_DestroyTexture(lvRegionNames[i].tex);
        }
        lvRegionNames[i].tex     = NULL;
        lvRegionNames[i].w       = 0;
        lvRegionNames[i].h       = 0;
        lvRegionNames[i].name[0] = '\0';
    }
}

/* The recording's declared regions, into the overview's render target, while
 * Options -> Regions is on. The outline lies inside the region's border
 * squares and the name inside the squares at its top-left, and every one of
 * those is marked for redraw the way a marker's are: the next frame repaints
 * the ground under them before drawing them again, so a region that has gone -
 * the toggle off, or another recording loaded - leaves nothing. The toggle
 * also repaints the whole map (lv_imgui_toggle_regions). Not in the lobby's
 * reel, which draws a recording as background and has no menu to turn this
 * off from. */
static void lvDrawRegions(BYTE zoomFactor) {
    const LvScripts *scripts = lv_screenGetScripts();
    LogViewerState *lv = lv_screenGetState();
    float side = (float)(zoomFactor * TILE_SIZE_X);
    LvRegionView view;
    int count, i;

    if (!lv_g_show_regions || g_embedded || scripts == NULL) return;
    count = scripts->regionCount;
    if (count > LV_SCRIPTS_REGIONS_MAX) count = LV_SCRIPTS_REGIONS_MAX;
    if (count <= 0) return;

    view.mapX  = -(float)lv->xOffset * side;
    view.mapY  = -(float)lv->yOffset * side;
    view.tileW = side;
    view.tileH = side;
    view.viewX = 0.0f;
    view.viewY = 0.0f;
    view.viewW = (float)(lv_screenGetSizeX() + 1) * side;
    view.viewH = (float)(lv_screenGetSizeY() + 1) * side;

    for (i = 0; i < count; i++) {
        const LvScriptRegion *r = &scripts->regions[i];
        LvRegionName *label = &lvRegionNames[i];
        LvRegionRect rect;
        int nameW, nameH;
        int x0, y0, x1, y1;

        if (!lvRegionScreenRect(&view, r->x, r->y, r->w, r->h, &rect)) continue;
        if (label->tex == NULL || SDL_strcmp(label->name, r->name) != 0) {
            if (label->tex != NULL) SDL_DestroyTexture(label->tex);
            label->tex = lv_drawRegionName(sdlRenderer, labelFont, r->name,
                                           &label->w, &label->h);
            SDL_strlcpy(label->name, r->name, sizeof(label->name));
        }
        nameW = label->w;
        nameH = label->h;
        lv_drawRegion(sdlRenderer, rect.x, rect.y, rect.w, rect.h, side,
                      label->tex, nameW, nameH);

        /* The four sides, then the name with a square to spare for its
           inset. */
        x0 = (int)r->x - (int)lv->xOffset;
        y0 = (int)r->y - (int)lv->yOffset;
        x1 = x0 + (int)r->w - 1;
        y1 = y0 + (int)r->h - 1;
        lvMarkSquares(x0, y0, x1, y0);
        lvMarkSquares(x0, y1, x1, y1);
        lvMarkSquares(x0, y0, x0, y1);
        lvMarkSquares(x1, y0, x1, y1);
        if (nameW > 0) {
            lvMarkSquares(x0, y0,
                          x0 + (int)((float)nameW / side) + 1,
                          y0 + (int)((float)nameH / side) + 1);
        }
    }
}

void lv_drawMainScreen(screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullets, screenLgm *lgms, BYTE showPillLabels, BYTE showBaseLabels, int32_t srtDelay, BYTE isPillView, int edgeX, int edgeY, BYTE useCursor, BYTE cursorLeft, BYTE cursorTop) {
    bool done, isPill, isBase, shouldDraw;
    /* x/y must be wider than BYTE: the loop runs to lv_screenGetSizeX()/Y(),
     * which clamp to 255 at a fullscreen, zoomed-out viewport. A BYTE counter
     * would wrap 255->0 at the ++x/++y test and never exceed the bound, looping
     * forever (hard lockup). */
    int x, y;
    BYTE pos, zoomFactor, itc, pillHealth;
    int outputX, outputY;
    LogViewerState *lv = lv_screenGetState();

    (void)gs; (void)showPillLabels; (void)showBaseLabels; (void)srtDelay;
    (void)isPillView; (void)edgeX; (void)edgeY; (void)useCursor;
    (void)cursorLeft; (void)cursorTop;

    /* Game-view path: routes the same per-frame pointers lv_screenUpdate
     * built into the direct-render game-look frame. Serves both the
     * standalone viewer and the embedded "Watch a Log" flow — game-view
     * drives the active window (borrowed when embedded) for its duration;
     * lv_drawGameViewSetup/Teardown save and restore its size. */
    if (lv->gameView) {
        lv_drawGameViewFrame(value, mineView, tks, sBullets, lgms);
        return;
    }

    zoomFactor = lv_windowGetZoomFactor();

    /* The simplified view. Tiles are drawn at their native 16 px into the
       target and the blit downscales the lot, so below 1x a square lands on
       the window as eight to fourteen pixels and the sprite in it is a
       smudge; one flat map colour reads instead. The setting is the game's,
       out of the prefs document both apps share (loadPreferences).

       Only the ground. Tanks, pillboxes and bases keep their sprites,
       because this viewer colours them by team and the game's marker palette
       has two sides and a neutral - it cannot say which of sixteen teams
       owns a pillbox, which is most of what a recording is watched for. */
    bool simple = lvSimpleView();
    LvItemMarker itemHits[LV_ITEM_MARKER_MAX];
    int          itemHitCount = 0;
    if (simple != g_lastSimple) {
        g_lastSimple = simple;
        lv_drawDirtyScreen();
    }

    /* Save the caller's target rather than assuming the framebuffer: the
     * embedded host calls this from inside its own frame, which may already
     * be rendering to a target of its own. */
    SDL_Texture *prevTarget = SDL_GetRenderTarget(sdlRenderer);
    SDL_SetRenderTarget(sdlRenderer, textureTarget);

    for (x = 0, y = 0, done = FALSE; !done; ) {
        pos = lv_screenGetPos(value, x, y);
        shouldDraw = (lv_drawLast[x][y] == 10000) ||
                     (lv_screenIsMine(mineView, x, y) && lv_drawLast[x][y] != (0 - pos)) ||
                     (!lv_screenIsMine(mineView, x, y) && lv_drawLast[x][y] != pos);
        
        if (shouldDraw) {
            lv_drawLast[x][y] = pos;
            isPill = isBase = FALSE;
            
            if (pos == PILL_EVIL_15 || (pos >= PILL_EVIL_14 && pos <= PILL_EVIL_0) || (pos >= PILL_GOOD_15 && pos <= PILL_GOOD_0)) {
                isPill = TRUE;
                lv_drawLast[x][y] = 10000;
            }
            if (pos == BASE_GOOD || pos == BASE_NEUTRAL || pos == BASE_EVIL) {
                isBase = TRUE;
                lv_drawLast[x][y] = 10000;
            }
            
            if (simple) {
                /* The ground under everything, an item square included: a
                   pillbox or base answers with the ground beneath it, and its
                   marker goes on after the pass. */
                SDL_Color ground;
                if (mapColourTerrain(pos, &ground)) {
                    SDL_FRect square = {
                        (float)(zoomFactor * (x * TILE_SIZE_X)),
                        (float)(zoomFactor * (y * TILE_SIZE_Y)),
                        (float)(zoomFactor * TILE_SIZE_X),
                        (float)(zoomFactor * TILE_SIZE_Y)
                    };
                    SDL_SetRenderDrawColor(sdlRenderer, ground.r, ground.g,
                                           ground.b, ground.a);
                    SDL_RenderFillRect(sdlRenderer, &square);
                } else {
                    outputX = mapViewPosX[pos];
                    outputY = mapViewPosY[pos];
                    drawRenderTexture(textureTiles, outputX, outputY, zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y,
                        zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
                }

                if (isPill || isBase) {
                    if (itemHitCount == LV_ITEM_MARKER_MAX) {
                        lvDrawItemMarkers(itemHits, itemHitCount, zoomFactor);
                        itemHitCount = 0;
                    }
                    itemHits[itemHitCount].mx     = x;
                    itemHits[itemHitCount].my     = y;
                    itemHits[itemHitCount].isBase = (isBase != FALSE);
                    /* Team colours name the owner, which is what a recording
                       is watched for; without them the tile itself says which
                       side holds it, the way the game's markers do. */
                    if (lv->useTeamColours) {
                        itc = isBase ? lv_screenGetBaseTeam(x, y)
                                     : lv_screenGetPillTeam(x, y, &pillHealth);
                        itemHits[itemHitCount].colour = mapColourTeam(lv->tc[itc]);
                    } else {
                        MapColourItem kind = mapColourItemKind(pos);
                        if (kind == MAP_COLOUR_ITEM_PILL_GOOD ||
                            kind == MAP_COLOUR_ITEM_BASE_GOOD) {
                            itemHits[itemHitCount].colour = mapColourMarkerGood();
                        } else if (kind == MAP_COLOUR_ITEM_BASE_NEUTRAL) {
                            itemHits[itemHitCount].colour = mapColourMarkerNeutral();
                        } else {
                            itemHits[itemHitCount].colour = mapColourMarkerEvil();
                        }
                    }
                    itemHitCount++;
                }
            } else if (isPill && lv->useTeamColours) {
                itc = lv_screenGetPillTeam(x, y, &pillHealth);
                drawRenderTexture(textureItems, pillHealth * zoomFactor * TILE_SIZE_X, lv->tc[itc] * zoomFactor * TILE_SIZE_Y,
                    zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y, zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
            } else if (isBase && lv->useTeamColours) {
                itc = lv_screenGetBaseTeam(x, y);
                drawRenderTexture(textureItems, 16 * zoomFactor * TILE_SIZE_X, lv->tc[itc] * zoomFactor * TILE_SIZE_Y,
                    zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y, zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
            } else {
                outputX = mapViewPosX[pos];
                outputY = mapViewPosY[pos];
                drawRenderTexture(textureTiles, outputX, outputY, zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y,
                    zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
            }
            
            if (lv_screenIsMine(mineView, x, y)) {
                drawRenderTexture(textureTiles, zoomFactor * MINE_X, zoomFactor * MINE_Y,
                    zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y,
                    zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
                lv_drawLast[x][y] = -pos;
            }
        }
        
        /* Iterate sizeX+1 by sizeY+1 to paint the +1 margin column/row;
         * clipped from view by the final-blit srcRect. */
        if (++x > lv_screenGetSizeX()) { x = 0; y++; if (y > lv_screenGetSizeY()) done = TRUE; }
    }
    
    /* The markers, on top of the finished ground. */
    if (simple) {
        lvDrawItemMarkers(itemHits, itemHitCount, zoomFactor);
    }

    /* A scenario's markers: on the ground, under everything that moves, as
       the game view draws them. */
    lvDrawScnMarkers(zoomFactor);

    /* The declared regions, on the ground beside the markers. */
    lvDrawRegions(zoomFactor);

    lv_drawShells(sBullets);
    lv_drawTanks(tks);
    lv_drawLGMs(lgms);
    
    SDL_SetRenderTarget(sdlRenderer, prevTarget);

    /* Embedded: the host blits the render target itself (as an ImGui image),
     * so painting it to the framebuffer here would draw the world over the
     * host's live frame. */
    if (g_embedded) {
        return;
    }

    lvDrawBlitTargetToWindow();

    /* NOTE: Don't call SDL_RenderPresent here - ImGui needs to render after the game
     * and present once at the end. Calling present here causes the game to overwrite
     * the ImGui menu bar each frame. ImGui's lv_imgui_context_render() handles the final present. */
}

void lv_drawMarkRedraw(int mx, int my, int px, int py, int itemSize) {
    if (mx < 0 || mx > 255 || my < 0 || my > 255) return;
    lv_drawLast[mx][my] = 10000;
    if (px > itemSize && mx < 255) { lv_drawLast[mx+1][my] = 10000; if (my < 255) lv_drawLast[mx+1][my+1] = 10000; }
    if (py > itemSize && my < 255) { lv_drawLast[mx][my+1] = 10000; if (mx < 255) lv_drawLast[mx+1][my+1] = 10000; }
}

int lv_drawGetFrameRate(void) { return (int)g_dwFrameTotal; }

void lv_drawShells(screenBullets *sBullets) {
    int total, count, x, y, srcX, srcY, srcW, srcH;
    BYTE px, py, frame, mx, my, zf;
    
    total = lv_screenBulletsGetNumEntries(sBullets);
    zf = lv_windowGetZoomFactor();
    
    for (count = 1; count <= total; count++) {
        lv_screenBulletsGetItem(sBullets, count, &mx, &my, &px, &py, &frame);
        x = (mx * zf * TILE_SIZE_X) + zf * px;
        y = (my * zf * TILE_SIZE_Y) + zf * py;
        
        switch (frame) {
        case SHELL_EXPLOSION8: srcX = zf*EXPLOSION8_X; srcY = zf*EXPLOSION8_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION7: srcX = zf*EXPLOSION7_X; srcY = zf*EXPLOSION7_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION6: srcX = zf*EXPLOSION6_X; srcY = zf*EXPLOSION6_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION5: srcX = zf*EXPLOSION5_X; srcY = zf*EXPLOSION5_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION4: srcX = zf*EXPLOSION4_X; srcY = zf*EXPLOSION4_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION3: srcX = zf*EXPLOSION3_X; srcY = zf*EXPLOSION3_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION2: srcX = zf*EXPLOSION2_X; srcY = zf*EXPLOSION2_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION1: srcX = zf*EXPLOSION1_X; srcY = zf*EXPLOSION1_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_DIR0: srcX = zf*SHELL_0_X; srcY = zf*SHELL_0_Y; srcW = zf*SHELL_0_WIDTH; srcH = zf*SHELL_0_HEIGHT; break;
        case SHELL_DIR1: srcX = zf*SHELL_1_X; srcY = zf*SHELL_1_Y; srcW = zf*SHELL_1_WIDTH; srcH = zf*SHELL_1_HEIGHT; break;
        case SHELL_DIR2: srcX = zf*SHELL_2_X; srcY = zf*SHELL_2_Y; srcW = zf*SHELL_2_WIDTH; srcH = zf*SHELL_2_HEIGHT; break;
        case SHELL_DIR3: srcX = zf*SHELL_3_X; srcY = zf*SHELL_3_Y; srcW = zf*SHELL_3_WIDTH; srcH = zf*SHELL_3_HEIGHT; break;
        case SHELL_DIR4: srcX = zf*SHELL_4_X; srcY = zf*SHELL_4_Y; srcW = zf*SHELL_4_WIDTH; srcH = zf*SHELL_4_HEIGHT; break;
        case SHELL_DIR5: srcX = zf*SHELL_5_X; srcY = zf*SHELL_5_Y; srcW = zf*SHELL_5_WIDTH; srcH = zf*SHELL_5_HEIGHT; break;
        case SHELL_DIR6: srcX = zf*SHELL_6_X; srcY = zf*SHELL_6_Y; srcW = zf*SHELL_6_WIDTH; srcH = zf*SHELL_6_HEIGHT; break;
        case SHELL_DIR7: srcX = zf*SHELL_7_X; srcY = zf*SHELL_7_Y; srcW = zf*SHELL_7_WIDTH; srcH = zf*SHELL_7_HEIGHT; break;
        case SHELL_DIR8: srcX = zf*SHELL_8_X; srcY = zf*SHELL_8_Y; srcW = zf*SHELL_8_WIDTH; srcH = zf*SHELL_8_HEIGHT; break;
        case SHELL_DIR9: srcX = zf*SHELL_9_X; srcY = zf*SHELL_9_Y; srcW = zf*SHELL_9_WIDTH; srcH = zf*SHELL_9_HEIGHT; break;
        case SHELL_DIR10: srcX = zf*SHELL_10_X; srcY = zf*SHELL_10_Y; srcW = zf*SHELL_10_WIDTH; srcH = zf*SHELL_10_HEIGHT; break;
        case SHELL_DIR11: srcX = zf*SHELL_11_X; srcY = zf*SHELL_11_Y; srcW = zf*SHELL_11_WIDTH; srcH = zf*SHELL_11_HEIGHT; break;
        case SHELL_DIR12: srcX = zf*SHELL_12_X; srcY = zf*SHELL_12_Y; srcW = zf*SHELL_12_WIDTH; srcH = zf*SHELL_12_HEIGHT; break;
        case SHELL_DIR13: srcX = zf*SHELL_13_X; srcY = zf*SHELL_13_Y; srcW = zf*SHELL_13_WIDTH; srcH = zf*SHELL_13_HEIGHT; break;
        case SHELL_DIR14: srcX = zf*SHELL_14_X; srcY = zf*SHELL_14_Y; srcW = zf*SHELL_14_WIDTH; srcH = zf*SHELL_14_HEIGHT; break;
        default: srcX = zf*SHELL_15_X; srcY = zf*SHELL_15_Y; srcW = zf*SHELL_15_WIDTH; srcH = zf*SHELL_15_HEIGHT; break;
        }
        
        if (frame >= SHELL_EXPLOSION8 && frame <= SHELL_EXPLOSION1) lv_drawMarkRedraw(mx, my, px, py, 0);
        else lv_drawMarkRedraw(mx, my, px, py, 10);
        drawRenderTexture(textureTiles, srcX, srcY, srcW, srcH, x, y);
    }
}

/* Rows of tanks.bmp / boats.bmp the ally colouring draws from. The sheets carry
   17 rows of team colours; row 2 is the same green and row 11 the same red the
   game's own good and evil tank sprites use, and row 0 is the uncoloured tank
   an unloaded tc[] already lands every team on. */
#define TANK_ROW_SELF 0
#define TANK_ROW_GOOD 2
#define TANK_ROW_EVIL 11

/* lv_playersMakeScreenTanks adds TANK_GOOD_ADD / TANK_EVIL_ADD on top of the
   direction and boat frames, so the alliance to whoever was "self" when the
   screen was built is the top of the frame number. */
static BYTE lv_drawTankAllyRow(BYTE frame) {
    if (frame >= TANK_EVIL_ADD) {
        return TANK_ROW_EVIL;
    }
    if (frame >= TANK_GOOD_ADD) {
        return TANK_ROW_GOOD;
    }
    return TANK_ROW_SELF;
}

static void lvDrawRecordTankLabel(const char *str, int mx, int my, BYTE px, BYTE py);

void lv_drawTanks(screenTanks *tks) {
    int x, y, srcX, srcY;
    BYTE count, total, px, py, mx, my, team, zoomFactor, dir, frame;
    bool onBoat, simple, allyRead;
    char playerName[PLAYER_NAME_LEN];
    SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
    LogViewerState *lv = lv_screenGetState();

    total = lv_screenTanksGetNumEntries(tks);
    zoomFactor = lv_windowGetZoomFactor();
    s_tankLabelCount = 0;

    /* The same answer the ground pass used this frame, so the tanks and the
       squares under them cannot end up in different styles. */
    simple = lvSimpleView();
    if (simple) {
        SDL_GetRenderDrawBlendMode(sdlRenderer, &oldBlend);
        SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);
    }

    /* Which reading the tanks take. The reel's ally colours and team colours
       switched off both want the game's own self / ally / enemy sides; only
       the team palette says which player is which, which is the one thing
       that reading cannot. */
    allyRead = lv->allyColours || !lv->useTeamColours;

    for (count = 1; count <= total; count++) {
        lv_screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &team, &dir, &onBoat, playerName);
        px += 2; py += 2;
        x = mx * (zoomFactor * TILE_SIZE_X) + (zoomFactor * px);
        y = my * (zoomFactor * TILE_SIZE_Y) + (zoomFactor * py);

        if (simple) {
            /* A triangle pointing the way it faces, in whichever reading the
               sprites would have used, so the two cannot disagree. A tank on
               a boat is still a tank. */
            SDL_FColor colour;
            if (allyRead) {
                BYTE row = lv_drawTankAllyRow(frame);
                colour = (row == TANK_ROW_SELF) ? mapColourMarkerSelf()
                       : (row == TANK_ROW_GOOD) ? mapColourMarkerGood()
                                                : mapColourMarkerEvil();
            } else {
                colour = mapColourTeam(lv->tc[team]);
            }
            {
                float side = (float)(zoomFactor * TILE_SIZE_X);
                mapMarkerTank(sdlRenderer,
                              (float)x + side / 2.0f, (float)y + side / 2.0f,
                              SDL_max(3.0f, side * 0.45f), (int)dir, colour);
            }
        } else if (allyRead) {
            srcX = zoomFactor * TILE_SIZE_X * dir;
            srcY = zoomFactor * TILE_SIZE_Y * lv_drawTankAllyRow(frame);
            drawRenderTexture(onBoat ? textureBoats : textureTanks, srcX, srcY, zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y, x, y);
        } else {
            srcX = zoomFactor * TILE_SIZE_X * dir;
            srcY = zoomFactor * TILE_SIZE_Y * lv->tc[team];
            drawRenderTexture(onBoat ? textureBoats : textureTanks, srcX, srcY, zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y, x, y);
        }
        if (s_tankLabelsInTarget) {
            lv_drawTankLabel(playerName, mx, my, px, py);
        } else {
            lvDrawRecordTankLabel(playerName, mx, my, px, py);
        }
        lv_drawMarkRedraw(mx, my, px, py, 0);
    }

    if (simple) SDL_SetRenderDrawBlendMode(sdlRenderer, oldBlend);
}

void lv_drawLGMs(screenLgm *lgms) {
    BYTE total, count, frame, mx, my, px, py, zf;
    int x, y, srcX, srcY, srcW, srcH;
    
    total = lv_screenLgmGetNumEntries(lgms);
    zf = lv_windowGetZoomFactor();
    
    for (count = 1; count <= total; count++) {
        lv_screenLgmGetItem(lgms, count, &mx, &my, &px, &py, &frame);
        switch (frame) {
        case LGM0: srcX = zf*LGM0_X; srcY = zf*LGM0_Y; srcW = zf*LGM_WIDTH; srcH = zf*LGM_HEIGHT; lv_drawMarkRedraw(mx,my,px,py,12); break;
        case LGM1: srcX = zf*LGM1_X; srcY = zf*LGM1_Y; srcW = zf*LGM_WIDTH; srcH = zf*LGM_HEIGHT; lv_drawMarkRedraw(mx,my,px,py,12); break;
        case LGM2: srcX = zf*LGM2_X; srcY = zf*LGM2_Y; srcW = zf*LGM_WIDTH; srcH = zf*LGM_HEIGHT; lv_drawMarkRedraw(mx,my,px,py,12); break;
        default: srcX = zf*LGM_HELICOPTER_X; srcY = zf*LGM_HELICOPTER_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; lv_drawMarkRedraw(mx,my,px,py,1); break;
        }
        x = (zf * mx * TILE_SIZE_X) + (zf * px);
        y = (zf * my * TILE_SIZE_Y) + (zf * py);
        drawRenderTexture(textureTiles, srcX, srcY, srcW, srcH, x, y);
    }
}

void lv_drawTankLabel(char *str, int mx, int my, BYTE px, BYTE py) {
    SDL_Surface *surface;
    SDL_Texture *texture;
    int len, x, y, width = 5, count1, count2;
    SDL_FRect dstRect;
    SDL_Color color = {255, 255, 255, 255};
    BYTE zf;
    
    if (!labelFont || !str || (len = (int)strlen(str)) == 0) return;
    
    zf = lv_windowGetZoomFactor();
    /* SDL3_ttf: TTF_RenderText_Blended renders with transparent background and proper alpha anti-aliasing.
     * This avoids the green fringe issue that occurred with TTF_RenderText_Shaded + color key,
     * since anti-aliased edge pixels were blended with green and didn't match the exact color key. */
    surface = TTF_RenderText_Blended(labelFont, str, len, color);
    if (!surface) return;
    
    texture = SDL_CreateTextureFromSurface(sdlRenderer, surface);
    if (!texture) { SDL_DestroySurface(surface); return; }
    
    x = (mx+1) * zf * TILE_SIZE_X + zf * (px+1);
    y = my * zf * TILE_SIZE_Y + zf * py;
    
    dstRect.x = (float)x; dstRect.y = (float)y;
    dstRect.w = (float)surface->w; dstRect.h = (float)surface->h;
    
    if ((x + surface->w) > zf * lv_screenGetSizeX() * TILE_SIZE_X) dstRect.w = zf * lv_screenGetSizeX() * TILE_SIZE_X - x;
    if ((y + surface->h) > lv_screenGetSizeY() * (zf * TILE_SIZE_Y)) dstRect.h = zf * (lv_screenGetSizeY() * TILE_SIZE_Y - y);
    
    SDL_RenderTexture(sdlRenderer, texture, NULL, &dstRect);
    SDL_DestroyTexture(texture);
    SDL_DestroySurface(surface);
    
    if (len > 6) width = 25;
    for (count1 = 0; count1 < 2; count1++) {
        for (count2 = 0; count2 < width; count2++) {
            lv_drawMarkRedraw(mx+count2, my+count1, px, py, 10);
        }
    }
}

/* Note a tank's name and the target pixel lv_drawTankLabel would have drawn it
 * at, for drawing over the blit. */
static void lvDrawRecordTankLabel(const char *str, int mx, int my, BYTE px, BYTE py) {
    LvTankLabelPos *l;
    BYTE zf;

    if (str == NULL || str[0] == '\0' || s_tankLabelCount >= MAX_TANKS) return;
    zf = lv_windowGetZoomFactor();
    l = &s_tankLabelPos[s_tankLabelCount++];
    SDL_strlcpy(l->name, str, sizeof(l->name));
    l->x = (mx+1) * zf * TILE_SIZE_X + zf * (px+1);
    l->y = my * zf * TILE_SIZE_Y + zf * py;
    l->tex = -1;
}

static void lvFlushTankLabels(void) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (s_tankLabelTex[i].tex != NULL) SDL_DestroyTexture(s_tankLabelTex[i].tex);
        s_tankLabelTex[i].tex = NULL;
        s_tankLabelTex[i].name[0] = '\0';
    }
    if (s_tankLabelFont != NULL) TTF_CloseFont(s_tankLabelFont);
    s_tankLabelFont = NULL;
    s_tankLabelFontPx = 0;
}

void lv_drawSetTankLabelsInTarget(BYTE inTarget) {
    bool on = inTarget ? TRUE : FALSE;
    if (s_tankLabelsInTarget == on) return;
    s_tankLabelsInTarget = on;
    s_tankLabelCount = 0;
    /* Repaint everything: names already in the target have to go, and the
       ones about to be drawn into it need the squares under them marked. */
    lv_drawDirtyScreen();
}

int lv_drawTankLabelCount(float scale) {
    int px, i, j;
    bool used[MAX_TANKS];

    if (s_tankLabelsInTarget || sdlRenderer == NULL) return 0;

    /* The game's label size times the scale the target is drawn at, so the
       glyphs are rasterized at the size they appear and never resampled. */
    px = (int)SDL_lroundf((float)LV_TANK_LABEL_PX * scale);
    if (px < LV_TANK_LABEL_MIN_PX) px = LV_TANK_LABEL_MIN_PX;
    if (px != s_tankLabelFontPx) {
        const char *base = SDL_GetBasePath();
        char path[1024];
        lvFlushTankLabels();
        SDL_snprintf(path, sizeof(path), "%sdata/fonts/SarasaMonoSlabJ-Regular.ttf",
                     base ? base : "./");
        s_tankLabelFont = TTF_OpenFont(path, (float)px);
        s_tankLabelFontPx = px;
    }
    if (s_tankLabelFont == NULL) return 0;

    /* First the names that already have a texture, wherever it sits. */
    for (j = 0; j < MAX_TANKS; j++) used[j] = FALSE;
    for (i = 0; i < s_tankLabelCount; i++) {
        s_tankLabelPos[i].tex = -1;
        for (j = 0; j < MAX_TANKS; j++) {
            if (!used[j] && s_tankLabelTex[j].tex != NULL &&
                SDL_strcmp(s_tankLabelTex[j].name, s_tankLabelPos[i].name) == 0) {
                s_tankLabelPos[i].tex = j;
                used[j] = TRUE;
                break;
            }
        }
    }
    /* Then the rest, each into a slot no name on screen holds. There are as
       many slots as names, so one is always free. */
    for (i = 0; i < s_tankLabelCount; i++) {
        LvTankLabelTex *t;
        if (s_tankLabelPos[i].tex >= 0) continue;
        for (j = 0; j < MAX_TANKS && used[j]; j++) {}
        if (j == MAX_TANKS) break;
        used[j] = TRUE;
        t = &s_tankLabelTex[j];
        if (t->tex != NULL) SDL_DestroyTexture(t->tex);
        t->tex = NULL;
        SDL_strlcpy(t->name, s_tankLabelPos[i].name, sizeof(t->name));
        {
            SDL_Color white = {255, 255, 255, 255};
            SDL_Surface *s = TTF_RenderText_Blended(s_tankLabelFont, t->name, 0, white);
            if (s == NULL) continue;
            t->tex = SDL_CreateTextureFromSurface(sdlRenderer, s);
            t->w = s->w;
            t->h = s->h;
            SDL_DestroySurface(s);
        }
        if (t->tex != NULL) {
            SDL_SetTextureBlendMode(t->tex, SDL_BLENDMODE_BLEND);
            s_tankLabelPos[i].tex = j;
        }
    }
    return s_tankLabelCount;
}

BYTE lv_drawTankLabelGet(int index, SDL_Texture **outTex, int *outX, int *outY,
                         int *outW, int *outH) {
    const LvTankLabelTex *t;

    if (index < 0 || index >= s_tankLabelCount || s_tankLabelPos[index].tex < 0) {
        return FALSE;
    }
    t = &s_tankLabelTex[s_tankLabelPos[index].tex];
    if (t->tex == NULL) return FALSE;
    if (outTex != NULL) *outTex = t->tex;
    if (outX != NULL) *outX = s_tankLabelPos[index].x;
    if (outY != NULL) *outY = s_tankLabelPos[index].y;
    if (outW != NULL) *outW = t->w;
    if (outH != NULL) *outH = t->h;
    return TRUE;
}

/* The names over the world the blit just drew: src is the slice of the target
   it took, dst where it went. The game's look: grey over a black shadow
   offset about one glyph pixel, so a name reads on sea and on road alike. */
static void lvDrawTankLabelsOverBlit(const SDL_FRect *src, const SDL_FRect *dst) {
    float scale;
    int n, i;
    SDL_Rect clip, oldClip;
    bool hadClip;

    if (src->w <= 0.0f || src->h <= 0.0f) return;
    scale = dst->w / src->w;
    n = lv_drawTankLabelCount(scale);
    if (n == 0) return;

    hadClip = SDL_RenderClipEnabled(sdlRenderer);
    SDL_GetRenderClipRect(sdlRenderer, &oldClip);
    clip.x = (int)dst->x;
    clip.y = (int)dst->y;
    clip.w = (int)dst->w;
    clip.h = (int)dst->h;
    SDL_SetRenderClipRect(sdlRenderer, &clip);

    for (i = 0; i < n; i++) {
        SDL_Texture *tex;
        int tx, ty, tw, th;
        float off;
        SDL_FRect d;

        if (!lv_drawTankLabelGet(i, &tex, &tx, &ty, &tw, &th)) continue;
        d.x = SDL_floorf(dst->x + ((float)tx - src->x) * scale);
        d.y = SDL_floorf(dst->y + ((float)ty - src->y) * scale);
        d.w = (float)tw;
        d.h = (float)th;
        off = SDL_floorf((float)th / (float)LV_TANK_LABEL_PX);
        if (off < 1.0f) off = 1.0f;
        d.x += off; d.y += off;
        SDL_SetTextureColorMod(tex, 0, 0, 0);
        SDL_RenderTexture(sdlRenderer, tex, NULL, &d);
        d.x -= off; d.y -= off;
        SDL_SetTextureColorMod(tex, LV_TANK_LABEL_GREY, LV_TANK_LABEL_GREY, LV_TANK_LABEL_GREY);
        SDL_RenderTexture(sdlRenderer, tex, NULL, &d);
    }

    SDL_SetRenderClipRect(sdlRenderer, hadClip ? &oldClip : NULL);
}

/*********************************************************
*NAME:          lv_drawBlitGameTexture
*PURPOSE:
*  Blits the game render texture to the screen without
*  re-rendering the game. Used when the game state hasn't
*  changed but we need to display the last frame.
*********************************************************/
void lv_drawBlitGameTexture(void) {
    if (!textureTarget || !sdlRenderer) return;
    /* Embedded: the host owns the blit (see lv_drawMainScreen). */
    if (g_embedded) return;

    lvDrawBlitTargetToWindow();
}

/*********************************************************
*NAME:          lv_drawGetSDLWindow
*PURPOSE:
*  Returns the SDL window handle for ImGui integration
*********************************************************/
SDL_Window* lv_drawGetSDLWindow(void) {
    return sdlWindow;
}

/*********************************************************
*NAME:          lv_drawGetSDLRenderer
*PURPOSE:
*  Returns the SDL renderer handle for ImGui integration
*********************************************************/
SDL_Renderer* lv_drawGetSDLRenderer(void) {
    return sdlRenderer;
}

/*********************************************************
*NAME:          lv_drawGetGameTexture
*PURPOSE:
*  Returns the game render texture for ImGui viewport display
*********************************************************/
SDL_Texture* lv_drawGetGameTexture(void) {
    return textureTarget;
}

/* Tile atlas accessor — exposes the unified SVG/PNG/BMP atlas built by
 * buildTileAtlas() so the standalone game-view path can route it through
 * mapview.c's MapViewCtx. */
SDL_Texture* lv_drawGetTilesTexture(void) {
    return textureTiles;
}

/* Atlas is always built at scale 1 (see buildTileAtlas()) — the blit-time
 * scaling is in the texture target, not the source atlas. */
int lv_drawGetSheetScale(void) {
    return 1;
}
