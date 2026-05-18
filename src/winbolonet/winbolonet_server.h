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
*Name:          WinBolo.net Server
*Filename:      winbolonet_server.h
*Author:        John Morrison
*Creation Date: 23/09/01
*Last Modified: 30/03/26
*Purpose:
*  Server-side WinBolo.net tracker surface: register,
*  per-tick update, lobby/map/teams/balance, client verify
*  and leave. Linked into binaries that run a server.
*********************************************************/

#ifndef __WINBOLO_NET_SERVER_H
#define __WINBOLO_NET_SERVER_H

#include <stdio.h>
#include <string.h>
#include "global.h"

/* Forward declaration — defined in server_sim.h */
#ifndef BALANCEPROPOSAL_TYPEDEF
#define BALANCEPROPOSAL_TYPEDEF
typedef struct BalanceProposal BalanceProposal;
#endif

/*********************************************************
*NAME:          winbolonetCreateServer
*PURPOSE:
* Initialises the WinBolo.net module for a game server.
* Registers with WinBolo.net and obtains a server key.
* Returns success.
*
*ARGUMENTS:
* mapName    - Name of the map
* port       - Port we are running on
* gameType   - Game Type
* ai         - Is AI allowed
* mines      - Mines allowed
* password   - Has password
* numBases   - Number of bases
* numPills   - Number of pills
* freeBases  - Free bases
* freePills  - Free pills
* numPlayers - Number of players in the game
*********************************************************/
bool winbolonetCreateServer(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers);

/*********************************************************
*NAME:          winboloNetVerifyClientKey
*PURPOSE:
* Validates a player_key received on the wire via POST
* /api/v1/client/verify. On success, stores the player_key
* at winboloNetPlayerKey[playerNum] so subsequent
* events/leaves can identify the player to WBN.
* Returns TRUE if WBN accepts the key.
*
*ARGUMENTS:
* playerKey   - 33-byte player_key from the JOIN/REAUTH packet
* playerName  - Player display name (server already has this
*               from the JOIN packet)
* playerNum   - Player slot number
* errorMsg    - Buffer for error message on failure
* hasSteam    - Output: set TRUE if the WBN response declares
*               a linked Steam identity
* isSupporter - Output: set TRUE iff the WBN response declares
*               Supporter status (currently always FALSE —
*               DLC field not yet exposed by WBN).
*********************************************************/
bool winboloNetVerifyClientKey(const char *playerKey, const char *playerName, BYTE playerNum, char *errorMsg, bool *hasSteam, bool *isSupporter);

/*********************************************************
*NAME:          winbolonetServerSendTeams
*PURPOSE:
* Sends the list of teams at the end of the game.
*
*ARGUMENTS:
* array    - BYTE array containing team memberships
* length   - Length of the array
* numTeams - Number of teams in the array
*********************************************************/
void winbolonetServerSendTeams(BYTE *array, BYTE length, BYTE numTeams);

/*********************************************************
*NAME:          winbolonetServerUpdate
*PURPOSE:
* Sends a server winbolo.net update with current state
* and queued events.
*
*ARGUMENTS:
* numPlayers   - Number of players in the game
* numFreeBases - Number of free bases in the game
* numFreePills - Number of free pills in the game
* sendNow      - If TRUE the data should not be queued
*********************************************************/
void winbolonetServerUpdate(BYTE numPlayers, BYTE numFreeBases, BYTE numFreePills, bool sendNow);

/*********************************************************
*NAME:          winboloNetClientLeaveGame
*PURPOSE:
* Called when a player leaves the game. Tells WinBolo.net.
*
*ARGUMENTS:
* playerNum  - Player position Number
* numPlayers - Number of players now in the game
* freeBases  - Number of free bases
* freePills  - Number of free pills
*********************************************************/
void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills);

/*********************************************************
*NAME:          winboloNetIsPlayerParticipant
*PURPOSE:
* Returns if this player number is a winbolo.net
* participant or not.
*
*ARGUMENTS:
* playerNum - Player number to check
*********************************************************/
bool winboloNetIsPlayerParticipant(BYTE playerNum);

/*********************************************************
*NAME:          winboloNetSendLock
*PURPOSE:
* Sends whether the game is locked or not to winbolo.net.
*
*ARGUMENTS:
* isLocked - Is this game locked or not
*********************************************************/
void winboloNetSendLock(bool isLocked);

/*********************************************************
*NAME:          winbolonetSendMapChange
*PURPOSE:
* Notifies WinBolo.net that the map changed during the
* lobby (e.g. via skip vote). POSTs to /api/v1/server/map.
*
*ARGUMENTS:
* mapName   - Name of the new map
* numBases  - Number of bases on the new map
* numPills  - Number of pills on the new map
* freeBases - Free bases (all, since lobby)
* freePills - Free pills (all, since lobby)
*********************************************************/
void winbolonetSendMapChange(char *mapName, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills);

/*********************************************************
*NAME:          winbolonetReturnToLobby
*PURPOSE:
* Handles the WBN session cycle when the server returns to
* the lobby between rounds. Quits the old session, clears
* player keys and events, and registers a new session with
* the new map/settings. HTTP layer is preserved.
* Returns TRUE on success, FALSE on registration failure.
*
*ARGUMENTS:
* mapName    - Name of the new map
* port       - Port we are running on
* gameType   - Game Type
* ai         - Is AI allowed
* mines      - Mines allowed
* password   - Has password
* numBases   - Number of bases
* numPills   - Number of pills
* freeBases  - Free bases
* freePills  - Free pills
* numPlayers - Number of players in the game
*********************************************************/
bool winbolonetReturnToLobby(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers);

/*********************************************************
*NAME:          winbolonetSendLobbyStatus
*PURPOSE:
* Notifies WinBolo.net whether this server is currently in
* the lobby or in-game. POSTs to /api/v1/server/lobby.
*
*ARGUMENTS:
* inLobby - TRUE if server is in lobby state
*********************************************************/
void winbolonetSendLobbyStatus(bool inLobby);

/*********************************************************
*NAME:          winbolonetServerRequestBalance
*PURPOSE:
* Calls the WBN API to get skill-based team assignments
* for the current lobby players.
* Returns TRUE on success, FALSE on failure.
*
*ARGUMENTS:
* totalPlayers - Total number of player slots in the game
* teamSize     - Desired team size
* outProposal  - Output: filled BalanceProposal
*********************************************************/
bool winbolonetServerRequestBalance(uint8_t totalPlayers, uint8_t teamSize, BalanceProposal *outProposal);

#endif /* __WINBOLO_NET_SERVER_H */
