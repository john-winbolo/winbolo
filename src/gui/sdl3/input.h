/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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
*Name:          Input
*Filename:      input.h
*Author:        John Morrison
*Creation Date: 16/12/98
*Last Modified:   1/5/00
*Purpose:
*  Keyboard and mouse routines (SDL3 implementation)
*********************************************************/

#ifndef _SDL3_INPUT_H
#define _SDL3_INPUT_H

#include <SDL3/SDL.h>
#include "../input.h"

/*********************************************************
*NAME:          inputSetup
*PURPOSE:
*  Sets up input systems. No-op for SDL3 (event-driven).
*  Returns whether the operation was successful or not
*********************************************************/
bool inputSetup(void);

/*********************************************************
*NAME:          inputCleanup
*PURPOSE:
*  Destroys and cleans up input systems.
*********************************************************/
void inputCleanup(void);

/*********************************************************
*NAME:          inputGetKeys
*PURPOSE:
*  Gets the current Buttons that are being pressed.
*  Returns tank buttons being pressed.
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - True if we are in a menu
*********************************************************/
tankButton inputGetKeys(struct ClientSim *cs, keyItems *setKeys, bool isMenu);

/*********************************************************
*NAME:          inputScroll
*PURPOSE:
*  Checks and does scrolling of the window
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - True if we are in a menu
*********************************************************/
void inputScroll(struct ClientSim *cs, keyItems *setKeys, bool isMenu);

/*********************************************************
*NAME:          inputIsFireKeyPressed
*PURPOSE:
*  Returns whether the fire key is pressed
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - TRUE if we are in a menu
*********************************************************/
bool inputIsFireKeyPressed(keyItems *setKeys, bool isMenu);

/*********************************************************
*NAME:          inputIsMineKeyPressed
*PURPOSE:
*  Returns whether the lay mine key is pressed
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - TRUE if we are in a menu
*********************************************************/
bool inputIsMineKeyPressed(keyItems *setKeys, bool isMenu);

/*********************************************************
*NAME:          inputActivate
*PURPOSE:
*  Application has just got focus. No-op for SDL3.
*********************************************************/
void inputActivate(void);

/*********************************************************
*NAME:          inputButtonInput
*PURPOSE:
*  Called from the SDL event loop on SDL_EVENT_KEY_DOWN /
*  SDL_EVENT_KEY_UP to update held-key state.
*
*ARGUMENTS:
*  setKeys  - Structure that holds the key bindings
*  scancode - SDL_Scancode of the key
*  newState - true if pressed, false if released
*********************************************************/
void inputButtonInput(keyItems *setKeys, SDL_Scancode scancode, bool newState);

/*********************************************************
*NAME:          inputConsumeGunsightAdj
*PURPOSE:
*  Returns and clears the pending gunsight adjustment
*  from keyboard input. 0 = none, 1 = increase, 2 = decrease.
*  Called by screenBuildInputPacket to flow gunsight changes
*  through the InputPacket to the server.
*********************************************************/
uint8_t inputConsumeGunsightAdj(void);

#endif /* _SDL3_INPUT_H */
