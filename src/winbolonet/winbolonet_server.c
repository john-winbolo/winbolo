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
  cJSON_AddBoolToObject(body, "classicmode", s_lobbyInfo.classicMode);
  cJSON_AddBoolToObject(body, "alliesintrees", s_lobbyInfo.alliesInTrees);
  /* Absent reads as false, which is what every server did before the key. */
  cJSON_AddBoolToObject(body, "positionalsound", s_lobbyInfo.positionalSound);
  cJSON_AddNumberToObject(body, "pillviewdecay", s_lobbyInfo.pillViewDecay);
  cJSON_AddNumberToObject(body, "baseviewdecay", s_lobbyInfo.baseViewDecay);
  cJSON_AddNumberToObject(body, "allyviewdecay", s_lobbyInfo.allyViewDecay);
  /* Numbers like the view policies rather than flags: each is a selector
   * that can take a third value. 0 is the expanded overview window and 0
   * is sight off, so a reader that finds neither key reads what every
   * server did before the fields existed. */
  cJSON_AddNumberToObject(body, "overviewwindow", s_lobbyInfo.overviewWindow);
  cJSON_AddNumberToObject(body, "lineofsight", s_lobbyInfo.lineOfSight);
  /* A flag, and a negative one: true bans smart pings. A reader that finds
   * no key gets false and so reads "allowed", which is what every server
   * did before the field existed — the same rule the two above follow. */
  cJSON_AddBoolToObject(body, "smartpingsoff", s_lobbyInfo.smartPingsOff);
  /* Sent as a number like the view policies, not a word: 0 on, 1 off,
   * 2 proximity. A reader that finds no "voice" key reads on, which is
   * what servers did before the field existed. */
  cJSON_AddNumberToObject(body, "voice", s_lobbyInfo.voiceMode);
  /* No "scenario" or "scenario_max_players" key is a round no scenario
   * decides: a plain one, or one only mods change. "mods" is always sent,
   * [] when none run, so a reader that finds no key is reading a server
   * from before the field existed. */
  if (s_lobbyInfo.hasScenario) {
    cJSON_AddStringToObject(body, "scenario", s_lobbyInfo.scenarioName);
    cJSON_AddNumberToObject(body, "scenario_max_players",
                            s_lobbyInfo.scenarioMaxPlayers);
  }
  {
    cJSON *mods = cJSON_CreateArray();
    BYTE   i;
    for (i = 0; i < s_lobbyInfo.modCount && i < WBN_MODS_MAX; i++) {
      cJSON_AddItemToArray(mods, cJSON_CreateString(s_lobbyInfo.modNames[i]));
    }
    cJSON_AddItemToObject(body, "mods", mods);
  }
}

void winbolonetSendLobbyUpdate(void) {
  cJSON *body = NULL;
  char *json_str;

  if (winboloNetRunning != TRUE) {
    return;
  }

  /* No server_key here or in the other queued server posts below: the
   * worker stamps the key that is current when the post fires, so one
   * queued across a round transition names the session that is live then. */
  body = cJSON_CreateObject();
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
    winbolonetThreadAddServerKeyedRequest("server/lobby_update", json_str);
    free(json_str);
  }
  cJSON_Delete(body);
}

/*********************************************************
*NAME:          winbolonetBuildRegisterBody
*PURPOSE:
* Builds the JSON body for a server/register POST. Shared
* by winbolonetCreateServer and winbolonetBeginSession,
* which send the same set of fields.
*
*ARGUMENTS:
* As winbolonetCreateServer.
*
*RETURNS:
* The body object. The caller deletes it.
*********************************************************/
static cJSON *winbolonetBuildRegisterBody(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  cJSON *body;
  char versionStr[16];

  /* Build version string from game version defines.
     Note: the hex defines (0x01, 0x08) are for the binary protocol;
     the display version is constructed here as "major.minor.revision". */
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

  return body;
}

/*********************************************************
*NAME:          winbolonetApplyRegisterResponse
*PURPOSE:
* Reads a server/register reply and, on success, stores the
* returned server key and bearer token. Shared by
* winbolonetCreateServer and winbolonetBeginSession, which
* differ only in their console wording and in how they shut
* WinBolo.net down when registration fails.
*
*ARGUMENTS:
* status    - HTTP status returned by wbn_api_call
* resp      - Parsed reply, or NULL
* okMsg     - Console message on success
* errMsg    - Console message when the reply carries an error
* noRespMsg - Console message when there was no usable reply
*
*RETURNS:
* TRUE when the server key was stored. The caller owns resp
* either way.
*********************************************************/
static bool winbolonetApplyRegisterResponse(int status, cJSON *resp, const char *okMsg, const char *errMsg, const char *noRespMsg) {
  cJSON *errObj;
  cJSON *keyObj;
  cJSON *tokenObj;

  if (status != 200 || !resp) {
    if (resp) {
      errObj = cJSON_GetObjectItem(resp, "error");
      if (errObj && cJSON_IsString(errObj)) {
        fprintf(stderr, "WinBolo.net error: %s\n", errObj->valuestring);
      }
    }
    serverSimConsoleMessage(noRespMsg);
    return FALSE;
  }

  errObj = cJSON_GetObjectItem(resp, "error");
  if (errObj && cJSON_IsString(errObj)) {
    fprintf(stderr, "WinBolo.net register error: %s\n", errObj->valuestring);
    serverSimConsoleMessage(errMsg);
    return FALSE;
  }

  keyObj = cJSON_GetObjectItem(resp, "server_key");
  if (!keyObj || !cJSON_IsString(keyObj)) {
    serverSimConsoleMessage("Error: WinBolo.net returned no server key");
    return FALSE;
  }

  /* Through the worker's setter: a queued keyed post reads the key on the
   * worker when it fires, and this write can land while one is being read. */
  winbolonetThreadSetServerKey(keyObj->valuestring);

  tokenObj = cJSON_GetObjectItem(resp, "server_token");
  if (tokenObj && cJSON_IsString(tokenObj)) {
    httpSetServerBearerToken(tokenObj->valuestring);
  } else {
    /* Register response missing server_token — leave bearer
     * unset; subsequent server/ calls will refuse-to-send. */
    httpSetServerBearerToken(NULL);
    fprintf(stderr, "WinBolo.net register response missing server_token\n");
  }

  serverSimConsoleMessage(okMsg);
  return TRUE;
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

  serverSimConsoleMessage("WinBolo.net Startup");
  winboloNetRunning = FALSE;
  winbolonetEventsCreate();
  winbolonetThreadSetServerKey(NULL);
  for (count = 0; count < MAX_TANKS; count++) {
    winboloNetPlayerKey[count][0] = '\0';
  }

  winboloNetRunning = httpCreate();
  if (winboloNetRunning != TRUE) {
    return FALSE;
  }

  /* Register server with WinBolo.net */
  body = winbolonetBuildRegisterBody(mapName, port, gameType, ai, mines, password, numBases, numPills, freeBases, freePills, numPlayers);
  status = wbn_api_call("server/register", body, &resp);
  cJSON_Delete(body);

  if (!winbolonetApplyRegisterResponse(status, resp,
                                       "\tWinBolo.net: Server registered",
                                       "Error: WinBolo.net registration failed",
                                       "Error: No response from WinBolo.net - WinBolo.net disabled")) {
    cJSON_Delete(resp);
    winbolonetDestroy(TRUE);
    return winboloNetRunning;
  }

  winbolonetThreadCreate();
  winboloNetLastSent = time(NULL);

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
  char *json_str = NULL;
  cJSON *teams = NULL;
  cJSON *currentTeam = NULL;
  BYTE arrayPos;
  int teamIndex = 0;
  char teamId[16];

  body = cJSON_CreateObject();

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

  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    winbolonetThreadAddServerKeyedRequest("server/teams", json_str);
    free(json_str);
  }
  cJSON_Delete(body);
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
      winbolonetThreadAddServerKeyedRequest("server/update", json_str);
      free(json_str);
    }
    cJSON_Delete(body);
  } else {
    /* A flush: queued like the periodic update so the caller's thread does
       not wait on the post. It goes in through
       winbolonetThreadAddSessionRequest, which the backlog cap does not
       apply to: both round transitions make this call from the game tick,
       and a full queue used to refuse it and send the post here, stopping
       the game for as long as it took. A FALSE therefore means the worker
       is not running, which is the case winbolonetGoodbye is in - it
       flushes after the thread has been destroyed and there is no later
       moment for the post to go out - so the fallback posts here, with the
       key the worker would have stamped. */
    char *json_str = cJSON_PrintUnformatted(body);
    bool queued = FALSE;
    if (json_str) {
      queued = winbolonetThreadAddSessionRequest("server/update", json_str);
      free(json_str);
    }
    if (queued == TRUE) {
      cJSON_Delete(body);
    } else {
      cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
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
*NAME:          winbolonetBuildVerifyBody
*PURPOSE:
* Builds the client/verify request body. Shared by the
* synchronous verify and the queued one so the two send the
* same request.
*
*ARGUMENTS:
* playerKey  - The key the client presented
* playerName - Name to verify and attribute under
*********************************************************/
static cJSON *winbolonetBuildVerifyBody(const char *playerKey,
                                        const char *playerName) {
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "player_key", playerKey);
  cJSON_AddStringToObject(body, "player_name", playerName);
  return body;
}

/*********************************************************
*NAME:          copyWireError
*PURPOSE:
* Copies a WinBolo.net-supplied error string into a
* caller's error buffer, bounded. The strings come off the
* wire and the buffer is contracted at 256 bytes, so the
* copy is capped at 255 characters plus the terminator.
*
*ARGUMENTS:
* errorMsg - Caller's buffer (>= 256)
* src      - The "error" string from the reply
*********************************************************/
static void copyWireError(char *errorMsg, const char *src) {
  strncpy(errorMsg, src, 255);
  errorMsg[255] = '\0';
}

/*********************************************************
*NAME:          applyVerifyResponse
*PURPOSE:
* Reads one client/verify reply. Shared by the synchronous
* winboloNetVerifyClientKey and the queued
* winbolonetApplyVerifyResult so the two cannot drift.
*
* Writes winboloNetPlayerKey[playerNum] on acceptance, so
* whichever thread calls this is the thread the slot's key
* is written on.
*
*ARGUMENTS:
* status      - HTTP status, or -1 when the post never sent
* resp        - Parsed reply, or NULL
* playerKey   - The key that was presented
* playerNum   - Slot the key belongs to
* errorMsg    - Filled on refusal (>= 256)
* hasSteam    - Set from the reply, or FALSE
* isSupporter - Set from the reply, or FALSE
*********************************************************/
static bool applyVerifyResponse(int status, cJSON *resp, const char *playerKey,
                                BYTE playerNum, char *errorMsg,
                                bool *hasSteam, bool *isSupporter) {
  bool ok = FALSE;

  if (hasSteam) *hasSteam = FALSE;
  if (isSupporter) *isSupporter = FALSE;

  if (status == 200 && resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      copyWireError(errorMsg, errObj->valuestring);
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
      copyWireError(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "WinBolo.net verification failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

  return ok;
}

/*********************************************************
*NAME:          winboloNetVerifyClientKey
*PURPOSE:
* Validates a player_key received off the wire via
* POST /api/v1/client/verify. On success, copies the
* player_key into the player's slot in winboloNetPlayerKey
* so subsequent events/leaves can identify the player.
*
* Posts on the calling thread. Callers on the server tick
* use winbolonetQueueVerifyClientKey instead.
*********************************************************/
bool winboloNetVerifyClientKey(const char *playerKey, const char *playerName, BYTE playerNum, char *errorMsg, bool *hasSteam, bool *isSupporter) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok;

  if (hasSteam) *hasSteam = FALSE;
  if (isSupporter) *isSupporter = FALSE;

  if (winboloNetRunning != TRUE || winboloNetServerKey[0] == '\0') {
    strcpy(errorMsg, "WinBolo.net not running");
    return FALSE;
  }

  body = winbolonetBuildVerifyBody(playerKey, playerName);
  status = wbn_api_call("client/verify", body, &resp);
  cJSON_Delete(body);

  ok = applyVerifyResponse(status, resp, playerKey, playerNum, errorMsg,
                           hasSteam, isSupporter);

  cJSON_Delete(resp);
  return ok;
}

/*********************************************************
*NAME:          winbolonetQueueVerifyClientKey
*PURPOSE:
* Queues a client/verify as a job whose reply comes back
* through winbolonetThreadDrainResults with kind
* WBN_JOB_VERIFY. The caller applies it with
* winbolonetApplyVerifyResult, which is where the slot's
* key is written.
*
* Sent without the bearer, as the synchronous verify is.
*
* Returns the job id, or 0 when nothing was queued — in
* which case no result is coming.
*
*ARGUMENTS:
* playerKey  - The key the client presented
* playerName - Name to verify and attribute under
*********************************************************/
uint32_t winbolonetQueueVerifyClientKey(const char *playerKey,
                                        const char *playerName) {
  cJSON *body = NULL;
  char *json_str = NULL;
  uint32_t id = 0;

  if (winboloNetRunning != TRUE || winboloNetServerKey[0] == '\0') {
    return 0;
  }

  body = winbolonetBuildVerifyBody(playerKey, playerName);
  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    id = winbolonetThreadAddJob("client/verify", json_str,
                                /*needs_bearer*/ FALSE, WBN_JOB_VERIFY);
    free(json_str);
  }
  cJSON_Delete(body);

  return id;
}

/*********************************************************
*NAME:          winbolonetApplyVerifyResult
*PURPOSE:
* Applies the reply to a queued client/verify. Same reading
* as the synchronous winboloNetVerifyClientKey, which is why
* both run through the same apply, and the same write of
* winboloNetPlayerKey[playerNum] on acceptance — so the slot
* key is written on whichever thread drains the result.
*
*ARGUMENTS:
* status      - HTTP status the worker got, or -1
* response    - Reply body, or NULL
* playerKey   - The key the client presented
* playerNum   - Slot the key belongs to
* errorMsg    - Filled on refusal (>= 256)
* hasSteam    - Set from the reply, or FALSE
* isSupporter - Set from the reply, or FALSE
*********************************************************/
bool winbolonetApplyVerifyResult(int status, const char *response,
                                 const char *playerKey, BYTE playerNum,
                                 char *errorMsg, bool *hasSteam,
                                 bool *isSupporter) {
  cJSON *resp = NULL;
  bool ok;

  if (hasSteam) *hasSteam = FALSE;
  if (isSupporter) *isSupporter = FALSE;

  if (winboloNetRunning != TRUE) {
    strcpy(errorMsg, "WinBolo.net not running");
    return FALSE;
  }

  if (response != NULL) {
    resp = cJSON_Parse(response);
  }

  ok = applyVerifyResponse(status, resp, playerKey, playerNum, errorMsg,
                           hasSteam, isSupporter);

  cJSON_Delete(resp);
  return ok;
}

/*********************************************************
*NAME:          winbolonetBuildVerifyJoinCodeBody
*PURPOSE:
* Builds the client/verify_join_code request body. Shared by
* the synchronous resolve and the queued one so the two send
* the same request.
*
*ARGUMENTS:
* joinCode - The code the web client presented
*********************************************************/
static cJSON *winbolonetBuildVerifyJoinCodeBody(const char *joinCode) {
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "server_key", winboloNetServerKey);
  cJSON_AddStringToObject(body, "join_code", joinCode);
  return body;
}

/*********************************************************
*NAME:          applyVerifyJoinCodeResponse
*PURPOSE:
* Reads one client/verify_join_code reply. Shared by the
* synchronous winboloNetVerifyJoinCode and the queued
* winbolonetApplyVerifyJoinCodeResult so the two cannot
* drift.
*
* Read-only: nothing is stored against a slot here, so this
* is safe to run on whichever thread drains the result. The
* caller places what comes back.
*
*ARGUMENTS:
* status        - HTTP status, or -1 when the post never sent
* resp          - Parsed reply, or NULL
* playerNameOut - Resolved name (>= PACKET_MAX_PLAYER_NAME)
* isLoggedInOut - Whether the code names an account
* countryOut    - ISO-2 country (>= 3)
* userIdOut     - WBN user id, or -1
* errorMsg      - Filled on refusal (>= 256)
*********************************************************/
static bool applyVerifyJoinCodeResponse(int status, cJSON *resp,
                                        char *playerNameOut,
                                        bool *isLoggedInOut,
                                        char *countryOut, int *userIdOut,
                                        char *errorMsg) {
  bool ok = FALSE;

  if (isLoggedInOut) *isLoggedInOut = FALSE;
  if (userIdOut) *userIdOut = -1;
  if (playerNameOut) playerNameOut[0] = '\0';
  if (countryOut) countryOut[0] = '\0';

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
        copyWireError(errorMsg, errObj->valuestring);
      } else {
        strcpy(errorMsg, "WinBolo.net join code verification rejected");
      }
    }
  } else if (resp) {
    cJSON *errObj = cJSON_GetObjectItem(resp, "error");
    if (errObj && cJSON_IsString(errObj)) {
      copyWireError(errorMsg, errObj->valuestring);
    } else {
      strcpy(errorMsg, "WinBolo.net join code verification failed");
    }
  } else {
    strcpy(errorMsg, "No response from WinBolo.net");
  }

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
*
* Posts on the calling thread. Callers on the server tick
* use winbolonetQueueVerifyJoinCode instead.
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
  bool ok;

  if (isLoggedInOut) *isLoggedInOut = FALSE;
  if (userIdOut) *userIdOut = -1;
  if (playerNameOut) playerNameOut[0] = '\0';
  if (countryOut) countryOut[0] = '\0';

  if (winboloNetRunning != TRUE || winboloNetServerKey[0] == '\0') {
    strcpy(errorMsg, "WinBolo.net not running");
    return FALSE;
  }

  body = winbolonetBuildVerifyJoinCodeBody(joinCode);
  status = wbn_api_call("client/verify_join_code", body, &resp);
  cJSON_Delete(body);

  ok = applyVerifyJoinCodeResponse(status, resp, playerNameOut, isLoggedInOut,
                                   countryOut, userIdOut, errorMsg);

  cJSON_Delete(resp);
  return ok;
}

/*********************************************************
*NAME:          winbolonetQueueVerifyJoinCode
*PURPOSE:
* Queues the client/verify_join_code winboloNetVerifyJoinCode
* would post. The reply arrives through
* winbolonetThreadDrainResults with kind WBN_JOB_VERIFY and
* is read with winbolonetApplyVerifyJoinCodeResult.
*
* Sent without the bearer, as the synchronous resolve is.
* Nothing is stored against a slot here, so the caller keeps
* whatever it needs to place the reply.
*
* Returns the job id, or 0 when nothing was queued, in which
* case no result is coming.
*
*ARGUMENTS:
* joinCode - The code the web client presented
*********************************************************/
uint32_t winbolonetQueueVerifyJoinCode(const char *joinCode) {
  cJSON *body = NULL;
  char *json_str = NULL;
  uint32_t id = 0;

  if (winboloNetRunning != TRUE || winboloNetServerKey[0] == '\0') {
    return 0;
  }

  body = winbolonetBuildVerifyJoinCodeBody(joinCode);
  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    id = winbolonetThreadAddJob("client/verify_join_code", json_str,
                                /*needs_bearer*/ FALSE, WBN_JOB_VERIFY);
    free(json_str);
  }
  cJSON_Delete(body);

  return id;
}

/*********************************************************
*NAME:          winbolonetApplyVerifyJoinCodeResult
*PURPOSE:
* Reads the reply to a queued client/verify_join_code,
* exactly as winboloNetVerifyJoinCode reads its own. Stores
* nothing: the caller places the resolved identity on the
* slot it queued for, on the thread that drains the result.
*
*ARGUMENTS:
* status        - HTTP status the worker got, or -1
* response      - Reply body, or NULL
* playerNameOut - Resolved name (>= PACKET_MAX_PLAYER_NAME)
* isLoggedInOut - Whether the code names an account
* countryOut    - ISO-2 country (>= 3)
* userIdOut     - WBN user id, or -1
* errorMsg      - Filled on refusal (>= 256)
*********************************************************/
bool winbolonetApplyVerifyJoinCodeResult(int status, const char *response,
                                         char *playerNameOut,
                                         bool *isLoggedInOut,
                                         char *countryOut, int *userIdOut,
                                         char *errorMsg) {
  cJSON *resp = NULL;
  bool ok;

  if (isLoggedInOut) *isLoggedInOut = FALSE;
  if (userIdOut) *userIdOut = -1;
  if (playerNameOut) playerNameOut[0] = '\0';
  if (countryOut) countryOut[0] = '\0';

  if (winboloNetRunning != TRUE) {
    strcpy(errorMsg, "WinBolo.net not running");
    return FALSE;
  }

  if (response != NULL) {
    resp = cJSON_Parse(response);
  }

  ok = applyVerifyJoinCodeResponse(status, resp, playerNameOut, isLoggedInOut,
                                   countryOut, userIdOut, errorMsg);

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
      copyWireError(errorMsg, errObj->valuestring);
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
      copyWireError(errorMsg, errObj->valuestring);
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
  char *json_str = NULL;

  winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_LEAVE, TRUE, playerNum, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
  if (winboloNetPlayerKey[playerNum][0] == '\0' || winboloNetRunning != TRUE) {
    return;
  }

  /* Flush buffered events */
  winbolonetServerUpdate(numPlayers, freeBases, freePills, TRUE);

  /* Send leave */
  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "player_key", winboloNetPlayerKey[playerNum]);
  cJSON_AddNumberToObject(body, "num_players", numPlayers);
  cJSON_AddNumberToObject(body, "free_bases", freeBases);
  cJSON_AddNumberToObject(body, "free_pills", freePills);

  /* The key is copied into the body above and serialised here, so clearing
     the slot below cannot reach the post. */
  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    winbolonetThreadAddServerKeyedRequest("client/leave", json_str);
    free(json_str);
  }
  cJSON_Delete(body);

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
  char *json_str = NULL;

  if (spectatorKey == NULL || spectatorKey[0] == '\0' || winboloNetRunning != TRUE) {
    return;
  }

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "player_key", spectatorKey);

  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    winbolonetThreadAddServerKeyedRequest("client/leave", json_str);
    free(json_str);
  }
  cJSON_Delete(body);
}

/*********************************************************
*NAME:          winboloNetSendLock
*PURPOSE:
* Sends lock/unlock status via POST /api/v1/server/lock.
*********************************************************/
void winboloNetSendLock(bool isLocked) {
  cJSON *body = NULL;
  char *json_str = NULL;

  if (winboloNetRunning != TRUE) {
    return;
  }

  body = cJSON_CreateObject();
  cJSON_AddBoolToObject(body, "locked", isLocked);

  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    winbolonetThreadAddServerKeyedRequest("server/lock", json_str);
    free(json_str);
  }
  cJSON_Delete(body);
}

/*********************************************************
*NAME:          winbolonetEndSession
*PURPOSE:
* Ends the current WBN session: drains the background
* thread, POSTs server/quit, clears the bearer + per-slot
* player keys, and resets the event queue. The worker is
* drained, not destroyed: it runs for the server's life, so
* its pooled connection carries into the next session. The
* HTTP layer stays alive so a subsequent
* winbolonetBeginSession can re-register (and so the
* per-round log uploader can fire
* httpSendLogFile in between, against the still-valid
* winboloNetServerKey — WBN rejects uploads to an active
* session, so the upload has to follow the server/quit
* POST but precede server/register's key swap).
*********************************************************/
void winbolonetEndSession(uint32_t drainMaxMs) {
  BYTE count;
  cJSON *body = NULL;
  cJSON *resp = NULL;

  if (winboloNetRunning != TRUE) {
    return;
  }

  serverSimConsoleMessage("WinBolo.net: Ending session...");

  /* Empty the queue before the bearer below is cleared, so nothing
   * queued against this session fires against the next one. A caller
   * that cannot wait out an unreachable WinBolo.net gives a deadline;
   * what is still queued when it passes is posted against the next
   * session, which is the cost of not holding the caller. */
  winbolonetThreadDrainFor(drainMaxMs);

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
* Registers a fresh WBN session for the next round. The
* background thread is not started here: it is created once
* after the first server/register and runs for the server's
* life. Pairs with winbolonetEndSession at round boundaries.
*********************************************************/
bool winbolonetBeginSession(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;

  if (winboloNetRunning != TRUE) {
    return FALSE;
  }

  body = winbolonetBuildRegisterBody(mapName, port, gameType, ai, mines, password, numBases, numPills, freeBases, freePills, numPlayers);
  status = wbn_api_call("server/register", body, &resp);
  cJSON_Delete(body);

  if (!winbolonetApplyRegisterResponse(status, resp,
                                       "\tWinBolo.net: New session registered",
                                       "Error: WinBolo.net re-registration failed",
                                       "Error: WinBolo.net re-registration failed - WBN disabled")) {
    cJSON_Delete(resp);
    winboloNetRunning = FALSE;
    return FALSE;
  }

  cJSON_Delete(resp);

  winboloNetLastSent = time(NULL);

  return TRUE;
}

/*********************************************************
*NAME:          winbolonetQueueEndSession
*PURPOSE:
* Ends the current WBN session without waiting on it.
* Queues server/quit for the worker, then clears the
* per-slot player keys and resets the event queue, as
* winbolonetEndSession does.
*
* What it deliberately does not clear is the bearer and
* winboloNetServerKey. The queued quit needs the bearer at
* fire time, the round-log upload queued behind it needs the
* key, and the server/register queued behind that replaces
* both when its result is applied. Pairs with
* winbolonetQueueBeginSession, and the caller puts the
* upload between the two.
*
* Returns TRUE when the worker took the quit, FALSE when it
* is not running. The answer is the caller's signal for the
* whole rotation: the register behind this would be refused
* the same way, so a FALSE means the round transition has to
* be sent on the calling thread.
*********************************************************/
bool winbolonetQueueEndSession(void) {
  BYTE count;
  cJSON *body = NULL;
  char *json_str = NULL;
  bool queued = FALSE;

  if (winboloNetRunning != TRUE) {
    return FALSE;
  }

  /* Keyed at fire time like the rest. The quit fires before the register
   * behind it, and the register's result is what swaps the key, so it still
   * names the session being ended. Queued through the session form so a
   * full waiting queue cannot refuse it: a dropped quit leaves the finished
   * session listed, and the refusal here is what sends the whole rotation
   * down the synchronous path. */
  body = cJSON_CreateObject();
  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    queued = winbolonetThreadAddSessionRequest("server/quit", json_str);
    free(json_str);
  }
  cJSON_Delete(body);
  /* Only once the quit is on the queue: a refusal sends the caller to
     winbolonetEndSession, which announces the same session's end itself. */
  if (queued) {
    serverSimConsoleMessage("WinBolo.net: Ending session...");
  }

  for (count = 0; count < MAX_TANKS; count++) {
    winboloNetPlayerKey[count][0] = '\0';
  }

  winbolonetEventsDestroy();
  winbolonetEventsCreate();

  return queued;
}

/*********************************************************
*NAME:          winbolonetQueueBeginSession
*PURPOSE:
* Queues the next round's server/register as a job whose
* reply comes back through winbolonetThreadDrainResults with
* kind WBN_JOB_REGISTER. The caller applies it with
* winbolonetApplyRegisterResult.
*
* Sent without the bearer, as the synchronous register is:
* the reply is what issues the next one.
*
* Returns the job id, or 0 when nothing was queued — in
* which case no result is coming and the caller owns the
* tail itself.
*
*ARGUMENTS:
* As winbolonetCreateServer.
*********************************************************/
uint32_t winbolonetQueueBeginSession(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  cJSON *body = NULL;
  char *json_str = NULL;
  uint32_t id = 0;

  if (winboloNetRunning != TRUE) {
    return 0;
  }

  body = winbolonetBuildRegisterBody(mapName, port, gameType, ai, mines, password, numBases, numPills, freeBases, freePills, numPlayers);
  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    id = winbolonetThreadAddJob("server/register", json_str,
                                /*needs_bearer*/ FALSE, WBN_JOB_REGISTER);
    free(json_str);
  }
  cJSON_Delete(body);

  return id;
}

/*********************************************************
*NAME:          winbolonetApplyRegisterResult
*PURPOSE:
* Applies the reply to a queued server/register, installing
* the new server key and bearer. Same handling as the
* synchronous winbolonetBeginSession, which is why it runs
* through the same apply.
*
* Returns TRUE when the new session is live. On FALSE the
* old session is gone and no new one replaced it, so
* WinBolo.net is switched off and the bearer the old session
* was issued cleared.
*
*ARGUMENTS:
* status   - HTTP status the worker got, or -1
* response - Reply body, or NULL
*********************************************************/
bool winbolonetApplyRegisterResult(int status, const char *response) {
  cJSON *resp = NULL;
  bool ok;

  if (winboloNetRunning != TRUE) {
    return FALSE;
  }

  if (response != NULL) {
    resp = cJSON_Parse(response);
  }
  ok = winbolonetApplyRegisterResponse(status, resp,
                                       "\tWinBolo.net: New session registered",
                                       "Error: WinBolo.net re-registration failed",
                                       "Error: WinBolo.net re-registration failed - WBN disabled");
  cJSON_Delete(resp);

  if (ok != TRUE) {
    httpClearServerBearerToken();
    winboloNetRunning = FALSE;
    return FALSE;
  }

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
  cJSON_AddBoolToObject(body, "in_lobby", inLobby);

  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    winbolonetThreadAddServerKeyedRequest("server/lobby", json_str);
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
  cJSON_AddStringToObject(body, "map", mapName);
  cJSON_AddNumberToObject(body, "num_bases", numBases);
  cJSON_AddNumberToObject(body, "num_pills", numPills);
  cJSON_AddNumberToObject(body, "free_bases", freeBases);
  cJSON_AddNumberToObject(body, "free_pills", freePills);

  json_str = cJSON_PrintUnformatted(body);
  if (json_str) {
    winbolonetThreadAddServerKeyedRequest("server/map", json_str);
    free(json_str);
  }
  cJSON_Delete(body);
}

