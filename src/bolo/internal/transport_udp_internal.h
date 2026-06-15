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
#include "control_event.h"

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

/* Per-client reliable control event queues.  Smaller than the game /
 * map queues because control events are produced at lower rates, but
 * sized large enough that a sync-replay burst (~15 events) plus any
 * concurrent publishes cannot overflow under normal operation —
 * silently dropping an overflow event would resurrect exactly the
 * lobby-desync bug class this plan exists to fix. */
#define CONTROL_EVENT_QUEUE_SIZE 128

typedef struct {
    uint32_t      seq;
    ControlEvent  event;
} ReliableControlEvent;

typedef struct {
    ReliableControlEvent buffer[CONTROL_EVENT_QUEUE_SIZE];
    uint32_t             nextSeq;
    uint32_t             ackedSeq;
} ClientControlEventQueue;

static inline bool controlEventQueueHasSpace(const ClientControlEventQueue *q) {
    return (q->nextSeq - q->ackedSeq) < CONTROL_EVENT_QUEUE_SIZE;
}

/* Queue maintenance contract — the invariants that must hold at every
 * observable point.  Call from every site that mutates ackedSeq, nextSeq,
 * or buffer (enqueue, ack-advance, reset/wipe, slot-init).  In release
 * builds this expands to nothing.
 *
 * History: every queue corruption we've shipped to date violated one of
 * these.  The stale-ack-after-wipe bug pushed ackedSeq past nextSeq; the
 * empty-queue-idle-timer bug doesn't violate these invariants but exposed
 * how absent the maintenance-contract documentation was.  Wire-checking
 * here surfaces the next sibling at first occurrence rather than waiting
 * for the symptom. */
static inline void controlEventQueueAssertValid(const ClientControlEventQueue *q,
                                                const char *site) {
    (void)site;
    SDL_assert(q->ackedSeq <= q->nextSeq);
    SDL_assert((q->nextSeq - q->ackedSeq) <= CONTROL_EVENT_QUEUE_SIZE);
}

/* Decide whether the coalesced standalone PACKET_CONTROL_ACK should be
 * (re)sent this tick.  pendingTick is the localTick at which the most
 * recent PACKET_CONTROL_TICK armed the ack (0 = nothing pending); the
 * emission resets it to 0 after sending, so each subsequent TICK re-arms
 * it.  The ack is sent — including re-sending an already-sent value —
 * whenever a TICK is pending and either:
 *   - overdue: ~3 ticks (~60ms at 50 Hz) have elapsed since arming,
 *              coalescing a burst of retransmitted TICKs into one ack; or
 *   - eager:   a single TICK delivered 2+ new events (ack jumped past
 *              lastSent+1), to free server queue slots promptly.
 * The value-advance gate is deliberately absent: a re-send of an
 * unchanged ack value is exactly what recovers a dropped lobby ack while
 * the server keeps retransmitting already-acked events.  Self-terminating:
 * pendingTick is only re-armed by an incoming TICK, and the server stops
 * sending TICKs the moment its ackedSeq reaches nextSeq, so a caught-up
 * server yields zero re-sends — no per-tick ack storm in steady state. */
static inline bool controlAckResendDue(uint32_t pendingTick,
                                       uint32_t localTick,
                                       uint32_t controlEventAck,
                                       uint32_t lastSentControlAck) {
    if (pendingTick == 0) {
        return false;
    }
    bool overdue = (localTick - pendingTick) >= 3;
    bool eager = (controlEventAck > lastSentControlAck + 1);
    return overdue || eager;
}

/* Decide whether an incoming control-event tail is the server's
 * game-start sequence-space restart that the client must adopt.  The
 * server wipes the per-client control queue to (ackedSeq=1, nextSeq=1)
 * in transportUdpServerOnGameStart and publishes CTRL_GAME_PHASE_RUNNING
 * at seq 1, so the running flip lands at the bottom of a new sequence
 * space while the client's controlEventAck is still high from the lobby
 * phase.  When this returns true the caller snaps controlEventAck down to
 * baseSeq so seq-1 RUNNING isn't dedup'd away.
 *
 * Deliberately independent of inLobby: under loss/reorder inLobby can
 * flip false before the seq-1 RUNNING snapshot is processed.  runningSeqAdopted
 * makes the snap fire exactly once per wipe (set true on snap, cleared on
 * the reverse game-over/lobby transition) and stay loop-free on retransmits:
 * after adoption the flag blocks a re-snap and seq-1 RUNNING dedups normally
 * (baseSeq < the now-advanced ack but adopted == true → no re-fire). */
static inline bool controlSeqResetDetected(uint32_t controlEventCount,
                                           uint32_t controlEventBaseSeq,
                                           uint32_t controlEventAck,
                                           bool runningSeqAdopted,
                                           bool firstEventIsRunning) {
    return controlEventCount > 0 &&
           !runningSeqAdopted &&
           firstEventIsRunning &&
           controlEventBaseSeq < controlEventAck;
}

/* Join retry interval in ticks (1 second) */
#define JOIN_RETRY_INTERVAL 50

/* Max join attempts before giving up */
#define JOIN_MAX_RETRIES 10

/* PACKET_HEADER_SIZE lives in netpacks.h next to the rest of the
 * wire constants — clients that need to build a wire packet from
 * outside the UDP transport (e.g. client_net.c building chat) can
 * see it without dragging this header's SDL + platform_net
 * dependencies. */

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

#define INPUT_PACKET_WIRE_SIZE 29
/* Upper bound on one non-stub tank entry: the 11-byte core (incl. presence
 * mask) plus every field group present at once. NOT the typical on-wire size —
 * most entries are far smaller because absent (zero) groups are omitted. */
#define TANK_SNAPSHOT_WIRE_SIZE 28

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

/* True when cur's sampled controls differ from prev's — the edge that
 * promotes a recorded input to an immediate send. */
bool udpInputEdgeChanged(const InputPacket *prev, const InputPacket *cur);
int packTankSnapshot(uint8_t *buf, const TankSnapshot *ts);
int unpackTankSnapshot(const uint8_t *buf, size_t avail, TankSnapshot *ts);
int packShellSnapshot(uint8_t *buf, const ShellSnapshot *ss);
int unpackShellSnapshot(const uint8_t *buf, size_t avail, ShellSnapshot *ss);
int packTkExplosionSnapshot(uint8_t *buf, const TkExplosionSnapshot *tke);
int unpackTkExplosionSnapshot(const uint8_t *buf, size_t avail,
                              TkExplosionSnapshot *tke);
int packGameEvent(uint8_t *buf, const GameEvent *ev);
int unpackGameEvent(const uint8_t *buf, size_t avail, GameEvent *ev);
int packBaseSnapshot(uint8_t *buf, const BaseSnapshot *bs);
int unpackBaseSnapshot(const uint8_t *buf, size_t avail, BaseSnapshot *bs);
int packPillSnapshot(uint8_t *buf, const PillSnapshot *ps);
int unpackPillSnapshot(const uint8_t *buf, size_t avail, PillSnapshot *ps);

/* Fixed-layout headers that precede the hand-written data/loop of each
 * map-transfer stream. Only the header is generated; the chunk loop,
 * reassembly, and length validation around each stay hand-written. */
typedef struct {
    uint32_t resyncGen;
    uint32_t mapSize;
    uint16_t chunkIdx;
    uint16_t chunkSize;
} MapDownloadChunkHeader;
typedef struct {
    uint32_t offset;
    uint16_t dataLen;
} MapUploadChunkHeader;
typedef struct {
    uint8_t  seq;
    uint32_t offset;
    uint16_t len;
} MapPreviewChunkHeader;

int packMapDownloadChunkHeader(uint8_t *buf, const MapDownloadChunkHeader *h);
int unpackMapDownloadChunkHeader(const uint8_t *buf, size_t avail,
                                 MapDownloadChunkHeader *h);
int packMapUploadChunkHeader(uint8_t *buf, const MapUploadChunkHeader *h);
int unpackMapUploadChunkHeader(const uint8_t *buf, size_t avail,
                               MapUploadChunkHeader *h);
int packMapPreviewChunkHeader(uint8_t *buf, const MapPreviewChunkHeader *h);
int unpackMapPreviewChunkHeader(const uint8_t *buf, size_t avail,
                                MapPreviewChunkHeader *h);

/* ---- Socket helpers ---- */

SOCKET createUdpSocket(bool exclusive);
void udpSendTo(SOCKET sock, const uint8_t *buf, int len,
               const struct sockaddr_in *addr);
int udpRecvFrom(SOCKET sock, uint8_t *buf, int maxLen,
                struct sockaddr_in *fromAddr);

#endif /* TRANSPORT_UDP_INTERNAL_H */
