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

/* The record in use under playerName, or -1 when there is none. A player
   who was signed in to WinBolo.net and a player who was not can each have
   a record under one name. They are two players, so the two records are
   kept apart, and the signed-in one is the one found: a seat gets it only
   once WinBolo.net has confirmed that name. */
static int rejoinFindRecord(const char *playerName) {
  int count;      /* Looping variable */
  int found = -1; /* A record of a player who was not signed in */

  if (playerName == NULL) {
    return -1;
  }
  for (count = 0; count < MAX_TANKS; count++) {
    if (rejoin.item[count].inUse == TRUE && strcmp(rejoin.item[count].playerName, playerName) == 0) {
      if (rejoin.item[count].signedIn) {
        return count;
      }
      if (found < 0) {
        found = count;
      }
    }
  }
  return found;
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
void playersRejoinAddPlayer(char *playerName, PlayerBitMap pills, PlayerBitMap bases, BYTE team, bool signedIn) {
  BYTE count;    /* Looping variable */
  BYTE best;     /* Best position to add */
  int timeBest;  /* Best time to beat - If we can't find a free spot */
  bool done;     /* Finished looping */
 
  count = 0;
  done = FALSE;
  best = 0;
  /* With every record in use, the oldest one (the largest timeOut) is
     the one replaced. */
  timeBest = -1;
  /* A record already kept under this name, from a player with the same
     signed-in state, is replaced, so a player who leaves twice has one
     record, the newest. A record from a player who was signed in to
     WinBolo.net is never replaced by one from a player who was not, or
     the other way: they are two players with one name. */
  while (count < MAX_TANKS && done == FALSE) {
    if (rejoin.item[count].inUse == TRUE && strcmp(rejoin.item[count].playerName, playerName) == 0 &&
        rejoin.item[count].signedIn == signedIn) {
      done = TRUE;
      best = count;
    }
    count++;
  }
  count = 0;
  while (count < MAX_TANKS && done == FALSE) {
    if (rejoin.item[count].inUse == FALSE) {
      done = TRUE;
      best = count;
    } else if (rejoin.item[count].timeOut > timeBest) {
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
  rejoin.item[best].team = team;
  rejoin.item[best].signedIn = signedIn;
  rejoin.item[best].ipv4 = 0;
  rejoin.item[best].timeOut = 0;
}

/*********************************************************
*NAME:          playersRejoinSetAddress
*PURPOSE:
* Puts the IPv4 address the player had when leaving in the
* record for that name and signed-in state. Does nothing when
* there is no such record.
*
*ARGUMENTS:
*  playerName - The player name
*  signedIn   - The signed-in state the record was added with
*  ipv4       - The address (network byte order), 0 for none
*********************************************************/
void playersRejoinSetAddress(const char *playerName, bool signedIn, uint32_t ipv4) {
  BYTE count; /* Looping variable */

  if (playerName == NULL) {
    return;
  }
  count = 0;
  while (count < MAX_TANKS) {
    if (rejoin.item[count].inUse == TRUE && strcmp(rejoin.item[count].playerName, playerName) == 0 &&
        rejoin.item[count].signedIn == signedIn) {
      rejoin.item[count].ipv4 = ipv4;
      return;
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersRejoinGetAddress
*PURPOSE:
* Returns the IPv4 address kept in the record for that name,
* or 0 when there is no record or no address was kept.
*
*ARGUMENTS:
*  playerName - The player name
*********************************************************/
uint32_t playersRejoinGetAddress(const char *playerName) {
  int num = rejoinFindRecord(playerName);

  if (num < 0) {
    return 0;
  }
  return rejoin.item[num].ipv4;
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
  int num;       /* Number player is at, -1 for none */
  unsigned long testItem; /* Pill base item we are testing */
  base ba;
 
  num = rejoinFindRecord(playerName);

  if (num >= 0) {
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
          basesSetBaseOwner(sim, (BYTE) (count+1), playerNum, TRUE, TRUE);
        }        
      }
      count++;
    }
    rejoin.item[num].inUse = FALSE;
  }
}

/*********************************************************
*NAME:          playersRejoinGetTeam
*PURPOSE:
* Returns the team a rejoining player was on when leaving,
* or 0 when there is no record for that name. Leaves the
* record in place: playersRejoinRequest clears it.
*
*ARGUMENTS:
*  playerName - The player name requesting the rejoin
*********************************************************/
BYTE playersRejoinGetTeam(const char *playerName) {
  BYTE team;      /* The record's team */
  bool signedIn;  /* The record's signed-in state, not used here */

  if (playersRejoinFind(playerName, &team, &signedIn) == FALSE) {
    return 0;
  }
  return team;
}

/*********************************************************
*NAME:          playersRejoinFind
*PURPOSE:
* Returns TRUE when there is a record for that name, and
* puts its team and signed-in state in team and signedIn.
* Returns FALSE when there is no record. Leaves the record
* in place.
*
*ARGUMENTS:
*  playerName - The player name requesting the rejoin
*  team       - Set to the record's team
*  signedIn   - Set to the record's signed-in state
*********************************************************/
bool playersRejoinFind(const char *playerName, BYTE *team, bool *signedIn) {
  int num = rejoinFindRecord(playerName);

  *team = 0;
  *signedIn = FALSE;
  if (num < 0) {
    return FALSE;
  }
  *team = rejoin.item[num].team;
  *signedIn = rejoin.item[num].signedIn;
  return TRUE;
}

/*********************************************************
*NAME:          playersRejoinGetAge
*PURPOSE:
* Returns the age (timeOut, in server ticks) of the record
* for that name, or -1 when there is no record. Leaves the
* record in place.
*
*ARGUMENTS:
*  playerName - The player name
*********************************************************/
int playersRejoinGetAge(const char *playerName) {
  int num = rejoinFindRecord(playerName);

  if (num < 0) {
    return -1;
  }
  return rejoin.item[num].timeOut;
}

/*********************************************************
*NAME:          playersRejoinHeldTeams
*PURPOSE:
* For each team number, the age (timeOut) of the oldest
* record in use that holds it, or -1 when no record holds
* it.
*
*ARGUMENTS:
*  age - Filled with MAX_TANKS values, one per team number
*********************************************************/
void playersRejoinHeldTeams(int age[MAX_TANKS]) {
  BYTE count; /* Looping variable */
  BYTE team;  /* The record's team */

  count = 0;
  while (count < MAX_TANKS) {
    age[count] = -1;
    count++;
  }
  count = 0;
  while (count < MAX_TANKS) {
    team = rejoin.item[count].team;
    if (rejoin.item[count].inUse == TRUE && team > 0 && team < MAX_TANKS &&
        rejoin.item[count].timeOut > age[team]) {
      age[team] = rejoin.item[count].timeOut;
    }
    count++;
  }
}

/*********************************************************
*NAME:          playersRejoinClearTeam
*PURPOSE:
* Sets the team to 0 in every record that has that team.
*
*ARGUMENTS:
*  team - The team number
*********************************************************/
void playersRejoinClearTeam(BYTE team) {
  BYTE count; /* Looping variable */

  if (team == 0) {
    return;
  }
  count = 0;
  while (count < MAX_TANKS) {
    if (rejoin.item[count].inUse == TRUE && rejoin.item[count].team == team) {
      rejoin.item[count].team = 0;
    }
    count++;
  }
}
