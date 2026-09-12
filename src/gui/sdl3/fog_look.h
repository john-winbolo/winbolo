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
 * Name:          fog_look.h
 * Purpose:       What fog over unseen ground looks like.
 *
 *   The two views that draw the world draw the fog their
 *   own way — the classic view washes over the tiles it has
 *   just put down, the full screen map blends a mask texture
 *   over the whole terrain — but they have to agree on the
 *   colour and the strength or the same ground reads as two
 *   different amounts of hidden. That agreement lives here.
 *
 *   The fog is a blend towards a colour rather than a
 *   dimming, because the terrain art gives a dimming nothing
 *   to work on: road_solid is 256 black pixels out of 256,
 *   and grass is 224 out of 256 with the rest green speckle.
 *   Multiplying black by anything leaves black, so a road
 *   under the old fog was pixel for pixel a road in plain
 *   sight. Blending lifts those black pixels instead, which
 *   is most of every square.
 *
 *   Fogged ground therefore comes out lighter than lit
 *   ground, not darker. That is the only direction with any
 *   room in it.
 *********************************************************/

#ifndef FOG_LOOK_H
#define FOG_LOOK_H

/* The colour unseen ground is taken towards. Neutral grey: the terrain that
 * does carry colour is green, cyan and blue, and a fog with any blue in it
 * reads as water over land. */
#define FOG_LOOK_R 96
#define FOG_LOOK_G 96
#define FOG_LOOK_B 96

/* How far towards it, out of 255. At 145 a black square settles at 55 grey —
 * plainly not black — while terrain bright enough to have a colour keeps it,
 * washed. Raise it for thicker fog; raise the colour above for a lighter
 * fog that dims the bright terrain no further. */
#define FOG_LOOK_ALPHA 145

#endif /* FOG_LOOK_H */
