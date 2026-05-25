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
#include <string.h>

#include "client_command.h"
#include "control_event.h"            /* ControlEvent, CTRL_CHAT, CTRL_ALLIANCE_REQUEST */
#include "log.h"
#include "netpacks.h"                 /* lobbyBotNameAcceptable */
#include "server_sim.h"
#include "server_sim_internal.h"      /* serverSimGameVoteToggle */
#include "server_sim_lifecycle.h"     /* serverSimSetTeam, lobbyAutoUnreadyOnChange */
#include "threads.h"
#include "transport_udp.h"            /* transportUdpServerGetPlayerName,
                                         transportUdpServerSetBotName */

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
    case CMD_READY: {
        if (!serverSimIsLobbyEnabled(sim)) return CMD_REJECT_BAD_STATE;
        /* Ranked-shape gate: silently drop ready=true that doesn't
         * qualify. No reject code — the client tooltip explains why. */
        if (cmd->u.ready.ready && serverSimGetRanked(sim) &&
            serverSimGetState(sim) == serverStateLobby &&
            !serverSimRankedShapeReady(sim)) {
            return CMD_OK;
        }
        if (serverSimGetState(sim) == serverStateLobby) {
            serverSimSetReady(sim, (BYTE)senderSlot, cmd->u.ready.ready);
            logAddEvent(cmd->u.ready.ready ? log_PlayerReady : log_PlayerUnready,
                        (BYTE)senderSlot, 0, 0, 0, 0, NULL);
            serverSimPublishLobbySlot(sim, (BYTE)senderSlot);
            serverSimLobbyCheckAllReady(sim);
            if (serverSimGetState(sim) == serverStateCountdown) {
                logAddEvent(log_CountdownStart, 0, 0, 0, 0, 0, NULL);
            }
        } else if (serverSimGetState(sim) == serverStateCountdown &&
                   !cmd->u.ready.ready) {
            serverSimSetReady(sim, (BYTE)senderSlot, false);
            serverSimAbortCountdown(sim);
            logAddEvent(log_PlayerUnready, (BYTE)senderSlot, 0, 0, 0, 0, NULL);
            logAddEvent(log_CountdownCancel, 0, 0, 0, 0, 0, NULL);
            serverSimConsoleMessage("Countdown cancelled — player unreadied.");
            serverSimPublishLobbySlot(sim, (BYTE)senderSlot);
        }
        /* Other states (Running, GameOver): silent drop, no state
         * change. */
        return CMD_OK;
    }
    case CMD_LOBBY_BOT_CONFIG: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbyBotConfig *p = &cmd->u.lobbyBotConfig;
        if (p->slot >= MAX_TANKS || p->difficulty > 2 || p->personality > 3 ||
            p->nameLen > 31 || !serverSimIsBot(sim, p->slot)) {
            return CMD_REJECT_INVALID;
        }
        char validatedName[PACKET_MAX_PLAYER_NAME];
        if (p->nameLen > 0) {
            char rawName[PACKET_MAX_PLAYER_NAME];
            memset(rawName, 0, sizeof(rawName));
            memcpy(rawName, p->name, p->nameLen);
            if (!lobbyBotNameAcceptable(rawName, validatedName,
                                        sizeof(validatedName), (int)p->slot,
                                        transportUdpServerGetPlayerName,
                                        NULL, NULL)) {
                return CMD_REJECT_INVALID;
            }
            transportUdpServerSetBotName(p->slot, validatedName);
        }
        serverSimSetBotConfig(sim, p->slot, p->difficulty, p->personality,
                              p->nameLen > 0 ? validatedName : NULL);
        return CMD_OK;
    }
    case CMD_LOBBY_TEAM_META: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbyTeamMeta *p = &cmd->u.lobbyTeamMeta;
        if (p->teamId == 0 || p->teamId >= MAX_TANKS ||
            p->nameLen > LOBBY_TEAM_NAME_LEN - 1) {
            return CMD_REJECT_INVALID;
        }
        serverSimSetTeamMeta(sim, p->teamId, p->color, p->namingPool,
                             (const uint8_t *)p->name, p->nameLen);
        return CMD_OK;
    }
    case CMD_LOBBY_TEAM_CLEAR: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        if (cmd->u.lobbyTeamClear.teamId == 0 ||
            cmd->u.lobbyTeamClear.teamId >= MAX_TANKS) {
            return CMD_REJECT_INVALID;
        }
        serverSimClearTeamMeta(sim, cmd->u.lobbyTeamClear.teamId);
        return CMD_OK;
    }
    case CMD_LOBBY_SETTING: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbySetting *p = &cmd->u.lobbySetting;
        if (p->valueLen > 32) return CMD_REJECT_INVALID;
        uint16_t lockBit = serverSimGetSettingLockBit(p->settingType);
        if (lockBit == 0xFFFFu) {
            /* Unknown setting — silent forward-compat drop. */
            return CMD_OK;
        }
        if (lockBit != 0u &&
            (serverSimGetServerLocks(sim) & lockBit) != 0u) {
            return CMD_REJECT_LOCKED;
        }
        if (!serverSimApplyLobbySetting(sim, p->settingType, p->value,
                                        p->valueLen)) {
            return CMD_REJECT_INVALID;
        }
        return CMD_OK;
    }
    case CMD_CHAT: {
        const CmdChat *p = &cmd->u.chat;
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_CHAT;
        evt.u.chat.fromPlayer = (BYTE)senderSlot;
        evt.u.chat.destPlayer = p->destPlayer;
        evt.u.chat.bodyLen    = p->bodyLen;
        if (p->bodyLen > 0) {
            memcpy(evt.u.chat.body, p->body, p->bodyLen);
        }
        serverSimPublishControl(sim, &evt);
        {
            char pstr[256];
            int pLen = p->bodyLen;
            if (pLen > 255) pLen = 255;
            pstr[0] = (char)pLen;
            memcpy(pstr + 1, p->body, pLen);
            if (p->destPlayer == 0xFF) {
                logAddEvent(log_MessageAll, (BYTE)senderSlot, 0, 0, 0, 0, pstr);
            } else {
                logAddEvent(log_MessagePlayers, (BYTE)senderSlot,
                            p->destPlayer, 0, 0, 0, pstr);
            }
        }
        return CMD_OK;
    }
    case CMD_ALLIANCE_REQUEST: {
        if (serverSimGetRanked(sim)) return CMD_REJECT_BAD_STATE;
        const CmdAllianceRequest *p = &cmd->u.allianceRequest;
        if (p->toPlayer >= MAX_TANKS) return CMD_REJECT_INVALID;
        if (!serverSimIsPlayerConnected(sim, p->toPlayer)) {
            return CMD_REJECT_INVALID;  /* silent on wire today; preserved */
        }
        logAddEvent(log_AllyRequest, (BYTE)senderSlot, p->toPlayer,
                    0, 0, 0, NULL);
        ControlEvent reqEvt;
        memset(&reqEvt, 0, sizeof(reqEvt));
        reqEvt.type = CTRL_ALLIANCE_REQUEST;
        reqEvt.u.allianceRequest.fromPlayer = (BYTE)senderSlot;
        reqEvt.u.allianceRequest.toPlayer   = p->toPlayer;
        serverSimPublishControl(sim, &reqEvt);
        return CMD_OK;
    }
    case CMD_ALLIANCE_ACCEPT: {
        serverSimAcceptAlliance(sim, (BYTE)senderSlot,
                                cmd->u.allianceAccept.newMember);
        return CMD_OK;
    }
    case CMD_ALLIANCE_LEAVE: {
        serverSimLeaveAlliance(sim, (BYTE)senderSlot);
        return CMD_OK;
    }
    case CMD_GAME_VOTE_TOGGLE: {
        serverSimGameVoteToggle(sim, (uint8_t)senderSlot,
                                cmd->u.gameVoteToggle.kind,
                                cmd->u.gameVoteToggle.toggleMode);
        return CMD_OK;
    }
    case CMD_NONE:
    default:
        return CMD_REJECT_BAD_STATE;
    }
}
