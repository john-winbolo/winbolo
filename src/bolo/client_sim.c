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
 *Name:          Client Simulation
 *Filename:      client_sim.c
 *Author:        John Morrison
 *Purpose:
 *  ClientSim lifecycle API and tick functions.
 *  Moved from screen.c as part of the client/server
 *  simulation separation.
 *********************************************************/

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <SDL3/SDL.h>
#include "client_sim.h"
#include "client_command.h"   /* ViewStateKind — the viewport's view kinds */
#include "client_sim_internal.h"
#include "spectator_drain.h"   /* dep-free seam: logviewer host drains capture */
#include "spectator_replay.h"          /* extract a seed's control-snapshot slice */
#include "transport_control_codec.h"   /* decode the snapshot's lobby-settings event */
#include "client_net.h"   /* clientSimNetSendChat — default chatSendFunc body */
#include "client_snapshot.h"
#include "client_state.h"
#include "client_ui_events.h"
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
#include <SDL3/SDL.h>
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
#include "overview_map.h"
#include "util.h"
#include "netpacks.h"
#include "transport.h"
#include "transport_udp.h"
#include "../gui/lang.h"
#include "../gui/dnsLookups.h"
#include "../gui/clientmutex.h"
#include "../gui/dialogAlliance.h"
#include "../winbolonet/winbolonet_core.h"
#include "../common/mp_diag_log.h"
#include "frontend.h"

/* Must match the value used inside shellsAddItem (shells.c redefines
 * SHELL_START_ADD from 6 to 5 locally). */
#undef SHELL_START_ADD
#define SHELL_START_ADD 5

/* Forward declarations */
static void clientSimAddPredictedShellAt(ClientSim *cs, WORLD wx, WORLD wy, TURNTYPE angle, tank *tk, uint32_t fireTick);

/* GameSim callback wrappers for the client side */
static void csCallbackMessageAdd(void *ctx, messageType msgType,
                                 langid topId, langid bodyId,
                                 const MessageArgs *args) {
  ClientSim *cs = (ClientSim *)ctx;
  /* THE newswire gate. Every engine-generated newswire line in the client
   * arrives through this one callback — base and pill captures/steals
   * (bases.c, pillbox.c, client_snapshot.c), builder lost (lgm.c), player
   * quit and name handover (players.c), saved map. When the server has the
   * newswire muted (a scripted wave filing on or off the field), drop them
   * here. Assistant and brain lines still come through, and server text
   * (game.message -> CTRL_SERVER_TEXT, which the scenario's wave banner
   * rides) never uses this callback at all — it calls clientMessageAdd
   * directly from client_sim_control.c, so the banner still shows. */
  if (msgType == newsWireMessage && clientSimNewswireMuted(cs)) return;
  /* Render at receive time using the client's currently-loaded language.
   * langGetTextFmt returns a pointer into a small thread-local ring of
   * buffers, so copy the result before any further langGet* call could
   * recycle the slot. The message log copies the strings character by
   * character in messageAddItem, so once clientMessageAdd returns the
   * stack buffers are safe to drop. Switching language mid-game does
   * not retranslate prior log entries — that's by design. */
  char topBuf[FILENAME_MAX];
  char bodyBuf[FILENAME_MAX];
  const char *topRendered = langGetText(topId);
  const char *bodyRendered = langGetTextFmt(bodyId, args);
  topBuf[0] = '\0';
  bodyBuf[0] = '\0';
  if (topRendered) {
    strncpy(topBuf, topRendered, sizeof(topBuf) - 1);
    topBuf[sizeof(topBuf) - 1] = '\0';
  }
  if (bodyRendered) {
    strncpy(bodyBuf, bodyRendered, sizeof(bodyBuf) - 1);
    bodyBuf[sizeof(bodyBuf) - 1] = '\0';
  }
  clientMessageAdd(&cs->messages, msgType, topBuf, bodyBuf);
}

/* Shared predicate for every newswire emitter: is the server's newswire
 * mute (CTRL_NEWSWIRE_MUTE) currently on for this client? */
bool clientSimNewswireMuted(const ClientSim *cs) {
  return (cs != NULL) && cs->newswireMuted;
}

static void csCallbackSoundDist(void *ctx, sndEffects value, BYTE mx, BYTE my) {
  ClientSim *cs = (ClientSim *)ctx;
  if (cs->sim.isPredicting) return;
  clientSoundDistLocal(&cs->sim, value, mx, my);
}

static void csCallbackCenterTank(void *ctx) {
  ClientSim *cs = (ClientSim *)ctx;
  clientSimTankView(cs);
}

static void csCallbackSoundDistShoot(void *ctx, BYTE mx, BYTE my, BYTE owner) {
  /* no-op — client plays shootSelf via prediction; other players' shoots arrive via EVENT_SOUND_SHOOT */
}

static void csCallbackSoundDistTankHit(void *ctx, BYTE mx, BYTE my, BYTE hitPlayer) {
  /* no-op — client receives tank hit sounds via EVENT_SOUND_TANK_HIT from server */
}

static void csCallbackTankKill(void *ctx, BYTE killer, BYTE killed, BYTE deathCause, BYTE carriedPills) {
  (void)deathCause; (void)carriedPills;
  /* no-op — client receives kills via EVENT_TANK_KILLED from server */
}

static void csCallbackConsoleMessage(void *ctx, char *msg) {
  /* no-op on the client */
}

static void csCallbackMineVisible(void *ctx, BYTE mx, BYTE my, BYTE sourcePlayer) {
  /* no-op — client receives mine visibility via EVENT_MINE_VISIBLE from server */
}

/* Brain state now lives inside the ClientSim struct (see client_sim.h).
 * The static globals were removed in the Phase 0 refactor. */

/* Forward declaration — definition lives further down with the other
 * chatSendFunc helpers. clientSimCreate references this as the default
 * value for cs->chatSendFunc. */
void clientSimDefaultChatSend(ClientSim *cs, BYTE fromPlayer, BYTE destPlayer,
                              const char *message);

/*********************************************************
 *NAME:          clientSimAlloc
 *PURPOSE:
 *  Heap-allocates a zero-initialised ClientSim. Pairs with
 *  clientSimDestroy, which frees the returned pointer.
 *********************************************************/
ClientSim *clientSimAlloc(void) {
  ClientSim *cs = (ClientSim *)calloc(1, sizeof(ClientSim));
  if (cs != NULL) {
    /* SubscriberHandle's canonical "none" sentinel is -1, not the
     * calloc'd 0 (which encodes a real slot/gen). Patch it here so
     * subsequent disconnect / destroy paths can distinguish
     * "never registered" from "registered, handle == 0". */
    cs->autoSubHandle = SUBSCRIBER_HANDLE_INVALID;
  }
  return cs;
}

/*********************************************************
 *NAME:          clientSimCreate
 *PURPOSE:
 *  Initializes a ClientSim struct with all simulation state.
 *  This is the simulation-only initialization; rendering
 *  setup is handled separately by viewportInit().
 *
 *  Game settings (game type, hidden mines, start delay,
 *  length) install via clientSimSet{GameType,HiddenMines,
 *  GmeStartDelay,GmeLength} — the UDP transport applies
 *  them at JOIN_ACCEPT, and the local path inherits them
 *  from the bound ServerSim.
 *
 *ARGUMENTS:
 *  cs - Pointer to the ClientSim to initialize
 *********************************************************/
bool clientSimCreate(ClientSim *cs) {
  /* Preserve transport / subscriber / brain-list / observer /
   * server-endpoint / LAN-only state across the memset. Fresh
   * clientSimAlloc + clientSimCreate callers have zeroed
   * fields anyway, so save/restore is a no-op there.
   *
   * lobbyBrainList: server delivers it once at subscribe time
   * and never re-broadcasts after a round.
   *
   * controlObserverCb/Ctx: external test-harness lifetime,
   * independent of map reloads.
   *
   * serverAddress / serverPort / isLanOnly: connection-lifetime
   * state — set once at join, never re-published. */
  Transport savedTransport = cs->transport;
  bool savedHasTransport   = cs->hasTransport;
  bool savedIsUdpTransport = cs->isUdpTransport;
  bool savedTransportTicksServer = cs->transportTicksServer;
  struct ServerSim *savedBoundServerSim = cs->boundServerSim;
  SubscriberHandle savedAutoSubHandle = cs->autoSubHandle;
  BrainList savedBrainList = cs->lobbyBrainList;
  ControlObserverCb savedObserverCb  = cs->controlObserverCb;
  void             *savedObserverCtx = cs->controlObserverCtx;
  ControlObserverCb savedTransportObserverCb  = cs->transportObserverCb;
  void             *savedTransportObserverCtx = cs->transportObserverCtx;
  struct in_addr savedServerAddress  = cs->serverAddress;
  unsigned short savedServerPort     = cs->serverPort;
  bool           savedIsLanOnly      = cs->isLanOnly;
  memset(cs, 0, sizeof(*cs));
  cs->transport          = savedTransport;
  cs->hasTransport       = savedHasTransport;
  cs->isUdpTransport     = savedIsUdpTransport;
  cs->transportTicksServer = savedTransportTicksServer;
  cs->boundServerSim     = savedBoundServerSim;
  cs->autoSubHandle      = savedAutoSubHandle;
  cs->lobbyBrainList     = savedBrainList;
  cs->controlObserverCb  = savedObserverCb;
  cs->controlObserverCtx = savedObserverCtx;
  cs->transportObserverCb  = savedTransportObserverCb;
  cs->transportObserverCtx = savedTransportObserverCtx;
  cs->serverAddress      = savedServerAddress;
  cs->serverPort         = savedServerPort;
  cs->isLanOnly          = savedIsLanOnly;
  cs->pendingAllianceRequestFrom = 0xFF;
  /* No view state reported yet — the first display tick of the session sends
   * one. The memset above would otherwise read as "tank view already sent". */
  cs->lastSentViewKind = 0xFF;
  cs->lastSentViewTarget = 0xFF;
  /* Default chat-send callback: route outbound chat through this cs's
   * own transport. Bots, SP host humans, and UDP-connected humans all
   * use the same path out of the box. Frontends that want different
   * behavior override via clientSimSetChatSendFunc. The other 5 send
   * callbacks (name change, alliance, vote, etc.) stay NULL —
   * client_net.h's wrappers for those still UDP-gate, so a local-
   * transport client can't use them yet. */
  cs->chatSendFunc = clientSimDefaultChatSend;
  cs->myPlayerNum = 0;
  cs->sim.viewPlayer = 0;
  /* Default to "joins accepted" so the UI renders a checked "Now"
   * box on the very first frame, before the server's first
   * CTRL_LOBBY_SETTINGS broadcast lands and overwrites this. */
  cs->lobbyAllowNewPlayers = true;

  /* Sentinel value for "no batch start assigned" — clients never run the
   * batch placement, but startsGetStart still checks the slot when bots /
   * single-player tankCreate runs, so leave them all unset. */
  for (int i = 0; i < MAX_TANKS; i++) {
    cs->sim.pendingStartIdx[i] = MAX_STARTS;
  }

  /* Initialize GameSim identity and callbacks */
  cs->sim.isServer = false;
  cs->sim.isLocalTransport = true;
  cs->sim.callbacks.messageAdd = csCallbackMessageAdd;
  cs->sim.callbacks.soundDist = csCallbackSoundDist;
  cs->sim.callbacks.soundDistShoot = csCallbackSoundDistShoot;
  cs->sim.callbacks.soundDistTankHit = csCallbackSoundDistTankHit;
  cs->sim.callbacks.tankKill = csCallbackTankKill;
  cs->sim.callbacks.centerTank = csCallbackCenterTank;
  cs->sim.callbacks.consoleMessage = csCallbackConsoleMessage;
  cs->sim.callbacks.mineVisible = csCallbackMineVisible;
  cs->sim.callbacks.explosion = NULL;
  cs->sim.callbacks.tkExplosion = NULL;
  cs->sim.callbacks.recordDamage = NULL;
  cs->sim.callbacks.chooseStart = NULL;
  cs->sim.callbacks.recordPlayerAction = NULL;
  cs->sim.callbacks.recordPillPickup = NULL;
  cs->sim.callbacks.ctx = cs;

  cs->currentBuildSelect = BsTrees;
  cs->labelOwnTank = TRUE;
  cs->labelMessage = lblShort;
  cs->labelTankLabel = lblShort;

  minesCreate(&cs->sim.mns, false);
  gameTypeSet(&cs->sim.game, 0);
  mapCreate(&cs->sim.mp);
  startsCreate(&cs->sim.ss);
  basesCreate(&cs->sim.bs);
  playersCreate(&cs->sim.plyrs, FALSE);
  cs->sim.shs = shellsCreate();
  cs->serverShellCount = 0;
  cs->projectedShellCount = 0;
  cs->projectionPingMs = 0;
  memset(cs->displayPing, 0, sizeof(cs->displayPing));
  explosionsCreate(&cs->sim.expl);
  rubbleCreate(&cs->sim.rbl);
  buildingCreate(&cs->sim.blds);
  messageCreate(&cs->messages);
  scrollCreate(&cs->scroll);
  grassCreate(&cs->sim.grs);
  swampCreate(&cs->sim.swp);
  MY_LGM(cs) = lgmCreate(cs->myPlayerNum);
  floodCreate(&cs->sim.ff);
  tkExplosionCreate(&cs->sim.tankExplosions);
  minesExpCreate(&cs->sim.minesExplosions);
  treeGrowCreate(&cs->sim);
  /* Init base timers (was in basesCreate, now lives in GameSim) */
  {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
      cs->sim.baseTimer[i] = 30000;
    }
    cs->sim.baseTimer[cs->myPlayerNum] = BASE_TICKS_BETWEEN_REFUEL;
  }
  pillsCreate(&cs->sim.pb);
  screenBrainMapCreate(cs);
  /* Not covered by the memset above: an unseen square is 0xFF, not 0. */
  overviewMapReset(&cs->overview);
  
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

  /* Visibility rules, until the server's own arrive with the lobby settings.
   * The three a server starts with (server_sim.c), so a display tick before
   * they land draws what the server is actually sending rather than a block
   * round every allied base. The memset above would leave every category on
   * viewPolicyAlways. */
  cs->viewPolicy[viewCategoryPill] = viewPolicyAlways;
  cs->viewPolicy[viewCategoryBase] = viewPolicyOff;
  cs->viewPolicy[viewCategoryAlly] = viewPolicyAlways;

  /* Lobby state defaults (memset already zeroed, but be explicit) */
  memset(cs->lobbySlots, 0, sizeof(cs->lobbySlots));
  cs->voiceTalkingMap = 0;
  cs->countdownSeconds = 0;
  cs->mapDownloadComplete = false;
  cs->inLobby = false;
  cs->lobbyAvailable = false;

  /* 0 is a valid brain-catalogue index, so the bulk memset above can't
   * be the "no brain assigned" marker — use 0xFF, matching the sentinel
   * the server uses for botBrainIdx[]. */
  memset(cs->lobbyBotBrainIdx, 0xFF, sizeof(cs->lobbyBotBrainIdx));

  return true;
}

void clientSimSetPlayerNum(ClientSim *cs, BYTE playerNum) {
    BYTE prevPlayerNum = cs->myPlayerNum;

    cs->myPlayerNum = playerNum;
    cs->sim.viewPlayer = playerNum;
    /* The client owns exactly one tank and one lgm — its own — and both sit
     * at the slot index, so changing slot has to carry them across. Take the
     * source from the slot they are actually in rather than from 0: only the
     * first assignment finds them at 0, where clientSimCreate pre-allocated
     * them. A second assignment (a mid-lobby PACKET_LOBBY_MAP_CHANGE sends
     * the joiner through JOIN_REQUEST -> JOIN_ACCEPT again, and the server
     * may hand back a different slot) then copied the already-NULL slot 0
     * over the live pointers and dropped the only reference to both. */
    if (playerNum != prevPlayerNum) {
        cs->sim.tanks[playerNum] = cs->sim.tanks[prevPlayerNum];
        cs->sim.tanks[prevPlayerNum] = NULL;
        cs->sim.lgmen[playerNum] = cs->sim.lgmen[prevPlayerNum];
        cs->sim.lgmen[prevPlayerNum] = NULL;
        lgmSetPlayerNum(&cs->sim.lgmen[playerNum], playerNum);
    }
    if (playerNum != 0) {
        cs->sim.baseTimer[playerNum] = BASE_TICKS_BETWEEN_REFUEL;
    }
}

void clientSimSetupSelf(ClientSim *cs, BYTE playerNum,
                        const char *playerName,
                        uint8_t clientType, uint8_t clientFlags) {
    if (MY_TANK(cs) != NULL) {
        tankDestroy(&cs->sim, &MY_TANK(cs));
        MY_TANK(cs) = NULL;
    }
    tankCreate(&cs->sim, &MY_TANK(cs));
    /* LGM lifecycle must mirror the tank's. setupSelf runs on first-time
     * slot assignment and again on re-assignment, and both times the slot
     * holds the objects clientSimSetPlayerNum carried over. Destroy before
     * recreating so the re-assignment path frees the pair it was handed
     * instead of leaking it, and so MY_LGM is valid by the time the first
     * per-frame lgmGetStatus runs after game start. */
    if (MY_LGM(cs) != NULL) {
        lgmDestroy(&MY_LGM(cs));
        MY_LGM(cs) = NULL;
    }
    MY_LGM(cs) = lgmCreate(playerNum);
    SDL_assert(MY_TANK(cs) != NULL);
    SDL_assert(MY_LGM(cs)  != NULL);
    playersSetSelf(cs, &cs->sim, &cs->sim.plyrs,
                   (playerNumbers)playerNum,
                   (char *)playerName, TRUE);
    playersSetClientType (&cs->sim.plyrs, playerNum, clientType);
    playersSetClientFlags(&cs->sim.plyrs, playerNum, clientFlags);
}

void clientSimOnAssignedSlot(ClientSim *cs, BYTE playerNum,
                             const char *playerName,
                             uint8_t clientType, uint8_t clientFlags) {
    /* Single entry point for "we just learned our slot."  Both the SP
     * local-transport connect path and the UDP JOIN_ACCEPT handler call
     * here so they cannot drift.  Pre-consolidation, the SP path called
     * setPlayerNum + setupSelf + frontEndApplyLocalTankPrefs by hand,
     * the UDP path called only setPlayerNum — and MY_TANK stayed NULL
     * for the lifetime of every UDP session, crashing every per-frame
     * code path the moment inLobby flipped to false.  Funnel both
     * paths through one function so future maintainers cannot drop a
     * step on one side without breaking it on the other. */
    clientSimSetPlayerNum(cs, playerNum);
    clientSimSetupSelf(cs, playerNum, playerName, clientType, clientFlags);
    frontEndApplyLocalTankPrefs(cs);
}

/*********************************************************
 *NAME:          clientSimDestroy
 *PURPOSE:
 *  Cleans up all simulation state in a ClientSim struct and
 *  frees the cs pointer (pairs with clientSimAlloc). Also
 *  frees the viewport buffers (view/mineView) so the caller
 *  doesn't need a separate viewportDestroy call.
 *  Accepts NULL as a no-op.
 *
 *ARGUMENTS:
 *  cs - Pointer to the ClientSim to destroy and free
 *********************************************************/
/* Tears down all owned simulation state on cs but does NOT free cs
 * itself or touch the transport binding. Called from
 * clientSimDestroy, which then frees the pointer. */
static void clientSimDestroyContents(ClientSim *cs) {
  viewportDestroy(&cs->viewport);
  cs->running = FALSE;
  clientStateDestroy(&cs->clientState);
  tankDestroy(&cs->sim, &MY_TANK(cs));
  MY_TANK(cs) = NULL;
  mapDestroy(&cs->sim.mp);
  startsDestroy(&cs->sim.ss);
  basesDestroy(&cs->sim.bs);
  shellsDestroy(&cs->sim.shs);
  cs->serverShellCount = 0;
  cs->projectedShellCount = 0;
  explosionsDestroy(&cs->sim.expl);
  rubbleDestroy(&cs->sim.rbl);
  buildingDestroy(&cs->sim.blds);
  messageDestroy(&cs->messages);
  grassDestroy(&cs->sim.grs);
  floodDestroy(&cs->sim.ff);
  lgmDestroy(&MY_LGM(cs));
  swampDestroy(&cs->sim.swp);
  cs->sim.swp = NULL;
  screenBrainMapDestroy(cs);
  tkExplosionDestroy(&cs->sim.tankExplosions);
  /* clientSimCreate allocates the mine field and only clientSimResetWorld
   * ever paired it with a destroy, so every ClientSim that reached teardown
   * left it behind. Dispose() does not clear the pointer, so null it here as
   * the other teardowns do. */
  minesDestroy(&cs->sim.mns);
  cs->sim.mns = NULL;
  minesExpDestroy(&cs->sim.minesExplosions);
  treeGrowDestroy(&cs->sim);
  pillsDestroy(&cs->sim.pb);
  playersDestroy(&cs->sim.plyrs);

  if (cs->brainBuildInfo != NULL) {
    free(cs->brainBuildInfo);
    cs->brainBuildInfo = NULL;
  }

  /* Free any captured-but-undrained spectator seed/records. */
  clientSimSpectatorFeedClear(cs);

  cs->sim.mp = NULL;
  cs->sim.bs = NULL;
  cs->sim.pb = NULL;
  cs->sim.ss = NULL;
  MY_TANK(cs) = NULL;
  cs->sim.shs = NULL;
  MY_LGM(cs) = NULL;
  cs->sim.plyrs = NULL;

  /* Clear lobby chat buffer */
  cs->lobbyChatHistory[0] = '\0';
  cs->lobbyTeamChatHistory[0] = '\0';
  cs->lobbyHostSlotKnown = false;

  /* Clear network callbacks */
  cs->chatSendFunc = NULL;
  cs->nameChangeSendFunc = NULL;
  cs->allianceRequestFunc = NULL;
  cs->allianceAcceptFunc = NULL;
  cs->allianceLeaveFunc = NULL;
  cs->lockToggleSendFunc = NULL;
}

void clientSimDestroy(ClientSim *cs) {
  if (cs == NULL) return;
  /* Unregister the auto-subscriber BEFORE the transport teardown so
   * boundServerSim is still valid. Callers that skip clientSimDisconnect
   * (gym / headless / wasm) rely on destroy to clean this up too. */
  if (cs->boundServerSim != NULL &&
      cs->autoSubHandle != SUBSCRIBER_HANDLE_INVALID) {
    serverSimUnregisterSubscriber(cs->boundServerSim, cs->autoSubHandle);
    cs->autoSubHandle = SUBSCRIBER_HANDLE_INVALID;
  }
  if (cs->hasTransport) {
    if (cs->isUdpTransport) {
      transportUdpClientDestroy(&cs->transport);
    } else {
      transportLocalDestroy(&cs->transport);
    }
    cs->hasTransport = false;
  }
  cs->boundServerSim = NULL;
  clientSimDestroyContents(cs);
  free(cs);
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
  clientStatePredictTick(cs, &cs->clientState, pkt, &MY_TANK(cs), &cs->sim, TRUE, FALSE);
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
  if ((pkt->actions & INPUT_ACTION_FIRE) && MY_TANK(cs) != NULL) {
    tank *tk = &MY_TANK(cs);
    if (tankGetReloadTime(tk) <= 1 &&
        tankGetShells(tk) > 0 &&
        !tankIsDestroyed(tk)) {
      tankGetWorld(tk, &preX, &preY);
      preAngle = tankGetAngle(tk);
      canFire = true;
    }
  }

  clientStateRecordInput(&cs->clientState, pkt);
  /* The tick being simulated, for base-death prediction: the movement below
   * resolves a predicted-dead base against it, and the shell advance stamps
   * it onto any base its hit is about to kill. Cleared once the tick is done
   * so nothing outside prediction sees a stale value. */
  cs->sim.replayTick = pkt->tick;
  clientStatePredictTick(cs, &cs->clientState, pkt, &MY_TANK(cs), &cs->sim, FALSE, isBrain);

  /* Create predicted shell using pre-movement position to match server */
  if (canFire) {
    if (!isBrain) {
      frontEndPlaySound(cs, shootSelf);
    }
    clientSimAddPredictedShellAt(cs, preX, preY, preAngle, &MY_TANK(cs), pkt->tick);
    /* Update predicted tank state to match what the server will do */
    tankSetReload(&MY_TANK(cs), TANK_RELOAD_TIME);
    tankSetShells(&MY_TANK(cs), tankGetShells(&MY_TANK(cs)) - 1);
  }

  /* Advance existing predicted shells */
  clientSimAdvancePredictedShells(cs);
  cs->sim.replayTick = 0;

  /* Advance render-only projected shells (other players', human clients).
   * Empty for bots, so this is a no-op there. */
  clientSimAdvanceProjectedShells(cs);

  /* Roll the reconcile-stats window once per second. pkt->tick is the
   * 100Hz sub-tick counter; >>1 yields the game-tick index, which
   * advances 50/s, so 50 game ticks is one second. The completed window
   * is copied into the *Last fields (read by the Net Info dialog) and
   * logged only when a transport is active. */
  {
    uint32_t gameTick = pkt->tick >> 1;
    if (gameTick - cs->reconWindowStartTick >= 50) {
      cs->reconCountLastSec = cs->reconCountThisWindow;
      cs->reconErrMaxPxLast = cs->reconErrMaxPx;
      cs->reconErrAvgPxLast = (cs->reconCountThisWindow > 0)
                                  ? (cs->reconErrSumPx / cs->reconCountThisWindow)
                                  : 0.0f;
      if (clientSimHasTransport(cs)) {
        float errOffPx =
            sqrtf(cs->errX * cs->errX + cs->errY * cs->errY) / 16.0f;
        mpDiagLog("[cli] netstat recon=%u/s errAvg=%.1fpx errMax=%.1fpx "
                  "renderOff=%.1fpx",
                  cs->reconCountLastSec, cs->reconErrAvgPxLast,
                  cs->reconErrMaxPxLast, errOffPx);
      }
      cs->reconCountThisWindow = 0;
      cs->reconErrSumPx = 0.0f;
      cs->reconErrMaxPx = 0.0f;
      cs->reconWindowStartTick = gameTick;
    }
  }
}

void clientSimSyncFromSnapshot(ClientSim *cs, const SnapshotHeader *hdr,
                               const TankSnapshot *tanks, int tankCount,
                               const ShellSnapshot *shellSnaps, int shellCount,
                               const TkExplosionSnapshot *tkExplSnaps, int tkExplosionCount,
                               const BaseSnapshot *baseSnaps, int baseCount,
                               const PillSnapshot *pillSnaps, int pillCount,
                               const GameEvent *events, int eventCount,
                               BYTE playerNum) {
  clientApplySnapshot(cs, hdr, tanks, tankCount, shellSnaps, shellCount,
                      tkExplSnaps, tkExplosionCount,
                      baseSnaps, baseCount,
                      pillSnaps, pillCount, events, eventCount, playerNum);
}

void clientSimIncomingMessage(ClientSim *cs, BYTE playerNum, char *messageStr) {
  char topLine[FILENAME_MAX];

  topLine[0] = '\0';
  playersMakeMessageName(cs, &clientSimGetGameSim(cs)->plyrs, clientSimGetMyPlayerNum(cs), playerNum, topLine);

  if (clientSimIsInLobby(cs) && playerNum < 16) {
    clientSimAppendLobbyChat(cs, clientSimGetLobbySlot(cs, playerNum)->playerName, messageStr);
  } else {
    clientMessageAdd(clientSimGetMessages(cs), (messageType) (playerNum + PLAYER_MESSAGE_OFFSET), topLine, messageStr);
  }
}

/* Wrap a bradian quantity into [-128, 128) so a correction across the
 * 0/256 boundary reads as the small rotation it is, not ~256. */
static float clientErrWrapBradians(float a) {
  while (a >= 128.0f) a -= 256.0f;
  while (a < -128.0f) a += 256.0f;
  return a;
}

bool clientErrSmoothAccumulate(float *errX, float *errY, float *errAngle,
                               float dX, float dY, float dAngle,
                               float posClamp) {
  float nx = *errX + dX;
  float ny = *errY + dY;
  float na = clientErrWrapBradians(*errAngle + dAngle);

  if (fabsf(nx) > posClamp ||
      fabsf(ny) > posClamp ||
      fabsf(na) > CLIENT_ERR_ANGLE_CLAMP) {
    *errX = 0.0f;
    *errY = 0.0f;
    *errAngle = 0.0f;
    return true;
  }
  *errX = nx;
  *errY = ny;
  *errAngle = na;
  return false;
}

void clientErrSmoothDecay(float *errX, float *errY, float *errAngle, float dtMs) {
  float f = expf(-dtMs / CLIENT_ERR_DECAY_TAU_MS);
  *errX *= f;
  *errY *= f;
  *errAngle *= f;
}

void clientSimGetRenderedTankPos(ClientSim *cs, WORLD *x, WORLD *y, float *angle) {
  WORLD wx, wy;
  TURNTYPE a;

  tankGetWorld(&MY_TANK(cs), &wx, &wy);
  a = tankGetAngle(&MY_TANK(cs));

  if (x != NULL) {
    float fx = (float)wx + cs->errX;
    if (fx < 0.0f) fx = 0.0f;
    if (fx > (float)WORLD_MAX) fx = (float)WORLD_MAX;
    *x = (WORLD)(fx + 0.5f);
  }
  if (y != NULL) {
    float fy = (float)wy + cs->errY;
    if (fy < 0.0f) fy = 0.0f;
    if (fy > (float)WORLD_MAX) fy = (float)WORLD_MAX;
    *y = (WORLD)(fy + 0.5f);
  }
  if (angle != NULL) {
    float fa = a + cs->errAngle;
    while (fa < 0.0f) fa += 256.0f;
    while (fa >= 256.0f) fa -= 256.0f;
    *angle = fa;
  }
}

/* Chebyshev distance in map squares — the shape of the proximity test, the
 * same one the server stamps its own clocks on. */
static bool viewDecayNearSquare(int aMX, int aMY, int bMX, int bMY) {
  int dx; /* Squares apart across */
  int dy; /* Squares apart down */

  dx = aMX - bMX;
  dy = aMY - bMY;
  if (dx < 0) {
    dx = -dx;
  }
  if (dy < 0) {
    dy = -dy;
  }
  return dx <= VIEW_DECAY_NEAR_TILES && dy <= VIEW_DECAY_NEAR_TILES;
}

void clientSimResetViewDecay(ClientSim *cs) {
  if (cs == NULL) {
    return;
  }
  cs->viewDecayTick = 0;
  memset(cs->pillNearTick, 0, sizeof(cs->pillNearTick));
  memset(cs->baseNearTick, 0, sizeof(cs->baseNearTick));
  memset(cs->allyNearTick, 0, sizeof(cs->allyNearTick));
  /* Where the other tanks were last round says nothing about this one, and
   * the timer that reads those stamps runs on the same clock. */
  memset(cs->allyLastMapX, 0, sizeof(cs->allyLastMapX));
  memset(cs->allyLastMapY, 0, sizeof(cs->allyLastMapY));
  memset(cs->allySeenTick, 0, sizeof(cs->allySeenTick));
  cs->allyViewStubTarget = 0;
  cs->allyViewStubTick = 0;
}

void clientSimViewDecayTick(ClientSim *cs) {
  GameSim *gs;      /* The client's own world */
  uint32_t stamp;   /* What a fresh clock reads */
  bool pillDecay;   /* Is this category on viewPolicyDecay */
  bool baseDecay;   /* Is this category on viewPolicyDecay */
  bool allyDecay;   /* Is this category on viewPolicyDecay */
  BYTE mx;          /* Where our tank is */
  BYTE my;          /* Where our tank is */
  BYTE num;         /* Items of the kind being walked */
  BYTE i;           /* Looping variable */

  if (cs == NULL) {
    return;
  }

  /* The clock runs whatever the player is doing, the way the server's tick
   * does: a window has to be able to run out while its owner is dead or
   * standing still, or dying would freeze what they can see. Advanced before
   * it is read, so a stamp is never the 0 that means never. */
  cs->viewDecayTick++;

  pillDecay = (cs->viewPolicy[viewCategoryPill] == viewPolicyDecay);
  baseDecay = (cs->viewPolicy[viewCategoryBase] == viewPolicyDecay);
  allyDecay = (cs->viewPolicy[viewCategoryAlly] == viewPolicyDecay);
  if (pillDecay == FALSE && baseDecay == FALSE && allyDecay == FALSE) {
    return;
  }

  /* A tank waiting to respawn has no position to be near anything from — it
   * reads as the map origin — so nothing is stamped while it is dead, which
   * is what the server does with the same players. */
  mx = 0;
  my = 0;
  if (clientSimGetMyTankMapPos(cs, &mx, &my) != TRUE) {
    return;
  }

  gs = &cs->sim;
  stamp = cs->viewDecayTick;

  if (pillDecay == TRUE && gs->pb != NULL) {
    num = pillsGetNumPills(&gs->pb);
    for (i = 0; i < num; i++) {
      if (viewDecayNearSquare(mx, my, gs->pb->item[i].x, gs->pb->item[i].y) ==
          TRUE) {
        cs->pillNearTick[i] = stamp;
      }
    }
  }
  if (baseDecay == TRUE && gs->bs != NULL) {
    num = basesGetNumBases(&gs->bs);
    for (i = 0; i < num; i++) {
      if (viewDecayNearSquare(mx, my, gs->bs->item[i].x, gs->bs->item[i].y) ==
          TRUE) {
        cs->baseNearTick[i] = stamp;
      }
    }
  }
  if (allyDecay == TRUE) {
    /* Everybody else's last known square, allied or not — the same square the
     * ally view reads, and the only one the client has for a remote tank. */
    for (i = 0; i < MAX_TANKS; i++) {
      if (i == cs->myPlayerNum) {
        continue;
      }
      if (playersIsInUse(&gs->plyrs, i) != TRUE) {
        continue;
      }
      /* A slot the server is not sending arrives as a hidden stub, which
       * zeroes its players entry. (0,0) is not a square a live tank can be
       * on, so it reads as "no position", not as the map origin, and a tank
       * of ours parked near that corner would otherwise stamp every stubbed
       * slot as though it were standing beside us. Skipped rather than
       * stamped from allyLastMapX/Y: VIEW_DECAY_NEAR_TILES is well inside our
       * own tank rect, so an ally that close is never stubbed in the first
       * place, while an ally that died beside us has been moved to its
       * restart square and the server has stopped stamping the old one. */
      if (gs->plyrs->item[i].mapX == 0 && gs->plyrs->item[i].mapY == 0) {
        continue;
      }
      if (viewDecayNearSquare(mx, my, gs->plyrs->item[i].mapX,
                              gs->plyrs->item[i].mapY) == TRUE) {
        cs->allyNearTick[i] = stamp;
      }
    }
  }
}

/* How many clocks a category keeps, so a view target that has gone out of
 * range is never used to index one. */
static BYTE viewDecayCategoryCount(ViewCategory cat) {
  switch (cat) {
  case viewCategoryPill: return MAX_PILLS;
  case viewCategoryBase: return MAX_BASES;
  default:               return MAX_TANKS;
  }
}

bool clientSimViewDecayExpired(const ClientSim *cs) {
  OverviewViewInputs in;  /* The rules and clocks the window test reads */
  ViewCategory cat;       /* Which category the camera is parked on */
  const uint32_t *clocks; /* That category's clocks */
  BYTE target;            /* The item being watched */

  if (cs == NULL) {
    return FALSE;
  }
  switch (cs->viewport.viewKind) {
  case VIEW_KIND_PILL:
    cat = viewCategoryPill;
    clocks = cs->pillNearTick;
    break;
  case VIEW_KIND_BASE:
    cat = viewCategoryBase;
    clocks = cs->baseNearTick;
    break;
  case VIEW_KIND_ALLY:
    cat = viewCategoryAlly;
    clocks = cs->allyNearTick;
    break;
  default:
    return FALSE; /* the tank view, and a kind this client does not know */
  }

  if (cs->viewPolicy[cat] != viewPolicyDecay) {
    return FALSE;
  }
  target = cs->viewport.viewTarget;
  if (target >= viewDecayCategoryCount(cat)) {
    /* Nothing to read a clock from. The item-view upkeep is what drops a
     * target this far gone; claiming it has expired here would only say the
     * same thing in a worse place. */
    return FALSE;
  }

  clientSimFillOverviewViewInputs(cs, &in);
  return overviewViewDecayLive(&in, cat, clocks[target], NULL) == FALSE;
}

/* That category's clocks, or NULL for a category this client does not know. */
static const uint32_t *clientSimViewDecayClocks(const ClientSim *cs,
                                                ViewCategory cat) {
  switch (cat) {
  case viewCategoryPill: return cs->pillNearTick;
  case viewCategoryBase: return cs->baseNearTick;
  case viewCategoryAlly: return cs->allyNearTick;
  default:               return NULL;
  }
}

PlayerBitMap clientSimViewEligibleMask(const ClientSim *cs, ViewCategory cat) {
  OverviewViewInputs in;  /* The rules and clocks the window test reads */
  const uint32_t *clocks; /* That category's clocks */
  PlayerBitMap mask;      /* Bits to return */
  BYTE count;             /* Items the category keeps a clock for */
  BYTE i;                 /* Looping variable */

  count = viewDecayCategoryCount(cat);
  mask = (PlayerBitMap)((((PlayerBitMap)1) << count) - 1);
  clocks = (cs == NULL) ? NULL : clientSimViewDecayClocks(cs, cat);
  if (cs == NULL || clocks == NULL || cs->viewPolicy[cat] != viewPolicyDecay) {
    /* Every item, which is what leaves the other three policies cycling
     * exactly as they did before there were any policies at all. */
    return mask;
  }

  clientSimFillOverviewViewInputs(cs, &in);
  for (i = 0; i < count; i++) {
    if (overviewViewDecayLive(&in, cat, clocks[i], NULL) == FALSE) {
      mask &= ~((PlayerBitMap)1 << i);
    }
  }
  return mask;
}

void clientSimFillViewCycleInputs(const ClientSim *cs, ViewCycleInputs *in) {
  int cat; /* Looping variable */

  if (in == NULL) {
    return;
  }
  viewCycleInputsDefaults(in);
  if (cs == NULL) {
    return;
  }

  in->allyViewable = clientSimAllyViewMask(cs);
  for (cat = 0; cat < VIEW_CATEGORY_COUNT; cat++) {
    in->eligible[cat] = clientSimViewEligibleMask(cs, (ViewCategory)cat);
  }
  in->allyLastMapX = cs->allyLastMapX;
  in->allyLastMapY = cs->allyLastMapY;
}

bool clientSimAllyViewStubExpired(ClientSim *cs) {
  BYTE target;     /* The ally being watched */
  uint32_t since;  /* Tick the current stale run started */

  if (cs == NULL) {
    return FALSE;
  }
  if (cs->viewport.viewKind != VIEW_KIND_ALLY ||
      cs->viewport.viewTarget >= MAX_TANKS) {
    cs->allyViewStubTick = 0;
    return FALSE;
  }

  target = cs->viewport.viewTarget;
  if (cs->allyViewStubTick == 0 || cs->allyViewStubTarget != target) {
    /* The view has just been entered, or moved to another ally: the grace
     * starts over, because under viewPolicyKey the server has only now been
     * told to send this one. */
    cs->allyViewStubTarget = target;
    cs->allyViewStubTick = cs->viewDecayTick;
  }

  /* The later of "we started watching" and "we last saw it" — a real update
   * for the ally resets the grace wherever in the run it lands. */
  since = cs->allyViewStubTick;
  if (cs->allySeenTick[target] > since) {
    since = cs->allySeenTick[target];
  }
  return (cs->viewDecayTick - since) > CLIENT_VIEW_ALLY_STUB_GRACE_TICKS;
}

bool clientSimAllyViewAwaitingFirstData(const ClientSim *cs) {
  if (cs == NULL) {
    return FALSE;
  }
  if (cs->viewport.viewKind != VIEW_KIND_ALLY ||
      cs->viewport.viewTarget >= MAX_TANKS) {
    return FALSE;
  }
  /* allySeenTick is stamped by every real tank record and cleared by
   * clientSimResetViewDecay, so a zero stamp means nothing has arrived for
   * that slot this round. */
  return cs->allySeenTick[cs->viewport.viewTarget] == 0;
}

void clientSimFillOverviewViewInputs(const ClientSim *cs,
                                     OverviewViewInputs *in) {
  int cat; /* Looping variable */

  if (in == NULL) {
    return;
  }
  overviewViewInputsDefaults(in);
  if (cs == NULL) {
    return;
  }

  for (cat = 0; cat < VIEW_CATEGORY_COUNT; cat++) {
    in->policy[cat] = cs->viewPolicy[cat];
    in->decaySecs[cat] = cs->viewDecaySecs[cat];
  }
  in->pillNearTick = cs->pillNearTick;
  in->baseNearTick = cs->baseNearTick;
  in->allyNearTick = cs->allyNearTick;
  in->nowTick = cs->viewDecayTick;
  in->ticksPerSec = CLIENT_VIEW_DECAY_TICKS_SEC;
  in->allyViewable = clientSimAllyViewMask(cs);
  in->viewKind = cs->viewport.viewKind;
  in->viewTarget = cs->viewport.viewTarget;
}

/* Feeds the overview its per-tick view of the world. Reads the local tank's
 * map square, or reports that there isn't one. The two go together: a tank
 * waiting to respawn is still in its slot, but its position is deliberately
 * not read, because a dead tank reads as the map origin and stamping a block
 * there would reveal map the player has never reached. The overview is told
 * how far through its death wait it is rather than simply that it is gone: it
 * holds the block on the square it remembers so the player watches the
 * explosion where it happened. */
static void overviewMapTick(ClientSim *cs) {
  BYTE mx = 0, my = 0;
  bool haveTank = clientSimIsMyTankAlive(cs) &&
                  clientSimGetMyTankMapPos(cs, &mx, &my);
  bool inSlot = !haveTank && MY_TANK(cs) != NULL;
  int deathWait = inSlot ? tankGetDeathWait(&MY_TANK(cs)) : 0;
  OverviewViewInputs in;

  /* Armour goes over full the tick the tank takes the hit; the wait is written
   * by the update after it. Reporting a full wait across that gap holds the
   * block, where a wait of 0 would close it and reopen it a tick later. */
  if (inSlot == TRUE && deathWait <= 0) {
    deathWait = TANK_DEATH_WAIT;
  }

  /* A map install armed this: stamp the whole map dimmed before the live
   * regions brighten over it, now that myPlayerNum is settled. */
  if (cs->overviewSeedPending == TRUE) {
    overviewMapSeedAll(&cs->overview, &cs->sim, cs->myPlayerNum);
    cs->overviewSeedPending = FALSE;
  }
  clientSimFillOverviewViewInputs(cs, &in);
  overviewMapUpdate(&cs->overview, &cs->sim, cs->myPlayerNum, &in, haveTank,
                    deathWait, mx, my);
}

void clientSimDisplayTick(ClientSim *cs, bool isBrain) {
  if (cs == NULL) {
    return;
  }
  /* Ahead of the overview, which reads the clocks this advances to decide
   * which blocks it may draw and how bright. */
  clientSimViewDecayTick(cs);
  /* The overview memory is the one consumer that needs the no-tank tick:
   * that is when a player who has left has their regions given a last stamp
   * and a spectating player keeps seeing what they saw. */
  overviewMapTick(cs);
  /* Master gate for the per-frame game-render pipeline.  Both
   * clientUiOnTick and basesTickMessageQueue assume the local tank
   * exists — clientUiOnTick reads MY_TANK at ~7 sites for scroll,
   * status bars, kills/deaths, gunsight, etc.  In the gap between
   * CTRL_GAME_PHASE_RUNNING flipping inLobby=false on the client and
   * the first post-running snapshot's clientSimSyncFromSnapshot creating
   * the tank, those derefs crash (stack: clientUiOnTick → tankGetSpeed
   * → NULL+0x7).  Skip the per-frame render until the tank shows up;
   * the lobby/menu rendering paths run elsewhere, gated by inLobby/
   * netStatus, so the user just sees the previous frame for a tick
   * or two rather than a NULL dereference. */
  if (cs->sim.tanks[cs->myPlayerNum] == NULL) {
    return;
  }
  /* Decay the render-only error offset one display-tick step. Display
   * ticks run at 50/s whether or not input is held, so a held-still tank
   * still heals; the fixed 20ms step keeps the decay deterministic and
   * frame-rate independent. */
  clientErrSmoothDecay(&cs->errX, &cs->errY, &cs->errAngle, 20.0f);
  clientUiOnTick(cs, isBrain);
  basesTickMessageQueue(&cs->sim, cs);
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
  ps->fx = (float)(int)wx;
  ps->fy = (float)(int)wy;
  {
    int32_t xStepHP, yStepHP;
    utilCalcDistanceHP(&xStepHP, &yStepHP, angle, SHELL_SPEED);
    ps->vx = (float)xStepHP / 256.0f;
    ps->vy = (float)yStepHP / 256.0f;
  }
  ps->angle = angle;
  ps->length = (uint8_t)(1 + (SHELL_LIFE * (sightLen / 2)) - SHELL_START_ADD);
  ps->owner = gameSimGetTankPlayer(&cs->sim, tk);
  ps->onBoat = tankIsOnBoat(tk);
  ps->fireTick = fireTick;
  ps->active = true;
  cs->predictedShellCount++;
}

/*********************************************************
 *NAME:          clientShellVisualBlocked
 *PURPOSE:
 *  Shared visual collision short-circuit for client shells.
 *  Returns true if a shell at (newX,newY) should be culled
 *  because it hit a pillbox, impassable terrain, a hostile
 *  base, or overlaps another player's interpolated tank.
 *  Purely cosmetic — the server does authoritative collision.
 *  onBoat governs water passability (predicted shells carry
 *  the launching tank's flag; projected snapshot shells, which
 *  don't carry it, pass false).
 *
 *  hitBaseIdx, when non-NULL, reports the zero-based index of
 *  the base a hostile-base hit landed on, and BASE_NOT_FOUND
 *  otherwise, so the caller can carry the damage into
 *  base-death prediction. (basesGetBaseNum itself answers
 *  1-based; the conversion happens here so callers index the
 *  base arrays directly.) Callers with nothing to predict
 *  pass NULL.
 *********************************************************/
static bool clientShellVisualBlocked(ClientSim *cs, WORLD newX, WORLD newY,
                                     uint8_t owner, bool onBoat,
                                     BYTE *hitBaseIdx) {
  BYTE mapX = (BYTE)(newX >> TANK_SHIFT_MAPSIZE);
  BYTE mapY = (BYTE)(newY >> TANK_SHIFT_MAPSIZE);
  BYTE p;
  bool onHostileBase = basesExistPos(&cs->sim.bs, mapX, mapY) &&
                       basesCanHit(&cs->sim, mapX, mapY, owner);
  if (hitBaseIdx != NULL) {
    *hitBaseIdx = BASE_NOT_FOUND;
  }
  if (pillsIsPillHit(&cs->sim.pb, mapX, mapY) ||
      (!mapIsPassable(&cs->sim.mp, mapX, mapY, onBoat) &&
       !basesExistPos(&cs->sim.bs, mapX, mapY)) ||
      (basesExistPos(&cs->sim.bs, mapX, mapY) && (onBoat || onHostileBase))) {
    if (onHostileBase && hitBaseIdx != NULL) {
      BYTE num = basesGetBaseNum(&cs->sim.bs, mapX, mapY);
      if (num != BASE_NOT_FOUND && num != 0) {
        *hitBaseIdx = (BYTE)(num - 1);
      }
    }
    return true;
  }
  /* Overlap with other players' tanks (interpolated positions) */
  for (p = 0; p < MAX_TANKS; p++) {
    WORLD tkX, tkY;
    TURNTYPE tkAngle;
    bool tkOnBoat;
    if (p == cs->interpCtx.localPlayer) continue;
    if (!interpIsAlive(&cs->interpCtx, p)) continue;
    if (interpGetPosition(&cs->interpCtx, p, 1.0f, &tkX, &tkY, &tkAngle, &tkOnBoat)) {
      /* Match the authoritative shell hit-zone (circle by default; square
       * under BOLO_LEGACY_SQUARE_COLLISION). See TANK_HIT_RADIUS. */
#ifdef BOLO_LEGACY_SQUARE_COLLISION
      if (abs((int)newX - (int)tkX) < 128 && abs((int)newY - (int)tkY) < 128) {
        return true;
      }
#else
      int dx = (int)newX - (int)tkX, dy = (int)newY - (int)tkY;
      /* Bounding-box pre-test bounds dx/dy before squaring; see tankIsTankHit
       * in tank.c. Never rejects a real hit, prevents int overflow. */
      if (abs(dx) < TANK_HIT_RADIUS && abs(dy) < TANK_HIT_RADIUS &&
          dx * dx + dy * dy < TANK_HIT_RADIUS_SQUARED) {
        return true;
      }
#endif
    }
  }
  return false;
}

/*********************************************************
 *NAME:          clientSimAdvancePredictedShells
 *PURPOSE:
 *  Moves each predicted shell forward by one tick and
 *  removes any that have expired.
 *
 *  A shell that lands on an enemy base also arms base-death
 *  prediction when the hit would drop it to MIN_ARMOUR_CAPTURE:
 *  see tankBasePredictedDrivable. This is what stops a tank
 *  driving into a base it is killing from fighting its own
 *  prediction for a round trip.
 *
 *  Only the shell in flight is counted, not a second one
 *  behind it — the client's base armour is a mirror of what
 *  the server sent and is deliberately not decremented here,
 *  so two of our shells in flight at once predict off the same
 *  starting armour. That under-predicts (the square stays
 *  blocked until the server says otherwise, the behaviour we
 *  had all along) rather than predicting a death that isn't
 *  coming. Out of BASE_PREDICT_REVEAL_RANGE the armour reads
 *  BASE_FULL_ARMOUR, which fails the same safe way.
 *********************************************************/
void clientSimAdvancePredictedShells(ClientSim *cs) {
  int i;
  for (i = 0; i < cs->predictedShellCount; ) {
    PredictedShell *ps = &cs->predictedShells[i];
    WORLD newX, newY;
    BYTE hitBaseIdx;
    if (ps->length <= SHELL_DEATH) {
      /* Shell expired — remove by swapping with last */
      cs->predictedShells[i] = cs->predictedShells[cs->predictedShellCount - 1];
      cs->predictedShellCount--;
      continue;
    }
    /* Move shell forward using float accumulation — matches server HP precision */
    ps->fx += ps->vx;
    ps->fy += ps->vy;
    newX = (WORLD)(int)ps->fx;
    newY = (WORLD)(int)ps->fy;

    if (clientShellVisualBlocked(cs, newX, newY, ps->owner, ps->onBoat,
                                 &hitBaseIdx)) {
      if (hitBaseIdx < MAX_BASES && cs->sim.replayTick != 0) {
        int armour = (int)(*cs->sim.bs).item[hitBaseIdx].armour;
        /* Remember the landing whatever the armour read: if the mirror was
         * behind by an earlier hit still in flight from the server,
         * clientBaseArmourArrived arms the stamp from this tick once that
         * hit's armour lands. */
        cs->sim.basePredictedHitTick[hitBaseIdx] = cs->sim.replayTick;
        if (armour > MIN_ARMOUR_CAPTURE &&
            armour - DAMAGE <= MIN_ARMOUR_CAPTURE) {
          cs->sim.basePredictedDeadTick[hitBaseIdx] = cs->sim.replayTick;
        }
      }
      cs->predictedShells[i] = cs->predictedShells[cs->predictedShellCount - 1];
      cs->predictedShellCount--;
      continue;
    }

    ps->x = newX;
    ps->y = newY;
    ps->length--;
    i++;
  }
}

/*********************************************************
 *NAME:          clientShellProjectAgeTicks
 *PURPOSE:
 *  Converts a one-way-latency ping (ms) into a snapshot age
 *  in 20ms game ticks — the unit the projection velocity
 *  steps in — clamped to PROJECTION_MAX_TICKS so a wild ping
 *  estimate cannot fling a shell arbitrarily far ahead.
 *********************************************************/
int clientShellProjectAgeTicks(uint16_t pingMs) {
  int ageTicks = ((int)pingMs / 2) / 20;   /* one-way latency in game ticks */
  if (ageTicks > PROJECTION_MAX_TICKS) {
    ageTicks = PROJECTION_MAX_TICKS;
  }
  if (ageTicks < 0) {
    ageTicks = 0;
  }
  return ageTicks;
}

/*********************************************************
 *NAME:          clientShellProject
 *PURPOSE:
 *  Pure dead-reckoning of a snapshot shell: derives the
 *  per-game-tick velocity from angle+SHELL_SPEED (same basis
 *  as predicted shells) and returns the anchored float
 *  position snap + velocity*ageTicks. No ClientSim needed.
 *********************************************************/
void clientShellProject(uint16_t snapX, uint16_t snapY, uint8_t angle,
                        int ageTicks, float *outFx, float *outFy,
                        float *outVx, float *outVy) {
  int32_t xStepHP, yStepHP;
  float vx, vy;
  utilCalcDistanceHP(&xStepHP, &yStepHP, (TURNTYPE)angle, SHELL_SPEED);
  vx = (float)xStepHP / 256.0f;
  vy = (float)yStepHP / 256.0f;
  *outVx = vx;
  *outVy = vy;
  *outFx = (float)snapX + vx * (float)ageTicks;
  *outFy = (float)snapY + vy * (float)ageTicks;
}

/* Cross-snapshot match tolerance for projected shells, in world units.
 * A shell travels SHELL_SPEED units per tick, so this is a few ticks of
 * travel: within it an incoming shell is taken to be the same shell as one
 * we were already carrying (re-anchored, keeping its smooth accumulator);
 * beyond it the old shell is dropped and the new one anchored fresh. */
#define PROJECTION_MATCH_DIST (4 * SHELL_SPEED)

/*********************************************************
 *NAME:          clientSimRebuildProjectedShells
 *PURPOSE:
 *  (Re)builds the projected-shell array from the current
 *  serverShellSnaps (already own-filtered for humans),
 *  anchoring each to snap + velocity*age. Incoming shells
 *  that match one carried from the previous snapshot (same
 *  owner, nearest position within PROJECTION_MATCH_DIST)
 *  keep that shell's float accumulator so frame-to-frame
 *  age-estimate jitter doesn't twitch the rendered position;
 *  unmatched incoming shells anchor fresh; carried shells
 *  with no match are dropped. Human clients only.
 *********************************************************/
void clientSimSetProjectionPing(ClientSim *cs, uint16_t pingMs) {
  cs->projectionPingMs = pingMs;
}

void clientSimRebuildProjectedShells(ClientSim *cs, uint16_t pingMs) {
  ProjectedShell next[MAX_SNAPSHOT_SHELLS];
  bool usedPrev[MAX_SNAPSHOT_SHELLS];
  int nextCount = 0;
  int ageTicks = clientShellProjectAgeTicks(pingMs);
  int i, j;

  for (j = 0; j < cs->projectedShellCount; j++) {
    usedPrev[j] = false;
  }

  for (i = 0; i < cs->serverShellCount && nextCount < MAX_SNAPSHOT_SHELLS; i++) {
    const ShellSnapshot *s = &cs->serverShellSnaps[i];
    ProjectedShell *ns = &next[nextCount];
    float fx, fy, vx, vy;
    int best = -1;
    float bestDistSq = (float)PROJECTION_MATCH_DIST * (float)PROJECTION_MATCH_DIST;

    clientShellProject(s->worldX, s->worldY, s->angle, ageTicks, &fx, &fy, &vx, &vy);

    for (j = 0; j < cs->projectedShellCount; j++) {
      const ProjectedShell *prev = &cs->projectedShells[j];
      float dx, dy, distSq;
      if (usedPrev[j] || prev->owner != s->owner) {
        continue;
      }
      dx = prev->fx - fx;
      dy = prev->fy - fy;
      distSq = dx * dx + dy * dy;
      if (distSq <= bestDistSq) {
        bestDistSq = distSq;
        best = j;
      }
    }

    if (best >= 0) {
      usedPrev[best] = true;
      ns->fx = cs->projectedShells[best].fx;
      ns->fy = cs->projectedShells[best].fy;
    } else {
      ns->fx = fx;
      ns->fy = fy;
    }
    ns->vx = vx;
    ns->vy = vy;
    ns->angle = s->angle;
    ns->owner = s->owner;
    /* The projection flew the shell ageTicks forward, so it has ageTicks less
     * life remaining. Without this a projected shell renders up to ageTicks
     * past its true impact during a snapshot gap. Clamp at 0 — a shell already
     * at/past impact is culled by clientSimAdvanceProjectedShells. */
    ns->length = (s->length > (uint8_t)ageTicks)
                     ? (uint8_t)(s->length - ageTicks)
                     : 0;
    ns->active = true;
    nextCount++;
  }

  memcpy(cs->projectedShells, next, (size_t)nextCount * sizeof(ProjectedShell));
  cs->projectedShellCount = nextCount;
}

/*********************************************************
 *NAME:          clientSimAdvanceProjectedShells
 *PURPOSE:
 *  Moves each projected shell forward one game tick between
 *  snapshots and culls any that expire or hit the shared
 *  visual collision short-circuit. onBoat is unknown for
 *  snapshot shells, so false is used: a boat-launched shell
 *  over water may cull a tick early and respawn on the next
 *  snapshot — an accepted visual approximation.
 *********************************************************/
void clientSimAdvanceProjectedShells(ClientSim *cs) {
  int i;
  for (i = 0; i < cs->projectedShellCount; ) {
    ProjectedShell *ps = &cs->projectedShells[i];
    WORLD newX, newY;
    if (ps->length <= SHELL_DEATH) {
      cs->projectedShells[i] = cs->projectedShells[cs->projectedShellCount - 1];
      cs->projectedShellCount--;
      continue;
    }
    ps->fx += ps->vx;
    ps->fy += ps->vy;
    newX = (WORLD)(int)ps->fx;
    newY = (WORLD)(int)ps->fy;

    /* Other players' shells — NULL: nothing of ours to predict off them. */
    if (clientShellVisualBlocked(cs, newX, newY, ps->owner, false, NULL)) {
      cs->projectedShells[i] = cs->projectedShells[cs->projectedShellCount - 1];
      cs->projectedShellCount--;
      continue;
    }

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
aiType  clientSimGetAiType(ClientSim *cs)              { return *clientSimGetAllowComputerTanks(cs); }
void    clientSimSetAiType(ClientSim *cs, aiType value) { *clientSimGetAllowComputerTanks(cs) = value; }

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

void clientSimSetControlObserver(ClientSim *cs, ControlObserverCb cb, void *ctx) {
  if (cs == NULL) return;
  cs->controlObserverCb  = cb;
  cs->controlObserverCtx = ctx;
}

void clientSimSetTransportControlObserver(ClientSim *cs, ControlObserverCb cb, void *ctx) {
  if (cs == NULL) return;
  cs->transportObserverCb  = cb;
  cs->transportObserverCtx = ctx;
}

BYTE clientSimGetPendingAllianceRequest(const ClientSim *cs) {
  if (cs == NULL) return 0xFF;
  return cs->pendingAllianceRequestFrom;
}

void clientSimClearPendingAllianceRequest(ClientSim *cs) {
  if (cs == NULL) return;
  cs->pendingAllianceRequestFrom = 0xFF;
}

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

void clientSimClearLobbyChatHistory(ClientSim *cs) {
  cs->lobbyChatHistory[0] = '\0';
  cs->lobbyTeamChatHistory[0] = '\0';
}

void clientSimAppendLobbyTeamChat(ClientSim *cs, const char *name, const char *message) {
  size_t histLen = strlen(cs->lobbyTeamChatHistory);
  size_t needed = strlen(name) + 2 + strlen(message) + 2; /* "name: message\n" */
  if (histLen + needed < sizeof(cs->lobbyTeamChatHistory)) {
    if (histLen > 0) {
      strcat(cs->lobbyTeamChatHistory, "\n");
    }
    strcat(cs->lobbyTeamChatHistory, name);
    strcat(cs->lobbyTeamChatHistory, ": ");
    strcat(cs->lobbyTeamChatHistory, message);
  }
}

/* Default chatSendFunc: route outbound chat through clientSimSubmitCommand
 * via clientSimNetSendChat. Set as the default for every ClientSim —
 * frontends that want different behavior can override via
 * clientSimSetChatSendFunc, but the default is correct for both
 * UDP-connected humans (carrier enqueue → server CMD_CHAT arm) and
 * in-process bots (dispatcher under the mutex). fromPlayer is unused —
 * the slot is stamped either from the connection (UDP) or from
 * lctx->playerNum (local). */
void clientSimDefaultChatSend(ClientSim *cs, BYTE fromPlayer, BYTE destPlayer,
                              const char *message) {
  (void)fromPlayer;
  clientSimNetSendChat(cs, destPlayer, message);
}

void clientSimMessageSendAllPlayers(ClientSim *cs, BYTE playerNum, char *message) {
  if (cs->chatSendFunc != NULL) {
    cs->chatSendFunc(cs, playerNum, 0xFF, message);
  }
}

void clientSimMessageSendPlayer(ClientSim *cs, BYTE playerNum, BYTE destPlayer, char *message) {
  if (cs->chatSendFunc != NULL) {
    cs->chatSendFunc(cs, playerNum, destPlayer, message);
  }
}

void clientSimSendChangePlayerName(ClientSim *cs, BYTE playerNum, char *newName) {
  if (cs->nameChangeSendFunc != NULL) {
    cs->nameChangeSendFunc(newName);
  }
}

void clientSimGetPlayerName(ClientSim *csPtr, char *value, size_t valueSize) {
  if (valueSize == 0) {
    return;
  }
  if (clientSimGetGameSim(csPtr)->plyrs == NULL) {
    SDL_strlcpy(value, clientSimGetMyLastPlayerName(csPtr), valueSize);
  } else {
    playersGetPlayerName(&clientSimGetGameSim(csPtr)->plyrs,
                         clientSimGetMyPlayerNum(csPtr), value, valueSize,
                         FALSE);
  }
}

bool clientSimSetPlayerName(ClientSim *csPtr, char *value) {
  bool returnValue;              /* Value to return */

  utilStripNameReplace(value);
  returnValue = playersSetPlayerName(csPtr, clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), value, FALSE);
  if (returnValue == TRUE) {
    if (clientSimGetNetType(csPtr) != netSingle) {
      clientSimSendChangePlayerName(csPtr, clientSimGetMyPlayerNum(csPtr), value);
    }
  }
  return returnValue;
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

/* High-level send-message wrappers — build the sender topLine, locally
 * echo into the message ticker, then dispatch through the lower-level
 * transport / players_send_* fan-out. */
void clientSimSendMessageAllPlayers(ClientSim *csPtr, char *messageStr) {
  char topLine[FILENAME_MAX];       /* The message topline */

  topLine[0] = '\0';
  playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), topLine);
  clientMessageAdd(clientSimGetMessages(csPtr), (messageType) (clientSimGetMyPlayerNum(csPtr) + PLAYER_MESSAGE_OFFSET), topLine, messageStr);
  clientSimMessageSendAllPlayers(csPtr, clientSimGetMyPlayerNum(csPtr), messageStr);
}

void clientSimSendMessageAllAllies(ClientSim *csPtr, char *messageStr) {
  char topLine[FILENAME_MAX];       /* The message topline */

  topLine[0] = '\0';
  playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), topLine);
  clientMessageAdd(clientSimGetMessages(csPtr), (messageType) (clientSimGetMyPlayerNum(csPtr) + PLAYER_MESSAGE_OFFSET), topLine, messageStr);
  playersSendMessageAllAllies(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), messageStr);
}

void clientSimSendMessageAllNearby(ClientSim *csPtr, char *messageStr) {
  char topLine[FILENAME_MAX];       /* The message topline */

  topLine[0] = '\0';
  playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), topLine);
  clientMessageAdd(clientSimGetMessages(csPtr), (messageType) (clientSimGetMyPlayerNum(csPtr) + PLAYER_MESSAGE_OFFSET), topLine, messageStr);
  playersSendMessageAllNearby(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), tankGetMX(&MY_TANK(csPtr)), tankGetMY(&MY_TANK(csPtr)), messageStr);
}

void clientSimSendMessageAllSelected(ClientSim *csPtr, char *messageStr) {
  playersSendMessageAllSelected(csPtr, clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), messageStr);
}

/* Local-player alliance actions — sugar over the targeted variants. */
void clientSimLeaveAllianceSelf(ClientSim *csPtr) {
  clientSimLeaveAlliance(csPtr, clientSimGetMyPlayerNum(csPtr));
}

void clientSimRequestAllianceSelected(ClientSim *csPtr) {
  playersRequestAlliance(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr));
}

/* Players-panel selection helpers — thread gameSim->plyrs + the local
 * player num through to the underlying players* API. */
void clientSimTogglePlayerCheckState(ClientSim *csPtr, BYTE playerNum) {
  playersToggleCheckedState(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), playerNum, FALSE);
}

void clientSimCheckAllNonePlayers(ClientSim *csPtr, bool isChecked) {
  playersCheckAllNone(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), isChecked, FALSE);
}

void clientSimCheckAlliedPlayers(ClientSim *csPtr) {
  playersCheckAllies(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), FALSE);
}

void clientSimCheckNearbyPlayers(ClientSim *csPtr) {
  playersCheckNearbyPlayers(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), tankGetMX(&MY_TANK(csPtr)), tankGetMY(&MY_TANK(csPtr)), FALSE);
}

int clientSimGetNumCheckedPlayers(ClientSim *csPtr) {
  return playersGetNumChecked(&clientSimGetGameSim(csPtr)->plyrs);
}

int clientSimGetNumAllies(ClientSim *csPtr) {
  return playersGetNumAllies(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr));
}

int clientSimGetNumNearbyTanks(ClientSim *csPtr) {
  return playersNumNearbyPlayers(&clientSimGetGameSim(csPtr)->plyrs, tankGetMX(&MY_TANK(csPtr)), tankGetMY(&MY_TANK(csPtr)));
}

/* Tank preferences (per-instance) — operate on the local player's tank */
bool clientSimGetTankAutoSlowdown(ClientSim *cs) {
  return tankGetAutoSlowdown(&MY_TANK(cs));
}

void clientSimSetTankAutoSlowdown(ClientSim *cs, bool useSlowdown) {
  tankSetAutoSlowdown(&MY_TANK(cs), useSlowdown);
}

bool clientSimGetTankAutoHideGunsight(ClientSim *cs) {
  return tankGetAutoHideGunsight(&MY_TANK(cs));
}

void clientSimSetTankAutoHideGunsight(ClientSim *cs, bool useAutohide) {
  tankSetAutoHideGunsight(&MY_TANK(cs), useAutohide);
}

void clientSimSetGunsight(ClientSim *cs, bool shown) {
  tankSetGunsight(&MY_TANK(cs), shown);
}

BYTE clientSimGetTank256Dir(ClientSim *cs) {
  return tankGet256Dir(&MY_TANK(cs));
}

int clientSimGetMyTankDeathWait(ClientSim *cs) {
  if (cs == NULL) return 0;
  return tankGetDeathWait(&MY_TANK(cs));
}

int clientSimGetMyTankLastDeath(ClientSim *cs) {
  if (cs == NULL) return 0;
  return tankGetLastTankDeath(&MY_TANK(cs));
}

bool clientSimIsMyTankDeathBlackout(ClientSim *cs) {
  if (cs == NULL || MY_TANK(cs) == NULL || clientSimIsMyTankAlive(cs)) {
    return false;
  }
  return overviewMapDeathBlackout(clientSimGetMyTankDeathWait(cs),
                                  clientSimGetMyTankLastDeath(cs));
}

/* Game info (per-instance) */
bool clientSimGetAllowHiddenMines(ClientSim *cs) {
  return minesGetAllowHiddenMines(&clientSimGetGameSim(cs)->mns);
}

BYTE clientSimGetNumPlayers(ClientSim *cs) {
  return playersGetNumPlayers(&clientSimGetGameSim(cs)->plyrs);
}

bool clientSimIsTutorial(const ClientSim *cs) {
  return clientSimGetGameSim((ClientSim *)cs)->isTutorial;
}

void clientSimSetTutorial(ClientSim *cs, bool v) {
  clientSimGetGameSim(cs)->isTutorial = v;
}

gameType clientSimGetGameType(const ClientSim *cs) {
  return clientSimGetGameSim((ClientSim *)cs)->game;
}

uint16_t clientSimGetPlayerPing(ClientSim *cs, BYTE playerNum) {
  return playersGetPing(&clientSimGetGameSim(cs)->plyrs, playerNum);
}

PingBand clientSimGetPlayerPingBand(ClientSim *cs, BYTE playerNum) {
  if (cs == NULL || playerNum >= MAX_TANKS) {
    return PING_BAND_NONE;
  }
  return pingDisplayBand(&cs->displayPing[playerNum]);
}

void clientSimResetPlayerDisplayPing(ClientSim *cs, BYTE playerNum) {
  if (cs != NULL && playerNum < MAX_TANKS) {
    pingDisplayReset(&cs->displayPing[playerNum]);
  }
}

uint8_t clientSimGetPlayerClientFlags(ClientSim *cs, BYTE playerNum) {
  return playersGetClientFlags(&clientSimGetGameSim(cs)->plyrs, playerNum);
}

uint8_t clientSimGetPlayerClientType(ClientSim *cs, BYTE playerNum) {
  return playersGetClientType(&clientSimGetGameSim(cs)->plyrs, playerNum);
}

void clientSimGetPlayerLocation(ClientSim *cs, BYTE playerNum, char *dest,
                                size_t destSize) {
  playersGetPlayerLocation(&clientSimGetGameSim(cs)->plyrs, playerNum, dest,
                           destSize);
}

uint8_t clientSimGetPlayerAccountFlags(ClientSim *cs, BYTE playerNum) {
  return playersGetAccountFlags(&clientSimGetGameSim(cs)->plyrs, playerNum);
}

void clientSimGetPlayerCountryCode(ClientSim *cs, BYTE playerNum, char *dest) {
  playersGetCountryCode(&clientSimGetGameSim(cs)->plyrs, playerNum, dest);
}

bool clientSimIsPlayerAlly(ClientSim *cs, BYTE playerA, BYTE playerB) {
  return playersIsAllie(&clientSimGetGameSim(cs)->plyrs, playerA, playerB);
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

bool netSetup(ClientSim *cs, netType value, unsigned short myPort, char *targetIp, unsigned short targetPort, char *password, bool usCreate, char *trackerAddr, unsigned short trackerPort, bool useTracker, bool wantRejoin, bool useWinboloNet, const char *wbnApiToken, const char *wbnServerKey) {
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

/* DNS lookup completion callback — called from the dns_lookups.c thread.
 * Stores the reverse-DNS of the server address for the lobby / net-info UI
 * (visual only; the connected host is never changed). The lookup thread passes
 * host == ip when no PTR record exists, which we record as "no name". Guarded
 * by the client mutex so the GUI thread reading these fields can't tear. */
void netProcessedDnsLookup(ClientSim *cs, char *ip, char *host) {
  clientMutexWaitFor();
  strncpy(cs->serverHostResultIp, ip, sizeof(cs->serverHostResultIp) - 1);
  cs->serverHostResultIp[sizeof(cs->serverHostResultIp) - 1] = '\0';
  if (host != NULL && strcmp(host, ip) != 0) {
    strncpy(cs->serverHostName, host, sizeof(cs->serverHostName) - 1);
    cs->serverHostName[sizeof(cs->serverHostName) - 1] = '\0';
  } else {
    cs->serverHostName[0] = '\0'; /* address has no PTR record */
  }
  clientMutexRelease();
}

void clientSimRequestServerHostname(ClientSim *cs, const char *ip) {
  bool needRequest = false;
  if (cs == NULL || ip == NULL || ip[0] == '\0') {
    return;
  }
  clientMutexWaitFor();
  /* Debounce on the last-requested IP. On a new address, clear any cached
   * result so a stale name isn't shown for the new server while the lookup
   * is in flight. */
  if (strcmp(cs->serverHostReqIp, ip) != 0) {
    strncpy(cs->serverHostReqIp, ip, sizeof(cs->serverHostReqIp) - 1);
    cs->serverHostReqIp[sizeof(cs->serverHostReqIp) - 1] = '\0';
    cs->serverHostResultIp[0] = '\0';
    cs->serverHostName[0] = '\0';
    needRequest = true;
  }
  clientMutexRelease();
  if (needRequest) {
    dnsLookupsAddRequest((char *)ip, NULL);
  }
}

bool clientSimGetServerHostname(ClientSim *cs, const char *ip, char *out,
                                size_t outLen) {
  bool ok = false;
  if (out == NULL || outLen == 0 || ip == NULL) {
    return false;
  }
  clientMutexWaitFor();
  if (cs->serverHostName[0] != '\0' &&
      strcmp(cs->serverHostResultIp, ip) == 0) {
    strncpy(out, cs->serverHostName, outLen - 1);
    out[outLen - 1] = '\0';
    ok = true;
  }
  clientMutexRelease();
  return ok;
}

/* ================================================================
 * Read accessors — see header for the contract.
 * ================================================================ */

bool clientSimIsRunning(const ClientSim *cs)              { return cs->running; }
bool clientSimIsBot(const ClientSim *cs)                  { return cs->isBot; }
bool clientSimIsInPillView(const ClientSim *cs)           { return cs->viewport.viewKind == VIEW_KIND_PILL; }
bool clientSimIsInItemView(const ClientSim *cs)           { return cs->viewport.viewKind != VIEW_KIND_TANK; }
uint8_t clientSimGetViewKind(const ClientSim *cs)         { return cs->viewport.viewKind; }
BYTE clientSimGetViewTarget(const ClientSim *cs)          { return cs->viewport.viewTarget; }
bool clientSimIsNeedScreenReCalc(const ClientSim *cs)     { return cs->viewport.needRecalc; }
bool clientSimIsInLobby(const ClientSim *cs)              { return cs->inLobby; }
bool clientSimIsMapDownloadComplete(const ClientSim *cs)  { return cs->mapDownloadComplete; }

uint8_t clientSimGetMapDownloadPercent(const ClientSim *cs) {
    if (cs == NULL || !cs->hasTransport) return 0;
    if (!cs->isUdpTransport) return 100;
    return transportUdpClientGetMapDownloadPercent((Transport *)&cs->transport);
}
bool clientSimIsMapSkipAvailable(const ClientSim *cs)     { return cs->mapSkipAvailable; }
bool clientSimIsLobbyAvailable(const ClientSim *cs)       { return cs->lobbyAvailable; }
bool clientSimIsMapSkipMyVote(const ClientSim *cs)        { return cs->mapSkipMyVote; }
bool clientSimIsLobbyHiddenMines(const ClientSim *cs)     { return cs->lobbyHiddenMines; }
bool clientSimIsBalanceProposalActive(const ClientSim *cs){ return cs->balanceProposalActive; }
uint64_t clientSimGetLastBalanceProposalArrivedMs(const ClientSim *cs) {
    return cs ? cs->lastBalanceProposalArrivedMs : 0;
}
uint64_t clientSimGetLastBalanceFailedMs(const ClientSim *cs) {
    return cs ? cs->lastBalanceFailedMs : 0;
}
uint8_t  clientSimGetLastBalanceFailedReason(const ClientSim *cs) {
    return cs ? cs->lastBalanceFailedReason : 0;
}
bool clientSimIsLabelOwnTank(const ClientSim *cs)         { return cs->labelOwnTank; }

buildSelect clientSimGetCurrentBuildSelect(const ClientSim *cs) { return cs->currentBuildSelect; }
gameType    clientSimGetLobbyGameType(const ClientSim *cs)      { return cs->lobbyGameType; }
bool        clientSimGetLobbyIsScenarioMap(const ClientSim *cs) { return cs->lobbyScenarioMap; }
const char *clientSimGetLobbyScenarioDesc(const ClientSim *cs) { return cs->lobbyScenarioDesc; }
bool        clientSimGetLobbyScenarioExtraTeams(const ClientSim *cs) { return cs->lobbyScenarioExtraTeams; }
labelLen    clientSimGetLabelMessage(const ClientSim *cs)       { return cs->labelMessage; }
labelLen    clientSimGetLabelTankLabel(const ClientSim *cs)     { return cs->labelTankLabel; }

BYTE     clientSimGetMyPlayerNum(const ClientSim *cs)       { return cs->myPlayerNum; }
bool     clientSimIsSpectator(const ClientSim *cs)         { return cs->isSpectator; }
BYTE     clientSimGetXOffset(const ClientSim *cs)           { return cs->viewport.xOffset; }
BYTE     clientSimGetYOffset(const ClientSim *cs)           { return cs->viewport.yOffset; }
int      clientSimGetSubPosX(const ClientSim *cs)           { return (int)cs->scroll.subPosX; }
int      clientSimGetSubPosY(const ClientSim *cs)           { return (int)cs->scroll.subPosY; }
BYTE     clientSimGetPillViewX(const ClientSim *cs)         { return cs->viewport.viewX; }
BYTE     clientSimGetPillViewY(const ClientSim *cs)         { return cs->viewport.viewY; }
BYTE     clientSimGetPendingBuildAction(const ClientSim *cs){ return cs->pendingBuildAction; }
BYTE     clientSimGetPendingBuildX(const ClientSim *cs)     { return cs->pendingBuildX; }
BYTE     clientSimGetPendingBuildY(const ClientSim *cs)     { return cs->pendingBuildY; }
int      clientSimGetCursorPosX(const ClientSim *cs)        { return cs->viewport.cursorPosX; }
int      clientSimGetCursorPosY(const ClientSim *cs)        { return cs->viewport.cursorPosY; }
int      clientSimGetGmeStartDelay(const ClientSim *cs)     { return cs->gmeStartDelay; }
int      clientSimGetCountdownSeconds(const ClientSim *cs)  { return cs->countdownSeconds; }
int      clientSimGetServerShellCount(const ClientSim *cs)  { return cs->serverShellCount; }
int      clientSimGetPredictedShellCount(const ClientSim *cs){ return cs->predictedShellCount; }
int      clientSimGetProjectedShellCount(const ClientSim *cs){ return cs->projectedShellCount; }
int      clientSimGetBrainEventCount(const ClientSim *cs)   { return cs->brainEventCount; }
int32_t  clientSimGetGmeLength(const ClientSim *cs)         { return cs->gmeLength; }
int32_t  clientSimGetLobbyTimeLimit(const ClientSim *cs)    { return cs->lobbyTimeLimit; }
uint8_t  clientSimGetLobbyAiType(const ClientSim *cs)       { return cs->lobbyAiType; }
uint8_t  clientSimGetLobbyPillCount(const ClientSim *cs)    { return cs->lobbyPillCount; }
uint8_t  clientSimGetLobbyBaseCount(const ClientSim *cs)    { return cs->lobbyBaseCount; }
uint8_t  clientSimGetLobbyStartCount(const ClientSim *cs)   { return cs->lobbyStartCount; }
uint8_t  clientSimGetBrainLastAssistMsg(const ClientSim *cs){ return cs->brainLastAssistMsg; }
uint32_t clientSimGetLastServerTick(const ClientSim *cs)    { return cs->lastServerTick; }
unsigned short clientSimGetServerPort(const ClientSim *cs)  { return cs->serverPort; }
time_t   clientSimGetTimeStart(const ClientSim *cs)         { return cs->timeStart; }

const char *clientSimGetMapName(const ClientSim *cs)          { return cs->mapName; }
const char *clientSimGetLobbyChatHistory(const ClientSim *cs) { return cs->lobbyChatHistory; }
const char *clientSimGetLobbyTeamChatHistory(const ClientSim *cs) { return cs->lobbyTeamChatHistory; }
const char *clientSimGetMyLastPlayerName(const ClientSim *cs) { return cs->myLastPlayerName; }

const ClientLobbySlot *clientSimGetLobbySlot(const ClientSim *cs, BYTE n) {
  if (n >= 16) return NULL;
  return &cs->lobbySlots[n];
}

const ClientPlayerStats *clientSimGetPlayerStats(const ClientSim *cs,
                                                 BYTE playerNum) {
  if (cs == NULL || playerNum >= MAX_TANKS) return NULL;
  return &cs->liveStats[playerNum];
}

const ClientSpectatorSlot *clientSimGetSpectatorSlot(const ClientSim *cs, uint8_t idx) {
  if (idx >= MAX_SPECTATORS) return NULL;
  return &cs->spectatorSlots[idx];
}

BYTE clientSimGetLobbyNumConnected(const ClientSim *cs) {
  if (cs == NULL) return 0;
  BYTE count = 0;
  for (BYTE i = 0; i < 16; i++) {
    if (cs->lobbySlots[i].connected) count++;
  }
  return count;
}

PlayerBitMap clientSimGetVoiceTalkingMap(const ClientSim *cs) {
  if (cs == NULL) return 0;
  return cs->voiceTalkingMap;
}

bool clientSimIsMapSkipVote(const ClientSim *cs, BYTE n) {
  if (n >= 16) return false;
  return cs->mapSkipVotes[n];
}

static int clientGameVoteIdx(uint8_t kind) {
  if (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) return 0;
  if (kind == GAME_VOTE_KIND_SURRENDER)     return 1;
  return -1;
}

bool clientSimGetGameVote(const ClientSim *cs, uint8_t kind,
                          ClientGameVoteSnapshot *out) {
  if (!cs || !out) return false;
  int idx = clientGameVoteIdx(kind);
  if (idx < 0) return false;
  const struct ClientGameVote *gv = &cs->gameVotes[idx];
  out->kind             = gv->kind ? gv->kind : kind;
  out->active           = gv->active;
  out->triggerSrc       = gv->triggerSrc;
  out->teamId           = gv->teamId;
  out->threshold        = gv->threshold;
  out->yesCount         = gv->yesCount;
  out->noCount          = gv->noCount;
  out->eligibleCount    = gv->eligibleCount;
  out->secondsRemaining = gv->secondsRemaining;
  out->votes            = gv->votes;
  out->widgetVisible    = gv->widgetVisible;
  out->concludedAtMs    = gv->concludedAtMs;
  return true;
}

bool clientSimMayAnswerGameVote(const ClientSim *cs, uint8_t kind,
                                uint8_t teamId) {
  if (!cs) return false;
  if (kind != GAME_VOTE_KIND_SURRENDER) return true;
  /* A spectator has no slot and no team, so it is never part of a
   * surrender electorate. Guard explicitly — myPlayerNum is a stale
   * player index for a spectator, not an empty one. */
  if (cs->isSpectator) return false;
  BYTE me = cs->myPlayerNum;
  if (me >= MAX_TANKS) return false;
  BYTE myTeam = cs->lobbySlots[me].teamNumber;
  return myTeam != 0 && myTeam == teamId;
}

void clientSimSetGameVoteWidgetVisible(ClientSim *cs, uint8_t kind, bool visible) {
  if (!cs) return;
  int idx = clientGameVoteIdx(kind);
  if (idx < 0) return;
  cs->gameVotes[idx].widgetVisible = visible;
}

/* Stub kept so the public declaration stays linkable. Client-side
 * countdown rendering didn't work in this build's render-loop
 * topology (in-game loop exits as soon as gameOver fires when
 * cs->inLobby is already latched true). The countdown is now
 * server-driven again via transportUdpServerSendServerMessage. */
void clientSimTickLobbyReturnCountdown(ClientSim *cs) {
  (void)cs;
}

bool clientSimGameVoteMyVote(const ClientSim *cs, uint8_t kind) {
  if (!cs) return false;
  int idx = clientGameVoteIdx(kind);
  if (idx < 0) return false;
  BYTE me = cs->myPlayerNum;
  if (me >= 16) return false;
  return (cs->gameVotes[idx].votes & (uint16_t)(1u << me)) != 0;
}

uint8_t clientSimGetReturnToLobbySecs(const ClientSim *cs) {
  return cs ? cs->lastReturnToLobbySecs : 0;
}

uint8_t clientSimGetBalanceProposal(const ClientSim *cs, BYTE n) {
  if (n >= 16) return 0;
  return cs->balanceProposal[n];
}

BYTE *clientSimGetBrainMap(ClientSim *cs) {
  return &cs->brainMap[0][0];
}

const ShellSnapshot *clientSimGetServerShellSnaps(const ClientSim *cs) {
  return cs->serverShellSnaps;
}

const PredictedShell *clientSimGetPredictedShells(const ClientSim *cs) {
  return cs->predictedShells;

}

const ProjectedShell *clientSimGetProjectedShells(const ClientSim *cs) {
  return cs->projectedShells;
}

const GameEvent *clientSimGetBrainEvents(const ClientSim *cs) {
  return cs->brainEvents;
}

void clientSimAddPing(ClientSim *cs, uint8_t sender, uint8_t kind,
                      uint16_t worldX, uint16_t worldY, uint32_t nowMs) {
  ClientPing *slot;
  if (cs == NULL) {
    return;
  }
  /* The write cursor wraps, so a burst overwrites the oldest entries rather
     than being dropped at the door — a player who has just been pinged six
     times wants the six newest. */
  slot = &cs->pings[cs->pingWriteIdx];
  slot->sender = sender;
  slot->kind   = kind;
  slot->worldX = worldX;
  slot->worldY = worldY;
  slot->recvMs = nowMs;
  /* The sender's name, taken now — see ClientPing. The players object is
     gone between games, and a ping cannot arrive then; the guard is for the
     order teardown happens in, not for a case that has a name to find. */
  slot->senderName[0] = '\0';
  if (clientSimGetGameSim(cs)->plyrs != NULL) {
    playersGetPlayerName(&clientSimGetGameSim(cs)->plyrs, sender,
                         slot->senderName, sizeof(slot->senderName), FALSE);
  }
  cs->pingWriteIdx = (cs->pingWriteIdx + 1) % MAX_CLIENT_PINGS;
}

int clientSimGetPings(const ClientSim *cs, uint32_t nowMs,
                      ClientPing *out, int maxOut) {
  int i;
  int count = 0;
  if (cs == NULL || out == NULL || maxOut <= 0) {
    return 0;
  }
  /* Walk from the oldest slot forward so the copy comes out in arrival
     order: the newest ping draws last and therefore on top. */
  for (i = 0; i < MAX_CLIENT_PINGS && count < maxOut; i++) {
    const ClientPing *p = &cs->pings[(cs->pingWriteIdx + i) % MAX_CLIENT_PINGS];
    if (p->recvMs == 0) {
      continue;   /* never written */
    }
    if (nowMs < p->recvMs || nowMs - p->recvMs >= (uint32_t)PING_DISPLAY_MS) {
      continue;   /* expired, or a clock that went backwards */
    }
    out[count++] = *p;
  }
  return count;
}

const OverviewMap *clientSimGetOverviewMap(const ClientSim *cs) {
  if (cs == NULL) {
    return NULL;
  }
  return &cs->overview;
}

struct in_addr clientSimGetServerAddress(const ClientSim *cs) {
  return cs->serverAddress;
}

GameSim       *clientSimGetGameSim(ClientSim *cs)    { return &cs->sim; }
MessageState  *clientSimGetMessages(ClientSim *cs)   { return &cs->messages; }
ScrollState   *clientSimGetScroll(ClientSim *cs)     { return &cs->scroll; }
InterpContext *clientSimGetInterpCtx(ClientSim *cs)  { return &cs->interpCtx; }
screen        *clientSimGetView(ClientSim *cs)       { return &cs->viewport.view; }
screenMines   *clientSimGetMineView(ClientSim *cs)   { return &cs->viewport.mineView; }

const struct ViewPort *clientSimViewport(const ClientSim *cs)  { return &cs->viewport; }
struct ViewPort       *clientSimViewportMut(ClientSim *cs)     { return &cs->viewport; }

BYTE *clientSimGetXOffsetPtr(ClientSim *cs)          { return &cs->viewport.xOffset; }
BYTE *clientSimGetYOffsetPtr(ClientSim *cs)          { return &cs->viewport.yOffset; }
BYTE *clientSimGetPillViewXPtr(ClientSim *cs)        { return &cs->viewport.viewX; }
BYTE *clientSimGetPillViewYPtr(ClientSim *cs)        { return &cs->viewport.viewY; }

char *clientSimGetMapNameMutable(ClientSim *cs)      { return cs->mapName; }

/* ================================================================
 * Mutators — see header for the contract.
 * ================================================================ */

void clientSimSetBalanceProposalActive(ClientSim *cs, bool active) {
  cs->balanceProposalActive = active;
}

void clientSimClearBalanceProposal(ClientSim *cs) {
  memset(cs->balanceProposal, 0, sizeof(cs->balanceProposal));
}

void clientSimSetMapSkipMyVote(ClientSim *cs, bool vote) {
  cs->mapSkipMyVote = vote;
}

void clientSimSetXOffset(ClientSim *cs, BYTE v)            { cs->viewport.xOffset = v; }
void clientSimSetYOffset(ClientSim *cs, BYTE v)            { cs->viewport.yOffset = v; }
void clientSimSetCursorPosX(ClientSim *cs, int v)          { cs->viewport.cursorPosX = v; }
void clientSimSetCursorPosY(ClientSim *cs, int v)          { cs->viewport.cursorPosY = v; }
void clientSimSetNeedScreenReCalc(ClientSim *cs, bool v)   { cs->viewport.needRecalc = v; }
void clientSimSetInPillView(ClientSim *cs, bool v)         { cs->viewport.viewKind = (uint8_t)(v ? VIEW_KIND_PILL : VIEW_KIND_TANK); }
void clientSimSetPillViewX(ClientSim *cs, BYTE v)          { cs->viewport.viewX = v; }
void clientSimSetPillViewY(ClientSim *cs, BYTE v)          { cs->viewport.viewY = v; }
void clientSimSetView(ClientSim *cs, screen v)             { cs->viewport.view = v; }
void clientSimSetMineView(ClientSim *cs, screenMines v)    { cs->viewport.mineView = v; }

void clientSimSetGmeStartDelay(ClientSim *cs, int v)       { cs->gmeStartDelay = v; }
void clientSimSetGmeLength(ClientSim *cs, int32_t v)       { cs->gmeLength = v; }
void clientSimSetGameType(ClientSim *cs, gameType v)       { if (cs) gameTypeSet(&cs->sim.game, v); }
void clientSimSetHiddenMines(ClientSim *cs, bool v)        { if (cs) cs->sim.hiddenMines = v; }
void clientSimSetTimeStart(ClientSim *cs, time_t v)        { cs->timeStart = v; }
void clientSimSetRunning(ClientSim *cs, bool v)            { cs->running = v; }
void clientSimSetLocalTransport(ClientSim *cs, bool isLocal) {
  clientSimGetGameSim(cs)->isLocalTransport = isLocal;
}

void clientSimSetBoundServerSim(ClientSim *cs, struct ServerSim *sim) {
  if (cs == NULL) return;
  cs->boundServerSim = sim;
}

struct ServerSim *clientSimGetBoundServerSim(const ClientSim *cs) {
  if (cs == NULL) return NULL;
  return cs->boundServerSim;
}

void clientSimSetConnectErrorReason(ClientSim *cs, const char *str) {
  if (cs == NULL) return;
  if (str == NULL || str[0] == '\0') {
    cs->connectErrorReason[0] = '\0';
    return;
  }
  strncpy(cs->connectErrorReason, str, sizeof(cs->connectErrorReason) - 1);
  cs->connectErrorReason[sizeof(cs->connectErrorReason) - 1] = '\0';
}

/* Reset the transient game world to a clean slate on entering the
 * lobby. Mirrors serverSimResetGameWorld's subsystem rebuild so the
 * client's GameSim drops the round that just ended — otherwise the
 * frozen last-round world (every remote player frozen where they
 * stopped, spent shells, in-flight explosions, mine craters) lingers
 * behind the lobby until the next game's first full-sync snapshot
 * repopulates it.
 *
 * Scope is deliberately the transient/dynamic world only. The map
 * itself — terrain plus pill/base placement and ownership — is owned by
 * the CTRL_LOBBY_MAP_CHANGE reinstall path and is NOT touched here, so
 * the two reactions don't fight over mp/pb/bs. Player identity in
 * cs->sim.plyrs (name, country, flags) is connection-scoped and
 * survives; only round-scoped render state is cleared, matching the
 * server.
 *
 * The local slot is preserved: only MY_TANK is a real tank object on
 * the client, and the snapshot's first-sync branch repositions it but
 * does NOT recreate it (client_snapshot.c gates on MY_TANK != NULL).
 * Nulling it would leave windowRunGameTick dereferencing NULL the
 * instant the next round flips inLobby false. Remote players carry no
 * tanks[] entry — they render from cs->sim.plyrs + the interp context
 * (client_snapshot.c's other-player branch) — so their stale positions
 * are cleared there instead.
 *
 * Subsystem rebuilds use the same set and order as
 * serverSimResetGameWorld; keep them in step. */
void clientSimResetWorld(ClientSim *cs) {
  BYTE i;
  GameSim *gs;
  if (cs == NULL) return;
  gs = clientSimGetGameSim(cs);

  /* Clear remote players' render state — frozen tank/LGM positions in
   * the players struct plus the interpolation history feeding them. Any
   * stray remote tanks[] object (defensive; the snapshot path doesn't
   * create them) is torn down too. The local slot is skipped so MY_TANK
   * stays valid. */
  for (i = 0; i < MAX_TANKS; i++) {
    if (i == cs->myPlayerNum) continue;
    if (gs->tanks[i] != NULL) {
      tankDestroy(gs, &gs->tanks[i]);
      gs->tanks[i] = NULL;
    }
    if (gs->lgmen[i] != NULL) {
      lgmDestroy(&gs->lgmen[i]);
      gs->lgmen[i] = NULL;
    }
    playersResetRoundState(&gs->plyrs, i);
  }
  interpCreate(&cs->interpCtx, cs->myPlayerNum);

  shellsDestroy(&gs->shs);
  gs->shs = shellsCreate();

  explosionsDestroy(&gs->expl);
  explosionsCreate(&gs->expl);

  minesDestroy(&gs->mns);
  minesCreate(&gs->mns, gs->hiddenMines);

  minesExpDestroy(&gs->minesExplosions);
  minesExpCreate(&gs->minesExplosions);

  rubbleDestroy(&gs->rbl);
  rubbleCreate(&gs->rbl);

  buildingDestroy(&gs->blds);
  buildingCreate(&gs->blds);

  grassDestroy(&gs->grs);
  grassCreate(&gs->grs);

  swampDestroy(&gs->swp);
  swampCreate(&gs->swp);

  floodDestroy(&gs->ff);
  floodCreate(&gs->ff);

  tkExplosionDestroy(&gs->tankExplosions);
  tkExplosionCreate(&gs->tankExplosions);
  gs->tkExpUpdateTime = 0;

  treeGrowReset(gs);

  overviewMapReset(&cs->overview);

  /* Client-only round-scoped render/predict state (interp reset above
   * alongside the per-player clear). */
  cs->serverShellCount = 0;
  cs->predictedShellCount = 0;
  cs->projectedShellCount = 0;
  cs->projectionPingMs = 0;
  /* Base-death prediction is stamped in input ticks, which restart with the
   * round; a stamp carried over would compare against the wrong clock. */
  memset(gs->basePredictedDeadTick, 0, sizeof(gs->basePredictedDeadTick));
  memset(gs->basePredictedHitTick, 0, sizeof(gs->basePredictedHitTick));
  gs->replayTick = 0;
  /* Rows render "---" until the first snapshot of the new round lands,
   * rather than a smoothed value carried over from the last one. */
  memset(cs->displayPing, 0, sizeof(cs->displayPing));

  /* Reconciliation stats are predict-scoped — start each game fresh. */
  cs->reconCountThisWindow = 0;
  cs->reconErrSumPx = 0.0f;
  cs->reconErrMaxPx = 0.0f;
  cs->reconWindowStartTick = 0;
  cs->reconCountLastSec = 0;
  cs->reconErrAvgPxLast = 0.0f;
  cs->reconErrMaxPxLast = 0.0f;

  /* Render-only error offset is predict-scoped too — start fresh. */
  cs->errX = 0.0f;
  cs->errY = 0.0f;
  cs->errAngle = 0.0f;
  cs->basePassableSmoothSnapshots = 0;

  /* The server resets every client's view state on the round reset too, so
   * report ours again once the next round is running, and start the decay
   * clocks over — where the player drove last round earns them nothing in
   * this one. */
  viewportSetTankView(clientSimViewportMut(cs));
  clientSimResetViewStateReport(cs);
  clientSimResetViewDecay(cs);
}

bool installCompressedMap(ClientSim *cs, const BYTE *buf, int len, const char *name,
                          bool initViewport) {
  GameSim *gs;
  if (cs == NULL || buf == NULL || len <= 0) return false;

  gs = clientSimGetGameSim(cs);
  if (!mapLoadCompressedMap(&gs->mp, &gs->pb, &gs->bs, &gs->ss,
                            (BYTE *)buf, len)) {
    return false;
  }

  /* First map load (initViewport): clientSimCreate deliberately skips
   * viewport init (see comment above on clientSimCreate); the map install
   * path owns it, allocating vp->view/vp->mineView and parking the camera
   * at the map origin. A mid-game resync passes initViewport=false: those
   * buffers already exist from the first install and the player's camera
   * must survive the terrain swap, so the viewport is left untouched. The
   * clientSimUpdateView(cs, redraw) below recalculates and redraws the new
   * terrain either way, so the swapped-in map still renders in place. */
  if (initViewport) {
    viewportInit(clientSimViewportMut(cs));
    /* viewportInit parks the camera back on the tank; say so to the server
     * once the next display tick runs. The decay clocks go with it: the items
     * they were counting for belong to the map being replaced. */
    clientSimResetViewStateReport(cs);
    clientSimResetViewDecay(cs);
    /* New map, so nothing seen on the old one still means anything. The
     * next overview tick seeds the memory from the map just installed —
     * the whole map dimmed, live regions bright over it. The resync path
     * keeps the memory instead: same map, and the player has not stopped
     * having been where they have been. */
    overviewMapReset(&cs->overview);
    cs->overviewSeedPending = TRUE;
  }

  {
    char *mapNameMut = clientSimGetMapNameMutable(cs);
    if (name != NULL) {
      strncpy(mapNameMut, name, MAP_STR_SIZE - 1);
      mapNameMut[MAP_STR_SIZE - 1] = '\0';
    }
    /* name == NULL: caller doesn't know the map name; leave whatever
     * CTRL_LOBBY_SETTINGS has populated (or empty, until it arrives). */
  }

  clientSimUpdateView(cs, redraw);
  basesClearMines(gs);
  return true;
}

void clientSimSetCurrentBuildSelect(ClientSim *cs, buildSelect v) {
  if (v != BsTrees && v != BsRoad && v != BsBuilding && v != BsPillbox && v != BsMine) {
    return;
  }
  cs->currentBuildSelect = v;
}

void clientSimSetPendingBuild(ClientSim *cs, BYTE action, BYTE x, BYTE y) {
  cs->pendingBuildAction = action;
  cs->pendingBuildX = x;
  cs->pendingBuildY = y;
}

void clientSimSetBrainLastAssistMsg(ClientSim *cs, uint8_t v) { cs->brainLastAssistMsg = v; }
void clientSimSetBrainEventCount(ClientSim *cs, int v)        { cs->brainEventCount = v; }
void clientSimSetLabelMessage(ClientSim *cs, labelLen v)      { cs->labelMessage = v; }
void clientSimSetLabelTankLabel(ClientSim *cs, labelLen v)    { cs->labelTankLabel = v; }
void clientSimSetLabelOwnTank(ClientSim *cs, bool v)          { cs->labelOwnTank = v; }
void clientSimSetMyLastPlayerName(ClientSim *cs, const char *name) {
  strcpy(cs->myLastPlayerName, name);
}

void clientSimSetInLobby(ClientSim *cs, bool v)             { cs->inLobby = v; }
void clientSimSetMapDownloadComplete(ClientSim *cs, bool v) { cs->mapDownloadComplete = v; }

void clientSimSetServerAddress(ClientSim *cs, struct in_addr v) { cs->serverAddress = v; }
void clientSimSetServerPort(ClientSim *cs, unsigned short v)    { cs->serverPort = v; }
void clientSimSetIsBot(ClientSim *cs, bool v)                   { cs->isBot = v; }

bool clientSimIsSinglePlayer(const ClientSim *cs)               { return cs->isSinglePlayer; }
bool clientSimIsUdpTransport(const ClientSim *cs)               { return cs && cs->isUdpTransport; }
bool clientSimTransportTicksServer(const ClientSim *cs)         { return cs && cs->hasTransport && cs->transportTicksServer; }
bool clientSimIsLanOnly(const ClientSim *cs)                    { return cs->isLanOnly; }
void clientSimSetIsSinglePlayer(ClientSim *cs, bool v)          { cs->isSinglePlayer = v; }
void clientSimSetIsLanOnly(ClientSim *cs, bool v)               { cs->isLanOnly = v; }

bool     clientSimGetLobbyOpenHost(const ClientSim *cs)              { return cs->lobbyOpenHost; }
BYTE     clientSimGetLobbyHostSlot(const ClientSim *cs)             { return cs ? cs->lobbyHostSlot : 0; }
bool     clientSimGetLobbyAutoLockOnGameStart(const ClientSim *cs)   { return cs->lobbyAutoLockOnGameStart; }
bool     clientSimGetLobbyRanked(const ClientSim *cs)                { return cs ? cs->lobbyRanked : false; }
bool     clientSimGetLobbyAllowNewPlayers(const ClientSim *cs)       { return cs ? cs->lobbyAllowNewPlayers : true; }
bool     clientSimGetLobbyWbnAvailable(const ClientSim *cs)          { return cs ? cs->lobbyWbnAvailable : false; }
uint16_t clientSimGetLobbyServerLocks(const ClientSim *cs)           { return cs->lobbyServerLocks; }
UploadPolicy clientSimGetUploadPolicy(const ClientSim *cs)           { return cs ? cs->uploadPolicy : UPLOAD_POLICY_ALLOW; }

ViewPolicy clientSimGetViewPolicy(const ClientSim *cs, ViewCategory cat) {
  if (cs == NULL || (int)cat < 0 || (int)cat >= VIEW_CATEGORY_COUNT) {
    return viewPolicyAlways;
  }
  return cs->viewPolicy[cat];
}

bool clientSimGetClassicMode(const ClientSim *cs) {
  return cs ? cs->classicMode : false;
}

bool clientSimGetAlliesInTrees(const ClientSim *cs) {
  return cs ? cs->alliesInTrees : false;
}

ServerVoiceMode clientSimGetServerVoiceMode(const ClientSim *cs) {
  return cs ? cs->serverVoiceMode : serverVoiceOn;
}

uint16_t clientSimGetViewDecaySecs(const ClientSim *cs, ViewCategory cat) {
  if (cs == NULL || (int)cat < 0 || (int)cat >= VIEW_CATEGORY_COUNT) {
    return 0;
  }
  return cs->viewDecaySecs[cat];
}

uint8_t clientSimGetLobbyTeamInUse(const ClientSim *cs, BYTE teamId) {
  if (teamId >= 16) return 0;
  return cs->lobbyTeamInUse[teamId];
}
uint8_t clientSimGetLobbyTeamColor(const ClientSim *cs, BYTE teamId) {
  if (teamId >= 16) return 0;
  return cs->lobbyTeamColor[teamId];
}
uint8_t clientSimGetLobbyTeamPool(const ClientSim *cs, BYTE teamId) {
  if (teamId >= 16) return 0;
  return cs->lobbyTeamPool[teamId];
}
uint8_t clientSimGetLobbyTeamStartSide(const ClientSim *cs, BYTE teamId) {
  if (teamId >= 16) return 0;
  return cs->lobbyTeamStartSide[teamId];
}
const char *clientSimGetLobbyTeamName(const ClientSim *cs, BYTE teamId) {
  if (teamId >= 16) return "";
  return cs->lobbyTeamName[teamId];
}

uint8_t clientSimGetLobbyBotMode(const ClientSim *cs, BYTE slot) {
  if (slot >= 16) return 0;
  return cs->lobbyBotMode[slot];
}
uint8_t clientSimGetLobbyBotDifficulty(const ClientSim *cs, BYTE slot) {
  if (slot >= 16) return 0;
  return cs->lobbyBotDifficulty[slot];
}
uint8_t clientSimGetLobbyBotPersonality(const ClientSim *cs, BYTE slot) {
  if (slot >= 16) return 0;
  return cs->lobbyBotPersonality[slot];
}
uint8_t clientSimGetLobbyBotBrain(const ClientSim *cs, BYTE slot) {
  if (slot >= 16) return 0xFF;
  return cs->lobbyBotBrainIdx[slot];
}

const BrainList *clientSimGetLobbyBrainList(const ClientSim *cs) {
  return &cs->lobbyBrainList;
}

const RoundStatsSummary *clientSimGetLastRoundStats(const ClientSim *cs) {
  return cs->lastRoundStatsValid ? &cs->lastRoundStats : NULL;
}

uint32_t clientSimGetRatingPostedSeq(const ClientSim *cs) {
  if (cs == NULL) return 0;
  return cs->ratingPostedSeq;
}

const char *clientSimGetLobbyMapListPath(const ClientSim *cs) {
  return cs->lobbyMapListPath;
}
int clientSimGetLobbyMapListCount(const ClientSim *cs) {
  return cs->lobbyMapListCount;
}
const char *clientSimGetLobbyMapListName(const ClientSim *cs, int idx) {
  if (idx < 0 || idx >= cs->lobbyMapListCount) return "";
  return cs->lobbyMapListNames[idx];
}
bool clientSimGetLobbyMapListIsFolder(const ClientSim *cs, int idx) {
  if (idx < 0 || idx >= cs->lobbyMapListCount) return false;
  return cs->lobbyMapListIsFolder[idx] != 0;
}
int64_t clientSimGetLobbyMapListModTime(const ClientSim *cs, int idx) {
  if (idx < 0 || idx >= cs->lobbyMapListCount) return 0;
  return cs->lobbyMapListModTime[idx];
}
bool clientSimGetLobbyMapListReady(const ClientSim *cs) {
  return cs->lobbyMapListReady;
}
const char *clientSimGetLobbyMapListReqPath(const ClientSim *cs) {
  return cs->lobbyMapListReqPath;
}
bool clientSimGetLobbyMapListInFlight(const ClientSim *cs) {
  return cs->lobbyMapListInFlight;
}
uint32_t clientSimGetLobbyMapChangeSeq(const ClientSim *cs) {
  return cs ? cs->lobbyMapChangeSeq : 0;
}

const char *clientSimGetLobbyMapSearchPath(const ClientSim *cs) {
  return cs->lobbyMapSearchPath;
}
const char *clientSimGetLobbyMapSearchQuery(const ClientSim *cs) {
  return cs->lobbyMapSearchQuery;
}
int clientSimGetLobbyMapSearchCount(const ClientSim *cs) {
  return cs->lobbyMapSearchCount;
}
const char *clientSimGetLobbyMapSearchName(const ClientSim *cs, int idx) {
  if (idx < 0 || idx >= cs->lobbyMapSearchCount) return "";
  return cs->lobbyMapSearchNames[idx];
}
bool clientSimGetLobbyMapSearchIsFolder(const ClientSim *cs, int idx) {
  if (idx < 0 || idx >= cs->lobbyMapSearchCount) return false;
  return cs->lobbyMapSearchIsFolder[idx] != 0;
}
int64_t clientSimGetLobbyMapSearchModTime(const ClientSim *cs, int idx) {
  if (idx < 0 || idx >= cs->lobbyMapSearchCount) return 0;
  return cs->lobbyMapSearchModTime[idx];
}
bool clientSimGetLobbyMapSearchReady(const ClientSim *cs) {
  return cs->lobbyMapSearchReady;
}
const char *clientSimGetLobbyMapSearchReqPath(const ClientSim *cs) {
  return cs->lobbyMapSearchReqPath;
}
const char *clientSimGetLobbyMapSearchReqQuery(const ClientSim *cs) {
  return cs->lobbyMapSearchReqQuery;
}
bool clientSimGetLobbyMapSearchInFlight(const ClientSim *cs) {
  return cs->lobbyMapSearchInFlight;
}

bool clientSimGetLobbyMapPreviewReady(const ClientSim *cs) {
  return cs && cs->lobbyMapPreviewReady;
}
bool clientSimGetLobbyMapPreviewInFlight(const ClientSim *cs) {
  return cs && cs->lobbyMapPreviewInFlight;
}
bool clientSimGetLobbyMapPreviewError(const ClientSim *cs) {
  return cs && cs->lobbyMapPreviewError;
}
const char *clientSimGetLobbyMapPreviewReqPath(const ClientSim *cs) {
  return cs ? cs->lobbyMapPreviewReqPath : "";
}
const char *clientSimGetLobbyMapPreviewPath(const ClientSim *cs) {
  return cs ? cs->lobbyMapPreviewPath : "";
}
const uint8_t *clientSimGetLobbyMapPreviewBytes(const ClientSim *cs) {
  return cs ? cs->lobbyMapPreviewBytes : NULL;
}
uint32_t clientSimGetLobbyMapPreviewLen(const ClientSim *cs) {
  return cs ? cs->lobbyMapPreviewReceived : 0;
}
void clientSimClearLobbyMapPreview(ClientSim *cs) {
  if (!cs) return;
  cs->lobbyMapPreviewReqPath[0] = '\0';
  cs->lobbyMapPreviewPath[0]    = '\0';
  cs->lobbyMapPreviewInFlight   = false;
  cs->lobbyMapPreviewReady      = false;
  cs->lobbyMapPreviewError      = false;
  cs->lobbyMapPreviewTotal      = 0;
  cs->lobbyMapPreviewReceived   = 0;
}

/* ---- Spectator feed: capture (transport-facing) + drain (session-facing) ---- */

void clientSimSpectatorPushSeed(ClientSim *cs, uint8_t *blob, uint32_t len) {
  if (!cs) { free(blob); return; }
  if (cs->spectatorFeed.seedBlob != NULL) {
    free(cs->spectatorFeed.seedBlob);   /* one seed per session — replace */
  }
  cs->spectatorFeed.seedBlob  = blob;   /* ownership transferred in */
  cs->spectatorFeed.seedLen   = len;
  cs->spectatorFeed.seedReady = true;
}

bool clientSimSpectatorPushRecord(ClientSim *cs, bool isKeyframe,
                                  uint32_t gameTick, uint32_t segment,
                                  const uint8_t *payload, uint32_t payloadLen) {
  ClientSpecRecordNode *node;
  if (!cs) return false;
  node = (ClientSpecRecordNode *)calloc(1, sizeof(*node));
  if (node == NULL) return false;
  if (payloadLen > 0) {
    node->payload = (uint8_t *)malloc(payloadLen);
    if (node->payload == NULL) { free(node); return false; }
    memcpy(node->payload, payload, payloadLen);
  }
  node->isKeyframe = isKeyframe;
  node->gameTick   = gameTick;
  node->segment    = segment;
  node->payloadLen = payloadLen;
  node->next       = NULL;
  if (cs->spectatorFeed.recordTail != NULL) {
    cs->spectatorFeed.recordTail->next = node;
  } else {
    cs->spectatorFeed.recordHead = node;
  }
  cs->spectatorFeed.recordTail = node;
  cs->spectatorFeed.recordCount++;
  return true;
}

void clientSimSpectatorFeedClear(ClientSim *cs) {
  ClientSpecRecordNode *n;
  if (!cs) return;
  if (cs->spectatorFeed.seedBlob != NULL) {
    free(cs->spectatorFeed.seedBlob);
  }
  n = cs->spectatorFeed.recordHead;
  while (n != NULL) {
    ClientSpecRecordNode *next = n->next;
    free(n->payload);
    free(n);
    n = next;
  }
  memset(&cs->spectatorFeed, 0, sizeof(cs->spectatorFeed));
}

bool clientSimSpectatorSeedReady(const ClientSim *cs) {
  return cs && cs->spectatorFeed.seedReady;
}

bool clientSimSpectatorTakeSeed(ClientSim *cs, uint8_t **outBlob,
                                uint32_t *outLen) {
  if (!cs || !cs->spectatorFeed.seedReady || cs->spectatorFeed.seedBlob == NULL) {
    return false;
  }
  if (outBlob != NULL) *outBlob = cs->spectatorFeed.seedBlob;
  if (outLen  != NULL) *outLen  = cs->spectatorFeed.seedLen;
  cs->spectatorFeed.seedBlob  = NULL;   /* ownership transferred out */
  cs->spectatorFeed.seedLen   = 0;
  cs->spectatorFeed.seedReady = false;
  return true;
}

uint32_t clientSimSpectatorRecordCount(const ClientSim *cs) {
  return cs ? cs->spectatorFeed.recordCount : 0;
}

void clientSimSpectatorSetCountdown(ClientSim *cs, uint32_t remainingTicks) {
  if (cs == NULL) return;
  cs->spectatorFeed.countdownRemaining = remainingTicks;
  cs->spectatorFeed.countdownReceived  = true;
}

bool clientSimSpectatorCountdown(const ClientSim *cs, uint32_t *outRemaining) {
  if (cs == NULL || !cs->spectatorFeed.countdownReceived) return false;
  if (outRemaining != NULL) *outRemaining = cs->spectatorFeed.countdownRemaining;
  return true;
}

void clientSimSpectatorSetLiveLobby(ClientSim *cs, bool liveLobby) {
  if (cs == NULL) return;
  cs->spectatorFeed.liveLobby = liveLobby;
}

bool clientSimSpectatorIsLiveLobby(const ClientSim *cs) {
  return cs != NULL && cs->spectatorFeed.liveLobby;
}

bool clientSimSpectatorPopRecord(ClientSim *cs, ClientSpectatorRecord *out) {
  ClientSpecRecordNode *node;
  if (!cs || out == NULL) return false;
  node = cs->spectatorFeed.recordHead;
  if (node == NULL) return false;
  cs->spectatorFeed.recordHead = node->next;
  if (cs->spectatorFeed.recordHead == NULL) {
    cs->spectatorFeed.recordTail = NULL;
  }
  cs->spectatorFeed.recordCount--;
  out->isKeyframe = node->isKeyframe;
  out->gameTick   = node->gameTick;
  out->segment    = node->segment;
  out->payload    = node->payload;     /* ownership transferred to caller */
  out->payloadLen = node->payloadLen;
  free(node);                          /* node only — payload is the caller's now */
  return true;
}

/* spectator_drain.h seam: the logviewer-world host pulls the captured seed and
 * forward records through these void *-handle wrappers because it cannot
 * include client_sim.h (screenObj redefinition vs backend.h). Each forwards to
 * the matching clientSimSpectator* accessor; ownership transfers are unchanged.
 */
void specDrainPump(void *handle) {
  clientSimNetTick((ClientSim *)handle);
}

bool specDrainCountdown(void *handle, uint32_t *outRemaining) {
  return clientSimSpectatorCountdown((const ClientSim *)handle, outRemaining);
}

bool specDrainSeedReady(void *handle) {
  return clientSimSpectatorSeedReady((const ClientSim *)handle);
}

bool specDrainTakeSeed(void *handle, uint8_t **outBlob, uint32_t *outLen) {
  return clientSimSpectatorTakeSeed((ClientSim *)handle, outBlob, outLen);
}

uint32_t specDrainRecordCount(void *handle) {
  return clientSimSpectatorRecordCount((const ClientSim *)handle);
}

bool specDrainPopRecord(void *handle, SpecDrainRecord *out) {
  ClientSpectatorRecord rec;
  if (out == NULL) return false;
  if (!clientSimSpectatorPopRecord((ClientSim *)handle, &rec)) return false;
  out->isKeyframe = rec.isKeyframe;
  out->gameTick   = rec.gameTick;
  out->segment    = rec.segment;
  out->payload    = rec.payload;       /* ownership passes straight through */
  out->payloadLen = rec.payloadLen;
  return true;
}

bool specDrainLiveResumed(void *handle) {
  return clientSimSpectatorIsLiveLobby((const ClientSim *)handle);
}

bool specSeedDecodeInfo(const uint8_t *seed, size_t seedLen, SpecSeedInfo *out) {
  BYTE *scratch;
  const BYTE *ctrl = NULL;
  int ctrlLen = 0;
  int keyframeLen;
  ControlDecodeBodyFn decSettings;
  ControlDecodeBodyFn decSlot;
  bool haveSettings = false;
  int pos;
  bool found = false;

  if (out == NULL) return false;
  memset(out, 0, sizeof(*out));
  if (seed == NULL || seedLen == 0 || seedLen >= (size_t)INT_MAX) return false;

  /* Reuse the blessed keyframe translator to slice out the control snapshot
   * (it sets ctrl/ctrlLen to the snapshot within the seed). The translated
   * body is discarded — only the control slice is wanted here. */
  scratch = (BYTE *)malloc(seedLen + 1);
  if (scratch == NULL) return false;
  keyframeLen = specReplayTranslateKeyframe(seed, (int)seedLen, scratch,
                                            (int)(seedLen + 1), &ctrl, &ctrlLen);
  if (keyframeLen < 0 || ctrl == NULL || ctrlLen <= 0) {
    free(scratch);
    return false;
  }

  /* Walk the snapshot's [u16 BE type][u16 BE bodyLen][body] records (the same
   * events a normal joiner receives, embedded in the sync-replay slice
   * serverSimSerializeControlSnapshot wrote). Three events are wanted: the
   * lobby-settings event (map/settings the synthesized header needs), the
   * lobby-slot events (one per connected player — the roster a spectator host
   * uses to name lobby-chat senders who hold no tank), and the game-phase
   * discriminant (lobby/countdown/running/game-over). All records are walked
   * so the slot events, which follow the settings event, are not missed. */
  decSettings = transportControlCodecBodyDecoder(CTRL_LOBBY_SETTINGS);
  decSlot     = transportControlCodecBodyDecoder(CTRL_LOBBY_SLOT);
  pos = 0;
  while (pos + 4 <= ctrlLen) {
    uint16_t type    = (uint16_t)(((uint16_t)ctrl[pos] << 8) | ctrl[pos + 1]);
    uint16_t bodyLen = (uint16_t)(((uint16_t)ctrl[pos + 2] << 8) | ctrl[pos + 3]);
    pos += 4;
    if ((size_t)pos + bodyLen > (size_t)ctrlLen) break;
    if (type == CTRL_LOBBY_SETTINGS && !haveSettings) {
      ControlEvent evt;
      memset(&evt, 0, sizeof(evt));
      if (decSettings != NULL && decSettings(ctrl + pos, bodyLen, &evt)) {
        /* Bound the copy by the source field (<= MAP_STR_SIZE) as well as the
         * destination, so an unterminated wire name can't over-read. */
        size_t nameCap = sizeof(evt.u.lobbySettings.mapName);
        if (nameCap > sizeof(out->mapName)) nameCap = sizeof(out->mapName);
        strncpy(out->mapName, evt.u.lobbySettings.mapName, nameCap - 1);
        out->mapName[nameCap - 1] = '\0';
        out->gameType         = (uint8_t)evt.u.lobbySettings.lobbyGameType;
        out->allowHiddenMines = evt.u.lobbySettings.lobbyHiddenMines ? 1 : 0;
        out->ai               = evt.u.lobbySettings.lobbyAiType;
        out->haveInfo         = true;
        found = true;
      }
      haveSettings = true;   /* one lobby-settings event per snapshot */
    } else if (type == CTRL_GAME_PHASE_LOBBY) {
      out->specPhase = SPEC_PHASE_LOBBY;
    } else if (type == CTRL_GAME_PHASE_COUNTDOWN) {
      out->specPhase = SPEC_PHASE_COUNTDOWN;
    } else if (type == CTRL_GAME_PHASE_RUNNING) {
      out->specPhase = SPEC_PHASE_RUNNING;
    } else if (type == CTRL_GAME_PHASE_GAME_OVER) {
      out->specPhase = SPEC_PHASE_GAMEOVER;
    } else if (type == CTRL_LOBBY_SLOT) {
      ControlEvent evt;
      memset(&evt, 0, sizeof(evt));
      if (decSlot != NULL && decSlot(ctrl + pos, bodyLen, &evt)) {
        BYTE pn = evt.u.lobbySlot.playerNum;
        if (evt.u.lobbySlot.slot.connected && pn < MAX_TANKS) {
          out->lobbyPresent[pn] = true;
          /* Source playerName is PACKET_MAX_PLAYER_NAME (64); lobbyName[pn] is
           * the same width. Copy bounded by the destination and NUL-terminate
           * so an unterminated wire name can't over-read. */
          strncpy(out->lobbyName[pn], evt.u.lobbySlot.slot.playerName,
                  sizeof(out->lobbyName[pn]) - 1);
          out->lobbyName[pn][sizeof(out->lobbyName[pn]) - 1] = '\0';
        }
      }
    }
    pos += bodyLen;
  }

  free(scratch);
  return found;
}

uint8_t  clientSimGetLobbyMapUploadStatus(const ClientSim *cs)     { return cs->lobbyMapUploadStatus; }
uint8_t  clientSimGetLobbyMapUploadRejectCode(const ClientSim *cs) { return cs->lobbyMapUploadRejectCode; }
const char *clientSimGetLobbyMapUploadFinalPath(const ClientSim *cs){ return cs->lobbyMapUploadFinalPath; }

bool clientSimConsumeUseLocalFallback(ClientSim *cs) {
  if (!cs || !cs->lobbyMapUseLocalNeedsFallback) return false;
  cs->lobbyMapUseLocalNeedsFallback = false;
  return true;
}

uint8_t clientSimGetLobbyWbnPreviewStatus(const ClientSim *cs) {
  return cs ? cs->lobbyWbnPreviewStatus : 0;
}
const char *clientSimGetLobbyWbnPreviewErrMsg(const ClientSim *cs) {
  return cs ? cs->lobbyWbnPreviewErrMsg : "";
}
void clientSimSetLobbyWbnPreviewStatus(ClientSim *cs, uint8_t status) {
  if (!cs) return;
  cs->lobbyWbnPreviewStatus = status;
  if (status != 3) cs->lobbyWbnPreviewErrMsg[0] = '\0';
}
void clientSimSetLobbyWbnPreviewErrMsg(ClientSim *cs, const char *msg) {
  if (!cs) return;
  if (!msg) msg = "";
  SDL_strlcpy(cs->lobbyWbnPreviewErrMsg, msg,
              sizeof(cs->lobbyWbnPreviewErrMsg));
}

uint8_t clientSimGetLobbyLastRejectPacket(const ClientSim *cs)  { return cs->lobbyLastRejectPacket; }
uint8_t clientSimGetLobbyLastRejectReason(const ClientSim *cs)  { return cs->lobbyLastRejectReason; }
void    clientSimClearLobbyLastReject(ClientSim *cs) {
  cs->lobbyLastRejectPacket = 0;
  cs->lobbyLastRejectReason = 0;
}

/* View-control composition helpers. */
void clientSimTankView(ClientSim *cs) {
  viewportFollowTank(clientSimViewportMut(cs), clientSimGetScroll(cs), MY_TANK(cs));
}

void clientSimCenterTank(ClientSim *cs) {
  viewportCenterOnTank(clientSimViewportMut(cs), clientSimGetScroll(cs), MY_TANK(cs));
}

void clientSimSetAutoScroll(ClientSim *cs, bool isAuto) {
  scrollSetScrollType(clientSimGetScroll(cs), isAuto);
}

int clientSimGetScrollMechanism(void) {
  return (int)scrollGetMechanism();
}

void clientSimSetScrollMechanism(int mech) {
  scrollSetMechanism((ScrollMechanism)mech);
}

void clientSimSetAutoScrollOverride(ClientSim *cs, bool value) {
  cs->scroll.autoScrollOverRide = value;
}

bool clientSimGetMyTankMapPos(ClientSim *cs, BYTE *mapX, BYTE *mapY) {
  if (!clientSimIsMyTankAlive(cs)) return false;
  if (mapX) *mapX = tankGetMX(&MY_TANK(cs));
  if (mapY) *mapY = tankGetMY(&MY_TANK(cs));
  return true;
}

bool clientSimGetMyTankMapPosF(ClientSim *cs, float *mapX, float *mapY) {
  WORLD wx = 0, wy = 0;
  if (!clientSimIsMyTankAlive(cs)) return false;
  tankGetWorld(&MY_TANK(cs), &wx, &wy);
  if (mapX) *mapX = (float)wx / (float)(1 << TANK_SHIFT_MAPSIZE);
  if (mapY) *mapY = (float)wy / (float)(1 << TANK_SHIFT_MAPSIZE);
  return true;
}

bool clientSimIsMyTankAlive(const ClientSim *cs) {
  if (!cs || MY_TANK((ClientSim *)cs) == NULL) return false;
  /* The destroyed state the tank carries, the same test viewportCenterOnTank
   * makes before it re-centres the main view. */
  return !tankIsDestroyed(&MY_TANK((ClientSim *)cs));
}

void clientSimBuildShellList(ClientSim *cs, screenBullets *sb,
                             int left, int rightExcl,
                             int top, int bottomExcl,
                             int originX, int originY) {
  int si;              /* Looping variable */
  BYTE myPlayer = cs->interpCtx.localPlayer;

  /* Other players' shells. Humans draw the forward-projected layer so
   * incoming shells appear at their true present position rather than
   * ~RTT/2 in the past; bots (e.g. BrainTest overlay) draw the raw
   * serverShellSnaps the brain perceives. Own shells are filtered during
   * snapshot sync, so neither source contains them. */
  if (cs->isBot) {
    for (si = 0; si < clientSimGetServerShellCount(cs); si++) {
      const ShellSnapshot *ss = &clientSimGetServerShellSnaps(cs)[si];
      BYTE smx = (BYTE)(ss->worldX >> TANK_SHIFT_MAPSIZE);
      BYTE smy = (BYTE)(ss->worldY >> TANK_SHIFT_MAPSIZE);
      if (ss->owner == myPlayer) {
        continue;
      }
      if (smx >= left && smx < rightExcl && smy >= top && smy < bottomExcl) {
        WORLD conv;
        BYTE spx, spy, swx, swy, sframe;
        swx = (BYTE)ss->worldX;
        swy = (BYTE)ss->worldY;
        conv = ss->worldX;
        conv <<= TANK_SHIFT_MAPSIZE;
        conv >>= TANK_SHIFT_PIXELSIZE;
        spx = (BYTE)conv;
        conv = ss->worldY;
        conv <<= TANK_SHIFT_MAPSIZE;
        conv >>= TANK_SHIFT_PIXELSIZE;
        spy = (BYTE)conv;
        sframe = (BYTE)(utilGetDir((TURNTYPE)ss->angle) + SHELL_START_EXPLODE + 1);
        screenBulletsAddItem(sb, (BYTE)(smx - originX), (BYTE)(smy - originY),
                             spx, spy, sframe, swx, swy);
      }
    }
  } else {
    for (si = 0; si < clientSimGetProjectedShellCount(cs); si++) {
      const ProjectedShell *ps = &clientSimGetProjectedShells(cs)[si];
      WORLD sx = (WORLD)(int)ps->fx;
      WORLD sy = (WORLD)(int)ps->fy;
      BYTE smx = (BYTE)(sx >> TANK_SHIFT_MAPSIZE);
      BYTE smy = (BYTE)(sy >> TANK_SHIFT_MAPSIZE);
      /* Ours come from the predicted array below. */
      if (ps->owner == myPlayer) {
        continue;
      }
      if (smx >= left && smx < rightExcl && smy >= top && smy < bottomExcl) {
        WORLD conv;
        BYTE spx, spy, sframe;
        conv = sx;
        conv <<= TANK_SHIFT_MAPSIZE;
        conv >>= TANK_SHIFT_PIXELSIZE;
        spx = (BYTE)conv;
        conv = sy;
        conv <<= TANK_SHIFT_MAPSIZE;
        conv >>= TANK_SHIFT_PIXELSIZE;
        spy = (BYTE)conv;
        sframe = (BYTE)(utilGetDir((TURNTYPE)ps->angle) + SHELL_START_EXPLODE + 1);
        screenBulletsAddItem(sb, (BYTE)(smx - originX), (BYTE)(smy - originY),
                             spx, spy, sframe, (BYTE)sx, (BYTE)sy);
      }
    }
  }

  /* Client-predicted shells (local player only) */
  for (si = 0; si < clientSimGetPredictedShellCount(cs); si++) {
    const PredictedShell *ps = &clientSimGetPredictedShells(cs)[si];
    BYTE pmx, pmy;
    /* An expired shell would draw a ghost frame beside its own explosion. */
    if (ps->length <= SHELL_DEATH) {
      continue;
    }
    pmx = (BYTE)(ps->x >> TANK_SHIFT_MAPSIZE);
    pmy = (BYTE)(ps->y >> TANK_SHIFT_MAPSIZE);
    if (pmx >= left && pmx < rightExcl && pmy >= top && pmy < bottomExcl) {
      WORLD conv;
      BYTE ppx, ppy, pframe;
      conv = ps->x;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv >>= TANK_SHIFT_PIXELSIZE;
      ppx = (BYTE)conv;
      conv = ps->y;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv >>= TANK_SHIFT_PIXELSIZE;
      ppy = (BYTE)conv;
      pframe = (BYTE)(utilGetDir(ps->angle) + SHELL_START_EXPLODE + 1);
      screenBulletsAddItem(sb, (BYTE)(pmx - originX), (BYTE)(pmy - originY),
                           ppx, ppy, pframe, (BYTE)ps->x, (BYTE)ps->y);
    }
  }
}

void clientSimPrepareOverviewEntities(ClientSim *cs, screenTanks *tks,
                                      screenLgm *lgms, screenBullets *sb) {
  GameSim *gs; /* The client's own sim, source of the shell and explosion lists */

  /* Nothing to walk the map for without a local tank: screenTanksPrepare
   * dereferences it, and the two other builders are of no use on their own.
   * The caller's lists are left as it created them — empty. */
  if (cs == NULL || MY_TANK(cs) == NULL) {
    return;
  }

  /* One rect over the whole map. With leftPos and top at 0 the relative BYTE
   * positions the builders write out are the absolute map squares, which is
   * what lets the caller test each entity against the square it stands on.
   * The last row and column drop out of the LGM and explosion walks, whose
   * far bound is exclusive, and nothing can ever be there: tanks and LGM
   * destinations are held inside the mine border, and nothing a shell can
   * reach and blow up on is further out. */
  if (tks != NULL) {
    screenTanksPrepare(cs, tks, &MY_TANK(cs), 0, MAP_ARRAY_LAST, 0,
                       MAP_ARRAY_LAST);
  }
  if (lgms != NULL) {
    screenLgmPrepare(cs, lgms, 0, MAP_ARRAY_LAST, 0, MAP_ARRAY_LAST);
  }
  if (sb != NULL) {
    gs = clientSimGetGameSim(cs);

    /* Shells do not come from gs->shs. A client's shs list is not where the
     * shells it can see live: other players' arrive as the forward-projected
     * layer and the local player's own as client-side predictions, which is
     * why the main view walks those two arrays rather than calling
     * shellsCalcScreenBullets. Same list builder here, over a rect every
     * square passes — the caller wants the whole map and filters per square —
     * and with no origin to subtract, so the squares stay absolute like the
     * rest of what this fills in. */
    clientSimBuildShellList(cs, sb, 0, MAP_ARRAY_SIZE, 0, MAP_ARRAY_SIZE, 0, 0);

    explosionsCalcScreenBullets(&gs->expl, sb, 0, MAP_ARRAY_LAST, 0,
                                MAP_ARRAY_LAST);
    tkExplosionCalcScreenBullets(&gs->tankExplosions, sb, 0, MAP_ARRAY_LAST, 0,
                                 MAP_ARRAY_LAST);
  }
}

bool clientSimGetGunsightTile(ClientSim *cs, BYTE *mapX, BYTE *mapY) {
  if (!cs || MY_TANK(cs) == NULL) return false;
  BYTE px, py;
  tankGetGunsight(&MY_TANK(cs), mapX, mapY, &px, &py);
  return true;
}

bool clientSimGetGunsightPos(ClientSim *cs, BYTE *mapX, BYTE *mapY,
                             BYTE *pixelX, BYTE *pixelY) {
  if (!cs || MY_TANK(cs) == NULL) return false;
  if (!clientSimIsMyTankAlive(cs)) return false;
  if (!tankIsGunsightShow(&MY_TANK(cs))) return false;
  /* tankGetGunsight reads the raw sim pose, not the render-smoothed one the
   * main view feeds tankGetGunsightAt. That view also redraws the own hull at
   * the smoothed pose, so the two agree there; a caller that draws the tank
   * straight from screenTanksPrepare has no such fixup, and smoothing only the
   * crosshair would put it out of step with the tank sprite beside it. */
  tankGetGunsight(&MY_TANK(cs), mapX, mapY, pixelX, pixelY);
  return true;
}

/* --- The map overview's render snapshot --- */

struct OverviewSnapshot {
  /* The memory, copied only when the sim's generation has moved on from the
   * one this copy was taken at. mapValid says a copy has been taken at all: a
   * fresh snapshot and a fresh memory both start at generation 0. haveMap is
   * false after a fill from no sim, which draws nothing. The counter restarts
   * at 0 on a round reset; a repeat of the stored value after that is at
   * worst one frame stale, since the next change moves it on again. */
  OverviewMap   map;
  unsigned      haveGeneration;
  bool          mapValid;
  bool          haveMap;

  /* The per-frame lists, already through the live-square filter. The two
   * linked lists are emptied and rebuilt on every fill. */
  screenTanks   tks;
  screenLgm     lgms;
  screenBullets sb;
  bool          selfDrawn;
  BYTE          myPlayerNum;

  bool          tankAlive;
  float         tankX;
  float         tankY;
  bool          blackout;

  bool          gunsightValid;
  BYTE          gsMX;
  BYTE          gsMY;
  BYTE          gsPX;
  BYTE          gsPY;

  bool          inItemView;
  uint8_t       viewKind;
  BYTE          viewTarget;
  int           pillViewX;
  int           pillViewY;

  /* Every pill and base at its square, numbered as the classic view numbers
   * them. Rebuilt on every fill; nothing is filtered here. */
  OverviewItemLabel itemLabels[MAX_PILLS + MAX_BASES];
  int               itemLabelCount;

  /* The smart pings still on their clock, oldest first, taken with the rest
   * of the frame's reads so the render half never touches the ping ring the
   * network thread writes. */
  ClientPing        pings[MAX_CLIENT_PINGS];
  int               pingCount;
};

/* Whether an entity standing on (mapX, mapY) may be drawn: only a square the
 * player can see this instant. The renderer's overviewEntityIsVisible makes
 * the same test; it is restated here so the filter can run in the sim. */
static bool overviewSquareIsLive(const OverviewMap *om, int mapX, int mapY) {
  if (mapX < 0 || mapX >= MAP_ARRAY_SIZE ||
      mapY < 0 || mapY >= MAP_ARRAY_SIZE) {
    return false;
  }
  if (om == NULL) return false;
  return (om->flags[mapX][mapY] & OVERVIEW_F_LIVE) != 0;
}

/* Copies the entries the player is allowed to see into a second set of lists.
 * The builders work over the whole map — wider than anything the server culls
 * to — so this is where sight is enforced: an entity is kept only when the
 * square it stands on is live, the local player's own tank included. Whether
 * that tank survived the test is reported back through outSelfDrawn, so the
 * reticle can follow it. Copying into fresh lists rather than editing the
 * built ones leaves the sim's per-frame views untouched. */
static void overviewViewFilterEntities(const OverviewMap *om, BYTE me,
                                       bool selfAlive,
                                       const screenTanks *allTks,
                                       const screenLgm *allLgms,
                                       const screenBullets *allSb,
                                       screenTanks *outTks, screenLgm *outLgms,
                                       screenBullets *outSb,
                                       bool *outSelfDrawn) {
  BYTE mx, my, px, py, frame, playerNum;
  BYTE wx, wy, angle;
  char name[PLAYER_NAME_LEN];
  BYTE count;
  BYTE total;
  int bulletTotal;
  int bullet;

  *outSelfDrawn = false;

  total = screenTanksGetNumEntries(allTks);
  for (count = 1; count <= total; count++) {
    screenTanksGetItem(allTks, count, &mx, &my, &px, &py, &frame,
                       &playerNum, name);
    bool isSelf = (playerNum == me);
    /* A tank waiting to respawn reads as sitting on the map origin, which
     * is nowhere it is, so it goes before the square it claims is tested
     * at all. This is also what takes the reticle off for the death wait:
     * outSelfDrawn stays false. */
    if (isSelf && !selfAlive) continue;
    if (!overviewSquareIsLive(om, mx, my)) continue;
    screenTanksGetSubPixel(allTks, count, &wx, &wy, &angle);
    screenTanksAddItem(outTks, mx, my, px, py, frame, playerNum, name,
                       wx, wy, angle);
    if (isSelf) *outSelfDrawn = true;
  }

  total = screenLgmGetNumEntries(allLgms);
  for (count = 1; count <= total; count++) {
    screenLgmGetItem(allLgms, count, &mx, &my, &px, &py, &frame);
    if (!overviewSquareIsLive(om, mx, my)) continue;
    screenLgmGetSubPixel(allLgms, count, &wx, &wy);
    screenLgmAddItem(outLgms, mx, my, px, py, frame, wx, wy);
  }

  /* Shells, shell explosions and tank explosions share one list and one
   * position per entry, so the square each stands on is all the filter
   * needs; what the frame draws as never enters into it. */
  bulletTotal = screenBulletsGetNumEntries(allSb);
  for (bullet = 1; bullet <= bulletTotal; bullet++) {
    screenBulletsGetItem(allSb, bullet, &mx, &my, &px, &py, &frame);
    if (!overviewSquareIsLive(om, mx, my)) continue;
    screenBulletsGetSubPixel(allSb, bullet, &wx, &wy);
    screenBulletsAddItem(outSb, mx, my, px, py, frame, wx, wy);
  }
}

OverviewSnapshot *overviewSnapshotCreate(void) {
  OverviewSnapshot *s = (OverviewSnapshot *)calloc(1, sizeof(*s));
  if (s == NULL) return NULL;
  screenTanksCreate(&s->tks);
  screenLgmCreate(&s->lgms);
  s->sb = screenBulletsCreate();
  return s;
}

void overviewSnapshotDestroy(OverviewSnapshot *s) {
  if (s == NULL) return;
  screenBulletsDestroy(&s->sb);
  screenLgmDestroy(&s->lgms);
  screenTanksDestroy(&s->tks);
  free(s);
}

void clientSimFillOverviewSnapshot(ClientSim *cs, OverviewSnapshot *s) {
  screenTanks   allTks;
  screenLgm     allLgms;
  screenBullets allSb;

  if (s == NULL) return;

  /* Empty lists and every scalar at its nothing-to-draw value first, so a
   * fill from no sim leaves what a NULL ClientSim gave the render before:
   * no map, no sprites, no marks. */
  screenTanksCreate(&s->tks);
  screenLgmDestroy(&s->lgms);
  screenLgmCreate(&s->lgms);
  screenBulletsDestroy(&s->sb);
  s->sb = screenBulletsCreate();
  s->selfDrawn     = false;
  s->myPlayerNum   = 0;
  s->tankAlive     = false;
  s->tankX         = 0.0f;
  s->tankY         = 0.0f;
  s->blackout      = false;
  s->gunsightValid = false;
  s->gsMX = s->gsMY = s->gsPX = s->gsPY = 0;
  s->inItemView    = false;
  s->viewKind      = 0;
  s->viewTarget    = 0;
  s->pillViewX     = 0;
  s->pillViewY     = 0;
  s->itemLabelCount = 0;
  s->pingCount     = 0;
  s->haveMap       = (cs != NULL);
  if (cs == NULL) return;

  /* The pings, on the clock the drawer ages them against. Sampled here rather
   * than in the render so the ring — written from the network thread — is
   * only ever read under the same lock the rest of this fill runs under. */
  s->pingCount = clientSimGetPings(cs, (uint32_t)SDL_GetTicks(), s->pings,
                                   MAX_CLIENT_PINGS);

  /* Generation 0 only exists between a round reset and the seed that follows
   * it, so a match on 0 can be a snapshot filled in that same window a round
   * earlier, still holding the map from then. Copy rather than trust it; it
   * costs one memcpy on a frame between rounds. */
  if (!s->mapValid || cs->overview.generation == 0 ||
      s->haveGeneration != cs->overview.generation) {
    memcpy(&s->map, &cs->overview, sizeof(s->map));
    s->haveGeneration = cs->overview.generation;
    s->mapValid       = true;
  }

  s->myPlayerNum = clientSimGetMyPlayerNum(cs);
  s->tankAlive   = clientSimIsMyTankAlive(cs);
  if (s->tankAlive) {
    clientSimGetMyTankMapPosF(cs, &s->tankX, &s->tankY);
  }
  s->blackout      = clientSimIsMyTankDeathBlackout(cs);
  s->gunsightValid = clientSimGetGunsightPos(cs, &s->gsMX, &s->gsMY,
                                             &s->gsPX, &s->gsPY);
  s->inItemView = clientSimIsInItemView(cs);
  s->viewKind   = clientSimGetViewKind(cs);
  s->viewTarget = clientSimGetViewTarget(cs);
  s->pillViewX  = clientSimGetPillViewX(cs);
  s->pillViewY  = clientSimGetPillViewY(cs);

  /* Every pill and base at its square. The number is the one the classic
   * view draws — pillsGetViewPillNum / basesGetBaseNum at that square, less
   * one — so two pills the sim reports on one square number as the classic
   * view numbers them. Which squares show a number is decided at draw time
   * from the memory copy above: the tile there, and whether it is live. */
  {
    GameSim *gs = clientSimGetGameSim(cs);
    BYTE n = pillsGetNumPills(&gs->pb);
    BYTE i;
    for (i = 1; i <= n && s->itemLabelCount < MAX_PILLS + MAX_BASES; i++) {
      pillbox item;
      OverviewItemLabel *l = &s->itemLabels[s->itemLabelCount++];
      memset(&item, 0, sizeof(item));
      pillsGetPill(&gs->pb, &item, i);
      l->mapX   = item.x;
      l->mapY   = item.y;
      l->number = (BYTE)(pillsGetViewPillNum(&gs->pb, item.x, item.y,
                                             FALSE, FALSE) - 1);
      l->isBase = false;
    }
    n = basesGetNumBases(&gs->bs);
    for (i = 1; i <= n && s->itemLabelCount < MAX_PILLS + MAX_BASES; i++) {
      base item;
      OverviewItemLabel *l = &s->itemLabels[s->itemLabelCount++];
      memset(&item, 0, sizeof(item));
      basesGetBase(&gs->bs, &item, i);
      l->mapX   = item.x;
      l->mapY   = item.y;
      l->number = (BYTE)(basesGetBaseNum(&gs->bs, item.x, item.y) - 1);
      l->isBase = true;
    }
  }

  /* The whole-map lists, then the filter against the copy just taken, so the
   * sprites stand on squares the same picture says are live. */
  screenTanksCreate(&allTks);
  screenLgmCreate(&allLgms);
  allSb = screenBulletsCreate();
  clientSimPrepareOverviewEntities(cs, &allTks, &allLgms, &allSb);
  overviewViewFilterEntities(&s->map, s->myPlayerNum, s->tankAlive, &allTks,
                             &allLgms, &allSb, &s->tks, &s->lgms, &s->sb,
                             &s->selfDrawn);
  screenBulletsDestroy(&allSb);
  screenLgmDestroy(&allLgms);
  screenTanksDestroy(&allTks);
}

const OverviewMap *overviewSnapshotMap(const OverviewSnapshot *s) {
  if (s == NULL || !s->haveMap) return NULL;
  return &s->map;
}

const screenTanks *overviewSnapshotTanks(const OverviewSnapshot *s) {
  return s ? &s->tks : NULL;
}

const screenLgm *overviewSnapshotLgms(const OverviewSnapshot *s) {
  return s ? &s->lgms : NULL;
}

const screenBullets *overviewSnapshotBullets(const OverviewSnapshot *s) {
  return s ? &s->sb : NULL;
}

bool overviewSnapshotSelfDrawn(const OverviewSnapshot *s) {
  return s ? s->selfDrawn : false;
}

BYTE overviewSnapshotMyPlayerNum(const OverviewSnapshot *s) {
  return s ? s->myPlayerNum : 0;
}

bool overviewSnapshotTankAlive(const OverviewSnapshot *s) {
  return s ? s->tankAlive : false;
}

bool overviewSnapshotTankPos(const OverviewSnapshot *s, float *mapX,
                             float *mapY) {
  if (s == NULL || !s->tankAlive) return false;
  if (mapX) *mapX = s->tankX;
  if (mapY) *mapY = s->tankY;
  return true;
}

bool overviewSnapshotBlackout(const OverviewSnapshot *s) {
  return s ? s->blackout : false;
}

bool overviewSnapshotGunsight(const OverviewSnapshot *s, BYTE *mapX,
                              BYTE *mapY, BYTE *pixelX, BYTE *pixelY) {
  if (s == NULL || !s->gunsightValid) return false;
  if (mapX)   *mapX   = s->gsMX;
  if (mapY)   *mapY   = s->gsMY;
  if (pixelX) *pixelX = s->gsPX;
  if (pixelY) *pixelY = s->gsPY;
  return true;
}

bool overviewSnapshotInItemView(const OverviewSnapshot *s) {
  return s ? s->inItemView : false;
}

uint8_t overviewSnapshotViewKind(const OverviewSnapshot *s) {
  return s ? s->viewKind : 0;
}

BYTE overviewSnapshotViewTarget(const OverviewSnapshot *s) {
  return s ? s->viewTarget : 0;
}

void overviewSnapshotItemViewSquare(const OverviewSnapshot *s, int *mapX,
                                    int *mapY) {
  if (mapX) *mapX = s ? s->pillViewX : 0;
  if (mapY) *mapY = s ? s->pillViewY : 0;
}

int overviewSnapshotItemLabelCount(const OverviewSnapshot *s) {
  return s ? s->itemLabelCount : 0;
}

const ClientPing *overviewSnapshotPings(const OverviewSnapshot *s) {
  return s ? &s->pings[0] : NULL;
}

int overviewSnapshotPingCount(const OverviewSnapshot *s) {
  return s ? s->pingCount : 0;
}

const OverviewItemLabel *overviewSnapshotItemLabels(const OverviewSnapshot *s) {
  return s ? s->itemLabels : NULL;
}

void clientSimShowMessages(ClientSim *cs, BYTE msgType, bool isShown) {
  switch (msgType) {
  case MSG_NEWSWIRE:
    messageSetNewswire(clientSimGetMessages(cs), isShown);
    break;
  case MSG_ASSISTANT:
    messageSetAssistant(clientSimGetMessages(cs), isShown);
    break;
  case MSG_AI:
    messageSetAI(clientSimGetMessages(cs), isShown);
    break;
  case MSG_NETSTATUS:
    messageSetNetStatus(clientSimGetMessages(cs), isShown);
    break;
  default:
    messageSetNetwork(clientSimGetMessages(cs), isShown);
    break;
  }
}

void clientSimNetStatusMessage(ClientSim *cs, char *messageStr) {
  clientMessageAdd(clientSimGetMessages(cs), networkStatus, (char *) "Network Status", messageStr);
}

void clientSimSetCursorPos(ClientSim *cs, BYTE posX, BYTE posY) {
  viewportSetCursor(clientSimViewportMut(cs), posX, posY);
}

bool clientSimGetCursorPos(ClientSim *cs, BYTE *posX, BYTE *posY) {
  return viewportGetCursor(clientSimViewport(cs), posX, posY);
}

PlayerBitMap clientSimAllyViewMask(const ClientSim *cs) {
  PlayerBitMap mask;
  BYTE playerNum;

  mask = 0;
  if (cs == NULL) {
    return mask;
  }
  for (playerNum = 0; playerNum < MAX_TANKS; playerNum++) {
    if (playerNum == cs->myPlayerNum) {
      continue;
    }
    if (interpIsAlive(&cs->interpCtx, playerNum) == TRUE) {
      mask |= (PlayerBitMap)1 << playerNum;
    }
  }
  return mask;
}

/* The one body behind clientSimPillView / clientSimBaseView /
 * clientSimAllyView. */
static void clientSimItemView(ClientSim *cs, uint8_t kind, int horz, int vert) {
  ViewCycleInputs in; /* What the cycling takes from this client */

  clientSimFillViewCycleInputs(cs, &in);
  viewportPanInView(clientSimViewportMut(cs), clientSimGetGameSim(cs),
                    clientSimGetScroll(cs), MY_TANK(cs), kind, &in, horz, vert);
}

void clientSimPillView(ClientSim *cs, int horz, int vert) {
  clientSimItemView(cs, VIEW_KIND_PILL, horz, vert);
}

void clientSimBaseView(ClientSim *cs, int horz, int vert) {
  clientSimItemView(cs, VIEW_KIND_BASE, horz, vert);
}

/* The direction byte a cycle request carries. Both zero is the plain "next"
 * the ally key sends; the scroll keys set exactly one of the four. */
static uint8_t clientSimViewCycleDirection(int horz, int vert) {
  if (horz < 0) {
    return VIEW_CYCLE_LEFT;
  }
  if (horz > 0) {
    return VIEW_CYCLE_RIGHT;
  }
  if (vert < 0) {
    return VIEW_CYCLE_UP;
  }
  if (vert > 0) {
    return VIEW_CYCLE_DOWN;
  }
  return VIEW_CYCLE_NEXT;
}

void clientSimAllyView(ClientSim *cs, int horz, int vert) {
  uint8_t from; /* The ally we are stepping away from */

  if (cs == NULL) {
    return;
  }
  if (!cs->hasTransport) {
    /* No server to ask, so pick here. Every real game has a transport, the
     * in-process host included, which leaves the tools and tests on this
     * path. */
    clientSimItemView(cs, VIEW_KIND_ALLY, horz, vert);
    return;
  }

  /* Which ally to watch is the server's answer. Our own idea of who is
   * watchable is just the players we have happened to be sent this round,
   * which under viewPolicyKey is exactly the allies the policy is holding
   * back — so choosing here can only ever reach the ones already on screen.
   * The answer arrives as CTRL_VIEW_TARGET. */
  from = (cs->viewport.viewKind == VIEW_KIND_ALLY)
             ? (uint8_t)cs->viewport.viewTarget
             : (uint8_t)VIEW_CYCLE_FROM_NONE;
  clientSimNetSendViewCycle(cs, VIEW_KIND_ALLY,
                            clientSimViewCycleDirection(horz, vert), from);
}

void clientSimStepView(ClientSim *cs, int horz, int vert) {
  uint8_t kind = clientSimGetViewKind(cs);

  if (kind == VIEW_KIND_TANK) {
    return;
  }
  if (kind == VIEW_KIND_ALLY) {
    /* The scroll keys step between allies for the same reason the ally key
     * enters the view: the server is the one that knows who is watchable.
     * Pill and base stepping stays here — the client holds every pill and
     * base position from the map. */
    clientSimAllyView(cs, horz, vert);
    return;
  }
  clientSimItemView(cs, kind, horz, vert);
}

void clientSimApplyAllyViewTarget(ClientSim *cs, BYTE target, BYTE mapX,
                                  BYTE mapY) {
  if (cs == NULL) {
    return;
  }
  viewportEnterAllyView(clientSimViewportMut(cs), clientSimGetScroll(cs),
                        target, mapX, mapY);
}

void clientSimSyncViewState(ClientSim *cs) {
  uint8_t kind;
  uint8_t target;

  if (cs == NULL || !cs->hasTransport) {
    /* Nothing to report through — leave the last-sent pair alone so the
     * report still goes out once a transport is bound. */
    return;
  }
  kind = cs->viewport.viewKind;
  target = cs->viewport.viewTarget;
  if (kind == VIEW_KIND_TANK) {
    target = 0;
  }
  if (kind == cs->lastSentViewKind && target == cs->lastSentViewTarget) {
    return;
  }
  clientSimNetSendViewState(cs, kind, target);
  cs->lastSentViewKind = kind;
  cs->lastSentViewTarget = target;
}

void clientSimResetViewStateReport(ClientSim *cs) {
  if (cs == NULL) {
    return;
  }
  cs->lastSentViewKind = 0xFF;
  cs->lastSentViewTarget = 0xFF;
}

void clientSimRecalc(ClientSim *cs) {
  viewportRecalc(clientSimViewportMut(cs));
}

void clientSimUpdateView(ClientSim *cs, updateType value) {
  viewportUpdateView(clientSimViewportMut(cs), clientSimGetGameSim(cs),
                     clientSimGetMyPlayerNum(cs),
                     (BYTE (*)[MAP_ARRAY_SIZE])clientSimGetBrainMap(cs), value);
}

void clientSimPanX(ClientSim *cs, int dxTiles) {
  viewportPanX(clientSimViewportMut(cs), dxTiles);
}

void clientSimPanY(ClientSim *cs, int dyTiles) {
  viewportPanY(clientSimViewportMut(cs), dyTiles);
}

bool clientSimTankIsDead(ClientSim *cs) {
  return tankIsDestroyed(&MY_TANK(cs));
}

bool clientSimTankScroll(ClientSim *cs) {
  /* Don't scroll the view while watching an item — the view is locked on it */
  if (clientSimIsInItemView(cs) == TRUE) {
    return FALSE;
  }

  /* When autoscroll is on, scrollAutoScroll (called from clientUiOnTick)
   * is the sole owner of *xValue/*yValue and subPosX/Y. The legacy
   * per-tank-tick scrollManual call here stomps on subPos (resets to 0)
   * mid-frame, producing a visible flicker — the renderer at 60Hz can
   * sample between the zero-out and the next scrollAutoScroll. Skip it. */
  if (cs->scroll.autoScroll) {
    return FALSE;
  }

  /* Autoscroll OFF = WinBolo-style manual scrolling: scrollNoAutoScroll
   * (run each tick from clientUiOnTick) is the sole follow. This legacy
   * per-tank-tick scrollManual was a second, more aggressive follow (its
   * left-pull fires on rightPos==FALSE / any non-west facing), so driving
   * east it yanked the tank back to column >=2 and fought a manual keyboard
   * scroll-right (the column 1<->2 jitter). Don't run it. */
  return FALSE;
}

void clientSimManMove(ClientSim *cs, buildSelect buildS) {
  if (!tankIsDestroyed(&MY_TANK(cs)) && clientSimGetNetStatus(cs) != netFailed) {
    /* Route build request through InputPacket so the server sim
     * processes it authoritatively (matches brain build path). */
    clientSimSetPendingBuild(cs,
                             (BYTE) buildS + 1,  /* 1-based in InputPacket (0=none) */
                             (BYTE) (clientSimGetCursorPosX(cs) + clientSimGetXOffset(cs)),
                             (BYTE) (clientSimGetCursorPosY(cs) + clientSimGetYOffset(cs)));
  }
}

void clientSimManMoveToMap(ClientSim *cs, BYTE mapX, BYTE mapY, buildSelect buildS) {
  if (!tankIsDestroyed(&MY_TANK(cs)) && clientSimGetNetStatus(cs) != netFailed) {
    clientSimSetPendingBuild(cs, (BYTE) buildS + 1, mapX, mapY);
  }
}

void clientSimCycleBuildSelect(ClientSim *cs, int delta) {
  static const buildSelect order[] = { BsTrees, BsRoad, BsBuilding, BsPillbox, BsMine };
  const int N = (int)(sizeof(order) / sizeof(order[0]));
  buildSelect cur = clientSimGetCurrentBuildSelect(cs);
  int idx = 0;
  for (int i = 0; i < N; i++) {
    if (order[i] == cur) { idx = i; break; }
  }
  idx = ((idx + delta) % N + N) % N;
  clientSimSetCurrentBuildSelect(cs, order[idx]);
}

/* Alliance accessors. */
tankAlliance clientSimGetTankAlliance(ClientSim *cs, BYTE playerNum) {
  return playersScreenAllience(&clientSimGetGameSim(cs)->plyrs, clientSimGetMyPlayerNum(cs), (BYTE) (playerNum - 1));
}

pillAlliance clientSimGetPillAlliance(ClientSim *cs, BYTE pillNum) {
  return pillsGetAllianceNum(clientSimGetGameSim(cs), &clientSimGetGameSim(cs)->pb, pillNum);
}

baseAlliance clientSimGetBaseAlliance(ClientSim *cs, BYTE baseNum) {
  return basesGetStatusNum(clientSimGetGameSim(cs), baseNum);
}

BYTE clientSimGetPillNumPos(ClientSim *cs, BYTE mx, BYTE my) {
  return pillsGetViewPillNum(&clientSimGetGameSim(cs)->pb, (BYTE) (clientSimGetXOffset(cs) + mx), (BYTE) (clientSimGetYOffset(cs) + my), FALSE, FALSE);
}

BYTE clientSimGetBaseNumPos(ClientSim *cs, BYTE mx, BYTE my) {
  return basesGetBaseNum(&clientSimGetGameSim(cs)->bs, (BYTE) (clientSimGetXOffset(cs) + mx), (BYTE) (clientSimGetYOffset(cs) + my));
}

/* Local tank stat accessors. */
void clientSimGetTankStats(ClientSim *cs, BYTE *shellsAmount, BYTE *minesAmount, BYTE *armourAmount, BYTE *treesAmount) {
  tankGetStats(&MY_TANK(cs), shellsAmount, minesAmount, armourAmount, treesAmount);
}

void clientSimGetKillsDeaths(ClientSim *cs, int *kills, int *deaths) {
  tankGetKillsDeaths(&MY_TANK(cs), kills, deaths);
}

void clientSimConnectionLost(ClientSim *cs) {
  lgmConnectionLost(clientSimGetGameSim(cs), &MY_LGM(cs), &MY_TANK(cs), &clientSimGetGameSim(cs)->ss);
  playersConnectionLost(cs, clientSimGetGameSim(cs), &clientSimGetGameSim(cs)->plyrs, clientSimGetMyPlayerNum(cs));
}

/*********************************************************
 *NAME:          clientSaveMap
 *PURPOSE:
 * Saves the map. Returns whether the operation was
 * successful or not.
 *
 *ARGUMENTS:
 *  fileName - path and filename to save
 *********************************************************/
bool clientSaveMap(ClientSim *csPtr, char *fileName) {
  bool returnValue;

  returnValue = mapWrite(fileName, &clientSimGetGameSim(csPtr)->mp, &clientSimGetGameSim(csPtr)->pb, &clientSimGetGameSim(csPtr)->bs, &clientSimGetGameSim(csPtr)->ss);
  if (returnValue == TRUE) {
    if (clientSimGetNetType(csPtr) == netSingle) {
      MessageArgs args;
      memset(&args, 0, sizeof(args));
      playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), args.playerName);
      args.playerFlags = playersGetAccountFlags(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr));
      playersGetCountryCode(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), args.playerCountry);
      clientSimGetGameSim(csPtr)->callbacks.messageAdd(clientSimGetGameSim(csPtr)->callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_SAVED_MAP, &args);
    }
  }
  return returnValue;
}

/* ================================================================
 * Live-sim map / pill / base / start readers.
 * ================================================================ */

BYTE clientSimGetMapTerrain(const ClientSim *cs, BYTE x, BYTE y) {
  return mapGetPos(&clientSimGetGameSim((ClientSim *)cs)->mp, x, y);
}

bool clientSimMapIsMine(const ClientSim *cs, BYTE x, BYTE y) {
  return mapIsMine(&clientSimGetGameSim((ClientSim *)cs)->mp, x, y);
}

bool clientSimPillExistsAt(const ClientSim *cs, BYTE x, BYTE y) {
  return pillsExistPos(&clientSimGetGameSim((ClientSim *)cs)->pb, x, y);
}

BYTE clientSimPillGetScreenHealthAt(ClientSim *cs, BYTE x, BYTE y) {
  GameSim *gs = clientSimGetGameSim(cs);
  return pillsGetScreenHealth(gs, &gs->pb, x, y, gs->viewPlayer);
}

bool clientSimBaseExistsAt(const ClientSim *cs, BYTE x, BYTE y) {
  return basesExistPos(&clientSimGetGameSim((ClientSim *)cs)->bs, x, y);
}

baseAlliance clientSimBaseGetAllianceAt(ClientSim *cs, BYTE x, BYTE y) {
  GameSim *gs = clientSimGetGameSim(cs);
  return basesGetAlliancePos(gs, x, y, gs->viewPlayer);
}

bool clientSimBaseAmOwnerAt(ClientSim *cs, BYTE player, BYTE x, BYTE y) {
  return basesAmOwner(clientSimGetGameSim(cs), player, x, y);
}

BYTE clientSimGetPillCount(const ClientSim *cs) {
  return pillsGetNumPills(&clientSimGetGameSim((ClientSim *)cs)->pb);
}

BYTE clientSimGetBaseCount(const ClientSim *cs) {
  return basesGetNumBases(&clientSimGetGameSim((ClientSim *)cs)->bs);
}

BYTE clientSimGetStartCount(const ClientSim *cs) {
  return startsGetNumStarts(&clientSimGetGameSim((ClientSim *)cs)->ss);
}

bool clientSimGetPill(ClientSim *cs, BYTE i,
                      BYTE *x, BYTE *y, BYTE *owner, BYTE *armour,
                      bool *inTank) {
  GameSim *gs = clientSimGetGameSim(cs);
  pillbox p;
  BYTE n = pillsGetNumPills(&gs->pb);
  if (i == 0 || i > n) return false;
  pillsGetPill(&gs->pb, &p, i);
  if (x)      *x      = p.x;
  if (y)      *y      = p.y;
  if (owner)  *owner  = p.owner;
  if (armour) *armour = p.armour;
  if (inTank) *inTank = p.inTank;
  return true;
}

bool clientSimGetBase(ClientSim *cs, BYTE i,
                      BYTE *x, BYTE *y, BYTE *owner) {
  GameSim *gs = clientSimGetGameSim(cs);
  base b;
  BYTE n = basesGetNumBases(&gs->bs);
  if (i == 0 || i > n) return false;
  basesGetBase(&gs->bs, &b, i);
  if (x)     *x     = b.x;
  if (y)     *y     = b.y;
  if (owner) *owner = b.owner;
  return true;
}

bool clientSimGetBaseStats(ClientSim *cs, BYTE i,
                           BYTE *shells, BYTE *mines, BYTE *armour) {
  GameSim *gs = clientSimGetGameSim(cs);
  BYTE n = basesGetNumBases(&gs->bs);
  if (i == 0 || i > n) return false;
  basesGetStats(&gs->bs, i, shells, mines, armour);
  return true;
}

bool clientSimGetStart(ClientSim *cs, BYTE i,
                       BYTE *x, BYTE *y, BYTE *dir) {
  GameSim *gs = clientSimGetGameSim(cs);
  start s;
  BYTE n = startsGetNumStarts(&gs->ss);
  if (i == 0 || i > n) return false;
  startsGetStartStruct(&gs->ss, &s, i);
  if (x)   *x   = s.x;
  if (y)   *y   = s.y;
  if (dir) *dir = startsConvertDir((BYTE)((s.dir < 16) ? s.dir : 0));
  return true;
}
