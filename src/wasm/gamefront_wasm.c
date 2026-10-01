/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * gamefront_wasm.c - WASM game frontend (skips all dialogs)
 *
 * Replaces gui/sdl3/gamefront.c for the WASM build.
 * Instead of showing dialogs, immediately starts
 * a single-player practice game on the built-in map.
 */

#include <SDL3/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include <emscripten.h>
#include <emscripten/html5.h>

#include "bolo_rand.h"
#include "client_frontend_connect.h"
#include "client_frontend_sp.h"
#include "client_sim.h"
#include "control_event.h"
#include "global.h"
#include "platform_net.h"
#include "client_net.h"
#include "gui_message.h"
#include "everard_map.h"
#include "frontend.h"
#include "server_sim.h"
#include "../server/server_lifecycle.h"
#include "../scenario/scenario_host.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../gui/gamefront.h"
#include "../gui/input.h"
#include "../gui/lang.h"
#include "../gui/sound.h"
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/bg_game.h"
#include "../winbolonet/winbolonet_core.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/dialogs/imgui_mapchooser.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../gui/sdl3/input_gamepad.h"
#include "../gui/sdl3/build_cursor.h"
#include "../gui/voice.h"
#include "../common/prefs.h"
#include "cJSON.h"
#include "gamefront_wasm.h"

/* Forward declaration */
extern void sdl3MessageHandler(const char *message, const char *title);
extern void wasmReportConnectFailure(const char *reason);  /* main_wasm.c */
extern void wbPrefsPumpUpload(uint64_t nowMs);  /* prefs_bridge_wasm.c */

/* Mint a fresh single-use join code from the reusable game_key by awaiting
 * Module.wbMintJoinCode (POST /api/join, cookie-authed) via ASYNCIFY — mirrors
 * prefs_bridge_wasm.c's fetch helpers. Writes the code into out (up to outSize)
 * on HTTP 200; on any other outcome writes a malloc'd reason string through
 * *errOut (caller frees) when the backend supplied one. Returns the HTTP
 * status, or -1 on a transport error. */
EM_ASYNC_JS(int, wasmMintJoinCode,
            (const char *gameKey, char *out, int outSize, char **errOut), {
    try {
        const r = await Module.wbMintJoinCode(UTF8ToString(gameKey));
        if (r && r.status === 200 && typeof r.code === 'string') {
            stringToUTF8(r.code, out, outSize);
        } else if (r && typeof r.error === 'string') {
            const len = lengthBytesUTF8(r.error) + 1;
            const p = _malloc(len);
            stringToUTF8(r.error, p, len);
            setValue(errOut, p, '*');
        }
        return (r && r.status) ? r.status : -1;
    } catch (e) {
        console.error('[join] mint failed', e);
        return -1;
    }
});

/* Default user-facing message for a mint failure when the backend gave no
 * error body of its own. */
static const char *gameFrontMintFailReason(int status) {
  switch (status) {
    case 401: return langGetText(STR_WEB_JOIN_NEED_SIGNIN);
    case 403: return langGetText(STR_WEB_JOIN_NOT_ACCEPTING);
    case 404: return langGetText(STR_WEB_JOIN_LINK_INVALID);
    case 409: return langGetText(STR_WEB_JOIN_GAME_FULL);
    default:  return langGetText(STR_WEB_JOIN_CODE_UNREACHABLE);
  }
}


/* Parse "server=host:port" from a proxy URL query string.
 * Fills serverHost (up to hostSize bytes) and *serverPort. */
static void parseProxyServerParam(const char *proxyUrl,
                                   char *serverHost, int hostSize,
                                   unsigned short *serverPort) {
  const char *sp = strstr(proxyUrl, "server=");
  if (!sp) {
    strncpy(serverHost, "localhost", (size_t)hostSize - 1);
    serverHost[hostSize - 1] = '\0';
    *serverPort = 27500;
    return;
  }
  sp += 7; /* skip "server=" */

  /* Copy until '&' or end */
  char tmp[256] = "";
  int i = 0;
  while (sp[i] && sp[i] != '&' && i < (int)sizeof(tmp) - 1) {
    tmp[i] = sp[i];
    i++;
  }
  tmp[i] = '\0';

  /* Split on the LAST ':' (handles IPv6-style if needed) */
  char *colon = strrchr(tmp, ':');
  if (colon && colon != tmp) {
    *colon = '\0';
    strncpy(serverHost, tmp, (size_t)hostSize - 1);
    serverHost[hostSize - 1] = '\0';
    *serverPort = (unsigned short)atoi(colon + 1);
  } else {
    strncpy(serverHost, tmp, (size_t)hostSize - 1);
    serverHost[hostSize - 1] = '\0';
    *serverPort = 27500;
  }
}

/* -------------------------------------------------------
 * Game playing options (global state)
 * ------------------------------------------------------- */
char fileName[FILENAME_MAX];
char password[MAP_STR_SIZE];
bool hiddenMines;
aiType compTanks = aiNone;
gameType gametype;
int32_t startDelay;
int32_t timeLen;
bool gameFrontRemeber;

/* UDP stuff */
char gameFrontName[PLAYER_NAME_LEN];
char gameFrontUdpAddress[FILENAME_MAX];
unsigned short gameFrontMyUdp;
unsigned short gameFrontTargetUdp;

char gameFrontTrackerAddr[FILENAME_MAX];
unsigned short gameFrontTrackerPort;
bool gameFrontTrackerEnabled;

/* Winbolo.net settings */
char gameFrontWbnToken[FILENAME_MAX];
char gameFrontWbnTokenExpiry[FILENAME_MAX];
bool gameFrontWbnUse;
static int gameFrontWbnRank = -1;
static int gameFrontWbnRankTotal = 0;
static WbnStats gameFrontWbnStats;
static char gameFrontWbnAuthMethod[32];
static bool gameFrontWbnSignedOut;

/* Dialog states */
openingStates dlgState = openStart;

/* Saved dialog position (-1 = no saved position). The wasm build never
 * reads or persists these, but the shared SDL3 dialog code references
 * the symbols. */
int gameFrontDialogX = -1;
int gameFrontDialogY = -1;

/* Lobby players/map divider offsets ([WINDOW] section on desktop). The wasm
 * build never persists window settings, but the shared lobby dialog code
 * (imgui_lobby.cpp) references the symbols. */
float gameFrontLobbySplit = 0.0f;
float gameFrontLobbySplitRecap = 0.0f;

/* Map overview pop-out geometry and camera state ([WINDOW] section on
   desktop). The wasm build never opens the pop-out — a browser tab has no
   second OS window — but sdl3imgui.cpp is shared and references the
   symbols. */
int   gameFrontOverviewW = 640;
int   gameFrontOverviewH = 640;
int   gameFrontOverviewX = -1;
int   gameFrontOverviewY = -1;
float gameFrontOverviewZoom = 1.0f;
bool  gameFrontOverviewFollow = TRUE;
bool  gameFrontShowMapOverview = FALSE;
bool  gameFrontFullScreen = FALSE;

/* Scenario panel position, size and opacity (-1 = never moved/resized/set;
 * [WINDOW] section on desktop). The wasm build never persists window
 * settings, but the shared scenario panel in sdl3imgui.cpp reads and writes
 * the symbols. */
int gameFrontScnPanelX = -1;
int gameFrontScnPanelY = -1;
int gameFrontScnPanelScale = -1;
int gameFrontScnPanelAlpha = -1;

bool isServer = FALSE;
bool useAutoslow;
bool useAutohide;
bool wantRejoin;

/* NAT/UPnP toggles — wasm client never hosts a server, but the shared
 * settings dialog references these globals. */
bool gameFrontUseUpnp = FALSE;
bool gameFrontUseNatTraversal = FALSE;

/* Client-hosting settings ([HOSTING] section). A browser tab cannot listen
 * for connections, so nothing here is ever applied to a server — the values
 * exist because the shared settings and UDP-setup dialogs read them. The
 * defaults mirror gamefront.c so anything that echoes one shows the number
 * the desktop build would. */
unsigned short gameFrontHostingPort             = 27500;
bool           gameFrontHostingAllowSpec        = TRUE;
bool           gameFrontHostingScripts          = TRUE;
int            gameFrontHostingMaxSpec          = 16;
int            gameFrontHostingUploadPolicy     = UPLOAD_POLICY_ALLOW;
int            gameFrontHostingUploadMaxFiles   = 64;
int            gameFrontHostingUploadMaxStorage = 8;
char           gameFrontHostingUploadDir[FILENAME_MAX] = "";
int            gameFrontHostingScriptUploadPolicy     = SCRIPT_UPLOAD_ALLOW;
bool           gameFrontHostingShareScripts           = TRUE;
int            gameFrontHostingScriptUploadMaxFiles   = 32;
int            gameFrontHostingScriptUploadMaxStorage = 64;
char           gameFrontHostingScriptUploadDir[FILENAME_MAX] = "";
bool           gameFrontHostingLogging          = TRUE;
char           gameFrontHostingLogDir[FILENAME_MAX] = "";
bool           gameFrontHostingServeReplays     = TRUE;
int            gameFrontHostingVoiceMode        = serverVoiceOn;

/* Visibility rules a hosted game starts with. Same story as the hosting
 * knobs above — held for the dialogs, never applied to a server here.
 * The stock set from view_policy.h, matching serverSimInit. */
int gameFrontViewPillPolicy    = VIEW_POLICY_STOCK_PILL;
int gameFrontViewBasePolicy    = VIEW_POLICY_STOCK_BASE;
int gameFrontViewAllyPolicy    = VIEW_POLICY_STOCK_ALLY;
int gameFrontViewPillDecaySecs = VIEW_DECAY_DEFAULT_SECS;
int gameFrontViewBaseDecaySecs = VIEW_DECAY_DEFAULT_SECS;
int gameFrontViewAllyDecaySecs = VIEW_DECAY_DEFAULT_SECS;
bool gameFrontClassicMode      = FALSE;
bool gameFrontAlliesInTrees    = FALSE;
bool gameFrontPositionalSound  = FALSE;
int gameFrontOverviewWindow    = OVERVIEW_WINDOW_STOCK;
int gameFrontLineOfSight       = LINE_OF_SIGHT_STOCK;

/* Server-authoritative state — the Transport handle itself now lives
 * inside humanSim; only high-level lifecycle gating is tracked here. */
static ServerSim *wasmServerSim = NULL;
/* The practice game's scenario: the script beside its map, or the mods picked
 * in its lobby. Follows the committed map, and is detached before the sim
 * goes. NULL for a map with no script, and always for the tutorial. */
static ScenarioHost *wasmScenarioHost = NULL;
static bool wasmTransportActive = FALSE;
static SubscriberHandle wasmControlSub = SUBSCRIBER_HANDLE_INVALID;

ClientSim *humanSim = NULL;

extern bool isTutorial;

/* Used to set the preferences — defined in main_wasm.c */
extern int frameRate;
extern bool showGunsight;
extern bool soundEffects;
extern bool backgroundSound;
extern bool useSoundKeepalive;
extern bool showNewswireMessages;
extern bool showAssistantMessages;
extern bool showAIMessages;
extern bool showNetworkStatusMessages;
extern bool showNetworkDebugMessages;
extern bool autoScrollingEnabled;
extern BYTE zoomFactor;
extern bool showPillLabels;
extern bool showBaseLabels;
extern bool labelSelf;
extern labelLen labelMsg;
extern labelLen labelTank;
extern int soundVolume;
extern int windowMasterVolume;
bool windowGetShowTankMicIcons(void);

/* -------------------------------------------------------
 * Default key setup
 * ------------------------------------------------------- */
static void gameFrontSetDefaultKeys(keyItems *keys) {
  keys->kiForward      = DEFAULT_FORWARD;
  keys->kiBackward     = DEFAULT_BACKWARD;
  keys->kiLeft         = DEFAULT_LEFT;
  keys->kiRight        = DEFAULT_RIGHT;
  keys->kiShoot        = DEFAULT_SHOOT;
  keys->kiLayMine      = DEFAULT_LAY_MINE;
  keys->kiGunIncrease  = DEFAULT_SCROLL_GUNINCREASE;
  keys->kiGunDecrease  = DEFAULT_SCROLL_GUNDECREASE;
  keys->kiTankView     = DEFAULT_TANKVIEW;
  keys->kiPillView     = DEFAULT_PILLVIEW;
  keys->kiAllyView     = DEFAULT_ALLYVIEW;
  keys->kiBaseView     = DEFAULT_BASEVIEW;
  keys->kiOverviewZoom = DEFAULT_OVERVIEW_ZOOM;
  keys->kiOverviewFollow  = DEFAULT_OVERVIEW_FOLLOW;
  keys->kiOverviewZoomIn  = DEFAULT_OVERVIEW_ZOOMIN;
  keys->kiOverviewZoomOut = DEFAULT_OVERVIEW_ZOOMOUT;
  keys->kiScrollUp     = DEFAULT_SCROLLUP;
  keys->kiScrollDown   = DEFAULT_SCROLLDOWN;
  keys->kiScrollLeft   = DEFAULT_SCROLLLEFT;
  keys->kiScrollRight  = DEFAULT_SCROLLRIGHT;
  keys->kiQuickTree    = DEFAULT_QUICKTREE;
  keys->kiQuickRoad    = DEFAULT_QUICKROAD;
  keys->kiQuickWall    = DEFAULT_QUICKWALL;
  keys->kiQuickPillbox = DEFAULT_QUICKPILLBOX;
  keys->kiQuickMine    = DEFAULT_QUICKMINE;
  keys->kiPushToTalk   = DEFAULT_PUSHTOTALK;
  keys->kiMuteMic      = DEFAULT_MUTEMIC;
  /* Smart-ping chord slots. The web client runs the same pie menu as the
     desktop (ping_overlay.cpp is in the wasm build and shared sdl3imgui.cpp
     drives it), so it starts on the same chords: Ctrl and Alt with the right
     mouse button open the menu, the third slot is spare. */
  keys->kiPing[0]      = DEFAULT_PING1;
  keys->kiPing[1]      = DEFAULT_PING2;
  keys->kiPing[2]      = DEFAULT_PING3;
  /* The direct pings are all unbound out of the box, on the desktop and
     here alike. */
  {
    int pi;
    for (pi = 0; pi < PING_BIND_DIRECT_SLOTS; pi++) {
      keys->kiPingDirect[pi] = 0;
    }
  }
}

/* Outbound control-event callbacks. Desktop wires these in gamefront.c; the
 * web client previously wired none, so accepting/requesting/leaving an
 * alliance, the lock toggle, and name change were all silent no-ops (the send
 * path itself works fine over the relay transport — only the callback was
 * unset). Each mirrors its desktop counterpart. */
static void wasmNameChangeSendCallback(const char *newName) {
  clientSimNetSendNameChange(humanSim, newName);
}
static void wasmAllianceRequestCallback(uint8_t toPlayer) {
  clientSimNetSendAllianceRequest(humanSim, toPlayer);
}
static void wasmAllianceAcceptCallback(uint8_t toPlayer) {
  clientSimNetSendAllianceAccept(humanSim, toPlayer);
}
static void wasmAllianceLeaveCallback(void) {
  clientSimNetSendAllianceLeave(humanSim);
}
static void wasmLockToggleCallback(bool allow) {
  clientSimNetSendLockToggle(humanSim, allow);
}

/* Ask the browser for the join password with window.prompt, the only
 * blocking text entry this build has before the game loop runs. wrongBefore
 * adds the incorrect-password line above the request. Returns FALSE on
 * Cancel and leaves the password global as it was; otherwise the entry
 * replaces it. The reply comes back with a one-letter prefix so an empty
 * entry and a cancelled prompt read differently. */
static bool wasmAskJoinPassword(bool wrongBefore) {
  char msg[512];
  char escaped[1024];
  /* Room for a fully escaped msg plus the wrapper, so a long localised
   * message cannot truncate the script mid string literal. */
  char js[sizeof(escaped) + 96];
  const char *reply;
  size_t i, o = 0;
  if (wrongBefore) {
    snprintf(msg, sizeof(msg), "%s\n\n%s",
             langGetText(NETERR_PASSWORDWRONG),
             langGetText(STR_DLGPASSWORD_BLURB));
  } else {
    snprintf(msg, sizeof(msg), "%s", langGetText(STR_DLGPASSWORD_BLURB));
  }
  /* JS string literal escape: backslash, quote, newline. */
  for (i = 0; msg[i] != '\0' && o + 2 < sizeof(escaped); i++) {
    char c = msg[i];
    if (c == '\\' || c == '\'') { escaped[o++] = '\\'; escaped[o++] = c; }
    else if (c == '\n') { escaped[o++] = '\\'; escaped[o++] = 'n'; }
    else if (c == '\r') { /* dropped */ }
    else escaped[o++] = c;
  }
  escaped[o] = '\0';
  snprintf(js, sizeof(js),
    "(function(){ var r = window.prompt('%s'); return r === null ? 'C' : 'P' + r; })()",
    escaped);
  reply = emscripten_run_script_string(js);
  if (reply == NULL || reply[0] != 'P') return FALSE;
  strncpy(password, reply + 1, sizeof(password) - 1);
  password[sizeof(password) - 1] = '\0';
  return TRUE;
}

/* Pick a random .map file from the preloaded data/maps for the menu's
 * background game, leaving out the same two maps the desktop's pick does
 * (the tutorial map and Better Best Map Ever). */
static bool wasmPickBackgroundMap(char *out, size_t outLen) {
  const char *dir = "data/maps";
  int count = 0;
  int filtered = 0;
  int i;
  char **list = SDL_GlobDirectory(dir, "*.map", 0, &count);
  if (list == NULL || count == 0) {
    printf("[WASM] background game: no maps in '%s'\n", dir);
    SDL_free(list);
    return FALSE;
  }
  for (i = 0; i < count; i++) {
    if (SDL_strcasecmp(list[i], "Inbuilt Tutorial.map") != 0 &&
        SDL_strcasecmp(list[i], "Better Best Map Ever.map") != 0) {
      list[filtered++] = list[i];
    }
  }
  if (filtered == 0) {
    printf("[WASM] background game: no non-tutorial maps in '%s'\n", dir);
    SDL_free(list);
    return FALSE;
  }
  SDL_snprintf(out, outLen, "%s/%s", dir,
               list[bolo_rand_below((uint32_t)filtered)]);
  SDL_free(list);
  return TRUE;
}

/* Find the brain practice bots run, trying the same paths in the same order
 * as the desktop's single-player lookup (gamefront.c findBrainPath). The web
 * preloads /Brains/GoalHunter_1.7, which the first path finds. */
static bool wasmFindBrainPath(char *out, size_t outLen) {
  const char *candidates[] = {
    "Brains/GoalHunter_1.7/init.lua",
    "brains/GoalHunter_1.7/init.lua",
    "data/Brains/GoalHunter_1.7/init.lua",
  };
  int i;
  for (i = 0; i < 3; i++) {
    FILE *f = fopen(candidates[i], "r");
    if (f) {
      fclose(f);
      snprintf(out, outLen, "%s", candidates[i]);
      return TRUE;
    }
  }
  return FALSE;
}

/* Seed the practice lobby with one enemy bot, as desktop single player does
 * when there is no saved bot setup: the player on team 1, "Bot 1" in slot 1
 * on team 2. The bot's steps are the shared seeding body the desktop's
 * single-player start calls too (client_frontend_sp.c). No bot with the AI
 * policy on none or no brain found.
 *
 * scriptSeats is a lobby the map's script lays out itself (Survival seats its
 * whole horde). As on desktop, no bot is made there, since it would take a
 * slot ahead of the script's seats; the player still takes their team and the
 * alliance pass still runs. */
static void wasmSeedPracticeBot(const char *brainPath, bool scriptSeats) {
  const BYTE slot       = 1;
  const BYTE botTeam    = 2;
  const BYTE playerTeam = 1;
  uint8_t spMode, spLevel;

  if (compTanks == aiNone || brainPath[0] == '\0') return;

  if (scriptSeats) {
    clientSimNetSendTeamSet(humanSim, 0, playerTeam);
    serverSimReapplyTeamAlliances(wasmServerSim);
    return;
  }

  spMode  = gameFrontSpBotMode(brainPath);
  spLevel = gameFrontSpBotLevel(brainPath, spMode);
  clientFrontSeedBot(wasmServerSim, humanSim, slot, brainPath, "Bot 1",
                     compTanks, gametype, hiddenMines, botTeam,
                     spMode, spLevel);
  clientSimNetSendTeamSet(humanSim, 0, playerTeam);
  /* The alliance pass at lobby entry saw an empty lobby; run it again now
   * that the player and the bot have teams. */
  serverSimReapplyTeamAlliances(wasmServerSim);
}

/* -------------------------------------------------------
 * gameFrontWasmSetup — page-lifetime setup, run once per page
 * ------------------------------------------------------- */
bool gameFrontWasmSetup(keyItems *keys) {
  /* Seed the player name. main_wasm.c chooses the single-player name after
   * this, and a join chooses its network name before it connects; this seed
   * only matters to any path that reads the name before then. */
  strcpy(gameFrontName, "Me");
  strcpy(gameFrontTrackerAddr, TRACKER_ADDRESS);
  gameFrontTrackerPort = TRACKER_PORT;
  gameFrontTrackerEnabled = FALSE;
  gameFrontWbnToken[0] = '\0';
  gameFrontWbnTokenExpiry[0] = '\0';
  gameFrontWbnUse = FALSE;
  gameFrontRemeber = FALSE;

  useAutoslow = FALSE;
  useAutohide = FALSE;

  /* Seed default keys. A logged-in player's stored bindings (and the rest of
   * their synced settings) are applied afterwards by wasmApplyJoinPrefs, and
   * no game start seeds them again, so they last for the page. */
  gameFrontSetDefaultKeys(keys);

  langSetup();

  /* Initialise subsystems */
  {
    /* Tell SDL3 to use the existing canvas element from shell.html */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, "#canvas");

    /* For custom mode, use ceiling integer zoom so render target >= window.
       sdl3DrawAdaptRenderTarget will adjust dynamically on resize. */
    BYTE zf = windowGetZoomFactor();
    if (zf == ZOOM_FACTOR_CUSTOM) zf = ZOOM_FACTOR_DOUBLE;
    if (sdl3DrawSetup(zf) == FALSE) {
      printf("[WASM] sdl3DrawSetup FAILED\n");
      return FALSE;
    }
    printf("[WASM] sdl3DrawSetup OK\n");

    if (soundSetup() == FALSE) {
      soundEffects = FALSE;
    }

    brainsHandlerLoadBrains();

    /* The shared background game the menu and its dialogs draw behind
     * them, made once for the page. Not fatal when it fails: the menu is
     * then drawn plain. main_wasm.c hides it while a game runs; the page
     * never frees it. */
    {
      BgGame *bg = (BgGame *)SDL_calloc(1, sizeof(BgGame));
      if (bg != NULL) {
        char mapPath[512];
        if (wasmPickBackgroundMap(mapPath, sizeof(mapPath)) &&
            bgGameCreate(bg, mapPath, sdl3DrawGetRenderer())) {
          bgGameSetShared(bg);
        } else {
          SDL_free(bg);
        }
      }
    }
  }

  guiMessageSetHandler(sdl3MessageHandler);
  return TRUE;
}

/* -------------------------------------------------------
 * gameFrontWasmStart — skip all dialogs, start the launch's game
 * ------------------------------------------------------- */
bool gameFrontWasmStart(const char *cmdLine, keyItems *keys,
                        const WasmLaunch *launch) {
  (void)keys;

  /* Per-game state, reset for each game. */
  isTutorial = FALSE;
  password[0] = '\0';
  wantRejoin = FALSE;
  gameFrontUdpAddress[0] = '\0';
  gameFrontMyUdp = 27500;
  gameFrontTargetUdp = 27500;

  /* Default game options */
  gametype = gameOpen;
  hiddenMines = FALSE;
  compTanks = aiNone;
  startDelay = 0;
  timeLen = UNLIMITED_GAME_TIME;

  /* Process command line */
  if (cmdLine != NULL && cmdLine[0] != '\0') {
    strncpy(fileName, cmdLine, FILENAME_MAX - 1);
    fileName[FILENAME_MAX - 1] = '\0';
  } else {
    fileName[0] = '\0';
  }

  /* Start the tutorial step sequencer from the first step, whatever the
   * mode, so a game never inherits the last one's step or frame count. */
  frontEndTutorialReset();

  /* ---- Determine net mode from the launch ----
   * Production web play is selected by ?game_key= (the shareable
   * play.winbolo.net/join/<game_key> link). A dev/LAN run may instead pass an
   * explicit ?proxyURL=. Either selects UDP-over-WebSocket mode. The single-use
   * join code is minted from the game_key at connect time (JS POST /api/join),
   * not carried in the URL. shell.html points Module.websocket.url at the real
   * relay, so the host:port handed to the transport here is an ignored
   * sentinel — routing lives in the minted join code (or the dev proxy URL).
   * main_wasm.c read these from the URL once, at page start. */
  netType urlNetType = netSingle;
  const char *gameKey = launch->gameKey;
  const char *devProxy = launch->devProxy;
  bool wantTutorial = (launch->mode == WASM_GAME_TUTORIAL);
  bool haveGameKey = (gameKey[0] != '\0');
  if (haveGameKey || devProxy[0] != '\0') {
    urlNetType = netUdp;
  }

  if (urlNetType == netUdp) {
    if (haveGameKey) {
      /* Production: routing rides Module.websocket.url (the join code JS mints
       * from the game_key); the transport target is a sentinel the relay never
       * sees. Use a loopback literal so no DNS lookup is attempted. */
      printf("[WASM] netUdp mode: web play via relay (game_key)\n");
      strncpy(gameFrontUdpAddress, "127.0.0.1", sizeof(gameFrontUdpAddress) - 1);
      gameFrontUdpAddress[sizeof(gameFrontUdpAddress) - 1] = '\0';
      gameFrontTargetUdp = 1;
    } else {
      /* Dev/LAN: explicit proxyURL. shell.html uses it verbatim as the WS
       * URL; any server=host:port within it is informational only. */
      char serverHost[256];
      unsigned short serverPort = 27500;
      parseProxyServerParam(devProxy, serverHost, sizeof(serverHost), &serverPort);
      printf("[WASM] netUdp mode (dev proxy): %s -> %s:%d\n",
             devProxy, serverHost, serverPort);
      strncpy(gameFrontUdpAddress, serverHost, sizeof(gameFrontUdpAddress) - 1);
      gameFrontUdpAddress[sizeof(gameFrontUdpAddress) - 1] = '\0';
      gameFrontTargetUdp = serverPort;
    }
  }

  /* Start the game directly — no dialogs */
  printf("[WASM] Setting up screen...\n");
  humanSim = clientSimAlloc();
  clientSimCreate(humanSim);
  frontEndSetActiveClientSim(humanSim);

  if (urlNetType == netUdp) {
    /* ---- UDP multiplayer via new transport ---- */
    printf("[WASM] Connecting via UDP transport...\n");
    /* For web play the join_code rides the WBN-token argument: the client
     * has no libcurl to mint a player_key, so it presents the join_code raw
     * at PACKET_WBN_REAUTH (see udpClientSendWbnReauth). A non-empty token
     * also sets JOIN_FLAG_WILL_AUTHENTICATE, which the server needs to send
     * the first REKEY. Dev/LAN proxy runs keep the normal WBN token. */
    /* Network identity. Prefer the logged-in WinBolo.net account name that
     * shell.html probed from /api/v1/me (window.WB_PREFS_NAME); fall back to a
     * throwaway web<rand> placeholder so a stale name can't ride the JOIN. This
     * MUST run before clientSimConnectUdp so the real name rides the first JOIN
     * instead of the "Me" default. The server re-verifies the join_code at
     * re-auth and stays authoritative. */
    {
      const char *jsName =
          emscripten_run_script_string("(window.WB_PREFS_NAME||'')");
      if (jsName != NULL && jsName[0] != '\0') {
        strncpy(gameFrontName, jsName, sizeof(gameFrontName) - 1);
        gameFrontName[sizeof(gameFrontName) - 1] = '\0';
      } else {
        unsigned suffix = (unsigned)(emscripten_get_now() * 1000.0) % 1000000u;
        snprintf(gameFrontName, sizeof(gameFrontName), "web%u", suffix);
      }
      /* Assert the name locally so the player panel shows it immediately,
       * before the server's re-auth rename lands. */
      clientSimSetMyLastPlayerName(humanSim, gameFrontName);
      printf("[WASM] network play: join name=%s\n", gameFrontName);
    }

    /* Join password. A ?password= parameter on the launch URL seeds it (a
     * dev/LAN proxy link can carry one); otherwise it starts empty and an
     * incorrect-password reject below asks through the browser and retries.
     * The loop only repeats for that retry. */
    {
      strncpy(password, launch->password, sizeof(password) - 1);
      password[sizeof(password) - 1] = '\0';
    }
    for (;;) {
    /* Mint the single-use join code from the game_key just before connecting.
     * A fresh code is minted on every (re)connect: a page refresh or relay
     * failover re-runs this path and mints again, so a consumed code is never
     * reused. The mint blocks here via ASYNCIFY; on failure we surface the
     * reason and bail into the error dialog rather than connecting anonymously. */
    char joinCode[128] = "";
    if (haveGameKey) {
      char *mintErr = NULL;
      int mintStatus = wasmMintJoinCode(gameKey, joinCode, sizeof(joinCode),
                                        &mintErr);
      if (mintStatus != 200 || joinCode[0] == '\0') {
        const char *reason = (mintErr && mintErr[0] != '\0')
                           ? mintErr : gameFrontMintFailReason(mintStatus);
        printf("[WASM] join-code mint failed (status %d): %s\n",
               mintStatus, reason);
        wasmReportConnectFailure(reason);
        free(mintErr);
        return FALSE;
      }
      free(mintErr);
    }

    const char *wbnArg = haveGameKey ? joinCode
                       : (gameFrontWbnUse ? gameFrontWbnToken : "");
    clientSimConnectUdp(humanSim, gameFrontUdpAddress,
                        gameFrontTargetUdp,
                        gameFrontName,
                        winbolonetGetCountryCode(),
                        password,
                        wbnArg,
                        "",
                        wantRejoin,
                        "", 0, /*spectator*/ false);
    if (clientSimGetConnectState(humanSim) == CLIENT_CONNECT_ERROR) {
      const char *reason = clientSimGetConnectErrorReason(humanSim);
      printf("[WASM] UDP connect failed: %s\n", reason ? reason : "unknown");
      /* Keep humanSim alive in its error state; main() falls through to the
       * loop which shows the error dialog (never a blank screen). */
      wasmReportConnectFailure(reason && reason[0]
                                   ? reason
                                   : langGetText(STR_WEB_CONNECT_FAILED));
      return FALSE;
    }

    /* Wait for the join handshake (30s timeout). Landing accepts either a
     * running game or entry into the server lobby (the production join_code
     * path lands in the in-game lobby) — see clientFrontAwaitJoin. */
    if (!clientFrontAwaitJoin(humanSim, 1500)) {
      const char *reason = clientSimGetConnectErrorReason(humanSim);
      printf("[WASM] Join failed: %s\n", reason ? reason : "timeout");
      if (clientSimGetConnectErrorLangId(humanSim) == STR_REJECT_INCORRECT_PASSWORD) {
        /* The first reject means the game has a password; a later one
         * means the entry was wrong. Retry on a fresh ClientSim, with a
         * fresh join code: the rejected request never reached re-auth,
         * but the mint is cheap and a new code is always valid. */
        bool wrongBefore = (password[0] != '\0');
        if (wasmAskJoinPassword(wrongBefore)) {
          clientSimDisconnect(humanSim);
          clientSimDestroy(humanSim);
          humanSim = clientSimAlloc();
          clientSimCreate(humanSim);
          frontEndSetActiveClientSim(humanSim);
          clientSimSetMyLastPlayerName(humanSim, gameFrontName);
          continue;
        }
      }
      wasmReportConnectFailure(
          reason && reason[0] ? reason
                              : langGetText(STR_WEB_JOIN_NO_RESPONSE));
      return FALSE;
    }
    break;
    }

    wasmTransportActive = TRUE;

    /* Store server address in ClientSim for brain info */
    {
      struct sockaddr_in saddr;
      memset(&saddr, 0, sizeof(saddr));
      saddr.sin_family = AF_INET;
      saddr.sin_addr.s_addr = inet_addr(gameFrontUdpAddress);
      if (saddr.sin_addr.s_addr == INADDR_NONE) {
        saddr.sin_addr.s_addr = 0;
      }
      clientSimSetServerAddress(humanSim, saddr.sin_addr);
      clientSimSetServerPort(humanSim, gameFrontTargetUdp);
    }

    /* Wire the outbound control-event send callbacks (desktop does this in
     * gamefront.c). Without these, alliance accept/request/leave, lock toggle,
     * and name change silently do nothing in web play. */
    clientSimSetNameChangeSendFunc(humanSim, wasmNameChangeSendCallback);
    clientSimSetAllianceRequestFunc(humanSim, wasmAllianceRequestCallback);
    clientSimSetAllianceAcceptFunc(humanSim, wasmAllianceAcceptCallback);
    clientSimSetAllianceLeaveFunc(humanSim, wasmAllianceLeaveCallback);
    clientSimSetLockToggleSendFunc(humanSim, wasmLockToggleCallback);

    /* Map install + snapshot apply happen inside the UDP transport
     * (MAP_DOWNLOAD inline install + CTRL_GAME_PHASE LOBBY→RUNNING watcher),
     * and the first snapshot apply fires the viewport finalisation. The lobby
     * vs running landing (netLobby) is settled by clientFrontAwaitJoin, and the
     * mapDownloadComplete flag stays transport-driven, so nothing to do here. */
    printf("[WASM] UDP connected as player %d\n",
           clientSimGetServerPlayerNum(humanSim));
  } else {
    /* ---- Single-player via ServerSim + local transport ---- */
    printf("[WASM] Setting up single-player ServerSim...\n");

    if (wantTutorial) {
      /* Guided tutorial (?tutorial=): load the inbuilt tutorial map with
       * tournament rules and no bots, mirroring the desktop openTutorial
       * path. The step driver + overlay (main_wasm.c) take it from here. */
      strncpy(fileName, "data/maps/Inbuilt Tutorial.map", FILENAME_MAX - 1);
      fileName[FILENAME_MAX - 1] = '\0';
      gametype = gameStrictTournament;
      hiddenMines = FALSE;
      startDelay = 0;
      timeLen = UNLIMITED_GAME_TIME;
      compTanks = aiNone;
      isTutorial = TRUE;
      printf("[WASM] starting guided tutorial\n");
    }

    /* Whether the sim was built from fileName, which is the only map a
     * script can sit beside: the built-in map has no file on disk. */
    bool mapFromFile = FALSE;
    {
      if (fileName[0] != '\0') {
        wasmServerSim = serverSimCreate(fileName, gametype, hiddenMines, startDelay, timeLen);
        if (wasmServerSim == NULL) {
          printf("[WASM] Failed to load map '%s' into ServerSim, trying built-in\n", fileName);
        } else {
          mapFromFile = TRUE;
        }
      }
      if (wasmServerSim == NULL) {
        BYTE emap[6000] = E_MAP;
        wasmServerSim = serverSimCreateCompressed(emap, E_MAP_LEN, "EverardIsland", gametype, hiddenMines, startDelay, timeLen);
      }
      if (wasmServerSim == NULL) {
        printf("[WASM] Failed to create ServerSim\n");
        clientSimDestroy(humanSim);
        return FALSE;
      }
    }

    if (!wantTutorial) {
      /* The scenario steps desktop single player takes after it creates its
       * sim (gamefront.c), in the same order: the scripts preferences set on
       * the library before the attach, so the map commits that follow answer
       * to them too; the map chooser's scripted question and the mods
       * listing registered on the sim whatever map is hosted; the script
       * beside this map attached; and from then on the scenario follows the
       * committed map, which is how a scripted map or a mod picked in the
       * lobby takes effect. The mods listed are the ones shipped in
       * /data/mods (the preloaded data directory). There is no Workshop, no
       * Mod Dir preference and no uploads directory here, so those are left
       * unset. The tutorial takes none of this. */
      scenarioHostSetEnabled(gameFrontHostingScripts);
      scenarioHostSetUploadScriptsEnabled(
          gameFrontHostingScriptUploadPolicy != SCRIPT_UPLOAD_OFF);
      scenarioHostRegisterMapScripted(wasmServerSim);
      scenarioHostRegisterScenarioLister(wasmServerSim);
      if (mapFromFile && strncmp(fileName, "randommap:", 10) != 0) {
        char scenarioErr[512];
        wasmScenarioHost = scenarioHostAttach(wasmServerSim, fileName,
                                              scenarioErr, sizeof(scenarioErr));
        if (wasmScenarioHost != NULL) {
          printf("[WASM] Scenario loaded: %s (from %s)\n",
                 scenarioHostName(wasmScenarioHost),
                 scenarioHostScriptPath(wasmScenarioHost));
        } else if (scenarioErr[0] != '\0') {
          printf("[WASM] %s\n", scenarioErr);
        }
      }
      scenarioHostFollowMap(wasmServerSim, &wasmScenarioHost);
    }

    if (wantTutorial) {
      /* Enter tutorial mode BEFORE the tank spawns (serverInstanceStartup +
       * clientSimConnectLocal below). startsGetStart reads sim->isTutorial and
       * sim->tutorialStartIdx at spawn time, so the initial placement at start
       * 0 (held until the boat-building step) depends on these being set
       * first. Seed start idx 0 explicitly; the driver bumps it to 1 after the
       * boat step. Mark both sims so tank.c's stop logic fires and the client
       * predicts consistently. */
      serverSimSetTutorial(wasmServerSim, true);
      serverSimSetTutorialStartIdx(wasmServerSim, 0);
      clientSimSetTutorial(humanSim, true);
    }

    /* The brain practice bots run. Resolved once for the config's bot brain
     * and the starting bot below. */
    char spBrainPath[FILENAME_MAX] = "";
    if (!wantTutorial) {
      wasmFindBrainPath(spBrainPath, sizeof(spBrainPath));
    }

    /* acceptRemoteClients, WinBolo.net, the tracker and NAT stay zero-init
     * off, so the network bring-up is skipped. The tutorial skips the lobby
     * and runs immediately (skipLobby drives the StartGameInPlace
     * transition); practice opens the lobby as desktop single player does,
     * with the desktop's no-prefs AI policy, Full Advantage. viewPlayer 0 is
     * the SP convention. */
    {
      ServerInstanceConfig cfg;
      memset(&cfg, 0, sizeof(cfg));
      if (wantTutorial) {
        cfg.skipLobby = true;
      } else {
        compTanks             = aiFull;
        cfg.password          = password;
        cfg.maxPlayers        = MAX_TANKS;
        cfg.compTanks         = (BYTE)compTanks;
        cfg.lobbyEnabled      = true;
        cfg.emptyResetEnabled = true;
        cfg.hasPassword       = (password[0] != '\0');
        cfg.botBrainPath      = (spBrainPath[0] != '\0') ? spBrainPath : NULL;
        cfg.botAiType         = (BYTE)compTanks;
      }
      if (!serverInstanceStartup(wasmServerSim, &cfg)) {
        printf("[WASM] serverInstanceStartup failed\n");
        scenarioHostDetach(wasmScenarioHost);
        wasmScenarioHost = NULL;
        serverSimDestroy(wasmServerSim);
        wasmServerSim = NULL;
        clientSimDestroy(humanSim);
        return FALSE;
      }
    }

    /* The local join below carries gameFrontName to the server. main_wasm.c
     * chose the single-player name (a validated ?name= or "Me") before this
     * start ran. */

    /* Run the 12-step join+install in one call. The tutorial's transport
     * ticks the server itself; practice's is passive, as desktop single
     * player's is, and main_wasm.c ticks the server through
     * serverInstanceTick. */
    bool connected = wantTutorial
        ? clientSimConnectLocal(humanSim, wasmServerSim,
                                gameFrontName, "", 0, 0)
        : clientSimConnectLocalPassive(humanSim, wasmServerSim,
                                       gameFrontName, "", 0, 0);
    if (!connected) {
      printf("[WASM] local connect failed: %s\n",
             clientSimGetConnectErrorReason(humanSim));
      scenarioHostDetach(wasmScenarioHost);
      wasmScenarioHost = NULL;
      serverSimDestroy(wasmServerSim);
      wasmServerSim = NULL;
      clientSimDestroy(humanSim);
      return FALSE;
    }
    wasmTransportActive = TRUE;
    /* Session-type flag for the lobby/UI (hide multiplayer-only controls).
     * The shared tick core's keys-half pump skip keys off
     * clientSimTransportTicksServer, which the connect set above — not off
     * this flag. */
    clientSimSetIsSinglePlayer(humanSim, true);
    if (!wantTutorial) {
      /* Practice enters the lobby, where the player picks the map, bots
       * and settings and presses Start. The same four flags desktop
       * single player sets after its connect. */
      clientSimSetInLobby(humanSim, true);
      clientSimSetNetStatus(humanSim, netLobby);
      clientSimSetMapDownloadComplete(humanSim, true);
      wasmSeedPracticeBot(spBrainPath,
                          wasmScenarioHost != NULL &&
                              serverSimScenarioHasLobbyTemplate(wasmServerSim));
      /* The lobby the map's scenario asks for, and its settings, as desktop
       * single player seats them: after the player has joined, so slot 0 is
       * theirs, and after the seeded bot, which a seat in its slot would
       * otherwise replace. A map with no scenario seats nothing. */
      if (wasmScenarioHost != NULL) {
        serverSimScenarioSeatLobby(wasmServerSim);
        serverSimScenarioApplyLobbyRules(wasmServerSim);
      }
    }
    /* Phase 2: connect registers the auto-subscriber. Clear the
     * legacy handle so the teardown path's unregister is a no-op. */
    wasmControlSub = SUBSCRIBER_HANDLE_INVALID;
    printf("[WASM] Single-player ServerSim ready\n");
  }

  clientSimSetAiType(humanSim, compTanks);
  clientMutexWaitFor();
  clientSimSetTankAutoSlowdown(humanSim, useAutoslow);
  clientSimSetTankAutoHideGunsight(humanSim, useAutohide);
  clientMutexRelease();

  dlgState = openFinished;
  return TRUE;
}

/* -------------------------------------------------------
 * gameFrontEnd — shutdown
 * ------------------------------------------------------- */
void gameFrontEnd(keyItems *keys, bool gamePlayed, bool isQuiting) {
  clientMutexWaitFor();
  if (gamePlayed == TRUE) {
    useAutoslow = clientSimGetTankAutoSlowdown(humanSim);
    useAutohide = clientSimGetTankAutoHideGunsight(humanSim);
  }
  brainsHandlerShutdown();
  if (wasmTransportActive) {
    if (wasmServerSim != NULL) {
      serverSimUnregisterSubscriber(wasmServerSim, wasmControlSub);
      wasmControlSub = SUBSCRIBER_HANDLE_INVALID;
      /* Before the sim goes, as desktop's shutdown does, so the next
       * practice game starts with no scenario. */
      scenarioHostDetach(wasmScenarioHost);
      wasmScenarioHost = NULL;
      serverSimDestroy(wasmServerSim);
      wasmServerSim = NULL;
    }
    wasmTransportActive = FALSE;
  }
  frontEndSetActiveClientSim(NULL);
  clientSimDestroy(humanSim);  /* also tears down the embedded transport */
  humanSim = NULL;
  if (isQuiting == TRUE) {
    sdl3ImguiCleanup();
    sdl3DrawCleanup();
    soundCleanup();
    langCleanup();
  }
  clientMutexRelease();
}

/* -------------------------------------------------------
 * gameFrontSetDlgState — simplified for WASM
 * ------------------------------------------------------- */
bool gameFrontSetDlgState(openingStates newState) {
  dlgState = newState;
  return TRUE;
}

/* -------------------------------------------------------
 * Getters / setters (same as original)
 * ------------------------------------------------------- */

void gameFrontGetCmdArg(char *getName)   { strcpy(getName, fileName); }
void gameFrontSetFileName(char *getName) { strcpy(fileName, getName); }

void gameFrontSetGameOptions(char *pword, gameType gt, bool hm, aiType ai,
                             int32_t sd, int32_t tlimit, bool justPass) {
  strcpy(password, pword);
  if (justPass == FALSE) {
    gametype = gt;
    hiddenMines = hm;
    compTanks = ai;
    if (compTanks == aiNone) brainsHandlerSet(FALSE);
    else brainsHandlerSet(TRUE);
    startDelay = sd;
    timeLen = tlimit;
  }
}

void gameFrontGetGameOptions(char *pword, gameType *gt, bool *hm,
                             aiType *ai, int32_t *sd, int32_t *tlimit) {
  strcpy(pword, password);
  *gt = gametype; *hm = hiddenMines; *ai = compTanks;
  *sd = startDelay; *tlimit = timeLen;
}

void gameFrontGetUdpOptions(char *pn, char *add,
                            unsigned short *theirUdp, unsigned short *myUdp) {
  strcpy(pn, gameFrontName);
  strcpy(add, gameFrontUdpAddress);
  *myUdp = gameFrontMyUdp;
  *theirUdp = gameFrontTargetUdp;
}

void gameFrontSetUdpOptions(char *pn, char *add,
                            unsigned short theirUdp, unsigned short myUdp) {
  strcpy(gameFrontName, pn);
  strcpy(gameFrontUdpAddress, add);
  gameFrontMyUdp = myUdp;
  gameFrontTargetUdp = theirUdp;
}

void gameFrontGetPlayerName(char *pn)  { strcpy(pn, gameFrontName); }
void gameFrontSetPlayerName(char *pn)  { strcpy(gameFrontName, pn); }

void gameFrontSetAIType(aiType ait) {
  compTanks = ait;
  clientSimSetAiType(humanSim, compTanks);
  if (compTanks == aiNone) brainsHandlerSet(FALSE);
  else brainsHandlerSet(TRUE);
}

void gameFrontSetRemeber(bool isSet)   { gameFrontRemeber = isSet; }
bool gameFrontGetRemeber(void)         { return gameFrontRemeber; }

void gameFrontGetTrackerOptions(char *address, unsigned short *port, bool *enabled) {
  strcpy(address, gameFrontTrackerAddr);
  *port = gameFrontTrackerPort;
  *enabled = gameFrontTrackerEnabled;
}

void gameFrontSetTrackerOptions(char *address, unsigned short port, bool enabled) {
  strcpy(gameFrontTrackerAddr, address);
  gameFrontTrackerPort = port;
  gameFrontTrackerEnabled = enabled;
}

void gameFrontEnableRejoin(void)       { wantRejoin = TRUE; }

void gameFrontSetWinbolonetToken(const char *token, const char *expiry) {
  SDL_strlcpy(gameFrontWbnToken, token, FILENAME_MAX);
  SDL_strlcpy(gameFrontWbnTokenExpiry, expiry, FILENAME_MAX);
  gameFrontWbnUse = (token[0] != '\0');
}

void gameFrontGetWinbolonetToken(char *token, char *expiry) {
  strcpy(token, gameFrontWbnToken);
  strcpy(expiry, gameFrontWbnTokenExpiry);
}

void gameFrontClearWinbolonetToken(void) {
  gameFrontWbnToken[0] = '\0';
  gameFrontWbnTokenExpiry[0] = '\0';
  gameFrontWbnUse = FALSE;
}

bool gameFrontGetWinbolonetUse(void) {
  return gameFrontWbnUse;
}

void gameFrontSetWbnAuthMethod(const char *method) {
  SDL_strlcpy(gameFrontWbnAuthMethod, method ? method : "",
              sizeof(gameFrontWbnAuthMethod));
}

void gameFrontGetWbnAuthMethod(char *out, size_t outSize) {
  SDL_strlcpy(out, gameFrontWbnAuthMethod, outSize);
}

void gameFrontSetWbnSignedOut(bool signedOut) {
  gameFrontWbnSignedOut = signedOut;
}

bool gameFrontGetWbnSignedOut(void) {
  return gameFrontWbnSignedOut;
}

void gameFrontSetWinbolonetRank(int rank, int rankTotal) {
  gameFrontWbnRank = rank;
  gameFrontWbnRankTotal = rankTotal;
}

void gameFrontGetWinbolonetRank(int *rank, int *rankTotal) {
  if (rank) *rank = gameFrontWbnRank;
  if (rankTotal) *rankTotal = gameFrontWbnRankTotal;
}

void gameFrontSetWinbolonetStats(const WbnStats *s) {
  if (s) gameFrontWbnStats = *s;
}

void gameFrontGetWinbolonetStats(WbnStats *out) {
  if (out) *out = gameFrontWbnStats;
}

/* Steam isn't available under Emscripten (the wrapper is stubbed), so there
 * is never an encrypted app-ticket to hand to WinBolo.net. */
bool gameFrontGetSteamTicketHex(char *outHex, size_t outSize) {
  if (outHex && outSize > 0) outHex[0] = '\0';
  return FALSE;
}

/* Cloud preferences sync runs over libcurl in the native client; the wasm
 * build has no curl, so the session-sync hooks are no-ops here. */
void gameFrontStartPrefsSync(void) { }
void gameFrontResetPrefsSyncSession(void) { }

void gameFrontApplySteamAuthResult(const char *token, const char *expiry,
                                   const char *playerName, int rank,
                                   int rankTotal, const WbnStats *stats) {
  gameFrontSetWinbolonetToken(token, expiry);
  gameFrontSetWbnAuthMethod("steam");
  gameFrontSetWinbolonetRank(rank, rankTotal);
  gameFrontSetWinbolonetStats(stats);
  if (playerName && playerName[0] != '\0') {
    gameFrontSetPlayerName((char *)playerName);
  }
}

void gameFrontGetBotOptions(int *count, char *brainPath, size_t brainPathSize) {
  *count = 0; brainPath[0] = '\0'; (void)brainPathSize;
}
void gameFrontSetBotSetup(const GameFrontBotSetup *setup) { (void)setup; }
void gameFrontGetBotSetup(GameFrontBotSetup *setup) { memset(setup, 0, sizeof(*setup)); }
void gameFrontSetRegistryKeys(void)           { }
void gameFrontSetAddressFromWebLink(char *a)  { (void)a; }
void gameFrontReloadSkins(void)               { }
void gameFrontShutdownServer(void)            { }
bool gameFrontPreferencesExist(void)          { return FALSE; }
bool gameFrontSetupServer(void)               { return FALSE; }
/* The web build never owns a round log to offer back, so callers that gate
 * on a locally recorded round see nothing. */
bool gameFrontHasLocalServer(void)            { return FALSE; }

/* Lobby/host helpers the in-game lobby pulls in now that it renders in the
 * web build (C6). Host-only / Steam / persistence features that are inert in
 * the browser. The SP server sim is the same handle gameFrontGetServerSim
 * returns (NULL for a netUdp lobby — the player is not the host). */
ServerSim *gameFrontGetSinglePlayerServerSim(void) { return wasmServerSim; }
void gameFrontTickSteamPresenceLobby(ClientSim *cs)  { (void)cs; }
/* No prefs file in the browser, so the player never has a stored bot
 * difficulty: the getter always reports "not chosen" and the lobby /
 * skill-guess callers fall back to their own default. */
bool gameFrontGetChosenBotDifficulty(uint8_t *out) { (void)out; return false; }
void gameFrontSetChosenBotDifficulty(uint8_t difficulty) { (void)difficulty; }
/* Same for the mode preference: nothing is stored, so a bot always runs in
 * the brain's first (default) mode at that mode's own default level. */
void gameFrontSetChosenBotModeAndLevel(const char *modeKey, const char *levelKey) {
  (void)modeKey; (void)levelKey;
}
bool gameFrontGetChosenBotModeKey(char *out, size_t outSz) {
  if (out && outSz) out[0] = '\0';
  return false;
}
bool gameFrontGetChosenBotLevelKey(char *out, size_t outSz) {
  if (out && outSz) out[0] = '\0';
  return false;
}
uint8_t gameFrontSpBotMode(const char *brainPath) { (void)brainPath; return 0; }
/* No prefs in the browser: a bot's tag colour is never remembered, so the
 * lobby re-derives it from the name each session (same result every time). */
bool gameFrontGetBotTagColor(const char *botName, uint32_t *rgb) { (void)botName; (void)rgb; return false; }
void gameFrontSetBotTagColor(const char *botName, uint32_t rgb) { (void)botName; (void)rgb; }
/* No per-scenario panel layout either. Window geometry stays out of the
 * cloud-synced prefs, so no row is ever remembered and the panel keeps
 * whatever the gameFrontScnPanel* globals hold. */
bool gameFrontGetScnPanelLayout(const char *scenario, int *x, int *y,
                                int *scale, int *alpha) {
  (void)scenario; (void)x; (void)y; (void)scale; (void)alpha;
  return false;
}
void gameFrontSetScnPanelLayout(const char *scenario, int x, int y,
                                int scale, int alpha) {
  (void)scenario; (void)x; (void)y; (void)scale; (void)alpha;
}
/* The map chooser tags a map that has a script beside it, answered by the
 * scenario library as on desktop. */
bool mapChooserMapHasScript(const char *mapPath) {
  return scenarioHostMapHasScript(mapPath);
}
/* No WinBolo.net stats plumbing here either, so the skill guess has nothing
 * to go on: the browser build gets the same Hard every difficulty currently
 * plays like. */
uint8_t gameFrontSpBotDifficulty(void) { return BOT_DIFFICULTY_HARD; }
uint8_t gameFrontSpBotLevel(const char *brainPath, uint8_t mode) {
  (void)brainPath; (void)mode; return BOT_DIFFICULTY_HARD;
}

/* Client-hosting write-through setters. The desktop build persists each key
 * into [HOSTING] as it changes; there is no prefs file in the browser, so
 * these only hold the value for the session the dialogs read it back in.
 *
 * The two script setters also pass the answer to the scenario library, as
 * desktop's do: the library decides whether a practice game's attach loads a
 * script, and the map chooser's scripted tag reads it too. */
void gameFrontSetHostingPort(unsigned short port)    { gameFrontHostingPort = port; }
void gameFrontSetHostingAllowSpec(bool allow)        { gameFrontHostingAllowSpec = allow; }

void gameFrontSetHostingScripts(bool allow) {
  gameFrontHostingScripts = allow;
  scenarioHostSetEnabled(allow);
}

void gameFrontSetHostingMaxSpec(int maxSpec)         { gameFrontHostingMaxSpec = maxSpec; }
void gameFrontSetHostingUploadPolicy(int policy)     { gameFrontHostingUploadPolicy = policy; }
void gameFrontSetHostingUploadMaxFiles(int maxFiles) { gameFrontHostingUploadMaxFiles = maxFiles; }
void gameFrontSetHostingLogging(bool logging)        { gameFrontHostingLogging = logging; }
void gameFrontSetHostingServeReplays(bool serve)     { gameFrontHostingServeReplays = serve; }
void gameFrontSetHostingVoiceMode(int mode)          { gameFrontHostingVoiceMode = mode; }

void gameFrontSetHostingUploadMaxStorage(int maxStorageMb) {
  gameFrontHostingUploadMaxStorage = maxStorageMb;
}

void gameFrontSetHostingUploadDir(const char *dir) {
  SDL_strlcpy(gameFrontHostingUploadDir, dir ? dir : "",
              sizeof(gameFrontHostingUploadDir));
}

void gameFrontSetHostingScriptUploadPolicy(int policy) {
  gameFrontHostingScriptUploadPolicy = policy;
  scenarioHostSetUploadScriptsEnabled(policy != SCRIPT_UPLOAD_OFF);
}

void gameFrontSetHostingShareScripts(bool on) {
  gameFrontHostingShareScripts = on;
}

void gameFrontSetHostingScriptUploadMaxFiles(int maxFiles) {
  gameFrontHostingScriptUploadMaxFiles = maxFiles;
}

void gameFrontSetHostingScriptUploadMaxStorage(int maxStorageMb) {
  gameFrontHostingScriptUploadMaxStorage = maxStorageMb;
}

void gameFrontSetHostingScriptUploadDir(const char *dir) {
  SDL_strlcpy(gameFrontHostingScriptUploadDir, dir ? dir : "",
              sizeof(gameFrontHostingScriptUploadDir));
}

void gameFrontSetHostingLogDir(const char *dir) {
  SDL_strlcpy(gameFrontHostingLogDir, dir ? dir : "",
              sizeof(gameFrontHostingLogDir));
}

void gameFrontSetViewPillPolicy(int policy)  { gameFrontViewPillPolicy = policy; }
void gameFrontSetViewBasePolicy(int policy)  { gameFrontViewBasePolicy = policy; }
void gameFrontSetViewAllyPolicy(int policy)  { gameFrontViewAllyPolicy = policy; }
void gameFrontSetViewPillDecaySecs(int secs) { gameFrontViewPillDecaySecs = secs; }
void gameFrontSetViewBaseDecaySecs(int secs) { gameFrontViewBaseDecaySecs = secs; }
void gameFrontSetViewAllyDecaySecs(int secs) { gameFrontViewAllyDecaySecs = secs; }
void gameFrontSetClassicMode(bool on)        { gameFrontClassicMode = on; }
void gameFrontSetAlliesInTrees(bool on)      { gameFrontAlliesInTrees = on; }
void gameFrontSetPositionalSound(bool on)    { gameFrontPositionalSound = on; }
void gameFrontSetOverviewWindow(int window)  { gameFrontOverviewWindow = window; }
void gameFrontSetLineOfSight(int mode)       { gameFrontLineOfSight = mode; }

/* Visibility preset and the remembered custom set. Nothing persists in a
 * browser tab, so these hold the choice for the session and no further. */
int                gameFrontVisibilityPreset = (int)visibilityPresetClassic;
VisibilitySettings gameFrontVisibilityCustom;
bool               gameFrontVisibilityCustomSaved = FALSE;

void gameFrontSetVisibilityPreset(int preset) {
  gameFrontVisibilityPreset = preset;
}

void gameFrontSetVisibilityCustom(const VisibilitySettings *v) {
  if (v == NULL) return;
  gameFrontVisibilityCustom      = *v;
  gameFrontVisibilityCustomSaved = TRUE;
}

/* The browser build never hosts (gameFrontHasLocalServer is FALSE), so
 * the lobby never records a pick here. */
void gameFrontRememberGameType(gameType gt)     { (void)gt; }
void gameFrontRememberAiPolicy(aiType ai)       { (void)ai; }
void gameFrontRememberHiddenMines(bool hm)      { (void)hm; }

void gameFrontRememberVisibility(const VisibilitySettings *v,
                                 bool                      saveCustom) {
  VisibilityPreset p;

  if (v == NULL) return;
  gameFrontViewPillPolicy    = (int)v->policy[viewCategoryPill];
  gameFrontViewBasePolicy    = (int)v->policy[viewCategoryBase];
  gameFrontViewAllyPolicy    = (int)v->policy[viewCategoryAlly];
  gameFrontViewPillDecaySecs = (int)v->decaySecs[viewCategoryPill];
  gameFrontViewBaseDecaySecs = (int)v->decaySecs[viewCategoryBase];
  gameFrontViewAllyDecaySecs = (int)v->decaySecs[viewCategoryAlly];
  gameFrontClassicMode       = v->classicMode;
  gameFrontAlliesInTrees     = v->alliesInTrees;
  gameFrontPositionalSound   = v->positionalSound;
  gameFrontOverviewWindow    = (int)v->overviewWindow;
  gameFrontLineOfSight       = (int)v->lineOfSight;
  p = visibilityPresetMatch(v);
  gameFrontVisibilityPreset = (int)p;
  if (saveCustom && p == visibilityPresetCustom) {
    gameFrontSetVisibilityCustom(v);
  }
}

void gameFrontGetVisibilitySettings(VisibilitySettings *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  out->policy[viewCategoryPill]    = (uint8_t)gameFrontViewPillPolicy;
  out->policy[viewCategoryBase]    = (uint8_t)gameFrontViewBasePolicy;
  out->policy[viewCategoryAlly]    = (uint8_t)gameFrontViewAllyPolicy;
  out->decaySecs[viewCategoryPill] = (uint16_t)gameFrontViewPillDecaySecs;
  out->decaySecs[viewCategoryBase] = (uint16_t)gameFrontViewBaseDecaySecs;
  out->decaySecs[viewCategoryAlly] = (uint16_t)gameFrontViewAllyDecaySecs;
  out->classicMode                 = gameFrontClassicMode;
  out->overviewWindow              = (uint8_t)gameFrontOverviewWindow;
  out->lineOfSight                 = (uint8_t)gameFrontLineOfSight;
  out->alliesInTrees               = gameFrontAlliesInTrees;
  out->positionalSound             = gameFrontPositionalSound;
}

/* Steam rich presence — there is no Steam client behind a browser tab. */
void gameFrontSetSteamPresenceMenu(void)           { }
void gameFrontSetSteamPresenceLobby(ClientSim *cs) { (void)cs; }

/* Supporter shield. Desktop resolves this from Steam DLC ownership, and the
 * WinBolo.net account flag isn't plumbed to the client yet, so the web build
 * has nothing to check. */
bool gameFrontIsSupporter(void) { return FALSE; }

/* Auto-join is the Steam-invite hand-off into the UDP setup dialog; nothing
 * requests one in the browser, so the dialog never auto-fires Join. */
bool gameFrontConsumeUdpAutoJoinRequest(void) { return FALSE; }

/* Cross-dialog transition request. Desktop feeds this from the host-OS menu
 * bar; in the browser the WinBolo.net dialog uses it to hand off to the log
 * viewer, so it carries a real value rather than stubbing out. */
static openingStates wasmPendingTransition;
static bool wasmPendingTransitionSet = FALSE;

void gameFrontRequestTransition(openingStates s) {
  wasmPendingTransition    = s;
  wasmPendingTransitionSet = TRUE;
}

bool gameFrontConsumeRequestedTransition(openingStates *out) {
  if (!wasmPendingTransitionSet) return FALSE;
  if (out) *out = wasmPendingTransition;
  wasmPendingTransitionSet = FALSE;
  return TRUE;
}


ServerSim *gameFrontGetServerSim(void) {
  return wasmServerSim;
}

BYTE gameFrontGetPlayerNum(void) {
  /* Same race as the desktop gamefront.c: the join can land on the lobby
   * replay before JOIN_ACCEPT, so the copy taken at join may still be 0.
   * The resent accept updates the transport, so read it live. It answers 0
   * with no sim and for the local (non-UDP) transport. */
  return clientSimGetServerPlayerNum(humanSim);
}

bool gameFrontGetPrefs(keyItems *keys, bool *pUseAutoslow, bool *pUseAutohide) {
  gameFrontSetDefaultKeys(keys);
  *pUseAutoslow = FALSE;
  *pUseAutohide = FALSE;
  return TRUE;
}

/* While wasmPrefsSnapshot runs, the writers below record each value into
 * this object instead of the prefs document. */
static cJSON *s_wasmPrefsRecord = NULL;

static void wasmPrefsPut(const char *section, const char *key,
                         const char *value) {
  if (s_wasmPrefsRecord != NULL) {
    cJSON *sec = cJSON_GetObjectItemCaseSensitive(s_wasmPrefsRecord, section);
    if (sec == NULL) {
      sec = cJSON_CreateObject();
      if (sec == NULL) return;
      cJSON_AddItemToObject(s_wasmPrefsRecord, section, sec);
    }
    cJSON_AddStringToObject(sec, key, value);
    return;
  }
  prefsSetString(section, key, value);
}

/* Writers for the prefs document, in the desktop's formats (gamefront.c):
 * integers as "%d", "Yes"/"No" for flags, "%.2f" for the float settings. */
static void wasmPrefsSetInt(const char *section, const char *key, int value) {
  char buff[32];
  snprintf(buff, sizeof(buff), "%d", value);
  wasmPrefsPut(section, key, buff);
}

static void wasmPrefsSetBool(const char *section, const char *key,
                             bool value) {
  wasmPrefsPut(section, key, TRUEFALSE_TO_STR(value));
}

static void wasmPrefsSetFloat(const char *section, const char *key,
                              float value) {
  char buff[32];
  snprintf(buff, sizeof(buff), "%.2f", value);
  wasmPrefsPut(section, key, buff);
}

/* Write the settings the web applies from a WinBolo.net prefs document
 * (wasmApplyJoinPrefs in main_wasm.c), one for one: every key that function
 * reads, this writes, under the desktop's section and name, and nothing
 * else. The
 * document lives on MEMFS for the page; wbPrefsPumpUpload sends it to
 * WinBolo.net when the player is signed in. A write that changes nothing
 * leaves the document clean, so calling this with no change uploads
 * nothing. */
void gameFrontPutPrefs(keyItems *keys) {
  int i;

  wasmPrefsSetInt("KEYS", "Forward",        keys->kiForward);
  wasmPrefsSetInt("KEYS", "Backwards",      keys->kiBackward);
  wasmPrefsSetInt("KEYS", "Left",           keys->kiLeft);
  wasmPrefsSetInt("KEYS", "Right",          keys->kiRight);
  wasmPrefsSetInt("KEYS", "Shoot",          keys->kiShoot);
  wasmPrefsSetInt("KEYS", "Lay Mine",       keys->kiLayMine);
  wasmPrefsSetInt("KEYS", "Increase Range", keys->kiGunIncrease);
  wasmPrefsSetInt("KEYS", "Decrease Range", keys->kiGunDecrease);
  wasmPrefsSetInt("KEYS", "Tank View",      keys->kiTankView);
  wasmPrefsSetInt("KEYS", "Pill View",      keys->kiPillView);
  wasmPrefsSetInt("KEYS", "Ally View",      keys->kiAllyView);
  wasmPrefsSetInt("KEYS", "Base View",      keys->kiBaseView);
  wasmPrefsSetInt("KEYS", "Overview Zoom",  keys->kiOverviewZoom);
  wasmPrefsSetInt("KEYS", "Overview Follow",   keys->kiOverviewFollow);
  wasmPrefsSetInt("KEYS", "Overview Zoom In",  keys->kiOverviewZoomIn);
  wasmPrefsSetInt("KEYS", "Overview Zoom Out", keys->kiOverviewZoomOut);
  wasmPrefsSetInt("KEYS", "Scroll Up",      keys->kiScrollUp);
  wasmPrefsSetInt("KEYS", "Scroll Down",    keys->kiScrollDown);
  wasmPrefsSetInt("KEYS", "Scroll Left",    keys->kiScrollLeft);
  wasmPrefsSetInt("KEYS", "Scroll Right",   keys->kiScrollRight);
  wasmPrefsSetInt("KEYS", "Quick Tree",     keys->kiQuickTree);
  wasmPrefsSetInt("KEYS", "Quick Road",     keys->kiQuickRoad);
  wasmPrefsSetInt("KEYS", "Quick Wall",     keys->kiQuickWall);
  wasmPrefsSetInt("KEYS", "Quick Pillbox",  keys->kiQuickPillbox);
  wasmPrefsSetInt("KEYS", "Quick Mine",     keys->kiQuickMine);
  wasmPrefsSetInt("KEYS", "Ping 1",         keys->kiPing[0]);
  wasmPrefsSetInt("KEYS", "Ping 2",         keys->kiPing[1]);
  wasmPrefsSetInt("KEYS", "Ping 3",         keys->kiPing[2]);
  for (i = 0; i < PING_BIND_DIRECT_SLOTS; i++) {
    char name[32];
    snprintf(name, sizeof(name), "Ping Direct %d", i + 1);
    wasmPrefsSetInt("KEYS", name, keys->kiPingDirect[i]);
  }

  wasmPrefsSetBool("MENU", "Show Gunsight",   showGunsight);
  wasmPrefsSetBool("MENU", "Sound Effects",   soundEffects);
  wasmPrefsSetBool("MENU", "Show Newswire Messages",  showNewswireMessages);
  wasmPrefsSetBool("MENU", "Show Assistant Messages", showAssistantMessages);
  wasmPrefsSetBool("MENU", "Show AI Messages",        showAIMessages);
  wasmPrefsSetBool("MENU", "Show Network Status Messages",
                   showNetworkStatusMessages);
  wasmPrefsSetBool("MENU", "Show Network Debug Messages",
                   showNetworkDebugMessages);
  wasmPrefsSetBool("MENU", "Autoscroll Enabled", autoScrollingEnabled);
  wasmPrefsSetBool("MENU", "Show Pill Labels",   showPillLabels);
  wasmPrefsSetBool("MENU", "Show Base Labels",   showBaseLabels);
  wasmPrefsSetBool("MENU", "Label Own Tank",     labelSelf);
  wasmPrefsSetInt("MENU", "Message Label Size", (int)labelMsg);
  wasmPrefsSetInt("MENU", "Tank Label Size",    (int)labelTank);
  wasmPrefsSetInt("MENU", "Sound Volume",       soundVolume);
  wasmPrefsSetInt("MENU", "Master Volume",      windowMasterVolume);

  wasmPrefsSetBool("GAME OPTIONS", "Auto Slowdown", useAutoslow);
  wasmPrefsSetBool("GAME OPTIONS", "Auto Show-Hide Gunsight", useAutohide);

  wasmPrefsSetFloat("SETTINGS", "Gamepad Scroll Sens",
                    g_gamepadScrollSensitivity);
  wasmPrefsSetFloat("SETTINGS", "Gamepad Tank Sens",
                    g_gamepadTankSensitivity);
  wasmPrefsSetFloat("SETTINGS", "Gamepad Build Cursor Sens",
                    g_gamepadBuildCursorSensitivity);
  wasmPrefsSetBool("SETTINGS", "Build Exit Executes", g_buildExitExecutes);
  wasmPrefsSetBool("SETTINGS", "Build Exit Executes Momentary Only",
                   g_buildExitExecutesMomentaryOnly);
  wasmPrefsSetBool("SETTINGS", "Build Double Tap Road", g_buildDoubleTapRoad);
  wasmPrefsSetBool("SETTINGS", "Build Hold Momentary", g_buildHoldMomentary);
  wasmPrefsSetBool("SETTINGS", "Build Auto Close On Execute",
                   g_buildAutoCloseOnExecute);

  /* Voice is read back out of the running voice module, as the desktop
   * does. The mode names are the desktop's (gamefront.c). */
  wasmPrefsSetBool("VOICE", "Enabled", voiceIsEnabled());
  {
    VoiceMode vm = voiceGetMode();
    wasmPrefsPut("VOICE", "Mode",
                   vm == VOICE_MODE_OFF    ? "Off"
                   : vm == VOICE_MODE_OPEN ? "Open Mic"
                                           : "Push To Talk");
  }
  wasmPrefsSetFloat("VOICE", "Mic Gain",     voiceGetMicGain());
  wasmPrefsSetFloat("VOICE", "Voice Volume", voiceGetOutputVolume());
  wasmPrefsSetBool("VOICE", "Tank Icons", windowGetShowTankMicIcons());
}

/* Write the current settings, with the key bindings the game holds. */
void gameFrontSaveCurrentPrefs(void) {
  keyItems k;
  windowGetKeys(&k);
  gameFrontPutPrefs(&k);
}

/* The settings gameFrontSaveCurrentPrefs would write, as a JSON object of
 * sections of string values, without touching the prefs document. The
 * caller frees the result; NULL if it could not be built. */
char *wasmPrefsSnapshot(void) {
  keyItems k;
  char *out;

  s_wasmPrefsRecord = cJSON_CreateObject();
  if (s_wasmPrefsRecord == NULL) return NULL;
  windowGetKeys(&k);
  gameFrontPutPrefs(&k);
  out = cJSON_PrintUnformatted(s_wasmPrefsRecord);
  cJSON_Delete(s_wasmPrefsRecord);
  s_wasmPrefsRecord = NULL;
  return out;
}

/* Write into the prefs document each setting whose value in current (a
 * wasmPrefsSnapshot) differs from its value in baseline (an earlier one):
 * the settings changed since baseline was taken. The rest of the document
 * is left as it is. */
void wasmPrefsWriteChanged(const char *baseline, const char *current) {
  cJSON *base = cJSON_Parse(baseline);
  cJSON *cur = cJSON_Parse(current);
  cJSON *sec;

  if (base != NULL && cur != NULL) {
    cJSON_ArrayForEach(sec, cur) {
      cJSON *baseSec = cJSON_GetObjectItemCaseSensitive(base, sec->string);
      cJSON *item;
      cJSON_ArrayForEach(item, sec) {
        cJSON *was = cJSON_GetObjectItemCaseSensitive(baseSec, item->string);
        if (!cJSON_IsString(item)) continue;
        if (cJSON_IsString(was) &&
            strcmp(was->valuestring, item->valuestring) == 0) {
          continue;
        }
        prefsSetString(sec->string, item->string, item->valuestring);
      }
    }
  }
  cJSON_Delete(base);
  cJSON_Delete(cur);
}

void gameFrontHandleUrlOpen(char *url) {
  (void)url;
}

void gameFrontUpdateSteamPresence(ClientSim *cs) {
  (void)cs;
}

void gameFrontGetLanguageCode(char *out, int outSize) {
  if (!out || outSize <= 0) return;
  out[0] = '\0';
}

void gameFrontSetLanguageCode(const char *code) {
  (void)code;
}

/* Settings' Play Tutorial button. Settings closes after the click and main's
 * screen loop shows the menu again, which takes this request on its first
 * frame and returns openTutorial, so the loop starts the tutorial. */
void gameFrontRequestPlayTutorial(void) {
  gameFrontRequestTransition(openTutorial);
}

void gameFrontSaveWindowSettings(void) {
}

/* Called every frame by the game frame (sdl3ImguiProcessEvents) and the
 * shared dialog loops: push any setting changed this session, debounced.
 * No-op when not signed in or when nothing is sync-dirty. */
void gameFrontPumpDirty(void) {
  wbPrefsPumpUpload(SDL_GetTicks());
}

void gameFrontSaveTankPrefs(ClientSim *cs) {
  if (cs != NULL) {
    useAutoslow = clientSimGetTankAutoSlowdown(cs);
    useAutohide = clientSimGetTankAutoHideGunsight(cs);
  }
  gameFrontSaveCurrentPrefs();
}

/* Whether the menu shows the Tutorial row. Lives for the page: finishing the
 * tutorial clears it, as on the desktop, and a reload brings it back. */
static bool wasmShowTutorialButton = TRUE;

bool gameFrontGetShowTutorialButton(void) {
  return wasmShowTutorialButton;
}

void gameFrontSetShowTutorialButton(bool show) {
  wasmShowTutorialButton = show;
}
