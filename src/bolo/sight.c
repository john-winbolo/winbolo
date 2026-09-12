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

/* Whether a square is one of the trees the depth count is kept over. Off the
 * map is not: there is nothing out there, and it has stopped the line already
 * by the test above. */
static bool sightIsTree(map *mp, int x, int y) {
  BYTE terrain; /* What is on the square */

  if (sightOnMap(x, y) == FALSE) {
    return FALSE;
  }
  terrain = mapGetPos(mp, (BYTE)x, (BYTE)y);
  return (SIGHT_TREE(terrain) ? TRUE : FALSE);
}

/* Whether a square is close enough to the origin that trees never hide it.
 * Measured on each axis rather than as a distance, so the ground it covers is
 * a box - the shape the tank hide uses, for the same reason. */
static bool sightNearOrigin(int x0, int y0, int x, int y) {
  int dx; /* Squares across, either way round */
  int dy; /* Squares down */

  dx = (x > x0) ? (x - x0) : (x0 - x);
  dy = (y > y0) ? (y - y0) : (y0 - y);
  return (dx <= SIGHT_TREE_NEAR && dy <= SIGHT_TREE_NEAR);
}

/* Walks the line from one square to another and says whether it arrives.
 * Neither end is tested for a building; every square in between is, and the
 * far end is counted as a tree. The walk carries the run of forest squares it
 * has come through back to back, which is what makes the depth a thickness of
 * wood rather than a tally of every tree on the line: one square that is not
 * forest puts it back to zero.
 *
 * The run hides nothing inside SIGHT_TREE_NEAR of the origin, so a wood the
 * player is standing beside is seen into as far as that and stopped at beyond
 * it. Passing over an exempt square leaves the run standing rather than
 * clearing it: the wood is still that deep, and a square further out behind it
 * is still behind it. */
static bool sightLineReaches(map *mp, int x0, int y0, int x1, int y1) {
  int trees;     /* Forest squares passed through in a row */
  bool atTarget; /* Is the walk standing on the square being asked about */
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
  trees = 0;
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
     * sight does not slip between them. Trees are left out of this: the depth
     * rule is about how far into a wood a player sees, and a pair of trees is
     * not a wall. */
    if (stepX != 0 && stepY != 0 &&
        sightBlocks(mp, x + stepX, y) == TRUE &&
        sightBlocks(mp, x, y + stepY) == TRUE) {
      return FALSE;
    }

    x += stepX;
    y += stepY;
    atTarget = (x == x1 && y == y1);

    if (atTarget == FALSE && sightBlocks(mp, x, y) == TRUE) {
      return FALSE;
    }
    if (sightIsTree(mp, x, y) == TRUE) {
      trees++;
      if (trees >= SIGHT_TREE_BLOCK_RUN &&
          sightNearOrigin(x0, y0, x, y) == FALSE) {
        return FALSE;
      }
    } else {
      trees = 0;
    }
    if (atTarget == TRUE) {
      return TRUE;
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
