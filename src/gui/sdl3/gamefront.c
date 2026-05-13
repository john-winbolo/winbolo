/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
*Name:          Game Front
*Filename:      gamefront.c (SDL3)
*Author:        John Morrison
*Purpose:
*  SDL3 cross-platform game front-end: init/shutdown,
*  setup dialog state machine, preferences I/O,
*  and all functions declared in gamefront.h.
*
*  Uses GetPrivateProfileString / WritePrivateProfileString
*  provided by server/posix_stubs on non-Win32 platforms.
*********************************************************/

/* MSVC: include crtdbg before SDL to avoid _malloca redefinition warning */
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

/* SDL3 must come before bolo headers (#pragma pack assertions). */
#include <SDL3/SDL.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#if defined(__IPHONEOS__)
#include <dirent.h>
#endif
#ifndef _MSC_VER
#include <strings.h>
#endif
#include <ctype.h>
#include <time.h>

#include "../../common/wb_log.h"
#include "client_mapload.h"
#include "client_sim.h"
#include "client_snapshot.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "global.h"
#include "players.h"
#include "gui_message.h"
#include "frontend.h"
#include "../brainsHandler.h"
#include "../clientmutex.h"
#include "../gamefront.h"
#include "../../server/threads.h"
#include "../input.h"
#include "../lang.h"
#include "../sound.h"
#include "../winbolo.h"
#include "sdl3draw.h"
#include "sdl3imgui.h"
#include "luabrainshandler.h"
#include "dialog_backend.h"
#include "dialogs/imgui_messagebox.h"
#include "bg_game.h"

#include "everard_map.h"
#include "platform_net.h"
#include "playername_validate.h"
#include "client_net.h"
#include "../../server/server_lifecycle.h"
#include "../../winbolonet/winbolonet.h"
#include "../../steam/steam_wrapper.h"
#include "../../mapeditor/mapeditor.h"
#include "../../mapeditor/mapeditor_generate.h"
/* Forward declaration only — don't include logviewer.h to avoid type conflicts
   between src/logviewer/ and src/bolo/ headers (both define map, bases, etc.) */
void logViewerRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  const char *logPath, bool fromMainMenu);
void logViewerRunFromMemory(struct SDL_Window *window, struct SDL_Renderer *renderer,
                            uint8_t *zipData, size_t zipLen, bool fromMainMenu);

#include "dialogs/imgui_wbn_browser.h"

#ifndef DEFAULT_UDP_PORT
#define DEFAULT_UDP_PORT 27500
#endif

/* Cross-platform INI file stubs — provided by posix_stubs on non-Win32 */
#ifndef _WIN32
extern void preferencesGetPreferenceFile(char *dest);
extern void preferencesSetPreferenceFileOverride(const char *path);
extern DWORD GetPrivateProfileString(const char *section, const char *key,
                                      const char *def, char *dest,
                                      DWORD size, const char *file);
extern int WritePrivateProfileString(const char *section, const char *key,
                                      const char *value, const char *file);
#endif

/* Number of bot players for local/practice games */
#define LOCAL_GAME_NUM_BOTS 0

/* -------------------------------------------------------
 * getPreferenceFilePath — return absolute path to WinBolo.ini
 *
 * Uses SDL_GetPrefPath so settings survive across sessions
 * regardless of CWD.  On Windows this also avoids the Win32
 * WritePrivateProfileString pitfall of writing to C:\Windows.
 * ------------------------------------------------------- */
static const char *getPreferenceFilePath(void) {
  static char path[FILENAME_MAX];
  static bool resolved = false;
  if (!resolved) {
    const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir) {
      snprintf(path, sizeof(path), "%sWinBolo.ini", prefDir);
    } else {
      /* Fallback to relative path if SDL_GetPrefPath fails */
      snprintf(path, sizeof(path), "%s", "WinBolo.ini");
    }
    resolved = true;
  }
  return path;
}

/* Forward declarations */
static bool gameFrontDialogs(void);
extern void sdl3MessageHandler(const char *message, const char *title);

/* Find the brain script — try several paths */
static bool findBrainPath(char *out, size_t outLen) {
    const char *candidates[] = {
        "Brains/NewAutopilot/init.lua",
        "brains/NewAutopilot/init.lua",
        "data/Brains/NewAutopilot/init.lua",
    };
    for (int i = 0; i < 3; i++) {
        FILE *f = fopen(candidates[i], "r");
        if (f) {
            fclose(f);
            snprintf(out, outLen, "%s", candidates[i]);
            return true;
        }
    }
    return false;
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
int gameFrontBotCount = LOCAL_GAME_NUM_BOTS;
char gameFrontBrainPath[FILENAME_MAX] = "";
GameFrontBotSetup gameFrontBotSetupData = {0};

/* UDP stuff */
char gameFrontName[PLAYER_NAME_LEN];
char gameFrontUdpAddress[FILENAME_MAX];
unsigned short gameFrontMyUdp;
unsigned short gameFrontTargetUdp;

char gameFrontTrackerAddr[FILENAME_MAX];
unsigned short gameFrontTrackerPort;
bool gameFrontTrackerEnabled;

bool gameFrontUseUpnp         = TRUE;
bool gameFrontUseNatTraversal = TRUE;

/* Tutorial: shown on the welcome menu until the player completes it.
 * Defaults to TRUE on a fresh install (key absent from INI). The player
 * can toggle it back on from the Settings dialog at any time. */
static bool gameFrontShowTutorialButton = TRUE;

/* Country-flag rendering in chat / newswire / players panels. Default
 * TRUE; WBN and Steam badges are always shown when present and are not
 * gated on this preference. */
static bool gameFrontShowCountryFlagsInChat = TRUE;

/* Persisted BCP-47 language code (e.g. "en", "de", "pt-br"). Empty
 * string means the user has not picked one yet — Phase 5 startup runs
 * langAutoDetect() in that case. */
static char gameFrontLanguageCode[32] = "";

/* One-shot flag set by the Settings dialog's "Play Tutorial" button.
 * Consumed by the openSettings handler in gameFrontDialogs() so that
 * settings → tutorial transitions in one menu cycle. */
static bool gameFrontPlayTutorialRequested = FALSE;

/* Winbolo.net settings */
char gameFrontWbnToken[FILENAME_MAX];
char gameFrontWbnTokenExpiry[FILENAME_MAX];
bool gameFrontWbnUse;

/* Dialog window position (separate from game window) */
int gameFrontDialogX = -1;
int gameFrontDialogY = -1;

/* Dialog states */
openingStates dlgState = openStart;

bool isServer = FALSE;

bool useAutoslow;
bool useAutohide;

bool wantRejoin;

/* Human player's ClientSim — owned by the frontend, allocated lazily
 * via clientSimAlloc when entering a session and freed via
 * clientSimDestroy when leaving. */
ClientSim *humanSim = NULL;

/* Server-authoritative single-player state */
static ServerSim *spServerSim = NULL;
static SubscriberHandle spHumanSubHandle = SUBSCRIBER_HANDLE_INVALID;

static void humanDeliverControl(void *ctx, const ControlEvent *evt) {
    clientSimApplyControl((ClientSim *)ctx, evt);
}
static bool spServerSimActive = FALSE;
static bool spTransportLocalUsed = FALSE;
static bool spServerHosted = FALSE;
static SDL_TimerID hostedServerTimerID = 0;

static Uint32 SDLCALL hostedServerTimerCb(void *userdata, SDL_TimerID id, Uint32 interval) {
  (void)userdata; (void)id;
  /* Read spServerSim under the mutex so a concurrent shutdown can NULL
   * it out without us racing with a freed pointer cached on this stack
   * frame.  serverInstanceTick re-takes the mutex internally; the
   * threading mutex is recursive on both Windows and SDL3. */
  threadsWaitForMutex();
  if (spServerHosted && spServerSim != NULL) {
    serverInstanceTick(spServerSim);
  }
  threadsReleaseMutex();
  return interval;
}

/* UDP multiplayer transport state — the Transport handle itself now
 * lives inside humanSim; these flags only track whether a UDP join
 * is active for higher-level lifecycle gating. */
static bool udpTransportActive = FALSE;
static BYTE udpPlayerNum = 0;

/* Send callbacks for ClientSim — route through the client_net.h wrappers.
 * (The callback layer is retained for this transition; future cleanup
 * will let ClientSim callers call clientSimNetSend* directly.) */
static void gameFrontChatSendCallback(uint8_t destPlayer, const char *message) {
    clientSimNetSendChat(humanSim, destPlayer, message);
}

static void gameFrontNameChangeSendCallback(const char *newName) {
    clientSimNetSendNameChange(humanSim, newName);
}

static void gameFrontAllianceRequestCallback(uint8_t toPlayer) {
    clientSimNetSendAllianceRequest(humanSim, toPlayer);
}

static void gameFrontAllianceAcceptCallback(uint8_t toPlayer) {
    clientSimNetSendAllianceAccept(humanSim, toPlayer);
}

static void gameFrontAllianceLeaveCallback(void) {
    clientSimNetSendAllianceLeave(humanSim);
}

static void gameFrontLockToggleCallback(bool allow) {
    clientSimNetSendLockToggle(humanSim, allow);
}

/* -------------------------------------------------------
 * Steam rich presence helpers
 * ------------------------------------------------------- */
void gameFrontUpdateSteamPresence(ClientSim *cs) {
  if (cs == NULL) return;
  char status[256];
  BYTE numPlayers = clientSimGetNumPlayers(cs);
  snprintf(status, sizeof(status), "On map '%s' - %d player%s",
           clientSimGetMapName(cs), numPlayers, numPlayers == 1 ? "" : "s");
  steam_set_rich_presence("status", status);
  steam_set_rich_presence("steam_display", "#StatusWithMap");

  /* Set connect string so friends see a "Join Game" button */
  if (udpTransportActive && gameFrontUdpAddress[0] != '\0') {
    char connect[FILENAME_MAX];
    snprintf(connect, sizeof(connect), "+connect %.255s:%u",
             gameFrontUdpAddress, (unsigned)gameFrontTargetUdp);
    steam_set_rich_presence("connect", connect);
  }
}

extern bool isTutorial;

/* Used to set the preferences — defined in winbolo.c */
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
extern bool smoothScrollingEnabled;
extern BYTE zoomFactor;
extern bool showPillLabels;
extern bool showBaseLabels;
extern bool labelSelf;
extern labelLen labelMsg;
extern labelLen labelTank;

/* Helper: itoa replacement for portability */
static void intToStr(int val, char *buf, int bufSize) {
  snprintf(buf, bufSize, "%d", val);
}

static void longToStr(long val, char *buf, int bufSize) {
  snprintf(buf, bufSize, "%ld", val);
}

/* -------------------------------------------------------
 * gameFrontStart — initialise game subsystems
 * ------------------------------------------------------- */
bool gameFrontStart(const char *cmdLine, keyItems *keys, bool isLoaded, ClientSim **out_cs) {
  bool OKStart;

  isTutorial = FALSE;
  password[0] = '\0';
  gameFrontName[0] = '\0';
  gameFrontUdpAddress[0] = '\0';
  wantRejoin = FALSE;

  /* Pin posix_stubs / http.c / map editor / log viewer to the same WinBolo.ini
   * the SDL3 client uses (SDL_GetPrefPath). Without this, http.c's
   * preferencesGetPreferenceFile would hit posix_stubs' headless fallback
   * (~/.config/winbolo/) and read [WINBOLO.NET] Host from a different file
   * than where Token gets written. */
#ifndef _WIN32
  preferencesSetPreferenceFileOverride(getPreferenceFilePath());
#endif

  langSetup();

  /* Read preferences */
  gameFrontGetPrefs(keys, &useAutoslow, &useAutohide);

  /* Apply persisted language, or auto-detect if this is a fresh
   * install (empty Language slot in the INI). Either way, this runs
   * before any dialog draws so langGetText() returns the right text
   * on the very first frame. */
  if (gameFrontLanguageCode[0] != '\0') {
    /* Try to load the file matching the saved code. If it's gone
     * (deleted, renamed) silently fall back to English. */
    if (strcmp(gameFrontLanguageCode, "en") != 0) {
      char langPath[FILENAME_MAX];
      snprintf(langPath, sizeof(langPath), "data/lang/%s.txt",
               gameFrontLanguageCode);
      if (!langLoadFile(langPath)) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET, "gameFrontStart: persisted language '%s' not found — "
                "falling back to English",
                gameFrontLanguageCode);
      }
    }
  } else {
    char detected[32];
    detected[0] = '\0';
    langAutoDetect(detected, (int)sizeof(detected));
    if (detected[0] != '\0') {
      gameFrontSetLanguageCode(detected);
    }
  }

  /* Process the command line argument.
     If fileName already contains a pending winbolo:// URL (set by
     gameFrontHandleUrlOpen), keep it so the URL handler below picks it up. */
  if (strncmp(fileName, "winbolo://", 10) != 0) {
    if (cmdLine != NULL) {
      strncpy(fileName, cmdLine, FILENAME_MAX - 1);
      fileName[FILENAME_MAX - 1] = '\0';
    } else {
      fileName[0] = '\0';
    }
  }

  /* Initialise game subsystems */
  OKStart = TRUE;
  if (isLoaded == FALSE) {
    if (!SDL_Init(0)) {
      OKStart = FALSE;
    }

    {
      /* For custom mode, use ceiling integer zoom so render target >= window.
         sdl3DrawAdaptRenderTarget will adjust dynamically on resize. */
      BYTE zf = windowGetZoomFactor();
      if (zf == ZOOM_FACTOR_CUSTOM) zf = ZOOM_FACTOR_DOUBLE;
      if (sdl3DrawSetup(zf) == FALSE) {
        OKStart = FALSE;
      }
    }

    if (soundSetup() == FALSE) {
      /* Sound failure is non-fatal — disable sound */
      soundEffects = FALSE;
    }

    if (brainsHandlerLoadBrains() == FALSE) {
      /* Brain loading failure is non-fatal */
    }
  }

  if (OKStart == FALSE) {
    gameFrontEnd(keys, FALSE, TRUE);
    return FALSE;
  }

  /* Register winbolo:// URL protocol handler (Windows only; other
     platforms use Info.plist / .desktop file declarations). */
  gameFrontSetRegistryKeys();

  /* Handle winbolo:// URL links */
  dlgState = openStart;
  if (strncmp(fileName, "winbolo://", 10) == 0) {
    if (strcmp(fileName, "winbolo:///") != 0) {
      gameFrontSetAddressFromWebLink(fileName);
      dlgState = openInternetManual;
    }
    fileName[0] = '\0';
  }

  /* Player name is set on the ClientSim after clientSimCreate (see below) */

  guiMessageSetHandler(sdl3MessageHandler);
  if (gameFrontDialogs() == FALSE) {
    gameFrontEnd(keys, FALSE, TRUE);
    return FALSE;
  }

  clientSimSetAiType(humanSim, compTanks);

  if (isTutorial == FALSE) {
    clientMutexWaitFor();
    clientSimSetTankAutoSlowdown(humanSim, useAutoslow);
    clientSimSetTankAutoHideGunsight(humanSim, useAutohide);
    clientMutexRelease();
  }

  if (out_cs) {
    *out_cs = humanSim;
  }
  return TRUE;
}

/* -------------------------------------------------------
 * gameFrontSaveTankPrefs — copy auto-slowdown and
 * auto-hide-gunsight from the tank into frontend globals
 * so they survive a return-to-lobby (which skips
 * gameFrontEnd).
 * ------------------------------------------------------- */
void gameFrontSaveTankPrefs(ClientSim *cs) {
  if (cs != NULL) {
    useAutoslow = clientSimGetTankAutoSlowdown(cs);
    useAutohide = clientSimGetTankAutoHideGunsight(cs);
  }
}

/* -------------------------------------------------------
 * gameFrontEnd — shutdown game subsystems
 * ------------------------------------------------------- */
void gameFrontEnd(keyItems *keys, bool gamePlayed, bool isQuiting) {
  steam_clear_rich_presence();
  clientMutexWaitFor();
  if (gamePlayed == TRUE && humanSim != NULL) {
    useAutoslow = clientSimGetTankAutoSlowdown(humanSim);
    useAutohide = clientSimGetTankAutoHideGunsight(humanSim);
  }
  brainsHandlerShutdown();
  /* Clean up server-authoritative single-player state.
   * clientSimDestroy() frees the shared game objects (map, bases, etc.) and
   * the client's predicted tank. The server's tanks are separate deep copies
   * since Phase 4 prediction, so we must free them here before screenDestroy
   * invalidates the shared map/bases/pills pointers.
   * We still must NOT call serverSimDestroy (would double-free map etc.). */
  if (spServerSimActive && !spServerHosted) {
    /* Destroy bot brains before cleaning up tanks */
    serverSimDestroyBots(spServerSim);
    /* Free server's tanks — they're separate from the client's predicted
     * tanks (which are deep copies since Phase 4 prediction).
     * LGMs are still shared (not deep-copied), so screenDestroy frees them. */
    {
      GameSim *gs = serverSimGetGameSim(spServerSim);
      BYTE i;
      for (i = 0; i < MAX_TANKS; i++) {
        if (gs->tanks[i] != NULL) {
          tankDestroy(gs, &gs->tanks[i]);
        }
      }
    }
    if (spTransportLocalUsed) {
      serverSimUnregisterSubscriber(spServerSim, spHumanSubHandle);
      spHumanSubHandle = SUBSCRIBER_HANDLE_INVALID;
      clientSimDisconnect(humanSim);
      spTransportLocalUsed = FALSE;
    }
    serverSimClearActive(spServerSim);
    free(spServerSim);
    spServerSim = NULL;
    spServerSimActive = FALSE;
  }
  if (udpTransportActive) {
    clientSimSetChatSendFunc(humanSim, NULL);
    clientSimSetNameChangeSendFunc(humanSim, NULL);
    clientSimSetAllianceRequestFunc(humanSim, NULL);
    clientSimSetAllianceAcceptFunc(humanSim, NULL);
    clientSimSetAllianceLeaveFunc(humanSim, NULL);
    clientSimSetLockToggleSendFunc(humanSim, NULL);
    clientSimDisconnect(humanSim);
    udpTransportActive = FALSE;
  }
  /* Don't call windowSaveCurrentPosition() here - we already save the corrected
     position on every resize/move. Calling it here would overwrite the corrected
     position (e.g. centered within maximized bounds) with the actual position. */
  gameFrontPutPrefs(keys);
  if (humanSim != NULL) {
    frontEndSetActiveClientSim(NULL);
    netDestroy(humanSim);
    clientSimDestroy(humanSim);
    humanSim = NULL;
  }
  if (isQuiting == TRUE) {
    sdl3ImguiCleanup();
    sdl3DrawCleanup();
    soundCleanup();
    langCleanup();
  }
  if (isServer == TRUE) {
    gameFrontShutdownServer();
    isServer = FALSE;
  }
  clientMutexRelease();
  threadsDestroy();
}

/* -------------------------------------------------------
 * gameFrontDialogs — setup dialog state machine
 * ------------------------------------------------------- */
/* Pick a random .map file from data/maps/ for the background game */
static bool pickRandomMap(char *out, size_t outLen) {
    const char *dir = "data/maps";
    int count = 0;

#if defined(__IPHONEOS__)
    /* SDL_GlobDirectory doesn't work with the iOS app bundle filesystem.
     * Use opendir/readdir directly instead. */
    {
        char *mapFiles[256];
        DIR *d = opendir(dir);
        if (!d) {
            WB_LOG_WARN(WB_LOG_CAT_MAP, "[BgGame] pickRandomMap: opendir('%s') failed", dir);
            return false;
        }
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && count < 256) {
            size_t len = strlen(ent->d_name);
            if (len > 4 && strcasecmp(ent->d_name + len - 4, ".map") == 0 &&
                strcasecmp(ent->d_name, "Inbuilt Tutorial.map") != 0 &&
                strcasecmp(ent->d_name, "Better Best Map Ever.map") != 0) {
                mapFiles[count] = SDL_strdup(ent->d_name);
                count++;
            }
        }
        closedir(d);
        if (count == 0) {
            WB_LOG_WARN(WB_LOG_CAT_MAP, "[BgGame] pickRandomMap: no .map files in '%s'", dir);
            return false;
        }
        int idx = rand() % count;
        SDL_snprintf(out, outLen, "%s/%s", dir, mapFiles[idx]);
        WB_LOG_DEBUG(WB_LOG_CAT_MAP, "[BgGame] pickRandomMap: picked '%s' from %d maps", out, count);
        for (int i = 0; i < count; i++) SDL_free(mapFiles[i]);
        return true;
    }
#else
    char **list = SDL_GlobDirectory(dir, "*.map", 0, &count);
    if (!list || count == 0) {
        WB_LOG_WARN(WB_LOG_CAT_MAP, "[BgGame] pickRandomMap: SDL_GlobDirectory found 0 maps in '%s'", dir);
        if (list) SDL_free(list);
        return false;
    }
    /* Filter out maps unsuitable for the background game */
    int filtered = 0;
    for (int i = 0; i < count; i++) {
        if (SDL_strcasecmp(list[i], "Inbuilt Tutorial.map") != 0 &&
            SDL_strcasecmp(list[i], "Better Best Map Ever.map") != 0) {
            list[filtered++] = list[i];
        }
    }
    if (filtered == 0) {
        WB_LOG_WARN(WB_LOG_CAT_MAP, "[BgGame] pickRandomMap: no non-tutorial maps in '%s'", dir);
        SDL_free(list);
        return false;
    }
    int idx = rand() % filtered;
    SDL_snprintf(out, outLen, "%s/%s", dir, list[idx]);
    WB_LOG_DEBUG(WB_LOG_CAT_MAP, "[BgGame] pickRandomMap: picked '%s' from %d maps", out, filtered);
    SDL_free(list);
    return true;
#endif
}

static bool gameFrontDialogs(void) {
  bool done = FALSE;
  bool userQuit = FALSE;

  WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[BgGame] gameFrontDialogs entered, dlgState=%d", dlgState);

  /* Disable render logical presentation during dialogs so that ImGui
   * touch/mouse coordinates match the rendering coordinates.  On Android
   * (non-tablet), sdl3DrawSetup sets a logical presentation for the game
   * view which would otherwise cause a coordinate mismatch in ImGui. */
  sdl3DrawDisableLogicalPresentation();

  /* Create shared background game for all pre-game dialogs */
  BgGame bg;
  bool hasBg = false;
  {
    char mapPath[512];
    if (pickRandomMap(mapPath, sizeof(mapPath))) {
      hasBg = bgGameCreate(&bg, mapPath, sdl3DrawGetRenderer());
    }
  }
  WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[BgGame] hasBg=%d", hasBg);
  if (hasBg) bgGameSetShared(&bg);

  while (done == FALSE) {
    switch (dlgState) {
    case openStart:
      dlgState = openWelcome;
      break;
    case openWelcome: {
      const DialogBackend *db = dialogBackendGet();
      int result = db->welcomeShow();
      if (result < 0) {
        done = TRUE;
        userQuit = TRUE;
      } else {
        dlgState = (openingStates)result;
      }
      break;
    }
    case openLang:
      /* Language dialog disabled on SDL3 */
      dlgState = openWelcome;
      break;
    case openSkins:
      /* Skins dialog not yet ported */
      dlgState = openWelcome;
      break;
    case openUdp:
    case openInternetManual:
    case openLanManual: {
      const DialogBackend *db = dialogBackendGet();
      openingStates prev = dlgState;
      db->udpSetupShow();
      /* dlgState already updated by gameFrontSetDlgState inside the dialog
       * (OnJoin/OnNew/OnCancel all call gameFrontSetDlgState before EndModal).
       * If the dialog stub didn't change state, fall back to welcome. */
      if (dlgState == prev) dlgState = openWelcome;
      break;
    }
    case openSetup:
    case openInternetSetup:
    case openLanSetup:
    case openUdpSetup: {
      const DialogBackend *db = dialogBackendGet();
      openingStates prev = dlgState;
      if (db->gameSetupShow(humanSim)) {
        gameFrontSetDlgState(openFinished);
      } else {
        /* Go back to the parent dialog, not the welcome screen */
        if (prev == openInternetSetup) dlgState = openInternet;
        else if (prev == openLanSetup) dlgState = openLan;
        else if (prev == openUdpSetup) dlgState = openUdp;
        else dlgState = openWelcome;
      }
      break;
    }
    case openInternet: {
      const DialogBackend *db = dialogBackendGet();
      openingStates prev = dlgState;
      db->gameBrowserShow(langGetText(STR_GAMEFRONT_TRACKERFINDER_TITLE), TRUE);
      if (dlgState == prev) dlgState = openWelcome;
      break;
    }
    case openLan: {
      const DialogBackend *db = dialogBackendGet();
      openingStates prev = dlgState;
      db->gameBrowserShow(langGetText(STR_GAMEFRONT_LANFINDER_TITLE), FALSE);
      if (dlgState == prev) dlgState = openWelcome;
      break;
    }
    case openTutorial:
      /* Reuse the single-player setup path: preload the tutorial map,
       * disable bots, then drive the state machine through openSetup ->
       * openFinished so ServerSim / ClientSim / transport are created
       * the same way as a normal single-player game. */
      strncpy(fileName, "data/maps/Inbuilt Tutorial.map", FILENAME_MAX - 1);
      fileName[FILENAME_MAX - 1] = '\0';
      gametype = gameStrictTournament;
      hiddenMines = FALSE;
      startDelay = 0;
      timeLen = UNLIMITED_GAME_TIME;
      compTanks = aiNone;
      gameFrontBotSetupData.count = 0;
      /* Raise the client UI flag before setup runs so the very first
       * tick (which may already place the tank on a trigger row) is
       * handled by frontEndTutorial. Reset the step sequencer so a
       * previously-completed tutorial in this session starts fresh. */
      frontEndTutorialReset();
      isTutorial = TRUE;
      dlgState = openSetup;
      gameFrontSetDlgState(openFinished);
      /* Mark both sims so tank.c's tutorial stop logic fires
       * authoritatively on the server and keeps client prediction
       * consistent. */
      if (spServerSim != NULL) serverSimSetTutorial(spServerSim, true);
      if (humanSim != NULL)    clientSimSetTutorial(humanSim, true);
      break;
    case openSettings: {
      const DialogBackend *db = dialogBackendGet();
      db->settingsShow();
      /* Settings → Tutorial shortcut: if the Play Tutorial button was
       * clicked inside the settings dialog, jump straight into the
       * tutorial state instead of bouncing back to the welcome menu. */
      if (gameFrontConsumePlayTutorialRequest()) {
        dlgState = openTutorial;
      } else {
        dlgState = openWelcome;
      }
      break;
    }
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
    case openMapEditor:
      mapEditorRun(sdl3DrawGetWindow(), sdl3DrawGetRenderer(), NULL, true);
      dlgState = openWelcome;
      break;
    case openLogViewer: {
      WbnBrowserResult wbnResult = imguiWbnBrowserShow(sdl3DrawGetWindow(),
                                                       sdl3DrawGetRenderer());
      switch (wbnResult.action) {
      case WBN_BROWSER_PLAY_FILE:
        logViewerRun(sdl3DrawGetWindow(), sdl3DrawGetRenderer(), wbnResult.filePath, true);
        break;
      case WBN_BROWSER_PLAY_MEMORY:
        logViewerRunFromMemory(sdl3DrawGetWindow(), sdl3DrawGetRenderer(),
                               wbnResult.memoryData, wbnResult.memorySize, true);
        break;
      case WBN_BROWSER_OPEN_LOCAL:
        logViewerRun(sdl3DrawGetWindow(), sdl3DrawGetRenderer(), NULL, true);
        break;
      case WBN_BROWSER_CLOSE:
      default:
        break;
      }
      dlgState = openWelcome;
      break;
    }
#endif
    case openFinished:
      done = TRUE;
      break;
    default:
      done = TRUE;
      break;
    }
  }

  /* Clean up shared background game */
  bgGameSetShared(NULL);
  if (hasBg) bgGameDestroy(&bg);

  /* Restore render logical presentation for the game view (Android). */
  sdl3DrawRestoreLogicalPresentation();

  return !userQuit;
}

/* -------------------------------------------------------
 * gameFrontValidateWbnBeforeJoin — If a WBN token is stored,
 * validate it synchronously and update the player name from
 * the API response.  If the token is no longer valid, clear
 * it so the join proceeds without WBN.
 * ------------------------------------------------------- */
static void gameFrontValidateWbnBeforeJoin(void) {
  char token[256], expiry[256];
  gameFrontGetWinbolonetToken(token, expiry);

  /* If no WBN token exists, try automatic Steam authentication */
  if (token[0] == '\0') {
    uint8_t ticketBuf[1024];
    uint32_t ticketLen = 0;
    if (steam_get_auth_ticket(ticketBuf, sizeof(ticketBuf), &ticketLen) && ticketLen > 0) {
      char ticketHex[2049];
      uint32_t i;
      for (i = 0; i < ticketLen; i++) {
        snprintf(ticketHex + i * 2, 3, "%02x", ticketBuf[i]);
      }
      ticketHex[ticketLen * 2] = '\0';

      char tokenOut[256], expiryOut[256];
      char playerName[PLAYER_NAME_LEN];
      char errorMsg[512];
      tokenOut[0] = '\0';
      expiryOut[0] = '\0';
      playerName[0] = '\0';
      errorMsg[0] = '\0';

      if (winbolonetAuthSteam(ticketHex, tokenOut, expiryOut, playerName, errorMsg)) {
        gameFrontSetWinbolonetToken(tokenOut, expiryOut);
        if (playerName[0] != '\0') {
          char persisted[PLAYER_NAME_LEN];
          persisted[0] = '\0';
          gameFrontGetPlayerName(persisted);

          if (persisted[0] == '\0') {
            /* First-launch seed: persisted name is empty.  Run the Steam
             * persona through Phase 2 validation; fall back to the app
             * default name on rejection. */
            char validated[PLAYER_NAME_LEN];
            if (playerNameValidate(playerName, validated, PLAYER_NAME_LEN, NULL)) {
              gameFrontSetPlayerName(validated);
            } else {
              gameFrontSetPlayerName((char *)langGetText(STR_DLGGAMESETUP_DEFAULTNAME));
            }
          }
          /* Otherwise: keep the user's chosen name.  The Steam persona
           * is NOT used to update an existing name (Phase 7 / Decision 3). */
        }
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Steam] Authenticated with WinBolo.net via Steam");
      } else {
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Steam] WBN Steam auth failed: %s", errorMsg);
      }
    }
    return;
  }

  char playerName[PLAYER_NAME_LEN];
  char errorMsg[512];
  playerName[0] = '\0';
  errorMsg[0] = '\0';

  if (winbolonetAuthValidate(token, playerName, errorMsg)) {
    if (playerName[0] != '\0') {
      gameFrontSetPlayerName(playerName);
    }
  } else {
    fprintf(stderr, "[gameFront] WBN token validation failed: %s\n", errorMsg);
    gameFrontClearWinbolonetToken();
  }
}

/* -------------------------------------------------------
 * gameFrontSetDlgState — dialog state machine transitions
 * ------------------------------------------------------- */
bool gameFrontSetDlgState(openingStates newState) {
  bool returnValue = TRUE;
  openingStates prevState = dlgState;

  if ((dlgState == openInternet || dlgState == openLan || dlgState == openUdp ||
       dlgState == openLanManual || dlgState == openInternetManual) &&
      newState == openUdpJoin) {
    gameFrontValidateWbnBeforeJoin();
    humanSim = clientSimAlloc(); clientSimCreate(humanSim, 0, FALSE, 0, UNLIMITED_GAME_TIME);
    frontEndSetActiveClientSim(humanSim);
    if (gameFrontRemeber) playersSetMyLastPlayerName(humanSim, gameFrontName);
    fprintf(stderr, "[gameFront] openUdpJoin: addr=%s port=%u myPort=%u\n",
            gameFrontUdpAddress, (unsigned)gameFrontTargetUdp, (unsigned)gameFrontMyUdp);
    fflush(stderr);

    /* Create UDP client transport for the new protocol */
    clientSimConnectUdp(humanSim, gameFrontUdpAddress,
                        gameFrontTargetUdp,
                        gameFrontName, password,
                        gameFrontWbnUse ? gameFrontWbnToken : "",
                        wantRejoin,
                        gameFrontTrackerEnabled ? gameFrontTrackerAddr : "",
                        gameFrontTrackerPort);
    if (clientSimGetConnectState(humanSim) == CLIENT_CONNECT_ERROR) {
      const char *reason = clientSimGetConnectErrorReason(humanSim);
      imguiMessageBoxEx(DIALOG_BOX_TITLE,
                        (reason && reason[0]) ? reason : langGetText(STR_GAMEFRONTERR_JOINGAME),
                        IMGUI_MSG_ERROR, IMGUI_MSG_OK);
      clientSimDestroy(humanSim);
      humanSim = NULL;
      gameFrontShutdownServer();
      dlgState = prevState;
      returnValue = FALSE;
    } else {
      /* Wait for join handshake. Break early if we enter the lobby
       * (lobby-enabled servers send PACKET_LOBBY_STATE before map chunks,
       * so inLobby may become true while map is still downloading). */
      int joinWaitTicks = 0;
      while (joinWaitTicks < 1500) {  /* 30 second timeout */
        ClientConnectState js = clientSimGetConnectState(humanSim);
        if (js != CLIENT_CONNECT_JOINING && js != CLIENT_CONNECT_DOWNLOADING_MAP) break;
        if (clientSimIsInLobby(humanSim)) break;  /* Enter lobby immediately */
        clientSimNetTick(humanSim);
        SDL_Delay(20);
        joinWaitTicks++;
      }

      ClientConnectState finalState = clientSimGetConnectState(humanSim);
      if (finalState == CLIENT_CONNECT_CONNECTED || clientSimIsInLobby(humanSim)) {
        udpPlayerNum = clientSimGetServerPlayerNum(humanSim);
        udpTransportActive = TRUE;

        /* Store server address in ClientSim for brain info */
        {
          struct sockaddr_in saddr;
          memset(&saddr, 0, sizeof(saddr));
          saddr.sin_family = AF_INET;
          saddr.sin_addr.s_addr = inet_addr(gameFrontUdpAddress);
          if (saddr.sin_addr.s_addr == INADDR_NONE) {
            struct hostent *he = gethostbyname(gameFrontUdpAddress);
            if (he) memcpy(&saddr.sin_addr, he->h_addr_list[0], he->h_length);
          }
          clientSimSetServerAddress(humanSim, saddr.sin_addr);
          clientSimSetServerPort(humanSim, gameFrontTargetUdp);
        }

        clientSimSetChatSendFunc(humanSim, gameFrontChatSendCallback);
        clientSimSetNameChangeSendFunc(humanSim, gameFrontNameChangeSendCallback);
        clientSimSetAllianceRequestFunc(humanSim, gameFrontAllianceRequestCallback);
        clientSimSetAllianceAcceptFunc(humanSim, gameFrontAllianceAcceptCallback);
        clientSimSetAllianceLeaveFunc(humanSim, gameFrontAllianceLeaveCallback);
        clientSimSetLockToggleSendFunc(humanSim, gameFrontLockToggleCallback);

        if (clientSimIsInLobby(humanSim)) {
          /* Lobby path: enter lobby immediately, map downloads in background.
           * The lobby UI shows a progress bar and gates the ready button
           * on mapDownloadComplete. Deferred map loading happens when
           * the lobby exits (game start). */
          clientSimSetNetStatus(humanSim, netLobby);
          dlgState = openFinished;
        } else {
          /* No-lobby path: map already downloaded, load it now */
          const BYTE *mapData;
          int mapLen = 0;
          gameType serverGame;
          bool serverHiddenMines;
          int32_t serverStartDelay, serverGameLen;
          bool mapLoadOk = TRUE;

          mapData = clientSimGetServerMapData(humanSim, &mapLen);
          clientSimGetServerGameSettings(humanSim, &serverGame,
                                         &serverHiddenMines,
                                         &serverStartDelay,
                                         &serverGameLen);

          if (mapData != NULL && mapLen > 0) {
            char savedMapName[MAP_STR_SIZE];
            strncpy(savedMapName, clientSimGetMapName(humanSim), MAP_STR_SIZE - 1);
            savedMapName[MAP_STR_SIZE - 1] = '\0';
            gametype = serverGame;
            hiddenMines = serverHiddenMines;
            startDelay = serverStartDelay;
            timeLen = serverGameLen;

            /* Reset map-dependent state in place; clientSimResetForMapLoad
             * keeps the transport binding so the mapData pointer (which
             * lives inside the UDP transport's buffer) stays valid. */
            clientSimResetForMapLoad(humanSim);

            if (clientLoadCompressedMap(humanSim, (BYTE *)mapData, mapLen,
                                       savedMapName, serverGame,
                                       serverHiddenMines, serverStartDelay,
                                       serverGameLen, gameFrontName,
                                       (BYTE)udpPlayerNum, FALSE) == FALSE) {
              mapLoadOk = FALSE;
            } else {
              clientSimSetLocalTransport(humanSim, false);
              /* Re-register network callbacks (clientSimResetForMapLoad cleared them). */
              clientSimSetChatSendFunc(humanSim, gameFrontChatSendCallback);
              clientSimSetNameChangeSendFunc(humanSim, gameFrontNameChangeSendCallback);
              clientSimSetAllianceRequestFunc(humanSim, gameFrontAllianceRequestCallback);
              clientSimSetAllianceAcceptFunc(humanSim, gameFrontAllianceAcceptCallback);
              clientSimSetAllianceLeaveFunc(humanSim, gameFrontAllianceLeaveCallback);
              clientSimSetLockToggleSendFunc(humanSim, gameFrontLockToggleCallback);
            }
          } else {
            mapLoadOk = FALSE;
          }

          if (!mapLoadOk) {
            imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_GAMEFRONTERR_MAPLOAD),
                              IMGUI_MSG_ERROR, IMGUI_MSG_OK);
            clientSimSetChatSendFunc(humanSim, NULL);
            clientSimSetNameChangeSendFunc(humanSim, NULL);
            clientSimSetLockToggleSendFunc(humanSim, NULL);
            clientSimDisconnect(humanSim);
            udpTransportActive = FALSE;
            gameFrontShutdownServer();
            dlgState = prevState;
            returnValue = FALSE;
          } else {
            clientMutexWaitFor();
            clientNetSetupTankGo(humanSim);
            clientMutexRelease();
            gameFrontUpdateSteamPresence(humanSim);
            dlgState = openFinished;
          }
        }
      } else {
        const char *reason = clientSimGetConnectErrorReason(humanSim);
        imguiMessageBoxEx(DIALOG_BOX_TITLE,
                          (reason && reason[0]) ? reason : langGetText(NETERR_SERVERCONNECT),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
        clientSimDestroy(humanSim);
        humanSim = NULL;
        gameFrontShutdownServer();
        dlgState = prevState;
        returnValue = FALSE;
      }
    }
  } else if ((dlgState == openInternetManual || dlgState == openInternetSetup) &&
             newState == openWelcome) {
    dlgState = openInternet;
  } else if ((dlgState == openLanManual || dlgState == openLanSetup) &&
             newState == openWelcome) {
    dlgState = openLan;
  } else if ((dlgState == openUdpSetup || dlgState == openInternetSetup ||
              dlgState == openLanSetup) && newState == openFinished) {
    /* Host joins its own server via loopback UDP — same JOIN_REQUEST
     * handshake every joiner uses, so the server's join handler owns
     * slot 0 registration and the lobby/name-collision checks behave
     * identically for host and joiners. */
    gameFrontValidateWbnBeforeJoin();
    dlgState = newState;
    if (gameFrontSetupServer() == TRUE) {
      strncpy(gameFrontUdpAddress, "127.0.0.1", sizeof(gameFrontUdpAddress) - 1);
      gameFrontUdpAddress[sizeof(gameFrontUdpAddress) - 1] = '\0';
      gameFrontTargetUdp = gameFrontMyUdp;
      dlgState = (prevState == openInternetSetup) ? openInternet : openLan;
      gameFrontSetDlgState(openUdpJoin);
    } else {
      imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_GAMEFRONTERR_STARTSERVER),
                        IMGUI_MSG_ERROR, IMGUI_MSG_OK);
      dlgState = openStart;
    }
  } else if (dlgState == openSetup && newState == openFinished) {
    dlgState = openFinished;
    /* New architecture: ServerSim owns the map and all game state.
     * Create the server sim, then load the map on the client side
     * using the same compressed data the UDP path uses. */
    {
        if (strncmp(fileName, "randommap:", 10) == 0) {
          /* Random map — parse seed from "randommap:<seed>" */
          MapGenConfig cfg = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
          const char *seedStr = fileName + 10;
          if (!mapGenSeedToConfig(seedStr, &cfg)) {
              WB_LOG_WARN(WB_LOG_CAT_MAP, "failed to parse random map seed '%s', using defaults", seedStr);
          }
          cfg.x1 = MAP_MINE_EDGE_LEFT + 1; cfg.y1 = MAP_MINE_EDGE_TOP + 1;
          cfg.x2 = MAP_MINE_EDGE_RIGHT - 1; cfg.y2 = MAP_MINE_EDGE_BOTTOM - 1;
          spServerSim = serverSimCreateRandomMap(&cfg, gametype, hiddenMines, startDelay, timeLen);
        } else if (strcmp(fileName, "") != 0) {
          spServerSim = serverSimCreate(fileName, gametype, hiddenMines, startDelay, timeLen);
        } else {
          BYTE emap[6000] = E_MAP;
          spServerSim = serverSimCreateCompressed(emap, 5097, gametype, hiddenMines, startDelay, timeLen);
        }
        if (spServerSim != NULL) {
          /* Single-player: no lobby, run immediately */
          serverSimSetLobbyEnabled(spServerSim, false);
          serverSimStartGame(spServerSim);
          serverSimAddPlayer(spServerSim, 0, gameFrontName, false);
          serverSimSetViewPlayer(spServerSim, 0);
          spTransportLocalUsed = TRUE;
          spServerSimActive = TRUE;
          /* Load map/bases/pills on client via compressed map (same as UDP path) */
          humanSim = clientSimAlloc();
          clientSimConnectLocal(humanSim, spServerSim, 0);
          frontEndSetActiveClientSim(humanSim);
          {
            BYTE compressedMap[65536];
            int compLen = serverSimGetCompressedMap(spServerSim, compressedMap);
            if (compLen > 0) {
              clientLoadCompressedMap(humanSim, compressedMap, compLen, serverSimGetMapName(spServerSim),
                                     gametype, hiddenMines, startDelay,
                                     timeLen, gameFrontName, 0, FALSE);
            } else {
              clientSimCreate(humanSim, gametype, hiddenMines, startDelay, timeLen);
            }
          }
          if (gameFrontRemeber) playersSetMyLastPlayerName(humanSim, gameFrontName);
          /* Set up networking state after ClientSim is fully initialized */
          netSetup(humanSim, netSingle, gameFrontMyUdp, gameFrontUdpAddress, gameFrontTargetUdp,
                   password, TRUE, gameFrontTrackerAddr, gameFrontTrackerPort,
                   gameFrontTrackerEnabled, wantRejoin, gameFrontWbnUse,
                   gameFrontWbnToken);
          /* Sync tank state from initial snapshot */
          {
            SnapshotHeader snapHdr;
            TankSnapshot snapTanks[MAX_TANKS];
            ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
            TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
            BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
            PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
            GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
            serverSimBuildSnapshot(spServerSim, 0, &snapHdr,
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
          /* Register humanSim as a control-event subscriber. Placed after
           * clientSimCreate (run from the clientLoadCompressedMap / else
           * branch above) so myPlayerNum is initialized to 0 — matching the
           * SP slot — and the dispatcher's self-skip protects this slot
           * during sync. */
          spHumanSubHandle = serverSimRegisterSubscriber(spServerSim,
                                                        humanDeliverControl,
                                                        humanSim);
          clientNetSetupTankGo(humanSim);
          /* Destroy background game before adding real bots — bgGameDestroy
           * calls serverSimDestroyBots which would wipe bots we add below. */
          {
            BgGame *sharedBg = bgGameGetShared();
            if (sharedBg != NULL) {
              bgGameDestroy(sharedBg);
              bgGameSetShared(NULL);
            }
          }
          /* Add bot brains for local game if AI is enabled */
          if (!serverSimBotPoolInit(0)) {
            fprintf(stderr, "[gameFront] serverSimBotPoolInit failed; bots disabled for this session\n");
          } else {
            /* Resolve brain path for lobby "Add Bot" support and initial bots */
            char brainPath[FILENAME_MAX];
            bool haveBrain = false;
            if (gameFrontBrainPath[0] != '\0') {
              SDL_strlcpy(brainPath, gameFrontBrainPath, sizeof(brainPath));
              haveBrain = true;
            } else if (compTanks != aiNone) {
              haveBrain = findBrainPath(brainPath, sizeof(brainPath));
            }
            /* Set botBrainPath on the ServerSim so lobby Add Bot requests work */
            if (haveBrain) {
              serverSimSetBotBrainPath(spServerSim, brainPath);
              serverSimSetBotAiType(spServerSim, compTanks);
            }
            if (compTanks != aiNone && gameFrontBotSetupData.count > 0 && haveBrain) {
              for (int bi = 0; bi < gameFrontBotSetupData.count && bi < MAX_BOT_SLOTS; bi++) {
                BYTE slot = (BYTE)(bi + 1);
                char botName[32];
                snprintf(botName, sizeof(botName), "Bot %d", slot);
                /* Use per-bot brain path if set, otherwise fall back to default */
                const char *botBrain = gameFrontBotSetupData.bots[bi].brainPath;
                if (botBrain[0] == '\0') botBrain = brainPath;
                serverSimCreateBot(spServerSim, slot, botBrain, botName, compTanks, gametype, hiddenMines);
                /* Apply team number */
                uint8_t team = gameFrontBotSetupData.bots[bi].teamNumber;
                if (team > 0) {
                  serverSimSetTeam(spServerSim, slot, team);
                  {
                    ControlEvent slotEvt;
                    memset(&slotEvt, 0, sizeof(slotEvt));
                    serverSimFillLobbySlotEvent(spServerSim, slot, &slotEvt);
                    serverSimPublishControl(spServerSim, &slotEvt);
                  }
                }
              }
              /* Apply human player team number */
              if (gameFrontBotSetupData.playerTeamNumber > 0) {
                serverSimSetTeam(spServerSim, 0, gameFrontBotSetupData.playerTeamNumber);
                {
                  ControlEvent slotEvt;
                  memset(&slotEvt, 0, sizeof(slotEvt));
                  serverSimFillLobbySlotEvent(spServerSim, 0, &slotEvt);
                  serverSimPublishControl(spServerSim, &slotEvt);
                }
              }
              /* Apply team alliances — players with same non-zero team become allies. */
              for (int a = 0; a < 16; a++) {
                if (!serverSimIsPlayerConnected(spServerSim, (BYTE)a)) continue;
                if (serverSimGetLobbyPlayer(spServerSim, (BYTE)a)->teamNumber == 0) continue;
                for (int b = a + 1; b < 16; b++) {
                  if (!serverSimIsPlayerConnected(spServerSim, (BYTE)b)) continue;
                  if (serverSimGetLobbyPlayer(spServerSim, (BYTE)b)->teamNumber == serverSimGetLobbyPlayer(spServerSim, (BYTE)a)->teamNumber) {
                    GameSim *gs = serverSimGetGameSim(spServerSim);
                    ControlEvent allyEvt;
                    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, (BYTE)a, (BYTE)b, TRUE);
                    memset(&allyEvt, 0, sizeof(allyEvt));
                    allyEvt.type = CTRL_ALLIANCE_ACCEPT;
                    allyEvt.u.allianceAccept.acceptedBy = (BYTE)a;
                    allyEvt.u.allianceAccept.newMember  = (BYTE)b;
                    serverSimPublishControl(spServerSim, &allyEvt);
                  }
                }
              }
            }
          }
          gameFrontUpdateSteamPresence(humanSim);
        } else {
          if (spServerSim != NULL) {
            free(spServerSim);
            spServerSim = NULL;
          }
          returnValue = FALSE;
        }
      }
  } else {
    dlgState = newState;
  }

  return returnValue;
}

/* -------------------------------------------------------
 * Simple getter/setter functions
 * ------------------------------------------------------- */

void gameFrontGetCmdArg(char *getName) {
  strcpy(getName, fileName);
}

void gameFrontSetFileName(char *getName) {
  strcpy(fileName, getName);
}

void gameFrontSetGameOptions(char *pword, gameType gt, bool hm, aiType ai, int32_t sd, int32_t tlimit, bool justPass) {
  strcpy(password, pword);
  if (justPass == FALSE) {
    gametype = gt;
    hiddenMines = hm;
    compTanks = ai;
    if (compTanks == aiNone) {
      brainsHandlerSet(FALSE);
    } else {
      brainsHandlerSet(TRUE);
    }
    startDelay = sd;
    timeLen = tlimit;
  }
}

void gameFrontGetGameOptions(char *pword, gameType *gt, bool *hm, aiType *ai, int32_t *sd, int32_t *tlimit) {
  strcpy(pword, password);
  *gt = gametype;
  *hm = hiddenMines;
  *ai = compTanks;
  *sd = startDelay;
  *tlimit = timeLen;
}

void gameFrontSetBotOptions(int count, const char *brainPath) {
  gameFrontBotCount = count;
  if (brainPath) {
    SDL_strlcpy(gameFrontBrainPath, brainPath, FILENAME_MAX);
  } else {
    gameFrontBrainPath[0] = '\0';
  }
}

void gameFrontGetBotOptions(int *count, char *brainPath, size_t brainPathSize) {
  *count = gameFrontBotCount;
  SDL_strlcpy(brainPath, gameFrontBrainPath, brainPathSize);
}

void gameFrontSetBotSetup(const GameFrontBotSetup *setup) {
  memcpy(&gameFrontBotSetupData, setup, sizeof(GameFrontBotSetup));
  /* Keep legacy fields in sync */
  gameFrontBotCount = setup->count;
  if (setup->count > 0 && setup->bots[0].brainPath[0] != '\0') {
    SDL_strlcpy(gameFrontBrainPath, setup->bots[0].brainPath, FILENAME_MAX);
  } else {
    gameFrontBrainPath[0] = '\0';
  }
}

void gameFrontGetBotSetup(GameFrontBotSetup *setup) {
  memcpy(setup, &gameFrontBotSetupData, sizeof(GameFrontBotSetup));
}

void gameFrontGetUdpOptions(char *pn, char *add, unsigned short *theirUdp, unsigned short *myUdp) {
  strcpy(pn, gameFrontName);
  strcpy(add, gameFrontUdpAddress);
  *myUdp = gameFrontMyUdp;
  *theirUdp = gameFrontTargetUdp;
}

void gameFrontSetUdpOptions(char *pn, char *add, unsigned short theirUdp, unsigned short myUdp) {
  strcpy(gameFrontName, pn);
  strcpy(gameFrontUdpAddress, add);
  gameFrontMyUdp = myUdp;
  gameFrontTargetUdp = theirUdp;
}

void gameFrontGetPassword(char *pword) {
  password[0] = '\0';
  sdl3ImguiShowPassword();
  /* TODO: this needs to block until the ImGui modal returns.
   * For now just return the current password. */
  strcpy(pword, password);
}

void gameFrontGetPlayerName(char *pn) {
  strcpy(pn, gameFrontName);
}

void gameFrontSetPlayerName(char *pn) {
  strcpy(gameFrontName, pn);
}

void gameFrontSetAIType(aiType ait) {
  compTanks = ait;
  if (humanSim != NULL) {
    clientSimSetAiType(humanSim, compTanks);
  }
  if (compTanks == aiNone) {
    brainsHandlerSet(FALSE);
  } else {
    brainsHandlerSet(TRUE);
  }
}

void gameFrontSetRemeber(bool isSet) {
  gameFrontRemeber = isSet;
}

bool gameFrontGetRemeber(void) {
  return gameFrontRemeber;
}

bool gameFrontGetShowTutorialButton(void) {
  return gameFrontShowTutorialButton;
}

void gameFrontSetShowTutorialButton(bool show) {
  gameFrontShowTutorialButton = show;
  /* Persist immediately so a crash or hard quit after completing the
   * tutorial doesn't leave the welcome-menu entry showing again. */
  WritePrivateProfileString("SETTINGS", "Show Tutorial Button",
                            TRUEFALSE_TO_STR(show),
                            getPreferenceFilePath());
}

bool gameFrontGetShowCountryFlagsInChat(void) {
  return gameFrontShowCountryFlagsInChat;
}

void gameFrontSetShowCountryFlagsInChat(bool show) {
  gameFrontShowCountryFlagsInChat = show;
  WritePrivateProfileString("SETTINGS", "Show Country Flags In Chat",
                            TRUEFALSE_TO_STR(show),
                            getPreferenceFilePath());
}

void gameFrontGetLanguageCode(char *out, int outSize) {
  if (!out || outSize <= 0) return;
  size_t n = strlen(gameFrontLanguageCode);
  if (n >= (size_t)outSize) n = (size_t)outSize - 1;
  memcpy(out, gameFrontLanguageCode, n);
  out[n] = '\0';
}

void gameFrontSetLanguageCode(const char *code) {
  if (!code) code = "";
  size_t n = strlen(code);
  if (n >= sizeof(gameFrontLanguageCode)) n = sizeof(gameFrontLanguageCode) - 1;
  memcpy(gameFrontLanguageCode, code, n);
  gameFrontLanguageCode[n] = '\0';
  /* Persist immediately so the picked language survives a hard quit
   * even if the user never reaches gameFrontPutPrefs. */
  WritePrivateProfileString("SETTINGS", "Language",
                            gameFrontLanguageCode, getPreferenceFilePath());
}

void gameFrontRequestPlayTutorial(void) {
  gameFrontPlayTutorialRequested = TRUE;
}

bool gameFrontConsumePlayTutorialRequest(void) {
  bool was = gameFrontPlayTutorialRequested;
  gameFrontPlayTutorialRequested = FALSE;
  return was;
}

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

void gameFrontEnableRejoin(void) {
  wantRejoin = TRUE;
}

static void gameFrontSaveWbnTokenToPrefs(void) {
  const char *prefsFile = getPreferenceFilePath();
  WritePrivateProfileString("WINBOLO.NET", "Token", gameFrontWbnToken, prefsFile);
  WritePrivateProfileString("WINBOLO.NET", "TokenExpiry", gameFrontWbnTokenExpiry, prefsFile);
}

void gameFrontSetWinbolonetToken(const char *token, const char *expiry) {
  SDL_strlcpy(gameFrontWbnToken, token, FILENAME_MAX);
  SDL_strlcpy(gameFrontWbnTokenExpiry, expiry, FILENAME_MAX);
  gameFrontWbnUse = (token[0] != '\0');
  gameFrontSaveWbnTokenToPrefs();
}

void gameFrontGetWinbolonetToken(char *token, char *expiry) {
  strcpy(token, gameFrontWbnToken);
  strcpy(expiry, gameFrontWbnTokenExpiry);
}

void gameFrontClearWinbolonetToken(void) {
  gameFrontWbnToken[0] = '\0';
  gameFrontWbnTokenExpiry[0] = '\0';
  gameFrontWbnUse = FALSE;
  gameFrontSaveWbnTokenToPrefs();
}

bool gameFrontGetWinbolonetUse(void) {
  return gameFrontWbnUse;
}

void gameFrontSetRegistryKeys(void) {
#ifdef _WIN32
  /* Register winbolo:// URL protocol handler in the Windows registry.
     Creates: HKCU\Software\Classes\winbolo with URL Protocol and
     a shell\open\command pointing to the current executable. */
  HKEY hKey;
  char exePath[MAX_PATH];
  char command[MAX_PATH + 16];

  GetModuleFileNameA(NULL, exePath, MAX_PATH);
  snprintf(command, sizeof(command), "\"%s\" \"%%1\"", exePath);

  if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Classes\\winbolo", 0, NULL,
                       0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
    const char *desc = "WinBolo Game Link";
    RegSetValueExA(hKey, NULL, 0, REG_SZ, (const BYTE *)desc, (DWORD)strlen(desc) + 1);
    RegSetValueExA(hKey, "URL Protocol", 0, REG_SZ, (const BYTE *)"", 1);
    RegCloseKey(hKey);
  }
  if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Classes\\winbolo\\shell\\open\\command", 0, NULL,
                       0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
    RegSetValueExA(hKey, NULL, 0, REG_SZ, (const BYTE *)command, (DWORD)strlen(command) + 1);
    RegCloseKey(hKey);
  }
#endif
  /* No-op on macOS/Linux/iOS — registration is handled by Info.plist / .desktop file */
}

void gameFrontSetAddressFromWebLink(char *address) {
  char *tok;
  char *ptr;

  gameFrontTargetUdp = 0;
  ptr = address + strlen("winbolo://");
  tok = strtok(ptr, ":");
  if (tok != NULL) {
    strcpy(gameFrontUdpAddress, tok);
    tok = strtok(NULL, ":");
    if (tok != NULL) {
      gameFrontTargetUdp = atoi(tok);
    }
  }
}

void gameFrontHandleUrlOpen(char *url) {
  if (strncmp(url, "winbolo://", 10) == 0 && strcmp(url, "winbolo:///") != 0) {
    /* Parse address/port from the url parameter (gameFrontSetAddressFromWebLink
       uses strtok which modifies the string in place). Then store the original
       URL in fileName so gameFrontStart() can pick it up on restart. */
    gameFrontSetAddressFromWebLink(url);
    strncpy(fileName, "winbolo://", FILENAME_MAX - 1);
    /* Reconstruct a clean URL from the parsed globals so fileName
       is not mangled by strtok. */
    if (gameFrontTargetUdp > 0) {
      snprintf(fileName, FILENAME_MAX, "winbolo://%.255s:%d", gameFrontUdpAddress, gameFrontTargetUdp);
    } else {
      snprintf(fileName, FILENAME_MAX, "winbolo://%.255s", gameFrontUdpAddress);
    }
    dlgState = openInternetManual;
  }
}

void gameFrontReloadSkins(void) {
  sdl3DrawCleanup();
  soundCleanup();
  sdl3DrawSetup(1);
  if (soundSetup() == FALSE) {
    /* Non-fatal */
  }
}

void gameFrontShutdownServer(void) {
  ServerSim *toFree;
  if (!spServerSimActive) return;
  if (hostedServerTimerID != 0) {
    SDL_RemoveTimer(hostedServerTimerID);
    hostedServerTimerID = 0;
  }
  /* Hold the mutex while we transfer ownership of spServerSim into a
   * local.  SDL_RemoveTimer above stops new fires; this block stops
   * in-flight ones from racing the destruction below.  A callback
   * waiting on the mutex will see spServerSim == NULL when it runs and
   * bail without dereferencing a freed pointer. */
  threadsWaitForMutex();
  toFree = spServerSim;
  spServerSim = NULL;
  spServerHosted = FALSE;
  spServerSimActive = FALSE;
  threadsReleaseMutex();

  serverInstanceShutdown(toFree);
  serverSimDestroy(toFree);
}

bool gameFrontPreferencesExist(void) {
  FILE *fp;
  const char *path = getPreferenceFilePath();

  fp = fopen(path, "r");
  if (fp != NULL) {
    fclose(fp);
    return TRUE;
  }
  return FALSE;
}

bool gameFrontSetupServer(void) {
  ServerInstanceConfig cfg;

  /* Welcome-screen BgGame leaves the global botManager state populated
   * with its eye-candy bots; without clearing it here, serverFindFreeSlot
   * skips those slots and the host's loopback JOIN_REQUEST gets a
   * non-zero player number. Mirrors the SP path. */
  {
    BgGame *sharedBg = bgGameGetShared();
    if (sharedBg != NULL) {
      bgGameDestroy(sharedBg);
      bgGameSetShared(NULL);
    }
  }

  if (strncmp(fileName, "randommap:", 10) == 0) {
    MapGenConfig mcfg = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
    const char *seedStr = fileName + 10;
    if (!mapGenSeedToConfig(seedStr, &mcfg)) {
      WB_LOG_WARN(WB_LOG_CAT_MAP, "failed to parse random map seed '%s', using defaults", seedStr);
    }
    mcfg.x1 = MAP_MINE_EDGE_LEFT + 1; mcfg.y1 = MAP_MINE_EDGE_TOP + 1;
    mcfg.x2 = MAP_MINE_EDGE_RIGHT - 1; mcfg.y2 = MAP_MINE_EDGE_BOTTOM - 1;
    spServerSim = serverSimCreateRandomMap(&mcfg, gametype, hiddenMines, startDelay, timeLen);
  } else if (fileName[0] != '\0') {
    spServerSim = serverSimCreate(fileName, gametype, hiddenMines, startDelay, timeLen);
  } else {
    BYTE emap[6000] = E_MAP;
    spServerSim = serverSimCreateCompressed(emap, 5097, gametype, hiddenMines, startDelay, timeLen);
  }
  if (spServerSim == NULL) {
    return FALSE;
  }

  serverSimSetLobbyEnabled(spServerSim, true);
  serverSimSetEmptyResetEnabled(spServerSim, true);
  serverSimEnterLobby(spServerSim);
  serverSimSetHasPassword(spServerSim, (password[0] != '\0'));

  memset(&cfg, 0, sizeof(cfg));
  cfg.udpPort         = gameFrontMyUdp;
  cfg.bindAddr        = "";
  cfg.password        = password;
  cfg.maxPlayers      = MAX_TANKS;
  cfg.useWbn          = gameFrontWbnUse;
  cfg.compTanks       = (BYTE)compTanks;
  cfg.useTracker      = gameFrontTrackerEnabled;
  cfg.trackerAddr     = gameFrontTrackerAddr;
  cfg.trackerPort     = gameFrontTrackerPort;
  cfg.useNatKeepalive = gameFrontUseNatTraversal;
  cfg.useNatPortmap   = gameFrontUseUpnp;

  if (!serverInstanceStartup(spServerSim, &cfg)) {
    serverSimDestroy(spServerSim);
    spServerSim = NULL;
    return FALSE;
  }

  hostedServerTimerID = SDL_AddTimer(SERVER_TICK_LENGTH, hostedServerTimerCb, NULL);
  if (hostedServerTimerID == 0) {
    serverInstanceShutdown(spServerSim);
    serverSimDestroy(spServerSim);
    spServerSim = NULL;
    return FALSE;
  }

  spServerSimActive = TRUE;
  spServerHosted = TRUE;
  isServer = TRUE;
  return TRUE;
}

bool gameFrontLoadInBuiltMap(void) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4305)
#endif
  BYTE emap[6000] = E_MAP;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
  return clientLoadCompressedMap(humanSim, emap, 5097, "Everard Island", gametype, hiddenMines, startDelay, timeLen, gameFrontName, 0, FALSE);
}

bool gameFrontLoadTutorial(void) {
  const char *candidates[3];
  char basePathBuf[FILENAME_MAX];
  const char *basePath = SDL_GetBasePath();
  int i;
  FILE *fp;

  candidates[0] = "data/maps/Inbuilt Tutorial.map";

  if (basePath != NULL) {
    snprintf(basePathBuf, FILENAME_MAX, "%sdata/maps/Inbuilt Tutorial.map", basePath);
    candidates[1] = basePathBuf;
  } else {
    candidates[1] = NULL;
  }
  candidates[2] = "Inbuilt Tutorial.map";

  for (i = 0; i < 3; i++) {
    if (candidates[i] == NULL) continue;
    fp = fopen(candidates[i], "rb");
    if (fp != NULL) {
      fclose(fp);
      /* clientLoadMap takes char* (not const) but doesn't mutate. */
      return clientLoadMap(humanSim, (char *)candidates[i], gameStrictTournament, FALSE, 0, UNLIMITED_GAME_TIME, gameFrontName, FALSE);
    }
  }
  return FALSE;
}

/* -------------------------------------------------------
 * gameFrontGetPrefs — read preferences from INI file
 * ------------------------------------------------------- */
bool gameFrontGetPrefs(keyItems *keys, bool *pUseAutoslow, bool *pUseAutohide) {
  char buff[FILENAME_MAX];
  char def[FILENAME_MAX];
  const char *prefsFile = getPreferenceFilePath();

  /* Player Name */
  strcpy(def, langGetText(STR_DLGGAMESETUP_DEFAULTNAME));
  GetPrivateProfileString("SETTINGS", "Player Name", def, gameFrontName, sizeof(gameFrontName), prefsFile);

  /* Target Address */
  def[0] = '\0';
  GetPrivateProfileString("SETTINGS", "Target Address", def, gameFrontUdpAddress, FILENAME_MAX, prefsFile);

  /* Target UDP Port */
  intToStr(DEFAULT_UDP_PORT, def, sizeof(def));
  GetPrivateProfileString("SETTINGS", "Target UDP Port", def, buff, FILENAME_MAX, prefsFile);
  gameFrontMyUdp = atoi(buff);
  gameFrontTargetUdp = atoi(buff);
  /* My UDP Port */
  intToStr(DEFAULT_UDP_PORT, def, sizeof(def));
  GetPrivateProfileString("SETTINGS", "UDP Port", def, buff, FILENAME_MAX, prefsFile);
  gameFrontMyUdp = atoi(buff);

  /* Driving keys */
  intToStr(DEFAULT_FORWARD, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Forward", def, buff, FILENAME_MAX, prefsFile);
  keys->kiForward = atoi(buff);
  intToStr(DEFAULT_BACKWARD, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Backwards", def, buff, FILENAME_MAX, prefsFile);
  keys->kiBackward = atoi(buff);
  intToStr(DEFAULT_LEFT, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Left", def, buff, FILENAME_MAX, prefsFile);
  keys->kiLeft = atoi(buff);
  intToStr(DEFAULT_RIGHT, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Right", def, buff, FILENAME_MAX, prefsFile);
  keys->kiRight = atoi(buff);

  /* Shooting, mines, gunsights */
  intToStr(DEFAULT_SHOOT, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Shoot", def, buff, FILENAME_MAX, prefsFile);
  keys->kiShoot = atoi(buff);
  intToStr(DEFAULT_LAY_MINE, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Lay Mine", def, buff, FILENAME_MAX, prefsFile);
  keys->kiLayMine = atoi(buff);
  intToStr(DEFAULT_SCROLL_GUNINCREASE, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Increase Range", def, buff, FILENAME_MAX, prefsFile);
  keys->kiGunIncrease = atoi(buff);
  intToStr(DEFAULT_SCROLL_GUNDECREASE, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Decrease Range", def, buff, FILENAME_MAX, prefsFile);
  keys->kiGunDecrease = atoi(buff);

  /* Views */
  intToStr(DEFAULT_TANKVIEW, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Tank View", def, buff, FILENAME_MAX, prefsFile);
  keys->kiTankView = atoi(buff);
  intToStr(DEFAULT_PILLVIEW, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Pill View", def, buff, FILENAME_MAX, prefsFile);
  keys->kiPillView = atoi(buff);
  intToStr(DEFAULT_ALLYVIEW, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Ally View", def, buff, FILENAME_MAX, prefsFile);
  keys->kiAllyView = atoi(buff);
  intToStr(DEFAULT_LGMVIEW, def, sizeof(def));
  GetPrivateProfileString("KEYS", "LGM View", def, buff, FILENAME_MAX, prefsFile);
  keys->kiLGMView = atoi(buff);
  intToStr(DEFAULT_BASEVIEW, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Base View", def, buff, FILENAME_MAX, prefsFile);
  keys->kiBaseView = atoi(buff);

  /* Scrolling */
  intToStr(DEFAULT_SCROLLUP, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Scroll Up", def, buff, FILENAME_MAX, prefsFile);
  keys->kiScrollUp = atoi(buff);
  intToStr(DEFAULT_SCROLLDOWN, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Scroll Down", def, buff, FILENAME_MAX, prefsFile);
  keys->kiScrollDown = atoi(buff);
  intToStr(DEFAULT_SCROLLLEFT, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Scroll Left", def, buff, FILENAME_MAX, prefsFile);
  keys->kiScrollLeft = atoi(buff);
  intToStr(DEFAULT_SCROLLRIGHT, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Scroll Right", def, buff, FILENAME_MAX, prefsFile);
  keys->kiScrollRight = atoi(buff);

  /* Quick keys */
  intToStr(DEFAULT_QUICKTREE, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Quick Tree", def, buff, FILENAME_MAX, prefsFile);
  keys->kiQuickTree = atoi(buff);
  intToStr(DEFAULT_QUICKROAD, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Quick Road", def, buff, FILENAME_MAX, prefsFile);
  keys->kiQuickRoad = atoi(buff);
  intToStr(DEFAULT_QUICKWALL, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Quick Wall", def, buff, FILENAME_MAX, prefsFile);
  keys->kiQuickWall = atoi(buff);
  intToStr(DEFAULT_QUICKPILLBOX, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Quick Pillbox", def, buff, FILENAME_MAX, prefsFile);
  keys->kiQuickPillbox = atoi(buff);
  intToStr(DEFAULT_QUICKMINE, def, sizeof(def));
  GetPrivateProfileString("KEYS", "Quick Mine", def, buff, FILENAME_MAX, prefsFile);
  keys->kiQuickMine = atoi(buff);

  /* Remember */
  GetPrivateProfileString("SETTINGS", "Remember Player Name", "Yes", buff, FILENAME_MAX, prefsFile);
  gameFrontRemeber = YESNO_TO_TRUEFALSE(buff[0]);

  /* Tutorial visibility — defaults to "Yes" (show on first run). */
  GetPrivateProfileString("SETTINGS", "Show Tutorial Button", "Yes", buff, FILENAME_MAX, prefsFile);
  gameFrontShowTutorialButton = YESNO_TO_TRUEFALSE(buff[0]);

  /* Country-flag rendering in chat / newswire — defaults to "Yes". */
  GetPrivateProfileString("SETTINGS", "Show Country Flags In Chat", "Yes", buff, FILENAME_MAX, prefsFile);
  gameFrontShowCountryFlagsInChat = YESNO_TO_TRUEFALSE(buff[0]);

  /* Language code (BCP-47, e.g. "en", "de", "pt-br"). Empty string on
   * fresh install — startup walks SDL_GetPreferredLocales() in that
   * case (see gameFrontStart). */
  GetPrivateProfileString("SETTINGS", "Language", "",
                          gameFrontLanguageCode,
                          (DWORD)sizeof(gameFrontLanguageCode), prefsFile);

  /* Game Options */
  GetPrivateProfileString("GAME OPTIONS", "Hidden Mines", "No", buff, FILENAME_MAX, prefsFile);
  hiddenMines = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("GAME OPTIONS", "Allow Computer Tanks", "0", buff, FILENAME_MAX, prefsFile);
  compTanks = atoi(buff);
  GetPrivateProfileString("GAME OPTIONS", "Game Type", "1", buff, FILENAME_MAX, prefsFile);
  gametype = atoi(buff);
  GetPrivateProfileString("GAME OPTIONS", "Start Delay", "0", buff, FILENAME_MAX, prefsFile);
  startDelay = atoi(buff);
  longToStr(UNLIMITED_GAME_TIME, def, sizeof(def));
  GetPrivateProfileString("GAME OPTIONS", "Time Length", def, buff, FILENAME_MAX, prefsFile);
  timeLen = (int32_t)atol(buff);
#if defined(__IPHONEOS__) || defined(__ANDROID__)
  GetPrivateProfileString("GAME OPTIONS", "Auto Slowdown", "Yes", buff, FILENAME_MAX, prefsFile);
#else
  GetPrivateProfileString("GAME OPTIONS", "Auto Slowdown", "No", buff, FILENAME_MAX, prefsFile);
#endif
  *pUseAutoslow = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("GAME OPTIONS", "Auto Show-Hide Gunsight", "No", buff, FILENAME_MAX, prefsFile);
  *pUseAutohide = YESNO_TO_TRUEFALSE(buff[0]);

  GetPrivateProfileString("SETTINGS", "Use UPnP", "Yes", buff, FILENAME_MAX, prefsFile);
  gameFrontUseUpnp = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("SETTINGS", "Use NAT Traversal", "Yes", buff, FILENAME_MAX, prefsFile);
  gameFrontUseNatTraversal = YESNO_TO_TRUEFALSE(buff[0]);

  /* Tracker options */
  GetPrivateProfileString("TRACKER", "Address", TRACKER_ADDRESS, gameFrontTrackerAddr, FILENAME_MAX, prefsFile);
  intToStr(TRACKER_PORT, def, sizeof(def));
  GetPrivateProfileString("TRACKER", "Port", def, buff, FILENAME_MAX, prefsFile);
  gameFrontTrackerPort = atoi(buff);
  GetPrivateProfileString("TRACKER", "Enabled", "No", buff, FILENAME_MAX, prefsFile);
  gameFrontTrackerEnabled = YESNO_TO_TRUEFALSE(buff[0]);

  /* Menu Items */
  intToStr(FRAME_RATE_30, def, sizeof(def));
  GetPrivateProfileString("MENU", "Frame Rate", def, buff, FILENAME_MAX, prefsFile);
  frameRate = atoi(buff);
#if defined(__IPHONEOS__) || defined(__ANDROID__)
  GetPrivateProfileString("MENU", "Show Gunsight", "Yes", buff, FILENAME_MAX, prefsFile);
#else
  GetPrivateProfileString("MENU", "Show Gunsight", "No", buff, FILENAME_MAX, prefsFile);
#endif
  showGunsight = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Sound Effects", "Yes", buff, FILENAME_MAX, prefsFile);
  soundEffects = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Allow Background Sound", "Yes", buff, FILENAME_MAX, prefsFile);
  backgroundSound = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Sound keepalive", "No", buff, FILENAME_MAX, prefsFile);
  useSoundKeepalive = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Show Newswire Messages", "Yes", buff, FILENAME_MAX, prefsFile);
  showNewswireMessages = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Show Assistant Messages", "Yes", buff, FILENAME_MAX, prefsFile);
  showAssistantMessages = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Show AI Messages", "Yes", buff, FILENAME_MAX, prefsFile);
  showAIMessages = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Show Network Status Messages", "Yes", buff, FILENAME_MAX, prefsFile);
  showNetworkStatusMessages = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Show Network Debug Messages", "No", buff, FILENAME_MAX, prefsFile);
  showNetworkDebugMessages = YESNO_TO_TRUEFALSE(buff[0]);
#if defined(__IPHONEOS__) || defined(__ANDROID__)
  GetPrivateProfileString("MENU", "Autoscroll Enabled", "Yes", buff, FILENAME_MAX, prefsFile);
#else
  GetPrivateProfileString("MENU", "Autoscroll Enabled", "No", buff, FILENAME_MAX, prefsFile);
#endif
  autoScrollingEnabled = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Smooth Scrolling", "Yes", buff, FILENAME_MAX, prefsFile);
  smoothScrollingEnabled = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Show Pill Labels", "No", buff, FILENAME_MAX, prefsFile);
  showPillLabels = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Show Base Labels", "No", buff, FILENAME_MAX, prefsFile);
  showBaseLabels = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Label Own Tank", "No", buff, FILENAME_MAX, prefsFile);
  labelSelf = YESNO_TO_TRUEFALSE(buff[0]);
  GetPrivateProfileString("MENU", "Window Size", "1", buff, FILENAME_MAX, prefsFile);
  zoomFactor = atoi(buff);
  /* Custom window size (for ZOOM_FACTOR_CUSTOM mode) */
  GetPrivateProfileString("MENU", "Custom Width", "0", buff, FILENAME_MAX, prefsFile);
  {
    int customW = atoi(buff);
    GetPrivateProfileString("MENU", "Custom Height", "0", buff, FILENAME_MAX, prefsFile);
    int customH = atoi(buff);
    if (customW > 0 && customH > 0) {
      windowSetCustomSize(customW, customH);
    }
  }
  /* Window position */
  GetPrivateProfileString("MENU", "Window X", "-1", buff, FILENAME_MAX, prefsFile);
  {
    int winX = atoi(buff);
    GetPrivateProfileString("MENU", "Window Y", "-1", buff, FILENAME_MAX, prefsFile);
    int winY = atoi(buff);
    windowSetSavedPosition(winX, winY);
  }
  /* Dialog window position (welcome screen, etc.) */
  GetPrivateProfileString("MENU", "Dialog X", "-1", buff, FILENAME_MAX, prefsFile);
  gameFrontDialogX = atoi(buff);
  GetPrivateProfileString("MENU", "Dialog Y", "-1", buff, FILENAME_MAX, prefsFile);
  gameFrontDialogY = atoi(buff);

  GetPrivateProfileString("MENU", "Message Label Size", "1", buff, FILENAME_MAX, prefsFile);
  labelMsg = atoi(buff);
  GetPrivateProfileString("MENU", "Tank Label Size", "1", buff, FILENAME_MAX, prefsFile);
  labelTank = atoi(buff);

  /* Winbolo.net */
  GetPrivateProfileString("WINBOLO.NET", "Token", "", gameFrontWbnToken, FILENAME_MAX, prefsFile);
  GetPrivateProfileString("WINBOLO.NET", "TokenExpiry", "", gameFrontWbnTokenExpiry, FILENAME_MAX, prefsFile);
  gameFrontWbnUse = (gameFrontWbnToken[0] != '\0');

  return TRUE;
}

/* Forward declaration — defined after gameFrontPutPrefs */
void gameFrontFlushWindowSettings(void);

/* -------------------------------------------------------
 * gameFrontPutPrefs — write preferences to INI file
 * ------------------------------------------------------- */
void gameFrontPutPrefs(keyItems *keys) {
  char playerName[PLAYER_NAME_LEN];
  char buff[FILENAME_MAX];
  const char *prefsFile = getPreferenceFilePath();

  /* Player Name */
  if (((humanSim != NULL && clientSimGetNetType(humanSim) == netSingle) || (gameFrontRemeber == TRUE && humanSim != NULL)) && dlgState != openSetup && !clientSimIsInLobby(humanSim)) {
    clientSimGetPlayerName(humanSim, playerName);
    strcpy(gameFrontName, playerName);
    WritePrivateProfileString("SETTINGS", "Player Name", playerName, prefsFile);
  } else {
    WritePrivateProfileString("SETTINGS", "Player Name", gameFrontName, prefsFile);
  }

  /* Target Address */
  WritePrivateProfileString("SETTINGS", "Target Address", gameFrontUdpAddress, prefsFile);

  /* Ports */
  intToStr(gameFrontTargetUdp, buff, sizeof(buff));
  WritePrivateProfileString("SETTINGS", "Target UDP Port", buff, prefsFile);
  intToStr(gameFrontMyUdp, buff, sizeof(buff));
  WritePrivateProfileString("SETTINGS", "UDP Port", buff, prefsFile);

  /* Language — persist the BCP-47 code, not a file path. */
  WritePrivateProfileString("SETTINGS", "Language",
                            gameFrontLanguageCode, prefsFile);

  /* Keys — driving */
  intToStr(keys->kiForward, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Forward", buff, prefsFile);
  intToStr(keys->kiBackward, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Backwards", buff, prefsFile);
  intToStr(keys->kiLeft, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Left", buff, prefsFile);
  intToStr(keys->kiRight, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Right", buff, prefsFile);

  /* Shooting, mines, gunsight */
  intToStr(keys->kiShoot, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Shoot", buff, prefsFile);
  intToStr(keys->kiLayMine, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Lay Mine", buff, prefsFile);
  intToStr(keys->kiGunIncrease, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Increase Range", buff, prefsFile);
  intToStr(keys->kiGunDecrease, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Decrease Range", buff, prefsFile);

  /* Views */
  intToStr(keys->kiTankView, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Tank View", buff, prefsFile);
  intToStr(keys->kiPillView, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Pill View", buff, prefsFile);
  intToStr(keys->kiAllyView, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Ally View", buff, prefsFile);
  intToStr(keys->kiLGMView, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "LGM View", buff, prefsFile);
  intToStr(keys->kiBaseView, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Base View", buff, prefsFile);

  /* Scrolling */
  intToStr(keys->kiScrollUp, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Scroll Up", buff, prefsFile);
  intToStr(keys->kiScrollDown, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Scroll Down", buff, prefsFile);
  intToStr(keys->kiScrollLeft, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Scroll Left", buff, prefsFile);
  intToStr(keys->kiScrollRight, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Scroll Right", buff, prefsFile);

  /* Quick keys */
  intToStr(keys->kiQuickTree, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Quick Tree", buff, prefsFile);
  intToStr(keys->kiQuickRoad, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Quick Road", buff, prefsFile);
  intToStr(keys->kiQuickWall, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Quick Wall", buff, prefsFile);
  intToStr(keys->kiQuickPillbox, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Quick Pillbox", buff, prefsFile);
  intToStr(keys->kiQuickMine, buff, sizeof(buff));
  WritePrivateProfileString("KEYS", "Quick Mine", buff, prefsFile);

  /* Remember */
  WritePrivateProfileString("SETTINGS", "Remember Player Name", TRUEFALSE_TO_STR(gameFrontRemeber), prefsFile);

  /* Options */
  WritePrivateProfileString("GAME OPTIONS", "Hidden Mines", TRUEFALSE_TO_STR(hiddenMines), prefsFile);
  intToStr(compTanks, buff, sizeof(buff));
  WritePrivateProfileString("GAME OPTIONS", "Allow Computer Tanks", buff, prefsFile);
  intToStr(gametype, buff, sizeof(buff));
  WritePrivateProfileString("GAME OPTIONS", "Game Type", buff, prefsFile);
  intToStr(startDelay, buff, sizeof(buff));
  WritePrivateProfileString("GAME OPTIONS", "Start Delay", buff, prefsFile);
  intToStr(timeLen, buff, sizeof(buff));
  WritePrivateProfileString("GAME OPTIONS", "Time Length", buff, prefsFile);
  WritePrivateProfileString("GAME OPTIONS", "Auto Slowdown", TRUEFALSE_TO_STR(useAutoslow), prefsFile);
  WritePrivateProfileString("GAME OPTIONS", "Auto Show-Hide Gunsight", TRUEFALSE_TO_STR(useAutohide), prefsFile);

  WritePrivateProfileString("SETTINGS", "Use UPnP", TRUEFALSE_TO_STR(gameFrontUseUpnp), prefsFile);
  WritePrivateProfileString("SETTINGS", "Use NAT Traversal", TRUEFALSE_TO_STR(gameFrontUseNatTraversal), prefsFile);

  /* Tracker */
  WritePrivateProfileString("TRACKER", "Address", gameFrontTrackerAddr, prefsFile);
  intToStr(gameFrontTrackerPort, buff, sizeof(buff));
  WritePrivateProfileString("TRACKER", "Port", buff, prefsFile);
  WritePrivateProfileString("TRACKER", "Enabled", TRUEFALSE_TO_STR(gameFrontTrackerEnabled), prefsFile);

  /* Menu Items */
  intToStr(frameRate, buff, sizeof(buff));
  WritePrivateProfileString("MENU", "Frame Rate", buff, prefsFile);
  WritePrivateProfileString("MENU", "Show Gunsight", TRUEFALSE_TO_STR(showGunsight), prefsFile);
  WritePrivateProfileString("MENU", "Sound Effects", TRUEFALSE_TO_STR(soundEffects), prefsFile);
  WritePrivateProfileString("MENU", "Allow Background Sound", TRUEFALSE_TO_STR(backgroundSound), prefsFile);
  WritePrivateProfileString("MENU", "Sound keepalive", TRUEFALSE_TO_STR(useSoundKeepalive), prefsFile);
  WritePrivateProfileString("MENU", "Show Newswire Messages", TRUEFALSE_TO_STR(showNewswireMessages), prefsFile);
  WritePrivateProfileString("MENU", "Show Assistant Messages", TRUEFALSE_TO_STR(showAssistantMessages), prefsFile);
  WritePrivateProfileString("MENU", "Show AI Messages", TRUEFALSE_TO_STR(showAIMessages), prefsFile);
  WritePrivateProfileString("MENU", "Show Network Status Messages", TRUEFALSE_TO_STR(showNetworkStatusMessages), prefsFile);
  WritePrivateProfileString("MENU", "Show Network Debug Messages", TRUEFALSE_TO_STR(showNetworkDebugMessages), prefsFile);
  WritePrivateProfileString("MENU", "Autoscroll Enabled", TRUEFALSE_TO_STR(autoScrollingEnabled), prefsFile);
  WritePrivateProfileString("MENU", "Smooth Scrolling", TRUEFALSE_TO_STR(smoothScrollingEnabled), prefsFile);
  WritePrivateProfileString("MENU", "Show Pill Labels", TRUEFALSE_TO_STR(showPillLabels), prefsFile);
  WritePrivateProfileString("MENU", "Show Base Labels", TRUEFALSE_TO_STR(showBaseLabels), prefsFile);
  WritePrivateProfileString("MENU", "Label Own Tank", TRUEFALSE_TO_STR(labelSelf), prefsFile);
  /* Window settings (zoom, custom size, position, dialog position) — flush immediately,
     bypassing debounce since this is the shutdown save path. */
  gameFrontFlushWindowSettings();
  intToStr(labelMsg, buff, sizeof(buff));
  WritePrivateProfileString("MENU", "Message Label Size", buff, prefsFile);
  intToStr(labelTank, buff, sizeof(buff));
  WritePrivateProfileString("MENU", "Tank Label Size", buff, prefsFile);

  /* Winbolo.net */
  WritePrivateProfileString("WINBOLO.NET", "Token", gameFrontWbnToken, prefsFile);
  WritePrivateProfileString("WINBOLO.NET", "TokenExpiry", gameFrontWbnTokenExpiry, prefsFile);
}

/* -------------------------------------------------------
 * gameFrontSaveWindowSettings — save window position/size
 *
 * Persists zoom, custom size, window position, and dialog
 * position to the INI file. Called on resize/move and also
 * from gameFrontPutPrefs at shutdown. Debounced: writes at
 * most once per 500ms during rapid resize/move events.
 * ------------------------------------------------------- */
static bool s_windowSettingsDirty = false;

void gameFrontFlushWindowSettings(void) {
  char buff[FILENAME_MAX];
  const char *prefsFile = getPreferenceFilePath();

  intToStr(zoomFactor, buff, sizeof(buff));
  WritePrivateProfileString("MENU", "Window Size", buff, prefsFile);

  {
    int customW, customH;
    windowGetCustomSize(&customW, &customH);
    intToStr(customW, buff, sizeof(buff));
    WritePrivateProfileString("MENU", "Custom Width", buff, prefsFile);
    intToStr(customH, buff, sizeof(buff));
    WritePrivateProfileString("MENU", "Custom Height", buff, prefsFile);
  }

  {
    int winX, winY;
    windowGetSavedPosition(&winX, &winY);
    intToStr(winX, buff, sizeof(buff));
    WritePrivateProfileString("MENU", "Window X", buff, prefsFile);
    intToStr(winY, buff, sizeof(buff));
    WritePrivateProfileString("MENU", "Window Y", buff, prefsFile);
  }

  intToStr(gameFrontDialogX, buff, sizeof(buff));
  WritePrivateProfileString("MENU", "Dialog X", buff, prefsFile);
  intToStr(gameFrontDialogY, buff, sizeof(buff));
  WritePrivateProfileString("MENU", "Dialog Y", buff, prefsFile);

  s_windowSettingsDirty = false;
}

void gameFrontSaveWindowSettings(void) {
  static Uint64 lastWriteTime = 0;
  Uint64 now = SDL_GetTicks();
  if (now - lastWriteTime < 500) {
    s_windowSettingsDirty = true;
    return;
  }
  gameFrontFlushWindowSettings();
  lastWriteTime = now;
}

ServerSim *gameFrontGetServerSim(void) {
  return spServerSimActive ? spServerSim : NULL;
}

BYTE gameFrontGetPlayerNum(void) {
  if (spServerSimActive && !spServerHosted) return 0;
  if (udpTransportActive) return udpPlayerNum;
  return 0;
}

bool gameFrontLoadDeferredMap(ClientSim **cs) {
  const BYTE *mapData;
  int mapLen = 0;
  gameType serverGame;
  bool serverHiddenMines;
  int32_t serverStartDelay, serverGameLen;

  mapData = clientSimGetServerMapData(*cs, &mapLen);
  clientSimGetServerGameSettings(*cs, &serverGame,
                                 &serverHiddenMines,
                                 &serverStartDelay,
                                 &serverGameLen);

  if (mapData == NULL || mapLen <= 0) {
    return FALSE;
  }

  gametype = serverGame;
  hiddenMines = serverHiddenMines;
  startDelay = serverStartDelay;
  timeLen = serverGameLen;

  /* Preserve lobby flag and map name across reset — clientSimCreate
   * (re-entered via clientLoadCompressedMap below) clears them. */
  bool wasInLobby = clientSimIsInLobby(*cs);
  char savedMapName[MAP_STR_SIZE];
  strncpy(savedMapName, clientSimGetMapName(*cs), MAP_STR_SIZE - 1);
  savedMapName[MAP_STR_SIZE - 1] = '\0';

  /* Reset map-dependent state in place; clientSimResetForMapLoad keeps
   * the transport binding so the mapData pointer (which lives inside
   * the UDP transport's buffer) stays valid for clientLoadCompressedMap. */
  clientSimResetForMapLoad(*cs);
  sdl3DrawResetCachedText();

  if (clientLoadCompressedMap(*cs, (BYTE *)mapData, mapLen,
                             savedMapName, serverGame,
                             serverHiddenMines, serverStartDelay,
                             serverGameLen, gameFrontName,
                             (BYTE)udpPlayerNum, FALSE) == FALSE) {
    return FALSE;
  }

  clientSimSetLocalTransport(*cs, false);
  clientSimSetInLobby(*cs, wasInLobby);
  clientSimSetMapDownloadComplete(*cs, true);

  clientSimSetChatSendFunc(*cs, gameFrontChatSendCallback);
  clientSimSetNameChangeSendFunc(*cs, gameFrontNameChangeSendCallback);
  clientSimSetAllianceRequestFunc(*cs, gameFrontAllianceRequestCallback);
  clientSimSetAllianceAcceptFunc(*cs, gameFrontAllianceAcceptCallback);
  clientSimSetAllianceLeaveFunc(*cs, gameFrontAllianceLeaveCallback);
  clientSimSetLockToggleSendFunc(*cs, gameFrontLockToggleCallback);

  clientMutexWaitFor();
  clientNetSetupTankGo(*cs);
  clientMutexRelease();

  return TRUE;
}

