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
*Name:          Screen
*Filename:      screen.c
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 17/12/03
*Purpose:
*  Provides Interfaces with the front end
*********************************************************/


/* NOTE: This really just here for historical reasons. It was present in each of the WIP releases
 * and used to scroll across the newswire as the game started.
 * Stuarts email address no longer works */
/*#define BOLO_VERSION_STRING "  WinBolo - WIP R11 (13/6/99) - DO NOT DISTRIBUTE\0"
#define OLD_BOLO_COPYRIGHT_STRING "Bolo � 1987-1995 Stuart Cheshire <>\0"
#define NEW_BOLO_COPYRIGHT_STRING "- WinBolo � 1998-1999 John Morrison <john@winbolo.com>          \0" */

/* Includes */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "../common/wb_log.h"
#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "shells.h"
#include "rubble.h"
#include "explosions.h"
#include "screenbullet.h"
#include "frontend.h"
#include "sounddist.h"
#include "messages.h"
#include "grass.h"
#include "swamp.h"
#include "building.h"
#include "tilenum.h"
#include "screencalc.h"
#include "tankexp.h"
#include "scroll.h"
#include "lgm.h"
#include "log.h"
#include "floodfill.h"
#include "minesexp.h"
#include "treegrow.h"
#include "mines.h"
#include "labels.h"
#include "players.h"
#include "screentank.h"
#include "screenlgm.h"
#include "screenbrainmap.h"
#include "screen.h"
#include "client_state.h"
#include "interpolation.h"
#include "util.h"
#include "client_sim.h"
#include "viewport.h"
#include "../server/server_sim.h"
#include <SDL3/SDL.h>
#include "../steam/steam_wrapper.h"
/* Forward declaration — implemented in gui/sdl3/cursor.c */
extern void moveMousePointer(updateType value);

/* clientCenterTankCS is declared in screen.h (extern) so client_snapshot.c
 * can call it; the bot path never invokes it (guarded by isHuman/keypress
 * checks) but the linker still needs to resolve the symbol. */

/* Module Level Variables */

/* Display statics removed — now fields of ClientSim (see client_sim.h) */

/*********************************************************
*NAME:          screenCalcSquare
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 30/01/02
*PURPOSE:
*  Calculates the terrain type for a given location
*
*ARGUMENTS:
*  xValue - The x co-ordinate
*  yValue - The y co-ordinate
*********************************************************/
BYTE screenCalcSquareCS(ClientSim *csPtr, BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY) {
  return viewportCalcSquare(clientSimViewportMut(csPtr), clientSimGetGameSim(csPtr),
                            clientSimGetMyPlayerNum(csPtr), xValue, yValue, scrX, scrY);
}


/*********************************************************
*NAME:          screenTranslateBrainButtons
*AUTHOR:        John Morrison
*CREATION DATE: 27/11/99
*LAST MODIFIED: 27/11/99
*PURPOSE:
* Translates the brain's key pressed into our keys presses
*.
*ARGUMENTS:
* isShoot    - Pointer to hold if we are shooting
* isGameTick - TRUE if this is a game tick
*********************************************************/
tankButton screenTranslateBrainButtonsCS(ClientSim *csPtr, bool *isShoot, bool isGameTick) {
  tankButton returnValue; /* Value to return */
  unsigned long temp;     /* Used to temp store brainHoldKeys */
  unsigned long temp2;    /* Used to temp store keyShoot if not game tick */
  uint32_t *holdKeys = clientSimGetBrainHoldKeys(csPtr);
  uint32_t *tapKeys = clientSimGetBrainTapKeys(csPtr);

  temp = *holdKeys;
  temp2 = 0;

  /* Handle tap keys */
  if (testkey(*tapKeys, KEY_faster)) {
    setkey(*holdKeys, KEY_faster);
  }
  if (testkey(*tapKeys, KEY_slower)) {
    setkey(*holdKeys, KEY_slower);
  }
  if (testkey(*tapKeys, KEY_turnleft)) {
    setkey(*holdKeys, KEY_turnleft);
  }
  if (testkey(*tapKeys, KEY_turnright)) {
    setkey(*holdKeys, KEY_turnright);
  }
  if (testkey(*tapKeys, KEY_morerange)) {
    tankGunsightIncrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*tapKeys, KEY_lessrange)) {
    tankGunsightDecrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*tapKeys, KEY_shoot)) {
    if (isGameTick == TRUE) {
      *isShoot = TRUE;
    } else {
      setkey(temp2, KEY_shoot);
    }
  }
  if (testkey(*tapKeys, KEY_dropmine)) {
    tankLayMine(clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*tapKeys, KEY_TankView)) {
    clientSimSetInPillView(csPtr, FALSE);
    clientCenterTankCS(csPtr);
  }
  if (testkey(*tapKeys, KEY_PillView)) {
    clientSimPillView(csPtr, 0, 0);
  }

  *tapKeys = temp2;

  /* Do the lookups */
  if (testkey(*holdKeys, KEY_faster) && testkey(*holdKeys, KEY_turnright)) {
    returnValue = TRIGHTACCEL;
  } else if (testkey(*holdKeys, KEY_faster) && testkey(*holdKeys, KEY_turnleft)) {
    returnValue = TLEFTACCEL;
  } else if (testkey(*holdKeys, KEY_slower) && testkey(*holdKeys, KEY_turnleft)) {
    returnValue = TLEFTDECEL;
  } else if (testkey(*holdKeys, KEY_slower) && testkey(*holdKeys, KEY_turnright)) {
    returnValue = TRIGHTDECEL;
  } else if (testkey(*holdKeys, KEY_faster)) {
    returnValue = TACCEL;
  } else if (testkey(*holdKeys, KEY_slower)) {
    returnValue = TDECEL;
  } else if (testkey(*holdKeys, KEY_turnleft)) {
    returnValue = TLEFT;
  } else if (testkey(*holdKeys, KEY_turnright)) {
    returnValue = TRIGHT;
  } else {
    returnValue = TNONE;
  }

  /* Restore it */
  *holdKeys = temp;

  /* Handle remaining keys */
  if (testkey(*holdKeys, KEY_morerange)) {
    tankGunsightIncrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*holdKeys, KEY_lessrange)) {
    tankGunsightDecrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*holdKeys, KEY_shoot) && isGameTick == TRUE) {
    *isShoot = TRUE;
  }
  if (testkey(*holdKeys, KEY_dropmine)) {
    tankLayMine(clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*holdKeys, KEY_TankView)) {
    clientSimSetInPillView(csPtr, FALSE);
    clientCenterTankCS(csPtr);
  }
  if (testkey(*holdKeys, KEY_PillView)) {
    clientSimPillView(csPtr, 0, 0);
  }

  return returnValue;
}

/* screenGameTick and screenKeysTick have been removed.
 * All frontends now use the server-authoritative path:
 *   clientSimKeysTick / clientSimGameTick (prediction)
 *   + transport->sendInput / transport->tick / transport->getSnapshot
 *   + clientSimSyncFromSnapshot (reconciliation)
 *   + clientSimDisplayTick (display updates)
 */

/*********************************************************
*NAME:          screenGunsightRange
*AUTHOR:        John Morrison
*CREATION DATE: 24/12/98
*LAST MODIFIED: 24/12/98
*PURPOSE:
*  Changes the gunsight range
*
*ARGUMENTS:
*  increase - Set to TRUE if should increase else 
*             decrease range
*********************************************************/
void screenGunsightRangeCS(ClientSim *csPtr, bool increase) {
  if (increase == TRUE) {
    tankGunsightIncrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  } else {
    tankGunsightDecrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
}


/*********************************************************
*NAME:          screenGetMessages
*AUTHOR:        John Morrison
*CREATION DATE: 3/1/99
*LAST MODIFIED: 3/1/99
*PURPOSE:
*  Gets the messages on screen
*
*ARGUMENTS:
*  top - Top message line to get
*  top - Top message line to get
*********************************************************/
void screenGetMessages(ClientSim *csPtr, char *top, char *bottom) {
  messageGetMessage(clientSimGetMessages(csPtr), top,bottom);
}

/*********************************************************
*NAME:          clientCenterTank
*AUTHOR:        John Morrison
*CREATION DATE: 5/11/99
*LAST MODIFIED: 5/11/99
*PURPOSE:
*  Centers the screen around the tank.
*
*ARGUMENTS:
*
*********************************************************/
void clientCenterTankCS(ClientSim *csPtr) {
  viewportCenterOnTank(clientSimViewportMut(csPtr), clientSimGetScroll(csPtr), MY_TANK(csPtr));
}

/*********************************************************
*NAME:          screenShowMessages
*AUTHOR:        John Morrison
*CREATION DATE:  8/1/99
*LAST MODIFIED:  1/6/00
*PURPOSE:
*  Turns on/off messages menus stuff
*
*ARGUMENTS:
*  msgType - The message type that is being set
*  isShown - Is it being turned on or off
*********************************************************/
void screenShowMessages(ClientSim *csPtr, BYTE msgType, bool isShown) {
  switch (msgType) {
  case MSG_NEWSWIRE:
    messageSetNewswire(clientSimGetMessages(csPtr), isShown);
    break;
  case MSG_ASSISTANT:
    messageSetAssistant(clientSimGetMessages(csPtr), isShown);
    break;
  case MSG_AI:
    messageSetAI(clientSimGetMessages(csPtr), isShown);
    break;
  case MSG_NETSTATUS:
    messageSetNetStatus(clientSimGetMessages(csPtr), isShown);
    break;
  default:
    /* MSG_Network */
    messageSetNetwork(clientSimGetMessages(csPtr), isShown);
    break;
  }
}

/*********************************************************
*NAME:          screenManMove
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 27/5/00
*PURPOSE:
* The mouse has been clicked indicating a build operation
* is requested. Send request to lgm structure
*
*ARGUMENTS:
*  buildS - The building type selected
*********************************************************/
void screenManMoveCS(ClientSim *csPtr, buildSelect buildS) {
  clientSimManMove(csPtr, buildS);
}


/*********************************************************
*NAME:          screenSetAutoScroll
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 16/1/99
*PURPOSE:
* The autoscrolling item has been checked on the frontend
* Pass the value onto the scroll module.
*
*ARGUMENTS:
*  isAuto - Is the scrolling option automatic or not?
*********************************************************/
void screenSetAutoScroll(ClientSim *csPtr, bool isAuto) {
  scrollSetScrollType(clientSimGetScroll(csPtr), isAuto);
}

/*********************************************************
*NAME:          screenLgmDropPill
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 14/3/99
*PURPOSE:
* Man has died and needs to drop a pill. Pass this onto
* the pillbox structure
*
*ARGUMENTS:
*  mx      - Pointer to hold Map X Co-ordinates
*  my      - Pointer to hold Map Y Co-ordinates
*  owner   - Who owns the pill
*  pillNum - Pill number to place 
*********************************************************/
void screenLgmDropPillCS(ClientSim *csPtr, BYTE mx, BYTE my, BYTE owner, BYTE pillNum) {
  pillbox item;   /* Item to add to the pillbox */

  item.armour = 0;
  item.owner = owner;
  item.speed = PILLBOX_ATTACK_NORMAL;
  item.reload = PILLBOX_ATTACK_NORMAL;
  item.coolDown = 0;
  item.inTank = FALSE;
  item.x = mx;
  item.y = my;
  item.justSeen = FALSE;
  pillsSetPill(&clientSimGetGameSim(csPtr)->pb,&item, pillNum);
  frontEndStatusPillbox(csPtr, pillNum, (pillsGetAllianceNum(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, pillNum)));
}

/*********************************************************
*NAME:          screenTankLayMine
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/99
*LAST MODIFIED: 21/1/99
*PURPOSE:
* Request for tank to lay mine has occured
*
*ARGUMENTS:
*
*********************************************************/
void screenTankLayMineCS(ClientSim *csPtr) {
  tankLayMine(clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
}

/*********************************************************
*NAME:          screenCheckTankMineDamage
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/99
*LAST MODIFIED: 21/1/99
*PURPOSE:
* Check for damage to a tank by a mine being set off.
*
*ARGUMENTS:
*  mx - X Map Co-ordinate
*  my - Y Map Co-ordinate
*********************************************************/
void screenCheckTankMineDamageCS(ClientSim *csPtr, BYTE mx, BYTE my) {
  tankMineDamage(clientSimGetGameSim(csPtr), &MY_TANK(csPtr), mx, my);
}

/*********************************************************
*NAME:          screenSetAllowHiddenMines
*AUTHOR:        John Morrison
*CREATION DATE: 26/2/99
*LAST MODIFIED: 26/2/99
*PURPOSE:
* Sets whether hidden mines are allowed or not 
*
*ARGUMENTS:
*  hidden - TRUE if hidden mines are allowed
*********************************************************/
void screenSetAllowHiddenMinesCS(ClientSim *csPtr, bool hidden) {
  minesDestroy(&clientSimGetGameSim(csPtr)->mns);
  minesCreate(&clientSimGetGameSim(csPtr)->mns, hidden);
}


/*********************************************************
*NAME:          screenGetGameStartDelay
*AUTHOR:        John Morrison
*CREATION DATE: 27/1/99
*LAST MODIFIED: 27/1/99
*PURPOSE:
* Returns the time remaining to start the current game
*
*ARGUMENTS:
*
*********************************************************/
int32_t screenGetGameStartDelayCS(ClientSim *csPtr) {
  return clientSimGetGmeStartDelay(csPtr);
}



/*********************************************************
*NAME:          screenSetLabelOwnTank
*AUTHOR:        John Morrison
*CREATION DATE: 2/2/99
*LAST MODIFIED: 2/2/99
*PURPOSE:
* Sets whether should label itself to the value passed
*
*ARGUMENTS:
*  value - Should the tank be labeled?
*********************************************************/
void screenSetLabelOwnTank(ClientSim *csPtr, bool value) {
  labelSetLabelOwnTank(csPtr, value);
}

/*********************************************************
*NAME:          screenSetMesageLabelLen
*AUTHOR:        John Morrison
*CREATION DATE: 2/2/99
*LAST MODIFIED: 2/2/99
*PURPOSE:
* Sets the message label len to the value passed
*
*ARGUMENTS:
*  value - New length of the labels
*********************************************************/
void screenSetMesageLabelLen(ClientSim *csPtr, labelLen value) {
  labelSetSenderLength(csPtr, value);
}

/*********************************************************
*NAME:          screenSetTankLabelLen
*AUTHOR:        John Morrison
*CREATION DATE: 2/2/99
*LAST MODIFIED: 2/2/99
*PURPOSE:
* Sets the tank label len to the value passed
*
*ARGUMENTS:
*  value - New length of the labels
*********************************************************/
void screenSetTankLabelLen(ClientSim *csPtr, labelLen value) {
  labelSetTankLength(csPtr, value);
}

/*********************************************************
*NAME:          screenTanksAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns the tank allience of a specific player slot.
* sucessful or not.
*
*ARGUMENTS:
*  fileName - path and filename to save
*********************************************************/
tankAlliance screenTankAllianceCS(ClientSim *csPtr, BYTE playerNum) {
  return clientSimGetTankAlliance(csPtr, playerNum);
}


/*********************************************************
*NAME:          screenIsItemInTrees
*AUTHOR:        John Morrison
*CREATION DATE: 19/2/99
*LAST MODIFIED:  6/1/00
*PURPOSE:
* Returns whether a item is surrounded by trees.
*
*ARGUMENTS:
*  bmx - X position
*  bmy - Y position
*********************************************************/
bool screenIsItemInTrees(GameSim *sim, tank viewerTank, WORLD bmx, WORLD bmy) {
  bool returnValue; /* Value to return */
  int xDiff;        /* X and Y differences in location */
  int yDiff;

  xDiff = tankGetScreenMX(&viewerTank) - bmx;
  yDiff = tankGetScreenMY(&viewerTank) - bmy;
  if (xDiff >= MIN_SIGHT_DISTANCE_LEFT && xDiff <= MIN_SIGHT_DISTANCE_RIGHT && yDiff >= MIN_SIGHT_DISTANCE_LEFT && yDiff <= MIN_SIGHT_DISTANCE_RIGHT) {
    returnValue = FALSE;
  } else {
    returnValue = utilIsTankInTrees(&sim->mp, &sim->pb, &sim->bs, bmx, bmy);
  }
  return returnValue;
}

/*********************************************************
*NAME:          screenGetNumNeutralPills
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Returns the number of neutral pills
*
*ARGUMENTS:
*
*********************************************************/
BYTE screenGetNumNeutralPillsCS(ClientSim *csPtr) {
  return pillsGetNumNeutral(&clientSimGetGameSim(csPtr)->pb);
}

/*********************************************************
*NAME:          screenGetTimeGameCreated
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Returns the time the game was created
*
*ARGUMENTS:
*
*********************************************************/
int32_t screenGetTimeGameCreatedCS(ClientSim *csPtr) {
  return (int32_t) clientSimGetTimeStart(csPtr);
}

/*********************************************************
*NAME:          screenSetTimeGameCreated
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Sets the time the game was created
*
*ARGUMENTS:
*  value - Time to set it to
*********************************************************/
void screenSetTimeGameCreatedCS(ClientSim *csPtr, int32_t value) {
  clientSimSetTimeStart(csPtr, value);
}

/*********************************************************
*NAME:          screenSetMapName
*AUTHOR:        John Morrison
*CREATION DATE: 26/2/99
*LAST MODIFIED: 26/2/99
*PURPOSE:
* Sets the map name to the parameter passed.
* (Came from network module)
*
*ARGUMENTS:
*  name - Map name
*********************************************************/
void screenSetMapNameCS(ClientSim *csPtr, char *name) {
  strcpy(clientSimGetMapNameMutable(csPtr), name);
}

/*********************************************************
*NAME:          screenSetTimeLengths
*AUTHOR:        John Morrison
*CREATION DATE: 26/2/99
*LAST MODIFIED: 26/2/99
*PURPOSE:
* Sets the time lengths. (Came from network module)
*
*ARGUMENTS:
*  srtDelay    - Game start delay (50th second increments)
*  gmeLen      - Length of the game (in 50ths) 
*                (-1 =unlimited)
*********************************************************/
void screenSetTimeLengthsCS(ClientSim *csPtr, int srtDelay, int32_t gmeLen) {
  clientSimSetGmeStartDelay(csPtr, srtDelay);
  clientSimSetGmeLength(csPtr, gmeLen);
}


/*********************************************************
*NAME:          screenSetGameType
*AUTHOR:        John Morrison
*CREATION DATE: 26/2/99
*LAST MODIFIED: 26/2/99
*PURPOSE:
* Sets the game type. (Came from network module)
*
*ARGUMENTS:
*  gt - The game type to set to
*********************************************************/
void screenSetGameTypeCS(ClientSim *csPtr, gameType gt) {
  gameTypeSet(&clientSimGetGameSim(csPtr)->game, gt);
}

/*********************************************************
*NAME:          screenChangeOwnership
*AUTHOR:        John Morrison
*CREATION DATE: 2/11/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* Called because we have lost our connection to the server
* Set all of our allies stuff to be owned by us.
*
*ARGUMENTS:
*  oldOwner - The old Owner of the stuff
*********************************************************/
void screenChangeOwnershipCS(ClientSim *csPtr, BYTE oldOwner) {
  basesMigrate(clientSimGetGameSim(csPtr), oldOwner, clientSimGetMyPlayerNum(csPtr));
  pillsMigrate(clientSimGetGameSim(csPtr), oldOwner, clientSimGetMyPlayerNum(csPtr));
}

/*********************************************************
*NAME:          screenGetLgmStatus
*AUTHOR:        John Morrison
*CREATION DATE: 14/11/99
*LAST MODIFIED: 14/11/99
*PURPOSE:
*  Gets the man status for drawing on the status bars
*
*ARGUMENTS:
*  isOut  - TRUE if man is out of tank
*  isDead - TRUE if man is dead
*  angle  - Angle man is travelling on
*********************************************************/
void screenGetLgmStatusCS(ClientSim *csPtr, bool *isOut, bool *isDead, TURNTYPE *angle) {
  lgmGetStatus(&MY_LGM(csPtr), &MY_TANK(csPtr), isOut, isDead, angle);
}

/*********************************************************
*NAME:          screenGetSubMapSquareOffset
*AUTHOR:        John Morrison
*CREATION DATE: 18/11/99
*LAST MODIFIED: 18/11/99
*PURPOSE:
*  Gets the map sqaure sub offsets.
*
*ARGUMENTS:
*  xPos - Pointer to hold X maps square offset
*  yPos - Pointer to hold Y maps square offset
*********************************************************/
void screenGetSubMapSquareOffset(int *xPos, int *yPos) {
  *xPos = 0; /* edgeX; */
  *yPos = 0; /* edgeY; */
}





/*********************************************************
*NAME:          screenSetCursorPos
*AUTHOR:        John Morrison
*CREATION DATE: 27/05/00
*LAST MODIFIED: 27/05/00
*PURPOSE:
*  Sets the cursor position in steps from the top and 
*  left corner position of the active screen
*
*ARGUMENTS:
*  posX - Left position
*  posY - Top position
*********************************************************/
void screenSetCursorPosCS(ClientSim *csPtr, BYTE posX, BYTE posY) {
  clientSimSetCursorPos(csPtr, posX, posY);
}

/*********************************************************
*NAME:          screenNetStatusMessage
*AUTHOR:        John Morrison
*CREATION DATE: 1/6/00
*LAST MODIFIED: 1/6/00
*PURPOSE:
*  A network status message has arrived from the server
*
*ARGUMENTS:
*  messageStr - The message text
*********************************************************/
void screenNetStatusMessage(ClientSim *csPtr, char *messageStr) {
  clientMessageAdd(clientSimGetMessages(csPtr), networkStatus, (char *) "Network Status", messageStr);
}

/*********************************************************
*NAME:          screenTankStopCarryingPill
*AUTHOR:        John Morrison
*CREATION DATE: 21/6/00
*LAST MODIFIED: 21/6/00
*PURPOSE:
* Someone else has picked up a pill. We should check that
* we aren't carrying it ourselves and if so drop it (The
* server said so) because this can lead to desync problems
*
*ARGUMENTS:
*  message - The message text
*********************************************************/
void screenTankStopCarryingPillCS(ClientSim *csPtr, BYTE itemNum) {
  tankStopCarryingPill(&MY_TANK(csPtr), itemNum);
}

/*********************************************************
*NAME:          screenSetPlayersMenu
*AUTHOR:        John Morrison
*CREATION DATE: 9/02/02
*LAST MODIFIED: 9/02/02
*PURPOSE:
* Sets the player menu entries
*
*ARGUMENTS:
*
*********************************************************/
void screenSetPlayersMenuCS(ClientSim *csPtr) {
  playersSetPlayersMenu(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), FALSE);
}




