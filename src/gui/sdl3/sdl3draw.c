/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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
*Name:          SDL3 Draw Mirror
*Filename:      sdl3draw.c
*Purpose:
*  Phase 1-5 SDL3 mirror renderer. Opens a second window
*  alongside the Win32/DirectDraw window. Phase 2 adds the
*  background bitmap. Phase 3 adds scrolling map tiles.
*  Phase 4 adds sprites (shells/tanks/LGMs), status panel
*  render targets, bar textures and man-status circle.
*  Phase 5 adds text via SDL_ttf: tank labels, messages,
*  kills/deaths counter, pill-view and net-failed overlays.
*  Functions are prefixed sdl3Draw* to avoid symbol
*  collision while both renderers coexist.
*********************************************************/

/* Must be included before any bolo headers that use #pragma pack */
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../common/wb_log.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

#include "stb_image.h"
#include "sdl3draw.h"
#include "sdl3draw_status.h"
#include "sdl3imgui.h"
#include "cursor.h"
#include "mapview.h"
#include "../clientmutex.h"
#include "../tiles.h"
#include "../ui_mode.h"
#include "tileloader.h"
#include "sdl_bmp.h"
#include "glyphs.h"
#include "../../bolo/global.h"
#include "../../bolo/screen.h"
#include "../../bolo/client_sim.h"
#include "../../bolo/tank.h"
#include "../gamefront.h"
#include "../../bolo/tilenum.h"
#include "../positions.h"
#include "../../bolo/screenbullet.h"
#include "../../bolo/screentank.h"
#include "../../bolo/screenlgm.h"
#include "macos_pinch.h"

/* From gui/winbolo.h (can't include directly — Win32 headers) */
#ifndef NO_SELECT
#define NO_SELECT -1
#endif
#ifndef ZOOM_FACTOR_CUSTOM
#define ZOOM_FACTOR_CUSTOM 0
#endif
#ifndef MENU_BAR_HEIGHT
#define MENU_BAR_HEIGHT 22
#endif

static SDL_Window   *gWindow        = NULL;
static SDL_Renderer *gRenderer      = NULL;
static SDL_Texture  *gBackgroundTex = NULL;
static SDL_Texture  *gTilesTex      = NULL;
static SDL_Texture  *gCrosshairTex  = NULL;  /* crosshairs_17x17.png — center pixel (8,8) is aim point */
static int           gZoomFactor    = 1;
static int           gSheetScale    = 1;  /* atlas scale: sheet is TILE_FILE * gSheetScale */

/* Phase 4 render-target textures.
   Status icon panels (bases/pills/tanks) are drawn directly to the
   framebuffer each frame rather than cached in off-screen textures,
   which avoids render-target-switching issues. */
static SDL_Texture *gManStatusTex   = NULL;  /* MAN_STATUS_WIDTH*zf x MAN_STATUS_HEIGHT*zf */
static SDL_Texture *gTankBarsTex    = NULL;  /* TOTALWIDTH x HEIGHT */
static SDL_Texture *gBaseBarsTex    = NULL;  /* MAX_WIDTH x TOTALHEIGHT */

/* Render target for game content — allows game to scale while menu stays 1x.
   The game is rendered to this texture at logical size, then blitted scaled
   to the window below the menu bar. */
static SDL_Texture *gGameRenderTarget = NULL;
static int          gGameRTWidth      = 0;   /* Render target dimensions */
static int          gGameRTHeight     = 0;

/* Where the scaled game content is blitted in window coordinates.
   Set each frame after calculating aspect-preserving scale. */
static SDL_FRect    gGameDestRect = {0, 0, 0, 0};
static float        gGameScale    = 1.0f;    /* Scale factor from RT to dest */

static buildSelect  gCurrentBuildSelect = BsTrees;

/* Frame rate counting (mirrors g_dwFrame* in win32/draw.c) */
static DWORD g_dwFrameTime  = 0;
static DWORD g_dwFrameCount = 0;
static DWORD g_dwFrameTotal = 0;

/* Phase 5 — SDL_ttf fonts.
   gFontMsg  : newswire / overlay text (slightly smaller — fits more chars)
   gFontKD   : kills/deaths counters   (slightly larger  — easier to read) */
static TTF_Font    *gFontMsg  = NULL;
static TTF_Font    *gFontKD   = NULL;
static TTF_Font    *gFontTiny  = NULL;  /* tiny font for pill/base status panel labels */
static TTF_Font    *gFontLabel = NULL;  /* larger font for pill/base labels in main view */
/* SarasaMonoK chained via TTF_AddFallbackFont so hangul renders when
 * the primary is J/SC/TC. Owned here; closed alongside the primaries. */
static TTF_Font    *gFallbackFontMsg   = NULL;
static TTF_Font    *gFallbackFontKD    = NULL;
static TTF_Font    *gFallbackFontTiny  = NULL;
static TTF_Font    *gFallbackFontLabel = NULL;

/* Last scroll offsets used when rendering tank labels (set each frame).
   Tank-label cache (gLabelTex/gLabelStr) moved to sdl3draw_status.c. */
static int          gCurrentEdgeX = 0;
static int          gCurrentEdgeY = 0;

/* Network-failed flag: set by sdl3DrawSetNetFailed() before each frame */
static bool         gNetFailed    = false;

/* Death static effect state — pixel-based Bolo-style PRNG noise */
static int          gStaticLast   = 0;
static uint32_t     gStaticSeed   = 1;
static SDL_Texture *gStaticTex    = NULL;
static int          gStaticTexW   = 0;
static int          gStaticTexH   = 0;

static uint32_t getRandomStaticNoiseSeed(void) {
  gStaticSeed ^= gStaticSeed << 13;
  gStaticSeed ^= gStaticSeed >> 17;
  gStaticSeed ^= gStaticSeed << 5;
  return gStaticSeed;
}

/* Guard: only blit gManStatusTex after sdl3DrawSetManStatus has drawn into it.
   Prevents a one-frame artifact where the LGM arrow points top-left before
   the first real angle is computed. */
static bool         gManStatusReady = false;
static bool         gManStatusDead  = false;
static TURNTYPE     gManStatusAngle = 0;

/* Tablet viewport bounds — set each frame by sdl3DrawMainScreen(),
   read by sdl3DrawGetTabletViewport(). */
static int          gTabletVpX = 0, gTabletVpY = 0;
static int          gTabletVpW = 0, gTabletVpH = 0;
static int          gTabletVpZoom = 0;

/* Drag scroll pixel offset — applied on top of engine edgeX/edgeY
   for smooth sub-tile scrolling from touch dragging. */
static int          gDragOffsetX = 0;
static int          gDragOffsetY = 0;

/* Configurable status panel origins (zoomed pixel coords).
   -1 means "use desktop default" (zf * STATUS_*_LEFT/TOP).
   Set by sdl3DrawSetStatusPanelOrigins() for tablet mode. */
static float        gStatusTanksOrgX = -1, gStatusTanksOrgY = -1;
static float        gStatusPillsOrgX = -1, gStatusPillsOrgY = -1;
static float        gStatusBasesOrgX = -1, gStatusBasesOrgY = -1;

/* Cached text caches (gMsgTop/Bottom, gCachedKills/Deaths) and their
   render-target texture caches (gTexMsg*, gTexKills, gTexDeaths) moved
   to sdl3draw_status.c. */

/* Query safe area insets in renderer coordinates.
   Returns left/top/right/bottom insets (pixels). */
static void sdl3GetSafeAreaInsets(float *outLeft, float *outTop,
                                   float *outRight, float *outBottom) {
  float l = 0, t = 0, r = 0, b = 0;
  if (gWindow) {
    SDL_Rect safeRect;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(gWindow, &winW, &winH);
    if (SDL_GetWindowSafeArea(gWindow, &safeRect) && winW > 0 && winH > 0) {
      int renW = 0, renH = 0;
      SDL_RendererLogicalPresentation logMode;
      SDL_GetRenderLogicalPresentation(gRenderer, &renW, &renH, &logMode);
      if (renW <= 0 || renH <= 0) {
        SDL_GetCurrentRenderOutputSize(gRenderer, &renW, &renH);
      }
      if (renW > 0 && renH > 0) {
        l = (float)safeRect.x * (float)renW / (float)winW;
        t = (float)safeRect.y * (float)renH / (float)winH;
        r = (float)(winW - safeRect.x - safeRect.w) * (float)renW / (float)winW;
        b = (float)(winH - safeRect.y - safeRect.h) * (float)renH / (float)winH;
      }
    }
  }
  if (outLeft) *outLeft = l;
  if (outTop) *outTop = t;
  if (outRight) *outRight = r;
  if (outBottom) *outBottom = b;
}

/* Source-rect lookup tables for tiles now live in mapview.c (mapViewPosX/Y). */

/* SDL3_SCREEN_W / SDL3_SCREEN_H are defined in sdl3draw.h */


/*********************************************************
*NAME:          sdl3RenderText
*PURPOSE:
*  Renders a text string with the game font directly onto
*  the current render target at (x, y) in screen pixels.
*  No-op if font is NULL or text is NULL/empty.
*********************************************************/
static void sdl3RenderText(TTF_Font *font, const char *text, SDL_Color fg, float x, float y) {
  if (!font || !gRenderer || !text || text[0] == '\0') return;

  SDL_Surface *sSurf = TTF_RenderText_Blended(font, text, 0, fg);
  if (!sSurf) return;
  SDL_Texture *tTex = SDL_CreateTextureFromSurface(gRenderer, sSurf);
  if (tTex) {
    SDL_FRect d = { x, y, (float)sSurf->w, (float)sSurf->h };
    SDL_RenderTexture(gRenderer, tTex, NULL, &d);
    SDL_DestroyTexture(tTex);
  }
  SDL_DestroySurface(sSurf);
}

/* sdl3UpdateTextCache moved to sdl3draw_status.c (private helper for
   sdl3RenderCachedText). */

/* sdl3SetupDrawArrays moved to mapview.c as mapViewInit() */
/* (old sdl3SetupDrawArrays body removed — now in mapview.c) */
/* Builds the sprite sheet from individual SVG/PNG files (with BMP fallback)
 * and creates gTilesTex from the assembled surface. */
static bool sdl3LoadTiles(void) {
  if (gTilesTex != NULL) {
    return TRUE;
  }

  /* Atlas scale = gZoomFactor.  With HIGH_PIXEL_DENSITY on tablet,
     gZoomFactor is already set to the native-pixel effective zoom
     (e.g. 5 on iPhone 17 Pro).  SVGs rasterize at gZoomFactor*16
     per tile for near-1:1 mapping to screen pixels. */
  int atlasZoom = gZoomFactor;
  if (atlasZoom < 1) atlasZoom = 1;
  gSheetScale = atlasZoom;

  SDL_Surface *sheet = tileLoaderBuildSheet(TILE_SIZE_X * atlasZoom);
  if (!sheet) {
    WB_LOG_ERROR(WB_LOG_CAT_ASSET, "sdl3LoadTiles: tileLoaderBuildSheet failed");
    gSheetScale = 1;
    return FALSE;
  }

  WB_LOG_INFO(WB_LOG_CAT_ASSET, "sdl3LoadTiles: atlas scale=%d, sheet=%dx%d",
          atlasZoom, sheet->w, sheet->h);

  gTilesTex = SDL_CreateTextureFromSurface(gRenderer, sheet);
  SDL_DestroySurface(sheet);
  if (gTilesTex == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_ASSET, "sdl3LoadTiles: SDL_CreateTextureFromSurface failed: %s", SDL_GetError());
    gSheetScale = 1;
    return FALSE;
  }
  SDL_SetTextureBlendMode(gTilesTex, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(gTilesTex, SDL_SCALEMODE_NEAREST);
  sdl3DrawStatusSetAtlas(gTilesTex, gSheetScale);
  return TRUE;
}

/* Loads data/background.bmp as gBackgroundTex. */
static bool sdl3LoadBackground(void) {
  if (gBackgroundTex != NULL) {
    return TRUE;
  }

  gBackgroundTex = sdlLoadBmpAsTexture(gRenderer, "data/background.bmp", false);
  if (gBackgroundTex == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_ASSET, "sdl3DrawBackground: could not load background.bmp: %s", SDL_GetError());
    return FALSE;
  }
  SDL_SetTextureScaleMode(gBackgroundTex, SDL_SCALEMODE_NEAREST);
  return TRUE;
}

/*********************************************************
*NAME:          sdl3CreateRenderTarget
*PURPOSE:
*  Helper: create a render-target texture of given size,
*  clear it to black, and set blend mode on it.
*  Returns NULL on failure.
*********************************************************/
static SDL_Texture *sdl3CreateRenderTarget(int w, int h) {
  SDL_Texture *tex = SDL_CreateTexture(gRenderer,
                                       SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_TARGET,
                                       w, h);
  if (tex == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_GUI, "sdl3CreateRenderTarget: failed %dx%d: %s", w, h, SDL_GetError());
    return NULL;
  }
  /* No premultiplied alpha when drawing into the target */
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  SDL_SetRenderTarget(gRenderer, tex);
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */
  SDL_SetRenderTarget(gRenderer, NULL);
  return tex;
}

/*********************************************************
*NAME:          sdl3RenderStatusPanels
*PURPOSE:
*  Blits all persistent status-panel textures to the
*  main framebuffer at their zoomed screen positions,
*  then draws the current build-select indent overlay.
*********************************************************/
static void sdl3RenderStatusPanels(void) {
  /* In tablet mode the ImGui overlay draws its own resource bars,
     build-select bar, and man-status — skip the desktop versions. */
  if (uiModeIsTablet()) return;

  int zf = gZoomFactor;

  /* Status icon panels (bases/pills/tanks) are drawn directly to the
     framebuffer by sdl3DrawSet*StatusClear / sdl3DrawStatus* calls that
     occur before sdl3RenderStatusPanels in each frame.  Only the
     man-status circle and resource bars still use render-target textures. */

  if (gManStatusTex && gManStatusReady) {
    SDL_SetTextureBlendMode(gManStatusTex, SDL_BLENDMODE_BLEND);
    /* Texture is (MAN_STATUS_WIDTH+2)*(MAN_STATUS_HEIGHT+2)*zf to give the
       circle 1px padding on each edge — blit at same screen position but
       allow the extra pixels to show by expanding the dest rect by 2*zf. */
    SDL_FRect d = { (float)(zf * MAN_STATUS_X), (float)(zf * MAN_STATUS_Y),
                    (float)(zf * (MAN_STATUS_WIDTH  + 2)),
                    (float)(zf * (MAN_STATUS_HEIGHT + 2)) };
    SDL_RenderTexture(gRenderer, gManStatusTex, NULL, &d);
  }
  if (gTankBarsTex) {
    SDL_SetTextureBlendMode(gTankBarsTex, SDL_BLENDMODE_NONE);
    SDL_FRect d = { (float)(zf * STATUS_TANK_SHELLS), (float)(zf * STATUS_TANK_BARS_TOP),
                    (float)(zf * STATUS_TANK_BARS_TOTALWIDTH), (float)(zf * STATUS_TANK_BARS_HEIGHT) };
    SDL_RenderTexture(gRenderer, gTankBarsTex, NULL, &d);
  }
  if (gBaseBarsTex) {
    SDL_SetTextureBlendMode(gBaseBarsTex, SDL_BLENDMODE_NONE);
    SDL_FRect d = { (float)(zf * STATUS_BASE_BARS_LEFT), (float)(zf * STATUS_BASE_BARS_TOP),
                    (float)(zf * STATUS_BASE_BARS_MAX_WIDTH), (float)(zf * STATUS_BASE_BARS_TOTALHEIGHT) };
    SDL_RenderTexture(gRenderer, gBaseBarsTex, NULL, &d);
  }

  /* Draw current build-select indent (ON tile) over the background */
  if (gTilesTex) {
    int bx, by, dotX, dotY;
    switch (gCurrentBuildSelect) {
      case BsTrees:
        bx = BS_TREE_OFFSET_X; by = BS_TREE_OFFSET_Y;
        dotX = BS_DOT_TREE_OFFSET_X; dotY = BS_DOT_TREE_OFFSET_Y;
        break;
      case BsRoad:
        bx = BS_ROAD_OFFSET_X; by = BS_ROAD_OFFSET_Y;
        dotX = BS_DOT_ROAD_OFFSET_X; dotY = BS_DOT_ROAD_OFFSET_Y;
        break;
      case BsBuilding:
        bx = BS_BUILDING_OFFSET_X; by = BS_BUILDING_OFFSET_Y;
        dotX = BS_DOT_BUILDING_OFFSET_X; dotY = BS_DOT_BUILDING_OFFSET_Y;
        break;
      case BsPillbox:
        bx = BS_PILLBOX_OFFSET_X; by = BS_PILLBOX_OFFSET_Y;
        dotX = BS_DOT_PILLBOX_OFFSET_X; dotY = BS_DOT_PILLBOX_OFFSET_Y;
        break;
      default: /* BsMine */
        bx = BS_MINE_OFFSET_X; by = BS_MINE_OFFSET_Y;
        dotX = BS_DOT_MINE_OFFSET_X; dotY = BS_DOT_MINE_OFFSET_Y;
        break;
    }
    /* Large indent tile */
    int ss = gSheetScale;
    SDL_FRect iSrc = { (float)(INDENT_ON_X * ss), (float)(INDENT_ON_Y * ss),
                       (float)(BS_ITEM_SIZE_X * ss), (float)(BS_ITEM_SIZE_Y * ss) };
    SDL_FRect iDst = { (float)(zf * bx), (float)(zf * by),
                       (float)(zf * BS_ITEM_SIZE_X), (float)(zf * BS_ITEM_SIZE_Y) };
    SDL_RenderTexture(gRenderer, gTilesTex, &iSrc, &iDst);

    /* Small dot tile */
    SDL_FRect dSrc = { (float)(INDENT_DOT_ON_X * ss), (float)(INDENT_DOT_ON_Y * ss),
                       (float)(BS_DOT_ITEM_SIZE_X * ss), (float)(BS_DOT_ITEM_SIZE_Y * ss) };
    SDL_FRect dDst = { (float)(zf * dotX), (float)(zf * dotY),
                       (float)(zf * BS_DOT_ITEM_SIZE_X), (float)(zf * BS_DOT_ITEM_SIZE_Y) };
    SDL_RenderTexture(gRenderer, gTilesTex, &dSrc, &dDst);
  }
}

/* sdl3StatusItemPos and sdl3RenderCachedText moved to sdl3draw_status.c.
   sdl3RenderCachedText is now public so sdl3DrawMainScreen can still
   call it via sdl3draw_status.h. */

int sdl3DrawGetZoomFactor(void) {
  return gZoomFactor;
}

SDL_Window *sdl3DrawGetWindow(void) {
  return gWindow;
}

SDL_Renderer *sdl3DrawGetRenderer(void) {
  return gRenderer;
}

SDL_Texture *sdl3DrawGetTilesTexture(void) {
  return gTilesTex;
}

int sdl3DrawGetSheetScale(void) {
  return gSheetScale;
}

SDL_Texture *sdl3DrawGetManStatusTexture(bool *ready) {
  if (ready) *ready = gManStatusReady;
  return gManStatusTex;
}

bool sdl3DrawGetManStatusState(bool *isDead, TURNTYPE *angle) {
  if (!gManStatusReady) return false;
  if (isDead) *isDead = gManStatusDead;
  if (angle)  *angle  = gManStatusAngle;
  return true;
}

void sdl3DrawDisableLogicalPresentation(void) {
  if (gRenderer) {
    SDL_SetRenderLogicalPresentation(gRenderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
  }
}

void sdl3DrawRestoreLogicalPresentation(void) {
  if (!gRenderer) return;
#ifdef __ANDROID__
  if (!uiModeIsTablet()) {
    SDL_SetRenderLogicalPresentation(gRenderer,
                                     gZoomFactor * SDL3_SCREEN_W,
                                     gZoomFactor * SDL3_SCREEN_H,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    return;
  }
#endif
#if defined(__IPHONEOS__) || defined(__ANDROID__)
  if (uiModeIsTablet()) {
    /* Re-apply the mobile tablet logical presentation (same logic as setup).
       Always set logical presentation so game rendering and ImGui share
       the same coordinate space (critical with HIGH_PIXEL_DENSITY). */
    int ww, wh;
    SDL_GetCurrentRenderOutputSize(gRenderer, &ww, &wh);
    int gameUnit = MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y;
    int curZoom = wh / gameUnit;
    if (curZoom < 1) curZoom = 1;
    int used = gameUnit * curZoom;
    int bestZoom = curZoom;
    if ((wh - used) * 100 / wh > 15) {
      bestZoom = curZoom + 1;
    }
    int logH = gameUnit * bestZoom;
    int logW = ww * logH / wh;
    SDL_SetRenderLogicalPresentation(gRenderer, logW, logH,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
  }
#endif
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(__IPHONEOS__)
  /* Desktop mode: no logical presentation needed.
     Game is rendered to a texture and blitted scaled to the window.
     No-op here — the render target approach handles coordinate transformation. */
  (void)0;
#endif
}

/* Transform window coordinates to game logical coordinates.
   Returns false if the point is outside the game area (in letterbox/pillarbox). */
static bool windowToGameCoords(float winX, float winY, float *gameX, float *gameY) {
  if (!gRenderer) return false;

  /* Desktop mode with render target: use gGameDestRect for transformation */
  if (gGameRenderTarget != NULL && gGameDestRect.w > 0 && gGameDestRect.h > 0) {
    /* Check if point is inside the scaled game area */
    if (winX < gGameDestRect.x || winX >= gGameDestRect.x + gGameDestRect.w ||
        winY < gGameDestRect.y || winY >= gGameDestRect.y + gGameDestRect.h) {
      return false;
    }
    /* Transform: subtract offset, then scale down to logical coords */
    *gameX = (winX - gGameDestRect.x) / gGameScale;
    *gameY = (winY - gGameDestRect.y) / gGameScale;
    return true;
  }

  /* Fallback: check SDL logical presentation (mobile/tablet modes) */
  SDL_FRect logRect;
  if (!SDL_GetRenderLogicalPresentationRect(gRenderer, &logRect)) {
    /* No logical presentation - coords are 1:1 */
    *gameX = winX;
    *gameY = winY;
    return true;
  }

  /* Check if point is inside the game area */
  if (winX < logRect.x || winX >= logRect.x + logRect.w ||
      winY < logRect.y || winY >= logRect.y + logRect.h) {
    return false;
  }

  /* Get logical size */
  int logW = 0, logH = 0;
  SDL_RendererLogicalPresentation logMode;
  SDL_GetRenderLogicalPresentation(gRenderer, &logW, &logH, &logMode);
  if (logW <= 0 || logH <= 0) {
    *gameX = winX;
    *gameY = winY;
    return true;
  }

  /* Transform: subtract offset, then scale */
  float scale = logRect.w / (float)logW;
  *gameX = (winX - logRect.x) / scale;
  *gameY = (winY - logRect.y) / scale;
  return true;
}

void sdl3DrawHandleEvent(ClientSim *cs, SDL_Event *ev) {
  if (!ev) return;
  switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION: {
      /* Transform window coords to game coords */
      float gameX, gameY;
      if (!windowToGameCoords(ev->motion.x, ev->motion.y, &gameX, &gameY)) {
        screenSetCursorPosCS(cs, 0, 0);
        break;
      }
      cursorMove((int)gameX, (int)gameY);
      BYTE cx = 0, cy = 0;
      if (cursorPos(NULL, &cx, &cy)) {
        if (cx > 16 || cy > 16) cx = 100;
        screenSetCursorPosCS(cs, cx, cy);
      } else {
        screenSetCursorPosCS(cs, 0, 0);
      }
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
      if (ev->button.button == SDL_BUTTON_LEFT) {
        BYTE xVal = 0, yVal = 0;
        if (cursorPos(NULL, &xVal, &yVal)) {
          clientMutexWaitFor();
          screenManMoveCS(cs, getBuildCurrentSelectCS(cs));
          clientMutexRelease();
        } else {
          /* Check if click landed on one of the 5 build-select buttons */
          float gx, gy;
          if (!windowToGameCoords(ev->button.x, ev->button.y, &gx, &gy)) break;
          int xPos = (int)gx;
          int yPos = (int)gy;
          int zf = gZoomFactor;
          buildSelect newSelect = NO_SELECT;
          if (xPos >= zf * BS_TREE_OFFSET_X && xPos <= zf * (BS_TREE_OFFSET_X + BS_ITEM_SIZE_X) &&
              yPos >= zf * BS_TREE_OFFSET_Y && yPos <= zf * (BS_TREE_OFFSET_Y + BS_ITEM_SIZE_Y)) {
            newSelect = BsTrees;
          } else if (xPos >= zf * BS_ROAD_OFFSET_X && xPos <= zf * (BS_ROAD_OFFSET_X + BS_ITEM_SIZE_X) &&
                     yPos >= zf * BS_ROAD_OFFSET_Y && yPos <= zf * (BS_ROAD_OFFSET_Y + BS_ITEM_SIZE_Y)) {
            newSelect = BsRoad;
          } else if (xPos >= zf * BS_BUILDING_OFFSET_X && xPos <= zf * (BS_BUILDING_OFFSET_X + BS_ITEM_SIZE_X) &&
                     yPos >= zf * BS_BUILDING_OFFSET_Y && yPos <= zf * (BS_BUILDING_OFFSET_Y + BS_ITEM_SIZE_Y)) {
            newSelect = BsBuilding;
          } else if (xPos >= zf * BS_PILLBOX_OFFSET_X && xPos <= zf * (BS_PILLBOX_OFFSET_X + BS_ITEM_SIZE_X) &&
                     yPos >= zf * BS_PILLBOX_OFFSET_Y && yPos <= zf * (BS_PILLBOX_OFFSET_Y + BS_ITEM_SIZE_Y)) {
            newSelect = BsPillbox;
          } else if (xPos >= zf * BS_MINE_OFFSET_X && xPos <= zf * (BS_MINE_OFFSET_X + BS_ITEM_SIZE_X) &&
                     yPos >= zf * BS_MINE_OFFSET_Y && yPos <= zf * (BS_MINE_OFFSET_Y + BS_ITEM_SIZE_Y)) {
            newSelect = BsMine;
          }
          if (newSelect != NO_SELECT && newSelect != getBuildCurrentSelectCS(cs)) {
            sdl3DrawSelectIndentsOff(getBuildCurrentSelectCS(cs), 0, 0);
            sdl3DrawSelectIndentsOn(newSelect, 0, 0);
            setBuildCurrentSelectCS(cs, newSelect);
          }
        }
      }
      break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      cursorLeaveWindow();
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      cursorAcquireCursor();
      break;
  }
}

/* Set the window icon (taskbar / title bar) from data/icons/bolo-icon.png.
 * macOS Cocoa ignores this — the .icns in the bundle drives the Dock icon. */
static void sdl3DrawSetWindowIcon(SDL_Window *window) {
  if (!window) return;

  SDL_IOStream *io = SDL_IOFromFile("data/icons/bolo-icon.png", "rb");
  if (!io) {
    const char *base = SDL_GetBasePath();
    if (!base) base = "./";
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%sdata/icons/bolo-icon.png", base);
    io = SDL_IOFromFile(path, "rb");
  }
  if (!io) return;

  Sint64 fileSize = SDL_GetIOSize(io);
  if (fileSize <= 0) { SDL_CloseIO(io); return; }
  unsigned char *buf = (unsigned char *)SDL_malloc((size_t)fileSize);
  if (!buf) { SDL_CloseIO(io); return; }
  SDL_ReadIO(io, buf, (size_t)fileSize);
  SDL_CloseIO(io);

  int w, h, channels;
  unsigned char *pixels = stbi_load_from_memory(buf, (int)fileSize, &w, &h, &channels, 4);
  SDL_free(buf);
  if (!pixels) return;

  SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
  if (surf) {
    SDL_SetWindowIcon(window, surf);
    SDL_DestroySurface(surf);
  }
  stbi_image_free(pixels);
}

/* -------------------------------------------------------
 * Loading screen — show smalllogo-transparent.png centered
 * on black.  Called immediately after window/renderer
 * creation so the user sees something while fonts, tiles
 * and audio load.
 * ------------------------------------------------------- */
static void sdl3DrawShowLoadingScreen(void) {
  if (!gRenderer) return;

  /* Load the logo PNG via stb_image */
  const char *basePath = SDL_GetBasePath();
  if (!basePath) basePath = "./";
  char logoPath[1024];
  SDL_snprintf(logoPath, sizeof(logoPath), "%ssmalllogo-transparent.png", basePath);

  SDL_IOStream *io = SDL_IOFromFile(logoPath, "rb");
  if (!io) return;
  Sint64 fileSize = SDL_GetIOSize(io);
  if (fileSize <= 0) { SDL_CloseIO(io); return; }
  unsigned char *buf = (unsigned char *)SDL_malloc((size_t)fileSize);
  if (!buf) { SDL_CloseIO(io); return; }
  SDL_ReadIO(io, buf, (size_t)fileSize);
  SDL_CloseIO(io);

  int imgW, imgH, channels;
  unsigned char *pixels = stbi_load_from_memory(buf, (int)fileSize, &imgW, &imgH, &channels, 4);
  SDL_free(buf);
  if (!pixels) return;

  SDL_Surface *surf = SDL_CreateSurfaceFrom(imgW, imgH, SDL_PIXELFORMAT_RGBA32, pixels, imgW * 4);
  if (!surf) { stbi_image_free(pixels); return; }
  SDL_Texture *logoTex = SDL_CreateTextureFromSurface(gRenderer, surf);
  SDL_DestroySurface(surf);
  stbi_image_free(pixels);
  if (!logoTex) return;

  /* Get the coordinate space the renderer is using */
  int screenW = 0, screenH = 0;
  {
    SDL_RendererLogicalPresentation mode;
    SDL_GetRenderLogicalPresentation(gRenderer, &screenW, &screenH, &mode);
  }
  if (screenW <= 0 || screenH <= 0)
    SDL_GetCurrentRenderOutputSize(gRenderer, &screenW, &screenH);

  /* Center the logo */
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  if (screenW > 0 && screenH > 0) {
    /* Scale logo to fit ~40% of the smaller screen dimension */
    int maxDim = (screenW < screenH) ? screenW : screenH;
    int drawSize = maxDim * 2 / 5;
    float scaleW = (float)drawSize / (float)imgW;
    float scaleH = (float)drawSize / (float)imgH;
    float scale = (scaleW < scaleH) ? scaleW : scaleH;
    float dstW = imgW * scale;
    float dstH = imgH * scale;
    SDL_FRect dst = {
      (screenW - dstW) / 2.0f,
      (screenH - dstH) / 2.0f,
      dstW, dstH
    };
    SDL_RenderTexture(gRenderer, logoTex, NULL, &dst);
  }

  SDL_RenderPresent(gRenderer);
  SDL_DestroyTexture(logoTex);
}

/* Map the active BCP-47 language code to the Sarasa Mono Slab region
 * that carries its CJK glyphs. Sarasa Mono Slab is a true monospace
 * (Iosevka Slab Latin merged with Source Han Sans) — typewriter
 * aesthetic plus correct half-width / full-width cells, so the columnar
 * status panels and the marquee align when mixing Latin and CJK. All
 * regional variants share the same Iosevka cell metrics, so the
 * SlabK hangul fallback chains cleanly onto a SlabJ / SlabSC / SlabTC
 * primary. The picker normalises codes to lowercase (lang.c
 * langPickerScan); compare case-insensitively to also accept codes set
 * by other paths. */
static const char *sarasaMonoFontPath(const char *langCode) {
  if (langCode && *langCode) {
    if (SDL_strcasecmp(langCode, "ko")    == 0) return "data/fonts/SarasaMonoSlabK-Regular.ttf";
    if (SDL_strcasecmp(langCode, "zh-CN") == 0) return "data/fonts/SarasaMonoSlabSC-Regular.ttf";
    if (SDL_strcasecmp(langCode, "zh-TW") == 0) return "data/fonts/SarasaMonoSlabTC-Regular.ttf";
  }
  return "data/fonts/SarasaMonoSlabJ-Regular.ttf";
}

/* Open the four in-game TTF font handles at sizes derived from gZoomFactor,
 * plus the SarasaMonoSlabK fallback chain (so hangul renders even when the
 * primary is SlabJ / SlabSC / SlabTC). Used by both sdl3DrawSetup and the
 * zoom-change path in sdl3DrawAdaptRenderTarget so a window resize keeps
 * the same font primary + fallback set. Sets all eight globals; assumes
 * the existing handles have already been closed by the caller (or are
 * NULL). */
static void openInGameFonts(void) {
  char langCode[32];
  langCode[0] = '\0';
  gameFrontGetLanguageCode(langCode, (int)sizeof(langCode));
  const char *sarasaRel  = sarasaMonoFontPath(langCode);
  const char *sarasaKRel = "data/fonts/SarasaMonoSlabK-Regular.ttf";

#if defined(__EMSCRIPTEN__)
  static char sarasaBuf[1024];
  SDL_snprintf(sarasaBuf, sizeof(sarasaBuf), "/%s", sarasaRel);
  const char *sarasaPath  = sarasaBuf;
  const char *sarasaKPath = "/data/fonts/SarasaMonoSlabK-Regular.ttf";
#elif defined(__ANDROID__)
  const char *sarasaPath  = sarasaRel;
  const char *sarasaKPath = sarasaKRel;
#else
  const char *base = SDL_GetBasePath();
  static char sarasaBuf[1024];
  static char sarasaKBuf[1024];
  if (base) {
    SDL_snprintf(sarasaBuf,  sizeof(sarasaBuf),  "%s%s", base, sarasaRel);
    SDL_snprintf(sarasaKBuf, sizeof(sarasaKBuf), "%s%s", base, sarasaKRel);
  } else {
    SDL_snprintf(sarasaBuf,  sizeof(sarasaBuf),  "%s", sarasaRel);
    SDL_snprintf(sarasaKBuf, sizeof(sarasaKBuf), "%s", sarasaKRel);
  }
  const char *sarasaPath  = sarasaBuf;
  const char *sarasaKPath = sarasaKBuf;
#endif

  gFontMsg   = TTF_OpenFont(sarasaPath, 13 * gZoomFactor);
  gFontKD    = TTF_OpenFont(sarasaPath, 13 * gZoomFactor);
  gFontTiny  = TTF_OpenFont(sarasaPath,  8 * gZoomFactor);
  gFontLabel = TTF_OpenFont(sarasaPath, 10 * gZoomFactor);
  if (!gFontMsg) {
    WB_LOG_ERROR(WB_LOG_CAT_ASSET, "openInGameFonts: failed to open primary %s — text will not render",
            sarasaPath);
  }

  /* Hangul fallback. Skip if primary already is SarasaMonoSlabK. */
  if (SDL_strcmp(sarasaPath, sarasaKPath) != 0) {
    gFallbackFontMsg   = TTF_OpenFont(sarasaKPath, 13 * gZoomFactor);
    gFallbackFontKD    = TTF_OpenFont(sarasaKPath, 13 * gZoomFactor);
    gFallbackFontTiny  = TTF_OpenFont(sarasaKPath,  8 * gZoomFactor);
    gFallbackFontLabel = TTF_OpenFont(sarasaKPath, 10 * gZoomFactor);
    if (gFontMsg   && gFallbackFontMsg)   TTF_AddFallbackFont(gFontMsg,   gFallbackFontMsg);
    if (gFontKD    && gFallbackFontKD)    TTF_AddFallbackFont(gFontKD,    gFallbackFontKD);
    if (gFontTiny  && gFallbackFontTiny)  TTF_AddFallbackFont(gFontTiny,  gFallbackFontTiny);
    if (gFontLabel && gFallbackFontLabel) TTF_AddFallbackFont(gFontLabel, gFallbackFontLabel);
    if (!gFallbackFontMsg) {
      WB_LOG_WARN(WB_LOG_CAT_ASSET, "openInGameFonts: failed to open SlabK fallback %s — hangul will tofu",
              sarasaKPath);
    }
  }

  /* Push the new font handles into sdl3draw_status's private copies
     (covers both the initial sdl3DrawSetup path and the
     sdl3DrawAdaptRenderTarget reload path). */
  sdl3DrawStatusSetFonts(gFontTiny, gFontMsg, gFontKD, gFontLabel,
                         gFallbackFontTiny, gFallbackFontMsg,
                         gFallbackFontKD, gFallbackFontLabel);
}

bool sdl3DrawSetup(int zoomFactor) {
  gZoomFactor = zoomFactor;
  sdl3DrawStatusSetZoom(gZoomFactor);

#ifdef __EMSCRIPTEN__
  /* Pre-size the canvas so SDL3's external_size probe sees the right
     dimensions (it temporarily sets the canvas to 1x1 and checks CSS). */
  {
    int cw = zoomFactor * SDL3_SCREEN_W;
    int ch = zoomFactor * SDL3_SCREEN_H;
    emscripten_set_canvas_element_size("#canvas", cw, ch);
    emscripten_set_element_css_size("#canvas", (double)cw, (double)ch);
  }
#endif

  /* --- Create window and renderer first, so we can compute the
         effective zoom for font sizing and tile loading. --- */
  if (uiModeIsSteamDeck()) {
    /* Steam Deck: fullscreen at native 1280x800, no DPI scaling.
       Uses desktop UI baseline so menu bar / dialogs render
       normally inside the fullscreen surface. */
    gWindow = SDL_CreateWindow("WinBolo", 1280, 800,
                               SDL_WINDOW_FULLSCREEN);
  } else if (uiModeIsTablet()) {
    /* Tablet mode: fullscreen window, no fixed-size chrome */
    gWindow = SDL_CreateWindow("WinBolo", 0, 0,
                               SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  } else {
    gWindow = SDL_CreateWindow("WinBolo",
                               zoomFactor * SDL3_SCREEN_W,
                               zoomFactor * SDL3_SCREEN_H + MENU_BAR_HEIGHT,
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
                               0);
#else
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
#endif
  }
  if (gWindow == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_GUI, "sdl3DrawSetup: SDL_CreateWindow failed: %s", SDL_GetError());
    return FALSE;
  }

  sdl3DrawSetWindowIcon(gWindow);

  /* Aspect ratio is enforced dynamically in sdl3imgui.cpp resize handler
     to account for the fixed 22px menu bar. */

  gRenderer = SDL_CreateRenderer(gWindow, NULL);
  if (gRenderer == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_GUI, "sdl3DrawSetup: SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_DestroyWindow(gWindow);
    gWindow = NULL;
    return FALSE;
  }

  SDL_SetRenderVSync(gRenderer, 1);

  /* Steam Deck: scale the desktop view via SDL logical presentation, the
     same path mobile uses — picks an integer zoom that fits the 1280x800
     screen, then SDL upscales the entire 515x325 desktop layout (chrome,
     status panels, playfield, text) in one consistent step.  Fonts open
     below at gZoomFactor so glyphs rasterize crisply rather than being
     LINEAR-upscaled from a 1x render. */
  if (uiModeIsSteamDeck()) {
    int ww, wh;
    SDL_GetCurrentRenderOutputSize(gRenderer, &ww, &wh);
    int zoomH = wh / SDL3_SCREEN_H;   /* 800 / 325 = 2 */
    int zoomW = ww / SDL3_SCREEN_W;   /* 1280 / 515 = 2 */
    int bestZoom = (zoomH < zoomW) ? zoomH : zoomW;
    if (bestZoom < 1) bestZoom = 1;
    SDL_SetRenderLogicalPresentation(gRenderer,
        SDL3_SCREEN_W * bestZoom, SDL3_SCREEN_H * bestZoom,
        SDL_LOGICAL_PRESENTATION_LETTERBOX);
    gZoomFactor = bestZoom;
    sdl3DrawStatusSetZoom(gZoomFactor);
    WB_LOG_INFO(WB_LOG_CAT_GUI,
        "sdl3DrawSetup: deck logical presentation %dx%d (zoom %d, render output %dx%d)",
        SDL3_SCREEN_W * bestZoom, SDL3_SCREEN_H * bestZoom, bestZoom, ww, wh);
  }

  /* macOS trackpad pinch-to-zoom */
  macOSPinchZoomInit();

#ifdef __ANDROID__
  if (!uiModeIsTablet()) {
    /* On Android with desktop mode: set a logical presentation so SDL3
       scales our fixed-size layout to fit the screen. */
    SDL_SetRenderLogicalPresentation(gRenderer,
                                     zoomFactor * SDL3_SCREEN_W,
                                     zoomFactor * SDL3_SCREEN_H,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
  }
#endif

#if defined(__IPHONEOS__) || defined(__ANDROID__)
  if (uiModeIsTablet()) {
    /* On mobile in tablet mode, the screen may be too small for integer
       zoom to fill the height well (e.g. 402pt → zoom 1 uses only 60%).
       Set a logical presentation so the next integer zoom fits exactly. */
    int ww, wh;
    SDL_GetCurrentRenderOutputSize(gRenderer, &ww, &wh);
    int gameUnit = MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y; /* 240 */
    int curZoom = wh / gameUnit;
    if (curZoom < 1) curZoom = 1;
    int used = gameUnit * curZoom;
    /* If more than 15% of screen height is wasted, bump to next zoom */
    int bestZoom = curZoom;
    if ((wh - used) * 100 / wh > 15) {
      bestZoom = curZoom + 1;
    }
    /* Always set a logical presentation so game rendering and ImGui
       share the same coordinate space (critical with HIGH_PIXEL_DENSITY
       where the render output is in native pixels). */
    int logH = gameUnit * bestZoom;
    int logW = ww * logH / wh;
    SDL_SetRenderLogicalPresentation(gRenderer, logW, logH,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    WB_LOG_INFO(WB_LOG_CAT_GUI, "sdl3DrawSetup: mobile tablet logical presentation %dx%d (zoom %d)",
            logW, logH, bestZoom);
    gZoomFactor = bestZoom;
    sdl3DrawStatusSetZoom(gZoomFactor);
    WB_LOG_INFO(WB_LOG_CAT_GUI, "sdl3DrawSetup: render output %dx%d, effective zoom %d", ww, wh, gZoomFactor);
  }
#endif

  /* --- Show loading screen while heavy resources load --- */
  sdl3DrawShowLoadingScreen();

  /* --- Now load fonts at the correct zoom --- */
  bool ttfOk = TTF_Init();
  if (ttfOk) {
    openInGameFonts();
  }

  /* Tank-label cache zeroing moved to sdl3draw_status's static
     initialisers; nothing to do here. */

  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */
  SDL_RenderPresent(gRenderer);

  /* Set up the SDL cursor (crosshair inside game area, system cursor outside) */
  cursorSetup();

  /* Pre-populate source-rect lookup tables at 1x (SDL3 scales dest rects) */
  mapViewInit();

  /* Load tiles now so status panels can draw during game init, before the
     first sdl3DrawMainScreen call. */
  sdl3LoadTiles();

  /* Glyphs module — Path A controller-icon cache.  Steam Input is
     already initialised in winbolo.c before sdl3DrawSetup runs, so
     glyphForAction() will resolve correctly once a controller binds.
     No-op (returns NULL) when Steam Input isn't active. */
  glyphsInit(gRenderer);

  /* Load custom crosshair (17×17 PNG, center pixel (8,8) = aim point). */
  {
    const char *basePath = SDL_GetBasePath();
    if (!basePath) basePath = "./";
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%sdata/crosshairs_17x17.png", basePath);
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (io) {
      Sint64 sz = SDL_GetIOSize(io);
      if (sz > 0) {
        unsigned char *buf = (unsigned char *)SDL_malloc((size_t)sz);
        if (buf) {
          SDL_ReadIO(io, buf, (size_t)sz);
          int imgW, imgH, ch;
          unsigned char *pix = stbi_load_from_memory(buf, (int)sz, &imgW, &imgH, &ch, 4);
          SDL_free(buf);
          if (pix) {
            SDL_Surface *surf = SDL_CreateSurfaceFrom(imgW, imgH, SDL_PIXELFORMAT_RGBA32, pix, imgW * 4);
            if (surf) {
              gCrosshairTex = SDL_CreateTextureFromSurface(gRenderer, surf);
              SDL_DestroySurface(surf);
              if (gCrosshairTex) {
                SDL_SetTextureBlendMode(gCrosshairTex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(gCrosshairTex, SDL_SCALEMODE_NEAREST);
              }
            }
            stbi_image_free(pix);
          }
        }
      }
      SDL_CloseIO(io);
    }
  }

  /* Create Phase 4 render-target textures.
     Man-status is created at zoom-factor resolution so the circle is drawn
     at actual screen pixels — no upscaling means no clipping or jaggedness.
     Add 2 pixels of padding so the arrow can reach the circle edge without
     being clipped.  Status icon panels (bases/pills/tanks) are drawn
     directly to the framebuffer so they need no render-target texture. */
  gManStatusTex   = sdl3CreateRenderTarget((MAN_STATUS_WIDTH  + 2) * gZoomFactor,
                                           (MAN_STATUS_HEIGHT + 2) * gZoomFactor);
  gTankBarsTex    = sdl3CreateRenderTarget(STATUS_TANK_BARS_TOTALWIDTH, STATUS_TANK_BARS_HEIGHT);
  gBaseBarsTex    = sdl3CreateRenderTarget(STATUS_BASE_BARS_MAX_WIDTH,  STATUS_BASE_BARS_TOTALHEIGHT);

  /* Phase C of plans/ctrailer.md — push renderer / atlas / fonts /
     zoom / bar textures into sdl3draw_status's private copies so the
     moved status renderers see live state. openInGameFonts and
     sdl3LoadTiles above already pushed fonts and atlas individually;
     Init covers the gRenderer and bar textures and re-asserts the rest. */
  sdl3DrawStatusInit(gRenderer, gTilesTex, gSheetScale, gZoomFactor,
                     gFontTiny, gFontMsg, gFontKD, gFontLabel,
                     gFallbackFontTiny, gFallbackFontMsg,
                     gFallbackFontKD, gFallbackFontLabel,
                     gTankBarsTex, gBaseBarsTex);

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(__IPHONEOS__)
  /* Desktop resizable: create a render target for the game content.
     The game is rendered at its logical size, then blitted scaled to the
     window below the menu bar. This allows the menu to stay at 1x size
     while the game scales.  Skipped on Deck — logical presentation
     already upscales the whole layout, an extra RT would re-introduce a
     1x rasterization step that defeats the font sharpness. */
  if (!uiModeIsTablet() && !uiModeIsSteamDeck()) {
    gGameRTWidth  = gZoomFactor * SDL3_SCREEN_W;
    gGameRTHeight = gZoomFactor * SDL3_SCREEN_H;
    gGameRenderTarget = SDL_CreateTexture(gRenderer,
                                          SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET,
                                          gGameRTWidth, gGameRTHeight);
    if (gGameRenderTarget) {
      SDL_SetTextureScaleMode(gGameRenderTarget, SDL_SCALEMODE_LINEAR);
      WB_LOG_INFO(WB_LOG_CAT_GUI, "sdl3DrawSetup: created game render target %dx%d", gGameRTWidth, gGameRTHeight);
    }
  }
#endif

  return TRUE;
}

void sdl3DrawCleanup(void) {
  tileLoaderCleanup();
  cursorCleanup();

  /* ImGui cleanup before destroying renderer/window */
  sdl3ImguiCleanup();

  /* Phase 5 — destroy text/label caches via the status module (which
     owns them since Phase C of plans/ctrailer.md), then close fonts
     and shut down TTF. */
  sdl3DrawStatusShutdown();
  if (gFontMsg)  { TTF_CloseFont(gFontMsg);  gFontMsg  = NULL; }
  if (gFontKD)   { TTF_CloseFont(gFontKD);   gFontKD   = NULL; }
  if (gFontTiny)  { TTF_CloseFont(gFontTiny);  gFontTiny  = NULL; }
  if (gFontLabel) { TTF_CloseFont(gFontLabel); gFontLabel = NULL; }
  if (gFallbackFontMsg)   { TTF_CloseFont(gFallbackFontMsg);   gFallbackFontMsg   = NULL; }
  if (gFallbackFontKD)    { TTF_CloseFont(gFallbackFontKD);    gFallbackFontKD    = NULL; }
  if (gFallbackFontTiny)  { TTF_CloseFont(gFallbackFontTiny);  gFallbackFontTiny  = NULL; }
  if (gFallbackFontLabel) { TTF_CloseFont(gFallbackFontLabel); gFallbackFontLabel = NULL; }
  TTF_Quit();

  /* Glyph cache textures must be destroyed before the renderer. */
  glyphsShutdown();

  if (gManStatusTex)     { SDL_DestroyTexture(gManStatusTex);     gManStatusTex     = NULL; }
  if (gTankBarsTex)      { SDL_DestroyTexture(gTankBarsTex);      gTankBarsTex      = NULL; }
  if (gBaseBarsTex)      { SDL_DestroyTexture(gBaseBarsTex);      gBaseBarsTex      = NULL; }
  if (gCrosshairTex)     { SDL_DestroyTexture(gCrosshairTex);     gCrosshairTex     = NULL; }
  if (gGameRenderTarget) { SDL_DestroyTexture(gGameRenderTarget); gGameRenderTarget = NULL; }
  if (gTilesTex) {
    SDL_DestroyTexture(gTilesTex);
    gTilesTex = NULL;
    gSheetScale = 1;
  }
  if (gBackgroundTex) {
    SDL_DestroyTexture(gBackgroundTex);
    gBackgroundTex = NULL;
  }
  if (gRenderer) {
    SDL_DestroyRenderer(gRenderer);
    gRenderer = NULL;
  }
  macOSPinchZoomDestroy();
  if (gWindow) {
    SDL_DestroyWindow(gWindow);
    gWindow = NULL;
  }
}

/* sdl3DrawShells, sdl3DrawTanks, sdl3DrawLGMs moved to mapview.c
   as mapViewDrawShells/Tanks/LGMs. */

/*--------------------------------------------------------
 * Draw tank labels only (separate pass after mapViewDrawTanks).
 *--------------------------------------------------------*/
static void sdl3DrawTankLabels(screenTanks *tks) {
  BYTE total = screenTanksGetNumEntries(tks);
  for (BYTE count = 1; count <= total; count++) {
    BYTE mx, my, px, py, frame, playerNum;
    char playerName[256];
    screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &playerNum, playerName);
    sdl3DrawTankLabel(playerName, playerNum, mx, my, px, py);
  }
}

/* -------------------------------------------------------
 * sdl3DrawAdaptRenderTarget — dynamically resize the game
 * render target when the window size changes in Custom zoom
 * mode.  Computes the ceiling integer zoom so the render
 * target is always >= the window size, then downscales the
 * blit for crisp output at any window size.
 * ------------------------------------------------------- */
static void sdl3DrawAdaptRenderTarget(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(__IPHONEOS__)
  extern BYTE zoomFactor;
  if (zoomFactor != ZOOM_FACTOR_CUSTOM) return;
  if (!gRenderer || !gWindow) return;
  if (uiModeIsTablet()) return;
  /* Deck is fullscreen 1280x800 with logical presentation already set in
     sdl3DrawSetup.  No resizes ever fire here; even if they did, this
     function would recompute gZoomFactor and stomp the value the Deck
     branch picked. */
  if (uiModeIsSteamDeck()) return;

  int winW, winH;
  SDL_GetCurrentRenderOutputSize(gRenderer, &winW, &winH);

  /* Ceiling integer zoom: smallest integer where zoom * gameSize >= windowSize */
  int zoomForW = (winW + SDL3_SCREEN_W - 1) / SDL3_SCREEN_W;
  int zoomForH = (winH + SDL3_SCREEN_H - 1) / SDL3_SCREEN_H;
  int needZoom = (zoomForW > zoomForH) ? zoomForW : zoomForH;
  if (needZoom < 1) needZoom = 1;

  /* Nothing to do if already at the right zoom */
  if (needZoom == gZoomFactor && gGameRenderTarget != NULL) return;

  WB_LOG_INFO(WB_LOG_CAT_GUI, "sdl3DrawAdaptRenderTarget: window %dx%d -> zoom %d (was %d)",
          winW, winH, needZoom, gZoomFactor);

  /* Destroy old resources that are zoom-dependent */
  if (gTilesTex) { SDL_DestroyTexture(gTilesTex); gTilesTex = NULL; gSheetScale = 1; }
  sdl3DrawStatusSetAtlas(NULL, 1);
  tileLoaderCleanup();
  if (gGameRenderTarget) { SDL_DestroyTexture(gGameRenderTarget); gGameRenderTarget = NULL; }
  if (gManStatusTex) { SDL_DestroyTexture(gManStatusTex); gManStatusTex = NULL; }

  /* Destroy font resources — sdl3draw_status owns the per-zoom label
     and message texture caches; have it free those before we close
     the fonts they were rendered against. */
  sdl3DrawStatusShutdown();
  if (gFontMsg)   { TTF_CloseFont(gFontMsg);   gFontMsg   = NULL; }
  if (gFontKD)    { TTF_CloseFont(gFontKD);    gFontKD    = NULL; }
  if (gFontTiny)  { TTF_CloseFont(gFontTiny);  gFontTiny  = NULL; }
  if (gFontLabel) { TTF_CloseFont(gFontLabel); gFontLabel = NULL; }
  if (gFallbackFontMsg)   { TTF_CloseFont(gFallbackFontMsg);   gFallbackFontMsg   = NULL; }
  if (gFallbackFontKD)    { TTF_CloseFont(gFallbackFontKD);    gFallbackFontKD    = NULL; }
  if (gFallbackFontTiny)  { TTF_CloseFont(gFallbackFontTiny);  gFallbackFontTiny  = NULL; }
  if (gFallbackFontLabel) { TTF_CloseFont(gFallbackFontLabel); gFallbackFontLabel = NULL; }

  /* Update zoom factor */
  gZoomFactor = needZoom;
  sdl3DrawStatusSetZoom(gZoomFactor);

  /* Reload fonts at new zoom (Sarasa Mono primary + SarasaMonoK fallback,
   * matching sdl3DrawSetup so chat / newswire keep CJK coverage after a
   * window resize). */
  openInGameFonts();
  /* Re-attach renderer / atlas / bar textures into the status module
     after Shutdown nulled them out. (gTilesTex is reloaded lazily by
     sdl3LoadTiles, which calls SetAtlas.) */
  sdl3DrawStatusInit(gRenderer, gTilesTex, gSheetScale, gZoomFactor,
                     gFontTiny, gFontMsg, gFontKD, gFontLabel,
                     gFallbackFontTiny, gFallbackFontMsg,
                     gFallbackFontKD, gFallbackFontLabel,
                     gTankBarsTex, gBaseBarsTex);

  /* Tiles will be reloaded lazily by sdl3LoadTiles() at new gZoomFactor */

  /* Recreate man-status render target at new zoom */
  gManStatusTex = sdl3CreateRenderTarget((MAN_STATUS_WIDTH  + 2) * gZoomFactor,
                                         (MAN_STATUS_HEIGHT + 2) * gZoomFactor);

  /* Recreate game render target at new zoom */
  gGameRTWidth  = gZoomFactor * SDL3_SCREEN_W;
  gGameRTHeight = gZoomFactor * SDL3_SCREEN_H;
  gGameRenderTarget = SDL_CreateTexture(gRenderer,
                                        SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET,
                                        gGameRTWidth, gGameRTHeight);
  if (gGameRenderTarget) {
    SDL_SetTextureScaleMode(gGameRenderTarget, SDL_SCALEMODE_LINEAR);
    WB_LOG_INFO(WB_LOG_CAT_GUI, "sdl3DrawAdaptRenderTarget: created render target %dx%d", gGameRTWidth, gGameRTHeight);
  }
#endif
}

void sdl3DrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                        screenGunsight *gs, screenBullets *sBullets, screenLgm *lgms,
                        RECT *rcWindow, bool showPillLabels, bool showBaseLabels,
                        int32_t srtDelay, bool isPillView, int edgeX, int edgeY,
                        bool useCursor, BYTE cursorLeft, BYTE cursorTop, tank *tank) {
  (void)rcWindow;

  if (gRenderer == NULL) {
    return;
  }

  sdl3DrawAdaptRenderTarget();

  bool tabletMode = uiModeIsTablet();
  bool useRenderTarget = !tabletMode && gGameRenderTarget != NULL;

  /* Desktop resizable: render game to off-screen texture, then blit scaled */
  if (useRenderTarget) {
    SDL_SetRenderTarget(gRenderer, gGameRenderTarget);
  }

  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  /* Effective zoom: in tablet mode, compute a scale that fits the 15x15
     tile game view to the screen, then centre it. */
  int effectiveZoom = gZoomFactor;
  int tabletOriginX = 0, tabletOriginY = 0;

  if (tabletMode) {
    int ww, wh;
    /* Use logical presentation size if set, otherwise render output size */
    {
      SDL_RendererLogicalPresentation logMode;
      SDL_GetRenderLogicalPresentation(gRenderer, &ww, &wh, &logMode);
      if (ww <= 0 || wh <= 0) {
        SDL_GetCurrentRenderOutputSize(gRenderer, &ww, &wh);
      }
    }
    /* Integer zoom that fits 15 tiles in both dimensions */
    int zoomX = ww / (MAIN_SCREEN_SIZE_X * TILE_SIZE_X);
    int zoomY = wh / (MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y);
    effectiveZoom = (zoomX < zoomY) ? zoomX : zoomY;
    if (effectiveZoom < 1) effectiveZoom = 1;
    /* Centre the game area */
    int gamePixW = MAIN_SCREEN_SIZE_X * TILE_SIZE_X * effectiveZoom;
    int gamePixH = MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y * effectiveZoom;
    tabletOriginX = (ww - gamePixW) / 2;
    tabletOriginY = (wh - gamePixH) / 2;
    /* Store for sdl3DrawGetTabletViewport() */
    gTabletVpX = tabletOriginX;
    gTabletVpY = tabletOriginY;
    gTabletVpW = gamePixW;
    gTabletVpH = gamePixH;
    gTabletVpZoom = effectiveZoom;
  } else if (!tabletMode) {
    gTabletVpX = gTabletVpY = gTabletVpW = gTabletVpH = gTabletVpZoom = 0;
  }

  if (!tabletMode && sdl3LoadBackground()) {
    SDL_FRect bgDest = { 0.0f, 0.0f,
                         (float)(gZoomFactor * SDL3_SCREEN_W),
                         (float)(gZoomFactor * SDL3_SCREEN_H) };
    SDL_RenderTexture(gRenderer, gBackgroundTex, NULL, &bgDest);
  } else if (tabletMode) {
    /* Tablet chrome: beveled gray background matching desktop background.bmp style.
       Draw via SDL so it appears behind the game tiles (which draw next).
       Panel borders in the gutters are drawn later by renderTabletBackground()
       in sdl3imgui_tablet.cpp via ImGui's BackgroundDrawList. */
    int scrW, scrH;
    {
      SDL_RendererLogicalPresentation logMode;
      SDL_GetRenderLogicalPresentation(gRenderer, &scrW, &scrH, &logMode);
      if (scrW <= 0 || scrH <= 0)
        SDL_GetCurrentRenderOutputSize(gRenderer, &scrW, &scrH);
    }
    float ps = (float)scrH / 480.0f;
    if (ps < 0.7f) ps = 0.7f;
    float border = 2.0f * ps;
    float vpPad = 3.0f * ps;

    /* Fill entire screen with chrome gray */
    SDL_SetRenderDrawColor(gRenderer, 107, 107, 107, 255);
    SDL_FRect fullScr = { 0, 0, (float)scrW, (float)scrH };
    SDL_RenderFillRect(gRenderer, &fullScr);

    /* Inset bevel around the game viewport */
    float gamePixW = (float)(MAIN_SCREEN_SIZE_X * TILE_SIZE_X * effectiveZoom);
    float gamePixH = (float)(MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y * effectiveZoom);
    float bx = (float)tabletOriginX - vpPad - border;
    float by = (float)tabletOriginY - vpPad - border;
    float bw = gamePixW + (vpPad + border) * 2;
    float bh = gamePixH + (vpPad + border) * 2;
    /* Dark edge on top and left */
    SDL_SetRenderDrawColor(gRenderer, 64, 64, 64, 255);
    SDL_FRect topE  = { bx, by, bw, border };
    SDL_FRect leftE = { bx, by + border, border, bh - border };
    SDL_RenderFillRect(gRenderer, &topE);
    SDL_RenderFillRect(gRenderer, &leftE);
    /* Light edge on bottom and right */
    SDL_SetRenderDrawColor(gRenderer, 160, 160, 160, 255);
    SDL_FRect botE   = { bx, by + bh - border, bw, border };
    SDL_FRect rightE = { bx + bw - border, by, border, bh - border };
    SDL_RenderFillRect(gRenderer, &botE);
    SDL_RenderFillRect(gRenderer, &rightE);
    /* Black fill inside bevel (game tiles draw over this) */
    SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
    SDL_FRect inner = { bx + border, by + border,
                        bw - border * 2, bh - border * 2 };
    SDL_RenderFillRect(gRenderer, &inner);
  }

  /* In tablet mode, temporarily override gZoomFactor so that sprite
     helper functions (sdl3DrawShells/Tanks/LGMs) and gunsight code
     that reference gZoomFactor directly pick up the tablet scale. */
  int savedZoomFactor = gZoomFactor;
  if (tabletMode) {
    gZoomFactor = effectiveZoom;
    sdl3DrawStatusSetZoom(gZoomFactor);
  }
  /* Apply scroll pixel offset for smooth sub-tile scrolling.
     Source is touch drag in tablet mode, or arrow-key smooth scroll
     on desktop.  Value is 0 when neither is active. */
  edgeX += gDragOffsetX;
  edgeY += gDragOffsetY;

  if (sdl3LoadTiles()) {
    int tileW  = TILE_SIZE_X * gZoomFactor;
    int tileH  = TILE_SIZE_Y * gZoomFactor;
    int originX, originY, gameW, gameH;

    if (tabletMode) {
      originX = tabletOriginX;
      originY = tabletOriginY;
    } else {
      originX = MAIN_OFFSET_X * gZoomFactor;
      originY = MAIN_OFFSET_Y * gZoomFactor;
    }
    gameW = MAIN_SCREEN_SIZE_X * tileW;
    gameH = MAIN_SCREEN_SIZE_Y * tileH;

    SDL_Rect gameClip = {
      originX, originY,
      gameW, gameH
    };
    SDL_SetRenderClipRect(gRenderer, &gameClip);

    if (srtDelay > 0) {
      /* Black out the game area during start delay */
      SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
      SDL_FRect blackRect = { (float)originX, (float)originY,
                               (float)gameW, (float)gameH };
      SDL_RenderFillRect(gRenderer, &blackRect);

      /* Render "Game Starts in X" countdown text centred in the black area */
      if (gFontMsg) {
        char str[64];
        long secs = srtDelay / GAME_NUMGAMETICKS_SEC;
        snprintf(str, sizeof(str), "Game Starts in %ld", secs);
        SDL_Color white = {200, 200, 200, 255};
        SDL_Surface *sMeasure = TTF_RenderText_Blended(gFontMsg, str, 0, white);
        if (sMeasure) {
          float tx = (float)originX + ((float)gameW - (float)sMeasure->w) * 0.5f;
          float ty = (float)originY + ((float)gameH - (float)sMeasure->h) * 0.5f;
          SDL_DestroySurface(sMeasure);
          sdl3RenderText(gFontMsg, str, white, tx, ty);
        }
      }
    } else if (!isPillView && tankGetDeathWait(tank) != 0 &&
               ((tankGetLastTankDeath(tank) == LAST_DEATH_BY_DEEPSEA && tankGetDeathWait(tank) < STATIC_ON_TICKS_DEEPSEA) ||
                (tankGetLastTankDeath(tank) == LAST_DEATH_BY_SHELL   && tankGetDeathWait(tank) < STATIC_ON_TICKS_SHELL) ||
                (tankGetLastTankDeath(tank) == LAST_DEATH_BY_MINES   && tankGetDeathWait(tank) < STATIC_ON_TICKS_MINES))) {
      /* Tank died and is waiting to respawn — draw Bolo-style pixel static noise */
      /* On iOS use half-res texture so static dots appear larger */
#if defined(__IPHONEOS__)
      int staticW = gameW / 2;
      int staticH = gameH / 2;
#else
      int staticW = gameW;
      int staticH = gameH;
#endif
      /* Recreate static texture if size changed or doesn't exist yet */
      if (!gStaticTex || gStaticTexW != gameW || gStaticTexH != gameH) {
        if (gStaticTex) SDL_DestroyTexture(gStaticTex);
        gStaticTex = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, staticW, staticH);
        SDL_SetTextureScaleMode(gStaticTex, SDL_SCALEMODE_NEAREST);
        gStaticTexW = gameW;
        gStaticTexH = gameH;
        gStaticLast = 0;
      }
      /* Add new static points when the death tick changes */
      if (tankGetDeathWait(tank) != gStaticLast) {
        gStaticLast = tankGetDeathWait(tank);
        uint32_t *pixels;
        int pitch;
        if (SDL_LockTexture(gStaticTex, NULL, (void **)&pixels, &pitch)) {
          int rowLen = pitch / 4;
          /* First tick: clear to black */
          if (gStaticLast == 1 || gStaticSeed == 1) {
            for (int y = 0; y < staticH; y++)
              for (int x = 0; x < staticW; x++)
                pixels[y * rowLen + x] = 0xFF000000;
          }
          int numPoints = staticW * staticH / 3;
          int col = 0;
          uint32_t white = 0xFFFFFFFF;
          uint32_t black = 0xFF000000;
          for (int i = 0; i < numPoints; i++) {
            uint32_t rx = getRandomStaticNoiseSeed();
            uint32_t ry = getRandomStaticNoiseSeed();
            int px = rx % staticW;
            int py = ry % staticH;
            pixels[py * rowLen + px] = (col++ & 1) ? white : black;
          }
          SDL_UnlockTexture(gStaticTex);
        }
      }
      /* Blit the accumulated static texture every frame */
      SDL_FRect staticDest = { (float)originX, (float)originY, (float)gameW, (float)gameH };
      SDL_RenderTexture(gRenderer, gStaticTex, NULL, &staticDest);
    } else {
      /* Draw map tiles via mapview */
      MapViewCtx mvCtx = { gRenderer, gTilesTex, gZoomFactor, gSheetScale };
      mapViewDrawTiles(&mvCtx, value, mineView, originX, originY, tileW, tileH, edgeX, edgeY);

      /* Draw pillbox/base number labels (needs fonts — stays here) */
      if (gFontLabel) {
        BYTE lx = 0, ly = 0;
        bool lDone = FALSE;
        while (!lDone) {
          BYTE pos = screenGetPos(value, lx, ly);
          bool isPill = (pos == PILL_EVIL_15 || (pos >= PILL_EVIL_14 && pos <= PILL_EVIL_0) ||
                         (pos >= PILL_GOOD_15 && pos <= PILL_GOOD_0));
          bool isBase = (pos == BASE_GOOD || pos == BASE_NEUTRAL || pos == BASE_EVIL);
          int labelNum = -1;
          if (isPill && showPillLabels) {
            labelNum = screenPillNumPosCS(cs, lx, ly) - 1;
          } else if (isBase && showBaseLabels) {
            labelNum = screenBaseNumPosCS(cs, lx, ly) - 1;
          }
          if (labelNum >= 0) {
            SDL_FRect dest = {
              (float)(originX + ((int)lx - 1) * tileW - edgeX),
              (float)(originY + ((int)ly - 1) * tileH - edgeY),
              (float)tileW,
              (float)tileH
            };
            char str[4];
            sprintf(str, "%d", labelNum);
            SDL_Color white = {200, 200, 200, 255};
            TTF_Font *labelFont = isBase ? gFontTiny : gFontLabel;
            SDL_Surface *surf = TTF_RenderText_Blended(labelFont, str, 0, white);
            if (surf) {
              SDL_Texture *tex = SDL_CreateTextureFromSurface(gRenderer, surf);
              if (tex) {
                float tw = (float)surf->w;
                float th = (float)surf->h;
                float tx, ty;
                if (isBase) {
                  tx = dest.x;
                  ty = dest.y;
                } else {
                  tx = dest.x + (dest.w - tw) * 0.5f;
                  ty = dest.y + (dest.h - th) * 0.5f;
                }
                SDL_FRect bgRect = { tx - 1.0f, ty - 1.0f, tw + 2.0f, th + 2.0f };
                SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
                SDL_RenderFillRect(gRenderer, &bgRect);
                SDL_FRect d = { tx, ty, tw, th };
                SDL_RenderTexture(gRenderer, tex, NULL, &d);
                SDL_DestroyTexture(tex);
              }
              SDL_DestroySurface(surf);
            }
          }
          lx++;
          if (lx == MAIN_BACK_BUFFER_SIZE_X) {
            lx = 0; ly++;
            if (ly == MAIN_BACK_BUFFER_SIZE_Y) lDone = TRUE;
          }
        }
      }

      /* Gunsight overlay — custom 17×17 crosshair, center pixel (8,8) = aim point.
       * Top-left is at the same position as the old 16×16 tile sprite so the
       * center aligns with the gunsight world position. */
      if (gs->mapX != NO_GUNSIGHT && gCrosshairTex) {
        int gsGameX = gs->mapX * TILE_SIZE_X + (int)gs->pixelX;
        int gsGameY = gs->mapY * TILE_SIZE_Y + (int)gs->pixelY;
        SDL_FRect gsDest = {
          (float)(originX + (gsGameX - TILE_SIZE_X) * gZoomFactor - edgeX),
          (float)(originY + (gsGameY - TILE_SIZE_Y) * gZoomFactor - edgeY),
          17.0f * (float)gZoomFactor, 17.0f * (float)gZoomFactor
        };
        SDL_RenderTexture(gRenderer, gCrosshairTex, NULL, &gsDest);
      }

      /* Build-mode cursor overlay */
      if (useCursor) {
        SDL_FRect curSrc = { (float)(MOUSE_SQUARE_X * gSheetScale), (float)(MOUSE_SQUARE_Y * gSheetScale),
                             (float)(TILE_SIZE_X * gSheetScale), (float)(TILE_SIZE_Y * gSheetScale) };
        SDL_FRect curDest = {
          (float)(originX + ((int)cursorLeft - 1) * tileW - edgeX),
          (float)(originY + ((int)cursorTop  - 1) * tileH - edgeY),
          (float)tileW, (float)tileH
        };
        SDL_RenderTexture(gRenderer, gTilesTex, &curSrc, &curDest);
      }


      /* Sprites via mapview */
      gCurrentEdgeX = edgeX;
      gCurrentEdgeY = edgeY;
      sdl3DrawStatusSetEdgeOffset(edgeX, edgeY);
      mapViewDrawShells(&mvCtx, sBullets, originX, originY, tileW, tileH, edgeX, edgeY);
      mapViewDrawTanks(&mvCtx, tks, originX, originY, tileW, tileH, edgeX, edgeY);
      /* Tank labels (needs fonts — separate pass after tank sprites) */
      sdl3DrawTankLabels(tks);
      mapViewDrawLGMs(&mvCtx, lgms, originX, originY, tileW, tileH, edgeX, edgeY);

      /* Phase 5 overlays (inside clip rect so they stay within the game area) */
      if (isPillView) {
        sdl3DrawPillInView();
      }
      if (gNetFailed) {
        sdl3DrawNetFailed();
      }
    }

    SDL_SetRenderClipRect(gRenderer, NULL);
  }

  /* Refresh status panel textures every frame so bases/pills/tanks
     stay current without depending on sdl3DrawRedrawAll firing on WM_PAINT. */
  if (tabletMode) {
    /* In tablet mode, position status grids in the left gutter using the
       same SDL rendering as desktop (BLENDMODE_NONE for correct icons).
       Only show if the gutter is wide enough (>= 100px). */
    int leftGutter = tabletOriginX;
    if (leftGutter >= 100) {
      int zf = gZoomFactor; /* currently set to effectiveZoom */
      float safeL = 0, safeT = 0;
      sdl3GetSafeAreaInsets(&safeL, &safeT, NULL, NULL);
      float gutterX = safeL + 4.0f;
      float gutterY = safeT + 4.0f;
      float gridH = (float)(zf * STATUS_TANKS_HEIGHT);
      float gridGap = 4.0f;
      sdl3DrawSetStatusPanelOrigins(
        gutterX, gutterY,                         /* tanks */
        gutterX, gutterY + gridH + gridGap,        /* pills */
        gutterX, gutterY + 2*(gridH + gridGap));   /* bases */
      sdl3DrawTabletStatusGrids(cs);
      sdl3DrawSetStatusPanelOrigins(-1,-1,-1,-1,-1,-1); /* reset */
    }
  } else {
    sdl3DrawSetBasesStatusClear();
    {
      BYTE total = basesGetNumBases(&cs->sim.bs);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusBase(i, screenBaseAllianceCS(cs, i), showBaseLabels);
      }
    }
    sdl3DrawSetPillsStatusClear();
    {
      BYTE total = pillsGetNumPills(&cs->sim.pb);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusPillbox(i, screenPillAllianceCS(cs, i), showPillLabels);
      }
    }
    sdl3DrawSetTanksStatusClear();
    for (BYTE i = 1; i <= MAX_TANKS; i++) {
      sdl3DrawStatusTank(i, screenTankAllianceCS(cs, i));
    }

    sdl3RenderStatusPanels();
    sdl3RenderCachedText();
  }

  /* Frame rate counting */
  g_dwFrameCount++;
  {
    DWORD now = (DWORD)SDL_GetTicks();
    DWORD elapsed = now - g_dwFrameTime;
    if (elapsed > 1000) {
      g_dwFrameTotal = g_dwFrameCount;
      g_dwFrameTime = now;
      g_dwFrameCount = 0;
    }
  }

  /* Restore original zoom factor after tablet-mode override */
  gZoomFactor = savedZoomFactor;
  sdl3DrawStatusSetZoom(gZoomFactor);

  /* Desktop resizable: blit game texture to window, scaled below menu bar */
  if (useRenderTarget) {
    /* Switch back to window */
    SDL_SetRenderTarget(gRenderer, NULL);

    /* Get window size */
    int winW, winH;
    SDL_GetCurrentRenderOutputSize(gRenderer, &winW, &winH);

    /* Menu bar height — ImGui default is ~20 pixels, but we'll query it later.
       For now, use a reasonable estimate.  Zero when the menu bar is hidden
       (controller mode) so the game render fills the freed top strip
       instead of leaving a 22px band. */
    float menuBarHeight = uiShouldUseControllerMode() ? 0.0f : 22.0f;

    /* Available area below menu */
    float availW = (float)winW;
    float availH = (float)winH - menuBarHeight;

    /* Fill available area — aspect ratio is enforced by WM_SIZING on Windows.
       On other platforms or if aspect differs, use centering as fallback. */
    float gameAspect = (float)gGameRTWidth / (float)gGameRTHeight;
    float availAspect = availW / availH;
    float aspectDiff = gameAspect - availAspect;
    if (aspectDiff < 0) aspectDiff = -aspectDiff;

    float destW, destH, destX, destY;
    if (aspectDiff < 0.01f) {
      /* Aspect ratios match (within tolerance) — fill entire area */
      destX = 0;
      destY = menuBarHeight;
      destW = availW;
      destH = availH;
    } else if (gameAspect > availAspect) {
      /* Game is wider — fit to width, letterbox top/bottom */
      destW = availW;
      destH = availW / gameAspect;
      destX = 0;
      destY = menuBarHeight + (availH - destH) / 2.0f;
    } else {
      /* Game is taller — fit to height, pillarbox left/right */
      destH = availH;
      destW = availH * gameAspect;
      destX = (availW - destW) / 2.0f;
      destY = menuBarHeight;
    }

    /* Store for coordinate transformation */
    gGameDestRect.x = destX;
    gGameDestRect.y = destY;
    gGameDestRect.w = destW;
    gGameDestRect.h = destH;
    gGameScale = destW / (float)gGameRTWidth;

    /* Clear window and blit game texture.
       Use BLENDMODE_NONE so that any alpha < 255 stored in the render target
       (e.g. from anti-aliased tile sprites drawn with BLENDMODE_NONE) doesn't
       cause semi-transparency when composited onto the window.
       Black letterbox/pillarbox fill — visible when the window aspect ratio
       differs from the game (e.g. fullscreen on a widescreen monitor). */
    SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
    SDL_RenderFillRect(gRenderer, NULL);
    SDL_SetTextureBlendMode(gGameRenderTarget, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(gRenderer, gGameRenderTarget, NULL, &gGameDestRect);
  }
}

void sdl3DrawRedrawAll(ClientSim *cs, buildSelect value, RECT *rcWindow,
                       bool showPillsStatus, bool showBasesStatus) {
  (void)rcWindow;
  if (gRenderer == NULL) return;

  sdl3DrawAdaptRenderTarget();

  bool tabletMode = uiModeIsTablet();
  bool useRenderTarget = !tabletMode && gGameRenderTarget != NULL;

  /* Desktop resizable: render to off-screen texture */
  if (useRenderTarget) {
    SDL_SetRenderTarget(gRenderer, gGameRenderTarget);
  }

  sdl3DrawSelectIndentsOn(value, 0, 0);

  /* Clear and draw background first so that status draws go on top */
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  if (!tabletMode && sdl3LoadBackground()) {
    SDL_FRect dest = {
      0.0f, 0.0f,
      (float)(gZoomFactor * SDL3_SCREEN_W),
      (float)(gZoomFactor * SDL3_SCREEN_H)
    };
    SDL_RenderTexture(gRenderer, gBackgroundTex, NULL, &dest);
  }

  if (tabletMode) {
    /* Tablet: render status grids in gutter if space allows */
    if (gTabletVpX >= 100) {
      int zf = gZoomFactor;
      float safeL = 0, safeT = 0;
      sdl3GetSafeAreaInsets(&safeL, &safeT, NULL, NULL);
      float gutterX = safeL + 4.0f;
      float gutterY = safeT + 4.0f;
      float gridH = (float)(zf * STATUS_TANKS_HEIGHT);
      float gridGap = 4.0f;
      sdl3DrawSetStatusPanelOrigins(gutterX, gutterY,
                                    gutterX, gutterY + gridH + gridGap,
                                    gutterX, gutterY + 2*(gridH + gridGap));
      sdl3DrawTabletStatusGrids(cs);
      sdl3DrawSetStatusPanelOrigins(-1,-1,-1,-1,-1,-1);
    }
  } else {
    sdl3DrawSetBasesStatusClear();
    {
      BYTE total = basesGetNumBases(&cs->sim.bs);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusBase(i, screenBaseAllianceCS(cs, i), showBasesStatus);
      }
    }
    sdl3DrawSetPillsStatusClear();
    {
      BYTE total = pillsGetNumPills(&cs->sim.pb);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusPillbox(i, screenPillAllianceCS(cs, i), showPillsStatus);
      }
    }
    sdl3DrawSetTanksStatusClear();
    for (BYTE i = 1; i <= MAX_TANKS; i++) {
      sdl3DrawStatusTank(i, screenTankAllianceCS(cs, i));
    }

    sdl3RenderStatusPanels();
    sdl3RenderCachedText();
  }

  /* Desktop resizable: blit game texture to window, scaled below menu bar */
  if (useRenderTarget) {
    SDL_SetRenderTarget(gRenderer, NULL);

    int winW, winH;
    SDL_GetCurrentRenderOutputSize(gRenderer, &winW, &winH);

    /* Zero when the menu bar is hidden (controller mode) — see matching
       block above. */
    float menuBarHeight = uiShouldUseControllerMode() ? 0.0f : 22.0f;
    float availW = (float)winW;
    float availH = (float)winH - menuBarHeight;

    /* Fill available area — aspect ratio is enforced by WM_SIZING on Windows */
    float gameAspect = (float)gGameRTWidth / (float)gGameRTHeight;
    float availAspect = availW / availH;
    float aspectDiff = gameAspect - availAspect;
    if (aspectDiff < 0) aspectDiff = -aspectDiff;

    float destW, destH, destX, destY;
    if (aspectDiff < 0.01f) {
      /* Aspect ratios match — fill entire area */
      destX = 0;
      destY = menuBarHeight;
      destW = availW;
      destH = availH;
    } else if (gameAspect > availAspect) {
      destW = availW;
      destH = availW / gameAspect;
      destX = 0;
      destY = menuBarHeight + (availH - destH) / 2.0f;
    } else {
      destH = availH;
      destW = availH * gameAspect;
      destX = (availW - destW) / 2.0f;
      destY = menuBarHeight;
    }

    gGameDestRect.x = destX;
    gGameDestRect.y = destY;
    gGameDestRect.w = destW;
    gGameDestRect.h = destH;
    gGameScale = destW / (float)gGameRTWidth;

    /* Black letterbox/pillarbox fill — visible when window aspect differs
       from the game (e.g. fullscreen on a widescreen monitor). */
    SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
    SDL_RenderFillRect(gRenderer, NULL);
    SDL_SetTextureBlendMode(gGameRenderTarget, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(gRenderer, gGameRenderTarget, NULL, &gGameDestRect);
  }
}

void sdl3DrawDownloadScreen(ClientSim *cs, RECT *rcWindow, bool justBlack) {
  (void)rcWindow;
  if (!gRenderer) return;

  int zf = gZoomFactor;
  bool tabletMode = uiModeIsTablet();

  /* Draw the full UI chrome (background, status icons, panels, newswire)
   * exactly as sdl3DrawRedrawAll does, so everything except the playfield
   * area remains visible during map download. */
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  if (!tabletMode && sdl3LoadBackground()) {
    SDL_FRect dest = {
      0.0f, 0.0f,
      (float)(zf * SDL3_SCREEN_W),
      (float)(zf * SDL3_SCREEN_H)
    };
    SDL_RenderTexture(gRenderer, gBackgroundTex, NULL, &dest);
  }

  if (tabletMode) {
    if (gTabletVpX >= 100) {
      int gridZf = gZoomFactor;
      float safeL = 0, safeT = 0;
      sdl3GetSafeAreaInsets(&safeL, &safeT, NULL, NULL);
      float gutterX = safeL + 4.0f;
      float gutterY = safeT + 4.0f;
      float gridH = (float)(gridZf * STATUS_TANKS_HEIGHT);
      float gridGap = 4.0f;
      sdl3DrawSetStatusPanelOrigins(gutterX, gutterY,
                                    gutterX, gutterY + gridH + gridGap,
                                    gutterX, gutterY + 2*(gridH + gridGap));
      sdl3DrawTabletStatusGrids(cs);
      sdl3DrawSetStatusPanelOrigins(-1,-1,-1,-1,-1,-1);
    }
  } else {
    sdl3DrawSetBasesStatusClear();
    {
      BYTE total = basesGetNumBases(&cs->sim.bs);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusBase(i, screenBaseAllianceCS(cs, i), FALSE);
      }
    }
    sdl3DrawSetPillsStatusClear();
    {
      BYTE total = pillsGetNumPills(&cs->sim.pb);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusPillbox(i, screenPillAllianceCS(cs, i), FALSE);
      }
    }
    sdl3DrawSetTanksStatusClear();
    for (BYTE i = 1; i <= MAX_TANKS; i++) {
      sdl3DrawStatusTank(i, screenTankAllianceCS(cs, i));
    }
    sdl3RenderStatusPanels();
    sdl3RenderCachedText();
  }

  /* Fill the playfield area black, then draw white progress bar from the top. */
  SDL_FRect playfield;
  if (tabletMode) {
    int ww, wh;
    {
      SDL_RendererLogicalPresentation logMode;
      SDL_GetRenderLogicalPresentation(gRenderer, &ww, &wh, &logMode);
      if (ww <= 0 || wh <= 0) {
        SDL_GetCurrentRenderOutputSize(gRenderer, &ww, &wh);
      }
    }
    int tzX = ww / (MAIN_SCREEN_SIZE_X * TILE_SIZE_X);
    int tzY = wh / (MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y);
    int tz = (tzX < tzY) ? tzX : tzY;
    if (tz < 1) tz = 1;
    int gpW = MAIN_SCREEN_SIZE_X * TILE_SIZE_X * tz;
    int gpH = MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y * tz;
    playfield = (SDL_FRect){
      (float)((ww - gpW) / 2), (float)((wh - gpH) / 2),
      (float)gpW, (float)gpH
    };
  } else {
    playfield = (SDL_FRect){
      (float)(zf * MAIN_OFFSET_X),
      (float)(zf * MAIN_OFFSET_Y),
      (float)(zf * MAIN_SCREEN_SIZE_X * TILE_SIZE_X),
      (float)(zf * MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y)
    };
  }
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, &playfield);

  if (!justBlack) {
    /* Map netGetDownloadPos() (0–255) to playfield height.
     * The back-buffer is (MAIN_SCREEN_SIZE_Y+2)*TILE_SIZE_Y = 272 pixels;
     * the playfield occupies rows TILE_SIZE_Y..TILE_SIZE_Y+MAIN_SCREEN_SIZE_Y*TILE_SIZE_Y.
     * The download position is a raw pixel count from the top of the back buffer,
     * so subtract the TILE_SIZE_Y row offset and clamp. */
    int downloadRaw = (int)netGetDownloadPos();
    int visibleY = downloadRaw - TILE_SIZE_Y;
    if (visibleY < 0) visibleY = 0;
    int playfieldH = MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y;
    if (visibleY > playfieldH) visibleY = playfieldH;

    SDL_FRect bar = {
      (float)(zf * MAIN_OFFSET_X),
      (float)(zf * MAIN_OFFSET_Y),
      (float)(zf * MAIN_SCREEN_SIZE_X * TILE_SIZE_X),
      (float)(zf * visibleY)
    };
    SDL_SetRenderDrawColor(gRenderer, 255, 255, 255, 255);
    SDL_RenderFillRect(gRenderer, &bar);
  }
}

void sdl3DrawMainScreenBlack(RECT *rcWindow) {
  (void)rcWindow;
  if (gRenderer == NULL) {
    return;
  }
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */
}

int drawGetFrameRate(void) {
  return (int)g_dwFrameTotal;
}

/* -------------------------------------------------------
 * Phase 4 — Status panel implementations
 * ------------------------------------------------------- */

/* Status-panel renderers (sdl3DrawSetBasesStatusClear / sdl3DrawStatusBase
   / sdl3DrawCopyBasesStatus and the pillbox / tank / *bars equivalents)
   moved to sdl3draw_status.c.  statusPanelOrigin moved with them. */

/* sdl3DrawGetCachedTankStats / sdl3DrawGetCachedBaseStats stay here as
   thin delegates to the implementations in sdl3draw_status.c, which
   now own the cached bar-stats statics that used to live next to the
   StatusTankBars / StatusBaseBars writers. */
void sdl3DrawGetCachedTankStats(BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees) {
  sdl3DrawStatusGetCachedTankStats(shells, mines, armour, trees);
}

void sdl3DrawGetCachedBaseStats(BYTE *shells, BYTE *mines, BYTE *armour, bool *hasBase) {
  sdl3DrawStatusGetCachedBaseStats(shells, mines, armour, hasBase);
}

void sdl3DrawSetManClear(void) {
  gManStatusReady = false;
  if (!gRenderer || !gManStatusTex) return;
  SDL_SetRenderTarget(gRenderer, gManStatusTex);
  SDL_SetTextureBlendMode(gManStatusTex, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */
  SDL_SetRenderTarget(gRenderer, NULL);
}

void sdl3DrawSetManStatus(int x, int y, bool isDead, TURNTYPE angle) {
  (void)x; (void)y;
  gManStatusDead  = isDead;
  gManStatusAngle = angle;
  if (!gRenderer || !gManStatusTex) return;

  /* Compute endpoint of direction arrow (same math as Win32 draw.c) */
  double dbAngle, dbTemp;
  int addX, addY;
  int cx = MAN_STATUS_CENTER_X;
  int cy = MAN_STATUS_CENTER_Y;

  TURNTYPE a = angle + BRADIANS_SOUTH;
  if (a >= BRADIANS_MAX) a -= BRADIANS_MAX;

  if (a >= BRADIANS_NORTH && a < BRADIANS_EAST) {
    dbAngle = (DEGREES_MAX / BRADIANS_MAX) * a;
    dbAngle = (dbAngle / DEGREES_MAX) * RADIANS_MAX;
    addX = cx; addY = cy;
    dbTemp = (MAN_STATUS_RADIUS-1) * sin(dbAngle); addX += (int)dbTemp;
    dbTemp = (MAN_STATUS_RADIUS-1) * cos(dbAngle); addY -= (int)dbTemp;
  } else if (a >= BRADIANS_EAST && a < BRADIANS_SOUTH) {
    a = (float)BRADIANS_SOUTH - a;
    dbAngle = (DEGREES_MAX / BRADIANS_MAX) * a;
    dbAngle = (dbAngle / DEGREES_MAX) * RADIANS_MAX;
    addX = cx; addY = cy;
    dbTemp = (MAN_STATUS_RADIUS-1) * sin(dbAngle); addX += (int)dbTemp;
    dbTemp = (MAN_STATUS_RADIUS-1) * cos(dbAngle); addY += (int)dbTemp;
  } else if (a >= BRADIANS_SOUTH && a < BRADIANS_WEST) {
    a = (float)BRADIANS_WEST - a;
    a = (float)BRADIANS_EAST - a;
    dbAngle = (DEGREES_MAX / BRADIANS_MAX) * a;
    dbAngle = (dbAngle / DEGREES_MAX) * RADIANS_MAX;
    addX = cx; addY = cy;
    dbTemp = (MAN_STATUS_RADIUS-1) * sin(dbAngle); addX -= (int)dbTemp;
    dbTemp = (MAN_STATUS_RADIUS-1) * cos(dbAngle); addY += (int)dbTemp;
  } else {
    a = (float)BRADIANS_MAX - a;
    dbAngle = (DEGREES_MAX / BRADIANS_MAX) * a;
    dbAngle = (dbAngle / DEGREES_MAX) * RADIANS_MAX;
    addX = cx; addY = cy;
    dbTemp = (MAN_STATUS_RADIUS-1) * sin(dbAngle); addX -= (int)dbTemp;
    dbTemp = (MAN_STATUS_RADIUS-1) * cos(dbAngle); addY -= (int)dbTemp;
  }

  /* Texture is (MAN_STATUS_WIDTH+2)*(MAN_STATUS_HEIGHT+2)*zf — 1px padding on
     each edge so the circle outline never clips. Shift center by +1 to match. */
  int zf = gZoomFactor;
  int scx = (MAN_STATUS_CENTER_X + 1) * zf;
  int scy = (MAN_STATUS_CENTER_Y + 1) * zf;
  int r   = (MAN_STATUS_RADIUS - 1) * zf;

  /* Re-compute arrow endpoint in zoom-factor space */
  int sAddX = addX * zf;
  int sAddY = addY * zf;

  SDL_SetRenderTarget(gRenderer, gManStatusTex);
  SDL_SetTextureBlendMode(gManStatusTex, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  if (isDead) {
    /* Filled circle in red/orange using scan lines */
    SDL_SetRenderDrawColor(gRenderer, 200, 80, 0, 255);
    for (int dy = -r; dy <= r; dy++) {
      int dx = (int)sqrtf((float)(r * r - dy * dy));
      SDL_RenderLine(gRenderer,
                     (float)(scx - dx), (float)(scy + dy),
                     (float)(scx + dx), (float)(scy + dy));
    }
  } else {
    /* Outline circle using parametric line segments */
    SDL_SetRenderDrawColor(gRenderer, 255, 255, 255, 255);
    int steps = 4 * r * 4;  /* ~4 steps per pixel of circumference */
    if (steps < 64) steps = 64;
    for (int i = 0; i < steps; i++) {
      double a1 = (RADIANS_MAX * i) / steps;
      double a2 = (RADIANS_MAX * (i + 1)) / steps;
      SDL_RenderLine(gRenderer,
                     (float)(scx + r * cos(a1)), (float)(scy + r * sin(a1)),
                     (float)(scx + r * cos(a2)), (float)(scy + r * sin(a2)));
    }
    /* Arrow from centre to endpoint */
    SDL_RenderLine(gRenderer, (float)scx, (float)scy, (float)sAddX, (float)sAddY);
  }

  SDL_SetRenderTarget(gRenderer, NULL);
  gManStatusReady = true;
}

void sdl3DrawCopyManStatus(int x, int y) {
  (void)x; (void)y;
  /* flushed via sdl3RenderStatusPanels each frame */
}

void sdl3DrawSelectIndentsOn(buildSelect value, int x, int y) {
  (void)x; (void)y;
  gCurrentBuildSelect = value;
}

void sdl3DrawSelectIndentsOff(buildSelect value, int x, int y) {
  (void)value; (void)x; (void)y;
  /* No-op: background.bmp already shows all buttons in off state */
}

void sdl3DrawStartDelay(RECT *rcWindow, int32_t srtDelay) {
  (void)rcWindow;
  if (srtDelay <= 0 || gRenderer == NULL) return;
  int originX = MAIN_OFFSET_X * gZoomFactor;
  int originY = MAIN_OFFSET_Y * gZoomFactor;
  int tileW   = TILE_SIZE_X   * gZoomFactor;
  int tileH   = TILE_SIZE_Y   * gZoomFactor;
  int areaW   = MAIN_SCREEN_SIZE_X * tileW;
  int areaH   = MAIN_SCREEN_SIZE_Y * tileH;
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_FRect r = { (float)originX, (float)originY,
                  (float)areaW, (float)areaH };
  SDL_RenderFillRect(gRenderer, &r);

  /* Render "Game Starts in X" countdown text centred in the blacked-out area */
  if (gFontMsg) {
    char str[64];
    long secs = srtDelay / GAME_NUMGAMETICKS_SEC;
    snprintf(str, sizeof(str), "Game Starts in %ld", secs);
    SDL_Color white = {200, 200, 200, 255};
    /* Measure text width to centre it */
    SDL_Surface *sMeasure = TTF_RenderText_Blended(gFontMsg, str, 0, white);
    if (sMeasure) {
      float tx = (float)originX + ((float)areaW - (float)sMeasure->w) * 0.5f;
      float ty = (float)originY + ((float)areaH - (float)sMeasure->h) * 0.5f;
      SDL_DestroySurface(sMeasure);
      sdl3RenderText(gFontMsg, str, white, tx, ty);
    }
  }
}

void sdl3DrawSetNetFailed(bool v) {
  gNetFailed = v;
}

void sdl3DrawNetFailed(void) {
  if (!gRenderer) return;
  SDL_Color white = {200, 200, 200, 255};
  int originX = MAIN_OFFSET_X * gZoomFactor;
  int originY = MAIN_OFFSET_Y * gZoomFactor;
  float tx = (float)(originX + 3 * TILE_SIZE_X * gZoomFactor);
  float ty = (float)(originY + 8 * TILE_SIZE_Y * gZoomFactor);
  sdl3RenderText(gFontMsg, "Network Failed - Resyncing", white, tx, ty);
}

void sdl3DrawPillInView(void) {
  if (!gRenderer) return;
  SDL_Color white = {200, 200, 200, 255};
  int originX = MAIN_OFFSET_X * gZoomFactor;
  int originY = MAIN_OFFSET_Y * gZoomFactor;
  int fontH = 13 * gZoomFactor;
  float tx = (float)(originX + 2 * gZoomFactor);
  float ty = (float)(originY + MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y * gZoomFactor - fontH - 2 * gZoomFactor);
  sdl3RenderText(gFontMsg, "Pillbox View", white, tx, ty);
}

/* sdl3DrawResetCachedText, sdl3DrawMessages, sdl3DrawGetCachedMessages,
   sdl3DrawKillsDeaths, and sdl3DrawTankLabel moved to sdl3draw_status.c. */

void sdl3DrawGetTabletViewport(int *x, int *y, int *w, int *h, int *zoom) {
  if (x) *x = gTabletVpX;
  if (y) *y = gTabletVpY;
  if (w) *w = gTabletVpW;
  if (h) *h = gTabletVpH;
  if (zoom) *zoom = gTabletVpZoom;
}

void sdl3DrawSetDragOffset(int dx, int dy) {
  gDragOffsetX = dx;
  gDragOffsetY = dy;
}

void sdl3DrawGetDragOffset(int *dx, int *dy) {
  if (dx) *dx = gDragOffsetX;
  if (dy) *dy = gDragOffsetY;
}

void sdl3DrawSetStatusPanelOrigins(float tanksX, float tanksY,
                                    float pillsX, float pillsY,
                                    float basesX, float basesY) {
  gStatusTanksOrgX = tanksX;
  gStatusTanksOrgY = tanksY;
  gStatusPillsOrgX = pillsX;
  gStatusPillsOrgY = pillsY;
  gStatusBasesOrgX = basesX;
  gStatusBasesOrgY = basesY;
  /* sdl3draw_status's moved status renderers consult their own copy
     of these overrides via statusPanelOrigin(); push the new values. */
  sdl3DrawStatusSetPanelOrigins(tanksX, tanksY, pillsX, pillsY, basesX, basesY);
}

void sdl3DrawTabletStatusGrids(ClientSim *cs) {
  if (!gRenderer || !gTilesTex) return;

  sdl3DrawSetBasesStatusClear();
  {
    BYTE total = basesGetNumBases(&cs->sim.bs);
    for (BYTE i = 1; i <= total; i++) {
      sdl3DrawStatusBase(i, screenBaseAllianceCS(cs, i), FALSE);
    }
  }
  sdl3DrawSetPillsStatusClear();
  {
    BYTE total = pillsGetNumPills(&cs->sim.pb);
    for (BYTE i = 1; i <= total; i++) {
      sdl3DrawStatusPillbox(i, screenPillAllianceCS(cs, i), FALSE);
    }
  }
  sdl3DrawSetTanksStatusClear();
  for (BYTE i = 1; i <= MAX_TANKS; i++) {
    sdl3DrawStatusTank(i, screenTankAllianceCS(cs, i));
  }
}
