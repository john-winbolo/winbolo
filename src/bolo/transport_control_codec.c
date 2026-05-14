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
 *Name:          Transport Control Codec
 *Filename:      transport_control_codec.c
 *Author:        John Morrison
 *Purpose:
 *  Encoder table (keyed by ControlEventType) and decoder
 *  table (keyed by wire packet type) for ControlEvent.
 *
 *  All encoders return ENCODE_SKIP and all decoders return
 *  false in this scaffold revision; bodies are filled in as
 *  each variant migrates from its dedicated transportUdp
 *  broadcast helper to the per-client subscriber path.
 *********************************************************/

#include "transport_control_codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "control_event.h"
#include "netpacks.h"
#include "transport_udp_internal.h"

/* ================================================================
 * Encoders — one per ControlEventType variant with a wire form.
 *
 * Signature: (const ControlEvent *evt,
 *             const struct UdpServerClient *recipient,
 *             uint8_t *buf, size_t bufCap, size_t *outLen)
 *
 * Recipient context lets the encoder filter single-target events
 * (e.g. alliance request to a specific player) or stamp per-client
 * sequence numbers. Stubs ignore it and return ENCODE_SKIP.
 * ================================================================ */

static EncodeResult encodeAllianceRequest(const ControlEvent *evt,
                                          const struct UdpServerClient *recipient,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodeAllianceAccept(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodeAllianceLeave(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodePlayerJoin(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodePlayerName(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodeLobbySlot(const ControlEvent *evt,
                                    const struct UdpServerClient *recipient,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodeLobbySettings(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodeLobbyMapChange(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodeBalanceProposal(const ControlEvent *evt,
                                          const struct UdpServerClient *recipient,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    (void)recipient;
    const size_t needed = PACKET_HEADER_SIZE + MAX_TANKS;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_BALANCE_PROPOSAL, 0);
    memcpy(buf + PACKET_HEADER_SIZE, evt->u.balanceProposal.teamForSlot, MAX_TANKS);
    *outLen = needed;
    return ENCODE_OK;
}

static EncodeResult encodeMapSkipState(const ControlEvent *evt,
                                       const struct UdpServerClient *recipient,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    (void)recipient;
    const size_t needed = PACKET_HEADER_SIZE + MAX_TANKS;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_MAP_SKIP_STATE, 0);
    memcpy(buf + PACKET_HEADER_SIZE, evt->u.mapSkipState.votes, MAX_TANKS);
    *outLen = needed;
    return ENCODE_OK;
}

/* encodeGamePhase produces one of PACKET_COUNTDOWN, PACKET_GAME_START,
 * or PACKET_GAME_OVER based on evt->u.gamePhase.phase. */
static EncodeResult encodeGamePhase(const ControlEvent *evt,
                                    const struct UdpServerClient *recipient,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodeGameOver(const ControlEvent *evt,
                                   const struct UdpServerClient *recipient,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap; (void)outLen;
    return ENCODE_SKIP;
}

static EncodeResult encodeServerShutdown(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)evt; (void)recipient;
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_SERVER_SHUTDOWN, 0);
    *outLen = PACKET_HEADER_SIZE;
    return ENCODE_OK;
}

/* ================================================================
 * Decoders — keyed by wire packet type.
 *
 * Three thin decoders for the game-phase wire family
 * (PACKET_COUNTDOWN/GAME_START/GAME_OVER) keep the dispatcher's
 * lookup 1:1 with wire packets. decodeAllianceUpdate is the single
 * entry for PACKET_ALLIANCE_UPDATE; once implemented it will
 * inspect the sub-event byte and produce the matching
 * CTRL_ALLIANCE_REQUEST/ACCEPT/LEAVE event.
 * ================================================================ */

static bool decodeAllianceUpdate(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    (void)buf; (void)len; (void)outEvt;
    return false;
}

static bool decodePlayerJoin(const uint8_t *buf, size_t len,
                             ControlEvent *outEvt) {
    (void)buf; (void)len; (void)outEvt;
    return false;
}

static bool decodePlayerName(const uint8_t *buf, size_t len,
                             ControlEvent *outEvt) {
    (void)buf; (void)len; (void)outEvt;
    return false;
}

static bool decodeLobbySlot(const uint8_t *buf, size_t len,
                            ControlEvent *outEvt) {
    (void)buf; (void)len; (void)outEvt;
    return false;
}

static bool decodeLobbyMapChange(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    (void)buf; (void)len; (void)outEvt;
    return false;
}

static bool decodeBalanceProposal(const uint8_t *buf, size_t len,
                                  ControlEvent *outEvt) {
    if (len < MAX_TANKS) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_BALANCE_PROPOSAL;
    memcpy(outEvt->u.balanceProposal.teamForSlot, buf, MAX_TANKS);
    return true;
}

static bool decodeMapSkipState(const uint8_t *buf, size_t len,
                               ControlEvent *outEvt) {
    if (len < MAX_TANKS) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_MAP_SKIP_STATE;
    memcpy(outEvt->u.mapSkipState.votes, buf, MAX_TANKS);
    return true;
}

static bool decodeCountdown(const uint8_t *buf, size_t len,
                            ControlEvent *outEvt) {
    if (len < 1) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_GAME_PHASE;
    outEvt->u.gamePhase.phase = CTRL_PHASE_COUNTDOWN;
    outEvt->u.gamePhase.countdownSeconds = buf[0];
    return true;
}

static bool decodeGameStart(const uint8_t *buf, size_t len,
                            ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_GAME_PHASE;
    outEvt->u.gamePhase.phase = CTRL_PHASE_RUNNING;
    outEvt->u.gamePhase.countdownSeconds = 0;
    return true;
}

static bool decodeGameOver(const uint8_t *buf, size_t len,
                           ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_GAME_OVER;
    return true;
}

static bool decodeServerShutdown(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_SERVER_SHUTDOWN;
    return true;
}

/* ================================================================
 * Encoder lookup — indexed by ControlEventType. Variants without
 * a wire form leave NULL slots (CTRL_MAP_DOWNLOAD_COMPLETE is
 * client-internal — published by the client on its own bus when
 * the map download finishes — and never crosses the wire).
 * ================================================================ */

static const ControlEncodeFn s_encoders[CTRL_EVENT_TYPE_COUNT] = {
    [CTRL_ALLIANCE_REQUEST]   = encodeAllianceRequest,
    [CTRL_ALLIANCE_ACCEPT]    = encodeAllianceAccept,
    [CTRL_ALLIANCE_LEAVE]     = encodeAllianceLeave,
    [CTRL_PLAYER_JOIN]        = encodePlayerJoin,
    [CTRL_PLAYER_NAME]        = encodePlayerName,
    [CTRL_LOBBY_SLOT]         = encodeLobbySlot,
    [CTRL_LOBBY_SETTINGS]     = encodeLobbySettings,
    [CTRL_LOBBY_MAP_CHANGE]   = encodeLobbyMapChange,
    /* CTRL_MAP_DOWNLOAD_COMPLETE intentionally absent (NULL). */
    [CTRL_BALANCE_PROPOSAL]   = encodeBalanceProposal,
    [CTRL_MAP_SKIP_STATE]     = encodeMapSkipState,
    [CTRL_GAME_PHASE]         = encodeGamePhase,
    [CTRL_GAME_OVER]          = encodeGameOver,
    [CTRL_SERVER_SHUTDOWN]    = encodeServerShutdown,
};

ControlEncodeFn transportControlCodecEncoder(ControlEventType type) {
    if (type < 0 || type >= CTRL_EVENT_TYPE_COUNT) return NULL;
    return s_encoders[type];
}

ControlDecodeFn transportControlCodecDecoder(uint16_t packetType) {
    switch (packetType) {
        case PACKET_ALLIANCE_UPDATE:  return decodeAllianceUpdate;
        case PACKET_PLAYER_JOINED:    return decodePlayerJoin;
        case PACKET_NAME_CHANGE:      return decodePlayerName;
        case PACKET_LOBBY_UPDATE:     return decodeLobbySlot;
        case PACKET_LOBBY_MAP_CHANGE: return decodeLobbyMapChange;
        case PACKET_BALANCE_PROPOSAL: return decodeBalanceProposal;
        case PACKET_MAP_SKIP_STATE:   return decodeMapSkipState;
        case PACKET_COUNTDOWN:        return decodeCountdown;
        case PACKET_GAME_START:       return decodeGameStart;
        case PACKET_GAME_OVER:        return decodeGameOver;
        case PACKET_SERVER_SHUTDOWN:  return decodeServerShutdown;
        default:                      return NULL;
    }
}
