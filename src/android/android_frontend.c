/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * android_frontend.c - Frontend callbacks and window functions for Android
 *
 * The window* set this driver keeps for itself and the frontEnd* callbacks
 * that draw into the SDL3 renderer, as src/gui/ios/winbolo_ios.m keeps its
 * own. Everything that carries sim-state logic (the active-sim test, the
 * tutorial step sequencer, the time-limit game over, the settings toggles
 * that tell a running game) comes from client_frontend_common.c, the one
 * body every platform calls; see "Platform variants" in docs/ARCHITECTURE.md.
 */

#include <SDL3/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>

#include "../common/wb_log.h"
#include "client_sim.h"
#include "frontend.h"
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
#include "../gui/sdl3/build_cursor.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../gui/sdl3/dialogs/imgui_messagebox.h"
#include "client_frontend_common.h"

/* -------------------------------------------------------
 * Externs from main_android.c
 * ------------------------------------------------------- */

extern int frameRate;
extern bool showGunsight;
extern bool soundEffects;
extern bool backgroundSound;
extern bool useSoundKeepalive;
extern int  soundVolume;
extern int  windowMasterVolume;
extern bool positionalSound;
extern bool letterboxBarsGray;
extern bool showNewswireMessages;
extern bool showAssistantMessages;
extern bool showAIMessages;
extern bool showNetworkStatusMessages;
extern bool showNetworkDebugMessages;
extern bool allowNewPlayers;
extern bool autoScrollingEnabled;
extern BYTE zoomFactor;
extern bool showPillLabels;
extern bool showBaseLabels;
extern bool labelSelf;
extern labelLen labelMsg;
extern labelLen labelTank;
extern keyItems keys;

extern bool drawBusy;
extern bool winboloQuit;
extern bool finishedLoop;

extern DWORD dwSysFrameTotal;
extern DWORD dwSysGameTotal;
extern DWORD dwSysBrainTotal;

extern time_t ticks;

/* -------------------------------------------------------
 * SDL message handler
 * ------------------------------------------------------- */
void sdl3MessageHandler(const char *message, const char *title) {
  imguiMessageBoxEx(title ? title : "WinBolo",
                    message ? message : "",
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
}

/* -------------------------------------------------------
 * winbolo.h function implementations
 * ------------------------------------------------------- */

void *windowWnd(void) { return NULL; }
void windowReCreate(void)  { }
/* Raised only by windowSetQuitting, exactly as on the desktop: winboloQuit
 * cannot answer the question on its own, because the game loop sets it TRUE
 * on the way in as its default answer.  See windowIsQuitting in
 * src/gui/sdl3/winbolo.c. */
static bool quitRequested = FALSE;
void windowSetQuitting(void) { quitRequested = TRUE; winboloQuit = TRUE; finishedLoop = TRUE; }
bool windowIsQuitting(void)   { return quitRequested; }

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

/* The touch layout rebuilds the window and renderer for a zoom change, as
   the iOS touch layout does; ImGui is torn down first because its textures
   were made on the renderer that goes. */
void windowZoomChange(BYTE amount, bool fromDragResize) {
  (void)fromDragResize;  /* Android doesn't use resize detection */
  if (amount == zoomFactor) return;
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] windowZoomChange: %d -> %d", zoomFactor, amount);
  drawBusy = TRUE;
  clientMutexWaitFor();
  sdl3ImguiCleanup();
  sdl3DrawCleanup();
  sdl3DrawSetup(amount);
  {
    SDL_Window *win = sdl3DrawGetWindow();
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (win && ren) {
      sdl3ImguiSetup(win, ren);
    }
  }
  clientMutexRelease();
  drawBusy = FALSE;
  windowSetZoomFactor(amount);
}

void windowSetFrameRate(int newFrameRate, bool setTimer) {
  (void)setTimer;
  frameRate = newFrameRate;
}

/* windowShowGunsight_toggle, windowAutomaticScrolling_toggle,
   windowShowPillLabels_toggle, windowShowBaseLabels_toggle, windowKeyPressed,
   windowSmoothScrolling_toggle and frontEndRedrawAll come from
   client_frontend_common.c, the one body every platform calls. */

void windowSoundEffects_toggle(void)          { soundEffects = !soundEffects; }
void windowPositionalSound_toggle(void)       { positionalSound = !positionalSound; }
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
void windowSetMasterVolume(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  windowMasterVolume = pct;
  soundSetMasterVolume(pct);
}
void windowLabelOwnTank_toggle(ClientSim *cs)    { labelSelf = !labelSelf; if (cs) clientSimSetLabelOwnTank(cs, labelSelf); }
void windowSetMessageLabelLen(ClientSim *cs, labelLen n){ labelMsg = n; if (cs) clientSimSetLabelMessage(cs, labelMsg); }
void windowSetTankLabelLen(ClientSim *cs, labelLen n)   { labelTank = n; if (cs) clientSimSetLabelTankLabel(cs, labelTank); }

/* windowMenuAllowNewPlayers_toggle and the five message toggles are in
   client_frontend_common.c: they also tell the running game. */

/* Leaving a game: the loop in main_android.c ends and goes back to the
   menu, as it does when a game ends. */
void windowNewGame(void) {
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] windowNewGame: leaving game");
  winboloQuit = FALSE;
  finishedLoop = TRUE;
}
void windowQuit(void)                    { }

/* The Session Info panels, drawn in-window as the web's are. */
void windowShowGameInfo(windowShowRequest req) { sdl3ImguiShowGameInfo(req == wsrOpen); }
void windowShowSysInfo(windowShowRequest req)  { sdl3ImguiShowSysInfo(req == wsrOpen); }
void windowShowNetInfo(windowShowRequest req)  { sdl3ImguiShowNetInfo(req == wsrOpen); }
void windowShowSetPlayerName(windowShowRequest req) { (void)req; }
void windowShowSendMessages(windowShowRequest req)  { sdl3ImguiShowSendMsg(req == wsrOpen); }
void windowShowSetKeys(windowShowRequest req)  { if (req == wsrOpen) sdl3ImguiShowKeySetup(); }
void windowShowAboutBox(void)           { }

/* Alliance requests from other players are shown, as on the desktop. */
bool windowShowAllianceRequest(void)    { return TRUE; }
void windowDisableSound(void)           { soundEffects = FALSE; useSoundKeepalive = FALSE; }
bool windowGetBackgroundSound(void)     { return backgroundSound; }
void windowRedrawAll(ClientSim *cs) {
  clientMutexWaitFor();
  sdl3DrawRedrawAll(cs, clientSimGetCurrentBuildSelect(cs), NULL, showPillLabels, showBaseLabels);
  clientMutexRelease();
}

/* No save dialog on Android: the map goes into a Maps folder in the app's
   own storage under a made-up name, and the message box names the file. */
void windowSaveMap(ClientSim *cs) {
  const char *pref = SDL_GetPrefPath("WinBolo", "WinBolo");
  char dir[FILENAME_MAX];
  if (!pref || !pref[0]) {
    imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_WBERR_SAVEMAP),
                      IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    return;
  }
  snprintf(dir, sizeof(dir), "%sMaps", pref);
  clientFrontSaveMapAutoNamed(cs, dir);
}

void windowButtonAdd(int keyCode)    { (void)keyCode; }
void windowButtonRemove(int keyCode) { (void)keyCode; }
void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
  (void)xWin; (void)yWin; (void)xPos; (void)yPos;
}
void windowStartTutorial(void) { }
void windowAllowPlayerNameChange(bool allow) { (void)allow; }

/* The Full Screen choice from the settings dialog. An Android window
   always fills the screen, so only the preference is recorded; with it on
   the in-window map fills the screen in place of the game view's square. */
void windowFullScreenChoose(bool on) {
  gameFrontFullScreen = on;
  gameFrontSaveCurrentPrefs();
}

/* Gray or black letterbox bars, from Settings. */
void windowLetterboxBarsGray_toggle(void) {
  letterboxBarsGray = !letterboxBarsGray;
  gameFrontSaveCurrentPrefs();
}

/* -------------------------------------------------------
 * Frontend callbacks — called by the sim
 *
 * frontEndSetActiveClientSim and the active-sim test the callbacks here
 * make (clientFrontIsActiveUiSim) come from client_frontend_common.c.
 * ------------------------------------------------------- */
void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
  if (drawBusy == FALSE) {
    BYTE cursorX, cursorY;
    bool showCursor = clientSimGetCursorPos(cs, &cursorX, &cursorY);
    /* Resolve the reticle through the shared build cursor, as every other
       frontend does — the tap-to-build handler latches the tapped square
       there, so the reticle has to read it back from the same place. */
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
  BYTE fullShells, fullMines, fullArmour, fullTrees;
  if (!clientFrontIsActiveUiSim(cs)) return;
  clientSimGetTankFullStats(cs, &fullShells, &fullMines, &fullArmour, &fullTrees);
  sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees,
                         fullShells, fullMines, fullArmour, fullTrees);
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
  BYTE fullShells, fullMines, fullArmour;
  if (!clientFrontIsActiveUiSim(cs)) return;
  clientSimGetBaseFullStats(cs, &fullShells, &fullMines, &fullArmour);
  sdl3DrawStatusBaseBars(0, 0, shells, mines, armour,
                         fullShells, fullMines, fullArmour, FALSE);
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void frontEndPlaySoundPan(ClientSim *cs, sndEffects value,
                          uint16_t gainL, uint16_t gainR) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  if (soundEffects) {
    /* The player has turned positional sound off, so every sound plays
       centred; the near/far variant is already chosen. */
    if (!positionalSound) {
      gainL = SOUND_GAIN_UNITY;
      gainR = SOUND_GAIN_UNITY;
    }
    soundPlayEffectPan(value, gainL, gainR);
  }
}

void windowPlaySound(sndEffects value) {
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
  sdl3DrawCopyPillsStatus(0, 0);
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  sdl3DrawStatusTank(tankNum, ts);
  sdl3DrawCopyTanksStatus(0, 0);
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
  sdl3DrawCopyBasesStatus(0, 0);
}

void frontEndMessages(ClientSim *cs, char *top, char *bottom) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  if (drawBusy == FALSE) sdl3DrawMessages(0, 0, top, bottom);
}

void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  if (drawBusy == FALSE) sdl3DrawKillsDeaths(0, 0, kills, deaths);
}

void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  clientMutexWaitFor();
  sdl3DrawSetManStatus(0, 0, isDead, angle);
  clientMutexRelease();
}

void frontEndManClear(ClientSim *cs) {
  if (!clientFrontIsActiveUiSim(cs)) return;
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

/* frontEndGameOver is in client_frontend_common.c; these are this driver's
   parts of it. Every time-limit game over leaves the game: a single-player
   round connects passively, as on the desktop, so the web's test for a
   practice round that ends in its lobby does not apply. */
bool windowGameOverEndsGame(ClientSim *cs) {
  (void)cs;
  return TRUE;
}

void windowGameOverStopTicks(void) {
}

void windowGameOverLeave(void) {
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Game over (time limit expired)");
  finishedLoop = TRUE;
}

void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  sdl3ImguiClearPlayer((unsigned char)value);
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) {
  char cc[3];
  if (!clientFrontIsActiveUiSim(cs)) return;
  if (!clientSimIsRunning(cs)) {
    cc[0] = 'X'; cc[1] = 'X'; cc[2] = '\0';
    sdl3ImguiSetPlayer((unsigned char)value, str, cc);
    return;
  }
  cc[0] = countryCode[0];
  cc[1] = countryCode[1];
  cc[2] = '\0';
  sdl3ImguiSetPlayer((unsigned char)value, str, cc);
  sdl3ImguiUpdatePlayerMeta((unsigned char)value, ping, clientType, clientFlags);
}

void frontEndUpdatePlayerPing(ClientSim *cs, playerNumbers value, uint16_t ping) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  if (!clientSimIsRunning(cs)) return;
  sdl3ImguiUpdatePlayerPing((unsigned char)value, ping);
}

/* Runs off the snapshot path, so it stays cheap: the badge cache the player
   list draws from is updated and nothing is drawn here. */
void frontEndUpdatePlayerFlags(ClientSim *cs, playerNumbers value,
                               uint8_t clientType, uint8_t clientFlags) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  sdl3ImguiUpdatePlayerFlags((unsigned char)value, clientType, clientFlags);
}

void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}

/* The auto-slowdown and auto-hide-gunsight prefs, from gamefront.c, as on
   the desktop. */
void frontEndApplyLocalTankPrefs(struct ClientSim *cs) {
  extern bool useAutoslow;
  extern bool useAutohide;
  if (cs == NULL) return;
  clientSimSetTankAutoSlowdown(cs, useAutoslow);
  clientSimSetTankAutoHideGunsight(cs, useAutohide);
}

void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled)   { (void)enabled; }

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  showGunsight = !isShown;
  clientSimSetGunsight(cs, showGunsight);
}

void frontEndShowAllianceRequest(char *playerName, BYTE playerNum) {
  sdl3ImguiShowAllianceRequest(playerName, playerNum);
}

/* frontEndTutorial and frontEndTutorialReset come from
   client_frontend_common.c, the tutorial step sequencer every platform
   calls. */

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

bool soundIsPlayingEffect(sndEffects value) { (void)value; return FALSE; }
