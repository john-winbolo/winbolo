/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * server_stubs_wasm.c — wasm-only stubs for server-frontend symbols
 *
 * server_sim.c and the shared lobby dialog are linked into the wasm
 * client but reference symbols that only exist in the real server
 * frontend (servermain.c) or in libplum-backed code paths that the
 * wasm build never exercises:
 *
 *   - isLogging / dontSendLog / makeLogFileName: server-side game log
 *     writing. The wasm client never hosts a server log, so the
 *     globals stay FALSE and logStart() is fed an empty filename so
 *     it fails cleanly.
 *
 *   - serverDedicatedLogLastRoundFile: the real body is in
 *     server_dedicated_log.c (server_static), which the wasm client
 *     doesn't link. The lobby's recap reel asks it for a round this
 *     process recorded itself, ahead of the copy the server sends; the
 *     wasm client never hosts, so there is never one, and "" is the
 *     answer the header already defines for that.
 *
 *   - serverInstanceGetPortmapInfo / TriggerManualProbe /
 *     GetManualProbeState / IsNatPunchActive: NAT/UPnP probe state owned
 *     by the server-instance lifecycle. The wasm client never hosts a
 *     server, so report idle/empty/inactive state.
 *
 *   - serverInstanceStartup / Tick / Shutdown: the real bodies live in
 *     server_lifecycle.c (server_static), which the wasm client doesn't
 *     link. Single-player passes acceptRemoteClients=false, so the only
 *     live work startup does is serverSimApplyInstanceConfig; the
 *     transport / WBN / NAT branches are dead. Mirrors the iOS / Android
 *     / BrainTest local-only lifecycle stubs. The tick runs the lobby and
 *     round steps a local game needs (server_sim_lifecycle_local.c) for
 *     the practice game, whose server the page ticks.
 */

#include <stddef.h>
#include <string.h>
#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimApplyInstanceConfig, the
                                    * serverSimLocal* round steps */
#include "server_lifecycle.h"
#include "server_dedicated_log.h"

bool isLogging = FALSE;
bool dontSendLog = TRUE;

void makeLogFileName(char *outFileName, const char *mapName) {
  (void)mapName;
  if (outFileName) outFileName[0] = '\0';
}

const char *serverDedicatedLogLastRoundFile(void) {
  return "";
}

void serverInstanceGetPortmapInfo(ServerPortmapInfo *out) {
  if (out) memset(out, 0, sizeof(*out));
}

void serverInstanceTriggerManualProbe(void) {
}

ManualProbeState serverInstanceGetManualProbeState(void) {
  return MANUAL_PROBE_IDLE;
}

bool serverInstanceIsNatPunchActive(void) {
  return FALSE;
}

bool serverInstanceStartup(ServerSim *sim, const ServerInstanceConfig *cfg) {
  if (sim == NULL || cfg == NULL) return FALSE;
  serverSimApplyInstanceConfig(sim, cfg);
  return TRUE;
}

/* Ticks run, counted at the top of each call the way the UDP server counts
 * its receive passes. The lobby-slot heartbeat reads it. */
static uint32_t s_localTickCount = 0;

/* The browser client's local server tick: the same lobby and round steps as
 * serverInstanceTick in server_lifecycle.c, in the same order and under the
 * same conditions, without the network server around them (no UDP, no
 * WinBolo.net, no tracker or NAT, no brain-record sessions) and without the
 * dedicated server's options (map rotation, -mapdir, auto-close, empty
 * reset). main_wasm.c calls it once per 20 ms tick for a game whose local
 * transport does not tick the server itself. The web's threads are
 * single-threaded no-ops, so there is no lock to take. */
void serverInstanceTick(ServerSim *sim) {
  if (sim == NULL) return;
  s_localTickCount++;

  if (serverSimGetState(sim) == serverStateRunning) {
    ServerState preTickState;
    serverSimLocalBotTick(sim);
    preTickState = serverSimGetState(sim);
    serverSimTick(sim);
    if (preTickState == serverStateRunning &&
        serverSimGetState(sim) == serverStateGameOver) {
      serverSimLocalOnGameOver(sim);
    }
  } else {
    /* Lobby/countdown/gameover: single tick for state machine processing */
    ServerState preTickState = serverSimGetState(sim);
    serverSimTick(sim);
    serverSimLocalPublishBalanceProposal(sim);
    if (preTickState == serverStateCountdown) {
      if (serverSimGetState(sim) == serverStateRunning) {
        serverSimLocalOnGameStart(sim);
      } else {
        serverSimLocalCountdownTick(sim);
      }
    }
    if (preTickState == serverStateGameOver &&
        serverSimGetState(sim) == serverStateLobby) {
      serverSimLocalOnReturnToLobby(sim);
    }
    serverSimLocalLobbySlotHeartbeat(sim, s_localTickCount);
    if (serverSimGetState(sim) == serverStateLobby ||
        serverSimGetState(sim) == serverStateCountdown) {
      serverSimFlushBotConfigPublishes(sim);
    }
  }
}

void serverInstanceShutdown(ServerSim *sim) { (void)sim; }
