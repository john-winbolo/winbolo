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
 *Name:          Transport UDP Common
 *Filename:      transport_udp_common.c
 *Author:        John Morrison
 *Purpose:
 *  Shared serialization (pack/unpack) and socket helpers
 *  used by both client and server UDP transport code.
 *********************************************************/

#include "transport_udp_internal.h"
#include "wire_codec.h"
#include "wire_messages.h"
#include "../common/wb_log.h"

/* ================================================================
 * Serialization helpers — pack/unpack structs to/from wire format
 * All multi-byte values use network byte order (big-endian).
 * ================================================================ */

void packU16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
}

uint16_t unpackU16(const uint8_t *buf) {
    return (uint16_t)((buf[0] << 8) | buf[1]);
}

void packU32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val & 0xFF);
}

uint32_t unpackU32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |
           (uint32_t)buf[3];
}

void packHeader(uint8_t *buf, uint8_t packetType, uint32_t sequence) {
    buf[0] = BOLO_NEW_MAGIC_0;
    buf[1] = BOLO_NEW_MAGIC_1;
    buf[2] = packetType;
    buf[3] = 0; /* reserved */
    packU32(buf + 4, sequence);
}

uint8_t getPacketType(const uint8_t *buf, int len) {
    if (len < PACKET_HEADER_SIZE) return 0;
    if (buf[0] != BOLO_NEW_MAGIC_0 || buf[1] != BOLO_NEW_MAGIC_1) return 0;
    return buf[2];
}

/* Packet-type -> debug name. The one declarative list every type/name
 * mapping derives from; the switch below expands it, and the unit test
 * re-expands it to pin every string. Types absent here resolve to
 * "UNKNOWN" (a logging helper only — not a wire field). */
#define PACKET_NAME_TABLE(X) \
    X(PACKET_INPUT, "INPUT") \
    X(PACKET_JOIN_REQUEST, "JOIN_REQUEST") \
    X(PACKET_CHAT_MESSAGE, "CHAT_MESSAGE") \
    X(PACKET_PING, "PING") \
    X(PACKET_QUIT, "QUIT") \
    X(PACKET_STATE_SNAPSHOT, "STATE_SNAPSHOT") \
    X(PACKET_JOIN_ACCEPT, "JOIN_ACCEPT") \
    X(PACKET_JOIN_REJECT, "JOIN_REJECT") \
    X(PACKET_PLAYER_JOINED, "PLAYER_JOINED") \
    X(PACKET_PLAYER_LEFT, "PLAYER_LEFT") \
    X(PACKET_CHAT_BROADCAST, "CHAT_BROADCAST") \
    X(PACKET_FULL_STATE, "FULL_STATE") \
    X(PACKET_MAP_DELTA, "MAP_DELTA") \
    X(PACKET_BASE_STATE, "BASE_STATE") \
    X(PACKET_PILL_STATE, "PILL_STATE") \
    X(PACKET_PONG, "PONG") \
    X(PACKET_GAME_EVENT, "GAME_EVENT") \
    X(PACKET_MAP_RESYNC_REQUEST, "MAP_RESYNC_REQUEST") \
    X(PACKET_NAME_CHANGE, "NAME_CHANGE") \
    X(PACKET_ALLIANCE_REQUEST, "ALLIANCE_REQUEST") \
    X(PACKET_ALLIANCE_ACCEPT, "ALLIANCE_ACCEPT") \
    X(PACKET_ALLIANCE_LEAVE, "ALLIANCE_LEAVE") \
    X(PACKET_ALLIANCE_UPDATE, "ALLIANCE_UPDATE") \
    X(PACKET_LOCK_TOGGLE, "LOCK_TOGGLE") \
    X(PACKET_SERVER_SHUTDOWN, "SERVER_SHUTDOWN") \
    X(PACKET_LOBBY_TEAM_SET, "LOBBY_TEAM_SET") \
    X(PACKET_LOBBY_READY, "LOBBY_READY") \
    X(PACKET_LOBBY_ADD_BOT, "LOBBY_ADD_BOT") \
    X(PACKET_LOBBY_REMOVE_BOT, "LOBBY_REMOVE_BOT") \
    X(PACKET_LOBBY_UPDATE, "LOBBY_UPDATE") \
    X(PACKET_LOBBY_SETTINGS, "LOBBY_SETTINGS") \
    X(PACKET_LOBBY_SET_SETTING, "LOBBY_SET_SETTING") \
    X(PACKET_LOBBY_SETTING_CHG, "LOBBY_SETTING_CHG") \
    X(PACKET_LOBBY_OPEN_HOST, "LOBBY_OPEN_HOST") \
    X(PACKET_LOBBY_OPEN_HOST_CHG, "LOBBY_OPEN_HOST_CHG") \
    X(PACKET_LOBBY_TEAM_META, "LOBBY_TEAM_META") \
    X(PACKET_LOBBY_TEAM_META_CHG, "LOBBY_TEAM_META_CHG") \
    X(PACKET_LOBBY_TEAM_CLEAR, "LOBBY_TEAM_CLEAR") \
    X(PACKET_LOBBY_BOT_CONFIG, "LOBBY_BOT_CONFIG") \
    X(PACKET_LOBBY_BOT_CONFIG_CHG, "LOBBY_BOT_CONFIG_CHG") \
    X(PACKET_LOBBY_KICK, "LOBBY_KICK") \
    X(PACKET_KICKED, "KICKED") \
    X(PACKET_LOBBY_SET_BOT_BRAIN, "LOBBY_SET_BOT_BRAIN") \
    X(PACKET_LOBBY_BOT_BRAIN_CHG, "LOBBY_BOT_BRAIN_CHG") \
    X(PACKET_LOBBY_BRAIN_LIST, "LOBBY_BRAIN_LIST") \
    X(PACKET_LOBBY_SET_MAP, "LOBBY_SET_MAP") \
    X(PACKET_LOBBY_SET_SCENARIO, "LOBBY_SET_SCENARIO") \
    X(PACKET_SET_SCRIPT_LIST, "SET_SCRIPT_LIST") \
    X(PACKET_SET_SCRIPT_SETTING, "SET_SCRIPT_SETTING") \
    X(PACKET_LOBBY_SET_PASSWORD, "LOBBY_SET_PASSWORD") \
    X(PACKET_LOBBY_MAP_LIST_REQ, "LOBBY_MAP_LIST_REQ") \
    X(PACKET_LOBBY_MAP_LIST_RSP, "LOBBY_MAP_LIST_RSP") \
    X(PACKET_LOBBY_MAP_SEARCH_REQ, "LOBBY_MAP_SEARCH_REQ") \
    X(PACKET_LOBBY_MAP_SEARCH_RSP, "LOBBY_MAP_SEARCH_RSP") \
    X(PACKET_LOBBY_MAP_UPLOAD_BEGIN, "LOBBY_MAP_UPLOAD_BEGIN") \
    X(PACKET_LOBBY_MAP_UPLOAD_ACK, "LOBBY_MAP_UPLOAD_ACK") \
    X(PACKET_LOBBY_MAP_UPLOAD_DONE, "LOBBY_MAP_UPLOAD_DONE") \
    X(PACKET_LOBBY_MAP_USE_LOCAL, "LOBBY_MAP_USE_LOCAL") \
    X(PACKET_LOBBY_MAP_USE_LOCAL_NACK, "LOBBY_MAP_USE_LOCAL_NACK") \
    X(PACKET_LOBBY_MAP_PREVIEW_REQ, "LOBBY_MAP_PREVIEW_REQ") \
    X(PACKET_LOBBY_MAP_PREVIEW_ERR, "LOBBY_MAP_PREVIEW_ERR") \
    X(PACKET_LOBBY_PREVIEW_CANCEL, "LOBBY_PREVIEW_CANCEL") \
    X(PACKET_LOBBY_RELOAD_SCENARIO, "LOBBY_RELOAD_SCENARIO") \
    X(PACKET_LOBBY_SCENARIO_LIST_REQ, "LOBBY_SCENARIO_LIST_REQ") \
    X(PACKET_LOBBY_SCENARIO_LIST_RSP, "LOBBY_SCENARIO_LIST_RSP") \
    X(PACKET_LOBBY_SCENARIO_DETAILS_REQ, "LOBBY_SCENARIO_DETAILS_REQ") \
    X(PACKET_LOBBY_BRAIN_DOCS_REQ, "LOBBY_BRAIN_DOCS_REQ") \
    X(PACKET_LOBBY_SCRIPT_FETCH_REQ, "LOBBY_SCRIPT_FETCH_REQ") \
    X(PACKET_LOBBY_PREVIEW_COMMIT, "LOBBY_PREVIEW_COMMIT") \
    X(PACKET_LOBBY_PREVIEW_RANDOM, "LOBBY_PREVIEW_RANDOM") \
    X(PACKET_LOBBY_CLAIM_START, "LOBBY_CLAIM_START") \
    X(PACKET_COUNTDOWN, "COUNTDOWN") \
    X(PACKET_GAME_START, "GAME_START") \
    X(PACKET_GAME_OVER, "GAME_OVER") \
    X(PACKET_LOBBY_MAP_CHANGE, "LOBBY_MAP_CHANGE") \
    X(PACKET_WBN_REAUTH, "WBN_REAUTH") \
    X(PACKET_WBN_REKEY, "WBN_REKEY") \
    X(PACKET_BALANCE_REQUEST, "BALANCE_REQUEST") \
    X(PACKET_BALANCE_PROPOSAL, "BALANCE_PROPOSAL") \
    X(PACKET_BALANCE_APPLY, "BALANCE_APPLY") \
    X(PACKET_BALANCE_DISMISS, "BALANCE_DISMISS") \
    X(PACKET_MAP_SKIP_VOTE, "MAP_SKIP_VOTE") \
    X(PACKET_MAP_SKIP_STATE, "MAP_SKIP_STATE") \
    X(PACKET_GAME_VOTE_TOGGLE, "GAME_VOTE_TOGGLE") \
    X(PACKET_GAME_VOTE_STATE, "GAME_VOTE_STATE") \
    X(PACKET_PUNCH_REQUEST, "PUNCH_REQUEST") \
    X(PACKET_PUNCH_NOTIFY, "PUNCH_NOTIFY") \
    X(PACKET_PUNCH_REQUEST_ACK, "PUNCH_REQUEST_ACK") \
    X(PACKET_PUNCH_PROBE_REQUEST, "PUNCH_PROBE_REQUEST") \
    X(PACKET_PUNCH_PROBE_REPLY, "PUNCH_PROBE_REPLY") \
    X(PACKET_COMMAND_TICK, "COMMAND_TICK") \
    X(PACKET_COMMAND_ACK, "COMMAND_ACK") \
    X(PACKET_COMMAND_REJECTED, "COMMAND_REJECTED") \
    X(PACKET_BALANCE_FAILED, "BALANCE_FAILED") \
    X(PACKET_MAP_DL_READY, "MAP_DL_READY") \
    X(PACKET_PLAYER_MUTE, "PLAYER_MUTE") \
    X(PACKET_VOICE_STATE, "VOICE_STATE") \
    X(PACKET_MAP_PING, "MAP_PING") \
    X(PACKET_PLAYER_PING_MUTE, "PLAYER_PING_MUTE")

const char *packetTypeName(uint8_t type) {
    switch (type) {
#define X(sym, str) case sym: return str;
    PACKET_NAME_TABLE(X)
#undef X
    default: return "UNKNOWN";
    }
}

/* Serialize one InputPacket into buf. Returns bytes written (21). */
int packInputPacket(uint8_t *buf, const InputPacket *pkt) {
    packU32(buf, pkt->tick);
    buf[4] = pkt->playerNum;
    buf[5] = pkt->buttons;
    buf[6] = pkt->actions;
    buf[7] = pkt->buildAction;
    buf[8] = pkt->buildX;
    buf[9] = pkt->buildY;
    buf[10] = pkt->flags;
    packU32(buf + 11, pkt->mapEventAck);
    packU16(buf + 15, pkt->pingMs);
    packU32(buf + 17, pkt->viewTick);
    return 21;
}

void unpackInputPacket(const uint8_t *buf, InputPacket *pkt) {
    pkt->tick = unpackU32(buf);
    pkt->playerNum = buf[4];
    pkt->buttons = buf[5];
    pkt->actions = buf[6];
    pkt->buildAction = buf[7];
    pkt->buildX = buf[8];
    pkt->buildY = buf[9];
    pkt->flags = buf[10];
    pkt->mapEventAck = unpackU32(buf + 11);
    pkt->pingMs = unpackU16(buf + 15);
    pkt->viewTick = unpackU32(buf + 17);
}

/* Variable-length tank entry: a 1-byte stub when playerNum carries
 * TANK_SNAPSHOT_HIDDEN_FLAG, otherwise an 11-byte core plus the present groups.
 * Generated from TANK_SNAPSHOT_FIELDS in wire_messages.h. */
DEFINE_WIRE_CODEC_MASKED(TankSnapshot, "tank_snapshot", playerNum,
                         TANK_SNAPSHOT_HIDDEN_FLAG, TANK_SNAPSHOT_FIELDS)

/* Flat fixed-layout leaf snapshot codecs, generated from the field lists in
 * wire_messages.h. pack returns bytes written; unpack returns bytes consumed
 * or 0 if `avail` is too short for a field. */
DEFINE_WIRE_CODEC(ShellSnapshot,       "shell_snapshot",        SHELL_SNAPSHOT_FIELDS)
DEFINE_WIRE_CODEC(TkExplosionSnapshot, "tk_explosion_snapshot", TK_EXPLOSION_SNAPSHOT_FIELDS)
DEFINE_WIRE_CODEC(BaseSnapshot,        "base_snapshot",         BASE_SNAPSHOT_FIELDS)
DEFINE_WIRE_CODEC(PillSnapshot,        "pill_snapshot",         PILL_SNAPSHOT_FIELDS)

/* Tie the generated wire size to the hand-maintained *_WIRE_SIZE constants that
 * the server's pre-pack buffer guards still use, so a field added to a list
 * without bumping the constant fails the build rather than under-counting. */
BOLO_STATIC_ASSERT(WIRE_SIZE_OF(SHELL_SNAPSHOT_FIELDS)        == SHELL_SNAPSHOT_WIRE_SIZE,        shell_wire_size_drift);
BOLO_STATIC_ASSERT(WIRE_SIZE_OF(TK_EXPLOSION_SNAPSHOT_FIELDS) == TK_EXPLOSION_SNAPSHOT_WIRE_SIZE, tk_explosion_wire_size_drift);
BOLO_STATIC_ASSERT(WIRE_SIZE_OF(BASE_SNAPSHOT_FIELDS)         == BASE_SNAPSHOT_WIRE_SIZE,         base_wire_size_drift);
BOLO_STATIC_ASSERT(WIRE_SIZE_OF(PILL_SNAPSHOT_FIELDS)         == PILL_SNAPSHOT_WIRE_SIZE,         pill_wire_size_drift);
BOLO_STATIC_ASSERT(WIRE_MASKED_SIZE_OF(TANK_SNAPSHOT_FIELDS)  == TANK_SNAPSHOT_WIRE_SIZE,         tank_wire_size_drift);

/* Serialize one GameEvent into buf. Returns bytes written (1 + dataSize). */
int packGameEvent(uint8_t *buf, const GameEvent *ev) {
    int dataLen = gameEventDataSize(ev->type);
    buf[0] = ev->type;
    memcpy(buf + 1, ev->data, dataLen);
    return 1 + dataLen;
}

/* Deserialize one GameEvent from buf, reading at most `avail` bytes. Returns
 * bytes consumed (1 + dataSize), or 0 if the buffer is too short for the type
 * byte or its data — mirroring the generated unpack* codecs so a truncated
 * trailing event can't over-read the datagram. */
int unpackGameEvent(const uint8_t *buf, size_t avail, GameEvent *ev) {
    int dataLen;
    if (avail < 1) return 0;
    ev->type = buf[0];
    dataLen = gameEventDataSize(ev->type);
    if (avail < (size_t)(1 + dataLen)) return 0;
    memset(ev->data, 0, sizeof(ev->data));
    memcpy(ev->data, buf + 1, dataLen);
    return 1 + dataLen;
}

/* Splice channel-delivered game events into `events` ahead of the map-tail
 * events already staged at [tailStart, tailStart+tailCount).  The `chanCount`
 * events in `chan` are placed at tailStart and the existing tail is shifted up
 * behind them, preserving the game-then-map order the snapshot game tail used.
 * Counts are clamped to `cap` (placing channel events first, truncating the
 * tail if room runs out) so the splice can never index past `events[cap]`.
 * Returns the new total event count. */
int spliceGameEventsBeforeTail(GameEvent *events, int tailStart, int tailCount,
                               const GameEvent *chan, int chanCount, int cap) {
    int room;
    if (chanCount <= 0) {
        return tailStart + tailCount;
    }
    if (chanCount > cap - tailStart) {
        chanCount = cap - tailStart;
    }
    if (chanCount < 0) {
        chanCount = 0;
    }
    room = cap - (tailStart + chanCount);
    if (tailCount > room) {
        tailCount = room;
    }
    if (tailCount < 0) {
        tailCount = 0;
    }
    memmove(&events[tailStart + chanCount], &events[tailStart],
            (size_t)tailCount * sizeof(GameEvent));
    memcpy(&events[tailStart], chan, (size_t)chanCount * sizeof(GameEvent));
    return tailStart + chanCount + tailCount;
}

/* ================================================================
 * Socket helpers
 * ================================================================ */

/* Create a non-blocking UDP socket.
 *
 * exclusive=true: caller wants a hard failure on bind() if the port is
 *   already in use (server case — a second listen-server host on the
 *   same machine must NOT silently share the port with the first). On
 *   Windows that means SO_EXCLUSIVEADDRUSE and skipping SO_REUSEADDR;
 *   SO_REUSEADDR there is permissive enough that two binds to the same
 *   port both succeed and the OS dispatches packets to one of them.
 * exclusive=false: ephemeral / client socket — no options. The kernel
 *   picks the port on the first send, so there is no fixed port to
 *   rebind. SO_REUSEADDR and SO_REUSEPORT used to be set here, and they
 *   let the kernel give the client a port number another process's
 *   server already held on 127.0.0.1. Datagrams to 127.0.0.1:port then
 *   went to that server, the more specific bind, and the client never
 *   heard its own server's replies. */
SOCKET createUdpSocket(bool exclusive) {
    SOCKET sock;
    unsigned long nonBlock = 1;
#ifdef _WIN32
    int reuse = 1;
#endif

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
#ifdef _WIN32
    if (exclusive) {
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&reuse, sizeof(reuse));
    }
#else
    (void)exclusive;
#endif
    ioctlsocket(sock, FIONBIO, &nonBlock);
    return sock;
}

/* Send a buffer via UDP to a specific address */
void udpSendTo(SOCKET sock, const uint8_t *buf, int len,
               const struct sockaddr_in *addr) {
    sendto(sock, (const char *)buf, len, 0,
           (const struct sockaddr *)addr, sizeof(*addr));
}

/* Receive a UDP datagram. Returns bytes received, or -1 if none available. */
int udpRecvFrom(SOCKET sock, uint8_t *buf, int maxLen,
                struct sockaddr_in *fromAddr) {
    socklen_t addrLen = sizeof(*fromAddr);
    int ret;

    ret = recvfrom(sock, (char *)buf, maxLen, 0,
                   (struct sockaddr *)fromAddr, &addrLen);
    if (ret == SOCKET_ERROR) {
        return -1;
    }
    return ret;
}
