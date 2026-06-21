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
 *     / BrainTest local-only lifecycle stubs.
 */

#include <stddef.h>
#include <string.h>
#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimApplyInstanceConfig */
#include "server_lifecycle.h"

bool isLogging = FALSE;
bool dontSendLog = TRUE;

void makeLogFileName(char *outFileName, const char *mapName) {
  (void)mapName;
  if (outFileName) outFileName[0] = '\0';
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

void serverInstanceTick(ServerSim *sim) { (void)sim; }
void serverInstanceShutdown(ServerSim *sim) { (void)sim; }
