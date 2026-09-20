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
#include "client_sim_internal.h"  /* clientSimGetBoundServerSim — the brain stub */
#include "frontend.h"
#include "server_sim.h"
#include "../../src/winbolonet/winbolonet_server.h"
#include "../../src/winbolonet/winbolonet_core.h"  /* WINBOLO_NET_EVENT_* */
#include "../../src/winbolonet/winbolonetthread.h" /* WbnResultHandler,
                                                    WBN_JOB_REGISTER,
                                                    WBN_JOB_VERIFY */
#include "luabrainshandler.h"
#include "lang_message.h"
#include "nat_portmap.h"
#include "test_harness.h"  /* ut_sound_* — the played-sound recorder below */

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
/* The create stub doubles as a fixture brain. It refuses by default, which
 * is what every test that never meant to build a bot wants and what the
 * unit binary did before it could be armed. A test that drives a bot all
 * the way through botManagerAddBot arms it, and it then reports success and
 * writes down what its bot was made with: the init table, which is the last
 * thing the sim does with that table before a real brain VM would read it,
 * and the team the slot held at that moment, which is the team the add had
 * already picked the slot's lobby start from. */
static bool     s_brainStubArmed = false;
static ScnTable s_brainStubInit[MAX_TANKS];
static bool     s_brainStubMade[MAX_TANKS];
static BYTE     s_brainStubTeam[MAX_TANKS];
/* How many brains each slot has been asked for, and how many have been
 * destroyed in all. s_brainStubMade answers whether a slot ever had one,
 * which cannot tell a seat fielded once from a seat fielded, taken off the
 * field and fielded again — the count can. The destroys are a total rather
 * than per slot because luaBrainInstanceDestroy is handed an instance and no
 * player number, so there is nothing to file them under. */
static int      s_brainStubCreates[MAX_TANKS];
static int      s_brainStubDestroys;

void ut_brain_stub_arm(bool succeed) {
  s_brainStubArmed = succeed;
  memset(s_brainStubInit, 0, sizeof(s_brainStubInit));
  memset(s_brainStubMade, 0, sizeof(s_brainStubMade));
  memset(s_brainStubTeam, 0, sizeof(s_brainStubTeam));
  memset(s_brainStubCreates, 0, sizeof(s_brainStubCreates));
  s_brainStubDestroys = 0;
}

int ut_brain_stub_team(int player_num) {
  if (player_num < 0 || player_num >= MAX_TANKS) return 0;
  return (int)s_brainStubTeam[player_num];
}

bool ut_brain_stub_made(int player_num) {
  if (player_num < 0 || player_num >= MAX_TANKS) return false;
  return s_brainStubMade[player_num];
}

const ScnTable *ut_brain_stub_init(int player_num) {
  if (player_num < 0 || player_num >= MAX_TANKS) return NULL;
  if (!s_brainStubMade[player_num]) return NULL;
  return &s_brainStubInit[player_num];
}

int ut_brain_stub_creates(int player_num) {
  if (player_num < 0 || player_num >= MAX_TANKS) return 0;
  return s_brainStubCreates[player_num];
}

int ut_brain_stub_destroys(void) {
  return s_brainStubDestroys;
}

bool luaBrainInstanceCreate(LuaBrainInstance *inst, const char *path,
                            const char *name, struct ClientSim *cs,
                            aiType aiMode, bool debug_mode,
                            int player_num, const ScnTable *init) {
  (void)path; (void)name; (void)aiMode; (void)debug_mode;
  if (!s_brainStubArmed) {
    (void)cs; (void)player_num; (void)init;
    return false;
  }
  /* Zeroed the way the real create leaves an instance it is about to fill:
   * bot_manager reads L, pathfinder and worldsim straight after this and
   * skips each one when it is NULL. */
  memset(inst, 0, sizeof(*inst));
  if (player_num >= 0 && player_num < MAX_TANKS) {
    struct ServerSim *sim = cs ? clientSimGetBoundServerSim(cs) : NULL;
    s_brainStubMade[player_num] = true;
    s_brainStubCreates[player_num]++;
    if (init != NULL) {
      s_brainStubInit[player_num] = *init;
    } else {
      memset(&s_brainStubInit[player_num], 0, sizeof(s_brainStubInit[0]));
    }
    /* The team on the slot as the brain is made. serverSimAddBot has
     * already run by this point in the add, so this is the team it wrote
     * and picked the slot's lobby start from — a caller that sets the team
     * after the add records 0 here. */
    if (sim != NULL) {
      const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, (BYTE)player_num);
      s_brainStubTeam[player_num] = lp ? lp->teamNumber : 0;
    }
  }
  return true;
}

bool luaBrainInstanceTick(LuaBrainInstance *inst) {
  (void)inst;
  return false;
}

/* Counted whether or not a brain was ever made for the instance.
 * botTearDownRunner calls this for every runner it takes down, and the count
 * is of the work the teardown asks for. */
void luaBrainInstanceDestroy(LuaBrainInstance *inst) {
  (void)inst;
  s_brainStubDestroys++;
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

/* A per-bot BRAIN_INIT_ARG is staged through this before serverSimCreateBot
 * (servermain.c and braintest_main.c both do it). luabrainshandler.c is a
 * GUI-side file that the test binary does not link, so the symbol has to
 * resolve here even though no test spawns a bot with an init arg. */
void luaBrainsSetNextInitArg(const char *arg);
void luaBrainsSetNextInitArg(const char *arg) {
  (void)arg;
}

/* bot_manager reads the staged arg back so it can APPEND its
 * "difficulty=<word>" token instead of clobbering a -bot-init suffix.
 * Nothing is staged in the test binary, so this reports the empty string —
 * the same answer the real one gives an unstaged create. */
const char *luaBrainsPeekNextInitArg(void);
const char *luaBrainsPeekNextInitArg(void) {
  return "";
}

/* bot_manager.c seeds a freshly created brain's tick counter from the engine
 * tick through this, the same way it stages the init arg above. No test
 * creates a bot brain, so the symbol only has to resolve. */
void luaBrainsSetNextStartEngineTick(unsigned int tick);
void luaBrainsSetNextStartEngineTick(unsigned int tick) {
  (void)tick;
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

/* What frontEndPlaySound was handed, so a test can assert which variant of a
   sound the client played. Bounded: once the array is full, further sounds are
   neither stored nor counted. */
#define UT_SOUND_MAX 64
static int s_ut_sounds[UT_SOUND_MAX];
static int s_ut_sound_count;

void ut_sound_reset(void) {
  s_ut_sound_count = 0;
}

int ut_sound_count(void) {
  return s_ut_sound_count;
}

int ut_sound_get(int index) {
  if (index < 0 || index >= s_ut_sound_count) {
    return -1;
  }
  return s_ut_sounds[index];
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
  (void)cs;
  if (s_ut_sound_count < UT_SOUND_MAX) {
    s_ut_sounds[s_ut_sound_count] = (int)value;
    s_ut_sound_count++;
  }
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

/* What frontEndStatusTank was last handed. playersSetPlayer works out the
   status tile for a join or a rename itself and hands it straight to the front
   end, so this is the only place a test can read the answer it came to. The
   player number is the 1-based one the call takes; -1 until the first call. */
static int s_ut_statusTankPlayer   = -1;
static int s_ut_statusTankAlliance = -1;

int ut_status_tank_last_player(void) {
  return s_ut_statusTankPlayer;
}

int ut_status_tank_last_alliance(void) {
  return s_ut_statusTankAlliance;
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
  (void)cs;
  s_ut_statusTankPlayer   = (int)tankNum;
  s_ut_statusTankAlliance = (int)ts;
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

/* winbolonetAddEvent TANK_KILL spy: the tank-arm tests watch these to prove a
 * scripted kill is reported the way any other kill is, and with which slots. */
int  wbnStubKillEventCalls = 0;
BYTE wbnStubLastKiller = 0xFF;
BYTE wbnStubLastKilled = 0xFF;

/* winbolonetAddEvent LGM_LOST / LGM_KILL spies: the builder-arm tests watch
 * these to prove a scripted builder death is reported the way an explosion's
 * is, and that a death nobody caused credits nobody with the kill. */
int  wbnStubLgmLostCalls = 0;
BYTE wbnStubLastLgmLost = 0xFF;
int  wbnStubLgmKillCalls = 0;
BYTE wbnStubLastLgmKiller = 0xFF;
BYTE wbnStubLastLgmKilled = 0xFF;

/* The per-slot WinBolo.net keys the real winbolonet_core owns. Empty until a
 * verify is accepted, which only winbolonetApplyVerifyResult below does, so
 * every case that never settles a verify reads exactly as it did: no key, and
 * winboloNetIsPlayerParticipant FALSE. */
char wbnStubPlayerKey[MAX_TANKS][WINBOLONET_KEY_LEN];

/* winbolonetAddEvent PLAYER_JOIN spy: the reauth cases watch these to prove
 * a verified re-authentication emits the deferred join exactly once, and for
 * which slot.
 *
 * wbnStubLastJoinKey is the slot's key AT PUBLISH TIME, which is what says
 * whether the join announced an account or an unidentified player: the real
 * winbolonetAddEvent reads winboloNetPlayerKey[playerA] when it is called and
 * winbolonetServerUpdate leaves player_a out when that key is empty. Empty
 * here means the join went out un-keyed. */
int      wbnStubJoinEventCalls = 0;
uint16_t wbnStubJoinEventMask  = 0;   /* bit i = a join emitted for slot i */
char     wbnStubLastJoinKey[WINBOLONET_KEY_LEN] = "";

bool winbolonetIsRunning(void) { return wbnStubRunning; }
void winbolonetDestroy(bool isServer) { (void)isServer; }
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB, bool aIsBot, bool bIsBot) {
  (void)isServer; (void)aIsBot; (void)bIsBot;
  if (eventType == WINBOLO_NET_EVENT_WIN) {
    wbnStubWinEventCalls++;
    if (playerA < MAX_TANKS) {
      wbnStubWinEventMask |= (uint16_t)(1u << playerA);
    }
  } else if (eventType == WINBOLO_NET_EVENT_TANK_KILL) {
    wbnStubKillEventCalls++;
    wbnStubLastKiller = playerA;
    wbnStubLastKilled = playerB;
  } else if (eventType == WINBOLO_NET_EVENT_LGM_LOST) {
    wbnStubLgmLostCalls++;
    wbnStubLastLgmLost = playerA;
  } else if (eventType == WINBOLO_NET_EVENT_LGM_KILL) {
    wbnStubLgmKillCalls++;
    wbnStubLastLgmKiller = playerA;
    wbnStubLastLgmKilled = playerB;
  } else if (eventType == WINBOLO_NET_EVENT_PLAYER_JOIN) {
    wbnStubJoinEventCalls++;
    wbnStubLastJoinKey[0] = '\0';
    if (playerA < MAX_TANKS) {
      wbnStubJoinEventMask |= (uint16_t)(1u << playerA);
      SDL_strlcpy(wbnStubLastJoinKey, wbnStubPlayerKey[playerA],
                  WINBOLONET_KEY_LEN);
    }
  }
}
/* The session key the server is holding. Empty unless a test sets one, which
 * keeps every case that never looks at it reading exactly as it did. The
 * round-transition case needs a non-empty one: transportUdpServerSendWbnRekey
 * drops the broadcast when the server has no key. */
char wbnStubServerKey[WINBOLONET_KEY_LEN] = "";

void winboloNetGetServerKey(char *keyBuff) {
  if (keyBuff) {
    SDL_strlcpy(keyBuff, wbnStubServerKey, WINBOLONET_KEY_LEN);
  }
}
void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
  (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}
void winboloNetSendLock(bool isLocked) {
  wbnStubSendLockCalls++;
  wbnStubLastLockReported = isLocked;
}
bool winboloNetIsPlayerParticipant(BYTE playerNum) {
  if (playerNum >= MAX_TANKS) return FALSE;
  return wbnStubPlayerKey[playerNum][0] != '\0' ? TRUE : FALSE;
}
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

/* ── Round-transition queue spy (test_round_transition_tick.c) ──────────
 * The lifecycle queues server/quit, the round-log upload and
 * server/register for the WinBolo.net worker instead of posting them, and
 * picks the register's reply up later through winbolonetThreadDrainResults.
 * These record what was queued and in what order, and hold the reply until
 * a test releases it. */
#define WBN_STUB_MAX_JOBS 8
int      wbnStubJobCount = 0;
char     wbnStubJobs[WBN_STUB_MAX_JOBS][16];
uint32_t wbnStubRegisterJobId = 0;
bool     wbnStubRegisterResultReady = FALSE;
int      wbnStubRegisterResultStatus = 200;
int      wbnStubApplyRegisterCalls = 0;
bool     wbnStubApplyRegisterOk = TRUE;

static uint32_t wbnStubNextJobId = 1;

static uint32_t wbnStubRecordJob(const char *what) {
  if (wbnStubJobCount >= 0 && wbnStubJobCount < WBN_STUB_MAX_JOBS) {
    SDL_strlcpy(wbnStubJobs[wbnStubJobCount], what,
                sizeof(wbnStubJobs[wbnStubJobCount]));
  }
  wbnStubJobCount++;
  return wbnStubNextJobId++;
}

void winbolonetQueueEndSession(void) {
  wbnStubRecordJob("quit");
}

uint32_t winbolonetQueueBeginSession(char *mapName, unsigned short port,
                                     BYTE gameType, BYTE ai, bool mines,
                                     bool password, BYTE numBases,
                                     BYTE numPills, BYTE freeBases,
                                     BYTE freePills, BYTE numPlayers) {
  (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines;
  (void)password; (void)numBases; (void)numPills; (void)freeBases;
  (void)freePills; (void)numPlayers;
  wbnStubRegisterJobId = wbnStubRecordJob("register");
  return wbnStubRegisterJobId;
}

bool winbolonetApplyRegisterResult(int status, const char *response) {
  (void)status; (void)response;
  wbnStubApplyRegisterCalls++;
  return wbnStubApplyRegisterOk;
}

/* ── client/verify queue spy (test_reauth_result.c) ─────────────────────
 * The reauth path queues client/verify for the worker and stamps the slot
 * from udpServerApplyReauthResult when the reply lands. These record the
 * enqueue, hold the reply until a test releases it, and decide what the
 * reply says.
 *
 * wbnStubVerifyOk defaults FALSE, which is what winboloNetVerifyClientKey
 * has always answered here, so a case that never sets it sees the failure
 * arm exactly as before. */
int      wbnStubVerifyQueueCalls = 0;
uint32_t wbnStubVerifyJobId = 0;
char     wbnStubVerifyLastKey[96] = "";   /* >= WBN_JOIN_KEY_WIRE_LEN */
char     wbnStubVerifyLastName[96] = "";  /* >= PACKET_MAX_PLAYER_NAME */
bool     wbnStubVerifyResultReady = FALSE;
int      wbnStubVerifyResultStatus = 200;
int      wbnStubApplyVerifyCalls = 0;
bool     wbnStubVerifyOk = FALSE;
bool     wbnStubVerifyHasSteam = FALSE;
bool     wbnStubVerifySupporter = FALSE;

uint32_t winbolonetQueueVerifyClientKey(const char *playerKey,
                                        const char *playerName) {
  wbnStubVerifyQueueCalls++;
  SDL_strlcpy(wbnStubVerifyLastKey, playerKey ? playerKey : "",
              sizeof(wbnStubVerifyLastKey));
  SDL_strlcpy(wbnStubVerifyLastName, playerName ? playerName : "",
              sizeof(wbnStubVerifyLastName));
  wbnStubVerifyJobId = wbnStubNextJobId++;
  return wbnStubVerifyJobId;
}

bool winbolonetApplyVerifyResult(int status, const char *response,
                                 const char *playerKey, BYTE playerNum,
                                 char *errorMsg, bool *hasSteam,
                                 bool *isSupporter) {
  (void)status; (void)response;
  wbnStubApplyVerifyCalls++;
  if (errorMsg)    errorMsg[0]  = '\0';
  if (hasSteam)    *hasSteam    = wbnStubVerifyHasSteam;
  if (isSupporter) *isSupporter = wbnStubVerifySupporter;
  if (wbnStubVerifyOk != TRUE) {
    if (errorMsg) SDL_strlcpy(errorMsg, "stub verify refused", 256);
    return FALSE;
  }
  /* The real apply writes the slot's key here, on the draining thread. */
  if (playerNum < MAX_TANKS) {
    SDL_strlcpy(wbnStubPlayerKey[playerNum], playerKey ? playerKey : "",
                WINBOLONET_KEY_LEN);
  }
  return TRUE;
}

uint32_t winbolonetThreadAddUpload(const char *fileName, const char *key) {
  (void)fileName; (void)key;
  return wbnStubRecordJob("upload");
}

/* ── client/verify_join_code queue spy (test_reauth_result.c) ───────────
 * A web slot's reauth presents a join_code and takes the read-only
 * verify_join_code route, queued for the worker like the key route and
 * placed by the same handler. Shares wbnStubVerifyJobId and the verify
 * result-ready flag, so the drain below delivers either kind.
 *
 * wbnStubJoinCodeOk defaults FALSE, which is what winboloNetVerifyJoinCode
 * has always answered here. */
int      wbnStubJoinCodeQueueCalls = 0;
char     wbnStubJoinCodeLast[128] = "";
int      wbnStubApplyJoinCodeCalls = 0;
bool     wbnStubJoinCodeOk = FALSE;
bool     wbnStubJoinCodeLoggedIn = FALSE;
char     wbnStubJoinCodeName[PACKET_MAX_PLAYER_NAME] = "";
char     wbnStubJoinCodeCountry[3] = "";

uint32_t winbolonetQueueVerifyJoinCode(const char *joinCode) {
  wbnStubJoinCodeQueueCalls++;
  SDL_strlcpy(wbnStubJoinCodeLast, joinCode ? joinCode : "",
              sizeof(wbnStubJoinCodeLast));
  wbnStubVerifyJobId = wbnStubNextJobId++;
  return wbnStubVerifyJobId;
}

bool winbolonetApplyVerifyJoinCodeResult(int status, const char *response,
                                         char *playerNameOut,
                                         bool *isLoggedInOut,
                                         char *countryOut, int *userIdOut,
                                         char *errorMsg) {
  (void)status; (void)response;
  wbnStubApplyJoinCodeCalls++;
  if (playerNameOut) playerNameOut[0] = '\0';
  if (isLoggedInOut) *isLoggedInOut   = FALSE;
  if (countryOut)    countryOut[0]    = '\0';
  if (userIdOut)    *userIdOut        = -1;
  if (errorMsg)      errorMsg[0]      = '\0';
  if (wbnStubJoinCodeOk != TRUE) {
    if (errorMsg) SDL_strlcpy(errorMsg, "stub join code refused", 256);
    return FALSE;
  }
  if (playerNameOut) {
    SDL_strlcpy(playerNameOut, wbnStubJoinCodeName, PACKET_MAX_PLAYER_NAME);
  }
  if (isLoggedInOut) *isLoggedInOut = wbnStubJoinCodeLoggedIn;
  if (countryOut)    SDL_strlcpy(countryOut, wbnStubJoinCodeCountry, 3);
  if (userIdOut)    *userIdOut = 4242;
  return TRUE;
}

void winbolonetThreadDrainResults(WbnResultHandler handler, void *ctx) {
  if (handler == NULL) {
    return;
  }
  if (wbnStubRegisterResultReady == TRUE) {
    wbnStubRegisterResultReady = FALSE;
    handler(wbnStubRegisterJobId, WBN_JOB_REGISTER, wbnStubRegisterResultStatus,
            "{}", ctx);
  }
  if (wbnStubVerifyResultReady == TRUE) {
    wbnStubVerifyResultReady = FALSE;
    handler(wbnStubVerifyJobId, WBN_JOB_VERIFY, wbnStubVerifyResultStatus,
            "{}", ctx);
  }
}

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
