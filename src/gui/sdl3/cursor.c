/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Cursor
*Filename:      cursor.c
*Author:        John Morrison
*Creation Date: 26/12/98
*Last Modified: 19/6/00
*Purpose:
*  Loads the different Cursors. SDL3 implementation.
*********************************************************/

/* SDL3 must come before bolo headers (#pragma pack guard) */
#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdio.h>
#include "../../bolo/global.h"
#include "../../bolo/screen.h"   /* MAIN_SCREEN_SIZE_X/Y */
#include "../tiles.h"             /* TILE_SIZE_X/Y */
#include "../positions.h"         /* MAIN_OFFSET_X/Y */
#include "sdl3draw.h"             /* sdl3DrawGetZoomFactor, sdl3DrawGetWindow */
#include "../winbolo.h"           /* isInMenu */
#include "cursor.h"

/* Is the cursor inside the main view area */
bool cursorInMainView = false;

static SDL_Cursor *s_saveCursor = NULL;
static SDL_Cursor *s_boloCursor = NULL;

/* 7×7 crosshair: data (XOR) and mask bits, same as the Linux version */
static const Uint8 s_cd[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static const Uint8 s_cm[8] = { 56, 40, 238, 124, 238, 40, 56, 0 };

/*********************************************************
*NAME:          cursorSetup
*PURPOSE:
*  Loads and sets up cursors.
*  Returns whether the operation was successful or not.
*********************************************************/
bool cursorSetup(void) {
  s_saveCursor = SDL_GetCursor();
  s_boloCursor = SDL_CreateCursor(s_cd, s_cm, 8, 8, 3, 3);
  cursorSetCursor(true);
  if (s_boloCursor == NULL || s_saveCursor == NULL) {
    fprintf(stderr, "Error building cursor\n");
    return false;
  }
  return true;
}

/*********************************************************
*NAME:          cursorCleanup
*PURPOSE:
*  Destroys and cleans up cursor resources.
*********************************************************/
void cursorCleanup(void) {
  if (s_boloCursor != NULL) {
    SDL_DestroyCursor(s_boloCursor);
    s_boloCursor = NULL;
  }
}

/*********************************************************
*NAME:          cursorSetCursor
*PURPOSE:
*  Switches between the normal system cursor and the
*  custom Bolo crosshair cursor.
*********************************************************/
void cursorSetCursor(bool normalCurs) {
  if (normalCurs) {
    SDL_SetCursor(s_saveCursor);
  } else {
    SDL_SetCursor(s_boloCursor);
  }
}

/*********************************************************
*NAME:          cursorMove
*PURPOSE:
*  The cursor has moved. Switches between the system
*  cursor and the Bolo crosshair depending on whether
*  the mouse is inside the main game-view area.
*********************************************************/
void cursorMove(int mouseX, int mouseY) {
  int zf    = sdl3DrawGetZoomFactor();
  int left  = zf * MAIN_OFFSET_X;
  int right = left + (MAIN_SCREEN_SIZE_X * (zf * TILE_SIZE_X));
  int top   = zf * MAIN_OFFSET_Y;
  int bot   = top  + (MAIN_SCREEN_SIZE_Y * (zf * TILE_SIZE_Y));

  if (mouseX >= left && mouseX <= right && mouseY >= top && mouseY <= bot) {
    if (!cursorInMainView) {
      cursorSetCursor(false);
      cursorInMainView = true;
    }
  } else {
    if (cursorInMainView) {
      cursorSetCursor(true);
      cursorInMainView = false;
    }
  }
}

/*********************************************************
*NAME:          cursorPos
*PURPOSE:
*  Returns whether the cursor is inside the main view.
*  If it is, xValue and yValue are filled with the map
*  tile coordinates (1-based). Otherwise they are set
*  to 0.
*********************************************************/
bool cursorPos(RECT *rcWindow, BYTE *xValue, BYTE *yValue) {
  (void)rcWindow; /* SDL3 uses window-relative coords from SDL_GetMouseState */
  static float oldX = -1.0f;
  static float oldY = -1.0f;

  if (cursorInMainView) {
    float mx, my;
    SDL_GetMouseState(&mx, &my);
    if (mx != oldX || my != oldY) {
      oldX = mx;
      oldY = my;
      int zf   = sdl3DrawGetZoomFactor();
      int xPos = (int)mx - (zf * MAIN_OFFSET_X);
      int yPos = (int)my - (zf * MAIN_OFFSET_Y);
      div_t dx = div(xPos, zf * (MAIN_SCREEN_SIZE_X + 1));
      div_t dy = div(yPos, zf * (MAIN_SCREEN_SIZE_Y + 1));
      *xValue = (BYTE)(dx.quot + 1);
      *yValue = (BYTE)(dy.quot + 1);
      if (*xValue > MAIN_SCREEN_SIZE_X || *yValue > MAIN_SCREEN_SIZE_Y) {
        return false;
      }
    }
  } else {
    *xValue = 0;
    *yValue = 0;
  }
  return cursorInMainView;
}

/*********************************************************
*NAME:          cursorAcquireCursor
*PURPOSE:
*  The window has just acquired the cursor.
*  Updates the cursor icon based on current mouse position.
*********************************************************/
void cursorAcquireCursor(void) {
  float mx, my;
  SDL_GetMouseState(&mx, &my);
  cursorMove((int)mx, (int)my);
}

/*********************************************************
*NAME:          cursorLeaveWindow
*PURPOSE:
*  The window has lost focus. Restores the system cursor.
*********************************************************/
void cursorLeaveWindow(void) {
  cursorSetCursor(true);
  cursorInMainView = false;
}

/*********************************************************
*NAME:          cursorSetPos
*PURPOSE:
*  Sets the cursor position on screen to the centre of
*  the given map tile. Win32 used SetCursorPos; SDL3
*  uses SDL_WarpMouseInWindow. No-op if not in main view.
*********************************************************/
void cursorSetPos(RECT rcWindow, BYTE xValue, BYTE yValue) {
  (void)rcWindow; /* SDL3 uses window-relative coordinates; RECT not needed */
  static BYTE lastX = 0;
  static BYTE lastY = 0;
  if (!cursorInMainView || xValue == 0 || yValue == 0) return;
  if (lastX == xValue && lastY == yValue) return;
  lastX = xValue;
  lastY = yValue;
  int zf = sdl3DrawGetZoomFactor();
  float x = (float)((xValue - 1) * zf * TILE_SIZE_X + zf * MAIN_OFFSET_X + MIDDLE_PIXEL);
  float y = (float)((yValue - 1) * zf * TILE_SIZE_Y + zf * MAIN_OFFSET_Y + MIDDLE_PIXEL);
  SDL_WarpMouseInWindow(sdl3DrawGetWindow(), x, y);
}

/*********************************************************
*NAME:          moveMousePointer
*PURPOSE:
*  Moves the mouse pointer to counteract map scrolling so
*  the cursor stays at the same visual position as the
*  map scrolls. Win32 used SetCursorPos; SDL3 uses
*  SDL_WarpMouseInWindow.
*********************************************************/
void moveMousePointer(updateType value) {
  if (isInMenu || !cursorInMainView) return;

  float mx, my;
  SDL_GetMouseState(&mx, &my);

  int zf    = sdl3DrawGetZoomFactor();
  int limL  = (MAIN_OFFSET_X - TILE_SIZE_X) * zf;
  int limR  = (MAIN_OFFSET_X + TILE_SIZE_X + (MAIN_SCREEN_SIZE_X * TILE_SIZE_X)) * zf;
  int limT  = (MAIN_OFFSET_Y - TILE_SIZE_Y) * zf;
  int limB  = (MAIN_OFFSET_Y + TILE_SIZE_Y + (MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y)) * zf;

  if (mx <= limL || mx >= limR || my <= limT || my >= limB) return;

  int step = MIDDLE_PIXEL * 2 * zf;
  if      (value == left)  mx += (float)step;
  else if (value == right) mx -= (float)step;
  else if (value == up)    my += (float)step;
  else if (value == down)  my -= (float)step;

  if (mx >= limR) mx = (float)(limR);
  else if (mx < limL) mx = (float)(limL);
  if (my >= limB)  my = (float)(limB);
  else if (my < limT)  my = (float)(limT);

  SDL_WarpMouseInWindow(sdl3DrawGetWindow(), mx, my);
}
