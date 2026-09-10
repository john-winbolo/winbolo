/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Dispatch
 *Filename:      udp_server_dispatch.c
 *Author:        John Morrison
 *Purpose:
 *  Every inbound packet the server parses, split out of
 *  transport_udp_server.c: the packet-type switch and one
 *  handler per packet type, reached from both the polled
 *  receive path and the recv-thread drain.
 *    - Liveness and carrier traffic: ping, the standalone
 *      channel frame, the command tick and quit.
 *    - Map traffic: the download re-ask, the live-map
 *      resync request, and the lobby map list, search,
 *      preview, use-local and upload-begin requests.
 *    - The completed round's log request.
 *    - The WinBolo.net reauth, and the two hole-punch
 *      packets the tracker path answers.
 *********************************************************/

#include <stdio.h>   /* fprintf, stderr, FILENAME_MAX */
#include <stdlib.h>  /* malloc, free */
#include <string.h>  /* memcpy, memcmp, memset, strlen, strncpy */

#include "transport_udp_internal.h"        /* packHeader, packU32, unpackU16,
                                            * unpackU32, getPacketType,
                                            * PACKET_HEADER_SIZE, UDP_MAX_PAYLOAD,
                                            * and SDL3/SDL.h for SDL_strlen /
                                            * SDL_strlcpy / SDL_strcasecmp /
                                            * SDL_snprintf */
#include "transport_udp_server_internal.h" /* udpServer, srvSendTo, SpectatorConn,
                                            * ClientMapDownload, MAP_XFER_*,
                                            * punchQueue, PUNCH_QUEUE_SIZE,
                                            * PUNCH_BURST_PACKETS,
                                            * LOBBY_REQ_COOLDOWN_TICKS,
                                            * s_roundLogSource, s_roundLogSourceSet,
                                            * serverSendRoundLogErr, unpackConnId,
                                            * serverFindClient, serverFindSpectator,
                                            * serverDrainBulk, serverCleanupMapDownload,
                                            * serverDisconnectClient,
                                            * serverHandleJoinRequest,
                                            * serverSendJoinAccept,
                                            * serverRebaseBulkAndRearmDownload,
                                            * lobbyClientMayEdit, the five entry
                                            * points transport_udp_server.c still
                                            * owns, and via platform_net.h the
                                            * sockaddr_in / inet_ntoa / htons /
                                            * ntohs the punch and quit paths use */
#include "transport_udp.h"   /* UdpServerClient, RoundLogReadResult,
                              * ROUND_LOG_READ_OK, ROUND_LOG_READ_TOO_LARGE,
                              * lobbyAnyOtherUploadActive, uploadFilenameIsSafe */
#include "netpacks.h"        /* the PACKET_* ids the type switch reads,
                              * LOBBY_REJECT_*, ROUND_LOG_ERR_*,
                              * MAP_DOWNLOAD_MAX_SIZE */
#include "wire_limits.h"     /* LOBBY_MAP_UPLOAD_MAX_BYTES, LOBBY_LOCK_MAP */
#include "global.h"          /* BYTE, MAX_TANKS, MAP_STR_SIZE, TRUE */
#include "bolo_map.h"        /* map, mapCreate, mapDestroy, mapGetPos,
                              * mapCalcChecksum, mapLoadCompressedMap,
                              * MAP_ARRAY_SIZE */
#include "pillbox.h"         /* pillboxes, struct pillsObj, pillsCreate,
                              * pillsDestroy, pillsExistPos */
#include "bases.h"           /* bases, basesCreate, basesDestroy, basesExistPos */
#include "starts.h"          /* starts, startsCreate, startsDestroy */
#include "client_command.h"  /* ClientCommand, CMD_CHAT */
#include "transport_command_codec.h" /* commandCodecDecode */
#include "channel_mux.h"     /* channelRecvFrame */
#include "bulk_transfer.h"   /* BulkStreamHeader, bulkSenderBusy, bulkSenderBegin,
                              * bulkReceiverInit, BULK_KIND_PREVIEW,
                              * BULK_KIND_ROUND_LOG, BULK_PATH_MAX */
#include "upload_policy.h"   /* UPLOAD_POLICY_OFF, UPLOAD_POLICY_PERSIST */
#include "game_sim.h"        /* GameSim — serverSimGetGameSim(sim)->mp / ->pb / ->bs */
#include "server_sim.h"      /* ServerSim, ServerState, ServerMapEntry, and the sim
                              * accessors the handlers read and write */
#include "server_sim_internal.h"  /* sim->clientKnownMap, serverSimGetPillsForSlot,
                                   * serverSimGetCompressedMapFor — sim co-owner */
#include "server_sim_lifecycle.h" /* serverSimGetMapDirRoot */
#include "client_sim_internal.h"  /* LOBBY_MAP_LIST_MAX cap shared with the wire */
#include "../server_lifecycle.h"  /* serverInstanceRecordProbeReply */
#include "../../common/md5.h"     /* md5Compute, md5ToHex */
#include "../../common/wb_log.h"  /* WB_LOG_DEBUG / INFO / WARN, WB_LOG_CAT_NET */

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

static void handlePing(ServerSim *sim, uint8_t *buf, int len,
                       struct sockaddr_in *fromAddr) {
    serverHandlePing(buf, len, fromAddr);
    /* PACKET_PING is the client's steady ~1 Hz keepalive: it is the
     * ONLY traffic a client sends while idle in the lobby. Refresh the
     * client's liveness clock here, or a client that is quietly waiting
     * in the lobby (long survival setup, say) gets timed out and
     * dropped even though it is pinging us every second. The spectator
     * path below already did this; the connected-client path did not. */
    {
        int cIdx = serverFindClient(fromAddr);
        if (cIdx >= 0) {
            udpServer.clients[cIdx].lastReceivedTick = udpServer.tickCount;
        }
        /* Spectators aren't in clients[]; refresh their liveness too. */
        int sIdx = serverFindSpectator(fromAddr);
        if (sIdx >= 0) {
            udpServer.spectators[sIdx].lastReceivedTick = udpServer.tickCount;
        }
    }
}

static void handleChannel(ServerSim *sim, uint8_t *buf, int len,
                          struct sockaddr_in *fromAddr) {
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
        return;
    }
    udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;
    if (channelRecvFrame(&udpServer.channelMux[clientIdx],
                         buf + PACKET_HEADER_SIZE,
                         len - PACKET_HEADER_SIZE) >= 0) {
        udpServer.channelFramesRx[clientIdx]++;
        serverDrainBulk(sim, clientIdx);
    }
}

static void handleCommandTick(ServerSim *sim, uint8_t *buf, int len,
                              struct sockaddr_in *fromAddr) {
    int clientIdx = serverFindClient(fromAddr);
    if (clientIdx < 0) {
        /* A tankless spectator may send CMD_CHAT — and nothing else,
         * and only while the server is in lobby/countdown. This branch
         * is the hard isolation boundary: a viewer has no slot or sim
         * state to mutate, so a decoded command of any other type is
         * dropped here. The cmdSeq dedup mirrors the player path so the
         * spectator's reliable carrier acks and retransmits coherently. */
        int sIdx = serverFindSpectator(fromAddr);
        if (sIdx < 0) return;
        SpectatorConn *sp = &udpServer.spectators[sIdx];
        ServerState st = serverSimGetState(sim);
        bool lobbyish =
            (st == serverStateLobby || st == serverStateCountdown);
        sp->lastReceivedTick = udpServer.tickCount;
        if (len < PACKET_HEADER_SIZE + 1) return;
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
        return;
    }
    UdpServerClient *client = &udpServer.clients[clientIdx];
    /* A command packet from a connected client is proof of life:
     * refresh the timeout clock, exactly as every other client
     * packet path does (and as the spectator branch above does).
     * Without this, a client whose only inbound traffic is
     * COMMAND_TICK -- which is all it sends while sitting in the
     * lobby (ready / team-set / chat / ping) -- goes stale after
     * CLIENT_TIMEOUT_TICKS and the timeout sweep disconnects it
     * mid-lobby or the instant the game starts, even though it is
     * actively talking to us. */
    client->lastReceivedTick = udpServer.tickCount;
    if (len < PACKET_HEADER_SIZE + 1) return;
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
}

static void handleQuit(ServerSim *sim, uint8_t *buf, int len,
                       struct sockaddr_in *fromAddr) {
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
}

static void handleMapDlReady(ServerSim *sim, uint8_t *buf, int len,
                             struct sockaddr_in *fromAddr) {
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
        if (cl->connId != 0 && reqConnId != cl->connId) return;
        cl->lastReceivedTick = udpServer.tickCount;
        if (dl->xferKind == MAP_XFER_RESYNC || dl->resyncInProgress) {
            return;
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
}

static void handleMapResyncRequest(ServerSim *sim, uint8_t *buf, int len,
                                   struct sockaddr_in *fromAddr) {
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
}

static void handleLobbyMapListReq(ServerSim *sim, uint8_t *buf, int len,
                                  struct sockaddr_in *fromAddr) {
    /* [header 8] [pathLen 1] [path N] — any lobby client may
     * ask. Response is sent back to the requester only
     * (wire-only handshake per ARCHITECTURE.md §"Load-bearing
     * wire-only exceptions"). */
    int clientIdx = serverFindClient(fromAddr);
    if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
        serverSimGetState(sim) != serverStateLobby ||
        len < PACKET_HEADER_SIZE + 1) return;
    if (udpServer.clientReqCooldownTicks[clientIdx] > 0) return;
    udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
    uint8_t pathLen = buf[PACKET_HEADER_SIZE];
    if (pathLen > 255 ||
        len < PACKET_HEADER_SIZE + 1 + pathLen) return;
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
}

static void handleLobbyMapUseLocal(ServerSim *sim, uint8_t *buf, int len,
                                   struct sockaddr_in *fromAddr) {
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
        len < PACKET_HEADER_SIZE + 4 + 1 + 1 + 32) return;
    int rpos = PACKET_HEADER_SIZE;
    uint32_t totalLen =
        ((uint32_t)buf[rpos + 0] << 24) |
        ((uint32_t)buf[rpos + 1] << 16) |
        ((uint32_t)buf[rpos + 2] <<  8) |
        ((uint32_t)buf[rpos + 3]);
    rpos += 4;
    uint8_t nameLen = buf[rpos++];
    if (nameLen == 0 || nameLen > 127 ||
        rpos + nameLen + 1 + 32 > (int)len) return;
    char nameBuf[128];
    memset(nameBuf, 0, sizeof(nameBuf));
    memcpy(nameBuf, buf + rpos, nameLen);
    rpos += nameLen;
    uint8_t relLen = buf[rpos++];
    if (relLen == 0 || relLen > 255 ||
        rpos + relLen + 32 > (int)len) return;
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
        return;
    }

    /* Single-thread the upload slot. USE_LOCAL writes to
     * sim->pendingUpload* the same as UPLOAD_DONE, so a
     * USE_LOCAL landing while another client's upload is
     * in flight would clobber their pending preview. */
    if (lobbyAnyOtherUploadActive(udpServer.clientUploadActive,
                                   clientIdx)) {
        SEND_USE_LOCAL_NACK();
        return;
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
        return;
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
        return;
    }

    uint8_t haveMd5[16];
    md5Compute(bytes, byteLen, haveMd5);
    char haveMd5Hex[33];
    md5ToHex(haveMd5, haveMd5Hex);
    if (memcmp(haveMd5Hex, wantMd5Hex, 32) != 0) {
        free(bytes);
        SEND_USE_LOCAL_NACK();
        return;
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
}

static void handleLobbyMapUploadBegin(ServerSim *sim, uint8_t *buf, int len,
                                      struct sockaddr_in *fromAddr) {
    /* [header 8] [totalLen 4] [nameLen 1] [name N] — only host
     * / admin / openHost may push files. Per-client wire-only
     * ACK (handshake/reliability). */
    int clientIdx = serverFindClient(fromAddr);
    if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
        serverSimGetState(sim) != serverStateLobby ||
        len < PACKET_HEADER_SIZE + 5) return;
    /* Cooldown gate — silent break used to leave the client at
     * upload-status=1 (BEGIN sent, awaiting ACK) indefinitely,
     * jamming further picks. Reply with COOLDOWN so the client's
     * upload pump transitions to status=4 and frees the slot. */
    if (udpServer.clientReqCooldownTicks[clientIdx] > 0) {
        uint8_t ack[PACKET_HEADER_SIZE + 1];
        packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
        ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_COOLDOWN;
        srvSendTo(ack, sizeof(ack), fromAddr);
        return;
    }
    udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
    if (!lobbyClientMayEdit(sim, clientIdx)) {
        uint8_t ack[PACKET_HEADER_SIZE + 1];
        packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
        ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_NOT_HOST;
        srvSendTo(ack, sizeof(ack), fromAddr);
        return;
    }
    if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
        uint8_t ack[PACKET_HEADER_SIZE + 1];
        packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
        ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_LOCKED;
        srvSendTo(ack, sizeof(ack), fromAddr);
        return;
    }
    if (udpServer.uploadPolicy == UPLOAD_POLICY_OFF) {
        uint8_t ack[PACKET_HEADER_SIZE + 1];
        packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
        ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_UPLOAD_DISABLED;
        srvSendTo(ack, sizeof(ack), fromAddr);
        return;
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
        return;
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
        return;
    }
    char nameBuf[128];
    memset(nameBuf, 0, sizeof(nameBuf));
    memcpy(nameBuf, buf + PACKET_HEADER_SIZE + 5, nameLen);

    if (!uploadFilenameIsSafe(nameBuf, nameLen)) {
        uint8_t ack[PACKET_HEADER_SIZE + 1];
        packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
        ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_INVALID;
        srvSendTo(ack, sizeof(ack), fromAddr);
        return;
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
            return;
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
}

static void handleLobbyMapSearchReq(ServerSim *sim, uint8_t *buf, int len,
                                    struct sockaddr_in *fromAddr) {
    /* [header 8] [pathLen 1] [path N] [queryLen 1] [query M].
     * Recursive search of data/maps/<path> for .map files
     * whose basename contains <query>. Read-only, any
     * connected client may issue. Per-client wire-only RSP. */
    int clientIdx = serverFindClient(fromAddr);
    if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
        serverSimGetState(sim) != serverStateLobby ||
        len < PACKET_HEADER_SIZE + 2) return;
    if (udpServer.clientReqCooldownTicks[clientIdx] > 0) return;
    udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
    int rpos = PACKET_HEADER_SIZE;
    uint8_t pathLen = buf[rpos++];
    if (pathLen > 255 ||
        rpos + pathLen + 1 > (int)len) return;
    char relPath[256];
    memset(relPath, 0, sizeof(relPath));
    if (pathLen > 0) {
        memcpy(relPath, buf + rpos, pathLen);
    }
    rpos += pathLen;
    uint8_t qLen = buf[rpos++];
    if (qLen > 127 || rpos + qLen > (int)len) return;
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
}

static void handleLobbyMapPreviewReq(ServerSim *sim, uint8_t *buf, int len,
                                     struct sockaddr_in *fromAddr) {
    /* [header 8] [pathLen 1] [path N]. Reads data/maps/<path>
     * from the server's filesystem and streams the bytes back
     * over CHANNEL_BULK behind a stream header. Client rasterises
     * locally — server has no dep on a renderer or image encoder,
     * and the protocol is the same shape in SP-host (loopback)
     * and MP. */
    int clientIdx = serverFindClient(fromAddr);
    if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
        len < PACKET_HEADER_SIZE + 1) return;
    int rpos = PACKET_HEADER_SIZE;
    uint8_t pathLen = buf[rpos++];
    if (pathLen == 0 || pathLen > 255 ||
        rpos + pathLen > len) return;
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
        return;
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
        return;
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
}

static void handleRoundLogReq(ServerSim *sim, uint8_t *buf, int len,
                              struct sockaddr_in *fromAddr) {
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

    if (len < PACKET_HEADER_SIZE + 4) return;
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
        return;
    }

    /* Lobby and game-over only. While a game runs CHANNEL_BULK
     * belongs to joiner map downloads and desync resyncs, so the
     * refusal is transient — the state will change. */
    st = serverSimGetState(sim);
    if (st != serverStateLobby && st != serverStateGameOver) {
        serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_BUSY, fromAddr);
        return;
    }

    /* No source registered means no recorder in this build, which the
     * transport reports as "disabled" without knowing the recorder
     * exists. Serve policy is asked per request, never latched. */
    if (!s_roundLogSourceSet || !s_roundLogSource.serveEnabled()) {
        serverSendRoundLogErr(reqSeq, ROUND_LOG_ERR_DISABLED, fromAddr);
        return;
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
        return;
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
        return;
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
        return;
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
}

static void handleWbnReauth(ServerSim *sim, uint8_t *buf, int len,
                            struct sockaddr_in *fromAddr) {
    /* Wire: [header 8] [wbnJoinKey 65]. */
    int clientIdx = serverFindClient(fromAddr);
    if (clientIdx < 0) return;
    ClientCommand cmd;
    if (!commandCodecDecode(buf, len, &cmd)) return;
    (void)serverSimApplyCommand(sim, clientIdx, &cmd);
}

static void handlePunchNotify(ServerSim *sim, uint8_t *buf, int len,
                              struct sockaddr_in *fromAddr) {
    /* Wire: [header 8] [joiner reflexive IP 4 BE] [joiner port 2 BE].
     * Total 14 bytes. Tracker pushed this through our keepalive's NAT
     * mapping; the body tells us where to fire punch packets. */
    if (len < PACKET_HEADER_SIZE + 6) return;
    int slot;
    for (slot = 0; slot < PUNCH_QUEUE_SIZE; slot++) {
        if (punchQueue[slot].packetsRemaining == 0) break;
    }
    if (slot >= PUNCH_QUEUE_SIZE) return;
    memset(&punchQueue[slot].addr, 0, sizeof(punchQueue[slot].addr));
    punchQueue[slot].addr.sin_family = AF_INET;
    memcpy(&punchQueue[slot].addr.sin_addr, buf + PACKET_HEADER_SIZE, 4);
    punchQueue[slot].addr.sin_port =
        htons(unpackU16(buf + PACKET_HEADER_SIZE + 4));
    punchQueue[slot].packetsRemaining = PUNCH_BURST_PACKETS;
    punchQueue[slot].ticksUntilNext   = 0;
}

static void handlePunchProbeReply(ServerSim *sim, uint8_t *buf, int len,
                                  struct sockaddr_in *fromAddr) {
    /* Wire: [header 8] [reflexive IP 4 bytes network order]
     *       [reflexive port 2 bytes BE]. Total 14 bytes. */
    if (len < PACKET_HEADER_SIZE + 6) return;
    char reflexiveIp[64];
    uint16_t reflexivePort;
    struct in_addr addr;
    memcpy(&addr.s_addr, buf + PACKET_HEADER_SIZE, 4);
    {
        const char *s = inet_ntoa(addr);
        if (s == NULL) return;
        strncpy(reflexiveIp, s, sizeof(reflexiveIp) - 1);
        reflexiveIp[sizeof(reflexiveIp) - 1] = '\0';
    }
    reflexivePort = unpackU16(buf + PACKET_HEADER_SIZE + 4);
    serverInstanceRecordProbeReply(reflexiveIp, reflexivePort);
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
            handlePing(sim, buf, len, fromAddr);
            break;
        case PACKET_CHANNEL:
            handleChannel(sim, buf, len, fromAddr);
            break;
        case PACKET_COMMAND_TICK:
            handleCommandTick(sim, buf, len, fromAddr);
            break;
        case PACKET_QUIT:
            handleQuit(sim, buf, len, fromAddr);
            break;
        case PACKET_MAP_DL_READY:
            handleMapDlReady(sim, buf, len, fromAddr);
            break;
        case PACKET_MAP_RESYNC_REQUEST:
            handleMapResyncRequest(sim, buf, len, fromAddr);
            break;
        case PACKET_LOBBY_MAP_LIST_REQ:
            handleLobbyMapListReq(sim, buf, len, fromAddr);
            break;
        case PACKET_LOBBY_MAP_USE_LOCAL:
            handleLobbyMapUseLocal(sim, buf, len, fromAddr);
            break;
        case PACKET_LOBBY_MAP_UPLOAD_BEGIN:
            handleLobbyMapUploadBegin(sim, buf, len, fromAddr);
            break;
        case PACKET_LOBBY_MAP_SEARCH_REQ:
            handleLobbyMapSearchReq(sim, buf, len, fromAddr);
            break;
        case PACKET_LOBBY_MAP_PREVIEW_REQ:
            handleLobbyMapPreviewReq(sim, buf, len, fromAddr);
            break;
        case PACKET_ROUND_LOG_REQ:
            handleRoundLogReq(sim, buf, len, fromAddr);
            break;
        case PACKET_WBN_REAUTH:
            handleWbnReauth(sim, buf, len, fromAddr);
            break;
        case PACKET_PUNCH_NOTIFY:
            handlePunchNotify(sim, buf, len, fromAddr);
            break;
        case PACKET_PUNCH_PROBE_REPLY:
            handlePunchProbeReply(sim, buf, len, fromAddr);
            break;
        default:
            break;
        }
}
