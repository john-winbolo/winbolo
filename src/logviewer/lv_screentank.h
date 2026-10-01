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
*Name:          Screen Tanks
*Filename:      screenTanks.h
*Author:        John Morrison
*Creation Date: 15/2/99
*Last Modified: 15/2/99
*Purpose:
*  Responsable for tanks on the screen
*********************************************************/

#ifndef SCREENTANKS_H
#define SCREENTANKS_H

#include "lv_global.h"

/* Empty / Non Empty / Head / Tail Macros */
#define IsEmpty(list) ((list) ==NULL)
#define NonEmpty(list) (!IsEmpty(list))
#define ScreenTanksHeadMX(list) ((list)->mx);
#define ScreenTanksHeadMY(list) ((list)->my);
#define ScreenTanksHeadPX(list) ((list)->px);
#define ScreenTanksHeadPY(list) ((list)->py);
#define ScreenTanksHeadFrame(list) ((list)->frame);
#define ScreenTanksTail(list) ((list)->next);

/* Type structure */

typedef struct {
  BYTE mx;  /* The map x co-ordinate it is on */
  BYTE my;  /* The map y co-ordinate it is on */
  BYTE px;  /* The pixel offset from the left it is on */
  BYTE py;  /* The pixel offset from the top it is on */
  BYTE wx;  /* X world offset inside the map square, 0-255. wx >> 4 == px */
  BYTE wy;  /* Y world offset inside the map square, 0-255. wy >> 4 == py */
  BYTE angle; /* The full 0-255 facing angle. frame is the 16 step version */
  BYTE frame; /* The direction it is facing */
  BYTE team;
  BYTE dir;
  bool onBoat;
  char playerName[PLAYER_NAME_LEN];
} screenPosTank;

/*typedef struct screenTanksObj *screenTanks;
struct {
  BYTE numTanksScreen; * The number of tanks on screen *
  screenPosTank pos[MAX_TANKS]; * Data for each tank *
} screenTanksObj; */


typedef struct {
  BYTE numTanksScreen; /* The number of tanks on screen */
  screenPosTank pos[MAX_TANKS];
} screenTanks;

/* Determines the tank type relative to ourselves, good, neutral or evil */
typedef enum {
  tankNone,
  tankSelf,
  tankAllie,
  tankEvil
} tankAlliance;

/* Prototypes */

/*********************************************************
*NAME:          lv_screenTanksCreate
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/99
*LAST MODIFIED: 15/2/99
*PURPOSE:
*  Sets up the screen tanks data structure
*
*ARGUMENTS:
*  value - New item to create
*********************************************************/
void lv_screenTanksCreate(screenTanks *value);

/*********************************************************
*NAME:          screenTanksPrepare
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/99
*LAST MODIFIED: 15/2/99
*PURPOSE:
*  Prepares the screenTanks data structure prior to
*  displaying
*
*ARGUMENTS:
*  value    - Pointer to the screenTanks data structure
*  leftPos  - Left bounds of the screen
*  rightPos - Right bounds of the screen
*  top      - Top bounds of the screen
*  bottom   - Bottom bounds of the screen
*********************************************************/
void screenTanksPrepare(screenTanks *value, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom);

/*********************************************************
*NAME:          lv_screenTanksGetNumEntries
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/99
*LAST MODIFIED: 15/2/99
*PURPOSE:
*  Returns the number of elements in the data structure
*
*ARGUMENTS:
*  value - Pointer to the screenTanks data structure
*********************************************************/
BYTE lv_screenTanksGetNumEntries(screenTanks *value);

/*********************************************************
*NAME:          lv_screenTanksDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/99
*LAST MODIFIED: 15/2/99
*PURPOSE:
*  Destroys and frees memory for the data structure
*
*ARGUMENTS:
*  value - Pointer to the screenTanks data structure
*********************************************************/
void lv_screenTanksDestroy(screenTanks *value);

/*********************************************************
*NAME:          lv_screenTanksAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/98
*LAST MODIFIED: 18/2/98
*PURPOSE:
*  Adds a data set for a specific tank
*
*ARGUMENTS:
*  value      - Pointer to the screenBullets data structure
*  mx         - X co-ord of the map position
*  my         - Y co-ord of the map position
*  px         - X pixel offset
*  py         - Y pixel offset
*  frame      - Frame identifer of the tank
*  playerName - String to hold the player name
*  wx         - X world offset inside the map square (wx >> 4 == px)
*  wy         - Y world offset inside the map square (wy >> 4 == py)
*  angle      - The full 0-255 facing angle
*********************************************************/
void lv_screenTanksAddItem(screenTanks *value, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame, BYTE team, BYTE dir, bool onBoat, char *playerName, BYTE wx, BYTE wy, BYTE angle);

/*********************************************************
*NAME:          lv_screenTanksGetItem
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/98
*LAST MODIFIED: 15/2/98
*PURPOSE:
*  Gets data for a specific item
*
*ARGUMENTS:
*  value      - Pointer to the screenBullets data structure
*  itemNum    - The item number to get
*  mx         - X co-ord of the map position
*  my         - Y co-ord of the map position
*  px         - X pixel offset
*  py         - Y pixel offset
*  frame      - Frame identifer of the bullet
*  playerName - String to hold the player name
*********************************************************/
void lv_screenTanksGetItem(screenTanks *value, BYTE itemNum, BYTE *mx, BYTE *my, BYTE *px, BYTE *py, BYTE *frame, BYTE *team, BYTE *dir, bool *onBoat, char *playerName);

/*********************************************************
*NAME:          lv_screenTanksGetSubPixel
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/98
*LAST MODIFIED: 15/2/98
*PURPOSE:
*  Gets the world offsets inside the map square and the
*  full facing angle for a specific item
*
*ARGUMENTS:
*  value      - Pointer to the screenTanks data structure
*  itemNum    - The item number to get
*  wx         - X world offset inside the map square
*  wy         - Y world offset inside the map square
*  angle      - The full 0-255 facing angle
*********************************************************/
void lv_screenTanksGetSubPixel(screenTanks *value, BYTE itemNum, BYTE *wx, BYTE *wy, BYTE *angle);

#endif /* SCREENTANKS_H */

