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
 * Name:          Render Island ScreenTanks Adapter
 * Filename:      render_island_screentank_adapter.c
 * Purpose:
 *   The client links a second, log-viewer-bound copy of
 *   gui/sdl3/mapview.c (the "render island") so game_view.c
 *   can draw the log viewer's pointer-layout screen buffers.
 *   Seven of the eight screen accessors mapViewDraw* calls are
 *   rebound to their lv_* equivalents by a plain build-time
 *   macro (matching parameter lists; a pointer is just a
 *   pointer at the ABI level).
 *
 *   screenTanksGetItem is the exception. bolo's signature ends
 *   in (BYTE *playerNum, char *playerName), but the log viewer's
 *   lv_screenTanksGetItem ends in (BYTE *team, BYTE *dir,
 *   bool *onBoat, char *playerName) — a different parameter list
 *   a macro can't bridge. The island remaps screenTanksGetItem
 *   to this adapter instead.
 *
 *   mapViewDrawTanks captures playerNum/playerName but never
 *   reads them (the game-view tank-label pass walks the player
 *   table directly), and the log viewer's screenTanks slot does
 *   not carry playerNum, so it is reported as 0.
 *********************************************************/

#include "lv_screentank.h"  /* screenTanks, lv_screenTanksGetItem, BYTE, bool, FALSE */

void lvg_screenTanksGetItem(screenTanks *value, BYTE itemNum,
                            BYTE *mx, BYTE *my, BYTE *px, BYTE *py,
                            BYTE *frame, BYTE *playerNum, char *playerName) {
  BYTE team = 0, dir = 0;
  bool onBoat = FALSE;
  lv_screenTanksGetItem(value, itemNum, mx, my, px, py, frame,
                        &team, &dir, &onBoat, playerName);
  if (playerNum) *playerNum = 0;
}
