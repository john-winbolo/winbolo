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
 * Name:          sprite_positions.h
 * Purpose:
 *   Tile-number -> atlas-coordinate lookup tables, and the
 *   arithmetic that puts a sprite on screen. Split out of
 *   mapview.h so modules that only need the table (e.g.
 *   the log viewer) can include it without pulling in bolo
 *   screen / sprite-list typedefs that conflict with their
 *   own equivalents, and so the position maths can be
 *   linked into the unit tests with no renderer behind it.
 *
 *   The tables are populated by mapViewInit(), defined in
 *   sprite_positions.c. Values are atlas pixel offsets at
 *   scale 1; callers multiply by their atlas sheet scale
 *   at blit time.
 *********************************************************/

#ifndef SPRITE_POSITIONS_H
#define SPRITE_POSITIONS_H

#ifdef __cplusplus
extern "C" {
#endif

extern int mapViewPosX[256];
extern int mapViewPosY[256];

/* Populate mapViewPosX/Y. Idempotent — safe to call from multiple
 * subsystems' setup paths. */
void mapViewInit(void);

/* Offset of a sprite from the map origin, in screen pixels. `square` is the
   map square on this axis, `pixelOff` its 0..15 game-pixel offset and
   `worldOff` its 0..255 world offset inside the square. `scale` is screen
   pixels per game pixel, `mode` a GfxAnimSmoothness value and `sheetScale`
   the atlas scale Match pixelation snaps to. */
float spritePositionOffset(int mode, float scale, int sheetScale,
                           int square, int pixelOff, int worldOff);

/* Shell top-left, including the tip anchoring that puts the leading pixel on
   the shell's world position. frame is the raw screenBullets frame; when it
   is outside SHELL_DIR0..SHELL_DIR15 no anchoring is applied. baseX/baseY is
   where the lists' square 0,0 lands on screen: the drawers' originX - tileW
   - edgeX. */
void spritePositionShell(float baseX, float baseY, int mode, float scale,
                         int sheetScale, int mx, int my, int px, int py,
                         int wx, int wy, int frame,
                         float *outX, float *outY);

/* LGM top-left, including the (1.5, 2.0) game-pixel centring and the
   whole-game-pixel display snap that every mode but Smooth applies. Both
   are for the on-foot frames LGM0..LGM2; any other frame is the helicopter,
   drawn from its own top-left. The snap is relative to baseX/baseY, so the
   base has to be the same one the tanks are placed from. */
void spritePositionLgm(float baseX, float baseY, int mode, float scale,
                       int sheetScale, int mx, int my, int px, int py,
                       int wx, int wy, int frame,
                       float *outX, float *outY);

#ifdef __cplusplus
}
#endif

#endif /* SPRITE_POSITIONS_H */
