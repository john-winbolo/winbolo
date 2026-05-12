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
 *Name:          Transport UDP Client
 *Filename:      transport_udp_client.c
 *Author:        John Morrison
 *Purpose:
 *  Client-side UDP network transport for multiplayer games.
 *    - Sends InputPackets to the server (with redundancy:
 *      last 3 inputs per packet for loss tolerance).
 *    - Receives state snapshots from server.
 *    - Handles join handshake and ping measurement.
 *********************************************************/

#include "transport_udp_internal.h"
#include "bases.h"
#include "pillbox.h"
#include "players.h"
#include "util.h"
#include "messages.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "control_event.h"
#include "client_sim_control.h"
#include "../gui/lang.h"
#include "../gui/winbolo.h"
#include "../gui/dialogAlliance.h"
#include "../steam/steam_wrapper.h"
#include "../common/wb_log.h"

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
    TkExplosionSnapshot snapshotTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot snapshotBases[MAX_SNAPSHOT_BASES];
    PillSnapshot snapshotPills[MAX_SNAPSHOT_PILLS];
    GameEvent snapshotEvents[MAX_SNAPSHOT_EVENTS];
    uint32_t lastSnapshotSeq;  /* Sequence number of latest snapshot */
    uint32_t lastSnapshotTick; /* Local tick when last snapshot arrived (for timeout) */

    /* Reliable event dedup */
    uint32_t reliableEventAck;  /* Next expected reliable game event seq (init to 1) */
    uint32_t mapEventAck;       /* Next expected reliable map event seq (init to 1) */

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

    /* Join reject reason from server, rendered locally via langGetTextFmt
     * after Phase 9d wire format change. Sized for the longest expected
     * localized rendering. */
    char joinRejectReason[256];

    /* Owning ClientSim â€” used for player state updates in callbacks */
    ClientSim *clientSim;

    /* Network stats (client-side only) */
    uint32_t packetsRecvThisSec;  /* Packets received in current 1-second window */
    uint32_t packetsSentThisSec;  /* Packets sent in current 1-second window */
    uint32_t bytesRecvThisSec;    /* Bytes received in current 1-second window */
    uint32_t bytesSentThisSec;    /* Bytes sent in current 1-second window */
    uint32_t ppsWindowStart;      /* localTick when current PPS window started */
    uint32_t ppsRecv;             /* Last completed PPS (recv) */
    uint32_t ppsSent;             /* Last completed PPS (sent) */
    uint32_t bpsRecv;             /* Last completed bytes/sec (recv) */
    uint32_t bpsSent;             /* Last completed bytes/sec (sent) */
    uint32_t netErrors;           /* Cumulative: stale snapshots, truncated packets */

    bool wantRejoin;               /* Request rejoin (restore pills/bases) on connect */

    /* Phase 3 â€” UDP hole-punching fallback. Empty trackerAddr disables
     * punch entirely (LAN/manual-connect joiners). */
    char           trackerAddr[FILENAME_MAX];
    unsigned short trackerPort;
    struct in_addr targetIp;       /* host IP (network order) for PUNCH_REQUEST body */
    unsigned short targetPort;     /* host port (host order) for PUNCH_REQUEST body */
    bool           punchSent;      /* sent at least one PUNCH_REQUEST */
} TransportUdpClientCtx;

/* Client send wrapper â€” tracks packet and byte counters */
static void udpClientSendTo(TransportUdpClientCtx *c, const uint8_t *buf, int len) {
    udpSendTo(c->sock, buf, len, &c->serverAddr);
    c->packetsSentThisSec++;
    c->bytesSentThisSec += len;
}

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

/* Record input into redundancy ring without sending a packet.
 * Used on keys ticks so the input is carried by the next sendInput. */
static void udpClientRecordInput(void *ctx, const InputPacket *input) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;

    if (c->joinState != UDP_CLIENT_CONNECTED) {
        return;
    }

    /* Store in ring buffer â€” stamp with current reliable ACKs and ping */
    {
        InputPacket stamped = *input;
        stamped.eventAck = c->reliableEventAck;
        stamped.mapEventAck = c->mapEventAck;
        stamped.pingMs = c->pingMs;
        c->inputRing[c->inputRingCount % CLIENT_INPUT_RING_SIZE] = stamped;
    }
    c->inputRingCount++;
}

/* Client sendInput: record input and send packet with redundancy to server */
static void udpClientSendInput(void *ctx, const InputPacket *input) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;
    uint8_t buf[UDP_MAX_PAYLOAD];
    int len;

    udpClientRecordInput(ctx, input);

    if (c->joinState != UDP_CLIENT_CONNECTED) {
        return;
    }

    len = buildInputPacket(c, buf);
    udpClientSendTo(c, buf, len);
}

/* Decode a localized payload (langid + arg list) at buf[startPos..len)
 * into outId and outArgs.  Mirrors packLocalizedPayload on the server.
 * Args land in MessageArgs slots in order: #1->playerName, #2->otherName,
 * #3->string1, #4->string2.  Returns false on malformed packet (bad
 * length, argCount > 4, lenByte oversized, langid == 0). */
static bool decodeLocalizedPayload(const uint8_t *buf, int len, int startPos,
                                   langid *outId, MessageArgs *outArgs) {
    int pos = startPos;
    uint8_t argCount;
    uint16_t id16;
    int i;
    if (pos + 3 > len) {
        fprintf(stderr,
                "[UDP CLIENT] localized payload truncated (need 3 hdr bytes, len=%d pos=%d)\n",
                len, pos);
        return false;
    }
    id16 = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
    pos += 2;
    argCount = buf[pos++];
    if (id16 == 0) {
        fprintf(stderr, "[UDP CLIENT] localized payload langid=0\n");
        return false;
    }
    if (argCount > 4) {
        fprintf(stderr,
                "[UDP CLIENT] localized payload argCount=%u exceeds 4\n",
                argCount);
        return false;
    }
    memset(outArgs, 0, sizeof(*outArgs));
    for (i = 0; i < argCount; i++) {
        uint8_t aLen;
        char *dst = NULL;
        size_t cap = 0;
        if (pos + 1 > len) {
            fprintf(stderr,
                    "[UDP CLIENT] localized payload truncated at arg %d lenByte\n",
                    i);
            return false;
        }
        aLen = buf[pos++];
        if (pos + aLen > len) {
            fprintf(stderr,
                    "[UDP CLIENT] localized payload truncated: arg %d aLen=%u\n",
                    i, aLen);
            return false;
        }
        if (aLen >= PLAYER_NAME_LEN) {
            fprintf(stderr,
                    "[UDP CLIENT] localized payload arg %d aLen=%u exceeds %d\n",
                    i, aLen, PLAYER_NAME_LEN - 1);
            return false;
        }
        switch (i) {
            case 0: dst = outArgs->playerName; cap = PLAYER_NAME_LEN; break;
            case 1: dst = outArgs->otherName;  cap = PLAYER_NAME_LEN; break;
            case 2: dst = outArgs->string1;    cap = LANG_MSGARG_STRING_LEN; break;
            case 3: dst = outArgs->string2;    cap = LANG_MSGARG_STRING_LEN; break;
        }
        if (dst && cap > 0) {
            size_t copy = (aLen < cap - 1) ? aLen : cap - 1;
            if (copy > 0) memcpy(dst, buf + pos, copy);
            dst[copy] = '\0';
        }
        pos += aLen;
    }
    *outId = (langid)id16;
    return true;
}

/* Process a single incoming packet (used by both direct and delayed paths) */
static void udpClientProcessPacket(TransportUdpClientCtx *c,
                                   const uint8_t *buf, int len) {
    uint8_t pktType = getPacketType(buf, len);

    c->packetsRecvThisSec++;
    c->bytesRecvThisSec += len;

    /* Reset timeout on any valid server packet â€” lobby state doesn't send
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
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "PACKET_JOIN_ACCEPT received: state=%d len=%d (need>=%d)",
            (int)c->joinState, len, PACKET_HEADER_SIZE + 19);
        if ((c->joinState == UDP_CLIENT_JOINING ||
             c->joinState == UDP_CLIENT_DOWNLOADING_MAP) &&
            len >= PACKET_HEADER_SIZE + 19) {
            int pos = PACKET_HEADER_SIZE;
            uint32_t mapSize;

            c->playerNum = buf[pos++];
            clientSimSetPlayerNum(c->clientSim, c->playerNum);
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
                udpClientSendTo(c, ackBuf, sizeof(ackBuf));
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
                udpClientSendTo(c, ackBuf, sizeof(ackBuf));
            }

            /* Check if all chunks received */
            if (c->mapChunksReceived >= c->mapChunksExpected) {
                WB_LOG_INFO(WB_LOG_CAT_NET,
                    "map download complete: chunks=%u/%u bytes=%u/%u "
                    "-> CONNECTED (playerNum=%u)",
                    (unsigned)c->mapChunksReceived,
                    (unsigned)c->mapChunksExpected,
                    (unsigned)c->mapDownloadReceived,
                    (unsigned)c->mapDownloadTotal,
                    (unsigned)c->playerNum);
                c->joinState = UDP_CLIENT_CONNECTED;
                {
                    ControlEvent evt = { .type = CTRL_MAP_DOWNLOAD_COMPLETE };
                    clientSimApplyControl(c->clientSim, &evt);
                }
            }
        }
        break;

    case PACKET_JOIN_REJECT: {
        /* Wire format (Phase 9d):
         *   [header 8] [langid 2 BE] [argCount 1] [args...] */
        langid id = 0;
        MessageArgs args;
        if (decodeLocalizedPayload(buf, len, PACKET_HEADER_SIZE, &id, &args)) {
            const char *rendered = langGetTextFmt(id, &args);
            if (rendered && rendered[0]) {
                strncpy(c->joinRejectReason, rendered,
                        sizeof(c->joinRejectReason) - 1);
                c->joinRejectReason[sizeof(c->joinRejectReason) - 1] = '\0';
            } else {
                strncpy(c->joinRejectReason, "Connection rejected",
                        sizeof(c->joinRejectReason) - 1);
                c->joinRejectReason[sizeof(c->joinRejectReason) - 1] = '\0';
            }
        } else {
            strncpy(c->joinRejectReason, "Connection rejected",
                    sizeof(c->joinRejectReason) - 1);
            c->joinRejectReason[sizeof(c->joinRejectReason) - 1] = '\0';
        }
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "PACKET_JOIN_REJECT: langid=%u reason='%s'",
            (unsigned)id, c->joinRejectReason);
        c->joinState = UDP_CLIENT_ERROR;
        break;
    }

    case PACKET_STATE_SNAPSHOT: {
        uint32_t seq = unpackU32(buf + 4);
        int pos = PACKET_HEADER_SIZE;
        int i;
        uint8_t tankCount, shellCount, tkExplosionCount;
        uint8_t baseCount, pillCount, reliableEventCount;
        uint32_t reliableBaseSeq;
        uint8_t mapEventCount;
        uint32_t mapEventBaseSeq;
        int newEventCount = (c->hasSnapshot) ? c->snapshotHdr.reliableEventCount : 0;
        int actuallyUnpacked = 0;
        int actuallyUnpackedMap = 0;

        /* Ignore stale snapshots */
        if (c->hasSnapshot && seq <= c->lastSnapshotSeq) {
            c->netErrors++;
            break;
        }

        /* Header: serverTick(4) + lastProcessedInput(4) + tankCount(1)
         * + shellCount(1) + tkExplosionCount(1)
         * + baseCount(1) + pillCount(1)
         * + reliableEventCount(1) + reliableBaseSeq(4)
         * + mapEventCount(1) + mapEventBaseSeq(4)
         * + mapChecksum(2) = 25 bytes */
        if (len < pos + 25) { c->netErrors++; break; }

        c->snapshotHdr.serverTick = unpackU32(buf + pos);
        pos += 4;
        c->snapshotHdr.lastProcessedInput = unpackU32(buf + pos);
        pos += 4;
        tankCount = buf[pos++];
        shellCount = buf[pos++];
        tkExplosionCount = buf[pos++];
        baseCount = buf[pos++];
        pillCount = buf[pos++];
        reliableEventCount = buf[pos++];
        reliableBaseSeq = unpackU32(buf + pos);
        pos += 4;
        mapEventCount = buf[pos++];
        mapEventBaseSeq = unpackU32(buf + pos);
        pos += 4;
        c->snapshotHdr.mapChecksum = unpackU16(buf + pos);
        pos += 2;

        c->snapshotHdr.tankCount = tankCount;
        c->snapshotHdr.shellCount = shellCount;
        c->snapshotHdr.tkExplosionCount = tkExplosionCount;
        c->snapshotHdr.baseCount = baseCount;
        c->snapshotHdr.pillCount = pillCount;

        /* Unpack tanks â€” variable length: stubs are 1 byte, full entries
         * are TANK_SNAPSHOT_WIRE_SIZE bytes.  The first byte's high bit
         * (TANK_SNAPSHOT_HIDDEN_FLAG) tells us which. */
        if (tankCount > MAX_TANKS) tankCount = MAX_TANKS;
        {
            bool tankBoundsOk = TRUE;
            for (i = 0; i < tankCount; i++) {
                int needed;
                if (pos + 1 > len) { tankBoundsOk = FALSE; break; }
                needed = (buf[pos] & TANK_SNAPSHOT_HIDDEN_FLAG) ? 1 : TANK_SNAPSHOT_WIRE_SIZE;
                if (pos + needed > len) { tankBoundsOk = FALSE; break; }
                pos += unpackTankSnapshot(buf + pos, &c->snapshotTanks[i]);
            }
            if (!tankBoundsOk) break;
        }

        /* Unpack shells */
        if (shellCount > MAX_SNAPSHOT_SHELLS) shellCount = MAX_SNAPSHOT_SHELLS;
        if (len < pos + shellCount * SHELL_SNAPSHOT_WIRE_SIZE) break;
        for (i = 0; i < shellCount; i++) {
            unpackShellSnapshot(buf + pos, &c->snapshotShells[i]);
            pos += SHELL_SNAPSHOT_WIRE_SIZE;
        }

        /* Unpack tank explosions */
        if (tkExplosionCount > MAX_SNAPSHOT_TK_EXPLOSIONS) tkExplosionCount = MAX_SNAPSHOT_TK_EXPLOSIONS;
        if (len < pos + tkExplosionCount * TK_EXPLOSION_SNAPSHOT_WIRE_SIZE) break;
        for (i = 0; i < tkExplosionCount; i++) {
            unpackTkExplosionSnapshot(buf + pos, &c->snapshotTkExplosions[i]);
            pos += TK_EXPLOSION_SNAPSHOT_WIRE_SIZE;
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

        /* Unpack reliable game events with dedup.
         * Only advance ACK based on events we actually consumed â€” stop
         * on truncated packet OR when the local buffer is full. */
        for (i = 0; i < reliableEventCount; i++) {
            uint32_t evSeq = reliableBaseSeq + (uint32_t)i;
            GameEvent ev;
            if (pos + 1 > len) break;  /* Truncated packet â€” stop */
            pos += unpackGameEvent(buf + pos, &ev);
            actuallyUnpacked++;
            /* Only apply events we haven't seen yet */
            if (evSeq >= c->reliableEventAck) {
                if (newEventCount < MAX_SNAPSHOT_EVENTS) {
                    c->snapshotEvents[newEventCount++] = ev;
                } else {
                    break;  /* Buffer full â€” stop so we don't ACK unconsumed events */
                }
            }
        }
        /* Only ACK events we actually unpacked from the wire */
        if (actuallyUnpacked > 0) {
            uint32_t lastSeq = reliableBaseSeq + (uint32_t)actuallyUnpacked;
            if (lastSeq > c->reliableEventAck) {
                c->reliableEventAck = lastSeq;
            }
        }

        /* Unpack reliable map events with dedup (separate stream).
         * Map events are merged into snapshotEvents after game events
         * so callers don't need to change. */
        for (i = 0; i < mapEventCount; i++) {
            uint32_t evSeq = mapEventBaseSeq + (uint32_t)i;
            GameEvent ev;
            if (pos + 1 > len) break;  /* Truncated packet â€” stop */
            pos += unpackGameEvent(buf + pos, &ev);
            actuallyUnpackedMap++;
            if (evSeq >= c->mapEventAck) {
                if (newEventCount < MAX_SNAPSHOT_EVENTS) {
                    c->snapshotEvents[newEventCount++] = ev;
                } else {
                    break;  /* Buffer full â€” stop so we don't ACK unconsumed events */
                }
            }
        }
        /* Only ACK map events we actually unpacked from the wire */
        if (actuallyUnpackedMap > 0) {
            uint32_t lastSeq = mapEventBaseSeq + (uint32_t)actuallyUnpackedMap;
            if (lastSeq > c->mapEventAck) {
                c->mapEventAck = lastSeq;
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
            /* Immediately echo the server timestamp back so the server can
             * measure RTT.  Use clientTime=0 as a marker so the server
             * knows this is an echo-only PING and won't send another PONG. */
            {
                uint32_t srvTime = unpackU32(buf + PACKET_HEADER_SIZE + 4);
                if (srvTime > 0) {
                    uint8_t echoBuf[PACKET_HEADER_SIZE + 8];
                    packHeader(echoBuf, PACKET_PING, c->outSequence++);
                    packU32(echoBuf + PACKET_HEADER_SIZE, 0);       /* clientTime=0: echo-only */
                    packU32(echoBuf + PACKET_HEADER_SIZE + 4, srvTime);
                    udpClientSendTo(c, echoBuf, sizeof(echoBuf));
                }
            }
        }
        break;

    case PACKET_PLAYER_JOINED: {
        /* Wire format: [pNum 1][name 32][cc 2][clientType 1][clientFlags 1]. */
        if (len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 2 + 2) {
            int pos = PACKET_HEADER_SIZE;
            uint8_t pNum = buf[pos++];
            char pName[PACKET_MAX_PLAYER_NAME];
            char cc[3] = {0, 0, 0};
            uint8_t clientType;
            uint8_t clientFlags;
            memcpy(pName, buf + pos, PACKET_MAX_PLAYER_NAME);
            pName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
            pos += PACKET_MAX_PLAYER_NAME;
            cc[0] = (char)buf[pos++];
            cc[1] = (char)buf[pos++];
            clientType = buf[pos++];
            clientFlags = buf[pos++];
            if (clientType >= CLIENT_TYPE_COUNT) clientType = CLIENT_TYPE_UNKNOWN;
            if (pNum != c->playerNum) {
                ControlEvent evt = { .type = CTRL_PLAYER_JOIN };
                evt.u.playerJoin.playerNum = pNum;
                memcpy(evt.u.playerJoin.name, pName, sizeof(evt.u.playerJoin.name));
                evt.u.playerJoin.name[sizeof(evt.u.playerJoin.name) - 1] = '\0';
                evt.u.playerJoin.country[0] = cc[0];
                evt.u.playerJoin.country[1] = cc[1];
                evt.u.playerJoin.country[2] = '\0';
                evt.u.playerJoin.clientType = clientType;
                evt.u.playerJoin.clientFlags = clientFlags;
                evt.u.playerJoin.numAllies = 0;
                clientSimApplyControl(c->clientSim, &evt);
                /* Show join message in lobby chat */
                if (c->clientSim->inLobby) {
                    char joinMsg[PACKET_MAX_PLAYER_NAME + 16];
                    snprintf(joinMsg, sizeof(joinMsg), "%s has joined.", pName);
                    clientSimAppendLobbyChat(c->clientSim, "***", joinMsg);
                }
            }
        }
        break;
    }

    case PACKET_PLAYER_LIST:
        /* Player list format: [header][count]
         *   [playerNum 1][name 32][cc 2][clientType 1][clientFlags 1]
         *   [numAllies 1][ally0 1]...
         * Each entry is variable-length. */
        if (len >= PACKET_HEADER_SIZE + 1) {
            uint8_t plCount = buf[PACKET_HEADER_SIZE];
            int plPos = PACKET_HEADER_SIZE + 1;
            int p;
            for (p = 0; p < plCount; p++) {
                uint8_t pNum;
                char pName[PACKET_MAX_PLAYER_NAME];
                char cc[3] = {0, 0, 0};
                uint8_t clientType;
                uint8_t clientFlags;
                uint8_t numAllies;
                BYTE allies[MAX_TANKS];

                if (plPos + 1 + PACKET_MAX_PLAYER_NAME + 2 + 2 + 1 > len) break;
                pNum = buf[plPos];
                memcpy(pName, buf + plPos + 1, PACKET_MAX_PLAYER_NAME);
                pName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
                plPos += 1 + PACKET_MAX_PLAYER_NAME;
                cc[0] = (char)buf[plPos++];
                cc[1] = (char)buf[plPos++];
                clientType = buf[plPos++];
                clientFlags = buf[plPos++];
                if (clientType >= CLIENT_TYPE_COUNT) clientType = CLIENT_TYPE_UNKNOWN;
                numAllies = buf[plPos++];
                if (numAllies > MAX_TANKS) numAllies = MAX_TANKS;
                if (plPos + numAllies > len) break;
                if (numAllies > 0) {
                    memcpy(allies, buf + plPos, numAllies);
                    plPos += numAllies;
                }
                {
                    ControlEvent evt = { .type = CTRL_PLAYER_JOIN };
                    evt.u.playerJoin.playerNum = pNum;
                    memcpy(evt.u.playerJoin.name, pName, sizeof(evt.u.playerJoin.name));
                    evt.u.playerJoin.name[sizeof(evt.u.playerJoin.name) - 1] = '\0';
                    evt.u.playerJoin.country[0] = cc[0];
                    evt.u.playerJoin.country[1] = cc[1];
                    evt.u.playerJoin.country[2] = '\0';
                    evt.u.playerJoin.clientType = clientType;
                    evt.u.playerJoin.clientFlags = clientFlags;
                    evt.u.playerJoin.numAllies = numAllies;
                    if (numAllies > 0) {
                        memcpy(evt.u.playerJoin.allies, allies, numAllies);
                    }
                    clientSimApplyControl(c->clientSim, &evt);
                }
            }
            /* Server skips our own slot when building PLAYER_LIST, so the
             * loop above never updates item[selfPlayer].allie. Rebuild it
             * now from the per-player lists we just decoded. */
            playersRebuildSelfAlliance(&c->clientSim->sim, &c->clientSim->sim.plyrs,
                                       c->clientSim->myPlayerNum);
        }
        break;

    case PACKET_PLAYER_LEFT:
        if (len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME) {
            uint8_t pNum = buf[PACKET_HEADER_SIZE];
            char pName[PACKET_MAX_PLAYER_NAME];
            memcpy(pName, buf + PACKET_HEADER_SIZE + 1, PACKET_MAX_PLAYER_NAME);
            pName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
            if (pNum != c->playerNum) {
                /* Show leave message in lobby chat */
                if (c->clientSim->inLobby) {
                    char leaveMsg[PACKET_MAX_PLAYER_NAME + 16];
                    snprintf(leaveMsg, sizeof(leaveMsg), "%s has left.", pName);
                    clientSimAppendLobbyChat(c->clientSim, "***", leaveMsg);
                }
            }
        }
        break;

    case PACKET_NAME_CHANGE: {
        /* Name change format:
         *   [header 8] [playerNum 1] [newName PACKET_MAX_PLAYER_NAME] */
        if (len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME) {
            uint8_t pNum = buf[PACKET_HEADER_SIZE];
            ControlEvent evt = { .type = CTRL_PLAYER_NAME };
            evt.u.playerName.playerNum = pNum;
            memcpy(evt.u.playerName.name, buf + PACKET_HEADER_SIZE + 1, PACKET_MAX_PLAYER_NAME);
            evt.u.playerName.name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
            clientSimApplyControl(c->clientSim, &evt);
        }
        break;
    }

    case PACKET_NAME_CHANGE_REJECT:
        /* Name change reject format:
         *   [header 8] [reasonCode 1] */
        if (len < PACKET_HEADER_SIZE + 1) {
            fprintf(stderr, "[UDP CLIENT] PACKET_NAME_CHANGE_REJECT: short packet (len=%d)\n", len);
        } else {
            uint8_t reasonCode = buf[PACKET_HEADER_SIZE];
            langid msgId;
            const char *rendered;
            char rendBuf[FILENAME_MAX];
            switch (reasonCode) {
                case NAME_REJECT_TAKEN:
                    msgId = STR_DLGSETNAME_INUSE_ERR;
                    break;
                case NAME_REJECT_RESERVED_PREFIX:
                    msgId = STR_NAME_INVALID_RESERVED_PREFIX;
                    break;
                case NAME_REJECT_RESERVED_SUFFIX:
                    msgId = STR_NAME_INVALID_RESERVED_SUFFIX;
                    break;
                case NAME_REJECT_MIXED_SCRIPTS:
                    msgId = STR_NAME_INVALID_MIXED_SCRIPTS;
                    break;
                case NAME_REJECT_EMPTY:
                    msgId = STR_NAME_INVALID_EMPTY;
                    break;
                case NAME_REJECT_INVALID:
                    msgId = STR_NAME_INVALID_CHARS;
                    break;
                default:
                    fprintf(stderr, "[UDP CLIENT] PACKET_NAME_CHANGE_REJECT: unknown reasonCode=%u\n", reasonCode);
                    msgId = STR_NAME_INVALID_CHARS;
                    break;
            }
            rendered = langGetText(msgId);
            rendBuf[0] = '\0';
            if (rendered) {
                strncpy(rendBuf, rendered, sizeof(rendBuf) - 1);
                rendBuf[sizeof(rendBuf) - 1] = '\0';
            }
            if (c->clientSim->inLobby) {
                clientSimAppendLobbyChat(c->clientSim, "Server", rendBuf);
            } else {
                clientSimNetStatusMessage(c->clientSim, rendBuf);
            }
        }
        break;

    case PACKET_CHAT_BROADCAST:
        /* Chat broadcast â€” wire format depends on fromPlayer (see netpacks.h):
         *   < MAX_TANKS  : player-to-player chat, payload is plain message
         *   == 0xFF      : server localized, payload is langid + args
         *   == 0xFE      : server raw English (transitional), payload is plain message
         */
        if (len > PACKET_HEADER_SIZE + 2) {
            uint8_t fromPlayer = buf[PACKET_HEADER_SIZE];
            if (fromPlayer == 0xFF) {
                /* Localized server message: decode and render. */
                langid id = 0;
                MessageArgs args;
                if (decodeLocalizedPayload(buf, len, PACKET_HEADER_SIZE + 2,
                                           &id, &args)) {
                    const char *rendered = langGetTextFmt(id, &args);
                    if (rendered && rendered[0]) {
                        if (c->clientSim->inLobby) {
                            clientSimAppendLobbyChat(c->clientSim, "Server",
                                                     rendered);
                        } else {
                            clientSimNetStatusMessage(c->clientSim, (char *)rendered);
                        }
                    }
                }
            } else if (fromPlayer == 0xFE || fromPlayer >= MAX_TANKS) {
                /* Raw English server message (legacy / un-localized ops). */
                int msgLen = len - PACKET_HEADER_SIZE - 2;
                char message[PACKET_MAX_CHAT_MESSAGE + 1];
                if (msgLen > PACKET_MAX_CHAT_MESSAGE) msgLen = PACKET_MAX_CHAT_MESSAGE;
                memcpy(message, buf + PACKET_HEADER_SIZE + 2, msgLen);
                message[msgLen] = '\0';
                if (c->clientSim->inLobby) {
                    clientSimAppendLobbyChat(c->clientSim, "Server", message);
                } else {
                    clientSimNetStatusMessage(c->clientSim, message);
                }
            } else {
                /* Player-to-player chat: payload is plain message bytes. */
                int msgLen = len - PACKET_HEADER_SIZE - 2;
                char message[PACKET_MAX_CHAT_MESSAGE + 1];
                if (msgLen > PACKET_MAX_CHAT_MESSAGE) msgLen = PACKET_MAX_CHAT_MESSAGE;
                memcpy(message, buf + PACKET_HEADER_SIZE + 2, msgLen);
                message[msgLen] = '\0';
                clientSimIncomingMessage(c->clientSim, fromPlayer, message);
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
            case ALLIANCE_EVENT_REQUEST: {
                ControlEvent evt = { .type = CTRL_ALLIANCE_REQUEST };
                evt.u.allianceRequest.fromPlayer = fromPlayer;
                evt.u.allianceRequest.toPlayer = toPlayer;
                clientSimApplyControl(c->clientSim, &evt);
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
            }
            case ALLIANCE_EVENT_ACCEPT: {
                ControlEvent evt = { .type = CTRL_ALLIANCE_ACCEPT };
                evt.u.allianceAccept.acceptedBy = fromPlayer;
                evt.u.allianceAccept.newMember = toPlayer;
                clientSimApplyControl(c->clientSim, &evt);
                break;
            }
            case ALLIANCE_EVENT_LEAVE: {
                ControlEvent evt = { .type = CTRL_ALLIANCE_LEAVE };
                evt.u.allianceLeave.playerNum = fromPlayer;
                clientSimApplyControl(c->clientSim, &evt);
                break;
            }
            }
        }
        break;

    case PACKET_SERVER_SHUTDOWN: {
        ControlEvent evt = { .type = CTRL_SERVER_SHUTDOWN };
        clientSimApplyControl(c->clientSim, &evt);
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "PACKET_SERVER_SHUTDOWN received -> SERVER_SHUTDOWN");
        c->joinState = UDP_CLIENT_SERVER_SHUTDOWN;
        break;
    }

    case PACKET_LOBBY_STATE:
        /* Full lobby snapshot (variable-length per slot):
         *   [header 8] [serverState 1]
         *   16 slots, each:
         *     [connected 1]
         *     If connected:
         *       [nameLen 1] [name nameLen UTF-8 bytes (no NUL)]
         *       [teamNumber 1] [ready 1] [isBot 1] [pingMs 2]
         *       [countryCode 2] [clientType 1] [clientFlags 1]
         *   Game settings tail:
         *     [mapName 36] [gameType 1] [hiddenMines 1] [aiType 1]
         *     [gameLength 4] [pillCount 1] [baseCount 1] [startCount 1]
         *     [mapSkipAvailable 1] */
        if (len > UDP_MAX_PAYLOAD) {
            fprintf(stderr, "[UDP CLIENT] LOBBY_STATE oversized: len=%d\n", len);
            break;
        }
        {
            int pos = PACKET_HEADER_SIZE;
            int i;
            uint8_t serverState;
            ClientLobbySlot tmpSlots[MAX_TANKS];
            bool decodeOk = true;

            /* Need at least serverState byte. */
            if (pos + 1 > len) {
                fprintf(stderr, "[UDP CLIENT] LOBBY_STATE truncated (no serverState): len=%d\n", len);
                break;
            }
            serverState = buf[pos++];

            memset(tmpSlots, 0, sizeof(tmpSlots));
            for (i = 0; i < MAX_TANKS && decodeOk; i++) {
                /* connected flag */
                if (pos + 1 > len) {
                    fprintf(stderr, "[UDP CLIENT] LOBBY_STATE truncated at slot %d connected\n", i);
                    decodeOk = false;
                    break;
                }
                tmpSlots[i].connected = buf[pos++] ? true : false;
                if (!tmpSlots[i].connected) {
                    /* Disconnected slots carry no further bytes. */
                    continue;
                }
                /* nameLen */
                if (pos + 1 > len) {
                    fprintf(stderr, "[UDP CLIENT] LOBBY_STATE truncated at slot %d nameLen\n", i);
                    decodeOk = false;
                    break;
                }
                {
                    uint8_t nameLen = buf[pos++];
                    if (nameLen > PACKET_MAX_PLAYER_NAME - 1) {
                        fprintf(stderr, "[UDP CLIENT] LOBBY_STATE slot %d nameLen=%u exceeds %d\n",
                                i, nameLen, PACKET_MAX_PLAYER_NAME - 1);
                        decodeOk = false;
                        break;
                    }
                    /* name (nameLen bytes) + 9 bytes of fixed slot fields. */
                    if (pos + nameLen + 9 > len) {
                        fprintf(stderr, "[UDP CLIENT] LOBBY_STATE truncated at slot %d body\n", i);
                        decodeOk = false;
                        break;
                    }
                    if (nameLen > 0) {
                        memcpy(tmpSlots[i].playerName, buf + pos, nameLen);
                    }
                    tmpSlots[i].playerName[nameLen] = '\0';
                    pos += nameLen;
                }
                tmpSlots[i].teamNumber = buf[pos++];
                tmpSlots[i].ready = buf[pos++] ? true : false;
                tmpSlots[i].isBot = buf[pos++] ? true : false;
                tmpSlots[i].pingMs = (uint16_t)(buf[pos] << 8 | buf[pos + 1]);
                pos += 2;
                tmpSlots[i].countryCode[0] = (char)buf[pos++];
                tmpSlots[i].countryCode[1] = (char)buf[pos++];
                tmpSlots[i].countryCode[2] = '\0';
                tmpSlots[i].clientType  = buf[pos++];
                tmpSlots[i].clientFlags = buf[pos++];
                if (tmpSlots[i].clientType >= CLIENT_TYPE_COUNT)
                    tmpSlots[i].clientType = CLIENT_TYPE_UNKNOWN;
            }
            if (!decodeOk) {
                break;
            }
            /* Settings tail must be exactly LOBBY_SETTINGS_SIZE bytes. */
            if (pos + LOBBY_SETTINGS_SIZE > len) {
                fprintf(stderr, "[UDP CLIENT] LOBBY_STATE truncated at settings tail: pos=%d len=%d\n",
                        pos, len);
                break;
            }

            /* Commit decoded slots. */
            for (i = 0; i < MAX_TANKS; i++) {
                {
                    ControlEvent slotEvt = { .type = CTRL_LOBBY_SLOT };
                    slotEvt.u.lobbySlot.playerNum = (BYTE)i;
                    slotEvt.u.lobbySlot.slot = tmpSlots[i];
                    clientSimApplyControl(c->clientSim, &slotEvt);
                }
                if (tmpSlots[i].clientFlags & (PLAYER_FLAG_WBN_VERIFIED | PLAYER_FLAG_WBN_STEAM_LINKED)) {
                    WB_LOG_DEBUG(WB_LOG_CAT_NET, "[WBN LOBBY] slot %d wbn=%d steam=%d",
                            i,
                            (tmpSlots[i].clientFlags & PLAYER_FLAG_WBN_VERIFIED) ? 1 : 0,
                            (tmpSlots[i].clientFlags & PLAYER_FLAG_WBN_STEAM_LINKED) ? 1 : 0);
                }
            }
            /* Game settings tail */
            {
                ControlEvent settingsEvt = { .type = CTRL_LOBBY_SETTINGS };
                memset(settingsEvt.u.lobbySettings.mapName, 0, MAP_STR_SIZE);
                strncpy(settingsEvt.u.lobbySettings.mapName,
                        (const char *)(buf + pos), MAP_STR_SIZE - 1);
                pos += MAP_STR_SIZE;
                settingsEvt.u.lobbySettings.lobbyGameType    = (gameType)buf[pos++];
                settingsEvt.u.lobbySettings.lobbyHiddenMines = buf[pos++] ? true : false;
                settingsEvt.u.lobbySettings.lobbyAiType      = buf[pos++];
                settingsEvt.u.lobbySettings.lobbyTimeLimit   = (int32_t)unpackU32(buf + pos);
                pos += 4;
                settingsEvt.u.lobbySettings.lobbyPillCount   = buf[pos++];
                settingsEvt.u.lobbySettings.lobbyBaseCount   = buf[pos++];
                settingsEvt.u.lobbySettings.lobbyStartCount  = buf[pos++];
                settingsEvt.u.lobbySettings.mapSkipAvailable = buf[pos++] ? true : false;
                /* Map server state to client netStatus — preserve countdown state */
                if (serverState == 1) { /* serverStateCountdown */
                    settingsEvt.u.lobbySettings.netStat = netLobbyCountdown;
                } else {
                    settingsEvt.u.lobbySettings.netStat = netLobby;
                }
                settingsEvt.u.lobbySettings.inLobby = true;
                clientSimApplyControl(c->clientSim, &settingsEvt);
            }

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
                !(c->clientSim->lobbySlots[c->playerNum].clientFlags &
                  PLAYER_FLAG_WBN_VERIFIED)) {
                if (!c->wbnReauthSent) {
                    c->wbnReauthSent = TRUE;
                    /* Inline re-auth send (we have ctx, not Transport*) */
                    {
                        uint8_t ra[PACKET_HEADER_SIZE + WBN_TOKEN_WIRE_LEN];
                        packHeader(ra, PACKET_WBN_REAUTH, c->outSequence++);
                        memcpy(ra + PACKET_HEADER_SIZE, c->wbnToken, WBN_TOKEN_WIRE_LEN);
                        udpClientSendTo(c, ra, sizeof(ra));
                    }
                    WB_LOG_INFO(WB_LOG_CAT_NET, "[WBN] Sent re-auth for slot %d", c->playerNum);
                }
            } else {
                /* Flag was restored or not needed â€” reset for next round */
                c->wbnReauthSent = FALSE;
            }
        }
        break;

    case PACKET_LOBBY_UPDATE:
        /* Single-player delta (variable length):
         *   [header 8] [playerNum 1] [connected 1]
         *   If connected:
         *     [nameLen 1] [name nameLen UTF-8 bytes] [teamNumber 1] [ready 1]
         *     [isBot 1] [pingMs 2] [countryCode 2] [clientType 1] [clientFlags 1] */
        if (len > UDP_MAX_PAYLOAD) {
            fprintf(stderr, "[UDP CLIENT] LOBBY_UPDATE oversized: len=%d\n", len);
            break;
        }
        {
            int pos = PACKET_HEADER_SIZE;
            uint8_t playerNum;
            ClientLobbySlot tmp;
            ControlEvent evt = { .type = CTRL_LOBBY_SLOT };

            /* playerNum + connected flag (2 bytes minimum). */
            if (pos + 2 > len) {
                fprintf(stderr, "[UDP CLIENT] LOBBY_UPDATE truncated header: len=%d\n", len);
                break;
            }
            playerNum = buf[pos++];
            if (playerNum >= MAX_TANKS) {
                fprintf(stderr, "[UDP CLIENT] LOBBY_UPDATE bad playerNum=%u\n", playerNum);
                break;
            }
            memset(&tmp, 0, sizeof(tmp));
            tmp.connected = buf[pos++] ? true : false;
            if (tmp.connected) {
                uint8_t nameLen;
                if (pos + 1 > len) {
                    fprintf(stderr, "[UDP CLIENT] LOBBY_UPDATE truncated at nameLen\n");
                    break;
                }
                nameLen = buf[pos++];
                if (nameLen > PACKET_MAX_PLAYER_NAME - 1) {
                    fprintf(stderr, "[UDP CLIENT] LOBBY_UPDATE nameLen=%u exceeds %d\n",
                            nameLen, PACKET_MAX_PLAYER_NAME - 1);
                    break;
                }
                if (pos + nameLen + 9 > len) {
                    fprintf(stderr, "[UDP CLIENT] LOBBY_UPDATE truncated body for slot %u\n", playerNum);
                    break;
                }
                if (nameLen > 0) {
                    memcpy(tmp.playerName, buf + pos, nameLen);
                }
                tmp.playerName[nameLen] = '\0';
                pos += nameLen;
                tmp.teamNumber = buf[pos++];
                tmp.ready = buf[pos++] ? true : false;
                tmp.isBot = buf[pos++] ? true : false;
                tmp.pingMs = (uint16_t)(buf[pos] << 8 | buf[pos + 1]);
                pos += 2;
                tmp.countryCode[0] = (char)buf[pos++];
                tmp.countryCode[1] = (char)buf[pos++];
                tmp.countryCode[2] = '\0';
                tmp.clientType  = buf[pos++];
                tmp.clientFlags = buf[pos++];
                if (tmp.clientType >= CLIENT_TYPE_COUNT)
                    tmp.clientType = CLIENT_TYPE_UNKNOWN;
            }
            evt.u.lobbySlot.playerNum = playerNum;
            evt.u.lobbySlot.slot = tmp;
            clientSimApplyControl(c->clientSim, &evt);
        }
        break;

    case PACKET_COUNTDOWN:
        /* [header 8] [secondsRemaining 1] */
        if (len >= PACKET_HEADER_SIZE + 1) {
            ControlEvent evt = { .type = CTRL_GAME_PHASE };
            evt.u.gamePhase.phase = CTRL_PHASE_COUNTDOWN;
            evt.u.gamePhase.countdownSeconds = buf[PACKET_HEADER_SIZE];
            clientSimApplyControl(c->clientSim, &evt);
        }
        break;

    case PACKET_GAME_START:
        /* [header 8] */
        {
            ControlEvent evt = { .type = CTRL_GAME_PHASE };
            evt.u.gamePhase.phase = CTRL_PHASE_RUNNING;
            evt.u.gamePhase.countdownSeconds = 0;
            clientSimApplyControl(c->clientSim, &evt);
        }
        /* Reset input ring so stale inputs from the previous game
         * are not sent as redundant packets in the new game. */
        c->inputRingCount = 0;
        /* Reset reliable event acks so they match the server's reset queues.
         * Stale events from the previous game must not be applied to
         * the freshly-loaded map. */
        c->reliableEventAck = 1;
        c->mapEventAck = 1;
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
            /* else: time limit or other end condition â€” no winner */

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

        {
            ControlEvent phaseEvt = { .type = CTRL_GAME_PHASE };
            phaseEvt.u.gamePhase.phase = CTRL_PHASE_GAME_OVER;
            phaseEvt.u.gamePhase.countdownSeconds = 0;
            clientSimApplyControl(c->clientSim, &phaseEvt);
            ControlEvent overEvt = { .type = CTRL_GAME_OVER };
            clientSimApplyControl(c->clientSim, &overEvt);
        }

        if (c->clientSim->inLobby) {
            /* Reset timeout tracking — the server won't send snapshots
             * during gameOver countdown, and the client's catch-up loop
             * advances localTick rapidly which can trigger a spurious
             * timeout before the game loop exits to the lobby. */
            c->lastSnapshotTick = c->localTick;
        } else {
            c->joinState = UDP_CLIENT_SERVER_SHUTDOWN;
        }
        break;

    case PACKET_LOBBY_MAP_CHANGE: {
        /* [header 8] – server loaded a new map; reset to re-download */
        ControlEvent evt = { .type = CTRL_LOBBY_MAP_CHANGE };
        clientSimApplyControl(c->clientSim, &evt);
        c->joinState = UDP_CLIENT_JOINING;
        c->joinAttempts = 0;
        c->ticksSinceJoinSent = JOIN_RETRY_INTERVAL; /* send immediately */
        break;
    }

    case PACKET_BALANCE_PROPOSAL:
        /* [header 8] [teamForSlot Ã— 16] */
        if (len >= PACKET_HEADER_SIZE + 16) {
            ControlEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_BALANCE_PROPOSAL;
            memcpy(evt.u.balanceProposal.teamForSlot, buf + PACKET_HEADER_SIZE, MAX_TANKS);
            clientSimApplyControl(c->clientSim, &evt);
        }
        break;

    case PACKET_MAP_SKIP_STATE:
        /* [header 8] [votes: 16 bytes, one per slot, 0 or 1] */
        if (len >= PACKET_HEADER_SIZE + MAX_TANKS) {
            ControlEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_MAP_SKIP_STATE;
            memcpy(evt.u.mapSkipState.votes, buf + PACKET_HEADER_SIZE, MAX_TANKS);
            clientSimApplyControl(c->clientSim, &evt);
        }
        break;

    /* â”€â”€ Layout A lobby â€” server â†’ client broadcasts â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */
    case PACKET_LOBBY_SETTING_CHG:
        /* [header 8] [settingType 1] [valueLen 1] [value valueLen] */
        if (len >= PACKET_HEADER_SIZE + 2) {
            uint8_t st = buf[PACKET_HEADER_SIZE];
            uint8_t vl = buf[PACKET_HEADER_SIZE + 1];
            const uint8_t *v = buf + PACKET_HEADER_SIZE + 2;
            if (len >= PACKET_HEADER_SIZE + 2 + vl && vl <= 32) {
                switch (st) {
                    case LST_GAME_TYPE:
                        if (vl == 1) c->clientSim->lobbyGameType = (gameType)v[0];
                        break;
                    case LST_HIDDEN_MINES:
                        if (vl == 1) c->clientSim->lobbyHiddenMines = v[0] != 0;
                        break;
                    case LST_AI_POLICY:
                        if (vl == 1) c->clientSim->lobbyAiType = v[0];
                        break;
                    case LST_TIME_LIMIT:
                        /* Bool â€” preserved separately on the client; lobbyTimeLimit
                         * itself is the tick-encoded length, set via TIME_MINUTES. */
                        break;
                    case LST_TIME_MINUTES:
                        if (vl == 2) {
                            uint16_t mins = (uint16_t)((v[0] << 8) | v[1]);
                            c->clientSim->lobbyTimeLimit =
                                (int32_t)mins * 60 * 50;  /* mins â†’ seconds â†’ ticks */
                        }
                        break;
                    case LST_AUTO_LOCK_ON_GAME:
                        if (vl == 1) c->clientSim->lobbyAutoLockOnGameStart = v[0] != 0;
                        break;
                    default: /* unknown setting type â€” ignore (forward-compat) */ break;
                }
            }
        }
        break;

    case PACKET_LOBBY_OPEN_HOST_CHG:
        if (len >= PACKET_HEADER_SIZE + 1) {
            c->clientSim->lobbyOpenHost = buf[PACKET_HEADER_SIZE] != 0;
        }
        break;

    case PACKET_LOBBY_TEAM_META_CHG:
        /* [header 8] [teamId 1] [color 1] [namingPool 1] [nameLen 1] [name N] */
        if (len >= PACKET_HEADER_SIZE + 4) {
            uint8_t teamId  = buf[PACKET_HEADER_SIZE + 0];
            uint8_t color   = buf[PACKET_HEADER_SIZE + 1];
            uint8_t pool    = buf[PACKET_HEADER_SIZE + 2];
            uint8_t nameLen = buf[PACKET_HEADER_SIZE + 3];
            if (teamId > 0 && teamId < 16 && nameLen <= 31 &&
                len >= PACKET_HEADER_SIZE + 4 + nameLen) {
                c->clientSim->lobbyTeamColor[teamId] = color;
                c->clientSim->lobbyTeamPool[teamId]  = pool;
                memset(c->clientSim->lobbyTeamName[teamId], 0, 32);
                if (nameLen > 0) {
                    memcpy(c->clientSim->lobbyTeamName[teamId],
                           buf + PACKET_HEADER_SIZE + 4, nameLen);
                }
                /* TEAM_META broadcasts (in_use=1) and TEAM_CLEAR (sent
                 * with teams[id] zeroed, so nameLen=0+color=0+pool=0)
                 * use the same packet â€” derive in_use from nameLen
                 * being non-zero OR color being explicitly set. */
                c->clientSim->lobbyTeamInUse[teamId] =
                    (nameLen > 0 || color != 0 || pool != 0) ? 1 : 0;
            }
        }
        break;

    case PACKET_LOBBY_BOT_CONFIG_CHG:
        /* [header 8] [slot 1] [difficulty 1] [personality 1]
         *   [nameLen 1] [name N]. Bot name change also lands in the
         *   slot's lobbySlots entry on the next LOBBY_UPDATE. */
        if (len >= PACKET_HEADER_SIZE + 4) {
            uint8_t slot = buf[PACKET_HEADER_SIZE + 0];
            uint8_t diff = buf[PACKET_HEADER_SIZE + 1];
            uint8_t pers = buf[PACKET_HEADER_SIZE + 2];
            if (slot < 16) {
                c->clientSim->lobbyBotDifficulty[slot]  = diff;
                c->clientSim->lobbyBotPersonality[slot] = pers;
            }
        }
        break;

    case PACKET_LOBBY_BRAIN_LIST: {
        /* [header 8] [count 1]
         * per entry: [nameLen 1][name][verLen 1][ver][pathLen 1][path] */
        if (len < PACKET_HEADER_SIZE + 1) break;
        int pos = PACKET_HEADER_SIZE;
        uint8_t cnt = buf[pos++];
        BrainList *bl = &c->clientSim->lobbyBrainList;
        memset(bl, 0, sizeof(*bl));
        if (cnt > BRAIN_LIST_MAX) cnt = BRAIN_LIST_MAX;
        for (int i = 0; i < cnt && pos < len; i++) {
            if (pos + 1 > len) break;
            uint8_t nlen = buf[pos++];
            if (nlen >= BRAIN_LIST_NAME_LEN || pos + nlen > len) break;
            memcpy(bl->entries[i].name, buf + pos, nlen);
            bl->entries[i].name[nlen] = '\0';
            pos += nlen;
            if (pos + 1 > len) break;
            uint8_t vlen = buf[pos++];
            if (vlen >= BRAIN_LIST_VER_LEN || pos + vlen > len) break;
            memcpy(bl->entries[i].version, buf + pos, vlen);
            bl->entries[i].version[vlen] = '\0';
            pos += vlen;
            if (pos + 1 > len) break;
            uint8_t plen = buf[pos++];
            if (plen >= BRAIN_LIST_PATH_LEN || pos + plen > len) break;
            memcpy(bl->entries[i].path, buf + pos, plen);
            bl->entries[i].path[plen] = '\0';
            pos += plen;
            bl->count++;
        }
        break;
    }

    case PACKET_LOBBY_BOT_BRAIN_CHG: {
        /* [header 8] [slot 1] [pathLen 1] [path N]. */
        if (len < PACKET_HEADER_SIZE + 2) break;
        uint8_t slot = buf[PACKET_HEADER_SIZE + 0];
        uint8_t plen = buf[PACKET_HEADER_SIZE + 1];
        if (slot >= MAX_TANKS) break;
        if (plen >= sizeof(c->clientSim->lobbyBotBrain[0])) break;
        if (len < PACKET_HEADER_SIZE + 2 + plen) break;
        memset(c->clientSim->lobbyBotBrain[slot], 0,
               sizeof(c->clientSim->lobbyBotBrain[slot]));
        if (plen > 0) {
            memcpy(c->clientSim->lobbyBotBrain[slot],
                   buf + PACKET_HEADER_SIZE + 2, plen);
        }
        break;
    }

    case PACKET_LOBBY_REJECT:
        /* [header 8] [origPacket 1] [reasonCode 1] â€” server rejected
         * one of our lobby commands. Stored for the UI to surface as
         * a toast or status line. */
        if (len >= PACKET_HEADER_SIZE + 2) {
            c->clientSim->lobbyLastRejectPacket = buf[PACKET_HEADER_SIZE];
            c->clientSim->lobbyLastRejectReason = buf[PACKET_HEADER_SIZE + 1];
        }
        break;

    case PACKET_LOBBY_AUTO_UNREADY:
        /* No payload. Server cleared everyone's ready flag; clients
         * mirror that locally so the Ready button restates "Click to
         * Ready". Server also broadcasts individual LOBBY_UPDATEs for
         * any slot that had a readyâ†’unready transition, so this is a
         * defensive belt-and-braces sweep. */
        {
            int i;
            for (i = 0; i < MAX_TANKS; i++) {
                c->clientSim->lobbySlots[i].ready = false;
            }
        }
        break;

    case PACKET_PUNCH_REQUEST_ACK:
        /* Tracker acked our PUNCH_REQUEST. Status byte at PACKET_HEADER_SIZE
         * could drive UX someday; for now just consume so it doesn't fall
         * into the unknown-packet warning path. */
        break;

    default:
        break;
    }
}

/* Send a PACKET_PUNCH_REQUEST to the configured tracker over the join
 * socket so the tracker learns our reflexive address as seen by the
 * same mapping the host's punch packet will land on. Wire body is the
 * host (target) IP+port the tracker should forward to. */
static void udpClientSendPunchRequest(TransportUdpClientCtx *c) {
    struct sockaddr_in dest;
    struct hostent *he;
    uint8_t buf[PACKET_HEADER_SIZE + 6];

    if (c->sock == INVALID_SOCKET) return;
    if (c->trackerAddr[0] == '\0') return;
    he = gethostbyname(c->trackerAddr);
    if (he == NULL) return;

    packHeader(buf, PACKET_PUNCH_REQUEST, c->outSequence++);
    memcpy(buf + PACKET_HEADER_SIZE, &c->targetIp.s_addr, 4);
    packU16(buf + PACKET_HEADER_SIZE + 4, c->targetPort);

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    memcpy(&dest.sin_addr, he->h_addr_list[0], he->h_length);
    dest.sin_port = htons(c->trackerPort);
    sendto(c->sock, (const char *)buf, sizeof(buf), 0,
           (const struct sockaddr *)&dest, sizeof(dest));
}

/* Client tick: receive packets from server, handle join flow, ping */
static bool udpClientTick(void *ctx) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;
    uint8_t buf[UDP_MAX_PAYLOAD];
    struct sockaddr_in fromAddr;
    int len;

    c->localTick++;

    /* Roll over PPS counters every second (100 ticks at 10ms/tick) */
    if (c->localTick - c->ppsWindowStart >= 100) {
        c->ppsRecv = c->packetsRecvThisSec;
        c->ppsSent = c->packetsSentThisSec;
        c->bpsRecv = c->bytesRecvThisSec;
        c->bpsSent = c->bytesSentThisSec;
        c->packetsRecvThisSec = 0;
        c->packetsSentThisSec = 0;
        c->bytesRecvThisSec = 0;
        c->bytesSentThisSec = 0;
        c->ppsWindowStart = c->localTick;
    }

    /* Receive all pending packets from the wire */
    while ((len = udpRecvFrom(c->sock, buf, sizeof(buf), &fromAddr)) > 0) {
        udpClientProcessPacket(c, buf, len);
    }

    /* Handle join handshake â€” send/resend join requests */
    if (c->joinState == UDP_CLIENT_JOINING) {
        c->ticksSinceJoinSent++;
        if (c->ticksSinceJoinSent >= JOIN_RETRY_INTERVAL) {
            if (c->joinAttempts >= JOIN_MAX_RETRIES) {
                WB_LOG_WARN(WB_LOG_CAT_NET,
                    "join handshake exhausted: attempts=%d max=%d -> ERROR",
                    (int)c->joinAttempts, (int)JOIN_MAX_RETRIES);
                c->joinState = UDP_CLIENT_ERROR;
            } else {
                uint8_t jbuf[PACKET_HEADER_SIZE + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3 + WBN_TOKEN_WIRE_LEN + 1 + 2];
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
                /* Flags byte: bit 0 = wantRejoin */
                jbuf[joffset++] = c->wantRejoin ? 0x01 : 0x00;
                jbuf[joffset++] = bolo_detect_client_type();
                {
                    uint8_t clientHints = 0;
#ifdef HAVE_STEAM
                    clientHints |= PLAYER_FLAG_STEAM_BUILD;
#endif
                    if (bolo_steam_has_supporter_dlc()) clientHints |= PLAYER_FLAG_SUPPORTER;
                    jbuf[joffset++] = clientHints;
                }
                /* Join requests bypass delay â€” they're control plane */
                udpClientSendTo(c, jbuf, joffset);
                WB_LOG_DEBUG(WB_LOG_CAT_NET,
                    "join request sent: attempt=%d/%d to=%s:%u name='%s'",
                    (int)(c->joinAttempts + 1), (int)JOIN_MAX_RETRIES,
                    inet_ntoa(c->serverAddr.sin_addr),
                    (unsigned)ntohs(c->serverAddr.sin_port),
                    c->playerName);
                c->joinAttempts++;
                c->ticksSinceJoinSent = 0;

                /* On the second JOIN attempt with no response, kick off
                 * the punch fallback. Only fires once per session â€” once
                 * the host's punch packet arrives, our subsequent
                 * JOIN_REQUEST retries will get through. Skipped when no
                 * tracker configured (LAN/manual-connect joiners). */
                if (!c->punchSent && c->joinAttempts >= 2 &&
                    c->trackerAddr[0] != '\0') {
                    udpClientSendPunchRequest(c);
                    c->punchSent = true;
                }
            }
        }
    }

    /* Periodic ping â€” bypasses delay so RTT measurement is accurate
     * (measures real network RTT, not simulated RTT) */
    if (c->joinState == UDP_CLIENT_CONNECTED) {
        if (c->localTick - c->lastPingSentTick >= PING_INTERVAL_TICKS) {
            uint8_t pbuf[PACKET_HEADER_SIZE + 8];
            packHeader(pbuf, PACKET_PING, c->outSequence++);
            packU32(pbuf + PACKET_HEADER_SIZE, SDL_GetTicks());
            packU32(pbuf + PACKET_HEADER_SIZE + 4, 0);  /* echo handled in PONG handler */
            udpClientSendTo(c, pbuf, sizeof(pbuf));
            c->lastPingSentTick = c->localTick;
        }

        /* Timeout: if no valid server packet received for CLIENT_TIMEOUT_TICKS,
         * the server has likely crashed or network is dead.
         * lastSnapshotTick is reset on any valid packet (line ~197), including
         * LOBBY_STATE broadcasts, so this works in all states. */
        if (c->lastSnapshotTick > 0 &&
            c->localTick - c->lastSnapshotTick >= CLIENT_TIMEOUT_TICKS) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                "client timeout: localTick=%u lastSnapshot=%u diff=%u "
                ">= CLIENT_TIMEOUT_TICKS=%d -> SERVER_SHUTDOWN",
                (unsigned)c->localTick,
                (unsigned)c->lastSnapshotTick,
                (unsigned)(c->localTick - c->lastSnapshotTick),
                (int)CLIENT_TIMEOUT_TICKS);
            c->joinState = UDP_CLIENT_SERVER_SHUTDOWN;
        }
    }

    return true;
}

static bool udpClientGetSnapshotVtable(void *ctx, BYTE clientIdx,
                                       SnapshotHeader *hdr,
                                       TankSnapshot *tanks, int maxTanks,
                                       ShellSnapshot *shells, int maxShells,
                                       TkExplosionSnapshot *tkExplosions, int maxTkExplosions,
                                       BaseSnapshot *bases, int maxBases,
                                       PillSnapshot *pills, int maxPills,
                                       GameEvent *events, int maxEvents) {
    TransportUdpClientCtx *c;
    int count;
    (void)clientIdx; /* UDP client doesn't need this â€” server sends per-client data */
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

    if (tkExplosions != NULL) {
        count = c->snapshotHdr.tkExplosionCount;
        if (count > maxTkExplosions) count = maxTkExplosions;
        memcpy(tkExplosions, c->snapshotTkExplosions, count * sizeof(TkExplosionSnapshot));
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
                                   const char *wbnToken,
                                   bool wantRejoin,
                                   const char *trackerAddr,
                                   unsigned short trackerPort) {
    Transport t;
    TransportUdpClientCtx *c;
    struct hostent *he;

    WB_LOG_INFO(WB_LOG_CAT_NET,
        "client connect: server=%s:%u name='%s' wantRejoin=%d "
        "wbnToken=%s tracker=%s:%u",
        serverAddr ? serverAddr : "(null)", (unsigned)serverPort,
        playerName ? playerName : "(null)",
        (int)wantRejoin,
        (wbnToken && *wbnToken) ? "yes" : "no",
        (trackerAddr && *trackerAddr) ? trackerAddr : "(none)",
        (unsigned)trackerPort);

    memset(&t, 0, sizeof(t));
    c = (TransportUdpClientCtx *)malloc(sizeof(TransportUdpClientCtx));
    memset(c, 0, sizeof(TransportUdpClientCtx));
    c->clientSim = clientSim;

    bolo_net_init();

    c->sock = createUdpSocket();
    if (c->sock == INVALID_SOCKET) {
        WB_LOG_ERROR(WB_LOG_CAT_NET, "client connect: createUdpSocket failed");
        c->joinState = UDP_CLIENT_ERROR;
        t.recordInput = udpClientRecordInput;
        t.sendInput = udpClientSendInput;
        t.kind = TRANSPORT_KIND_UDP_CLIENT;
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
            WB_LOG_DEBUG(WB_LOG_CAT_NET,
                "client connect: resolved %s -> %s",
                serverAddr,
                inet_ntoa(c->serverAddr.sin_addr));
        } else {
            WB_LOG_ERROR(WB_LOG_CAT_NET,
                "client connect: gethostbyname('%s') failed",
                serverAddr ? serverAddr : "(null)");
            c->joinState = UDP_CLIENT_ERROR;
            t.recordInput = udpClientRecordInput;
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

    c->wantRejoin = wantRejoin;

    c->trackerAddr[0] = '\0';
    if (trackerAddr != NULL) {
        strncpy(c->trackerAddr, trackerAddr, sizeof(c->trackerAddr) - 1);
    }
    c->trackerPort = trackerPort;
    c->targetIp    = c->serverAddr.sin_addr;
    c->targetPort  = serverPort;
    c->punchSent   = false;

    c->joinState = UDP_CLIENT_JOINING;
    c->joinAttempts = 0;
    c->ticksSinceJoinSent = JOIN_RETRY_INTERVAL; /* Send immediately on first tick */
    c->outSequence = 1;

    c->lastSnapshotSeq = 0;
    c->hasSnapshot = false;
    c->reliableEventAck = 1;  /* First valid seq is 1 */
    c->mapEventAck = 1;       /* First valid map event seq is 1 */
    c->localTick = 0;
    c->lastPingSentTick = 0;
    c->pingMs = 0;

    t.recordInput = udpClientRecordInput;
    t.sendInput = udpClientSendInput;
    t.kind = TRANSPORT_KIND_UDP_CLIENT;
    t.tick = udpClientTick;
    t.getSnapshot = udpClientGetSnapshotVtable;
    t.ctx = c;
    return t;
}

void transportUdpClientDestroy(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return;
    c = (TransportUdpClientCtx *)t->ctx;
    WB_LOG_INFO(WB_LOG_CAT_NET,
        "client destroy: joinState=%d localTick=%u lastSnapshot=%u",
        (int)c->joinState, (unsigned)c->localTick,
        (unsigned)c->lastSnapshotTick);
    if (c->sock != INVALID_SOCKET) {
        /* Send graceful quit packet to server before closing */
        if (c->joinState == UDP_CLIENT_CONNECTED) {
            uint8_t qbuf[PACKET_HEADER_SIZE];
            packHeader(qbuf, PACKET_QUIT, c->outSequence++);
            udpClientSendTo(c, qbuf, PACKET_HEADER_SIZE);
            WB_LOG_DEBUG(WB_LOG_CAT_NET, "client sent PACKET_QUIT to server");
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

void transportUdpClientGetNetStats(Transport *t, int *ppsRecv, int *ppsSent,
                                   int *bpsRecv, int *bpsSent, int *numErrors) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) {
        *ppsRecv = 0;
        *ppsSent = 0;
        *bpsRecv = 0;
        *bpsSent = 0;
        *numErrors = 0;
        return;
    }
    c = (TransportUdpClientCtx *)t->ctx;
    *ppsRecv = (int)c->ppsRecv;
    *ppsSent = (int)c->ppsSent;
    *bpsRecv = (int)c->bpsRecv;
    *bpsSent = (int)c->bpsSent;
    *numErrors = (int)c->netErrors;
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
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_CHAT_MESSAGE];
    int msgLen;
    int len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (message == NULL || message[0] == '\0') return;

    msgLen = (int)strlen(message);
    if (msgLen > PACKET_MAX_CHAT_MESSAGE) msgLen = PACKET_MAX_CHAT_MESSAGE;

    packHeader(buf, PACKET_CHAT_MESSAGE, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = destPlayer;
    memcpy(buf + PACKET_HEADER_SIZE + 1, message, msgLen);
    len = PACKET_HEADER_SIZE + 1 + msgLen;
    udpClientSendTo(c, buf, len);
}

/* Send a name change request to the server. */
void transportUdpClientSendNameChange(Transport *t, const char *newName) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (newName == NULL || newName[0] == '\0') return;

    packHeader(buf, PACKET_NAME_CHANGE, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    memset(buf + PACKET_HEADER_SIZE + 1, 0, PACKET_MAX_PLAYER_NAME);
    strncpy((char *)(buf + PACKET_HEADER_SIZE + 1), newName,
            PACKET_MAX_PLAYER_NAME - 1);
    udpClientSendTo(c, buf, sizeof(buf));
}

/* Send an alliance request to another player via server.
 * Wire: [header 8] [fromPlayer 1] [toPlayer 1] */
void transportUdpClientSendAllianceRequest(Transport *t, uint8_t toPlayer) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 2];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_ALLIANCE_REQUEST, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    buf[PACKET_HEADER_SIZE + 1] = toPlayer;
    udpClientSendTo(c, buf, sizeof(buf));
}

/* Send an alliance accept to server.
 * Wire: [header 8] [fromPlayer 1] [toPlayer 1]
 * fromPlayer = us (the accepter), toPlayer = who requested */
void transportUdpClientSendAllianceAccept(Transport *t, uint8_t toPlayer) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 2];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_ALLIANCE_ACCEPT, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    buf[PACKET_HEADER_SIZE + 1] = toPlayer;
    udpClientSendTo(c, buf, sizeof(buf));
}

/* Send a leave alliance request to server.
 * Wire: [header 8] [playerNum 1] */
void transportUdpClientSendAllianceLeave(Transport *t) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_ALLIANCE_LEAVE, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    udpClientSendTo(c, buf, sizeof(buf));
}

/* Send a lock toggle to the server.
 * Wire: [header 8] [allow 1] */
void transportUdpClientSendLockToggle(Transport *t, bool allow) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOCK_TOGGLE, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = allow ? 1 : 0;
    udpClientSendTo(c, buf, sizeof(buf));
}


/* ---- Client lobby send functions ---- */

void transportUdpClientSendTeamSet(Transport *t, uint8_t playerNum,
                                   uint8_t teamNumber) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 2];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_TEAM_SET, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = playerNum;
    buf[PACKET_HEADER_SIZE + 1] = teamNumber;
    udpClientSendTo(c, buf, sizeof(buf));
}

void transportUdpClientSendReady(Transport *t, bool ready) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 2];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_READY, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = c->playerNum;
    buf[PACKET_HEADER_SIZE + 1] = ready ? 1 : 0;
    udpClientSendTo(c, buf, sizeof(buf));
}

void transportUdpClientSendAddBot(Transport *t, uint8_t teamNumber,
                                  const char *brainPath,
                                  const char *botName) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 3 + BRAIN_LIST_PATH_LEN +
                PACKET_MAX_PLAYER_NAME];

    if (c->joinState != UDP_CLIENT_CONNECTED) {
        WB_LOG_INFO(WB_LOG_CAT_NET,
                    "[LOBBY] Client SendAddBot skipped: joinState=%d (not CONNECTED)",
                    (int)c->joinState);
        return;
    }

    if (brainPath == NULL) brainPath = "";
    if (botName   == NULL) botName   = "";
    int pathLen = (int)strlen(brainPath);
    int nameLen = (int)strlen(botName);
    if (pathLen >= BRAIN_LIST_PATH_LEN)    pathLen = BRAIN_LIST_PATH_LEN - 1;
    if (nameLen >= PACKET_MAX_PLAYER_NAME) nameLen = PACKET_MAX_PLAYER_NAME - 1;
    if (nameLen > 31)                      nameLen = 31;

    WB_LOG_INFO(WB_LOG_CAT_NET,
                "[LOBBY] Client -> server: ADD_BOT team=%u brain='%s' name='%s'",
                (unsigned)teamNumber, brainPath, botName);
    int pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_LOBBY_ADD_BOT, c->outSequence++);
    buf[pos++] = teamNumber;
    buf[pos++] = (uint8_t)pathLen;
    if (pathLen > 0) { memcpy(buf + pos, brainPath, pathLen); pos += pathLen; }
    buf[pos++] = (uint8_t)nameLen;
    if (nameLen > 0) { memcpy(buf + pos, botName, nameLen); pos += nameLen; }
    udpClientSendTo(c, buf, pos);
}

void transportUdpClientSendRemoveBot(Transport *t, uint8_t playerNum) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_REMOVE_BOT, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = playerNum;
    udpClientSendTo(c, buf, sizeof(buf));
}

void transportUdpClientSendWbnReauth(Transport *t) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + WBN_TOKEN_WIRE_LEN];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (c->wbnToken[0] == '\0') return;

    packHeader(buf, PACKET_WBN_REAUTH, c->outSequence++);
    memcpy(buf + PACKET_HEADER_SIZE, c->wbnToken, WBN_TOKEN_WIRE_LEN);
    udpClientSendTo(c, buf, sizeof(buf));
}

/* ---- Client balance send functions ---- */

void transportUdpClientSendBalanceRequest(Transport *t, uint8_t teamSize) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_BALANCE_REQUEST, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = teamSize;
    udpClientSendTo(c, buf, sizeof(buf));
}

void transportUdpClientSendBalanceApply(Transport *t) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_BALANCE_APPLY, c->outSequence++);
    udpClientSendTo(c, buf, sizeof(buf));
}

void transportUdpClientSendBalanceDismiss(Transport *t) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_BALANCE_DISMISS, c->outSequence++);
    udpClientSendTo(c, buf, sizeof(buf));
}

void transportUdpClientSendMapSkipVote(Transport *t) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_MAP_SKIP_VOTE, c->outSequence++);
    udpClientSendTo(c, buf, sizeof(buf));
}

/* â”€â”€ Layout A lobby commands â€” Client â†’ Server â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

void transportUdpClientSendLobbySetting(Transport *t,
                                        uint8_t settingType,
                                        const uint8_t *value, uint8_t valueLen) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    /* Header + type byte + len byte + max 32 bytes of value (currently
     * settings are 1-2 bytes; 32 leaves room for future LST_* values). */
    uint8_t buf[PACKET_HEADER_SIZE + 2 + 32];
    int len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (valueLen > 32) valueLen = 32;
    if (valueLen > 0 && value == NULL) return;

    packHeader(buf, PACKET_LOBBY_SET_SETTING, c->outSequence++);
    buf[PACKET_HEADER_SIZE]     = settingType;
    buf[PACKET_HEADER_SIZE + 1] = valueLen;
    if (valueLen > 0) memcpy(buf + PACKET_HEADER_SIZE + 2, value, valueLen);
    len = PACKET_HEADER_SIZE + 2 + valueLen;
    udpClientSendTo(c, buf, len);
}

void transportUdpClientSendLobbyOpenHost(Transport *t, bool openHost) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_OPEN_HOST, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = openHost ? 1 : 0;
    udpClientSendTo(c, buf, sizeof(buf));
}

void transportUdpClientSendLobbyTeamMeta(Transport *t, uint8_t teamId,
                                         uint8_t color, uint8_t namingPool,
                                         const char *name) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    /* Header + teamId + color + namingPool + nameLen + max 31 bytes
     * (LOBBY_TEAM_NAME_LEN-1, leaving room for caller terminator). */
    uint8_t buf[PACKET_HEADER_SIZE + 4 + 31];
    int nameLen, len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (name == NULL) name = "";

    nameLen = (int)strlen(name);
    if (nameLen > 31) nameLen = 31;

    packHeader(buf, PACKET_LOBBY_TEAM_META, c->outSequence++);
    buf[PACKET_HEADER_SIZE + 0] = teamId;
    buf[PACKET_HEADER_SIZE + 1] = color;
    buf[PACKET_HEADER_SIZE + 2] = namingPool;
    buf[PACKET_HEADER_SIZE + 3] = (uint8_t)nameLen;
    if (nameLen > 0) memcpy(buf + PACKET_HEADER_SIZE + 4, name, nameLen);
    len = PACKET_HEADER_SIZE + 4 + nameLen;
    udpClientSendTo(c, buf, len);
}

void transportUdpClientSendLobbyTeamClear(Transport *t, uint8_t teamId) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_TEAM_CLEAR, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = teamId;
    udpClientSendTo(c, buf, sizeof(buf));
}

void transportUdpClientSendLobbyBotConfig(Transport *t, uint8_t slot,
                                          uint8_t difficulty, uint8_t personality,
                                          const char *name) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    /* Header + slot + difficulty + personality + nameLen + max 31 bytes. */
    uint8_t buf[PACKET_HEADER_SIZE + 4 + 31];
    int nameLen, len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (name == NULL) name = "";

    nameLen = (int)strlen(name);
    if (nameLen > 31) nameLen = 31;

    packHeader(buf, PACKET_LOBBY_BOT_CONFIG, c->outSequence++);
    buf[PACKET_HEADER_SIZE + 0] = slot;
    buf[PACKET_HEADER_SIZE + 1] = difficulty;
    buf[PACKET_HEADER_SIZE + 2] = personality;
    buf[PACKET_HEADER_SIZE + 3] = (uint8_t)nameLen;
    if (nameLen > 0) memcpy(buf + PACKET_HEADER_SIZE + 4, name, nameLen);
    len = PACKET_HEADER_SIZE + 4 + nameLen;
    udpClientSendTo(c, buf, len);
}

void transportUdpClientSendLobbySetBotBrain(Transport *t, uint8_t slot,
                                            const char *brainPath) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 2 + BRAIN_LIST_PATH_LEN];
    int pathLen, len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (brainPath == NULL) brainPath = "";

    pathLen = (int)strlen(brainPath);
    if (pathLen >= BRAIN_LIST_PATH_LEN) pathLen = BRAIN_LIST_PATH_LEN - 1;

    packHeader(buf, PACKET_LOBBY_SET_BOT_BRAIN, c->outSequence++);
    buf[PACKET_HEADER_SIZE + 0] = slot;
    buf[PACKET_HEADER_SIZE + 1] = (uint8_t)pathLen;
    if (pathLen > 0) memcpy(buf + PACKET_HEADER_SIZE + 2, brainPath, pathLen);
    len = PACKET_HEADER_SIZE + 2 + pathLen;
    udpClientSendTo(c, buf, len);
}

void transportUdpClientSendLobbyKick(Transport *t, uint8_t slot) {
    if (!t || t->kind != TRANSPORT_KIND_UDP_CLIENT) return;
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_KICK, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = slot;
    udpClientSendTo(c, buf, sizeof(buf));
}
