/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "build_cursor.h"

#include "client_render.h"
#include "client_sim.h"
#include "input_gamepad.h"   /* WB_CONTROLLER_DEBUG */
#include "../tiles.h"
#include "sdl3draw.h"

#include <stdio.h>
#include <stdarg.h>

/* Debug: writes to controller.log next to the exe when WB_CONTROLLER_DEBUG. */
static void bcLog(const char *fmt, ...) {
  if (!WB_CONTROLLER_DEBUG) return;
  static FILE *f = NULL;
  if (!f) f = fopen("controller.log", "a");
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fflush(f);
}

/* Edge margin (in tiles) before the camera scrolls to follow the
   cursor.  2 tiles leaves room for the 17-tile back buffer's 1-tile
   ghost margin plus a visible breathing tile. */
#define WB_CURSOR_MARGIN_TILES 2

/* Behaviour options (see header) — defaults match the gestures we shipped. */
bool g_buildExitExecutes  = false;
bool g_buildExitExecutesMomentaryOnly = false;
bool g_buildDoubleTapRoad = true;
bool g_buildHoldMomentary = true;
bool g_buildAutoCloseOnExecute = false;

static bool s_active     = false;
static bool s_positioned = false;  /* has the cursor been placed at least once? */
static BYTE s_mapX   = 128;
static BYTE s_mapY   = 128;
static int  s_subX   = 0;  /* sub-tile accumulator in zoomed pixels */
static int  s_subY   = 0;

void buildCursorReset(void) {
  s_active = false;
  s_positioned = false;
  s_mapX = 128;
  s_mapY = 128;
  s_subX = 0;
  s_subY = 0;
}

bool buildCursorIsActive(void) {
  return s_active;
}

void buildCursorExit(void) {
  bcLog("[bc] EXIT  pos=(%u,%u) positioned=%d", (unsigned)s_mapX, (unsigned)s_mapY, s_positioned);
  s_active = false;
  s_subX = 0;
  s_subY = 0;
}

/* True when the cursor's current absolute tile is within the visible
   15x15 area (1-based screen tile indices 1..MAIN_SCREEN_SIZE_X). */
static bool cursor_on_screen(struct ClientSim *cs);

/* Move the cursor onto the screen edge where the line from its current
   (off-screen) tile to the tank crosses into the visible area — the entry
   point closest to the cursor's original position.  Keeps the cursor as
   near as possible to where it was while bringing it into view.  No-op if
   the tank position is unavailable (cursor then stays put). */
static void clamp_to_edge_toward_tank(struct ClientSim *cs) {
  BYTE tx = 0, ty = 0;
  if (!clientSimGetMyTankMapPos(cs, &tx, &ty)) return;

  int xOff = (int)clientSimGetXOffset(cs);
  int yOff = (int)clientSimGetYOffset(cs);
  float xmin = (float)(xOff + 1);
  float xmax = (float)(xOff + MAIN_SCREEN_SIZE_X);
  float ymin = (float)(yOff + 1);
  float ymax = (float)(yOff + MAIN_SCREEN_SIZE_Y);

  float p0x = (float)s_mapX, p0y = (float)s_mapY;   /* off-screen origin */
  float dx  = (float)tx - p0x, dy = (float)ty - p0y;

  /* Liang-Barsky: t0 is the entry parameter (portion of the segment inside
     the rect, nearest P0).  The tank end is inside and the cursor end is
     outside, so a single crossing exists. */
  float t0 = 0.0f, t1 = 1.0f;
  float p[4] = { -dx, dx, -dy, dy };
  float q[4] = { p0x - xmin, xmax - p0x, p0y - ymin, ymax - p0y };
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0.0f) {
      if (q[i] < 0.0f) return;        /* parallel and outside — give up */
    } else {
      float r = q[i] / p[i];
      if (p[i] < 0.0f) { if (r > t0) t0 = r; }
      else             { if (r < t1) t1 = r; }
    }
  }
  if (t0 > t1) return;

  int mx = (int)(p0x + t0 * dx + 0.5f);
  int my = (int)(p0y + t0 * dy + 0.5f);
  if (mx < (int)xmin) mx = (int)xmin;
  if (mx > (int)xmax) mx = (int)xmax;
  if (my < (int)ymin) my = (int)ymin;
  if (my > (int)ymax) my = (int)ymax;
  bcLog("[bc] EDGE  off-screen (%u,%u) tank=(%u,%u) view=(%d,%d) -> (%d,%d)",
        (unsigned)s_mapX, (unsigned)s_mapY, (unsigned)tx, (unsigned)ty, xOff, yOff, mx, my);
  s_mapX = (BYTE)mx;
  s_mapY = (BYTE)my;
}

void buildCursorToggle(struct ClientSim *cs) {
  if (s_active) {
    buildCursorExit();
    return;
  }
  if (!cs) return;

  bcLog("[bc] TOGGLE-ON enter: stored=(%u,%u) positioned=%d onscreen=%d view=(%u,%u)",
        (unsigned)s_mapX, (unsigned)s_mapY, s_positioned, cursor_on_screen(cs),
        (unsigned)clientSimGetXOffset(cs), (unsigned)clientSimGetYOffset(cs));

  /* Toggling on must NOT move the cursor when it is already on-screen — it
     stays exactly where it was last left (by the stick or the mouse). */
  if (!s_positioned) {
    /* Very first activation: seed to the gunsight tile (or the tank tile if
       the gunsight isn't valid) so it starts somewhere visible.
       clientSimGetGunsightTile only writes valid coords when armour <=
       TANK_FULL_ARMOUR; if our tank is dead the locals stay at zero. */
    BYTE gx = 0, gy = 0;
    clientSimGetGunsightTile(cs, &gx, &gy);
    if (gx == 0 && gy == 0) {
      if (!clientSimGetMyTankMapPos(cs, &gx, &gy)) {
        gx = 128;
        gy = 128;
      }
    }
    s_mapX = gx;
    s_mapY = gy;
    s_positioned = true;
  } else if (!cursor_on_screen(cs)) {
    /* Off-screen: bring it to the visible edge along the line toward the
       tank, landing on the edge tile nearest its original position. */
    clamp_to_edge_toward_tank(cs);
  }
  s_subX = 0;
  s_subY = 0;
  s_active = true;
  bcLog("[bc] TOGGLE-ON result: pos=(%u,%u)", (unsigned)s_mapX, (unsigned)s_mapY);
}

bool buildCursorGetTile(BYTE *mapX, BYTE *mapY) {
  if (!s_active) return false;
  if (mapX) *mapX = s_mapX;
  if (mapY) *mapY = s_mapY;
  return true;
}

void buildCursorSetTile(BYTE mapX, BYTE mapY) {
  bcLog("[bc] SET-TILE (mouse) (%u,%u) -> (%u,%u) active=%d",
        (unsigned)s_mapX, (unsigned)s_mapY, (unsigned)mapX, (unsigned)mapY, s_active);
  s_mapX = mapX;
  s_mapY = mapY;
  s_subX = 0;
  s_subY = 0;
  s_positioned = true;
}

bool buildCursorGetTargetTile(BYTE *mapX, BYTE *mapY) {
  if (!s_positioned) return false;
  if (mapX) *mapX = s_mapX;
  if (mapY) *mapY = s_mapY;
  return true;
}

void buildCursorClampToView(struct ClientSim *cs) {
  /* Only while cursor mode is ON: the cursor must never sit outside the
     visible edge.  As the tank drives and the view scrolls, this drags the
     cursor along the edge so it stays on-screen.  When cursor mode is OFF the
     target tile is left pinned to its absolute position (it may scroll
     off-screen — that's the "lock a target then run away" case). */
  if (!s_active || !cs) return;
  int xOff = (int)clientSimGetXOffset(cs);
  int yOff = (int)clientSimGetYOffset(cs);
  int xmin = xOff + 1, xmax = xOff + MAIN_SCREEN_SIZE_X;
  int ymin = yOff + 1, ymax = yOff + MAIN_SCREEN_SIZE_Y;
  int mx = (int)s_mapX, my = (int)s_mapY;
  if (mx < xmin) mx = xmin;
  if (mx > xmax) mx = xmax;
  if (my < ymin) my = ymin;
  if (my > ymax) my = ymax;
  if (mx != (int)s_mapX || my != (int)s_mapY) {
    bcLog("[bc] CLAMP-VIEW (%u,%u) -> (%d,%d) view=(%d,%d)",
          (unsigned)s_mapX, (unsigned)s_mapY, mx, my, xOff, yOff);
  }
  s_mapX = (BYTE)mx;
  s_mapY = (BYTE)my;
}

/* Camera-follow helper.  Steps xOffset/yOffset toward the cursor when
   the cursor is within WB_CURSOR_MARGIN_TILES of the visible-area
   edge.  screenLx is the cursor's 1-based screen tile (visible range
   1..MAIN_SCREEN_SIZE_X), so margin=2 means we keep the cursor in the
   range [margin+1, SCREEN-margin] = [3, 13].  clientRenderFrame clamps
   to map bounds; if the offset stops moving the loop breaks so the
   cursor can still reach map-edge tiles without spinning. */
static void follow_camera(struct ClientSim *cs) {
  for (int i = 0; i < MAIN_SCREEN_SIZE_X; ++i) {
    int screenLx = (int)s_mapX - (int)clientSimGetXOffset(cs);
    if (screenLx < WB_CURSOR_MARGIN_TILES + 1) {
      BYTE prev = clientSimGetXOffset(cs);
      clientRenderFrame(cs, left);
      if (clientSimGetXOffset(cs) == prev) break;
    } else if (screenLx > MAIN_SCREEN_SIZE_X - WB_CURSOR_MARGIN_TILES) {
      BYTE prev = clientSimGetXOffset(cs);
      clientRenderFrame(cs, right);
      if (clientSimGetXOffset(cs) == prev) break;
    } else {
      break;
    }
  }
  for (int i = 0; i < MAIN_SCREEN_SIZE_Y; ++i) {
    int screenLy = (int)s_mapY - (int)clientSimGetYOffset(cs);
    if (screenLy < WB_CURSOR_MARGIN_TILES + 1) {
      BYTE prev = clientSimGetYOffset(cs);
      clientRenderFrame(cs, up);
      if (clientSimGetYOffset(cs) == prev) break;
    } else if (screenLy > MAIN_SCREEN_SIZE_Y - WB_CURSOR_MARGIN_TILES) {
      BYTE prev = clientSimGetYOffset(cs);
      clientRenderFrame(cs, down);
      if (clientSimGetYOffset(cs) == prev) break;
    } else {
      break;
    }
  }
}

/* True when the cursor's current absolute tile is within the visible
   15x15 area (1-based screen tile indices 1..MAIN_SCREEN_SIZE_X). */
static bool cursor_on_screen(struct ClientSim *cs) {
  int screenLx = (int)s_mapX - (int)clientSimGetXOffset(cs);
  int screenLy = (int)s_mapY - (int)clientSimGetYOffset(cs);
  return (screenLx >= 1 && screenLx <= MAIN_SCREEN_SIZE_X &&
          screenLy >= 1 && screenLy <= MAIN_SCREEN_SIZE_Y);
}

void buildCursorTick(struct ClientSim *cs, int dxPx, int dyPx) {
  if (!s_active || !cs) return;

  int zoom = sdl3DrawGetZoomFactor();
  if (zoom < 1) zoom = 1;
  int tileW = TILE_SIZE_X * zoom;
  int tileH = TILE_SIZE_Y * zoom;

  s_subX += dxPx;
  s_subY += dyPx;

  while (s_subX >=  tileW) { if (s_mapX < 255) s_mapX++; s_subX -= tileW; }
  while (s_subX <= -tileW) { if (s_mapX > 0)   s_mapX--; s_subX += tileW; }
  while (s_subY >=  tileH) { if (s_mapY < 255) s_mapY++; s_subY -= tileH; }
  while (s_subY <= -tileH) { if (s_mapY > 0)   s_mapY--; s_subY += tileH; }

  /* Scroll the view to follow, then clamp the cursor inside the visible edge:
     while cursor mode is ON the cursor can never be actively moved off-screen
     (at a map boundary the view can't scroll, so it stops at the edge tile). */
  follow_camera(cs);
  buildCursorClampToView(cs);
}
