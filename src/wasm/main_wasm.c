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
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../gui/sdl3/dialogs/imgui_messagebox.h"

#include <sys/stat.h>

extern ClientSim *humanSim;

/* -------------------------------------------------------
 * Globals declared by winbolo.h
 * ------------------------------------------------------- */

void windowStartTutorial(void);
bool isTutorial = FALSE;

int frameRate = FRAME_RATE_30;
static int frameRateTime = (int)(MILLISECONDS / FRAME_RATE_30) - 1;

bool showGunsight = FALSE;
bool soundEffects = TRUE;
bool backgroundSound = TRUE;
bool useSoundKeepalive = FALSE;
bool allowNewPlayers = TRUE;

bool showNewswireMessages = TRUE;
bool showAssistantMessages = TRUE;
bool showAIMessages = FALSE;
bool showNetworkStatusMessages = TRUE;
bool showNetworkDebugMessages = FALSE;

bool autoScrollingEnabled = FALSE;
bool smoothScrollingEnabled = FALSE;  /* WASM: arrow-key smooth scroll inactive */
BYTE zoomFactor = ZOOM_FACTOR_DOUBLE;

bool showPillLabels = FALSE;
bool showBaseLabels = FALSE;
bool labelSelf = TRUE;
labelLen labelMsg = lblShort;
labelLen labelTank = lblShort;

bool hideMainView = FALSE;
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
      clientSimNetRecordInput(cs, &pkt);
      clientSimNetTick(cs);
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
static void main_loop_iteration(void) {
  DWORD tick;
  ClientSim *cs = humanSim;

  /* Process events */
  sdl3ImguiProcessEvents(cs);

  /* Game tick accumulation (replaces SDL_AddTimer).
   *
   * After a JS GC pause or tab throttle, `elapsed` can spike to hundreds of
   * ms.  Running every backlogged sim tick in a single render frame causes
   * a visible hitch — but dropping ticks outright would lose
   * transport->tick() calls (UDP packet send/recv) and break multiplayer.
   *
   * Compromise: cap per-frame sim ticks at MAX_CATCHUP so no single render
   * frame stalls, and let leftover backlog stay in gameTickAccum to drain
   * naturally over the following frames.  Each subsequent frame runs up to
   * MAX_CATCHUP ticks until the debt clears, so windowRunGameTick (and the
   * transport->tick inside it) eventually fire for every missed tick.
   *
   * Only MAX_ELAPSED_MS itself is hard-capped — a multi-second tab
   * suspension shouldn't trigger an unbounded catch-up sequence. */
  if (clientSimHasTransport(cs)) {
    const double MAX_ELAPSED_MS = 200.0;  /* hard limit on accumulated debt */
    const int    MAX_CATCHUP    = 4;      /* at most 4 sim ticks per render frame */
    double now = emscripten_get_now();
    double elapsed = now - lastFrameTime;
    lastFrameTime = now;
    if (elapsed > MAX_ELAPSED_MS) elapsed = MAX_ELAPSED_MS;
    gameTickAccum += elapsed;

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

  /* Apply player name from URL after gameFrontStart sets defaults.
   * Gated like the Phase 7.1 Steam-persona seed: only honour ?name=
   * when there is no persisted name and no stored WBN token (which
   * would overwrite us authoritatively), and run the URL value
   * through the Phase 2 validator before applying it. */
  {
    const char *urlName = getUrlParam("name");
    if (urlName[0] != '\0') {
      char persisted[PLAYER_NAME_LEN];
      char token[256], expiry[256];
      persisted[0] = '\0';
      token[0] = '\0';
      expiry[0] = '\0';
      gameFrontGetPlayerName(persisted);
      gameFrontGetWinbolonetToken(token, expiry);

      if (persisted[0] != '\0') {
        printf("[WASM] URL name ignored (already have %s)\n", persisted);
      } else if (token[0] != '\0') {
        printf("[WASM] URL name ignored (WBN token present)\n");
      } else {
        char validated[PLAYER_NAME_LEN];
        if (playerNameValidate(urlName, validated, PLAYER_NAME_LEN, NULL)) {
          gameFrontSetPlayerName(validated);
          printf("[WASM] URL name=%s\n", validated);
        } else {
          gameFrontSetPlayerName((char *)langGetText(STR_DLGGAMESETUP_DEFAULTNAME));
          printf("[WASM] URL name rejected, using default\n");
        }
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
void windowMenuAllowNewPlayers_toggle(ClientSim *cs) {
  allowNewPlayers = !allowNewPlayers;
  clientSimSetAllowNewPlayers(cs, allowNewPlayers);
}
void windowMenuNewswire_toggle(ClientSim *cs)    { showNewswireMessages = !showNewswireMessages; if (cs) clientSimShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages); }
void windowMenuAssistant_toggle(ClientSim *cs)   { showAssistantMessages = !showAssistantMessages; if (cs) clientSimShowMessages(cs, MSG_ASSISTANT, showAssistantMessages); }
void windowMenuAI_toggle(ClientSim *cs)          { showAIMessages = !showAIMessages; if (cs) clientSimShowMessages(cs, MSG_AI, showAIMessages); }
void windowMenuNetwork_toggle(ClientSim *cs)     { showNetworkStatusMessages = !showNetworkStatusMessages; if (cs) clientSimShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages); }
void windowMenuNetworkDebug_toggle(ClientSim *cs){ showNetworkDebugMessages = !showNetworkDebugMessages; if (cs) clientSimShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages); }
void windowHideMainView_toggle(void)    { hideMainView = !hideMainView; }
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

/* -------------------------------------------------------
 * Frontend callbacks — called by backend (bolo engine)
 * ------------------------------------------------------- */
void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
  if (hideMainView == FALSE && drawBusy == FALSE) {
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
  if (hideMainView == FALSE && drawBusy == FALSE) {
    sdl3DrawDownloadScreen(cs, NULL, justBlack);
  }
}

void frontEndDrawReturningToLobby(ClientSim *cs) {
  if (hideMainView == FALSE && drawBusy == FALSE) {
    sdl3DrawReturningToLobby(cs);
  }
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

bool frontEndTutorial(BYTE pos) {
  (void)pos;
  return FALSE;  /* Tutorial not supported in WASM build */
}

void frontEndTutorialReset(void) { }

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
