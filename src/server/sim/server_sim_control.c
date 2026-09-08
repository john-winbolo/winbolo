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
 *Name:          Server Simulation Control Events
 *Filename:      server_sim_control.c
 *Author:        John Morrison
 *Purpose:
 *  The control-event bus — the per-event fillers, the
 *  subscriber registry and its join-time sync, and the
 *  fan-out hub every other source publishes through.
 *********************************************************/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server_sim_shared.h"      /* serverSimTrackAppend, serverSimSetActive, and the serverSimFillMapSkipStateEvent declaration */
#include "server_sim_internal.h"
#include "round_stats_derive.h"     /* roundStatsApplyRecord — serverSimAddEvent's per-round stats funnel */
#include "lobby_bot_pools.h"        /* lobbyBotPoolsSerialize — the bot-pool catalog streamed during sync */
#include "client_sim_control.h"     /* clientSimApplyControl — the in-process subscriber's deliver */
#include "transport_control_codec.h"   /* the body encoders the ring keyframe's control snapshot writes */
#include "log_internal.h"           /* serverSimSerializeControlSnapshot prototype */
#include "../../winbolonet/winbolonet_core.h"     /* winbolonetIsRunning — the lobby-settings WBN availability flag */
#include "../../winbolonet/winbolonet_server.h"   /* winbolonetServerRequestBalance — the WBN team-balance request */
#include "../../common/mp_diag_log.h"
#include "../../common/wb_log.h"   /* WB_LOG_INFO — the newswire-mute flip trace */

void serverSimAddEvent(ServerSim *sim, const GameEvent *event) {
    /* Per-round stats funnel. Runs before the snapshot-event buffering below
     * so a full event buffer never drops a stat. Only during a running game,
     * so any state-load/replay re-emit can't double-count. */
    if (sim->state == serverStateRunning) {
        const uint8_t *d = event->data;
        switch (event->type) {
        case EVENT_TANK_KILLED: {
            AttrKillRecord r;
            r.type = ATTR_REC_KILL; r.tick = sim->tick;
            r.killer = d[0]; r.killed = d[1]; r.deathCause = d[2];
            r.carriedPills = d[3]; r.treesWasted = d[4];
            r.mapX = d[5]; r.mapY = d[6];   /* stashed in serverSimCbTankKill */
            serverSimTrackAppend(sim, &r, sizeof r);
            roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                                  &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
            break;
        }
        case EVENT_PILL_CAPTURED:
        case EVENT_BASE_CAPTURED: {
            AttrCaptureRecord r;
            r.type = ATTR_REC_CAPTURE; r.tick = sim->tick;
            r.target = (event->type == EVENT_PILL_CAPTURED)
                           ? ATTR_CAP_TGT_PILL : ATTR_CAP_TGT_BASE;
            r.targetIndex = d[3];   /* pill/base array index (server-internal, past wire size) */
            r.newOwner = d[0]; r.prevOwner = d[1]; r.captureClass = d[2];
            r.mapX = d[4]; r.mapY = d[5];   /* pill/base map cell, stashed at emit */
            serverSimTrackAppend(sim, &r, sizeof r);
            roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                                  &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
            break;
        }
        case EVENT_LGM_LOST: {
            AttrLgmRecord r;
            r.type = ATTR_REC_LGM; r.tick = sim->tick;
            r.victim = d[0]; r.killer = d[1];
            r.mapX = d[2]; r.mapY = d[3];   /* LGM map cell, stashed at emit */
            serverSimTrackAppend(sim, &r, sizeof r);
            roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                                  &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
            break;
        }
        default: break;
        }
    }
    if (sim->eventCount < MAX_SNAPSHOT_EVENTS) {
        sim->events[sim->eventCount] = *event;
        sim->eventCount++;
    }
}

void serverSimClearBalanceProposal(ServerSim *sim) {
    memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
}

/* serverSimBufferLobbyChat — append a chat event to the current-session
 * lobby-chat catch-up buffer, dropping the oldest entry when full. Only
 * called for events that should be replayed to a returning spectator
 * (broadcast player chat + spectator chat captured during lobby/countdown);
 * gating is the caller's responsibility. The event is stored verbatim so the
 * drain-flip replay (serverSimReplayLobbyChat) re-delivers exactly what was
 * fanned live. */
static void serverSimBufferLobbyChat(ServerSim *sim, const ControlEvent *evt) {
    if (sim->lobbyChatCount == LOBBY_CHAT_BUFFER_MAX) {
        memmove(&sim->lobbyChatBuffer[0], &sim->lobbyChatBuffer[1],
                (LOBBY_CHAT_BUFFER_MAX - 1) * sizeof(sim->lobbyChatBuffer[0]));
        sim->lobbyChatCount = LOBBY_CHAT_BUFFER_MAX - 1;
    }
    sim->lobbyChatBuffer[sim->lobbyChatCount++] = *evt;
}

/* ---------------------------------------------------------------------- */
/* Subscriber registry                                                    */
/* ---------------------------------------------------------------------- */

/* SUBSCRIBER_SLOT_COUNT is defined in server_sim_internal.h (it sizes the
 * subscriber arrays on the ServerSim struct). */
#define SUBSCRIBER_HANDLE_ENCODE(slot, gen) (((int)(slot) << 16) | (uint16_t)(gen))
#define SUBSCRIBER_HANDLE_SLOT(h)           (((h) >> 16) & 0xFFFF)
#define SUBSCRIBER_HANDLE_GEN(h)            ((uint16_t)((h) & 0xFFFF))

static netStatus serverPhaseToNetStat(ServerState s) {
    switch (s) {
    case serverStateLobby:     return netLobby;
    case serverStateCountdown: return netLobbyCountdown;
    case serverStateRunning:   return netRunning;
    case serverStateGameOver:  return netLobby;
    }
    return netLobby;
}

void serverSimFillGamePhaseEvent(const ServerSim *sim, ControlEvent *evt) {
    switch (sim->state) {
    case serverStateLobby:     evt->type = CTRL_GAME_PHASE_LOBBY;     break;
    case serverStateCountdown: evt->type = CTRL_GAME_PHASE_COUNTDOWN; break;
    case serverStateRunning:   evt->type = CTRL_GAME_PHASE_RUNNING;   break;
    case serverStateGameOver:  evt->type = CTRL_GAME_PHASE_GAME_OVER; break;
    default:                   evt->type = CTRL_GAME_PHASE_LOBBY;     break;
    }
    /* countdownTicks is a 50Hz counter; round up so a partial second still
     * surfaces as 1 rather than 0 to a freshly-synced subscriber. */
    if (sim->countdownTicks > 0) {
        evt->u.gamePhase.countdownSeconds = (int)((sim->countdownTicks + 49) / 50);
    } else {
        evt->u.gamePhase.countdownSeconds = 0;
    }
}

void serverSimFillLobbySettingsEvent(ServerSim *sim, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_SETTINGS;
    memset(evt->u.lobbySettings.mapName, 0, MAP_STR_SIZE);
    snprintf(evt->u.lobbySettings.mapName, MAP_STR_SIZE, "%s", sim->mapName);
    evt->u.lobbySettings.lobbyGameType    = gameTypeGet(&sim->sim.game);
    evt->u.lobbySettings.lobbyHiddenMines = sim->sim.hiddenMines ? true : false;
    evt->u.lobbySettings.lobbyAiType      = (uint8_t)sim->botAiType;
    evt->u.lobbySettings.lobbyTimeLimit   = sim->gameLength;
    evt->u.lobbySettings.lobbyStartDelay  = serverSimGetStartDelay(sim);
    evt->u.lobbySettings.lobbyPillCount   = pillsGetNumPills(&sim->sim.pb);
    evt->u.lobbySettings.lobbyBaseCount   = basesGetNumBases(&sim->sim.bs);
    evt->u.lobbySettings.lobbyStartCount  = startsGetNumStarts(&sim->sim.ss);
    evt->u.lobbySettings.mapSkipAvailable =
        (sim->mapDirCount > 1 || sim->randomMapEnabled) ? true : false;
    evt->u.lobbySettings.netStat          = serverPhaseToNetStat(sim->state);
    evt->u.lobbySettings.hasLobby         = sim->lobbyEnabled ? true : false;
    evt->u.lobbySettings.lobbyOpenHost            = sim->openHost;
    evt->u.lobbySettings.hostSlot                 = sim->hostSlot;
    evt->u.lobbySettings.lobbyAutoLockOnGameStart = sim->autoLockOnGameStart;
    evt->u.lobbySettings.lobbyRanked              = sim->ranked;
    evt->u.lobbySettings.lobbyAllowNewPlayers     = sim->allowNewPlayers;
    evt->u.lobbySettings.lobbyWbnAvailable        = winbolonetIsRunning();
    evt->u.lobbySettings.lobbyServerLocks         = sim->serverLocks;
    evt->u.lobbySettings.uploadPolicy             = sim->uploadPolicy;
    for (int vc = 0; vc < VIEW_CATEGORY_COUNT; vc++) {
        evt->u.lobbySettings.viewPolicy[vc]    = sim->viewPolicy[vc];
        evt->u.lobbySettings.viewDecaySecs[vc] = sim->viewDecaySecs[vc];
    }
    evt->u.lobbySettings.lobbyClassicMode = sim->classicMode;
    evt->u.lobbySettings.lobbyAlliesInTrees = sim->alliesInTrees;
}

void serverSimFillLobbySlotEvent(ServerSim *sim, BYTE i, ControlEvent *evt) {
    ClientLobbySlot slot;
    memset(&slot, 0, sizeof(slot));
    slot.connected = sim->playerConnected[i] ? true : false;
    if (slot.connected) {
        const char *name = sim->sim.plyrs->item[i].playerName;
        strncpy(slot.playerName, name, PACKET_MAX_PLAYER_NAME - 1);
        slot.playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
        slot.teamNumber = sim->lobbyPlayers[i].teamNumber;
        slot.ready      = sim->lobbyPlayers[i].ready;
        slot.isBot      = sim->lobbyPlayers[i].isBot;
        slot.startIdx   = sim->lobbyPlayers[i].startIdx;
        /* sim->playerPing[i] is only refreshed by queueInput; in lobby
         * no inputs flow, so it sits at 0 the whole time. The PING/PONG
         * handler keeps udpServer.clients[i].pingMs live across every
         * state, so route through that for the lobby fill. */
        slot.pingMs     = transportUdpServerGetClientPing(i);
        slot.countryCode[0] = sim->sim.plyrs->item[i].location[0];
        slot.countryCode[1] = sim->sim.plyrs->item[i].location[1];
        slot.countryCode[2] = '\0';
        slot.clientType  = playersGetClientType(&sim->sim.plyrs, i);
        slot.clientFlags = playersGetClientFlags(&sim->sim.plyrs, i);
    }
    evt->type = CTRL_LOBBY_SLOT;
    evt->u.lobbySlot.playerNum = i;
    evt->u.lobbySlot.slot = slot;
}

void serverSimFillPlayerJoinEvent(ServerSim *sim, BYTE i, ControlEvent *evt) {
    PlayerBitMap allies = playersGetAlliesBitMap(&sim->sim.plyrs, i);
    BYTE numAllies = 0;
    BYTE bit;

    evt->type = CTRL_PLAYER_JOIN;
    evt->u.playerJoin.playerNum = i;
    memset(evt->u.playerJoin.name, 0, PACKET_MAX_PLAYER_NAME);
    strncpy(evt->u.playerJoin.name, sim->sim.plyrs->item[i].playerName,
            PACKET_MAX_PLAYER_NAME - 1);
    evt->u.playerJoin.country[0] = sim->sim.plyrs->item[i].location[0];
    evt->u.playerJoin.country[1] = sim->sim.plyrs->item[i].location[1];
    evt->u.playerJoin.country[2] = '\0';
    evt->u.playerJoin.clientType  = playersGetClientType(&sim->sim.plyrs, i);
    evt->u.playerJoin.clientFlags = playersGetClientFlags(&sim->sim.plyrs, i);
    for (bit = 0; bit < MAX_TANKS && numAllies < MAX_TANKS; bit++) {
        if (allies & ((PlayerBitMap)1u << bit)) {
            evt->u.playerJoin.allies[numAllies++] = bit;
        }
    }
    evt->u.playerJoin.numAllies = numAllies;
}

void serverSimFillPlayerLeaveEvent(ServerSim *sim, BYTE i, ControlEvent *evt) {
    evt->type = CTRL_PLAYER_LEAVE;
    evt->u.playerLeave.playerNum = i;
    memset(evt->u.playerLeave.name, 0, PACKET_MAX_PLAYER_NAME);
    strncpy(evt->u.playerLeave.name, sim->sim.plyrs->item[i].playerName,
            PACKET_MAX_PLAYER_NAME - 1);
    evt->u.playerLeave.country[0] = sim->sim.plyrs->item[i].location[0];
    evt->u.playerLeave.country[1] = sim->sim.plyrs->item[i].location[1];
    evt->u.playerLeave.country[2] = '\0';
}

void serverSimFillLobbyTeamMetaEvent(const ServerSim *sim, BYTE teamId, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_TEAM_META;
    evt->u.lobbyTeamMeta.teamId = teamId;
    if (teamId == 0 || teamId >= MAX_TANKS) {
        evt->u.lobbyTeamMeta.in_use     = 0;
        evt->u.lobbyTeamMeta.color      = 0;
        evt->u.lobbyTeamMeta.namingPool = 0;
        evt->u.lobbyTeamMeta.startSide  = 0;
        evt->u.lobbyTeamMeta.name[0]    = '\0';
        return;
    }
    evt->u.lobbyTeamMeta.in_use     = sim->teams[teamId].in_use;
    evt->u.lobbyTeamMeta.color      = sim->teams[teamId].color;
    evt->u.lobbyTeamMeta.namingPool = sim->teams[teamId].namingPool;
    evt->u.lobbyTeamMeta.startSide  = sim->teams[teamId].startSide;
    memset(evt->u.lobbyTeamMeta.name, 0, LOBBY_TEAM_NAME_LEN);
    strncpy(evt->u.lobbyTeamMeta.name, sim->teams[teamId].name,
            LOBBY_TEAM_NAME_LEN - 1);
}

void serverSimFillLobbyBotConfigEvent(ServerSim *sim, BYTE slot, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_BOT_CONFIG;
    evt->u.lobbyBotConfig.slot = slot;
    memset(evt->u.lobbyBotConfig.name, 0, PACKET_MAX_PLAYER_NAME);
    if (slot >= MAX_TANKS) {
        evt->u.lobbyBotConfig.difficulty  = 0;
        evt->u.lobbyBotConfig.personality = 0;
        return;
    }
    evt->u.lobbyBotConfig.difficulty  = sim->botConfigs[slot].difficulty;
    evt->u.lobbyBotConfig.personality = sim->botConfigs[slot].personality;
    if (sim->playerConnected[slot]) {
        strncpy(evt->u.lobbyBotConfig.name,
                sim->sim.plyrs->item[slot].playerName,
                PACKET_MAX_PLAYER_NAME - 1);
    }
}

void serverSimFillLobbyBotBrainEvent(const ServerSim *sim, BYTE slot, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_BOT_BRAIN;
    evt->u.lobbyBotBrain.slot = slot;
    evt->u.lobbyBotBrain.brainIdx = 0xFF;
    if (slot >= MAX_TANKS) return;
    evt->u.lobbyBotBrain.brainIdx = sim->botBrainIdx[slot];
}

void serverSimFillLobbyBrainListEvent(const ServerSim *sim, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_BRAIN_LIST;
    evt->u.lobbyBrainList.list = sim->brainList;
}

/* Fill a CTRL_GAME_VOTE_STATE event for the given vote kind. Returns false
 * if there's no snapshot (caller must not deliver). Mirrors the inline
 * publish at publishGameVoteState. */
static bool serverSimFillGameVoteStateEvent(const ServerSim *sim, uint8_t kind,
                                            ControlEvent *evt) {
    ServerGameVoteSnapshot snap;
    if (!serverSimGetGameVoteSnapshot(sim, kind, &snap)) return false;
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_GAME_VOTE_STATE;
    evt->u.gameVoteState.kind             = snap.kind;
    evt->u.gameVoteState.active           = snap.active;
    evt->u.gameVoteState.triggerSrc       = snap.triggerSrc;
    evt->u.gameVoteState.teamId           = snap.teamId;
    evt->u.gameVoteState.threshold        = snap.threshold;
    evt->u.gameVoteState.yesCount         = snap.yesCount;
    evt->u.gameVoteState.noCount          = snap.noCount;
    evt->u.gameVoteState.eligibleCount    = snap.eligibleCount;
    evt->u.gameVoteState.secondsRemaining = snap.secondsRemaining;
    evt->u.gameVoteState.votes            = snap.votes;
    return true;
}

/* Fill a CTRL_BALANCE_PROPOSAL event with the current proposed team
 * assignment. Mirrors the publish at server_lifecycle.c. */
static void serverSimFillBalanceProposalEvent(const ServerSim *sim,
                                              ControlEvent *evt) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_BALANCE_PROPOSAL;
    memcpy(evt->u.balanceProposal.teamForSlot,
           sim->balanceProposal.teamForSlot, MAX_TANKS);
}

/* Fill a CTRL_MAP_SKIP_STATE event with the current per-slot skip votes.
 * Mirrors the inline builders at the join + map-change publish sites. */
void serverSimFillMapSkipStateEvent(const ServerSim *sim,
                                    ControlEvent *evt) {
    BYTE k;
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_MAP_SKIP_STATE;
    for (k = 0; k < MAX_TANKS; k++) {
        evt->u.mapSkipState.votes[k] = sim->mapSkipVotes[k] ? 1 : 0;
    }
}

/* Wrapper used to enforce the documented sync ordering:
 *   a CTRL_GAME_PHASE_* event first; CTRL_PLAYER_JOIN events last
 *   (a regression that reorders sync would silently mis-initialize a
 *   subscriber, so catch it loudly in debug builds). Asserts compile
 *   out under NDEBUG.
 */
typedef struct {
    void (*inner)(void *, const struct ControlEvent *);
    void *innerCtx;
    bool sawNonPhase;
    bool sawPlayerJoin;
} SyncOrderingCheck;

static void serverSimSyncOrderingDeliver(void *ctx,
                                         const struct ControlEvent *evt) {
    SyncOrderingCheck *check = (SyncOrderingCheck *)ctx;
    bool isPhase = (evt->type == CTRL_GAME_PHASE_LOBBY ||
                    evt->type == CTRL_GAME_PHASE_COUNTDOWN ||
                    evt->type == CTRL_GAME_PHASE_RUNNING ||
                    evt->type == CTRL_GAME_PHASE_GAME_OVER);

    if (isPhase) {
        assert(!check->sawNonPhase &&
               "a CTRL_GAME_PHASE_* event must be the first event in sync");
    } else {
        check->sawNonPhase = true;
    }

    if (evt->type != CTRL_PLAYER_JOIN &&
        evt->type != CTRL_LOBBY_SYNC_COMPLETE) {
        assert(!check->sawPlayerJoin &&
               "no non-CTRL_PLAYER_JOIN event may follow "
               "CTRL_PLAYER_JOIN in sync");
    } else {
        check->sawPlayerJoin = true;
    }

    check->inner(check->innerCtx, evt);
}

static void serverSimSyncSubscriber(
    ServerSim *sim,
    void (*deliver)(void *, const struct ControlEvent *),
    void *ctx) {
    ControlEvent evt;
    BYTE i;
    SyncOrderingCheck check;
    {
        int connectedCount = 0;
        char connectedSlots[64];
        int csPos = 0;
        connectedSlots[0] = '\0';
        for (i = 0; i < MAX_TANKS; i++) {
            if (sim->playerConnected[i]) {
                connectedCount++;
                if (csPos < (int)sizeof(connectedSlots) - 8) {
                    csPos += snprintf(connectedSlots + csPos,
                                      sizeof(connectedSlots) - csPos,
                                      "%s%d", csPos == 0 ? "" : ",", (int)i);
                }
            }
        }
        mpDiagLog("[bus] SYNC-REPLAY begin state=%d connectedCount=%d connectedSlots=[%s]",
                  (int)sim->state, connectedCount, connectedSlots);
    }

    check.inner         = deliver;
    check.innerCtx      = ctx;
    check.sawNonPhase   = false;
    check.sawPlayerJoin = false;
    deliver = serverSimSyncOrderingDeliver;
    ctx     = &check;

    memset(&evt, 0, sizeof(evt));
    serverSimFillGamePhaseEvent(sim, &evt);
    deliver(ctx, &evt);

    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    deliver(ctx, &evt);

    /* BrainList (~900 bytes) is only used by the lobby AiConfig combobox.
     * Mid-game joiners don't need it during sync replay; embedding it in a
     * snapshot would risk exceeding MTU room.  The game-over → lobby
     * transition re-publishes it so the mid-game joiner gets it before
     * the lobby UI needs it. */
    if (sim->state == serverStateLobby || sim->state == serverStateCountdown) {
        memset(&evt, 0, sizeof(evt));
        serverSimFillLobbyBrainListEvent(sim, &evt);
        deliver(ctx, &evt);

        /* Bot-pool catalog: the server's themed naming pools (loaded from
         * -botnames / data/bot_names.json), zlib-compressed and streamed
         * as CTRL_LOBBY_BOT_POOL_CHUNK fragments so the joiner renders and
         * picks from the SERVER's pools rather than its own shipped file.
         * Same lobby-only gate as the brain list. */
        {
            unsigned char *blob =
                (unsigned char *)malloc(LOBBY_BOT_CATALOG_WIRE_MAX);
            if (blob) {
                int blen = lobbyBotPoolsSerialize(blob,
                                                  (int)LOBBY_BOT_CATALOG_WIRE_MAX);
                if (blen > 0) {
                    int frag = LOBBY_BOT_POOL_CHUNK_FRAG_MAX;
                    int nChunks = (blen + frag - 1) / frag;
                    int off = 0, ci;
                    if (nChunks <= 255) {
                        for (ci = 0; ci < nChunks; ci++) {
                            int fl = blen - off;
                            if (fl > frag) fl = frag;
                            memset(&evt, 0, sizeof(evt));
                            evt.type = CTRL_LOBBY_BOT_POOL_CHUNK;
                            evt.u.lobbyBotPoolChunk.seq     = (uint8_t)ci;
                            evt.u.lobbyBotPoolChunk.count   = (uint8_t)nChunks;
                            evt.u.lobbyBotPoolChunk.fragLen = (uint16_t)fl;
                            memcpy(evt.u.lobbyBotPoolChunk.frag, blob + off,
                                   (size_t)fl);
                            deliver(ctx, &evt);
                            off += fl;
                        }
                    }
                }
                free(blob);
            }
        }
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbySlotEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
    }

    /* Spectator roster — one CTRL_SPECTATOR_SLOT per connected spectator. The
     * roster lives in the transport layer, so the sim asks the registered
     * enumerator to emit the rows through this same deliver path. Feeds both the
     * live sync replay and serverSimSerializeControlSnapshot (the delayed ring
     * keyframe). */
    if (sim->specRosterEnum != NULL) {
        sim->specRosterEnum(sim->specRosterEnumCtx, deliver, ctx);
    }

    /* Team metadata for every team in use (skip team 0 — unassigned). */
    for (i = 1; i < MAX_TANKS; i++) {
        if (sim->teams[i].in_use) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbyTeamMetaEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
    }

    /* Bot config + brain for each connected bot slot. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].isBot) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbyBotConfigEvent(sim, i, &evt);
            deliver(ctx, &evt);

            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbyBotBrainEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
    }

    /* Game vote state per kind. Each fill returns false when no snapshot
     * exists, so an inactive vote contributes nothing to the replay. */
    {
        if (serverSimFillGameVoteStateEvent(sim, GAME_VOTE_KIND_BACK_TO_LOBBY, &evt)) {
            deliver(ctx, &evt);
        }
        if (serverSimFillGameVoteStateEvent(sim, GAME_VOTE_KIND_SURRENDER, &evt)) {
            deliver(ctx, &evt);
        }
    }

    /* Balance proposal — only emitted when one is pending (matches the
     * predicate the join handler uses to dismiss the proposal). */
    if (sim->balanceProposal.pending) {
        serverSimFillBalanceProposalEvent(sim, &evt);
        deliver(ctx, &evt);
    }

    /* Map skip state — emitted under the same gate as the inline publish:
     * lobby phase with a map-skip pool available. */
    if (sim->lobbyEnabled && sim->state == serverStateLobby
        && (sim->mapDirCount > 1 || sim->randomMapEnabled)) {
        serverSimFillMapSkipStateEvent(sim, &evt);
        deliver(ctx, &evt);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (playersIsInUse(&sim->sim.plyrs, i) == TRUE) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillPlayerJoinEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
    }

    /* Newswire mute — only when it is actually on, so a normal join replay
     * is unchanged. A client that connects while a scripted wave is filing
     * on or off the field must start muted, or it newswires the half of the
     * churn it arrives in time to see. The matching un-mute reaches it as a
     * live publish like everyone else's. */
    if (sim->newswireMuted) {
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_NEWSWIRE_MUTE;
        evt.u.newswireMute.muted = 1;
        deliver(ctx, &evt);
    }

    /* Terminal marker: the roster replay above re-announces every existing
     * player/slot with the subscriber already in the lobby. This final event
     * lets the subscriber tell the replay burst apart from live events, so it
     * can suppress per-event lobby sounds until the burst is done. Must be the
     * last event delivered in the sync. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SYNC_COMPLETE;
    deliver(ctx, &evt);
    mpDiagLog("[bus] SYNC-REPLAY end");
}

/* Buffer-writing sink for serverSimSerializeControlSnapshot: each delivered
 * sync event is body-encoded and appended as a [u16 type][u16 bodyLen][body]
 * record. */
typedef struct {
    BYTE *out;
    int   cap;
    int   len;
    bool  overflow;
} ControlSnapshotSink;

static void serverSimControlSnapshotDeliver(void *ctx,
                                            const struct ControlEvent *evt) {
    ControlSnapshotSink *s = (ControlSnapshotSink *)ctx;
    ControlEncodeBodyFn fn;
    uint8_t body[MAX_CONTROL_PACKET];
    size_t bodyLen = 0;
    EncodeResult r;
    int recLen;

    if (s->overflow) {
        return;  /* already failed; drain the rest of the replay silently */
    }
    fn = transportControlCodecBodyEncoder(evt->type);
    if (fn == NULL) {
        return;  /* no body codec for this kind — nothing to record */
    }
    /* Body encoders ignore the recipient (per-recipient filtering lives in the
     * delivery path), so NULL is safe here. */
    r = fn(evt, NULL, body, sizeof(body), &bodyLen);
    if (r == ENCODE_SKIP) {
        return;  /* event has no form here (e.g. an invalid slot) — skip */
    }
    if (r != ENCODE_OK) {
        s->overflow = true;  /* ENCODE_OVERFLOW: body did not fit MAX_CONTROL_PACKET */
        return;
    }
    recLen = 4 + (int)bodyLen;
    if (s->len > s->cap - recLen) {
        s->overflow = true;
        return;
    }
    s->out[s->len++] = (BYTE)(((uint16_t)evt->type >> 8) & 0xFF);
    s->out[s->len++] = (BYTE)((uint16_t)evt->type & 0xFF);
    s->out[s->len++] = (BYTE)(((uint16_t)bodyLen >> 8) & 0xFF);
    s->out[s->len++] = (BYTE)((uint16_t)bodyLen & 0xFF);
    memcpy(s->out + s->len, body, bodyLen);
    s->len += (int)bodyLen;
}

int serverSimSerializeControlSnapshot(ServerSim *sim, BYTE *out, int cap) {
    ControlSnapshotSink sink;

    if (sim == NULL || out == NULL || cap < 0) {
        return -1;
    }
    sink.out      = out;
    sink.cap      = cap;
    sink.len      = 0;
    sink.overflow = false;
    serverSimSyncSubscriber(sim, serverSimControlSnapshotDeliver, &sink);
    return sink.overflow ? -1 : sink.len;
}

/* serverSimReplayLobbyChat — re-deliver the current-session lobby-chat buffer
 * oldest->newest through the caller's deliver callback. Mirrors the sync /
 * roster-enumerator inversion: the sim owns the buffer, the transport supplies
 * delivery. Invoked only at the spectator drain-flip (after the re-register's
 * sync replay has set the lobby phase, so the events land in lobbyChatHistory);
 * never on a fresh accept or player join, which is what makes it
 * drain-flip-only. */
/* Server-wide engine-newswire switch. Publishes only on a change, so a
 * script that asks for the state it already has costs nothing; the join
 * sync above replays the ON state for late arrivals. */
void serverSimSetNewswireMute(ServerSim *sim, bool muted) {
    ControlEvent evt;
    if (sim == NULL) return;
    if (sim->newswireMuted == muted) return;
    sim->newswireMuted = muted;
    WB_LOG_INFO(WB_LOG_CAT_SERVER, "newswire mute %s (tick %u)",
                muted ? "ON" : "OFF", (unsigned)sim->tick);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_NEWSWIRE_MUTE;
    evt.u.newswireMute.muted = muted ? 1 : 0;
    serverSimPublishControl(sim, &evt);
}

bool serverSimGetNewswireMuted(const ServerSim *sim) {
    return (sim != NULL) && sim->newswireMuted;
}

void serverSimReplayLobbyChat(
    ServerSim *sim,
    void (*deliver)(void *, const struct ControlEvent *),
    void *ctx) {
    int i;
    if (sim == NULL || deliver == NULL) return;
    for (i = 0; i < sim->lobbyChatCount; i++) {
        deliver(ctx, &sim->lobbyChatBuffer[i]);
    }
}

SubscriberHandle serverSimRegisterSubscriber(
    ServerSim *sim,
    void (*deliver)(void *, const struct ControlEvent *),
    void *ctx) {
    int i;
    int slot = -1;

    if (sim == NULL || deliver == NULL) {
        return SUBSCRIBER_HANDLE_INVALID;
    }

    for (i = 0; i < SUBSCRIBER_SLOT_COUNT; i++) {
        if (sim->subscribers[i].deliver == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return SUBSCRIBER_HANDLE_INVALID;
    }

    assert(sim->subscriberGen[slot] < UINT16_MAX);
    sim->subscriberGen[slot]++;

    /* Fire the sync replay BEFORE inserting the slot into the bus
     * array, so that any side-effect publish triggered by a deliver
     * during replay reaches the existing subscribers only — not this
     * new one mid-way through its own snapshot.
     *
     * (The serverSimPublishControl reentrancy assert (!sim->publishing)
     * is the primary guard against deliver-publishes-during-replay; this
     * ordering is defence-in-depth so a release build that bypasses the
     * assert still gives the new subscriber a coherent replay rather
     * than an interleaved one.)
     *
     * The deliver function must already be ready to receive events at
     * this point — for the wire transport that means
     * udpServer.clients[slot] is fully populated before the caller
     * invokes serverSimRegisterSubscriber. */
    serverSimSyncSubscriber(sim, deliver, ctx);

    sim->subscribers[slot].deliver    = deliver;
    sim->subscribers[slot].ctx        = ctx;
    sim->subscribers[slot].generation = sim->subscriberGen[slot];
    sim->numSubscribers++;

    return SUBSCRIBER_HANDLE_ENCODE(slot, sim->subscriberGen[slot]);
}

static void serverSimDeliverToClientSim(void *ctx, const struct ControlEvent *evt) {
    /* In-process bus delivery — the host's local ClientSim (and bots) land here.
     * Log so we can tell whether the host's view divergence is at the publish
     * stage or the wire stage. */
    char extra[160];
    extra[0] = '\0';
    if (evt->type == CTRL_LOBBY_SLOT) {
        snprintf(extra, sizeof(extra),
                 " lobbySlot[player=%d team=%d ready=%d connected=%d isBot=%d name='%.12s']",
                 (int)evt->u.lobbySlot.playerNum,
                 (int)evt->u.lobbySlot.slot.teamNumber,
                 (int)evt->u.lobbySlot.slot.ready,
                 (int)evt->u.lobbySlot.slot.connected,
                 (int)evt->u.lobbySlot.slot.isBot,
                 evt->u.lobbySlot.slot.playerName);
    } else if (evt->type == CTRL_PLAYER_JOIN) {
        snprintf(extra, sizeof(extra),
                 " playerJoin[player=%d name='%.16s']",
                 (int)evt->u.playerJoin.playerNum,
                 evt->u.playerJoin.name);
    }
    mpDiagLog("[bus] in-process deliver cs=%p type=%d%s",
              ctx, (int)evt->type, extra);
    clientSimApplyControl((ClientSim *)ctx, evt);
}

SubscriberHandle serverSimRegisterClientSubscriber(ServerSim *sim, ClientSim *cs) {
    return serverSimRegisterSubscriber(sim, serverSimDeliverToClientSim, cs);
}

void serverSimSetSpectatorRosterEnumerator(ServerSim *sim,
                                           SpectatorRosterEnumFn fn,
                                           void *enumCtx) {
    if (sim == NULL) return;
    sim->specRosterEnum    = fn;
    sim->specRosterEnumCtx = enumCtx;
}

void serverSimRequestBalanceProposal(ServerSim *sim,
                                     uint8_t totalPlayers,
                                     uint8_t teamSize,
                                     const uint8_t *botSlots,
                                     uint8_t numBotSlots) {
    winbolonetServerRequestBalance(totalPlayers, teamSize,
                                   botSlots, numBotSlots,
                                   &sim->balanceProposal);
}

void serverSimUnregisterSubscriber(ServerSim *sim, SubscriberHandle h) {
    int slot;
    uint16_t gen;

    if (sim == NULL || h == SUBSCRIBER_HANDLE_INVALID) {
        return;
    }
    slot = SUBSCRIBER_HANDLE_SLOT(h);
    gen  = SUBSCRIBER_HANDLE_GEN(h);
    if (slot < 0 || slot >= SUBSCRIBER_SLOT_COUNT) {
        return;
    }
    if (sim->subscribers[slot].deliver == NULL ||
        sim->subscribers[slot].generation != gen) {
        return;
    }
    sim->subscribers[slot].deliver    = NULL;
    sim->subscribers[slot].ctx        = NULL;
    sim->subscribers[slot].generation = 0;
    if (sim->numSubscribers > 0) {
        sim->numSubscribers--;
    }
}

void serverSimPublishControl(ServerSim *sim, const struct ControlEvent *evt) {
    ControlSubscriber snapshot[SUBSCRIBER_SLOT_COUNT];
    int snapCount = 0;
    int i;

    if (sim == NULL || evt == NULL) {
        return;
    }

    /* Spectator lobby-chat catch-up capture. EVERY chat reaches the bus through
     * here — the wire CMD_CHAT dispatcher and the serverSimReceiveChat funnel
     * both publish via this one call — so capturing here (not in either caller)
     * is the single chokepoint that catches both. Broadcast player chat
     * (destPlayer 0xFF) + spectator chat, lobby/countdown only. */
    if ((sim->state == serverStateLobby || sim->state == serverStateCountdown) &&
        ((evt->type == CTRL_CHAT && evt->u.chat.destPlayer == 0xFF) ||
         evt->type == CTRL_SPECTATOR_CHAT)) {
        serverSimBufferLobbyChat(sim, evt);
    }

    /* Reentrancy guard: a deliver callback that triggers another publish
     * is a design error. */
    assert(!sim->publishing);

    /* Server is not a subscriber: double-mutating sim itself would corrupt
     * already-applied state. */
    for (i = 0; i < SUBSCRIBER_SLOT_COUNT; i++) {
        assert(sim->subscribers[i].ctx != sim);
    }

    sim->publishing = true;

    /* Iterate a snapshot of the active list so a deliver that
     * registers/unregisters does not corrupt our walk. */
    for (i = 0; i < SUBSCRIBER_SLOT_COUNT; i++) {
        if (sim->subscribers[i].deliver != NULL) {
            snapshot[snapCount++] = sim->subscribers[i];
        }
    }
    {
        char extra[256];
        extra[0] = '\0';
        if (evt->type == CTRL_LOBBY_SLOT) {
            snprintf(extra, sizeof(extra),
                     " lobbySlot[player=%d team=%d ready=%d connected=%d isBot=%d name='%.12s']",
                     (int)evt->u.lobbySlot.playerNum,
                     (int)evt->u.lobbySlot.slot.teamNumber,
                     (int)evt->u.lobbySlot.slot.ready,
                     (int)evt->u.lobbySlot.slot.connected,
                     (int)evt->u.lobbySlot.slot.isBot,
                     evt->u.lobbySlot.slot.playerName);
        } else if (evt->type == CTRL_PLAYER_JOIN) {
            snprintf(extra, sizeof(extra),
                     " playerJoin[player=%d name='%.16s']",
                     (int)evt->u.playerJoin.playerNum,
                     evt->u.playerJoin.name);
        } else if (evt->type == CTRL_LOBBY_SETTINGS) {
            snprintf(extra, sizeof(extra),
                     " settings[map='%.16s' gameType=%d hiddenMines=%d aiType=%d timeLimit=%d startDelay=%d open=%d autoLock=%d ranked=%d allowNew=%d locks=0x%04x]",
                     evt->u.lobbySettings.mapName,
                     (int)evt->u.lobbySettings.lobbyGameType,
                     (int)evt->u.lobbySettings.lobbyHiddenMines,
                     (int)evt->u.lobbySettings.lobbyAiType,
                     (int)evt->u.lobbySettings.lobbyTimeLimit,
                     (int)evt->u.lobbySettings.lobbyStartDelay,
                     (int)evt->u.lobbySettings.lobbyOpenHost,
                     (int)evt->u.lobbySettings.lobbyAutoLockOnGameStart,
                     (int)evt->u.lobbySettings.lobbyRanked,
                     (int)evt->u.lobbySettings.lobbyAllowNewPlayers,
                     (unsigned)evt->u.lobbySettings.lobbyServerLocks);
        }
        mpDiagLog("[bus] PUBLISH type=%d subscribers=%d%s",
                  (int)evt->type, snapCount, extra);
    }
    /* Make sim visible to deliver callbacks that recover it via
     * serverSimGetActive() (e.g. udpClientDeliverControl's enqueue
     * diagnostic + running-phase early-send gate). The main thread
     * already sets this per tick; worker threads (WBN balance) have
     * NULL in their TLS slot until we set it here. */
    serverSimSetActive(sim);
    for (i = 0; i < snapCount; i++) {
        snapshot[i].deliver(snapshot[i].ctx, evt);
    }

    sim->publishing = false;
}
