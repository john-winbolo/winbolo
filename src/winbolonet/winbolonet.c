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
*Filename:      winbolonet.c
*Author:        John Morrison
*Creation Date: 23/09/01
*Last Modified: 30/03/26
*Purpose:
*  Responsible for interacting with WinBolo.net via the
*  JSON REST API (/api/v1/).
*********************************************************/

#include <time.h>
#include <stdlib.h>
#include "cJSON.h"
#include "winbolonet.h"
#include "http.h"
#include "../server/server_sim.h"
#include "../bolo/netpacks.h"
#include "winbolonetevents.h"
#include "winbolonetthread.h"

bool winboloNetRunning = FALSE;

char winboloNetServerKey[WINBOLONET_KEY_LEN];
/* Keys for each player if in use - Always position 0 if we are a client */
char winboloNetPlayerKey[MAX_TANKS][WINBOLONET_KEY_LEN];
time_t winboloNetLastSent;

/*********************************************************
*NAME:          winbolonetCreateServer
*PURPOSE:
* Initialises the WinBolo.net module for a game server.
* Registers with WinBolo.net via POST /api/v1/server/register.
* If successful, stores the server key and starts the
* background update thread.
*********************************************************/
bool winbolonetCreateServer(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  BYTE count;
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  char versionStr[16];

  serverSimConsoleMessage("WinBolo.net Startup");
  winboloNetRunning = FALSE;
  winbolonetEventsCreate();
  winboloNetServerKey[0] = '\0';
  for (count = 0; count < MAX_TANKS; count++) {
    winboloNetPlayerKey[count][0] = '\0';
  }

  winboloNetRunning = httpCreate();
  if (winboloNetRunning != TRUE) {
    return FALSE;
  }

  /* Build version string from game version defines.
     Note: the hex defines (0x01, 0x08) are for the binary protocol;
     the display version is constructed here as "major.minor.revision". */
  snprintf(versionStr, sizeof(versionStr), "%d.%d%d", BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION);

  /* Register server with WinBolo.net */
  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "map", mapName);
  cJSON_AddNumberToObject(body, "port", port);
  cJSON_AddNumberToObject(body, "game_type", gameType);
  cJSON_AddNumberToObject(body, "ai", ai);
  cJSON_AddBoolToObject(body, "mines", mines);
  cJSON_AddBoolToObject(body, "password", password);
  cJSON_AddNumberToObject(body, "num_bases", numBases);
  cJSON_AddNumberToObject(body, "num_pills", numPills);
  cJSON_AddNumberToObject(body, "free_bases", freeBases);
  cJSON_AddNumberToObject(body, "free_pills", freePills);
  cJSON_AddNumberToObject(body, "num_players", numPlayers);
  cJSON_AddStringToObject(body, "version", versionStr);

  status = wbn_api_call("server/register", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    cJSON *keyObj = cJSON_GetObjectItem(resp, "server_key");
    if (errObj && cJSON_IsString(errObj)) {
      fprintf(stderr, "WinBolo.net register error: %s\n", errObj->valuestring);
      serverSimConsoleMessage("Error: WinBolo.net registration failed");
      cJSON_Delete(resp);
      winbolonetDestroy(TRUE);
      return winboloNetRunning;
    }
    if (keyObj && cJSON_IsString(keyObj)) {
      strncpy(winboloNetServerKey, keyObj->valuestring, WINBOLONET_KEY_LEN - 1);
      winboloNetServerKey[WINBOLONET_KEY_LEN - 1] = '\0';
      serverSimConsoleMessage("\tWinBolo.net: Server registered");
      winbolonetThreadCreate();
      winboloNetLastSent = time(NULL);
    } else {
      serverSimConsoleMessage("Error: WinBolo.net returned no server key");
      cJSON_Delete(resp);
      winbolonetDestroy(TRUE);
      return winboloNetRunning;
    }
  } else {
    if (resp) {
      cJSON *errObj = cJSON_GetObjectItem(resp, "error");
      if (errObj && cJSON_IsString(errObj)) {
        fprintf(stderr, "WinBolo.net error: %s\n", errObj->valuestring);
      }
    }
    serverSimConsoleMessage("Error: No response from WinBolo.net - WinBolo.net disabled");
    cJSON_Delete(resp);
    winbolonetDestroy(TRUE);
    return winboloNetRunning;
  }

  cJSON_Delete(resp);
  return winboloNetRunning;
}

/*********************************************************
*NAME:          winbolonetCreateClient
*PURPOSE:
* Initialises the WinBolo.net module for a client.
* Joins a game session via POST /api/v1/client/join.
* Returns success.
*********************************************************/
bool winbolonetCreateClient(const char *token, const char *serverKey, char *errorMsg) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;

  if (winboloNetRunning == FALSE) {
    winboloNetRunning = httpCreate();
    winboloNetServerKey[0] = '\0';
  }
  if (winboloNetRunning != TRUE) {
    strcpy(errorMsg, "Error: Could not initialise HTTP");
    return FALSE;
  }

  /* Join game via WinBolo.net */
  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "token", token);
  cJSON_AddStringToObject(body, "server_key", serverKey);

  status = wbn_api_call("client/join", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
      cJSON_Delete(resp);
      winbolonetDestroy(FALSE);
      return winboloNetRunning;
    }
    cJSON *keyObj = cJSON_GetObjectItem(resp, "player_key");
    if (keyObj && cJSON_IsString(keyObj)) {
      strncpy(winboloNetServerKey, serverKey, WINBOLONET_KEY_LEN - 1);
      winboloNetServerKey[WINBOLONET_KEY_LEN - 1] = '\0';
      strncpy(winboloNetPlayerKey[0], keyObj->valuestring, WINBOLONET_KEY_LEN - 1);
      winboloNetPlayerKey[0][WINBOLONET_KEY_LEN - 1] = '\0';
    } else {
      strcpy(errorMsg, "Error: WinBolo.net returned no player key");
      cJSON_Delete(resp);
      winbolonetDestroy(FALSE);
      return winboloNetRunning;
    }
  } else {
    if (resp) {
      cJSON *errObj = cJSON_GetObjectItem(resp, "error");
      if (errObj && cJSON_IsString(errObj)) {
        strcpy(errorMsg, errObj->valuestring);
      } else {
        strcpy(errorMsg, "Error: No response from WinBolo.net - WinBolo.net disabled");
      }
    } else {
      strcpy(errorMsg, "Error: No response from WinBolo.net - WinBolo.net disabled");
    }
    cJSON_Delete(resp);
    winbolonetDestroy(FALSE);
    return winboloNetRunning;
  }

  cJSON_Delete(resp);
  return winboloNetRunning;
}

/*********************************************************
*NAME:          winbolonetDestroy
*PURPOSE:
* Destroys the winbolonet module.
* Cleans up any open libraries.
*********************************************************/
void winbolonetDestroy(bool isServer) {
  serverSimConsoleMessage("WinBolo.net Shutdown");
  if (winboloNetRunning == TRUE) {
    winbolonetThreadDestroy();
    if (isServer == TRUE && winboloNetServerKey[0] != '\0') {
      winbolonetGoodbye();
    }
    httpDestroy();
  }
  winbolonetEventsDestroy();
  winboloNetRunning = FALSE;
}

/*********************************************************
*NAME:          winbolonetGoodbye
*PURPOSE:
* Sends final update and server quit to WinBolo.net via
* POST /api/v1/server/quit.
*********************************************************/
void winbolonetGoodbye(void) {
  cJSON *body = NULL;
  cJSON *resp = NULL;

  /* Send off final data */
  winbolonetServerUpdate(0, 0, 0, TRUE);

  /* Shutdown */
  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);

  wbn_api_call("server/quit", body, &resp);
  cJSON_Delete(body);
  if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      fprintf(stderr, "WinBolo.net quit error: %s\n", errObj->valuestring);
    }
    cJSON_Delete(resp);
  }
}

/*********************************************************
*NAME:          winbolonetServerSendTeams
*PURPOSE:
* Sends the list of teams via POST /api/v1/server/teams.
*
* Converts the flat BYTE array (with WINBOLO_NET_TEAM_MARKER
* separators) into the JSON teams object format:
* {"server_key": "...", "teams": {"0": ["pk1", "pk2"], ...}}
*********************************************************/
void winbolonetServerSendTeams(BYTE *array, BYTE length, BYTE numTeams) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  cJSON *teams = NULL;
  cJSON *currentTeam = NULL;
  BYTE arrayPos;
  int teamIndex = 0;
  char teamId[16];

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);

  teams = cJSON_CreateObject();
  snprintf(teamId, sizeof(teamId), "%d", teamIndex);
  currentTeam = cJSON_CreateArray();

  arrayPos = 1;
  while (arrayPos <= length) {
    if (array[arrayPos] == WINBOLO_NET_TEAM_MARKER) {
      /* End current team, start a new one */
      cJSON_AddItemToObject(teams, teamId, currentTeam);
      teamIndex++;
      snprintf(teamId, sizeof(teamId), "%d", teamIndex);
      currentTeam = cJSON_CreateArray();
    } else {
      /* Team member */
      if (winboloNetPlayerKey[array[arrayPos]][0] != '\0') {
        cJSON_AddItemToArray(currentTeam, cJSON_CreateString(winboloNetPlayerKey[array[arrayPos]]));
      } else {
        cJSON_AddItemToArray(currentTeam, cJSON_CreateNull());
      }
    }
    arrayPos++;
  }
  /* Add final team */
  cJSON_AddItemToObject(teams, teamId, currentTeam);

  cJSON_AddItemToObject(body, "teams", teams);

  wbn_api_call("server/teams", body, &resp);
  cJSON_Delete(body);
  cJSON_Delete(resp);
}

/*********************************************************
*NAME:          winbolonetServerUpdate
*PURPOSE:
* Sends a server update via POST /api/v1/server/update
* with current state and queued events.
*********************************************************/
void winbolonetServerUpdate(BYTE numPlayers, BYTE numFreeBases, BYTE numFreePills, bool sendNow) {
  static BYTE staticNumPlayers = 0;
  static BYTE staticNumFreeBases = 0;
  static BYTE staticNumFreePills = 0;
  int size;
  cJSON *body = NULL;
  cJSON *events = NULL;
  cJSON *eventObj = NULL;
  cJSON *resp = NULL;
  char keyA[WINBOLONET_KEY_LEN];
  char keyB[WINBOLONET_KEY_LEN];
  BYTE val;

  if (winboloNetRunning != TRUE) {
    return;
  }

  size = winbolonetEventsGetSize();
  if (size == 0 && staticNumFreePills == numFreePills && numFreeBases == staticNumFreeBases && numPlayers == staticNumPlayers && time(NULL) - winboloNetLastSent <= WINBOLO_NET_MAX_NOSEND) {
    return;
  }

  staticNumFreePills = numFreePills;
  staticNumFreeBases = numFreeBases;
  staticNumPlayers = numPlayers;

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddNumberToObject(body, "num_players", staticNumPlayers);
  cJSON_AddNumberToObject(body, "free_bases", staticNumFreeBases);
  cJSON_AddNumberToObject(body, "free_pills", staticNumFreePills);

  /* Drain event queue into JSON array */
  events = cJSON_CreateArray();
  val = winbolonetEventsRemove(keyA, keyB);
  while (val != WINBOLONET_EVENT_NOITEM) {
    eventObj = cJSON_CreateObject();
    cJSON_AddNumberToObject(eventObj, "type", val);
    if (keyA[0] != '\0') {
      cJSON_AddStringToObject(eventObj, "player_a", keyA);
    }
    if (keyB[0] != '\0') {
      cJSON_AddStringToObject(eventObj, "player_b", keyB);
    }
    cJSON_AddItemToArray(events, eventObj);
    val = winbolonetEventsRemove(keyA, keyB);
  }
  cJSON_AddItemToObject(body, "events", events);

  if (sendNow == FALSE) {
    /* Queue for background thread */
    char *json_str = cJSON_PrintUnformatted(body);
    if (json_str) {
      winbolonetThreadAddRequest("server/update", json_str);
      free(json_str);
    }
    cJSON_Delete(body);
  } else {
    /* Send immediately */
    wbn_api_call("server/update", body, &resp);
    cJSON_Delete(body);
    if (resp) {
      cJSON *errObj = cJSON_GetObjectItem(resp, "error");
      if (errObj && cJSON_IsString(errObj)) {
        fprintf(stderr, "WinBolo.net update error: %s\n", errObj->valuestring);
      }
      cJSON_Delete(resp);
    }
  }

  winboloNetLastSent = time(NULL);
}

/*********************************************************
*NAME:          winboloNetGetServerKey
*PURPOSE:
* Copies the server key into keyBuff.
*********************************************************/
void winboloNetGetServerKey(char *keyBuff) {
  strncpy(keyBuff, winboloNetServerKey, WINBOLONET_KEY_LEN);
}

/*********************************************************
*NAME:          winboloNetGetMyClientKey
*PURPOSE:
* Copies this client's key into keyBuff.
*********************************************************/
void winboloNetGetMyClientKey(char *keyBuff) {
  strncpy(keyBuff, winboloNetPlayerKey[0], WINBOLONET_KEY_LEN);
}

/*********************************************************
*NAME:          winboloNetIsPlayerParticipant
*PURPOSE:
* Returns if this player number is a winbolo.net
* participant or not.
*********************************************************/
bool winboloNetIsPlayerParticipant(BYTE playerNum) {
  if (winboloNetRunning == TRUE && winboloNetPlayerKey[playerNum][0] != '\0') {
    return TRUE;
  }
  return FALSE;
}

/*********************************************************
*NAME:          winboloNetVerifyClientKey
*PURPOSE:
* Verifies a client key via POST /api/v1/client/verify.
* If valid, stores the player key for the given player slot.
*********************************************************/
bool winboloNetVerifyClientKey(const char *playerKey, char *userName, BYTE playerNum) {
  bool returnValue = FALSE;
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;

  if (winboloNetPlayerKey[playerNum][0] != '\0' || winboloNetRunning == FALSE) {
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "player_key", playerKey);
  cJSON_AddStringToObject(body, "player_name", userName);

  status = wbn_api_call("client/verify", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      fprintf(stderr, "WinBolo.net verify error: %s\n", errObj->valuestring);
    } else {
      cJSON *validObj = cJSON_GetObjectItem(resp, "valid");
      if (validObj && cJSON_IsTrue(validObj)) {
        returnValue = TRUE;
        strncpy(winboloNetPlayerKey[playerNum], playerKey, WINBOLONET_KEY_LEN - 1);
        winboloNetPlayerKey[playerNum][WINBOLONET_KEY_LEN - 1] = '\0';
      }
    }
  } else {
    fprintf(stderr, "Error: No response from WinBolo.net\n");
  }

  cJSON_Delete(resp);
  return returnValue;
}

/*********************************************************
*NAME:          winbolonetServerVerifyToken
*PURPOSE:
* Called by the server when a player joins with a WBN
* token.  POSTs to client/join and stores the resulting
* player_key at the given slot.  Returns TRUE on success.
*********************************************************/
bool winbolonetServerVerifyToken(const char *token, BYTE playerNum, char *errorMsg, bool *hasSteam) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

  if (hasSteam) *hasSteam = FALSE;

  if (winboloNetRunning != TRUE || winboloNetServerKey[0] == '\0') {
    strcpy(errorMsg, "WinBolo.net not running");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "token", token);
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);

  status = wbn_api_call("client/join", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      cJSON *keyObj = cJSON_GetObjectItem(resp, "player_key");
      if (keyObj && cJSON_IsString(keyObj)) {
        strncpy(winboloNetPlayerKey[playerNum], keyObj->valuestring, WINBOLONET_KEY_LEN - 1);
        winboloNetPlayerKey[playerNum][WINBOLONET_KEY_LEN - 1] = '\0';
        ok = TRUE;
        /* Extract has_steam flag */
        if (hasSteam) {
          cJSON *steamObj = cJSON_GetObjectItem(resp, "has_steam");
          if (steamObj && cJSON_IsBool(steamObj)) {
            *hasSteam = cJSON_IsTrue(steamObj) ? TRUE : FALSE;
          }
        }
      } else {
        strcpy(errorMsg, "WinBolo.net returned no player key");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "WinBolo.net verification failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

  cJSON_Delete(resp);
  return ok;
}

/*********************************************************
*NAME:          winboloNetClientLeaveGame
*PURPOSE:
* Called when a player leaves the game. Sends a leave
* event, flushes the update, and calls
* POST /api/v1/client/leave.
*********************************************************/
void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
  cJSON *body = NULL;
  cJSON *resp = NULL;

  winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_LEAVE, TRUE, playerNum, WINBOLO_NET_NO_PLAYER);
  if (winboloNetPlayerKey[playerNum][0] == '\0' || winboloNetRunning != TRUE) {
    return;
  }

  /* Flush buffered events */
  winbolonetServerUpdate(numPlayers, freeBases, freePills, TRUE);

  /* Send leave */
  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "player_key", winboloNetPlayerKey[playerNum]);
  cJSON_AddNumberToObject(body, "num_players", numPlayers);
  cJSON_AddNumberToObject(body, "free_bases", freeBases);
  cJSON_AddNumberToObject(body, "free_pills", freePills);

  wbn_api_call("client/leave", body, &resp);
  cJSON_Delete(body);
  if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      fprintf(stderr, "WinBolo.net leave error: %s\n", errObj->valuestring);
    }
    cJSON_Delete(resp);
  }

  winboloNetPlayerKey[playerNum][0] = '\0';
}

/*********************************************************
*NAME:          winbolonetAddEvent
*PURPOSE:
* Adds a WinBolo.net Event for sending to the server.
*********************************************************/
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB) {
  const char *keyA;
  const char *keyB;
  char emptyKey[WINBOLONET_KEY_LEN];

  if (winboloNetRunning == TRUE && isServer == TRUE) {
    emptyKey[0] = '\0';
    keyA = winboloNetPlayerKey[playerA];
    if (playerB == WINBOLO_NET_NO_PLAYER) {
      keyB = emptyKey;
    } else {
      keyB = winboloNetPlayerKey[playerB];
    }
    winbolonetEventsAddItem(eventType, keyA, keyB);
  }
}

/*********************************************************
*NAME:          winbolonetIsRunning
*PURPOSE:
* Returns if the winbolonet module is running or not.
*********************************************************/
bool winbolonetIsRunning(void) {
  return winboloNetRunning;
}

/*********************************************************
*NAME:          winboloNetSendLock
*PURPOSE:
* Sends lock/unlock status via POST /api/v1/server/lock.
*********************************************************/
void winboloNetSendLock(bool isLocked) {
  cJSON *body = NULL;
  cJSON *resp = NULL;

  if (winboloNetRunning != TRUE) {
    return;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddBoolToObject(body, "locked", isLocked);

  wbn_api_call("server/lock", body, &resp);
  cJSON_Delete(body);
  cJSON_Delete(resp);
}

/*********************************************************
*NAME:          winbolonetAuthLogin
*PURPOSE:
* Authenticates via POST /api/v1/auth/login and returns
* the token and expiry on success.
*********************************************************/
bool winbolonetAuthLogin(const char *username, const char *password, char *tokenOut, char *expiryOut, char *playerNameOut, char *errorMsg) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

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
        cJSON *nameObj = cJSON_GetObjectItem(resp, "player_name");
        if (nameObj && cJSON_IsString(nameObj)) {
          strncpy(playerNameOut, nameObj->valuestring, PLAYER_NAME_LEN - 1);
          playerNameOut[PLAYER_NAME_LEN - 1] = '\0';
        }
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
  httpDestroy();
  return ok;
}

/*********************************************************
*NAME:          winbolonetAuthValidate
*PURPOSE:
* Validates a token via POST /api/v1/auth/validate.
* Returns TRUE if the token is still valid.
*********************************************************/
bool winbolonetAuthValidate(const char *token, char *playerNameOut, char *errorMsg) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

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
        cJSON *nameObj = cJSON_GetObjectItem(resp, "player_name");
        if (nameObj && cJSON_IsString(nameObj)) {
          strncpy(playerNameOut, nameObj->valuestring, PLAYER_NAME_LEN - 1);
          playerNameOut[PLAYER_NAME_LEN - 1] = '\0';
        }
      }
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
  httpDestroy();
  return ok;
}
