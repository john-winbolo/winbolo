/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
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
*Filename:      winbolonet_client.c
*Author:        John Morrison
*Creation Date: 23/09/01
*Last Modified: 30/03/26
*Purpose:
*  Client-side WinBolo.net auth: login (username/password),
*  Steam ticket exchange, and token validate.
*********************************************************/

#include <stdlib.h>
#include "cJSON.h"
#include "winbolonet_client.h"
#include "winbolonet_core.h"
#include "http.h"

/* Defined in winbolonet_core.c; consulted before httpDestroy() so the
 * one-shot auth calls below don't tear down a shared HTTP handle that
 * a running server/client session owns. Without the guard, the next
 * wbn_api_post short-circuits to -1 on !httpStarted, which masquerades
 * as "no response from WinBolo.net" for things like server/balance. */
extern bool winboloNetRunning;

/*********************************************************
*NAME:          wbnParseRank
*PURPOSE:
* Extracts the optional top-level 1v1 ladder position from
* an auth response. `rank` is JSON null/absent when the
* player is unranked, mapped to -1; an integer is the
* 1-based position. `rank_total` is the ranked-player
* count, 0 when absent. Either out-param may be NULL.
* Tolerates older servers that omit both fields.
*********************************************************/
static void wbnParseRank(cJSON *resp, int *rankOut, int *rankTotalOut) {
  if (rankOut) {
    cJSON *rankObj = cJSON_GetObjectItem(resp, "rank");
    *rankOut = (rankObj && cJSON_IsNumber(rankObj)) ? rankObj->valueint : -1;
  }
  if (rankTotalOut) {
    cJSON *totalObj = cJSON_GetObjectItem(resp, "rank_total");
    *rankTotalOut = (totalObj && cJSON_IsNumber(totalObj)) ? totalObj->valueint : 0;
  }
}

/*********************************************************
*NAME:          wbnJsonInt
*PURPOSE:
* Reads an integer member from a JSON object, returning
* `absent` when the object or member is missing or not a
* number (e.g. an explicit null rank).
*********************************************************/
static int wbnJsonInt(cJSON *obj, const char *key, int absent) {
  cJSON *item = cJSON_GetObjectItem(obj, key);
  return (item && cJSON_IsNumber(item)) ? item->valueint : absent;
}

/*********************************************************
*NAME:          wbnParseModeStats
*PURPOSE:
* Fills one WbnModeStats from a per-mode stats object.
* Every absent field maps to -1, including a null rank.
* `mode` may be NULL (e.g. the response omits that mode),
* in which case all fields come out -1.
*********************************************************/
static void wbnParseModeStats(cJSON *mode, WbnModeStats *out) {
  out->numGames = wbnJsonInt(mode, "num_games", -1);
  out->numBases = wbnJsonInt(mode, "num_bases", -1);
  out->numPills = wbnJsonInt(mode, "num_pills", -1);
  out->numTanks = wbnJsonInt(mode, "num_tanks", -1);
  out->score = wbnJsonInt(mode, "score", -1);
  out->wins = wbnJsonInt(mode, "wins", -1);
  out->loses = wbnJsonInt(mode, "loses", -1);
  out->rank = wbnJsonInt(mode, "rank", -1);
  out->rankTotal = wbnJsonInt(mode, "rank_total", -1);
}

/*********************************************************
*NAME:          wbnParseStats
*PURPOSE:
* Extracts the optional per-mode `stats` object from an auth
* response into `out`. `valid` is set when the response
* carried a stats object; each mode's absent fields map to
* -1 (a null rank included). Tolerates older servers that
* omit the object entirely.
*********************************************************/
static void wbnParseStats(cJSON *resp, WbnStats *out) {
  cJSON *stats = cJSON_GetObjectItem(resp, "stats");
  out->valid = (stats != NULL && cJSON_IsObject(stats));
  wbnParseModeStats(cJSON_GetObjectItem(stats, "open"), &out->open);
  wbnParseModeStats(cJSON_GetObjectItem(stats, "tourn"), &out->tourn);
  wbnParseModeStats(cJSON_GetObjectItem(stats, "strict"), &out->strict);
}

/*********************************************************
*NAME:          winbolonetAuthLogin
*PURPOSE:
* Authenticates via POST /api/v1/auth/login and returns
* the token and expiry on success.
*********************************************************/
bool winbolonetAuthLogin(const char *username, const char *password, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

  if (rankOut) *rankOut = -1;
  if (rankTotalOut) *rankTotalOut = 0;
  if (statsOut) statsOut->valid = FALSE;

  if (httpCreate() != TRUE) {
    strcpy(errorMsg, "Could not initialise HTTP");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "username", username);
  cJSON_AddStringToObject(body, "password", password);

  status = wbn_api_call("auth/login", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      cJSON *tokenObj = cJSON_GetObjectItem(resp, "token");
      cJSON *expiryObj = cJSON_GetObjectItem(resp, "expires_at");
      if (tokenObj && cJSON_IsString(tokenObj) && expiryObj && cJSON_IsString(expiryObj)) {
        strcpy(tokenOut, tokenObj->valuestring);
        strcpy(expiryOut, expiryObj->valuestring);
        playerNameOut[0] = '\0';
        cJSON *nameObj = cJSON_GetObjectItem(resp, "display_name");
        if (nameObj && cJSON_IsString(nameObj)) {
          strncpy(playerNameOut, nameObj->valuestring, PLAYER_NAME_LEN - 1);
          playerNameOut[PLAYER_NAME_LEN - 1] = '\0';
        }
        wbnParseRank(resp, rankOut, rankTotalOut);
        if (statsOut) wbnParseStats(resp, statsOut);
        ok = TRUE;
      } else {
        strcpy(errorMsg, "Invalid response from WinBolo.net");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "Login failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

  cJSON_Delete(resp);
  if (winboloNetRunning != TRUE) {
    httpDestroy();
  }
  return ok;
}

/*********************************************************
*NAME:          winbolonetAuthSteam
*PURPOSE:
* Authenticates via POST /api/v1/auth/steam using a
* hex-encoded Steam auth ticket. Returns token and expiry
* on success, just like winbolonetAuthLogin.
*********************************************************/
bool winbolonetAuthSteam(const char *steamTicketHex, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

  if (rankOut) *rankOut = -1;
  if (rankTotalOut) *rankTotalOut = 0;
  if (statsOut) statsOut->valid = FALSE;

  if (httpCreate() != TRUE) {
    strcpy(errorMsg, "Could not initialise HTTP");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "ticket", steamTicketHex);

  status = wbn_api_call("auth/steam", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      cJSON *tokenObj = cJSON_GetObjectItem(resp, "token");
      cJSON *expiryObj = cJSON_GetObjectItem(resp, "expires_at");
      if (tokenObj && cJSON_IsString(tokenObj) && expiryObj && cJSON_IsString(expiryObj)) {
        strcpy(tokenOut, tokenObj->valuestring);
        strcpy(expiryOut, expiryObj->valuestring);
        playerNameOut[0] = '\0';
        cJSON *nameObj = cJSON_GetObjectItem(resp, "display_name");
        if (nameObj && cJSON_IsString(nameObj)) {
          strncpy(playerNameOut, nameObj->valuestring, PLAYER_NAME_LEN - 1);
          playerNameOut[PLAYER_NAME_LEN - 1] = '\0';
        }
        wbnParseRank(resp, rankOut, rankTotalOut);
        if (statsOut) wbnParseStats(resp, statsOut);
        ok = TRUE;
      } else {
        strcpy(errorMsg, "Invalid response from WinBolo.net");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "Steam authentication failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

  cJSON_Delete(resp);
  if (winboloNetRunning != TRUE) {
    httpDestroy();
  }
  return ok;
}

/*********************************************************
*NAME:          winbolonetAuthSteamRegister
*PURPOSE:
* Registers a new WinBolo.net account via
* POST /api/v1/auth/steam/register using a hex-encoded
* Steam auth ticket plus a chosen username (and optional
* email). Returns token and expiry on success, just like
* winbolonetAuthSteam.
*********************************************************/
bool winbolonetAuthSteamRegister(const char *steamTicketHex, const char *username, const char *email, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg, char *errorCodeOut) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

  if (rankOut) *rankOut = -1;
  if (rankTotalOut) *rankTotalOut = 0;
  if (statsOut) statsOut->valid = FALSE;
  if (errorCodeOut) errorCodeOut[0] = '\0';

  if (httpCreate() != TRUE) {
    strcpy(errorMsg, "Could not initialise HTTP");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "ticket", steamTicketHex);
  cJSON_AddStringToObject(body, "username", username);
  if (email && email[0] != '\0') {
    cJSON_AddStringToObject(body, "email", email);
  }

  status = wbn_api_call("auth/steam/register", body, &resp);
  cJSON_Delete(body);

  /* The server pairs a human "error" message with a machine "code"
   * (e.g. "steam_already_linked") on failure; capture the code so the
   * caller can map it to a localized message. */
  if (resp && errorCodeOut) {
    cJSON *codeObj = cJSON_GetObjectItem(resp, "code");
    if (codeObj && cJSON_IsString(codeObj)) {
      strcpy(errorCodeOut, codeObj->valuestring);
    }
  }

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      cJSON *tokenObj = cJSON_GetObjectItem(resp, "token");
      cJSON *expiryObj = cJSON_GetObjectItem(resp, "expires_at");
      if (tokenObj && cJSON_IsString(tokenObj) && expiryObj && cJSON_IsString(expiryObj)) {
        strcpy(tokenOut, tokenObj->valuestring);
        strcpy(expiryOut, expiryObj->valuestring);
        playerNameOut[0] = '\0';
        cJSON *nameObj = cJSON_GetObjectItem(resp, "display_name");
        if (nameObj && cJSON_IsString(nameObj)) {
          strncpy(playerNameOut, nameObj->valuestring, PLAYER_NAME_LEN - 1);
          playerNameOut[PLAYER_NAME_LEN - 1] = '\0';
        }
        wbnParseRank(resp, rankOut, rankTotalOut);
        if (statsOut) wbnParseStats(resp, statsOut);
        ok = TRUE;
      } else {
        strcpy(errorMsg, "Invalid response from WinBolo.net");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "Steam registration failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

  cJSON_Delete(resp);
  if (winboloNetRunning != TRUE) {
    httpDestroy();
  }
  return ok;
}

/*********************************************************
*NAME:          winbolonetAuthValidate
*PURPOSE:
* Validates a token via POST /api/v1/auth/validate.
* Returns TRUE if the token is still valid.
*********************************************************/
bool winbolonetAuthValidate(const char *token, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

  if (rankOut) *rankOut = -1;
  if (rankTotalOut) *rankTotalOut = 0;
  if (statsOut) statsOut->valid = FALSE;

  if (httpCreate() != TRUE) {
    strcpy(errorMsg, "Could not initialise HTTP");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "token", token);

  status = wbn_api_call("auth/validate", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *validObj = cJSON_GetObjectItem(resp, "valid");
    if (validObj && cJSON_IsTrue(validObj)) {
      if (playerNameOut) {
        playerNameOut[0] = '\0';
        cJSON *nameObj = cJSON_GetObjectItem(resp, "display_name");
        if (nameObj && cJSON_IsString(nameObj)) {
          strncpy(playerNameOut, nameObj->valuestring, PLAYER_NAME_LEN - 1);
          playerNameOut[PLAYER_NAME_LEN - 1] = '\0';
        }
      }
      wbnParseRank(resp, rankOut, rankTotalOut);
      if (statsOut) wbnParseStats(resp, statsOut);
      ok = TRUE;
    } else {
      cJSON *errObj = cJSON_GetObjectItem(resp, "error");
      if (errObj && cJSON_IsString(errObj)) {
        strcpy(errorMsg, errObj->valuestring);
      } else {
        strcpy(errorMsg, "Token is no longer valid");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "Validation failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

  cJSON_Delete(resp);
  if (winboloNetRunning != TRUE) {
    httpDestroy();
  }
  return ok;
}

/*********************************************************
*NAME:          winbolonetClientJoinSession
*PURPOSE:
* Exchanges (apiToken, serverKey) for a server-scoped
* player_key via POST /api/v1/client/join. The API token
* never leaves the client; only the issued player_key gets
* shipped in the JOIN packet's wire field.
*********************************************************/
bool winbolonetClientJoinSession(const char *apiToken, const char *serverKey, char *playerKeyOut, char *errorMsg) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

  playerKeyOut[0] = '\0';

  if (httpCreate() != TRUE) {
    strcpy(errorMsg, "Could not initialise HTTP");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "token", apiToken);
  cJSON_AddStringToObject(body, "server_key", serverKey);

  status = wbn_api_call("client/join", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      cJSON *keyObj = cJSON_GetObjectItem(resp, "player_key");
      if (keyObj && cJSON_IsString(keyObj)) {
        strncpy(playerKeyOut, keyObj->valuestring, WINBOLONET_KEY_LEN - 1);
        playerKeyOut[WINBOLONET_KEY_LEN - 1] = '\0';
        ok = TRUE;
      } else {
        strcpy(errorMsg, "WinBolo.net returned no player key");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "WinBolo.net join failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

  cJSON_Delete(resp);
  if (winboloNetRunning != TRUE) {
    httpDestroy();
  }
  return ok;
}

/*********************************************************
*NAME:          winbolonetClientJoinSpectatorSession
*PURPOSE:
* Exchanges a logged-in user's apiToken (bearer) or an
* anonymous {server_key, player_name} for a server-scoped
* spectator_key via POST /api/v1/client/join_spectator.
* Mirrors winbolonetClientJoinSession; the apiToken never
* leaves the client (it rides only as the Authorization
* bearer), and only the issued spectator_key is shipped in
* the JOIN packet.
*********************************************************/
bool winbolonetClientJoinSpectatorSession(const char *apiToken, const char *serverKey, const char *playerName, char *spectatorKeyOut, char *errorMsg) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;
  bool loggedIn = (apiToken != NULL && apiToken[0] != '\0');

  spectatorKeyOut[0] = '\0';

  if (httpCreate() != TRUE) {
    strcpy(errorMsg, "Could not initialise HTTP");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", serverKey);
  if (!loggedIn) {
    /* Anonymous viewer: no bearer; the backend mints an
     * unattributed spectator_key against the supplied name. */
    cJSON_AddStringToObject(body, "player_name", playerName ? playerName : "");
  }

  status = wbn_api_call_bearer("client/join_spectator", body,
                               loggedIn ? apiToken : NULL, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      cJSON *keyObj = cJSON_GetObjectItem(resp, "spectator_key");
      if (keyObj && cJSON_IsString(keyObj)) {
        strncpy(spectatorKeyOut, keyObj->valuestring, WINBOLONET_KEY_LEN - 1);
        spectatorKeyOut[WINBOLONET_KEY_LEN - 1] = '\0';
        ok = TRUE;
      } else {
        strcpy(errorMsg, "WinBolo.net returned no spectator key");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "WinBolo.net spectator join failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

  cJSON_Delete(resp);
  if (winboloNetRunning != TRUE) {
    httpDestroy();
  }
  return ok;
}
