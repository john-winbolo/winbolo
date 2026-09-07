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
 *Name:          Facing Table
 *Filename:      facing_table.h
 *Purpose:
 *  The direction a 16-step facing points in, as a pair of
 *  tables. Shared by everything that has to put something
 *  in front of or behind a tank, so there is one copy of
 *  the numbers rather than one per module.
 *********************************************************/

#ifndef FACING_TABLE_H
#define FACING_TABLE_H

/* Facing unit vectors (sin/cos × 256), 16-step BRADIANS index.
 * Used for the autoscroll forward-bias term and the parked-rear hemisphere
 * test in scroll.c. Callers scale by the distance they want and divide by 256
 * to come back to whole squares. */
static const int kForwardX[16] = {
     0,   98,  181,  237,  256,  237,  181,   98,
     0,  -98, -181, -237, -256, -237, -181,  -98
};
static const int kForwardY[16] = {
  -256, -237, -181,  -98,    0,   98,  181,  237,
   256,  237,  181,   98,    0,  -98, -181, -237
};

#endif /* FACING_TABLE_H */
