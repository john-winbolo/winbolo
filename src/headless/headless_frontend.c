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
*Name:          Headless Frontend
*Filename:      headless_frontend.c
*Purpose:
*  Stub implementations of frontend display callbacks
*  for the headless client. These are called by the game
*  engine but have no visual effect in headless mode.
*
*  Modeled after src/server/serverfrontend.c but for
*  a client context (so we don't stub network/screen
*  functions that have real implementations in the
*  bolo engine).
*********************************************************/

#include <stdio.h>
#include <SDL3/SDL.h>
#include "../bolo/global.h"
#include "../bolo/client_sim.h"
#include "../bolo/frontend.h"

/* ================================================================== */
/* Frontend display callbacks (all no-ops in headless mode)            */
/* ================================================================== */

void frontEndUpdateTankStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  (void)cs; (void)shells; (void)mines; (void)armour; (void)trees;
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
  (void)cs; (void)shells; (void)mines; (void)armour;
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
  (void)cs; (void)value;
}

void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, tank *tank,
                            int edgeX, int edgeY) {
  (void)cs; (void)value; (void)mineView; (void)tks; (void)gs; (void)sBullet;
  (void)lgms; (void)srtDelay; (void)isPillView; (void)tank;
  (void)edgeX; (void)edgeY;
}

void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
  (void)cs; (void)pillNum; (void)pb;
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
  (void)cs; (void)tankNum; (void)ts;
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
  (void)cs; (void)baseNum; (void)bs;
}

void frontEndMessages(ClientSim *cs, char *top, char *bottom) {
  (void)cs; (void)top; (void)bottom;
}

void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) {
  (void)cs; (void)kills; (void)deaths;
}

void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) {
  (void)cs; (void)isDead; (void)angle;
}

void frontEndManClear(ClientSim *cs) {
  (void)cs;
}

void frontEndGameOver(ClientSim *cs) {
  (void)cs;
  fprintf(stderr, "[headless] Game over (time limit expired)\n");
}

void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) {
  (void)cs; (void)value;
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) {
  (void)cs; (void)value; (void)str; (void)countryCode; (void)ping; (void)clientType; (void)clientFlags;
}

void frontEndDrawDownload(ClientSim *cs, bool justBlack) {
  (void)cs; (void)justBlack;
}

void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
  (void)cs; (void)value; (void)isChecked;
}

void frontEndSetActiveClientSim(struct ClientSim *cs) {
  (void)cs;
}

void frontEndEnableRequestAllyMenu(bool enabled) {
  (void)enabled;
}

void frontEndEnableLeaveAllyMenu(bool enabled) {
  (void)enabled;
}

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
  (void)cs; (void)isShown;
}

void frontEndRedrawAll(ClientSim *cs) {
  (void)cs;
}

bool frontEndTutorial(BYTE pos) {
  (void)pos;
  return FALSE;
}

void frontEndTutorialReset(void) { }

/* ================================================================== */
/* Screen/draw stubs not needed in headless mode                       */
/* ================================================================== */

/* screenReCalc is in screen.c, clientSoundDist is in sounddist.c */

/* ================================================================== */
/* Window/input stubs                                                  */
/* ================================================================== */

void windowRedrawAll(ClientSim *cs) {
  (void)cs;
}

bool windowShowAllianceRequest(void) {
  return FALSE;
}

void windowAllowPlayerNameChange(bool allow) {
  (void)allow;
}

/* ================================================================== */
/* Sound stubs                                                         */
/* ================================================================== */

bool soundSetup(void) {
  return TRUE;
}

void soundCleanup(void) {
}

void soundPlayEffect(sndEffects value) {
  (void)value;
}

void soundKeepalive(bool on) {
  (void)on;
}

bool soundIsPlayingEffect(sndEffects value) {
  (void)value;
  return FALSE;
}

/* ================================================================== */
/* Draw stubs — sdl3draw.c functions referenced by the engine          */
/* ================================================================== */

/* screenUpdate is in screen.c and calls frontEndDrawMainScreen
 * which we've already stubbed. But if called directly: */

/* ================================================================== */
/* Miscellaneous stubs                                                 */
/* ================================================================== */

/* netErrorOccured is in network.c */

/* Skins - not applicable */
void gameFrontReloadSkins(void) {
}

void gameFrontShutdownServer(void) {
}

/* ================================================================== */
/* Dialog Alliance stubs (called from network.c)                       */
/* ================================================================== */

void *dialogAllianceCreate(void) {
  return NULL;
}

void dialogAllianceDestroy(void *dlg) {
  (void)dlg;
}

void dialogAllianceSetName(char *playerName, BYTE playerNum) {
  (void)playerName; (void)playerNum;
}

/* ================================================================== */
/* Cursor stub (called from screen.c during map scrolling)             */
/* ================================================================== */

void moveMousePointer(updateType value) {
  (void)value;
}
