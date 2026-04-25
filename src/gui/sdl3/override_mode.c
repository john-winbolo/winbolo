/*
 * override_mode — see override_mode.h.
 *
 * Implementation note: the world-coord-to-screen-coord conversion has
 * to match what mapViewDrawShells does so overlays land where the
 * sprites do.  The classic game's bullet coord is
 *   bbx = mx * TILE_SIZE_X + px   (with mx,px being the screen-bullet
 *                                  coords, NOT raw q->x/y)
 *   sx  = originX - tileW + bbx*zf - edgeX
 * where mx,px come from `screenBullets` populated via TANK_SUBTRACT-
 * based transforms from sim shell positions.
 *
 * For overlays we read the AUTHORITATIVE world coords directly from
 * cs->sim.shs / cs->sim.lgmen / cs->sim.tanks / cs->sim.pb / cs->sim.bs
 * which are in plain world units (1/256 tile).  Convert via the
 * inverse of the same transform — the rule is:
 *   game_pixel = world_unit / 16    (1 game pixel = 16 wu)
 *   sx = originX - tileW + (game_pixel + TILE_SIZE_X) * zf - edgeX
 *      = originX + game_pixel * zf - edgeX
 * (the -tileW + +TILE_SIZE_X*zf cancels at zf=1, but at higher zoom
 *  we still want a tile of head-room left of origin for the
 *  off-screen edge tiles — same as mapViewDrawShells).
 *
 * Actually the cleanest formula: the existing mapViewDrawShells uses
 *   sx = originX - tileW + bbx * zf - edgeX
 * where bbx = mx*16 + px is in game pixels.  For our overlay coord at
 * world units (wx, wy), the equivalent game-pixel is wx/16, wy/16.
 * The +TILE_SIZE_X (=16) head-room compensates for screenBullets'
 * coordinate convention (top-left of viewport at -1 tile).  We use:
 *   sx = originX - tileW + (wx * zf / 16) - edgeX
 * with float math so sub-pixel precision is preserved.
 */

#include "override_mode.h"
#include "gfx_settings.h"

#include "../../bolo/global.h"
#include "../../bolo/client_sim.h"
#include "../../bolo/tank.h"
#include "../../bolo/util.h"
#include "../../bolo/pillbox.h"
#include "../../bolo/bases.h"
#include "../../bolo/lgm.h"
#include "../tiles.h"

static bool s_overrideOn = false;
static int  s_extraDelayMs = 0;        /* 0 = normal speed; >0 = slower */
#define EXTRA_DELAY_STEP_MS 10
#define EXTRA_DELAY_MAX_MS 200

void overrideModeToggle(void) {
  s_overrideOn = !s_overrideOn;
  /* Toggling override OFF resets speed back to normal so we don't
   * leave the game running slow without the visual indicator. */
  if (!s_overrideOn) s_extraDelayMs = 0;
  SDL_Log("[OverrideMode] %s", s_overrideOn ? "ON" : "OFF");
}

bool overrideModeIsOn(void) {
  return s_overrideOn;
}

void overrideModeSlower(void) {
  s_extraDelayMs += EXTRA_DELAY_STEP_MS;
  if (s_extraDelayMs > EXTRA_DELAY_MAX_MS) s_extraDelayMs = EXTRA_DELAY_MAX_MS;
  SDL_Log("[OverrideMode] speed slower (extra delay %d ms)", s_extraDelayMs);
}

void overrideModeFaster(void) {
  s_extraDelayMs -= EXTRA_DELAY_STEP_MS;
  if (s_extraDelayMs < 0) s_extraDelayMs = 0;
  SDL_Log("[OverrideMode] speed faster (extra delay %d ms)", s_extraDelayMs);
}

int overrideModeExtraDelayMs(void) {
  return s_extraDelayMs;
}

/* Convert world units to screen X using the same convention as
 * mapViewDrawShells.  wx is in 1/256-tile (full precision).
 * Quantisation is controlled by the Graphics → Animation style:
 *   Pixel Floor    → floor(wx/16)        — integer game pixel
 *   Pixel Nearest  → round(wx/16)        — nearest game pixel
 *   Smooth         → wx/16.0             — full sub-pixel float */
static float wuToScreenX(int wx, int originX, int tileW,
                         int edgeX, int zoomFactor) {
  float gpx = gfxSettingsWuToGamePixel(wx);
  return (float)originX - (float)tileW
       + gpx * (float)zoomFactor
       - (float)edgeX;
}

static float wuToScreenY(int wy, int originY, int tileH,
                         int edgeY, int zoomFactor) {
  float gpy = gfxSettingsWuToGamePixel(wy);
  return (float)originY - (float)tileH
       + gpy * (float)zoomFactor
       - (float)edgeY;
}

/* Draw a yellow outlined rectangle covering ±halfWu around (cxWu, cyWu). */
static void drawHitboxRect(SDL_Renderer *r, int cxWu, int cyWu, int halfWu,
                           int originX, int originY,
                           int tileW, int tileH,
                           int edgeX, int edgeY, int zoomFactor) {
  float left   = wuToScreenX(cxWu - halfWu, originX, tileW, edgeX, zoomFactor);
  float top    = wuToScreenY(cyWu - halfWu, originY, tileH, edgeY, zoomFactor);
  float right  = wuToScreenX(cxWu + halfWu, originX, tileW, edgeX, zoomFactor);
  float bottom = wuToScreenY(cyWu + halfWu, originY, tileH, edgeY, zoomFactor);
  SDL_FRect rect = { left, top, right - left, bottom - top };
  SDL_RenderRect(r, &rect);
}

/* Filled orange dot centered on (cxWu, cyWu).  Always drawn at a min
 * of 3 screen pixels so it's visible at low zoom. */
static void drawHitDot(SDL_Renderer *r, int cxWu, int cyWu,
                       int originX, int originY,
                       int tileW, int tileH,
                       int edgeX, int edgeY, int zoomFactor) {
  float cx = wuToScreenX(cxWu, originX, tileW, edgeX, zoomFactor);
  float cy = wuToScreenY(cyWu, originY, tileH, edgeY, zoomFactor);
  /* 1 wu in screen pixels = zoomFactor / 16 */
  float oneWu = (float)zoomFactor / 16.0f;
  float size  = oneWu < 3.0f ? 3.0f : oneWu;
  SDL_FRect dot = { cx - size * 0.5f, cy - size * 0.5f, size, size };
  SDL_RenderFillRect(r, &dot);
}

/* Draw a 16-grid over the world view.  Per-game-pixel lines are thin
 * blue, every 4th game pixel is a slightly thicker line, every tile
 * boundary is the thickest.  Tile lines are drawn last so they sit
 * on top of the finer divisions. */
static void drawWorldGrid(SDL_Renderer *r,
                          int originX, int originY,
                          int tileW, int tileH,
                          int edgeX, int edgeY, int zoomFactor) {
  /* The world covers a 256-tile grid.  In screen pixels, the screen
   * area for the world view is approximately MAIN_BACK_BUFFER_SIZE_X
   * tiles by MAIN_BACK_BUFFER_SIZE_Y, but we don't have those here.
   * Just iterate enough lines to cover the visible viewport: pull
   * the renderer's draw bounds. */
  int viewW = 0, viewH = 0;
  SDL_GetCurrentRenderOutputSize(r, &viewW, &viewH);

  /* World pixel = wu / 16.  Screen pixel = world_pixel * zoomFactor.
   * gridSpacing in screen pixels for one game-pixel line = zoomFactor.
   * For a tile boundary it's tileW (= 16 * zoomFactor). */
  int gp = zoomFactor;          /* screen pixels per game pixel */
  if (gp < 1) gp = 1;

  /* Faint per-game-pixel lines (1/16 of a tile). */
  SDL_SetRenderDrawColor(r, 60, 60, 110, 60);
  for (float x = (float)(originX - tileW - edgeX); x < (float)viewW; x += (float)gp) {
    if (x >= (float)originX) SDL_RenderLine(r, x, 0, x, (float)viewH);
  }
  for (float y = (float)(originY - tileH - edgeY); y < (float)viewH; y += (float)gp) {
    if (y >= (float)originY) SDL_RenderLine(r, 0, y, (float)viewW, y);
  }

  /* Slightly thicker on every 4th game pixel. */
  SDL_SetRenderDrawColor(r, 90, 90, 160, 100);
  for (float x = (float)(originX - tileW - edgeX); x < (float)viewW; x += (float)(gp * 4)) {
    if (x >= (float)originX) SDL_RenderLine(r, x, 0, x, (float)viewH);
  }
  for (float y = (float)(originY - tileH - edgeY); y < (float)viewH; y += (float)(gp * 4)) {
    if (y >= (float)originY) SDL_RenderLine(r, 0, y, (float)viewW, y);
  }

  /* Tile boundaries (thickest looking) drawn brighter. */
  SDL_SetRenderDrawColor(r, 160, 160, 220, 180);
  for (float x = (float)(originX - tileW - edgeX); x < (float)viewW; x += (float)tileW) {
    if (x >= (float)originX) SDL_RenderLine(r, x, 0, x, (float)viewH);
  }
  for (float y = (float)(originY - tileH - edgeY); y < (float)viewH; y += (float)tileH) {
    if (y >= (float)originY) SDL_RenderLine(r, 0, y, (float)viewW, y);
  }
}

void overrideModeDrawOverlays(SDL_Renderer *renderer,
                              struct ClientSim *cs,
                              int originX, int originY,
                              int tileW, int tileH,
                              int edgeX, int edgeY,
                              int zoomFactor) {
  if (!s_overrideOn) return;
  if (!renderer || !cs) return;

  GameSim *sim = &cs->sim;
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

  /* Grid first so hitboxes/dots overlay on top. */
  drawWorldGrid(renderer, originX, originY, tileW, tileH, edgeX, edgeY, zoomFactor);

  /* Tank hitboxes:
   *   YELLOW = tankIsTankHit ±128 wu shell-vs-tank box (tank.c:1089).
   *   CYAN   = direction-dependent tank-vs-world bbox used by
   *            tankNudgeBuildings (boat or land table).  This is the
   *            collision shape against walls/pillboxes/bases — and
   *            it's NOT a square: it varies per facing direction. */
  for (int i = 0; i < MAX_TANKS; i++) {
    tank *tk = &sim->tanks[i];
    if (*tk == NULL) continue;
    if (tankGetDeathWait(tk) > 0) continue;
    if (tankGetArmour(tk) > TANK_FULL_ARMOUR) continue;
    WORLD twx, twy;
    tankGetWorld(tk, &twx, &twy);

    /* Yellow shell hitbox first */
    SDL_SetRenderDrawColor(renderer, 255, 255, 0, 220);
    drawHitboxRect(renderer, (int)twx, (int)twy, 128,
                   originX, originY, tileW, tileH, edgeX, edgeY, zoomFactor);

    /* Cyan world-collision bbox: per-direction insets from edge of
     * 16x16 sprite, converted to wu offsets via (8 - inset) * 16. */
    BYTE dirIdx = utilGetDir(tankGetAngle(tk));
    bool onBoat = tankIsOnBoat(tk);
    TankBoundingBox bb = tankGetBoundingBox(dirIdx, onBoat);
    int leftWu   = (int)twx - ((8 - (int)bb.left)   * 16);
    int rightWu  = (int)twx + ((8 - (int)bb.right)  * 16);
    int topWu    = (int)twy - ((8 - (int)bb.top)    * 16);
    int bottomWu = (int)twy + ((8 - (int)bb.bottom) * 16);
    float l = wuToScreenX(leftWu,   originX, tileW, edgeX, zoomFactor);
    float r = wuToScreenX(rightWu,  originX, tileW, edgeX, zoomFactor);
    float t = wuToScreenY(topWu,    originY, tileH, edgeY, zoomFactor);
    float b = wuToScreenY(bottomWu, originY, tileH, edgeY, zoomFactor);
    SDL_FRect bbRect = { l, t, r - l, b - t };
    SDL_SetRenderDrawColor(renderer, 0, 220, 220, 220);
    SDL_RenderRect(renderer, &bbRect);
  }

  /* Yellow pill 1-tile hitboxes (pillsIsPillHit is whole-tile). */
  if (sim->pb != NULL) {
    BYTE numPb = pillsGetNumPills(&sim->pb);
    for (BYTE pi = 0; pi < numPb; pi++) {
      if ((*sim->pb).item[pi].inTank) continue;
      if ((*sim->pb).item[pi].armour == 0) continue;  /* dead pill = capturable, not a hit target */
      int mx = (int)(*sim->pb).item[pi].x;
      int my = (int)(*sim->pb).item[pi].y;
      /* Tile centre + ±128 wu for a 1-tile box, snapped to grid. */
      int cxWu = mx * 256 + 128;
      int cyWu = my * 256 + 128;
      drawHitboxRect(renderer, cxWu, cyWu, 128,
                     originX, originY, tileW, tileH, edgeX, edgeY, zoomFactor);
    }
  }

  /* Yellow base 1-tile hitboxes (capture/refuel is whole-tile). */
  if (sim->bs != NULL) {
    BYTE numBs = basesGetNumBases(&sim->bs);
    for (BYTE bi = 0; bi < numBs; bi++) {
      int mx = (int)(*sim->bs).item[bi].x;
      int my = (int)(*sim->bs).item[bi].y;
      int cxWu = mx * 256 + 128;
      int cyWu = my * 256 + 128;
      drawHitboxRect(renderer, cxWu, cyWu, 128,
                     originX, originY, tileW, tileH, edgeX, edgeY, zoomFactor);
    }
  }

  /* Orange sub-pixel dot at each live shell's authoritative position. */
  SDL_SetRenderDrawColor(renderer, 255, 140, 0, 255);
  {
    shells q = sim->shs;
    while (q != NULL) {
      if (!q->shellDead) {
        drawHitDot(renderer, (int)q->x, (int)q->y,
                   originX, originY, tileW, tileH, edgeX, edgeY, zoomFactor);
      }
      q = q->next;
    }
  }

  /* Orange sub-pixel dot at each live LGM's position. */
  for (int i = 0; i < MAX_TANKS; i++) {
    lgm *l = &sim->lgmen[i];
    if (*l == NULL) continue;
    if ((*l)->inTank || (*l)->isDead) continue;
    drawHitDot(renderer, (int)(*l)->x, (int)(*l)->y,
               originX, originY, tileW, tileH, edgeX, edgeY, zoomFactor);
  }
}
