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
*Name:          PlayersRejoin
*Filename:      playersrejoin.c
*Author:        John Morrison
*Creation Date: 22/6/00
*Last Modified: 22/6/00
*Purpose:
*  Looks after players rejoin and ownerships.
*********************************************************/

#include <string.h>
#include "global.h"
#include "pillbox.h"
#include "bases.h"
#include "game_sim.h"
#include "playersrejoin.h"
#include "screen.h"
#include "log.h"

playersRejoin rejoin;

/*********************************************************
*NAME:          playersRejoinCreate
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* Sets up the players rejoin structure.
*
*ARGUMENTS:
*
*********************************************************/
void playersRejoinCreate() {
  BYTE count; /* Looping variable */
 
  count = 0;
  while (count < MAX_TANKS) {
    rejoin.item[count].inUse = FALSE;
    count++;
  }
}

/*********************************************************
*NAME:          playersRejoinDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* Destroys the players rejoin structure
*
*ARGUMENTS:
*
*********************************************************/
void playersRejoinDestroy() {
  BYTE count; /* Looping variable */
 
  count = 0;
  while (count < MAX_TANKS) {
    rejoin.item[count].inUse = FALSE;
    count++;
  }
}

/*********************************************************
*NAME:          playersRejoinUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* Updates the players rejoin structure. Timeouts etc.
*
*ARGUMENTS:
*
*********************************************************/
void playersRejoinUpdate() {
  BYTE count; /* Looping variable */
 
  count = 0;
  while (count < MAX_TANKS) {
    if (rejoin.item[count].inUse == TRUE) {
      rejoin.item[count].timeOut++;
      if (rejoin.item[count].timeOut == MAX_REJOIN_TIME) {
        rejoin.item[count].inUse = FALSE;
      }
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersAddPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* Adds a player to the rejoin structure
*
*ARGUMENTS:
*
*********************************************************/
void playersRejoinAddPlayer(char *playerName, PlayerBitMap pills, PlayerBitMap bases) {
  BYTE count;    /* Looping variable */
  BYTE best;     /* Best position to add */
  int timeBest;  /* Best time to beat - If we can't find a free spot */
  bool done;     /* Finished looping */
 
  count = 0;
  done = FALSE;
  best = 0;
  timeBest = MAX_REJOIN_TIME;
  while (count < MAX_TANKS && done == FALSE) {
    if (rejoin.item[count].inUse == FALSE) {
      done = TRUE;
      best = count;
    } else if (rejoin.item[count].timeOut < timeBest) {
      best = count;
      timeBest = rejoin.item[count].timeOut;
    }
    count++;
  }

  /* Add it to best slot */
  rejoin.item[best].inUse  = TRUE;
  strcpy(rejoin.item[best].playerName, playerName);
  rejoin.item[best].pills = pills;
  rejoin.item[best].bases = bases;
  rejoin.item[best].timeOut = 0;
}

/*********************************************************
*NAME:          playersRejoinRequest
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* A player wish to rejoin. See if they exists and assign
* items to his ownership if they aren't owned by someone
* else.
*
*ARGUMENTS:
*  playerName - The player name requesting the rejoin
*  playerNum  - The rejoining players player number
*  pb         - Pointer to the pillboxes structure
*  bs         - Pointer to the bases structure
*********************************************************/
void playersRejoinRequest(GameSim *sim, char *playerName, BYTE playerNum, pillboxes *pb) {
  BYTE count;    /* Looping variable */
  BYTE num = 255;      /* Number player is at */
  unsigned long testItem; /* Pill base item we are testing */
  base ba;
 
  count = 0;
  while (count < MAX_TANKS) {
    if (strcmp(rejoin.item[count].playerName, playerName) == 0) {
      /* Found them */
      num = count;
      break;
    }
    count++;
  }

  if (num < 255) {
    /* They did exist */
    count = 0;
    logAddEvent(log_PlayerRejoin, playerNum, 0, 0, 0, 0, NULL);
    while (count < MAX_TANKS) {
      /* Pillbox */
      testItem = (rejoin.item[num].pills >>count);
      testItem &= 1;
      if (testItem) {
        if (pillsGetPillOwner(pb, (BYTE) (count+1)) == NEUTRAL) {
          pillsSetPillOwner(sim, pb, (BYTE) (count+1), playerNum, TRUE);
        }
      }
      /* Base */
      testItem = (rejoin.item[num].bases >>count);
      testItem &= 1;
      if (testItem) {
        if (basesGetBaseOwner(&sim->bs, (BYTE) (count+1)) == NEUTRAL) {
          basesGetBase(&sim->bs, &ba, (BYTE) (count+1));
          basesSetBaseOwner(sim, (BYTE) (count+1), playerNum, TRUE);
        }        
      }
      count++;
    }
    rejoin.item[num].inUse = FALSE;
  }
}

