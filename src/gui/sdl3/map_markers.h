/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Map Markers
 *Filename:      map_markers.h
 *Purpose:
 *  The shapes a thing on the map becomes when it is drawn
 *  too small for its sprite: a triangle pointing the way a
 *  tank faces, a disc for a pillbox, a square for a base.
 *
 *  Shape and stroke only. Where a marker goes, how big it
 *  is and what colour it takes are the caller's - it knows
 *  where its entities are and what it wants to say about
 *  them, and those answers differ between views. The game
 *  colours by allegiance; the log viewer colours by the
 *  player's assigned slot.
 *
 *  A pillbox and a base are told apart by shape rather than
 *  by size or colour, so the two read differently even when
 *  one side owns both.
 *
 *  Every marker is three stacked layers out of
 *  map_colours.h - an outline, the fill darkened, the fill -
 *  with the stroke straddling the shape's edge, so a marker
 *  is the size the caller asked for rather than that size
 *  plus a stroke, and reads against dark ground and light
 *  alike.
 *
 *  Plain SDL, ring_band.h and map_colours.h, and nothing
 *  else - no ClientSim, no ImGui, no game headers - because
 *  the log viewer draws these too.
 *
 *  The blend mode is the caller's: these set none and put
 *  none back, so a caller drawing a run of markers sets
 *  SDL_BLENDMODE_BLEND once around the lot. The outline
 *  layer is translucent, so without it the outline is drawn
 *  opaque black and the markers gain a hard border.
 *********************************************************/

#ifndef WINBOLO_MAP_MARKERS_H
#define WINBOLO_MAP_MARKERS_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
*NAME:          mapMarkerTank
*PURPOSE:
*  A tank, as a triangle pointing the way it faces.
*
*ARGUMENTS:
*  r        - the renderer
*  cx, cy   - the tank's centre, in renderer coordinates
*  radius   - half the marker's height, in pixels
*  facing16 - 0 to 15, the sixteen headings; 0 is north and
*             they run clockwise, which is the order the
*             tank sprite frames are in. Values outside the
*             range wrap, so a raw sprite frame can be
*             passed as it stands.
*  fill     - the marker's colour
*
*RETURNS:
*  none
*********************************************************/
void mapMarkerTank(SDL_Renderer *r, float cx, float cy, float radius,
                   int facing16, SDL_FColor fill);

/*********************************************************
*NAME:          mapMarkerPill
*PURPOSE:
*  A pillbox, as a disc. Every health state draws alike: at
*  these sizes there is no room to show it.
*
*ARGUMENTS:
*  r      - the renderer
*  cx, cy - the pillbox's centre, in renderer coordinates
*  radius - the disc's radius in pixels, before the stroke
*  fill   - the marker's colour
*
*RETURNS:
*  none
*********************************************************/
void mapMarkerPill(SDL_Renderer *r, float cx, float cy, float radius,
                   SDL_FColor fill);

/*********************************************************
*NAME:          mapMarkerBase
*PURPOSE:
*  A base, as a square.
*
*ARGUMENTS:
*  r      - the renderer
*  cx, cy - the base's centre, in renderer coordinates
*  radius - half the square's side in pixels, before the
*           stroke
*  fill   - the marker's colour
*
*RETURNS:
*  none
*********************************************************/
void mapMarkerBase(SDL_Renderer *r, float cx, float cy, float radius,
                   SDL_FColor fill);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_MAP_MARKERS_H */
