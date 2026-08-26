/*
 * Copyright (c) 1998-2026 John Morrison.
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
*  Reads and writes settings through the process-global
*  preferences document (common/prefs.h, WinBolo.json).
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
#include "../../common/prefs.h"
#include "bolo_rand.h"
#include "client_frontend_connect.h"
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
#include "../ui_mode.h"
#include "../winbolo.h"
#include "sdl3draw.h"
#include "sdl3imgui.h"
#include "input_gamepad.h"
#include "build_cursor.h"
#include "luabrainshandler.h"
#include "dialog_backend.h"
#include "dialogs/imgui_mapchooser.h"
#include "dialogs/imgui_messagebox.h"
#include "bg_game.h"

#include "everard_map.h"
#include "lobby_bot_pools.h"
#include "platform_net.h"
#include "playername_validate.h"
#include "client_net.h"
#include "../../server/server_lifecycle.h"
#include "../../server/server_dedicated_log.h"
#include "../../winbolonet/winbolonet_client.h"
#include "../../winbolonet/winbolonet_core.h"
#include "../../winbolonet/winbolonet_server.h"
#include "../../winbolonet/http.h"
#include "../../winbolonet/wbn_prefs_sync.h"
#include "../../common/prefs_doc.h"
#include "../../steam/steam_wrapper.h"
#include "../../mapeditor/mapeditor.h"
#include "mapgen.h"
/* Forward declaration only — don't include logviewer.h to avoid type conflicts
   between src/logviewer/ and src/bolo/ headers (both define map, bases, etc.) */
void logViewerRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  const char *logPath, bool fromMainMenu);
void logViewerRunFromMemory(struct SDL_Window *window, struct SDL_Renderer *renderer,
                            uint8_t *zipData, size_t zipLen, bool fromMainMenu);
bool spectatorRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  void *cs, const char *serverHost, uint16_t serverPort);

#include "dialogs/imgui_wbn_browser.h"
#include "dialogs/imgui_onboarding.h"
#include "dialogs/imgui_lobby.h"

#ifndef DEFAULT_UDP_PORT
#define DEFAULT_UDP_PORT 27500
#endif

/* Number of bot players for local/practice games */
#define LOCAL_GAME_NUM_BOTS 0

/* -------------------------------------------------------
 * getPreferenceFilePath — return absolute path to WinBolo.json
 *
 * Uses SDL_GetPrefPath so settings survive across sessions
 * regardless of CWD.
 * ------------------------------------------------------- */
static const char *getPreferenceFilePath(void) {
  static char path[FILENAME_MAX];
  static bool resolved = false;
  if (!resolved) {
    const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir) {
      snprintf(path, sizeof(path), "%sWinBolo.json", prefDir);
    } else {
      /* Fallback to relative path if SDL_GetPrefPath fails */
      snprintf(path, sizeof(path), "%s", "WinBolo.json");
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
        "Brains/GoalHunter_1.6/init.lua",
        "brains/GoalHunter_1.6/init.lua",
        "data/Brains/GoalHunter_1.6/init.lua",
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

/* Single-player default-bot skill guess. Returns the brain dir name for an
 * auto-seeded SP bot: the harder "GoalHunter_1.6" only when the player is
 * signed in to WinBolo.net with more than 5 games on record; otherwise the
 * gentler "GoalHunter_1.0" (the default for everyone not signed in). */
static const char *gameFrontGuessSpBotBrain(void) {
    /* Gospel: if the player has ever explicitly picked a brain from the lobby
     * wrench dropdown, honour that from then on, ignoring the skill guess. */
    static char chosen[64];
    gameFrontGetChosenBotBrain(chosen, sizeof(chosen));
    if (chosen[0] != '\0') return chosen;

    WbnStats st;
    gameFrontGetWinbolonetStats(&st);
    if (st.valid) {
        int games = 0;
        const WbnModeStats *modes[3] = { &st.open, &st.tourn, &st.strict };
        for (int i = 0; i < 3; i++) {
            if (modes[i]->numGames > 0) games += modes[i]->numGames;
        }
        if (games > 5) return "GoalHunter_1.6";
    }
    return "GoalHunter_1.0";
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

/* Client-hosting settings ([HOSTING] section). Defaults match the historical
 * hard-coded listen-server behaviour plus the newly-exposed knobs. */
unsigned short gameFrontHostingPort            = DEFAULT_UDP_PORT;
bool           gameFrontHostingAllowSpec       = TRUE;
int            gameFrontHostingMaxSpec         = 16;
int            gameFrontHostingUploadPolicy    = UPLOAD_POLICY_ALLOW;
int            gameFrontHostingUploadMaxFiles  = 64;
int            gameFrontHostingUploadMaxStorage = 8;
/* Persist upload dir. Empty until gameFrontGetPrefs seeds the default
 * (<prefs path>uploads) or the user picks one. */
char           gameFrontHostingUploadDir[FILENAME_MAX] = "";
bool           gameFrontHostingLogging         = TRUE;
/* Round-log dir. Empty until gameFrontGetPrefs seeds the default
 * (the prefs path) or the user picks one. */
char           gameFrontHostingLogDir[FILENAME_MAX] = "";
bool           gameFrontHostingServeReplays   = TRUE;

/* Tutorial: shown on the welcome menu until the player completes it.
 * Defaults to TRUE on a fresh install (key absent from INI). The player
 * can toggle it back on from the Settings dialog at any time. */
static bool gameFrontShowTutorialButton = TRUE;

/* Persisted BCP-47 language code (e.g. "en", "de", "pt-br"). Empty
 * string means the user has not picked one yet — Phase 5 startup runs
 * langAutoDetect() in that case. */
static char gameFrontLanguageCode[32] = "";

/* One-shot flag set by the Settings dialog's "Play Tutorial" button.
 * Consumed by the openSettings handler in gameFrontDialogs() so that
 * settings → tutorial transitions in one menu cycle. */
static bool gameFrontPlayTutorialRequested = FALSE;

/* One-shot flag set when a Steam "join game" arrives (cold launch, in-game,
 * or welcome screen). Consumed by the UDP setup dialog, which auto-fires its
 * Join button after a brief visible dwell so the player sees the pre-filled
 * server address before the connection starts. */
static bool gameFrontUdpAutoJoinRequested = FALSE;

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

/* WinBolo.net 1v1 ladder position, refreshed on every auth/validate.
 * rank is -1 when unranked or unknown; total is 0 until first populated. */
int gameFrontWbnRank = -1;
int gameFrontWbnRankTotal = 0;

/* Per-mode WinBolo.net play stats, refreshed on every auth/validate.
 * `valid` is FALSE until the first response that carries a stats object. */
static WbnStats gameFrontWbnStats;

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

/* Set by gameFrontSetServerPaused while a single-player game is paused
 * (Steam overlay open, or the app backgrounded). The hosted-server timer
 * stays armed but skips serverInstanceTick, freezing the world without
 * tearing the timer down. Only ever set for single-player — a listen-server
 * host keeps simulating so remote players aren't frozen. */
static volatile bool spServerPaused = false;

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
  bool alive = (!spServerTimerShutdown && spServerSim != NULL);
  /* Paused freezes the tick but keeps the timer armed (return interval),
   * so a single-player resume picks straight back up. */
  if (alive && !spServerPaused) {
    serverInstanceTick(spServerSim);
  }
  threadsReleaseMutex();
  return alive ? interval : 0;
}

void gameFrontSetServerPaused(bool paused) {
  spServerPaused = paused ? true : false;
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
static void gameFrontSetConnectIfAvailable(ClientSim *cs) {
  char connect[FILENAME_MAX];
  /* Host on an Internet game: advertise the externally-reachable address
   * (the same one the lobby header shows) so friends connect to us, not to
   * our loopback/private gameFrontUdpAddress. Mirrors the lobby's own
   * address-replacement gating in imgui_lobby. */
  if (cs != NULL && !clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs) &&
      serverInstanceIsNatPunchActive()) {
    ServerPortmapInfo pm;
    serverInstanceGetPortmapInfo(&pm);
    if (pm.externalIp[0] != '\0' && pm.externalPort != 0) {
      snprintf(connect, sizeof(connect), "+connect %.255s:%u",
               pm.externalIp, (unsigned)pm.externalPort);
      steam_set_rich_presence("connect", connect);
      return;
    }
  }
  /* Client (or host before its external address has resolved): the address
   * we joined is reachable by our friends too. */
  if (udpTransportActive && gameFrontUdpAddress[0] != '\0') {
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
  /* "Solo" is a property of the game *mode*, not the live player count: an
   * Internet game with one player present is still an open, joinable game
   * and must not read as solo. Single-player is the only true solo case. */
  steam_set_rich_presence("steam_display",
                          clientSimIsSinglePlayer(cs) ? "#StatusInGameSolo"
                                                      : "#StatusInGamePlural");
  gameFrontSetConnectIfAvailable(cs);
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
  gameFrontSetConnectIfAvailable(cs);
}

/* Steam rate-limits rich-presence updates, so the per-frame lobby/game
 * loops refresh through these throttled wrappers instead of pushing every
 * frame. Both share one timer (you're only ever in one state at a time);
 * the first call after a quiet period fires immediately. */
#define STEAM_PRESENCE_REFRESH_MS 3000u
static uint32_t s_steamPresenceLastMs = 0;

static bool gameFrontSteamPresenceDue(void) {
  uint32_t now = (uint32_t)SDL_GetTicks();
  if (s_steamPresenceLastMs != 0 &&
      (now - s_steamPresenceLastMs) < STEAM_PRESENCE_REFRESH_MS) {
    return false;
  }
  s_steamPresenceLastMs = now;
  return true;
}

void gameFrontTickSteamPresenceLobby(ClientSim *cs) {
  if (gameFrontSteamPresenceDue()) gameFrontSetSteamPresenceLobby(cs);
}

void gameFrontTickSteamPresenceGame(ClientSim *cs) {
  if (gameFrontSteamPresenceDue()) gameFrontUpdateSteamPresence(cs);
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
extern bool letterboxBarsGray;
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

  /* Load the process-global preferences document (WinBolo.json) before any
   * prefs access. */
  prefsInit(getPreferenceFilePath());
  /* Coalesce bursts of pref edits (multi-key rebind, slider drag) into a
   * single trailing disk write, driven from gameFrontPumpDirty and flushed
   * before each join and at shutdown. */
  prefsSetAutosaveDebounce(15000);

  langSetup();

  /* Replace the built-in bot naming pools with the shipped
   * data/bot_names.json so the lobby dropdown and on-add name picks
   * match what the server (and other clients shipping the same file)
   * expect.  Falls back silently to the compiled-in defaults if the
   * file is missing or unreadable. */
  {
    LobbyBotPoolLoadStats poolStats;
    if (lobbyBotPoolsLoadDefault(&poolStats)) {
      WB_LOG_INFO(WB_LOG_CAT_ASSET,
                  "bot name pools: loaded %d pool(s) from data/bot_names.json",
                  poolStats.poolsKept);
    }
  }

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

    /* Bring up the gamepad subsystem now (before any dialogs run) so the
       ImGui SDL3 backend can enumerate gamepads in the welcome / lobby /
       settings / etc. dialogs and route D-pad + face buttons through nav.
       inputGamepadInit() is still called later (when the game window is
       set up) to register edge-trigger state for the in-game input path —
       that second call is a no-op for the subsystem (ref-counted). */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);

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

/* Snapshot the current key bindings via the winbolo.c global and
   write the full prefs file. Cheap enough for toggle-handler use. */
void gameFrontSaveCurrentPrefs(void) {
  keyItems k;
  windowGetKeys(&k);
  gameFrontPutPrefs(&k);
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
  if (isQuiting != TRUE) {
    /* Drop any bot-pool catalog a server streamed us this session so the
     * previous server's themed pools never linger in a later lobby, then
     * restore our own shipped default. The next server we join re-sends
     * its catalog during join sync. (Skipped when quitting — nothing
     * will read the table again.) */
    lobbyBotPoolsReset();
    lobbyBotPoolsLoadDefault(NULL);
  }
  /* Don't call windowSaveCurrentPosition() here - we already save the corrected
     position on every resize/move. Calling it here would overwrite the corrected
     position (e.g. centered within maximized bounds) with the actual position. */
  gameFrontPutPrefs(keys);
  prefsFlush();
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
      if (!gameFrontOnboardingComplete()) {
        if (!imguiOnboardingShow()) { dlgState = openWelcome; break; }
      }
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
      if (!gameFrontOnboardingComplete()) {
        if (!imguiOnboardingShow()) { dlgState = openWelcome; break; }
      }
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
    case openSpectate: {
      /* Spectate the server the browser selected: connect a tankless
       * spectator and hand it to the modal spectator host, which borrows
       * the main window until the user exits (window-close or Esc).
       *
       * The connection rides a fresh, self-contained ClientSim — NOT the
       * shared humanSim. A spectator needs none of the player-session
       * wiring (chat/alliance callbacks, active-clientsim, steam presence)
       * and the whole connect/run/disconnect cycle lives inside this case,
       * so leaving humanSim untouched (NULL) guarantees a later Join starts
       * from a clean slate exactly as if no spectate had happened.
       *
       * The address/port/name were stashed by gameFrontSetUdpOptions when
       * the Spectate button fired (same path Join uses). The tracker is
       * gated like a join (Internet uses it for NAT traversal, LAN doesn't);
       * no WBN token — a spectator registers no identity. */
      ClientSim *spectatorSim = clientSimAlloc();
      clientSimCreate(spectatorSim);
      clientSimConnectUdp(spectatorSim, gameFrontUdpAddress, gameFrontTargetUdp,
                          gameFrontName, winbolonetGetCountryCode(), password,
                          "", "", FALSE,
                          !s_isLanOnly ? gameFrontTrackerAddr : "",
                          gameFrontTrackerPort,
                          /*spectator*/ TRUE);
      if (clientSimGetConnectState(spectatorSim) == CLIENT_CONNECT_ERROR) {
        const char *reason = clientSimGetConnectErrorReason(spectatorSim);
        imguiMessageBoxEx(DIALOG_BOX_TITLE,
                          (reason && reason[0]) ? reason
                                                : langGetText(NETERR_SERVERCONNECT),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
      } else {
        /* clientSimConnectUdp only fires the JOIN; the spectator accept — which
         * carries the initial live/delayed mode byte — lands on a later
         * transport tick. Pump until the handshake reaches SPECTATING (so the
         * mode bit is known before the first view is chosen) or it fails, the
         * same wait the join path runs before entering the lobby. */
        int specWaitTicks = 0;
        while (specWaitTicks < 1500) {  /* 30 second timeout */
          ClientConnectState ss = clientSimGetConnectState(spectatorSim);
          if (ss == CLIENT_CONNECT_SPECTATING) break;
          if (ss == CLIENT_CONNECT_ERROR ||
              ss == CLIENT_CONNECT_SERVER_SHUTDOWN ||
              ss == CLIENT_CONNECT_KICKED) {
            break;
          }
          clientSimNetTick(spectatorSim);
          SDL_Delay(20);
          specWaitTicks++;
        }

        if (clientSimGetConnectState(spectatorSim) != CLIENT_CONNECT_SPECTATING) {
          const char *reason = clientSimGetConnectErrorReason(spectatorSim);
          imguiMessageBoxEx(DIALOG_BOX_TITLE,
                            (reason && reason[0]) ? reason
                                                  : langGetText(NETERR_SERVERCONNECT),
                            IMGUI_MSG_ERROR, IMGUI_MSG_OK);
        } else {
          /* Dual-mode session: the live read-only lobby while the server is in
           * lobby/countdown, the delayed game once it starts. The mode follows
           * which feed is arriving (clientSimSpectatorIsLiveLobby — seeded from
           * the accept byte, flipped by the feeding channel). imguiLobbyShow
           * returns 1 when the delayed feed begins at game start (a spectator
           * never reaches the RUNNING phase the player path keys on, since the
           * server unsubscribes it before that publish); spectatorRun returns
           * true when live lobby control resumes after the delayed game drains.
           * Any other return (user left / lost connection / quit) ends it. */
          for (;;) {
            if (clientSimSpectatorIsLiveLobby(spectatorSim)) {
              if (imguiLobbyShow(spectatorSim) != 1) break;
            } else if (!spectatorRun(sdl3DrawGetWindow(), sdl3DrawGetRenderer(),
                                     spectatorSim, gameFrontUdpAddress,
                                     gameFrontTargetUdp)) {
              break;
            }
          }
          /* spectatorRun retitles the borrowed window for the live session;
           * restore the normal app title now that the session has ended. */
          SDL_SetWindowTitle(sdl3DrawGetWindow(), WIND_TITLE);
        }
      }
      /* Caller owns the ClientSim lifetime (spectatorRun never disconnects):
       * tear it down so the socket/transport is released before returning. */
      clientSimDisconnect(spectatorSim);
      clientSimDestroy(spectatorSim);
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

bool gameFrontGetSteamTicketHex(char *outHex, size_t outSize) {
  uint8_t ticketBuf[1024];
  uint32_t ticketLen = 0;
  if (!steam_get_auth_ticket(ticketBuf, sizeof(ticketBuf), &ticketLen) ||
      ticketLen == 0) {
    return false;
  }
  /* Each byte expands to two hex chars plus the NUL terminator. */
  if (outSize < (size_t)ticketLen * 2 + 1) {
    return false;
  }
  uint32_t i;
  for (i = 0; i < ticketLen; i++) {
    snprintf(outHex + i * 2, 3, "%02x", ticketBuf[i]);
  }
  outHex[ticketLen * 2] = '\0';
  return true;
}

void gameFrontApplySteamAuthResult(const char *token, const char *expiry,
                                   const char *playerName, int rank,
                                   int rankTotal, const WbnStats *stats) {
  gameFrontSetWinbolonetToken(token, expiry);
  gameFrontSetWbnAuthMethod("steam");
  gameFrontSetWinbolonetRank(rank, rankTotal);
  gameFrontSetWinbolonetStats(stats);
  if (playerName[0] != '\0') {
    /* The WBN account display_name is canonical: it wins over any locally
     * chosen name, matching the username/password sign-in path. Validate it
     * before applying; on rejection keep an existing name, or fall back to
     * the default when there is none. */
    char validated[PLAYER_NAME_LEN];
    if (playerNameValidate(playerName, validated, PLAYER_NAME_LEN, NULL)) {
      gameFrontSetPlayerName(validated);
    } else {
      char persisted[PLAYER_NAME_LEN];
      persisted[0] = '\0';
      gameFrontGetPlayerName(persisted);
      if (persisted[0] == '\0') {
        gameFrontSetPlayerName((char *)langGetText(STR_DLGGAMESETUP_DEFAULTNAME));
      }
    }
  }
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Steam] Authenticated with WinBolo.net via Steam");
  /* First Steam auth of the session: the token went from empty to set,
   * so pull the cloud prefs once. Placed after the name handling so the
   * sync captures the name that should win over the synced Player Name.
   * The per-join validate of an existing token below never reaches here. */
  gameFrontStartPrefsSync();
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

  /* If no WBN token exists, try automatic Steam authentication — unless the
   * player explicitly signed out, in which case joining must not silently
   * sign them back in. */
  if (token[0] == '\0') {
    char ticketHex[2049];
    if (!gameFrontGetWbnSignedOut() &&
        gameFrontGetSteamTicketHex(ticketHex, sizeof(ticketHex))) {
      char tokenOut[256], expiryOut[256], playerName[PLAYER_NAME_LEN], errorMsg[512];
      int rank = -1, rankTotal = 0;
      WbnStats stats;
      stats.valid = FALSE;
      tokenOut[0] = expiryOut[0] = playerName[0] = errorMsg[0] = '\0';

      if (winbolonetAuthSteam(ticketHex, tokenOut, expiryOut, playerName,
                              &rank, &rankTotal, &stats, errorMsg)) {
        gameFrontApplySteamAuthResult(tokenOut, expiryOut, playerName,
                                      rank, rankTotal, &stats);
      } else {
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Steam] WBN Steam auth failed: %s", errorMsg);
      }
      steam_cancel_auth_ticket();
    }
    return;
  }

  char playerName[PLAYER_NAME_LEN];
  char errorMsg[512];
  int rank = -1, rankTotal = 0;
  WbnStats stats;
  stats.valid = FALSE;
  playerName[0] = '\0';
  errorMsg[0] = '\0';

  if (winbolonetAuthValidate(token, playerName, &rank, &rankTotal, &stats, errorMsg)) {
    gameFrontSetWinbolonetRank(rank, rankTotal);
    gameFrontSetWinbolonetStats(&stats);
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
     * fall through to the standard "server unreachable" error.
     *
     * Skip when joining our own freshly-started server: the version
     * is BOLO_VERSION_* by definition, and the round-trip races the
     * host timer's first fire — under rapid open-LAN-finder /
     * New-Game cycling on Windows the response can miss the 5-second
     * SO_RCVTIMEO and look like a crash. spServerSimActive is set
     * by gameFrontSetupServer two stack frames up before this
     * recurses into openUdpJoin, so it's only true on the self-host
     * path; every remote join (LAN finder Join, Manual Connect,
     * tracker Join) leaves it false and still pre-flights. */
    if (!spServerSimActive) {
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

    prefsFlush();
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
    /* Match the Internet-host config in gameFrontSetupServer: an Internet
     * join turns the tracker on (NAT traversal + external-address
     * resolution) and, if the player is signed in, sends the WBN identity
     * token. LAN joins and SP/tutorial stay private — no tracker, no WBN.
     * Gated on s_isLanOnly (false only for Internet games), not on the old
     * buried default-off "Use Tracker" checkbox. WBN from the join side
     * carries only the player's own identity — there is no server being
     * registered here — so it follows the sign-in state. */
    clientSimConnectUdp(humanSim, gameFrontUdpAddress,
                        gameFrontTargetUdp,
                        gameFrontName,
                        winbolonetGetCountryCode(),
                        password,
                        (!s_isLanOnly && gameFrontWbnUse) ? gameFrontWbnToken : "",
                        "",
                        wantRejoin,
                        !s_isLanOnly ? gameFrontTrackerAddr : "",
                        gameFrontTrackerPort,
                        /*spectator*/ false);
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
      /* Wait for the join handshake (30s timeout). Landing accepts either a
       * running game or entry into the server lobby — see clientFrontAwaitJoin. */
      if (clientFrontAwaitJoin(humanSim, 1500)) {
        udpPlayerNum = clientSimGetServerPlayerNum(humanSim);
        udpTransportActive = TRUE;

        /* Store server address in ClientSim for brain info */
        {
          struct sockaddr_in saddr;
          memset(&saddr, 0, sizeof(saddr));
          saddr.sin_family = AF_INET;
          saddr.sin_addr.s_addr = inet_addr(gameFrontUdpAddress);
          if (saddr.sin_addr.s_addr == INADDR_NONE) {
            bolo_resolve_ipv4(gameFrontUdpAddress, &saddr.sin_addr);
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

        /* The lobby landing (netLobby — lobby UI, map downloads in the
         * background, ready button gated on the real mapDownloadComplete) is
         * settled inside clientFrontAwaitJoin. Only the no-lobby path has extra
         * work: the transport already installed the map inline on MAP_DOWNLOAD
         * completion, and the first snapshot apply fires the viewport
         * finalisation — update Steam presence now that we're in a game. */
        if (!clientSimIsInLobby(humanSim)) {
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
    prefsFlush();
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
        /* Single-player opens the lobby at sensible defaults the host can
         * still change inline before Start: Open game, Full Advantage AI,
         * and one enemy bot. Held in SP-local values so the host-game path
         * (gameFrontSetupServer) keeps its own settings. The tutorial forces
         * a solo strict-tournament game with no AI, ignoring any SP-lobby
         * settings left over from earlier in the session. */
        gameType spGameType = isTutorial ? gameStrictTournament : gameOpen;
        aiType   spAiPolicy = isTutorial ? aiNone : aiFull;
        /* Seed one enemy bot when the launch carried no bot setup: human
         * on team 1, the bot on team 2 so they oppose each other. A setup
         * the user already configured (count > 0) is left untouched. */
        if (!isTutorial && gameFrontBotSetupData.count == 0) {
          memset(&gameFrontBotSetupData, 0, sizeof(gameFrontBotSetupData));
          gameFrontBotSetupData.count              = 1;
          gameFrontBotSetupData.playerTeamNumber   = 1;
          gameFrontBotSetupData.bots[0].teamNumber = 2;
          /* brainPath left empty -> the bot loop falls back to spBrainPath. */
        }
        if (strncmp(fileName, "randommap:", 10) == 0) {
          /* Random map — parse seed from "randommap:<seed>" */
          MapGenConfig cfg = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
          const char *seedStr = fileName + 10;
          if (!mapGenSeedToConfig(seedStr, &cfg)) {
              WB_LOG_WARN(WB_LOG_CAT_MAP, "failed to parse random map seed '%s', using defaults", seedStr);
          }
          cfg.x1 = MAP_MINE_EDGE_LEFT + 1; cfg.y1 = MAP_MINE_EDGE_TOP + 1;
          cfg.x2 = MAP_MINE_EDGE_RIGHT - 1; cfg.y2 = MAP_MINE_EDGE_BOTTOM - 1;
          spServerSim = serverSimCreateRandomMap(&cfg, spGameType, hiddenMines, startDelay, timeLen);
        } else if (strcmp(fileName, "") != 0) {
          spServerSim = serverSimCreate(fileName, spGameType, hiddenMines, startDelay, timeLen);
        } else {
          BYTE emap[6000] = E_MAP;
          spServerSim = serverSimCreateCompressed(emap, 5097, "Everard Island", spGameType, hiddenMines, startDelay, timeLen);
        }
        if (spServerSim != NULL) {
          /* Embedded server: silence its console messages (Thread Manager
           * Startup, Game started!, …) — the client has no server console. */
          serverSimSetQuiet(spServerSim, true);
          /* Tutorial: mark the freshly-created sim authoritative-tutorial and
             reset the respawn start to 0 (sea) BEFORE the host player is added
             in gameFrontStartServerSim below.  startsGetStart only takes the
             deterministic tutorial start when sim->isTutorial is already set;
             the old serverSimSetTutorial at openTutorial ran after the spawn,
             so the host was placed by the open-game algorithm instead. */
          if (isTutorial) {
            serverSimSetTutorial(spServerSim, true);
            serverSimSetTutorialStartIdx(spServerSim, 0);
          }
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
          cfg.botAiType    = (BYTE)spAiPolicy;

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
            /* Single-player rounds are recorded so the lobby recap can play
             * the round back. Two paths in the prefs dir, and they have to
             * stay distinct: singleplayer-recording.wbv is the live
             * recording, which every lobby entry truncates and reopens, while
             * singleplayer.wbv holds the last completed round. Three seconds
             * after game over the sim returns to the lobby and the log module
             * reopens the recording path — so folding these back into one
             * name means the lobby the round returns to destroys the round
             * itself. Both are explicit file paths rather than a directory,
             * so the path composer uses them verbatim instead of auto-naming
             * a fresh timestamped file per round. dontSendLog is
             * unconditionally true; a single-player round is never uploaded
             * to WinBolo.net. Tutorials are skipped — they set cfg.skipLobby,
             * so they never reach the lobby that would offer the playback.
             * Installed before the client-type resolution because the
             * subscriber's sync replay opens the log immediately, and the
             * completed path is set after the install, which clears it. */
            if (!isTutorial) {
              char spLogPath[FILENAME_MAX];
              char spRoundPath[FILENAME_MAX];
              const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
              if (prefDir != NULL) {
                snprintf(spLogPath, sizeof(spLogPath),
                         "%ssingleplayer-recording.wbv", prefDir);
                snprintf(spRoundPath, sizeof(spRoundPath),
                         "%ssingleplayer.wbv", prefDir);
                SDL_free((void *)prefDir);
              } else {
                snprintf(spLogPath, sizeof(spLogPath),
                         "singleplayer-recording.wbv");
                snprintf(spRoundPath, sizeof(spRoundPath), "singleplayer.wbv");
              }
              serverSimSetWantLogging(spServerSim, true);
              serverSimSetUserLogFileName(spServerSim, spLogPath);
              serverDedicatedLogInstall(spServerSim, true);
              serverDedicatedLogSetCompletedPath(spRoundPath);
            }

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
            if (spAiPolicy != aiNone && gameFrontBotSetupData.count > 0 && haveBrain) {
              for (int bi = 0; bi < gameFrontBotSetupData.count && bi < MAX_BOT_SLOTS; bi++) {
                BYTE slot = (BYTE)(bi + 1);
                char botName[32];
                snprintf(botName, sizeof(botName), "Bot %d", slot);
                /* Use per-bot brain path if set; otherwise pick the default by
                 * a single-player skill guess (WinBolo.net signed-in with more
                 * than 5 games → the harder 1.5, else the gentler 1.0). */
                const char *botBrain = gameFrontBotSetupData.bots[bi].brainPath;
                if (botBrain[0] == '\0') {
                  botBrain = spBrainPath;  /* fallback if the guess isn't in the catalogue */
                  const char *guess = gameFrontGuessSpBotBrain();
                  const BrainList *gbl = serverSimGetBrainList(spServerSim);
                  if (gbl) {
                    for (int k = 0; k < gbl->count; k++) {
                      if (SDL_strcasecmp(gbl->entries[k].name, guess) == 0) {
                        const char *gp = serverSimGetBrainPathForIdx(spServerSim, (uint8_t)k);
                        if (gp) botBrain = gp;
                        break;
                      }
                    }
                  }
                }
                serverSimCreateBot(spServerSim, slot, botBrain, botName, spAiPolicy, spGameType, hiddenMines);
                /* serverSimCreateBot loads the brain from the path but leaves
                 * the lobby brain-INDEX at the 0xFF "default" sentinel, so the
                 * lobby Bot Code dropdown renders "(none)". Resolve the index
                 * from the path (case-insensitive exact match, else the
                 * version-suffixed dir name as a substring) so the dropdown
                 * shows the actual brain — GoalHunter_1.6 by default. */
                const BrainList *spbl = serverSimGetBrainList(spServerSim);
                if (spbl) {
                  for (int k = 0; k < spbl->count; k++) {
                    const char *kp = serverSimGetBrainPathForIdx(spServerSim, (uint8_t)k);
                    if ((kp && SDL_strcasecmp(kp, botBrain) == 0) ||
                        strstr(botBrain, spbl->entries[k].name) != NULL) {
                      serverSimSetBotBrainIdxFor(spServerSim, slot, (uint8_t)k);
                      break;
                    }
                  }
                }
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
  prefsSetString("SETTINGS", "Show Tutorial Button",
                            TRUEFALSE_TO_STR(show));
}

/* Client-hosting write-through setters — update the global and persist the
 * [HOSTING] key immediately so both settings shells save without relying on
 * the pre-game modal's close-time flush. */
void gameFrontSetHostingPort(unsigned short port) {
  gameFrontHostingPort = port;
  char buf[16];
  intToStr(port, buf, sizeof(buf));
  prefsSetString("HOSTING", "Port", buf);
}

void gameFrontSetHostingAllowSpec(bool allow) {
  gameFrontHostingAllowSpec = allow;
  prefsSetString("HOSTING", "Allow Spectators", TRUEFALSE_TO_STR(allow));
}

void gameFrontSetHostingMaxSpec(int maxSpec) {
  gameFrontHostingMaxSpec = maxSpec;
  char buf[16];
  intToStr(maxSpec, buf, sizeof(buf));
  prefsSetString("HOSTING", "Max Spectators", buf);
}

void gameFrontSetHostingUploadPolicy(int policy) {
  gameFrontHostingUploadPolicy = policy;
  const char *str = (policy == UPLOAD_POLICY_OFF)     ? "Off"
                  : (policy == UPLOAD_POLICY_PERSIST) ? "Persist"
                                                      : "Allow";
  prefsSetString("HOSTING", "Upload Policy", str);
}

void gameFrontSetHostingUploadMaxFiles(int maxFiles) {
  gameFrontHostingUploadMaxFiles = maxFiles;
  char buf[16];
  intToStr(maxFiles, buf, sizeof(buf));
  prefsSetString("HOSTING", "Upload Max Files", buf);
}

void gameFrontSetHostingUploadMaxStorage(int maxStorageMb) {
  gameFrontHostingUploadMaxStorage = maxStorageMb;
  char buf[16];
  intToStr(maxStorageMb, buf, sizeof(buf));
  prefsSetString("HOSTING", "Upload Max Storage", buf);
}

void gameFrontSetHostingUploadDir(const char *dir) {
  SDL_strlcpy(gameFrontHostingUploadDir, dir ? dir : "",
              sizeof(gameFrontHostingUploadDir));
  prefsSetString("HOSTING", "Upload Dir", gameFrontHostingUploadDir);
}

void gameFrontSetHostingLogging(bool logging) {
  gameFrontHostingLogging = logging;
  prefsSetString("HOSTING", "Logging", TRUEFALSE_TO_STR(logging));
}

void gameFrontSetHostingLogDir(const char *dir) {
  SDL_strlcpy(gameFrontHostingLogDir, dir ? dir : "",
              sizeof(gameFrontHostingLogDir));
  prefsSetString("HOSTING", "Log Dir", gameFrontHostingLogDir);
}

void gameFrontSetHostingServeReplays(bool serve) {
  gameFrontHostingServeReplays = serve;
  prefsSetString("HOSTING", "Serve Replays", TRUEFALSE_TO_STR(serve));
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
  prefsSetString("SETTINGS", "Language",
                            gameFrontLanguageCode);
}

void gameFrontRequestPlayTutorial(void) {
  gameFrontPlayTutorialRequested = TRUE;
}

bool gameFrontConsumePlayTutorialRequest(void) {
  bool was = gameFrontPlayTutorialRequested;
  gameFrontPlayTutorialRequested = FALSE;
  return was;
}

void gameFrontRequestUdpAutoJoin(void) {
  gameFrontUdpAutoJoinRequested = TRUE;
}

bool gameFrontConsumeUdpAutoJoinRequest(void) {
  bool was = gameFrontUdpAutoJoinRequested;
  gameFrontUdpAutoJoinRequested = FALSE;
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
  prefsSetString("WINBOLO.NET", "Token", gameFrontWbnToken);
  prefsSetString("WINBOLO.NET", "TokenExpiry", gameFrontWbnTokenExpiry);
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
  gameFrontWbnRank = -1;
  gameFrontWbnRankTotal = 0;
  gameFrontWbnStats.valid = FALSE;
  gameFrontSaveWbnTokenToPrefs();
  gameFrontSetWbnAuthMethod("");
}

void gameFrontSetWbnAuthMethod(const char *method) {
  prefsSetString("WINBOLO.NET", "AuthMethod", method ? method : "");
}

void gameFrontGetWbnAuthMethod(char *out, size_t outSize) {
  prefsGetString("WINBOLO.NET", "AuthMethod", "", out, outSize);
}

void gameFrontSetWbnSignedOut(bool signedOut) {
  prefsSetString("WINBOLO.NET", "SignedOut", signedOut ? "Yes" : "No");
}

bool gameFrontGetWbnSignedOut(void) {
  char buf[8];
  prefsGetString("WINBOLO.NET", "SignedOut", "No", buf, sizeof(buf));
  return strcmp(buf, "Yes") == 0;
}

bool gameFrontGetWinbolonetUse(void) {
  return gameFrontWbnUse;
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

bool gameFrontIsSupporter(void) {
  /* Mirrors the self-flag resolution used at join time. Currently this is
   * Steam DLC ownership only (the WBN-account supporter flag is not yet
   * plumbed to the client). */
  return bolo_steam_has_supporter_dlc();
}

/* -------------------------------------------------------
 * Cloud preferences sync (download-on-login).
 *
 * Signing in launches exactly one sync per session off a worker
 * thread. The document is only ever mutated on the main thread in
 * gameFrontPumpPrefsSync, which joins the worker and applies the
 * outcome. The worker reads everything it needs through a snapshot
 * captured on the main thread, so it never touches prefs.c globals.
 * ------------------------------------------------------- */
typedef struct {
  /* inputs (captured on the main thread) */
  char userToken[FILENAME_MAX];
  char *uploadSnapshot;            /* malloc'd; freed in the pump */
  char deviceType[65];
  bool localDirty;
  char lastSynced[33];
  char displayName[PLAYER_NAME_LEN]; /* account name; reasserted after adopt */
  /* output */
  WbnSyncOutcome outcome;
  SDL_AtomicInt done;
} PrefsSyncWork;

static SDL_Thread *s_prefsSyncThread = NULL;
static PrefsSyncWork s_prefsSyncWork;
static bool s_prefsSyncedThisSession = false;

static int gameFrontPrefsSyncThreadFunc(void *data) {
  PrefsSyncWork *w = (PrefsSyncWork *)data;
  w->outcome = wbnPrefsSyncOnce(w->userToken, w->uploadSnapshot,
                                w->deviceType,
                                w->localDirty, w->lastSynced);
  SDL_SetAtomicInt(&w->done, 1);
  return 0;
}

/* Capture the sync inputs on the main thread and start the worker.
 * Returns true when the worker was launched, false when there was nothing
 * to do (not signed in, serialize failed, or thread create failed). Shared
 * by the login trigger and the debounce-flush upload trigger; the
 * once-per-session gate and the in-flight check belong to the callers, not
 * here. */
static bool gameFrontLaunchPrefsSyncWorker(void) {
  char token[FILENAME_MAX], expiry[FILENAME_MAX];
  gameFrontGetWinbolonetToken(token, expiry);
  if (token[0] == '\0') {
    WB_LOG_DEBUG(WB_LOG_CAT_NET,
                 "wbn_prefs: launch skipped — no WBN token (not signed in)");
    return false; /* not signed in: nothing to sync */
  }

  memset(&s_prefsSyncWork, 0, sizeof(s_prefsSyncWork));
  SDL_SetAtomicInt(&s_prefsSyncWork.done, 0);
  SDL_strlcpy(s_prefsSyncWork.userToken, token, sizeof(s_prefsSyncWork.userToken));
  s_prefsSyncWork.uploadSnapshot = prefsSerializeForUpload();
  SDL_strlcpy(s_prefsSyncWork.deviceType,
              bolo_client_type_name(bolo_detect_client_type()),
              sizeof(s_prefsSyncWork.deviceType));
  s_prefsSyncWork.localDirty = prefsSyncDirty();
  prefsGetLastSyncedUpdatedAt(s_prefsSyncWork.lastSynced,
                              sizeof(s_prefsSyncWork.lastSynced));
  gameFrontGetPlayerName(s_prefsSyncWork.displayName);

  if (s_prefsSyncWork.uploadSnapshot == NULL) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "wbn_prefs: launch skipped — serialize for upload failed");
    return false; /* serialize failed (OOM / pre-init): retry later */
  }

  s_prefsSyncThread = SDL_CreateThread(gameFrontPrefsSyncThreadFunc,
                                       "WBNPrefsSync", &s_prefsSyncWork);
  if (s_prefsSyncThread == NULL) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "wbn_prefs: launch failed — SDL_CreateThread returned NULL");
    free(s_prefsSyncWork.uploadSnapshot);
    s_prefsSyncWork.uploadSnapshot = NULL;
    return false;
  }
  WB_LOG_DEBUG(WB_LOG_CAT_NET,
               "wbn_prefs: sync worker launched (localDirty=%d, snapshot=%zu bytes)",
               s_prefsSyncWork.localDirty ? 1 : 0,
               strlen(s_prefsSyncWork.uploadSnapshot));
  return true;
}

void gameFrontStartPrefsSync(void) {
  if (s_prefsSyncThread != NULL || s_prefsSyncedThisSession) {
    WB_LOG_DEBUG(WB_LOG_CAT_NET,
                 "wbn_prefs: login sync skipped — %s",
                 s_prefsSyncThread != NULL ? "worker already in flight"
                                           : "already synced this session");
    return;
  }
  WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_prefs: login sync requested");
  if (gameFrontLaunchPrefsSyncWorker()) {
    s_prefsSyncedThisSession = true;
  }
}

/* Debounce-flush upload trigger. Reuses the single shared worker
 * (wbnPrefsSyncOnce GETs, reconciles, then PUTs — correct here because the
 * local doc is dirty) and the gameFrontPumpPrefsSync completion handler,
 * which clears sync-dirty on a PUSHED outcome. Unlike the login trigger it
 * does not consult the once-per-session gate. A 429 returns the existing
 * NOOP outcome and leaves sync-dirty set, so the next debounce flush
 * (>=15 s later, already under the server's ~1/sec write limit) retries; we
 * deliberately do not parse Retry-After. */
static void gameFrontMaybeUploadPrefs(void) {
  if (s_prefsSyncThread != NULL) {
    WB_LOG_DEBUG(WB_LOG_CAT_NET,
                 "wbn_prefs: debounce upload skipped — worker already in flight");
    return; /* a sync/upload worker is already in flight */
  }
  if (!gameFrontGetWinbolonetUse() || !prefsSyncDirty()) {
    WB_LOG_DEBUG(WB_LOG_CAT_NET,
                 "wbn_prefs: debounce upload skipped — use=%d dirty=%d",
                 gameFrontGetWinbolonetUse() ? 1 : 0, prefsSyncDirty() ? 1 : 0);
    return; /* not signed in, or nothing to push */
  }
  WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_prefs: debounce flush -> uploading prefs");
  gameFrontLaunchPrefsSyncWorker();
}

void gameFrontPumpPrefsSync(void) {
  if (s_prefsSyncThread == NULL || !SDL_GetAtomicInt(&s_prefsSyncWork.done)) {
    return;
  }

  SDL_WaitThread(s_prefsSyncThread, NULL);
  s_prefsSyncThread = NULL;

  WbnSyncOutcome *o = &s_prefsSyncWork.outcome;
  WB_LOG_DEBUG(WB_LOG_CAT_NET,
               "wbn_prefs: worker joined; outcome kind=%d token=%s",
               (int)o->kind, o->token[0] ? o->token : "(none)");
  switch (o->kind) {
    case WBN_SYNC_OUT_ADOPTED:
      if (o->serverPrefs != NULL &&
          prefsAdoptServerDocument(o->serverPrefs) == PREFS_ADOPT_OK) {
        prefsMarkSynced(o->token);
        /* Record which platform last wrote the cloud doc (device-local, so
         * this write does not re-dirty the just-synced document). */
        prefsSetString("DEVICE", "LastSavedFromType", o->serverDeviceType);
        /* Live-apply the downloaded settings: re-read into the live globals
         * and push the keys/tank options onto the running client without a
         * restart (mirrors the Key Setup confirm path). */
        keyItems liveKeys;
        windowGetKeys(&liveKeys);
        gameFrontGetPrefs(&liveKeys, &useAutoslow, &useAutohide);
        windowSetKeys(&liveKeys);
        if (humanSim != NULL) {
          clientSimSetTankAutoSlowdown(humanSim, useAutoslow);
          clientSimSetTankAutoHideGunsight(humanSim, useAutohide);
        }
        /* The account display_name wins over the synced Player Name. */
        if (s_prefsSyncWork.displayName[0] != '\0') {
          prefsSetString("SETTINGS", "Player Name", s_prefsSyncWork.displayName);
          gameFrontSetPlayerName(s_prefsSyncWork.displayName);
        }
      }
      break;
    case WBN_SYNC_OUT_PUSHED:
      prefsMarkSynced(o->token);
      break;
    case WBN_SYNC_OUT_REAUTH:
      /* Token is stale server-side: sign out and let a later sign-in resync. */
      gameFrontClearWinbolonetToken();
      gameFrontResetPrefsSyncSession();
      break;
    case WBN_SYNC_OUT_NOOP:
    default:
      break;
  }

  if (o->serverPrefs != NULL) {
    free(o->serverPrefs);
    o->serverPrefs = NULL;
  }
  if (s_prefsSyncWork.uploadSnapshot != NULL) {
    free(s_prefsSyncWork.uploadSnapshot);
    s_prefsSyncWork.uploadSnapshot = NULL;
  }
}

void gameFrontResetPrefsSyncSession(void) {
  s_prefsSyncedThisSession = false;
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
  sdl3DrawSetReconfigureGuard(true);
  sdl3DrawReloadTiles();
  sdl3DrawSetReconfigureGuard(false);
  soundCleanup();
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

  /* Finalize the current round's log and upload it before serverInstanceShutdown
   * tears WinBolo.net down — otherwise a host that plays a round and then leaves
   * or quits never uploads that final round (only round transitions flush).
   * Stash first (closes the file; no-op when not logging and skips the upload
   * for hosts that opted out); only end the session + upload when something is
   * actually pending and WBN is up, so non-logging and Local hosts are
   * untouched. End the session so the server accepts the upload against the
   * still-valid key; the shorter timeout keeps an unreachable server from
   * stalling the leave. serverInstanceShutdown's own quit follows harmlessly. */
  serverDedicatedLogStashCurrentRound();
  if (serverDedicatedLogHasPendingUpload() && winbolonetIsRunning()) {
    winbolonetEndSession();
    httpSetLogUploadTimeout(10);
    serverDedicatedLogFlushPendingUpload();
    httpSetLogUploadTimeout(0);
  }

  /* And let go of the round, after the stash and upload above have had it.
   * Unconditional, because the server being torn down may never have installed
   * the writer: hosting installs only when the host has logging on, and a host
   * that has it off would otherwise leave the previous server's round in place
   * — a single-player game played earlier in this process — to be served to
   * whoever joins and named in the host's recap as the last round. */
  serverDedicatedLogUninstall();

  serverInstanceShutdown(toFree);
  serverSimDestroy(toFree);
}

bool gameFrontHasLocalServer(void) {
  return spServerSimActive;
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

bool gameFrontOnboardingComplete(void) {
  char buff[FILENAME_MAX];
  prefsGetString("SETTINGS", "Onboarding Complete", "No", buff, FILENAME_MAX);
  return YESNO_TO_TRUEFALSE(buff[0]);
}

void gameFrontSetOnboardingComplete(void) {
  prefsSetString("SETTINGS", "Onboarding Complete", "Yes");
}

/* The bot brain the player last explicitly chose from the lobby wrench
 * dropdown. This is the player's own difficulty preference and overrides the
 * automatic single-player skill guess from then on. Empty until first chosen
 * (the default on a fresh install). */
void gameFrontSetChosenBotBrain(const char *name) {
  prefsSetString("BOT", "Chosen Brain", name ? name : "");
}

void gameFrontGetChosenBotBrain(char *out, size_t outLen) {
  if (!out || outLen == 0) return;
  prefsGetString("BOT", "Chosen Brain", "", out, (int)outLen);
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
  /* Hosting port/spectators/uploads come from the [HOSTING] prefs, re-read
   * here at host time. gameFrontMyUdp is not trustworthy as the hosting port:
   * the browser Join path zeroes it (gameFrontSetUdpOptions(..., 0)). */
  cfg.udpPort             = gameFrontHostingPort;
  cfg.maxSpectators       = gameFrontHostingAllowSpec
                              ? (BYTE)(gameFrontHostingMaxSpec < 1 ? 1
                                       : gameFrontHostingMaxSpec > 32 ? 32
                                       : gameFrontHostingMaxSpec)
                              : 0;
  cfg.specDelaySeconds    = 0;  /* client hosts run live */
  cfg.uploadPolicy        = (UploadPolicy)gameFrontHostingUploadPolicy;
  cfg.uploadMaxFiles      = (uint8_t)gameFrontHostingUploadMaxFiles;
  cfg.uploadMaxStorageBytes =
      (uint32_t)gameFrontHostingUploadMaxStorage * 1024u * 1024u;
  /* Persist saves uploads to disk under the chosen directory. Create it on
   * use and refuse to host if that fails — no silent fallback. Off/Allow
   * never touch disk, so leave uploadPersistDir NULL (memset-zero) for them. */
  if (gameFrontHostingUploadPolicy == UPLOAD_POLICY_PERSIST) {
    if (!SDL_CreateDirectory(gameFrontHostingUploadDir)) {
      WB_LOG_WARN(WB_LOG_CAT_NET,
                  "cannot create upload directory '%s' — refusing to host",
                  gameFrontHostingUploadDir);
      serverSimDestroy(spServerSim);
      spServerSim = NULL;
      return FALSE;
    }
    cfg.uploadPersistDir  = gameFrontHostingUploadDir;
  }
  /* Round logging writes .wbv files into the chosen directory. Create it on
   * use and refuse to host if that fails — no silent fallback. Done here,
   * before the server starts, so the failure unwind is the simple pre-start
   * destroy; the log subscriber itself is installed after startup below. The
   * directory must exist before then because the log-path composer only
   * treats its argument as a directory if it already exists on disk. */
  if (gameFrontHostingLogging) {
    if (!SDL_CreateDirectory(gameFrontHostingLogDir)) {
      WB_LOG_WARN(WB_LOG_CAT_NET,
                  "cannot create log directory '%s' — refusing to host",
                  gameFrontHostingLogDir);
      serverSimDestroy(spServerSim);
      spServerSim = NULL;
      return FALSE;
    }
  }
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
  /* Internet host: always register with the tracker (no user toggle). The
   * tracker is what makes the game discoverable and resolves the host's
   * external address for NAT traversal and Steam "Join Game". Address/port
   * come from the INI ([TRACKER] Address/Port, default tracker.winbolo.com:
   * 50000) — there is no in-app UI for it. The LAN-only block below forces
   * this back off for Local games; the single-player/passive path disables
   * it separately. */
  cfg.useTracker          = TRUE;
  cfg.trackerAddr         = gameFrontTrackerAddr;
  cfg.trackerPort         = gameFrontTrackerPort;
  cfg.useNatKeepalive     = gameFrontUseNatTraversal;
  cfg.useNatPortmap       = gameFrontUseUpnp;
  /* Advertise on the LAN via mDNS. Left on for Local games too — LAN
   * discovery is exactly what those want — so the LAN-only block below
   * does not clear it. */
  cfg.mdnsAdvertise       = true;
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
  /* Install the round-log writer against the now-running server. Its
   * sync-replay opens the log immediately using the directory validated
   * above. Local games pass dontSendLog = true so the module writes the log
   * but skips the WinBolo.net upload; Internet games upload it. */
  if (gameFrontHostingLogging) {
    serverSimSetWantLogging(spServerSim, true);
    serverSimSetUserLogFileName(spServerSim, gameFrontHostingLogDir);
    serverDedicatedLogInstall(spServerSim, s_isLanOnly);
    /* Whether a joined player can pull the finished round's log back for
     * the recap. The install above resets the mode, so this runs after it.
     * The host's Yes means AUTO, not ON: AUTO still declines to serve while
     * WinBolo.net is running, because a WBN round's log is uploaded there
     * instead. No is the hard off the host asked for. */
    if (!gameFrontHostingServeReplays) {
      serverDedicatedLogSetServeMode(ROUND_LOG_SERVE_OFF);
    }
  }
  return TRUE;
}

/* -------------------------------------------------------
 * gameFrontGetPrefs — read preferences from INI file
 * ------------------------------------------------------- */
bool gameFrontGetPrefs(keyItems *keys, bool *pUseAutoslow, bool *pUseAutohide) {
  char buff[FILENAME_MAX];
  char def[FILENAME_MAX];

  /* Steam Deck detection: prefer the SteamDeck=1 hint/env Steam sets,
     falling back to /etc/os-release for launch paths (Desktop-mode,
     non-Steam) where the env var isn't propagated. Used below to flip
     touch/controller-oriented defaults ON (autoscroll, autoslow,
     autohide) for first-launch UX on the Deck. Gunsight defaults ON
     everywhere. Saved values still override. */
  bool isSteamDeck = uiModeIsSteamDeckHardware();
  const char *gunsightDefault   = "Yes";
#if defined(__IPHONEOS__) || defined(__ANDROID__)
  const char *autoScrollDefault = "Yes";
  const char *autoSlowDefault   = "Yes";
  const char *autoHideDefault   = "Yes";
#else
  const char *autoScrollDefault = isSteamDeck ? "Yes" : "No";
  const char *autoSlowDefault   = isSteamDeck ? "Yes" : "No";
  const char *autoHideDefault   = isSteamDeck ? "Yes" : "No";
#endif

  /* Player Name */
  strcpy(def, langGetText(STR_DLGGAMESETUP_DEFAULTNAME));
  prefsGetString("SETTINGS", "Player Name", def, gameFrontName, sizeof(gameFrontName));

  /* Target Address */
  def[0] = '\0';
  prefsGetString("SETTINGS", "Target Address", def, gameFrontUdpAddress, FILENAME_MAX);

  /* Target UDP Port */
  intToStr(DEFAULT_UDP_PORT, def, sizeof(def));
  prefsGetString("SETTINGS", "Target UDP Port", def, buff, FILENAME_MAX);
  gameFrontMyUdp = atoi(buff);
  gameFrontTargetUdp = atoi(buff);
  /* My UDP Port */
  intToStr(DEFAULT_UDP_PORT, def, sizeof(def));
  prefsGetString("SETTINGS", "UDP Port", def, buff, FILENAME_MAX);
  gameFrontMyUdp = atoi(buff);

  /* Hosting settings ([HOSTING] section — the port/spectator/upload knobs
   * for a game hosted from the finder). Clamp on read to the same ranges the
   * UI enforces so a hand-edited INI can't inject an out-of-range value. */
  intToStr(DEFAULT_UDP_PORT, def, sizeof(def));
  prefsGetString("HOSTING", "Port", def, buff, FILENAME_MAX);
  {
    int p = atoi(buff);
    if (p < 1024) p = 1024;
    if (p > 65535) p = 65535;
    gameFrontHostingPort = (unsigned short)p;
  }
  prefsGetString("HOSTING", "Allow Spectators", "Yes", buff, FILENAME_MAX);
  gameFrontHostingAllowSpec = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("HOSTING", "Max Spectators", "16", buff, FILENAME_MAX);
  {
    int m = atoi(buff);
    if (m < 1) m = 1;
    if (m > 32) m = 32;
    gameFrontHostingMaxSpec = m;
  }
  prefsGetString("HOSTING", "Upload Policy", "Allow", buff, FILENAME_MAX);
  if (strcmp(buff, "Off") == 0) {
    gameFrontHostingUploadPolicy = UPLOAD_POLICY_OFF;
  } else if (strcmp(buff, "Persist") == 0) {
    gameFrontHostingUploadPolicy = UPLOAD_POLICY_PERSIST;
  } else {
    gameFrontHostingUploadPolicy = UPLOAD_POLICY_ALLOW;
  }
  prefsGetString("HOSTING", "Upload Max Files", "64", buff, FILENAME_MAX);
  {
    int f = atoi(buff);
    if (f < 1) f = 1;
    if (f > 255) f = 255;
    gameFrontHostingUploadMaxFiles = f;
  }
  prefsGetString("HOSTING", "Upload Max Storage", "8", buff, FILENAME_MAX);
  {
    int st = atoi(buff);
    if (st < 1) st = 1;
    if (st > 4095) st = 4095;
    gameFrontHostingUploadMaxStorage = st;
  }
  /* Upload Dir default lives under the writable prefs path — the app's
   * default maps dir is inside the read-only bundle. SDL_GetPrefPath
   * returns a trailing separator, so append "uploads" directly. */
  {
    const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir) {
      snprintf(def, FILENAME_MAX, "%suploads", prefDir);
      SDL_free((void *)prefDir);
    } else {
      snprintf(def, FILENAME_MAX, "%s", "uploads");
    }
    prefsGetString("HOSTING", "Upload Dir", def, gameFrontHostingUploadDir,
                   FILENAME_MAX);
  }
  prefsGetString("HOSTING", "Logging", "Yes", buff, FILENAME_MAX);
  gameFrontHostingLogging = YESNO_TO_TRUEFALSE(buff[0]);
  /* Log Dir default is the writable prefs path itself — same place as
   * WinBolo.json. SDL_GetPrefPath returns a trailing separator, so pass it
   * as-is for the directory. */
  {
    const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir) {
      snprintf(def, FILENAME_MAX, "%s", prefDir);
      SDL_free((void *)prefDir);
    } else {
      snprintf(def, FILENAME_MAX, "%s", ".");
    }
    prefsGetString("HOSTING", "Log Dir", def, gameFrontHostingLogDir,
                   FILENAME_MAX);
  }
  prefsGetString("HOSTING", "Serve Replays", "Yes", buff, FILENAME_MAX);
  gameFrontHostingServeReplays = YESNO_TO_TRUEFALSE(buff[0]);

  /* Driving keys */
  intToStr(DEFAULT_FORWARD, def, sizeof(def));
  prefsGetString("KEYS", "Forward", def, buff, FILENAME_MAX);
  keys->kiForward = atoi(buff);
  intToStr(DEFAULT_BACKWARD, def, sizeof(def));
  prefsGetString("KEYS", "Backwards", def, buff, FILENAME_MAX);
  keys->kiBackward = atoi(buff);
  intToStr(DEFAULT_LEFT, def, sizeof(def));
  prefsGetString("KEYS", "Left", def, buff, FILENAME_MAX);
  keys->kiLeft = atoi(buff);
  intToStr(DEFAULT_RIGHT, def, sizeof(def));
  prefsGetString("KEYS", "Right", def, buff, FILENAME_MAX);
  keys->kiRight = atoi(buff);

  /* Shooting, mines, gunsights */
  intToStr(DEFAULT_SHOOT, def, sizeof(def));
  prefsGetString("KEYS", "Shoot", def, buff, FILENAME_MAX);
  keys->kiShoot = atoi(buff);
  intToStr(DEFAULT_LAY_MINE, def, sizeof(def));
  prefsGetString("KEYS", "Lay Mine", def, buff, FILENAME_MAX);
  keys->kiLayMine = atoi(buff);
  intToStr(DEFAULT_SCROLL_GUNINCREASE, def, sizeof(def));
  prefsGetString("KEYS", "Increase Range", def, buff, FILENAME_MAX);
  keys->kiGunIncrease = atoi(buff);
  intToStr(DEFAULT_SCROLL_GUNDECREASE, def, sizeof(def));
  prefsGetString("KEYS", "Decrease Range", def, buff, FILENAME_MAX);
  keys->kiGunDecrease = atoi(buff);

  /* Views */
  intToStr(DEFAULT_TANKVIEW, def, sizeof(def));
  prefsGetString("KEYS", "Tank View", def, buff, FILENAME_MAX);
  keys->kiTankView = atoi(buff);
  intToStr(DEFAULT_PILLVIEW, def, sizeof(def));
  prefsGetString("KEYS", "Pill View", def, buff, FILENAME_MAX);
  keys->kiPillView = atoi(buff);
  intToStr(DEFAULT_ALLYVIEW, def, sizeof(def));
  prefsGetString("KEYS", "Ally View", def, buff, FILENAME_MAX);
  keys->kiAllyView = atoi(buff);
  intToStr(DEFAULT_LGMVIEW, def, sizeof(def));
  prefsGetString("KEYS", "LGM View", def, buff, FILENAME_MAX);
  keys->kiLGMView = atoi(buff);
  intToStr(DEFAULT_BASEVIEW, def, sizeof(def));
  prefsGetString("KEYS", "Base View", def, buff, FILENAME_MAX);
  keys->kiBaseView = atoi(buff);

  /* Scrolling */
  intToStr(DEFAULT_SCROLLUP, def, sizeof(def));
  prefsGetString("KEYS", "Scroll Up", def, buff, FILENAME_MAX);
  keys->kiScrollUp = atoi(buff);
  intToStr(DEFAULT_SCROLLDOWN, def, sizeof(def));
  prefsGetString("KEYS", "Scroll Down", def, buff, FILENAME_MAX);
  keys->kiScrollDown = atoi(buff);
  intToStr(DEFAULT_SCROLLLEFT, def, sizeof(def));
  prefsGetString("KEYS", "Scroll Left", def, buff, FILENAME_MAX);
  keys->kiScrollLeft = atoi(buff);
  intToStr(DEFAULT_SCROLLRIGHT, def, sizeof(def));
  prefsGetString("KEYS", "Scroll Right", def, buff, FILENAME_MAX);
  keys->kiScrollRight = atoi(buff);

  /* Quick keys */
  intToStr(DEFAULT_QUICKTREE, def, sizeof(def));
  prefsGetString("KEYS", "Quick Tree", def, buff, FILENAME_MAX);
  keys->kiQuickTree = atoi(buff);
  intToStr(DEFAULT_QUICKROAD, def, sizeof(def));
  prefsGetString("KEYS", "Quick Road", def, buff, FILENAME_MAX);
  keys->kiQuickRoad = atoi(buff);
  intToStr(DEFAULT_QUICKWALL, def, sizeof(def));
  prefsGetString("KEYS", "Quick Wall", def, buff, FILENAME_MAX);
  keys->kiQuickWall = atoi(buff);
  intToStr(DEFAULT_QUICKPILLBOX, def, sizeof(def));
  prefsGetString("KEYS", "Quick Pillbox", def, buff, FILENAME_MAX);
  keys->kiQuickPillbox = atoi(buff);
  intToStr(DEFAULT_QUICKMINE, def, sizeof(def));
  prefsGetString("KEYS", "Quick Mine", def, buff, FILENAME_MAX);
  keys->kiQuickMine = atoi(buff);

  /* Gamepad — right-stick scroll sensitivity multiplier (0.25..4.0). */
  prefsGetString("SETTINGS", "Gamepad Scroll Sens", "1.00", buff, FILENAME_MAX);
  {
    float gs = (float)atof(buff);
    if (!(gs >= 0.25f && gs <= 4.0f)) gs = 1.0f;
    g_gamepadScrollSensitivity = gs;
  }

  /* Gamepad — left-stick tank turn sensitivity (0.10..1.0; 1.0 = snap). */
  prefsGetString("SETTINGS", "Gamepad Tank Sens", "1.00", buff, FILENAME_MAX);
  {
    float ts = (float)atof(buff);
    if (!(ts >= 0.10f && ts <= 1.0f)) ts = 1.0f;
    g_gamepadTankSensitivity = ts;
  }

  /* Gamepad — build-cursor move sensitivity (0.25..2.0, lower = finer). */
  prefsGetString("SETTINGS", "Gamepad Build Cursor Sens", "1.00", buff, FILENAME_MAX);
  {
    float bs = (float)atof(buff);
    if (!(bs >= 0.25f && bs <= 2.0f)) bs = 1.0f;
    g_gamepadBuildCursorSensitivity = bs;
  }

  /* Gamepad — build-cursor behaviour options. */
  prefsGetString("SETTINGS", "Build Exit Executes", "No", buff, FILENAME_MAX);
  g_buildExitExecutes = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("SETTINGS", "Build Exit Executes Momentary Only", "No", buff, FILENAME_MAX);
  g_buildExitExecutesMomentaryOnly = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("SETTINGS", "Build Double Tap Road", "Yes", buff, FILENAME_MAX);
  g_buildDoubleTapRoad = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("SETTINGS", "Build Hold Momentary", "Yes", buff, FILENAME_MAX);
  g_buildHoldMomentary = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("SETTINGS", "Build Auto Close On Execute", "No", buff, FILENAME_MAX);
  g_buildAutoCloseOnExecute = YESNO_TO_TRUEFALSE(buff[0]);

  /* UI scale override (0 Auto / 1 Small / 2 Medium / 3 Large).  Auto keeps
     the display-derived scale; a preset pins the ImGui scale.  Desktop-only;
     Deck/tablet/mobile ignore it. */
  prefsGetString("SETTINGS", "UI Scale", "0", buff, FILENAME_MAX);
  {
    int v = atoi(buff);
    if (v < 0 || v > 3) v = 0;
    uiUiScaleSet((UiScalePref)v);
  }

  /* Gamepad — Path B rebindable action table.  Start from defaults so
     missing prefs keys leave each action at its historical mapping;
     present keys overlay on top.  inputGamepadInit may run after this
     prefs load, so we seed via SetAll which marks the table as
     initialised and prevents init from re-resetting it.

     Two slots per action persist as gpb_<name>_pri_{kind,code} and
     gpb_<name>_sec_{kind,code}.  Older prefs files used a single
     gpb_<name>_{kind,code} pair — when the new keys are absent but
     the old ones exist, the old values migrate into the primary slot
     and the secondary is forced to NONE (matching the single-binding
     behaviour the legacy file expressed).  Players who want the
     historical FIRE=RT-or-A behaviour can re-bind the secondary or
     remove [GAMEPAD] entirely to fall back to defaults. */
  {
    GamepadBindings gb;
    inputGamepadBindingsResetDefaults(&gb);
    /* Sentinel that survives any plausible legacy code value. */
    static const char kAbsentSentinel[] = "__absent__";
    for (int i = 0; i < GP_ACT_COUNT; ++i) {
      const char *name = inputGamepadActionName((GamepadAction)i);
      char keyOldKind[64], keyOldCode[64];
      char keyPriKind[64], keyPriCode[64];
      char keySecKind[64], keySecCode[64];
      snprintf(keyOldKind, sizeof(keyOldKind), "gpb_%s_kind",     name);
      snprintf(keyOldCode, sizeof(keyOldCode), "gpb_%s_code",     name);
      snprintf(keyPriKind, sizeof(keyPriKind), "gpb_%s_pri_kind", name);
      snprintf(keyPriCode, sizeof(keyPriCode), "gpb_%s_pri_code", name);
      snprintf(keySecKind, sizeof(keySecKind), "gpb_%s_sec_kind", name);
      snprintf(keySecCode, sizeof(keySecCode), "gpb_%s_sec_code", name);

      char rPriKind[FILENAME_MAX], rPriCode[FILENAME_MAX];
      char rSecKind[FILENAME_MAX], rSecCode[FILENAME_MAX];
      prefsGetString("GAMEPAD", keyPriKind, kAbsentSentinel, rPriKind, FILENAME_MAX);
      prefsGetString("GAMEPAD", keyPriCode, kAbsentSentinel, rPriCode, FILENAME_MAX);
      prefsGetString("GAMEPAD", keySecKind, kAbsentSentinel, rSecKind, FILENAME_MAX);
      prefsGetString("GAMEPAD", keySecCode, kAbsentSentinel, rSecCode, FILENAME_MAX);

      bool havePri = (strcmp(rPriKind, kAbsentSentinel) != 0 &&
                      strcmp(rPriCode, kAbsentSentinel) != 0);
      bool haveSec = (strcmp(rSecKind, kAbsentSentinel) != 0 &&
                      strcmp(rSecCode, kAbsentSentinel) != 0);

      if (havePri || haveSec) {
        /* New-format file: any present slot wins; any missing slot
           clears to NONE.  Mixing with legacy keys is impossible — the
           writer below only emits new keys, and a hand-edited file
           that mixes the two is treated as new-format authoritative. */
        gb.b[i].pri.kind = GP_BIND_NONE;
        gb.b[i].pri.code = 0;
        gb.b[i].sec.kind = GP_BIND_NONE;
        gb.b[i].sec.code = 0;
        if (havePri) {
          int k = atoi(rPriKind);
          int c = atoi(rPriCode);
          if (k < 0 || k > GP_BIND_TRIGGER) k = GP_BIND_NONE;
          gb.b[i].pri.kind = (GamepadBindKind)k;
          gb.b[i].pri.code = c;
        }
        if (haveSec) {
          int k = atoi(rSecKind);
          int c = atoi(rSecCode);
          if (k < 0 || k > GP_BIND_TRIGGER) k = GP_BIND_NONE;
          gb.b[i].sec.kind = (GamepadBindKind)k;
          gb.b[i].sec.code = c;
        }
      } else {
        /* No new keys — try legacy single-suffix migration. */
        char rOldKind[FILENAME_MAX], rOldCode[FILENAME_MAX];
        prefsGetString("GAMEPAD", keyOldKind, kAbsentSentinel, rOldKind, FILENAME_MAX);
        prefsGetString("GAMEPAD", keyOldCode, kAbsentSentinel, rOldCode, FILENAME_MAX);
        if (strcmp(rOldKind, kAbsentSentinel) != 0 &&
            strcmp(rOldCode, kAbsentSentinel) != 0) {
          int k = atoi(rOldKind);
          int c = atoi(rOldCode);
          if (k < 0 || k > GP_BIND_TRIGGER) k = (int)gb.b[i].pri.kind;
          gb.b[i].pri.kind = (GamepadBindKind)k;
          gb.b[i].pri.code = c;
          /* Legacy file had no concept of a secondary slot. */
          gb.b[i].sec.kind = GP_BIND_NONE;
          gb.b[i].sec.code = 0;
        }
        /* else: neither new nor old keys present; defaults stand. */
      }
    }
    inputGamepadBindingsSetAll(&gb);
  }

  /* Remember */
  prefsGetString("SETTINGS", "Remember Player Name", "Yes", buff, FILENAME_MAX);
  gameFrontRemeber = YESNO_TO_TRUEFALSE(buff[0]);

  /* Tutorial visibility — defaults to "Yes" (show on first run). */
  prefsGetString("SETTINGS", "Show Tutorial Button", "Yes", buff, FILENAME_MAX);
  gameFrontShowTutorialButton = YESNO_TO_TRUEFALSE(buff[0]);

  /* Language code (BCP-47, e.g. "en", "de", "pt-br"). Empty string on
   * fresh install — startup walks SDL_GetPreferredLocales() in that
   * case (see gameFrontStart). */
  prefsGetString("SETTINGS", "Language", "",
                          gameFrontLanguageCode,
                          (DWORD)sizeof(gameFrontLanguageCode));

  /* Game Options */
  prefsGetString("GAME OPTIONS", "Hidden Mines", "No", buff, FILENAME_MAX);
  hiddenMines = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("GAME OPTIONS", "Allow Computer Tanks", "0", buff, FILENAME_MAX);
  compTanks = atoi(buff);
  prefsGetString("GAME OPTIONS", "Game Type", "1", buff, FILENAME_MAX);
  gametype = atoi(buff);
  prefsGetString("GAME OPTIONS", "Start Delay", "0", buff, FILENAME_MAX);
  startDelay = atoi(buff);
  longToStr(UNLIMITED_GAME_TIME, def, sizeof(def));
  prefsGetString("GAME OPTIONS", "Time Length", def, buff, FILENAME_MAX);
  timeLen = (int32_t)atol(buff);
  prefsGetString("GAME OPTIONS", "Auto Slowdown", autoSlowDefault, buff, FILENAME_MAX);
  *pUseAutoslow = YESNO_TO_TRUEFALSE(buff[0]);
  /* A connected controller forces auto-slowdown on, regardless of the
     saved pref — analog-stick steering with no slowdown is unmanageable.
     This overrides at apply time only; the stored pref is left untouched,
     so it takes over again once the controller is disconnected. */
  if (inputGamepadIsConnected()) {
    *pUseAutoslow = TRUE;
  }
  prefsGetString("GAME OPTIONS", "Auto Show-Hide Gunsight", autoHideDefault, buff, FILENAME_MAX);
  *pUseAutohide = YESNO_TO_TRUEFALSE(buff[0]);

  prefsGetString("SETTINGS", "Use UPnP", "Yes", buff, FILENAME_MAX);
  gameFrontUseUpnp = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("SETTINGS", "Use NAT Traversal", "Yes", buff, FILENAME_MAX);
  gameFrontUseNatTraversal = YESNO_TO_TRUEFALSE(buff[0]);

  /* Tracker options */
  prefsGetString("TRACKER", "Address", TRACKER_ADDRESS, gameFrontTrackerAddr, FILENAME_MAX);
  intToStr(TRACKER_PORT, def, sizeof(def));
  prefsGetString("TRACKER", "Port", def, buff, FILENAME_MAX);
  gameFrontTrackerPort = atoi(buff);
  prefsGetString("TRACKER", "Enabled", "No", buff, FILENAME_MAX);
  gameFrontTrackerEnabled = YESNO_TO_TRUEFALSE(buff[0]);

  /* Menu Items */
  intToStr(FRAME_RATE_30, def, sizeof(def));
  prefsGetString("MENU", "Frame Rate", def, buff, FILENAME_MAX);
  frameRate = atoi(buff);
  /* Default ON for first-time players (no prefs file yet). Existing users
   * keep whatever "Show Gunsight" they already saved in their INI. */
  prefsGetString("MENU", "Show Gunsight", gunsightDefault, buff, FILENAME_MAX);
  showGunsight = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Sound Effects", "Yes", buff, FILENAME_MAX);
  soundEffects = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Allow Background Sound", "Yes", buff, FILENAME_MAX);
  backgroundSound = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Sound keepalive", "No", buff, FILENAME_MAX);
  useSoundKeepalive = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Sound Volume", "50", buff, FILENAME_MAX);
  soundVolume = atoi(buff);
  if (soundVolume < 0) soundVolume = 0;
  if (soundVolume > 100) soundVolume = 100;
  prefsGetString("MENU", "Show Newswire Messages", "Yes", buff, FILENAME_MAX);
  showNewswireMessages = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Show Assistant Messages", "Yes", buff, FILENAME_MAX);
  showAssistantMessages = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Show AI Messages", "Yes", buff, FILENAME_MAX);
  showAIMessages = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Show Network Status Messages", "Yes", buff, FILENAME_MAX);
  showNetworkStatusMessages = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Show Network Debug Messages", "No", buff, FILENAME_MAX);
  showNetworkDebugMessages = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Autoscroll Enabled", autoScrollDefault, buff, FILENAME_MAX);
  autoScrollingEnabled = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Smooth Scrolling", "Yes", buff, FILENAME_MAX);
  smoothScrollingEnabled = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Letterbox Bars Gray", "No", buff, FILENAME_MAX);
  letterboxBarsGray = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Show Pill Labels", "No", buff, FILENAME_MAX);
  showPillLabels = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Show Base Labels", "No", buff, FILENAME_MAX);
  showBaseLabels = YESNO_TO_TRUEFALSE(buff[0]);
  prefsGetString("MENU", "Label Own Tank", "No", buff, FILENAME_MAX);
  labelSelf = YESNO_TO_TRUEFALSE(buff[0]);
#if defined(__IPHONEOS__) || defined(__ANDROID__) || defined(__EMSCRIPTEN__)
  prefsGetString("WINDOW", "Window Size", "1", buff, FILENAME_MAX);
#else
  /* First-time desktop players open at 2x so the game isn't a tiny window on
   * modern monitors. If the monitor can't fit 2x, the window setup in
   * winbolo.c clamps down to the largest cardinal size that does fit. Anyone
   * who has finished onboarding keeps whatever size they previously saved. */
  prefsGetString("WINDOW", "Window Size",
                 gameFrontOnboardingComplete() ? "1" : "2", buff, FILENAME_MAX);
#endif
  zoomFactor = atoi(buff);
  /* Custom window size (for ZOOM_FACTOR_CUSTOM mode) */
  prefsGetString("WINDOW", "Custom Width", "0", buff, FILENAME_MAX);
  {
    int customW = atoi(buff);
    prefsGetString("WINDOW", "Custom Height", "0", buff, FILENAME_MAX);
    int customH = atoi(buff);
    if (customW > 0 && customH > 0) {
      windowSetCustomSize(customW, customH);
    }
  }
  /* Window position */
  prefsGetString("WINDOW", "Window X", "-1", buff, FILENAME_MAX);
  {
    int winX = atoi(buff);
    prefsGetString("WINDOW", "Window Y", "-1", buff, FILENAME_MAX);
    int winY = atoi(buff);
    windowSetSavedPosition(winX, winY);
  }
  /* Dialog window position (welcome screen, etc.) */
  prefsGetString("WINDOW", "Dialog X", "-1", buff, FILENAME_MAX);
  gameFrontDialogX = atoi(buff);
  prefsGetString("WINDOW", "Dialog Y", "-1", buff, FILENAME_MAX);
  gameFrontDialogY = atoi(buff);

  prefsGetString("MENU", "Message Label Size", "1", buff, FILENAME_MAX);
  labelMsg = atoi(buff);
  prefsGetString("MENU", "Tank Label Size", "1", buff, FILENAME_MAX);
  labelTank = atoi(buff);

  /* Winbolo.net */
  prefsGetString("WINBOLO.NET", "Token", "", gameFrontWbnToken, FILENAME_MAX);
  prefsGetString("WINBOLO.NET", "TokenExpiry", "", gameFrontWbnTokenExpiry, FILENAME_MAX);
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

  /* Player Name */
  if (((humanSim != NULL && clientSimGetNetType(humanSim) == netSingle) || (gameFrontRemeber == TRUE && humanSim != NULL)) && dlgState != openSetup && !clientSimIsInLobby(humanSim)) {
    clientSimGetPlayerName(humanSim, playerName, sizeof(playerName));
    strcpy(gameFrontName, playerName);
    prefsSetString("SETTINGS", "Player Name", playerName);
  } else {
    prefsSetString("SETTINGS", "Player Name", gameFrontName);
  }

  /* Target Address */
  prefsSetString("SETTINGS", "Target Address", gameFrontUdpAddress);

  /* Ports */
  intToStr(gameFrontTargetUdp, buff, sizeof(buff));
  prefsSetString("SETTINGS", "Target UDP Port", buff);
  intToStr(gameFrontMyUdp, buff, sizeof(buff));
  prefsSetString("SETTINGS", "UDP Port", buff);

  /* Hosting settings ([HOSTING] section). Also written through immediately by
   * the per-setting setters; mirrored here for parity with the close-time
   * flush of the other sections. */
  intToStr(gameFrontHostingPort, buff, sizeof(buff));
  prefsSetString("HOSTING", "Port", buff);
  prefsSetString("HOSTING", "Allow Spectators",
                            TRUEFALSE_TO_STR(gameFrontHostingAllowSpec));
  intToStr(gameFrontHostingMaxSpec, buff, sizeof(buff));
  prefsSetString("HOSTING", "Max Spectators", buff);
  prefsSetString("HOSTING", "Upload Policy",
                 gameFrontHostingUploadPolicy == UPLOAD_POLICY_OFF     ? "Off"
                 : gameFrontHostingUploadPolicy == UPLOAD_POLICY_PERSIST ? "Persist"
                                                                         : "Allow");
  intToStr(gameFrontHostingUploadMaxFiles, buff, sizeof(buff));
  prefsSetString("HOSTING", "Upload Max Files", buff);
  intToStr(gameFrontHostingUploadMaxStorage, buff, sizeof(buff));
  prefsSetString("HOSTING", "Upload Max Storage", buff);
  prefsSetString("HOSTING", "Upload Dir", gameFrontHostingUploadDir);
  prefsSetString("HOSTING", "Logging",
                            TRUEFALSE_TO_STR(gameFrontHostingLogging));
  prefsSetString("HOSTING", "Log Dir", gameFrontHostingLogDir);
  prefsSetString("HOSTING", "Serve Replays",
                            TRUEFALSE_TO_STR(gameFrontHostingServeReplays));

  /* Language — persist the BCP-47 code, not a file path. */
  prefsSetString("SETTINGS", "Language",
                            gameFrontLanguageCode);

  /* Keys — driving */
  intToStr(keys->kiForward, buff, sizeof(buff));
  prefsSetString("KEYS", "Forward", buff);
  intToStr(keys->kiBackward, buff, sizeof(buff));
  prefsSetString("KEYS", "Backwards", buff);
  intToStr(keys->kiLeft, buff, sizeof(buff));
  prefsSetString("KEYS", "Left", buff);
  intToStr(keys->kiRight, buff, sizeof(buff));
  prefsSetString("KEYS", "Right", buff);

  /* Shooting, mines, gunsight */
  intToStr(keys->kiShoot, buff, sizeof(buff));
  prefsSetString("KEYS", "Shoot", buff);
  intToStr(keys->kiLayMine, buff, sizeof(buff));
  prefsSetString("KEYS", "Lay Mine", buff);
  intToStr(keys->kiGunIncrease, buff, sizeof(buff));
  prefsSetString("KEYS", "Increase Range", buff);
  intToStr(keys->kiGunDecrease, buff, sizeof(buff));
  prefsSetString("KEYS", "Decrease Range", buff);

  /* Views */
  intToStr(keys->kiTankView, buff, sizeof(buff));
  prefsSetString("KEYS", "Tank View", buff);
  intToStr(keys->kiPillView, buff, sizeof(buff));
  prefsSetString("KEYS", "Pill View", buff);
  intToStr(keys->kiAllyView, buff, sizeof(buff));
  prefsSetString("KEYS", "Ally View", buff);
  intToStr(keys->kiLGMView, buff, sizeof(buff));
  prefsSetString("KEYS", "LGM View", buff);
  intToStr(keys->kiBaseView, buff, sizeof(buff));
  prefsSetString("KEYS", "Base View", buff);

  /* Scrolling */
  intToStr(keys->kiScrollUp, buff, sizeof(buff));
  prefsSetString("KEYS", "Scroll Up", buff);
  intToStr(keys->kiScrollDown, buff, sizeof(buff));
  prefsSetString("KEYS", "Scroll Down", buff);
  intToStr(keys->kiScrollLeft, buff, sizeof(buff));
  prefsSetString("KEYS", "Scroll Left", buff);
  intToStr(keys->kiScrollRight, buff, sizeof(buff));
  prefsSetString("KEYS", "Scroll Right", buff);

  /* Quick keys */
  intToStr(keys->kiQuickTree, buff, sizeof(buff));
  prefsSetString("KEYS", "Quick Tree", buff);
  intToStr(keys->kiQuickRoad, buff, sizeof(buff));
  prefsSetString("KEYS", "Quick Road", buff);
  intToStr(keys->kiQuickWall, buff, sizeof(buff));
  prefsSetString("KEYS", "Quick Wall", buff);
  intToStr(keys->kiQuickPillbox, buff, sizeof(buff));
  prefsSetString("KEYS", "Quick Pillbox", buff);
  intToStr(keys->kiQuickMine, buff, sizeof(buff));
  prefsSetString("KEYS", "Quick Mine", buff);

  /* Gamepad — right-stick scroll sensitivity multiplier. */
  snprintf(buff, sizeof(buff), "%.2f", g_gamepadScrollSensitivity);
  prefsSetString("SETTINGS", "Gamepad Scroll Sens", buff);

  /* Gamepad — left-stick tank-move + build-cursor move sensitivities. */
  snprintf(buff, sizeof(buff), "%.2f", g_gamepadTankSensitivity);
  prefsSetString("SETTINGS", "Gamepad Tank Sens", buff);
  snprintf(buff, sizeof(buff), "%.2f", g_gamepadBuildCursorSensitivity);
  prefsSetString("SETTINGS", "Gamepad Build Cursor Sens", buff);

  /* Gamepad — build-cursor behaviour options. */
  prefsSetString("SETTINGS", "Build Exit Executes",   TRUEFALSE_TO_STR(g_buildExitExecutes));
  prefsSetString("SETTINGS", "Build Exit Executes Momentary Only", TRUEFALSE_TO_STR(g_buildExitExecutesMomentaryOnly));
  prefsSetString("SETTINGS", "Build Double Tap Road", TRUEFALSE_TO_STR(g_buildDoubleTapRoad));
  prefsSetString("SETTINGS", "Build Hold Momentary",  TRUEFALSE_TO_STR(g_buildHoldMomentary));
  prefsSetString("SETTINGS", "Build Auto Close On Execute", TRUEFALSE_TO_STR(g_buildAutoCloseOnExecute));

  /* UI scale override (0 Auto / 1 Small / 2 Medium / 3 Large). */
  intToStr((int)uiUiScaleGet(), buff, sizeof(buff));
  prefsSetString("SETTINGS", "UI Scale", buff);

  /* Gamepad — Path B rebindable action table.  Four keys per action:
     gpb_<name>_pri_{kind,code} and gpb_<name>_sec_{kind,code} where
     kind is 0=NONE / 1=BUTTON / 2=TRIGGER and code is the integer SDL
     enum value.  The loader applies defaults when no key for an action
     is present so removing the [GAMEPAD] section reverts to historical
     behaviour.  Legacy single-suffix keys (gpb_<name>_{kind,code}) are
     migrated on load and ignored thereafter; we don't bother deleting
     them from the file. */
  {
    GamepadBindings gb;
    inputGamepadBindingsGetAll(&gb);
    for (int i = 0; i < GP_ACT_COUNT; ++i) {
      char keyPriKind[64], keyPriCode[64];
      char keySecKind[64], keySecCode[64];
      const char *name = inputGamepadActionName((GamepadAction)i);
      snprintf(keyPriKind, sizeof(keyPriKind), "gpb_%s_pri_kind", name);
      snprintf(keyPriCode, sizeof(keyPriCode), "gpb_%s_pri_code", name);
      snprintf(keySecKind, sizeof(keySecKind), "gpb_%s_sec_kind", name);
      snprintf(keySecCode, sizeof(keySecCode), "gpb_%s_sec_code", name);
      intToStr((int)gb.b[i].pri.kind, buff, sizeof(buff));
      prefsSetString("GAMEPAD", keyPriKind, buff);
      intToStr(gb.b[i].pri.code, buff, sizeof(buff));
      prefsSetString("GAMEPAD", keyPriCode, buff);
      intToStr((int)gb.b[i].sec.kind, buff, sizeof(buff));
      prefsSetString("GAMEPAD", keySecKind, buff);
      intToStr(gb.b[i].sec.code, buff, sizeof(buff));
      prefsSetString("GAMEPAD", keySecCode, buff);
    }
  }

  /* Remember */
  prefsSetString("SETTINGS", "Remember Player Name", TRUEFALSE_TO_STR(gameFrontRemeber));

  /* Options */
  prefsSetString("GAME OPTIONS", "Hidden Mines", TRUEFALSE_TO_STR(hiddenMines));
  intToStr(compTanks, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "Allow Computer Tanks", buff);
  intToStr(gametype, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "Game Type", buff);
  intToStr(startDelay, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "Start Delay", buff);
  intToStr(timeLen, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "Time Length", buff);
  prefsSetString("GAME OPTIONS", "Auto Slowdown", TRUEFALSE_TO_STR(useAutoslow));
  prefsSetString("GAME OPTIONS", "Auto Show-Hide Gunsight", TRUEFALSE_TO_STR(useAutohide));

  prefsSetString("SETTINGS", "Use UPnP", TRUEFALSE_TO_STR(gameFrontUseUpnp));
  prefsSetString("SETTINGS", "Use NAT Traversal", TRUEFALSE_TO_STR(gameFrontUseNatTraversal));

  /* Tracker */
  prefsSetString("TRACKER", "Address", gameFrontTrackerAddr);
  intToStr(gameFrontTrackerPort, buff, sizeof(buff));
  prefsSetString("TRACKER", "Port", buff);
  prefsSetString("TRACKER", "Enabled", TRUEFALSE_TO_STR(gameFrontTrackerEnabled));

  /* Menu Items */
  intToStr(frameRate, buff, sizeof(buff));
  prefsSetString("MENU", "Frame Rate", buff);
  prefsSetString("MENU", "Show Gunsight", TRUEFALSE_TO_STR(showGunsight));
  prefsSetString("MENU", "Sound Effects", TRUEFALSE_TO_STR(soundEffects));
  prefsSetString("MENU", "Allow Background Sound", TRUEFALSE_TO_STR(backgroundSound));
  prefsSetString("MENU", "Sound keepalive", TRUEFALSE_TO_STR(useSoundKeepalive));
  intToStr(soundVolume, buff, sizeof(buff));
  prefsSetString("MENU", "Sound Volume", buff);
  prefsSetString("MENU", "Show Newswire Messages", TRUEFALSE_TO_STR(showNewswireMessages));
  prefsSetString("MENU", "Show Assistant Messages", TRUEFALSE_TO_STR(showAssistantMessages));
  prefsSetString("MENU", "Show AI Messages", TRUEFALSE_TO_STR(showAIMessages));
  prefsSetString("MENU", "Show Network Status Messages", TRUEFALSE_TO_STR(showNetworkStatusMessages));
  prefsSetString("MENU", "Show Network Debug Messages", TRUEFALSE_TO_STR(showNetworkDebugMessages));
  prefsSetString("MENU", "Autoscroll Enabled", TRUEFALSE_TO_STR(autoScrollingEnabled));
  prefsSetString("MENU", "Smooth Scrolling", TRUEFALSE_TO_STR(smoothScrollingEnabled));
  prefsSetString("MENU", "Letterbox Bars Gray", TRUEFALSE_TO_STR(letterboxBarsGray));
  prefsSetString("MENU", "Show Pill Labels", TRUEFALSE_TO_STR(showPillLabels));
  prefsSetString("MENU", "Show Base Labels", TRUEFALSE_TO_STR(showBaseLabels));
  prefsSetString("MENU", "Label Own Tank", TRUEFALSE_TO_STR(labelSelf));
  /* Window settings (zoom, custom size, position, dialog position) — flush immediately,
     bypassing debounce since this is the shutdown save path. */
  gameFrontFlushWindowSettings();
  intToStr(labelMsg, buff, sizeof(buff));
  prefsSetString("MENU", "Message Label Size", buff);
  intToStr(labelTank, buff, sizeof(buff));
  prefsSetString("MENU", "Tank Label Size", buff);

  /* Winbolo.net */
  prefsSetString("WINBOLO.NET", "Token", gameFrontWbnToken);
  prefsSetString("WINBOLO.NET", "TokenExpiry", gameFrontWbnTokenExpiry);
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

  /* Big Picture / Gamepad UI runs maximised and transient — don't let that
     overwrite the user's saved desktop window size/position. */
  if (steam_is_big_picture()) {
    s_windowSettingsDirty = false;
    return;
  }

  intToStr(zoomFactor, buff, sizeof(buff));
  prefsSetString("WINDOW", "Window Size", buff);

  {
    int customW, customH;
    windowGetCustomSize(&customW, &customH);
    intToStr(customW, buff, sizeof(buff));
    prefsSetString("WINDOW", "Custom Width", buff);
    intToStr(customH, buff, sizeof(buff));
    prefsSetString("WINDOW", "Custom Height", buff);
  }

  {
    int winX, winY;
    windowGetSavedPosition(&winX, &winY);
    intToStr(winX, buff, sizeof(buff));
    prefsSetString("WINDOW", "Window X", buff);
    intToStr(winY, buff, sizeof(buff));
    prefsSetString("WINDOW", "Window Y", buff);
  }

  intToStr(gameFrontDialogX, buff, sizeof(buff));
  prefsSetString("WINDOW", "Dialog X", buff);
  intToStr(gameFrontDialogY, buff, sizeof(buff));
  prefsSetString("WINDOW", "Dialog Y", buff);

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
  /* Upload to the cloud only when the debounce actually flushed (and only
   * if there is something dirty to sync); the 15 s debounce doubles as the
   * back-off that keeps writes under the server's ~1/sec limit. */
  if (prefsPumpAutosave((uint64_t)SDL_GetTicks())) gameFrontMaybeUploadPrefs();
  gameFrontPumpPrefsSync();
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

