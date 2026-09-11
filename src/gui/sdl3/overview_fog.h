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
 *                the mask puts it at every zoom. Every rule here
 *                answers per square, so the texture is one texel
 *                to a square and the edges land on square
 *                boundaries.
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

/* Squares the fade takes to reach that, counted outside the live region. At 0
 * a region's edge is its edge: a square the region covers is lit to the
 * region's alpha, the square beside it carries full fog, and there is nothing
 * in between. Put it back to 3 for the fade that spread over three squares of
 * remembered ground outside every region. */
#define OVERVIEW_FOG_RAMP 0

/* The mask is square and row-major — mask[y * OVERVIEW_FOG_MASK_SIDE + x] —
 * because that is the order a texture's rows want it in, and one texel covers
 * one map square. 256x256, so 64 KB of mask and 256 KB written into the
 * texture on each upload. */
#define OVERVIEW_FOG_MASK_SIDE  MAP_ARRAY_SIZE
#define OVERVIEW_FOG_MASK_BYTES (OVERVIEW_FOG_MASK_SIDE * OVERVIEW_FOG_MASK_SIDE)

/* One byte per map square, row-major over the map — the shape dark is handed
 * in, which is the same shape as the mask itself. */
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
 * dark is OVERVIEW_FOG_SQUARE_BYTES, row-major over the map, and darkens rather
 * than lights: the darkest answer wins, so a square it names is fogged whatever
 * a region has said about it. It is what covers ground inside a region the
 * player cannot see into - behind a building, with line of sight on - and NULL
 * is no such ground, which gives the mask the regions alone produce. */
void overviewFogBuildMask(const OverviewRect *live, int liveCount,
                          const BYTE *dark, BYTE *mask);

#ifdef __cplusplus
}
#endif

#endif /* OVERVIEW_FOG_H */
