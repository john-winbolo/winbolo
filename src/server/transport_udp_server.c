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
#include "bases.h"
#include "pillbox.h"
#include "starts.h"
#include "players.h"
#include "client_enums.h"  /* aiType, gameType, sndEffects, updateType */
#include "viewport_types.h"  /* screen */
#include "messages.h"
#include "util.h"
#include "game_sim.h"
#include "geolookup.h"
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
#include "wbn_key_codec.h"
#include "../winbolonet/winbolonet_core.h"
#include "../winbolonet/winbolonet_server.h"
#include "threads.h"
#include "sounddist.h"
#include "bot_manager.h"
#include "log.h"
#include "playername_validate.h"
#include "server_sim_join.h"
#include "server_sim_lifecycle.h"
#include "../common/wb_log.h"
#include "../common/mp_diag_log.h"
#include "net_impair.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

/* OS cryptographic RNG, used to seed the address-proof cookie secret. */
#if defined(_WIN32)
#  include <bcrypt.h>
#elif defined(__linux__)
#  include <sys/random.h>  /* getrandom */
#  include <fcntl.h>       /* open, O_RDONLY (/dev/urandom fallback) */
#  include <unistd.h>      /* read, close */
#else
#  include <stdlib.h>      /* arc4random_buf (macOS/BSD) */
#endif

/* ---- Server dedicated recv thread ----
 * A background thread continuously polls the server socket and queues
 * packets into an SPSC ring buffer.  The timer callback drains the
 * queue each tick, keeping packet processing on the main thread. */

#define RECV_QUEUE_SIZE 128

typedef struct {
    uint8_t data[UDP_MAX_PAYLOAD];
    int     len;
    struct sockaddr_in fromAddr;
} RecvQueueEntry;

static RecvQueueEntry recvQueue[RECV_QUEUE_SIZE];
static SDL_AtomicInt  recvQueueHead;  /* written by recv thread */
static SDL_AtomicInt  recvQueueTail;  /* written by timer thread */
static SDL_AtomicInt  recvThreadRunning;
static SDL_Thread    *recvThread = NULL;
static SOCKET         recvThreadSock = INVALID_SOCKET; /* copy for thread */
static uint32_t       recvDropCount = 0; /* packets dropped due to full queue */

static int SDLCALL serverRecvThreadFunc(void *userdata) {
    (void)userdata;
    SOCKET sock = recvThreadSock;

    while (SDL_GetAtomicInt(&recvThreadRunning)) {
        fd_set readfds;
        struct timeval tv;
        int selRet;

        if (sock == INVALID_SOCKET) break;

        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 1000; /* 1ms timeout */

        selRet = select((int)(sock + 1), &readfds, NULL, NULL, &tv);
        if (selRet <= 0) continue;

        /* Drain all available packets from the socket */
        while (SDL_GetAtomicInt(&recvThreadRunning)) {
            int head = SDL_GetAtomicInt(&recvQueueHead);
            int tail = SDL_GetAtomicInt(&recvQueueTail);
            int next = (head + 1) % RECV_QUEUE_SIZE;

            if (next == tail) {
                /* Queue full — drop packet by reading and discarding */
                uint8_t discard[UDP_MAX_PAYLOAD];
                struct sockaddr_in discardAddr;
                socklen_t addrLen = sizeof(discardAddr);
                int ret = recvfrom(sock, (char *)discard, sizeof(discard), 0,
                                   (struct sockaddr *)&discardAddr, &addrLen);
                if (ret <= 0) break;
                recvDropCount++;
                if ((recvDropCount & 255) == 1) {
                    fprintf(stderr, "[UDP SERVER] Recv queue full, dropped %u packets\n",
                            recvDropCount);
                }
                continue;
            }

            {
                RecvQueueEntry *entry = &recvQueue[head];
                socklen_t addrLen = sizeof(entry->fromAddr);
                int ret = recvfrom(sock, (char *)entry->data, UDP_MAX_PAYLOAD, 0,
                                   (struct sockaddr *)&entry->fromAddr, &addrLen);
                if (ret <= 0) break; /* No more data or error */
                entry->len = ret;
                SDL_SetAtomicInt(&recvQueueHead, next);
            }
        }
    }
    return 0;
}

/* ================================================================
 * SERVER SIDE
 * ================================================================ */

/* Map transfer flavor armed on a slot's bulk channel. */
typedef enum {
    MAP_XFER_NONE = 0,
    MAP_XFER_DOWNLOAD,   /* join / mid-game-join full map download */
    MAP_XFER_RESYNC      /* live map re-send for desync recovery   */
} MapTransferKind;

/* Per-client map download tracking. The compressed map streams to the client
 * on CHANNEL_BULK via the per-slot BulkSender (bulk_transfer.c); there is no
 * separate chunker, ack packet, or resend loop — the channel's own
 * reliability carries and retransmits the bytes. */
typedef struct {
    BYTE    *compressedMap;     /* Per-client copy of compressed map (owned, must free) */
    uint32_t mapSize;          /* Total compressed map size */
    bool     downloadComplete; /* True once the join download's bytes are acked
                                * (ackedSeq >= xferEndSeq) or force-set at game
                                * start. Gates snapshot send + map-event flush. */
    bool     resyncInProgress; /* True while serving a client-requested live map
                                * resync (map desync recovery). While set, the
                                * snapshot builder holds this slot's map events
                                * (packs zero) so the freshly compressed blob and
                                * the held terrain changes can't double-apply. */
    uint32_t resyncGen;        /* Generation id of the in-flight resync (0 = none),
                                * carried in the bulk stream header so the client
                                * drops a superseded request and the freshly
                                * installed blob wins the gen gate. */
    /* Bulk-stream transfer arming. The transfer is armed (xferKind set) at join
     * / resync, begun once the bulk channel is idle (serverBeginMapTransferIfReady),
     * and read complete from the channel's ackedSeq (serverCompleteMapTransferIfAcked). */
    MapTransferKind xferKind;  /* armed transfer flavor (NONE once finished)    */
    bool     xferBegun;        /* bulkSenderBegin has been issued               */
    uint32_t xferStartSeq;     /* CHANNEL_BULK nextSeq captured at begin         */
    uint32_t xferEndSeq;       /* startSeq + segment count; done when ackedSeq>= */
} ClientMapDownload;

#define UPLOAD_MAX_BYTES (64u * 1024u)
#define LOBBY_REQ_COOLDOWN_TICKS 25  /* ~0.5s at 50 Hz */

/* Standalone PACKET_CHANNEL frames a downloading client gets per tick while
 * snapshots are gated (no snapshot trailer to carry the bulk stream). One
 * frame carries ~5 segments under the datagram budget, so this clears a full
 * CHANNEL_BULK window (96 segments) in a tick rather than throttling the map to
 * ~one frame/tick; the unacked window then bounds bytes in flight. */
#define MAP_DOWNLOAD_FRAMES_PER_TICK 24

/* Anonymous-fallback ceiling for a deferred WBN PLAYER_JOIN: how long
 * we wait for the joiner's rekey->reauth round-trip (two network hops
 * plus two blocking winbolo.net HTTP calls) to fill the slot's key
 * before announcing the join un-keyed.  The keyed path fires the moment
 * the reauth verifies, so this only bounds the never-reauth case
 * (direct-IP / not signed in / WBN unreachable).  ~5 s @ 50 Hz. */
#define WBN_JOIN_REGISTER_GRACE_TICKS 250

/* ── Deferred WBN PLAYER_JOIN pure core (declared in transport_udp.h) ─
 * Value-only sequencing so the join/reauth/grace/disconnect logic is
 * unit-testable without sockets or the WBN HTTP layer. */
void wbnJoinArm(WbnJoinState *s, uint32_t nowTick, uint32_t graceTicks) {
    s->pending = true;
    s->deadlineTick = nowTick + graceTicks;
}

bool wbnJoinOnReauth(WbnJoinState *s, bool wasParticipant) {
    s->pending = false;
    return !wasParticipant;
}

bool wbnJoinOnTick(WbnJoinState *s, uint32_t nowTick) {
    /* Wrap-safe compare: nowTick - deadlineTick >= 0 once reached. */
    if (s->pending && (int32_t)(nowTick - s->deadlineTick) >= 0) {
        s->pending = false;
        return true;
    }
    return false;
}

void wbnJoinClear(WbnJoinState *s) {
    s->pending = false;
}

bool wbnRekeyTargetSelected(bool connected, bool wbnWasVerified) {
    return connected && wbnWasVerified;
}

JoinCollisionVerdict joinCollisionDecide(bool incomingWillAuth,
                                         bool existingIsVerified) {
    if (existingIsVerified) return JOIN_COLLISION_REJECT_VERIFIED;
    if (!incomingWillAuth)  return JOIN_COLLISION_REJECT_IN_USE;
    return JOIN_COLLISION_ADMIT_PROVISIONAL;
}

ClaimResolveAction claimResolveDecide(bool bareNameHeld, bool holderIsVerified) {
    if (!bareNameHeld)    return CLAIM_RESOLVE_PROMOTE_FREE;
    if (holderIsVerified) return CLAIM_RESOLVE_KEEP_TEMP;
    return CLAIM_RESOLVE_PREEMPT_SQUATTER;
}

/* Per-source-IP JOIN rate limit. Keys on the source IP alone (a spoofer can
 * walk source ports), tracking a small LRU of recent sources. Scope: this
 * contains port-walking from a *single* source IP; it does NOT rate-limit a
 * flood from random/spoofed source IPs, which gets a fresh bucket per packet
 * and keeps evicting the LRU. That's acceptable — the address-proof cookie
 * prevents slot exhaustion regardless of flood shape, and the challenge it
 * draws is smaller than the JOIN (de-amplifying). */
#define JOIN_RL_MAX_SOURCES 64    /* LRU of recent source IPs */
#define JOIN_RL_BURST       5     /* token-bucket capacity per source IP */
#define JOIN_RL_REFILL_MS   2000  /* +1 token every 2 s */

typedef struct {
    uint32_t srcAddr;  /* network-order sin_addr.s_addr; 0 = empty entry */
    uint32_t tokens;   /* tokens remaining */
    uint64_t lastMs;   /* SDL_GetTicks() at last touch */
} JoinRateEntry;

/* Tankless spectator connection — a viewer that holds no tank slot, is fed
 * only from the delayed ring (2d), and is never on the control-event bus.
 * Peer to UdpServerClient but carries only the resources a viewer uses: its
 * own ChannelMux (forward events + acks) and BulkSender (seed keyframe). No
 * map-event queue / upload receiver / subscriber handle. */
typedef struct {
    struct sockaddr_in addr;
    bool     connected;
    uint64_t connId;
    char     playerName[PACKET_MAX_PLAYER_NAME];
    uint32_t lastReceivedTick;
    uint32_t outSequence;
    uint32_t inboundCmdSeq;
    uint16_t pingMs;
    uint8_t  clientType;
    uint8_t  clientHints;
    ChannelMux channelMux;
    BulkSender bulkSend;
    /* Delayed-keyframe seed transfer (2d-c). seedBlob is a spectator-owned copy
     * of the ring keyframe at head - specDelayTicks (the ring's own pointer
     * invalidates on the next RecordTick, so the bytes are copied at seek time
     * and the bulk transfer drains the copy over multiple ticks). seedBlob ==
     * NULL && !seedComplete means "not yet seeded" — the seek is retried each
     * tick until the ring has enough history. xferStartSeq/xferEndSeq mirror
     * ClientMapDownload's bulk-ack bookkeeping. */
    uint8_t *seedBlob;
    uint32_t seedLen;
    uint32_t seedGen;        /* ring segment of the seeded keyframe (header gen) */
    bool     seedBegun;
    bool     seedComplete;
    uint32_t xferStartSeq;
    uint32_t xferEndSeq;
    /* Forward feed (2d-e). seedSeq is the recordSeq of the seeded keyframe,
     * captured at seek; once the seed is acked, lastEmittedSeq starts there and
     * walks forward, emitting each record in (lastEmittedSeq, head - delay] as a
     * BULK_KIND_SPEC_RECORD blob — so the view lags exactly specDelayTicks and
     * never reaches the live head. No persistent record buffer: bulkSenderBegin
     * copies each record, so it is built in a transient local and freed at once;
     * the bulkSenderBusy guard is the single-blob-in-flight control. */
    uint32_t seedSeq;
    uint32_t lastEmittedSeq;
    /* Cold-start countdown (2d-f). While the delayed ring holds less than
     * specDelayTicks of history the seek returns COLD_START: no seed is copied;
     * instead a throttled SPEC_CTRL_COUNTDOWN status rides the spectator's own
     * CHANNEL_CONTROL carrying countdownRemaining (= delay - history, clamped
     * >= 0). inCountdown clears the tick the seek first returns OK and the seed
     * arms. countdownSentTick throttles the resend — CHANNEL_CONTROL is a
     * 64-deep window, so one send per SPEC_COUNTDOWN_RESEND_TICKS, not per tick. */
    bool     inCountdown;
    uint32_t countdownRemaining;
    uint32_t countdownSentTick;
} SpectatorConn;

/* Server-side global state */
static struct {
    SOCKET sock;
    bool running;
    UdpServerClient clients[MAX_TANKS];
    SpectatorConn   spectators[MAX_SPECTATORS];
    uint32_t tickCount;

    /* Compressed map buffer for sending to joining clients */
    BYTE     compressedMap[MAP_DOWNLOAD_MAX_SIZE];
    uint32_t compressedMapSize;

    /* Per-client map download state */
    ClientMapDownload mapDownload[MAX_TANKS];

    /* Per-client reliable map event queues (EVENT_MAP_CHANGE only).
     * Game events ride CHANNEL_GAME directly; there is no game-event queue. */
    ClientEventQueue mapEventQueues[MAX_TANKS];

    /* Cumulative count of EVENT_MAP_CHANGE events dropped per slot because the
     * map-event queue was full (client too far behind on its acks). A dropped
     * terrain change is a permanent desync the client recovers from with a map
     * resync request — this counter says how often recovery is being leaned on. */
    uint32_t mapEventQueueDrops[MAX_TANKS];

    /* Per-client map generation, tagged onto every map-change event sent on
     * CHANNEL_MAP. Persistent across resyncs (distinct from the transient
     * mapDownload[].resyncGen, which resets to 0 once a resync completes):
     * set to the request's gen when a resync is accepted, so the client can
     * drop a stale in-flight map change that the fresh blob already carries. */
    uint32_t mapGen[MAX_TANKS];

    /* Per-client reliable-ordered channel multiplexer (channel_mux.c).
     * Runs empty and in parallel with the queues above: a channel frame
     * rides every snapshot as a trailer, and a standalone PACKET_CHANNEL
     * carries it when no snapshot flows.  Indexed by clientIdx like the
     * queues; channelMuxInit on join, re-init on disconnect. */
    ChannelMux              channelMux[MAX_TANKS];
    /* Count of channel frames consumed from this slot (trailer + standalone),
     * for test observability of the otherwise-silent parallel layer. */
    uint32_t                channelFramesRx[MAX_TANKS];
    /* Per-client server->client bulk transfer serializer. Holds the in-flight
     * blob (map preview today) and feeds it onto CHANNEL_BULK as the stream
     * window drains; busy while a transfer is mid-flight so two never
     * interleave on one client's byte stream. */
    BulkSender              bulkSend[MAX_TANKS];
    /* Per-client client->server bulk receiver. Reassembles a map upload
     * streamed on CHANNEL_BULK after the BEGIN/ACK handshake approves it. */
    BulkReceiver            bulkRecvUp[MAX_TANKS];
    /* Suppress immediate-send-on-enqueue during the sync-replay burst
     * fired by serverSimRegisterSubscriber, so one carrier datagram
     * packs all replayed events instead of one per event.  Set/cleared
     * by the UDP server around the register call; consumed by the
     * carrier path (Phase 5).  Unused in Phase 3. */
    bool                    controlSyncInProgress[MAX_TANKS];
    /* Slots that overflowed their control-event queue inside a deliver
     * callback (mid-publish) and need a deferred disconnect. The disconnect
     * publishes (the "X has left." broadcast plus the PLAYER_LEFT fan-out from
     * serverSimRemovePlayer), so it can't run mid-publish; the whole teardown
     * is deferred to transportUdpServerDrainPendingRemovals at a safe point in
     * the tick. The slot stays connected until then, so it can't be reused in
     * the meantime. */
    bool                    pendingSimRemove[MAX_TANKS];

    /* Game lock — prevents new players from joining */
    bool gameLocked;           /* Server admin lock */
    bool clientLocked[MAX_TANKS]; /* Per-player lock votes */

    /* Per-client map upload state. clientUploadActive=true between
     * PACKET_LOBBY_MAP_UPLOAD_BEGIN and the final write-out at
     * MAP_UPLOAD_DONE. clientUploadTotal is the approved byte count the
     * incoming bulk transfer must match. clientUploadBuf is a fixed slot of
     * UPLOAD_MAX_BYTES the bulk receiver reassembles into. */
    bool     clientUploadActive[MAX_TANKS];
    uint32_t clientUploadTotal[MAX_TANKS];
    uint8_t  clientUploadBuf[MAX_TANKS][UPLOAD_MAX_BYTES];
    char     clientUploadName[MAX_TANKS][128];
    uint8_t  clientReqCooldownTicks[MAX_TANKS];

    /* Persist-mode staging: bytes of the upload backing the current
     * preview, held until PREVIEW_COMMIT writes them to disk or
     * PREVIEW_CANCEL / a replacing preview drops them. */
    uint8_t      pendingPersistBytes[UPLOAD_MAX_BYTES];
    uint32_t     pendingPersistLen;
    char         pendingPersistName[MAP_STR_SIZE];  /* basename, no .map suffix */
    bool         pendingPersistActive;

    /* Operator-controlled upload handling — zero-init = ALLOW + defaults below. */
    UploadPolicy uploadPolicy;
    uint8_t      uploadMaxFiles;
    uint32_t     uploadMaxStorageBytes;

    /* LRU token buckets for the per-source-IP JOIN rate limit. A zeroed
     * table reads as all-empty (srcAddr 0), so the existing
     * memset(&udpServer, 0, …) is the only reset needed. */
    JoinRateEntry joinRate[JOIN_RL_MAX_SOURCES];
} udpServer;

/* Runtime network impairment (delay/jitter/loss/burst) on the server's
 * inbound (client->server) and outbound (server->client) datagram paths.
 * Disabled unless transportUdpServerSetNetImpair() enables them.  Driven
 * only from the per-tick recv/drain entry points — never from the recv
 * thread, since bolo_rand is not thread-safe. */
static NetImpair srvImpairIn;
static NetImpair srvImpairOut;

/* Outbound datagram wrapper.  Every server->peer send routes through here
 * so the outbound impairment layer can delay/drop/reorder it.  When
 * impairment is disabled (or the datagram is too large for the queue),
 * the packet goes straight onto the wire — behaviourally identical to a
 * direct udpSendTo. */
static void srvSendTo(const uint8_t *buf, int len,
                      const struct sockaddr_in *addr) {
    if (netImpairEnabled(&srvImpairOut) &&
        netImpairOffer(&srvImpairOut, buf, len, addr, (uint64_t)SDL_GetTicks())) {
        return;
    }
    udpSendTo(udpServer.sock, buf, len, addr);
}

/* Public-address override populated by transportUdpServerSetPublicAddress
 * once libplum negotiates a UPnP/NAT-PMP/PCP mapping.  When non-empty the
 * INFO_PACKET build sites advertise these instead of the internal port
 * and a zero address. */
static char           udpServerPublicIp[64];
static unsigned short udpServerPublicPort;

typedef struct {
    struct sockaddr_in addr;
    int packetsRemaining;   /* 0 = slot empty */
    int ticksUntilNext;
} PunchQueueEntry;

#define PUNCH_QUEUE_SIZE        8
#define PUNCH_BURST_PACKETS     5
#define PUNCH_BURST_INTERVAL    3   /* 3 ticks @ 50 Hz ≈ 60 ms */

static PunchQueueEntry punchQueue[PUNCH_QUEUE_SIZE];

/* Forward declaration */
static bool serverClientsAllLocked(void);
static void serverDisconnectClient(ServerSim *sim, int idx, bool graceful);

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

/* Token-bucket join rate limit keyed on source IP (port ignored: a spoofer
 * walks ports). Returns true and consumes a token if allowed; false if the
 * source is over-rate. LRU-evicts the least-recently-seen source on overflow.
 * A real join never originates from 0.0.0.0, so reusing srcAddr==0 as the
 * empty-entry sentinel can't collide with a legitimate source.
 *
 * This throttles port-walking from one source IP; it does not throttle a
 * random-source-IP flood (each packet lands in a fresh bucket and evicts the
 * LRU). The cookie gate is what actually prevents slot exhaustion, so that
 * residual is acceptable. */
static bool serverJoinRateLimitAllow(const struct sockaddr_in *fromAddr) {
    uint32_t key = fromAddr->sin_addr.s_addr;
    uint64_t now = SDL_GetTicks();
    JoinRateEntry *match = NULL;
    JoinRateEntry *empty = NULL;
    JoinRateEntry *oldest = NULL;
    JoinRateEntry *e;
    int i;

    for (i = 0; i < JOIN_RL_MAX_SOURCES; i++) {
        JoinRateEntry *cur = &udpServer.joinRate[i];
        if (cur->srcAddr == key) { match = cur; break; }
        if (cur->srcAddr == 0) {
            if (empty == NULL) empty = cur;
        } else if (oldest == NULL || cur->lastMs < oldest->lastMs) {
            oldest = cur;
        }
    }

    if (match != NULL) {
        e = match;
        /* Refill whole tokens for the elapsed time, capped at the burst, and
         * advance lastMs by the consumed whole windows so the sub-window
         * remainder still counts toward the next refill. */
        if (e->tokens < JOIN_RL_BURST) {
            uint64_t elapsed = now - e->lastMs;
            uint64_t refill  = elapsed / JOIN_RL_REFILL_MS;
            if (refill > 0) {
                if (refill > JOIN_RL_BURST - e->tokens) {
                    refill = JOIN_RL_BURST - e->tokens;
                }
                e->tokens += (uint32_t)refill;
                e->lastMs += refill * JOIN_RL_REFILL_MS;
            }
        }
    } else {
        /* Fresh source: take an empty slot, else evict the least-recently
         * seen one. A new bucket starts full. */
        e = (empty != NULL) ? empty : oldest;
        e->srcAddr = key;
        e->tokens  = JOIN_RL_BURST;
        e->lastMs  = now;
    }

    if (e->tokens == 0) {
        return false;
    }
    e->tokens--;
    return true;
}

/* ── Address-proof retry cookie (anti-spoof) ─────────────────────────────
 * Before a JOIN is allowed to allocate a slot the joiner must echo a cookie
 * the server can recompute for its source address.  The cookie is an HMAC of
 * (address ‖ port ‖ time-window) under a per-process secret, so it is
 * stateless on the server: a blind/IP-spoofed JOIN can't produce one without
 * receiving the challenge at the real address.  The cookie is server-only —
 * the client never computes it, it just stores and echoes the opaque bytes. */
#define COOKIE_WINDOW_SEC 16

/* Fill buf with n bytes from the OS cryptographic RNG. Returns true on
 * success. Deliberately NOT the project's randombytes() (a no-op tweetnacl
 * linkage stub) — the cookie secret must come from a real CSPRNG. */
static bool serverFillRandomBytes(uint8_t *buf, size_t n) {
#if defined(_WIN32)
    /* STATUS_SUCCESS == 0. */
    return BCryptGenRandom(NULL, buf, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
    {
        ssize_t got = getrandom(buf, n, 0);
        if (got == (ssize_t)n) {
            return true;
        }
        /* Old kernel / ENOSYS, or a short read: fall back to /dev/urandom. */
        {
            int fd = open("/dev/urandom", O_RDONLY);
            size_t off = 0;
            if (fd < 0) {
                return false;
            }
            while (off < n) {
                ssize_t r = read(fd, buf + off, n - off);
                if (r <= 0) {
                    close(fd);
                    return false;
                }
                off += (size_t)r;
            }
            close(fd);
            return true;
        }
    }
#else
    arc4random_buf(buf, n);
    return true;
#endif
}

/* Per-process secret keying the cookies. Filled once on first use from the
 * OS CSPRNG so it is unpredictable to a remote attacker: even an attacker who
 * observes a valid (addr,port,window,cookie) tuple from an honest handshake
 * can't recover the secret or forge cookies for a spoofed address. Never
 * leaves the process.
 *
 * Fails closed: if no RNG path succeeds, the secret is left unseeded and this
 * returns NULL so callers refuse to issue or accept cookies, rather than
 * keying them off a guessable value (a predictable secret would defeat the
 * whole anti-spoof feature). A later call retries the RNG. */
static const uint8_t *serverCookieSecret(void) {
    static uint8_t secret[32];
    static bool seeded = false;
    if (!seeded) {
        if (serverFillRandomBytes(secret, sizeof(secret))) {
            seeded = true;
        } else {
            WB_LOG_ERROR(WB_LOG_CAT_NET,
                "cookie secret: no OS CSPRNG available; refusing to key "
                "address-proof cookies from a predictable source");
            return NULL;
        }
    }
    return secret;
}

/* Standard HMAC (RFC 2104) over MD5: 64-byte block, ipad/opad. The cookie is
 * an address proof, not a confidentiality primitive — MD5's break doesn't help
 * an attacker forge one without the secret, and it avoids a new crypto dep. */
static void hmacMd5(const uint8_t *key, size_t keyLen,
                    const uint8_t *msg, size_t msgLen, uint8_t out[16]) {
    uint8_t k0[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[16];
    Md5Ctx ctx;
    size_t i;

    /* Block-pad the key. A key longer than the block would be hashed first;
     * our secret is a fixed 32 bytes so that arm is effectively unused. */
    memset(k0, 0, sizeof(k0));
    if (keyLen > sizeof(k0)) {
        md5Compute(key, keyLen, k0);
    } else {
        memcpy(k0, key, keyLen);
    }
    for (i = 0; i < sizeof(k0); i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36);
        opad[i] = (uint8_t)(k0[i] ^ 0x5c);
    }
    md5Init(&ctx);
    md5Update(&ctx, ipad, sizeof(ipad));
    md5Update(&ctx, msg, msgLen);
    md5Final(inner, &ctx);

    md5Init(&ctx);
    md5Update(&ctx, opad, sizeof(opad));
    md5Update(&ctx, inner, sizeof(inner));
    md5Final(out, &ctx);
}

/* The current cookie time-window.
 * Test-only clock seam: WB_COOKIE_WINDOW_OFFSET shifts the window counter so a
 * test can simulate cookie expiry without waiting real time. Default 0; unset
 * in production. Security-neutral — an attacker can't set a server-side env var
 * remotely and holds no secret, so a shifted window only affects the server's
 * own consistent issue/accept (worst case a self-inflicted reject). Read per
 * call (not cached) so a test can advance the clock mid-run. */
static uint64_t serverCookieCurrentWindow(void) {
    uint64_t w = (SDL_GetTicks() / 1000ULL) / COOKIE_WINDOW_SEC;
    const char *off = getenv("WB_COOKIE_WINDOW_OFFSET");
    if (off != NULL) w += (uint64_t)strtoll(off, NULL, 10);
    return w;
}

/* HMAC-MD5 of (sin_addr ‖ sin_port ‖ window) under the per-process secret.
 * Byte order is irrelevant for security — the secret never leaves the process,
 * so only self-consistency between issue and accept matters. Returns false
 * (out untouched) when the CSPRNG secret is unavailable, so the caller fails
 * closed instead of computing a cookie under a missing key. */
static bool serverCookieCompute(const struct sockaddr_in *addr, uint64_t window,
                                uint8_t out[JOIN_COOKIE_LEN]) {
    const uint8_t *secret = serverCookieSecret();
    uint8_t msg[4 + 2 + 8];
    if (secret == NULL) {
        return false;
    }
    memcpy(msg + 0, &addr->sin_addr.s_addr, 4);
    memcpy(msg + 4, &addr->sin_port, 2);
    memcpy(msg + 6, &window, 8);
    hmacMd5(secret, 32, msg, sizeof(msg), out);
    return true;
}

/* Accept a JOIN cookie for the current window or the one before it (so a
 * cookie issued near a boundary still validates). Constant-time compare:
 * OR-accumulate the byte diffs so a partial match can't be timed byte by byte.
 * A NULL cookie (none echoed) never validates. */
static bool serverCookieAccept(const struct sockaddr_in *addr,
                               const uint8_t *cookieOrNull) {
    uint64_t w;
    uint8_t expect[JOIN_COOKIE_LEN];
    int diff;
    int i;

    if (cookieOrNull == NULL) return false;
    w = serverCookieCurrentWindow();

    /* No secret (CSPRNG unavailable) → fail closed: nothing validates. */
    if (!serverCookieCompute(addr, w, expect)) return false;
    diff = 0;
    for (i = 0; i < JOIN_COOKIE_LEN; i++) diff |= expect[i] ^ cookieOrNull[i];
    if (diff == 0) return true;

    if (!serverCookieCompute(addr, w - 1, expect)) return false;
    diff = 0;
    for (i = 0; i < JOIN_COOKIE_LEN; i++) diff |= expect[i] ^ cookieOrNull[i];
    return diff == 0;
}

/* Issue a fresh challenge: [header PACKET_JOIN_CHALLENGE][cookie for window w].
 * Smaller than a JOIN_REQUEST, so it can't amplify a spoofed source. */
static void serverSendJoinChallenge(const struct sockaddr_in *addr) {
    uint8_t buf[PACKET_HEADER_SIZE + JOIN_COOKIE_LEN];
    packHeader(buf, PACKET_JOIN_CHALLENGE, 0);
    /* Fail closed: with no secret we can't issue a valid challenge, so send
     * nothing rather than a cookie keyed off a missing/guessable secret. */
    if (!serverCookieCompute(addr, serverCookieCurrentWindow(),
                             buf + PACKET_HEADER_SIZE)) {
        return;
    }
    srvSendTo(buf, sizeof(buf), addr);
}

/* connId rides the wire as two 32-bit halves through the existing packU32
 * helpers — low half first, then high. Client send/read must agree with these. */
static void packConnId(uint8_t *buf, uint64_t connId) {
    packU32(buf,     (uint32_t)(connId & 0xffffffffULL));
    packU32(buf + 4, (uint32_t)(connId >> 32));
}
static uint64_t unpackConnId(const uint8_t *buf) {
    uint32_t lo = unpackU32(buf);
    uint32_t hi = unpackU32(buf + 4);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Generate a per-session connection id. SplitMix64 over its own state, seeded
 * once from the high-resolution performance counter — deliberately independent
 * of the process-global bolo_rand stream so a join never perturbs the
 * deterministic sim sequence (baseline replays depend on that stream). Never
 * returns 0, which the wire reserves for "no connId". */
static uint64_t serverNextConnId(void) {
    static uint64_t state;
    static bool seeded = false;
    uint64_t z;
    if (!seeded) {
        state = (uint64_t)SDL_GetPerformanceCounter();
        state ^= 0x9e3779b97f4a7c15ULL * (uint64_t)SDL_GetTicks();
        seeded = true;
    }
    state += 0x9e3779b97f4a7c15ULL;
    z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    return z ? z : 0x9e3779b97f4a7c15ULL;
}

/* Resolve the slot owning an inbound INPUT by its connection id (see header). */
int transportUdpServerFindByConnId(const UdpServerClient *clients,
                                   uint64_t connId,
                                   const struct sockaddr_in *fromAddr,
                                   bool *outRehome) {
    int i;
    if (outRehome) *outRehome = false;
    if (connId == 0 || clients == NULL) return -1;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!clients[i].connected || clients[i].connId != connId) {
            continue;
        }
        if (outRehome &&
            (clients[i].addr.sin_addr.s_addr != fromAddr->sin_addr.s_addr ||
             clients[i].addr.sin_port != fromAddr->sin_port)) {
            *outRehome = true;
        }
        return i;
    }
    return -1;
}

/* Data passed to the balance thread — snapshot of values needed for the
 * HTTP call so the thread doesn't read ServerSim without the mutex. */
typedef struct {
    ServerSim *sim;
    uint8_t    totalPlayers;
    uint8_t    teamSize;
    uint8_t    botSlots[MAX_TANKS];   /* bot slots to include; empty if !includeBots */
    uint8_t    numBotSlots;
    bool       includeBots;
} BalanceThreadData;

/* Background thread: calls WBN balance API (blocks on HTTP) then writes
 * results back under the game mutex so the timer can broadcast them. */
static int balanceThreadFunc(void *data) {
    BalanceThreadData *btd = (BalanceThreadData *)data;
    ServerSim *sim = btd->sim;
    bool includeBots = btd->includeBots;  /* captured before free(btd) below */

    /* This blocks on HTTP — runs outside the game mutex */
    serverSimRequestBalanceProposal(sim, btd->totalPlayers, btd->teamSize,
                                     btd->numBotSlots > 0 ? btd->botSlots : NULL,
                                     btd->numBotSlots);

    free(btd);

    /* If the server is shutting down, signal completion and exit without
     * acquiring the mutex (the main thread may have already torn it down). */
    if (serverSimBalanceShutdownRequested(sim)) {
        serverSimSetBalanceRequestInFlight(sim, false);
        return 0;
    }

    /* Write results back under the game mutex. No approval step — the
     * host already committed to the rebalance by confirming the popup,
     * so apply WBN's assignments directly and broadcast the new slots. */
    threadsWaitForMutex();
    serverSimSetBalanceRequestInFlight(sim, false);
    if (serverSimGetBalanceProposal(sim)->pending) {
        serverSimSetBalanceIncludeBots(sim, includeBots);
        int i;
        /* Publish the proposal data first so every client's dispatcher
         * latches lastBalanceProposalArrivedMs — gives the host's
         * "Teams balanced" status label a chance to fire even though
         * we'll clear right after applying. */
        {
            ControlEvent propEvt;
            memset(&propEvt, 0, sizeof(propEvt));
            propEvt.type = CTRL_BALANCE_PROPOSAL;
            memcpy(propEvt.u.balanceProposal.teamForSlot,
                   serverSimGetBalanceProposal(sim)->teamForSlot,
                   MAX_TANKS);
            serverSimPublishControl(sim, &propEvt);
        }
        /* "Humans only" kicks every bot before applying the human-only
         * team assignments. */
        if (!includeBots) {
            for (i = 0; i < MAX_TANKS; i++) {
                if (serverSimIsBot(sim, (BYTE)i)) {
                    serverSimRemoveBot(sim, (BYTE)i);
                }
            }
        }
        for (i = 0; i < MAX_TANKS; i++) {
            if (serverSimGetBalanceProposal(sim)->teamForSlot[i] != 0) {
                serverSimSetTeamBatch(sim, (BYTE)i,
                                      serverSimGetBalanceProposal(sim)->teamForSlot[i]);
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
        }
        serverSimReapplyTeamAlliances(sim);
        serverSimClearBalanceProposal(sim);
        /* Publish the cleared proposal so balanceProposalActive flips
         * back to false on every client — keeps canBalance gating
         * from staying disabled on the Balance-from-WBN button. */
        {
            ControlEvent clrEvt;
            memset(&clrEvt, 0, sizeof(clrEvt));
            clrEvt.type = CTRL_BALANCE_PROPOSAL;
            serverSimPublishControl(sim, &clrEvt);
        }
        logAddEvent(log_BalanceApplied, 0, 0, 0, 0, 0, NULL);
        serverSimConsoleMessage("Team balance applied (WBN)");
    } else {
        /* WBN call returned without a usable proposal (non-200, null
         * body, or an "error" field — see winbolonet_server.c). Tell
         * the host so its "Asking WBN…" pill can flip immediately. */
        ControlEvent failEvt;
        memset(&failEvt, 0, sizeof(failEvt));
        failEvt.type = CTRL_BALANCE_FAILED;
        failEvt.u.balanceFailed.reasonCode = 1; /* http/transport */
        serverSimPublishControl(sim, &failEvt);
    }
    threadsReleaseMutex();
    return 0;
}

/* Pack a langid + arg list into buf at *pos.  Used by the localized
 * server→client packets (PACKET_JOIN_REJECT and the fromPlayer=0xFF
 * variant of PACKET_CHAT_BROADCAST).  Args are written verbatim as
 * UTF-8 byte strings, each preceded by a length byte (0..PLAYER_NAME_LEN-1).
 * Returns false (and leaves *pos undefined) if argCount > 4 or any arg
 * exceeds the per-arg byte cap. */
static bool packLocalizedPayload(uint8_t *buf, int *pos, int bufSize,
                                 langid id, int argCount,
                                 const char *const args[]) {
    int i;
    if (argCount < 0 || argCount > 4) return false;
    if (*pos + 3 > bufSize) return false;
    /* langid: 2 bytes big-endian */
    buf[(*pos)++] = (uint8_t)((id >> 8) & 0xFF);
    buf[(*pos)++] = (uint8_t)(id & 0xFF);
    buf[(*pos)++] = (uint8_t)argCount;
    for (i = 0; i < argCount; i++) {
        const char *a = (args && args[i]) ? args[i] : "";
        size_t aLen = strlen(a);
        if (aLen > PLAYER_NAME_LEN - 1) return false;
        if (*pos + 1 + (int)aLen > bufSize) return false;
        buf[(*pos)++] = (uint8_t)aLen;
        if (aLen > 0) {
            memcpy(buf + *pos, a, aLen);
            *pos += (int)aLen;
        }
    }
    return true;
}

/* Send a localized join reject to a specific address.  Wire format is
 * documented at PACKET_JOIN_REJECT in netpacks.h. */
static void serverSendJoinReject(const struct sockaddr_in *addr, langid id,
                                 int argCount, const char *const args[]) {
    /* Worst case: 8 hdr + 2 langid + 1 argCount + 4*(1 + 64) = 271. */
    uint8_t buf[PACKET_HEADER_SIZE + 3 + 4 * (1 + PLAYER_NAME_LEN - 1)];
    int pos = PACKET_HEADER_SIZE;
    {
        struct in_addr ia = addr->sin_addr;
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "join reject: dest=%s:%u langid=%u argc=%d",
            inet_ntoa(ia),
            (unsigned)ntohs(addr->sin_port),
            (unsigned)id, argCount);
    }
    packHeader(buf, PACKET_JOIN_REJECT, 0);
    if (!packLocalizedPayload(buf, &pos, sizeof(buf), id, argCount, args)) {
        WB_LOG_ERROR(WB_LOG_CAT_NET,
            "serverSendJoinReject: pack failed id=%u argc=%d",
            (unsigned)id, argCount);
        fprintf(stderr,
                "[UDP SERVER] serverSendJoinReject: pack failed id=%u argc=%d\n",
                (unsigned)id, argCount);
        return;
    }
    /* wire-only: per-client handshake (response to a single client's request) */
    srvSendTo(buf, pos, addr);
}

/* Short name for a ControlEventType — diagnostic logging only. */
static const char *mpDiagCtrlName(int type) {
    switch (type) {
    case CTRL_ALLIANCE_REQUEST: return "ALLIANCE_REQUEST";
    case CTRL_ALLIANCE_ACCEPT:  return "ALLIANCE_ACCEPT";
    case CTRL_ALLIANCE_LEAVE:   return "ALLIANCE_LEAVE";
    case CTRL_ALLIANCE_RESET:   return "ALLIANCE_RESET";
    case CTRL_PLAYER_JOIN:      return "PLAYER_JOIN";
    case CTRL_PLAYER_NAME:      return "PLAYER_NAME";
    case CTRL_LOBBY_SLOT:       return "LOBBY_SLOT";
    case CTRL_LOBBY_SETTINGS:   return "LOBBY_SETTINGS";
    case CTRL_LOBBY_MAP_CHANGE: return "LOBBY_MAP_CHANGE";
    case CTRL_MAP_DOWNLOAD_COMPLETE: return "MAP_DOWNLOAD_COMPLETE";
    case CTRL_BALANCE_PROPOSAL: return "BALANCE_PROPOSAL";
    case CTRL_MAP_SKIP_STATE:   return "MAP_SKIP_STATE";
    case CTRL_GAME_PHASE_LOBBY: return "GAME_PHASE_LOBBY";
    case CTRL_GAME_PHASE_COUNTDOWN: return "GAME_PHASE_COUNTDOWN";
    case CTRL_GAME_PHASE_RUNNING:   return "GAME_PHASE_RUNNING";
    case CTRL_GAME_PHASE_GAME_OVER: return "GAME_PHASE_GAME_OVER";
    case CTRL_GAME_OVER:        return "GAME_OVER";
    case CTRL_SERVER_SHUTDOWN:  return "SERVER_SHUTDOWN";
    case CTRL_CHAT:             return "CHAT";
    case CTRL_PLAYER_LEAVE:     return "PLAYER_LEAVE";
    case CTRL_LOBBY_TEAM_META:  return "LOBBY_TEAM_META";
    case CTRL_LOBBY_BOT_CONFIG: return "LOBBY_BOT_CONFIG";
    case CTRL_LOBBY_BOT_BRAIN:  return "LOBBY_BOT_BRAIN";
    case CTRL_LOBBY_BRAIN_LIST: return "LOBBY_BRAIN_LIST";
    case CTRL_GAME_VOTE_STATE:  return "GAME_VOTE_STATE";
    case CTRL_SERVER_TEXT:      return "SERVER_TEXT";
    case CTRL_SHELL_DEATH:      return "SHELL_DEATH";
    case CTRL_CHANNEL_RESET:    return "CHANNEL_RESET";
    default:                    return "<unknown>";
    }
}

/* Flush one client's channel onto the wire immediately as a standalone
 * PACKET_CHANNEL.  Used to carry a just-published control event when no later
 * carrier tick is guaranteed to follow — the server teardown publishes
 * CTRL_SERVER_SHUTDOWN and then tears the slot down in the same call, with no
 * snapshot or check-timeouts pass after it.  During running the snapshot
 * trailer is the carrier, so callers skip this path there. */
static void transportUdpServerFlushChannel(int clientIdx) {
    UdpServerClient *client;
    uint8_t cbuf[UDP_MAX_PAYLOAD];
    int frameLen;
    if (clientIdx < 0 || clientIdx >= MAX_TANKS) return;
    client = &udpServer.clients[clientIdx];
    if (!client->connected) return;
    channelTick(&udpServer.channelMux[clientIdx], udpServer.tickCount,
                client->pingMs);
    frameLen = channelBuildFrame(&udpServer.channelMux[clientIdx],
                                 cbuf + PACKET_HEADER_SIZE,
                                 UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
    if (frameLen > 2) {
        packHeader(cbuf, PACKET_CHANNEL, client->outSequence++);
        srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen, &client->addr);
    }
}

/* Per-client subscriber deliver callback.  Filters single-recipient
 * variants, enqueues into this client's reliable control queue, and
 * sends the unacked tail immediately when outside running (the snapshot
 * tail handles running).  The controlSyncInProgress flag suppresses
 * the immediate-send during a serverSimRegisterSubscriber replay so
 * the burst lands in one carrier datagram rather than one per event. */
static void udpClientDeliverControl(void *ctx, const ControlEvent *evt) {
    UdpServerClient *client = (UdpServerClient *)ctx;
    int idx;

    idx = (int)(client - udpServer.clients);
    if (!client->connected) {
        mpDiagLog("[srv] deliver SKIP slot=%d type=%s reason=not-connected",
                  idx, mpDiagCtrlName((int)evt->type));
        return;
    }

    /* Per-recipient filtering for single-target variants.  The codec
     * stays UdpServerClient-agnostic; the slot comparison lives here
     * where the recipient's player number is in scope. */
    if (evt->type == CTRL_ALLIANCE_REQUEST &&
        evt->u.allianceRequest.toPlayer != client->playerNum) {
        mpDiagLog("[srv] deliver FILTER slot=%d type=ALLIANCE_REQUEST toPlayer=%d clientPlayerNum=%d",
                  idx, (int)evt->u.allianceRequest.toPlayer, (int)client->playerNum);
        return;
    }
    if (evt->type == CTRL_CHAT) {
        BYTE from = evt->u.chat.fromPlayer;
        BYTE dest = evt->u.chat.destPlayer;
        if (dest == 0xFF) {
            /* Broadcast: skip the original sender if it's a real player. */
            if (from < MAX_TANKS && client->playerNum == from) {
                mpDiagLog("[srv] deliver FILTER slot=%d type=CHAT reason=sender-skip from=%d",
                          idx, (int)from);
                return;
            }
        } else if (CHAT_DEST_IS_TEAM(dest)) {
            /* Team-addressed: only members of the addressed team receive,
             * and the real-player sender is skipped (local echo covers it). */
            const LobbyPlayer *lp =
                serverSimGetLobbyPlayer(serverSimGetActive(), client->playerNum);
            if (!lp || lp->teamNumber != CHAT_DEST_TEAM_OF(dest)) {
                mpDiagLog("[srv] deliver FILTER slot=%d type=CHAT reason=not-on-team dest=%d clientPlayerNum=%d",
                          idx, (int)dest, (int)client->playerNum);
                return;
            }
            if (from < MAX_TANKS && client->playerNum == from) {
                mpDiagLog("[srv] deliver FILTER slot=%d type=CHAT reason=sender-skip from=%d",
                          idx, (int)from);
                return;
            }
        } else {
            /* Unicast: only the addressed slot receives. */
            if (client->playerNum != dest) {
                mpDiagLog("[srv] deliver FILTER slot=%d type=CHAT reason=not-addressed dest=%d clientPlayerNum=%d",
                          idx, (int)dest, (int)client->playerNum);
                return;
            }
        }
    }
    if (evt->type == CTRL_COMMAND_REJECTED &&
        evt->u.commandRejected.origSlot != client->playerNum) {
        mpDiagLog("[srv] deliver FILTER slot=%d type=COMMAND_REJECTED "
                  "origSlot=%d clientPlayerNum=%d",
                  idx, (int)evt->u.commandRejected.origSlot,
                  (int)client->playerNum);
        return;
    }
    if (evt->type == CTRL_BALANCE_FAILED && client->playerNum != 0) {
        /* The balance flow is host-driven; only slot 0 needs the
         * failure pill. Skip the fan-out for everyone else. */
        return;
    }
    if (evt->type == CTRL_SHELL_DEATH &&
        evt->u.shellDeath.owner != client->playerNum) {
        /* Owner-only: the firing player is the sole recipient (mirrors
         * CTRL_COMMAND_REJECTED). Drop for every other slot. */
        return;
    }

    /* Route onto the reliable control channel (CHANNEL_CONTROL).  Per-event
     * wire layout matches the body-only codec table: type(1) + bodyLen(2 BE)
     * + body(N); the receiver dispatches each event through
     * transportControlCodecBodyDecoder.  The channel carries and retransmits
     * the event in both phases — its frame rides the snapshot trailer during
     * running and a standalone PACKET_CHANNEL otherwise — so no phase-gated
     * immediate send is needed here.
     *
     * An event with no body encoder is dropped (it was never deliverable),
     * matching the former send-time `enc == NULL` skip.  A full window means
     * the client has stopped acking control: defer its disconnect off this
     * publish path (mirrors the game/map channel overflow at the snapshot
     * drain), flag-guarded so a re-hit on the still-connected slot can't spam
     * the log.  serverDisconnectClient / serverSimRemovePlayer cannot run from
     * inside this deliver callback without re-entering serverSimPublishControl
     * and tripping its reentrancy guard, so the teardown waits for
     * transportUdpServerDrainPendingRemovals at a safe point in the tick. */
    {
        ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(evt->type);
        uint8_t msg[CHANNEL_CONTROL_SEG];
        size_t bodyLen = 0;
        if (enc == NULL) {
            mpDiagLog("[srv] deliver SKIP slot=%d type=%s reason=no-encoder",
                      idx, mpDiagCtrlName((int)evt->type));
            return;
        }
        if (enc(evt, client, msg + 3, sizeof(msg) - 3, &bodyLen) != ENCODE_OK) {
            mpDiagLog("[srv] deliver SKIP slot=%d type=%s reason=encode",
                      idx, mpDiagCtrlName((int)evt->type));
            return;
        }
        msg[0] = (uint8_t)evt->type;
        packU16(msg + 1, (uint16_t)bodyLen);
        if (!channelSend(&udpServer.channelMux[idx], CHANNEL_CONTROL,
                         msg, (uint16_t)(3 + bodyLen))) {
            if (!udpServer.pendingSimRemove[idx]) {
                WB_LOG_ERROR(WB_LOG_CAT_NET,
                             "control channel overflow for slot %d, deferring disconnect",
                             idx);
                mpDiagLog("[srv] OVERFLOW slot=%d type=%s -> deferring disconnect",
                          idx, mpDiagCtrlName((int)evt->type));
                udpServer.pendingSimRemove[idx] = true;
            }
            return;
        }
        mpDiagLog("[srv] CTRL->ch2 slot=%d type=%s bodyLen=%u",
                  idx, mpDiagCtrlName((int)evt->type), (unsigned)bodyLen);
    }

    /* Carry it now when outside running: the per-tick standalone PACKET_CHANNEL
     * would otherwise pick it up, but a control event published with no later
     * tick (CTRL_SERVER_SHUTDOWN, emitted as the server tears the slot down)
     * must flush synchronously.  During running the snapshot trailer is the
     * carrier; during the join sync-replay the burst is coalesced into one
     * flush after registration. */
    if (!udpServer.controlSyncInProgress[idx] &&
        serverSimGetState(serverSimGetActive()) != serverStateRunning) {
        transportUdpServerFlushChannel(idx);
    }
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
static void udpServerClearClientUploadState(int idx) {
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

/* Build and send the join accept packet with the slot, current server
 * tick, and compressed map size. */
static void serverSendJoinAccept(int slot, ServerSim *sim,
                                 const struct sockaddr_in *addr) {
    uint8_t acceptBuf[PACKET_HEADER_SIZE + 9 + 8];
    int pos;

    packHeader(acceptBuf, PACKET_JOIN_ACCEPT,
               udpServer.clients[slot].outSequence++);
    pos = PACKET_HEADER_SIZE;
    acceptBuf[pos++] = (uint8_t)slot;
    packU32(acceptBuf + pos, serverSimGetTick(sim));
    pos += 4;
    packU32(acceptBuf + pos, udpServer.compressedMapSize);
    pos += 4;
    /* connId the client echoes on its INPUT packets for NAT-rebind re-homing. */
    packConnId(acceptBuf + pos, udpServer.clients[slot].connId);
    pos += 8;

    /* wire-only: per-client handshake (response to a single client's request) */
    srvSendTo(acceptBuf, pos, addr);
}

/* Find a connected spectator by source address. Returns the spectators[]
 * index or -1. Mirrors serverFindClient over the parallel array. */
static int serverFindSpectator(const struct sockaddr_in *addr) {
    int i;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (udpServer.spectators[i].connected &&
            udpServer.spectators[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            udpServer.spectators[i].addr.sin_port == addr->sin_port) {
            return i;
        }
    }
    return -1;
}

/* Spectator-flavoured JOIN_ACCEPT: same PACKET_JOIN_ACCEPT layout as
 * serverSendJoinAccept, but reads spectators[s] and marks the connection a
 * viewer — slot byte = 0xFF (no tank slot) and compressedMapSize = 0 (the
 * map arrives in the ring seed, not the live download path). */
static void serverSendSpectatorAccept(int s, ServerSim *sim,
                                      const struct sockaddr_in *addr) {
    uint8_t acceptBuf[PACKET_HEADER_SIZE + 9 + 8];
    int pos;

    packHeader(acceptBuf, PACKET_JOIN_ACCEPT,
               udpServer.spectators[s].outSequence++);
    pos = PACKET_HEADER_SIZE;
    acceptBuf[pos++] = 0xFF;                       /* viewer: no tank slot */
    packU32(acceptBuf + pos, serverSimGetTick(sim));
    pos += 4;
    packU32(acceptBuf + pos, 0);                   /* no live map download */
    pos += 4;
    packConnId(acceptBuf + pos, udpServer.spectators[s].connId);
    pos += 8;

    srvSendTo(acceptBuf, pos, addr);
}

/* Accept a "join as viewer" connection: register it in spectators[] with no
 * tank slot, no sim player, and no control-bus subscription, then send the
 * spectator accept. Caller has already passed every shared JOIN pre-check
 * (cookie, version, name, password). */
static void serverAcceptSpectator(ServerSim *sim,
                                  const struct sockaddr_in *fromAddr,
                                  const char *name,
                                  uint8_t clientType, uint8_t clientHints) {
    int effectiveCap;
    int s;

    /* Re-JOIN from a known spectator address: resend the accept, no new slot. */
    s = serverFindSpectator(fromAddr);
    if (s >= 0) {
        serverSendSpectatorAccept(s, sim, fromAddr);
        return;
    }

    /* Cap: operator setting clamped to the array size. 0 disables spectating. */
    effectiveCap = (int)serverSimGetMaxSpectators(sim);
    if (effectiveCap > MAX_SPECTATORS) effectiveCap = MAX_SPECTATORS;
    if (effectiveCap <= 0) {
        serverSendJoinReject(fromAddr, STR_REJECT_SERVER_FULL, 0, NULL);
        return;
    }

    /* First free slot within the effective cap. */
    s = -1;
    {
        int i;
        for (i = 0; i < effectiveCap; i++) {
            if (!udpServer.spectators[i].connected) {
                s = i;
                break;
            }
        }
    }
    if (s < 0) {
        serverSendJoinReject(fromAddr, STR_REJECT_SERVER_FULL, 0, NULL);
        return;
    }

    udpServer.spectators[s].connected        = true;
    udpServer.spectators[s].addr             = *fromAddr;
    udpServer.spectators[s].connId           = serverNextConnId();
    snprintf(udpServer.spectators[s].playerName,
             PACKET_MAX_PLAYER_NAME, "%s", name);
    udpServer.spectators[s].lastReceivedTick = udpServer.tickCount;
    udpServer.spectators[s].outSequence      = 1;
    udpServer.spectators[s].inboundCmdSeq     = 0;
    udpServer.spectators[s].pingMs           = 0;
    udpServer.spectators[s].clientType       = clientType;
    udpServer.spectators[s].clientHints      = clientHints;
    channelMuxInit(&udpServer.spectators[s].channelMux);
    bulkSenderInit(&udpServer.spectators[s].bulkSend);
    udpServer.spectators[s].seedBlob     = NULL;
    udpServer.spectators[s].seedLen      = 0;
    udpServer.spectators[s].seedGen      = 0;
    udpServer.spectators[s].seedBegun    = false;
    udpServer.spectators[s].seedComplete = false;
    udpServer.spectators[s].xferStartSeq = 0;
    udpServer.spectators[s].xferEndSeq   = 0;
    udpServer.spectators[s].seedSeq        = 0;
    udpServer.spectators[s].lastEmittedSeq = 0;
    udpServer.spectators[s].inCountdown        = false;
    udpServer.spectators[s].countdownRemaining = 0;
    udpServer.spectators[s].countdownSentTick  = 0;

    serverSendSpectatorAccept(s, sim, fromAddr);
}

/* Release a spectator slot. A spectator holds no tank, no sim player, and no
 * control-bus subscription, so teardown is just freeing the slot and resetting
 * its in-place channel/bulk state — none of serverDisconnectClient's
 * WBN/chat/subscriber/serverSimRemovePlayer work applies. graceful=false is a
 * timeout; graceful=true is reserved for the explicit leave path (2c) and does
 * the same teardown for now. */
static void serverDisconnectSpectator(int s, bool graceful) {
    if (s < 0 || s >= MAX_SPECTATORS || !udpServer.spectators[s].connected) {
        return;
    }
    mpDiagLog("[srv] SPECTATOR DISCONNECT idx=%d graceful=%d", s, (int)graceful);
    udpServer.spectators[s].connected = false;
    channelMuxInit(&udpServer.spectators[s].channelMux);   /* reset in place */
    bulkSenderInit(&udpServer.spectators[s].bulkSend);
    if (udpServer.spectators[s].seedBlob != NULL) {
        free(udpServer.spectators[s].seedBlob);
        udpServer.spectators[s].seedBlob = NULL;
    }
    udpServer.spectators[s].seedLen = 0;
    udpServer.spectators[s].seedGen = 0;
    udpServer.spectators[s].seedBegun = false;
    udpServer.spectators[s].seedComplete = false;
    udpServer.spectators[s].xferStartSeq = 0;
    udpServer.spectators[s].xferEndSeq = 0;
    udpServer.spectators[s].seedSeq = 0;
    udpServer.spectators[s].lastEmittedSeq = 0;
    udpServer.spectators[s].inCountdown = false;
    udpServer.spectators[s].countdownRemaining = 0;
    udpServer.spectators[s].countdownSentTick = 0;
    udpServer.spectators[s].playerName[0] = '\0';
    udpServer.spectators[s].outSequence = 0;
    udpServer.spectators[s].inboundCmdSeq = 0;
}

/* Send PACKET_WBN_REKEY to a single connected client carrying the current
 * server_key.  Called right after JOIN_ACCEPT so the joiner learns the
 * WBN session key without a credential ever riding the JOIN wire field,
 * and from the broadcast wrapper after each return-to-lobby rotation.
 * Silently no-ops when WBN isn't running or the server has no session
 * key yet, so non-WBN servers (and the JOIN_ACCEPT path on them) pay
 * nothing. */
static void transportUdpServerSendWbnRekey(UdpServerClient *c) {
    uint8_t buf[PACKET_HEADER_SIZE + WBN_JOIN_KEY_WIRE_LEN];
    char serverKey[WINBOLONET_KEY_LEN];

    if (!winbolonetIsRunning()) return;

    winboloNetGetServerKey(serverKey);
    if (serverKey[0] == '\0') return;

    packHeader(buf, PACKET_WBN_REKEY, c->outSequence++);
    wbnKeyEncode(buf + PACKET_HEADER_SIZE, serverKey);

    /* wire-only: per-client capability refresh (no in-process audience) */
    srvSendTo(buf, sizeof(buf), &c->addr);
}

/* Broadcast PACKET_WBN_REKEY to every connected client that was
 * WBN-verified last round, after the server rotates its server_key
 * (post-returnToLobby).  Each client mints a fresh player_key against
 * the new key and re-auths, re-registering for the new session.
 *
 * The gate is the durable per-connection wbnWasVerified bit, NOT the
 * sim-side PLAYER_FLAG_WBN_VERIFIED nor the per-slot WBN key.  The key is
 * out: winbolonetEndSession just wiped every key, so a key-based gate
 * (winboloNetIsPlayerParticipant) would match nobody.  The flag is out
 * too: serverSimReturnToLobby clears PLAYER_FLAG_WBN_VERIFIED on every
 * slot earlier in this same tick (it means "verified for the current
 * session", and the session was just torn down), so a flag-based gate
 * would likewise match nobody and silently strand every player un-keyed
 * for the new round.  wbnWasVerified lives in the transport client struct,
 * untouched by the sim reset, so it survives as the cross-round signal.
 * Re-arm the deferred-join state for each rekeyed slot so the incoming
 * reauth fires a fresh keyed PLAYER_JOIN for the new game (or the grace
 * sweep an anonymous one if the reauth never lands). */
void transportUdpServerBroadcastWbnRekey(ServerSim *sim) {
    int i;
    if (!winbolonetIsRunning()) return;
    for (i = 0; i < MAX_TANKS; i++) {
        bool wasVerified = udpServer.clients[i].wbnWasVerified;
        if (!wbnRekeyTargetSelected(udpServer.clients[i].connected, wasVerified))
            continue;
        transportUdpServerSendWbnRekey(&udpServer.clients[i]);
        wbnJoinArm(&udpServer.clients[i].wbnJoin,
                   udpServer.tickCount, WBN_JOIN_REGISTER_GRACE_TICKS);
    }
}

/* Send map chunks to a client that is downloading */
/* Begin the armed map transfer once the bulk channel is quiescent. The
 * BulkSender's busy flag clears at staging-complete, not drain-complete, so a
 * correct endSeq needs the stream truly idle: no pending staging bytes and the
 * send window fully acked. startSeq is captured before any byte is staged, so
 * endSeq = startSeq + segment count is exact (channelStreamRefill only ever
 * forms a short final segment for a contiguous blob). No readiness round-trip
 * gates this: the join cookie already proved the address (serverHandleJoinRequest),
 * and a resync targets an already-established slot. */
static void serverBeginMapTransferIfReady(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    ChannelMux *m = &udpServer.channelMux[slot];
    ChannelState *bulk = &m->ch[CHANNEL_BULK];
    BulkStreamHeader sh;
    uint32_t headerLen, totalBytes, segs;

    if (dl->xferKind == MAP_XFER_NONE || dl->xferBegun) return;
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
 * held map events flush and the client's installedMapGen gate takes over). */
static void serverCompleteMapTransferIfAcked(int slot) {
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
}

/* Per-tick service: begin an armed transfer when the channel is idle, then
 * fire completion when the peer has acked it through. Safe to call every tick
 * for any connected slot; a no-op when nothing is armed. */
static void serverServiceMapTransfer(int slot) {
    serverBeginMapTransferIfReady(slot);
    serverCompleteMapTransferIfAcked(slot);
}

/* Per-tick spectator service: seed each connected spectator with the delayed
 * keyframe at head - specDelayTicks, drain it over CHANNEL_BULK exactly as the
 * client map-download carrier drains a join download, then stream the ring
 * forward — emit each record in (lastEmittedSeq, head - delay] as a
 * BULK_KIND_SPEC_RECORD blob so the view lags exactly specDelayTicks behind the
 * live head and never reaches the current tick. Cold-start tolerant — the seek
 * is retried every tick until the ring has enough history; the countdown signal
 * to the client is 2d-f, not here.
 *
 * Runs on the tick thread, the same thread as logWriteTick's
 * spectatorRingRecordTick, so the seek is race-free. A cursor/keyframe pointer
 * invalidates on the next RecordTick, so the keyframe bytes are copied into
 * spectator-owned storage at seek time (mirroring serverInitMapDownload's copy
 * of the compressed map); the bulk transfer then drains that copy across ticks. */

/* CHANNEL_CONTROL is a 64-deep reliable window; a cold-start countdown that ran
 * for up to specDelayTicks at one send per tick would overflow it. Resend the
 * countdown status no more than once every this many ticks (~2/s at 50 tick/s). */
#define SPEC_COUNTDOWN_RESEND_TICKS 25u

static void serverServiceSpectators(ServerSim *sim) {
    int i;

    for (i = 0; i < MAX_SPECTATORS; i++) {
        SpectatorConn *sp = &udpServer.spectators[i];
        ChannelState *bulk;

        if (!sp->connected) continue;

        /* Seek the delayed keyframe. OK -> copy it into spectator-owned storage
         * and clear any countdown. COLD_START (the ring does not yet hold a full
         * specDelayTicks of history) -> send a throttled countdown status on the
         * spectator's own CHANNEL_CONTROL and keep retrying; no seed is copied
         * and the feed below cannot run (!seedComplete), so no live state leaks
         * during the wait. The seek flips to OK exactly when head - delay >=
         * oldest, at which point the existing seed path takes over unchanged. */
        if (sp->seedBlob == NULL && !sp->seedComplete) {
            SpectatorRing *r = serverInstanceGetSpectatorRing();
            if (r != NULL) {
                SpectatorRingCursor cur;
                uint32_t delay = serverSimGetSpecDelayTicks(sim);
                SpectatorRingSeekStatus st =
                    spectatorRingSeekDelayed(r, delay, &cur);
                if (st == SPECTATOR_RING_OK) {
                    int kfLen = 0;
                    const uint8_t *kf =
                        spectatorRingCursorKeyframe(&cur, &kfLen, NULL);
                    sp->inCountdown = false;
                    if (kf != NULL && kfLen > 0) {
                        uint8_t *copy = (uint8_t *)malloc((size_t)kfLen);
                        if (copy != NULL) {
                            memcpy(copy, kf, (size_t)kfLen);
                            sp->seedBlob = copy;
                            sp->seedLen  = (uint32_t)kfLen;
                            sp->seedGen  = cur.segment;
                            sp->seedSeq  = spectatorRingCursorSeedSeq(&cur);
                        }
                    }
                } else if (st == SPECTATOR_RING_COLD_START) {
                    uint32_t history =
                        spectatorRingHeadSeq(r) - spectatorRingOldestSeq(r);
                    uint32_t remaining =
                        (delay > history) ? delay - history : 0;
                    bool firstEntry = !sp->inCountdown;
                    sp->inCountdown        = true;
                    sp->countdownRemaining = remaining;
                    /* Throttle: send on first entry, then at most once every
                     * SPEC_COUNTDOWN_RESEND_TICKS so the control window can't
                     * overflow across a long wait. */
                    if (firstEntry ||
                        udpServer.tickCount - sp->countdownSentTick
                            >= SPEC_COUNTDOWN_RESEND_TICKS) {
                        uint8_t buf[SPEC_CTRL_COUNTDOWN_LEN];
                        buf[0] = SPEC_CTRL_COUNTDOWN;
                        buf[1] = (uint8_t)(remaining >> 24);
                        buf[2] = (uint8_t)(remaining >> 16);
                        buf[3] = (uint8_t)(remaining >> 8);
                        buf[4] = (uint8_t)remaining;
                        channelSend(&sp->channelMux, CHANNEL_CONTROL,
                                    buf, SPEC_CTRL_COUNTDOWN_LEN);
                        sp->countdownSentTick = udpServer.tickCount;
                    }
                }
                /* AGED_OUT at join should not occur — retention covers the
                 * delay; leave it as a no-op and retry next tick. */
            }
        }

        /* Arm a seed transfer once the bulk channel is fully idle — same gates
         * serverBeginMapTransferIfReady applies (sender idle, staging empty,
         * send window drained). */
        bulk = &sp->channelMux.ch[CHANNEL_BULK];
        if (sp->seedBlob != NULL && !sp->seedBegun &&
            !bulkSenderBusy(&sp->bulkSend) &&
            sp->channelMux.streamCount == 0 &&
            bulk->ackedSeq == bulk->nextSeq) {
            BulkStreamHeader sh;
            uint32_t headerLen, totalBytes, segs;

            memset(&sh, 0, sizeof(sh));
            sh.kind = BULK_KIND_SPEC_SEED;
            sh.gen = sp->seedGen;
            sh.totalSize = sp->seedLen;
            sh.pathLen = 0;
            sh.path[0] = '\0';

            sp->xferStartSeq = bulk->nextSeq;
            if (bulkSenderBegin(&sp->bulkSend, &sh, sp->seedBlob, sp->seedLen)) {
                headerLen = (uint32_t)BULK_STREAM_HEADER_FIXED + sh.pathLen;
                totalBytes = headerLen + sp->seedLen;
                segs = (totalBytes + CHANNEL_BULK_SEG - 1) / CHANNEL_BULK_SEG;
                sp->xferEndSeq = sp->xferStartSeq + segs;
                sp->seedBegun = true;
            }
            /* allocation failure: retry next tick */
        }

        /* Complete: peer has acked the whole seed. Free the copy and start the
         * forward feed from the seeded keyframe's recordSeq. Once-guarded
         * (!seedComplete) so the lastEmittedSeq init fires only on the
         * transition — the ack gate stays true on every later tick. */
        if (sp->seedBegun && !sp->seedComplete &&
            bulk->ackedSeq >= sp->xferEndSeq) {
            sp->seedComplete = true;
            if (sp->seedBlob != NULL) {
                free(sp->seedBlob);
                sp->seedBlob = NULL;
            }
            sp->lastEmittedSeq = sp->seedSeq;
        }

        /* Forward feed: after the seed, emit each ring record in
         * (lastEmittedSeq, head - delay] as a BULK_KIND_SPEC_RECORD blob,
         * walking recordSeq forward (no per-tick re-seek). The view lags exactly
         * specDelayTicks behind the live head and never reaches it (DD-7).
         * bulkSenderBegin copies each record, so it is assembled in a transient
         * local and freed at once; the bulkSenderBusy guard keeps one blob in
         * flight and is the per-tick staging backpressure. */
        if (sp->seedComplete) {
            SpectatorRing *r = serverInstanceGetSpectatorRing();
            if (r != NULL) {
                uint32_t head  = spectatorRingHeadSeq(r);
                uint32_t delay = serverSimGetSpecDelayTicks(sim);
                uint32_t target = (head > delay) ? head - delay : 0;

                while (sp->lastEmittedSeq < target &&
                       !bulkSenderBusy(&sp->bulkSend)) {
                    uint32_t seq = sp->lastEmittedSeq + 1;
                    bool isKf;
                    const uint8_t *pl;
                    int plen;
                    uint32_t gt, seg, blen;
                    uint8_t *blob;
                    BulkStreamHeader sh;

                    SDL_assert(delay == 0 || head - seq >= delay);   /* DD-7 */

                    if (!spectatorRingRecordAt(r, seq, &isKf, &pl, &plen,
                                               &gt, &seg)) {
                        /* The next record aged out (pathological slow
                         * spectator) — drop the feed and re-seed a fresh
                         * keyframe (the seek block re-arms once the channel
                         * drains). */
                        sp->seedComplete = false;
                        sp->seedBegun = false;
                        break;
                    }

                    blen = 9u + (uint32_t)plen;
                    blob = (uint8_t *)malloc(blen);
                    if (blob == NULL) break;   /* retry next tick */
                    blob[0] = isKf ? 1u : 0u;
                    packU32(blob + 1, gt);
                    packU32(blob + 5, seg);
                    if (plen > 0 && pl != NULL) {
                        memcpy(blob + 9, pl, (size_t)plen);
                    }

                    memset(&sh, 0, sizeof(sh));
                    sh.kind = BULK_KIND_SPEC_RECORD;
                    sh.gen = seq;
                    sh.totalSize = blen;
                    if (!bulkSenderBegin(&sp->bulkSend, &sh, blob, blen)) {
                        free(blob);
                        break;
                    }
                    free(blob);   /* bulkSenderBegin copied it into its own buf */
                    sp->lastEmittedSeq = seq;
                    bulkSenderPump(&sp->bulkSend, &sp->channelMux);
                }
            }
        }

        /* Carrier: pump staged bytes into the mux and emit standalone
         * PACKET_CHANNEL frames — carries both the seed and the forward records.
         * Mirrors the client map-download carrier exactly. */
        bulkSenderPump(&sp->bulkSend, &sp->channelMux);
        channelTick(&sp->channelMux, udpServer.tickCount, sp->pingMs);
        {
            int frames;
            for (frames = 0; frames < MAP_DOWNLOAD_FRAMES_PER_TICK; frames++) {
                uint8_t cbuf[UDP_MAX_PAYLOAD];
                int frameLen = channelBuildFrame(
                    &sp->channelMux, cbuf + PACKET_HEADER_SIZE,
                    UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
                if (frameLen <= 2) break;   /* nothing (more) to carry this tick */
                packHeader(cbuf, PACKET_CHANNEL, sp->outSequence++);
                srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen, &sp->addr);
            }
        }
    }
}

/* Initialize map download tracking for a client and arm a join download on the
 * bulk channel. The blob begins streaming once the channel is idle and the
 * per-tick carrier (transportUdpServerSend) feeds it. */
static void serverInitMapDownload(int slot) {
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
}

/* Clean up map download tracking for a client */
static void serverCleanupMapDownload(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    if (dl->compressedMap != NULL) {
        free(dl->compressedMap);
        dl->compressedMap = NULL;
    }
    dl->downloadComplete = FALSE;
    dl->xferKind = MAP_XFER_NONE;
    dl->xferBegun = false;
}

static void serverSendServerMessage(ServerSim *sim, langid id, int argCount,
                                    const char *const args[]);
static void serverSendServerEnglishBroadcast(ServerSim *sim, const char *message);

/* Phase 5 verified-priority preempt — rename `victimSlot` to `chosenName`
 * and broadcast the change to all connected clients.  Sets the
 * sticky-suffix flag so the slot keeps its renamed form for the rest of
 * the session.  `incomingName` and `incomingCountry` describe the
 * verified joiner that triggered the rename; both are used for the
 * newswire announce. */
static void serverPreemptRename(ServerSim *sim, int victimSlot,
                                const char *chosenName,
                                const char *incomingName,
                                const char *incomingCountry) {
    GameSim *gs = serverSimGetGameSim(sim);
    char originalName[PACKET_MAX_PLAYER_NAME];

    strncpy(originalName, udpServer.clients[victimSlot].playerName,
            PACKET_MAX_PLAYER_NAME - 1);
    originalName[PACKET_MAX_PLAYER_NAME - 1] = '\0';

    /* Update the per-slot transport-side name. */
    snprintf(udpServer.clients[victimSlot].playerName,
             PACKET_MAX_PLAYER_NAME, "%s", chosenName);
    udpServer.clients[victimSlot].nameStickySuffix = true;

    /* Update the gameSim player record and publish the name change —
     * same path PACKET_NAME_CHANGE uses. */
    serverSimSetPlayerName(sim, (BYTE)victimSlot, chosenName);

    /* Publish a single-slot lobby update so other surfaces (lobby
     * table, players panel) refresh. */
    serverSimPublishLobbySlot(sim, (BYTE)victimSlot);

    /* Post a newswire announcement. The server's messageAdd callback
     * drops newswire messages today (see server_sim.c:serverSimCbMessageAdd),
     * but we mirror the existing emit pattern for consistency and so a
     * future server-side listener picks it up automatically.  Clients
     * generate their own MESSAGE_CHANGENAME announce when they apply
     * PACKET_NAME_CHANGE, so the visible chat update on each client comes
     * from that path; the verified-specific phrasing is informational
     * for now (Phase 11 wires it to chat). */
    {
        MessageArgs args;
        memset(&args, 0, sizeof(args));
        strncpy(args.playerName, originalName, PLAYER_NAME_LEN - 1);
        strncpy(args.otherName, incomingName, PLAYER_NAME_LEN - 1);
        args.playerFlags = playersGetAccountFlags(&gs->plyrs,
                                                  (BYTE)victimSlot);
        playersGetCountryCode(&gs->plyrs, (BYTE)victimSlot,
                              args.playerCountry);
        /* Incoming joiner has a slot allocated but isn't in the players
         * struct yet; pull country from the caller-supplied lookup and
         * mark the WBN flag manually since we only get here when they're
         * verified. */
        args.otherFlags = (uint8_t)PLAYER_FLAG_WBN_VERIFIED;
        if (incomingCountry) {
            args.otherCountry[0] = incomingCountry[0];
            args.otherCountry[1] = incomingCountry[1];
            args.otherCountry[2] = '\0';
        }
        gs->callbacks.messageAdd(gs->callbacks.ctx,
                                 globalMessage, MESSAGE_NEWSWIRE,
                                 STR_NAME_RENAMED_BY_VERIFIED, &args);
    }

    /* Phase 5.1: surface the announcement to clients via the server-message
     * broadcast path (the messageAdd callback above drops on the server). */
    {
        const char *renameArgs[2];
        renameArgs[0] = originalName;
        renameArgs[1] = incomingName;
        serverSendServerMessage(sim, STR_NAME_RENAMED_BY_VERIFIED, 2, renameArgs);
    }

    {
        char consoleMsg[256];
        snprintf(consoleMsg, sizeof(consoleMsg),
                 "Player '%s' renamed to '%s' (verified player '%s' joined)",
                 originalName, chosenName, incomingName);
        serverSimConsoleMessage(consoleMsg);
    }
}

/* Choose a unique "<baseName>-unverified[-N]" name, skipping index 1
 * (the bare "-unverified" form IS the "1").  The candidate must not
 * collide with any connected slot other than excludeSlot.  Returns true
 * and writes the chosen name into out (capacity outLen) on success;
 * returns false when the suffix pool (indices 0, 2..99) is exhausted. */
static bool serverChooseUnverifiedSuffix(const char *baseName,
                                         int excludeSlot,
                                         char *out, size_t outLen) {
    int suffixIdx;
    /* Try indices 0, 2, 3, ..., 99 (1 is reserved — the bare
     * "-unverified" form IS the "1"). */
    for (suffixIdx = 0; suffixIdx <= 99; suffixIdx++) {
        if (suffixIdx == 1) continue;
        char candidate[PACKET_MAX_PLAYER_NAME];
        if (!playerNameMakeUnverifiedSuffix(baseName, suffixIdx,
                                            candidate,
                                            sizeof(candidate))) {
            continue;
        }

        /* Candidate must be unique against ALL other connected
         * slots (not just the excluded slot). */
        bool clash = false;
        int k;
        for (k = 0; k < MAX_TANKS; k++) {
            if (!udpServer.clients[k].connected) continue;
            if (k == excludeSlot) continue;
            if (playerNameCompare(udpServer.clients[k].playerName,
                                  candidate) == 0) {
                clash = true;
                break;
            }
        }
        if (clash) continue;

        strncpy(out, candidate, outLen - 1);
        out[outLen - 1] = '\0';
        return true;
    }
    return false;
}

/* Handle a join request from a new client */
static void serverHandleJoinRequest(const uint8_t *buf, int len,
                                    const struct sockaddr_in *fromAddr,
                                    ServerSim *sim) {
    int pos = PACKET_HEADER_SIZE;
    char name[PACKET_MAX_PLAYER_NAME];
    char pass[MAP_STR_SIZE];
    char wbnJoinKey[WBN_JOIN_KEY_WIRE_LEN];
    int slot;

    WB_LOG_DEBUG(WB_LOG_CAT_NET,
        "join request from %s:%u len=%d",
        inet_ntoa(fromAddr->sin_addr),
        (unsigned)ntohs(fromAddr->sin_port), len);
#ifndef WB_FUZZ
    fprintf(stderr, "[UDP SERVER] Join request received, len=%d\n", len);
#endif
    /* Full JOIN_REQUEST payload after header: name + pass + 3 version bytes
     * + WBN token + 1 flags byte + 2 client-identity bytes (clientType,
     * clientHints).  No backward-compat path — older clients are rejected. */
    int joinReqMin = pos + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3
                     + WBN_JOIN_KEY_WIRE_LEN + 1 + 2;
    if (len < joinReqMin) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "join request malformed: need=%d got=%d from=%s:%u",
            joinReqMin, len,
            inet_ntoa(fromAddr->sin_addr),
            (unsigned)ntohs(fromAddr->sin_port));
#ifndef WB_FUZZ
        fprintf(stderr, "[UDP SERVER] Join request malformed (need %d, got %d)\n",
                joinReqMin, len);
#endif
        return; /* Malformed */
    }

    /* Already connected from this address? */
    if (serverFindClient(fromAddr) >= 0) {
        /* Resend accept in case they missed it */
        slot = serverFindClient(fromAddr);
        WB_LOG_DEBUG(WB_LOG_CAT_NET,
            "join from already-connected slot=%d, resending accept", slot);
        serverSendJoinAccept(slot, sim, fromAddr);
        /* Resend the rekey alongside the accept — recovers the rare case
         * where the original JOIN_ACCEPT was delivered but the trailing
         * REKEY wasn't, which would otherwise leave the client without
         * a wbnServerKey until the next return-to-lobby rotation. */
        transportUdpServerSendWbnRekey(&udpServer.clients[slot]);
        /* No map re-poke needed: an incomplete download is still armed/in-flight
         * on CHANNEL_BULK and the channel retransmits its own unacked segments. */
        return;
    }

    memcpy(name, buf + pos, PACKET_MAX_PLAYER_NAME);
    name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    pos += PACKET_MAX_PLAYER_NAME;

    /* Validate the requested name before any other check.  The validator
     * returns the NFC-normalized, stripped, codepoint-safe-truncated form
     * which then becomes the canonical name we accept and store. */
    {
        char validated[PACKET_MAX_PLAYER_NAME];
        PlayerNameValidationError nameErr = PLAYER_NAME_OK;
        if (!playerNameValidate(name, validated, sizeof(validated), &nameErr)) {
            char consoleMsg[160];
            snprintf(consoleMsg, sizeof(consoleMsg),
                     "Join rejected for '%s': Invalid player name (code %d)",
                     name, (int)nameErr);
            serverSimConsoleMessage(consoleMsg);
            serverSendJoinReject(fromAddr, STR_REJECT_INVALID_PLAYER_NAME, 0, NULL);
            return;
        }
        strncpy(name, validated, PACKET_MAX_PLAYER_NAME - 1);
        name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    }

    memcpy(pass, buf + pos, MAP_STR_SIZE);
    pass[MAP_STR_SIZE - 1] = '\0';
    pos += MAP_STR_SIZE;

    /* Version gate: require an exact protocol-version triple match against
     * the version this server was compiled with.  A self-built or stale
     * client sending a different triple is rejected here — before the
     * password check — so it gets the version error rather than a
     * misleading password failure.  The three bytes keep their fixed wire
     * offset so even an old client's JOIN stays parseable for rejection. */
    {
        uint8_t cliMajor = buf[pos];
        uint8_t cliMinor = buf[pos + 1];
        uint8_t cliRev   = buf[pos + 2];
        pos += 3;
        if (cliMajor != BOLO_VERSION_MAJOR ||
            cliMinor != BOLO_VERSION_MINOR ||
            cliRev   != BOLO_VERSION_REVISION) {
            char serverVer[16];
            char clientVer[16];
            char consoleMsg[160];
            const char *args[4];
            snprintf(serverVer, sizeof(serverVer), "%u.%u.%u",
                     (unsigned)BOLO_VERSION_MAJOR,
                     (unsigned)BOLO_VERSION_MINOR,
                     (unsigned)BOLO_VERSION_REVISION);
            snprintf(clientVer, sizeof(clientVer), "%u.%u.%u",
                     (unsigned)cliMajor, (unsigned)cliMinor, (unsigned)cliRev);
            snprintf(consoleMsg, sizeof(consoleMsg),
                     "Join rejected for '%s': Version mismatch "
                     "(server %s, client %s)", name, serverVer, clientVer);
            serverSimConsoleMessage(consoleMsg);
            /* The 1389 string renders {string1}=server, {string2}=client.
             * The client decode fills string1/string2 from arg slots 2/3
             * (slots 0/1 are the player/other name, unused here), so pass
             * the two versions in slots 2 and 3 with empty leading args. */
            args[0] = "";
            args[1] = "";
            args[2] = serverVer;
            args[3] = clientVer;
            serverSendJoinReject(fromAddr, STR_REJECT_VERSION_MISMATCH, 4, args);
            return;
        }
    }

    /* Read WBN token if present (backwards compatible — older clients won't send it) */
    memset(wbnJoinKey, 0, WBN_JOIN_KEY_WIRE_LEN);
    if (len >= pos + WBN_JOIN_KEY_WIRE_LEN) {
        memcpy(wbnJoinKey, buf + pos, WBN_JOIN_KEY_WIRE_LEN);
        wbnJoinKey[WBN_JOIN_KEY_WIRE_LEN - 1] = '\0';
        pos += WBN_JOIN_KEY_WIRE_LEN;
    }

    /* Read flags byte if present (backwards compatible — older clients default to 0) */
    bool wantRejoin = false;
    bool incomingWillAuth = false;
    bool isSpectator = false;
    if (len > pos) {
        wantRejoin       = (buf[pos] & JOIN_FLAG_WANT_REJOIN) != 0;
        incomingWillAuth = (buf[pos] & JOIN_FLAG_WILL_AUTHENTICATE) != 0;
        isSpectator      = (buf[pos] & JOIN_FLAG_SPECTATOR) != 0;
        pos++;
    }

    /* Read clientType + clientHints (length already gated above). */
    uint8_t clientType  = buf[pos++];
    uint8_t clientHints = buf[pos++];
    if (clientType >= CLIENT_TYPE_COUNT) clientType = CLIENT_TYPE_UNKNOWN;
    /* Drop reserved/server-only bits — clients are never trusted to set
     * WBN_VERIFIED or WBN_STEAM_LINKED. */
    clientHints &= PLAYER_CLIENT_HINT_MASK;

    /* Optional trailing fallbackCountry (2 bytes). Old clients won't
     * send it — leave empty in that case. Used below as the GeoIP-failed
     * fallback so loopback and private-LAN joiners can supply their own
     * cached country code without the server reading its own WBN cache. */
    char wireFallbackCountry[3];
    wireFallbackCountry[0] = '\0';
    wireFallbackCountry[1] = '\0';
    wireFallbackCountry[2] = '\0';
    if (len >= pos + 2) {
        wireFallbackCountry[0] = (char)buf[pos++];
        wireFallbackCountry[1] = (char)buf[pos++];
    }

    /* Optional trailing address-proof cookie. Old/initial JOINs omit it
     * (NULL) and draw a challenge below; a returning JOIN echoes the 16
     * bytes from a prior PACKET_JOIN_CHALLENGE. */
    const uint8_t *joinCookie = NULL;
    if (len >= pos + JOIN_COOKIE_LEN) {
        joinCookie = buf + pos;
        pos += JOIN_COOKIE_LEN;
    }

    /* Address proof, gated before the password / game-lock / full checks: a
     * slot is allocated only after the joiner echoes a valid retry cookie, and
     * the password and game-locked rejects below run only for a proven address
     * so they can't be reflected to a spoofed source. The version reject
     * earlier is intentionally left ahead of this gate — an old, cookie-
     * incapable client must get a clean version reject rather than a silent
     * timeout (that residual reflection is de-amplifying — the version reject
     * precedes this gate, so it is not itself rate-limited).
     *
     * The cookie — not the rate limiter — is what prevents slot exhaustion, so
     * an unproven JOIN (no/stale cookie) is the only thing the per-source-IP
     * rate limit guards: it bounds the cheap challenge/reflection path. A
     * proven (valid-cookie) JOIN is never rate-limited, so many legitimate
     * clients behind one NAT or public IP still join promptly. Drop an
     * over-rate unproven JOIN silently — a reply would reflect to a
     * possibly-spoofed source — otherwise issue a fresh challenge (smaller
     * than the JOIN, so it can't amplify). */
    if (!serverCookieAccept(fromAddr, joinCookie)) {
        if (!serverJoinRateLimitAllow(fromAddr)) {
            WB_LOG_DEBUG(WB_LOG_CAT_NET,
                "join rate-limited from %s:%u",
                inet_ntoa(fromAddr->sin_addr),
                (unsigned)ntohs(fromAddr->sin_port));
            return;
        }
        serverSendJoinChallenge(fromAddr);
        return;
    }

    /* Check password */
    {
        const char *expected = serverSimGetPassword(sim);
        if (expected[0] != '\0' && strcmp(pass, expected) != 0) {
            char consoleMsg[128];
            snprintf(consoleMsg, sizeof(consoleMsg),
                     "Join rejected for '%s': Incorrect password", name);
            serverSimConsoleMessage(consoleMsg);
            serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0, NULL);
            return;
        }
    }

    /* Join as viewer: no tank slot, no sim player, no control-bus
     * subscription.  All shared pre-checks above (cookie, version, name,
     * password) have run; the player game-lock below does not gate
     * spectating — a locked or running game stays watchable. */
    if (isSpectator) {
        serverAcceptSpectator(sim, fromAddr, name, clientType, clientHints);
        return;
    }

    /* Check game lock: server admin command OR host toggled
     * "Allow New Players: Now" off (mirrored as !sim->allowNewPlayers). */
    if (udpServer.gameLocked || !serverSimIsAcceptingJoins(sim)) {
        char consoleMsg[128];
        snprintf(consoleMsg, sizeof(consoleMsg),
                 "Join rejected for '%s': Game is locked", name);
        serverSimConsoleMessage(consoleMsg);
        serverSendJoinReject(fromAddr, STR_REJECT_GAME_LOCKED, 0, NULL);
        return;
    }

    /* Find free slot for the incoming player.  Slot allocation moved
     * ahead of the duplicate check (Phase 5) so the WBN-verification
     * step below has a slot to bind its player_key to, and the collision
     * policy has the slot available before applying any preempt. */
    slot = serverSimFindFreeSlot(sim);
    if (slot < 0) {
        serverSimConsoleMessage("Join rejected: Server full");
        serverSendJoinReject(fromAddr, STR_REJECT_SERVER_FULL, 0, NULL);
        return;
    }

    /* Clear any stale provisional-claim state before the duplicate-search
     * loop (which sets it for a will-auth provisional admit).  A slot freed
     * by the map-serialize failure path below leaves connected=false without
     * routing through serverDisconnectClient, so the claim would otherwise
     * persist and mis-route the next reuser's reauth. */
    udpServer.clients[slot].claimPending = false;
    udpServer.clients[slot].claimDesiredName[0] = '\0';

    /* Country resolution for the incoming player.  Done early so the
     * preempt path can include it in the rename newswire. Uniform
     * across loopback / private-LAN / public-WAN joiners: GeoIP first,
     * then the client-supplied fallbackCountry if GeoIP can't resolve
     * the address. The host self-join over loopback supplies its own
     * cached WBN country via clientSimConnectUdp; private-LAN joiners
     * supply whatever they cached. Empty stays empty for non-WBN
     * old clients. */
    char incomingCountry[3];
    {
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &fromAddr->sin_addr, ipStr, sizeof(ipStr));
        if (!geoLookupCountry(ipStr, incomingCountry)) {
            incomingCountry[0] = wireFallbackCountry[0];
            incomingCountry[1] = wireFallbackCountry[1];
        }
        incomingCountry[2] = '\0';
    }

    /* Verify WBN token if provided. Verification failure is no longer
     * fatal — the client simply joins as a non-WBN player and forfeits
     * WBN-mediated features (Balance from WBN, ranked credit, ladder
     * placement). This keeps a game playable when winbolo.net is
     * unreachable or returns transient errors, instead of locking
     * everyone out of the lobby. The collision policy below already
     * treats !incomingIsWBN as the lower-priority class. */
    bool incomingIsWBN = false;
    bool wbnHasSteam = false;
    bool wbnIsSupporter = false;
    if (wbnJoinKey[0] != '\0' && winbolonetIsRunning()) {
        char errorMsg[512];
        errorMsg[0] = '\0';
        if (winboloNetVerifyClientKey(wbnJoinKey, name, (BYTE)slot, errorMsg,
                                      &wbnHasSteam, &wbnIsSupporter)) {
            fprintf(stderr, "[UDP SERVER] Player '%s' verified with WinBolo.net\n", name);
            incomingIsWBN = true;
        } else {
            /* Degrade to non-WBN join instead of rejecting outright.
             * The original "WinBolo.net verification failed: <reason>"
             * message is extended with "Proceeding without WBN.net
             * features" so the host's console explains both halves
             * (what broke, and that the lobby keeps running anyway). */
            char failMsg[256];
            snprintf(failMsg, sizeof(failMsg),
                     "WinBolo.net verification failed: %s. Proceeding "
                     "without WBN.net features.", errorMsg);
            fprintf(stderr, "[UDP SERVER] %s (player='%s')\n", failMsg, name);
            serverSimConsoleMessage(failMsg);
            incomingIsWBN = false;
        }
    }

    /* Verified-priority collision policy (Phase 5).  Replaces the older
     * single-rule "Name already in use" check.  Loop terminates on the
     * first match (existing duplicate-loop semantics). */
    {
        int i;
        for (i = 0; i < MAX_TANKS; i++) {
            if (!udpServer.clients[i].connected) continue;
            if (i == slot) continue; /* slot is unconnected; defensive */
            if (playerNameCompare(udpServer.clients[i].playerName, name) != 0)
                continue;

            bool existingIsWBN =
                (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, (BYTE)i)
                 & PLAYER_FLAG_WBN_VERIFIED) != 0;

            /* An already-verified joiner (incomingIsWBN) takes the inline
             * verified-priority arms below.  These are dead under today's
             * handshake — the client never holds a server key at join — but
             * if incomingIsWBN is ever true the joiner must take the
             * immediate-preempt path: routing a verified slot to provisional
             * admission would strand it on a temp name forever, since a
             * verified slot never sends a reauth. */
            if (incomingIsWBN) {
                if (existingIsWBN) {
                    /* Two verified users with the same display name — Decision 7
                     * says reject the second joiner rather than preempt. */
                    char consoleMsg[200];
                    snprintf(consoleMsg, sizeof(consoleMsg),
                             "WARNING: verified-vs-verified collision for '%s'; "
                             "rejecting joiner", name);
                    serverSimConsoleMessage(consoleMsg);
                    /* Roll back the WBN client/join we recorded above. */
                    winboloNetClientLeaveGame(
                        (BYTE)slot, serverSimGetNumPlayers(sim),
                        serverSimGetNumNeutralBases(sim),
                        serverSimGetNumNeutralPills(sim));
                    serverSendJoinReject(fromAddr,
                                         STR_NAME_TAKEN_BY_OTHER_VERIFIED, 0, NULL);
                    return;
                }

                /* incomingIsWBN && !existingIsWBN — preempt. */

                if (udpServer.clients[i].nameStickySuffix) {
                    /* In practice unreachable: a sticky slot already stores
                     * "<base>-unverified[-N]" so playerNameCompare wouldn't
                     * have matched the bare incoming name.  Guard defensively
                     * — never re-preempt a slot that has already been
                     * suffix-renamed. */
                    continue;
                }

                /* Find a unique -unverified[-N] candidate for the victim. */
                char chosenName[PACKET_MAX_PLAYER_NAME];
                bool chosenFound = serverChooseUnverifiedSuffix(
                    udpServer.clients[i].playerName, i,
                    chosenName, sizeof(chosenName));

                if (!chosenFound) {
                    /* Suffix pool exhausted.  Never preempt a verified
                     * player even transitively (Decision 4 implication);
                     * also never preempt twice — reject the verified joiner
                     * instead. */
                    char consoleMsg[200];
                    snprintf(consoleMsg, sizeof(consoleMsg),
                             "WARNING: -unverified suffix pool exhausted for "
                             "'%s'; rejecting verified joiner", name);
                    serverSimConsoleMessage(consoleMsg);
                    winboloNetClientLeaveGame(
                        (BYTE)slot, serverSimGetNumPlayers(sim),
                        serverSimGetNumNeutralBases(sim),
                        serverSimGetNumNeutralPills(sim));
                    serverSendJoinReject(fromAddr,
                                         STR_REJECT_NAME_POOL_EXHAUSTED, 0, NULL);
                    return;
                }

                /* Apply the rename + broadcasts + newswire. */
                serverPreemptRename(sim, i, chosenName, name, incomingCountry);
                break; /* terminate the duplicate-search loop on first match */
            }

            /* !incomingIsWBN — consult the pure verdict core. */
            JoinCollisionVerdict verdict =
                joinCollisionDecide(incomingWillAuth, existingIsWBN);

            if (verdict == JOIN_COLLISION_REJECT_VERIFIED) {
                /* Unverified joiner can't take a verified player's name. */
                char consoleMsg[160];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "Join rejected for '%s': name taken by verified player",
                         name);
                serverSimConsoleMessage(consoleMsg);
                serverSendJoinReject(fromAddr, STR_NAME_TAKEN_BY_VERIFIED, 0, NULL);
                return;
            }
            if (verdict == JOIN_COLLISION_REJECT_IN_USE) {
                /* Both unverified (incl. Steam, bot): existing behavior. */
                char consoleMsg[128];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "Join rejected for '%s': Name already in use", name);
                serverSimConsoleMessage(consoleMsg);
                serverSendJoinReject(fromAddr, STR_DLGSETNAME_INUSE_ERR, 0, NULL);
                return;
            }

            /* JOIN_COLLISION_ADMIT_PROVISIONAL — a will-authenticate joiner
             * whose desired bare name is held by an unverified squatter.
             * Seat it under a temporary -unverified[-N] name keyed off the
             * squatter slot and record the pending claim so reauth can
             * promote it to the bare name. */
            char tempName[PACKET_MAX_PLAYER_NAME];
            if (!serverChooseUnverifiedSuffix(udpServer.clients[i].playerName, i,
                                              tempName, sizeof(tempName))) {
                /* Suffix pool exhausted — reject as at the join-time preempt.
                 * This joiner is !incomingIsWBN, so there is no WBN-recorded
                 * join to roll back. */
                char consoleMsg[200];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "WARNING: -unverified suffix pool exhausted for "
                         "'%s'; rejecting provisional joiner", name);
                serverSimConsoleMessage(consoleMsg);
                serverSendJoinReject(fromAddr,
                                     STR_REJECT_NAME_POOL_EXHAUSTED, 0, NULL);
                return;
            }

            /* Record the pending claim on the joiner's slot (not the
             * squatter's): the desired bare name is the validated incoming
             * name, resolved at reauth. */
            udpServer.clients[slot].claimPending = true;
            snprintf(udpServer.clients[slot].claimDesiredName,
                     sizeof(udpServer.clients[slot].claimDesiredName), "%s", name);

            /* Seat the provisional joiner under the temp name; the bare name is
             * recorded above and claimed at reauth.  Overwriting the local name
             * makes all downstream seating use the temp name. */
            snprintf(name, sizeof(name), "%s", tempName);
            break; /* terminate the duplicate-search loop on first match */
        }
    }

    /* Accept the player */
    udpServer.clients[slot].connected = true;
    udpServer.clients[slot].connId = serverNextConnId();
    udpServer.clients[slot].nameStickySuffix = false;
    udpServer.clients[slot].addr = *fromAddr;
    udpServer.clients[slot].playerNum = (uint8_t)slot;
    snprintf(udpServer.clients[slot].playerName,
             PACKET_MAX_PLAYER_NAME, "%s", name);
    udpServer.clients[slot].lastReceivedTick = udpServer.tickCount;
    udpServer.clients[slot].outSequence = 1;
    udpServer.clients[slot].inboundCmdSeq = 0;
    udpServer.clients[slot].lastPingTime = udpServer.tickCount;
    udpServer.clients[slot].pingMs = 0;
    udpServer.clients[slot].wantRejoin = wantRejoin;

    /* Persist the early GeoIP lookup result. */
    udpServer.clients[slot].countryCode[0] = incomingCountry[0];
    udpServer.clients[slot].countryCode[1] = incomingCountry[1];
    udpServer.clients[slot].countryCode[2] = '\0';
    udpServer.clients[slot].clientType  = clientType;
    udpServer.clients[slot].clientHints = clientHints;

    /* Initialize the reliable map event queue for this client */
    udpServer.mapEventQueues[slot].nextSeq = 1;
    udpServer.mapEventQueues[slot].ackedSeq = 1;
    memset(udpServer.mapEventQueues[slot].buffer, 0, sizeof(udpServer.mapEventQueues[slot].buffer));
    udpServer.mapGen[slot] = 0;

    /* Bring up this slot's parallel channel mux alongside the queues. */
    channelMuxInit(&udpServer.channelMux[slot]);
    udpServer.channelFramesRx[slot] = 0;
    bulkSenderInit(&udpServer.bulkSend[slot]);
    bulkReceiverInit(&udpServer.bulkRecvUp[slot]);

    /* Merge client-supplied hints with server-determined WBN trust into a
     * single clientFlags byte, then run the four-step join sequence so a
     * single CTRL_PLAYER_JOIN fans out with name, country, clientType,
     * and clientFlags all populated. */
    {
        uint8_t flags = clientHints & PLAYER_CLIENT_HINT_MASK;
        if (incomingIsWBN)                  flags |= PLAYER_FLAG_WBN_VERIFIED;
        if (incomingIsWBN && wbnHasSteam)   flags |= PLAYER_FLAG_WBN_STEAM_LINKED;
        if (incomingIsWBN && wbnIsSupporter) flags |= PLAYER_FLAG_SUPPORTER;
        /* Durable cross-round signal for the rekey-rotation gate: set it
         * definitively here (true for a WBN joiner, false otherwise) so a
         * non-WBN client reusing a slot can't inherit a stale true. */
        udpServer.clients[slot].wbnWasVerified = incomingIsWBN;
        addPlayerInternal(sim, (BYTE)slot,
                          udpServer.clients[slot].playerName,
                          udpServer.clients[slot].wantRejoin);
        setPlayerCountryInternal(sim, (BYTE)slot,
                                 udpServer.clients[slot].countryCode);
        setClientTypeFlagsInternal(sim, (BYTE)slot, clientType, flags);
        fillAndPublishPlayerJoin(sim, (BYTE)slot);
    }
    WB_LOG_INFO(WB_LOG_CAT_NET,
                "join accept: slot=%d clientType=%u clientHints=0x%02x",
                slot, (unsigned)clientType, (unsigned)clientHints);

    /* Compress current map state for the joining player.
     * Done after serverSimAddPlayer so rejoin ownership is included. */
    {
        int mapLen = serverSimGetCompressedMap(sim, udpServer.compressedMap);
        if (mapLen <= 0) {
            serverSimRemovePlayer(sim, (BYTE)slot);
            udpServer.clients[slot].connected = false;
            serverSendJoinReject(fromAddr, NETERR_MAPSERIALIZE, 0, NULL);
            return;
        }
        udpServer.compressedMapSize = (uint32_t)mapLen;
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
    /* Hand the joiner the current WBN server_key so it can mint a
     * player_key and re-auth via the lobby-snapshot path.  Gated inside
     * the send function — no-op on non-WBN servers. */
    transportUdpServerSendWbnRekey(&udpServer.clients[slot]);

    /* Subscribe this client to the server's control-event bus so
     * future events can be encoded and unicast to it via the codec
     * table.  Register's sync-replay walks the lobby settings, every
     * connected slot, and every player-join and feeds them through
     * the codec encoder to this client's socket — replacing the old
     * composite PACKET_LOBBY_STATE handshake.
     *
     * The replay burst is queued onto CHANNEL_CONTROL by udpClientDeliverControl
     * and carried by the channel's own framing — the snapshot trailer during
     * running, a standalone PACKET_CHANNEL otherwise — so no explicit post-burst
     * flush is needed.  controlSyncInProgress is still bracketed here for the
     * dormant queue machinery; the channel ignores it. */
    mpDiagLog("[srv] SYNC START slot=%d phase=%d (about to register subscriber + replay)",
              slot, (int)serverSimGetState(sim));
    udpServer.controlSyncInProgress[slot] = true;
    udpServer.clients[slot].controlSub =
        serverSimRegisterSubscriber(sim, udpClientDeliverControl,
                                    &udpServer.clients[slot]);
    udpServer.controlSyncInProgress[slot] = false;
    mpDiagLog("[srv] SYNC END slot=%d phase=%d (replay queued onto CHANNEL_CONTROL)",
              slot, (int)serverSimGetState(sim));
    /* Flush the coalesced replay burst now when outside running (mirrors the
     * per-event eager flush the sync guard suppressed); during running the
     * next snapshot trailer carries it. */
    if (serverSimGetState(sim) != serverStateRunning) {
        transportUdpServerFlushChannel(slot);
    }

    /* Announce the join to WBN.  If the slot's key already rode the JOIN
     * field and verified inline (incomingIsWBN), the player is already a
     * participant — register keyed right now.  Otherwise, when WBN is
     * running, a will-authenticate joiner defers: the rekey we sent above
     * prompts a reauth that fills the key and fires a keyed join
     * (transportUdpServerHandleWbnReauth); if no reauth lands within the
     * grace window the per-tick sweep in transportUdpServerCheckTimeouts
     * fires an anonymous one.  A not-signed-in joiner owes no reauth, so
     * its anonymous join fires now.  On a non-WBN server there is nothing
     * to defer (and winbolonetAddEvent is a no-op anyway). */
    wbnJoinClear(&udpServer.clients[slot].wbnJoin);
    if (incomingIsWBN) {
        winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                           (BYTE)slot, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
    } else if (winbolonetIsRunning()) {
        if (incomingWillAuth) {
            /* Signed-in joiner: a reauth is coming.  Arm the anonymous
             * fallback so a never-landing reauth still announces the join. */
            wbnJoinArm(&udpServer.clients[slot].wbnJoin,
                       udpServer.tickCount, WBN_JOIN_REGISTER_GRACE_TICKS);
        } else {
            /* Not signed in: no reauth will ever land, so there is nothing
             * to wait for — announce the anonymous join now. */
            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                               (BYTE)slot, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
        }
    }

    if (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown) {
        /* Publish a lobby-slot update for the new player so existing
         * clients pick up the joiner's team/ready/ping fields (the
         * CTRL_PLAYER_JOIN already fanned out from serverSimAddPlayer
         * carries name/country/clientType, but not the lobby-slot
         * extras). */
        serverSimPublishLobbySlot(sim, (BYTE)slot);
        /* Dismiss any pending balance proposal — player composition changed */
        if (serverSimGetBalanceProposal(sim)->pending) {
            ControlEvent evt;
            serverSimClearBalanceProposal(sim);
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_BALANCE_PROPOSAL;
            serverSimPublishControl(sim, &evt);
        }
    }
    /* No-lobby and mid-game-join PACKET_GAME_START is emitted by the
     * subscriber's sync replay: CTRL_GAME_PHASE_RUNNING flows through
     * the codec encoder to this client's socket as part of
     * serverSimRegisterSubscriber above. */

    /* Initialize map download. The blob is not streamed here: it is armed and
     * begins on CHANNEL_BULK once the channel is idle, carried by the per-tick
     * frame in transportUdpServerSend. No anti-reflection round-trip gates it —
     * the join cookie (serverCookieAccept) already proved this address can
     * receive a reply, so a spoofed JOIN never reaches a slot to be exploited. */
    serverInitMapDownload(slot);

    /* The sync-replay just enqueued a CTRL_PLAYER_JOIN for every in-use
     * player into this client's controlEventQueue, so the JOIN-time
     * roster is covered by the reliable bus path. */

    /* Surface the join in everyone's lobby chat and unready any humans
     * who were ready. The chat line rides CTRL_SERVER_TEXT, which the
     * bus fans to both in-process subscribers and UDP clients via the
     * codec — same path serverSendServerEnglishBroadcast already uses
     * for lock-toggle / ping-enforcement announcements. The unready
     * call is a no-op outside lobby/countdown (no human is ready in
     * running state), so it stays unconditional. */
    {
        char chatMsg[32 + PACKET_MAX_PLAYER_NAME];
        snprintf(chatMsg, sizeof(chatMsg), "%s has joined.",
                 udpServer.clients[slot].playerName);
        serverSendServerEnglishBroadcast(sim, chatMsg);
    }
    lobbyAutoUnreadyOnChange(sim);
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

    if (previewed && udpServer.uploadPolicy == UPLOAD_POLICY_PERSIST) {
        memcpy(udpServer.pendingPersistBytes,
               udpServer.clientUploadBuf[clientIdx], total);
        udpServer.pendingPersistLen = total;
        SDL_strlcpy(udpServer.pendingPersistName, displayName,
                    sizeof(udpServer.pendingPersistName));
        udpServer.pendingPersistActive = true;
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
static void serverDrainBulk(ServerSim *sim, int clientIdx) {
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
    /* An actual disconnect supersedes any deferred overflow-disconnect for
     * this slot (see the queue-overflow branch in udpClientDeliverControl):
     * clear the flag so the drain can't later tear down a fresh occupant that
     * reused the slot after this teardown frees it. */
    udpServer.pendingSimRemove[idx] = false;

    {
        UdpServerClient *c = &udpServer.clients[idx];
        (void)c;
        WB_LOG_DEBUG(WB_LOG_CAT_NET,
            "disconnect slot=%d name='%s' addr=%s:%u graceful=%d "
            "tickCount=%u lastReceivedTick=%u tickDiff=%u (timeout=%d)",
            idx, c->playerName,
            inet_ntoa(c->addr.sin_addr), (unsigned)ntohs(c->addr.sin_port),
            (int)graceful,
            (unsigned)udpServer.tickCount,
            (unsigned)c->lastReceivedTick,
            (unsigned)(udpServer.tickCount - c->lastReceivedTick),
            (int)CLIENT_TIMEOUT_TICKS);
        mpDiagLog("[srv] DISCONNECT slot=%d name='%s' graceful=%d tickDiff=%u (timeout=%d)",
                  idx, c->playerName, (int)graceful,
                  (unsigned)(udpServer.tickCount - c->lastReceivedTick),
                  (int)CLIENT_TIMEOUT_TICKS);
    }

    if (graceful) {
        snprintf(msg, sizeof(msg), "%s is quitting.",
                 udpServer.clients[idx].playerName);
        winbolonetAddEvent(WINBOLO_NET_EVENT_QUITTING, TRUE,
                           (BYTE)idx, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
    } else {
        snprintf(msg, sizeof(msg), "%s timed out.",
                 udpServer.clients[idx].playerName);
    }
    WB_LOG_INFO(WB_LOG_CAT_NET, "%s", msg);
    fprintf(stderr, "[UDP SERVER] %s\n", msg);
    serverSimConsoleMessage(msg);

    /* Mirror the leave into every client's lobby chat panel via
     * CTRL_SERVER_TEXT. Console keeps the graceful-vs-timeout detail
     * (`msg` above); the chat line is the uniform "X has left." form
     * — players don't need the distinction and it matches what the
     * client-side wire-packet branch used to render. Done before the
     * subscriber/slot teardown below so the leaving client's
     * still-attached subscriber sees it if they're reachable, and
     * the formatted name is still in udpServer.clients[idx].playerName
     * (wiped below). */
    {
        char chatMsg[32 + PACKET_MAX_PLAYER_NAME];
        snprintf(chatMsg, sizeof(chatMsg), "%s has left.",
                 udpServer.clients[idx].playerName);
        /* Flip connected=false BEFORE the broadcast. Without this, the
         * "X has left." event re-enters udpClientDeliverControl for this
         * same slot; if the leaving slot's queue is what overflowed in
         * the first place (the path that brought us into this function
         * via the queue-overflow branch), the enqueue fails again and
         * recurses back into serverDisconnectClient — unbounded
         * recursion until the timer thread's stack blows. The deliver
         * callback's existing !connected short-circuit makes the
         * broadcast a no-op for this slot; the other slots still see
         * "X has left." normally. */
        udpServer.clients[idx].connected = false;
        serverSendServerEnglishBroadcast(sim, chatMsg);
    }

    /* Notify WinBolo.net that the player is leaving (must happen before
     * clearing the slot so the player key is still valid) */
    winboloNetClientLeaveGame((BYTE)idx,
                              serverSimGetNumPlayers(sim),
                              serverSimGetNumNeutralBases(sim),
                              serverSimGetNumNeutralPills(sim));

    /* PACKET_PLAYER_LEFT is fanned out by the codec when the outer caller
     * invokes serverSimRemovePlayer; no hand-built broadcast here. */
    serverSimUnregisterSubscriber(sim, udpServer.clients[idx].controlSub);
    udpServer.clients[idx].controlSub = SUBSCRIBER_HANDLE_INVALID;
    udpServer.clients[idx].nameStickySuffix = false;
    udpServer.clients[idx].claimPending = false;
    udpServer.clients[idx].claimDesiredName[0] = '\0';
    udpServer.clients[idx].inboundCmdSeq = 0;
    memset(udpServer.clients[idx].playerName, 0, PACKET_MAX_PLAYER_NAME);
    udpServer.clientLocked[idx] = false;
    /* Drop any owed PLAYER_JOIN — the player left before it resolved, so
     * no orphan anonymous join (and no leave it would need to pair with). */
    wbnJoinClear(&udpServer.clients[idx].wbnJoin);
    /* Slot is free; a fresh occupant re-establishes WBN status at its join. */
    udpServer.clients[idx].wbnWasVerified = false;

    /* Reset control-sync state so a re-using slot starts fresh. */
    udpServer.controlSyncInProgress[idx] = false;

    /* Reset the channel mux so a re-using slot starts fresh. */
    channelMuxInit(&udpServer.channelMux[idx]);
    udpServer.channelFramesRx[idx] = 0;
    udpServer.mapGen[idx] = 0;
    bulkSenderReset(&udpServer.bulkSend[idx]);
    bulkReceiverInit(&udpServer.bulkRecvUp[idx]);

    /* Release any in-flight upload state. Without this, a client
     * who drops mid-upload would leave clientUploadActive set,
     * blocking every subsequent UPLOAD_BEGIN from a different
     * client with LOBBY_REJECT_UPLOAD_BUSY until server restart. */
    udpServerClearClientUploadState(idx);

    /* Unready any humans who were ready — a leaver changes the lobby
     * composition. A no-op in running state (no one is ready then).
     * Outer callers also fire serverSimRemovePlayer immediately after
     * this; running this here rather than in each caller keeps the
     * leave-side hook centralized alongside the chat broadcast above. */
    lobbyAutoUnreadyOnChange(sim);
}

void transportUdpServerDrainPendingRemovals(ServerSim *sim) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.pendingSimRemove[i]) continue;
        udpServer.pendingSimRemove[i] = false;
        /* The slot was flagged on a control-queue overflow detected inside a
         * publish (udpClientDeliverControl); the whole disconnect was deferred
         * to here so its leave broadcast + PLAYER_LEFT fan-out run outside any
         * publish. If the slot is no longer connected, another path already
         * disconnected it (and cleared this flag before any reuse), so there is
         * nothing to tear down. */
        if (!udpServer.clients[i].connected) continue;
        serverDisconnectClient(sim, i, false);
        serverSimRemovePlayer(sim, (BYTE)i);
    }
}

/* Send a localized server-originated message to all connected clients
 * via PACKET_CHAT_BROADCAST.  Uses fromPlayer=0xFF, destPlayer=0xFF to
 * mark the localized variant; client decodes langid + args and renders
 * via langGetTextFmt.  The packed langid+args payload rides as the
 * opaque body[] of CTRL_CHAT; the per-client codec encoder fans it out. */
static void serverSendServerMessage(ServerSim *sim, langid id, int argCount,
                                    const char *const args[]) {
    /* Scratch buffer mirrors the wire encoding so we can reuse
     * packLocalizedPayload; only the bytes after PACKET_HEADER_SIZE+2
     * become the CTRL_CHAT body. */
    uint8_t scratch[PACKET_HEADER_SIZE + 2 + 3 + 4 * (1 + PLAYER_NAME_LEN - 1)];
    int pos = PACKET_HEADER_SIZE + 2;
    ControlEvent evt;

    if (!packLocalizedPayload(scratch, &pos, sizeof(scratch), id, argCount, args)) {
        fprintf(stderr,
                "[UDP SERVER] serverSendServerMessage: pack failed id=%u argc=%d\n",
                (unsigned)id, argCount);
        return;
    }

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 0xFF; /* server localized */
    evt.u.chat.destPlayer = 0xFF; /* broadcast */
    evt.u.chat.bodyLen = (uint16_t)(pos - (PACKET_HEADER_SIZE + 2));
    if (evt.u.chat.bodyLen > 0) {
        memcpy(evt.u.chat.body, scratch + PACKET_HEADER_SIZE + 2,
               evt.u.chat.bodyLen);
    }
    serverSimPublishControl(sim, &evt);
}

/* Send a raw English server-originated message to all connected clients.
 * Uses fromPlayer=0xFE to mark the legacy English variant — used by
 * server-ops broadcasts (admin "say", lock toggle, ping enforcement)
 * that don't yet have dedicated langids.  As individual messages are
 * localized they should migrate to serverSendServerMessage above. */
static void serverSendServerEnglishBroadcast(ServerSim *sim, const char *message) {
    ControlEvent evt;
    size_t maxChars = sizeof(evt.u.serverText.text) - 1; /* PACKET_MAX_CHAT_MESSAGE */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    if (SDL_strlen(message) <= maxChars) {
        SDL_strlcpy(evt.u.serverText.text, message, sizeof(evt.u.serverText.text));
    } else {
        /* CTRL_SERVER_TEXT / PACKET_CHAT_BROADCAST cap the wire payload at
         * PACKET_MAX_CHAT_MESSAGE. Rather than let SDL_strlcpy lop the tail
         * mid-character (a long winners list overflows the cap), cut on a
         * UTF-8 boundary and append an ellipsis so the overflow reads as an
         * intentional truncation. */
        size_t cut = maxChars - 3; /* leave room for "..." */
        while (cut > 0 && ((unsigned char)message[cut] & 0xC0) == 0x80) {
            cut--; /* back up so a multi-byte sequence isn't split */
        }
        SDL_memcpy(evt.u.serverText.text, message, cut);
        SDL_strlcpy(evt.u.serverText.text + cut, "...",
                    sizeof(evt.u.serverText.text) - cut);
    }
    /* In-process subscribers (SP host bots + human) consume CTRL_SERVER_TEXT
     * directly; UDP clients receive the encoder-emitted
     * PACKET_CHAT_BROADCAST(fromPlayer=0xFE) via the codec. */
    serverSimPublishControl(sim, &evt);
}

void transportUdpServerKickPlayer(ServerSim *sim, const char *playerName) {
    int i;
    char msg[128];

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        if (playerNameCompare(udpServer.clients[i].playerName, playerName) == 0) {
            const char *kickArgs[1];
            snprintf(msg, sizeof(msg), "%s has been server kicked.",
                     udpServer.clients[i].playerName);
            WB_LOG_WARN(WB_LOG_CAT_NET, "admin kick slot=%d name='%s'",
                        i, udpServer.clients[i].playerName);
            fprintf(stderr, "[UDP SERVER] %s\n", msg);
            serverSimConsoleMessage(msg);
            /* Send kick message to all clients (including the kicked player) */
            kickArgs[0] = udpServer.clients[i].playerName;
            serverSendServerMessage(sim, STR_KICK_ANNOUNCE, 1, kickArgs);
            /* Hand the kicked client an immediate disconnect notification so
             * they don't sit waiting for the keepalive timeout. PACKET_KICKED
             * transitions the client's joinState to UDP_CLIENT_KICKED, which
             * surfaces a "you were kicked" dialog in the lobby. */
            {
                uint8_t kbuf[PACKET_HEADER_SIZE];
                packHeader(kbuf, PACKET_KICKED, 0);
                srvSendTo(kbuf, sizeof(kbuf), &udpServer.clients[i].addr);
            }
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (serverSimIsLobbyEnabled(sim) &&
                (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown)) {
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
            return;
        }
    }
    serverSimConsoleMessage("Player not found.");
}

void transportUdpServerDisconnectAll(ServerSim *sim) {
    int i;
    bool anyConnected = false;

    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            anyConnected = true;
            break;
        }
    }

    /* Tell every connected client the round is over before tearing their
     * slots down, so they return to the server browser cleanly instead of
     * waiting out the keepalive timeout. The per-client codec subscriber
     * unicasts PACKET_SERVER_SHUTDOWN during this publish (same mechanism
     * transportUdpServerDestroy uses). */
    if (anyConnected) {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_SERVER_SHUTDOWN;
        serverSimPublishControl(sim, &evt);
    }

    /* Boot every human client. serverDisconnectClient handles the
     * transport-side teardown for real UDP clients; serverSimRemovePlayer then
     * clears the sim-side player (gated on the sim's own connected flag, which
     * serverDisconnectClient leaves set). Bots are deliberately kept: they are
     * server configuration, not joined players, so they persist across a map
     * rotation exactly as they do across a normal lobby round (the caller
     * re-arms them for the new round via botManagerOnGameStart). A bot has no
     * udpServer.clients entry, so it is skipped by the connected check; the
     * removal is additionally gated on !serverSimIsBot so it survives. Removal
     * is index-based and doesn't compact the arrays, so a plain forward loop is
     * safe. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
        }
        if (serverSimIsPlayerConnected(sim, (BYTE)i) &&
            !serverSimIsBot(sim, (BYTE)i)) {
            serverSimRemovePlayer(sim, (BYTE)i);
        }
    }
}

bool transportUdpServerSetHostByName(ServerSim *sim, const char *playerName) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        if (serverSimIsBot(sim, (BYTE)i)) continue;
        if (playerNameCompare(udpServer.clients[i].playerName, playerName) == 0) {
            serverSimSetHostSlot(sim, (BYTE)i);
            return true;
        }
    }
    return false;
}

void transportUdpServerEnforcePing(ServerSim *sim) {
    int i;
    if (serverSimGetState(sim) != serverStateRunning) return;

    for (i = 0; i < MAX_TANKS; i++) {
        UdpServerClient *client = &udpServer.clients[i];
        uint16_t ping;
        if (!client->connected) continue;

        ping = client->pingMs;
        if (ping == 0) continue;  /* No measurement yet */
        if (ping == client->lastEnforcedPingMs) continue;  /* Same measurement, already checked */
        client->lastEnforcedPingMs = ping;

        /* Kick threshold */
        if (ping >= PING_KICK_THRESHOLD_MS) {
            client->pingKickStrikes++;
            if (client->pingKickStrikes >= PING_KICK_COUNT) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "%s kicked for high ping (%dms).",
                         client->playerName, ping);
                WB_LOG_WARN(WB_LOG_CAT_NET,
                    "ping-kick slot=%d name='%s' ping=%ums strikes=%d/%d threshold=%d",
                    i, client->playerName, (unsigned)ping,
                    (int)client->pingKickStrikes, (int)PING_KICK_COUNT,
                    (int)PING_KICK_THRESHOLD_MS);
                fprintf(stderr, "[UDP SERVER] %s\n", msg);
                serverSimConsoleMessage(msg);
                serverSendServerEnglishBroadcast(sim, msg);
                serverCleanupMapDownload(i);
                serverDisconnectClient(sim, i, FALSE);
                serverSimRemovePlayer(sim, (BYTE)i);
                continue;
            }
        } else {
            client->pingKickStrikes = 0;
        }

        /* Warn threshold */
        if (ping >= PING_WARN_THRESHOLD_MS) {
            client->pingWarnStrikes++;
            if (client->pingWarnStrikes >= PING_WARN_COUNT && !client->pingWarned) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "%s has high ping (%dms) and may be kicked.",
                         client->playerName, ping);
                serverSendServerEnglishBroadcast(sim, msg);
                client->pingWarned = true;
            }
        } else {
            client->pingWarnStrikes = 0;
            client->pingWarned = false;
        }
    }
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
    }

    netImpairInit(&srvImpairIn);
    netImpairInit(&srvImpairOut);

    /* Start dedicated recv thread */
    SDL_SetAtomicInt(&recvQueueHead, 0);
    SDL_SetAtomicInt(&recvQueueTail, 0);
    recvDropCount = 0;
    recvThreadSock = udpServer.sock;
    SDL_SetAtomicInt(&recvThreadRunning, 1);
    recvThread = SDL_CreateThread(serverRecvThreadFunc, "SrvRecv", NULL);
    if (recvThread) {
        WB_LOG_INFO(WB_LOG_CAT_NET, "recv thread started");
    } else {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "failed to create recv thread, using polled fallback: %s",
            SDL_GetError());
        SDL_SetAtomicInt(&recvThreadRunning, 0);
    }

    return true;
}

void transportUdpServerSetUploadConfig(UploadPolicy policy,
                                       uint8_t maxFiles,
                                       uint32_t maxStorageBytes) {
    udpServer.uploadPolicy = policy;
    if (maxFiles != 0) {
        udpServer.uploadMaxFiles = maxFiles;
    }
    if (maxStorageBytes != 0) {
        udpServer.uploadMaxStorageBytes = maxStorageBytes;
    }
}

void transportUdpServerSetNetImpair(const char *spec) {
#if WB_ENABLE_NETIMPAIR
    NetImpairConfig cfg;
    if (spec == NULL || !netImpairParseConfig(spec, &cfg)) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "netimpair: bad spec '%s' — impairment left off",
            spec ? spec : "(null)");
        return;
    }
    netImpairEnable(&srvImpairIn, &cfg);
    netImpairEnable(&srvImpairOut, &cfg);
    WB_LOG_INFO(WB_LOG_CAT_NET,
        "netimpair enabled: delay=%ums jitter=%ums loss=%u%% burst=%u",
        (unsigned)cfg.baseDelayMs, (unsigned)cfg.jitterMs,
        (unsigned)cfg.lossPercent, (unsigned)cfg.burstLossLen);
#else
    /* Impairment tooling compiled out (WB_ENABLE_NETIMPAIR == 0). */
    (void)spec;
#endif
}

void transportUdpServerDestroy(void) {
    int i;

    WB_LOG_INFO(WB_LOG_CAT_NET, "server destroy: tickCount=%u dropCount=%u",
                (unsigned)udpServer.tickCount, (unsigned)recvDropCount);

    /* Stop recv thread before touching the socket */
    if (recvThread) {
        SDL_SetAtomicInt(&recvThreadRunning, 0);
        SDL_WaitThread(recvThread, NULL);
        recvThread = NULL;
        recvThreadSock = INVALID_SOCKET;
    }

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
            serverCleanupMapDownload(i);
        }
    }
    udpServer.running = false;

    udpServerPublicIp[0] = '\0';
    udpServerPublicPort  = 0;
    memset(punchQueue, 0, sizeof(punchQueue));
}

/* Handle an old-protocol info request (server browser compatibility).
 * Builds an INFO_PACKET response from the current sim state. */
static void serverHandleInfoRequest(const struct sockaddr_in *fromAddr,
                                    ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    INFO_PACKET pkt;
    int i;
    BYTE numPlayers = 0, numHumans = 0, numBots = 0;
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
    if (udpServerPublicPort != 0) {
        pkt.gameid.serveraddress.s_addr = inet_addr(udpServerPublicIp);
        pkt.gameid.serverport = udpServerPublicPort;
    } else {
        pkt.gameid.serveraddress.s_addr = 0;
        pkt.gameid.serverport = serverSimGetServerPort(sim);
    }
    pkt.gameid.start_time = htonl(serverSimGetTimeCreated(sim));

    /* Map name as Pascal string */
    utilCtoPString((char *)serverSimGetMapName(sim), pkt.mapname);

    /* Game settings */
    pkt.gametype = (BYTE)gs->game;
    pkt.allow_mines = gs->hiddenMines ? HIDDEN_MINES : ALL_MINES_VISIBLE;
    pkt.allow_AI = 0;  /* AI type not tracked in new sim — report as none */
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
        pkt.flags = flags;
    }
    pkt.start_delay = serverSimGetStartDelay(sim);
    pkt.time_limit = serverSimGetGameLength(sim);

    /* Count connected players, classifying humans vs bots */
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(sim, i)) {
            numPlayers++;
            if (serverSimIsBot(sim, (BYTE)i)) numBots++;
            else                              numHumans++;
        }
    }
    pkt.num_players = numPlayers;
    pkt.num_humans  = numHumans;
    pkt.num_bots    = numBots;
    pkt.max_players = serverSimGetMaxPlayers(sim);

    /* Neutral pills and bases */
    pkt.free_pills = pillsGetNumNeutral(&gs->pb);
    pkt.free_bases = basesGetNumNeutral(&gs->bs);

    pkt.has_password = serverSimGetPassword(sim)[0] != '\0' ? 1 : 0;
    pkt.spectator_count = 0;

    {
        const char *md5Hex = serverSimGetMapMd5Hex(sim);
        if (md5Hex[0] != '\0' && !serverSimIsRandomMapEnabled(sim)) {
            memcpy(pkt.map_md5, md5Hex, 32);
        }
    }

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
    (void)sim;
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
    }
    WB_LOG_INFO(WB_LOG_CAT_NET, "ctrl queue reset all slots (game start)");
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
    BYTE scratchMap[131072];

    mapLen = serverSimGetCompressedMap(sim, scratchMap);
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
        /* Drop any in-flight map transfer and re-base CHANNEL_BULK so the new
         * map's stream starts clean on both ends: bulkSenderReset drops the old
         * staged blob, channelResetSend(CHANNEL_BULK) collapses the send window
         * and clears the staging tail, and a CTRL_CHANNEL_RESET carries the new
         * bulk baseline so the client lifts its receive baseline and abandons
         * the old partial. Without this the old transfer's stragglers would
         * segmentize into the new stream and the single BulkReceiver would
         * misparse it. */
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
        /* Arm a fresh join download from the new blob (re-gates snapshots). */
        serverInitMapDownload(i);
    }

    fprintf(stderr, "[UDP SERVER] Map change prep: %u bytes compressed map\n",
            udpServer.compressedMapSize);
}

void transportUdpServerSetBotName(BYTE playerNum, const char *name) {
    if (playerNum >= MAX_TANKS) return;
    strncpy(udpServer.clients[playerNum].playerName, name,
            PACKET_MAX_PLAYER_NAME - 1);
    udpServer.clients[playerNum].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
}

bool transportUdpServerStartBalanceRequest(ServerSim *sim,
                                           uint8_t teamSize,
                                           bool includeBots) {
    BalanceThreadData *btd = malloc(sizeof(BalanceThreadData));
    if (!btd) {
        return false;
    }
    SDL_Thread *t;
    int i;
    memset(btd, 0, sizeof(*btd));
    btd->sim = sim;
    btd->teamSize = teamSize;
    btd->includeBots = includeBots;
    btd->totalPlayers = 0;
    btd->numBotSlots = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsPlayerConnected(sim, i)) continue;
        btd->totalPlayers++;
        /* Include bot slots in the WBN request only when the caller
         * asked for "Bots included". When !includeBots the bots are
         * kicked at APPLY time and don't need to be skill-placed. */
        if (btd->includeBots && serverSimIsBot(sim, (BYTE)i)) {
            btd->botSlots[btd->numBotSlots++] = (uint8_t)i;
        }
    }
    serverSimSetBalanceRequestInFlight(sim, true);
    t = SDL_CreateThread(balanceThreadFunc, "WbnBalance", btd);
    if (!t) {
        serverSimSetBalanceRequestInFlight(sim, false);
        free(btd);
        serverSimConsoleMessage("Failed to start balance thread");
        return false;
    }
    SDL_DetachThread(t);
    return true;
}

void transportUdpServerHandleWbnReauth(ServerSim *sim, BYTE slot,
                                       const char *token) {
    if (!winbolonetIsRunning() || token == NULL || token[0] == '\0') {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[WBN] re-auth for slot %d ignored: running=%d tokenLen=%d",
                    (int)slot, winbolonetIsRunning() ? 1 : 0,
                    token ? (int)strlen(token) : -1);
        return;
    }
    char errorMsg[512];
    bool hasSteam = FALSE;
    bool wbnIsSupporter = FALSE;
    /* Capture the slot's keyed state *before* the verify fills the key,
     * so the deferred-join core can tell a fresh registration (key
     * absent->present) from an idempotent rekey resend. */
    bool wasParticipant = winboloNetIsPlayerParticipant(slot);
    /* For a pending provisional claim the slot's display name is the temp
     * -unverified[-N] handed out at join; WBN must verify and attribute
     * under the real account display name, which is the stored desired bare
     * name.  Non-claim reauths verify under the slot's own name as before. */
    bool isPendingClaim = udpServer.clients[slot].claimPending;
    const char *verifyName = isPendingClaim
        ? udpServer.clients[slot].claimDesiredName
        : udpServer.clients[slot].playerName;
    errorMsg[0] = '\0';
    if (winboloNetVerifyClientKey(token,
                                  verifyName,
                                  slot, errorMsg,
                                  &hasSteam, &wbnIsSupporter)) {
        /* Re-merge using the clientHints captured at JOIN_REQUEST (the
         * client doesn't re-send them on REAUTH; we re-verify against
         * WBN, not the network). */
        uint8_t storedHints = udpServer.clients[slot].clientHints;
        uint8_t flags = storedHints & PLAYER_CLIENT_HINT_MASK;
        flags |= PLAYER_FLAG_WBN_VERIFIED;
        if (hasSteam) flags |= PLAYER_FLAG_WBN_STEAM_LINKED;
        if (wbnIsSupporter) flags |= PLAYER_FLAG_SUPPORTER;
        playersSetClientFlags(&serverSimGetGameSim(sim)->plyrs, slot, flags);
        /* Keep the durable rekey-gate bit in step with the session flag. */
        udpServer.clients[slot].wbnWasVerified = true;
        playersSetClientType (&serverSimGetGameSim(sim)->plyrs, slot,
                              udpServer.clients[slot].clientType);
        WB_LOG_INFO(WB_LOG_CAT_NET,
                    "[WBN] Player %d re-authenticated (steam=%d)",
                    (int)slot, hasSteam ? 1 : 0);
        /* Fire the deferred PLAYER_JOIN now that the slot is keyed — in
         * every phase, not just running.  The edge guard emits exactly
         * once per session: a fresh join or a post-rotation re-register
         * (key was absent) emits; an idempotent rekey resend (already a
         * participant) does not.  This also satisfies the anonymous
         * fallback armed at join, so the grace sweep won't fire too. */
        if (wbnJoinOnReauth(&udpServer.clients[slot].wbnJoin, wasParticipant)) {
            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                               slot, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
        }
        if (serverSimGetState(sim) == serverStateLobby ||
            serverSimGetState(sim) == serverStateCountdown) {
            serverSimPublishLobbySlot(sim, slot);
        }

        /* Resolve a pending provisional claim: verify ran under the desired
         * bare name above, so attribution is correct; now reconcile the local
         * display.  Three outcomes by who holds the bare name now. */
        if (isPendingClaim) {
            const char *desired = udpServer.clients[slot].claimDesiredName;
            int s;
            int holder = -1;
            for (s = 0; s < MAX_TANKS; s++) {
                if (s == (int)slot) continue;
                if (!udpServer.clients[s].connected) continue;
                if (playerNameCompare(udpServer.clients[s].playerName,
                                      desired) == 0) {
                    holder = s;
                    break;
                }
            }

            bool holderIsVerified =
                holder >= 0 &&
                (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs,
                                       (BYTE)holder)
                 & PLAYER_FLAG_WBN_VERIFIED) != 0;

            switch (claimResolveDecide(holder >= 0, holderIsVerified)) {
            case CLAIM_RESOLVE_PROMOTE_FREE:
                /* Bare name free — the squatter left during grace.  Promote
                 * straight to the bare name. */
                serverSimSetPlayerName(sim, slot, desired);
                serverSimPublishLobbySlot(sim, slot);
                snprintf(udpServer.clients[slot].playerName,
                         PACKET_MAX_PLAYER_NAME, "%s", desired);
                udpServer.clients[slot].nameStickySuffix = false;
                break;
            case CLAIM_RESOLVE_PREEMPT_SQUATTER: {
                /* Unverified squatter still holds the bare name.  Rename it
                 * off first (it must vacate before the joiner claims), then
                 * promote this slot.  If the squatter's suffix pool is
                 * exhausted, keep this slot on its temp name. */
                char squatterName[PACKET_MAX_PLAYER_NAME];
                if (serverChooseUnverifiedSuffix(
                        udpServer.clients[holder].playerName, holder,
                        squatterName, sizeof(squatterName))) {
                    serverPreemptRename(sim, holder, squatterName,
                                        desired,
                                        udpServer.clients[slot].countryCode);
                    /* Plain set, NOT serverPreemptRename — that would emit a
                     * spurious "renamed by verified player" naming the joiner
                     * as its own victim. */
                    serverSimSetPlayerName(sim, slot, desired);
                    serverSimPublishLobbySlot(sim, slot);
                    snprintf(udpServer.clients[slot].playerName,
                             PACKET_MAX_PLAYER_NAME, "%s", desired);
                    udpServer.clients[slot].nameStickySuffix = false;
                }
                /* else: squatter pool exhausted — stay on the temp name. */
                break;
            }
            case CLAIM_RESOLVE_KEEP_TEMP:
                /* A verified slot won the bare name (a second reclaimer won
                 * the race); keep this slot on its temp name permanently.  WBN
                 * attribution is already correct since verify ran under the
                 * bare name; only the local display stays suffixed. */
                break;
            }

            /* Clear the claim in every outcome.  desired aliases the buffer,
             * so this clear must come after all uses of desired. */
            udpServer.clients[slot].claimPending = false;
            udpServer.clients[slot].claimDesiredName[0] = '\0';
        }
    } else {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[WBN] Player %d re-auth failed: %s", (int)slot, errorMsg);
    }
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

/* Send an INFO_RESPONSE packet to the tracker so the game is listed. */
void transportUdpServerSendTrackerUpdate(ServerSim *sim,
                                         const char *trackerAddr,
                                         unsigned short trackerPort) {
    GameSim *gs = serverSimGetGameSim(sim);
    INFO_PACKET pkt;
    struct sockaddr_in dest;
    struct in_addr trackerIp;
    int i;
    BYTE numPlayers = 0, numHumans = 0, numBots = 0;

    if (bolo_resolve_ipv4(trackerAddr, &trackerIp) != 0) {
        fprintf(stderr, "[TRACKER] Failed to resolve %s\n", trackerAddr);
        return;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr = trackerIp;
    dest.sin_port = htons(trackerPort);

    memset(&pkt, 0, sizeof(pkt));

    memcpy(pkt.h.signature, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE);
    pkt.h.versionMajor = BOLO_VERSION_MAJOR;
    pkt.h.versionMinor = BOLO_VERSION_MINOR;
    pkt.h.versionRevision = BOLO_VERSION_REVISION;
    pkt.h.type = BOLOPACKET_INFORESPONSE;

    if (udpServerPublicPort != 0) {
        pkt.gameid.serveraddress.s_addr = inet_addr(udpServerPublicIp);
        pkt.gameid.serverport = udpServerPublicPort;
    } else {
        pkt.gameid.serveraddress.s_addr = 0;
        pkt.gameid.serverport = serverSimGetServerPort(sim);
    }
    pkt.gameid.start_time = htonl(serverSimGetTimeCreated(sim));

    utilCtoPString((char *)serverSimGetMapName(sim), pkt.mapname);

    pkt.gametype = (BYTE)gs->game;
    pkt.allow_mines = gs->hiddenMines ? HIDDEN_MINES : ALL_MINES_VISIBLE;
    pkt.allow_AI = 0;
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
        pkt.flags = flags;
    }
    pkt.start_delay = serverSimGetStartDelay(sim);
    pkt.time_limit = serverSimGetGameLength(sim);

    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(sim, i)) {
            numPlayers++;
            if (serverSimIsBot(sim, (BYTE)i)) numBots++;
            else                              numHumans++;
        }
    }
    pkt.num_players = numPlayers;
    pkt.num_humans  = numHumans;
    pkt.num_bots    = numBots;
    pkt.max_players = serverSimGetMaxPlayers(sim);
    pkt.free_pills = pillsGetNumNeutral(&gs->pb);
    pkt.free_bases = basesGetNumNeutral(&gs->bs);
    pkt.has_password = serverSimGetPassword(sim)[0] != '\0' ? 1 : 0;
    pkt.spectator_count = 0;

    {
        const char *md5Hex = serverSimGetMapMd5Hex(sim);
        if (md5Hex[0] != '\0' && !serverSimIsRandomMapEnabled(sim)) {
            memcpy(pkt.map_md5, md5Hex, 32);
        }
    }

    srvSendTo((const uint8_t *)&pkt, sizeof(pkt), &dest);
}

void transportUdpServerSetPublicAddress(const char *externalIp,
                                        unsigned short externalPort) {
    if (externalPort == 0 || externalIp == NULL || externalIp[0] == '\0') {
        udpServerPublicIp[0] = '\0';
        udpServerPublicPort  = 0;
        return;
    }
    strncpy(udpServerPublicIp, externalIp, sizeof(udpServerPublicIp) - 1);
    udpServerPublicIp[sizeof(udpServerPublicIp) - 1] = '\0';
    udpServerPublicPort = externalPort;
}

void transportUdpServerSendNatKeepalive(ServerSim *sim,
                                        const char *trackerAddr,
                                        unsigned short trackerPort) {
    struct sockaddr_in dest;
    struct in_addr trackerIp;
    uint8_t buf[8];

    if (udpServer.sock == INVALID_SOCKET) return;
    if (trackerAddr == NULL || trackerAddr[0] == '\0') return;

    if (bolo_resolve_ipv4(trackerAddr, &trackerIp) != 0) return;

    buf[0] = 'W';
    buf[1] = 'B';
    buf[2] = 'K';
    buf[3] = 'A';
    /* 4-byte game token = serverSimGetTimeCreated(sim), big-endian. Tracker's exact-
     * match path uses (sourceIp, starttime) so multiple games behind one
     * NAT each refresh their own entry. */
    packU32(buf + 4, (uint32_t)serverSimGetTimeCreated(sim));

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr = trackerIp;
    dest.sin_port = htons(trackerPort);

    srvSendTo(buf, sizeof(buf), &dest);
}

void transportUdpServerSendPunchProbe(const char *trackerAddr,
                                      unsigned short trackerPort) {
    struct sockaddr_in dest;
    struct in_addr trackerIp;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (udpServer.sock == INVALID_SOCKET) return;
    if (trackerAddr == NULL || trackerAddr[0] == '\0') return;

    if (bolo_resolve_ipv4(trackerAddr, &trackerIp) != 0) return;

    packHeader(buf, PACKET_PUNCH_PROBE_REQUEST, 0);

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr = trackerIp;
    dest.sin_port = htons(trackerPort);

    srvSendTo(buf, sizeof(buf), &dest);
}

void transportUdpServerDrainPunchQueue(void) {
    int i;
    if (udpServer.sock == INVALID_SOCKET) return;
    for (i = 0; i < PUNCH_QUEUE_SIZE; i++) {
        PunchQueueEntry *e = &punchQueue[i];
        uint8_t sentinel;
        if (e->packetsRemaining == 0) continue;
        if (e->ticksUntilNext > 0) {
            e->ticksUntilNext--;
            continue;
        }
        /* 1-byte sentinel — joiner's recv loop drops anything shorter
         * than PACKET_HEADER_SIZE (8 bytes), so this is harmless on
         * arrival; its only purpose is to open our outbound NAT
         * mapping toward the joiner. */
        sentinel = 'P';
        srvSendTo(&sentinel, 1, &e->addr);
        e->packetsRemaining--;
        e->ticksUntilNext = PUNCH_BURST_INTERVAL;
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

/* Process a single received packet — extracted from the recv loop so both
 * the polled fallback and the recv-thread drain path can share it. */
static void serverProcessPacket(ServerSim *sim, uint8_t *buf, int len,
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
            if (clientIdx < 0) break;
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
                    /* Idle slot: refresh the live blob into this slot's copy,
                     * cut the map-event queue so the blob and the queue can't
                     * both carry the same change, then arm a resync transfer.
                     * It begins on CHANNEL_BULK once the channel is idle
                     * (serverServiceMapTransfer) and rides the snapshot trailer. */
                    int mapLen = serverSimGetCompressedMap(sim, udpServer.compressedMap);
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
                         * round-trip back to this server's live terrain. If it
                         * doesn't, the client can never match the live checksum
                         * and loops resync requests until it self-kicks — so
                         * decode the blob into scratch structures and compare
                         * tile-for-tile against the live map. Resyncs are
                         * infrequent; the cost is acceptable for the diagnosis. */
                        {
                            map *live = &serverSimGetGameSim(sim)->mp;
                            uint16_t liveSum = mapCalcChecksum(live);
                            map rtMap; pillboxes rtPb; bases rtBs; starts rtSs;
                            mapCreate(&rtMap);
                            pillsCreate(&rtPb);
                            basesCreate(&rtBs);
                            startsCreate(&rtSs);
                            if (mapLoadCompressedMap(&rtMap, &rtPb, &rtBs, &rtSs,
                                                     udpServer.compressedMap, mapLen)) {
                                uint16_t rtSum = mapCalcChecksum(&rtMap);
                                if (rtSum != liveSum) {
                                    int diffs = 0, shown = 0, xx, yy;
                                    for (yy = 0; yy < MAP_ARRAY_SIZE; yy++) {
                                        for (xx = 0; xx < MAP_ARRAY_SIZE; xx++) {
                                            BYTE lv = mapGetPos(live, (BYTE)xx, (BYTE)yy);
                                            BYTE rv = mapGetPos(&rtMap, (BYTE)xx, (BYTE)yy);
                                            if (lv != rv) {
                                                diffs++;
                                                if (shown < 8) {
                                                    WB_LOG_WARN(WB_LOG_CAT_NET,
                                                        "map resync blob diff @(%d,%d) live=%u roundtrip=%u",
                                                        xx, yy, (unsigned)lv, (unsigned)rv);
                                                    shown++;
                                                }
                                            }
                                        }
                                    }
                                    WB_LOG_WARN(WB_LOG_CAT_NET,
                                        "map resync blob does NOT round-trip: %d differing tile(s) "
                                        "(live sum=%u blob sum=%u) - client cannot converge",
                                        diffs, (unsigned)liveSum, (unsigned)rtSum);
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
                                    "[UDP SERVER] Client %d map resync gen=%u (%d bytes) livesum=%u\n",
                                    clientIdx, reqGen, mapLen, (unsigned)liveSum);
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

/* Decrement each connected client's per-tick rate-limit window for
 * lobby setter packets (LOBBY_SET_SETTING, LOBBY_PREVIEW_RANDOM, etc.).
 * Called once per server tick from whichever receive path is active —
 * without this, a client's cooldown would stick at LOBBY_REQ_COOLDOWN_TICKS
 * after its first rate-limited request and every subsequent one would be
 * silently dropped. */
static void udpServerTickPerClientCooldowns(void) {
    for (int i = 0; i < MAX_TANKS; i++) {
        SDL_assert(udpServer.clientReqCooldownTicks[i] <= LOBBY_REQ_COOLDOWN_TICKS);
        if (udpServer.clientReqCooldownTicks[i] > 0) {
            udpServer.clientReqCooldownTicks[i]--;
        }
    }
}

/* Release any datagrams now due from the impairment queues: inbound
 * packets back into serverProcessPacket, outbound packets onto the wire.
 * Both pops are no-ops while their layer is disabled (nothing queued), so
 * the disabled path is byte-for-byte the direct path.  Called from the
 * per-tick recv/drain entry points only — never the recv thread. */
static void srvDrainImpair(ServerSim *sim) {
    uint8_t pbuf[NET_IMPAIR_MAX_PACKET];
    struct sockaddr_in paddr;
    uint64_t now = (uint64_t)SDL_GetTicks();
    int plen;

    while ((plen = netImpairPop(&srvImpairIn, pbuf, sizeof(pbuf),
                                &paddr, now)) > 0) {
        serverProcessPacket(sim, pbuf, plen, &paddr);
    }
    while ((plen = netImpairPop(&srvImpairOut, pbuf, sizeof(pbuf),
                                &paddr, now)) > 0) {
        udpSendTo(udpServer.sock, pbuf, plen, &paddr);
    }

#if WB_ENABLE_NETIMPAIR
    /* Once-per-second impairment-queue summary so genuine injected loss
     * (overflow = the 512-slot queue filled, the only drop path when loss=0)
     * can be told apart from jitter-induced reordering — which is not loss at
     * all but shows up on the per-player [netstat] line as stale= when an
     * overtaken packet arrives after a newer one and is discarded. If overflow
     * holds at 0 while stale climbs, the "loss" is reordering, not drops. */
    if (netImpairEnabled(&srvImpairIn) || netImpairEnabled(&srvImpairOut)) {
        static uint64_t lastImpairLogMs = 0;
        if (now - lastImpairLogMs >= 1000) {
            lastImpairLogMs = now;
            mpDiagLog("[netimpair] in: q=%d overflow=%u  out: q=%d overflow=%u",
                      srvImpairIn.count, (unsigned)srvImpairIn.overflowDrops,
                      srvImpairOut.count, (unsigned)srvImpairOut.overflowDrops);
        }
    }
#endif
}

/* Receive all pending packets from clients (polled fallback) */
void transportUdpServerRecv(ServerSim *sim) {
    uint8_t buf[UDP_MAX_PAYLOAD];
    struct sockaddr_in fromAddr;
    int len;
    int c;

    if (!udpServer.running) return;

    udpServerTickPerClientCooldowns();

    for (c = 0; c < MAX_TANKS; c++) {
        udpServer.clients[c].inputsThisTick = 0;
    }

    udpServer.tickCount++;

    while ((len = udpRecvFrom(udpServer.sock, buf, sizeof(buf), &fromAddr)) > 0) {
        if (netImpairEnabled(&srvImpairIn)) {
            netImpairOffer(&srvImpairIn, buf, len, &fromAddr,
                           (uint64_t)SDL_GetTicks());
        } else {
            serverProcessPacket(sim, buf, len, &fromAddr);
        }
    }
    srvDrainImpair(sim);
}

/* Drain the recv thread's packet queue (called from timer callback) */
void transportUdpServerDrainRecvQueue(ServerSim *sim) {
    int c;
    int head, tail;

    if (!udpServer.running) return;

    udpServerTickPerClientCooldowns();

    for (c = 0; c < MAX_TANKS; c++) {
        udpServer.clients[c].inputsThisTick = 0;
    }

    udpServer.tickCount++;

    tail = SDL_GetAtomicInt(&recvQueueTail);
    head = SDL_GetAtomicInt(&recvQueueHead);

    while (tail != head) {
        RecvQueueEntry *entry = &recvQueue[tail];
        if (netImpairEnabled(&srvImpairIn)) {
            netImpairOffer(&srvImpairIn, entry->data, entry->len,
                           &entry->fromAddr, (uint64_t)SDL_GetTicks());
        } else {
            serverProcessPacket(sim, entry->data, entry->len, &entry->fromAddr);
        }
        tail = (tail + 1) % RECV_QUEUE_SIZE;
        SDL_SetAtomicInt(&recvQueueTail, tail);
        /* Re-read head in case more packets arrived during processing */
        head = SDL_GetAtomicInt(&recvQueueHead);
    }
    srvDrainImpair(sim);
}

/* Returns true if a dedicated recv thread is running */
bool transportUdpServerHasRecvThread(void) {
    return recvThread != NULL;
}

/* Drain sim events into per-client reliable queues.
 * Must be called after each serverSimTick() so events survive
 * being cleared at the start of the next tick.
 * EVENT_SOUND events are culled by distance and deduplicated per
 * sound type (only the closest instance of each type is sent). */
void transportUdpServerDrainEvents(ServerSim *sim) {
    int i, c;

    if (!udpServer.running) return;

    /* Once-per-second map-event-drop summary. A dropped EVENT_MAP_CHANGE
     * silently desyncs a client's terrain until it requests a map resync, so
     * surface how often the drop guard is firing. Mirrors the [netimpair]
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

    if (serverSimGetEventCount(sim) == 0 && serverSimGetMapEventCount(sim) == 0) return;

    for (c = 0; c < MAX_TANKS; c++) {
        WORLD cwx = 0, cwy = 0;
        BYTE clientMX = 0, clientMY = 0;
        bool hasPos;

        if (!udpServer.clients[c].connected) continue;

        /* Always enqueue map events, even during map download. The map
         * snapshot was taken when the client joined, so any map changes
         * that happen during the download window must be queued here.
         * They'll be sent once downloadComplete becomes true (the send
         * path in transportUdpServerSend checks downloadComplete
         * separately). Without this, mid-game joiners permanently
         * desync because map events during download are lost. */
        {
            ClientEventQueue *mq = &udpServer.mapEventQueues[c];
            for (i = 0; i < (int)serverSimGetMapEventCount(sim); i++) {
                if (!eventQueueHasSpace(mq)) {
                    int dropped = (int)serverSimGetMapEventCount(sim) - i;
                    udpServer.mapEventQueueDrops[c] += (uint32_t)dropped;
                    fprintf(stderr, "[UDP SERVER] Map event queue full for client %d, dropping %d events\n",
                            c, dropped);
                    break;
                }
                uint32_t idx = mq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                mq->buffer[idx].event = serverSimGetMapEvents(sim)[i];
                mq->buffer[idx].seq = mq->nextSeq;
                mq->nextSeq++;
            }
        }

        /* Game events (sounds, kills, etc.) only matter once the client
         * is in-game with a loaded map — skip if still downloading. */
        if (!udpServer.mapDownload[c].downloadComplete) continue;

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

        /* Best-effort fx (sounds/explosions) are culled to the recipient's
         * tank + owned/allied pillbox viewports, matching the snapshot cull. */
        ViewportRect fxViewports[MAX_VIEWPORTS];
        int fxViewportCount = serverSimBuildViewports(sim, (BYTE)c, fxViewports, MAX_VIEWPORTS);

        /* Pass 1: find best (closest) sound event per type for this client */
        #define MAX_SOUND_TYPES 32
        int bestSoundIdx[MAX_SOUND_TYPES];
        int bestSoundDist[MAX_SOUND_TYPES];
        int s;
        for (s = 0; s < MAX_SOUND_TYPES; s++) {
            bestSoundIdx[s] = -1;
            bestSoundDist[s] = 255;
        }

        for (i = 0; i < (int)serverSimGetEventCount(sim); i++) {
            uint8_t evType = serverSimGetEvents(sim)[i].type;
            if ((evType == EVENT_SOUND || evType == EVENT_SOUND_TANK_HIT || evType == EVENT_SOUND_SHOOT) && hasPos) {
                uint8_t soundId = serverSimGetEvents(sim)[i].data[0];
                uint8_t mx = serverSimGetEvents(sim)[i].data[1];
                uint8_t my = serverSimGetEvents(sim)[i].data[2];
                int dx = (clientMX > mx) ? (clientMX - mx) : (mx - clientMX);
                int dy = (clientMY > my) ? (clientMY - my) : (my - clientMY);

                /* Skip own shoot sound — client plays shootSelf via prediction */
                if (evType == EVENT_SOUND_SHOOT && serverSimGetEvents(sim)[i].data[3] == (uint8_t)c) {
                    continue;
                }

                /* Always send tank hit to the hit player (plays hitTankSelf at full volume) */
                if (evType == EVENT_SOUND_TANK_HIT && serverSimGetEvents(sim)[i].data[3] == (uint8_t)c) {
                    /* Skip distance cull */
                } else if (!inAnyViewport(fxViewports, fxViewportCount, mx, my)) {
                    /* Cull beyond the recipient's viewports */
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

        /* Pass 2: route non-sound game events, then deduplicated sounds.
         * Each event goes to the reliable game channel (CHANNEL_GAME) if
         * gameEventIsReliable, otherwise to the best-effort channel
         * (CHANNEL_GAME_EFFECT). Sounds are all best-effort. */
        for (i = 0; i < (int)serverSimGetEventCount(sim); i++) {
            uint8_t evType = serverSimGetEvents(sim)[i].type;
            if (evType != EVENT_SOUND && evType != EVENT_SOUND_TANK_HIT && evType != EVENT_SOUND_SHOOT) {
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
        for (s = 0; s < MAX_SOUND_TYPES; s++) {
            if (bestSoundIdx[s] >= 0) {
                uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
                int evLen = packGameEvent(evBuf, &serverSimGetEvents(sim)[bestSoundIdx[s]]);
                /* Sounds are ephemeral — best-effort: never blocks, never
                 * disconnects on overflow (drops oldest). */
                channelSendBestEffort(&udpServer.channelMux[c],
                                      CHANNEL_GAME_EFFECT, evBuf,
                                      (uint16_t)evLen);
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
            serverDisconnectSpectator(i, false);
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

bool transportUdpServerTestDownloadComplete(int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return false;
    return udpServer.mapDownload[slot].downloadComplete;
}

/* Test-only: stage one terrain change for a slot exactly as a real sim tick
 * does — mutate the live server map AND enqueue an EVENT_MAP_CHANGE into the
 * slot's map-event hold queue (mirroring simMapChangeCallback →
 * transportUdpServerDrainEvents). Mutating the map keeps its snapshot checksum
 * in step with the change the client applies, so the client doesn't see a
 * spurious terrain divergence and self-trigger a resync. The event then flows
 * through the real hold → channel drain → tagged channelSend(CHANNEL_MAP) path,
 * so the loopback test exercises the live wiring rather than poking the channel
 * directly. Call between ticks: the map-change callback is dormant then, so the
 * server-side mapSetPos won't double-enqueue. Returns false if the slot is
 * invalid or its hold queue is full. */
bool transportUdpServerTestAddMapEvent(ServerSim *sim, int slot, uint8_t x,
                                       uint8_t y, uint8_t terrain) {
    ClientEventQueue *mq;
    uint32_t idx;
    if (slot < 0 || slot >= MAX_TANKS) return false;
    mq = &udpServer.mapEventQueues[slot];
    if (!eventQueueHasSpace(mq)) return false;
    if (sim != NULL) {
        mapSetPos(&sim->sim, &sim->sim.mp, x, y, terrain, FALSE, TRUE);
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

/* Set or clear the server's "locked to new players" state and announce the
 * change to every connected, download-complete client with a reliable
 * EVENT_SERVER_MSG on CHANNEL_GAME — the same path the per-tick game-event
 * producer uses (transportUdpServerDrainEvents).
 *
 * Precondition: the caller must hold threadsMutex.  The notice goes out via
 * channelSend, which mutates per-client channelMux state that is otherwise only
 * touched on the tick; the mutex is what serializes the two.  All callers
 * comply — the servermain console wraps each call (lock/unlock/alarm), and the
 * server_sim auto-lock/unlock callers run inside serverInstanceTick.  The
 * assert no-ops in bare logic unit tests that never start the threading system
 * (no tick thread to race, and no client is connected there to send to). */
void transportUdpServerSetLock(ServerSim *sim, bool locked) {
    (void)sim;
    SDL_assert(!threadsContextActive() || threadsCurrentlyHoldsMutex());
    if (udpServer.gameLocked == locked) return;
    udpServer.gameLocked = locked;
    serverSimConsoleMessage(locked
        ? "This game is now locked to new players (server lock)"
        : "This game is now unlocked to new players (server unlock)");
    winboloNetSendLock(locked);
    /* Announce on the reliable game channel.  serverSimAddEvent() won't do —
     * the sim's per-tick event buffer is cleared at the start of each tick and
     * this runs between ticks — so pack the event once and channelSend it onto
     * CHANNEL_GAME per connected, download-complete client, exactly as the
     * per-tick producer does. */
    {
        GameEvent ev;
        uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
        int evLen;
        int c;
        ev.type = EVENT_SERVER_MSG;
        memset(ev.data, 0, sizeof(ev.data));
        ev.data[0] = locked ? SERVER_MSG_GAME_LOCKED : SERVER_MSG_GAME_UNLOCKED;
        evLen = packGameEvent(evBuf, &ev);
        for (c = 0; c < MAX_TANKS; c++) {
            if (!udpServer.clients[c].connected) continue;
            if (!udpServer.mapDownload[c].downloadComplete) continue;
            if (!channelSend(&udpServer.channelMux[c], CHANNEL_GAME,
                             evBuf, (uint16_t)evLen)) {
                /* Window full — defer the disconnect off this path, mirroring
                 * the per-tick game-channel overflow handling.  The flag guard
                 * keeps a re-hit from spamming the log. */
                if (!udpServer.pendingSimRemove[c]) {
                    WB_LOG_ERROR(WB_LOG_CAT_NET,
                                 "game channel overflow for slot %d, deferring disconnect",
                                 c);
                    udpServer.pendingSimRemove[c] = true;
                }
            }
        }
    }
}

bool transportUdpServerGetLock(void) {
    return udpServer.gameLocked;
}

void transportUdpServerSendServerMessage(const char *message) {
    /* Public API: external callers (servermain console "say", saveMap
     * announcement, server_lifecycle pendingWinMessage) pass arbitrary
     * pre-rendered English strings.  These don't have dedicated langids
     * yet, so route through the legacy English passthrough wire format.
     * Sim handle comes from serverSimGetActive() — the public signature
     * doesn't carry it, matching the pattern other public entry points
     * in this TU use when they need the active sim. */
    ServerSim *sim = serverSimGetActive();
    if (sim == NULL) return;
    serverSendServerEnglishBroadcast(sim, message);
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

#ifdef WB_FUZZ
/* ================================================================
 * Fuzz-only dispatcher seam (hardening plan §1.2, tier 2)
 *
 * Drives serverProcessPacket directly with attacker-controlled bytes —
 * no socket, no recv thread. serverProcessPacket and the file-static
 * `udpServer` it mutates have internal linkage, so this seam must live in
 * the same TU. Compiled only under -DWB_FUZZ (the dedicated fuzz build);
 * every shipping build leaves these symbols out entirely.
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

