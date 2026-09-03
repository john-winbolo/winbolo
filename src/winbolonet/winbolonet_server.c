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
*Name:          WinBolo.net Server
*Filename:      winbolonet_server.c
*Author:        John Morrison
*Creation Date: 23/09/01
*Last Modified: 30/03/26
*Purpose:
*  Server-side WinBolo.net tracker calls: register, per-tick
*  update, lobby/map/teams/balance, and client verify/leave.
*********************************************************/

#include <time.h>
#include <stdlib.h>
#include "cJSON.h"
#include "winbolonet_core.h"
#include "winbolonet_server.h"
#include "http.h"
#include "wbn_bearer.h"
#include "server_sim.h"
#include "winbolonetevents.h"
#include "winbolonetthread.h"

#define WINBOLO_NET_MAX_NOSEND 60 /* Maximum non transmission time in seconds */
#define WINBOLO_NET_TEAM_MARKER (254)

/* Storage owned by winbolonet_core */
extern bool winboloNetRunning;
extern char winboloNetServerKey[WINBOLONET_KEY_LEN];
extern char winboloNetPlayerKey[MAX_TANKS][WINBOLONET_KEY_LEN];

static time_t winboloNetLastSent;

/* Current lobby/server state, stashed by winbolonetSetLobbyInfo and
 * read by the register/beginSession/update builders and
 * winbolonetSendLobbyUpdate. Zero-initialised until the server sets
 * it. */
static WbnLobbyInfo s_lobbyInfo;

void winbolonetSetLobbyInfo(const WbnLobbyInfo *info) {
  if (info != NULL) {
    s_lobbyInfo = *info;
  }
}

/* Adds the extended lobby/setting fields shared by register,
 * beginSession and lobby_update to a cJSON body from the stashed
 * lobby info. Counts are sent both split (num_humans/num_bots) and
 * summed (num_players) so existing consumers keep working. */
static void winbolonetAddLobbyInfoFields(cJSON *body) {
  cJSON_AddStringToObject(body, "map_md5", s_lobbyInfo.mapMd5);
  cJSON_AddBoolToObject(body, "random_map", s_lobbyInfo.randomMap);
  cJSON_AddBoolToObject(body, "ranked", s_lobbyInfo.ranked);
  cJSON_AddBoolToObject(body, "allow_new_players", s_lobbyInfo.allowNewPlayers);
  cJSON_AddBoolToObject(body, "allow_spectators", s_lobbyInfo.allowSpectators);
  cJSON_AddBoolToObject(body, "auto_lock", s_lobbyInfo.autoLock);
  cJSON_AddBoolToObject(body, "has_lobby", s_lobbyInfo.hasLobby);
  cJSON_AddBoolToObject(body, "time_limit", s_lobbyInfo.timeLimit);
  cJSON_AddNumberToObject(body, "time_minutes", s_lobbyInfo.timeMinutes);
  cJSON_AddNumberToObject(body, "lobby_locks", s_lobbyInfo.lobbyLocks);
  cJSON_AddNumberToObject(body, "num_humans", s_lobbyInfo.numHumans);
  cJSON_AddNumberToObject(body, "num_bots", s_lobbyInfo.numBots);
  cJSON_AddNumberToObject(body, "pillview", s_lobbyInfo.pillView);
  cJSON_AddNumberToObject(body, "baseview", s_lobbyInfo.baseView);
  cJSON_AddNumberToObject(body, "allyview", s_lobbyInfo.allyView);
}

void winbolonetSendLobbyUpdate(void) {
  cJSON *body = NULL;
  char *json_str;

  if (winboloNetRunning != TRUE) {
    return;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "map", s_lobbyInfo.map);
  cJSON_AddNumberToObject(body, "num_bases", s_lobbyInfo.numBases);
  cJSON_AddNumberToObject(body, "num_pills", s_lobbyInfo.numPills);
  cJSON_AddNumberToObject(body, "free_bases", s_lobbyInfo.freeBases);
  cJSON_AddNumberToObject(body, "free_pills", s_lobbyInfo.freePills);
  cJSON_AddNumberToObject(body, "game_type", s_lobbyInfo.gameType);
  cJSON_AddNumberToObject(body, "ai", s_lobbyInfo.ai);
  cJSON_AddBoolToObject(body, "mines", s_lobbyInfo.mines);
  cJSON_AddNumberToObject(body, "num_players",
                          s_lobbyInfo.numHumans + s_lobbyInfo.numBots);
  winbolonetAddLobbyInfoFields(body);

  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    winbolonetThreadAddServerRequest("server/lobby_update", json_str);
    free(json_str);
  }
  cJSON_Delete(body);
}

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
  cJSON_AddBoolToObject(body, "in_lobby", TRUE);
  winbolonetAddLobbyInfoFields(body);

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
      {
        cJSON *tokenObj = cJSON_GetObjectItem(resp, "server_token");
        if (tokenObj && cJSON_IsString(tokenObj)) {
          httpSetServerBearerToken(tokenObj->valuestring);
        } else {
          /* Register response missing server_token — leave bearer
           * unset; subsequent server/ calls will refuse-to-send. */
          httpSetServerBearerToken(NULL);
          fprintf(stderr, "WinBolo.net register response missing server_token\n");
        }
      }
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

  wbn_api_call_server("server/teams", body, &resp);
  cJSON_Delete(body);
  cJSON_Delete(resp);
}

/*********************************************************
*NAME:          winbolonetServerRequestBalance
*PURPOSE:
* Calls the WBN API to get skill-based team assignments
* for the current lobby players.
* Returns TRUE on success, FALSE on failure.
*********************************************************/
bool winbolonetServerRequestBalance(uint8_t totalPlayers, uint8_t teamSize,
                                     const uint8_t *botSlots, uint8_t numBotSlots,
                                     BalanceProposal *outProposal) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  cJSON *playerKeys = NULL;
  cJSON *teams = NULL;
  cJSON *teamArray = NULL;
  cJSON *entry = NULL;
  int status;
  BYTE count;
  int teamIdx;
  char teamId[16];
  char botKey[16];

  memset(outProposal, 0, sizeof(BalanceProposal));

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddNumberToObject(body, "total_players", totalPlayers);
  cJSON_AddNumberToObject(body, "team_size", teamSize);

  /* Player keys array carries one entry per slot we want WBN to
   * place. Human keys are real WBN player_key strings; bot slots
   * are sent as "bot:<slot>" sentinels that WBN treats as
   * non-WBN players when computing skill-balanced teams. The slot
   * number is embedded so the response parser can map the entry
   * back to a slot without a separate index. */
  playerKeys = cJSON_CreateArray();
  for (count = 0; count < MAX_TANKS; count++) {
    if (winboloNetPlayerKey[count][0] != '\0') {
      cJSON_AddItemToArray(playerKeys, cJSON_CreateString(winboloNetPlayerKey[count]));
    }
  }
  for (BYTE bi = 0; bi < numBotSlots; bi++) {
    BYTE slot = botSlots[bi];
    snprintf(botKey, sizeof(botKey), "bot:%u", (unsigned)slot);
    cJSON_AddItemToArray(playerKeys, cJSON_CreateString(botKey));
  }
  cJSON_AddItemToObject(body, "player_keys", playerKeys);

  status = wbn_api_call_server("server/balance", body, &resp);
  cJSON_Delete(body);

  if (status != 200 || resp == NULL) {
    serverSimConsoleMessage("WBN: Balance request failed");
    cJSON_Delete(resp);
    return FALSE;
  }

  /* Check for error in response */
  {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      fprintf(stderr, "WinBolo.net balance error: %s\n", errObj->valuestring);
      serverSimConsoleMessage("WBN: Balance error from server");
      cJSON_Delete(resp);
      return FALSE;
    }
  }

  /* Parse response `teams`. WBN returns this as a JSON array of
   * per-team arrays, e.g. teams:[[k1,k2],[k3,null]]. teamIdx is the
   * outer array position; entries are player_key strings or null
   * placeholders for unregistered slots. */
  teams = cJSON_GetObjectItemCaseSensitive(resp, "teams");
  if (teams == NULL || !cJSON_IsArray(teams)) {
    serverSimConsoleMessage("WBN: Balance response missing teams");
    cJSON_Delete(resp);
    return FALSE;
  }
  (void)teamId; /* legacy buffer kept for ABI; unused under array layout */

  for (teamIdx = 0; ; teamIdx++) {
    teamArray = cJSON_GetArrayItem(teams, teamIdx);
    if (teamArray == NULL) {
      break;
    }
    if (!cJSON_IsArray(teamArray)) {
      continue;
    }

    cJSON_ArrayForEach(entry, teamArray) {
      if (cJSON_IsNull(entry)) {
        continue; /* Unregistered player placeholder */
      }
      if (!cJSON_IsString(entry) || entry->valuestring == NULL) {
        continue;
      }
      /* Map player_key back to slot index. Bot sentinels carry the
       * slot in the key itself ("bot:N") so they round-trip without
       * needing the WBN-key lookup table. */
      if (strncmp(entry->valuestring, "bot:", 4) == 0) {
        int botSlot = atoi(entry->valuestring + 4);
        if (botSlot >= 0 && botSlot < MAX_TANKS) {
          if (teamIdx + 1 >= MAX_TANKS) continue;
          outProposal->teamForSlot[botSlot] = (uint8_t)(teamIdx + 1);
        }
        continue;
      }
      for (count = 0; count < MAX_TANKS; count++) {
        if (winboloNetPlayerKey[count][0] != '\0' &&
            strcmp(winboloNetPlayerKey[count], entry->valuestring) == 0) {
          if (teamIdx + 1 >= MAX_TANKS) continue;
          outProposal->teamForSlot[count] = (uint8_t)(teamIdx + 1); /* 1-indexed */
          break;
        }
      }
    }
  }

  outProposal->pending = TRUE;
  cJSON_Delete(resp);
  return TRUE;
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
  static BYTE staticNumHumans = 0;
  static BYTE staticNumBots = 0;
  int size;
  cJSON *body = NULL;
  cJSON *events = NULL;
  cJSON *eventObj = NULL;
  cJSON *resp = NULL;
  char keyA[WINBOLONET_KEY_LEN];
  char keyB[WINBOLONET_KEY_LEN];
  bool evtAIsBot;
  bool evtBIsBot;
  BYTE val;

  if (winboloNetRunning != TRUE) {
    return;
  }

  size = winbolonetEventsGetSize();
  if (size == 0 && staticNumFreePills == numFreePills && numFreeBases == staticNumFreeBases && numPlayers == staticNumPlayers && staticNumHumans == s_lobbyInfo.numHumans && staticNumBots == s_lobbyInfo.numBots && time(NULL) - winboloNetLastSent <= WINBOLO_NET_MAX_NOSEND) {
    return;
  }

  staticNumFreePills = numFreePills;
  staticNumFreeBases = numFreeBases;
  staticNumPlayers = numPlayers;
  staticNumHumans = s_lobbyInfo.numHumans;
  staticNumBots = s_lobbyInfo.numBots;

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddNumberToObject(body, "num_players", staticNumPlayers);
  cJSON_AddNumberToObject(body, "num_humans", s_lobbyInfo.numHumans);
  cJSON_AddNumberToObject(body, "num_bots", s_lobbyInfo.numBots);
  cJSON_AddNumberToObject(body, "free_bases", staticNumFreeBases);
  cJSON_AddNumberToObject(body, "free_pills", staticNumFreePills);

  /* Drain event queue into JSON array. Bot actors carry no WBN key,
   * so a_is_bot/b_is_bot are emitted (true only) to distinguish AI
   * actions from unregistered humans. */
  events = cJSON_CreateArray();
  val = winbolonetEventsRemove(keyA, keyB, &evtAIsBot, &evtBIsBot);
  while (val != WINBOLONET_EVENT_NOITEM) {
    eventObj = cJSON_CreateObject();
    cJSON_AddNumberToObject(eventObj, "type", val);
    if (keyA[0] != '\0') {
      cJSON_AddStringToObject(eventObj, "player_a", keyA);
    }
    if (keyB[0] != '\0') {
      cJSON_AddStringToObject(eventObj, "player_b", keyB);
    }
    if (evtAIsBot) {
      cJSON_AddBoolToObject(eventObj, "a_is_bot", TRUE);
    }
    if (evtBIsBot) {
      cJSON_AddBoolToObject(eventObj, "b_is_bot", TRUE);
    }
    cJSON_AddItemToArray(events, eventObj);
    val = winbolonetEventsRemove(keyA, keyB, &evtAIsBot, &evtBIsBot);
  }
  cJSON_AddItemToObject(body, "events", events);

  if (sendNow == FALSE) {
    /* Queue for background thread (bearer attached at fire time) */
    char *json_str = cJSON_PrintUnformatted(body);
    if (json_str) {
      winbolonetThreadAddServerRequest("server/update", json_str);
      free(json_str);
    }
    cJSON_Delete(body);
  } else {
    /* Send immediately */
    wbn_api_call_server("server/update", body, &resp);
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
* Validates a player_key received off the wire via
* POST /api/v1/client/verify. On success, copies the
* player_key into the player's slot in winboloNetPlayerKey
* so subsequent events/leaves can identify the player.
*********************************************************/
bool winboloNetVerifyClientKey(const char *playerKey, const char *playerName, BYTE playerNum, char *errorMsg, bool *hasSteam, bool *isSupporter) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

  if (hasSteam) *hasSteam = FALSE;
  if (isSupporter) *isSupporter = FALSE;

  if (winboloNetRunning != TRUE || winboloNetServerKey[0] == '\0') {
    strcpy(errorMsg, "WinBolo.net not running");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "player_key", playerKey);
  cJSON_AddStringToObject(body, "player_name", playerName);

  status = wbn_api_call("client/verify", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strncpy(winboloNetPlayerKey[playerNum], playerKey, WINBOLONET_KEY_LEN - 1);
      winboloNetPlayerKey[playerNum][WINBOLONET_KEY_LEN - 1] = '\0';
      ok = TRUE;
      if (hasSteam) {
        cJSON *steamObj = cJSON_GetObjectItem(resp, "has_steam");
        if (steamObj && cJSON_IsBool(steamObj)) {
          *hasSteam = cJSON_IsTrue(steamObj) ? TRUE : FALSE;
        }
      }
      if (isSupporter) {
        /* TODO: enable once WBN /client/verify returns "supporter" field
        cJSON *supObj = cJSON_GetObjectItem(resp, "supporter");
        if (supObj && cJSON_IsBool(supObj))
            *isSupporter = cJSON_IsTrue(supObj) ? TRUE : FALSE;
        */
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
*NAME:          winboloNetVerifyJoinCode
*PURPOSE:
* Resolves a join_code via POST
* /api/v1/client/verify_join_code. Read-only: it does not
* consume the code and stores nothing in the slot-keyed
* winboloNetPlayerKey[] array. On acceptance it reports the
* resolved player name, login state, country and user id.
*********************************************************/
bool winboloNetVerifyJoinCode(const char *joinCode,
                              char  *playerNameOut,   /* >= PACKET_MAX_PLAYER_NAME */
                              bool  *isLoggedInOut,
                              char  *countryOut,      /* >= 3 (ISO-2 + NUL) */
                              int   *userIdOut,       /* -1 when null */
                              char  *errorMsg) {      /* >= 256 */
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

  if (isLoggedInOut) *isLoggedInOut = FALSE;
  if (userIdOut) *userIdOut = -1;
  if (playerNameOut) playerNameOut[0] = '\0';
  if (countryOut) countryOut[0] = '\0';

  if (winboloNetRunning != TRUE || winboloNetServerKey[0] == '\0') {
    strcpy(errorMsg, "WinBolo.net not running");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "join_code", joinCode);

  status = wbn_api_call("client/verify_join_code", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    /* "ok" gates acceptance; treat its absence as success so a lean
     * backend response still verifies, mirroring the spectator verify. */
    cJSON *okObj = cJSON_GetObjectItem(resp, "ok");
    bool accepted = (okObj == NULL) || cJSON_IsTrue(okObj);
    if (accepted) {
      cJSON *liObj = cJSON_GetObjectItem(resp, "is_logged_in");
      cJSON *nameObj = cJSON_GetObjectItem(resp, "player_name");
      cJSON *ccObj = cJSON_GetObjectItem(resp, "country_code");
      cJSON *uidObj = cJSON_GetObjectItem(resp, "user_id");

      if (isLoggedInOut && liObj && cJSON_IsBool(liObj)) {
        *isLoggedInOut = cJSON_IsTrue(liObj) ? TRUE : FALSE;
      }
      if (playerNameOut && nameObj && cJSON_IsString(nameObj)) {
        strncpy(playerNameOut, nameObj->valuestring, PACKET_MAX_PLAYER_NAME - 1);
        playerNameOut[PACKET_MAX_PLAYER_NAME - 1] = '\0';
      }
      if (countryOut && ccObj && cJSON_IsString(ccObj)) {
        strncpy(countryOut, ccObj->valuestring, 2);
        countryOut[2] = '\0';
      }
      if (userIdOut && uidObj && cJSON_IsNumber(uidObj)) {
        *userIdOut = uidObj->valueint;
      }
      ok = TRUE;
    } else {
      cJSON *errObj = cJSON_GetObjectItem(resp, "error");
      if (errObj && cJSON_IsString(errObj)) {
        /* Backend-controlled string into a fixed (>=256) buffer: bound it. */
        strncpy(errorMsg, errObj->valuestring, 255);
        errorMsg[255] = '\0';
      } else {
        strcpy(errorMsg, "WinBolo.net join code verification rejected");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "WinBolo.net join code verification failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

  cJSON_Delete(resp);
  return ok;
}

/*********************************************************
*NAME:          winboloNetVerifySpectatorKey
*PURPOSE:
* Validates a spectator_key via POST
* /api/v1/client/verify_spectator. Unlike the player verify
* it stores nothing in the slot-keyed winboloNetPlayerKey[]
* array — spectators are not slot-indexed and the caller
* keeps the key on the spectator connection itself.
*********************************************************/
bool winboloNetVerifySpectatorKey(const char *spectatorKey, const char *playerName, char *errorMsg, bool *isLoggedIn) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

  if (isLoggedIn) *isLoggedIn = FALSE;

  if (winboloNetRunning != TRUE || winboloNetServerKey[0] == '\0') {
    strcpy(errorMsg, "WinBolo.net not running");
    return FALSE;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "player_key", spectatorKey);
  cJSON_AddStringToObject(body, "player_name", playerName);

  status = wbn_api_call("client/verify_spectator", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      /* "ok" gates acceptance; treat its absence as success so a
       * lean backend response still verifies, mirroring the player
       * verify which accepts a 200 with no error. */
      cJSON *okObj = cJSON_GetObjectItem(resp, "ok");
      bool accepted = (okObj == NULL) || cJSON_IsTrue(okObj);
      if (accepted) {
        ok = TRUE;
        if (isLoggedIn) {
          cJSON *liObj = cJSON_GetObjectItem(resp, "is_logged_in");
          if (liObj && cJSON_IsBool(liObj)) {
            *isLoggedIn = cJSON_IsTrue(liObj) ? TRUE : FALSE;
          }
        }
      } else {
        strcpy(errorMsg, "WinBolo.net spectator verification rejected");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      strcpy(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "WinBolo.net spectator verification failed");
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

  winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_LEAVE, TRUE, playerNum, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
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

  wbn_api_call_server("client/leave", body, &resp);
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
*NAME:          winboloNetSpectatorLeaveGame
*PURPOSE:
* Releases a spectator's WBN session via POST
* /api/v1/client/leave, keyed only by its spectator_key.
* Spectators hold no slot-keyed events to flush, so this is
* a bare leave with no player-count payload.
*********************************************************/
void winboloNetSpectatorLeaveGame(const char *spectatorKey) {
  cJSON *body = NULL;
  cJSON *resp = NULL;

  if (spectatorKey == NULL || spectatorKey[0] == '\0' || winboloNetRunning != TRUE) {
    return;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "player_key", spectatorKey);

  wbn_api_call_server("client/leave", body, &resp);
  cJSON_Delete(body);
  if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      fprintf(stderr, "WinBolo.net spectator leave error: %s\n", errObj->valuestring);
    }
    cJSON_Delete(resp);
  }
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

  wbn_api_call_server("server/lock", body, &resp);
  cJSON_Delete(body);
  cJSON_Delete(resp);
}

/*********************************************************
*NAME:          winbolonetEndSession
*PURPOSE:
* Ends the current WBN session: drains the background
* thread, POSTs server/quit, clears the bearer + per-slot
* player keys, and resets the event queue. The HTTP layer
* stays alive so a subsequent winbolonetBeginSession can
* re-register (and so the per-round log uploader can fire
* httpSendLogFile in between, against the still-valid
* winboloNetServerKey — WBN rejects uploads to an active
* session, so the upload has to follow the server/quit
* POST but precede server/register's key swap).
*********************************************************/
void winbolonetEndSession(void) {
  BYTE count;
  cJSON *body = NULL;
  cJSON *resp = NULL;

  if (winboloNetRunning != TRUE) {
    return;
  }

  serverSimConsoleMessage("WinBolo.net: Ending session...");

  winbolonetThreadDestroy();

  /* server/quit carries the still-valid bearer for this POST. */
  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  wbn_api_call_server("server/quit", body, &resp);
  cJSON_Delete(body);
  if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      fprintf(stderr, "WinBolo.net quit error: %s\n", errObj->valuestring);
    }
    cJSON_Delete(resp);
  }

  /* Old session is over — the old bearer is now invalid. Clear before
   * the next BeginSession issues a fresh pair. httpSendLogFile is
   * permissive about missing bearer, so the round-end log upload that
   * runs between End and Begin still works against just the URL key. */
  httpClearServerBearerToken();

  for (count = 0; count < MAX_TANKS; count++) {
    winboloNetPlayerKey[count][0] = '\0';
  }

  winbolonetEventsDestroy();
  winbolonetEventsCreate();
}

/*********************************************************
*NAME:          winbolonetBeginSession
*PURPOSE:
* Registers a fresh WBN session for the next round and
* restarts the background thread. Pairs with
* winbolonetEndSession at round boundaries.
*********************************************************/
bool winbolonetBeginSession(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  char versionStr[16];

  if (winboloNetRunning != TRUE) {
    return FALSE;
  }

  snprintf(versionStr, sizeof(versionStr), "%d.%d%d", BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION);

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
  cJSON_AddBoolToObject(body, "in_lobby", TRUE);
  winbolonetAddLobbyInfoFields(body);

  status = wbn_api_call("server/register", body, &resp);
  cJSON_Delete(body);

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    cJSON *keyObj = cJSON_GetObjectItem(resp, "server_key");
    if (errObj && cJSON_IsString(errObj)) {
      fprintf(stderr, "WinBolo.net register error: %s\n", errObj->valuestring);
      serverSimConsoleMessage("Error: WinBolo.net re-registration failed");
      cJSON_Delete(resp);
      winboloNetRunning = FALSE;
      return FALSE;
    }
    if (keyObj && cJSON_IsString(keyObj)) {
      strncpy(winboloNetServerKey, keyObj->valuestring, WINBOLONET_KEY_LEN - 1);
      winboloNetServerKey[WINBOLONET_KEY_LEN - 1] = '\0';
      {
        cJSON *tokenObj = cJSON_GetObjectItem(resp, "server_token");
        if (tokenObj && cJSON_IsString(tokenObj)) {
          httpSetServerBearerToken(tokenObj->valuestring);
        } else {
          httpSetServerBearerToken(NULL);
          fprintf(stderr, "WinBolo.net register response missing server_token\n");
        }
      }
      serverSimConsoleMessage("\tWinBolo.net: New session registered");
    } else {
      serverSimConsoleMessage("Error: WinBolo.net returned no server key");
      cJSON_Delete(resp);
      winboloNetRunning = FALSE;
      return FALSE;
    }
  } else {
    if (resp) {
      cJSON *errObj = cJSON_GetObjectItem(resp, "error");
      if (errObj && cJSON_IsString(errObj)) {
        fprintf(stderr, "WinBolo.net error: %s\n", errObj->valuestring);
      }
    }
    serverSimConsoleMessage("Error: WinBolo.net re-registration failed - WBN disabled");
    cJSON_Delete(resp);
    winboloNetRunning = FALSE;
    return FALSE;
  }

  cJSON_Delete(resp);

  winbolonetThreadCreate();
  winboloNetLastSent = time(NULL);

  return TRUE;
}

/*********************************************************
*NAME:          winbolonetSendLobbyStatus
*PURPOSE:
* Notifies WinBolo.net whether this server is currently in
* the lobby or in-game. POSTs to /api/v1/server/lobby.
* Queued via background thread (fire-and-forget).
*********************************************************/
void winbolonetSendLobbyStatus(bool inLobby) {
  cJSON *body = NULL;
  char *json_str;

  if (winboloNetRunning != TRUE) {
    return;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddBoolToObject(body, "in_lobby", inLobby);

  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    winbolonetThreadAddServerRequest("server/lobby", json_str);
    free(json_str);
  }
  cJSON_Delete(body);
}

/*********************************************************
*NAME:          winbolonetSendMapChange
*PURPOSE:
* Notifies WinBolo.net that the map changed during the
* lobby (e.g. via skip vote). POSTs to /api/v1/server/map.
* Queued via background thread (fire-and-forget).
*********************************************************/
void winbolonetSendMapChange(char *mapName, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills) {
  cJSON *body = NULL;
  char *json_str;

  if (winboloNetRunning != TRUE) {
    return;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "map", mapName);
  cJSON_AddNumberToObject(body, "num_bases", numBases);
  cJSON_AddNumberToObject(body, "num_pills", numPills);
  cJSON_AddNumberToObject(body, "free_bases", freeBases);
  cJSON_AddNumberToObject(body, "free_pills", freePills);

  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    winbolonetThreadAddServerRequest("server/map", json_str);
    free(json_str);
  }
  cJSON_Delete(body);
}

