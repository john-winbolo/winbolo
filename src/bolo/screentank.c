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
*Filename:      screenTanks.c
*Author:        John Morrison
*Creation Date: 15/2/99
*Last Modified: 26/1/02
*Purpose:
*  Responsable for tanks on the screen
*********************************************************/

#include <string.h>
#include "global.h"
#include "tank.h"
#include "labels.h"
#include "messages.h"
#include "screentank.h"
#include "players.h"
#include "frontend.h"
#include "../gui/lang.h"
#include "client_sim.h"
#include "game_sim.h"

/*********************************************************
*NAME:          screenTanksCreate
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/99
*LAST MODIFIED: 15/2/99
*PURPOSE:
*  Sets up the screen tanks data structure
*
*ARGUMENTS:
*  value - New item to create
*********************************************************/
void screenTanksCreate(screenTanks *value) {
  BYTE count; /* Looping variable */
  /*  New(*value); */
  (*value).numTanksScreen = 0;
  for (count=0;count<MAX_TANKS;count++) {
    (*value).pos[count].playerName[0] = '\0';
  }
}

/*********************************************************
*NAME:          screenTanksPrepare
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/99
*LAST MODIFIED: 26/1/02
*PURPOSE:
*  Prepares the screenTanks data structure prior to
*  displaying
*
*ARGUMENTS:
*  value  - Pointer to the screenTanks data structure
*  tnk    - Pointer to your tank data structure
*  left   - Left bounds of the screen
*  right  - Right bounds of the screen
*  top    - Top bounds of the screen
*  bottom - Bottom bounds of the screen
*********************************************************/
void screenTanksPrepare(ClientSim *cs, screenTanks *value, tank *tnk, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom) {
  char playerName[PLAYER_NAME_LEN] = "\0"; /* Player Name */
  BYTE x; /* X and Y map pos of it */
  BYTE y;
  BYTE px;
  BYTE py;
  WORLD wx; /* World co-ords, for the offsets inside the map square */
  WORLD wy;
  BYTE count; /* Looping variable */

  for (count=0;count<MAX_TANKS;count++) {
    (*value).pos[count].playerName[0] = '\0';
  }
  (*value).numTanksScreen = 0;

  x = tankGetScreenMX(tnk);
  y = tankGetScreenMY(tnk);
  px = tankGetScreenPX(tnk);
  py = tankGetScreenPY(tnk);
  tankGetWorld(tnk, &wx, &wy);

  if (x >= leftPos && x <= rightPos && y >= top && y <= bottom) {
    (*value).numTanksScreen = 1;
    (*value).pos[0].mx = x - leftPos;
    (*value).pos[0].my = y - top;
    (*value).pos[0].px = px;
    (*value).pos[0].py = py;
    /* Same TANK_SUBTRACT shift as tankGetScreenPX/PY, stopping one step
       earlier so the low 8 bits are kept: wx >> 4 is px. */
    (*value).pos[0].wx = (BYTE) (WORLD) (wx - TANK_SUBTRACT);
    (*value).pos[0].wy = (BYTE) (WORLD) (wy - TANK_SUBTRACT);
    (*value).pos[0].angle = tankGet256Dir(tnk);
    (*value).pos[0].frame = tankGetFrame(tnk);
    (*value).pos[0].playerNum = clientSimGetMyPlayerNum(cs);
    /* Get the tanks names */
    (*value).pos[0].playerName[0] = '\0';
    if (!tankIsDestroyed(tnk)) {
      GameSim *gs = clientSimGetGameSim(cs);
      BYTE selfPN = clientSimGetMyPlayerNum(cs);
      playersGetPlayerName(&gs->plyrs, selfPN, playerName, sizeof(playerName),
                           FALSE);
      /* Prefer the resolved 2-char country code over the
       * "This Computer" placeholder. The country code arrives via the
       * WBN news fetch / server-side geo path and is written into
       * plrs->item[selfPN].location — exactly the same field
       * playersMakeScreenTanks reads for every other tank, so the self
       * label parses through sdl3DrawTankLabel's @<loc> splitter the
       * same as everyone else and gets the country flag drawn beside
       * it. The placeholder only kicks in pre-handshake when no
       * country has come back yet. */
      const char *selfLoc = gs->plyrs->item[selfPN].location;
      const char *labelLoc = (selfLoc[0] != '\0')
                             ? selfLoc
                             : langGetText(MESSAGE_THIS_COMPUTER);
      labelMakeTankLabel(cs, (*value).pos[0].playerName, playerName,
                         (char *)labelLoc, TRUE);
    }
  }
  /* Add the rest of the tanks as required */
  {
    GameSim *gs = clientSimGetGameSim(cs);
    playersMakeScreenTanks(cs, gs, &gs->plyrs, value, leftPos, rightPos, top, bottom);
  }
}

/*********************************************************
*NAME:          screenTanksGetNumEntries
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/99
*LAST MODIFIED: 15/2/99
*PURPOSE:
*  Returns the number of elements in the data structure
*
*ARGUMENTS:
*  value - Pointer to the screenTanks data structure
*********************************************************/
BYTE screenTanksGetNumEntries(const screenTanks *value) {
  return ((*value).numTanksScreen);
}

/*********************************************************
*NAME:          screenTanksDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/99
*LAST MODIFIED: 15/2/99
*PURPOSE:
*  Destroys and frees memory for the data structure
*
*ARGUMENTS:
*  value - Pointer to the screenTanks data structure
*********************************************************/
void screenTanksDestroy(screenTanks *value) {
/*  Dispose(*value); */
}

/*********************************************************
*NAME:          screenTanksAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/98
*LAST MODIFIED: 26/1/02
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
*  playerNum  - Player Number of this tank
*  playerName - String to hold the player name
*  wx         - X world offset inside the map square (wx >> 4 == px)
*  wy         - Y world offset inside the map square (wy >> 4 == py)
*  angle      - The full 0-255 facing angle
*********************************************************/
void screenTanksAddItem(screenTanks *value, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame, BYTE playerNum, char *playerName, BYTE wx, BYTE wy, BYTE angle) {
  (*value).pos[(*value).numTanksScreen].mx = mx;
  (*value).pos[(*value).numTanksScreen].my = my;
  (*value).pos[(*value).numTanksScreen].px = px;
  (*value).pos[(*value).numTanksScreen].py = py;
  (*value).pos[(*value).numTanksScreen].wx = wx;
  (*value).pos[(*value).numTanksScreen].wy = wy;
  (*value).pos[(*value).numTanksScreen].angle = angle;
  (*value).pos[(*value).numTanksScreen].frame = frame;
  (*value).pos[(*value).numTanksScreen].playerNum = playerNum;
  /* Get the tanks names */
  strcpy(((*value).pos[(*value).numTanksScreen].playerName), playerName);
  (*value).numTanksScreen++;
}

/*********************************************************
*NAME:          screenTanksGetItem
*AUTHOR:        John Morrison
*CREATION DATE: 15/2/98
*LAST MODIFIED: 26/1/02
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
*  frame      - Frame identifer of the tank
*  playerNum  - Player Number of this tank
*  playerName - String to hold the player name
*********************************************************/
void screenTanksGetItem(const screenTanks *value, BYTE itemNum, BYTE *mx, BYTE *my, BYTE *px, BYTE *py, BYTE *frame, BYTE *playerNum, char *playerName) {
  itemNum--;
  if (itemNum < (*value).numTanksScreen) {
    *mx = (*value).pos[itemNum].mx;
    *my = (*value).pos[itemNum].my;
    *px = (*value).pos[itemNum].px;
    *py = (*value).pos[itemNum].py;
    *frame = (*value).pos[itemNum].frame;
    *playerNum = (*value).pos[itemNum].playerNum;
    strcpy(playerName, (*value).pos[itemNum].playerName);
  }
}

/*********************************************************
*NAME:          screenTanksGetSubPixel
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
void screenTanksGetSubPixel(const screenTanks *value, BYTE itemNum, BYTE *wx, BYTE *wy, BYTE *angle) {
  itemNum--;
  if (itemNum < (*value).numTanksScreen) {
    *wx = (*value).pos[itemNum].wx;
    *wy = (*value).pos[itemNum].wy;
    *angle = (*value).pos[itemNum].angle;
  }
}

/*********************************************************
*NAME:          screenTanksGetCentreSquare
*AUTHOR:        John Morrison
*CREATION DATE: 1/10/26
*LAST MODIFIED: 1/10/26
*PURPOSE:
*  Gets the map square the tank's centre is standing on,
*  in the same frame as the item's mx/my. See screentank.h.
*
*ARGUMENTS:
*  value      - Pointer to the screenTanks data structure
*  itemNum    - The item number to get
*  mx         - Map X of the square the centre is on
*  my         - Map Y of the square the centre is on
*********************************************************/
void screenTanksGetCentreSquare(const screenTanks *value, BYTE itemNum, BYTE *mx, BYTE *my) {
  itemNum--;
  if (itemNum < (*value).numTanksScreen) {
    /* wx and wy are the low byte of the centre less TANK_SUBTRACT, so the
       centre is wx + TANK_SUBTRACT into the listed square: on the next square
       over once that reaches a whole square. */
    *mx = (BYTE) ((*value).pos[itemNum].mx +
                  (((*value).pos[itemNum].wx >= TANK_SUBTRACT) ? 1 : 0));
    *my = (BYTE) ((*value).pos[itemNum].my +
                  (((*value).pos[itemNum].wy >= TANK_SUBTRACT) ? 1 : 0));
  }
}
