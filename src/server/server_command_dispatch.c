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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bot_manager.h"              /* botManagerSetBrainIdx, botManagerAddBot */
#include "client_command.h"
#include "control_event.h"            /* ControlEvent, CTRL_CHAT, CTRL_ALLIANCE_REQUEST */
#include "log.h"
#include "mapgen.h"                   /* MapGenConfig, mapGenSeedToConfig */
#include "netpacks.h"                 /* lobbyBotNameAcceptable */
#include "player_flags.h"             /* PLAYER_FLAG_ADMIN */
#include "players.h"                  /* playersGetClientFlags */
#include "playername_validate.h"      /* playerNameValidate, playerNameCompare */
#include "server_sim.h"
#include "server_sim_internal.h"      /* serverSimGameVoteToggle */
#include "server_sim_join.h"          /* serverSimAssignLobbyStartOnJoin, serverSimLobbyStartSideMask, serverSimLobbyClosedMaskFor; serverSimFindFreeSlot */
#include "../server/scenario.h"       /* scenarioIsActive — enemy-team slot pick */
#include "server_sim_lifecycle.h"     /* serverSimSetTeam, lobbyAutoUnreadyOnChange */
#include "start_sides.h"              /* startSideEligible — the claim command's side check */
#include "threads.h"
#include "../common/wb_log.h"
#include "transport_udp.h"            /* transportUdpServerGetPlayerName,
                                         transportUdpServerSetBotName,
                                         transportUdpServerKickPlayer */
#include "../winbolonet/winbolonet_server.h" /* winboloNetIsPlayerParticipant */
#include "../winbolonet/winbolonet_core.h"   /* winbolonetIsRunning */

/* Authority gate shared by the command dispatcher (this TU) and the
 * lobby command handlers in transport_udp_server.c, where the function
 * is defined. */
extern bool lobbyClientMayEdit(ServerSim *sim, int clientIdx);

/* The START_SIDE_* choice of a slot's team; a slot on team 0 has no side. */
static BYTE lobbySlotStartSide(const ServerSim *sim, BYTE slot) {
    const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, slot);
    BYTE t = lp ? lp->teamNumber : 0;
    if (t == 0 || t >= MAX_TANKS) return START_SIDE_ANY;
    return sim->teams[t].startSide;
}

/* Whether a slot may hold a 1-based start under the side rules: the same
 * question the lobby pick, the map-change release and the batch placement
 * ask, so a claim can never land a slot on a start the next lobby event
 * would take away again. A side team takes what its side accepts; a slot
 * with no side stays off the sides the other teams present chose. */
static bool lobbySlotMayHoldStart(ServerSim *sim, BYTE slot, BYTE idx1) {
    return startSideEligible(serverSimLobbyStartSideMask(sim, idx1),
                             lobbySlotStartSide(sim, slot),
                             serverSimLobbyClosedMaskFor(sim, slot));
}

static CmdResult applyCommandInner(ServerSim *sim, int senderSlot,
                                   const ClientCommand *cmd) {
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
    case CMD_LOBBY_CLAIM_START: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        const CmdLobbyClaimStart *p = &cmd->u.lobbyClaimStart;
        BYTE target = p->targetSlot;
        BYTE idx    = p->startIdx;
        BYTE numStarts = startsGetNumStarts(&sim->sim.ss);
        if (target >= MAX_TANKS || !serverSimIsPlayerConnected(sim, target)) {
            return CMD_REJECT_INVALID;
        }
        if (idx != 0xFF && idx != START_CLAIM_TEAM_SIDE &&
            (idx < 1 || idx > numStarts)) {
            return CMD_REJECT_INVALID;
        }
        bool isHost = lobbyClientMayEdit(sim, senderSlot);
        if ((int)target != senderSlot && !isHost) {
            return CMD_REJECT_NOT_HOST;
        }
        /* A player picking for themselves may take only a start the side
         * rules let their slot hold; the host may hand anyone any start. */
        if (!isHost && idx != 0xFF && idx != START_CLAIM_TEAM_SIDE &&
            !lobbySlotMayHoldStart(sim, target, idx)) {
            return CMD_REJECT_INVALID;
        }
        if (idx == START_CLAIM_TEAM_SIDE) {
            /* Team side: drop the reservation and pick a fresh start on
             * the slot's side at once; 0xFF when the side is full. */
            serverSimSetLobbyStartIdx(sim, target, 0xFF);
            serverSimAssignLobbyStartOnJoin(sim, target);
            serverSimPublishLobbySlot(sim, target);
            lobbyAutoUnreadyOnChange(sim);
            return CMD_OK;
        }
        /* Connected slot currently holding idx (none when idx == 0xFF). */
        BYTE holder = 0xFF;
        if (idx != 0xFF) {
            for (BYTE k = 0; k < MAX_TANKS; k++) {
                if (!serverSimIsPlayerConnected(sim, k)) continue;
                const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, k);
                if (lp && lp->startIdx == idx) { holder = k; break; }
            }
        }
        if (holder != 0xFF && holder != target) {
            /* Non-host may not take a start someone else holds. */
            if (!isHost) return CMD_REJECT_INVALID;
            const LobbyPlayer *tlp = serverSimGetLobbyPlayer(sim, target);
            BYTE oldTarget = tlp ? tlp->startIdx : 0xFF;
            serverSimSetLobbyStartIdx(sim, target, idx);
            serverSimSetLobbyStartIdx(sim, holder, oldTarget);
            /* The displaced holder inherits the assignee's old start. When
             * that is none, or one the holder's side rules reject, pick the
             * holder a fresh start now instead. */
            if (oldTarget == 0xFF || !lobbySlotMayHoldStart(sim, holder, oldTarget)) {
                serverSimAssignLobbyStartOnJoin(sim, holder);
            }
            serverSimPublishLobbySlot(sim, target);
            serverSimPublishLobbySlot(sim, holder);
        } else {
            /* Free start, release, or already mine. */
            serverSimSetLobbyStartIdx(sim, target, idx);
            serverSimPublishLobbySlot(sim, target);
        }
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
                             p->startSide,
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
        BYTE dest = p->destPlayer;
        /* Reject out-of-range high values: anything >= 0x80 that is
         * neither broadcast (0xFF), the server-message sentinel (0xFE),
         * nor a valid team address (0x81..0x90). */
        if (dest >= 0x80 && dest != 0xFF && dest != 0xFE
            && CHAT_DEST_IS_TEAM(dest) == 0) {
            return CMD_REJECT_INVALID;
        }
        if (CHAT_DEST_IS_TEAM(dest)) {
            const LobbyPlayer *sender =
                serverSimGetLobbyPlayer(sim, (BYTE)senderSlot);
            BYTE senderTeam = sender ? sender->teamNumber : 0;
            if (senderTeam == 0 || CHAT_DEST_TEAM_OF(dest) != senderTeam) {
                return CMD_REJECT_INVALID;  /* not your team / unassigned phantom */
            }
        }
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
    case CMD_RATING_POSTED: {
        const CmdRatingPosted *p = &cmd->u.ratingPosted;
        if (p->key[0] == '\0') return CMD_REJECT_INVALID;
        /* The server has no view of WinBolo.net, so the key is passed
         * through unchecked; each receiving client compares it against the
         * round its own recap is showing. */
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_ROUND_RATING_POSTED;
        evt.u.ratingPosted.fromPlayer = (BYTE)senderSlot;
        memcpy(evt.u.ratingPosted.key, p->key, sizeof(evt.u.ratingPosted.key));
        evt.u.ratingPosted.key[sizeof(evt.u.ratingPosted.key) - 1] = '\0';
        serverSimPublishControl(sim, &evt);
        return CMD_OK;
    }
    case CMD_VIEW_STATE: {
        /* Range check only, in any server state. The claim decides nothing by
         * itself, so one that names a target this server does not have is
         * degraded to the tank view rather than rejected — a client that was
         * viewing through a pill when it died would otherwise collect a
         * reject for a view it has already left. Whether the target still
         * qualifies (allied, alive, not carried, policy) is decided by the
         * viewport builder and serverSimValidateViewTargets. */
        uint8_t kind   = cmd->u.viewState.kind;
        uint8_t target = cmd->u.viewState.target;
        bool inRange = false;
        switch (kind) {
        case VIEW_KIND_PILL:
            inRange = (sim->sim.pb != NULL &&
                       target < pillsGetNumPills(&sim->sim.pb));
            break;
        case VIEW_KIND_BASE:
            inRange = (sim->sim.bs != NULL &&
                       target < basesGetNumBases(&sim->sim.bs));
            break;
        case VIEW_KIND_ALLY:
            inRange = (target < MAX_TANKS && target != (uint8_t)senderSlot);
            break;
        default:
            break;  /* tank, or a kind this server does not know */
        }
        if (!inRange) {
            kind   = VIEW_KIND_TANK;
            target = 0;
        }
        sim->viewKind[senderSlot]   = kind;
        sim->viewTarget[senderSlot] = target;
        return CMD_OK;
    }
    case CMD_VIEW_CYCLE: {
        /* The server picks rather than the client because the client only
         * knows who is watchable from the snapshots it has been sent, and
         * under viewPolicyKey that is exactly the state the policy withholds:
         * left to itself the client would only ever reach allies it had
         * already seen. Pill and base selection stays client-side, so any
         * kind other than ally is accepted and answered with nothing — the
         * field is there so they could move here later. */
        BYTE target = 0, mapX = 0, mapY = 0;
        bool found;
        ControlEvent evt;

        if (cmd->u.viewCycle.kind != VIEW_KIND_ALLY) {
            return CMD_OK;
        }

        found = serverSimPickAlly(sim, (BYTE)senderSlot,
                                  cmd->u.viewCycle.direction,
                                  cmd->u.viewCycle.from,
                                  &target, &mapX, &mapY);
        if (found) {
            sim->viewKind[senderSlot]   = VIEW_KIND_ALLY;
            sim->viewTarget[senderSlot] = target;
        }
        /* Nothing found leaves the stored view alone: the client decides what
         * to do with the answer, and serverSimValidateViewTargets already
         * clears a target that has stopped qualifying. */

        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_VIEW_TARGET;
        evt.u.viewTarget.origSlot = (BYTE)senderSlot;
        evt.u.viewTarget.kind     = VIEW_KIND_ALLY;
        evt.u.viewTarget.fromEcho = cmd->u.viewCycle.from;
        evt.u.viewTarget.found    = found ? 1 : 0;
        if (found) {
            evt.u.viewTarget.target = target;
            evt.u.viewTarget.mapX   = mapX;
            evt.u.viewTarget.mapY   = mapY;
        }
        serverSimPublishControl(sim, &evt);
        /* Nothing to watch is an answer, not a rejection — a reject would
         * reach the player as an error. */
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
    case CMD_LOBBY_REMOVE_BOT: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        if (cmd->u.lobbyRemoveBot.slot >= MAX_TANKS ||
            !serverSimIsBot(sim, cmd->u.lobbyRemoveBot.slot)) {
            return CMD_REJECT_INVALID;
        }
        serverSimRemoveBot(sim, cmd->u.lobbyRemoveBot.slot);
        return CMD_OK;
    }
    case CMD_LOBBY_SET_BOT_BRAIN: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbySetBotBrain *p = &cmd->u.lobbySetBotBrain;
        if (p->slot >= MAX_TANKS || !serverSimIsBot(sim, p->slot) ||
            serverSimGetBrainPathForIdx(sim, p->brainIdx) == NULL) {
            return CMD_REJECT_INVALID;
        }
        serverSimSetBotBrainIdxFor(sim, p->slot, p->brainIdx);
        botManagerSetBrainIdx(sim, p->slot, p->brainIdx);
        return CMD_OK;
    }
    case CMD_LOBBY_OPEN_HOST: {
        /* Host-only: the toggle that enables openHost cannot be
         * gated through openHost itself. */
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_OPEN_HOST) {
            return CMD_REJECT_LOCKED;
        }
        serverSimSetOpenHost(sim, cmd->u.lobbyOpenHost.openHost);
        return CMD_OK;
    }
    case CMD_MAP_SKIP_VOTE: {
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
            return CMD_REJECT_LOCKED;
        }
        serverSimMapSkipVoteToggle(sim, (uint8_t)senderSlot);
        return CMD_OK;
    }
    case CMD_NAME_CHANGE: {
        const CmdNameChange *p = &cmd->u.nameChange;
        if (winboloNetIsPlayerParticipant((BYTE)senderSlot)) {
            return CMD_REJECT_BAD_STATE;
        }
        char validated[PACKET_MAX_PLAYER_NAME];
        PlayerNameValidationError vErr = PLAYER_NAME_OK;
        if (!playerNameValidate(p->newName, validated, sizeof(validated), &vErr)) {
            switch (vErr) {
                case PLAYER_NAME_ERR_EMPTY:
                    return CMD_REJECT_NAME_EMPTY;
                case PLAYER_NAME_ERR_RESERVED_PREFIX:
                    return CMD_REJECT_NAME_RESERVED_PREFIX;
                case PLAYER_NAME_ERR_RESERVED_SUFFIX:
                    return CMD_REJECT_NAME_RESERVED_SUFFIX;
                case PLAYER_NAME_ERR_MIXED_SCRIPTS:
                    return CMD_REJECT_NAME_MIXED_SCRIPTS;
                case PLAYER_NAME_ERR_INVALID_UTF8:
                case PLAYER_NAME_ERR_DISALLOWED_CHAR:
                case PLAYER_NAME_ERR_TOO_LONG:
                default:
                    return CMD_REJECT_NAME_INVALID;
            }
        }
        /* Uniqueness check via the connected-player table. On UDP that's
         * udpServer.clients[]; the stub on SP returns NULL for every
         * slot (so SP-host effectively skips the check). */
        for (int j = 0; j < MAX_TANKS; j++) {
            if (j == senderSlot) continue;
            const char *other = transportUdpServerGetPlayerName((BYTE)j);
            if (other != NULL && playerNameCompare(other, validated) == 0) {
                return CMD_REJECT_NAME_TAKEN;
            }
        }
        if (validated[0] != '\0') {
            const char *oldName = transportUdpServerGetPlayerName((BYTE)senderSlot);
            char msg[30 + 2 * PACKET_MAX_PLAYER_NAME];
            snprintf(msg, sizeof(msg), "Player '%s' changed name to '%s'.",
                     oldName != NULL ? oldName : "?", validated);
            serverSimConsoleMessage(msg);
            transportUdpServerSetBotName((BYTE)senderSlot, validated);
            serverSimSetPlayerName(sim, (BYTE)senderSlot, validated);
        }
        return CMD_OK;
    }
    case CMD_LOCK_TOGGLE: {
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        bool allow = cmd->u.lockToggle.allow;
        if (serverSimIsAcceptingJoins(sim) == allow) return CMD_OK;  /* no-op */
        serverSimSetAllowNewPlayers(sim, allow);
        return CMD_OK;
    }
    case CMD_LOBBY_ADD_BOT: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby ||
            serverSimGetBotAiType(sim) == aiNone ||
            serverSimGetBotBrainPath(sim)[0] == '\0') {
            fprintf(stderr, "ADD_BOT reject BAD_STATE: lobbyEnabled=%d state=%d aiType=%d brain='%s'\n",
                    (int)serverSimIsLobbyEnabled(sim), (int)serverSimGetState(sim),
                    (int)serverSimGetBotAiType(sim), serverSimGetBotBrainPath(sim));
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) {
            fprintf(stderr, "ADD_BOT reject NOT_HOST: senderSlot=%d\n", (int)senderSlot);
            return CMD_REJECT_NOT_HOST;
        }
        /* Enforce the operator-configured -maxbots cap (0 = no cap). */
        BYTE maxBots = serverSimGetMaxBots(sim);
        if (maxBots > 0 && serverSimGetLobbyBotCount(sim) >= maxBots) {
            fprintf(stderr, "ADD_BOT reject BOT_LIMIT: count=%d max=%d\n",
                    (int)serverSimGetLobbyBotCount(sim), (int)maxBots);
            return CMD_REJECT_BOT_LIMIT;
        }
        const CmdLobbyAddBot *p = &cmd->u.lobbyAddBot;
        char validatedName[PACKET_MAX_PLAYER_NAME];
        bool haveName = (p->nameLen > 0);
        if (haveName) {
            char rawName[PACKET_MAX_PLAYER_NAME];
            memset(rawName, 0, sizeof(rawName));
            memcpy(rawName, p->name, p->nameLen);
            if (!lobbyBotNameAcceptable(rawName, validatedName,
                                        sizeof(validatedName), -1,
                                        transportUdpServerGetPlayerName,
                                        NULL, NULL)) {
                fprintf(stderr, "ADD_BOT reject INVALID: bot name not acceptable ('%s')\n", rawName);
                return CMD_REJECT_INVALID;
            }
        }
        /* Slot pick: on a scenario map, bots joining the ENEMY side
         * (team 2) fill from the TOP — those seats are the script's
         * roster and live above the defender cap. Everything else goes
         * through serverSimFindFreeSlot, which honors the effective
         * player cap (operator -maxplayers clamped by the scenario's
         * max_players) so scenario defender seats stay bounded. */
        int freeSlot = -1;
        if (scenarioIsActive(sim) && p->teamNumber == 2) {
            for (int s2 = MAX_TANKS - 1; s2 >= 0; s2--) {
                if (!serverSimIsPlayerConnected(sim, (BYTE)s2)) {
                    freeSlot = s2;
                    break;
                }
            }
        } else {
            freeSlot = serverSimFindFreeSlot(sim);
        }
        if (freeSlot < 0) {
            fprintf(stderr, "ADD_BOT reject INVALID: no free slot under the "
                    "player cap (%d)\n", (int)serverSimGetMaxPlayers(sim));
            return CMD_REJECT_INVALID;
        }
        BYTE slot = (BYTE)freeSlot;
        char botName[64];
        if (haveName) {
            SDL_strlcpy(botName, validatedName, sizeof(botName));
        } else {
            snprintf(botName, sizeof(botName), "Bot %d", slot + 1);
        }
        if (!botManagerAddBot(sim, slot, serverSimGetBotBrainPath(sim), botName,
                              serverSimGetBotAiType(sim),
                              gameTypeGet(&serverSimGetGameSim(sim)->game),
                              serverSimGetGameSim(sim)->hiddenMines)) {
            fprintf(stderr, "ADD_BOT reject INVALID: botManagerAddBot failed (slot=%d name='%s' brain='%s')\n",
                    (int)slot, botName, serverSimGetBotBrainPath(sim));
            return CMD_REJECT_INVALID;
        }
        transportUdpServerSetBotName(slot, botName);
        if (p->teamNumber > 0 && p->teamNumber < MAX_TANKS) {
            serverSimSetTeam(sim, slot, p->teamNumber);
        }
        serverSimPublishLobbySlot(sim, slot);
        serverSimPublishLobbyBotBrain(sim, slot);
        lobbyAutoUnreadyOnChange(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_SET_MAP: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbySetMap *p = &cmd->u.lobbySetMap;
        if (p->relPathLen == 0) return CMD_REJECT_INVALID;
        char relPath[256];
        memcpy(relPath, p->relPath, p->relPathLen);
        relPath[p->relPathLen] = '\0';
        bool safe = true;
        if (relPath[0] == '/' || relPath[0] == '\\') safe = false;
        else if (relPath[0] != '\0' && relPath[1] == ':') safe = false;
        else {
            for (const char *s = relPath; *s;) {
                if (s[0] == '.' && s[1] == '.' &&
                    (s[2] == '\0' || s[2] == '/' || s[2] == '\\')) {
                    safe = false; break;
                }
                while (*s && *s != '/' && *s != '\\') s++;
                while (*s == '/' || *s == '\\') s++;
            }
        }
        if (!safe) return CMD_REJECT_INVALID;
        char fullPath[FILENAME_MAX];
        SDL_snprintf(fullPath, sizeof(fullPath), "%s/%s",
                     serverSimGetMapDirRoot(sim), relPath);
        if (!serverSimReloadMap(sim, fullPath)) return CMD_REJECT_INVALID;
        return CMD_OK;
    }
    case CMD_LOBBY_PREVIEW_CANCEL: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        (void)serverSimRevertPreview(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_PREVIEW_COMMIT: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
            return CMD_REJECT_LOCKED;
        }
        serverSimCommitPreview(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_PREVIEW_RANDOM: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
            return CMD_REJECT_LOCKED;
        }
        const CmdLobbyPreviewRandom *p = &cmd->u.lobbyPreviewRandom;
        if (p->seedLen == 0 || p->seedLen > 63) return CMD_REJECT_INVALID;
        char seedStr[64];
        memset(seedStr, 0, sizeof(seedStr));
        memcpy(seedStr, p->seed, p->seedLen);
        MapGenConfig cfg;
        if (!mapGenSeedToConfig(seedStr, &cfg)) return CMD_REJECT_INVALID;
        cfg.x1 = MAP_MINE_EDGE_LEFT + 1;
        cfg.y1 = MAP_MINE_EDGE_TOP + 1;
        cfg.x2 = MAP_MINE_EDGE_RIGHT - 1;
        cfg.y2 = MAP_MINE_EDGE_BOTTOM - 1;
        if (!serverSimReloadRandomMap(sim, &cfg)) return CMD_REJECT_INVALID;
        return CMD_OK;
    }
    case CMD_LOBBY_KICK: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_NOT_HOST;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        uint8_t slot = cmd->u.lobbyKick.slot;
        if (slot >= MAX_TANKS || slot == serverSimGetHostSlot(sim) || (int)slot == senderSlot) {
            return CMD_REJECT_INVALID;
        }
        const char *name = transportUdpServerGetPlayerName(slot);
        if (name != NULL) transportUdpServerKickPlayer(sim, name);
        lobbyAutoUnreadyOnChange(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_TRANSFER_HOST: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        /* Host-only — openHost must NOT grant transfer (a connected
         * player must not be able to hand off the host role). */
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        uint8_t slot = cmd->u.lobbyTransferHost.slot;
        /* Target must be a connected human other than the current host
         * (self == host here, so the self/already-host cases coincide). */
        if (slot >= MAX_TANKS ||
            (int)slot == senderSlot ||
            slot == serverSimGetHostSlot(sim) ||
            !serverSimIsPlayerConnected(sim, slot) ||
            serverSimIsBot(sim, slot)) {
            return CMD_REJECT_INVALID;
        }
        serverSimSetHostSlot(sim, slot);
        return CMD_OK;
    }
    case CMD_LOBBY_SET_PASSWORD: {
        /* Host or admin only — openHost does NOT grant this. */
        bool isHost  = (senderSlot == serverSimGetHostSlot(sim));
        bool isAdmin = serverSimIsPlayerConnected(sim, (BYTE)senderSlot) &&
            (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs,
                                   (BYTE)senderSlot) & PLAYER_FLAG_ADMIN);
        if (!isHost && !isAdmin) return CMD_REJECT_NOT_HOST;
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_PASSWORD) {
            return CMD_REJECT_LOCKED;
        }
        const CmdLobbySetPassword *p = &cmd->u.lobbySetPassword;
        if (p->pwLen >= MAP_STR_SIZE) return CMD_REJECT_INVALID;
        serverSimSetPassword(sim, p->pwLen > 0 ? p->password : "", p->pwLen);
        serverSimSetHasPassword(sim, p->pwLen > 0);
        return CMD_OK;
    }
    case CMD_BALANCE_REQUEST: {
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        const BalanceProposal *bp = serverSimGetBalanceProposal(sim);
        if (bp->requestInFlight || bp->pending) return CMD_REJECT_BAD_STATE;
        if (!transportUdpServerStartBalanceRequest(
                sim, cmd->u.balanceRequest.teamSize,
                cmd->u.balanceRequest.includeBots)) {
            return CMD_REJECT_BAD_STATE;
        }
        return CMD_OK;
    }
    case CMD_BALANCE_APPLY: {
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        const BalanceProposal *bp = serverSimGetBalanceProposal(sim);
        if (!bp->pending) return CMD_REJECT_BAD_STATE;
        /* "Humans only" kicks every bot before applying the human-only
         * team assignments — the proposal contains no team for those
         * slots. */
        if (!bp->includeBots) {
            for (int i = 0; i < MAX_TANKS; i++) {
                if (serverSimIsBot(sim, (BYTE)i)) {
                    serverSimRemoveBot(sim, (BYTE)i);
                }
            }
        }
        for (int i = 0; i < MAX_TANKS; i++) {
            if (bp->teamForSlot[i] != 0) {
                serverSimSetTeamBatch(sim, (BYTE)i, bp->teamForSlot[i]);
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
        }
        serverSimReapplyTeamAlliances(sim);
        serverSimClearBalanceProposal(sim);
        /* Publish the cleared proposal so balanceProposalActive flips
         * back to false on every client — keeps canBalance gating from
         * staying disabled on the Balance-from-WBN button. */
        {
            ControlEvent clrEvt;
            memset(&clrEvt, 0, sizeof(clrEvt));
            clrEvt.type = CTRL_BALANCE_PROPOSAL;
            serverSimPublishControl(sim, &clrEvt);
        }
        logAddEvent(log_BalanceApplied, 0, 0, 0, 0, 0, NULL);
        serverSimConsoleMessage("Team balance applied");
        return CMD_OK;
    }
    case CMD_BALANCE_DISMISS: {
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!serverSimGetBalanceProposal(sim)->pending) return CMD_REJECT_BAD_STATE;
        serverSimClearBalanceProposal(sim);
        {
            ControlEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_BALANCE_PROPOSAL;
            serverSimPublishControl(sim, &evt);
        }
        return CMD_OK;
    }
    case CMD_WBN_REAUTH: {
        /* Gate on WBN availability, not ranked mode: identity verification
         * (globe / supporter / steam-linked badges) applies on any
         * WBN-registered server, matching the join-time verify path. The
         * handler re-checks winbolonetIsRunning before touching the net. */
        if (!winbolonetIsRunning()) return CMD_REJECT_BAD_STATE;
        transportUdpServerHandleWbnReauth(sim, (BYTE)senderSlot,
                                          cmd->u.wbnReauth.token);
        return CMD_OK;
    }
    case CMD_NONE:
    default:
        return CMD_REJECT_BAD_STATE;
    }
}

CmdResult serverSimApplyCommand(ServerSim *sim, int senderSlot,
                                const ClientCommand *cmd) {
    assert(threadsCurrentlyHoldsMutex());
    /* Defense-in-depth: every inbound call site resolves the sender through
     * serverFindClient over the player table, which never yields a spectator
     * (spectators are tracked in a separate table and hold no tank slot). A
     * connected player slot is always in [0, MAX_TANKS); reject anything else
     * before dispatch so a non-player sender can never index sim state. Real
     * players always pass, so this cannot alter their behaviour. */
    if (senderSlot < 0 || senderSlot >= MAX_TANKS) {
        return CMD_REJECT_INVALID;
    }
    CmdResult r = applyCommandInner(sim, senderSlot, cmd);
    if (r != CMD_OK) {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_COMMAND_REJECTED;
        evt.u.commandRejected.origCmdSeq  = cmd->cmdSeq;
        evt.u.commandRejected.origCmdType = (uint8_t)cmd->type;
        evt.u.commandRejected.reasonCode  = (uint8_t)r;
        evt.u.commandRejected.origSlot    = (uint8_t)senderSlot;
        serverSimPublishControl(sim, &evt);
    }
    return r;
}
