/*
 * winbolonet_wasm.c - No-op WinBolo.net stubs for WASM build
 *
 * Replaces all of winbolonet/*.c (winbolonet.c, http.c,
 * winbolonetevents.c, winbolonetthread.c).  WinBolo.net requires
 * libcurl which is not available in Emscripten, so all functions
 * are safe no-ops.
 */

#include <string.h>
#include "../bolo/global.h"
#include "../winbolonet/winbolonet.h"
#include "../winbolonet/winbolonetevents.h"
#include "../winbolonet/winbolonetthread.h"
#include "../winbolonet/http.h"

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
void winbolonetEventsAddItem(BYTE t, const char *a, const char *b) { (void)t; (void)a; (void)b; }
int  winbolonetEventsGetSize(void)                                 { return 0; }
BYTE winbolonetEventsRemove(char *a, char *b)                      { (void)a; (void)b; return WINBOLONET_EVENT_NOITEM; }

/* -------------------------------------------------------
 * winbolonetthread.h
 * ------------------------------------------------------- */
bool winbolonetThreadCreate(void)                                  { return FALSE; }
void winbolonetThreadDestroy(void)                                 { }
void winbolonetThreadAddRequest(const char *ep, const char *jb)    { (void)ep; (void)jb; }
int  winbolonetThreadRun(void)                                     { return 0; }

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

bool winbolonetCreateClient(const char *token, const char *serverKey, char *errorMsg) {
  (void)token; (void)serverKey;
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  return FALSE;
}

void winbolonetDestroy(bool isServer)                               { (void)isServer; }
void winbolonetGoodbye(void)                                       { }

void winboloNetGetServerKey(char *keyBuff)                         { if (keyBuff) keyBuff[0] = '\0'; }
void winboloNetGetMyClientKey(char *keyBuff)                       { if (keyBuff) keyBuff[0] = '\0'; }
bool winboloNetVerifyClientKey(const char *k, char *u, BYTE n)     { (void)k; (void)u; (void)n; return FALSE; }
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
                         BYTE playerA, BYTE playerB) {
  (void)eventType; (void)isServer; (void)playerA; (void)playerB;
}

void winboloNetSendLock(bool isLocked)                             { (void)isLocked; }
