/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../gui/gamefront.h"
#include "../gui/input.h"
#include "../gui/lang.h"
#include "../gui/sound.h"
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
#include "../winbolonet/winbolonet_core.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/luabrainshandler.h"

/* Forward declaration */
extern void sdl3MessageHandler(const char *message, const char *title);


/* -------------------------------------------------------
 * URL parameter helper (WASM only)
 * ------------------------------------------------------- */
static const char *gameFrontGetUrlParam(const char *name) {
  static char buf[512];
  char js[640];
  snprintf(js, sizeof(js),
    "(function(){ var p = new URLSearchParams(window.location.search).get('%s');"
    " return p ? p : ''; })()", name);
  const char *result = emscripten_run_script_string(js);
  if (result) {
    strncpy(buf, result, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
  }
  return "";
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

bool isServer = FALSE;
bool useAutoslow;
bool useAutohide;
bool wantRejoin;

/* NAT/UPnP toggles — wasm client never hosts a server, but the shared
 * settings dialog references these globals. */
bool gameFrontUseUpnp = FALSE;
bool gameFrontUseNatTraversal = FALSE;

/* Server-authoritative state — the Transport handle itself now lives
 * inside humanSim; only high-level lifecycle gating is tracked here. */
static ServerSim *wasmServerSim = NULL;
static bool wasmTransportActive = FALSE;
static BYTE wasmPlayerNum = 0;
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
  keys->kiLGMView      = DEFAULT_LGMVIEW;
  keys->kiBaseView     = DEFAULT_BASEVIEW;
  keys->kiScrollUp     = DEFAULT_SCROLLUP;
  keys->kiScrollDown   = DEFAULT_SCROLLDOWN;
  keys->kiScrollLeft   = DEFAULT_SCROLLLEFT;
  keys->kiScrollRight  = DEFAULT_SCROLLRIGHT;
  keys->kiQuickTree    = DEFAULT_QUICKTREE;
  keys->kiQuickRoad    = DEFAULT_QUICKROAD;
  keys->kiQuickWall    = DEFAULT_QUICKWALL;
  keys->kiQuickPillbox = DEFAULT_QUICKPILLBOX;
  keys->kiQuickMine    = DEFAULT_QUICKMINE;
}

/* -------------------------------------------------------
 * gameFrontStart — skip all dialogs, start practice game
 * ------------------------------------------------------- */
bool gameFrontStart(const char *cmdLine, keyItems *keys, bool isLoaded, ClientSim **out_cs) {
  isTutorial = FALSE;
  password[0] = '\0';
  wantRejoin = FALSE;

  /* Set defaults. The real player name is chosen in main_wasm.c after this
   * returns (web<rand> for join-code play, ?name= or "Me" for single player);
   * this seed only matters to any path that reads the name before then. */
  strcpy(gameFrontName, "Me");
  gameFrontUdpAddress[0] = '\0';
  gameFrontMyUdp = 27500;
  gameFrontTargetUdp = 27500;
  strcpy(gameFrontTrackerAddr, TRACKER_ADDRESS);
  gameFrontTrackerPort = TRACKER_PORT;
  gameFrontTrackerEnabled = FALSE;
  gameFrontWbnToken[0] = '\0';
  gameFrontWbnTokenExpiry[0] = '\0';
  gameFrontWbnUse = FALSE;
  gameFrontRemeber = FALSE;

  /* Default game options */
  gametype = gameOpen;
  hiddenMines = FALSE;
  compTanks = aiNone;
  startDelay = 0;
  timeLen = UNLIMITED_GAME_TIME;
  useAutoslow = FALSE;
  useAutohide = FALSE;

  /* Default keys */
  gameFrontSetDefaultKeys(keys);

  langSetup();

  /* Process command line */
  if (cmdLine != NULL && cmdLine[0] != '\0') {
    strncpy(fileName, cmdLine, FILENAME_MAX - 1);
    fileName[FILENAME_MAX - 1] = '\0';
  } else {
    fileName[0] = '\0';
  }

  /* Initialise subsystems */
  if (isLoaded == FALSE) {
    /* Tell SDL3 to use the existing canvas element from shell.html */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, "#canvas");

    if (sdl3DrawSetup(windowGetZoomFactor()) == FALSE) {
      printf("[WASM] sdl3DrawSetup FAILED\n");
      return FALSE;
    }
    printf("[WASM] sdl3DrawSetup OK\n");

    /* SDL3's Emscripten backend resets the canvas element size during probing.
       Force both the canvas buffer and SDL window to the desired resolution. */
    {
      SDL_Window *win = sdl3DrawGetWindow();
      int w, h;
      SDL_GetWindowSize(win, &w, &h);
      emscripten_set_canvas_element_size("#canvas", w, h);
      printf("[WASM] Forced canvas to: %d x %d\n", w, h);
    }

    if (soundSetup() == FALSE) {
      soundEffects = FALSE;
    }

    brainsHandlerLoadBrains();
  }

  guiMessageSetHandler(sdl3MessageHandler);

  /* ---- Determine net mode from URL params ----
   * Production web play is selected by ?join_code= (the WBN chooser
   * redirects to play.winbolo.net/?join_code=<code>). A dev/LAN run may
   * instead pass an explicit ?proxyURL=. Either selects UDP-over-WebSocket
   * mode. shell.html points Module.websocket.url at the real relay, so the
   * host:port handed to the transport here is an ignored sentinel —
   * routing lives in the join_code (or the dev proxy URL). */
  netType urlNetType = netSingle;
  /* gameFrontGetUrlParam returns a shared static buffer, so copy each value
   * out before the next call overwrites it. */
  char joinCode[128];
  strncpy(joinCode, gameFrontGetUrlParam("join_code"), sizeof(joinCode) - 1);
  joinCode[sizeof(joinCode) - 1] = '\0';
  char devProxy[1024];
  strncpy(devProxy, gameFrontGetUrlParam("proxyURL"), sizeof(devProxy) - 1);
  devProxy[sizeof(devProxy) - 1] = '\0';
  bool wantTutorial = (gameFrontGetUrlParam("tutorial")[0] != '\0');
  bool haveJoinCode = (joinCode[0] != '\0');
  if (haveJoinCode || devProxy[0] != '\0') {
    urlNetType = netUdp;
  }

  if (urlNetType == netUdp) {
    if (haveJoinCode) {
      /* Production: routing is in the join_code carried by
       * Module.websocket.url; the transport target is a sentinel the relay
       * never sees. Use a loopback literal so no DNS lookup is attempted. */
      printf("[WASM] netUdp mode: web play via relay (join_code)\n");
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
    const char *wbnArg = haveJoinCode ? joinCode
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
      clientSimDestroy(humanSim);
      return FALSE;
    }

    /* Wait for join + map download */
    {
      int joinWaitTicks = 0;
      while ((clientSimGetConnectState(humanSim) == CLIENT_CONNECT_JOINING ||
              clientSimGetConnectState(humanSim) == CLIENT_CONNECT_DOWNLOADING_MAP) &&
             joinWaitTicks < 1500) {
        clientSimNetTick(humanSim);
        SDL_Delay(20);
        joinWaitTicks++;
      }
    }

    if (clientSimGetConnectState(humanSim) != CLIENT_CONNECT_CONNECTED) {
      const char *reason = clientSimGetConnectErrorReason(humanSim);
      printf("[WASM] Join failed: %s\n", reason ? reason : "timeout");
      clientSimDestroy(humanSim);
      return FALSE;
    }

    wasmPlayerNum = clientSimGetServerPlayerNum(humanSim);
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

    /* Map install + snapshot apply happen inside the UDP transport
     * (MAP_DOWNLOAD inline install + CTRL_GAME_PHASE LOBBY→RUNNING
     * watcher), and the first snapshot apply fires the viewport
     * finalisation. */
    /* Gate lobby vs running: if we received CTRL_LOBBY_SETTINGS during
     * join, stay in lobby state; otherwise proceed to running */
    if (clientSimIsInLobby(humanSim)) {
      clientSimSetMapDownloadComplete(humanSim, true);
      clientSimSetNetStatus(humanSim, netLobby);
    }
    printf("[WASM] UDP connected as player %d\n", wasmPlayerNum);
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
      frontEndTutorialReset();
      isTutorial = TRUE;
      printf("[WASM] starting guided tutorial\n");
    }

    {
      if (fileName[0] != '\0') {
        wasmServerSim = serverSimCreate(fileName, gametype, hiddenMines, startDelay, timeLen);
        if (wasmServerSim == NULL) {
          printf("[WASM] Failed to load map '%s' into ServerSim, trying built-in\n", fileName);
        }
      }
      if (wasmServerSim == NULL) {
        BYTE emap[6000] = E_MAP;
        wasmServerSim = serverSimCreateCompressed(emap, 5097, "EverardIsland", gametype, hiddenMines, startDelay, timeLen);
      }
      if (wasmServerSim == NULL) {
        printf("[WASM] Failed to create ServerSim\n");
        clientSimDestroy(humanSim);
        return FALSE;
      }
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

    /* WASM single-player: no lobby, run immediately. acceptRemoteClients
     * is zero-init false so the UDP / WBN / tracker bring-up is
     * skipped; skipLobby drives the StartGameInPlace transition;
     * viewPlayer 0 is the SP convention. */
    {
      ServerInstanceConfig cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.skipLobby = true;
      if (!serverInstanceStartup(wasmServerSim, &cfg)) {
        printf("[WASM] serverInstanceStartup failed\n");
        free(wasmServerSim);
        wasmServerSim = NULL;
        clientSimDestroy(humanSim);
        return FALSE;
      }
    }

    /* Run the 12-step join+install in one call. */
    if (!clientSimConnectLocal(humanSim, wasmServerSim,
                               gameFrontName, "", 0, 0)) {
      printf("[WASM] clientSimConnectLocal failed: %s\n",
             clientSimGetConnectErrorReason(humanSim));
      free(wasmServerSim);
      wasmServerSim = NULL;
      clientSimDestroy(humanSim);
      return FALSE;
    }
    wasmTransportActive = TRUE;
    wasmPlayerNum = 0;
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

void gameFrontGetPassword(char *pword) { strcpy(pword, password); }
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

/* Lobby/host helpers the in-game lobby pulls in now that it renders in the
 * web build (C6). Host-only / Steam / persistence features that are inert in
 * the browser. The SP server sim is the same handle gameFrontGetServerSim
 * returns (NULL for a netUdp lobby — the player is not the host). */
ServerSim *gameFrontGetSinglePlayerServerSim(void) { return wasmServerSim; }
void gameFrontTickSteamPresenceLobby(ClientSim *cs)  { (void)cs; }
void gameFrontGetChosenBotBrain(char *out, size_t outLen) { if (out && outLen) out[0] = '\0'; }
void gameFrontSetChosenBotBrain(const char *name)    { (void)name; }


ServerSim *gameFrontGetServerSim(void) {
  return wasmServerSim;
}

BYTE gameFrontGetPlayerNum(void) {
  return wasmPlayerNum;
}

bool gameFrontGetPrefs(keyItems *keys, bool *pUseAutoslow, bool *pUseAutohide) {
  gameFrontSetDefaultKeys(keys);
  *pUseAutoslow = FALSE;
  *pUseAutohide = FALSE;
  return TRUE;
}

void gameFrontPutPrefs(keyItems *keys) {
  (void)keys;
}

/* No prefs file in the browser — settings live only for the session. */
void gameFrontSaveCurrentPrefs(void) {
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

void gameFrontRequestPlayTutorial(void) {
}

void gameFrontSaveWindowSettings(void) {
}

void gameFrontPumpDirty(void) {
}

void gameFrontSaveTankPrefs(ClientSim *cs) {
  if (cs != NULL) {
    useAutoslow = clientSimGetTankAutoSlowdown(cs);
    useAutohide = clientSimGetTankAutoHideGunsight(cs);
  }
}

bool gameFrontGetShowTutorialButton(void) {
  return FALSE;
}

void gameFrontSetShowTutorialButton(bool show) {
  (void)show;
}
