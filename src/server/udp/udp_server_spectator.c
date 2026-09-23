/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Spectator
 *Filename:      udp_server_spectator.c
 *Author:        John Morrison
 *Purpose:
 *  The server transport's tankless spectator support,
 *  split out of transport_udp_server.c.
 *    - Accepting a viewer into a spectators[] slot and
 *      releasing it again on leave or timeout.
 *    - The delayed-view feed: the ring keyframe seed, the
 *      forward record stream, and the cold-start countdown
 *      the viewer waits on while the ring fills.
 *    - The drop-by-default control-event subscriber a
 *      live-lobby viewer reads, and the live-lobby map it
 *      streams for its preview.
 *    - The spectator roster row and its broadcast to
 *      players and viewers alike.
 *********************************************************/

#include <stdio.h>   /* snprintf */
#include <stdlib.h>  /* malloc, free */
#include <string.h>  /* memcpy, memset, strlen, strncpy */

#include "transport_udp_internal.h"        /* packHeader, packU16, packU32,
                                            * PACKET_HEADER_SIZE, UDP_MAX_PAYLOAD,
                                            * SDL_assert */
#include "transport_udp_server_internal.h" /* udpServer, SpectatorConn, srvSendTo,
                                            * mpDiagCtrlName, udpClientDeliverControl,
                                            * serverSendJoinReject, serverNextConnId,
                                            * packConnId, MAP_DOWNLOAD_FRAMES_PER_TICK */
#include "transport_udp.h"   /* MAX_SPECTATORS, SubscriberHandle,
                              * SUBSCRIBER_HANDLE_INVALID, SPEC_CTRL_COUNTDOWN,
                              * SPEC_CTRL_COUNTDOWN_LEN */
#include "netpacks.h"        /* PACKET_JOIN_ACCEPT, PACKET_CHANNEL,
                              * PACKET_MAX_PLAYER_NAME, MAP_DOWNLOAD_MAX_SIZE */
#include "global.h"          /* BYTE, MAX_TANKS, and BOLO_STATIC_ASSERT via
                              * platform_types.h */
#include "player_flags.h"    /* PLAYER_CLIENT_HINT_MASK */
#include "game_sim.h"        /* via lang.h, STR_REJECT_SERVER_FULL */
#include "server_sim.h"      /* ServerSim, ServerState, serverSimGetState/GetTick,
                              * serverSimGetCompressedMap, serverSimGetMaxSpectators,
                              * serverSimGetSpecDelayTicks, serverSimReplayLobbyChat,
                              * serverSim{Register,Unregister}Subscriber */
#include "server_sim_internal.h" /* LOBBY_CHAT_BUFFER_MAX — sim co-owner */
#include "control_event.h"   /* ControlEvent, CTRL_*, CHAT_BODY_MAX,
                              * GAME_VOTE_KIND_SURRENDER, and ClientSpectatorSlot
                              * via client_sim.h */
#include "transport_control_codec.h" /* ControlEncodeBodyFn, ENCODE_OK,
                                      * transportControlCodecBodyEncoder,
                                      * MAX_CONTROL_PACKET */
#include "channel_mux.h"     /* ChannelState, channelMuxInit, channelSend,
                              * channelResetSend, channelTick, channelBuildFrame,
                              * CHANNEL_CONTROL, CHANNEL_BULK, CHANNEL_CONTROL_SEG,
                              * CHANNEL_BULK_SEG */
#include "bulk_transfer.h"   /* BulkStreamHeader, bulkSenderInit/Reset/Begin/Busy/Pump,
                              * BULK_KIND_*, BULK_STREAM_HEADER_FIXED */
#include "spectator_ring.h"  /* SpectatorRing, SpectatorRingCursor,
                              * spectatorRingHeadSeq/OldestSeq/SeekDelayed/RecordAt,
                              * spectatorRingCursorKeyframe/SeedSeq,
                              * SPECTATOR_RING_OK, SPECTATOR_RING_COLD_START */
#include "log.h"             /* logAddEvent, log_SpectatorJoined, log_SpectatorLeft */
#include "../server_lifecycle.h"                /* serverInstanceGetSpectatorRing */
#include "../../common/wb_log.h"                /* WB_LOG_WARN */
#include "../../common/mp_diag_log.h"           /* mpDiagLog */
#include "../../winbolonet/winbolonet_core.h"   /* WINBOLONET_KEY_LEN */
#include "../../winbolonet/winbolonet_server.h" /* winboloNetSpectatorLeaveGame */

/* Find a connected spectator by source address. Returns the spectators[]
 * index or -1. Mirrors serverFindClient over the parallel array. */
int serverFindSpectator(const struct sockaddr_in *addr) {
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
void serverSendSpectatorAccept(int s, ServerSim *sim,
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
void serverArmSpectatorLobbyMap(int s, bool resetChannel) {
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
    /* The brains' lobby texts, which index into the list above. A spectator
     * reads the lobby's team chat, and a bot's announce line is a team-chat
     * line whose docs open when it is clicked — without these it would see
     * the line and find nothing behind it. */
    case CTRL_LOBBY_BRAIN_DOCS_CHUNK:
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
    /* A pillbox, base or start joining or leaving the map, and the liveness
     * masks a client is handed with a map: a spectator draws the map and
     * would otherwise keep drawing an item that has gone. */
    case CTRL_ENTITY_CHANGE:
    case CTRL_ENTITY_SYNC:
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
    /* A scenario's presentation, for the three that carry a recipient
     * pair: a viewer sees what is addressed to everyone and nothing
     * narrower. A spectator belongs to no team and holds no slot, so a
     * team- or player-addressed one never qualifies — the same reasoning
     * the CTRL_CHAT arm above uses, and the same answer the per-client
     * filter in udpClientDeliverControl reaches by comparing. */
    case CTRL_SCN_PANEL:
        allow = (evt->u.scnPanel.destTeam == 0 &&
                 evt->u.scnPanel.destPlayer == 0xFF);
        break;
    case CTRL_SCN_ANNOUNCE:
        allow = (evt->u.scnAnnounce.destTeam == 0 &&
                 evt->u.scnAnnounce.destPlayer == 0xFF);
        break;
    case CTRL_SCN_MARKER:
        allow = (evt->u.scnMarker.destTeam == 0 &&
                 evt->u.scnMarker.destPlayer == 0xFF);
        break;
    /* Scores are broadcast — a viewer reading a scenario's panel and
     * markers with a blank scoreboard beside them looks broken, the
     * same reason CTRL_STATS_SEED is allowed above. */
    case CTRL_SCN_SCORE:
    /* And the rules that scenario's manifest sets: the lobby line a viewer
     * already reads names the scenario, and this is what is behind it. As
     * public as the name and the description CTRL_LOBBY_SETTINGS carries to
     * the same viewer, and addressed to nobody. */
    case CTRL_SCENARIO_RULES:
        allow = true;
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
            /* Full backlog behind the window: drop and warn. Do NOT disconnect
             * from inside the deliver callback — there is no deferred-removal
             * path for spectators, and tearing the slot down here risks
             * reentrancy. */
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
void serverEnumSpectatorRoster(
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
void serverDisconnectSpectator(ServerSim *sim, int s, bool graceful) {
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

/* CHANNEL_CONTROL is a 64-deep reliable window with a bounded backlog behind
 * it; a cold-start countdown that ran for up to specDelayTicks at one send per
 * tick would pile up behind it for no reason. Resend the countdown status no
 * more than once every this many ticks (~2/s at 50 tick/s). */
#define SPEC_COUNTDOWN_RESEND_TICKS 25u

/* Arm/refresh a spectator's "spectating begins in X" countdown carrying
 * `remaining` ticks. Updates the state every tick (so a reader sees it track
 * toward zero) but only puts a SPEC_CTRL_COUNTDOWN on CHANNEL_CONTROL on first
 * entry and then once per SPEC_COUNTDOWN_RESEND_TICKS, so a long wait can't
 * fill its window and backlog. Shared by the cutover gate (delayed view not yet at the
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

void serverServiceSpectators(ServerSim *sim) {
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
