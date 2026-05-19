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
        cJSON *nameObj = cJSON_GetObjectItem(resp, "display_name");
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
bool winbolonetAuthSteam(const char *steamTicketHex, char *tokenOut, char *expiryOut, char *playerNameOut, char *errorMsg) {
  cJSON *body = NULL;
  cJSON *resp = NULL;
  int status;
  bool ok = FALSE;

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
        cJSON *nameObj = cJSON_GetObjectItem(resp, "display_name");
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
