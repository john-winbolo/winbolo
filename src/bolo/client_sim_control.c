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
 *
 *  All wire decoders in transport_udp_client.c build a
 *  ControlEvent and route through clientSimApplyControl.
 *  They do not mutate ClientSim state directly and do not
 *  call frontEnd* callbacks directly. SP, bots, and network
 *  converge on this single funnel — see docs/ARCHITECTURE.md
 *  "Adding a new server event" for the recipe.
 *********************************************************/

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "client_sim_control.h"
#include "client_sim_internal.h"
#include "client_sim.h"
#include "client_mapload_internal.h"  /* installCompressedMap — local map reload */
#include "messages.h"
#include "netpacks.h"
#include "players.h"
#include "server_sim.h"  /* serverSimGetCompressedMap / serverSimGetMapName */
#include "global.h"      /* balanceDebugLog */
#include "../common/wb_log.h"

void clientSimApplyControl(ClientSim *cs, const ControlEvent *evt) {
    if (cs == NULL || evt == NULL) {
        return;
    }

    /* Test-only observer hook (set via clientSimSetControlObserver).
     * Fires before any state mutation so the observed stream matches
     * what the dispatcher actually receives, including the self-skip
     * branch below. The callback gets a const event and returns void —
     * it cannot influence dispatch. */
    if (cs->controlObserverCb != NULL) {
        cs->controlObserverCb(cs->controlObserverCtx, evt);
    }

    /* Self-skip on CTRL_PLAYER_LEAVE only: a recipient must not process
     * their own departure. CTRL_PLAYER_JOIN is allowed for self because
     * playersSetPlayer's self-branch (inUse already TRUE, iMyPlayerNum
     * == iPlayerNum) updates only the location field and leaves
     * position, name, and alliances untouched — making it the correct
     * sink for country-code refreshes. CTRL_LOBBY_SLOT and
     * CTRL_PLAYER_NAME similarly update self because the server is the
     * source of truth for those fields. */
    switch (evt->type) {
    case CTRL_PLAYER_LEAVE:
        if (evt->u.playerLeave.playerNum == cs->myPlayerNum) return;
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
        WB_LOG_INFO(WB_LOG_CAT_CLIENT,
                    "[DIAG] clientSimApplyControl CTRL_LOBBY_SLOT cs=%p slot=%u team=%u ready=%d isBot=%d name='%s' connected=%d",
                    (void *)cs,
                    (unsigned)evt->u.lobbySlot.playerNum,
                    (unsigned)evt->u.lobbySlot.slot.teamNumber,
                    (int)evt->u.lobbySlot.slot.ready,
                    (int)evt->u.lobbySlot.slot.isBot,
                    evt->u.lobbySlot.slot.playerName,
                    (int)evt->u.lobbySlot.slot.connected);
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
        {
            bool prevR = cs->lobbyRanked;
            cs->lobbyRanked          = evt->u.lobbySettings.lobbyRanked;
            /* Unconditional log so we can see every settings arrival
             * even when ranked stays the same — tells us whether the
             * client is receiving publishes at all. */
            balanceDebugLog("[RANKED CLIENT] CTRL_LOBBY_SETTINGS arrived: "
                            "lobbyRanked prev=%d new=%d (changed=%d) cs=%p",
                            (int)prevR, (int)cs->lobbyRanked,
                            (int)(prevR != cs->lobbyRanked), (void *)cs);
        }
        {
            bool prevA = cs->lobbyAllowNewPlayers;
            cs->lobbyAllowNewPlayers = evt->u.lobbySettings.lobbyAllowNewPlayers;
            if (prevA != cs->lobbyAllowNewPlayers) {
                balanceDebugLog("[ALLOW CLIENT] CTRL_LOBBY_SETTINGS arrived: "
                                "lobbyAllowNewPlayers %d -> %d cs=%p",
                                (int)prevA, (int)cs->lobbyAllowNewPlayers,
                                (void *)cs);
            }
        }
        cs->lobbyWbnAvailable = evt->u.lobbySettings.lobbyWbnAvailable;
        cs->lobbyServerLocks         = evt->u.lobbySettings.lobbyServerLocks;
        cs->uploadPolicy             = evt->u.lobbySettings.uploadPolicy;
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
        uint8_t s   = evt->u.lobbyBotBrain.slot;
        uint8_t idx = evt->u.lobbyBotBrain.brainIdx;
        if (s >= MAX_TANKS) break;
        /* Clamp out-of-range catalogue indices to the 0xFF sentinel so
         * downstream consumers never see a byte that points past the
         * brain catalogue. Matches the server-side write-time clamp in
         * serverSimSetBotBrainIdxFor. */
        if (idx != 0xFF && idx >= cs->lobbyBrainList.count) idx = 0xFF;
        cs->lobbyBotBrainIdx[s] = idx;
        break;
    }

    case CTRL_LOBBY_BRAIN_LIST:
        cs->lobbyBrainList = evt->u.lobbyBrainList.list;
        break;

    case CTRL_LOBBY_MAP_CHANGE:
        cs->mapDownloadComplete = false;
        memset(cs->mapSkipVotes, 0, sizeof(cs->mapSkipVotes));
        cs->mapSkipMyVote = false;
        if (!cs->isUdpTransport && cs->boundServerSim != NULL) {
            /* Local transport: the server is in-process. Pull the
             * freshly-compressed map directly and reinstall — there is
             * no MAP_DOWNLOAD wire path to wait on. */
            BYTE buf[MAP_DOWNLOAD_MAX_SIZE];
            int  len = serverSimGetCompressedMap(cs->boundServerSim, buf);
            if (len > 0) {
                installCompressedMap(cs, buf, len,
                                     serverSimGetMapName(cs->boundServerSim));
                cs->mapDownloadComplete = true;
            }
        }
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
        /* Latch a one-shot timestamp the lobby UI reads to show a brief
         * "Teams balanced" success label next to the Balance-from-WBN
         * button. The server auto-applies and immediately clears the
         * proposal, so an in-process host's render thread would only
         * ever see balanceProposalActive=false; this timestamp survives
         * that race. Set only on a non-empty arrival so the matching
         * clear publish doesn't overwrite it. */
        if (anyNonZero) {
            cs->lastBalanceProposalArrivedMs = SDL_GetTicks();
        }
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
        gv->noCount          = evt->u.gameVoteState.noCount;
        gv->eligibleCount    = evt->u.gameVoteState.eligibleCount;
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
            /* Frontends flip out of the lobby view on the running
             * transition; previously the SP finisher set this by hand
             * after StartGameInPlace, but now StartGameInPlace publishes
             * CTRL_PHASE_RUNNING and every subscriber should pick up
             * the lobby→game flip from this event. */
            cs->inLobby = false;
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

    case CTRL_SERVER_TEXT: {
        const char *text = evt->u.serverText.text;
        if (text[0] == '\0') break;
        if (cs->inLobby) {
            clientSimAppendLobbyChat(cs, "Server", text);
        } else {
            /* In-game: route to newswire only. Server announcements
             * (vote countdown, surrender, etc.) belong on the same
             * channel as base captures / kills, and newswire is on by
             * default. */
            clientMessageAdd(clientSimGetMessages(cs), newsWireMessage,
                             (char *)"Server", (char *)text);
        }
        break;
    }

    case CTRL_SERVER_SHUTDOWN:
        /* No ClientSim field maps to UDP joinState; that field stays
         * transport-internal per the architectural commitment. */
        break;

    case CTRL_CHAT:
        /* Display side effects stay at the wire boundary
         * (transport_udp_client.c PACKET_CHAT_BROADCAST branch); the
         * bus publish exists so in-process subscribers can observe
         * chat alongside the other control events. */
        break;

    case CTRL_PLAYER_LEAVE:
        /* Lobby chat "X has left" rendering stays at the wire boundary
         * (transport_udp_client.c PACKET_PLAYER_LEFT branch).  Bots and
         * SP read playerConnected directly, so no in-process state
         * mutation is needed here — the event exists so replay logs and
         * other subscribers see leaves alongside joins. */
        break;
    }
}
