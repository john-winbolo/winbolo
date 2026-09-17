/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * android_frontend.c - Frontend callbacks and window functions for Android
 *
 * Provides the frontend callbacks that the bolo engine calls, plus all
 * the window* function implementations. Modeled on src/wasm/main_wasm.c.
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
#include "players_panel.h"

/* -------------------------------------------------------
 * Externs from main_android.c
 * ------------------------------------------------------- */

extern bool isTutorial;
extern int frameRate;
extern bool showGunsight;
extern bool soundEffects;
extern bool backgroundSound;
extern bool useSoundKeepalive;
extern int  soundVolume;
extern int  windowMasterVolume;
extern bool allowNewPlayers;
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
extern bool isInMenu;
extern keyItems keys;

extern bool drawBusy;
extern bool winboloQuit;
extern bool finishedLoop;
extern bool showAllianceReq;

extern DWORD dwSysFrameTotal;
extern DWORD dwSysFrame;
extern DWORD dwSysGameTotal;
extern DWORD dwSysGame;
extern DWORD dwSysBrainTotal;
extern DWORD dwSysBrain;

extern DWORD oldTick;
extern DWORD ttick;
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

void windowZoomChange(BYTE amount, bool fromDragResize) {
  (void)fromDragResize;  /* Android doesn't use resize detection */
  if (amount == zoomFactor) return;
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] windowZoomChange: %d -> %d", zoomFactor, amount);
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
}

void windowSetFrameRate(int newFrameRate, bool setTimer) {
  (void)setTimer;
  frameRate = newFrameRate;
}

/* Menu toggles */
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
  soundSetEffectsVolume(pct);
}
void windowSetMasterVolume(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  windowMasterVolume = pct;
  soundSetMasterVolume(pct);
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

/* Window show/hide — no-ops for now (no ImGui panels on Android yet) */
void windowShowGameInfo(windowShowRequest req) { (void)req; }
void windowShowSysInfo(windowShowRequest req)  { (void)req; }
void windowShowNetInfo(windowShowRequest req)  { (void)req; }
void windowShowSetPlayerName(windowShowRequest req) { (void)req; }
void windowShowSendMessages(windowShowRequest req)  { (void)req; }
void windowShowSetKeys(windowShowRequest req)  { (void)req; }
void windowShowAboutBox(void)           { }
bool windowShowAllianceRequest(void)    { return FALSE; }
void windowDisableSound(void)           { soundEffects = FALSE; useSoundKeepalive = FALSE; }
bool windowGetBackgroundSound(void)     { return backgroundSound; }
void windowRedrawAll(ClientSim *cs) {
  clientMutexWaitFor();
  sdl3DrawRedrawAll(cs, clientSimGetCurrentBuildSelect(cs), NULL, showPillLabels, showBaseLabels);
  clientMutexRelease();
}
void *windowWnd(void) { return NULL; }

void windowSaveMap(ClientSim *cs) { (void)cs; }

void windowKeyPressed(ClientSim *cs, int keyCode) {
  if (keyCode == keys.kiTankView) clientSimTankView(cs);
  else if (keyCode == keys.kiPillView) clientSimPillView(cs, 0, 0);
}
void windowButtonAdd(int keyCode)    { (void)keyCode; }
void windowButtonRemove(int keyCode) { (void)keyCode; }
void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
  (void)xWin; (void)yWin; (void)xPos; (void)yPos;
}
void windowStartTutorial(void) { }
void windowAllowPlayerNameChange(bool allow) { (void)allow; }

/* Active-cs gate — see desktop frontend in src/gui/sdl3/winbolo.c for
 * the rationale. NULL = no registration yet; calls fall through. */
static ClientSim *s_activeUiCs = NULL;

void frontEndSetActiveClientSim(struct ClientSim *cs) {
  if (cs != s_activeUiCs) {
    /* Drop the previous game's latched build target: it is the square a tap
       builds at, and it outlives the ClientSim that set it, so without this
       the first tap of the next game is dispatched to a tile chosen in the
       last one. */
    buildCursorReset();
  }
  s_activeUiCs = cs;
}

/* -------------------------------------------------------
 * Frontend callbacks — called by backend (bolo engine)
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
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  BYTE fullShells, fullMines, fullArmour, fullTrees;
  clientSimGetTankFullStats(cs, &fullShells, &fullMines, &fullArmour, &fullTrees);
  sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees,
                         fullShells, fullMines, fullArmour, fullTrees);
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  BYTE fullShells, fullMines, fullArmour;
  clientSimGetBaseFullStats(cs, &fullShells, &fullMines, &fullArmour);
  sdl3DrawStatusBaseBars(0, 0, shells, mines, armour,
                         fullShells, fullMines, fullArmour, FALSE);
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void windowPlaySound(sndEffects value) {
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
  sdl3DrawCopyPillsStatus(0, 0);
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3DrawStatusTank(tankNum, ts);
  sdl3DrawCopyTanksStatus(0, 0);
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
  sdl3DrawCopyBasesStatus(0, 0);
}

void frontEndMessages(ClientSim *cs, char *top, char *bottom) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (drawBusy == FALSE) sdl3DrawMessages(0, 0, top, bottom);
}

void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (drawBusy == FALSE) sdl3DrawKillsDeaths(0, 0, kills, deaths);
}

void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  clientMutexWaitFor();
  sdl3DrawSetManStatus(0, 0, isDead, angle);
  clientMutexRelease();
}

void frontEndManClear(ClientSim *cs) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
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
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Game over (time limit expired)");
  finishedLoop = TRUE;
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
  cc[0] = countryCode[0];
  cc[1] = countryCode[1];
  cc[2] = '\0';
  sdl3ImguiSetPlayer((unsigned char)value, str, cc);
  sdl3ImguiUpdatePlayerMeta((unsigned char)value, ping, clientType, clientFlags);
}
void frontEndUpdatePlayerPing(ClientSim *cs, playerNumbers value, uint16_t ping) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (!clientSimIsRunning(cs)) return;
  sdl3ImguiUpdatePlayerPing((unsigned char)value, ping);
}
void frontEndUpdatePlayerFlags(ClientSim *cs, playerNumbers value, uint8_t clientType, uint8_t clientFlags) {
  (void)cs; (void)value; (void)clientType; (void)clientFlags;
}
void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}
void frontEndApplyLocalTankPrefs(struct ClientSim *cs) { (void)cs; }
void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled)   { (void)enabled; }

void frontEndRedrawAll(ClientSim *cs) { windowRedrawAll(cs); }

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  showGunsight = !isShown;
  clientSimSetGunsight(cs, showGunsight);
}

void frontEndShowAllianceRequest(char *playerName, BYTE playerNum) {
  sdl3ImguiShowAllianceRequest(playerName, playerNum);
}

bool frontEndTutorial(BYTE pos) {
  (void)pos;
  return FALSE;
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
  (void)playerName; (void)playerNum;
}

bool winUtilWBSubDirExist(char *subDirName) {
  struct stat st;
  if (stat(subDirName, &st) == 0 && S_ISDIR(st.st_mode)) {
    return TRUE;
  }
  return FALSE;
}

bool soundIsPlayingEffect(sndEffects value) { (void)value; return FALSE; }
