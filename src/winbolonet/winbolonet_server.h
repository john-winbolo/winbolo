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
* Current lobby/server state pushed to WinBolo.net.
*
* The server keeps a copy of this in the winbolonet module
* via winbolonetSetLobbyInfo(); the register / beginSession
* / update body builders and winbolonetSendLobbyUpdate()
* read the extended fields from it (the winbolonet module
* has no ServerSim handle of its own). map/md5/counts/
* settings are all carried so a single lobby_update POST is
* a complete, idempotent snapshot.
*********************************************************/
typedef struct {
  char     map[256];                 /* Map name */
  char     mapMd5[33];               /* 32 hex of BMAPBOLO bytes; "" if none */
  bool     randomMap;                /* Randomly generated map */
  BYTE     gameType;                 /* 1=open, 2=tournament, 3=strict */
  BYTE     ai;                       /* aiType policy (0..3) */
  bool     mines;                    /* Hidden mines */
  bool     ranked;                   /* Ranked match */
  bool     allowNewPlayers;          /* Lobby join gate */
  bool     autoLock;                 /* Auto-lock on game start */
  bool     hasLobby;                 /* Server has a lobby (false for -nolobby/-maprotate) */
  bool     timeLimit;               /* Time limit enabled (false=unlimited) */
  uint16_t timeMinutes;              /* Time limit in minutes */
  uint16_t lobbyLocks;               /* LOBBY_LOCK_* bitmask */
  BYTE     numBases;                 /* Total bases */
  BYTE     numPills;                 /* Total pills */
  BYTE     freeBases;                /* Neutral bases */
  BYTE     freePills;                /* Neutral pills */
  BYTE     numHumans;                /* Connected human players */
  BYTE     numBots;                  /* Bot players */
} WbnLobbyInfo;

/*********************************************************
*NAME:          winbolonetSetLobbyInfo
*PURPOSE:
* Stashes the current lobby/server state in the winbolonet
* module. The register / beginSession / update builders and
* winbolonetSendLobbyUpdate read the extended fields from
* this copy. Call before register/update and whenever the
* lobby state changes.
*
*ARGUMENTS:
* info - Pointer to the current lobby state
*********************************************************/
void winbolonetSetLobbyInfo(const WbnLobbyInfo *info);

/*********************************************************
*NAME:          winbolonetSendLobbyUpdate
*PURPOSE:
* Sends the full current lobby snapshot (map + settings +
* counts) via POST /api/v1/server/lobby_update, keyed by
* the server key. Idempotent; queued via the background
* thread (fire-and-forget). Supersedes server/map.
*********************************************************/
void winbolonetSendLobbyUpdate(void);

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
*NAME:          winbolonetEndSession
*PURPOSE:
* Ends the current WBN session: drains the background
* thread, POSTs server/quit, clears the server bearer,
* clears per-slot player keys, and resets the event queue.
* The HTTP layer stays alive. Caller pairs this with
* winbolonetBeginSession to start the next round; for
* round-end log uploads, callers also call
* serverDedicatedLogFlushPendingUpload between the two so
* the upload runs after WBN accepts that the session is
* over and before server/register issues a new key.
*********************************************************/
void winbolonetEndSession(void);

/*********************************************************
*NAME:          winbolonetBeginSession
*PURPOSE:
* Registers a fresh WBN session with the supplied
* map/settings, stores the new server_key + bearer, and
* restarts the background thread. Pairs with
* winbolonetEndSession at round boundaries.
* Returns TRUE on success, FALSE on registration failure
* (sets winboloNetRunning=FALSE on failure).
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
bool winbolonetBeginSession(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers);

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
* botSlots     - Optional slot indices to include as non-WBN bot
*                participants (sent to WBN as "bot:N" sentinels).
*                NULL or numBotSlots=0 = humans-only request.
* numBotSlots  - Number of bot slots in botSlots[]
* outProposal  - Output: filled BalanceProposal
*********************************************************/
bool winbolonetServerRequestBalance(uint8_t totalPlayers, uint8_t teamSize,
                                     const uint8_t *botSlots, uint8_t numBotSlots,
                                     BalanceProposal *outProposal);

#endif /* __WINBOLO_NET_SERVER_H */
