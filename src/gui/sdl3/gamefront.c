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
#include "bolo_rand.h"
#include "client_sim.h"
#include "control_event.h"
#include "discovery.h"
#include "global.h"
#include "util.h"
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
#include "dialogs/imgui_mapchooser.h"
#include "dialogs/imgui_messagebox.h"
#include "bg_game.h"

#include "everard_map.h"
#include "platform_net.h"
#include "playername_validate.h"
#include "client_net.h"
#include "../../server/server_lifecycle.h"
#include "../../winbolonet/winbolonet_client.h"
#include "../../winbolonet/winbolonet_core.h"
#include "../../steam/steam_wrapper.h"
#include "../../mapeditor/mapeditor.h"
#include "mapgen.h"
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
static bool gameFrontStartServerSim(ServerSim *sim,
                                    const ServerInstanceConfig *cfg);
extern void sdl3MessageHandler(const char *message, const char *title);

/* Find the brain script — try several paths */
static bool findBrainPath(char *out, size_t outLen) {
    const char *candidates[] = {
        "Brains/GoalHunter/init.lua",
        "brains/GoalHunter/init.lua",
        "data/Brains/GoalHunter/init.lua",
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

/* Pending state-machine transition posted from outside the welcome
 * loop (host-OS menus). Read by the welcome dialog at the top of each
 * poll iteration. Single-threaded: setters and consumers all run on
 * the main thread (AppKit menu actions land on the same thread the
 * welcome loop pumps SDL events on), so no atomics are needed. */
static bool          gameFrontPendingTransitionSet = FALSE;
static openingStates gameFrontPendingTransition    = openWelcome;

/* TRUE while gameFrontDialogs() is sitting inside welcomeShow(). The
 * macOS Dock menu reads this to dim items outside the welcome screen. */
static bool gameFrontAtWelcome = FALSE;

/* Winbolo.net settings */
char gameFrontWbnToken[FILENAME_MAX];
char gameFrontWbnTokenExpiry[FILENAME_MAX];
bool gameFrontWbnUse;

/* Dialog window position (separate from game window) */
int gameFrontDialogX = -1;
int gameFrontDialogY = -1;

/* Dialog states */
openingStates dlgState = openStart;

/* Remembered "entry path" used to reach the lobby — set whenever the
 * state machine transitions to openFinished from one of the Internet
 * / LAN browser or manual-setup screens. After the user leaves the
 * lobby, gameFrontDialogs() consumes this once so the welcome screen
 * is skipped and we drop back to the same browser the user came from.
 * Defaults to openWelcome so first-launch behaviour is unchanged. */
static openingStates s_lobbyReturnState = openWelcome;

bool isServer = FALSE;

bool useAutoslow;
bool useAutohide;

bool wantRejoin;

/* Human player's ClientSim — owned by the frontend, allocated lazily
 * via clientSimAlloc when entering a session and freed via
 * clientSimDestroy when leaving. */
ClientSim *humanSim = NULL;

/* LAN-only session flag. Set when the user enters via openLan*
 * (host, browser, or manual join). Cleared on openInternet*. Mirrored
 * onto humanSim->isLanOnly via clientSimSetIsLanOnly at humanSim
 * creation so the lobby's "show LAN IP instead of 127.0.0.1" and
 * "skip reachability check" branches fire. Also short-circuits WBN /
 * tracker / NAT keepalive / UPnP at host bind time so an SP / LAN
 * host doesn't dial out. */
static bool s_isLanOnly = FALSE;

/* Set TRUE by the openUdpJoin error path when a join attempt fails
 * (server NACK'd the JOIN_REQUEST, name taken, password wrong, server
 * full, etc.). The outer dialog loop's openLan / openInternet /
 * openUdp / openLanManual / openInternetManual cases consult this
 * flag before applying the "fall back to Welcome if the dialog
 * exited without picking a state" guard — when set, the user
 * deliberately wants to stay on the list/manual-entry dialog so
 * they can pick a different game / tweak their name. Cleared the
 * moment the outer loop notices it. */
static bool s_joinAttemptFailed = FALSE;

/* Server-authoritative single-player state */
static ServerSim *spServerSim = NULL;
static SubscriberHandle spHumanSubHandle = SUBSCRIBER_HANDLE_INVALID;

static bool spServerSimActive = FALSE;
static SDL_TimerID hostedServerTimerID = 0;

/* Set true by gameFrontShutdownServer the moment shutdown begins so
 * any in-flight or already-scheduled callback bails out and returns 0
 * (cancels the timer) instead of returning `interval` which would
 * re-arm it. Without this, SDL_RemoveTimer + a non-zero return value
 * can cooperate to fire one extra callback after the mutex it grabs
 * has been destroyed. */
static volatile bool spServerTimerShutdown = false;

static Uint32 SDLCALL hostedServerTimerCb(void *userdata, SDL_TimerID id, Uint32 interval) {
  (void)userdata; (void)id;
  if (spServerTimerShutdown) return 0;
  /* Read spServerSim under the mutex so a concurrent shutdown can NULL
   * it out without us racing with a freed pointer cached on this stack
   * frame.  serverInstanceTick re-takes the mutex internally; the
   * threading mutex is recursive on both Windows and SDL3.  Single
   * non-NULL check covers both SP and listen-server now that both go
   * through this timer. */
  threadsWaitForMutex();
  bool active = (!spServerTimerShutdown && spServerSim != NULL);
  if (active) {
    serverInstanceTick(spServerSim);
  }
  threadsReleaseMutex();
  return active ? interval : 0;
}

/* UDP multiplayer transport state — the Transport handle itself now
 * lives inside humanSim; these flags only track whether a UDP join
 * is active for higher-level lifecycle gating. */
static bool udpTransportActive = FALSE;
static BYTE udpPlayerNum = 0;

/* Send callbacks for ClientSim — route through the client_net.h wrappers.
 * (The callback layer is retained for this transition; future cleanup
 * will let ClientSim callers call clientSimNetSend* directly.)
 *
 * Chat callback uses the passed cs (not the module-static humanSim) so
 * the same function can be wired onto a bot's ClientSim too — bot chat
 * then flows down the same path human chat does: clientSimNetSendChat
 * → clientSimSubmitCommand → CMD_CHAT arm publishes CTRL_CHAT. */
static void gameFrontChatSendCallback(struct ClientSim *cs, uint8_t fromPlayer,
                                      uint8_t destPlayer, const char *message) {
    (void)fromPlayer;
    clientSimNetSendChat(cs, destPlayer, message);
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
 *
 * Display tokens (defined in data/steam/rich_presence_localization.vdf):
 *   #StatusInMenu          — main menu / welcome screen
 *   #StatusInLobbySingular — in a lobby with 1 player (uses %map%)
 *   #StatusInLobbyPlural   — in a lobby with N players (uses %map%, %numplayers%)
 *   #StatusInGameSolo      — playing alone (uses %map%)
 *   #StatusInGamePlural    — playing with N players (uses %map%, %numplayers%)
 * ------------------------------------------------------- */
static void gameFrontSetConnectIfAvailable(void) {
  if (udpTransportActive && gameFrontUdpAddress[0] != '\0') {
    char connect[FILENAME_MAX];
    snprintf(connect, sizeof(connect), "+connect %.255s:%u",
             gameFrontUdpAddress, (unsigned)gameFrontTargetUdp);
    steam_set_rich_presence("connect", connect);
  }
}

void gameFrontUpdateSteamPresence(ClientSim *cs) {
  if (cs == NULL) return;
  BYTE numPlayers = clientSimGetNumPlayers(cs);
  char numStr[16];
  snprintf(numStr, sizeof(numStr), "%d", (int)numPlayers);
  steam_set_rich_presence("map", clientSimGetMapName(cs));
  steam_set_rich_presence("numplayers", numStr);
  steam_set_rich_presence("steam_display",
                          numPlayers == 1 ? "#StatusInGameSolo"
                                          : "#StatusInGamePlural");
  gameFrontSetConnectIfAvailable();
}

void gameFrontSetSteamPresenceMenu(void) {
  steam_clear_rich_presence();
  steam_set_rich_presence("steam_display", "#StatusInMenu");
}

void gameFrontSetSteamPresenceLobby(ClientSim *cs) {
  if (cs == NULL) return;
  /* clientSimGetNumPlayers counts in-use tank slots, which omits humans
   * who haven't been assigned a tank yet and lingers on removed bots.
   * For the lobby, use the connected-lobby-slot count (humans + bots) —
   * same data the lobby UI's player table renders. */
  BYTE numPlayers = clientSimGetLobbyNumConnected(cs);
  char numStr[16];
  snprintf(numStr, sizeof(numStr), "%d", (int)numPlayers);

  /* Only set "map" when we actually have a name. Steam treats an empty
   * value as a delete, which would leave "%map%" literal in the rendered
   * display string while the lobby waits for the server's lobbySettings
   * packet to populate cs->mapName. */
  const char *mapName = clientSimGetMapName(cs);
  if (mapName != NULL && mapName[0] != '\0') {
    steam_set_rich_presence("map", mapName);
  }
  steam_set_rich_presence("numplayers", numStr);
  steam_set_rich_presence("steam_display",
                          numPlayers == 1 ? "#StatusInLobbySingular"
                                          : "#StatusInLobbyPlural");
  gameFrontSetConnectIfAvailable();
}

extern bool isTutorial;

/* Used to set the preferences — defined in winbolo.c */
extern int frameRate;
extern bool showGunsight;
extern bool soundEffects;
extern bool backgroundSound;
extern bool useSoundKeepalive;
extern int  soundVolume;
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

/* Forward declaration — pickRandomMap is defined below near gameFrontDialogs,
 * but gameFrontStart calls it during the one-shot init block. */
static bool pickRandomMap(char *out, size_t outLen);

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

#if defined(__APPLE__) && !defined(BOLO_MOBILE)
    /* Install only the Dock-icon menu now so it is live during the
     * welcome screen. The full menu bar comes later via
     * mac_menubar_install() from sdl3ImguiSetup when a game starts —
     * pre-game dialogs intentionally have no menu bar of their own. */
    if (OKStart) {
      extern void mac_menubar_install_dock_menu(void);
      mac_menubar_install_dock_menu();
    }
#endif

    if (soundSetup() == FALSE) {
      /* Sound failure is non-fatal — disable sound */
      soundEffects = FALSE;
    }

    if (brainsHandlerLoadBrains() == FALSE) {
      /* Brain loading failure is non-fatal */
    }

    /* Heap-allocate the shared welcome-screen background game so it
     * survives every SP/host transition. Hidden by gameFrontStart* while
     * a foreground game runs; freed at app shutdown by main(). */
    {
      BgGame *bg = (BgGame *)SDL_calloc(1, sizeof(BgGame));
      if (bg != NULL) {
        char mapPath[512];
        if (pickRandomMap(mapPath, sizeof(mapPath)) &&
            bgGameCreate(bg, mapPath, sdl3DrawGetRenderer())) {
          bgGameSetShared(bg);
        } else {
          SDL_free(bg);
        }
      }
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
  /* No-op since the keys dialog persists useAutoslow / useAutohide
   * directly to INI on OK. */
  (void)cs;
}

/* -------------------------------------------------------
 * gameFrontEnd — shutdown game subsystems
 * ------------------------------------------------------- */
void gameFrontEnd(keyItems *keys, bool gamePlayed, bool isQuiting) {
  steam_clear_rich_presence();
  clientMutexWaitFor();
  /* No tank-readback here. The keys dialog persists useAutoslow /
   * useAutohide directly to INI on OK (immediate-flush) and nothing
   * during gameplay mutates tank->autoSlowdown after the initial
   * clientSimSetTankAutoSlowdown — so reading it back would just
   * round-trip the same value most of the time. The exception was
   * the buggy case where clientSimSetTankAutoSlowdown ran before
   * clientSimSetupSelf existed: it no-op'd on the NULL tank pointer,
   * then tankCreate later defaulted autoSlowdown to FALSE, and this
   * readback clobbered the user's INI choice with that default. */
  (void)gamePlayed;
  brainsHandlerShutdown();
  /* Stop the map-preview worker. Idempotent: a no-op if the chooser
   * was never opened (worker is spawned lazily on first preview
   * request) or if the dialog already closed (its close edge stops
   * the worker). Belt-and-suspenders for the quit-with-chooser-open
   * case: without this, the still-joinable std::thread destructor
   * runs at atexit and trips std::terminate. */
  mapChooserStopPreviewWorker();
  if (spServerSimActive) {
    /* Unregister the SP humanSim subscriber before gameFrontShutdownServer
     * destroys the ServerSim's subscriber registry.  The hostedServer-only
     * path skips this (no subscriber was registered) by leaving the handle
     * at SUBSCRIBER_HANDLE_INVALID and short-circuiting here. */
    if (spHumanSubHandle != SUBSCRIBER_HANDLE_INVALID) {
      serverSimUnregisterSubscriber(spServerSim, spHumanSubHandle);
      spHumanSubHandle = SUBSCRIBER_HANDLE_INVALID;
      clientSimDisconnect(humanSim);
    }
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
  /* Single shutdown path for both SP and listen-server: the timer is
   * removed, ownership of spServerSim is transferred under threadsMutex
   * so any in-flight hostedServerTimerCb bails, then serverInstanceShutdown
   * tears down whatever acceptRemoteClients set up (a no-op in SP).
   * gameFrontShutdownServer no-ops when spServerSimActive is FALSE. */
  gameFrontShutdownServer();
  isServer = FALSE;
  clientMutexRelease();
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
        int idx = (int)bolo_rand_below((uint32_t)count);
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
    int idx = (int)bolo_rand_below((uint32_t)filtered);
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

  /* Retrieve the process-lifetime shared bg (created in gameFrontStart's
   * one-shot init); mark it visible so bgGameTick runs while we're on
   * the welcome / settings dialogs. */
  BgGame *bg = bgGameGetShared();
  bool hasBg = (bg != NULL);
  if (hasBg) bgGameSetHiddenByForeground(bg, false);
  WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[BgGame] hasBg=%d", hasBg);

  /* Re-entry path after the user leaves a lobby: if the previous
   * gameFrontSetDlgState recorded an entry browser/manual screen,
   * jump straight back to it instead of falling through to welcome.
   * Consumed once. */
  if (s_lobbyReturnState != openWelcome) {
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[gameFront] resuming from lobby exit, dlgState %d -> %d",
                (int)dlgState, (int)s_lobbyReturnState);
    dlgState = s_lobbyReturnState;
    s_lobbyReturnState = openWelcome;
  }

  while (done == FALSE) {
    switch (dlgState) {
    case openStart:
      dlgState = openWelcome;
      break;
    case openWelcome: {
      const DialogBackend *db = dialogBackendGet();
      gameFrontAtWelcome = TRUE;
      int result = db->welcomeShow();
      gameFrontAtWelcome = FALSE;
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
      s_isLanOnly = (dlgState == openLanManual);
      db->udpSetupShow();
      /* dlgState already updated by gameFrontSetDlgState inside the dialog
       * (OnJoin/OnNew/OnCancel all call gameFrontSetDlgState before EndModal).
       * If the dialog stub didn't change state, fall back to welcome
       * — UNLESS a join attempt failed and the error path explicitly
       * routed us back here. In that case the user has intentionally
       * stayed on the setup screen to tweak their name / address and
       * the welcome-fallback would just throw away their context. */
      if (s_joinAttemptFailed) {
        s_joinAttemptFailed = FALSE;
      } else if (dlgState == prev) {
        dlgState = openWelcome;
      }
      break;
    }
    case openSetup:
    case openInternetSetup:
    case openLanSetup:
    case openUdpSetup: {
      /* The gamesetup dialog was retired — settings the user used to
       * configure here now live in the lobby and are edited inline
       * before clicking Start.  All paths transition straight to
       * openFinished, which creates the ServerSim with default
       * settings and runs the appropriate finisher (lobby host, SP
       * lobby, or tutorial). */
      s_isLanOnly = (dlgState == openLanSetup);
      gameFrontSetDlgState(openFinished);
      break;
    }
    case openInternet: {
      const DialogBackend *db = dialogBackendGet();
      openingStates prev = dlgState;
      s_isLanOnly = FALSE;
      db->gameBrowserShow(langGetText(STR_GAMEFRONT_TRACKERFINDER_TITLE), TRUE);
      /* See note in openLanManual case — preserve the browser when a
       * join attempt was rejected so the user can pick a different
       * game / change their name. */
      if (s_joinAttemptFailed) {
        s_joinAttemptFailed = FALSE;
      } else if (dlgState == prev) {
        dlgState = openWelcome;
      }
      break;
    }
    case openLan: {
      const DialogBackend *db = dialogBackendGet();
      openingStates prev = dlgState;
      s_isLanOnly = TRUE;
      db->gameBrowserShow(langGetText(STR_GAMEFRONT_LANFINDER_TITLE), FALSE);
      if (s_joinAttemptFailed) {
        s_joinAttemptFailed = FALSE;
      } else if (dlgState == prev) {
        dlgState = openWelcome;
      }
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
      /* The LogViewer NSMenu install/uninstall lives inside logViewerRun
       * itself — its save/restore stack swaps WinBolo's menu out on
       * entry and restores it on exit, so no wrapping is needed here. */
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

  /* Capture the entry path when committing to a game/lobby so the
   * post-lobby re-entry can skip the welcome screen. Only the browser
   * / manual-connect screens count — single-player / tutorial / map
   * editor / settings flows should return to welcome as before. */
  if (newState == openUdpJoin || newState == openFinished) {
    switch (dlgState) {
      case openInternet:
      case openInternetManual:
      case openLan:
      case openLanManual:
        s_lobbyReturnState = dlgState;
        break;
      default:
        s_lobbyReturnState = openWelcome;
        break;
    }
  }

  if ((dlgState == openInternet || dlgState == openLan || dlgState == openUdp ||
       dlgState == openLanManual || dlgState == openInternetManual) &&
      newState == openUdpJoin) {
    /* Pre-flight version negotiation. The legacy info-request is the
     * universal cross-version handshake — any server answers regardless
     * of build, so we can read the server's version triple before
     * committing to a JOIN_REQUEST whose new-protocol length gate would
     * silently drop on mismatch. On version mismatch surface a
     * localized "Server is version X, you have Y" error; on timeout
     * fall through to the standard "server unreachable" error. */
    {
      DiscoveryPingResult dpr;
      if (!discoveryPingServer(gameFrontUdpAddress, gameFrontTargetUdp, &dpr)) {
        imguiMessageBoxEx(DIALOG_BOX_TITLE,
                          langGetText(NETERR_SERVERCONNECT),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
        gameFrontShutdownServer();
        dlgState = prevState;
        s_joinAttemptFailed = TRUE;
        return FALSE;
      }
      if (dpr.versionMajor    != BOLO_VERSION_MAJOR ||
          dpr.versionMinor    != BOLO_VERSION_MINOR ||
          dpr.versionRevision != BOLO_VERSION_REVISION) {
        MessageArgs args;
        memset(&args, 0, sizeof(args));
        SDL_snprintf(args.string1, sizeof(args.string1), "%u.%u.%u",
                     (unsigned)dpr.versionMajor,
                     (unsigned)dpr.versionMinor,
                     (unsigned)dpr.versionRevision);
        SDL_snprintf(args.string2, sizeof(args.string2), "%u.%u.%u",
                     (unsigned)BOLO_VERSION_MAJOR,
                     (unsigned)BOLO_VERSION_MINOR,
                     (unsigned)BOLO_VERSION_REVISION);
        imguiMessageBoxEx(DIALOG_BOX_TITLE,
                          langGetTextFmt(STR_REJECT_VERSION_MISMATCH, &args),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
        gameFrontShutdownServer();
        dlgState = prevState;
        s_joinAttemptFailed = TRUE;
        return FALSE;
      }
    }

    gameFrontValidateWbnBeforeJoin();
    humanSim = clientSimAlloc(); clientSimCreate(humanSim);
    clientSimSetIsLanOnly(humanSim, s_isLanOnly);
    frontEndSetActiveClientSim(humanSim);
    if (gameFrontRemeber) clientSimSetMyLastPlayerName(humanSim, gameFrontName);
    fprintf(stderr, "[gameFront] openUdpJoin: addr=%s port=%u myPort=%u\n",
            gameFrontUdpAddress, (unsigned)gameFrontTargetUdp, (unsigned)gameFrontMyUdp);
    fflush(stderr);

    /* Create UDP client transport. The transport drives the JOIN
     * handshake, map download + install, and inline snapshot apply
     * by itself — the frontend only ticks it until the join state
     * settles or inLobby flips true. */
    clientSimConnectUdp(humanSim, gameFrontUdpAddress,
                        gameFrontTargetUdp,
                        gameFrontName,
                        winbolonetGetCountryCode(),
                        password,
                        gameFrontWbnUse ? gameFrontWbnToken : "",
                        "",
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
      s_joinAttemptFailed = TRUE;
      returnValue = FALSE;
    } else {
      /* Wait for join handshake. Break early if we enter the lobby
       * (lobby-enabled servers deliver CTRL_LOBBY_SETTINGS via sync
       * replay before map chunks, so inLobby may become true while
       * the map is still downloading). */
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
          /* Lobby path: enter lobby immediately, map downloads in
           * background. The lobby UI shows a progress bar and gates
           * the ready button on mapDownloadComplete. Install happens
           * inside the transport on the CTRL_GAME_PHASE LOBBY→RUNNING
           * watcher when the host starts the game. */
          clientSimSetNetStatus(humanSim, netLobby);
        } else {
          /* No-lobby path: transport already installed the map inline
           * on MAP_DOWNLOAD completion; the first snapshot apply will
           * fire the viewport finalisation. */
          gameFrontUpdateSteamPresence(humanSim);
        }
        dlgState = openFinished;
      } else {
        const char *reason = clientSimGetConnectErrorReason(humanSim);
        imguiMessageBoxEx(DIALOG_BOX_TITLE,
                          (reason && reason[0]) ? reason : langGetText(NETERR_SERVERCONNECT),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
        clientSimDestroy(humanSim);
        humanSim = NULL;
        gameFrontShutdownServer();
        dlgState = prevState;
        s_joinAttemptFailed = TRUE;
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
          spServerSim = serverSimCreateCompressed(emap, 5097, "Everard Island", gametype, hiddenMines, startDelay, timeLen);
        }
        if (spServerSim != NULL) {
          /* Embedded server: silence its console messages (Thread Manager
           * Startup, Game started!, …) — the client has no server console. */
          serverSimSetQuiet(spServerSim, true);
          bgGameSetHiddenByForeground(bgGameGetShared(), true);
          /* Single-player runs through the same serverInstanceStartup +
           * timer-thread ticking path as the host, so SP and listen-server
           * share one shutdown path and the bot tick is owned by the
           * timer thread only.  acceptRemoteClients=false short-circuits
           * the UDP server / WBN / tracker / NAT-portmap bring-up. */
          /* Resolve the brain path once for both the cfg-driven setter
           * application inside serverInstanceStartup and the later bot
           * setup loop. Falls back to findBrainPath when the user has
           * not pre-set gameFrontBrainPath. */
          char spBrainPath[FILENAME_MAX] = "";
          if (gameFrontBrainPath[0] != '\0') {
            SDL_strlcpy(spBrainPath, gameFrontBrainPath, sizeof(spBrainPath));
          } else {
            findBrainPath(spBrainPath, sizeof(spBrainPath));
          }

          ServerInstanceConfig cfg;
          memset(&cfg, 0, sizeof(cfg));
          cfg.udpPort             = gameFrontMyUdp;
          cfg.bindAddr            = "";
          cfg.password            = password;
          cfg.maxPlayers          = MAX_TANKS;
          cfg.acceptRemoteClients = false;
          cfg.useWbn              = false;
          cfg.compTanks           = (BYTE)compTanks;
          cfg.useTracker          = false;
          cfg.trackerAddr         = gameFrontTrackerAddr;
          cfg.trackerPort         = gameFrontTrackerPort;
          cfg.useNatKeepalive     = false;
          cfg.useNatPortmap       = false;
          if (isTutorial) {
            cfg.skipLobby         = true;
          } else {
            cfg.lobbyEnabled      = true;
            cfg.emptyResetEnabled = true;
            cfg.hasPassword       = (password[0] != '\0');
          }
          cfg.botBrainPath = (spBrainPath[0] != '\0') ? spBrainPath : NULL;
          cfg.botAiType    = (BYTE)((compTanks == aiNone) ? aiFull : compTanks);

          /* Build the ClientSim first — clientSimConnectLocalPassive
           * runs the full join+install body against an alive ClientSim. */
          humanSim = clientSimAlloc();
          clientSimCreate(humanSim);
          clientSimSetIsLanOnly(humanSim, s_isLanOnly);
          frontEndSetActiveClientSim(humanSim);

          /* Start the host timer; serverInstanceStartup applies the
           * lobby/skipLobby + hasPassword + brain/AI fields above. */
          if (!gameFrontStartServerSim(spServerSim, &cfg)) {
            WB_LOG_ERROR(WB_LOG_CAT_GUI,
                         "[SP-FAIL] gameFrontStartServerSim failed — "
                         "humanSim being nulled, dlgState stays at openFinished");
            frontEndSetActiveClientSim(NULL);
            clientSimDestroy(humanSim);
            humanSim = NULL;
            serverSimDestroy(spServerSim);
            spServerSim = NULL;
            spServerSimActive = FALSE;
            spHumanSubHandle = SUBSCRIBER_HANDLE_INVALID;
            returnValue = FALSE;
          } else {
            /* Resolve the self client type / flags. */
            uint8_t selfType  = bolo_detect_client_type();
            uint8_t selfFlags = 0;
#ifdef HAVE_STEAM
            selfFlags |= PLAYER_FLAG_STEAM_BUILD;
#endif
            if (bolo_steam_has_supporter_dlc()) selfFlags |= PLAYER_FLAG_SUPPORTER;

            /* Run the 12-step join+install body. On success the
             * ClientSim is populated with the map, the local tank, the
             * first snapshot, the auto-subscriber, and a netSingle
             * net type. */
            bool connectOk = clientSimConnectLocalPassive(
                humanSim, spServerSim,
                gameFrontName,
                winbolonetGetCountryCode(),
                selfType, selfFlags);

            if (!connectOk) {
              WB_LOG_ERROR(WB_LOG_CAT_GUI,
                           "[SP-FAIL] clientSimConnectLocalPassive failed: %s — "
                           "humanSim being nulled, dlgState stays at openFinished",
                           clientSimGetConnectErrorReason(humanSim));
              frontEndSetActiveClientSim(NULL);
              clientSimDestroy(humanSim);
              humanSim = NULL;
              /* gameFrontStartServerSim already armed the host timer;
               * gameFrontShutdownServer tears it down and runs
               * serverInstanceShutdown + serverSimDestroy on
               * spServerSim under the mutex. */
              gameFrontShutdownServer();
              spServerSimActive = FALSE;
              spHumanSubHandle = SUBSCRIBER_HANDLE_INVALID;
              returnValue = FALSE;
            } else {
            spServerSimActive = TRUE;
            /* Phase 2 leaves the legacy spHumanSubHandle field unset —
             * the auto-subscriber registered by connect lives inside
             * the ClientSim. Phase 4 deletes the field. */
            spHumanSubHandle = SUBSCRIBER_HANDLE_INVALID;
            if (gameFrontRemeber) clientSimSetMyLastPlayerName(humanSim, gameFrontName);
            /* Set up networking state after ClientSim is fully initialized */
            netSetup(humanSim, netSingle, gameFrontMyUdp, gameFrontUdpAddress, gameFrontTargetUdp,
                     password, TRUE, gameFrontTrackerAddr, gameFrontTrackerPort,
                     gameFrontTrackerEnabled, wantRejoin, gameFrontWbnUse,
                     gameFrontWbnToken, "");
            /* Non-tutorial SP enters the lobby; the host clicks Start
             * to fire clientSimNetSendReady, which trips the
             * all-ready detector and (worldPreLoaded → TRUE) runs
             * StartGameInPlace synchronously. netSetup just overwrote
             * netStat to netRunning, so we re-set the four lobby
             * flags here (after netSetup, the right moment). */
            if (!isTutorial) {
              clientSimSetIsSinglePlayer(humanSim, true);
              clientSimSetInLobby(humanSim, true);
              clientSimSetNetStatus(humanSim, netLobby);
              clientSimSetMapDownloadComplete(humanSim, true);
            }
            /* Tutorial (no-lobby) and lobby paths both rely on the
             * first-snapshot apply inside the local transport to fire
             * the viewport finalisation. */
            /* Add bot brains for local game if AI is enabled.
             * Serialise bot creation, team assignment, and the
             * reapply-alliances pass against the host timer thread,
             * which is already calling serverInstanceTick on spServerSim. */
            threadsWaitForMutex();
            /* botBrainPath / botAiType were already pushed into the
             * sim via cfg above; here we only need brainPath as a
             * per-bot default for the serverSimCreateBot loop. */
            bool haveBrain = (spBrainPath[0] != '\0');
            if (compTanks != aiNone && gameFrontBotSetupData.count > 0 && haveBrain) {
              for (int bi = 0; bi < gameFrontBotSetupData.count && bi < MAX_BOT_SLOTS; bi++) {
                BYTE slot = (BYTE)(bi + 1);
                char botName[32];
                snprintf(botName, sizeof(botName), "Bot %d", slot);
                /* Use per-bot brain path if set, otherwise fall back to default */
                const char *botBrain = gameFrontBotSetupData.bots[bi].brainPath;
                if (botBrain[0] == '\0') botBrain = spBrainPath;
                serverSimCreateBot(spServerSim, slot, botBrain, botName, compTanks, gametype, hiddenMines);
                /* Apply team number */
                uint8_t team = gameFrontBotSetupData.bots[bi].teamNumber;
                if (team > 0) {
                  clientSimNetSendTeamSet(humanSim, slot, team);
                }
              }
              /* Apply human player team number */
              if (gameFrontBotSetupData.playerTeamNumber > 0) {
                clientSimNetSendTeamSet(humanSim, 0,
                                        gameFrontBotSetupData.playerTeamNumber);
              }
              /* Apply team alliances — players with same non-zero team become
               * allies. The alliance pass that runs as part of round
               * start at the top of this block saw an empty lobby (the
               * human and bots hadn't been added yet) and found no
               * pairs. Re-run it now that the lobby is populated. */
              serverSimReapplyTeamAlliances(spServerSim);
            }
            threadsReleaseMutex();
            gameFrontUpdateSteamPresence(humanSim);
            }  /* end "clientSimConnectLocalPassive succeeded" */
          } /* end "gameFrontStartServerSim succeeded" */
        } else {
          WB_LOG_ERROR(WB_LOG_CAT_GUI,
                       "[SP-FAIL] spServerSim creation returned NULL "
                       "(fileName='%s') — humanSim never allocated, "
                       "dlgState stays at openFinished",
                       fileName);
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

void gameFrontRequestTransition(openingStates s) {
  gameFrontPendingTransition    = s;
  gameFrontPendingTransitionSet = TRUE;
}

bool gameFrontConsumeRequestedTransition(openingStates *out) {
  if (!gameFrontPendingTransitionSet) return FALSE;
  if (out) *out = gameFrontPendingTransition;
  gameFrontPendingTransitionSet = FALSE;
  return TRUE;
}

bool gameFrontIsAtWelcome(void) {
  return gameFrontAtWelcome;
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
  /* Tell the timer callback to bail and cancel itself BEFORE we call
   * SDL_RemoveTimer — RemoveTimer doesn't wait for an in-flight
   * callback, and a callback that's already past the mutex lock can
   * return non-zero and re-arm the timer despite the remove. With
   * the flag, any cb invocation past this point returns 0. */
  spServerTimerShutdown = true;
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


static bool gameFrontStartServerSim(ServerSim *sim,
                                    const ServerInstanceConfig *cfg) {
  if (!serverInstanceStartup(sim, cfg)) return false;
  /* Clear the shutdown latch — gameFrontShutdownServerSim sets it true
   * on teardown to make in-flight timer callbacks bail; if we restart
   * a hosted server in the same process (e.g. exiting a game back to
   * the lobby and starting another) the latch would still be true and
   * the freshly-installed timer would self-cancel on its first fire. */
  spServerTimerShutdown = false;
  hostedServerTimerID = SDL_AddTimer(SERVER_TICK_LENGTH, hostedServerTimerCb, NULL);
  if (hostedServerTimerID == 0) {
    serverInstanceShutdown(sim);
    return false;
  }
  return true;
}

bool gameFrontSetupServer(void) {
  ServerInstanceConfig cfg;

  /* Idempotently clear any prior host session. Backing out of the
   * lobby to the LAN/Internet game finder doesn't fire shutdown on
   * its own — the dlgState machine just transitions back to openLan
   * while the old timer thread, UDP socket, and ServerSim are still
   * alive. Without this, the second "New" overwrites spServerSim
   * (leaking the first) and memsets the global udpServer struct
   * (orphaning the first socket and recv thread). The subsequent
   * discoveryPingServer then sees no info-reply and hangs the main
   * thread for the full 5-second recvfrom timeout. Shutdown no-ops
   * when spServerSimActive is false, so this is safe on first entry. */
  gameFrontShutdownServer();

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
    spServerSim = serverSimCreateCompressed(emap, 5097, "Everard Island", gametype, hiddenMines, startDelay, timeLen);
  }
  if (spServerSim == NULL) {
    return FALSE;
  }
  /* Embedded listen server: silence its console messages — no server console. */
  serverSimSetQuiet(spServerSim, true);

  /* Resolve a brain path so the lobby's "Add Bot" works regardless of
   * whether the host set compTanks at startup. The AI Policy can be
   * flipped on later via the lobby UI; without a pre-resolved brain
   * the server silently drops PACKET_LOBBY_ADD_BOT. */
  char brainPath[FILENAME_MAX] = "";
  if (gameFrontBrainPath[0] != '\0') {
    SDL_strlcpy(brainPath, gameFrontBrainPath, sizeof(brainPath));
  } else {
    findBrainPath(brainPath, sizeof(brainPath));
  }
  memset(&cfg, 0, sizeof(cfg));
  cfg.udpPort             = gameFrontMyUdp;
  cfg.bindAddr            = "";
  cfg.password            = password;
  cfg.maxPlayers          = MAX_TANKS;
  cfg.acceptRemoteClients = true;
  /* Internet games always register on WinBolo.net so the server is
   * publicly listed, regardless of whether the host is signed in:
   * server/register is anonymous and returns its own server_token for
   * later server/ calls. Being signed in (gameFrontWbnUse) only governs
   * whether the host's own tank is WBN-identified for ratings, handled
   * separately on the client-join path. The LAN-only block below forces
   * this back off for Local games. */
  cfg.useWbn              = TRUE;
  cfg.compTanks           = (BYTE)compTanks;
  cfg.useTracker          = gameFrontTrackerEnabled;
  cfg.trackerAddr         = gameFrontTrackerAddr;
  cfg.trackerPort         = gameFrontTrackerPort;
  cfg.useNatKeepalive     = gameFrontUseNatTraversal;
  cfg.useNatPortmap       = gameFrontUseUpnp;
  cfg.lobbyEnabled        = true;
  cfg.emptyResetEnabled   = true;
  cfg.hasPassword         = (password[0] != '\0');
  cfg.botBrainPath        = (brainPath[0] != '\0') ? brainPath : NULL;
  if (compTanks != aiNone) {
    cfg.botAiType         = (BYTE)compTanks;
  }

  /* LAN-only host: no public-facing services. winbolonetCreateServer
   * would advertise to the global tracker, the WBN tracker reports
   * public IPs, NAT keepalive ("punch") makes the server reachable
   * from outside the LAN, and UPnP/PCP/NAT-PMP open router ports —
   * none of which the user wants for a Local game. Disabling these
   * also makes serverInstanceIsNatPunchActive() return false, which
   * skips the lobby's "Checking server reachability…" badge for both
   * the host and joining LAN clients. */
  if (s_isLanOnly) {
    cfg.useWbn          = FALSE;
    cfg.useTracker      = FALSE;
    cfg.useNatKeepalive = FALSE;
    cfg.useNatPortmap   = FALSE;
  }

  if (!gameFrontStartServerSim(spServerSim, &cfg)) {
    serverSimDestroy(spServerSim);
    spServerSim = NULL;
    return FALSE;
  }
  /* The host-side flags previously set by gameFrontFinishLobbyHost
   * after the startup call. The lobby state itself is now driven by
   * cfg.lobbyEnabled inside serverInstanceStartup. */
  isServer = TRUE;
  spServerSimActive = TRUE;
  bgGameSetHiddenByForeground(bgGameGetShared(), true);
  return TRUE;
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
  GetPrivateProfileString("MENU", "Sound Volume", "50", buff, FILENAME_MAX, prefsFile);
  soundVolume = atoi(buff);
  if (soundVolume < 0) soundVolume = 0;
  if (soundVolume > 100) soundVolume = 100;
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
  intToStr(soundVolume, buff, sizeof(buff));
  WritePrivateProfileString("MENU", "Sound Volume", buff, prefsFile);
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

/* Throttle state shared with gameFrontPumpDirty: the pump is the consume
 * point for s_windowSettingsDirty, so the last drag/resize burst doesn't
 * get silently dropped when its events fall inside the 500ms window. */
static Uint64 s_lastWindowWriteTime = 0;

void gameFrontSaveWindowSettings(void) {
  Uint64 now = SDL_GetTicks();
  if (now - s_lastWindowWriteTime < 500) {
    s_windowSettingsDirty = true;
    return;
  }
  gameFrontFlushWindowSettings();
  s_lastWindowWriteTime = now;
}

void gameFrontPumpDirty(void) {
  if (!s_windowSettingsDirty) return;
  Uint64 now = SDL_GetTicks();
  if (now - s_lastWindowWriteTime < 500) return;
  gameFrontFlushWindowSettings();
  s_lastWindowWriteTime = now;
}

ServerSim *gameFrontGetServerSim(void) {
  return spServerSimActive ? spServerSim : NULL;
}

ServerSim *gameFrontGetSinglePlayerServerSim(void) {
  return spServerSim;
}

BYTE gameFrontGetPlayerNum(void) {
  if (udpTransportActive) return udpPlayerNum;
  return 0;
}

