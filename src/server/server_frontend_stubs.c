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
 *  that bolo/*.c shared code calls. Used by server-only
 *  targets (WinBoloDS, WinBoloSimTest) that have no GUI.
 *  Client/headless targets get real implementations from
 *  screen.c and their respective frontends instead.
 *********************************************************/

#include <stdio.h>
#include "../bolo/global.h"
#include "../bolo/screen.h"
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
void frontEndUpdateTankStatusBars(BYTE shells, BYTE mines, BYTE armour, BYTE trees) { (void)shells; (void)mines; (void)armour; (void)trees; }
void frontEndUpdateBaseStatusBars(BYTE shells, BYTE mines, BYTE armour) { (void)shells; (void)mines; (void)armour; }
void screenLgmAddItem(screenLgm *value, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame) { (void)value; (void)mx; (void)my; (void)px; (void)py; (void)frame; }
void frontEndPlaySound(sndEffects value) { (void)value; }
void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms, int32_t srtDelay, bool isPillView, tank *tank, int edgeX, int edgeY) { (void)cs; (void)value; (void)mineView; (void)tks; (void)gs; (void)sBullet; (void)lgms; (void)srtDelay; (void)isPillView; (void)tank; (void)edgeX; (void)edgeY; }
void frontEndStatusPillbox(BYTE pillNum, pillAlliance pb) { (void)pillNum; (void)pb; }
void frontEndStatusTank(BYTE tankNum, tankAlliance ts) { (void)tankNum; (void)ts; }
void frontEndStatusBase(BYTE baseNum, baseAlliance bs) { (void)baseNum; (void)bs; }
void frontEndMessages(char *top, char *bottom) { (void)top; (void)bottom; }
void frontEndKillsDeaths(int kills, int deaths) { (void)kills; (void)deaths; }
void frontEndManStatus(bool isDead, TURNTYPE angle) { (void)isDead; (void)angle; }
void frontEndManClear(void) {}
void frontEndGameOver(void) {
  serverSimConsoleMessage("Game Timelimit has expired. Shutting down");
}
void frontEndClearPlayer(playerNumbers value) { (void)value; }
void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, bool wbnParticipant, bool steamParticipant) { (void)cs; (void)value; (void)str; (void)countryCode; (void)ping; (void)wbnParticipant; (void)steamParticipant; }
void frontEndDrawDownload(ClientSim *cs, bool justBlack) { (void)cs; (void)justBlack; }
void frontEndSetPlayerCheckState(playerNumbers value, bool isChecked) { (void)value; (void)isChecked; }
void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled) { (void)enabled; }
void frontEndShowGunsight(ClientSim *cs, bool isShown) { (void)cs; (void)isShown; }
void frontEndRedrawAll(ClientSim *cs) { (void)cs; }
bool frontEndTutorial(BYTE pos) { (void)pos; return FALSE; }

/* Screen stubs — only functions still called from bolo/ engine code in the server build */
bool screenIsItemInTrees(GameSim *sim, WORLD bmx, WORLD bmy) { (void)sim; (void)bmx; (void)bmy; return TRUE; }
void screenTanksAddItem(screenTanks *value, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame, BYTE playerNum, char *playerName) { (void)value; (void)mx; (void)my; (void)px; (void)py; (void)frame; (void)playerNum; (void)playerName; }
/* screenAddBrainObject is defined here only for targets that lack screen.c
 * (i.e. WinBoloDS).  bot_manager routes objects to each bot's own ClientSim
 * via the cs parameter, so this stub is a no-op. */
#ifndef HAVE_SCREEN_C
void screenAddBrainObject(ClientSim *cs, unsigned short object, WORLD wx, WORLD wy, unsigned short idNum, BYTE dir, BYTE info, BYTE speed) {
  (void)cs; (void)object; (void)wx; (void)wy; (void)idNum; (void)dir; (void)info; (void)speed;
}
#endif
void screenNetStatusMessage(ClientSim *csPtr, char *messageStr) { (void)csPtr; (void)messageStr; }


/* Screen CS stubs — engine code calls these but the server has no display.
   The isServer guards should prevent execution, but the linker needs symbols. */
void screenReCalcCS(ClientSim *cs) { (void)cs; }
bool screenTankScrollCS(ClientSim *cs) { (void)cs; return FALSE; }
void screenMoveViewOffsetUpCS(ClientSim *cs, bool isUp) { (void)cs; (void)isUp; }
void screenMoveViewOffsetLeftCS(ClientSim *cs, bool isLeft) { (void)cs; (void)isLeft; }
void screenIncomingMessageCS(ClientSim *cs, BYTE playerNum, char *messageStr) { (void)cs; (void)playerNum; (void)messageStr; }

/* Screen / display stubs — client_sim.c calls these but the server has no display */
void screenTankViewCS(ClientSim *cs) { (void)cs; }
void screenSyncFromSnapshotCS(ClientSim *cs,
    const SnapshotHeader *hdr,
    const TankSnapshot *tanks, int tankCount,
    const ShellSnapshot *shellSnaps, int shellCount,
    const ExplosionSnapshot *explSnaps, int explosionCount,
    const BaseSnapshot *baseSnaps, int baseCount,
    const PillSnapshot *pillSnaps, int pillCount,
    const GameEvent *events, int eventCount,
    BYTE playerNum) {
  (void)cs; (void)hdr; (void)tanks; (void)tankCount;
  (void)shellSnaps; (void)shellCount; (void)explSnaps; (void)explosionCount;
  (void)baseSnaps; (void)baseCount; (void)pillSnaps; (void)pillCount;
  (void)events; (void)eventCount; (void)playerNum;
}
void screenSimDisplayTickCS(ClientSim *cs, bool isBrain) { (void)cs; (void)isBrain; }
void messageCreate(MessageState *ms) { (void)ms; }
void messageDestroy(MessageState *ms) { (void)ms; }
void scrollCreate(ScrollState *ss) { (void)ss; }


/* Client-side stubs — the server has no ClientSim; these calls are guarded
   by isServer==FALSE checks so they never execute, but the linker needs
   the symbols. */
void screenBuildInputPacketCS(ClientSim *cs, InputPacket *pkt, tankButton tb, bool isShoot, bool isMine, bool isBrain, bool isGameTick, BYTE playerNum, uint32_t tick) { (void)cs; (void)pkt; (void)tb; (void)isShoot; (void)isMine; (void)isBrain; (void)isGameTick; (void)playerNum; (void)tick; }
void screenMakeBrainInfoCS(ClientSim *cs, BrainInfo *value, bool first, aiType aiMode) { (void)cs; (void)value; (void)first; (void)aiMode; }
void screenExtractBrainInfoCS(ClientSim *cs, BrainInfo *value) { (void)cs; (void)value; }
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
