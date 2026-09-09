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
 *Name:          Transport UDP Server
 *Filename:      transport_udp_server.c
 *Author:        John Morrison
 *Purpose:
 *  Server-side UDP network transport for multiplayer games.
 *    - Receives inputs from clients, applies to ServerSim.
 *    - Broadcasts per-player filtered snapshots each tick.
 *    - Manages connection lifecycle (join/leave/timeout).
 *    - Periodic ping/pong for latency measurement.
 *********************************************************/

#include "transport_udp_internal.h"
#include "transport_udp_server_internal.h" /* UdpServerState and the per-slot
                                            * types held in it */
#include "global.h"
#include "bases.h"
#include "pillbox.h"
#include "starts.h"
#include "players.h"
#include "client_enums.h"  /* aiType, gameType, sndEffects, updateType */
#include "viewport_types.h"  /* screen */
#include "messages.h"
#include "util.h"
#include "game_sim.h"
#include "server_sim.h"
#include "server_sim_internal.h" /* serverSimGameVoteToggle — T2 (sim co-owner) */
#include "server_lifecycle.h"
#include "control_event.h"
#include "lobby_bot_pools.h"
#include "client_sim_internal.h"  /* LOBBY_MAP_LIST_MAX cap shared with the wire */
#include "../common/md5.h"
#include "mapgen.h"
#include "brain_list_internal.h"   /* BRAIN_LIST_PATH_LEN — ADD_BOT pathLen bound */
#include "transport_control_codec.h"
#include "transport_command_codec.h"
#include "channel_mux.h"
#include "bulk_transfer.h"
#include "spectator_ring.h"
#include "../winbolonet/winbolonet_core.h"
#include "sounddist.h"
#include "bot_manager.h"
#include "server_sim_lifecycle.h"
#include "../common/wb_log.h"
#include "../common/mp_diag_log.h"
#include "net_impair.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

/* Human-readable terrain name for map-resync diagnostics. Covers the terrain
 * byte stored in mapItem, including the mine range (10-15). */
static const char *resyncTerrainName(BYTE t) {
    switch (t) {
        case DEEP_SEA:     return "DEEP_SEA";
        case BUILDING:     return "BUILDING";
        case RIVER:        return "RIVER";
        case SWAMP:        return "SWAMP";
        case CRATER:       return "CRATER";
        case ROAD:         return "ROAD";
        case FOREST:       return "FOREST";
        case RUBBLE:       return "RUBBLE";
        case GRASS:        return "GRASS";
        case HALFBUILDING: return "HALFBUILDING";
        case BOAT:         return "BOAT";
        case MINE_SWAMP:   return "MINE_SWAMP";
        case MINE_CRATER:  return "MINE_CRATER";
        case MINE_ROAD:    return "MINE_ROAD";
        case MINE_FOREST:  return "MINE_FOREST";
        case MINE_RUBBLE:  return "MINE_RUBBLE";
        case MINE_GRASS:   return "MINE_GRASS";
        default:           return "?";
    }
}

/* ================================================================
 * SERVER SIDE
 * ================================================================ */

/* Bounds on serving the last completed round's log (PACKET_ROUND_LOG_REQ).
 * The BulkSender's busy guard is per peer, so it bounds one client's byte
 * stream and nothing else: every client has its own sender holding its own
 * malloc'd copy of the blob, and sixteen simultaneous requests would be
 * sixteen simultaneous transfers. The concurrency cap is what supplies the
 * fleet-wide bound the guard does not (worst case 2 x ROUND_LOG_MAX_BYTES
 * resident); the interval and the per-round attempt ceiling bound how often
 * one client can ask. */
#define ROUND_LOG_MAX_CONCURRENT  2
#define ROUND_LOG_MIN_REQ_TICKS   100  /* 2s at 50 Hz, between requests */
#define ROUND_LOG_MAX_ATTEMPTS    3    /* transfers started per client, per round */

/* Bounds on the catch-up sweep that keeps a culled slot's copy of the terrain
 * (mapEventQueues below carries its output). One slot sweeps per
 * MAP_SWEEP_STRIDE sim ticks — the sim advances two ticks a frame, so each
 * slot comes up every five frames — and a sweep queues at most
 * MAP_SWEEP_MAX_EVENTS squares, well under RELIABLE_EVENT_BUFFER_SIZE so the
 * catch-up can never crowd out live changes. A screen's worth of stale ground
 * clears in a handful of sweeps. */
#define MAP_SWEEP_STRIDE      5
#define MAP_SWEEP_MAX_EVENTS 64

/* Minimum interval between map-download re-asks from one client.
 *
 * A re-ask is the most expensive thing one small datagram can ask this server
 * to do: it recompresses that slot's whole copy of the terrain, re-sends
 * JOIN_ACCEPT and re-bases the bulk channel, then streams the map again. The
 * first READY of a download is not a re-ask and is never held off — this
 * bounds only the restart path.
 *
 * It has to sit *under* the honest client's fastest re-ask, not outside it.
 * A client whose stream head was consumed before its buffers existed sees no
 * progress while the server thinks it is streaming, so its watchdog re-asks on
 * the short MAP_DL_READY_RESEND_TICKS threshold (~1s) and every one of those
 * lands here on the restart path. Holding those off would drop half of a
 * recovery the client only gets MAP_DL_MAX_RESTARTS attempts at — the wedge
 * this path exists to clear. Half a second serves every one of them and still
 * takes a client that asks in a tight loop from hundreds of restarts a second
 * down to two. */
#define MAP_REASK_MIN_TICKS 25  /* 0.5s at 50 Hz, between re-asks */

/* The one definition of the server transport's file-scope state; the
 * declaration lives in transport_udp_server_internal.h. */
UdpServerState udpServer;

/* Outbound datagram wrapper.  Every server->peer send routes through here
 * so the outbound impairment layer can delay/drop/reorder it.  When
 * impairment is disabled (or the datagram is too large for the queue),
 * the packet goes straight onto the wire — behaviourally identical to a
 * direct udpSendTo. */
void srvSendTo(const uint8_t *buf, int len,
               const struct sockaddr_in *addr) {
    if (netImpairEnabled(&srvImpairOut) &&
        netImpairOffer(&srvImpairOut, buf, len, addr, (uint64_t)SDL_GetTicks())) {
        return;
    }
    udpSendTo(udpServer.sock, buf, len, addr);
}

/* Handle input packet from a connected client */
static void serverHandleInput(const uint8_t *buf, int len,
                              const struct sockaddr_in *fromAddr,
                              ServerSim *sim) {
    int clientIdx;
    /* INPUT framing: [header 8][connId 8][count 1][25-byte inputs…]. */
    int pos = PACKET_HEADER_SIZE + 8;
    uint8_t inputCount;
    uint64_t connId = 0;
    bool rehome = false;
    int i;

    /* Match the session by connId first so a client whose NAT mapping rebound
     * keeps its slot; fall back to IP:port when the connId is absent (0) or
     * matches no slot. The connId is only present on a packet long enough to
     * hold it — a short/old frame leaves it 0 and takes the IP:port path. */
    if (len >= PACKET_HEADER_SIZE + 8) {
        connId = unpackConnId(buf + PACKET_HEADER_SIZE);
    }
    clientIdx = transportUdpServerFindByConnId(udpServer.clients, connId,
                                               fromAddr, &rehome);
    if (clientIdx >= 0) {
        if (rehome) {
            char oldAddr[32];
            snprintf(oldAddr, sizeof(oldAddr), "%s:%u",
                     inet_ntoa(udpServer.clients[clientIdx].addr.sin_addr),
                     (unsigned)ntohs(udpServer.clients[clientIdx].addr.sin_port));
            WB_LOG_INFO(WB_LOG_CAT_NET,
                "slot %d NAT rebind: %s -> %s:%u (connId match, re-homing)",
                clientIdx, oldAddr, inet_ntoa(fromAddr->sin_addr),
                (unsigned)ntohs(fromAddr->sin_port));
            udpServer.clients[clientIdx].addr = *fromAddr;
        }
    } else {
        clientIdx = serverFindClient(fromAddr);
    }
    if (clientIdx < 0) return; /* Unknown client */

    udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;

    /* Only process inputs during running state — silently discard otherwise */
    if (serverSimGetState(sim) != serverStateRunning) {
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

        /* Advance the reliable map-event ACK from this client.  Game events
         * ride CHANNEL_GAME with their own acks; the InputPacket carries no
         * game-event ack. */
        if (pkt.mapEventAck > udpServer.mapEventQueues[clientIdx].ackedSeq) {
            udpServer.mapEventQueues[clientIdx].ackedSeq = pkt.mapEventAck;
        }
        /* Control events ride CHANNEL_CONTROL; their acks arrive on the
         * channel frame trailer (ingested below), not in the input packet. */

        /* Only apply if this is a newer input than what we last processed */
        if (pkt.tick > serverSimGetLastProcessedInput(sim, clientIdx)) {
            if (udpServer.clients[clientIdx].inputsThisTick >= INPUT_REDUNDANCY_COUNT) break;
            serverSimApplyInput(sim, &pkt);
            udpServer.clients[clientIdx].inputsThisTick++;
        }
    }

    /* Anything past the inputs is the parallel channel layer's trailer. */
    if (pos < len &&
        channelRecvFrame(&udpServer.channelMux[clientIdx],
                         buf + pos, len - pos) >= 0) {
        udpServer.channelFramesRx[clientIdx]++;
        serverDrainBulk(sim, clientIdx);
    }
}

/* Handle ping from client — respond with pong */
static void serverHandlePing(const uint8_t *buf, int len,
                              const struct sockaddr_in *fromAddr) {
    uint8_t pongBuf[PACKET_HEADER_SIZE + 8];
    uint32_t clientTime;

    if (len < PACKET_HEADER_SIZE + 8) return;

    clientTime = unpackU32(buf + PACKET_HEADER_SIZE);

    /* Only respond to pings from connected clients — otherwise a
     * disconnected client keeps receiving pongs and never detects
     * that the server dropped it. */
    {
        uint32_t now = SDL_GetTicks();
        int clientIdx = serverFindClient(fromAddr);
        if (clientIdx < 0) return;

        /* Server-measured RTT: the second uint32 from the client is the server
         * timestamp we sent in the previous PONG, echoed back.  RTT = now - that. */
        {
            uint32_t echoedServerTime = unpackU32(buf + PACKET_HEADER_SIZE + 4);
            if (echoedServerTime > 0) {
                udpServer.clients[clientIdx].pingMs = (uint16_t)(now - echoedServerTime);
            }
        }
        udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;

        /* Echo-only PING (clientTime == 0): server already computed RTT above,
         * don't send a PONG back or it creates an infinite ping-pong loop. */
        if (clientTime == 0) return;

        udpServer.clients[clientIdx].lastPongSentMs = now;
        packHeader(pongBuf, PACKET_PONG, 0);
        packU32(pongBuf + PACKET_HEADER_SIZE, clientTime);
        packU32(pongBuf + PACKET_HEADER_SIZE + 4, now);
    }
    /* wire-only: per-client handshake (response to a single client's request) */
    srvSendTo(pongBuf, sizeof(pongBuf), fromAddr);
}

/* Build and send a snapshot to one client.
 * Uses serverSimBuildSnapshot() for all game state, then serializes
 * and appends reliable events from the per-client queue. */
static void serverSendSnapshot(ServerSim *sim, int clientIdx) {
    uint8_t buf[2048];
    int pos;
    int i;
    int countsPos;
    SnapshotHeader hdr;
    TankSnapshot tankSnaps[MAX_TANKS];
    ShellSnapshot shellSnaps[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot tkExplSnaps[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot baseSnaps[MAX_SNAPSHOT_BASES];
    PillSnapshot pillSnaps[MAX_SNAPSHOT_PILLS];
    GameEvent eventSnaps[MAX_SNAPSHOT_EVENTS];
    ClientEventQueue *mapQ = &udpServer.mapEventQueues[clientIdx];
    UdpServerClient *client = &udpServer.clients[clientIdx];

    /* Build snapshot from sim state (same code as local transport) */
    serverSimBuildSnapshot(sim, (BYTE)clientIdx, &hdr,
                           tankSnaps, MAX_TANKS,
                           shellSnaps, MAX_SNAPSHOT_SHELLS,
                           tkExplSnaps, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           baseSnaps, MAX_SNAPSHOT_BASES,
                           pillSnaps, MAX_SNAPSHOT_PILLS,
                           eventSnaps, MAX_SNAPSHOT_EVENTS,
                           false);

    /* Packet header */
    packHeader(buf, PACKET_STATE_SNAPSHOT, client->outSequence++);
    pos = PACKET_HEADER_SIZE;

    /* Snapshot header — we'll fill in counts after packing data.
     * Format: serverTick(4) + lastProcessedInput(4) + tankCount(1)
     * + shellCount(1) + tkExplosionCount(1)
     * + baseCount(1) + pillCount(1)
     * + mapChecksum(2) + returnToLobbyTicks(2) = SNAPSHOT_HEADER_WIRE_SIZE.
     * The static assert ties that constant to this field breakdown, and the
     * reserve below derives from it, so this packer and the client's size
     * guard can't drift. */
    BOLO_STATIC_ASSERT(SNAPSHOT_HEADER_WIRE_SIZE == 4 + 4 + 5 + 2 + 2,
                       snapshot_header_wire_size);
    packU32(buf + pos, hdr.serverTick);
    pos += 4;
    packU32(buf + pos, hdr.lastProcessedInput);
    pos += 4;
    countsPos = pos;
    /* Reserve the 5 count bytes + 2-byte mapChecksum + 2-byte
     * returnToLobbyTicks — the header bytes after the two u32s above. */
    pos += SNAPSHOT_HEADER_WIRE_SIZE - 8;

    /* Pack tank snapshots — variable length: a stub is 1 byte; a full entry is
     * a presence-mask-driven run of at most TANK_SNAPSHOT_WIRE_SIZE bytes
     * (packTankSnapshot returns the actual size, usually far smaller because
     * zero field groups are omitted). Reserve the conservative max here. */
    for (i = 0; i < hdr.tankCount; i++) {
        bool isStub = (tankSnaps[i].playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) != 0;
        int needed = isStub ? 1 : TANK_SNAPSHOT_WIRE_SIZE;
        if (pos + needed > (int)sizeof(buf)) {
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

    /* Pack tank explosion snapshots */
    for (i = 0; i < hdr.tkExplosionCount; i++) {
        if (pos + TK_EXPLOSION_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
            hdr.tkExplosionCount = (uint8_t)i;
            break;
        }
        pos += packTkExplosionSnapshot(buf + pos, &tkExplSnaps[i]);
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

    /* No snapshot reliable game-tail — game events ride CHANNEL_GAME. */

    /* Drain held map-change events onto reliable channel 1 (CHANNEL_MAP),
     * tagged with this slot's map generation: payload = [gen u32][GameEvent].
     * mapEventQueues stays the hold buffer — while a download or live resync is
     * in flight the freshly compressed blob already carries every change up to
     * the cut, and changes during the transfer sit undrained, so hold them
     * (gate on downloadComplete && !resyncInProgress) and flush once both gates
     * clear. Each successful channelSend advances ackedSeq to free the slot.
     * A full window defers the disconnect off this path, mirroring the
     * game-channel overflow. The snapshot no longer carries a map tail. */
    if (udpServer.mapDownload[clientIdx].downloadComplete &&
        !udpServer.mapDownload[clientIdx].resyncInProgress) {
        uint32_t seq;
        for (seq = mapQ->ackedSeq; seq < mapQ->nextSeq; seq++) {
            uint32_t idx = seq % RELIABLE_EVENT_BUFFER_SIZE;
            uint8_t mapMsg[4 + GAME_EVENT_MAX_WIRE_SIZE];
            int evLen;
            if (mapQ->buffer[idx].seq != seq) break; /* Buffer wrapped — stop */
            packU32(mapMsg, udpServer.mapGen[clientIdx]);
            evLen = packGameEvent(mapMsg + 4, &mapQ->buffer[idx].event);
            if (!channelSend(&udpServer.channelMux[clientIdx], CHANNEL_MAP,
                             mapMsg, (uint16_t)(4 + evLen))) {
                if (!udpServer.pendingSimRemove[clientIdx]) {
                    WB_LOG_ERROR(WB_LOG_CAT_NET,
                                 "map channel overflow for slot %d, deferring disconnect",
                                 clientIdx);
                    udpServer.pendingSimRemove[clientIdx] = true;
                }
                break;
            }
            mapQ->ackedSeq = seq + 1; /* Sent reliably — free the hold slot. */
        }
    }

    /* Control events ride reliable channel 2 (CHANNEL_CONTROL), carried by the
     * channel-frame trailer appended below — not this snapshot tail. */

    /* Fill in counts */
    buf[countsPos]     = hdr.tankCount;
    buf[countsPos + 1] = hdr.shellCount;
    buf[countsPos + 2] = hdr.tkExplosionCount;
    buf[countsPos + 3] = hdr.baseCount;
    buf[countsPos + 4] = hdr.pillCount;
    packU16(buf + countsPos + 5, hdr.mapChecksum);
    packU16(buf + countsPos + 7, hdr.returnToLobbyTicks);

    /* Parallel channel layer rides as a trailer on the snapshot: tick the
     * mux on this client's clock+RTT, then append one channel frame after
     * the event tails, keeping the datagram within UDP_MAX_PAYLOAD.  The
     * client recovers it as the bytes past the snapshot's parsed end. */
    bulkSenderPump(&udpServer.bulkSend[clientIdx], &udpServer.channelMux[clientIdx]);
    channelTick(&udpServer.channelMux[clientIdx], udpServer.tickCount,
                client->pingMs);
    {
        int budget = UDP_MAX_PAYLOAD - pos;
        if (budget >= 2) {
            pos += channelBuildFrame(&udpServer.channelMux[clientIdx],
                                     buf + pos, budget);
        }
    }

    /* wire-only: per-tick snapshot — high-volume delta-encoded path with its own reliability discipline */
    srvSendTo(buf, pos, &client->addr);
}

bool transportUdpServerCreate(unsigned short port,
                              const char *addrToUse,
                              ServerSim *sim,
                              const char *password) {
    struct sockaddr_in bindAddr;
    int i;

    (void)sim; /* Used later during tick */

    bolo_net_init();
    memset(&udpServer, 0, sizeof(udpServer));
    memset(punchQueue, 0, sizeof(punchQueue));

    udpServer.sock = createUdpSocket(true);
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
        WB_LOG_ERROR(WB_LOG_CAT_NET, "bind() failed on port %u", port);
        fprintf(stderr, "[UDP SERVER] bind() failed on port %u\n", port);
        closesocket(udpServer.sock);
        udpServer.sock = INVALID_SOCKET;
        return false;
    }

    serverSimSetPassword(sim, password,
                         password != NULL ? strlen(password) : 0);

    /* The spectator roster lives here in the transport layer; register the
     * enumerator so the sim's sync-replay (and the ring keyframe control
     * snapshot) can carry one CTRL_SPECTATOR_SLOT per connected viewer. */
    serverSimSetSpectatorRosterEnumerator(sim, serverEnumSpectatorRoster, NULL);

    udpServer.running = true;
    udpServer.tickCount = 0;
    udpServer.uploadMaxFiles        = 64;
    udpServer.uploadMaxStorageBytes = 8u * 1024u * 1024u;
    serverSimSetServerPort(sim, port);
    WB_LOG_INFO(WB_LOG_CAT_NET,
        "server created: port=%u bindAddr=%s maxPlayers=%u password=%s",
        port,
        (addrToUse && *addrToUse) ? addrToUse : "0.0.0.0",
        (unsigned)serverSimGetMaxPlayers(sim),
        (password && *password) ? "yes" : "no");
    udpServer.compressedMapSize = 0;

    for (i = 0; i < MAX_TANKS; i++) {
        udpServer.clients[i].connected = false;
        udpServer.clients[i].nameStickySuffix = false;
        udpServer.clients[i].claimPending = false;
        udpServer.clients[i].claimDesiredName[0] = '\0';
        udpServer.clients[i].inboundCmdSeq = 0;
        udpServer.clients[i].controlSub = SUBSCRIBER_HANDLE_INVALID;
        memset(&udpServer.mapDownload[i], 0, sizeof(ClientMapDownload));
        udpServer.controlSyncInProgress[i] = false;
        /* No slot is this transport's until someone joins it. The sim can
         * outlive an earlier server on the same process, so start from a
         * clean mask rather than whatever that server left behind. */
        serverSimSetShadowCulled(sim, (BYTE)i, false);
    }

    netImpairInit(&srvImpairIn);
    netImpairInit(&srvImpairOut);

    /* Start dedicated recv thread */
    udpServerRecvThreadStart(udpServer.sock);

    return true;
}

void transportUdpServerSetUploadConfig(UploadPolicy policy,
                                       uint8_t maxFiles,
                                       uint32_t maxStorageBytes,
                                       const char *persistDir) {
    udpServer.uploadPolicy = policy;
    if (maxFiles != 0) {
        udpServer.uploadMaxFiles = maxFiles;
    }
    if (maxStorageBytes != 0) {
        udpServer.uploadMaxStorageBytes = maxStorageBytes;
    }
    if (persistDir != NULL) {
        SDL_strlcpy(udpServer.uploadPersistDir, persistDir,
                    sizeof(udpServer.uploadPersistDir));
    } else {
        udpServer.uploadPersistDir[0] = '\0';
    }
}

void transportUdpServerDestroy(void) {
    int i;

    WB_LOG_INFO(WB_LOG_CAT_NET, "server destroy: tickCount=%u dropCount=%u",
                (unsigned)udpServer.tickCount, (unsigned)udpServerRecvDropCount());

    /* Stop recv thread before touching the socket */
    udpServerRecvThreadStop();

    /* Publish first so the per-client subscriber encodes and unicasts
     * PACKET_SERVER_SHUTDOWN while the socket is still open, then close. */
    if (udpServer.sock != INVALID_SOCKET) {
        {
            ControlEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_SERVER_SHUTDOWN;
            serverSimPublishControl(serverSimGetActive(), &evt);
        }
        closesocket(udpServer.sock);
        udpServer.sock = INVALID_SOCKET;
    }
    {
        ServerSim *activeSim = serverSimGetActive();
        for (i = 0; i < MAX_TANKS; i++) {
            if (udpServer.clients[i].connected) {
                serverSimUnregisterSubscriber(activeSim,
                                              udpServer.clients[i].controlSub);
                udpServer.clients[i].controlSub = SUBSCRIBER_HANDLE_INVALID;
            }
            udpServer.clients[i].connected = false;
            udpServer.clients[i].nameStickySuffix = false;
            udpServer.clients[i].claimPending = false;
            udpServer.clients[i].claimDesiredName[0] = '\0';
            /* The sim can be ticked on without this transport (a host that
             * drops back to single player), so give every copy back to the
             * tick as the server goes down. */
            serverSimSetShadowCulled(activeSim, (BYTE)i, false);
            serverCleanupMapDownload(i);
        }
    }
    udpServer.running = false;

    udpServerPublicIp[0] = '\0';
    udpServerPublicPort  = 0;
    memset(punchQueue, 0, sizeof(punchQueue));
}

/* Fill an INFO_PACKET from the current sim state. The info-request reply
 * and the tracker update both advertise the same server, so they share
 * this builder and differ only in where the finished packet is sent.
 * Layout and field semantics are documented in docs/info_packet_wire.md. */
void buildInfoPacket(ServerSim *sim, INFO_PACKET *pkt) {
    GameSim *gs = serverSimGetGameSim(sim);
    int i;
    BYTE numPlayers = 0, numHumans = 0, numBots = 0;

    memset(pkt, 0, sizeof(*pkt));

    /* Header */
    memcpy(pkt->h.signature, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE);
    pkt->h.versionMajor = BOLO_VERSION_MAJOR;
    pkt->h.versionMinor = BOLO_VERSION_MINOR;
    pkt->h.versionRevision = BOLO_VERSION_REVISION;
    pkt->h.type = BOLOPACKET_INFORESPONSE;

    /* Game ID — address zeroed (browser uses UDP source), port and timestamp set.
     * Tracker reads port raw for v1.1.8 (only ntohs for v1.1.1-3).
     * start_time is the only field the tracker byte-swaps on read. */
    if (udpServerPublicPort != 0) {
        pkt->gameid.serveraddress.s_addr = inet_addr(udpServerPublicIp);
        pkt->gameid.serverport = udpServerPublicPort;
    } else {
        pkt->gameid.serveraddress.s_addr = 0;
        pkt->gameid.serverport = serverSimGetServerPort(sim);
    }
    pkt->gameid.start_time = htonl(serverSimGetTimeCreated(sim));

    /* Map name as Pascal string */
    utilCtoPString((char *)serverSimGetMapName(sim), pkt->mapname);

    /* Game settings */
    pkt->gametype = (BYTE)gs->game;
    pkt->allow_mines = gs->hiddenMines ? HIDDEN_MINES : ALL_MINES_VISIBLE;
    pkt->allow_AI = 0;  /* AI type not tracked in new sim — report as none */
    {
        BYTE flags = 0;
        if (serverSimIsAcceptingJoins(sim))              flags |= INFO_FLAG_ALLOW_NEW_PLAYERS;
        if (transportUdpServerGetLock() || !serverSimIsAcceptingJoins(sim)) flags |= INFO_FLAG_LOCKED;
        if (serverSimGetRanked(sim))                     flags |= INFO_FLAG_RANKED;
        if (serverSimIsRandomMapEnabled(sim))            flags |= INFO_FLAG_RANDOM_MAP;
        if (serverSimGetState(sim) == serverStateLobby)  flags |= INFO_FLAG_IN_LOBBY;
        /* Advertise spectator support so finders can enable a Spectate action;
         * the cap accessor returns 0 when spectating is disabled. The live
         * spectator_count has no accessor yet, so it stays 0 below. */
        if (serverSimGetMaxSpectators(sim) > 0)          flags |= INFO_FLAG_ALLOW_SPECTATORS;
        /* The voice mode is two bits rather than a flag, in the top of the
         * same byte. serverVoiceOn packs as zero. */
        flags |= infoPacketPackVoiceMode(serverSimGetVoiceMode(sim));
        pkt->flags = flags;
    }
    pkt->start_delay = serverSimGetStartDelay(sim);
    pkt->time_limit = serverSimGetGameLength(sim);

    /* Count connected players, classifying humans vs bots */
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(sim, i)) {
            numPlayers++;
            if (serverSimIsBot(sim, (BYTE)i)) numBots++;
            else                              numHumans++;
        }
    }
    pkt->num_players = numPlayers;
    pkt->num_humans  = numHumans;
    pkt->num_bots    = numBots;
    pkt->max_players = serverSimGetMaxPlayers(sim);

    /* Neutral pills and bases */
    pkt->free_pills = pillsGetNumNeutral(&gs->pb);
    pkt->free_bases = basesGetNumNeutral(&gs->bs);

    pkt->has_password = serverSimGetPassword(sim)[0] != '\0' ? 1 : 0;
    pkt->spectator_count = 0;

    {
        const char *md5Hex = serverSimGetMapMd5Hex(sim);
        if (md5Hex[0] != '\0' && !serverSimIsRandomMapEnabled(sim)) {
            memcpy(pkt->map_md5, md5Hex, 32);
        }
    }

    pkt->view_policies = infoPacketPackViewPolicies(
        serverSimGetViewPolicy(sim, viewCategoryPill),
        serverSimGetViewPolicy(sim, viewCategoryBase),
        serverSimGetViewPolicy(sim, viewCategoryAlly),
        serverSimGetClassicMode(sim),
        serverSimGetAlliesInTrees(sim));
}

/* Handle an old-protocol info request (server browser compatibility).
 * Replies to the requester with the current server advertisement. */
static void serverHandleInfoRequest(const struct sockaddr_in *fromAddr,
                                    ServerSim *sim) {
    INFO_PACKET pkt;
    char consoleMsg[256];

    buildInfoPacket(sim, &pkt);

    /* wire-only: tracker / external reply (no in-process audience) */
    srvSendTo((uint8_t *)&pkt, sizeof(pkt), fromAddr);

    {
        struct in_addr addrCopy = fromAddr->sin_addr;
        snprintf(consoleMsg, sizeof(consoleMsg), "Info packet request from %s",
                 inet_ntoa(addrCopy));
    }
    serverSimConsoleMessage(consoleMsg);
}

/* Check if a packet is an old-protocol info request.
 * Gate is magic + length + type only — the info-request is the universal
 * version-negotiation primitive, so a v1.0 client asking a v2.0 server
 * (or vice versa) must receive an INFO_RESPONSE carrying the server's
 * own version triple.  Mismatched-version joiners then see a localized
 * pre-flight error rather than a silent JOIN_REQUEST length-gate drop.
 * The version bytes inside the request body are still parsed elsewhere
 * for logging but no longer gate the response. */
static bool isOldProtocolInfoRequest(const uint8_t *buf, int len) {
    return len == BOLOPACKET_REQUEST_SIZE &&
           memcmp(buf, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE) == 0 &&
           buf[BOLOPACKET_REQUEST_TYPEPOS] == BOLOPACKET_INFOREQUEST;
}

uint32_t transportUdpServerGetTickCount(void) {
    return udpServer.tickCount;
}

bool transportUdpServerHasAnyClient(void) {
    int i;
    if (!udpServer.running) return false;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) return true;
    }
    return false;
}

void transportUdpServerOnGameStart(ServerSim *sim) {
    int i;
    mpDiagLog("[srv] GAME_START BEGIN (rebasing game/map channel send baselines)");
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            mpDiagLog("[srv] GAME_START wipe slot=%d pre mapEv(ack=%u next=%u)",
                      i,
                      (unsigned)udpServer.mapEventQueues[i].ackedSeq,
                      (unsigned)udpServer.mapEventQueues[i].nextSeq);
        }
        if (udpServer.clients[i].connected) {
            /* Ensure map download is considered complete so snapshots
             * are sent during the game even if a final chunk ack was lost. */
            udpServer.mapDownload[i].downloadComplete = TRUE;
            /* This force-completes the download without serverInitMapDownload,
             * so clear any in-flight resync explicitly — otherwise the send
             * gate above would keep holding this slot's map events forever.
             * Also disarm any pending bulk transfer so a stale arming can't
             * re-fire a completion against the new game's channel state. */
            udpServer.mapDownload[i].resyncInProgress = FALSE;
            udpServer.mapDownload[i].resyncGen = 0;
            udpServer.mapDownload[i].xferKind = MAP_XFER_NONE;
            udpServer.mapDownload[i].xferBegun = false;
        }
        /* Drop the previous game's unacked reliable events on the map queue,
         * but keep the sequence counter monotonic — never reuse low seq
         * numbers.  Set ackedSeq = nextSeq (queue now empty) and memset the
         * buffer (clears stale delivered bodies so they can't be resent), but
         * do NOT reset nextSeq.
         *
         * This is the fix for the lobby→running seq-reuse desync: a stale
         * in-flight lobby event delayed past game start now carries seq
         * numbers BELOW the client's continuing ack, so it dedups harmlessly
         * instead of being mistaken for fresh running-space events (which is
         * what happened when the queue restarted at seq 1 and old high-seq
         * lobby events looked newer than the new low-seq running events).
         * The reliable game/map channels get the same forward truncation via
         * the per-client CHANNEL_RESET below. */
        udpServer.mapEventQueues[i].ackedSeq = udpServer.mapEventQueues[i].nextSeq;
        memset(udpServer.mapEventQueues[i].buffer, 0,
               sizeof(udpServer.mapEventQueues[i].buffer));

        /* Drop this client's previous-game send tail on the reliable game
         * (channel 0) and map (channel 1) channels and tell it the new
         * baselines so its receive side lifts past any in-flight straggler.
         * channelResetSend collapses each channel's unacked window and returns
         * the post-reset sequence floor (its nextSeq); the per-client
         * CTRL_CHANNEL_RESET carries those two floors. Both ride the in-order
         * control channel (channel 2) ahead of the CTRL_GAME_PHASE_RUNNING the
         * caller publishes immediately after this returns, so the client
         * applies the baseline lift before the running flip and a previous-game
         * game/map event left in flight dedup-drops in the new game. The
         * baselines are this client's own channel state — the control encoder
         * stays recipient-agnostic, so the per-client value lives in the event,
         * not the encoder. A full control window defers the disconnect off this
         * path (mirrors the deliver-callback overflow). */
        if (udpServer.clients[i].connected) {
            uint32_t b0 = channelResetSend(&udpServer.channelMux[i], CHANNEL_GAME);
            uint32_t b1 = channelResetSend(&udpServer.channelMux[i], CHANNEL_MAP);
            ControlEvent resetEvt;
            ControlEncodeBodyFn enc =
                transportControlCodecBodyEncoder(CTRL_CHANNEL_RESET);
            uint8_t msg[CHANNEL_CONTROL_SEG];
            size_t bodyLen = 0;
            memset(&resetEvt, 0, sizeof(resetEvt));
            resetEvt.type = CTRL_CHANNEL_RESET;
            resetEvt.u.channelReset.channelMask =
                (uint8_t)((1u << CHANNEL_GAME) | (1u << CHANNEL_MAP));
            resetEvt.u.channelReset.ch0Baseline = b0;
            resetEvt.u.channelReset.ch1Baseline = b1;
            if (enc != NULL &&
                enc(&resetEvt, &udpServer.clients[i], msg + 3,
                    sizeof(msg) - 3, &bodyLen) == ENCODE_OK) {
                msg[0] = (uint8_t)CTRL_CHANNEL_RESET;
                packU16(msg + 1, (uint16_t)bodyLen);
                if (!channelSend(&udpServer.channelMux[i], CHANNEL_CONTROL,
                                 msg, (uint16_t)(3 + bodyLen))) {
                    if (!udpServer.pendingSimRemove[i]) {
                        WB_LOG_ERROR(WB_LOG_CAT_NET,
                                     "control channel overflow sending baseline "
                                     "reset for slot %d, deferring disconnect", i);
                        udpServer.pendingSimRemove[i] = true;
                    }
                }
            }
        }

        /* New round, fresh round-log request budget for every slot. */
        udpServerResetRoundLogLimits(i);
        udpServerResetMapReaskLimit(i);

        /* A round-log transfer is a lobby/game-over affair and must not bleed
         * into the round starting now: CHANNEL_BULK is deliberately not
         * re-based above, so an unfinished one would keep streaming into the
         * new game's map downloads. Abort it with the same triple the map
         * change uses — drop the staged blob, collapse the send window, and
         * carry the new bulk baseline so the client abandons its partial.
         * Gating on the sender's kind is what leaves every other in-flight
         * transfer (a join download, a resync) undisturbed. */
        if (udpServer.clients[i].connected &&
            bulkSenderBusy(&udpServer.bulkSend[i]) &&
            udpServer.bulkSend[i].kind == BULK_KIND_ROUND_LOG) {
            uint32_t b3;
            ControlEvent resetEvt;
            ControlEncodeBodyFn enc =
                transportControlCodecBodyEncoder(CTRL_CHANNEL_RESET);
            uint8_t msg[CHANNEL_CONTROL_SEG];
            size_t bodyLen = 0;
            bulkSenderReset(&udpServer.bulkSend[i]);
            b3 = channelResetSend(&udpServer.channelMux[i], CHANNEL_BULK);
            memset(&resetEvt, 0, sizeof(resetEvt));
            resetEvt.type = CTRL_CHANNEL_RESET;
            resetEvt.u.channelReset.channelMask = (uint8_t)(1u << CHANNEL_BULK);
            resetEvt.u.channelReset.ch3Baseline = b3;
            if (enc != NULL &&
                enc(&resetEvt, &udpServer.clients[i], msg + 3,
                    sizeof(msg) - 3, &bodyLen) == ENCODE_OK) {
                msg[0] = (uint8_t)CTRL_CHANNEL_RESET;
                packU16(msg + 1, (uint16_t)bodyLen);
                if (!channelSend(&udpServer.channelMux[i], CHANNEL_CONTROL,
                                 msg, (uint16_t)(3 + bodyLen))) {
                    if (!udpServer.pendingSimRemove[i]) {
                        WB_LOG_ERROR(WB_LOG_CAT_NET,
                                     "control channel overflow sending bulk reset "
                                     "for slot %d, deferring disconnect", i);
                        udpServer.pendingSimRemove[i] = true;
                    }
                }
            }
        }
    }
    WB_LOG_INFO(WB_LOG_CAT_NET, "ctrl queue reset all slots (game start)");

    /* Cut every live-lobby spectator over to the delayed ring as the game
     * starts. Drop its control-bus subscription so the imminent
     * CTRL_GAME_PHASE_RUNNING publish (and the forced snapshot after it)
     * cannot reach it — a live spectator must see zero running-state state,
     * or the configured spectator delay is undercut. This runs before the
     * RUNNING publish on both start paths (the lifecycle countdown→running
     * step and the in-place start), so the unsubscribe is the anti-cheat
     * boundary. No channel reset: a live spectator never ran the delayed
     * seed/feed block, so its seed/feed fields are still at their accept-time
     * zeros and the next serverServiceSpectators tick finds head - delay still
     * inside the pre-game lobby (below gameStartSeq) → arms the "spectating
     * begins in X" countdown cleanly until the delayed view reaches the game. */
    for (i = 0; i < MAX_SPECTATORS; i++) {
        SpectatorConn *sp = &udpServer.spectators[i];
        if (!sp->connected || !sp->live) continue;
        serverSimUnregisterSubscriber(sim, sp->controlSub);
        sp->controlSub = SUBSCRIBER_HANDLE_INVALID;
        sp->live = false;
        mpDiagLog("[srv] GAME_START spec idx=%d live->delayed (unsubscribed)", i);
    }

    mpDiagLog("[srv] GAME_START END (game/map channel send baselines rebased)");
}

void transportUdpServerOnLobbyMapChange(ServerSim *sim) {
    int i;
    int mapLen;
    uint8_t notifyBuf[PACKET_HEADER_SIZE];
    /* Compress into a local oversized scratch buffer first — the map
     * RLE encoder has no internal output-bound check, and an
     * incompressible map can encode slightly larger than its 64KB
     * input. Validate the result fits the wire size before copying. */
    BYTE scratchMap[MAP_COMPRESSED_MAX_SIZE];

    mapLen = serverSimGetCompressedMap(sim, scratchMap, (int)sizeof(scratchMap));
    if (mapLen <= 0) {
        fprintf(stderr, "[UDP SERVER] Map change: failed to compress new map\n");
        return;
    }
    if (mapLen > (int)MAP_DOWNLOAD_MAX_SIZE) {
        fprintf(stderr,
                "[UDP SERVER] Map change: compressed map (%d bytes) exceeds "
                "MAP_DOWNLOAD_MAX_SIZE (%d); aborting broadcast\n",
                mapLen, (int)MAP_DOWNLOAD_MAX_SIZE);
        return;
    }
    memcpy(udpServer.compressedMap, scratchMap, (size_t)mapLen);
    udpServer.compressedMapSize = (uint32_t)mapLen;

    packHeader(notifyBuf, PACKET_LOBBY_MAP_CHANGE, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        /* Send PACKET_LOBBY_MAP_CHANGE so the client flushes its
         * stale map state before the new chunk stream lands. */
        srvSendTo(notifyBuf, sizeof(notifyBuf),
                  &udpServer.clients[i].addr);
        /* Re-send JOIN_ACCEPT so the client picks up the new compressed
         * map size. */
        serverSendJoinAccept(i, sim, &udpServer.clients[i].addr);
        /* Re-base CHANNEL_BULK and arm a fresh join download from the new
         * blob (re-gates snapshots). It streams once the client's
         * PACKET_MAP_DL_READY confirms its buffers were re-armed for the new
         * size — the accept above (or the one re-sent for the client's forced
         * re-JOIN) triggers that. */
        serverRebaseBulkAndRearmDownload(i);
    }

    /* Live-lobby spectators: mirror the player loop — re-send the accept (new map
     * size) and re-arm the lobby-map download, which resets the spectator's bulk
     * sender + re-bases its CHANNEL_BULK (via CTRL_CHANNEL_RESET) so it abandons
     * the old partial and downloads the new map. Delayed (in-game) viewers are
     * skipped: their map rides the ring seed, not this path. */
    for (i = 0; i < MAX_SPECTATORS; i++) {
        SpectatorConn *sp = &udpServer.spectators[i];
        if (!sp->connected || !sp->live) continue;
        serverSendSpectatorAccept(i, sim, &sp->addr);
        serverArmSpectatorLobbyMap(i, /*resetChannel=*/true);
    }

    fprintf(stderr, "[UDP SERVER] Map change prep: %u bytes compressed map\n",
            udpServer.compressedMapSize);
}

const char *transportUdpServerGetPlayerName(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) {
        return NULL;
    }
    /* Bots have no UDP connection but their name was set via
     * transportUdpServerSetBotName; treat them as valid name owners. */
    /* sim not in scope here (this is a callback fed to the snapshot
     * builder); reach the active sim through serverSimGetActive so the
     * bot check still works after BotManager moved onto ServerSim. */
    ServerSim *active = serverSimGetActive();
    if (!udpServer.clients[playerNum].connected &&
        (active == NULL || !serverSimIsBot(active, playerNum))) {
        return NULL;
    }
    return udpServer.clients[playerNum].playerName;
}

const char *transportUdpServerGetClientCountryCode(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) {
        return NULL;
    }
    ServerSim *active = serverSimGetActive();
    if (!udpServer.clients[playerNum].connected &&
        (active == NULL || !serverSimIsBot(active, playerNum))) {
        return NULL;
    }
    return udpServer.clients[playerNum].countryCode;
}

uint8_t transportUdpServerGetClientType(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) {
        return CLIENT_TYPE_UNKNOWN;
    }
    ServerSim *active = serverSimGetActive();
    if (!udpServer.clients[playerNum].connected &&
        (active == NULL || !serverSimIsBot(active, playerNum))) {
        return CLIENT_TYPE_UNKNOWN;
    }
    return udpServer.clients[playerNum].clientType;
}

uint64_t transportUdpServerGetClientConnId(BYTE playerNum) {
    if (playerNum >= MAX_TANKS || !udpServer.clients[playerNum].connected) {
        return 0;
    }
    return udpServer.clients[playerNum].connId;
}

/* Process a single received packet — extracted from the recv loop so both
 * the polled fallback and the recv-thread drain path can share it. */
void serverProcessPacket(ServerSim *sim, uint8_t *buf, int len,
                         struct sockaddr_in *fromAddr) {
    uint8_t pktType;

    /* Check for old-protocol info request before new-protocol handling */
    if (isOldProtocolInfoRequest(buf, len)) {
        serverHandleInfoRequest(fromAddr, sim);
        return;
    }

    pktType = getPacketType(buf, len);
    switch (pktType) {
        case PACKET_JOIN_REQUEST:
            serverHandleJoinRequest(buf, len, fromAddr, sim);
            break;
        case PACKET_INPUT:
            serverHandleInput(buf, len, fromAddr, sim);
            break;
        case PACKET_PING:
            serverHandlePing(buf, len, fromAddr);
            /* Spectators aren't in clients[]; refresh their liveness too. */
            {
                int sIdx = serverFindSpectator(fromAddr);
                if (sIdx >= 0) {
                    udpServer.spectators[sIdx].lastReceivedTick = udpServer.tickCount;
                }
            }
            break;
        case PACKET_CHANNEL: {
            /* Standalone channel frame (client → server, sent when no input
             * rides this tick).  Body is one frame directly after the header. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0) {
                /* Spectator acks ride the same standalone-frame path; consume
                 * them into the spectator's own mux.  No bulk-receive drain —
                 * a spectator never uploads. */
                int sIdx = serverFindSpectator(fromAddr);
                if (sIdx >= 0) {
                    udpServer.spectators[sIdx].lastReceivedTick = udpServer.tickCount;
                    channelRecvFrame(&udpServer.spectators[sIdx].channelMux,
                                     buf + PACKET_HEADER_SIZE,
                                     len - PACKET_HEADER_SIZE);
                }
                break;
            }
            udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;
            if (channelRecvFrame(&udpServer.channelMux[clientIdx],
                                 buf + PACKET_HEADER_SIZE,
                                 len - PACKET_HEADER_SIZE) >= 0) {
                udpServer.channelFramesRx[clientIdx]++;
                serverDrainBulk(sim, clientIdx);
            }
            break;
        }
        case PACKET_COMMAND_TICK: {
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0) {
                /* A tankless spectator may send CMD_CHAT — and nothing else,
                 * and only while the server is in lobby/countdown. This branch
                 * is the hard isolation boundary: a viewer has no slot or sim
                 * state to mutate, so a decoded command of any other type is
                 * dropped here. The cmdSeq dedup mirrors the player path so the
                 * spectator's reliable carrier acks and retransmits coherently. */
                int sIdx = serverFindSpectator(fromAddr);
                if (sIdx < 0) break;
                SpectatorConn *sp = &udpServer.spectators[sIdx];
                ServerState st = serverSimGetState(sim);
                bool lobbyish =
                    (st == serverStateLobby || st == serverStateCountdown);
                sp->lastReceivedTick = udpServer.tickCount;
                if (len < PACKET_HEADER_SIZE + 1) break;
                uint8_t scount = buf[PACKET_HEADER_SIZE];
                size_t spos = PACKET_HEADER_SIZE + 1;
                for (uint8_t i = 0; i < scount; i++) {
                    if (spos + 2 > (size_t)len) break;
                    uint16_t entryLen = unpackU16(buf + spos);
                    spos += 2;
                    if (spos + entryLen > (size_t)len) break;
                    ClientCommand cmd;
                    if (!commandCodecDecode(buf + spos, entryLen, &cmd)) {
                        spos += entryLen;
                        continue;
                    }
                    spos += entryLen;
                    if (cmd.cmdSeq <= sp->inboundCmdSeq) continue;
                    if (cmd.cmdSeq != sp->inboundCmdSeq + 1) continue;
                    sp->inboundCmdSeq = cmd.cmdSeq;
                    if (cmd.type == CMD_CHAT && lobbyish) {
                        serverSimReceiveSpectatorChat(sim, (uint8_t)sIdx,
                                                      cmd.u.chat.body,
                                                      cmd.u.chat.bodyLen);
                    }
                }
                uint8_t sackBuf[PACKET_HEADER_SIZE + 4];
                packHeader(sackBuf, PACKET_COMMAND_ACK, sp->outSequence++);
                packU32(sackBuf + PACKET_HEADER_SIZE, sp->inboundCmdSeq);
                srvSendTo(sackBuf, sizeof(sackBuf), &sp->addr);
                break;
            }
            UdpServerClient *client = &udpServer.clients[clientIdx];
            if (len < PACKET_HEADER_SIZE + 1) break;
            uint8_t count = buf[PACKET_HEADER_SIZE];
            size_t pos = PACKET_HEADER_SIZE + 1;
            for (uint8_t i = 0; i < count; i++) {
                if (pos + 2 > (size_t)len) break;
                uint16_t entryLen = unpackU16(buf + pos);
                pos += 2;
                if (pos + entryLen > (size_t)len) break;
                ClientCommand cmd;
                if (!commandCodecDecode(buf + pos, entryLen, &cmd)) {
                    pos += entryLen;
                    continue;
                }
                pos += entryLen;
                if (cmd.cmdSeq <= client->inboundCmdSeq) continue;
                if (cmd.cmdSeq != client->inboundCmdSeq + 1) continue;
                (void)serverSimApplyCommand(sim, clientIdx, &cmd);
                client->inboundCmdSeq = cmd.cmdSeq;
            }
            uint8_t ackBuf[PACKET_HEADER_SIZE + 4];
            packHeader(ackBuf, PACKET_COMMAND_ACK, client->outSequence++);
            packU32(ackBuf + PACKET_HEADER_SIZE, client->inboundCmdSeq);
            srvSendTo(ackBuf, sizeof(ackBuf), &client->addr);
            break;
        }
        case PACKET_QUIT: {
            int clientIdx = serverFindClient(fromAddr);
            WB_LOG_INFO(WB_LOG_CAT_NET,
                "PACKET_QUIT from %s:%u clientIdx=%d",
                inet_ntoa(fromAddr->sin_addr),
                (unsigned)ntohs(fromAddr->sin_port),
                clientIdx);
            if (clientIdx >= 0) {
                serverCleanupMapDownload(clientIdx);
                serverDisconnectClient(sim, clientIdx, TRUE);
                serverSimRemovePlayer(sim, (BYTE)clientIdx);
                /* Broadcast lobby update if in lobby/countdown state */
                if (serverSimIsLobbyEnabled(sim) &&
                    (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown)) {
                    serverSimPublishLobbySlot(sim, (BYTE)clientIdx);
                }
            }
            break;
        }
        case PACKET_MAP_DL_READY: {
            /* Client's join-download buffers are armed (its JOIN_ACCEPT
             * landed). Body: [connId u64]. First ask for an armed-but-unbegun
             * download simply releases it (serverBeginMapTransferIfReady).
             * A re-ask while a stream is — or already was — in flight means
             * the client cannot complete that stream (its head was consumed
             * before the buffers existed, or the transfer finished into a
             * receiver that had been reset mid-body): the channel has acked
             * those bytes, so only a full restart behind a CHANNEL_BULK
             * re-base can deliver the map again. connId must match the slot's
             * so an address-spoofed READY can't reset a healthy client's
             * transfer or re-gate its snapshots. A resync in flight is left
             * alone — it owns the channel, and its own request/stall machinery
             * recovers it. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 8) {
                uint64_t reqConnId = unpackConnId(buf + PACKET_HEADER_SIZE);
                UdpServerClient *cl = &udpServer.clients[clientIdx];
                ClientMapDownload *dl = &udpServer.mapDownload[clientIdx];
                if (cl->connId != 0 && reqConnId != cl->connId) break;
                cl->lastReceivedTick = udpServer.tickCount;
                if (dl->xferKind == MAP_XFER_RESYNC || dl->resyncInProgress) {
                    break;
                }
                if (dl->xferKind == MAP_XFER_DOWNLOAD && !dl->xferBegun) {
                    dl->readySeen = TRUE;
                } else if (udpServer.mapReaskSeen[clientIdx] &&
                           (uint32_t)(udpServer.tickCount -
                                      udpServer.mapReaskLastTick[clientIdx]) <
                               MAP_REASK_MIN_TICKS) {
                    /* Inside the interval: drop it. The client's own watchdog
                     * asks again after its resend window, and that ask lands
                     * outside this one. Nothing is sent back — a re-ask has no
                     * reply of its own, and answering would hand back a second
                     * datagram for the one that was refused. */
                    udpServer.mapReaskThrottled[clientIdx]++;
                    WB_LOG_DEBUG(WB_LOG_CAT_NET,
                        "MAP_DL_READY re-ask slot=%d inside the %d-tick "
                        "interval -> throttled (%u so far)",
                        clientIdx, MAP_REASK_MIN_TICKS,
                        (unsigned)udpServer.mapReaskThrottled[clientIdx]);
                } else {
                    /* Stamp for every re-ask the server acts on, so the
                     * interval measures from the last restart it actually
                     * paid for. */
                    udpServer.mapReaskSeen[clientIdx] = true;
                    udpServer.mapReaskLastTick[clientIdx] = udpServer.tickCount;
                    /* Restart from this slot's own copy of the terrain rather
                     * than from whatever the shared staging buffer happens to
                     * hold: serverInitMapDownload copies staging, and staging
                     * carries the blob of whichever slot joined or resynced
                     * last. So recompress this slot's copy into staging, then
                     * re-send JOIN_ACCEPT so the size the client expects
                     * matches the blob it is about to be sent — the client
                     * drops a stream whose header size disagrees with the size
                     * its accept carried, so a restart off another slot's blob
                     * leaves it stuck on "Downloading map" instead of
                     * recovering it. Staging, then accept, then re-arm, the
                     * same order transportUdpServerOnLobbyMapChange uses. A
                     * compress that will not fit the wire size leaves staging's
                     * size alone and restarts exactly as before. */
                    int mapLen = serverSimGetCompressedMapFor(
                        sim, (BYTE)clientIdx, udpServer.compressedMap,
                        (int)sizeof(udpServer.compressedMap));
                    WB_LOG_INFO(WB_LOG_CAT_NET,
                        "MAP_DL_READY re-ask slot=%d (kind=%d begun=%d "
                        "complete=%d) -> restarting download",
                        clientIdx, (int)dl->xferKind, (int)dl->xferBegun,
                        (int)dl->downloadComplete);
                    if (mapLen > 0 && mapLen <= (int)MAP_DOWNLOAD_MAX_SIZE) {
                        udpServer.compressedMapSize = (uint32_t)mapLen;
                        serverSendJoinAccept(clientIdx, sim,
                                             &udpServer.clients[clientIdx].addr);
                    }
                    serverRebaseBulkAndRearmDownload(clientIdx);
                    dl->readySeen = TRUE;
                }
            }
            break;
        }
        case PACKET_MAP_RESYNC_REQUEST: {
            /* Client detected its terrain diverged (a dropped EVENT_MAP_CHANGE)
             * and asks for a fresh copy of the live map. Body: [resyncGen u32].
             * Only an established slot may ask — this is a data re-send the
             * client is already entitled to, not a state assertion.
             *
             * The compress + queue-cut must be atomic w.r.t. serverSimTick:
             * if a sim tick assigned a new EVENT_MAP_CHANGE a seq between the
             * compress and the cut, that change would be both baked into the
             * blob AND retained at seq >= cut, and apply twice. Packet handling
             * and the sim tick run on the same thread (the recv thread only
             * enqueues raw datagrams into recvQueue; serverProcessPacket and
             * serverSimTick are both driven from the timer/drain thread), so a
             * synchronous handler is naturally atomic. Keep it synchronous. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 4) {
                uint32_t reqGen = unpackU32(buf + PACKET_HEADER_SIZE);
                ClientMapDownload *dl = &udpServer.mapDownload[clientIdx];

                udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;

                if (dl->resyncInProgress) {
                    /* Idempotent: a resync is already armed/in-flight for this
                     * slot. Do NOT re-compress or re-cut — re-cutting would
                     * advance ackedSeq past changes enqueued since the first cut
                     * and drop them (the exact desync this recovers from). The
                     * bulk channel retransmits its own unacked segments, so
                     * there is nothing to re-poke. */
                } else {
                    /* Idle slot: compress this slot's copy of the terrain into
                     * its download buffer — the client is asking for the map
                     * it was given, not some other slot's — cut the map-event
                     * queue so the blob and the queue can't both carry the
                     * same change, then arm a resync transfer.
                     * It begins on CHANNEL_BULK once the channel is idle
                     * (serverServiceMapTransfer) and rides the snapshot trailer. */
                    int mapLen = serverSimGetCompressedMapFor(
                        sim, (BYTE)clientIdx, udpServer.compressedMap,
                        (int)sizeof(udpServer.compressedMap));
                    if (mapLen > 0 && mapLen <= (int)MAP_DOWNLOAD_MAX_SIZE) {
                        udpServer.compressedMapSize = (uint32_t)mapLen;
                        if (dl->compressedMap != NULL) free(dl->compressedMap);
                        dl->compressedMap = (BYTE *)malloc(udpServer.compressedMapSize);
                        memcpy(dl->compressedMap, udpServer.compressedMap,
                               udpServer.compressedMapSize);
                        dl->mapSize = udpServer.compressedMapSize;
                        /* The cut: the blob carries every change up to
                         * nextSeq-1, so empty the queue. Changes during the
                         * transfer land at seq >= nextSeq and are held by the
                         * send gate until completion. */
                        udpServer.mapEventQueues[clientIdx].ackedSeq =
                            udpServer.mapEventQueues[clientIdx].nextSeq;
                        dl->resyncGen = reqGen;
                        dl->resyncInProgress = TRUE;
                        /* Tag map-change events sent from here on with this
                         * request's generation. The client drops any map event
                         * tagged older than the generation it installs, so a
                         * stale change still in flight on the channel can't
                         * apply on top of the freshly downloaded blob. */
                        udpServer.mapGen[clientIdx] = reqGen;
                        /* Arm the resync stream (downloadComplete stays true for
                         * this established slot — only resyncInProgress gates the
                         * held map events). */
                        dl->xferKind = MAP_XFER_RESYNC;
                        dl->xferBegun = false;
                        dl->xferStartSeq = 0;
                        dl->xferEndSeq = 0;

                        /* Self-check: the blob the client will install must
                         * round-trip back to this slot's copy of the terrain
                         * and its record of the pill squares — the same two the
                         * snapshot header's checksum is stamped from. If it
                         * doesn't, the client can never match that checksum and
                         * loops resync requests until it self-kicks, so decode
                         * the blob into scratch structures and compare
                         * tile-for-tile against that copy. The pill list
                         * follows the same rule as the terrain: the check is
                         * against what the client was actually given, not
                         * against the live state, because comparing against the
                         * live map would report a difference on every resync
                         * from a culled slot, whose copy lags the live map by
                         * design. The live map and pill list are used only for
                         * a slot with no records bound. Resyncs are infrequent;
                         * the cost is acceptable for the diagnosis. */
                        {
                            map *known = sim->clientKnownMap[clientIdx] != NULL
                                             ? &sim->clientKnownMap[clientIdx]
                                             : &serverSimGetGameSim(sim)->mp;
                            struct pillsObj slotPills;
                            pillboxes slotPb = &slotPills;
                            bool useSlotPills =
                                serverSimGetPillsForSlot(sim, (BYTE)clientIdx,
                                                         &slotPills);
                            pillboxes *knownPb = useSlotPills
                                             ? &slotPb
                                             : &serverSimGetGameSim(sim)->pb;
                            uint16_t knownSum = mapCalcChecksum(known,
                                                   &serverSimGetGameSim(sim)->bs,
                                                   knownPb);
                            map rtMap; pillboxes rtPb; bases rtBs; starts rtSs;
                            mapCreate(&rtMap);
                            pillsCreate(&rtPb);
                            basesCreate(&rtBs);
                            startsCreate(&rtSs);
                            if (mapLoadCompressedMap(&rtMap, &rtPb, &rtBs, &rtSs,
                                                     udpServer.compressedMap, mapLen)) {
                                uint16_t rtSum = mapCalcChecksum(&rtMap, &rtBs, &rtPb);
                                if (rtSum != knownSum) {
                                    /* Dedupe: an unconverged divergence repeats on every
                                     * resync request and floods the log. Dump full per-tile
                                     * detail only when the (copy,blob) checksum pair changes;
                                     * identical repeats get one concise line. The state is
                                     * process-wide and this runs on the single drain thread. */
                                    static uint32_t s_lastResyncDiffSig = 0xFFFFFFFFu;
                                    uint32_t sig = ((uint32_t)knownSum << 16) | (uint32_t)rtSum;
                                    const char *mapName = serverSimGetMapName(sim);
                                    if (sig == s_lastResyncDiffSig) {
                                        WB_LOG_WARN(WB_LOG_CAT_NET,
                                            "map resync still not converging on '%s' "
                                            "(client copy sum=%u blob sum=%u, client %d gen=%u) - detail suppressed",
                                            mapName, (unsigned)knownSum, (unsigned)rtSum,
                                            clientIdx, (unsigned)reqGen);
                                    } else {
                                        bases *liveBs = &serverSimGetGameSim(sim)->bs;
                                        int diffs = 0, realDiffs = 0, shown = 0, xx, yy;
                                        s_lastResyncDiffSig = sig;
                                        for (yy = 0; yy < MAP_ARRAY_SIZE; yy++) {
                                            for (xx = 0; xx < MAP_ARRAY_SIZE; xx++) {
                                                BYTE kv = mapGetPos(known, (BYTE)xx, (BYTE)yy);
                                                BYTE rv = mapGetPos(&rtMap, (BYTE)xx, (BYTE)yy);
                                                if (kv != rv) {
                                                    /* Terrain under a base/pill is folded to ROAD
                                                     * by the checksum (it is not authoritative), so
                                                     * such a tile can never be the real cause of
                                                     * non-convergence — flag it benign. */
                                                    bool onBase = (basesExistPos(liveBs, (BYTE)xx, (BYTE)yy) ||
                                                                   basesExistPos(&rtBs, (BYTE)xx, (BYTE)yy));
                                                    bool onPill = (pillsExistPos(knownPb, (BYTE)xx, (BYTE)yy) ||
                                                                   pillsExistPos(&rtPb, (BYTE)xx, (BYTE)yy));
                                                    diffs++;
                                                    if (!onBase && !onPill) { realDiffs++; }
                                                    if (shown < 8) {
                                                        WB_LOG_WARN(WB_LOG_CAT_NET,
                                                            "map resync blob diff @(%d,%d) "
                                                            "clientcopy=%s(%u) roundtrip=%s(%u) [%s] map='%s'",
                                                            xx, yy,
                                                            resyncTerrainName(kv), (unsigned)kv,
                                                            resyncTerrainName(rv), (unsigned)rv,
                                                            onBase ? "base" : (onPill ? "pill" : "REAL"),
                                                            mapName);
                                                        shown++;
                                                    }
                                                }
                                            }
                                        }
                                        WB_LOG_WARN(WB_LOG_CAT_NET,
                                            "map resync blob does NOT round-trip on '%s': "
                                            "%d differing tile(s) (%d genuine, %d under base/pill fixup) "
                                            "(client copy sum=%u blob sum=%u) - %s",
                                            mapName, diffs, realDiffs, diffs - realDiffs,
                                            (unsigned)knownSum, (unsigned)rtSum,
                                            realDiffs ? "client cannot converge"
                                                      : "benign structure fixup only");
                                    }
                                }
                            } else {
                                WB_LOG_WARN(WB_LOG_CAT_NET,
                                    "map resync blob failed self-check decode (gen=%u, %d bytes)",
                                    (unsigned)reqGen, mapLen);
                            }
                            mapDestroy(&rtMap);
                            pillsDestroy(&rtPb);
                            basesDestroy(&rtBs);
                            startsDestroy(&rtSs);
                            fprintf(stderr,
                                    "[UDP SERVER] Client %d map resync gen=%u (%d bytes) clientcopysum=%u\n",
                                    clientIdx, reqGen, mapLen, (unsigned)knownSum);
                        }
                    } else {
                        fprintf(stderr,
                                "[UDP SERVER] Client %d map resync: compress failed (%d)\n",
                                clientIdx, mapLen);
                    }
                }
            }
            break;
        }
        case PACKET_LOBBY_MAP_LIST_REQ: {
            /* [header 8] [pathLen 1] [path N] — any lobby client may
             * ask. Response is sent back to the requester only
             * (wire-only handshake per ARCHITECTURE.md §"Load-bearing
             * wire-only exceptions"). */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 1) break;
            if (udpServer.clientReqCooldownTicks[clientIdx] > 0) break;
            udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
            uint8_t pathLen = buf[PACKET_HEADER_SIZE];
            if (pathLen > 255 ||
                len < PACKET_HEADER_SIZE + 1 + pathLen) break;
            char relPath[256];
            memset(relPath, 0, sizeof(relPath));
            if (pathLen > 0) {
                memcpy(relPath, buf + PACKET_HEADER_SIZE + 1, pathLen);
            }

            bool safe = true;
            if (relPath[0] == '/' || relPath[0] == '\\') safe = false;
            else if (relPath[0] != '\0' && relPath[1] == ':') safe = false;
            else {
                for (const char *s = relPath; *s;) {
                    if (s[0] == '.' && s[1] == '.' &&
                        (s[2] == '\0' || s[2] == '/' || s[2] == '\\')) {
                        safe = false; break;
                    }
                    while (*s && *s != '/' && *s != '\\') s++;
                    while (*s == '/' || *s == '\\') s++;
                }
            }

            /* Cap matches LOBBY_MAP_LIST_MAX on the client so a
             * directory's full content survives end-to-end. Stack-
             * resident; each ServerMapEntry is ~152 bytes → ~76 KB,
             * fine for any normal thread stack. */
            ServerMapEntry entries[LOBBY_MAP_LIST_MAX];
            int got = -1;
            if (safe) {
                got = serverSimEnumerateMapDir(sim,
                    relPath[0] == '\0' ? NULL : relPath,
                    entries, LOBBY_MAP_LIST_MAX);
            }
            if (got < 0) got = 0;

            /* Chunked send: each frame fits in UDP_MAX_PAYLOAD and
             * carries [header][pathLen][path][final][count][entries].
             * Last chunk sets final=1; empty result is a single chunk
             * with count=0, final=1. Lost final-chunk failure mode is
             * accepted — chooser shows a partial list until next req. */
            uint8_t rsp[UDP_MAX_PAYLOAD];
            int i = 0;
            do {
                int rpos = PACKET_HEADER_SIZE;
                packHeader(rsp, PACKET_LOBBY_MAP_LIST_RSP, 0);
                rsp[rpos++] = pathLen;
                if (pathLen > 0) {
                    memcpy(rsp + rpos, relPath, pathLen);
                    rpos += pathLen;
                }
                int finalPos = rpos;
                rsp[rpos++] = 0;
                int countPos = rpos;
                rsp[rpos++] = 0;
                int written = 0;
                for (; i < got; i++) {
                    int nameLen = (int)SDL_strlen(entries[i].name);
                    if (nameLen > 127) nameLen = 127;
                    if (rpos + 1 + nameLen + 1 + 8 > (int)sizeof(rsp)) break;
                    rsp[rpos++] = (uint8_t)nameLen;
                    memcpy(rsp + rpos, entries[i].name, nameLen);
                    rpos += nameLen;
                    rsp[rpos++] = entries[i].isFolder ? 1 : 0;
                    uint64_t mt = (uint64_t)entries[i].modTime;
                    for (int b = 7; b >= 0; b--) {
                        rsp[rpos++] = (uint8_t)((mt >> (b * 8)) & 0xFF);
                    }
                    written++;
                }
                rsp[countPos] = (uint8_t)written;
                rsp[finalPos] = (i >= got) ? 1 : 0;
                srvSendTo(rsp, rpos, fromAddr);
            } while (i < got);
            break;
        }
        case PACKET_LOBBY_MAP_USE_LOCAL: {
            /* [header 8] [totalLen 4] [nameLen 1] [name N]
             *           [relPathLen 1] [relPath M] [md5 32]
             *
             * Pre-upload optimisation: if our local data/maps/<relPath>
             * matches the supplied MD5, install it directly and reply
             * UPLOAD_DONE — no byte transfer needed. On any miss
             * (permission, path/name unsafe, file missing, MD5
             * mismatch) reply MAP_USE_LOCAL_NACK and let the client
             * fall back to the regular UPLOAD_BEGIN + bulk-stream flow. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 4 + 1 + 1 + 32) break;
            int rpos = PACKET_HEADER_SIZE;
            uint32_t totalLen =
                ((uint32_t)buf[rpos + 0] << 24) |
                ((uint32_t)buf[rpos + 1] << 16) |
                ((uint32_t)buf[rpos + 2] <<  8) |
                ((uint32_t)buf[rpos + 3]);
            rpos += 4;
            uint8_t nameLen = buf[rpos++];
            if (nameLen == 0 || nameLen > 127 ||
                rpos + nameLen + 1 + 32 > (int)len) break;
            char nameBuf[128];
            memset(nameBuf, 0, sizeof(nameBuf));
            memcpy(nameBuf, buf + rpos, nameLen);
            rpos += nameLen;
            uint8_t relLen = buf[rpos++];
            if (relLen == 0 || relLen > 255 ||
                rpos + relLen + 32 > (int)len) break;
            char relBuf[256];
            memset(relBuf, 0, sizeof(relBuf));
            memcpy(relBuf, buf + rpos, relLen);
            rpos += relLen;
            char wantMd5Hex[33];
            memcpy(wantMd5Hex, buf + rpos, 32);
            wantMd5Hex[32] = '\0';

            /* NACK helper for every miss path: server echoes the
             * announce name so the client correlates the reply to
             * the right in-flight USE_LOCAL. */
            #define SEND_USE_LOCAL_NACK() do { \
                uint8_t nack[PACKET_HEADER_SIZE + 1 + 128]; \
                int npos = PACKET_HEADER_SIZE; \
                packHeader(nack, PACKET_LOBBY_MAP_USE_LOCAL_NACK, 0); \
                nack[npos++] = (uint8_t)nameLen; \
                if (nameLen > 0) { memcpy(nack + npos, nameBuf, nameLen); npos += nameLen; } \
                srvSendTo(nack, npos, fromAddr); \
            } while (0)

            if (!lobbyClientMayEdit(sim, clientIdx)) {
                SEND_USE_LOCAL_NACK();
                break;
            }

            /* Single-thread the upload slot. USE_LOCAL writes to
             * sim->pendingUpload* the same as UPLOAD_DONE, so a
             * USE_LOCAL landing while another client's upload is
             * in flight would clobber their pending preview. */
            if (lobbyAnyOtherUploadActive(udpServer.clientUploadActive,
                                           clientIdx)) {
                SEND_USE_LOCAL_NACK();
                break;
            }

            /* Name safety: must end in ".map", no separators, no
             * leading dot. Mirrors the UPLOAD_BEGIN guards so a
             * malicious relPath can't bypass them. */
            bool nameSafe = true;
            if (nameBuf[0] == '.') nameSafe = false;
            for (int i = 0; i < nameLen; i++) {
                char ch = nameBuf[i];
                if (ch == '/' || ch == '\\' || ch == ':') {
                    nameSafe = false; break;
                }
            }
            if (nameSafe) {
                if (nameLen < 4 ||
                    SDL_strcasecmp(nameBuf + nameLen - 4, ".map") != 0) {
                    nameSafe = false;
                }
            }
            if (totalLen == 0 || totalLen > LOBBY_MAP_UPLOAD_MAX_BYTES || !nameSafe) {
                SEND_USE_LOCAL_NACK();
                break;
            }

            /* Try to read data/maps/<relPath>. serverSimReadMapFile
             * handles the path-safety check (".." rejection, etc.). */
            uint8_t *bytes = NULL;
            size_t   byteLen = 0;
            if (!serverSimReadMapFile(sim, relBuf, &bytes, &byteLen) ||
                bytes == NULL ||
                (uint32_t)byteLen != totalLen) {
                if (bytes) free(bytes);
                SEND_USE_LOCAL_NACK();
                break;
            }

            uint8_t haveMd5[16];
            md5Compute(bytes, byteLen, haveMd5);
            char haveMd5Hex[33];
            md5ToHex(haveMd5, haveMd5Hex);
            if (memcmp(haveMd5Hex, wantMd5Hex, 32) != 0) {
                free(bytes);
                SEND_USE_LOCAL_NACK();
                break;
            }

            /* MD5 match — install as preview directly from the local
             * file. The file already lives at its final path, so we
             * just reload it; there is no temp-staging step in the
             * in-memory upload model. */
            free(bytes);  /* serverSimReloadMap re-reads it via its own path */

            char localPath[FILENAME_MAX];
            SDL_snprintf(localPath, sizeof(localPath), "%s/%s",
                         serverSimGetMapDirRoot(sim), relBuf);
            bool previewed = false;
            if (serverSimReloadMap(sim, localPath)) {
                /* Display name: the announce name without ".map". */
                char displayName[MAP_STR_SIZE];
                SDL_strlcpy(displayName, nameBuf, sizeof(displayName));
                {
                    size_t dlen = SDL_strlen(displayName);
                    if (dlen >= 4 &&
                        SDL_strcasecmp(displayName + dlen - 4, ".map") == 0) {
                        displayName[dlen - 4] = '\0';
                    }
                }
                serverSimSetMapName(sim, displayName);
                previewed = true;
            }

            uint8_t done[PACKET_HEADER_SIZE + 2 + 256];
            int dpos = PACKET_HEADER_SIZE;
            packHeader(done, PACKET_LOBBY_MAP_UPLOAD_DONE, 0);
            done[dpos++] = previewed ? 0 : LOBBY_REJECT_INVALID;
            done[dpos++] = (uint8_t)relLen;
            if (relLen > 0) { memcpy(done + dpos, relBuf, relLen); dpos += relLen; }
            srvSendTo(done, dpos, fromAddr);
            #undef SEND_USE_LOCAL_NACK
            break;
        }
        case PACKET_LOBBY_MAP_UPLOAD_BEGIN: {
            /* [header 8] [totalLen 4] [nameLen 1] [name N] — only host
             * / admin / openHost may push files. Per-client wire-only
             * ACK (handshake/reliability). */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 5) break;
            /* Cooldown gate — silent break used to leave the client at
             * upload-status=1 (BEGIN sent, awaiting ACK) indefinitely,
             * jamming further picks. Reply with COOLDOWN so the client's
             * upload pump transitions to status=4 and frees the slot. */
            if (udpServer.clientReqCooldownTicks[clientIdx] > 0) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_COOLDOWN;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_NOT_HOST;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_LOCKED;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            if (udpServer.uploadPolicy == UPLOAD_POLICY_OFF) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_UPLOAD_DISABLED;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            /* Single-thread the upload slot. The sim has one preview
             * pending-upload slot; allowing two clients to race
             * truncates the loser's bytes on the global temp file
             * and overwrites their pending paths. Same-client
             * retry is fine — the clientUploadBuf cleanup below
             * handles that — only OTHER clients trigger this gate. */
            if (lobbyAnyOtherUploadActive(udpServer.clientUploadActive,
                                           clientIdx)) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_UPLOAD_BUSY;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            uint32_t totalLen =
                ((uint32_t)buf[PACKET_HEADER_SIZE + 0] << 24) |
                ((uint32_t)buf[PACKET_HEADER_SIZE + 1] << 16) |
                ((uint32_t)buf[PACKET_HEADER_SIZE + 2] <<  8) |
                ((uint32_t)buf[PACKET_HEADER_SIZE + 3]);
            uint8_t nameLen = buf[PACKET_HEADER_SIZE + 4];
            if (nameLen == 0 || nameLen > 127 ||
                len < PACKET_HEADER_SIZE + 5 + nameLen ||
                totalLen == 0 || totalLen > LOBBY_MAP_UPLOAD_MAX_BYTES) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_INVALID;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }
            char nameBuf[128];
            memset(nameBuf, 0, sizeof(nameBuf));
            memcpy(nameBuf, buf + PACKET_HEADER_SIZE + 5, nameLen);

            if (!uploadFilenameIsSafe(nameBuf, nameLen)) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_INVALID;
                srvSendTo(ack, sizeof(ack), fromAddr);
                break;
            }

            if (udpServer.uploadPolicy == UPLOAD_POLICY_PERSIST) {
                ServerMapEntry entries[256];
                int got = serverSimEnumerateMapDir(sim, "Uploads",
                                                    entries,
                                                    (int)(sizeof(entries) /
                                                          sizeof(entries[0])));
                if (got < 0) got = 0;
                int fileCount = 0;
                uint64_t totalBytes = 0;
                for (int i = 0; i < got; i++) {
                    if (!entries[i].isFolder) {
                        fileCount++;
                        totalBytes += (uint64_t)entries[i].size;
                    }
                }
                if (fileCount >= udpServer.uploadMaxFiles ||
                    totalBytes + totalLen > udpServer.uploadMaxStorageBytes) {
                    uint8_t ack[PACKET_HEADER_SIZE + 1];
                    packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                    ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_UPLOAD_LIMIT_HIT;
                    srvSendTo(ack, sizeof(ack), fromAddr);
                    break;
                }
            }

            udpServer.clientUploadActive[clientIdx] = true;
            udpServer.clientUploadTotal[clientIdx]  = totalLen;
            SDL_strlcpy(udpServer.clientUploadName[clientIdx], nameBuf,
                        sizeof(udpServer.clientUploadName[clientIdx]));
            /* Fresh receiver for this transfer; the bulk stream that follows
             * carries the bytes (no offset reassembly). */
            bulkReceiverInit(&udpServer.bulkRecvUp[clientIdx]);

            uint8_t ack[PACKET_HEADER_SIZE + 1];
            packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
            ack[PACKET_HEADER_SIZE] = 0;
            srvSendTo(ack, sizeof(ack), fromAddr);
            break;
        }
        case PACKET_LOBBY_MAP_SEARCH_REQ: {
            /* [header 8] [pathLen 1] [path N] [queryLen 1] [query M].
             * Recursive search of data/maps/<path> for .map files
             * whose basename contains <query>. Read-only, any
             * connected client may issue. Per-client wire-only RSP. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 2) break;
            if (udpServer.clientReqCooldownTicks[clientIdx] > 0) break;
            udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
            int rpos = PACKET_HEADER_SIZE;
            uint8_t pathLen = buf[rpos++];
            if (pathLen > 255 ||
                rpos + pathLen + 1 > (int)len) break;
            char relPath[256];
            memset(relPath, 0, sizeof(relPath));
            if (pathLen > 0) {
                memcpy(relPath, buf + rpos, pathLen);
            }
            rpos += pathLen;
            uint8_t qLen = buf[rpos++];
            if (qLen > 127 || rpos + qLen > (int)len) break;
            char query[128];
            memset(query, 0, sizeof(query));
            if (qLen > 0) {
                memcpy(query, buf + rpos, qLen);
            }

            ServerMapEntry entries[LOBBY_MAP_LIST_MAX];
            int got = serverSimSearchMapDir(sim,
                relPath[0] == '\0' ? NULL : relPath,
                query, entries, LOBBY_MAP_LIST_MAX);
            if (got < 0) got = 0;

            /* Chunked send — every chunk repeats the full path+query
             * prefix so the client can filter stale responses from a
             * prior navigation. Final chunk sets final=1; empty
             * result is a single chunk with count=0, final=1. */
            uint8_t rsp[UDP_MAX_PAYLOAD];
            int i = 0;
            do {
                int wpos = PACKET_HEADER_SIZE;
                packHeader(rsp, PACKET_LOBBY_MAP_SEARCH_RSP, 0);
                rsp[wpos++] = pathLen;
                if (pathLen > 0) {
                    memcpy(rsp + wpos, relPath, pathLen);
                    wpos += pathLen;
                }
                rsp[wpos++] = qLen;
                if (qLen > 0) {
                    memcpy(rsp + wpos, query, qLen);
                    wpos += qLen;
                }
                int finalPos = wpos;
                rsp[wpos++] = 0;
                int countPos = wpos;
                rsp[wpos++] = 0;
                int written = 0;
                for (; i < got; i++) {
                    int nameLen = (int)SDL_strlen(entries[i].name);
                    if (nameLen > 127) nameLen = 127;
                    if (wpos + 1 + nameLen + 1 + 8 > (int)sizeof(rsp)) break;
                    rsp[wpos++] = (uint8_t)nameLen;
                    memcpy(rsp + wpos, entries[i].name, nameLen);
                    wpos += nameLen;
                    rsp[wpos++] = entries[i].isFolder ? 1 : 0;
                    uint64_t mt = (uint64_t)entries[i].modTime;
                    for (int b = 7; b >= 0; b--) {
                        rsp[wpos++] = (uint8_t)((mt >> (b * 8)) & 0xFF);
                    }
                    written++;
                }
                rsp[countPos] = (uint8_t)written;
                rsp[finalPos] = (i >= got) ? 1 : 0;
                srvSendTo(rsp, wpos, fromAddr);
            } while (i < got);
            break;
        }
        case PACKET_LOBBY_MAP_PREVIEW_REQ: {
            /* [header 8] [pathLen 1] [path N]. Reads data/maps/<path>
             * from the server's filesystem and streams the bytes back
             * over CHANNEL_BULK behind a stream header. Client rasterises
             * locally — server has no dep on a renderer or image encoder,
             * and the protocol is the same shape in SP-host (loopback)
             * and MP. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                len < PACKET_HEADER_SIZE + 1) break;
            int rpos = PACKET_HEADER_SIZE;
            uint8_t pathLen = buf[rpos++];
            if (pathLen == 0 || pathLen > 255 ||
                rpos + pathLen > len) break;
            char relPath[256];
            memset(relPath, 0, sizeof(relPath));
            memcpy(relPath, buf + rpos, pathLen);

            uint8_t *mapBytes = NULL;
            size_t   mapLen   = 0;
            bool ok = serverSimReadMapFile(sim, relPath,
                                            &mapBytes, &mapLen);
            if (!ok) {
                uint8_t err[PACKET_HEADER_SIZE + 1 + 256 + 1];
                packHeader(err, PACKET_LOBBY_MAP_PREVIEW_ERR, 0);
                int wpos = PACKET_HEADER_SIZE;
                err[wpos++] = pathLen;
                memcpy(err + wpos, relPath, pathLen);
                wpos += pathLen;
                err[wpos++] = 1;  /* not-found / unreadable */
                srvSendTo(err, wpos, fromAddr);
                break;
            }

            /* One transfer at a time on this client's bulk byte stream: if a
             * transfer is already in flight, reject with PREVIEW_ERR and let
             * the client re-request via PREVIEW_REQ once it drains. */
            if (bulkSenderBusy(&udpServer.bulkSend[clientIdx])) {
                uint8_t err[PACKET_HEADER_SIZE + 1 + 256 + 1];
                packHeader(err, PACKET_LOBBY_MAP_PREVIEW_ERR, 0);
                int wpos = PACKET_HEADER_SIZE;
                err[wpos++] = pathLen;
                memcpy(err + wpos, relPath, pathLen);
                wpos += pathLen;
                err[wpos++] = 3;  /* transient: bulk channel busy */
                srvSendTo(err, wpos, fromAddr);
                free(mapBytes);
                break;
            }

            /* Monotonic per-process preview sequence, carried in the stream
             * header so the client can match a completed blob to the request
             * it issued. udpServer is a single global, so a process-wide
             * counter is fine; collisions across long sessions wrap harmlessly. */
            static uint32_t s_previewSeq = 0;
            uint32_t seq = ++s_previewSeq;

            /* Frame the preview as a sized blob on CHANNEL_BULK: the stream
             * header (kind/gen/total/path) then the map bytes, fed onto the
             * reliable stream by bulkSenderPump as the window drains. */
            BulkStreamHeader sh;
            memset(&sh, 0, sizeof(sh));
            sh.kind = BULK_KIND_PREVIEW;
            sh.gen = seq;
            sh.totalSize = (uint32_t)mapLen;
            sh.pathLen = pathLen;
            memcpy(sh.path, relPath, pathLen);
            sh.path[pathLen] = '\0';

            if (!bulkSenderBegin(&udpServer.bulkSend[clientIdx], &sh,
                                 mapBytes, (uint32_t)mapLen)) {
                uint8_t err[PACKET_HEADER_SIZE + 1 + 256 + 1];
                packHeader(err, PACKET_LOBBY_MAP_PREVIEW_ERR, 0);
                int wpos = PACKET_HEADER_SIZE;
                err[wpos++] = pathLen;
                memcpy(err + wpos, relPath, pathLen);
                wpos += pathLen;
                err[wpos++] = 3;  /* internal: could not stage transfer */
                srvSendTo(err, wpos, fromAddr);
            }
            free(mapBytes);
            break;
        }
        case PACKET_ROUND_LOG_REQ: {
            /* [header 8] [reqSeq 4 BE]. Hands back the last completed round's
             * .wbv over CHANNEL_BULK behind a BULK_KIND_ROUND_LOG stream
             * header, so a client that joined after the round can replay what
             * its lobby recap describes. Gates run cheapest-refusal first and
             * every one of them answers with PACKET_ROUND_LOG_ERR. */
            uint32_t reqSeq;
            int clientIdx;
            int j;
            int concurrent = 0;
            ServerState st;
            uint8_t *blob = NULL;
            uint32_t blobLen = 0;
            char logName[BULK_PATH_MAX + 1];
            size_t nameLen;
            RoundLogReadResult rr;
            BulkStreamHeader sh;

            if (len < PACKET_HEADER_SIZE + 4) break;
            reqSeq = unpackU32(buf + PACKET_HEADER_SIZE);

            /* Players only. A spectator's bulk sender is saturated by the
             * delayed feed it connected to receive, so serving one there
             * would starve the thing it came for. An address that is neither
             * gets nothing: there is no session to answer, and this file's
             * other lobby handlers drop unknown senders the same way. */
            clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0) {
                if (serverFindSpectator(fromAddr) >= 0) {
                    serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_DISABLED,
                                          fromAddr);
                }
                break;
            }

            /* Lobby and game-over only. While a game runs CHANNEL_BULK
             * belongs to joiner map downloads and desync resyncs, so the
             * refusal is transient — the state will change. */
            st = serverSimGetState(sim);
            if (st != serverStateLobby && st != serverStateGameOver) {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_BUSY, fromAddr);
                break;
            }

            /* No source registered means no recorder in this build, which the
             * transport reports as "disabled" without knowing the recorder
             * exists. Serve policy is asked per request, never latched. */
            if (!s_roundLogSourceSet || !s_roundLogSource.serveEnabled()) {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_DISABLED, fromAddr);
                break;
            }

            /* Per-client bounds. A busy sender means this client already has a
             * transfer on its byte stream — its own round log, or a map
             * preview draining — and the interval plus the per-round ceiling
             * stop it re-asking in a loop. */
            if (bulkSenderBusy(&udpServer.bulkSend[clientIdx]) ||
                udpServer.roundLogServed[clientIdx] >= ROUND_LOG_MAX_ATTEMPTS ||
                (udpServer.roundLogReqSeen[clientIdx] &&
                 (uint32_t)(udpServer.tickCount -
                            udpServer.roundLogLastReqTick[clientIdx]) <
                     ROUND_LOG_MIN_REQ_TICKS)) {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_RATE_LIMITED,
                                      fromAddr);
                break;
            }
            /* Stamp the clock for every request the server considers, served
             * or not, so the interval holds whatever the answer turns out to
             * be. */
            udpServer.roundLogReqSeen[clientIdx] = true;
            udpServer.roundLogLastReqTick[clientIdx] = udpServer.tickCount;

            /* Fleet-wide cap. Transient: the client retries and gets in when
             * one of the transfers ahead of it finishes. */
            for (j = 0; j < MAX_TANKS; j++) {
                if (bulkSenderBusy(&udpServer.bulkSend[j]) &&
                    udpServer.bulkSend[j].kind == BULK_KIND_ROUND_LOG) {
                    concurrent++;
                }
            }
            if (concurrent >= ROUND_LOG_MAX_CONCURRENT) {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_BUSY, fromAddr);
                break;
            }

            /* The source owns the cap check, so an over-cap log is refused
             * without ever being read. A read failure reports as "none": the
             * round is not gettable and the client can do nothing different
             * with the distinction. */
            rr = s_roundLogSource.read(&blob, &blobLen, logName,
                                       sizeof(logName));
            if (rr != ROUND_LOG_READ_OK) {
                serverSendRoundLogErr(reqSeq,
                                      (rr == ROUND_LOG_READ_TOO_LARGE)
                                          ? ROUND_LOG_ERR_TOO_LARGE
                                          : ROUND_LOG_ERR_NONE,
                                      fromAddr);
                break;
            }

            /* gen echoes reqSeq so the client can match the blob to the
             * request it issued and drop a superseded one; the path carries
             * the basename only. */
            memset(&sh, 0, sizeof(sh));
            sh.kind = BULK_KIND_ROUND_LOG;
            sh.gen = reqSeq;
            sh.totalSize = blobLen;
            nameLen = strlen(logName);
            if (nameLen > BULK_PATH_MAX) nameLen = BULK_PATH_MAX;
            sh.pathLen = (uint8_t)nameLen;
            memcpy(sh.path, logName, nameLen);
            sh.path[nameLen] = '\0';

            if (bulkSenderBegin(&udpServer.bulkSend[clientIdx], &sh,
                                blob, blobLen)) {
                udpServer.roundLogServed[clientIdx]++;
            } else {
                serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_BUSY, fromAddr);
            }
            free(blob);   /* bulkSenderBegin copied it into its own buffer */
            break;
        }
        case PACKET_WBN_REAUTH: {
            /* Wire: [header 8] [wbnJoinKey 65]. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0) break;
            ClientCommand cmd;
            if (!commandCodecDecode(buf, len, &cmd)) break;
            (void)serverSimApplyCommand(sim, clientIdx, &cmd);
            break;
        }
        case PACKET_PUNCH_NOTIFY: {
            /* Wire: [header 8] [joiner reflexive IP 4 BE] [joiner port 2 BE].
             * Total 14 bytes. Tracker pushed this through our keepalive's NAT
             * mapping; the body tells us where to fire punch packets. */
            if (len < PACKET_HEADER_SIZE + 6) break;
            int slot;
            for (slot = 0; slot < PUNCH_QUEUE_SIZE; slot++) {
                if (punchQueue[slot].packetsRemaining == 0) break;
            }
            if (slot >= PUNCH_QUEUE_SIZE) break;
            memset(&punchQueue[slot].addr, 0, sizeof(punchQueue[slot].addr));
            punchQueue[slot].addr.sin_family = AF_INET;
            memcpy(&punchQueue[slot].addr.sin_addr, buf + PACKET_HEADER_SIZE, 4);
            punchQueue[slot].addr.sin_port =
                htons(unpackU16(buf + PACKET_HEADER_SIZE + 4));
            punchQueue[slot].packetsRemaining = PUNCH_BURST_PACKETS;
            punchQueue[slot].ticksUntilNext   = 0;
            break;
        }
        case PACKET_PUNCH_PROBE_REPLY: {
            /* Wire: [header 8] [reflexive IP 4 bytes network order]
             *       [reflexive port 2 bytes BE]. Total 14 bytes. */
            if (len < PACKET_HEADER_SIZE + 6) break;
            char reflexiveIp[64];
            uint16_t reflexivePort;
            struct in_addr addr;
            memcpy(&addr.s_addr, buf + PACKET_HEADER_SIZE, 4);
            {
                const char *s = inet_ntoa(addr);
                if (s == NULL) break;
                strncpy(reflexiveIp, s, sizeof(reflexiveIp) - 1);
                reflexiveIp[sizeof(reflexiveIp) - 1] = '\0';
            }
            reflexivePort = unpackU16(buf + PACKET_HEADER_SIZE + 4);
            serverInstanceRecordProbeReply(reflexiveIp, reflexivePort);
            break;
        }
        default:
            break;
        }
}

/* Drain sim events into per-client reliable queues.
 * Must be called after each serverSimTick() so events survive
 * being cleared at the start of the next tick.
 * The three sound events are culled against SDIST_NONE measured from the
 * recipient's own tank and deduplicated per sound type — only the closest
 * instance of each type is sent, and it goes out carrying a near/far tier and
 * a compass bearing in place of the map square it was raised at. */
void transportUdpServerDrainEvents(ServerSim *sim) {
    int i, c;

    if (!udpServer.running) return;

    /* Once-per-second map-event-drop summary. A dropped EVENT_MAP_CHANGE
     * leaves that square owed until a sweep re-sends it, so surface how often
     * the drop guard is firing. Mirrors the [netimpair]
     * once-per-second pattern; only emitted when at least one slot has dropped
     * something, to keep clean logs quiet. */
    {
        static uint64_t lastMapDropLogMs = 0;
        uint64_t nowMs = (uint64_t)SDL_GetTicks();
        if (nowMs - lastMapDropLogMs >= 1000) {
            uint32_t total = 0;
            for (c = 0; c < MAX_TANKS; c++) total += udpServer.mapEventQueueDrops[c];
            lastMapDropLogMs = nowMs;
            if (total > 0) {
                char perSlot[256];
                int p = 0;
                perSlot[0] = '\0';
                for (c = 0; c < MAX_TANKS; c++) {
                    if (udpServer.mapEventQueueDrops[c] == 0) continue;
                    p += snprintf(perSlot + p, sizeof(perSlot) - (size_t)p,
                                  " p%d=%u", c,
                                  (unsigned)udpServer.mapEventQueueDrops[c]);
                    if (p >= (int)sizeof(perSlot)) break;
                }
                mpDiagLog("[netstat] mapEventDrops total=%u%s", (unsigned)total, perSlot);
            }
        }
    }

    /* A tick with nothing to say still has sweep work to do while any slot is
     * culled — that slot's copy is behind by whatever it has not been sent,
     * and the sweep is the only thing that pays it back. */
    if (serverSimGetEventCount(sim) == 0 && serverSimGetMapEventCount(sim) == 0 &&
        serverSimGetShadowCulledMask(sim) == 0) return;

    for (c = 0; c < MAX_TANKS; c++) {
        WORLD cwx = 0, cwy = 0;
        BYTE clientMX = 0, clientMY = 0;
        bool hasPos;
        bool culled;

        if (!udpServer.clients[c].connected) continue;

        culled = serverSimIsShadowCulled(sim, (BYTE)c);

        /* The recipient's visibility set: its tank screen plus a screen for
         * each allied pillbox, base and tank its view policies allow. Built
         * once per client here because both the map-event cull below and the
         * best-effort fx cull further down want the same rects. */
        ViewportRect fxViewports[MAX_VIEWPORTS];
        int fxViewportCount = serverSimBuildViewports(sim, (BYTE)c, fxViewports,
                                                      MAX_VIEWPORTS);

        /* Always enqueue map events, even during map download. The map
         * snapshot was taken when the client joined, so any map changes
         * that happen during the download window must be queued here.
         * They'll be sent once downloadComplete becomes true (the send
         * path in transportUdpServerSend checks downloadComplete
         * separately). Without this, mid-game joiners permanently
         * desync because map events during download are lost. */
        {
            ClientEventQueue *mq = &udpServer.mapEventQueues[c];
            uint32_t dropped = 0;
            for (i = 0; i < (int)serverSimGetMapEventCount(sim); i++) {
                const GameEvent *mev = &serverSimGetMapEvents(sim)[i];
                /* Ground this client cannot see: not sent, and its copy of the
                 * terrain deliberately left holding the old square. That
                 * staleness is the record of what is owed, and the sweep below
                 * pays it if the client ever gets a view of the square. */
                if (culled && !inAnyViewport(fxViewports, fxViewportCount,
                                             mev->data[0], mev->data[1])) {
                    continue;
                }
                if (!eventQueueHasSpace(mq)) {
                    dropped++;
                    continue;
                }
                uint32_t idx = mq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                mq->buffer[idx].event = *mev;
                mq->buffer[idx].seq = mq->nextSeq;
                mq->nextSeq++;
                /* Queued, so this client is owed nothing more for the square:
                 * write it into that slot's copy. Tied to the enqueue on
                 * purpose — a drop above leaves the copy stale and the sweep
                 * heals it. */
                if (culled) {
                    serverSimShadowApplySlot(sim, (BYTE)c, mev->data[0],
                                             mev->data[1], mev->data[2]);
                }
            }
            if (dropped > 0) {
                udpServer.mapEventQueueDrops[c] += dropped;
                fprintf(stderr, "[UDP SERVER] Map event queue full for client %d, dropping %u events\n",
                        c, (unsigned)dropped);
            }
        }

        /* Game events (sounds, kills, etc.) only matter once the client
         * is in-game with a loaded map — skip if still downloading. */
        if (!udpServer.mapDownload[c].downloadComplete) continue;

        /* Catch-up sweep: compare this slot's copy of the terrain against the
         * live map inside the rects above and queue whatever it is behind on.
         * One slot per tick on a stride keeps the per-tick cost flat. Held off
         * while a resync is in flight — that transfer carries the slot's copy
         * whole, and its queue cut would throw the corrections away anyway. */
        if (culled && !udpServer.mapDownload[c].resyncInProgress &&
            (serverSimGetTick(sim) % MAP_SWEEP_STRIDE) ==
                (uint32_t)(c % MAP_SWEEP_STRIDE)) {
            ClientEventQueue *mq = &udpServer.mapEventQueues[c];
            GameEvent sweep[MAP_SWEEP_MAX_EVENTS];
            uint32_t depth = mq->nextSeq - mq->ackedSeq;
            int space = (depth >= (uint32_t)RELIABLE_EVENT_BUFFER_SIZE)
                            ? 0
                            : (int)((uint32_t)RELIABLE_EVENT_BUFFER_SIZE - depth);
            int want = (space < MAP_SWEEP_MAX_EVENTS) ? space : MAP_SWEEP_MAX_EVENTS;
            int got, k;
            /* Asking for no more than the queue holds is what makes the sweep's
             * write-through safe: every event it returns is queued below. */
            if (want > 0) {
                got = serverSimShadowSweep(sim, (BYTE)c, fxViewports,
                                           fxViewportCount, sweep, want);
                for (k = 0; k < got; k++) {
                    uint32_t idx = mq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                    mq->buffer[idx].event = sweep[k];
                    mq->buffer[idx].seq = mq->nextSeq;
                    mq->nextSeq++;
                }
            }
        }

        hasPos = serverSimGetTankState(sim, (BYTE)c, &cwx, &cwy);
        if (hasPos) {
            clientMX = (BYTE)(cwx >> 8);
            clientMY = (BYTE)(cwy >> 8);
        }

        /* Client's closest neutral/allied base drives the arrival push; the
         * per-base stock cull below keeps stock for every neutral/allied base
         * within this same send range, not just the closest. */
        WORLD stockRange = serverSimClosestBaseSendRange(sim, (BYTE)c);
        BYTE closestBase = BASE_NOT_FOUND;
        if (hasPos) {
            closestBase = basesGetClosestForPlayer(serverSimGetGameSim(sim), (BYTE)c, cwx, cwy, stockRange);
        }

        /* On arrival (closest base changed) push that base's current stock
         * immediately so ammo appears at once instead of lagging to the next
         * full-sync. Consuming the change here means the later snapshot build
         * for this same UDP slot sees no change. */
        {
            GameEvent arrivalEv;
            if (serverSimTakeClosestBaseStock(sim, (BYTE)c, closestBase, &arrivalEv)) {
                uint8_t abuf[GAME_EVENT_MAX_WIRE_SIZE];
                int alen = packGameEvent(abuf, &arrivalEv);
                channelSendBestEffort(&udpServer.channelMux[c], CHANNEL_GAME_EFFECT,
                                      abuf, (uint16_t)alen);
            }
        }

        /* Best-effort explosions are culled to the recipient's tank +
         * owned/allied pillbox viewports, matching the snapshot cull —
         * fxViewports is the set built at the top of this client's pass.
         * Sounds are culled by distance instead, so the rect set has no say in
         * what a recipient hears. */

        /* Pass 1: the closest sound of each type for this client, shaped as
         * a tier and a bearing. soundPickOffer holds every rule about who
         * hears what, shared with the snapshot builder. keepSquare is false
         * here whatever the slot is flagged: a wire recipient is never trusted
         * with the square. */
        SoundPick pick;
        int s;
        soundPickInit(&pick);
        if (hasPos) {
            for (i = 0; i < (int)serverSimGetEventCount(sim); i++) {
                soundPickOffer(&pick, &serverSimGetEvents(sim)[i], (BYTE)c,
                               clientMX, clientMY, false);
            }
        }

        /* Pass 2: route non-sound game events, then deduplicated sounds.
         * Each event goes to the reliable game channel (CHANNEL_GAME) if
         * gameEventIsReliable, otherwise to the best-effort channel
         * (CHANNEL_GAME_EFFECT). Sounds are all best-effort. */
        for (i = 0; i < (int)serverSimGetEventCount(sim); i++) {
            uint8_t evType = serverSimGetEvents(sim)[i].type;
            if (!soundEventIsSound(evType)) {
                /* Per-recipient working copy so a non-closest dead base's stock
                 * event can be reshaped (armour-only) without mutating the
                 * shared event; forceReliable promotes that copy to the
                 * reliable channel. */
                GameEvent evToSend = serverSimGetEvents(sim)[i];
                bool forceReliable = false;
                /* Filter EVENT_MINE_VISIBLE: tank mines (bit 7 set) go to all,
                 * LGM mines go only to the placer and their allies */
                if (evType == EVENT_MINE_VISIBLE) {
                    BYTE sourcePlayer = serverSimGetEvents(sim)[i].data[2];
                    if (sourcePlayer & 0x80) {
                        /* Tank mine — broadcast to all */
                    } else {
                        /* LGM mine — only placer and allies */
                        if (c != sourcePlayer && !playersIsAllie(&serverSimGetGameSim(sim)->plyrs, (BYTE)c, sourcePlayer)) {
                            continue;
                        }
                    }
                }
                /* Distance-cull explosion events */
                if (evType == EVENT_EXPLOSION && hasPos) {
                    if (!inAnyViewport(fxViewports, fxViewportCount,
                                       serverSimGetEvents(sim)[i].data[0],
                                       serverSimGetEvents(sim)[i].data[1])) continue;
                }
                /* Base stock is culled to neutral/allied bases. Exception: a
                 * dead enemy base (armour <= MIN_ARMOUR_CAPTURE) is delivered to
                 * non-friendly recipients too — armour only, with shells/mines
                 * zeroed so its reserve stays hidden — on the reliable channel,
                 * so the shooter unblocks the now-drivable tile promptly and the
                 * one-shot transition can't be dropped. */
                if (evType == EVENT_BASE_STOCK) {
                    BYTE bIdx = serverSimGetEvents(sim)[i].data[0];
                    GameSim *gs = serverSimGetGameSim(sim);
                    BYTE bOwner = (*gs->bs).item[bIdx].owner;
                    bool bFriendly = (bOwner == NEUTRAL) || (bOwner == (BYTE)c) ||
                                     playersIsAllie(&gs->plyrs, bOwner, (BYTE)c);
                    if (!bFriendly) {
                        if (serverSimGetEvents(sim)[i].data[1] <= MIN_ARMOUR_CAPTURE) {
                            evToSend.data[2] = 0;
                            evToSend.data[3] = 0;
                            forceReliable = true;
                        } else {
                            continue;
                        }
                    }
                }
                /* A pill this recipient cannot see keeps the square it was last
                 * given, rewritten into its own copy of the event rather than
                 * dropped — the event is the only carrier for that pill's
                 * armour, owner and in-tank flag between full syncs. */
                if (evType == EVENT_PILL_UPDATE) {
                    serverSimFogPillUpdateEvent(sim, (BYTE)c, &evToSend,
                                                fxViewports, fxViewportCount);
                }
                uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
                int evLen = packGameEvent(evBuf, &evToSend);
                if (forceReliable || gameEventIsReliable(evType)) {
                    if (!channelSend(&udpServer.channelMux[c], CHANNEL_GAME,
                                     evBuf, (uint16_t)evLen)) {
                        /* Channel window full — defer the disconnect off the
                         * event loop, mirroring the control-queue overflow path:
                         * set the deferred-removal flag (drained at a safe point
                         * by transportUdpServerDrainPendingRemovals) and stop. The
                         * flag guard keeps a re-hit from spamming the log. */
                        if (!udpServer.pendingSimRemove[c]) {
                            WB_LOG_ERROR(WB_LOG_CAT_NET,
                                         "game channel overflow for slot %d, deferring disconnect",
                                         c);
                            udpServer.pendingSimRemove[c] = true;
                        }
                        break;
                    }
                } else {
                    /* Ephemeral event — best-effort: never blocks, never
                     * disconnects on overflow (drops oldest). */
                    channelSendBestEffort(&udpServer.channelMux[c],
                                          CHANNEL_GAME_EFFECT, evBuf,
                                          (uint16_t)evLen);
                }
            }
        }
        for (s = 0; s < SOUND_PICK_TYPES; s++) {
            if (pick.has[s]) {
                uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
                int evLen = packGameEvent(evBuf, &pick.ev[s]);
                /* Sounds are ephemeral — best-effort: never blocks, never
                 * disconnects on overflow (drops oldest). */
                channelSendBestEffort(&udpServer.channelMux[c],
                                      CHANNEL_GAME_EFFECT, evBuf,
                                      (uint16_t)evLen);
            }
        }
    }
}

/* Send snapshots and check timeouts */
void transportUdpServerSend(ServerSim *sim) {
    int i;

    if (!udpServer.running) return;

    /* Ahead of the snapshot loop below, so a voice frame received this tick
     * rides this tick's snapshot trailer rather than waiting for the next. */
    serverPumpVoice(sim);

    /* Broadcast snapshots to connected clients that have finished map download */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;

        /* Begin an armed map transfer when the bulk channel is idle and fire
         * completion once the peer has acked it through (drives both the join
         * download and a live resync). */
        serverServiceMapTransfer(i);

        if (!udpServer.mapDownload[i].downloadComplete) {
            /* Still downloading the map: stream it on CHANNEL_BULK. Snapshots
             * are gated until complete, so this standalone carrier is the only
             * server->client bulk path — including for a mid-game joiner while
             * the server is Running (the snapshot trailer that carries the bulk
             * stream in other states is itself gated behind downloadComplete, so
             * without this a running joiner would deadlock). Emit several frames
             * a tick so a large map isn't throttled to ~one frame/tick; the
             * unacked window bounds the bytes actually in flight. */
            int frames;
            bulkSenderPump(&udpServer.bulkSend[i], &udpServer.channelMux[i]);
            channelTick(&udpServer.channelMux[i], udpServer.tickCount,
                        udpServer.clients[i].pingMs);
            for (frames = 0; frames < MAP_DOWNLOAD_FRAMES_PER_TICK; frames++) {
                uint8_t cbuf[UDP_MAX_PAYLOAD];
                int frameLen = channelBuildFrame(
                    &udpServer.channelMux[i], cbuf + PACKET_HEADER_SIZE,
                    UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
                if (frameLen <= 2) break;   /* nothing left to carry this tick */
                packHeader(cbuf, PACKET_CHANNEL,
                           udpServer.clients[i].outSequence++);
                srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen,
                          &udpServer.clients[i].addr);
            }
            continue;
        }

        serverSendSnapshot(sim, i);
    }

    /* Seed connected spectators from the delayed ring and drain the seed over
     * CHANNEL_BULK (mirrors the per-client map-download carrier above). */
    serverServiceSpectators(sim);

    transportUdpServerCheckTimeouts(sim);
}

/* Check for client timeouts — call from any server state (lobby, running, etc.) */
void transportUdpServerCheckTimeouts(ServerSim *sim) {
    int i;

    if (!udpServer.running) return;

    /* Outside a running game this is the once-per-tick path, so voice is
     * carried from here — ahead of the standalone PACKET_CHANNEL frames
     * built below, which are its only carrier in the lobby. While running,
     * transportUdpServerSend has already pumped it before the snapshots. */
    if (serverSimGetState(sim) != serverStateRunning) {
        serverPumpVoice(sim);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;

        /* Service the parallel channel layer once per tick.  During running
         * the snapshot trailer (serverSendSnapshot) is the carrier, so this
         * only ticks + carries when snapshots aren't flowing (lobby / countdown
         * / gameover — transportUdpServerSend isn't called there): begin/complete
         * an armed map transfer, tick the mux, and emit standalone PACKET_CHANNEL
         * frames carrying the channel data. A quiet lobby builds one empty frame
         * and suppresses it; a map download in lobby has a backlog, so emit up to
         * MAP_DOWNLOAD_FRAMES_PER_TICK frames (each build is destructive, so the
         * loop stops as soon as a frame comes back empty). */
        if (serverSimGetState(sim) != serverStateRunning) {
            int frames;
            serverServiceMapTransfer(i);
            bulkSenderPump(&udpServer.bulkSend[i], &udpServer.channelMux[i]);
            channelTick(&udpServer.channelMux[i], udpServer.tickCount,
                        udpServer.clients[i].pingMs);
            for (frames = 0; frames < MAP_DOWNLOAD_FRAMES_PER_TICK; frames++) {
                uint8_t cbuf[UDP_MAX_PAYLOAD];
                int frameLen = channelBuildFrame(
                    &udpServer.channelMux[i], cbuf + PACKET_HEADER_SIZE,
                    UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
                if (frameLen <= 2) break;   /* nothing (more) to carry this tick */
                packHeader(cbuf, PACKET_CHANNEL,
                           udpServer.clients[i].outSequence++);
                srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen,
                          &udpServer.clients[i].addr);
            }
        }

        /* Anonymous-fallback for a deferred WBN PLAYER_JOIN: the joiner's
         * reauth never landed within the grace window (direct-IP, not
         * signed in, or WBN unreachable), so announce the join un-keyed. */
        if (wbnJoinOnTick(&udpServer.clients[i].wbnJoin, udpServer.tickCount)) {
            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                               (BYTE)i, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
        }

        if (udpServer.tickCount - udpServer.clients[i].lastReceivedTick
            > CLIENT_TIMEOUT_TICKS) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                "timeout: slot=%d name='%s' tickCount=%u lastReceived=%u "
                "diff=%u > CLIENT_TIMEOUT_TICKS=%d -> disconnect",
                i, udpServer.clients[i].playerName,
                (unsigned)udpServer.tickCount,
                (unsigned)udpServer.clients[i].lastReceivedTick,
                (unsigned)(udpServer.tickCount - udpServer.clients[i].lastReceivedTick),
                (int)CLIENT_TIMEOUT_TICKS);
            mpDiagLog("[srv] TIMEOUT(no-traffic) slot=%d diff=%u CLIENT_TIMEOUT_TICKS=%d -> disconnect",
                      i,
                      (unsigned)(udpServer.tickCount - udpServer.clients[i].lastReceivedTick),
                      (int)CLIENT_TIMEOUT_TICKS);
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (serverSimIsLobbyEnabled(sim) &&
                (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown)) {
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
        }
    }

    /* Seed connected spectators outside running too (lobby / countdown /
     * gameover — transportUdpServerSend isn't called there, so it can't drive
     * this). When running, transportUdpServerSend already serviced spectators
     * before calling here, so this is gated off to avoid a double service. */
    if (serverSimGetState(sim) != serverStateRunning) {
        serverServiceSpectators(sim);
    }

    /* Age out idle spectators. This only frees a slot whose viewer has gone
     * silent; the seed/feed servicing happens in serverServiceSpectators. */
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (!udpServer.spectators[i].connected) continue;
        if (udpServer.tickCount - udpServer.spectators[i].lastReceivedTick
            > CLIENT_TIMEOUT_TICKS) {
            mpDiagLog("[srv] SPECTATOR TIMEOUT idx=%d diff=%u CLIENT_TIMEOUT_TICKS=%d",
                      i,
                      (unsigned)(udpServer.tickCount - udpServer.spectators[i].lastReceivedTick),
                      (int)CLIENT_TIMEOUT_TICKS);
            serverDisconnectSpectator(sim, i, false);
        }
    }
}


int transportUdpServerGetClientCount(void) {
    int count = 0;
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) count++;
    }
    return count;
}

int transportUdpServerGetSpectatorCount(void) {
    int count = 0;
    int i;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (udpServer.spectators[i].connected) count++;
    }
    return count;
}

uint16_t transportUdpServerGetClientPing(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) return 0;
    if (!udpServer.clients[playerNum].connected) return 0;
    return udpServer.clients[playerNum].pingMs;
}

bool transportUdpServerGetClientAddrStr(BYTE playerNum, char *out, size_t outLen) {
    if (out == NULL || outLen == 0) return false;
    out[0] = '\0';
    if (playerNum >= MAX_TANKS || !udpServer.clients[playerNum].connected) {
        return false;
    }
    const struct sockaddr_in *a = &udpServer.clients[playerNum].addr;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(out, outLen, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
    return true;
}
