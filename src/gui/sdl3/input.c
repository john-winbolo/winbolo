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
#include "global.h"
#include "client_sim.h"
#include "client_render.h"
#include "../gamefront.h"
#include "../tiles.h"
#include "input.h"
#include "input_touch.h"
#include "sdl3imgui.h"
#include "sdl3draw.h"
#include "../ui_mode.h"

extern bool smoothScrollingEnabled;

/* Smooth-scroll speed: game pixels advanced per scroll tick.
   Tile = 16 game pixels, game runs at 20 ticks/sec, so:
     px=4  →  5 tiles/sec
     px=6  →  7.5 tiles/sec  (default)
     px=8  → 10 tiles/sec
     px=12 → 15 tiles/sec
   Adjust to taste. */
static int smoothScrollSpeedPx = 6;

/* Sub-tile pixel accumulators for smooth scrolling (in zoomed pixels,
   matching gDragOffsetX/Y units). */
static int smoothScrollAccumX = 0;
static int smoothScrollAccumY = 0;

static BYTE scrollKeyCount = 0;

/* Pill view auto-repeat: minimum wall-clock gap between pill advances while a
 * key is held. Each advance jumps a whole pill, so these are deliberately slow
 * compared with map scrolling. Wall-clock based so they're independent of how
 * often inputGetKeys/inputScroll are polled. The pill-view toggle key cycles a
 * bit faster than directional stepping. */
#define PILLVIEW_CYCLE_INTERVAL_MS 165
#define PILLVIEW_STEP_INTERVAL_MS  250
static Uint32 pillViewCycleMs = 0;
static Uint32 pillViewStepMs  = 0;

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
  pillViewCycleMs = 0;
  pillViewStepMs = 0;
  smoothScrollAccumX = 0;
  smoothScrollAccumY = 0;
  return TRUE;
}

/*********************************************************
*NAME:          pillViewInputStep
*PURPOSE:
*  Handles the pill-view toggle key (enter / cycle to next
*  pill) and, while in pill view, directional pill stepping
*  via the scroll keys. Both auto-repeat while a key is held,
*  with the first action firing immediately and subsequent
*  ones gated to PILLVIEW_STEP_INTERVAL_MS so it doesn't race
*  through the pills. Returns TRUE if in pill view after
*  processing (caller then suppresses map scrolling).
*********************************************************/
static bool pillViewInputStep(ClientSim *cs, keyItems *setKeys) {
  bool inPill = clientSimIsInPillView(cs);
  Uint32 now  = SDL_GetTicks();

  /* Pill-view toggle key: enters pill view when not already in it, else
   * advances to the next pill. First press acts immediately, then repeats
   * on the cycle cadence. */
  bool cycle = KEY_DOWN(setKeys->kiPillView);
  if (!cycle) {
    pillViewCycleMs = 0;
  } else if (pillViewCycleMs == 0 ||
             (now - pillViewCycleMs) >= PILLVIEW_CYCLE_INTERVAL_MS) {
    pillViewCycleMs = now;
    clientSimPillView(cs, 0, 0);
    inPill = clientSimIsInPillView(cs);
  }

  /* Directional pill stepping — only in pill view, on the slower step
   * cadence (computed from inPill so it can't fire on the entering press). */
  bool stepUp    = inPill && KEY_DOWN(setKeys->kiScrollUp);
  bool stepDown  = inPill && KEY_DOWN(setKeys->kiScrollDown);
  bool stepLeft  = inPill && KEY_DOWN(setKeys->kiScrollLeft);
  bool stepRight = inPill && KEY_DOWN(setKeys->kiScrollRight);
  if (!stepUp && !stepDown && !stepLeft && !stepRight) {
    pillViewStepMs = 0;
  } else if (pillViewStepMs == 0 ||
             (now - pillViewStepMs) >= PILLVIEW_STEP_INTERVAL_MS) {
    pillViewStepMs = now;
    if (stepUp)    { clientRenderFrame(cs, up); }
    if (stepDown)  { clientRenderFrame(cs, down); }
    if (stepLeft)  { clientRenderFrame(cs, left); }
    if (stepRight) { clientRenderFrame(cs, right); }
  }

  return clientSimIsInPillView(cs);
}

/*********************************************************
*NAME:          smoothScrollTick
*PURPOSE:
*  Smooth (pixel-level) arrow-key scrolling.  Advances a
*  sub-tile pixel accumulator each call; commits full-tile
*  crossings to the engine via clientRenderFrame and pushes
*  the remainder to sdl3DrawSetDragOffset for sub-tile
*  rendering.
*
*  When no scroll key is held, snaps the accumulator to
*  the nearest tile boundary so the view comes to rest
*  cleanly.
*********************************************************/
static void smoothScrollTick(ClientSim *cs, keyItems *setKeys) {
  int dx = 0, dy = 0;

  if (KEY_DOWN(setKeys->kiScrollLeft))  dx -= 1;
  if (KEY_DOWN(setKeys->kiScrollRight)) dx += 1;
  if (KEY_DOWN(setKeys->kiScrollUp))    dy -= 1;
  if (KEY_DOWN(setKeys->kiScrollDown))  dy += 1;

  int zoom = sdl3DrawGetZoomFactor();
  if (zoom < 1) zoom = 1;
  int tileW = TILE_SIZE_X * zoom;
  int tileH = TILE_SIZE_Y * zoom;
  int stepZoomed = smoothScrollSpeedPx * zoom;
  if (stepZoomed < 1) stepZoomed = 1;

  /* No direction held: snap to nearest tile boundary. */
  if (dx == 0 && dy == 0) {
    if (smoothScrollAccumX != 0 || smoothScrollAccumY != 0) {
      if (smoothScrollAccumX >  tileW / 2) clientRenderFrame(cs, right);
      if (smoothScrollAccumX < -tileW / 2) clientRenderFrame(cs, left);
      if (smoothScrollAccumY >  tileH / 2) clientRenderFrame(cs, down);
      if (smoothScrollAccumY < -tileH / 2) clientRenderFrame(cs, up);
      smoothScrollAccumX = 0;
      smoothScrollAccumY = 0;
      sdl3DrawSetDragOffset(0, 0);
    }
    return;
  }

  smoothScrollAccumX += dx * stepZoomed;
  smoothScrollAccumY += dy * stepZoomed;

  while (smoothScrollAccumX >= tileW)  { clientRenderFrame(cs, right); smoothScrollAccumX -= tileW; }
  while (smoothScrollAccumX <= -tileW) { clientRenderFrame(cs, left);  smoothScrollAccumX += tileW; }
  while (smoothScrollAccumY >= tileH)  { clientRenderFrame(cs, down);  smoothScrollAccumY -= tileH; }
  while (smoothScrollAccumY <= -tileH) { clientRenderFrame(cs, up);    smoothScrollAccumY += tileH; }

  sdl3DrawSetDragOffset(smoothScrollAccumX, smoothScrollAccumY);
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
    inputTouchSetTankAngle(clientSimGetTank256Dir(cs));
    tb = inputTouchGetMovement();
  }

  /* Mine laying is now handled via InputPacket — see inputIsMineKeyPressed() */

  {
    buildSelect newSelect = BsTrees; /* init to suppress warning */
    bool wantSwitch = false;
    if (KEY_DOWN(setKeys->kiQuickTree))         { newSelect = BsTrees;    wantSwitch = true; }
    else if (KEY_DOWN(setKeys->kiQuickRoad))    { newSelect = BsRoad;     wantSwitch = true; }
    else if (KEY_DOWN(setKeys->kiQuickWall))    { newSelect = BsBuilding; wantSwitch = true; }
    else if (KEY_DOWN(setKeys->kiQuickPillbox)) { newSelect = BsPillbox;  wantSwitch = true; }
    else if (KEY_DOWN(setKeys->kiQuickMine))    { newSelect = BsMine;     wantSwitch = true; }
    if (wantSwitch) {
      curSelect = clientSimGetCurrentBuildSelect(cs);
      if (curSelect != newSelect) {
        sdl3DrawSelectIndentsOff(curSelect, 0, 0);
        sdl3DrawSelectIndentsOn(newSelect, 0, 0);
        clientSimSetCurrentBuildSelect(cs, newSelect);
      }
    }
  }

  /* Pill view consumes the scroll keys (and the pill-view toggle key) to
   * step between pills; map scrolling is suppressed while it is active. */
  if (pillViewInputStep(cs, setKeys)) {
    smoothScrollAccumX = 0;
    smoothScrollAccumY = 0;
    sdl3DrawSetDragOffset(0, 0);
  } else if (smoothScrollingEnabled) {
    smoothScrollTick(cs, setKeys);
  } else {
    /* Drop any stale sub-tile accumulation from a previous smooth-scroll session. */
    smoothScrollAccumX = 0;
    smoothScrollAccumY = 0;
    scrollKeyCount++;
    if (scrollKeyCount >= INPUT_SCROLL_WAIT_TIME) {
      scrollKeyCount = 0;
      if (KEY_DOWN(setKeys->kiScrollUp))    { clientRenderFrame(cs, up); }
      if (KEY_DOWN(setKeys->kiScrollDown))  { clientRenderFrame(cs, down); }
      if (KEY_DOWN(setKeys->kiScrollLeft))  { clientRenderFrame(cs, left); }
      if (KEY_DOWN(setKeys->kiScrollRight)) { clientRenderFrame(cs, right); }
    }
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

  /* Pill view consumes the scroll keys (and the pill-view toggle key) to
   * step between pills; map scrolling is suppressed while it is active. */
  if (pillViewInputStep(cs, setKeys)) {
    smoothScrollAccumX = 0;
    smoothScrollAccumY = 0;
    sdl3DrawSetDragOffset(0, 0);
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
    if (KEY_DOWN(setKeys->kiScrollUp))    { clientRenderFrame(cs, up); }
    if (KEY_DOWN(setKeys->kiScrollDown))  { clientRenderFrame(cs, down); }
    if (KEY_DOWN(setKeys->kiScrollLeft))  { clientRenderFrame(cs, left); }
    if (KEY_DOWN(setKeys->kiScrollRight)) { clientRenderFrame(cs, right); }
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

void inputBumpGunsight(int direction) {
  if (direction > 0) {
    lastGunsightAdj = 1;
  } else if (direction < 0) {
    lastGunsightAdj = 2;
  }
}
