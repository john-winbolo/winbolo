/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Gamepad Input
*Filename:      input_gamepad.h
*Purpose:
*  Gamepad input handling. Polls SDL3 gamepad state each
*  frame for movement, actions, gunsight, and map scroll.
*  Single-controller policy: first-connected wins; on
*  disconnect, promotes the next-connected gamepad.
*  Works on all platforms whenever a gamepad is connected.
*********************************************************/

#ifndef INPUT_GAMEPAD_H
#define INPUT_GAMEPAD_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "../../bolo/global.h"
#include "../../bolo/screen.h"

#ifdef __cplusplus
extern "C" {
#endif

void inputGamepadInit(void);
void inputGamepadShutdown(void);
void inputGamepadProcessEvent(const SDL_Event *e);
bool inputGamepadIsConnected(void);

tankButton inputGamepadGetMovement(BYTE tankAngle);
bool inputGamepadIsFireHeld(void);
bool inputGamepadIsMineHeld(void);
int  inputGamepadGetGunsightChange(void);  /* -1, 0, +1; edge-triggered, consumed on read */
bool inputGamepadGetScrollDirection(float *dx, float *dy);

/* Builder UX edge-triggered getters (consume on read). */
int  inputGamepadGetBuildSelectChange(void);  /* -1 (D-pad LEFT), +1 (D-pad RIGHT), 0 */
bool inputGamepadIsViewToggleEdge(void);       /* Y press, consumed on read */
bool inputGamepadIsBuilderConfirmEdge(void);   /* X press, consumed on read */

/* Right-stick scroll sensitivity multiplier (clamped 0.25..4.0 by UI). */
extern float g_gamepadScrollSensitivity;

SDL_Gamepad     *inputGamepadGetActiveHandle(void);  /* may be NULL */
SDL_GamepadType  inputGamepadGetActiveType(void);
void             inputGamepadRumble(float strength, Uint32 durationMs);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_GAMEPAD_H */
