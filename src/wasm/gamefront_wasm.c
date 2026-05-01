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

#include "../bolo/screen.h"
#include "../bolo/client_sim.h"
#include "../bolo/global.h"
#include "../bolo/players.h"
#include "../bolo/platform_net.h"
#include "../bolo/transport.h"
#include "../bolo/transport_udp.h"
#include "../bolo/gui_message.h"
#include "../bolo/everard_map.h"
#include "../server/server_sim.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../gui/gamefront.h"
#include "../gui/input.h"
#include "../gui/lang.h"
#include "../gui/sound.h"
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
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

/* Dialog states */
openingStates dlgState = openStart;

bool isServer = FALSE;
bool useAutoslow;
bool useAutohide;
bool wantRejoin;

/* Server-authoritative state */
static ServerSim *wasmServerSim = NULL;
static Transport wasmTransport;
static bool wasmTransportActive = FALSE;
static BYTE wasmPlayerNum = 0;
ClientSim humanSimStorage;
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
 * gameFrontLoadInBuiltMap
 * ------------------------------------------------------- */
bool gameFrontLoadInBuiltMap(void) {
  /* In WASM, maps are preloaded into the virtual filesystem */
  const char *paths[] = {
    "/data/EverardIsland.map",
    "/data/Everard Island.map",
    "Everard Island.map",
    NULL
  };

  for (int i = 0; paths[i] != NULL; i++) {
    FILE *fp = fopen(paths[i], "rb");
    if (fp != NULL) {
      fclose(fp);
      printf("[WASM] Loading map: %s\n", paths[i]);
      return screenLoadMapCS(humanSim, (char *)paths[i], gametype, hiddenMines,
                           startDelay, timeLen, gameFrontName, FALSE);
    }
  }

  printf("[WASM] ERROR: Could not find Everard Island.map\n");
  return FALSE;
}

bool gameFrontLoadTutorial(void) {
  const char *paths[] = {
    "/data/InbuiltTutorial.map",
    "/data/Inbuilt Tutorial.map",
    "Inbuilt Tutorial.map",
    NULL
  };

  for (int i = 0; paths[i] != NULL; i++) {
    FILE *fp = fopen(paths[i], "rb");
    if (fp != NULL) {
      fclose(fp);
      return screenLoadMapCS(humanSim, (char *)paths[i], gameStrictTournament, FALSE,
                           0, UNLIMITED_GAME_TIME, gameFrontName, FALSE);
    }
  }
  return FALSE;
}

/* -------------------------------------------------------
 * gameFrontStart — skip all dialogs, start practice game
 * ------------------------------------------------------- */
bool gameFrontStart(const char *cmdLine, keyItems *keys, bool isLoaded, ClientSim **out_cs) {
  isTutorial = FALSE;
  password[0] = '\0';
  wantRejoin = FALSE;

  /* Set defaults */
  strcpy(gameFrontName, "WASM Player");
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

  /* ---- Determine net mode from URL params ---- */
  netType urlNetType = netSingle;
  {
    const char *nt = gameFrontGetUrlParam("netType");
    if (nt[0] != '\0' && (strcmp(nt, "udp") == 0 || strcmp(nt, "netUdp") == 0)) {
      urlNetType = netUdp;
    }
  }

  if (urlNetType == netUdp) {
    /* Read proxy URL — default if not supplied */
    const char *rawProxy = gameFrontGetUrlParam("proxyURL");
    char proxyUrl[1024];
    if (rawProxy[0] != '\0') {
      strncpy(proxyUrl, rawProxy, sizeof(proxyUrl) - 1);
      proxyUrl[sizeof(proxyUrl) - 1] = '\0';
    } else {
      strncpy(proxyUrl, "ws://192.168.42.200:8085/proxy?server=192.168.42.14:27500",
              sizeof(proxyUrl) - 1);
      proxyUrl[sizeof(proxyUrl) - 1] = '\0';
    }
    printf("[WASM] netUdp mode, proxy: %s\n", proxyUrl);

    /* Parse server host:port from proxy URL for transport */
    char serverHost[256];
    unsigned short serverPort = 27500;
    parseProxyServerParam(proxyUrl, serverHost, sizeof(serverHost), &serverPort);
    printf("[WASM] Game server: %s:%d\n", serverHost, serverPort);

    strncpy(gameFrontUdpAddress, serverHost, sizeof(gameFrontUdpAddress) - 1);
    gameFrontUdpAddress[sizeof(gameFrontUdpAddress) - 1] = '\0';
    gameFrontTargetUdp = serverPort;
  }

  /* Start the game directly — no dialogs */
  printf("[WASM] Setting up screen...\n");
  humanSim = &humanSimStorage;
  clientSimCreate(humanSim, 0, FALSE, 0, UNLIMITED_GAME_TIME);

  if (urlNetType == netUdp) {
    /* ---- UDP multiplayer via new transport ---- */
    printf("[WASM] Connecting via UDP transport...\n");
    wasmTransport = transportUdpClientCreate(humanSim, gameFrontUdpAddress,
                                              gameFrontTargetUdp,
                                              gameFrontName, password,
                                              gameFrontWbnUse ? gameFrontWbnToken : "",
                                              wantRejoin,
                                              "", 0);
    if (transportUdpClientGetJoinState(&wasmTransport) == UDP_CLIENT_ERROR) {
      const char *reason = transportUdpClientGetJoinRejectReason(&wasmTransport);
      printf("[WASM] UDP connect failed: %s\n", reason ? reason : "unknown");
      transportUdpClientDestroy(&wasmTransport);
      clientSimDestroy(humanSim);
      return FALSE;
    }

    /* Wait for join + map download */
    {
      int joinWaitTicks = 0;
      while ((transportUdpClientGetJoinState(&wasmTransport) == UDP_CLIENT_JOINING ||
              transportUdpClientGetJoinState(&wasmTransport) == UDP_CLIENT_DOWNLOADING_MAP) &&
             joinWaitTicks < 1500) {
        wasmTransport.tick(wasmTransport.ctx);
        SDL_Delay(20);
        joinWaitTicks++;
      }
    }

    if (transportUdpClientGetJoinState(&wasmTransport) != UDP_CLIENT_CONNECTED) {
      const char *reason = transportUdpClientGetJoinRejectReason(&wasmTransport);
      printf("[WASM] Join failed: %s\n", reason ? reason : "timeout");
      transportUdpClientDestroy(&wasmTransport);
      clientSimDestroy(humanSim);
      return FALSE;
    }

    wasmPlayerNum = transportUdpClientGetPlayerNum(&wasmTransport);
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
      humanSim->serverAddress = saddr.sin_addr;
      humanSim->serverPort = gameFrontTargetUdp;
    }

    /* Load map from server */
    {
      const BYTE *mapData;
      int mapLen = 0;
      gameType serverGame;
      bool serverHiddenMines;
      int32_t serverStartDelay, serverGameLen;

      mapData = transportUdpClientGetMapData(&wasmTransport, &mapLen);
      transportUdpClientGetGameSettings(&wasmTransport, &serverGame,
                                         &serverHiddenMines,
                                         &serverStartDelay, &serverGameLen);

      if (mapData != NULL && mapLen > 0) {
        char savedMapName[MAP_STR_SIZE];
        strncpy(savedMapName, humanSim->mapName, MAP_STR_SIZE - 1);
        savedMapName[MAP_STR_SIZE - 1] = '\0';
        clientSimDestroy(humanSim);
        if (screenLoadCompressedMapCS(humanSim, (BYTE *)mapData, mapLen, savedMapName,
                                     serverGame, serverHiddenMines,
                                     serverStartDelay, serverGameLen,
                                     gameFrontName, wasmPlayerNum, FALSE) == FALSE) {
          printf("[WASM] Failed to load map from server\n");
          transportUdpClientDestroy(&wasmTransport);
          wasmTransportActive = FALSE;
          return FALSE;
        }
        screenSetLocalTransportCS(humanSim, false);
      } else {
        printf("[WASM] No map data from server\n");
        transportUdpClientDestroy(&wasmTransport);
        wasmTransportActive = FALSE;
        clientSimDestroy(humanSim);
        return FALSE;
      }
    }

    screenNetSetupTankGoCS(humanSim);
    /* Gate lobby vs running: if we received PACKET_LOBBY_STATE during
     * join, stay in lobby state; otherwise proceed to running */
    if (humanSim->inLobby) {
      humanSim->mapDownloadComplete = true;
      humanSim->netStat = netLobby;
    }
    printf("[WASM] UDP connected as player %d\n", wasmPlayerNum);
  } else {
    /* ---- Single-player via ServerSim + local transport ---- */
    printf("[WASM] Setting up single-player ServerSim...\n");

    wasmServerSim = (ServerSim *)malloc(sizeof(ServerSim));
    if (wasmServerSim == NULL) {
      printf("[WASM] Failed to allocate ServerSim\n");
      clientSimDestroy(humanSim);
      return FALSE;
    }

    {
      bool simOk = FALSE;
      if (fileName[0] != '\0') {
        simOk = serverSimCreate(wasmServerSim, fileName, gametype, hiddenMines, startDelay, timeLen);
        if (!simOk) {
          printf("[WASM] Failed to load map '%s' into ServerSim, trying built-in\n", fileName);
        }
      }
      if (!simOk) {
        BYTE emap[6000] = E_MAP;
        simOk = serverSimCreateCompressed(wasmServerSim, emap, 5097, gametype, hiddenMines, startDelay, timeLen);
      }
      if (!simOk) {
        printf("[WASM] Failed to create ServerSim\n");
        free(wasmServerSim);
        wasmServerSim = NULL;
        clientSimDestroy(humanSim);
        return FALSE;
      }
    }

    /* WASM single-player: no lobby, run immediately */
    wasmServerSim->lobbyEnabled = false;
    wasmServerSim->state = serverStateRunning;
    serverSimAddPlayer(wasmServerSim, 0, gameFrontName, false);
    wasmServerSim->sim.viewPlayer = 0;
    wasmTransport = transportLocalCreate(wasmServerSim, 0);
    wasmTransportActive = TRUE;
    wasmPlayerNum = 0;

    /* Load compressed map on client side */
    {
      BYTE compressedMap[65536];
      int compLen = serverSimGetCompressedMap(wasmServerSim, compressedMap);
      if (compLen > 0) {
        clientSimDestroy(humanSim);
        screenLoadCompressedMapCS(humanSim, compressedMap, compLen, "Local Game",
                                 gametype, hiddenMines, startDelay,
                                 timeLen, gameFrontName, 0, FALSE);
      }
    }

    /* Sync initial snapshot */
    {
      SnapshotHeader snapHdr;
      TankSnapshot snapTanks[MAX_TANKS];
      ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
      TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
      BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
      PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
      GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
      serverSimBuildSnapshot(wasmServerSim, 0, &snapHdr,
                             snapTanks, MAX_TANKS,
                             snapShells, MAX_SNAPSHOT_SHELLS,
                             snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                             snapBases, MAX_SNAPSHOT_BASES,
                             snapPills, MAX_SNAPSHOT_PILLS,
                             snapEvents, MAX_SNAPSHOT_EVENTS,
                             false);
      clientSimSyncFromSnapshot(humanSim, &snapHdr, snapTanks, snapHdr.tankCount,
                             snapShells, snapHdr.shellCount,
                             snapTkExplosions, snapHdr.tkExplosionCount,
                             snapBases, snapHdr.baseCount,
                             snapPills, snapHdr.pillCount,
                             snapEvents, snapHdr.reliableEventCount, 0);
    }
    screenNetSetupTankGoCS(humanSim);
    printf("[WASM] Single-player ServerSim ready\n");
  }

  screenSetAiTypeCS(humanSim, compTanks);
  clientMutexWaitFor();
  screenSetTankAutoSlowdownCS(humanSim, useAutoslow);
  screenSetTankAutoHideGunsightCS(humanSim, useAutohide);
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
    useAutoslow = screenGetTankAutoSlowdownCS(humanSim);
    useAutohide = screenGetTankAutoHideGunsightCS(humanSim);
  }
  brainsHandlerShutdown();
  if (wasmTransportActive) {
    if (wasmServerSim != NULL) {
      /* Local transport: free server tanks, then local transport */
      BYTE i;
      for (i = 0; i < MAX_TANKS; i++) {
        if (wasmServerSim->sim.tanks[i] != NULL) {
          tankDestroy(&wasmServerSim->sim, &wasmServerSim->sim.tanks[i]);
        }
      }
      transportLocalDestroy(&wasmTransport);
      free(wasmServerSim);
      wasmServerSim = NULL;
    } else {
      /* UDP transport */
      transportUdpClientDestroy(&wasmTransport);
    }
    wasmTransportActive = FALSE;
  }
  clientSimDestroy(humanSim);
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
  screenSetAiTypeCS(humanSim, compTanks);
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

bool gameFrontLoadDeferredMap(ClientSim *cs)   { (void)cs; return FALSE; }
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


ServerSim *gameFrontGetServerSim(void) {
  return wasmServerSim;
}

Transport *gameFrontGetTransport(void) {
  return wasmTransportActive ? &wasmTransport : NULL;
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

void gameFrontHandleUrlOpen(char *url) {
  (void)url;
}

void gameFrontUpdateSteamPresence(ClientSim *cs) {
  (void)cs;
}
