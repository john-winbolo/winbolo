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
    case PACKET_LOBBY_STATE:        return "LOBBY_STATE";
    case PACKET_LOBBY_UPDATE:       return "LOBBY_UPDATE";
    case PACKET_COUNTDOWN:          return "COUNTDOWN";
    case PACKET_GAME_START:         return "GAME_START";
    case PACKET_GAME_OVER:          return "GAME_OVER";
    case PACKET_LOBBY_MAP_CHANGE:   return "LOBBY_MAP_CHANGE";
    case PACKET_WBN_REAUTH:        return "WBN_REAUTH";
    case PACKET_BALANCE_REQUEST:   return "BALANCE_REQUEST";
    case PACKET_BALANCE_PROPOSAL:  return "BALANCE_PROPOSAL";
    case PACKET_BALANCE_APPLY:     return "BALANCE_APPLY";
    case PACKET_BALANCE_DISMISS:   return "BALANCE_DISMISS";
    case PACKET_MAP_SKIP_VOTE:     return "MAP_SKIP_VOTE";
    case PACKET_MAP_SKIP_STATE:    return "MAP_SKIP_STATE";
    default:                        return "UNKNOWN";
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
    packU32(buf + 11, pkt->eventAck);
    packU32(buf + 15, pkt->mapEventAck);
    packU16(buf + 19, pkt->pingMs);
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
    pkt->eventAck = unpackU32(buf + 11);
    pkt->mapEventAck = unpackU32(buf + 15);
    pkt->pingMs = unpackU16(buf + 19);
}

/* Serialize one TankSnapshot into buf. Returns bytes written (27). */
int packTankSnapshot(uint8_t *buf, const TankSnapshot *ts) {
    buf[0] = ts->playerNum;
    packU16(buf + 1, ts->worldX);
    packU16(buf + 3, ts->worldY);
    packU16(buf + 5, ts->angle);
    packU16(buf + 7, ts->speed);
    buf[9] = ts->tankStatus;
    buf[10] = ts->lgmFrame;
    buf[11] = ts->lgmMX;
    buf[12] = ts->lgmMY;
    buf[13] = ts->lgmPX;
    buf[14] = ts->lgmPY;
    buf[15] = ts->armour;
    buf[16] = ts->shells;
    buf[17] = ts->mines;
    buf[18] = ts->trees;
    buf[19] = ts->firstLeft;
    buf[20] = ts->firstRight;
    buf[21] = ts->gunsightLen;
    buf[22] = ts->deathWait;
    buf[23] = ts->reload;
    packU16(buf + 24, ts->pingMs);
    buf[26] = ts->accountFlags;
    return 27;
}

void unpackTankSnapshot(const uint8_t *buf, TankSnapshot *ts) {
    ts->playerNum = buf[0];
    ts->worldX = unpackU16(buf + 1);
    ts->worldY = unpackU16(buf + 3);
    ts->angle = unpackU16(buf + 5);
    ts->speed = unpackU16(buf + 7);
    ts->tankStatus = buf[9];
    ts->lgmFrame = buf[10];
    ts->lgmMX = buf[11];
    ts->lgmMY = buf[12];
    ts->lgmPX = buf[13];
    ts->lgmPY = buf[14];
    ts->armour = buf[15];
    ts->shells = buf[16];
    ts->mines = buf[17];
    ts->trees = buf[18];
    ts->firstLeft = buf[19];
    ts->firstRight = buf[20];
    ts->gunsightLen = buf[21];
    ts->deathWait = buf[22];
    ts->reload = buf[23];
    ts->pingMs = unpackU16(buf + 24);
    ts->accountFlags = buf[26];
}

/* Serialize one ShellSnapshot into buf. Returns bytes written (7). */
int packShellSnapshot(uint8_t *buf, const ShellSnapshot *ss) {
    packU16(buf, ss->worldX);
    packU16(buf + 2, ss->worldY);
    buf[4] = ss->angle;
    buf[5] = ss->owner;
    buf[6] = ss->length;
    return SHELL_SNAPSHOT_WIRE_SIZE;
}

void unpackShellSnapshot(const uint8_t *buf, ShellSnapshot *ss) {
    ss->worldX = unpackU16(buf);
    ss->worldY = unpackU16(buf + 2);
    ss->angle = buf[4];
    ss->owner = buf[5];
    ss->length = buf[6];
}

/* Serialize one TkExplosionSnapshot into buf. Returns bytes written (8). */
int packTkExplosionSnapshot(uint8_t *buf, const TkExplosionSnapshot *tke) {
    packU16(buf, tke->worldX);
    packU16(buf + 2, tke->worldY);
    buf[4] = tke->angle;
    buf[5] = tke->length;
    buf[6] = tke->explodeType;
    buf[7] = tke->creator;
    return TK_EXPLOSION_SNAPSHOT_WIRE_SIZE;
}

void unpackTkExplosionSnapshot(const uint8_t *buf, TkExplosionSnapshot *tke) {
    tke->worldX = unpackU16(buf);
    tke->worldY = unpackU16(buf + 2);
    tke->angle = buf[4];
    tke->length = buf[5];
    tke->explodeType = buf[6];
    tke->creator = buf[7];
}

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

int packBaseSnapshot(uint8_t *buf, const BaseSnapshot *bs) {
    buf[0] = bs->owner;
    buf[1] = bs->armour;
    buf[2] = bs->shells;
    buf[3] = bs->mines;
    return BASE_SNAPSHOT_WIRE_SIZE;
}

void unpackBaseSnapshot(const uint8_t *buf, BaseSnapshot *bs) {
    bs->owner = buf[0];
    bs->armour = buf[1];
    bs->shells = buf[2];
    bs->mines = buf[3];
}

int packPillSnapshot(uint8_t *buf, const PillSnapshot *ps) {
    buf[0] = ps->x;
    buf[1] = ps->y;
    buf[2] = ps->owner;
    buf[3] = ps->armour;
    buf[4] = ps->speed;
    buf[5] = ps->inTank;
    return PILL_SNAPSHOT_WIRE_SIZE;
}

void unpackPillSnapshot(const uint8_t *buf, PillSnapshot *ps) {
    ps->x = buf[0];
    ps->y = buf[1];
    ps->owner = buf[2];
    ps->armour = buf[3];
    ps->speed = buf[4];
    ps->inTank = buf[5];
}

/* ================================================================
 * Socket helpers
 * ================================================================ */

/* Create a non-blocking UDP socket */
SOCKET createUdpSocket(void) {
    SOCKET sock;
    unsigned long nonBlock = 1;
    int reuse = 1;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    /* Allow rebinding immediately after a previous process exits without
     * a clean close — the OS may not have reaped the port descriptor yet
     * (most visible on Windows after a force-quit). */
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT,
               (const char *)&reuse, sizeof(reuse));
#endif
    ioctlsocket(sock, FIONBIO, &nonBlock);
    return sock;
}

/* Send a buffer via UDP to a specific address */
void udpSendTo(SOCKET sock, const uint8_t *buf, int len,
               const struct sockaddr_in *addr) {
    uint8_t pktType = getPacketType(buf, len);
    if (pktType != PACKET_STATE_SNAPSHOT && pktType != PACKET_PONG && pktType != PACKET_INPUT) {
        fprintf(stderr, "[UDP SEND] %s (%u) len=%d to %s:%u\n",
                packetTypeName(pktType), pktType, len,
                inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
    }
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
