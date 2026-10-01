/*
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

#include <stdbool.h>

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

/* The overlay's placements, on the same base and scale. These are whole
   game pixels whatever the animation mode: the cursor and the item numbers
   sit on squares, and the gunsight and the tank labels have always been
   placed from the square and pixel alone. */

/* Top-left of a whole square: where the build cursor goes, and the box an
   item number is drawn in. */
float spritePositionSquare(float base, float scale, int square);

/* Top-left of the gunsight sprite: the tank formula at the sight's game
   pixel, so a crosshair one pixel wider than a tile has its centre pixel
   on the aim point. */
float spritePositionGunsight(float base, float scale, int square, int pixelOff);

/* Top-left of a tank's name: one square to the right of the sprite's game
   pixel, and held inside the clip's left edge so a name is never cut off
   on the left. */
void spritePositionTankLabel(float baseX, float baseY, float scale,
                             int mx, int my, int px, int py, float clipLeft,
                             float *outX, float *outY);

/* The tank name as the overlay draws it in animation mode `mode`. In Smooth
   the name is placed from the same world position the tank sprite is drawn
   at (square, world offset wx/wy), so the two move together; every other
   mode is spritePositionTankLabel, from the square and game pixel. */
void spritePositionTankLabelAt(float baseX, float baseY, int mode, float scale,
                               int sheetScale, int mx, int my, int px, int py,
                               int wx, int wy, float clipLeft,
                               float *outX, float *outY);

/* Whether the pill and base numbers are drawn at this scale. */
bool spritePositionItemLabelShown(float scale, float minScale);

/* The menu background's camera (mapViewRenderCentered). A view edge at
   `pos` pixels from the map's left or top splits into the first square it
   shows and how far into that square it starts: pos = square * unit + edge,
   with edge always 0..unit-1. pos can be negative (the view reaches past
   the map's left or top edge) and square then is too; the two are never
   changed apart, or the whole view jumps by up to a square. */
void mapViewCameraSplit(int pos, int unit, int *outSquare, int *outEdge);

/* One axis of the menu background's camera, the whole of it:
   mapViewRenderCentered draws every tile and sprite from these two values
   and nothing else, so they are what the unit test checks. centerW is the
   view centre in world units (256 per square); preciseCenterW, when not
   NULL, is the fractional centre of a MapViewPreciseCam and is used
   instead. viewLen is the view's width or height in screen pixels. The
   first square drawn and the screen-pixel offset into it come back from
   mapViewCameraSplit and are not clamped to the map. */
void mapViewCameraAxis(int centerW, const float *preciseCenterW, int viewLen,
                       int zf, int *outSquare, int *outEdge);

/* Whether square (mx, my) is inside the 256x256 map. The camera draws the
   squares outside it as open deep sea. */
bool mapViewSquareInMap(int mx, int my);

#ifdef __cplusplus
}
#endif

#endif /* SPRITE_POSITIONS_H */
