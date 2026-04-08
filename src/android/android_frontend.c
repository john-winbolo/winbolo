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

#include "../bolo/screen.h"
#include "../bolo/client_sim.h"
#include "../bolo/frontend.h"
#include "../bolo/gui_message.h"
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
#include "../gui/aresource.h"
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
extern bool hideMainView;
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
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,
                           title ? title : "WinBolo",
                           message ? message : "",
                           sdl3DrawGetWindow());
}

/* -------------------------------------------------------
 * winbolo.h function implementations
 * ------------------------------------------------------- */

void windowReCreate(void)  { }
void windowSetQuitting(void) { winboloQuit = TRUE; finishedLoop = TRUE; }

void windowApplyMenuChecks(ClientSim *cs) {
  screenSetGunsightCS(cs, showGunsight);
  screenSetAutoScroll(cs, autoScrollingEnabled);
  screenSetLabelOwnTank(cs, labelSelf);
  screenSetMesageLabelLen(cs, labelMsg);
  screenSetTankLabelLen(cs, labelTank);
  screenShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages);
  screenShowMessages(cs, MSG_ASSISTANT, showAssistantMessages);
  screenShowMessages(cs, MSG_AI, showAIMessages);
  screenShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages);
  screenShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages);
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

void windowZoomChange(BYTE amount) {
  if (amount == zoomFactor) return;
  SDL_Log("[Android] windowZoomChange: %d -> %d", zoomFactor, amount);
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
  screenSetGunsightCS(cs, showGunsight);
}
void windowAutomaticScrolling_toggle(ClientSim *cs) {
  autoScrollingEnabled = !autoScrollingEnabled;
  if (cs) screenSetAutoScroll(cs, autoScrollingEnabled);
}
void windowShowPillLabels_toggle(ClientSim *cs) {
  BYTE count, total;
  showPillLabels = !showPillLabels;
  sdl3DrawSetPillsStatusClear();
  total = pillsGetNumPills(&cs->sim.pb);
  for (count = 1; count <= total; count++) {
    BYTE pillStat = screenPillAllianceCS(cs, count);
    sdl3DrawStatusPillbox(count, pillStat, showPillLabels);
  }
  sdl3DrawCopyPillsStatus(0, 0);
}
void windowShowBaseLabels_toggle(ClientSim *cs) {
  BYTE count, total;
  showBaseLabels = !showBaseLabels;
  sdl3DrawSetBasesStatusClear();
  total = basesGetNumBases(&cs->sim.bs);
  for (count = 1; count <= total; count++) {
    BYTE baseStat = screenBaseAllianceCS(cs, count);
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
void windowMenuNewswire_toggle(ClientSim *cs)    { showNewswireMessages = !showNewswireMessages; if (cs) screenShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages); }
void windowMenuAssistant_toggle(ClientSim *cs)   { showAssistantMessages = !showAssistantMessages; if (cs) screenShowMessages(cs, MSG_ASSISTANT, showAssistantMessages); }
void windowMenuAI_toggle(ClientSim *cs)          { showAIMessages = !showAIMessages; if (cs) screenShowMessages(cs, MSG_AI, showAIMessages); }
void windowMenuNetwork_toggle(ClientSim *cs)     { showNetworkStatusMessages = !showNetworkStatusMessages; if (cs) screenShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages); }
void windowMenuNetworkDebug_toggle(ClientSim *cs){ showNetworkDebugMessages = !showNetworkDebugMessages; if (cs) screenShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages); }
void windowHideMainView_toggle(void)    { hideMainView = !hideMainView; }
void windowLabelOwnTank_toggle(ClientSim *cs)    { labelSelf = !labelSelf; if (cs) screenSetLabelOwnTank(cs, labelSelf); }
void windowSetMessageLabelLen(ClientSim *cs, labelLen n){ labelMsg = n; if (cs) screenSetMesageLabelLen(cs, labelMsg); }
void windowSetTankLabelLen(ClientSim *cs, labelLen n)   { labelTank = n; if (cs) screenSetTankLabelLen(cs, labelTank); }
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
  sdl3DrawRedrawAll(cs, getBuildCurrentSelectCS(cs), NULL, showPillLabels, showBaseLabels);
  clientMutexRelease();
}
void *windowWnd(void) { return NULL; }

void windowSaveMap(ClientSim *cs) { (void)cs; }

void windowKeyPressed(ClientSim *cs, int keyCode) {
  if (keyCode == keys.kiTankView) screenTankViewCS(cs);
  else if (keyCode == keys.kiPillView) screenPillViewCS(cs, 0, 0);
}
void windowButtonAdd(int keyCode)    { (void)keyCode; }
void windowButtonRemove(int keyCode) { (void)keyCode; }
void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
  (void)xWin; (void)yWin; (void)xPos; (void)yPos;
}
void windowStartTutorial(void) { }
void windowAllowPlayerNameChange(bool allow) { (void)allow; }

/* -------------------------------------------------------
 * Frontend callbacks — called by backend (bolo engine)
 * ------------------------------------------------------- */
void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, tank *tank, int edgeX, int edgeY) {
  if (hideMainView == FALSE && drawBusy == FALSE) {
    BYTE cursorX, cursorY;
    bool showCursor = screenGetCursorPosCS(cs, &cursorX, &cursorY);
    sdl3DrawSetNetFailed(cs->netStat == netFailed);
    sdl3DrawMainScreen(cs, value, mineView, tks, gs, sBullet, lgms,
                       NULL, showPillLabels, showBaseLabels,
                       srtDelay, isPillView, edgeX, edgeY,
                       showCursor, cursorX, cursorY, tank);
  }
}

void frontEndUpdateTankStatusBars(BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  if (armour > TANK_FULL_ARMOUR) {
    armour = 0;
  }
  sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees);
}

void frontEndUpdateBaseStatusBars(BYTE shells, BYTE mines, BYTE armour) {
  sdl3DrawStatusBaseBars(0, 0, shells, mines, armour, FALSE);
}

void frontEndPlaySound(sndEffects value) {
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void windowPlaySound(sndEffects value) {
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void frontEndStatusPillbox(BYTE pillNum, pillAlliance pb) {
  sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
  sdl3DrawCopyPillsStatus(0, 0);
}

void frontEndStatusTank(BYTE tankNum, tankAlliance ts) {
  sdl3DrawStatusTank(tankNum, ts);
  sdl3DrawCopyTanksStatus(0, 0);
}

void frontEndStatusBase(BYTE baseNum, baseAlliance bs) {
  sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
  sdl3DrawCopyBasesStatus(0, 0);
}

void frontEndMessages(char *top, char *bottom) {
  if (drawBusy == FALSE) sdl3DrawMessages(0, 0, top, bottom);
}

void frontEndKillsDeaths(int kills, int deaths) {
  if (drawBusy == FALSE) sdl3DrawKillsDeaths(0, 0, kills, deaths);
}

void frontEndManStatus(bool isDead, TURNTYPE angle) {
  clientMutexWaitFor();
  sdl3DrawSetManStatus(0, 0, isDead, angle);
  clientMutexRelease();
}

void frontEndManClear(void) {
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

void frontEndGameOver(void) {
  SDL_Log("[Android] Game over (time limit expired)");
  finishedLoop = TRUE;
}

void frontEndClearPlayer(playerNumbers value) {
  sdl3ImguiClearPlayer((unsigned char)value);
}
void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, bool wbnParticipant, bool steamParticipant) {
  char cc[3];
  if (!screenGetGameRunningCS(cs)) {
    cc[0] = 'X'; cc[1] = 'X'; cc[2] = '\0';
    sdl3ImguiSetPlayer((unsigned char)value, str, cc);
    return;
  }
  cc[0] = countryCode[0];
  cc[1] = countryCode[1];
  cc[2] = '\0';
  sdl3ImguiSetPlayer((unsigned char)value, str, cc);
  sdl3ImguiUpdatePlayerMeta((unsigned char)value, ping, wbnParticipant, steamParticipant);
}
void frontEndSetPlayerCheckState(playerNumbers value, bool isChecked) {
  sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}
void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled)   { (void)enabled; }

void frontEndRedrawAll(ClientSim *cs) { windowRedrawAll(cs); }

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
  showGunsight = !isShown;
  screenSetGunsightCS(cs, showGunsight);
}

void frontEndShowAllianceRequest(char *playerName, BYTE playerNum) {
  sdl3ImguiShowAllianceRequest(playerName, playerNum);
}

bool frontEndTutorial(BYTE pos) {
  (void)pos;
  return FALSE;
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
