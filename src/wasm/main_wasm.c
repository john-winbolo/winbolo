/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * main_wasm.c - Emscripten/WASM entry point for WinBolo game client
 *
 * Replaces gui/sdl3/winbolo.c for the WASM build.
 * Uses emscripten_set_main_loop() instead of a blocking event loop,
 * and frame-based tick accumulation instead of SDL_AddTimer.
 */

#include <SDL3/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

#include <emscripten.h>
#include <emscripten/html5.h>

#include "bolo_rand.h"
#include "client_render.h"
#include "client_frontend_tick.h"
#include "client_sim.h"
#include "frontend.h"
#include "playername_validate.h"
#include "client_net.h"
#include "gui_message.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../gui/draw.h"
#include "../gui/gamefront.h"
#include "../common/prefs.h"
#include "../gui/input.h"
#include "../gui/lang.h"
#include "../gui/sound.h"
#include "../gui/ui_mode.h"
#include "../gui/voice.h"
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/input_gamepad.h"
#include "../gui/sdl3/build_cursor.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../gui/sdl3/dialogs/imgui_messagebox.h"
#include "../gui/sdl3/dialogs/imgui_tutorial_overlay.h"
#include "server_sim.h"
#include "tutorial.h"
#include "cJSON.h"

#include <sys/stat.h>

extern ClientSim *humanSim;

/* -------------------------------------------------------
 * Globals declared by winbolo.h
 * ------------------------------------------------------- */

void windowStartTutorial(void);
bool isTutorial = FALSE;

int frameRate = FRAME_RATE_30;
static int frameRateTime = (int)(MILLISECONDS / FRAME_RATE_30) - 1;

bool showGunsight = TRUE;  /* WASM default: gunsight on unless synced prefs override */
bool soundEffects = TRUE;
bool backgroundSound = TRUE;
bool useSoundKeepalive = FALSE;
int  soundVolume = 50;         /* MENU / Sound Volume — the game sounds */
int  windowMasterVolume = 100; /* MENU / Master Volume — sounds and voice */
bool allowNewPlayers = TRUE;

bool showNewswireMessages = TRUE;
bool showAssistantMessages = TRUE;
bool showAIMessages = FALSE;
bool showNetworkStatusMessages = TRUE;
bool showNetworkDebugMessages = FALSE;

bool autoScrollingEnabled = TRUE;  /* WASM default: autoscroll on unless synced prefs override */
bool smoothScrollingEnabled = FALSE;  /* WASM: arrow-key smooth scroll inactive */
bool letterboxBarsGray = FALSE;       /* gray vs black letterbox bars (sdl3draw) */
BYTE zoomFactor = ZOOM_FACTOR_DOUBLE;

bool showPillLabels = FALSE;
bool showBaseLabels = FALSE;
bool labelSelf = TRUE;
labelLen labelMsg = lblShort;
labelLen labelTank = lblShort;

bool isInMenu = FALSE;

keyItems keys;

/* Timing */
static DWORD dwSysFrameTotal = 0;
static DWORD dwSysFrame = 0;
static DWORD dwSysGameTotal = 0;
static DWORD dwSysGame = 0;
static DWORD dwSysBrainTotal = 0;
static DWORD dwSysBrain = 0;

static bool drawBusy = FALSE;
static bool doingTutorial = FALSE;
static bool winboloQuit = FALSE;
static bool finishedLoop = FALSE;
static bool showAllianceReq = TRUE;

/* Terminal connection-failure state. Set on any unrecoverable connection
 * problem (can't reach the relay, version mismatch, used/expired join code,
 * mid-game disconnect, or an unknown transport error). Once set, the main loop
 * shows one error dialog and freezes the game — it stops ticking and sending,
 * so we never re-open the WebSocket (join codes are single-use; there is
 * nothing to reconnect to) and never present a blank screen. */
static bool s_connFailed = FALSE;
static bool s_connErrorShown = FALSE;
static char s_connReason[256] = "";

/* Record a terminal connection failure. reason may be NULL/empty, in which case
 * a generic message is used. Called from gamefront_wasm.c (initial connect) and
 * from the main loop (mid-game disconnect). Idempotent — first reason wins. */
void wasmReportConnectFailure(const char *reason) {
  if (s_connFailed) return;
  if (reason != NULL && reason[0] != '\0') {
    strncpy(s_connReason, reason, sizeof(s_connReason) - 1);
    s_connReason[sizeof(s_connReason) - 1] = '\0';
  } else {
    strncpy(s_connReason, langGetText(STR_WEB_CONNECT_FAILED),
            sizeof(s_connReason) - 1);
    s_connReason[sizeof(s_connReason) - 1] = '\0';
  }
  s_connFailed = TRUE;
}

static DWORD oldTick = 0;
static DWORD ttick = 0;
static time_t ticks = 0;

/* Frame-based tick accumulator (replaces SDL_AddTimer) */
static double gameTickAccum = 0.0;
static double lastFrameTime = 0.0;

/* -------------------------------------------------------
 * SDL message handler
 * ------------------------------------------------------- */
void sdl3MessageHandler(const char *message, const char *title) {
  imguiMessageBoxEx(title ? title : "WinBolo",
                    message ? message : "",
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
}

/* Mouse-wheel bridge. SDL3's emscripten backend registers its wheel callback on
 * a different target from the pointer-event shim that delivers mouse motion and
 * clicks, and in the browser build it doesn't fire — so scrolling never reached
 * the shared gunsight handler. shell.html adds its own canvas 'wheel' listener
 * and calls this, which injects a native SDL wheel event; sdl3ImguiProcessEvents
 * then handles it exactly like a real wheel (same in-game + ImGui-capture
 * gating). deltaY is the browser value (negative = scroll up); SDL uses
 * positive-up, so flip the sign to a unit step. */
extern void inputBumpGunsight(int direction);       /* gui/sdl3/input.h */
extern uint8_t inputConsumeGunsightAdj(void);       /* gui/sdl3/input.h */
extern bool sdl3ImguiWantCaptureMouse(void);        /* sdl3imgui.cpp */

EMSCRIPTEN_KEEPALIVE
void wbWasmWheel(double deltaY) {
  if (deltaY == 0.0) return;
  /* Only adjust the gunsight in a running game, and not while the wheel is over
   * an ImGui panel/dialog (mirrors the shared desktop handler's gating). Browser
   * deltaY is negative when scrolling up; the gunsight increases on scroll-up. */
  if (!humanSim || clientSimGetNetStatus(humanSim) != netRunning) return;
  if (sdl3ImguiWantCaptureMouse()) return;
  inputBumpGunsight(deltaY < 0.0 ? +1 : -1);
}

extern void inputResetHeldKeys(void);               /* gui/sdl3/input.h */

/* Held-key safety reset for browser blur paths that never reach SDL. The game
 * polls SDL_GetKeyboardState; if the browser stops delivering keyup (tab
 * hidden, window blurred) a movement key reads as held forever and the client
 * keeps sending the turn button. shell.html calls this on visibilitychange
 * (hidden) and on window blur; inputResetHeldKeys clears SDL's key array and
 * the input module's latched edge state. */
EMSCRIPTEN_KEEPALIVE
void wbWasmResetHeldKeys(void) {
  inputResetHeldKeys();
}

/* -------------------------------------------------------
 * windowRunGameTick — game logic using transport
 * ------------------------------------------------------- */
static void windowRunGameTick(ClientSim *cs) {
  static bool inBrain = FALSE;
  static BYTE t2 = 0;
  static int trackerTime = 11500;
  bool used = FALSE;
  bool brainRunning;

  brainRunning = brainHandlerIsBrainRunning();

  if (!clientSimHasTransport(cs) || doingTutorial) {
    return;
  }

  /* One keys/game/lobby half-step lives in the shared client-frontend tick
   * core (both desktop and web call it); the brain and per-second stat
   * rollover below stay here in the single-threaded driver. */
  if (clientFrontRunTickStep(cs)) {
    t2++;
    trackerTime++;
    ticks++;
    used = TRUE;
  }

  /* AI */
  if (used == TRUE && inBrain == FALSE && brainRunning == TRUE &&
      clientSimGetNetStatus(cs) != netFailed) {
    clientMutexWaitFor();
    inBrain = TRUE;
    clientMutexRelease();
    ttick = SDL_GetTicks();
    brainHandlerRun();
    dwSysBrain += SDL_GetTicks() - ttick;
    clientMutexWaitFor();
    inBrain = FALSE;
    clientMutexRelease();
  }

  if (t2 >= 19) {
    dwSysFrameTotal = dwSysFrame;
    dwSysGameTotal = dwSysGame;
    dwSysBrainTotal = dwSysBrain;
    dwSysBrain = 0;
    dwSysFrame = 0;
    dwSysGame = 0;
    t2 = 0;
  }
}

/* -------------------------------------------------------
 * main_loop_iteration — called by emscripten_set_main_loop
 * ------------------------------------------------------- */
void frontEndTutorialNotePresentedFrame(void);
static void tutorialRespawnPoll(void);

/* Defined further down with the other winbolo.h entry points. */
void windowLeaveGame(void);

/* Cloud-prefs bridge (prefs_bridge_wasm.c). */
void wbPrefsSyncNow(void);
void wbPrefsPumpUpload(uint64_t nowMs);

static void main_loop_iteration(void) {
  DWORD tick;
  ClientSim *cs = humanSim;

  /* Process events */
  sdl3ImguiProcessEvents(cs);

  /* Detect a mid-game terminal disconnect (server shutdown / dropped /
   * unrecoverable error). The initial-connect failure path sets s_connFailed
   * directly from gameFrontStart, so this only needs to catch failures that
   * arise while running. */
  if (!s_connFailed && cs != NULL && clientSimHasTransport(cs) &&
      gameFrontGetServerSim() == NULL) {  /* UDP only — not local single-player */
    ClientConnectState st = clientSimGetConnectState(cs);
    if (st == CLIENT_CONNECT_ERROR || st == CLIENT_CONNECT_SERVER_SHUTDOWN ||
        st == CLIENT_CONNECT_KICKED) {
      clientSimConnectionLost(cs);
      /* A kick sets no connectErrorReason; show the same message the
       * desktop lobby loop does (STR_DLGLOBBY_KICKED) instead of the
       * generic could-not-connect fallback. */
      if (st == CLIENT_CONNECT_KICKED) {
        wasmReportConnectFailure(langGetText(STR_DLGLOBBY_KICKED));
      } else {
        wasmReportConnectFailure(clientSimGetConnectErrorReason(cs));
      }
    }
  }

  /* On the first frame after a terminal failure, raise the error dialog. The
   * frozen state below keeps rendering without ticking or sending for the few
   * frames that run before the browser unloads the page.
   *
   * Dismissing it navigates back to the page the game launched from: there is
   * no welcome screen to fall back to in the browser build, so without this the
   * player is left on the cleared frame with only the menu bar over it. The
   * latch stops this branch re-arming while the navigation completes. */
  if (s_connFailed && !s_connErrorShown) {
    imguiMessageBoxEx(DIALOG_BOX_TITLE, s_connReason, IMGUI_MSG_ERROR,
                      IMGUI_MSG_OK);
    s_connErrorShown = TRUE;
    windowLeaveGame();
  }

  /* Game tick accumulation (replaces SDL_AddTimer).
   *
   * Sim ticks owed since the last rendered frame are derived from wall-clock
   * time.  Two kinds of gap need different handling:
   *
   *   - Ordinary jank (a JS GC pause, a couple of dropped frames): a gap of
   *     tens to a couple hundred ms.  Replay it, but spread the catch-up over
   *     a few frames (MAX_CATCHUP) so no single frame hitches.  This keeps a
   *     networked game's transport->tick() calls flowing through a brief
   *     stall instead of skipping packet send/recv.
   *
   *   - Background / suspend: when the tab is hidden the browser throttles or
   *     pauses requestAnimationFrame, so on return `gap` jumps to a second or
   *     more.  Replaying that backlog fast-forwards the game on return, and
   *     it buys nothing: past CLIENT_TIMEOUT_TICKS (~1000 ticks of no
   *     traffic) the server has already dropped us and the client has already
   *     declared SERVER_SHUTDOWN, so the owed ticks are dead either way.  In
   *     single-player there is simply nothing to catch up to.  Drop the debt
   *     and resume from real time. */
  if (!s_connFailed && clientSimHasTransport(cs)) {
    const double MAX_ELAPSED_MS  = 200.0;  /* per-frame catch-up bound (ordinary jank) */
    const double STALL_RESET_MS  = 500.0;  /* gap above this = background/suspend → drop */
    const int    MAX_CATCHUP     = 4;      /* at most 4 sim ticks per render frame */
    double now = emscripten_get_now();
    double gap = now - lastFrameTime;
    lastFrameTime = now;
    if (gap > STALL_RESET_MS) {
      gameTickAccum = 0.0;
    } else {
      gameTickAccum += (gap > MAX_ELAPSED_MS) ? MAX_ELAPSED_MS : gap;
    }

    int ticksThisFrame = 0;
    while (gameTickAccum >= GAME_TICK_LENGTH && ticksThisFrame < MAX_CATCHUP) {
      gameTickAccum -= GAME_TICK_LENGTH;
      windowRunGameTick(cs);
      ticksThisFrame++;
    }
    /* Leftover `gameTickAccum` (>= GAME_TICK_LENGTH) drains in future frames. */
  }

  /* Voice encode/decode runs here, beside the game tick and outside the
   * catch-up gate — once per rendered frame, whatever the sim owes. cs is
   * NULL until there is a connection, which the runtime expects. */
  voiceTick(cs);

  /* Render. Frozen still draws, so the error dialog lands over the last frame
   * instead of a blank screen; the lobby is the one case that clears instead
   * of rendering (see below). */
  tick = SDL_GetTicks();
  clientMutexWaitFor();
  if (finishedLoop == FALSE && !s_connFailed) {
    if (cs != NULL && clientSimIsInLobby(cs)) {
      /* In the lobby there is no game to show: clientRenderFrame would take
       * its netLobby branch and paint the whole download-screen chrome, which
       * then ghosts through the 97%-opaque ##LobbyBg the ImGui pass draws on
       * top (and shows outright in its rounded corners). The desktop lobby
       * runs its own blocking loop and clears to this same colour before
       * compositing; do the equivalent here. Nothing is lost by skipping the
       * render: the lobby tick step pumps the transport itself, and its
       * udpClientTick drains the socket. */
      SDL_Renderer *ren = sdl3DrawGetRenderer();
      if (ren) {
        SDL_SetRenderDrawColor(ren, 30, 30, 30, 255);
        SDL_RenderClear(ren);
      }
    } else {
      clientSimRenderPrepare(cs, tick);
      clientRenderFrame(cs, redraw);
    }
  } else if (s_connFailed) {
    /* Frozen: don't render the (possibly never-connected) game; clear to black
     * so the error dialog draws over a clean background, not garbage. */
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (ren) {
      SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
      SDL_RenderClear(ren);
    }
  }
  clientMutexRelease();
  /* The in-window map overview draws from the snapshot the frame above
     filled, now that the lock is off. */
  sdl3DrawFlushOverviewInWindow();
  dwSysFrame += (SDL_GetTicks() - tick);

  /* ImGui overlay + present */
  sdl3ImguiPumpAndRender(cs);
  {
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (ren) SDL_RenderPresent(ren);
  }

  /* Tutorial: count presented frames (gates the intro overlay) and poll the
   * server's once-per-run respawn message. No-ops outside tutorial mode. */
  frontEndTutorialNotePresentedFrame();
  tutorialRespawnPoll();

  /* Cloud prefs: push any setting changed this session, debounced. No-op when
   * not signed in or when nothing is sync-dirty. */
  wbPrefsPumpUpload(SDL_GetTicks());

  if (finishedLoop) {
    emscripten_cancel_main_loop();
  }
}

/* -------------------------------------------------------
 * main — Emscripten entry point
 * ------------------------------------------------------- */
/* Read a URL query parameter. Returns "" if not found. */
static const char *getUrlParam(const char *name) {
  static char buf[256];
  char js[512];
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

int main(int argc, char *argv[]) {
  const char *cmdLine = "";
  int urlZoom = 0;

  (void)argc;
  (void)argv;

  bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());

  if (argc > 1) {
    cmdLine = argv[1];
  }

  SDL_Init(0);

  /* Force the desktop UI (with the chrome background) for the browser
   * build. uiModeDetect would otherwise flip to the touch layout —
   * which omits the background bitmap — whenever a touch device is
   * present and the canvas is under 1200px wide, as desktop browsers
   * commonly report. Touch users can still switch via Ctrl+T. */
  uiModeSet(UI_MODE_DESKTOP);

  /* Parse URL query parameters: ?name=Player&zoom=2 */
  {
    const char *val;
    val = getUrlParam("zoom");
    if (val[0] != '\0') {
      urlZoom = atoi(val);
      if (urlZoom == 1 || urlZoom == 2 || urlZoom == 4) {
        zoomFactor = (BYTE)urlZoom;
        printf("[WASM] URL zoom=%d\n", urlZoom);
      } else {
        urlZoom = 0;
      }
    }
  }

  if (clientMutexCreate() == FALSE) {
    printf("[WASM] Failed to create client mutex\n");
    return 1;
  }

  /* Initialise the in-memory preferences document. The MEMFS path is a scratch
   * backing only — persistence is the WinBolo.net API (prefs_bridge_wasm.c),
   * not the browser filesystem. */
  prefsInit("/WinBolo.json");

  /* Voice runs for the life of the process. It comes up before
   * gameFrontStart, as on desktop. This platform has no capture or playback
   * device yet (voice_wasm.c), so this only brings the codec up — every
   * device-facing entry point declines. */
  voiceInit();

  printf("[WASM] Starting gameFrontStart...\n");
  bool started = (gameFrontStart(cmdLine, &keys, FALSE, NULL) != FALSE);
  if (!started && !s_connFailed) {
    /* A genuine init failure (not a connection problem) — nothing to show. */
    printf("[WASM] gameFrontStart FAILED\n");
    clientMutexDestroy();
    SDL_Quit();
    return 1;
  }
  /* From here either we connected, or the connection failed but gameFrontStart
   * kept humanSim alive in its error state — we still set up ImGui and enter
   * the loop so the error dialog can draw (never a blank screen). */
  fprintf(stderr, "[WASM] gameFrontStart %s; humanSim=%p\n",
          started ? "OK" : "CONNECT FAILED", (void*)humanSim);
  fflush(stderr);

  if (started) {
    /* Start the shared tick cadence from a known state (first step is a game
     * step on tick 0), mirroring the desktop run-start. */
    clientFrontTickReset();
    /* Pull the account's cloud prefs and apply them. This runs AFTER
     * gameFrontStart (which seeds defaults and creates humanSim) so
     * wasmApplyJoinPrefs overrides exactly what the player synced — keys, menu
     * toggles, game options, gamepad sensitivities and build options — on the
     * first frame. No-op for single-player (not signed in). Same apply path the
     * relay's join-prefs frame uses. */
    wbPrefsSyncNow();
  }

  /* Single-player name selection. Network play (?game_key=) already set its
   * join name inside gameFrontStart, before the JOIN went out (the account
   * name from /api/v1/me, or a web<rand> fallback) — so there's nothing to do
   * for the network case here. Setting it now would be too late (the JOIN has
   * already been sent) and would only clobber the local copy.
   *
   * Single player: a validated ?name= wins; otherwise default to "Me". */
  {
    const char *gameKey = getUrlParam("game_key");
    if (gameKey[0] != '\0') {
      /* network play: name handled in gameFrontStart before JOIN */
    } else {
      /* Single player: a validated ?name= wins; otherwise default to "Me".
       * The WASM build re-seeds gameFrontName on every launch, so there is
       * no persisted user name to preserve here. */
      const char *urlName = getUrlParam("name");
      char validated[PLAYER_NAME_LEN];
      if (urlName[0] != '\0' &&
          playerNameValidate(urlName, validated, PLAYER_NAME_LEN, NULL)) {
        gameFrontSetPlayerName(validated);
        printf("[WASM] single player: name=%s (from URL)\n", validated);
      } else {
        gameFrontSetPlayerName((char *)"Me");
        printf("[WASM] single player: default name=Me\n");
      }
    }
  }

  isInMenu = FALSE;
  finishedLoop = FALSE;
  windowApplyMenuChecks(humanSim);

  if (soundEffects == TRUE) {
    soundKeepalive(useSoundKeepalive);
  }

  /* Set up ImGui */
  {
    SDL_Window *win = sdl3DrawGetWindow();
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (win && ren) {
      sdl3ImguiSetup(win, ren);
    }
    if (win) {
      SDL_ShowWindow(win);
    }
  }

  /* Force canvas CSS display size to match the backing store. */
  {
    int w, h;
    SDL_GetWindowSize(sdl3DrawGetWindow(), &w, &h);
    emscripten_set_element_css_size("#canvas", (double)w, (double)h);
    printf("[WASM] Canvas CSS forced to %dx%d\n", w, h);
  }

  guiMessageSetHandler(sdl3MessageHandler);

  oldTick = SDL_GetTicks();
  lastFrameTime = emscripten_get_now();

  fprintf(stderr, "[WASM] Starting main loop; humanSim=%p\n", (void*)humanSim);
  fflush(stderr);
  emscripten_set_main_loop(main_loop_iteration, 0, 1);

  /* Cleanup (not reached with simulate_infinite_loop=1) */
  gameFrontEnd(&keys, TRUE, TRUE);
  clientMutexDestroy();
  sdl3ImguiCleanup();
  voiceCleanup();
  sdl3DrawCleanup();
  SDL_Quit();
  return 0;
}

/* -------------------------------------------------------
 * winbolo.h function implementations
 * ------------------------------------------------------- */

void windowReCreate(void)  { }
/* Raised only by windowSetQuitting, exactly as on the desktop: winboloQuit
 * cannot answer the question on its own, because the game loop sets it TRUE
 * on the way in as its default answer.  See windowIsQuitting in
 * src/gui/sdl3/winbolo.c. */
static bool quitRequested = FALSE;
void windowSetQuitting(void) { quitRequested = TRUE; winboloQuit = TRUE; finishedLoop = TRUE; }
bool windowIsQuitting(void)   { return quitRequested; }

/* Leave the game: navigate the hosting page back to the lobby landing. */
void windowLeaveGame(void) { emscripten_run_script("window.location.href='/'"); }

void windowApplyMenuChecks(ClientSim *cs) {
  clientSimSetGunsight(cs, showGunsight);
  clientSimSetAutoScroll(cs, autoScrollingEnabled);
  clientSimSetLabelOwnTank(cs, labelSelf);
  clientSimSetLabelMessage(cs, labelMsg);
  clientSimSetLabelTankLabel(cs, labelTank);
  clientSimShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages);
  clientSimShowMessages(cs, MSG_ASSISTANT, showAssistantMessages);
  clientSimShowMessages(cs, MSG_AI, showAIMessages);
  clientSimShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages);
  clientSimShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages);
  clientSimSetAllowNewPlayers(cs, allowNewPlayers);
}

/* -------------------------------------------------------
 * C5: apply WinBolo.net synced prefs from the join metadata frame.
 *
 * Applies a projected prefs subset (KEYS / MENU / GAME OPTIONS / SETTINGS)
 * from JSON. Two callers feed it: the relay's join-prefs 0x01 frame
 * (udpClientParseProxyMeta, emscripten only) and the WinBolo.net cloud-prefs
 * GET (prefs_bridge_wasm.c, on adopt). Values mirror the
 * desktop INI store: "Yes"/"No" bools, stringified ints, SDL3 keycodes for
 * KEYS. Native JSON bool/number is tolerated too. Absent keys keep the
 * current (default) value. This runs after gameFrontStart seeded defaults,
 * so it overrides exactly what the user has synced.
 * ------------------------------------------------------- */
extern bool useAutoslow;   /* defined in gamefront_wasm.c */
extern bool useAutohide;
void windowSetSoundVolume(int pct);   /* defined below, after this function */
void windowSetMasterVolume(int pct);  /* likewise */
/* Likewise the voice settings, defined below with the rest of them. */
void windowSetVoiceEnabled(bool on);
void windowSetVoiceMode(int mode);
void windowSetVoiceMicGain(float gain);
void windowSetVoiceVolume(float gain);
void windowSetShowTankMicIcons(bool on);
bool windowGetShowTankMicIcons(void);

static bool prefBool(cJSON *o, const char *k, bool dflt) {
  cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
  if (it == NULL) return dflt;
  if (cJSON_IsBool(it))   return cJSON_IsTrue(it);
  if (cJSON_IsNumber(it)) return it->valuedouble != 0;
  if (cJSON_IsString(it) && it->valuestring)
    return (SDL_strcasecmp(it->valuestring, "Yes")  == 0 ||
            SDL_strcasecmp(it->valuestring, "true") == 0 ||
            strcmp(it->valuestring, "1") == 0);
  return dflt;
}

static int prefInt(cJSON *o, const char *k, int dflt) {
  cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
  if (it == NULL) return dflt;
  if (cJSON_IsNumber(it)) return (int)it->valuedouble;
  if (cJSON_IsString(it) && it->valuestring) return atoi(it->valuestring);
  return dflt;
}

static float prefFloat(cJSON *o, const char *k, float dflt) {
  cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
  if (it == NULL) return dflt;
  if (cJSON_IsNumber(it)) return (float)it->valuedouble;
  if (cJSON_IsString(it) && it->valuestring) return (float)atof(it->valuestring);
  return dflt;
}

static const char *prefStr(cJSON *o, const char *k, const char *dflt) {
  cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
  if (it == NULL) return dflt;
  if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
  return dflt;
}

void wasmApplyJoinPrefs(const char *prefsJson, int len) {
  if (prefsJson == NULL || len <= 0) return;
  cJSON *root = cJSON_ParseWithLength(prefsJson, (size_t)len);
  if (root == NULL) { printf("[WASM] join prefs: parse failed\n"); return; }

  cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "KEYS");
  if (cJSON_IsObject(k)) {
    keys.kiForward      = prefInt(k, "Forward",        keys.kiForward);
    keys.kiBackward     = prefInt(k, "Backwards",      keys.kiBackward);
    keys.kiLeft         = prefInt(k, "Left",           keys.kiLeft);
    keys.kiRight        = prefInt(k, "Right",          keys.kiRight);
    keys.kiShoot        = prefInt(k, "Shoot",          keys.kiShoot);
    keys.kiLayMine      = prefInt(k, "Lay Mine",       keys.kiLayMine);
    keys.kiGunIncrease  = prefInt(k, "Increase Range", keys.kiGunIncrease);
    keys.kiGunDecrease  = prefInt(k, "Decrease Range", keys.kiGunDecrease);
    keys.kiTankView     = prefInt(k, "Tank View",      keys.kiTankView);
    keys.kiPillView     = prefInt(k, "Pill View",      keys.kiPillView);
    keys.kiAllyView     = prefInt(k, "Ally View",      keys.kiAllyView);
    keys.kiBaseView     = prefInt(k, "Base View",      keys.kiBaseView);
    keys.kiOverviewZoom = prefInt(k, "Overview Zoom",  keys.kiOverviewZoom);
    keys.kiOverviewFollow  = prefInt(k, "Overview Follow",   keys.kiOverviewFollow);
    keys.kiOverviewZoomIn  = prefInt(k, "Overview Zoom In",  keys.kiOverviewZoomIn);
    keys.kiOverviewZoomOut = prefInt(k, "Overview Zoom Out", keys.kiOverviewZoomOut);
    keys.kiScrollUp     = prefInt(k, "Scroll Up",      keys.kiScrollUp);
    keys.kiScrollDown   = prefInt(k, "Scroll Down",    keys.kiScrollDown);
    keys.kiScrollLeft   = prefInt(k, "Scroll Left",    keys.kiScrollLeft);
    keys.kiScrollRight  = prefInt(k, "Scroll Right",   keys.kiScrollRight);
    keys.kiQuickTree    = prefInt(k, "Quick Tree",     keys.kiQuickTree);
    keys.kiQuickRoad    = prefInt(k, "Quick Road",     keys.kiQuickRoad);
    keys.kiQuickWall    = prefInt(k, "Quick Wall",     keys.kiQuickWall);
    keys.kiQuickPillbox = prefInt(k, "Quick Pillbox",  keys.kiQuickPillbox);
    keys.kiQuickMine    = prefInt(k, "Quick Mine",     keys.kiQuickMine);
    keys.kiPing[0]      = prefInt(k, "Ping 1",         keys.kiPing[0]);
    keys.kiPing[1]      = prefInt(k, "Ping 2",         keys.kiPing[1]);
    keys.kiPing[2]      = prefInt(k, "Ping 3",         keys.kiPing[2]);
    {
      int pi;
      for (pi = 0; pi < PING_BIND_DIRECT_SLOTS; pi++) {
        char name[32];
        snprintf(name, sizeof(name), "Ping Direct %d", pi + 1);
        keys.kiPingDirect[pi] = prefInt(k, name, keys.kiPingDirect[pi]);
      }
    }
  }

  cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "MENU");
  if (cJSON_IsObject(m)) {
    showGunsight              = prefBool(m, "Show Gunsight",                 showGunsight);
    soundEffects              = prefBool(m, "Sound Effects",                 soundEffects);
    showNewswireMessages      = prefBool(m, "Show Newswire Messages",        showNewswireMessages);
    showAssistantMessages     = prefBool(m, "Show Assistant Messages",       showAssistantMessages);
    showAIMessages            = prefBool(m, "Show AI Messages",              showAIMessages);
    showNetworkStatusMessages = prefBool(m, "Show Network Status Messages",  showNetworkStatusMessages);
    showNetworkDebugMessages  = prefBool(m, "Show Network Debug Messages",   showNetworkDebugMessages);
    autoScrollingEnabled      = prefBool(m, "Autoscroll Enabled",            autoScrollingEnabled);
    showPillLabels            = prefBool(m, "Show Pill Labels",              showPillLabels);
    showBaseLabels            = prefBool(m, "Show Base Labels",              showBaseLabels);
    labelSelf                 = prefBool(m, "Label Own Tank",                labelSelf);
    labelMsg  = (labelLen)prefInt(m, "Message Label Size", (int)labelMsg);
    labelTank = (labelLen)prefInt(m, "Tank Label Size",    (int)labelTank);
    int vol = prefInt(m, "Sound Volume", soundVolume);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    windowSetSoundVolume(vol);
    vol = prefInt(m, "Master Volume", windowMasterVolume);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    windowSetMasterVolume(vol);
  }

  cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "GAME OPTIONS");
  if (cJSON_IsObject(g)) {
    useAutoslow = prefBool(g, "Auto Slowdown",           useAutoslow);
    useAutohide = prefBool(g, "Auto Show-Hide Gunsight",  useAutohide);
  }

  /* SETTINGS: gamepad sensitivities (clamped to the same ranges as desktop
   * gamefront.c) and build-cursor options. The globals live in input_gamepad.c
   * and build_cursor.c (both compiled into the wasm build) and are read by the
   * shared input code each tick. */
  cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "SETTINGS");
  if (cJSON_IsObject(s)) {
    float gs = prefFloat(s, "Gamepad Scroll Sens", g_gamepadScrollSensitivity);
    if (gs >= 0.25f && gs <= 4.0f) g_gamepadScrollSensitivity = gs;
    float ts = prefFloat(s, "Gamepad Tank Sens", g_gamepadTankSensitivity);
    if (ts >= 0.10f && ts <= 1.0f) g_gamepadTankSensitivity = ts;
    float bs = prefFloat(s, "Gamepad Build Cursor Sens", g_gamepadBuildCursorSensitivity);
    if (bs >= 0.25f && bs <= 2.0f) g_gamepadBuildCursorSensitivity = bs;

    g_buildExitExecutes = prefBool(s, "Build Exit Executes", g_buildExitExecutes);
    g_buildExitExecutesMomentaryOnly =
        prefBool(s, "Build Exit Executes Momentary Only", g_buildExitExecutesMomentaryOnly);
    g_buildDoubleTapRoad = prefBool(s, "Build Double Tap Road", g_buildDoubleTapRoad);
    g_buildHoldMomentary = prefBool(s, "Build Hold Momentary", g_buildHoldMomentary);
    g_buildAutoCloseOnExecute = prefBool(s, "Build Auto Close On Execute", g_buildAutoCloseOnExecute);
  }

  /* VOICE: written by the desktop, adopted here, so a player's voice
   * settings follow them into the browser. Applied straight onto the
   * running voice module. The mode names and the clamps are the desktop
   * reader's — keep them in step with gamefront.c. "Echo Cancel" is not
   * read: it is desktop-only, the browser gets cancellation from
   * getUserMedia. */
  cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "VOICE");
  if (cJSON_IsObject(v)) {
    windowSetVoiceEnabled(prefBool(v, "Enabled", voiceIsEnabled()));

    const char *mode = prefStr(v, "Mode", NULL);
    if (mode != NULL) {
      if (strcmp(mode, "Off") == 0) {
        windowSetVoiceMode(VOICE_MODE_OFF);
      } else if (strcmp(mode, "Open Mic") == 0) {
        windowSetVoiceMode(VOICE_MODE_OPEN);
      } else {
        windowSetVoiceMode(VOICE_MODE_PTT);
      }
    }

    /* An out-of-range value is rejected back to 1.0 rather than clamped to
     * the edge, so this reader and the desktop's agree on it. */
    float mg = prefFloat(v, "Mic Gain", voiceGetMicGain());
    if (!(mg >= 0.0f && mg <= 4.0f)) mg = 1.0f;
    windowSetVoiceMicGain(mg);
    float vv = prefFloat(v, "Voice Volume", voiceGetOutputVolume());
    if (!(vv >= 0.0f && vv <= 2.0f)) vv = 1.0f;
    windowSetVoiceVolume(vv);

    windowSetShowTankMicIcons(prefBool(v, "Tank Icons", windowGetShowTankMicIcons()));
  }

  cJSON_Delete(root);

  /* Push the toggles into the live client/sim. Keys are read from the global
   * on the next input tick; pill/base label globals on the next status draw. */
  if (humanSim) {
    windowApplyMenuChecks(humanSim);
    clientSimSetTankAutoSlowdown(humanSim, useAutoslow);
    clientSimSetTankAutoHideGunsight(humanSim, useAutohide);
  }
  printf("[WASM] applied join prefs\n");
}

int windowGetDrawTime(void) { return (int)dwSysFrameTotal; }
int windowGetNetTime(void)  { return netGetNetTime(); }
int windowGetAiTime(void)   { return (int)dwSysBrainTotal; }
int windowGetSimTime(void)  { return (int)dwSysGameTotal; }

void windowGetKeys(keyItems *value) { *value = keys; }
void windowSetKeys(keyItems *value) { keys = *value; }

void windowSetZoomFactor(BYTE amount)  { zoomFactor = amount; }
BYTE windowGetZoomFactor(void)         { return zoomFactor; }

void windowZoomChange(BYTE amount, bool fromDragResize) {
  (void)fromDragResize;  /* WASM doesn't use resize detection */
  if (amount == zoomFactor) return;
  printf("[WASM] windowZoomChange: %d -> %d\n", zoomFactor, amount);
  drawBusy = TRUE;
  clientMutexWaitFor();
  sdl3DrawCleanup();
  sdl3DrawSetup(amount);
  clientMutexRelease();
  drawBusy = FALSE;
  windowSetZoomFactor(amount);

  /* Re-initialise ImGui on the new window/renderer */
  {
    SDL_Window *win = sdl3DrawGetWindow();
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (win && ren) {
      sdl3ImguiSetup(win, ren);
    }
  }

  /* Force canvas buffer + CSS to match the new window size */
  {
    int w, h;
    SDL_GetWindowSize(sdl3DrawGetWindow(), &w, &h);
    emscripten_set_canvas_element_size("#canvas", w, h);
    emscripten_set_element_css_size("#canvas", (double)w, (double)h);
    printf("[WASM] Zoom canvas forced to %dx%d\n", w, h);
  }
}

void windowSetFrameRate(int newFrameRate, bool setTimer) {
  (void)setTimer;
  switch (newFrameRate) {
  case FRAME_RATE_10: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_10); break;
  case FRAME_RATE_12: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_12); break;
  case FRAME_RATE_15: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_15); break;
  case FRAME_RATE_20: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_20) - 1; break;
  case FRAME_RATE_30: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_30) - 1; break;
  case FRAME_RATE_50: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_50) - 4; break;
  case FRAME_RATE_60: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_60) - 4; break;
  }
  frameRate = newFrameRate;
}

/* Menu toggles — called by sdl3imgui.cpp */
void windowSoundEffects_toggle(void)          { soundEffects = !soundEffects; }
void windowBackgroundSoundChange_toggle(void) {
  backgroundSound = !backgroundSound;
  if (soundEffects == TRUE) {
    soundKeepalive(backgroundSound ? useSoundKeepalive : FALSE);
  }
}
void windowSoundKeepalive(void) {
  useSoundKeepalive = !useSoundKeepalive;
  if (soundEffects == TRUE && soundIsPlayable() == TRUE) {
    soundKeepalive(useSoundKeepalive);
  }
}

void windowSetSoundVolume(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  soundVolume = pct;
  soundSetEffectsVolume(pct);
}

/* Master reaches the mixer and the voice module separately: voice has its own
 * playback path and its own gain, and nothing downstream covers both. */
void windowSetMasterVolume(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  windowMasterVolume = pct;
  soundSetMasterVolume(pct);
  voiceSetMasterVolume((float)pct / 100.0f);
}

/* Full screen from the screens outside a game. Here for the same reason as
 * the setters below: the welcome, lobby and settings dialogs are shared and
 * call this, and winbolo.c's copy is not part of this build. SDL maps it onto
 * the browser's Fullscreen API, which needs a user gesture — every call site
 * is inside a click handler, so that holds. No SDL_SyncWindow: the desktop
 * copy waits because Wayland and X11 apply the change asynchronously under a
 * dialog that is about to read the window position, and neither applies. */
void windowFullScreenChoose(bool on) {
  gameFrontFullScreen = on;
  SDL_Window *win = sdl3DrawGetWindow();
  if (win) {
    SDL_SetWindowFullscreen(win, on);
  }
  gameFrontSaveCurrentPrefs();
}

/* -------------------------------------------------------
 * Voice settings — apply a value to the running voice module
 * and clamp it to the range the UI offers, the same way
 * winbolo.c does for the desktop.  The settings dialog is
 * shared and compiles into this target with WINBOLO_VOICE
 * set, so it calls these here as well; winbolo.c, which holds
 * the desktop copies, is not part of the wasm build.  Keep
 * the clamps in step with that copy.
 * ------------------------------------------------------- */
void windowSetVoiceEnabled(bool on) {
  voiceSetEnabled(on);
}

void windowSetVoiceMode(int mode) {
  if (mode < VOICE_MODE_OFF || mode > VOICE_MODE_OPEN) {
    mode = VOICE_MODE_PTT;
  }
  voiceSetMode((VoiceMode)mode);
}

void windowSetVoiceMicGain(float gain) {
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 4.0f) gain = 4.0f;
  voiceSetMicGain(gain);
}

void windowSetVoiceVolume(float gain) {
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 2.0f) gain = 2.0f;
  voiceSetOutputVolume(gain);
}

/* The mic icons live on the status pane, which is shared, so these are
 * here for the same reason the setters above are: the settings dialog
 * calls them and winbolo.c's copies are not part of this build. */
void windowSetShowTankMicIcons(bool on) {
  sdl3DrawStatusSetShowMicIcons(on);
}

bool windowGetShowTankMicIcons(void) {
  return sdl3DrawStatusGetShowMicIcons();
}

void windowMenuAllowNewPlayers_toggle(ClientSim *cs) {
  allowNewPlayers = !allowNewPlayers;
  clientSimSetAllowNewPlayers(cs, allowNewPlayers);
}
void windowMenuNewswire_toggle(ClientSim *cs)    { showNewswireMessages = !showNewswireMessages; if (cs) clientSimShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages); }
void windowMenuAssistant_toggle(ClientSim *cs)   { showAssistantMessages = !showAssistantMessages; if (cs) clientSimShowMessages(cs, MSG_ASSISTANT, showAssistantMessages); }
void windowMenuAI_toggle(ClientSim *cs)          { showAIMessages = !showAIMessages; if (cs) clientSimShowMessages(cs, MSG_AI, showAIMessages); }
void windowMenuNetwork_toggle(ClientSim *cs)     { showNetworkStatusMessages = !showNetworkStatusMessages; if (cs) clientSimShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages); }
void windowMenuNetworkDebug_toggle(ClientSim *cs){ showNetworkDebugMessages = !showNetworkDebugMessages; if (cs) clientSimShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages); }
void windowLabelOwnTank_toggle(ClientSim *cs)    { labelSelf = !labelSelf; if (cs) clientSimSetLabelOwnTank(cs, labelSelf); }
void windowSetMessageLabelLen(ClientSim *cs, labelLen n){ labelMsg = n; if (cs) clientSimSetLabelMessage(cs, labelMsg); }
void windowSetTankLabelLen(ClientSim *cs, labelLen n)   { labelTank = n; if (cs) clientSimSetLabelTankLabel(cs, labelTank); }
void windowNewGame(void)                 { winboloQuit = FALSE; }
void windowQuit(void)                    { winboloQuit = TRUE; }

/* Window show/hide — ImGui panels */
void windowShowGameInfo(windowShowRequest req) { sdl3ImguiShowGameInfo(req == wsrOpen); }
void windowShowSysInfo(windowShowRequest req)  { sdl3ImguiShowSysInfo(req == wsrOpen); }
void windowShowNetInfo(windowShowRequest req)  { sdl3ImguiShowNetInfo(req == wsrOpen); }
void windowShowSetPlayerName(windowShowRequest req) { (void)req; }
void windowShowSendMessages(windowShowRequest req)  { sdl3ImguiShowSendMsg(req == wsrOpen); }
void windowShowSetKeys(windowShowRequest req) {
  if (req == wsrOpen) sdl3ImguiShowKeySetup();
}
void windowShowAboutBox(void)           { }
bool windowShowAllianceRequest(void)    { return showAllianceReq; }
void windowDisableSound(void)           { soundEffects = FALSE; useSoundKeepalive = FALSE; }
bool windowGetBackgroundSound(void)     { return backgroundSound; }
void windowRedrawAll(ClientSim *cs) {
  clientMutexWaitFor();
  sdl3DrawRedrawAll(cs, clientSimGetCurrentBuildSelect(cs), NULL, showPillLabels, showBaseLabels);
  clientMutexRelease();
}
void *windowWnd(void) { return NULL; }

void windowSaveMap(ClientSim *cs) {
  (void)cs;
  imguiMessageBoxEx(DIALOG_BOX_TITLE,
                    "Save Map is not available in the web version.",
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
}

void windowButtonAdd(int keyCode)    { (void)keyCode; }
void windowButtonRemove(int keyCode) { (void)keyCode; }
void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
  (void)xWin; (void)yWin; (void)xPos; (void)yPos;
}
void windowStartTutorial(void) { doingTutorial = TRUE; }

/* Desktop-only window helpers — wasm has no native window position/size to
 * persist or aspect-correct, so these are no-ops. */
void windowSmoothScrolling_toggle(void) { smoothScrollingEnabled = !smoothScrollingEnabled; }
void windowLetterboxBarsGray_toggle(void) {
  letterboxBarsGray = !letterboxBarsGray;
  gameFrontSaveCurrentPrefs();
}
void windowComputeAspectCorrectSize(int actualW, int actualH, int actualX, int actualY,
                                    int *saveW, int *saveH, int *saveX, int *saveY) {
  if (saveW) *saveW = actualW;
  if (saveH) *saveH = actualH;
  if (saveX) *saveX = actualX;
  if (saveY) *saveY = actualY;
}
void windowGetSavedPosition(int *x, int *y) { if (x) *x = -1; if (y) *y = -1; }
void windowSetSavedPosition(int x, int y) { (void)x; (void)y; }
void windowGetCustomSize(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; }
void windowSetCustomSize(int w, int h) { (void)w; (void)h; }
void windowSaveCurrentPosition(void) {}
void windowAllowPlayerNameChange(bool allow) { (void)allow; }

/* Suspend/resume and pause hooks.  The desktop build freezes the local
 * (and solo-server) sim and mutes audio while backgrounded, while a
 * controller is lost, or while the Deck/tutorial pause menus are up.  The
 * browser build has no OS-level background/foreground or controller hot-plug
 * lifecycle to drive these, so they are no-ops. */
void windowSuspendBackground(ClientSim *cs) { (void)cs; }
void windowResumeForeground(ClientSim *cs) { (void)cs; }
void windowControllerLostPause(ClientSim *cs, bool active) { (void)cs; (void)active; }
void windowDeckPause(ClientSim *cs, bool active) { (void)cs; (void)active; }
void windowTutorialPause(ClientSim *cs, bool active) { (void)cs; (void)active; }

/* -------------------------------------------------------
 * Frontend callbacks — called by backend (bolo engine)
 * ------------------------------------------------------- */
void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
  if (drawBusy == FALSE) {
    BYTE cursorX, cursorY;
    bool showCursor = clientSimGetCursorPos(cs, &cursorX, &cursorY);
    /* Resolve the reticle through the shared build cursor, exactly as the
       desktop frontend does — the click path (sdl3draw.c, shared with this
       build) dispatches to the latched target, so the reticle must come from
       the same place or the two would disagree here only. */
    bool cursorFaint = false;
    showCursor = buildCursorResolveReticle(cs, showCursor, cursorX, cursorY,
                                           &cursorX, &cursorY, &cursorFaint);
    sdl3DrawSetCursorFaint(cursorFaint);
    sdl3DrawSetNetFailed(clientSimGetNetStatus(cs) == netFailed);
    sdl3DrawMainScreen(cs, value, mineView, tks, gs, sBullet, lgms,
                       NULL, showPillLabels, showBaseLabels,
                       srtDelay, isPillView, edgeX, edgeY,
                       showCursor, cursorX, cursorY);
  }
}

void frontEndUpdateTankStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  (void)cs;
  BYTE fullShells, fullMines, fullArmour, fullTrees;
  clientSimGetTankFullStats(cs, &fullShells, &fullMines, &fullArmour, &fullTrees);
  sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees,
                         fullShells, fullMines, fullArmour, fullTrees);
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
  (void)cs;
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void frontEndPlaySoundPan(ClientSim *cs, sndEffects value,
                          uint16_t gainL, uint16_t gainR) {
  (void)gainL; (void)gainR;
  frontEndPlaySound(cs, value);
}

void windowPlaySound(sndEffects value) {
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
  (void)cs;
  sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
  sdl3DrawCopyPillsStatus(0, 0);
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
  (void)cs;
  sdl3DrawStatusTank(tankNum, ts);
  sdl3DrawCopyTanksStatus(0, 0);
}

void frontEndMessages(ClientSim *cs, char *top, char *bottom) {
  (void)cs;
  if (drawBusy == FALSE) sdl3DrawMessages(0, 0, top, bottom);
}

void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) {
  (void)cs;
  if (drawBusy == FALSE) sdl3DrawKillsDeaths(0, 0, kills, deaths);
}

void frontEndUpdatePlayerPing(ClientSim *cs, playerNumbers value, uint16_t ping) {
  if (!clientSimIsRunning(cs)) return;
  sdl3ImguiUpdatePlayerPing((unsigned char)value, ping);
}

/* Tracks which ClientSim owns the on-screen player panel, so stale callbacks
 * from a previous game can't write into the live UI (mirrors winbolo.c). */
static struct ClientSim *s_activeUiCs = NULL;

void frontEndUpdatePlayerFlags(ClientSim *cs, playerNumbers value, uint8_t clientType, uint8_t clientFlags) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiUpdatePlayerFlags((unsigned char)value, clientType, clientFlags);
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
  (void)cs;
  sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
  sdl3DrawCopyBasesStatus(0, 0);
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
  (void)cs;
  BYTE fullShells, fullMines, fullArmour;
  clientSimGetBaseFullStats(cs, &fullShells, &fullMines, &fullArmour);
  sdl3DrawStatusBaseBars(0, 0, shells, mines, armour,
                         fullShells, fullMines, fullArmour, FALSE);
}

void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) {
  (void)cs;
  clientMutexWaitFor();
  sdl3DrawSetManStatus(0, 0, isDead, angle);
  clientMutexRelease();
}

void frontEndManClear(ClientSim *cs) {
  (void)cs;
  clientMutexWaitFor();
  sdl3DrawSetManClear();
  sdl3DrawCopyManStatus(0, 0);
  clientMutexRelease();
}

void frontEndDrawDownload(ClientSim *cs, bool justBlack) {
  if (drawBusy == FALSE) {
    sdl3DrawDownloadScreen(cs, NULL, justBlack);
  }
}

void frontEndDrawReturningToLobby(ClientSim *cs) {
  if (drawBusy == FALSE) {
    sdl3DrawReturningToLobby(cs);
  }
}

void frontEndAudioReturningToLobby(bool active) {
  soundSetReturningToLobby(active);
}

void frontEndGameOver(ClientSim *cs) {
  (void)cs;
  imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_WBTIMELIMIT_END),
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
  finishedLoop = TRUE;
  /* Dismissing the dialog goes back to the page the game launched from: the
   * browser build has no welcome screen to rebuild through the way the desktop
   * loop does, so the launching page is the destination. finishedLoop stops the
   * tick for the frames that run before the browser unloads the page. */
  windowLeaveGame();
}

void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiClearPlayer((unsigned char)value);
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) {
  char cc[3];
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (!clientSimIsRunning(cs)) {
    cc[0] = 'X'; cc[1] = 'X'; cc[2] = '\0';
    sdl3ImguiSetPlayer((unsigned char)value, str, cc);
    return;
  }
  /* Country code may arrive as "" when unknown; substitute 'X' so we never
   * read past the end (matches winbolo.c). */
  cc[0] = countryCode[0] ? countryCode[0] : 'X';
  cc[1] = (countryCode[0] && countryCode[1]) ? countryCode[1] : 'X';
  cc[2] = '\0';
  sdl3ImguiSetPlayer((unsigned char)value, str, cc);
  sdl3ImguiUpdatePlayerMeta((unsigned char)value, ping, clientType, clientFlags);
}

void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}

void frontEndApplyLocalTankPrefs(struct ClientSim *cs) {
  if (cs == NULL) return;
  clientSimSetTankAutoSlowdown(cs, useAutoslow);
  clientSimSetTankAutoHideGunsight(cs, useAutohide);
}

void frontEndSetActiveClientSim(struct ClientSim *cs) {
  if (cs != s_activeUiCs) {
    for (BYTE i = 0; i < MAX_TANKS; i++) {
      sdl3ImguiClearPlayer(i);
    }
    /* Drop the previous game's newswire/kills text so it doesn't linger. */
    sdl3DrawResetCachedText();
    /* Drop the previous game's latched build target for the same reason: it
       is the square a click builds at, and it outlives the ClientSim that
       set it, so without this the first click of the next game is dispatched
       to a tile chosen in the last one. */
    buildCursorReset();
  }
  s_activeUiCs = cs;
}
void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled)   { (void)enabled; }

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
  showGunsight = !isShown;
  clientSimSetGunsight(cs, showGunsight);
}

void frontEndShowAllianceRequest(char *playerName, BYTE playerNum) {
  if (showAllianceReq) {
    sdl3ImguiShowAllianceRequest(playerName, playerNum);
  }
}

/* -------------------------------------------------------
 * Tutorial step driver. Ported from gui/sdl3/winbolo.c — the WASM build
 * replaces that event loop with this file. The step DATA lives in
 * bolo/tutorial.c (tutorialSteps[]), the trigger is tank.c calling
 * frontEndTutorial(pos), and the overlay is imgui_tutorial_overlay.cpp drawn
 * from sdl3imgui.cpp. This drives the step sequence and intro gating.
 * Single-threaded under emscripten, so no client-mutex dance is needed.
 * ------------------------------------------------------- */
#define TUTORIAL_INTRO_MIN_FRAMES 3
static int  tutorialStepIdx = 0;
static int  tutorialFramesPresented = 0;
static bool respawn1Shown = false;

void frontEndTutorialReset(void) {
  tutorialStepIdx = 0;
  tutorialFramesPresented = 0;
  respawn1Shown = false;
}

/* Called once per frame after present so the intro defers until a real game
 * frame is on screen (the modal dims over the map, not a grey backbuffer). */
void frontEndTutorialNotePresentedFrame(void) {
  if (tutorialFramesPresented < TUTORIAL_INTRO_MIN_FRAMES) {
    tutorialFramesPresented++;
  }
}

/* Shows the respawn message once per run when the server flags a death. */
static void tutorialRespawnPoll(void) {
  if (isTutorial != TRUE || respawn1Shown) return;
  if (tutorialOverlayIsOpen()) return;  /* don't stack on a step message */
  ServerSim *srv = gameFrontGetServerSim();
  if (!srv) return;
  if (!serverSimTakeTutorialRespawn1(srv)) return;
  respawn1Shown = true;
  {
    uint16_t ids[1];
    ids[0] = STR_TUTORIAL_RESPAWN1;
    tutorialOverlayShow(ids, 1, NULL);
  }
}

/* Runs when the player dismisses the last message of the current step.
 * Advances the sequencer; on the final step, leaves tutorial mode so the
 * player can keep driving. */
static void tutorialStepComplete(void) {
  bool finalStep = (tutorialStepIdx == tutorialStepCount - 1);
  if (finalStep) {
    isTutorial = FALSE;
    if (humanSim) clientSimSetTutorial(humanSim, false);
    {
      ServerSim *srv = gameFrontGetServerSim();
      if (srv) serverSimSetTutorial(srv, false);
    }
    gameFrontSetShowTutorialButton(false);
  }
  doingTutorial = FALSE;
  tutorialStepIdx++;

  /* After the boat-building step (the row with pos == 66), respawn at start 1
   * (the far bank) instead of start 0 (at sea). Scan for the row so it
   * survives step-table edits. */
  {
    int boatStep = -1;
    int s;
    for (s = 0; s < tutorialStepCount; s++) {
      if (tutorialSteps[s].pos == 66) { boatStep = s; break; }
    }
    ServerSim *srv = gameFrontGetServerSim();
    if (srv && boatStep >= 0) {
      serverSimSetTutorialStartIdx(srv, (tutorialStepIdx > boatStep) ? 1 : 0);
    }
  }
}

bool frontEndTutorial(BYTE pos) {
  int i;
  int count;

  if (isTutorial != TRUE) {
    tutorialStepIdx = 0;  /* reset for the next run */
    return FALSE;
  }
  if (tutorialStepIdx >= tutorialStepCount) return FALSE;
  /* A message is up (or pending this frame): hold the tank, don't re-trigger. */
  if (tutorialOverlayIsOpen()) return TRUE;

  {
    BYTE stepPos = tutorialSteps[tutorialStepIdx].pos;
    if (stepPos != TUTORIAL_POS_ANY && stepPos != pos) return FALSE;
  }
  /* Intro step: defer until a frame has rendered so the modal opens over the
   * map rather than a grey backbuffer. */
  if (tutorialSteps[tutorialStepIdx].pos == TUTORIAL_POS_ANY &&
      tutorialFramesPresented < TUTORIAL_INTRO_MIN_FRAMES) {
    return FALSE;
  }

  doingTutorial = TRUE;
  count = 0;
  for (i = 0; i < TUTORIAL_MAX_MSGS; i++) {
    if (tutorialSteps[tutorialStepIdx].msgs[i] == 0) break;
    count++;
  }
  tutorialOverlayShow(tutorialSteps[tutorialStepIdx].msgs, count,
                      tutorialStepComplete);
  return TRUE;
}

/* -------------------------------------------------------
 * Misc required symbols
 * ------------------------------------------------------- */
time_t serverMainGetTicks(void) { return ticks; }
time_t windowsGetTicks(void)    { return ticks; }
int winboloCC(void)             { return 0; }

void *dialogAllianceCreate(void)                  { return NULL; }
void dialogAllianceDestroy(void *dlg)             { (void)dlg; }
void dialogAllianceSetName(char *playerName, BYTE playerNum) {
  if (playerName != NULL) {
    sdl3ImguiShowAllianceRequest(playerName, playerNum);
  }
}

bool winUtilWBSubDirExist(char *subDirName) {
  struct stat st;
  if (stat(subDirName, &st) == 0 && S_ISDIR(st.st_mode)) {
    return TRUE;
  }
  return FALSE;
}
