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
 *Name:          Transport UDP
 *Filename:      transport_udp.c
 *Author:        John Morrison
 *Purpose:
 *  UDP network transport for multiplayer games.
 *
 *  Client side:
 *    - Sends InputPackets to the server (with redundancy:
 *      last 3 inputs per packet for loss tolerance).
 *    - Receives state snapshots from server.
 *    - Handles join handshake and ping measurement.
 *
 *  Server side:
 *    - Receives inputs from clients, applies to ServerSim.
 *    - Broadcasts per-player filtered snapshots each tick.
 *    - Manages connection lifecycle (join/leave/timeout).
 *    - Periodic ping/pong for latency measurement.
 *********************************************************/

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
#include "bases.h"
#include "pillbox.h"
#include "starts.h"
#include "gametype.h"
#include "players.h"
#include "screen.h"
#include "messages.h"
#include "util.h"
#include "client_sim.h"
#include "game_sim.h"
#include "../gui/winbolo.h"
#include "../gui/dialogAlliance.h"
#include "../server/geolookup.h"
#include "../server/server_sim.h"
#include "../winbolonet/winbolonet.h"
#include "../server/threads.h"
#include "sounddist.h"
#include "bot_manager.h"
#include "../steam/steam_wrapper.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

/* ---- Internal helpers ---- */

/* Maximum UDP datagram payload we'll send */
#define UDP_MAX_PAYLOAD 1024

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

/* Join retry interval in ticks (1 second) */
#define JOIN_RETRY_INTERVAL 50

/* Max join attempts before giving up */
#define JOIN_MAX_RETRIES 10

/* ---- Simulated server latency (set to 0 to disable) ----
 * Delays packet delivery on the server receive path by this many ms.
 * For localhost testing, set to 100 to simulate ~200ms RTT (100ms each way
 * since the server delays both receiving client inputs and sending snapshots
 * back through the same delayed receive path on the client's next poll).
 * Only compiled into the dedicated server target (WinBoloDS). */
#define SIM_LATENCY_SERVER_MS  0

#if SIM_LATENCY_SERVER_MS > 0 && !defined(HAVE_SCREEN_C)
typedef struct {
    uint8_t  data[2048];
    int      len;
    struct sockaddr_in from;
    uint64_t deliverAt;  /* SDL tick (ms) when this packet should be released */
} DelayedPacket;

#define DELAY_QUEUE_SIZE 512
static DelayedPacket serverDelayQueue[DELAY_QUEUE_SIZE];
static int serverDelayHead = 0;
static int serverDelayCount = 0;
#endif

/* ================================================================
 * Serialization helpers — pack/unpack structs to/from wire format
 * All multi-byte values use network byte order (big-endian).
 * ================================================================ */

static void packU16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
}

static uint16_t unpackU16(const uint8_t *buf) {
    return (uint16_t)((buf[0] << 8) | buf[1]);
}

static void packU32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val & 0xFF);
}

static uint32_t unpackU32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |
           (uint32_t)buf[3];
}

static void packHeader(uint8_t *buf, uint8_t packetType, uint32_t sequence) {
    buf[0] = BOLO_NEW_MAGIC_0;
    buf[1] = BOLO_NEW_MAGIC_1;
    buf[2] = packetType;
    buf[3] = 0; /* reserved */
    packU32(buf + 4, sequence);
}

#define PACKET_HEADER_SIZE 8

/* Lobby slot wire format size: connected(1) + playerName(32) + teamNumber(1) + ready(1) + isBot(1) + pingMs(2) + countryCode(2) + wbn(1) + steam(1) */
#define LOBBY_SLOT_WIRE_SIZE (1 + PACKET_MAX_PLAYER_NAME + 1 + 1 + 1 + 2 + 2 + 1 + 1)

/* Lobby settings tail: mapName(36) + gameType(1) + hiddenMines(1) + aiType(1) + gameLength(4) + pillCount(1) + baseCount(1) + startCount(1) + mapSkipAvailable(1) */
#define LOBBY_SETTINGS_SIZE  (MAP_STR_SIZE + 1 + 1 + 1 + 4 + 1 + 1 + 1 + 1)
#define LOBBY_STATE_PAYLOAD  (1 + MAX_TANKS * LOBBY_SLOT_WIRE_SIZE + LOBBY_SETTINGS_SIZE)

static uint8_t getPacketType(const uint8_t *buf, int len) {
    if (len < PACKET_HEADER_SIZE) return 0;
    if (buf[0] != BOLO_NEW_MAGIC_0 || buf[1] != BOLO_NEW_MAGIC_1) return 0;
    return buf[2];
}

static const char *packetTypeName(uint8_t type) {
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

/* Serialize one InputPacket into buf. Returns bytes written (15). */
static int packInputPacket(uint8_t *buf, const InputPacket *pkt) {
    packU32(buf, pkt->tick);
    buf[4] = pkt->playerNum;
    buf[5] = pkt->buttons;
    buf[6] = pkt->actions;
    buf[7] = pkt->buildAction;
    buf[8] = pkt->buildX;
    buf[9] = pkt->buildY;
    buf[10] = pkt->flags;
    packU32(buf + 11, pkt->eventAck);
    packU16(buf + 15, pkt->pingMs);
    return 17;
}

#define INPUT_PACKET_WIRE_SIZE 17

static void unpackInputPacket(const uint8_t *buf, InputPacket *pkt) {
    pkt->tick = unpackU32(buf);
    pkt->playerNum = buf[4];
    pkt->buttons = buf[5];
    pkt->actions = buf[6];
    pkt->buildAction = buf[7];
    pkt->buildX = buf[8];
    pkt->buildY = buf[9];
    pkt->flags = buf[10];
    pkt->eventAck = unpackU32(buf + 11);
    pkt->pingMs = unpackU16(buf + 15);
}

/* Serialize one TankSnapshot into buf. Returns bytes written (20). */
static int packTankSnapshot(uint8_t *buf, const TankSnapshot *ts) {
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

#define TANK_SNAPSHOT_WIRE_SIZE 27

/* Serialize one ShellSnapshot into buf. Returns bytes written (7). */
static int packShellSnapshot(uint8_t *buf, const ShellSnapshot *ss) {
    packU16(buf, ss->worldX);
    packU16(buf + 2, ss->worldY);
    buf[4] = ss->angle;
    buf[5] = ss->owner;
    buf[6] = ss->length;
    return SHELL_SNAPSHOT_WIRE_SIZE;
}

static void unpackShellSnapshot(const uint8_t *buf, ShellSnapshot *ss) {
    ss->worldX = unpackU16(buf);
    ss->worldY = unpackU16(buf + 2);
    ss->angle = buf[4];
    ss->owner = buf[5];
    ss->length = buf[6];
}

/* Serialize one ExplosionSnapshot into buf. Returns bytes written (5). */
static int packExplosionSnapshot(uint8_t *buf, const ExplosionSnapshot *es) {
    buf[0] = es->mx;
    buf[1] = es->my;
    buf[2] = es->px;
    buf[3] = es->py;
    buf[4] = es->length;
    return EXPLOSION_SNAPSHOT_WIRE_SIZE;
}

static void unpackExplosionSnapshot(const uint8_t *buf, ExplosionSnapshot *es) {
    es->mx = buf[0];
    es->my = buf[1];
    es->px = buf[2];
    es->py = buf[3];
    es->length = buf[4];
}

/* Serialize one GameEvent into buf. Returns bytes written (1 + dataSize). */
static int packGameEvent(uint8_t *buf, const GameEvent *ev) {
    int dataLen = gameEventDataSize(ev->type);
    buf[0] = ev->type;
    memcpy(buf + 1, ev->data, dataLen);
    return 1 + dataLen;
}

/* Deserialize one GameEvent from buf. Returns bytes consumed (1 + dataSize). */
static int unpackGameEvent(const uint8_t *buf, GameEvent *ev) {
    int dataLen;
    ev->type = buf[0];
    dataLen = gameEventDataSize(ev->type);
    memset(ev->data, 0, sizeof(ev->data));
    memcpy(ev->data, buf + 1, dataLen);
    return 1 + dataLen;
}

static int packBaseSnapshot(uint8_t *buf, const BaseSnapshot *bs) {
    buf[0] = bs->owner;
    buf[1] = bs->armour;
    buf[2] = bs->shells;
    buf[3] = bs->mines;
    return BASE_SNAPSHOT_WIRE_SIZE;
}

static void unpackBaseSnapshot(const uint8_t *buf, BaseSnapshot *bs) {
    bs->owner = buf[0];
    bs->armour = buf[1];
    bs->shells = buf[2];
    bs->mines = buf[3];
}

static int packPillSnapshot(uint8_t *buf, const PillSnapshot *ps) {
    buf[0] = ps->x;
    buf[1] = ps->y;
    buf[2] = ps->owner;
    buf[3] = ps->armour;
    buf[4] = ps->speed;
    buf[5] = ps->inTank;
    return PILL_SNAPSHOT_WIRE_SIZE;
}

static void unpackPillSnapshot(const uint8_t *buf, PillSnapshot *ps) {
    ps->x = buf[0];
    ps->y = buf[1];
    ps->owner = buf[2];
    ps->armour = buf[3];
    ps->speed = buf[4];
    ps->inTank = buf[5];
}

static void unpackTankSnapshot(const uint8_t *buf, TankSnapshot *ts) {
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

/* Create a non-blocking UDP socket */
static SOCKET createUdpSocket(void) {
    SOCKET sock;
    unsigned long nonBlock = 1;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    ioctlsocket(sock, FIONBIO, &nonBlock);
    return sock;
}

/* Send a buffer via UDP to a specific address */
static void udpSendTo(SOCKET sock, const uint8_t *buf, int len,
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

#if SIM_LATENCY_SERVER_MS > 0 && !defined(HAVE_SCREEN_C)
/* Receive with simulated latency — buffers packets and releases after delayMs. */
static int udpRecvFromDelayed(SOCKET sock, uint8_t *buf, int maxLen,
                               struct sockaddr_in *fromAddr,
                               int delayMs, DelayedPacket *queue,
                               int *head, int *count) {
    /* Drain socket into delay queue */
    while (*count < DELAY_QUEUE_SIZE) {
        struct sockaddr_in tmpAddr;
        socklen_t tmpLen = sizeof(tmpAddr);
        int n = recvfrom(sock, (char *)queue[(*head + *count) % DELAY_QUEUE_SIZE].data,
                         sizeof(queue[0].data), 0,
                         (struct sockaddr *)&tmpAddr, &tmpLen);
        if (n == SOCKET_ERROR) break;
        int idx = (*head + *count) % DELAY_QUEUE_SIZE;
        queue[idx].len = n;
        queue[idx].from = tmpAddr;
        queue[idx].deliverAt = SDL_GetTicks() + delayMs;
        (*count)++;
    }

    /* Release oldest packet if its delay has elapsed */
    if (*count > 0 && SDL_GetTicks() >= queue[*head].deliverAt) {
        int copyLen = queue[*head].len;
        if (copyLen > maxLen) copyLen = maxLen;
        memcpy(buf, queue[*head].data, copyLen);
        *fromAddr = queue[*head].from;
        *head = (*head + 1) % DELAY_QUEUE_SIZE;
        (*count)--;
        return copyLen;
    }
    return -1;
}
#endif

/* Receive a UDP datagram. Returns bytes received, or -1 if none available. */
static int udpRecvFrom(SOCKET sock, uint8_t *buf, int maxLen,
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

/* ================================================================
 * CLIENT SIDE
 * ================================================================ */

typedef struct {
    SOCKET sock;
    struct sockaddr_in serverAddr;
    BYTE playerNum;
    UdpClientJoinState joinState;
    char playerName[PACKET_MAX_PLAYER_NAME];
    char password[MAP_STR_SIZE];
    char wbnToken[WBN_TOKEN_WIRE_LEN];
    bool wbnReauthSent;  /* TRUE after sending re-auth, reset when WBN flag restored */
    uint32_t outSequence;

    /* Input redundancy ring buffer */
    InputPacket inputRing[CLIENT_INPUT_RING_SIZE];
    uint32_t inputRingCount;  /* Total inputs recorded */

    /* Latest received snapshot */
    bool hasSnapshot;
    SnapshotHeader snapshotHdr;
    TankSnapshot snapshotTanks[MAX_TANKS];
    ShellSnapshot snapshotShells[MAX_SNAPSHOT_SHELLS];
    ExplosionSnapshot snapshotExplosions[MAX_SNAPSHOT_EXPLOSIONS];
    BaseSnapshot snapshotBases[MAX_SNAPSHOT_BASES];
    PillSnapshot snapshotPills[MAX_SNAPSHOT_PILLS];
    GameEvent snapshotEvents[MAX_SNAPSHOT_EVENTS];
    uint32_t lastSnapshotSeq;  /* Sequence number of latest snapshot */
    uint32_t lastSnapshotTick; /* Local tick when last snapshot arrived (for timeout) */

    /* Reliable event dedup */
    uint32_t reliableEventAck;  /* Next expected reliable event seq (init to 1) */

    /* Join handshake state */
    uint32_t joinAttempts;
    uint32_t ticksSinceJoinSent;

    /* Ping */
    uint32_t lastPingSentTick;
    uint32_t pingClientTime;  /* Monotonic counter used as ping timestamp */
    uint16_t pingMs;
    uint32_t localTick;  /* Local tick counter for timing */

    /* Map download state */
    BYTE    *mapDownloadBuf;     /* Buffer for reassembling compressed map */
    uint32_t mapDownloadTotal;   /* Total expected bytes */
    uint32_t mapDownloadReceived;/* Bytes received so far */
    uint16_t mapChunksExpected;  /* Total chunks expected */
    uint16_t mapChunksReceived;  /* Number of unique chunks received */
    bool    *mapChunkReceived;   /* Bitfield: which chunks we've gotten */

    /* Game settings received from server */
    gameType serverGameType;
    bool     serverHiddenMines;
    int32_t  serverStartDelay;
    int32_t  serverGameLen;

    /* Join reject reason from server */
    char joinRejectReason[64];

    /* Owning ClientSim — used for player state updates in callbacks */
    ClientSim *clientSim;

} TransportUdpClientCtx;

/* Build an input packet into buf, returns length */
static int buildInputPacket(TransportUdpClientCtx *c, uint8_t *buf) {
    int offset;
    int i, count;

    packHeader(buf, PACKET_INPUT, c->outSequence++);
    offset = PACKET_HEADER_SIZE;

    count = INPUT_REDUNDANCY_COUNT;
    if (c->inputRingCount < (uint32_t)count) {
        count = (int)c->inputRingCount;
    }

    buf[offset++] = (uint8_t)count;

    for (i = count - 1; i >= 0; i--) {
        uint32_t idx = (c->inputRingCount - 1 - (uint32_t)i) % CLIENT_INPUT_RING_SIZE;
        offset += packInputPacket(buf + offset, &c->inputRing[idx]);
    }

    return offset;
}

/* Client sendInput: serialize input with redundancy, send to server */
static void udpClientSendInput(void *ctx, const InputPacket *input) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;
    uint8_t buf[UDP_MAX_PAYLOAD];
    int len;

    if (c->joinState != UDP_CLIENT_CONNECTED) {
        return;
    }

    /* Store in ring buffer for redundancy — stamp with current reliable ACK and ping */
    {
        InputPacket stamped = *input;
        stamped.eventAck = c->reliableEventAck;
        stamped.pingMs = c->pingMs;
        c->inputRing[c->inputRingCount % CLIENT_INPUT_RING_SIZE] = stamped;
    }
    c->inputRingCount++;

    len = buildInputPacket(c, buf);
    udpSendTo(c->sock, buf, len, &c->serverAddr);
}

/* Process a single incoming packet (used by both direct and delayed paths) */
static void udpClientProcessPacket(TransportUdpClientCtx *c,
                                   const uint8_t *buf, int len) {
    uint8_t pktType = getPacketType(buf, len);


    /* Reset timeout on any valid server packet — lobby state doesn't send
     * snapshots, so without this the client times out after 20s in lobby.
     * Also reset during map (re-)download so a map change doesn't time out. */
    if (pktType != 0 && (c->joinState == UDP_CLIENT_CONNECTED ||
                         c->joinState == UDP_CLIENT_DOWNLOADING_MAP)) {
        c->lastSnapshotTick = c->localTick;
    }

    switch (pktType) {
    case PACKET_JOIN_ACCEPT:
        /* Accept packet format:
         *   [header 8] [playerNum 1] [serverTick 4] [gameType 1]
         *   [hiddenMines 1] [startDelay 4] [gameLen 4] [mapSize 4]
         * Total: 8 + 19 = 27 bytes minimum */
        if ((c->joinState == UDP_CLIENT_JOINING ||
             c->joinState == UDP_CLIENT_DOWNLOADING_MAP) &&
            len >= PACKET_HEADER_SIZE + 19) {
            int pos = PACKET_HEADER_SIZE;
            uint32_t mapSize;

            c->playerNum = buf[pos++];
            pos += 4; /* skip serverTick */
            c->serverGameType = (gameType)buf[pos++];
            c->serverHiddenMines = buf[pos++] ? TRUE : FALSE;
            c->serverStartDelay = (int32_t)unpackU32(buf + pos);
            pos += 4;
            c->serverGameLen = (int32_t)unpackU32(buf + pos);
            pos += 4;
            mapSize = unpackU32(buf + pos);
            pos += 4;

            if (mapSize == 0 || mapSize > MAP_DOWNLOAD_MAX_SIZE) {
                c->joinState = UDP_CLIENT_ERROR;
                break;
            }

            /* Allocate map download buffer */
            if (c->mapDownloadBuf != NULL) {
                free(c->mapDownloadBuf);
            }
            c->mapDownloadBuf = (BYTE *)malloc(mapSize);
            if (c->mapDownloadBuf == NULL) {
                c->joinState = UDP_CLIENT_ERROR;
                break;
            }
            memset(c->mapDownloadBuf, 0, mapSize);
            c->mapDownloadTotal = mapSize;
            c->mapDownloadReceived = 0;

            /* Calculate expected chunks */
            c->mapChunksExpected = (uint16_t)((mapSize + MAP_DOWNLOAD_CHUNK_SIZE - 1) / MAP_DOWNLOAD_CHUNK_SIZE);
            c->mapChunksReceived = 0;
            if (c->mapChunkReceived != NULL) {
                free(c->mapChunkReceived);
            }
            c->mapChunkReceived = (bool *)calloc(c->mapChunksExpected, sizeof(bool));
            if (c->mapChunkReceived == NULL) {
                free(c->mapDownloadBuf);
                c->mapDownloadBuf = NULL;
                c->joinState = UDP_CLIENT_ERROR;
                break;
            }

            c->joinState = UDP_CLIENT_DOWNLOADING_MAP;

            /* Send ack to tell server we're ready for map chunks */
            {
                uint8_t ackBuf[PACKET_HEADER_SIZE + 2];
                packHeader(ackBuf, PACKET_MAP_ACK, c->outSequence++);
                packU16(ackBuf + PACKET_HEADER_SIZE, 0xFFFF); /* 0xFFFF = "ready for map" */
                udpSendTo(c->sock, ackBuf, sizeof(ackBuf), &c->serverAddr);
            }
        }
        break;

    case PACKET_MAP_DOWNLOAD:
        /* Map chunk format:
         *   [header 8] [chunkIndex 2] [chunkSize 2] [data...] */
        if (c->joinState == UDP_CLIENT_DOWNLOADING_MAP &&
            len >= PACKET_HEADER_SIZE + 4 && c->mapDownloadBuf != NULL) {
            uint16_t chunkIdx = unpackU16(buf + PACKET_HEADER_SIZE);
            uint16_t chunkSize = unpackU16(buf + PACKET_HEADER_SIZE + 2);
            uint32_t offset;
            const uint8_t *chunkData = buf + PACKET_HEADER_SIZE + 4;

            if (chunkIdx >= c->mapChunksExpected) break;
            if (len < PACKET_HEADER_SIZE + 4 + chunkSize) break;

            offset = (uint32_t)chunkIdx * MAP_DOWNLOAD_CHUNK_SIZE;
            if (offset + chunkSize > c->mapDownloadTotal) break;

            /* Copy chunk data */
            memcpy(c->mapDownloadBuf + offset, chunkData, chunkSize);

            /* Track which chunks we've received */
            if (!c->mapChunkReceived[chunkIdx]) {
                c->mapChunkReceived[chunkIdx] = TRUE;
                c->mapChunksReceived++;
                c->mapDownloadReceived += chunkSize;
            }

            /* Ack this chunk */
            {
                uint8_t ackBuf[PACKET_HEADER_SIZE + 2];
                packHeader(ackBuf, PACKET_MAP_ACK, c->outSequence++);
                packU16(ackBuf + PACKET_HEADER_SIZE, chunkIdx);
                udpSendTo(c->sock, ackBuf, sizeof(ackBuf), &c->serverAddr);
            }

            /* Check if all chunks received */
            if (c->mapChunksReceived >= c->mapChunksExpected) {
                c->joinState = UDP_CLIENT_CONNECTED;
                c->clientSim->mapDownloadComplete = true;
            }
        }
        break;

    case PACKET_JOIN_REJECT:
        if (len >= PACKET_HEADER_SIZE + 64) {
            memcpy(c->joinRejectReason, buf + PACKET_HEADER_SIZE, 64);
            c->joinRejectReason[63] = '\0';
        } else if (len > PACKET_HEADER_SIZE) {
            memcpy(c->joinRejectReason, buf + PACKET_HEADER_SIZE, len - PACKET_HEADER_SIZE);
            c->joinRejectReason[len - PACKET_HEADER_SIZE - 1] = '\0';
        } else {
            strncpy(c->joinRejectReason, "Connection rejected", 63);
            c->joinRejectReason[63] = '\0';
        }
        c->joinState = UDP_CLIENT_ERROR;
        break;

    case PACKET_STATE_SNAPSHOT: {
        uint32_t seq = unpackU32(buf + 4);
        int pos = PACKET_HEADER_SIZE;
        int i;
        uint8_t tankCount, shellCount, explosionCount;
        uint8_t baseCount, pillCount, reliableEventCount;
        uint32_t reliableBaseSeq;
        int newEventCount = 0;

        /* Ignore stale snapshots */
        if (c->hasSnapshot && seq <= c->lastSnapshotSeq) {
            break;
        }

        /* New header: serverTick(4) + lastProcessedInput(4) + tankCount(1)
         * + shellCount(1) + explosionCount(1) + baseCount(1) + pillCount(1)
         * + reliableEventCount(1) + reliableBaseSeq(4) + mapChecksum(2) = 20 bytes */
        if (len < pos + 20) break;

        c->snapshotHdr.serverTick = unpackU32(buf + pos);
        pos += 4;
        c->snapshotHdr.lastProcessedInput = unpackU32(buf + pos);
        pos += 4;
        tankCount = buf[pos++];
        shellCount = buf[pos++];
        explosionCount = buf[pos++];
        baseCount = buf[pos++];
        pillCount = buf[pos++];
        reliableEventCount = buf[pos++];
        reliableBaseSeq = unpackU32(buf + pos);
        pos += 4;
        c->snapshotHdr.mapChecksum = unpackU16(buf + pos);
        pos += 2;

        c->snapshotHdr.tankCount = tankCount;
        c->snapshotHdr.shellCount = shellCount;
        c->snapshotHdr.explosionCount = explosionCount;
        c->snapshotHdr.baseCount = baseCount;
        c->snapshotHdr.pillCount = pillCount;

        /* Unpack tanks */
        if (tankCount > MAX_TANKS) tankCount = MAX_TANKS;
        if (len < pos + tankCount * TANK_SNAPSHOT_WIRE_SIZE) break;
        for (i = 0; i < tankCount; i++) {
            unpackTankSnapshot(buf + pos, &c->snapshotTanks[i]);
            pos += TANK_SNAPSHOT_WIRE_SIZE;
        }

        /* Unpack shells */
        if (shellCount > MAX_SNAPSHOT_SHELLS) shellCount = MAX_SNAPSHOT_SHELLS;
        if (len < pos + shellCount * SHELL_SNAPSHOT_WIRE_SIZE) break;
        for (i = 0; i < shellCount; i++) {
            unpackShellSnapshot(buf + pos, &c->snapshotShells[i]);
            pos += SHELL_SNAPSHOT_WIRE_SIZE;
        }

        /* Unpack explosions */
        if (explosionCount > MAX_SNAPSHOT_EXPLOSIONS) explosionCount = MAX_SNAPSHOT_EXPLOSIONS;
        if (len < pos + explosionCount * EXPLOSION_SNAPSHOT_WIRE_SIZE) break;
        for (i = 0; i < explosionCount; i++) {
            unpackExplosionSnapshot(buf + pos, &c->snapshotExplosions[i]);
            pos += EXPLOSION_SNAPSHOT_WIRE_SIZE;
        }

        /* Unpack bases */
        if (baseCount > MAX_SNAPSHOT_BASES) baseCount = MAX_SNAPSHOT_BASES;
        if (len < pos + baseCount * BASE_SNAPSHOT_WIRE_SIZE) break;
        for (i = 0; i < baseCount; i++) {
            unpackBaseSnapshot(buf + pos, &c->snapshotBases[i]);
            pos += BASE_SNAPSHOT_WIRE_SIZE;
        }

        /* Unpack pills */
        if (pillCount > MAX_SNAPSHOT_PILLS) pillCount = MAX_SNAPSHOT_PILLS;
        if (len < pos + pillCount * PILL_SNAPSHOT_WIRE_SIZE) break;
        for (i = 0; i < pillCount; i++) {
            unpackPillSnapshot(buf + pos, &c->snapshotPills[i]);
            pos += PILL_SNAPSHOT_WIRE_SIZE;
        }

        /* Unpack reliable events with dedup */
        if (reliableEventCount > MAX_SNAPSHOT_EVENTS) reliableEventCount = MAX_SNAPSHOT_EVENTS;
        if (reliableEventCount > 0) {
            /*
            fprintf(stderr, "[UDP CLIENT] Snapshot has %u wire events, baseSeq=%u, our ack=%u\n",
                    reliableEventCount, reliableBaseSeq, c->reliableEventAck);
        */
        }
        for (i = 0; i < reliableEventCount; i++) {
            uint32_t evSeq = reliableBaseSeq + (uint32_t)i;
            GameEvent ev;
            if (pos + 1 > len) break;  /* Need at least the type byte */
            pos += unpackGameEvent(buf + pos, &ev);
            /* Only apply events we haven't seen yet */
            if (evSeq >= c->reliableEventAck) {
                if (newEventCount < MAX_SNAPSHOT_EVENTS) {
                    c->snapshotEvents[newEventCount++] = ev;
                }
            }
        }
        /* Advance our ACK to cover all events we just received */
        if (reliableEventCount > 0) {
            uint32_t lastSeq = reliableBaseSeq + reliableEventCount;
            if (lastSeq > c->reliableEventAck) {
                c->reliableEventAck = lastSeq;
            }
        }
        c->snapshotHdr.reliableEventCount = (uint8_t)newEventCount;
        c->snapshotHdr.reliableBaseSeq = reliableBaseSeq;

        c->hasSnapshot = true;
        c->lastSnapshotSeq = seq;
        c->lastSnapshotTick = c->localTick;
        break;
    }

    case PACKET_PONG:
        if (len >= PACKET_HEADER_SIZE + 8) {
            uint32_t clientTime = unpackU32(buf + PACKET_HEADER_SIZE);
            uint32_t now = SDL_GetTicks();
            if (now >= clientTime) {
                c->pingMs = (uint16_t)(now - clientTime);
                playersSetPing(&c->clientSim->sim.plyrs, c->playerNum, c->pingMs);
            }
        }
        break;

    case PACKET_PLAYER_JOINED:
        if (len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME) {
            uint8_t pNum = buf[PACKET_HEADER_SIZE];
            char pName[PACKET_MAX_PLAYER_NAME];
            char cc[3] = {0, 0, 0};
            memcpy(pName, buf + PACKET_HEADER_SIZE + 1, PACKET_MAX_PLAYER_NAME);
            pName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
            if (len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 2) {
                cc[0] = (char)buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME];
                cc[1] = (char)buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 1];
            }
            if (pNum != c->playerNum) {
                playersSetPlayer(c->clientSim, &c->clientSim->sim.plyrs, pNum, pName, cc,
                                 0, 0, 0, 0, 0, FALSE, 0, NULL, FALSE);
                /* Show join message in lobby chat */
                if (c->clientSim->inLobby) {
                    char joinMsg[64];
                    snprintf(joinMsg, sizeof(joinMsg), "%s has joined.", pName);
                    clientSimAppendLobbyChat(c->clientSim, "***", joinMsg);
                }
            }
        }
        break;

    case PACKET_PLAYER_LIST:
        /* Player list format: [header][count]
         *   [playerNum 1][name 32][countryCode 2][numAllies 1][ally0 1]...
         * Each entry is variable-length. */
        if (len >= PACKET_HEADER_SIZE + 1) {
            uint8_t plCount = buf[PACKET_HEADER_SIZE];
            int plPos = PACKET_HEADER_SIZE + 1;
            int p;
            for (p = 0; p < plCount; p++) {
                uint8_t pNum;
                char pName[PACKET_MAX_PLAYER_NAME];
                char cc[3] = {0, 0, 0};
                uint8_t numAllies;
                BYTE allies[MAX_TANKS];

                if (plPos + 1 + PACKET_MAX_PLAYER_NAME + 2 + 1 > len) break;
                pNum = buf[plPos];
                memcpy(pName, buf + plPos + 1, PACKET_MAX_PLAYER_NAME);
                pName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
                plPos += 1 + PACKET_MAX_PLAYER_NAME;
                cc[0] = (char)buf[plPos++];
                cc[1] = (char)buf[plPos++];
                numAllies = buf[plPos++];
                if (numAllies > MAX_TANKS) numAllies = MAX_TANKS;
                if (plPos + numAllies > len) break;
                if (numAllies > 0) {
                    memcpy(allies, buf + plPos, numAllies);
                    plPos += numAllies;
                }
                if (pNum != c->playerNum) {
                    playersSetPlayer(c->clientSim, &c->clientSim->sim.plyrs, pNum, pName, cc,
                                     0, 0, 0, 0, 0, FALSE,
                                     numAllies, numAllies > 0 ? allies : NULL, FALSE);
                }
            }
        }
        break;

    case PACKET_PLAYER_LEFT:
        if (len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME) {
            uint8_t pNum = buf[PACKET_HEADER_SIZE];
            char pName[PACKET_MAX_PLAYER_NAME];
            memcpy(pName, buf + PACKET_HEADER_SIZE + 1, PACKET_MAX_PLAYER_NAME);
            pName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
            if (pNum != c->playerNum) {
                /* Register player name from packet before leaving,
                 * in case they were only auto-registered with a placeholder */
                if (playersIsInUse(&c->clientSim->sim.plyrs, pNum) == TRUE) {
                    playersLeaveGame(&c->clientSim->sim, &c->clientSim->sim.plyrs, pNum, FALSE);
                }
                /* Show leave message in lobby chat */
                if (c->clientSim->inLobby) {
                    char leaveMsg[64];
                    snprintf(leaveMsg, sizeof(leaveMsg), "%s has left.", pName);
                    clientSimAppendLobbyChat(c->clientSim, "***", leaveMsg);
                }
            }
        }
        break;

    case PACKET_NAME_CHANGE:
        /* Name change format:
         *   [header 8] [playerNum 1] [newName PACKET_MAX_PLAYER_NAME] */
        if (len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME) {
            uint8_t pNum = buf[PACKET_HEADER_SIZE];
            char newName[PACKET_MAX_PLAYER_NAME];
            memcpy(newName, buf + PACKET_HEADER_SIZE + 1, PACKET_MAX_PLAYER_NAME);
            newName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
            playersSetPlayerName(c->clientSim, &c->clientSim->sim, &c->clientSim->sim.plyrs, pNum, newName, FALSE);
        }
        break;

    case PACKET_CHAT_BROADCAST:
        /* Chat broadcast format:
         *   [header 8] [fromPlayer 1] [destPlayer 1] [message up to 128] */
        if (len > PACKET_HEADER_SIZE + 2) {
            uint8_t fromPlayer = buf[PACKET_HEADER_SIZE];
            int msgLen = len - PACKET_HEADER_SIZE - 2;
            char message[129];
            if (msgLen > 128) msgLen = 128;
            memcpy(message, buf + PACKET_HEADER_SIZE + 2, msgLen);
            message[msgLen] = '\0';
            if (fromPlayer >= MAX_TANKS) {
                /* Server message */
                screenNetStatusMessage(c->clientSim, message);
            } else {
                screenIncomingMessageCS(c->clientSim, fromPlayer, message);
            }
        }
        break;

    case PACKET_ALLIANCE_UPDATE:
        /* Alliance update format:
         *   [header 8] [eventType 1] [fromPlayer 1] [toPlayer 1] */
        if (len >= PACKET_HEADER_SIZE + 3) {
            uint8_t eventType = buf[PACKET_HEADER_SIZE];
            uint8_t fromPlayer = buf[PACKET_HEADER_SIZE + 1];
            uint8_t toPlayer = buf[PACKET_HEADER_SIZE + 2];

            switch (eventType) {
            case ALLIANCE_EVENT_REQUEST:
                /* Only show dialog if we are the target */
                if (toPlayer == c->playerNum) {
                    char pName[FILENAME_MAX];
                    playersGetPlayerName(&c->clientSim->sim.plyrs, fromPlayer, pName, FALSE);
                    if (windowShowAllianceRequest() == TRUE) {
                        dialogAllianceSetName(pName, fromPlayer);
                    } else {
                        char str[FILENAME_MAX + 64];
                        snprintf(str, sizeof(str),
                                 "You have ignored alliance request from %s",
                                 pName);
                        clientMessageAdd(&c->clientSim->messages, networkStatus, "Alliance Request", str);
                    }
                }
                break;
            case ALLIANCE_EVENT_ACCEPT:
                playersAcceptAlliance(&c->clientSim->sim, &c->clientSim->sim.plyrs, fromPlayer,
                                     toPlayer, FALSE);
                break;
            case ALLIANCE_EVENT_LEAVE:
                playersLeaveAlliance(&c->clientSim->sim, &c->clientSim->sim.plyrs, fromPlayer, FALSE);
                break;
            }
        }
        break;

    case PACKET_SERVER_SHUTDOWN:
        c->joinState = UDP_CLIENT_SERVER_SHUTDOWN;
        break;

    case PACKET_LOBBY_STATE:
        /* Full lobby snapshot:
         *   [header 8] [serverState 1]
         *   16 slots × [connected 1] [playerName 32] [teamNumber 1] [ready 1] [isBot 1]
         *              [pingMs 2] [countryCode 2] [wbn 1] [steam 1]
         *   Game settings tail:
         *     [mapName 36] [gameType 1] [hiddenMines 1] [aiType 1]
         *     [gameLength 4] [pillCount 1] [baseCount 1] [startCount 1]
         *     [mapSkipAvailable 1] */
        if (len >= PACKET_HEADER_SIZE + LOBBY_STATE_PAYLOAD) {
            int pos = PACKET_HEADER_SIZE;
            int i;
            uint8_t serverState = buf[pos++];
            for (i = 0; i < MAX_TANKS; i++) {
                c->clientSim->lobbySlots[i].connected = buf[pos++] ? true : false;
                memcpy(c->clientSim->lobbySlots[i].playerName, buf + pos, PACKET_MAX_PLAYER_NAME);
                c->clientSim->lobbySlots[i].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
                pos += PACKET_MAX_PLAYER_NAME;
                c->clientSim->lobbySlots[i].teamNumber = buf[pos++];
                c->clientSim->lobbySlots[i].ready = buf[pos++] ? true : false;
                c->clientSim->lobbySlots[i].isBot = buf[pos++] ? true : false;
                c->clientSim->lobbySlots[i].pingMs = (uint16_t)(buf[pos] << 8 | buf[pos + 1]);
                pos += 2;
                c->clientSim->lobbySlots[i].countryCode[0] = (char)buf[pos++];
                c->clientSim->lobbySlots[i].countryCode[1] = (char)buf[pos++];
                c->clientSim->lobbySlots[i].countryCode[2] = '\0';
                c->clientSim->lobbySlots[i].wbnParticipant = buf[pos++] ? true : false;
                c->clientSim->lobbySlots[i].steamParticipant = buf[pos++] ? true : false;
                if (c->clientSim->lobbySlots[i].wbnParticipant || c->clientSim->lobbySlots[i].steamParticipant) {
                    SDL_Log("[WBN LOBBY] slot %d wbn=%d steam=%d",
                            i, c->clientSim->lobbySlots[i].wbnParticipant,
                            c->clientSim->lobbySlots[i].steamParticipant);
                }
            }
            /* Game settings tail */
            strncpy(c->clientSim->mapName, (const char *)(buf + pos), MAP_STR_SIZE - 1);
            c->clientSim->mapName[MAP_STR_SIZE - 1] = '\0';
            pos += MAP_STR_SIZE;
            c->clientSim->lobbyGameType = (gameType)buf[pos++];
            c->clientSim->lobbyHiddenMines = buf[pos++] ? true : false;
            c->clientSim->lobbyAiType = buf[pos++];
            c->clientSim->lobbyTimeLimit = (int32_t)unpackU32(buf + pos);
            pos += 4;
            c->clientSim->lobbyPillCount = buf[pos++];
            c->clientSim->lobbyBaseCount = buf[pos++];
            c->clientSim->lobbyStartCount = buf[pos++];
            c->clientSim->mapSkipAvailable = buf[pos++] ? true : false;
            /* Map server state to client netStatus — preserve countdown state */
            if (serverState == 1) { /* serverStateCountdown */
                c->clientSim->netStat = netLobbyCountdown;
            } else {
                c->clientSim->netStat = netLobby;
            }
            c->clientSim->inLobby = true;

            /* Lonely lobby tracking (ACH_LONELY_LOBBY) */
            {
                int connectedCount = 0;
                for (i = 0; i < MAX_TANKS; i++) {
                    if (c->clientSim->lobbySlots[i].connected) {
                        connectedCount++;
                    }
                }
                if (connectedCount == 1) {
                    if (c->clientSim->lobbyAloneStartTick == 0) {
                        c->clientSim->lobbyAloneStartTick = SDL_GetTicks();
                        if (c->clientSim->lobbyAloneStartTick == 0) {
                            c->clientSim->lobbyAloneStartTick = 1; /* avoid 0 sentinel */
                        }
                    } else {
                        uint32_t elapsed = SDL_GetTicks() - c->clientSim->lobbyAloneStartTick;
                        if (elapsed >= 3600000) { /* 60 minutes */
                            steam_set_achievement("ACH_LONELY_LOBBY");
                            steam_store_stats();
                        }
                    }
                } else {
                    c->clientSim->lobbyAloneStartTick = 0;
                }
            }

            /* A fresh lobby snapshot supersedes any pending balance proposal */
            c->clientSim->balanceProposalActive = false;
            memset(c->clientSim->balanceProposal, 0, sizeof(c->clientSim->balanceProposal));

            /* WBN re-auth: if our slot lost its WBN flag (server re-registered
             * with WBN between rounds) and we have a token, re-authenticate */
            if (c->wbnToken[0] != '\0' && c->playerNum < MAX_TANKS &&
                !c->clientSim->lobbySlots[c->playerNum].wbnParticipant) {
                if (!c->wbnReauthSent) {
                    c->wbnReauthSent = TRUE;
                    /* Inline re-auth send (we have ctx, not Transport*) */
                    {
                        uint8_t ra[PACKET_HEADER_SIZE + WBN_TOKEN_WIRE_LEN];
                        packHeader(ra, PACKET_WBN_REAUTH, c->outSequence++);
                        memcpy(ra + PACKET_HEADER_SIZE, c->wbnToken, WBN_TOKEN_WIRE_LEN);
                        udpSendTo(c->sock, ra, sizeof(ra), &c->serverAddr);
                    }
                    SDL_Log("[WBN] Sent re-auth for slot %d", c->playerNum);
                }
            } else {
                /* Flag was restored or not needed — reset for next round */
                c->wbnReauthSent = FALSE;
            }
        }
        break;

    case PACKET_LOBBY_UPDATE:
        /* Single-player delta:
         *   [header 8] [playerNum 1] [connected 1] [playerName 32]
         *   [teamNumber 1] [ready 1] [isBot 1] [pingMs 2] [countryCode 2]
         *   [wbn 1] [steam 1] */
        if (len >= PACKET_HEADER_SIZE + 1 + LOBBY_SLOT_WIRE_SIZE) {
            int pos = PACKET_HEADER_SIZE;
            uint8_t playerNum = buf[pos++];
            if (playerNum < MAX_TANKS) {
                c->clientSim->lobbySlots[playerNum].connected = buf[pos++] ? true : false;
                memcpy(c->clientSim->lobbySlots[playerNum].playerName, buf + pos, PACKET_MAX_PLAYER_NAME);
                c->clientSim->lobbySlots[playerNum].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
                pos += PACKET_MAX_PLAYER_NAME;
                c->clientSim->lobbySlots[playerNum].teamNumber = buf[pos++];
                c->clientSim->lobbySlots[playerNum].ready = buf[pos++] ? true : false;
                c->clientSim->lobbySlots[playerNum].isBot = buf[pos++] ? true : false;
                c->clientSim->lobbySlots[playerNum].pingMs = (uint16_t)(buf[pos] << 8 | buf[pos + 1]);
                pos += 2;
                c->clientSim->lobbySlots[playerNum].countryCode[0] = (char)buf[pos++];
                c->clientSim->lobbySlots[playerNum].countryCode[1] = (char)buf[pos++];
                c->clientSim->lobbySlots[playerNum].countryCode[2] = '\0';
                c->clientSim->lobbySlots[playerNum].wbnParticipant = buf[pos++] ? true : false;
                c->clientSim->lobbySlots[playerNum].steamParticipant = buf[pos++] ? true : false;
            }
        }
        break;

    case PACKET_COUNTDOWN:
        /* [header 8] [secondsRemaining 1] */
        if (len >= PACKET_HEADER_SIZE + 1) {
            c->clientSim->countdownSeconds = buf[PACKET_HEADER_SIZE];
            c->clientSim->netStat = netLobbyCountdown;
        }
        break;

    case PACKET_GAME_START:
        /* [header 8] */
        c->clientSim->netStat = netRunning;
        c->clientSim->countdownSeconds = 0;
        /* Reset input ring so stale inputs from the previous game
         * are not sent as redundant packets in the new game. */
        c->inputRingCount = 0;
        /* Reset reliable event ack so it matches the server's reset queue.
         * Stale events from the previous game must not be applied to
         * the freshly-loaded map. */
        c->reliableEventAck = 1;
        /* Discard any snapshot buffered during the lobby/gameOver
         * transition.  A late STATE_SNAPSHOT from the previous game
         * can sit in hasSnapshot because the lobby tick path calls
         * transport->tick() but never getSnapshot().  If this stale
         * snapshot carries EVENT_MAP_CHANGE events from the old game,
         * they would overwrite the freshly-loaded new map. */
        c->hasSnapshot = false;
        break;

    case PACKET_GAME_OVER:
        /* [header 8] */
        /* Check win/loss achievements before transitioning state.
         * Determine winner using same logic as serverSimCheckGameWin:
         * one alliance owns all bases with armour above capture threshold. */
        {
            ClientSim *cs = c->clientSim;
            BYTE numBases = basesGetNumBases(&cs->sim.bs);
            BYTE first = NEUTRAL;
            bool allOwned = true;
            bool localWon = false;
            BYTE b;

            for (b = 1; b <= numBases && allOwned; b++) {
                BYTE owner = basesGetBaseOwner(&cs->sim.bs, b);
                BYTE shellsAmt, minesAmt, armourAmt;
                basesGetStats(&cs->sim.bs, b, &shellsAmt, &minesAmt, &armourAmt);
                if (owner == NEUTRAL || armourAmt <= MIN_ARMOUR_CAPTURE) {
                    allOwned = false;
                } else if (b == 1) {
                    first = owner;
                } else {
                    allOwned = playersIsAllie(&cs->sim.plyrs, owner, first);
                }
            }

            if (allOwned && numBases > 0) {
                /* Game was won by the alliance owning 'first' */
                localWon = (c->playerNum == first) ||
                           playersIsAllie(&cs->sim.plyrs, c->playerNum, first);
            }
            /* else: time limit or other end condition — no winner */

            if (allOwned && numBases > 0) {
                gameType gt = gameTypeGet(&cs->sim.game);
                BYTE numPlayers = playersGetNumPlayers(&cs->sim.plyrs);

                /* Tournament/strict tournament stats */
                if (gt == gameTournament || gt == gameStrictTournament) {
                    if (localWon) {
                        steam_increment_stat("STAT_TOURN_WINS", 1);
                        if (numPlayers == 2) {
                            steam_set_achievement("ACH_TOURN_WIN_1V1");
                        }
                    } else {
                        steam_increment_stat("STAT_TOURN_LOSSES", 1);
                        if (numPlayers == 2) {
                            steam_set_achievement("ACH_TOURN_LOSE_1V1");
                        }
                    }
                }

                /* Any game type win achievements */
                if (localWon) {
                    if (cs->myLgmLossesThisGame == 0) {
                        steam_set_achievement("ACH_WIN_NO_LGM_LOSS");
                    }
                    if (cs->myDeathsThisGame == 0 && numPlayers == 2) {
                        steam_set_achievement("ACH_1V1_FLAWLESS");
                    }
                }

                steam_store_stats();
            }
        }

        if (c->clientSim->inLobby) {
            c->clientSim->netStat = netLobby;
            c->clientSim->countdownSeconds = 0;
            c->clientSim->lobbyChatHistory[0] = '\0';
            /* Reset timeout tracking — the server won't send snapshots
             * during gameOver countdown, and the client's catch-up loop
             * advances localTick rapidly which can trigger a spurious
             * timeout before the game loop exits to the lobby. */
            c->lastSnapshotTick = c->localTick;
        } else {
            c->joinState = UDP_CLIENT_SERVER_SHUTDOWN;
        }
        break;

    case PACKET_LOBBY_MAP_CHANGE:
        /* [header 8] – server loaded a new map; reset to re-download */
        c->clientSim->mapDownloadComplete = false;
        c->joinState = UDP_CLIENT_JOINING;
        c->joinAttempts = 0;
        c->ticksSinceJoinSent = JOIN_RETRY_INTERVAL; /* send immediately */
        /* Votes reset server-side on map change */
        memset(c->clientSim->mapSkipVotes, 0, sizeof(c->clientSim->mapSkipVotes));
        c->clientSim->mapSkipMyVote = false;
        break;

    case PACKET_BALANCE_PROPOSAL:
        /* [header 8] [teamForSlot × 16] */
        if (len >= PACKET_HEADER_SIZE + 16) {
            int i;
            bool anyNonZero = false;
            memcpy(c->clientSim->balanceProposal, buf + PACKET_HEADER_SIZE, 16);
            for (i = 0; i < 16; i++) {
                if (c->clientSim->balanceProposal[i] != 0) {
                    anyNonZero = true;
                    break;
                }
            }
            c->clientSim->balanceProposalActive = anyNonZero;
        }
        break;

    case PACKET_MAP_SKIP_STATE:
        /* [header 8] [votes: 16 bytes, one per slot, 0 or 1] */
        if (len >= PACKET_HEADER_SIZE + MAX_TANKS) {
            int i;
            for (i = 0; i < MAX_TANKS; i++) {
                c->clientSim->mapSkipVotes[i] = buf[PACKET_HEADER_SIZE + i] ? true : false;
            }
            c->clientSim->mapSkipMyVote = c->clientSim->mapSkipVotes[c->playerNum];
        }
        break;

    default:
        break;
    }
}

/* Client tick: receive packets from server, handle join flow, ping */
static bool udpClientTick(void *ctx) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;
    uint8_t buf[UDP_MAX_PAYLOAD];
    struct sockaddr_in fromAddr;
    int len;

    c->localTick++;

    /* Receive all pending packets from the wire */
    while ((len = udpRecvFrom(c->sock, buf, sizeof(buf), &fromAddr)) > 0) {
        udpClientProcessPacket(c, buf, len);
    }

    /* Handle join handshake — send/resend join requests */
    if (c->joinState == UDP_CLIENT_JOINING) {
        c->ticksSinceJoinSent++;
        if (c->ticksSinceJoinSent >= JOIN_RETRY_INTERVAL) {
            if (c->joinAttempts >= JOIN_MAX_RETRIES) {
                c->joinState = UDP_CLIENT_ERROR;
            } else {
                uint8_t jbuf[PACKET_HEADER_SIZE + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3 + WBN_TOKEN_WIRE_LEN];
                int joffset = PACKET_HEADER_SIZE;
                packHeader(jbuf, PACKET_JOIN_REQUEST, c->outSequence++);
                memcpy(jbuf + joffset, c->playerName, PACKET_MAX_PLAYER_NAME);
                joffset += PACKET_MAX_PLAYER_NAME;
                memcpy(jbuf + joffset, c->password, MAP_STR_SIZE);
                joffset += MAP_STR_SIZE;
                jbuf[joffset++] = BOLO_VERSION_MAJOR;
                jbuf[joffset++] = BOLO_VERSION_MINOR;
                jbuf[joffset++] = BOLO_VERSION_REVISION;
                memcpy(jbuf + joffset, c->wbnToken, WBN_TOKEN_WIRE_LEN);
                joffset += WBN_TOKEN_WIRE_LEN;
                /* Join requests bypass delay — they're control plane */
                udpSendTo(c->sock, jbuf, joffset, &c->serverAddr);
                c->joinAttempts++;
                c->ticksSinceJoinSent = 0;
            }
        }
    }

    /* Periodic ping — bypasses delay so RTT measurement is accurate
     * (measures real network RTT, not simulated RTT) */
    if (c->joinState == UDP_CLIENT_CONNECTED) {
        if (c->localTick - c->lastPingSentTick >= PING_INTERVAL_TICKS) {
            uint8_t pbuf[PACKET_HEADER_SIZE + 8];
            packHeader(pbuf, PACKET_PING, c->outSequence++);
            packU32(pbuf + PACKET_HEADER_SIZE, SDL_GetTicks());
            packU32(pbuf + PACKET_HEADER_SIZE + 4, (uint32_t)c->pingMs);
            udpSendTo(c->sock, pbuf, sizeof(pbuf), &c->serverAddr);
            c->lastPingSentTick = c->localTick;
        }

        /* Timeout: if no snapshot received for CLIENT_TIMEOUT_TICKS,
         * the server has likely crashed or network is dead.
         * Skip when in lobby — the server doesn't send snapshots during
         * gameOver countdown or lobby state, and the catch-up loop can
         * advance localTick far beyond lastSnapshotTick. */
        if (c->lastSnapshotTick > 0 &&
            c->clientSim->netStat != netLobby &&
            c->clientSim->netStat != netLobbyCountdown &&
            c->localTick - c->lastSnapshotTick >= CLIENT_TIMEOUT_TICKS) {
            c->joinState = UDP_CLIENT_SERVER_SHUTDOWN;
        }
    }

    return true;
}

static bool udpClientGetSnapshotVtable(void *ctx, BYTE clientIdx,
                                       SnapshotHeader *hdr,
                                       TankSnapshot *tanks, int maxTanks,
                                       ShellSnapshot *shells, int maxShells,
                                       ExplosionSnapshot *explosions, int maxExplosions,
                                       BaseSnapshot *bases, int maxBases,
                                       PillSnapshot *pills, int maxPills,
                                       GameEvent *events, int maxEvents) {
    TransportUdpClientCtx *c;
    int count;
    (void)clientIdx; /* UDP client doesn't need this — server sends per-client data */
    if (ctx == NULL) return false;
    c = (TransportUdpClientCtx *)ctx;
    if (!c->hasSnapshot) return false;

    *hdr = c->snapshotHdr;
    count = c->snapshotHdr.tankCount;
    if (count > maxTanks) count = maxTanks;
    memcpy(tanks, c->snapshotTanks, count * sizeof(TankSnapshot));

    if (shells != NULL) {
        count = c->snapshotHdr.shellCount;
        if (count > maxShells) count = maxShells;
        memcpy(shells, c->snapshotShells, count * sizeof(ShellSnapshot));
    }

    if (explosions != NULL) {
        count = c->snapshotHdr.explosionCount;
        if (count > maxExplosions) count = maxExplosions;
        memcpy(explosions, c->snapshotExplosions, count * sizeof(ExplosionSnapshot));
    }

    if (bases != NULL) {
        count = c->snapshotHdr.baseCount;
        if (count > maxBases) count = maxBases;
        memcpy(bases, c->snapshotBases, count * sizeof(BaseSnapshot));
    }

    if (pills != NULL) {
        count = c->snapshotHdr.pillCount;
        if (count > maxPills) count = maxPills;
        memcpy(pills, c->snapshotPills, count * sizeof(PillSnapshot));
    }

    if (events != NULL) {
        count = c->snapshotHdr.reliableEventCount;
        if (count > maxEvents) count = maxEvents;
        memcpy(events, c->snapshotEvents, count * sizeof(GameEvent));
    }

    c->hasSnapshot = false;
    return true;
}

Transport transportUdpClientCreate(ClientSim *clientSim,
                                   const char *serverAddr,
                                   unsigned short serverPort,
                                   const char *playerName,
                                   const char *password,
                                   const char *wbnToken) {
    Transport t;
    TransportUdpClientCtx *c;
    struct hostent *he;

    memset(&t, 0, sizeof(t));
    c = (TransportUdpClientCtx *)malloc(sizeof(TransportUdpClientCtx));
    memset(c, 0, sizeof(TransportUdpClientCtx));
    c->clientSim = clientSim;

    bolo_net_init();

    c->sock = createUdpSocket();
    if (c->sock == INVALID_SOCKET) {
        c->joinState = UDP_CLIENT_ERROR;
        t.sendInput = udpClientSendInput;
        t.tick = udpClientTick;
        t.getSnapshot = udpClientGetSnapshotVtable;
        t.ctx = c;
        return t;
    }

    /* Resolve server address */
    memset(&c->serverAddr, 0, sizeof(c->serverAddr));
    c->serverAddr.sin_family = AF_INET;
    c->serverAddr.sin_port = htons(serverPort);
    c->serverAddr.sin_addr.s_addr = inet_addr(serverAddr);
    if (c->serverAddr.sin_addr.s_addr == INADDR_NONE) {
        he = gethostbyname(serverAddr);
        if (he != NULL) {
            memcpy(&c->serverAddr.sin_addr, he->h_addr_list[0], he->h_length);
        } else {
            c->joinState = UDP_CLIENT_ERROR;
            t.sendInput = udpClientSendInput;
            t.tick = udpClientTick;
            t.getSnapshot = udpClientGetSnapshotVtable;
            t.ctx = c;
            return t;
        }
    }

    /* Copy player name, password, and WBN token */
    memset(c->playerName, 0, PACKET_MAX_PLAYER_NAME);
    strncpy(c->playerName, playerName, PACKET_MAX_PLAYER_NAME - 1);
    memset(c->password, 0, MAP_STR_SIZE);
    if (password != NULL) {
        strncpy(c->password, password, MAP_STR_SIZE - 1);
    }
    memset(c->wbnToken, 0, WBN_TOKEN_WIRE_LEN);
    c->wbnReauthSent = FALSE;
    if (wbnToken != NULL) {
        strncpy(c->wbnToken, wbnToken, WBN_TOKEN_WIRE_LEN - 1);
    }

    c->joinState = UDP_CLIENT_JOINING;
    c->joinAttempts = 0;
    c->ticksSinceJoinSent = JOIN_RETRY_INTERVAL; /* Send immediately on first tick */
    c->outSequence = 1;

    c->lastSnapshotSeq = 0;
    c->hasSnapshot = false;
    c->reliableEventAck = 1;  /* First valid seq is 1 */
    c->localTick = 0;
    c->lastPingSentTick = 0;
    c->pingMs = 0;

    t.sendInput = udpClientSendInput;
    t.tick = udpClientTick;
    t.getSnapshot = udpClientGetSnapshotVtable;
    t.ctx = c;
    return t;
}

void transportUdpClientDestroy(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->sock != INVALID_SOCKET) {
        /* Send graceful quit packet to server before closing */
        if (c->joinState == UDP_CLIENT_CONNECTED) {
            uint8_t qbuf[PACKET_HEADER_SIZE];
            packHeader(qbuf, PACKET_QUIT, c->outSequence++);
            udpSendTo(c->sock, qbuf, PACKET_HEADER_SIZE, &c->serverAddr);
        }
        closesocket(c->sock);
    }
    if (c->mapDownloadBuf != NULL) {
        free(c->mapDownloadBuf);
    }
    if (c->mapChunkReceived != NULL) {
        free(c->mapChunkReceived);
    }
    free(c);
    t->ctx = NULL;
}

UdpClientJoinState transportUdpClientGetJoinState(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return UDP_CLIENT_ERROR;
    c = (TransportUdpClientCtx *)t->ctx;
    return c->joinState;
}

BYTE transportUdpClientGetPlayerNum(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return 0;
    c = (TransportUdpClientCtx *)t->ctx;
    return c->playerNum;
}

bool transportUdpClientGetSnapshot(Transport *t,
                                   SnapshotHeader *hdr,
                                   TankSnapshot *tanks,
                                   int maxTanks,
                                   ShellSnapshot *shellsOut,
                                   int maxShells,
                                   ExplosionSnapshot *explosionsOut,
                                   int maxExplosions,
                                   BaseSnapshot *basesOut,
                                   int maxBases,
                                   PillSnapshot *pillsOut,
                                   int maxPills,
                                   GameEvent *eventsOut,
                                   int maxEvents) {
    TransportUdpClientCtx *c;
    int count;
    if (t == NULL || t->ctx == NULL) return false;
    c = (TransportUdpClientCtx *)t->ctx;
    if (!c->hasSnapshot) return false;

    *hdr = c->snapshotHdr;
    count = c->snapshotHdr.tankCount;
    if (count > maxTanks) count = maxTanks;
    memcpy(tanks, c->snapshotTanks, count * sizeof(TankSnapshot));

    if (shellsOut != NULL) {
        count = c->snapshotHdr.shellCount;
        if (count > maxShells) count = maxShells;
        memcpy(shellsOut, c->snapshotShells, count * sizeof(ShellSnapshot));
    }

    if (explosionsOut != NULL) {
        count = c->snapshotHdr.explosionCount;
        if (count > maxExplosions) count = maxExplosions;
        memcpy(explosionsOut, c->snapshotExplosions, count * sizeof(ExplosionSnapshot));
    }

    if (basesOut != NULL) {
        count = c->snapshotHdr.baseCount;
        if (count > maxBases) count = maxBases;
        memcpy(basesOut, c->snapshotBases, count * sizeof(BaseSnapshot));
    }

    if (pillsOut != NULL) {
        count = c->snapshotHdr.pillCount;
        if (count > maxPills) count = maxPills;
        memcpy(pillsOut, c->snapshotPills, count * sizeof(PillSnapshot));
    }

    if (eventsOut != NULL) {
        count = c->snapshotHdr.reliableEventCount;
        if (count > maxEvents) count = maxEvents;
        memcpy(eventsOut, c->snapshotEvents, count * sizeof(GameEvent));
    }

    c->hasSnapshot = false;
    return true;
}

uint16_t transportUdpClientGetPing(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return 0;
    c = (TransportUdpClientCtx *)t->ctx;
    return c->pingMs;
}

const char *transportUdpClientGetJoinRejectReason(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return NULL;
    c = (TransportUdpClientCtx *)t->ctx;
    return c->joinRejectReason;
}

const BYTE *transportUdpClientGetMapData(Transport *t, int *outLen) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return NULL;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->mapDownloadBuf == NULL || c->joinState != UDP_CLIENT_CONNECTED) {
        return NULL;
    }
    if (outLen != NULL) {
        *outLen = (int)c->mapDownloadTotal;
    }
    return c->mapDownloadBuf;
}

void transportUdpClientGetGameSettings(Transport *t, gameType *game,
                                       bool *hiddenMines, int32_t *startDelay,
                                       int32_t *gameLen) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return;
    c = (TransportUdpClientCtx *)t->ctx;
    if (game != NULL) *game = c->serverGameType;
    if (hiddenMines != NULL) *hiddenMines = c->serverHiddenMines;
    if (startDelay != NULL) *startDelay = c->serverStartDelay;
    if (gameLen != NULL) *gameLen = c->serverGameLen;
}

/* Send a chat message to the server.
 * destPlayer: 0xFF = all players, else specific player number. */
void transportUdpClientSendChat(Transport *t, uint8_t destPlayer,
                                const char *message) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1 + 128];
    int msgLen;
    int len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (message == NULL || message[0] == '\0') return;

    msgLen = (int)strlen(message);
    if (msgLen > 128) msgLen = 128;

    packHeader(buf, PACKET_CHAT_MESSAGE, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = destPlayer;
    memcpy(buf + PACKET_HEADER_SIZE + 1, message, msgLen);
    len = PACKET_HEADER_SIZE + 1 + msgLen;
    udpSendTo(c->sock, buf, len, &c->serverAddr);
}

/* Send a name change request to the server. */
void transportUdpClientSendNameChange(Transport *t, const char *newName) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (newName == NULL || newName[0] == '\0') return;

    packHeader(buf, PACKET_NAME_CHANGE, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    memset(buf + PACKET_HEADER_SIZE + 1, 0, PACKET_MAX_PLAYER_NAME);
    strncpy((char *)(buf + PACKET_HEADER_SIZE + 1), newName,
            PACKET_MAX_PLAYER_NAME - 1);
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

/* Send an alliance request to another player via server.
 * Wire: [header 8] [fromPlayer 1] [toPlayer 1] */
void transportUdpClientSendAllianceRequest(Transport *t, uint8_t toPlayer) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 2];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_ALLIANCE_REQUEST, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    buf[PACKET_HEADER_SIZE + 1] = toPlayer;
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

/* Send an alliance accept to server.
 * Wire: [header 8] [fromPlayer 1] [toPlayer 1]
 * fromPlayer = us (the accepter), toPlayer = who requested */
void transportUdpClientSendAllianceAccept(Transport *t, uint8_t toPlayer) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 2];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_ALLIANCE_ACCEPT, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    buf[PACKET_HEADER_SIZE + 1] = toPlayer;
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

/* Send a leave alliance request to server.
 * Wire: [header 8] [playerNum 1] */
void transportUdpClientSendAllianceLeave(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_ALLIANCE_LEAVE, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

/* Send a lock toggle to the server.
 * Wire: [header 8] [allow 1] */
void transportUdpClientSendLockToggle(Transport *t, bool allow) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOCK_TOGGLE, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = allow ? 1 : 0;
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}


/* ---- Client lobby send functions ---- */

void transportUdpClientSendTeamSet(Transport *t, uint8_t teamNumber) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 2];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_TEAM_SET, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    buf[PACKET_HEADER_SIZE + 1] = teamNumber;
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

void transportUdpClientSendReady(Transport *t, bool ready) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 2];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_READY, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    buf[PACKET_HEADER_SIZE + 1] = ready ? 1 : 0;
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

void transportUdpClientSendAddBot(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_ADD_BOT, c->outSequence++);
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

void transportUdpClientSendRemoveBot(Transport *t, uint8_t playerNum) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_REMOVE_BOT, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = playerNum;
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

void transportUdpClientSendWbnReauth(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + WBN_TOKEN_WIRE_LEN];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (c->wbnToken[0] == '\0') return;

    packHeader(buf, PACKET_WBN_REAUTH, c->outSequence++);
    memcpy(buf + PACKET_HEADER_SIZE, c->wbnToken, WBN_TOKEN_WIRE_LEN);
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

/* ---- Client balance send functions ---- */

void transportUdpClientSendBalanceRequest(Transport *t, uint8_t teamSize) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_BALANCE_REQUEST, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = teamSize;
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

void transportUdpClientSendBalanceApply(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_BALANCE_APPLY, c->outSequence++);
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

void transportUdpClientSendBalanceDismiss(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_BALANCE_DISMISS, c->outSequence++);
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

void transportUdpClientSendMapSkipVote(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_MAP_SKIP_VOTE, c->outSequence++);
    udpSendTo(c->sock, buf, sizeof(buf), &c->serverAddr);
}

/* ================================================================
 * SERVER SIDE
 * ================================================================ */

/* Per-client map download tracking */
typedef struct {
    BYTE    *compressedMap;     /* Server's compressed map data (shared, do not free) */
    uint32_t mapSize;          /* Total compressed map size */
    uint16_t totalChunks;      /* Total number of chunks */
    bool    *chunkAcked;       /* Which chunks the client has acked */
    uint16_t chunksAcked;      /* Number of acked chunks */
    bool     downloadComplete; /* True when all chunks acked */
    uint32_t lastSendTick;     /* Last tick we sent chunks (for resend timing) */
} ClientMapDownload;

/* Server-side global state */
static struct {
    SOCKET sock;
    bool running;
    char password[MAP_STR_SIZE];
    BYTE maxPlayers;
    UdpServerClient clients[MAX_TANKS];
    uint32_t tickCount;

    /* Compressed map buffer for sending to joining clients */
    BYTE     compressedMap[MAP_DOWNLOAD_MAX_SIZE];
    uint32_t compressedMapSize;

    /* Per-client map download state */
    ClientMapDownload mapDownload[MAX_TANKS];

    /* Per-client reliable event queues */
    ClientEventQueue eventQueues[MAX_TANKS];

    /* Game lock — prevents new players from joining */
    bool gameLocked;           /* Server admin lock */
    bool clientLocked[MAX_TANKS]; /* Per-player lock votes */
} udpServer;

/* Forward declaration */
static bool serverClientsAllLocked(void);

/* Find client slot by address. Returns player index or -1. */
static int serverFindClient(const struct sockaddr_in *addr) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected &&
            udpServer.clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            udpServer.clients[i].addr.sin_port == addr->sin_port) {
            return i;
        }
    }
    return -1;
}

/* Data passed to the balance thread — snapshot of values needed for the
 * HTTP call so the thread doesn't read ServerSim without the mutex. */
typedef struct {
    ServerSim *sim;
    uint8_t    totalPlayers;
    uint8_t    teamSize;
} BalanceThreadData;

/* Background thread: calls WBN balance API (blocks on HTTP) then writes
 * results back under the game mutex so the timer can broadcast them. */
static int balanceThreadFunc(void *data) {
    BalanceThreadData *btd = (BalanceThreadData *)data;
    ServerSim *sim = btd->sim;

    /* This blocks on HTTP — runs outside the game mutex */
    winbolonetServerRequestBalance(btd->totalPlayers, btd->teamSize, &sim->balanceProposal);

    free(btd);

    /* If the server is shutting down, signal completion and exit without
     * acquiring the mutex (the main thread may have already torn it down). */
    if (SDL_GetAtomicInt(&sim->balanceProposal.shutdownFlag)) {
        sim->balanceProposal.requestInFlight = false;
        return 0;
    }

    /* Write results back under the game mutex */
    threadsWaitForMutex();
    sim->balanceProposal.requestInFlight = false;
    if (sim->balanceProposal.pending) {
        sim->balanceProposal.broadcastNeeded = true;
    }
    threadsReleaseMutex();
    return 0;
}

/* Find a free player slot. Returns index or -1. */
static int serverFindFreeSlot(void) {
    int i;
    BYTE limit = (udpServer.maxPlayers > 0) ? udpServer.maxPlayers : MAX_TANKS;
    for (i = 0; i < limit; i++) {
        if (!udpServer.clients[i].connected && !botManagerIsBot((BYTE)i)) {
            return i;
        }
    }
    return -1;
}

/* Send a join reject to a specific address */
static void serverSendJoinReject(const struct sockaddr_in *addr,
                                 const char *reason) {
    uint8_t buf[PACKET_HEADER_SIZE + 64];
    packHeader(buf, PACKET_JOIN_REJECT, 0);
    memset(buf + PACKET_HEADER_SIZE, 0, 64);
    strncpy((char *)(buf + PACKET_HEADER_SIZE), reason, 63);
    udpSendTo(udpServer.sock, buf, sizeof(buf), addr);
}

/* Send a player joined/left notification to all connected clients */
static void serverBroadcastPlayerEvent(uint8_t eventType, uint8_t playerNum,
                                       const char *playerName) {
    uint8_t buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 2];
    int i;
    int pktLen = PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 2;

    packHeader(buf, eventType, 0);
    buf[PACKET_HEADER_SIZE] = playerNum;
    memset(buf + PACKET_HEADER_SIZE + 1, 0, PACKET_MAX_PLAYER_NAME);
    if (playerName != NULL) {
        strncpy((char *)(buf + PACKET_HEADER_SIZE + 1), playerName,
                PACKET_MAX_PLAYER_NAME - 1);
    }
    /* Append country code */
    buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME] =
        (uint8_t)udpServer.clients[playerNum].countryCode[0];
    buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 1] =
        (uint8_t)udpServer.clients[playerNum].countryCode[1];

    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, pktLen, &udpServer.clients[i].addr);
        }
    }
}

/* Build and send the join accept packet with game settings and map size */
static void serverSendJoinAccept(int slot, ServerSim *sim,
                                 const struct sockaddr_in *addr) {
    uint8_t acceptBuf[PACKET_HEADER_SIZE + 19];
    int pos;

    packHeader(acceptBuf, PACKET_JOIN_ACCEPT,
               udpServer.clients[slot].outSequence++);
    pos = PACKET_HEADER_SIZE;
    acceptBuf[pos++] = (uint8_t)slot;
    packU32(acceptBuf + pos, sim->tick);
    pos += 4;
    acceptBuf[pos++] = (uint8_t)gameTypeGet(&sim->sim.game);
    acceptBuf[pos++] = sim->sim.hiddenMines ? 1 : 0;
    packU32(acceptBuf + pos, (uint32_t)sim->startDelay);
    pos += 4;
    packU32(acceptBuf + pos, (uint32_t)sim->gameLength);
    pos += 4;
    packU32(acceptBuf + pos, udpServer.compressedMapSize);
    pos += 4;

    udpSendTo(udpServer.sock, acceptBuf, pos, addr);
}

/* Send map chunks to a client that is downloading */
static void serverSendMapChunks(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    UdpServerClient *client = &udpServer.clients[slot];
    uint16_t i;

    if (dl->downloadComplete) return;

    for (i = 0; i < dl->totalChunks; i++) {
        uint8_t chunkBuf[PACKET_HEADER_SIZE + 4 + MAP_DOWNLOAD_CHUNK_SIZE];
        uint32_t offset;
        uint16_t chunkSize;
        int pktLen;

        if (dl->chunkAcked[i]) continue;

        offset = (uint32_t)i * MAP_DOWNLOAD_CHUNK_SIZE;
        chunkSize = (uint16_t)(dl->mapSize - offset);
        if (chunkSize > MAP_DOWNLOAD_CHUNK_SIZE) {
            chunkSize = MAP_DOWNLOAD_CHUNK_SIZE;
        }

        packHeader(chunkBuf, PACKET_MAP_DOWNLOAD, client->outSequence++);
        packU16(chunkBuf + PACKET_HEADER_SIZE, i);
        packU16(chunkBuf + PACKET_HEADER_SIZE + 2, chunkSize);
        memcpy(chunkBuf + PACKET_HEADER_SIZE + 4,
               udpServer.compressedMap + offset, chunkSize);
        pktLen = PACKET_HEADER_SIZE + 4 + chunkSize;

        udpSendTo(udpServer.sock, chunkBuf, pktLen, &client->addr);
    }

    dl->lastSendTick = udpServer.tickCount;
}

/* Initialize map download tracking for a client */
static void serverInitMapDownload(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];

    dl->compressedMap = udpServer.compressedMap;
    dl->mapSize = udpServer.compressedMapSize;
    dl->totalChunks = (uint16_t)((dl->mapSize + MAP_DOWNLOAD_CHUNK_SIZE - 1) / MAP_DOWNLOAD_CHUNK_SIZE);
    dl->chunksAcked = 0;
    dl->downloadComplete = FALSE;
    dl->lastSendTick = 0;

    if (dl->chunkAcked != NULL) {
        free(dl->chunkAcked);
    }
    dl->chunkAcked = (bool *)calloc(dl->totalChunks, sizeof(bool));
}

/* Clean up map download tracking for a client */
static void serverCleanupMapDownload(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    if (dl->chunkAcked != NULL) {
        free(dl->chunkAcked);
        dl->chunkAcked = NULL;
    }
    dl->downloadComplete = FALSE;
    dl->chunksAcked = 0;
}

/* Forward declarations for lobby broadcast helpers (defined after serverRecv) */
static void transportUdpServerSendLobbyStateToClient(ServerSim *sim, int clientIdx);
void transportUdpServerBroadcastLobbyUpdate(ServerSim *sim, BYTE playerNum);

/* Handle a join request from a new client */
static void serverHandleJoinRequest(const uint8_t *buf, int len,
                                    const struct sockaddr_in *fromAddr,
                                    ServerSim *sim) {
    int pos = PACKET_HEADER_SIZE;
    char name[PACKET_MAX_PLAYER_NAME];
    char pass[MAP_STR_SIZE];
    char wbnToken[WBN_TOKEN_WIRE_LEN];
    int slot;

    fprintf(stderr, "[UDP SERVER] Join request received, len=%d\n", len);
    if (len < pos + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3) {
        fprintf(stderr, "[UDP SERVER] Join request malformed (need %d, got %d)\n",
                pos + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3, len);
        return; /* Malformed */
    }

    /* Already connected from this address? */
    if (serverFindClient(fromAddr) >= 0) {
        /* Resend accept in case they missed it */
        slot = serverFindClient(fromAddr);
        serverSendJoinAccept(slot, sim, fromAddr);
        /* Resend map chunks if download not complete */
        if (!udpServer.mapDownload[slot].downloadComplete) {
            serverSendMapChunks(slot);
        }
        return;
    }

    memcpy(name, buf + pos, PACKET_MAX_PLAYER_NAME);
    name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    pos += PACKET_MAX_PLAYER_NAME;

    memcpy(pass, buf + pos, MAP_STR_SIZE);
    pass[MAP_STR_SIZE - 1] = '\0';
    pos += MAP_STR_SIZE;

    /* Skip version bytes */
    pos += 3;

    /* Read WBN token if present (backwards compatible — older clients won't send it) */
    memset(wbnToken, 0, WBN_TOKEN_WIRE_LEN);
    if (len >= pos + WBN_TOKEN_WIRE_LEN) {
        memcpy(wbnToken, buf + pos, WBN_TOKEN_WIRE_LEN);
        wbnToken[WBN_TOKEN_WIRE_LEN - 1] = '\0';
    }

    /* Check password */
    if (udpServer.password[0] != '\0') {
        if (strcmp(pass, udpServer.password) != 0) {
            char consoleMsg[128];
            snprintf(consoleMsg, sizeof(consoleMsg),
                     "Join rejected for '%s': Incorrect password", name);
            serverSimConsoleMessage(consoleMsg);
            serverSendJoinReject(fromAddr, "Incorrect password");
            return;
        }
    }

    /* Check game lock (server admin OR all clients voted to lock) */
    if (udpServer.gameLocked || serverClientsAllLocked()) {
        char consoleMsg[128];
        snprintf(consoleMsg, sizeof(consoleMsg),
                 "Join rejected for '%s': Game is locked", name);
        serverSimConsoleMessage(consoleMsg);
        serverSendJoinReject(fromAddr, "Game is locked");
        return;
    }

    /* Check name uniqueness */
    {
        int i;
        for (i = 0; i < MAX_TANKS; i++) {
            if (udpServer.clients[i].connected &&
                strcmp(udpServer.clients[i].playerName, name) == 0) {
                char consoleMsg[128];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "Join rejected for '%s': Name already in use", name);
                serverSimConsoleMessage(consoleMsg);
                serverSendJoinReject(fromAddr, "Name already in use");
                return;
            }
        }
    }

    /* Find free slot */
    slot = serverFindFreeSlot();
    if (slot < 0) {
        serverSimConsoleMessage("Join rejected: Server full");
        serverSendJoinReject(fromAddr, "Server full");
        return;
    }

    /* Compress current map state for the joining player */
    {
        int mapLen = serverSimGetCompressedMap(sim, udpServer.compressedMap);
        if (mapLen <= 0) {
            serverSendJoinReject(fromAddr, "Map serialization failed");
            return;
        }
        udpServer.compressedMapSize = (uint32_t)mapLen;
    }

    /* Accept the player */
    udpServer.clients[slot].connected = true;
    udpServer.clients[slot].addr = *fromAddr;
    udpServer.clients[slot].playerNum = (uint8_t)slot;
    strncpy(udpServer.clients[slot].playerName, name,
            PACKET_MAX_PLAYER_NAME - 1);
    udpServer.clients[slot].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    udpServer.clients[slot].lastReceivedTick = udpServer.tickCount;
    udpServer.clients[slot].outSequence = 1;
    udpServer.clients[slot].lastPingTime = udpServer.tickCount;
    udpServer.clients[slot].pingMs = 0;

    /* GeoIP country lookup from client IP */
    {
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &fromAddr->sin_addr, ipStr, sizeof(ipStr));
        if (!geoLookupCountry(ipStr, udpServer.clients[slot].countryCode)) {
            udpServer.clients[slot].countryCode[0] = '\0';
            udpServer.clients[slot].countryCode[1] = '\0';
        }
        udpServer.clients[slot].countryCode[2] = '\0';
    }

    /* Initialize reliable event queue for this client */
    udpServer.eventQueues[slot].nextSeq = 1;
    udpServer.eventQueues[slot].ackedSeq = 1;
    memset(udpServer.eventQueues[slot].buffer, 0, sizeof(udpServer.eventQueues[slot].buffer));

    /* Verify WBN token if provided */
    bool wbnVerified = false;
    bool wbnHasSteam = false;
    if (wbnToken[0] != '\0' && winbolonetIsRunning()) {
        char errorMsg[512];
        errorMsg[0] = '\0';
        if (winbolonetServerVerifyToken(wbnToken, (BYTE)slot, errorMsg, &wbnHasSteam)) {
            fprintf(stderr, "[UDP SERVER] Player '%s' verified with WinBolo.net\n", name);
            wbnVerified = true;
        } else {
            char rejectMsg[256];
            snprintf(rejectMsg, sizeof(rejectMsg),
                     "WinBolo.net verification failed: %s", errorMsg);
            fprintf(stderr, "[UDP SERVER] %s\n", rejectMsg);
            udpServer.clients[slot].connected = false;
            serverSendJoinReject(fromAddr, rejectMsg);
            return;
        }
    }

    /* Initialize player in the simulation */
    serverSimAddPlayer(sim, (BYTE)slot, udpServer.clients[slot].playerName);

    /* Set WBN/Steam participant flags if token was verified */
    if (wbnVerified) {
        playersSetWbnParticipant(&sim->sim.plyrs, (BYTE)slot, TRUE);
        playersSetSteamParticipant(&sim->sim.plyrs, (BYTE)slot, wbnHasSteam);
        SDL_Log("[WBN] Set player %d wbn=1 steam=%d", slot, wbnHasSteam ? 1 : 0);
    }

    /* Send accept with game settings and map size */
    {
        char consoleMsg[128];
        snprintf(consoleMsg, sizeof(consoleMsg),
                 "New Player '%s' accepted into game.", name);
        serverSimConsoleMessage(consoleMsg);
    }
    fprintf(stderr, "[UDP SERVER] Player '%s' assigned slot %d, mapSize=%u\n",
            name, slot, udpServer.compressedMapSize);
    serverSendJoinAccept(slot, sim, fromAddr);

    /* Notify all players about the new player */
    serverBroadcastPlayerEvent(PACKET_PLAYER_JOINED, (uint8_t)slot, name);
    winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                       (BYTE)slot, WINBOLO_NET_NO_PLAYER);

    /* Send lobby state or game start signal BEFORE map chunks so the
     * client enters the lobby immediately and downloads the map in the
     * background (with the lobby progress bar visible). */
    if (sim->state == serverStateLobby || sim->state == serverStateCountdown) {
        /* Send full lobby snapshot to the joining client */
        transportUdpServerSendLobbyStateToClient(sim, slot);
        /* Broadcast lobby update to existing clients about the new player */
        transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)slot);
        /* Dismiss any pending balance proposal — player composition changed */
        if (sim->balanceProposal.pending) {
            uint8_t zeros[MAX_TANKS];
            memset(zeros, 0, sizeof(zeros));
            memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
            transportUdpServerBroadcastBalanceProposal(sim, zeros);
        }
    } else if (sim->state == serverStateRunning) {
        /* No-lobby mode or mid-game join: send game start signal */
        uint8_t startBuf[PACKET_HEADER_SIZE];
        packHeader(startBuf, PACKET_GAME_START,
                   udpServer.clients[slot].outSequence++);
        udpSendTo(udpServer.sock, startBuf, sizeof(startBuf),
                  &udpServer.clients[slot].addr);
    }

    /* Initialize map download and send first batch of chunks */
    serverInitMapDownload(slot);
    fprintf(stderr, "[UDP SERVER] Sending %u map chunks to slot %d\n",
            udpServer.mapDownload[slot].totalChunks, slot);
    serverSendMapChunks(slot);

    /* Defer sending existing player list until after map download completes.
     * Sending now would be wiped by screenDestroy()/screenLoadCompressedMap()
     * on the client side when it finishes downloading the map. */
    udpServer.clients[slot].needsPlayerList = true;
}

/* Handle input packet from a connected client */
static void serverHandleInput(const uint8_t *buf, int len,
                              const struct sockaddr_in *fromAddr,
                              ServerSim *sim) {
    int clientIdx;
    int pos = PACKET_HEADER_SIZE;
    uint8_t inputCount;
    int i;

    clientIdx = serverFindClient(fromAddr);
    if (clientIdx < 0) return; /* Unknown client */

    udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;

    /* Only process inputs during running state — silently discard otherwise */
    if (sim->state != serverStateRunning) {
        return;
    }

    if (len < pos + 1) return;
    inputCount = buf[pos++];
    if (inputCount > INPUT_REDUNDANCY_COUNT) inputCount = INPUT_REDUNDANCY_COUNT;

    /* Process each input — the last one is the newest.
     * Apply only inputs newer than what we've already processed. */
    for (i = 0; i < inputCount; i++) {
        InputPacket pkt;
        if (len < pos + INPUT_PACKET_WIRE_SIZE) break;
        unpackInputPacket(buf + pos, &pkt);
        pos += INPUT_PACKET_WIRE_SIZE;

        /* Override playerNum to prevent spoofing */
        pkt.playerNum = (uint8_t)clientIdx;

        /* Advance reliable event ACK from this client */
        if (pkt.eventAck > udpServer.eventQueues[clientIdx].ackedSeq) {
            udpServer.eventQueues[clientIdx].ackedSeq = pkt.eventAck;
        }

        /* Only apply if this is a newer input than what we last processed */
        if (pkt.tick > sim->lastProcessedInput[clientIdx]) {
            if (udpServer.clients[clientIdx].inputsThisTick >= 4) break;
            serverSimApplyInput(sim, &pkt);
            udpServer.clients[clientIdx].inputsThisTick++;
        }
    }
}

/* Handle ping from client — respond with pong */
static void serverHandlePing(const uint8_t *buf, int len,
                              const struct sockaddr_in *fromAddr) {
    uint8_t pongBuf[PACKET_HEADER_SIZE + 8];
    uint32_t clientTime;

    if (len < PACKET_HEADER_SIZE + 8) return;

    clientTime = unpackU32(buf + PACKET_HEADER_SIZE);

    /* Client embeds its last measured pingMs in the second uint32 */
    {
        int clientIdx = serverFindClient(fromAddr);
        if (clientIdx >= 0) {
            udpServer.clients[clientIdx].pingMs =
                (uint16_t)unpackU32(buf + PACKET_HEADER_SIZE + 4);
            udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;
        }
    }

    packHeader(pongBuf, PACKET_PONG, 0);
    packU32(pongBuf + PACKET_HEADER_SIZE, clientTime);
    packU32(pongBuf + PACKET_HEADER_SIZE + 4, udpServer.tickCount);
    udpSendTo(udpServer.sock, pongBuf, sizeof(pongBuf), fromAddr);
}

/* Build and send a snapshot to one client.
 * Uses serverSimBuildSnapshot() for all game state, then serializes
 * and appends reliable events from the per-client queue. */
static void serverSendSnapshot(ServerSim *sim, int clientIdx) {
    uint8_t buf[2048];
    int pos;
    int i;
    int countsPos;
    int reliableEventCount = 0;
    uint32_t reliableBaseSeq = 0;
    SnapshotHeader hdr;
    TankSnapshot tankSnaps[MAX_TANKS];
    ShellSnapshot shellSnaps[MAX_SNAPSHOT_SHELLS];
    ExplosionSnapshot explSnaps[MAX_SNAPSHOT_EXPLOSIONS];
    BaseSnapshot baseSnaps[MAX_SNAPSHOT_BASES];
    PillSnapshot pillSnaps[MAX_SNAPSHOT_PILLS];
    GameEvent eventSnaps[MAX_SNAPSHOT_EVENTS];
    ClientEventQueue *evQ = &udpServer.eventQueues[clientIdx];
    UdpServerClient *client = &udpServer.clients[clientIdx];

    /* Build snapshot from sim state (same code as local transport) */
    serverSimBuildSnapshot(sim, (BYTE)clientIdx, &hdr,
                           tankSnaps, MAX_TANKS,
                           shellSnaps, MAX_SNAPSHOT_SHELLS,
                           explSnaps, MAX_SNAPSHOT_EXPLOSIONS,
                           baseSnaps, MAX_SNAPSHOT_BASES,
                           pillSnaps, MAX_SNAPSHOT_PILLS,
                           eventSnaps, MAX_SNAPSHOT_EVENTS);

    /* Packet header */
    packHeader(buf, PACKET_STATE_SNAPSHOT, client->outSequence++);
    pos = PACKET_HEADER_SIZE;

    /* Snapshot header — we'll fill in counts after packing data.
     * Format: serverTick(4) + lastProcessedInput(4) + tankCount(1)
     * + shellCount(1) + explosionCount(1) + baseCount(1) + pillCount(1)
     * + reliableEventCount(1) + reliableBaseSeq(4) + mapChecksum(2) = 20 bytes */
    packU32(buf + pos, hdr.serverTick);
    pos += 4;
    packU32(buf + pos, hdr.lastProcessedInput);
    pos += 4;
    countsPos = pos;
    pos += 12; /* 6 count bytes + 4 byte reliableBaseSeq + 2 byte mapChecksum */

    /* Pack tank snapshots */
    for (i = 0; i < hdr.tankCount; i++) {
        if (pos + TANK_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.tankCount = (uint8_t)i;
            break;
        }
        pos += packTankSnapshot(buf + pos, &tankSnaps[i]);
    }

    /* Pack shell snapshots */
    for (i = 0; i < hdr.shellCount; i++) {
        if (pos + SHELL_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.shellCount = (uint8_t)i;
            break;
        }
        pos += packShellSnapshot(buf + pos, &shellSnaps[i]);
    }

    /* Pack explosion snapshots */
    for (i = 0; i < hdr.explosionCount; i++) {
        if (pos + EXPLOSION_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.explosionCount = (uint8_t)i;
            break;
        }
        pos += packExplosionSnapshot(buf + pos, &explSnaps[i]);
    }

    /* Pack base snapshots */
    for (i = 0; i < hdr.baseCount; i++) {
        if (pos + BASE_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.baseCount = (uint8_t)i;
            break;
        }
        pos += packBaseSnapshot(buf + pos, &baseSnaps[i]);
    }

    /* Pack pill snapshots */
    for (i = 0; i < hdr.pillCount; i++) {
        if (pos + PILL_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.pillCount = (uint8_t)i;
            break;
        }
        pos += packPillSnapshot(buf + pos, &pillSnaps[i]);
    }

    /* Pack reliable events from per-client queue (all unacked events).
     * These are transport-specific — serverSimBuildSnapshot() produces
     * per-tick events, but the reliable queue handles retransmission. */
    reliableBaseSeq = evQ->ackedSeq;
    {
        uint32_t seq;
        for (seq = evQ->ackedSeq; seq < evQ->nextSeq; seq++) {
            uint32_t idx = seq % RELIABLE_EVENT_BUFFER_SIZE;
            if (pos + GAME_EVENT_MAX_WIRE_SIZE > (int)sizeof(buf)) break;
            if (evQ->buffer[idx].seq != seq) break; /* Buffer wrapped — stop */
            pos += packGameEvent(buf + pos, &evQ->buffer[idx].event);
            reliableEventCount++;
        }
    }

    /* Fill in counts */
    buf[countsPos]     = hdr.tankCount;
    buf[countsPos + 1] = hdr.shellCount;
    buf[countsPos + 2] = hdr.explosionCount;
    buf[countsPos + 3] = hdr.baseCount;
    buf[countsPos + 4] = hdr.pillCount;
    buf[countsPos + 5] = (uint8_t)reliableEventCount;
    packU32(buf + countsPos + 6, reliableBaseSeq);
    packU16(buf + countsPos + 10, hdr.mapChecksum);

    udpSendTo(udpServer.sock, buf, pos, &client->addr);
    if (sim->tick % 50 == 0) {
        fprintf(stderr, "[UDP SERVER] Send SNAPSHOT to slot %d: tick=%u tanks=%u shells=%u expl=%u bases=%u pills=%u events=%u(ack=%u next=%u) len=%d\n",
                clientIdx, sim->tick, (unsigned)hdr.tankCount, (unsigned)hdr.shellCount,
                (unsigned)hdr.explosionCount, (unsigned)hdr.baseCount,
                (unsigned)hdr.pillCount, reliableEventCount,
                evQ->ackedSeq, evQ->nextSeq, pos);
    }
}

/* Disconnect a player by index.
 * graceful=TRUE means the player chose to quit (sends "is quitting" message).
 * graceful=FALSE means timeout or kick (no quit message). */
/* Check if all connected clients have voted to lock.
 * Returns TRUE only if there is at least one client and all are locked. */
static bool serverClientsAllLocked(void) {
    int i;
    bool anyConnected = false;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            anyConnected = true;
            if (!udpServer.clientLocked[i]) return false;
        }
    }
    return anyConnected;
}

static void serverDisconnectClient(ServerSim *sim, int idx, bool graceful) {
    char msg[128];
    if (!udpServer.clients[idx].connected) return;

    if (graceful) {
        snprintf(msg, sizeof(msg), "%s is quitting.",
                 udpServer.clients[idx].playerName);
        winbolonetAddEvent(WINBOLO_NET_EVENT_QUITTING, TRUE,
                           (BYTE)idx, WINBOLO_NET_NO_PLAYER);
    } else {
        snprintf(msg, sizeof(msg), "%s timed out.",
                 udpServer.clients[idx].playerName);
    }
    fprintf(stderr, "[UDP SERVER] %s\n", msg);
    serverSimConsoleMessage(msg);

    /* Notify WinBolo.net that the player is leaving (must happen before
     * clearing the slot so the player key is still valid) */
    winboloNetClientLeaveGame((BYTE)idx,
                              serverSimGetNumPlayers(sim),
                              serverSimGetNumNeutralBases(sim),
                              serverSimGetNumNeutralPills(sim));

    serverBroadcastPlayerEvent(PACKET_PLAYER_LEFT,
                               (uint8_t)idx,
                               udpServer.clients[idx].playerName);
    udpServer.clients[idx].connected = false;
    memset(udpServer.clients[idx].playerName, 0, PACKET_MAX_PLAYER_NAME);
    udpServer.clientLocked[idx] = false;
}

/* Send a server-originated message to all connected clients via chat broadcast.
 * Uses fromPlayer=0xFF to indicate it's from the server. */
static void serverSendServerMessage(const char *message) {
    uint8_t outBuf[PACKET_HEADER_SIZE + 2 + 200];
    int msgLen = (int)strlen(message);
    int outLen;
    int j;

    if (msgLen > 200) msgLen = 200;
    packHeader(outBuf, PACKET_CHAT_BROADCAST, 0);
    outBuf[PACKET_HEADER_SIZE] = 0xFF;     /* fromPlayer = server */
    outBuf[PACKET_HEADER_SIZE + 1] = 0xFF; /* destPlayer = all */
    memcpy(outBuf + PACKET_HEADER_SIZE + 2, message, msgLen);
    outLen = PACKET_HEADER_SIZE + 2 + msgLen;

    for (j = 0; j < MAX_TANKS; j++) {
        if (udpServer.clients[j].connected) {
            udpSendTo(udpServer.sock, outBuf, outLen,
                      &udpServer.clients[j].addr);
        }
    }
}

void transportUdpServerKickPlayer(ServerSim *sim, const char *playerName) {
    int i;
    char msg[128];

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        if (strcasecmp(udpServer.clients[i].playerName, playerName) == 0) {
            snprintf(msg, sizeof(msg), "%s has been server kicked.",
                     udpServer.clients[i].playerName);
            fprintf(stderr, "[UDP SERVER] %s\n", msg);
            serverSimConsoleMessage(msg);
            /* Send kick message to all clients (including the kicked player) */
            serverSendServerMessage(msg);
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (sim->lobbyEnabled &&
                (sim->state == serverStateLobby || sim->state == serverStateCountdown)) {
                transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)i);
            }
            return;
        }
    }
    serverSimConsoleMessage("Player not found.");
}

bool transportUdpServerCreate(unsigned short port,
                              const char *addrToUse,
                              ServerSim *sim,
                              const char *password,
                              BYTE maxPlayers) {
    struct sockaddr_in bindAddr;
    int i;

    (void)sim; /* Used later during tick */

    bolo_net_init();
    memset(&udpServer, 0, sizeof(udpServer));

    udpServer.sock = createUdpSocket();
    if (udpServer.sock == INVALID_SOCKET) {
        return false;
    }

    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    if (addrToUse != NULL && addrToUse[0] != '\0') {
        bindAddr.sin_addr.s_addr = inet_addr(addrToUse);
    }
    bindAddr.sin_port = htons(port);

    if (bind(udpServer.sock, (struct sockaddr *)&bindAddr,
             sizeof(bindAddr)) == SOCKET_ERROR) {
        fprintf(stderr, "[UDP SERVER] bind() failed on port %u\n", port);
        closesocket(udpServer.sock);
        udpServer.sock = INVALID_SOCKET;
        return false;
    }

    memset(udpServer.password, 0, MAP_STR_SIZE);
    if (password != NULL) {
        strncpy(udpServer.password, password, MAP_STR_SIZE - 1);
    }

    udpServer.maxPlayers = maxPlayers;
    udpServer.running = true;
    udpServer.tickCount = 0;
    sim->serverPort = port;
    fprintf(stderr, "[UDP SERVER] Created, bound to port %u\n", port);
    udpServer.compressedMapSize = 0;

    for (i = 0; i < MAX_TANKS; i++) {
        udpServer.clients[i].connected = false;
        memset(&udpServer.mapDownload[i], 0, sizeof(ClientMapDownload));
    }

    return true;
}

void transportUdpServerDestroy(void) {
    int i;

    /* Broadcast shutdown packet to all connected clients before closing */
    if (udpServer.sock != INVALID_SOCKET) {
        uint8_t shutdownBuf[PACKET_HEADER_SIZE];
        packHeader(shutdownBuf, PACKET_SERVER_SHUTDOWN, 0);
        for (i = 0; i < MAX_TANKS; i++) {
            if (udpServer.clients[i].connected) {
                udpSendTo(udpServer.sock, shutdownBuf, PACKET_HEADER_SIZE,
                          &udpServer.clients[i].addr);
            }
        }
        closesocket(udpServer.sock);
        udpServer.sock = INVALID_SOCKET;
    }
    for (i = 0; i < MAX_TANKS; i++) {
        udpServer.clients[i].connected = false;
        serverCleanupMapDownload(i);
    }
    udpServer.running = false;
}

/* Handle an old-protocol info request (server browser compatibility).
 * Builds a 76-byte INFO_PACKET response from the current sim state. */
static void serverHandleInfoRequest(const struct sockaddr_in *fromAddr,
                                    ServerSim *sim) {
    INFO_PACKET pkt;
    int i;
    BYTE numPlayers = 0;
    char consoleMsg[256];

    memset(&pkt, 0, sizeof(pkt));

    /* Header */
    memcpy(pkt.h.signature, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE);
    pkt.h.versionMajor = BOLO_VERSION_MAJOR;
    pkt.h.versionMinor = BOLO_VERSION_MINOR;
    pkt.h.versionRevision = BOLO_VERSION_REVISION;
    pkt.h.type = BOLOPACKET_INFORESPONSE;

    /* Game ID — address zeroed (browser uses UDP source), port and timestamp set.
     * Tracker reads port raw for v1.1.8 (only ntohs for v1.1.1-3).
     * start_time is the only field the tracker byte-swaps on read. */
    pkt.gameid.serveraddress.s_addr = 0;
    pkt.gameid.serverport = sim->serverPort;
    pkt.gameid.start_time = htonl(sim->timeCreated);

    /* Map name as Pascal string */
    utilCtoPString(sim->mapName, pkt.mapname);

    /* Game settings */
    pkt.gametype = (BYTE)sim->sim.game;
    pkt.allow_mines = sim->sim.hiddenMines ? HIDDEN_MINES : ALL_MINES_VISIBLE;
    pkt.allow_AI = 0;  /* AI type not tracked in new sim — report as none */
    pkt.spare1 = 0;
    pkt.start_delay = sim->startDelay;
    pkt.time_limit = sim->gameLength;

    /* Count connected players */
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) numPlayers++;
    }
    pkt.num_players = numPlayers;

    /* Neutral pills and bases */
    pkt.free_pills = pillsGetNumNeutral(&sim->sim.pb);
    pkt.free_bases = basesGetNumNeutral(&sim->sim.bs);

    pkt.has_password = udpServer.password[0] != '\0' ? 1 : 0;
    pkt.spare2 = 0;

    udpSendTo(udpServer.sock, (uint8_t *)&pkt, sizeof(pkt), fromAddr);

    {
        struct in_addr addrCopy = fromAddr->sin_addr;
        snprintf(consoleMsg, sizeof(consoleMsg), "Info packet request from %s",
                 inet_ntoa(addrCopy));
    }
    serverSimConsoleMessage(consoleMsg);
}

/* Check if a packet is an old-protocol info request.
 * Old protocol: 8-byte BOLOHEADER with "Bolo" signature + version + type. */
static bool isOldProtocolInfoRequest(const uint8_t *buf, int len) {
    return len == BOLOPACKET_REQUEST_SIZE &&
           memcmp(buf, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE) == 0 &&
           buf[BOLO_VERSION_MAJORPOS] == BOLO_VERSION_MAJOR &&
           buf[BOLO_VERSION_MINORPOS] == BOLO_VERSION_MINOR &&
           buf[BOLO_VERSION_REVISIONPOS] == BOLO_VERSION_REVISION &&
           buf[BOLOPACKET_REQUEST_TYPEPOS] == BOLOPACKET_INFOREQUEST;
}

/* ---- Lobby broadcast helpers ---- */

/* Lobby state wire format:
 *   [header 8] [serverState 1]
 *   For each of 16 slots:
 *     [connected 1] [playerName 32] [teamNumber 1] [ready 1] [isBot 1]
 *     [pingMs 2 (big-endian)] [countryCode 2] [wbn 1] [steam 1]
 *   Total per slot: 42 bytes.
 *   Game settings tail:
 *     [mapName 36] [gameType 1] [hiddenMines 1] [aiType 1]
 *     [gameLength 4 (big-endian)] [pillCount 1] [baseCount 1] [startCount 1]
 *   Total payload: 1 + 16*42 + 46 = 719 bytes.
 */

static void serverBuildLobbyStatePayload(ServerSim *sim, uint8_t *buf) {
    int pos = 0;
    int i;
    buf[pos++] = (uint8_t)sim->state;
    for (i = 0; i < MAX_TANKS; i++) {
        buf[pos++] = sim->playerConnected[i] ? 1 : 0;
        memset(buf + pos, 0, PACKET_MAX_PLAYER_NAME);
        if (sim->playerConnected[i]) {
            strncpy((char *)(buf + pos), udpServer.clients[i].playerName,
                    PACKET_MAX_PLAYER_NAME - 1);
        }
        pos += PACKET_MAX_PLAYER_NAME;
        buf[pos++] = sim->lobbyPlayers[i].teamNumber;
        buf[pos++] = sim->lobbyPlayers[i].ready ? 1 : 0;
        buf[pos++] = sim->lobbyPlayers[i].isBot ? 1 : 0;
        /* Ping (big-endian) */
        buf[pos++] = (uint8_t)(udpServer.clients[i].pingMs >> 8);
        buf[pos++] = (uint8_t)(udpServer.clients[i].pingMs & 0xFF);
        /* Country code (2 chars, or zeros if unknown) */
        buf[pos++] = (uint8_t)udpServer.clients[i].countryCode[0];
        buf[pos++] = (uint8_t)udpServer.clients[i].countryCode[1];
        /* WBN/Steam participant flags */
        buf[pos++] = playersGetWbnParticipant(&sim->sim.plyrs, (BYTE)i) ? 1 : 0;
        buf[pos++] = playersGetSteamParticipant(&sim->sim.plyrs, (BYTE)i) ? 1 : 0;
    }
    /* Game settings tail */
    memset(buf + pos, 0, MAP_STR_SIZE);
    strncpy((char *)(buf + pos), sim->mapName, MAP_STR_SIZE - 1);
    pos += MAP_STR_SIZE;
    buf[pos++] = (uint8_t)gameTypeGet(&sim->sim.game);
    buf[pos++] = sim->sim.hiddenMines ? 1 : 0;
    buf[pos++] = (uint8_t)sim->botAiType;
    packU32(buf + pos, (uint32_t)sim->gameLength);
    pos += 4;
    buf[pos++] = pillsGetNumPills(&sim->sim.pb);
    buf[pos++] = basesGetNumBases(&sim->sim.bs);
    buf[pos++] = startsGetNumStarts(&sim->sim.ss);
    buf[pos++] = sim->mapDirCount > 1 ? 1 : 0;
}

void transportUdpServerBroadcastLobbyState(ServerSim *sim) {
    uint8_t buf[PACKET_HEADER_SIZE + LOBBY_STATE_PAYLOAD];
    int i;
    packHeader(buf, PACKET_LOBBY_STATE, 0);
    serverBuildLobbyStatePayload(sim, buf + PACKET_HEADER_SIZE);
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
        }
    }
}

static void transportUdpServerSendLobbyStateToClient(ServerSim *sim, int clientIdx) {
    uint8_t buf[PACKET_HEADER_SIZE + LOBBY_STATE_PAYLOAD];
    packHeader(buf, PACKET_LOBBY_STATE, udpServer.clients[clientIdx].outSequence++);
    serverBuildLobbyStatePayload(sim, buf + PACKET_HEADER_SIZE);
    udpSendTo(udpServer.sock, buf, sizeof(buf), &udpServer.clients[clientIdx].addr);
}

/* Lobby update wire format:
 *   [header 8] [playerNum 1] [connected 1] [playerName 32]
 *   [teamNumber 1] [ready 1] [isBot 1] [pingMs 2] [countryCode 2]
 *   [wbn 1] [steam 1]
 */
#define LOBBY_UPDATE_PAYLOAD (1 + LOBBY_SLOT_WIRE_SIZE)

void transportUdpServerBroadcastLobbyUpdate(ServerSim *sim, BYTE playerNum) {
    uint8_t buf[PACKET_HEADER_SIZE + LOBBY_UPDATE_PAYLOAD];
    int pos = PACKET_HEADER_SIZE;
    int i;

    packHeader(buf, PACKET_LOBBY_UPDATE, 0);
    buf[pos++] = playerNum;
    buf[pos++] = sim->playerConnected[playerNum] ? 1 : 0;
    memset(buf + pos, 0, PACKET_MAX_PLAYER_NAME);
    if (sim->playerConnected[playerNum]) {
        strncpy((char *)(buf + pos), udpServer.clients[playerNum].playerName,
                PACKET_MAX_PLAYER_NAME - 1);
    }
    pos += PACKET_MAX_PLAYER_NAME;
    buf[pos++] = sim->lobbyPlayers[playerNum].teamNumber;
    buf[pos++] = sim->lobbyPlayers[playerNum].ready ? 1 : 0;
    buf[pos++] = sim->lobbyPlayers[playerNum].isBot ? 1 : 0;
    buf[pos++] = (uint8_t)(udpServer.clients[playerNum].pingMs >> 8);
    buf[pos++] = (uint8_t)(udpServer.clients[playerNum].pingMs & 0xFF);
    buf[pos++] = (uint8_t)udpServer.clients[playerNum].countryCode[0];
    buf[pos++] = (uint8_t)udpServer.clients[playerNum].countryCode[1];
    buf[pos++] = playersGetWbnParticipant(&sim->sim.plyrs, playerNum) ? 1 : 0;
    buf[pos++] = playersGetSteamParticipant(&sim->sim.plyrs, playerNum) ? 1 : 0;

    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, pos, &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerBroadcastCountdown(ServerSim *sim, uint8_t secondsRemaining) {
    uint8_t buf[PACKET_HEADER_SIZE + 1];
    int i;
    packHeader(buf, PACKET_COUNTDOWN, 0);
    buf[PACKET_HEADER_SIZE] = secondsRemaining;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerBroadcastGameStart(ServerSim *sim) {
    uint8_t buf[PACKET_HEADER_SIZE];
    int i;
    (void)sim;
    packHeader(buf, PACKET_GAME_START, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
            /* Clients reload the map on game start which wipes their player
               data.  Re-send the player list so names are restored. */
            udpServer.clients[i].needsPlayerList = true;
            /* Ensure map download is considered complete so snapshots
             * are sent during the game even if a final chunk ack was lost. */
            udpServer.mapDownload[i].downloadComplete = TRUE;
        }
        /* Reset reliable event queue — stale events from the previous game
         * must not be resent after clients load the fresh map. */
        udpServer.eventQueues[i].nextSeq = 1;
        udpServer.eventQueues[i].ackedSeq = 1;
    }
}

void transportUdpServerBroadcastGameOver(ServerSim *sim) {
    uint8_t buf[PACKET_HEADER_SIZE];
    int i;
    (void)sim;
    packHeader(buf, PACKET_GAME_OVER, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerNotifyMapChange(ServerSim *sim) {
    int i;
    int mapLen;
    uint8_t notifyBuf[PACKET_HEADER_SIZE];

    /* 1. Refresh the server's compressed map from the sim */
    mapLen = serverSimGetCompressedMap(sim, udpServer.compressedMap);
    if (mapLen <= 0) {
        fprintf(stderr, "[UDP SERVER] Map change: failed to compress new map\n");
        return;
    }
    udpServer.compressedMapSize = (uint32_t)mapLen;

    /* 2. Send PACKET_LOBBY_MAP_CHANGE to all connected clients */
    packHeader(notifyBuf, PACKET_LOBBY_MAP_CHANGE, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        udpSendTo(udpServer.sock, notifyBuf, sizeof(notifyBuf),
                  &udpServer.clients[i].addr);

        /* 3. Re-send join accept so client gets the new map size */
        serverSendJoinAccept(i, sim, &udpServer.clients[i].addr);

        /* 4. Reset map download tracking and start sending new chunks */
        serverInitMapDownload(i);
    }

    fprintf(stderr, "[UDP SERVER] Map change broadcast: %u bytes compressed map\n",
            udpServer.compressedMapSize);
}

void transportUdpServerBroadcastBalanceProposal(ServerSim *sim, uint8_t teamForSlot[MAX_TANKS]) {
    uint8_t buf[PACKET_HEADER_SIZE + MAX_TANKS];
    int i;
    (void)sim;
    packHeader(buf, PACKET_BALANCE_PROPOSAL, 0);
    memcpy(buf + PACKET_HEADER_SIZE, teamForSlot, MAX_TANKS);
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerBroadcastMapSkipState(ServerSim *sim) {
    uint8_t buf[PACKET_HEADER_SIZE + MAX_TANKS];
    int i;
    packHeader(buf, PACKET_MAP_SKIP_STATE, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        buf[PACKET_HEADER_SIZE + i] = sim->mapSkipVotes[i] ? 1 : 0;
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerSetBotName(BYTE playerNum, const char *name) {
    if (playerNum >= MAX_TANKS) return;
    strncpy(udpServer.clients[playerNum].playerName, name,
            PACKET_MAX_PLAYER_NAME - 1);
    udpServer.clients[playerNum].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
}

const char *transportUdpServerGetPlayerName(BYTE playerNum) {
    if (playerNum >= MAX_TANKS || !udpServer.clients[playerNum].connected) {
        return NULL;
    }
    return udpServer.clients[playerNum].playerName;
}

/* Send an INFO_RESPONSE packet to the tracker so the game is listed. */
void transportUdpServerSendTrackerUpdate(ServerSim *sim,
                                         const char *trackerAddr,
                                         unsigned short trackerPort) {
    INFO_PACKET pkt;
    struct sockaddr_in dest;
    struct hostent *he;
    int i;
    BYTE numPlayers = 0;

    he = gethostbyname(trackerAddr);
    if (he == NULL) {
        fprintf(stderr, "[TRACKER] Failed to resolve %s\n", trackerAddr);
        return;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    memcpy(&dest.sin_addr, he->h_addr_list[0], he->h_length);
    dest.sin_port = htons(trackerPort);

    memset(&pkt, 0, sizeof(pkt));

    memcpy(pkt.h.signature, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE);
    pkt.h.versionMajor = BOLO_VERSION_MAJOR;
    pkt.h.versionMinor = BOLO_VERSION_MINOR;
    pkt.h.versionRevision = BOLO_VERSION_REVISION;
    pkt.h.type = BOLOPACKET_INFORESPONSE;

    pkt.gameid.serveraddress.s_addr = 0;
    pkt.gameid.serverport = sim->serverPort;
    pkt.gameid.start_time = htonl(sim->timeCreated);

    utilCtoPString(sim->mapName, pkt.mapname);

    pkt.gametype = (BYTE)sim->sim.game;
    pkt.allow_mines = sim->sim.hiddenMines ? HIDDEN_MINES : ALL_MINES_VISIBLE;
    pkt.allow_AI = 0;
    pkt.spare1 = 0;
    pkt.start_delay = sim->startDelay;
    pkt.time_limit = sim->gameLength;

    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) numPlayers++;
    }
    pkt.num_players = numPlayers;
    pkt.free_pills = pillsGetNumNeutral(&sim->sim.pb);
    pkt.free_bases = basesGetNumNeutral(&sim->sim.bs);
    pkt.has_password = udpServer.password[0] != '\0' ? 1 : 0;
    pkt.spare2 = 0;

    sendto(udpServer.sock, (const char *)&pkt, sizeof(pkt), 0,
           (const struct sockaddr *)&dest, sizeof(dest));
}

/* Receive all pending packets from clients */
void transportUdpServerRecv(ServerSim *sim) {
    uint8_t buf[UDP_MAX_PAYLOAD];
    struct sockaddr_in fromAddr;
    int len;
    uint8_t pktType;

    int c;
    if (!udpServer.running) return;

    for (c = 0; c < MAX_TANKS; c++) {
        udpServer.clients[c].inputsThisTick = 0;
    }

    udpServer.tickCount++;

#if SIM_LATENCY_SERVER_MS > 0 && !defined(HAVE_SCREEN_C)
    while ((len = udpRecvFromDelayed(udpServer.sock, buf, sizeof(buf), &fromAddr,
            SIM_LATENCY_SERVER_MS, serverDelayQueue,
            &serverDelayHead, &serverDelayCount)) > 0) {
#else
    while ((len = udpRecvFrom(udpServer.sock, buf, sizeof(buf), &fromAddr)) > 0) {
#endif
        /* Check for old-protocol info request before new-protocol handling */
        if (isOldProtocolInfoRequest(buf, len)) {
            serverHandleInfoRequest(&fromAddr, sim);
            continue;
        }

        pktType = getPacketType(buf, len);
        if (pktType != PACKET_INPUT && pktType != PACKET_MAP_ACK) {
            fprintf(stderr, "[UDP SERVER] Recv %s (%u) len=%d from %s:%u\n",
                    packetTypeName(pktType), pktType, len,
                    inet_ntoa(fromAddr.sin_addr), ntohs(fromAddr.sin_port));
        }
        switch (pktType) {
        case PACKET_JOIN_REQUEST:
            serverHandleJoinRequest(buf, len, &fromAddr, sim);
            break;
        case PACKET_INPUT:
            serverHandleInput(buf, len, &fromAddr, sim);
            break;
        case PACKET_PING:
            serverHandlePing(buf, len, &fromAddr);
            break;
        case PACKET_CHAT_MESSAGE: {
            /* Chat message format:
             *   [header 8] [destPlayer 1] [message up to 128] */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && len > PACKET_HEADER_SIZE + 1) {
                uint8_t destPlayer = buf[PACKET_HEADER_SIZE];
                int msgLen = len - PACKET_HEADER_SIZE - 1;
                if (msgLen > 128) msgLen = 128;

                /* Build broadcast packet:
                 *   [header 8] [fromPlayer 1] [destPlayer 1] [message] */
                {
                    uint8_t outBuf[PACKET_HEADER_SIZE + 2 + 128];
                    int outLen;
                    packHeader(outBuf, PACKET_CHAT_BROADCAST, 0);
                    outBuf[PACKET_HEADER_SIZE] = (uint8_t)clientIdx;
                    outBuf[PACKET_HEADER_SIZE + 1] = destPlayer;
                    memcpy(outBuf + PACKET_HEADER_SIZE + 2,
                           buf + PACKET_HEADER_SIZE + 1, msgLen);
                    outLen = PACKET_HEADER_SIZE + 2 + msgLen;

                    if (destPlayer == 0xFF) {
                        /* Broadcast to all connected clients except sender */
                        int j;
                        for (j = 0; j < MAX_TANKS; j++) {
                            if (udpServer.clients[j].connected && j != clientIdx) {
                                udpSendTo(udpServer.sock, outBuf, outLen,
                                          &udpServer.clients[j].addr);
                            }
                        }
                    } else {
                        /* Send to specific player only */
                        if (destPlayer < MAX_TANKS &&
                            udpServer.clients[destPlayer].connected) {
                            udpSendTo(udpServer.sock, outBuf, outLen,
                                      &udpServer.clients[destPlayer].addr);
                        }
                    }
                }
            }
            break;
        }
        case PACKET_NAME_CHANGE: {
            /* Name change format:
             *   [header 8] [playerNum 1] [newName PACKET_MAX_PLAYER_NAME] */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 &&
                len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME) {
                char newName[PACKET_MAX_PLAYER_NAME];
                int j;
                bool nameTaken = FALSE;

                /* WBN participants are not allowed to change their name */
                if (winboloNetIsPlayerParticipant((BYTE)clientIdx)) {
                    break;
                }

                memcpy(newName, buf + PACKET_HEADER_SIZE + 1,
                       PACKET_MAX_PLAYER_NAME);
                newName[PACKET_MAX_PLAYER_NAME - 1] = '\0';

                /* Check name uniqueness */
                for (j = 0; j < MAX_TANKS; j++) {
                    if (j != clientIdx && udpServer.clients[j].connected &&
                        strcmp(udpServer.clients[j].playerName, newName) == 0) {
                        nameTaken = TRUE;
                        break;
                    }
                }

                if (!nameTaken && newName[0] != '\0') {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Player '%s' changed name to '%s'.",
                             udpServer.clients[clientIdx].playerName, newName);
                    serverSimConsoleMessage(msg);

                    /* Update server-side name */
                    strncpy(udpServer.clients[clientIdx].playerName, newName,
                            PACKET_MAX_PLAYER_NAME - 1);
                    udpServer.clients[clientIdx].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';

                    /* Update in players struct */
                    {
                        ServerSim *ssim = serverSimGetActive();
                        playersSetPlayerName(NULL, &ssim->sim, &ssim->sim.plyrs, (BYTE)clientIdx, newName, TRUE);
                    }

                    /* Broadcast to all other clients */
                    {
                        uint8_t outBuf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME];
                        packHeader(outBuf, PACKET_NAME_CHANGE, 0);
                        outBuf[PACKET_HEADER_SIZE] = (uint8_t)clientIdx;
                        memset(outBuf + PACKET_HEADER_SIZE + 1, 0,
                               PACKET_MAX_PLAYER_NAME);
                        strncpy((char *)(outBuf + PACKET_HEADER_SIZE + 1),
                                newName, PACKET_MAX_PLAYER_NAME - 1);
                        for (j = 0; j < MAX_TANKS; j++) {
                            if (udpServer.clients[j].connected &&
                                j != clientIdx) {
                                udpSendTo(udpServer.sock, outBuf,
                                          sizeof(outBuf),
                                          &udpServer.clients[j].addr);
                            }
                        }
                    }
                }
            }
            break;
        }
        case PACKET_QUIT: {
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0) {
                serverCleanupMapDownload(clientIdx);
                serverDisconnectClient(sim, clientIdx, TRUE);
                serverSimRemovePlayer(sim, (BYTE)clientIdx);
                /* Broadcast lobby update if in lobby/countdown state */
                if (sim->lobbyEnabled &&
                    (sim->state == serverStateLobby || sim->state == serverStateCountdown)) {
                    transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
                    /* Dismiss any pending balance proposal — player composition changed */
                    if (sim->balanceProposal.pending) {
                        uint8_t zeros[MAX_TANKS];
                        memset(zeros, 0, sizeof(zeros));
                        memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
                        transportUdpServerBroadcastBalanceProposal(sim, zeros);
                    }
                }
            }
            break;
        }
        case PACKET_LOCK_TOGGLE: {
            /* Wire: [header 8] [allow 1] */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 1) {
                bool allow = buf[PACKET_HEADER_SIZE] != 0;
                bool wasLocked = serverClientsAllLocked();
                char msg[128];

                udpServer.clientLocked[clientIdx] = !allow;

                snprintf(msg, sizeof(msg), "%s is now %s players to join.",
                         udpServer.clients[clientIdx].playerName,
                         allow ? "allowing" : "not allowing");
                serverSimConsoleMessage(msg);
                serverSendServerMessage(msg);

                /* Check if consensus lock state changed */
                {
                    bool nowLocked = serverClientsAllLocked();
                    if (nowLocked && !wasLocked) {
                        serverSendServerMessage(
                            "This game is now locked to new players (client lock)");
                        serverSimConsoleMessage(
                            "Game locked by client consensus.");
                        winboloNetSendLock(TRUE);
                    } else if (!nowLocked && wasLocked) {
                        serverSendServerMessage(
                            "This game is now unlocked to new players (client unlock)");
                        serverSimConsoleMessage(
                            "Game unlocked by client consensus.");
                        if (!udpServer.gameLocked) {
                            winboloNetSendLock(FALSE);
                        }
                    }
                }
            }
            break;
        }
        case PACKET_ALLIANCE_REQUEST: {
            /* Wire: [header 8] [fromPlayer 1] [toPlayer 1] */
            int clientIdx = serverFindClient(&fromAddr);
            fprintf(stderr, "[UDP SERVER] Alliance request: clientIdx=%d len=%d\n",
                    clientIdx, len);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 2) {
                uint8_t toPlayer = buf[PACKET_HEADER_SIZE + 1];
                fprintf(stderr, "[UDP SERVER] Alliance request from=%d to=%d connected=%d\n",
                        clientIdx, toPlayer,
                        (toPlayer < MAX_TANKS) ? udpServer.clients[toPlayer].connected : -1);
                /* Forward as ALLIANCE_UPDATE (REQUEST) to target player only */
                if (toPlayer < MAX_TANKS &&
                    udpServer.clients[toPlayer].connected) {
                    uint8_t outBuf[PACKET_HEADER_SIZE + 3];
                    packHeader(outBuf, PACKET_ALLIANCE_UPDATE, 0);
                    outBuf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_REQUEST;
                    outBuf[PACKET_HEADER_SIZE + 1] = (uint8_t)clientIdx;
                    outBuf[PACKET_HEADER_SIZE + 2] = toPlayer;
                    udpSendTo(udpServer.sock, outBuf, sizeof(outBuf),
                              &udpServer.clients[toPlayer].addr);
                    fprintf(stderr, "[UDP SERVER] Alliance update forwarded to player %d\n", toPlayer);
                }
            }
            break;
        }
        case PACKET_ALLIANCE_ACCEPT: {
            /* Wire: [header 8] [fromPlayer 1] [toPlayer 1]
             * fromPlayer = the accepter, toPlayer = who requested */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 2) {
                uint8_t newMember = buf[PACKET_HEADER_SIZE + 1];
                /* Apply alliance on server-side players struct */
                ServerSim *ssim = serverSimGetActive();
                playersAcceptAlliance(&ssim->sim, &ssim->sim.plyrs,
                                     (BYTE)clientIdx, newMember, TRUE);
                winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_JOIN, TRUE,
                                   (BYTE)clientIdx, newMember);
                /* Broadcast ALLIANCE_UPDATE (ACCEPT) to all clients */
                {
                    uint8_t outBuf[PACKET_HEADER_SIZE + 3];
                    int j;
                    packHeader(outBuf, PACKET_ALLIANCE_UPDATE, 0);
                    outBuf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_ACCEPT;
                    outBuf[PACKET_HEADER_SIZE + 1] = (uint8_t)clientIdx;
                    outBuf[PACKET_HEADER_SIZE + 2] = newMember;
                    for (j = 0; j < MAX_TANKS; j++) {
                        if (udpServer.clients[j].connected) {
                            udpSendTo(udpServer.sock, outBuf, sizeof(outBuf),
                                      &udpServer.clients[j].addr);
                        }
                    }
                }
            }
            break;
        }
        case PACKET_ALLIANCE_LEAVE: {
            /* Wire: [header 8] [playerNum 1] */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 1) {
                /* Apply on server-side players struct */
                {
                    ServerSim *ssim = serverSimGetActive();
                    playersLeaveAlliance(&ssim->sim, &ssim->sim.plyrs, (BYTE)clientIdx, TRUE);
                }
                winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_LEAVE, TRUE,
                                   (BYTE)clientIdx, WINBOLO_NET_NO_PLAYER);
                /* Broadcast ALLIANCE_UPDATE (LEAVE) to all clients */
                {
                    uint8_t outBuf[PACKET_HEADER_SIZE + 3];
                    int j;
                    packHeader(outBuf, PACKET_ALLIANCE_UPDATE, 0);
                    outBuf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_LEAVE;
                    outBuf[PACKET_HEADER_SIZE + 1] = (uint8_t)clientIdx;
                    outBuf[PACKET_HEADER_SIZE + 2] = 0; /* unused for leave */
                    for (j = 0; j < MAX_TANKS; j++) {
                        if (udpServer.clients[j].connected) {
                            udpSendTo(udpServer.sock, outBuf, sizeof(outBuf),
                                      &udpServer.clients[j].addr);
                        }
                    }
                }
            }
            break;
        }
        case PACKET_MAP_ACK: {
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 2) {
                uint16_t chunkIdx = unpackU16(buf + PACKET_HEADER_SIZE);
                ClientMapDownload *dl = &udpServer.mapDownload[clientIdx];

                udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;

                if (chunkIdx == 0xFFFF) {
                    fprintf(stderr, "[UDP SERVER] Client %d ready for map\n", clientIdx);
                    if (!dl->downloadComplete) {
                        serverSendMapChunks(clientIdx);
                    }
                } else if (chunkIdx < dl->totalChunks && !dl->downloadComplete) {
                    if (!dl->chunkAcked[chunkIdx]) {
                        dl->chunkAcked[chunkIdx] = TRUE;
                        dl->chunksAcked++;
                        fprintf(stderr, "[UDP SERVER] Client %d acked chunk %u/%u\n",
                                clientIdx, dl->chunksAcked, dl->totalChunks);
                    }
                    if (dl->chunksAcked >= dl->totalChunks) {
                        dl->downloadComplete = TRUE;
                        fprintf(stderr, "[UDP SERVER] Client %d map download complete\n", clientIdx);
                    }
                }
            }
            break;
        }
        case PACKET_LOBBY_TEAM_SET: {
            /* Wire: [header 8] [playerNum 1] [teamNumber 1] */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                len >= PACKET_HEADER_SIZE + 2) {
                uint8_t teamNum = buf[PACKET_HEADER_SIZE + 1];
                if (teamNum <= 16) {
                    sim->lobbyPlayers[clientIdx].teamNumber = teamNum;
                    transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
                }
            }
            break;
        }
        case PACKET_LOBBY_READY: {
            /* Wire: [header 8] [playerNum 1] [ready 1] */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && sim->lobbyEnabled &&
                len >= PACKET_HEADER_SIZE + 2) {
                bool ready = buf[PACKET_HEADER_SIZE + 1] != 0;

                if (sim->state == serverStateLobby) {
                    sim->lobbyPlayers[clientIdx].ready = ready;
                    transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
                    serverSimLobbyCheckAllReady(sim);
                    /* If all-ready check triggered countdown, broadcast it */
                    if (sim->state == serverStateCountdown) {
                        uint8_t secs = (uint8_t)(sim->countdownTicks / 50);
                        transportUdpServerBroadcastCountdown(sim, secs);
                    }
                } else if (sim->state == serverStateCountdown && !ready) {
                    /* Someone unreadied during countdown — revert to lobby */
                    sim->lobbyPlayers[clientIdx].ready = FALSE;
                    sim->state = serverStateLobby;
                    sim->countdownTicks = 0;
                    serverSimConsoleMessage("Countdown cancelled — player unreadied.");
                    transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
                }
            }
            break;
        }
        case PACKET_LOBBY_ADD_BOT: {
            /* Wire: [header 8] — no payload needed, server uses its own brain config */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                sim->botAiType != aiNone &&
                sim->botBrainPath[0] != '\0') {
                /* Find first free player slot */
                BYTE slot;
                bool found = false;
                for (slot = 0; slot < MAX_TANKS; slot++) {
                    if (!sim->playerConnected[slot]) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    char botName[64];
                    snprintf(botName, sizeof(botName), "Bot %d", slot + 1);
                    if (botManagerAddBot(sim, slot, sim->botBrainPath, botName,
                                         sim->botAiType,
                                         gameTypeGet(&sim->sim.game),
                                         sim->sim.hiddenMines)) {
                        transportUdpServerBroadcastLobbyUpdate(sim, slot);
                    }
                }
            }
            break;
        }
        case PACKET_LOBBY_REMOVE_BOT: {
            /* Wire: [header 8] [playerNum 1] */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                len >= PACKET_HEADER_SIZE + 1) {
                uint8_t targetSlot = buf[PACKET_HEADER_SIZE];
                if (targetSlot < MAX_TANKS && botManagerIsBot(targetSlot)) {
                    botManagerRemoveBot(sim, targetSlot);
                    transportUdpServerBroadcastLobbyUpdate(sim, targetSlot);
                }
            }
            break;
        }
        case PACKET_WBN_REAUTH: {
            /* Wire: [header 8] [wbnToken 65] */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + WBN_TOKEN_WIRE_LEN) {
                char token[WBN_TOKEN_WIRE_LEN];
                memcpy(token, buf + PACKET_HEADER_SIZE, WBN_TOKEN_WIRE_LEN);
                token[WBN_TOKEN_WIRE_LEN - 1] = '\0';

                if (winbolonetIsRunning() && token[0] != '\0') {
                    char errorMsg[512];
                    bool hasSteam = FALSE;
                    errorMsg[0] = '\0';
                    if (winbolonetServerVerifyToken(token, (BYTE)clientIdx, errorMsg, &hasSteam)) {
                        playersSetWbnParticipant(&sim->sim.plyrs, (BYTE)clientIdx, TRUE);
                        playersSetSteamParticipant(&sim->sim.plyrs, (BYTE)clientIdx, hasSteam);
                        fprintf(stderr, "[UDP SERVER] Player %d WBN re-authenticated (steam=%d)\n",
                                clientIdx, hasSteam ? 1 : 0);
                        /* If game is already running, send the join event now */
                        if (sim->state == serverStateRunning) {
                            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                               (BYTE)clientIdx, WINBOLO_NET_NO_PLAYER);
                        }
                        /* Broadcast updated flags so other clients see WBN badge */
                        if (sim->state == serverStateLobby || sim->state == serverStateCountdown) {
                            transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
                        }
                    } else {
                        fprintf(stderr, "[UDP SERVER] Player %d WBN re-auth failed: %s\n",
                                clientIdx, errorMsg);
                    }
                }
            }
            break;
        }
        case PACKET_BALANCE_REQUEST: {
            /* Wire: [header 8] [teamSize 1] */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx == 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                len >= PACKET_HEADER_SIZE + 1 &&
                !sim->balanceProposal.requestInFlight &&
                !sim->balanceProposal.pending &&
                winbolonetIsRunning()) {
                BalanceThreadData *btd = malloc(sizeof(BalanceThreadData));
                if (btd) {
                    SDL_Thread *t;
                    int i;
                    btd->sim = sim;
                    btd->teamSize = buf[PACKET_HEADER_SIZE];
                    btd->totalPlayers = 0;
                    for (i = 0; i < MAX_TANKS; i++) {
                        if (sim->playerConnected[i]) btd->totalPlayers++;
                    }
                    sim->balanceProposal.requestInFlight = true;
                    t = SDL_CreateThread(balanceThreadFunc, "WbnBalance", btd);
                    if (t) {
                        SDL_DetachThread(t);
                    } else {
                        sim->balanceProposal.requestInFlight = false;
                        free(btd);
                        serverSimConsoleMessage("Failed to start balance thread");
                    }
                }
            }
            break;
        }
        case PACKET_BALANCE_APPLY: {
            /* Wire: [header 8] (no payload) */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx == 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                sim->balanceProposal.pending) {
                int i;
                for (i = 0; i < MAX_TANKS; i++) {
                    if (sim->balanceProposal.teamForSlot[i] != 0) {
                        sim->lobbyPlayers[i].teamNumber = sim->balanceProposal.teamForSlot[i];
                    }
                }
                memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
                transportUdpServerBroadcastLobbyState(sim);
                serverSimConsoleMessage("Team balance applied");
            }
            break;
        }
        case PACKET_BALANCE_DISMISS: {
            /* Wire: [header 8] (no payload) */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx == 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                sim->balanceProposal.pending) {
                uint8_t zeros[MAX_TANKS];
                memset(zeros, 0, sizeof(zeros));
                memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
                transportUdpServerBroadcastBalanceProposal(sim, zeros);
            }
            break;
        }
        case PACKET_MAP_SKIP_VOTE: {
            /* Wire: [header 8] (no payload — server identifies player by source) */
            int clientIdx = serverFindClient(&fromAddr);
            if (clientIdx >= 0) {
                serverSimMapSkipVoteToggle(sim, (uint8_t)clientIdx);
                transportUdpServerBroadcastMapSkipState(sim);
            }
            break;
        }
        default:
            break;
        }
    }

}

/* Drain sim events into per-client reliable queues.
 * Must be called after each serverSimTick() so events survive
 * being cleared at the start of the next tick.
 * EVENT_SOUND events are culled by distance and deduplicated per
 * sound type (only the closest instance of each type is sent). */
void transportUdpServerDrainEvents(ServerSim *sim) {
    int i, c;

    if (!udpServer.running) return;
    if (sim->eventCount == 0 && sim->mapEventCount == 0) return;

    fprintf(stderr, "[UDP SERVER] Enqueuing %d events + %d map events from tick=%u\n",
            sim->eventCount, sim->mapEventCount, sim->tick);

    for (c = 0; c < MAX_TANKS; c++) {
        ClientEventQueue *cq;
        WORLD cwx = 0, cwy = 0;
        BYTE clientMX = 0, clientMY = 0;
        bool hasPos;

        if (!udpServer.clients[c].connected) continue;
        if (!udpServer.mapDownload[c].downloadComplete) continue;
        cq = &udpServer.eventQueues[c];

        hasPos = serverSimGetTankState(sim, (BYTE)c, &cwx, &cwy);
        if (hasPos) {
            clientMX = (BYTE)(cwx >> 8);
            clientMY = (BYTE)(cwy >> 8);
        }

        /* Pass 1: find best (closest) sound event per type for this client */
        #define MAX_SOUND_TYPES 32
        int bestSoundIdx[MAX_SOUND_TYPES];
        int bestSoundDist[MAX_SOUND_TYPES];
        int s;
        for (s = 0; s < MAX_SOUND_TYPES; s++) {
            bestSoundIdx[s] = -1;
            bestSoundDist[s] = 255;
        }

        for (i = 0; i < (int)sim->eventCount; i++) {
            uint8_t evType = sim->events[i].type;
            if ((evType == EVENT_SOUND || evType == EVENT_SOUND_TANK_HIT || evType == EVENT_SOUND_SHOOT) && hasPos) {
                uint8_t soundId = sim->events[i].data[0];
                uint8_t mx = sim->events[i].data[1];
                uint8_t my = sim->events[i].data[2];
                int dx = (clientMX > mx) ? (clientMX - mx) : (mx - clientMX);
                int dy = (clientMY > my) ? (clientMY - my) : (my - clientMY);

                /* Skip own shoot sound — client plays shootSelf via prediction */
                if (evType == EVENT_SOUND_SHOOT && sim->events[i].data[3] == (uint8_t)c) {
                    continue;
                }

                /* Always send tank hit to the hit player (plays hitTankSelf at full volume) */
                if (evType == EVENT_SOUND_TANK_HIT && sim->events[i].data[3] == (uint8_t)c) {
                    /* Skip distance cull */
                } else if (dx >= SDIST_NONE || dy >= SDIST_NONE) {
                    /* Cull beyond audible range */
                    continue;
                }

                /* Keep closest per type */
                int dist = dx + dy;
                if (soundId < MAX_SOUND_TYPES && dist < bestSoundDist[soundId]) {
                    bestSoundIdx[soundId] = i;
                    bestSoundDist[soundId] = dist;
                }
            }
        }

        /* Pass 2: enqueue map events first (from dedicated buffer),
         * then non-sound events, then deduplicated sounds */
        for (i = 0; i < (int)sim->mapEventCount; i++) {
            uint32_t idx = cq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
            cq->buffer[idx].event = sim->mapEvents[i];
            cq->buffer[idx].seq = cq->nextSeq;
            cq->nextSeq++;
        }
        for (i = 0; i < (int)sim->eventCount; i++) {
            uint8_t evType = sim->events[i].type;
            if (evType != EVENT_SOUND && evType != EVENT_SOUND_TANK_HIT && evType != EVENT_SOUND_SHOOT) {
                uint32_t idx = cq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                cq->buffer[idx].event = sim->events[i];
                cq->buffer[idx].seq = cq->nextSeq;
                cq->nextSeq++;
            }
        }
        for (s = 0; s < MAX_SOUND_TYPES; s++) {
            if (bestSoundIdx[s] >= 0) {
                uint32_t idx = cq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                cq->buffer[idx].event = sim->events[bestSoundIdx[s]];
                cq->buffer[idx].seq = cq->nextSeq;
                cq->nextSeq++;
            }
        }
        #undef MAX_SOUND_TYPES
    }
}

/* Send snapshots and check timeouts */
void transportUdpServerSend(ServerSim *sim) {
    int i;

    if (!udpServer.running) return;

    /* Broadcast snapshots to connected clients that have finished map download */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        if (!udpServer.mapDownload[i].downloadComplete) {
            /* Still downloading map — resend un-acked chunks every 25 ticks (0.5s) */
            if (udpServer.tickCount - udpServer.mapDownload[i].lastSendTick >= 25) {
                serverSendMapChunks(i);
            }
            continue;
        }

        /* Send existing player list before the first snapshot.
         * This must happen after map download so the client's players
         * struct (recreated during map load) is ready.
         * Format: [header][count]
         *   [playerNum 1][name 32][numAllies 1][ally0 1][ally1 1]...
         * Each player entry is variable-length due to allies. */
        if (udpServer.clients[i].needsPlayerList) {
            uint8_t plBuf[UDP_MAX_PAYLOAD];
            int plPos = PACKET_HEADER_SIZE;
            uint8_t plCount = 0;
            int j;

            plPos++; /* reserve byte for count */
            for (j = 0; j < MAX_TANKS; j++) {
                BYTE allies[MAX_TANKS];
                BYTE numAllies;
                char playerName[PACKET_MAX_PLAYER_NAME];
                if (j == i) continue;
                /* Include both UDP clients and bot players (sim-connected but no UDP client) */
                if (!udpServer.clients[j].connected && !sim->playerConnected[j]) continue;

                numAllies = playersMakeNetAlliences(
                    &serverSimGetActive()->sim.plyrs, (BYTE)j, allies);

                /* Check we have room: 1 + 32 + 2 + 1 + numAllies */
                if (plPos + 1 + PACKET_MAX_PLAYER_NAME + 2 + 1 + numAllies
                    > (int)sizeof(plBuf))
                    break;

                plBuf[plPos++] = (uint8_t)j;
                memset(plBuf + plPos, 0, PACKET_MAX_PLAYER_NAME);
                /* Get name from players struct (works for both UDP clients and bots) */
                memset(playerName, 0, sizeof(playerName));
                playersGetPlayerName(&serverSimGetActive()->sim.plyrs, (BYTE)j, playerName, TRUE);
                strncpy((char *)(plBuf + plPos), playerName, PACKET_MAX_PLAYER_NAME - 1);
                plPos += PACKET_MAX_PLAYER_NAME;
                /* Country code (2 bytes) */
                plBuf[plPos++] = (uint8_t)udpServer.clients[j].countryCode[0];
                plBuf[plPos++] = (uint8_t)udpServer.clients[j].countryCode[1];
                plBuf[plPos++] = numAllies;
                if (numAllies > 0) {
                    memcpy(plBuf + plPos, allies, numAllies);
                    plPos += numAllies;
                }
                plCount++;
            }
            packHeader(plBuf, PACKET_PLAYER_LIST, 0);
            plBuf[PACKET_HEADER_SIZE] = plCount;
            if (plCount > 0) {
                udpSendTo(udpServer.sock, plBuf, plPos,
                          &udpServer.clients[i].addr);
            }
            udpServer.clients[i].needsPlayerList = false;
        }

        serverSendSnapshot(sim, i);
    }

    transportUdpServerCheckTimeouts(sim);
}

/* Check for client timeouts — call from any server state (lobby, running, etc.) */
void transportUdpServerCheckTimeouts(ServerSim *sim) {
    int i;

    if (!udpServer.running) return;

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        if (udpServer.tickCount - udpServer.clients[i].lastReceivedTick
            > CLIENT_TIMEOUT_TICKS) {
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (sim->lobbyEnabled &&
                (sim->state == serverStateLobby || sim->state == serverStateCountdown)) {
                transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)i);
                /* Dismiss any pending balance proposal — player composition changed */
                if (sim->balanceProposal.pending) {
                    uint8_t zeros[MAX_TANKS];
                    memset(zeros, 0, sizeof(zeros));
                    memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
                    transportUdpServerBroadcastBalanceProposal(sim, zeros);
                }
            }
        }
    }
}

/* Combined receive + tick + send (for callers that don't need split) */
void transportUdpServerTick(ServerSim *sim) {
    transportUdpServerRecv(sim);
    transportUdpServerDrainEvents(sim);
    transportUdpServerSend(sim);
}

int transportUdpServerGetClientCount(void) {
    int count = 0;
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) count++;
    }
    return count;
}

uint16_t transportUdpServerGetClientPing(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) return 0;
    if (!udpServer.clients[playerNum].connected) return 0;
    return udpServer.clients[playerNum].pingMs;
}

void transportUdpServerSetLock(ServerSim *sim, bool locked) {
    (void)sim;
    if (udpServer.gameLocked == locked) return;
    udpServer.gameLocked = locked;
    serverSimConsoleMessage(locked
        ? "This game is now locked to new players (server lock)"
        : "This game is now unlocked to new players (server unlock)");
    winboloNetSendLock(locked);
    /* Enqueue directly into per-client reliable queues.
     * We can't use serverSimAddEvent() because the sim's event buffer
     * gets cleared at the start of each tick — this runs from the
     * console thread between ticks so the event would be lost. */
    {
        GameEvent ev;
        int c;
        ev.type = EVENT_SERVER_MSG;
        memset(ev.data, 0, sizeof(ev.data));
        ev.data[0] = locked ? SERVER_MSG_GAME_LOCKED : SERVER_MSG_GAME_UNLOCKED;
        for (c = 0; c < MAX_TANKS; c++) {
            ClientEventQueue *q;
            uint32_t idx;
            if (!udpServer.clients[c].connected) continue;
            if (!udpServer.mapDownload[c].downloadComplete) continue;
            q = &udpServer.eventQueues[c];
            idx = q->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
            q->buffer[idx].event = ev;
            q->buffer[idx].seq = q->nextSeq;
            q->nextSeq++;
        }
    }
}

bool transportUdpServerGetLock(void) {
    return udpServer.gameLocked;
}

void transportUdpServerSendServerMessage(const char *message) {
    serverSendServerMessage(message);
}

void transportUdpServerPrintStatus(bool toFile) {
    int i;
    FILE *fp = NULL;

    if (toFile) {
        fp = fopen("status.txt", "w");
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        fprintf(stdout, "%s (slot %d, ping %dms)\n",
                udpServer.clients[i].playerName, i,
                udpServer.clients[i].pingMs);
        if (fp != NULL) {
            fprintf(fp, "%s (slot %d, ping %dms)\n",
                    udpServer.clients[i].playerName, i,
                    udpServer.clients[i].pingMs);
        }
    }

    if (fp != NULL) {
        fclose(fp);
    }
}

BYTE transportUdpServerGetMaxPlayers(void) {
    return udpServer.maxPlayers;
}
