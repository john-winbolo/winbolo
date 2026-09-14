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
 * Name:          game_view
 * Filename:      game_view.c
 * Purpose:
 *   Phase D of plans/ctrailer.md — drives the live game's
 *   render path (mapview.c + sdl3draw_status.c) from the
 *   standalone LogViewer's replayed state. Output is
 *   pixel-identical to live-game footage of the same map.
 *
 *   This translation unit sees the bolo-side type names
 *   (screen, screenTanks, ...) via mapview.h. It must NOT
 *   include any logviewer / *.h that pulls in backend.h —
 *   the two define the same struct names with different
 *   layouts. State the file needs from LogViewerState
 *   reaches it via a small set of accessor functions
 *   declared `extern` below; the pointers they return are
 *   ABI-safe because mapview.c only goes through accessors
 *   (routed via bolo_shim.c) and never indexes the structs
 *   directly.
 *********************************************************/

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <math.h>
#include <stdio.h>

#include "global.h"
#include "../gui/sdl3/mapview.h"
#include "../gui/sdl3/sdl3draw_status.h"
#include "../gui/sdl3/sdl_bmp.h"
#include "../gui/positions.h"
#include "../gui/tiles.h"
#include "../gui/ping_kinds.h"
#include "../gui/sdl3/ping_icons.h"   /* pingIconsInit -- the replay shares the game's icons */
#include "../gui/sdl3/ping_marker.h"  /* pingMarkerDraw */

#include "game_view.h"

/* Game-view window logical size (pre-zoom). Same constants the live game's
 * sdl3draw.c uses; duplicated here to avoid pulling in sdl3draw.h's heavier
 * client_sim transitive deps. */
#define GV_SCREEN_W 515
#define GV_SCREEN_H 325

/* --- Logviewer-side accessors. Declared extern (no logviewer header
 *     include) so this TU stays on bolo types. --- */
extern bool lv_screenGetPing(int index, unsigned char *sender,
                             unsigned char *kind, uint16_t *worldX,
                             uint16_t *worldY, uint32_t *ageMs);
extern int  lv_screenGetPingCapacity(void);

/* The names under the replay's ping markers, in a cache of their own rather
 * than the classic label pass's: the shared drawer in tank_label.c keys on
 * the player slot, and this one holds the bare name for a slot while that one
 * holds the full "name@location" label for it. Flushed with the view's other
 * textures in lv_drawGameViewTeardown. */
static TankLabelCache s_pingNameCache;

extern void *lv_gameViewGetScreen(void);
extern void *lv_gameViewGetMineView(void);
extern void *lv_gameViewGetBases(void);
extern void *lv_gameViewGetPills(void);
extern BYTE  lv_gameViewGetCameraSlot(void);
extern bool  lv_gameViewIsHudAlive(BYTE slot);
extern uint16_t lv_gameViewGetKills(BYTE slot);
extern uint16_t lv_gameViewGetDeaths(BYTE slot);
extern void  lv_gameViewGetInventory(BYTE slot, BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees);
extern void  lv_gameViewGetTankFulls(BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees);
extern void  lv_gameViewGetBaseFulls(BYTE *shells, BYTE *mines, BYTE *armour);

extern SDL_Window   *lv_drawGetSDLWindow(void);
extern SDL_Renderer *lv_drawGetSDLRenderer(void);
extern SDL_Texture  *lv_drawGetTilesTexture(void);
extern int           lv_drawGetSheetScale(void);

extern bool  lv_playersIsInUse(BYTE playerNumber);
extern void  lv_playersGetTankDetails(BYTE playerNumber, BYTE *mx, BYTE *my,
                                      BYTE *px, BYTE *py, BYTE *frame,
                                      bool *onBoat);
extern void  lv_playersGetLgmDetails(BYTE playerNumber, BYTE *mx, BYTE *my,
                                     BYTE *px, BYTE *py, BYTE *frame);
extern void  lv_playersGetLgmStatus(BYTE playerNumber, bool *isOut, bool *isDead);
extern void  lv_playersGetPlayerName(BYTE playerNum, char *dest, size_t destSize);
extern bool         lv_playersIsBot(BYTE playerNumber);
extern tankAlliance lv_playersScreenAllience(BYTE playerNum);

extern BYTE         lv_basesGetNumBases(bases *value);
extern baseAlliance lv_basesGetAlliancePos(bases *value, BYTE x, BYTE y);
extern void         lv_basesGetBase(bases *value, base *item, BYTE baseNum);
extern bool         lv_basesIsActive(bases *value, BYTE baseNum);
extern BYTE         lv_pillsGetNumPills(pillboxes *value);
extern pillAlliance lv_pillsGetAllianceNum(pillboxes *value, BYTE pillNum);
extern bool         lv_pillsIsActive(pillboxes *value, BYTE pillNum);

extern int          lv_imgui_events_get_count(void);
extern const char  *lv_imgui_events_get_text(int i);

/* Scrolling-marquee API. Declared inline (no #include "messages.h") so
 * this TU stays clear of logviewer/global.h which conflicts with the
 * bolo-side struct layouts mapview.h pulls in. MESSAGE_WIDTH mirrors
 * messages.h's value (68) — the live game uses the same constant. */
#define GV_MESSAGE_WIDTH 68
extern void lv_messageCreate(void);
extern void lv_messageDestroy(void);
extern void lv_messageUpdate(void);
extern void lv_messageGetMessage(char *top, char *bottom);

/* logviewer's screen-size setters (parallel to logviewer/screen.c). The
 * trailer view needs the screen buffer sized to MAIN_BACK_BUFFER_SIZE so
 * mapView's 17×17 read pattern is fully populated and the existing
 * lv_imgui_game_view_update_camera math (mx - screenSizeX/2) centres the
 * camera tank in the visible 15-tile-wide main view. */
extern void lv_screenSetSizeX(BYTE x);
extern void lv_screenSetSizeY(BYTE y);
extern BYTE lv_screenGetSizeX(void);
extern BYTE lv_screenGetSizeY(void);

/* Defined in logviewer/screen.c — viewport tile origin (top-left of
 * the visible 16-tile window into the 256-tile map). Not declared in
 * backend.h; extern'd here to avoid pulling in the logviewer's
 * conflicting backend.h types. Used to convert tank map-space coords
 * to viewport-relative screen-space for sdl3DrawTankLabel. */
extern BYTE lv_screenGetXOffset(void);
extern BYTE lv_screenGetYOffset(void);

/* Sub-tile pan within (xOffset,yOffset), in unzoomed pixels [0,16).
 * Drives edgeX/edgeY so the camera moves at pixel granularity rather
 * than snapping by tiles. The dead-zone autoscroll and arrow-key pan
 * both update this via lv_screenPanToTotalPixels. */
extern void lv_screenGetSubOffset(int *x, int *y);

/* --- Module statics ----------------------------------------------- */

/* Mirrors live-game openInGameFonts: 4 primary + 4 SlabK fallback. */
#define GV_NUM_FONTS 8

static SDL_Texture *s_backgroundTex = NULL;
static SDL_Texture *s_tankBarsTex   = NULL;
static SDL_Texture *s_baseBarsTex   = NULL;
/* Camera tank's LGM status circle. Owned here (not in sdl3draw_status)
 * so the live game's gManStatusTex remains untouched. Mirrors
 * sdl3draw.c:1885 sdl3DrawSetManStatus drawing exactly. */
static SDL_Texture *s_manStatusTex  = NULL;
static bool         s_manStatusReady = false;
static TTF_Font    *s_fonts[GV_NUM_FONTS] = { NULL };
/* s_zoom is the magnification the frame is drawn at, in framebuffer pixels;
   s_displayZoom is the 1..4 the user picked, in window points. They differ by
   the window's pixel density — the window is sized in points, everything drawn
   into it is sized in pixels. Same split sdl3DrawSetup makes for tablet. */
static int          s_zoom          = 0;
static int          s_displayZoom   = 0;
static int          s_savedWindowW  = 0;
static int          s_savedWindowH  = 0;
static BYTE         s_savedScreenSizeX = 0;
static BYTE         s_savedScreenSizeY = 0;
static bool         s_didTtfInit    = false;

/* Scrolling-marquee tick. The live game advances the marquee every
 * MESSAGE_SCROLL_TIME (4) display ticks. Display ticks run every OTHER
 * GAME_TICK_LENGTH iteration of the live client's main loop (the keys/game
 * alternation in client_frontend_tick.c — only the "game tick" branch
 * calls clientSimDisplayTick), so they fire every 20ms wall, not 10ms.
 * That gives one column shift per 4 × 20ms = 80ms wall-clock at 1×
 * speed. Anchored to SDL_GetTicks (NOT timeRunning) so fast-forward /
 * rewind / pause don't change scroll speed; only gameplay events fly
 * by faster. Reset on setup. */
#define GV_MESSAGE_TICK_MS 80
static uint32_t     s_lastMessageTickMs = 0;

/* Death-static effect — Bolo-style pixel noise drawn over the main view
 * while the camera tank is dead (gameViewHud[camera].alive == false).
 * Algorithm replicated from sdl3draw.c (live game's gStatic* state at
 * sdl3draw.c:136-148 + render block at :1315-1367). The live game keys
 * the throttle on tankGetDeathWait (per-tick countdown sourced from the
 * deathcause); the log doesn't carry the deathcause, so we tick on
 * SDL_GetTicks/GAME_TICK_LENGTH instead — same 10ms wall-clock cadence,
 * just driven externally. */
static SDL_Texture *s_deathStaticTex      = NULL;
static int          s_deathStaticTexW     = 0;
static int          s_deathStaticTexH     = 0;
static uint32_t     s_deathStaticSeed     = 1;
static uint32_t     s_deathStaticLastTick = 0;

/* mapView's tile loop iterates MAIN_BACK_BUFFER_SIZE_X × MAIN_BACK_BUFFER_SIZE_Y
 * (17×17, defined as MAIN_SCREEN_SIZE + 2 in bolo/screen.h). To populate
 * exactly that footprint inside the logviewer's pointer-style screen buffer
 * we want screenSizeX == screenSizeY == 16 (the loop in
 * lv_screenUpdateView covers count=0..ssx, so ssx=16 fills the 17 columns
 * mapView reads). */
#define GV_SCREEN_TILES 16

/* --- Font load (mirrors sdl3draw.c openInGameFonts) ----------------
 *
 * Standalone LogViewer links gamefront_stubs.c, whose
 * gameFrontGetLanguageCode returns an empty string. sarasaMonoFontPath
 * therefore falls through to data/fonts/SarasaMonoSlabJ-Regular.ttf —
 * the trailer build is English-only, which is fine.
 *
 * We don't share openInGameFonts with the live game because that
 * function mutates sdl3draw.c's own gFont* statics, which the
 * standalone path doesn't (and shouldn't) own. Instead we open the
 * fonts here and push them into sdl3draw_status's setter. */
static const char *gv_sarasaMonoFontPath(const char *langCode) {
  if (langCode && *langCode) {
    if (SDL_strcasecmp(langCode, "ko")    == 0) return "data/fonts/SarasaMonoSlabK-Regular.ttf";
    if (SDL_strcasecmp(langCode, "zh-CN") == 0) return "data/fonts/SarasaMonoSlabSC-Regular.ttf";
    if (SDL_strcasecmp(langCode, "zh-TW") == 0) return "data/fonts/SarasaMonoSlabTC-Regular.ttf";
  }
  return "data/fonts/SarasaMonoSlabJ-Regular.ttf";
}

static void gv_loadInGameFonts(int zoomFactor) {
  /* Standalone LogViewer never sets a language code, so this resolves to
   * SlabJ. We still call gv_sarasaMonoFontPath so the fallback chain is
   * structurally identical to the live game. */
  const char *sarasaRel  = gv_sarasaMonoFontPath("");
  const char *sarasaKRel = "data/fonts/SarasaMonoSlabK-Regular.ttf";

  static char sarasaBuf[1024];
  static char sarasaKBuf[1024];
  const char *base = SDL_GetBasePath();
  if (base) {
    SDL_snprintf(sarasaBuf,  sizeof(sarasaBuf),  "%s%s", base, sarasaRel);
    SDL_snprintf(sarasaKBuf, sizeof(sarasaKBuf), "%s%s", base, sarasaKRel);
  } else {
    SDL_snprintf(sarasaBuf,  sizeof(sarasaBuf),  "%s", sarasaRel);
    SDL_snprintf(sarasaKBuf, sizeof(sarasaKBuf), "%s", sarasaKRel);
  }

  /* Same size table as openInGameFonts: msg=13, KD=13, tiny=8, label=10. */
  s_fonts[0] = TTF_OpenFont(sarasaBuf,  8 * zoomFactor); /* tiny  */
  s_fonts[1] = TTF_OpenFont(sarasaBuf, 13 * zoomFactor); /* msg   */
  s_fonts[2] = TTF_OpenFont(sarasaBuf, 13 * zoomFactor); /* kd    */
  s_fonts[3] = TTF_OpenFont(sarasaBuf, 10 * zoomFactor); /* label */

  /* Hangul fallback — skip if primary IS SlabK. */
  if (SDL_strcmp(sarasaBuf, sarasaKBuf) != 0) {
    s_fonts[4] = TTF_OpenFont(sarasaKBuf,  8 * zoomFactor);
    s_fonts[5] = TTF_OpenFont(sarasaKBuf, 13 * zoomFactor);
    s_fonts[6] = TTF_OpenFont(sarasaKBuf, 13 * zoomFactor);
    s_fonts[7] = TTF_OpenFont(sarasaKBuf, 10 * zoomFactor);
    if (s_fonts[0] && s_fonts[4]) TTF_AddFallbackFont(s_fonts[0], s_fonts[4]);
    if (s_fonts[1] && s_fonts[5]) TTF_AddFallbackFont(s_fonts[1], s_fonts[5]);
    if (s_fonts[2] && s_fonts[6]) TTF_AddFallbackFont(s_fonts[2], s_fonts[6]);
    if (s_fonts[3] && s_fonts[7]) TTF_AddFallbackFont(s_fonts[3], s_fonts[7]);
  }

  if (!s_fonts[1]) {
    fprintf(stderr, "lv_drawGameViewSetup: failed to open primary font %s\n", sarasaBuf);
  }
}

static void gv_closeInGameFonts(void) {
  for (int i = 0; i < GV_NUM_FONTS; i++) {
    if (s_fonts[i]) {
      TTF_CloseFont(s_fonts[i]);
      s_fonts[i] = NULL;
    }
  }
}

static SDL_Texture *gv_loadBackground(SDL_Renderer *renderer) {
  /* Same path as live-game sdl3LoadBackground. SDL_GetBasePath prefix
   * isn't strictly needed (sdlLoadBmpAsTexture handles cwd-relative
   * paths) but try the prefixed path first to match live behaviour. */
  SDL_Texture *tex = NULL;
  const char *base = SDL_GetBasePath();
  if (base) {
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%sdata/background.bmp", base);
    tex = sdlLoadBmpAsTexture(renderer, path);
  }
  if (!tex) {
    tex = sdlLoadBmpAsTexture(renderer, "data/background.bmp");
  }
  if (tex) {
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  } else {
    fprintf(stderr, "lv_drawGameViewSetup: data/background.bmp not loaded\n");
  }
  return tex;
}

/* xorshift32 PRNG matching sdl3draw.c's getRandomStaticNoiseSeed. Seeded
 * non-zero; one shared sequence between the live game's gStaticSeed and
 * our s_deathStaticSeed is unnecessary — they're independent visual
 * effects in different windows. */
static uint32_t gv_nextStaticNoise(void) {
  s_deathStaticSeed ^= s_deathStaticSeed << 13;
  s_deathStaticSeed ^= s_deathStaticSeed >> 17;
  s_deathStaticSeed ^= s_deathStaticSeed << 5;
  return s_deathStaticSeed;
}

/* Pixel-noise "TV static" overlay covering the main view rectangle.
 * Mirrors sdl3draw.c:1328-1367 — recreate texture on size change, lock,
 * scatter (w*h/3) random white/black pixels each tick, unlock, blit.
 * `tick` is monotonic; the texture is updated only when it changes,
 * matching the live game's tankGetDeathWait throttle. */
static void gv_drawDeathStatic(SDL_Renderer *renderer, int originX, int originY,
                               int gameW, int gameH, uint32_t tick) {
  if (!renderer || gameW <= 0 || gameH <= 0) return;

  const int staticW = gameW;
  const int staticH = gameH;

  if (!s_deathStaticTex || s_deathStaticTexW != gameW || s_deathStaticTexH != gameH) {
    if (s_deathStaticTex) SDL_DestroyTexture(s_deathStaticTex);
    s_deathStaticTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         staticW, staticH);
    if (s_deathStaticTex) {
      SDL_SetTextureScaleMode(s_deathStaticTex, SDL_SCALEMODE_NEAREST);
    }
    s_deathStaticTexW = gameW;
    s_deathStaticTexH = gameH;
    s_deathStaticLastTick = 0;
  }
  if (!s_deathStaticTex) return;

  if (tick != s_deathStaticLastTick) {
    bool firstTick = (s_deathStaticLastTick == 0);
    s_deathStaticLastTick = tick;
    uint32_t *pixels;
    int pitch;
    if (SDL_LockTexture(s_deathStaticTex, NULL, (void **)&pixels, &pitch)) {
      const int rowLen = pitch / 4;
      if (firstTick) {
        for (int y = 0; y < staticH; y++) {
          for (int x = 0; x < staticW; x++) {
            pixels[y * rowLen + x] = 0xFF000000u;
          }
        }
      }
      const int numPoints = staticW * staticH / 3;
      int col = 0;
      const uint32_t white = 0xFFFFFFFFu;
      const uint32_t black = 0xFF000000u;
      for (int i = 0; i < numPoints; i++) {
        uint32_t rx = gv_nextStaticNoise();
        uint32_t ry = gv_nextStaticNoise();
        int px = (int)(rx % (uint32_t)staticW);
        int py = (int)(ry % (uint32_t)staticH);
        pixels[py * rowLen + px] = (col++ & 1) ? white : black;
      }
      SDL_UnlockTexture(s_deathStaticTex);
    }
  }

  SDL_FRect dest = { (float)originX, (float)originY, (float)gameW, (float)gameH };
  SDL_RenderTexture(renderer, s_deathStaticTex, NULL, &dest);
}

/* sdl3DrawStatusTankBars / sdl3DrawStatusBaseBars early-return when the
 * bar render-target textures are NULL, so we have to allocate real ones
 * to get visible bars. Mirrors sdl3draw.c:963-964 sizes. */
static SDL_Texture *gv_createBarTarget(SDL_Renderer *renderer, int w, int h) {
  SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_TARGET, w, h);
  if (t) {
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
  }
  return t;
}

/* Local copy of bolo/util.c utilCalcAngle. The LogViewer links its own
 * logviewer/util.c instead of bolo/util.c, so the symbol isn't
 * available. Self-contained math; no shared state. */
static TURNTYPE gv_calcAngle(WORLD object1X, WORLD object1Y, WORLD object2X, WORLD object2Y) {
  TURNTYPE returnValue;
  double angle;
  double gapX, gapY;

  if (object2X - object1X < 0) {
    gapX = object1X - object2X;
  } else {
    gapX = object2X - object1X;
  }
  if (object2Y - object1Y < 0) {
    gapY = object1Y - object2Y;
  } else {
    gapY = object2Y - object1Y;
  }

  angle = atan((gapX / gapY));
  angle = (angle / RADIANS_MAX) * DEGREES_MAX;
  returnValue = (TURNTYPE)((BRADIANS_MAX / DEGREES_MAX) * angle);

  if (object2X - object1X <= 0 && object2Y - object1Y <= 0) {
    returnValue = (TURNTYPE)(BRADIANS_MAX - returnValue);
  } else if (object2X - object1X > 0 && object2Y - object1Y >= 0) {
    returnValue = (TURNTYPE)(BRADIANS_SOUTH - returnValue);
  } else if (object2X - object1X <= 0 && object2Y - object1Y >= 0) {
    returnValue += BRADIANS_SOUTH;
  }
  return returnValue;
}

static void gv_setManClear(SDL_Renderer *renderer) {
  s_manStatusReady = false;
  if (!renderer || !s_manStatusTex) return;
  SDL_SetRenderTarget(renderer, s_manStatusTex);
  SDL_SetTextureBlendMode(s_manStatusTex, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderFillRect(renderer, NULL);
  SDL_SetRenderTarget(renderer, NULL);
}

/* Mirrors sdl3draw.c:1885 sdl3DrawSetManStatus. The circle/arrow math
 * is duplicated rather than shared so this TU stays independent of the
 * live game's gManStatusTex globals. */
static void gv_setManStatus(SDL_Renderer *renderer, bool isDead, TURNTYPE angle) {
  if (!renderer || !s_manStatusTex) return;

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

  int zf = s_zoom;
  int scx = (MAN_STATUS_CENTER_X + 1) * zf;
  int scy = (MAN_STATUS_CENTER_Y + 1) * zf;
  int r   = (MAN_STATUS_RADIUS - 1) * zf;
  int sAddX = addX * zf;
  int sAddY = addY * zf;

  SDL_SetRenderTarget(renderer, s_manStatusTex);
  SDL_SetTextureBlendMode(s_manStatusTex, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderFillRect(renderer, NULL);

  if (isDead) {
    SDL_SetRenderDrawColor(renderer, 200, 80, 0, 255);
    for (int dy = -r; dy <= r; dy++) {
      int dx = (int)sqrtf((float)(r * r - dy * dy));
      SDL_RenderLine(renderer,
                     (float)(scx - dx), (float)(scy + dy),
                     (float)(scx + dx), (float)(scy + dy));
    }
  } else {
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    int steps = 4 * r * 4;
    if (steps < 64) steps = 64;
    for (int i = 0; i < steps; i++) {
      double a1 = (RADIANS_MAX * i) / steps;
      double a2 = (RADIANS_MAX * (i + 1)) / steps;
      SDL_RenderLine(renderer,
                     (float)(scx + r * cos(a1)), (float)(scy + r * sin(a1)),
                     (float)(scx + r * cos(a2)), (float)(scy + r * sin(a2)));
    }
    SDL_RenderLine(renderer, (float)scx, (float)scy, (float)sAddX, (float)sAddY);
  }

  SDL_SetRenderTarget(renderer, NULL);
  s_manStatusReady = true;
}

/* --- Public API --------------------------------------------------- */

/* The zoom the user picked, not the one the frame is drawn at — logviewer.c
   compares this against the 1..4 number bound to the number keys. */
int lv_drawGameViewGetZoom(void) {
  return s_displayZoom;
}

void lv_drawGameViewSetup(int zoomFactor) {
  SDL_Window   *window   = lv_drawGetSDLWindow();
  SDL_Renderer *renderer = lv_drawGetSDLRenderer();
  if (!window || !renderer) return;

  /* Fold the pixel density into the magnification so the view keeps the
     physical size the chosen zoom implies and gains pixels instead of being
     upscaled by the compositor. Fonts and the status textures below are built
     at this same number, so their glyphs rasterize crisply rather than being
     magnified from a 1x render. */
  float density = SDL_GetWindowPixelDensity(window);
  int   dz      = (int)(density + 0.5f);
  if (dz < 1) dz = 1;

  /* A full screen or maximized window is not ours to resize, so the two
     halves of the normal path are both wrong there: fit the zoom to the
     window instead of the window to the zoom, and leave the saved size
     unwritten — it would be the display's size, not the size the player
     had, and teardown would stretch the window to the whole display on the
     way back to a normal window. Teardown skips the restore when the saved
     size is 0. */
  SDL_WindowFlags wflags = SDL_GetWindowFlags(window);
  bool windowFixed = (wflags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) != 0;
  if (windowFixed) {
    int winW = 0, winH = 0;
    SDL_GetWindowSize(window, &winW, &winH);
    int fit  = winW / GV_SCREEN_W;
    int fitH = winH / GV_SCREEN_H;
    if (fitH < fit) fit = fitH;
    if (fit < 1) fit = 1;
    if (zoomFactor > fit) zoomFactor = fit;
  }

  s_displayZoom = zoomFactor;
  s_zoom        = zoomFactor * dz;
  zoomFactor    = s_zoom;   /* everything below this point is in pixels */

  if (!windowFixed) {
    SDL_GetWindowSize(window, &s_savedWindowW, &s_savedWindowH);
    /* The window is sized in points, so it takes the display zoom. */
    SDL_SetWindowSize(window, GV_SCREEN_W * s_displayZoom,
                              GV_SCREEN_H * s_displayZoom);
  }

  /* Resize the logviewer's screen buffer to match mapView's read
   * pattern. Saved here and restored on teardown so the normal
   * logviewer UI returns to its previous tile count. */
  s_savedScreenSizeX = lv_screenGetSizeX();
  s_savedScreenSizeY = lv_screenGetSizeY();
  lv_screenSetSizeX(GV_SCREEN_TILES);
  lv_screenSetSizeY(GV_SCREEN_TILES);

  if (!TTF_WasInit()) {
    s_didTtfInit = TTF_Init();
  } else {
    s_didTtfInit = false;
  }

  gv_loadInGameFonts(zoomFactor);
  s_backgroundTex = gv_loadBackground(renderer);
  s_tankBarsTex   = gv_createBarTarget(renderer,
                                       STATUS_TANK_BARS_TOTALWIDTH,
                                       STATUS_TANK_BARS_HEIGHT);
  s_baseBarsTex   = gv_createBarTarget(renderer,
                                       STATUS_BASE_BARS_MAX_WIDTH,
                                       STATUS_BASE_BARS_TOTALHEIGHT);
  /* +2 for the 1px outline padding live-game uses (sdl3draw.c:961). */
  s_manStatusTex  = gv_createBarTarget(renderer,
                                       (MAN_STATUS_WIDTH  + 2) * zoomFactor,
                                       (MAN_STATUS_HEIGHT + 2) * zoomFactor);
  s_manStatusReady = false;

  sdl3DrawStatusInit(renderer,
                     lv_drawGetTilesTexture(),
                     lv_drawGetSheetScale(),
                     zoomFactor,
                     /* tiny  */ s_fonts[0],
                     /* msg   */ s_fonts[1],
                     /* kd    */ s_fonts[2],
                     /* label */ s_fonts[3],
                     /* fallbacks */
                     s_fonts[4], s_fonts[5], s_fonts[6], s_fonts[7],
                     s_tankBarsTex,
                     s_baseBarsTex);

  /* No edge offset — replay never does sub-tile drag-pan. */
  sdl3DrawStatusSetEdgeOffset(0, 0);

  /* Scrolling-marquee init. screen.c has been feeding lv_messageAddItem
   * for the whole replay, but the queue had no consumer; drain it first
   * (lv_messageCreate just resets the head pointer and would leak the
   * previously-accumulated nodes), then rebuild the visible-cells
   * buffer fresh. Older events are dropped — option (a) backfill, see
   * note in lv_drawGameViewFrame's step 12. */
  lv_messageDestroy();
  lv_messageCreate();
  s_lastMessageTickMs = SDL_GetTicks();
}

void lv_drawGameViewTeardown(void) {
  SDL_Window *window = lv_drawGetSDLWindow();

  /* Drain any queued marquee items so they don't accumulate forever
   * between game-view sessions. screen.c keeps appending in any mode. */
  lv_messageDestroy();

  sdl3DrawStatusShutdown();
  tankLabelCacheFlush(&s_pingNameCache);

  if (s_tankBarsTex) { SDL_DestroyTexture(s_tankBarsTex); s_tankBarsTex = NULL; }
  if (s_baseBarsTex) { SDL_DestroyTexture(s_baseBarsTex); s_baseBarsTex = NULL; }
  if (s_manStatusTex) { SDL_DestroyTexture(s_manStatusTex); s_manStatusTex = NULL; }
  s_manStatusReady = false;
  if (s_backgroundTex) { SDL_DestroyTexture(s_backgroundTex); s_backgroundTex = NULL; }
  if (s_deathStaticTex) { SDL_DestroyTexture(s_deathStaticTex); s_deathStaticTex = NULL; }
  s_deathStaticTexW = 0;
  s_deathStaticTexH = 0;
  s_deathStaticSeed = 1;
  s_deathStaticLastTick = 0;

  gv_closeInGameFonts();

  if (s_didTtfInit) {
    TTF_Quit();
    s_didTtfInit = false;
  }

  if (s_savedScreenSizeX > 0) lv_screenSetSizeX(s_savedScreenSizeX);
  if (s_savedScreenSizeY > 0) lv_screenSetSizeY(s_savedScreenSizeY);
  s_savedScreenSizeX = 0;
  s_savedScreenSizeY = 0;

  if (window && s_savedWindowW > 0 && s_savedWindowH > 0) {
    SDL_SetWindowSize(window, s_savedWindowW, s_savedWindowH);
  }
  s_savedWindowW = 0;
  s_savedWindowH = 0;
  s_zoom = 0;
  s_displayZoom = 0;
}

/* --- Smart-ping markers ------------------------------------------
 * The replay's half of the live game's ping pass: the same square, icon and
 * pulse (ping_marker.c) over the same ground for the same five seconds,
 * drawn where the live frame draws them -- after the terrain and before the
 * sprites, so the tanks, shells and builders a ping points at stay on top.
 *
 * No edge markers here. Those point from the viewer's own tank towards a ping
 * they cannot see, and a replay's camera is not a player: whose tank the
 * arrow would start from is not a question the recording answers. */
static void gv_drawPings(SDL_Renderer *renderer,
                         int originX, int originY, int tileW, int tileH,
                         int edgeX, int edgeY) {
  int cap = lv_screenGetPingCapacity();
  int i;

  pingIconsInit(renderer);

  for (i = 0; i < cap; i++) {
    unsigned char sender = 0;
    unsigned char kind = 0;
    uint16_t wx = 0, wy = 0;
    uint32_t ageMs = 0;
    float alpha, cx, cy;
    char senderName[PLAYER_NAME_LEN];
    PingMarkerLabel label;

    if (!lv_screenGetPing(i, &sender, &kind, &wx, &wy, &ageMs)) continue;
    /* Expiry only; pingMarkerDraw does the blink, so a recording shows what
       the player was shown. */
    alpha = pingDisplayAlpha((int)ageMs);
    if (alpha <= 0.0f) continue;

    /* Who sent it, from the recording's own player table — the live game
     * carries the name on the ping record instead, but a replay has the
     * whole table and can simply look the slot up. */
    senderName[0] = '\0';
    lv_playersGetPlayerName(sender, senderName, sizeof(senderName));
    label.cache = &s_pingNameCache;
    label.font  = sdl3DrawGetMessageFont();
    label.name  = senderName;
    label.slot  = sender;
    label.scale = 1.0f;

    /* WORLD units are 256 to a map square; the marker names the square the
     * ping landed in, so anchor on that square's centre. The view shows
     * squares xOffset+1 .. xOffset+MAIN_SCREEN_SIZE_X and pans sub-square by
     * edgeX/edgeY, the same arithmetic mapViewDrawTanks uses for a sprite. */
    cx = (float)originX
       + ((float)(wx >> 8) + 0.5f - (float)(lv_screenGetXOffset() + 1)) * (float)tileW
       - (float)edgeX;
    cy = (float)originY
       + ((float)(wy >> 8) + 0.5f - (float)(lv_screenGetYOffset() + 1)) * (float)tileH
       - (float)edgeY;

    /* The world marker, so with the arrival ring: it runs off the ping's age
       out of the recording, which is the same number the live game fed it, so
       the replay shows the ring the player was shown. */
    pingMarkerDraw(renderer, kind, cx, cy, (float)tileW, (float)tileH,
                   ageMs, &label);
  }
}

/* --- Frame assembly (mirrors sdl3DrawMainScreen step-for-step) ---- */

void lv_drawGameViewFrame(void *screenView, void *mineView,
                          void *tanks, void *bullets, void *lgms) {
  SDL_Renderer *renderer = lv_drawGetSDLRenderer();
  if (!renderer || s_zoom <= 0) return;

  screen        *view    = (screen *)screenView;
  screenMines   *mines   = (screenMines *)mineView;
  screenTanks   *tks     = (screenTanks *)tanks;
  screenBullets *shells  = (screenBullets *)bullets;
  screenLgm     *lgmList = (screenLgm *)lgms;

  const int zf       = s_zoom;
  const int originX  = MAIN_OFFSET_X * zf;
  const int originY  = MAIN_OFFSET_Y * zf;
  const int tileW    = TILE_SIZE_X * zf;
  const int tileH    = TILE_SIZE_Y * zf;
  const BYTE camera  = lv_gameViewGetCameraSlot();

  /* Sub-tile camera pan from the viewport's pixel-precise position.
   * lv_imgui_game_view_update_camera maintains (xOffset,yOffset) in
   * whole tiles and (subPxX,subPxY) in sub-tile pixels. Deriving edgeX
   * from subPxX (rather than the camera tank's camPx) decouples the
   * world shift from the tank's intra-tile motion: in the dead-zone
   * safe area the camera sits still and the tank slides on screen,
   * and at tile boundaries there is no 16-px snap because the camera
   * never tracked camPx in the first place. */
  int subX = 0, subY = 0;
  lv_screenGetSubOffset(&subX, &subY);
  const int edgeX = subX * zf;
  const int edgeY = subY * zf;

  /* Step 1 — background. */
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderFillRect(renderer, NULL);
  if (s_backgroundTex) {
    SDL_FRect dst = { 0.0f, 0.0f,
                      (float)(GV_SCREEN_W * zf),
                      (float)(GV_SCREEN_H * zf) };
    SDL_RenderTexture(renderer, s_backgroundTex, NULL, &dst);
  }

  /* Step 2 — MapViewCtx for the same atlas the rest of the LogViewer uses. */
  MapViewCtx ctx;
  ctx.renderer   = renderer;
  ctx.tilesTex   = lv_drawGetTilesTexture();
  ctx.zoomFactor = zf;
  ctx.sheetScale = lv_drawGetSheetScale();
  ctx.scale      = (float)zf;
  /* The log viewer builds no padded copy: it replays at a whole-number zoom
     with no sub-pixel motion, so the sampler has nothing to blend. */
  ctx.spritesTex = NULL;
  ctx.sprites    = NULL;

  /* Set clip rect so the map render stays within the main view (no
   * spillover into the surrounding chrome from the 1-tile mapView
   * margin). Mirrors sdl3DrawMainScreen — without it the leading-edge
   * margin tile (col 0 / row 0 of the buffer) paints over the side
   * panels during sub-tile pan. */
  {
    SDL_Rect clip = {
      originX, originY,
      MAIN_SCREEN_SIZE_X * tileW,
      MAIN_SCREEN_SIZE_Y * tileH
    };
    SDL_SetRenderClipRect(renderer, &clip);
  }

  /* Step 3 — tiles. */
  mapViewDrawTiles(&ctx, view, mines, NULL,
                   originX, originY, tileW, tileH,
                   edgeX, edgeY);

  /* Smart pings: on the ground, under everything that moves. Inside the
   * step-3 clip so a marker near the edge of the view is trimmed at the
   * border rather than painting over the chrome. A replay has no team, so
   * every ping in the recording is shown. */
  gv_drawPings(renderer, originX, originY, tileW, tileH, edgeX, edgeY);

  /* Steps 4-5 — sprites. lv_screenUpdate already populated tks/shells/
   * lgmList just before calling us via lv_drawMainScreen. */
  mapViewDrawShells(&ctx, shells,  originX, originY, tileW, tileH, edgeX, edgeY);
  mapViewDrawTanks (&ctx, tks,     originX, originY, tileW, tileH, edgeX, edgeY);
  mapViewDrawLGMs  (&ctx, lgmList, originX, originY, tileW, tileH, edgeX, edgeY);

  /* Death-static overlay. Painted INSIDE the step-3 clip rect so the
   * noise stays within the main view rectangle and doesn't bleed into
   * the surrounding chrome. The live game makes this branch exclusive
   * with the map render (sdl3draw.c:1315 else-if) — drawing additively
   * is simpler and visually equivalent since static covers the map
   * fully. Tick rate: SDL_GetTicks/GAME_TICK_LENGTH gives one tick per
   * 10ms wall clock, mirroring the live game's countdown cadence. */
  if (!lv_gameViewIsHudAlive(camera)) {
    gv_drawDeathStatic(renderer,
                       originX, originY,
                       MAIN_SCREEN_SIZE_X * tileW,
                       MAIN_SCREEN_SIZE_Y * tileH,
                       SDL_GetTicks() / GAME_TICK_LENGTH);
  }

  SDL_SetRenderClipRect(renderer, NULL);

  /* Step 6 — push edge offset into the moved sdl3DrawTankLabel for this
   * frame so labels track sub-tile pan along with the sprites. */
  sdl3DrawStatusSetEdgeOffset(edgeX, edgeY);

  /* Step 7 — self-perspective is now established by logviewer.c
   * around lv_screenUpdate (see logviewer.c's `if (g_lv->wantScreenUpdate
   * || g_lv->gameView)` block). Setting it there means tank sprite
   * frame indices baked during lv_screenUpdate also see camera as
   * self — without that, Tab updates the panels but not the map
   * sprite colours. The save/set/restore that used to live here
   * around just the panel renders is therefore redundant. */

  /* Step 8 — bases. */
  {
    bases *bs = (bases *)lv_gameViewGetBases();
    BYTE total = lv_basesGetNumBases(bs);
    sdl3DrawSetBasesStatusClear();
    for (BYTE i = 1; i <= total; i++) {
      base item;
      /* A base the recording has taken off the map keeps its number so the
         numbers above it go on naming the same base; its panel slot stays
         as the clear above left it. */
      if (lv_basesIsActive(bs, i) == FALSE) {
        continue;
      }
      lv_basesGetBase(bs, &item, i);
      baseAlliance ba = lv_basesGetAlliancePos(bs, item.x, item.y);
      sdl3DrawStatusBase(i, ba, /* labels */ false);
    }
  }

  /* Step 9 — pillboxes. */
  {
    pillboxes *pb = (pillboxes *)lv_gameViewGetPills();
    BYTE total = lv_pillsGetNumPills(pb);
    sdl3DrawSetPillsStatusClear();
    for (BYTE i = 1; i <= total; i++) {
      /* Same as the bases above: a removed pillbox keeps its number and
         draws nothing. */
      if (lv_pillsIsActive(pb, i) == FALSE) {
        continue;
      }
      pillAlliance pa = lv_pillsGetAllianceNum(pb, i);
      sdl3DrawStatusPillbox(i, pa, /* labels */ false);
    }
  }

  /* Step 10 — tanks. Walk all 16 slots so dead/never-joined slots get
   * cleared out of the panel (matches live game's MAX_TANKS loop). */
  sdl3DrawSetTanksStatusClear();
  for (BYTE i = 1; i <= MAX_TANKS; i++) {
    tankAlliance ta = lv_playersScreenAllience((BYTE)(i - 1));
    sdl3DrawStatusTank(i, ta);
  }

  /* Step 11 — tank stat bars. The values come from the recording: each
   * snapshot's player block carries the camera tank's stocks and a
   * log_TankSetStock record carries each change between snapshots. A
   * recording written before either was recorded reads as zero and the bars
   * draw empty. Base bars stay empty — the camera tank's view doesn't
   * spectate a specific base. */
  {
    BYTE shells = 0, mines = 0, armour = 0, trees = 0;
    if (lv_gameViewIsHudAlive(camera)) {
      lv_gameViewGetInventory(camera, &shells, &mines, &armour, &trees);
    }
    BYTE fullShells, fullMines, fullArmour, fullTrees;
    lv_gameViewGetTankFulls(&fullShells, &fullMines, &fullArmour, &fullTrees);
    sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees,
                           fullShells, fullMines, fullArmour, fullTrees);
    BYTE baseFullShells, baseFullMines, baseFullArmour;
    lv_gameViewGetBaseFulls(&baseFullShells, &baseFullMines, &baseFullArmour);
    sdl3DrawStatusBaseBars(0, 0, /* shells */ 0, /* mines */ 0, /* armour */ 0,
                           baseFullShells, baseFullMines, baseFullArmour, FALSE);
  }

  /* Step 11b — man-status circle. Mirrors live game's screen.c:4042-4051:
   * lgmGetStatus → frontEndManStatus/Clear. Source data is the camera
   * slot's lgmIsOut/lgmIsDead (set by log_LgmLocation / log_LostMan
   * during replay), with the arrow angle pointing from LGM toward tank
   * — same utilCalcAngle the live game uses. */
  {
    bool isOut = false, isDead = false;
    lv_playersGetLgmStatus(camera, &isOut, &isDead);
    if (!isOut) {
      gv_setManClear(renderer);
    } else if (isDead) {
      gv_setManStatus(renderer, true, 0.0f);
    } else {
      BYTE tmx = 0, tmy = 0, tpx = 0, tpy = 0, tframe = 0;
      bool tOnBoat = false;
      BYTE lmx = 0, lmy = 0, lpx = 0, lpy = 0, lframe = 0;
      lv_playersGetTankDetails(camera, &tmx, &tmy, &tpx, &tpy, &tframe, &tOnBoat);
      lv_playersGetLgmDetails(camera, &lmx, &lmy, &lpx, &lpy, &lframe);
      WORLD lgmWX  = ((WORLD)lmx << TANK_SHIFT_MAPSIZE) | ((WORLD)lpx << TANK_SHIFT_RIGHT2);
      WORLD lgmWY  = ((WORLD)lmy << TANK_SHIFT_MAPSIZE) | ((WORLD)lpy << TANK_SHIFT_RIGHT2);
      WORLD tankWX = ((WORLD)tmx << TANK_SHIFT_MAPSIZE) | ((WORLD)tpx << TANK_SHIFT_RIGHT2);
      WORLD tankWY = ((WORLD)tmy << TANK_SHIFT_MAPSIZE) | ((WORLD)tpy << TANK_SHIFT_RIGHT2);
      TURNTYPE angle = gv_calcAngle(lgmWX, lgmWY, tankWX, tankWY);
      gv_setManStatus(renderer, false, angle);
    }
  }

  /* Step 12 — scrolling newswire. Tick the marquee at the live game's
   * 40ms wall-clock cadence (4 * GAME_TICK_LENGTH) so playback speed
   * doesn't change scroll rate; render the current visible cells via
   * lv_messageGetMessage. Backfill option (a): events that arrived
   * before game-view activated are not replayed — only events from
   * activation forward scroll across. */
  {
    uint32_t now = SDL_GetTicks();
    /* SDL_GetTicks rolls over after ~49 days; if it appears to have
     * jumped backwards, restart the cadence anchor. */
    if (now < s_lastMessageTickMs) {
      s_lastMessageTickMs = now;
    }
    while (now - s_lastMessageTickMs >= GV_MESSAGE_TICK_MS) {
      lv_messageUpdate();
      s_lastMessageTickMs += GV_MESSAGE_TICK_MS;
    }

    char top[GV_MESSAGE_WIDTH];
    char bottom[GV_MESSAGE_WIDTH];
    lv_messageGetMessage(top, bottom);
    sdl3DrawMessages(0, 0, top, bottom);
  }

  /* Step 13 — kills/deaths text cache. */
  sdl3DrawKillsDeaths(0, 0,
                      (int)lv_gameViewGetKills(camera),
                      (int)lv_gameViewGetDeaths(camera));

  /* Step 14 — tank labels for every visible non-camera, in-use, alive
   * tank (decision §2: "other tanks only" — camera tank's name is
   * already in the tanks status panel). Strategy (i): walk
   * lv_playersIsInUse + lv_playersGetTankDetails directly. The
   * playerNum=0 hardcode in the bolo_shim screenTanksGetItem adapter
   * does not affect this path. */
  {
    const BYTE viewOffX = lv_screenGetXOffset();
    const BYTE viewOffY = lv_screenGetYOffset();
    for (BYTE slot = 0; slot < MAX_TANKS; slot++) {
      if (slot == camera) continue;
      if (!lv_playersIsInUse(slot)) continue;
      if (!lv_gameViewIsHudAlive(slot)) continue;

      char rawName[PLAYER_NAME_LEN];
      rawName[0] = '\0';
      lv_playersGetPlayerName(slot, rawName, sizeof(rawName));
      if (rawName[0] == '\0') continue;

      /* Brain-driven slots get an "[AI]" tag so a viewer scanning the
       * map can tell bots apart from humans at a glance. Same prefix
       * lv_playersMakeScreenName applies to the non-camera tank
       * labels — keeps both label paths visually consistent. */
      char name[PLAYER_NAME_LEN + 8];
      if (lv_playersIsBot(slot)) {
        snprintf(name, sizeof(name), "[AI] %s", rawName);
      } else {
        snprintf(name, sizeof(name), "%s", rawName);
      }

      BYTE rawMx = 0, rawMy = 0, rawPx = 0, rawPy = 0, frame = 0;
      bool onBoat = false;
      lv_playersGetTankDetails(slot, &rawMx, &rawMy, &rawPx, &rawPy, &frame, &onBoat);

      /* Apply the same TANK_SUBTRACT (128) adjustment that
       * lv_playersMakeScreenTanks (players.c:506-514) bakes into
       * screenTanks before mapView renders the sprite. The tank's
       * sprite anchor sits 8 unzoomed px (½ tile) up-and-left of the
       * raw mapX/pixelX/mapY/pixelY the log carries; without this the
       * label drifts ½ tile south-east of the sprite. The encoded
       * position is (mx<<8)+(px<<4) in 12-bit world units. */
      uint16_t encX = (uint16_t)(((uint16_t)rawMx << 8) + ((uint16_t)rawPx << 4) - 128);
      uint16_t encY = (uint16_t)(((uint16_t)rawMy << 8) + ((uint16_t)rawPy << 4) - 128);
      BYTE mx = (BYTE)(encX >> 8);
      BYTE my = (BYTE)(encY >> 8);
      BYTE px = (BYTE)((encX & 0xF0) >> 4);
      BYTE py = (BYTE)((encY & 0xF0) >> 4);

      /* mx/my are now in map-space (0..255); sdl3DrawTankLabel computes
       * `bbx = mx*TILE_SIZE_X+apx` and expects viewport-relative
       * screen-space (0..15). Subtract the viewport tile origin to
       * convert; skip tanks outside the visible viewport — their labels
       * would clip off-screen anyway. */
      int sx = (int)mx - (int)viewOffX;
      int sy = (int)my - (int)viewOffY;
      if (sx < 0 || sx >= GV_SCREEN_TILES) continue;
      if (sy < 0 || sy >= GV_SCREEN_TILES) continue;

      sdl3DrawTankLabel(name, slot, (BYTE)sx, (BYTE)sy, px, py);
    }
  }

  /* Step 15 — flush message + kills/deaths text caches. */
  sdl3RenderCachedText();

  /* Bar overlays. Mirrors sdl3draw.c sdl3RenderStatusPanels — the
   * status renderers above wrote into s_tankBarsTex / s_baseBarsTex
   * (render-target textures); blit those into the framebuffer at the
   * fixed positions live-game uses. */
  if (s_tankBarsTex) {
    SDL_SetTextureBlendMode(s_tankBarsTex, SDL_BLENDMODE_NONE);
    SDL_FRect d = { (float)(zf * STATUS_TANK_SHELLS),
                    (float)(zf * STATUS_TANK_BARS_TOP),
                    (float)(zf * STATUS_TANK_BARS_TOTALWIDTH),
                    (float)(zf * STATUS_TANK_BARS_HEIGHT) };
    SDL_RenderTexture(renderer, s_tankBarsTex, NULL, &d);
  }
  if (s_baseBarsTex) {
    SDL_SetTextureBlendMode(s_baseBarsTex, SDL_BLENDMODE_NONE);
    SDL_FRect d = { (float)(zf * STATUS_BASE_BARS_LEFT),
                    (float)(zf * STATUS_BASE_BARS_TOP),
                    (float)(zf * STATUS_BASE_BARS_MAX_WIDTH),
                    (float)(zf * STATUS_BASE_BARS_TOTALHEIGHT) };
    SDL_RenderTexture(renderer, s_baseBarsTex, NULL, &d);
  }
  if (s_manStatusTex && s_manStatusReady) {
    SDL_SetTextureBlendMode(s_manStatusTex, SDL_BLENDMODE_BLEND);
    SDL_FRect d = { (float)(zf * MAN_STATUS_X),
                    (float)(zf * MAN_STATUS_Y),
                    (float)(zf * (MAN_STATUS_WIDTH  + 2)),
                    (float)(zf * (MAN_STATUS_HEIGHT + 2)) };
    SDL_RenderTexture(renderer, s_manStatusTex, NULL, &d);
  }

  /* Step 16 — self is restored by the wrapping block in logviewer.c
   * after lv_screenUpdate returns; nothing to do here. */
}
