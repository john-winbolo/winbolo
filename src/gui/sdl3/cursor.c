/*
 * Copyright (c) 1998-2026 John Morrison.
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

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include "global.h"
#include "viewport_types.h"   /* MAIN_SCREEN_SIZE_X/Y */
#include "../tiles.h"             /* TILE_SIZE_X/Y */
#include "../positions.h"         /* MAIN_OFFSET_X/Y */
#include "sdl3draw.h"             /* sdl3DrawGetZoomFactor, sdl3DrawGetWindow */
#include "../winbolo.h"           /* isInMenu */
#include "input_source.h"         /* inputSourceNoteCursorWarp */
#include "cursor.h"

/* Is the cursor inside the main view area */
bool cursorInMainView = false;

/* Cached transformed mouse coordinates (in game logical coordinates) */
static float gCachedMouseX = 0.0f;
static float gCachedMouseY = 0.0f;

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
  /* Cache the transformed coordinates for use by cursorPos */
  gCachedMouseX = (float)mouseX;
  gCachedMouseY = (float)mouseY;

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
*
*  The result must reflect both the latest mouse position
*  and the current sub-tile view offset, so this recomputes
*  on every call rather than caching by mouse-pos alone —
*  subPosX/Y change every tick while autoscroll is active,
*  and a cached column would drift left of the rendered
*  cursor square as the world scrolls under a stationary
*  mouse.
*********************************************************/
/* DEBUG: per-call cursor decision log. Captures the inputs (mx, my,
 * subPos, zoom) and outputs (xPos, yPos, col, row) so we can correlate
 * "cursor jumped to a wrong tile" reports with the math that produced
 * the jump. Logs only when the column/row CHANGES so the file stays
 * small. Remove once cursor behavior is locked in. */
/* Debug file logging. Off for shipping builds — flip to 1 to re-enable
 * the cursor.log trace. When 0 no file is opened or written. */
#define WB_DEBUG_FILE_LOG 0

static FILE *gCursorLog = NULL;
static bool  gCursorLogTried = false;
static BYTE  gCursorLogLastCol = 0xFF, gCursorLogLastRow = 0xFF;
static int   gCursorLogEntries = 0;

static void cursorLog(const char *fmt, ...) {
  if (!WB_DEBUG_FILE_LOG) return;
  if (gCursorLogEntries >= 2000) return;
  if (!gCursorLog && !gCursorLogTried) {
    gCursorLogTried = true;
    gCursorLog = fopen("cursor.log", "a");
    if (gCursorLog) {
      fprintf(gCursorLog, "--- cursor session start ---\n");
      fflush(gCursorLog);
    }
  }
  if (!gCursorLog) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(gCursorLog, fmt, ap);
  va_end(ap);
  fflush(gCursorLog);
  gCursorLogEntries++;
}

bool cursorPos(RECT *rcWindow, BYTE *xValue, BYTE *yValue,
               int subPosX, int subPosY) {
  (void)rcWindow; /* SDL3 uses window-relative coords from SDL_GetMouseState */

  if (cursorInMainView) {
    float mx = gCachedMouseX;
    float my = gCachedMouseY;
    int zf    = sdl3DrawGetZoomFactor();
    int tileW = zf * TILE_SIZE_X;
    int tileH = zf * TILE_SIZE_Y;
    int xPos  = (int)mx - (zf * MAIN_OFFSET_X);
    int yPos  = (int)my - (zf * MAIN_OFFSET_Y);
    int edgePxX = subPosX * tileW / 256;
    int edgePxY = subPosY * tileH / 256;
    xPos += edgePxX;
    yPos += edgePxY;
    div_t dx = div(xPos, tileW);
    div_t dy = div(yPos, tileH);
    *xValue = (BYTE)(dx.quot + 1);
    *yValue = (BYTE)(dy.quot + 1);
    if (*xValue > MAIN_SCREEN_SIZE_X || *yValue > MAIN_SCREEN_SIZE_Y) {
      cursorLog("[cur] OUT_OF_RANGE mx=%.1f my=%.1f zf=%d xPos=%d yPos=%d col=%u row=%u\n",
                (double)mx, (double)my, zf, xPos, yPos,
                (unsigned)*xValue, (unsigned)*yValue);
      return false;
    }
    if (*xValue != gCursorLogLastCol || *yValue != gCursorLogLastRow) {
      cursorLog("[cur] mx=%.1f my=%.1f zf=%d xPos=%d yPos=%d -> col=%u row=%u (was %u,%u)\n",
                (double)mx, (double)my, zf, xPos, yPos,
                (unsigned)*xValue, (unsigned)*yValue,
                (unsigned)gCursorLogLastCol, (unsigned)gCursorLogLastRow);
      gCursorLogLastCol = *xValue;
      gCursorLogLastRow = *yValue;
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
  inputSourceNoteCursorWarp();
}

/*********************************************************
*NAME:          cursorApplyScrollDelta
*PURPOSE:
*  Sub-pixel scroll-tracking warp. Called once per render
*  frame with the pixel delta the view scrolled (sum of
*  whole-tile xOffset bumps and sub-tile autoscroll ease,
*  in main-view pixels). Shifts both the cached game-coord
*  mouse position and the OS cursor by the same delta so
*  the mouse stays over the same world tile while the map
*  slides beneath it. Pairs with the subPos correction in
*  cursorPos: warp keeps the OS cursor and gCachedMouseX
*  in sync with the new world position; subPos correction
*  then computes the same tile cell every frame.
*********************************************************/
void cursorApplyScrollDelta(int dpx, int dpy) {
  if (!cursorInMainView) return;
  if (dpx == 0 && dpy == 0) return;
  gCachedMouseX -= (float)dpx;
  gCachedMouseY -= (float)dpy;
  float mx, my;
  SDL_GetMouseState(&mx, &my);
  mx -= (float)dpx;
  my -= (float)dpy;
  SDL_WarpMouseInWindow(sdl3DrawGetWindow(), mx, my);
  inputSourceNoteCursorWarp();
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
  inputSourceNoteCursorWarp();
}
