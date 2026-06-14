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
#include "../common/mp_diag_log.h"
#include "../winbolonet/winbolonet_server.h"
#include "threads.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "server_lifecycle.h"

/* Round-end log hooks installed by WinBoloDS via
 * serverLifecycleSetRoundLogHooks. NULL on every other binary that
 * links server_static, so the lifecycle's stash/flush calls become
 * no-ops there. */
static void (*s_roundLogStash)(void) = NULL;
static void (*s_roundLogFlush)(void) = NULL;

void serverLifecycleSetRoundLogHooks(void (*stash)(void),
                                     void (*flush)(void)) {
  s_roundLogStash = stash;
  s_roundLogFlush = flush;
}

static void roundLogStash(void) {
  if (s_roundLogStash != NULL) s_roundLogStash();
}

static void roundLogFlush(void) {
  if (s_roundLogFlush != NULL) s_roundLogFlush();
}

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

  /* Gate the MP diagnostic log on acceptRemoteClients so bg_game's own
   * ServerSim (which also runs serverInstanceStartup at welcome-screen
   * boot) doesn't fill the log with its publishes.  Enable BEFORE
   * serverSimApplyInstanceConfig below so the very first lobby-settings
   * publish for the real MP host is captured. */
  if (cfg->acceptRemoteClients) {
    mpDiagLogEnable(1);
  }

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
    /* Populate the WBN lobby snapshot so register carries the
     * extended settings + human/bot counts on the first POST. */
    serverSimRefreshWbnLobbyInfo(sim);
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

/* Boot-all map-rotation round restart. Pairs serverSimMapRotateRound's
 * sim-core reset with the surrounding transport teardown and WBN session
 * rotation, mirroring the gameOver->lobby and empty-reset dances but starting
 * a fresh no-lobby running round instead of returning to a lobby.
 *
 * Always stashes the finished round's log here: the dedicated-log
 * subscriber's handleGameOver no-ops in no-lobby mode (it defers to the
 * process-shutdown path that doesn't run in rotation), so neither the win
 * trigger's CTRL_GAME_PHASE_GAME_OVER publish nor the empty trigger stashes
 * for us. serverDedicatedLogStashCurrentRound is idempotent (no-ops once the
 * log is stopped), so an extra call is harmless. */
static void serverLifecycleRotateRound(ServerSim *sim) {
  /* Boot everyone first — the round is over and nobody carries forward. */
  if (instanceAcceptRemoteClients) {
    transportUdpServerDisconnectAll(sim);
  }

  roundLogStash();

  /* Sim-core reset: opens the WBN rotation window, picks the next map and
   * starts a fresh empty running round (state -> running). */
  serverSimMapRotateRound(sim);

  /* WBN session rotation around the round-log upload. End the finished round's
   * session (server/quit) so WBN accepts the upload against the still-live
   * key, flush the upload, then register the next round's session — which
   * overwrites winboloNetServerKey with the freshly-picked map's key. Same
   * sandwich as the gameOver->lobby and empty-reset sites. Done after
   * serverSimMapRotateRound so BeginSession reports the new map / base / pill
   * counts, not the round that just ended. */
  if (winbolonetIsRunning()) {
    /* Flush any WBN events still queued from the finished round (win
     * crediting, final kills) against the live key before tearing the
     * session down — mirrors the gameOver->lobby block's pre-EndSession
     * flush. Player counts read 0 here (everyone was just booted); only
     * the queued events matter. */
    winbolonetServerUpdate(serverSimGetNumPlayers(sim),
                           serverSimGetNumNeutralBases(sim),
                           serverSimGetNumNeutralPills(sim), TRUE);
    winbolonetEndSession();
  }
  roundLogFlush();
  if (winbolonetIsRunning()) {
    serverSimRefreshWbnLobbyInfo(sim);
    winbolonetBeginSession(
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
    /* Clients were just booted, so this is a no-op here; kept for symmetry
     * with the other rotation sites (gated inside on connected clients). */
    transportUdpServerBroadcastWbnRekey(sim);
  }
  /* New key installed (or WBN off) — close the rotation window so any deferred
   * lobby_update flushes against the right session on the next WBN tick. */
  sim->wbnSessionRotating = FALSE;

  /* Start the next round's log and push the fresh map to any in-process
   * subscriber (SP host loopback, replay-log writer). serverSimMapRotateRound
   * already moved the sim to running; publish RUNNING (the dedicated-log
   * subscriber opens a fresh round log on it) and the map change. */
  {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_RUNNING;
    serverSimPublishControl(sim, &evt);
  }
  if (instanceAcceptRemoteClients) {
    transportUdpServerOnLobbyMapChange(sim);
  }
  {
    ControlEvent mapEvt;
    memset(&mapEvt, 0, sizeof(mapEvt));
    mapEvt.type = CTRL_LOBBY_MAP_CHANGE;
    serverSimPublishControl(sim, &mapEvt);
  }
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

  /* Run deferred removals for slots force-disconnected mid-publish (control
   * queue overflow). Done here, after recv processing and outside any
   * publish, so serverSimRemovePlayer can safely fan its events out. */
  transportUdpServerDrainPendingRemovals(sim);

  if (sim->state == serverStateRunning) {
    /* Run brain AI bots — queues two InputPackets per bot (keys + game) */
    if (serverSimGetNumBots(sim) > 0) {
      serverSimBotTick(sim, sim->botAiType);
    }
    /* Advance the sim by one 20ms frame.  serverSimTick internally runs
     * the keys-tick + game-tick pair and accumulates events from both
     * half-steps into a single frame's worth of state, so the prior
     * save/restore dance is no longer needed here.  The half-step split
     * is private to server_sim.c. */
    ServerState preTickState = sim->state;
    Uint64 simStart = SDL_GetPerformanceCounter();
    serverSimTick(sim);
    Uint64 simEnd = SDL_GetPerformanceCounter();
    /* If game ended during this tick, publish game-over events */
    if (preTickState == serverStateRunning && sim->state == serverStateGameOver) {
      /* Decide the win/exit message and WBN crediting. The policy lives in
       * the sim core (serverSimResolveGameOver) so the dedicated server and
       * the in-process SP/host both resolve a game over identically. */
      serverSimResolveGameOver(sim);
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
      /* No-lobby map rotation: a win boots everyone and restarts a fresh
       * round here, inside the tick, so sim->state leaves gameOver before
       * the main loop's exit check observes it — the server never quits. */
      if (serverSimIsMapRotateEnabled(sim)) {
        serverLifecycleRotateRound(sim);
      }
    }
    if (sim->state == serverStateRunning) {
      if (instanceAcceptRemoteClients) {
        transportUdpServerDrainEvents(sim);
      }
    }
    double simFreq = (double)SDL_GetPerformanceFrequency();
    double simMs = (double)(simEnd - simStart) * 1000.0 / simFreq;
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
                                 pi, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
            }
          }
        }
        /* Force a snapshot to every client in the same tick as the
         * RUNNING publish above. The running-state branch's periodic
         * transportUdpServerSend fires on the NEXT tick (~20ms), so
         * without this push the client receives CTRL_GAME_PHASE_RUNNING,
         * flips inLobby=false, and renders its (stale, round-1) MY_TANK
         * for a frame before the first authoritative snapshot lands.
         * serverSendSnapshot drains the per-client control queue into
         * the same packet, so RUNNING and the fresh tank state arrive
         * bundled — pairs with the client's hasPredictedTank=FALSE
         * reset on LOBBY to make that first snapshot run the init
         * branch (stocks, camera centre). */
        if (instanceAcceptRemoteClients) {
          transportUdpServerSend(sim);
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
      }
      /* End the round's WBN session, upload the round log against the
       * just-quit key (WBN rejects uploads to an active session), then
       * register a fresh session for the next round. The upload has to
       * sit between End and Begin — End sends server/quit so WBN will
       * accept the upload, Begin overwrites winboloNetServerKey with
       * the new round's key. handleGameOver already stashed the
       * round's filename when the GAME_OVER phase fired; Flush is a
       * no-op when there's nothing pending or when WBN is offline. */
      if (winbolonetIsRunning()) {
        winbolonetEndSession();
      }
      roundLogFlush();
      if (winbolonetIsRunning()) {
        serverSimRefreshWbnLobbyInfo(sim);
        winbolonetBeginSession(
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
      /* Close the rotation window: the new session's server_key is now
       * installed, so the deferred lobby_update (held dirty by
       * serverSimReturnToLobby's map pick) flushes against the right
       * key on the next WBN tick. Cleared unconditionally so a WBN-off
       * run doesn't leave the flag stuck. */
      sim->wbnSessionRotating = FALSE;
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
      /* Republish lobby state so every client's mirror reflects the
       * fresh lobby. serverSimReturnToLobby's contract says the caller
       * does this fan-out; CTRL_GAME_PHASE_LOBBY alone doesn't carry
       * the inLobby flag or per-slot data, so without these the host's
       * own UDP loopback ClientSim leaves cs->inLobby false and never
       * opens the lobby dialog — the window looks frozen because there
       * is no game view either. */
      serverSimPublishLobbySettings(sim);
      {
        BYTE pi;
        /* Republish EVERY slot, not just connected ones. A player who
         * left mid-round had their CTRL_LOBBY_SLOT suppressed — the
         * leave-time publish is gated to lobby/countdown state
         * (transport_udp_server.c PACKET_QUIT), so a running-state quit
         * never told clients to clear that slot. The client's lobbySlots
         * mirror is only mutated by CTRL_LOBBY_SLOT (CTRL_PLAYER_LEAVE is
         * chat-only), so without this the departed player lingers as a
         * ghost in the returning lobby. A vacant slot fills as
         * connected=false (serverSimFillLobbySlotEvent), which clears it. */
        for (pi = 0; pi < MAX_TANKS; pi++) {
          serverSimPublishLobbySlot(sim, pi);
        }
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
     * so this scan only matters when snapshots aren't flowing.
     *
     * Gate on udpServer.tickCount (always-advancing) rather than sim->tick
     * because sim->tick freezes during serverStateCountdown and
     * serverStateGameOver (see serverSimTick in server_sim.c).  A frozen
     * sim->tick whose residue mod 4 isn't 0 would silently disable
     * retransmit for the entire countdown / game-over window. */
    if (transportUdpServerGetTickCount() % 4 == 0) {
      transportUdpServerRetransmitUnackedControl();
    }

    /* Periodic lobby-slot republish so the ping column in the lobby
     * UI tracks live values instead of freezing between unrelated
     * slot changes (ready toggle, bot config, etc.). CTRL_LOBBY_SLOT
     * carries pingMs; without this heartbeat a quiet lobby shows the
     * value from whenever someone last clicked something. 250 ticks
     * at 50 Hz is ~5 s, well under the perceptible-staleness window
     * and far below the wire cost the queue can absorb. Skipped in
     * running state — snapshots already carry pingMs per tick there. */
    if (transportUdpServerGetTickCount() % 250 == 0 &&
        (sim->state == serverStateLobby ||
         sim->state == serverStateCountdown)) {
      BYTE pi;
      for (pi = 0; pi < MAX_TANKS; pi++) {
        if (sim->playerConnected[pi]) {
          serverSimPublishLobbySlot(sim, pi);
        }
      }
    }

    /* Timeout check — not called via transportUdpServerSend() during lobby */
    transportUdpServerCheckTimeouts(sim);
  }

  /* Auto-close / empty-rotation check — works in any state. The shared
   * serverSimCheckAutoClose latches hadPlayersEver and fires once the server
   * empties after having had players. In map-rotation mode an empty server
   * rotates to a fresh round instead of shutting down; otherwise -autoclose
   * forces a no-lobby shutdown (no players to return to a lobby for). */
  if ((sim->autoCloseOnEmpty || serverSimIsMapRotateEnabled(sim)) &&
      serverSimCheckAutoClose(sim)) {
    if (serverSimIsMapRotateEnabled(sim)) {
      serverSimConsoleMessage("Server empty — rotating to a new round.");
      serverLifecycleRotateRound(sim);
    } else {
      sim->lobbyEnabled = FALSE;
      serverSimEnterGameOver(sim);
    }
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
    /* Empty-reset bypasses serverSimReturnToLobby, so open the WBN
     * session-rotation window here before the map pick below reports
     * the next round's map. Cleared after BeginSession installs the
     * new key. */
    sim->wbnSessionRotating = TRUE;
    /* Empty-reset fires from the running state without going through
     * GAME_OVER, so handleGameOver never stashed the in-flight round.
     * Do it here so the upload below picks it up. */
    roundLogStash();
    serverSimResetGameWorld(sim);
    sim->state = serverStateLobby;
    sim->gameLength = sim->originalGameLength;
    sim->hadPlayersEver = FALSE;
    sim->emptyResetTicks = -1;
    /* Pick next map from rotation if mapdir is configured */
    if (sim->mapDirFiles != NULL) {
      serverSimMapDirPickRandom(sim);
    }
    /* End the WBN session, upload the round's log against the just-
     * quit key, then begin a new session for the next round. Same
     * sandwich as the game-over → lobby site (see comment there). */
    if (winbolonetIsRunning()) {
      winbolonetEndSession();
    }
    roundLogFlush();
    if (winbolonetIsRunning()) {
      winbolonetBeginSession(
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
    /* Close the rotation window — new key installed (or WBN off). */
    sim->wbnSessionRotating = FALSE;
    /* Empty-reset bypasses serverSimReturnToLobby, so the lobby phase
     * event is never published from the state machine. Publish it
     * explicitly so handleLobbyEnter fires and starts a fresh log for
     * the next round (mirroring serverSimReturnToLobby's tail). */
    {
      ControlEvent evt;
      memset(&evt, 0, sizeof(evt));
      serverSimFillGamePhaseEvent(sim, &evt);
      serverSimPublishControl(sim, &evt);
    }
    /* Push the freshly-reset map to every audience — same reasoning
     * as the serverSimReturnToLobby tail. The wire helper updates
     * the UDP server's cached compressed map / JOIN_ACCEPT / per-
     * client download tracking; the CTRL_LOBBY_MAP_CHANGE publish
     * fans through the bus so in-process subscribers reinstall via
     * boundServerSim. Empty-reset is usually a no-op for the wire
     * leg (no UDP clients at the moment the reset fires), but any
     * SP host or replay-log subscriber bound to this sim still
     * needs the event. */
    if (instanceAcceptRemoteClients) {
      transportUdpServerOnLobbyMapChange(sim);
    }
    {
      ControlEvent mapEvt;
      memset(&mapEvt, 0, sizeof(mapEvt));
      mapEvt.type = CTRL_LOBBY_MAP_CHANGE;
      serverSimPublishControl(sim, &mapEvt);
    }
  }

  threadsReleaseMutex();

  if (wbnTime > 100) {
    threadsWaitForMutex();
    /* Refresh the WBN lobby snapshot so server/update carries current
     * human/bot counts, then flush a deferred lobby_update if its rate
     * window has elapsed. */
    serverSimRefreshWbnLobbyInfo(sim);
    winbolonetServerUpdate(serverSimGetNumPlayers(sim), serverSimGetNumNeutralBases(sim), serverSimGetNumNeutralPills(sim), FALSE);
    serverSimWbnLobbyTick(sim);
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
  if (instanceAcceptRemoteClients) {
    mpDiagLogEnable(0);
  }
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
