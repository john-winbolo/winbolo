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
#include "brain_list.h"   /* BrainModes, brainListLoadModesForPath — bot modes */
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
#include "../../scenario/scenario_host.h"
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
#include "skin_source.h"
#include "gfx_settings.h"

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
bool logViewerAppQuitRequested(void);
bool spectatorRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  void *cs, const char *serverHost, uint16_t serverPort);

#include "dialogs/imgui_wbn_browser.h"
#include "dialogs/imgui_onboarding.h"
#include "dialogs/imgui_lobby.h"

#if defined(WINBOLO_VOICE)
#include "../voice.h"

/* Voice settings live in the voice module; these apply them with the same
   clamping the settings dialog uses, and read them back for the save.
   Declared here rather than in winbolo.h so the voice build option does not
   leak into the shared frontend header — the mobile targets compile this
   file with no voice sources at all. */
void  windowSetVoiceEnabled(bool on);
bool  windowGetVoiceEnabled(void);
void  windowSetVoiceMode(int mode);
int   windowGetVoiceMode(void);
void  windowSetVoiceMicGain(float gain);
float windowGetVoiceMicGain(void);
void  windowSetVoiceVolume(float gain);
float windowGetVoiceVolume(void);
void  windowSetVoiceRecordingDevice(const char *name);
const char *windowGetVoiceRecordingDevice(void);
void  windowSetVoicePlaybackDevice(const char *name);
const char *windowGetVoicePlaybackDevice(void);
void  windowSetShowTankMicIcons(bool on);
bool  windowGetShowTankMicIcons(void);
#if defined(WINBOLO_VOICE_AEC)
void  windowSetVoiceEchoCancel(bool on);
bool  windowGetVoiceEchoCancel(void);
#endif

/* Mode is stored as a name, not a number, so a hand-edited prefs file reads
   as something. An unrecognised name falls back to the default. */
#define VOICE_MODE_NAME_OFF  "Off"
#define VOICE_MODE_NAME_PTT  "Push To Talk"
#define VOICE_MODE_NAME_OPEN "Open Mic"
#endif

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
        "Brains/GoalHunter_1.7/init.lua",
        "brains/GoalHunter_1.7/init.lua",
        "data/Brains/GoalHunter_1.7/init.lua",
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

/* Single-player default-bot skill guess. Returns the DIFFICULTY an
 * auto-seeded SP bot gets: Hard only when the player is signed in to
 * WinBolo.net with more than 5 games on record, otherwise Easy (what
 * everyone not signed in gets). One brain plays every difficulty, so this
 * used to pick between brain directories (1.7 vs 1.0) and now picks the
 * per-bot difficulty instead — the same rule, a different knob. */
uint8_t gameFrontSpBotDifficulty(void) {
    /* Gospel: if the player has ever explicitly picked a difficulty from the
     * lobby wrench dropdown, honour that from then on, skill guess ignored. */
    uint8_t chosen;
    if (gameFrontGetChosenBotDifficulty(&chosen)) return chosen;

    WbnStats st;
    gameFrontGetWinbolonetStats(&st);
    if (st.valid) {
        int games = 0;
        const WbnModeStats *modes[3] = { &st.open, &st.tourn, &st.strict };
        for (int i = 0; i < 3; i++) {
            if (modes[i]->numGames > 0) games += modes[i]->numGames;
        }
        if (games > 5) return BOT_DIFFICULTY_HARD;
    }
    return BOT_DIFFICULTY_EASY;
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
bool           gameFrontHostingScripts         = TRUE;
int            gameFrontHostingMaxSpec         = 16;
int            gameFrontHostingUploadPolicy    = UPLOAD_POLICY_ALLOW;
int            gameFrontHostingUploadMaxFiles  = 64;
int            gameFrontHostingUploadMaxStorage = 8;
/* Persist upload dir. Empty until gameFrontGetPrefs seeds the default
 * (<prefs path>uploads) or the user picks one. */
char           gameFrontHostingUploadDir[FILENAME_MAX] = "";
int            gameFrontHostingScriptUploadPolicy     = SCRIPT_UPLOAD_ALLOW;
bool           gameFrontHostingShareScripts           = TRUE;
int            gameFrontHostingScriptUploadMaxFiles   = 32;
int            gameFrontHostingScriptUploadMaxStorage = 64;
/* Persist script dir. Empty until gameFrontGetPrefs seeds the default
 * (<prefs path>uploads/Scripts) or the user picks one. */
char           gameFrontHostingScriptUploadDir[FILENAME_MAX] = "";
bool           gameFrontHostingLogging         = TRUE;
/* Round-log dir. Empty until gameFrontGetPrefs seeds the default
 * (the prefs path) or the user picks one. */
char           gameFrontHostingLogDir[FILENAME_MAX] = "";
/* The scenarios this host offers on their own, independently of any map.
 * Empty until gameFrontGetPrefs seeds the default (<prefs path>scenarios). */
char           gameFrontHostingScenarioDir[FILENAME_MAX] = "";
bool           gameFrontHostingServeReplays   = TRUE;
/* How the hosted server handles the voice its clients send it. Holds a
 * ServerVoiceMode; serverVoiceOn is what a client host did before this
 * setting existed. */
int            gameFrontHostingVoiceMode      = serverVoiceOn;

/* Visibility rules a hosted game starts with ([GAME OPTIONS] section).
 * The stock set from view_policy.h, the same one serverSimInit writes —
 * a pillbox shows only while it is watched, bases and allied tanks not
 * at all — so hosting with an untouched INI leaves the sim exactly as
 * it was created. */
int gameFrontViewPillPolicy    = VIEW_POLICY_STOCK_PILL;
int gameFrontViewBasePolicy    = VIEW_POLICY_STOCK_BASE;
int gameFrontViewAllyPolicy    = VIEW_POLICY_STOCK_ALLY;
int gameFrontViewPillDecaySecs = VIEW_DECAY_DEFAULT_SECS;
int gameFrontViewBaseDecaySecs = VIEW_DECAY_DEFAULT_SECS;
int gameFrontViewAllyDecaySecs = VIEW_DECAY_DEFAULT_SECS;
bool gameFrontClassicMode      = FALSE;
bool gameFrontAlliesInTrees    = FALSE;
/* Which block of squares the map overview keeps live, and what stops the
 * player seeing inside it. Ints rather than bools because each holds a
 * named value — OverviewWindow and LineOfSightMode — the way the three
 * policies above hold a ViewPolicy. */
int gameFrontOverviewWindow    = OVERVIEW_WINDOW_STOCK;
int gameFrontLineOfSight       = LINE_OF_SIGHT_STOCK;

/* Tutorial: shown on the welcome menu until the player completes it.
 * Defaults to TRUE on a fresh install (key absent from INI). The player
 * can toggle it back on from the Settings dialog at any time. */
static bool gameFrontShowTutorialButton = TRUE;

/* Persisted BCP-47 language code (e.g. "en", "de", "pt-br"). Empty
 * string means the user has not picked one yet — Phase 5 startup runs
 * langAutoDetect() in that case. */
static char gameFrontLanguageCode[32] = "";

/* Persisted skin id ("user:foo", "builtin:bar"). Read by
 * gameFrontGetPrefs and applied by gameFrontSetup before the first tile
 * sheet is built. Empty string means the built-in assets. */
static char gameFrontSkinId[SKIN_ID_MAX] = "";

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

/* Lobby window size and the lobby's players/map column split. The lobby
 * runs in the shared dialog window, so its position rides on
 * gameFrontDialogX/Y above and only the size needs its own keys; -1 means
 * "never saved", so the lobby opens at its built-in default.
 *
 * The split offsets are stored in logical (UI-scale-independent) pixels —
 * the lobby multiplies them by the scale it computes for the current
 * window, so a scale change moves the divider with the rest of the
 * layout instead of stranding it. There are two: the post-game view wants
 * the right column wide for the replay, the map view wants it back for the
 * teams table, and a player who shared one would re-drag the divider after
 * every round. */
int gameFrontLobbyW = -1;
int gameFrontLobbyH = -1;
float gameFrontLobbySplit = 0.0f;
float gameFrontLobbySplitRecap = 0.0f;

/* Map overview pop-out geometry, camera state and last-open flag. The
   defaults are the size the window was created at before it had preferences,
   2x zoom with follow on (what overviewCameraInit picks), no saved position
   and closed. */
int   gameFrontOverviewW = 640;
int   gameFrontOverviewH = 640;
int   gameFrontOverviewX = -1;
int   gameFrontOverviewY = -1;
int   gameFrontScnPanelX = -1;
int   gameFrontScnPanelY = -1;
int   gameFrontScnPanelScale = -1;
int   gameFrontScnPanelAlpha = -1;
float gameFrontOverviewZoom = 2.0f;
bool  gameFrontOverviewFollow = TRUE;
bool  gameFrontShowMapOverview = FALSE;

/* App full screen mode, and with it the in-window Full Screen Map view. */
bool  gameFrontFullScreen = FALSE;

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

/* Ask the player for the password of the game being joined and store it in
 * the password global that clientSimConnectUdp sends. wrongBefore adds the
 * incorrect-password line above the request, for a retry after the server
 * rejected the previous entry. Returns FALSE when the player cancelled, in
 * which case the global is left as it was. Runs before any in-game frame
 * loop exists, so it uses the blocking prompt beside imguiMessageBoxEx
 * rather than an in-frame modal. */
static bool gameFrontAskJoinPassword(bool wrongBefore) {
  char msg[512];
  char entry[MAP_STR_SIZE];
  if (wrongBefore) {
    SDL_snprintf(msg, sizeof(msg), "%s\n\n%s",
                 langGetText(NETERR_PASSWORDWRONG),
                 langGetText(STR_DLGPASSWORD_BLURB));
  } else {
    SDL_strlcpy(msg, langGetText(STR_DLGPASSWORD_BLURB), sizeof(msg));
  }
  entry[0] = '\0';
  if (imguiPasswordPrompt(langGetText(STR_DLGPASSWORD_TITLE), msg,
                          entry, sizeof(entry)) != IMGUI_MSG_RESULT_OK) {
    return FALSE;
  }
  SDL_strlcpy(password, entry, sizeof(password));
  return TRUE;
}

/* Server-authoritative single-player state */
static ServerSim *spServerSim = NULL;
/* The scenario attached to spServerSim, if the map it was built from has
 * a script beside it. NULL whenever there is no host to speak of, which
 * scenarioHostDetach treats as nothing to do. */
static ScenarioHost *spScenarioHost = NULL;
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
extern int  windowMasterVolume;
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

/* The two volumes go in through their setters rather than into the globals
   above, so the load reaches everything a slider move would.  Declared here
   for the same reason the voice ones are: they live in the per-platform
   frontend, not in winbolo.h. */
void windowSetSoundVolume(int pct);
void windowSetMasterVolume(int pct);

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

  /* Make the saved skin current before anything builds a tile sheet or
   * loads the background. An id that no longer resolves leaves the
   * built-in assets active. */
  if (!skinSetActive(gameFrontSkinId) && gameFrontSkinId[0] != '\0') {
    WB_LOG_DEBUG(WB_LOG_CAT_ASSET,
                 "gameFrontStart: skin '%s' did not resolve — using the "
                 "built-in assets; the choice is kept and applies once the "
                 "files are there", gameFrontSkinId);
  }

  /* Push the saved texture filter to the draw layer. There is no sheet
     yet; the mode is kept and applied when one is built. */
  sdl3DrawSetTilesScaleMode(sdl3DrawScaleModeForFilter(gfxGetTextureFilter()));

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

#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
    /* Full screen covers the menus and the lobby too, so the window goes
       full screen here rather than waiting for a game. The preferences are
       already read and the window is created hidden, so the first dialog is
       the first thing drawn and there is no windowed flash. Big Picture,
       tablet and the Deck are full screen from creation and are left alone. */
    if (OKStart && gameFrontFullScreen && !uiModeIsTablet() &&
        !uiModeIsSteamDeck() && !steam_is_big_picture()) {
      SDL_SetWindowFullscreen(sdl3DrawGetWindow(), true);
    }
#endif

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

  clientSimSetAiType(humanSim, isTutorial ? aiNone : compTanks);

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

/* Where scripts players upload under Allow land for the session:
 * <prefs path>uploads/Session, beside the Upload Dir default and for the same
 * reason — the prefs path is writable and the app's own maps directory is
 * inside the read-only bundle. SDL_GetPrefPath returns a trailing separator.
 * Both paths that start a server set it. */
static void gameFrontScriptSessionDir(char *out, size_t outLen) {
  const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
  if (prefDir) {
    snprintf(out, outLen, "%suploads/Session", prefDir);
    SDL_free((void *)prefDir);
  } else {
    snprintf(out, outLen, "%s", "uploads/Session");
  }
}

/* The visibility rules a game this machine hosts starts on, taken from the
 * [GAME OPTIONS] prefs and pushed onto the sim after create. They do not
 * travel in ServerInstanceConfig, which has no field for them.
 *
 * Every path here that creates a server calls this: the listen server in
 * gameFrontSetupServer and the single-player game in the dialog state
 * machine below. Both open a lobby with the Visibility dropdown in it, and
 * both have to open it on whatever this player last chose. The
 * single-player path had no push of its own, so it always opened on the
 * values serverSimCreate left, whatever the prefs said and whatever the
 * player had picked in the lobby the game before.
 *
 * Call it before serverInstanceStartup. That is where
 * serverSimApplyInstanceConfig takes the snapshot
 * serverSimResetLobbyToDefaults restores from, so a push made after it
 * would leave the snapshot holding the created values and hand an emptied
 * lobby back to them. */
static void gameFrontApplyVisibilityPrefs(ServerSim *sim) {
  if (sim == NULL) return;
  serverSimSetViewPolicy(sim, viewCategoryPill,
                         (ViewPolicy)gameFrontViewPillPolicy,
                         (uint16_t)gameFrontViewPillDecaySecs);
  serverSimSetViewPolicy(sim, viewCategoryBase,
                         (ViewPolicy)gameFrontViewBasePolicy,
                         (uint16_t)gameFrontViewBaseDecaySecs);
  serverSimSetViewPolicy(sim, viewCategoryAlly,
                         (ViewPolicy)gameFrontViewAllyPolicy,
                         (uint16_t)gameFrontViewAllyDecaySecs);
  /* After the three policies, so classic mode wins over them when both
   * are set, and allies in trees before classic mode, which forces it
   * back off. Both only pushed when on — off is what the sim was
   * created with. */
  if (gameFrontAlliesInTrees) {
    serverSimSetAlliesInTrees(sim, true);
  }
  /* These two go on whatever they hold, not only when on: either value
   * is a real choice, and the expanded window is not what the sim was
   * created with. Still before classic mode, which writes both. */
  serverSimSetOverviewWindow(sim, (uint8_t)gameFrontOverviewWindow);
  serverSimSetLineOfSight(sim, (uint8_t)gameFrontLineOfSight);
  if (gameFrontClassicMode) {
    serverSimSetClassicMode(sim, true);
  }
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
    /* A dialog closes on a quit the same way it closes on Cancel, and most
     * of them steer back to the welcome screen as they go.  Asked here,
     * once, so the unwinding stops at the first screen to notice rather
     * than walking the player back up the menus one dialog at a time. */
    if (windowIsQuitting()) {
      done = TRUE;
      userQuit = TRUE;
      break;
    }
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
      /* The tutorial's own type, mines, timing and AI are chosen where the
       * single-player path builds the server, off isTutorial. They are not
       * written into the globals here: those are saved on exit as what the
       * next hosted game opens on, and a tutorial would replace the
       * player's picks with its own. */
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
      /* The editor runs its own loop in WinBolo's window, so a quit taken
       * there stops with it.  Leaving the editor comes back to the welcome
       * screen; quitting carries on out. */
      if (mapEditorAppQuitRequested()) windowSetQuitting();
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
      /* Same as the editor above: the viewer owns the loop while it is up. */
      if (logViewerAppQuitRequested()) windowSetQuitting();
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
      /* A spectator passes the server's password check like a player. The
       * browser's Spectate button runs no INFO pre-flight, so the first
       * request goes out with an empty password and the incorrect-password
       * reject is what asks for one; a wrong entry asks again. A spectator
       * is never the host, so the global always starts empty here. */
      password[0] = '\0';
      ClientSim *spectatorSim;
      for (;;) {
        spectatorSim = clientSimAlloc();
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
            if (clientSimGetConnectErrorLangId(spectatorSim) == STR_REJECT_INCORRECT_PASSWORD) {
              /* First reject means "this game has a password"; a later one
               * means the entry was wrong. Cancel falls out with no box. */
              bool wrongBefore = (password[0] != '\0');
              clientSimDisconnect(spectatorSim);
              clientSimDestroy(spectatorSim);
              if (gameFrontAskJoinPassword(wrongBefore)) continue;
              spectatorSim = NULL;
            } else {
              imguiMessageBoxEx(DIALOG_BOX_TITLE,
                                (reason && reason[0]) ? reason
                                                      : langGetText(NETERR_SERVERCONNECT),
                                IMGUI_MSG_ERROR, IMGUI_MSG_OK);
            }
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
        break;
      }
      /* Caller owns the ClientSim lifetime (spectatorRun never disconnects):
       * tear it down so the socket/transport is released before returning.
       * NULL when the password prompt was cancelled: that path already
       * tore its ClientSim down before asking. */
      if (spectatorSim != NULL) {
        clientSimDisconnect(spectatorSim);
        clientSimDestroy(spectatorSim);
      }
      /* Same as the editor and the viewer above: spectatorRun owns the loop
       * while it is up, so a quit taken there stops with it.  Asked after the
       * disconnect so the socket is released either way. */
      if (logViewerAppQuitRequested()) windowSetQuitting();
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
      /* The password global is only meaningful for a host joining its own
       * server (set by the game setup dialog and sent with the server
       * config). For a remote join it starts empty, so a password entered
       * for an earlier host or join never rides this JOIN_REQUEST. The
       * same INFO reply that carried the version says whether the server
       * wants a password: ask now, before the join, rather than sending an
       * empty one and reading the reject. Cancel ends the attempt quietly,
       * like the version and reachability failures above but with no
       * error box. */
      password[0] = '\0';
      if (dpr.password && !gameFrontAskJoinPassword(FALSE)) {
        gameFrontShutdownServer();
        dlgState = prevState;
        s_joinAttemptFailed = TRUE;
        return FALSE;
      }
    }

    prefsFlush();
    gameFrontValidateWbnBeforeJoin();
    /* One pass per join attempt. The loop only repeats when the server
     * rejected the password and the player typed another one; every other
     * outcome leaves through a break. */
    for (;;) {
      const char *failFallback = NULL;
      bool joined = FALSE;
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
        failFallback = langGetText(STR_GAMEFRONTERR_JOINGAME);
      } else {
        /* Wait for the join handshake (30s timeout). Landing accepts either a
         * running game or entry into the server lobby — see clientFrontAwaitJoin. */
        if (clientFrontAwaitJoin(humanSim, 1500)) {
          joined = TRUE;
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
          /* Hosting our own game on a map with a scenario: seat the lobby the
           * scenario asks for, now that the host's own join has landed. Its
           * template reached the sim at the attach in gameFrontSetupServer,
           * and the settings that go with it were applied there; the seating
           * waits until here because a seat takes the first free slot and the
           * host has to hold slot 0 — the lobby's host role starts there, and
           * a seat sitting in it would leave the host unable to change a
           * setting or start the game. spServerSimActive tells a host joining
           * its own server from somebody joining another one — a single-player
           * game sets it too, but never comes through openUdpJoin. Under the
           * mutex: the host timer is already ticking the sim. */
          if (spServerSimActive && spScenarioHost != NULL) {
            threadsWaitForMutex();
            serverSimScenarioSeatLobby(spServerSim);
            threadsReleaseMutex();
          }
          dlgState = openFinished;
        } else {
          failFallback = langGetText(NETERR_SERVERCONNECT);
        }
      }
      if (joined) break;

      /* The join failed. An incorrect-password reject on a remote join asks
       * again and retries with a fresh ClientSim; the host joining its own
       * server sent the password it configured, so a reject there is an
       * error like any other. Cancel at the prompt ends the attempt without
       * an error box: the player already knows why. */
      if (!spServerSimActive &&
          clientSimGetConnectErrorLangId(humanSim) == STR_REJECT_INCORRECT_PASSWORD) {
        clientSimDestroy(humanSim);
        humanSim = NULL;
        if (gameFrontAskJoinPassword(TRUE)) continue;
      } else {
        const char *reason = clientSimGetConnectErrorReason(humanSim);
        imguiMessageBoxEx(DIALOG_BOX_TITLE,
                          (reason && reason[0]) ? reason : failFallback,
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
        clientSimDestroy(humanSim);
        humanSim = NULL;
      }
      gameFrontShutdownServer();
      dlgState = prevState;
      s_joinAttemptFailed = TRUE;
      returnValue = FALSE;
      break;
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
        /* Single-player opens the lobby on the game type, computer tanks and
         * hidden mines the player last picked in a lobby they hosted, the
         * same saved values Internet New and LAN New open on. A player who
         * has never picked gets an Open game with Full Advantage AI. The
         * tutorial forces a solo strict-tournament game with no AI and no
         * time limit, ignoring every saved value. */
        gameType spGameType    = isTutorial ? gameStrictTournament : gametype;
        aiType   spAiPolicy    = isTutorial ? aiNone : compTanks;
        bool     spHiddenMines = isTutorial ? FALSE : hiddenMines;
        int32_t  spStartDelay  = isTutorial ? 0 : startDelay;
        int32_t  spTimeLen     = isTutorial ? UNLIMITED_GAME_TIME : timeLen;
        /* Seed one enemy bot when the launch carried no bot setup: human
         * on team 1, the bot on team 2 so they oppose each other. A setup
         * the user already configured (count > 0) is left untouched. With
         * the saved AI policy on none, the bot loop below makes nothing. */
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
          spServerSim = serverSimCreateRandomMap(&cfg, spGameType, spHiddenMines, spStartDelay, spTimeLen);
        } else if (strcmp(fileName, "") != 0) {
          spServerSim = serverSimCreate(fileName, spGameType, spHiddenMines, spStartDelay, spTimeLen);
        } else {
          BYTE emap[6000] = E_MAP;
          spServerSim = serverSimCreateCompressed(emap, 5097, "Everard Island", spGameType, spHiddenMines, spStartDelay, spTimeLen);
        }
        if (spServerSim != NULL) {
          /* Embedded server: silence its console messages (Thread Manager
           * Startup, Game started!, …) — the client has no server console. */
          serverSimSetQuiet(spServerSim, true);
          /* A scenario script beside the map this game was built from. A
             random or built-in map has no file on disk, so it carries none.
             No script says nothing; one loaded, one that cannot be used and
             one refused because scripts are off each say so. The switch is
             the library's, so this path has no test of its own. */
          /* The host's own preference, set on the library before the attach
             so the map commits that follow answer to it as well. Set both
             ways, because unlike a command-line switch this can be turned
             back on without restarting. */
          scenarioHostSetEnabled(gameFrontHostingScripts);
          /* And the narrower one beside it, applied at the same point: a map
             this host took as an upload plays plainly with it off. */
          scenarioHostSetUploadScriptsEnabled(
              gameFrontHostingScriptUploadPolicy != SCRIPT_UPLOAD_OFF);
          /* And the question the map chooser's server list asks of each map,
             registered beside the switch rather than at the attach: an attach
             answers NULL for a map with no script, so hosting a plain map
             would report every scripted map in the directory as plain. */
          scenarioHostRegisterMapScripted(spServerSim);
          /* And the read of this host's scenarios directory, registered
             beside it for the same reason: what the list holds has nothing to
             do with whichever map is being hosted. */
          serverSimSetScenarioDir(spServerSim, gameFrontHostingScenarioDir);
#if !defined(__ANDROID__) && !defined(__IPHONEOS__) && !defined(__EMSCRIPTEN__)
          /* And where subscribed Workshop items are copied to, which the map
             list offers as a "Workshop" folder. Mobile has no Workshop. */
          {
            char workshopDir[FILENAME_MAX];
            if (scenarioHostWorkshopDir(workshopDir, sizeof(workshopDir))) {
              serverSimSetWorkshopMapDir(spServerSim, workshopDir);
            }
          }
#endif
          scenarioHostRegisterScenarioLister(spServerSim);
          if (strncmp(fileName, "randommap:", 10) != 0 && fileName[0] != '\0') {
            char scenarioErr[512];
            spScenarioHost = scenarioHostAttach(spServerSim, fileName,
                                                scenarioErr,
                                                sizeof(scenarioErr));
            if (spScenarioHost != NULL) {
              WB_LOG_INFO(WB_LOG_CAT_GUI, "Scenario loaded: %s (from %s)",
                          scenarioHostName(spScenarioHost),
                          scenarioHostScriptPath(spScenarioHost));
            } else if (scenarioErr[0] != '\0') {
              WB_LOG_WARN(WB_LOG_CAT_GUI, "%s", scenarioErr);
            }
          }
          /* And from here on the scenario follows the committed map. */
          scenarioHostFollowMap(spServerSim, &spScenarioHost);
          /* The visibility rules this player last chose, from the [GAME
             OPTIONS] prefs, the same push the listen server makes. A
             single-player game opens a lobby with the Visibility dropdown
             in it, so it has to open on the set the player left the last
             lobby on rather than on the stock set serverSimCreate made.
             Before gameFrontStartServerSim, so the lobby snapshot behind
             serverSimResetLobbyToDefaults is taken from these values.

             Not for the tutorial. That runs with skipLobby, so it never
             shows the dropdown and nobody can change what it plays on;
             it keeps the plain view it has always had, the same way it
             ignores every other setting left over from an earlier game. */
          if (!isTutorial) {
            gameFrontApplyVisibilityPrefs(spServerSim);
          }
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
          cfg.compTanks           = (BYTE)spAiPolicy;
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
          /* No player can upload here, but the uploads directory is the last
           * one the script list reads, and it belongs under the prefs path
           * rather than inside the bundle. */
          char spScriptSessionDir[FILENAME_MAX];
          gameFrontScriptSessionDir(spScriptSessionDir,
                                    sizeof(spScriptSessionDir));
          cfg.scriptSessionDir = spScriptSessionDir;

          /* Build the ClientSim first — clientSimConnectLocalPassive
           * runs the full join+install body against an alive ClientSim. */
          humanSim = clientSimAlloc();
          clientSimCreate(humanSim);
          clientSimSetIsLanOnly(humanSim, s_isLanOnly);
          frontEndSetActiveClientSim(humanSim);

          /* A game that skips the lobby starts its round inside the startup
           * below, so the scenario's own settings have to be in force before
           * it: the round is built and the first tanks placed in there, and
           * a game type set afterwards would never be asked for. A game that
           * opens the lobby takes them once the host has joined, with the
           * seating. */
          if (spScenarioHost != NULL && cfg.skipLobby) {
            serverSimScenarioApplyLobbyRules(spServerSim);
          }

          /* Start the host timer; serverInstanceStartup applies the
           * lobby/skipLobby + hasPassword + brain/AI fields above. */
          if (!gameFrontStartServerSim(spServerSim, &cfg)) {
            WB_LOG_ERROR(WB_LOG_CAT_GUI,
                         "[SP-FAIL] gameFrontStartServerSim failed — "
                         "humanSim being nulled, dlgState stays at openFinished");
            frontEndSetActiveClientSim(NULL);
            clientSimDestroy(humanSim);
            humanSim = NULL;
            scenarioHostDetach(spScenarioHost);
            spScenarioHost = NULL;
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
            /* No setup bot at all in a lobby the map's script lays out
             * itself (Survival seats its whole horde): the script says who
             * sits in that lobby, and a bot made here, the seeded enemy or
             * one from the player's own settings, takes a slot ahead of the
             * script's seats and lands on a side the script never meant.
             * The player adds their own team's bots in the lobby by hand,
             * which is a different path and untouched. Only the bot loop
             * is skipped: the human's own team and the alliance pass below
             * still run, so the player lands on the defenders' side. */
            bool scriptSeats = (spScenarioHost != NULL) &&
                               serverSimScenarioHasLobbyTemplate(spServerSim);
            int botsToMake = scriptSeats ? 0 : gameFrontBotSetupData.count;
            if (spAiPolicy != aiNone && gameFrontBotSetupData.count > 0 && haveBrain) {
              for (int bi = 0; bi < botsToMake && bi < MAX_BOT_SLOTS; bi++) {
                BYTE slot = (BYTE)(bi + 1);
                char botName[32];
                snprintf(botName, sizeof(botName), "Bot %d", slot);
                /* One brain now, so the brain is just spBrainPath (the per-bot
                 * override is still honoured if some caller ever sets one).
                 * What the single-player skill guess picks is the bot's
                 * DIFFICULTY, written into the slot's lobby config before the
                 * bot is created so the brain is handed the difficulty= token
                 * on its very first load. */
                const char *botBrain = gameFrontBotSetupData.bots[bi].brainPath;
                if (botBrain[0] == '\0') botBrain = spBrainPath;
                uint8_t spMode  = gameFrontSpBotMode(botBrain);
                uint8_t spLevel = gameFrontSpBotLevel(botBrain, spMode);
                /* Resolved through the one rule a new bot follows, so the
                 * single-player path and the lobby cannot disagree. These bots
                 * are appearing for the first time, so the player's remembered
                 * manual pick is NOT applied — that pick is for the Add Bot
                 * button afterwards. */
                serverSimResolveNewBotConfig(spServerSim,
                                             (int)gameFrontBotSetupData.bots[bi].teamNumber,
                                             botBrain, false, &spMode, &spLevel);
                serverSimSetBotConfig(spServerSim, slot, spMode, spLevel,
                                      0 /* personality: normal */, NULL);
                /* No team and no init table here: single-player bots are
                 * placed by the alliance pass below, and their config comes
                 * from the slot config set just above. */
                serverSimCreateBot(spServerSim, slot, botBrain, botName, spAiPolicy,
                                   spGameType, spHiddenMines, 0, NULL);
                /* serverSimCreateBot loads the brain from the path but leaves
                 * the lobby brain-INDEX at the 0xFF "default" sentinel, so the
                 * lobby Bot Code dropdown renders "(none)". Resolve the index
                 * from the path (case-insensitive exact match, else the
                 * version-suffixed dir name as a substring) so the dropdown
                 * shows the actual brain — GoalHunter_1.7 by default. */
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
            /* The lobby the scenario asks for, and the settings that go with
             * it. Its template reached the sim at the attach above; seating
             * it is the separate step made wherever a lobby is built, and a
             * game opening on this map is one of those points — the callers
             * inside the sim are a map being committed and a lobby resetting
             * once the last player leaves, and this is neither.
             *
             * After the host has joined, so slot 0 is the host's rather than
             * a seat's, and after the bots above, which are put in slots 1
             * upward by number: a seat already in one of those slots would
             * be replaced by the bot that names it. The brain path and the
             * AI level the seating reads are in the sim from the startup's
             * config, and the bot pool has been up since the client booted.
             *
             * Not on a tutorial, which is the one path here that skips the
             * lobby: its round started inside the startup above, and the
             * startup seated the template itself on the way in so the round
             * could build a tank for every fielded seat. Seating again now
             * would empty those seats and rebuild them inside a round
             * already running, leaving them with no tanks.
             *
             * A map with no scenario has no template and this seats nothing;
             * a game that skipped the lobby has already had the settings
             * applied above and the second call changes nothing. */
            if (spScenarioHost != NULL) {
              if (!isTutorial) {
                serverSimScenarioSeatLobby(spServerSim);
              }
              serverSimScenarioApplyLobbyRules(spServerSim);
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

void gameFrontGetPlayerName(char *pn) {
  strcpy(pn, gameFrontName);
}

void gameFrontSetPlayerName(char *pn) {
  strcpy(gameFrontName, pn);
}

/* The AI type of the game being joined. It goes to the client sim and
 * the brains menu only. compTanks is left alone: it holds the computer
 * tanks pick the next game this machine hosts opens on, and a joined
 * game's setting is not that pick. Writing it here made a single-player
 * game started after joining a no-bots game open with no computer
 * tanks and no enemy bot. */
void gameFrontSetAIType(aiType ait) {
  if (humanSim != NULL) {
    clientSimSetAiType(humanSim, ait);
  }
  if (ait == aiNone) {
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

/* The map chooser's question, answered by the scenario library. The editor
   builds the chooser without that library and stubs this to false. */
bool mapChooserMapHasScript(const char *mapPath) {
  return scenarioHostMapHasScript(mapPath);
}

void gameFrontSetHostingScripts(bool allow) {
  gameFrontHostingScripts = allow;
  prefsSetString("HOSTING", "Run Map Scripts", TRUEFALSE_TO_STR(allow));
  /* And the library, which is what actually decides whether an attach loads
     a script. The two hosting-start paths set it as well, so a game started
     after this reads the same answer; setting it here is what makes the
     preference true of the process the moment it is changed, rather than
     only from the next hosted game. The map chooser's scripted tag reads it
     too, so a map stops being tagged as soon as the preference goes off. */
  scenarioHostSetEnabled(allow);
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

void gameFrontSetHostingScriptUploadPolicy(int policy) {
  gameFrontHostingScriptUploadPolicy = policy;
  prefsSetString("HOSTING", "Script Upload Policy",
                 scriptUploadPolicyWord((ScriptUploadPolicy)policy));
  /* And the library, for the reason gameFrontSetHostingScripts sets it: the
     preference is true of the process the moment it moves rather than from
     the next hosted game, and the map chooser's scripted tag reads it too,
     so an uploaded map stops being tagged as soon as this goes to Off. */
  scenarioHostSetUploadScriptsEnabled(policy != SCRIPT_UPLOAD_OFF);
}

void gameFrontSetHostingShareScripts(bool on) {
  gameFrontHostingShareScripts = on;
  prefsSetString("HOSTING", "Share Scripts", TRUEFALSE_TO_STR(on));
}

void gameFrontSetHostingScriptUploadMaxFiles(int maxFiles) {
  gameFrontHostingScriptUploadMaxFiles = maxFiles;
  char buf[16];
  intToStr(maxFiles, buf, sizeof(buf));
  prefsSetString("HOSTING", "Script Upload Max Files", buf);
}

void gameFrontSetHostingScriptUploadMaxStorage(int maxStorageMb) {
  gameFrontHostingScriptUploadMaxStorage = maxStorageMb;
  char buf[16];
  intToStr(maxStorageMb, buf, sizeof(buf));
  prefsSetString("HOSTING", "Script Upload Max Storage", buf);
}

void gameFrontSetHostingScriptUploadDir(const char *dir) {
  SDL_strlcpy(gameFrontHostingScriptUploadDir, dir ? dir : "",
              sizeof(gameFrontHostingScriptUploadDir));
  prefsSetString("HOSTING", "Script Upload Dir",
                 gameFrontHostingScriptUploadDir);
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

/* The scenarios directory is read at the two hosting-start paths, which pass
   it to serverSimSetScenarioDir, so a change made here is picked up by the
   next hosted game rather than by the one already running. */
void gameFrontSetHostingScenarioDir(const char *dir) {
  SDL_strlcpy(gameFrontHostingScenarioDir, dir ? dir : "",
              sizeof(gameFrontHostingScenarioDir));
  prefsSetString("HOSTING", "Mod Dir", gameFrontHostingScenarioDir);
}

void gameFrontSetHostingServeReplays(bool serve) {
  gameFrontHostingServeReplays = serve;
  prefsSetString("HOSTING", "Serve Replays", TRUEFALSE_TO_STR(serve));
}

/* Stored as a word rather than the enum number so a hand-edited INI reads
 * clearly, the same as Upload Policy. */
void gameFrontSetHostingVoiceMode(int mode) {
  gameFrontHostingVoiceMode = mode;
  const char *str = (mode == serverVoiceOff)       ? "Off"
                  : (mode == serverVoiceProximity) ? "Proximity"
                                                   : "On";
  prefsSetString("HOSTING", "Voice", str);
}

/* Visibility write-through setters. Same shape as the hosting ones
 * above: update the global and persist the [GAME OPTIONS] key now. The
 * policies are stored as words so a hand-edited INI reads clearly. */
static const char *viewPolicyPrefWord(int policy) {
  return (policy == viewPolicyKey)   ? "Key"
       : (policy == viewPolicyDecay) ? "Decay"
       : (policy == viewPolicyOff)   ? "Off"
                                     : "Always";
}

/* Reads back what viewPolicyPrefWord wrote. A word that is none of the four
 * returns the caller's fallback, so a mistyped INI value cannot turn a
 * category on. */
static int viewPolicyFromPrefWord(const char *word, int fallback) {
  if (strcmp(word, "Always") == 0) return viewPolicyAlways;
  if (strcmp(word, "Key")    == 0) return viewPolicyKey;
  if (strcmp(word, "Decay")  == 0) return viewPolicyDecay;
  if (strcmp(word, "Off")    == 0) return viewPolicyOff;
  return fallback;
}

/* The overview window in the same word form, one of the two
 * OverviewWindow names rather than its byte. */
static const char *overviewWindowPrefWord(int window) {
  return (window == overviewWindowNone)    ? "None"
       : (window == overviewWindowClassic) ? "Classic"
                                           : "Expanded";
}

/* Reads back what overviewWindowPrefWord wrote. Any other word returns
 * the caller's fallback, the same as viewPolicyFromPrefWord. */
static int overviewWindowFromPrefWord(const char *word, int fallback) {
  if (strcmp(word, "Expanded") == 0) return overviewWindowExpanded;
  if (strcmp(word, "Classic")  == 0) return overviewWindowClassic;
  if (strcmp(word, "None")     == 0) return overviewWindowNone;
  return fallback;
}

static int viewDecayClamp(int secs) {
  if (secs < VIEW_DECAY_MIN_SECS) return VIEW_DECAY_MIN_SECS;
  if (secs > VIEW_DECAY_MAX_SECS) return VIEW_DECAY_MAX_SECS;
  return secs;
}

void gameFrontSetViewPillPolicy(int policy) {
  gameFrontViewPillPolicy = policy;
  prefsSetString("GAME OPTIONS", "Pill View", viewPolicyPrefWord(policy));
}

void gameFrontSetViewBasePolicy(int policy) {
  gameFrontViewBasePolicy = policy;
  prefsSetString("GAME OPTIONS", "Base View", viewPolicyPrefWord(policy));
}

void gameFrontSetViewAllyPolicy(int policy) {
  gameFrontViewAllyPolicy = policy;
  prefsSetString("GAME OPTIONS", "Ally View", viewPolicyPrefWord(policy));
}

void gameFrontSetViewPillDecaySecs(int secs) {
  char buf[16];
  gameFrontViewPillDecaySecs = viewDecayClamp(secs);
  intToStr(gameFrontViewPillDecaySecs, buf, sizeof(buf));
  prefsSetString("GAME OPTIONS", "Pill View Decay", buf);
}

void gameFrontSetViewBaseDecaySecs(int secs) {
  char buf[16];
  gameFrontViewBaseDecaySecs = viewDecayClamp(secs);
  intToStr(gameFrontViewBaseDecaySecs, buf, sizeof(buf));
  prefsSetString("GAME OPTIONS", "Base View Decay", buf);
}

void gameFrontSetViewAllyDecaySecs(int secs) {
  char buf[16];
  gameFrontViewAllyDecaySecs = viewDecayClamp(secs);
  intToStr(gameFrontViewAllyDecaySecs, buf, sizeof(buf));
  prefsSetString("GAME OPTIONS", "Ally View Decay", buf);
}

void gameFrontSetClassicMode(bool on) {
  gameFrontClassicMode = on;
  prefsSetString("GAME OPTIONS", "Classic Mode", TRUEFALSE_TO_STR(on));
}

void gameFrontSetAlliesInTrees(bool on) {
  gameFrontAlliesInTrees = on;
  prefsSetString("GAME OPTIONS", "Allies In Trees", TRUEFALSE_TO_STR(on));
}

void gameFrontSetOverviewWindow(int window) {
  gameFrontOverviewWindow = window;
  prefsSetString("GAME OPTIONS", "Overview Window",
                 overviewWindowPrefWord(window));
}

void gameFrontSetLineOfSight(int mode) {
  bool on = (mode != lineOfSightOff);
  gameFrontLineOfSight = mode;
  prefsSetString("GAME OPTIONS", "Line Of Sight", TRUEFALSE_TO_STR(on));
}

/* ── Visibility preset and the remembered custom set ──────────────
 * The keys above record the values a hosted game starts with, one per
 * setting. These two record what the host chose rather than what it came
 * out as: which named preset is in force, and — when none of them is —
 * the whole hand-made set, kept under its own keys so picking a preset
 * and picking Custom back again lands where the host left it.
 *
 * The custom set is written only where the player made it by hand: the
 * hosting settings dialog, and an edit or a Custom pick in the lobby. It
 * is not written from what the settings happen to be on, because a preset
 * arrives one setting at a time and the half-applied mixes on the way
 * match no named set. That is what lets the set survive a trip through
 * the presets, and a restart. */
int                gameFrontVisibilityPreset = (int)visibilityPresetClassic;
VisibilitySettings gameFrontVisibilityCustom;
bool               gameFrontVisibilityCustomSaved = FALSE;

/* The seven [GAME OPTIONS] visibility globals as one set, and back. Every
 * caller below works in the set rather than in the globals, so a setting
 * added to the struct is added in one place here. */
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
}

/* Writes a whole set through the per-setting setters above, so the keys
 * that record the current values are kept up to date by the same code
 * that has always written them. */
static void gameFrontPutVisibilitySettings(const VisibilitySettings *v) {
  if (v == NULL) return;
  gameFrontSetViewPillPolicy((int)v->policy[viewCategoryPill]);
  gameFrontSetViewBasePolicy((int)v->policy[viewCategoryBase]);
  gameFrontSetViewAllyPolicy((int)v->policy[viewCategoryAlly]);
  gameFrontSetViewPillDecaySecs((int)v->decaySecs[viewCategoryPill]);
  gameFrontSetViewBaseDecaySecs((int)v->decaySecs[viewCategoryBase]);
  gameFrontSetViewAllyDecaySecs((int)v->decaySecs[viewCategoryAlly]);
  gameFrontSetAlliesInTrees(v->alliesInTrees);
  gameFrontSetOverviewWindow((int)v->overviewWindow);
  gameFrontSetLineOfSight((int)v->lineOfSight);
  gameFrontSetClassicMode(v->classicMode);
}

void gameFrontSetVisibilityPreset(int preset) {
  gameFrontVisibilityPreset = preset;
  prefsSetString("GAME OPTIONS", "Visibility Preset",
                 visibilityPresetPrefWord((VisibilityPreset)preset));
}

void gameFrontSetVisibilityCustom(const VisibilitySettings *v) {
  char buf[16];

  if (v == NULL) return;
  gameFrontVisibilityCustom      = *v;
  gameFrontVisibilityCustomSaved = TRUE;
  prefsSetString("GAME OPTIONS", "Custom Pill View",
                 viewPolicyPrefWord((int)v->policy[viewCategoryPill]));
  prefsSetString("GAME OPTIONS", "Custom Base View",
                 viewPolicyPrefWord((int)v->policy[viewCategoryBase]));
  prefsSetString("GAME OPTIONS", "Custom Ally View",
                 viewPolicyPrefWord((int)v->policy[viewCategoryAlly]));
  intToStr(viewDecayClamp((int)v->decaySecs[viewCategoryPill]), buf,
           sizeof(buf));
  prefsSetString("GAME OPTIONS", "Custom Pill View Decay", buf);
  intToStr(viewDecayClamp((int)v->decaySecs[viewCategoryBase]), buf,
           sizeof(buf));
  prefsSetString("GAME OPTIONS", "Custom Base View Decay", buf);
  intToStr(viewDecayClamp((int)v->decaySecs[viewCategoryAlly]), buf,
           sizeof(buf));
  prefsSetString("GAME OPTIONS", "Custom Ally View Decay", buf);
  prefsSetString("GAME OPTIONS", "Custom Classic Mode",
                 TRUEFALSE_TO_STR(v->classicMode));
  prefsSetString("GAME OPTIONS", "Custom Allies In Trees",
                 TRUEFALSE_TO_STR(v->alliesInTrees));
  prefsSetString("GAME OPTIONS", "Custom Overview Window",
                 overviewWindowPrefWord((int)v->overviewWindow));
  prefsSetString("GAME OPTIONS", "Custom Line Of Sight",
                 TRUEFALSE_TO_STR(v->lineOfSight != (uint8_t)lineOfSightOff));
}

/* Remembers a visibility set as the host's choice. Three things move
 * together, which is why they are one call rather than three: the seven
 * per-setting keys, so a game hosted again in this same session starts
 * there without a restart; which named set it is, so a preset that is
 * later given a different value follows the choice rather than the
 * values; and, when saveCustom is true and it is none of them, the set
 * itself.
 *
 * Called from every place a host changes visibility — the hosting
 * settings dialog, which edits these globals, and the lobby, which reads
 * the live settings off its own client. Each write only touches the INI
 * when the value moves, so calling it per frame costs a compare.
 *
 * The lobby passes false for saveCustom. It calls this every frame, and a
 * preset reaches the lobby one setting at a time, so the values it reads
 * pass through mixes that match no named set on the way. Saving those
 * would replace the player's hand-made set with a half-applied preset.
 * The lobby writes that set itself instead, with
 * gameFrontSetVisibilityCustom, when the player edits or picks something
 * on this machine. The hosting settings dialog passes true: the seven
 * controls there are edited by hand and nothing else writes them. */
void gameFrontRememberVisibility(const VisibilitySettings *v, bool saveCustom) {
  VisibilitySettings cur;
  VisibilityPreset   p;

  if (v == NULL) return;
  gameFrontGetVisibilitySettings(&cur);
  if (!visibilitySettingsEqual(&cur, v)) {
    gameFrontPutVisibilitySettings(v);
  }
  p = visibilityPresetMatch(v);
  if ((int)p != gameFrontVisibilityPreset) {
    gameFrontSetVisibilityPreset((int)p);
  }
  if (saveCustom && p == visibilityPresetCustom &&
      (!gameFrontVisibilityCustomSaved ||
       !visibilitySettingsEqual(v, &gameFrontVisibilityCustom))) {
    gameFrontSetVisibilityCustom(v);
  }
}

void gameFrontRememberGameType(gameType gt) {
  char buff[16];

  if ((int)gt < (int)gameOpen || (int)gt > (int)gameStrictTournament) return;
  gametype = gt;
  intToStr(gametype, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "New Game Type", buff);
}

void gameFrontRememberAiPolicy(aiType ai) {
  char buff[16];

  if ((int)ai < (int)aiNone || (int)ai > (int)aiFull) return;
  compTanks = ai;
  intToStr(compTanks, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "New Game Computer Tanks", buff);
}

void gameFrontRememberHiddenMines(bool hm) {
  hiddenMines = hm;
  prefsSetString("GAME OPTIONS", "New Game Hidden Mines",
                 TRUEFALSE_TO_STR(hiddenMines));
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

/* Picks up a skin change: rebuilds the tile atlas, drops the cached
 * background so the next frame reads it again, and reloads the sound set.
 * The renderer, window, fonts and zoom are left alone. */
void gameFrontReloadSkins(void) {
  sdl3DrawSetReconfigureGuard(true);
  sdl3DrawReloadTiles();
  sdl3DrawReloadBackground();
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
  scenarioHostDetach(spScenarioHost);
  spScenarioHost = NULL;
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
    /* A bounded drain: the host is waiting on this leave, and an
     * unreachable WinBolo.net would otherwise hold it for as long as the
     * queue is deep. Whatever is left rides the next session. */
    winbolonetEndSession(/*drainMaxMs*/ 2000);
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

/* The bot difficulty the player last explicitly chose from the lobby wrench
 * dropdown. This is the player's own preference and overrides the automatic
 * single-player skill guess from then on. Unset until first chosen (the
 * default on a fresh install), which is what the false return means.
 *
 * Stored as the word, not the number, so the prefs file stays readable and a
 * future difficulty doesn't have to reuse an index. */
void gameFrontSetChosenBotDifficulty(uint8_t difficulty) {
  gameFrontSetChosenBotModeAndLevel("default", botDifficultyName(difficulty));
}

/* The mode + level KEYS the player last picked in the lobby gear popup.
 * Both are the brain's own manifest keys (brains/<brain>/modes.txt), so
 * "Chosen Difficulty" keeps holding easy/medium/hard for the default mode
 * — the same words it held before modes existed, which is what makes the
 * old preference migrate by simply still being read. */
void gameFrontSetChosenBotModeAndLevel(const char *modeKey,
                                       const char *levelKey) {
  prefsSetString("BOT", "Chosen Mode", (modeKey && modeKey[0]) ? modeKey : "default");
  if (levelKey && levelKey[0]) {
    prefsSetString("BOT", "Chosen Difficulty", levelKey);
  }
}

bool gameFrontGetChosenBotModeKey(char *out, size_t outSz) {
  char buff[BRAIN_MODE_KEY_LEN];
  if (!out || outSz == 0) return false;
  out[0] = '\0';
  prefsGetString("BOT", "Chosen Mode", "", buff, (int)sizeof(buff));
  if (buff[0] == '\0') return false;
  SDL_strlcpy(out, buff, outSz);
  return true;
}

bool gameFrontGetChosenBotLevelKey(char *out, size_t outSz) {
  char buff[BRAIN_MODE_KEY_LEN];
  if (!out || outSz == 0) return false;
  out[0] = '\0';
  prefsGetString("BOT", "Chosen Difficulty", "", buff, (int)sizeof(buff));
  if (buff[0] == '\0') return false;
  SDL_strlcpy(out, buff, outSz);
  return true;
}

/* Which of a brain's modes a single-player bot is created in: the player's
 * chosen mode when the brain still declares it, else mode 0 (the default
 * mode every ordinary game uses). */
uint8_t gameFrontSpBotMode(const char *brainPath) {
  char key[BRAIN_MODE_KEY_LEN];
  BrainModes modes;
  if (!gameFrontGetChosenBotModeKey(key, sizeof(key))) return 0;
  brainListLoadModesForPath(brainPath, &modes);
  int idx = brainModesFindMode(&modes, key);
  return (idx > 0) ? (uint8_t)idx : 0;
}

/* The difficulty index that goes with gameFrontSpBotMode: the player's
 * chosen level key inside that mode, else the mode's own default level.
 * For mode 0 of a manifest-less brain this is exactly the old
 * gameFrontSpBotDifficulty answer. */
uint8_t gameFrontSpBotLevel(const char *brainPath, uint8_t mode) {
  char key[BRAIN_MODE_KEY_LEN];
  BrainModes modes;
  brainListLoadModesForPath(brainPath, &modes);
  if (mode >= (uint8_t)modes.modeCount) mode = 0;
  const BrainMode *m = &modes.modes[mode];
  if (mode == 0) {
    /* Default mode keeps the skill guess, which is the whole point of it. */
    uint8_t d = gameFrontSpBotDifficulty();
    return (d < (uint8_t)m->levelCount) ? d : (uint8_t)m->defaultLevel;
  }
  if (gameFrontGetChosenBotLevelKey(key, sizeof(key))) {
    int lvl = brainModeFindLevel(m, key);
    if (lvl >= 0) return (uint8_t)lvl;
  }
  return (uint8_t)m->defaultLevel;
}

/* Per-bot-name tag colour, kept under "BOT" / "Tag Color <name>" as
 * "#RRGGBB". Written once when the lobby first derives a colour for a bot
 * that declares none in its about.txt, read every time after, so the same
 * bot wears the same colour on every launch. */
static void gameFrontBotTagColorKey(const char *botName, char *key, size_t keySz) {
  SDL_snprintf(key, keySz, "Tag Color %s", botName ? botName : "");
}

bool gameFrontGetBotTagColor(const char *botName, uint32_t *rgb) {
  char key[96], buff[16];
  if (!botName || !botName[0] || !rgb) return false;
  gameFrontBotTagColorKey(botName, key, sizeof(key));
  prefsGetString("BOT", key, "", buff, (int)sizeof(buff));
  const char *v = buff;
  if (*v == '#') v++;
  if (strlen(v) != 6) return false;
  char *end = NULL;
  unsigned long val = strtoul(v, &end, 16);
  if (!end || *end != '\0') return false;
  *rgb = (uint32_t)val & 0xFFFFFFu;
  return true;
}

void gameFrontSetBotTagColor(const char *botName, uint32_t rgb) {
  char key[96], val[16];
  if (!botName || !botName[0]) return;
  gameFrontBotTagColorKey(botName, key, sizeof(key));
  SDL_snprintf(val, sizeof(val), "#%06X", (unsigned)(rgb & 0xFFFFFFu));
  prefsSetString("BOT", key, val);
}

/* Per-scenario scenario-panel layout, kept under "SCENARIO PANEL" / the
 * scenario as one "x,y,scale,alpha" row. One row rather than four keys keeps
 * a scenario's numbers together and keeps the file short enough to read, and
 * the scenario is the whole key so a player scanning the section sees the
 * scenarios they have played by name.
 *
 * The key is copied a character at a time rather than with SDL_snprintf
 * because a control character in a scenario's name would reach the
 * preferences file as an escape and make the row impossible to match up by
 * eye. Anything below a space becomes an underscore here, and it does so in
 * the read and the write alike, so both still name the same row. A byte
 * above 0x7F is left as it is: those are the middle of a UTF-8 character in
 * a name somebody chose, not a control code. */
static void gameFrontScnPanelLayoutKey(const char *scenario, char *key,
                                       size_t keySz) {
  size_t i = 0;
  if (keySz == 0) return;
  while (scenario[i] != '\0' && i + 1 < keySz) {
    key[i] = ((unsigned char)scenario[i] >= 0x20) ? scenario[i] : '_';
    i++;
  }
  key[i] = '\0';
}

bool gameFrontGetScnPanelLayout(const char *scenario, int *x, int *y,
                                int *scale, int *alpha) {
  char key[SCN_PANEL_SCENARIO_LEN], buff[64];
  int rx, ry, rscale, ralpha;
  if (!scenario || !scenario[0] || !x || !y || !scale || !alpha) return false;
  gameFrontScnPanelLayoutKey(scenario, key, sizeof(key));
  prefsGetString("SCENARIO PANEL", key, "", buff, sizeof(buff));
  /* All four or none: a half-written row says nothing about where the panel
     belongs, and the caller's fallback is a whole layout of its own rather
     than something to fill the gaps in this one with. */
  if (SDL_sscanf(buff, "%d,%d,%d,%d", &rx, &ry, &rscale, &ralpha) != 4) {
    return false;
  }
  /* Clamped the way the [WINDOW] load clamps, and for the same reason: a
     scale out of range would open the panel bigger than the screen or too
     small to get a pointer onto a corner of. -1 is not a size or an opacity,
     it is "never touched", so it passes through. The coordinates pass
     through too — the panel clamps a position against the window it is
     restored into, which is the only place the screen size is known. */
  if (rscale != -1) {
    if (rscale < SCN_PANEL_SCALE_MIN) {
      rscale = SCN_PANEL_SCALE_MIN;
    } else if (rscale > SCN_PANEL_SCALE_MAX) {
      rscale = SCN_PANEL_SCALE_MAX;
    }
  }
  if (ralpha != -1) {
    if (ralpha < 0) {
      ralpha = 0;
    } else if (ralpha > 100) {
      ralpha = 100;
    }
  }
  *x = rx;
  *y = ry;
  *scale = rscale;
  *alpha = ralpha;
  return true;
}

void gameFrontSetScnPanelLayout(const char *scenario, int x, int y,
                                int scale, int alpha) {
  char key[SCN_PANEL_SCENARIO_LEN], val[64];
  if (!scenario || !scenario[0]) return;
  gameFrontScnPanelLayoutKey(scenario, key, sizeof(key));
  SDL_snprintf(val, sizeof(val), "%d,%d,%d,%d", x, y, scale, alpha);
  prefsSetString("SCENARIO PANEL", key, val);
}

bool gameFrontGetChosenBotDifficulty(uint8_t *out) {
  char buff[32];
  if (!out) return false;
  /* Only the DEFAULT mode's level keys are easy/medium/hard, so a player
   * whose last pick was in another mode has no default-mode preference to
   * honour here — the skill guess takes over instead. */
  prefsGetString("BOT", "Chosen Mode", "default", buff, (int)sizeof(buff));
  if (buff[0] != '\0' && SDL_strcasecmp(buff, "default") != 0) return false;
  prefsGetString("BOT", "Chosen Difficulty", "", buff, (int)sizeof(buff));
  if (buff[0] != '\0' && botDifficultyFromName(buff, out)) return true;
  /* Migration from the pre-difficulty pref, which named a brain directory:
   * the one gentle brain was GoalHunter_1.0, everything else was a hard
   * one. Read-only — the new key is written the next time the player picks
   * a difficulty, and until then the old choice keeps being honoured. */
  prefsGetString("BOT", "Chosen Brain", "", buff, (int)sizeof(buff));
  if (buff[0] == '\0') return false;
  *out = (SDL_strcasecmp(buff, "GoalHunter_1.0") == 0) ? BOT_DIFFICULTY_EASY
                                                       : BOT_DIFFICULTY_HARD;
  return true;
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

/* A config cleared with memset, an advertisement from a server built before
 * the voice field existed, and a [HOSTING] Voice value that cannot be parsed
 * all have to mean voice on, and each of them arrives as a zero. Stated here
 * because the memset that leans on it is in the function below. Reordering
 * ServerVoiceMode would turn all three into off with no line changing. */
BOLO_STATIC_ASSERT(serverVoiceOn == 0, server_voice_default_is_on);

bool gameFrontSetupServer(void) {
  ServerInstanceConfig cfg;
  char scriptSessionDir[FILENAME_MAX];

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

  /* A scenario script beside the map, as on the single-player path, and the
     same host preference deciding whether it runs at all. */
  scenarioHostSetEnabled(gameFrontHostingScripts);
  scenarioHostSetUploadScriptsEnabled(
      gameFrontHostingScriptUploadPolicy != SCRIPT_UPLOAD_OFF);
  scenarioHostRegisterMapScripted(spServerSim);
  serverSimSetScenarioDir(spServerSim, gameFrontHostingScenarioDir);
#if !defined(__ANDROID__) && !defined(__IPHONEOS__) && !defined(__EMSCRIPTEN__)
  /* And the Workshop map folder, as on the single-player path. */
  {
    char workshopDir[FILENAME_MAX];
    if (scenarioHostWorkshopDir(workshopDir, sizeof(workshopDir))) {
      serverSimSetWorkshopMapDir(spServerSim, workshopDir);
    }
  }
#endif
  scenarioHostRegisterScenarioLister(spServerSim);
  if (strncmp(fileName, "randommap:", 10) != 0 && fileName[0] != '\0') {
    char scenarioErr[512];
    spScenarioHost = scenarioHostAttach(spServerSim, fileName,
                                        scenarioErr, sizeof(scenarioErr));
    if (spScenarioHost != NULL) {
      WB_LOG_INFO(WB_LOG_CAT_GUI, "Scenario loaded: %s (from %s)",
                  scenarioHostName(spScenarioHost),
                  scenarioHostScriptPath(spScenarioHost));
    } else if (scenarioErr[0] != '\0') {
      WB_LOG_WARN(WB_LOG_CAT_GUI, "%s", scenarioErr);
    }
  }
  /* And from here on the scenario follows the committed map. */
  scenarioHostFollowMap(spServerSim, &spScenarioHost);

  /* The visibility rules this host last chose, from the [GAME OPTIONS]
   * prefs. Shared with the single-player path, which opens the same
   * lobby and has to open it on the same settings. */
  gameFrontApplyVisibilityPrefs(spServerSim);

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
  /* cfg is memset above, which would leave voiceMode at serverVoiceOn; this
   * line is what carries the [HOSTING] Voice pref to the server instead. */
  cfg.voiceMode           = (ServerVoiceMode)gameFrontHostingVoiceMode;
  /* Persist saves uploads to disk under the chosen directory. Create it on
   * use and refuse to host if that fails — no silent fallback. Off/Allow
   * never touch disk, so leave uploadPersistDir NULL (memset-zero) for them. */
  if (gameFrontHostingUploadPolicy == UPLOAD_POLICY_PERSIST) {
    if (!SDL_CreateDirectory(gameFrontHostingUploadDir)) {
      WB_LOG_WARN(WB_LOG_CAT_NET,
                  "cannot create upload directory '%s' — refusing to host",
                  gameFrontHostingUploadDir);
      scenarioHostDetach(spScenarioHost);
      spScenarioHost = NULL;
      serverSimDestroy(spServerSim);
      spServerSim = NULL;
      return FALSE;
    }
    cfg.uploadPersistDir  = gameFrontHostingUploadDir;
  }
  cfg.scriptUploadPolicy  =
      (ScriptUploadPolicy)gameFrontHostingScriptUploadPolicy;
  cfg.noScriptSharing     = !gameFrontHostingShareScripts;
  cfg.scriptUploadMaxFiles = (uint8_t)gameFrontHostingScriptUploadMaxFiles;
  cfg.scriptUploadMaxStorageBytes =
      (uint32_t)gameFrontHostingScriptUploadMaxStorage * 1024u * 1024u;
  /* Only Persist keeps scripts on disk, so only Persist names a directory.
   * Off and Allow leave scriptUploadDir NULL (memset-zero). */
  if (gameFrontHostingScriptUploadPolicy == SCRIPT_UPLOAD_PERSIST) {
    cfg.scriptUploadDir   = gameFrontHostingScriptUploadDir;
  }
  /* Allow keeps them for the session, under the prefs path. Set whatever the
   * policy, since the server only reads it under Allow. */
  gameFrontScriptSessionDir(scriptSessionDir, sizeof(scriptSessionDir));
  cfg.scriptSessionDir    = scriptSessionDir;
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
      scenarioHostDetach(spScenarioHost);
      spScenarioHost = NULL;
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
    scenarioHostDetach(spScenarioHost);
    spScenarioHost = NULL;
    serverSimDestroy(spServerSim);
    spServerSim = NULL;
    return FALSE;
  }
  /* The settings the scenario asks for. A hosted game always opens the
   * lobby, so no round is starting here and this is the first point the
   * sim's own settings can be brought into line with the script beside the
   * map — the two callers inside the sim are a map being committed and a
   * lobby resetting once the last player leaves, and a fresh host is
   * neither. Without it the lobby the host and every joiner see is on the
   * host's own game type, and gameTypeResolve is never asked for the game
   * the scenario declares.
   *
   * Under the mutex: the host timer is armed by the call above and is
   * already ticking this sim. A map with no scenario leaves this alone. */
  if (spScenarioHost != NULL) {
    threadsWaitForMutex();
    serverSimScenarioApplyLobbyRules(spServerSim);
    threadsReleaseMutex();
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
  prefsGetString("HOSTING", "Run Map Scripts", "Yes", buff, FILENAME_MAX);
  gameFrontHostingScripts = YESNO_TO_TRUEFALSE(buff[0]);
  /* Script Upload Policy replaced the Yes/No "Run Upload Scripts". A file
   * written before it has only the old key, so that is read in its place
   * (No is Off, anything else Allow); the old key is never written again,
   * and once the new one is saved it is not read. */
  prefsGetString("HOSTING", "Script Upload Policy", "", buff, FILENAME_MAX);
  if (buff[0] != '\0') {
    gameFrontHostingScriptUploadPolicy =
        scriptUploadPolicyResolve(buff, false);
  } else {
    prefsGetString("HOSTING", "Run Upload Scripts", "Yes", buff, FILENAME_MAX);
    gameFrontHostingScriptUploadPolicy =
        scriptUploadPolicyResolve(NULL, !YESNO_TO_TRUEFALSE(buff[0]));
  }
  prefsGetString("HOSTING", "Share Scripts", "Yes", buff, FILENAME_MAX);
  gameFrontHostingShareScripts = YESNO_TO_TRUEFALSE(buff[0]);
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
  prefsGetString("HOSTING", "Script Upload Max Files", "32", buff, FILENAME_MAX);
  {
    int f = atoi(buff);
    if (f < 1) f = 1;
    if (f > 255) f = 255;
    gameFrontHostingScriptUploadMaxFiles = f;
  }
  prefsGetString("HOSTING", "Script Upload Max Storage", "64", buff, FILENAME_MAX);
  {
    int st = atoi(buff);
    if (st < 1) st = 1;
    if (st > 4095) st = 4095;
    gameFrontHostingScriptUploadMaxStorage = st;
  }
  /* Script Upload Dir defaults under the upload dir's default, for the same
   * reason: the prefs path is writable and the bundle is not. */
  {
    const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir) {
      snprintf(def, FILENAME_MAX, "%suploads/Scripts", prefDir);
      SDL_free((void *)prefDir);
    } else {
      snprintf(def, FILENAME_MAX, "%s", "uploads/Scripts");
    }
    prefsGetString("HOSTING", "Script Upload Dir", def,
                   gameFrontHostingScriptUploadDir, FILENAME_MAX);
  }
  /* The mod directory, defaulted under the writable prefs path for the
   * reason the upload dir is: the app's own data directory is inside the
   * read-only bundle, and this is a place a player drops files into.
   * SDL_GetPrefPath returns a trailing separator, so append "Mods" directly,
   * spelled the way brain_list.c spells Brains beside it. A directory that
   * is not there is not an error — the host is still offered the mods that
   * ship with the build.
   *
   * This is the same directory scnModDirs reads on its own, so leaving the
   * preference alone changes nothing about what a host is offered. It is
   * still a preference because a player who keeps their mods somewhere else
   * — a shared drive, a checkout — has to be able to say so.
   *
   * "Mod Dir" and not the "Scenario Dir" this key was called before. A
   * settings file written by an older build still has the old key, so it is
   * what the new one defaults to: a player who had pointed it somewhere
   * keeps pointing there, and the value moves to the new key the next time
   * the settings are written. The old key stays in the file and is never
   * read again once "Mod Dir" is there, because it is only ever consulted as
   * that key's default. */
  {
    const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    char        old[FILENAME_MAX];

    if (prefDir) {
      snprintf(def, FILENAME_MAX, "%sMods", prefDir);
      SDL_free((void *)prefDir);
    } else {
      snprintf(def, FILENAME_MAX, "%s", "Mods");
    }
    prefsGetString("HOSTING", "Scenario Dir", def, old, FILENAME_MAX);
    prefsGetString("HOSTING", "Mod Dir", old,
                   gameFrontHostingScenarioDir, FILENAME_MAX);
    /* Made if it is not there, unlike the directories above it: this is the
     * one a player is told to drop files into, and a folder that has to be
     * created before it can be used is a folder most players never find. It
     * succeeding is not required — a read-only home directory means no mods
     * of their own, which the listing already says nothing about.
     *
     * The one the preference settled on, and after the reads rather than
     * before them: a player who keeps their mods on a shared drive named it
     * here, and making the default under the prefs path as well would leave
     * an empty Mods folder they never asked for in their home directory
     * every time the settings are read. */
    (void)SDL_CreateDirectory(gameFrontHostingScenarioDir);
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
  /* Anything the setter did not write — a mistyped word, or the key absent
   * on an INI written before this setting existed — reads as On, which is
   * what a client host did then. */
  prefsGetString("HOSTING", "Voice", "On", buff, FILENAME_MAX);
  if (strcmp(buff, "Off") == 0) {
    gameFrontHostingVoiceMode = serverVoiceOff;
  } else if (strcmp(buff, "Proximity") == 0) {
    gameFrontHostingVoiceMode = serverVoiceProximity;
  } else {
    gameFrontHostingVoiceMode = serverVoiceOn;
  }

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
  intToStr(DEFAULT_BASEVIEW, def, sizeof(def));
  prefsGetString("KEYS", "Base View", def, buff, FILENAME_MAX);
  keys->kiBaseView = atoi(buff);
  intToStr(DEFAULT_OVERVIEW_ZOOM, def, sizeof(def));
  prefsGetString("KEYS", "Overview Zoom", def, buff, FILENAME_MAX);
  keys->kiOverviewZoom = atoi(buff);
  intToStr(DEFAULT_OVERVIEW_FOLLOW, def, sizeof(def));
  prefsGetString("KEYS", "Overview Follow", def, buff, FILENAME_MAX);
  keys->kiOverviewFollow = atoi(buff);
  intToStr(DEFAULT_OVERVIEW_ZOOMIN, def, sizeof(def));
  prefsGetString("KEYS", "Overview Zoom In", def, buff, FILENAME_MAX);
  keys->kiOverviewZoomIn = atoi(buff);
  intToStr(DEFAULT_OVERVIEW_ZOOMOUT, def, sizeof(def));
  prefsGetString("KEYS", "Overview Zoom Out", def, buff, FILENAME_MAX);
  keys->kiOverviewZoomOut = atoi(buff);

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

  /* The voice keys shipped unbound, so every preferences file written before
     they had defaults holds an explicit 0 for both and would never see the
     defaults below. Apply them once over that stored 0, and record that it has
     been done: after this the player's own binding stands, an empty one
     included, so Clear in key setup is not undone on the next launch. */
  prefsGetString("KEYS", "Voice Defaults Applied", "", buff, FILENAME_MAX);
  bool voiceKeyDefaults = (buff[0] == '\0');

  /* Push to talk — Q, which is bound to nothing else and sits under the left
     hand beside the movement keys (E/D/S/F). A file saved while the key was
     unbound holds 0, which the one-shot above turns into Q. */
  intToStr(DEFAULT_PUSHTOTALK, def, sizeof(def));
  prefsGetString("KEYS", "Push To Talk", def, buff, FILENAME_MAX);
  keys->kiPushToTalk = atoi(buff);
  if (voiceKeyDefaults && keys->kiPushToTalk == 0) {
    keys->kiPushToTalk = DEFAULT_PUSHTOTALK;
  }

  /* Mute microphone — Z, on the same terms as the key above: unbound
     elsewhere, and taking the default over a stored 0 the first time only. */
  intToStr(DEFAULT_MUTEMIC, def, sizeof(def));
  prefsGetString("KEYS", "Mute Mic", def, buff, FILENAME_MAX);
  keys->kiMuteMic = atoi(buff);
  if (voiceKeyDefaults && keys->kiMuteMic == 0) {
    keys->kiMuteMic = DEFAULT_MUTEMIC;
  }

  /* Marked here rather than in gameFrontPutPrefs so a session that never
     saves its preferences does not apply the defaults a second time. */
  prefsSetString("KEYS", "Voice Defaults Applied", "Yes");

  /* Smart ping — three menu chord slots and one per ping kind for the direct
     pings. Stored as the same packed int the rest of the section uses, so an
     older build reading a newer file just sees a number it does not recognise
     in a key it does not know. A file written by the four-slot build still
     loads: "Ping 4" is simply never read. */
  {
    static const int pingDefaults[PING_BIND_SLOTS] = {
      DEFAULT_PING1, DEFAULT_PING2, DEFAULT_PING3
    };
    int pi;
    for (pi = 0; pi < PING_BIND_SLOTS; pi++) {
      char name[32];
      snprintf(name, sizeof(name), "Ping %d", pi + 1);
      intToStr(pingDefaults[pi], def, sizeof(def));
      prefsGetString("KEYS", name, def, buff, FILENAME_MAX);
      keys->kiPing[pi] = atoi(buff);
    }
    for (pi = 0; pi < PING_BIND_DIRECT_SLOTS; pi++) {
      char name[32];
      snprintf(name, sizeof(name), "Ping Direct %d", pi + 1);
      prefsGetString("KEYS", name, "0", buff, FILENAME_MAX);
      keys->kiPingDirect[pi] = atoi(buff);
    }
  }
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

  /* Skin id.  Applied in gameFrontSetup before the tile sheet is built;
     an empty value keeps the built-in assets. */
  prefsGetString("SETTINGS", "Skin", "", buff, FILENAME_MAX);
  strncpy(gameFrontSkinId, buff, sizeof(gameFrontSkinId) - 1);
  gameFrontSkinId[sizeof(gameFrontSkinId) - 1] = '\0';

  /* Graphics settings.  Tile detail is 0 Classic / 1 Match to Zoom /
     2 High Detail, animation smoothness 0 Classic / 1 Match Pixelation /
     2 Smooth, texture filter 0 Nearest / 1 Linear / 2 Pixel Art.  Only tile
     detail has a reader so far; the rest are kept so all four settings load
     and save in one place. */
  prefsGetString("SETTINGS", "TileDetail", "0", buff, FILENAME_MAX);
  {
    int v = atoi(buff);
    if (v < (int)GFX_TILE_DETAIL_CLASSIC || v > (int)GFX_TILE_DETAIL_HIGH) v = 0;
    gfxSetTileDetail((GfxTileDetail)v);
  }
  prefsGetString("SETTINGS", "AnimSmoothness", "0", buff, FILENAME_MAX);
  {
    int v = atoi(buff);
    if (v < (int)GFX_ANIM_CLASSIC || v > (int)GFX_ANIM_SMOOTH) v = 0;
    gfxSetAnimSmoothness((GfxAnimSmoothness)v);
  }
  prefsGetString("SETTINGS", "SmoothShells", "No", buff, FILENAME_MAX);
  gfxSetSmoothShells(YESNO_TO_TRUEFALSE(buff[0]));
  prefsGetString("SETTINGS", "TextureFilter", "0", buff, FILENAME_MAX);
  {
    int v = atoi(buff);
    if (v < (int)GFX_FILTER_NEAREST || v > (int)GFX_FILTER_PIXELART) v = 0;
    gfxSetTextureFilter((GfxTextureFilter)v);
  }
  /* The simplified view, and whether it is held to the Map Overview window.
     On by default, since that is how the zoomed-out map is meant to look;
     the sub-option off, so it applies to the full screen map as well.  The
     log viewer reads these same two keys out of the same prefs document. */
  prefsGetString("SETTINGS", "SimplifiedZoomOut", "Yes", buff, FILENAME_MAX);
  gfxSetSimplifiedZoomOut(YESNO_TO_TRUEFALSE(buff[0]));
  prefsGetString("SETTINGS", "SimplifiedOverviewOnly", "No", buff, FILENAME_MAX);
  gfxSetSimplifiedOverviewOnly(YESNO_TO_TRUEFALSE(buff[0]));

  /* Fog of war look: 0 Grey / 1 Darker / 2 Darker with fog edge / 3 None.
     The fallback is "-1" rather than a style number so that a missing key and
     an unreadable one take the same road out - both fail the range check below
     and land on FOG_STYLE_DEFAULT, which is the one place the default is
     written down.  A player who has picked keeps their pick: only an absent or
     out-of-range value is replaced. */
  prefsGetString("SETTINGS", "FogStyle", "-1", buff, FILENAME_MAX);
  {
    int v = atoi(buff);
    if (v < (int)FOG_STYLE_GREY || v >= FOG_STYLE_COUNT) {
      v = (int)FOG_STYLE_DEFAULT;
    }
    gfxSetFogStyle((FogStyle)v);
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

  /* Game Options. The type, computer tanks and hidden mines a hosted game
   * opens on are the ones last picked in a lobby this machine ran, under
   * keys of their own. The older "Game Type", "Allow Computer Tanks" and
   * "Hidden Mines" keys were written on every exit although nothing let a
   * player set them, so they cannot tell a pick from the stock value and
   * are no longer read. A player who has not picked gets an Open game
   * with Full Advantage computer tanks. Clamped on read so a hand-edited
   * value cannot open a lobby on a type or policy it does not offer. */
  prefsGetString("GAME OPTIONS", "New Game Hidden Mines", "No", buff, FILENAME_MAX);
  hiddenMines = YESNO_TO_TRUEFALSE(buff[0]);
  intToStr(aiFull, def, sizeof(def));
  prefsGetString("GAME OPTIONS", "New Game Computer Tanks", def, buff, FILENAME_MAX);
  {
    int ai = atoi(buff);
    compTanks = (ai < (int)aiNone || ai > (int)aiFull) ? aiFull : (aiType)ai;
  }
  intToStr(gameOpen, def, sizeof(def));
  prefsGetString("GAME OPTIONS", "New Game Type", def, buff, FILENAME_MAX);
  {
    int gt = atoi(buff);
    gametype = (gt < (int)gameOpen || gt > (int)gameStrictTournament)
                 ? gameOpen : (gameType)gt;
  }
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

  /* Visibility rules for games this client hosts. Clamped on read so a
   * hand-edited INI can't inject an out-of-range decay. A key that is
   * absent, and a word that is none of the four, both read as that
   * row's stock policy — the same set the dedicated server's -pillview
   * / -baseview / -allyview start from. The word handed to
   * prefsGetString is derived from that policy through
   * viewPolicyPrefWord rather than spelled out again, so the INI
   * default and the value it stands for cannot drift apart. */
  {
    static const struct {
      const char *policyKey;
      const char *decayKey;
      int         policyStock;   /* meaning A in view_policy.h */
      int        *policyOut;
      int        *decayOut;
    } viewPrefs[] = {
      { "Pill View", "Pill View Decay", VIEW_POLICY_STOCK_PILL,
        &gameFrontViewPillPolicy, &gameFrontViewPillDecaySecs },
      { "Base View", "Base View Decay", VIEW_POLICY_STOCK_BASE,
        &gameFrontViewBasePolicy, &gameFrontViewBaseDecaySecs },
      { "Ally View", "Ally View Decay", VIEW_POLICY_STOCK_ALLY,
        &gameFrontViewAllyPolicy, &gameFrontViewAllyDecaySecs },
    };
    intToStr(VIEW_DECAY_DEFAULT_SECS, def, sizeof(def));
    for (int vi = 0; vi < (int)(sizeof(viewPrefs) / sizeof(viewPrefs[0])); vi++) {
      prefsGetString("GAME OPTIONS", viewPrefs[vi].policyKey,
                     viewPolicyPrefWord(viewPrefs[vi].policyStock), buff,
                     FILENAME_MAX);
      *viewPrefs[vi].policyOut =
          viewPolicyFromPrefWord(buff, viewPrefs[vi].policyStock);
      prefsGetString("GAME OPTIONS", viewPrefs[vi].decayKey, def, buff,
                     FILENAME_MAX);
      *viewPrefs[vi].decayOut = viewDecayClamp(atoi(buff));
    }
    prefsGetString("GAME OPTIONS", "Classic Mode", "No", buff, FILENAME_MAX);
    gameFrontClassicMode = YESNO_TO_TRUEFALSE(buff[0]);
    prefsGetString("GAME OPTIONS", "Allies In Trees", "No", buff, FILENAME_MAX);
    gameFrontAlliesInTrees = YESNO_TO_TRUEFALSE(buff[0]);
    /* Same derivation as the three above: the INI default word and the
     * fallback both come from OVERVIEW_WINDOW_STOCK. */
    prefsGetString("GAME OPTIONS", "Overview Window",
                   overviewWindowPrefWord(OVERVIEW_WINDOW_STOCK), buff,
                   FILENAME_MAX);
    gameFrontOverviewWindow =
        overviewWindowFromPrefWord(buff, OVERVIEW_WINDOW_STOCK);
    /* Stored Yes/No rather than a mode word, so the INI default is the
     * answer LINE_OF_SIGHT_STOCK gives to "does anything block sight". */
    prefsGetString("GAME OPTIONS", "Line Of Sight",
                   (LINE_OF_SIGHT_STOCK != lineOfSightOff) ? "Yes" : "No",
                   buff, FILENAME_MAX);
    gameFrontLineOfSight = YESNO_TO_TRUEFALSE(buff[0])
                               ? lineOfSightBuildingsAndTrees
                               : lineOfSightOff;

    /* The remembered custom set, read whether or not it is the one in
     * force: the lobby needs it to put "Custom: ..." in the dropdown and
     * to go back to it when the host picks that row. An absent "Custom
     * Pill View" is how an INI that predates the presets — or one whose
     * owner has never left the presets — says there is no custom set;
     * every other Custom key then defaults to the value next to it, so a
     * half-written section still reads as something sane. */
    gameFrontGetVisibilitySettings(&gameFrontVisibilityCustom);
    prefsGetString("GAME OPTIONS", "Custom Pill View", "", buff, FILENAME_MAX);
    gameFrontVisibilityCustomSaved = (buff[0] != '\0');
    if (gameFrontVisibilityCustomSaved) {
      static const struct {
        const char  *policyKey;
        const char  *decayKey;
        ViewCategory cat;
      } customPrefs[] = {
        { "Custom Pill View", "Custom Pill View Decay", viewCategoryPill },
        { "Custom Base View", "Custom Base View Decay", viewCategoryBase },
        { "Custom Ally View", "Custom Ally View Decay", viewCategoryAlly },
      };
      for (int ci = 0; ci < (int)(sizeof(customPrefs) / sizeof(customPrefs[0]));
           ci++) {
        ViewCategory cat = customPrefs[ci].cat;
        prefsGetString("GAME OPTIONS", customPrefs[ci].policyKey,
                       viewPolicyPrefWord(
                           (int)gameFrontVisibilityCustom.policy[cat]),
                       buff, FILENAME_MAX);
        gameFrontVisibilityCustom.policy[cat] = (uint8_t)viewPolicyFromPrefWord(
            buff, (int)gameFrontVisibilityCustom.policy[cat]);
        intToStr((int)gameFrontVisibilityCustom.decaySecs[cat], def,
                 sizeof(def));
        prefsGetString("GAME OPTIONS", customPrefs[ci].decayKey, def, buff,
                       FILENAME_MAX);
        gameFrontVisibilityCustom.decaySecs[cat] =
            (uint16_t)viewDecayClamp(atoi(buff));
      }
      prefsGetString("GAME OPTIONS", "Custom Classic Mode",
                     TRUEFALSE_TO_STR(gameFrontVisibilityCustom.classicMode),
                     buff, FILENAME_MAX);
      gameFrontVisibilityCustom.classicMode = YESNO_TO_TRUEFALSE(buff[0]);
      prefsGetString("GAME OPTIONS", "Custom Allies In Trees",
                     TRUEFALSE_TO_STR(gameFrontVisibilityCustom.alliesInTrees),
                     buff, FILENAME_MAX);
      gameFrontVisibilityCustom.alliesInTrees = YESNO_TO_TRUEFALSE(buff[0]);
      prefsGetString("GAME OPTIONS", "Custom Overview Window",
                     overviewWindowPrefWord(
                         (int)gameFrontVisibilityCustom.overviewWindow),
                     buff, FILENAME_MAX);
      gameFrontVisibilityCustom.overviewWindow =
          (uint8_t)overviewWindowFromPrefWord(
              buff, (int)gameFrontVisibilityCustom.overviewWindow);
      prefsGetString("GAME OPTIONS", "Custom Line Of Sight",
                     TRUEFALSE_TO_STR(gameFrontVisibilityCustom.lineOfSight !=
                                      (uint8_t)lineOfSightOff),
                     buff, FILENAME_MAX);
      gameFrontVisibilityCustom.lineOfSight =
          YESNO_TO_TRUEFALSE(buff[0]) ? (uint8_t)lineOfSightBuildingsAndTrees
                                      : (uint8_t)lineOfSightOff;
    }

    /* What the host last chose, which is what a game hosted from here
     * starts on. The seven keys above have already put the last values on
     * the globals; this writes the chosen set over them, so a preset that
     * is later given a different value follows the host's choice rather
     * than the values it happened to have when they made it. An INI with
     * no "Visibility Preset" key — every INI that predates this — reads as
     * whichever preset the seven values already add up to, so nothing
     * moves on the first run. */
    {
      VisibilitySettings cur;
      VisibilityPreset   fallback;
      VisibilityPreset   chosen;

      gameFrontGetVisibilitySettings(&cur);
      fallback = visibilityPresetMatch(&cur);
      prefsGetString("GAME OPTIONS", "Visibility Preset",
                     visibilityPresetPrefWord(fallback), buff, FILENAME_MAX);
      chosen = visibilityPresetFromPrefWord(buff, fallback);
      gameFrontVisibilityPreset = (int)chosen;
      if (chosen == visibilityPresetCustom) {
        if (gameFrontVisibilityCustomSaved) {
          gameFrontPutVisibilitySettings(&gameFrontVisibilityCustom);
        }
      } else {
        VisibilitySettings want;
        if (visibilityPresetSettings(chosen, &want)) {
          /* A preset says nothing about the decay seconds, so the host's
           * own stay where the keys above left them. */
          want.decaySecs[viewCategoryPill] = cur.decaySecs[viewCategoryPill];
          want.decaySecs[viewCategoryBase] = cur.decaySecs[viewCategoryBase];
          want.decaySecs[viewCategoryAlly] = cur.decaySecs[viewCategoryAlly];
          gameFrontPutVisibilitySettings(&want);
        }
      }
    }
  }

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
  /* Through the setters, not into the globals: master has to reach the voice
     module as well as the mixer, and a value that only landed in the global
     would leave voice at the wrong gain until the slider was touched.  Both
     setters clamp to 0-100, so a hand-edited file cannot get past them. */
  prefsGetString("MENU", "Sound Volume", "50", buff, FILENAME_MAX);
  windowSetSoundVolume(atoi(buff));
  prefsGetString("MENU", "Master Volume", "100", buff, FILENAME_MAX);
  windowSetMasterVolume(atoi(buff));
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
  prefsGetString("MENU", "Show Map Overview", "No", buff, FILENAME_MAX);
  gameFrontShowMapOverview = YESNO_TO_TRUEFALSE(buff[0]);
  /* The key keeps the name it has always had so settings in existing player
     files carry over; what it feeds now drives full screen for the whole app,
     not just the in-window map view a game opens with. */
  prefsGetString("MENU", "Show Full Screen Map", "No", buff, FILENAME_MAX);
  gameFrontFullScreen = YESNO_TO_TRUEFALSE(buff[0]);
  /* The full screen map's three HUD panels: how see-through each is drawn,
     and whether the newswire hides itself between messages. Each falls back
     to what sdl3draw already holds, which is its own default, and sdl3draw
     clamps the transparencies it is given. */
  intToStr(sdl3DrawGetNewswireTransparency(), def, sizeof(def));
  prefsGetString("SETTINGS", "Newswire Transparency", def, buff, FILENAME_MAX);
  sdl3DrawSetNewswireTransparency(atoi(buff));
  prefsGetString("SETTINGS", "Newswire Auto Hide",
                 TRUEFALSE_TO_STR(sdl3DrawGetNewswireAutoHide()), buff,
                 FILENAME_MAX);
  sdl3DrawSetNewswireAutoHide(YESNO_TO_TRUEFALSE(buff[0]));
  intToStr(sdl3DrawGetBuildPanelTransparency(), def, sizeof(def));
  prefsGetString("SETTINGS", "Build Panel Transparency", def, buff,
                 FILENAME_MAX);
  sdl3DrawSetBuildPanelTransparency(atoi(buff));
  intToStr(sdl3DrawGetStatusPanelTransparency(), def, sizeof(def));
  prefsGetString("SETTINGS", "Status Panel Transparency", def, buff,
                 FILENAME_MAX);
  sdl3DrawSetStatusPanelTransparency(atoi(buff));
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

  /* Lobby window size and column split */
  prefsGetString("WINDOW", "Lobby Width", "-1", buff, FILENAME_MAX);
  gameFrontLobbyW = atoi(buff);
  prefsGetString("WINDOW", "Lobby Height", "-1", buff, FILENAME_MAX);
  gameFrontLobbyH = atoi(buff);
  prefsGetString("WINDOW", "Lobby Split", "0", buff, FILENAME_MAX);
  gameFrontLobbySplit = (float)atof(buff);
  {
    /* The post-game split defaults to the map one, so a WinBolo.json written
       before the two views had separate keys comes up on the width the player
       already set and neither view jumps on the first launch. */
    char splitDefault[FILENAME_MAX];
    strcpy(splitDefault, buff);
    prefsGetString("WINDOW", "Lobby Split Recap", splitDefault, buff, FILENAME_MAX);
    gameFrontLobbySplitRecap = (float)atof(buff);
  }

  /* Map overview pop-out: size, position, and the camera state it reopens
     with. -1 for either coordinate means no saved position, so the window
     lands wherever the OS puts it. */
  prefsGetString("WINDOW", "Overview Width",  "640", buff, FILENAME_MAX);
  gameFrontOverviewW = atoi(buff);
  prefsGetString("WINDOW", "Overview Height", "640", buff, FILENAME_MAX);
  gameFrontOverviewH = atoi(buff);
  prefsGetString("WINDOW", "Overview X", "-1", buff, FILENAME_MAX);
  gameFrontOverviewX = atoi(buff);
  prefsGetString("WINDOW", "Overview Y", "-1", buff, FILENAME_MAX);
  gameFrontOverviewY = atoi(buff);
  prefsGetString("WINDOW", "Overview Zoom", "2", buff, FILENAME_MAX);
  gameFrontOverviewZoom = (float)atof(buff);
  prefsGetString("WINDOW", "Overview Follow", "Yes", buff, FILENAME_MAX);
  gameFrontOverviewFollow = YESNO_TO_TRUEFALSE(buff[0]);

  /* The scenario panel's place inside the main window, the size the player
     has dragged it to and how opaque its backing is. -1 for either
     coordinate means it has never been moved, so it opens at the top-right
     of the game view; -1 for the scale or the alpha means that one has never
     been touched, so the panel opens at the size the game's zoom alone gives
     it and at the backing it has always had.

     The scale and the alpha are clamped here and not only on the drag that
     writes them. The preferences file is a text file a player can edit, and a
     scale out of range would open the panel bigger than the screen or too
     small to get a pointer onto a corner of, neither of which leaves anything
     to drag it back with. */
  prefsGetString("WINDOW", "Scenario Panel X", "-1", buff, FILENAME_MAX);
  gameFrontScnPanelX = atoi(buff);
  prefsGetString("WINDOW", "Scenario Panel Y", "-1", buff, FILENAME_MAX);
  gameFrontScnPanelY = atoi(buff);
  prefsGetString("WINDOW", "Scenario Panel Scale", "-1", buff, FILENAME_MAX);
  gameFrontScnPanelScale = atoi(buff);
  if (gameFrontScnPanelScale != -1) {
    if (gameFrontScnPanelScale < SCN_PANEL_SCALE_MIN) {
      gameFrontScnPanelScale = SCN_PANEL_SCALE_MIN;
    } else if (gameFrontScnPanelScale > SCN_PANEL_SCALE_MAX) {
      gameFrontScnPanelScale = SCN_PANEL_SCALE_MAX;
    }
  }
  prefsGetString("WINDOW", "Scenario Panel Alpha", "-1", buff, FILENAME_MAX);
  gameFrontScnPanelAlpha = atoi(buff);
  if (gameFrontScnPanelAlpha != -1) {
    if (gameFrontScnPanelAlpha < 0) {
      gameFrontScnPanelAlpha = 0;
    } else if (gameFrontScnPanelAlpha > 100) {
      gameFrontScnPanelAlpha = 100;
    }
  }

  prefsGetString("MENU", "Message Label Size", "1", buff, FILENAME_MAX);
  labelMsg = atoi(buff);
  prefsGetString("MENU", "Tank Label Size", "1", buff, FILENAME_MAX);
  labelTank = atoi(buff);

#if defined(WINBOLO_VOICE)
  /* Voice.  Applied straight onto the running voice module, which is already
     up by the time this runs — winbolo.c brings it up before gameFrontStart. */
  /* Off unless the prefs file says otherwise, so a fresh install joins
     without opening a microphone.  A player who has already chosen has
     Enabled written in their file and keeps whatever they chose. */
  prefsGetString("VOICE", "Enabled", "No", buff, FILENAME_MAX);
  windowSetVoiceEnabled(YESNO_TO_TRUEFALSE(buff[0]));
  prefsGetString("VOICE", "Mode", VOICE_MODE_NAME_PTT, buff, FILENAME_MAX);
  if (strcmp(buff, VOICE_MODE_NAME_OFF) == 0) {
    windowSetVoiceMode(VOICE_MODE_OFF);
  } else if (strcmp(buff, VOICE_MODE_NAME_OPEN) == 0) {
    windowSetVoiceMode(VOICE_MODE_OPEN);
  } else {
    windowSetVoiceMode(VOICE_MODE_PTT);
  }
  /* Clamp on read to the ranges the sliders offer, so a hand-edited file
     cannot leave the microphone dead or the other players deafening. */
  prefsGetString("VOICE", "Mic Gain", "1.0", buff, FILENAME_MAX);
  {
    float mg = (float)atof(buff);
    if (!(mg >= 0.0f && mg <= 4.0f)) mg = 1.0f;
    windowSetVoiceMicGain(mg);
  }
  prefsGetString("VOICE", "Voice Volume", "1.0", buff, FILENAME_MAX);
  {
    float vv = (float)atof(buff);
    if (!(vv >= 0.0f && vv <= 2.0f)) vv = 1.0f;
    windowSetVoiceVolume(vv);
  }
  /* The chosen audio devices, by display name — "" is the system default,
     and so is a name nothing present answers to. Read into a bounded buffer
     and truncated, because the name is free-form and comes from the driver.

     Their own section, because it is device-local and the rest of VOICE is
     not: a device name saved on a desktop means nothing on a Steam Deck, and
     syncing it would overwrite the Deck's own choice on every adopt. The
     section is listed in kDeviceLocalSections in prefs.c, which is what keeps
     it out of the upload. */
  prefsGetString("VOICE.DEVICE", "Recording Device", "", buff, FILENAME_MAX);
  windowSetVoiceRecordingDevice(buff);
  prefsGetString("VOICE.DEVICE", "Playback Device", "", buff, FILENAME_MAX);
  windowSetVoicePlaybackDevice(buff);
  prefsGetString("VOICE", "Tank Icons", "Yes", buff, FILENAME_MAX);
  windowSetShowTankMicIcons(YESNO_TO_TRUEFALSE(buff[0]));
#if defined(WINBOLO_VOICE_AEC)
  prefsGetString("VOICE", "Echo Cancel", "Yes", buff, FILENAME_MAX);
  windowSetVoiceEchoCancel(YESNO_TO_TRUEFALSE(buff[0]));
#endif
#endif

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
  prefsSetString("HOSTING", "Run Map Scripts",
                            TRUEFALSE_TO_STR(gameFrontHostingScripts));
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
  prefsSetString("HOSTING", "Script Upload Policy",
                 scriptUploadPolicyWord(
                     (ScriptUploadPolicy)gameFrontHostingScriptUploadPolicy));
  prefsSetString("HOSTING", "Share Scripts",
                            TRUEFALSE_TO_STR(gameFrontHostingShareScripts));
  prefsSetString("HOSTING", "Script Upload Dir",
                 gameFrontHostingScriptUploadDir);
  intToStr(gameFrontHostingScriptUploadMaxFiles, buff, sizeof(buff));
  prefsSetString("HOSTING", "Script Upload Max Files", buff);
  intToStr(gameFrontHostingScriptUploadMaxStorage, buff, sizeof(buff));
  prefsSetString("HOSTING", "Script Upload Max Storage", buff);
  prefsSetString("HOSTING", "Mod Dir", gameFrontHostingScenarioDir);
  prefsSetString("HOSTING", "Logging",
                            TRUEFALSE_TO_STR(gameFrontHostingLogging));
  prefsSetString("HOSTING", "Log Dir", gameFrontHostingLogDir);
  prefsSetString("HOSTING", "Serve Replays",
                            TRUEFALSE_TO_STR(gameFrontHostingServeReplays));
  prefsSetString("HOSTING", "Voice",
                 gameFrontHostingVoiceMode == serverVoiceOff       ? "Off"
                 : gameFrontHostingVoiceMode == serverVoiceProximity ? "Proximity"
                                                                     : "On");

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
  intToStr(keys->kiBaseView, buff, sizeof(buff));
  prefsSetString("KEYS", "Base View", buff);
  intToStr(keys->kiOverviewZoom, buff, sizeof(buff));
  prefsSetString("KEYS", "Overview Zoom", buff);
  intToStr(keys->kiOverviewFollow, buff, sizeof(buff));
  prefsSetString("KEYS", "Overview Follow", buff);
  intToStr(keys->kiOverviewZoomIn, buff, sizeof(buff));
  prefsSetString("KEYS", "Overview Zoom In", buff);
  intToStr(keys->kiOverviewZoomOut, buff, sizeof(buff));
  prefsSetString("KEYS", "Overview Zoom Out", buff);

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

  /* Push to talk — 0 is unbound, and round-trips as such. */
  intToStr(keys->kiPushToTalk, buff, sizeof(buff));
  prefsSetString("KEYS", "Push To Talk", buff);

  /* Mute microphone — 0 is unbound, and round-trips as such. */
  intToStr(keys->kiMuteMic, buff, sizeof(buff));
  prefsSetString("KEYS", "Mute Mic", buff);

  /* Smart ping — the menu chords, then the per-kind direct ones. */
  {
    int pi;
    for (pi = 0; pi < PING_BIND_SLOTS; pi++) {
      char name[32];
      snprintf(name, sizeof(name), "Ping %d", pi + 1);
      intToStr(keys->kiPing[pi], buff, sizeof(buff));
      prefsSetString("KEYS", name, buff);
    }
    for (pi = 0; pi < PING_BIND_DIRECT_SLOTS; pi++) {
      char name[32];
      snprintf(name, sizeof(name), "Ping Direct %d", pi + 1);
      intToStr(keys->kiPingDirect[pi], buff, sizeof(buff));
      prefsSetString("KEYS", name, buff);
    }
  }

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

  /* Skin id the player chose, "" for the built-in assets.  The choice, not
     what loaded: a skin that cannot be read right now stays saved. */
  prefsSetString("SETTINGS", "Skin", skinGetRequested());

  /* Graphics settings.  Same keys the loader reads. */
  intToStr((int)gfxGetTileDetail(), buff, sizeof(buff));
  prefsSetString("SETTINGS", "TileDetail", buff);
  intToStr((int)gfxGetAnimSmoothness(), buff, sizeof(buff));
  prefsSetString("SETTINGS", "AnimSmoothness", buff);
  prefsSetString("SETTINGS", "SmoothShells", TRUEFALSE_TO_STR(gfxGetSmoothShells()));
  intToStr((int)gfxGetTextureFilter(), buff, sizeof(buff));
  prefsSetString("SETTINGS", "TextureFilter", buff);
  prefsSetString("SETTINGS", "SimplifiedZoomOut",
                 TRUEFALSE_TO_STR(gfxGetSimplifiedZoomOut()));
  prefsSetString("SETTINGS", "SimplifiedOverviewOnly",
                 TRUEFALSE_TO_STR(gfxGetSimplifiedOverviewOnly()));
  intToStr((int)gfxGetFogStyle(), buff, sizeof(buff));
  prefsSetString("SETTINGS", "FogStyle", buff);

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

  /* Options. The game type, computer tanks and hidden mines are not
   * written here: gameFrontRememberGameType and its two siblings write
   * them when the host picks one, so a player who never picked keeps no
   * key and goes on getting the current stock value. */
  intToStr(startDelay, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "Start Delay", buff);
  intToStr(timeLen, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "Time Length", buff);
  prefsSetString("GAME OPTIONS", "Auto Slowdown", TRUEFALSE_TO_STR(useAutoslow));
  prefsSetString("GAME OPTIONS", "Auto Show-Hide Gunsight", TRUEFALSE_TO_STR(useAutohide));
  prefsSetString("GAME OPTIONS", "Pill View",
                 viewPolicyPrefWord(gameFrontViewPillPolicy));
  prefsSetString("GAME OPTIONS", "Base View",
                 viewPolicyPrefWord(gameFrontViewBasePolicy));
  prefsSetString("GAME OPTIONS", "Ally View",
                 viewPolicyPrefWord(gameFrontViewAllyPolicy));
  intToStr(gameFrontViewPillDecaySecs, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "Pill View Decay", buff);
  intToStr(gameFrontViewBaseDecaySecs, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "Base View Decay", buff);
  intToStr(gameFrontViewAllyDecaySecs, buff, sizeof(buff));
  prefsSetString("GAME OPTIONS", "Ally View Decay", buff);
  prefsSetString("GAME OPTIONS", "Classic Mode",
                 TRUEFALSE_TO_STR(gameFrontClassicMode));
  prefsSetString("GAME OPTIONS", "Allies In Trees",
                 TRUEFALSE_TO_STR(gameFrontAlliesInTrees));
  prefsSetString("GAME OPTIONS", "Overview Window",
                 overviewWindowPrefWord(gameFrontOverviewWindow));
  prefsSetString("GAME OPTIONS", "Line Of Sight",
                 TRUEFALSE_TO_STR((gameFrontLineOfSight != lineOfSightOff)));

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
  intToStr(windowMasterVolume, buff, sizeof(buff));
  prefsSetString("MENU", "Master Volume", buff);
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
  prefsSetString("MENU", "Show Map Overview",
                 TRUEFALSE_TO_STR(gameFrontShowMapOverview));
  prefsSetString("MENU", "Show Full Screen Map",
                 TRUEFALSE_TO_STR(gameFrontFullScreen));
  /* The full screen map's HUD panels. */
  intToStr(sdl3DrawGetNewswireTransparency(), buff, sizeof(buff));
  prefsSetString("SETTINGS", "Newswire Transparency", buff);
  prefsSetString("SETTINGS", "Newswire Auto Hide",
                 TRUEFALSE_TO_STR(sdl3DrawGetNewswireAutoHide()));
  intToStr(sdl3DrawGetBuildPanelTransparency(), buff, sizeof(buff));
  prefsSetString("SETTINGS", "Build Panel Transparency", buff);
  intToStr(sdl3DrawGetStatusPanelTransparency(), buff, sizeof(buff));
  prefsSetString("SETTINGS", "Status Panel Transparency", buff);
  /* Window settings (zoom, custom size, position, dialog position) — flush immediately,
     bypassing debounce since this is the shutdown save path. */
  gameFrontFlushWindowSettings();
  intToStr(labelMsg, buff, sizeof(buff));
  prefsSetString("MENU", "Message Label Size", buff);
  intToStr(labelTank, buff, sizeof(buff));
  prefsSetString("MENU", "Tank Label Size", buff);

#if defined(WINBOLO_VOICE)
  /* Voice — read back out of the running voice module, which is what holds
     these while the game is up. */
  prefsSetString("VOICE", "Enabled", TRUEFALSE_TO_STR(windowGetVoiceEnabled()));
  {
    int vm = windowGetVoiceMode();
    prefsSetString("VOICE", "Mode",
                   vm == VOICE_MODE_OFF    ? VOICE_MODE_NAME_OFF
                   : vm == VOICE_MODE_OPEN ? VOICE_MODE_NAME_OPEN
                                           : VOICE_MODE_NAME_PTT);
  }
  snprintf(buff, sizeof(buff), "%.2f", windowGetVoiceMicGain());
  prefsSetString("VOICE", "Mic Gain", buff);
  snprintf(buff, sizeof(buff), "%.2f", windowGetVoiceVolume());
  prefsSetString("VOICE", "Voice Volume", buff);
  /* Written back even when the device is not plugged in at the moment, so
     the choice survives it being away. In the device-local section, as on the
     read side. */
  prefsSetString("VOICE.DEVICE", "Recording Device",
                 windowGetVoiceRecordingDevice());
  prefsSetString("VOICE.DEVICE", "Playback Device",
                 windowGetVoicePlaybackDevice());
  prefsSetString("VOICE", "Tank Icons", TRUEFALSE_TO_STR(windowGetShowTankMicIcons()));
#if defined(WINBOLO_VOICE_AEC)
  prefsSetString("VOICE", "Echo Cancel", TRUEFALSE_TO_STR(windowGetVoiceEchoCancel()));
#endif
#endif

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

  intToStr(gameFrontLobbyW, buff, sizeof(buff));
  prefsSetString("WINDOW", "Lobby Width", buff);
  intToStr(gameFrontLobbyH, buff, sizeof(buff));
  prefsSetString("WINDOW", "Lobby Height", buff);
  /* Two decimals: the lobby's change test uses a 0.02 logical-pixel
     epsilon, so the stored value must round finer than that or every
     launch would re-write it. */
  SDL_snprintf(buff, sizeof(buff), "%.2f", (double)gameFrontLobbySplit);
  prefsSetString("WINDOW", "Lobby Split", buff);
  SDL_snprintf(buff, sizeof(buff), "%.2f", (double)gameFrontLobbySplitRecap);
  prefsSetString("WINDOW", "Lobby Split Recap", buff);

  intToStr(gameFrontOverviewW, buff, sizeof(buff));
  prefsSetString("WINDOW", "Overview Width", buff);
  intToStr(gameFrontOverviewH, buff, sizeof(buff));
  prefsSetString("WINDOW", "Overview Height", buff);
  intToStr(gameFrontOverviewX, buff, sizeof(buff));
  prefsSetString("WINDOW", "Overview X", buff);
  intToStr(gameFrontOverviewY, buff, sizeof(buff));
  prefsSetString("WINDOW", "Overview Y", buff);
  /* The zoom is one of a handful of ladder rungs — 0.5 to 4, the finest gap
     being 0.25 — so two decimals name every one of them exactly. */
  SDL_snprintf(buff, sizeof(buff), "%.2f", (double)gameFrontOverviewZoom);
  prefsSetString("WINDOW", "Overview Zoom", buff);
  prefsSetString("WINDOW", "Overview Follow",
                 TRUEFALSE_TO_STR(gameFrontOverviewFollow));

  intToStr(gameFrontScnPanelX, buff, sizeof(buff));
  prefsSetString("WINDOW", "Scenario Panel X", buff);
  intToStr(gameFrontScnPanelY, buff, sizeof(buff));
  prefsSetString("WINDOW", "Scenario Panel Y", buff);
  intToStr(gameFrontScnPanelScale, buff, sizeof(buff));
  prefsSetString("WINDOW", "Scenario Panel Scale", buff);
  intToStr(gameFrontScnPanelAlpha, buff, sizeof(buff));
  prefsSetString("WINDOW", "Scenario Panel Alpha", buff);

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

