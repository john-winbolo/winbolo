/*
 * Stub implementations of frontend / WinBoloNet / DNS / dialog
 * callbacks for the WinBoloUnitTests binary. Same surface as
 * braintest_frontend.c minus the threads stubs — the unit-test
 * binary links threads_static so cross-thread tests actually
 * serialise on a recursive SDL mutex.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "global.h"
#include "client_sim.h"
#include "frontend.h"
#include "server_sim.h"
#include "../../src/winbolonet/winbolonet_server.h"
#include "../../src/winbolonet/winbolonet_core.h"  /* WINBOLO_NET_EVENT_* */
#include "luabrainshandler.h"
#include "lang_message.h"
#include "nat_portmap.h"

bool isInMenu = FALSE;

/* Localized-string accessors live in src/gui/sdl3/lang.c, which we
 * don't link (it pulls SDL3-IO + INI parsing). bolo TUs sprinkle
 * langGetText / langGetTextFmt into message-formatting paths
 * (brain_data.c, client_sim.c, players.c, etc.). Return a fixed
 * placeholder so anything that actually reaches a message render in
 * the tests sees a non-NULL string rather than crashing. */
char       *langGetText(langid id);
const char *langGetTextFmt(langid id, const MessageArgs *args);

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
                            aiType aiMode, bool debug_mode,
                            int player_num) {
  (void)inst; (void)path; (void)name; (void)cs;
  (void)aiMode; (void)debug_mode; (void)player_num;
  return false;
}

bool luaBrainInstanceTick(LuaBrainInstance *inst) {
  (void)inst;
  return false;
}

void luaBrainInstanceDestroy(LuaBrainInstance *inst) {
  (void)inst;
}

/* Lua seeding (ec47fa43 / 73ba4e7a): the bot manager forwards -brain-lua-seed
 * here; nothing in the unit suite runs a Lua brain. */
void luaBrainSeedRandom(LuaBrainInstance *inst, long seed) {
  (void)inst; (void)seed;
}

void luaBrainSetDefaultRandomSeed(long seed) {
  (void)seed;
}

void luaBrainSetTickInputs(LuaBrainInstance *inst,
                           double lastThinkMs,
                           double targetMs,
                           bool   wasKilled,
                           int    tierOverride) {
  (void)inst; (void)lastThinkMs; (void)targetMs; (void)wasKilled; (void)tierOverride;
}

void luaBrainInstanceSetDebugMode(LuaBrainInstance *inst, bool enabled) {
  (void)inst; (void)enabled;
}

/* scenario.c's game.spawn_bot stages a per-bot BRAIN_INIT_ARG through this
 * before serverSimCreateBot. scenario.obj is pulled into the test binary by
 * server_sim_control.c's scenarioTick/scenarioReset calls, so the symbol has
 * to resolve even though no test spawns a scripted bot. */
void luaBrainsSetNextInitArg(const char *arg);
void luaBrainsSetNextInitArg(const char *arg) {
  (void)arg;
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

void frontEndUpdatePlayerPing(struct ClientSim *cs, playerNumbers value, uint16_t ping) {
  (void)cs; (void)value; (void)ping;
}

void frontEndUpdatePlayerFlags(struct ClientSim *cs, playerNumbers value, uint8_t clientType, uint8_t clientFlags) {
  (void)cs; (void)value; (void)clientType; (void)clientFlags;
}

void frontEndDrawDownload(ClientSim *cs, bool justBlack) {
  (void)cs; (void)justBlack;
}

void frontEndAudioReturningToLobby(bool active) {
  (void)active;
}

void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
  (void)cs; (void)value; (void)isChecked;
}

void frontEndApplyLocalTankPrefs(struct ClientSim *cs) { (void)cs; }

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
void dnsLookupsAddRequest(char *ip, void *func) { (void)ip; (void)func; }
void netRemovePlayer(BYTE playerNum) { (void)playerNum; }
void netRequestStartPosition(void) {}
void netErrorOccured(void) {}

/* Test-controllable WBN state. Defaults match a WBN-off run so every
 * other test is unaffected; the session-rotation test flips
 * wbnStubRunning on and watches wbnStubLobbyUpdateCalls to prove the
 * next round's map is never reported on the old server_key. */
bool wbnStubRunning = FALSE;
int  wbnStubLobbyUpdateCalls = 0;

/* winboloNetSendLock spy: the auto-lock-on-game-start tests watch these to
 * prove the lock state is reported to WinBolo.net (and with which value). */
int  wbnStubSendLockCalls = 0;
bool wbnStubLastLockReported = FALSE;

/* winbolonetAddEvent WIN spy: the game-over resolve tests watch these to
 * prove which side a finished round credited with the win (and that a
 * round nobody won credits nobody). Only WINBOLO_NET_EVENT_WIN is
 * recorded; every other event type stays unobserved as before. */
int      wbnStubWinEventCalls = 0;
uint16_t wbnStubWinEventMask  = 0;   /* bit i = a win credited to slot i */

bool winbolonetIsRunning(void) { return wbnStubRunning; }
void winbolonetDestroy(bool isServer) { (void)isServer; }
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB, bool aIsBot, bool bIsBot) {
  (void)isServer; (void)playerB; (void)aIsBot; (void)bIsBot;
  if (eventType == WINBOLO_NET_EVENT_WIN) {
    wbnStubWinEventCalls++;
    if (playerA < MAX_TANKS) {
      wbnStubWinEventMask |= (uint16_t)(1u << playerA);
    }
  }
}
void winboloNetGetServerKey(char *keyBuff) { if (keyBuff) keyBuff[0] = '\0'; }
void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
  (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}
void winboloNetSendLock(bool isLocked) {
  wbnStubSendLockCalls++;
  wbnStubLastLockReported = isLocked;
}
bool winboloNetIsPlayerParticipant(BYTE playerNum) { (void)playerNum; return FALSE; }
bool winboloNetVerifyClientKey(const char *playerKey, const char *playerName,
                               BYTE playerNum, char *errorMsg,
                               bool *hasSteam, bool *isSupporter) {
  (void)playerKey; (void)playerName; (void)playerNum; (void)errorMsg;
  if (hasSteam)    *hasSteam    = FALSE;
  if (isSupporter) *isSupporter = FALSE;
  return FALSE;
}
bool winboloNetVerifyJoinCode(const char *joinCode, char *playerNameOut,
                              bool *isLoggedInOut, char *countryOut,
                              int *userIdOut, char *errorMsg) {
  (void)joinCode;
  if (playerNameOut) playerNameOut[0] = '\0';
  if (isLoggedInOut) *isLoggedInOut   = FALSE;
  if (countryOut)    countryOut[0]    = '\0';
  if (userIdOut)    *userIdOut        = -1;
  if (errorMsg)      errorMsg[0]      = '\0';
  return FALSE;
}
bool winboloNetVerifySpectatorKey(const char *spectatorKey, const char *playerName,
                                  char *errorMsg, bool *isLoggedIn) {
  (void)spectatorKey; (void)playerName;
  if (errorMsg)    errorMsg[0]  = '\0';
  if (isLoggedIn) *isLoggedIn   = FALSE;
  return FALSE;
}
void winboloNetSpectatorLeaveGame(const char *spectatorKey) {
  (void)spectatorKey;
}

bool winbolonetClientJoinSession(const char *apiToken, const char *serverKey,
                                 char *playerKeyOut, char *errorMsg) {
  (void)apiToken; (void)serverKey;
  if (playerKeyOut) playerKeyOut[0] = '\0';
  if (errorMsg)     errorMsg[0]     = '\0';
  return FALSE;
}
bool winbolonetClientJoinSpectatorSession(const char *apiToken, const char *serverKey,
                                          const char *playerName, char *spectatorKeyOut,
                                          char *errorMsg) {
  (void)apiToken; (void)serverKey; (void)playerName;
  if (spectatorKeyOut) spectatorKeyOut[0] = '\0';
  if (errorMsg)        errorMsg[0]        = '\0';
  return FALSE;
}

bool winbolonetServerRequestBalance(uint8_t totalPlayers, uint8_t teamSize,
                                     const uint8_t *botSlots, uint8_t numBotSlots,
                                     BalanceProposal *outProposal) {
  (void)totalPlayers; (void)teamSize;
  (void)botSlots; (void)numBotSlots;
  (void)outProposal;
  return FALSE;
}

void winbolonetSendMapChange(char *mapName, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills) {
  (void)mapName; (void)numBases; (void)numPills; (void)freeBases; (void)freePills;
}

void winbolonetSetLobbyInfo(const WbnLobbyInfo *info) { (void)info; }

void winbolonetSendLobbyUpdate(void) { wbnStubLobbyUpdateCalls++; }

/* Stubbed because server_lifecycle.c references these but they live
 * outside server_static (per-target client networking dependencies:
 * winbolonet.c with its libcurl + tweetnacl chain, nat_portmap.c with
 * libplum). acceptRemoteClients=false makes the call paths unreachable
 * at test runtime. */

bool winbolonetCreateServer(char *mapName, unsigned short port, BYTE gameType, BYTE ai,
                            bool mines, bool password, BYTE numBases, BYTE numPills,
                            BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
  (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers;
  return FALSE;
}

void winbolonetEndSession(void) { }

bool winbolonetBeginSession(char *mapName, unsigned short port, BYTE gameType, BYTE ai,
                            bool mines, bool password, BYTE numBases, BYTE numPills,
                            BYTE freeBases, BYTE freePills, BYTE numPlayers) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
  (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers;
  return FALSE;
}

void winbolonetSendLobbyStatus(bool inLobby) { (void)inLobby; }

void winbolonetServerUpdate(BYTE numPlayers, BYTE numFreeBases, BYTE numFreePills, bool sendNow) {
  (void)numPlayers; (void)numFreeBases; (void)numFreePills; (void)sendNow;
}

void natPortMapRequest(unsigned short internalPort, NatPortMap *out) {
  if (out) {
    memset(out, 0, sizeof(*out));
    out->internalPort = internalPort;
  }
}

void natPortMapRelease(NatPortMap *map)       { (void)map; }
void natPortMapRenewIfNeeded(NatPortMap *map) { (void)map; }

/* The cloud-prefs transport lives in http.c (libcurl + tweetnacl). The
 * test links wbn_prefs_sync.c standalone, whose wbnPrefsSyncOnce references
 * these two symbols; stubbing them here keeps http.o (and curl) out of the
 * link. wbnPrefsSyncOnce itself is never exercised by the unit tests — only
 * the pure helpers (wbnPrefsBuildPutBody, the parsers, the decision) are. */
int wbn_prefs_get(const char *bearerToken, char **response_out) {
  (void)bearerToken;
  if (response_out) *response_out = NULL;
  return -1;
}

int wbn_prefs_put(const char *bearerToken, const char *json_body,
                  char **response_out) {
  (void)bearerToken; (void)json_body;
  if (response_out) *response_out = NULL;
  return -1;
}

/* The public game-list transport lives in http.c; stubbed here to keep curl
 * out of the link. wbnFetchServerList is never exercised by the unit tests —
 * only the pure wbnServerListParse is. */
int wbn_api_get_public(const char *path, char **response_out) {
  (void)path;
  if (response_out) *response_out = NULL;
  return -1;
}

/* POST transport lives in http.c; stubbed to keep curl out of the link.
 * wbnMapFetchByMd5 is never exercised by the unit tests — only the pure
 * wbnMapParseResponse is. */
int wbn_api_post(const char *endpoint, const char *json_body, char **response_out) {
  (void)endpoint; (void)json_body;
  if (response_out) *response_out = NULL;
  return -1;
}

/* GET transport lives in http.c; stubbed to keep curl out of the link. The
 * wbn_news worker routes through a test seam (wbn_news_set_api_get_for_test),
 * so test_wbn_news_fetch never calls this — it is only the link-time default
 * the seam's function pointer is initialised to. */
int wbn_api_get(const char *path, char **response_out) {
  (void)path;
  if (response_out) *response_out = NULL;
  return -1;
}
