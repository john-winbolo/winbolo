/*
 * winbolonet_wasm.c - No-op WinBolo.net stubs for WASM build
 *
 * Replaces all of winbolonet/*.c (winbolonet.c, http.c,
 * winbolonetevents.c, winbolonetthread.c).  WinBolo.net requires
 * libcurl which is not available in Emscripten, and the service is
 * effectively defunct, so all functions are safe no-ops.
 */

#include <string.h>
#include "../bolo/global.h"
#include "../winbolonet/winbolonet.h"
#include "../winbolonet/winbolonetevents.h"
#include "../winbolonet/winbolonetthread.h"
#include "../winbolonet/http.h"

/* -------------------------------------------------------
 * http.h
 * ------------------------------------------------------- */
bool httpCreate(void)                                              { return FALSE; }
void httpDestroy(void)                                             { }
int  httpSendMessage(BYTE *msg, int len, BYTE *resp, int maxSize)  { (void)msg; (void)len; (void)resp; (void)maxSize; return -1; }
bool httpSendLogFile(char *fn, BYTE *key, bool fb)                 { (void)fn; (void)key; (void)fb; return FALSE; }
void httpSetAltIpAddress(char *ip)                                 { (void)ip; }

/* -------------------------------------------------------
 * winbolonetevents.h
 * ------------------------------------------------------- */
void winbolonetEventsCreate(void)                                  { }
void winbolonetEventsDestroy(void)                                 { }
void winbolonetEventsAddItem(BYTE t, BYTE *a, BYTE *b)             { (void)t; (void)a; (void)b; }
int  winbolonetEventsGetSize(void)                                 { return 0; }
BYTE winbolonetEventsRemove(BYTE *a, BYTE *b)                      { (void)a; (void)b; return WINBOLONET_EVENT_NOITEM; }

/* -------------------------------------------------------
 * winbolonetthread.h
 * ------------------------------------------------------- */
bool winbolonetThreadCreate(void)                                  { return FALSE; }
void winbolonetThreadDestroy(void)                                 { }
void winbolonetThreadAddRequest(BYTE *data, int len)               { (void)data; (void)len; }
int  winbolonetThreadRun(void)                                     { return 0; }

/* -------------------------------------------------------
 * winbolonet.h
 * ------------------------------------------------------- */
bool winbolonetCreateServer(char *mapName, unsigned short port,
    BYTE gameType, BYTE ai, bool mines, bool password,
    BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills,
    BYTE numPlayers, long startTime) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines;
  (void)password; (void)numBases; (void)numPills; (void)freeBases;
  (void)freePills; (void)numPlayers; (void)startTime;
  return FALSE;
}

bool winbolonetCreateClient(char *userName, char *password,
                             BYTE *serverKey, char *errorMsg) {
  (void)userName; (void)password; (void)serverKey;
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  return FALSE;
}

void winbolonetDestroy(bool isServer)                               { (void)isServer; }
void winbolonetGoodbye(void)                                       { }

bool winbolonetRequestServerKey(char *mapName, unsigned short port,
    BYTE gameType, BYTE ai, bool mines, bool password,
    BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills,
    BYTE numPlayers, long startTime) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines;
  (void)password; (void)numBases; (void)numPills; (void)freeBases;
  (void)freePills; (void)numPlayers; (void)startTime;
  return FALSE;
}

bool winbolonetRequestClientKey(char *userName, char *password,
                                 BYTE *serverKey, char *errorMsg) {
  (void)userName; (void)password; (void)serverKey;
  if (errorMsg) strcpy(errorMsg, "WinBolo.net not supported in WASM build");
  return FALSE;
}

void winboloNetGetServerKey(BYTE *keyBuff)                         { if (keyBuff) keyBuff[0] = '\0'; }
void winboloNetGetMyClientKey(BYTE *keyBuff)                       { if (keyBuff) keyBuff[0] = '\0'; }
bool winboloNetVerifyClientKey(BYTE *k, char *u, BYTE n)           { (void)k; (void)u; (void)n; return FALSE; }
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

void winboloNetSendVersion(void)                                   { }
void winboloNetSendLock(bool isLocked)                             { (void)isLocked; }
