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
 * Name:          overview_fog.h
 * Purpose:       How dark the map overview draws each square,
 *                given the regions the player can see this
 *                instant: one byte per square, clear inside a
 *                live region and rising to full fog over the
 *                few squares outside one.
 *
 *                The renderer hands the whole mask to the GPU
 *                as a 256x256 texture stretched over the map,
 *                so bilinear filtering shades between the
 *                bytes and the fade comes out smooth at every
 *                zoom instead of stepping square by square.
 *
 *                Pure state in, bytes out — no SDL, so it
 *                compiles into the unit-test binary the way
 *                overview_camera.cpp does.
 *********************************************************/

#ifndef OVERVIEW_FOG_H
#define OVERVIEW_FOG_H

/* windows.h — reached through WinSock2 — has to be seen at the default
 * packing, for the reason spelled out in overview_camera.h. */
#ifdef _WIN32
#include <WinSock2.h>
#endif

#include "types.h"          /* BYTE, MAP_ARRAY_SIZE */
#include "overview_types.h" /* OverviewRect */

/* Fog over ground the player is not looking at, as an alpha blended over the
 * terrain. 145 leaves 110/255 of the colour through, which is the multiply
 * the two-pass renderer used to dim remembered squares with. */
#define OVERVIEW_FOG_ALPHA 145

/* Squares the fade takes to go from clear to that. The ramp lies entirely
 * outside the live region: a live square is always fully clear, so full
 * brightness still means "the client has this square live" and the softening
 * spends itself on remembered ground. */
#define OVERVIEW_FOG_RAMP 3

/* Row-major — mask[y * MAP_ARRAY_SIZE + x] — because that is the order a
 * texture's rows want it in. */
#define OVERVIEW_FOG_MASK_BYTES (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)

#ifdef __cplusplus
extern "C" {
#endif

/* Writes OVERVIEW_FOG_MASK_BYTES bytes: 0 for a square inside any of the
 * `liveCount` regions, OVERVIEW_FOG_ALPHA for one more than OVERVIEW_FOG_RAMP
 * squares away from all of them, and a smoothstep of the Euclidean distance to
 * the nearest region in between — which is what rounds the corners the regions
 * are square at. A region's alpha scales its brightness: 255 gives the values
 * above, a lower one lifts every square it covers less far out of the fog, and
 * 0 reads as if the region were not in the list. No live regions at all is a
 * legitimate call and fogs the whole map.
 *
 * lift is one byte per square, row-major over the map the way the mask is, and
 * lifts each square out of the fog on its own account after the regions have
 * had their say: 255 leaves it as clear as a live square, 0 leaves it as the
 * regions left it, and the brightest answer wins, so no square a region holds
 * is ever darkened by one. It is what draws the ground an Afterimage block has
 * left and has not finished fading; NULL is no such ground, and gives the mask
 * the regions alone produce. */
void overviewFogBuildMask(const OverviewRect *live, int liveCount,
                          const BYTE *lift, BYTE *mask);

#ifdef __cplusplus
}
#endif

#endif /* OVERVIEW_FOG_H */
