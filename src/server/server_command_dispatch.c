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

#include <assert.h>

#include "client_command.h"
#include "log.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"     /* serverSimSetTeam, lobbyAutoUnreadyOnChange */
#include "threads.h"

/* Authority gate shared by the command dispatcher (this TU) and the
 * lobby command handlers in transport_udp_server.c, where the function
 * is defined. */
extern bool lobbyClientMayEdit(ServerSim *sim, int clientIdx);

CmdResult serverSimApplyCommand(ServerSim *sim, int senderSlot,
                                const ClientCommand *cmd) {
    assert(threadsCurrentlyHoldsMutex());
    switch (cmd->type) {
    case CMD_TEAM_SET: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (cmd->u.teamSet.slot >= MAX_TANKS ||
            cmd->u.teamSet.team >= MAX_TANKS) {
            return CMD_REJECT_INVALID;
        }
        if ((int)cmd->u.teamSet.slot != senderSlot &&
            !lobbyClientMayEdit(sim, senderSlot)) {
            return CMD_REJECT_NOT_HOST;
        }
        serverSimSetTeam(sim, cmd->u.teamSet.slot, cmd->u.teamSet.team);
        logAddEvent(log_TeamSet,
                    cmd->u.teamSet.slot, cmd->u.teamSet.team,
                    0, 0, 0, NULL);
        serverSimPublishLobbySlot(sim, cmd->u.teamSet.slot);
        lobbyAutoUnreadyOnChange(sim);
        return CMD_OK;
    }
    case CMD_NONE:
    default:
        return CMD_REJECT_BAD_STATE;
    }
}
