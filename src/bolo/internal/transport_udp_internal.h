/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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

/* ---- Ping RTT smoothers (pure, transport-struct-independent) ----
 *
 * The PONG handler feeds each raw RTT sample to both smoothers. The
 * min-over-window value drives shell forward-projection and the server's
 * viewTick==0 lag-comp fallback (stamped into InputPacket) — taking the
 * floor of recent samples rejects transient upward spikes so projection
 * doesn't over-advance. The EWMA value drives the player-facing displays
 * (HUD own-ping, scoreboard) where a steady, lightly-smoothed number reads
 * better than a jumpy minimum. */

/* Min over the last PING_MIN_WINDOW_LEN samples. At the ~0.4s ping cadence
 * this is a ~3.2s floor window. */
#define PING_MIN_WINDOW_LEN 8

typedef struct {
    uint16_t samples[PING_MIN_WINDOW_LEN];
    uint8_t  count;   /* number of valid samples (saturates at LEN) */
    uint8_t  head;    /* index of the next slot to overwrite */
} PingMinWindow;

void     pingMinWindowReset(PingMinWindow *w);
/* Push a sample and return the minimum over the currently stored window. */
uint16_t pingMinWindowPush(PingMinWindow *w, uint16_t sample);

/* EWMA with alpha = 1/4, carried in Q8 fixed point so the running value
 * keeps sub-millisecond resolution instead of truncating toward the input
 * each step. */
typedef struct {
    uint32_t valueQ8;
    bool     init;
} PingEwma;

void     pingEwmaReset(PingEwma *e);
/* Fold one sample in (first sample seeds the value) and return the rounded
 * milliseconds estimate. */
uint16_t pingEwmaUpdate(PingEwma *e, uint16_t sample);

/* Join retry interval in ticks (1 second) */
#define JOIN_RETRY_INTERVAL 50

/* Max join attempts before giving up. The browser client reaches the server
 * over a reliable WebSocket (so a lost JOIN is rare) and its join codes are
 * single-use, so repeatedly re-sending a JOIN that the relay rejects only
 * re-opens the WebSocket and trips rate-limiting — cap it low there. A
 * JOIN_CHALLENGE resets joinAttempts, so the cookie handshake is unaffected. */
#ifdef __EMSCRIPTEN__
#define JOIN_MAX_RETRIES 2
#else
#define JOIN_MAX_RETRIES 10
#endif

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

#define INPUT_PACKET_WIRE_SIZE 21
/* Fixed on-wire size of the PACKET_STATE_SNAPSHOT header (server -> client):
 * serverTick(4) + lastProcessedInput(4) + tankCount(1) + shellCount(1)
 * + tkExplosionCount(1) + baseCount(1) + pillCount(1) + mapChecksum(2)
 * + returnToLobbyTicks(2). The server packer reserves and the client size
 * guard check this one constant so the two sides can't drift. */
#define SNAPSHOT_HEADER_WIRE_SIZE 17
/* Upper bound on one non-stub tank entry: the 12-byte core (incl. the two-byte
 * presence mask) plus every field group present at once. NOT the typical
 * on-wire size — most entries are far smaller because absent (zero) groups are
 * omitted. */
#define TANK_SNAPSHOT_WIRE_SIZE 37

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

/* ---- WS↔UDP relay metadata frame (browser client) ---- */

/* First byte of the relay's one-shot metadata frame. Real game packets
 * always begin with the 'W''B' magic (getPacketType), so a 0x01 first byte
 * unambiguously marks the frame. */
#define PROXY_META_FRAME_TYPE 0x01

/* Decoded 0x01 frame. The identity fields (name / wbn / country) exist for
 * diagnostics only — the server stamps web identity itself from the
 * join-code verify (udpServerApplyWebIdentity); only the prefs JSON is acted
 * on (handed to the wasm frontend). */
typedef struct {
    char name[PACKET_MAX_PLAYER_NAME]; /* WBN-resolved display name */
    char country[3];                   /* ISO-2 + NUL */
    bool wbn;                          /* is_logged_in flag */
    char prefs[1024];                  /* raw prefs JSON, NUL-terminated */
    int  prefsLen;
} ProxyMetaFrame;

/* Parse a 0x01 metadata frame into *out. Tolerant of a short/garbled frame:
 * parses as far as the length allows and leaves the unreached fields at
 * their zeroed defaults; an oversized name is truncated into out->name but
 * skipped at its full wire length so the fields after it stay framed. The
 * consume site is emscripten-only, but the parser compiles everywhere so it
 * stays unit-testable off-browser. */
void transportUdpParseProxyMeta(const uint8_t *buf, int len,
                                ProxyMetaFrame *out);

/* ---- Socket helpers ---- */

SOCKET createUdpSocket(bool exclusive);
void udpSendTo(SOCKET sock, const uint8_t *buf, int len,
               const struct sockaddr_in *addr);
int udpRecvFrom(SOCKET sock, uint8_t *buf, int maxLen,
                struct sockaddr_in *fromAddr);

#endif /* TRANSPORT_UDP_INTERNAL_H */
