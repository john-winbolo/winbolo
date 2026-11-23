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
*Filename:      playersrejoin.h
*Author:        John Morrison
*Creation Date: 22/6/00
*Last Modified: 22/6/00
*Purpose:
*  Looks after players rejoin and ownerships.
*********************************************************/

#ifndef _PLAYERS_REJOIN_H
#define _PLAYERS_REJOIN_H

struct GameSim;

#include "global.h"
#include "pillbox.h"
#include "bases.h"

/* The server tick that counts a record's timeOut, SERVER_TICK_LENGTH
 * (server_lifecycle.h), is two game ticks: 20 ms, 50 times a second */
#define REJOIN_TICKS_PER_SECOND (1000 / (GAME_TICK_LENGTH * 2))

/* A record lasts 5 minutes */
#define MAX_REJOIN_TIME (5 * 60 * REJOIN_TICKS_PER_SECOND)

/* The different player rejoin information */
typedef struct {
  bool inUse;                       /* Is player slot in use? */
  char playerName[PLAYER_NAME_LEN]; /* Player name */
  PlayerBitMap pills;               /* pills and base ownership */
  PlayerBitMap bases;
  BYTE team;                        /* Team the player was on when leaving, 0 for none */
  bool signedIn;                    /* The player was signed in to WinBolo.net when leaving */
  uint32_t ipv4;                    /* The player's IPv4 address when leaving, in network
                                       byte order, 0 when not known. No port. */
  int timeOut;                     /* Time left */
} playerRejoinInfo;

/* Array of all the players */
typedef struct {
  playerRejoinInfo item[MAX_TANKS];
} playersRejoin;

/* Prototypes */

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
void playersRejoinCreate();

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
void playersRejoinDestroy();

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
void playersRejoinUpdate();

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
void playersRejoinAddPlayer(char *playerName, PlayerBitMap pills, PlayerBitMap bases, BYTE team, bool signedIn);

/*********************************************************
*NAME:          playersRejoinSetAddress
*PURPOSE:
* Puts the IPv4 address the player had when leaving (network
* byte order, no port) in the record for that name. Does
* nothing when there is no record. playersRejoinAddPlayer
* sets the address to 0, so call this after it.
*
*ARGUMENTS:
*  playerName - The player name
*  ipv4       - The address, 0 for not known
*********************************************************/
void playersRejoinSetAddress(const char *playerName, uint32_t ipv4);

/*********************************************************
*NAME:          playersRejoinGetAddress
*PURPOSE:
* Returns the IPv4 address (network byte order) kept in the
* record for that name, or 0 when there is no record or no
* address was kept. Leaves the record in place.
*
*ARGUMENTS:
*  playerName - The player name
*********************************************************/
uint32_t playersRejoinGetAddress(const char *playerName);

/*********************************************************
*NAME:          playersRejoinGetTeam
*PURPOSE:
* Returns the team a rejoining player was on when leaving,
* or 0 when there is no record for that name (none was
* kept, or it has timed out). Leaves the record in place.
*
*ARGUMENTS:
*  playerName - The player name requesting the rejoin
*********************************************************/
BYTE playersRejoinGetTeam(const char *playerName);

/*********************************************************
*NAME:          playersRejoinFind
*PURPOSE:
* Returns TRUE when there is a record for that name, and
* puts its team (0 for none) in team and whether the player
* was signed in to WinBolo.net when leaving in signedIn.
* Returns FALSE when there is no record. Leaves the record
* in place.
*
*ARGUMENTS:
*  playerName - The player name requesting the rejoin
*  team       - Set to the record's team
*  signedIn   - Set to the record's signed-in state
*********************************************************/
bool playersRejoinFind(const char *playerName, BYTE *team, bool *signedIn);

/*********************************************************
*NAME:          playersRejoinClearTeam
*PURPOSE:
* Sets the team to 0 in every record that has that team.
* Call it when the team number is given to a new side, so
* that no record puts a rejoiner on a side they were never
* on.
*
*ARGUMENTS:
*  team - The team number
*********************************************************/
void playersRejoinClearTeam(BYTE team);

/*********************************************************
*NAME:          playersRejoinHeldTeams
*PURPOSE:
* For each team number, the age (timeOut) of the oldest
* record in use that holds it, or -1 when no record holds
* it. age[0] is always -1.
*
*ARGUMENTS:
*  age - Filled with MAX_TANKS values, one per team number
*********************************************************/
void playersRejoinHeldTeams(int age[MAX_TANKS]);

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
int playersRejoinGetAge(const char *playerName);

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
void playersRejoinRequest(struct GameSim *sim, char *playerName, BYTE playerNum, pillboxes *pb);

#endif /* _PLAYERS_REJOIN_H */
