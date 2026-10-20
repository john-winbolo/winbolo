/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * winbolonet_stub.c - Stub implementations of WinBolo.net functions
 *
 * On Android we don't include the winbolonet source files (which depend
 * on libcurl). Instead we stub out all functions declared in the
 * winbolonet_core / winbolonet_server / winbolonet_client headers.
 */

#include <stdint.h>

#include <string.h>

#include "global.h"
#include "../winbolonet/winbolonet_core.h"
#include "../winbolonet/winbolonet_server.h"
#include "../winbolonet/winbolonet_client.h"
#include "../winbolonet/wbn_serverlist.h"
#include "../winbolonet/wbn_prefs_sync.h"
#include "../winbolonet/winbolonetthread.h"
#include "../winbolonet/http.h"

bool winbolonetCreateServer(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
  (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers;
  return FALSE;
}

void winbolonetDestroy(bool isServer) { (void)isServer; }

void winboloNetGetServerKey(char *keyBuff) {
  if (keyBuff) keyBuff[0] = '\0';
}

void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
  (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}

void winbolonetGoodbye(void) { }

void winbolonetServerSendTeams(BYTE *array, BYTE length, BYTE numTeams) {
  (void)array; (void)length; (void)numTeams;
}

void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB, bool aIsBot, bool bIsBot) {
  (void)eventType; (void)isServer; (void)playerA; (void)playerB; (void)aIsBot; (void)bIsBot;
}

void winbolonetServerUpdate(BYTE numPlayers, BYTE numFreeBases, BYTE numFreePills, bool sendNow) {
  (void)numPlayers; (void)numFreeBases; (void)numFreePills; (void)sendNow;
}

bool winbolonetIsRunning(void) {
  return FALSE;
}

bool winboloNetIsPlayerParticipant(BYTE playerNum) {
  (void)playerNum;
  return FALSE;
}

void winboloNetSendLock(bool isLocked) {
  (void)isLocked;
}

bool winboloNetVerifyClientKey(const char *playerKey, const char *playerName, BYTE playerNum, char *errorMsg, bool *hasSteam, bool *isSupporter) {
  (void)playerKey; (void)playerName; (void)playerNum; (void)errorMsg;
  if (hasSteam)    *hasSteam    = FALSE;
  if (isSupporter) *isSupporter = FALSE;
  return FALSE;
}

bool winboloNetVerifyJoinCode(const char *joinCode, char *playerNameOut, bool *isLoggedInOut, char *countryOut, int *userIdOut, char *errorMsg) {
  (void)joinCode;
  if (playerNameOut) playerNameOut[0] = '\0';
  if (isLoggedInOut) *isLoggedInOut   = FALSE;
  if (countryOut)    countryOut[0]    = '\0';
  if (userIdOut)     *userIdOut       = -1;
  if (errorMsg)      errorMsg[0]      = '\0';
  return FALSE;
}

bool winbolonetClientJoinSession(const char *apiToken, const char *serverKey, char *playerKeyOut, char *errorMsg) {
  (void)apiToken; (void)serverKey; (void)errorMsg;
  if (playerKeyOut) playerKeyOut[0] = '\0';
  return FALSE;
}

bool winbolonetClientJoinSpectatorSession(const char *apiToken, const char *serverKey, const char *playerName, char *spectatorKeyOut, char *errorMsg) {
  (void)apiToken; (void)serverKey; (void)playerName; (void)errorMsg;
  if (spectatorKeyOut) spectatorKeyOut[0] = '\0';
  return FALSE;
}

bool winboloNetVerifySpectatorKey(const char *spectatorKey, const char *playerName, char *errorMsg, bool *isLoggedIn) {
  (void)spectatorKey; (void)playerName; (void)errorMsg;
  if (isLoggedIn) *isLoggedIn = FALSE;
  return FALSE;
}

void winboloNetSpectatorLeaveGame(const char *spectatorKey) {
  (void)spectatorKey;
}

bool winbolonetAuthLogin(const char *username, const char *password, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
  (void)username; (void)password; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut; (void)errorMsg;
  return FALSE;
}

bool winbolonetAuthSteam(const char *steamTicketHex, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
  (void)steamTicketHex; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut; (void)errorMsg;
  return FALSE;
}

bool winbolonetAuthSteamRegister(const char *steamTicketHex, const char *username, const char *email, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg, char *errorCodeOut) {
  (void)steamTicketHex; (void)username; (void)email; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut; (void)errorMsg; (void)errorCodeOut;
  return FALSE;
}

bool winbolonetAuthValidate(const char *token, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
  (void)token; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut; (void)errorMsg;
  return FALSE;
}

/* Reports the token as turned down, so callers clear it as they always
 * have here. */
bool winbolonetAuthValidateEx(const char *token, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg, bool *tokenRejectedOut) {
  if (tokenRejectedOut) *tokenRejectedOut = TRUE;
  return winbolonetAuthValidate(token, playerNameOut, rankOut, rankTotalOut, statsOut, errorMsg);
}

void winbolonetSendMapChange(char *mapName, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills) {
  (void)mapName; (void)numBases; (void)numPills; (void)freeBases; (void)freePills;
}

void winbolonetSetLobbyInfo(const WbnLobbyInfo *info) { (void)info; }

void winbolonetSendLobbyUpdate(void) { }

bool winbolonetServerRequestBalance(uint8_t totalPlayers, uint8_t teamSize,
                                     const uint8_t *botSlots, uint8_t numBotSlots,
                                     BalanceProposal *outProposal) {
  (void)totalPlayers; (void)teamSize;
  (void)botSlots; (void)numBotSlots;
  (void)outProposal;
  return FALSE;
}

void winbolonetEndSession(uint32_t drainMaxMs) { (void)drainMaxMs; }

const char *winbolonetGetCountryCode(void) { return ""; }

/* The internet game list: no list, so the finder shows none. */
bool wbnFetchServerList(WbnServerList *out) {
  if (out) memset(out, 0, sizeof(*out));
  return FALSE;
}

void wbnServerListFree(WbnServerList *out) {
  if (out) memset(out, 0, sizeof(*out));
}

/* Cloud preferences: nothing changed, so the local document stands. */
WbnSyncOutcome wbnPrefsSyncOnce(const char *userToken, const char *uploadSnapshot,
                                const char *deviceType,
                                bool localDirty, const char *lastSynced) {
  (void)userToken; (void)uploadSnapshot; (void)deviceType;
  (void)localDirty; (void)lastSynced;
  WbnSyncOutcome out;
  memset(&out, 0, sizeof(out));
  out.kind = WBN_SYNC_OUT_NOOP;
  return out;
}

void httpSetLogUploadTimeout(long seconds) { (void)seconds; }

/* The lifecycle-driven server surface server_static calls: the session
 * register and quit, the queued key and join-code checks, the lobby status
 * line, and the round-log upload. Nothing is queued, so no result ever
 * arrives and every check reports the player as not verified. */
bool winbolonetBeginSession(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
  (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers;
  return FALSE;
}

uint32_t winbolonetQueueBeginSession(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
  (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers;
  return 0;
}

bool winbolonetApplyRegisterResult(int status, const char *response) {
  (void)status; (void)response;
  return FALSE;
}

bool winbolonetQueueEndSession(void) { return FALSE; }

void winbolonetSendLobbyStatus(bool inLobby) { (void)inLobby; }

uint32_t winbolonetQueueVerifyClientKey(const char *playerKey, const char *playerName) {
  (void)playerKey; (void)playerName;
  return 0;
}

bool winbolonetApplyVerifyResult(int status, const char *response,
                                 const char *playerKey, BYTE playerNum,
                                 char *errorMsg, bool *hasSteam,
                                 bool *isSupporter) {
  (void)status; (void)response; (void)playerKey; (void)playerNum;
  if (errorMsg)    errorMsg[0]  = '\0';
  if (hasSteam)    *hasSteam    = FALSE;
  if (isSupporter) *isSupporter = FALSE;
  return FALSE;
}

uint32_t winbolonetQueueVerifyJoinCode(const char *joinCode) {
  (void)joinCode;
  return 0;
}

bool winbolonetApplyVerifyJoinCodeResult(int status, const char *response,
                                         char *playerNameOut,
                                         bool *isLoggedInOut,
                                         char *countryOut, int *userIdOut,
                                         char *errorMsg) {
  (void)status; (void)response;
  if (playerNameOut) playerNameOut[0] = '\0';
  if (isLoggedInOut) *isLoggedInOut   = FALSE;
  if (countryOut)    countryOut[0]    = '\0';
  if (userIdOut)     *userIdOut       = -1;
  if (errorMsg)      errorMsg[0]      = '\0';
  return FALSE;
}

uint32_t winbolonetThreadAddUpload(const char *fileName, const char *key) {
  (void)fileName; (void)key;
  return 0;
}

void winbolonetThreadDrainResults(WbnResultHandler handler, void *ctx) {
  (void)handler; (void)ctx;
}

bool httpSendLogFile(const char *fileName, char *key, bool wantFeedback) {
  (void)fileName; (void)key; (void)wantFeedback;
  return FALSE;
}
