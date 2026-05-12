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
*Name:          Players
*Filename:      players.c
*Author:        John Morrison
*Creation Date: 09/02/02
*Last Modified: 09/08/04
*Purpose:
*  Looks after players. Alliences between etc.
*********************************************************/

#include <math.h>
#include <string.h>

#include "allience.h"
#include "bases.h"
#include "frontend.h"
#include "global.h"
#include "labels.h"
#include "lgm.h"
#include "log.h"
#include "messages.h"
#include "pillbox.h"
#include "players.h"
#include "playername_validate.h"
#include "screen.h"
#include "client_sim.h"
#include "screenlgm.h"
#include "screentank.h"
#include "tank.h"
#include "tilenum.h"
#include "game_sim.h"
#include "gametype.h"
#include "util.h"


/*********************************************************
*NAME:          playersCreate
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
* Sets up the players structure.
*
*ARGUMENTS:
* plrs - Pointer to the players object to create
*********************************************************/
void playersCreate(players *plrs, bool isServer) {
  BYTE count; /* Looping variable */

  New(*plrs);
  memset(*plrs, 0, sizeof(**plrs));
    
  
  for (count = 0;count<MAX_TANKS;count++) {
    (*plrs)->item[count].inUse = FALSE;
    (*plrs)->item[count].needUpdate = FALSE;
    (*plrs)->item[count].allie = allienceCreate();
    (*plrs)->item[count].isChecked = FALSE;
    (*plrs)->item[count].ping = 0;
    (*plrs)->item[count].clientFlags = 0;
    (*plrs)->item[count].clientType = CLIENT_TYPE_UNKNOWN;
    (*plrs)->playerBrainNames[count][0] = '\0';
  }
}

/*********************************************************
*NAME:          playersDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Destroys the playesr structure
*
*ARGUMENTS:
* plrs - Pointer to the players object 
*********************************************************/
void playersDestroy(players *plrs) {
  BYTE count; /* Looping variable */

  if ((*plrs) != NULL) {
    for (count = 0;count<MAX_TANKS;count++) {
      (*plrs)->item[count].inUse = FALSE;
      allienceDestroy(&((*plrs)->item[count].allie));
    }
    if (*plrs != NULL) {
      Dispose(*plrs);
    }
    (*plrs) = NULL;
  }
}

/*********************************************************
*NAME:          playersSetSelf
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
* Sets your own player number and player name. Returns 
* whether the operation succeeded.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum  - The player number to set
* playerName - The player name to set
*********************************************************/
bool playersSetSelf(ClientSim *csParam, GameSim *sim, players *plrs, BYTE playerNum, char *playerName, bool isServer) {
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  if ((*plrs)->item[playerNum].inUse == FALSE) {
    if (playersNameTaken(plrs, playerName) == FALSE) {
      if (csParam) strcpy(csParam->myLastPlayerName, playerName);
      returnValue = TRUE;
      strcpy((*plrs)->item[playerNum].playerName, playerName);
      (*plrs)->item[playerNum].inUse = TRUE;
      lgmSetPlayerNum(&sim->lgmen[playerNum], playerNum);
      utilCtoPString(playerName, (char *) (*plrs)->playerBrainNames[playerNum]);
      if (isServer == FALSE) {
        frontEndSetPlayer(csParam, (playerNumbers) playerNum, playerName, "", 0, CLIENT_TYPE_UNKNOWN, 0);
      }
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          playersSetPlayerName
*AUTHOR:        John Morrison
*CREATION DATE: 18/02/99
*LAST MODIFIED: 05/05/01
*PURPOSE:
* Sets/changes a player name. Returns whether the operation
* succeed or not. (Fails if name is already in use) If it
* sccueeds then it makes the appropriate anouncement
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum  - The player number to set
* playerName - The player name to set
*********************************************************/
bool playersSetPlayerName(ClientSim *csParam, GameSim *sim, players *plrs, BYTE selfPlayer, BYTE playerNum, char *playerName, bool isServer) {
  bool returnValue;           /* Value to return */
  char temp[FILENAME_MAX];

  returnValue = FALSE;
  if ((*plrs)->item[playerNum].inUse == TRUE) {
    if (playersNameTaken(plrs, playerName) == FALSE) {
      /* OK to change do so and then make the message */
      returnValue = TRUE;
      /* Make Message */
      {
        MessageArgs args;
        memset(&args, 0, sizeof(args));
        /* New name (playerName) and old name (otherName) refer to the
         * same player, so both decoration sets come from playerNum. */
        strncpy(args.otherName, (*plrs)->item[playerNum].playerName, PLAYER_NAME_LEN - 1);
        strncpy(args.playerName, playerName, PLAYER_NAME_LEN - 1);
        args.playerFlags = playersGetAccountFlags(plrs, playerNum);
        playersGetCountryCode(plrs, playerNum, args.playerCountry);
        args.otherFlags = args.playerFlags;
        memcpy(args.otherCountry, args.playerCountry, sizeof(args.otherCountry));
        sim->callbacks.messageAdd(sim->callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_CHANGENAME, &args);
      }
      /* Update the name. Defense-in-depth bound (Phase 9 / Phase 5):
       * upstream validators cap names at the wire size, but harden the
       * raw write against a future validator regression. */
      strncpy((*plrs)->item[playerNum].playerName, playerName, PLAYER_NAME_LAST);
      (*plrs)->item[playerNum].playerName[PLAYER_NAME_LAST] = '\0';
      utilCtoPString(playerName, (char *) (*plrs)->playerBrainNames[playerNum]);
      strcpy(temp, playerName);
      if (playerNum != selfPlayer) {
        strcat(temp, "@");
        strcat(temp, (*plrs)->item[playerNum].location);
      } else if (isServer == FALSE && csParam) {
        strcpy(csParam->myLastPlayerName, playerName);
      }
      if (isServer == FALSE) {
        frontEndSetPlayer(csParam, (playerNumbers) playerNum, temp,
                          (*plrs)->item[playerNum].location,
                          (*plrs)->item[playerNum].ping,
                          playersGetClientType(plrs, playerNum),
                          playersGetClientFlags(plrs, playerNum));
      }
      /* Log it */
      logAddEvent(log_ChangeName, playerNum, 0, 0, 0, 0, (*plrs)->playerBrainNames[playerNum]);
    }
  }
  return returnValue;
}


/*********************************************************
*NAME:          playersSetPlayersMenu
*AUTHOR:        John Morrison
*CREATION DATE: 09/02/02
*LAST MODIFIED: 09/02/02
*PURPOSE:
* Sets the entries in the players menu if they are in use
*
*ARGUMENTS:
* plrs - Pointer to the players object 
********************************************************/
void playersSetPlayersMenu(ClientSim *csParam, players *plrs, BYTE selfPlayer, bool isServer) {
  BYTE count; /* Looping variable */
  char temp[FILENAME_MAX];

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE) {
      strcpy(temp, (*plrs)->item[count].playerName);
      if (count != selfPlayer) {
        strcat(temp, "@");
        strcat(temp, (*plrs)->item[count].location);
      }
      if (isServer == FALSE) {
        frontEndSetPlayer(csParam, (playerNumbers) count, temp,
                          (*plrs)->item[count].location,
                          (*plrs)->item[count].ping,
                          playersGetClientType(plrs, count),
                          playersGetClientFlags(plrs, count));
      }
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersSetPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
* Sets a player up.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum  - The player number to set
* pla7yerName - The player name to set
* location   - Location string of the player.
* mx         - Map X location
* my         - Map Y location
* px         - Pixel X location
* py         - Pixel Y location
* frame      - Tank animation frame.
* onBoat     - is the player on a boat?
* numAllies  - Number of Allies the player has
* allies     - BYTE buffer containing each allie
*********************************************************/
void playersSetPlayer(ClientSim *csParam, players *plrs, BYTE selfPlayer, BYTE playerNum, char *playerName, char *location, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame, bool onBoat, BYTE numAllies, BYTE *allies, bool isServer) {
  BYTE count; /* Looping variable */
  char str[512]; /* The player name */
  int iMyPlayerNum;
  int iPlayerNum;

  iMyPlayerNum = (int)selfPlayer;
  iPlayerNum = (int)playerNum;

  /* This code block is only executed for clients besides your own in the player list */
  if ((*plrs)->item[playerNum].inUse == FALSE) {
    (*plrs)->item[playerNum].inUse = TRUE;
    strcpy((*plrs)->item[playerNum].playerName, playerName);
    (*plrs)->item[playerNum].location[0] = location[0];
    (*plrs)->item[playerNum].location[1] = location[1];
    (*plrs)->item[playerNum].location[2] = '\0';
    utilCtoPString(playerName, (char *) ((*plrs)->playerBrainNames[playerNum]));
    (*plrs)->item[playerNum].mapX = mx;
    (*plrs)->item[playerNum].mapY = my;
    (*plrs)->item[playerNum].pixelX = px;
    (*plrs)->item[playerNum].pixelY = py;
    (*plrs)->item[playerNum].frame = frame;
    (*plrs)->item[playerNum].onBoat = onBoat;
    (*plrs)->item[playerNum].allie = allienceCreate();
    count = 0;
    while (count < numAllies) {
      allienceAdd(&((*plrs)->item[playerNum].allie), allies[count]);
      count++;
    }
  }
  else if (iMyPlayerNum == iPlayerNum) {
    /* Processing our client, store location for the network info */
    (*plrs)->item[playerNum].location[0] = location[0];
    (*plrs)->item[playerNum].location[1] = location[1];
    (*plrs)->item[playerNum].location[2] = '\0';
  } else {
    /* Already registered (e.g. auto-registered from snapshot with a
       placeholder name) — update name and location from authoritative
       source such as PACKET_PLAYER_LIST. Also rebuild the alliance list:
       PLAYER_LIST carries the authoritative alliance bitmap, and the
       snapshot auto-register path leaves the list empty so server-driven
       team alliances would otherwise never reach the client view. */
    strcpy((*plrs)->item[playerNum].playerName, playerName);
    (*plrs)->item[playerNum].location[0] = location[0];
    (*plrs)->item[playerNum].location[1] = location[1];
    (*plrs)->item[playerNum].location[2] = '\0';
    utilCtoPString(playerName, (char *) ((*plrs)->playerBrainNames[playerNum]));
    allienceDestroy(&((*plrs)->item[playerNum].allie));
    (*plrs)->item[playerNum].allie = allienceCreate();
    count = 0;
    while (count < numAllies) {
      allienceAdd(&((*plrs)->item[playerNum].allie), allies[count]);
      count++;
    }
  }

  /* Update front end if we are in a running game (ie not in the joining phase) */
  if (csParam == NULL || csParam->netStat != netFailed) {
    strcpy(str, (*plrs)->item[playerNum].playerName);
    if (playerNum != selfPlayer && (*plrs)->item[playerNum].location[0] != '\0') {
      strcat(str, " (");
      strcat(str, (*plrs)->item[playerNum].location);
      strcat(str, ")");
    }
    if (isServer == FALSE) {
      frontEndSetPlayer(csParam, (playerNumbers) playerNum, str,
                        (*plrs)->item[playerNum].location,
                        (*plrs)->item[playerNum].ping,
                        playersGetClientType(plrs, playerNum),
                        playersGetClientFlags(plrs, playerNum));
      frontEndStatusTank(csParam, (BYTE) (playerNum+1), playersScreenAllience(plrs, selfPlayer, playerNum));
      frontEndRedrawAll(csParam);
    }
  }

}


/*********************************************************
*NAME:          playerSetLocation
*AUTHOR:        John Morrison
*CREATION DATE: 10/04/01
*LAST MODIFIED: 10/04/01
*PURPOSE:
* Sets the location of the player at ip to the hostname
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* ip   - Current IP address of the player
* host - Hostname of the player
*********************************************************/
void playerSetLocation(players *plrs, char *ip, char *host) {
  /* Legacy DNS callback — no longer used since country codes come from the server.
     Kept for API compatibility but does nothing. */
  (void)plrs;
  (void)ip;
  (void)host;
}

/*********************************************************
*NAME:          playersUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 28/2/99
*PURPOSE:
* Updates a player with specific location data.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum  - The player number to set
* mx         - Map X location
* my         - Map Y location
* px         - Pixel X location
* py         - Pixel Y location
* frame      - Tank animation frame.
* onBoat     - is the player on a boat?
* lgmMX      - Lgm Map X Position
* lgmMY      - Lgm Map Y Position
* lgmPX      - Lgm Map X Position
* lgmPY      - Lgm Map Y Position
* lgmFrame   - Lgm Frame number
*********************************************************/
void playersUpdate(players *plrs, BYTE playerNum, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame, bool onBoat, BYTE lgmMX, BYTE lgmMY, BYTE lgmPX, BYTE lgmPY, BYTE lgmFrame) {
 if ((*plrs)->item[playerNum].inUse == TRUE) {
    (*plrs)->item[playerNum].mapX = mx;
    (*plrs)->item[playerNum].mapY = my;
    (*plrs)->item[playerNum].pixelX = px;
    (*plrs)->item[playerNum].pixelY = py;
    (*plrs)->item[playerNum].frame = frame;
    (*plrs)->item[playerNum].onBoat = onBoat;
    (*plrs)->item[playerNum].lgmMapX = lgmMX;
    (*plrs)->item[playerNum].lgmMapY = lgmMY;
    (*plrs)->item[playerNum].lgmPixelX = lgmPX;
    (*plrs)->item[playerNum].lgmPixelY = lgmPY;
    (*plrs)->item[playerNum].lgmFrame = lgmFrame;
  }
}

/*********************************************************
*NAME:          playersGameTickUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 15/09/02
*LAST MODIFIED: 15/09/02
*PURPOSE:
* Updates a player with specific location data.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum  - The player number to set
* mx         - Map X location
* my         - Map Y location
* px         - Pixel X location
* py         - Pixel Y location
* frame      - Tank animation frame.
* onBoat     - is the player on a boat?
* lgmMX      - Lgm Map X Position
* lgmMY      - Lgm Map Y Position
* lgmPX      - Lgm Map X Position
* lgmPY      - Lgm Map Y Position
* lgmFrame   - Lgm Frame number
*********************************************************/
void playersGameTickUpdate(players *plrs) {
  BYTE count; /* Looping variable */

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE) {
      if ((*plrs)->item[count].lgmMapX != 0 && (*plrs)->item[count].lgmMapY != 0 && (*plrs)->item[count].lgmFrame != LGM_HELICOPTER_FRAME) {
        (*plrs)->item[count].lgmFrame++;
        if ((*plrs)->item[count].lgmFrame > LGM_MAX_FRAMES ) {
          (*plrs)->item[count].lgmFrame = 0;
        }
      }
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersGetPlayerName
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Gets a player name.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum  - The player number to set
* dest       - Destination string
*********************************************************/
void playersGetPlayerName(players *plrs, BYTE playerNum, char *dest, bool isServer) {
  if (plrs != NULL) {
    if (playerNum >= MAX_TANKS) {
      strcpy(dest, NO_TANK);
      return;
    }
    if ((*plrs)->item[playerNum].inUse == TRUE) {
      strcpy(dest, (*plrs)->item[playerNum].playerName);
    } else {
      strcpy(dest, NO_TANK);
    }
  } else {
    strcpy(dest, NO_TANK);
  }
}

/*********************************************************
*NAME:          playersGetPlayerLocation
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Gets a player location.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum  - The player number to set
* dest       - Destination string
*********************************************************/
void playersGetPlayerLocation(players *plrs, BYTE playerNum, char *dest) {
  if (plrs != NULL) {
    if ((*plrs)->item[playerNum].inUse == TRUE) {
      strcpy(dest, (*plrs)->item[playerNum].location);
    } else {
      strcpy(dest, NO_TANK);
    }
  } 
}

void playersGetCountryCode(players *plrs, BYTE playerNum, char *dest) {
  if (plrs != NULL && (*plrs)->item[playerNum].inUse == TRUE) {
    dest[0] = (*plrs)->item[playerNum].location[0];
    dest[1] = (*plrs)->item[playerNum].location[1];
    dest[2] = '\0';
  } else {
    dest[0] = 'X';
    dest[1] = 'X';
    dest[2] = '\0';
  }
}

uint8_t playersGetAccountFlags(players *plrs, BYTE playerNum) {
  uint8_t flags = 0;
  if (plrs != NULL && (*plrs)->item[playerNum].inUse == TRUE) {
    flags = (*plrs)->item[playerNum].clientFlags
            & (PLAYER_FLAG_WBN_VERIFIED | PLAYER_FLAG_WBN_STEAM_LINKED);
  }
  return flags;
}

/*********************************************************
*NAME:          playersMakeMessageName
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Makes the message name for a specific player
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum  - The player number to set
* dest       - Destination string
*********************************************************/
void playersMakeMessageName(ClientSim *cs, players *plrs, BYTE selfPlayer, BYTE playerNum, char *dest) {
  char label[FILENAME_MAX];   /* Used to hold the string made by label */

  label[0] = '\0';
  if (playerNum == selfPlayer) {
    labelMakeMessage(cs, label, (*plrs)->item[playerNum].playerName, langGetText(MESSAGE_THIS_COMPUTER));
    strcpy(dest, label);
  } else if ((*plrs)->item[playerNum].inUse == FALSE) {
    strcpy(dest, NO_TANK);
  } else {
    labelMakeMessage(cs, label, (*plrs)->item[playerNum].playerName, (*plrs)->item[playerNum].location);
    strcpy(dest, label);
  }
}

/*********************************************************
*NAME:          playersMakeScreenName
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Makes the message name for a specific player
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum  - The player number to set
* dest       - Destination string
*********************************************************/
void playersMakeScreenName(ClientSim *cs, players *plrs, BYTE selfPlayer, BYTE playerNum, char *dest) {
  char label[FILENAME_MAX];   /* Used to hold the string made by label */

  label[0] = '\0';
  if ((*plrs)->item[playerNum].inUse == TRUE) {
    if (playerNum == selfPlayer) {
      labelMakeTankLabel(cs, label, (*plrs)->item[playerNum].playerName, langGetText(MESSAGE_THIS_COMPUTER), TRUE);
    } else {
      labelMakeTankLabel(cs, label, (*plrs)->item[playerNum].playerName, (*plrs)->item[playerNum].location, FALSE);
    }
    strcpy(dest, label);
  }
}


/*********************************************************
*NAME:          playersIsAllie
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns whether playerA is allied to playerB
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerA - The player number to check
* playerB  - The player number to check
*********************************************************/
bool playersIsAllie(players *plrs, BYTE playerA, BYTE playerB) {
  bool returnValue; /* Value to return */
  BYTE check;       /* Which item to check - In case one player has left */
  BYTE check2;      /* Whick item to check for - In case one player has left */

  check = check2 = NEUTRAL;
  returnValue = FALSE;

  if (playerA == playerB) {
    returnValue = TRUE;
  } else if (playerA == NEUTRAL || playerB == NEUTRAL) {
    returnValue = FALSE;
  } else {
    /* Check for exist */
    if ((*plrs)->item[playerA].inUse == TRUE) {
      check = playerA;
      check2 = playerB;
    } else if ((*plrs)->item[playerB].inUse == TRUE) {
      check = playerB;
      check2 = playerA;
    }
    
    if (check != NEUTRAL) {
      returnValue = allienceExist(&((*plrs)->item[check].allie), check2);
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          playersGetNumAllie
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns the number allies a player has. (Atleast 1 as it
* includes themselves)
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - The player number to check
*********************************************************/
BYTE playersGetNumAllie(players *plrs, BYTE playerNum) {
  BYTE returnValue; /* Value to return */

  returnValue = 0;
  if ((*plrs)->item[playerNum].inUse == TRUE) {
    returnValue = allienceNumAllies(&((*plrs)->item[playerNum].allie)) + 1;
  }
  return returnValue;
}

/*********************************************************
*NAME:          playersNameTaken
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns whether some player already is using that name
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* checkName - The player name to check
*********************************************************/
bool playersNameTaken(players *plrs, char *checkName) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping variable */

  count = 0;
  returnValue = FALSE;

  while (count<MAX_TANKS && returnValue == FALSE) {
    if ((*plrs)->item[count].inUse == TRUE) {
      if (playerNameCompare((*plrs)->item[count].playerName, checkName) == 0) {
        returnValue = TRUE;
      }

    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          playersScreenAllience
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns a player is screen allience type.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - Player number to check
*********************************************************/
tankAlliance playersScreenAllience(players *plrs, BYTE selfPlayer, BYTE playerNum) {
  tankAlliance returnValue; /* Value to return */

  returnValue = tankNone;
  if (playerNum < MAX_TANKS) {
    if ((*plrs)->item[playerNum].inUse == FALSE) {
      returnValue = tankNone;
    } else if (playerNum == selfPlayer) {
      returnValue = tankSelf;
    } else if (selfPlayer < MAX_TANKS &&
        allienceExist(&((*plrs)->item[selfPlayer].allie), playerNum) == TRUE) {
      returnValue = tankAllie;
    } else {
      returnValue = tankEvil;
    }
  }
  return returnValue;
}


/*********************************************************
*NAME:          playersMakeScreenTanks
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED:  8/1/00
*PURPOSE:
* Adds each player to the screen tanks structure
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* value    - screen tanks data structure
* leftPos  - Left bound
* rightPos - Left bound
* top      - top bound
* bottom   - Bottom bound
*********************************************************/
void playersMakeScreenTanks(ClientSim *cs, GameSim *sim, players *plrs, screenTanks *value, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom) {
  char playerName[FILENAME_MAX]; /* Holds playername/location info */
  WORLD conv;                    /* Used in conversion */
  WORLD conv2;
  WORLD ourTankX;                /* Our tank X and Y co-ordinates */
  WORLD ourTankY;
  WORLD tx;                      /* Current tanks X and Y co-ordinates */
  WORLD ty;
  BYTE frame;                    /* Holds frame info */
  BYTE count;                    /* Looping variable */
  BYTE mx;                       /* Tank map and pixel X and Y co-ordinates */
  BYTE my;
  BYTE px;
  BYTE py;

/* FIXME: This function could use some optimisation I think */
  {
    BYTE self = clientSimGetMyPlayerNum(cs);
    if (self >= MAX_TANKS || sim->tanks[self] == NULL) return;
    tankGetWorld(&sim->tanks[self], &ourTankX, &ourTankY);
  }

  for (count=0;count<MAX_TANKS;count++) {
    if ((*plrs)->item[count].inUse == TRUE && count != clientSimGetMyPlayerNum(cs)) {
      playerName[0] = EMPTY_CHAR;
      /* Extract fixed map co-ordinates */
      conv = (*plrs)->item[count].mapX;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv2 = (*plrs)->item[count].pixelX;
      conv2 <<= TANK_SHIFT_RIGHT2;
      conv += conv2;
      conv -= TANK_SUBTRACT;
      conv >>= TANK_SHIFT_MAPSIZE;
      mx = (BYTE) conv;
      conv = (*plrs)->item[count].mapY;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv2 = (*plrs)->item[count].pixelY;
      conv2 <<= TANK_SHIFT_RIGHT2;
      conv += conv2;
      conv -= TANK_SUBTRACT;
      conv >>= TANK_SHIFT_MAPSIZE;
      my = (BYTE) conv;

      if (mx >= leftPos && mx <= rightPos && my >= top && my <= bottom) {
        tx = (WORLD) (((*plrs)->item[count].mapX << TANK_SHIFT_MAPSIZE) + ((*plrs)->item[count].pixelX<< TANK_SHIFT_RIGHT2));
        if (tx > ourTankX) {
          conv = tx - ourTankX;
        } else {
          conv = ourTankX - tx;
        }
        ty = (WORLD) ((((*plrs)->item[count].mapY << TANK_SHIFT_MAPSIZE)) + (((*plrs)->item[count].pixelY<< TANK_SHIFT_RIGHT2)));
        if (ty > ourTankY) {
          conv2 = ty - ourTankY;
        } else {
          conv2 = ourTankY - ty;
        }
        if ((screenIsItemInTrees(sim, MY_TANK(cs), tx, ty) == FALSE) || (conv < MIN_TREEHIDE_DIST && conv2 < MIN_TREEHIDE_DIST)  ) {
          /* Extract fixed pixel co-ordinates */
          conv = (*plrs)->item[count].mapX;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv2 = (*plrs)->item[count].pixelX;
          conv2 <<= TANK_SHIFT_RIGHT2;
          conv += conv2;
          conv -= TANK_SUBTRACT;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          px = (BYTE) conv;

          conv = (*plrs)->item[count].mapY;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv2 = (*plrs)->item[count].pixelY;
          conv2 <<= TANK_SHIFT_RIGHT2;
          conv += conv2;
          conv -= TANK_SUBTRACT;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          py = (BYTE) conv;
          /* Extract player screen name */
          playersMakeScreenName(cs, plrs, clientSimGetMyPlayerNum(cs), count, playerName);
          frame = (*plrs)->item[count].frame;
          if ((*plrs)->item[count].onBoat == TRUE) {
            frame += TANK_BOAT_ADD;
          }
          if (allienceExist(&((*plrs)->item[count].allie), clientSimGetMyPlayerNum(cs)) == TRUE) {
            frame += TANK_GOOD_ADD;
          } else {
            frame += TANK_EVIL_ADD;
          }
          screenTanksAddItem(value,(BYTE) (mx - leftPos), (BYTE) (my - top), px, py, frame, count, playerName); 
        }
      }
    }
  }
}

/*********************************************************
*NAME:          playersMakeScreenLgm
*AUTHOR:        John Morrison
*CREATION DATE: 19/2/99
*LAST MODIFIED:  7/3/99
*PURPOSE:
* Adds each players lgm to the screen LGM structure
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* value    - screen LGM data structure
* leftPos  - Left bound
* rightPos - Left bound
* top      - top bound
* bottom   - Bottom bound
*********************************************************/
void playersMakeScreenLgm(ClientSim *cs, players *plrs, screenLgm *value, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom) {
  WORLD wx;
  WORLD wy;
  WORLD conv;                    /* Used in conversion */
  WORLD conv2;
  WORLD ourTankX;                /* Our tank X and Y co-ordinates */
  WORLD ourTankY;
  BYTE count;                    /* Looping variable */

  for (count=0;count<MAX_TANKS;count++) {
    if ((*plrs)->item[count].inUse == TRUE && count != clientSimGetMyPlayerNum(cs)) {
      if ((*plrs)->item[count].lgmMapX >= leftPos && (*plrs)->item[count].lgmMapX <= rightPos && (*plrs)->item[count].lgmMapY >= top && (*plrs)->item[count].lgmMapY <= bottom) {
        wx = (*plrs)->item[count].lgmMapX << TANK_SHIFT_MAPSIZE;
        wx += (*plrs)->item[count].lgmPixelX << TANK_SHIFT_RIGHT2;
        wy = (*plrs)->item[count].lgmMapY << TANK_SHIFT_MAPSIZE;
        wy += (*plrs)->item[count].lgmPixelY << TANK_SHIFT_RIGHT2;
        tankGetWorld(&MY_TANK(cs), &ourTankX, &ourTankY);
        if (wx > ourTankX) {
          conv = wx - ourTankX;
        } else {
          conv = ourTankX - wx;
        }
        if (wy > ourTankY) {
          conv2 = wy - ourTankY;
        } else {
          conv2 = ourTankY - wy;
        }

        if ((*plrs)->item[count].lgmFrame == LGM_HELICOPTER_FRAME || screenIsItemInTrees(clientSimGetGameSim(cs), MY_TANK(cs), wx, wy) == FALSE || (conv < MIN_TREEHIDE_DIST && conv2 < MIN_TREEHIDE_DIST)) {
          screenLgmAddItem(value,(BYTE) ((*plrs)->item[count].lgmMapX - leftPos), (BYTE) ((*plrs)->item[count].lgmMapY - top), (*plrs)->item[count].lgmPixelX, (*plrs)->item[count].lgmPixelY, (*plrs)->item[count].lgmFrame);
        }
      }
    }
  }
}

/*********************************************************
*NAME:          playersGetNumPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns the number of players in the game
*
*ARGUMENTS:
* plrs - Pointer to the players object 
*********************************************************/
BYTE playersGetNumPlayers(players *plrs) {
  BYTE returnValue; /* Value to return */
  BYTE count;       /* Looping variable */


  returnValue = 0;
  // This is some code to attempt to debug a linbolods crash with plrs = 0x0
  if (*plrs != 0x0){
	  if (*plrs != NULL) {
		for (count=0;count<MAX_TANKS;count++) {
		  if ((*plrs)->item[count].inUse == TRUE) {
			returnValue++;
		  }
		}
	  }
  } else {
	// At this point we will print a message to the console, and then allow the function to return
	// hopefully this will allow a logfile to be generated rather than a seg fault, so that we can perhaps track this error better.
    fprintf(stderr, "Players is equal to zero, something has happened that shouldn't have.\n");
  }
  return returnValue;
}


/*********************************************************
*NAME:          playersMakeNetAlliences
*AUTHOR:        John Morrison
*CREATION DATE: 25/2/99
*LAST MODIFIED: 29/2/99
*PURPOSE:
* Returns the number of alliences playerNum has. Also
* copies each into the array value
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - Player number to make for
* value     - Array to hold the alliences
*********************************************************/
BYTE playersMakeNetAlliences(players *plrs, BYTE playerNum, BYTE *value) {
  BYTE returnValue; /* Value to return */
  BYTE count; /* Looping variable */

  returnValue = allienceNumAllies(&((*plrs)->item[playerNum].allie));
  count = 1;
  while (count <= returnValue) {
    value[count-1] = allienceReturnNum(&((*plrs)->item[playerNum].allie), (BYTE) (count-1));
    count++;
  }
  return returnValue;
}

void playersRebuildSelfAlliance(GameSim *sim, players *plrs, BYTE selfPlayer) {
  BYTE count;
  BYTE total;

  if (selfPlayer >= MAX_TANKS) return;
  if ((*plrs)->item[selfPlayer].inUse == FALSE) return;

  allienceDestroy(&((*plrs)->item[selfPlayer].allie));
  (*plrs)->item[selfPlayer].allie = allienceCreate();
  for (count = 0; count < MAX_TANKS; count++) {
    if (count == selfPlayer) continue;
    if ((*plrs)->item[count].inUse == FALSE) continue;
    if (allienceExist(&((*plrs)->item[count].allie), selfPlayer) == TRUE) {
      allienceAdd(&((*plrs)->item[selfPlayer].allie), count);
    }
  }

  total = basesGetNumBases(&sim->bs);
  for (count = 1; count <= total; count++) {
    frontEndStatusBase(clientSimFromSim(sim), count, basesGetStatusNum(sim, count));
  }
  total = pillsGetNumPills(&sim->pb);
  for (count = 1; count <= total; count++) {
    frontEndStatusPillbox(clientSimFromSim(sim), count, pillsGetAllianceNum(sim, &sim->pb, count));
  }
  total = playersGetNumPlayers(&sim->plyrs);
  for (count = 1; count <= total; count++) {
    frontEndStatusTank(clientSimFromSim(sim), count, playersScreenAllience(plrs, selfPlayer, (BYTE)(count - 1)));
  }
  playersSetAllieMenu(plrs, selfPlayer, FALSE);
}

/*********************************************************
*NAME:          playersGetFirstNotUsed
*AUTHOR:        John Morrison
*CREATION DATE: 25/2/99
*LAST MODIFIED: 25/2/99
*PURPOSE:
* Returns the first not used player number.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* value - Array to hold the alliences
*********************************************************/
BYTE playersGetFirstNotUsed(players *plrs) {
  BYTE returnValue; /* Value to return */
  BYTE count; /* Looping variable */

  returnValue = 0;
  returnValue = NEUTRAL;
  count = 0;
  while (count < MAX_TANKS && returnValue == NEUTRAL) {
    if ((*plrs)->item[count].inUse == FALSE) {
      returnValue = count;
    }
    count++;
  }
  return returnValue;
}

/*********************************************************
*NAME:          playersLeaveGame
*AUTHOR:        John Morrison
*CREATION DATE: 20/3/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* A player has left the game.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - The number of the player that has left
*********************************************************/
void playersLeaveGame(ClientSim *csParam, GameSim *sim, players *plrs, BYTE selfPlayer, BYTE playerNum, bool isServer) {
  BYTE count;                /* Looping variable */


  if ((*plrs)->item[playerNum].inUse == TRUE) {
    allienceDestroy(&((*plrs)->item[playerNum].allie));

    count = 0;
    while (count < MAX_TANKS) {
      if ((*plrs)->item[playerNum].inUse == TRUE && count != playerNum) {
        allienceRemove(&((*plrs)->item[count].allie), playerNum);
      }
      count++;
    }

    {
      MessageArgs args;
      memset(&args, 0, sizeof(args));
      playersMakeMessageName(NULL, plrs, selfPlayer, playerNum, args.playerName);
      args.playerFlags = playersGetAccountFlags(plrs, playerNum);
      playersGetCountryCode(plrs, playerNum, args.playerCountry);
      (*plrs)->item[playerNum].inUse = FALSE;
      (*plrs)->item[playerNum].needUpdate = FALSE;
      (*plrs)->item[playerNum].isChecked = FALSE;
      (*plrs)->playerBrainNames[playerNum][0] = '\0';
      if (isServer == FALSE) {
        frontEndClearPlayer(csParam, (playerNumbers) playerNum);
        frontEndStatusTank(csParam, (BYTE) (playerNum + 1), tankNone);
        frontEndSetPlayerCheckState(csParam, (playerNumbers) playerNum, FALSE);
      }
      /* Make a message about it */
      sim->callbacks.messageAdd(sim->callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_QUIT_GAME, &args);
    }
  }
}

/*********************************************************
*NAME:          playersSetMenuItems
*AUTHOR:        John Morrison
*CREATION DATE: 23/3/99
*LAST MODIFIED: 23/3/99
*PURPOSE:
* Sets all the players in the players menu
*
*ARGUMENTS:
* plrs - Pointer to the players object 
*********************************************************/
void playersSetMenuItems(ClientSim *csParam, players *plrs, BYTE selfPlayer, bool isServer) {
  BYTE count;                /* Looping variable */
  char str[512]; /* The player name */

  for (count=0;count<MAX_TANKS;count++) {
    if ((*plrs)->item[count].inUse == TRUE  ) {
      strcpy(str, (*plrs)->item[count].playerName);
      if (count != selfPlayer) {
        strcat(str, "@");
        strcat(str, (*plrs)->item[count].location);
      }
      if (isServer == FALSE) {
        frontEndSetPlayer(csParam, (playerNumbers) count, str,
                          (*plrs)->item[count].location,
                          (*plrs)->item[count].ping,
                          playersGetClientType(plrs, count),
                          playersGetClientFlags(plrs, count));
      }
    }
  }
  if (isServer == FALSE) {
    frontEndEnableRequestAllyMenu(FALSE);
    frontEndEnableLeaveAllyMenu(FALSE);
  }
}

/*********************************************************
*NAME:          playersGetNumAllies
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Returns the number of allies to your player. (Includes
* self
*
*ARGUMENTS:
* plrs - Pointer to the players object 
*********************************************************/
int playersGetNumAllies(players *plrs, BYTE selfPlayer) {
  BYTE returnValue; /* Value to return */

  returnValue = allienceNumAllies(&((*plrs)->item[selfPlayer].allie)) + 1;
  return returnValue;
}


/*********************************************************
*NAME:          playersGetNumChecked
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Gets the number of players with the checked bytes set to
* TRUE
*
*ARGUMENTS:
* plrs - Pointer to the players object 
*********************************************************/
int playersGetNumChecked(players *plrs) {
  BYTE returnValue; /* Value to return */
  BYTE count; /* Looping variable */

  returnValue = 0;
  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && (*plrs)->item[count].isChecked == TRUE) {
      returnValue++;
    }
    count++;
  }
  return returnValue;
}

/*********************************************************
*NAME:          playersCheckAllies
*AUTHOR:        John Morrison
*CREATION DATE:  6/4/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
* Checks all your players allies bytes and sends frontend
* messages
*
*ARGUMENTS:
* plrs - Pointer to the players object 
*********************************************************/
void playersCheckAllies(ClientSim *csParam, players *plrs, BYTE selfPlayer, bool isServer) {
  BYTE count; /* Looping variable */

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && (allienceExist(&((*plrs)->item[selfPlayer].allie), count) == TRUE || count == selfPlayer)) {
      (*plrs)->item[count].isChecked = TRUE;
    } else {
      (*plrs)->item[count].isChecked = FALSE;
    }
    if (isServer == FALSE) {
      frontEndSetPlayerCheckState(csParam, (playerNumbers) count, (*plrs)->item[count].isChecked);
    }
    count++;
  }
  playersSetAllieMenu(plrs, selfPlayer, isServer);
}

/*********************************************************
*NAME:          playersCheckAllNone
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Checks/Unchecks all players allies bytes and sends 
* frontend messages
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* isChecked - TRUE if check all
*********************************************************/
void playersCheckAllNone(ClientSim *csParam, players *plrs, BYTE selfPlayer, bool isChecked, bool isServer) {
  BYTE count;         /* Looping variable */

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE) {
      (*plrs)->item[count].isChecked = isChecked;
      if (isServer == FALSE) {
        frontEndSetPlayerCheckState(csParam, (playerNumbers) count, isChecked);
      }
    }
    count++;
  }
  playersSetAllieMenu(plrs, selfPlayer, isServer);
}

/*********************************************************
*NAME:          playersToggleCheckedState
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Toggles the checked state of a player.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - The number of the player to check
*********************************************************/
void playersToggleCheckedState(ClientSim *csParam, players *plrs, BYTE selfPlayer, BYTE playerNum, bool isServer) {
  if (playerNum < MAX_TANKS) {
    if ((*plrs)->item[playerNum].inUse == TRUE  ) {
      if ((*plrs)->item[playerNum].isChecked == TRUE) {
        (*plrs)->item[playerNum].isChecked = FALSE;
      } else {
        (*plrs)->item[playerNum].isChecked = TRUE;
      }
      if (isServer == FALSE) {
        frontEndSetPlayerCheckState(csParam, (playerNumbers) playerNum, (*plrs)->item[playerNum].isChecked);
      }
    }
  }
  playersSetAllieMenu(plrs, selfPlayer, isServer);
}

/*********************************************************
*NAME:          playersCheckNearbyPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Checks nearby players
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* xValue - Your tanks X Map position
* yValue - Your tanks Y Map position
*********************************************************/
void playersCheckNearbyPlayers(ClientSim *csParam, players *plrs, BYTE selfPlayer, BYTE xValue, BYTE yValue, bool isServer) {
  int xDiff;  /* X and Y differences in location */
  int yDiff;
  BYTE count; /* Looping variable */

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE) {
      xDiff = xValue - (*plrs)->item[count].mapX;
      yDiff = yValue - (*plrs)->item[count].mapY;
      if (xDiff >= PLAYER_MAX_SELECT_LEFT && xDiff <= PLAYER_MAX_SELECT_RIGHT && yDiff >= PLAYER_MAX_SELECT_TOP && yDiff <= PLAYER_MAX_SELECT_BOTTOM) {
        (*plrs)->item[count].isChecked = TRUE;
      } else {
        (*plrs)->item[count].isChecked = FALSE;
      }
      if (isServer == FALSE) {
        frontEndSetPlayerCheckState(csParam, (playerNumbers) count, (*plrs)->item[count].isChecked);
      }
    }
    count++;
  }
  playersSetAllieMenu(plrs, selfPlayer, isServer);
}

/*********************************************************
*NAME:          playersNumNearbyPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Returns number of nearby players. (Includes self)
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* xValue - Your tanks X Map position
* yValue - Your tanks Y Map position
*********************************************************/
int playersNumNearbyPlayers(players *plrs, BYTE xValue, BYTE yValue) {
  int returnValue; /* Value to return */
  int xDiff;       /* X and Y differences in location */
  int yDiff;
  BYTE count;      /* Looping variable */

  count = 0;
  returnValue = 1;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE) {
      xDiff = xValue - (*plrs)->item[count].mapX;
      yDiff = yValue - (*plrs)->item[count].mapY;
      if (xDiff >= PLAYER_MAX_SELECT_LEFT && xDiff <= PLAYER_MAX_SELECT_RIGHT && yDiff >= PLAYER_MAX_SELECT_TOP && yDiff <= PLAYER_MAX_SELECT_BOTTOM) {
        returnValue++;
      }
    }
    count++;
  }
  
  return returnValue;
}

/*********************************************************
*NAME:          playersSendMessageAllAllies
*AUTHOR:        John Morrison
*CREATION DATE: 7/4/99
*LAST MODIFIED: 7/4/99
*PURPOSE:
* Sends message to all allies
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* message - Message to send
*********************************************************/
void playersSendMessageAllAllies(ClientSim *cs, players *plrs, BYTE selfPlayer, char *messageStr) {
  BYTE count; /* Looping variable */

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && allienceExist(&((*plrs)->item[selfPlayer].allie), count) == TRUE) {
      clientSimMessageSendPlayer(cs, selfPlayer, count, messageStr);
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersSendMessageAllSelected
*AUTHOR:        John Morrison
*CREATION DATE: 7/4/99
*LAST MODIFIED: 7/4/99
*PURPOSE:
* Sends message to all selected players
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* message - Message to send
*********************************************************/
void playersSendMessageAllSelected(ClientSim *cs, GameSim *sim, players *plrs, BYTE selfPlayer, char *messageStr) {
  char topLine[FILENAME_MAX]; /* The message topline */
  BYTE count;                 /* Looping variable */
  (void)sim;

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && (*plrs)->item[count].isChecked == TRUE) {
      if (selfPlayer == count) {
        /* Self-echo of an outgoing chat — both top (player label) and
         * body (free-form chat) are dynamic strings, so they bypass the
         * langid-based messageAdd callback and go straight into the
         * client message log. */
        topLine[0] = '\0';
        playersMakeMessageName(cs, plrs, selfPlayer, selfPlayer, topLine);
        clientMessageAdd(clientSimGetMessages(cs), (messageType) selfPlayer, topLine, messageStr);
      } else {
        clientSimMessageSendPlayer(cs, selfPlayer, count, messageStr);
      }

    }
    count++;
  }
}

/*********************************************************
*NAME:          playersSendMessageAllNearby
*AUTHOR:        John Morrison
*CREATION DATE: 7/4/99
*LAST MODIFIED: 7/4/99
*PURPOSE:
* Sends message to all nearby players
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* xValue - Your tanks X Map position
* yValue - Your tanks Y Map position
* message - Message to send
*********************************************************/
void playersSendMessageAllNearby(ClientSim *cs, players *plrs, BYTE selfPlayer, BYTE xValue, BYTE yValue, char *messageStr) {
  int xDiff;       /* X and Y differences in location */
  int yDiff;
  BYTE count;      /* Looping variable */

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE) {
      xDiff = xValue - (*plrs)->item[count].mapX;
      yDiff = yValue - (*plrs)->item[count].mapY;
      if (xDiff >= PLAYER_MAX_SELECT_LEFT && xDiff <= PLAYER_MAX_SELECT_RIGHT && yDiff >= PLAYER_MAX_SELECT_TOP && yDiff <= PLAYER_MAX_SELECT_BOTTOM) {
        clientSimMessageSendPlayer(cs, selfPlayer, count, messageStr);
      }
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersIsInUse
*AUTHOR:        John Morrison
*CREATION DATE: 31/8/99
*LAST MODIFIED: 31/8/99
*PURPOSE:
* Returns whether a player Number is in use
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - The player num to check
*********************************************************/
bool playersIsInUse(players *plrs, BYTE playerNumber) {
  return (*plrs)->item[playerNumber].inUse;
}

/*********************************************************
*NAME:          playersGetLgmDetails
*AUTHOR:        John Morrison
*CREATION DATE: 31/8/99
*LAST MODIFIED: 31/8/99
*PURPOSE:
* Gets the LGM details for a player
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - The player num to check
* mx        - LGM Map X Position
* my        - LGM Map Y Position
* px        - LGM Pixel X Position
* py        - LGM Pixel Y Position
* frame     - LGM Frame
*********************************************************/
void playersGetLgmDetails(players *plrs, BYTE playerNumber, BYTE *mx, BYTE *my , BYTE *px, BYTE *py, BYTE *frame){
  if ((*plrs)->item[playerNumber].inUse == TRUE) {
    *mx = (*plrs)->item[playerNumber].lgmMapX;
    *my = (*plrs)->item[playerNumber].lgmMapY;
    *px = (*plrs)->item[playerNumber].lgmPixelX;
    *py = (*plrs)->item[playerNumber].lgmPixelY;
    *frame = (*plrs)->item[playerNumber].lgmFrame;
  } else {
    *mx = 0;
    *my = 0;
    *px = 0;
    *py = 0;
    *frame = 0;
  }
}

/*********************************************************
*NAME:          playersCheckCollision
*AUTHOR:        John Morrison
*CREATION DATE: 31/10/99
*LAST MODIFIED:  4/11/99
*PURPOSE:
* Checks for a collision between our tank (given as 
* variables & the tanks in the players structure)
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - The player num to check
* xValue  - Tank X World co-ordinate
* yValue  - Tank Y World co-ordinate
* leftPos - If left < 0 then hit from left > 0 from right
* downPos - If up < 0 then hit from above > 0 from below
*********************************************************/
bool playersCheckCollision(players *plrs, BYTE playerNum, WORLD xValue, WORLD yValue, int *leftPos, int *downPos) {
  bool returnValue; /* Value to return */
  int count;       /* Looping Variable */
  WORLD conv;   /* Used for conversions */
  WORLD mx;     /* Tank map offsets */
  WORLD my;
  WORLD testX;
  WORLD testY;

  count = 0;
  *leftPos = 0;
  *downPos = 0;
  returnValue = FALSE;

  while (returnValue == FALSE && count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && count != playerNum) {
      /* Test for collision */
      conv = (*plrs)->item[count].mapX;
      conv <<= TANK_SHIFT_MAPSIZE;
      mx = conv;
      conv = (*plrs)->item[count].pixelX;
      conv <<= TANK_SHIFT_RIGHT2;
      mx += conv;
 
      conv = (*plrs)->item[count].mapY;
      conv <<= TANK_SHIFT_MAPSIZE;
      my = conv;
      conv = (*plrs)->item[count].pixelY;
      conv <<= TANK_SHIFT_RIGHT2;
      my += conv;
 
      if (mx > xValue) {
        testX = mx - xValue;
        *leftPos = -1;
      } else {
        testX = xValue - mx;
        *leftPos = 1;
      }
      if (testX < 128) {
        *leftPos = 0;
      }


      if (my > yValue) {
        testY = my - yValue;
        *downPos = 1;
      } else {
        testY = yValue - my;
        *downPos = -1;
      }
      if (testY < 128) {
        *downPos = 0;
      }

      if (testX < (256) && testY < (256)) {
        returnValue = TRUE;
      }    
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          playersCalcTankCollision
*PURPOSE:
* Calculates combined tank collision effects from all
* nearby tanks. Reads positions directly from the tanks[]
* array so this works on both server and client. Adjusts
* velocity to slide along tanks and computes separation
* push for overlapping tanks.
*
*ARGUMENTS:
* sim       - Pointer to the GameSim
* playerNum - The player num to check
* xValue    - Tank X World co-ordinate
* yValue    - Tank Y World co-ordinate
* xVel      - In/Out: X velocity component
* yVel      - In/Out: Y velocity component
* pushX     - Output: X separation push
* pushY     - Output: Y separation push
*********************************************************/
bool playersCalcTankCollision(GameSim *sim, BYTE playerNum, WORLD xValue, WORLD yValue, int *xVel, int *yVel, int *pushX, int *pushY) {
  int count;
  WORLD mx, my;
  bool found = FALSE;
  float vx = (float)*xVel;
  float vy = (float)*yVel;

  *pushX = 0;
  *pushY = 0;

  for (count = 0; count < MAX_TANKS; count++) {
    if (count != playerNum && sim->tanks[count] != NULL && tankGetArmour(&sim->tanks[count]) <= TANK_FULL_ARMOUR) {
      tankGetWorld(&sim->tanks[count], &mx, &my);

      float dx = (float)((int)xValue - (int)mx);
      float dy = (float)((int)yValue - (int)my);
      float distSq = dx * dx + dy * dy;

      if (distSq < 256.0f * 256.0f) {
        float dist = sqrtf(distSq);
        float nx, ny;
        if (dist > 0.0f) {
          nx = dx / dist;
          ny = dy / dist;
        } else {
          /* Tanks at exact same position: push apart using player index as tiebreaker */
          nx = (count > playerNum) ? 1.0f : -1.0f;
          ny = 0.0f;
        }
        found = TRUE;

        /* Remove velocity component moving toward this tank */
        float velDotNormal = vx * nx + vy * ny;
        if (velDotNormal < 0.0f) {
          vx -= velDotNormal * nx;
          vy -= velDotNormal * ny;
        }

        /* Separation push if overlapping */
        if (dist < 256.0f) {
          float push = (256.0f - dist) * 0.5f;
          if (push < 1.0f) push = 1.0f;
          *pushX += (int)(nx * push);
          *pushY += (int)(ny * push);
        }
      }
    }
  }

  if (found) {
    *xVel = (int)vx;
    *yVel = (int)vy;
  }

  return found;
}

/*********************************************************
*NAME:          playersSetAllieMenu
*AUTHOR:        John Morrison
*CREATION DATE: 31/10/99
*LAST MODIFIED:  1/11/99
*PURPOSE:
* Determines whether the request and leave alliance menu
* items should be checked or not and passes it onto the
* frontend
*
*ARGUMENTS:
* plrs - Pointer to the players object 
*********************************************************/
void playersSetAllieMenu(players *plrs, BYTE selfPlayer, bool isServer) {
  bool req;   /* Enable the request menu */
  bool leave; /* Enable the leave menu   */
  BYTE count; /* Looping variable */

  count = 0;
  req = FALSE;
  leave = FALSE;
  while (count < MAX_TANKS && leave == FALSE) {
    if ((*plrs)->item[count].inUse == TRUE && count != selfPlayer) {
      if (allienceExist(&((*plrs)->item[selfPlayer].allie), count) == TRUE) {
        leave = TRUE;
        req = FALSE;
      } else if ((*plrs)->item[count].isChecked == TRUE){
        req = TRUE;
      }
    }
    count++;
  }
  if (isServer == FALSE) {
    frontEndEnableRequestAllyMenu(req);
    frontEndEnableLeaveAllyMenu(leave);
  }
}

/*********************************************************
*NAME:          playersRequestAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
* Process a request alliance request.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
*********************************************************/
void playersRequestAlliance(ClientSim *cs, players *plrs, BYTE selfPlayer) {
  BYTE count; /* Looping variable */

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && count != selfPlayer && (*plrs)->item[count].isChecked == TRUE) {
      if (allienceExist(&((*plrs)->item[selfPlayer].allie), count) == FALSE) {
        /* Place request */
        clientSimRequestAlliance(cs, selfPlayer, count);
      }
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersLeaveAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
* Process a leave alliance request.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - Player number that is leaving the alliance
*********************************************************/
void playersLeaveAlliance(GameSim *sim, players *plrs, BYTE selfPlayer, BYTE playerNum, bool isServer) {
  BYTE count; /* Looping variable */
  BYTE total; /* Amount of items to redraw */
  bool found;
  count = 0;
  found = FALSE;
  while (count < MAX_TANKS && found == FALSE) {
    if (playerNum != count) {
      if (playersIsAllie(plrs, count, playerNum) == TRUE) {
        found = TRUE;
	  }
	}
    count++;
  }
  count--;

  basesMigrate(sim, playerNum, count);
  pillsMigratePlanted(sim, playerNum, count);

  allienceDestroy(&((*plrs)->item[playerNum].allie));
  (*plrs)->item[playerNum].allie = allienceCreate();
  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && count != playerNum) {
       allienceRemove(&((*plrs)->item[count].allie), playerNum);
    }
    count++;
  }
 
  
  /* Update the screen */
  if (isServer == FALSE) {
    GameSim *gsim = sim;
    total = basesGetNumBases(&gsim->bs);
    for (count=1;count<=total;count++) {
      frontEndStatusBase(clientSimFromSim(gsim), count, basesGetStatusNum(gsim, count));
    }
    total = pillsGetNumPills(&gsim->pb);
    for (count=1;count<=total;count++) {
      frontEndStatusPillbox(clientSimFromSim(gsim), count, pillsGetAllianceNum(gsim, &gsim->pb, count));
    }
    total = playersGetNumPlayers(&gsim->plyrs);
    for (count=1;count<=total;count++) {
      frontEndStatusTank(clientSimFromSim(gsim), count, playersScreenAllience(plrs, selfPlayer, (BYTE) (count-1)));
    }
    playersSetAllieMenu(plrs, selfPlayer, isServer);
  }
}

/*********************************************************
*NAME:          playersAcceptAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* A player has been accepted into an alliance
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* acceptedBy - Who accepted them in
* newMember  - Who the new member is
*********************************************************/
void playersAcceptAlliance(GameSim *sim, players *plrs, BYTE selfPlayer, BYTE acceptedBy, BYTE newMember, bool isServer) {
  BYTE count;   /* Looping variable */
  BYTE count2;   /* Looping variable */
  BYTE total;   /* Number of alliances acceptedBy has */
  PlayerBitMap allyA; /* Alliances for a and b */
  PlayerBitMap allyB;
  PlayerBitMap test;
  PlayerBitMap test2;


  allyA = playersGetAlliesBitMap(plrs, acceptedBy);
  allyB = playersGetAlliesBitMap(plrs, newMember);

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE) {
      test = (allyA >>count);
      test &= 1;
      if (test) {
        count2 = 0;
        while (count2 < MAX_TANKS) {
          test2 = (allyB >>count2);
          test2 &= 1;
          if (test2) {
            allienceAdd(&((*plrs)->item[count].allie), count2);
          }
          count2++;
        }
      }
      test = (allyB >>count);
      test &= 1;
      if (test) {
        count2 = 0;
        while (count2 < MAX_TANKS) {
          test2 = (allyA >>count2);
          test2 &= 1;
          if (test2) {
            allienceAdd(&((*plrs)->item[count].allie), count2);
          }
          count2++;
        }
      }
    }
    count++;
  }
      
      
 
  /* Update the screen */
  if (isServer == FALSE) {
    GameSim *gsim = sim;
    total = basesGetNumBases(&gsim->bs);
    for (count=1;count<=total;count++) {
      frontEndStatusBase(clientSimFromSim(gsim), count, basesGetStatusNum(gsim, count));
    }
    total = pillsGetNumPills(&gsim->pb);
    for (count=1;count<=total;count++) {
      frontEndStatusPillbox(clientSimFromSim(gsim), count, pillsGetAllianceNum(gsim, &gsim->pb, count));
    }
    total = playersGetNumPlayers(&gsim->plyrs);
    for (count=1;count<=total;count++) {
      frontEndStatusTank(clientSimFromSim(gsim), count, playersScreenAllience(plrs, selfPlayer, (BYTE) (count-1)));
    }
    playersSetAllieMenu(plrs, selfPlayer, isServer);
  }
}


/*********************************************************
*NAME:          playersConnectionLost
*AUTHOR:        John Morrison
*CREATION DATE: 2/11/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* Called if your connection is lost to a network game.
* Drops all other players except yourself.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - The number of the player that has left
*********************************************************/
void playersConnectionLost(ClientSim *csParam, GameSim *sim, players *plrs, BYTE selfPlayer) {
  BYTE count; /* Looping variable */
  BYTE total;   /* Number of alliances acceptedBy has */
  BYTE current; /* Current Allie we are working on  */


  /* Move allies stuff to us */
  count = 0;
  total = allienceNumAllies(&((*plrs)->item[selfPlayer].allie));
  while (count < total) {
    current = allienceReturnNum(&((*plrs)->item[selfPlayer].allie), count);
    basesMigrate(sim, current, selfPlayer);
    pillsMigrate(sim, current, selfPlayer);
    count++;
  }

  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && count != selfPlayer) {
      playersLeaveGame(csParam, sim, plrs, selfPlayer, count, FALSE);
    }
    count++;
  }
}


/*********************************************************
*NAME:          playersGetBrainTanksInRect
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 9/1/00
*PURPOSE:
*  Makes the brain tank info for each tank inside the
*  rectangle formed by the function parameters.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* leftPos   - Left position of rectangle
* rightPos  - Right position of rectangle
* top    - Top position of rectangle
* bottom - Bottom position of rectangle
* tankX  - Our tanks X position
* tankY  - Our tanks Y position
*********************************************************/
void playersGetBrainTanksInRect(ClientSim *cs, players *plrs, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom, WORLD tankX, WORLD tankY) {
  BYTE count;      /* Looping variable */
  WORLD conv;      /* Used in converting items world co-ordinates */
  WORLD wx;        /* Items X and Y positions */
  WORLD wy;
  WORLD diffX;     /* Tanks differences in position */
  WORLD diffY;
  BYTE owner;      /* Owner of the tank */

  count = 0;

/* typedef struct
	{
	OBJECT object; = 0
	WORLD_X x;
	WORLD_Y y;
	WORD idnum;
	BYTE direction;
	BYTE info;
	} ObjectInfo;
*/

  while (count<MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && count != clientSimGetMyPlayerNum(cs)) {
      /* X Position */
      wx = (*plrs)->item[count].mapX;
      wx <<= TANK_SHIFT_MAPSIZE;
      conv = (*plrs)->item[count].pixelX;
      conv <<= TANK_SHIFT_RIGHT2;
      wx += conv;
      /* Y Position */
      wy = (*plrs)->item[count].mapY;
      wy <<= TANK_SHIFT_MAPSIZE;
      conv = (*plrs)->item[count].pixelY;
      conv <<= TANK_SHIFT_RIGHT2;
      wy += conv;
      /* Difference for tree check */
      if (wx > tankX) {
        diffX = wx - tankX;
      } else {
        diffX = tankX - wx;
      }
      if (wy > tankY) {
        diffY = wy - tankY;
      } else {
        diffY = tankY - wy;
      }


      if ((*plrs)->item[count].mapX >= leftPos && (*plrs)->item[count].mapX <= rightPos && (*plrs)->item[count].mapY >= top && (*plrs)->item[count].mapY <= bottom && (screenIsItemInTrees(clientSimGetGameSim(cs), MY_TANK(cs), wx, wy) == FALSE || (diffX < MIN_TREEHIDE_DIST && diffY < MIN_TREEHIDE_DIST))) {
        /* In the rectangle */
        /* wx and wy already set */
        /* Info */
        if (allienceExist(&((*plrs)->item[count].allie), clientSimGetMyPlayerNum(cs)) == TRUE) {
          owner = PLAYERS_BRAIN_FRIENDLY;
        } else {
          owner = PLAYERS_BRAIN_HOSTILE;
        }
        screenAddBrainObject(cs, PLAYERS_BRAIN_OBJECT_TYPE_TANK, wx, wy, count, (*plrs)->item[count].frame, owner, (*plrs)->item[count].speed);
      }
    }
    count++;
  }
}


/*********************************************************
*NAME:          playersGetBrainLgmsInRect
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 09/08/04
*PURPOSE:
*  Makes the brain lgm info for each lgm inside the
*  rectangle formed by the function parameters.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* left   - Left position of rectangle
* rightPos  - Right position of rectangle
* topPos    - Top position of rectangle
* bottom - Bottom position of rectangle
*********************************************************/
void playersGetBrainLgmsInRect(ClientSim *cs, players *plrs, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom) {
  BYTE count;      /* Looping variable */
  WORLD conv;      /* Used in converting items world co-ordinates */
  WORLD wx;        /* Items world positions */
  WORLD wy;
  WORLD conv2;
  WORLD ourTankX;
  WORLD ourTankY;
  BYTE lgmType;    /* Type of lgm this is */
  BYTE owner;      /* Owner of the lgm    */

  count = 0;

/* typedef struct
	{
	OBJECT object; = 0
	WORLD_X x;
	WORLD_Y y;
	WORD idnum;
	BYTE direction;
	BYTE info;
	} ObjectInfo;
*/

  while (count<MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && count != clientSimGetMyPlayerNum(cs)) {
      if ((*plrs)->item[count].lgmMapX >= leftPos && (*plrs)->item[count].lgmMapX <= rightPos && (*plrs)->item[count].lgmMapY >= top && (*plrs)->item[count].lgmMapY <= bottom) {
        /* In trees check */
        wx = (*plrs)->item[count].lgmMapX << TANK_SHIFT_MAPSIZE;
        wx += (*plrs)->item[count].lgmPixelX << TANK_SHIFT_RIGHT2;
        wy = (*plrs)->item[count].lgmMapY << TANK_SHIFT_MAPSIZE;
        wy += (*plrs)->item[count].lgmPixelY << TANK_SHIFT_RIGHT2;
        tankGetWorld(&MY_TANK(cs), &ourTankX, &ourTankY);
        if (wx > ourTankX) {
          conv = wx - ourTankX;
        } else {
          conv = ourTankX - wx;
        }
        if (wy > ourTankY) {
          conv2 = wy - ourTankY;
        } else {
          conv2 = ourTankY - wy;
        }
        
        if ((*plrs)->item[count].lgmFrame == LGM_HELICOPTER_FRAME || (screenIsItemInTrees(clientSimGetGameSim(cs), MY_TANK(cs), wx, wy) == FALSE || (conv < MIN_TREEHIDE_DIST && conv2 < MIN_TREEHIDE_DIST))) {
          /* In the rectangle */
          /* Object Type */
          if ((*plrs)->item[count].lgmFrame == LGM_HELICOPTER_FRAME) {
            lgmType = PLAYERS_BRAIN_OBJECT_TYPE_PARA;
          } else {
            lgmType = PLAYERS_BRAIN_OBJECT_TYPE_LGM;
          }
          /* X Position */
          wx = (*plrs)->item[count].lgmMapX;
          wx <<= TANK_SHIFT_MAPSIZE;
          conv = (*plrs)->item[count].lgmPixelX;
          conv <<= TANK_SHIFT_RIGHT2;
          wx += conv;
          /* Y Position */
          wy = (*plrs)->item[count].lgmMapY;
          wy <<= TANK_SHIFT_MAPSIZE;
          conv = (*plrs)->item[count].lgmPixelY;
          conv <<= TANK_SHIFT_RIGHT2;
          wy += conv;
          /* Info */
          if (allienceExist(&((*plrs)->item[count].allie), clientSimGetMyPlayerNum(cs)) == TRUE) {
            owner = PLAYERS_BRAIN_FRIENDLY;
          } else {
            owner = PLAYERS_BRAIN_HOSTILE;
          }
          screenAddBrainObject(cs, lgmType, wx, wy, count, 0, owner, 0);
        }
      }
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersGetBrainsNamesArray
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
*  Gets the players brain name array.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
*********************************************************/
u_char36 **playersGetBrainsNamesArray(players *plrs) {
  return (u_char36 **) &((*plrs)->playerBrainNames);
}

/*********************************************************
*NAME:          playersGetAlliesBitMap
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED:   4/7/00
*PURPOSE:
*  Returns the playerBitMap of all players that are allied
*  to us.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - Player number to get for
*********************************************************/
PlayerBitMap playersGetAlliesBitMap(players *plrs, BYTE playerNum) {
  PlayerBitMap returnValue; /* Value to return */
  BYTE count;                /* Looping variable */

  returnValue = 0;
  count = 0;

  while (count<MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE) {
      if (count == playerNum || allienceExist(&((*plrs)->item[count].allie), playerNum) == TRUE) {
        returnValue |= 1 << count;
      }
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          playersSendAiMessage
*AUTHOR:        John Morrison
*CREATION DATE: 12/12/99
*LAST MODIFIED: 12/12/99
*PURPOSE:
*  Called when a brain wishes to send a message to players
*  in the game.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* bitMap     - The checked and unchecked player bitMap
* messageStr - The message to be sent.
*********************************************************/
void playersSendAiMessage(ClientSim *cs, GameSim *sim, players *plrs, PlayerBitMap bitMap, char *messageStr) {
  char topLine[FILENAME_MAX]; /* The message topline */
  BYTE count; /* Looping variable */
  PlayerBitMap test;
  (void)sim;

  count = 0;
  while (count < MAX_TANKS) {
    test = (bitMap >>count);
    test &= 1;
    if ((*plrs)->item[count].inUse == TRUE && test) {
      if (count == clientSimGetMyPlayerNum(cs)) {
        /* Self-echo of an AI-generated chat — same dynamic-text path as
         * playersSendMessageAllSelected; bypass the langid callback. */
        BYTE myPN = clientSimGetMyPlayerNum(cs);
        topLine[0] = '\0';
        playersMakeMessageName(cs, plrs, myPN, myPN, topLine);
        clientMessageAdd(clientSimGetMessages(cs), (messageType) myPN, topLine, messageStr);
      } else {
        clientSimMessageSendPlayer(cs, clientSimGetMyPlayerNum(cs), count, messageStr);
      }
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersPrepareLogSnapshotForPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 25/07/04
*LAST MODIFIED: 25/07/04
*PURPOSE:
* Prepares a single player log snapshot. Returns if the
* player spot is in use
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - Player to get snapshot for
* buff      - Destination buffer
* len       - Length of the buffer
*********************************************************/
bool playersPrepareLogSnapshotForPlayer(GameSim *sim, players *value, BYTE playerNum, BYTE *buff, BYTE *len) {
  bool returnValue;
  tank *tnk;
  lgm *lgm;

  returnValue = (*value)->item[playerNum].inUse;
  buff[0] = playerNum;
  buff[1] = returnValue;
  *len = 2;
  tnk = &sim->tanks[playerNum];
  lgm = &sim->lgmen[playerNum];
  if (returnValue == TRUE && sim->tanks[playerNum] != NULL && sim->lgmen[playerNum] != NULL) {
    /* Set MX/MY/PX/PY/frame/onBoat/lgmMX/lgmMY/lgmPX/lgmPX/frame/Name/Location */
    buff[2] = tankGetMX(tnk);
    buff[3] = tankGetMY(tnk);
    buff[4] = utilPutNibble(tankGetPX(tnk), tankGetPY(tnk));
    buff[5] = tankGetFrame(tnk);
    buff[6] = tankIsOnBoat(tnk);
    buff[7] = lgmGetMX(lgm);
    buff[8] = lgmGetMY(lgm);
    buff[9] = utilPutNibble(lgmGetPX(lgm), lgmGetPY(lgm));
    buff[10] = lgmGetFrame(lgm);
    *len = 11;
    utilCtoPString((*value)->item[playerNum].playerName, (char *)(buff+11));
    *len += buff[11]+1; /* Add 1 for len prefix */
    utilCtoPString((*value)->item[playerNum].location, (char *)(buff+*len));
    *len += *(buff+*len) + 1;
    *len += allianceMakeLogAlliance(&((*value)->item[playerNum].allie), buff+*len);
  }
  return returnValue;
}

void playersCheckUpdate(players *plrs, BYTE playerNum) {
  if ((*plrs)->item[playerNum].inUse == TRUE) {
    (*plrs)->item[playerNum].mapX = 0;
    (*plrs)->item[playerNum].mapY = 0;
    (*plrs)->item[playerNum].needUpdate = TRUE;
  }
}

bool playersNeedUpdate(players *plrs, BYTE playerNum) {
  if ((*plrs)->item[playerNum].inUse == TRUE && (*plrs)->item[playerNum].needUpdate == TRUE) {
    (*plrs)->item[playerNum].needUpdate = FALSE;
    return TRUE;
  }
  return FALSE;
}

void playerNeedUpdateDone(players *plrs) {
  BYTE count;
  count = 0;
  while (count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE) {
      (*plrs)->item[count].needUpdate = FALSE;
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersCheckSameSquare
*AUTHOR:        Minhiriath
*CREATION DATE: 13/3/2009
*LAST MODIFIED: 13/3/2009
*PURPOSE:
* Checks to see if our tank is in the same square as any other tank.
*
*ARGUMENTS:
* plrs - Pointer to the players object 
* playerNum - The player num to check
* xValue  - Tank X Map co-ordinate
* yValue  - Tank Y Map co-ordinate
*********************************************************/
bool playersCheckSameSquare(players *plrs, BYTE playerNum, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */
  WORLD conv;   /* Used for conversions */
  WORLD mx;     /* Tank map offsets */
  WORLD my;

  count = 0;
  returnValue = FALSE;

  while (returnValue == FALSE && count < MAX_TANKS) {
    if ((*plrs)->item[count].inUse == TRUE && count != playerNum) {
      /* Test for collision */
      conv = (*plrs)->item[count].mapX;
      conv <<= TANK_SHIFT_MAPSIZE;
      mx = conv;
	  
      conv = (*plrs)->item[count].mapY;
      conv <<= TANK_SHIFT_MAPSIZE;
      my = conv;

	  if(xValue == mx && yValue == my) {
		  returnValue = TRUE;
	  }
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          playersSetMyLastPlayerName
*AUTHOR:        Chris Lesnieski
*CREATION DATE: 14/02/09
*LAST MODIFIED: 14/02/09
*PURPOSE:
* Will copy a string to the myLastPlayerName variable.
* This should be done on initial start-up of WinBolo when
* it loads the winbolo.ini file.
*
*ARGUMENTS:
* dest       - Name to be set as player's previous name 
*********************************************************/
void playersSetMyLastPlayerName(ClientSim *cs, char *dest)
{
  clientSimSetMyLastPlayerName(cs, dest);
}

void playersSetPing(players *plrs, BYTE playerNum, uint16_t ping) {
  if (playerNum < MAX_TANKS && (*plrs) != NULL) {
    (*plrs)->item[playerNum].ping = ping;
  }
}

uint16_t playersGetPing(players *plrs, BYTE playerNum) {
  if (playerNum < MAX_TANKS && (*plrs) != NULL) {
    return (*plrs)->item[playerNum].ping;
  }
  return 0;
}

void playersSetClientFlags(players *plrs, BYTE playerNum, uint8_t flags) {
  if (playerNum < MAX_TANKS && (*plrs) != NULL) {
    (*plrs)->item[playerNum].clientFlags = flags;
  }
}

uint8_t playersGetClientFlags(players *plrs, BYTE playerNum) {
  if (playerNum < MAX_TANKS && (*plrs) != NULL) {
    return (*plrs)->item[playerNum].clientFlags;
  }
  return 0;
}

void playersSetClientType(players *plrs, BYTE playerNum, uint8_t clientType) {
  if (playerNum < MAX_TANKS && (*plrs) != NULL) {
    (*plrs)->item[playerNum].clientType = clientType;
  }
}

uint8_t playersGetClientType(players *plrs, BYTE playerNum) {
  if (playerNum < MAX_TANKS && (*plrs) != NULL) {
    return (*plrs)->item[playerNum].clientType;
  }
  return CLIENT_TYPE_UNKNOWN;
}
