/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "build_cursor.h"

#include "../../bolo/screen.h"
#include "../../bolo/tank.h"
#include "../../bolo/client_sim.h"
#include "../tiles.h"
#include "sdl3draw.h"

/* Edge margin (in tiles) before the camera scrolls to follow the
   cursor.  2 tiles leaves room for the 17-tile back buffer's 1-tile
   ghost margin plus a visible breathing tile. */
#define WB_CURSOR_MARGIN_TILES 2

static bool s_active = false;
static BYTE s_mapX   = 128;
static BYTE s_mapY   = 128;
static int  s_subX   = 0;  /* sub-tile accumulator in zoomed pixels */
static int  s_subY   = 0;

void buildCursorReset(void) {
  s_active = false;
  s_mapX = 128;
  s_mapY = 128;
  s_subX = 0;
  s_subY = 0;
}

bool buildCursorIsActive(void) {
  return s_active;
}

void buildCursorExit(void) {
  s_active = false;
  s_subX = 0;
  s_subY = 0;
}

void buildCursorToggle(struct ClientSim *cs) {
  if (s_active) {
    buildCursorExit();
    return;
  }
  if (!cs) return;

  /* Snap to the gunsight tile so the player has a known starting
     point.  screenGetGunsightTileCS only writes valid coords when
     armour <= TANK_FULL_ARMOUR; if our tank is dead the locals stay
     at zero, in which case fall back to the tank's own tile. */
  BYTE gx = 0, gy = 0;
  screenGetGunsightTileCS(cs, &gx, &gy);
  if (gx == 0 && gy == 0) {
    if (MY_TANK(cs) != NULL) {
      gx = tankGetMX(&MY_TANK(cs));
      gy = tankGetMY(&MY_TANK(cs));
    } else {
      gx = 128;
      gy = 128;
    }
  }
  s_mapX = gx;
  s_mapY = gy;
  s_subX = 0;
  s_subY = 0;
  s_active = true;
}

bool buildCursorGetTile(BYTE *mapX, BYTE *mapY) {
  if (!s_active) return false;
  if (mapX) *mapX = s_mapX;
  if (mapY) *mapY = s_mapY;
  return true;
}

/* Camera-follow helper.  Steps xOffset/yOffset toward the cursor when
   the cursor is within WB_CURSOR_MARGIN_TILES of the visible-area
   edge.  screenLx is the cursor's 1-based screen tile (visible range
   1..MAIN_SCREEN_SIZE_X), so margin=2 means we keep the cursor in the
   range [margin+1, SCREEN-margin] = [3, 13].  screenUpdateCS clamps
   to map bounds; if the offset stops moving the loop breaks so the
   cursor can still reach map-edge tiles without spinning. */
static void follow_camera(struct ClientSim *cs) {
  for (int i = 0; i < MAIN_SCREEN_SIZE_X; ++i) {
    int screenLx = (int)s_mapX - (int)cs->xOffset;
    if (screenLx < WB_CURSOR_MARGIN_TILES + 1) {
      BYTE prev = cs->xOffset;
      screenUpdateCS(cs, left);
      if (cs->xOffset == prev) break;
    } else if (screenLx > MAIN_SCREEN_SIZE_X - WB_CURSOR_MARGIN_TILES) {
      BYTE prev = cs->xOffset;
      screenUpdateCS(cs, right);
      if (cs->xOffset == prev) break;
    } else {
      break;
    }
  }
  for (int i = 0; i < MAIN_SCREEN_SIZE_Y; ++i) {
    int screenLy = (int)s_mapY - (int)cs->yOffset;
    if (screenLy < WB_CURSOR_MARGIN_TILES + 1) {
      BYTE prev = cs->yOffset;
      screenUpdateCS(cs, up);
      if (cs->yOffset == prev) break;
    } else if (screenLy > MAIN_SCREEN_SIZE_Y - WB_CURSOR_MARGIN_TILES) {
      BYTE prev = cs->yOffset;
      screenUpdateCS(cs, down);
      if (cs->yOffset == prev) break;
    } else {
      break;
    }
  }
}

/* True when the cursor's current absolute tile is within the visible
   15x15 area (1-based screen tile indices 1..MAIN_SCREEN_SIZE_X). */
static bool cursor_on_screen(struct ClientSim *cs) {
  int screenLx = (int)s_mapX - (int)cs->xOffset;
  int screenLy = (int)s_mapY - (int)cs->yOffset;
  return (screenLx >= 1 && screenLx <= MAIN_SCREEN_SIZE_X &&
          screenLy >= 1 && screenLy <= MAIN_SCREEN_SIZE_Y);
}

void buildCursorTick(struct ClientSim *cs, int dxPx, int dyPx) {
  if (!s_active || !cs) return;

  /* Mirror the mouse-cursor behaviour the user described: when the
     cursor has drifted off-screen (the tank drove the view away while
     the cursor stayed pinned to its absolute tile) and the player
     starts moving it again, restart from the tank tile so they have a
     visible reference instead of nudging an invisible reticle around
     the map. */
  if (!cursor_on_screen(cs)) {
    if (MY_TANK(cs) != NULL) {
      s_mapX = tankGetMX(&MY_TANK(cs));
      s_mapY = tankGetMY(&MY_TANK(cs));
    }
    s_subX = 0;
    s_subY = 0;
  }

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

  follow_camera(cs);
}
