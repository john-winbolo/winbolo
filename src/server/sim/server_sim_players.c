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

/*********************************************************
 *Name:          Server Simulation Player Roster
 *Filename:      server_sim_players.c
 *Author:        John Morrison
 *Purpose:
 *  The player roster — joining and leaving, team and
 *  ready state, the alliance mutators, and the clustered
 *  lobby start each slot reserves while it holds a place.
 *********************************************************/

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "server_sim_shared.h"      /* serverSimSetActive, serverSimResetLobbyToDefaults, publishMapSkipState */
#include "server_sim_internal.h"
#include "../scenario.h"   /* the scripted-scenario VM this TU drives */
#include "server_sim_join.h"        /* the join step-internals, serverSimFindFreeSlot and LocalJoinResult */
#include "server_sim_lifecycle.h"   /* serverSimSetHostSlot, serverSimLobbyCheckAllReady — the leave path's lobby fixups */
#include "playername_validate.h"    /* playerNameValidate — the local-join name check */
#include "playersrejoin.h"          /* playersRejoinAddPlayer, playersRejoinRequest — pill/base ownership across a rejoin */
#include "log.h"                    /* logAddEvent — the .wbv join, leave and alliance records */
#include "wire_limits.h"            /* PACKET_MAX_PLAYER_NAME — the rename event's name field */
#include "../../winbolonet/winbolonet_core.h"   /* winbolonetAddEvent — WBN join and alliance tracking */
#include "../../common/wb_log.h"    /* WB_LOG_INFO — the join and leave trace */

void addPlayerInternal(ServerSim *sim, BYTE playerNum, const char *playerName,
                       const char *country, bool wantRejoin) {
    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "addPlayer slot=%u name='%s' wantRejoin=%d state=%d",
        (unsigned)playerNum,
        playerName ? playerName : "(null)",
        (int)wantRejoin,
        (int)sim->state);
    if (playerNum >= MAX_TANKS) {
        WB_LOG_WARN(WB_LOG_CAT_SERVER,
            "addPlayer rejected: slot=%u >= MAX_TANKS=%d",
            (unsigned)playerNum, (int)MAX_TANKS);
        return;
    }

    /* Clean up any leftover state from a previous player in this slot */
    if (sim->sim.tanks[playerNum] != NULL) {
        tankDestroy(&sim->sim, &sim->sim.tanks[playerNum]);
        sim->sim.tanks[playerNum] = NULL;
    }
    if (sim->sim.lgmen[playerNum] != NULL) {
        lgmDestroy(&sim->sim.lgmen[playerNum]);
        sim->sim.lgmen[playerNum] = NULL;
    }
    sim->inputQueueHead[playerNum] = 0;
    sim->inputQueueTail[playerNum] = 0;
    sim->lastProcessedInput[playerNum] = 0;
    sim->lastInputButtons[playerNum] = 0;
    sim->lastActionAppliedTick[playerNum] = 0;
    sim->pendingHarvestActions[playerNum] = 0;
    sim->pendingHarvestBuildAction[playerNum] = 0;
    sim->pendingHarvestBuildX[playerNum] = 0;
    sim->pendingHarvestBuildY[playerNum] = 0;
    sim->inputBufferFilled[playerNum] = 0;
    sim->inputDryTicks[playerNum] = 0;
    sim->jitterTarget[playerNum] = JITTER_BUFFER_DEFAULT;
    sim->jitterStallCount[playerNum] = 0;
    sim->jitterStarveCount[playerNum] = 0;
    sim->jitterStableTicks[playerNum] = 0;
    sim->statStallTicks[playerNum] = 0;
    sim->statGapFillTicks[playerNum] = 0;
    sim->statDroppedStaleInputs[playerNum] = 0;
    sim->statCatchupTicks[playerNum] = 0;
    sim->statLastRewindTicks[playerNum] = 0;

    sim->playerConnected[playerNum] = TRUE;
    sim->hadPlayersEver = TRUE;

    /* Initialize lobby player state. Default-team assignment (Layout A):
     *   slot 0 (host)             → team 1
     *   slot 1 (second joiner)    → team 2
     *   slot 2+ (subsequent)      → smallest existing team (load balance)
     * Bots take whichever team the bot-add path picks (existing logic).
     * Players can self-reassign via the team picker after joining. */
    sim->lobbyPlayers[playerNum].ready = FALSE;
    sim->lobbyPlayers[playerNum].isBot = FALSE;
    sim->lobbyPlayers[playerNum].startIdx = 0xFF;
    {
        uint8_t defaultTeam = 1;
        if (playerNum == 0) {
            defaultTeam = 1;
        } else if (playerNum == 1) {
            defaultTeam = 2;
        } else {
            /* Smallest in-use team wins. Counts include bots. */
            int counts[16] = {0};
            for (int i = 0; i < MAX_TANKS; i++) {
                if (i == playerNum) continue;
                if (!sim->playerConnected[i]) continue;
                uint8_t t = sim->lobbyPlayers[i].teamNumber;
                if (t > 0 && t < 16) counts[t]++;
            }
            int bestTeam = 1, bestCount = INT_MAX;
            for (int t = 1; t < 16; t++) {
                if (!sim->teams[t].in_use) continue;
                if (counts[t] < bestCount) {
                    bestCount = counts[t];
                    bestTeam  = t;
                }
            }
            defaultTeam = (uint8_t)bestTeam;
        }
        sim->lobbyPlayers[playerNum].teamNumber = defaultTeam;
    }

    /* Reserve a clustered start now the team is known (humans and bots). */
    serverSimAssignLobbyStartOnJoin(sim, playerNum);

    /* Set active sim so routing functions access sim state during tankCreate */
    serverSimSetActive(sim);

    /* Only create tank immediately in running state (no-lobby mode or mid-game join).
     * In lobby state, tanks are created at game start. */
    if (sim->state == serverStateRunning) {
        tankCreate(&sim->sim, &sim->sim.tanks[playerNum]);
        sim->sim.lgmen[playerNum] = lgmCreate(playerNum);
        basesUpdateTimer(&sim->sim, playerNum);
    }

    /* Register player in sim's players struct so message formatting
     * (e.g. "Player captured a base") uses the correct name. */
    if (playerName != NULL) {
        playersSetPlayer(NULL, &sim->sim.plyrs, NEUTRAL, playerNum, (char *)playerName, "XX",
                         0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);
        /* Apply the country before the log event below reads it back out —
         * the recorded join otherwise carries the "XX" placeholder for the
         * whole round. No-ops on NULL or a malformed code, leaving "XX". */
        setPlayerCountryInternal(sim, playerNum, country);
        {
            char pstr[256];
            int nameLen = (int)strlen(playerName);
            BYTE accountFlags = playersGetAccountFlags(&sim->sim.plyrs, playerNum);
            if (nameLen > 255) nameLen = 255;
            pstr[0] = (char)nameLen;
            memcpy(pstr + 1, playerName, nameLen);
            logAddEvent(log_PlayerJoined, playerNum,
                        sim->sim.plyrs->item[playerNum].location[0],
                        sim->sim.plyrs->item[playerNum].location[1],
                        accountFlags, 0, pstr);
        }
    }

    /* Attempt to restore ownership of pills/bases from a previous session */
    if (wantRejoin && sim->state == serverStateRunning && playerName != NULL) {
        playersRejoinRequest(&sim->sim, (char *)playerName, playerNum, &sim->sim.pb);
    }

    /* Broadcast current skip vote state to the new player — existing votes
     * are preserved since the threshold naturally adjusts with more players. */
    if (sim->lobbyEnabled && sim->state == serverStateLobby && (sim->mapDirCount > 1 || sim->randomMapEnabled)) {
        ControlEvent skipEvt;
        BYTE k;
        memset(&skipEvt, 0, sizeof(skipEvt));
        skipEvt.type = CTRL_MAP_SKIP_STATE;
        for (k = 0; k < MAX_TANKS; k++) {
            skipEvt.u.mapSkipState.votes[k] = sim->mapSkipVotes[k] ? 1 : 0;
        }
        serverSimPublishControl(sim, &skipEvt);
    }

    /* Start this slot's copy of the terrain from the map as it stands now.
     * The join blob the caller sends next is compressed from this copy, so
     * the client and the copy begin the session holding the same tiles; the
     * rejoin ownership restore above has already run, matching the point the
     * blob is taken. */
    serverSimShadowSeed(sim, playerNum);
}

void fillAndPublishPlayerJoin(ServerSim *sim, BYTE playerNum) {
    ControlEvent joinEvt;
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    memset(&joinEvt, 0, sizeof(joinEvt));
    serverSimFillPlayerJoinEvent(sim, playerNum, &joinEvt);
    serverSimPublishControl(sim, &joinEvt);
}

void setPlayerCountryInternal(ServerSim *sim, BYTE playerNum, const char *cc) {
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    if (cc == NULL) return;
    if (cc[0] == '\0' || cc[1] == '\0' || cc[2] != '\0') return;
    if (!isalpha((unsigned char)cc[0]) || !isalpha((unsigned char)cc[1])) return;
    if (sim->sim.plyrs == NULL) return;
    sim->sim.plyrs->item[playerNum].location[0] = (char)toupper((unsigned char)cc[0]);
    sim->sim.plyrs->item[playerNum].location[1] = (char)toupper((unsigned char)cc[1]);
    sim->sim.plyrs->item[playerNum].location[2] = '\0';
}

void setClientTypeFlagsInternal(ServerSim *sim, BYTE playerNum,
                                uint8_t clientType, uint8_t clientFlags) {
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    playersSetClientType(&sim->sim.plyrs, playerNum, clientType);
    playersSetClientFlags(&sim->sim.plyrs, playerNum, clientFlags);
}

void serverSimAddPlayer(ServerSim *sim, BYTE playerNum, const char *playerName, bool wantRejoin) {
    addPlayerInternal(sim, playerNum, playerName, NULL, wantRejoin);
    fillAndPublishPlayerJoin(sim, playerNum);
}

void serverSimSetPlayerCountry(ServerSim *sim, BYTE playerNum, const char *cc) {
    setPlayerCountryInternal(sim, playerNum, cc);
    fillAndPublishPlayerJoin(sim, playerNum);
}

int serverSimFindFreeSlot(const ServerSim *sim) {
    int  i;
    BYTE limit;
    if (sim == NULL) return -1;
    limit = (sim->maxPlayers > 0) ? sim->maxPlayers : (BYTE)MAX_TANKS;
    for (i = 0; i < limit; i++) {
        if (!sim->playerConnected[i] && !botManagerIsBot(sim, (BYTE)i)) {
            return i;
        }
    }
    return -1;
}

LocalJoinResult serverSimLocalJoin(ServerSim *sim,
                                   const char *playerName,
                                   const char *fallbackCountry,
                                   uint8_t clientType,
                                   uint8_t clientFlags,
                                   BYTE *outSlot) {
    char validatedName[PLAYER_NAME_LEN];
    int  slot;
    const char *country;

    if (sim == NULL || playerName == NULL || outSlot == NULL) {
        return LOCAL_JOIN_INVALID_INPUT;
    }

    if (!playerNameValidate(playerName, validatedName, sizeof(validatedName), NULL)) {
        return LOCAL_JOIN_INVALID_NAME;
    }

    /* Mirror the UDP-side game-lock predicate (transport_udp_server.c). The
     * lobby gate is the only one we can replicate locally; the UDP-only
     * gameLocked flag has no local-join analogue. */
    if (!serverSimIsAcceptingJoins(sim)) {
        return LOCAL_JOIN_GAME_LOCKED;
    }

    slot = serverSimFindFreeSlot(sim);
    if (slot < 0) {
        return LOCAL_JOIN_SLOT_FULL;
    }

    country = (fallbackCountry != NULL) ? fallbackCountry : "";

    addPlayerInternal(sim, (BYTE)slot, validatedName, country, false);
    setClientTypeFlagsInternal(sim, (BYTE)slot, clientType, clientFlags);
    fillAndPublishPlayerJoin(sim, (BYTE)slot);

    {
        char serverKey[WINBOLONET_KEY_LEN];
        serverKey[0] = '\0';
        if (winbolonetIsRunning()) {
            winboloNetGetServerKey(serverKey);
            if (serverKey[0] != '\0') {
                winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                   (BYTE)slot, WINBOLO_NET_NO_PLAYER,
                                   botManagerIsBot(sim, (BYTE)slot), FALSE);
            }
        }
    }

    *outSlot = (BYTE)slot;
    return LOCAL_JOIN_OK;
}

void serverSimRemovePlayer(ServerSim *sim, BYTE playerNum) {
    bool wasBot;
    if (playerNum >= MAX_TANKS) return;
    /* Captured before any teardown so the last-human-left reset below can
     * tell a human departure from a bot one. Bot removals run through this
     * same path (botManagerRemoveBot), and the reset itself removes bots —
     * gating on a human leaver keeps that from re-entering. */
    wasBot = botManagerIsBot(sim, playerNum) ||
             sim->botMgr.removingBotSlot == (BYTE)(playerNum + 1);
    {
        char nm[PLAYER_NAME_LEN];
        playersGetPlayerName(&sim->sim.plyrs, playerNum, nm, sizeof(nm), TRUE);
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "removePlayer slot=%u name='%s' state=%d",
            (unsigned)playerNum, nm, (int)sim->state);
    }
    logAddEvent(log_PlayerLeaving, playerNum, 0, 0, 0, 0, NULL);
    logAddEvent(log_PlayerQuit, playerNum, 0, 0, 0, 0, NULL);

    /* A scripted scenario may have armed a spawn-loadout override for
     * this slot (open-mode wave bots) — it must not leak to the slot's
     * next occupant. */
    sim->sim.spawnLoadout[playerNum] = 0;

    /* Publish before clearing the slot — the filler reads the player's
     * name and country out of sim->sim.plyrs->item[playerNum], which is
     * still valid here and gets zeroed later in this function. */
    {
        ControlEvent leaveEvt;
        memset(&leaveEvt, 0, sizeof(leaveEvt));
        serverSimFillPlayerLeaveEvent(sim, playerNum, &leaveEvt);
        serverSimPublishControl(sim, &leaveEvt);
    }

    /* Freeze this slot's identity before the roster entry is torn down: the
     * attribution track's identity table is otherwise only filled at game over,
     * which would leave a mid-round leaver nameless in the finished log. */
    {
        AttrSlotIdentity *id = &sim->trackIdentity[playerNum];
        /* Read the full-length name first; id->name is the shorter
         * wire-sized field, so the copy into it truncates. */
        char nameBuf[PLAYER_NAME_LEN];
        memset(id, 0, sizeof(*id));
        id->isBot = wasBot ? 1 : 0;
        id->team  = sim->lobbyPlayers[playerNum].teamNumber;
        playersGetPlayerName(&sim->sim.plyrs, playerNum, nameBuf,
                             sizeof(nameBuf), TRUE);
        snprintf(id->name, sizeof(id->name), "%s", nameBuf);
    }

    sim->playerConnected[playerNum] = FALSE;
    if (sim->sim.tanks[playerNum] != NULL) {
        tankDestroy(&sim->sim, &sim->sim.tanks[playerNum]);
        sim->sim.tanks[playerNum] = NULL;
    }
    if (sim->sim.lgmen[playerNum] != NULL) {
        lgmDestroy(&sim->sim.lgmen[playerNum]);
        sim->sim.lgmen[playerNum] = NULL;
    }
    sim->inputQueueHead[playerNum] = 0;
    sim->inputQueueTail[playerNum] = 0;
    sim->lastProcessedInput[playerNum] = 0;
    sim->lastInputButtons[playerNum] = 0;
    sim->lastActionAppliedTick[playerNum] = 0;
    sim->pendingHarvestActions[playerNum] = 0;
    sim->pendingHarvestBuildAction[playerNum] = 0;
    sim->pendingHarvestBuildX[playerNum] = 0;
    sim->pendingHarvestBuildY[playerNum] = 0;
    sim->playerPing[playerNum] = 0;
    sim->inputBufferFilled[playerNum] = 0;
    sim->inputDryTicks[playerNum] = 0;
    sim->jitterTarget[playerNum] = JITTER_BUFFER_DEFAULT;
    sim->jitterStallCount[playerNum] = 0;
    sim->jitterStarveCount[playerNum] = 0;
    sim->jitterStableTicks[playerNum] = 0;
    sim->statStallTicks[playerNum] = 0;
    sim->statGapFillTicks[playerNum] = 0;
    sim->statDroppedStaleInputs[playerNum] = 0;
    sim->statCatchupTicks[playerNum] = 0;
    sim->statLastRewindTicks[playerNum] = 0;

    /* Post-game stats: a mid-round leaver is dropped from the round summary as
     * if never present. Zero this slot's accumulator row, clear every other
     * slot's matrix cells that reference this slot (the column), and prune the
     * notable-event timeline of entries involving this slot. Other players'
     * aggregate counters are intentionally left as-is — only the leaver's own
     * stats and direct references to them are removed. Mine cells laid by this
     * slot are released so a later detonation isn't credited to a gone player. */
    {
        BYTE s;
        memset(&sim->roundStats[playerNum], 0, sizeof(sim->roundStats[playerNum]));
        for (s = 0; s < MAX_TANKS; s++) {
            sim->roundStats[s].killsOf[playerNum]  = 0;
            sim->roundStats[s].killedBy[playerNum] = 0;
        }
        minesClearOwner(&sim->sim.mns, playerNum);
        {
            uint16_t r, w = 0;
            for (r = 0; r < sim->notableEventCount; r++) {
                const NotableEvent *ne = &sim->notableEvents[r];
                if (ne->actorA == playerNum || ne->actorB == playerNum) continue;
                if (w != r) sim->notableEvents[w] = *ne;
                w++;
            }
            sim->notableEventCount = w;
        }
    }

    /* Record ownership for rejoin before migration changes it */
    {
        char pName[PLAYER_NAME_LEN];
        PlayerBitMap pillBits = 0, baseBits = 0;
        BYTE numPills = pillsGetNumPills(&sim->sim.pb);
        BYTE numBases = basesGetNumBases(&sim->sim.bs);
        BYTE i;
        playersGetPlayerName(&sim->sim.plyrs, playerNum, pName, sizeof(pName),
                             TRUE);
        for (i = 1; i <= numPills; i++) {
            if (pillsGetPillOwner(&sim->sim.pb, i) == playerNum) {
                pillBits |= (1u << (i - 1));
            }
        }
        for (i = 1; i <= numBases; i++) {
            if (basesGetBaseOwner(&sim->sim.bs, i) == playerNum) {
                baseBits |= (1u << (i - 1));
            }
        }
        if (pName[0] != '\0' && (pillBits != 0 || baseBits != 0)) {
            playersRejoinAddPlayer(pName, pillBits, baseBits);
        }
    }

    /* Migrate or neutralize pillboxes owned by the leaving player */
    {
        BYTE numPills = pillsGetNumPills(&sim->sim.pb);
        BYTE i;
        for (i = 1; i <= numPills; i++) {
            if (pillsGetPillOwner(&sim->sim.pb, i) == playerNum) {
                /* Look for a connected allied player to inherit */
                BYTE newOwner = NEUTRAL;
                BYTE k;
                for (k = 0; k < MAX_TANKS; k++) {
                    if (k != playerNum && sim->playerConnected[k] && playersIsAllie(&sim->sim.plyrs, playerNum, k)) {
                        newOwner = k;
                        break;
                    }
                }
                pillsSetPillOwner(&sim->sim, &sim->sim.pb, i, newOwner, TRUE);
            }
        }
    }

    /* Migrate or neutralize bases owned by the leaving player */
    {
        BYTE numBases = basesGetNumBases(&sim->sim.bs);
        BYTE i;
        for (i = 1; i <= numBases; i++) {
            if (basesGetBaseOwner(&sim->sim.bs, i) == playerNum) {
                BYTE newOwner = NEUTRAL;
                BYTE k;
                for (k = 0; k < MAX_TANKS; k++) {
                    if (k != playerNum && sim->playerConnected[k] && playersIsAllie(&sim->sim.plyrs, playerNum, k)) {
                        newOwner = k;
                        break;
                    }
                }
                basesSetBaseOwner(&sim->sim, i, newOwner, TRUE);
            }
        }
    }

    /* Drop the leaving slot from every alliance bitmap — its own, and
     * every other slot's reference to it. Without this, a new player
     * taking the vacated slot is silently inherited as an ally by the
     * old team (because other slots still have the bit set), and the
     * round-end team-carry-forward at serverSimResetGameAndReturnToLobby
     * walks playersIsAllie and propagates the stale grouping into next-
     * round teamNumber. Done after pill/base migration above, which
     * needs the still-intact alliance info to pick an heir. */
    playersLeaveAlliance(&sim->sim, &sim->sim.plyrs, NEUTRAL, playerNum, TRUE);
    {
        ControlEvent allyLeaveEvt;
        memset(&allyLeaveEvt, 0, sizeof(allyLeaveEvt));
        allyLeaveEvt.type = CTRL_ALLIANCE_LEAVE;
        allyLeaveEvt.u.allianceLeave.playerNum = playerNum;
        serverSimPublishControl(sim, &allyLeaveEvt);
    }

    /* Force immediate full sync so clients see ownership changes right away */
    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));

    /* Drop the departing slot's view state: the decay clocks it owns as a
     * viewer, every other slot's clock for it as a target, the view it
     * reported, and the last position its own view was anchored to. Anyone
     * who was viewing through the leaver's tank goes back to their own. */
    {
        BYTE k;
        memset(sim->pillNearTick[playerNum], 0, sizeof(sim->pillNearTick[playerNum]));
        memset(sim->baseNearTick[playerNum], 0, sizeof(sim->baseNearTick[playerNum]));
        memset(sim->allyNearTick[playerNum], 0, sizeof(sim->allyNearTick[playerNum]));
        sim->viewKind[playerNum]   = VIEW_KIND_TANK;
        sim->viewTarget[playerNum] = 0;
        for (k = 0; k < MAX_TANKS; k++) {
            sim->allyNearTick[k][playerNum] = 0;
            if (sim->viewKind[k] == VIEW_KIND_ALLY &&
                sim->viewTarget[k] == playerNum) {
                sim->viewKind[k]   = VIEW_KIND_TANK;
                sim->viewTarget[k] = 0;
            }
        }
    }
    sim->lastTankMX[playerNum] = 0;
    sim->lastTankMY[playerNum] = 0;
    sim->lastTankValid[playerNum] = false;

    /* Clear lobby state */
    sim->lobbyPlayers[playerNum].teamNumber = 0;
    sim->lobbyPlayers[playerNum].ready = FALSE;
    sim->lobbyPlayers[playerNum].isBot = FALSE;
    sim->lobbyPlayers[playerNum].startIdx = 0xFF;
    sim->mapSkipVotes[playerNum] = false;

    /* Check if disconnect pushes skip votes over threshold */
    if (sim->lobbyEnabled && sim->state == serverStateLobby && (sim->mapDirCount > 1 || sim->randomMapEnabled)) {
        int voteCount = 0;
        int connectedHumans = 0;
        BYTE k;
        for (k = 0; k < MAX_TANKS; k++) {
            if (!sim->playerConnected[k] || sim->lobbyPlayers[k].isBot) continue;
            connectedHumans++;
            if (sim->mapSkipVotes[k]) voteCount++;
        }
        if (connectedHumans > 0 && voteCount * 2 > connectedHumans) {
            WB_LOG_INFO(WB_LOG_CAT_SERVER, "Map skip: disconnect pushed votes over threshold (%d/%d), skipping map", voteCount, connectedHumans);
            if (sim->randomMapEnabled) {
                serverSimRandomMapRegenerate(sim);
            } else {
                serverSimMapDirPickRandom(sim);
            }
            serverSimMapSkipVotesReset(sim);
            publishMapSkipState(sim);
            serverSimWbnLobbyUpdate(sim, FALSE);
        }
    }

    /* If in countdown and someone disconnects, revert to lobby. Route
     * through serverSimAbortCountdown rather than mutating state inline
     * so the CTRL_GAME_PHASE_LOBBY publish fires — without it, remote
     * clients' netStat stays at netLobbyCountdown and their overlay
     * doesn't clear. The disconnect path through serverDisconnectClient
     * already aborts via lobbyAutoUnreadyOnChange, so this site is a
     * no-op there (state is already Lobby); it carries the abort for
     * the non-UDP callers — bot removal and local-transport
     * disconnect via client_net.c — that don't share that path. */
    if (sim->lobbyEnabled && sim->state == serverStateCountdown) {
        serverSimAbortCountdown(sim);
        logAddEvent(log_CountdownCancel, 0, 0, 0, 0, 0, NULL);
        serverSimConsoleMessage("Countdown cancelled — player disconnected.");
    }

    /* Re-check all-ready after disconnect (may need to re-trigger or cancel) */
    if (sim->lobbyEnabled && sim->state == serverStateLobby) {
        serverSimLobbyCheckAllReady(sim);
    }

    /* Notify clients that this player left */
    {
        GameEvent ev;
        ev.type = EVENT_PLAYER_LEAVE;
        memset(ev.data, 0, sizeof(ev.data));
        ev.data[0] = playerNum;
        serverSimAddEvent(sim, &ev);
    }

    /* Player composition changed — any pending balance proposal is now
     * sized against a stale roster. Dismiss and broadcast the cleared
     * state so balanceProposalActive flips back to false on every
     * subscriber. */
    if (serverSimGetBalanceProposal(sim)->pending) {
        ControlEvent evt;
        serverSimClearBalanceProposal(sim);
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_BALANCE_PROPOSAL;
        serverSimPublishControl(sim, &evt);
    }

    /* Clear the players-struct identity for the vacated slot. Done last,
     * after every read above that needs the departing player's name /
     * alliances (CTRL_PLAYER_LEAVE fill, rejoin-ownership record, ally
     * migration). Without this the slot stays inUse with the old name and
     * the join sync-replay's inUse-gated CTRL_PLAYER_JOIN loop re-announces
     * the departed player or bot to every new client as a frozen phantom —
     * it never receives snapshot updates, which gate on playerConnected.
     * This is the identity teardown serverSimResetGameWorld's comment
     * already delegates to the leave path. */
    playersClearSlot(&sim->sim.plyrs, playerNum);

    /* If the departing slot was the host, hand the role to the lowest-
     * numbered connected human. Bots can never host; if no humans remain,
     * fall back to slot 0. The setter publishes the lobby settings. */
    if (playerNum == sim->hostSlot) {
        BYTE next = 0;
        for (BYTE i = 0; i < MAX_TANKS; i++) {
            if (sim->playerConnected[i] && !serverSimIsBot(sim, i)) {
                next = i;
                break;
            }
        }
        serverSimSetHostSlot(sim, next);
    }

    /* Last human out of the lobby — wipe the slate so the next joiner gets
     * a fresh lobby: drop any bots, restore the operator's startup settings,
     * and unlock. Gated on a human leaver (bots removed here don't recurse)
     * and on the lobby state (running-game departures are handled by the
     * return-to-lobby / empty-reset paths). */
    if (!wasBot && sim->lobbyEnabled && sim->state == serverStateLobby &&
        serverSimGetNumHumans(sim) == 0) {
        serverSimResetLobbyToDefaults(sim);
    }
}

void serverSimSetTeamBatch(ServerSim *sim, BYTE playerNum, BYTE teamNumber) {
    if (playerNum >= MAX_TANKS) {
        return;
    }
    if (teamNumber >= MAX_TANKS) {
        teamNumber = 1;
    }
    sim->lobbyPlayers[playerNum].teamNumber = teamNumber;
}

void serverSimSetTeam(ServerSim *sim, BYTE playerNum, BYTE teamNumber) {
    serverSimSetTeamBatch(sim, playerNum, teamNumber);
    serverSimReapplyTeamAlliances(sim);
    /* Re-cluster the slot's reserved start to its new team now the team is
     * written. The helper frees the slot's own current reservation back into
     * the candidate pool (so the existing start can be re-chosen) and clusters
     * toward same-team holders, or falls to farthest-first when the new team
     * has no other members. No-ops outside lobby state or for an unconnected
     * slot, so the headless/batch drivers are unaffected. Callers republish
     * the slot themselves. */
    serverSimAssignLobbyStartOnJoin(sim, playerNum);
}

void serverSimSetLobbyStartIdx(ServerSim *sim, BYTE slot, BYTE idx) {
    if (slot >= MAX_TANKS) {
        return;
    }
    sim->lobbyPlayers[slot].startIdx = idx;
}

void serverSimSetReady(ServerSim *sim, BYTE playerNum, bool ready) {
    if (playerNum >= MAX_TANKS) {
        return;
    }
    if (!sim->lobbyEnabled) {
        return;
    }
    sim->lobbyPlayers[playerNum].ready = ready ? TRUE : FALSE;
}

void serverSimAcceptAlliance(ServerSim *sim, BYTE accepter, BYTE newMember) {
    GameSim *gs;
    ControlEvent evt;
    if (sim == NULL) {
        return;
    }
    gs = serverSimGetGameSim(sim);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, accepter, newMember, TRUE);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_ACCEPT;
    evt.u.allianceAccept.acceptedBy = accepter;
    evt.u.allianceAccept.newMember  = newMember;
    serverSimPublishControl(sim, &evt);
    /* WBN tracker + replay-log side effects live here so every input
     * source (UDP wire, local transport, headless cmd-stdin) fires
     * them uniformly. winbolonetAddEvent is gated internally by
     * winbolonetIsRunning(), so SP / non-WBN-aware builds pay nothing.
     * logAddEvent is gated by whether a replay log is open. */
    winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_JOIN, TRUE, accepter, newMember,
                       botManagerIsBot(sim, accepter), botManagerIsBot(sim, newMember));
    logAddEvent(log_AllyAccept, accepter, newMember, 0, 0, 0, NULL);
}

void serverSimLeaveAlliance(ServerSim *sim, BYTE playerNum) {
    GameSim *gs;
    ControlEvent evt;
    if (sim == NULL) {
        return;
    }
    gs = serverSimGetGameSim(sim);
    playersLeaveAlliance(gs, &gs->plyrs, NEUTRAL, playerNum, TRUE);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_LEAVE;
    evt.u.allianceLeave.playerNum = playerNum;
    serverSimPublishControl(sim, &evt);
    /* WBN + replay-log side effects — see serverSimAcceptAlliance. */
    winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_LEAVE, TRUE,
                       playerNum, WINBOLO_NET_NO_PLAYER,
                       botManagerIsBot(sim, playerNum), FALSE);
    logAddEvent(log_AllyLeave, playerNum, 0, 0, 0, 0, NULL);
}

void serverSimSetPlayerName(ServerSim *sim, BYTE playerNum, const char *name) {
    GameSim *gs;
    ControlEvent evt;
    char nameBuf[PACKET_MAX_PLAYER_NAME];
    if (sim == NULL || name == NULL) {
        return;
    }
    gs = serverSimGetGameSim(sim);
    strncpy(nameBuf, name, sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    playersSetPlayerName(NULL, gs, &gs->plyrs, NEUTRAL, playerNum, nameBuf, TRUE);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_NAME;
    evt.u.playerName.playerNum = playerNum;
    snprintf(evt.u.playerName.name, PACKET_MAX_PLAYER_NAME, "%s", nameBuf);
    serverSimPublishControl(sim, &evt);
}

/* Reserve a free lobby start for one slot, storing it in lobbyStartIdx.
 * No-op (leaves startIdx at 0xFF) outside lobby state, for an unconnected
 * slot, or when the lobby map has no starts. Builds the taken set from
 * every other connected slot's reservation and the teammate set from
 * same-team holders, then picks a clustered (or farthest-first when
 * teamless) start. The slot's own current reservation is ignored, so
 * this is safe to call to re-pick a slot that already holds one. Does
 * not publish — callers republish the slot (the join/team-set/add paths
 * already do, and the map-change reconcile publishes reassigned slots),
 * which keeps the reservation out of the add-time event stream. Runs for
 * humans and bots alike. */
void serverSimAssignLobbyStartOnJoin(ServerSim *sim, BYTE slot) {
    BYTE numStarts;
    bool taken[MAX_STARTS];
    BYTE teammateStarts0[MAX_TANKS];
    int  teammateCount = 0;
    BYTE myTeam;
    BYTE picked;
    BYTE i;
    BYTE k;

    if (sim == NULL) return;
    if (sim->state != serverStateLobby) return;
    if (slot >= MAX_TANKS) return;
    if (!sim->playerConnected[slot]) return;
    numStarts = startsGetNumStarts(&sim->sim.ss);
    if (numStarts == 0) return;

    /* Scripted scenario: on_choose_start decides EVERY real placement
     * (startsGetStart consults it ahead of these reservations), so the
     * lobby preview must show the script's answer, not the generic
     * clustering — which happily parked the seeded enemy bots on the
     * defenders' inner starts and made the roster look miswired. */
    {
        BYTE scIdx = MAX_STARTS;
        if (scenarioChooseStart(sim, slot, &scIdx) && scIdx < numStarts) {
            sim->lobbyPlayers[slot].startIdx = (BYTE)(scIdx + 1);
            return;
        }
    }

    for (i = 0; i < MAX_STARTS; i++) {
        taken[i] = false;
    }
    myTeam = sim->lobbyPlayers[slot].teamNumber;

    /* taken[] = every connected slot's reservation (1-based -> 0-based);
     * teammateStarts0[] = same-team holders' reservations. Team 0
     * (unassigned) has no teammates, so it falls to farthest-first. */
    for (k = 0; k < MAX_TANKS; k++) {
        BYTE r;
        if (k == slot) continue; /* never count our own current reservation */
        if (!sim->playerConnected[k]) continue;
        r = sim->lobbyPlayers[k].startIdx;
        if (r == 0xFF) continue;
        if (r < 1 || r > numStarts) continue;
        taken[r - 1] = true;
        if (myTeam != 0 && sim->lobbyPlayers[k].teamNumber == myTeam) {
            teammateStarts0[teammateCount++] = (BYTE)(r - 1);
        }
    }

    picked = startsPickIncremental(&sim->sim, &sim->sim.ss, taken,
                                   teammateStarts0, teammateCount);
    if (picked >= numStarts) {
        sim->lobbyPlayers[slot].startIdx = 0xFF;
    } else {
        sim->lobbyPlayers[slot].startIdx = (BYTE)(picked + 1);
    }
}
