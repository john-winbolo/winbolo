/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * winbolonet_wasm.c - No-op WinBolo.net stubs for WASM build
 *
 * Replaces all of winbolonet/ *.c (winbolonet.c, http.c,
 * winbolonetevents.c, winbolonetthread.c).  WinBolo.net requires
 * libcurl which is not available in Emscripten, so all functions
 * are safe no-ops.
 *
 * Also holds the web's own account helpers. The page learns who is
 * signed in from /api/v1/me (shell.html), and signing in and out
 * happens on www.winbolo.net, so the account block reads the page's
 * state and navigates rather than calling the WinBolo.net API.
 */

#include <string.h>
#include <emscripten.h>
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
bool httpSendLogFile(const char *fn, char *key, bool fb)          { (void)fn; (void)key; (void)fb; return FALSE; }
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
bool winbolonetThreadAddRequest(const char *ep, const char *jb)    { (void)ep; (void)jb; return FALSE; }
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

bool winbolonetClientJoinSpectatorSession(const char *apiToken, const char *serverKey, const char *playerName, char *spectatorKeyOut, char *errorMsg) {
  (void)apiToken; (void)serverKey; (void)playerName;
  if (spectatorKeyOut) spectatorKeyOut[0] = '\0';
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  return FALSE;
}

bool winboloNetVerifySpectatorKey(const char *spectatorKey, const char *playerName, char *errorMsg, bool *isLoggedIn) {
  (void)spectatorKey; (void)playerName;
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  if (isLoggedIn) *isLoggedIn = FALSE;
  return FALSE;
}

void winboloNetSpectatorLeaveGame(const char *spectatorKey) {
  (void)spectatorKey;
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

/* The settings dialog's news auto-show checkbox. There is no news popup on
 * the web, so the choice is only held for the page to keep the checkbox
 * consistent. */
static char s_newsAutoShow[16] = "unset";
const char *newsPrefGetAutoShow(void)                              { return s_newsAutoShow; }
void newsPrefSetAutoShow(const char *value) {
  if (value == NULL) return;
  strncpy(s_newsAutoShow, value, sizeof(s_newsAutoShow) - 1);
  s_newsAutoShow[sizeof(s_newsAutoShow) - 1] = '\0';
}

/* -------------------------------------------------------
 * The web's WinBolo.net account, read from the page
 *
 * shell.html fetches /api/v1/me at load and, once it comes back,
 * sets WB_PREFS_AUTH and WB_PREFS_NAME. WB_PREFS_AUTH starts out
 * true so the prefs sync still downloads a signed-in player's prefs
 * when main() runs first; WB_PREFS_NAME stays undefined until the
 * fetch comes back, which is what tells a pending check from a
 * finished one. The account block calls these once per frame, so
 * none of them waits on anything.
 * ------------------------------------------------------- */

/* 1 until the /api/v1/me fetch has come back. A fetch that fails
 * outright never sets the name, so the caller bounds the wait. */
EM_JS(int, wbAccountPendingJs, (void), {
  return (typeof window.WB_PREFS_NAME === 'undefined') ? 1 : 0;
});

/* Signed in only with a name, so the optimistic WB_PREFS_AUTH the
 * page starts with never reads as signed in on its own. */
EM_JS(int, wbAccountSignedInJs, (void), {
  var name = window.WB_PREFS_NAME;
  return (window.WB_PREFS_AUTH === true &&
          typeof name === 'string' && name.length > 0) ? 1 : 0;
});

EM_JS(void, wbAccountNameJs, (char *out, int outSize), {
  var name = window.WB_PREFS_NAME;
  stringToUTF8((typeof name === 'string') ? name : '', out, outSize);
});

/* Same tab: the site's login returns the player to this page, which
 * then fetches /api/v1/me again and finds them signed in. */
EM_JS(void, wbAccountSignInJs, (void), {
  window.location.href = 'https://www.winbolo.net/login?return=' +
                         encodeURIComponent(window.location.href);
});

EM_JS(void, wbAccountSignOutJs, (void), {
  window.location.href = 'https://www.winbolo.net/logout';
});

bool wasmAccountPending(void)  { return wbAccountPendingJs() != 0; }
bool wasmAccountSignedIn(void) { return wbAccountSignedInJs() != 0; }

void wasmAccountName(char *out, size_t outSize) {
  if (out == NULL || outSize == 0) return;
  out[0] = '\0';
  wbAccountNameJs(out, (int)outSize);
}

void wasmAccountSignIn(void)  { wbAccountSignInJs(); }
void wasmAccountSignOut(void) { wbAccountSignOutJs(); }
