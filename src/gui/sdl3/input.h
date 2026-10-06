/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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
*NAME:          inputPushToTalkPoll
*PURPOSE:
*  Tells the voice runtime whether the push-to-talk key is
*  held. Called once per turn of whichever loop is reading
*  the keyboard — the in-game key polls above, and the lobby,
*  which reads no keys of its own. Callers that are not
*  reading input this turn pass active FALSE rather than
*  skipping the call, so a key held as a loop hands over
*  cannot leave the microphone open behind it.
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  active  - FALSE when this poll is not reading input
*********************************************************/
void inputPushToTalkPoll(keyItems *setKeys, bool active);

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
*NAME:          inputResetHeldKeys
*PURPOSE:
*  Drops all held-key and latched edge state. Called on a
*  window focus transition so a key released while the
*  game window was unfocused (no KEY_UP delivered) can't
*  stay "held" in the polled keyboard state and drive the
*  tank when focus returns.
*********************************************************/
void inputResetHeldKeys(void);

/*********************************************************
*NAME:          inputSwallowKeyUntilRelease
*PURPOSE:
*  Marks one key as taken by the shortcut layer, so the
*  game binding on it does not also fire. The key reads as
*  released to every binding poll until it is physically
*  let go.
*
*  For a menu shortcut whose letter is also a game key —
*  Ctrl+M opens Send Message and M is the default Base View
*  key, so both used to happen at once. Dropping the event
*  is not enough: bindings are polled from SDL's keyboard
*  state, where the letter is still down.
*
*ARGUMENTS:
*  scancode - SDL_Scancode the shortcut consumed
*********************************************************/
void inputSwallowKeyUntilRelease(int scancode);

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
*  repeat   - true for an OS auto-repeat of a held key
*  allowTurn - true while a running game can accept turn taps
*********************************************************/
void inputButtonInput(keyItems *setKeys, SDL_Scancode scancode, bool newState,
                      bool repeat, bool allowTurn);

/*********************************************************
*NAME:          inputConsumeGunsightAdj
*PURPOSE:
*  Returns and clears the pending gunsight adjustment
*  from keyboard input. 0 = none, 1 = increase, 2 = decrease.
*  Called by screenBuildInputPacket to flow gunsight changes
*  through the InputPacket to the server.
*********************************************************/
uint8_t inputConsumeGunsightAdj(void);

/*********************************************************
*NAME:          inputAutoSlowdownAssist
*PURPOSE:
*  Returns TRUE while controller driving should add
*  auto-slowdown on top of the saved preference. The
*  frontend tick ORs it into each InputPacket's
*  INPUT_FLAG_AUTOSLOW, so the tank's own setting stays the
*  saved preference and every input carries the value the
*  server and the local replay both apply to it.
*********************************************************/
bool inputAutoSlowdownAssist(void);

/*********************************************************
*NAME:          inputBumpGunsight
*PURPOSE:
*  Queues a single gunsight adjustment from a non-keyboard
*  source (e.g. mouse wheel). Positive direction = increase,
*  negative = decrease. Consumed by the next
*  inputConsumeGunsightAdj() call on the input-packet tick.
*  Calling again before consume overwrites the pending value.
*********************************************************/
void inputBumpGunsight(int direction);

#endif /* _SDL3_INPUT_H */
