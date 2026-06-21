/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include <SDL3/SDL.h>
#include "global.h"
#include "client_enums.h"  /* tankButton */

/* Initialize touch input system. Call once after SDL_Init.
   safeLeft/safeTop/safeRight/safeBottom are insets from screen edges
   where controls should not be placed (e.g. display cutout areas). */
void touchInputSetup(int screenWidth, int screenHeight,
                     int safeLeft, int safeTop, int safeRight, int safeBottom);

/* Process an SDL touch/finger event. Call from the event loop.
   Returns true if the event was consumed by the touch system. */
bool touchInputProcessEvent(const SDL_Event *event);

/* Get current tank movement from the thumbstick.
   Returns a tankButton value (TNONE, TACCEL, TDECEL, TLEFT, etc.) */
tankButton touchInputGetKeys(void);

/* Returns true if the fire button is currently pressed. */
bool touchInputIsFireKeyPressed(void);

/* Returns true if the lay mine button was pressed (single-shot, resets after read). */
bool touchInputShouldLayMine(void);

/* Returns +1 if gunsight increase was pressed, -1 if decrease, 0 if neither.
   Single-shot, resets after read. */
int touchInputGetGunsightChange(void);

/* Render touch control overlays. Call each frame after game rendering. */
void touchInputRender(SDL_Renderer *renderer);

#endif
