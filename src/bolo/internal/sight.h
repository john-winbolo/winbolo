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
 *Filename:      sight.h
 *Purpose:
 *  Which squares of a block can be seen from one square of
 *  the map. A straight line is drawn from that square to
 *  every square of the block, and the square is seen when
 *  the line arrives without passing through anything that
 *  stops it.
 *
 *  Terrain and geometry, nothing else: no regions, no fog,
 *  and nothing kept between calls, so everything that wants
 *  this answer asks here rather than working it out again.
 *
 *  Buildings stop a line outright. Trees stop one by depth:
 *  "you cannot see through trees more than two deep" is read
 *  here as a run of consecutive forest squares along the line,
 *  so a single row of trees is seen through and a stand of
 *  three or more is not. Forest, grass, forest is two one-deep
 *  runs and stops nothing; any square that is not forest puts
 *  the count back to zero. The other reading - every tree on
 *  the line added up however far apart they are - would blind
 *  a player who is looking across scattered ground, so the run
 *  is what is counted.
 *********************************************************/

#ifndef SIGHT_H
#define SIGHT_H

#include "global.h"
#include "types.h"
#include "overview_types.h" /* OverviewRect, OVERVIEW_TANK_HALF */

/* Which terrain stops a line. This is the only place opacity is decided -
 * every test in the module goes through it - so a terrain is added or taken
 * away by changing this one line. */
#define SIGHT_OPAQUE(t) ((t) == BUILDING || (t) == HALFBUILDING)

/* Which terrain counts towards the tree depth, and how many of them in a row a
 * line gets through. Each is the only place its question is decided, so what
 * counts as a tree and how far into one a player sees are one line each. A
 * mined tree is still a tree: the square reads back as MINE_FOREST rather than
 * FOREST, and without the second term laying a mine in a wood would open a
 * one-square hole in the depth count that nobody can see on the map. */
#define SIGHT_TREE(t) ((t) == FOREST || (t) == MINE_FOREST)
#define SIGHT_TREE_MAX_DEPTH 2

/* There is no "off" here: a caller that is not working sight out builds no
 * mask at all rather than asking for one that hides nothing.
 *
 * The widest block anything asks about is the whole scroll envelope, so a
 * caller sizes one buffer from this and reuses it for whatever block it hands
 * over. A narrower block simply leaves the tail of the buffer alone. */
#define SIGHT_MAX_HALF   OVERVIEW_TANK_HALF
#define SIGHT_MAX_SIDE   (2 * SIGHT_MAX_HALF + 1)
#define SIGHT_MASK_BYTES (SIGHT_MAX_SIDE * SIGHT_MAX_SIDE)

/* Writes one byte per square of the block: 1 for a square that can be seen
 * from (originX, originY) and 0 for one that cannot.
 *
 * vis is indexed by the block's own width, not the buffer's:
 *
 *   vis[(y - block->top) * (block->right - block->left + 1) + (x - block->left)]
 *
 * so one buffer of SIGHT_MASK_BYTES serves every block, and a block narrower
 * than the widest one is written with a shorter stride. A block wider or taller
 * than SIGHT_MAX_SIDE, or one with no squares in it, is refused and the buffer
 * is left as it was.
 *
 * The origin square is always seen, whatever is standing on it, and never
 * stops its own lines. Every other square is seen when the line to it arrives
 * without passing through an opaque square and without passing through more
 * than SIGHT_TREE_MAX_DEPTH forest squares in a row: the square at the far end
 * is not tested either way, so a wall is seen and the ground behind it is not,
 * and the tree that stops the line is seen as well. Where two opaque squares
 * touch along an edge the diagonal between them is closed, so sight does not
 * slip through the corner where two buildings meet; trees never close a
 * corner. Squares off the map are never seen. */
void sightBuildMask(map *mp, BYTE originX, BYTE originY,
                    const OverviewRect *block, BYTE *vis);

#endif /* SIGHT_H */
