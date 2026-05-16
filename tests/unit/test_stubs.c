/*
 * Stub implementations of frontend / WinBoloNet / DNS / dialog
 * callbacks for the WinBoloUnitTests binary. Same surface as
 * braintest_frontend.c minus the threads stubs — the unit-test
 * binary links the real src/server/threads.c so cross-thread tests
 * actually serialise on a recursive SDL mutex.
 */

#include <stdio.h>
#include <SDL3/SDL.h>
#include "global.h"
#include "client_sim.h"
#include "frontend.h"
#include "server_sim.h"
#include "luabrainshandler.h"
#include "../../src/gui/lang.h"

bool isInMenu = FALSE;

/* Localized-string accessors live in src/gui/sdl3/lang.c, which we
 * don't link (it pulls SDL3-IO + INI parsing). bolo TUs sprinkle
 * langGetText / langGetTextFmt into message-formatting paths
 * (brain_data.c, client_sim.c, players.c, etc.). Return a fixed
 * placeholder so anything that actually reaches a message render in
 * the tests sees a non-NULL string rather than crashing. */
static char s_lang_placeholder[] = "?";

char *langGetText(langid id) {
  (void)id;
  return s_lang_placeholder;
}

const char *langGetTextFmt(langid id, const MessageArgs *args) {
  (void)id; (void)args;
  return s_lang_placeholder;
}

/* bot_manager.c (linked from bolo_static) references these five
 * Lua-brain entry points. None are reachable at runtime because the
 * unit tests never call serverSimBotPoolInit / serverSimAddBot, so
 * link-time stubs are sufficient — and they let us avoid pulling
 * luabrainshandler.c (and its clientmutex.c / gamefront.c / lang.c
 * dependency closure) into the test binary. */
bool luaBrainInstanceCreate(LuaBrainInstance *inst, const char *path,
                            const char *name, struct ClientSim *cs,
                            aiType aiMode, bool debug_mode) {
  (void)inst; (void)path; (void)name; (void)cs;
  (void)aiMode; (void)debug_mode;
  return false;
}

bool luaBrainInstanceTick(LuaBrainInstance *inst) {
  (void)inst;
  return false;
}

void luaBrainInstanceDestroy(LuaBrainInstance *inst) {
  (void)inst;
}

void luaBrainSetTickInputs(LuaBrainInstance *inst,
                           double lastThinkMs,
                           double targetMs,
                           bool   wasKilled) {
  (void)inst; (void)lastThinkMs; (void)targetMs; (void)wasKilled;
}

void luaBrainInstanceSetDebugMode(LuaBrainInstance *inst, bool enabled) {
  (void)inst; (void)enabled;
}

/* client_sim.c::netProcessedDnsLookup pairs clientMutexWaitFor with
 * clientMutexRelease around a player-location write. The tests never
 * exercise the DNS-lookup completion path, but the symbols still need
 * to resolve at link time. Stubbed empty rather than linking
 * src/gui/sdl3/clientmutex.c (which would also drag the SDL3 GUI's
 * gamefront/lang dependency closure). */
bool clientMutexCreate(void)        { return TRUE; }
void clientMutexDestroy(void)       {}
void clientMutexWaitFor(void)       {}
bool clientMutexTryWaitFor(void)    { return TRUE; }
void clientMutexRelease(void)       {}

void frontEndUpdateTankStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  (void)cs; (void)shells; (void)mines; (void)armour; (void)trees;
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
  (void)cs; (void)shells; (void)mines; (void)armour;
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
  (void)cs; (void)value;
}

void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView,
                            int edgeX, int edgeY) {
  (void)cs; (void)value; (void)mineView; (void)tks; (void)gs; (void)sBullet;
  (void)lgms; (void)srtDelay; (void)isPillView;
  (void)edgeX; (void)edgeY;
}

void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
  (void)cs; (void)pillNum; (void)pb;
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
  (void)cs; (void)tankNum; (void)ts;
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
  (void)cs; (void)baseNum; (void)bs;
}

void frontEndMessages(ClientSim *cs, char *top, char *bottom) {
  (void)cs; (void)top; (void)bottom;
}

void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) {
  (void)cs; (void)kills; (void)deaths;
}

void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) {
  (void)cs; (void)isDead; (void)angle;
}

void frontEndManClear(ClientSim *cs) {
  (void)cs;
}

void frontEndGameOver(ClientSim *cs) {
  (void)cs;
}

void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) {
  (void)cs; (void)value;
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) {
  (void)cs; (void)value; (void)str; (void)countryCode; (void)ping; (void)clientType; (void)clientFlags;
}

void frontEndDrawDownload(ClientSim *cs, bool justBlack) {
  (void)cs; (void)justBlack;
}

void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
  (void)cs; (void)value; (void)isChecked;
}

void frontEndSetActiveClientSim(struct ClientSim *cs) {
  (void)cs;
}

void frontEndEnableRequestAllyMenu(bool enabled) {
  (void)enabled;
}

void frontEndEnableLeaveAllyMenu(bool enabled) {
  (void)enabled;
}

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
  (void)cs; (void)isShown;
}

void frontEndRedrawAll(ClientSim *cs) {
  (void)cs;
}

bool frontEndTutorial(BYTE pos) {
  (void)pos;
  return FALSE;
}

void frontEndTutorialReset(void) { }

void windowRedrawAll(ClientSim *cs) {
  (void)cs;
}

bool windowShowAllianceRequest(void) {
  return FALSE;
}

void windowAllowPlayerNameChange(bool allow) {
  (void)allow;
}

bool soundSetup(void) {
  return TRUE;
}

void soundCleanup(void) {
}

void soundPlayEffect(sndEffects value) {
  (void)value;
}

void soundKeepalive(bool on) {
  (void)on;
}

bool soundIsPlayingEffect(sndEffects value) {
  (void)value;
  return FALSE;
}

void gameFrontReloadSkins(void) {
}

void gameFrontShutdownServer(void) {
}

void *dialogAllianceCreate(void) {
  return NULL;
}

void dialogAllianceDestroy(void *dlg) {
  (void)dlg;
}

void dialogAllianceSetName(char *playerName, BYTE playerNum) {
  (void)playerName; (void)playerNum;
}

void moveMousePointer(updateType value) {
  (void)value;
}

bool dnsLookupsCreate(ClientSim *cs) { (void)cs; return TRUE; }
void dnsLookupsDestroy(void) {}
void netRemovePlayer(BYTE playerNum) { (void)playerNum; }
void netRequestStartPosition(void) {}
void netErrorOccured(void) {}

bool winbolonetIsRunning(void) { return FALSE; }
void winbolonetDestroy(bool isServer) { (void)isServer; }
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB) {
  (void)eventType; (void)isServer; (void)playerA; (void)playerB;
}
void winboloNetGetServerKey(char *keyBuff) { if (keyBuff) keyBuff[0] = '\0'; }
void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
  (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}
void winboloNetSendLock(bool isLocked) { (void)isLocked; }
bool winboloNetIsPlayerParticipant(BYTE playerNum) { (void)playerNum; return FALSE; }
bool winbolonetServerVerifyToken(const char *token, BYTE playerNum, char *errorMsg,
                                 bool *hasSteam, bool *isSupporter) {
  (void)token; (void)playerNum; (void)errorMsg;
  if (hasSteam)    *hasSteam    = FALSE;
  if (isSupporter) *isSupporter = FALSE;
  return FALSE;
}

bool winbolonetServerRequestBalance(uint8_t totalPlayers, uint8_t teamSize, BalanceProposal *outProposal) {
  (void)totalPlayers; (void)teamSize; (void)outProposal;
  return FALSE;
}

void winbolonetSendMapChange(char *mapName, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills) {
  (void)mapName; (void)numBases; (void)numPills; (void)freeBases; (void)freePills;
}
