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
 *Name:          Transport UDP Internal
 *Filename:      transport_udp_internal.h
 *Author:        John Morrison
 *Purpose:
 *  Shared types, constants, and helper declarations used
 *  by transport_udp_common.c, transport_udp_client.c,
 *  and transport_udp_server.c.
 *
 *  Not part of the public API — include transport_udp.h
 *  for the external interface.
 *********************************************************/

#ifndef TRANSPORT_UDP_INTERNAL_H
#define TRANSPORT_UDP_INTERNAL_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <SDL3/SDL.h>
#include "global.h"
#include "platform_net.h"
#include "transport.h"
#include "transport_udp.h"
#include "netpacks.h"
#include "input_packet.h"
#include "gametype.h"

/* Maximum UDP datagram payload we'll send.
 * Sits under the standard 1500-byte Ethernet MTU minus IPv4 (20) + UDP (8)
 * headers (= 1472 cap), with headroom for IPv4 options or minor encapsulation
 * overhead. Avoids fragmentation on typical internet paths. */
#define UDP_MAX_PAYLOAD 1400

/* Input packet ring buffer size for client redundancy */
#define CLIENT_INPUT_RING_SIZE 16

/* Reliable event queue for per-client retransmission */
#define RELIABLE_EVENT_BUFFER_SIZE 2048

typedef struct {
    GameEvent event;
    uint32_t  seq;
} ReliableEvent;

typedef struct {
    ReliableEvent buffer[RELIABLE_EVENT_BUFFER_SIZE];
    uint32_t nextSeq;    /* next seq to assign */
    uint32_t ackedSeq;   /* client confirmed up to here (exclusive) */
} ClientEventQueue;

/* Check if a reliable event queue has space. Returns false if the ring
 * buffer would wrap and overwrite unacked entries. */
static inline bool eventQueueHasSpace(const ClientEventQueue *q) {
    return (q->nextSeq - q->ackedSeq) < RELIABLE_EVENT_BUFFER_SIZE;
}

/* Join retry interval in ticks (1 second) */
#define JOIN_RETRY_INTERVAL 50

/* Max join attempts before giving up */
#define JOIN_MAX_RETRIES 10

#define PACKET_HEADER_SIZE 8

/* Lobby slot wire format (variable length).
 *   Disconnected slot: connected(0) — 1 byte total.
 *   Connected slot:    connected(1) + nameLen(1) + name(0..63 UTF-8 bytes,
 *                      no NUL on the wire) + teamNumber(1) + ready(1)
 *                      + isBot(1) + pingMs(2) + countryCode(2)
 *                      + clientType(1) + clientFlags(1)
 *                      ->  11 + nameLen bytes (max 74).
 * The macros below are buffer-size upper bounds, NOT the actual on-wire size.
 * Encoders track running `pos` and emit only the bytes they actually wrote;
 * decoders length-check each field and reject malformed packets. */
#define LOBBY_SLOT_WIRE_SIZE (1 + 1 + PACKET_MAX_PLAYER_NAME + 1 + 1 + 1 + 2 + 2 + 1 + 1)

/* Lobby settings tail: mapName(36) + gameType(1) + hiddenMines(1) + aiType(1) + gameLength(4) + pillCount(1) + baseCount(1) + startCount(1) + mapSkipAvailable(1) */
#define LOBBY_SETTINGS_SIZE  (MAP_STR_SIZE + 1 + 1 + 1 + 4 + 1 + 1 + 1 + 1)
/* Upper bound for buffer sizing: serverState(1) + 16 max-size slots + settings tail. */
#define LOBBY_STATE_PAYLOAD  (1 + MAX_TANKS * LOBBY_SLOT_WIRE_SIZE + LOBBY_SETTINGS_SIZE)

#define INPUT_PACKET_WIRE_SIZE 21
#define TANK_SNAPSHOT_WIRE_SIZE 27
/* Upper bound: slot index(1) + max-size slot. */
#define LOBBY_UPDATE_PAYLOAD (1 + LOBBY_SLOT_WIRE_SIZE)

/* ---- Serialization helpers ---- */

void packU16(uint8_t *buf, uint16_t val);
uint16_t unpackU16(const uint8_t *buf);
void packU32(uint8_t *buf, uint32_t val);
uint32_t unpackU32(const uint8_t *buf);
void packHeader(uint8_t *buf, uint8_t packetType, uint32_t sequence);
uint8_t getPacketType(const uint8_t *buf, int len);
const char *packetTypeName(uint8_t type);

int packInputPacket(uint8_t *buf, const InputPacket *pkt);
void unpackInputPacket(const uint8_t *buf, InputPacket *pkt);
int packTankSnapshot(uint8_t *buf, const TankSnapshot *ts);
void unpackTankSnapshot(const uint8_t *buf, TankSnapshot *ts);
int packShellSnapshot(uint8_t *buf, const ShellSnapshot *ss);
void unpackShellSnapshot(const uint8_t *buf, ShellSnapshot *ss);
int packTkExplosionSnapshot(uint8_t *buf, const TkExplosionSnapshot *tke);
void unpackTkExplosionSnapshot(const uint8_t *buf, TkExplosionSnapshot *tke);
int packGameEvent(uint8_t *buf, const GameEvent *ev);
int unpackGameEvent(const uint8_t *buf, GameEvent *ev);
int packBaseSnapshot(uint8_t *buf, const BaseSnapshot *bs);
void unpackBaseSnapshot(const uint8_t *buf, BaseSnapshot *bs);
int packPillSnapshot(uint8_t *buf, const PillSnapshot *ps);
void unpackPillSnapshot(const uint8_t *buf, PillSnapshot *ps);

/* ---- Socket helpers ---- */

SOCKET createUdpSocket(void);
void udpSendTo(SOCKET sock, const uint8_t *buf, int len,
               const struct sockaddr_in *addr);
int udpRecvFrom(SOCKET sock, uint8_t *buf, int maxLen,
                struct sockaddr_in *fromAddr);

#endif /* TRANSPORT_UDP_INTERNAL_H */
