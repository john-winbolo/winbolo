/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Send
 *Filename:      udp_server_send.c
 *Author:        John Morrison
 *Purpose:
 *  The server transport's outbound per-tick path to a
 *  connected player, split out of transport_udp_server.c.
 *    - Applying an inbound input packet to the sim and
 *      ingesting the channel trailer that rides with it.
 *    - Answering a ping with a pong.
 *    - Building one client's filtered snapshot, appending
 *      its map-event and channel tails, and sending it.
 *    - The per-tick send loop over all clients, which
 *      carries the map download for a slot whose snapshots
 *      are still held back.
 *********************************************************/

#include <stdio.h>   /* snprintf */

#include "transport_udp_internal.h"        /* packHeader, packU16, packU32,
                                            * unpackU32, unpackInputPacket,
                                            * packTankSnapshot, packShellSnapshot,
                                            * packTkExplosionSnapshot,
                                            * packBaseSnapshot, packPillSnapshot,
                                            * packGameEvent, ClientEventQueue,
                                            * PACKET_HEADER_SIZE, UDP_MAX_PAYLOAD,
                                            * INPUT_PACKET_WIRE_SIZE,
                                            * SNAPSHOT_HEADER_WIRE_SIZE,
                                            * RELIABLE_EVENT_BUFFER_SIZE, and
                                            * SDL3/SDL.h for SDL_GetTicks */
#include "transport_udp_server_internal.h" /* udpServer, srvSendTo, unpackConnId,
                                            * serverFindClient, serverDrainBulk,
                                            * serverPumpVoice,
                                            * serverServiceMapTransfer,
                                            * serverServiceSpectators,
                                            * MAP_DOWNLOAD_FRAMES_PER_TICK, and via
                                            * platform_net.h the sockaddr_in /
                                            * inet_ntoa / ntohs the re-home log uses */
#include "transport_udp.h"   /* UdpServerClient, transportUdpServerFindByConnId,
                              * transportUdpServerSend, transportUdpServerCheckTimeouts */
#include "netpacks.h"        /* PACKET_PONG, PACKET_STATE_SNAPSHOT, PACKET_CHANNEL,
                              * INPUT_REDUNDANCY_COUNT */
#include "global.h"          /* BYTE, MAX_TANKS, and BOLO_STATIC_ASSERT via
                              * platform_types.h */
#include "input_packet.h"    /* InputPacket, SnapshotHeader, TankSnapshot,
                              * ShellSnapshot, TkExplosionSnapshot, BaseSnapshot,
                              * PillSnapshot, GameEvent, the MAX_SNAPSHOT_* bounds,
                              * the *_SNAPSHOT_WIRE_SIZE sizes,
                              * TANK_SNAPSHOT_HIDDEN_FLAG, GAME_EVENT_MAX_WIRE_SIZE */
#include "server_sim.h"      /* ServerSim, ServerState, serverStateRunning,
                              * serverSimGetState, serverSimApplyInput,
                              * serverSimGetLastProcessedInput,
                              * serverSimBuildSnapshot */
#include "channel_mux.h"     /* channelRecvFrame, channelSend, channelTick,
                              * channelBuildFrame, CHANNEL_MAP,
                              * channelChargeBestEffortLeftover */
#include "bulk_transfer.h"   /* bulkSenderPump */
#include "../../common/wb_log.h" /* WB_LOG_INFO, WB_LOG_WARN, WB_LOG_CAT_NET */

/* Handle input packet from a connected client */
void serverHandleInput(const uint8_t *buf, int len,
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

        /* pkt.mapEventAck is not read. It dates from when map events rode
         * the snapshot tail and this was their ack; they ride CHANNEL_MAP now
         * with the channel's own acks, and the hold buffer's ackedSeq means
         * "handed to the channel", which only the snapshot drain knows. A
         * client cannot know that, and honouring its claim let a value past
         * nextSeq wrap the space check so every later change for the slot
         * was dropped. The field stays on the wire (clients send 1) so the
         * InputPacket layout is unchanged. Game events ride CHANNEL_GAME and
         * control events CHANNEL_CONTROL, both acked on the channel frame
         * trailer ingested below. */

        /* Only apply if this is a newer input than what we last processed,
         * or if it is the one that breaks a stall-advance lockout: a tick
         * this client has never sent before, arriving stale because the slot
         * has been stall-advanced past everything the client has produced.
         * Dropping that one here is what makes the lockout permanent. */
        if (pkt.tick > serverSimGetLastProcessedInput(sim, clientIdx) ||
            serverSimInputWouldRebase(sim, (BYTE)clientIdx, pkt.tick)) {
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
void serverHandlePing(const uint8_t *buf, int len,
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
     * A full window is backpressure, not a failure: the drain stops where it
     * is, leaves the remainder held, and the next snapshot resumes at the same
     * seq once acks free space. The snapshot no longer carries a map tail. */
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
                /* Window full. Leave this event and everything behind it in
                 * the hold buffer: ackedSeq is not advanced, so the next
                 * snapshot picks up at exactly this seq. One line per stall —
                 * the flag clears only once the queue has drained, so a window
                 * that frees a slot at a time stays quiet while it catches
                 * up. */
                if (!udpServer.mapChannelStalled[clientIdx]) {
                    WB_LOG_WARN(WB_LOG_CAT_NET,
                                "map channel window full for slot %d, holding "
                                "%u event(s) until acks free it",
                                clientIdx, (unsigned)(mapQ->nextSeq - seq));
                    udpServer.mapChannelStalled[clientIdx] = true;
                }
                break;
            }
            mapQ->ackedSeq = seq + 1; /* Sent reliably — free the hold slot. */
        }
        /* Caught up — the next window-full is a new stall and logs again. */
        if (mapQ->ackedSeq == mapQ->nextSeq) {
            udpServer.mapChannelStalled[clientIdx] = false;
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
     * client recovers it as the bytes past the snapshot's parsed end.
     * Anything the trailer had no room for goes out in the standalone frames
     * after the send below. */
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

    /* The trailer above took whatever the snapshot left; these carry what did
     * not fit, so a burst of effects is not thrown away on the tick it
     * happened. Same loop shape as the map-download carrier below. The mux is
     * NOT ticked again — it was ticked once before the trailer was built, and
     * a second tick would double-advance the channel's clock. On a retransmit
     * tick the rewound cursor means these carry resend rather than new data,
     * which the unacked window bounds. */
    {
        int frames;
        for (frames = 0; frames < SNAPSHOT_EXTRA_CHANNEL_FRAMES; frames++) {
            uint8_t cbuf[UDP_MAX_PAYLOAD];
            int frameLen = channelBuildFrame(
                &udpServer.channelMux[clientIdx], cbuf + PACKET_HEADER_SIZE,
                UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
            if (frameLen <= 2) break;   /* nothing left to carry this tick */
            packHeader(cbuf, PACKET_CHANNEL, client->outSequence++);
            srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen, &client->addr);
        }
    }

    /* Every frame this tick has for this client is now built, so whatever is
     * still pending on a best-effort channel is what the tick could not carry.
     * Charged here rather than in channelTick because the producers run before
     * the frames do — transportUdpServerDrainEvents publishes this tick's
     * effects and serverPumpVoice forwards this tick's voice, both ahead of
     * the channelTick above — so a charge at the tick boundary would count
     * everything the tick produced instead of what it lost. This is the only
     * caller, so the counter describes the running path; the lobby and
     * map-download carriers do not charge. */
    channelChargeBestEffortLeftover(&udpServer.channelMux[clientIdx]);
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
        serverServiceMapTransfer(sim, i);

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
