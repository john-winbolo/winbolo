/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Control
 *Filename:      udp_server_control.c
 *Author:        John Morrison
 *Purpose:
 *  Control-event delivery to one connected player, split
 *  out of transport_udp_server.c.
 *    - The per-recipient filter that decides what each
 *      client is allowed to see.
 *    - Queueing an accepted event onto that client's
 *      control channel.
 *    - The immediate flush of that channel the join path
 *      uses.
 *********************************************************/

#include <stddef.h>  /* size_t — the encoder's bodyLen */

#include "transport_udp_internal.h"        /* packHeader, packU16,
                                            * UDP_MAX_PAYLOAD */
#include "transport_udp_server_internal.h" /* udpServer, UdpServerClient, srvSendTo,
                                            * mpDiagCtrlName,
                                            * transportUdpServerFlushChannel,
                                            * udpClientDeliverControl */
#include "global.h"          /* BYTE, MAX_TANKS, PlayerBitMap */
#include "netpacks.h"        /* PACKET_CHANNEL, PACKET_HEADER_SIZE,
                              * GAME_VOTE_KIND_SURRENDER */
#include "client_command.h"  /* CHAT_DEST_IS_TEAM, CHAT_DEST_TEAM_OF */
#include "server_sim.h"      /* LobbyPlayer, serverSimGetActive,
                              * serverSimGetLobbyPlayer, serverSimGetState,
                              * serverStateRunning */
#include "control_event.h"   /* ControlEvent, CTRL_* */
#include "transport_control_codec.h" /* ControlEncodeBodyFn, ENCODE_OK,
                                      * transportControlCodecBodyEncoder */
#include "channel_mux.h"     /* channelSend, channelTick, channelBuildFrame,
                              * CHANNEL_CONTROL, CHANNEL_CONTROL_SEG */
#include "../../common/wb_log.h"      /* WB_LOG_ERROR, WB_LOG_CAT_NET */
#include "../../common/mp_diag_log.h" /* mpDiagLog */

/* Short name for a ControlEventType — diagnostic logging only. */
const char *mpDiagCtrlName(int type) {
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
    case CTRL_LOBBY_BRAIN_DOCS_CHUNK: return "LOBBY_BRAIN_DOCS_CHUNK";
    case CTRL_GAME_VOTE_STATE:  return "GAME_VOTE_STATE";
    case CTRL_SERVER_TEXT:      return "SERVER_TEXT";
    case CTRL_SHELL_DEATH:      return "SHELL_DEATH";
    case CTRL_CHANNEL_RESET:    return "CHANNEL_RESET";
    case CTRL_VIEW_TARGET:      return "VIEW_TARGET";
    case CTRL_VOICE_TALKING:    return "VOICE_TALKING";
    case CTRL_ENTITY_CHANGE:    return "ENTITY_CHANGE";
    case CTRL_ENTITY_SYNC:      return "ENTITY_SYNC";
    case CTRL_SIM_RULES:        return "SIM_RULES";
    case CTRL_SCN_PANEL:        return "SCN_PANEL";
    case CTRL_SCN_SCORE:        return "SCN_SCORE";
    case CTRL_SCN_ANNOUNCE:     return "SCN_ANNOUNCE";
    case CTRL_SCN_MARKER:       return "SCN_MARKER";
    case CTRL_SCENARIO_RULES:   return "SCENARIO_RULES";
    case CTRL_LOBBY_SCRIPT_LIST: return "LOBBY_SCRIPT_LIST";
    case CTRL_LOBBY_SCRIPT_SETTING: return "LOBBY_SCRIPT_SETTING";
    case CTRL_LOBBY_BRAIN_ANNOUNCE: return "LOBBY_BRAIN_ANNOUNCE";
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
    if (evt->type == CTRL_SERVER_TEXT &&
        evt->u.serverText.destPlayer != 0xFF) {
        /* Player-scoped server text (a scenario talking to one player): only
         * the addressed slot receives it. */
        if (client->playerNum != evt->u.serverText.destPlayer) {
            mpDiagLog("[srv] deliver FILTER slot=%d type=SERVER_TEXT "
                      "reason=not-addressed destPlayer=%d clientPlayerNum=%d",
                      idx, (int)evt->u.serverText.destPlayer,
                      (int)client->playerNum);
            return;
        }
    }
    if (evt->type == CTRL_SCN_PANEL || evt->type == CTRL_SCN_ANNOUNCE ||
        evt->type == CTRL_SCN_MARKER) {
        /* A scenario's presentation, addressed the way server text is:
         * destTeam 0 means everyone, destPlayer 0xFF means everyone.
         * CTRL_SCN_SCORE is deliberately not here — it is broadcast, and
         * its target says whose score it is, not who receives it. */
        uint8_t destTeam;
        uint8_t destPlayer;
        switch (evt->type) {
        case CTRL_SCN_PANEL:
            destTeam   = evt->u.scnPanel.destTeam;
            destPlayer = evt->u.scnPanel.destPlayer;
            break;
        case CTRL_SCN_ANNOUNCE:
            destTeam   = evt->u.scnAnnounce.destTeam;
            destPlayer = evt->u.scnAnnounce.destPlayer;
            break;
        default:
            destTeam   = evt->u.scnMarker.destTeam;
            destPlayer = evt->u.scnMarker.destPlayer;
            break;
        }
        if (destTeam != 0) {
            const LobbyPlayer *lp =
                serverSimGetLobbyPlayer(serverSimGetActive(), client->playerNum);
            if (!lp || lp->teamNumber != destTeam) {
                mpDiagLog("[srv] deliver FILTER slot=%d type=%s "
                          "reason=not-on-team destTeam=%d clientPlayerNum=%d",
                          idx, mpDiagCtrlName((int)evt->type),
                          (int)destTeam, (int)client->playerNum);
                return;
            }
        }
        if (destPlayer != 0xFF && client->playerNum != destPlayer) {
            mpDiagLog("[srv] deliver FILTER slot=%d type=%s "
                      "reason=not-addressed destPlayer=%d clientPlayerNum=%d",
                      idx, mpDiagCtrlName((int)evt->type),
                      (int)destPlayer, (int)client->playerNum);
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
     * matching the former send-time `enc == NULL` skip.  A send that finds
     * the window full waits in the channel's backlog; a full backlog means
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
