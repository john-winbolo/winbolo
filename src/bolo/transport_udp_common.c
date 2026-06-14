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

const char *packetTypeName(uint8_t type) {
    switch (type) {
    case PACKET_INPUT:          return "INPUT";
    case PACKET_JOIN_REQUEST:   return "JOIN_REQUEST";
    case PACKET_CHAT_MESSAGE:   return "CHAT_MESSAGE";
    case PACKET_PING:           return "PING";
    case PACKET_MAP_ACK:        return "MAP_ACK";
    case PACKET_QUIT:           return "QUIT";
    case PACKET_STATE_SNAPSHOT: return "STATE_SNAPSHOT";
    case PACKET_JOIN_ACCEPT:    return "JOIN_ACCEPT";
    case PACKET_JOIN_REJECT:    return "JOIN_REJECT";
    case PACKET_PLAYER_JOINED:  return "PLAYER_JOINED";
    case PACKET_PLAYER_LEFT:    return "PLAYER_LEFT";
    case PACKET_CHAT_BROADCAST: return "CHAT_BROADCAST";
    case PACKET_FULL_STATE:     return "FULL_STATE";
    case PACKET_MAP_DELTA:      return "MAP_DELTA";
    case PACKET_BASE_STATE:     return "BASE_STATE";
    case PACKET_PILL_STATE:     return "PILL_STATE";
    case PACKET_PONG:           return "PONG";
    case PACKET_GAME_EVENT:     return "GAME_EVENT";
    case PACKET_MAP_DOWNLOAD:   return "MAP_DOWNLOAD";
    case PACKET_MAP_RESYNC_REQUEST: return "MAP_RESYNC_REQUEST";
    case PACKET_PLAYER_LIST:    return "PLAYER_LIST";
    case PACKET_NAME_CHANGE:        return "NAME_CHANGE";
    case PACKET_ALLIANCE_REQUEST:   return "ALLIANCE_REQUEST";
    case PACKET_ALLIANCE_ACCEPT:    return "ALLIANCE_ACCEPT";
    case PACKET_ALLIANCE_LEAVE:     return "ALLIANCE_LEAVE";
    case PACKET_ALLIANCE_UPDATE:    return "ALLIANCE_UPDATE";
    case PACKET_LOCK_TOGGLE:        return "LOCK_TOGGLE";
    case PACKET_SERVER_SHUTDOWN:    return "SERVER_SHUTDOWN";
    case PACKET_LOBBY_TEAM_SET:     return "LOBBY_TEAM_SET";
    case PACKET_LOBBY_READY:        return "LOBBY_READY";
    case PACKET_LOBBY_ADD_BOT:      return "LOBBY_ADD_BOT";
    case PACKET_LOBBY_REMOVE_BOT:   return "LOBBY_REMOVE_BOT";
    case PACKET_LOBBY_UPDATE:       return "LOBBY_UPDATE";
    case PACKET_LOBBY_SETTINGS:     return "LOBBY_SETTINGS";
    case PACKET_LOBBY_SET_SETTING:  return "LOBBY_SET_SETTING";
    case PACKET_LOBBY_SETTING_CHG:  return "LOBBY_SETTING_CHG";
    case PACKET_LOBBY_OPEN_HOST:    return "LOBBY_OPEN_HOST";
    case PACKET_LOBBY_OPEN_HOST_CHG: return "LOBBY_OPEN_HOST_CHG";
    case PACKET_LOBBY_TEAM_META:    return "LOBBY_TEAM_META";
    case PACKET_LOBBY_TEAM_META_CHG: return "LOBBY_TEAM_META_CHG";
    case PACKET_LOBBY_TEAM_CLEAR:   return "LOBBY_TEAM_CLEAR";
    case PACKET_LOBBY_BOT_CONFIG:   return "LOBBY_BOT_CONFIG";
    case PACKET_LOBBY_BOT_CONFIG_CHG: return "LOBBY_BOT_CONFIG_CHG";
    case PACKET_LOBBY_KICK:         return "LOBBY_KICK";
    case PACKET_KICKED:             return "KICKED";
    case PACKET_LOBBY_SET_BOT_BRAIN: return "LOBBY_SET_BOT_BRAIN";
    case PACKET_LOBBY_BOT_BRAIN_CHG: return "LOBBY_BOT_BRAIN_CHG";
    case PACKET_LOBBY_BRAIN_LIST:   return "LOBBY_BRAIN_LIST";
    case PACKET_LOBBY_SET_MAP:      return "LOBBY_SET_MAP";
    case PACKET_LOBBY_SET_PASSWORD: return "LOBBY_SET_PASSWORD";
    case PACKET_LOBBY_MAP_LIST_REQ: return "LOBBY_MAP_LIST_REQ";
    case PACKET_LOBBY_MAP_LIST_RSP: return "LOBBY_MAP_LIST_RSP";
    case PACKET_LOBBY_MAP_SEARCH_REQ: return "LOBBY_MAP_SEARCH_REQ";
    case PACKET_LOBBY_MAP_SEARCH_RSP: return "LOBBY_MAP_SEARCH_RSP";
    case PACKET_LOBBY_MAP_UPLOAD_BEGIN: return "LOBBY_MAP_UPLOAD_BEGIN";
    case PACKET_LOBBY_MAP_UPLOAD_CHUNK: return "LOBBY_MAP_UPLOAD_CHUNK";
    case PACKET_LOBBY_MAP_UPLOAD_ACK:   return "LOBBY_MAP_UPLOAD_ACK";
    case PACKET_LOBBY_MAP_UPLOAD_DONE:  return "LOBBY_MAP_UPLOAD_DONE";
    case PACKET_LOBBY_MAP_USE_LOCAL:     return "LOBBY_MAP_USE_LOCAL";
    case PACKET_LOBBY_MAP_USE_LOCAL_NACK: return "LOBBY_MAP_USE_LOCAL_NACK";
    case PACKET_LOBBY_MAP_PREVIEW_REQ:   return "LOBBY_MAP_PREVIEW_REQ";
    case PACKET_LOBBY_MAP_PREVIEW_BEGIN: return "LOBBY_MAP_PREVIEW_BEGIN";
    case PACKET_LOBBY_MAP_PREVIEW_CHUNK: return "LOBBY_MAP_PREVIEW_CHUNK";
    case PACKET_LOBBY_MAP_PREVIEW_ERR:   return "LOBBY_MAP_PREVIEW_ERR";
    case PACKET_LOBBY_PREVIEW_CANCEL: return "LOBBY_PREVIEW_CANCEL";
    case PACKET_LOBBY_PREVIEW_COMMIT: return "LOBBY_PREVIEW_COMMIT";
    case PACKET_LOBBY_PREVIEW_RANDOM: return "LOBBY_PREVIEW_RANDOM";
    case PACKET_COUNTDOWN:          return "COUNTDOWN";
    case PACKET_GAME_START:         return "GAME_START";
    case PACKET_GAME_OVER:          return "GAME_OVER";
    case PACKET_LOBBY_MAP_CHANGE:   return "LOBBY_MAP_CHANGE";
    case PACKET_WBN_REAUTH:        return "WBN_REAUTH";
    case PACKET_WBN_REKEY:         return "WBN_REKEY";
    case PACKET_BALANCE_REQUEST:   return "BALANCE_REQUEST";
    case PACKET_BALANCE_PROPOSAL:  return "BALANCE_PROPOSAL";
    case PACKET_BALANCE_APPLY:     return "BALANCE_APPLY";
    case PACKET_BALANCE_DISMISS:   return "BALANCE_DISMISS";
    case PACKET_MAP_SKIP_VOTE:     return "MAP_SKIP_VOTE";
    case PACKET_MAP_SKIP_STATE:    return "MAP_SKIP_STATE";
    case PACKET_GAME_VOTE_TOGGLE:  return "GAME_VOTE_TOGGLE";
    case PACKET_GAME_VOTE_STATE:   return "GAME_VOTE_STATE";
    case PACKET_PUNCH_REQUEST:       return "PUNCH_REQUEST";
    case PACKET_PUNCH_NOTIFY:        return "PUNCH_NOTIFY";
    case PACKET_PUNCH_REQUEST_ACK:   return "PUNCH_REQUEST_ACK";
    case PACKET_PUNCH_PROBE_REQUEST: return "PUNCH_PROBE_REQUEST";
    case PACKET_PUNCH_PROBE_REPLY:   return "PUNCH_PROBE_REPLY";
    case PACKET_CONTROL_TICK:      return "CONTROL_TICK";
    case PACKET_CONTROL_ACK:       return "CONTROL_ACK";
    case PACKET_COMMAND_TICK:      return "COMMAND_TICK";
    case PACKET_COMMAND_ACK:       return "COMMAND_ACK";
    case PACKET_COMMAND_REJECTED:  return "COMMAND_REJECTED";
    case PACKET_BALANCE_FAILED:    return "BALANCE_FAILED";
    default:                        return "UNKNOWN";
    }
}

/* Serialize one InputPacket into buf. Returns bytes written (29). */
int packInputPacket(uint8_t *buf, const InputPacket *pkt) {
    packU32(buf, pkt->tick);
    buf[4] = pkt->playerNum;
    buf[5] = pkt->buttons;
    buf[6] = pkt->actions;
    buf[7] = pkt->buildAction;
    buf[8] = pkt->buildX;
    buf[9] = pkt->buildY;
    buf[10] = pkt->flags;
    packU32(buf + 11, pkt->eventAck);
    packU32(buf + 15, pkt->mapEventAck);
    packU32(buf + 19, pkt->controlEventAck);
    packU16(buf + 23, pkt->pingMs);
    packU32(buf + 25, pkt->viewTick);
    return 29;
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
    pkt->eventAck = unpackU32(buf + 11);
    pkt->mapEventAck = unpackU32(buf + 15);
    pkt->controlEventAck = unpackU32(buf + 19);
    pkt->pingMs = unpackU16(buf + 23);
    pkt->viewTick = unpackU32(buf + 25);
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

/* Fixed map-transfer chunk headers. Each precedes a hand-written data payload
 * and chunk loop (reassembly and length validation stay hand-rolled). */
DEFINE_WIRE_CODEC(MapDownloadChunkHeader, "map_download_chunk_hdr", MAP_DOWNLOAD_CHUNK_HEADER_FIELDS)
DEFINE_WIRE_CODEC(MapUploadChunkHeader,   "map_upload_chunk_hdr",   MAP_UPLOAD_CHUNK_HEADER_FIELDS)
DEFINE_WIRE_CODEC(MapPreviewChunkHeader,  "map_preview_chunk_hdr",  MAP_PREVIEW_CHUNK_HEADER_FIELDS)

/* Serialize one GameEvent into buf. Returns bytes written (1 + dataSize). */
int packGameEvent(uint8_t *buf, const GameEvent *ev) {
    int dataLen = gameEventDataSize(ev->type);
    buf[0] = ev->type;
    memcpy(buf + 1, ev->data, dataLen);
    return 1 + dataLen;
}

/* Deserialize one GameEvent from buf. Returns bytes consumed (1 + dataSize). */
int unpackGameEvent(const uint8_t *buf, GameEvent *ev) {
    int dataLen;
    ev->type = buf[0];
    dataLen = gameEventDataSize(ev->type);
    memset(ev->data, 0, sizeof(ev->data));
    memcpy(ev->data, buf + 1, dataLen);
    return 1 + dataLen;
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
 * exclusive=false: ephemeral / client socket — set SO_REUSEADDR so a
 *   rebind after an unclean exit doesn't fail with EADDRINUSE. */
SOCKET createUdpSocket(bool exclusive) {
    SOCKET sock;
    unsigned long nonBlock = 1;
    int reuse = 1;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    if (exclusive) {
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&reuse, sizeof(reuse));
#endif
    } else {
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT,
                   (const char *)&reuse, sizeof(reuse));
#endif
    }
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
