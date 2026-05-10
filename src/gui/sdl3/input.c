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
#include "../tiles.h"
#include "input.h"
#include "input_touch.h"
#include "input_gamepad.h"
#include "build_cursor.h"
#include "sdl3imgui.h"
#include "sdl3draw.h"
#include "../ui_mode.h"
#include "../clientmutex.h"

extern bool smoothScrollingEnabled;

/* Smooth-scroll speed: game pixels advanced per scroll tick.
   Tile = 16 game pixels.  Adjust to taste. */
static int smoothScrollSpeedPx = 4;

/* Sub-tile pixel accumulators for smooth scrolling (in zoomed pixels,
   matching gDragOffsetX/Y units). */
static int smoothScrollAccumX = 0;
static int smoothScrollAccumY = 0;

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
  return false;
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
  smoothScrollAccumX = 0;
  smoothScrollAccumY = 0;
  buildCursorReset();
  return TRUE;
}

/*********************************************************
*NAME:          smoothScrollAccumulate
*PURPOSE:
*  Pure accumulator: feeds pixel deltas (in zoomed pixels)
*  into the smooth-scroll sub-tile accumulator. Commits
*  whole-tile crossings to the engine via screenUpdateCS
*  and pushes the remainder to sdl3DrawSetDragOffset for
*  sub-tile rendering.
*
*  When called with dx=dy=0 and no key/stick was held,
*  snaps the accumulator to the nearest tile boundary so
*  the view comes to rest cleanly.
*********************************************************/
static void smoothScrollAccumulate(ClientSim *cs, int dx, int dy) {
  int zoom = sdl3DrawGetZoomFactor();
  if (zoom < 1) zoom = 1;
  int tileW = TILE_SIZE_X * zoom;
  int tileH = TILE_SIZE_Y * zoom;

  if (dx == 0 && dy == 0) {
    if (smoothScrollAccumX != 0 || smoothScrollAccumY != 0) {
      if (smoothScrollAccumX >  tileW / 2) screenUpdateCS(cs, right);
      if (smoothScrollAccumX < -tileW / 2) screenUpdateCS(cs, left);
      if (smoothScrollAccumY >  tileH / 2) screenUpdateCS(cs, down);
      if (smoothScrollAccumY < -tileH / 2) screenUpdateCS(cs, up);
      smoothScrollAccumX = 0;
      smoothScrollAccumY = 0;
      sdl3DrawSetDragOffset(0, 0);
    }
    return;
  }

  smoothScrollAccumX += dx;
  smoothScrollAccumY += dy;

  while (smoothScrollAccumX >= tileW)  { screenUpdateCS(cs, right); smoothScrollAccumX -= tileW; }
  while (smoothScrollAccumX <= -tileW) { screenUpdateCS(cs, left);  smoothScrollAccumX += tileW; }
  while (smoothScrollAccumY >= tileH)  { screenUpdateCS(cs, down);  smoothScrollAccumY -= tileH; }
  while (smoothScrollAccumY <= -tileH) { screenUpdateCS(cs, up);    smoothScrollAccumY += tileH; }

  sdl3DrawSetDragOffset(smoothScrollAccumX, smoothScrollAccumY);
}

/*********************************************************
*NAME:          smoothScrollGetStepPx
*PURPOSE:
*  Returns the per-frame zoomed pixel step for one unit
*  of input (keyboard direction or full-deflection stick).
*********************************************************/
static int smoothScrollGetStepPx(void) {
  int zoom = sdl3DrawGetZoomFactor();
  if (zoom < 1) zoom = 1;
  int stepZoomed = smoothScrollSpeedPx * zoom;
  if (stepZoomed < 1) stepZoomed = 1;
  return stepZoomed;
}

/*********************************************************
*NAME:          smoothScrollTick
*PURPOSE:
*  Sums keyboard and gamepad scroll contributions and feeds
*  a single (dx, dy) into smoothScrollAccumulate per call.
*  Unifying the two prevents the keyboard path's snap-to-tile
*  branch from clearing a sub-tile accumulator that the
*  gamepad path is still building up.
*********************************************************/
static void smoothScrollTick(ClientSim *cs, keyItems *setKeys) {
  int dx = 0, dy = 0;
  int step = smoothScrollGetStepPx();

  /* Keyboard contribution (dx/dy in {-1, 0, +1}). */
  if (KEY_DOWN(setKeys->kiScrollLeft))  dx -= 1;
  if (KEY_DOWN(setKeys->kiScrollRight)) dx += 1;
  if (KEY_DOWN(setKeys->kiScrollUp))    dy -= 1;
  if (KEY_DOWN(setKeys->kiScrollDown))  dy += 1;
  dx *= step;
  dy *= step;

  /* Gamepad contribution (right stick, normalised).  When the free
     build cursor is active the right stick steers the cursor instead
     of scrolling — buildCursorTick handles its own camera follow, so
     the scroll path gets no contribution from the stick that frame. */
  if (inputGamepadIsConnected()) {
    float fdx = 0.0f, fdy = 0.0f;
    if (inputGamepadGetScrollDirection(&fdx, &fdy)) {
      int gx = (int)(fdx * (float)step);
      int gy = (int)(fdy * (float)step);
      /* Min 1px nudge so deadzone-grazing input still moves. */
      if (gx == 0 && fdx >  0.0f) gx =  1;
      if (gx == 0 && fdx <  0.0f) gx = -1;
      if (gy == 0 && fdy >  0.0f) gy =  1;
      if (gy == 0 && fdy <  0.0f) gy = -1;
      if (buildCursorIsActive()) {
        buildCursorTick(cs, gx, gy);
      } else {
        dx += gx;
        dy += gy;
      }
    }
  }

  /* Latch the manual-scroll override on any user scroll input.
     scrollAutoScroll honours the flag and skips its recenter pull
     until the tank's screen position reaches NO_SCROLL_EDGE, at which
     point it clears the flag and resumes tracking. */
  if (dx != 0 || dy != 0) {
    cs->scroll.autoScrollOverRide = TRUE;
  }

  smoothScrollAccumulate(cs, dx, dy);
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

  /* Movement priority: keyboard -> gamepad -> touch.
     Gamepad outranks touch on devices that have both
     (e.g. Steam Deck in dock with touchscreen monitor + pad). */
  if (tb == TNONE && inputGamepadIsConnected()) {
    tb = inputGamepadGetMovement(screenGetTank256DirCS(cs));
  }
  if (tb == TNONE && uiModeIsTablet()) {
    inputTouchSetTankAngle(screenGetTank256DirCS(cs));
    tb = inputTouchGetMovement();
  }

  /* Gamepad-only actions: build-type cycle, builder confirm, view toggle. */
  if (inputGamepadIsConnected()) {
    int delta = inputGamepadGetBuildSelectChange();
    if (delta != 0) {
      cycleBuildSelectCS(cs, delta);
      /* Sync the status-panel's cached gCurrentBuildSelect — the mouse
         click path does this at sdl3draw.c:589, but cycleBuildSelectCS
         only updates the ClientSim field. Without this the left-side
         indent doesn't move when D-pad cycles. */
      sdl3DrawSelectIndentsOn(getBuildCurrentSelectCS(cs), 0, 0);
    }

    if (inputGamepadIsBuildCursorToggleEdge()) {
      buildCursorToggle(cs);
    }

    if (inputGamepadIsViewPlayersEdge()) {
      sdl3ImguiTogglePlayersPanel();
    }

    if (inputGamepadIsBuilderConfirmEdge()) {
      BYTE bx, by;
      if (buildCursorGetTile(&bx, &by)) {
        /* Free build cursor active — dispatch to the cursor tile.
           The cursor stays on (and at the same absolute tile) so the
           player can fire repeated builds at the same spot, mirroring
           how the mouse cursor outline persists between clicks. */
        clientMutexWaitFor();
        screenManMoveToMapCS(cs, bx, by, getBuildCurrentSelectCS(cs));
        clientMutexRelease();
      } else {
        BYTE gsX, gsY;
        screenGetGunsightTileCS(cs, &gsX, &gsY);
        clientMutexWaitFor();
        screenManMoveToMapCS(cs, gsX, gsY, getBuildCurrentSelectCS(cs));
        clientMutexRelease();
      }
    }

    if (inputGamepadIsViewToggleEdge()) {
      static bool inPillView = false;
      if (inPillView) { screenTankViewCS(cs); inPillView = false; }
      else            { screenPillViewCS(cs, 0, 0); inPillView = true; }
    }
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

  if (smoothScrollingEnabled) {
    smoothScrollTick(cs, setKeys);
  } else {
    /* Drop any stale sub-tile accumulation from a previous smooth-scroll session. */
    smoothScrollAccumX = 0;
    smoothScrollAccumY = 0;
    scrollKeyCount++;
    if (scrollKeyCount >= INPUT_SCROLL_WAIT_TIME) {
      scrollKeyCount = 0;
      bool scrolled = FALSE;
      if (KEY_DOWN(setKeys->kiScrollUp))    { screenUpdateCS(cs, up);    scrolled = TRUE; }
      if (KEY_DOWN(setKeys->kiScrollDown))  { screenUpdateCS(cs, down);  scrolled = TRUE; }
      if (KEY_DOWN(setKeys->kiScrollLeft))  { screenUpdateCS(cs, left);  scrolled = TRUE; }
      if (KEY_DOWN(setKeys->kiScrollRight)) { screenUpdateCS(cs, right); scrolled = TRUE; }
      if (scrolled) cs->scroll.autoScrollOverRide = TRUE;
    }
  }

  gunsightKeyCount++;
  if (gunsightKeyCount >= INPUT_GUNSIGHT_WAIT_TIME) {
    /* Consume the gamepad edge once; merge with keyboard so a sub-rate
       gamepad press isn't dropped by the WAIT_TIME branch. */
    int padDelta = inputGamepadGetGunsightChange();
    if (KEY_DOWN(setKeys->kiGunIncrease) || padDelta > 0) {
      lastGunsightAdj = 1;  /* increase — flows through InputPacket */
      gunsightKeyCount = 0;
    } else if (KEY_DOWN(setKeys->kiGunDecrease) || padDelta < 0) {
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

  if (smoothScrollingEnabled) {
    smoothScrollTick(cs, setKeys);
    return;
  }

  /* Drop any stale sub-tile accumulation from a previous smooth-scroll session. */
  smoothScrollAccumX = 0;
  smoothScrollAccumY = 0;
  scrollKeyCount++;
  if (scrollKeyCount >= INPUT_SCROLL_WAIT_TIME) {
    scrollKeyCount = 0;
    bool scrolled = FALSE;
    if (KEY_DOWN(setKeys->kiScrollUp))    { screenUpdateCS(cs, up);    scrolled = TRUE; }
    if (KEY_DOWN(setKeys->kiScrollDown))  { screenUpdateCS(cs, down);  scrolled = TRUE; }
    if (KEY_DOWN(setKeys->kiScrollLeft))  { screenUpdateCS(cs, left);  scrolled = TRUE; }
    if (KEY_DOWN(setKeys->kiScrollRight)) { screenUpdateCS(cs, right); scrolled = TRUE; }
    if (scrolled) cs->scroll.autoScrollOverRide = TRUE;
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
  return KEY_DOWN(setKeys->kiShoot)
      || (uiModeIsTablet() && inputTouchIsFirePressed())
      || (inputGamepadIsConnected() && inputGamepadIsFireHeld());
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

  bool touchMine   = uiModeIsTablet() ? inputTouchIsMinePressed() : false;
  bool gamepadMine = inputGamepadIsConnected() ? inputGamepadIsMineHeld() : false;
  return KEY_DOWN(setKeys->kiLayMine) || touchMine || gamepadMine;
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
