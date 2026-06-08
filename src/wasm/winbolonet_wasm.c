/*
 * winbolonet_wasm.c - No-op WinBolo.net stubs for WASM build
 *
 * Replaces all of winbolonet/*.c (winbolonet.c, http.c,
 * winbolonetevents.c, winbolonetthread.c).  WinBolo.net requires
 * libcurl which is not available in Emscripten, so all functions
 * are safe no-ops.
 */

#include <string.h>
#include "global.h"
#include "../winbolonet/winbolonet_core.h"
#include "../winbolonet/winbolonet_server.h"
#include "../winbolonet/winbolonet_client.h"
#include "../winbolonet/winbolonetevents.h"
#include "../winbolonet/winbolonetthread.h"
#include "../winbolonet/http.h"
#include "../gui/sdl3/dialogs/imgui_news.h"

struct cJSON;

/* -------------------------------------------------------
 * http.h
 * ------------------------------------------------------- */
bool httpCreate(void)                                              { return FALSE; }
void httpDestroy(void)                                             { }
int  wbn_api_post(const char *ep, const char *jb, char **ro)      { (void)ep; (void)jb; if (ro) *ro = NULL; return -1; }
int  wbn_api_call(const char *ep, struct cJSON *b, struct cJSON **r) { (void)ep; (void)b; if (r) *r = NULL; return -1; }
bool httpSendLogFile(char *fn, char *key, bool fb)                 { (void)fn; (void)key; (void)fb; return FALSE; }
void httpSetAltIpAddress(char *ip)                                 { (void)ip; }

/* -------------------------------------------------------
 * winbolonetevents.h
 * ------------------------------------------------------- */
void winbolonetEventsCreate(void)                                  { }
void winbolonetEventsDestroy(void)                                 { }
void winbolonetEventsAddItem(BYTE t, const char *a, const char *b, bool aIsBot, bool bIsBot) { (void)t; (void)a; (void)b; (void)aIsBot; (void)bIsBot; }
int  winbolonetEventsGetSize(void)                                 { return 0; }
BYTE winbolonetEventsRemove(char *a, char *b, bool *aIsBot, bool *bIsBot)  { (void)a; (void)b; (void)aIsBot; (void)bIsBot; return WINBOLONET_EVENT_NOITEM; }

/* -------------------------------------------------------
 * winbolonetthread.h
 * ------------------------------------------------------- */
bool winbolonetThreadCreate(void)                                  { return FALSE; }
void winbolonetThreadDestroy(void)                                 { }
void winbolonetThreadAddRequest(const char *ep, const char *jb)    { (void)ep; (void)jb; }
int  winbolonetThreadRun(void *data)                                { (void)data; return 0; }

/* -------------------------------------------------------
 * winbolonet.h
 * ------------------------------------------------------- */
bool winbolonetCreateServer(char *mapName, unsigned short port,
    BYTE gameType, BYTE ai, bool mines, bool password,
    BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills,
    BYTE numPlayers) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines;
  (void)password; (void)numBases; (void)numPills; (void)freeBases;
  (void)freePills; (void)numPlayers;
  return FALSE;
}

void winbolonetDestroy(bool isServer)                               { (void)isServer; }
void winbolonetGoodbye(void)                                       { }

void winboloNetGetServerKey(char *keyBuff)                         { if (keyBuff) keyBuff[0] = '\0'; }
bool winboloNetIsPlayerParticipant(BYTE playerNum)                 { (void)playerNum; return FALSE; }
bool winbolonetIsRunning(void)                                     { return FALSE; }

void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers,
                                BYTE freeBases, BYTE freePills) {
  (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}

void winbolonetServerUpdate(BYTE numPlayers, BYTE numFreeBases,
                             BYTE numFreePills, bool sendNow) {
  (void)numPlayers; (void)numFreeBases; (void)numFreePills; (void)sendNow;
}

void winbolonetServerSendTeams(BYTE *array, BYTE length, BYTE numTeams) {
  (void)array; (void)length; (void)numTeams;
}

void winbolonetAddEvent(BYTE eventType, bool isServer,
                         BYTE playerA, BYTE playerB, bool aIsBot, bool bIsBot) {
  (void)eventType; (void)isServer; (void)playerA; (void)playerB; (void)aIsBot; (void)bIsBot;
}

void winboloNetSendLock(bool isLocked)                             { (void)isLocked; }

void winbolonetSendMapChange(char *mapName, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills) {
  (void)mapName; (void)numBases; (void)numPills; (void)freeBases; (void)freePills;
}

void winbolonetSetLobbyInfo(const WbnLobbyInfo *info) { (void)info; }

void winbolonetSendLobbyUpdate(void) { }

bool winboloNetVerifyClientKey(const char *playerKey, const char *playerName, BYTE playerNum, char *errorMsg, bool *hasSteam, bool *isSupporter) {
  (void)playerKey; (void)playerName; (void)playerNum;
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  if (hasSteam)    *hasSteam    = FALSE;
  if (isSupporter) *isSupporter = FALSE;
  return FALSE;
}

bool winbolonetClientJoinSession(const char *apiToken, const char *serverKey, char *playerKeyOut, char *errorMsg) {
  (void)apiToken; (void)serverKey;
  if (playerKeyOut) playerKeyOut[0] = '\0';
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  return FALSE;
}

bool winbolonetAuthLogin(const char *username, const char *password, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
  (void)username; (void)password; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut;
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  return FALSE;
}

bool winbolonetAuthSteam(const char *steamTicketHex, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
  (void)steamTicketHex; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut;
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  return FALSE;
}

bool winbolonetAuthSteamRegister(const char *steamTicketHex, const char *username, const char *email, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg, char *errorCodeOut) {
  (void)steamTicketHex; (void)username; (void)email; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut;
  if (errorCodeOut) errorCodeOut[0] = '\0';
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  return FALSE;
}

bool winbolonetAuthValidate(const char *token, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
  (void)token; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut;
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  return FALSE;
}

/* -------------------------------------------------------
 * winbolonet_core.h — country code (no tracker lookup here)
 * ------------------------------------------------------- */
const char *winbolonetGetCountryCode(void)                         { return ""; }

/* -------------------------------------------------------
 * imgui_news.h — the news popup fetches over HTTP; no-op in WASM
 * ------------------------------------------------------- */
bool newsPopupIsOpen(void)                                         { return FALSE; }
bool newsPopupHasUnread(void)                                      { return FALSE; }
void newsPopupKickFetch(void)                                      { }
void newsPopupTick(void)                                           { }
void newsPopupOpenManual(void)                                     { }
void newsPopupShutdown(void)                                       { }
