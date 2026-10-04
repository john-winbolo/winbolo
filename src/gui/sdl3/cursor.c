/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include <stdlib.h>
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

/* Scroll follow (cursorAnchorToView / cursorFollowView). The anchor is the
   world point the player's hand last put the pointer on, in zoomed game
   pixels from the map origin; the view origin is the same units, recorded
   each frame so a view jump can be told from a scroll. */
static bool  gAnchorValid = false;
static int   gAnchorX = 0;
static int   gAnchorY = 0;
static bool  gViewValid = false;
static int   gViewX = 0;
static int   gViewY = 0;
static int   gViewZoom = 0;
/* Window position of the last follow warp, so its echo motion is not taken
   for the player moving the mouse. */
static bool  gWarpPending = false;
static float gWarpWinX = 0.0f;
static float gWarpWinY = 0.0f;

/* A view that moves further than this between frames jumped (respawn, pill
   view, centring on the tank) rather than scrolled; the pointer stays put. */
#define CURSOR_FOLLOW_MAX_TILES 4

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
  /* That position is untransformed window pixels, so it is no place to
     follow from; the next real motion re-anchors. */
  gAnchorValid = false;
}

/*********************************************************
*NAME:          cursorLeaveWindow
*PURPOSE:
*  The window has lost focus. Restores the system cursor.
*********************************************************/
void cursorLeaveWindow(void) {
  cursorSetCursor(true);
  cursorInMainView = false;
  gAnchorValid = false;
}

/* The view origin in zoomed game pixels from the map origin. Uses the same
   sub-tile rounding as cursorPos, so an anchor taken here resolves to the
   tile cursorPos reported for the same pointer and view. */
static void cursorViewOrigin(int xOffset, int yOffset, int subPosX, int subPosY,
                             int *outX, int *outY) {
  int zf    = sdl3DrawGetZoomFactor();
  int tileW = zf * TILE_SIZE_X;
  int tileH = zf * TILE_SIZE_Y;
  *outX = xOffset * tileW + subPosX * tileW / 256;
  *outY = yOffset * tileH + subPosY * tileH / 256;
}

/*********************************************************
*NAME:          cursorIsWarpEcho
*PURPOSE:
*  Returns whether a motion event at this window position
*  is the echo of the last cursorFollowView warp rather
*  than the player moving the mouse. The first motion
*  anywhere else ends the wait for the echo.
*********************************************************/
bool cursorIsWarpEcho(float winX, float winY) {
  if (!gWarpPending) return false;
  /* Within a pixel: SDL reports the warp exactly, but the OS pointer lands
     on a whole pixel and can report that too. */
  if (SDL_fabsf(winX - gWarpWinX) <= 1.0f && SDL_fabsf(winY - gWarpWinY) <= 1.0f) {
    return true;
  }
  gWarpPending = false;
  return false;
}

/*********************************************************
*NAME:          cursorAnchorToView
*PURPOSE:
*  The player has moved the mouse. Pins the pointer to
*  the world point it is now over, for cursorFollowView
*  to keep it on as the view scrolls. Takes the view the
*  pointer's tile was resolved against.
*********************************************************/
void cursorAnchorToView(int xOffset, int yOffset, int subPosX, int subPosY) {
  int vx, vy;
  if (!cursorInMainView) {
    gAnchorValid = false;
    return;
  }
  int zf = sdl3DrawGetZoomFactor();
  cursorViewOrigin(xOffset, yOffset, subPosX, subPosY, &vx, &vy);
  gAnchorX = vx + (int)gCachedMouseX - zf * MAIN_OFFSET_X;
  gAnchorY = vy + (int)gCachedMouseY - zf * MAIN_OFFSET_Y;
  gAnchorValid = true;
  gWarpPending = false;
}

/*********************************************************
*NAME:          cursorFollowView
*PURPOSE:
*  Called once a frame with the view being drawn. Moves
*  the mouse pointer with the map as it scrolls, so it
*  stays on the square the player put it on, as WinBolo
*  1.x did. The pointer is placed from its world anchor
*  through the inverse of the event transform rather
*  than nudged by a delta, so it cannot drift on a
*  scaled window. It is held inside the view while its
*  square is scrolled off, and left alone when the view
*  jumps or allowWarp is false.
*********************************************************/
void cursorFollowView(int xOffset, int yOffset, int subPosX, int subPosY,
                      bool allowWarp) {
  int vx, vy;
  int zf    = sdl3DrawGetZoomFactor();
  int tileW = zf * TILE_SIZE_X;
  int tileH = zf * TILE_SIZE_Y;
  cursorViewOrigin(xOffset, yOffset, subPosX, subPosY, &vx, &vy);
  bool jumped = !gViewValid || zf != gViewZoom ||
                abs(vx - gViewX) > CURSOR_FOLLOW_MAX_TILES * tileW ||
                abs(vy - gViewY) > CURSOR_FOLLOW_MAX_TILES * tileH;
  gViewX = vx;
  gViewY = vy;
  gViewZoom = zf;
  gViewValid = true;

  if (jumped || !allowWarp || isInMenu || !cursorInMainView) {
    /* Nothing to follow until the hand next puts the pointer somewhere. */
    gAnchorValid = false;
    return;
  }
  if (!gAnchorValid) return;

  int left = zf * MAIN_OFFSET_X;
  int top  = zf * MAIN_OFFSET_Y;
  int gx   = gAnchorX - vx + left;
  int gy   = gAnchorY - vy + top;
  if (gx < left) gx = left;
  if (gx > left + MAIN_SCREEN_SIZE_X * tileW - 1) gx = left + MAIN_SCREEN_SIZE_X * tileW - 1;
  if (gy < top) gy = top;
  if (gy > top + MAIN_SCREEN_SIZE_Y * tileH - 1) gy = top + MAIN_SCREEN_SIZE_Y * tileH - 1;
  if (gx == (int)gCachedMouseX && gy == (int)gCachedMouseY) return;

  /* Aim at the middle of the game pixel, so the echo's trip back through
     windowToGameCoords truncates to this pixel and not the one before. */
  float wx, wy;
  if (!sdl3DrawGameToWindowCoords((float)gx + 0.5f, (float)gy + 0.5f, &wx, &wy)) return;
  gCachedMouseX = (float)gx + 0.5f;
  gCachedMouseY = (float)gy + 0.5f;
  gWarpWinX = wx;
  gWarpWinY = wy;
  gWarpPending = true;
  SDL_WarpMouseInWindow(sdl3DrawGetWindow(), wx, wy);
  inputSourceNoteCursorWarp();
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
