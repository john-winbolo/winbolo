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

/*********************************************************
 *Name:          Client Sim Control
 *Filename:      client_sim_control.c
 *Purpose:
 *  Dispatcher implementation. Mutates ClientSim game state
 *  only — UI, achievement, transport-internal, and wire-
 *  protocol housekeeping live in their respective callers.
 *********************************************************/

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "client_sim_control.h"
#include "client_sim_internal.h"
#include "client_sim.h"
#include "messages.h"
#include "netpacks.h"
#include "players.h"

void clientSimApplyControl(ClientSim *cs, const ControlEvent *evt) {
    if (cs == NULL || evt == NULL) {
        return;
    }

    /* Self-skip on CTRL_PLAYER_JOIN only: the recipient's own player
     * record is established via the join handshake / snapshot stream
     * and must not be overwritten by sync or live publish with stale
     * or partial data. CTRL_LOBBY_SLOT and CTRL_PLAYER_NAME do update
     * self — the server is the source of truth for the recipient's
     * lobby slot and name, matching the pre-migration UDP handlers
     * which had no self-guard. */
    switch (evt->type) {
    case CTRL_PLAYER_JOIN:
        if (evt->u.playerJoin.playerNum == cs->myPlayerNum) return;
        break;
    default:
        break;
    }

    switch (evt->type) {
    case CTRL_ALLIANCE_REQUEST:
        /* No state mutation — request is UI/dialog territory. */
        break;

    case CTRL_ALLIANCE_ACCEPT:
        playersAcceptAlliance(&cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                              evt->u.allianceAccept.acceptedBy,
                              evt->u.allianceAccept.newMember,
                              FALSE);
        break;

    case CTRL_ALLIANCE_LEAVE:
        playersLeaveAlliance(&cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                             evt->u.allianceLeave.playerNum, FALSE);
        break;

    case CTRL_PLAYER_JOIN: {
        BYTE pNum = evt->u.playerJoin.playerNum;
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        char ccBuf[3];
        BYTE allies[MAX_TANKS];
        BYTE numAllies = evt->u.playerJoin.numAllies;
        memcpy(nameBuf, evt->u.playerJoin.name, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ccBuf[0] = evt->u.playerJoin.country[0];
        ccBuf[1] = evt->u.playerJoin.country[1];
        ccBuf[2] = '\0';
        if (numAllies > MAX_TANKS) numAllies = MAX_TANKS;
        if (numAllies > 0) {
            memcpy(allies, evt->u.playerJoin.allies, numAllies);
        }
        playersSetClientType(&cs->sim.plyrs, pNum, evt->u.playerJoin.clientType);
        playersSetClientFlags(&cs->sim.plyrs, pNum, evt->u.playerJoin.clientFlags);
        playersSetPlayer(cs, &cs->sim.plyrs, cs->myPlayerNum, pNum,
                         nameBuf, ccBuf,
                         0, 0, 0, 0, 0, FALSE,
                         numAllies, numAllies > 0 ? allies : NULL, FALSE);
        break;
    }

    case CTRL_PLAYER_NAME: {
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        memcpy(nameBuf, evt->u.playerName.name, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        playersSetPlayerName(cs, &cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                             evt->u.playerName.playerNum, nameBuf, FALSE);
        break;
    }

    case CTRL_LOBBY_SLOT:
        cs->lobbySlots[evt->u.lobbySlot.playerNum] = evt->u.lobbySlot.slot;
        break;

    case CTRL_LOBBY_SETTINGS:
        strncpy(cs->mapName, evt->u.lobbySettings.mapName, MAP_STR_SIZE - 1);
        cs->mapName[MAP_STR_SIZE - 1] = '\0';
        cs->lobbyGameType    = evt->u.lobbySettings.lobbyGameType;
        cs->lobbyHiddenMines = evt->u.lobbySettings.lobbyHiddenMines;
        cs->lobbyAiType      = evt->u.lobbySettings.lobbyAiType;
        cs->lobbyTimeLimit   = evt->u.lobbySettings.lobbyTimeLimit;
        cs->lobbyPillCount   = evt->u.lobbySettings.lobbyPillCount;
        cs->lobbyBaseCount   = evt->u.lobbySettings.lobbyBaseCount;
        cs->lobbyStartCount  = evt->u.lobbySettings.lobbyStartCount;
        cs->mapSkipAvailable = evt->u.lobbySettings.mapSkipAvailable;
        cs->netStat          = evt->u.lobbySettings.netStat;
        cs->inLobby          = evt->u.lobbySettings.inLobby;
        cs->lobbyOpenHost            = evt->u.lobbySettings.lobbyOpenHost;
        cs->lobbyAutoLockOnGameStart = evt->u.lobbySettings.lobbyAutoLockOnGameStart;
        cs->lobbyRanked              = evt->u.lobbySettings.lobbyRanked;
        cs->lobbyServerLocks         = evt->u.lobbySettings.lobbyServerLocks;
        break;

    case CTRL_LOBBY_TEAM_META: {
        uint8_t t = evt->u.lobbyTeamMeta.teamId;
        if (t == 0 || t >= MAX_TANKS) break;
        cs->lobbyTeamInUse[t] = evt->u.lobbyTeamMeta.in_use;
        cs->lobbyTeamColor[t] = evt->u.lobbyTeamMeta.color;
        cs->lobbyTeamPool[t]  = evt->u.lobbyTeamMeta.namingPool;
        strncpy(cs->lobbyTeamName[t], evt->u.lobbyTeamMeta.name,
                sizeof(cs->lobbyTeamName[t]) - 1);
        cs->lobbyTeamName[t][sizeof(cs->lobbyTeamName[t]) - 1] = '\0';
        break;
    }

    case CTRL_LOBBY_BOT_CONFIG: {
        uint8_t s = evt->u.lobbyBotConfig.slot;
        if (s >= MAX_TANKS) break;
        cs->lobbyBotDifficulty[s]  = evt->u.lobbyBotConfig.difficulty;
        cs->lobbyBotPersonality[s] = evt->u.lobbyBotConfig.personality;
        /* Bot display name flows through the lobbySlot path; the
         * name field on this event is informational and ignored here
         * to avoid stomping the slot's playerName on a partial mirror. */
        break;
    }

    case CTRL_LOBBY_BOT_BRAIN: {
        uint8_t s = evt->u.lobbyBotBrain.slot;
        if (s >= MAX_TANKS) break;
        strncpy(cs->lobbyBotBrain[s], evt->u.lobbyBotBrain.path,
                sizeof(cs->lobbyBotBrain[s]) - 1);
        cs->lobbyBotBrain[s][sizeof(cs->lobbyBotBrain[s]) - 1] = '\0';
        break;
    }

    case CTRL_LOBBY_BRAIN_LIST:
        cs->lobbyBrainList = evt->u.lobbyBrainList.list;
        break;

    case CTRL_LOBBY_MAP_CHANGE:
        cs->mapDownloadComplete = false;
        memset(cs->mapSkipVotes, 0, sizeof(cs->mapSkipVotes));
        cs->mapSkipMyVote = false;
        break;

    case CTRL_MAP_DOWNLOAD_COMPLETE:
        cs->mapDownloadComplete = true;
        break;

    case CTRL_BALANCE_PROPOSAL: {
        bool anyNonZero = false;
        int i;
        memcpy(cs->balanceProposal, evt->u.balanceProposal.teamForSlot, MAX_TANKS);
        for (i = 0; i < MAX_TANKS; i++) {
            if (cs->balanceProposal[i] != 0) {
                anyNonZero = true;
                break;
            }
        }
        cs->balanceProposalActive = anyNonZero;
        break;
    }

    case CTRL_MAP_SKIP_STATE: {
        int i;
        for (i = 0; i < MAX_TANKS; i++) {
            cs->mapSkipVotes[i] = evt->u.mapSkipState.votes[i] ? true : false;
        }
        cs->mapSkipMyVote = cs->mapSkipVotes[cs->myPlayerNum];
        break;
    }

    case CTRL_GAME_VOTE_STATE: {
        uint8_t k = evt->u.gameVoteState.kind;
        int idx = (k == GAME_VOTE_KIND_BACK_TO_LOBBY) ? 0
                : (k == GAME_VOTE_KIND_SURRENDER)     ? 1 : -1;
        if (idx < 0) break;
        struct ClientGameVote *gv = &cs->gameVotes[idx];
        uint8_t prevActive = gv->active;
        bool wasRunning = (prevActive == GAME_VOTE_ACTIVE_RUNNING);
        gv->kind             = evt->u.gameVoteState.kind;
        gv->active           = evt->u.gameVoteState.active;
        gv->triggerSrc       = evt->u.gameVoteState.triggerSrc;
        gv->teamId           = evt->u.gameVoteState.teamId;
        gv->threshold        = evt->u.gameVoteState.threshold;
        gv->yesCount         = evt->u.gameVoteState.yesCount;
        gv->secondsRemaining = evt->u.gameVoteState.secondsRemaining;
        gv->votes            = evt->u.gameVoteState.votes;
        /* Auto-pop the widget when a vote starts — players can X to hide. */
        if (!wasRunning && gv->active == GAME_VOTE_ACTIVE_RUNNING) {
            gv->widgetVisible  = true;
            gv->concludedAtMs  = 0;
        }
        /* Record conclusion time so the widget can auto-hide a few
         * seconds after a pass/fail/cancel. */
        if (wasRunning && gv->active != GAME_VOTE_ACTIVE_RUNNING) {
            gv->concludedAtMs = SDL_GetTicks();
        }
        /* Newswire notifications on state transitions so players who
         * have the widget closed still see what happened. */
        const char *kindLabel = (k == GAME_VOTE_KIND_BACK_TO_LOBBY)
                                ? "Return-to-lobby vote" : "Surrender vote";
        if (!wasRunning && gv->active == GAME_VOTE_ACTIVE_RUNNING) {
            const char *trig =
                (gv->triggerSrc == GAME_VOTE_TRIGGER_BASE_MONOPOLY)
                    ? "started (one team controls every base)"
                : (gv->triggerSrc == GAME_VOTE_TRIGGER_POST_SURRENDER)
                    ? "started (following a surrender)"
                : "started";
            char body[128];
            snprintf(body, sizeof(body), "%s %s.", kindLabel, trig);
            clientMessageAdd(clientSimGetMessages(cs), newsWireMessage,
                             (char *)"Vote", body);
        } else if (wasRunning && gv->active != GAME_VOTE_ACTIVE_RUNNING) {
            const char *outcome =
                (gv->active == GAME_VOTE_ACTIVE_PASSED)    ? "passed"
              : (gv->active == GAME_VOTE_ACTIVE_FAILED)    ? "failed"
              : (gv->active == GAME_VOTE_ACTIVE_CANCELLED) ? "cancelled"
              :                                              "ended";
            char body[160];
            snprintf(body, sizeof(body), "%s %s (%u / %u).",
                     kindLabel, outcome,
                     (unsigned)gv->yesCount, (unsigned)gv->threshold);
            clientMessageAdd(clientSimGetMessages(cs), newsWireMessage,
                             (char *)"Vote", body);
        }
        break;
    }

    case CTRL_GAME_PHASE:
        switch (evt->u.gamePhase.phase) {
        case CTRL_PHASE_LOBBY:
            cs->netStat = netLobby;
            cs->countdownSeconds = 0;
            break;
        case CTRL_PHASE_COUNTDOWN:
            cs->netStat = netLobbyCountdown;
            cs->countdownSeconds = evt->u.gamePhase.countdownSeconds;
            break;
        case CTRL_PHASE_RUNNING:
            cs->netStat = netRunning;
            cs->countdownSeconds = 0;
            break;
        case CTRL_PHASE_GAME_OVER:
            /* Lobby-branch reset; non-lobby end-of-game is transport-internal. */
            if (cs->inLobby) {
                cs->netStat = netLobby;
                cs->countdownSeconds = 0;
            }
            break;
        }
        break;

    case CTRL_GAME_OVER:
        if (cs->inLobby) {
            cs->netStat = netLobby;
            cs->countdownSeconds = 0;
            cs->lobbyChatHistory[0] = '\0';
        }
        break;

    case CTRL_SERVER_SHUTDOWN:
        /* No ClientSim field maps to UDP joinState; that field stays
         * transport-internal per the architectural commitment. */
        break;
    }
}
