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
 *Name:          Sight
 *Filename:      sight.c
 *Purpose:
 *  Draws a straight line from one square to every square of
 *  a block and reports which of them the line reaches - see
 *  sight.h for the rules it works to.
 *
 *  The line is the integer one Bresenham's algorithm walks,
 *  so a square is either on it or it is not and no square is
 *  half seen. Both ends of a line are left out of the test:
 *  the origin is where the player is standing and the far end
 *  is the square being asked about, so a wall is seen and the
 *  ground behind it is not.
 *
 *  Every line shares the origin, so the question the walk
 *  answers is always "can this square be seen from there" and
 *  never "can these two squares see each other". Bresenham
 *  breaks a tie between two equally good steps one way round
 *  and the other way round it would break it the other, which
 *  would matter to the second question and does not arise in
 *  the first.
 *********************************************************/

#include "global.h"
#include "sight.h"
#include "bolo_map.h"

/* Whether a square is on the map at all. The block is a rect of ints, so it
 * can name a square off the top or left edge, and a coordinate cast into a
 * BYTE there would come back somewhere else entirely. */
static bool sightOnMap(int x, int y) {
  return (x >= 0 && x < MAP_ARRAY_SIZE && y >= 0 && y < MAP_ARRAY_SIZE);
}

/* Whether a square stops a line passing through it. Off the map counts: there
 * is nothing out there to see past. */
static bool sightBlocks(map *mp, int x, int y) {
  BYTE terrain; /* What is on the square */

  if (sightOnMap(x, y) == FALSE) {
    return TRUE;
  }
  terrain = mapGetPos(mp, (BYTE)x, (BYTE)y);
  return (SIGHT_OPAQUE(terrain) ? TRUE : FALSE);
}

/* Walks the line from one square to another and says whether it arrives.
 * Neither end is tested for what is on it; every square the walk passes
 * through in between is. */
static bool sightLineReaches(map *mp, int x0, int y0, int x1, int y1) {
  int dx;    /* Squares across, counted up */
  int dy;    /* Squares down, counted down, so one error term serves both */
  int sx;    /* Which way x moves */
  int sy;    /* Which way y moves */
  int err;   /* The running error the step is chosen from */
  int e2;    /* Twice it, which is what the two tests compare against */
  int stepX; /* What this step adds to x */
  int stepY; /* What this step adds to y */
  int x;     /* Where the walk has got to */
  int y;

  x = x0;
  y = y0;
  dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
  sx = (x0 < x1) ? 1 : -1;
  sy = (y0 < y1) ? 1 : -1;
  err = dx + dy;

  while (x != x1 || y != y1) {
    e2 = 2 * err;
    stepX = 0;
    stepY = 0;
    if (e2 >= dy) {
      err += dy;
      stepX = sx;
    }
    if (e2 <= dx) {
      err += dx;
      stepY = sy;
    }

    /* A step that moves on both axes cuts the corner where the two squares
     * beside it meet. Two buildings touching along that edge close it, so
     * sight does not slip between them. */
    if (stepX != 0 && stepY != 0 &&
        sightBlocks(mp, x + stepX, y) == TRUE &&
        sightBlocks(mp, x, y + stepY) == TRUE) {
      return FALSE;
    }

    x += stepX;
    y += stepY;
    if (x == x1 && y == y1) {
      return TRUE;
    }
    if (sightBlocks(mp, x, y) == TRUE) {
      return FALSE;
    }
  }
  return TRUE;
}

void sightBuildMask(map *mp, BYTE originX, BYTE originY,
                    const OverviewRect *block, BYTE *vis) {
  int width;  /* Squares across the block, which is the mask's stride */
  int height; /* Squares down it */
  int ox;     /* The origin, as the walk counts */
  int oy;
  int x;      /* Looping variable */
  int y;      /* Looping variable */
  BYTE *row;  /* The mask row this square belongs to */
  bool seen;  /* Does the line reach this square */

  if (mp == NULL || block == NULL || vis == NULL) {
    return;
  }

  width = block->right - block->left + 1;
  height = block->bottom - block->top + 1;
  if (width <= 0 || height <= 0 || width > SIGHT_MAX_SIDE ||
      height > SIGHT_MAX_SIDE) {
    return;
  }

  ox = (int)originX;
  oy = (int)originY;
  for (y = block->top; y <= block->bottom; y++) {
    row = vis + (y - block->top) * width;
    for (x = block->left; x <= block->right; x++) {
      if (sightOnMap(x, y) == FALSE) {
        seen = FALSE;
      } else if (x == ox && y == oy) {
        seen = TRUE;
      } else {
        seen = sightLineReaches(mp, ox, oy, x, y);
      }
      row[x - block->left] = (BYTE)((seen == TRUE) ? 1 : 0);
    }
  }
}
