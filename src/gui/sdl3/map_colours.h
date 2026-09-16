/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Map Colours
 *Filename:      map_colours.h
 *Purpose:
 *  The colours a map square gets when it is drawn too
 *  small for its sprite: one flat colour for the ground,
 *  and one palette for the markers that stand in for the
 *  things on it — the tank triangle, the pill disc and the
 *  base square.
 *
 *  Several views need them. The map overview and the full
 *  screen map drop to these below 1x, the map choosers draw
 *  a whole map in a thumbnail, and the log viewer's own view
 *  zooms out past 1x as well. One table, so a square is the
 *  same colour wherever a player sees it small.
 *
 *  A skin may replace any of them through the [MapPalette]
 *  section of its skin.ini; see docs/SKINS.md. Entry by
 *  entry, so a skin that names three colours gets those
 *  three and the built-in rest. Nothing has to be told when
 *  a skin changes: the table notices for itself, by the
 *  skin source's serial.
 *
 *  SDL, the tile numbers and skin_source.h, and nothing
 *  else — no ClientSim, no ImGui, no fonts — because the log
 *  viewer links this too and has none of those.
 *
 *  Why these colours and not minimapTerrainColor's: that set
 *  is the lobby's map preview, a whole map in 256 px where
 *  every terrain has to be told apart at a glance, so its
 *  greens and greys are loud. A square here is eight pixels
 *  or more and sits under markers, pings and the fog, so the
 *  ground is darker and lower contrast and the markers are
 *  what stands out.
 *********************************************************/

#ifndef WINBOLO_MAP_COLOURS_H
#define WINBOLO_MAP_COLOURS_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#include "platform_types.h"  /* BYTE */

#ifdef __cplusplus
extern "C" {
#endif

/* What a square holds, when it holds something a marker is drawn for. The
   one classifier: the ground pass, the marker pass and the item numbers all
   ask this, so they cannot disagree about which squares carry an item. */
typedef enum MapColourItem {
    MAP_COLOUR_ITEM_NONE = 0,
    MAP_COLOUR_ITEM_PILL_GOOD,
    MAP_COLOUR_ITEM_PILL_EVIL,
    MAP_COLOUR_ITEM_BASE_GOOD,
    MAP_COLOUR_ITEM_BASE_EVIL,
    MAP_COLOUR_ITEM_BASE_NEUTRAL
} MapColourItem;

/*********************************************************
*NAME:          mapColourItemKind
*PURPOSE:
*  Whether this tile is a pillbox or a base, and whose.
*  Every pill health state answers alike.
*
*ARGUMENTS:
*  tile - the tile number drawn on the square
*
*RETURNS:
*  the item on the square, MAP_COLOUR_ITEM_NONE for ground
*********************************************************/
MapColourItem mapColourItemKind(BYTE tile);

/*********************************************************
*NAME:          mapColourTerrain
*PURPOSE:
*  The flat colour for a square's ground.
*
*  Either kind of number is accepted, because the two kinds
*  of caller hold different ones and neither should have to
*  convert.
*
*  A view drawing a live game hands in what a sprite pass
*  would have drawn: viewportCalcSquarePure turns ROAD,
*  BUILDING, FOREST, RIVER, DEEP_SEA, BOAT and CRATER into
*  their shape-variant ranges in tilenum.h and passes
*  everything else through, so from there the raw terrains
*  that arrive are SWAMP, RUBBLE, GRASS and HALFBUILDING.
*
*  A caller reading a map file - the map choosers - hands in
*  raw terrain throughout, including BUILDING, RIVER, ROAD,
*  BOAT and DEEP_SEA, and may hand in a mined square, which
*  answers with the ground under the mine. None of those
*  numbers is also a drawn tile, so covering both kinds costs
*  nothing in ambiguity, and a family's two numbers always
*  answer with the same colour.
*
*  A pill or base square answers with the ground under it —
*  grass and road respectively — because its marker is drawn
*  on top afterwards.
*
*  Note that tile numbers are not one space: the tank frames
*  overlap the road range, because a tank is drawn from a
*  different sheet. Nothing here can tell them apart, and
*  nothing needs to — tanks reach a view through its own
*  entity list, never through a square.
*
*ARGUMENTS:
*  tile - the tile number drawn on the square
*  out  - written with the colour, opaque; untouched on false
*
*RETURNS:
*  false for a tile the table has no entry for, which the
*  caller should draw from the sprite sheet as it would at
*  any other zoom
*********************************************************/
bool mapColourTerrain(BYTE tile, SDL_Color *out);

/* The marker palette, by allegiance, after any skin override. Neutral bases
   are amber; a neutral pill is nobody's, so it takes the evil colour rather
   than a fourth one. */
SDL_FColor mapColourMarkerGood(void);
SDL_FColor mapColourMarkerEvil(void);
SDL_FColor mapColourMarkerNeutral(void);

/* Every marker is drawn as three stacked layers, largest first, so a shape
   on a dark square and a shape on a light one are both outlined the same
   way: an outline MAP_COLOUR_STROKE_PX / 2 outside the shape at
   MAP_COLOUR_OUTLINE_ALPHA, then the fill darkened by MAP_COLOUR_DARKEN over
   the inner half of the stroke, then the plain fill. */
#define MAP_COLOUR_STROKE_PX     1.5f
#define MAP_COLOUR_OUTLINE_ALPHA 0.65f
#define MAP_COLOUR_DARKEN        0.35f

/*********************************************************
*NAME:          mapColourMarkerShades
*PURPOSE:
*  The three layers' colours, outermost first, for a marker
*  filled `fill`.
*
*ARGUMENTS:
*  fill - the marker's own colour
*  out  - written with the three layers
*
*RETURNS:
*  none
*********************************************************/
void mapColourMarkerShades(SDL_FColor fill, SDL_FColor out[3]);

/*********************************************************
*NAME:          mapColourMarkerLayerGrow
*PURPOSE:
*  How far outside the shape layer `layer` (0..2) reaches,
*  in pixels. The stroke straddles the shape's edge rather
*  than sitting outside it, so the three answers are half the
*  stroke out, the shape itself, and half the stroke in, so a
*  marker is the size the caller asked for rather than that
*  size plus a stroke.
*
*  A layer outside 0..2 clamps to the nearest, so a caller's
*  loop bound cannot turn into a stray offset.
*
*ARGUMENTS:
*  layer - 0, 1 or 2
*
*RETURNS:
*  pixels to grow the shape by before drawing that layer;
*  negative for the innermost
*********************************************************/
float mapColourMarkerLayerGrow(int layer);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_MAP_COLOURS_H */
