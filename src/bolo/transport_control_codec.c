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
#include "player_flags.h"  /* CLIENT_TYPE_COUNT / CLIENT_TYPE_UNKNOWN */
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

/* PACKET_ALLIANCE_UPDATE shares one wire shape across the three
 * sub-events: [header 8][event 1][fromPlayer 1][toPlayer 1].  For
 * LEAVE the third byte is unused on the wire and on the decode side. */
#define ALLIANCE_UPDATE_PAYLOAD 3

static EncodeResult encodeAllianceRequest(const ControlEvent *evt,
                                          const struct UdpServerClient *recipient,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + ALLIANCE_UPDATE_PAYLOAD;
    /* Single-target filtering lives in the per-client deliver
     * callback — the codec stays agnostic to UdpServerClient internals
     * (forward-declared here on purpose). */
    (void)recipient;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_ALLIANCE_UPDATE, 0);
    buf[PACKET_HEADER_SIZE]     = ALLIANCE_EVENT_REQUEST;
    buf[PACKET_HEADER_SIZE + 1] = evt->u.allianceRequest.fromPlayer;
    buf[PACKET_HEADER_SIZE + 2] = evt->u.allianceRequest.toPlayer;
    *outLen = needed;
    return ENCODE_OK;
}

static EncodeResult encodeAllianceAccept(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + ALLIANCE_UPDATE_PAYLOAD;
    (void)recipient;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_ALLIANCE_UPDATE, 0);
    buf[PACKET_HEADER_SIZE]     = ALLIANCE_EVENT_ACCEPT;
    buf[PACKET_HEADER_SIZE + 1] = evt->u.allianceAccept.acceptedBy;
    buf[PACKET_HEADER_SIZE + 2] = evt->u.allianceAccept.newMember;
    *outLen = needed;
    return ENCODE_OK;
}

static EncodeResult encodeAllianceLeave(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + ALLIANCE_UPDATE_PAYLOAD;
    (void)recipient;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_ALLIANCE_UPDATE, 0);
    buf[PACKET_HEADER_SIZE]     = ALLIANCE_EVENT_LEAVE;
    buf[PACKET_HEADER_SIZE + 1] = evt->u.allianceLeave.playerNum;
    buf[PACKET_HEADER_SIZE + 2] = 0; /* unused for leave */
    *outLen = needed;
    return ENCODE_OK;
}

/* PACKET_PLAYER_JOINED wire format:
 *   [header 8] [pNum 1] [name PACKET_MAX_PLAYER_NAME] [cc 2]
 *   [clientType 1] [clientFlags 1] [numAllies 1] [ally 1 × numAllies]
 * The trailing numAllies/allies pair is an additive change from the
 * pre-codec wire format — receiving clients now have the join's full
 * alliance bitmap on the wire instead of waiting for PACKET_PLAYER_LIST. */
static EncodeResult encodePlayerJoin(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    (void)recipient;
    BYTE numAllies = evt->u.playerJoin.numAllies;
    if (numAllies > MAX_TANKS) numAllies = MAX_TANKS;
    const size_t needed = PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME
                          + 2 + 1 + 1 + 1 + numAllies;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_PLAYER_JOINED, 0);
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

/* PACKET_PLAYER_LEFT wire format:
 *   [header 8] [playerNum 1] [name PACKET_MAX_PLAYER_NAME (NUL-padded)]
 *   [cc 2] */
static EncodeResult encodePlayerLeave(const ControlEvent *evt,
                                      const struct UdpServerClient *recipient,
                                      uint8_t *buf, size_t bufCap,
                                      size_t *outLen) {
    (void)recipient;
    const size_t needed = PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 2;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_PLAYER_LEFT, 0);
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

/* PACKET_NAME_CHANGE wire format:
 *   [header 8] [playerNum 1] [newName PACKET_MAX_PLAYER_NAME (NUL-padded)] */
static EncodeResult encodePlayerName(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    (void)recipient;
    const size_t needed = PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_NAME_CHANGE, 0);
    buf[PACKET_HEADER_SIZE] = evt->u.playerName.playerNum;
    memset(buf + PACKET_HEADER_SIZE + 1, 0, PACKET_MAX_PLAYER_NAME);
    {
        size_t nameLen = strnlen(evt->u.playerName.name, PACKET_MAX_PLAYER_NAME - 1);
        if (nameLen > 0) {
            memcpy(buf + PACKET_HEADER_SIZE + 1, evt->u.playerName.name, nameLen);
        }
    }
    *outLen = needed;
    return ENCODE_OK;
}

/* PACKET_LOBBY_UPDATE wire format (variable length):
 *   [header 8] [playerNum 1] [connected 1]
 *   If connected:
 *     [nameLen 1] [name nameLen bytes] [teamNumber 1] [ready 1]
 *     [isBot 1] [pingMs 2 BE] [cc 2] [clientType 1] [clientFlags 1] */
static EncodeResult encodeLobbySlot(const ControlEvent *evt,
                                    const struct UdpServerClient *recipient,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    (void)recipient;
    const ClientLobbySlot *slot = &evt->u.lobbySlot.slot;
    size_t nameLen = 0;
    if (slot->connected) {
        nameLen = strnlen(slot->playerName, PACKET_MAX_PLAYER_NAME - 1);
    }
    const size_t needed = PACKET_HEADER_SIZE + 1 + 1
                          + (slot->connected ? (1 + nameLen + 1 + 1 + 1 + 2 + 2 + 1 + 1) : 0);
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_LOBBY_UPDATE, 0);
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
    }
    *outLen = pos;
    return ENCODE_OK;
}

/* PACKET_LOBBY_SETTINGS wire format:
 *   [header 8] [mapName MAP_STR_SIZE] [gameType 1] [hiddenMines 1]
 *   [aiType 1] [gameLength 4 BE] [pillCount 1] [baseCount 1]
 *   [startCount 1] [mapSkipAvailable 1] [netStat 1] [inLobby 1]
 *   [openHost 1] [autoLockOnGameStart 1] [serverLocks 2 BE]
 *   [ranked 1] [allowNewPlayers 1] [wbnAvailable 1] [uploadPolicy 1]
 *
 * The trailing four bytes are appended after the base layout so the
 * existing fields keep their offsets. The decoder reads each one
 * optionally and leaves zero-init defaults in place when the sender
 * omits them, which keeps old/new codec pairs interoperable.
 * serverLocks is packed big-endian to match every other multi-byte
 * field in the codec (packU16/packU32). */
#define LOBBY_SETTINGS_WIRE_PAYLOAD_BASE \
    (MAP_STR_SIZE + 1 + 1 + 1 + 4 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 2)
#define LOBBY_SETTINGS_WIRE_PAYLOAD (LOBBY_SETTINGS_WIRE_PAYLOAD_BASE + 4)

static EncodeResult encodeLobbySettings(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    (void)recipient;
    const size_t needed = PACKET_HEADER_SIZE + LOBBY_SETTINGS_WIRE_PAYLOAD;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_LOBBY_SETTINGS, 0);
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
    buf[pos++] = evt->u.lobbySettings.inLobby ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyOpenHost ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyAutoLockOnGameStart ? 1 : 0;
    packU16(buf + pos, evt->u.lobbySettings.lobbyServerLocks);
    pos += 2;
    buf[pos++] = evt->u.lobbySettings.lobbyRanked ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyAllowNewPlayers ? 1 : 0;
    buf[pos++] = evt->u.lobbySettings.lobbyWbnAvailable ? 1 : 0;
    buf[pos++] = (uint8_t)evt->u.lobbySettings.uploadPolicy;
    *outLen = pos;
    return ENCODE_OK;
}

/* PACKET_LOBBY_TEAM_META_CHG wire format (ported verbatim from the
 * branch's deleted transportUdpServerBroadcastLobbyTeamMetaChg):
 *   [header 8] [teamId 1] [color 1] [namingPool 1] [nameLen 1]
 *   [name nameLen]
 * The `in_use` flag is not on the wire — the decoder reconstructs it
 * from (nameLen > 0 || color != 0 || pool != 0), matching the existing
 * PACKET_LOBBY_TEAM_META_CHG decoder in transport_udp_client.c. */
static EncodeResult encodeLobbyTeamMeta(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    (void)recipient;
    if (evt->u.lobbyTeamMeta.teamId == 0 ||
        evt->u.lobbyTeamMeta.teamId >= MAX_TANKS) {
        return ENCODE_SKIP;
    }
    size_t nameLen = strnlen(evt->u.lobbyTeamMeta.name, LOBBY_TEAM_NAME_LEN - 1);
    const size_t needed = PACKET_HEADER_SIZE + 4 + nameLen;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_LOBBY_TEAM_META_CHG, 0);
    buf[pos++] = evt->u.lobbyTeamMeta.teamId;
    buf[pos++] = evt->u.lobbyTeamMeta.color;
    buf[pos++] = evt->u.lobbyTeamMeta.namingPool;
    buf[pos++] = (uint8_t)nameLen;
    if (nameLen > 0) {
        memcpy(buf + pos, evt->u.lobbyTeamMeta.name, nameLen);
        pos += nameLen;
    }
    *outLen = pos;
    return ENCODE_OK;
}

/* PACKET_LOBBY_BOT_CONFIG_CHG wire format (ported verbatim from
 * branch's transportUdpServerBroadcastLobbyBotConfigChg):
 *   [header 8] [slot 1] [difficulty 1] [personality 1] [nameLen 1]
 *   [name nameLen] */
static EncodeResult encodeLobbyBotConfig(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)recipient;
    if (evt->u.lobbyBotConfig.slot >= MAX_TANKS) return ENCODE_SKIP;
    size_t nameLen = strnlen(evt->u.lobbyBotConfig.name,
                             PACKET_MAX_PLAYER_NAME - 1);
    const size_t needed = PACKET_HEADER_SIZE + 4 + nameLen;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_LOBBY_BOT_CONFIG_CHG, 0);
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

/* PACKET_LOBBY_BOT_BRAIN_CHG wire format:
 *   [header 8] [slot 1] [brainIdx 1]
 * brainIdx == 0xFF signals "use the server's global bot brain";
 * any other value indexes into the server's brain catalogue. */
static EncodeResult encodeLobbyBotBrain(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    (void)recipient;
    if (evt->u.lobbyBotBrain.slot >= MAX_TANKS) return ENCODE_SKIP;
    const size_t needed = PACKET_HEADER_SIZE + 2;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_LOBBY_BOT_BRAIN_CHG, 0);
    buf[pos++] = evt->u.lobbyBotBrain.slot;
    buf[pos++] = evt->u.lobbyBotBrain.brainIdx;
    *outLen = pos;
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
static EncodeResult encodeLobbyBrainList(const ControlEvent *evt,
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
    size_t needed = PACKET_HEADER_SIZE + 1;
    for (int i = 0; i < count; i++) {
        const BrainListEntry *e = &list->entries[i];
        needed += 2
                + strnlen(e->name,    BRAIN_LIST_NAME_LEN - 1)
                + strnlen(e->version, BRAIN_LIST_VER_LEN  - 1);
    }
    if (bufCap < needed) return ENCODE_OVERFLOW;
    size_t pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_LOBBY_BRAIN_LIST, 0);
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

/* Compile-time guarantee that the brain-list worst case fits inside
 * MAX_CONTROL_PACKET — keeps the runtime ENCODE_OVERFLOW path above
 * unreachable as long as the codec header keeps its budget. */
BOLO_STATIC_ASSERT(
    PACKET_HEADER_SIZE + 1 +
        (size_t)BRAIN_LIST_MAX *
            (2 + (BRAIN_LIST_NAME_LEN - 1) + (BRAIN_LIST_VER_LEN - 1))
        <= MAX_CONTROL_PACKET,
    brain_list_worst_case_fits_MAX_CONTROL_PACKET);

/* PACKET_LOBBY_MAP_CHANGE wire format: header only (no payload).
 * The lobbyMapChange union member carries no fields — receipt of
 * the packet is itself the signal that the server has loaded a new
 * map and the client should reset and re-download. */
static EncodeResult encodeLobbyMapChange(const ControlEvent *evt,
                                         const struct UdpServerClient *recipient,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    (void)evt; (void)recipient;
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_LOBBY_MAP_CHANGE, 0);
    *outLen = PACKET_HEADER_SIZE;
    return ENCODE_OK;
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

/* encodeGamePhase produces PACKET_COUNTDOWN for the COUNTDOWN phase and
 * PACKET_GAME_START for RUNNING.  GAME_OVER is owned by encodeGameOver
 * (via CTRL_GAME_OVER → PACKET_GAME_OVER); the phase encoder returns
 * SKIP so the GAME_OVER transition isn't sent twice on the wire. */
static EncodeResult encodeGamePhase(const ControlEvent *evt,
                                    const struct UdpServerClient *recipient,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    (void)recipient;
    switch (evt->u.gamePhase.phase) {
        case CTRL_PHASE_COUNTDOWN: {
            const size_t needed = PACKET_HEADER_SIZE + 1;
            if (bufCap < needed) return ENCODE_OVERFLOW;
            packHeader(buf, PACKET_COUNTDOWN, 0);
            buf[PACKET_HEADER_SIZE] = (uint8_t)evt->u.gamePhase.countdownSeconds;
            *outLen = needed;
            return ENCODE_OK;
        }
        case CTRL_PHASE_RUNNING: {
            if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
            packHeader(buf, PACKET_GAME_START, 0);
            *outLen = PACKET_HEADER_SIZE;
            return ENCODE_OK;
        }
        case CTRL_PHASE_GAME_OVER:
        default:
            return ENCODE_SKIP;
    }
}

static EncodeResult encodeGameOver(const ControlEvent *evt,
                                   const struct UdpServerClient *recipient,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    (void)evt; (void)recipient;
    if (bufCap < PACKET_HEADER_SIZE) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_GAME_OVER, 0);
    *outLen = PACKET_HEADER_SIZE;
    return ENCODE_OK;
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

/* PACKET_CHAT_BROADCAST wire format (variable length, three subtypes
 * discriminated by fromPlayer):
 *   [header 8] [fromPlayer 1] [destPlayer 1] [body bodyLen]
 * body is opaque to the codec — raw text when fromPlayer < MAX_TANKS
 * or == 0xFE, packed langid+args when fromPlayer == 0xFF.  Per-
 * recipient filtering (broadcast-skip-sender, unicast-only-dest)
 * lives in udpClientDeliverControl, matching CTRL_ALLIANCE_REQUEST. */
static EncodeResult encodeChat(const ControlEvent *evt,
                               const struct UdpServerClient *recipient,
                               uint8_t *buf, size_t bufCap,
                               size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 2 + evt->u.chat.bodyLen;
    (void)recipient;
    if (evt->u.chat.bodyLen > CHAT_BODY_MAX) return ENCODE_OVERFLOW;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_CHAT_BROADCAST, 0);
    buf[PACKET_HEADER_SIZE]     = evt->u.chat.fromPlayer;
    buf[PACKET_HEADER_SIZE + 1] = evt->u.chat.destPlayer;
    if (evt->u.chat.bodyLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 2, evt->u.chat.body,
               evt->u.chat.bodyLen);
    }
    *outLen = needed;
    return ENCODE_OK;
}

/* CTRL_SERVER_TEXT → PACKET_CHAT_BROADCAST(fromPlayer=0xFE).  The wire
 * format is the same as a regular chat broadcast so existing UDP
 * clients keep their PACKET_CHAT_BROADCAST handler unchanged.
 * In-process subscribers consume CTRL_SERVER_TEXT directly. */
static EncodeResult encodeServerText(const ControlEvent *evt,
                                     const struct UdpServerClient *recipient,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    (void)recipient;
    size_t textLen = strnlen(evt->u.serverText.text,
                             sizeof(evt->u.serverText.text));
    if (textLen > CHAT_BODY_MAX) textLen = CHAT_BODY_MAX;
    const size_t needed = PACKET_HEADER_SIZE + 2 + textLen;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_CHAT_BROADCAST, 0);
    buf[PACKET_HEADER_SIZE]     = 0xFE; /* server raw English */
    buf[PACKET_HEADER_SIZE + 1] = 0xFF; /* broadcast */
    if (textLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 2, evt->u.serverText.text, textLen);
    }
    *outLen = needed;
    return ENCODE_OK;
}

/* Wire: [header 8] [kind 1] [active 1] [triggerSrc 1] [teamId 1]
 *   [threshold 1] [yes 1] [no 1] [eligible 1] [secsRemaining 1]
 *   [votes 2 BE] — 11-byte body. Same shape on every recipient. */
static EncodeResult encodeGameVoteState(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    (void)recipient;
    const size_t needed = PACKET_HEADER_SIZE + 11;
    if (bufCap < needed) return ENCODE_OVERFLOW;
    packHeader(buf, PACKET_GAME_VOTE_STATE, 0);
    buf[PACKET_HEADER_SIZE + 0] = evt->u.gameVoteState.kind;
    buf[PACKET_HEADER_SIZE + 1] = evt->u.gameVoteState.active;
    buf[PACKET_HEADER_SIZE + 2] = evt->u.gameVoteState.triggerSrc;
    buf[PACKET_HEADER_SIZE + 3] = evt->u.gameVoteState.teamId;
    buf[PACKET_HEADER_SIZE + 4] = evt->u.gameVoteState.threshold;
    buf[PACKET_HEADER_SIZE + 5] = evt->u.gameVoteState.yesCount;
    buf[PACKET_HEADER_SIZE + 6] = evt->u.gameVoteState.noCount;
    buf[PACKET_HEADER_SIZE + 7] = evt->u.gameVoteState.eligibleCount;
    buf[PACKET_HEADER_SIZE + 8] = evt->u.gameVoteState.secondsRemaining;
    packU16(buf + PACKET_HEADER_SIZE + 9, evt->u.gameVoteState.votes);
    *outLen = needed;
    return ENCODE_OK;
}

/* ================================================================
 * Decoders — keyed by wire packet type.
 *
 * Three thin decoders for the game-phase wire family
 * (PACKET_COUNTDOWN/GAME_START/GAME_OVER) keep the dispatcher's
 * lookup 1:1 with wire packets. decodeAllianceUpdate is the single
 * entry for PACKET_ALLIANCE_UPDATE; it inspects the sub-event byte
 * and produces the matching CTRL_ALLIANCE_REQUEST/ACCEPT/LEAVE
 * event.
 * ================================================================ */

static bool decodeAllianceUpdate(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    if (len < ALLIANCE_UPDATE_PAYLOAD) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    switch (buf[0]) {
        case ALLIANCE_EVENT_REQUEST:
            outEvt->type = CTRL_ALLIANCE_REQUEST;
            outEvt->u.allianceRequest.fromPlayer = buf[1];
            outEvt->u.allianceRequest.toPlayer   = buf[2];
            return true;
        case ALLIANCE_EVENT_ACCEPT:
            outEvt->type = CTRL_ALLIANCE_ACCEPT;
            outEvt->u.allianceAccept.acceptedBy = buf[1];
            outEvt->u.allianceAccept.newMember  = buf[2];
            return true;
        case ALLIANCE_EVENT_LEAVE:
            outEvt->type = CTRL_ALLIANCE_LEAVE;
            outEvt->u.allianceLeave.playerNum = buf[1];
            return true;
        default:
            return false;
    }
}

static bool decodePlayerJoin(const uint8_t *buf, size_t len,
                             ControlEvent *outEvt) {
    /* Layout matches encodePlayerJoin's wire format. */
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

static bool decodePlayerName(const uint8_t *buf, size_t len,
                             ControlEvent *outEvt) {
    if (len < (size_t)(1 + PACKET_MAX_PLAYER_NAME)) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_PLAYER_NAME;
    outEvt->u.playerName.playerNum = buf[0];
    memcpy(outEvt->u.playerName.name, buf + 1, PACKET_MAX_PLAYER_NAME);
    outEvt->u.playerName.name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    return true;
}

static bool decodePlayerLeave(const uint8_t *buf, size_t len,
                              ControlEvent *outEvt) {
    /* Layout matches encodePlayerLeave's wire format. */
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

static bool decodeLobbySlot(const uint8_t *buf, size_t len,
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
        if (pos + nameLen + 9 > len) return false;
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
    }
    return true;
}

static bool decodeLobbySettings(const uint8_t *buf, size_t len,
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
    outEvt->u.lobbySettings.inLobby          = buf[pos++] ? true : false;
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
    return true;
}

static bool decodeLobbyTeamMeta(const uint8_t *buf, size_t len,
                                ControlEvent *outEvt) {
    /* Layout matches encodeLobbyTeamMeta. */
    if (len < 4) return false;
    uint8_t teamId  = buf[0];
    uint8_t color   = buf[1];
    uint8_t pool    = buf[2];
    uint8_t nameLen = buf[3];
    if (teamId == 0 || teamId >= MAX_TANKS) return false;
    if (nameLen > LOBBY_TEAM_NAME_LEN - 1) return false;
    if (len < (size_t)(4 + nameLen)) return false;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_TEAM_META;
    outEvt->u.lobbyTeamMeta.teamId     = teamId;
    outEvt->u.lobbyTeamMeta.color      = color;
    outEvt->u.lobbyTeamMeta.namingPool = pool;
    if (nameLen > 0) {
        memcpy(outEvt->u.lobbyTeamMeta.name, buf + 4, nameLen);
    }
    outEvt->u.lobbyTeamMeta.name[nameLen] = '\0';
    /* in_use is not on the wire — reconstruct it the same way the
     * existing PACKET_LOBBY_TEAM_META_CHG decoder in
     * transport_udp_client.c does. */
    outEvt->u.lobbyTeamMeta.in_use =
        (nameLen > 0 || color != 0 || pool != 0) ? 1 : 0;
    return true;
}

static bool decodeLobbyBotConfig(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    /* Layout matches encodeLobbyBotConfig. */
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

static bool decodeLobbyBotBrain(const uint8_t *buf, size_t len,
                                ControlEvent *outEvt) {
    /* Layout matches encodeLobbyBotBrain: [slot 1][brainIdx 1]. */
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

static bool decodeLobbyBrainList(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    /* Layout matches encodeLobbyBrainList: [count 1] then per entry
     * [nameLen 1][name][verLen 1][version]. */
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

static bool decodeLobbyMapChange(const uint8_t *buf, size_t len,
                                 ControlEvent *outEvt) {
    (void)buf; (void)len;
    memset(outEvt, 0, sizeof(*outEvt));
    outEvt->type = CTRL_LOBBY_MAP_CHANGE;
    return true;
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

static bool decodeChat(const uint8_t *buf, size_t len,
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

static bool decodeGameVoteState(const uint8_t *buf, size_t len,
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
    [CTRL_CHAT]               = encodeChat,
    [CTRL_PLAYER_LEAVE]       = encodePlayerLeave,
    [CTRL_LOBBY_TEAM_META]    = encodeLobbyTeamMeta,
    [CTRL_LOBBY_BOT_CONFIG]   = encodeLobbyBotConfig,
    [CTRL_LOBBY_BOT_BRAIN]    = encodeLobbyBotBrain,
    [CTRL_LOBBY_BRAIN_LIST]   = encodeLobbyBrainList,
    [CTRL_GAME_VOTE_STATE]    = encodeGameVoteState,
    [CTRL_SERVER_TEXT]        = encodeServerText,
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
        case PACKET_LOBBY_SETTINGS:   return decodeLobbySettings;
        case PACKET_LOBBY_MAP_CHANGE: return decodeLobbyMapChange;
        case PACKET_BALANCE_PROPOSAL: return decodeBalanceProposal;
        case PACKET_MAP_SKIP_STATE:   return decodeMapSkipState;
        case PACKET_COUNTDOWN:        return decodeCountdown;
        case PACKET_GAME_START:       return decodeGameStart;
        case PACKET_GAME_OVER:        return decodeGameOver;
        case PACKET_SERVER_SHUTDOWN:  return decodeServerShutdown;
        case PACKET_CHAT_BROADCAST:   return decodeChat;
        case PACKET_PLAYER_LEFT:      return decodePlayerLeave;
        case PACKET_LOBBY_TEAM_META_CHG:  return decodeLobbyTeamMeta;
        case PACKET_LOBBY_BOT_CONFIG_CHG: return decodeLobbyBotConfig;
        case PACKET_LOBBY_BOT_BRAIN_CHG:  return decodeLobbyBotBrain;
        case PACKET_LOBBY_BRAIN_LIST:     return decodeLobbyBrainList;
        case PACKET_GAME_VOTE_STATE:      return decodeGameVoteState;
        default:                      return NULL;
    }
}
