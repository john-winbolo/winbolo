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
 *    - Under -DWB_FUZZ only, the dispatcher seam that
 *      feeds serverProcessPacket fuzzer bytes.
 *  Called by the tests under tests/unit/ and the fuzz
 *  harness under tests/fuzz/, never by shipping code.
 *********************************************************/

#include <stdio.h>  /* fprintf, stderr */
#include <stdlib.h> /* malloc, free, abort */
#include <string.h>

#include "transport_udp_internal.h"        /* eventQueueHasSpace, unpackU16, packGameEvent */
#include "transport_udp_server_internal.h" /* udpServer and the per-slot types held in it */
#include "channel_mux.h"                   /* channelSend, channelSendBestEffort,
                                              channelMuxInit */
#include "control_event.h"                 /* ControlEvent, CTRL_CHANNEL_RESET */
#include "transport_control_codec.h"       /* transportControlCodecBodyDecoder */
#include "bolo_map.h"                      /* mapSetPos */
#include "server_sim_internal.h"           /* serverSimShadowApply */
#include "../threads.h"                    /* threadsCreate, threadsWaitForMutex,
                                              threadsReleaseMutex */

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

uint32_t transportUdpServerTestLastReceivedTick(int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return 0;
    if (!udpServer.clients[slot].connected) return 0;
    return udpServer.clients[slot].lastReceivedTick;
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

uint32_t transportUdpServerTestMapQueueOutstanding(int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return 0;
    /* What the snapshot drain still owes the channel: the cumulative ack only
     * advances on a successful channelSend, so this is what a full window is
     * holding back. Zero means the queue has been handed over in full. */
    return udpServer.mapEventQueues[slot].nextSeq -
           udpServer.mapEventQueues[slot].ackedSeq;
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

/* Test-only: queue one whole game event on a slot's best-effort effect channel
 * (CHANNEL_GAME_EFFECT), exactly as the real producer does for an ephemeral
 * event in transportUdpServerDrainEvents — pack the GameEvent and
 * channelSendBestEffort it. The sibling above stages reliable traffic; this
 * stages the sounds and explosions a busy tick raises, which is what the
 * per-tick frame budget used to throw away.
 *
 * Returns what the enqueue reported: false for a bad slot, a NULL or
 * unpackable event, or a segment too large for the channel. A full ring is not
 * a failure here — channelSendBestEffort drops the oldest pending segment to
 * make room and still returns true — so a caller measuring loss reads
 * channelGetBestEffortStats' ring-drop count rather than this return. */
bool transportUdpServerTestAddEffectEvent(int slot, const GameEvent *ev) {
    uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
    int evLen;
    if (slot < 0 || slot >= MAX_TANKS || ev == NULL) return false;
    evLen = packGameEvent(evBuf, ev);
    if (evLen <= 0) return false;
    return channelSendBestEffort(&udpServer.channelMux[slot],
                                 CHANNEL_GAME_EFFECT, evBuf, (uint16_t)evLen);
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

#ifdef WB_FUZZ
/* ================================================================
 * Fuzz-only dispatcher seam (hardening plan §1.2, tier 2)
 *
 * Feeds serverProcessPacket attacker-controlled bytes directly —
 * no socket, no recv thread. Compiled only under -DWB_FUZZ (the dedicated
 * fuzz build); every shipping build leaves these symbols out entirely.
 *
 * The harness owns the ServerSim lifetime and calls Init once before
 * feeding packets. State accumulates across inputs by design — that is the
 * standard libFuzzer persistent-target pattern and explores deeper handler
 * paths than a per-input reset would.
 * ================================================================ */

/* The fixed peer the dispatcher seam attributes every fuzz datagram to. The
 * warm-up JOIN below and transportUdpServerFuzzProcessPacket share this exact
 * (addr,port) so serverFindClient resolves a fuzz datagram to the pre-connected
 * slot — change one without the other and the post-JOIN handlers go dark. */
static void fuzzServerPeerAddr(struct sockaddr_in *from) {
    memset(from, 0, sizeof(*from));
    from->sin_family = AF_INET;
    from->sin_addr.s_addr = htonl(0x7f000001u); /* 127.0.0.1 */
    from->sin_port = htons((unsigned short)40000);
}

/* Drive one real JOIN to completion so the dispatcher starts with a connected
 * client. Without it, serverFindClient() returns -1 for the fuzz peer and every
 * post-JOIN handler (COMMAND_TICK, INPUT, CONTROL_ACK, the reliable event
 * loops, map reassembly) bails at its `clientIdx < 0` guard — i.e. the hand-
 * written count-loops this target exists to reach stay unfuzzed.
 *
 * The join is cookie-gated: normally the joiner echoes a cookie from a prior
 * PACKET_JOIN_CHALLENGE, but that challenge reply is a no-op over the seam's
 * INVALID_SOCKET, so a two-pass handshake can't observe it. Instead we mint the
 * cookie directly (same secret/window the acceptor checks) and submit a single
 * well-formed JOIN_REQUEST through the very dispatch path the fuzzer drives.
 *
 * The body layout mirrors serverHandleJoinRequest's reader exactly. One subtle
 * ordering contract: the optional 2-byte fallbackCountry is read *before* the
 * trailing cookie, so it must be present here — omit it and the handler eats
 * the cookie's first two bytes as a country code and the address proof fails. */
static bool fuzzServerWarmJoin(ServerSim *sim) {
    uint8_t pkt[PACKET_HEADER_SIZE + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3 +
                WBN_JOIN_KEY_WIRE_LEN + 1 /*flags*/ + 2 /*type,hints*/ +
                2 /*country*/ + JOIN_COOKIE_LEN];
    struct sockaddr_in from;
    size_t pos;

    fuzzServerPeerAddr(&from);
    memset(pkt, 0, sizeof(pkt));
    packHeader(pkt, PACKET_JOIN_REQUEST, 0);
    pos = PACKET_HEADER_SIZE;

    /* Player name (NUL-padded, validator-clean ASCII). */
    memcpy(pkt + pos, "FuzzPeer", 8);
    pos += PACKET_MAX_PLAYER_NAME;

    /* Password: empty — the fuzz ServerSim is created without one. */
    pos += MAP_STR_SIZE;

    /* Protocol version triple: the CMake-defined values this server gates on,
     * so it always matches and can't take the version-reject branch. */
    pkt[pos++] = (uint8_t)BOLO_VERSION_MAJOR;
    pkt[pos++] = (uint8_t)BOLO_VERSION_MINOR;
    pkt[pos++] = (uint8_t)BOLO_VERSION_REVISION;

    /* WBN join key empty → joins as a non-WBN player (skips token verify). */
    pos += WBN_JOIN_KEY_WIRE_LEN;

    pos += 1; /* flags: 0 (no rejoin, won't-authenticate) */
    pos += 2; /* clientType, clientHints: 0 (unknown client) */

    /* fallbackCountry — present so the cookie that follows stays aligned. */
    pkt[pos++] = 'X';
    pkt[pos++] = 'X';

    /* Address-proof cookie for the current window. Fails closed if the CSPRNG
     * secret is unavailable; we then skip the warm-up and the target degrades
     * to its pre-warm (JOIN-gated) behaviour rather than connecting. */
    if (!serverCookieCompute(&from, serverCookieCurrentWindow(), pkt + pos)) {
        return false;
    }
    pos += JOIN_COOKIE_LEN;

    /* serverProcessPacket's sim-mutating handlers assert the server mutex is
     * held; take it here exactly as serverInstanceTick does in production. */
    threadsWaitForMutex();
    serverProcessPacket(sim, pkt, (int)pos, &from);
    threadsReleaseMutex();

    /* Caller decides how to treat a join that didn't connect: init aborts (a
     * dead gate from the first input is a build-level regression), the per-
     * input re-arm shrugs and lets the input bounce off the gate. */
    return serverFindClient(&from) >= 0;
}

/* Minimal server context: mirrors the non-socket, non-thread portion of
 * transportUdpServerCreate. sock stays INVALID_SOCKET so srvSendTo's
 * underlying udpSendTo is a no-op and no datagrams leave the process. After
 * the context is up we drive one JOIN so the dispatcher begins with a
 * connected client (see fuzzServerWarmJoin). */
void transportUdpServerFuzzInit(ServerSim *sim) {
    int i;
    /* The seam drives serverProcessPacket directly (no recv thread), but its
     * sim-mutating handlers assert the server mutex is held. Create the mutex
     * here so the seam can take it around each serverProcessPacket call, the
     * way serverInstanceTick holds it in production. Idempotent. */
    threadsCreate(true);
    memset(&udpServer, 0, sizeof(udpServer));
    memset(punchQueue, 0, sizeof(punchQueue));
    udpServer.sock = INVALID_SOCKET;
    udpServer.running = true;
    udpServer.tickCount = 0;
    udpServer.uploadMaxFiles        = 64;
    udpServer.uploadMaxStorageBytes = 8u * 1024u * 1024u;
    udpServer.compressedMapSize = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        udpServer.clients[i].connected = false;
        udpServer.clients[i].controlSub = SUBSCRIBER_HANDLE_INVALID;
        memset(&udpServer.mapDownload[i], 0, sizeof(ClientMapDownload));
        udpServer.controlSyncInProgress[i] = false;
        serverSimSetShadowCulled(sim, (BYTE)i, false);
    }
    netImpairInit(&srvImpairIn);
    netImpairInit(&srvImpairOut);

    /* Open the JOIN gate: leave one client connected at the fuzz peer address
     * so the post-JOIN dispatcher paths are reachable from the first input. A
     * join that doesn't connect here means the target is neutered before a
     * single input runs — a build-level regression, so fail loudly. The
     * -runs=0 corpus-replay ctest exercises this path on every build, turning
     * a silent dead gate into a red test. */
    if (!fuzzServerWarmJoin(sim)) {
        fprintf(stderr, "fuzzServerWarmJoin: peer not connected after JOIN — "
                        "server_dispatch would fuzz the dead JOIN gate\n");
        abort();
    }
}

/* One datagram, as if received on the game socket from a LAN peer. The buffer
 * is heap-allocated to the EXACT input length (handlers need it mutable, and
 * an exact size lets ASan's redzone catch any read/write past len — an
 * oversized buffer would mask the very over-reads this target hunts). */
void transportUdpServerFuzzProcessPacket(ServerSim *sim,
                                         const uint8_t *data, size_t size) {
    uint8_t *buf;
    struct sockaddr_in from;
    if (size == 0 || size > 65535) return;
    fuzzServerPeerAddr(&from); /* same peer the warm-up JOIN registered */

    /* Keep the post-JOIN surface live across inputs. State persists by design,
     * so an earlier input that disconnected the peer (a QUIT, an idle-timeout
     * path, a handler that drops the client) would otherwise leave every later
     * input bouncing off the JOIN gate. Re-arm if needed — the input that did
     * the disconnect already exercised that path; this just restores the
     * connected-client context the next input wants to fuzz. */
    if (serverFindClient(&from) < 0) {
        (void)fuzzServerWarmJoin(sim);
    }

    buf = (uint8_t *)malloc(size);
    if (buf == NULL) return;
    memcpy(buf, data, size);
    /* Hold the server mutex across dispatch, mirroring serverInstanceTick — the
     * sim-mutating handlers (COMMAND_TICK → serverSimApplyCommand, etc.) assert
     * it. The re-arm above self-locks, so this is a fresh, non-nested region. */
    threadsWaitForMutex();
    serverProcessPacket(sim, buf, (int)size, &from);
    /* Mirror serverInstanceTick: drain deferred overflow-disconnects after
     * recv, outside any publish, under the same mutex. Keeps the fuzz server's
     * state consistent and exercises the deferred-disconnect path
     * (serverDisconnectClient + leave broadcast + serverSimRemovePlayer) that
     * the control-queue-overflow handler now defers here. */
    transportUdpServerDrainPendingRemovals(sim);
    threadsReleaseMutex();
    free(buf);
}
#endif /* WB_FUZZ */
