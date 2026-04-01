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
 *Name:          Client Simulation
 *Filename:      client_sim.c
 *Author:        John Morrison
 *Purpose:
 *  ClientSim lifecycle API and tick functions.
 *  Moved from screen.c as part of the client/server
 *  simulation separation.
 *********************************************************/

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "client_sim.h"
#include "client_state.h"
#include "screen.h"
#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "shells.h"
#include "rubble.h"
#include "explosions.h"
#include "messages.h"
#include "grass.h"
#include "swamp.h"
#include "lgm.h"
#include "floodfill.h"
#include "minesexp.h"
#include "treegrow.h"
#include "mines.h"
#include "labels.h"
#include "players.h"
#include "tankexp.h"
#include "log.h"
#include "screenbrainmap.h"
#include "util.h"
#include "netpacks.h"
#include "../gui/lang.h"
#include "../gui/dnsLookups.h"
#include "../gui/clientmutex.h"
#include "../gui/dialogAlliance.h"
#include "../winbolonet/winbolonet.h"
#include "frontend.h"

/* Must match the value used inside shellsAddItem (shells.c redefines
 * SHELL_START_ADD from 6 to 5 locally). */
#undef SHELL_START_ADD
#define SHELL_START_ADD 5

/* Forward declarations */
static void clientSimAddPredictedShellAt(ClientSim *cs, WORLD wx, WORLD wy, TURNTYPE angle, tank *tk, uint32_t fireTick);

/* GameSim callback wrappers for the client side */
static void csCallbackMessageAdd(void *ctx, messageType msgType, char *top, char *bottom) {
  clientMessageAdd(msgType, top, bottom);
}

static void csCallbackSoundDist(void *ctx, sndEffects value, BYTE mx, BYTE my) {
  ClientSim *cs = (ClientSim *)ctx;
  clientSoundDist(&cs->sim, value, mx, my);
}

static void csCallbackCenterTank(void *ctx) {
  ClientSim *cs = (ClientSim *)ctx;
  screenTankViewCS(cs);
}

static void csCallbackSoundDistShoot(void *ctx, BYTE mx, BYTE my, BYTE owner) {
  /* no-op — client plays shootSelf via prediction; other players' shoots arrive via EVENT_SOUND_SHOOT */
}

static void csCallbackSoundDistTankHit(void *ctx, BYTE mx, BYTE my, BYTE hitPlayer) {
  /* no-op — client receives tank hit sounds via EVENT_SOUND_TANK_HIT from server */
}

static void csCallbackTankKill(void *ctx, BYTE killer, BYTE killed) {
  /* no-op — client receives kills via EVENT_TANK_KILLED from server */
}

static void csCallbackConsoleMessage(void *ctx, char *msg) {
  /* no-op on the client */
}

/* Brain state now lives inside the ClientSim struct (see client_sim.h).
 * The static globals were removed in the Phase 0 refactor. */

/*********************************************************
 *NAME:          clientSimCreate
 *PURPOSE:
 *  Initializes a ClientSim struct with all simulation state.
 *  This is the simulation-only initialization; rendering
 *  setup is handled separately by screenRenderSetup().
 *
 *ARGUMENTS:
 *  cs         - Pointer to the ClientSim to initialize
 *  game       - The game type-Open/tournament/strict tournament
 *  hiddenMines - Are hidden mines allowed
 *  srtDelay   - Game start delay (50th second increments)
 *  gmeLen     - Length of the game (in 50ths) (-1 =unlimited)
 *********************************************************/
bool clientSimCreate(ClientSim *cs, gameType game, bool hiddenMines, int srtDelay, int32_t gmeLen) {
  (void)srtDelay;  /* Used by screen.c for display */
  (void)gmeLen;    /* Used by screen.c for display */
  
  srand((unsigned int) time(NULL));
  memset(cs, 0, sizeof(*cs));

  /* Initialize GameSim identity and callbacks */
  cs->sim.isServer = false;
  cs->sim.isLocalTransport = true;
  cs->sim.hiddenMines = hiddenMines;
  cs->sim.callbacks.messageAdd = csCallbackMessageAdd;
  cs->sim.callbacks.soundDist = csCallbackSoundDist;
  cs->sim.callbacks.soundDistShoot = csCallbackSoundDistShoot;
  cs->sim.callbacks.soundDistTankHit = csCallbackSoundDistTankHit;
  cs->sim.callbacks.tankKill = csCallbackTankKill;
  cs->sim.callbacks.centerTank = csCallbackCenterTank;
  cs->sim.callbacks.consoleMessage = csCallbackConsoleMessage;
  cs->sim.callbacks.ctx = cs;

  cs->currentBuildSelect = BsTrees;

  minesCreate(&cs->sim.mns, hiddenMines);
  gameTypeSet(&cs->sim.game, game);
  mapCreate(&cs->sim.mp);
  startsCreate(&cs->sim.ss);
  basesCreate(&cs->sim.bs);
  playersCreate(&cs->sim.plyrs, FALSE);
  cs->sim.shs = shellsCreate();
  cs->serverShellCount = 0;
  explosionsCreate(&cs->sim.expl);
  rubbleCreate(&cs->sim.rbl);
  buildingCreate(&cs->sim.blds);
  messageCreate();
  grassCreate(&cs->sim.grs);
  swampCreate(&cs->sim.swp);
  cs->sim.lgmen[0] = lgmCreate(0);
  floodCreate(&cs->sim.ff);
  tkExplosionCreate(&cs->sim.tankExplosions);
  minesExpCreate(&cs->sim.minesExplosions);
  treeGrowCreate();
  pillsCreate(&cs->sim.pb);
  logCreate();
  screenBrainMapCreate(cs);
  
  /* Initialize brain state (now per-instance in the struct).
   * memset above already zeroed the scalar fields. */
  cs->brainBuildInfo = (BuildInfo*) malloc(sizeof(BuildInfo));
  if (cs->brainBuildInfo != NULL) {
    cs->brainBuildInfo->action = 0;
  }

  clientStateCreate(&cs->clientState);
  interpCreate(&cs->interpCtx, 0);

  cs->sim.inStartFind = TRUE;
  cs->running = TRUE;

  /* Network state defaults (memset already zeroed pointers) */
  cs->networkGameType = netNone;
  cs->netStat = netRunning;

  /* Lobby state defaults (memset already zeroed, but be explicit) */
  memset(cs->lobbySlots, 0, sizeof(cs->lobbySlots));
  cs->countdownSeconds = 0;
  cs->mapDownloadComplete = false;
  cs->inLobby = false;

  return true;
}

/*********************************************************
 *NAME:          clientSimDestroy
 *PURPOSE:
 *  Cleans up all simulation state in a ClientSim struct.
 *  Rendering cleanup is handled separately by screenRenderDestroy().
 *
 *ARGUMENTS:
 *  cs - Pointer to the ClientSim to destroy
 *********************************************************/
void clientSimDestroy(ClientSim *cs) {
  logDestroy();
  cs->running = FALSE;
  clientStateDestroy(&cs->clientState);
  tankDestroy(&cs->sim, &cs->sim.tanks[0]);
  cs->sim.tanks[0] = NULL;
  mapDestroy(&cs->sim.mp);
  startsDestroy(&cs->sim.ss);
  basesDestroy(&cs->sim.bs);
  shellsDestroy(&cs->sim.shs);
  cs->serverShellCount = 0;
  explosionsDestroy(&cs->sim.expl);
  rubbleDestroy(&cs->sim.rbl);
  buildingDestroy(&cs->sim.blds);
  messageDestroy();
  grassDestroy(&cs->sim.grs);
  floodDestroy(&cs->sim.ff);
  lgmDestroy(&cs->sim.lgmen[0]);
  swampDestroy(&cs->sim.swp);
  cs->sim.swp = NULL;
  screenBrainMapDestroy(cs);
  tkExplosionDestroy(&cs->sim.tankExplosions);
  minesExpDestroy(&cs->sim.minesExplosions);
  treeGrowDestroy();
  pillsDestroy(&cs->sim.pb);
  playersDestroy(&cs->sim.plyrs);
  
  if (cs->brainBuildInfo != NULL) {
    free(cs->brainBuildInfo);
    cs->brainBuildInfo = NULL;
  }
  
  cs->sim.mp = NULL;
  cs->sim.bs = NULL;
  cs->sim.pb = NULL;
  cs->sim.ss = NULL;
  cs->sim.tanks[0] = NULL;
  cs->sim.shs = NULL;
  cs->sim.lgmen[0] = NULL;
  cs->sim.plyrs = NULL;

  /* Clear lobby chat buffer */
  cs->lobbyChatHistory[0] = '\0';

  /* Clear network callbacks */
  cs->chatSendFunc = NULL;
  cs->nameChangeSendFunc = NULL;
  cs->allianceRequestFunc = NULL;
  cs->allianceAcceptFunc = NULL;
  cs->allianceLeaveFunc = NULL;
  cs->lockToggleSendFunc = NULL;
}

/*********************************************************
 *NAME:          clientSimKeysTick
 *PURPOSE:
 *  Records the input and applies prediction for a keys
 *  tick (tankTurn only). Called BEFORE the server tick.
 *
 *ARGUMENTS:
 *  cs  - Pointer to the ClientSim
 *  pkt - The InputPacket for this tick
 *********************************************************/
void clientSimKeysTick(ClientSim *cs, const InputPacket *pkt) {
  if (!cs->clientState.initialized || !cs->clientState.hasPredictedTank) {
    return;
  }
  clientStateRecordInput(&cs->clientState, pkt);
  clientStatePredictTick(cs, &cs->clientState, pkt, &cs->sim.tanks[0], &cs->sim, TRUE, FALSE);
}

/*********************************************************
 *NAME:          clientSimGameTick
 *PURPOSE:
 *  Records the input and applies prediction for a full
 *  game tick (tankUpdate). Called BEFORE the server tick.
 *
 *ARGUMENTS:
 *  cs      - Pointer to the ClientSim
 *  pkt     - The InputPacket for this tick
 *  isBrain - TRUE if a brain is running
 *********************************************************/
void clientSimGameTick(ClientSim *cs, const InputPacket *pkt, bool isBrain) {
  WORLD preX = 0, preY = 0;
  TURNTYPE preAngle = 0;
  bool canFire = false;

  if (!cs->clientState.initialized || !cs->clientState.hasPredictedTank) {
    return;
  }

  /* Capture pre-movement state for shell prediction. The server fires
   * the shell BEFORE moving the tank inside tankUpdate, so we need the
   * tank's position before clientStatePredictTick moves it.
   * Check reload <= 1 because tankUpdate decrements reload before the
   * fire check — a reload of 1 becomes 0 and allows firing. */
  if ((pkt->actions & INPUT_ACTION_FIRE) && cs->sim.tanks[0] != NULL) {
    tank *tk = &cs->sim.tanks[0];
    if (tankGetReloadTime(tk) <= 1 &&
        tankGetShells(tk) > 0 &&
        tankGetArmour(tk) <= TANK_FULL_ARMOUR) {
      tankGetWorld(tk, &preX, &preY);
      preAngle = tankGetAngle(tk);
      canFire = true;
    }
  }

  clientStateRecordInput(&cs->clientState, pkt);
  clientStatePredictTick(cs, &cs->clientState, pkt, &cs->sim.tanks[0], &cs->sim, FALSE, isBrain);

  /* Create predicted shell using pre-movement position to match server */
  if (canFire) {
    if (!isBrain) {
      frontEndPlaySound(shootSelf);
    }
    clientSimAddPredictedShellAt(cs, preX, preY, preAngle, &cs->sim.tanks[0], pkt->tick);
    /* Update predicted tank state to match what the server will do */
    tankSetReload(&cs->sim.tanks[0], TANK_RELOAD_TIME);
    tankSetShells(&cs->sim.tanks[0], tankGetShells(&cs->sim.tanks[0]) - 1);
  }

  /* Advance existing predicted shells */
  clientSimAdvancePredictedShells(cs);
}

void clientSimSyncFromSnapshot(ClientSim *cs, const SnapshotHeader *hdr,
                               const TankSnapshot *tanks, int tankCount,
                               const ShellSnapshot *shellSnaps, int shellCount,
                               const ExplosionSnapshot *explSnaps, int explosionCount,
                               const BaseSnapshot *baseSnaps, int baseCount,
                               const PillSnapshot *pillSnaps, int pillCount,
                               const GameEvent *events, int eventCount,
                               BYTE playerNum) {
  screenSyncFromSnapshotCS(cs, hdr, tanks, tankCount, shellSnaps, shellCount,
                           explSnaps, explosionCount, baseSnaps, baseCount,
                           pillSnaps, pillCount, events, eventCount, playerNum);
}

void clientSimDisplayTick(ClientSim *cs, bool isBrain) {
  screenSimDisplayTickCS(cs, isBrain);
}

/*********************************************************
 *NAME:          clientSimAddPredictedShell
 *PURPOSE:
 *  Creates a predicted shell matching what the server will
 *  create from the same fire input. Uses the same parameters
 *  as shellsAddItem.
 *********************************************************/
static void clientSimAddPredictedShellAt(ClientSim *cs, WORLD wx, WORLD wy, TURNTYPE angle, tank *tk, uint32_t fireTick) {
  PredictedShell *ps;
  int xAdd, yAdd;
  TURNTYPE sightLen;

  if (cs->predictedShellCount >= MAX_PREDICTED_SHELLS) {
    return;
  }

  sightLen = tankGetGunsightLength(tk);

  /* Replicate shellsAddItem's offset calculation */
  utilCalcDistance(&xAdd, &yAdd, angle, SHELL_SPEED);
  wx = (WORLD)(wx + SHELL_START_ADD * xAdd);
  wy = (WORLD)(wy + SHELL_START_ADD * yAdd);

  ps = &cs->predictedShells[cs->predictedShellCount];
  ps->x = wx;
  ps->y = wy;
  ps->angle = angle;
  ps->length = (uint8_t)(1 + (SHELL_LIFE * (sightLen / 2)) - SHELL_START_ADD);
  ps->owner = gameSimGetTankPlayer(&cs->sim, tk);
  ps->onBoat = tankIsOnBoat(tk);
  ps->fireTick = fireTick;
  ps->active = true;
  cs->predictedShellCount++;
}

/*********************************************************
 *NAME:          clientSimAdvancePredictedShells
 *PURPOSE:
 *  Moves each predicted shell forward by one tick and
 *  removes any that have expired.
 *********************************************************/
void clientSimAdvancePredictedShells(ClientSim *cs) {
  int i;
  for (i = 0; i < cs->predictedShellCount; ) {
    PredictedShell *ps = &cs->predictedShells[i];
    int xAdd, yAdd;
    WORLD newX, newY;
    BYTE mapX, mapY;
    if (ps->length <= SHELL_DEATH) {
      /* Shell expired — remove by swapping with last */
      cs->predictedShells[i] = cs->predictedShells[cs->predictedShellCount - 1];
      cs->predictedShellCount--;
      continue;
    }
    /* Move shell forward */
    utilCalcDistance(&xAdd, &yAdd, ps->angle, SHELL_SPEED);
    newX = (WORLD)(ps->x + xAdd);
    newY = (WORLD)(ps->y + yAdd);

    /* Client-side collision check: remove predicted shell if it hits
     * a pillbox, impassable terrain, or a hostile base. This is purely
     * visual — the server does the authoritative collision. */
    mapX = (BYTE)(newX >> TANK_SHIFT_MAPSIZE);
    mapY = (BYTE)(newY >> TANK_SHIFT_MAPSIZE);
    if (pillsIsPillHit(&cs->sim.pb, mapX, mapY) ||
        (!mapIsPassable(&cs->sim.mp, mapX, mapY, ps->onBoat) &&
         !basesExistPos(&cs->sim.bs, mapX, mapY)) ||
        (basesExistPos(&cs->sim.bs, mapX, mapY) &&
         (ps->onBoat || basesCanHit(&cs->sim, mapX, mapY, ps->owner)))) {
      cs->predictedShells[i] = cs->predictedShells[cs->predictedShellCount - 1];
      cs->predictedShellCount--;
      continue;
    }

    /* Check collision with other players' tanks (interpolated positions) */
    {
      BYTE p;
      bool tankHit = false;
      for (p = 0; p < MAX_TANKS && !tankHit; p++) {
        WORLD tkX, tkY;
        TURNTYPE tkAngle;
        bool tkOnBoat;
        if (p == cs->interpCtx.localPlayer) continue;
        if (!interpIsAlive(&cs->interpCtx, p)) continue;
        if (interpGetPosition(&cs->interpCtx, p, 1.0f, &tkX, &tkY, &tkAngle, &tkOnBoat)) {
          if (abs((int)newX - (int)tkX) < 128 && abs((int)newY - (int)tkY) < 128) {
            tankHit = true;
          }
        }
      }
      if (tankHit) {
        cs->predictedShells[i] = cs->predictedShells[cs->predictedShellCount - 1];
        cs->predictedShellCount--;
        continue;
      }
    }

    ps->x = newX;
    ps->y = newY;
    ps->length--;
    i++;
  }
}

/*********************************************************
 *NAME:          clientSimReconcilePredictedShells
 *PURPOSE:
 *  No-op. Predicted shells are no longer reconciled against
 *  server state — they expire naturally via their length
 *  counter in clientSimAdvancePredictedShells. Server
 *  shells owned by the local player are filtered from
 *  rendering so there is no duplication.
 *********************************************************/
void clientSimReconcilePredictedShells(ClientSim *cs, uint32_t lastProcessedInput) {
  (void)cs;
  (void)lastProcessedInput;
}

/*********************************************************
 *NAME:          clientSimGetBrainState
 *PURPOSE:
 *  Returns pointers to brain state variables within the
 *  given ClientSim instance.
 *********************************************************/
uint32_t *clientSimGetBrainHoldKeys(ClientSim *cs) { return &cs->brainHoldKeys; }
uint32_t *clientSimGetBrainTapKeys(ClientSim *cs) { return &cs->brainTapKeys; }
BuildInfo **clientSimGetBrainBuildInfo(ClientSim *cs) { return &cs->brainBuildInfo; }
PlayerBitMap *clientSimGetBrainsWantAllies(ClientSim *cs) { return &cs->brainsWantAllies; }
PlayerBitMap *clientSimGetBrainsMessageDest(ClientSim *cs) { return &cs->brainsMessageDest; }
char *clientSimGetBrainsMessage(ClientSim *cs) { return cs->brainsMessage; }
unsigned short *clientSimGetBrainsNumObjects(ClientSim *cs) { return &cs->brainsNumObjects; }
ObjectInfo *clientSimGetBrainObjects(ClientSim *cs) { return cs->brainObjects; }
aiType *clientSimGetAllowComputerTanks(ClientSim *cs) { return &cs->allowComputerTanks; }

/* -------------------------------------------------------
 * Network state accessors (per-instance)
 * ------------------------------------------------------- */
netType clientSimGetNetType(ClientSim *cs) { return cs->networkGameType; }
netStatus clientSimGetNetStatus(ClientSim *cs) { return cs->netStat; }
void clientSimSetNetType(ClientSim *cs, netType value) { cs->networkGameType = value; }
void clientSimSetNetStatus(ClientSim *cs, netStatus value) { cs->netStat = value; }

void clientSimSetChatSendFunc(ClientSim *cs, NetChatSendFunc func) { cs->chatSendFunc = func; }
void clientSimSetNameChangeSendFunc(ClientSim *cs, NetNameChangeSendFunc func) { cs->nameChangeSendFunc = func; }
void clientSimSetAllianceRequestFunc(ClientSim *cs, NetAllianceRequestFunc func) { cs->allianceRequestFunc = func; }
void clientSimSetAllianceAcceptFunc(ClientSim *cs, NetAllianceAcceptFunc func) { cs->allianceAcceptFunc = func; }
void clientSimSetAllianceLeaveFunc(ClientSim *cs, NetAllianceLeaveFunc func) { cs->allianceLeaveFunc = func; }
void clientSimSetLockToggleSendFunc(ClientSim *cs, NetLockToggleSendFunc func) { cs->lockToggleSendFunc = func; }

void clientSimAppendLobbyChat(ClientSim *cs, const char *name, const char *message) {
  size_t histLen = strlen(cs->lobbyChatHistory);
  size_t needed = strlen(name) + 2 + strlen(message) + 2; /* "name: message\n" */
  if (histLen + needed < sizeof(cs->lobbyChatHistory)) {
    if (histLen > 0) {
      strcat(cs->lobbyChatHistory, "\n");
    }
    strcat(cs->lobbyChatHistory, name);
    strcat(cs->lobbyChatHistory, ": ");
    strcat(cs->lobbyChatHistory, message);
  }
}

void clientSimMessageSendAllPlayers(ClientSim *cs, BYTE playerNum, char *message) {
  if (cs->chatSendFunc != NULL) {
    cs->chatSendFunc(0xFF, message);
  }
}

void clientSimMessageSendPlayer(ClientSim *cs, BYTE playerNum, BYTE destPlayer, char *message) {
  if (cs->chatSendFunc != NULL) {
    cs->chatSendFunc(destPlayer, message);
  }
}

void clientSimSendChangePlayerName(ClientSim *cs, BYTE playerNum, char *newName) {
  if (cs->nameChangeSendFunc != NULL) {
    cs->nameChangeSendFunc(newName);
  }
}

void clientSimRequestAlliance(ClientSim *cs, BYTE playerNum, BYTE requestTo) {
  if (cs->allianceRequestFunc != NULL) {
    cs->allianceRequestFunc(requestTo);
  }
}

void clientSimAllianceAccept(ClientSim *cs, BYTE playerNum) {
  if (cs->allianceAcceptFunc != NULL) {
    if (playersIsInUse(&cs->sim.plyrs, playerNum) == TRUE) {
      cs->allianceAcceptFunc(playerNum);
    }
  }
}

void clientSimLeaveAlliance(ClientSim *cs, BYTE playerNum) {
  if (cs->allianceLeaveFunc != NULL) {
    cs->allianceLeaveFunc();
  }
}

void clientSimSetAllowNewPlayers(ClientSim *cs, bool allow) {
  if (cs->lockToggleSendFunc != NULL) {
    cs->lockToggleSendFunc(allow);
  }
}


void netGetStats(ClientSim *cs, char *status, int *ping, int *ppsec, int *retrans) {
  if (cs->netStat == netFailed) {
    strcpy(status, langGetText(NET_STATUS_FAILED));
  } else {
    strcpy(status, langGetText(NET_STATUS_OK));
  }
  *ping = 0;
  *ppsec = 0;
  *retrans = 0;
}

void netGetServerAddressStr(ClientSim *cs, char *dest) {
  if (cs->networkGameType == netUdp) {
    strcpy(dest, "Network Game");
  } else {
    strcpy(dest, NET_SINGLE_PLAYER_GAME);
  }
}

void netGetOurAddressStr(ClientSim *cs, char *dest) {
  if (cs->networkGameType == netUdp) {
    dest[0] = '\0';
  } else {
    strcpy(dest, NET_SINGLE_PLAYER_GAME);
  }
}

BYTE netGetDownloadPos(void) {
  return 255; /* complete */
}

void netSecond(void) {
  /* no-op — old ring protocol timing removed */
}

int netGetNetTime(void) {
  return 0;
}

void netSendTrackerUpdate(void) {
  /* no-op — tracker update removed */
}

/* Alliance dialog window — created/destroyed with network lifecycle */
static void *dlgAllianceWnd = NULL;

bool netSetup(ClientSim *cs, netType value, unsigned short myPort, char *targetIp, unsigned short targetPort, char *password, bool usCreate, char *trackerAddr, unsigned short trackerPort, bool useTracker, bool wantRejoin, bool useWinboloNet, char *wbnToken) {
  cs->networkGameType = value;
  cs->netStat = netRunning;
  dlgAllianceWnd = dialogAllianceCreate();

  if (value == netUdp) {
    dnsLookupsCreate(cs);
  }
  return TRUE;
}

void netDestroy(ClientSim *cs) {
  if (dlgAllianceWnd != NULL) {
    dialogAllianceDestroy(dlgAllianceWnd);
    dlgAllianceWnd = NULL;
  }
  if (cs->networkGameType == netUdp) {
    dnsLookupsDestroy();
  }
  if (winbolonetIsRunning()) {
    winbolonetDestroy(FALSE);
  }
  cs->networkGameType = netNone;
  cs->netStat = netRunning;
  cs->chatSendFunc = NULL;
  cs->nameChangeSendFunc = NULL;
  cs->allianceRequestFunc = NULL;
  cs->allianceAcceptFunc = NULL;
  cs->allianceLeaveFunc = NULL;
  cs->lockToggleSendFunc = NULL;
}

/* DNS lookup completion callback — called from dns_lookups.c thread */
void netProcessedDnsLookup(ClientSim *cs, char *ip, char *host) {
  clientMutexWaitFor();
  playerSetLocation(&cs->sim.plyrs, ip, host);
  clientMutexRelease();
}
