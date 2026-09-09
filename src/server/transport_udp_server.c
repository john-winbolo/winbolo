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
 *Name:          Transport UDP Server
 *Filename:      transport_udp_server.c
 *Author:        John Morrison
 *Purpose:
 *  Server-side UDP network transport for multiplayer games.
 *    - Receives inputs from clients, applies to ServerSim.
 *    - Broadcasts per-player filtered snapshots each tick.
 *    - Manages connection lifecycle (join/leave/timeout).
 *    - Periodic ping/pong for latency measurement.
 *********************************************************/

#include "transport_udp_internal.h"
#include "transport_udp_server_internal.h" /* UdpServerState and the per-slot
                                            * types held in it */
#include "global.h"
#include "bases.h"
#include "pillbox.h"
#include "starts.h"
#include "players.h"
#include "client_enums.h"  /* aiType, gameType, sndEffects, updateType */
#include "viewport_types.h"  /* screen */
#include "messages.h"
#include "util.h"
#include "game_sim.h"
#include "server_sim.h"
#include "server_sim_internal.h" /* serverSimGameVoteToggle — T2 (sim co-owner) */
#include "server_lifecycle.h"
#include "control_event.h"
#include "lobby_bot_pools.h"
#include "client_sim_internal.h"  /* LOBBY_MAP_LIST_MAX cap shared with the wire */
#include "../common/md5.h"
#include "mapgen.h"
#include "brain_list_internal.h"   /* BRAIN_LIST_PATH_LEN — ADD_BOT pathLen bound */
#include "transport_control_codec.h"
#include "transport_command_codec.h"
#include "channel_mux.h"
#include "bulk_transfer.h"
#include "spectator_ring.h"
#include "wbn_key_codec.h"
#include "../winbolonet/winbolonet_core.h"
#include "../winbolonet/winbolonet_server.h"
#include "threads.h"
#include "sounddist.h"
#include "bot_manager.h"
#include "log.h"
#include "playername_validate.h"
#include "server_sim_lifecycle.h"
#include "../common/wb_log.h"
#include "../common/mp_diag_log.h"
#include "net_impair.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

/* Human-readable terrain name for map-resync diagnostics. Covers the terrain
 * byte stored in mapItem, including the mine range (10-15). */
static const char *resyncTerrainName(BYTE t) {
    switch (t) {
        case DEEP_SEA:     return "DEEP_SEA";
        case BUILDING:     return "BUILDING";
        case RIVER:        return "RIVER";
        case SWAMP:        return "SWAMP";
        case CRATER:       return "CRATER";
        case ROAD:         return "ROAD";
        case FOREST:       return "FOREST";
        case RUBBLE:       return "RUBBLE";
        case GRASS:        return "GRASS";
        case HALFBUILDING: return "HALFBUILDING";
        case BOAT:         return "BOAT";
        case MINE_SWAMP:   return "MINE_SWAMP";
        case MINE_CRATER:  return "MINE_CRATER";
        case MINE_ROAD:    return "MINE_ROAD";
        case MINE_FOREST:  return "MINE_FOREST";
        case MINE_RUBBLE:  return "MINE_RUBBLE";
        case MINE_GRASS:   return "MINE_GRASS";
        default:           return "?";
    }
}

/* ================================================================
 * SERVER SIDE
 * ================================================================ */

/* Bounds on serving the last completed round's log (PACKET_ROUND_LOG_REQ).
 * The BulkSender's busy guard is per peer, so it bounds one client's byte
 * stream and nothing else: every client has its own sender holding its own
 * malloc'd copy of the blob, and sixteen simultaneous requests would be
 * sixteen simultaneous transfers. The concurrency cap is what supplies the
 * fleet-wide bound the guard does not (worst case 2 x ROUND_LOG_MAX_BYTES
 * resident); the interval and the per-round attempt ceiling bound how often
 * one client can ask. */
#define ROUND_LOG_MAX_CONCURRENT  2
#define ROUND_LOG_MIN_REQ_TICKS   100  /* 2s at 50 Hz, between requests */
#define ROUND_LOG_MAX_ATTEMPTS    3    /* transfers started per client, per round */

/* Standalone PACKET_CHANNEL frames a downloading client gets per tick while
 * snapshots are gated (no snapshot trailer to carry the bulk stream). One
 * frame carries ~5 segments under the datagram budget, so this clears a full
 * CHANNEL_BULK window (96 segments) in a tick rather than throttling the map to
 * ~one frame/tick; the unacked window then bounds bytes in flight. */
#define MAP_DOWNLOAD_FRAMES_PER_TICK 24

/* Bounds on the catch-up sweep that keeps a culled slot's copy of the terrain
 * (mapEventQueues below carries its output). One slot sweeps per
 * MAP_SWEEP_STRIDE sim ticks — the sim advances two ticks a frame, so each
 * slot comes up every five frames — and a sweep queues at most
 * MAP_SWEEP_MAX_EVENTS squares, well under RELIABLE_EVENT_BUFFER_SIZE so the
 * catch-up can never crowd out live changes. A screen's worth of stale ground
 * clears in a handful of sweeps. */
#define MAP_SWEEP_STRIDE      5
#define MAP_SWEEP_MAX_EVENTS 64

/* Minimum interval between map-download re-asks from one client.
 *
 * A re-ask is the most expensive thing one small datagram can ask this server
 * to do: it recompresses that slot's whole copy of the terrain, re-sends
 * JOIN_ACCEPT and re-bases the bulk channel, then streams the map again. The
 * first READY of a download is not a re-ask and is never held off — this
 * bounds only the restart path.
 *
 * It has to sit *under* the honest client's fastest re-ask, not outside it.
 * A client whose stream head was consumed before its buffers existed sees no
 * progress while the server thinks it is streaming, so its watchdog re-asks on
 * the short MAP_DL_READY_RESEND_TICKS threshold (~1s) and every one of those
 * lands here on the restart path. Holding those off would drop half of a
 * recovery the client only gets MAP_DL_MAX_RESTARTS attempts at — the wedge
 * this path exists to clear. Half a second serves every one of them and still
 * takes a client that asks in a tight loop from hundreds of restarts a second
 * down to two. */
#define MAP_REASK_MIN_TICKS 25  /* 0.5s at 50 Hz, between re-asks */

/* The one definition of the server transport's file-scope state; the
 * declaration lives in transport_udp_server_internal.h. */
UdpServerState udpServer;

/* The registered round-log source. Held outside udpServer so a transport
 * create/destroy cycle does not drop the recorder's registration, and read
 * only through here: the transport names no symbol in the recorder, which is
 * what keeps the recorder (and the WinBolo.net upload it needs) out of every
 * target that links this file. Unregistered means no recorder is installed,
 * and the server answers ROUND_LOG_ERR_DISABLED without knowing one exists. */
static RoundLogSource s_roundLogSource;
static bool           s_roundLogSourceSet = false;

void transportUdpServerSetRoundLogSource(const RoundLogSource *src) {
    if (src == NULL || src->serveEnabled == NULL || src->read == NULL) {
        memset(&s_roundLogSource, 0, sizeof(s_roundLogSource));
        s_roundLogSourceSet = false;
        return;
    }
    s_roundLogSource = *src;
    s_roundLogSourceSet = true;
}

/* Clear one slot's round-log request limits. A disconnect mid-transfer runs
 * this too, so a client that reconnects is not still spending the old
 * occupant's attempts. */
void udpServerResetRoundLogLimits(int idx) {
    udpServer.roundLogReqSeen[idx]     = false;
    udpServer.roundLogLastReqTick[idx] = 0;
    udpServer.roundLogServed[idx]      = 0;
}

/* Clear one slot's map-download re-ask limit. Same reason as the round-log
 * reset above: the interval belongs to the connection, not to the slot. */
void udpServerResetMapReaskLimit(int idx) {
    udpServer.mapReaskSeen[idx]      = false;
    udpServer.mapReaskLastTick[idx]  = 0;
    udpServer.mapReaskThrottled[idx] = 0;
}

/* Outbound datagram wrapper.  Every server->peer send routes through here
 * so the outbound impairment layer can delay/drop/reorder it.  When
 * impairment is disabled (or the datagram is too large for the queue),
 * the packet goes straight onto the wire — behaviourally identical to a
 * direct udpSendTo. */
void srvSendTo(const uint8_t *buf, int len,
               const struct sockaddr_in *addr) {
    if (netImpairEnabled(&srvImpairOut) &&
        netImpairOffer(&srvImpairOut, buf, len, addr, (uint64_t)SDL_GetTicks())) {
        return;
    }
    udpSendTo(udpServer.sock, buf, len, addr);
}

/* Forward declaration */
static bool serverClientsAllLocked(void);

/* Data passed to the balance thread — snapshot of values needed for the
 * HTTP call so the thread doesn't read ServerSim without the mutex. */
typedef struct {
    ServerSim *sim;
    uint8_t    totalPlayers;
    uint8_t    teamSize;
    uint8_t    botSlots[MAX_TANKS];   /* bot slots to include; empty if !includeBots */
    uint8_t    numBotSlots;
    bool       includeBots;
} BalanceThreadData;

/* Background thread: calls WBN balance API (blocks on HTTP) then writes
 * results back under the game mutex so the timer can broadcast them. */
static int balanceThreadFunc(void *data) {
    BalanceThreadData *btd = (BalanceThreadData *)data;
    ServerSim *sim = btd->sim;
    bool includeBots = btd->includeBots;  /* captured before free(btd) below */

    /* This blocks on HTTP — runs outside the game mutex */
    serverSimRequestBalanceProposal(sim, btd->totalPlayers, btd->teamSize,
                                     btd->numBotSlots > 0 ? btd->botSlots : NULL,
                                     btd->numBotSlots);

    free(btd);

    /* If the server is shutting down, signal completion and exit without
     * acquiring the mutex (the main thread may have already torn it down). */
    if (serverSimBalanceShutdownRequested(sim)) {
        serverSimSetBalanceRequestInFlight(sim, false);
        return 0;
    }

    /* Write results back under the game mutex. No approval step — the
     * host already committed to the rebalance by confirming the popup,
     * so apply WBN's assignments directly and broadcast the new slots. */
    threadsWaitForMutex();
    serverSimSetBalanceRequestInFlight(sim, false);
    if (serverSimGetBalanceProposal(sim)->pending) {
        serverSimSetBalanceIncludeBots(sim, includeBots);
        int i;
        /* Publish the proposal data first so every client's dispatcher
         * latches lastBalanceProposalArrivedMs — gives the host's
         * "Teams balanced" status label a chance to fire even though
         * we'll clear right after applying. */
        {
            ControlEvent propEvt;
            memset(&propEvt, 0, sizeof(propEvt));
            propEvt.type = CTRL_BALANCE_PROPOSAL;
            memcpy(propEvt.u.balanceProposal.teamForSlot,
                   serverSimGetBalanceProposal(sim)->teamForSlot,
                   MAX_TANKS);
            serverSimPublishControl(sim, &propEvt);
        }
        /* "Humans only" kicks every bot before applying the human-only
         * team assignments. */
        if (!includeBots) {
            for (i = 0; i < MAX_TANKS; i++) {
                if (serverSimIsBot(sim, (BYTE)i)) {
                    serverSimRemoveBot(sim, (BYTE)i);
                }
            }
        }
        for (i = 0; i < MAX_TANKS; i++) {
            if (serverSimGetBalanceProposal(sim)->teamForSlot[i] != 0) {
                serverSimSetTeamBatch(sim, (BYTE)i,
                                      serverSimGetBalanceProposal(sim)->teamForSlot[i]);
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
        }
        serverSimReapplyTeamAlliances(sim);
        serverSimClearBalanceProposal(sim);
        /* Publish the cleared proposal so balanceProposalActive flips
         * back to false on every client — keeps canBalance gating
         * from staying disabled on the Balance-from-WBN button. */
        {
            ControlEvent clrEvt;
            memset(&clrEvt, 0, sizeof(clrEvt));
            clrEvt.type = CTRL_BALANCE_PROPOSAL;
            serverSimPublishControl(sim, &clrEvt);
        }
        logAddEvent(log_BalanceApplied, 0, 0, 0, 0, 0, NULL);
        serverSimConsoleMessage("Team balance applied (WBN)");
    } else {
        /* WBN call returned without a usable proposal (non-200, null
         * body, or an "error" field — see winbolonet_server.c). Tell
         * the host so its "Asking WBN…" pill can flip immediately. */
        ControlEvent failEvt;
        memset(&failEvt, 0, sizeof(failEvt));
        failEvt.type = CTRL_BALANCE_FAILED;
        failEvt.u.balanceFailed.reasonCode = 1; /* http/transport */
        serverSimPublishControl(sim, &failEvt);
    }
    threadsReleaseMutex();
    return 0;
}

/* Short name for a ControlEventType — diagnostic logging only. */
static const char *mpDiagCtrlName(int type) {
    switch (type) {
    case CTRL_ALLIANCE_REQUEST: return "ALLIANCE_REQUEST";
    case CTRL_ALLIANCE_ACCEPT:  return "ALLIANCE_ACCEPT";
    case CTRL_ALLIANCE_LEAVE:   return "ALLIANCE_LEAVE";
    case CTRL_ALLIANCE_RESET:   return "ALLIANCE_RESET";
    case CTRL_PLAYER_JOIN:      return "PLAYER_JOIN";
    case CTRL_PLAYER_NAME:      return "PLAYER_NAME";
    case CTRL_LOBBY_SLOT:       return "LOBBY_SLOT";
    case CTRL_LOBBY_SETTINGS:   return "LOBBY_SETTINGS";
    case CTRL_LOBBY_MAP_CHANGE: return "LOBBY_MAP_CHANGE";
    case CTRL_MAP_DOWNLOAD_COMPLETE: return "MAP_DOWNLOAD_COMPLETE";
    case CTRL_BALANCE_PROPOSAL: return "BALANCE_PROPOSAL";
    case CTRL_MAP_SKIP_STATE:   return "MAP_SKIP_STATE";
    case CTRL_GAME_PHASE_LOBBY: return "GAME_PHASE_LOBBY";
    case CTRL_GAME_PHASE_COUNTDOWN: return "GAME_PHASE_COUNTDOWN";
    case CTRL_GAME_PHASE_RUNNING:   return "GAME_PHASE_RUNNING";
    case CTRL_GAME_PHASE_GAME_OVER: return "GAME_PHASE_GAME_OVER";
    case CTRL_GAME_OVER:        return "GAME_OVER";
    case CTRL_SERVER_SHUTDOWN:  return "SERVER_SHUTDOWN";
    case CTRL_CHAT:             return "CHAT";
    case CTRL_PLAYER_LEAVE:     return "PLAYER_LEAVE";
    case CTRL_LOBBY_TEAM_META:  return "LOBBY_TEAM_META";
    case CTRL_LOBBY_BOT_CONFIG: return "LOBBY_BOT_CONFIG";
    case CTRL_LOBBY_BOT_BRAIN:  return "LOBBY_BOT_BRAIN";
    case CTRL_LOBBY_BRAIN_LIST: return "LOBBY_BRAIN_LIST";
    case CTRL_GAME_VOTE_STATE:  return "GAME_VOTE_STATE";
    case CTRL_SERVER_TEXT:      return "SERVER_TEXT";
    case CTRL_SHELL_DEATH:      return "SHELL_DEATH";
    case CTRL_CHANNEL_RESET:    return "CHANNEL_RESET";
    case CTRL_VIEW_TARGET:      return "VIEW_TARGET";
    case CTRL_VOICE_TALKING:    return "VOICE_TALKING";
    default:                    return "<unknown>";
    }
}

/* Flush one client's channel onto the wire immediately as a standalone
 * PACKET_CHANNEL.  Used to carry a just-published control event when no later
 * carrier tick is guaranteed to follow — the server teardown publishes
 * CTRL_SERVER_SHUTDOWN and then tears the slot down in the same call, with no
 * snapshot or check-timeouts pass after it.  During running the snapshot
 * trailer is the carrier, so callers skip this path there. */
void transportUdpServerFlushChannel(int clientIdx) {
    UdpServerClient *client;
    uint8_t cbuf[UDP_MAX_PAYLOAD];
    int frameLen;
    if (clientIdx < 0 || clientIdx >= MAX_TANKS) return;
    client = &udpServer.clients[clientIdx];
    if (!client->connected) return;
    channelTick(&udpServer.channelMux[clientIdx], udpServer.tickCount,
                client->pingMs);
    frameLen = channelBuildFrame(&udpServer.channelMux[clientIdx],
                                 cbuf + PACKET_HEADER_SIZE,
                                 UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
    if (frameLen > 2) {
        packHeader(cbuf, PACKET_CHANNEL, client->outSequence++);
        srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen, &client->addr);
    }
}

/* Per-client subscriber deliver callback.  Filters single-recipient
 * variants, enqueues into this client's reliable control queue, and
 * sends the unacked tail immediately when outside running (the snapshot
 * tail handles running).  The controlSyncInProgress flag suppresses
 * the immediate-send during a serverSimRegisterSubscriber replay so
 * the burst lands in one carrier datagram rather than one per event. */
void udpClientDeliverControl(void *ctx, const ControlEvent *evt) {
    UdpServerClient *client = (UdpServerClient *)ctx;
    int idx;

    idx = (int)(client - udpServer.clients);
    if (!client->connected) {
        mpDiagLog("[srv] deliver SKIP slot=%d type=%s reason=not-connected",
                  idx, mpDiagCtrlName((int)evt->type));
        return;
    }

    /* Per-recipient filtering for single-target variants.  The codec
     * stays UdpServerClient-agnostic; the slot comparison lives here
     * where the recipient's player number is in scope. */
    if (evt->type == CTRL_ALLIANCE_REQUEST &&
        evt->u.allianceRequest.toPlayer != client->playerNum) {
        mpDiagLog("[srv] deliver FILTER slot=%d type=ALLIANCE_REQUEST toPlayer=%d clientPlayerNum=%d",
                  idx, (int)evt->u.allianceRequest.toPlayer, (int)client->playerNum);
        return;
    }
    if (evt->type == CTRL_CHAT) {
        BYTE from = evt->u.chat.fromPlayer;
        BYTE dest = evt->u.chat.destPlayer;
        if (dest == 0xFF) {
            /* Broadcast: skip the original sender if it's a real player. */
            if (from < MAX_TANKS && client->playerNum == from) {
                mpDiagLog("[srv] deliver FILTER slot=%d type=CHAT reason=sender-skip from=%d",
                          idx, (int)from);
                return;
            }
        } else if (CHAT_DEST_IS_TEAM(dest)) {
            /* Team-addressed: only members of the addressed team receive,
             * and the real-player sender is skipped (local echo covers it). */
            const LobbyPlayer *lp =
                serverSimGetLobbyPlayer(serverSimGetActive(), client->playerNum);
            if (!lp || lp->teamNumber != CHAT_DEST_TEAM_OF(dest)) {
                mpDiagLog("[srv] deliver FILTER slot=%d type=CHAT reason=not-on-team dest=%d clientPlayerNum=%d",
                          idx, (int)dest, (int)client->playerNum);
                return;
            }
            if (from < MAX_TANKS && client->playerNum == from) {
                mpDiagLog("[srv] deliver FILTER slot=%d type=CHAT reason=sender-skip from=%d",
                          idx, (int)from);
                return;
            }
        } else {
            /* Unicast: only the addressed slot receives. */
            if (client->playerNum != dest) {
                mpDiagLog("[srv] deliver FILTER slot=%d type=CHAT reason=not-addressed dest=%d clientPlayerNum=%d",
                          idx, (int)dest, (int)client->playerNum);
                return;
            }
        }
        /* Per-recipient mute, applied after the destination rules so it can
         * only ever remove a line this client would otherwise have seen.
         * Guarded on a real player slot: server-source messages arrive with
         * fromPlayer 0xFF (localized) or 0xFE (raw English) and must stay
         * unsilenceable — no player may suppress a server announcement. */
        if (from < MAX_TANKS &&
            (client->voiceMuteMask & ((PlayerBitMap)1u << from)) != 0) {
            mpDiagLog("[srv] deliver FILTER slot=%d type=CHAT reason=muted from=%d",
                      idx, (int)from);
            return;
        }
    }
    if (evt->type == CTRL_SERVER_TEXT && evt->u.serverText.destTeam != 0) {
        /* Team-scoped server text (e.g. a private surrender-vote notice):
         * only members of the addressed team receive it. */
        const LobbyPlayer *lp =
            serverSimGetLobbyPlayer(serverSimGetActive(), client->playerNum);
        if (!lp || lp->teamNumber != evt->u.serverText.destTeam) {
            mpDiagLog("[srv] deliver FILTER slot=%d type=SERVER_TEXT "
                      "reason=not-on-team destTeam=%d clientPlayerNum=%d",
                      idx, (int)evt->u.serverText.destTeam, (int)client->playerNum);
            return;
        }
    }
    if (evt->type == CTRL_GAME_VOTE_STATE &&
        evt->u.gameVoteState.kind == GAME_VOTE_KIND_SURRENDER &&
        evt->u.gameVoteState.teamId != 0) {
        /* A surrender vote belongs to the surrendering team: only its
         * members see that one is running, the live tally, and how it
         * ended. Withholding the event here (rather than hiding it in the
         * UI) keeps the tally off the wire entirely, so a modified client
         * can't watch the other side deliberate.
         *
         * This covers every emission path — vote start, the 1Hz heartbeat,
         * the conclusion, and the mid-game join replay — because they all
         * funnel through this deliver callback.
         *
         * The kind check matters: the base-monopoly auto-vote sets a
         * non-zero teamId on a BACK_TO_LOBBY vote, which is public and
         * must not be filtered. teamId==0 is the idle/never-run snapshot
         * emitted during sync replay; it carries no vote to hide. */
        const LobbyPlayer *lp =
            serverSimGetLobbyPlayer(serverSimGetActive(), client->playerNum);
        if (!lp || lp->teamNumber != evt->u.gameVoteState.teamId) {
            mpDiagLog("[srv] deliver FILTER slot=%d type=GAME_VOTE_STATE "
                      "reason=not-on-team teamId=%d clientPlayerNum=%d",
                      idx, (int)evt->u.gameVoteState.teamId,
                      (int)client->playerNum);
            return;
        }
    }
    if (evt->type == CTRL_COMMAND_REJECTED &&
        evt->u.commandRejected.origSlot != client->playerNum) {
        mpDiagLog("[srv] deliver FILTER slot=%d type=COMMAND_REJECTED "
                  "origSlot=%d clientPlayerNum=%d",
                  idx, (int)evt->u.commandRejected.origSlot,
                  (int)client->playerNum);
        return;
    }
    if (evt->type == CTRL_VIEW_TARGET &&
        evt->u.viewTarget.origSlot != client->playerNum) {
        /* The answer belongs to the slot that asked; drop it for everyone
         * else so no other client learns where that ally is. */
        mpDiagLog("[srv] deliver FILTER slot=%d type=VIEW_TARGET "
                  "origSlot=%d clientPlayerNum=%d",
                  idx, (int)evt->u.viewTarget.origSlot,
                  (int)client->playerNum);
        return;
    }
    if (evt->type == CTRL_BALANCE_FAILED && client->playerNum != 0) {
        /* The balance flow is host-driven; only slot 0 needs the
         * failure pill. Skip the fan-out for everyone else. */
        return;
    }
    if (evt->type == CTRL_SHELL_DEATH &&
        evt->u.shellDeath.owner != client->playerNum) {
        /* Owner-only: the firing player is the sole recipient (mirrors
         * CTRL_COMMAND_REJECTED). Drop for every other slot. */
        return;
    }

    /* Route onto the reliable control channel (CHANNEL_CONTROL).  Per-event
     * wire layout matches the body-only codec table: type(1) + bodyLen(2 BE)
     * + body(N); the receiver dispatches each event through
     * transportControlCodecBodyDecoder.  The channel carries and retransmits
     * the event in both phases — its frame rides the snapshot trailer during
     * running and a standalone PACKET_CHANNEL otherwise — so no phase-gated
     * immediate send is needed here.
     *
     * An event with no body encoder is dropped (it was never deliverable),
     * matching the former send-time `enc == NULL` skip.  A full window means
     * the client has stopped acking control: defer its disconnect off this
     * publish path (mirrors the game/map channel overflow at the snapshot
     * drain), flag-guarded so a re-hit on the still-connected slot can't spam
     * the log.  serverDisconnectClient / serverSimRemovePlayer cannot run from
     * inside this deliver callback without re-entering serverSimPublishControl
     * and tripping its reentrancy guard, so the teardown waits for
     * transportUdpServerDrainPendingRemovals at a safe point in the tick. */
    {
        ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(evt->type);
        uint8_t msg[CHANNEL_CONTROL_SEG];
        size_t bodyLen = 0;
        if (enc == NULL) {
            mpDiagLog("[srv] deliver SKIP slot=%d type=%s reason=no-encoder",
                      idx, mpDiagCtrlName((int)evt->type));
            return;
        }
        if (enc(evt, client, msg + 3, sizeof(msg) - 3, &bodyLen) != ENCODE_OK) {
            mpDiagLog("[srv] deliver SKIP slot=%d type=%s reason=encode",
                      idx, mpDiagCtrlName((int)evt->type));
            return;
        }
        msg[0] = (uint8_t)evt->type;
        packU16(msg + 1, (uint16_t)bodyLen);
        if (!channelSend(&udpServer.channelMux[idx], CHANNEL_CONTROL,
                         msg, (uint16_t)(3 + bodyLen))) {
            if (!udpServer.pendingSimRemove[idx]) {
                WB_LOG_ERROR(WB_LOG_CAT_NET,
                             "control channel overflow for slot %d, deferring disconnect",
                             idx);
                mpDiagLog("[srv] OVERFLOW slot=%d type=%s -> deferring disconnect",
                          idx, mpDiagCtrlName((int)evt->type));
                udpServer.pendingSimRemove[idx] = true;
            }
            return;
        }
        mpDiagLog("[srv] CTRL->ch2 slot=%d type=%s bodyLen=%u",
                  idx, mpDiagCtrlName((int)evt->type), (unsigned)bodyLen);
    }

    /* Carry it now when outside running: the per-tick standalone PACKET_CHANNEL
     * would otherwise pick it up, but a control event published with no later
     * tick (CTRL_SERVER_SHUTDOWN, emitted as the server tears the slot down)
     * must flush synchronously.  During running the snapshot trailer is the
     * carrier; during the join sync-replay the burst is coalesced into one
     * flush after registration. */
    if (!udpServer.controlSyncInProgress[idx] &&
        serverSimGetState(serverSimGetActive()) != serverStateRunning) {
        transportUdpServerFlushChannel(idx);
    }
}

bool lobbyAnyOtherUploadActive(const bool *active, int exceptIdx) {
    int i;
    if (active == NULL) return false;
    for (i = 0; i < MAX_TANKS; i++) {
        if (i == exceptIdx) continue;
        if (active[i]) return true;
    }
    return false;
}

/* Free a client's per-slot upload state. Called from
 * serverDisconnectClient so a client that drops mid-upload doesn't
 * leave clientUploadActive set, which would falsely flag the
 * upload slot as busy and block every subsequent uploader. */
static void udpServerClearClientUploadState(int idx) {
    if (idx < 0 || idx >= MAX_TANKS) return;
    udpServer.clientUploadActive[idx]   = false;
    udpServer.clientUploadTotal[idx]    = 0;
    udpServer.clientUploadName[idx][0]  = '\0';
    udpServer.clientReqCooldownTicks[idx] = 0;
    bulkReceiverInit(&udpServer.bulkRecvUp[idx]);
}

/* Authority check used by every lobby command handler.
 * Returns TRUE if the sender at clientIdx is allowed to issue the
 * command (host, OR open-host is on and they're an active player,
 * OR they're an admin). */
bool lobbyClientMayEdit(ServerSim *sim, int clientIdx) {
    if (clientIdx < 0 || clientIdx >= MAX_TANKS) return FALSE;
    if (clientIdx == serverSimGetHostSlot(sim)) return TRUE;  /* the host slot */
    if (serverSimIsPlayerConnected(sim, clientIdx) &&
        (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, (BYTE)clientIdx)
         & PLAYER_FLAG_ADMIN)) {
        return TRUE;
    }
    return serverSimGetOpenHost(sim) && serverSimIsPlayerConnected(sim, clientIdx);
}

/* Find a connected spectator by source address. Returns the spectators[]
 * index or -1. Mirrors serverFindClient over the parallel array. */
static int serverFindSpectator(const struct sockaddr_in *addr) {
    int i;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (udpServer.spectators[i].connected &&
            udpServer.spectators[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            udpServer.spectators[i].addr.sin_port == addr->sin_port) {
            return i;
        }
    }
    return -1;
}

/* Spectator-flavoured JOIN_ACCEPT: same PACKET_JOIN_ACCEPT layout as
 * serverSendJoinAccept, but reads spectators[s] and marks the connection a
 * viewer — slot byte = 0xFF (no tank slot). The map size is the real compressed
 * size in lobby/countdown (the live-lobby map streams for the preview) and 0
 * while running (the game-time map rides the ring seed instead). */
static void serverSendSpectatorAccept(int s, ServerSim *sim,
                                      const struct sockaddr_in *addr) {
    uint8_t acceptBuf[PACKET_HEADER_SIZE + 9 + 8 + 1];
    ServerState st;
    int pos;

    packHeader(acceptBuf, PACKET_JOIN_ACCEPT,
               udpServer.spectators[s].outSequence++);
    pos = PACKET_HEADER_SIZE;
    st = serverSimGetState(sim);
    acceptBuf[pos++] = 0xFF;                       /* viewer: no tank slot */
    packU32(acceptBuf + pos, serverSimGetTick(sim));
    pos += 4;
    /* In lobby/countdown the viewer watches the live lobby and needs the current
     * map for its preview, so carry the real size and stream it (mirrors a
     * player's join download, on the spectator's own CHANNEL_BULK). Running -> 0:
     * the delayed seed carries the game-time map instead. */
    packU32(acceptBuf + pos,
            (st == serverStateLobby || st == serverStateCountdown)
                ? udpServer.compressedMapSize : 0u);
    pos += 4;
    packConnId(acceptBuf + pos, udpServer.spectators[s].connId);
    pos += 8;
    /* Initial spectator mode byte (appended after the connId trailer so the
     * connId offset and its client-side length gate are unchanged): 1 when the
     * server is in lobby/countdown (the viewer watches the live lobby), 0 when a
     * game is running (delayed ring). Same predicate serverAcceptSpectator uses
     * to decide live bus registration. */
    acceptBuf[pos++] =
        (st == serverStateLobby || st == serverStateCountdown) ? 1 : 0;

    srvSendTo(acceptBuf, pos, addr);
}

/* Arm (or re-arm) the live-lobby map download for spectator s: copy the current
 * compressed map into the spectator's own buffer; serverServiceSpectators begins
 * the BULK_KIND_DOWNLOAD once its CHANNEL_BULK is idle, then frees the copy.
 * resetChannel drops any in-flight transfer first — bulkSenderReset + a
 * CHANNEL_BULK send-window re-base, carried to the viewer as a CTRL_CHANNEL_RESET
 * so it re-bases its receiver (mirrors the player map-change path) — needed on a
 * re-join or map change where an older transfer may be mid-flight; false for a
 * fresh connect whose channel is still empty. No-op when the server holds no map. */
static void serverArmSpectatorLobbyMap(int s, bool resetChannel) {
    SpectatorConn *sp = &udpServer.spectators[s];

    if (udpServer.compressedMapSize == 0) return;

    if (resetChannel) {
        uint32_t b3;
        ControlEvent resetEvt;
        ControlEncodeBodyFn enc;
        uint8_t msg[CHANNEL_CONTROL_SEG];
        size_t bodyLen = 0;

        bulkSenderReset(&sp->bulkSend);
        b3 = channelResetSend(&sp->channelMux, CHANNEL_BULK);
        memset(&resetEvt, 0, sizeof(resetEvt));
        resetEvt.type = CTRL_CHANNEL_RESET;
        resetEvt.u.channelReset.channelMask = (uint8_t)(1u << CHANNEL_BULK);
        resetEvt.u.channelReset.ch3Baseline = b3;
        enc = transportControlCodecBodyEncoder(CTRL_CHANNEL_RESET);
        if (enc != NULL &&
            enc(&resetEvt, NULL, msg + 3, sizeof(msg) - 3, &bodyLen) == ENCODE_OK) {
            msg[0] = (uint8_t)CTRL_CHANNEL_RESET;
            packU16(msg + 1, (uint16_t)bodyLen);
            channelSend(&sp->channelMux, CHANNEL_CONTROL, msg,
                        (uint16_t)(3 + bodyLen));
        }
    }

    if (sp->lobbyMap != NULL) free(sp->lobbyMap);
    sp->lobbyMap = (BYTE *)malloc(udpServer.compressedMapSize);
    if (sp->lobbyMap == NULL) {
        sp->lobbyMapSize = 0;
        return;
    }
    memcpy(sp->lobbyMap, udpServer.compressedMap, udpServer.compressedMapSize);
    sp->lobbyMapSize = udpServer.compressedMapSize;
}

/* Per-spectator subscriber deliver callback — the spectator peer of
 * udpClientDeliverControl. ctx is the SpectatorConn. A spectator has no
 * playerNum/team, so this is an explicit drop-by-default ALLOWLIST: only
 * lobby-visible, non-player-targeted control reaches a viewer, so no future
 * CTRL_* can leak a player-private event onto a spectator. Passed events are
 * body-encoded onto the spectator's own CHANNEL_CONTROL; the carrier at the
 * bottom of serverServiceSpectators flushes it (no explicit flush here). */
static void serverSpectatorDeliverControl(void *ctx, const ControlEvent *evt) {
    SpectatorConn *sp = (SpectatorConn *)ctx;
    int idx;
    bool allow;

    if (!sp->connected) {
        return;
    }
    idx = (int)(sp - udpServer.spectators);

    switch (evt->type) {
    case CTRL_GAME_PHASE_LOBBY:
    case CTRL_GAME_PHASE_COUNTDOWN:
    case CTRL_GAME_PHASE_RUNNING:
    case CTRL_GAME_PHASE_GAME_OVER:
    case CTRL_LOBBY_SETTINGS:
    case CTRL_LOBBY_MAP_CHANGE:
    case CTRL_LOBBY_SLOT:
    case CTRL_LOBBY_TEAM_META:
    case CTRL_LOBBY_BOT_CONFIG:
    case CTRL_LOBBY_BOT_BRAIN:
    case CTRL_LOBBY_BRAIN_LIST:
    case CTRL_PLAYER_JOIN:
    case CTRL_BALANCE_PROPOSAL:
    case CTRL_MAP_SKIP_STATE:
    case CTRL_SPECTATOR_SLOT:
    case CTRL_SPECTATOR_CHAT:
    case CTRL_LOBBY_SYNC_COMPLETE:
    /* The live scoreboard is public — a viewer can already read captures and
     * kills off the map, and a spectator whose board never leaves zero looks
     * broken. */
    case CTRL_STATS_SEED:
        allow = true;
        break;
    case CTRL_GAME_VOTE_STATE:
        /* Back-to-lobby votes are public. Surrender votes are private to
         * the surrendering team, and a spectator belongs to no team, so
         * it never qualifies (mirrors the per-client filter in
         * udpClientDeliverControl). teamId==0 is the idle snapshot. */
        allow = (evt->u.gameVoteState.kind != GAME_VOTE_KIND_SURRENDER ||
                 evt->u.gameVoteState.teamId == 0);
        break;
    case CTRL_CHAT:
        /* Broadcast chat only; team (0x81..0x90) and unicast are player-private.
         * No sender-skip needed — the sender is a player, the spectator has no
         * playerNum. */
        allow = (evt->u.chat.destPlayer == 0xFF);
        break;
    default:
        allow = false;
        break;
    }

    if (!allow) {
        return;
    }

    /* Body-encode and queue on CHANNEL_CONTROL, mirroring udpClientDeliverControl:
     * [type u8][bodyLen u16 BE][body]. The codec is recipient-agnostic, so the
     * recipient arg is NULL (matches serverSimControlSnapshotDeliver). */
    {
        ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(evt->type);
        uint8_t msg[CHANNEL_CONTROL_SEG];
        size_t bodyLen = 0;
        if (enc == NULL) {
            mpDiagLog("[srv] spec deliver SKIP idx=%d type=%s reason=no-encoder",
                      idx, mpDiagCtrlName((int)evt->type));
            return;
        }
        if (enc(evt, NULL, msg + 3, sizeof(msg) - 3, &bodyLen) != ENCODE_OK) {
            mpDiagLog("[srv] spec deliver SKIP idx=%d type=%s reason=encode",
                      idx, mpDiagCtrlName((int)evt->type));
            return;
        }
        msg[0] = (uint8_t)evt->type;
        packU16(msg + 1, (uint16_t)bodyLen);
        if (!channelSend(&sp->channelMux, CHANNEL_CONTROL,
                         msg, (uint16_t)(3 + bodyLen))) {
            /* Full window: drop and warn. Do NOT disconnect from inside the
             * deliver callback — there is no deferred-removal path for
             * spectators, and tearing the slot down here risks reentrancy. */
            WB_LOG_WARN(WB_LOG_CAT_NET,
                        "spectator control channel overflow for slot %d, dropping event",
                        idx);
            mpDiagLog("[srv] spec OVERFLOW idx=%d type=%s -> drop",
                      idx, mpDiagCtrlName((int)evt->type));
            return;
        }
        mpDiagLog("[srv] spec CTRL->ch2 idx=%d type=%s bodyLen=%u",
                  idx, mpDiagCtrlName((int)evt->type), (unsigned)bodyLen);
    }
}

/* Worst case for the lobby-chat backlog blob. The buffer holds the two event
 * types serverSimPublishControl captures (CTRL_CHAT broadcasts and
 * CTRL_SPECTATOR_CHAT). The bigger encoded body is CTRL_CHAT: a localized
 * server message fills body[] to CHAT_BODY_MAX (272), and the codec frames it
 * as [fromPlayer 1][destPlayer 1][body] -> 2 + CHAT_BODY_MAX. (CTRL_SPECTATOR_CHAT
 * is only 2 + PACKET_MAX_CHAT_MESSAGE = 130, so it never dominates.) Each blob
 * record then adds [type 1][bodyLen 2 BE], and up to LOBBY_CHAT_BUFFER_MAX
 * records pack back to back. Sizing off the constants keeps this from silently
 * undersizing if a chat limit moves; a typed-line assumption (128) undersized
 * it and dropped the whole catch-up on a busy lobby. */
#define LOBBY_CHAT_BACKLOG_BODY_MAX  (2 + CHAT_BODY_MAX)
#define LOBBY_CHAT_BACKLOG_REC_MAX   (3 + LOBBY_CHAT_BACKLOG_BODY_MAX)
#define LOBBY_CHAT_BACKLOG_WIRE_MAX  (LOBBY_CHAT_BUFFER_MAX * LOBBY_CHAT_BACKLOG_REC_MAX)

/* The client reassembles this blob into a buffer bounded by MAP_DOWNLOAD_MAX_SIZE
 * (transport_udp_client.c). If the worst case ever outgrew that bound the server
 * could stage a blob the client would refuse, so pin the relationship here. */
BOLO_STATIC_ASSERT(LOBBY_CHAT_BACKLOG_WIRE_MAX <= MAP_DOWNLOAD_MAX_SIZE,
                   lobby_chat_backlog_fits_client_reassembly_bound);

/* Sink that serializes the sim's replayed lobby-chat events into one blob, in
 * the same [type][bodyLen BE][body] framing serverSpectatorDeliverControl puts
 * on CHANNEL_CONTROL — so the client decodes each record with the existing
 * control-codec path. On any encode error or capacity overrun it latches
 * `overflow` and the caller declines to send (the catch-up is best-effort). */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    bool     overflow;
} LobbyChatBlobSink;

static void serverLobbyChatBlobDeliver(void *ctx, const struct ControlEvent *evt) {
    LobbyChatBlobSink *s = (LobbyChatBlobSink *)ctx;
    ControlEncodeBodyFn enc;
    uint8_t body[MAX_CONTROL_PACKET];
    size_t bodyLen = 0;

    if (s->overflow) return;
    enc = transportControlCodecBodyEncoder(evt->type);
    if (enc == NULL ||
        enc(evt, NULL, body, sizeof(body), &bodyLen) != ENCODE_OK) {
        s->overflow = true;
        return;
    }
    if ((uint32_t)(s->len + 3 + bodyLen) > s->cap) {
        s->overflow = true;
        return;
    }
    s->buf[s->len++] = (uint8_t)evt->type;
    packU16(s->buf + s->len, (uint16_t)bodyLen);
    s->len += 2;
    memcpy(s->buf + s->len, body, bodyLen);
    s->len += (uint32_t)bodyLen;
}

/* Drain the sim's current-session lobby-chat backlog to spectator s as a single
 * BULK_KIND_LOBBY_CHAT_BACKLOG blob on CHANNEL_BULK. Called ONLY at the
 * delayed->live drain-flip (after the sync re-register sets the lobby phase, and
 * after serverArmSpectatorLobbyMap has reset/idled the bulk sender) — never on a
 * fresh accept or player join, so fresh joiners get no backlog. The armed lobby
 * map streams next, once this blob drains (single-blob-in-flight guard). */
static void serverSendSpectatorBacklog(int s, ServerSim *sim) {
    SpectatorConn *sp = &udpServer.spectators[s];
    LobbyChatBlobSink sink;
    uint8_t *blob = (uint8_t *)malloc(LOBBY_CHAT_BACKLOG_WIRE_MAX);

    if (blob == NULL) return;
    sink.buf = blob;
    sink.cap = LOBBY_CHAT_BACKLOG_WIRE_MAX;
    sink.len = 0;
    sink.overflow = false;
    serverSimReplayLobbyChat(sim, serverLobbyChatBlobDeliver, &sink);

    if (!sink.overflow && sink.len > 0 && !bulkSenderBusy(&sp->bulkSend)) {
        BulkStreamHeader sh;
        memset(&sh, 0, sizeof(sh));
        sh.kind = BULK_KIND_LOBBY_CHAT_BACKLOG;
        sh.gen = 0;
        sh.totalSize = sink.len;
        sh.pathLen = 0;
        sh.path[0] = '\0';
        bulkSenderBegin(&sp->bulkSend, &sh, blob, sink.len);
        mpDiagLog("[srv] spec idx=%d lobby-chat backlog -> bulk (%u bytes)",
                  s, (unsigned)sink.len);
    }
    free(blob);   /* bulkSenderBegin copied it into its own buffer */
}

/* Fill a CTRL_SPECTATOR_SLOT event from spectator slot s. A disconnected slot
 * emits a connected==false roster row carrying only specIdx; the decoder leaves
 * the rest zeroed. clientFlags merges the client-supplied hint bits with the
 * server-determined WBN trust bit (mirrors the player badge derivation). */
static void serverFillSpectatorSlotEvent(int s, ControlEvent *evt) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_SPECTATOR_SLOT;
    evt->u.spectatorSlot.specIdx = (uint8_t)s;
    ClientSpectatorSlot *slot = &evt->u.spectatorSlot.slot;
    slot->connected = udpServer.spectators[s].connected;
    if (slot->connected) {
        snprintf(slot->playerName, sizeof(slot->playerName), "%s",
                 udpServer.spectators[s].playerName);
        slot->countryCode[0] = udpServer.spectators[s].countryCode[0];
        slot->countryCode[1] = udpServer.spectators[s].countryCode[1];
        slot->countryCode[2] = '\0';
        slot->clientType  = udpServer.spectators[s].clientType;
        slot->clientFlags = (uint8_t)((udpServer.spectators[s].clientHints
                                       & PLAYER_CLIENT_HINT_MASK)
                                      | udpServer.spectators[s].wbnFlags);
    }
}

/* Spectator roster enumerator (registered on the sim via
 * serverSimSetSpectatorRosterEnumerator). The sim invokes this during
 * sync-replay to emit one CTRL_SPECTATOR_SLOT per connected spectator through
 * the subscriber's own deliver path, so every new subscriber — players and
 * spectators alike — is seeded the spectator roster, and the delayed ring
 * keyframe carries it for free. */
static void serverEnumSpectatorRoster(
    void *enumCtx,
    void (*deliver)(void *, const ControlEvent *),
    void *deliverCtx) {
    int sp;
    (void)enumCtx;
    for (sp = 0; sp < MAX_SPECTATORS; sp++) {
        ControlEvent evt;
        if (!udpServer.spectators[sp].connected) continue;
        serverFillSpectatorSlotEvent(sp, &evt);
        deliver(deliverCtx, &evt);
    }
}

/* Broadcast spectator slot s's current roster row to every connected player
 * client and every live spectator. Safe to call from the JOIN/disconnect paths
 * (not the subscriber bus); each deliver callback skips disconnected slots on
 * its own. The subject slot itself is included in the spectator fan, so a
 * spectator sees both itself and the others on its roster. */
static void serverBroadcastSpectatorSlot(int s) {
    ControlEvent evt;
    int idx;
    serverFillSpectatorSlotEvent(s, &evt);
    for (idx = 0; idx < MAX_TANKS; idx++) {
        if (udpServer.clients[idx].connected) {
            udpClientDeliverControl(&udpServer.clients[idx], &evt);
        }
    }
    for (idx = 0; idx < MAX_SPECTATORS; idx++) {
        if (udpServer.spectators[idx].live) {
            serverSpectatorDeliverControl(&udpServer.spectators[idx], &evt);
        }
    }
}

/* Store a resolved 2-char ISO country into spectator slot s, empty-safe:
 * a NULL or empty country stores "". */
static void serverSetSpectatorCountry(int s, const char *country) {
    if (country != NULL && country[0] != '\0') {
        udpServer.spectators[s].countryCode[0] = country[0];
        udpServer.spectators[s].countryCode[1] = country[1];
    } else {
        udpServer.spectators[s].countryCode[0] = '\0';
        udpServer.spectators[s].countryCode[1] = '\0';
    }
    udpServer.spectators[s].countryCode[2] = '\0';
}

/* Accept a "join as viewer" connection: register it in spectators[] with no
 * tank slot, no sim player, and no control-bus subscription, then send the
 * spectator accept. Caller has already passed every shared JOIN pre-check
 * (cookie, version, name, password). country is the resolved GeoIP-or-fallback
 * ISO code stored for the roster broadcast ("" when unknown). */
void serverAcceptSpectator(ServerSim *sim,
                           const struct sockaddr_in *fromAddr,
                           const char *name,
                           uint8_t clientType, uint8_t clientHints,
                           const char *spectatorKey, uint8_t wbnFlags,
                           const char *country) {
    int effectiveCap;
    int s;

    /* Ensure the compressed map exists for the live-lobby map download. Player
     * joins populate it (after serverSimAddPlayer), but a spectator-only lobby
     * may have had no player join yet, leaving it empty — compress the current
     * map now so the accept carries a real size and the download can stream.
     * Guarded so a re-JOIN doesn't recompress; map changes refresh it separately. */
    if (udpServer.compressedMapSize == 0 &&
        (serverSimGetState(sim) == serverStateLobby ||
         serverSimGetState(sim) == serverStateCountdown)) {
        int mapLen = serverSimGetCompressedMap(sim, udpServer.compressedMap,
                                               (int)sizeof(udpServer.compressedMap));
        if (mapLen > 0) {
            udpServer.compressedMapSize = (uint32_t)mapLen;
        }
    }

    /* Re-JOIN from a known spectator address: resend the accept, no new slot.
     * Refresh the stored country and re-broadcast the roster row so a player
     * client that joined after this spectator (or missed the first broadcast)
     * still learns it — harmless when nothing changed. */
    s = serverFindSpectator(fromAddr);
    if (s >= 0) {
        serverSetSpectatorCountry(s, country);
        serverSendSpectatorAccept(s, sim, fromAddr);
        /* The re-accept makes a live viewer re-allocate its map buffer, so
         * re-arm the lobby-map download (with a channel reset, since an earlier
         * transfer may be in flight) or it would wait for bytes that never come. */
        if (udpServer.spectators[s].live) {
            serverArmSpectatorLobbyMap(s, /*resetChannel=*/true);
        }
        serverBroadcastSpectatorSlot(s);
        return;
    }

    /* Cap: operator setting clamped to the array size. 0 disables spectating. */
    effectiveCap = (int)serverSimGetMaxSpectators(sim);
    if (effectiveCap > MAX_SPECTATORS) effectiveCap = MAX_SPECTATORS;
    if (effectiveCap <= 0) {
        serverSendJoinReject(fromAddr, STR_REJECT_SERVER_FULL, 0, NULL);
        return;
    }

    /* First free slot within the effective cap. */
    s = -1;
    {
        int i;
        for (i = 0; i < effectiveCap; i++) {
            if (!udpServer.spectators[i].connected) {
                s = i;
                break;
            }
        }
    }
    if (s < 0) {
        serverSendJoinReject(fromAddr, STR_REJECT_SERVER_FULL, 0, NULL);
        return;
    }

    udpServer.spectators[s].connected        = true;
    udpServer.spectators[s].addr             = *fromAddr;
    udpServer.spectators[s].connId           = serverNextConnId();
    snprintf(udpServer.spectators[s].playerName,
             PACKET_MAX_PLAYER_NAME, "%s", name);
    udpServer.spectators[s].lastReceivedTick = udpServer.tickCount;
    udpServer.spectators[s].outSequence      = 1;
    udpServer.spectators[s].inboundCmdSeq     = 0;
    udpServer.spectators[s].pingMs           = 0;
    udpServer.spectators[s].clientType       = clientType;
    udpServer.spectators[s].clientHints      = clientHints;
    serverSetSpectatorCountry(s, country);
    if (spectatorKey != NULL) {
        strncpy(udpServer.spectators[s].spectatorKey, spectatorKey,
                WINBOLONET_KEY_LEN - 1);
        udpServer.spectators[s].spectatorKey[WINBOLONET_KEY_LEN - 1] = '\0';
    } else {
        udpServer.spectators[s].spectatorKey[0] = '\0';
    }
    udpServer.spectators[s].wbnFlags         = wbnFlags;
    channelMuxInit(&udpServer.spectators[s].channelMux);
    bulkSenderInit(&udpServer.spectators[s].bulkSend);
    udpServer.spectators[s].seedBlob     = NULL;
    udpServer.spectators[s].seedLen      = 0;
    udpServer.spectators[s].seedGen      = 0;
    udpServer.spectators[s].seedBegun    = false;
    udpServer.spectators[s].seedComplete = false;
    udpServer.spectators[s].xferStartSeq = 0;
    udpServer.spectators[s].xferEndSeq   = 0;
    udpServer.spectators[s].seedSeq        = 0;
    udpServer.spectators[s].lastEmittedSeq = 0;
    udpServer.spectators[s].inCountdown        = false;
    udpServer.spectators[s].countdownRemaining = 0;
    udpServer.spectators[s].countdownSentTick  = 0;
    udpServer.spectators[s].live               = false;
    udpServer.spectators[s].controlSub         = SUBSCRIBER_HANDLE_INVALID;
    udpServer.spectators[s].lobbyMap           = NULL;
    udpServer.spectators[s].lobbyMapSize       = 0;

    serverSendSpectatorAccept(s, sim, fromAddr);
    /* Tell every connected player client a new viewer is on the roster. */
    serverBroadcastSpectatorSlot(s);

    /* Live-lobby spectating: in lobby/countdown the viewer is a real-time
     * control-bus subscriber, so it tracks roster/ready/chat live. Registration
     * fires the sync replay through serverSpectatorDeliverControl (allowlist-
     * filtered) onto this spectator's CHANNEL_CONTROL; the serverServiceSpectators
     * carrier flushes it, so no explicit flush is needed. In-game viewers stay on
     * the delayed ring (live stays false). */
    {
        ServerState st = serverSimGetState(sim);
        if (st == serverStateLobby || st == serverStateCountdown) {
            SubscriberHandle sub =
                serverSimRegisterSubscriber(sim, serverSpectatorDeliverControl,
                                            &udpServer.spectators[s]);
            if (sub == SUBSCRIBER_HANDLE_INVALID) {
                /* Subscriber budget full (unreachable within the 49-slot budget
                 * today): stay not-live with no bus handle rather than a live
                 * state backed by a dead subscription that delivers nothing. */
                WB_LOG_WARN(WB_LOG_CAT_NET,
                            "spectator %d: control-bus subscriber budget full; "
                            "not entering live-lobby", s);
            } else {
                udpServer.spectators[s].live       = true;
                udpServer.spectators[s].controlSub = sub;
            }
            /* Fresh connect: the channel is empty, so arm the lobby-map download
             * without a reset. serverServiceSpectators streams it once idle. */
            serverArmSpectatorLobbyMap(s, /*resetChannel=*/false);
        }
    }

    /* Record the viewer's arrival in the .wbv replay log (and any spectator
     * ring tap). logAddEvent self-gates: a no-op unless a log is recording. */
    {
        char pstr[256];
        int nameLen = (int)strlen(udpServer.spectators[s].playerName);
        if (nameLen > 255) nameLen = 255;
        pstr[0] = (char)nameLen;
        memcpy(pstr + 1, udpServer.spectators[s].playerName, (size_t)nameLen);
        logAddEvent(log_SpectatorJoined, (BYTE)s,
                    (BYTE)udpServer.spectators[s].countryCode[0],
                    (BYTE)udpServer.spectators[s].countryCode[1],
                    udpServer.spectators[s].wbnFlags, 0, pstr);
    }
}

/* Release a spectator slot. A spectator holds no tank, no sim player, and no
 * control-bus subscription, so teardown is just freeing the slot and resetting
 * its in-place channel/bulk state — none of serverDisconnectClient's
 * WBN/chat/subscriber/serverSimRemovePlayer work applies. graceful=false is a
 * timeout; graceful=true is reserved for the explicit leave path (2c) and does
 * the same teardown for now. */
static void serverDisconnectSpectator(ServerSim *sim, int s, bool graceful) {
    if (s < 0 || s >= MAX_SPECTATORS || !udpServer.spectators[s].connected) {
        return;
    }
    mpDiagLog("[srv] SPECTATOR DISCONNECT idx=%d graceful=%d", s, (int)graceful);
    /* Record the viewer's departure in the .wbv replay log (and any spectator
     * ring tap) while the slot's name is still valid — teardown below clears
     * it. logAddEvent self-gates: a no-op unless a log is recording. */
    {
        char pstr[256];
        int nameLen = (int)strlen(udpServer.spectators[s].playerName);
        if (nameLen > 255) nameLen = 255;
        pstr[0] = (char)nameLen;
        memcpy(pstr + 1, udpServer.spectators[s].playerName, (size_t)nameLen);
        logAddEvent(log_SpectatorLeft, (BYTE)s, 0, 0, 0, 0, pstr);
    }
    /* Release the WBN spectator session if this viewer was verified. Empty key
     * (anonymous / non-WBN) makes this a no-op. */
    if (udpServer.spectators[s].spectatorKey[0] != '\0') {
        winboloNetSpectatorLeaveGame(udpServer.spectators[s].spectatorKey);
        udpServer.spectators[s].spectatorKey[0] = '\0';
    }
    udpServer.spectators[s].wbnFlags = 0;
    /* Drop the live control-bus subscription (lobby/countdown viewers only). */
    if (udpServer.spectators[s].controlSub != SUBSCRIBER_HANDLE_INVALID) {
        serverSimUnregisterSubscriber(sim, udpServer.spectators[s].controlSub);
        udpServer.spectators[s].controlSub = SUBSCRIBER_HANDLE_INVALID;
    }
    udpServer.spectators[s].live = false;
    udpServer.spectators[s].connected = false;
    channelMuxInit(&udpServer.spectators[s].channelMux);   /* reset in place */
    bulkSenderInit(&udpServer.spectators[s].bulkSend);
    if (udpServer.spectators[s].seedBlob != NULL) {
        free(udpServer.spectators[s].seedBlob);
        udpServer.spectators[s].seedBlob = NULL;
    }
    if (udpServer.spectators[s].lobbyMap != NULL) {
        free(udpServer.spectators[s].lobbyMap);
        udpServer.spectators[s].lobbyMap = NULL;
    }
    udpServer.spectators[s].lobbyMapSize = 0;
    udpServer.spectators[s].seedLen = 0;
    udpServer.spectators[s].seedGen = 0;
    udpServer.spectators[s].seedBegun = false;
    udpServer.spectators[s].seedComplete = false;
    udpServer.spectators[s].xferStartSeq = 0;
    udpServer.spectators[s].xferEndSeq = 0;
    udpServer.spectators[s].seedSeq = 0;
    udpServer.spectators[s].lastEmittedSeq = 0;
    udpServer.spectators[s].inCountdown = false;
    udpServer.spectators[s].countdownRemaining = 0;
    udpServer.spectators[s].countdownSentTick = 0;
    udpServer.spectators[s].playerName[0] = '\0';
    udpServer.spectators[s].outSequence = 0;
    udpServer.spectators[s].inboundCmdSeq = 0;
    /* The slot is now cleared (connected == false), so the fill helper emits a
     * disconnect roster row. Tell every connected player client the viewer is
     * gone. */
    serverBroadcastSpectatorSlot(s);
}

/* Send PACKET_WBN_REKEY to a single connected client carrying the current
 * server_key.  Called right after JOIN_ACCEPT so the joiner learns the
 * WBN session key without a credential ever riding the JOIN wire field,
 * and from the broadcast wrapper after each return-to-lobby rotation.
 * Silently no-ops when WBN isn't running or the server has no session
 * key yet, so non-WBN servers (and the JOIN_ACCEPT path on them) pay
 * nothing. */
void transportUdpServerSendWbnRekey(UdpServerClient *c) {
    uint8_t buf[PACKET_HEADER_SIZE + WBN_JOIN_KEY_WIRE_LEN];
    char serverKey[WINBOLONET_KEY_LEN];

    if (!winbolonetIsRunning()) return;

    winboloNetGetServerKey(serverKey);
    if (serverKey[0] == '\0') return;

    packHeader(buf, PACKET_WBN_REKEY, c->outSequence++);
    wbnKeyEncode(buf + PACKET_HEADER_SIZE, serverKey);

    /* wire-only: per-client capability refresh (no in-process audience) */
    srvSendTo(buf, sizeof(buf), &c->addr);
}

/* Defined below near the reauth handler; used here for the web lobby-return
 * path that re-stamps identity instead of sending a REKEY. */
static void udpServerApplyWebIdentity(ServerSim *sim, BYTE slot);

/* Broadcast PACKET_WBN_REKEY to every connected client that was
 * WBN-verified last round, after the server rotates its server_key
 * (post-returnToLobby).  Each client mints a fresh player_key against
 * the new key and re-auths, re-registering for the new session.
 *
 * The gate is the durable per-connection wbnWasVerified bit, NOT the
 * sim-side PLAYER_FLAG_WBN_VERIFIED nor the per-slot WBN key.  The key is
 * out: winbolonetEndSession just wiped every key, so a key-based gate
 * (winboloNetIsPlayerParticipant) would match nobody.  The flag is out
 * too: serverSimReturnToLobby clears PLAYER_FLAG_WBN_VERIFIED on every
 * slot earlier in this same tick (it means "verified for the current
 * session", and the session was just torn down), so a flag-based gate
 * would likewise match nobody and silently strand every player un-keyed
 * for the new round.  wbnWasVerified lives in the transport client struct,
 * untouched by the sim reset, so it survives as the cross-round signal.
 * Re-arm the deferred-join state for each rekeyed slot so the incoming
 * reauth fires a fresh keyed PLAYER_JOIN for the new game (or the grace
 * sweep an anonymous one if the reauth never lands). */
void transportUdpServerBroadcastWbnRekey(ServerSim *sim) {
    int i;
    if (!winbolonetIsRunning()) return;
    for (i = 0; i < MAX_TANKS; i++) {
        bool wasVerified = udpServer.clients[i].wbnWasVerified;
        if (!wbnRekeyTargetSelected(udpServer.clients[i].connected, wasVerified))
            continue;
        if (udpServer.clients[i].clientType == CLIENT_TYPE_WEB) {
            /* A web slot can't mint a fresh player_key and won't re-present its
             * single-use join_code, so its identity is re-stamped directly from
             * the cached join-code result (it survives the sim reset, like
             * wbnWasVerified) rather than via a reauth round-trip. */
            if (udpServer.clients[i].wbnWebIdentityCached &&
                udpServer.clients[i].wbnWebIsLoggedIn) {
                udpServerApplyWebIdentity(sim, (BYTE)i);
                if (serverSimGetState(sim) == serverStateLobby ||
                    serverSimGetState(sim) == serverStateCountdown) {
                    serverSimPublishLobbySlot(sim, (BYTE)i);
                }
                winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                   (BYTE)i, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
            }
            /* Still send the REKEY so the web client learns the rotated
             * server_key: it adopts the key and notifies JS (to keep the
             * shareable /join/<key> URL on the live game) but does NOT reauth,
             * so there is no wbnJoinArm here — identity was just re-stamped. */
            transportUdpServerSendWbnRekey(&udpServer.clients[i]);
            continue;
        }
        transportUdpServerSendWbnRekey(&udpServer.clients[i]);
        wbnJoinArm(&udpServer.clients[i].wbnJoin,
                   udpServer.tickCount, WBN_JOIN_REGISTER_GRACE_TICKS);
    }
}

/* Send map chunks to a client that is downloading */
/* Begin the armed map transfer once the bulk channel is quiescent. The
 * BulkSender's busy flag clears at staging-complete, not drain-complete, so a
 * correct endSeq needs the stream truly idle: no pending staging bytes and the
 * send window fully acked. startSeq is captured before any byte is staged, so
 * endSeq = startSeq + segment count is exact (channelStreamRefill only ever
 * forms a short final segment for a contiguous blob). A join download is
 * additionally gated on the client's PACKET_MAP_DL_READY (readySeen below);
 * a resync targets an already-established slot and its request is the
 * readiness signal. */
static void serverBeginMapTransferIfReady(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    ChannelMux *m = &udpServer.channelMux[slot];
    ChannelState *bulk = &m->ch[CHANNEL_BULK];
    BulkStreamHeader sh;
    uint32_t headerLen, totalBytes, segs;

    if (dl->xferKind == MAP_XFER_NONE || dl->xferBegun) return;
    /* A join download waits for the client's PACKET_MAP_DL_READY — proof the
     * accept landed and the receive buffers exist — so the stream head can
     * never arrive at a client that has nowhere to put it. A resync needs no
     * such gate: its own request is the readiness signal. */
    if (dl->xferKind == MAP_XFER_DOWNLOAD && !dl->readySeen) return;
    if (bulkSenderBusy(&udpServer.bulkSend[slot])) return;  /* preview draining */
    if (m->streamCount != 0) return;                        /* staging not empty  */
    if (bulk->ackedSeq != bulk->nextSeq) return;            /* window not drained */
    if (dl->compressedMap == NULL || dl->mapSize == 0) return;

    memset(&sh, 0, sizeof(sh));
    sh.kind = (dl->xferKind == MAP_XFER_RESYNC) ? BULK_KIND_RESYNC
                                                : BULK_KIND_DOWNLOAD;
    sh.gen = dl->resyncGen;          /* 0 for a join download */
    sh.totalSize = dl->mapSize;
    sh.pathLen = 0;
    sh.path[0] = '\0';

    dl->xferStartSeq = bulk->nextSeq;
    if (!bulkSenderBegin(&udpServer.bulkSend[slot], &sh,
                         dl->compressedMap, dl->mapSize)) {
        return;   /* allocation failure — retry next tick */
    }
    headerLen = (uint32_t)BULK_STREAM_HEADER_FIXED + sh.pathLen;
    totalBytes = headerLen + dl->mapSize;
    segs = (totalBytes + CHANNEL_BULK_SEG - 1) / CHANNEL_BULK_SEG;
    dl->xferEndSeq = dl->xferStartSeq + segs;
    dl->xferBegun = true;
}

/* Read transfer completion from the bulk channel. Once the peer has acked every
 * segment of the armed transfer (ackedSeq >= xferEndSeq) the same gates the old
 * chunk-ack path drove re-fire: a join download flips downloadComplete (snapshot
 * send + map-event flush lift); a resync clears resyncInProgress/resyncGen (the
 * held map events flush and the client's installedMapGen gate takes over). */
static void serverCompleteMapTransferIfAcked(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    ChannelState *bulk = &udpServer.channelMux[slot].ch[CHANNEL_BULK];

    if (dl->xferKind == MAP_XFER_NONE || !dl->xferBegun) return;
    if (bulk->ackedSeq < dl->xferEndSeq) return;

    if (dl->xferKind == MAP_XFER_DOWNLOAD) {
        dl->downloadComplete = TRUE;
        fprintf(stderr, "[UDP SERVER] Client %d map download complete\n", slot);
    } else { /* MAP_XFER_RESYNC */
        dl->resyncInProgress = FALSE;
        dl->resyncGen = 0;
    }
    dl->xferKind = MAP_XFER_NONE;
    dl->xferBegun = false;
}

/* Per-tick service: begin an armed transfer when the channel is idle, then
 * fire completion when the peer has acked it through. Safe to call every tick
 * for any connected slot; a no-op when nothing is armed. */
static void serverServiceMapTransfer(int slot) {
    serverBeginMapTransferIfReady(slot);
    serverCompleteMapTransferIfAcked(slot);
}

/* Per-tick spectator service: seed each connected spectator with the delayed
 * keyframe at head - specDelayTicks, drain it over CHANNEL_BULK exactly as the
 * client map-download carrier drains a join download, then stream the ring
 * forward — emit each record in (lastEmittedSeq, head - delay] as a
 * BULK_KIND_SPEC_RECORD blob so the view lags exactly specDelayTicks behind the
 * live head and never reaches the current tick. Cold-start tolerant — the seek
 * is retried every tick until the ring has enough history; the countdown signal
 * to the client is 2d-f, not here.
 *
 * Runs on the tick thread, the same thread as logWriteTick's
 * spectatorRingRecordTick, so the seek is race-free. A cursor/keyframe pointer
 * invalidates on the next RecordTick, so the keyframe bytes are copied into
 * spectator-owned storage at seek time (mirroring serverInitMapDownload's copy
 * of the compressed map); the bulk transfer then drains that copy across ticks. */

/* CHANNEL_CONTROL is a 64-deep reliable window; a cold-start countdown that ran
 * for up to specDelayTicks at one send per tick would overflow it. Resend the
 * countdown status no more than once every this many ticks (~2/s at 50 tick/s). */
#define SPEC_COUNTDOWN_RESEND_TICKS 25u

/* Arm/refresh a spectator's "spectating begins in X" countdown carrying
 * `remaining` ticks. Updates the state every tick (so a reader sees it track
 * toward zero) but only puts a SPEC_CTRL_COUNTDOWN on the 64-deep CHANNEL_CONTROL
 * window on first entry and then once per SPEC_COUNTDOWN_RESEND_TICKS, so a long
 * wait can't overflow it. Shared by the cutover gate (delayed view not yet at the
 * game) and the cold-start path (ring not yet holding a full delay of history). */
static void serverSpectatorArmCountdown(SpectatorConn *sp, uint32_t remaining) {
    bool firstEntry = !sp->inCountdown;
    sp->inCountdown        = true;
    sp->countdownRemaining = remaining;
    if (firstEntry ||
        udpServer.tickCount - sp->countdownSentTick >= SPEC_COUNTDOWN_RESEND_TICKS) {
        uint8_t buf[SPEC_CTRL_COUNTDOWN_LEN];
        buf[0] = SPEC_CTRL_COUNTDOWN;
        buf[1] = (uint8_t)(remaining >> 24);
        buf[2] = (uint8_t)(remaining >> 16);
        buf[3] = (uint8_t)(remaining >> 8);
        buf[4] = (uint8_t)remaining;
        channelSend(&sp->channelMux, CHANNEL_CONTROL, buf, SPEC_CTRL_COUNTDOWN_LEN);
        sp->countdownSentTick = udpServer.tickCount;
    }
}

static void serverServiceSpectators(ServerSim *sim) {
    int i;

    /* While the game runs the ring head is the latest game record, so track it
     * here every running tick (this runs unconditionally, even with no
     * spectators connected). When the game ends the head freezes at the last
     * running record X — game-over ticks are not recorded — and stays there
     * through the hold and the reset, so lastRunningSeq is exactly the record a
     * delayed spectator must reach to have watched the whole game, tail
     * included. All spectators share one ring and one X; a spectator still
     * draining an older game is held delayed by the lobby/countdown gate below
     * until it reaches the latest X. */
    if (serverSimGetState(sim) == serverStateRunning) {
        SpectatorRing *r = serverInstanceGetSpectatorRing();
        if (r != NULL) {
            uint32_t head = spectatorRingHeadSeq(r);
            /* First running record of THIS game: on the non-running->running
             * transition the head is the game segment's opening record. Freeze
             * it as the bar a delayed view must reach before it is past the
             * recorded pre-game lobby. */
            if (!udpServer.specWasRunning) {
                udpServer.gameStartSeq = head;
            }
            udpServer.lastRunningSeq = head;
        }
        udpServer.specWasRunning = true;
    } else {
        udpServer.specWasRunning = false;
    }

    for (i = 0; i < MAX_SPECTATORS; i++) {
        SpectatorConn *sp = &udpServer.spectators[i];

        if (!sp->connected) continue;

        /* Live-lobby spectators read the control bus, not the ring: skip the
         * entire delayed seed/feed path for them. Only the unconditional carrier
         * below runs, so their allowlist-filtered bus replay is still delivered.
         * In-game (delayed) viewers run the full block. */
        if (!sp->live) {
        ChannelState *bulk;

        /* Seek the delayed keyframe. OK -> copy it into spectator-owned storage
         * and clear any countdown. COLD_START (the ring does not yet hold a full
         * specDelayTicks of history) -> send a throttled countdown status on the
         * spectator's own CHANNEL_CONTROL and keep retrying; no seed is copied
         * and the feed below cannot run (!seedComplete), so no live state leaks
         * during the wait. The seek flips to OK exactly when head - delay >=
         * oldest, at which point the existing seed path takes over unchanged. */
        if (sp->seedBlob == NULL && !sp->seedComplete) {
            SpectatorRing *r = serverInstanceGetSpectatorRing();
            if (r != NULL) {
                uint32_t delay  = serverSimGetSpecDelayTicks(sim);
                uint32_t head   = spectatorRingHeadSeq(r);
                uint32_t target = (head > delay) ? head - delay : 0;
                /* Cutover gate: the delayed view has not yet reached the current
                 * game (head - delay is still inside the recorded pre-game lobby
                 * segment). Seeking here would seed that lobby and replay it — the
                 * static map a just-cut-over spectator already watched live, which
                 * reads as broken. Hold on the "spectating begins in X" countdown
                 * until head - delay reaches gameStartSeq instead; remaining is the
                 * exact records-to-go. Only game content is gated, never sent
                 * early, so the spec delay / anti-cheat bound is untouched. When
                 * gameStartSeq is 0 (no game has started, or the first record ever
                 * is the game's) target < 0 is impossible, so this no-ops and the
                 * seek below (and its cold-start countdown) runs unchanged. */
                if (target < udpServer.gameStartSeq) {
                    serverSpectatorArmCountdown(sp,
                                                udpServer.gameStartSeq - target);
                } else {
                SpectatorRingCursor cur;
                SpectatorRingSeekStatus st =
                    spectatorRingSeekDelayed(r, delay, &cur);
                if (st == SPECTATOR_RING_OK) {
                    int kfLen = 0;
                    const uint8_t *kf =
                        spectatorRingCursorKeyframe(&cur, &kfLen, NULL);
                    sp->inCountdown = false;
                    if (kf != NULL && kfLen > 0) {
                        uint8_t *copy = (uint8_t *)malloc((size_t)kfLen);
                        if (copy != NULL) {
                            memcpy(copy, kf, (size_t)kfLen);
                            sp->seedBlob = copy;
                            sp->seedLen  = (uint32_t)kfLen;
                            sp->seedGen  = cur.segment;
                            sp->seedSeq  = spectatorRingCursorSeedSeq(&cur);
                        }
                    }
                } else if (st == SPECTATOR_RING_COLD_START) {
                    uint32_t history = head - spectatorRingOldestSeq(r);
                    serverSpectatorArmCountdown(
                        sp, (delay > history) ? delay - history : 0);
                }
                /* AGED_OUT at join should not occur — retention covers the
                 * delay; leave it as a no-op and retry next tick. */
                }
            }
        }

        /* Arm a seed transfer once the bulk channel is fully idle — same gates
         * serverBeginMapTransferIfReady applies (sender idle, staging empty,
         * send window drained). */
        bulk = &sp->channelMux.ch[CHANNEL_BULK];
        if (sp->seedBlob != NULL && !sp->seedBegun &&
            !bulkSenderBusy(&sp->bulkSend) &&
            sp->channelMux.streamCount == 0 &&
            bulk->ackedSeq == bulk->nextSeq) {
            BulkStreamHeader sh;
            uint32_t headerLen, totalBytes, segs;

            memset(&sh, 0, sizeof(sh));
            sh.kind = BULK_KIND_SPEC_SEED;
            sh.gen = sp->seedGen;
            sh.totalSize = sp->seedLen;
            sh.pathLen = 0;
            sh.path[0] = '\0';

            sp->xferStartSeq = bulk->nextSeq;
            if (bulkSenderBegin(&sp->bulkSend, &sh, sp->seedBlob, sp->seedLen)) {
                headerLen = (uint32_t)BULK_STREAM_HEADER_FIXED + sh.pathLen;
                totalBytes = headerLen + sp->seedLen;
                segs = (totalBytes + CHANNEL_BULK_SEG - 1) / CHANNEL_BULK_SEG;
                sp->xferEndSeq = sp->xferStartSeq + segs;
                sp->seedBegun = true;
            }
            /* allocation failure: retry next tick */
        }

        /* Complete: peer has acked the whole seed. Free the copy and start the
         * forward feed from the seeded keyframe's recordSeq. Once-guarded
         * (!seedComplete) so the lastEmittedSeq init fires only on the
         * transition — the ack gate stays true on every later tick. */
        if (sp->seedBegun && !sp->seedComplete &&
            bulk->ackedSeq >= sp->xferEndSeq) {
            sp->seedComplete = true;
            if (sp->seedBlob != NULL) {
                free(sp->seedBlob);
                sp->seedBlob = NULL;
            }
            sp->lastEmittedSeq = sp->seedSeq;
        }

        /* Forward feed: after the seed, emit each ring record in
         * (lastEmittedSeq, head - delay] as a BULK_KIND_SPEC_RECORD blob,
         * walking recordSeq forward (no per-tick re-seek). The view lags exactly
         * specDelayTicks behind the live head and never reaches it (DD-7).
         * bulkSenderBegin copies each record, so it is assembled in a transient
         * local and freed at once; the bulkSenderBusy guard keeps one blob in
         * flight and is the per-tick staging backpressure. */
        if (sp->seedComplete) {
            SpectatorRing *r = serverInstanceGetSpectatorRing();
            if (r != NULL) {
                uint32_t head  = spectatorRingHeadSeq(r);
                uint32_t delay = serverSimGetSpecDelayTicks(sim);
                uint32_t target = (head > delay) ? head - delay : 0;

                while (sp->lastEmittedSeq < target &&
                       !bulkSenderBusy(&sp->bulkSend)) {
                    uint32_t seq = sp->lastEmittedSeq + 1;
                    bool isKf;
                    const uint8_t *pl;
                    int plen;
                    uint32_t gt, seg, blen;
                    uint8_t *blob;
                    BulkStreamHeader sh;

                    SDL_assert(delay == 0 || head - seq >= delay);   /* DD-7 */

                    if (!spectatorRingRecordAt(r, seq, &isKf, &pl, &plen,
                                               &gt, &seg)) {
                        /* The next record aged out (pathological slow
                         * spectator) — drop the feed and re-seed a fresh
                         * keyframe (the seek block re-arms once the channel
                         * drains). */
                        sp->seedComplete = false;
                        sp->seedBegun = false;
                        break;
                    }

                    blen = 9u + (uint32_t)plen;
                    blob = (uint8_t *)malloc(blen);
                    if (blob == NULL) break;   /* retry next tick */
                    blob[0] = isKf ? 1u : 0u;
                    packU32(blob + 1, gt);
                    packU32(blob + 5, seg);
                    if (plen > 0 && pl != NULL) {
                        memcpy(blob + 9, pl, (size_t)plen);
                    }

                    memset(&sh, 0, sizeof(sh));
                    sh.kind = BULK_KIND_SPEC_RECORD;
                    sh.gen = seq;
                    sh.totalSize = blen;
                    if (!bulkSenderBegin(&sp->bulkSend, &sh, blob, blen)) {
                        free(blob);
                        break;
                    }
                    free(blob);   /* bulkSenderBegin copied it into its own buf */
                    sp->lastEmittedSeq = seq;
                    bulkSenderPump(&sp->bulkSend, &sp->channelMux);
                }

                /* Drain-then-flip: once the spectator has replayed through the
                 * last running record (lastRunningSeq) it has shown the whole
                 * game, including the post-gameover tail, so when the live game
                 * is back in the lobby/countdown return it to the live lobby.
                 * Re-registering on the bus replays the current lobby in one
                 * step (the ~delay gap is jumped). A newer running game (live,
                 * or still unwatched in the ring) advances lastRunningSeq or
                 * holds the state non-lobby, so the spectator stays delayed.
                 * The flip may fire while a trailing delayed record is still
                 * draining on CHANNEL_BULK (no channel reset), which the client
                 * intake must tolerate. The aged-out path above may have
                 * cleared seedComplete, so re-check it. */
                if (sp->seedComplete &&
                    !bulkSenderBusy(&sp->bulkSend) &&
                    (serverSimGetState(sim) == serverStateLobby ||
                     serverSimGetState(sim) == serverStateCountdown) &&
                    sp->lastEmittedSeq >= udpServer.lastRunningSeq) {
                    /* Tear down the delayed feed, keeping the slot connected
                     * and its name; no channel reset. */
                    if (sp->seedBlob != NULL) {
                        free(sp->seedBlob);
                        sp->seedBlob = NULL;
                    }
                    sp->seedLen = 0;
                    sp->seedGen = 0;
                    sp->seedBegun = false;
                    sp->seedComplete = false;
                    sp->xferStartSeq = 0;
                    sp->xferEndSeq = 0;
                    sp->seedSeq = 0;
                    sp->lastEmittedSeq = 0;
                    sp->inCountdown = false;
                    sp->countdownRemaining = 0;
                    sp->countdownSentTick = 0;
                    SubscriberHandle sub = serverSimRegisterSubscriber(
                        sim, serverSpectatorDeliverControl, sp);
                    if (sub == SUBSCRIBER_HANDLE_INVALID) {
                        /* Subscriber budget full (unreachable within the 49-slot
                         * budget today): leave the spectator not-live so it stays
                         * on the delayed path and re-seeds/retries next service
                         * tick, rather than flip to a live state with no real
                         * subscription. Skip the live-lobby payload below —
                         * re-sending the accept would flip the client live against
                         * a server that isn't delivering. */
                        WB_LOG_WARN(WB_LOG_CAT_NET,
                                    "spectator %d: control-bus subscriber budget "
                                    "full at return-to-lobby; staying delayed", i);
                    } else {
                        sp->live = true;
                        sp->controlSub = sub;
                        /* Back in the live lobby (the map may have rotated since the
                         * game began): re-send the accept so the viewer re-allocates,
                         * and re-arm the lobby map so its preview refreshes. Mirrors
                         * the map-change path; does not alter the drain-flip above. */
                        serverSendSpectatorAccept(i, sim, &sp->addr);
                        serverArmSpectatorLobbyMap(i, /*resetChannel=*/true);
                        /* Catch-up: the lobby chat that accumulated while this
                         * spectator was finishing the delayed game. Delivered as one
                         * blob on CHANNEL_BULK — NOT on the reliable control window,
                         * which the same-tick sync re-register already fills (the
                         * backlog would overflow it and be dropped). Sent here, after
                         * the map re-arm has left the bulk sender idle and after the
                         * sync set the lobby phase (so inLobby is true when the client
                         * applies it); the armed map streams once this blob drains. */
                        serverSendSpectatorBacklog(i, sim);
                        mpDiagLog("[srv] spec idx=%d delayed->live "
                                  "(drained, lobby)", i);
                    }
                }
            }
        }
        }   /* end if (!sp->live) — delayed ring path */

        /* Live-lobby map: stream the armed lobby map to the viewer over its own
         * CHANNEL_BULK (mirrors a player's join download) so its lobby preview and
         * start positions render. Begun once the bulk channel is idle — the same
         * gate the seed uses — then the copy is freed (bulkSenderBegin copied it).
         * Only while sp->live, so it can never overlap the delayed seed (which
         * arms only when !sp->live); at the live->delayed cutover an in-flight
         * lobby map simply finishes first, since this same idle gate makes the
         * seed wait for the channel to drain. */
        if (sp->live && sp->lobbyMap != NULL) {
            ChannelState *lbulk = &sp->channelMux.ch[CHANNEL_BULK];
            if (!bulkSenderBusy(&sp->bulkSend) &&
                sp->channelMux.streamCount == 0 &&
                lbulk->ackedSeq == lbulk->nextSeq) {
                BulkStreamHeader sh;
                memset(&sh, 0, sizeof(sh));
                sh.kind = BULK_KIND_DOWNLOAD;
                sh.gen = 0;
                sh.totalSize = sp->lobbyMapSize;
                sh.pathLen = 0;
                sh.path[0] = '\0';
                if (bulkSenderBegin(&sp->bulkSend, &sh,
                                    sp->lobbyMap, sp->lobbyMapSize)) {
                    free(sp->lobbyMap);
                    sp->lobbyMap = NULL;
                    sp->lobbyMapSize = 0;
                }
                /* allocation failure: keep lobbyMap, retry next tick */
            }
        }

        /* Carrier: pump staged bytes into the mux and emit standalone
         * PACKET_CHANNEL frames — carries both the seed and the forward records.
         * Mirrors the client map-download carrier exactly. */
        bulkSenderPump(&sp->bulkSend, &sp->channelMux);
        channelTick(&sp->channelMux, udpServer.tickCount, sp->pingMs);
        {
            int frames;
            for (frames = 0; frames < MAP_DOWNLOAD_FRAMES_PER_TICK; frames++) {
                uint8_t cbuf[UDP_MAX_PAYLOAD];
                int frameLen = channelBuildFrame(
                    &sp->channelMux, cbuf + PACKET_HEADER_SIZE,
                    UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
                if (frameLen <= 2) break;   /* nothing (more) to carry this tick */
                packHeader(cbuf, PACKET_CHANNEL, sp->outSequence++);
                srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen, &sp->addr);
            }
        }
    }
}

/* Initialize map download tracking for a client and arm a join download on the
 * bulk channel. The blob begins streaming once the channel is idle and the
 * per-tick carrier (transportUdpServerSend) feeds it. */
void serverInitMapDownload(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];

    if (dl->compressedMap != NULL) {
        free(dl->compressedMap);
    }
    dl->compressedMap = (BYTE *)malloc(udpServer.compressedMapSize);
    memcpy(dl->compressedMap, udpServer.compressedMap, udpServer.compressedMapSize);
    dl->mapSize = udpServer.compressedMapSize;
    dl->downloadComplete = FALSE;
    dl->resyncInProgress = FALSE;
    dl->resyncGen = 0;
    dl->xferKind = MAP_XFER_DOWNLOAD;
    dl->xferBegun = false;
    dl->xferStartSeq = 0;
    dl->xferEndSeq = 0;
    dl->readySeen = false;
}

/* Drop any in-flight map transfer for `slot` and re-base CHANNEL_BULK so the
 * next stream starts clean on both ends: bulkSenderReset drops the old staged
 * blob, channelResetSend(CHANNEL_BULK) collapses the send window and clears
 * the staging tail, and a CTRL_CHANNEL_RESET carries the new bulk baseline so
 * the client lifts its receive baseline and abandons any old partial. Without
 * this the old transfer's stragglers would segmentize into the new stream and
 * the client's single BulkReceiver would misparse it. Then arm a fresh join
 * download from the current blob (re-gates snapshots). The download begins
 * once the client's PACKET_MAP_DL_READY arrives (serverBeginMapTransferIfReady). */
static void serverRebaseBulkAndRearmDownload(int i) {
    bulkSenderReset(&udpServer.bulkSend[i]);
    {
        uint32_t b3 = channelResetSend(&udpServer.channelMux[i], CHANNEL_BULK);
        ControlEvent resetEvt;
        ControlEncodeBodyFn enc =
            transportControlCodecBodyEncoder(CTRL_CHANNEL_RESET);
        uint8_t msg[CHANNEL_CONTROL_SEG];
        size_t bodyLen = 0;
        memset(&resetEvt, 0, sizeof(resetEvt));
        resetEvt.type = CTRL_CHANNEL_RESET;
        resetEvt.u.channelReset.channelMask = (uint8_t)(1u << CHANNEL_BULK);
        resetEvt.u.channelReset.ch3Baseline = b3;
        if (enc != NULL &&
            enc(&resetEvt, &udpServer.clients[i], msg + 3,
                sizeof(msg) - 3, &bodyLen) == ENCODE_OK) {
            msg[0] = (uint8_t)CTRL_CHANNEL_RESET;
            packU16(msg + 1, (uint16_t)bodyLen);
            if (!channelSend(&udpServer.channelMux[i], CHANNEL_CONTROL,
                             msg, (uint16_t)(3 + bodyLen))) {
                if (!udpServer.pendingSimRemove[i]) {
                    WB_LOG_ERROR(WB_LOG_CAT_NET,
                                 "control channel overflow sending bulk reset "
                                 "for slot %d, deferring disconnect", i);
                    udpServer.pendingSimRemove[i] = true;
                }
            }
        }
    }
    serverInitMapDownload(i);
}

/* Clean up map download tracking for a client */
static void serverCleanupMapDownload(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    if (dl->compressedMap != NULL) {
        free(dl->compressedMap);
        dl->compressedMap = NULL;
    }
    dl->downloadComplete = FALSE;
    dl->xferKind = MAP_XFER_NONE;
    dl->xferBegun = false;
    dl->readySeen = false;
}

/* Reassembled-upload completion: hand the bytes to the sim (in-memory reload,
 * plus a persist stage under PERSIST policy), clear the per-client upload slot,
 * and reply MAP_UPLOAD_DONE. The bytes already sit in clientUploadBuf because
 * the bulk receiver's onBegin pointed it there. */
static void serverFinishUpload(ServerSim *sim, int clientIdx) {
    uint32_t total = udpServer.clientUploadTotal[clientIdx];
    const char *origName = udpServer.clientUploadName[clientIdx];

    char displayName[MAP_STR_SIZE];
    SDL_strlcpy(displayName, origName, sizeof(displayName));
    {
        size_t dlen = SDL_strlen(displayName);
        if (dlen >= 4 &&
            SDL_strcasecmp(displayName + dlen - 4, ".map") == 0) {
            displayName[dlen - 4] = '\0';
        }
    }

    bool previewed = serverSimReloadCompressedInMemory(
        sim, udpServer.clientUploadBuf[clientIdx], (int)total, displayName);

    /* PERSIST: write the accepted bytes to disk. The configured persist
     * directory is the concrete home of the virtual "Uploads/" folder; when
     * unset it falls back to "<mapDirRoot>/Uploads" (WinBoloDS back-compat).
     * The per-map file/storage caps were already enforced at MAP_UPLOAD_BEGIN.
     * An I/O failure is logged and swallowed — the in-memory preview stands. */
    if (previewed && udpServer.uploadPolicy == UPLOAD_POLICY_PERSIST) {
        char persistDir[FILENAME_MAX];
        if (udpServer.uploadPersistDir[0] != '\0') {
            SDL_strlcpy(persistDir, udpServer.uploadPersistDir,
                        sizeof(persistDir));
        } else {
            SDL_snprintf(persistDir, sizeof(persistDir), "%s/Uploads",
                         serverSimGetMapDirRoot(sim));
        }
        /* SDL_CreateDirectory creates missing parents; a no-op if it exists. */
        if (!SDL_CreateDirectory(persistDir)) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                "persist upload: cannot create directory '%s': %s",
                persistDir, SDL_GetError());
        } else {
            char persistPath[FILENAME_MAX];
            SDL_snprintf(persistPath, sizeof(persistPath), "%s/%s.map",
                         persistDir, displayName);
            FILE *pf = fopen(persistPath, "wb");
            if (pf == NULL) {
                WB_LOG_WARN(WB_LOG_CAT_NET,
                    "persist upload: cannot open '%s' for write", persistPath);
            } else {
                size_t wrote = fwrite(udpServer.clientUploadBuf[clientIdx],
                                      1, total, pf);
                fclose(pf);
                if (wrote != total) {
                    WB_LOG_WARN(WB_LOG_CAT_NET,
                        "persist upload: short write (%zu/%u) to '%s'",
                        wrote, total, persistPath);
                } else {
                    WB_LOG_INFO(WB_LOG_CAT_NET,
                        "persist upload: wrote '%s' (%u bytes)",
                        persistPath, total);
                }
            }
        }
    }

    udpServer.clientUploadActive[clientIdx] = false;
    udpServer.clientUploadTotal[clientIdx]  = 0;

    char relReturn[256];
    SDL_snprintf(relReturn, sizeof(relReturn), "Uploads/%s", origName);
    int relLen = (int)SDL_strlen(relReturn);
    if (relLen > 255) relLen = 255;
    uint8_t done[PACKET_HEADER_SIZE + 2 + 256];
    int dpos = PACKET_HEADER_SIZE;
    packHeader(done, PACKET_LOBBY_MAP_UPLOAD_DONE, 0);
    done[dpos++] = previewed ? 0 : LOBBY_REJECT_INVALID;
    done[dpos++] = (uint8_t)relLen;
    memcpy(done + dpos, relReturn, relLen);
    dpos += relLen;
    srvSendTo(done, dpos, &udpServer.clients[clientIdx].addr);
}

/* Bulk-receiver sink for a client->server map upload on CHANNEL_BULK. onBegin
 * validates the announced size against the approved BEGIN and the hard cap,
 * then points the receiver at the per-client upload buffer; onComplete runs
 * the reload/persist + DONE reply. */
typedef struct {
    ServerSim *sim;
    int        clientIdx;
} ServerUploadSinkCtx;

static uint8_t *serverBulkUploadOnBegin(void *vctx, const BulkStreamHeader *h) {
    ServerUploadSinkCtx *ctx = (ServerUploadSinkCtx *)vctx;
    int idx = ctx->clientIdx;
    if (h->kind != BULK_KIND_UPLOAD) return NULL;
    if (!udpServer.clientUploadActive[idx]) return NULL;        /* no approved BEGIN */
    if (h->totalSize != udpServer.clientUploadTotal[idx]) return NULL; /* size mismatch */
    if (h->totalSize == 0 || h->totalSize > LOBBY_MAP_UPLOAD_MAX_BYTES) return NULL;
    return udpServer.clientUploadBuf[idx];
}

static void serverBulkUploadOnComplete(void *vctx, const BulkStreamHeader *h,
                                       uint8_t *buf) {
    ServerUploadSinkCtx *ctx = (ServerUploadSinkCtx *)vctx;
    (void)h;
    (void)buf;
    serverFinishUpload(ctx->sim, ctx->clientIdx);
}

/* Drain every stream fragment waiting on this client's CHANNEL_BULK through the
 * upload receiver. Called wherever the client's channel frames are ingested. */
static void serverDrainBulk(ServerSim *sim, int clientIdx) {
    ServerUploadSinkCtx ctx;
    BulkRecvSink sink;
    uint8_t chanBuf[CHANNEL_MAX_SEG];
    uint16_t chanLen;
    ctx.sim = sim;
    ctx.clientIdx = clientIdx;
    sink.onBegin = serverBulkUploadOnBegin;
    sink.onComplete = serverBulkUploadOnComplete;
    sink.ctx = &ctx;
    while (channelReceive(&udpServer.channelMux[clientIdx], CHANNEL_BULK,
                          chanBuf, &chanLen)) {
        bulkReceiverFeed(&udpServer.bulkRecvUp[clientIdx], chanBuf, chanLen,
                         &sink);
    }
}

/* Handle input packet from a connected client */
static void serverHandleInput(const uint8_t *buf, int len,
                              const struct sockaddr_in *fromAddr,
                              ServerSim *sim) {
    int clientIdx;
    /* INPUT framing: [header 8][connId 8][count 1][25-byte inputs…]. */
    int pos = PACKET_HEADER_SIZE + 8;
    uint8_t inputCount;
    uint64_t connId = 0;
    bool rehome = false;
    int i;

    /* Match the session by connId first so a client whose NAT mapping rebound
     * keeps its slot; fall back to IP:port when the connId is absent (0) or
     * matches no slot. The connId is only present on a packet long enough to
     * hold it — a short/old frame leaves it 0 and takes the IP:port path. */
    if (len >= PACKET_HEADER_SIZE + 8) {
        connId = unpackConnId(buf + PACKET_HEADER_SIZE);
    }
    clientIdx = transportUdpServerFindByConnId(udpServer.clients, connId,
                                               fromAddr, &rehome);
    if (clientIdx >= 0) {
        if (rehome) {
            char oldAddr[32];
            snprintf(oldAddr, sizeof(oldAddr), "%s:%u",
                     inet_ntoa(udpServer.clients[clientIdx].addr.sin_addr),
                     (unsigned)ntohs(udpServer.clients[clientIdx].addr.sin_port));
            WB_LOG_INFO(WB_LOG_CAT_NET,
                "slot %d NAT rebind: %s -> %s:%u (connId match, re-homing)",
                clientIdx, oldAddr, inet_ntoa(fromAddr->sin_addr),
                (unsigned)ntohs(fromAddr->sin_port));
            udpServer.clients[clientIdx].addr = *fromAddr;
        }
    } else {
        clientIdx = serverFindClient(fromAddr);
    }
    if (clientIdx < 0) return; /* Unknown client */

    udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;

    /* Only process inputs during running state — silently discard otherwise */
    if (serverSimGetState(sim) != serverStateRunning) {
        return;
    }

    if (len < pos + 1) return;
    inputCount = buf[pos++];
    if (inputCount > INPUT_REDUNDANCY_COUNT) inputCount = INPUT_REDUNDANCY_COUNT;

    /* Process each input — the last one is the newest.
     * Apply only inputs newer than what we've already processed. */
    for (i = 0; i < inputCount; i++) {
        InputPacket pkt;
        if (len < pos + INPUT_PACKET_WIRE_SIZE) break;
        unpackInputPacket(buf + pos, &pkt);
        pos += INPUT_PACKET_WIRE_SIZE;

        /* Override playerNum to prevent spoofing */
        pkt.playerNum = (uint8_t)clientIdx;

        /* Advance the reliable map-event ACK from this client.  Game events
         * ride CHANNEL_GAME with their own acks; the InputPacket carries no
         * game-event ack. */
        if (pkt.mapEventAck > udpServer.mapEventQueues[clientIdx].ackedSeq) {
            udpServer.mapEventQueues[clientIdx].ackedSeq = pkt.mapEventAck;
        }
        /* Control events ride CHANNEL_CONTROL; their acks arrive on the
         * channel frame trailer (ingested below), not in the input packet. */

        /* Only apply if this is a newer input than what we last processed */
        if (pkt.tick > serverSimGetLastProcessedInput(sim, clientIdx)) {
            if (udpServer.clients[clientIdx].inputsThisTick >= INPUT_REDUNDANCY_COUNT) break;
            serverSimApplyInput(sim, &pkt);
            udpServer.clients[clientIdx].inputsThisTick++;
        }
    }

    /* Anything past the inputs is the parallel channel layer's trailer. */
    if (pos < len &&
        channelRecvFrame(&udpServer.channelMux[clientIdx],
                         buf + pos, len - pos) >= 0) {
        udpServer.channelFramesRx[clientIdx]++;
        serverDrainBulk(sim, clientIdx);
    }
}

/* Handle ping from client — respond with pong */
static void serverHandlePing(const uint8_t *buf, int len,
                              const struct sockaddr_in *fromAddr) {
    uint8_t pongBuf[PACKET_HEADER_SIZE + 8];
    uint32_t clientTime;

    if (len < PACKET_HEADER_SIZE + 8) return;

    clientTime = unpackU32(buf + PACKET_HEADER_SIZE);

    /* Only respond to pings from connected clients — otherwise a
     * disconnected client keeps receiving pongs and never detects
     * that the server dropped it. */
    {
        uint32_t now = SDL_GetTicks();
        int clientIdx = serverFindClient(fromAddr);
        if (clientIdx < 0) return;

        /* Server-measured RTT: the second uint32 from the client is the server
         * timestamp we sent in the previous PONG, echoed back.  RTT = now - that. */
        {
            uint32_t echoedServerTime = unpackU32(buf + PACKET_HEADER_SIZE + 4);
            if (echoedServerTime > 0) {
                udpServer.clients[clientIdx].pingMs = (uint16_t)(now - echoedServerTime);
            }
        }
        udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;

        /* Echo-only PING (clientTime == 0): server already computed RTT above,
         * don't send a PONG back or it creates an infinite ping-pong loop. */
        if (clientTime == 0) return;

        udpServer.clients[clientIdx].lastPongSentMs = now;
        packHeader(pongBuf, PACKET_PONG, 0);
        packU32(pongBuf + PACKET_HEADER_SIZE, clientTime);
        packU32(pongBuf + PACKET_HEADER_SIZE + 4, now);
    }
    /* wire-only: per-client handshake (response to a single client's request) */
    srvSendTo(pongBuf, sizeof(pongBuf), fromAddr);
}

/* Build and send a snapshot to one client.
 * Uses serverSimBuildSnapshot() for all game state, then serializes
 * and appends reliable events from the per-client queue. */
static void serverSendSnapshot(ServerSim *sim, int clientIdx) {
    uint8_t buf[2048];
    int pos;
    int i;
    int countsPos;
    SnapshotHeader hdr;
    TankSnapshot tankSnaps[MAX_TANKS];
    ShellSnapshot shellSnaps[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot tkExplSnaps[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot baseSnaps[MAX_SNAPSHOT_BASES];
    PillSnapshot pillSnaps[MAX_SNAPSHOT_PILLS];
    GameEvent eventSnaps[MAX_SNAPSHOT_EVENTS];
    ClientEventQueue *mapQ = &udpServer.mapEventQueues[clientIdx];
    UdpServerClient *client = &udpServer.clients[clientIdx];

    /* Build snapshot from sim state (same code as local transport) */
    serverSimBuildSnapshot(sim, (BYTE)clientIdx, &hdr,
                           tankSnaps, MAX_TANKS,
                           shellSnaps, MAX_SNAPSHOT_SHELLS,
                           tkExplSnaps, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           baseSnaps, MAX_SNAPSHOT_BASES,
                           pillSnaps, MAX_SNAPSHOT_PILLS,
                           eventSnaps, MAX_SNAPSHOT_EVENTS,
                           false);

    /* Packet header */
    packHeader(buf, PACKET_STATE_SNAPSHOT, client->outSequence++);
    pos = PACKET_HEADER_SIZE;

    /* Snapshot header — we'll fill in counts after packing data.
     * Format: serverTick(4) + lastProcessedInput(4) + tankCount(1)
     * + shellCount(1) + tkExplosionCount(1)
     * + baseCount(1) + pillCount(1)
     * + mapChecksum(2) + returnToLobbyTicks(2) = SNAPSHOT_HEADER_WIRE_SIZE.
     * The static assert ties that constant to this field breakdown, and the
     * reserve below derives from it, so this packer and the client's size
     * guard can't drift. */
    BOLO_STATIC_ASSERT(SNAPSHOT_HEADER_WIRE_SIZE == 4 + 4 + 5 + 2 + 2,
                       snapshot_header_wire_size);
    packU32(buf + pos, hdr.serverTick);
    pos += 4;
    packU32(buf + pos, hdr.lastProcessedInput);
    pos += 4;
    countsPos = pos;
    /* Reserve the 5 count bytes + 2-byte mapChecksum + 2-byte
     * returnToLobbyTicks — the header bytes after the two u32s above. */
    pos += SNAPSHOT_HEADER_WIRE_SIZE - 8;

    /* Pack tank snapshots — variable length: a stub is 1 byte; a full entry is
     * a presence-mask-driven run of at most TANK_SNAPSHOT_WIRE_SIZE bytes
     * (packTankSnapshot returns the actual size, usually far smaller because
     * zero field groups are omitted). Reserve the conservative max here. */
    for (i = 0; i < hdr.tankCount; i++) {
        bool isStub = (tankSnaps[i].playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) != 0;
        int needed = isStub ? 1 : TANK_SNAPSHOT_WIRE_SIZE;
        if (pos + needed > (int)sizeof(buf)) {
            hdr.tankCount = (uint8_t)i;
            break;
        }
        pos += packTankSnapshot(buf + pos, &tankSnaps[i]);
    }

    /* Pack shell snapshots */
    for (i = 0; i < hdr.shellCount; i++) {
        if (pos + SHELL_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.shellCount = (uint8_t)i;
            break;
        }
        pos += packShellSnapshot(buf + pos, &shellSnaps[i]);
    }

    /* Pack tank explosion snapshots */
    for (i = 0; i < hdr.tkExplosionCount; i++) {
        if (pos + TK_EXPLOSION_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.tkExplosionCount = (uint8_t)i;
            break;
        }
        pos += packTkExplosionSnapshot(buf + pos, &tkExplSnaps[i]);
    }

    /* Pack base snapshots */
    for (i = 0; i < hdr.baseCount; i++) {
        if (pos + BASE_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.baseCount = (uint8_t)i;
            break;
        }
        pos += packBaseSnapshot(buf + pos, &baseSnaps[i]);
    }

    /* Pack pill snapshots */
    for (i = 0; i < hdr.pillCount; i++) {
        if (pos + PILL_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.pillCount = (uint8_t)i;
            break;
        }
        pos += packPillSnapshot(buf + pos, &pillSnaps[i]);
    }

    /* No snapshot reliable game-tail — game events ride CHANNEL_GAME. */

    /* Drain held map-change events onto reliable channel 1 (CHANNEL_MAP),
     * tagged with this slot's map generation: payload = [gen u32][GameEvent].
     * mapEventQueues stays the hold buffer — while a download or live resync is
     * in flight the freshly compressed blob already carries every change up to
     * the cut, and changes during the transfer sit undrained, so hold them
     * (gate on downloadComplete && !resyncInProgress) and flush once both gates
     * clear. Each successful channelSend advances ackedSeq to free the slot.
     * A full window defers the disconnect off this path, mirroring the
     * game-channel overflow. The snapshot no longer carries a map tail. */
    if (udpServer.mapDownload[clientIdx].downloadComplete &&
        !udpServer.mapDownload[clientIdx].resyncInProgress) {
        uint32_t seq;
        for (seq = mapQ->ackedSeq; seq < mapQ->nextSeq; seq++) {
            uint32_t idx = seq % RELIABLE_EVENT_BUFFER_SIZE;
            uint8_t mapMsg[4 + GAME_EVENT_MAX_WIRE_SIZE];
            int evLen;
            if (mapQ->buffer[idx].seq != seq) break; /* Buffer wrapped — stop */
            packU32(mapMsg, udpServer.mapGen[clientIdx]);
            evLen = packGameEvent(mapMsg + 4, &mapQ->buffer[idx].event);
            if (!channelSend(&udpServer.channelMux[clientIdx], CHANNEL_MAP,
                             mapMsg, (uint16_t)(4 + evLen))) {
                if (!udpServer.pendingSimRemove[clientIdx]) {
                    WB_LOG_ERROR(WB_LOG_CAT_NET,
                                 "map channel overflow for slot %d, deferring disconnect",
                                 clientIdx);
                    udpServer.pendingSimRemove[clientIdx] = true;
                }
                break;
            }
            mapQ->ackedSeq = seq + 1; /* Sent reliably — free the hold slot. */
        }
    }

    /* Control events ride reliable channel 2 (CHANNEL_CONTROL), carried by the
     * channel-frame trailer appended below — not this snapshot tail. */

    /* Fill in counts */
    buf[countsPos]     = hdr.tankCount;
    buf[countsPos + 1] = hdr.shellCount;
    buf[countsPos + 2] = hdr.tkExplosionCount;
    buf[countsPos + 3] = hdr.baseCount;
    buf[countsPos + 4] = hdr.pillCount;
    packU16(buf + countsPos + 5, hdr.mapChecksum);
    packU16(buf + countsPos + 7, hdr.returnToLobbyTicks);

    /* Parallel channel layer rides as a trailer on the snapshot: tick the
     * mux on this client's clock+RTT, then append one channel frame after
     * the event tails, keeping the datagram within UDP_MAX_PAYLOAD.  The
     * client recovers it as the bytes past the snapshot's parsed end. */
    bulkSenderPump(&udpServer.bulkSend[clientIdx], &udpServer.channelMux[clientIdx]);
    channelTick(&udpServer.channelMux[clientIdx], udpServer.tickCount,
                client->pingMs);
    {
        int budget = UDP_MAX_PAYLOAD - pos;
        if (budget >= 2) {
            pos += channelBuildFrame(&udpServer.channelMux[clientIdx],
                                     buf + pos, budget);
        }
    }

    /* wire-only: per-tick snapshot — high-volume delta-encoded path with its own reliability discipline */
    srvSendTo(buf, pos, &client->addr);
}

/* Disconnect a player by index.
 * graceful=TRUE means the player chose to quit (sends "is quitting" message).
 * graceful=FALSE means timeout or kick (no quit message). */
/* Check if all connected clients have voted to lock.
 * Returns TRUE only if there is at least one client and all are locked. */
static bool serverClientsAllLocked(void) {
    int i;
    bool anyConnected = false;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            anyConnected = true;
            if (!udpServer.clientLocked[i]) return false;
        }
    }
    return anyConnected;
}

void serverDisconnectClient(ServerSim *sim, int idx, bool graceful) {
    char msg[128];
    if (!udpServer.clients[idx].connected) return;
    /* An actual disconnect supersedes any deferred overflow-disconnect for
     * this slot (see the queue-overflow branch in udpClientDeliverControl):
     * clear the flag so the drain can't later tear down a fresh occupant that
     * reused the slot after this teardown frees it. */
    udpServer.pendingSimRemove[idx] = false;

    {
        UdpServerClient *c = &udpServer.clients[idx];
        (void)c;
        WB_LOG_DEBUG(WB_LOG_CAT_NET,
            "disconnect slot=%d name='%s' addr=%s:%u graceful=%d "
            "tickCount=%u lastReceivedTick=%u tickDiff=%u (timeout=%d)",
            idx, c->playerName,
            inet_ntoa(c->addr.sin_addr), (unsigned)ntohs(c->addr.sin_port),
            (int)graceful,
            (unsigned)udpServer.tickCount,
            (unsigned)c->lastReceivedTick,
            (unsigned)(udpServer.tickCount - c->lastReceivedTick),
            (int)CLIENT_TIMEOUT_TICKS);
        mpDiagLog("[srv] DISCONNECT slot=%d name='%s' graceful=%d tickDiff=%u (timeout=%d)",
                  idx, c->playerName, (int)graceful,
                  (unsigned)(udpServer.tickCount - c->lastReceivedTick),
                  (int)CLIENT_TIMEOUT_TICKS);
    }

    if (graceful) {
        snprintf(msg, sizeof(msg), "%s is quitting.",
                 udpServer.clients[idx].playerName);
        winbolonetAddEvent(WINBOLO_NET_EVENT_QUITTING, TRUE,
                           (BYTE)idx, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
    } else {
        snprintf(msg, sizeof(msg), "%s timed out.",
                 udpServer.clients[idx].playerName);
    }
    WB_LOG_INFO(WB_LOG_CAT_NET, "%s", msg);
    fprintf(stderr, "[UDP SERVER] %s\n", msg);
    serverSimConsoleMessage(msg);

    /* Mirror the leave into every client's lobby chat panel via
     * CTRL_SERVER_TEXT. Console keeps the graceful-vs-timeout detail
     * (`msg` above); the chat line is the uniform "X has left." form
     * — players don't need the distinction and it matches what the
     * client-side wire-packet branch used to render. Done before the
     * subscriber/slot teardown below so the leaving client's
     * still-attached subscriber sees it if they're reachable, and
     * the formatted name is still in udpServer.clients[idx].playerName
     * (wiped below). */
    {
        char chatMsg[32 + PACKET_MAX_PLAYER_NAME];
        snprintf(chatMsg, sizeof(chatMsg), "%s has left.",
                 udpServer.clients[idx].playerName);
        /* Flip connected=false BEFORE the broadcast. Without this, the
         * "X has left." event re-enters udpClientDeliverControl for this
         * same slot; if the leaving slot's queue is what overflowed in
         * the first place (the path that brought us into this function
         * via the queue-overflow branch), the enqueue fails again and
         * recurses back into serverDisconnectClient — unbounded
         * recursion until the timer thread's stack blows. The deliver
         * callback's existing !connected short-circuit makes the
         * broadcast a no-op for this slot; the other slots still see
         * "X has left." normally. */
        udpServer.clients[idx].connected = false;
        serverSendServerEnglishBroadcast(sim, chatMsg);
    }

    /* Notify WinBolo.net that the player is leaving (must happen before
     * clearing the slot so the player key is still valid) */
    winboloNetClientLeaveGame((BYTE)idx,
                              serverSimGetNumPlayers(sim),
                              serverSimGetNumNeutralBases(sim),
                              serverSimGetNumNeutralPills(sim));

    /* PACKET_PLAYER_LEFT is fanned out by the codec when the outer caller
     * invokes serverSimRemovePlayer; no hand-built broadcast here. */
    serverSimUnregisterSubscriber(sim, udpServer.clients[idx].controlSub);
    udpServer.clients[idx].controlSub = SUBSCRIBER_HANDLE_INVALID;
    udpServer.clients[idx].nameStickySuffix = false;
    udpServer.clients[idx].claimPending = false;
    udpServer.clients[idx].claimDesiredName[0] = '\0';
    udpServer.clients[idx].inboundCmdSeq = 0;
    /* Slots are recycled: without this a new occupant would inherit the
     * previous player's mutes. */
    udpServer.clients[idx].voiceMuteMask = 0;
    /* The other direction of the same hazard, and the one that is invisible
     * to the player it hits: everyone who muted THIS player still holds the
     * bit for this slot, so the next occupant would arrive already muted for
     * them — no voice, and no chat either, since both read this one bit.
     * Sweeping the leaver's bit out of every mask is what makes a mute
     * belong to the player rather than to the slot they sat in. */
    {
        const PlayerBitMap leaving = ~((PlayerBitMap)1u << idx);
        int muter;
        for (muter = 0; muter < MAX_TANKS; muter++) {
            udpServer.clients[muter].voiceMuteMask &= leaving;
        }
    }
    /* Same reason: a recycled slot inheriting the previous player's onset
     * would hand a new joiner talker priority they did not earn. */
    udpServer.voiceLastFrameTick[idx] = 0;
    udpServer.voiceOnsetTick[idx] = 0;
    /* If the set every other client holds names this slot as talking, they go
     * on showing it until they are told otherwise, and clearing the tick above
     * is what stops the next pass computing a set that differs from the
     * published one. Ask for the send explicitly so the leaver goes out of that
     * set even when what remains is empty — which is exactly the case where
     * nothing else would send it. A leaver nobody was hearing is not in the
     * published set and costs no event. */
    if ((udpServer.voiceTalkingPublished & ((PlayerBitMap)1u << idx)) != 0) {
        udpServer.voiceTalkingResend = true;
    }
    memset(udpServer.clients[idx].playerName, 0, PACKET_MAX_PLAYER_NAME);
    udpServer.clientLocked[idx] = false;
    /* Drop any owed PLAYER_JOIN — the player left before it resolved, so
     * no orphan anonymous join (and no leave it would need to pair with). */
    wbnJoinClear(&udpServer.clients[idx].wbnJoin);
    /* Slot is free; a fresh occupant re-establishes WBN status at its join. */
    udpServer.clients[idx].wbnWasVerified = false;
    udpServer.clients[idx].wbnWebIdentityCached = false;
    udpServer.clients[idx].wbnWebIsLoggedIn = false;
    udpServer.clients[idx].wbnWebName[0] = '\0';
    udpServer.clients[idx].wbnWebCountry[0] = '\0';
    udpServer.clients[idx].wbnWebUserId = -1;

    /* Clear the high-ping enforcement tally. These count consecutive breaches
     * by ONE occupant, so leaving them set hands the next occupant of this slot
     * the departing player's strikes. A player kicked for high ping leaves with
     * pingKickStrikes at PING_KICK_COUNT, so on their reconnect the first
     * measurement over the threshold reaches PING_KICK_COUNT + 1 and kicks them
     * again immediately, with none of the PING_KICK_COUNT samples of grace the
     * enforcement is built around — and pingWarned still true, so they get no
     * warning first either. lastEnforcedPingMs goes too: it is the dedup for
     * "already counted this measurement", and a stale value silently skips the
     * next occupant's first matching sample. */
    udpServer.clients[idx].pingKickStrikes = 0;
    udpServer.clients[idx].pingWarnStrikes = 0;
    udpServer.clients[idx].pingWarned = false;
    udpServer.clients[idx].lastEnforcedPingMs = 0;

    /* Reset control-sync state so a re-using slot starts fresh. */
    udpServer.controlSyncInProgress[idx] = false;

    /* Hand the slot's copy of the terrain back to the tick: nothing is being
     * sent map events on this slot any more, and a bot or the host player can
     * take it next, which is a client that gets every change. */
    serverSimSetShadowCulled(sim, (BYTE)idx, false);

    /* Reset the channel mux so a re-using slot starts fresh. */
    channelMuxInit(&udpServer.channelMux[idx]);
    udpServer.channelFramesRx[idx] = 0;
    udpServer.mapGen[idx] = 0;
    bulkSenderReset(&udpServer.bulkSend[idx]);
    bulkReceiverInit(&udpServer.bulkRecvUp[idx]);
    udpServerResetRoundLogLimits(idx);
    udpServerResetMapReaskLimit(idx);

    /* Release any in-flight upload state. Without this, a client
     * who drops mid-upload would leave clientUploadActive set,
     * blocking every subsequent UPLOAD_BEGIN from a different
     * client with LOBBY_REJECT_UPLOAD_BUSY until server restart. */
    udpServerClearClientUploadState(idx);

    /* Unready any humans who were ready — a leaver changes the lobby
     * composition. A no-op in running state (no one is ready then).
     * Outer callers also fire serverSimRemovePlayer immediately after
     * this; running this here rather than in each caller keeps the
     * leave-side hook centralized alongside the chat broadcast above. */
    lobbyAutoUnreadyOnChange(sim);
}

void transportUdpServerDrainPendingRemovals(ServerSim *sim) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.pendingSimRemove[i]) continue;
        udpServer.pendingSimRemove[i] = false;
        /* The slot was flagged on a control-queue overflow detected inside a
         * publish (udpClientDeliverControl); the whole disconnect was deferred
         * to here so its leave broadcast + PLAYER_LEFT fan-out run outside any
         * publish. If the slot is no longer connected, another path already
         * disconnected it (and cleared this flag before any reuse), so there is
         * nothing to tear down. */
        if (!udpServer.clients[i].connected) continue;
        /* Same teardown order every other disconnect path uses — the download
         * buffer is caller-owned, serverDisconnectClient does not free it. */
        serverCleanupMapDownload(i);
        serverDisconnectClient(sim, i, false);
        serverSimRemovePlayer(sim, (BYTE)i);
    }
}

/* Send a localized server-originated message to all connected clients
 * via PACKET_CHAT_BROADCAST.  Uses fromPlayer=0xFF, destPlayer=0xFF to
 * mark the localized variant; client decodes langid + args and renders
 * via langGetTextFmt.  The packed langid+args payload rides as the
 * opaque body[] of CTRL_CHAT; the per-client codec encoder fans it out. */
void serverSendServerMessage(ServerSim *sim, langid id, int argCount,
                             const char *const args[]) {
    /* Scratch buffer mirrors the wire encoding so we can reuse
     * packLocalizedPayload; only the bytes after PACKET_HEADER_SIZE+2
     * become the CTRL_CHAT body. */
    uint8_t scratch[PACKET_HEADER_SIZE + 2 + 3 + 4 * (1 + PLAYER_NAME_LEN - 1)];
    int pos = PACKET_HEADER_SIZE + 2;
    ControlEvent evt;

    if (!packLocalizedPayload(scratch, &pos, sizeof(scratch), id, argCount, args)) {
        fprintf(stderr,
                "[UDP SERVER] serverSendServerMessage: pack failed id=%u argc=%d\n",
                (unsigned)id, argCount);
        return;
    }

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 0xFF; /* server localized */
    evt.u.chat.destPlayer = 0xFF; /* broadcast */
    evt.u.chat.bodyLen = (uint16_t)(pos - (PACKET_HEADER_SIZE + 2));
    if (evt.u.chat.bodyLen > 0) {
        memcpy(evt.u.chat.body, scratch + PACKET_HEADER_SIZE + 2,
               evt.u.chat.bodyLen);
    }
    serverSimPublishControl(sim, &evt);
}

/* Send a raw English server-originated message to all connected clients.
 * Uses fromPlayer=0xFE to mark the legacy English variant — used by
 * server-ops broadcasts (admin "say", lock toggle, ping enforcement)
 * that don't yet have dedicated langids.  As individual messages are
 * localized they should migrate to serverSendServerMessage above. */
void serverSendServerEnglishBroadcast(ServerSim *sim, const char *message) {
    ControlEvent evt;
    size_t maxChars = sizeof(evt.u.serverText.text) - 1; /* PACKET_MAX_CHAT_MESSAGE */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    if (SDL_strlen(message) <= maxChars) {
        SDL_strlcpy(evt.u.serverText.text, message, sizeof(evt.u.serverText.text));
    } else {
        /* CTRL_SERVER_TEXT / PACKET_CHAT_BROADCAST cap the wire payload at
         * PACKET_MAX_CHAT_MESSAGE. Rather than let SDL_strlcpy lop the tail
         * mid-character (a long winners list overflows the cap), cut on a
         * UTF-8 boundary and append an ellipsis so the overflow reads as an
         * intentional truncation. */
        size_t cut = maxChars - 3; /* leave room for "..." */
        while (cut > 0 && ((unsigned char)message[cut] & 0xC0) == 0x80) {
            cut--; /* back up so a multi-byte sequence isn't split */
        }
        SDL_memcpy(evt.u.serverText.text, message, cut);
        SDL_strlcpy(evt.u.serverText.text + cut, "...",
                    sizeof(evt.u.serverText.text) - cut);
    }
    /* In-process subscribers (SP host bots + human) consume CTRL_SERVER_TEXT
     * directly; UDP clients receive the encoder-emitted
     * PACKET_CHAT_BROADCAST(fromPlayer=0xFE) via the codec. */
    serverSimPublishControl(sim, &evt);
}

void transportUdpServerKickPlayer(ServerSim *sim, const char *playerName) {
    int i;
    char msg[128];

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        if (playerNameCompare(udpServer.clients[i].playerName, playerName) == 0) {
            const char *kickArgs[1];
            snprintf(msg, sizeof(msg), "%s has been server kicked.",
                     udpServer.clients[i].playerName);
            WB_LOG_WARN(WB_LOG_CAT_NET, "admin kick slot=%d name='%s'",
                        i, udpServer.clients[i].playerName);
            fprintf(stderr, "[UDP SERVER] %s\n", msg);
            serverSimConsoleMessage(msg);
            /* Send kick message to all clients (including the kicked player) */
            kickArgs[0] = udpServer.clients[i].playerName;
            serverSendServerMessage(sim, STR_KICK_ANNOUNCE, 1, kickArgs);
            /* Hand the kicked client an immediate disconnect notification so
             * they don't sit waiting for the keepalive timeout. PACKET_KICKED
             * transitions the client's joinState to UDP_CLIENT_KICKED, which
             * surfaces a "you were kicked" dialog in the lobby. */
            {
                uint8_t kbuf[PACKET_HEADER_SIZE];
                packHeader(kbuf, PACKET_KICKED, 0);
                srvSendTo(kbuf, sizeof(kbuf), &udpServer.clients[i].addr);
            }
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (serverSimIsLobbyEnabled(sim) &&
                (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown)) {
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
            return;
        }
    }

    /* No human matched. Bots have no UDP client, so the connected check
     * above skipped them — but their display name is stored in the same
     * playerName slot via transportUdpServerSetBotName, so match on that
     * and fall back to bot removal. serverSimRemoveBot runs the same
     * teardown the crash-streak kick uses (botManagerRemoveBot ->
     * serverSimRemovePlayer, the human-leave path) and publishes the freed
     * lobby slot. It is safe mid-game: the caller already holds the sim
     * mutex, exactly as a human kick does. There is no client to hand a
     * PACKET_KICKED, so that step is simply absent. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsBot(sim, (BYTE)i)) continue;
        if (playerNameCompare(udpServer.clients[i].playerName, playerName) == 0) {
            const char *kickArgs[1];
            snprintf(msg, sizeof(msg), "%s has been server kicked.",
                     udpServer.clients[i].playerName);
            WB_LOG_WARN(WB_LOG_CAT_NET, "admin kick bot slot=%d name='%s'",
                        i, udpServer.clients[i].playerName);
            fprintf(stderr, "[UDP SERVER] %s\n", msg);
            serverSimConsoleMessage(msg);
            kickArgs[0] = udpServer.clients[i].playerName;
            serverSendServerMessage(sim, STR_KICK_ANNOUNCE, 1, kickArgs);
            serverSimRemoveBot(sim, (BYTE)i);
            return;
        }
    }

    serverSimConsoleMessage("Player not found.");
}

void transportUdpServerDisconnectAll(ServerSim *sim) {
    int i;
    bool anyConnected = false;

    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            anyConnected = true;
            break;
        }
    }

    /* Tell every connected client the round is over before tearing their
     * slots down, so they return to the server browser cleanly instead of
     * waiting out the keepalive timeout. The per-client codec subscriber
     * unicasts PACKET_SERVER_SHUTDOWN during this publish (same mechanism
     * transportUdpServerDestroy uses). */
    if (anyConnected) {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_SERVER_SHUTDOWN;
        serverSimPublishControl(sim, &evt);
    }

    /* Boot every human client. serverDisconnectClient handles the
     * transport-side teardown for real UDP clients; serverSimRemovePlayer then
     * clears the sim-side player (gated on the sim's own connected flag, which
     * serverDisconnectClient leaves set). Bots are deliberately kept: they are
     * server configuration, not joined players, so they persist across a map
     * rotation exactly as they do across a normal lobby round (the caller
     * re-arms them for the new round via botManagerOnGameStart). A bot has no
     * udpServer.clients entry, so it is skipped by the connected check; the
     * removal is additionally gated on !serverSimIsBot so it survives. Removal
     * is index-based and doesn't compact the arrays, so a plain forward loop is
     * safe. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
        }
        if (serverSimIsPlayerConnected(sim, (BYTE)i) &&
            !serverSimIsBot(sim, (BYTE)i)) {
            serverSimRemovePlayer(sim, (BYTE)i);
        }
    }
}

bool transportUdpServerSetHostByName(ServerSim *sim, const char *playerName) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        if (serverSimIsBot(sim, (BYTE)i)) continue;
        if (playerNameCompare(udpServer.clients[i].playerName, playerName) == 0) {
            serverSimSetHostSlot(sim, (BYTE)i);
            return true;
        }
    }
    return false;
}

void transportUdpServerEnforcePing(ServerSim *sim) {
    int i;
    if (serverSimGetState(sim) != serverStateRunning) return;

    for (i = 0; i < MAX_TANKS; i++) {
        UdpServerClient *client = &udpServer.clients[i];
        uint16_t ping;
        if (!client->connected) continue;

        /* Already queued for teardown by an earlier strike — the slot stays
         * connected until the drain runs, so skip it rather than re-striking
         * (and re-broadcasting) it on the intervening half-steps. */
        if (udpServer.pendingSimRemove[i]) continue;

        ping = client->pingMs;
        if (ping == 0) continue;  /* No measurement yet */
        if (ping == client->lastEnforcedPingMs) continue;  /* Same measurement, already checked */
        client->lastEnforcedPingMs = ping;

        /* Kick threshold */
        if (ping >= PING_KICK_THRESHOLD_MS) {
            client->pingKickStrikes++;
            if (client->pingKickStrikes >= PING_KICK_COUNT) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "%s kicked for high ping (%dms).",
                         client->playerName, ping);
                WB_LOG_WARN(WB_LOG_CAT_NET,
                    "ping-kick slot=%d name='%s' ping=%ums strikes=%d/%d threshold=%d",
                    i, client->playerName, (unsigned)ping,
                    (int)client->pingKickStrikes, (int)PING_KICK_COUNT,
                    (int)PING_KICK_THRESHOLD_MS);
                fprintf(stderr, "[UDP SERVER] %s\n", msg);
                serverSimConsoleMessage(msg);
                serverSendServerEnglishBroadcast(sim, msg);
                /* Teardown is DEFERRED, not run here. This function is called
                 * from simRunHalfStep, mid-sim-frame; serverSimRemovePlayer
                 * frees the slot's tank and lgm objects, and the half-step's
                 * world-update stage holds pointers into both. Freeing here
                 * left shellsUpdate dereferencing a NULLed lgm slot (fault at
                 * lgmObj::playerNum, offset 0x18) and walking freed tank
                 * pointers. Flag the slot instead and let
                 * transportUdpServerDrainPendingRemovals do the teardown at
                 * the top of the next tick — the same safe point the
                 * control-queue-overflow disconnect uses. The kick is
                 * announced now; only the free is delayed by one tick. */
                udpServer.pendingSimRemove[i] = true;
                continue;
            }
        } else {
            client->pingKickStrikes = 0;
        }

        /* Warn threshold */
        if (ping >= PING_WARN_THRESHOLD_MS) {
            client->pingWarnStrikes++;
            if (client->pingWarnStrikes >= PING_WARN_COUNT && !client->pingWarned) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "%s has high ping (%dms) and may be kicked.",
                         client->playerName, ping);
                serverSendServerEnglishBroadcast(sim, msg);
                client->pingWarned = true;
            }
        } else {
            client->pingWarnStrikes = 0;
            client->pingWarned = false;
        }
    }
}

bool transportUdpServerCreate(unsigned short port,
                              const char *addrToUse,
                              ServerSim *sim,
                              const char *password) {
    struct sockaddr_in bindAddr;
    int i;

    (void)sim; /* Used later during tick */

    bolo_net_init();
    memset(&udpServer, 0, sizeof(udpServer));
    memset(punchQueue, 0, sizeof(punchQueue));

    udpServer.sock = createUdpSocket(true);
    if (udpServer.sock == INVALID_SOCKET) {
        return false;
    }

    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    if (addrToUse != NULL && addrToUse[0] != '\0') {
        bindAddr.sin_addr.s_addr = inet_addr(addrToUse);
    }
    bindAddr.sin_port = htons(port);

    if (bind(udpServer.sock, (struct sockaddr *)&bindAddr,
             sizeof(bindAddr)) == SOCKET_ERROR) {
        WB_LOG_ERROR(WB_LOG_CAT_NET, "bind() failed on port %u", port);
        fprintf(stderr, "[UDP SERVER] bind() failed on port %u\n", port);
        closesocket(udpServer.sock);
        udpServer.sock = INVALID_SOCKET;
        return false;
    }

    serverSimSetPassword(sim, password,
                         password != NULL ? strlen(password) : 0);

    /* The spectator roster lives here in the transport layer; register the
     * enumerator so the sim's sync-replay (and the ring keyframe control
     * snapshot) can carry one CTRL_SPECTATOR_SLOT per connected viewer. */
    serverSimSetSpectatorRosterEnumerator(sim, serverEnumSpectatorRoster, NULL);

    udpServer.running = true;
    udpServer.tickCount = 0;
    udpServer.uploadMaxFiles        = 64;
    udpServer.uploadMaxStorageBytes = 8u * 1024u * 1024u;
    serverSimSetServerPort(sim, port);
    WB_LOG_INFO(WB_LOG_CAT_NET,
        "server created: port=%u bindAddr=%s maxPlayers=%u password=%s",
        port,
        (addrToUse && *addrToUse) ? addrToUse : "0.0.0.0",
        (unsigned)serverSimGetMaxPlayers(sim),
        (password && *password) ? "yes" : "no");
    udpServer.compressedMapSize = 0;

    for (i = 0; i < MAX_TANKS; i++) {
        udpServer.clients[i].connected = false;
        udpServer.clients[i].nameStickySuffix = false;
        udpServer.clients[i].claimPending = false;
        udpServer.clients[i].claimDesiredName[0] = '\0';
        udpServer.clients[i].inboundCmdSeq = 0;
        udpServer.clients[i].controlSub = SUBSCRIBER_HANDLE_INVALID;
        memset(&udpServer.mapDownload[i], 0, sizeof(ClientMapDownload));
        udpServer.controlSyncInProgress[i] = false;
        /* No slot is this transport's until someone joins it. The sim can
         * outlive an earlier server on the same process, so start from a
         * clean mask rather than whatever that server left behind. */
        serverSimSetShadowCulled(sim, (BYTE)i, false);
    }

    netImpairInit(&srvImpairIn);
    netImpairInit(&srvImpairOut);

    /* Start dedicated recv thread */
    udpServerRecvThreadStart(udpServer.sock);

    return true;
}

void transportUdpServerSetUploadConfig(UploadPolicy policy,
                                       uint8_t maxFiles,
                                       uint32_t maxStorageBytes,
                                       const char *persistDir) {
    udpServer.uploadPolicy = policy;
    if (maxFiles != 0) {
        udpServer.uploadMaxFiles = maxFiles;
    }
    if (maxStorageBytes != 0) {
        udpServer.uploadMaxStorageBytes = maxStorageBytes;
    }
    if (persistDir != NULL) {
        SDL_strlcpy(udpServer.uploadPersistDir, persistDir,
                    sizeof(udpServer.uploadPersistDir));
    } else {
        udpServer.uploadPersistDir[0] = '\0';
    }
}

void transportUdpServerDestroy(void) {
    int i;

    WB_LOG_INFO(WB_LOG_CAT_NET, "server destroy: tickCount=%u dropCount=%u",
                (unsigned)udpServer.tickCount, (unsigned)udpServerRecvDropCount());

    /* Stop recv thread before touching the socket */
    udpServerRecvThreadStop();

    /* Publish first so the per-client subscriber encodes and unicasts
     * PACKET_SERVER_SHUTDOWN while the socket is still open, then close. */
    if (udpServer.sock != INVALID_SOCKET) {
        {
            ControlEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_SERVER_SHUTDOWN;
            serverSimPublishControl(serverSimGetActive(), &evt);
        }
        closesocket(udpServer.sock);
        udpServer.sock = INVALID_SOCKET;
    }
    {
        ServerSim *activeSim = serverSimGetActive();
        for (i = 0; i < MAX_TANKS; i++) {
            if (udpServer.clients[i].connected) {
                serverSimUnregisterSubscriber(activeSim,
                                              udpServer.clients[i].controlSub);
                udpServer.clients[i].controlSub = SUBSCRIBER_HANDLE_INVALID;
            }
            udpServer.clients[i].connected = false;
            udpServer.clients[i].nameStickySuffix = false;
            udpServer.clients[i].claimPending = false;
            udpServer.clients[i].claimDesiredName[0] = '\0';
            /* The sim can be ticked on without this transport (a host that
             * drops back to single player), so give every copy back to the
             * tick as the server goes down. */
            serverSimSetShadowCulled(activeSim, (BYTE)i, false);
            serverCleanupMapDownload(i);
        }
    }
    udpServer.running = false;

    udpServerPublicIp[0] = '\0';
    udpServerPublicPort  = 0;
    memset(punchQueue, 0, sizeof(punchQueue));
}

/* Fill an INFO_PACKET from the current sim state. The info-request reply
 * and the tracker update both advertise the same server, so they share
 * this builder and differ only in where the finished packet is sent.
 * Layout and field semantics are documented in docs/info_packet_wire.md. */
void buildInfoPacket(ServerSim *sim, INFO_PACKET *pkt) {
    GameSim *gs = serverSimGetGameSim(sim);
    int i;
    BYTE numPlayers = 0, numHumans = 0, numBots = 0;

    memset(pkt, 0, sizeof(*pkt));

    /* Header */
    memcpy(pkt->h.signature, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE);
    pkt->h.versionMajor = BOLO_VERSION_MAJOR;
    pkt->h.versionMinor = BOLO_VERSION_MINOR;
    pkt->h.versionRevision = BOLO_VERSION_REVISION;
    pkt->h.type = BOLOPACKET_INFORESPONSE;

    /* Game ID — address zeroed (browser uses UDP source), port and timestamp set.
     * Tracker reads port raw for v1.1.8 (only ntohs for v1.1.1-3).
     * start_time is the only field the tracker byte-swaps on read. */
    if (udpServerPublicPort != 0) {
        pkt->gameid.serveraddress.s_addr = inet_addr(udpServerPublicIp);
        pkt->gameid.serverport = udpServerPublicPort;
    } else {
        pkt->gameid.serveraddress.s_addr = 0;
        pkt->gameid.serverport = serverSimGetServerPort(sim);
    }
    pkt->gameid.start_time = htonl(serverSimGetTimeCreated(sim));

    /* Map name as Pascal string */
    utilCtoPString((char *)serverSimGetMapName(sim), pkt->mapname);

    /* Game settings */
    pkt->gametype = (BYTE)gs->game;
    pkt->allow_mines = gs->hiddenMines ? HIDDEN_MINES : ALL_MINES_VISIBLE;
    pkt->allow_AI = 0;  /* AI type not tracked in new sim — report as none */
    {
        BYTE flags = 0;
        if (serverSimIsAcceptingJoins(sim))              flags |= INFO_FLAG_ALLOW_NEW_PLAYERS;
        if (transportUdpServerGetLock() || !serverSimIsAcceptingJoins(sim)) flags |= INFO_FLAG_LOCKED;
        if (serverSimGetRanked(sim))                     flags |= INFO_FLAG_RANKED;
        if (serverSimIsRandomMapEnabled(sim))            flags |= INFO_FLAG_RANDOM_MAP;
        if (serverSimGetState(sim) == serverStateLobby)  flags |= INFO_FLAG_IN_LOBBY;
        /* Advertise spectator support so finders can enable a Spectate action;
         * the cap accessor returns 0 when spectating is disabled. The live
         * spectator_count has no accessor yet, so it stays 0 below. */
        if (serverSimGetMaxSpectators(sim) > 0)          flags |= INFO_FLAG_ALLOW_SPECTATORS;
        /* The voice mode is two bits rather than a flag, in the top of the
         * same byte. serverVoiceOn packs as zero. */
        flags |= infoPacketPackVoiceMode(serverSimGetVoiceMode(sim));
        pkt->flags = flags;
    }
    pkt->start_delay = serverSimGetStartDelay(sim);
    pkt->time_limit = serverSimGetGameLength(sim);

    /* Count connected players, classifying humans vs bots */
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(sim, i)) {
            numPlayers++;
            if (serverSimIsBot(sim, (BYTE)i)) numBots++;
            else                              numHumans++;
        }
    }
    pkt->num_players = numPlayers;
    pkt->num_humans  = numHumans;
    pkt->num_bots    = numBots;
    pkt->max_players = serverSimGetMaxPlayers(sim);

    /* Neutral pills and bases */
    pkt->free_pills = pillsGetNumNeutral(&gs->pb);
    pkt->free_bases = basesGetNumNeutral(&gs->bs);

    pkt->has_password = serverSimGetPassword(sim)[0] != '\0' ? 1 : 0;
    pkt->spectator_count = 0;

    {
        const char *md5Hex = serverSimGetMapMd5Hex(sim);
        if (md5Hex[0] != '\0' && !serverSimIsRandomMapEnabled(sim)) {
            memcpy(pkt->map_md5, md5Hex, 32);
        }
    }

    pkt->view_policies = infoPacketPackViewPolicies(
        serverSimGetViewPolicy(sim, viewCategoryPill),
        serverSimGetViewPolicy(sim, viewCategoryBase),
        serverSimGetViewPolicy(sim, viewCategoryAlly),
        serverSimGetClassicMode(sim),
        serverSimGetAlliesInTrees(sim));
}

/* Handle an old-protocol info request (server browser compatibility).
 * Replies to the requester with the current server advertisement. */
static void serverHandleInfoRequest(const struct sockaddr_in *fromAddr,
                                    ServerSim *sim) {
    INFO_PACKET pkt;
    char consoleMsg[256];

    buildInfoPacket(sim, &pkt);

    /* wire-only: tracker / external reply (no in-process audience) */
    srvSendTo((uint8_t *)&pkt, sizeof(pkt), fromAddr);

    {
        struct in_addr addrCopy = fromAddr->sin_addr;
        snprintf(consoleMsg, sizeof(consoleMsg), "Info packet request from %s",
                 inet_ntoa(addrCopy));
    }
    serverSimConsoleMessage(consoleMsg);
}

/* Check if a packet is an old-protocol info request.
 * Gate is magic + length + type only — the info-request is the universal
 * version-negotiation primitive, so a v1.0 client asking a v2.0 server
 * (or vice versa) must receive an INFO_RESPONSE carrying the server's
 * own version triple.  Mismatched-version joiners then see a localized
 * pre-flight error rather than a silent JOIN_REQUEST length-gate drop.
 * The version bytes inside the request body are still parsed elsewhere
 * for logging but no longer gate the response. */
static bool isOldProtocolInfoRequest(const uint8_t *buf, int len) {
    return len == BOLOPACKET_REQUEST_SIZE &&
           memcmp(buf, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE) == 0 &&
           buf[BOLOPACKET_REQUEST_TYPEPOS] == BOLOPACKET_INFOREQUEST;
}

uint32_t transportUdpServerGetTickCount(void) {
    return udpServer.tickCount;
}

bool transportUdpServerHasAnyClient(void) {
    int i;
    if (!udpServer.running) return false;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) return true;
    }
    return false;
}

void transportUdpServerOnGameStart(ServerSim *sim) {
    int i;
    mpDiagLog("[srv] GAME_START BEGIN (rebasing game/map channel send baselines)");
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            mpDiagLog("[srv] GAME_START wipe slot=%d pre mapEv(ack=%u next=%u)",
                      i,
                      (unsigned)udpServer.mapEventQueues[i].ackedSeq,
                      (unsigned)udpServer.mapEventQueues[i].nextSeq);
        }
        if (udpServer.clients[i].connected) {
            /* Ensure map download is considered complete so snapshots
             * are sent during the game even if a final chunk ack was lost. */
            udpServer.mapDownload[i].downloadComplete = TRUE;
            /* This force-completes the download without serverInitMapDownload,
             * so clear any in-flight resync explicitly — otherwise the send
             * gate above would keep holding this slot's map events forever.
             * Also disarm any pending bulk transfer so a stale arming can't
             * re-fire a completion against the new game's channel state. */
            udpServer.mapDownload[i].resyncInProgress = FALSE;
            udpServer.mapDownload[i].resyncGen = 0;
            udpServer.mapDownload[i].xferKind = MAP_XFER_NONE;
            udpServer.mapDownload[i].xferBegun = false;
        }
        /* Drop the previous game's unacked reliable events on the map queue,
         * but keep the sequence counter monotonic — never reuse low seq
         * numbers.  Set ackedSeq = nextSeq (queue now empty) and memset the
         * buffer (clears stale delivered bodies so they can't be resent), but
         * do NOT reset nextSeq.
         *
         * This is the fix for the lobby→running seq-reuse desync: a stale
         * in-flight lobby event delayed past game start now carries seq
         * numbers BELOW the client's continuing ack, so it dedups harmlessly
         * instead of being mistaken for fresh running-space events (which is
         * what happened when the queue restarted at seq 1 and old high-seq
         * lobby events looked newer than the new low-seq running events).
         * The reliable game/map channels get the same forward truncation via
         * the per-client CHANNEL_RESET below. */
        udpServer.mapEventQueues[i].ackedSeq = udpServer.mapEventQueues[i].nextSeq;
        memset(udpServer.mapEventQueues[i].buffer, 0,
               sizeof(udpServer.mapEventQueues[i].buffer));

        /* Drop this client's previous-game send tail on the reliable game
         * (channel 0) and map (channel 1) channels and tell it the new
         * baselines so its receive side lifts past any in-flight straggler.
         * channelResetSend collapses each channel's unacked window and returns
         * the post-reset sequence floor (its nextSeq); the per-client
         * CTRL_CHANNEL_RESET carries those two floors. Both ride the in-order
         * control channel (channel 2) ahead of the CTRL_GAME_PHASE_RUNNING the
         * caller publishes immediately after this returns, so the client
         * applies the baseline lift before the running flip and a previous-game
         * game/map event left in flight dedup-drops in the new game. The
         * baselines are this client's own channel state — the control encoder
         * stays recipient-agnostic, so the per-client value lives in the event,
         * not the encoder. A full control window defers the disconnect off this
         * path (mirrors the deliver-callback overflow). */
        if (udpServer.clients[i].connected) {
            uint32_t b0 = channelResetSend(&udpServer.channelMux[i], CHANNEL_GAME);
            uint32_t b1 = channelResetSend(&udpServer.channelMux[i], CHANNEL_MAP);
            ControlEvent resetEvt;
            ControlEncodeBodyFn enc =
                transportControlCodecBodyEncoder(CTRL_CHANNEL_RESET);
            uint8_t msg[CHANNEL_CONTROL_SEG];
            size_t bodyLen = 0;
            memset(&resetEvt, 0, sizeof(resetEvt));
            resetEvt.type = CTRL_CHANNEL_RESET;
            resetEvt.u.channelReset.channelMask =
                (uint8_t)((1u << CHANNEL_GAME) | (1u << CHANNEL_MAP));
            resetEvt.u.channelReset.ch0Baseline = b0;
            resetEvt.u.channelReset.ch1Baseline = b1;
            if (enc != NULL &&
                enc(&resetEvt, &udpServer.clients[i], msg + 3,
                    sizeof(msg) - 3, &bodyLen) == ENCODE_OK) {
                msg[0] = (uint8_t)CTRL_CHANNEL_RESET;
                packU16(msg + 1, (uint16_t)bodyLen);
                if (!channelSend(&udpServer.channelMux[i], CHANNEL_CONTROL,
                                 msg, (uint16_t)(3 + bodyLen))) {
                    if (!udpServer.pendingSimRemove[i]) {
                        WB_LOG_ERROR(WB_LOG_CAT_NET,
                                     "control channel overflow sending baseline "
                                     "reset for slot %d, deferring disconnect", i);
                        udpServer.pendingSimRemove[i] = true;
                    }
                }
            }
        }

        /* New round, fresh round-log request budget for every slot. */
        udpServerResetRoundLogLimits(i);
        udpServerResetMapReaskLimit(i);

        /* A round-log transfer is a lobby/game-over affair and must not bleed
         * into the round starting now: CHANNEL_BULK is deliberately not
         * re-based above, so an unfinished one would keep streaming into the
         * new game's map downloads. Abort it with the same triple the map
         * change uses — drop the staged blob, collapse the send window, and
         * carry the new bulk baseline so the client abandons its partial.
         * Gating on the sender's kind is what leaves every other in-flight
         * transfer (a join download, a resync) undisturbed. */
        if (udpServer.clients[i].connected &&
            bulkSenderBusy(&udpServer.bulkSend[i]) &&
            udpServer.bulkSend[i].kind == BULK_KIND_ROUND_LOG) {
            uint32_t b3;
            ControlEvent resetEvt;
            ControlEncodeBodyFn enc =
                transportControlCodecBodyEncoder(CTRL_CHANNEL_RESET);
            uint8_t msg[CHANNEL_CONTROL_SEG];
            size_t bodyLen = 0;
            bulkSenderReset(&udpServer.bulkSend[i]);
            b3 = channelResetSend(&udpServer.channelMux[i], CHANNEL_BULK);
            memset(&resetEvt, 0, sizeof(resetEvt));
            resetEvt.type = CTRL_CHANNEL_RESET;
            resetEvt.u.channelReset.channelMask = (uint8_t)(1u << CHANNEL_BULK);
            resetEvt.u.channelReset.ch3Baseline = b3;
            if (enc != NULL &&
                enc(&resetEvt, &udpServer.clients[i], msg + 3,
                    sizeof(msg) - 3, &bodyLen) == ENCODE_OK) {
                msg[0] = (uint8_t)CTRL_CHANNEL_RESET;
                packU16(msg + 1, (uint16_t)bodyLen);
                if (!channelSend(&udpServer.channelMux[i], CHANNEL_CONTROL,
                                 msg, (uint16_t)(3 + bodyLen))) {
                    if (!udpServer.pendingSimRemove[i]) {
                        WB_LOG_ERROR(WB_LOG_CAT_NET,
                                     "control channel overflow sending bulk reset "
                                     "for slot %d, deferring disconnect", i);
                        udpServer.pendingSimRemove[i] = true;
                    }
                }
            }
        }
    }
    WB_LOG_INFO(WB_LOG_CAT_NET, "ctrl queue reset all slots (game start)");

    /* Cut every live-lobby spectator over to the delayed ring as the game
     * starts. Drop its control-bus subscription so the imminent
     * CTRL_GAME_PHASE_RUNNING publish (and the forced snapshot after it)
     * cannot reach it — a live spectator must see zero running-state state,
     * or the configured spectator delay is undercut. This runs before the
     * RUNNING publish on both start paths (the lifecycle countdown→running
     * step and the in-place start), so the unsubscribe is the anti-cheat
     * boundary. No channel reset: a live spectator never ran the delayed
     * seed/feed block, so its seed/feed fields are still at their accept-time
     * zeros and the next serverServiceSpectators tick finds head - delay still
     * inside the pre-game lobby (below gameStartSeq) → arms the "spectating
     * begins in X" countdown cleanly until the delayed view reaches the game. */
    for (i = 0; i < MAX_SPECTATORS; i++) {
        SpectatorConn *sp = &udpServer.spectators[i];
        if (!sp->connected || !sp->live) continue;
        serverSimUnregisterSubscriber(sim, sp->controlSub);
        sp->controlSub = SUBSCRIBER_HANDLE_INVALID;
        sp->live = false;
        mpDiagLog("[srv] GAME_START spec idx=%d live->delayed (unsubscribed)", i);
    }

    mpDiagLog("[srv] GAME_START END (game/map channel send baselines rebased)");
}

void transportUdpServerOnLobbyMapChange(ServerSim *sim) {
    int i;
    int mapLen;
    uint8_t notifyBuf[PACKET_HEADER_SIZE];
    /* Compress into a local oversized scratch buffer first — the map
     * RLE encoder has no internal output-bound check, and an
     * incompressible map can encode slightly larger than its 64KB
     * input. Validate the result fits the wire size before copying. */
    BYTE scratchMap[MAP_COMPRESSED_MAX_SIZE];

    mapLen = serverSimGetCompressedMap(sim, scratchMap, (int)sizeof(scratchMap));
    if (mapLen <= 0) {
        fprintf(stderr, "[UDP SERVER] Map change: failed to compress new map\n");
        return;
    }
    if (mapLen > (int)MAP_DOWNLOAD_MAX_SIZE) {
        fprintf(stderr,
                "[UDP SERVER] Map change: compressed map (%d bytes) exceeds "
                "MAP_DOWNLOAD_MAX_SIZE (%d); aborting broadcast\n",
                mapLen, (int)MAP_DOWNLOAD_MAX_SIZE);
        return;
    }
    memcpy(udpServer.compressedMap, scratchMap, (size_t)mapLen);
    udpServer.compressedMapSize = (uint32_t)mapLen;

    packHeader(notifyBuf, PACKET_LOBBY_MAP_CHANGE, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        /* Send PACKET_LOBBY_MAP_CHANGE so the client flushes its
         * stale map state before the new chunk stream lands. */
        srvSendTo(notifyBuf, sizeof(notifyBuf),
                  &udpServer.clients[i].addr);
        /* Re-send JOIN_ACCEPT so the client picks up the new compressed
         * map size. */
        serverSendJoinAccept(i, sim, &udpServer.clients[i].addr);
        /* Re-base CHANNEL_BULK and arm a fresh join download from the new
         * blob (re-gates snapshots). It streams once the client's
         * PACKET_MAP_DL_READY confirms its buffers were re-armed for the new
         * size — the accept above (or the one re-sent for the client's forced
         * re-JOIN) triggers that. */
        serverRebaseBulkAndRearmDownload(i);
    }

    /* Live-lobby spectators: mirror the player loop — re-send the accept (new map
     * size) and re-arm the lobby-map download, which resets the spectator's bulk
     * sender + re-bases its CHANNEL_BULK (via CTRL_CHANNEL_RESET) so it abandons
     * the old partial and downloads the new map. Delayed (in-game) viewers are
     * skipped: their map rides the ring seed, not this path. */
    for (i = 0; i < MAX_SPECTATORS; i++) {
        SpectatorConn *sp = &udpServer.spectators[i];
        if (!sp->connected || !sp->live) continue;
        serverSendSpectatorAccept(i, sim, &sp->addr);
        serverArmSpectatorLobbyMap(i, /*resetChannel=*/true);
    }

    fprintf(stderr, "[UDP SERVER] Map change prep: %u bytes compressed map\n",
            udpServer.compressedMapSize);
}

void transportUdpServerSetBotName(BYTE playerNum, const char *name) {
    if (playerNum >= MAX_TANKS) return;
    strncpy(udpServer.clients[playerNum].playerName, name,
            PACKET_MAX_PLAYER_NAME - 1);
    udpServer.clients[playerNum].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
}

bool transportUdpServerStartBalanceRequest(ServerSim *sim,
                                           uint8_t teamSize,
                                           bool includeBots) {
    BalanceThreadData *btd = malloc(sizeof(BalanceThreadData));
    if (!btd) {
        return false;
    }
    SDL_Thread *t;
    int i;
    memset(btd, 0, sizeof(*btd));
    btd->sim = sim;
    btd->teamSize = teamSize;
    btd->includeBots = includeBots;
    btd->totalPlayers = 0;
    btd->numBotSlots = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsPlayerConnected(sim, i)) continue;
        btd->totalPlayers++;
        /* Include bot slots in the WBN request only when the caller
         * asked for "Bots included". When !includeBots the bots are
         * kicked at APPLY time and don't need to be skill-placed. */
        if (btd->includeBots && serverSimIsBot(sim, (BYTE)i)) {
            btd->botSlots[btd->numBotSlots++] = (uint8_t)i;
        }
    }
    serverSimSetBalanceRequestInFlight(sim, true);
    t = SDL_CreateThread(balanceThreadFunc, "WbnBalance", btd);
    if (!t) {
        serverSimSetBalanceRequestInFlight(sim, false);
        free(btd);
        serverSimConsoleMessage("Failed to start balance thread");
        return false;
    }
    SDL_DetachThread(t);
    return true;
}

/* Resolve a WEB (CLIENT_TYPE_WEB) slot's WBN identity from its join_code.
 * Verifies once per connection via the read-only join-code route and caches
 * the result on the slot; later reauths re-stamp from cache with no network
 * call (the code expires at TTL and the server_key rotates between rounds, so
 * a re-verify would fail).  Returns true iff the slot holds a logged-in WBN
 * identity.  WEB joiners are never Steam/supporter-bearing. */
static bool udpServerResolveWebIdentity(BYTE slot, const char *joinCode,
                                        bool *isLoggedInOut, bool *hasSteamOut,
                                        bool *isSupporterOut) {
    if (hasSteamOut)    *hasSteamOut = FALSE;
    if (isSupporterOut) *isSupporterOut = FALSE;
    if (!udpServer.clients[slot].wbnWebIdentityCached) {
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        char countryBuf[3];
        char errorMsg[512];
        bool isLoggedIn = FALSE;
        int  userId = -1;
        nameBuf[0] = '\0';
        countryBuf[0] = '\0';
        errorMsg[0] = '\0';
        if (!winboloNetVerifyJoinCode(joinCode, nameBuf, &isLoggedIn,
                                      countryBuf, &userId, errorMsg)) {
            /* Invalid/expired/wrong-server code: fall back to anonymous,
             * exactly like an empty wbnJoinKey.  Not cached, so a later
             * reauth with a still-valid code can still succeed. */
            if (isLoggedInOut) *isLoggedInOut = FALSE;
            return false;
        }
        udpServer.clients[slot].wbnWebIdentityCached = true;
        udpServer.clients[slot].wbnWebIsLoggedIn = isLoggedIn;
        snprintf(udpServer.clients[slot].wbnWebName,
                 PACKET_MAX_PLAYER_NAME, "%s", nameBuf);
        udpServer.clients[slot].wbnWebCountry[0] = countryBuf[0];
        udpServer.clients[slot].wbnWebCountry[1] = countryBuf[1];
        udpServer.clients[slot].wbnWebCountry[2] = '\0';
        udpServer.clients[slot].wbnWebUserId = userId;
    }
    if (isLoggedInOut) *isLoggedInOut = udpServer.clients[slot].wbnWebIsLoggedIn;
    return udpServer.clients[slot].wbnWebIsLoggedIn;
}

/* Stamp a logged-in WEB slot's verified identity onto the sim + transport slot
 * from its cached join-code result.  The WBN-resolved name and country are
 * AUTHORITATIVE: a web client presents only a join_code, and the verify call
 * (unlike the native player_key route) never sends a name for the backend to
 * bind against — so the server, not the client, decides the verified display
 * name.  Without this a valid code could be paired with any spoofed name under
 * PLAYER_FLAG_WBN_VERIFIED.  serverSimSetPlayerName / serverSimSetPlayerCountry
 * publish CTRL_PLAYER_NAME / PLAYER_JOIN so the change fans out to every client.
 * Caller must hold a cached, logged-in identity. */
static void udpServerApplyWebIdentity(ServerSim *sim, BYTE slot) {
    uint8_t flags = udpServer.clients[slot].clientHints & PLAYER_CLIENT_HINT_MASK;
    flags |= PLAYER_FLAG_WBN_VERIFIED;  /* WEB joiners carry no Steam/supporter */
    /* Carry the mic bits across the rebuild. The client sends them once when
     * they change, so anything dropped here is gone for the rest of the
     * connection rather than re-reported on the next tick. */
    flags |= (uint8_t)(playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs,
                                             slot) & PLAYER_VOICE_FLAG_MASK);
    playersSetClientFlags(&serverSimGetGameSim(sim)->plyrs, slot, flags);
    udpServer.clients[slot].wbnWasVerified = true;
    playersSetClientType(&serverSimGetGameSim(sim)->plyrs, slot,
                         udpServer.clients[slot].clientType);

    if (udpServer.clients[slot].wbnWebName[0] != '\0') {
        serverSimSetPlayerName(sim, slot, udpServer.clients[slot].wbnWebName);
        snprintf(udpServer.clients[slot].playerName, PACKET_MAX_PLAYER_NAME,
                 "%s", udpServer.clients[slot].wbnWebName);
        udpServer.clients[slot].nameStickySuffix = false;
    }
    if (udpServer.clients[slot].wbnWebCountry[0] != '\0') {
        serverSimSetPlayerCountry(sim, slot,
                                  udpServer.clients[slot].wbnWebCountry);
        udpServer.clients[slot].countryCode[0] =
            udpServer.clients[slot].wbnWebCountry[0];
        udpServer.clients[slot].countryCode[1] =
            udpServer.clients[slot].wbnWebCountry[1];
        udpServer.clients[slot].countryCode[2] = '\0';
    }
}

void transportUdpServerHandleWbnReauth(ServerSim *sim, BYTE slot,
                                       const char *token) {
    if (!winbolonetIsRunning() || token == NULL || token[0] == '\0') {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[WBN] re-auth for slot %d ignored: running=%d tokenLen=%d",
                    (int)slot, winbolonetIsRunning() ? 1 : 0,
                    token ? (int)strlen(token) : -1);
        return;
    }
    char errorMsg[512];
    bool hasSteam = FALSE;
    bool wbnIsSupporter = FALSE;
    /* Capture the slot's keyed state *before* the verify fills the key,
     * so the deferred-join core can tell a fresh registration (key
     * absent->present) from an idempotent rekey resend. */
    bool wasParticipant = winboloNetIsPlayerParticipant(slot);
    /* For a pending provisional claim the slot's display name is the temp
     * -unverified[-N] handed out at join; WBN must verify and attribute
     * under the real account display name, which is the stored desired bare
     * name.  Non-claim reauths verify under the slot's own name as before. */
    bool isWeb = (udpServer.clients[slot].clientType == CLIENT_TYPE_WEB);
    /* Web slots take their verified name from WBN, not the wire, so the native
     * provisional-claim dance (temp -unverified[-N] names, squatter preemption)
     * does not apply to them. */
    bool isPendingClaim = !isWeb && udpServer.clients[slot].claimPending;
    const char *verifyName = isPendingClaim
        ? udpServer.clients[slot].claimDesiredName
        : udpServer.clients[slot].playerName;
    errorMsg[0] = '\0';
    bool verifyOk;
    if (isWeb) {
        /* WEB clients present a join_code (not a minted player_key); verify it
         * read-only and cache the identity for the connection's lifetime. */
        verifyOk = udpServerResolveWebIdentity(slot, token, NULL,
                                               &hasSteam, &wbnIsSupporter);
    } else {
        verifyOk = winboloNetVerifyClientKey(token, verifyName, slot, errorMsg,
                                             &hasSteam, &wbnIsSupporter);
    }
    if (verifyOk) {
        if (isWeb) {
            /* Web slot: stamp the WBN-authoritative identity (name, country,
             * flags) from the cached join-code result. */
            udpServerApplyWebIdentity(sim, slot);
        } else {
            /* Re-merge using the clientHints captured at JOIN_REQUEST (the
             * client doesn't re-send them on REAUTH; we re-verify against
             * WBN, not the network). */
            uint8_t storedHints = udpServer.clients[slot].clientHints;
            uint8_t flags = storedHints & PLAYER_CLIENT_HINT_MASK;
            flags |= PLAYER_FLAG_WBN_VERIFIED;
            if (hasSteam) flags |= PLAYER_FLAG_WBN_STEAM_LINKED;
            if (wbnIsSupporter) flags |= PLAYER_FLAG_SUPPORTER;
            /* Carry the mic bits across the rebuild. The client sends them
             * once when they change, so anything dropped here is gone for
             * the rest of the connection rather than re-reported on the
             * next tick. */
            flags |= (uint8_t)(playersGetClientFlags(
                                   &serverSimGetGameSim(sim)->plyrs, slot)
                               & PLAYER_VOICE_FLAG_MASK);
            playersSetClientFlags(&serverSimGetGameSim(sim)->plyrs, slot, flags);
            /* Keep the durable rekey-gate bit in step with the session flag. */
            udpServer.clients[slot].wbnWasVerified = true;
            playersSetClientType (&serverSimGetGameSim(sim)->plyrs, slot,
                                  udpServer.clients[slot].clientType);
        }
        WB_LOG_INFO(WB_LOG_CAT_NET,
                    "[WBN] Player %d re-authenticated (steam=%d)",
                    (int)slot, hasSteam ? 1 : 0);
        /* Fire the deferred PLAYER_JOIN now that the slot is keyed — in
         * every phase, not just running.  The edge guard emits exactly
         * once per session: a fresh join or a post-rotation re-register
         * (key was absent) emits; an idempotent rekey resend (already a
         * participant) does not.  This also satisfies the anonymous
         * fallback armed at join, so the grace sweep won't fire too. */
        if (wbnJoinOnReauth(&udpServer.clients[slot].wbnJoin, wasParticipant)) {
            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                               slot, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
        }
        if (serverSimGetState(sim) == serverStateLobby ||
            serverSimGetState(sim) == serverStateCountdown) {
            serverSimPublishLobbySlot(sim, slot);
        }

        /* Resolve a pending provisional claim: verify ran under the desired
         * bare name above, so attribution is correct; now reconcile the local
         * display.  Three outcomes by who holds the bare name now. */
        if (isPendingClaim) {
            const char *desired = udpServer.clients[slot].claimDesiredName;
            int s;
            int holder = -1;
            for (s = 0; s < MAX_TANKS; s++) {
                if (s == (int)slot) continue;
                if (!udpServer.clients[s].connected) continue;
                if (playerNameCompare(udpServer.clients[s].playerName,
                                      desired) == 0) {
                    holder = s;
                    break;
                }
            }

            bool holderIsVerified =
                holder >= 0 &&
                (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs,
                                       (BYTE)holder)
                 & PLAYER_FLAG_WBN_VERIFIED) != 0;

            switch (claimResolveDecide(holder >= 0, holderIsVerified)) {
            case CLAIM_RESOLVE_PROMOTE_FREE:
                /* Bare name free — the squatter left during grace.  Promote
                 * straight to the bare name. */
                serverSimSetPlayerName(sim, slot, desired);
                serverSimPublishLobbySlot(sim, slot);
                snprintf(udpServer.clients[slot].playerName,
                         PACKET_MAX_PLAYER_NAME, "%s", desired);
                udpServer.clients[slot].nameStickySuffix = false;
                break;
            case CLAIM_RESOLVE_PREEMPT_SQUATTER: {
                /* Unverified squatter still holds the bare name.  Rename it
                 * off first (it must vacate before the joiner claims), then
                 * promote this slot.  If the squatter's suffix pool is
                 * exhausted, keep this slot on its temp name. */
                char squatterName[PACKET_MAX_PLAYER_NAME];
                if (serverChooseUnverifiedSuffix(
                        udpServer.clients[holder].playerName, holder,
                        squatterName, sizeof(squatterName))) {
                    serverPreemptRename(sim, holder, squatterName,
                                        desired,
                                        udpServer.clients[slot].countryCode);
                    /* Plain set, NOT serverPreemptRename — that would emit a
                     * spurious "renamed by verified player" naming the joiner
                     * as its own victim. */
                    serverSimSetPlayerName(sim, slot, desired);
                    serverSimPublishLobbySlot(sim, slot);
                    snprintf(udpServer.clients[slot].playerName,
                             PACKET_MAX_PLAYER_NAME, "%s", desired);
                    udpServer.clients[slot].nameStickySuffix = false;
                }
                /* else: squatter pool exhausted — stay on the temp name. */
                break;
            }
            case CLAIM_RESOLVE_KEEP_TEMP:
                /* A verified slot won the bare name (a second reclaimer won
                 * the race); keep this slot on its temp name permanently.  WBN
                 * attribution is already correct since verify ran under the
                 * bare name; only the local display stays suffixed. */
                break;
            }

            /* Clear the claim in every outcome.  desired aliases the buffer,
             * so this clear must come after all uses of desired. */
            udpServer.clients[slot].claimPending = false;
            udpServer.clients[slot].claimDesiredName[0] = '\0';
        }
    } else if (isWeb && udpServer.clients[slot].wbnWebIdentityCached) {
        /* A web slot whose code verified but resolved to a guest (not logged
         * in) returns false here — that is the expected anonymous case, not a
         * failure, so log it at info and don't emit a scary warning. */
        WB_LOG_INFO(WB_LOG_CAT_NET,
                    "[WBN] Player %d web slot resolved as guest (anonymous)",
                    (int)slot);
    } else {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[WBN] Player %d re-auth failed: %s", (int)slot, errorMsg);
    }
}

const char *transportUdpServerGetPlayerName(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) {
        return NULL;
    }
    /* Bots have no UDP connection but their name was set via
     * transportUdpServerSetBotName; treat them as valid name owners. */
    /* sim not in scope here (this is a callback fed to the snapshot
     * builder); reach the active sim through serverSimGetActive so the
     * bot check still works after BotManager moved onto ServerSim. */
    ServerSim *active = serverSimGetActive();
    if (!udpServer.clients[playerNum].connected &&
        (active == NULL || !serverSimIsBot(active, playerNum))) {
        return NULL;
    }
    return udpServer.clients[playerNum].playerName;
}

const char *transportUdpServerGetClientCountryCode(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) {
        return NULL;
    }
    ServerSim *active = serverSimGetActive();
    if (!udpServer.clients[playerNum].connected &&
        (active == NULL || !serverSimIsBot(active, playerNum))) {
        return NULL;
    }
    return udpServer.clients[playerNum].countryCode;
}

uint8_t transportUdpServerGetClientType(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) {
        return CLIENT_TYPE_UNKNOWN;
    }
    ServerSim *active = serverSimGetActive();
    if (!udpServer.clients[playerNum].connected &&
        (active == NULL || !serverSimIsBot(active, playerNum))) {
        return CLIENT_TYPE_UNKNOWN;
    }
    return udpServer.clients[playerNum].clientType;
}

uint64_t transportUdpServerGetClientConnId(BYTE playerNum) {
    if (playerNum >= MAX_TANKS || !udpServer.clients[playerNum].connected) {
        return 0;
    }
    return udpServer.clients[playerNum].connId;
}

/* Validate an upload filename payload. The wire delivers a length-prefixed
 * name that may not be NUL-terminated, so iterate by index over nameLen.
 * Declared in transport_udp.h so the unit tests can exercise the matrix
 * directly; production callers stay inside this translation unit. */
bool uploadFilenameIsSafe(const char *name, size_t nameLen) {
    static const char *kReservedBasenames[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9",
    };

    if (!name || nameLen == 0) return false;
    /* At least one basename byte plus the 4-byte ".map" suffix. */
    if (nameLen < 5) return false;
    /* Basename must fit the display-name slot (MAP_STR_SIZE - 1). */
    if (nameLen > (size_t)(MAP_STR_SIZE - 1) + 4) return false;
    if (name[0] == '.') return false;
    for (size_t i = 0; i < nameLen; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == '/' || ch == '\\' || ch == ':') return false;
        if (ch == '\0') return false;
        if (ch < 0x20) return false;
    }
    if (SDL_strncasecmp(name + nameLen - 4, ".map", 4) != 0) return false;
    /* Trailing dot or space on the basename — Windows strips these on
     * file creation, which would bypass collision avoidance. */
    char preDot = name[nameLen - 5];
    if (preDot == '.' || preDot == ' ') return false;
    size_t baseLen = nameLen - 4;
    for (size_t i = 0;
         i < sizeof(kReservedBasenames) / sizeof(kReservedBasenames[0]);
         i++) {
        const char *r = kReservedBasenames[i];
        size_t rlen = SDL_strlen(r);
        if (baseLen == rlen && SDL_strncasecmp(name, r, rlen) == 0) {
            return false;
        }
    }
    return true;
}

/* Turn away a PACKET_ROUND_LOG_REQ. Every gate that refuses one sends this,
 * echoing the request's reqSeq: a silent refusal is indistinguishable from a
 * lost request, and the client would sit waiting for bytes that never come.
 * Wire: [header 8] [reqSeq 4 BE] [code 1]. */
static void serverSendRoundLogErr(uint32_t reqSeq, uint8_t code,
                                  const struct sockaddr_in *toAddr) {
    uint8_t err[PACKET_HEADER_SIZE + 5];
    packHeader(err, PACKET_ROUND_LOG_ERR, 0);
    packU32(err + PACKET_HEADER_SIZE, reqSeq);
    err[PACKET_HEADER_SIZE + 4] = code;
    srvSendTo(err, (int)sizeof(err), toAddr);
}

/* Process a single received packet — extracted from the recv loop so both
 * the polled fallback and the recv-thread drain path can share it. */
void serverProcessPacket(ServerSim *sim, uint8_t *buf, int len,
                         struct sockaddr_in *fromAddr) {
    uint8_t pktType;

    /* Check for old-protocol info request before new-protocol handling */
    if (isOldProtocolInfoRequest(buf, len)) {
        serverHandleInfoRequest(fromAddr, sim);
        return;
    }

    pktType = getPacketType(buf, len);
    switch (pktType) {
        case PACKET_JOIN_REQUEST:
            serverHandleJoinRequest(buf, len, fromAddr, sim);
            break;
        case PACKET_INPUT:
            serverHandleInput(buf, len, fromAddr, sim);
            break;
        case PACKET_PING:
            serverHandlePing(buf, len, fromAddr);
            /* Spectators aren't in clients[]; refresh their liveness too. */
            {
                int sIdx = serverFindSpectator(fromAddr);
                if (sIdx >= 0) {
                    udpServer.spectators[sIdx].lastReceivedTick = udpServer.tickCount;
                }
            }
            break;
        case PACKET_CHANNEL: {
            /* Standalone channel frame (client → server, sent when no input
             * rides this tick).  Body is one frame directly after the header. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0) {
                /* Spectator acks ride the same standalone-frame path; consume
                 * them into the spectator's own mux.  No bulk-receive drain —
                 * a spectator never uploads. */
                int sIdx = serverFindSpectator(fromAddr);
                if (sIdx >= 0) {
                    udpServer.spectators[sIdx].lastReceivedTick = udpServer.tickCount;
                    channelRecvFrame(&udpServer.spectators[sIdx].channelMux,
                                     buf + PACKET_HEADER_SIZE,
                                     len - PACKET_HEADER_SIZE);
                }
                break;
            }
            udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;
            if (channelRecvFrame(&udpServer.channelMux[clientIdx],
                                 buf + PACKET_HEADER_SIZE,
                                 len - PACKET_HEADER_SIZE) >= 0) {
                udpServer.channelFramesRx[clientIdx]++;
                serverDrainBulk(sim, clientIdx);
            }
            break;
        }
        case PACKET_COMMAND_TICK: {
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0) {
                /* A tankless spectator may send CMD_CHAT — and nothing else,
                 * and only while the server is in lobby/countdown. This branch
                 * is the hard isolation boundary: a viewer has no slot or sim
                 * state to mutate, so a decoded command of any other type is
                 * dropped here. The cmdSeq dedup mirrors the player path so the
                 * spectator's reliable carrier acks and retransmits coherently. */
                int sIdx = serverFindSpectator(fromAddr);
                if (sIdx < 0) break;
                SpectatorConn *sp = &udpServer.spectators[sIdx];
                ServerState st = serverSimGetState(sim);
                bool lobbyish =
                    (st == serverStateLobby || st == serverStateCountdown);
                sp->lastReceivedTick = udpServer.tickCount;
                if (len < PACKET_HEADER_SIZE + 1) break;
                uint8_t scount = buf[PACKET_HEADER_SIZE];
                size_t spos = PACKET_HEADER_SIZE + 1;
                for (uint8_t i = 0; i < scount; i++) {
                    if (spos + 2 > (size_t)len) break;
                    uint16_t entryLen = unpackU16(buf + spos);
                    spos += 2;
                    if (spos + entryLen > (size_t)len) break;
                    ClientCommand cmd;
                    if (!commandCodecDecode(buf + spos, entryLen, &cmd)) {
                        spos += entryLen;
                        continue;
                    }
                    spos += entryLen;
                    if (cmd.cmdSeq <= sp->inboundCmdSeq) continue;
                    if (cmd.cmdSeq != sp->inboundCmdSeq + 1) continue;
                    sp->inboundCmdSeq = cmd.cmdSeq;
                    if (cmd.type == CMD_CHAT && lobbyish) {
                        serverSimReceiveSpectatorChat(sim, (uint8_t)sIdx,
                                                      cmd.u.chat.body,
                                                      cmd.u.chat.bodyLen);
                    }
                }
                uint8_t sackBuf[PACKET_HEADER_SIZE + 4];
                packHeader(sackBuf, PACKET_COMMAND_ACK, sp->outSequence++);
                packU32(sackBuf + PACKET_HEADER_SIZE, sp->inboundCmdSeq);
                srvSendTo(sackBuf, sizeof(sackBuf), &sp->addr);
                break;
            }
            UdpServerClient *client = &udpServer.clients[clientIdx];
            if (len < PACKET_HEADER_SIZE + 1) break;
            uint8_t count = buf[PACKET_HEADER_SIZE];
            size_t pos = PACKET_HEADER_SIZE + 1;
            for (uint8_t i = 0; i < count; i++) {
                if (pos + 2 > (size_t)len) break;
                uint16_t entryLen = unpackU16(buf + pos);
                pos += 2;
                if (pos + entryLen > (size_t)len) break;
                ClientCommand cmd;
                if (!commandCodecDecode(buf + pos, entryLen, &cmd)) {
                    pos += entryLen;
                    continue;
                }
                pos += entryLen;
                if (cmd.cmdSeq <= client->inboundCmdSeq) continue;
                if (cmd.cmdSeq != client->inboundCmdSeq + 1) continue;
                (void)serverSimApplyCommand(sim, clientIdx, &cmd);
                client->inboundCmdSeq = cmd.cmdSeq;
            }
            uint8_t ackBuf[PACKET_HEADER_SIZE + 4];
            packHeader(ackBuf, PACKET_COMMAND_ACK, client->outSequence++);
            packU32(ackBuf + PACKET_HEADER_SIZE, client->inboundCmdSeq);
            srvSendTo(ackBuf, sizeof(ackBuf), &client->addr);
            break;
        }
        case PACKET_QUIT: {
            int clientIdx = serverFindClient(fromAddr);
            WB_LOG_INFO(WB_LOG_CAT_NET,
                "PACKET_QUIT from %s:%u clientIdx=%d",
                inet_ntoa(fromAddr->sin_addr),
                (unsigned)ntohs(fromAddr->sin_port),
                clientIdx);
            if (clientIdx >= 0) {
                serverCleanupMapDownload(clientIdx);
                serverDisconnectClient(sim, clientIdx, TRUE);
                serverSimRemovePlayer(sim, (BYTE)clientIdx);
                /* Broadcast lobby update if in lobby/countdown state */
                if (serverSimIsLobbyEnabled(sim) &&
                    (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown)) {
                    serverSimPublishLobbySlot(sim, (BYTE)clientIdx);
                }
            }
            break;
        }
        case PACKET_MAP_DL_READY: {
            /* Client's join-download buffers are armed (its JOIN_ACCEPT
             * landed). Body: [connId u64]. First ask for an armed-but-unbegun
             * download simply releases it (serverBeginMapTransferIfReady).
             * A re-ask while a stream is — or already was — in flight means
             * the client cannot complete that stream (its head was consumed
             * before the buffers existed, or the transfer finished into a
             * receiver that had been reset mid-body): the channel has acked
             * those bytes, so only a full restart behind a CHANNEL_BULK
             * re-base can deliver the map again. connId must match the slot's
             * so an address-spoofed READY can't reset a healthy client's
             * transfer or re-gate its snapshots. A resync in flight is left
             * alone — it owns the channel, and its own request/stall machinery
             * recovers it. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 8) {
                uint64_t reqConnId = unpackConnId(buf + PACKET_HEADER_SIZE);
                UdpServerClient *cl = &udpServer.clients[clientIdx];
                ClientMapDownload *dl = &udpServer.mapDownload[clientIdx];
                if (cl->connId != 0 && reqConnId != cl->connId) break;
                cl->lastReceivedTick = udpServer.tickCount;
                if (dl->xferKind == MAP_XFER_RESYNC || dl->resyncInProgress) {
                    break;
                }
                if (dl->xferKind == MAP_XFER_DOWNLOAD && !dl->xferBegun) {
                    dl->readySeen = TRUE;
                } else if (udpServer.mapReaskSeen[clientIdx] &&
                           (uint32_t)(udpServer.tickCount -
                                      udpServer.mapReaskLastTick[clientIdx]) <
                               MAP_REASK_MIN_TICKS) {
                    /* Inside the interval: drop it. The client's own watchdog
                     * asks again after its resend window, and that ask lands
                     * outside this one. Nothing is sent back — a re-ask has no
                     * reply of its own, and answering would hand back a second
                     * datagram for the one that was refused. */
                    udpServer.mapReaskThrottled[clientIdx]++;
                    WB_LOG_DEBUG(WB_LOG_CAT_NET,
                        "MAP_DL_READY re-ask slot=%d inside the %d-tick "
                        "interval -> throttled (%u so far)",
                        clientIdx, MAP_REASK_MIN_TICKS,
                        (unsigned)udpServer.mapReaskThrottled[clientIdx]);
                } else {
                    /* Stamp for every re-ask the server acts on, so the
                     * interval measures from the last restart it actually
                     * paid for. */
                    udpServer.mapReaskSeen[clientIdx] = true;
                    udpServer.mapReaskLastTick[clientIdx] = udpServer.tickCount;
                    /* Restart from this slot's own copy of the terrain rather
                     * than from whatever the shared staging buffer happens to
                     * hold: serverInitMapDownload copies staging, and staging
                     * carries the blob of whichever slot joined or resynced
                     * last. So recompress this slot's copy into staging, then
                     * re-send JOIN_ACCEPT so the size the client expects
                     * matches the blob it is about to be sent — the client
                     * drops a stream whose header size disagrees with the size
                     * its accept carried, so a restart off another slot's blob
                     * leaves it stuck on "Downloading map" instead of
                     * recovering it. Staging, then accept, then re-arm, the
                     * same order transportUdpServerOnLobbyMapChange uses. A
                     * compress that will not fit the wire size leaves staging's
                     * size alone and restarts exactly as before. */
                    int mapLen = serverSimGetCompressedMapFor(
                        sim, (BYTE)clientIdx, udpServer.compressedMap,
                        (int)sizeof(udpServer.compressedMap));
                    WB_LOG_INFO(WB_LOG_CAT_NET,
                        "MAP_DL_READY re-ask slot=%d (kind=%d begun=%d "
                        "complete=%d) -> restarting download",
                        clientIdx, (int)dl->xferKind, (int)dl->xferBegun,
                        (int)dl->downloadComplete);
                    if (mapLen > 0 && mapLen <= (int)MAP_DOWNLOAD_MAX_SIZE) {
                        udpServer.compressedMapSize = (uint32_t)mapLen;
                        serverSendJoinAccept(clientIdx, sim,
                                             &udpServer.clients[clientIdx].addr);
                    }
                    serverRebaseBulkAndRearmDownload(clientIdx);
                    dl->readySeen = TRUE;
                }
            }
            break;
        }
        case PACKET_MAP_RESYNC_REQUEST: {
            /* Client detected its terrain diverged (a dropped EVENT_MAP_CHANGE)
             * and asks for a fresh copy of the live map. Body: [resyncGen u32].
             * Only an established slot may ask — this is a data re-send the
             * client is already entitled to, not a state assertion.
             *
             * The compress + queue-cut must be atomic w.r.t. serverSimTick:
             * if a sim tick assigned a new EVENT_MAP_CHANGE a seq between the
             * compress and the cut, that change would be both baked into the
             * blob AND retained at seq >= cut, and apply twice. Packet handling
             * and the sim tick run on the same thread (the recv thread only
             * enqueues raw datagrams into recvQueue; serverProcessPacket and
             * serverSimTick are both driven from the timer/drain thread), so a
             * synchronous handler is naturally atomic. Keep it synchronous. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 4) {
                uint32_t reqGen = unpackU32(buf + PACKET_HEADER_SIZE);
                ClientMapDownload *dl = &udpServer.mapDownload[clientIdx];

                udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;

                if (dl->resyncInProgress) {
                    /* Idempotent: a resync is already armed/in-flight for this
                     * slot. Do NOT re-compress or re-cut — re-cutting would
                     * advance ackedSeq past changes enqueued since the first cut
                     * and drop them (the exact desync this recovers from). The
                     * bulk channel retransmits its own unacked segments, so
                     * there is nothing to re-poke. */
                } else {
                    /* Idle slot: compress this slot's copy of the terrain into
                     * its download buffer — the client is asking for the map
                     * it was given, not some other slot's — cut the map-event
                     * queue so the blob and the queue can't both carry the
                     * same change, then arm a resync transfer.
                     * It begins on CHANNEL_BULK once the channel is idle
                     * (serverServiceMapTransfer) and rides the snapshot trailer. */
                    int mapLen = serverSimGetCompressedMapFor(
                        sim, (BYTE)clientIdx, udpServer.compressedMap,
                        (int)sizeof(udpServer.compressedMap));
                    if (mapLen > 0 && mapLen <= (int)MAP_DOWNLOAD_MAX_SIZE) {
                        udpServer.compressedMapSize = (uint32_t)mapLen;
                        if (dl->compressedMap != NULL) free(dl->compressedMap);
                        dl->compressedMap = (BYTE *)malloc(udpServer.compressedMapSize);
                        memcpy(dl->compressedMap, udpServer.compressedMap,
                               udpServer.compressedMapSize);
                        dl->mapSize = udpServer.compressedMapSize;
                        /* The cut: the blob carries every change up to
                         * nextSeq-1, so empty the queue. Changes during the
                         * transfer land at seq >= nextSeq and are held by the
                         * send gate until completion. */
                        udpServer.mapEventQueues[clientIdx].ackedSeq =
                            udpServer.mapEventQueues[clientIdx].nextSeq;
                        dl->resyncGen = reqGen;
                        dl->resyncInProgress = TRUE;
                        /* Tag map-change events sent from here on with this
                         * request's generation. The client drops any map event
                         * tagged older than the generation it installs, so a
                         * stale change still in flight on the channel can't
                         * apply on top of the freshly downloaded blob. */
                        udpServer.mapGen[clientIdx] = reqGen;
                        /* Arm the resync stream (downloadComplete stays true for
                         * this established slot — only resyncInProgress gates the
                         * held map events). */
                        dl->xferKind = MAP_XFER_RESYNC;
                        dl->xferBegun = false;
                        dl->xferStartSeq = 0;
                        dl->xferEndSeq = 0;

                        /* Self-check: the blob the client will install must
                         * round-trip back to this slot's copy of the terrain
                         * and its record of the pill squares — the same two the
                         * snapshot header's checksum is stamped from. If it
                         * doesn't, the client can never match that checksum and
                         * loops resync requests until it self-kicks, so decode
                         * the blob into scratch structures and compare
                         * tile-for-tile against that copy. The pill list
                         * follows the same rule as the terrain: the check is
                         * against what the client was actually given, not
                         * against the live state, because comparing against the
                         * live map would report a difference on every resync
                         * from a culled slot, whose copy lags the live map by
                         * design. The live map and pill list are used only for
                         * a slot with no records bound. Resyncs are infrequent;
                         * the cost is acceptable for the diagnosis. */
                        {
                            map *known = sim->clientKnownMap[clientIdx] != NULL
                                             ? &sim->clientKnownMap[clientIdx]
                                             : &serverSimGetGameSim(sim)->mp;
                            struct pillsObj slotPills;
                            pillboxes slotPb = &slotPills;
                            bool useSlotPills =
                                serverSimGetPillsForSlot(sim, (BYTE)clientIdx,
                                                         &slotPills);
                            pillboxes *knownPb = useSlotPills
                                             ? &slotPb
                                             : &serverSimGetGameSim(sim)->pb;
                            uint16_t knownSum = mapCalcChecksum(known,
                                                   &serverSimGetGameSim(sim)->bs,
                                                   knownPb);
                            map rtMap; pillboxes rtPb; bases rtBs; starts rtSs;
                            mapCreate(&rtMap);
                            pillsCreate(&rtPb);
                            basesCreate(&rtBs);
                            startsCreate(&rtSs);
                            if (mapLoadCompressedMap(&rtMap, &rtPb, &rtBs, &rtSs,
                                                     udpServer.compressedMap, mapLen)) {
                                uint16_t rtSum = mapCalcChecksum(&rtMap, &rtBs, &rtPb);
                                if (rtSum != knownSum) {
                                    /* Dedupe: an unconverged divergence repeats on every
                                     * resync request and floods the log. Dump full per-tile
                                     * detail only when the (copy,blob) checksum pair changes;
                                     * identical repeats get one concise line. The state is
                                     * process-wide and this runs on the single drain thread. */
                                    static uint32_t s_lastResyncDiffSig = 0xFFFFFFFFu;
                                    uint32_t sig = ((uint32_t)knownSum << 16) | (uint32_t)rtSum;
                                    const char *mapName = serverSimGetMapName(sim);
                                    if (sig == s_lastResyncDiffSig) {
                                        WB_LOG_WARN(WB_LOG_CAT_NET,
                                            "map resync still not converging on '%s' "
                                            "(client copy sum=%u blob sum=%u, client %d gen=%u) - detail suppressed",
                                            mapName, (unsigned)knownSum, (unsigned)rtSum,
                                            clientIdx, (unsigned)reqGen);
                                    } else {
                                        bases *liveBs = &serverSimGetGameSim(sim)->bs;
                                        int diffs = 0, realDiffs = 0, shown = 0, xx, yy;
                                        s_lastResyncDiffSig = sig;
                                        for (yy = 0; yy < MAP_ARRAY_SIZE; yy++) {
                                            for (xx = 0; xx < MAP_ARRAY_SIZE; xx++) {
                                                BYTE kv = mapGetPos(known, (BYTE)xx, (BYTE)yy);
                                                BYTE rv = mapGetPos(&rtMap, (BYTE)xx, (BYTE)yy);
                                                if (kv != rv) {
                                                    /* Terrain under a base/pill is folded to ROAD
                                                     * by the checksum (it is not authoritative), so
                                                     * such a tile can never be the real cause of
                                                     * non-convergence — flag it benign. */
                                                    bool onBase = (basesExistPos(liveBs, (BYTE)xx, (BYTE)yy) ||
                                                                   basesExistPos(&rtBs, (BYTE)xx, (BYTE)yy));
                                                    bool onPill = (pillsExistPos(knownPb, (BYTE)xx, (BYTE)yy) ||
                                                                   pillsExistPos(&rtPb, (BYTE)xx, (BYTE)yy));
                                                    diffs++;
                                                    if (!onBase && !onPill) { realDiffs++; }
                                                    if (shown < 8) {
                                                        WB_LOG_WARN(WB_LOG_CAT_NET,
                                                            "map resync blob diff @(%d,%d) "
                                                            "clientcopy=%s(%u) roundtrip=%s(%u) [%s] map='%s'",
                                                            xx, yy,
                                                            resyncTerrainName(kv), (unsigned)kv,
                                                            resyncTerrainName(rv), (unsigned)rv,
                                                            onBase ? "base" : (onPill ? "pill" : "REAL"),
                                                            mapName);
                                                        shown++;
                                                    }
                                                }
                                            }
                                        }
                                        WB_LOG_WARN(WB_LOG_CAT_NET,
                                            "map resync blob does NOT round-trip on '%s': "
                                            "%d differing tile(s) (%d genuine, %d under base/pill fixup) "
                                            "(client copy sum=%u blob sum=%u) - %s",
                                            mapName, diffs, realDiffs, diffs - realDiffs,
                                            (unsigned)knownSum, (unsigned)rtSum,
                                            realDiffs ? "client cannot converge"
                                                      : "benign structure fixup only");
                                    }
                                }
                            } else {
                                WB_LOG_WARN(WB_LOG_CAT_NET,
                                    "map resync blob failed self-check decode (gen=%u, %d bytes)",
                                    (unsigned)reqGen, mapLen);
                            }
                            mapDestroy(&rtMap);
                            pillsDestroy(&rtPb);
                            basesDestroy(&rtBs);
                            startsDestroy(&rtSs);
                            fprintf(stderr,
                                    "[UDP SERVER] Client %d map resync gen=%u (%d bytes) clientcopysum=%u\n",
                                    clientIdx, reqGen, mapLen, (unsigned)knownSum);
                        }
                    } else {
                        fprintf(stderr,
                                "[UDP SERVER] Client %d map resync: compress failed (%d)\n",
                                clientIdx, mapLen);
                    }
                }
            }
            break;
        }
        case PACKET_LOBBY_MAP_LIST_REQ: {
            /* [header 8] [pathLen 1] [path N] — any lobby client may
             * ask. Response is sent back to the requester only
             * (wire-only handshake per ARCHITECTURE.md §"Load-bearing
             * wire-only exceptions"). */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 1) break;
            if (udpServer.clientReqCooldownTicks[clientIdx] > 0) break;
            udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
            uint8_t pathLen = buf[PACKET_HEADER_SIZE];
            if (pathLen > 255 ||
                len < PACKET_HEADER_SIZE + 1 + pathLen) break;
            char relPath[256];
            memset(relPath, 0, sizeof(relPath));
            if (pathLen > 0) {
                memcpy(relPath, buf + PACKET_HEADER_SIZE + 1, pathLen);
            }

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

            /* Cap matches LOBBY_MAP_LIST_MAX on the client so a
             * directory's full content survives end-to-end. Stack-
             * resident; each ServerMapEntry is ~152 bytes → ~76 KB,
             * fine for any normal thread stack. */
            ServerMapEntry entries[LOBBY_MAP_LIST_MAX];
            int got = -1;
            if (safe) {
                got = serverSimEnumerateMapDir(sim,
                    relPath[0] == '\0' ? NULL : relPath,
                    entries, LOBBY_MAP_LIST_MAX);
            }
            if (got < 0) got = 0;

            /* Chunked send: each frame fits in UDP_MAX_PAYLOAD and
             * carries [header][pathLen][path][final][count][entries].
             * Last chunk sets final=1; empty result is a single chunk
             * with count=0, final=1. Lost final-chunk failure mode is
             * accepted — chooser shows a partial list until next req. */
            uint8_t rsp[UDP_MAX_PAYLOAD];
            int i = 0;
            do {
                int rpos = PACKET_HEADER_SIZE;
                packHeader(rsp, PACKET_LOBBY_MAP_LIST_RSP, 0);
                rsp[rpos++] = pathLen;
                if (pathLen > 0) {
                    memcpy(rsp + rpos, relPath, pathLen);
                    rpos += pathLen;
                }
                int finalPos = rpos;
                rsp[rpos++] = 0;
                int countPos = rpos;
                rsp[rpos++] = 0;
                int written = 0;
                for (; i < got; i++) {
                    int nameLen = (int)SDL_strlen(entries[i].name);
                    if (nameLen > 127) nameLen = 127;
                    if (rpos + 1 + nameLen + 1 + 8 > (int)sizeof(rsp)) break;
                    rsp[rpos++] = (uint8_t)nameLen;
                    memcpy(rsp + rpos, entries[i].name, nameLen);
                    rpos += nameLen;
                    rsp[rpos++] = entries[i].isFolder ? 1 : 0;
                    uint64_t mt = (uint64_t)entries[i].modTime;
                    for (int b = 7; b >= 0; b--) {
                        rsp[rpos++] = (uint8_t)((mt >> (b * 8)) & 0xFF);
                    }
                    written++;
                }
                rsp[countPos] = (uint8_t)written;
                rsp[finalPos] = (i >= got) ? 1 : 0;
                srvSendTo(rsp, rpos, fromAddr);
            } while (i < got);
            break;
        }
        case PACKET_LOBBY_MAP_USE_LOCAL: {
            /* [header 8] [totalLen 4] [nameLen 1] [name N]
             *           [relPathLen 1] [relPath M] [md5 32]
             *
             * Pre-upload optimisation: if our local data/maps/<relPath>
             * matches the supplied MD5, install it directly and reply
             * UPLOAD_DONE — no byte transfer needed. On any miss
             * (permission, path/name unsafe, file missing, MD5
             * mismatch) reply MAP_USE_LOCAL_NACK and let the client
             * fall back to the regular UPLOAD_BEGIN + bulk-stream flow. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 4 + 1 + 1 + 32) break;
            int rpos = PACKET_HEADER_SIZE;
            uint32_t totalLen =
                ((uint32_t)buf[rpos + 0] << 24) |
                ((uint32_t)buf[rpos + 1] << 16) |
                ((uint32_t)buf[rpos + 2] <<  8) |
                ((uint32_t)buf[rpos + 3]);
            rpos += 4;
            uint8_t nameLen = buf[rpos++];
            if (nameLen == 0 || nameLen > 127 ||
                rpos + nameLen + 1 + 32 > (int)len) break;
            char nameBuf[128];
            memset(nameBuf, 0, sizeof(nameBuf));
            memcpy(nameBuf, buf + rpos, nameLen);
            rpos += nameLen;
            uint8_t relLen = buf[rpos++];
            if (relLen == 0 || relLen > 255 ||
                rpos + relLen + 32 > (int)len) break;
            char relBuf[256];
            memset(relBuf, 0, sizeof(relBuf));
            memcpy(relBuf, buf + rpos, relLen);
            rpos += relLen;
            char wantMd5Hex[33];
            memcpy(wantMd5Hex, buf + rpos, 32);
            wantMd5Hex[32] = '\0';

            /* NACK helper for every miss path: server echoes the
             * announce name so the client correlates the reply to
             * the right in-flight USE_LOCAL. */
            #define SEND_USE_LOCAL_NACK() do { \
                uint8_t nack[PACKET_HEADER_SIZE + 1 + 128]; \
                int npos = PACKET_HEADER_SIZE; \
                packHeader(nack, PACKET_LOBBY_MAP_USE_LOCAL_NACK, 0); \
                nack[npos++] = (uint8_t)nameLen; \
                if (nameLen > 0) { memcpy(nack + npos, nameBuf, nameLen); npos += nameLen; } \
                srvSendTo(nack, npos, fromAddr); \
            } while (0)

            if (!lobbyClientMayEdit(sim, clientIdx)) {
                SEND_USE_LOCAL_NACK();
                break;
            }

            /* Single-thread the upload slot. USE_LOCAL writes to
             * sim->pendingUpload* the same as UPLOAD_DONE, so a
             * USE_LOCAL landing while another client's upload is
             * in flight would clobber their pending preview. */
            if (lobbyAnyOtherUploadActive(udpServer.clientUploadActive,
                                           clientIdx)) {
                SEND_USE_LOCAL_NACK();
                break;
            }

            /* Name safety: must end in ".map", no separators, no
             * leading dot. Mirrors the UPLOAD_BEGIN guards so a
             * malicious relPath can't bypass them. */
            bool nameSafe = true;
            if (nameBuf[0] == '.') nameSafe = false;
            for (int i = 0; i < nameLen; i++) {
                char ch = nameBuf[i];
                if (ch == '/' || ch == '\\' || ch == ':') {
                    nameSafe = false; break;
                }
            }
            if (nameSafe) {
                if (nameLen < 4 ||
                    SDL_strcasecmp(nameBuf + nameLen - 4, ".map") != 0) {
                    nameSafe = false;
                }
            }
            if (totalLen == 0 || totalLen > LOBBY_MAP_UPLOAD_MAX_BYTES || !nameSafe) {
                SEND_USE_LOCAL_NACK();
                break;
            }

            /* Try to read data/maps/<relPath>. serverSimReadMapFile
             * handles the path-safety check (".." rejection, etc.). */
            uint8_t *bytes = NULL;
            size_t   byteLen = 0;
            if (!serverSimReadMapFile(sim, relBuf, &bytes, &byteLen) ||
                bytes == NULL ||
                (uint32_t)byteLen != totalLen) {
                if (bytes) free(bytes);
                SEND_USE_LOCAL_NACK();
                break;
            }

            uint8_t haveMd5[16];
            md5Compute(bytes, byteLen, haveMd5);
            char haveMd5Hex[33];
            md5ToHex(haveMd5, haveMd5Hex);
            if (memcmp(haveMd5Hex, wantMd5Hex, 32) != 0) {
                free(bytes);
                SEND_USE_LOCAL_NACK();
                break;
            }

            /* MD5 match — install as preview directly from the local
             * file. The file already lives at its final path, so we
             * just reload it; there is no temp-staging step in the
             * in-memory upload model. */
            free(bytes);  /* serverSimReloadMap re-reads it via its own path */

            char localPath[FILENAME_MAX];
            SDL_snprintf(localPath, sizeof(localPath), "%s/%s",
                         serverSimGetMapDirRoot(sim), relBuf);
            bool previewed = false;
            if (serverSimReloadMap(sim, localPath)) {
                /* Display name: the announce name without ".map". */
                char displayName[MAP_STR_SIZE];
                SDL_strlcpy(displayName, nameBuf, sizeof(displayName));
                {
                    size_t dlen = SDL_strlen(displayName);
                    if (dlen >= 4 &&
                        SDL_strcasecmp(displayName + dlen - 4, ".map") == 0) {
                        displayName[dlen - 4] = '\0';
                    }
                }
                serverSimSetMapName(sim, displayName);
                previewed = true;
            }

            uint8_t done[PACKET_HEADER_SIZE + 2 + 256];
            int dpos = PACKET_HEADER_SIZE;
            packHeader(done, PACKET_LOBBY_MAP_UPLOAD_DONE, 0);
            done[dpos++] = previewed ? 0 : LOBBY_REJECT_INVALID;
            done[dpos++] = (uint8_t)relLen;
            if (relLen > 0) { memcpy(done + dpos, relBuf, relLen); dpos += relLen; }
            srvSendTo(done, dpos, fromAddr);
            #undef SEND_USE_LOCAL_NACK
            break;
        }
        case PACKET_LOBBY_MAP_UPLOAD_BEGIN: {
            /* [header 8] [totalLen 4] [nameLen 1] [name N] — only host
             * / admin / openHost may push files. Per-client wire-only
             * ACK (handshake/reliability). */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 5) break;
            /* Cooldown gate — silent break used to leave the client at
             * upload-status=1 (BEGIN sent, awaiting ACK) indefinitely,
             * jamming further picks. Reply with COOLDOWN so the client's
             * upload pump transitions to status=4 and frees the slot. */
            if (udpServer.clientReqCooldownTicks[clientIdx] > 0) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_COOLDOWN;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_NOT_HOST;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_LOCKED;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            if (udpServer.uploadPolicy == UPLOAD_POLICY_OFF) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_UPLOAD_DISABLED;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            /* Single-thread the upload slot. The sim has one preview
             * pending-upload slot; allowing two clients to race
             * truncates the loser's bytes on the global temp file
             * and overwrites their pending paths. Same-client
             * retry is fine — the clientUploadBuf cleanup below
             * handles that — only OTHER clients trigger this gate. */
            if (lobbyAnyOtherUploadActive(udpServer.clientUploadActive,
                                           clientIdx)) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_UPLOAD_BUSY;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            uint32_t totalLen =
                ((uint32_t)buf[PACKET_HEADER_SIZE + 0] << 24) |
                ((uint32_t)buf[PACKET_HEADER_SIZE + 1] << 16) |
                ((uint32_t)buf[PACKET_HEADER_SIZE + 2] <<  8) |
                ((uint32_t)buf[PACKET_HEADER_SIZE + 3]);
            uint8_t nameLen = buf[PACKET_HEADER_SIZE + 4];
            if (nameLen == 0 || nameLen > 127 ||
                len < PACKET_HEADER_SIZE + 5 + nameLen ||
                totalLen == 0 || totalLen > LOBBY_MAP_UPLOAD_MAX_BYTES) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_INVALID;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            char nameBuf[128];
            memset(nameBuf, 0, sizeof(nameBuf));
            memcpy(nameBuf, buf + PACKET_HEADER_SIZE + 5, nameLen);

            if (!uploadFilenameIsSafe(nameBuf, nameLen)) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_INVALID;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }

            if (udpServer.uploadPolicy == UPLOAD_POLICY_PERSIST) {
                ServerMapEntry entries[256];
                int got = serverSimEnumerateMapDir(sim, "Uploads",
                                                    entries,
                                                    (int)(sizeof(entries) /
                                                          sizeof(entries[0])));
                if (got < 0) got = 0;
                int fileCount = 0;
                uint64_t totalBytes = 0;
                for (int i = 0; i < got; i++) {
                    if (!entries[i].isFolder) {
                        fileCount++;
                        totalBytes += (uint64_t)entries[i].size;
                    }
                }
                if (fileCount >= udpServer.uploadMaxFiles ||
                    totalBytes + totalLen > udpServer.uploadMaxStorageBytes) {
                    uint8_t ack[PACKET_HEADER_SIZE + 1];
                    packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                    ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_UPLOAD_LIMIT_HIT;
                    srvSendTo(ack, sizeof(ack), fromAddr);
                    break;
                }
            }

            udpServer.clientUploadActive[clientIdx] = true;
            udpServer.clientUploadTotal[clientIdx]  = totalLen;
            SDL_strlcpy(udpServer.clientUploadName[clientIdx], nameBuf,
                        sizeof(udpServer.clientUploadName[clientIdx]));
            /* Fresh receiver for this transfer; the bulk stream that follows
             * carries the bytes (no offset reassembly). */
            bulkReceiverInit(&udpServer.bulkRecvUp[clientIdx]);

            uint8_t ack[PACKET_HEADER_SIZE + 1];
            packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
            ack[PACKET_HEADER_SIZE] = 0;
            srvSendTo(ack, sizeof(ack), fromAddr);
            break;
        }
        case PACKET_LOBBY_MAP_SEARCH_REQ: {
            /* [header 8] [pathLen 1] [path N] [queryLen 1] [query M].
             * Recursive search of data/maps/<path> for .map files
             * whose basename contains <query>. Read-only, any
             * connected client may issue. Per-client wire-only RSP. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 2) break;
            if (udpServer.clientReqCooldownTicks[clientIdx] > 0) break;
            udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
            int rpos = PACKET_HEADER_SIZE;
            uint8_t pathLen = buf[rpos++];
            if (pathLen > 255 ||
                rpos + pathLen + 1 > (int)len) break;
            char relPath[256];
            memset(relPath, 0, sizeof(relPath));
            if (pathLen > 0) {
                memcpy(relPath, buf + rpos, pathLen);
            }
            rpos += pathLen;
            uint8_t qLen = buf[rpos++];
            if (qLen > 127 || rpos + qLen > (int)len) break;
            char query[128];
            memset(query, 0, sizeof(query));
            if (qLen > 0) {
                memcpy(query, buf + rpos, qLen);
            }

            ServerMapEntry entries[LOBBY_MAP_LIST_MAX];
            int got = serverSimSearchMapDir(sim,
                relPath[0] == '\0' ? NULL : relPath,
                query, entries, LOBBY_MAP_LIST_MAX);
            if (got < 0) got = 0;

            /* Chunked send — every chunk repeats the full path+query
             * prefix so the client can filter stale responses from a
             * prior navigation. Final chunk sets final=1; empty
             * result is a single chunk with count=0, final=1. */
            uint8_t rsp[UDP_MAX_PAYLOAD];
            int i = 0;
            do {
                int wpos = PACKET_HEADER_SIZE;
                packHeader(rsp, PACKET_LOBBY_MAP_SEARCH_RSP, 0);
                rsp[wpos++] = pathLen;
                if (pathLen > 0) {
                    memcpy(rsp + wpos, relPath, pathLen);
                    wpos += pathLen;
                }
                rsp[wpos++] = qLen;
                if (qLen > 0) {
                    memcpy(rsp + wpos, query, qLen);
                    wpos += qLen;
                }
                int finalPos = wpos;
                rsp[wpos++] = 0;
                int countPos = wpos;
                rsp[wpos++] = 0;
                int written = 0;
                for (; i < got; i++) {
                    int nameLen = (int)SDL_strlen(entries[i].name);
                    if (nameLen > 127) nameLen = 127;
                    if (wpos + 1 + nameLen + 1 + 8 > (int)sizeof(rsp)) break;
                    rsp[wpos++] = (uint8_t)nameLen;
                    memcpy(rsp + wpos, entries[i].name, nameLen);
                    wpos += nameLen;
                    rsp[wpos++] = entries[i].isFolder ? 1 : 0;
                    uint64_t mt = (uint64_t)entries[i].modTime;
                    for (int b = 7; b >= 0; b--) {
                        rsp[wpos++] = (uint8_t)((mt >> (b * 8)) & 0xFF);
                    }
                    written++;
                }
                rsp[countPos] = (uint8_t)written;
                rsp[finalPos] = (i >= got) ? 1 : 0;
                srvSendTo(rsp, wpos, fromAddr);
            } while (i < got);
            break;
        }
        case PACKET_LOBBY_MAP_PREVIEW_REQ: {
            /* [header 8] [pathLen 1] [path N]. Reads data/maps/<path>
             * from the server's filesystem and streams the bytes back
             * over CHANNEL_BULK behind a stream header. Client rasterises
             * locally — server has no dep on a renderer or image encoder,
             * and the protocol is the same shape in SP-host (loopback)
             * and MP. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                len < PACKET_HEADER_SIZE + 1) break;
            int rpos = PACKET_HEADER_SIZE;
            uint8_t pathLen = buf[rpos++];
            if (pathLen == 0 || pathLen > 255 ||
                rpos + pathLen > len) break;
            char relPath[256];
            memset(relPath, 0, sizeof(relPath));
            memcpy(relPath, buf + rpos, pathLen);

            uint8_t *mapBytes = NULL;
            size_t   mapLen   = 0;
            bool ok = serverSimReadMapFile(sim, relPath,
                                            &mapBytes, &mapLen);
            if (!ok) {
                uint8_t err[PACKET_HEADER_SIZE + 1 + 256 + 1];
                packHeader(err, PACKET_LOBBY_MAP_PREVIEW_ERR, 0);
                int wpos = PACKET_HEADER_SIZE;
                err[wpos++] = pathLen;
                memcpy(err + wpos, relPath, pathLen);
                wpos += pathLen;
                err[wpos++] = 1;  /* not-found / unreadable */
                srvSendTo(err, wpos, fromAddr);
                break;
            }

            /* One transfer at a time on this client's bulk byte stream: if a
             * transfer is already in flight, reject with PREVIEW_ERR and let
             * the client re-request via PREVIEW_REQ once it drains. */
            if (bulkSenderBusy(&udpServer.bulkSend[clientIdx])) {
                uint8_t err[PACKET_HEADER_SIZE + 1 + 256 + 1];
                packHeader(err, PACKET_LOBBY_MAP_PREVIEW_ERR, 0);
                int wpos = PACKET_HEADER_SIZE;
                err[wpos++] = pathLen;
                memcpy(err + wpos, relPath, pathLen);
                wpos += pathLen;
                err[wpos++] = 3;  /* transient: bulk channel busy */
                srvSendTo(err, wpos, fromAddr);
                free(mapBytes);
                break;
            }

            /* Monotonic per-process preview sequence, carried in the stream
             * header so the client can match a completed blob to the request
             * it issued. udpServer is a single global, so a process-wide
             * counter is fine; collisions across long sessions wrap harmlessly. */
            static uint32_t s_previewSeq = 0;
            uint32_t seq = ++s_previewSeq;

            /* Frame the preview as a sized blob on CHANNEL_BULK: the stream
             * header (kind/gen/total/path) then the map bytes, fed onto the
             * reliable stream by bulkSenderPump as the window drains. */
            BulkStreamHeader sh;
            memset(&sh, 0, sizeof(sh));
            sh.kind = BULK_KIND_PREVIEW;
            sh.gen = seq;
            sh.totalSize = (uint32_t)mapLen;
            sh.pathLen = pathLen;
            memcpy(sh.path, relPath, pathLen);
            sh.path[pathLen] = '\0';

            if (!bulkSenderBegin(&udpServer.bulkSend[clientIdx], &sh,
                                 mapBytes, (uint32_t)mapLen)) {
                uint8_t err[PACKET_HEADER_SIZE + 1 + 256 + 1];
                packHeader(err, PACKET_LOBBY_MAP_PREVIEW_ERR, 0);
                int wpos = PACKET_HEADER_SIZE;
                err[wpos++] = pathLen;
                memcpy(err + wpos, relPath, pathLen);
                wpos += pathLen;
                err[wpos++] = 3;  /* internal: could not stage transfer */
                srvSendTo(err, wpos, fromAddr);
            }
            free(mapBytes);
            break;
        }
        case PACKET_ROUND_LOG_REQ: {
            /* [header 8] [reqSeq 4 BE]. Hands back the last completed round's
             * .wbv over CHANNEL_BULK behind a BULK_KIND_ROUND_LOG stream
             * header, so a client that joined after the round can replay what
             * its lobby recap describes. Gates run cheapest-refusal first and
             * every one of them answers with PACKET_ROUND_LOG_ERR. */
            uint32_t reqSeq;
            int clientIdx;
            int j;
            int concurrent = 0;
            ServerState st;
            uint8_t *blob = NULL;
            uint32_t blobLen = 0;
            char logName[BULK_PATH_MAX + 1];
            size_t nameLen;
            RoundLogReadResult rr;
            BulkStreamHeader sh;

            if (len < PACKET_HEADER_SIZE + 4) break;
            reqSeq = unpackU32(buf + PACKET_HEADER_SIZE);

            /* Players only. A spectator's bulk sender is saturated by the
             * delayed feed it connected to receive, so serving one there
             * would starve the thing it came for. An address that is neither
             * gets nothing: there is no session to answer, and this file's
             * other lobby handlers drop unknown senders the same way. */
            clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0) {
                if (serverFindSpectator(fromAddr) >= 0) {
                    serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_DISABLED,
                                          fromAddr);
                }
                break;
            }

            /* Lobby and game-over only. While a game runs CHANNEL_BULK
             * belongs to joiner map downloads and desync resyncs, so the
             * refusal is transient — the state will change. */
            st = serverSimGetState(sim);
            if (st != serverStateLobby && st != serverStateGameOver) {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_BUSY, fromAddr);
                break;
            }

            /* No source registered means no recorder in this build, which the
             * transport reports as "disabled" without knowing the recorder
             * exists. Serve policy is asked per request, never latched. */
            if (!s_roundLogSourceSet || !s_roundLogSource.serveEnabled()) {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_DISABLED, fromAddr);
                break;
            }

            /* Per-client bounds. A busy sender means this client already has a
             * transfer on its byte stream — its own round log, or a map
             * preview draining — and the interval plus the per-round ceiling
             * stop it re-asking in a loop. */
            if (bulkSenderBusy(&udpServer.bulkSend[clientIdx]) ||
                udpServer.roundLogServed[clientIdx] >= ROUND_LOG_MAX_ATTEMPTS ||
                (udpServer.roundLogReqSeen[clientIdx] &&
                 (uint32_t)(udpServer.tickCount -
                            udpServer.roundLogLastReqTick[clientIdx]) <
                     ROUND_LOG_MIN_REQ_TICKS)) {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_RATE_LIMITED,
                                      fromAddr);
                break;
            }
            /* Stamp the clock for every request the server considers, served
             * or not, so the interval holds whatever the answer turns out to
             * be. */
            udpServer.roundLogReqSeen[clientIdx] = true;
            udpServer.roundLogLastReqTick[clientIdx] = udpServer.tickCount;

            /* Fleet-wide cap. Transient: the client retries and gets in when
             * one of the transfers ahead of it finishes. */
            for (j = 0; j < MAX_TANKS; j++) {
                if (bulkSenderBusy(&udpServer.bulkSend[j]) &&
                    udpServer.bulkSend[j].kind == BULK_KIND_ROUND_LOG) {
                    concurrent++;
                }
            }
            if (concurrent >= ROUND_LOG_MAX_CONCURRENT) {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_BUSY, fromAddr);
                break;
            }

            /* The source owns the cap check, so an over-cap log is refused
             * without ever being read. A read failure reports as "none": the
             * round is not gettable and the client can do nothing different
             * with the distinction. */
            rr = s_roundLogSource.read(&blob, &blobLen, logName,
                                       sizeof(logName));
            if (rr != ROUND_LOG_READ_OK) {
                serverSendRoundLogErr(reqSeq,
                                      (rr == ROUND_LOG_READ_TOO_LARGE)
                                          ? ROUND_LOG_ERR_TOO_LARGE
                                          : ROUND_LOG_ERR_NONE,
                                      fromAddr);
                break;
            }

            /* gen echoes reqSeq so the client can match the blob to the
             * request it issued and drop a superseded one; the path carries
             * the basename only. */
            memset(&sh, 0, sizeof(sh));
            sh.kind = BULK_KIND_ROUND_LOG;
            sh.gen = reqSeq;
            sh.totalSize = blobLen;
            nameLen = strlen(logName);
            if (nameLen > BULK_PATH_MAX) nameLen = BULK_PATH_MAX;
            sh.pathLen = (uint8_t)nameLen;
            memcpy(sh.path, logName, nameLen);
            sh.path[nameLen] = '\0';

            if (bulkSenderBegin(&udpServer.bulkSend[clientIdx], &sh,
                                blob, blobLen)) {
                udpServer.roundLogServed[clientIdx]++;
            } else {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_BUSY, fromAddr);
            }
            free(blob);   /* bulkSenderBegin copied it into its own buffer */
            break;
        }
        case PACKET_WBN_REAUTH: {
            /* Wire: [header 8] [wbnJoinKey 65]. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0) break;
            ClientCommand cmd;
            if (!commandCodecDecode(buf, len, &cmd)) break;
            (void)serverSimApplyCommand(sim, clientIdx, &cmd);
            break;
        }
        case PACKET_PUNCH_NOTIFY: {
            /* Wire: [header 8] [joiner reflexive IP 4 BE] [joiner port 2 BE].
             * Total 14 bytes. Tracker pushed this through our keepalive's NAT
             * mapping; the body tells us where to fire punch packets. */
            if (len < PACKET_HEADER_SIZE + 6) break;
            int slot;
            for (slot = 0; slot < PUNCH_QUEUE_SIZE; slot++) {
                if (punchQueue[slot].packetsRemaining == 0) break;
            }
            if (slot >= PUNCH_QUEUE_SIZE) break;
            memset(&punchQueue[slot].addr, 0, sizeof(punchQueue[slot].addr));
            punchQueue[slot].addr.sin_family = AF_INET;
            memcpy(&punchQueue[slot].addr.sin_addr, buf + PACKET_HEADER_SIZE, 4);
            punchQueue[slot].addr.sin_port =
                htons(unpackU16(buf + PACKET_HEADER_SIZE + 4));
            punchQueue[slot].packetsRemaining = PUNCH_BURST_PACKETS;
            punchQueue[slot].ticksUntilNext   = 0;
            break;
        }
        case PACKET_PUNCH_PROBE_REPLY: {
            /* Wire: [header 8] [reflexive IP 4 bytes network order]
             *       [reflexive port 2 bytes BE]. Total 14 bytes. */
            if (len < PACKET_HEADER_SIZE + 6) break;
            char reflexiveIp[64];
            uint16_t reflexivePort;
            struct in_addr addr;
            memcpy(&addr.s_addr, buf + PACKET_HEADER_SIZE, 4);
            {
                const char *s = inet_ntoa(addr);
                if (s == NULL) break;
                strncpy(reflexiveIp, s, sizeof(reflexiveIp) - 1);
                reflexiveIp[sizeof(reflexiveIp) - 1] = '\0';
            }
            reflexivePort = unpackU16(buf + PACKET_HEADER_SIZE + 4);
            serverInstanceRecordProbeReply(reflexiveIp, reflexivePort);
            break;
        }
        default:
            break;
        }
}

/* Drain sim events into per-client reliable queues.
 * Must be called after each serverSimTick() so events survive
 * being cleared at the start of the next tick.
 * The three sound events are culled against SDIST_NONE measured from the
 * recipient's own tank and deduplicated per sound type — only the closest
 * instance of each type is sent, and it goes out carrying a near/far tier and
 * a compass bearing in place of the map square it was raised at. */
void transportUdpServerDrainEvents(ServerSim *sim) {
    int i, c;

    if (!udpServer.running) return;

    /* Once-per-second map-event-drop summary. A dropped EVENT_MAP_CHANGE
     * leaves that square owed until a sweep re-sends it, so surface how often
     * the drop guard is firing. Mirrors the [netimpair]
     * once-per-second pattern; only emitted when at least one slot has dropped
     * something, to keep clean logs quiet. */
    {
        static uint64_t lastMapDropLogMs = 0;
        uint64_t nowMs = (uint64_t)SDL_GetTicks();
        if (nowMs - lastMapDropLogMs >= 1000) {
            uint32_t total = 0;
            for (c = 0; c < MAX_TANKS; c++) total += udpServer.mapEventQueueDrops[c];
            lastMapDropLogMs = nowMs;
            if (total > 0) {
                char perSlot[256];
                int p = 0;
                perSlot[0] = '\0';
                for (c = 0; c < MAX_TANKS; c++) {
                    if (udpServer.mapEventQueueDrops[c] == 0) continue;
                    p += snprintf(perSlot + p, sizeof(perSlot) - (size_t)p,
                                  " p%d=%u", c,
                                  (unsigned)udpServer.mapEventQueueDrops[c]);
                    if (p >= (int)sizeof(perSlot)) break;
                }
                mpDiagLog("[netstat] mapEventDrops total=%u%s", (unsigned)total, perSlot);
            }
        }
    }

    /* A tick with nothing to say still has sweep work to do while any slot is
     * culled — that slot's copy is behind by whatever it has not been sent,
     * and the sweep is the only thing that pays it back. */
    if (serverSimGetEventCount(sim) == 0 && serverSimGetMapEventCount(sim) == 0 &&
        serverSimGetShadowCulledMask(sim) == 0) return;

    for (c = 0; c < MAX_TANKS; c++) {
        WORLD cwx = 0, cwy = 0;
        BYTE clientMX = 0, clientMY = 0;
        bool hasPos;
        bool culled;

        if (!udpServer.clients[c].connected) continue;

        culled = serverSimIsShadowCulled(sim, (BYTE)c);

        /* The recipient's visibility set: its tank screen plus a screen for
         * each allied pillbox, base and tank its view policies allow. Built
         * once per client here because both the map-event cull below and the
         * best-effort fx cull further down want the same rects. */
        ViewportRect fxViewports[MAX_VIEWPORTS];
        int fxViewportCount = serverSimBuildViewports(sim, (BYTE)c, fxViewports,
                                                      MAX_VIEWPORTS);

        /* Always enqueue map events, even during map download. The map
         * snapshot was taken when the client joined, so any map changes
         * that happen during the download window must be queued here.
         * They'll be sent once downloadComplete becomes true (the send
         * path in transportUdpServerSend checks downloadComplete
         * separately). Without this, mid-game joiners permanently
         * desync because map events during download are lost. */
        {
            ClientEventQueue *mq = &udpServer.mapEventQueues[c];
            uint32_t dropped = 0;
            for (i = 0; i < (int)serverSimGetMapEventCount(sim); i++) {
                const GameEvent *mev = &serverSimGetMapEvents(sim)[i];
                /* Ground this client cannot see: not sent, and its copy of the
                 * terrain deliberately left holding the old square. That
                 * staleness is the record of what is owed, and the sweep below
                 * pays it if the client ever gets a view of the square. */
                if (culled && !inAnyViewport(fxViewports, fxViewportCount,
                                             mev->data[0], mev->data[1])) {
                    continue;
                }
                if (!eventQueueHasSpace(mq)) {
                    dropped++;
                    continue;
                }
                uint32_t idx = mq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                mq->buffer[idx].event = *mev;
                mq->buffer[idx].seq = mq->nextSeq;
                mq->nextSeq++;
                /* Queued, so this client is owed nothing more for the square:
                 * write it into that slot's copy. Tied to the enqueue on
                 * purpose — a drop above leaves the copy stale and the sweep
                 * heals it. */
                if (culled) {
                    serverSimShadowApplySlot(sim, (BYTE)c, mev->data[0],
                                             mev->data[1], mev->data[2]);
                }
            }
            if (dropped > 0) {
                udpServer.mapEventQueueDrops[c] += dropped;
                fprintf(stderr, "[UDP SERVER] Map event queue full for client %d, dropping %u events\n",
                        c, (unsigned)dropped);
            }
        }

        /* Game events (sounds, kills, etc.) only matter once the client
         * is in-game with a loaded map — skip if still downloading. */
        if (!udpServer.mapDownload[c].downloadComplete) continue;

        /* Catch-up sweep: compare this slot's copy of the terrain against the
         * live map inside the rects above and queue whatever it is behind on.
         * One slot per tick on a stride keeps the per-tick cost flat. Held off
         * while a resync is in flight — that transfer carries the slot's copy
         * whole, and its queue cut would throw the corrections away anyway. */
        if (culled && !udpServer.mapDownload[c].resyncInProgress &&
            (serverSimGetTick(sim) % MAP_SWEEP_STRIDE) ==
                (uint32_t)(c % MAP_SWEEP_STRIDE)) {
            ClientEventQueue *mq = &udpServer.mapEventQueues[c];
            GameEvent sweep[MAP_SWEEP_MAX_EVENTS];
            uint32_t depth = mq->nextSeq - mq->ackedSeq;
            int space = (depth >= (uint32_t)RELIABLE_EVENT_BUFFER_SIZE)
                            ? 0
                            : (int)((uint32_t)RELIABLE_EVENT_BUFFER_SIZE - depth);
            int want = (space < MAP_SWEEP_MAX_EVENTS) ? space : MAP_SWEEP_MAX_EVENTS;
            int got, k;
            /* Asking for no more than the queue holds is what makes the sweep's
             * write-through safe: every event it returns is queued below. */
            if (want > 0) {
                got = serverSimShadowSweep(sim, (BYTE)c, fxViewports,
                                           fxViewportCount, sweep, want);
                for (k = 0; k < got; k++) {
                    uint32_t idx = mq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                    mq->buffer[idx].event = sweep[k];
                    mq->buffer[idx].seq = mq->nextSeq;
                    mq->nextSeq++;
                }
            }
        }

        hasPos = serverSimGetTankState(sim, (BYTE)c, &cwx, &cwy);
        if (hasPos) {
            clientMX = (BYTE)(cwx >> 8);
            clientMY = (BYTE)(cwy >> 8);
        }

        /* Client's closest neutral/allied base drives the arrival push; the
         * per-base stock cull below keeps stock for every neutral/allied base
         * within this same send range, not just the closest. */
        WORLD stockRange = serverSimClosestBaseSendRange(sim, (BYTE)c);
        BYTE closestBase = BASE_NOT_FOUND;
        if (hasPos) {
            closestBase = basesGetClosestForPlayer(serverSimGetGameSim(sim), (BYTE)c, cwx, cwy, stockRange);
        }

        /* On arrival (closest base changed) push that base's current stock
         * immediately so ammo appears at once instead of lagging to the next
         * full-sync. Consuming the change here means the later snapshot build
         * for this same UDP slot sees no change. */
        {
            GameEvent arrivalEv;
            if (serverSimTakeClosestBaseStock(sim, (BYTE)c, closestBase, &arrivalEv)) {
                uint8_t abuf[GAME_EVENT_MAX_WIRE_SIZE];
                int alen = packGameEvent(abuf, &arrivalEv);
                channelSendBestEffort(&udpServer.channelMux[c], CHANNEL_GAME_EFFECT,
                                      abuf, (uint16_t)alen);
            }
        }

        /* Best-effort explosions are culled to the recipient's tank +
         * owned/allied pillbox viewports, matching the snapshot cull —
         * fxViewports is the set built at the top of this client's pass.
         * Sounds are culled by distance instead, so the rect set has no say in
         * what a recipient hears. */

        /* Pass 1: the closest sound of each type for this client, shaped as
         * a tier and a bearing. soundPickOffer holds every rule about who
         * hears what, shared with the snapshot builder. keepSquare is false
         * here whatever the slot is flagged: a wire recipient is never trusted
         * with the square. */
        SoundPick pick;
        int s;
        soundPickInit(&pick);
        if (hasPos) {
            for (i = 0; i < (int)serverSimGetEventCount(sim); i++) {
                soundPickOffer(&pick, &serverSimGetEvents(sim)[i], (BYTE)c,
                               clientMX, clientMY, false);
            }
        }

        /* Pass 2: route non-sound game events, then deduplicated sounds.
         * Each event goes to the reliable game channel (CHANNEL_GAME) if
         * gameEventIsReliable, otherwise to the best-effort channel
         * (CHANNEL_GAME_EFFECT). Sounds are all best-effort. */
        for (i = 0; i < (int)serverSimGetEventCount(sim); i++) {
            uint8_t evType = serverSimGetEvents(sim)[i].type;
            if (!soundEventIsSound(evType)) {
                /* Per-recipient working copy so a non-closest dead base's stock
                 * event can be reshaped (armour-only) without mutating the
                 * shared event; forceReliable promotes that copy to the
                 * reliable channel. */
                GameEvent evToSend = serverSimGetEvents(sim)[i];
                bool forceReliable = false;
                /* Filter EVENT_MINE_VISIBLE: tank mines (bit 7 set) go to all,
                 * LGM mines go only to the placer and their allies */
                if (evType == EVENT_MINE_VISIBLE) {
                    BYTE sourcePlayer = serverSimGetEvents(sim)[i].data[2];
                    if (sourcePlayer & 0x80) {
                        /* Tank mine — broadcast to all */
                    } else {
                        /* LGM mine — only placer and allies */
                        if (c != sourcePlayer && !playersIsAllie(&serverSimGetGameSim(sim)->plyrs, (BYTE)c, sourcePlayer)) {
                            continue;
                        }
                    }
                }
                /* Distance-cull explosion events */
                if (evType == EVENT_EXPLOSION && hasPos) {
                    if (!inAnyViewport(fxViewports, fxViewportCount,
                                       serverSimGetEvents(sim)[i].data[0],
                                       serverSimGetEvents(sim)[i].data[1])) continue;
                }
                /* Base stock is culled to neutral/allied bases. Exception: a
                 * dead enemy base (armour <= MIN_ARMOUR_CAPTURE) is delivered to
                 * non-friendly recipients too — armour only, with shells/mines
                 * zeroed so its reserve stays hidden — on the reliable channel,
                 * so the shooter unblocks the now-drivable tile promptly and the
                 * one-shot transition can't be dropped. */
                if (evType == EVENT_BASE_STOCK) {
                    BYTE bIdx = serverSimGetEvents(sim)[i].data[0];
                    GameSim *gs = serverSimGetGameSim(sim);
                    BYTE bOwner = (*gs->bs).item[bIdx].owner;
                    bool bFriendly = (bOwner == NEUTRAL) || (bOwner == (BYTE)c) ||
                                     playersIsAllie(&gs->plyrs, bOwner, (BYTE)c);
                    if (!bFriendly) {
                        if (serverSimGetEvents(sim)[i].data[1] <= MIN_ARMOUR_CAPTURE) {
                            evToSend.data[2] = 0;
                            evToSend.data[3] = 0;
                            forceReliable = true;
                        } else {
                            continue;
                        }
                    }
                }
                /* A pill this recipient cannot see keeps the square it was last
                 * given, rewritten into its own copy of the event rather than
                 * dropped — the event is the only carrier for that pill's
                 * armour, owner and in-tank flag between full syncs. */
                if (evType == EVENT_PILL_UPDATE) {
                    serverSimFogPillUpdateEvent(sim, (BYTE)c, &evToSend,
                                                fxViewports, fxViewportCount);
                }
                uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
                int evLen = packGameEvent(evBuf, &evToSend);
                if (forceReliable || gameEventIsReliable(evType)) {
                    if (!channelSend(&udpServer.channelMux[c], CHANNEL_GAME,
                                     evBuf, (uint16_t)evLen)) {
                        /* Channel window full — defer the disconnect off the
                         * event loop, mirroring the control-queue overflow path:
                         * set the deferred-removal flag (drained at a safe point
                         * by transportUdpServerDrainPendingRemovals) and stop. The
                         * flag guard keeps a re-hit from spamming the log. */
                        if (!udpServer.pendingSimRemove[c]) {
                            WB_LOG_ERROR(WB_LOG_CAT_NET,
                                         "game channel overflow for slot %d, deferring disconnect",
                                         c);
                            udpServer.pendingSimRemove[c] = true;
                        }
                        break;
                    }
                } else {
                    /* Ephemeral event — best-effort: never blocks, never
                     * disconnects on overflow (drops oldest). */
                    channelSendBestEffort(&udpServer.channelMux[c],
                                          CHANNEL_GAME_EFFECT, evBuf,
                                          (uint16_t)evLen);
                }
            }
        }
        for (s = 0; s < SOUND_PICK_TYPES; s++) {
            if (pick.has[s]) {
                uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
                int evLen = packGameEvent(evBuf, &pick.ev[s]);
                /* Sounds are ephemeral — best-effort: never blocks, never
                 * disconnects on overflow (drops oldest). */
                channelSendBestEffort(&udpServer.channelMux[c],
                                      CHANNEL_GAME_EFFECT, evBuf,
                                      (uint16_t)evLen);
            }
        }
    }
}

/* Send snapshots and check timeouts */
void transportUdpServerSend(ServerSim *sim) {
    int i;

    if (!udpServer.running) return;

    /* Ahead of the snapshot loop below, so a voice frame received this tick
     * rides this tick's snapshot trailer rather than waiting for the next. */
    serverPumpVoice(sim);

    /* Broadcast snapshots to connected clients that have finished map download */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;

        /* Begin an armed map transfer when the bulk channel is idle and fire
         * completion once the peer has acked it through (drives both the join
         * download and a live resync). */
        serverServiceMapTransfer(i);

        if (!udpServer.mapDownload[i].downloadComplete) {
            /* Still downloading the map: stream it on CHANNEL_BULK. Snapshots
             * are gated until complete, so this standalone carrier is the only
             * server->client bulk path — including for a mid-game joiner while
             * the server is Running (the snapshot trailer that carries the bulk
             * stream in other states is itself gated behind downloadComplete, so
             * without this a running joiner would deadlock). Emit several frames
             * a tick so a large map isn't throttled to ~one frame/tick; the
             * unacked window bounds the bytes actually in flight. */
            int frames;
            bulkSenderPump(&udpServer.bulkSend[i], &udpServer.channelMux[i]);
            channelTick(&udpServer.channelMux[i], udpServer.tickCount,
                        udpServer.clients[i].pingMs);
            for (frames = 0; frames < MAP_DOWNLOAD_FRAMES_PER_TICK; frames++) {
                uint8_t cbuf[UDP_MAX_PAYLOAD];
                int frameLen = channelBuildFrame(
                    &udpServer.channelMux[i], cbuf + PACKET_HEADER_SIZE,
                    UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
                if (frameLen <= 2) break;   /* nothing left to carry this tick */
                packHeader(cbuf, PACKET_CHANNEL,
                           udpServer.clients[i].outSequence++);
                srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen,
                          &udpServer.clients[i].addr);
            }
            continue;
        }

        serverSendSnapshot(sim, i);
    }

    /* Seed connected spectators from the delayed ring and drain the seed over
     * CHANNEL_BULK (mirrors the per-client map-download carrier above). */
    serverServiceSpectators(sim);

    transportUdpServerCheckTimeouts(sim);
}

/* Check for client timeouts — call from any server state (lobby, running, etc.) */
void transportUdpServerCheckTimeouts(ServerSim *sim) {
    int i;

    if (!udpServer.running) return;

    /* Outside a running game this is the once-per-tick path, so voice is
     * carried from here — ahead of the standalone PACKET_CHANNEL frames
     * built below, which are its only carrier in the lobby. While running,
     * transportUdpServerSend has already pumped it before the snapshots. */
    if (serverSimGetState(sim) != serverStateRunning) {
        serverPumpVoice(sim);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;

        /* Service the parallel channel layer once per tick.  During running
         * the snapshot trailer (serverSendSnapshot) is the carrier, so this
         * only ticks + carries when snapshots aren't flowing (lobby / countdown
         * / gameover — transportUdpServerSend isn't called there): begin/complete
         * an armed map transfer, tick the mux, and emit standalone PACKET_CHANNEL
         * frames carrying the channel data. A quiet lobby builds one empty frame
         * and suppresses it; a map download in lobby has a backlog, so emit up to
         * MAP_DOWNLOAD_FRAMES_PER_TICK frames (each build is destructive, so the
         * loop stops as soon as a frame comes back empty). */
        if (serverSimGetState(sim) != serverStateRunning) {
            int frames;
            serverServiceMapTransfer(i);
            bulkSenderPump(&udpServer.bulkSend[i], &udpServer.channelMux[i]);
            channelTick(&udpServer.channelMux[i], udpServer.tickCount,
                        udpServer.clients[i].pingMs);
            for (frames = 0; frames < MAP_DOWNLOAD_FRAMES_PER_TICK; frames++) {
                uint8_t cbuf[UDP_MAX_PAYLOAD];
                int frameLen = channelBuildFrame(
                    &udpServer.channelMux[i], cbuf + PACKET_HEADER_SIZE,
                    UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
                if (frameLen <= 2) break;   /* nothing (more) to carry this tick */
                packHeader(cbuf, PACKET_CHANNEL,
                           udpServer.clients[i].outSequence++);
                srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen,
                          &udpServer.clients[i].addr);
            }
        }

        /* Anonymous-fallback for a deferred WBN PLAYER_JOIN: the joiner's
         * reauth never landed within the grace window (direct-IP, not
         * signed in, or WBN unreachable), so announce the join un-keyed. */
        if (wbnJoinOnTick(&udpServer.clients[i].wbnJoin, udpServer.tickCount)) {
            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                               (BYTE)i, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
        }

        if (udpServer.tickCount - udpServer.clients[i].lastReceivedTick
            > CLIENT_TIMEOUT_TICKS) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                "timeout: slot=%d name='%s' tickCount=%u lastReceived=%u "
                "diff=%u > CLIENT_TIMEOUT_TICKS=%d -> disconnect",
                i, udpServer.clients[i].playerName,
                (unsigned)udpServer.tickCount,
                (unsigned)udpServer.clients[i].lastReceivedTick,
                (unsigned)(udpServer.tickCount - udpServer.clients[i].lastReceivedTick),
                (int)CLIENT_TIMEOUT_TICKS);
            mpDiagLog("[srv] TIMEOUT(no-traffic) slot=%d diff=%u CLIENT_TIMEOUT_TICKS=%d -> disconnect",
                      i,
                      (unsigned)(udpServer.tickCount - udpServer.clients[i].lastReceivedTick),
                      (int)CLIENT_TIMEOUT_TICKS);
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (serverSimIsLobbyEnabled(sim) &&
                (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown)) {
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
        }
    }

    /* Seed connected spectators outside running too (lobby / countdown /
     * gameover — transportUdpServerSend isn't called there, so it can't drive
     * this). When running, transportUdpServerSend already serviced spectators
     * before calling here, so this is gated off to avoid a double service. */
    if (serverSimGetState(sim) != serverStateRunning) {
        serverServiceSpectators(sim);
    }

    /* Age out idle spectators. This only frees a slot whose viewer has gone
     * silent; the seed/feed servicing happens in serverServiceSpectators. */
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (!udpServer.spectators[i].connected) continue;
        if (udpServer.tickCount - udpServer.spectators[i].lastReceivedTick
            > CLIENT_TIMEOUT_TICKS) {
            mpDiagLog("[srv] SPECTATOR TIMEOUT idx=%d diff=%u CLIENT_TIMEOUT_TICKS=%d",
                      i,
                      (unsigned)(udpServer.tickCount - udpServer.spectators[i].lastReceivedTick),
                      (int)CLIENT_TIMEOUT_TICKS);
            serverDisconnectSpectator(sim, i, false);
        }
    }
}


int transportUdpServerGetClientCount(void) {
    int count = 0;
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) count++;
    }
    return count;
}

int transportUdpServerGetSpectatorCount(void) {
    int count = 0;
    int i;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (udpServer.spectators[i].connected) count++;
    }
    return count;
}

uint16_t transportUdpServerGetClientPing(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) return 0;
    if (!udpServer.clients[playerNum].connected) return 0;
    return udpServer.clients[playerNum].pingMs;
}

bool transportUdpServerGetClientAddrStr(BYTE playerNum, char *out, size_t outLen) {
    if (out == NULL || outLen == 0) return false;
    out[0] = '\0';
    if (playerNum >= MAX_TANKS || !udpServer.clients[playerNum].connected) {
        return false;
    }
    const struct sockaddr_in *a = &udpServer.clients[playerNum].addr;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(out, outLen, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
    return true;
}

/* Set or clear the server's "locked to new players" state and announce the
 * change to every connected, download-complete client with a reliable
 * EVENT_SERVER_MSG on CHANNEL_GAME — the same path the per-tick game-event
 * producer uses (transportUdpServerDrainEvents).
 *
 * Precondition: the caller must hold threadsMutex.  The notice goes out via
 * channelSend, which mutates per-client channelMux state that is otherwise only
 * touched on the tick; the mutex is what serializes the two.  All callers
 * comply — the servermain console wraps each call (lock/unlock/alarm), and the
 * server_sim auto-lock/unlock callers run inside serverInstanceTick.  The
 * assert no-ops in bare logic unit tests that never start the threading system
 * (no tick thread to race, and no client is connected there to send to). */
void transportUdpServerSetLock(ServerSim *sim, bool locked) {
    (void)sim;
    SDL_assert(!threadsContextActive() || threadsCurrentlyHoldsMutex());
    if (udpServer.gameLocked == locked) return;
    udpServer.gameLocked = locked;
    serverSimConsoleMessage(locked
        ? "This game is now locked to new players (server lock)"
        : "This game is now unlocked to new players (server unlock)");
    winboloNetSendLock(locked);
    /* Announce on the reliable game channel.  serverSimAddEvent() won't do —
     * the sim's per-tick event buffer is cleared at the start of each tick and
     * this runs between ticks — so pack the event once and channelSend it onto
     * CHANNEL_GAME per connected, download-complete client, exactly as the
     * per-tick producer does. */
    {
        GameEvent ev;
        uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
        int evLen;
        int c;
        ev.type = EVENT_SERVER_MSG;
        memset(ev.data, 0, sizeof(ev.data));
        ev.data[0] = locked ? SERVER_MSG_GAME_LOCKED : SERVER_MSG_GAME_UNLOCKED;
        evLen = packGameEvent(evBuf, &ev);
        for (c = 0; c < MAX_TANKS; c++) {
            if (!udpServer.clients[c].connected) continue;
            if (!udpServer.mapDownload[c].downloadComplete) continue;
            if (!channelSend(&udpServer.channelMux[c], CHANNEL_GAME,
                             evBuf, (uint16_t)evLen)) {
                /* Window full — defer the disconnect off this path, mirroring
                 * the per-tick game-channel overflow handling.  The flag guard
                 * keeps a re-hit from spamming the log. */
                if (!udpServer.pendingSimRemove[c]) {
                    WB_LOG_ERROR(WB_LOG_CAT_NET,
                                 "game channel overflow for slot %d, deferring disconnect",
                                 c);
                    udpServer.pendingSimRemove[c] = true;
                }
            }
        }
    }
}

bool transportUdpServerGetLock(void) {
    return udpServer.gameLocked;
}

void transportUdpServerSendServerMessage(const char *message) {
    /* Public API: external callers (servermain console "say", saveMap
     * announcement, server_lifecycle pendingWinMessage) pass arbitrary
     * pre-rendered English strings.  These don't have dedicated langids
     * yet, so route through the legacy English passthrough wire format.
     * Sim handle comes from serverSimGetActive() — the public signature
     * doesn't carry it, matching the pattern other public entry points
     * in this TU use when they need the active sim. */
    ServerSim *sim = serverSimGetActive();
    if (sim == NULL) return;
    serverSendServerEnglishBroadcast(sim, message);
}

void transportUdpServerPrintStatus(bool toFile) {
    int i;
    FILE *fp = NULL;

    if (toFile) {
        fp = fopen("status.txt", "w");
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        fprintf(stdout, "%s (slot %d, ping %dms)\n",
                udpServer.clients[i].playerName, i,
                udpServer.clients[i].pingMs);
        if (fp != NULL) {
            fprintf(fp, "%s (slot %d, ping %dms)\n",
                    udpServer.clients[i].playerName, i,
                    udpServer.clients[i].pingMs);
        }
    }

    if (fp != NULL) {
        fclose(fp);
    }
}

#ifdef WB_FUZZ
/* ================================================================
 * Fuzz-only dispatcher seam (hardening plan §1.2, tier 2)
 *
 * Drives serverProcessPacket directly with attacker-controlled bytes —
 * no socket, no recv thread. serverProcessPacket and the file-static
 * `udpServer` it mutates have internal linkage, so this seam must live in
 * the same TU. Compiled only under -DWB_FUZZ (the dedicated fuzz build);
 * every shipping build leaves these symbols out entirely.
 *
 * The harness owns the ServerSim lifetime and calls Init once before
 * feeding packets. State accumulates across inputs by design — that is the
 * standard libFuzzer persistent-target pattern and explores deeper handler
 * paths than a per-input reset would.
 * ================================================================ */

/* The fixed peer the dispatcher seam attributes every fuzz datagram to. The
 * warm-up JOIN below and transportUdpServerFuzzProcessPacket share this exact
 * (addr,port) so serverFindClient resolves a fuzz datagram to the pre-connected
 * slot — change one without the other and the post-JOIN handlers go dark. */
static void fuzzServerPeerAddr(struct sockaddr_in *from) {
    memset(from, 0, sizeof(*from));
    from->sin_family = AF_INET;
    from->sin_addr.s_addr = htonl(0x7f000001u); /* 127.0.0.1 */
    from->sin_port = htons((unsigned short)40000);
}

/* Drive one real JOIN to completion so the dispatcher starts with a connected
 * client. Without it, serverFindClient() returns -1 for the fuzz peer and every
 * post-JOIN handler (COMMAND_TICK, INPUT, CONTROL_ACK, the reliable event
 * loops, map reassembly) bails at its `clientIdx < 0` guard — i.e. the hand-
 * written count-loops this target exists to reach stay unfuzzed.
 *
 * The join is cookie-gated: normally the joiner echoes a cookie from a prior
 * PACKET_JOIN_CHALLENGE, but that challenge reply is a no-op over the seam's
 * INVALID_SOCKET, so a two-pass handshake can't observe it. Instead we mint the
 * cookie directly (same secret/window the acceptor checks) and submit a single
 * well-formed JOIN_REQUEST through the very dispatch path the fuzzer drives.
 *
 * The body layout mirrors serverHandleJoinRequest's reader exactly. One subtle
 * ordering contract: the optional 2-byte fallbackCountry is read *before* the
 * trailing cookie, so it must be present here — omit it and the handler eats
 * the cookie's first two bytes as a country code and the address proof fails. */
static bool fuzzServerWarmJoin(ServerSim *sim) {
    uint8_t pkt[PACKET_HEADER_SIZE + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3 +
                WBN_JOIN_KEY_WIRE_LEN + 1 /*flags*/ + 2 /*type,hints*/ +
                2 /*country*/ + JOIN_COOKIE_LEN];
    struct sockaddr_in from;
    size_t pos;

    fuzzServerPeerAddr(&from);
    memset(pkt, 0, sizeof(pkt));
    packHeader(pkt, PACKET_JOIN_REQUEST, 0);
    pos = PACKET_HEADER_SIZE;

    /* Player name (NUL-padded, validator-clean ASCII). */
    memcpy(pkt + pos, "FuzzPeer", 8);
    pos += PACKET_MAX_PLAYER_NAME;

    /* Password: empty — the fuzz ServerSim is created without one. */
    pos += MAP_STR_SIZE;

    /* Protocol version triple: the CMake-defined values this server gates on,
     * so it always matches and can't take the version-reject branch. */
    pkt[pos++] = (uint8_t)BOLO_VERSION_MAJOR;
    pkt[pos++] = (uint8_t)BOLO_VERSION_MINOR;
    pkt[pos++] = (uint8_t)BOLO_VERSION_REVISION;

    /* WBN join key empty → joins as a non-WBN player (skips token verify). */
    pos += WBN_JOIN_KEY_WIRE_LEN;

    pos += 1; /* flags: 0 (no rejoin, won't-authenticate) */
    pos += 2; /* clientType, clientHints: 0 (unknown client) */

    /* fallbackCountry — present so the cookie that follows stays aligned. */
    pkt[pos++] = 'X';
    pkt[pos++] = 'X';

    /* Address-proof cookie for the current window. Fails closed if the CSPRNG
     * secret is unavailable; we then skip the warm-up and the target degrades
     * to its pre-warm (JOIN-gated) behaviour rather than connecting. */
    if (!serverCookieCompute(&from, serverCookieCurrentWindow(), pkt + pos)) {
        return false;
    }
    pos += JOIN_COOKIE_LEN;

    /* serverProcessPacket's sim-mutating handlers assert the server mutex is
     * held; take it here exactly as serverInstanceTick does in production. */
    threadsWaitForMutex();
    serverProcessPacket(sim, pkt, (int)pos, &from);
    threadsReleaseMutex();

    /* Caller decides how to treat a join that didn't connect: init aborts (a
     * dead gate from the first input is a build-level regression), the per-
     * input re-arm shrugs and lets the input bounce off the gate. */
    return serverFindClient(&from) >= 0;
}

/* Minimal server context: mirrors the non-socket, non-thread portion of
 * transportUdpServerCreate. sock stays INVALID_SOCKET so srvSendTo's
 * underlying udpSendTo is a no-op and no datagrams leave the process. After
 * the context is up we drive one JOIN so the dispatcher begins with a
 * connected client (see fuzzServerWarmJoin). */
void transportUdpServerFuzzInit(ServerSim *sim) {
    int i;
    /* The seam drives serverProcessPacket directly (no recv thread), but its
     * sim-mutating handlers assert the server mutex is held. Create the mutex
     * here so the seam can take it around each serverProcessPacket call, the
     * way serverInstanceTick holds it in production. Idempotent. */
    threadsCreate(true);
    memset(&udpServer, 0, sizeof(udpServer));
    memset(punchQueue, 0, sizeof(punchQueue));
    udpServer.sock = INVALID_SOCKET;
    udpServer.running = true;
    udpServer.tickCount = 0;
    udpServer.uploadMaxFiles        = 64;
    udpServer.uploadMaxStorageBytes = 8u * 1024u * 1024u;
    udpServer.compressedMapSize = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        udpServer.clients[i].connected = false;
        udpServer.clients[i].controlSub = SUBSCRIBER_HANDLE_INVALID;
        memset(&udpServer.mapDownload[i], 0, sizeof(ClientMapDownload));
        udpServer.controlSyncInProgress[i] = false;
        serverSimSetShadowCulled(sim, (BYTE)i, false);
    }
    netImpairInit(&srvImpairIn);
    netImpairInit(&srvImpairOut);

    /* Open the JOIN gate: leave one client connected at the fuzz peer address
     * so the post-JOIN dispatcher paths are reachable from the first input. A
     * join that doesn't connect here means the target is neutered before a
     * single input runs — a build-level regression, so fail loudly. The
     * -runs=0 corpus-replay ctest exercises this path on every build, turning
     * a silent dead gate into a red test. */
    if (!fuzzServerWarmJoin(sim)) {
        fprintf(stderr, "fuzzServerWarmJoin: peer not connected after JOIN — "
                        "server_dispatch would fuzz the dead JOIN gate\n");
        abort();
    }
}

/* One datagram, as if received on the game socket from a LAN peer. The buffer
 * is heap-allocated to the EXACT input length (handlers need it mutable, and
 * an exact size lets ASan's redzone catch any read/write past len — an
 * oversized buffer would mask the very over-reads this target hunts). */
void transportUdpServerFuzzProcessPacket(ServerSim *sim,
                                         const uint8_t *data, size_t size) {
    uint8_t *buf;
    struct sockaddr_in from;
    if (size == 0 || size > 65535) return;
    fuzzServerPeerAddr(&from); /* same peer the warm-up JOIN registered */

    /* Keep the post-JOIN surface live across inputs. State persists by design,
     * so an earlier input that disconnected the peer (a QUIT, an idle-timeout
     * path, a handler that drops the client) would otherwise leave every later
     * input bouncing off the JOIN gate. Re-arm if needed — the input that did
     * the disconnect already exercised that path; this just restores the
     * connected-client context the next input wants to fuzz. */
    if (serverFindClient(&from) < 0) {
        (void)fuzzServerWarmJoin(sim);
    }

    buf = (uint8_t *)malloc(size);
    if (buf == NULL) return;
    memcpy(buf, data, size);
    /* Hold the server mutex across dispatch, mirroring serverInstanceTick — the
     * sim-mutating handlers (COMMAND_TICK → serverSimApplyCommand, etc.) assert
     * it. The re-arm above self-locks, so this is a fresh, non-nested region. */
    threadsWaitForMutex();
    serverProcessPacket(sim, buf, (int)size, &from);
    /* Mirror serverInstanceTick: drain deferred overflow-disconnects after
     * recv, outside any publish, under the same mutex. Keeps the fuzz server's
     * state consistent and exercises the deferred-disconnect path
     * (serverDisconnectClient + leave broadcast + serverSimRemovePlayer) that
     * the control-queue-overflow handler now defers here. */
    transportUdpServerDrainPendingRemovals(sim);
    threadsReleaseMutex();
    free(buf);
}
#endif /* WB_FUZZ */

