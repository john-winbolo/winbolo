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

#include "../../bolo/global.h"
#include "../../bolo/client_sim.h"
#include "../../bolo/tank.h"
#include "../../bolo/pillbox.h"
#include "../../bolo/bases.h"
#include "../../bolo/lgm.h"
#include "../tiles.h"

static bool s_overrideOn = false;

void overrideModeToggle(void) {
  s_overrideOn = !s_overrideOn;
  SDL_Log("[OverrideMode] %s", s_overrideOn ? "ON" : "OFF");
}

bool overrideModeIsOn(void) {
  return s_overrideOn;
}

/* Convert world units to screen X using the same convention as
 * mapViewDrawShells.  wx is in 1/256-tile (full precision). */
static float wuToScreenX(int wx, int originX, int tileW,
                         int edgeX, int zoomFactor) {
  /* Game pixels (float, sub-pixel) */
  float gpx = (float)wx / 16.0f;
  /* Offset by 1 tile (head-room left of origin) and scale */
  return (float)originX - (float)tileW
       + gpx * (float)zoomFactor
       - (float)edgeX;
}

static float wuToScreenY(int wy, int originY, int tileH,
                         int edgeY, int zoomFactor) {
  float gpy = (float)wy / 16.0f;
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

  /* Yellow tank ±128 wu hitboxes (tankIsTankHit threshold). */
  SDL_SetRenderDrawColor(renderer, 255, 255, 0, 220);
  for (int i = 0; i < MAX_TANKS; i++) {
    tank *tk = &sim->tanks[i];
    if (*tk == NULL) continue;
    if (tankGetDeathWait(tk) > 0) continue;
    if (tankGetArmour(tk) > TANK_FULL_ARMOUR) continue;
    WORLD twx, twy;
    tankGetWorld(tk, &twx, &twy);
    drawHitboxRect(renderer, (int)twx, (int)twy, 128,
                   originX, originY, tileW, tileH, edgeX, edgeY, zoomFactor);
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
