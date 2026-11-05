/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer hit boxes sums
 *Filename:      lv_hitbox.h
 *Purpose:
 *  The areas the Log Viewer draws round each tank and each
 *  builder on the map (Options > Toggle Hit Boxes; draw.c
 *  draws them). They are the engine's own tests, written
 *  again here in the viewer's terms:
 *
 *  - A tank: tankShellInHitZone (src/bolo/tank.c). A shell
 *    hits a tank when the shell's centre is less than the
 *    tank_hit_radius rule from the tank's centre. A circle.
 *
 *  - A builder: lgmBlastCatches (src/bolo/lgm.c). An
 *    explosion catches a builder when it is on a square that
 *    is not a building, a half building, a pillbox or a base,
 *    and no more than half a square (128) from him. On one of
 *    those squares it catches him only when it is on his own
 *    square. The distance comes from utilIsItemInRange with
 *    the pill_range rule as its limit, so when pill_range is
 *    under 128 the circle is pill_range across, not 128.
 *
 *  Sizes are in world units: 256 to a map square, 16 to a
 *  pixel of the map's art.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_HITBOX_H
#define LV_HITBOX_H

#include <stdbool.h>

/* World units in one pixel of the map's art (TANK_SHIFT_PIXELSIZE). */
#define LV_HITBOX_WU_PER_PIXEL 16
/* World units in one map square. */
#define LV_HITBOX_WU_PER_SQUARE 256
/* Half a square (MAP_SQUARE_MIDDLE): the builder's catch distance on
   open ground. */
#define LV_HITBOX_BUILDER_REACH 128
/* lgmGetMX / lgmGetPX (src/bolo/lgm.c) take 1 world unit off a walking
   builder's x and 2 off his y before they round his place down to an art
   pixel, which is the place a recording holds. */
#define LV_HITBOX_LGM_OFFSET_X 1
#define LV_HITBOX_LGM_OFFSET_Y 2
/* Half the tank picture, in world units (TANK_SUBTRACT). The game draws a
   tank's 16 by 16 picture with its top left this far up and left of the
   tank's place (playersMakeScreenTanks, tankGetScreenMX), so the picture's
   middle is the tank's place. */
#define LV_HITBOX_TANK_HALF 128
/* Where the game puts the top left of a walking builder's 3 by 4 picture, in
   art pixels from the art pixel his recorded place is in. spritePositionLgm
   (src/gui/sdl3/sprite_positions.c) takes 1.5 across and 1.5 down off and
   then rounds to a whole art pixel, which gives 1 and 1 in every drawing
   style but Smooth. The viewer draws whole art pixels, as Classic does. */
#define LV_HITBOX_LGM_PICTURE_X (-1)
#define LV_HITBOX_LGM_PICTURE_Y (-1)

/* A recording holds a place rounded down to a whole art pixel, so the true
   place is somewhere in the 16 world units from that pixel's top left. These
   give the middle of those 16, the closest guess, in world units from the
   top left of the square. pixel is the 0 to 15 art pixel in the square.

   The tank: tankGetPX gives his x to the art pixel, so he is 0 to 15 units
   into it. */
static inline double lvHitboxTankPlace(int pixel) {
    return (double)(pixel * LV_HITBOX_WU_PER_PIXEL) +
           (LV_HITBOX_WU_PER_PIXEL - 1) / 2.0;
}
/* The builder: lgmGetPX and lgmGetPY take 1 off his x and 2 off his y first,
   so he is 1 to 16 units into the pixel across and 2 to 17 down. */
static inline double lvHitboxBuilderPlaceX(int pixel) {
    return lvHitboxTankPlace(pixel) + LV_HITBOX_LGM_OFFSET_X;
}
static inline double lvHitboxBuilderPlaceY(int pixel) {
    return lvHitboxTankPlace(pixel) + LV_HITBOX_LGM_OFFSET_Y;
}

/* The tank test, as tankShellInHitZone makes it: the shell's centre less
   than radius from the tank's centre, on both axes and as a distance. dx and
   dy are the shell's place less the tank's, in world units. */
static inline bool lvHitboxTankHit(int dx, int dy, int radius) {
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    return ax < radius && ay < radius && (dx * dx + dy * dy) < radius * radius;
}

/* The distance an explosion on open ground catches a builder at. The engine
   asks for distance <= 128, but the distance it reads is the one
   utilIsItemInRange gives with pill_range as its limit: past the limit it
   gives WORLD_MAX, which never passes. So the catch distance is the smaller
   of the two. */
static inline int lvHitboxBuilderReach(int pillRange) {
    if (pillRange < 0) return 0;
    return pillRange < LV_HITBOX_BUILDER_REACH ? pillRange : LV_HITBOX_BUILDER_REACH;
}

/* The builder test, as lgmBlastCatches makes it, for an explosion at (x, y)
   and a builder at (bx, by), both in world units measured from the top left
   corner of the square the builder is on. So the builder's square is the one
   from 0 to 255 on both axes, and an explosion's square is found by dividing
   by 256 (rounding down). solid is a mask of the 3 by 3 squares round the
   builder's: bit (row * 3 + col), with col and row 0, 1 or 2 and the
   builder's own square at col 1, row 1. A set bit is a building, a half
   building, a pillbox or a base. reach is lvHitboxBuilderReach's.

   An explosion off the 3 by 3 squares is further than 128 from the builder,
   so it never catches him: its square is not looked at. */
static inline bool lvHitboxBuilderCaught(double x, double y, double bx, double by,
                                         unsigned solid, int reach) {
    double dx = x - bx;
    double dy = y - by;
    int col, row;
    bool isSolid;

    col = (x < 0.0) ? 0 : (x < (double)LV_HITBOX_WU_PER_SQUARE) ? 1 :
          (x < 2.0 * LV_HITBOX_WU_PER_SQUARE) ? 2 : 3;
    row = (y < 0.0) ? 0 : (y < (double)LV_HITBOX_WU_PER_SQUARE) ? 1 :
          (y < 2.0 * LV_HITBOX_WU_PER_SQUARE) ? 2 : 3;
    if (x < -(double)LV_HITBOX_WU_PER_SQUARE || y < -(double)LV_HITBOX_WU_PER_SQUARE ||
        col > 2 || row > 2) {
        return false;
    }
    isSolid = ((solid >> (row * 3 + col)) & 1u) != 0;
    if (isSolid) {
        return col == 1 && row == 1;
    }
    return (dx * dx + dy * dy) <= (double)reach * (double)reach;
}

#endif /* LV_HITBOX_H */
