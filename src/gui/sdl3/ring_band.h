/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Ring Band
 *Filename:      ring_band.h
 *Purpose:
 *  The ring that closes onto a spot on the map, and the two
 *  pieces every drawer of one needs: the band itself, as a
 *  run of triangles between two circles, and the curve it
 *  travels along.
 *
 *  Two things use it. The map overview closes a yellow one
 *  onto the tank on a respawn, and draws the soft edge of
 *  that effect's spotlight out of the same band. The smart
 *  ping's world marker closes one in the ping's own colour
 *  as the ping lands.
 *
 *  Plain SDL and nothing else — no ClientSim, no ImGui, no
 *  game headers — because the ping marker that calls it is
 *  also compiled into the log viewer, which has neither.
 *
 *  Radii are PIXELS here. Both callers keep their own radii
 *  in map squares, so the ring is the same size in map terms
 *  at every zoom, and each multiplies by its own tile size on
 *  the way in. ringAnimAt is the exception: it is pure
 *  arithmetic on whatever units its RingAnim carries, and
 *  hands them back unchanged.
 *********************************************************/

#ifndef WINBOLO_RING_BAND_H
#define WINBOLO_RING_BAND_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sides of a drawn circle, from its radius: enough of them that a side is
 * about this many pixels long, so a ring stays round when it opens at 4x zoom
 * instead of turning into a polygon, and does not spend a hundred segments on
 * the small one it ends as. The ends are a floor that keeps a tiny ring from
 * going lumpy and a ceiling on ringBandDraw's vertex array. */
#define RING_BAND_SIDE_PX  4.0f
#define RING_BAND_MIN_SEG  24
#define RING_BAND_MAX_SEG  128

/*********************************************************
*NAME:          ringBandSegments
*PURPOSE:
*  Sides for a circle of this radius in pixels: see
*  RING_BAND_SIDE_PX. Always between RING_BAND_MIN_SEG and
*  RING_BAND_MAX_SEG, so the result is always safe to hand
*  straight to ringBandDraw.
*
*ARGUMENTS:
*  radiusPx - the circle's radius in pixels
*
*RETURNS:
*  the number of sides to draw it with
*********************************************************/
int ringBandSegments(float radiusPx);

/*********************************************************
*NAME:          ringBandDraw
*PURPOSE:
*  A band between two circles, as one run of triangles: a
*  ring is a narrow one, and the dim outside the overview's
*  respawn spotlight is a pair of wide ones in black. The two
*  alphas are the band's inner and outer edge, so a band can
*  ramp from clear to solid across its width — which is the
*  soft edge of that spotlight — or carry one alpha on both
*  and come out flat.
*
*  Not concentric one-pixel lines. Lines a pixel apart leave
*  hairlines through a band of any width, where the rasteriser
*  steps a segment across a row, and closing those by
*  overlapping the runs would blend the band onto itself at
*  whatever alpha it is drawn at. Triangles cover the band
*  once, and carry their own colour: SDL_RenderGeometry
*  ignores the draw colour, so the fades ride on the vertices
*  instead.
*
*  The blend mode is the caller's: this sets none and puts
*  none back, so a caller drawing several bands sets
*  SDL_BLENDMODE_BLEND once around the lot.
*
*  Nothing is drawn for a band with no width, for fewer than
*  three sides, or when the renderer is NULL. More sides than
*  RING_BAND_MAX_SEG are clamped to it rather than running off
*  the vertex array.
*
*ARGUMENTS:
*  r          - the renderer
*  cx, cy     - the band's centre, in the renderer's own
*               coordinates
*  innerPx    - inner edge radius in pixels; under 0 is read
*               as 0, which makes the band a filled disc
*  outerPx    - outer edge radius in pixels
*  segments   - sides to build it from, from ringBandSegments
*  red/green/blue - the colour, on both edges
*  innerAlpha - 0-255 alpha at the inner edge
*  outerAlpha - 0-255 alpha at the outer edge
*
*RETURNS:
*  none
*********************************************************/
void ringBandDraw(SDL_Renderer *r, float cx, float cy,
                  float innerPx, float outerPx, int segments,
                  Uint8 red, Uint8 green, Uint8 blue,
                  Uint8 innerAlpha, Uint8 outerAlpha);

/* The shape of a closing ring, in whatever radius unit the caller keeps its
 * own constants in — map squares for both of the callers in this tree.
 *
 * Two stages. The close runs from startRadius in to endRadius over closeMs;
 * then the pulse opens back out to pulseRadius and shuts again over pulseMs,
 * fading as it goes. The pulse is what keeps the ring from ending on a cut. */
typedef struct {
    float        startRadius;  /* where the close begins */
    float        endRadius;    /* where it settles, and where the pulse starts */
    float        pulseRadius;  /* how far the pulse reopens */
    unsigned int closeMs;      /* the close */
    unsigned int pulseMs;      /* the pulse, and the fade with it */
} RingAnim;

/*********************************************************
*NAME:          ringAnimAt
*PURPOSE:
*  Where a closing ring is, and how solid, after this long.
*  A pure function of the elapsed time — no wall clock and no
*  state — so a caller driving it from something recorded
*  (a ping's age) replays exactly what was shown live.
*
*  The close is eased out, cubically: away quickly and
*  settling onto the target, rather than arriving at the speed
*  it left. The pulse is a half sine, so it opens and shuts
*  without a corner at the top.
*
*ARGUMENTS:
*  anim      - the ring's shape, or NULL for nothing to draw
*  elapsedMs - milliseconds since the ring started
*  radius    - written with the radius now, in the unit
*              `anim` carries; NULL to skip
*  alpha     - written with 0-1 opacity now: 1 for the whole
*              close, then a straight ramp to 0 across the
*              pulse; NULL to skip
*
*RETURNS:
*  true while the ring is still running, false once
*  closeMs + pulseMs have passed — at which point neither
*  output is written and there is nothing left to draw
*********************************************************/
bool ringAnimAt(const RingAnim *anim, Uint64 elapsedMs,
                float *radius, float *alpha);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_RING_BAND_H */
