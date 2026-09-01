/*
 * Copyright (c) 1998-2026 John Morrison.
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
#include <string.h>   /* memcpy for the real messageInbox* ring below */
#include "global.h"
#include "scroll.h"
#include "frontend.h"

typedef struct ClientSim ClientSim;
#include "messages.h"
#include "pillbox.h"
#include "bases.h"
#include "tank.h"
#include "lgm.h"
#include "players.h"
#include "gametype.h"
#include "building.h"
#include "explosions.h"
#include "floodfill.h"
#include "grass.h"
#include "mines.h"
#include "minesexp.h"
#include "rubble.h"
#include "swamp.h"
#include "tankexp.h"
#include "sounddist.h"
#include "log.h"
#include "game_sim.h"
#include "client_sim.h"
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

/* Frontend stubs */
void frontEndUpdateTankStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees) { (void)cs; (void)shells; (void)mines; (void)armour; (void)trees; }
void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) { (void)cs; (void)shells; (void)mines; (void)armour; }
void frontEndPlaySound(ClientSim *cs, sndEffects value) { (void)cs; (void)value; }
void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms, int32_t srtDelay, bool isPillView, int edgeX, int edgeY) { (void)cs; (void)value; (void)mineView; (void)tks; (void)gs; (void)sBullet; (void)lgms; (void)srtDelay; (void)isPillView; (void)edgeX; (void)edgeY; }
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
void frontEndUpdatePlayerPing(struct ClientSim *cs, playerNumbers value, uint16_t ping) { (void)cs; (void)value; (void)ping; }
void frontEndUpdatePlayerFlags(struct ClientSim *cs, playerNumbers value, uint8_t clientType, uint8_t clientFlags) { (void)cs; (void)value; (void)clientType; (void)clientFlags; }
void frontEndDrawDownload(ClientSim *cs, bool justBlack) { (void)cs; (void)justBlack; }
void frontEndDrawReturningToLobby(ClientSim *cs) { (void)cs; }
void frontEndAudioReturningToLobby(bool active) { (void)active; }
void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) { (void)cs; (void)value; (void)isChecked; }
void frontEndApplyLocalTankPrefs(struct ClientSim *cs) { (void)cs; }
void frontEndSetActiveClientSim(struct ClientSim *cs) { (void)cs; }
void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled) { (void)enabled; }
void frontEndShowGunsight(ClientSim *cs, bool isShown) { (void)cs; (void)isShown; }
void frontEndRedrawAll(ClientSim *cs) { (void)cs; }
bool frontEndTutorial(BYTE pos) { (void)pos; return FALSE; }
void frontEndTutorialReset(void) { }

/* No screen stubs. screenTanksAddItem and screenLgmAddItem used to be
 * stubbed here: players.c calls them, so the symbols had to resolve, and
 * standing in for them kept screentank.c and screenlgm.c out of the link.
 * client_sim.c now calls screenTanksPrepare and screenLgmPrepare, which live
 * in those two files, so they are pulled from the archive either way and a
 * stub beside them is a duplicate definition. Nothing changes for the server:
 * the two builders are only ever reached through the prepare calls, which no
 * server code makes. */


/* clientSimIncomingMessage lives in client_sim.c (linked into WinBoloDS).
 * The body delegates to clientMessageAdd, which is stubbed below — so on
 * the server build the message-name lookup runs but the message itself is
 * silently dropped. clientApplySnapshot also lives in client_snapshot.c. */
void clientUiOnTick(ClientSim *cs, bool isBrain) { (void)cs; (void)isBrain; }
void messageCreate(MessageState *ms) { (void)ms; }
void messageDestroy(MessageState *ms) { (void)ms; }
void messageReset(MessageState *ms) { (void)ms; }
/* messageSet* — message-stream visibility toggles reached via
 * clientSimShowMessages. Bots have no message UI, so the toggles are
 * no-ops on the server build. */
void messageSetNewswire(MessageState *ms, bool isShown)  { (void)ms; (void)isShown; }
void messageSetAssistant(MessageState *ms, bool isShown) { (void)ms; (void)isShown; }
void messageSetAI(MessageState *ms, bool isShown)        { (void)ms; (void)isShown; }
void messageSetNetStatus(MessageState *ms, bool isShown) { (void)ms; (void)isShown; }
void messageSetNetwork(MessageState *ms, bool isShown)   { (void)ms; (void)isShown; }
/* messageIsNewMessage / messageGetNewMessage are called by brain_data.c
 * when building BrainInfo.  Bots have no chat inbox, so report "no message"
 * and never have GetNewMessage invoked. */
bool messageIsNewMessage(MessageState *ms) { (void)ms; return FALSE; }
BYTE messageGetNewMessage(MessageState *ms, char *dest, uint32_t **playerBitmap) {
  (void)ms; if (dest) dest[0] = '\0'; if (playerBitmap) *playerBitmap = NULL; return 0;
}
/* messageInbox* — brain-side per-tick chat inbox helpers reached from
 * brain_data.c's BrainInfo.messages population. These are REAL here (not
 * stubs): the inbox is bot-to-bot COORDINATION data, not a chat HUD — the
 * server hosts the bots, so they must actually receive each other's /info
 * messages. The old stubs (no-ops + Count==0) silently killed all bot comms
 * on WinBoloDS: botManagerDeliverInternalMessage "delivered" into a no-op push,
 * and every receiver saw Count==0. The bodies below are pure MessageState ring
 * operations (no GUI/archive deps), so they don't pull in messages.c.o and
 * don't conflict with the messageCreate/Destroy/etc. display stubs above. Keep
 * them byte-for-byte in sync with the canonical ring in src/bolo/messages.c. */
void messageInboxPush(MessageState *ms, BYTE from, const char *pascalText) {
  size_t plen, copyLen;
  if (ms == NULL || pascalText == NULL) return;
  if (ms->inboxCount >= BRAIN_INBOX_CAP) {
    ms->inboxHead = (ms->inboxHead + 1) % BRAIN_INBOX_CAP;
    ms->inboxCount--;
  }
  plen = (size_t)((unsigned char)pascalText[0]);
  if (plen + 2 > BRAIN_INBOX_MSG_LEN) plen = BRAIN_INBOX_MSG_LEN - 2;
  copyLen = plen + 1;
  memcpy(ms->inboxText[ms->inboxTail], pascalText, copyLen);
  ms->inboxText[ms->inboxTail][copyLen] = '\0';
  ms->inboxText[ms->inboxTail][0]       = (char)plen;
  ms->inboxFrom[ms->inboxTail]          = from;
  ms->inboxTail = (ms->inboxTail + 1) % BRAIN_INBOX_CAP;
  ms->inboxCount++;
}
int  messageInboxCount(const MessageState *ms) {
  return (ms != NULL) ? ms->inboxCount : 0;
}
BYTE messageInboxPeek(const MessageState *ms, int i, char *dest) {
  int slot; size_t plen;
  if (dest == NULL) return 0;
  if (ms == NULL || i < 0 || i >= ms->inboxCount) { dest[0] = '\0'; return 0; }
  slot = (ms->inboxHead + i) % BRAIN_INBOX_CAP;
  plen = (size_t)((unsigned char)ms->inboxText[slot][0]);
  if (plen + 2 > BRAIN_INBOX_MSG_LEN) plen = BRAIN_INBOX_MSG_LEN - 2;
  memcpy(dest, ms->inboxText[slot], plen + 1);
  dest[plen + 1] = '\0';
  return ms->inboxFrom[slot];
}
void messageInboxClear(MessageState *ms) {
  if (ms == NULL) return;
  ms->inboxHead = 0;
  ms->inboxTail = 0;
  ms->inboxCount = 0;
}
/* scrollCreate/scrollSetScrollType/scrollCenterObject/scrollManual are no
 * longer stubbed here. client_sim.c now references scrollGetMechanism /
 * scrollSetMechanism, which pulls scroll.c.o out of bolo_static, so the
 * real (sim-only, self-contained) scroll implementations resolve those
 * symbols. Keeping stubs too would be a duplicate definition. The server
 * never drives scrolling — the real scrollManual/scrollUpdate paths are
 * guarded by !isServer — so linking the real code is harmless. */


/* clientBuildInputPacket lives in client_snapshot.c; brainDataMakeInfo
   and brainDataExtractInfo live in brain_data.c (both linked into
   WinBoloDS). */
void clientMessageAdd(MessageState *ms, messageType msgType, char *top, char *bottom) { (void)ms; (void)msgType; (void)top; (void)bottom; }
void clientSoundDist(GameSim *sim, sndEffects value, BYTE mx, BYTE my) { (void)sim; (void)value; (void)mx; (void)my; }

/* Stubs for client-only subsystems that client_sim.c references */
void *dialogAllianceCreate(void) { return NULL; }
void dialogAllianceDestroy(void *dlg) { (void)dlg; }
bool dnsLookupsCreate(ClientSim *cs) { (void)cs; return TRUE; }
void dnsLookupsDestroy(void) {}
void dnsLookupsAddRequest(char *ip, void *func) { (void)ip; (void)func; }

/* Network stubs — netMessageSendPlayer, netRequestAlliance, netAllianceAccept,
 * netLeaveAlliance, netSetAllowNewPlayers are provided by client_sim.c. */
void netRemovePlayer(BYTE playerNum) { (void)playerNum; }
bool windowShowAllianceRequest(void) { return FALSE; }
void dialogAllianceSetName(char *playerName, BYTE playerNum) { (void)playerName; (void)playerNum; }
void netRequestStartPosition(void) {}
void netErrorOccured(void) {}
