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
 *Name:          Transport Control Codec
 *Filename:      transport_control_codec.c
 *Author:        John Morrison
 *Purpose:
 *  Encoder table (keyed by ControlEventType) and decoder
 *  table (keyed by wire packet type) for ControlEvent.
 *
 *  Each variant has two flavors:
 *    - encodeFooBody / decodeFooBody — body-only, no
 *      PacketHeader. Drives the reliable carrier path
 *      indexed by ControlEventType (s_bodyEncoders /
 *      s_bodyDecoders).
 *    - encodeFoo / decodeFoo — legacy full-packet form
 *      used by the still-direct-send code paths. Each
 *      encoder wrapper packs the PacketHeader and (for
 *      alliance) the sub-type discriminator, then delegates
 *      to the body sibling. Wire output is byte-identical
 *      to the pre-split code.
 *
 *  Above every encodeFooBody is a one-line classification
 *  of how its body uses the `recipient` argument. The
 *  carrier path encodes lazily at send time against the
 *  recipient's then-current state, so anything mutable
 *  matters for Phase 3+.
 *********************************************************/

#include "transport_control_codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../winbolonet/winbolonet_core.h" /* winbolonetKeyIsValid */
#include "control_event.h"
#include "netpacks.h"
#include "player_flags.h"  /* CLIENT_TYPE_COUNT / CLIENT_TYPE_UNKNOWN */
#include "transport_udp_internal.h"

/* ================================================================
 * Encoders — paired body + full-packet wrapper for each variant.
 *
 * Signature (both body and wrapper):
 *   (const ControlEvent *evt,
 *    const struct UdpServerClient *recipient,
 *    uint8_t *buf, size_t bufCap, size_t *outLen)
 *
 * Wrappers preserve today's wire layout. Body encoders write only
 * the post-header bytes; for the three alliance siblings the body
 * also drops the sub-type discriminator (the carrier byte is
 * authoritative in the reliable path).
 * ================================================================ */

/* PACKET_ALLIANCE_UPDATE shares one wire shape across the three
 * sub-events: [header 8][event 1][fromPlayer 1][toPlayer 1].  For
 * LEAVE the third byte is unused on the wire and on the decode side.
 * The body-only flavor drops the leading event byte (the carrier's
 * ControlEventType already discriminates request/accept/leave). */
#define ALLIANCE_UPDATE_PAYLOAD 3
#define ALLIANCE_BODY_PAYLOAD   2
/* CTRL_ALLIANCE_RESET — full matrix carried as MAX_TANKS×uint16_t bigendian
 * (per-player ally bitmap). Discriminator byte ALLIANCE_EVENT_RESET prefixes
 * the wire-packet payload; the body-only flavor (reliable carrier) drops
 * the discriminator the same way ACCEPT/LEAVE bodies do. */
#define ALLIANCE_RESET_BODY_PAYLOAD   (2 * MAX_TANKS)
#define ALLIANCE_RESET_WIRE_PAYLOAD   (1 + ALLIANCE_RESET_BODY_PAYLOAD)

/* recipient: safe — ignored. */
static EncodeResult encodeAllianceRequestBody(const ControlEvent *evt,
                                              const struct UdpServerClient *recipient,
                                              uint8_t *buf, size_t bufCap,
                                              size_t *outLen) {
    (void)recipient;
    if (bufCap < ALLIANCE_BODY_PAYLOAD) return ENCODE_OVERFLOW;
    buf[0] = evt->u.allianceRequest.fromPlayer;
    buf[1] = evt->u.allianceRequest.toPlayer;
    *outLen = ALLIANCE_BODY_PAYLOAD;
    return ENCODE_OK;
}

static EncodeResult encodeAllianceRequest(const ControlEvent *evt,
                                          const struct UdpServerClient *recipient,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + ALLIANCE_UPDATE_PAYLOAD;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_ALLIANCE_UPDATE, 0);
    buf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_REQUEST;
    size_t bodyLen = 0;
    EncodeResult r = encodeAllianceRequestBody(
        evt, recipient,
        buf + PACKET_HEADER_SIZE + 1,
        bufCap - PACKET_HEADER_SIZE - 1, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + 1 + bodyLen;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeAllianceAcceptBody(const ControlEvent *evt,
                                             const struct UdpServerClient *recipient,
                                             uint8_t *buf, size_t bufCap,
                                             size_t *outLen) {
    (void)recipient;
    if (bufCap < ALLIANCE_BODY_PAYLOAD) return ENCODE_OVERFLOW;
    buf[0] = evt->u.allianceAccept.acceptedBy;
    buf[1] = evt->u.allianceAccept.newMember;
    *outLen = ALLIANCE_BODY_PAYLOAD;
    return ENCODE_OK;
}

static EncodeResult encodeAllianceAccept(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + ALLIANCE_UPDATE_PAYLOAD;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_ALLIANCE_UPDATE, 0);
    buf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_ACCEPT;
    size_t bodyLen = 0;
    EncodeResult r = encodeAllianceAcceptBody(
        evt, recipient,
        buf + PACKET_HEADER_SIZE + 1,
        bufCap - PACKET_HEADER_SIZE - 1, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + 1 + bodyLen;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeAllianceLeaveBody(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)recipient;
    if (bufCap < ALLIANCE_BODY_PAYLOAD) return ENCODE_OVERFLOW;
    buf[0] = evt->u.allianceLeave.playerNum;
    buf[1] = 0; /* unused for leave — preserved for wire compat */
    *outLen = ALLIANCE_BODY_PAYLOAD;
    return ENCODE_OK;
}

static EncodeResult encodeAllianceLeave(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + ALLIANCE_UPDATE_PAYLOAD;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_ALLIANCE_UPDATE, 0);
    buf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_LEAVE;
    size_t bodyLen = 0;
    EncodeResult r = encodeAllianceLeaveBody(
        evt, recipient,
        buf + PACKET_HEADER_SIZE + 1,
        bufCap - PACKET_HEADER_SIZE - 1, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + 1 + bodyLen;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeAllianceResetBody(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)recipient;
    if (bufCap < ALLIANCE_RESET_BODY_PAYLOAD) return ENCODE_OVERFLOW;
    for (int i = 0; i < MAX_TANKS; i++) {
        packU16(buf + 2 * i, evt->u.allianceReset.allies[i]);
    }
    *outLen = ALLIANCE_RESET_BODY_PAYLOAD;
    return ENCODE_OK;
}

static EncodeResult encodeAllianceReset(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + ALLIANCE_RESET_WIRE_PAYLOAD;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_ALLIANCE_UPDATE, 0);
    buf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_RESET;
    size_t bodyLen = 0;
    EncodeResult r = encodeAllianceResetBody(
        evt, recipient,
        buf + PACKET_HEADER_SIZE + 1,
        bufCap - PACKET_HEADER_SIZE - 1, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + 1 + bodyLen;
    return ENCODE_OK;
}

/* PACKET_PLAYER_JOINED wire format:
 *   [header 8] [pNum 1] [name PACKET_MAX_PLAYER_NAME] [cc 2]
 *   [clientType 1] [clientFlags 1] [numAllies 1] [ally 1 × numAllies]
 * The trailing numAllies/allies pair is an additive change from the
 * pre-codec wire format — receiving clients now have the join's full
 * alliance bitmap on the wire from the join event itself. */

/* recipient: safe — ignored. */
static EncodeResult encodePlayerJoinBody(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)recipient;
    BYTE numAllies = evt->u.playerJoin.numAllies;
    if (numAllies > MAX_TANKS) numAllies = MAX_TANKS;
    const size_t needed = 1 + PACKET_MAX_PLAYER_NAME + 2 + 1 + 1 + 1 + numAllies;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = 0;
    buf[pos++] = evt->u.playerJoin.playerNum;
    memset(buf + pos, 0, PACKET_MAX_PLAYER_NAME);
    {
        size_t nameLen = strnlen(evt->u.playerJoin.name, PACKET_MAX_PLAYER_NAME - 1);
        if (nameLen > 0) memcpy(buf + pos, evt->u.playerJoin.name, nameLen);
    }
    pos += PACKET_MAX_PLAYER_NAME;
    buf[pos++] = (uint8_t)evt->u.playerJoin.country[0];
    buf[pos++] = (uint8_t)evt->u.playerJoin.country[1];
    buf[pos++] = evt->u.playerJoin.clientType;
    buf[pos++] = evt->u.playerJoin.clientFlags;
    buf[pos++] = numAllies;
    if (numAllies > 0) {
        memcpy(buf + pos, evt->u.playerJoin.allies, numAllies);
        pos += numAllies;
    }
    *outLen = pos;
    return ENCODE_OK;
}

static EncodeResult encodePlayerJoin(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_PLAYER_JOINED, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodePlayerJoinBody(evt, recipient,
                                          buf + PACKET_HEADER_SIZE,
                                          bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* PACKET_PLAYER_LEFT wire format:
 *   [header 8] [playerNum 1] [name PACKET_MAX_PLAYER_NAME (NUL-padded)]
 *   [cc 2] */

/* recipient: safe — ignored. */
static EncodeResult encodePlayerLeaveBody(const ControlEvent *evt,
                                          const struct UdpServerClient *recipient,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    (void)recipient;
    const size_t needed = 1 + PACKET_MAX_PLAYER_NAME + 2;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = 0;
    buf[pos++] = evt->u.playerLeave.playerNum;
    memset(buf + pos, 0, PACKET_MAX_PLAYER_NAME);
    {
        size_t nameLen = strnlen(evt->u.playerLeave.name, PACKET_MAX_PLAYER_NAME - 1);
        if (nameLen > 0) memcpy(buf + pos, evt->u.playerLeave.name, nameLen);
    }
    pos += PACKET_MAX_PLAYER_NAME;
    buf[pos++] = (uint8_t)evt->u.playerLeave.country[0];
    buf[pos++] = (uint8_t)evt->u.playerLeave.country[1];
    *outLen = pos;
    return ENCODE_OK;
}

static EncodeResult encodePlayerLeave(const ControlEvent *evt,
                                      const struct UdpServerClient *recipient,
                                      uint8_t *buf, size_t bufCap,
                                      size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_PLAYER_LEFT, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodePlayerLeaveBody(evt, recipient,
                                           buf + PACKET_HEADER_SIZE,
                                           bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* PACKET_NAME_CHANGE wire format:
 *   [header 8] [playerNum 1] [newName PACKET_MAX_PLAYER_NAME (NUL-padded)] */

/* recipient: safe — ignored. */
static EncodeResult encodePlayerNameBody(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)recipient;
    const size_t needed = 1 + PACKET_MAX_PLAYER_NAME;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    buf[0] = evt->u.playerName.playerNum;
    memset(buf + 1, 0, PACKET_MAX_PLAYER_NAME);
    {
        size_t nameLen = strnlen(evt->u.playerName.name, PACKET_MAX_PLAYER_NAME - 1);
        if (nameLen > 0) memcpy(buf + 1, evt->u.playerName.name, nameLen);
    }
    *outLen = needed;
    return ENCODE_OK;
}

static EncodeResult encodePlayerName(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_NAME_CHANGE, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodePlayerNameBody(evt, recipient,
                                          buf + PACKET_HEADER_SIZE,
                                          bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* PACKET_LOBBY_UPDATE wire format (variable length):
 *   [header 8] [playerNum 1] [connected 1]
 *   If connected:
 *     [nameLen 1] [name nameLen bytes] [teamNumber 1] [ready 1]
 *     [isBot 1] [pingMs 2 BE] [cc 2] [clientType 1] [clientFlags 1]
 *     [startIdx 1] */

/* recipient: safe — ignored. */
static EncodeResult encodeLobbySlotBody(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    (void)recipient;
    const ClientLobbySlot *slot = &evt->u.lobbySlot.slot;
    size_t nameLen = 0;
    if (slot->connected) {
        nameLen = strnlen(slot->playerName, PACKET_MAX_PLAYER_NAME - 1);
    }
    const size_t needed = 1 + 1
                          + (slot->connected ? (1 + nameLen + 1 + 1 + 1 + 2 + 2 + 1 + 1 + 1) : 0);
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = 0;
    buf[pos++] = evt->u.lobbySlot.playerNum;
    buf[pos++] = slot->connected ? 1 : 0;
    if (slot->connected) {
        buf[pos++] = (uint8_t)nameLen;
        if (nameLen > 0) {
            memcpy(buf + pos, slot->playerName, nameLen);
            pos += nameLen;
        }
        buf[pos++] = slot->teamNumber;
        buf[pos++] = slot->ready ? 1 : 0;
        buf[pos++] = slot->isBot ? 1 : 0;
        buf[pos++] = (uint8_t)(slot->pingMs >> 8);
        buf[pos++] = (uint8_t)(slot->pingMs & 0xFF);
        buf[pos++] = (uint8_t)slot->countryCode[0];
        buf[pos++] = (uint8_t)slot->countryCode[1];
        buf[pos++] = slot->clientType;
        buf[pos++] = slot->clientFlags;
        buf[pos++] = slot->startIdx;
    }
    *outLen = pos;
    return ENCODE_OK;
}

static EncodeResult encodeLobbySlot(const ControlEvent *evt,
                                    const struct UdpServerClient *recipient,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_UPDATE, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeLobbySlotBody(evt, recipient,
                                         buf + PACKET_HEADER_SIZE,
                                         bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* CTRL_SPECTATOR_SLOT body wire format (variable length):
 *   [specIdx 1] [connected 1]
 *   If connected:
 *     [nameLen 1] [name nameLen bytes] [cc0 1] [cc1 1]
 *     [clientType 1] [clientFlags 1]
 * Delivered body-only on CHANNEL_CONTROL; there is no full-packet
 * wrapper or PACKET_* type for this event. */

/* recipient: safe — ignored. */
static EncodeResult encodeSpectatorSlotBody(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)recipient;
    const ClientSpectatorSlot *slot = &evt->u.spectatorSlot.slot;
    size_t nameLen = 0;
    if (slot->connected) {
        nameLen = strnlen(slot->playerName, PACKET_MAX_PLAYER_NAME - 1);
    }
    const size_t needed = 1 + 1
                          + (slot->connected ? (1 + nameLen + 1 + 1 + 1 + 1) : 0);
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = 0;
    buf[pos++] = evt->u.spectatorSlot.specIdx;
    buf[pos++] = slot->connected ? 1 : 0;
    if (slot->connected) {
        buf[pos++] = (uint8_t)nameLen;
        if (nameLen > 0) {
            memcpy(buf + pos, slot->playerName, nameLen);
            pos += nameLen;
        }
        buf[pos++] = (uint8_t)slot->countryCode[0];
        buf[pos++] = (uint8_t)slot->countryCode[1];
        buf[pos++] = slot->clientType;
        buf[pos++] = slot->clientFlags;
    }
    *outLen = pos;
    return ENCODE_OK;
}

/* CTRL_SPECTATOR_CHAT body wire format (variable length):
 *   [specIdx 1] [msgLen 1] [msg msgLen bytes]
 * msgLen is a single byte (room for 255), but the encoder clamps the message
 * to PACKET_MAX_CHAT_MESSAGE (128). Delivered body-only on CHANNEL_CONTROL; there
 * is no full-packet wrapper or PACKET_* type for this event. */

/* recipient: safe — ignored. */
static EncodeResult encodeSpectatorChatBody(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)recipient;
    size_t msgLen = evt->u.spectatorChat.bodyLen;
    if (msgLen > PACKET_MAX_CHAT_MESSAGE) msgLen = PACKET_MAX_CHAT_MESSAGE;
    const size_t needed = 1 + 1 + msgLen;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = 0;
    buf[pos++] = evt->u.spectatorChat.specIdx;
    buf[pos++] = (uint8_t)msgLen;
    if (msgLen > 0) {
        memcpy(buf + pos, evt->u.spectatorChat.body, msgLen);
        pos += msgLen;
    }
    *outLen = pos;
    return ENCODE_OK;
}

/* PACKET_LOBBY_SETTINGS wire format:
 *   [header 8] [mapName MAP_STR_SIZE] [gameType 1] [hiddenMines 1]
 *   [aiType 1] [gameLength 4 BE] [pillCount 1] [baseCount 1]
 *   [startCount 1] [mapSkipAvailable 1] [netStat 1] [hasLobby 1]
 *   [openHost 1] [autoLockOnGameStart 1] [serverLocks 2 BE]
 *   [ranked 1] [allowNewPlayers 1] [wbnAvailable 1] [uploadPolicy 1]
 *   [lobbyStartDelay 4 BE] [hostSlot 1]
 *   [pillView 1] [baseView 1] [allyView 1]
 *   [pillDecay 2 BE] [baseDecay 2 BE] [allyDecay 2 BE]
 *   [classicMode 1] [alliesInTrees 1]
 *
 * The trailing bytes are appended after the base layout so the
 * existing fields keep their offsets. The decoder reads each one
 * optionally and leaves zero-init defaults in place when the sender
 * omits them, which keeps old/new codec pairs interoperable.
 * serverLocks is packed big-endian to match every other multi-byte
 * field in the codec (packU16/packU32). */
#define LOBBY_SETTINGS_WIRE_PAYLOAD_BASE \
    (MAP_STR_SIZE + 1 + 1 + 1 + 4 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 2)
/* Trailing optional tail: ranked(1) + allowNewPlayers(1) + wbnAvailable(1)
 * + uploadPolicy(1) + lobbyStartDelay(4) + hostSlot(1) + three view
 * policies(3) + three view decay seconds(6) + classicMode(1)
 * + alliesInTrees(1) + voiceMode(1). */
#define LOBBY_SETTINGS_WIRE_PAYLOAD \
    (LOBBY_SETTINGS_WIRE_PAYLOAD_BASE + 4 + 4 + 1 + 3 + 6 + 1 + 1 + 1)

/* recipient: safe — ignored. */
static EncodeResult encodeLobbySettingsBody(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)recipient;
    const size_t needed = LOBBY_SETTINGS_WIRE_PAYLOAD;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = 0;
    memset(buf + pos, 0, MAP_STR_SIZE);
    {
        size_t mapLen = strnlen(evt->u.lobbySettings.mapName, MAP_STR_SIZE - 1);
        if (mapLen > 0) memcpy(buf + pos, evt->u.lobbySettings.mapName, mapLen);
    }
    pos += MAP_STR_SIZE;
    buf[pos++] = (uint8_t)evt->u.lobbySettings.lobbyGameType;
    buf[pos++] = evt->u.lobbySettings.lobbyHiddenMines ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyAiType;
    packU32(buf + pos, (uint32_t)evt->u.lobbySettings.lobbyTimeLimit);
    pos += 4;
    buf[pos++] = evt->u.lobbySettings.lobbyPillCount;
    buf[pos++] = evt->u.lobbySettings.lobbyBaseCount;
    buf[pos++] = evt->u.lobbySettings.lobbyStartCount;
    buf[pos++] = evt->u.lobbySettings.mapSkipAvailable ? 1 : 0;
    buf[pos++] = (uint8_t)evt->u.lobbySettings.netStat;
    buf[pos++] = evt->u.lobbySettings.hasLobby ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyOpenHost ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyAutoLockOnGameStart ? 1 : 0;
    packU16(buf + pos, evt->u.lobbySettings.lobbyServerLocks);
    pos += 2;
    buf[pos++] = evt->u.lobbySettings.lobbyRanked ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyAllowNewPlayers ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyWbnAvailable ? 1 : 0;
    buf[pos++] = (uint8_t)evt->u.lobbySettings.uploadPolicy;
    packU32(buf + pos, (uint32_t)evt->u.lobbySettings.lobbyStartDelay);
    pos += 4;
    buf[pos++] = evt->u.lobbySettings.hostSlot;
    for (int vc = 0; vc < VIEW_CATEGORY_COUNT; vc++) {
        buf[pos++] = (uint8_t)evt->u.lobbySettings.viewPolicy[vc];
    }
    for (int vc = 0; vc < VIEW_CATEGORY_COUNT; vc++) {
        packU16(buf + pos, evt->u.lobbySettings.viewDecaySecs[vc]);
        pos += 2;
    }
    buf[pos++] = evt->u.lobbySettings.lobbyClassicMode ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyAlliesInTrees ? 1 : 0;
    buf[pos++] = (uint8_t)evt->u.lobbySettings.voiceMode;
    *outLen = pos;
    return ENCODE_OK;
}

static EncodeResult encodeLobbySettings(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_SETTINGS, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeLobbySettingsBody(evt, recipient,
                                             buf + PACKET_HEADER_SIZE,
                                             bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* PACKET_LOBBY_TEAM_META_CHG wire format:
 *   [header 8] [teamId 1] [color 1] [namingPool 1] [startSide 1]
 *   [nameLen 1] [name nameLen]
 * The `in_use` flag is not on the wire — the decoder reconstructs it
 * from (nameLen > 0 || color != 0 || pool != 0 || startSide != 0). */

/* recipient: safe — ignored. */
static EncodeResult encodeLobbyTeamMetaBody(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)recipient;
    if (evt->u.lobbyTeamMeta.teamId == 0 ||
        evt->u.lobbyTeamMeta.teamId >= MAX_TANKS) {
        return ENCODE_SKIP;
    }
    size_t nameLen = strnlen(evt->u.lobbyTeamMeta.name, LOBBY_TEAM_NAME_LEN - 1);
    const size_t needed = 5 + nameLen;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = 0;
    buf[pos++] = evt->u.lobbyTeamMeta.teamId;
    buf[pos++] = evt->u.lobbyTeamMeta.color;
    buf[pos++] = evt->u.lobbyTeamMeta.namingPool;
    buf[pos++] = evt->u.lobbyTeamMeta.startSide;
    buf[pos++] = (uint8_t)nameLen;
    if (nameLen > 0) {
        memcpy(buf + pos, evt->u.lobbyTeamMeta.name, nameLen);
        pos += nameLen;
    }
    *outLen = pos;
    return ENCODE_OK;
}

static EncodeResult encodeLobbyTeamMeta(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    /* Reproduce the body's validation guard before touching buf so the
     * wrapper short-circuits to ENCODE_SKIP without writing a header
     * the caller would then discard. */
    if (evt->u.lobbyTeamMeta.teamId == 0 ||
        evt->u.lobbyTeamMeta.teamId >= MAX_TANKS) {
        return ENCODE_SKIP;
    }
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_TEAM_META_CHG, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeLobbyTeamMetaBody(evt, recipient,
                                             buf + PACKET_HEADER_SIZE,
                                             bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* PACKET_LOBBY_BOT_CONFIG_CHG wire format (ported verbatim from
 * branch's transportUdpServerBroadcastLobbyBotConfigChg):
 *   [header 8] [slot 1] [difficulty 1] [personality 1] [nameLen 1]
 *   [name nameLen] */

/* recipient: safe — ignored. */
static EncodeResult encodeLobbyBotConfigBody(const ControlEvent *evt,
                                             const struct UdpServerClient *recipient,
                                             uint8_t *buf, size_t bufCap,
                                             size_t *outLen) {
    (void)recipient;
    if (evt->u.lobbyBotConfig.slot >= MAX_TANKS) return ENCODE_SKIP;
    size_t nameLen = strnlen(evt->u.lobbyBotConfig.name,
                             PACKET_MAX_PLAYER_NAME - 1);
    const size_t needed = 4 + nameLen;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = 0;
    buf[pos++] = evt->u.lobbyBotConfig.slot;
    buf[pos++] = evt->u.lobbyBotConfig.difficulty;
    buf[pos++] = evt->u.lobbyBotConfig.personality;
    buf[pos++] = (uint8_t)nameLen;
    if (nameLen > 0) {
        memcpy(buf + pos, evt->u.lobbyBotConfig.name, nameLen);
        pos += nameLen;
    }
    *outLen = pos;
    return ENCODE_OK;
}

static EncodeResult encodeLobbyBotConfig(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    /* Mirror the body's guard at the wrapper so an invalid slot
     * short-circuits before we stamp a header. */
    if (evt->u.lobbyBotConfig.slot >= MAX_TANKS) return ENCODE_SKIP;
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_BOT_CONFIG_CHG, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeLobbyBotConfigBody(evt, recipient,
                                              buf + PACKET_HEADER_SIZE,
                                              bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* PACKET_LOBBY_BOT_BRAIN_CHG wire format:
 *   [header 8] [slot 1] [brainIdx 1]
 * brainIdx == 0xFF signals "use the server's global bot brain";
 * any other value indexes into the server's brain catalogue. */

/* recipient: safe — ignored. */
static EncodeResult encodeLobbyBotBrainBody(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)recipient;
    if (evt->u.lobbyBotBrain.slot >= MAX_TANKS) return ENCODE_SKIP;
    if (bufCap < 2) return ENCODE_OVERFLOW;
    buf[0] = evt->u.lobbyBotBrain.slot;
    buf[1] = evt->u.lobbyBotBrain.brainIdx;
    *outLen = 2;
    return ENCODE_OK;
}

static EncodeResult encodeLobbyBotBrain(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    if (evt->u.lobbyBotBrain.slot >= MAX_TANKS) return ENCODE_SKIP;
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_BOT_BRAIN_CHG, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeLobbyBotBrainBody(evt, recipient,
                                             buf + PACKET_HEADER_SIZE,
                                             bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* PACKET_LOBBY_BRAIN_LIST wire format:
 *   [header 8] [count 1]
 *   repeat count times:
 *     [nameLen 1] [name nameLen]
 *     [verLen 1]  [version verLen]
 * Disk paths stay server-private and are never sent. Worst-case
 * payload is 1 + BRAIN_LIST_MAX * (2 + (NAME_LEN-1) + (VER_LEN-1))
 * = 1 + 16 * (2 + 31 + 23) = 897 bytes, which fits comfortably in
 * a single UDP datagram. */

/* recipient: safe — ignored. */
static EncodeResult encodeLobbyBrainListBody(const ControlEvent *evt,
                                             const struct UdpServerClient *recipient,
                                             uint8_t *buf, size_t bufCap,
                                             size_t *outLen) {
    (void)recipient;
    const BrainList *list = &evt->u.lobbyBrainList.list;
    int count = list->count;
    if (count < 0) count = 0;
    if (count > BRAIN_LIST_MAX) count = BRAIN_LIST_MAX;
    /* Pre-compute total size; bail with ENCODE_OVERFLOW before any
     * write if the recipient buffer can't hold the worst case. */
    size_t needed = 1;
    for (int i = 0; i < count; i++) {
        const BrainListEntry *e = &list->entries[i];
        needed += 2
                + strnlen(e->name,    BRAIN_LIST_NAME_LEN - 1)
                + strnlen(e->version, BRAIN_LIST_VER_LEN  - 1);
    }
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = 0;
    buf[pos++] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        const BrainListEntry *e = &list->entries[i];
        uint8_t n = (uint8_t)strnlen(e->name,    BRAIN_LIST_NAME_LEN - 1);
        uint8_t v = (uint8_t)strnlen(e->version, BRAIN_LIST_VER_LEN  - 1);
        buf[pos++] = n;
        if (n > 0) { memcpy(buf + pos, e->name, n);    pos += n; }
        buf[pos++] = v;
        if (v > 0) { memcpy(buf + pos, e->version, v); pos += v; }
    }
    *outLen = pos;
    return ENCODE_OK;
}

static EncodeResult encodeLobbyBrainList(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_BRAIN_LIST, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeLobbyBrainListBody(evt, recipient,
                                              buf + PACKET_HEADER_SIZE,
                                              bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* Compile-time guarantee that the brain-list worst case fits inside
 * MAX_CONTROL_PACKET — keeps the runtime ENCODE_OVERFLOW path above
 * unreachable as long as the codec header keeps its budget. */
BOLO_STATIC_ASSERT(
    PACKET_HEADER_SIZE + 1 +
        (size_t)BRAIN_LIST_MAX *
            (2 + (BRAIN_LIST_NAME_LEN - 1) + (BRAIN_LIST_VER_LEN - 1))
        <= MAX_CONTROL_PACKET,
    brain_list_worst_case_fits_MAX_CONTROL_PACKET);

/* PACKET_ROUND_STATS wire format:
 *   [header 8] [playerCount 1]
 *   repeat playerCount times (20 bytes each):
 *     [slot 1][isBot 1][kills 2][deaths 2][baseCaptures 2]
 *     [pillCaptures 2][dmgDealt 4][builds 2][lgmKills 2][lgmDeaths 2]
 *   [awardCount 1]
 *   repeat awardCount times (8 bytes each):
 *     [awardId 1][winnerSlot 1][subjectSlot 1][winnerIsBot 1][value 4]
 *   [keyLen 1] [wbnLogKey keyLen]
 *   [highlightCount 1]
 *   repeat highlightCount times (26 bytes each):
 *     [startTick 4][durationTicks 4][startMs 4][durationMs 4]
 *     [mapX 1][mapY 1][type 1][awardId 1][actorA 1][actorB 1][value 4]
 * The ticks are the scorer's units and the ms are the server's conversion of
 * them; both cross so a client can take the time without modelling the sim's
 * cadence and the viewer's own calibration still has the ticks to work from.
 * HighlightWindow.score is the scorer's internal ranking magnitude and does
 * not cross; it decodes as 0.
 * Multi-byte fields are big-endian via packU16/packU32, matching every
 * other body encoder. */

/* recipient: safe — ignored. */
static EncodeResult encodeRoundStatsBody(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)recipient;
    const RoundStatsSummary *s = &evt->u.roundStats;
    uint8_t pc = s->playerCount;
    if (pc > MAX_TANKS) pc = MAX_TANKS;
    uint8_t ac = s->awardCount;
    if (ac > AWARD_COUNT) ac = AWARD_COUNT;
    uint8_t keyLen = (uint8_t)strnlen(s->wbnLogKey, ROUND_STATS_LOGKEY_LEN - 1);
    uint8_t hc = s->highlightCount;
    if (hc > ROUND_STATS_HIGHLIGHTS_WIRE_MAX) hc = ROUND_STATS_HIGHLIGHTS_WIRE_MAX;

    /* Pre-compute total size; bail before any write if it can't fit. */
    size_t needed = 1 + (size_t)pc * 20 + 1 + (size_t)ac * 8 + 1 + keyLen +
                    1 + (size_t)hc * 26;
    if (bufCap < needed) return ENCODE_OVERFLOW;

    size_t pos = 0;
    buf[pos++] = pc;
    for (uint8_t i = 0; i < pc; i++) {
        const RoundPlayerSummary *p = &s->players[i];
        buf[pos++] = p->slot;
        buf[pos++] = p->isBot;
        packU16(buf + pos, p->kills);        pos += 2;
        packU16(buf + pos, p->deaths);       pos += 2;
        packU16(buf + pos, p->baseCaptures); pos += 2;
        packU16(buf + pos, p->pillCaptures); pos += 2;
        packU32(buf + pos, p->dmgDealt);     pos += 4;
        packU16(buf + pos, p->builds);       pos += 2;
        packU16(buf + pos, p->lgmKills);     pos += 2;
        packU16(buf + pos, p->lgmDeaths);    pos += 2;
    }
    buf[pos++] = ac;
    for (uint8_t i = 0; i < ac; i++) {
        const AwardResult *a = &s->awards[i];
        buf[pos++] = a->awardId;
        buf[pos++] = a->winnerSlot;
        buf[pos++] = a->subjectSlot;
        buf[pos++] = a->winnerIsBot;
        packU32(buf + pos, a->value);        pos += 4;
    }
    buf[pos++] = keyLen;
    if (keyLen > 0) { memcpy(buf + pos, s->wbnLogKey, keyLen); pos += keyLen; }
    buf[pos++] = hc;
    for (uint8_t i = 0; i < hc; i++) {
        const HighlightWindow *h = &s->highlights[i];
        packU32(buf + pos, h->startTick);     pos += 4;
        packU32(buf + pos, h->durationTicks); pos += 4;
        packU32(buf + pos, h->startMs);       pos += 4;
        packU32(buf + pos, h->durationMs);    pos += 4;
        buf[pos++] = h->mapX;
        buf[pos++] = h->mapY;
        buf[pos++] = h->type;
        buf[pos++] = h->awardId;
        buf[pos++] = h->actorA;
        buf[pos++] = h->actorB;
        packU32(buf + pos, h->value);         pos += 4;
    }
    *outLen = pos;
    return ENCODE_OK;
}

/* CTRL_STATS_SEED body wire format — the row half of CTRL_ROUND_STATS and
 * nothing else (no awards, no highlights, no log key):
 *   [playerCount 1]
 *   playerCount x [slot 1][isBot 1][kills 2][deaths 2][baseCaptures 2]
 *                 [pillCaptures 2][dmgDealt 4][builds 2][lgmKills 2]
 *                 [lgmDeaths 2]                                   (20 bytes)
 * Multi-byte fields are big-endian via packU16/packU32, matching every other
 * body encoder. Delivered body-only inside a joiner's sync replay: no
 * standalone encoder and no PACKET_* type of its own. */

/* recipient: safe — ignored. */
static EncodeResult encodeStatsSeedBody(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    (void)recipient;
    uint8_t pc = evt->u.statsSeed.playerCount;
    if (pc > MAX_TANKS) pc = MAX_TANKS;

    if (bufCap < (size_t)1 + (size_t)pc * 20) return ENCODE_OVERFLOW;

    size_t pos = 0;
    buf[pos++] = pc;
    for (uint8_t i = 0; i < pc; i++) {
        const RoundPlayerSummary *p = &evt->u.statsSeed.players[i];
        buf[pos++] = p->slot;
        buf[pos++] = p->isBot;
        packU16(buf + pos, p->kills);        pos += 2;
        packU16(buf + pos, p->deaths);       pos += 2;
        packU16(buf + pos, p->baseCaptures); pos += 2;
        packU16(buf + pos, p->pillCaptures); pos += 2;
        packU32(buf + pos, p->dmgDealt);     pos += 4;
        packU16(buf + pos, p->builds);       pos += 2;
        packU16(buf + pos, p->lgmKills);     pos += 2;
        packU16(buf + pos, p->lgmDeaths);    pos += 2;
    }
    *outLen = pos;
    return ENCODE_OK;
}

static bool decodeStatsSeedBody(const uint8_t *buf, size_t len,
                                ControlEvent *outEvt) {
    if (len < 1) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_STATS_SEED;
    size_t pos = 0;

    /* Untrusted input. The claimed count must be backed by real bytes before
     * any row is read, and only MAX_TANKS of them can be stored — a count
     * past that is clamped, not trusted. */
    uint8_t claimed = buf[pos++];
    if (pos + (size_t)claimed * 20 > len) return false;
    uint8_t pc = claimed;
    if (pc > MAX_TANKS) pc = MAX_TANKS;

    for (uint8_t i = 0; i < pc; i++) {
        RoundPlayerSummary *p = &outEvt->u.statsSeed.players[i];
        p->slot         = buf[pos++];
        p->isBot        = buf[pos++];
        p->kills        = unpackU16(buf + pos); pos += 2;
        p->deaths       = unpackU16(buf + pos); pos += 2;
        p->baseCaptures = unpackU16(buf + pos); pos += 2;
        p->pillCaptures = unpackU16(buf + pos); pos += 2;
        p->dmgDealt     = unpackU32(buf + pos); pos += 4;
        p->builds       = unpackU16(buf + pos); pos += 2;
        p->lgmKills     = unpackU16(buf + pos); pos += 2;
        p->lgmDeaths    = unpackU16(buf + pos); pos += 2;
        /* The slot is what the client indexes its per-slot board by. Reject
         * the whole body rather than silently dropping the row: a valid
         * server never sends one, and a partial seed would read as truth. */
        if (p->slot >= MAX_TANKS) return false;
    }
    outEvt->u.statsSeed.playerCount = pc;
    return true;
}

/* PACKET_LOBBY_BOT_POOL_CHUNK wire format:
 *   [header 8] [seq 1] [count 1] [fragLen 2 BE] [frag fragLen]
 * Each fragment is one slice of the server's zlib-compressed bot-pool
 * catalog; the client reassembles seq 0..count-1 and installs. */

/* recipient: safe — ignored. */
static EncodeResult encodeLobbyBotPoolChunkBody(const ControlEvent *evt,
                                                const struct UdpServerClient *recipient,
                                                uint8_t *buf, size_t bufCap,
                                                size_t *outLen) {
    (void)recipient;
    uint16_t fl = evt->u.lobbyBotPoolChunk.fragLen;
    size_t pos = 0;
    if (fl > LOBBY_BOT_POOL_CHUNK_FRAG_MAX) return ENCODE_OVERFLOW;
    if (bufCap < (size_t)(4 + fl)) return ENCODE_OVERFLOW;
    buf[pos++] = evt->u.lobbyBotPoolChunk.seq;
    buf[pos++] = evt->u.lobbyBotPoolChunk.count;
    buf[pos++] = (uint8_t)((fl >> 8) & 0xFF);
    buf[pos++] = (uint8_t)(fl & 0xFF);
    if (fl > 0) { memcpy(buf + pos, evt->u.lobbyBotPoolChunk.frag, fl); pos += fl; }
    *outLen = pos;
    return ENCODE_OK;
}

static EncodeResult encodeRoundStats(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_ROUND_STATS, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeRoundStatsBody(evt, recipient,
                                          buf + PACKET_HEADER_SIZE,
                                          bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

static EncodeResult encodeLobbyBotPoolChunk(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_BOT_POOL_CHUNK, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeLobbyBotPoolChunkBody(evt, recipient,
                                                 buf + PACKET_HEADER_SIZE,
                                                 bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

static bool decodeRoundStatsBody(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    if (len < 1) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_ROUND_STATS;
    RoundStatsSummary *s = &outEvt->u.roundStats;
    size_t pos = 0;

    uint8_t pc = buf[pos++];
    if (pc > MAX_TANKS) return false;
    if (pos + (size_t)pc * 20 > len) return false;
    for (uint8_t i = 0; i < pc; i++) {
        RoundPlayerSummary *p = &s->players[i];
        p->slot         = buf[pos++];
        p->isBot        = buf[pos++];
        p->kills        = unpackU16(buf + pos); pos += 2;
        p->deaths       = unpackU16(buf + pos); pos += 2;
        p->baseCaptures = unpackU16(buf + pos); pos += 2;
        p->pillCaptures = unpackU16(buf + pos); pos += 2;
        p->dmgDealt     = unpackU32(buf + pos); pos += 4;
        p->builds       = unpackU16(buf + pos); pos += 2;
        p->lgmKills     = unpackU16(buf + pos); pos += 2;
        p->lgmDeaths    = unpackU16(buf + pos); pos += 2;
    }
    s->playerCount = pc;

    if (pos + 1 > len) return false;
    uint8_t ac = buf[pos++];
    if (ac > AWARD_COUNT) return false;
    if (pos + (size_t)ac * 8 > len) return false;
    for (uint8_t i = 0; i < ac; i++) {
        AwardResult *a = &s->awards[i];
        a->awardId     = buf[pos++];
        a->winnerSlot  = buf[pos++];
        a->subjectSlot = buf[pos++];
        a->winnerIsBot = buf[pos++];
        a->value       = unpackU32(buf + pos); pos += 4;
    }
    s->awardCount = ac;

    if (pos + 1 > len) return false;
    uint8_t keyLen = buf[pos++];
    if (keyLen > ROUND_STATS_LOGKEY_LEN - 1) return false;
    if (pos + keyLen > len) return false;
    if (keyLen > 0) memcpy(s->wbnLogKey, buf + pos, keyLen);
    s->wbnLogKey[keyLen] = '\0';
    pos += keyLen;
    /* The key crosses from a server we do not trust and the recap hands it
     * straight to WinBolo.net inside "logs/%s/download" and "logs/%s/comment"
     * — authenticated, redirect-following requests. Anything that is not the
     * 32-hex shape WBN issues is dropped here, at the only door it comes in
     * by, rather than at each URL. Dropping it and not the packet is
     * deliberate: the scoreboard, awards and highlights in the rest of the
     * body are still worth showing, and an empty key is already the ordinary
     * state of a LAN or single-player round, so every consumer handles it. */
    if (keyLen > 0 && !winbolonetKeyIsValid(s->wbnLogKey)) {
        s->wbnLogKey[0] = '\0';
    }

    if (pos + 1 > len) return false;
    uint8_t hc = buf[pos++];
    if (hc > ROUND_STATS_HIGHLIGHTS_WIRE_MAX) return false;
    if (pos + (size_t)hc * 26 > len) return false;
    for (uint8_t i = 0; i < hc; i++) {
        HighlightWindow *h = &s->highlights[i];
        h->startTick     = unpackU32(buf + pos); pos += 4;
        h->durationTicks = unpackU32(buf + pos); pos += 4;
        h->startMs       = unpackU32(buf + pos); pos += 4;
        h->durationMs    = unpackU32(buf + pos); pos += 4;
        h->mapX    = buf[pos++];
        h->mapY    = buf[pos++];
        h->type    = buf[pos++];
        h->awardId = buf[pos++];
        h->actorA  = buf[pos++];
        h->actorB  = buf[pos++];
        h->value   = unpackU32(buf + pos); pos += 4;
        /* score is not on the wire; the event-wide memset above leaves it 0. */
    }
    s->highlightCount = hc;
    return true;
}

/* Compile-time guarantee that the round-stats worst case (every slot
 * present, every award won, a full-length key, a full clip list) fits
 * MAX_CONTROL_PACKET. 8 + 1 + 16*20 + 1 + 18*8 + 1 + 32 + 1 + 12*26 = 820. */
BOLO_STATIC_ASSERT(
    PACKET_HEADER_SIZE + 1 + (size_t)MAX_TANKS * 20 + 1 +
        (size_t)AWARD_COUNT * 8 + 1 + (ROUND_STATS_LOGKEY_LEN - 1) + 1 +
        (size_t)ROUND_STATS_HIGHLIGHTS_WIRE_MAX * 26
        <= MAX_CONTROL_PACKET,
    round_stats_worst_case_fits_MAX_CONTROL_PACKET);

BOLO_STATIC_ASSERT(
    PACKET_HEADER_SIZE + 4 + LOBBY_BOT_POOL_CHUNK_FRAG_MAX <= MAX_CONTROL_PACKET,
    bot_pool_chunk_worst_case_fits_MAX_CONTROL_PACKET);

/* CTRL_ROUND_RATING_POSTED body wire format (fixed length):
 *   [fromPlayer 1] [key RATING_POSTED_KEY_BODY_LEN]
 * A short key is NUL-padded out to fill the field, so the body is the same
 * size on every event and the decoder can reject anything else outright.
 * Delivered body-only on CHANNEL_CONTROL; there is no full-packet wrapper or
 * PACKET_* type for this event. */
#define RATING_POSTED_KEY_BODY_LEN (ROUND_STATS_LOGKEY_LEN - 1)

/* recipient: safe — ignored. */
static EncodeResult encodeRoundRatingPostedBody(const ControlEvent *evt,
                                                const struct UdpServerClient *recipient,
                                                uint8_t *buf, size_t bufCap,
                                                size_t *outLen) {
    (void)recipient;
    const size_t needed = 1 + RATING_POSTED_KEY_BODY_LEN;
    size_t keyLen = strnlen(evt->u.ratingPosted.key, RATING_POSTED_KEY_BODY_LEN);
    if (bufCap < needed) return ENCODE_OVERFLOW;
    buf[0] = evt->u.ratingPosted.fromPlayer;
    memset(buf + 1, 0, RATING_POSTED_KEY_BODY_LEN);
    memcpy(buf + 1, evt->u.ratingPosted.key, keyLen);
    *outLen = needed;
    return ENCODE_OK;
}

static bool decodeRoundRatingPostedBody(const uint8_t *buf, size_t len,
                                        ControlEvent *outEvt) {
    if (len != 1 + RATING_POSTED_KEY_BODY_LEN) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_ROUND_RATING_POSTED;
    outEvt->u.ratingPosted.fromPlayer = buf[0];
    memcpy(outEvt->u.ratingPosted.key, buf + 1, RATING_POSTED_KEY_BODY_LEN);
    outEvt->u.ratingPosted.key[RATING_POSTED_KEY_BODY_LEN] = '\0';
    return true;
}

/* CTRL_VIEW_TARGET body wire format (fixed length):
 *   [origSlot 1] [kind 1] [target 1] [mapX 1] [mapY 1] [found 1] [fromEcho 1]
 * Every field is a single byte, so the body is the same size on every event
 * and the decoder can reject anything else outright. Delivered body-only on
 * CHANNEL_CONTROL; there is no full-packet wrapper or PACKET_* type for this
 * event. */
#define VIEW_TARGET_BODY_LEN 7

/* recipient: safe — ignored. */
static EncodeResult encodeViewTargetBody(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)recipient;
    if (bufCap < VIEW_TARGET_BODY_LEN) return ENCODE_OVERFLOW;
    buf[0] = evt->u.viewTarget.origSlot;
    buf[1] = evt->u.viewTarget.kind;
    buf[2] = evt->u.viewTarget.target;
    buf[3] = evt->u.viewTarget.mapX;
    buf[4] = evt->u.viewTarget.mapY;
    buf[5] = evt->u.viewTarget.found;
    buf[6] = evt->u.viewTarget.fromEcho;
    *outLen = VIEW_TARGET_BODY_LEN;
    return ENCODE_OK;
}

static bool decodeViewTargetBody(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    if (len != VIEW_TARGET_BODY_LEN) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_VIEW_TARGET;
    outEvt->u.viewTarget.origSlot = buf[0];
    outEvt->u.viewTarget.kind     = buf[1];
    outEvt->u.viewTarget.target   = buf[2];
    outEvt->u.viewTarget.mapX     = buf[3];
    outEvt->u.viewTarget.mapY     = buf[4];
    outEvt->u.viewTarget.found    = buf[5];
    outEvt->u.viewTarget.fromEcho = buf[6];
    return true;
}

/* CTRL_VOICE_TALKING body wire format (fixed length):
 *   [talking 4]   PlayerBitMap, big-endian
 * One bitmap over the player slots, so the size does not move with the
 * number of talkers. Delivered body-only on CHANNEL_CONTROL; there is no
 * full-packet wrapper or PACKET_* type for this event. */
#define VOICE_TALKING_BODY_PAYLOAD 4

/* recipient: safe — ignored. */
static EncodeResult encodeVoiceTalkingBody(const ControlEvent *evt,
                                           const struct UdpServerClient *recipient,
                                           uint8_t *buf, size_t bufCap,
                                           size_t *outLen) {
    (void)recipient;
    if (bufCap < VOICE_TALKING_BODY_PAYLOAD) return ENCODE_OVERFLOW;
    packU32(buf, evt->u.voiceTalking.talking);
    *outLen = VOICE_TALKING_BODY_PAYLOAD;
    return ENCODE_OK;
}

static bool decodeVoiceTalkingBody(const uint8_t *buf, size_t len,
                                   ControlEvent *outEvt) {
    if (len < VOICE_TALKING_BODY_PAYLOAD) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_VOICE_TALKING;
    outEvt->u.voiceTalking.talking = unpackU32(buf);
    return true;
}

/* CTRL_ENTITY_CHANGE body wire format (fixed length):
 *   [kind 1] [index 1] [added 1] [record 6]
 * The record region is the same six bytes whatever the kind, so the body is
 * one size and the decoder rejects anything else outright. A pillbox spends
 * all six (x, y, owner, armour, speed, inTank), a base all six (x, y, owner,
 * armour, shells, mines), a start the first three (x, y, dir) with the rest
 * zero. Delivered body-only on CHANNEL_CONTROL; there is no full-packet
 * wrapper or PACKET_* type for this event. */
#define ENTITY_CHANGE_BODY_LEN 9
#define ENTITY_CHANGE_REC_OFF  3

/* recipient: safe — ignored. Which items are on the map is public. */
static EncodeResult encodeEntityChangeBody(const ControlEvent *evt,
                                           const struct UdpServerClient *recipient,
                                           uint8_t *buf, size_t bufCap,
                                           size_t *outLen) {
    uint8_t *rec;
    (void)recipient;
    if (bufCap < ENTITY_CHANGE_BODY_LEN) return ENCODE_OVERFLOW;
    rec = buf + ENTITY_CHANGE_REC_OFF;
    buf[0] = evt->u.entityChange.kind;
    buf[1] = evt->u.entityChange.index;
    buf[2] = evt->u.entityChange.added ? 1u : 0u;
    memset(rec, 0, ENTITY_CHANGE_BODY_LEN - ENTITY_CHANGE_REC_OFF);
    switch (evt->u.entityChange.kind) {
    case ENTITY_KIND_PILL:
        rec[0] = evt->u.entityChange.rec.pill.x;
        rec[1] = evt->u.entityChange.rec.pill.y;
        rec[2] = evt->u.entityChange.rec.pill.owner;
        rec[3] = evt->u.entityChange.rec.pill.armour;
        rec[4] = evt->u.entityChange.rec.pill.speed;
        rec[5] = evt->u.entityChange.rec.pill.inTank ? 1u : 0u;
        break;
    case ENTITY_KIND_BASE:
        rec[0] = evt->u.entityChange.rec.base.x;
        rec[1] = evt->u.entityChange.rec.base.y;
        rec[2] = evt->u.entityChange.rec.base.owner;
        rec[3] = evt->u.entityChange.rec.base.armour;
        rec[4] = evt->u.entityChange.rec.base.shells;
        rec[5] = evt->u.entityChange.rec.base.mines;
        break;
    case ENTITY_KIND_START:
        rec[0] = evt->u.entityChange.rec.start.x;
        rec[1] = evt->u.entityChange.rec.start.y;
        rec[2] = evt->u.entityChange.rec.start.dir;
        break;
    default:
        /* No record to place. Sending the header alone would decode as a
         * kind this codec does know and name an item in the wrong list, so
         * there is nothing here to deliver. */
        return ENCODE_SKIP;
    }
    *outLen = ENTITY_CHANGE_BODY_LEN;
    return ENCODE_OK;
}

static bool decodeEntityChangeBody(const uint8_t *buf, size_t len,
                                   ControlEvent *outEvt) {
    const uint8_t *rec;
    if (len != ENTITY_CHANGE_BODY_LEN) return false;
    rec = buf + ENTITY_CHANGE_REC_OFF;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_ENTITY_CHANGE;
    outEvt->u.entityChange.kind  = buf[0];
    outEvt->u.entityChange.index = buf[1];
    outEvt->u.entityChange.added = buf[2] ? 1u : 0u;
    switch (buf[0]) {
    case ENTITY_KIND_PILL:
        outEvt->u.entityChange.rec.pill.x      = rec[0];
        outEvt->u.entityChange.rec.pill.y      = rec[1];
        outEvt->u.entityChange.rec.pill.owner  = rec[2];
        outEvt->u.entityChange.rec.pill.armour = rec[3];
        outEvt->u.entityChange.rec.pill.speed  = rec[4];
        outEvt->u.entityChange.rec.pill.inTank = rec[5] ? 1u : 0u;
        break;
    case ENTITY_KIND_BASE:
        outEvt->u.entityChange.rec.base.x      = rec[0];
        outEvt->u.entityChange.rec.base.y      = rec[1];
        outEvt->u.entityChange.rec.base.owner  = rec[2];
        outEvt->u.entityChange.rec.base.armour = rec[3];
        outEvt->u.entityChange.rec.base.shells = rec[4];
        outEvt->u.entityChange.rec.base.mines  = rec[5];
        break;
    case ENTITY_KIND_START:
        outEvt->u.entityChange.rec.start.x   = rec[0];
        outEvt->u.entityChange.rec.start.y   = rec[1];
        outEvt->u.entityChange.rec.start.dir = rec[2];
        break;
    default:
        /* A kind with no list behind it: refuse rather than hand the
         * dispatcher an item it would have to guess the home of. */
        return false;
    }
    return true;
}

/* CTRL_ENTITY_SYNC body wire format (fixed length):
 *   [pills 2 BE] [bases 2 BE] [starts 2 BE]
 * One mask per list, bit i standing for index i, 0 based. Every list holds
 * at most 16 items, so the whole of one fits a u16 and the body is one size
 * whatever the lists hold; the decoder rejects any other length outright.
 * Delivered body-only on CHANNEL_CONTROL, as CTRL_ENTITY_CHANGE is: there is
 * no full-packet wrapper or PACKET_* type for this event. */
#define ENTITY_SYNC_BODY_LEN 6

/* recipient: safe — ignored. Which items are on the map is public. */
static EncodeResult encodeEntitySyncBody(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)recipient;
    if (bufCap < ENTITY_SYNC_BODY_LEN) return ENCODE_OVERFLOW;
    packU16(buf,     evt->u.entitySync.pills);
    packU16(buf + 2, evt->u.entitySync.bases);
    packU16(buf + 4, evt->u.entitySync.starts);
    *outLen = ENTITY_SYNC_BODY_LEN;
    return ENCODE_OK;
}

static bool decodeEntitySyncBody(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    if (len != ENTITY_SYNC_BODY_LEN) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_ENTITY_SYNC;
    outEvt->u.entitySync.pills  = unpackU16(buf);
    outEvt->u.entitySync.bases  = unpackU16(buf + 2);
    outEvt->u.entitySync.starts = unpackU16(buf + 4);
    return true;
}

/* PACKET_LOBBY_MAP_CHANGE wire format: header only (no payload).
 * The lobbyMapChange union member carries no fields — receipt of
 * the packet is itself the signal that the server has loaded a new
 * map and the client should reset and re-download. */

/* recipient: safe — ignored. */
static EncodeResult encodeLobbyMapChangeBody(const ControlEvent *evt,
                                             const struct UdpServerClient *recipient,
                                             uint8_t *buf, size_t bufCap,
                                             size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap;
    *outLen = 0;
    return ENCODE_OK;
}

static EncodeResult encodeLobbyMapChange(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_MAP_CHANGE, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeLobbyMapChangeBody(evt, recipient,
                                              buf + PACKET_HEADER_SIZE,
                                              bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeBalanceProposalBody(const ControlEvent *evt,
                                              const struct UdpServerClient *recipient,
                                              uint8_t *buf, size_t bufCap,
                                              size_t *outLen) {
    (void)recipient;
    if (bufCap < MAX_TANKS) return ENCODE_OVERFLOW;
    memcpy(buf, evt->u.balanceProposal.teamForSlot, MAX_TANKS);
    *outLen = MAX_TANKS;
    return ENCODE_OK;
}

static EncodeResult encodeBalanceProposal(const ControlEvent *evt,
                                          const struct UdpServerClient *recipient,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_BALANCE_PROPOSAL, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeBalanceProposalBody(evt, recipient,
                                               buf + PACKET_HEADER_SIZE,
                                               bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeMapSkipStateBody(const ControlEvent *evt,
                                           const struct UdpServerClient *recipient,
                                           uint8_t *buf, size_t bufCap,
                                           size_t *outLen) {
    (void)recipient;
    if (bufCap < MAX_TANKS) return ENCODE_OVERFLOW;
    memcpy(buf, evt->u.mapSkipState.votes, MAX_TANKS);
    *outLen = MAX_TANKS;
    return ENCODE_OK;
}

static EncodeResult encodeMapSkipState(const ControlEvent *evt,
                                       const struct UdpServerClient *recipient,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_MAP_SKIP_STATE, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeMapSkipStateBody(evt, recipient,
                                            buf + PACKET_HEADER_SIZE,
                                            bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* CTRL_GAME_PHASE_* — four sibling types, three of which (LOBBY,
 * RUNNING, GAME_OVER) carry no payload. Only COUNTDOWN puts a byte
 * on the wire (the seconds remaining). The full-packet wrapper
 * encodeGamePhase preserves today's direct-send behavior: COUNTDOWN
 * → PACKET_COUNTDOWN, RUNNING → PACKET_GAME_START, LOBBY/GAME_OVER
 * → ENCODE_SKIP (LOBBY has no wire form at all; CTRL_GAME_PHASE_GAME_OVER
 * is the phase transition — CTRL_GAME_OVER owns PACKET_GAME_OVER, so
 * emitting one here would double-send). */

/* recipient: safe — ignored. */
static EncodeResult encodeGamePhaseLobbyBody(const ControlEvent *evt,
                                             const struct UdpServerClient *recipient,
                                             uint8_t *buf, size_t bufCap,
                                             size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap;
    *outLen = 0;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeGamePhaseCountdownBody(const ControlEvent *evt,
                                                 const struct UdpServerClient *recipient,
                                                 uint8_t *buf, size_t bufCap,
                                                 size_t *outLen) {
    (void)recipient;
    if (bufCap < 1) return ENCODE_OVERFLOW;
    buf[0] = (uint8_t)evt->u.gamePhase.countdownSeconds;
    *outLen = 1;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeGamePhaseRunningBody(const ControlEvent *evt,
                                               const struct UdpServerClient *recipient,
                                               uint8_t *buf, size_t bufCap,
                                               size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap;
    *outLen = 0;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeGamePhaseGameOverBody(const ControlEvent *evt,
                                                const struct UdpServerClient *recipient,
                                                uint8_t *buf, size_t bufCap,
                                                size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap;
    *outLen = 0;
    return ENCODE_OK;
}

static EncodeResult encodeGamePhase(const ControlEvent *evt,
                                    const struct UdpServerClient *recipient,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    switch (evt->type) {
        case CTRL_GAME_PHASE_COUNTDOWN: {
            if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
            packHeader(buf, PACKET_COUNTDOWN, 0);
            size_t bodyLen = 0;
            EncodeResult r = encodeGamePhaseCountdownBody(
                evt, recipient,
                buf + PACKET_HEADER_SIZE,
                bufCap - PACKET_HEADER_SIZE, &bodyLen);
            if (r != ENCODE_OK) return r;
            *outLen = PACKET_HEADER_SIZE + bodyLen;
            return ENCODE_OK;
        }
        case CTRL_GAME_PHASE_RUNNING: {
            if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
            packHeader(buf, PACKET_GAME_START, 0);
            size_t bodyLen = 0;
            EncodeResult r = encodeGamePhaseRunningBody(
                evt, recipient,
                buf + PACKET_HEADER_SIZE,
                bufCap - PACKET_HEADER_SIZE, &bodyLen);
            if (r != ENCODE_OK) return r;
            *outLen = PACKET_HEADER_SIZE + bodyLen;
            return ENCODE_OK;
        }
        case CTRL_GAME_PHASE_LOBBY:
        case CTRL_GAME_PHASE_GAME_OVER:
        default:
            return ENCODE_SKIP;
    }
}

/* recipient: safe — ignored. */
static EncodeResult encodeGameOverBody(const ControlEvent *evt,
                                       const struct UdpServerClient *recipient,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap;
    *outLen = 0;
    return ENCODE_OK;
}

static EncodeResult encodeGameOver(const ControlEvent *evt,
                                   const struct UdpServerClient *recipient,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_GAME_OVER, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeGameOverBody(evt, recipient,
                                        buf + PACKET_HEADER_SIZE,
                                        bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeServerShutdownBody(const ControlEvent *evt,
                                             const struct UdpServerClient *recipient,
                                             uint8_t *buf, size_t bufCap,
                                             size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap;
    *outLen = 0;
    return ENCODE_OK;
}

static EncodeResult encodeServerShutdown(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_SERVER_SHUTDOWN, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeServerShutdownBody(evt, recipient,
                                              buf + PACKET_HEADER_SIZE,
                                              bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeLobbySyncCompleteBody(const ControlEvent *evt,
                                                const struct UdpServerClient *recipient,
                                                uint8_t *buf, size_t bufCap,
                                                size_t *outLen) {
    (void)evt; (void)recipient; (void)buf; (void)bufCap;
    *outLen = 0;
    return ENCODE_OK;
}

static EncodeResult encodeLobbySyncComplete(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_SYNC_COMPLETE, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeLobbySyncCompleteBody(evt, recipient,
                                                 buf + PACKET_HEADER_SIZE,
                                                 bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* PACKET_CHAT_BROADCAST wire format (variable length, three subtypes
 * discriminated by fromPlayer):
 *   [header 8] [fromPlayer 1] [destPlayer 1] [body bodyLen]
 * body is opaque to the codec — raw text when fromPlayer < MAX_TANKS
 * or == 0xFE, packed langid+args when fromPlayer == 0xFF.  Per-
 * recipient filtering (broadcast-skip-sender, unicast-only-dest)
 * lives in udpClientDeliverControl, matching CTRL_ALLIANCE_REQUEST. */

/* recipient: safe — ignored. */
static EncodeResult encodeChatBody(const ControlEvent *evt,
                                   const struct UdpServerClient *recipient,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    (void)recipient;
    if (evt->u.chat.bodyLen > CHAT_BODY_MAX) return ENCODE_OVERFLOW;
    const size_t needed = 2 + evt->u.chat.bodyLen;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    buf[0] = evt->u.chat.fromPlayer;
    buf[1] = evt->u.chat.destPlayer;
    if (evt->u.chat.bodyLen > 0) {
        memcpy(buf + 2, evt->u.chat.body, evt->u.chat.bodyLen);
    }
    *outLen = needed;
    return ENCODE_OK;
}

static EncodeResult encodeChat(const ControlEvent *evt,
                               const struct UdpServerClient *recipient,
                               uint8_t *buf, size_t bufCap,
                               size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_CHAT_BROADCAST, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeChatBody(evt, recipient,
                                    buf + PACKET_HEADER_SIZE,
                                    bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* CTRL_SERVER_TEXT → PACKET_CHAT_BROADCAST(fromPlayer=0xFE).  The wire
 * format is the same as a regular chat broadcast so existing UDP
 * clients keep their PACKET_CHAT_BROADCAST handler unchanged.
 * In-process subscribers consume CTRL_SERVER_TEXT directly. */

/* recipient: safe — ignored. */
static EncodeResult encodeServerTextBody(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)recipient;
    size_t textLen = strnlen(evt->u.serverText.text,
                             sizeof(evt->u.serverText.text));
    if (textLen > CHAT_BODY_MAX) textLen = CHAT_BODY_MAX;
    const size_t needed = 2 + textLen;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    buf[0] = 0xFE; /* server raw English */
    buf[1] = 0xFF; /* broadcast */
    if (textLen > 0) {
        memcpy(buf + 2, evt->u.serverText.text, textLen);
    }
    *outLen = needed;
    return ENCODE_OK;
}

static EncodeResult encodeServerText(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_CHAT_BROADCAST, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeServerTextBody(evt, recipient,
                                          buf + PACKET_HEADER_SIZE,
                                          bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* Pair for encodeServerTextBody: reads [0xFE][0xFF][text...] back into
 * a CTRL_SERVER_TEXT event. Without this, server-text broadcasts (the
 * "<team> has won the game" message and similar) ride CONTROL_TICK but
 * the client has no decoder, drops the event, and previously stalled
 * the reliable queue. */
static bool decodeServerTextBody(const uint8_t *buf, size_t bodyLen,
                                 ControlEvent *outEvt) {
    if (buf == NULL || outEvt == NULL) return false;
    if (bodyLen < 2) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_SERVER_TEXT;
    outEvt->u.serverText.destPlayer = 0xFF;  /* every recipient: udpClientDeliverControl
                                                already applied the destination filter
                                                server-side, so an event that arrives
                                                here is addressed to this client. */
    size_t textLen = bodyLen - 2;
    if (textLen >= sizeof(outEvt->u.serverText.text)) {
        textLen = sizeof(outEvt->u.serverText.text) - 1;
    }
    if (textLen > 0) {
        memcpy(outEvt->u.serverText.text, buf + 2, textLen);
    }
    outEvt->u.serverText.text[textLen] = '\0';
    return true;
}

/* Wire: [header 8] [kind 1] [active 1] [triggerSrc 1] [teamId 1]
 *   [threshold 1] [yes 1] [no 1] [eligible 1] [secsRemaining 1]
 *   [votes 2 BE] — 11-byte body. Same shape on every recipient. */

/* recipient: safe — ignored. */
static EncodeResult encodeGameVoteStateBody(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)recipient;
    if (bufCap < 11) return ENCODE_OVERFLOW;
    buf[0]  = evt->u.gameVoteState.kind;
    buf[1]  = evt->u.gameVoteState.active;
    buf[2]  = evt->u.gameVoteState.triggerSrc;
    buf[3]  = evt->u.gameVoteState.teamId;
    buf[4]  = evt->u.gameVoteState.threshold;
    buf[5]  = evt->u.gameVoteState.yesCount;
    buf[6]  = evt->u.gameVoteState.noCount;
    buf[7]  = evt->u.gameVoteState.eligibleCount;
    buf[8]  = evt->u.gameVoteState.secondsRemaining;
    packU16(buf + 9, evt->u.gameVoteState.votes);
    *outLen = 11;
    return ENCODE_OK;
}

static EncodeResult encodeGameVoteState(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_GAME_VOTE_STATE, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeGameVoteStateBody(evt, recipient,
                                             buf + PACKET_HEADER_SIZE,
                                             bufCap - PACKET_HEADER_SIZE, &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* Wire: [header 8] [origCmdSeq 4 BE] [origCmdType 1] [reasonCode 1]
 * [origSlot 1] — 7-byte body. serverSimApplyCommand publishes this on
 * every non-CMD_OK return; clients correlate by origCmdSeq and dismiss
 * when stale. Per-recipient filtering lives in udpClientDeliverControl
 * (matching CTRL_ALLIANCE_REQUEST) so non-originator slots never see
 * the event. */

/* recipient: safe — ignored. */
static EncodeResult encodeCommandRejectedBody(const ControlEvent *evt,
                                              const struct UdpServerClient *recipient,
                                              uint8_t *buf, size_t bufCap,
                                              size_t *outLen) {
    (void)recipient;
    if (bufCap < 7) return ENCODE_OVERFLOW;
    packU32(buf, evt->u.commandRejected.origCmdSeq);
    buf[4] = evt->u.commandRejected.origCmdType;
    buf[5] = evt->u.commandRejected.reasonCode;
    buf[6] = evt->u.commandRejected.origSlot;
    *outLen = 7;
    return ENCODE_OK;
}

static EncodeResult encodeCommandRejected(const ControlEvent *evt,
                                          const struct UdpServerClient *recipient,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_COMMAND_REJECTED, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeCommandRejectedBody(evt, recipient,
                                               buf + PACKET_HEADER_SIZE,
                                               bufCap - PACKET_HEADER_SIZE,
                                               &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* recipient: safe — ignored. */
static EncodeResult encodeBalanceFailedBody(const ControlEvent *evt,
                                            const struct UdpServerClient *recipient,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)recipient;
    if (bufCap < 1) return ENCODE_OVERFLOW;
    buf[0] = evt->u.balanceFailed.reasonCode;
    *outLen = 1;
    return ENCODE_OK;
}

static EncodeResult encodeBalanceFailed(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_BALANCE_FAILED, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeBalanceFailedBody(evt, recipient,
                                             buf + PACKET_HEADER_SIZE,
                                             bufCap - PACKET_HEADER_SIZE,
                                             &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* Wire: [header 8] [fireTick 4 BE] [impactWX 2 BE] [impactWY 2 BE]
 * [owner 1] [outcome 1] — 10-byte body. Unicast to the shell's owner;
 * per-recipient filtering lives in udpClientDeliverControl (matching
 * CTRL_COMMAND_REJECTED) so non-owner slots never see it. */

/* recipient: safe — ignored. */
static EncodeResult encodeShellDeathBody(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)recipient;
    if (bufCap < 10) return ENCODE_OVERFLOW;
    packU32(buf, evt->u.shellDeath.fireTick);
    packU16(buf + 4, evt->u.shellDeath.impactWX);
    packU16(buf + 6, evt->u.shellDeath.impactWY);
    buf[8] = evt->u.shellDeath.owner;
    buf[9] = evt->u.shellDeath.outcome;
    *outLen = 10;
    return ENCODE_OK;
}

static EncodeResult encodeShellDeath(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_SHELL_DEATH, 0);
    size_t bodyLen = 0;
    EncodeResult r = encodeShellDeathBody(evt, recipient,
                                          buf + PACKET_HEADER_SIZE,
                                          bufCap - PACKET_HEADER_SIZE,
                                          &bodyLen);
    if (r != ENCODE_OK) return r;
    *outLen = PACKET_HEADER_SIZE + bodyLen;
    return ENCODE_OK;
}

/* CTRL_CHANNEL_RESET — carrier-only (channel 2): a channel mask byte followed
 * by one big-endian u32 baseline for each set mask bit, in ascending channel
 * order (bit 0 -> game, bit 1 -> map, bit 3 -> bulk; bit 2 is the control
 * carrier and is never reset). No full-packet wrapper — this event has no
 * standalone wire form, it rides the reliable control channel exclusively. The
 * recipient argument is ignored; the per-client values live in the event. */

/* recipient: safe — ignored. */
static EncodeResult encodeChannelResetBody(const ControlEvent *evt,
                                           const struct UdpServerClient *recipient,
                                           uint8_t *buf, size_t bufCap,
                                           size_t *outLen) {
    uint8_t mask = evt->u.channelReset.channelMask;
    const uint32_t baselines[4] = {
        evt->u.channelReset.ch0Baseline,
        evt->u.channelReset.ch1Baseline,
        0u,                              /* ch2 (control) never carried */
        evt->u.channelReset.ch3Baseline,
    };
    size_t pos = 0;
    int c;
    (void)recipient;
    if (bufCap < 1) return ENCODE_OVERFLOW;
    buf[pos++] = mask;
    for (c = 0; c < 4; c++) {
        if (mask & (1u << c)) {
            if (pos + 4 > bufCap) return ENCODE_OVERFLOW;
            packU32(buf + pos, baselines[c]);
            pos += 4;
        }
    }
    *outLen = pos;
    return ENCODE_OK;
}

/* ================================================================
 * Decoders — body-only (the existing wire-packet dispatcher in
 * transportControlCodecDecoder already strips the PacketHeader
 * before calling into the codec). For PACKET_ALLIANCE_UPDATE the
 * thin wrapper decodeAllianceUpdate consumes the 1-byte sub-type
 * discriminator and delegates to one of three body decoders.
 *
 * Body decoder names match their ControlEventType for use in
 * s_bodyDecoders[].
 * ================================================================ */

static bool decodeAllianceRequestBody(const uint8_t *buf, size_t len,
                                      ControlEvent *outEvt) {
    if (len < ALLIANCE_BODY_PAYLOAD) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_ALLIANCE_REQUEST;
    outEvt->u.allianceRequest.fromPlayer = buf[0];
    outEvt->u.allianceRequest.toPlayer   = buf[1];
    return true;
}

static bool decodeAllianceAcceptBody(const uint8_t *buf, size_t len,
                                     ControlEvent *outEvt) {
    if (len < ALLIANCE_BODY_PAYLOAD) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_ALLIANCE_ACCEPT;
    outEvt->u.allianceAccept.acceptedBy = buf[0];
    outEvt->u.allianceAccept.newMember  = buf[1];
    return true;
}

static bool decodeAllianceLeaveBody(const uint8_t *buf, size_t len,
                                    ControlEvent *outEvt) {
    if (len < ALLIANCE_BODY_PAYLOAD) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_ALLIANCE_LEAVE;
    outEvt->u.allianceLeave.playerNum = buf[0];
    return true;
}

static bool decodeAllianceResetBody(const uint8_t *buf, size_t len,
                                    ControlEvent *outEvt) {
    if (len < ALLIANCE_RESET_BODY_PAYLOAD) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_ALLIANCE_RESET;
    for (int i = 0; i < MAX_TANKS; i++) {
        outEvt->u.allianceReset.allies[i] = unpackU16(buf + 2 * i);
    }
    return true;
}

static bool decodeAllianceUpdate(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    if (len < ALLIANCE_UPDATE_PAYLOAD) return false;
    switch (buf[0]) {
        case ALLIANCE_EVENT_REQUEST:
            return decodeAllianceRequestBody(buf + 1, len - 1, outEvt);
        case ALLIANCE_EVENT_ACCEPT:
            return decodeAllianceAcceptBody(buf + 1, len - 1, outEvt);
        case ALLIANCE_EVENT_LEAVE:
            return decodeAllianceLeaveBody(buf + 1, len - 1, outEvt);
        case ALLIANCE_EVENT_RESET:
            return decodeAllianceResetBody(buf + 1, len - 1, outEvt);
        default:
            return false;
    }
}

static bool decodePlayerJoinBody(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    /* Layout matches encodePlayerJoinBody's wire format. */
    const size_t fixedLen = 1 + PACKET_MAX_PLAYER_NAME + 2 + 1 + 1 + 1;
    if (len < fixedLen) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_PLAYER_JOIN;
    size_t pos = 0;
    outEvt->u.playerJoin.playerNum = buf[pos++];
    memcpy(outEvt->u.playerJoin.name, buf + pos, PACKET_MAX_PLAYER_NAME);
    outEvt->u.playerJoin.name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    pos += PACKET_MAX_PLAYER_NAME;
    outEvt->u.playerJoin.country[0] = (char)buf[pos++];
    outEvt->u.playerJoin.country[1] = (char)buf[pos++];
    outEvt->u.playerJoin.country[2] = '\0';
    outEvt->u.playerJoin.clientType  = buf[pos++];
    outEvt->u.playerJoin.clientFlags = buf[pos++];
    {
        BYTE numAllies = buf[pos++];
        if (numAllies > MAX_TANKS) numAllies = MAX_TANKS;
        if (pos + numAllies > len) return false;
        outEvt->u.playerJoin.numAllies = numAllies;
        if (numAllies > 0) {
            memcpy(outEvt->u.playerJoin.allies, buf + pos, numAllies);
        }
    }
    return true;
}

static bool decodePlayerNameBody(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    if (len < (size_t)(1 + PACKET_MAX_PLAYER_NAME)) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_PLAYER_NAME;
    outEvt->u.playerName.playerNum = buf[0];
    memcpy(outEvt->u.playerName.name, buf + 1, PACKET_MAX_PLAYER_NAME);
    outEvt->u.playerName.name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    return true;
}

static bool decodePlayerLeaveBody(const uint8_t *buf, size_t len,
                                  ControlEvent *outEvt) {
    /* Layout matches encodePlayerLeaveBody's wire format. */
    const size_t fixedLen = 1 + PACKET_MAX_PLAYER_NAME + 2;
    if (len < fixedLen) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_PLAYER_LEAVE;
    size_t pos = 0;
    outEvt->u.playerLeave.playerNum = buf[pos++];
    memcpy(outEvt->u.playerLeave.name, buf + pos, PACKET_MAX_PLAYER_NAME);
    outEvt->u.playerLeave.name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    pos += PACKET_MAX_PLAYER_NAME;
    outEvt->u.playerLeave.country[0] = (char)buf[pos++];
    outEvt->u.playerLeave.country[1] = (char)buf[pos++];
    outEvt->u.playerLeave.country[2] = '\0';
    return true;
}

static bool decodeLobbySlotBody(const uint8_t *buf, size_t len,
                                ControlEvent *outEvt) {
    if (len < 2) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_SLOT;
    size_t pos = 0;
    BYTE playerNum = buf[pos++];
    if (playerNum >= MAX_TANKS) return false;
    outEvt->u.lobbySlot.playerNum = playerNum;
    ClientLobbySlot *slot = &outEvt->u.lobbySlot.slot;
    slot->connected = buf[pos++] ? true : false;
    if (slot->connected) {
        if (pos + 1 > len) return false;
        uint8_t nameLen = buf[pos++];
        if (nameLen > PACKET_MAX_PLAYER_NAME - 1) return false;
        if (pos + nameLen + 10 > len) return false;
        if (nameLen > 0) memcpy(slot->playerName, buf + pos, nameLen);
        slot->playerName[nameLen] = '\0';
        pos += nameLen;
        slot->teamNumber = buf[pos++];
        slot->ready  = buf[pos++] ? true : false;
        slot->isBot  = buf[pos++] ? true : false;
        slot->pingMs = (uint16_t)(buf[pos] << 8 | buf[pos + 1]);
        pos += 2;
        slot->countryCode[0] = (char)buf[pos++];
        slot->countryCode[1] = (char)buf[pos++];
        slot->countryCode[2] = '\0';
        slot->clientType  = buf[pos++];
        slot->clientFlags = buf[pos++];
        if (slot->clientType >= CLIENT_TYPE_COUNT)
            slot->clientType = CLIENT_TYPE_UNKNOWN;
        slot->startIdx = buf[pos++];
    }
    return true;
}

static bool decodeSpectatorSlotBody(const uint8_t *buf, size_t len,
                                    ControlEvent *outEvt) {
    if (len < 2) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_SPECTATOR_SLOT;
    size_t pos = 0;
    uint8_t specIdx = buf[pos++];
    if (specIdx >= MAX_SPECTATORS) return false;
    outEvt->u.spectatorSlot.specIdx = specIdx;
    ClientSpectatorSlot *slot = &outEvt->u.spectatorSlot.slot;
    slot->connected = buf[pos++] ? true : false;
    if (slot->connected) {
        if (pos + 1 > len) return false;
        uint8_t nameLen = buf[pos++];
        if (nameLen > PACKET_MAX_PLAYER_NAME - 1) return false;
        if (pos + nameLen + 4 > len) return false;
        if (nameLen > 0) memcpy(slot->playerName, buf + pos, nameLen);
        slot->playerName[nameLen] = '\0';
        pos += nameLen;
        slot->countryCode[0] = (char)buf[pos++];
        slot->countryCode[1] = (char)buf[pos++];
        slot->countryCode[2] = '\0';
        slot->clientType  = buf[pos++];
        slot->clientFlags = buf[pos++];
        if (slot->clientType >= CLIENT_TYPE_COUNT)
            slot->clientType = CLIENT_TYPE_UNKNOWN;
    }
    return true;
}

static bool decodeSpectatorChatBody(const uint8_t *buf, size_t len,
                                    ControlEvent *outEvt) {
    if (len < 2) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_SPECTATOR_CHAT;
    size_t pos = 0;
    uint8_t specIdx = buf[pos++];
    if (specIdx >= MAX_SPECTATORS) return false;
    uint8_t msgLen = buf[pos++];
    if (pos + msgLen > len) return false;
    if (msgLen > PACKET_MAX_CHAT_MESSAGE) return false;
    outEvt->u.spectatorChat.specIdx = specIdx;
    outEvt->u.spectatorChat.bodyLen = msgLen;
    if (msgLen > 0) memcpy(outEvt->u.spectatorChat.body, buf + pos, msgLen);
    return true;
}

static bool decodeLobbySettingsBody(const uint8_t *buf, size_t len,
                                    ControlEvent *outEvt) {
    if (len < LOBBY_SETTINGS_WIRE_PAYLOAD_BASE) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_SETTINGS;
    size_t pos = 0;
    strncpy(outEvt->u.lobbySettings.mapName,
            (const char *)(buf + pos), MAP_STR_SIZE - 1);
    outEvt->u.lobbySettings.mapName[MAP_STR_SIZE - 1] = '\0';
    pos += MAP_STR_SIZE;
    outEvt->u.lobbySettings.lobbyGameType    = (gameType)buf[pos++];
    outEvt->u.lobbySettings.lobbyHiddenMines = buf[pos++] ? true : false;
    outEvt->u.lobbySettings.lobbyAiType      = buf[pos++];
    outEvt->u.lobbySettings.lobbyTimeLimit   = (int32_t)unpackU32(buf + pos);
    pos += 4;
    outEvt->u.lobbySettings.lobbyPillCount   = buf[pos++];
    outEvt->u.lobbySettings.lobbyBaseCount   = buf[pos++];
    outEvt->u.lobbySettings.lobbyStartCount  = buf[pos++];
    outEvt->u.lobbySettings.mapSkipAvailable = buf[pos++] ? true : false;
    outEvt->u.lobbySettings.netStat          = (netStatus)buf[pos++];
    outEvt->u.lobbySettings.hasLobby         = buf[pos++] ? true : false;
    outEvt->u.lobbySettings.lobbyOpenHost            = buf[pos++] ? true : false;
    outEvt->u.lobbySettings.lobbyAutoLockOnGameStart = buf[pos++] ? true : false;
    outEvt->u.lobbySettings.lobbyServerLocks         = unpackU16(buf + pos);
    pos += 2;
    if (len >= pos + 1) {
        outEvt->u.lobbySettings.lobbyRanked = buf[pos++] ? true : false;
    }
    if (len >= pos + 1) {
        outEvt->u.lobbySettings.lobbyAllowNewPlayers = buf[pos++] ? true : false;
    }
    if (len >= pos + 1) {
        outEvt->u.lobbySettings.lobbyWbnAvailable = buf[pos++] ? true : false;
    }
    if (len >= pos + 1) {
        outEvt->u.lobbySettings.uploadPolicy = (UploadPolicy)buf[pos++];
    }
    if (len >= pos + 4) {
        outEvt->u.lobbySettings.lobbyStartDelay = (int32_t)unpackU32(buf + pos);
        pos += 4;
    }
    if (len >= pos + 1) {
        outEvt->u.lobbySettings.hostSlot = buf[pos++];
    }
    if (len >= pos + VIEW_CATEGORY_COUNT) {
        for (int vc = 0; vc < VIEW_CATEGORY_COUNT; vc++) {
            outEvt->u.lobbySettings.viewPolicy[vc] = (ViewPolicy)buf[pos++];
        }
    }
    if (len >= pos + (2 * VIEW_CATEGORY_COUNT)) {
        for (int vc = 0; vc < VIEW_CATEGORY_COUNT; vc++) {
            outEvt->u.lobbySettings.viewDecaySecs[vc] = unpackU16(buf + pos);
            pos += 2;
        }
    }
    if (len >= pos + 1) {
        outEvt->u.lobbySettings.lobbyClassicMode = buf[pos++] ? true : false;
    }
    if (len >= pos + 1) {
        outEvt->u.lobbySettings.lobbyAlliesInTrees = buf[pos++] ? true : false;
    }
    if (len >= pos + 1) {
        /* A byte this build does not recognise reads as ON, so an unknown
         * mode leaves voice working rather than silently disabling it. */
        switch (buf[pos++]) {
        case (uint8_t)serverVoiceOff:
            outEvt->u.lobbySettings.voiceMode = serverVoiceOff;
            break;
        case (uint8_t)serverVoiceProximity:
            outEvt->u.lobbySettings.voiceMode = serverVoiceProximity;
            break;
        default:
            outEvt->u.lobbySettings.voiceMode = serverVoiceOn;
            break;
        }
    }
    return true;
}

static bool decodeLobbyTeamMetaBody(const uint8_t *buf, size_t len,
                                    ControlEvent *outEvt) {
    /* Layout matches encodeLobbyTeamMetaBody. */
    if (len < 5) return false;
    uint8_t teamId    = buf[0];
    uint8_t color     = buf[1];
    uint8_t pool      = buf[2];
    uint8_t startSide = buf[3];
    uint8_t nameLen   = buf[4];
    if (teamId == 0 || teamId >= MAX_TANKS) return false;
    if (nameLen > LOBBY_TEAM_NAME_LEN - 1) return false;
    if (len < (size_t)(5 + nameLen)) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_TEAM_META;
    outEvt->u.lobbyTeamMeta.teamId     = teamId;
    outEvt->u.lobbyTeamMeta.color      = color;
    outEvt->u.lobbyTeamMeta.namingPool = pool;
    outEvt->u.lobbyTeamMeta.startSide  = startSide;
    if (nameLen > 0) {
        memcpy(outEvt->u.lobbyTeamMeta.name, buf + 5, nameLen);
    }
    outEvt->u.lobbyTeamMeta.name[nameLen] = '\0';
    /* in_use is not on the wire — a team with any non-default field
     * is in use. */
    outEvt->u.lobbyTeamMeta.in_use =
        (nameLen > 0 || color != 0 || pool != 0 || startSide != 0) ? 1 : 0;
    return true;
}

static bool decodeLobbyBotConfigBody(const uint8_t *buf, size_t len,
                                     ControlEvent *outEvt) {
    /* Layout matches encodeLobbyBotConfigBody. */
    if (len < 4) return false;
    uint8_t slot    = buf[0];
    uint8_t diff    = buf[1];
    uint8_t pers    = buf[2];
    uint8_t nameLen = buf[3];
    if (slot >= MAX_TANKS) return false;
    if (nameLen > PACKET_MAX_PLAYER_NAME - 1) return false;
    if (len < (size_t)(4 + nameLen)) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_BOT_CONFIG;
    outEvt->u.lobbyBotConfig.slot        = slot;
    outEvt->u.lobbyBotConfig.difficulty  = diff;
    outEvt->u.lobbyBotConfig.personality = pers;
    if (nameLen > 0) {
        memcpy(outEvt->u.lobbyBotConfig.name, buf + 4, nameLen);
    }
    outEvt->u.lobbyBotConfig.name[nameLen] = '\0';
    return true;
}

static bool decodeLobbyBotBrainBody(const uint8_t *buf, size_t len,
                                    ControlEvent *outEvt) {
    /* Layout matches encodeLobbyBotBrainBody: [slot 1][brainIdx 1]. */
    if (len < 2) return false;
    uint8_t slot     = buf[0];
    uint8_t brainIdx = buf[1];
    if (slot >= MAX_TANKS) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_BOT_BRAIN;
    outEvt->u.lobbyBotBrain.slot     = slot;
    outEvt->u.lobbyBotBrain.brainIdx = brainIdx;
    return true;
}

static bool decodeLobbyBrainListBody(const uint8_t *buf, size_t len,
                                     ControlEvent *outEvt) {
    /* Layout matches encodeLobbyBrainListBody: [count 1] then per
     * entry [nameLen 1][name][verLen 1][version]. */
    if (len < 1) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_BRAIN_LIST;
    BrainList *list = &outEvt->u.lobbyBrainList.list;
    size_t pos = 0;
    uint8_t cnt = buf[pos++];
    if (cnt > BRAIN_LIST_MAX) cnt = BRAIN_LIST_MAX;
    int written = 0;
    for (uint8_t i = 0; i < cnt; i++) {
        if (pos + 1 > len) return false;
        uint8_t n = buf[pos++];
        if (n >= BRAIN_LIST_NAME_LEN || pos + n > len) return false;
        if (n > 0) memcpy(list->entries[written].name, buf + pos, n);
        list->entries[written].name[n] = '\0';
        pos += n;
        if (pos + 1 > len) return false;
        uint8_t v = buf[pos++];
        if (v >= BRAIN_LIST_VER_LEN || pos + v > len) return false;
        if (v > 0) memcpy(list->entries[written].version, buf + pos, v);
        list->entries[written].version[v] = '\0';
        pos += v;
        written++;
    }
    list->count = written;
    return true;
}

static bool decodeLobbyBotPoolChunkBody(const uint8_t *buf, size_t len,
                                        ControlEvent *outEvt) {
    /* Layout: [seq 1][count 1][fragLen 2 BE][frag fragLen]. */
    if (len < 4) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_BOT_POOL_CHUNK;
    size_t pos = 0;
    outEvt->u.lobbyBotPoolChunk.seq   = buf[pos++];
    outEvt->u.lobbyBotPoolChunk.count = buf[pos++];
    uint16_t fl = (uint16_t)(((uint16_t)buf[pos] << 8) | buf[pos + 1]);
    pos += 2;
    if (fl > LOBBY_BOT_POOL_CHUNK_FRAG_MAX) return false;
    if (pos + fl > len) return false;
    outEvt->u.lobbyBotPoolChunk.fragLen = fl;
    if (fl > 0) memcpy(outEvt->u.lobbyBotPoolChunk.frag, buf + pos, fl);
    return true;
}

static bool decodeLobbyMapChangeBody(const uint8_t *buf, size_t len,
                                     ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_MAP_CHANGE;
    return true;
}

static bool decodeBalanceProposalBody(const uint8_t *buf, size_t len,
                                      ControlEvent *outEvt) {
    if (len < MAX_TANKS) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_BALANCE_PROPOSAL;
    memcpy(outEvt->u.balanceProposal.teamForSlot, buf, MAX_TANKS);
    return true;
}

static bool decodeMapSkipStateBody(const uint8_t *buf, size_t len,
                                   ControlEvent *outEvt) {
    if (len < MAX_TANKS) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_MAP_SKIP_STATE;
    memcpy(outEvt->u.mapSkipState.votes, buf, MAX_TANKS);
    return true;
}

static bool decodeGamePhaseLobbyBody(const uint8_t *buf, size_t len,
                                     ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_GAME_PHASE_LOBBY;
    return true;
}

static bool decodeGamePhaseCountdownBody(const uint8_t *buf, size_t len,
                                         ControlEvent *outEvt) {
    if (len < 1) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_GAME_PHASE_COUNTDOWN;
    outEvt->u.gamePhase.countdownSeconds = buf[0];
    return true;
}

static bool decodeGamePhaseRunningBody(const uint8_t *buf, size_t len,
                                       ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_GAME_PHASE_RUNNING;
    return true;
}

static bool decodeGamePhaseGameOverBody(const uint8_t *buf, size_t len,
                                        ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_GAME_PHASE_GAME_OVER;
    return true;
}

static bool decodeGameOverBody(const uint8_t *buf, size_t len,
                               ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_GAME_OVER;
    return true;
}

static bool decodeServerShutdownBody(const uint8_t *buf, size_t len,
                                     ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_SERVER_SHUTDOWN;
    return true;
}

static bool decodeLobbySyncCompleteBody(const uint8_t *buf, size_t len,
                                        ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_SYNC_COMPLETE;
    return true;
}

static bool decodeChatBody(const uint8_t *buf, size_t len,
                           ControlEvent *outEvt) {
    if (len < 2) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_CHAT;
    outEvt->u.chat.fromPlayer = buf[0];
    outEvt->u.chat.destPlayer = buf[1];
    {
        size_t bodyLen = len - 2;
        if (bodyLen > CHAT_BODY_MAX) bodyLen = CHAT_BODY_MAX;
        outEvt->u.chat.bodyLen = (uint16_t)bodyLen;
        if (bodyLen > 0) memcpy(outEvt->u.chat.body, buf + 2, bodyLen);
    }
    return true;
}

static bool decodeGameVoteStateBody(const uint8_t *buf, size_t len,
                                    ControlEvent *outEvt) {
    if (len < 11) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_GAME_VOTE_STATE;
    outEvt->u.gameVoteState.kind             = buf[0];
    outEvt->u.gameVoteState.active           = buf[1];
    outEvt->u.gameVoteState.triggerSrc       = buf[2];
    outEvt->u.gameVoteState.teamId           = buf[3];
    outEvt->u.gameVoteState.threshold        = buf[4];
    outEvt->u.gameVoteState.yesCount         = buf[5];
    outEvt->u.gameVoteState.noCount          = buf[6];
    outEvt->u.gameVoteState.eligibleCount    = buf[7];
    outEvt->u.gameVoteState.secondsRemaining = buf[8];
    outEvt->u.gameVoteState.votes            = unpackU16(buf + 9);
    return true;
}

static bool decodeCommandRejectedBody(const uint8_t *buf, size_t len,
                                      ControlEvent *outEvt) {
    if (len < 7) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_COMMAND_REJECTED;
    outEvt->u.commandRejected.origCmdSeq  = unpackU32(buf);
    outEvt->u.commandRejected.origCmdType = buf[4];
    outEvt->u.commandRejected.reasonCode  = buf[5];
    outEvt->u.commandRejected.origSlot    = buf[6];
    return true;
}

static bool decodeBalanceFailedBody(const uint8_t *buf, size_t len,
                                    ControlEvent *outEvt) {
    if (len < 1) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_BALANCE_FAILED;
    outEvt->u.balanceFailed.reasonCode = buf[0];
    return true;
}

static bool decodeShellDeathBody(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    if (len < 10) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_SHELL_DEATH;
    outEvt->u.shellDeath.fireTick = unpackU32(buf);
    outEvt->u.shellDeath.impactWX = unpackU16(buf + 4);
    outEvt->u.shellDeath.impactWY = unpackU16(buf + 6);
    outEvt->u.shellDeath.owner    = buf[8];
    outEvt->u.shellDeath.outcome  = buf[9];
    return true;
}

static bool decodeChannelResetBody(const uint8_t *buf, size_t len,
                                   ControlEvent *outEvt) {
    uint8_t mask;
    size_t pos = 1;
    int c;
    uint32_t *const fields[4] = {
        &outEvt->u.channelReset.ch0Baseline,
        &outEvt->u.channelReset.ch1Baseline,
        NULL,                            /* ch2 (control) never carried */
        &outEvt->u.channelReset.ch3Baseline,
    };
    if (len < 1) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_CHANNEL_RESET;
    mask = buf[0];
    for (c = 0; c < 4; c++) {
        if (mask & (1u << c)) {
            uint32_t v;
            if (pos + 4 > len) return false;
            v = unpackU32(buf + pos);
            pos += 4;
            if (fields[c] != NULL) *fields[c] = v;
        }
    }
    outEvt->u.channelReset.channelMask = mask;
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
    [CTRL_GAME_PHASE_LOBBY]      = encodeGamePhase,
    [CTRL_GAME_PHASE_COUNTDOWN]  = encodeGamePhase,
    [CTRL_GAME_PHASE_RUNNING]    = encodeGamePhase,
    [CTRL_GAME_PHASE_GAME_OVER]  = encodeGamePhase,
    [CTRL_GAME_OVER]          = encodeGameOver,
    [CTRL_SERVER_SHUTDOWN]    = encodeServerShutdown,
    [CTRL_LOBBY_SYNC_COMPLETE] = encodeLobbySyncComplete,
    [CTRL_CHAT]               = encodeChat,
    [CTRL_PLAYER_LEAVE]       = encodePlayerLeave,
    [CTRL_LOBBY_TEAM_META]    = encodeLobbyTeamMeta,
    [CTRL_LOBBY_BOT_CONFIG]   = encodeLobbyBotConfig,
    [CTRL_LOBBY_BOT_BRAIN]    = encodeLobbyBotBrain,
    [CTRL_LOBBY_BRAIN_LIST]   = encodeLobbyBrainList,
    [CTRL_LOBBY_BOT_POOL_CHUNK] = encodeLobbyBotPoolChunk,
    [CTRL_GAME_VOTE_STATE]    = encodeGameVoteState,
    [CTRL_SERVER_TEXT]        = encodeServerText,
    [CTRL_COMMAND_REJECTED]   = encodeCommandRejected,
    [CTRL_ALLIANCE_RESET]     = encodeAllianceReset,
    [CTRL_BALANCE_FAILED]     = encodeBalanceFailed,
    [CTRL_SHELL_DEATH]        = encodeShellDeath,
    [CTRL_ROUND_STATS]        = encodeRoundStats,
};

/* ================================================================
 * Body lookups — indexed by ControlEventType, 1:1 with s_encoders
 * above. Used by the reliable carrier path (Phase 4+); no callers
 * outside this file today. Same NULL slots as s_encoders for
 * variants that have no wire form (CTRL_MAP_DOWNLOAD_COMPLETE).
 * ================================================================ */

static const ControlEncodeBodyFn s_bodyEncoders[CTRL_EVENT_TYPE_COUNT] = {
    [CTRL_ALLIANCE_REQUEST]      = encodeAllianceRequestBody,
    [CTRL_ALLIANCE_ACCEPT]       = encodeAllianceAcceptBody,
    [CTRL_ALLIANCE_LEAVE]        = encodeAllianceLeaveBody,
    [CTRL_PLAYER_JOIN]           = encodePlayerJoinBody,
    [CTRL_PLAYER_NAME]           = encodePlayerNameBody,
    [CTRL_LOBBY_SLOT]            = encodeLobbySlotBody,
    [CTRL_LOBBY_SETTINGS]        = encodeLobbySettingsBody,
    [CTRL_LOBBY_MAP_CHANGE]      = encodeLobbyMapChangeBody,
    /* CTRL_MAP_DOWNLOAD_COMPLETE intentionally absent (NULL). */
    [CTRL_BALANCE_PROPOSAL]      = encodeBalanceProposalBody,
    [CTRL_MAP_SKIP_STATE]        = encodeMapSkipStateBody,
    [CTRL_GAME_PHASE_LOBBY]      = encodeGamePhaseLobbyBody,
    [CTRL_GAME_PHASE_COUNTDOWN]  = encodeGamePhaseCountdownBody,
    [CTRL_GAME_PHASE_RUNNING]    = encodeGamePhaseRunningBody,
    [CTRL_GAME_PHASE_GAME_OVER]  = encodeGamePhaseGameOverBody,
    [CTRL_GAME_OVER]             = encodeGameOverBody,
    [CTRL_SERVER_SHUTDOWN]       = encodeServerShutdownBody,
    [CTRL_LOBBY_SYNC_COMPLETE]   = encodeLobbySyncCompleteBody,
    [CTRL_CHAT]                  = encodeChatBody,
    [CTRL_PLAYER_LEAVE]          = encodePlayerLeaveBody,
    [CTRL_LOBBY_TEAM_META]       = encodeLobbyTeamMetaBody,
    [CTRL_LOBBY_BOT_CONFIG]      = encodeLobbyBotConfigBody,
    [CTRL_LOBBY_BOT_BRAIN]       = encodeLobbyBotBrainBody,
    [CTRL_LOBBY_BRAIN_LIST]      = encodeLobbyBrainListBody,
    [CTRL_LOBBY_BOT_POOL_CHUNK]  = encodeLobbyBotPoolChunkBody,
    [CTRL_GAME_VOTE_STATE]       = encodeGameVoteStateBody,
    [CTRL_SERVER_TEXT]           = encodeServerTextBody,
    [CTRL_COMMAND_REJECTED]      = encodeCommandRejectedBody,
    [CTRL_ALLIANCE_RESET]        = encodeAllianceResetBody,
    [CTRL_BALANCE_FAILED]        = encodeBalanceFailedBody,
    [CTRL_SHELL_DEATH]           = encodeShellDeathBody,
    [CTRL_CHANNEL_RESET]         = encodeChannelResetBody,
    [CTRL_SPECTATOR_SLOT]        = encodeSpectatorSlotBody,
    [CTRL_ROUND_STATS]           = encodeRoundStatsBody,
    [CTRL_SPECTATOR_CHAT]        = encodeSpectatorChatBody,
    [CTRL_ROUND_RATING_POSTED]   = encodeRoundRatingPostedBody,
    [CTRL_VIEW_TARGET]           = encodeViewTargetBody,
    [CTRL_STATS_SEED]            = encodeStatsSeedBody,
    [CTRL_VOICE_TALKING]         = encodeVoiceTalkingBody,
    [CTRL_ENTITY_CHANGE]         = encodeEntityChangeBody,
    [CTRL_ENTITY_SYNC]           = encodeEntitySyncBody,
};

static const ControlDecodeBodyFn s_bodyDecoders[CTRL_EVENT_TYPE_COUNT] = {
    [CTRL_ALLIANCE_REQUEST]      = decodeAllianceRequestBody,
    [CTRL_ALLIANCE_ACCEPT]       = decodeAllianceAcceptBody,
    [CTRL_ALLIANCE_LEAVE]        = decodeAllianceLeaveBody,
    [CTRL_PLAYER_JOIN]           = decodePlayerJoinBody,
    [CTRL_PLAYER_NAME]           = decodePlayerNameBody,
    [CTRL_LOBBY_SLOT]            = decodeLobbySlotBody,
    [CTRL_LOBBY_SETTINGS]        = decodeLobbySettingsBody,
    [CTRL_LOBBY_MAP_CHANGE]      = decodeLobbyMapChangeBody,
    /* CTRL_MAP_DOWNLOAD_COMPLETE intentionally absent (NULL). */
    [CTRL_BALANCE_PROPOSAL]      = decodeBalanceProposalBody,
    [CTRL_MAP_SKIP_STATE]        = decodeMapSkipStateBody,
    [CTRL_GAME_PHASE_LOBBY]      = decodeGamePhaseLobbyBody,
    [CTRL_GAME_PHASE_COUNTDOWN]  = decodeGamePhaseCountdownBody,
    [CTRL_GAME_PHASE_RUNNING]    = decodeGamePhaseRunningBody,
    [CTRL_GAME_PHASE_GAME_OVER]  = decodeGamePhaseGameOverBody,
    [CTRL_GAME_OVER]             = decodeGameOverBody,
    [CTRL_SERVER_SHUTDOWN]       = decodeServerShutdownBody,
    [CTRL_LOBBY_SYNC_COMPLETE]   = decodeLobbySyncCompleteBody,
    [CTRL_CHAT]                  = decodeChatBody,
    [CTRL_PLAYER_LEAVE]          = decodePlayerLeaveBody,
    [CTRL_LOBBY_TEAM_META]       = decodeLobbyTeamMetaBody,
    [CTRL_LOBBY_BOT_CONFIG]      = decodeLobbyBotConfigBody,
    [CTRL_LOBBY_BOT_BRAIN]       = decodeLobbyBotBrainBody,
    [CTRL_LOBBY_BRAIN_LIST]      = decodeLobbyBrainListBody,
    [CTRL_LOBBY_BOT_POOL_CHUNK]  = decodeLobbyBotPoolChunkBody,
    [CTRL_GAME_VOTE_STATE]       = decodeGameVoteStateBody,
    [CTRL_SERVER_TEXT]           = decodeServerTextBody,
    [CTRL_COMMAND_REJECTED]      = decodeCommandRejectedBody,
    [CTRL_ALLIANCE_RESET]        = decodeAllianceResetBody,
    [CTRL_BALANCE_FAILED]        = decodeBalanceFailedBody,
    [CTRL_SHELL_DEATH]           = decodeShellDeathBody,
    [CTRL_CHANNEL_RESET]         = decodeChannelResetBody,
    [CTRL_SPECTATOR_SLOT]        = decodeSpectatorSlotBody,
    [CTRL_ROUND_STATS]           = decodeRoundStatsBody,
    [CTRL_SPECTATOR_CHAT]        = decodeSpectatorChatBody,
    [CTRL_ROUND_RATING_POSTED]   = decodeRoundRatingPostedBody,
    [CTRL_VIEW_TARGET]           = decodeViewTargetBody,
    [CTRL_STATS_SEED]            = decodeStatsSeedBody,
    [CTRL_VOICE_TALKING]         = decodeVoiceTalkingBody,
    [CTRL_ENTITY_CHANGE]         = decodeEntityChangeBody,
    [CTRL_ENTITY_SYNC]           = decodeEntitySyncBody,
};

ControlEncodeFn transportControlCodecEncoder(ControlEventType type) {
    if (type < 0 || type >= CTRL_EVENT_TYPE_COUNT) return NULL;
    return s_encoders[type];
}

ControlDecodeFn transportControlCodecDecoder(uint16_t packetType) {
    switch (packetType) {
        case PACKET_ALLIANCE_UPDATE:  return decodeAllianceUpdate;
        case PACKET_PLAYER_JOINED:    return decodePlayerJoinBody;
        case PACKET_NAME_CHANGE:      return decodePlayerNameBody;
        case PACKET_LOBBY_UPDATE:     return decodeLobbySlotBody;
        case PACKET_LOBBY_SETTINGS:   return decodeLobbySettingsBody;
        case PACKET_LOBBY_MAP_CHANGE: return decodeLobbyMapChangeBody;
        case PACKET_BALANCE_PROPOSAL: return decodeBalanceProposalBody;
        case PACKET_MAP_SKIP_STATE:   return decodeMapSkipStateBody;
        case PACKET_COUNTDOWN:        return decodeGamePhaseCountdownBody;
        case PACKET_GAME_START:       return decodeGamePhaseRunningBody;
        case PACKET_GAME_OVER:        return decodeGameOverBody;
        case PACKET_SERVER_SHUTDOWN:  return decodeServerShutdownBody;
        case PACKET_LOBBY_SYNC_COMPLETE: return decodeLobbySyncCompleteBody;
        case PACKET_CHAT_BROADCAST:   return decodeChatBody;
        case PACKET_PLAYER_LEFT:      return decodePlayerLeaveBody;
        case PACKET_LOBBY_TEAM_META_CHG:  return decodeLobbyTeamMetaBody;
        case PACKET_LOBBY_BOT_CONFIG_CHG: return decodeLobbyBotConfigBody;
        case PACKET_LOBBY_BOT_BRAIN_CHG:  return decodeLobbyBotBrainBody;
        case PACKET_LOBBY_BRAIN_LIST:     return decodeLobbyBrainListBody;
        case PACKET_LOBBY_BOT_POOL_CHUNK: return decodeLobbyBotPoolChunkBody;
        case PACKET_GAME_VOTE_STATE:      return decodeGameVoteStateBody;
        case PACKET_COMMAND_REJECTED:     return decodeCommandRejectedBody;
        case PACKET_BALANCE_FAILED:       return decodeBalanceFailedBody;
        case PACKET_SHELL_DEATH:          return decodeShellDeathBody;
        case PACKET_ROUND_STATS:          return decodeRoundStatsBody;
        default:                      return NULL;
    }
}

ControlEncodeBodyFn transportControlCodecBodyEncoder(ControlEventType type) {
    if (type < 0 || type >= CTRL_EVENT_TYPE_COUNT) return NULL;
    return s_bodyEncoders[type];
}

ControlDecodeBodyFn transportControlCodecBodyDecoder(ControlEventType type) {
    if (type < 0 || type >= CTRL_EVENT_TYPE_COUNT) return NULL;
    return s_bodyDecoders[type];
}
