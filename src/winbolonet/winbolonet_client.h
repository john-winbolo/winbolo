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
*Name:          WinBolo.net Client
*Filename:      winbolonet_client.h
*Author:        John Morrison
*Creation Date: 23/09/01
*Last Modified: 30/03/26
*Purpose:
*  Client-side WinBolo.net surface: user auth. Linked into
*  binaries with a UI (SDL3 client, LogViewer).
*********************************************************/

#ifndef __WINBOLO_NET_CLIENT_H
#define __WINBOLO_NET_CLIENT_H

#include <stdio.h>
#include <string.h>
#include "global.h"

/*********************************************************
* Per-mode WinBolo.net play stats, parsed from the auth/login,
* auth/steam and auth/validate responses. Each integer is -1 when
* the field is absent from the response: the "open" mode carries
* only the counter fields, while "tourn"/"strict" also carry
* score (ELO), win/loss tallies and a ladder rank (rank -1 means
* unranked or absent). `valid` is FALSE when the response carried
* no stats object at all.
*********************************************************/
typedef struct {
  int numGames, numBases, numPills, numTanks;  /* -1 if absent */
  int score, wins, loses;                       /* -1 if absent (e.g. open) */
  int rank, rankTotal;                          /* rank -1 = unranked/absent */
} WbnModeStats;

typedef struct {
  bool valid;
  WbnModeStats open, tourn, strict;
} WbnStats;

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
* rankOut       - 1v1 ladder position; -1 when unranked. May be NULL.
* rankTotalOut  - Total ranked players; 0 when absent. May be NULL.
* errorMsg      - Buffer for error message on failure
*********************************************************/
bool winbolonetAuthLogin(const char *username, const char *password, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg);

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
* rankOut         - 1v1 ladder position; -1 when unranked. May be NULL.
* rankTotalOut    - Total ranked players; 0 when absent. May be NULL.
* errorMsg        - Buffer for error message on failure
*********************************************************/
bool winbolonetAuthSteam(const char *steamTicketHex, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg);

/*********************************************************
*NAME:          winbolonetAuthSteamRegister
*PURPOSE:
* Registers a new WinBolo.net account via
* POST /api/v1/auth/steam/register using a hex-encoded
* Steam auth ticket, a chosen username and an optional
* email. On success, writes the token and expiry into the
* provided buffers and returns TRUE.
*
*ARGUMENTS:
* steamTicketHex  - Hex-encoded Steam auth ticket
* username        - Chosen WinBolo.net username
* email           - Optional email; omitted when NULL or empty
* tokenOut        - Buffer for token (must be >= 65 bytes)
* expiryOut       - Buffer for expiry string (must be >= 64 bytes)
* playerNameOut   - Buffer for player name (must be >= PLAYER_NAME_LEN)
* rankOut         - 1v1 ladder position; -1 when unranked. May be NULL.
* rankTotalOut    - Total ranked players; 0 when absent. May be NULL.
* errorMsg        - Buffer for the server's human error message on failure
* errorCodeOut    - Buffer for the server's machine error code (e.g.
*                   "steam_already_linked") on failure; empty when absent.
*                   May be NULL.
*********************************************************/
bool winbolonetAuthSteamRegister(const char *steamTicketHex, const char *username, const char *email, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg, char *errorCodeOut);

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
* rankOut       - 1v1 ladder position; -1 when unranked. May be NULL.
* rankTotalOut  - Total ranked players; 0 when absent. May be NULL.
* errorMsg      - Buffer for error message on failure
*********************************************************/
bool winbolonetAuthValidate(const char *token, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg);

/*********************************************************
*NAME:          winbolonetClientJoinSession
*PURPOSE:
* Exchange (apiToken, serverKey) for a server-scoped
* player_key via POST /api/v1/client/join. The apiToken is
* the long-lived WBN credential stored in prefs; the
* player_key is short-lived and scoped to one server's
* session and is what the client ships on the JOIN wire.
* Returns TRUE on success (playerKeyOut populated), FALSE
* on failure (errorMsg populated).
*
*ARGUMENTS:
* apiToken     - WBN API token (from prefs)
* serverKey    - server_key of the server we're about to join
* playerKeyOut - Buffer for issued player_key
*                (must be >= WINBOLONET_KEY_LEN bytes)
* errorMsg     - Buffer for error message on failure
*********************************************************/
bool winbolonetClientJoinSession(const char *apiToken, const char *serverKey, char *playerKeyOut, char *errorMsg);

#endif /* __WINBOLO_NET_CLIENT_H */
