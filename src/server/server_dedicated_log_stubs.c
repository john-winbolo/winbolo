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

/* No-op bodies for binaries that do not own the dedicated-server log
 * state (everything except WinBoloDS). server_sim.c calls these
 * unconditionally; the real implementations live in
 * server_dedicated_log.c. */

#include "server_dedicated_log.h"

void serverDedicatedLogOnEnterGameOver(ServerSim *sim) { (void)sim; }
void serverDedicatedLogOnReturnToLobby(ServerSim *sim) { (void)sim; }
void serverDedicatedLogOnLobbyExit(ServerSim *sim) { (void)sim; }
