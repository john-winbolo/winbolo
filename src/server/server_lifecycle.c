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

#include <SDL3/SDL.h>

#include "global.h"
#include "control_event.h"
#include "gametype.h"
#include "nat_portmap.h"
#include "transport_udp.h"
#include "bot_manager.h"
#include "../winbolonet/winbolonet_core.h"
#include "../winbolonet/winbolonet_server.h"
#include "threads.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "server_lifecycle.h"

static char  instanceTrackerAddr[FILENAME_MAX] = "";
static unsigned short instanceTrackerPort = 0;
static unsigned short instanceUdpPort = 0;
static bool  instanceAcceptRemoteClients = FALSE;
static bool  instanceUseTracker = FALSE;
static bool  instanceUseWbn = FALSE;
static bool  instanceUseNatKeepalive = FALSE;
static bool  instanceUseNatPortmap = FALSE;
static NatPortMap instancePortMap;
static int   natPortmapWaitTicks = 0;
static bool  natPortmapNotified = FALSE;
static bool  natPortmapTimedOut = FALSE;

static int   probeWaitTicks       = 0;     /* ticks since last probe send  */
static int   probeReplyWaitTicks  = 0;     /* ticks since last probe reply */
static bool  probeReceived        = FALSE; /* any reply ever arrived       */
static bool  probeTimedOut        = FALSE; /* deadline elapsed w/o reply   */
static char  probeReflexiveIp[64];
static unsigned short probeReflexivePort = 0;

#define PROBE_SEND_INTERVAL_TICKS  500   /* 10 s @ 50 Hz */
#define PROBE_TIMEOUT_TICKS       1500   /* 30 s @ 50 Hz */

static ManualProbeState manualProbeState = MANUAL_PROBE_IDLE;
static int              manualProbeWaitTicks = 0;

#define MANUAL_PROBE_TIMEOUT_TICKS 100   /* 2 s @ 50 Hz */

/* 1500 slices @ ~50Hz (SERVER_TICK_LENGTH = 20ms) ≈ 30s.  After this many
 * ticks without a successful libplum mapping, give up and let Phase 2d's
 * lobby UI surface the "could not open port automatically" state. */
#define NAT_PORTMAP_TIMEOUT_TICKS 1500

static int trackerTime = 5500;
static int wbnTime = 0;
static int natKeepaliveTime = 0;

/* Wall-clock duration of the most recent tick (ms) and its EWMA. Both
 * stay 0 until the first tick has been recorded; the EWMA is seeded
 * from that first measurement so it doesn't bias toward zero. */
static double s_lastTickMs = 0.0;
static double s_tickMsEwma = 0.0;
static const  double kTickAlpha = 0.1;

/* Wall-clock cost (ms) of the two serverSimTick calls combined for the
 * most recent tick, plus its EWMA. Same seeding rule as above. */
static double s_lastSimMs = 0.0;
static double s_simMsEwma = 0.0;

void serverLifecycleRecordTickMs(double ms) {
  s_lastTickMs = ms;
  if (s_tickMsEwma <= 0.0) {
    s_tickMsEwma = ms;
  } else {
    s_tickMsEwma = kTickAlpha * ms + (1.0 - kTickAlpha) * s_tickMsEwma;
  }
}

void serverLifecycleGetTickStats(double *outLastMs, double *outEwmaMs) {
  if (outLastMs)  *outLastMs  = s_lastTickMs;
  if (outEwmaMs)  *outEwmaMs  = s_tickMsEwma;
}

static void serverLifecycleRecordSimMs(double ms) {
  s_lastSimMs = ms;
  if (s_simMsEwma <= 0.0) {
    s_simMsEwma = ms;
  } else {
    s_simMsEwma = kTickAlpha * ms + (1.0 - kTickAlpha) * s_simMsEwma;
  }
}

void serverLifecycleGetSimStats(double *outLastMs, double *outEwmaMs) {
  if (outLastMs)  *outLastMs  = s_lastSimMs;
  if (outEwmaMs)  *outEwmaMs  = s_simMsEwma;
}

bool serverInstanceStartup(ServerSim *sim, const ServerInstanceConfig *cfg) {
  const char *bindAddr = (cfg->bindAddr != NULL) ? cfg->bindAddr : "";
  const char *password = (cfg->password != NULL) ? cfg->password : "";

  instanceAcceptRemoteClients = cfg->acceptRemoteClients;

  if (cfg->acceptRemoteClients) {
    sim->maxPlayers = (cfg->maxPlayers > 0) ? cfg->maxPlayers : (BYTE)MAX_TANKS;
    if (transportUdpServerCreate(cfg->udpPort, bindAddr, sim,
                                 password) == FALSE) {
      return FALSE;
    }
    transportUdpServerSetUploadConfig(cfg->uploadPolicy,
                                      cfg->uploadMaxFiles,
                                      cfg->uploadMaxStorageBytes);
    sim->uploadPolicy = cfg->uploadPolicy;
  }

  serverSimApplyInstanceConfig(sim, cfg);

  instanceUseWbn = cfg->acceptRemoteClients && cfg->useWbn;
  if (instanceUseWbn) {
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

  if (cfg->acceptRemoteClients) {
    instanceUseTracker = cfg->useTracker;
    if (cfg->useTracker && cfg->trackerAddr != NULL) {
      strncpy(instanceTrackerAddr, cfg->trackerAddr, FILENAME_MAX - 1);
      instanceTrackerAddr[FILENAME_MAX - 1] = '\0';
    } else {
      instanceTrackerAddr[0] = '\0';
    }
    instanceTrackerPort = cfg->trackerPort;
    instanceUseNatKeepalive = cfg->useNatKeepalive;
  } else {
    instanceUseTracker = FALSE;
    instanceTrackerAddr[0] = '\0';
    instanceTrackerPort = 0;
    instanceUseNatKeepalive = FALSE;
  }
  instanceUdpPort = cfg->udpPort;

  trackerTime = 5500;
  wbnTime = 0;
  natKeepaliveTime = 0;

  instanceUseNatPortmap = cfg->acceptRemoteClients && cfg->useNatPortmap;
  memset(&instancePortMap, 0, sizeof(instancePortMap));
  natPortmapWaitTicks = 0;
  natPortmapNotified  = FALSE;
  natPortmapTimedOut  = FALSE;
  probeWaitTicks       = 0;
  probeReplyWaitTicks  = 0;
  probeReceived        = FALSE;
  probeTimedOut        = FALSE;
  probeReflexiveIp[0]  = '\0';
  probeReflexivePort   = 0;
  manualProbeState     = MANUAL_PROBE_IDLE;
  manualProbeWaitTicks = 0;
  if (instanceUseNatPortmap) {
    natPortMapRequest(cfg->udpPort, &instancePortMap);
  }
  return TRUE;
}

void serverInstanceTick(ServerSim *sim) {
  /* Measure the entire tick wall-clock — outside the mutex acquire so
   * the EWMA captures contention wait time too. Single bottom-of-function
   * end measurement; the function has no early-return paths. */
  Uint64 tickStart = SDL_GetPerformanceCounter();

  trackerTime++;
  wbnTime++;

  transportUdpServerDrainPunchQueue();

  threadsWaitForMutex();
  /* Receive packets — queues inputs for both ticks */
  if (transportUdpServerHasRecvThread()) {
    transportUdpServerDrainRecvQueue(sim);
  } else {
    transportUdpServerRecv(sim);
  }

  if (sim->state == serverStateRunning) {
    /* Run brain AI bots — queues two InputPackets per bot (keys + game) */
    if (serverSimGetNumBots(sim) > 0) {
      serverSimBotTick(sim, sim->botAiType);
    }
    /* Run two sim ticks per 20ms callback to match the client's
     * 100Hz rate (alternating keys tick + game tick).
     * Drain events after each tick so they're captured before
     * the next tick clears the event buffer.
     *
     * Two timing pairs (sim1Start/End, sim2Start/End) summed into
     * simMs — the bookkeeping between the ticks (drain + memcpy +
     * any game-over broadcast) stays where it is but is excluded
     * from the simulation cost. */
    Uint64 sim1Start = 0, sim1End = 0;
    Uint64 sim2Start = 0, sim2End = 0;
    {
      ServerState preTickState = sim->state;
      sim1Start = SDL_GetPerformanceCounter();
      serverSimTick(sim);
      sim1End = SDL_GetPerformanceCounter();
      /* If game ended during this tick, publish game-over events */
      if (preTickState == serverStateRunning && sim->state == serverStateGameOver) {
        if (sim->lobbyEnabled) {
          if (serverSimConsumeSuppressNextWinMessage(sim)) {
            /* Vote-driven game end already announced itself. */
            sim->pendingWinMessage[0] = '\0';
          } else {
            /* Capture win message now while game state is intact;
             * it will be sent after players return to the lobby. */
            serverSimBuildWinMessage(sim,
                                     sim->pendingWinMessage,
                                     sizeof(sim->pendingWinMessage));
          }
          serverSimSendWbnWinEvents(sim);
        }
        {
          ControlEvent phaseEvt;
          ControlEvent overEvt;
          memset(&phaseEvt, 0, sizeof(phaseEvt));
          phaseEvt.type = CTRL_GAME_PHASE_GAME_OVER;
          serverSimPublishControl(sim, &phaseEvt);
          memset(&overEvt, 0, sizeof(overEvt));
          overEvt.type = CTRL_GAME_OVER;
          serverSimPublishControl(sim, &overEvt);
        }
      }
      if (sim->state == serverStateRunning) {
        if (instanceAcceptRemoteClients) {
          transportUdpServerDrainEvents(sim);
        }
      }
    }
    if (sim->state == serverStateRunning) {
      /* Save tick 1's events (regular + map) so snapshot readers see
       * them after tick 2.  serverSimTick clears both buffers at its
       * start, so without this save/restore the events from tick 1
       * are wiped before any reader (bots in next serverInstanceTick,
       * main-thread snapshot poll between serverInstanceTicks, UDP
       * drain) can observe them. transportUdpServerDrainEvents already
       * captured the regular events for UDP clients earlier; this
       * preserves them for the in-process snapshot path. Map events
       * have no parallel drain — losing them desyncs client terrain
       * (shell hits on the game-tick get wiped by the keys-tick
       * clear within the same serverInstanceTick). */
      GameEvent savedEvents[MAX_SNAPSHOT_EVENTS];
      uint8_t savedCount = sim->eventCount;
      GameEvent savedMapEvents[MAX_MAP_EVENTS];
      uint16_t savedMapCount = sim->mapEventCount;
      ServerState preTickState;
      if (savedCount > 0) {
        memcpy(savedEvents, sim->events,
               savedCount * sizeof(GameEvent));
      }
      if (savedMapCount > 0) {
        memcpy(savedMapEvents, sim->mapEvents,
               savedMapCount * sizeof(GameEvent));
      }
      preTickState = sim->state;
      sim2Start = SDL_GetPerformanceCounter();
      serverSimTick(sim);
      sim2End = SDL_GetPerformanceCounter();
      /* If game ended during this tick, publish game-over events */
      if (preTickState == serverStateRunning && sim->state == serverStateGameOver) {
        if (sim->lobbyEnabled) {
          serverSimBuildWinMessage(sim,
                                   sim->pendingWinMessage,
                                   sizeof(sim->pendingWinMessage));
          serverSimSendWbnWinEvents(sim);
        }
        {
          ControlEvent phaseEvt;
          ControlEvent overEvt;
          memset(&phaseEvt, 0, sizeof(phaseEvt));
          phaseEvt.type = CTRL_GAME_PHASE_GAME_OVER;
          serverSimPublishControl(sim, &phaseEvt);
          memset(&overEvt, 0, sizeof(overEvt));
          overEvt.type = CTRL_GAME_OVER;
          serverSimPublishControl(sim, &overEvt);
        }
      }
      if (sim->state == serverStateRunning) {
        if (instanceAcceptRemoteClients) {
          transportUdpServerDrainEvents(sim);
        }
        /* Prepend tick 1's events before tick 2's events */
        if (savedCount > 0 && savedCount + sim->eventCount <= MAX_SNAPSHOT_EVENTS) {
          memmove(sim->events + savedCount, sim->events,
                  sim->eventCount * sizeof(GameEvent));
          memcpy(sim->events, savedEvents,
                 savedCount * sizeof(GameEvent));
          sim->eventCount += savedCount;
        }
        /* Same prepend for map events — see comment at savedMapEvents above. */
        serverSimPrependMapEvents(sim, savedMapEvents, savedMapCount);
      }
    }
    double simFreq = (double)SDL_GetPerformanceFrequency();
    double simMs = ((double)(sim1End - sim1Start)
                    + (double)(sim2End - sim2Start)) * 1000.0 / simFreq;
    serverLifecycleRecordSimMs(simMs);
    /* Send snapshots only if still running */
    if (sim->state == serverStateRunning) {
      if (instanceAcceptRemoteClients) {
        transportUdpServerSend(sim);
      }
    }
  } else {
    /* Lobby/countdown/gameover: single tick for state machine processing */
    ServerState preTickState = sim->state;
    serverSimTick(sim);

    /* Check if a balance proposal just completed */
    if (sim->balanceProposal.broadcastNeeded) {
      ControlEvent evt;
      memset(&evt, 0, sizeof(evt));
      evt.type = CTRL_BALANCE_PROPOSAL;
      memcpy(evt.u.balanceProposal.teamForSlot,
             sim->balanceProposal.teamForSlot, MAX_TANKS);
      serverSimPublishControl(sim, &evt);
      sim->balanceProposal.broadcastNeeded = false;
    }

    /* Handle state transitions */
    if (preTickState == serverStateCountdown) {
      if (sim->state == serverStateRunning) {
        /* Countdown finished — game started.  Reset per-client and
         * per-slot transport state before publishing the RUNNING
         * transition so the codec encodes PACKET_GAME_START against
         * fresh queues. */
        transportUdpServerOnGameStart(sim);
        {
          ControlEvent evt;
          memset(&evt, 0, sizeof(evt));
          evt.type = CTRL_GAME_PHASE_RUNNING;
          serverSimPublishControl(sim, &evt);
        }
        if (serverSimGetNumBots(sim) > 0) {
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
        uint8_t secs = (uint8_t)((sim->countdownTicks + 49) / 50);
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_GAME_PHASE_COUNTDOWN;
        evt.u.gamePhase.countdownSeconds = secs;
        serverSimPublishControl(sim, &evt);
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
        transportUdpServerOnLobbyMapChange(sim);
        {
          ControlEvent evt;
          memset(&evt, 0, sizeof(evt));
          evt.type = CTRL_LOBBY_MAP_CHANGE;
          serverSimPublishControl(sim, &evt);
        }
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
        /* Push the freshly rotated server_key to every WBN-participating
         * client so they can mint a new player_key and re-auth.  Gated
         * inside; no-op when WBN isn't running. */
        transportUdpServerBroadcastWbnRekey(sim);
      }
      /* Republish the bot brain catalogue.  Mid-game joiners were gated
       * out of the BrainList during their sync replay (see
       * serverSimSyncSubscriber), so they need it now before the lobby
       * UI's AiConfig combobox appears.  In-lobby clients get it as a
       * (cheap) refresh. */
      {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        serverSimFillLobbyBrainListEvent(sim, &evt);
        serverSimPublishControl(sim, &evt);
      }
      /* Send the win message now that players are back in the lobby */
      if (sim->pendingWinMessage[0] != '\0') {
        transportUdpServerSendServerMessage(sim->pendingWinMessage);
        sim->pendingWinMessage[0] = '\0';
      }
    }

    /* Retransmit unacked control events every 4 ticks (~80ms at 50 Hz)
     * for loss recovery during lobby/countdown/gameover.  During running,
     * the snapshot tail carries the per-client unacked tail every tick,
     * so this scan only matters when snapshots aren't flowing. */
    if (sim->tick % 4 == 0) {
      transportUdpServerRetransmitUnackedControl();
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
      transportUdpServerOnLobbyMapChange(sim);
      {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_LOBBY_MAP_CHANGE;
        serverSimPublishControl(sim, &evt);
      }
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
      /* Same rotation push as the game-over → lobby site. */
      transportUdpServerBroadcastWbnRekey(sim);
    }
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
      transportUdpServerSendNatKeepalive(sim, instanceTrackerAddr, instanceTrackerPort);
      natKeepaliveTime = 0;
    }
  }

  if (instanceUseNatPortmap) {
    natPortMapRenewIfNeeded(&instancePortMap);
  }

  if (instanceUseNatPortmap && !natPortmapNotified && !natPortmapTimedOut) {
    if (instancePortMap.mapped) {
      /* First completion — point INFO_PACKET at the external address and
       * force a tracker refresh so the registered entry self-corrects in
       * one round trip instead of waiting up to ~120s for the next
       * periodic update. */
      transportUdpServerSetPublicAddress(instancePortMap.externalIp,
                                         instancePortMap.externalPort);
      if (instanceUseTracker && instanceTrackerAddr[0] != '\0') {
        transportUdpServerSendTrackerUpdate(sim, instanceTrackerAddr,
                                            instanceTrackerPort);
        trackerTime = 0;
      }
      natPortmapNotified = TRUE;
    } else if (++natPortmapWaitTicks >= NAT_PORTMAP_TIMEOUT_TICKS) {
      natPortmapTimedOut = TRUE;
    }
  }

  if (instanceUseNatPortmap && instanceUseTracker &&
      instanceTrackerAddr[0] != '\0') {
    if (++probeWaitTicks >= PROBE_SEND_INTERVAL_TICKS) {
      transportUdpServerSendPunchProbe(instanceTrackerAddr,
                                       instanceTrackerPort);
      probeWaitTicks = 0;
    }
    if (!probeReceived) {
      if (++probeReplyWaitTicks >= PROBE_TIMEOUT_TICKS) {
        probeTimedOut = TRUE;
      }
    }
  }

  if (manualProbeState == MANUAL_PROBE_IN_PROGRESS) {
    if (++manualProbeWaitTicks >= MANUAL_PROBE_TIMEOUT_TICKS) {
      manualProbeState = MANUAL_PROBE_TIMEOUT;
    }
  }

  Uint64 tickEnd = SDL_GetPerformanceCounter();
  double freq = (double)SDL_GetPerformanceFrequency();
  double tickMs = (double)(tickEnd - tickStart) * 1000.0 / freq;
  serverLifecycleRecordTickMs(tickMs);
}

void serverInstanceShutdown(ServerSim *sim) {
  if (instanceUseNatPortmap) {
    natPortMapRelease(&instancePortMap);
  }
  if (instanceUseWbn) {
    winbolonetDestroy(TRUE);
  }
  if (instanceAcceptRemoteClients) {
    transportUdpServerDestroy();
  }
  botManagerDestroy(sim);
  instanceAcceptRemoteClients = FALSE;
  instanceUseWbn = FALSE;
  instanceUseTracker = FALSE;
  instanceUseNatKeepalive = FALSE;
  instanceUseNatPortmap = FALSE;
  instanceUdpPort = 0;
  natPortmapWaitTicks = 0;
  natPortmapNotified  = FALSE;
  natPortmapTimedOut  = FALSE;
  probeWaitTicks       = 0;
  probeReplyWaitTicks  = 0;
  probeReceived        = FALSE;
  probeTimedOut        = FALSE;
  probeReflexiveIp[0]  = '\0';
  probeReflexivePort   = 0;
  manualProbeState     = MANUAL_PROBE_IDLE;
  manualProbeWaitTicks = 0;
}

bool serverInstanceIsNatPunchActive(void) {
  /* No mutex needed — these are plain bools written once at startup
   * / cleared once at shutdown. */
  return instanceUseNatKeepalive && instanceUseTracker;
}

void serverInstanceGetPortmapInfo(ServerPortmapInfo *out) {
  if (out == NULL) {
    return;
  }
  threadsWaitForMutex();
  if (!instanceUseNatPortmap) {
    out->status = SERVER_PORTMAP_DISABLED;
  } else if (instancePortMap.mapped) {
    /* libplum says we have a stable external mapping. Cross-check the
     * reflexive address against what the tracker reports — if they
     * differ, a NAT layer above libplum's gateway is rewriting our
     * port per destination (symmetric NAT) and the libplum mapping
     * is unreachable from third parties. */
    if (probeReceived &&
        (strcmp(probeReflexiveIp, instancePortMap.externalIp) != 0 ||
         probeReflexivePort != instancePortMap.externalPort)) {
      out->status = SERVER_PORTMAP_SYMMETRIC_NAT;
    } else {
      out->status = SERVER_PORTMAP_SUCCEEDED;
    }
  } else if (natPortmapTimedOut) {
    /* libplum gave up. Hole-punching is the fallback — viable iff the
     * tracker probe round-trips. */
    if (probeReceived) {
      out->status = SERVER_PORTMAP_HOLE_PUNCH_OK;
    } else if (probeTimedOut) {
      out->status = SERVER_PORTMAP_FAILED;
    } else {
      out->status = SERVER_PORTMAP_PENDING;
    }
  } else {
    out->status = SERVER_PORTMAP_PENDING;
  }
  out->internalPort   = instanceUdpPort;
  out->externalIp[0]  = '\0';
  out->externalPort   = 0;
  if (out->status == SERVER_PORTMAP_SUCCEEDED) {
    strncpy(out->externalIp, instancePortMap.externalIp,
            sizeof(out->externalIp) - 1);
    out->externalIp[sizeof(out->externalIp) - 1] = '\0';
    out->externalPort = instancePortMap.externalPort;
  } else if (probeReceived) {
    strncpy(out->externalIp, probeReflexiveIp,
            sizeof(out->externalIp) - 1);
    out->externalIp[sizeof(out->externalIp) - 1] = '\0';
    out->externalPort = probeReflexivePort;
  }
  threadsReleaseMutex();
}

void serverInstanceRecordProbeReply(const char *reflexiveIp,
                                    unsigned short reflexivePort) {
  if (reflexiveIp == NULL) return;
  threadsWaitForMutex();
  strncpy(probeReflexiveIp, reflexiveIp, sizeof(probeReflexiveIp) - 1);
  probeReflexiveIp[sizeof(probeReflexiveIp) - 1] = '\0';
  probeReflexivePort  = reflexivePort;
  probeReceived       = TRUE;
  probeTimedOut       = FALSE;       /* mapping is alive; reset deadline */
  probeReplyWaitTicks = 0;
  if (manualProbeState == MANUAL_PROBE_IN_PROGRESS) {
    manualProbeState = MANUAL_PROBE_SUCCESS;
  }
  threadsReleaseMutex();
}

void serverInstanceTriggerManualProbe(void) {
  threadsWaitForMutex();
  manualProbeState     = MANUAL_PROBE_IN_PROGRESS;
  manualProbeWaitTicks = 0;
  /* Force the periodic-probe block to fire on the next tick. The block
   * tests `++probeWaitTicks >= PROBE_SEND_INTERVAL_TICKS`, so setting
   * the counter to the threshold causes an immediate send. */
  probeWaitTicks = PROBE_SEND_INTERVAL_TICKS;
  threadsReleaseMutex();
}

ManualProbeState serverInstanceGetManualProbeState(void) {
  ManualProbeState s;
  threadsWaitForMutex();
  s = manualProbeState;
  threadsReleaseMutex();
  return s;
}
