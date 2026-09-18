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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "global.h"
#include "server_lifecycle.h"

void serverInstanceRecordProbeReply(const char *reflexiveIp,
                                    unsigned short reflexivePort) {
  (void)reflexiveIp;
  (void)reflexivePort;
}

/* server_sim.c references these from serverSimInformation. The full
 * implementations live in server_lifecycle.c, which BrainTest doesn't
 * link — report zero so the diagnostic prints fall through cleanly. */
void serverLifecycleGetTickStats(double *outLastMs, double *outEwmaMs) {
  if (outLastMs) *outLastMs = 0.0;
  if (outEwmaMs) *outEwmaMs = 0.0;
}

void serverLifecycleGetSimStats(double *outLastMs, double *outEwmaMs) {
  if (outLastMs) *outLastMs = 0.0;
  if (outEwmaMs) *outEwmaMs = 0.0;
}

/* The round's worst tick and its over-budget count, same story:
 * server_sim_round.c reads the pair in serverSimInformation and clears it at
 * each round start, and neither call has anything to measure on a binary
 * that never runs the tick loop. */
void serverLifecycleGetTickPeak(double *outPeakMs,
                                unsigned int *outOverBudget) {
  if (outPeakMs) *outPeakMs = 0.0;
  if (outOverBudget) *outOverBudget = 0;
}

void serverLifecycleResetTickPeak(void) {
}

/* server_sim.c references these. The real implementations live in
 * transport_udp_server.c (server_static), which the non-server-static
 * targets compiling this file (WinBoloIOS, BrainTest, MapEditor,
 * android main, wasm winbolo) don't link — they don't run a UDP
 * server, so the calls are dead paths that just need to link.
 * transportUdpServerGetPlayerName returns NULL per the
 * lobbyBotNameAcceptable contract: NULL means "no player in this
 * slot," which the validator skips during uniqueness checks. */
struct ServerSim;

/* server_command_dispatch.c (in server_sim_static) calls
 * lobbyClientMayEdit to gate other-slot lobby commands. The real
 * implementation lives in transport_udp_server.c (server_static),
 * which non-server-static binaries don't link. They always run as
 * an in-process host with full authority over their own sim, so the
 * stub permits unconditionally — matches slot 0's path in the real
 * helper. */
bool lobbyClientMayEdit(struct ServerSim *sim, int clientIdx) {
  (void)sim; (void)clientIdx;
  return true;
}

void transportUdpServerEnforcePing(struct ServerSim *sim) { (void)sim; }
void transportUdpServerOnLobbyMapChange(struct ServerSim *sim) { (void)sim; }
void transportUdpServerOnGameStart(struct ServerSim *sim) { (void)sim; }
void transportUdpServerSetLock(struct ServerSim *sim, bool locked) { (void)sim; (void)locked; }
uint16_t transportUdpServerGetClientPing(BYTE playerNum) { (void)playerNum; return 0; }
const char *transportUdpServerGetPlayerName(BYTE playerNum) { (void)playerNum; return NULL; }
/* server_command_dispatch.c's CMD_LOBBY_BOT_CONFIG arm calls this
 * to update the connected-player table's bot name. The non-server
 * binaries don't run that table; the stub is a no-op. */
void transportUdpServerSetBotName(BYTE playerNum, const char *name) {
  (void)playerNum; (void)name;
}
/* server_command_dispatch.c's CMD_LOBBY_KICK arm asks the UDP
 * server to close the kicked player's connection. The non-server
 * binaries don't run that table; the stub is a no-op. */
void transportUdpServerKickPlayer(struct ServerSim *sim, const char *name) {
  (void)sim; (void)name;
}
/* server_command_dispatch.c's CMD_PLAYER_MUTE arm records the mute in
 * the UDP server's per-client table. The non-server binaries have no
 * such table — and no remote voice or chat to gate — so the stub is a
 * no-op. */
void transportUdpServerSetVoiceMute(BYTE clientSlot, BYTE targetPlayer,
                                    bool muted) {
  (void)clientSlot; (void)targetPlayer; (void)muted;
}
/* server_command_dispatch.c calls these for WBN matchmaking (balance,
 * ranked-gated) and re-authentication (WBN-gated). The non-server
 * binaries never run the server command dispatcher, so these are never
 * invoked there; the symbols still need definitions for the link. */
bool transportUdpServerStartBalanceRequest(struct ServerSim *sim,
                                           uint8_t teamSize,
                                           bool includeBots) {
  (void)sim; (void)teamSize; (void)includeBots;
  return false;
}
void transportUdpServerHandleWbnReauth(struct ServerSim *sim,
                                       BYTE slot,
                                       const char *token) {
  (void)sim; (void)slot; (void)token;
}
const char *transportUdpServerGetClientCountryCode(BYTE playerNum) { (void)playerNum; return ""; }
uint8_t transportUdpServerGetClientType(BYTE playerNum) { (void)playerNum; return 0; /* CLIENT_TYPE_UNKNOWN */ }
bool transportUdpServerHasAnyClient(void) { return false; }
bool transportUdpServerGetClientAddrStr(BYTE playerNum, char *out, size_t outLen) {
  (void)playerNum; (void)out; (void)outLen; return false;
}
