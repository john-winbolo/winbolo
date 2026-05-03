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
*Name:          WinBolo.net
*Filename:      winbolonet.h
*Author:        John Morrison
*Creation Date: 23/09/01
*Last Modified: 30/03/26
*Purpose:
*  Responsible for interacting with WinBolo.net JSON API
*********************************************************/

#ifndef __WINBOLO_NET_H
#define __WINBOLO_NET_H

#include <stdio.h>
#include <string.h>
#include "../bolo/global.h"

/* Forward declaration — defined in server_sim.h */
#ifndef BALANCEPROPOSAL_TYPEDEF
#define BALANCEPROPOSAL_TYPEDEF
typedef struct BalanceProposal BalanceProposal;
#endif

/* Size of key buffers (32-char hex string + null terminator) */
#define WINBOLONET_KEY_LEN 33

/* Event Types — values match the JSON API event type field */
#define WINBOLO_NET_EVENT_ALLY_JOIN 0     /* Player B has joined Alliance A */
#define WINBOLO_NET_EVENT_ALLY_LEAVE 1    /* Player B has left Alliance A */
#define WINBOLO_NET_EVENT_BASE_CAPTURE 2  /* Player A has captured a base */
#define WINBOLO_NET_EVENT_PILL_CAPTURE 3  /* Player A has captured a pill */
#define WINBOLO_NET_EVENT_TANK_KILL 4     /* Player A has killed a tank */
#define WINBOLO_NET_EVENT_LGM_KILL 5      /* Player A has killed a lgm */
#define WINBOLO_NET_EVENT_LGM_LOST 6      /* Player A has lost their lgm */
#define WINBOLO_NET_EVENT_BASE_STEAL 7    /* Player A has stolen a base */
#define WINBOLO_NET_EVENT_PILL_STEAL 8    /* Player A has stolen a pill */
#define WINBOLO_NET_EVENT_PLAYER_JOIN 9   /* Player A has joined the game */
#define WINBOLO_NET_EVENT_PLAYER_LEAVE 10 /* Player A has left the game */
#define WINBOLO_NET_EVENT_WIN 11          /* Player A has won the game */
#define WINBOLO_NET_EVENT_REJOIN 12       /* Player A is rejoining */
#define WINBOLO_NET_EVENT_QUITTING 13     /* Player A is quitting */

#define WINBOLO_NET_NO_PLAYER 100 /* If no player use this holder */

#define WINBOLO_NET_MAX_NOSEND 60 /* Maximum non transmission time in seconds */

#define WINBOLO_NET_TEAM_MARKER (254)

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
*NAME:          winbolonetCreateClient
*PURPOSE:
* Initialises the WinBolo.net module for a client.
* Joins a game session using an auth token.
* Returns success.
*
*ARGUMENTS:
* token     - WinBolo.net auth token (64-char hex string)
* serverKey - Server session key (32-char hex string)
* errorMsg  - Buffer to hold error message if required
*********************************************************/
bool winbolonetCreateClient(const char *token, const char *serverKey, char *errorMsg);

/*********************************************************
*NAME:          winbolonetServerVerifyToken
*PURPOSE:
* Called by the server to verify a joining player's WBN
* auth token via POST /api/v1/client/join. Stores the
* resulting player_key at the given player slot.
* Returns TRUE on success.
*
*ARGUMENTS:
* token       - WBN auth token from the join request
* playerNum   - Player slot number
* errorMsg    - Buffer for error message on failure
* hasSteam    - Output: set to TRUE if player has linked Steam
* isSupporter - Output: set to TRUE iff the WBN response declares this
*               account as a Supporter (currently always FALSE — DLC field
*               not yet exposed by WBN).
*********************************************************/
bool winbolonetServerVerifyToken(const char *token, BYTE playerNum, char *errorMsg,
                                 bool *hasSteam, bool *isSupporter);

/*********************************************************
*NAME:          winbolonetDestroy
*PURPOSE:
* Destroys the winbolonet module.
* Cleans up any open libraries.
*
*ARGUMENTS:
* isServer - TRUE if we are the server
*********************************************************/
void winbolonetDestroy(bool isServer);

/*********************************************************
*NAME:          winbolonetGoodbye
*PURPOSE:
* Sends final update and server quit message to WinBolo.net.
*
*ARGUMENTS:
*
*********************************************************/
void winbolonetGoodbye(void);

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
*NAME:          winbolonetAddEvent
*PURPOSE:
* Adds a WinBolo.net Event for sending to the server.
*
*ARGUMENTS:
* eventType - Type of event this is
* isServer  - Are we the server for this and not a client
* playerA   - Player A player Number
* playerB   - Player B player Number
*********************************************************/
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB);

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
*NAME:          winbolonetIsRunning
*PURPOSE:
* Returns if the winbolonet module is running or not.
*
*ARGUMENTS:
*
*********************************************************/
bool winbolonetIsRunning(void);

/*********************************************************
*NAME:          winboloNetGetServerKey
*PURPOSE:
* Copies the server key into keyBuff. Will be empty string
* if not participating in WinBolo.net.
*
*ARGUMENTS:
* keyBuff - Buffer to hold key (must be WINBOLONET_KEY_LEN)
*********************************************************/
void winboloNetGetServerKey(char *keyBuff);

/*********************************************************
*NAME:          winboloNetGetMyClientKey
*PURPOSE:
* Copies this client's key into keyBuff. Will be empty
* string if not set or not participating in WinBolo.net.
* Expected to be called by clients.
*
*ARGUMENTS:
* keyBuff - Buffer to hold key (must be WINBOLONET_KEY_LEN)
*********************************************************/
void winboloNetGetMyClientKey(char *keyBuff);

/*********************************************************
*NAME:          winboloNetVerifyClientKey
*PURPOSE:
* Verifies a client key by sending it to WinBolo.net for
* authentication. Returns if it's a valid key for this
* session.
*
*ARGUMENTS:
* playerKey - Player key string to verify
* userName  - Username of the player
* playerNum - Player position Number
*********************************************************/
bool winboloNetVerifyClientKey(const char *playerKey, char *userName, BYTE playerNum);

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
*NAME:          winbolonetAuthLogin
*PURPOSE:
* Authenticates with WinBolo.net via POST /api/v1/auth/login.
* On success, writes the token and expiry into the provided
* buffers and returns TRUE.
*
*ARGUMENTS:
* username      - WinBolo.net username
* password      - WinBolo.net password
* tokenOut      - Buffer for token (must be >= 65 bytes)
* expiryOut     - Buffer for expiry string (must be >= 64 bytes)
* playerNameOut - Buffer for player name (must be >= PLAYER_NAME_LEN)
* errorMsg      - Buffer for error message on failure
*********************************************************/
bool winbolonetAuthLogin(const char *username, const char *password, char *tokenOut, char *expiryOut, char *playerNameOut, char *errorMsg);

/*********************************************************
*NAME:          winbolonetAuthSteam
*PURPOSE:
* Authenticates with WinBolo.net via POST /api/v1/auth/steam
* using a hex-encoded Steam auth ticket. On success, writes
* the token and expiry into the provided buffers and returns
* TRUE.
*
*ARGUMENTS:
* steamTicketHex  - Hex-encoded Steam auth ticket
* tokenOut        - Buffer for token (must be >= 65 bytes)
* expiryOut       - Buffer for expiry string (must be >= 64 bytes)
* playerNameOut   - Buffer for player name (must be >= PLAYER_NAME_LEN)
* errorMsg        - Buffer for error message on failure
*********************************************************/
bool winbolonetAuthSteam(const char *steamTicketHex, char *tokenOut, char *expiryOut, char *playerNameOut, char *errorMsg);

/*********************************************************
*NAME:          winbolonetAuthValidate
*PURPOSE:
* Validates a stored auth token by calling
* POST /api/v1/auth/validate.
* Returns TRUE if the token is valid.
*
*ARGUMENTS:
* token         - The auth token to validate
* playerNameOut - Buffer for player name (must be >= PLAYER_NAME_LEN), may be NULL
* errorMsg      - Buffer for error message on failure
*********************************************************/
bool winbolonetAuthValidate(const char *token, char *playerNameOut, char *errorMsg);

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

#endif /* __WINBOLO_NET_H */
