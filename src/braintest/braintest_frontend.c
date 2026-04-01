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
 * Name:          braintest_frontend.c
 * Purpose:
 *   Stub implementations of frontend display callbacks
 *   for the BrainTest standalone app. Similar to the
 *   headless frontend stubs.
 *********************************************************/

#include <stdio.h>
#include <SDL3/SDL.h>
#include "../bolo/global.h"
#include "../bolo/screen.h"
#include "../bolo/client_sim.h"
#include "../bolo/frontend.h"
#include "../server/server_sim.h"

void frontEndUpdateTankStatusBars(BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  (void)shells; (void)mines; (void)armour; (void)trees;
}

void frontEndUpdateBaseStatusBars(BYTE shells, BYTE mines, BYTE armour) {
  (void)shells; (void)mines; (void)armour;
}

void frontEndPlaySound(sndEffects value) {
  (void)value;
}

void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, tank *tank,
                            int edgeX, int edgeY) {
  (void)cs; (void)value; (void)mineView; (void)tks; (void)gs; (void)sBullet;
  (void)lgms; (void)srtDelay; (void)isPillView; (void)tank;
  (void)edgeX; (void)edgeY;
}

void frontEndStatusPillbox(BYTE pillNum, pillAlliance pb) {
  (void)pillNum; (void)pb;
}

void frontEndStatusTank(BYTE tankNum, tankAlliance ts) {
  (void)tankNum; (void)ts;
}

void frontEndStatusBase(BYTE baseNum, baseAlliance bs) {
  (void)baseNum; (void)bs;
}

void frontEndMessages(char *top, char *bottom) {
  (void)top; (void)bottom;
}

void frontEndKillsDeaths(int kills, int deaths) {
  (void)kills; (void)deaths;
}

void frontEndManStatus(bool isDead, TURNTYPE angle) {
  (void)isDead; (void)angle;
}

void frontEndManClear(void) {
}

void frontEndGameOver(void) {
  fprintf(stderr, "[braintest] Game over\n");
}

void frontEndClearPlayer(playerNumbers value) {
  (void)value;
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, bool wbnParticipant, bool steamParticipant) {
  (void)cs; (void)value; (void)str; (void)countryCode; (void)ping; (void)wbnParticipant; (void)steamParticipant;
}

void frontEndDrawDownload(ClientSim *cs, bool justBlack) {
  (void)cs; (void)justBlack;
}

void frontEndSetPlayerCheckState(playerNumbers value, bool isChecked) {
  (void)value; (void)isChecked;
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

void windowRedrawAll(ClientSim *cs) {
  (void)cs;
}

bool windowShowAllianceRequest(void) {
  return FALSE;
}

void windowAllowPlayerNameChange(bool allow) {
  (void)allow;
}

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

void gameFrontReloadSkins(void) {
}

void gameFrontShutdownServer(void) {
}

void *dialogAllianceCreate(void) {
  return NULL;
}

void dialogAllianceDestroy(void *dlg) {
  (void)dlg;
}

void dialogAllianceSetName(char *playerName, BYTE playerNum) {
  (void)playerName; (void)playerNum;
}

void moveMousePointer(updateType value) {
  (void)value;
}

/* DNS / network stubs */
bool dnsLookupsCreate(ClientSim *cs) { (void)cs; return TRUE; }
void dnsLookupsDestroy(void) {}
void netRemovePlayer(BYTE playerNum) { (void)playerNum; }
void netRequestStartPosition(void) {}
void netErrorOccured(void) {}

/* WinBoloNet stubs (client_sim.c / bases.c / log.c reference these) */
bool winbolonetIsRunning(void) { return FALSE; }
void winbolonetDestroy(bool isServer) { (void)isServer; }
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB) {
  (void)eventType; (void)isServer; (void)playerA; (void)playerB;
}
void winboloNetGetServerKey(char *keyBuff) { if (keyBuff) keyBuff[0] = '\0'; }
void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
  (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}
void winboloNetSendLock(bool isLocked) { (void)isLocked; }
bool winboloNetIsPlayerParticipant(BYTE playerNum) { (void)playerNum; return FALSE; }
bool winbolonetServerVerifyToken(const char *token, BYTE playerNum, char *errorMsg, bool *hasSteam) {
  (void)token; (void)playerNum; (void)errorMsg;
  if (hasSteam) *hasSteam = FALSE;
  return FALSE;
}

bool winbolonetServerRequestBalance(uint8_t totalPlayers, uint8_t teamSize, BalanceProposal *outProposal) {
  (void)totalPlayers; (void)teamSize; (void)outProposal;
  return FALSE;
}

/* threads stubs (clientmutex.c references these) */
void threadsWaitForMutex(void) {}
bool threadsTryWaitForMutex(void) { return TRUE; }
void threadsReleaseMutex(void) {}

/* geolookup stub (transport_udp.c references this) */
bool geoLookupCountry(const char *ipStr, char countryCode[3]) {
  (void)ipStr; countryCode[0] = '\0'; return FALSE;
}
