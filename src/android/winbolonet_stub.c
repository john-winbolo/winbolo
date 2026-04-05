/*
 * winbolonet_stub.c - Stub implementations of WinBolo.net functions
 *
 * On Android we don't include the winbolonet source files (which depend
 * on libcurl). Instead we stub out all functions declared in winbolonet.h.
 */

#include "../bolo/global.h"

bool winbolonetCreateServer(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
  (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers;
  return FALSE;
}

bool winbolonetCreateClient(const char *token, const char *serverKey, char *errorMsg) {
  (void)token; (void)serverKey; (void)errorMsg;
  return FALSE;
}

void winbolonetDestroy(bool isServer) { (void)isServer; }

void winboloNetGetServerKey(char *keyBuff) {
  if (keyBuff) keyBuff[0] = '\0';
}

void winboloNetGetMyClientKey(char *keyBuff) {
  if (keyBuff) keyBuff[0] = '\0';
}

bool winboloNetVerifyClientKey(const char *playerKey, char *userName, BYTE playerNum) {
  (void)playerKey; (void)userName; (void)playerNum;
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

void winboloNetSendLock(bool isLocked) {
  (void)isLocked;
}

bool winbolonetServerVerifyToken(const char *token, BYTE playerNum, char *errorMsg, bool *hasSteam) {
  (void)token; (void)playerNum; (void)errorMsg;
  if (hasSteam) *hasSteam = FALSE;
  return FALSE;
}

bool winbolonetAuthLogin(const char *username, const char *password, char *tokenOut, char *expiryOut, char *playerNameOut, char *errorMsg) {
  (void)username; (void)password; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)errorMsg;
  return FALSE;
}

bool winbolonetAuthSteam(const char *steamTicketHex, char *tokenOut, char *expiryOut, char *playerNameOut, char *errorMsg) {
  (void)steamTicketHex; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)errorMsg;
  return FALSE;
}

bool winbolonetAuthValidate(const char *token, char *playerNameOut, char *errorMsg) {
  (void)token; (void)playerNameOut; (void)errorMsg;
  return FALSE;
}
