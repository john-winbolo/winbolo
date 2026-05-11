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
 *Name:          Server Frontend Stubs
 *Filename:      server_frontend_stubs.c
 *Author:        John Morrison
 *Purpose:
 *  No-op stubs for frontend, client, and screen functions
 *  that bolo/ *.c shared code calls. Used by server-only
 *  targets (WinBoloDS, WinBoloSimTest) that have no GUI.
 *  Client/headless targets get real implementations from
 *  screen.c and their respective frontends instead.
 *********************************************************/

#include <stdio.h>
#include "../bolo/global.h"
#include "../bolo/screen.h"
#include "../bolo/scroll.h"
#include "../bolo/frontend.h"

typedef struct ClientSim ClientSim;
#include "../bolo/messages.h"
#include "../bolo/pillbox.h"
#include "../bolo/bases.h"
#include "../bolo/tank.h"
#include "../bolo/lgm.h"
#include "../bolo/players.h"
#include "../bolo/gametype.h"
#include "../bolo/building.h"
#include "../bolo/explosions.h"
#include "../bolo/floodfill.h"
#include "../bolo/grass.h"
#include "../bolo/mines.h"
#include "../bolo/minesexp.h"
#include "../bolo/rubble.h"
#include "../bolo/swamp.h"
#include "../bolo/tankexp.h"
#include "../bolo/sounddist.h"
#include "../bolo/log.h"
#include "../bolo/game_sim.h"
#include "../bolo/client_sim.h"
#include "../gui/clientmutex.h"
#include "../gui/gamefront.h"
#include "server_sim.h"

/* clientMutex stubs — the dedicated server is single-threaded on the
   client side so these are intentional no-ops. */
bool clientMutexCreate(void)    { return TRUE; }
void clientMutexDestroy(void)   {}
void clientMutexWaitFor(void)   {}
bool clientMutexTryWaitFor(void){ return TRUE; }
void clientMutexRelease(void)   {}

/* serverCoreSoundDist — logs sound events for game recording.
   In client/headless builds this is defined in screen.c instead. */
void serverCoreSoundDist(sndEffects value, BYTE mx, BYTE my) {
  BYTE logMessageType = 0;
  switch (value) {
  case shootSelf: case shootNear: case shootFar:
    logMessageType = log_SoundShoot; break;
  case shotTreeNear: case shotTreeFar:
    logMessageType = log_SoundHitTree; break;
  case shotBuildingNear: case shotBuildingFar:
    logMessageType = log_SoundHitWall; break;
  case hitTankNear: case hitTankFar: case hitTankSelf:
    logMessageType = log_SoundHitTank; break;
  case bubbles: case tankSinkNear: case tankSinkFar:
    break;
  case bigExplosionNear: case bigExplosionFar:
    logMessageType = log_SoundBigExplosion; break;
  case farmingTreeNear: case farmingTreeFar:
    logMessageType = log_SoundFarm; break;
  case manBuildingNear: case manBuildingFar:
    logMessageType = log_SoundBuild; break;
  case manDyingNear: case manDyingFar:
    logMessageType = log_SoundManDie; break;
  case manLayingMineNear:
    logMessageType = log_SoundMineLay; break;
  case mineExplosionNear: case mineExplosionFar:
    logMessageType = log_SoundMineExplode; break;
  }
  if (logMessageType) {
    logAddEvent(logMessageType, mx, my, 0, 0, 0, NULL);
  }
}

/* Frontend stubs */
void frontEndUpdateTankStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees) { (void)cs; (void)shells; (void)mines; (void)armour; (void)trees; }
void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) { (void)cs; (void)shells; (void)mines; (void)armour; }
void screenLgmAddItem(screenLgm *value, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame) { (void)value; (void)mx; (void)my; (void)px; (void)py; (void)frame; }
void frontEndPlaySound(ClientSim *cs, sndEffects value) { (void)cs; (void)value; }
void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms, int32_t srtDelay, bool isPillView, tank *tank, int edgeX, int edgeY) { (void)cs; (void)value; (void)mineView; (void)tks; (void)gs; (void)sBullet; (void)lgms; (void)srtDelay; (void)isPillView; (void)tank; (void)edgeX; (void)edgeY; }
void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) { (void)cs; (void)pillNum; (void)pb; }
void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) { (void)cs; (void)tankNum; (void)ts; }
void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) { (void)cs; (void)baseNum; (void)bs; }
void frontEndMessages(ClientSim *cs, char *top, char *bottom) { (void)cs; (void)top; (void)bottom; }
void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) { (void)cs; (void)kills; (void)deaths; }
void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) { (void)cs; (void)isDead; (void)angle; }
void frontEndManClear(ClientSim *cs) { (void)cs; }
void frontEndGameOver(ClientSim *cs) {
  (void)cs;
  serverSimConsoleMessage("Game Timelimit has expired. Shutting down");
}
void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) { (void)cs; (void)value; }
void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) { (void)cs; (void)value; (void)str; (void)countryCode; (void)ping; (void)clientType; (void)clientFlags; }
void frontEndDrawDownload(ClientSim *cs, bool justBlack) { (void)cs; (void)justBlack; }
void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) { (void)cs; (void)value; (void)isChecked; }
void frontEndSetActiveClientSim(struct ClientSim *cs) { (void)cs; }
void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled) { (void)enabled; }
void frontEndShowGunsight(ClientSim *cs, bool isShown) { (void)cs; (void)isShown; }
void frontEndRedrawAll(ClientSim *cs) { (void)cs; }
bool frontEndTutorial(BYTE pos) { (void)pos; return FALSE; }
void frontEndTutorialReset(void) { }

/* Screen stubs — only functions still called from bolo/ engine code in the server build */
bool screenIsItemInTrees(GameSim *sim, tank viewerTank, WORLD bmx, WORLD bmy) { (void)sim; (void)viewerTank; (void)bmx; (void)bmy; return TRUE; }
void screenTanksAddItem(screenTanks *value, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame, BYTE playerNum, char *playerName) { (void)value; (void)mx; (void)my; (void)px; (void)py; (void)frame; (void)playerNum; (void)playerName; }
void screenNetStatusMessage(ClientSim *csPtr, char *messageStr) { (void)csPtr; (void)messageStr; }
/* clientCenterTankCS / screenPillViewCS — display-only helpers called from
 * client_snapshot.c.  Bots never invoke them (guarded by isHuman / view-key
 * tests), but the linker still needs the symbol. */
void clientCenterTankCS(ClientSim *csPtr) { (void)csPtr; }
void screenPillViewCS(ClientSim *csPtr, int horz, int vert) { (void)csPtr; (void)horz; (void)vert; }


/* Screen CS stubs — engine code calls these but the server has no display.
   The isServer guards should prevent execution, but the linker needs symbols. */
void screenReCalcCS(ClientSim *cs) { (void)cs; }
bool screenTankScrollCS(ClientSim *cs) { (void)cs; return FALSE; }
void screenMoveViewOffsetUpCS(ClientSim *cs, bool isUp) { (void)cs; (void)isUp; }
void screenMoveViewOffsetLeftCS(ClientSim *cs, bool isLeft) { (void)cs; (void)isLeft; }
void screenIncomingMessageCS(ClientSim *cs, BYTE playerNum, char *messageStr) { (void)cs; (void)playerNum; (void)messageStr; }

/* Screen / display stubs — client_sim.c calls these but the server has no display */
void screenTankViewCS(ClientSim *cs) { (void)cs; }
/* screenSyncFromSnapshotCS lives in client_snapshot.c (linked into WinBoloDS) */
void screenSimDisplayTickCS(ClientSim *cs, bool isBrain) { (void)cs; (void)isBrain; }
void messageCreate(MessageState *ms) { (void)ms; }
void messageDestroy(MessageState *ms) { (void)ms; }
/* messageIsNewMessage / messageGetNewMessage are called by brain_data.c
 * when building BrainInfo.  Bots have no chat inbox, so report "no message"
 * and never have GetNewMessage invoked. */
bool messageIsNewMessage(MessageState *ms) { (void)ms; return FALSE; }
BYTE messageGetNewMessage(MessageState *ms, char *dest, uint32_t **playerBitmap) {
  (void)ms; if (dest) dest[0] = '\0'; if (playerBitmap) *playerBitmap = NULL; return 0;
}
void scrollCreate(ScrollState *ss) { (void)ss; }
/* scrollCenterObject is called by client_snapshot.c only when the brain
 * switches pillbox view — bots never do this, but the linker still needs
 * the symbol. */
void scrollCenterObject(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY) {
  (void)ss; (void)xValue; (void)yValue; (void)objectX; (void)objectY;
}


/* screenBuildInputPacketCS lives in client_snapshot.c; screenMakeBrainInfoCS
   and screenExtractBrainInfoCS live in brain_data.c (both linked into
   WinBoloDS). */
void clientMessageAdd(MessageState *ms, messageType msgType, char *top, char *bottom) { (void)ms; (void)msgType; (void)top; (void)bottom; }
void clientSoundDist(GameSim *sim, sndEffects value, BYTE mx, BYTE my) { (void)sim; (void)value; (void)mx; (void)my; }

/* Stubs for client-only subsystems that client_sim.c references */
void *dialogAllianceCreate(void) { return NULL; }
void dialogAllianceDestroy(void *dlg) { (void)dlg; }
bool dnsLookupsCreate(ClientSim *cs) { (void)cs; return TRUE; }
void dnsLookupsDestroy(void) {}

/* Network stubs — netMessageSendPlayer, netRequestAlliance, netAllianceAccept,
 * netLeaveAlliance, netSetAllowNewPlayers are provided by client_sim.c. */
void netRemovePlayer(BYTE playerNum) { (void)playerNum; }
bool windowShowAllianceRequest(void) { return FALSE; }
void dialogAllianceSetName(char *playerName, BYTE playerNum) { (void)playerName; (void)playerNum; }
void netRequestStartPosition(void) {}
void netErrorOccured(void) {}
