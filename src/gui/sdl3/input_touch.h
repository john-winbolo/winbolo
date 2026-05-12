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
*  (left side) and hit-tested action buttons (right side).
*********************************************************/

#ifndef INPUT_TOUCH_H
#define INPUT_TOUCH_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "global.h"
#include "tank.h"  /* tankButton + TNONE/TACCEL/... */

#ifdef __cplusplus
extern "C" {
#endif

/* Button IDs for the hit-testing system */
typedef enum {
  TOUCH_BTN_FIRE = 0,
  TOUCH_BTN_MINE,
  TOUCH_BTN_GS_INCREASE,
  TOUCH_BTN_GS_DECREASE,
  TOUCH_BTN_PILL_VIEW,
  TOUCH_BTN_TANK_VIEW,
  TOUCH_BTN_COUNT
} TouchButtonID;

void inputTouchSetup(void);
void inputTouchCleanup(void);

void inputTouchProcessEvent(SDL_Event *ev, int windowW, int windowH);

/*********************************************************
*NAME:          inputTouchRegisterButton
*PURPOSE:
*  Register a circular button for hit-testing.
*  Called each frame from the layout config.
*********************************************************/
void inputTouchRegisterButton(TouchButtonID id, float centerX, float centerY, float radius);

/*********************************************************
*NAME:          inputTouchRegisterRectButton
*PURPOSE:
*  Register a rectangular button for hit-testing.
*********************************************************/
void inputTouchRegisterRectButton(TouchButtonID id, float x, float y, float w, float h);

/*********************************************************
*NAME:          inputTouchClearButtons
*PURPOSE:
*  Clear all registered buttons. Called at start of frame.
*********************************************************/
void inputTouchClearButtons(void);

/*********************************************************
*NAME:          inputTouchIsButtonHeld
*PURPOSE:
*  Returns true while a button is held down (continuous).
*********************************************************/
bool inputTouchIsButtonHeld(TouchButtonID id);

/*********************************************************
*NAME:          inputTouchIsButtonTapped
*PURPOSE:
*  Returns true once per tap (edge-triggered, consuming).
*********************************************************/
bool inputTouchIsButtonTapped(TouchButtonID id);

tankButton inputTouchGetMovement(void);

/*********************************************************
*NAME:          inputTouchSetTankAngle
*PURPOSE:
*  Provides the tank's current 0-255 direction to the touch
*  input system for absolute steering calculations.
*  Called each frame before inputTouchGetMovement().
*********************************************************/
void inputTouchSetTankAngle(BYTE angle);

/*********************************************************
*NAME:          inputTouchSetAbsoluteSteering / Get
*PURPOSE:
*  Controls whether the joystick uses absolute steering
*  (point-to-face) or relative steering (left/right to turn).
*  Absolute is the default for tablet mode.
*********************************************************/
void inputTouchSetAbsoluteSteering(bool enabled);
bool inputTouchGetAbsoluteSteering(void);

void inputTouchGetJoystickState(float *anchorX, float *anchorY,
                                float *thumbX, float *thumbY, bool *active,
                                Uint64 *releaseTime);

/*********************************************************
*NAME:          inputTouchSetScrollJoystickZone
*PURPOSE:
*  Register the rectangular zone for the scroll joystick.
*  Finger-down inside this rect activates the scroll stick.
*********************************************************/
void inputTouchSetScrollJoystickZone(float x, float y, float w, float h);

/*********************************************************
*NAME:          inputTouchGetScrollDirection
*PURPOSE:
*  Returns the scroll direction from the scroll joystick.
*  Sets scrollX/scrollY to -1, 0, or +1.
*  Returns true if the scroll joystick is active.
*********************************************************/
bool inputTouchGetScrollDirection(int *scrollX, int *scrollY);

void inputTouchGetScrollJoystickState(float *anchorX, float *anchorY,
                                      float *thumbX, float *thumbY, bool *active,
                                      Uint64 *releaseTime);

void inputTouchTriggerHaptic(float strength, Uint32 durationMs);

/* Gunsight change: returns 1 for increase, -1 for decrease, 0 for none.
   Consuming — resets after read. */
int inputTouchGetGunsightChange(void);

/* Legacy API kept for build select (handled by ImGui) */
int inputTouchGetBuildSelect(void);

/*********************************************************
*NAME:          inputTouchConsumeTapInRect
*PURPOSE:
*  Returns true if a tap occurred in the given rectangle
*  since last call. Consuming — only one rect can claim it.
*********************************************************/
bool inputTouchConsumeTapInRect(float x, float y, float w, float h);

void inputTouchSetViewportBounds(int vpX, int vpY, int vpW, int vpH, int zoom);
bool inputTouchGetViewportTap(BYTE *tileX, BYTE *tileY);

/*********************************************************
*NAME:          inputTouchGetViewportDragScroll
*PURPOSE:
*  Returns the scroll direction from dragging on the viewport.
*  Uses natural scrolling (drag right → scroll left).
*  Sets scrollX/scrollY to -1, 0, or +1.
*  Returns true if a viewport drag is active.
*********************************************************/
bool inputTouchGetViewportDragScroll(int *scrollX, int *scrollY);

/*********************************************************
*NAME:          inputTouchGetViewportDragDelta
*PURPOSE:
*  Returns the raw pixel delta from the last finger motion
*  on the viewport drag. Consuming — clears after read.
*  Returns true if a drag motion occurred this frame.
*********************************************************/
bool inputTouchGetViewportDragDelta(float *deltaX, float *deltaY);

/* Legacy API — now wrappers around button system */
bool inputTouchIsFirePressed(void);
bool inputTouchIsMinePressed(void);
bool inputTouchIsMineHeld(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_TOUCH_H */
