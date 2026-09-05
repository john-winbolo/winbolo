/*
 * $Id$
 *
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
#include <limits.h>
#include <string.h>

#include "../../common/wb_log.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

#include "stb_image.h"
#include "sdl3draw.h"
#include "sdl3draw_status.h"
#include "sdl3imgui.h"
#if !defined(__ANDROID__) && !defined(__IPHONEOS__)
#include "dialogs/imgui_news.h"
#endif
#include "cursor.h"
#include "mapview.h"
#include "overview_hud_layout.h"
#include "overview_view.h"
#include "../clientmutex.h"
#include "../tiles.h"
#include "../ui_mode.h"
#include "../../steam/steam_wrapper.h"
#include "tileloader.h"
#include "skin_source.h"
#include "sdl_bmp.h"
#include "glyphs.h"
#include "global.h"
#include "client_sim.h"
#include "client_command.h" /* VIEW_KIND_* — which item view the label names */
#include "client_net.h"   /* clientSimGetConnectState — map-transfer progress */
#include "build_cursor.h"
#include "../gamefront.h"
#include "tilenum.h"
#include "../positions.h"
#include "screenbullet.h"
#include "screentank.h"
#include "screenlgm.h"
#include "client_render.h"
#include "macos_pinch.h"
#include "../lang.h"

/* From gui/winbolo.h (can't include directly — Win32 headers) */
#ifndef NO_SELECT
#define NO_SELECT -1
#endif
#ifndef ZOOM_FACTOR_CUSTOM
#define ZOOM_FACTOR_CUSTOM 0
#endif
#ifndef MENU_BAR_HEIGHT
  #ifdef __APPLE__
    #define MENU_BAR_HEIGHT 0
  #else
    #define MENU_BAR_HEIGHT 22
  #endif
#endif

static SDL_Window   *gWindow        = NULL;
static SDL_Renderer *gRenderer      = NULL;
static SDL_Texture  *gBackgroundTex = NULL;
static SDL_Texture  *gTilesTex      = NULL;

/* User option (winbolo.c): gray letterbox/pillarbox bars instead of black. */
extern bool letterboxBarsGray;

/* The status-panel label options the last game frame was drawn with. Every
   game frame is handed them as arguments, but the returning-to-lobby frame
   redraws the same map without going through that call; holding the last pair
   keeps the labels from blinking off for the last moments of a round. */
static bool gLastPillLabels = FALSE;
static bool gLastBaseLabels = FALSE;

/* Fill the whole window before the game render target is composited into
   gGameDestRect.  The exposed border is the letterbox/pillarbox area; make
   it gray instead of black when the option is on.  The fill only shows where
   bars actually exist (the game RT overdraws the rest), so this applies to
   the bars whenever the window aspect differs from the game -- fullscreen or
   a resized/maximised window alike. */
static void sdl3LetterboxFill(void) {
  Uint8 v = letterboxBarsGray ? 107 : 0;
  SDL_SetRenderDrawColor(gRenderer, v, v, v, 255);
  SDL_RenderFillRect(gRenderer, NULL);
}
static SDL_Texture  *gCrosshairTex  = NULL;  /* crosshairs_17x17.png — center pixel (8,8) is aim point */
static bool          gCursorFaint   = false; /* draw the build-mode cursor at 25% alpha (locked target, build mode off) */
static int           gZoomFactor    = 1;

/* Set true while a zoom/skin change reconfigures assets in place. If
   sdl3DrawCleanup is entered while this is set, a renderer/window teardown
   would recreate the Metal layer mid-reconfigure — the exact crash this
   path was rewritten to avoid. */
static bool          s_inZoomSkinReconfigure = false;

void sdl3DrawSetReconfigureGuard(bool active) {
  s_inZoomSkinReconfigure = active;
}

void sdl3DrawSetCursorFaint(bool faint) {
  gCursorFaint = faint;
}
static int           gSheetScale    = 1;  /* atlas scale: sheet is TILE_FILE * gSheetScale */
/* Bumped by sdl3DrawReloadTiles, the skin / tile-detail reload, so a caller
   holding its own atlas (bg_game, the lobby map preview) can tell that copy
   is stale. Not bumped by the zoom rebuild: that changes only this
   texture's scale, and the other atlases are built at their own sizes from
   art that has not changed. */
static unsigned int  gTilesGeneration = 0;

/* How the tile sheet is sampled.  Held here rather than read from
   gfx_settings at each build so a sheet built before the settings are
   loaded still picks the choice up, and so the default matches what the
   game did before the setting existed. */
static SDL_ScaleMode gTilesScaleMode = SDL_SCALEMODE_NEAREST;

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

/* In-window Map Overview mode. The overview is drawn at window size straight
   to the window, so it uses none of the game render target above (which is
   locked to the 515:325 chrome aspect and would letterbox the map). The view
   instance belongs here because the render needs gRenderer, gTilesTex and
   gCrosshairTex; sdl3imgui.cpp reaches it through the accessors below for the
   input handling and status strip, which have to run inside its ImGui frame.
   gOverviewRect is the last blit rect, so both halves agree on where the map
   is on screen. */
static bool          gOverviewInWindow = FALSE;
static OverviewView *gOverviewView     = NULL;
static SDL_FRect     gOverviewRect     = { 0.0f, 0.0f, 0.0f, 0.0f };

/* The HUD geometry the last frame blitted, kept so the ImGui side hit-tests
   the panels on exactly the rectangles that were drawn. Only meaningful while
   the flag is set: a frame that drew no HUD clears it. */
static OverviewHudLayout gOverviewHud;
static bool              gOverviewHudValid = FALSE;

/* The full screen map's newswire starts off the bottom edge, rides up while
   its text is changing and drops back off once it has been quiet for the
   hold time. 0 is fully on screen, 1 fully off; the classic frame never
   touches any of this. */
static float  gOverviewNewsSlide     = 1.0f;
static Uint64 gOverviewNewsSlideTick = 0;
#define OVERVIEW_NEWS_HOLD_MS  30000
#define OVERVIEW_NEWS_SLIDE_MS 300

/* The strip rides up over the middle of the map rather than sitting in a
   corner out of the way, so how much of the terrain reads through it is the
   player's to set, in Settings > Display & Sound > Full Screen. Percent
   transparent: 0 is solid, and the cap stops short of invisible — turning the
   strip off altogether is what the auto-hide tick is for. All three parts of
   it take the same share: the backing, the frame round it, and the slice of
   panel art carrying the two lines of text.

   With auto-hide off the strip stays up for the whole game instead of
   dropping off the bottom edge once the newswire has been quiet. */
#define OVERVIEW_NEWS_TRANSPARENCY_DEFAULT 30

/* The alpha the HUD panels' backing is filled at, and the value the strip
   takes its own share of. */
#define OVERVIEW_HUD_BACK_ALPHA 160

static int  gOverviewNewsTransparency = OVERVIEW_NEWS_TRANSPARENCY_DEFAULT;
static bool gOverviewNewsAutoHide     = TRUE;

/* How far the map is taken down behind the returning-to-lobby caption. Dark
   enough that the caption reads and the round is plainly over, light enough
   that the map is still the thing on screen. */
#define OVERVIEW_LOBBY_DIM_ALPHA 150

/* The item view caption along the bottom of the full screen map: how far its
   bottom sits above the newswire strip, and the padding round the text on its
   backing. */
#define OVERVIEW_ITEM_LABEL_GAP 4.0f
#define OVERVIEW_ITEM_LABEL_PAD 3.0f

/* A whole classic frame at gZoomFactor, drawn offscreen so the HUD column can
   be cut out of it as source rects. gGameRenderTarget cannot be borrowed for
   this: sdl3DrawReconfigureZoom never creates it on the Steam Deck or in
   tablet mode. Rebuilt when the zoom changes, since the frame is drawn at it. */
static SDL_Texture  *gHudSrcTex  = NULL;
static int           gHudSrcZoom = 0;

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

/* Guard: only blit gManStatusTex after the man-status texture has been
   rebuilt from cache on the render thread. Prevents a one-frame artifact
   where the LGM arrow points top-left before the first real angle is
   computed. gManStatusValid tracks whether there is a live man-status to
   show (set by the cache-only setter, cleared by the clear setter);
   gManStatusReady tracks whether the texture has actually been drawn this
   session (set by the render-thread rebuild). */
static bool         gManStatusReady = false;
static bool         gManStatusValid = false;
static bool         gManStatusDead  = false;
static TURNTYPE     gManStatusAngle = 0;

/* Renderer thread ownership. SDL's renderer (and the Metal command queue
   behind it) is not safe to touch from two threads at once. All GPU work
   must run on the thread that created the renderer (the main/render thread).
   We record that thread here at creation time and assert on it in the
   drawing choke points; sim-tick callbacks that used to draw directly now
   only update caches, and the per-frame render pass repaints from them. */
static SDL_ThreadID gRenderThread = 0;

bool sdl3DrawOnRenderThread(void) {
  /* Before the renderer exists (startup) there is no wrong thread yet. */
  return gRenderThread == 0 || SDL_GetCurrentThreadID() == gRenderThread;
}

/* Tablet viewport bounds — set each frame by sdl3DrawMainScreen(),
   read by sdl3DrawGetTabletViewport(). */
static int          gTabletVpX = 0, gTabletVpY = 0;
static int          gTabletVpW = 0, gTabletVpH = 0;
static int          gTabletVpZoom = 0;

/* Drag scroll pixel offset — applied on top of engine edgeX/edgeY
   for smooth sub-tile scrolling from touch dragging. */
static int          gDragOffsetX = 0;
static int          gDragOffsetY = 0;

/* DEBUG: per-frame render log for autoscroll diagnostics. Captures
 * exactly what edgeX/edgeY ends up at on the frame the renderer draws,
 * so we can tell whether smooth engine state translates to smooth
 * rendering or if something downstream re-introduces oscillation.
 * Logs only when values CHANGE from last frame to keep the file small. */
/* Debug file logging. Off for shipping builds — flip to 1 to re-enable
 * the render.log / cursor_square.log traces. When 0 no file is opened
 * or written. */
#define WB_DEBUG_FILE_LOG 0

static FILE *gRenderLog = NULL;
static bool  gRenderLogTried = false;
static int   gRenderLogPrevEngineX = -1, gRenderLogPrevEngineY = -1;
static int   gRenderLogPrevSubX = -1, gRenderLogPrevSubY = -1;
static int   gRenderLogPrevEdgeX = INT_MIN, gRenderLogPrevEdgeY = INT_MIN;
static int   gRenderLogFrameCounter = 0;
static int   gRenderLogEntryCount = 0;

/* Sub-pixel autoscroll smoothing — engine-driven.
 *
 * scroll.c carries sub-tile precision in ScrollState (subPosX/Y in
 * 1/256-tile units) and decomposes a continuous fractional view target
 * into a BYTE *xValue plus that fractional remainder. Per frame the
 * renderer just reads the remainder and folds it into edgeX/Y as a
 * sub-pixel drag offset; nothing here interpolates. The previous
 * prev-vs-last tile snapshot model has been removed.
 *
 * Side effect: tile shifts NOT driven by autoscroll (e.g. the fireball
 * pan during the tank-explosion death sequence) snap by a whole tile
 * each step. Death anim only, brief — accepted tradeoff. */

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
  SDL_SetTextureScaleMode(gTilesTex, gTilesScaleMode);
  sdl3DrawStatusSetAtlas(gTilesTex, gSheetScale);
  return TRUE;
}

unsigned int sdl3DrawGetTilesGeneration(void) {
  return gTilesGeneration;
}

SDL_ScaleMode sdl3DrawScaleModeForFilter(GfxTextureFilter filter) {
  switch (filter) {
    case GFX_FILTER_LINEAR:   return SDL_SCALEMODE_LINEAR;
    case GFX_FILTER_PIXELART: return SDL_SCALEMODE_PIXELART;
    case GFX_FILTER_NEAREST:
    default:                  return SDL_SCALEMODE_NEAREST;
  }
}

void sdl3DrawSetTilesScaleMode(SDL_ScaleMode mode) {
  gTilesScaleMode = mode;
  if (gTilesTex != NULL) {
    SDL_SetTextureScaleMode(gTilesTex, mode);
  }
}

/* Rebuilds only the tile atlas in place (for a skin change) by re-reading
 * the skin assets from disk.  Does not touch the renderer, window, fonts,
 * or zoom.  Bumps the generation whether or not the build succeeds: the art
 * has changed either way, and a consumer that rebuilds against it will see
 * the same failure this path did. */
void sdl3DrawReloadTiles(void) {
  if (gTilesTex) { SDL_DestroyTexture(gTilesTex); gTilesTex = NULL; gSheetScale = 1; }
  sdl3DrawStatusSetAtlas(NULL, 1);
  gTilesGeneration++;
  sdl3LoadTiles();
}

/* Drops the cached background (for a skin change) so the next frame reads it
 * again.  Destroys and clears only: the three draw sites call
 * sdl3LoadBackground() themselves and each is behind !tabletMode, so leaving
 * the load to them keeps tablet mode from building a texture it never draws,
 * and makes no assumption about which thread the caller is on or where in the
 * frame it calls from. */
void sdl3DrawReloadBackground(void) {
  if (gBackgroundTex) {
    SDL_DestroyTexture(gBackgroundTex);
    gBackgroundTex = NULL;
  }
}

/* Reads one BMP by name out of a skin and turns it into a texture.  NULL
 * when the skin carries no such file or the bytes do not decode. */
static SDL_Texture *sdl3LoadSkinBmpTexture(SkinSource *skin, const char *name) {
  void        *buf = NULL;
  size_t       len = 0;
  SDL_Texture *tex = NULL;

  if (!skinSourceRead(skin, name, &buf, &len)) {
    return NULL;
  }
  SDL_IOStream *io = SDL_IOFromMem(buf, len);
  if (io != NULL) {
    /* closeio closes the stream, not the bytes behind it. */
    tex = sdlLoadBmpStreamAsTexture(gRenderer, io, true, false);
  }
  SDL_free(buf);
  return tex;
}

/* Loads the game background as gBackgroundTex: from the active skin when it
 * carries one, otherwise from data/background.bmp beside the
 * executable. */
static bool sdl3LoadBackground(void) {
  if (gBackgroundTex != NULL) {
    return TRUE;
  }

  SkinSource *skin = skinGetActiveSource();
  if (skin != NULL) {
    /* background.bmp is ours; screen.bmp is what 1.x skins call it. */
    static const char *names[] = { "background.bmp", "screen.bmp" };
    for (int i = 0;
         i < (int)(sizeof(names) / sizeof(names[0])) && gBackgroundTex == NULL;
         i++) {
      gBackgroundTex = sdl3LoadSkinBmpTexture(skin, names[i]);
    }
  }

  if (gBackgroundTex == NULL) {
    /* Use SDL_GetBasePath() so the file is found regardless of CWD. */
    const char *basePath = SDL_GetBasePath();
    if (!basePath) basePath = "";
    char pathBuf[512];
    SDL_snprintf(pathBuf, sizeof(pathBuf), "%sdata/background.bmp", basePath);
    gBackgroundTex = sdlLoadBmpAsTexture(gRenderer, pathBuf, false);
  }

  if (gBackgroundTex == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_ASSET, "sdl3DrawBackground: could not load a background: %s", SDL_GetError());
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

/* Render-thread-only rebuild of the man-status texture from cache, and the
   shared drawer it and the full screen HUD both go through; defined later in
   this file. */
static void sdl3RenderManStatusTex(void);
static void sdl3DrawManStatusShape(float dstX, float dstY, float scale,
                                   float stroke, bool isDead, TURNTYPE angle);

/*********************************************************
*NAME:          sdl3RenderStatusPanels
*PURPOSE:
*  Blits all persistent status-panel textures to the
*  main framebuffer at their zoomed screen positions,
*  then draws the current build-select indent overlay.
*********************************************************/
static void sdl3RenderStatusPanels(void) {
  SDL_assert(sdl3DrawOnRenderThread());
  /* In tablet mode the ImGui overlay draws its own resource bars,
     build-select bar, and man-status — skip the desktop versions. */
  if (uiModeIsTablet()) return;

  int zf = gZoomFactor;

  /* Rebuild the render-target textures from cache on this (render) thread.
     The sim-tick callbacks only cache values now; the actual GPU drawing of
     these textures happens here so it can never race the main-thread present.
     Icon panels (bases/pills/tanks) are repainted directly to the framebuffer
     from sim state by the sdl3DrawStatus* calls that run just before this. */
  sdl3RenderTankBarsTex();
  sdl3RenderBaseBarsTex();
  if (gManStatusValid) sdl3RenderManStatusTex();

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

/* Expose the live game-render destination rect + scale so UI code can
 * position ImGui overlays in actual on-screen pixels.
 *
 *   on-screen X = gGameDestRect.x + sourceX * gGameScale
 *
 * In CUSTOM/ceiling-integer zoom mode the game is drawn into an
 * off-screen render target at `gZoomFactor` and then blitted into
 * gGameDestRect, possibly at a non-integer gGameScale. Multiplying
 * source unscaled coords by gZoomFactor alone is wrong when the
 * window has been resized to a fractional effective zoom (e.g.
 * maximized between 3x and 4x). */
void sdl3DrawGetGameRect(float *destX, float *destY,
                          float *destW, float *destH, float *scale) {
  if (destX) *destX = gGameDestRect.x;
  if (destY) *destY = gGameDestRect.y;
  if (destW) *destW = gGameDestRect.w;
  if (destH) *destH = gGameDestRect.h;
  if (scale) *scale = gGameScale;
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

/* The single place the mode is turned on and off, so the pointer the view may
   have switched to the game crosshair is handed back on every way out — the
   menu toggle, the end of a game and teardown all come through here. */
void sdl3DrawSetOverviewInWindow(bool active) {
  if (active == gOverviewInWindow) return;
  gOverviewInWindow = active;
  if (!active) {
    overviewViewReleaseCursor(gOverviewView);
    gOverviewRect.x = gOverviewRect.y = gOverviewRect.w = gOverviewRect.h = 0.0f;
    gOverviewHudValid = FALSE;
  }
}

bool sdl3DrawIsOverviewInWindow(void) {
  return gOverviewInWindow;
}

struct OverviewView *sdl3DrawOverviewInWindowView(void) {
  return gOverviewView;
}

bool sdl3DrawGetOverviewInWindowRect(float *outX, float *outY,
                                     float *outW, float *outH) {
  if (!gOverviewInWindow || gOverviewRect.w <= 0.0f) return false;
  if (outX) *outX = gOverviewRect.x;
  if (outY) *outY = gOverviewRect.y;
  if (outW) *outW = gOverviewRect.w;
  if (outH) *outH = gOverviewRect.h;
  return true;
}

bool sdl3DrawGetOverviewHudLayout(OverviewHudLayout *out) {
  if (!gOverviewInWindow || !gOverviewHudValid || out == NULL) return false;
  *out = gOverviewHud;
  return true;
}

/* Both settings are clamped here rather than trusted, so a hand-edited
   WinBolo.json cannot leave the strip invisible or blacker than the panels. */
void sdl3DrawSetNewswireTransparency(int percent) {
  if (percent < 0) percent = 0;
  if (percent > OVERVIEW_NEWS_TRANSPARENCY_MAX) {
    percent = OVERVIEW_NEWS_TRANSPARENCY_MAX;
  }
  gOverviewNewsTransparency = percent;
}

int sdl3DrawGetNewswireTransparency(void) {
  return gOverviewNewsTransparency;
}

void sdl3DrawSetNewswireAutoHide(bool on) {
  gOverviewNewsAutoHide = on ? TRUE : FALSE;
}

bool sdl3DrawGetNewswireAutoHide(void) {
  return gOverviewNewsAutoHide;
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
  if (uiModeIsSteamDeck()) {
    /* Steam Deck is a desktop build but uses the logical-presentation path
       (not the render-target blit), so the desktop no-op below would leave
       presentation disabled after the front-end dialogs.  Re-apply the same
       letterbox presentation sdl3DrawSetup computed; gZoomFactor already
       holds the deck's bestZoom. */
    SDL_SetRenderLogicalPresentation(gRenderer,
                                     gZoomFactor * SDL3_SCREEN_W,
                                     gZoomFactor * SDL3_SCREEN_H,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    return;
  }
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
      /* The classic 15x15 mapping means nothing while the overview owns the
         window, and the overview drives the shared build cursor itself. */
      if (gOverviewInWindow) break;
      /* Transform window coords to game coords */
      float gameX, gameY;
      if (!windowToGameCoords(ev->motion.x, ev->motion.y, &gameX, &gameY)) {
        clientSimSetCursorPos(cs, 0, 0);
        break;
      }
      cursorMove((int)gameX, (int)gameY);
      BYTE cx = 0, cy = 0;
      if (cursorPos(NULL, &cx, &cy, clientSimGetSubPosX(cs), clientSimGetSubPosY(cs))) {
        /* The mouse is just another way to drive the ONE shared build cursor:
           moving the pointer in the view repositions it whether or not cursor
           mode is active, so toggling build mode picks up exactly where the
           pointer is (no jump between a separate mouse reticle and the gamepad
           cursor).  Only on real movement (skip zero-delta focus events) and
           only while the pointer is in the view (handled by cursorPos) — so
           the pointer leaving the window leaves the gamepad in control.
           cx/cy are 1-based screen tiles; absolute map tile = offset + tile.

           This latch is the selection: it is what the reticle draws and what a
           click builds at, and hand movement is the only thing that moves it.
           The view scrolling under a resting hand must not change it. */
        if (ev->motion.xrel != 0.0f || ev->motion.yrel != 0.0f) {
          buildCursorSetTile((BYTE)((int)clientSimGetXOffset(cs) + (int)cx),
                             (BYTE)((int)clientSimGetYOffset(cs) + (int)cy));
        }
        if (cx > 16 || cy > 16) cx = 100;
        clientSimSetCursorPos(cs, cx, cy);
      } else {
        clientSimSetCursorPos(cs, 0, 0);
      }
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
      /* A click that arrives before ImGui's capture flag catches up would
         build twice — once at the overview's square, once at the classic
         cursor — and the build-select hit-test would fire at chrome
         positions that are not on screen in this mode. */
      if (gOverviewInWindow) break;
      if (ev->button.button == SDL_BUTTON_LEFT) {
        BYTE xVal = 0, yVal = 0;
        if (cursorPos(NULL, &xVal, &yVal, clientSimGetSubPosX(cs), clientSimGetSubPosY(cs))) {
          /* Build at the latched target — the square the reticle is drawn on,
             i.e. where the player last moved the mouse.  Deriving the tile
             from the pointer here instead would re-target the build to
             whatever square the pointer happens to sit over at click time:
             the view scrolls with the tank under a resting hand, and pressing
             a button also resyncs SDL's pointer position, so a click with no
             hand movement would silently move the selection.  Same dispatch
             the gamepad Build Now uses (input.c). */
          BYTE bx = 0, by = 0;
          clientMutexWaitFor();
          if (buildCursorGetTargetTile(&bx, &by)) {
            clientSimManMoveToMap(cs, bx, by, clientSimGetCurrentBuildSelect(cs));
          } else {
            /* Nothing latched yet — this click is the first placement. */
            clientSimManMove(cs, clientSimGetCurrentBuildSelect(cs));
          }
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
          if (newSelect != NO_SELECT && newSelect != clientSimGetCurrentBuildSelect(cs)) {
            sdl3DrawSelectIndentsOff(clientSimGetCurrentBuildSelect(cs), 0, 0);
            sdl3DrawSelectIndentsOn(newSelect, 0, 0);
            clientSimSetCurrentBuildSelect(cs, newSelect);
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

/* Decode data/crosshairs_17x17.png onto the given renderer. Returns NULL if
 * the file is missing or anything in the decode fails; the caller owns the
 * texture and destroys it. */
SDL_Texture *sdl3DrawCreateCrosshairTexture(SDL_Renderer *r) {
  SDL_Texture *tex = NULL;

  if (!r) return NULL;

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
            tex = SDL_CreateTextureFromSurface(r, surf);
            SDL_DestroySurface(surf);
            if (tex) {
              SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
              SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
            }
          }
          stbi_image_free(pix);
        }
      }
    }
    SDL_CloseIO(io);
  }
  return tex;
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
     dimensions (it temporarily sets the canvas to 1x1 and checks CSS).
     SDL3's Emscripten backend creates the window at the existing canvas
     size rather than honouring the size passed to SDL_CreateWindow, so
     this must already include the menu-bar row — otherwise the window
     ends up 22px short and the game blits at a non-integer downscale. */
  {
    int cw = zoomFactor * SDL3_SCREEN_W;
    int ch = zoomFactor * SDL3_SCREEN_H + MENU_BAR_HEIGHT;
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
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    gWindow = SDL_CreateWindow("WinBolo",
                               zoomFactor * SDL3_SCREEN_W,
                               zoomFactor * SDL3_SCREEN_H + MENU_BAR_HEIGHT,
                               0);
#else
    /* Big Picture / Gamepad UI: launch maximised so the game fills the
       couch-mode surface.  Window size/position prefs are not persisted in
       this mode (see gameFrontFlushWindowSettings). */
    SDL_WindowFlags bigPictureFlag =
        steam_is_big_picture() ? SDL_WINDOW_MAXIMIZED : 0;
    gWindow = SDL_CreateWindow("WinBolo",
                               zoomFactor * SDL3_SCREEN_W,
                               zoomFactor * SDL3_SCREEN_H + MENU_BAR_HEIGHT,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                               bigPictureFlag);
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
  /* Pin renderer ownership to this thread; every later GPU call is
     asserted against it (see sdl3DrawOnRenderThread). */
  gRenderThread = SDL_GetCurrentThreadID();

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

  /* macOS: disable the press-and-hold accent picker so held keys repeat,
     and start trackpad pinch-to-zoom monitoring. */
  macOSDisablePressAndHold();
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
  gCrosshairTex = sdl3DrawCreateCrosshairTexture(gRenderer);

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

#if !defined(__ANDROID__) && !defined(__IPHONEOS__)
  /* Desktop + wasm: create a render target for the game content.
     The game is rendered at its logical size, then blitted scaled to the
     window below the menu bar. This allows the menu to stay at 1x size
     while the game scales, and gives windowToGameCoords a defined
     gGameDestRect so mouse clicks map back to game cells. (On Emscripten
     the window is never resizable, so sdl3DrawAdaptRenderTarget is a
     no-op and this target persists for the session.) Skipped on Deck —
     logical presentation already upscales the whole layout, an extra RT
     would re-introduce a 1x rasterization step that defeats the font
     sharpness. */
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
  if (s_inZoomSkinReconfigure) {
    WB_LOG_ERROR(WB_LOG_CAT_GUI,
      "sdl3DrawCleanup entered during a zoom/skin reconfigure — a renderer/window "
      "teardown here recreates the Metal layer and crashes the Steam overlay");
    SDL_assert(!s_inZoomSkinReconfigure);
  }
  tileLoaderCleanup();
  cursorCleanup();

  /* ImGui cleanup before destroying renderer/window */
  sdl3ImguiCleanup();

#if !defined(__ANDROID__) && !defined(__IPHONEOS__)
  /* News popup teardown sits in this process-exit hook (not in
   * sdl3ImguiCleanup) because sdl3ImguiCleanup also fires on every
   * game-end / return-to-lobby transition — releasing the fetch
   * handle there would prevent the View News button from working
   * after the first game. The SDL renderer is still alive here, so
   * newsImageCacheShutdown's SDL_DestroyTexture calls land cleanly. */
  newsPopupShutdown();
#endif

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
  if (gStaticTex)        { SDL_DestroyTexture(gStaticTex);        gStaticTex        = NULL; }
  if (gGameRenderTarget) { SDL_DestroyTexture(gGameRenderTarget); gGameRenderTarget = NULL; }
  if (gHudSrcTex)        { SDL_DestroyTexture(gHudSrcTex);        gHudSrcTex        = NULL; }
  if (gTilesTex) {
    SDL_DestroyTexture(gTilesTex);
    gTilesTex = NULL;
    gSheetScale = 1;
  }
  if (gBackgroundTex) {
    SDL_DestroyTexture(gBackgroundTex);
    gBackgroundTex = NULL;
  }
  /* The in-window overview's offscreen was made on gRenderer, so it goes
     before the renderer does. */
  overviewViewDestroy(gOverviewView);
  gOverviewView = NULL;
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
 * sdl3DrawReconfigureZoom — rebuild all zoom-dependent
 * assets in place against the live renderer.  explicitZoom
 * >= 1 uses that integer render zoom directly; explicitZoom
 * == 0 (Custom mode) derives the ceiling integer zoom from
 * the current window size so the render target is always
 * >= the window size, then downscales the blit for crisp
 * output.  Does not touch the renderer or window.
 * ------------------------------------------------------- */
void sdl3DrawReconfigureZoom(int explicitZoom) {
  (void)explicitZoom;
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(__IPHONEOS__)
  if (!gRenderer || !gWindow) return;
  if (uiModeIsTablet()) return;
  /* Deck is fullscreen 1280x800 with logical presentation already set in
     sdl3DrawSetup.  No resizes ever fire here; even if they did, this
     function would recompute gZoomFactor and stomp the value the Deck
     branch picked. */
  if (uiModeIsSteamDeck()) return;

  int needZoom;
  if (explicitZoom >= 1) {
    /* Caller supplied an explicit integer render zoom (cardinal/menu zoom) */
    needZoom = explicitZoom;
  } else {
    /* Derive the ceiling integer zoom from the current window size */
    int winW, winH;
    SDL_GetCurrentRenderOutputSize(gRenderer, &winW, &winH);

    /* Ceiling integer zoom: smallest integer where zoom * gameSize >= windowSize */
    int zoomForW = (winW + SDL3_SCREEN_W - 1) / SDL3_SCREEN_W;
    int zoomForH = (winH + SDL3_SCREEN_H - 1) / SDL3_SCREEN_H;
    needZoom = (zoomForW > zoomForH) ? zoomForW : zoomForH;
    if (needZoom < 1) needZoom = 1;
  }

  /* Nothing to do if already at the right zoom */
  if (needZoom == gZoomFactor && gGameRenderTarget != NULL) return;

  WB_LOG_INFO(WB_LOG_CAT_GUI, "sdl3DrawReconfigureZoom: -> zoom %d (was %d)",
          needZoom, gZoomFactor);

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

/* Per-frame entry: only rebuilds in Custom zoom mode, deriving the
 * integer render zoom from the current window size. */
static void sdl3DrawAdaptRenderTarget(void) {
  extern BYTE zoomFactor;
  if (zoomFactor != ZOOM_FACTOR_CUSTOM) return;   /* mode gate — CUSTOM only */
  sdl3DrawReconfigureZoom(0);                       /* derive from window */
}

/* Called once per drawn frame by whichever branch drew it, so the FPS readout
   keeps working in the in-window overview as well as the classic view. */
static void sdl3DrawCountFrame(void) {
  g_dwFrameCount++;
  DWORD now = (DWORD)SDL_GetTicks();
  DWORD elapsed = now - g_dwFrameTime;
  if (elapsed > 1000) {
    g_dwFrameTotal = g_dwFrameCount;
    g_dwFrameTime = now;
    g_dwFrameCount = 0;
  }
}

/* Draws one whole classic frame into gHudSrcTex: the background bitmap, the
   three item grids, the status panels and the cached text, all at the
   positions.h coordinates they already live at. The HUD column is then nine
   source rects out of it, so the panel bevels, the build-item pictures, the
   selected indent, the bar labels and the numbers all come along without any
   of them being repositioned.

   Swaps the render target, so it has to run before any window drawing in the
   frame; the caller's target is saved and put back. Returns false when the
   scratch target could not be made, in which case no HUD is drawn. */
static bool hudSourceRender(ClientSim *cs, bool showPillLabels, bool showBaseLabels) {
  /* Read before the block below, which leaves the target on the window when
     it has to build the texture. */
  SDL_Texture *savedTarget = SDL_GetRenderTarget(gRenderer);

  if (gHudSrcTex != NULL && gHudSrcZoom != gZoomFactor) {
    SDL_DestroyTexture(gHudSrcTex);
    gHudSrcTex = NULL;
  }
  if (gHudSrcTex == NULL) {
    gHudSrcTex = sdl3CreateRenderTarget(gZoomFactor * SDL3_SCREEN_W,
                                        gZoomFactor * SDL3_SCREEN_H);
    if (gHudSrcTex == NULL) return false;
    gHudSrcZoom = gZoomFactor;
  }

  SDL_SetRenderTarget(gRenderer, gHudSrcTex);

  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  if (sdl3LoadBackground()) {
    SDL_FRect bgDest = { 0.0f, 0.0f,
                         (float)(gZoomFactor * SDL3_SCREEN_W),
                         (float)(gZoomFactor * SDL3_SCREEN_H) };
    SDL_RenderTexture(gRenderer, gBackgroundTex, NULL, &bgDest);
  }

  sdl3DrawSetBasesStatusClear();
  {
    BYTE total = clientSimGetBaseCount(cs);
    for (BYTE i = 1; i <= total; i++) {
      sdl3DrawStatusBase(i, clientSimGetBaseAlliance(cs, i), showBaseLabels);
    }
  }
  sdl3DrawSetPillsStatusClear();
  {
    BYTE total = clientSimGetPillCount(cs);
    for (BYTE i = 1; i <= total; i++) {
      sdl3DrawStatusPillbox(i, clientSimGetPillAlliance(cs, i), showPillLabels);
    }
  }
  sdl3DrawSetTanksStatusClear();
  for (BYTE i = 1; i <= MAX_TANKS; i++) {
    sdl3DrawStatusTank(i, clientSimGetTankAlliance(cs, i));
  }

  /* Safe with our target set: the three bar/man texture builders each take
     SDL_GetRenderTarget first and put it back. */
  sdl3RenderStatusPanels();
  sdl3RenderCachedText();

  SDL_SetRenderTarget(gRenderer, savedTarget);
  return true;
}

/* A thin chrome frame just outside one HUD backing rect, in the background
   art's greys: a light line on the outside, the chrome grey as the body, a
   dark line against the backing — the classic window bevel at HUD scale.
   The panel's own opacity comes in as alpha: 255 for the solid panels, the
   newswire's value for the strip. The caller leaves the draw blend mode on
   BLEND, which an alpha of 255 comes out of unchanged. */
static void sdl3DrawOverviewHudFrame(const SDL_FRect *r, float scale,
                                     Uint8 alpha) {
  int body = (int)SDL_ceilf(2.0f * scale);
  if (body < 2) body = 2;
  int rings = body + 2;   /* + the light outer and dark inner lines */
  for (int i = 1; i <= rings; i++) {
    if (i == 1) {
      SDL_SetRenderDrawColor(gRenderer, 49, 49, 49, alpha);
    } else if (i == rings) {
      SDL_SetRenderDrawColor(gRenderer, 165, 165, 165, alpha);
    } else {
      SDL_SetRenderDrawColor(gRenderer, 107, 107, 107, alpha);
    }
    SDL_FRect o = { r->x - (float)i, r->y - (float)i,
                    r->w + 2.0f * (float)i, r->h + 2.0f * (float)i };
    SDL_RenderRect(gRenderer, &o);
  }
}

/* In-window Map Overview: the whole game window is the map, and neither the
   classic 15x15 view nor the chrome is drawn. The view renders at window size
   into its own offscreen and is blitted straight to the window — going through
   gGameRenderTarget would letterbox the map to the 515:325 chrome aspect. The
   status panels and the newswire go over the map afterwards, as slices of a
   classic frame drawn offscreen alongside the view. */
static void sdl3DrawOverviewInWindowFrame(ClientSim *cs, bool showPillLabels,
                                          bool showBaseLabels) {
  /* Stale the moment this frame starts: every way out below either lays a new
     HUD out or draws none at all. */
  gOverviewHudValid = FALSE;

  int ww = 0, wh = 0;
  {
    SDL_RendererLogicalPresentation logMode;
    SDL_GetRenderLogicalPresentation(gRenderer, &ww, &wh, &logMode);
    if (ww <= 0 || wh <= 0 || logMode == SDL_LOGICAL_PRESENTATION_DISABLED) {
      SDL_GetCurrentRenderOutputSize(gRenderer, &ww, &wh);
    }
  }

  /* Zero when the menu bar is hidden (controller mode) so the map fills the
     freed top strip — same expression the classic blit uses. */
  float menuBarHeight = uiShouldUseControllerMode() ? 0.0f : (float)MENU_BAR_HEIGHT;
  int w = ww;
  int h = (int)((float)wh - menuBarHeight);
  if (w < 1 || h < 1) return;

  /* The classic draw loads the atlas lazily on its way past; this branch
     never gets there, so it loads it itself. */
  sdl3LoadTiles();

  if (!gOverviewView) gOverviewView = overviewViewCreate();
  if (!gOverviewView) return;

  overviewViewRenderOffscreen(gOverviewView, gRenderer, gTilesTex, gSheetScale,
                              gCrosshairTex, w, h, cs, true);

  /* Both offscreen passes belong here, before anything is drawn to the
     window: each of them swaps the render target. */
  OverviewHudLayout hud;
  bool drawHud = overviewHudLayout(w, h, &hud) &&
                 hudSourceRender(cs, showPillLabels, showBaseLabels);
  if (drawHud) {
    /* Slide the newswire: wanted on screen while its text has changed within
       the hold time, off the bottom edge otherwise. The offset moves the
       backing, the chrome frame, the slice and the click rect together, so
       everything below reads the adjusted rects. */
    Uint64 now     = SDL_GetTicks();
    Uint64 lastMsg = sdl3DrawGetMessageActivityTick();
    /* Auto-hide off wants it up whatever the newswire has been doing, so it
       rides up on the next frame and stays there. */
    bool newsWanted = !gOverviewNewsAutoHide ||
                      (lastMsg != 0 && (now - lastMsg) < OVERVIEW_NEWS_HOLD_MS);
    float step = (gOverviewNewsSlideTick == 0)
                     ? 1.0f
                     : (float)(now - gOverviewNewsSlideTick) /
                           (float)OVERVIEW_NEWS_SLIDE_MS;
    gOverviewNewsSlideTick = now;
    gOverviewNewsSlide += newsWanted ? -step : step;
    if (gOverviewNewsSlide < 0.0f) gOverviewNewsSlide = 0.0f;
    if (gOverviewNewsSlide > 1.0f) gOverviewNewsSlide = 1.0f;
    if (gOverviewNewsSlide > 0.0f) {
      /* Far enough down that the chrome frame's outer line leaves too. */
      float travel = hud.newswireH + SDL_ceilf(2.0f * hud.scale) + 3.0f;
      float off    = gOverviewNewsSlide * travel;
      hud.newswireY += off;
      hud.el[OVERVIEW_HUD_NEWSWIRE].dstY += off;
    }
    gOverviewHud      = hud;
    gOverviewHudValid = TRUE;
  }

  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  SDL_Texture *tex = overviewViewGetTexture(gOverviewView);
  if (tex) {
    SDL_FRect dest = { 0.0f, menuBarHeight, (float)w, (float)h };
    /* BLENDMODE_NONE for the same reason the classic blit uses it: alpha
       below 255 left in a render target would composite semi-transparent. */
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(gRenderer, tex, NULL, &dest);
    /* Both halves work from this rect: the ImGui side puts its pan item and
       status strip over exactly these numbers. */
    gOverviewRect = dest;
  }

  if (drawHud) {
    /* The layout works in map-rect pixels; the rect starts below the menu bar. */
    float originX = 0.0f;
    float originY = menuBarHeight;

    /* The strip's share of every colour it is drawn in, from the setting. */
    int   newsOpaque    = 100 - gOverviewNewsTransparency;
    Uint8 newsAlpha     = (Uint8)((255 * newsOpaque) / 100);
    Uint8 newsBackAlpha = (Uint8)((OVERVIEW_HUD_BACK_ALPHA * newsOpaque) / 100);

    /* Translucent backing, so the map still reads between the panels. */
    SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, OVERVIEW_HUD_BACK_ALPHA);
    SDL_FRect colBack = { originX + hud.columnX, originY + hud.columnY,
                          hud.columnW, hud.columnH };
    SDL_FRect buildBack = { originX + hud.buildX, originY + hud.buildY,
                            hud.buildW, hud.buildH };
    SDL_FRect newsBack = { originX + hud.newswireX, originY + hud.newswireY,
                           hud.newswireW, hud.newswireH };
    SDL_RenderFillRect(gRenderer, &colBack);
    SDL_RenderFillRect(gRenderer, &buildBack);

    /* The newswire's backing is only the margin between its frame and the
       slice, not the whole rect. The slice is translucent, and a fill under
       it would darken the map through it a second time — the strip would come
       out a good deal less see-through than the setting asked for. The margin
       is what the layout left round the slice on every side. */
    {
      float pad = hud.el[OVERVIEW_HUD_NEWSWIRE].dstX - hud.newswireX;
      if (pad > 0.0f) {
        SDL_FRect ring[4] = {
          { newsBack.x, newsBack.y, newsBack.w, pad },
          { newsBack.x, newsBack.y + newsBack.h - pad, newsBack.w, pad },
          { newsBack.x, newsBack.y + pad, pad, newsBack.h - 2.0f * pad },
          { newsBack.x + newsBack.w - pad, newsBack.y + pad, pad,
            newsBack.h - 2.0f * pad }
        };
        SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, newsBackAlpha);
        SDL_RenderFillRects(gRenderer, ring, 4);
      }
    }

    /* The chrome edging the classic window puts around its panels; the
       newswire strip sits on the bottom edge, so its bottom line is
       clipped by the window and the rest frames it. Drawn before the blend
       mode goes back to NONE, since the strip's frame is translucent. */
    sdl3DrawOverviewHudFrame(&colBack, hud.scale, 255);
    sdl3DrawOverviewHudFrame(&buildBack, hud.scale, 255);
    sdl3DrawOverviewHudFrame(&newsBack, hud.scale, newsAlpha);
    SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_NONE);

    /* The ridge across the column between the base bars and the tank bars,
       in the same greys as the frames: grey body, light top, dark bottom. */
    {
      SDL_FRect d = { originX + hud.dividerX, originY + hud.dividerY,
                      hud.dividerW, hud.dividerH };
      SDL_SetRenderDrawColor(gRenderer, 107, 107, 107, 255);
      SDL_RenderFillRect(gRenderer, &d);
      SDL_FRect edge = { d.x, d.y, d.w, 1.0f };
      SDL_SetRenderDrawColor(gRenderer, 165, 165, 165, 255);
      SDL_RenderFillRect(gRenderer, &edge);
      edge.y = d.y + d.h - 1.0f;
      SDL_SetRenderDrawColor(gRenderer, 49, 49, 49, 255);
      SDL_RenderFillRect(gRenderer, &edge);
    }

    /* The panel art is opaque, like the classic panel blits, so the backing
       shows only in the gaps between the pieces — the newswire slice is the
       one exception, blended over the map at the strip's own alpha. Linear
       filtering because the column is a downscale from gZoomFactor to the fit
       scale, and nearest aliases the panel artwork and the digits. */
    SDL_SetTextureBlendMode(gHudSrcTex, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(gHudSrcTex, SDL_SCALEMODE_LINEAR);
    for (int i = 0; i < OVERVIEW_HUD_COUNT; i++) {
      const OverviewHudElement *e = &hud.el[i];
      /* Every other element is artwork or text, which resamples from the
         source frame's zoom to the fit scale well enough. The LGM indicator
         is strokes — a ring one pixel wide and a line — and rescaling those
         is what loses them: the ring comes out uneven, bright where it landed
         on pixel centres and grey where it straddled two. It is drawn below
         at the scale it is shown at instead. */
      if (i == OVERVIEW_HUD_MANSTATUS) continue;
      /* The one translucent piece. The source frame is cleared and filled
         opaque, so every texel under the slice is alpha 255 and the mod is
         what sets the strip's opacity on its own. Put back straight after:
         the texture is the source for every other panel and for the next
         frame's blits. */
      if (i == OVERVIEW_HUD_NEWSWIRE) {
        SDL_SetTextureBlendMode(gHudSrcTex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(gHudSrcTex, newsAlpha);
      }
      SDL_FRect src = { (float)(e->srcX * gZoomFactor), (float)(e->srcY * gZoomFactor),
                        (float)(e->srcW * gZoomFactor), (float)(e->srcH * gZoomFactor) };
      SDL_FRect dst = { originX + e->dstX, originY + e->dstY, e->dstW, e->dstH };
      SDL_RenderTexture(gRenderer, gHudSrcTex, &src, &dst);
      if (i == OVERVIEW_HUD_NEWSWIRE) {
        SDL_SetTextureAlphaMod(gHudSrcTex, 255);
        SDL_SetTextureBlendMode(gHudSrcTex, SDL_BLENDMODE_NONE);
      }
    }

    /* The LGM indicator, drawn rather than copied. The black behind it stands
       in for the cleared texture the classic view blits, so the box reads the
       same as the opaque chrome either side of it. Its stroke is half a source
       pixel, which is the one pixel the classic view draws at zoom 2 and keeps
       the ring the same weight against the circle as the HUD scales up. */
    {
      bool manDead = false;
      TURNTYPE manAngle = 0;
      if (sdl3DrawGetManStatusState(&manDead, &manAngle)) {
        const OverviewHudElement *e = &hud.el[OVERVIEW_HUD_MANSTATUS];
        float stroke = hud.scale * 0.5f;
        if (stroke < 1.0f) stroke = 1.0f;

        SDL_FRect box = { originX + e->dstX, originY + e->dstY,
                          e->dstW, e->dstH };
        SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
        SDL_RenderFillRect(gRenderer, &box);
        sdl3DrawManStatusShape(box.x, box.y, hud.scale, stroke, manDead,
                               manAngle);
      }
    }
  }

  /* Name the item view across the bottom. The yellow border round the picture
     (overview_view.cpp) says one is on; this says which. Drawn by the host
     rather than into the offscreen for two reasons: the font's textures belong
     to the renderer that made them, and only here is it known where the
     newswire has slid to this frame. */
  {
    char label[128];
    int textW = 0;
    int textH = 0;
    if (gFontMsg && sdl3DrawGetItemViewLabel(cs, label, sizeof(label)) &&
        TTF_GetStringSize(gFontMsg, label, 0, &textW, &textH)) {
      /* The text rides on the newswire strip's top edge, so chat never covers
         it; hud.newswireY already carries the slide offset. Once the strip has
         slid fully away its recorded top is below the window, so the bottom of
         the map rect is what the text comes to rest against. */
      float bottom = menuBarHeight + (float)h;
      if (drawHud && menuBarHeight + hud.newswireY < bottom) {
        bottom = menuBarHeight + hud.newswireY;
      }
      float ty = bottom - OVERVIEW_ITEM_LABEL_GAP - (float)textH;
      float tx = ((float)w - (float)textW) * 0.5f;  /* the map rect starts at 0 */

      /* The text sits over terrain of any colour, so it gets the same
         translucent backing as the HUD panels and the zoom readout. */
      SDL_FRect back = { tx - OVERVIEW_ITEM_LABEL_PAD,
                         ty - OVERVIEW_ITEM_LABEL_PAD,
                         (float)textW + 2.0f * OVERVIEW_ITEM_LABEL_PAD,
                         (float)textH + 2.0f * OVERVIEW_ITEM_LABEL_PAD };
      SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 160);
      SDL_RenderFillRect(gRenderer, &back);
      SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_NONE);

      SDL_Color white = {200, 200, 200, 255};
      sdl3RenderText(gFontMsg, label, white, tx, ty);
    }
  }

  sdl3DrawCountFrame();
}

void sdl3DrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                        screenGunsight *gs, screenBullets *sBullets, screenLgm *lgms,
                        RECT *rcWindow, bool showPillLabels, bool showBaseLabels,
                        int32_t srtDelay, bool isItemView, int edgeX, int edgeY,
                        bool useCursor, BYTE cursorLeft, BYTE cursorTop) {
  (void)rcWindow;

  if (gRenderer == NULL) {
    return;
  }

  /* Held for the returning-to-lobby frame, which redraws the map with no
     arguments of its own. */
  gLastPillLabels = showPillLabels;
  gLastBaseLabels = showBaseLabels;

  if (gOverviewInWindow) {
    sdl3DrawOverviewInWindowFrame(cs, showPillLabels, showBaseLabels);
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

  /* Sub-pixel autoscroll — read engine sub-tile remainder and fold
   * into edgeX/Y as a fractional drag offset. See the comment block at
   * the top of this file. */
  if (cs != NULL) {
    /* An item view is camera-locked on what it is watching and must stay
     * exactly centred — no sub-tile drift. scrollCenterObject already zeroes
     * subPos on entry / cycling / return-to-tank, but ignore it here too so
     * the pill, base or allied tank can never render a fraction of a tile
     * off centre regardless of what subPos last held. */
    bool inItemView = clientSimIsInItemView(cs);
    int subX    = inItemView ? 0 : clientSimGetSubPosX(cs);  /* 0..255, 1/256-tile units */
    int subY    = inItemView ? 0 : clientSimGetSubPosY(cs);
    int tileWpx = TILE_SIZE_X * gZoomFactor;
    int tileHpx = TILE_SIZE_Y * gZoomFactor;
    edgeX += subX * tileWpx / 256;
    edgeY += subY * tileHpx / 256;

    /* DEBUG: log frames where rendering offset CHANGES, capped at 600
     * entries (~10s at 60fps) so we can see whether the renderer
     * draws smoothly or shakes given a smooth engine input. */
    if (WB_DEBUG_FILE_LOG) {
    if (!gRenderLog && !gRenderLogTried) {
      gRenderLogTried = true;
      gRenderLog = fopen("render.log", "a");
      if (gRenderLog) {
        fprintf(gRenderLog, "--- render log: f=frame engine=(xOff,yOff) sub=(subX,subY) drag=(dx,dy) edge=(edgeX,edgeY) zoom=z ---\n");
        fflush(gRenderLog);
      }
    }
    gRenderLogFrameCounter++;
    int engineX = (int)clientSimGetXOffset(cs);
    int engineY = (int)clientSimGetYOffset(cs);
    bool changed = (engineX != gRenderLogPrevEngineX || engineY != gRenderLogPrevEngineY ||
                    subX != gRenderLogPrevSubX || subY != gRenderLogPrevSubY ||
                    edgeX != gRenderLogPrevEdgeX || edgeY != gRenderLogPrevEdgeY);
    if (gRenderLog && changed && gRenderLogEntryCount < 600) {
      fprintf(gRenderLog, "[f=%d] engine=(%d,%d) sub=(%d,%d) drag=(%d,%d) edge=(%d,%d) zoom=%d\n",
              gRenderLogFrameCounter, engineX, engineY, subX, subY,
              gDragOffsetX, gDragOffsetY, edgeX, edgeY, gZoomFactor);
      fflush(gRenderLog);
      gRenderLogEntryCount++;
      gRenderLogPrevEngineX = engineX;
      gRenderLogPrevEngineY = engineY;
      gRenderLogPrevSubX = subX;
      gRenderLogPrevSubY = subY;
      gRenderLogPrevEdgeX = edgeX;
      gRenderLogPrevEdgeY = edgeY;
    }
    }
  }

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
    } else if (!isItemView && clientSimGetMyTankDeathWait(cs) != 0 &&
               ((clientSimGetMyTankLastDeath(cs) == LAST_DEATH_BY_DEEPSEA && clientSimGetMyTankDeathWait(cs) < STATIC_ON_TICKS_DEEPSEA) ||
                (clientSimGetMyTankLastDeath(cs) == LAST_DEATH_BY_SHELL   && clientSimGetMyTankDeathWait(cs) < STATIC_ON_TICKS_SHELL) ||
                (clientSimGetMyTankLastDeath(cs) == LAST_DEATH_BY_MINES   && clientSimGetMyTankDeathWait(cs) < STATIC_ON_TICKS_MINES))) {
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
      if (clientSimGetMyTankDeathWait(cs) != gStaticLast) {
        gStaticLast = clientSimGetMyTankDeathWait(cs);
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
            labelNum = clientSimGetPillNumPos(cs, lx, ly) - 1;
          } else if (isBase && showBaseLabels) {
            labelNum = clientSimGetBaseNumPos(cs, lx, ly) - 1;
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

      /* Build-mode cursor overlay */
      if (useCursor) {
        SDL_FRect curSrc = { (float)(MOUSE_SQUARE_X * gSheetScale), (float)(MOUSE_SQUARE_Y * gSheetScale),
                             (float)(TILE_SIZE_X * gSheetScale), (float)(TILE_SIZE_Y * gSheetScale) };
        float curDestX = (float)(originX + ((int)cursorLeft - 1) * tileW - edgeX);
        float curDestY = (float)(originY + ((int)cursorTop  - 1) * tileH - edgeY);
        SDL_FRect curDest = { curDestX, curDestY, (float)tileW, (float)tileH };
        /* Faint (50% alpha) when drawing a locked build target with build
           mode off; solid otherwise. Restore alpha after so other gTilesTex
           draws this frame are unaffected. */
        if (gCursorFaint) SDL_SetTextureAlphaMod(gTilesTex, 128);
        SDL_RenderTexture(gRenderer, gTilesTex, &curSrc, &curDest);
        if (gCursorFaint) SDL_SetTextureAlphaMod(gTilesTex, 255);

        /* DEBUG: log the cursor square position relative to the render
         * origin and edgeX, so a "square doesn't match mouse" report can
         * be cross-referenced with the cursor.log entry that produced
         * the cursorLeft/Top values. Logs only when those inputs change. */
        if (WB_DEBUG_FILE_LOG) {
          static int sLastCl = -1, sLastCt = -1, sLastEdgeX = INT_MIN, sLastEdgeY = INT_MIN;
          static FILE *sLog = NULL;
          static bool sLogTried = false;
          static int sCount = 0;
          if (!sLog && !sLogTried) {
            sLogTried = true;
            sLog = fopen("cursor_square.log", "a");
            if (sLog) fprintf(sLog, "--- cursor_square session start ---\n");
          }
          if (sLog && sCount < 2000 &&
              ((int)cursorLeft != sLastCl || (int)cursorTop != sLastCt ||
               edgeX != sLastEdgeX || edgeY != sLastEdgeY)) {
            fprintf(sLog, "[sq] cur=(%u,%u) edge=(%d,%d) zf=%d destPx=(%.1f,%.1f) tileW=%d tileH=%d\n",
                    (unsigned)cursorLeft, (unsigned)cursorTop, edgeX, edgeY, gZoomFactor,
                    (double)curDestX, (double)curDestY, tileW, tileH);
            fflush(sLog);
            sLastCl = (int)cursorLeft;
            sLastCt = (int)cursorTop;
            sLastEdgeX = edgeX;
            sLastEdgeY = edgeY;
            sCount++;
          }
        }
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

      /* Gunsight overlay — custom 17×17 crosshair, center pixel (8,8) = aim point.
       * Top-left is at the same position as the old 16×16 tile sprite so the
       * center aligns with the gunsight world position. Drawn after the sprite
       * passes so the aiming reticle stays on top of tanks (incl. boat tanks),
       * shells, and LGMs rather than being painted over by them. */
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

      /* Phase 5 overlays (inside clip rect so they stay within the game area) */
      if (isItemView) {
        sdl3DrawItemInView(cs);
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
      BYTE total = clientSimGetBaseCount(cs);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusBase(i, clientSimGetBaseAlliance(cs, i), showBaseLabels);
      }
    }
    sdl3DrawSetPillsStatusClear();
    {
      BYTE total = clientSimGetPillCount(cs);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusPillbox(i, clientSimGetPillAlliance(cs, i), showPillLabels);
      }
    }
    sdl3DrawSetTanksStatusClear();
    for (BYTE i = 1; i <= MAX_TANKS; i++) {
      sdl3DrawStatusTank(i, clientSimGetTankAlliance(cs, i));
    }

    sdl3RenderStatusPanels();
    sdl3RenderCachedText();
  }

  sdl3DrawCountFrame();

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

    /* Zero when the menu bar is hidden (controller mode) so the game render
       fills the freed top strip instead of leaving a MENU_BAR_HEIGHT band. */
    float menuBarHeight = uiShouldUseControllerMode() ? 0.0f : (float)MENU_BAR_HEIGHT;

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
       Letterbox/pillarbox fill — visible when the window aspect ratio
       differs from the game (e.g. fullscreen on a widescreen monitor). */
    sdl3LetterboxFill();
    SDL_SetTextureBlendMode(gGameRenderTarget, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(gRenderer, gGameRenderTarget, NULL, &gGameDestRect);
  }
}

void sdl3DrawRedrawAll(ClientSim *cs, buildSelect value, RECT *rcWindow,
                       bool showPillsStatus, bool showBasesStatus) {
  (void)rcWindow;
  if (gRenderer == NULL) return;

  /* The in-window overview redraws the whole window from sim state every
     frame, so a full repaint has nothing to add — and the classic chrome it
     would draw is not part of that mode. */
  if (gOverviewInWindow) return;

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
      BYTE total = clientSimGetBaseCount(cs);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusBase(i, clientSimGetBaseAlliance(cs, i), showBasesStatus);
      }
    }
    sdl3DrawSetPillsStatusClear();
    {
      BYTE total = clientSimGetPillCount(cs);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusPillbox(i, clientSimGetPillAlliance(cs, i), showPillsStatus);
      }
    }
    sdl3DrawSetTanksStatusClear();
    for (BYTE i = 1; i <= MAX_TANKS; i++) {
      sdl3DrawStatusTank(i, clientSimGetTankAlliance(cs, i));
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
    float menuBarHeight = uiShouldUseControllerMode() ? 0.0f : (float)MENU_BAR_HEIGHT;
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

    /* Letterbox/pillarbox fill — visible when window aspect differs
       from the game (e.g. fullscreen on a widescreen monitor). */
    sdl3LetterboxFill();
    SDL_SetTextureBlendMode(gGameRenderTarget, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(gRenderer, gGameRenderTarget, NULL, &gGameDestRect);
  }
}

/* Blit gGameRenderTarget into the window scaled to fit below the menu bar.
 * Mirrors the end-of-frame blit in sdl3DrawMainScreen / sdl3DrawRedrawAll;
 * keeping a copy here so the download / returning-to-lobby paths can share
 * the same window-coordinate transform without disturbing those callers. */
static void sdl3BlitGameRTToWindow(void) {
  SDL_SetRenderTarget(gRenderer, NULL);

  int winW, winH;
  SDL_GetCurrentRenderOutputSize(gRenderer, &winW, &winH);

  float menuBarHeight = (float)MENU_BAR_HEIGHT;
  float availW = (float)winW;
  float availH = (float)winH - menuBarHeight;

  float gameAspect = (float)gGameRTWidth / (float)gGameRTHeight;
  float availAspect = availW / availH;
  float aspectDiff = gameAspect - availAspect;
  if (aspectDiff < 0) aspectDiff = -aspectDiff;

  float destW, destH, destX, destY;
  if (aspectDiff < 0.01f) {
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

  sdl3LetterboxFill();
  SDL_SetTextureBlendMode(gGameRenderTarget, SDL_BLENDMODE_NONE);
  SDL_RenderTexture(gRenderer, gGameRenderTarget, NULL, &gGameDestRect);
}

/* Body of the download / returning-to-lobby chrome draw. Runs against
 * whatever render target the caller has set up (either gGameRenderTarget
 * on the desktop resizable path or the window directly on the mobile /
 * fixed paths). */
static void sdl3DrawDownloadScreenContent(ClientSim *cs, bool justBlack) {
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
      BYTE total = clientSimGetBaseCount(cs);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusBase(i, clientSimGetBaseAlliance(cs, i), FALSE);
      }
    }
    sdl3DrawSetPillsStatusClear();
    {
      BYTE total = clientSimGetPillCount(cs);
      for (BYTE i = 1; i <= total; i++) {
        sdl3DrawStatusPillbox(i, clientSimGetPillAlliance(cs, i), FALSE);
      }
    }
    sdl3DrawSetTanksStatusClear();
    for (BYTE i = 1; i <= MAX_TANKS; i++) {
      sdl3DrawStatusTank(i, clientSimGetTankAlliance(cs, i));
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
    /* Fill the playfield from the top in proportion to the map transfer.
     * The old source was a raw scanline in the 272-pixel back buffer, which
     * is why this used to offset by a tile row before clamping; the transport
     * reports a percentage, so scale that against the playfield directly.
     * Only while the transport is actually receiving — the byte counters
     * behind the percentage survive a finished transfer and would otherwise
     * paint a full white playfield during the re-join after a map change. */
    int playfieldH = MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y;
    int visibleY = 0;
    if (clientSimGetConnectState(cs) == CLIENT_CONNECT_DOWNLOADING_MAP) {
      visibleY = playfieldH * (int)clientSimGetMapDownloadPercent(cs) / 100;
    }
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

void sdl3DrawDownloadScreen(ClientSim *cs, RECT *rcWindow, bool justBlack) {
  (void)rcWindow;
  if (!gRenderer) return;

  sdl3DrawAdaptRenderTarget();
  bool tabletMode = uiModeIsTablet();
  bool useRenderTarget = !tabletMode && gGameRenderTarget != NULL;

  if (useRenderTarget) {
    SDL_SetRenderTarget(gRenderer, gGameRenderTarget);
  }

  sdl3DrawDownloadScreenContent(cs, justBlack);

  if (useRenderTarget) {
    sdl3BlitGameRTToWindow();
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

void sdl3DrawReturningToLobby(ClientSim *cs) {
  if (!gRenderer) return;

  /* Full screen map: keep drawing the map and put the caption over it behind
     a dim. The classic path below draws the 15x15 chrome around a black
     playfield, which is a different screen entirely — taking it while the
     overview owns the window swapped the whole picture out from under the
     player for the last moments of a round, in a window that is still full
     screen. The map is what they have been looking at, so it stays. */
  if (gOverviewInWindow) {
    const char *caption = langGetText(STR_RETURNING_TO_LOBBY);
    int textW = 0;
    int textH = 0;

    sdl3DrawOverviewInWindowFrame(cs, gLastPillLabels, gLastBaseLabels);

    SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, OVERVIEW_LOBBY_DIM_ALPHA);
    SDL_RenderFillRect(gRenderer, NULL);

    if (gFontMsg && TTF_GetStringSize(gFontMsg, caption, 0, &textW, &textH)) {
      /* Centred on the map, which is the whole window bar the menu bar. The
         frame above sets the rect it blitted to; before it has blitted once
         there is nothing to centre on and the caption waits a frame. */
      if (gOverviewRect.w > 0.0f && gOverviewRect.h > 0.0f) {
        SDL_Color white = {200, 200, 200, 255};
        sdl3RenderText(gFontMsg, caption, white,
                       gOverviewRect.x + (gOverviewRect.w - (float)textW) * 0.5f,
                       gOverviewRect.y + (gOverviewRect.h - (float)textH) * 0.5f);
      }
    }
    return;
  }

  sdl3DrawAdaptRenderTarget();
  bool tabletMode = uiModeIsTablet();
  bool useRenderTarget = !tabletMode && gGameRenderTarget != NULL;

  if (useRenderTarget) {
    SDL_SetRenderTarget(gRenderer, gGameRenderTarget);
  }

  /* justBlack=true skips the download progress bar that was the source of
   * the white screen. */
  sdl3DrawDownloadScreenContent(cs, TRUE);

  /* Centred caption in the playfield. Tablet mode uses a different
   * playfield rect; only the desktop layout matters for this transition
   * (the mobile UIs don't expose a vote-to-lobby flow). */
  if (!tabletMode) {
    int zf = gZoomFactor;
    int originX = MAIN_OFFSET_X * zf;
    int originY = MAIN_OFFSET_Y * zf;
    int playfieldW = MAIN_SCREEN_SIZE_X * TILE_SIZE_X * zf;
    int playfieldH = MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y * zf;
    const char *caption = langGetText(STR_RETURNING_TO_LOBBY);
    int textW = 0, textH = 0;
    if (gFontMsg && TTF_GetStringSize(gFontMsg, caption, 0, &textW, &textH)) {
      float tx = (float)(originX + (playfieldW - textW) / 2);
      float ty = (float)(originY + (playfieldH - textH) / 2);
      SDL_Color white = {200, 200, 200, 255};
      sdl3RenderText(gFontMsg, caption, white, tx, ty);
    }
  }

  if (useRenderTarget) {
    sdl3BlitGameRTToWindow();
  }
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
  /* Cache-only: may run on the server-tick thread. Marks that there is no
     live man-status to show; the per-frame render pass will skip the
     rebuild and the blit is gated on gManStatusReady. */
  gManStatusValid = false;
  gManStatusReady = false;
}

void sdl3DrawSetManStatus(int x, int y, bool isDead, TURNTYPE angle) {
  /* Cache-only — may run on the server-tick thread, so it must not touch
     the renderer. sdl3RenderManStatusTex() rebuilds the texture from this
     cache on the render thread, once per frame via sdl3RenderStatusPanels. */
  (void)x; (void)y;
  gManStatusDead  = isDead;
  gManStatusAngle = angle;
  gManStatusValid = true;
}

/* Passes needed to lay a stroke of `stroke` destination pixels down as lines
   half a pixel apart, and where pass p sits across it. One pass at a stroke of
   a pixel or less, which is the single SDL_RenderLine the classic view has
   always drawn. */
static int manStatusStrokePasses(float stroke) {
  int passes = (int)SDL_ceilf((stroke - 1.0f) * 2.0f) + 1;
  return (passes < 1) ? 1 : passes;
}

static float manStatusStrokeOffset(float stroke, int passes, int p) {
  if (passes <= 1) return 0.0f;
  return -(stroke - 1.0f) * 0.5f +
         (stroke - 1.0f) * (float)p / (float)(passes - 1);
}

/* The LGM indicator itself: the circle, and the arrow inside it pointing at
   the man. Drawn straight into the current render target at whatever size is
   asked for — dstX/dstY are the top-left of the padded
   (MAN_STATUS_WIDTH+2) x (MAN_STATUS_HEIGHT+2) box in destination pixels, and
   `scale` takes source pixels to destination ones.

   Both the circle and the arrow are strokes rather than art, so the only way
   they come out clean is to draw them at the size they are shown at: a ring
   drawn at one scale and resampled to another loses the ring — bright where
   it landed on pixel centres and grey where it straddled two. The classic
   view draws at gZoomFactor into a texture it blits 1:1, and the full screen
   HUD at its own fit scale straight to the window; both come through here.
   (The tablet overlay makes the same move with its own ImGui drawing, off the
   same cached state.)

   `stroke` is the line width in destination pixels. The classic view passes
   one, which is the single SDL_RenderLine it has drawn at every zoom. */
static void sdl3DrawManStatusShape(float dstX, float dstY, float scale,
                                   float stroke, bool isDead, TURNTYPE angle) {
  /* Padding of one source pixel on every edge, so the outline never clips. */
  float scx = dstX + ((float)MAN_STATUS_CENTER_X + 1.0f) * scale;
  float scy = dstY + ((float)MAN_STATUS_CENTER_Y + 1.0f) * scale;
  float r   = ((float)MAN_STATUS_RADIUS - 1.0f) * scale;
  int   passes = manStatusStrokePasses(stroke);

  if (r < 1.0f) return;

  if (isDead) {
    /* Filled circle in red/orange using scan lines. */
    int top = (int)SDL_floorf(-r);
    int bot = (int)SDL_ceilf(r);
    SDL_SetRenderDrawColor(gRenderer, 200, 80, 0, 255);
    for (int dy = top; dy <= bot; dy++) {
      float span = r * r - (float)dy * (float)dy;
      if (span < 0.0f) continue;
      float dx = SDL_sqrtf(span);
      SDL_RenderLine(gRenderer, scx - dx, scy + (float)dy,
                     scx + dx, scy + (float)dy);
    }
    return;
  }

  /* Outline circle using parametric line segments, one loop per pass so a
     stroke wider than a pixel comes out solid rather than as separate
     circles. */
  SDL_SetRenderDrawColor(gRenderer, 255, 255, 255, 255);
  int steps = (int)(r * 16.0f);  /* ~4 steps per pixel of circumference */
  if (steps < 64) steps = 64;
  for (int p = 0; p < passes; p++) {
    float rr = r + manStatusStrokeOffset(stroke, passes, p);
    for (int i = 0; i < steps; i++) {
      double a1 = (RADIANS_MAX * i) / steps;
      double a2 = (RADIANS_MAX * (i + 1)) / steps;
      SDL_RenderLine(gRenderer,
                     (float)(scx + rr * cos(a1)), (float)(scy + rr * sin(a1)),
                     (float)(scx + rr * cos(a2)), (float)(scy + rr * sin(a2)));
    }
  }

  /* The arrow, from the centre out to the man's bearing. The four quadrant
     cases the Win32 code split this into are one formula — x = cx + r sin,
     y = cy - r cos — with the signs falling out of the trig, so it is written
     once here. Kept in floats to the end: rounding the tip to a source pixel
     first, as the old code did, snapped it in whole-pixel steps, and at the
     HUD's scale one source pixel is several on screen. */
  TURNTYPE a = angle + BRADIANS_SOUTH;
  if (a >= BRADIANS_MAX) a -= BRADIANS_MAX;
  double bearing = ((double)a / BRADIANS_MAX) * RADIANS_MAX;
  float tipX = scx + r * (float)sin(bearing);
  float tipY = scy - r * (float)cos(bearing);

  /* Thickness across the line rather than along it, so the passes lie side by
     side. */
  float dx = tipX - scx;
  float dy = tipY - scy;
  float len = SDL_sqrtf(dx * dx + dy * dy);
  float perpX = (len > 0.0f) ? (-dy / len) : 0.0f;
  float perpY = (len > 0.0f) ? ( dx / len) : 0.0f;
  for (int p = 0; p < passes; p++) {
    float off = manStatusStrokeOffset(stroke, passes, p);
    SDL_RenderLine(gRenderer, scx + perpX * off, scy + perpY * off,
                   tipX + perpX * off, tipY + perpY * off);
  }
}

/* Render thread only: rebuild the man-status texture from the cached
   (dead, angle) values, at the zoom factor the classic chrome is drawn at.
   The texture is (MAN_STATUS_WIDTH+2)*(MAN_STATUS_HEIGHT+2)*zf and is blitted
   1:1, so the shape lands on the screen at exactly the size it was drawn. */
static void sdl3RenderManStatusTex(void) {
  SDL_Texture *prevTarget;
  SDL_assert(sdl3DrawOnRenderThread());
  if (!gRenderer || !gManStatusTex) return;

  /* Save/restore the caller's target: this runs mid-frame from
     sdl3RenderStatusPanels, where the active target may be the game
     render-to-texture, not the screen. */
  prevTarget = SDL_GetRenderTarget(gRenderer);

  SDL_SetRenderTarget(gRenderer, gManStatusTex);
  SDL_SetTextureBlendMode(gManStatusTex, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  sdl3DrawManStatusShape(0.0f, 0.0f, (float)gZoomFactor, 1.0f,
                         gManStatusDead, gManStatusAngle);

  SDL_SetRenderTarget(gRenderer, prevTarget);
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

/* The one place the three view names are spelled. The classic corner label
   below and the full screen map's caption both come through here, so they
   cannot end up calling the same view different things. */
bool sdl3DrawGetItemViewLabel(ClientSim *cs, char *out, size_t outLen) {
  if (cs == NULL || out == NULL || outLen == 0) return false;
  switch (clientSimGetViewKind(cs)) {
    case VIEW_KIND_PILL:
      snprintf(out, outLen, "Pillbox View");
      return true;
    case VIEW_KIND_BASE:
      snprintf(out, outLen, "Base View");
      return true;
    case VIEW_KIND_ALLY: {
      /* Name the ally we are riding along with. The player mirror is empty
         for a slot we have no name for yet; then just say what the view is. */
      const char *name = sdl3ImguiGetPlayerName(clientSimGetViewTarget(cs));
      if (name[0] != '\0') {
        snprintf(out, outLen, "Allied Tank View \xE2\x80\x94 %s", name);
      } else {
        snprintf(out, outLen, "Allied Tank View");
      }
      return true;
    }
    default:
      return false;
  }
}

void sdl3DrawItemInView(ClientSim *cs) {
  char label[128];
  if (!gRenderer || !sdl3DrawGetItemViewLabel(cs, label, sizeof(label))) return;
  SDL_Color white = {200, 200, 200, 255};
  int originX = MAIN_OFFSET_X * gZoomFactor;
  int originY = MAIN_OFFSET_Y * gZoomFactor;
  int fontH = 13 * gZoomFactor;
  float tx = (float)(originX + 2 * gZoomFactor);
  float ty = (float)(originY + MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y * gZoomFactor - fontH - 2 * gZoomFactor);
  sdl3RenderText(gFontMsg, label, white, tx, ty);
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
    BYTE total = clientSimGetBaseCount(cs);
    for (BYTE i = 1; i <= total; i++) {
      sdl3DrawStatusBase(i, clientSimGetBaseAlliance(cs, i), FALSE);
    }
  }
  sdl3DrawSetPillsStatusClear();
  {
    BYTE total = clientSimGetPillCount(cs);
    for (BYTE i = 1; i <= total; i++) {
      sdl3DrawStatusPillbox(i, clientSimGetPillAlliance(cs, i), FALSE);
    }
  }
  sdl3DrawSetTanksStatusClear();
  for (BYTE i = 1; i <= MAX_TANKS; i++) {
    sdl3DrawStatusTank(i, clientSimGetTankAlliance(cs, i));
  }
}
