/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          scenario_marker.h
 * Purpose:       A scenario's map marker as it appears on
 *                the map: the square it names outlined in
 *                the marker's palette colour, with a
 *                pointer above it so a marker on ground the
 *                player is not looking at is still findable,
 *                both breathing slowly so the mark reads as
 *                something a script put there rather than
 *                as terrain. Drawn with the SDL_Renderer as
 *                part of the frame.
 *
 *                Beside ping_marker.h because it is the same
 *                kind of thing and answers to the same rule:
 *                more than one view draws this, so every
 *                choice about how it looks is made in the
 *                draw function and none of it is passed in.
 *                A ping stopped blinking once because one
 *                caller was changed and two were not.
 *
 *                A marker's kind is not here either. SQUARE
 *                and FOLLOW differ in which square they are
 *                on, not in what they look like, and working
 *                out the square is the caller's job — it is
 *                the caller that knows where its view puts
 *                one.
 *********************************************************/

#ifndef WINBOLO_SCENARIO_MARKER_H
#define WINBOLO_SCENARIO_MARKER_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "screentank.h"  /* screenTanks — what a follow marker rides */

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
 *NAME:          scnMarkerTankSquare
 *PURPOSE:
 *  The map square the tank in `slot` is standing on, out of
 *  the list of tanks a view is drawing this frame.
 *
 *  False when that slot has no tank in the list, which is
 *  what keeps a follow marker off the screen when its
 *  player is dead, has not joined, or is somewhere the view
 *  does not reach. Both desktop views prepare the same
 *  screenTanks the tank sprites come out of, so a follow
 *  marker rides exactly the tank the player can see and the
 *  two views cannot disagree about where it is.
 *
 *ARGUMENTS:
 *  tks          - the tanks this view is drawing
 *  slot         - the player slot the marker follows
 *  outMx/outMy  - receive the map square; may be NULL
 *********************************************************/
bool scnMarkerTankSquare(const screenTanks *tks, uint8_t slot,
                         uint8_t *outMx, uint8_t *outMy);

/*********************************************************
 *NAME:          scnMarkerDraw
 *PURPOSE:
 *  Draw one scenario marker. (cx, cy) is the centre of the
 *  map square it names, in the renderer's current
 *  coordinates; tileW/tileH the square's size there; colour
 *  a scenario_panel.h palette index, which is where the
 *  sixteen colours are kept so this and the panel cannot
 *  drift apart.
 *
 *  nowMs is the wall clock, for the breathing. It is a
 *  clock rather than an age because a marker has no arrival
 *  to count from: a script puts one up and takes it down,
 *  and in between it simply is. Every marker on screen
 *  therefore breathes together, which is what makes two of
 *  them read as one scenario's marks.
 *
 *  NO alpha argument, deliberately, and no size or shape
 *  either — see the header comment above.
 *
 *  A colour index that draws nothing (0, or one of the four
 *  reserved entries) draws nothing at all, the same way it
 *  does on the panel.
 *
 *ARGUMENTS:
 *  renderer     - the renderer to draw on
 *  colour       - a ScnPanelColour index
 *  cx/cy        - centre of the square, in this view's pixels
 *  tileW/tileH  - one map square, in this view's pixels
 *  nowMs        - the wall clock
 *********************************************************/
void scnMarkerDraw(SDL_Renderer *renderer, uint8_t colour,
                   float cx, float cy, float tileW, float tileH,
                   unsigned int nowMs);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_SCENARIO_MARKER_H */
