/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Map Transfer
 *Filename:      udp_server_maptransfer.c
 *Author:        John Morrison
 *Purpose:
 *  The map moving in both directions between the server
 *  transport and one client, split out of
 *  transport_udp_server.c.
 *    - Streaming the compressed map down to a joining or
 *      resyncing client over the bulk channel: arming the
 *      transfer, beginning it once the channel is idle,
 *      and reading it complete off the peer's acks.
 *    - Re-basing that channel and re-arming a fresh
 *      download when the old stream has to be abandoned.
 *    - Receiving a lobby map upload back from a client:
 *      the filename check, the bulk reassembly sink, and
 *      the in-memory reload plus optional persist.
 *********************************************************/

#include <stdio.h>   /* fprintf, stderr, fopen, fwrite, fclose, FILENAME_MAX */
#include <stdlib.h>  /* malloc, free */
#include <string.h>  /* memcpy, memset */

#include "transport_udp_internal.h"        /* packHeader, packU16,
                                            * PACKET_HEADER_SIZE, and SDL3/SDL.h
                                            * for SDL_strlcpy / SDL_snprintf /
                                            * SDL_CreateDirectory */
#include "transport_udp_server_internal.h" /* udpServer, ClientMapDownload,
                                            * MAP_XFER_*, srvSendTo */
#include "netpacks.h"        /* PACKET_LOBBY_MAP_UPLOAD_DONE, LOBBY_REJECT_INVALID */
#include "wire_limits.h"     /* LOBBY_MAP_UPLOAD_MAX_BYTES */
#include "global.h"          /* BYTE, MAX_TANKS, MAP_STR_SIZE, TRUE, FALSE */
#include "players.h"         /* playersGetClientFlags, PLAYER_FLAG_ADMIN */
#include "game_sim.h"        /* GameSim — serverSimGetGameSim(sim)->plyrs */
#include "server_sim.h"      /* ServerSim, serverSimGetHostSlot,
                              * serverSimIsPlayerConnected, serverSimGetOpenHost,
                              * serverSimGetGameSim, serverSimFillEntitySyncEvent */
#include "server_sim_lifecycle.h" /* serverSimReloadCompressedInMemory,
                                   * serverSimGetMapDirRoot */
#include "upload_policy.h"   /* UPLOAD_POLICY_PERSIST */
#include "control_event.h"   /* ControlEvent, CTRL_CHANNEL_RESET,
                              * CTRL_ENTITY_SYNC */
#include "transport_control_codec.h" /* ControlEncodeBodyFn, ENCODE_OK,
                                      * transportControlCodecBodyEncoder */
#include "channel_mux.h"     /* ChannelMux, ChannelState, channelSend,
                              * channelReceive, channelResetSend, CHANNEL_BULK,
                              * CHANNEL_CONTROL, CHANNEL_CONTROL_SEG,
                              * CHANNEL_BULK_SEG, CHANNEL_MAX_SEG */
#include "bulk_transfer.h"   /* BulkStreamHeader, BulkRecvSink, bulkSender*,
                              * bulkReceiverInit, bulkReceiverFeed, BULK_KIND_*,
                              * BULK_STREAM_HEADER_FIXED */
#include "../../common/wb_log.h" /* WB_LOG_ERROR / WARN / INFO, WB_LOG_CAT_NET */

/* Clear one slot's map-download re-ask limit. Same reason as the round-log
 * reset above: the interval belongs to the connection, not to the slot. */
void udpServerResetMapReaskLimit(int idx) {
    udpServer.mapReaskSeen[idx]      = false;
    udpServer.mapReaskLastTick[idx]  = 0;
    udpServer.mapReaskThrottled[idx] = 0;
}

bool lobbyAnyOtherUploadActive(const bool *active, int exceptIdx) {
    int i;
    if (active == NULL) return false;
    for (i = 0; i < MAX_TANKS; i++) {
        if (i == exceptIdx) continue;
        if (active[i]) return true;
    }
    return false;
}

/* Free a client's per-slot upload state. Called from
 * serverDisconnectClient so a client that drops mid-upload doesn't
 * leave clientUploadActive set, which would falsely flag the
 * upload slot as busy and block every subsequent uploader. */
void udpServerClearClientUploadState(int idx) {
    if (idx < 0 || idx >= MAX_TANKS) return;
    udpServer.clientUploadActive[idx]   = false;
    udpServer.clientUploadTotal[idx]    = 0;
    udpServer.clientUploadName[idx][0]  = '\0';
    udpServer.clientReqCooldownTicks[idx] = 0;
    bulkReceiverInit(&udpServer.bulkRecvUp[idx]);
}

/* Authority check used by every lobby command handler.
 * Returns TRUE if the sender at clientIdx is allowed to issue the
 * command (host, OR open-host is on and they're an active player,
 * OR they're an admin). */
bool lobbyClientMayEdit(ServerSim *sim, int clientIdx) {
    if (clientIdx < 0 || clientIdx >= MAX_TANKS) return FALSE;
    if (clientIdx == serverSimGetHostSlot(sim)) return TRUE;  /* the host slot */
    if (serverSimIsPlayerConnected(sim, clientIdx) &&
        (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, (BYTE)clientIdx)
         & PLAYER_FLAG_ADMIN)) {
        return TRUE;
    }
    return serverSimGetOpenHost(sim) && serverSimIsPlayerConnected(sim, clientIdx);
}

/* Send map chunks to a client that is downloading */
/* Begin the armed map transfer once the bulk channel is quiescent. The
 * BulkSender's busy flag clears at staging-complete, not drain-complete, so a
 * correct endSeq needs the stream truly idle: no pending staging bytes and the
 * send window fully acked. startSeq is captured before any byte is staged, so
 * endSeq = startSeq + segment count is exact (channelStreamRefill only ever
 * forms a short final segment for a contiguous blob). A join download is
 * additionally gated on the client's PACKET_MAP_DL_READY (readySeen below);
 * a resync targets an already-established slot and its request is the
 * readiness signal. */
static void serverBeginMapTransferIfReady(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    ChannelMux *m = &udpServer.channelMux[slot];
    ChannelState *bulk = &m->ch[CHANNEL_BULK];
    BulkStreamHeader sh;
    uint32_t headerLen, totalBytes, segs;

    if (dl->xferKind == MAP_XFER_NONE || dl->xferBegun) return;
    /* A join download waits for the client's PACKET_MAP_DL_READY — proof the
     * accept landed and the receive buffers exist — so the stream head can
     * never arrive at a client that has nowhere to put it. A resync needs no
     * such gate: its own request is the readiness signal. */
    if (dl->xferKind == MAP_XFER_DOWNLOAD && !dl->readySeen) return;
    if (bulkSenderBusy(&udpServer.bulkSend[slot])) return;  /* preview draining */
    if (m->streamCount != 0) return;                        /* staging not empty  */
    if (bulk->ackedSeq != bulk->nextSeq) return;            /* window not drained */
    if (dl->compressedMap == NULL || dl->mapSize == 0) return;

    memset(&sh, 0, sizeof(sh));
    sh.kind = (dl->xferKind == MAP_XFER_RESYNC) ? BULK_KIND_RESYNC
                                                : BULK_KIND_DOWNLOAD;
    sh.gen = dl->resyncGen;          /* 0 for a join download */
    sh.totalSize = dl->mapSize;
    sh.pathLen = 0;
    sh.path[0] = '\0';

    dl->xferStartSeq = bulk->nextSeq;
    if (!bulkSenderBegin(&udpServer.bulkSend[slot], &sh,
                         dl->compressedMap, dl->mapSize)) {
        return;   /* allocation failure — retry next tick */
    }
    headerLen = (uint32_t)BULK_STREAM_HEADER_FIXED + sh.pathLen;
    totalBytes = headerLen + dl->mapSize;
    segs = (totalBytes + CHANNEL_BULK_SEG - 1) / CHANNEL_BULK_SEG;
    dl->xferEndSeq = dl->xferStartSeq + segs;
    dl->xferBegun = true;
}

/* Read transfer completion from the bulk channel. Once the peer has acked every
 * segment of the armed transfer (ackedSeq >= xferEndSeq) the same gates the old
 * chunk-ack path drove re-fire: a join download flips downloadComplete (snapshot
 * send + map-event flush lift); a resync clears resyncInProgress/resyncGen (the
 * held map events flush and the client's installedMapGen gate takes over).
 *
 * This is also where the client is told which items are on the map. The blob
 * carries a pillbox, base or start but not whether it has since been taken off
 * the map, so installing one puts every item it carries back on the map — and
 * a mask that arrived before the install would be wiped by it. The bulk ack
 * read here is the proof the install has happened: the peer acks its in-order
 * receive cursor, which advances only as it drains each segment, and draining
 * the last one is what runs the install. So a mask sent from here is behind
 * the install on the client, whatever the two channels do with it afterwards. */
static void serverCompleteMapTransferIfAcked(ServerSim *sim, int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    ChannelState *bulk = &udpServer.channelMux[slot].ch[CHANNEL_BULK];

    if (dl->xferKind == MAP_XFER_NONE || !dl->xferBegun) return;
    if (bulk->ackedSeq < dl->xferEndSeq) return;

    if (dl->xferKind == MAP_XFER_DOWNLOAD) {
        dl->downloadComplete = TRUE;
        fprintf(stderr, "[UDP SERVER] Client %d map download complete\n", slot);
    } else { /* MAP_XFER_RESYNC */
        dl->resyncInProgress = FALSE;
        dl->resyncGen = 0;
    }
    dl->xferKind = MAP_XFER_NONE;
    dl->xferBegun = false;

    /* Unicast, not published: only this slot has just installed a blob, and
     * the fill says nothing while every item is on the map. */
    {
        ControlEvent evt;
        if (serverSimFillEntitySyncEvent(sim, &evt)) {
            udpClientDeliverControl(&udpServer.clients[slot], &evt);
        }
    }
}

/* Per-tick service: begin an armed transfer when the channel is idle, then
 * fire completion when the peer has acked it through. Safe to call every tick
 * for any connected slot; a no-op when nothing is armed. */
void serverServiceMapTransfer(ServerSim *sim, int slot) {
    serverBeginMapTransferIfReady(slot);
    serverCompleteMapTransferIfAcked(sim, slot);
}

/* Initialize map download tracking for a client and arm a join download on the
 * bulk channel. The blob begins streaming once the channel is idle and the
 * per-tick carrier (transportUdpServerSend) feeds it. */
void serverInitMapDownload(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];

    if (dl->compressedMap != NULL) {
        free(dl->compressedMap);
    }
    dl->compressedMap = (BYTE *)malloc(udpServer.compressedMapSize);
    memcpy(dl->compressedMap, udpServer.compressedMap, udpServer.compressedMapSize);
    dl->mapSize = udpServer.compressedMapSize;
    dl->downloadComplete = FALSE;
    dl->resyncInProgress = FALSE;
    dl->resyncGen = 0;
    dl->xferKind = MAP_XFER_DOWNLOAD;
    dl->xferBegun = false;
    dl->xferStartSeq = 0;
    dl->xferEndSeq = 0;
    dl->readySeen = false;
}

/* Drop any in-flight map transfer for `slot` and re-base CHANNEL_BULK so the
 * next stream starts clean on both ends: bulkSenderReset drops the old staged
 * blob, channelResetSend(CHANNEL_BULK) collapses the send window and clears
 * the staging tail, and a CTRL_CHANNEL_RESET carries the new bulk baseline so
 * the client lifts its receive baseline and abandons any old partial. Without
 * this the old transfer's stragglers would segmentize into the new stream and
 * the client's single BulkReceiver would misparse it. Then arm a fresh join
 * download from the current blob (re-gates snapshots). The download begins
 * once the client's PACKET_MAP_DL_READY arrives (serverBeginMapTransferIfReady). */
void serverRebaseBulkAndRearmDownload(int i) {
    bulkSenderReset(&udpServer.bulkSend[i]);
    {
        uint32_t b3 = channelResetSend(&udpServer.channelMux[i], CHANNEL_BULK);
        ControlEvent resetEvt;
        ControlEncodeBodyFn enc =
            transportControlCodecBodyEncoder(CTRL_CHANNEL_RESET);
        uint8_t msg[CHANNEL_CONTROL_SEG];
        size_t bodyLen = 0;
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
    serverInitMapDownload(i);
}

/* Clean up map download tracking for a client */
void serverCleanupMapDownload(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    if (dl->compressedMap != NULL) {
        free(dl->compressedMap);
        dl->compressedMap = NULL;
    }
    dl->downloadComplete = FALSE;
    dl->xferKind = MAP_XFER_NONE;
    dl->xferBegun = false;
    dl->readySeen = false;
}

/* Reassembled-upload completion: hand the bytes to the sim (in-memory reload,
 * plus a persist stage under PERSIST policy), clear the per-client upload slot,
 * and reply MAP_UPLOAD_DONE. The bytes already sit in clientUploadBuf because
 * the bulk receiver's onBegin pointed it there. */
static void serverFinishUpload(ServerSim *sim, int clientIdx) {
    uint32_t total = udpServer.clientUploadTotal[clientIdx];
    const char *origName = udpServer.clientUploadName[clientIdx];

    char displayName[MAP_STR_SIZE];
    SDL_strlcpy(displayName, origName, sizeof(displayName));
    {
        size_t dlen = SDL_strlen(displayName);
        if (dlen >= 4 &&
            SDL_strcasecmp(displayName + dlen - 4, ".map") == 0) {
            displayName[dlen - 4] = '\0';
        }
    }

    bool previewed = serverSimReloadCompressedInMemory(
        sim, udpServer.clientUploadBuf[clientIdx], (int)total, displayName);

    /* PERSIST: write the accepted bytes to disk. The configured persist
     * directory is the concrete home of the virtual "Uploads/" folder; when
     * unset it falls back to "<mapDirRoot>/Uploads" (WinBoloDS back-compat).
     * The per-map file/storage caps were already enforced at MAP_UPLOAD_BEGIN.
     * An I/O failure is logged and swallowed — the in-memory preview stands. */
    if (previewed && udpServer.uploadPolicy == UPLOAD_POLICY_PERSIST) {
        char persistDir[FILENAME_MAX];
        if (udpServer.uploadPersistDir[0] != '\0') {
            SDL_strlcpy(persistDir, udpServer.uploadPersistDir,
                        sizeof(persistDir));
        } else {
            SDL_snprintf(persistDir, sizeof(persistDir), "%s/Uploads",
                         serverSimGetMapDirRoot(sim));
        }
        /* SDL_CreateDirectory creates missing parents; a no-op if it exists. */
        if (!SDL_CreateDirectory(persistDir)) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                "persist upload: cannot create directory '%s': %s",
                persistDir, SDL_GetError());
        } else {
            char persistPath[FILENAME_MAX];
            SDL_snprintf(persistPath, sizeof(persistPath), "%s/%s.map",
                         persistDir, displayName);
            FILE *pf = fopen(persistPath, "wb");
            if (pf == NULL) {
                WB_LOG_WARN(WB_LOG_CAT_NET,
                    "persist upload: cannot open '%s' for write", persistPath);
            } else {
                size_t wrote = fwrite(udpServer.clientUploadBuf[clientIdx],
                                      1, total, pf);
                fclose(pf);
                if (wrote != total) {
                    WB_LOG_WARN(WB_LOG_CAT_NET,
                        "persist upload: short write (%zu/%u) to '%s'",
                        wrote, total, persistPath);
                } else {
                    WB_LOG_INFO(WB_LOG_CAT_NET,
                        "persist upload: wrote '%s' (%u bytes)",
                        persistPath, total);
                }
            }
        }
    }

    udpServer.clientUploadActive[clientIdx] = false;
    udpServer.clientUploadTotal[clientIdx]  = 0;

    char relReturn[256];
    SDL_snprintf(relReturn, sizeof(relReturn), "Uploads/%s", origName);
    int relLen = (int)SDL_strlen(relReturn);
    if (relLen > 255) relLen = 255;
    uint8_t done[PACKET_HEADER_SIZE + 2 + 256];
    int dpos = PACKET_HEADER_SIZE;
    packHeader(done, PACKET_LOBBY_MAP_UPLOAD_DONE, 0);
    done[dpos++] = previewed ? 0 : LOBBY_REJECT_INVALID;
    done[dpos++] = (uint8_t)relLen;
    memcpy(done + dpos, relReturn, relLen);
    dpos += relLen;
    srvSendTo(done, dpos, &udpServer.clients[clientIdx].addr);
}

/* Bulk-receiver sink for a client->server map upload on CHANNEL_BULK. onBegin
 * validates the announced size against the approved BEGIN and the hard cap,
 * then points the receiver at the per-client upload buffer; onComplete runs
 * the reload/persist + DONE reply. */
typedef struct {
    ServerSim *sim;
    int        clientIdx;
} ServerUploadSinkCtx;

static uint8_t *serverBulkUploadOnBegin(void *vctx, const BulkStreamHeader *h) {
    ServerUploadSinkCtx *ctx = (ServerUploadSinkCtx *)vctx;
    int idx = ctx->clientIdx;
    if (h->kind != BULK_KIND_UPLOAD) return NULL;
    if (!udpServer.clientUploadActive[idx]) return NULL;        /* no approved BEGIN */
    if (h->totalSize != udpServer.clientUploadTotal[idx]) return NULL; /* size mismatch */
    if (h->totalSize == 0 || h->totalSize > LOBBY_MAP_UPLOAD_MAX_BYTES) return NULL;
    return udpServer.clientUploadBuf[idx];
}

static void serverBulkUploadOnComplete(void *vctx, const BulkStreamHeader *h,
                                       uint8_t *buf) {
    ServerUploadSinkCtx *ctx = (ServerUploadSinkCtx *)vctx;
    (void)h;
    (void)buf;
    serverFinishUpload(ctx->sim, ctx->clientIdx);
}

/* Drain every stream fragment waiting on this client's CHANNEL_BULK through the
 * upload receiver. Called wherever the client's channel frames are ingested. */
void serverDrainBulk(ServerSim *sim, int clientIdx) {
    ServerUploadSinkCtx ctx;
    BulkRecvSink sink;
    uint8_t chanBuf[CHANNEL_MAX_SEG];
    uint16_t chanLen;
    ctx.sim = sim;
    ctx.clientIdx = clientIdx;
    sink.onBegin = serverBulkUploadOnBegin;
    sink.onComplete = serverBulkUploadOnComplete;
    sink.ctx = &ctx;
    while (channelReceive(&udpServer.channelMux[clientIdx], CHANNEL_BULK,
                          chanBuf, &chanLen)) {
        bulkReceiverFeed(&udpServer.bulkRecvUp[clientIdx], chanBuf, chanLen,
                         &sink);
    }
}

/* Validate an upload filename payload. The wire delivers a length-prefixed
 * name that may not be NUL-terminated, so iterate by index over nameLen.
 * Declared in transport_udp.h so the unit tests can exercise the matrix
 * directly; production callers stay inside this translation unit. */
bool uploadFilenameIsSafe(const char *name, size_t nameLen) {
    static const char *kReservedBasenames[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9",
    };

    if (!name || nameLen == 0) return false;
    /* At least one basename byte plus the 4-byte ".map" suffix. */
    if (nameLen < 5) return false;
    /* Basename must fit the display-name slot (MAP_STR_SIZE - 1). */
    if (nameLen > (size_t)(MAP_STR_SIZE - 1) + 4) return false;
    if (name[0] == '.') return false;
    for (size_t i = 0; i < nameLen; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == '/' || ch == '\\' || ch == ':') return false;
        if (ch == '\0') return false;
        if (ch < 0x20) return false;
    }
    if (SDL_strncasecmp(name + nameLen - 4, ".map", 4) != 0) return false;
    /* Trailing dot or space on the basename — Windows strips these on
     * file creation, which would bypass collision avoidance. */
    char preDot = name[nameLen - 5];
    if (preDot == '.' || preDot == ' ') return false;
    size_t baseLen = nameLen - 4;
    for (size_t i = 0;
         i < sizeof(kReservedBasenames) / sizeof(kReservedBasenames[0]);
         i++) {
        const char *r = kReservedBasenames[i];
        size_t rlen = SDL_strlen(r);
        if (baseLen == rlen && SDL_strncasecmp(name, r, rlen) == 0) {
            return false;
        }
    }
    return true;
}
