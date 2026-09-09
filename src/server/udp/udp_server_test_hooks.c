/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Test Hooks
 *Filename:      udp_server_test_hooks.c
 *Author:        John Morrison
 *Purpose:
 *  The test-only observability surface for the UDP server
 *  transport, split out of transport_udp_server.c.
 *    - Reads back per-slot channel, map-event, download
 *      and spectator state.
 *    - Stages events and connections a test needs without
 *      standing up sockets.
 *  Driven by the tests under tests/unit/, not by shipping
 *  code.
 *********************************************************/

#include <string.h>

#include "transport_udp_internal.h"        /* eventQueueHasSpace, unpackU16, packGameEvent */
#include "transport_udp_server_internal.h" /* udpServer and the per-slot types held in it */
#include "channel_mux.h"                   /* channelSend, channelMuxInit */
#include "control_event.h"                 /* ControlEvent, CTRL_CHANNEL_RESET */
#include "transport_control_codec.h"       /* transportControlCodecBodyDecoder */
#include "bolo_map.h"                      /* mapSetPos */
#include "server_sim_internal.h"           /* serverSimShadowApply */

void transportUdpServerSetClientPingForTest(BYTE playerNum, uint16_t pingMs) {
    if (playerNum >= MAX_TANKS) return;
    if (!udpServer.clients[playerNum].connected) return;
    udpServer.clients[playerNum].pingMs = pingMs;
}

void transportUdpServerGetPingStrikesForTest(BYTE playerNum,
                                             uint8_t *outKickStrikes,
                                             uint8_t *outWarnStrikes,
                                             bool *outWarned,
                                             uint16_t *outLastEnforcedMs) {
    if (playerNum >= MAX_TANKS) return;
    if (outKickStrikes)    *outKickStrikes    = udpServer.clients[playerNum].pingKickStrikes;
    if (outWarnStrikes)    *outWarnStrikes    = udpServer.clients[playerNum].pingWarnStrikes;
    if (outWarned)         *outWarned         = udpServer.clients[playerNum].pingWarned;
    if (outLastEnforcedMs) *outLastEnforcedMs = udpServer.clients[playerNum].lastEnforcedPingMs;
}

/* ── Test-only channel-mux scaffolding ───────────────────────────────────
 * Honest test access to the otherwise-silent parallel channel layer: queue a
 * message on a slot's channel, and read back its send-side ack / receive-side
 * sequence state and the count of frames consumed.  Not used by shipping
 * code — only the loopback channel integration test drives these. */
bool transportUdpServerChannelTestSend(int slot, uint8_t ch,
                                       const uint8_t *msg, uint16_t len) {
    if (slot < 0 || slot >= MAX_TANKS) return false;
    return channelSend(&udpServer.channelMux[slot], ch, msg, len);
}

void transportUdpServerChannelTestStats(int slot, uint8_t ch,
                                        uint32_t *expectedSeq,
                                        uint32_t *ackedSeq,
                                        uint32_t *framesRx) {
    if (slot < 0 || slot >= MAX_TANKS || ch >= CHANNEL_COUNT) return;
    if (expectedSeq) *expectedSeq = udpServer.channelMux[slot].ch[ch].expectedSeq;
    if (ackedSeq)    *ackedSeq    = udpServer.channelMux[slot].ch[ch].ackedSeq;
    if (framesRx)    *framesRx    = udpServer.channelFramesRx[slot];
}

bool transportUdpServerTestPendingRemove(int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return false;
    return udpServer.pendingSimRemove[slot];
}

const uint8_t *transportUdpServerGetSpectatorSeed(int s, uint32_t *outLen,
                                                  uint8_t *outKind) {
    SpectatorConn *sp;
    if (s < 0 || s >= MAX_SPECTATORS) return NULL;
    sp = &udpServer.spectators[s];
    if (!sp->connected) return NULL;
    if (outLen)  *outLen  = sp->seedLen;
    if (outKind) *outKind = sp->bulkSend.kind;
    return sp->seedBlob;
}

bool transportUdpServerGetSpectatorFeedSeq(int s, uint32_t *outSeq,
                                           uint8_t *outKind) {
    SpectatorConn *sp;
    if (s < 0 || s >= MAX_SPECTATORS) return false;
    sp = &udpServer.spectators[s];
    if (!sp->connected) return false;
    if (outSeq)  *outSeq  = sp->lastEmittedSeq;
    if (outKind) *outKind = sp->bulkSend.kind;
    return true;
}

bool transportUdpServerGetSpectatorCountdown(int s, uint32_t *outRemaining) {
    SpectatorConn *sp;
    if (s < 0 || s >= MAX_SPECTATORS) return false;
    sp = &udpServer.spectators[s];
    if (!sp->connected) return false;
    if (outRemaining) *outRemaining = sp->countdownRemaining;
    return sp->inCountdown;
}

void transportUdpServerTestSpectatorAckBulk(int s) {
    ChannelState *bulk;
    if (s < 0 || s >= MAX_SPECTATORS) return;
    if (!udpServer.spectators[s].connected) return;
    /* Simulate a peer that keeps up: mark the whole CHANNEL_BULK send window
     * acked, which completes the seed and frees the window so the forward feed
     * keeps draining — without a real spectator channel endpoint (3b). */
    bulk = &udpServer.spectators[s].channelMux.ch[CHANNEL_BULK];
    bulk->ackedSeq = bulk->nextSeq;
}

uint32_t transportUdpServerGetSpectatorControlSeq(int s) {
    if (s < 0 || s >= MAX_SPECTATORS) return 0;
    return udpServer.spectators[s].channelMux.ch[CHANNEL_CONTROL].nextSeq;
}

uint32_t transportUdpServerGetClientControlSeq(int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return 0;
    return udpServer.channelMux[slot].ch[CHANNEL_CONTROL].nextSeq;
}

bool transportUdpServerGetSpectatorLive(int s) {
    if (s < 0 || s >= MAX_SPECTATORS) return false;
    return udpServer.spectators[s].live;
}

bool transportUdpServerTestDownloadComplete(int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return false;
    return udpServer.mapDownload[slot].downloadComplete;
}

uint32_t transportUdpServerGetMapEventDrops(int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return 0;
    return udpServer.mapEventQueueDrops[slot];
}

uint32_t transportUdpServerGetMapReaskThrottled(int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return 0;
    return udpServer.mapReaskThrottled[slot];
}

/* Test-only: stage one terrain change for a slot exactly as a real sim tick
 * does — mutate the live server map, write the tile into every slot's copy of
 * the terrain, AND enqueue an EVENT_MAP_CHANGE into the slot's map-event hold
 * queue (mirroring simMapChangeCallback → serverSimShadowTick →
 * transportUdpServerDrainEvents). Mutating both keeps the snapshot checksum
 * in step with the change the client applies, so the client doesn't see a
 * spurious terrain divergence and self-trigger a resync. The event then flows
 * through the real hold → channel drain → tagged channelSend(CHANNEL_MAP) path,
 * so the loopback test exercises the live wiring rather than poking the channel
 * directly. Call between ticks: the map-change callback is dormant then, so the
 * server-side mapSetPos won't double-enqueue. Returns false if the slot is
 * invalid or its hold queue is full. */
uint32_t transportUdpServerTestMapQueueCount(int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return 0;
    /* Seq 1 is the first event a slot is ever assigned (the join resets both
     * ends of the queue to 1), so nextSeq - 1 is how many it has been given. */
    return udpServer.mapEventQueues[slot].nextSeq - 1u;
}

bool transportUdpServerTestMapQueueHasSquare(int slot, uint8_t x, uint8_t y) {
    const ClientEventQueue *mq;
    uint32_t i;
    if (slot < 0 || slot >= MAX_TANKS) return false;
    mq = &udpServer.mapEventQueues[slot];
    /* The whole ring, not just the unacked window: an entry stays put until
     * its slot is reused 2048 events later, so a queue that has carried fewer
     * than that still holds every event it was ever given. */
    for (i = 0; i < (uint32_t)RELIABLE_EVENT_BUFFER_SIZE; i++) {
        if (mq->buffer[i].seq == 0 || mq->buffer[i].seq >= mq->nextSeq) continue;
        if (mq->buffer[i].event.type != EVENT_MAP_CHANGE) continue;
        if (mq->buffer[i].event.data[0] == x && mq->buffer[i].event.data[1] == y) {
            return true;
        }
    }
    return false;
}

bool transportUdpServerTestAddMapEvent(ServerSim *sim, int slot, uint8_t x,
                                       uint8_t y, uint8_t terrain) {
    ClientEventQueue *mq;
    uint32_t idx;
    if (slot < 0 || slot >= MAX_TANKS) return false;
    mq = &udpServer.mapEventQueues[slot];
    if (!eventQueueHasSpace(mq)) return false;
    if (sim != NULL) {
        mapSetPos(&sim->sim, &sim->sim.mp, x, y, terrain, FALSE, TRUE);
        /* A real tick also writes the change into every slot's copy of the
         * terrain (serverSimShadowTick over the tick's map events). Nothing
         * runs that here — the map-change callback is dormant between ticks,
         * so no map event is recorded — so apply it directly, or the slot's
         * snapshot checksum would describe the pre-change map and the client
         * would see a spurious divergence. Every slot takes the write, not
         * just this one, so a caller staging changes for a single client (what
         * every test using this does) leaves no slot mid-way. */
        serverSimShadowApply(sim, x, y, terrain);
    }
    idx = mq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
    mq->buffer[idx].event.type = EVENT_MAP_CHANGE;
    memset(mq->buffer[idx].event.data, 0, sizeof(mq->buffer[idx].event.data));
    mq->buffer[idx].event.data[0] = x;
    mq->buffer[idx].event.data[1] = y;
    mq->buffer[idx].event.data[2] = terrain;
    mq->buffer[idx].seq = mq->nextSeq;
    mq->nextSeq++;
    return true;
}

/* Test-only: queue one whole game event on a slot's reliable game channel
 * (CHANNEL_GAME), exactly as the real producer does in
 * transportUdpServerSendGameEventsToChannel — pack the GameEvent and
 * channelSend it. Lets a test stage a distinguishable ch0 event (the
 * straggler-gate test leaves one unacked across game start). Returns false if
 * the slot is invalid, the event is NULL/unpackable, or the channel window is
 * full. */
bool transportUdpServerTestAddGameEvent(int slot, const GameEvent *ev) {
    uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
    int evLen;
    if (slot < 0 || slot >= MAX_TANKS || ev == NULL) return false;
    evLen = packGameEvent(evBuf, ev);
    if (evLen <= 0) return false;
    return channelSend(&udpServer.channelMux[slot], CHANNEL_GAME, evBuf,
                       (uint16_t)evLen);
}

/* Test-only: fabricate a connected slot with a fresh channel mux, so a server
 * unit test can drive transportUdpServerOnGameStart over two distinct slots
 * without standing up sockets. Mirrors the per-slot state the join path sets
 * that OnGameStart reads (connected, playerNum, completed download, an
 * initialised channel mux); the reliable queues stay zero-initialised, which
 * the game-start wipe and its assert accept. */
void transportUdpServerTestForceConnect(int slot, BYTE playerNum) {
    if (slot < 0 || slot >= MAX_TANKS) return;
    udpServer.clients[slot].connected = true;
    udpServer.clients[slot].playerNum = playerNum;
    udpServer.mapDownload[slot].downloadComplete = TRUE;
    channelMuxInit(&udpServer.channelMux[slot]);
}

/* Test-only: find the CTRL_CHANNEL_RESET this slot has queued on its reliable
 * control channel (CHANNEL_CONTROL) and decode its two baselines. Scans the
 * live send window for the message tagged CTRL_CHANNEL_RESET (the only place
 * OnGameStart writes one). Returns false if none is queued or it fails to
 * decode. */
bool transportUdpServerTestPeekChannelReset(int slot, uint32_t *ch0Baseline,
                                            uint32_t *ch1Baseline) {
    ChannelState *cs;
    uint32_t seq;
    if (slot < 0 || slot >= MAX_TANKS) return false;
    cs = &udpServer.channelMux[slot].ch[CHANNEL_CONTROL];
    for (seq = cs->nextSeq; seq > cs->ackedSeq; ) {
        uint32_t idx;
        const uint8_t *m;
        uint16_t mlen;
        uint16_t bodyLen;
        ControlEvent evt;
        ControlDecodeBodyFn dec;
        seq--;
        idx = seq % cs->window;
        m = cs->sendData + (size_t)idx * cs->segSize;
        mlen = cs->sendLen[idx];
        if (mlen < 3 || m[0] != (uint8_t)CTRL_CHANNEL_RESET) continue;
        bodyLen = unpackU16(m + 1);
        if ((size_t)(3 + bodyLen) > (size_t)mlen) continue;
        dec = transportControlCodecBodyDecoder(CTRL_CHANNEL_RESET);
        if (dec == NULL || !dec(m + 3, bodyLen, &evt)) return false;
        if (ch0Baseline) *ch0Baseline = evt.u.channelReset.ch0Baseline;
        if (ch1Baseline) *ch1Baseline = evt.u.channelReset.ch1Baseline;
        return true;
    }
    return false;
}
