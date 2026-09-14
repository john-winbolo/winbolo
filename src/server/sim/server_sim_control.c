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
#include "brain_list.h"            /* brainListLoadTextsForPath — the brains' lobby texts */
#include "client_sim_control.h"     /* clientSimApplyControl — the in-process subscriber's deliver */
#include "transport_control_codec.h"   /* the body encoders the ring keyframe's control snapshot writes */
#include "log_internal.h"           /* serverSimSerializeControlSnapshot prototype */
#include "../../winbolonet/winbolonet_core.h"     /* winbolonetIsRunning — the lobby-settings WBN availability flag */
#include "../../winbolonet/winbolonet_server.h"   /* winbolonetServerRequestBalance — the WBN team-balance request */
#include "../../common/mp_diag_log.h"
#include "../../common/wb_log.h"   /* WB_LOG_INFO — the newswire-mute flip trace */

/* A ping is a team signal. The sender always sees its own (its client draws
 * nothing until the server echoes it back, so this is the only copy it gets);
 * everyone on the sender's lobby team, and anyone allied with the sender,
 * sees it too. Team 0 means "unassigned" rather than "team zero", so a
 * teamless sender pings for itself alone. */
bool serverSimPingReachesClient(ServerSim *sim, BYTE recipient, BYTE sender) {
    const LobbyPlayer *sLp;
    const LobbyPlayer *rLp;
    if (sim == NULL) return false;
    if (recipient >= MAX_TANKS || sender >= MAX_TANKS) return false;
    if (recipient == sender) return true;
    /* The recipient has muted this sender's pings. Checked after the own-copy
     * short-circuit above, so a player always sees their own ping even while
     * others have muted them, and before the team/ally rules below so a muted
     * teammate is dropped exactly as a muted voice/chat line is. A player
     * cannot mute itself (the dispatch arm rejects it), so this never fires on
     * recipient == sender in any case. */
    if ((sim->pingMuteMask[recipient] & ((PlayerBitMap)1u << sender)) != 0) {
        return false;
    }
    if (playersIsAllie(&sim->sim.plyrs, recipient, sender)) return true;
    sLp = serverSimGetLobbyPlayer(sim, sender);
    rLp = serverSimGetLobbyPlayer(sim, recipient);
    if (sLp == NULL || rLp == NULL) return false;
    if (sLp->teamNumber == 0) return false;
    return sLp->teamNumber == rLp->teamNumber;
}

void serverSimSetPingMute(ServerSim *sim, BYTE muterSlot, BYTE targetPlayer,
                          bool muted) {
    if (sim == NULL || muterSlot >= MAX_TANKS || targetPlayer >= MAX_TANKS) {
        return;
    }
    if (muted) {
        sim->pingMuteMask[muterSlot] |= (PlayerBitMap)1u << targetPlayer;
    } else {
        sim->pingMuteMask[muterSlot] &= ~((PlayerBitMap)1u << targetPlayer);
    }
}

void serverSimAddEvent(ServerSim *sim, const GameEvent *event) {
    /* Per-round stats funnel. Runs before the snapshot-event buffering below
     * so a full event buffer never drops a stat. Only during a running game,
     * so any state-load/replay re-emit can't double-count.
     *
     * And never while the scenario setup window is open. The state already
     * reads running when the round start makes its calls into the scenario,
     * so sixteen bases dealt at setup would otherwise credit a seat with
     * sixteen captures at tick 0 and put sixteen lines on the round's
     * timeline. What a script arranges before the round is nobody's doing.
     * The boot call is inside the window too, which is the same case. The
     * event still lands in the frame buffer below, where the start's own
     * truncation takes it back out. */
    if (sim->state == serverStateRunning && !sim->scenarioSetupWindow) {
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
            r.targetIndex = d[2];   /* 0-based pill/base array index; on the wire */
            r.newOwner = d[0]; r.prevOwner = d[1];
            /* d[3] is the quiet byte. The class and the square are past the
               wire size, stashed at emit for this funnel alone. */
            r.captureClass = d[4];
            r.mapX = d[5]; r.mapY = d[6];
            serverSimTrackAppend(sim, &r, sizeof r);
            roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                                  &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
            break;
        }
        case EVENT_LGM_LOST: {
            AttrLgmRecord r;
            r.type = ATTR_REC_LGM; r.tick = sim->tick;
            r.victim = d[0]; r.killer = d[1];
            /* d[2] is the quiet byte; the cell sits behind it. */
            r.mapX = d[3]; r.mapY = d[4];   /* LGM map cell, stashed at emit */
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

    /* The in-process game-event channel, the counterpart of the control
     * fan-out in serverSimPublishControl. A subscriber hears the event
     * whether or not the frame buffer had room for it above: the buffer is
     * what the snapshot stream sends to a remote client, and a full one is a
     * wire-side drop rather than a fact that did not happen.
     *
     * Nothing to do for the common case of a sim with no such subscriber, so
     * that case returns before the slot scan: this runs for every sound,
     * shell and per-tick update the engine raises.
     *
     * Walked off a snapshot of the active list, as the control fan-out is, so
     * a callback that registers or unregisters cannot corrupt the walk. And
     * guarded as the control fan-out is: this runs inside shared-code
     * mutation with the state half-written, so a subscriber that raises an
     * event or publishes a control event from here is a design error, and
     * the assert says so in Debug. A subscriber that wants to act on an event
     * queues it and acts later, which is what the scenario host does. */
    if (sim->numEventSubscribers > 0) {
        ControlSubscriber snapshot[SUBSCRIBER_SLOT_COUNT];
        int snapCount = 0;
        int i;

        assert(!sim->publishingEvent);
        for (i = 0; i < SUBSCRIBER_SLOT_COUNT; i++) {
            if (sim->subscribers[i].deliver != NULL &&
                sim->subscribers[i].deliverEvent != NULL) {
                snapshot[snapCount++] = sim->subscribers[i];
            }
        }
        sim->publishingEvent = true;
        for (i = 0; i < snapCount; i++) {
            snapshot[i].deliverEvent(snapshot[i].ctx, event);
        }
        sim->publishingEvent = false;
    }
}

void serverSimFlushPendingPings(ServerSim *sim) {
    int i;
    if (sim == NULL) return;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->hasPendingPing[i]) continue;
        /* A full frame buffer is a wait, not a loss. serverSimAddEvent drops
         * silently once the buffer is full, and this event must not be one of
         * the drops: EVENT_PING is reliable, and its sender has already been
         * answered CMD_OK, so a dropped one is a marker the player watched
         * themselves place that nobody — including them — ever sees. Leave the
         * pending flag set and let a later tick, with room again, buffer it.
         * Late by a frame or two beats gone. */
        if (sim->eventCount >= MAX_SNAPSHOT_EVENTS) break;
        sim->hasPendingPing[i] = false;
        /* Buffer the ping into the freshly-cleared per-frame event buffer so
         * both the per-client snapshot build and the UDP event drain (both run
         * after the tick) see it. The dispatch arm records but never buffers,
         * so this is the one and only add for this ping. */
        serverSimAddEvent(sim, &sim->pendingPing[i]);
    }
}

void serverSimResetPingState(ServerSim *sim) {
    if (sim == NULL) return;
    memset(sim->hasPendingPing, 0, sizeof(sim->hasPendingPing));
    memset(sim->pingLastTick, 0, sizeof(sim->pingLastTick));
    memset(sim->pingBurstTicks, 0, sizeof(sim->pingBurstTicks));
    memset(sim->pingBurstIdx, 0, sizeof(sim->pingBurstIdx));
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
    evt->u.lobbySettings.voiceMode = sim->voiceMode;
    evt->u.lobbySettings.lobbyOverviewWindow = sim->overviewWindow;
    evt->u.lobbySettings.lobbyLineOfSight    = sim->lineOfSight;
    evt->u.lobbySettings.lobbySmartPingsOff  = sim->smartPingsOff;
    /* What the lobby's scenario is, straight off what whoever attached it
       told the sim. A lobby with none leaves the source at lobbyScenarioNone
       and the strings empty, which is what keeps those bytes off the wire. */
    evt->u.lobbySettings.scenarioSource     = sim->scenarioIdentity.source;
    evt->u.lobbySettings.scenarioExtraTeams = sim->scenarioIdentity.extraTeams;
    snprintf(evt->u.lobbySettings.scenarioName,
             sizeof(evt->u.lobbySettings.scenarioName), "%s",
             sim->scenarioIdentity.name);
    snprintf(evt->u.lobbySettings.scenarioFileName,
             sizeof(evt->u.lobbySettings.scenarioFileName), "%s",
             sim->scenarioIdentity.fileName);
    snprintf(evt->u.lobbySettings.scenarioDescription,
             sizeof(evt->u.lobbySettings.scenarioDescription), "%s",
             sim->scenarioIdentity.description);
    /* The game underneath a scripted round, from where the spawn and start
       paths read it. lobbyGameType above says gameScripted for the whole of
       such a round, and a client that only had that would predict its first
       life as open. */
    evt->u.lobbySettings.scenarioBaseGame = (uint8_t)sim->sim.scenarioBaseGame;
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
        slot.fielded    = sim->lobbyPlayers[i].fielded;
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
    /* Asked here rather than at the publishes, because both of them — the
       live announce in server_sim_players.c and the sync replay below — go
       through this filler, and a wave of bots a script silenced must stay
       silent for a client that joins in the middle of it. */
    evt->u.playerJoin.quiet =
        serverSimAnnounce(sim, ANNOUNCE_KIND_JOINED, i, i) ? 0 : 1;
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
    evt->u.playerLeave.quiet =
        serverSimAnnounce(sim, ANNOUNCE_KIND_LEFT, i, i) ? 0 : 1;
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
        evt->u.lobbyBotConfig.mode        = 0;
        evt->u.lobbyBotConfig.difficulty  = 0;
        evt->u.lobbyBotConfig.personality = 0;
        return;
    }
    evt->u.lobbyBotConfig.mode        = sim->botConfigs[slot].mode;
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

/* ── The brains' lobby texts ─────────────────────────────────────────
 *
 * announce.txt and commands.txt are read off the server's disk here, at the
 * moment they are sent, rather than being kept in the ServerSim: the table is
 * ~139 KB, the send happens twice in a lobby's life (a join, and the return
 * from a round), and a file the operator edits between rounds is then picked
 * up without a restart.
 *
 * Unlike about.txt these DO travel: the server chooses the brain, so a client
 * that does not have it installed would otherwise have nothing to show.
 * Only brains that actually ship a file are sent, so the usual cost is one
 * brain's ten fragments, not sixteen brains' worth. */
void serverSimEmitBrainDocs(const ServerSim *sim,
                            void (*deliver)(void *, const struct ControlEvent *),
                            void *ctx) {
    char *announce = NULL, *docs = NULL;
    uint8_t *blob = NULL;
    int i;
    if (sim == NULL || deliver == NULL) return;
    if (sim->brainList.count <= 0) return;

    announce = (char *)malloc(BRAIN_ANNOUNCE_MAX + 1);
    docs     = (char *)malloc(BRAIN_DOCS_MAX + 1);
    blob     = (uint8_t *)malloc(LOBBY_BRAIN_DOCS_WIRE_MAX);
    if (announce == NULL || docs == NULL || blob == NULL) {
        free(announce); free(docs); free(blob);
        return;
    }

    for (i = 0; i < sim->brainList.count && i < BRAIN_LIST_MAX; i++) {
        bool truncated = false;
        size_t aLen, dLen, blen;
        int nChunks, ci;
        size_t off;
        if (!brainListLoadTextsForPath(sim->brainPaths[i],
                                       announce, (size_t)BRAIN_ANNOUNCE_MAX + 1,
                                       docs, (size_t)BRAIN_DOCS_MAX + 1,
                                       &truncated)) {
            continue;                      /* this brain ships neither file */
        }
        if (truncated) {
            WB_LOG_WARN(WB_LOG_CAT_SERVER,
                           "brain '%s': announce.txt/commands.txt is longer "
                           "than the wire allows (%d / %d bytes) and was cut",
                           sim->brainList.entries[i].name,
                           BRAIN_ANNOUNCE_MAX, BRAIN_DOCS_MAX);
        }
        aLen = strlen(announce);
        dLen = strlen(docs);
        blen = 0;
        blob[blen++] = (uint8_t)((aLen >> 8) & 0xFF);
        blob[blen++] = (uint8_t)(aLen & 0xFF);
        memcpy(blob + blen, announce, aLen); blen += aLen;
        blob[blen++] = (uint8_t)((dLen >> 8) & 0xFF);
        blob[blen++] = (uint8_t)(dLen & 0xFF);
        memcpy(blob + blen, docs, dLen); blen += dLen;

        nChunks = (int)((blen + LOBBY_BRAIN_DOCS_FRAG_MAX - 1) /
                        LOBBY_BRAIN_DOCS_FRAG_MAX);
        if (nChunks <= 0 || nChunks > 255) continue;
        off = 0;
        for (ci = 0; ci < nChunks; ci++) {
            ControlEvent evt;
            size_t fl = blen - off;
            if (fl > LOBBY_BRAIN_DOCS_FRAG_MAX) fl = LOBBY_BRAIN_DOCS_FRAG_MAX;
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_LOBBY_BRAIN_DOCS_CHUNK;
            evt.u.lobbyBrainDocsChunk.brainIdx = (uint8_t)i;
            evt.u.lobbyBrainDocsChunk.seq      = (uint8_t)ci;
            evt.u.lobbyBrainDocsChunk.count    = (uint8_t)nChunks;
            evt.u.lobbyBrainDocsChunk.fragLen  = (uint16_t)fl;
            memcpy(evt.u.lobbyBrainDocsChunk.frag, blob + off, fl);
            deliver(ctx, &evt);
            off += fl;
        }
    }
    free(announce);
    free(docs);
    free(blob);
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

/* Fill a CTRL_ENTITY_SYNC event from the live pill, base and start lists:
 * bit i of a mask is set when index i holds an item that is on the map.
 * Returns false with *evt untouched when every index of every list is on
 * the map — installing a compressed map marks exactly that, so there would
 * be nothing in the event a recipient did not already have. */
bool serverSimFillEntitySyncEvent(ServerSim *sim, ControlEvent *evt) {
    uint16_t pills  = 0;
    uint16_t bases  = 0;
    uint16_t starts = 0;
    uint16_t allPills, allBases, allStarts;
    BYTE nPills, nBases, nStarts;
    BYTE i;

    if (sim == NULL || evt == NULL) return false;

    nPills  = sim->sim.pb != NULL ? pillsGetNumPills(&sim->sim.pb)   : 0;
    nBases  = sim->sim.bs != NULL ? basesGetNumBases(&sim->sim.bs)   : 0;
    nStarts = sim->sim.ss != NULL ? startsGetNumStarts(&sim->sim.ss) : 0;

    for (i = 1; i <= nPills; i++) {
        if (pillsIsActive(&sim->sim.pb, i)) pills |= (uint16_t)(1u << (i - 1));
    }
    for (i = 1; i <= nBases; i++) {
        if (basesIsActive(&sim->sim.bs, i)) bases |= (uint16_t)(1u << (i - 1));
    }
    for (i = 1; i <= nStarts; i++) {
        if (startsIsActive(&sim->sim.ss, i)) starts |= (uint16_t)(1u << (i - 1));
    }

    /* Every index within a count set — what the install produces. */
    allPills  = (uint16_t)((1u << nPills)  - 1u);
    allBases  = (uint16_t)((1u << nBases)  - 1u);
    allStarts = (uint16_t)((1u << nStarts) - 1u);
    if (pills == allPills && bases == allBases && starts == allStarts) {
        return false;
    }

    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_ENTITY_SYNC;
    evt->u.entitySync.pills  = pills;
    evt->u.entitySync.bases  = bases;
    evt->u.entitySync.starts = starts;
    return true;
}

/* Fill a CTRL_SIM_RULES from the table this sim is running on. One
 * assignment per carried rule, generated from the same lists the codec is
 * written from, so a rule that travels on the wire cannot be one this
 * forgets to read out of the table. */
void serverSimFillSimRulesEvent(const ServerSim *sim, ControlEvent *evt) {
    if (sim == NULL || evt == NULL) return;
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_SIM_RULES;
#define SIM_RULES_FILL_FIELD(name) evt->u.simRules.name = sim->sim.rules.name;
    CTRL_SIM_RULES_ALL_FIELDS(SIM_RULES_FILL_FIELD)
#undef SIM_RULES_FILL_FIELD
}

void serverSimPublishSimRules(ServerSim *sim) {
    ControlEvent evt;
    if (sim == NULL) return;
    serverSimFillSimRulesEvent(sim, &evt);
    serverSimPublishControl(sim, &evt);
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
        evt->type != CTRL_LOBBY_SYNC_COMPLETE &&
        evt->type != CTRL_STATS_SEED) {
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

    /* The gameplay numbers this sim is running on. A joiner's own table
     * starts classic, and the round it is joining may not be on the classic
     * one — a scenario can have changed a rule before it arrived. Replayed
     * here so it starts the round reading what the server simulates with
     * rather than finding out at the first rule change after it joined. */
    serverSimFillSimRulesEvent(sim, &evt);
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

        /* The brains' own lobby texts, straight after the list they index
         * into: the lobby turns a bot's announce line into team chat and
         * hangs its commands docs off it. Same lobby-only gate. */
        serverSimEmitBrainDocs(sim, deliver, ctx);

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

    /* Which items are on the map. An in-process subscriber installs the
     * compressed map before it registers, so this replay lands on top of
     * that install and its holes stick. A wire client's map arrives later,
     * on the bulk channel, and its install would wipe these holes — the
     * copy that settles it there is the one the transfer-completion send
     * makes. Nothing is emitted while every item is on the map, which is
     * every round that runs no entity op. Placed ahead of the player-join
     * roster: the ordering check refuses a non-join event after the first
     * join. */
    if (serverSimFillEntitySyncEvent(sim, &evt)) {
        deliver(ctx, &evt);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (playersIsInUse(&sim->sim.plyrs, i) == TRUE) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillPlayerJoinEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
    }

    /* Live scoreboard seed. The client counts its per-slot board from the
     * game-event stream, so a joiner knows only what has happened since it
     * arrived; sim->roundStats already holds the running answer for every
     * slot, so hand it over. Built through serverSimBuildRoundStatsSummary so
     * the seed, the client's own counting and the end-of-round recap all read
     * the same rows. Placed here deliberately: after the CTRL_GAME_PHASE_*
     * echo that opens every replay (whose RUNNING arm wipes the client's
     * per-game counters) and after the CTRL_PLAYER_JOIN roster the rows are
     * indexed by. The running gate is also what limits this to a mid-round
     * join — a lobby or countdown join syncs in another state and has nothing
     * to seed. */
#if POSTGAME_STATS_ENABLED
    if (sim->state == serverStateRunning) {
        RoundStatsSummary seedSummary;
        serverSimBuildRoundStatsSummary(sim, &seedSummary);
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_STATS_SEED;
        evt.u.statsSeed.playerCount = seedSummary.playerCount;
        memcpy(evt.u.statsSeed.players, seedSummary.players,
               sizeof(evt.u.statsSeed.players));
        deliver(ctx, &evt);
    }
#endif

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

    sim->subscribers[slot].deliver      = deliver;
    /* Control events only until the caller asks for the second channel. */
    sim->subscribers[slot].deliverEvent = NULL;
    sim->subscribers[slot].ctx          = ctx;
    sim->subscribers[slot].generation   = sim->subscriberGen[slot];
    sim->numSubscribers++;

    return SUBSCRIBER_HANDLE_ENCODE(slot, sim->subscriberGen[slot]);
}

bool serverSimSetSubscriberEventDeliver(
    ServerSim *sim,
    SubscriberHandle h,
    void (*deliverEvent)(void *, const GameEvent *)) {
    int slot;
    uint16_t gen;

    if (sim == NULL || h == SUBSCRIBER_HANDLE_INVALID) {
        return false;
    }
    slot = SUBSCRIBER_HANDLE_SLOT(h);
    gen  = SUBSCRIBER_HANDLE_GEN(h);
    if (slot < 0 || slot >= SUBSCRIBER_SLOT_COUNT) {
        return false;
    }
    /* The same slot-and-generation test unregister makes, so a handle left
     * over from a subscriber that has gone writes nothing to the slot that
     * replaced it. */
    if (sim->subscribers[slot].deliver == NULL ||
        sim->subscribers[slot].generation != gen) {
        return false;
    }
    /* Keep the count of slots with a channel, so serverSimAddEvent can skip
       its scan when nobody is listening. */
    if (sim->subscribers[slot].deliverEvent == NULL && deliverEvent != NULL) {
        sim->numEventSubscribers++;
    } else if (sim->subscribers[slot].deliverEvent != NULL &&
               deliverEvent == NULL && sim->numEventSubscribers > 0) {
        sim->numEventSubscribers--;
    }
    sim->subscribers[slot].deliverEvent = deliverEvent;
    return true;
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
    if (sim->subscribers[slot].deliverEvent != NULL &&
        sim->numEventSubscribers > 0) {
        sim->numEventSubscribers--;
    }
    sim->subscribers[slot].deliver      = NULL;
    sim->subscribers[slot].deliverEvent = NULL;
    sim->subscribers[slot].ctx          = NULL;
    sim->subscribers[slot].generation   = 0;
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
     * is a design error, on either channel — a game-event subscriber that
     * publishes a control event from its deliver is the same mistake. */
    assert(!sim->publishing);
    assert(!sim->publishingEvent);

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
                     " settings[map='%.16s' gameType=%d hiddenMines=%d aiType=%d timeLimit=%d startDelay=%d open=%d autoLock=%d ranked=%d allowNew=%d locks=0x%08x]",
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
