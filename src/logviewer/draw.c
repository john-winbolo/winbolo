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
#include "imgui/imgui_dock.h"
#include "lv_dock_fit.h"
#include "lv_hud.h"
#include "lv_team_sprite.h"
#include "lv_tank_bars.h"
#include "lv_tank_pick.h"
#include "lv_event_flash.h"
#include "lv_builder_status.h"
#include "clientmutex.h"
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
/* The grey status shapes the HUD panel colours by team; see buildHudShapes. */
static SDL_Texture *textureHudShapes = NULL;
/* The game's background picture (data/background.bmp), for the Tank panel's
 * gauges. Loaded the first time it is asked for; tried once. */
static SDL_Texture *textureStatusBackground = NULL;
static bool statusBackgroundTried = false;
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

/* The bars over the tanks (lv_tank_bars.h). Like the names they are drawn
 * over the blit, at the screen's own resolution, from where lv_drawTanks put
 * each tank's picture in the target. */
typedef struct {
    int  x, y;                     /* the picture's top-left, in render-target pixels */
    BYTE slot;                     /* the player's slot */
} LvTankBarPos;
static LvTankBarPos s_tankBarPos[MAX_TANKS];
static int          s_tankBarCount = 0;
static bool         s_tankBars = FALSE;
/* The builder markers over the tanks (lv_builder_status.h), at the same
   places. */
static bool         s_builderStatus = FALSE;
/* The last blit of the map into the window: the slice of the target it took
 * and where it went, in framebuffer pixels. A click on the map is matched
 * against the tanks with these (lv_drawTankAtPoint). */
static SDL_FRect    s_blitSrc, s_blitDst;
static bool         s_blitKnown = FALSE;

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
static void lvDrawTankBarsOverBlit(const SDL_FRect *src, const SDL_FRect *dst);
static void lvDrawTankMarkOverBlit(const SDL_FRect *src, const SDL_FRect *dst);
static void lvDrawBuilderStatusOverBlit(const SDL_FRect *src, const SDL_FRect *dst);
static void lvDrawEventFlashOverBlit(const SDL_FRect *src, const SDL_FRect *dst);

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
    if (!g_embedded) {
        /* The standalone viewer's left and top sidebars push the map in from
         * the window's corner; the right and bottom ones only bound it. Each
         * offset is floored on its own so the map's corner is the pixel the
         * sidebar's edge is drawn at. */
        int dockL = 0, dockT = 0;
        lv_imgui_dock_margins(&dockL, &dockT, NULL, NULL);
        dstRect.x += SDL_floorf((float)dockL * pxScale);
        dstRect.y += SDL_floorf((float)dockT * pxScale);
    }
    dstRect.w = (float)(zoomFactor * lv_screenGetSizeX() * TILE_SIZE_X) * blitZoom;
    dstRect.h = (float)(zoomFactor * lv_screenGetSizeY() * TILE_SIZE_Y) * blitZoom;

    SDL_SetTextureScaleMode(textureTarget,
                            (blitZoom >= 1.0f) ? SDL_SCALEMODE_NEAREST
                                               : SDL_SCALEMODE_LINEAR);
    SDL_RenderTexture(sdlRenderer, textureTarget, &srcRect, &dstRect);
    s_blitKnown = !lv->gameView;
    if (!lv->gameView) {
        s_blitSrc = srcRect;
        s_blitDst = dstRect;
        lvDrawTankMarkOverBlit(&srcRect, &dstRect);
        /* The bars first, so a name is never under one. */
        lvDrawTankBarsOverBlit(&srcRect, &dstRect);
        lvDrawBuilderStatusOverBlit(&srcRect, &dstRect);
        lvDrawTankLabelsOverBlit(&srcRect, &dstRect);
        /* Last, so nothing on the map hides it. */
        lvDrawEventFlashOverBlit(&srcRect, &dstRect);
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
    int   mapW = 0;
    int   dockL = 0, dockT = 0, dockR = 0, dockB = 0;
    LvDockMargins dock = { 0, 0, 0, 0 };

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
    /* The standalone viewer's sidebars take the edges of the window; the map
     * is what they leave, as it is when a sidebar comes in or is resized
     * (lvHostDockSync). The embedded reel has none. */
    if (!g_embedded) lv_imgui_dock_margins(&dockL, &dockT, &dockR, &dockB);
    dock.left = dockL;
    dock.top = dockT;
    dock.right = dockR;
    dock.bottom = dockB;
    lvDockMapRect(&dock, windowW, windowH, menuH, NULL, NULL, &mapW, &gameH);

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

    gx = (float)(mouseScreenX - dockL);
    gy = (float)(mouseScreenY - dockT) - (float)menuH;
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
    /* Beside a sidebar the map runs up to the sidebar's edge: the last
     * square may go a little way under it rather than stop short. */
    if (dockL + dockR > 0) {
        gameW = mapW;
        newTilesX = lvFitTilesCover(gameW, newEffective);
    }
    newTilesY = (int)(((float)gameH + newEffective * 0.5f) / newEffective);
    if (dockT + dockB > 0) newTilesY = lvFitTilesCover(gameH, newEffective);
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

/* Build the strip of grey shapes the HUD panel colours by team (lv_hud.h).
 *
 * The status icons in the sheet are pure green or pure red on black, so a
 * team colour cannot be multiplied into them: blue times green is black.
 * Each shape is copied out as grey levels instead, white where the icon is
 * brightest, and the panel multiplies the team colour into that. The cells
 * keep the icons' black background, as the game's panels are black too.
 *
 * Read with SDL_ReadSurfacePixel so the sheet's pixel format does not
 * matter. The sheet is built at scale 1 (see buildTileAtlas). */
static void buildHudShapes(SDL_Renderer *renderer, SDL_Surface *sheet) {
    /* Where each shape is in the sheet, in the order of LV_HUD_SHAPE_*. The
       "hostile" icons are the solid ones; the "own" icons are hollow rings. */
    static const int src[LV_HUD_SHAPE_COUNT][2] = {
        { STATUS_BASE_ALLIEGOOD_X,   STATUS_BASE_ALLIEGOOD_Y   },
        { STATUS_PILLBOX_EVIL_X,     STATUS_PILLBOX_EVIL_Y     },
        { STATUS_PILLBOX_TANKEVIL_X, STATUS_PILLBOX_TANKEVIL_Y },
    };
    const int cellW = STATUS_ITEM_SIZE_X + 2 * LV_HUD_SHAPE_PAD;
    const int cellH = STATUS_ITEM_SIZE_Y + 2 * LV_HUD_SHAPE_PAD;

    if (textureHudShapes) {
        SDL_DestroyTexture(textureHudShapes);
        textureHudShapes = NULL;
    }
    SDL_Surface *strip = SDL_CreateSurface(cellW * LV_HUD_SHAPE_COUNT, cellH,
                                           SDL_PIXELFORMAT_RGBA32);
    if (!strip) return;
    SDL_FillSurfaceRect(strip, NULL, SDL_MapSurfaceRGBA(strip, 0, 0, 0, 255));

    for (int n = 0; n < LV_HUD_SHAPE_COUNT; n++) {
        Uint8 r, g, b, a;
        int cellMax = 0;
        /* First the brightest pixel of the icon, then the icon against it. */
        for (int pass = 0; pass < 2; pass++) {
            for (int y = 0; y < STATUS_ITEM_SIZE_Y; y++) {
                for (int x = 0; x < STATUS_ITEM_SIZE_X; x++) {
                    if (!SDL_ReadSurfacePixel(sheet, src[n][0] + x, src[n][1] + y,
                                              &r, &g, &b, &a)) {
                        continue;
                    }
                    if (pass == 0) {
                        int v = r > g ? r : g;
                        if (b > v) v = b;
                        if (v > cellMax) cellMax = v;
                    } else {
                        Uint8 v = lvHudGreyValue(r, g, b, cellMax);
                        SDL_WriteSurfacePixel(strip,
                                              n * cellW + LV_HUD_SHAPE_PAD + x,
                                              LV_HUD_SHAPE_PAD + y, v, v, v, 255);
                    }
                }
            }
        }
    }

    textureHudShapes = SDL_CreateTextureFromSurface(renderer, strip);
    SDL_DestroySurface(strip);
    if (textureHudShapes) {
        SDL_SetTextureBlendMode(textureHudShapes, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(textureHudShapes, SDL_SCALEMODE_NEAREST);
    }
}

/* The three team sheets all have the orange row (lv_team_sprite.h). */
static bool s_teamOrangeRow = false;

/* Load a team sheet (tanks.bmp, boats.bmp, items.bmp) with one row more than
 * the file has: the red row again in orange, for a team whose colour is the
 * red (lv_team_sprite.h says why). The rows of a sheet differ in one flat
 * colour, so changing that colour in a copy of a row is all it takes; the
 * pixels the key made transparent go through the same change, because the
 * edge bleed has put the red of the pixels beside them into them.
 *
 * If the taller sheet cannot be made the file's own rows are used, and
 * *hasOrange says so. Returns NULL only when the file itself did not load. */
static SDL_Texture *lvLoadTeamSheet(SDL_Renderer *renderer, const char *path,
                                    bool *hasOrange) {
    SDL_Surface *sheet = sdlLoadBmpSheetSurface(path, TILE_SIZE_X, TILE_SIZE_Y);
    SDL_Surface *tall = NULL;
    SDL_Texture *texture;

    *hasOrange = false;
    if (!sheet) return NULL;

    if (sheet->format == SDL_PIXELFORMAT_RGBA32 &&
        sheet->h >= LV_TEAM_SHEET_ROWS * TILE_SIZE_Y) {
        tall = SDL_CreateSurface(sheet->w, (LV_TEAM_SHEET_ROWS + 1) * TILE_SIZE_Y,
                                 SDL_PIXELFORMAT_RGBA32);
    }
    if (tall) {
        const size_t rowBytes = (size_t)sheet->w * 4;
        for (int y = 0; y < tall->h; y++) {
            /* The rows of the file as they are, then the red row once more. */
            int from = (y < LV_TEAM_SHEET_ROWS * TILE_SIZE_Y)
                           ? y
                           : LV_TEAM_PALETTE_RED * TILE_SIZE_Y +
                                 (y - LV_TEAM_SHEET_ROW_ORANGE * TILE_SIZE_Y);
            unsigned char *dst = (unsigned char *)tall->pixels + (size_t)y * (size_t)tall->pitch;
            memcpy(dst, (const unsigned char *)sheet->pixels + (size_t)from * (size_t)sheet->pitch,
                   rowBytes);
            if (from != y) {
                for (int x = 0; x < tall->w; x++) {
                    lvTeamRedPixelToOrange(&dst[x * 4], &dst[x * 4 + 1], &dst[x * 4 + 2]);
                }
            }
        }
        *hasOrange = true;
    }

    texture = SDL_CreateTextureFromSurface(renderer, tall ? tall : sheet);
    if (tall) SDL_DestroySurface(tall);
    SDL_DestroySurface(sheet);
    if (!texture) {
        *hasOrange = false;
        return NULL;
    }
    /* As sdlLoadBmpSheetAsTexture leaves a sheet. */
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    return texture;
}

/* Load tanks.bmp, boats.bmp and items.bmp out of the data directory. */
static void lvLoadTeamSheets(SDL_Renderer *renderer) {
    const char *basePath = SDL_GetBasePath();
    char bmpPath[1024];
    bool tanks = false, boats = false, items = false;

    if (!basePath) basePath = "./";
    SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/tanks.bmp", basePath);
    textureTanks = lvLoadTeamSheet(renderer, bmpPath, &tanks);
    SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/boats.bmp", basePath);
    textureBoats = lvLoadTeamSheet(renderer, bmpPath, &boats);
    SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/items.bmp", basePath);
    textureItems = lvLoadTeamSheet(renderer, bmpPath, &items);
    /* All three or none, so a team's tank and its pillboxes cannot end up in
       two different oranges. */
    s_teamOrangeRow = tanks && boats && items;
}

/* The row of a team sheet a team with this palette entry is drawn from. */
static int lvDrawTeamRow(int palette) {
    return lvTeamSheetRowHave(lvTeamSheetRow(palette), s_teamOrangeRow);
}

/* The flat colour of a team with this palette entry, for the shapes of the
   simplified view: the palette's, and orange for a red one, as its sprites
   are. */
static SDL_FColor lvDrawTeamColour(int palette) {
    SDL_FColor c = mapColourTeam(palette);
    lvHudRedToOrange(&c.r, &c.g, &c.b);
    return c;
}

/* Build the unified tile atlas (SVG/PNG/BMP combined sheet) at scale 1.
 * Returns NULL on failure. The log viewer always uses scale 1 — the
 * blit-time scaling is in the texture target, not the source atlas. */
static SDL_Texture *buildTileAtlas(SDL_Renderer *renderer) {
    SDL_Surface *sheet = tileLoaderBuildSheet(TILE_SIZE_X);
    if (!sheet) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, sheet);
    /* While the sheet's pixels are still in hand. Without the strip the HUD
       panel draws its grids with no owned icons, so a failure here is not a
       failure of the setup. */
    buildHudShapes(renderer, sheet);
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
     * 17-row team-colour palette indexed via lv->tc[], which the
     * unified atlas does not provide (lvLoadTeamSheets adds a row). */
    textureTiles = buildTileAtlas(sdlRenderer);
    lvLoadTeamSheets(sdlRenderer);

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
    lvLoadTeamSheets(sdlRenderer);

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
    if (textureHudShapes) { SDL_DestroyTexture(textureHudShapes); textureHudShapes = NULL; }
    if (textureStatusBackground) { SDL_DestroyTexture(textureStatusBackground); textureStatusBackground = NULL; }
    statusBackgroundTried = false;
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
    /* Centre in the part of the window the sidebars leave, not behind them. */
    int dockL = 0, dockT = 0, dockR = 0, dockB = 0;
    lv_imgui_dock_margins(&dockL, &dockT, &dockR, &dockB);
    int splashX0 = (int)((float)dockL * pxScale);
    int splashY0 = (int)((float)dockT * pxScale);
    windowWidth -= (int)((float)(dockL + dockR) * pxScale);
    windowHeight -= (int)((float)(dockT + dockB) * pxScale);
    if (windowWidth < 0) windowWidth = 0;
    if (windowHeight < 0) windowHeight = 0;
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

        dstRect.x = (float)(splashX0 + x);
        dstRect.y = (float)(splashY0 + y);
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

/* lv_team_sprite.h names the game's tiles by number, so it needs no other
   header. This fails to compile if a number there stops being the tile. */
typedef char lvTeamSpriteTileCheck[(LV_TILE_PILL_EVIL_15 == PILL_EVIL_15 &&
                                    LV_TILE_PILL_EVIL_14 == PILL_EVIL_14 &&
                                    LV_TILE_PILL_EVIL_0 == PILL_EVIL_0 &&
                                    PILL_EVIL_0 - PILL_EVIL_14 == 14 &&
                                    LV_TILE_BASE_NEUTRAL == BASE_NEUTRAL) ? 1 : -1];

/* Draw a pillbox or a base in map square x,y from the sheet lv_team_sprite.h
   picked: one of the game's own tiles, or a team's cell of items.bmp. */
static void lvDrawMapItem(LvMapItemSprite s, int x, int y, BYTE zoomFactor) {
    if (s.fromAtlas) {
        drawRenderTexture(textureTiles, mapViewPosX[s.tile], mapViewPosY[s.tile],
            zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y,
            zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
    } else {
        int row = lvTeamSheetRowHave(s.row, s_teamOrangeRow);
        drawRenderTexture(textureItems, s.col * zoomFactor * TILE_SIZE_X, row * zoomFactor * TILE_SIZE_Y,
            zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y,
            zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
    }
}

/* Whether tanks, pillboxes and bases are drawn in their team's colour. With
   a perspective on they are not, whatever the setting says: the map then
   shows them as that player's game does, own and allied against hostile. */
static bool lvDrawTeamColours(const LogViewerState *lv) {
    return lv->useTeamColours && lv_playersHasPerspective() == FALSE;
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
    bool teamColours = lvDrawTeamColours(lv);

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
                       side holds it, the way the game's markers do. A thing
                       nobody owns has no team: it takes the game's marker, a
                       neutral base's for a base and a hostile pillbox's for a
                       pillbox, as its sprite is the game's too. */
                    itc = NEUTRAL_TEAM;
                    if (teamColours) {
                        itc = isBase ? lv_screenGetBaseTeam(x, y)
                                     : lv_screenGetPillTeam(x, y, &pillHealth);
                    }
                    if (teamColours && itc != NEUTRAL_TEAM) {
                        itemHits[itemHitCount].colour = lvDrawTeamColour(lv->tc[itc]);
                    } else if (teamColours) {
                        itemHits[itemHitCount].colour = isBase ? mapColourMarkerNeutral()
                                                               : mapColourMarkerEvil();
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
            } else if (isPill && teamColours) {
                /* In its team's colour; a neutral one as the game draws it
                   (lv_team_sprite.h). */
                itc = lv_screenGetPillTeam(x, y, &pillHealth);
                lvDrawMapItem(lvMapPillSprite(itc == NEUTRAL_TEAM, pillHealth, lv->tc[itc]),
                              x, y, zoomFactor);
            } else if (isBase && teamColours) {
                itc = lv_screenGetBaseTeam(x, y);
                lvDrawMapItem(lvMapBaseSprite(itc == NEUTRAL_TEAM, lv->tc[itc]),
                              x, y, zoomFactor);
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

/* Mark every square a w by h pixel sprite drawn px, py pixels into square
 * mx, my covers. lv_drawMarkRedraw only reaches the next square over, which
 * is one short for a tank: it is drawn two pixels further in than its offset,
 * so at an offset of 15 it reaches one pixel into the square two along, and
 * that pixel was left behind when the tank moved on. */
static void lv_drawMarkRedrawRect(int mx, int my, int px, int py, int w, int h) {
    int x0 = mx + px / TILE_SIZE_X;
    int x1 = mx + (px + w - 1) / TILE_SIZE_X;
    int y0 = my + py / TILE_SIZE_Y;
    int y1 = my + (py + h - 1) / TILE_SIZE_Y;
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 255) x1 = 255;
    if (y1 > 255) y1 = 255;
    for (x = x0; x <= x1; x++) {
        for (y = y0; y <= y1; y++) {
            lv_drawLast[x][y] = 10000;
        }
    }
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
   an unloaded tc[] already lands every team on. Row 11 is drawn only here, as
   "hostile": with team colours a team whose colour is the red gets the orange
   row instead (lvDrawTeamRow). */
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
    s_tankBarCount = 0;

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
    allyRead = lv->allyColours || !lvDrawTeamColours(lv);

    for (count = 1; count <= total; count++) {
        lv_screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &team, &dir, &onBoat, playerName);
        px += 2; py += 2;
        x = mx * (zoomFactor * TILE_SIZE_X) + (zoomFactor * px);
        y = my * (zoomFactor * TILE_SIZE_Y) + (zoomFactor * py);
        if (s_tankBarCount < MAX_TANKS) {
            /* Where the picture goes, for the bars over it. */
            s_tankBarPos[s_tankBarCount].x = x;
            s_tankBarPos[s_tankBarCount].y = y;
            s_tankBarPos[s_tankBarCount].slot = lv_screenTanksGetSlot(tks, count);
            s_tankBarCount++;
        }

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
                colour = lvDrawTeamColour(lv->tc[team]);
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
            srcY = zoomFactor * TILE_SIZE_Y * lvDrawTeamRow(lv->tc[team]);
            drawRenderTexture(onBoat ? textureBoats : textureTanks, srcX, srcY, zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y, x, y);
        }
        if (s_tankLabelsInTarget) {
            lv_drawTankLabel(playerName, mx, my, px, py);
        } else {
            lvDrawRecordTankLabel(playerName, mx, my, px, py);
        }
        lv_drawMarkRedrawRect(mx, my, px, py, TILE_SIZE_X, TILE_SIZE_Y);
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

void lv_drawSetTankBars(BYTE on) {
    s_tankBars = on ? TRUE : FALSE;
}

BYTE lv_drawGetTankBars(void) {
    return s_tankBars ? TRUE : FALSE;
}

/* One bar: the filled part in its colour, the rest in the track's. */
static void lvDrawOneTankBar(const LvTankBarRect *bar, int value, int full,
                             Uint8 r, Uint8 g, Uint8 b) {
    const Uint8 alpha = (Uint8)(LV_TANK_BARS_ALPHA * 255.0f + 0.5f);
    float fillW = lvTankBarFillWidth(bar->w, value, full);
    SDL_FRect part;

    part.y = bar->y;
    part.h = bar->h;
    if (fillW > 0.0f) {
        part.x = bar->x;
        part.w = fillW;
        SDL_SetRenderDrawColor(sdlRenderer, r, g, b, alpha);
        SDL_RenderFillRect(sdlRenderer, &part);
    }
    if (fillW < bar->w) {
        part.x = bar->x + fillW;
        part.w = bar->w - fillW;
        SDL_SetRenderDrawColor(sdlRenderer, LV_TANK_BARS_TRACK_R, LV_TANK_BARS_TRACK_G,
                               LV_TANK_BARS_TRACK_B, alpha);
        SDL_RenderFillRect(sdlRenderer, &part);
    }
}

/* The armour and shells bars over each live tank the blit just drew: src is
   the slice of the target it took, dst where it went. The places are worked
   out as the blit works out the tanks' own, so the bars stay on their tanks
   through a scroll, a zoom and a sidebar. What a tank holds is read now, not
   when the target was painted, so a bar moves when its tank is hit without
   the map having to be repainted. */
static void lvDrawTankBarsOverBlit(const SDL_FRect *src, const SDL_FRect *dst) {
    LogViewerState *lv = lv_screenGetState();
    float scale, tilePx, pxScale;
    int i;
    SDL_Rect clip, oldClip;
    bool hadClip;
    SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
    Uint8 oldR, oldG, oldB, oldA;
    struct { bool alive; int armour, shells; } held[MAX_TANKS];
    int fullArmour, fullShells;
    bool hasStock;

    if (!s_tankBars || s_tankBarCount == 0 || lv == NULL) return;
    /* What the tanks hold is written by playback on the timer thread, under
       the client mutex. Copy it out under the same mutex and draw from the
       copy, so a bar never shows half of one tick and half of the next. The
       mutex is recursive (SDL_Mutex), so this is safe on both ways in: the
       repaint, which already holds it, and the plain blit of an unchanged
       frame, which does not. The places in s_tankBarPos are this thread's
       own. */
    lv_clientMutexWaitFor();
    hasStock = lv_screenHasTankStock() != 0;
    fullArmour = lv->rules.tankFullArmour;
    fullShells = lv->rules.tankFullShells;
    for (i = 0; i < MAX_TANKS; i++) {
        held[i].alive = lv->gameViewHud[i].alive;
        held[i].armour = lv->tankInv[i].armour;
        held[i].shells = lv->tankInv[i].shells;
    }
    lv_clientMutexRelease();
    /* A recording written before tank stocks were recorded reads zero for
       every tank. Two empty bars over each one would say "no armour, no
       shells", which is not known: draw none, as the Tank panel shows no
       gauges for such a recording. */
    if (!hasStock) return;
    if (src->w <= 0.0f || src->h <= 0.0f) return;
    scale = dst->w / src->w;
    tilePx = (float)TILE_SIZE_X * scale;
    /* scale is in framebuffer pixels (the blit folds the pixel density in),
       and the bars' sizes are in points: hand the density to the sums. */
    pxScale = lvDrawPixelScale();
    if (!lvTankBarsVisible(tilePx, pxScale)) return;

    hadClip = SDL_RenderClipEnabled(sdlRenderer);
    SDL_GetRenderClipRect(sdlRenderer, &oldClip);
    clip.x = (int)dst->x;
    clip.y = (int)dst->y;
    clip.w = (int)dst->w;
    clip.h = (int)dst->h;
    SDL_SetRenderClipRect(sdlRenderer, &clip);
    SDL_GetRenderDrawBlendMode(sdlRenderer, &oldBlend);
    SDL_GetRenderDrawColor(sdlRenderer, &oldR, &oldG, &oldB, &oldA);
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);

    for (i = 0; i < s_tankBarCount; i++) {
        BYTE slot = s_tankBarPos[i].slot;
        LvTankBarsLayout bars;
        Uint8 r, g, b;
        int armour, shells;

        /* No bars for a tank that is dead: the picture left in the target
           until the next repaint is not a tank that holds anything. */
        if (slot >= MAX_TANKS || !held[slot].alive) continue;
        bars = lvTankBarsLayout(dst->x + ((float)s_tankBarPos[i].x - src->x) * scale,
                                dst->y + ((float)s_tankBarPos[i].y - src->y) * scale,
                                tilePx, pxScale);
        if (!bars.visible) continue;

        armour = held[slot].armour;
        shells = held[slot].shells;
        lvTankBarArmourColour(lvTankBarFraction(armour, fullArmour), &r, &g, &b);
        lvDrawOneTankBar(&bars.armour, armour, fullArmour, r, g, b);
        lvDrawOneTankBar(&bars.shells, shells, fullShells,
                         LV_TANK_BARS_SHELLS_R, LV_TANK_BARS_SHELLS_G, LV_TANK_BARS_SHELLS_B);
    }

    SDL_SetRenderDrawColor(sdlRenderer, oldR, oldG, oldB, oldA);
    SDL_SetRenderDrawBlendMode(sdlRenderer, oldBlend);
    SDL_SetRenderClipRect(sdlRenderer, hadClip ? &oldClip : NULL);
}

void lv_drawSetBuilderStatus(BYTE on) {
    s_builderStatus = on ? TRUE : FALSE;
}

BYTE lv_drawGetBuilderStatus(void) {
    return s_builderStatus ? TRUE : FALSE;
}

/* The cells of one small picture, with its top left at x, y: each cell a
   square of `cell` pixels, grown by `grow` pixels on every side (the dark
   pass under the coloured one uses that for the edge). The colour is the
   renderer's. */
static void lvDrawBuilderGlyph(const LvBuilderGlyph *g, float x, float y,
                               float cell, float grow) {
    int row, col;

    if (g == NULL) return;
    for (row = 0; row < g->height; row++) {
        for (col = 0; col < g->width; col++) {
            SDL_FRect r;
            if (!lvBuilderGlyphCell(g, col, row)) continue;
            r.x = x + (float)col * cell - grow;
            r.y = y + (float)row * cell - grow;
            r.w = cell + grow * 2.0f;
            r.h = cell + grow * 2.0f;
            SDL_RenderFillRect(sdlRenderer, &r);
        }
    }
}

/* The countdown's characters from x, with their tops at y. */
static void lvDrawBuilderText(const char *text, float x, float y, float cell,
                              float grow) {
    for (; *text != '\0'; text++) {
        const LvBuilderGlyph *g = lvBuilderChar(*text);
        if (g == NULL) continue;
        lvDrawBuilderGlyph(g, x, y, cell, grow);
        x += (float)(g->width + 1) * cell;
    }
}

/* The arrow: a dart with its middle at cx, cy and its point `radius` pixels
   out along the direction. */
static void lvDrawBuilderArrow(float cx, float cy, float radius, int direction,
                               SDL_FColor colour) {
    SDL_Vertex v[4];
    static const int k_index[6] = { 0, 1, 3, 0, 3, 2 };
    float fx = 0.0f, fy = -1.0f;
    float sx, sy;
    int i;

    lvBuilderDirectionStep(direction, &fx, &fy);
    sx = -fy;   /* a quarter turn from the direction: the dart's width */
    sy = fx;
    /* The point, the two back corners, and the notch between them. */
    v[0].position.x = cx + fx * radius;
    v[0].position.y = cy + fy * radius;
    v[1].position.x = cx - fx * radius + sx * radius * 0.8f;
    v[1].position.y = cy - fy * radius + sy * radius * 0.8f;
    v[2].position.x = cx - fx * radius - sx * radius * 0.8f;
    v[2].position.y = cy - fy * radius - sy * radius * 0.8f;
    v[3].position.x = cx - fx * radius * 0.35f;
    v[3].position.y = cy - fy * radius * 0.35f;
    for (i = 0; i < 4; i++) {
        v[i].color = colour;
        v[i].tex_coord.x = 0.0f;
        v[i].tex_coord.y = 0.0f;
    }
    SDL_RenderGeometry(sdlRenderer, NULL, v, 4, k_index, 6);
}

/* The builder marker over each live tank the blit just drew, where the bars
   are drawn and by the bars' rules: none for a dead tank, none for a tank
   the view does not draw, none when the squares are too small. The state is
   the one at the playback time, copied out under the client mutex as the
   bars' is. */
static void lvDrawBuilderStatusOverBlit(const SDL_FRect *src, const SDL_FRect *dst) {
    LogViewerState *lv = lv_screenGetState();
    float scale, tilePx, pxScale, edge;
    int i;
    SDL_Rect clip, oldClip;
    bool hadClip;
    bool barsOn;
    SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
    Uint8 oldR, oldG, oldB, oldA;
    struct {
        bool alive;
        LvBuilderState state;
        int direction;      /* -1: at the tank's own place */
        bool sameSquare;
        bool hasTime;
        bool approx;
        long remainMs;
        unsigned char colour;
        bool hasTeam;
    } b[MAX_TANKS];

    if (!s_builderStatus || s_tankBarCount == 0 || lv == NULL) return;
    if (src->w <= 0.0f || src->h <= 0.0f) return;
    scale = dst->w / src->w;
    tilePx = (float)TILE_SIZE_X * scale;
    pxScale = lvDrawPixelScale();
    if (!lvTankBarsVisible(tilePx, pxScale)) return;

    lv_clientMutexWaitFor();
    /* The bars are drawn only for a recording that holds tank stocks; the
       marker stands on the tank when they are not. */
    barsOn = s_tankBars && lv_screenHasTankStock() != 0;
    for (i = 0; i < MAX_TANKS; i++) {
        bool isOut = FALSE, isDead = FALSE;
        BYTE team;

        b[i].alive = lv->gameViewHud[i].alive;
        lv_playersGetLgmStatus((BYTE)i, &isOut, &isDead);
        b[i].state = lvBuilderState(isOut != FALSE, isDead != FALSE);
        b[i].direction = -1;
        b[i].sameSquare = TRUE;
        b[i].hasTime = FALSE;
        b[i].approx = FALSE;
        b[i].remainMs = 0;
        if (!b[i].alive || b[i].state == LV_BUILDER_IN) continue;
        if (b[i].state == LV_BUILDER_OUT) {
            BYTE lx = 0, ly = 0, lpx = 0, lpy = 0, lframe = 0;
            BYTE tx = 0, ty = 0, tpx = 0, tpy = 0, tframe = 0;
            bool onBoat = FALSE;
            lv_playersGetLgmDetails((BYTE)i, &lx, &ly, &lpx, &lpy, &lframe);
            lv_playersGetTankDetails((BYTE)i, &tx, &ty, &tpx, &tpy, &tframe, &onBoat);
            b[i].sameSquare = (lx == tx && ly == ty);
            b[i].direction = lvBuilderDirection(
                (float)(lvBuilderWorld(lx, lpx) - lvBuilderWorld(tx, tpx)),
                (float)(lvBuilderWorld(ly, lpy) - lvBuilderWorld(ty, tpy)));
        }
        {
            long remain = 0;
            int approx = 0;
            if (lv_screenBuilderReturn(i, &remain, &approx)) {
                b[i].hasTime = TRUE;
                b[i].approx = approx != 0;
                b[i].remainMs = remain;
            }
        }
        team = lv_playersGetTeamForOwner((BYTE)i);
        b[i].hasTeam = (team != NEUTRAL_TEAM);
        b[i].colour = lv->tc[team];
    }
    lv_clientMutexRelease();

    hadClip = SDL_RenderClipEnabled(sdlRenderer);
    SDL_GetRenderClipRect(sdlRenderer, &oldClip);
    clip.x = (int)dst->x;
    clip.y = (int)dst->y;
    clip.w = (int)dst->w;
    clip.h = (int)dst->h;
    SDL_SetRenderClipRect(sdlRenderer, &clip);
    SDL_GetRenderDrawBlendMode(sdlRenderer, &oldBlend);
    SDL_GetRenderDrawColor(sdlRenderer, &oldR, &oldG, &oldB, &oldA);
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);
    /* The dark edge round every part: one point. */
    edge = lvTankBarPointsToPixels(LV_BUILDER_EDGE_POINTS, pxScale);

    for (i = 0; i < s_tankBarCount; i++) {
        BYTE slot = s_tankBarPos[i].slot;
        LvBuilderMarkLayout m;
        SDL_FColor mark, dark;
        char text[LV_BUILDER_TEXT_MAX];
        float x, y, rowW, textY;
        int pass;

        if (slot >= MAX_TANKS || !b[slot].alive || b[slot].state == LV_BUILDER_IN) continue;
        m = lvBuilderMarkLayout(dst->x + ((float)s_tankBarPos[i].x - src->x) * scale,
                                dst->y + ((float)s_tankBarPos[i].y - src->y) * scale,
                                tilePx, pxScale, barsOn);
        if (!m.visible) continue;

        text[0] = '\0';
        if (b[slot].hasTime) {
            lvBuilderCountdownText(b[slot].remainMs, b[slot].approx, text, sizeof(text));
        }
        rowW = (float)lvBuilderRowCells(text) * m.cell;
        x = SDL_floorf(m.centreX - rowW * 0.5f);
        y = m.top;
        /* The digits are 5 cells high in a row of 7: one cell down. */
        textY = y + m.cell;

        /* White for every team, with a black edge (lvBuilderMarkColour). */
        {
            LvBuilderColour w = lvBuilderMarkColour(b[slot].hasTeam, b[slot].colour);
            LvBuilderColour e = lvBuilderEdgeColour();
            mark.r = w.r; mark.g = w.g; mark.b = w.b; mark.a = w.a;
            dark.r = e.r; dark.g = e.g; dark.b = e.b; dark.a = e.a;
        }

        /* The dark edge first, then the white over it. */
        for (pass = 0; pass < 2; pass++) {
            float grow = (pass == 0) ? edge : 0.0f;
            SDL_FColor c = (pass == 0) ? dark : mark;

            SDL_SetRenderDrawColorFloat(sdlRenderer, c.r, c.g, c.b, c.a);
            if (b[slot].state == LV_BUILDER_FLYING) {
                lvDrawBuilderGlyph(lvBuilderParachute(), x, y, m.cell, grow);
            } else if (b[slot].direction < 0 || b[slot].sameSquare) {
                /* On the tank's own square: a dot in the middle of the box. */
                lvDrawBuilderGlyph(lvBuilderDot(), x + m.cell * 2.0f, y + m.cell * 2.0f,
                                   m.cell, grow);
            } else {
                float half = m.height * 0.5f;
                lvDrawBuilderArrow(x + half, y + half, half + grow * 1.5f,
                                   b[slot].direction, c);
            }
            if (text[0] != '\0') {
                lvDrawBuilderText(text, x + (float)(LV_BUILDER_CELLS + 1) * m.cell, textY,
                                  m.cell, grow);
            }
        }
    }

    SDL_SetRenderDrawColor(sdlRenderer, oldR, oldG, oldB, oldA);
    SDL_SetRenderDrawBlendMode(sdlRenderer, oldBlend);
    SDL_SetRenderClipRect(sdlRenderer, hadClip ? &oldClip : NULL);
}

/* The mark round the selected tank: four corners just outside its picture,
   white, 75% solid. It is drawn over the blit like the bars, with the same
   sums for the place, so it stays on its tank. It follows the rules of the
   bars: none for a dead tank, none for a tank the view does not draw, none
   when the squares are too small, and only in the viewer's own window. */
static void lvDrawTankMarkOverBlit(const SDL_FRect *src, const SDL_FRect *dst) {
    LogViewerState *lv = lv_screenGetState();
    float scale, tilePx, pxScale;
    int i, selected;
    bool alive;
    SDL_Rect clip, oldClip;
    bool hadClip;
    SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
    Uint8 oldR, oldG, oldB, oldA;

    if (s_tankBarCount == 0 || lv == NULL) return;
    if (src->w <= 0.0f || src->h <= 0.0f) return;
    lv_clientMutexWaitFor();
    selected = lv_screenSelectedTank();
    alive = (selected >= 0) ? lv->gameViewHud[selected].alive : FALSE;
    lv_clientMutexRelease();
    if (selected < 0 || !alive) return;
    scale = dst->w / src->w;
    tilePx = (float)TILE_SIZE_X * scale;
    pxScale = lvDrawPixelScale();
    if (!lvTankBarsVisible(tilePx, pxScale)) return;

    for (i = 0; i < s_tankBarCount; i++) {
        LvTankMark m;
        SDL_FRect r[8];
        if (s_tankBarPos[i].slot != (BYTE)selected) continue;
        m = lvTankMarkLayout(dst->x + ((float)s_tankBarPos[i].x - src->x) * scale,
                             dst->y + ((float)s_tankBarPos[i].y - src->y) * scale,
                             tilePx, pxScale);
        /* Each corner: an arm along the top or bottom, and one down or up
           the side. */
        r[0].x = m.x;                  r[0].y = m.y;                    r[0].w = m.arm;   r[0].h = m.thick;
        r[1].x = m.x;                  r[1].y = m.y;                    r[1].w = m.thick; r[1].h = m.arm;
        r[2].x = m.x + m.side - m.arm; r[2].y = m.y;                    r[2].w = m.arm;   r[2].h = m.thick;
        r[3].x = m.x + m.side - m.thick; r[3].y = m.y;                  r[3].w = m.thick; r[3].h = m.arm;
        r[4].x = m.x;                  r[4].y = m.y + m.side - m.thick; r[4].w = m.arm;   r[4].h = m.thick;
        r[5].x = m.x;                  r[5].y = m.y + m.side - m.arm;   r[5].w = m.thick; r[5].h = m.arm;
        r[6].x = m.x + m.side - m.arm; r[6].y = m.y + m.side - m.thick; r[6].w = m.arm;   r[6].h = m.thick;
        r[7].x = m.x + m.side - m.thick; r[7].y = m.y + m.side - m.arm; r[7].w = m.thick; r[7].h = m.arm;

        hadClip = SDL_RenderClipEnabled(sdlRenderer);
        SDL_GetRenderClipRect(sdlRenderer, &oldClip);
        clip.x = (int)dst->x;
        clip.y = (int)dst->y;
        clip.w = (int)dst->w;
        clip.h = (int)dst->h;
        SDL_SetRenderClipRect(sdlRenderer, &clip);
        SDL_GetRenderDrawBlendMode(sdlRenderer, &oldBlend);
        SDL_GetRenderDrawColor(sdlRenderer, &oldR, &oldG, &oldB, &oldA);
        SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255,
                               (Uint8)(LV_TANK_MARK_ALPHA * 255.0f + 0.5f));
        SDL_RenderFillRects(sdlRenderer, r, 8);
        SDL_SetRenderDrawColor(sdlRenderer, oldR, oldG, oldB, oldA);
        SDL_SetRenderDrawBlendMode(sdlRenderer, oldBlend);
        SDL_SetRenderClipRect(sdlRenderer, hadClip ? &oldClip : NULL);
        break;
    }
}

/* --- The ring round the place of an event the user went to ----------------
   A click on a line of the Events panel, or on a marker over the seek bar,
   takes playback to the event. The ring shows where on the map it happened:
   it closes in on the event's square twice over and fades, in LV_FLASH_MS
   of real time (lv_event_flash.h), so it runs with playback stopped too. The viewer's loop draws a frame every time round, and each
   frame comes through the blit, so the ring needs no redraw of its own.

   One ring at a time: a new click starts it again from the new place. It
   ends early when playback is moved by anything else (the seek count has
   moved on). It is in the viewer's own window, in the overview and in the
   game view: not embedded, not in a saved picture. A place the view does
   not show gets an arrow at the view's edge (lv_drawEventFlashAt). */
static struct {
    bool     on;
    int      mapX, mapY;
    int      team;       /* -1: no team, a white ring */
    Uint64   startMs;    /* SDL_GetTicks at the click */
    uint32_t seekSerial; /* lv_screenSeekSerial just after the jump */
} s_eventFlash;

void lv_drawEventFlashStart(int mapX, int mapY, int team) {
    s_eventFlash.on = true;
    s_eventFlash.mapX = mapX;
    s_eventFlash.mapY = mapY;
    s_eventFlash.team = team;
    s_eventFlash.startMs = SDL_GetTicks();
    s_eventFlash.seekSerial = lv_screenSeekSerial();
}

void lv_drawEventFlashCancel(void) {
    s_eventFlash.on = false;
}

int lv_drawEventFlashActive(void) {
    return s_eventFlash.on ? 1 : 0;
}

/* One band of the ring: the part between two radii, as a strip of
   triangles. */
#define LV_FLASH_SEGMENTS 48
static void lvDrawRingBand(float cx, float cy, float rIn, float rOut, SDL_FColor col) {
    SDL_Vertex v[(LV_FLASH_SEGMENTS + 1) * 2];
    int idx[LV_FLASH_SEGMENTS * 6];
    int i;

    if (rIn < 0.0f) rIn = 0.0f;
    if (rOut <= rIn) return;
    for (i = 0; i <= LV_FLASH_SEGMENTS; i++) {
        float a = (float)i * (2.0f * SDL_PI_F / (float)LV_FLASH_SEGMENTS);
        float c = SDL_cosf(a), s = SDL_sinf(a);
        v[i * 2].position.x = cx + c * rIn;
        v[i * 2].position.y = cy + s * rIn;
        v[i * 2].color = col;
        v[i * 2].tex_coord.x = 0.0f;
        v[i * 2].tex_coord.y = 0.0f;
        v[i * 2 + 1].position.x = cx + c * rOut;
        v[i * 2 + 1].position.y = cy + s * rOut;
        v[i * 2 + 1].color = col;
        v[i * 2 + 1].tex_coord.x = 0.0f;
        v[i * 2 + 1].tex_coord.y = 0.0f;
    }
    for (i = 0; i < LV_FLASH_SEGMENTS; i++) {
        idx[i * 6 + 0] = i * 2;
        idx[i * 6 + 1] = i * 2 + 1;
        idx[i * 6 + 2] = i * 2 + 2;
        idx[i * 6 + 3] = i * 2 + 1;
        idx[i * 6 + 4] = i * 2 + 3;
        idx[i * 6 + 5] = i * 2 + 2;
    }
    SDL_RenderGeometry(sdlRenderer, NULL, v, (LV_FLASH_SEGMENTS + 1) * 2, idx,
                       LV_FLASH_SEGMENTS * 6);
}

/* A filled triangle in one colour. */
static void lvDrawFlashTriangle(float ax, float ay, float bx, float by, float cx,
                                float cy, SDL_FColor col) {
    SDL_Vertex v[3];
    int i;
    v[0].position.x = ax; v[0].position.y = ay;
    v[1].position.x = bx; v[1].position.y = by;
    v[2].position.x = cx; v[2].position.y = cy;
    for (i = 0; i < 3; i++) {
        v[i].color = col;
        v[i].tex_coord.x = 0.0f;
        v[i].tex_coord.y = 0.0f;
    }
    SDL_RenderGeometry(sdlRenderer, NULL, v, 3, NULL, 0);
}

/* Draw the ring for a view of the map: (cx, cy) is where the middle of the
   event's square is on the screen, in pixels, tilePx the side of a map
   square there, and view the part of the screen the map is in. When the
   middle is outside the view (the view follows a tank and could not move to
   the event) an arrow at the view's edge points to the place instead, and
   pulses as the ring does. Both hosts of a view call this: the overview's
   blit below and the game view (game_view.c). */
void lv_drawEventFlashAt(float cx, float cy, float tilePx, const SDL_Rect *view) {
    LogViewerState *lv = lv_screenGetState();
    Uint64 now;
    uint32_t elapsed;
    float pxScale, radius, alpha, half, darkW;
    float ex = 0.0f, ey = 0.0f, dx = 0.0f, dy = 0.0f;
    SDL_Rect oldClip;
    bool hadClip;
    SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
    SDL_FColor white, dark, team;
    bool hasTeam;

    if (!s_eventFlash.on || view == NULL) return;
    now = SDL_GetTicks();
    elapsed = (now > s_eventFlash.startMs) ? (uint32_t)(now - s_eventFlash.startMs) : 0u;
    if (lv == NULL || !lv->isLoaded || !lvFlashActive(elapsed) ||
        s_eventFlash.seekSerial != lv_screenSeekSerial()) {
        s_eventFlash.on = false;
        return;
    }
    if (view->w <= 0 || view->h <= 0 || tilePx <= 0.0f) return;

    pxScale = lvDrawPixelScale();
    radius = lvFlashRadiusTiles(elapsed) * tilePx;
    alpha = lvFlashAlpha(elapsed);
    darkW = LV_FLASH_DARK_PT * pxScale;

    hadClip = SDL_RenderClipEnabled(sdlRenderer);
    SDL_GetRenderClipRect(sdlRenderer, &oldClip);
    SDL_SetRenderClipRect(sdlRenderer, view);
    SDL_GetRenderDrawBlendMode(sdlRenderer, &oldBlend);
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);

    white.r = 1.0f; white.g = 1.0f; white.b = 1.0f; white.a = alpha;
    /* A dark line each side, so the ring shows on pale ground too. */
    dark.r = 0.0f; dark.g = 0.0f; dark.b = 0.0f; dark.a = alpha * LV_FLASH_DARK_ALPHA;
    hasTeam = s_eventFlash.team >= 0;
    team = white;
    if (hasTeam) {
        /* The colour is the one the HUD panel gives the team. */
        team = mapColourTeam(lv_screenTeamColour(s_eventFlash.team));
        if (!lvHudRedToOrange(&team.r, &team.g, &team.b)) {
            lvHudReadableColour(&team.r, &team.g, &team.b);
        }
        team.a = alpha;
    }

    if (lvFlashEdgeArrow(cx, cy, (float)view->x, (float)view->y,
                         (float)(view->x + view->w), (float)(view->y + view->h),
                         LV_FLASH_ARROW_INSET_PT * pxScale, &ex, &ey, &dx, &dy)) {
        /* The place is outside the view: an arrow with its point at the
           edge. It moves in and out a little with the pulse. A dark arrow a
           little larger is under the white one, and the team's colour, when
           there is one, is a smaller arrow inside it. */
        float len = LV_FLASH_ARROW_LEN_PT * pxScale;
        float wid = LV_FLASH_ARROW_WIDTH_PT * pxScale * 0.5f;
        float back = (lvFlashRadiusTiles(elapsed) - LV_FLASH_END_TILES) /
                     (LV_FLASH_RADIUS_TILES - LV_FLASH_END_TILES) * len * 0.5f;
        float tx = ex - dx * back, ty = ey - dy * back;
        float bx = tx - dx * len, by = ty - dy * len;
        float nx = -dy, ny = dx;
        float grow = darkW * 2.0f;
        lvDrawFlashTriangle(tx + dx * grow, ty + dy * grow,
                            bx - dx * darkW + nx * (wid + grow), by - dy * darkW + ny * (wid + grow),
                            bx - dx * darkW - nx * (wid + grow), by - dy * darkW - ny * (wid + grow),
                            dark);
        lvDrawFlashTriangle(tx, ty, bx + nx * wid, by + ny * wid,
                            bx - nx * wid, by - ny * wid, white);
        if (hasTeam) {
            float in = LV_FLASH_WHITE_PT * pxScale;
            float tLen = len - in * 3.0f, tWid = wid - in * 1.5f;
            if (tLen > 0.0f && tWid > 0.0f) {
                float sx = tx - dx * in * 2.0f, sy = ty - dy * in * 2.0f;
                float cbx = sx - dx * tLen, cby = sy - dy * tLen;
                lvDrawFlashTriangle(sx, sy, cbx + nx * tWid, cby + ny * tWid,
                                    cbx - nx * tWid, cby - ny * tWid, team);
            }
        }
    } else if (hasTeam) {
        /* A white band outside, the team's colour inside it. */
        float whiteW = LV_FLASH_WHITE_PT * pxScale;
        float teamW = LV_FLASH_TEAM_PT * pxScale;
        half = (whiteW + teamW) * 0.5f;
        lvDrawRingBand(cx, cy, radius - half - darkW, radius + half + darkW, dark);
        lvDrawRingBand(cx, cy, radius + half - whiteW, radius + half, white);
        lvDrawRingBand(cx, cy, radius - half, radius - half + teamW, team);
    } else {
        half = LV_FLASH_RING_PT * pxScale * 0.5f;
        lvDrawRingBand(cx, cy, radius - half - darkW, radius + half + darkW, dark);
        lvDrawRingBand(cx, cy, radius - half, radius + half, white);
    }

    SDL_SetRenderDrawBlendMode(sdlRenderer, oldBlend);
    SDL_SetRenderClipRect(sdlRenderer, hadClip ? &oldClip : NULL);
}

/* The place of the ring, as a map square; 0 when no ring is on. For a host
   that works out where the square is on its own screen (the game view). */
int lv_drawEventFlashCell(int *mapX, int *mapY) {
    if (!s_eventFlash.on) return 0;
    if (mapX != NULL) *mapX = s_eventFlash.mapX;
    if (mapY != NULL) *mapY = s_eventFlash.mapY;
    return 1;
}

/* Draw the ring over the blit: src is the slice of the target the blit took,
   dst where it went. The place is worked out as the blit works out a tank's:
   a map square is (square - view offset) squares into the target. */
static void lvDrawEventFlashOverBlit(const SDL_FRect *src, const SDL_FRect *dst) {
    float scale, cx, cy;
    BYTE offX = 0, offY = 0;
    BYTE zoomFactor;
    SDL_Rect view;

    if (!s_eventFlash.on) return;
    if (src->w <= 0.0f || src->h <= 0.0f) return;

    scale = dst->w / src->w;
    zoomFactor = lv_windowGetZoomFactor();
    lv_screenGetOffsets(&offX, &offY);
    /* The middle of the event's square, in the target and then on screen. */
    cx = ((float)(s_eventFlash.mapX - (int)offX) + 0.5f) * (float)(zoomFactor * TILE_SIZE_X);
    cy = ((float)(s_eventFlash.mapY - (int)offY) + 0.5f) * (float)(zoomFactor * TILE_SIZE_Y);
    cx = dst->x + (cx - src->x) * scale;
    cy = dst->y + (cy - src->y) * scale;
    view.x = (int)dst->x;
    view.y = (int)dst->y;
    view.w = (int)dst->w;
    view.h = (int)dst->h;
    lv_drawEventFlashAt(cx, cy, (float)(zoomFactor * TILE_SIZE_X) * scale, &view);
}

/* The slot of the tank drawn at a point of the window, or -1 for none. The
   point is in points, as the mouse is. The test is against where the last
   blit put each tank's picture (lv_tank_pick.h): a tank the view does not
   draw is not in that list, and a dead one is passed over. */
int lv_drawTankAtPoint(float xPoints, float yPoints) {
    LogViewerState *lv = lv_screenGetState();
    LvTankPickBox boxes[MAX_TANKS];
    float scale, tilePx, pxScale, px, py;
    int i, found;

    if (!s_blitKnown || lv == NULL || s_tankBarCount == 0) return -1;
    if (s_blitSrc.w <= 0.0f || s_blitDst.w <= 0.0f) return -1;
    pxScale = lvDrawPixelScale();
    px = xPoints * pxScale;
    py = yPoints * pxScale;
    if (px < s_blitDst.x || py < s_blitDst.y ||
        px >= s_blitDst.x + s_blitDst.w || py >= s_blitDst.y + s_blitDst.h) {
        return -1;
    }
    scale = s_blitDst.w / s_blitSrc.w;
    tilePx = (float)TILE_SIZE_X * scale;
    lv_clientMutexWaitFor();
    for (i = 0; i < s_tankBarCount; i++) {
        BYTE slot = s_tankBarPos[i].slot;
        boxes[i].x = s_blitDst.x + ((float)s_tankBarPos[i].x - s_blitSrc.x) * scale;
        boxes[i].y = s_blitDst.y + ((float)s_tankBarPos[i].y - s_blitSrc.y) * scale;
        boxes[i].slot = slot;
        boxes[i].pickable = (slot < MAX_TANKS) && lv->gameViewHud[slot].alive &&
                            lv_playersIsInUse(slot) == TRUE;
    }
    lv_clientMutexRelease();
    found = lvTankPick(boxes, s_tankBarCount, px, py, tilePx, pxScale);
    return (found >= 0) ? boxes[found].slot : -1;
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

/* The strip of grey status shapes built beside the atlas (buildHudShapes).
 * NULL until the setup has run and when the strip could not be made. */
SDL_Texture* lv_drawGetHudShapesTexture(void) {
    return textureHudShapes;
}

/* The game's background picture, the one the game client draws its status
 * column from. Loaded from data/background.bmp beside the program the first
 * time it is asked for. NULL before the renderer exists and when the file
 * is not there; the load is tried once. */
SDL_Texture* lv_drawGetStatusBackgroundTexture(void) {
    if (textureStatusBackground == NULL && !statusBackgroundTried && sdlRenderer != NULL) {
        const char *basePath = SDL_GetBasePath();
        char path[1024];
        statusBackgroundTried = true;
        if (basePath != NULL) {
            SDL_snprintf(path, sizeof(path), "%sdata/background.bmp", basePath);
            textureStatusBackground = sdlLoadBmpAsTexture(sdlRenderer, path);
        }
        if (textureStatusBackground == NULL) {
            textureStatusBackground = sdlLoadBmpAsTexture(sdlRenderer, "data/background.bmp");
        }
        if (textureStatusBackground != NULL) {
            SDL_SetTextureScaleMode(textureStatusBackground, SDL_SCALEMODE_NEAREST);
        }
    }
    return textureStatusBackground;
}

/* Atlas is always built at scale 1 (see buildTileAtlas()) — the blit-time
 * scaling is in the texture target, not the source atlas. */
int lv_drawGetSheetScale(void) {
    return 1;
}
