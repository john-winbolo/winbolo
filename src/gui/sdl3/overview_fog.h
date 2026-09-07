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
 *                live region and full fog outside one.
 *
 *                The renderer hands the whole mask to the GPU
 *                as a square texture stretched over the map and
 *                samples it nearest, so the boundary lands where
 *                the mask puts it at every zoom. There are
 *                OVERVIEW_FOG_SUB texels to a square along each
 *                axis: the regions and the per-square passes fill
 *                a square's texels all alike, and only the
 *                Headlights beam is worked out texel by texel,
 *                which is what turns its edge from a staircase of
 *                whole squares into a straight line.
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
#include "overview_types.h" /* OverviewRect, OverviewBeam, overviewBeamLights */

/* Fog over ground the player is not looking at, as an alpha blended over the
 * terrain. 145 leaves 110/255 of the colour through, which is the multiply
 * the two-pass renderer used to dim remembered squares with. */
#define OVERVIEW_FOG_ALPHA 145

/* Squares the fade takes to reach that, counted outside the live region. At 0
 * a region's edge is its edge: a square the region covers is lit to the
 * region's alpha, the square beside it carries full fog, and there is nothing
 * in between. Put it back to 3 for the fade that spread over three squares of
 * remembered ground outside every region. */
#define OVERVIEW_FOG_RAMP 0

/* Texels to a map square along each axis. Raise it to 8 here alone if 4 still
 * reads as steps along the Headlights beam; everything below is worked out
 * from it, and the memory goes up with its square. */
#define OVERVIEW_FOG_SUB 4

/* The mask is square and row-major — mask[ty * OVERVIEW_FOG_MASK_SIDE + tx] —
 * because that is the order a texture's rows want it in. Map square x owns the
 * OVERVIEW_FOG_SUB texels from x * OVERVIEW_FOG_SUB, and the same in y.
 *
 * At a sub of 4 that is 1024x1024, so a megabyte of mask, and four megabytes
 * more written into the texture on each upload. */
#define OVERVIEW_FOG_MASK_SIDE  (MAP_ARRAY_SIZE * OVERVIEW_FOG_SUB)
#define OVERVIEW_FOG_MASK_BYTES (OVERVIEW_FOG_MASK_SIDE * OVERVIEW_FOG_MASK_SIDE)

/* One byte per map square, row-major over the map — the shape lift and dark
 * are handed in, which stays per-square whatever the mask's resolution is. */
#define OVERVIEW_FOG_SQUARE_BYTES (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)

#ifdef __cplusplus
extern "C" {
#endif

/* Writes OVERVIEW_FOG_MASK_BYTES bytes: 0 for a square inside any of the
 * `liveCount` regions and OVERVIEW_FOG_ALPHA for a square outside every one of
 * them. With OVERVIEW_FOG_RAMP at 0 there is nothing in between; a ramp of more
 * than 0 puts a smoothstep of the Euclidean distance to the nearest region over
 * that many squares outside it, which rounds the corners the regions are square
 * at. A region's alpha scales its brightness: 255 gives the values above, a
 * lower one lifts every square it covers less far out of the fog, and 0 reads
 * as if the region were not in the list. No live regions at all is a legitimate
 * call and fogs the whole map.
 *
 * The regions, lift and dark are all per-square: a region's edges land on
 * square boundaries, and the two arrays carry one byte a square. Each of them
 * writes every texel of the square it is about, so all three draw exactly what
 * they drew when the mask was one byte to a square. The beam is the one thing
 * asked about texel by texel.
 *
 * lift is OVERVIEW_FOG_SQUARE_BYTES, row-major over the map, and lifts each
 * square out of the fog on its own account after the regions have had their
 * say: 255 leaves it as clear as a live square, 0 leaves it as the regions left
 * it, and the brightest answer wins, so no square a region holds is ever
 * darkened by one. It is what draws the ground an Afterimage block has left and
 * has not finished fading; NULL is no such ground, and gives the mask the
 * regions alone produce.
 *
 * dark works the other way round: OVERVIEW_FOG_SQUARE_BYTES, row-major again,
 * and the darkest answer wins, so a square it names is fogged whatever a region
 * or a lift has said about it. It is what covers ground inside a region the
 * player cannot see into - behind a building, with line of sight on - and NULL
 * is no such ground.
 *
 * beam is the Headlights beam the map's latest update built, and NULL or one
 * with active false is no beam. Where there is one, every texel inside its
 * block that the beam does not light is pushed to full fog - the same thing
 * dark does for a square, at texel resolution, and it only ever darkens.
 * Nothing outside the block is touched by it, so the per-texel work is at most
 * the block's own 29x29 squares.
 *
 * With lift and dark NULL and no beam the mask is the one the regions alone
 * produce, byte for byte. */
void overviewFogBuildMask(const OverviewRect *live, int liveCount,
                          const BYTE *lift, const BYTE *dark,
                          const OverviewBeam *beam, BYTE *mask);

#ifdef __cplusplus
}
#endif

#endif /* OVERVIEW_FOG_H */
