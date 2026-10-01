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
*Name:          WinBolo.net Core
*Filename:      winbolonet_core.c
*Author:        John Morrison
*Creation Date: 23/09/01
*Last Modified: 30/03/26
*Purpose:
*  Core WinBolo.net lifecycle, the shared event queue, and
*  WBN key storage used by both server and client paths.
*********************************************************/

#include <stdlib.h>
#include "cJSON.h"
#include "winbolonet_core.h"
#include "winbolonet_server.h"
#include "http.h"
#include "wbn_bearer.h"
#include "server_sim.h"
#include "winbolonetevents.h"
#include "winbolonetthread.h"

bool winboloNetRunning = FALSE;

char winboloNetServerKey[WINBOLONET_KEY_LEN];
/* Keys for each player if in use - Always position 0 if we are a client */
char winboloNetPlayerKey[MAX_TANKS][WINBOLONET_KEY_LEN];

/*********************************************************
*NAME:          winbolonetDestroy
*PURPOSE:
* Destroys the winbolonet module.
* Cleans up any open libraries.
*********************************************************/
void winbolonetDestroy(bool isServer) {
  /* Listen-server case: the host is both the client and the server in
   * one process, sharing this module. The client teardown (netDestroy
   * -> winbolonetDestroy(FALSE)) runs before the server teardown, so if
   * it tore the module down here it would set winboloNetRunning=FALSE
   * and the server's winbolonetDestroy(TRUE) would skip server/quit —
   * leaving the game listed on WinBolo.net. While a server key is held
   * this process is the server; defer teardown to the server-side call.
   * A pure client never holds a server key, so it is unaffected. */
  if (isServer == FALSE && winboloNetServerKey[0] != '\0') {
    return;
  }
  serverSimConsoleMessage("WinBolo.net Shutdown");
  if (winboloNetRunning == TRUE) {
    winbolonetThreadDestroy();
    if (isServer == TRUE && winboloNetServerKey[0] != '\0') {
      winbolonetGoodbye();
    }
    if (isServer == TRUE) {
      winboloNetServerKey[0] = '\0';
      httpClearServerBearerToken();
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

  wbn_api_call_server("server/quit", body, &resp);
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
*NAME:          winbolonetAddEvent
*PURPOSE:
* Adds a WinBolo.net Event for sending to the server.
*********************************************************/
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB, bool aIsBot, bool bIsBot) {
  const char *keyA;
  const char *keyB;
  char emptyKey[WINBOLONET_KEY_LEN];

  if (winboloNetRunning == TRUE && isServer == TRUE) {
    emptyKey[0] = '\0';
    /* Bounds check: playerA/B are BYTE (0-255) but array is MAX_TANKS (16) */
    if (playerA >= MAX_TANKS) return;
    keyA = winboloNetPlayerKey[playerA];
    if (playerB == WINBOLO_NET_NO_PLAYER) {
      keyB = emptyKey;
      bIsBot = FALSE;
    } else {
      if (playerB >= MAX_TANKS) return;
      keyB = winboloNetPlayerKey[playerB];
    }
    winbolonetEventsAddItem(eventType, keyA, keyB, aIsBot, bIsBot);
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
*NAME:          winboloNetGetServerKey
*PURPOSE:
* Copies the server key into keyBuff.
*********************************************************/
void winboloNetGetServerKey(char *keyBuff) {
  strncpy(keyBuff, winboloNetServerKey, WINBOLONET_KEY_LEN);
}
