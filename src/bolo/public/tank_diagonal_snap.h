/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TANK_DIAGONAL_SNAP_H
#define TANK_DIAGONAL_SNAP_H

/* Where a tank facing one of the four diagonals is drawn, in whole game
 * pixels.
 *
 * A tank is drawn at (x >> 4, y >> 4): each axis is cut down to its game
 * pixel on its own. On a diagonal both axes move by the same amount each
 * tick, but their sub-pixel parts are almost never the same (any turn,
 * wall slide or bump leaves them apart), so the two axes cross their pixel
 * lines on different ticks — and, with render-clock interpolation, on
 * different render frames. The sprite then walks a staircase: one pixel up,
 * then one pixel across. At grass speed (8 world units a tick per axis) with
 * the axes half a pixel apart it is a perfect up, across, up, across.
 *
 * This puts the drawn position on the diagonal pixel lattice instead. For a
 * tank heading along (1,-1) (NE or SW) the sum x + y does not change as it
 * drives, so the drawn pixel sum is taken from it once; the difference x - y
 * is what grows, and it is rounded to the nearest value of the same parity
 * as the sum. Every drawn step then moves both axes by one pixel together.
 * (1,1) (SE or NW) is the same with sum and difference swapped. The drawn
 * pixel stays within one pixel of the tank's true position, and the speed
 * in pixels per tick is unchanged: the steps come as often as before, just
 * on both axes at once.
 *
 * A position already on the lattice maps to itself, so a position snapped
 * once (the other tanks, when their interpolated position is cut down to a
 * pixel in client_snapshot.c) is left alone when the drawer snaps it again.
 *
 * Only the drawing reads this. The sim, the wire, replays and brains all
 * keep the tank's real position.
 *
 * dir16 is the 16-step facing (0 = north, 2 = NE, 6 = SE, 10 = SW, 14 = NW).
 * x and y are world co-ordinates (256 to a square, 16 to a game pixel) and
 * are replaced by the snapped ones, whose low four bits are zero. Any other
 * facing leaves them untouched. */

/* Floor division for a possibly negative numerator and a positive divisor. */
static inline int tankDiagonalFloorDiv(int n, int d) {
  int q = n / d;
  if ((n % d) != 0 && n < 0) {
    q--;
  }
  return q;
}

/* The 16-step facing of a 0-255 angle, with the same buckets as utilGetDir. */
static inline int tankDiagonalDir16(int angle) {
  return ((angle + 7) >> 4) & 15;
}

static inline void tankDiagonalSnap(int dir16, int *x, int *y) {
  int sum;   /* The pixel sum or difference that does not change as it drives */
  int half;  /* Half the one that does, rounded to the lattice */

  if (dir16 == 2 || dir16 == 10) {
    /* NE / SW: x + y holds, x - y grows. */
    sum = tankDiagonalFloorDiv(*x + *y + 8, 16);
    half = tankDiagonalFloorDiv(*x - *y - 16 * sum + 16, 32);
    *x = (sum + half) * 16;
    *y = -half * 16;
  } else if (dir16 == 6 || dir16 == 14) {
    /* SE / NW: x - y holds, x + y grows. */
    sum = tankDiagonalFloorDiv(*x - *y + 8, 16);
    half = tankDiagonalFloorDiv(*x + *y - 16 * sum + 16, 32);
    *x = (sum + half) * 16;
    *y = half * 16;
  }
}

#endif /* TANK_DIAGONAL_SNAP_H */
