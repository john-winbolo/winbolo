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

/* BrainTest links server_sim_static but not server_static, so the real
 * serverInstanceStartup body (transport_udp_server / WBN / NAT) is not
 * available. This file provides a local-only equivalent: apply the
 * cfg-driven setters to the sim and skip everything else. The single
 * BrainTest caller passes acceptRemoteClients=false, so the network
 * paths from the real implementation are dead branches anyway. */

#include <stdbool.h>
#include "server_sim.h"
#include "../server/server_lifecycle.h"

bool serverInstanceStartup(ServerSim *sim, const ServerInstanceConfig *cfg) {
  if (sim == NULL || cfg == NULL) return false;
  serverSimSetEmptyResetEnabled(sim, cfg->emptyResetEnabled);
  serverSimSetHasPassword(sim, cfg->hasPassword);
  if (cfg->botBrainPath != NULL) {
    serverSimSetBotBrainPath(sim, cfg->botBrainPath);
  }
  if ((aiType)cfg->botAiType != aiNone) {
    serverSimSetBotAiType(sim, (aiType)cfg->botAiType);
  }
  serverSimSetAutoLockOnGameStart(sim,
      cfg->autoLockOnGameStart || cfg->ranked);
  serverSimSetRanked(sim, cfg->ranked);
  serverSimSetOpenHost(sim, cfg->openHost);
  serverSimSetServerLocks(sim, cfg->serverLocks);
  if (cfg->skipLobby) {
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
  } else if (cfg->lobbyEnabled) {
    serverSimSetLobbyEnabled(sim, true);
    serverSimEnterLobby(sim);
  }
  return true;
}

void serverInstanceTick(ServerSim *sim) { (void)sim; }
void serverInstanceShutdown(ServerSim *sim) { (void)sim; }
