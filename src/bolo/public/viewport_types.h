/*
 * Copyright (c) 1998-2026 John Morrison.
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
 *Name:          ViewPort Types
 *Filename:      viewport_types.h
 *Purpose:
 *  Tier-T1 header: visible-tile back-buffer typedefs,
 *  back-buffer size macros, and the gunsight position
 *  struct. Split out of screen.h so render/draw/frontend
 *  callers can depend on the types without pulling in the
 *  whole screen.h function facade.
 *********************************************************/

#ifndef VIEWPORT_TYPES_H
#define VIEWPORT_TYPES_H

#include "global.h"

/* Defines the screen sizes */
#define MAIN_SCREEN_SIZE_X 15
#define MAIN_SCREEN_SIZE_Y 15

/* Size of the back buffer */
#define MAIN_BACK_BUFFER_SIZE_X (MAIN_SCREEN_SIZE_X + 2)
#define MAIN_BACK_BUFFER_SIZE_Y (MAIN_SCREEN_SIZE_Y + 2)

/* The screen object - Details what tiles are on the screen */
typedef struct screenObj *screen;
struct screenObj {
  BYTE screenItem[MAIN_BACK_BUFFER_SIZE_X][MAIN_BACK_BUFFER_SIZE_Y];
};

/* Screen Mines - Array of boolean values */
typedef struct screenMineObj *screenMines;
struct screenMineObj {
  bool mineItem[MAIN_BACK_BUFFER_SIZE_X][MAIN_BACK_BUFFER_SIZE_Y];
};

/* Screen Hidden - the squares of the back buffer the player cannot see into.
 * Such a square draws the tile it last showed rather than the tile that is
 * there, and nothing moving on it is drawn. Every square is false while
 * buildings do not block sight. */
typedef struct screenHiddenObj *screenHidden;
struct screenHiddenObj {
  bool hiddenItem[MAIN_BACK_BUFFER_SIZE_X][MAIN_BACK_BUFFER_SIZE_Y];
};

/* Flag to indicate no gunsight is to be drawn */
#define NO_GUNSIGHT -1

/* Defines the gunsight position on the screen */
typedef struct {
  int mapX;
  BYTE mapY;
  BYTE pixelX;
  BYTE pixelY;
} screenGunsight;

#endif /* VIEWPORT_TYPES_H */
