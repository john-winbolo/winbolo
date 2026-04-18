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
*Filename:      input.c
*Author:        John Morrison
*Creation Date: 16/12/98
*Last Modified:   1/5/00
*Purpose:
*  Keyboard and mouse routines (SDL3 implementation)
*  Polls SDL_GetKeyboardState() using SDL_Scancode values.
*********************************************************/

#include <SDL3/SDL.h>
#include "../../bolo/global.h"
#include "../../bolo/screen.h"
#include "../../bolo/client_sim.h"
#include "../gamefront.h"
#include "input.h"
#include "input_touch.h"
#include "sdl3imgui.h"
#include "sdl3draw.h"
#include "debug_overlay.h"
#include "../ui_mode.h"

static BYTE scrollKeyCount = 0;

/* Gunsight adjustment state — set by inputGetKeys, consumed by
 * screenBuildInputPacket via inputConsumeGunsightAdj().
 * 0 = no change, 1 = increase, 2 = decrease. */
static uint8_t lastGunsightAdj = 0;

/* Mine key state tracking.
 * SDL_GetKeyboardState can report modifier keys (LSHIFT) as
 * permanently held on some platforms.  We track via SDL events
 * instead, but also use SDL_GetKeyboardState as a fallback
 * for the very first press detection. */
static bool mineKeyEventDown = FALSE;  /* set by SDL_EVENT_KEY_DOWN/UP */
static bool mineKeyPhysicalDown = FALSE; /* tracks physical key state for edge detection */
static bool mineKeyEventsActive = FALSE; /* TRUE once we've seen any event for this key */

/*********************************************************
*NAME:          appHasFocus
*PURPOSE:
*  Returns true if the SDL3 window currently has
*  keyboard focus.
*********************************************************/
static bool appHasFocus(void) {
  SDL_Window *sdlWin = sdl3DrawGetWindow();
  if (sdlWin && (SDL_GetWindowFlags(sdlWin) & SDL_WINDOW_INPUT_FOCUS)) {
    return true;
  }
  /* Also accept input when the debug zoom window has focus so the tank
   * can be controlled from there. */
  return debugZoomHasFocus();
}

/* Returns non-zero if the key at the given SDL_Scancode is currently held */
static bool keyDown(int sc) {
  const bool *state = SDL_GetKeyboardState(NULL);
  if (!state || sc <= 0 || sc >= SDL_SCANCODE_COUNT) {
    return false;
  }
  return state[sc];
}

#define KEY_DOWN(sc) keyDown(sc)

/*********************************************************
*NAME:          inputSetup
*PURPOSE:
*  Sets up input systems.
*  Returns whether the operation was successful or not
*********************************************************/
bool inputSetup(void) {
  scrollKeyCount = 0;
  return TRUE;
}

/*********************************************************
*NAME:          inputCleanup
*PURPOSE:
*  Destroys and cleans up input systems.
*********************************************************/
void inputCleanup(void) {
}

/*********************************************************
*NAME:          inputActivate
*PURPOSE:
*  Application has just got focus. Re-acquire input.
*  No-op for SDL3.
*********************************************************/
void inputActivate(void) {
}

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
tankButton inputGetKeys(ClientSim *cs, keyItems *setKeys, bool isMenu) {
  static BYTE gunsightKeyCount = 0;
  tankButton tb;
  buildSelect curSelect;

  if (isMenu == TRUE || sdl3ImguiWantsKeyboard() || !appHasFocus()) {
    return TNONE;
  }

  tb = TNONE;

  if (KEY_DOWN(setKeys->kiForward) && KEY_DOWN(setKeys->kiRight)) {
    tb = TRIGHTACCEL;
  } else if (KEY_DOWN(setKeys->kiForward) && KEY_DOWN(setKeys->kiLeft)) {
    tb = TLEFTACCEL;
  } else if (KEY_DOWN(setKeys->kiBackward) && KEY_DOWN(setKeys->kiLeft)) {
    tb = TLEFTDECEL;
  } else if (KEY_DOWN(setKeys->kiBackward) && KEY_DOWN(setKeys->kiRight)) {
    tb = TRIGHTDECEL;
  } else if (KEY_DOWN(setKeys->kiForward)) {
    tb = TACCEL;
  } else if (KEY_DOWN(setKeys->kiBackward)) {
    tb = TDECEL;
  } else if (KEY_DOWN(setKeys->kiLeft)) {
    tb = TLEFT;
  } else if (KEY_DOWN(setKeys->kiRight)) {
    tb = TRIGHT;
  }

  /* Combine with touch joystick input in tablet mode */
  if (tb == TNONE && uiModeIsTablet()) {
    inputTouchSetTankAngle(screenGetTank256DirCS(cs));
    tb = inputTouchGetMovement();
  }

  /* Mine laying is now handled via InputPacket — see inputIsMineKeyPressed() */

  if (KEY_DOWN(setKeys->kiQuickTree)) {
    curSelect = getBuildCurrentSelectCS(cs);
    if (curSelect != BsTrees) {
      setBuildCurrentSelectCS(cs, BsTrees);
    }
  } else if (KEY_DOWN(setKeys->kiQuickRoad)) {
    curSelect = getBuildCurrentSelectCS(cs);
    if (curSelect != BsRoad) {
      setBuildCurrentSelectCS(cs, BsRoad);
    }
  } else if (KEY_DOWN(setKeys->kiQuickWall)) {
    curSelect = getBuildCurrentSelectCS(cs);
    if (curSelect != BsBuilding) {
      setBuildCurrentSelectCS(cs, BsBuilding);
    }
  } else if (KEY_DOWN(setKeys->kiQuickPillbox)) {
    curSelect = getBuildCurrentSelectCS(cs);
    if (curSelect != BsPillbox) {
      setBuildCurrentSelectCS(cs, BsPillbox);
    }
  } else if (KEY_DOWN(setKeys->kiQuickMine)) {
    curSelect = getBuildCurrentSelectCS(cs);
    if (curSelect != BsMine) {
      setBuildCurrentSelectCS(cs, BsMine);
    }
  }

  scrollKeyCount++;
  if (scrollKeyCount >= INPUT_SCROLL_WAIT_TIME) {
    scrollKeyCount = 0;
    if (KEY_DOWN(setKeys->kiScrollUp))    { screenUpdateCS(cs, up); }
    if (KEY_DOWN(setKeys->kiScrollDown))  { screenUpdateCS(cs, down); }
    if (KEY_DOWN(setKeys->kiScrollLeft))  { screenUpdateCS(cs, left); }
    if (KEY_DOWN(setKeys->kiScrollRight)) { screenUpdateCS(cs, right); }
  }

  gunsightKeyCount++;
  if (gunsightKeyCount >= INPUT_GUNSIGHT_WAIT_TIME) {
    if (KEY_DOWN(setKeys->kiGunIncrease)) {
      lastGunsightAdj = 1;  /* increase — flows through InputPacket */
      gunsightKeyCount = 0;
    } else if (KEY_DOWN(setKeys->kiGunDecrease)) {
      lastGunsightAdj = 2;  /* decrease — flows through InputPacket */
      gunsightKeyCount = 0;
    } else if (gunsightKeyCount > (INPUT_GUNSIGHT_WAIT_TIME + 1)) {
      gunsightKeyCount = INPUT_GUNSIGHT_WAIT_TIME;
    }
  }

  return tb;
}

/*********************************************************
*NAME:          inputScroll
*PURPOSE:
*  Checks and does scrolling of the window
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - True if we are in a menu
*********************************************************/
void inputScroll(ClientSim *cs, keyItems *setKeys, bool isMenu) {
  if (isMenu == TRUE || sdl3ImguiWantsKeyboard() || !appHasFocus()) {
    return;
  }

  scrollKeyCount++;
  if (scrollKeyCount >= INPUT_SCROLL_WAIT_TIME) {
    scrollKeyCount = 0;
    if (KEY_DOWN(setKeys->kiScrollUp))    { screenUpdateCS(cs, up); }
    if (KEY_DOWN(setKeys->kiScrollDown))  { screenUpdateCS(cs, down); }
    if (KEY_DOWN(setKeys->kiScrollLeft))  { screenUpdateCS(cs, left); }
    if (KEY_DOWN(setKeys->kiScrollRight)) { screenUpdateCS(cs, right); }
  }
}

/*********************************************************
*NAME:          inputIsFireKeyPressed
*PURPOSE:
*  Returns whether the fire key is pressed
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - TRUE if we are in a menu
*********************************************************/
bool inputIsFireKeyPressed(keyItems *setKeys, bool isMenu) {
  if (isMenu == TRUE || sdl3ImguiWantsKeyboard() || !appHasFocus()) {
    return uiModeIsTablet() ? inputTouchIsFirePressed() : FALSE;
  }
  return KEY_DOWN(setKeys->kiShoot) || (uiModeIsTablet() && inputTouchIsFirePressed());
}

/*********************************************************
*NAME:          inputIsMineKeyPressed
*PURPOSE:
*  Returns TRUE while the lay-mine key is held down.
*  tankLayMine() on the server is self-limiting via
*  mapIsMine() so continuous TRUE is safe and lets the
*  player hold the key to lay mines while driving.
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - TRUE if we are in a menu
*********************************************************/
bool inputIsMineKeyPressed(keyItems *setKeys, bool isMenu) {
  if (isMenu == TRUE || sdl3ImguiWantsKeyboard() || !appHasFocus()) {
    return uiModeIsTablet() ? inputTouchIsMinePressed() : FALSE;
  }

  bool touchMine = uiModeIsTablet() ? inputTouchIsMinePressed() : false;
  return KEY_DOWN(setKeys->kiLayMine) || touchMine;
}

/*********************************************************
*NAME:          inputButtonInput
*PURPOSE:
*  For future use when an SDL event loop replaces the
*  Win32 message loop. No-op since we poll
*  SDL_GetKeyboardState() directly.
*
*ARGUMENTS:
*  setKeys  - Structure that holds the key bindings
*  scancode - SDL_Scancode of the key
*  newState - true if pressed, false if released
*********************************************************/
void inputButtonInput(keyItems *setKeys, SDL_Scancode scancode, bool newState) {
  if ((int)scancode == setKeys->kiLayMine) {
    /* Edge detection: only set mineKeyEventDown on a fresh press,
     * not on auto-repeat KEY_DOWN events */
    if (newState && !mineKeyPhysicalDown) {
      mineKeyEventDown = TRUE;
    }
    mineKeyPhysicalDown = newState;
    mineKeyEventsActive = TRUE;
  }
}

uint8_t inputConsumeGunsightAdj(void) {
  uint8_t val = lastGunsightAdj;
  lastGunsightAdj = 0;
  return val;
}
