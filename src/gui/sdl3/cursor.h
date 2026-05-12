/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Cursor
*Filename:      cursor.h
*Author:        John Morrison
*Creation Date: 26/12/98
*Last Modified: 19/6/00
*Purpose:
*  Loads the different Cursors
*********************************************************/

#ifndef CURSOR_H
#define CURSOR_H

#include <stdbool.h>
#include <SDL3/SDL.h>
#include "../../bolo/global.h"
#include "../../bolo/client_enums.h"   /* updateType */

/* Portable RECT when not compiling on Windows */
#ifndef _WIN32
#ifndef _RECT_DEFINED
#define _RECT_DEFINED
typedef struct { long left, top, right, bottom; } RECT;
#endif
#endif

/*********************************************************
*NAME:          cursorSetup
*PURPOSE:
*  Loads and sets up cursors.
*  Returns whether the operation was successful or not.
*********************************************************/
bool cursorSetup(void);

/*********************************************************
*NAME:          cursorCleanup
*PURPOSE:
*  Destroys and cleans up cursor resources.
*********************************************************/
void cursorCleanup(void);

/*********************************************************
*NAME:          cursorSetCursor
*PURPOSE:
*  Switches between the normal system cursor and the
*  custom Bolo crosshair cursor.
*  normalCurs == TRUE  → system cursor
*  normalCurs == FALSE → Bolo cursor
*********************************************************/
void cursorSetCursor(bool normalCurs);

/*********************************************************
*NAME:          cursorMove
*PURPOSE:
*  The cursor has moved. Switches between the system
*  cursor and the Bolo crosshair depending on whether
*  the mouse is inside the main game-view area.
*********************************************************/
void cursorMove(int mouseX, int mouseY);

/*********************************************************
*NAME:          cursorPos
*PURPOSE:
*  Returns whether the cursor is inside the main view.
*  If it is, xValue and yValue are filled with the map
*  tile coordinates (1-based). Otherwise they are set
*  to 0.
*********************************************************/
bool cursorPos(RECT *rcWindow, BYTE *xValue, BYTE *yValue);

/*********************************************************
*NAME:          cursorAcquireCursor
*PURPOSE:
*  The window has just acquired the cursor.
*  Updates the cursor icon accordingly.
*********************************************************/
void cursorAcquireCursor(void);

/*********************************************************
*NAME:          cursorLeaveWindow
*PURPOSE:
*  The window has lost focus. Restores the system cursor.
*********************************************************/
void cursorLeaveWindow(void);

/*********************************************************
*NAME:          cursorSetPos
*PURPOSE:
*  Warps the cursor to the centre of the given map tile.
*  No-op if the cursor is not in the main view.
*********************************************************/
void cursorSetPos(RECT rcWindow, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          moveMousePointer
*PURPOSE:
*  Moves the mouse pointer to counteract map scrolling.
*********************************************************/
void moveMousePointer(updateType value);

#endif /* CURSOR_H */
