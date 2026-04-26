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

#include <stdio.h>
#include <string.h>

#include "../bolo/global.h"
#include "../bolo/gametype.h"
#include "../bolo/nat_portmap.h"
#include "../bolo/transport_udp.h"
#include "../bolo/bot_manager.h"
#include "../winbolonet/winbolonet.h"
#include "threads.h"
#include "server_sim.h"
#include "server_lifecycle.h"

static char  instanceTrackerAddr[FILENAME_MAX] = "";
static unsigned short instanceTrackerPort = 0;
static bool  instanceUseTracker = FALSE;
static bool  instanceUseWbn = FALSE;
static bool  instanceUseNatKeepalive = FALSE;
static bool  instanceUseNatPortmap = FALSE;
static NatPortMap instancePortMap;

static int trackerTime = 5500;
static int wbnTime = 0;
static int natKeepaliveTime = 0;

bool serverInstanceStartup(ServerSim *sim, const ServerInstanceConfig *cfg) {
  const char *bindAddr = (cfg->bindAddr != NULL) ? cfg->bindAddr : "";
  const char *password = (cfg->password != NULL) ? cfg->password : "";

  if (transportUdpServerCreate(cfg->udpPort, bindAddr, sim,
                               password, cfg->maxPlayers) == FALSE) {
    return FALSE;
  }

  instanceUseWbn = cfg->useWbn;
  if (cfg->useWbn) {
    winbolonetCreateServer(sim->mapName, cfg->udpPort,
                           (BYTE)gameTypeGet(&sim->sim.game),
                           cfg->compTanks,
                           (BYTE)sim->sim.hiddenMines,
                           (BYTE)sim->hasPassword,
                           basesGetNumBases(&sim->sim.bs),
                           pillsGetNumPills(&sim->sim.pb),
                           serverSimGetNumNeutralBases(sim),
                           serverSimGetNumNeutralPills(sim),
                           serverSimGetNumPlayers(sim));
    if (!sim->lobbyEnabled) {
      winbolonetSendLobbyStatus(FALSE);
    }
  }

  instanceUseTracker = cfg->useTracker;
  if (cfg->useTracker && cfg->trackerAddr != NULL) {
    strncpy(instanceTrackerAddr, cfg->trackerAddr, FILENAME_MAX - 1);
    instanceTrackerAddr[FILENAME_MAX - 1] = '\0';
  } else {
    instanceTrackerAddr[0] = '\0';
  }
  instanceTrackerPort = cfg->trackerPort;
  instanceUseNatKeepalive = cfg->useNatKeepalive;

  trackerTime = 5500;
  wbnTime = 0;
  natKeepaliveTime = 0;

  instanceUseNatPortmap = cfg->useNatPortmap;
  memset(&instancePortMap, 0, sizeof(instancePortMap));
  if (cfg->useNatPortmap) {
    natPortMapRequest(cfg->udpPort, &instancePortMap);
  }
  return TRUE;
}

void serverInstanceTick(ServerSim *sim) {
  trackerTime++;
  wbnTime++;

  threadsWaitForMutex();
  /* Receive packets — queues inputs for both ticks */
  if (transportUdpServerHasRecvThread()) {
    transportUdpServerDrainRecvQueue(sim);
  } else {
    transportUdpServerRecv(sim);
  }

  if (sim->state == serverStateRunning) {
    /* Run brain AI bots — queues two InputPackets per bot (keys + game) */
    if (botManagerGetNumBots() > 0) {
      botManagerTick(sim, sim->botAiType);
    }
    /* Run two sim ticks per 20ms callback to match the client's
     * 100Hz rate (alternating keys tick + game tick).
     * Drain events after each tick so they're captured before
     * the next tick clears the event buffer. */
    {
      ServerState preTickState = sim->state;
      serverSimTick(sim);
      /* If game ended during this tick, broadcast game-over */
      if (preTickState == serverStateRunning && sim->state == serverStateGameOver) {
        if (sim->lobbyEnabled) {
          /* Capture win message now while game state is intact;
           * it will be sent after players return to the lobby. */
          serverSimBuildWinMessage(sim,
                                   sim->pendingWinMessage,
                                   sizeof(sim->pendingWinMessage));
          serverSimSendWbnWinEvents(sim);
          transportUdpServerBroadcastGameOver(sim);
        }
      }
      if (sim->state == serverStateRunning) {
        transportUdpServerDrainEvents(sim);
      }
    }
    if (sim->state == serverStateRunning) {
      /* Save tick 1's events so bots can see them next frame.
       * transportUdpServerDrainEvents already captured them for
       * UDP clients, but bots read directly from the event buffer
       * via serverSimBuildSnapshot — tick 2 would clear these. */
      GameEvent savedEvents[MAX_SNAPSHOT_EVENTS];
      uint8_t savedCount = sim->eventCount;
      ServerState preTickState;
      if (savedCount > 0) {
        memcpy(savedEvents, sim->events,
               savedCount * sizeof(GameEvent));
      }
      preTickState = sim->state;
      serverSimTick(sim);
      /* If game ended during this tick, broadcast game-over */
      if (preTickState == serverStateRunning && sim->state == serverStateGameOver) {
        if (sim->lobbyEnabled) {
          serverSimBuildWinMessage(sim,
                                   sim->pendingWinMessage,
                                   sizeof(sim->pendingWinMessage));
          serverSimSendWbnWinEvents(sim);
          transportUdpServerBroadcastGameOver(sim);
        }
      }
      if (sim->state == serverStateRunning) {
        transportUdpServerDrainEvents(sim);
        /* Prepend tick 1's events before tick 2's events */
        if (savedCount > 0 && savedCount + sim->eventCount <= MAX_SNAPSHOT_EVENTS) {
          memmove(sim->events + savedCount, sim->events,
                  sim->eventCount * sizeof(GameEvent));
          memcpy(sim->events, savedEvents,
                 savedCount * sizeof(GameEvent));
          sim->eventCount += savedCount;
        }
      }
    }
    /* Send snapshots only if still running */
    if (sim->state == serverStateRunning) {
      transportUdpServerSend(sim);
    }
  } else {
    /* Lobby/countdown/gameover: single tick for state machine processing */
    ServerState preTickState = sim->state;
    serverSimTick(sim);

    /* Check if a balance proposal just completed */
    if (sim->balanceProposal.broadcastNeeded) {
      transportUdpServerBroadcastBalanceProposal(sim, sim->balanceProposal.teamForSlot);
      sim->balanceProposal.broadcastNeeded = false;
    }

    /* Handle state transitions */
    if (preTickState == serverStateCountdown) {
      if (sim->state == serverStateRunning) {
        /* Countdown finished — game started */
        transportUdpServerBroadcastGameStart(sim);
        if (botManagerGetNumBots() > 0) {
          botManagerOnGameStart(sim);
        }
        /* Notify WBN that we are now in-game */
        winbolonetSendLobbyStatus(FALSE);
        /* Send EVENT_PLAYER_JOIN for each connected WBN player */
        {
          BYTE pi;
          for (pi = 0; pi < MAX_TANKS; pi++) {
            if (sim->playerConnected[pi] &&
                winboloNetIsPlayerParticipant(pi)) {
              winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                 pi, WINBOLO_NET_NO_PLAYER);
            }
          }
        }
      } else if (sim->state == serverStateCountdown &&
                 sim->countdownTicks > 0 &&
                 sim->countdownTicks % 50 == 0) {
        /* Broadcast countdown tick (once per second) */
        uint8_t secs = (uint8_t)(sim->countdownTicks / 50);
        transportUdpServerBroadcastCountdown(sim, secs);
      }
    }
    if (preTickState == serverStateGameOver &&
        sim->state == serverStateLobby) {
      /* Flush remaining WBN events (win, final kills, etc.) */
      winbolonetServerUpdate(serverSimGetNumPlayers(sim),
                             serverSimGetNumNeutralBases(sim),
                             serverSimGetNumNeutralPills(sim), TRUE);
      /* Pick next map from rotation if mapdir is configured */
      if (sim->mapDirFiles != NULL) {
        serverSimMapDirPickRandom(sim);
        transportUdpServerNotifyMapChange(sim);
      }
      /* Re-register with WBN for the new round */
      if (winbolonetIsRunning()) {
        winbolonetReturnToLobby(
          sim->mapName, sim->serverPort,
          (BYTE)gameTypeGet(&sim->sim.game),
          (BYTE)sim->botAiType,
          (BYTE)sim->sim.hiddenMines,
          sim->hasPassword,
          basesGetNumBases(&sim->sim.bs),
          pillsGetNumPills(&sim->sim.pb),
          serverSimGetNumNeutralBases(sim),
          serverSimGetNumNeutralPills(sim),
          serverSimGetNumPlayers(sim));
      }
      /* Returned to lobby — broadcast full lobby state */
      transportUdpServerBroadcastLobbyState(sim);
      /* Send the win message now that players are back in the lobby */
      if (sim->pendingWinMessage[0] != '\0') {
        transportUdpServerSendServerMessage(sim->pendingWinMessage);
        sim->pendingWinMessage[0] = '\0';
      }
    }

    /* Periodic lobby snapshot — twice per second (every 25 ticks)
     * for ping/country updates and state consistency */
    if ((sim->state == serverStateLobby || sim->state == serverStateCountdown) &&
        sim->tick % 25 == 0) {
      transportUdpServerBroadcastLobbyState(sim);
    }

    /* Timeout check — not called via transportUdpServerSend() during lobby */
    transportUdpServerCheckTimeouts(sim);
  }

  /* Auto-close check — works in any state.
   * When auto-close triggers, force a no-lobby shutdown regardless
   * of lobby mode, since there are no players to return to lobby for. */
  if (sim->autoCloseOnEmpty && serverSimCheckAutoClose(sim)) {
    sim->lobbyEnabled = FALSE;
    serverSimEnterGameOver(sim);
  }

  /* Empty reset check — when enabled and no players are connected,
   * count down and reset to lobby with map reload after the timeout.
   * Skipped if autoCloseOnEmpty is active (it takes priority). */
  if (sim->emptyResetEnabled && !sim->autoCloseOnEmpty &&
      sim->lobbyEnabled &&
      sim->state != serverStateGameOver &&
      sim->state != serverStateLobby &&
      sim->state != serverStateCountdown &&
      serverSimCheckEmptyReset(sim)) {
    serverSimConsoleMessage("Empty reset timer expired. Resetting to lobby...");
    serverSimResetGameWorld(sim);
    sim->state = serverStateLobby;
    sim->gameLength = sim->originalGameLength;
    sim->hadPlayersEver = FALSE;
    sim->emptyResetTicks = -1;
    /* Pick next map from rotation if mapdir is configured */
    if (sim->mapDirFiles != NULL) {
      serverSimMapDirPickRandom(sim);
      transportUdpServerNotifyMapChange(sim);
    }
    /* Re-register with WBN for the new round */
    if (winbolonetIsRunning()) {
      winbolonetReturnToLobby(
        sim->mapName, sim->serverPort,
        (BYTE)gameTypeGet(&sim->sim.game),
        (BYTE)sim->botAiType,
        (BYTE)sim->sim.hiddenMines,
        sim->hasPassword,
        basesGetNumBases(&sim->sim.bs),
        pillsGetNumPills(&sim->sim.pb),
        serverSimGetNumNeutralBases(sim),
        serverSimGetNumNeutralPills(sim),
        serverSimGetNumPlayers(sim));
    }
    transportUdpServerBroadcastLobbyState(sim);
  }

  threadsReleaseMutex();

  if (wbnTime > 100) {
    threadsWaitForMutex();
    winbolonetServerUpdate(serverSimGetNumPlayers(sim), serverSimGetNumNeutralBases(sim), serverSimGetNumNeutralPills(sim), FALSE);
    threadsReleaseMutex();
    wbnTime = 0;
  }

  if (trackerTime >= 6000 && instanceUseTracker) {
    transportUdpServerSendTrackerUpdate(sim, instanceTrackerAddr, instanceTrackerPort);
    trackerTime = 0;
  }

  if (instanceUseNatKeepalive && instanceUseTracker) {
    natKeepaliveTime++;
    if (natKeepaliveTime >= 1250) {
      transportUdpServerSendNatKeepalive(instanceTrackerAddr, instanceTrackerPort);
      natKeepaliveTime = 0;
    }
  }

  if (instanceUseNatPortmap) {
    natPortMapRenewIfNeeded(&instancePortMap);
  }
}

void serverInstanceShutdown(ServerSim *sim) {
  if (instanceUseNatPortmap) {
    natPortMapRelease(&instancePortMap);
  }
  if (instanceUseWbn) {
    winbolonetDestroy(TRUE);
  }
  transportUdpServerDestroy();
  botManagerDestroy(sim);
  instanceUseWbn = FALSE;
  instanceUseTracker = FALSE;
  instanceUseNatKeepalive = FALSE;
  instanceUseNatPortmap = FALSE;
}
