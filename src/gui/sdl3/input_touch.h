/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Touch Input
*Filename:      input_touch.h
*Purpose:
*  Touch input handling for tablet UI mode.
*  Processes SDL finger events into a virtual joystick
*  (left side), shoot button (right lower), mine button
*  (right upper), and build select buttons.
*********************************************************/

#ifndef INPUT_TOUCH_H
#define INPUT_TOUCH_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "../../bolo/global.h"
#include "../../bolo/screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
*NAME:          inputTouchSetup / Cleanup
*PURPOSE:
*  Initialise and tear down touch input state.
*********************************************************/
void inputTouchSetup(void);
void inputTouchCleanup(void);

/*********************************************************
*NAME:          inputTouchProcessEvent
*PURPOSE:
*  Route an SDL finger event to the touch input system.
*  windowW/windowH are the current window dimensions
*  (needed to convert normalised finger coordinates).
*********************************************************/
void inputTouchProcessEvent(SDL_Event *ev, int windowW, int windowH);

/*********************************************************
*NAME:          inputTouchGetMovement
*PURPOSE:
*  Returns the tankButton corresponding to the current
*  joystick direction. TNONE if joystick is inactive.
*********************************************************/
tankButton inputTouchGetMovement(void);

/*********************************************************
*NAME:          inputTouchIsFirePressed
*PURPOSE:
*  Returns true while the shoot button is held.
*********************************************************/
bool inputTouchIsFirePressed(void);

/*********************************************************
*NAME:          inputTouchIsMinePressed
*PURPOSE:
*  Returns true once per mine-button tap (edge-triggered).
*  Consuming: returns true once, then false until released
*  and pressed again.
*********************************************************/
bool inputTouchIsMinePressed(void);

/*********************************************************
*NAME:          inputTouchIsMineHeld
*PURPOSE:
*  Returns true while the mine button finger is down.
*  Non-consuming — for visual feedback only.
*********************************************************/
bool inputTouchIsMineHeld(void);

/*********************************************************
*NAME:          inputTouchGetJoystickState
*PURPOSE:
*  Fills in the joystick visualisation state for the
*  tablet overlay renderer.  All coordinates are in
*  screen pixels.
*  releaseTime is the SDL_GetTicks() timestamp of the
*  last finger-up on the joystick (0 if never released).
*********************************************************/
void inputTouchGetJoystickState(float *anchorX, float *anchorY,
                                float *thumbX, float *thumbY, bool *active,
                                Uint64 *releaseTime);

/*********************************************************
*NAME:          inputTouchTriggerHaptic
*PURPOSE:
*  Triggers a haptic feedback pulse via SDL.
*  strength: 0.0 (none) to 1.0 (max)
*  durationMs: pulse duration in milliseconds
*********************************************************/
void inputTouchTriggerHaptic(float strength, Uint32 durationMs);

/*********************************************************
*NAME:          inputTouchGetBuildSelect
*PURPOSE:
*  Returns >= 0 if a build-select button was tapped
*  this frame (0=tree, 1=road, 2=wall, 3=pill, 4=mine).
*  Returns -1 if no build button was tapped.
*  Consuming: resets after read.
*********************************************************/
int inputTouchGetBuildSelect(void);

/*********************************************************
*NAME:          inputTouchSetViewportBounds
*PURPOSE:
*  Tells the touch system where the game viewport is so
*  that taps inside it can be converted to tile coords.
*********************************************************/
void inputTouchSetViewportBounds(int vpX, int vpY, int vpW, int vpH, int zoom);

/*********************************************************
*NAME:          inputTouchGetViewportTap
*PURPOSE:
*  Returns true if a tap-to-build occurred this frame.
*  Fills tileX/tileY with 1-based viewport-relative tile
*  coordinates (1-15).  Consuming: resets after read.
*********************************************************/
bool inputTouchGetViewportTap(BYTE *tileX, BYTE *tileY);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_TOUCH_H */
