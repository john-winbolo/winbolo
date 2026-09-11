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
#include "brain_list.h"               /* BRAIN_MODES_MAX, BRAIN_LEVELS_MAX */
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
#include "server_sim_lifecycle.h"     /* serverSimSetTeam, lobbyAutoUnreadyOnChange */
#include "lobby_shared_starts.h"      /* lobbySharedStartsEnabled — several players per start */
#include "start_sides.h"              /* startSideEligible — the claim command's side check */
#include "threads.h"
#include "../common/wb_log.h"
#include "transport_udp.h"            /* transportUdpServerGetPlayerName,
                                         transportUdpServerSetBotName,
                                         transportUdpServerKickPlayer,
                                         transportUdpServerSetVoiceMute */
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
         * rules let their slot hold; the host may hand anyone any start.
         * Whether anyone already holds the start makes no difference to
         * this test — with shared starts on, joining a held start is
         * judged exactly like taking a free one. */
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
        /* Connected slot currently holding idx (none when idx == 0xFF).
         * With shared starts on nobody is displaced, so the holder does
         * not need looking up at all: the claim always just lands. */
        BYTE holder = 0xFF;
        if (idx != 0xFF && !lobbySharedStartsEnabled()) {
            for (BYTE k = 0; k < MAX_TANKS; k++) {
                if (!serverSimIsPlayerConnected(sim, k)) continue;
                const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, k);
                if (lp && lp->startIdx == idx) { holder = k; break; }
            }
        }
        if (holder != 0xFF && holder != target) {
            /* One player per start (LOBBY_SHARED_STARTS off): a non-host
             * may not take a start someone else holds, and a host putting
             * anyone on one swaps the two. */
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
            /* Free start, release, already mine — or, with shared starts
             * on, joining a start others hold: they keep it too. */
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
        /* mode indexes the brain's own mode list and difficulty that mode's
         * level list, so the bounds are the manifest maxima rather than the
         * old fixed 0..2 — the server does not read the client's brain
         * files, and bot_manager clamps both against the real list when it
         * builds the init tokens. */
        if (p->slot >= MAX_TANKS || p->mode >= BRAIN_MODES_MAX ||
            p->difficulty >= BRAIN_LEVELS_MAX || p->personality > 3 ||
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
        serverSimSetBotConfig(sim, p->slot, p->mode, p->difficulty,
                              p->personality,
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
    case CMD_PLAYER_MUTE: {
        const CmdPlayerMute *p = &cmd->u.playerMute;
        if (p->targetPlayer >= MAX_TANKS) return CMD_REJECT_INVALID;
        /* Muting yourself is meaningless — you never receive your own
         * voice or chat. */
        if ((int)p->targetPlayer == senderSlot) return CMD_REJECT_INVALID;
        transportUdpServerSetVoiceMute((BYTE)senderSlot, p->targetPlayer,
                                       p->muted != 0);
        /* No control event: the mute is private to the muting client.
         * Broadcasting it would tell the muted player they were muted. */
        return CMD_OK;
    }
    case CMD_VOICE_STATE: {
        const CmdVoiceState *p = &cmd->u.voiceState;
        GameSim *gs = serverSimGetGameSim(sim);
        uint8_t oldFlags = playersGetClientFlags(&gs->plyrs, (BYTE)senderSlot);
        uint8_t flags = oldFlags;
        flags &= (uint8_t)~PLAYER_VOICE_FLAG_MASK;
        if (p->hasMic) {
            flags |= PLAYER_FLAG_HAS_MIC;
            /* Muted only means anything with a mic. Never setting the two
             * together leaves the receiving end a clean three states — no
             * mic, muted, live — rather than four with a nonsense one. */
            if (p->selfMuted) flags |= PLAYER_FLAG_VOICE_MUTED;
        }
        /* A client packs many commands into one PACKET_COMMAND_TICK, and
         * a re-send of the state the slot already holds is not a change.
         * Publishing it anyway would fan one reliable lobby-slot control
         * event per command to every client. */
        if (flags == oldFlags) return CMD_OK;
        playersSetClientFlags(&gs->plyrs, (BYTE)senderSlot, flags);
        /* During a running game the snapshot carries clientFlags every
         * tick, so the new bits reach every client on their own. The lobby
         * slot only goes out when it is published, so a change made while
         * clients are looking at the lobby has to publish it here. Same
         * gate as the lobby-slot heartbeat in server_lifecycle.c. */
        if (serverSimGetState(sim) == serverStateLobby ||
            serverSimGetState(sim) == serverStateCountdown) {
            serverSimPublishLobbySlot(sim, (BYTE)senderSlot);
        }
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
        /* Slot pick goes through serverSimFindFreeSlot, which honors the
         * effective player cap (operator -maxplayers), rather than
         * scanning all MAX_TANKS: an unbounded scan let lobby bots fill
         * seats past the cap the operator set. */
        int freeSlot = serverSimFindFreeSlot(sim);
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
        /* Inherit the mode and difficulty the host last picked for a bot,
         * written BEFORE the brain is created so it reaches the brain as its
         * init-arg tokens and publishes nothing. Adding five bots at Medium
         * should mean choosing Medium once. */
        serverSimApplyLastBotConfig(sim, slot, serverSimGetBotBrainPath(sim));
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
    case CMD_PING: {
        /* A ping is a game-time signal drawn on the map, so it needs a
         * running game and a sender that still occupies a player slot in it:
         * a lobby sender has no map to point at, and an empty slot is
         * somebody who has left. The test is occupancy, not a live tank —
         * being dead is fine, because a player waiting to respawn has as much
         * to say about the map as anyone. */
        const CmdPing *p = &cmd->u.ping;
        BYTE slot = (BYTE)senderSlot;
        uint32_t now = sim->tick;
        uint32_t oldest;
        if (serverSimGetState(sim) != serverStateRunning) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!playersIsInUse(&sim->sim.plyrs, slot)) {
            return CMD_REJECT_BAD_STATE;
        }
        if (p->kind >= PING_KIND_COUNT) {
            return CMD_REJECT_INVALID;
        }
        /* World units are 256 per map square over a 256x256 map, so the
         * whole u16 range is on the map and only the sentinel-free bound
         * matters. Checked anyway so the arm still reads as a range check if
         * either constant ever changes. */
        if ((p->worldX >> M_W_SHIFT_SIZE) >= MAP_ARRAY_SIZE ||
            (p->worldY >> M_W_SHIFT_SIZE) >= MAP_ARRAY_SIZE) {
            return CMD_REJECT_INVALID;
        }
        /* Rate limit: a minimum gap, and a burst cap on top of it. Both are
         * measured in sim ticks, which only advance while the game runs — the
         * only state this arm accepts. Both stores hold tick+1 so that 0 can
         * mean "never", because tick 0 is itself a real tick. */
        if (sim->pingLastTick[slot] != 0 &&
            now + 1 - sim->pingLastTick[slot] < PING_RATE_MIN_GAP_TICKS) {
            return CMD_REJECT_COOLDOWN;
        }
        oldest = sim->pingBurstTicks[slot][sim->pingBurstIdx[slot]];
        if (oldest != 0 && now + 1 - oldest < PING_RATE_WINDOW_TICKS) {
            return CMD_REJECT_COOLDOWN;
        }
        sim->pingLastTick[slot] = now + 1;
        sim->pingBurstTicks[slot][sim->pingBurstIdx[slot]] = now + 1;
        sim->pingBurstIdx[slot] =
            (uint8_t)((sim->pingBurstIdx[slot] + 1) % PING_RATE_BURST);

        {
            GameEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EVENT_PING;
            ev.data[0] = slot;
            ev.data[1] = p->kind;
            ev.data[2] = (BYTE)(p->worldX >> 8);
            ev.data[3] = (BYTE)(p->worldX & 0xFF);
            ev.data[4] = (BYTE)(p->worldY >> 8);
            ev.data[5] = (BYTE)(p->worldY & 0xFF);
            /* The pending record IS this ping's queue — the arm deliberately
             * does not call serverSimAddEvent. It runs during packet receive,
             * before serverSimTick clears the per-frame event buffer, so an
             * event buffered here would be wiped before the post-tick UDP drain
             * could send it; and buffering it both here and at the flush would
             * deliver it twice to an in-process client, whose snapshot poll
             * dedups per serverTick and so would take the pre-clear copy on one
             * tick and the flushed copy on the next. serverSimFlushPendingPings,
             * at the top of the running tick, is the single point at which an
             * EVENT_PING enters sim->events. */
            sim->pendingPing[slot] = ev;
            sim->hasPendingPing[slot] = true;
        }
        /* Recorded whole so a replay can draw the marker where the sender
         * put it; the viewer culls nothing, since a replay watches every
         * team at once. */
        logAddEvent(log_Ping, slot, p->kind,
                    (BYTE)(p->worldX >> 8), (BYTE)(p->worldX & 0xFF),
                    p->worldY, NULL);
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
