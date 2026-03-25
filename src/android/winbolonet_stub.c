/*
 * winbolonet_stub.c - Stub implementations of WinBolo.net functions
 *
 * On Android we don't include the winbolonet source files (which depend
 * on libcurl). Instead we stub out all functions declared in winbolonet.h.
 */

#include "../bolo/global.h"

bool winbolonetCreateServer(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers, long startTime) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
  (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers; (void)startTime;
  return FALSE;
}

bool winbolonetCreateClient(char *userName, char *password, BYTE *serverKey, char *errorMsg) {
  (void)userName; (void)password; (void)serverKey; (void)errorMsg;
  return FALSE;
}

void winbolonetDestroy(bool isServer) { (void)isServer; }

bool winbolonetRequestServerKey(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers, long startTime) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
  (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers; (void)startTime;
  return FALSE;
}

bool winbolonetRequestClientKey(char *userName, char *password, BYTE *serverKey, char *errorMsg) {
  (void)userName; (void)password; (void)serverKey; (void)errorMsg;
  return FALSE;
}

void winboloNetGetServerKey(BYTE *keyBuff) {
  (void)keyBuff;
}

void winboloNetGetMyClientKey(BYTE *keyBuff) {
  (void)keyBuff;
}

bool winboloNetVerifyClientKey(BYTE *keyBuff, char *userName, BYTE playerNum) {
  (void)keyBuff; (void)userName; (void)playerNum;
  return FALSE;
}

void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
  (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}

void winbolonetGoodbye(void) { }

void winbolonetServerSendTeams(BYTE *array, BYTE length, BYTE numTeams) {
  (void)array; (void)length; (void)numTeams;
}

void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB) {
  (void)eventType; (void)isServer; (void)playerA; (void)playerB;
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

void winboloNetSendVersion(void) { }

void winboloNetSendLock(bool isLocked) {
  (void)isLocked;
}
