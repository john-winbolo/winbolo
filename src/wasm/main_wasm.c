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
#include "client_sim.h"
#include "frontend.h"
#include "playername_validate.h"
#include "client_net.h"
#include "gui_message.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../gui/draw.h"
#include "../gui/gamefront.h"
#include "../gui/input.h"
#include "../gui/lang.h"
#include "../gui/sound.h"
#include "../gui/ui_mode.h"
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../gui/sdl3/dialogs/imgui_messagebox.h"
#include "../gui/sdl3/dialogs/imgui_tutorial_overlay.h"
#include "server_sim.h"
#include "tutorial.h"

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
int  soundVolume = 50;
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

/* -------------------------------------------------------
 * windowRunGameTick — game logic using transport
 * ------------------------------------------------------- */
static void windowRunGameTick(ClientSim *cs) {
  static bool inBrain = FALSE;
  static bool justKeys = FALSE;
  static BYTE t2 = 0;
  static int trackerTime = 11500;
  static uint32_t simTickCounter = 0;
  tankButton tb;
  bool isShoot;
  bool isMine = FALSE;
  bool used = FALSE;
  bool brainRunning;

  brainRunning = brainHandlerIsBrainRunning();
  isShoot = FALSE;
  tb = 0;

  if (!clientSimHasTransport(cs) || doingTutorial) {
    return;
  }

  {
    BYTE myPlayerNum = gameFrontGetPlayerNum();
    if (justKeys == TRUE) {
      /* Keys tick */
      if (brainRunning == FALSE) {
        tb = inputGetKeys(cs, &keys, isInMenu);
      } else {
        inputScroll(cs, &keys, isInMenu);
      }
      InputPacket pkt;
      clientBuildInputPacket(cs, &pkt, tb, FALSE, FALSE, brainRunning, FALSE, myPlayerNum, simTickCounter);
      clientMutexWaitFor();
      clientSimKeysTick(cs, &pkt);
      clientMutexRelease();
      /* Deliver the keys-half input but do NOT advance the server here.
       * The active local transport's tick runs serverSimTick, which is a
       * full 20ms frame (both keys+game half-steps internally). Ticking it
       * in both branches would advance the sim every 10ms — 2x too fast.
       * Only the game-tick branch below advances it, so the server runs at
       * the 20ms SERVER_TICK_LENGTH cadence, matching the desktop build. */
      clientSimNetRecordInput(cs, &pkt);
      simTickCounter++;
      justKeys = FALSE;
    } else {
      /* Game tick */
      t2++;
      trackerTime++;
      if (brainRunning == FALSE) {
        tb = inputGetKeys(cs, &keys, isInMenu);
        isShoot = inputIsFireKeyPressed(&keys, isInMenu);
        isMine = inputIsMineKeyPressed(&keys, isInMenu);
      } else {
        inputScroll(cs, &keys, isInMenu);
      }
      InputPacket pkt;
      clientBuildInputPacket(cs, &pkt, tb, isShoot, isMine, brainRunning, TRUE, myPlayerNum, simTickCounter);
      clientMutexWaitFor();
      clientSimGameTick(cs, &pkt, brainRunning);
      clientMutexRelease();
      clientSimNetSendInput(cs, &pkt);
      clientSimNetTick(cs);
      clientMutexWaitFor();
      clientSimDisplayTick(cs, brainRunning);
      clientMutexRelease();
      simTickCounter++;
      ticks++;
      justKeys = TRUE;
      used = TRUE;
    }
  }

  /* AI */
  if (used == TRUE && inBrain == FALSE && brainRunning == TRUE) {
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

static void main_loop_iteration(void) {
  DWORD tick;
  ClientSim *cs = humanSim;

  /* Process events */
  sdl3ImguiProcessEvents(cs);

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
  if (clientSimHasTransport(cs)) {
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

  /* Render */
  tick = SDL_GetTicks();
  clientMutexWaitFor();
  if (finishedLoop == FALSE) {
    clientSimRenderPrepare(cs, tick);
    clientRenderFrame(cs, redraw);
  }
  clientMutexRelease();
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

  printf("[WASM] Starting gameFrontStart...\n");
  if (gameFrontStart(cmdLine, &keys, FALSE, NULL) == FALSE) {
    printf("[WASM] gameFrontStart FAILED\n");
    clientMutexDestroy();
    SDL_Quit();
    return 1;
  }
  fprintf(stderr, "[WASM] gameFrontStart OK; humanSim=%p\n", (void*)humanSim);
  fflush(stderr);

  /* Pick the player name by URL mode, after gameFrontStart sets defaults.
   *
   * Network play (?join_code= present): identity is WinBolo.net-authoritative
   * and the server stamps the real account name at re-auth, so the JOIN only
   * needs a throwaway placeholder. Use web<rand>, overriding any stored name
   * so a stale local name can't ride the JOIN.
   *
   * Single player: a validated ?name= wins; then a genuinely chosen stored
   * name; otherwise default to "Me" (the generic default-name seed counts as
   * "no name"). */
  {
    const char *joinCode = getUrlParam("join_code");
    if (joinCode[0] != '\0') {
      char placeholder[PLAYER_NAME_LEN];
      unsigned suffix =
          ((unsigned)time(NULL) ^ (unsigned)(emscripten_get_now() * 1000.0))
          % 1000000u;
      snprintf(placeholder, sizeof(placeholder), "web%u", suffix);
      gameFrontSetPlayerName(placeholder);
      printf("[WASM] network play: placeholder name=%s (WBN name set by server)\n",
             placeholder);
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
  sdl3DrawCleanup();
  SDL_Quit();
  return 0;
}

/* -------------------------------------------------------
 * winbolo.h function implementations
 * ------------------------------------------------------- */

void windowReCreate(void)  { }
void windowSetQuitting(void) { winboloQuit = TRUE; finishedLoop = TRUE; }

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
void windowShowGunsight_toggle(ClientSim *cs) {
  showGunsight = !showGunsight;
  clientSimSetGunsight(cs, showGunsight);
}
void windowAutomaticScrolling_toggle(ClientSim *cs) {
  autoScrollingEnabled = !autoScrollingEnabled;
  if (cs) clientSimSetAutoScroll(cs, autoScrollingEnabled);
}
void windowShowPillLabels_toggle(ClientSim *cs) {
  BYTE count, total;
  showPillLabels = !showPillLabels;
  sdl3DrawSetPillsStatusClear();
  total = clientSimGetPillCount(cs);
  for (count = 1; count <= total; count++) {
    BYTE pillStat = clientSimGetPillAlliance(cs, count);
    sdl3DrawStatusPillbox(count, pillStat, showPillLabels);
  }
  sdl3DrawCopyPillsStatus(0, 0);
}
void windowShowBaseLabels_toggle(ClientSim *cs) {
  BYTE count, total;
  showBaseLabels = !showBaseLabels;
  sdl3DrawSetBasesStatusClear();
  total = clientSimGetBaseCount(cs);
  for (count = 1; count <= total; count++) {
    BYTE baseStat = clientSimGetBaseAlliance(cs, count);
    sdl3DrawStatusBase(count, baseStat, showBaseLabels);
  }
  sdl3DrawCopyBasesStatus(0, 0);
}
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
  soundSetVolume(pct);
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

void windowKeyPressed(ClientSim *cs, int keyCode) {
  if (keyCode == keys.kiTankView) clientSimTankView(cs);
  else if (keyCode == keys.kiPillView) clientSimPillView(cs, 0, 0);
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
    sdl3DrawSetNetFailed(clientSimGetNetStatus(cs) == netFailed);
    sdl3DrawMainScreen(cs, value, mineView, tks, gs, sBullet, lgms,
                       NULL, showPillLabels, showBaseLabels,
                       srtDelay, isPillView, edgeX, edgeY,
                       showCursor, cursorX, cursorY);
  }
}

void frontEndUpdateTankStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  (void)cs;
  if (armour > TANK_FULL_ARMOUR) {
    armour = 0;
  }
  sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees);
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
  (void)cs;
  if (soundEffects == TRUE) soundPlayEffect(value);
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

void frontEndUpdatePlayerFlags(ClientSim *cs, playerNumbers value, uint8_t clientType, uint8_t clientFlags) {
  (void)cs; (void)value; (void)clientType; (void)clientFlags;
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
  (void)cs;
  sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
  sdl3DrawCopyBasesStatus(0, 0);
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
  (void)cs;
  sdl3DrawStatusBaseBars(0, 0, shells, mines, armour, FALSE);
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
}

void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) { (void)cs; (void)value; }
void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) { (void)cs; (void)value; (void)str; (void)countryCode; (void)ping; (void)clientType; (void)clientFlags; }
void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) { (void)cs; (void)value; (void)isChecked; }
void frontEndApplyLocalTankPrefs(struct ClientSim *cs) { (void)cs; }
void frontEndSetActiveClientSim(struct ClientSim *cs) { (void)cs; }
void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled)   { (void)enabled; }

void frontEndRedrawAll(ClientSim *cs) { windowRedrawAll(cs); }

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
