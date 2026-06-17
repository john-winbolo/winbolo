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
 *      last INPUT_REDUNDANCY_COUNT inputs per packet for loss tolerance).
 *    - Receives state snapshots from server.
 *    - Handles join handshake and ping measurement.
 *********************************************************/

#include "transport_udp_internal.h"
#include "client_timing.h"
#include "bases.h"
#include "pillbox.h"
#include "players.h"
#include "util.h"
#include "messages.h"
#include "client_sim.h"
#include "client_net.h"                /* clientSimGetViewTick */
#include "frontend.h"                  /* frontEndApplyLocalTankPrefs */
#include "client_sim_internal.h"
#include "control_event.h"
#include "client_sim_control.h"
#include "transport_control_codec.h"
#include "transport_command_codec.h"
#include "channel_mux.h"
#include "wbn_key_codec.h"
#include "bolo_map_validate.h"
#include "wire_limits.h"
#include "../common/md5.h"
#include "../gui/lang.h"
#include "../gui/winbolo.h"
#include "../gui/dialogAlliance.h"
#include "../steam/steam_wrapper.h"
#include "../common/wb_log.h"
#include "../common/mp_diag_log.h"
#include "net_impair.h"
#include "../winbolonet/winbolonet_client.h"
#include "../winbolonet/winbolonet_core.h"

/* ================================================================
 * CLIENT SIDE
 * ================================================================ */

#define OUT_CMD_QUEUE_CAP 64

typedef struct {
    ClientCommand cmd;          /* cmd.cmdSeq matches this entry's seq */
    uint32_t lastSentMs;        /* 0 = never sent yet; eager send sets it */
} OutCmdEntry;

typedef struct {
    SOCKET sock;
    struct sockaddr_in serverAddr;
    BYTE playerNum;
    UdpClientJoinState joinState;
    char playerName[PACKET_MAX_PLAYER_NAME];
    char password[MAP_STR_SIZE];
    /* Long-lived WBN credential from prefs; empty when the user is not
     * logged in.  Stays out of the wire — only fed to
     * winbolonetClientJoinSession to mint per-send player_keys. */
    char wbnApiToken[WBN_JOIN_KEY_WIRE_LEN];
    /* WBN session key for the connected server.  Empty for direct-IP
     * joins; refreshed by PACKET_WBN_REKEY when the server rotates. */
    char wbnServerKey[WINBOLONET_KEY_LEN];
    uint32_t outSequence;
    /* Per-session connection id handed back in JOIN_ACCEPT and echoed on every
     * INPUT so the server can re-home this slot after a NAT rebind. 0 until a
     * JOIN_ACCEPT carrying one arrives (old/short accept leaves it 0). */
    uint64_t connId;

    /* Reliable outbound command carrier. cmdSeq is monotonic per
     * connection (resets in transportUdpClientCreate). The queue holds
     * one entry per submitted ClientCommand from outHeadSeq up to but
     * not including outTailSeq. Capacity is fixed at OUT_CMD_QUEUE_CAP;
     * overflow asserts in debug, silently drops in release (rare
     * — user-action submit rate is bounded). */
    OutCmdEntry outCmdQueue[OUT_CMD_QUEUE_CAP];
    uint32_t outCmdNextSeq;   /* next cmdSeq to assign on submit (init 1) */
    uint32_t outHeadSeq;      /* lowest unacked seq; advances on ACK */
    uint32_t outTailSeq;      /* exclusive tail; outTailSeq == outCmdNextSeq */

    /* Input redundancy ring buffer */
    InputPacket inputRing[CLIENT_INPUT_RING_SIZE];
    uint32_t inputRingCount;  /* Total inputs recorded */
    uint32_t lastSentInputTick; /* tick of the most recent input written to the
                                 * ring — the newest input the client has put on
                                 * the wire, in InputPacket.tick/simTickCounter
                                 * space.  Differenced against the snapshot
                                 * header's lastProcessedInput (same clock) for
                                 * the estimator's pipeline depth. */

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
    uint32_t controlEventAck;   /* Next expected reliable control event seq (init to 1) */
    /* Coalesce PACKET_CONTROL_ACK emission to ~50ms — only relevant
     * during non-running phases when PACKET_CONTROL_TICK is the
     * carrier.  controlAckPendingTick: localTick when the first
     * post-ACK tick arrived (0 = no ack pending).  lastSentControlAck:
     * the controlEventAck value carried in the most recent ACK packet,
     * used to avoid resending an unchanged ACK. */
    uint32_t controlAckPendingTick;
    uint32_t lastSentControlAck;
    /* Set once the client has snapped controlEventAck down into the new
     * sequence space after the server's game-start queue wipe (seq-1
     * RUNNING).  Cleared on the reverse game-over / lobby transition so
     * the next game's wipe re-adopts.  Decouples RESET-DETECT from
     * inLobby timing: under loss/reorder inLobby can flip false before
     * the seq-1 RUNNING snapshot is processed, which used to skip the
     * snap forever and wedge the control queue. */
    bool runningSeqAdopted;

    /* Join handshake state */
    uint32_t joinAttempts;
    uint32_t ticksSinceJoinSent;

    /* Address-proof retry cookie. The server gates slot allocation on a valid
     * cookie; the client never computes one — it stores the opaque bytes from
     * a PACKET_JOIN_CHALLENGE and echoes them in the JOIN tail. Zeros (and
     * haveJoinCookie=false) until the first challenge arrives. */
    uint8_t  joinCookie[JOIN_COOKIE_LEN];
    bool     haveJoinCookie;

    /* Ping */
    uint32_t lastPingSentTick;
    uint32_t pingClientTime;  /* Monotonic counter used as ping timestamp */
    uint16_t pingMs;          /* Min-over-window RTT — feeds shell projection
                               * and the InputPacket lag-comp fallback. */
    PingMinWindow pingMinWin; /* Backing state for pingMs. */
    PingEwma pingEwma;        /* Backing state for pingDisplayMs. */
    uint16_t pingDisplayMs;   /* EWMA RTT — feeds the HUD/scoreboard displays. */
    uint32_t localTick;  /* Local tick counter for timing */

    /* Measurement-only clock/jitter/RTT estimator, fed at JOIN_ACCEPT, each
     * snapshot header, and each PONG.  Surfaced in the Net Info readout;
     * nothing consumes it to alter timing or pacing yet. */
    ClientTiming timing;

    /* Map download state */
    BYTE    *mapDownloadBuf;     /* Buffer for reassembling compressed map */
    uint32_t mapDownloadTotal;   /* Total expected bytes */
    uint32_t mapDownloadReceived;/* Bytes received so far */
    uint16_t mapChunksExpected;  /* Total chunks expected */
    uint16_t mapChunksReceived;  /* Number of unique chunks received */
    bool    *mapChunkReceived;   /* Bitfield: which chunks we've gotten */
    /* True once the buffered map has been installed onto the ClientSim
     * (mp/pb/bs/ss populated). Distinct from mapDownloadComplete on the
     * ClientSim (bytes-received) — this tracks "applied". Reset to false
     * when a fresh download begins (JOIN_ACCEPT reallocates the buffer)
     * so a mid-lobby map swap re-gates snapshots until the new map is
     * installed. */
    bool     mapInstalled;

    /* Map desync recovery (resync) — a parallel download that runs while the
     * client keeps playing (joinState stays CONNECTED) and hot-swaps the map
     * in on completion. Mirrors the join-download fields above. */
    BYTE    *mapResyncBuf;             /* Parallel reassembly buffer (owned) */
    bool    *mapResyncChunkReceived;   /* Bitfield: which chunks we've gotten */
    uint32_t mapResyncTotal;           /* Total expected bytes (from chunk mapSize) */
    uint16_t mapResyncChunksExpected;  /* Total chunks expected */
    uint16_t mapResyncChunksReceived;  /* Unique chunks received */
    uint32_t activeResyncGen;          /* gen of the in-flight resync (0 = none) */
    uint32_t resyncGenCounter;         /* Monotonic source for fresh nonzero gens */
    bool     resyncActive;             /* A resync request is outstanding/installing */
    bool     firstResyncChunkSeen;     /* Stop request retransmit once chunks arrive */
    uint32_t lastResyncRequestTick;    /* localTick of last request send (retransmit) */
    uint32_t resyncAttempts;           /* Resyncs that did not resolve the mismatch */
    uint32_t resyncSuppressUntilTick;  /* Gate new requests until here (grace window) */
    uint32_t lastResyncProgressTick;   /* localTick of last forward progress (request
                                        * sent at start, or a chunk received); drives
                                        * the stall watchdog that abandons a wedged
                                        * resync so the disconnect cap can fire */
    uint32_t mapResyncCount;           /* Cumulative successful resyncs (Net Info) */
    uint32_t installedMapGen;          /* Generation of the most recently installed
                                        * map (0 until the first resync installs).
                                        * Persistent across resyncs (distinct from
                                        * the transient activeResyncGen): a
                                        * CHANNEL_MAP event tagged older than this
                                        * is a stale change the fresh blob already
                                        * carries and is dropped on the drain. */

    /* Join reject reason from server, rendered locally via langGetTextFmt
     * after Phase 9d wire format change. Sized for the longest expected
     * localized rendering. */
    char joinRejectReason[256];

    /* Owning ClientSim — used for player state updates in callbacks */
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

    /* Inbound snapshot loss tracking.  Snapshots arrive once per server
     * frame (sim->tick advances by 2 per frame), so consecutive snapshots
     * have serverTick spaced by 2; a larger gap means snapshots were lost
     * on the wire. */
    uint32_t lastSnapshotServerTick;  /* serverTick of last accepted snapshot (0 = none) */
    uint32_t snapshotsRecvThisSec;    /* Snapshots accepted in current 1-second window */
    uint32_t snapshotsLostThisSec;    /* Snapshots inferred lost in current 1-second window */
    uint32_t snapshotsRecvLast;       /* Last completed window: accepted */
    uint32_t snapshotsLostLast;       /* Last completed window: lost */
    uint32_t snapshotsLostTotal;      /* Cumulative inferred-lost snapshots since join */

    bool wantRejoin;               /* Request rejoin (restore pills/bases) on connect */

    /* Phase 3 — UDP hole-punching fallback. Empty trackerAddr disables
     * punch entirely (LAN/manual-connect joiners). */
    char           trackerAddr[FILENAME_MAX];
    unsigned short trackerPort;
    struct in_addr targetIp;       /* host IP (network order) for PUNCH_REQUEST body */
    unsigned short targetPort;     /* host port (host order) for PUNCH_REQUEST body */
    bool           punchSent;      /* sent at least one PUNCH_REQUEST */

    /* ISO-3166 fallback country (2 chars + NUL). Empty when the caller
     * passed NULL/"". Written verbatim into the trailing slot of every
     * JOIN_REQUEST so the server's GeoIP-failed fallback path can use
     * it uniformly (loopback, LAN, missing MMDB). */
    char fallbackCountry[3];

    /* Lobby map upload — the chunked PACKET_LOBBY_MAP_UPLOAD_* state
     * machine that used to live in imgui_lobby's per-frame pump. The
     * frontend kicks it off via transportUdpClientStartLobbyMapUpload*
     * and reads progress back via clientSimGetLobbyMapUpload* status +
     * Percent getters. Pump fires from udpClientTick once per tick. */
    bool      uploadActive;
    uint8_t  *uploadBuf;                  /* malloc'd, sized to uploadTotal */
    uint32_t  uploadTotal;
    uint32_t  uploadOffset;               /* bytes already sent via CHUNK */
    char      uploadName[128];            /* wire-side filename announced to server */
    /* USE_LOCAL pre-check: when the source path resolves under
     * data/maps/, the kick computes md5 + the data/maps-relative
     * filename and sends PACKET_LOBBY_MAP_USE_LOCAL first. On
     * USE_LOCAL_NACK the pump transitions to BEGIN+CHUNK using the
     * bytes already buffered. */
    bool      uploadUseLocalPending;      /* USE_LOCAL sent, awaiting ACK/NACK */
    bool      uploadBeginSent;            /* BEGIN sent (USE_LOCAL never tried, or NACKed) */
    /* Watchdog timestamps (SDL ticks ms). Reset on forward progress:
     * status flip, offset advance, or the transition to awaiting-DONE
     * after the last chunk. */
    uint64_t  uploadStartedMs;
    uint8_t   uploadPrevStatus;
    uint64_t  uploadPrevProgressMs;
    uint32_t  uploadPrevOffset;

    /* Runtime network impairment on the inbound (server->client) and
     * outbound (client->server) datagram paths. Disabled unless the
     * WB_NETIMPAIR env var was set and parsed at ctx create. Driven from
     * udpClientTick off the process-global bolo_rand stream. */
    NetImpair impairIn;
    NetImpair impairOut;

    /* Reliable-ordered channel multiplexer (channel_mux.c), running empty and
     * in parallel with the existing reliable-event acks.  A channel frame
     * rides every outgoing input as a trailer, and a standalone PACKET_CHANNEL
     * carries it when no input flows (lobby/countdown).  Inbound frames are
     * recovered as the bytes past a snapshot's parsed end or from a standalone
     * PACKET_CHANNEL. */
    ChannelMux channelMux;
    /* Count of channel frames consumed (trailer + standalone), for test
     * observability of the otherwise-silent parallel layer. */
    uint32_t   channelFramesRx;
} TransportUdpClientCtx;

#define UPLOAD_ACK_TIMEOUT_MS   5000   /* BEGIN/USE_LOCAL → ACK */
#define UPLOAD_STALL_TIMEOUT_MS 10000  /* no chunk progress */
#define UPLOAD_CHUNK_SIZE       1024
#define UPLOAD_CHUNKS_PER_TICK  8

/* Diagnostic-only: one-shot guard so we log the kernel-assigned local
 * port once per process the first time getsockname() returns a non-zero
 * port (i.e. after the implicit bind from the first sendto). File scope
 * keeps the declaration off MSVC's C89 mixed-decl-and-statement path. */
static int udpClientLoggedLocalPort = 0;

/* Client send wrapper — tracks packet and byte counters */
static void udpClientSendTo(TransportUdpClientCtx *c, const uint8_t *buf, int len) {
    /* When outbound impairment is enabled, hand the datagram to the layer
     * instead of sending directly — udpClientTick later pops the delayed
     * packets onto the wire. An oversize datagram (offer returns false)
     * falls through to a direct send. Counters tick exactly once per packet
     * either way, here at offer/send time. */
    if (netImpairEnabled(&c->impairOut) &&
        netImpairOffer(&c->impairOut, buf, len, &c->serverAddr,
                       (uint64_t)SDL_GetTicks())) {
        c->packetsSentThisSec++;
        c->bytesSentThisSec += len;
        return;
    }
    udpSendTo(c->sock, buf, len, &c->serverAddr);
    c->packetsSentThisSec++;
    c->bytesSentThisSec += len;
    if (!udpClientLoggedLocalPort) {
        /* NB: don't name this `local` — brain.h does `#define local static`
         * for its Lua-flavoured pseudo-keyword and that macro is in scope
         * through the include chain.  `local sockaddr_in foo;` then
         * preprocesses to `static sockaddr_in foo;` which MSVC parses as
         * a bare type declaration with no variable name (C4091/C2059). */
        struct sockaddr_in localAddr;
        socklen_t locLen;
        memset(&localAddr, 0, sizeof(localAddr));
        locLen = (socklen_t)sizeof(localAddr);
        if (getsockname(c->sock, (struct sockaddr *)&localAddr, &locLen) == 0
            && localAddr.sin_port != 0) {
            mpDiagLog("[cli] local socket bound at %s:%u (kernel-assigned ephemeral; SO_REUSEADDR=on) -> server %s:%u",
                      inet_ntoa(localAddr.sin_addr),
                      (unsigned)ntohs(localAddr.sin_port),
                      inet_ntoa(c->serverAddr.sin_addr),
                      (unsigned)ntohs(c->serverAddr.sin_port));
            udpClientLoggedLocalPort = 1;
        }
    }
}

/* Mint a fresh player_key against the current WBN server_key and send a
 * PACKET_WBN_REAUTH to the server. The wire packet MUST be built through
 * commandCodecEncode — the server decodes it with commandCodecDecode, whose
 * body sits at CMD_PACKET_BODY_OFFSET (header + 4), not raw header offset.
 * Centralising the encode here keeps the one correct format. No-op without
 * a token/server_key; logs and returns on a failed key exchange (caller
 * decides *when* to re-auth, so there is no retry here). */
static void udpClientSendWbnReauth(TransportUdpClientCtx *c) {
    char playerKey[WBN_JOIN_KEY_WIRE_LEN];
    char errMsg[256];

    if (c->wbnApiToken[0] == '\0' || c->wbnServerKey[0] == '\0') return;

    memset(playerKey, 0, sizeof(playerKey));
    errMsg[0] = '\0';
    if (!winbolonetClientJoinSession(c->wbnApiToken, c->wbnServerKey,
                                     playerKey, errMsg)) {
        WB_LOG_WARN(WB_LOG_CAT_NET, "[WBN] re-auth exchange failed: %s",
                    errMsg[0] ? errMsg : "(no detail)");
        return;
    }

    ClientCommand cmd = { .type = CMD_WBN_REAUTH };
    memcpy(cmd.u.wbnReauth.token, playerKey, WBN_JOIN_KEY_WIRE_LEN);

    uint8_t buf[COMMAND_MAX_WIRE_BYTES];
    size_t len;
    if (commandCodecEncode(&cmd, buf, sizeof(buf), &len)) {
        udpClientSendTo(c, buf, (int)len);
        WB_LOG_INFO(WB_LOG_CAT_NET, "[WBN] Sent re-auth for slot %d", c->playerNum);
    }
}

/* Forward decl: defined alongside the upload state machine below;
 * called from the connected-state branch of udpClientTick. */
static void udpClientUploadPump(TransportUdpClientCtx *c);

/* connId rides the wire as two 32-bit halves through the existing packU32
 * helpers — low half first, then high. Server send/read must agree with these. */
static void packConnId(uint8_t *buf, uint64_t connId) {
    packU32(buf,     (uint32_t)(connId & 0xffffffffULL));
    packU32(buf + 4, (uint32_t)(connId >> 32));
}
static uint64_t unpackConnId(const uint8_t *buf) {
    uint32_t lo = unpackU32(buf);
    uint32_t hi = unpackU32(buf + 4);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Build an input packet into buf, returns length.
 * Wire layout: [header 8][connId 8][count 1][29-byte inputs…]. */
static int buildInputPacket(TransportUdpClientCtx *c, uint8_t *buf) {
    int offset;
    int i, count;

    packHeader(buf, PACKET_INPUT, c->outSequence++);
    offset = PACKET_HEADER_SIZE;

    /* connId framing prefix — the server reads this before the input count to
     * re-home the slot on a NAT rebind. */
    packConnId(buf + offset, c->connId);
    offset += 8;

    count = INPUT_REDUNDANCY_COUNT;
    if (c->inputRingCount < (uint32_t)count) {
        count = (int)c->inputRingCount;
    }

    buf[offset++] = (uint8_t)count;

    for (i = count - 1; i >= 0; i--) {
        uint32_t idx = (c->inputRingCount - 1 - (uint32_t)i) % CLIENT_INPUT_RING_SIZE;
        offset += packInputPacket(buf + offset, &c->inputRing[idx]);
    }

    /* Parallel channel layer rides as a trailer after the fixed inputs; the
     * server recovers it as the bytes past the last input.  channelTick runs
     * once per tick in udpClientTick, so only build the frame here. */
    {
        int budget = UDP_MAX_PAYLOAD - offset;
        if (budget >= 2) {
            offset += channelBuildFrame(&c->channelMux, buf + offset, budget);
        }
    }

    return offset;
}

bool udpInputEdgeChanged(const InputPacket *prev, const InputPacket *cur) {
    return prev->buttons != cur->buttons || prev->actions != cur->actions;
}

/* Record input into redundancy ring without sending a packet.
 * Used on keys ticks so the input is carried by the next sendInput, and
 * by sendInput itself so a game-tick send never promotes (see below). */
static void udpClientRecordInputInternal(TransportUdpClientCtx *c,
                                         const InputPacket *input) {
    if (c->joinState != UDP_CLIENT_CONNECTED) {
        return;
    }

    /* Store in ring buffer — stamp with current reliable ACKs and ping */
    {
        InputPacket stamped = *input;
        stamped.eventAck = c->reliableEventAck;
        stamped.mapEventAck = c->mapEventAck;
        stamped.controlEventAck = c->controlEventAck;
        stamped.pingMs = c->pingMs;
        stamped.viewTick = clientSimGetViewTick(c->clientSim);
        c->inputRing[c->inputRingCount % CLIENT_INPUT_RING_SIZE] = stamped;
        c->lastSentInputTick = stamped.tick;
    }
    c->inputRingCount++;
}

/* Transport.recordInput: stamp the input into the ring, then — if its
 * sampled controls differ from the previously recorded input — send one
 * packet immediately. The continuous 50/s game-tick send is the loss
 * channel and is untouched; this is the latency win on press/release
 * edges, where waiting up to a tick for the next cadence send is the
 * avoidable cost. The extra copy is free on the wire: the server's
 * tick > lastProcessedInput dedup discards it as stale if the cadence
 * send (or a redundant copy) already carried that tick. */
static void udpClientRecordInput(void *ctx, const InputPacket *input) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;
    bool hadPrev = c->inputRingCount > 0;
    InputPacket prev = {0};

    if (hadPrev) {
        prev = c->inputRing[(c->inputRingCount - 1) % CLIENT_INPUT_RING_SIZE];
    }

    udpClientRecordInputInternal(c, input);

    /* First input after connect has no predecessor to compare against —
     * the cadence carries it. Promotion only fires once connected, which
     * the internal record's guard above already enforced (the ring count
     * only advances in UDP_CLIENT_CONNECTED). */
    if (hadPrev && c->joinState == UDP_CLIENT_CONNECTED &&
        udpInputEdgeChanged(&prev, input)) {
        uint8_t buf[UDP_MAX_PAYLOAD];
        int len = buildInputPacket(c, buf);
        udpClientSendTo(c, buf, len);
    }
}

/* Client sendInput: record input and send packet with redundancy to server */
static void udpClientSendInput(void *ctx, const InputPacket *input) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;
    uint8_t buf[UDP_MAX_PAYLOAD];
    int len;

    /* Record without promotion: a game-tick send already emits the packet
     * below, so routing through the edge-checking wrapper would send twice
     * whenever a change lands on a game tick. */
    udpClientRecordInputInternal(c, input);

    if (c->joinState != UDP_CLIENT_CONNECTED) {
        return;
    }

    len = buildInputPacket(c, buf);
    udpClientSendTo(c, buf, len);
}

/* Pack every entry in [outHeadSeq, outTailSeq) into a single
 * PACKET_COMMAND_TICK and send. Updates each entry's lastSentMs. */
static void udpClientDrainCommandQueue(TransportUdpClientCtx *c) {
    if (c->outHeadSeq == c->outTailSeq) return;
    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    uint8_t buf[1400];
    packHeader(buf, PACKET_COMMAND_TICK, c->outSequence++);
    size_t pos = PACKET_HEADER_SIZE + 1;  /* +1 for count placeholder */
    uint8_t count = 0;
    uint32_t now = (uint32_t)SDL_GetTicks();
    for (uint32_t seq = c->outHeadSeq; seq != c->outTailSeq; seq++) {
        OutCmdEntry *e = &c->outCmdQueue[seq % OUT_CMD_QUEUE_CAP];
        uint8_t entry[COMMAND_MAX_WIRE_BYTES];
        size_t entryLen;
        if (!commandCodecEncode(&e->cmd, entry, sizeof(entry), &entryLen)) {
            continue;
        }
        if (pos + 2 + entryLen > sizeof(buf)) break;
        packU16(buf + pos, (uint16_t)entryLen);
        pos += 2;
        memcpy(buf + pos, entry, entryLen);
        pos += entryLen;
        e->lastSentMs = now;
        count++;
        if (count == 255) break;
    }
    buf[PACKET_HEADER_SIZE] = count;
    if (count > 0) udpClientSendTo(c, buf, (int)pos);
}

void transportUdpClientSubmitCommand(Transport *t, const ClientCommand *cmd) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (c->outTailSeq - c->outHeadSeq >= OUT_CMD_QUEUE_CAP) {
        SDL_assert(0 && "out command queue full");
        return;
    }
    bool wasEmpty = (c->outHeadSeq == c->outTailSeq);
    uint32_t seq = c->outCmdNextSeq++;
    OutCmdEntry *e = &c->outCmdQueue[seq % OUT_CMD_QUEUE_CAP];
    e->cmd = *cmd;
    e->cmd.cmdSeq = seq;
    e->lastSentMs = 0;
    c->outTailSeq = seq + 1;
    if (wasEmpty) udpClientDrainCommandQueue(c);
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
        WB_LOG_WARN(WB_LOG_CAT_NET,
                "localized payload truncated (need 3 hdr bytes, len=%d pos=%d)",
                len, pos);
        return false;
    }
    id16 = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
    pos += 2;
    argCount = buf[pos++];
    if (id16 == 0) {
        WB_LOG_WARN(WB_LOG_CAT_NET, "localized payload langid=0");
        return false;
    }
    if (argCount > 4) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                "localized payload argCount=%u exceeds 4",
                argCount);
        return false;
    }
    memset(outArgs, 0, sizeof(*outArgs));
    for (i = 0; i < argCount; i++) {
        uint8_t aLen;
        char *dst = NULL;
        size_t cap = 0;
        if (pos + 1 > len) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                    "localized payload truncated at arg %d lenByte",
                    i);
            return false;
        }
        aLen = buf[pos++];
        if (pos + aLen > len) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                    "localized payload truncated: arg %d aLen=%u",
                    i, aLen);
            return false;
        }
        if (aLen >= PLAYER_NAME_LEN) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                    "localized payload arg %d aLen=%u exceeds %d",
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

/* Apply one PACKET_LOBBY_MAP_LIST_RSP chunk to the client's accumulator.
 * Wire format:
 *   [header 8] [pathLen 1] [path N] [final 1] [count 1]
 *   per entry: [nameLen 1][name M][isFolder 1][modTime 8 BE].
 * Server may emit multiple chunks per request — append entries and only
 * flip Ready/InFlight on the final chunk. Stale chunks (path mismatched
 * against the in-flight request) are silently dropped.
 *
 * Declared in transport_udp.h so unit tests can drive the accumulator
 * directly without standing up a full TransportUdpClientCtx. */
void udpClientHandleLobbyMapListRsp(ClientSim *cs,
                                    const uint8_t *buf, int len) {
    if (!cs) return;
    if (len < PACKET_HEADER_SIZE + 1) return;
    int pos = PACKET_HEADER_SIZE;
    uint8_t plen = buf[pos++];
    if (pos + plen + 2 > len) return;
    char rspPath[256];
    memset(rspPath, 0, sizeof(rspPath));
    if (plen > 0) {
        if (plen >= sizeof(rspPath)) plen = (uint8_t)(sizeof(rspPath) - 1);
        memcpy(rspPath, buf + pos, plen);
    }
    pos += plen;
    uint8_t finalFlag = buf[pos++];
    uint8_t cnt = buf[pos++];

    if (strncmp(rspPath, cs->lobbyMapListReqPath,
                sizeof(cs->lobbyMapListReqPath)) != 0) {
        return;
    }

    memset(cs->lobbyMapListPath, 0, sizeof(cs->lobbyMapListPath));
    SDL_strlcpy(cs->lobbyMapListPath, rspPath,
                sizeof(cs->lobbyMapListPath));
    for (int i = 0; i < cnt && pos < len; i++) {
        if (pos + 1 > len) break;
        uint8_t nameLen = buf[pos++];
        if (nameLen >= LOBBY_MAP_LIST_NAME_LEN ||
            pos + nameLen + 1 + 8 > len) break;
        if (cs->lobbyMapListCount >= LOBBY_MAP_LIST_MAX) {
            pos += nameLen + 1 + 8;
            continue;
        }
        int idx = cs->lobbyMapListCount++;
        memset(cs->lobbyMapListNames[idx], 0, LOBBY_MAP_LIST_NAME_LEN);
        if (nameLen > 0) {
            memcpy(cs->lobbyMapListNames[idx], buf + pos, nameLen);
        }
        pos += nameLen;
        cs->lobbyMapListIsFolder[idx] = buf[pos++];
        uint64_t mt = 0;
        for (int b = 0; b < 8; b++) {
            mt = (mt << 8) | buf[pos++];
        }
        cs->lobbyMapListModTime[idx] = (int64_t)mt;
    }
    if (finalFlag) {
        cs->lobbyMapListReady = true;
        cs->lobbyMapListInFlight = false;
    }
}

/* PACKET_LOBBY_MAP_PREVIEW_BEGIN — server announces the upcoming byte
 * stream. Wire: [header 8] [pathLen 1] [path N] [seq 1] [total 4 BE].
 * Resets the accumulator and records the seq the chunks will carry. A
 * BEGIN whose path doesn't match the in-flight request is ignored
 * (the user navigated away before this arrived). */
void udpClientHandleLobbyMapPreviewBegin(ClientSim *cs,
                                         const uint8_t *buf, int len) {
    if (!cs) return;
    int pos = PACKET_HEADER_SIZE;
    if (pos + 1 > len) return;
    uint8_t plen = buf[pos++];
    if (plen == 0 || pos + plen + 1 + 4 > len) return;
    char path[256];
    memset(path, 0, sizeof(path));
    uint8_t cp = plen;
    if (cp >= sizeof(path)) cp = (uint8_t)(sizeof(path) - 1);
    memcpy(path, buf + pos, cp);
    pos += plen;
    uint8_t seq = buf[pos++];
    uint32_t total = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16) |
                     ((uint32_t)buf[pos + 2] << 8) | (uint32_t)buf[pos + 3];
    pos += 4;

    if (strncmp(path, cs->lobbyMapPreviewReqPath,
                sizeof(cs->lobbyMapPreviewReqPath)) != 0) {
        return;
    }
    if (total == 0 || total > LOBBY_MAP_UPLOAD_MAX_BYTES) {
        cs->lobbyMapPreviewError    = true;
        cs->lobbyMapPreviewInFlight = false;
        return;
    }
    cs->lobbyMapPreviewSeq      = seq;
    cs->lobbyMapPreviewTotal    = total;
    cs->lobbyMapPreviewReceived = 0;
    cs->lobbyMapPreviewReady    = false;
    cs->lobbyMapPreviewError    = false;
    cs->lobbyMapPreviewInFlight = true;
    memset(cs->lobbyMapPreviewPath, 0, sizeof(cs->lobbyMapPreviewPath));
    SDL_strlcpy(cs->lobbyMapPreviewPath, path,
                sizeof(cs->lobbyMapPreviewPath));
}

/* PACKET_LOBBY_MAP_PREVIEW_CHUNK — one slice of the byte stream. Wire:
 * [header 8] [seq 1] [offset 4 BE] [len 2 BE] [bytes len]. Stale
 * chunks (seq mismatch, or no BEGIN seen) are dropped. Completion is
 * by cumulative byte count — the server fires each chunk once with no
 * retransmit, so the running total reaching `total` means every chunk
 * landed regardless of arrival order. */
void udpClientHandleLobbyMapPreviewChunk(ClientSim *cs,
                                         const uint8_t *buf, int len) {
    if (!cs) return;
    int pos = PACKET_HEADER_SIZE;
    /* Fixed header is generated; the byte-stream reassembly below stays
     * hand-written. */
    MapPreviewChunkHeader hdr;
    if (unpackMapPreviewChunkHeader(buf + pos, (size_t)(len - pos), &hdr) == 0) return;
    uint8_t seq = hdr.seq; uint32_t off = hdr.offset; uint16_t n = hdr.len;
    pos += 7;
    if (pos + n > len) return;

    if (!cs->lobbyMapPreviewInFlight || cs->lobbyMapPreviewError) return;
    if (seq != cs->lobbyMapPreviewSeq) return;
    if ((uint64_t)off + n > cs->lobbyMapPreviewTotal) return;
    if ((uint64_t)off + n > LOBBY_MAP_UPLOAD_MAX_BYTES) return;

    memcpy(cs->lobbyMapPreviewBytes + off, buf + pos, n);
    cs->lobbyMapPreviewReceived += n;
    if (cs->lobbyMapPreviewReceived >= cs->lobbyMapPreviewTotal) {
        cs->lobbyMapPreviewReceived = cs->lobbyMapPreviewTotal;
        cs->lobbyMapPreviewReady    = true;
        cs->lobbyMapPreviewInFlight = false;
    }
}

/* PACKET_LOBBY_MAP_PREVIEW_ERR — server couldn't read the map. Wire:
 * [header 8] [pathLen 1] [path N] [code 1]. Flags the request failed
 * so the chooser shows "no preview" instead of spinning. */
void udpClientHandleLobbyMapPreviewErr(ClientSim *cs,
                                       const uint8_t *buf, int len) {
    if (!cs) return;
    int pos = PACKET_HEADER_SIZE;
    if (pos + 1 > len) return;
    uint8_t plen = buf[pos++];
    if (pos + plen > len) return;
    char path[256];
    memset(path, 0, sizeof(path));
    uint8_t cp = plen;
    if (cp >= sizeof(path)) cp = (uint8_t)(sizeof(path) - 1);
    memcpy(path, buf + pos, cp);
    if (strncmp(path, cs->lobbyMapPreviewReqPath,
                sizeof(cs->lobbyMapPreviewReqPath)) != 0) {
        return;
    }
    cs->lobbyMapPreviewError    = true;
    cs->lobbyMapPreviewReady    = false;
    cs->lobbyMapPreviewInFlight = false;
}

/* Apply one PACKET_LOBBY_MAP_SEARCH_RSP chunk to the client's search
 * accumulator.
 * Wire format:
 *   [header 8] [pathLen 1] [path N] [queryLen 1] [query M]
 *   [final 1] [count 1]
 *   per entry: [nameLen 1][name M][isFolder 1][modTime 8 BE].
 * Chunks repeat the full path+query prefix; stale chunks are dropped
 * by matching against (reqPath, reqQuery). */
void udpClientHandleLobbyMapSearchRsp(ClientSim *cs,
                                      const uint8_t *buf, int len) {
    if (!cs) return;
    if (len < PACKET_HEADER_SIZE + 1) return;
    int pos = PACKET_HEADER_SIZE;
    uint8_t plen = buf[pos++];
    if (pos + plen + 2 > len) return;
    char rspPath[256];
    memset(rspPath, 0, sizeof(rspPath));
    if (plen > 0) {
        if (plen >= sizeof(rspPath)) plen = (uint8_t)(sizeof(rspPath) - 1);
        memcpy(rspPath, buf + pos, plen);
    }
    pos += plen;
    uint8_t qlen = buf[pos++];
    if (pos + qlen + 2 > len) return;
    char rspQuery[128];
    memset(rspQuery, 0, sizeof(rspQuery));
    if (qlen > 0) {
        if (qlen >= sizeof(rspQuery)) qlen = (uint8_t)(sizeof(rspQuery) - 1);
        memcpy(rspQuery, buf + pos, qlen);
    }
    pos += qlen;
    uint8_t finalFlag = buf[pos++];
    uint8_t cnt = buf[pos++];

    if (strncmp(rspPath, cs->lobbyMapSearchReqPath,
                sizeof(cs->lobbyMapSearchReqPath)) != 0 ||
        strncmp(rspQuery, cs->lobbyMapSearchReqQuery,
                sizeof(cs->lobbyMapSearchReqQuery)) != 0) {
        return;
    }

    memset(cs->lobbyMapSearchPath, 0, sizeof(cs->lobbyMapSearchPath));
    SDL_strlcpy(cs->lobbyMapSearchPath, rspPath,
                sizeof(cs->lobbyMapSearchPath));
    memset(cs->lobbyMapSearchQuery, 0, sizeof(cs->lobbyMapSearchQuery));
    SDL_strlcpy(cs->lobbyMapSearchQuery, rspQuery,
                sizeof(cs->lobbyMapSearchQuery));
    for (int i = 0; i < cnt && pos < len; i++) {
        if (pos + 1 > len) break;
        uint8_t nameLen = buf[pos++];
        if (nameLen >= LOBBY_MAP_LIST_NAME_LEN ||
            pos + nameLen + 1 + 8 > len) break;
        if (cs->lobbyMapSearchCount >= LOBBY_MAP_LIST_MAX) {
            pos += nameLen + 1 + 8;
            continue;
        }
        int idx = cs->lobbyMapSearchCount++;
        memset(cs->lobbyMapSearchNames[idx], 0, LOBBY_MAP_LIST_NAME_LEN);
        if (nameLen > 0) {
            memcpy(cs->lobbyMapSearchNames[idx], buf + pos, nameLen);
        }
        pos += nameLen;
        cs->lobbyMapSearchIsFolder[idx] = buf[pos++];
        uint64_t mt = 0;
        for (int b = 0; b < 8; b++) {
            mt = (mt << 8) | buf[pos++];
        }
        cs->lobbyMapSearchModTime[idx] = (int64_t)mt;
    }
    if (finalFlag) {
        cs->lobbyMapSearchReady = true;
        cs->lobbyMapSearchInFlight = false;
    }
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
    case CTRL_COMMAND_REJECTED: return "COMMAND_REJECTED";
    case CTRL_BALANCE_FAILED:   return "BALANCE_FAILED";
    case CTRL_SHELL_DEATH:      return "SHELL_DEATH";
    default:                    return "<unknown>";
    }
}

/* Snapshot-time ordered dispatch for control events arriving on the
 * snapshot tail. Almost all variants forward to clientSimApplyControl;
 * the lobby→running flip carries side-effects that previously lived
 * inside the standalone PACKET_GAME_START handler (install buffered
 * map, reset all three reliable-event acks, clear the input ring, drop
 * any pre-flip snapshot) and they must fire BEFORE the same snapshot's
 * game-event and map-event tails are applied. Those side effects and
 * the skip-prior-tails signal only apply on a real lobby→running flip;
 * a no-lobby joiner's first event is also CTRL_GAME_PHASE_RUNNING (a
 * sync-replay echo from serverSimFillGamePhaseEvent), and for that
 * joiner the same snapshot's tails are current-game state that must
 * not be dropped. We capture wasInLobby up front and gate on it. */
static void clientSimApplyControlOrdered(TransportUdpClientCtx *c,
                                         const ControlEvent *evt,
                                         uint32_t evSeq,
                                         bool *skipPriorGameTails) {
    {
        char extra[256];
        extra[0] = '\0';
        if (evt->type == CTRL_LOBBY_SLOT) {
            snprintf(extra, sizeof(extra),
                     " lobbySlot[player=%d team=%d ready=%d connected=%d isBot=%d name='%.12s']",
                     (int)evt->u.lobbySlot.playerNum,
                     (int)evt->u.lobbySlot.slot.teamNumber,
                     (int)evt->u.lobbySlot.slot.ready,
                     (int)evt->u.lobbySlot.slot.connected,
                     (int)evt->u.lobbySlot.slot.isBot,
                     evt->u.lobbySlot.slot.playerName);
        } else if (evt->type == CTRL_PLAYER_JOIN) {
            snprintf(extra, sizeof(extra),
                     " playerJoin[player=%d name='%.16s']",
                     (int)evt->u.playerJoin.playerNum,
                     evt->u.playerJoin.name);
        } else if (evt->type == CTRL_LOBBY_SETTINGS) {
            snprintf(extra, sizeof(extra),
                     " settings[map='%.16s' gameType=%d hiddenMines=%d aiType=%d timeLimit=%d startDelay=%d open=%d autoLock=%d ranked=%d allowNew=%d locks=0x%04x]",
                     evt->u.lobbySettings.mapName,
                     (int)evt->u.lobbySettings.lobbyGameType,
                     (int)evt->u.lobbySettings.lobbyHiddenMines,
                     (int)evt->u.lobbySettings.lobbyAiType,
                     (int)evt->u.lobbySettings.lobbyTimeLimit,
                     (int)evt->u.lobbySettings.lobbyStartDelay,
                     (int)evt->u.lobbySettings.lobbyOpenHost,
                     (int)evt->u.lobbySettings.lobbyAutoLockOnGameStart,
                     (int)evt->u.lobbySettings.lobbyRanked,
                     (int)evt->u.lobbySettings.lobbyAllowNewPlayers,
                     (unsigned)evt->u.lobbySettings.lobbyServerLocks);
        }
        mpDiagLog("[cli] APPLY evSeq=%u type=%s%s inLobby=%d mapInstalled=%d",
                  (unsigned)evSeq, mpDiagCtrlName((int)evt->type), extra,
                  (int)c->clientSim->inLobby, (int)c->mapInstalled);
    }
    if (evt->type == CTRL_GAME_PHASE_RUNNING) {
        /* Capture before any side effects or the dispatch —
         * clientSimApplyControl clears inLobby on CTRL_GAME_PHASE_RUNNING. */
        bool wasInLobby = c->clientSim->inLobby;
        if (wasInLobby && !c->mapInstalled &&
            c->mapDownloadBuf != NULL &&
            c->mapDownloadReceived == c->mapDownloadTotal) {
            installCompressedMap(c->clientSim, c->mapDownloadBuf,
                                 (int)c->mapDownloadTotal, NULL);
            c->mapInstalled = true;
        }
        if (wasInLobby) {
            /* The reliable-event acks are NOT reset at the lobby→running
             * flip.  The server keeps its reliable-queue sequence counters
             * monotonic across game start — it drops the previous game's
             * unacked events (ackedSeq = nextSeq) but never reuses low seq
             * numbers — so reliableEventAck / mapEventAck / controlEventAck
             * stay valid in the same sequence space.  A stale in-flight
             * lobby packet now carries seq numbers below the continuing ack
             * and dedups harmlessly instead of jumping the ack back into the
             * dead lobby space.  (lastSentControlAck likewise stays
             * monotonic — resetting it would desync the coalesced-ack
             * resend logic from controlEventAck.) */
            /* Drop any pending coalesced standalone CONTROL_ACK: during
             * running the input piggyback carries controlEventAck, so the
             * standalone-ack path hands off here.  Harmless either way — a
             * fresh ack still ships if the server keeps retransmitting. */
            c->controlAckPendingTick = 0;
            /* Reset input ring so stale inputs from the previous game are
             * not sent as redundant packets in the new game. */
            c->inputRingCount = 0;
            /* Drop any pre-flip snapshot still buffered in hasSnapshot. */
            c->hasSnapshot = false;
        }
        /* Applying RUNNING through the reliable control sequence means the
         * client is now caught up in the running sequence space, so mark it
         * adopted.  This covers the direct-into-running joiner (controlEventAck
         * was already at seq 1, so RESET-DETECT's baseSeq < ack test never
         * tripped and never set the flag): without it, a retransmitted
         * join-sync tail would re-trip RESET-DETECT and re-apply the whole
         * sequence.  The out-of-band RUNNING delivery that flips inLobby early
         * does not reach this path (it bypasses the reliable ack machinery),
         * so the snapshot-tail snap is still reached when the genuine wipe
         * needs it. */
        c->runningSeqAdopted = true;
        /* Dispatch the event itself — flips netStat to running, clears
         * inLobby on the sim, etc. The no-lobby joiner still needs this
         * to flip netStat → netRunning even though wasInLobby is false. */
        clientSimApplyControl(c->clientSim, evt);
        if (skipPriorGameTails != NULL && wasInLobby) {
            *skipPriorGameTails = true;
        }
        return;
    }
    /* Clear the running-sequence adoption flag on the reverse transition so
     * the next game-start wipe re-adopts the fresh seq space.  Round-end
     * (running → game-over → lobby) does NOT wipe the server's control
     * queue — only transportUdpServerOnGameStart does — so clearing here
     * just re-arms RESET-DETECT for the next wipe.  No seq-1 RUNNING is
     * published until that wipe, so this can't trigger a spurious re-snap
     * within the current sequence space. */
    if (evt->type == CTRL_GAME_PHASE_GAME_OVER ||
        evt->type == CTRL_GAME_PHASE_LOBBY) {
        c->runningSeqAdopted = false;
    }
    /* Default path — identical to the legacy direct-dispatch route. */
    clientSimApplyControl(c->clientSim, evt);
}

/* Map resync (desync recovery) timing/limits. localTick runs at 100/s. */
#define MAP_RESYNC_REQUEST_RESEND_TICKS 75   /* ~0.75s between request resends */
#define MAP_RESYNC_GRACE_TICKS         1000  /* ~10s = 2 full-sync intervals */
#define MAP_RESYNC_MAX_ATTEMPTS        10    /* give up + disconnect after this */
#define MAP_RESYNC_STALL_TICKS         500   /* ~5s of no chunk progress -> abandon
                                              * the in-flight resync (server stopped
                                              * sending / state lost) so the next
                                              * checksum mismatch re-arms */

/* Free the parallel resync buffer and its chunk bitfield. */
static void udpClientFreeResyncBuf(TransportUdpClientCtx *c) {
    if (c->mapResyncBuf != NULL) {
        free(c->mapResyncBuf);
        c->mapResyncBuf = NULL;
    }
    if (c->mapResyncChunkReceived != NULL) {
        free(c->mapResyncChunkReceived);
        c->mapResyncChunkReceived = NULL;
    }
    c->mapResyncTotal = 0;
    c->mapResyncChunksExpected = 0;
    c->mapResyncChunksReceived = 0;
}

/* Abandon any in-flight resync (e.g. a wholesale map change supersedes it).
 * Keeps mapResyncCount (cumulative) and resyncGenCounter (monotonic). */
static void udpClientResetResync(TransportUdpClientCtx *c) {
    udpClientFreeResyncBuf(c);
    c->resyncActive = false;
    c->firstResyncChunkSeen = false;
    c->activeResyncGen = 0;
    c->resyncAttempts = 0;
    c->resyncSuppressUntilTick = 0;
}

/* Fresh nonzero resync generation id (wrap-skips 0). */
static uint32_t udpClientNextResyncGen(TransportUdpClientCtx *c) {
    c->resyncGenCounter++;
    if (c->resyncGenCounter == 0) c->resyncGenCounter = 1;
    return c->resyncGenCounter;
}

static void udpClientSendMapResyncRequest(TransportUdpClientCtx *c, uint32_t gen) {
    uint8_t reqBuf[PACKET_HEADER_SIZE + 4];
    packHeader(reqBuf, PACKET_MAP_RESYNC_REQUEST, c->outSequence++);
    packU32(reqBuf + PACKET_HEADER_SIZE, gen);
    udpClientSendTo(c, reqBuf, sizeof(reqBuf));
}

/* Handle a resync map chunk (resyncGen != 0). Reassembles into the parallel
 * buffer and hot-swaps the terrain in on the last chunk. */
static void udpClientHandleResyncChunk(TransportUdpClientCtx *c, uint32_t gen,
                                       uint32_t mapSize, uint16_t chunkIdx,
                                       uint16_t chunkSize, const uint8_t *data) {
    uint32_t offset;

    /* Only accept chunks for the request we currently have outstanding —
     * a stale gen from a superseded resync is dropped. */
    if (!c->resyncActive || gen != c->activeResyncGen) return;
    if (mapSize == 0 || mapSize > MAP_DOWNLOAD_MAX_SIZE) return;

    /* Allocate the buffer from the self-describing mapSize on the first chunk. */
    if (c->mapResyncBuf == NULL) {
        c->mapResyncTotal = mapSize;
        c->mapResyncChunksExpected =
            (uint16_t)((mapSize + MAP_DOWNLOAD_CHUNK_SIZE - 1) / MAP_DOWNLOAD_CHUNK_SIZE);
        c->mapResyncChunksReceived = 0;
        c->mapResyncBuf = (BYTE *)malloc(mapSize);
        c->mapResyncChunkReceived =
            (bool *)calloc(c->mapResyncChunksExpected, sizeof(bool));
        if (c->mapResyncBuf == NULL || c->mapResyncChunkReceived == NULL) {
            udpClientFreeResyncBuf(c);
            return;
        }
    }

    if (chunkIdx >= c->mapResyncChunksExpected) return;
    offset = (uint32_t)chunkIdx * MAP_DOWNLOAD_CHUNK_SIZE;
    if (offset + chunkSize > c->mapResyncTotal) return;

    /* First chunk seen: the request got through, stop resending it (the
     * server's own unacked-chunk loop carries the rest). */
    c->firstResyncChunkSeen = true;
    /* Forward progress — reset the stall watchdog. */
    c->lastResyncProgressTick = c->localTick;

    memcpy(c->mapResyncBuf + offset, data, chunkSize);
    if (!c->mapResyncChunkReceived[chunkIdx]) {
        c->mapResyncChunkReceived[chunkIdx] = true;
        c->mapResyncChunksReceived++;
    }

    /* Ack this chunk via the normal MAP_ACK path the join download uses. */
    {
        uint8_t ackBuf[PACKET_HEADER_SIZE + 2];
        packHeader(ackBuf, PACKET_MAP_ACK, c->outSequence++);
        packU16(ackBuf + PACKET_HEADER_SIZE, chunkIdx);
        udpClientSendTo(c, ackBuf, sizeof(ackBuf));
    }

    if (c->mapResyncChunksReceived >= c->mapResyncChunksExpected) {
        /* Atomic terrain swap — joinState never changed, so snapshots and
         * entities kept flowing. NULL name keeps the current map name. The
         * server-held map events (seq >= cut) flow on the next snapshots and
         * apply on top of the freshly installed blob. */
        installCompressedMap(c->clientSim, c->mapResyncBuf,
                             (int)c->mapResyncTotal, NULL);
        c->mapResyncCount++;
        c->resyncActive = false;
        c->firstResyncChunkSeen = false;
        /* Advance the installed map generation to this resync's gen before
         * clearing the transient activeResyncGen. Map-channel events tagged
         * older than this are now dropped on the drain — the blob just
         * installed already carries every change up to the resync cut. */
        c->installedMapGen = c->activeResyncGen;
        c->activeResyncGen = 0;
        /* Suppress new requests for a grace window: the next full-sync
         * checksum confirms convergence and clears the backoff counter. */
        c->resyncSuppressUntilTick = c->localTick + MAP_RESYNC_GRACE_TICKS;
        udpClientFreeResyncBuf(c);
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "map resync install complete (count=%u)", (unsigned)c->mapResyncCount);
    }
}

/* Process a single incoming packet (used by both direct and delayed paths) */
static void udpClientProcessPacket(TransportUdpClientCtx *c,
                                   const uint8_t *buf, int len) {
    uint8_t pktType = getPacketType(buf, len);

    c->packetsRecvThisSec++;
    c->bytesRecvThisSec += len;

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
         *   [header 8] [playerNum 1] [serverTick 4] [mapSize 4] [connId 8]
         * Total: 8 + 9 + 8 = 25 bytes. The connId trailer is optional: an
         * old/short accept (8 + 9) leaves connId 0 and the server then
         * re-homes off IP:port instead of the connId. */
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "PACKET_JOIN_ACCEPT received: state=%d len=%d (need>=%d)",
            (int)c->joinState, len, PACKET_HEADER_SIZE + 9);
        if ((c->joinState == UDP_CLIENT_JOINING ||
             c->joinState == UDP_CLIENT_DOWNLOADING_MAP) &&
            len >= PACKET_HEADER_SIZE + 9) {
            int pos = PACKET_HEADER_SIZE;
            uint32_t mapSize;
            BYTE assignedSlot = buf[pos++];

            /* A valid server only ever assigns slots 0..MAX_TANKS-1. An
             * out-of-range slot from a hostile or buggy server would make
             * myPlayerNum index the player/tank/lobby arrays out of bounds
             * throughout the client — reject the join instead. */
            if (assignedSlot >= MAX_TANKS) {
                c->joinState = UDP_CLIENT_ERROR;
                break;
            }
            c->playerNum = assignedSlot;

            /* Slot-assignment funnel — same function the SP
             * local-transport path calls.  Both transports MUST funnel
             * here so neither can drift.  clientType/clientFlags are
             * placeholders; the authoritative values arrive via
             * CTRL_PLAYER_JOIN during the subscriber's sync replay
             * (clientSimApplyControl(CTRL_PLAYER_JOIN) writes the
             * server-authoritative type/flags onto the Players
             * struct). */
            clientSimOnAssignedSlot(c->clientSim, c->playerNum,
                                    c->playerName, 0, 0);

            /* Seed the timing estimator's clock offset from the accept's
             * serverTick (the snapshot-derived estimate refines it shortly).
             * No clean join round-trip is measured here — the JOIN handshake
             * can span challenge/cookie retries — so pass 0 rather than
             * invent an RTT sample. */
            clientTimingSeedFromJoin(&c->timing, unpackU32(buf + pos), 0);
            pos += 4;
            mapSize = unpackU32(buf + pos);
            pos += 4;

            /* Optional connId trailer — only read when the accept is long
             * enough, else leave c->connId 0 (server falls back to IP:port). */
            if (len >= PACKET_HEADER_SIZE + 9 + 8) {
                c->connId = unpackConnId(buf + pos);
                pos += 8;
            }

            if (mapSize == 0 || mapSize > MAP_DOWNLOAD_MAX_SIZE) {
                c->joinState = UDP_CLIENT_ERROR;
                break;
            }

            /* Allocate map download buffer. A fresh allocation also resets
             * mapInstalled — snapshots stay gated until the new buffer is
             * applied (covers the mid-lobby PACKET_LOBBY_MAP_CHANGE swap,
             * which routes through JOIN_REQUEST → JOIN_ACCEPT). */
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
            c->mapInstalled = false;

            /* A fresh full download (initial join or a wholesale map change)
             * supersedes any in-flight resync — drop its buffer and state so a
             * stale resync can't install over the new map or wedge detection. */
            udpClientResetResync(c);

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

    case PACKET_MAP_DOWNLOAD: {
        /* Map chunk format:
         *   [header 8] [resyncGen 4] [mapSize 4] [chunkIdx 2] [chunkSize 2] [data...]
         * resyncGen 0 = join download; nonzero = a map-resync transfer. */
        uint32_t resyncGen;
        uint32_t chunkMapSize;
        uint16_t chunkIdx;
        uint16_t chunkSize;
        const uint8_t *chunkData;
        uint32_t offset;

        /* Fixed header is generated; the data pointer and chunk reassembly
         * below stay hand-written. */
        MapDownloadChunkHeader hdr;
        if (unpackMapDownloadChunkHeader(buf + PACKET_HEADER_SIZE,
                                         (size_t)(len - PACKET_HEADER_SIZE), &hdr) == 0) break;
        resyncGen = hdr.resyncGen; chunkMapSize = hdr.mapSize;
        chunkIdx = hdr.chunkIdx;   chunkSize = hdr.chunkSize;
        chunkData    = buf + PACKET_HEADER_SIZE + 12;
        if (len < PACKET_HEADER_SIZE + 12 + chunkSize) break;

        if (resyncGen != 0) {
            /* Map-resync chunk — reassemble in the parallel buffer; the join
             * state is untouched so play continues uninterrupted. */
            udpClientHandleResyncChunk(c, resyncGen, chunkMapSize,
                                       chunkIdx, chunkSize, chunkData);
            break;
        }

        /* Join download path (resyncGen 0). Keep using JOIN_ACCEPT's size —
         * the chunk's mapSize is redundant here. */
        if (c->joinState == UDP_CLIENT_DOWNLOADING_MAP &&
            c->mapDownloadBuf != NULL) {

            if (chunkIdx >= c->mapChunksExpected) break;

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
                /* Install the buffered map immediately, whether the server
                 * is running or in the lobby. The lobby case used to defer
                 * install to the LOBBY→RUNNING transition, but that left a
                 * remote client showing the PREVIOUS round's map (its
                 * captured bases/pills) behind the lobby, while the
                 * in-process host — which installs inline on
                 * CTRL_LOBBY_MAP_CHANGE — showed the fresh neutral map. That
                 * transport split is the asymmetric-runtime bug class; both
                 * paths now reinstall on a lobby map (re)download. The
                 * LOBBY→RUNNING transition's queue/ack reset still runs; its
                 * own install step is gated on !mapInstalled, so it is a
                 * no-op now. Order: install → flag → event. */
                installCompressedMap(c->clientSim, c->mapDownloadBuf,
                                     (int)c->mapDownloadTotal, NULL);
                c->mapInstalled = true;
                {
                    ControlEvent evt = { .type = CTRL_MAP_DOWNLOAD_COMPLETE };
                    clientSimApplyControl(c->clientSim, &evt);
                }
            }
        }
        break;
    }

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
        /* Mirror into the unified accessor's source buffer so the
         * frontend's clientSimGetConnectErrorReason call returns the
         * same rendered string for both local and UDP rejects. */
        clientSimSetConnectErrorReason(c->clientSim, c->joinRejectReason);
        c->joinState = UDP_CLIENT_ERROR;
        break;
    }

    case PACKET_JOIN_CHALLENGE:
        /* [header 8][JOIN_COOKIE_LEN cookie] — server proof-of-address
         * challenge. Store the opaque cookie and resend the JOIN echoing it.
         * Only meaningful while still joining. */
        if (c->joinState == UDP_CLIENT_JOINING &&
            len >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
            memcpy(c->joinCookie, buf + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
            if (!c->haveJoinCookie) {
                /* The challenge is an extra round-trip the joiner didn't
                 * budget for; reset the retry counter once on first
                 * acquisition so it can't exhaust JOIN_MAX_RETRIES. Gated on
                 * the false→true transition so a misbehaving server replaying
                 * challenges can't loop the reset and stall the handshake. */
                c->haveJoinCookie = true;
                c->joinAttempts = 0;
            }
            c->ticksSinceJoinSent = JOIN_RETRY_INTERVAL; /* resend immediately */
        }
        break;

    case PACKET_STATE_SNAPSHOT: {
        uint32_t seq = unpackU32(buf + 4);
        int pos = PACKET_HEADER_SIZE;
        int i;
        uint8_t tankCount, shellCount, tkExplosionCount;
        uint8_t baseCount, pillCount, reliableEventCount;
        uint32_t reliableBaseSeq;
        uint8_t mapEventCount;
        uint32_t mapEventBaseSeq;
        uint8_t controlEventCount;
        uint32_t controlEventBaseSeq;
        int newEventCount = (c->hasSnapshot) ? c->snapshotHdr.reliableEventCount : 0;
        int actuallyUnpacked = 0;
        int actuallyUnpackedMap = 0;
        bool skipPriorGameTails = false;
        /* Game and map event indices into snapshotEvents where the
         * prior-game tails landed.  Used to retroactively drop those
         * entries if CTRL_GAME_PHASE_RUNNING fires on this snapshot. */
        int eventTailStartIdx = newEventCount;

        /* Ignore stale snapshots. lastSnapshotSeq stays 0 until the
         * first valid arrival and only advances forward, so it's the
         * authoritative high-water mark — independent of whether the
         * staged snapshot has been consumed yet. */
        if (c->lastSnapshotSeq != 0 && seq <= c->lastSnapshotSeq) {
            c->netErrors++;
            break;
        }

        /* The !mapInstalled gate fires AFTER the control-tail decode
         * loop below, not here.  CTRL_GAME_PHASE_RUNNING arriving in
         * this snapshot's control tail is what installs the buffered
         * map for a lobby joiner (via clientSimApplyControlOrdered);
         * gating on mapInstalled before decoding would deadlock that
         * path.  The post-decode check still abandons the snapshot —
         * scratch arrays go unused and get overwritten by the next
         * decode — covering the asymmetric-arrival case (mid-lobby
         * map swap, joiner whose first PHASE_RUNNING is still pending). */

        /* Header: serverTick(4) + lastProcessedInput(4) + tankCount(1)
         * + shellCount(1) + tkExplosionCount(1)
         * + baseCount(1) + pillCount(1)
         * + reliableEventCount(1) + reliableBaseSeq(4)
         * + mapEventCount(1) + mapEventBaseSeq(4)
         * + controlEventCount(1) + controlEventBaseSeq(4)
         * + mapChecksum(2) + returnToLobbyTicks(2) = 32 bytes */
        if (len < pos + 32) { c->netErrors++; break; }

        c->snapshotHdr.serverTick = unpackU32(buf + pos);
        pos += 4;
        c->snapshotHdr.lastProcessedInput = unpackU32(buf + pos);
        pos += 4;

        /* Snapshot loss accounting.  Consecutive snapshots' serverTick
         * values are 2 apart; a delta > 2 means snapshots were lost in
         * transit.  Cap absurd jumps (round restart, joined mid-game)
         * so they don't poison the per-second window. */
        if (c->lastSnapshotServerTick != 0 &&
            c->snapshotHdr.serverTick > c->lastSnapshotServerTick) {
            uint32_t delta = c->snapshotHdr.serverTick - c->lastSnapshotServerTick;
            if (delta > 2 && delta < 1000) {
                uint32_t lost = (delta / 2) - 1;
                c->snapshotsLostThisSec += lost;
                c->snapshotsLostTotal += lost;
            }
        }
        c->lastSnapshotServerTick = c->snapshotHdr.serverTick;
        /* Refine the timing estimator: clock offset from the header's
         * serverTick vs local arrival tick, pipeline depth from the newest
         * sent input tick vs the header's lastProcessedInput (both in
         * InputPacket.tick space), inter-arrival jitter from the gap since the
         * previous snapshot's local arrival tick. */
        clientTimingOnSnapshot(&c->timing, c->snapshotHdr.serverTick,
                               c->snapshotHdr.lastProcessedInput,
                               c->lastSentInputTick, c->localTick);
        c->snapshotsRecvThisSec++;
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
        controlEventCount = buf[pos++];
        controlEventBaseSeq = unpackU32(buf + pos);
        pos += 4;
        c->snapshotHdr.controlEventCount = controlEventCount;
        c->snapshotHdr.controlEventBaseSeq = controlEventBaseSeq;
        c->snapshotHdr.mapChecksum = unpackU16(buf + pos);
        pos += 2;
        c->snapshotHdr.returnToLobbyTicks = unpackU16(buf + pos);
        pos += 2;
        /* Track the seconds-remaining derived from the new value and
         * emit a one-shot newswire line each time we cross a 1-second
         * boundary downward (3 → "Returning to lobby in 3", etc.).
         * The server runs at 100 sim-ticks/sec, so 1 second = 100
         * snapshot-header units. */
        {
            ClientSim *cs = c->clientSim;
            if (cs) {
                uint16_t rtl = c->snapshotHdr.returnToLobbyTicks;
                uint8_t secsNow = rtl > 0 ? (uint8_t)((rtl + 99) / 100) : 0;
                if (secsNow == 0) {
                    cs->lastReturnToLobbySecs = 0;
                } else if (cs->lastReturnToLobbySecs == 0 ||
                           secsNow < cs->lastReturnToLobbySecs) {
                    if (secsNow >= 1 && secsNow <= 3) {
                        char buf2[8];
                        snprintf(buf2, sizeof(buf2), "%u", (unsigned)secsNow);
                        clientMessageAdd(clientSimGetMessages(cs),
                                         newsWireMessage,
                                         (char *)"Server", buf2);
                    }
                    cs->lastReturnToLobbySecs = secsNow;
                }
            }
        }

        c->snapshotHdr.tankCount = tankCount;
        c->snapshotHdr.shellCount = shellCount;
        c->snapshotHdr.tkExplosionCount = tkExplosionCount;
        c->snapshotHdr.baseCount = baseCount;
        c->snapshotHdr.pillCount = pillCount;

        /* Unpack tanks — variable length: a stub is 1 byte; a full entry is a
         * presence-mask-driven run that unpackTankSnapshot length-checks
         * against the bytes remaining, returning 0 on truncation. */
        if (tankCount > MAX_TANKS) tankCount = MAX_TANKS;
        {
            bool tankBoundsOk = TRUE;
            for (i = 0; i < tankCount; i++) {
                int n = unpackTankSnapshot(buf + pos, (size_t)(len - pos),
                                           &c->snapshotTanks[i]);
                if (n == 0) { tankBoundsOk = FALSE; break; }
                pos += n;
            }
            if (!tankBoundsOk) break;
        }

        /* Unpack shells — each entry is length-checked against the bytes
         * remaining, returning 0 on truncation. */
        if (shellCount > MAX_SNAPSHOT_SHELLS) shellCount = MAX_SNAPSHOT_SHELLS;
        {
            bool shellBoundsOk = TRUE;
            for (i = 0; i < shellCount; i++) {
                int n = unpackShellSnapshot(buf + pos, (size_t)(len - pos),
                                            &c->snapshotShells[i]);
                if (n == 0) { shellBoundsOk = FALSE; break; }
                pos += n;
            }
            if (!shellBoundsOk) break;
        }

        /* Unpack tank explosions */
        if (tkExplosionCount > MAX_SNAPSHOT_TK_EXPLOSIONS) tkExplosionCount = MAX_SNAPSHOT_TK_EXPLOSIONS;
        {
            bool tkBoundsOk = TRUE;
            for (i = 0; i < tkExplosionCount; i++) {
                int n = unpackTkExplosionSnapshot(buf + pos, (size_t)(len - pos),
                                                  &c->snapshotTkExplosions[i]);
                if (n == 0) { tkBoundsOk = FALSE; break; }
                pos += n;
            }
            if (!tkBoundsOk) break;
        }

        /* Unpack bases */
        if (baseCount > MAX_SNAPSHOT_BASES) baseCount = MAX_SNAPSHOT_BASES;
        {
            bool baseBoundsOk = TRUE;
            for (i = 0; i < baseCount; i++) {
                int n = unpackBaseSnapshot(buf + pos, (size_t)(len - pos),
                                           &c->snapshotBases[i]);
                if (n == 0) { baseBoundsOk = FALSE; break; }
                pos += n;
            }
            if (!baseBoundsOk) break;
        }

        /* Unpack pills */
        if (pillCount > MAX_SNAPSHOT_PILLS) pillCount = MAX_SNAPSHOT_PILLS;
        {
            bool pillBoundsOk = TRUE;
            for (i = 0; i < pillCount; i++) {
                int n = unpackPillSnapshot(buf + pos, (size_t)(len - pos),
                                           &c->snapshotPills[i]);
                if (n == 0) { pillBoundsOk = FALSE; break; }
                pos += n;
            }
            if (!pillBoundsOk) break;
        }

        /* Unpack reliable game events with dedup.
         * Only advance ACK based on events we actually consumed — stop
         * on truncated packet OR when the local buffer is full. */
        for (i = 0; i < reliableEventCount; i++) {
            uint32_t evSeq = reliableBaseSeq + (uint32_t)i;
            GameEvent ev;
            int n = unpackGameEvent(buf + pos, (size_t)(len - pos), &ev);
            if (n == 0) break;  /* Truncated event — stop */
            pos += n;
            actuallyUnpacked++;
            /* Only apply events we haven't seen yet */
            if (evSeq >= c->reliableEventAck) {
                if (newEventCount < MAX_SNAPSHOT_EVENTS) {
                    c->snapshotEvents[newEventCount++] = ev;
                } else {
                    break;  /* Buffer full — stop so we don't ACK unconsumed events */
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
            int n = unpackGameEvent(buf + pos, (size_t)(len - pos), &ev);
            if (n == 0) break;  /* Truncated event — stop */
            pos += n;
            actuallyUnpackedMap++;
            if (evSeq >= c->mapEventAck) {
                if (newEventCount < MAX_SNAPSHOT_EVENTS) {
                    c->snapshotEvents[newEventCount++] = ev;
                } else {
                    break;  /* Buffer full — stop so we don't ACK unconsumed events */
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

        /* Decode the control-event tail.  Events are applied EAGERLY
         * via clientSimApplyControlOrdered — they must run before the
         * snapshot's game/map tails make it to the sim so the lobby
         * →running flip can install the new map and drop any pre-flip
         * tail.  Per-event wire layout: type(1) + bodyLen(2) + body(N).
         * The body decoder is looked up by ControlEventType through the
         * body-only codec table. */
        if (controlEventCount > 0) {
            mpDiagLog("[cli] SNAPSHOT-tail recv baseSeq=%u count=%u localAck=%u",
                      (unsigned)controlEventBaseSeq, (unsigned)controlEventCount,
                      (unsigned)c->controlEventAck);
        }
        /* NOTE: now vestigial.  transportUdpServerOnGameStart no longer
         * restarts the control queue at seq 1 — it drops unacked events
         * (ackedSeq = nextSeq) but keeps nextSeq monotonic, so at game start
         * baseSeq >= controlEventAck and the baseSeq < ack test below never
         * trips.  Left in place (inert) to keep the monotonic-seq change
         * focused; a follow-up can remove the controlSeqResetDetected /
         * runningSeqAdopted machinery once the monotonic behaviour has soaked.
         *
         * Server-queue-restart detection.  Historically the server reset its
         * per-client control queue to (ackedSeq=1, nextSeq=1) inside
         * transportUdpServerOnGameStart, immediately before publishing
         * CTRL_GAME_PHASE_RUNNING — so the running flip landed at seq=1 of a
         * new sequence space.  Without intervention the dedup gate below
         * (evSeq >= controlEventAck) would filter that seq=1 out because
         * controlEventAck was still high from the lobby phase, and
         * clientSimApplyControlOrdered's own ack-reset never got to run.
         *
         * Gate on a once-per-wipe adoption flag, not inLobby.  The flag
         * fires the snap exactly once when the client first sees the
         * running reset — whether or not inLobby has already flipped under
         * loss/reorder — and blocks re-firing on every subsequent seq=1
         * RUNNING retransmit (which would otherwise cause a snap-then-reapply
         * loop that starves the main loop).  The flag is cleared on the
         * reverse game-over / lobby transition so the next game re-adopts. */
        if (c->clientSim != NULL &&
            pos + 3 <= len &&
            controlSeqResetDetected(controlEventCount, controlEventBaseSeq,
                                    c->controlEventAck, c->runningSeqAdopted,
                                    buf[pos] == (uint8_t)CTRL_GAME_PHASE_RUNNING)) {
            mpDiagLog("[cli] SNAPSHOT-tail RESET-DETECT baseSeq=%u localAck=%u (server queues restarted; snapping back)",
                      (unsigned)controlEventBaseSeq,
                      (unsigned)c->controlEventAck);
            c->controlEventAck = controlEventBaseSeq;
            c->runningSeqAdopted = true;
        }
        for (i = 0; i < controlEventCount; i++) {
            uint32_t evSeq = controlEventBaseSeq + (uint32_t)i;
            uint8_t type;
            uint16_t bodyLen;
            ControlEvent evt;
            ControlDecodeBodyFn dec;
            if (pos + 3 > len) break;          /* Truncated header */
            type = buf[pos++];
            bodyLen = unpackU16(buf + pos); pos += 2;
            if (pos + bodyLen > len) break;    /* Truncated body */
            dec = transportControlCodecBodyDecoder((ControlEventType)type);
            if (dec == NULL || !dec(buf + pos, bodyLen, &evt)) {
                mpDiagLog("[cli] SNAPSHOT-tail decode SKIP seq=%u type=%s reason=%s",
                          (unsigned)evSeq, mpDiagCtrlName((int)type),
                          dec == NULL ? "no decoder" : "decode failed");
                pos += bodyLen;
                /* Advance the ack past the skipped event. Without this,
                 * an undecodable event at the tail of the queue stalls
                 * controlEventAck and the server retransmits until the
                 * unacked-control timeout fires and disconnects both
                 * sides. The event is dropped — retransmitting won't
                 * make it decodable — but the queue stays healthy. */
                if (evSeq >= c->controlEventAck) {
                    c->controlEventAck = evSeq + 1;
                }
                continue;
            }
            pos += bodyLen;
            if (evSeq >= c->controlEventAck) {
                clientSimApplyControlOrdered(c, &evt, evSeq,
                                             &skipPriorGameTails);
                c->controlEventAck = evSeq + 1;
            } else {
                mpDiagLog("[cli] SNAPSHOT-tail dedup seq=%u type=%s localAck=%u",
                          (unsigned)evSeq, mpDiagCtrlName((int)type),
                          (unsigned)c->controlEventAck);
            }
        }

        /* Map-install gate, moved past the control-tail decode so
         * CTRL_GAME_PHASE_RUNNING in this same snapshot has a chance
         * to flip mapInstalled = true (via clientSimApplyControlOrdered)
         * before we decide to apply.  If the control tail didn't carry
         * the running flip and the joiner is still pre-install, abandon
         * the snapshot — scratch decode arrays are reused on the next
         * arrival, and we leave hasSnapshot / lastSnapshotSeq /
         * lastSnapshotTick unadvanced. */
        if (!c->mapInstalled) {
            WB_LOG_DEBUG(WB_LOG_CAT_NET,
                "snapshot dropped post-decode — map still not installed (seq=%u)",
                (unsigned)seq);
            break;
        }

        /* Ingest this snapshot's channel trailer, then drain reliable game
         * events from channel 0 into the game-tail slot — ahead of the map
         * tail staged above, so they apply in the same game-then-map order
         * the snapshot game tail used and fall under the same
         * skipPriorGameTails rollback below.  The channel guarantees in-order
         * exactly-once delivery, so no per-event ack or dedup is applied. */
        if (pos < len &&
            channelRecvFrame(&c->channelMux, buf + pos, len - pos) >= 0) {
            c->channelFramesRx++;
        }
        {
            GameEvent chanGameEv[MAX_SNAPSHOT_EVENTS];
            uint8_t chanBuf[CHANNEL_MAX_SEG];
            uint16_t chanLen;
            int chanCount = 0;
            int mapTailCount;
            while (chanCount < MAX_SNAPSHOT_EVENTS &&
                   channelReceive(&c->channelMux, CHANNEL_GAME, chanBuf, &chanLen)) {
                if (unpackGameEvent(chanBuf, chanLen, &chanGameEv[chanCount]) > 0) {
                    chanCount++;
                }
            }
            /* Drain channel 1 (map) events: payload [gen u32][GameEvent]. Stage
             * survivors onto the map tail so they sit behind the game events the
             * splice places ahead of them (game-then-map order) and fall under
             * the skipPriorGameTails rollback below. Drop any event tagged older
             * than the installed map generation — a stale change a resync already
             * superseded. */
            while (newEventCount < MAX_SNAPSHOT_EVENTS &&
                   channelReceive(&c->channelMux, CHANNEL_MAP, chanBuf, &chanLen)) {
                GameEvent mapEv;
                uint32_t evGen;
                if (chanLen < 4) continue;
                evGen = unpackU32(chanBuf);
                if (evGen < c->installedMapGen) continue;
                if (unpackGameEvent(chanBuf + 4, (size_t)(chanLen - 4), &mapEv) > 0) {
                    c->snapshotEvents[newEventCount++] = mapEv;
                }
            }
            mapTailCount = newEventCount - eventTailStartIdx;
            newEventCount = spliceGameEventsBeforeTail(
                c->snapshotEvents, eventTailStartIdx, mapTailCount,
                chanGameEv, chanCount, MAX_SNAPSHOT_EVENTS);
        }

        /* If CTRL_GAME_PHASE_RUNNING fired in this snapshot's tail, the
         * game/map events decoded earlier in the same packet are pre-
         * flip and must not reach the sim — they would replay against
         * the freshly-installed new-game map. Roll the staged-event
         * count back so they're never passed to clientSimSyncFromSnapshot. */
        if (skipPriorGameTails) {
            newEventCount = eventTailStartIdx;
        }

        c->snapshotHdr.reliableEventCount = (uint8_t)newEventCount;
        c->snapshotHdr.reliableBaseSeq = reliableBaseSeq;

        c->hasSnapshot = true;
        c->lastSnapshotSeq = seq;
        c->lastSnapshotTick = c->localTick;

        /* Apply the freshly-staged snapshot directly onto the ClientSim.
         * The frontend's per-frame clientSimNetSyncSnapshot also reads
         * via the getSnapshot vtable; that path stays for the local
         * transport's first-snapshot pull and the headless cmd-stdin
         * loop. */
        clientSimSyncFromSnapshot(c->clientSim, &c->snapshotHdr,
                                  c->snapshotTanks, c->snapshotHdr.tankCount,
                                  c->snapshotShells, c->snapshotHdr.shellCount,
                                  c->snapshotTkExplosions, c->snapshotHdr.tkExplosionCount,
                                  c->snapshotBases, c->snapshotHdr.baseCount,
                                  c->snapshotPills, c->snapshotHdr.pillCount,
                                  c->snapshotEvents, c->snapshotHdr.reliableEventCount,
                                  c->playerNum);
        c->hasSnapshot = false;   /* Consumed inline — per-frame
                                   * syncSnapshot no-ops until the
                                   * next arrival. */
        break;
    }

    case PACKET_CHANNEL:
        /* Standalone channel frame (server → client, sent when no snapshot
         * rides this tick).  Body is one frame directly after the header. */
        if (channelRecvFrame(&c->channelMux, buf + PACKET_HEADER_SIZE,
                             len - PACKET_HEADER_SIZE) >= 0) {
            c->channelFramesRx++;
            /* Drain reliable game events from channel 0 and apply them
             * directly.  A standalone frame is only sent while the game is not
             * running, so it never coincides with an in-frame running-flip and
             * needs no map-install / new-game-flip gating.  The channel
             * guarantees in-order exactly-once delivery, so no dedup is added. */
            if (c->clientSim != NULL) {
                uint8_t chanBuf[CHANNEL_MAX_SEG];
                uint16_t chanLen;
                GameEvent gev;
                while (channelReceive(&c->channelMux, CHANNEL_GAME,
                                      chanBuf, &chanLen)) {
                    if (unpackGameEvent(chanBuf, chanLen, &gev) > 0) {
                        clientSimApplyGameEvents(c->clientSim, &gev, 1,
                                                 c->playerNum);
                    }
                }
                /* Channel 1 (map) events, applied after the game events
                 * (game-then-map order). Payload [gen u32][GameEvent]; drop any
                 * tagged older than the installed map generation. */
                while (channelReceive(&c->channelMux, CHANNEL_MAP,
                                      chanBuf, &chanLen)) {
                    uint32_t evGen;
                    if (chanLen < 4) continue;
                    evGen = unpackU32(chanBuf);
                    if (evGen < c->installedMapGen) continue;
                    if (unpackGameEvent(chanBuf + 4, (size_t)(chanLen - 4),
                                        &gev) > 0) {
                        clientSimApplyGameEvents(c->clientSim, &gev, 1,
                                                 c->playerNum);
                    }
                }
            }
        }
        break;

    case PACKET_PONG:
        if (len >= PACKET_HEADER_SIZE + 8) {
            uint32_t clientTime = unpackU32(buf + PACKET_HEADER_SIZE);
            uint32_t now = SDL_GetTicks();
            if (now >= clientTime) {
                uint16_t sample = (uint16_t)(now - clientTime);
                c->pingMs = pingMinWindowPush(&c->pingMinWin, sample);
                c->pingDisplayMs = pingEwmaUpdate(&c->pingEwma, sample);
                clientTimingOnRtt(&c->timing, sample);
                /* Display consumers (scoreboard, HUD) read the EWMA; shell
                 * projection reads the min-over-window value below. */
                playersSetPing(&c->clientSim->sim.plyrs, c->playerNum,
                               c->pingDisplayMs);
                clientSimSetProjectionPing(c->clientSim, c->pingMs);
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
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
                /* Lobby-chat join message is transport-side UI, gated on
                 * not-self so the joiner doesn't announce themselves. */
                if (evt.u.playerJoin.playerNum != c->playerNum &&
                    c->clientSim->inLobby) {
                    char joinMsg[PACKET_MAX_PLAYER_NAME + 16];
                    snprintf(joinMsg, sizeof(joinMsg), "%s has joined.",
                             evt.u.playerJoin.name);
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
            mpDiagLog("[cli] PLAYER_LIST recv count=%u", (unsigned)plCount);
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
                    mpDiagLog("[cli] PLAYER_LIST entry player=%d name='%.16s' type=%d flags=0x%02x allies=%d",
                              (int)pNum, pName, (int)clientType,
                              (int)clientFlags, (int)numAllies);
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

    case PACKET_PLAYER_LEFT: {
        /* Route through the codec so in-process subscribers see the
         * CTRL_PLAYER_LEAVE event, then keep the "X has left" lobby chat
         * rendering at the wire boundary — display is the transport's
         * job, same precedent as the chat-rendering migration. */
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        if (len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME) {
            uint8_t pNum = buf[PACKET_HEADER_SIZE];
            char pName[PACKET_MAX_PLAYER_NAME];
            memcpy(pName, buf + PACKET_HEADER_SIZE + 1, PACKET_MAX_PLAYER_NAME);
            pName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
            if (pNum != c->playerNum && c->clientSim->inLobby) {
                char leaveMsg[PACKET_MAX_PLAYER_NAME + 16];
                snprintf(leaveMsg, sizeof(leaveMsg), "%s has left.", pName);
                clientSimAppendLobbyChat(c->clientSim, "***", leaveMsg);
            }
        }
        break;
    }

    case PACKET_NAME_CHANGE: {
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        break;
    }

    case PACKET_CHAT_BROADCAST: {
        /* Chat broadcast — wire format depends on fromPlayer (see netpacks.h):
         *   < MAX_TANKS  : player-to-player chat, payload is plain message
         *   == 0xFF      : server localized, payload is langid + args
         *   == 0xFE      : server raw English (transitional), payload is plain message
         *
         * Player-to-player chat is delivered to MessageState by the
         * CTRL_CHAT subscriber in client_sim_control.c — do not deliver
         * here too or the recipient sees every line twice. The 0xFE/0xFF
         * server-message branches below stay because their display path
         * (clientSimAppendLobbyChat / clientSimNetStatusMessage) is
         * transport-aware and not replicated by the in-process handler. */
        {
            ControlDecodeFn dec = transportControlCodecDecoder(pktType);
            if (dec != NULL) {
                ControlEvent evt;
                if (dec(buf + PACKET_HEADER_SIZE,
                        (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                    clientSimApplyControl(c->clientSim, &evt);
                }
            }
        }
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
            }
            /* Player-to-player case (fromPlayer < MAX_TANKS) intentionally
             * falls through with no further action: the CTRL_CHAT subscriber
             * in client_sim_control.c handles MessageState delivery for
             * every subscriber (UDP clients, host, bots) uniformly. */
        }
        break;
    }

    case PACKET_ALLIANCE_UPDATE: {
        /* [header 8][event 1][fromPlayer 1][toPlayer 1] */
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
                /* Alliance-request dialog is transport-internal UI:
                 * the server encoder already filters REQUEST so only
                 * the target client receives the wire packet, so this
                 * always fires for "us" here. */
                if (evt.type == CTRL_ALLIANCE_REQUEST &&
                    evt.u.allianceRequest.toPlayer == c->playerNum) {
                    char pName[FILENAME_MAX];
                    playersGetPlayerName(&c->clientSim->sim.plyrs,
                                         evt.u.allianceRequest.fromPlayer,
                                         pName, FALSE);
                    if (windowShowAllianceRequest() == TRUE) {
                        dialogAllianceSetName(pName,
                                              evt.u.allianceRequest.fromPlayer);
                    } else {
                        char str[FILENAME_MAX + 64];
                        snprintf(str, sizeof(str),
                                 "You have ignored alliance request from %s",
                                 pName);
                        clientMessageAdd(&c->clientSim->messages, networkStatus,
                                         "Alliance Request", str);
                    }
                }
            }
        }
        break;
    }

    case PACKET_SERVER_SHUTDOWN: {
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE, (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "PACKET_SERVER_SHUTDOWN received -> SERVER_SHUTDOWN");
        c->joinState = UDP_CLIENT_SERVER_SHUTDOWN;
        break;
    }

    case PACKET_KICKED: {
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "PACKET_KICKED received -> KICKED");
        c->joinState = UDP_CLIENT_KICKED;
        break;
    }

    case PACKET_LOBBY_UPDATE: {
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        break;
    }

    case PACKET_LOBBY_SETTINGS: {
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec == NULL) break;
        {
            ControlEvent evt;
            if (!dec(buf + PACKET_HEADER_SIZE,
                     (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                break;
            }
            clientSimApplyControl(c->clientSim, &evt);
        }
        /* Lonely lobby tracking (ACH_LONELY_LOBBY) — a settings refresh
         * marks a stable lobby state, the natural trigger for the
         * "alone in lobby" timer. */
        {
            int i;
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
                        c->clientSim->lobbyAloneStartTick = 1;
                    }
                } else {
                    uint32_t elapsed = SDL_GetTicks() - c->clientSim->lobbyAloneStartTick;
                    if (elapsed >= 3600000) {
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
        /* WBN (re-)auth is not driven from here: it is performed once per
         * server-issued session key in the PACKET_WBN_REKEY handler. The
         * client never inspects its own flag state to decide whether to
         * re-authenticate. */
        break;
    }

    case PACKET_COUNTDOWN: {
        /* [header 8] [secondsRemaining 1] */
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        break;
    }

    /* PACKET_GAME_START is no longer dispatched on the client and no
     * longer emitted by the server: the lobby→running flip arrives as
     * CTRL_GAME_PHASE_RUNNING in the snapshot control-event tail, and
     * clientSimApplyControlOrdered runs the side-effects (install map,
     * reset acks, clear input ring, drop pre-flip snapshot) before the
     * same snapshot's prior-game tails would otherwise replay against
     * the new map. */

    case PACKET_CONTROL_TICK: {
        /* Server → client reliable carrier for control events during
         * non-running phases (lobby / countdown / gameover).  Wire layout
         * matches the snapshot control-event tail: each event is
         * type(1) + bodyLen(2 BE) + body(N), and we dedup against
         * controlEventAck so retransmits don't re-apply. */
        int pos = PACKET_HEADER_SIZE;
        uint32_t baseSeq;
        uint8_t count;
        int i;
        if (len < pos + 5) break;
        baseSeq = unpackU32(buf + pos); pos += 4;
        count = buf[pos++];
        mpDiagLog("[cli] CONTROL_TICK recv baseSeq=%u count=%u localAck=%u",
                  (unsigned)baseSeq, (unsigned)count,
                  (unsigned)c->controlEventAck);
        for (i = 0; i < count; i++) {
            uint32_t evSeq = baseSeq + (uint32_t)i;
            uint8_t type;
            uint16_t bodyLen;
            ControlEvent evt;
            ControlDecodeBodyFn dec;
            if (pos + 3 > len) break;
            type = buf[pos++];
            bodyLen = unpackU16(buf + pos); pos += 2;
            if (pos + bodyLen > len) break;
            dec = transportControlCodecBodyDecoder((ControlEventType)type);
            if (dec == NULL || !dec(buf + pos, bodyLen, &evt)) {
                mpDiagLog("[cli] CONTROL_TICK decode SKIP seq=%u type=%s reason=%s",
                          (unsigned)evSeq, mpDiagCtrlName((int)type),
                          dec == NULL ? "no decoder" : "decode failed");
                pos += bodyLen;
                /* Same belt-and-suspenders as the snapshot-tail path:
                 * undecodable events at the tail of the queue otherwise
                 * stall the ack and trigger the unacked-control timeout
                 * disconnect. Drop the event but keep the queue moving. */
                if (evSeq >= c->controlEventAck) {
                    c->controlEventAck = evSeq + 1;
                }
                continue;
            }
            pos += bodyLen;
            if (evSeq >= c->controlEventAck) {
                /* PACKET_CONTROL_TICK arrives outside the snapshot
                 * context — no game/map tails ride alongside it, so
                 * skipPriorGameTails has nothing to skip. */
                clientSimApplyControlOrdered(c, &evt, evSeq, NULL);
                c->controlEventAck = evSeq + 1;
            } else {
                mpDiagLog("[cli] CONTROL_TICK dedup seq=%u type=%s localAck=%u",
                          (unsigned)evSeq, mpDiagCtrlName((int)type),
                          (unsigned)c->controlEventAck);
            }
        }
        /* Schedule a coalesced ACK — the actual send rides
         * udpClientTick's per-tick driver below. */
        if (c->controlAckPendingTick == 0) {
            c->controlAckPendingTick = c->localTick;
        }
        break;
    }

    case PACKET_COMMAND_ACK: {
        if (len < PACKET_HEADER_SIZE + 4) break;
        uint32_t highest = unpackU32(buf + PACKET_HEADER_SIZE);
        if (highest >= c->outHeadSeq) {
            c->outHeadSeq = highest + 1;
            if (c->outHeadSeq > c->outTailSeq) {
                c->outHeadSeq = c->outTailSeq;
            }
            /* Drive the "coalesce into next outgoing frame" half of the
             * plan's eager-then-coalesce contract: a submit that arrived
             * while the head was in flight (wasEmpty=false → no immediate
             * drain) waits here for the prior head to ack, then ships.
             * Without this, never-sent tail entries sit forever because
             * the retransmit timer's lastSentMs!=0 gate excludes them. */
            if (c->outHeadSeq != c->outTailSeq) udpClientDrainCommandQueue(c);
        }
        break;
    }

    case PACKET_GAME_OVER:
        /* [header 8] */
        {
            /* Synthesize the matching CTRL_GAME_PHASE_GAME_OVER
             * locally so the client's bus sees the same publish
             * order as the server (PHASE then OVER); the server
             * encoder skips PACKET_GAME_OVER for the PHASE event so
             * only the CTRL_GAME_OVER side crosses the wire. */
            ControlEvent phaseEvt = { .type = CTRL_GAME_PHASE_GAME_OVER };
            clientSimApplyControl(c->clientSim, &phaseEvt);

            ControlDecodeFn dec = transportControlCodecDecoder(pktType);
            if (dec != NULL) {
                ControlEvent overEvt;
                if (dec(buf + PACKET_HEADER_SIZE,
                        (size_t)(len - PACKET_HEADER_SIZE), &overEvt)) {
                    clientSimApplyControl(c->clientSim, &overEvt);
                }
            }
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
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE, (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
                c->joinState = UDP_CLIENT_JOINING;
                c->joinAttempts = 0;
                /* Re-prove the address: a re-join must re-acquire a cookie. */
                c->haveJoinCookie = false;
                c->ticksSinceJoinSent = JOIN_RETRY_INTERVAL; /* send immediately */
            }
        }
        break;
    }

    case PACKET_BALANCE_PROPOSAL: {
        /* [header 8] [teamForSlot × 16] */
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE, (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        break;
    }

    case PACKET_MAP_SKIP_STATE: {
        /* [header 8] [votes: 16 bytes, one per slot, 0 or 1] */
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE, (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        break;
    }

    case PACKET_GAME_VOTE_STATE: {
        /* Migrated to ControlEvent codec — wire format owned by
         * transport_control_codec.c (encoder + decoder pair). */
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        break;
    }

    /* ── Layout A lobby — server → client broadcasts ─────────────── */
    case PACKET_LOBBY_TEAM_META_CHG:
    case PACKET_LOBBY_BOT_CONFIG_CHG:
    case PACKET_LOBBY_BRAIN_LIST: {
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        break;
    }

    case PACKET_LOBBY_MAP_LIST_RSP:
        udpClientHandleLobbyMapListRsp(c->clientSim, buf, len);
        break;

    case PACKET_LOBBY_MAP_SEARCH_RSP:
        udpClientHandleLobbyMapSearchRsp(c->clientSim, buf, len);
        break;

    case PACKET_LOBBY_MAP_PREVIEW_BEGIN:
        udpClientHandleLobbyMapPreviewBegin(c->clientSim, buf, len);
        break;

    case PACKET_LOBBY_MAP_PREVIEW_CHUNK:
        udpClientHandleLobbyMapPreviewChunk(c->clientSim, buf, len);
        break;

    case PACKET_LOBBY_MAP_PREVIEW_ERR:
        udpClientHandleLobbyMapPreviewErr(c->clientSim, buf, len);
        break;

    case PACKET_LOBBY_MAP_UPLOAD_ACK: {
        /* [header 8] [status 1]. 0 = ok, non-zero = reject. */
        if (!c->clientSim || len < PACKET_HEADER_SIZE + 1) break;
        uint8_t status = buf[PACKET_HEADER_SIZE];
        if (status == 0) {
            c->clientSim->lobbyMapUploadStatus = 2;
        } else {
            c->clientSim->lobbyMapUploadStatus = 4;
            c->clientSim->lobbyMapUploadRejectCode = status;
        }
        break;
    }

    case PACKET_LOBBY_MAP_USE_LOCAL_NACK: {
        /* [header 8] [nameLen 1] [name N] — server doesn't have a
         * matching local file. Caller should fall back to a regular
         * UPLOAD_BEGIN. UI pump notices the flag next frame. */
        if (!c->clientSim || len < PACKET_HEADER_SIZE + 1) break;
        /* nameLen + name bytes are informational here (echoes the
         * client's announce), we just flip the fallback flag. */
        c->clientSim->lobbyMapUseLocalNeedsFallback = true;
        break;
    }

    case PACKET_LOBBY_MAP_UPLOAD_DONE: {
        /* [header 8] [status 1] [pathLen 1] [path N] */
        if (!c->clientSim || len < PACKET_HEADER_SIZE + 2) break;
        uint8_t status = buf[PACKET_HEADER_SIZE];
        uint8_t plen   = buf[PACKET_HEADER_SIZE + 1];
        if (len < PACKET_HEADER_SIZE + 2 + plen) break;
        if (status == 0) {
            memset(c->clientSim->lobbyMapUploadFinalPath, 0,
                   sizeof(c->clientSim->lobbyMapUploadFinalPath));
            if (plen > 0 && plen < sizeof(c->clientSim->lobbyMapUploadFinalPath)) {
                memcpy(c->clientSim->lobbyMapUploadFinalPath,
                       buf + PACKET_HEADER_SIZE + 2, plen);
            }
            c->clientSim->lobbyMapUploadStatus = 3;
        } else {
            c->clientSim->lobbyMapUploadStatus = 4;
            c->clientSim->lobbyMapUploadRejectCode = status;
        }
        break;
    }

    case PACKET_LOBBY_BOT_BRAIN_CHG: {
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        if (dec != NULL) {
            ControlEvent evt;
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        break;
    }

    case PACKET_PUNCH_REQUEST_ACK:
        /* Tracker acked our PUNCH_REQUEST. Status byte at PACKET_HEADER_SIZE
         * could drive UX someday; for now just consume so it doesn't fall
         * into the unknown-packet warning path. */
        break;

    case PACKET_WBN_REKEY: {
        /* Wire: [header 8] [serverKey WBN_JOIN_KEY_WIRE_LEN] — same 65-byte
         * envelope as wbnJoinKey for symmetry; payload is a NUL-terminated
         * string within the first WINBOLONET_KEY_LEN bytes. */
        char newKey[WINBOLONET_KEY_LEN];
        bool keyChanged;
        if (len < PACKET_HEADER_SIZE + WBN_JOIN_KEY_WIRE_LEN) {
            return;
        }
        memset(newKey, 0, sizeof(newKey));
        if (!wbnKeyDecode(newKey, buf + PACKET_HEADER_SIZE)) {
            return;
        }
        /* This is the sole driver of WBN (re-)authentication. We authenticate
         * once for each *new* session key the server hands us: the first
         * delivery right after JOIN_ACCEPT (initial auth) and again after a
         * return-to-lobby key rotation (the server's "re-auth now" signal).
         * Identical resends (e.g. a recovered JOIN_ACCEPT) leave the key
         * unchanged, so we don't re-fire and provoke an "already in game"
         * rejection. On failure we simply don't retry — the next genuine
         * rotation will trigger a fresh attempt. */
        keyChanged = (strcmp(newKey, c->wbnServerKey) != 0);
        memcpy(c->wbnServerKey, newKey, sizeof(c->wbnServerKey));
        if (keyChanged) {
            udpClientSendWbnReauth(c);
        }
        break;
    }

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

/* Receive-and-apply seam: drain the socket and apply any snapshots (and all
 * other inbound packet processing — control, PONG, handshake, map) without
 * advancing any per-tick state.  Called both from udpClientTick (where the
 * recv loop used to be) and, once per render frame, from the main-thread
 * render seam so a frame composes from the freshest snapshot.  No localTick
 * advance, resends, acks, ping, or timeouts happen here — those stay in
 * udpClientTick.  Safe to call from the render path because the pump and the
 * render run on the same serialized main thread (the SDL timers only set
 * atomics), so inbound processing here is byte-for-byte identical to running
 * it from the pump. */
static void udpClientDrainSnapshots(TransportUdpClientCtx *c) {
    uint8_t buf[UDP_MAX_PAYLOAD];
    struct sockaddr_in fromAddr;
    int len;

    /* Receive all pending packets from the wire */
    while ((len = udpRecvFrom(c->sock, buf, sizeof(buf), &fromAddr)) > 0) {
        if (netImpairEnabled(&c->impairIn)) {
            netImpairOffer(&c->impairIn, buf, len, &fromAddr,
                           (uint64_t)SDL_GetTicks());
        } else {
            udpClientProcessPacket(c, buf, len);
        }
    }

    /* Release any impaired INBOUND datagrams now due into the processor.
     * No-op while the impairment layer is disabled.  (Outbound impairment
     * release is a send and stays on the per-tick path in udpClientTick.) */
    {
        uint8_t pbuf[NET_IMPAIR_MAX_PACKET];
        struct sockaddr_in paddr;
        uint64_t now = (uint64_t)SDL_GetTicks();
        int plen;
        while ((plen = netImpairPop(&c->impairIn, pbuf, sizeof(pbuf),
                                    &paddr, now)) > 0) {
            udpClientProcessPacket(c, pbuf, plen);
        }
    }
}

static void udpClientDrainSnapshotsVtable(void *ctx) {
    if (ctx == NULL) return;
    udpClientDrainSnapshots((TransportUdpClientCtx *)ctx);
}

/* Client tick: receive packets from server, handle join flow, ping */
static bool udpClientTick(void *ctx) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;

    c->localTick++;

    /* Roll over PPS counters every second (100 ticks at 10ms/tick) */
    if (c->localTick - c->ppsWindowStart >= 100) {
        c->ppsRecv = c->packetsRecvThisSec;
        c->ppsSent = c->packetsSentThisSec;
        c->bpsRecv = c->bytesRecvThisSec;
        c->bpsSent = c->bytesSentThisSec;
        c->snapshotsRecvLast = c->snapshotsRecvThisSec;
        c->snapshotsLostLast = c->snapshotsLostThisSec;
        c->packetsRecvThisSec = 0;
        c->packetsSentThisSec = 0;
        c->bytesRecvThisSec = 0;
        c->bytesSentThisSec = 0;
        c->snapshotsRecvThisSec = 0;
        c->snapshotsLostThisSec = 0;
        c->ppsWindowStart = c->localTick;
    }

    /* Receive and apply all pending inbound packets (recv loop + inbound
     * impairment release).  Factored into a seam the render path also calls
     * once per frame; it advances no per-tick state. */
    udpClientDrainSnapshots(c);

    /* Service the parallel channel layer once per tick.  Tick the mux on the
     * local clock + RTT estimate, then — for anything not already carried by
     * an input trailer this tick — send a standalone PACKET_CHANNEL when the
     * frame is non-empty (an empty 2-byte frame is suppressed so steady state
     * stays storm-free).  channelBuildFrame is destructive, so a frame already
     * drained onto an input this tick leaves nothing to send here. */
    if (c->joinState == UDP_CLIENT_CONNECTED) {
        channelTick(&c->channelMux, c->localTick, c->pingMs);
        {
            uint8_t cbuf[UDP_MAX_PAYLOAD];
            int frameLen = channelBuildFrame(&c->channelMux,
                                             cbuf + PACKET_HEADER_SIZE,
                                             UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
            if (frameLen > 2) {
                packHeader(cbuf, PACKET_CHANNEL, c->outSequence++);
                udpClientSendTo(c, cbuf, PACKET_HEADER_SIZE + frameLen);
            }
        }
    }

    /* Release any impaired OUTBOUND datagrams now due onto the wire (to their
     * stored addr).  This is a send and stays on the per-tick path; it is a
     * no-op while the outbound impairment layer is disabled. */
    {
        uint8_t pbuf[NET_IMPAIR_MAX_PACKET];
        struct sockaddr_in paddr;
        uint64_t now = (uint64_t)SDL_GetTicks();
        int plen;
        while ((plen = netImpairPop(&c->impairOut, pbuf, sizeof(pbuf),
                                    &paddr, now)) > 0) {
            udpSendTo(c->sock, pbuf, plen, &paddr);
        }
    }

    /* Map-resync request retransmit: the request datagram can be lost, so
     * resend it until the first resync chunk arrives, then stop — the server's
     * own unacked-chunk loop carries the rest of the transfer. */
    if (c->joinState == UDP_CLIENT_CONNECTED && c->resyncActive &&
        !c->firstResyncChunkSeen &&
        (c->localTick - c->lastResyncRequestTick) >= MAP_RESYNC_REQUEST_RESEND_TICKS) {
        udpClientSendMapResyncRequest(c, c->activeResyncGen);
        c->lastResyncRequestTick = c->localTick;
    }

    /* Map-resync stall watchdog.  Once the first chunk arrives the request
     * retransmit above stops, so a transfer that then stalls — later chunks
     * lost past the server's resend window, or the server dropped its download
     * state on a game restart — would leave resyncActive stuck forever:
     * clientSimNetReportMapChecksum early-returns while a resync is "active",
     * so neither a fresh resync nor the disconnect cap ever fires and the
     * client renders wrong terrain indefinitely.  If there's been no forward
     * progress (request sent at start, or a chunk received) for a while,
     * abandon the in-flight resync.  Keep resyncAttempts so the next checksum
     * mismatch re-arms and eventually trips MAP_RESYNC_MAX_ATTEMPTS. */
    if (c->joinState == UDP_CLIENT_CONNECTED && c->resyncActive &&
        (c->localTick - c->lastResyncProgressTick) >= MAP_RESYNC_STALL_TICKS) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "map resync stalled (no progress for %u ticks) -> abandon (attempt %u)",
            (unsigned)(c->localTick - c->lastResyncProgressTick),
            (unsigned)c->resyncAttempts);
        udpClientFreeResyncBuf(c);
        c->resyncActive = false;
        c->firstResyncChunkSeen = false;
        c->activeResyncGen = 0;
        /* resyncAttempts intentionally retained — this failed attempt counts
         * toward the disconnect cap. */
    }

    /* Coalesced PACKET_CONTROL_ACK emission.  Only fires when a
     * PACKET_CONTROL_TICK has armed controlAckPendingTick — during
     * running, PACKET_INPUT already carries controlEventAck so no
     * dedicated ACK packet is needed.  localTick advances at 50 Hz
     * (20ms/tick — see PING_INTERVAL_TICKS = 20 → ~0.4s), so 3 ticks
     * is ~60ms, close to the plan's ~50ms target.  Send earlier when
     * a single TICK delivered 2+ new events at once, to free server
     * queue slots promptly.
     *
     * The ack is re-sent (coalesced to ~60ms) even when its value has
     * not advanced, for as long as the server keeps retransmitting
     * already-acked events: each retransmitted TICK re-arms
     * controlAckPendingTick, and a dropped lobby ack is only recovered
     * by re-sending the same value.  This self-terminates — the server
     * stops sending TICKs once its ackedSeq reaches nextSeq, so the
     * pending flag stops being re-armed and a caught-up server produces
     * zero re-acks (no per-tick ack storm).  See controlAckResendDue. */
    if (c->joinState == UDP_CLIENT_CONNECTED &&
        controlAckResendDue(c->controlAckPendingTick, c->localTick,
                            c->controlEventAck, c->lastSentControlAck)) {
        bool overdue = (c->localTick - c->controlAckPendingTick) >= 3;
        bool eagerSend = (c->controlEventAck > c->lastSentControlAck + 1);
        uint8_t ackBuf[PACKET_HEADER_SIZE + 4];
        packHeader(ackBuf, PACKET_CONTROL_ACK, c->outSequence++);
        packU32(ackBuf + PACKET_HEADER_SIZE, c->controlEventAck);
        udpClientSendTo(c, ackBuf, sizeof(ackBuf));
        mpDiagLog("[cli] CONTROL_ACK send ack=%u prevSent=%u localTick=%u overdue=%d eager=%d",
                  (unsigned)c->controlEventAck,
                  (unsigned)c->lastSentControlAck,
                  (unsigned)c->localTick,
                  (int)overdue, (int)eagerSend);
        c->lastSentControlAck = c->controlEventAck;
        c->controlAckPendingTick = 0;
    }

    /* Handle join handshake — send/resend join requests */
    if (c->joinState == UDP_CLIENT_JOINING) {
        c->ticksSinceJoinSent++;
        if (c->ticksSinceJoinSent >= JOIN_RETRY_INTERVAL) {
            if (c->joinAttempts >= JOIN_MAX_RETRIES) {
                WB_LOG_WARN(WB_LOG_CAT_NET,
                    "join handshake exhausted: attempts=%d max=%d -> ERROR",
                    (int)c->joinAttempts, (int)JOIN_MAX_RETRIES);
                c->joinState = UDP_CLIENT_ERROR;
            } else {
                /* Buffer holds: header + name + pass + 3 version bytes
                 * + WBN token + flags + clientType + clientHints
                 * + 2-byte trailing fallbackCountry + JOIN_COOKIE_LEN
                 * address-proof cookie (additive, per the connect-driven
                 * model). Server treats the trailing fields as optional. */
                uint8_t jbuf[PACKET_HEADER_SIZE + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3 + WBN_JOIN_KEY_WIRE_LEN + 1 + 2 + 2 + JOIN_COOKIE_LEN];
                int joffset = PACKET_HEADER_SIZE;
                char playerKey[WBN_JOIN_KEY_WIRE_LEN];
                memset(playerKey, 0, sizeof(playerKey));
                if (c->wbnApiToken[0] != '\0' && c->wbnServerKey[0] != '\0') {
                    char errMsg[256];
                    errMsg[0] = '\0';
                    if (!winbolonetClientJoinSession(c->wbnApiToken,
                                                     c->wbnServerKey,
                                                     playerKey, errMsg)) {
                        WB_LOG_WARN(WB_LOG_CAT_NET,
                                "[WBN] join exchange failed: %s",
                                errMsg[0] ? errMsg : "(no detail)");
                        /* Degraded: ship empty wbnJoinKey, server treats
                         * the JOIN as anonymous (no WBN attribution). */
                        playerKey[0] = '\0';
                    }
                }
                /* else: direct-IP or not logged in — leave empty. */
                packHeader(jbuf, PACKET_JOIN_REQUEST, c->outSequence++);
                memcpy(jbuf + joffset, c->playerName, PACKET_MAX_PLAYER_NAME);
                joffset += PACKET_MAX_PLAYER_NAME;
                memcpy(jbuf + joffset, c->password, MAP_STR_SIZE);
                joffset += MAP_STR_SIZE;
                jbuf[joffset++] = BOLO_VERSION_MAJOR;
                jbuf[joffset++] = BOLO_VERSION_MINOR;
                jbuf[joffset++] = BOLO_VERSION_REVISION;
                memcpy(jbuf + joffset, playerKey, WBN_JOIN_KEY_WIRE_LEN);
                joffset += WBN_JOIN_KEY_WIRE_LEN;
                /* Flags byte: bit 0 = wantRejoin, bit 1 = signed in / will authenticate.
                 * The will-auth bit rides independently of whether the server key is
                 * populated yet — it reflects only that the client holds a WBN token. */
                uint8_t joinFlags = 0;
                if (c->wantRejoin)             joinFlags |= JOIN_FLAG_WANT_REJOIN;
                if (c->wbnApiToken[0] != '\0') joinFlags |= JOIN_FLAG_WILL_AUTHENTICATE;
                jbuf[joffset++] = joinFlags;
                jbuf[joffset++] = bolo_detect_client_type();
                {
                    uint8_t clientHints = 0;
#ifdef HAVE_STEAM
                    clientHints |= PLAYER_FLAG_STEAM_BUILD;
#endif
                    if (bolo_steam_has_supporter_dlc()) clientHints |= PLAYER_FLAG_SUPPORTER;
                    jbuf[joffset++] = clientHints;
                }
                /* fallbackCountry (2 bytes). Set at create time from
                 * clientSimConnectUdp's parameter; empty when the
                 * caller passed NULL/"". Server reads as the GeoIP
                 * fallback when the joiner's IP doesn't resolve. */
                jbuf[joffset++] = (uint8_t)c->fallbackCountry[0];
                jbuf[joffset++] = (uint8_t)c->fallbackCountry[1];
                /* Address-proof cookie (always present, fixed offset): the
                 * bytes from the last PACKET_JOIN_CHALLENGE, or zeros before
                 * one has arrived. A cookie-less/zero JOIN draws a challenge. */
                if (c->haveJoinCookie) {
                    memcpy(jbuf + joffset, c->joinCookie, JOIN_COOKIE_LEN);
                } else {
                    memset(jbuf + joffset, 0, JOIN_COOKIE_LEN);
                }
                joffset += JOIN_COOKIE_LEN;
                /* Join requests bypass delay — they're control plane */
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
                 * the punch fallback. Only fires once per session — once
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

    /* Periodic ping — bypasses delay so RTT measurement is accurate
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

        /* Drive in-flight lobby map upload (no-op when none active). */
        udpClientUploadPump(c);

        /* Retransmit head of the outbound command queue if the head
         * entry was sent more than 80ms ago and is still unacked. */
        if (c->outHeadSeq != c->outTailSeq) {
            uint32_t now = (uint32_t)SDL_GetTicks();
            OutCmdEntry *head = &c->outCmdQueue[c->outHeadSeq % OUT_CMD_QUEUE_CAP];
            if (head->lastSentMs != 0 && (now - head->lastSentMs) > 80) {
                udpClientDrainCommandQueue(c);
            }
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

/* Transport-internal observer wired by transportUdpClientCreate.  Runs
 * during clientSimApplyControl alongside the (test-only) controlObserverCb.
 * Handles side effects that used to live in standalone PACKET_* handlers
 * (PACKET_ALLIANCE_UPDATE, PACKET_SERVER_SHUTDOWN, PACKET_GAME_OVER,
 * PACKET_LOBBY_MAP_CHANGE, PACKET_LOBBY_SETTINGS) and were left orphaned
 * when those packets were superseded by the body-codec control queue. */
static void udpClientTransportObserver(void *ctx, const ControlEvent *evt) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;
    if (c == NULL || evt == NULL) return;
    switch (evt->type) {
    case CTRL_ALLIANCE_REQUEST:
        /* Pop the SDL/frontend alliance-request dialog for UDP-connected
         * clients.  The bolo-lib subscriber arm has already flagged
         * pendingAllianceRequestFrom for headless/test paths. */
        if (c->clientSim != NULL &&
            evt->u.allianceRequest.toPlayer == c->playerNum) {
            BYTE fromPN = evt->u.allianceRequest.fromPlayer;
            char pName[FILENAME_MAX];
            playersGetPlayerName(&c->clientSim->sim.plyrs, fromPN, pName, FALSE);
            if (windowShowAllianceRequest() == TRUE) {
                dialogAllianceSetName(pName, fromPN);
            } else {
                char str[FILENAME_MAX + 64];
                snprintf(str, sizeof(str),
                         "You have ignored alliance request from %s", pName);
                clientMessageAdd(&c->clientSim->messages, networkStatus,
                                 "Alliance Request", str);
            }
            /* Dialog has consumed the pending flag — clear so the
             * frontend's optional poll path doesn't double-pop. */
            c->clientSim->pendingAllianceRequestFrom = 0xFF;
        }
        break;

    case CTRL_SERVER_SHUTDOWN:
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "CTRL_SERVER_SHUTDOWN received -> SERVER_SHUTDOWN");
        c->joinState = UDP_CLIENT_SERVER_SHUTDOWN;
        break;

    case CTRL_GAME_OVER:
        /* Defensive timeout reset.  The server pauses snapshots during
         * the gameOver countdown and the catch-up loop can otherwise
         * fire a spurious timeout before the server's own state
         * machine fans out either CTRL_GAME_PHASE_LOBBY (back to
         * lobby) or CTRL_SERVER_SHUTDOWN (nolobby shutdown).  Don't
         * flip joinState here — wait for the explicit shutdown event,
         * or stay connected for the lobby-return case. */
        c->lastSnapshotTick = c->localTick;
        break;

    case CTRL_LOBBY_MAP_CHANGE:
        /* Server loaded a new map mid-session.  Re-join to pull the
         * fresh map + a clean state snapshot — same trigger the old
         * PACKET_LOBBY_MAP_CHANGE handler fired. Local-transport reinstall
         * is handled by the subscriber arm; UDP must round-trip.
         *
         * Gated on c->mapInstalled so the initial state-sync (which
         * fans CTRL_LOBBY_MAP_CHANGE before the first map download has
         * landed) doesn't tear down the join we're in the middle of
         * completing — only a *change* away from a map we already
         * have should re-trigger. */
        if (c->clientSim != NULL && c->clientSim->isUdpTransport &&
            c->mapInstalled) {
            c->joinState = UDP_CLIENT_JOINING;
            c->joinAttempts = 0;
            /* Re-prove the address: a re-join must re-acquire a cookie. */
            c->haveJoinCookie = false;
            c->ticksSinceJoinSent = JOIN_RETRY_INTERVAL;  /* send immediately */
        }
        break;

    case CTRL_LOBBY_SETTINGS:
        /* Settings arrival is a stable-lobby heartbeat.  Replicates the
         * old PACKET_LOBBY_SETTINGS handler's side effects:
         *   - Lonely-lobby Steam achievement tracking
         *   - WBN re-auth when our slot lost its verified flag between
         *     rounds (server re-registered with WBN)
         * Balance-proposal clear is in the subscriber arm. */
        if (c->clientSim != NULL) {
            int connectedCount = 0;
            for (int i = 0; i < MAX_TANKS; i++) {
                if (c->clientSim->lobbySlots[i].connected) connectedCount++;
            }
            if (connectedCount == 1) {
                if (c->clientSim->lobbyAloneStartTick == 0) {
                    c->clientSim->lobbyAloneStartTick = SDL_GetTicks();
                    if (c->clientSim->lobbyAloneStartTick == 0) {
                        c->clientSim->lobbyAloneStartTick = 1;
                    }
                } else {
                    uint32_t elapsed = SDL_GetTicks() - c->clientSim->lobbyAloneStartTick;
                    if (elapsed >= 3600000) {
                        steam_set_achievement("ACH_LONELY_LOBBY");
                        steam_store_stats();
                    }
                }
            } else {
                c->clientSim->lobbyAloneStartTick = 0;
            }
            /* WBN (re-)auth is driven by the server's PACKET_WBN_REKEY,
             * not by polling our own flag state here. */
        }
        break;

    default:
        break;
    }
}

Transport transportUdpClientCreate(ClientSim *clientSim,
                                   const char *serverAddr,
                                   unsigned short serverPort,
                                   const char *playerName,
                                   const char *fallbackCountry,
                                   const char *password,
                                   const char *wbnApiToken,
                                   const char *wbnServerKey,
                                   bool wantRejoin,
                                   const char *trackerAddr,
                                   unsigned short trackerPort) {
    Transport t;
    TransportUdpClientCtx *c;
    struct hostent *he;

    WB_LOG_INFO(WB_LOG_CAT_NET,
        "client connect: server=%s:%u name='%s' wantRejoin=%d "
        "wbnApiToken=%s wbnServerKey=%s tracker=%s:%u",
        serverAddr ? serverAddr : "(null)", (unsigned)serverPort,
        playerName ? playerName : "(null)",
        (int)wantRejoin,
        (wbnApiToken && *wbnApiToken) ? "yes" : "no",
        (wbnServerKey && *wbnServerKey) ? "yes" : "no",
        (trackerAddr && *trackerAddr) ? trackerAddr : "(none)",
        (unsigned)trackerPort);

    /* Enable the MP diagnostic log on the joiner side too — the host
     * already turns it on via serverInstanceStartup, but the joiner
     * never runs that path.  Without this, the joiner's log file is
     * never created. */
    mpDiagLogEnable(1);
    mpDiagLog("[cli] transportUdpClientCreate target=%s:%u name='%s' wantRejoin=%d",
              serverAddr ? serverAddr : "(null)", (unsigned)serverPort,
              playerName ? playerName : "(null)", (int)wantRejoin);

    memset(&t, 0, sizeof(t));
    c = (TransportUdpClientCtx *)malloc(sizeof(TransportUdpClientCtx));
    memset(c, 0, sizeof(TransportUdpClientCtx));
    c->clientSim = clientSim;
    c->outCmdNextSeq = 1;
    c->outHeadSeq = 1;
    c->outTailSeq = 1;

    /* Wire the transport observer so subscriber-arm events drive the
     * transport-internal side effects (joinState transitions, re-join
     * trigger, WBN re-auth, ACH_LONELY_LOBBY). */
    clientSimSetTransportControlObserver(clientSim, udpClientTransportObserver, c);

    bolo_net_init();

    c->sock = createUdpSocket(false);
    if (c->sock == INVALID_SOCKET) {
        WB_LOG_ERROR(WB_LOG_CAT_NET, "client connect: createUdpSocket failed");
        c->joinState = UDP_CLIENT_ERROR;
        t.recordInput = udpClientRecordInput;
        t.sendInput = udpClientSendInput;
        t.tick = udpClientTick;
        t.getSnapshot = udpClientGetSnapshotVtable;
        t.drainSnapshots = udpClientDrainSnapshotsVtable;
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
            t.drainSnapshots = udpClientDrainSnapshotsVtable;
            t.ctx = c;
            return t;
        }
    }

    /* Copy player name, password, and WBN credentials */
    memset(c->playerName, 0, PACKET_MAX_PLAYER_NAME);
    strncpy(c->playerName, playerName, PACKET_MAX_PLAYER_NAME - 1);
    memset(c->password, 0, MAP_STR_SIZE);
    if (password != NULL) {
        strncpy(c->password, password, MAP_STR_SIZE - 1);
    }
    memset(c->wbnApiToken, 0, sizeof(c->wbnApiToken));
    if (wbnApiToken != NULL) {
        strncpy(c->wbnApiToken, wbnApiToken, sizeof(c->wbnApiToken) - 1);
    }
    memset(c->wbnServerKey, 0, sizeof(c->wbnServerKey));
    if (wbnServerKey != NULL) {
        strncpy(c->wbnServerKey, wbnServerKey, sizeof(c->wbnServerKey) - 1);
    }

    c->wantRejoin = wantRejoin;

    /* Cache fallback country for the JOIN_REQUEST encoder. Two chars
     * + NUL; NULL/"" lands as \0\0 on the wire, which the server
     * treats as "no fallback supplied". */
    memset(c->fallbackCountry, 0, sizeof(c->fallbackCountry));
    if (fallbackCountry != NULL) {
        size_t i;
        for (i = 0; i < 2 && fallbackCountry[i] != '\0'; i++) {
            c->fallbackCountry[i] = fallbackCountry[i];
        }
    }

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
    c->haveJoinCookie = false; /* acquire a cookie via PACKET_JOIN_CHALLENGE */
    c->ticksSinceJoinSent = JOIN_RETRY_INTERVAL; /* Send immediately on first tick */
    c->outSequence = 1;

    c->lastSnapshotSeq = 0;
    c->lastSnapshotServerTick = 0;
    c->snapshotsRecvThisSec = 0;
    c->snapshotsLostThisSec = 0;
    c->snapshotsRecvLast = 0;
    c->snapshotsLostLast = 0;
    c->snapshotsLostTotal = 0;
    c->hasSnapshot = false;
    c->reliableEventAck = 1;  /* First valid seq is 1 */
    c->mapEventAck = 1;       /* First valid map event seq is 1 */
    c->controlEventAck = 1;   /* First valid control event seq is 1 */
    c->controlAckPendingTick = 0;
    c->lastSentControlAck = 0;
    c->runningSeqAdopted = false;
    c->localTick = 0;
    c->lastPingSentTick = 0;
    c->pingMs = 0;
    c->pingDisplayMs = 0;
    pingMinWindowReset(&c->pingMinWin);
    pingEwmaReset(&c->pingEwma);
    clientTimingReset(&c->timing);

    /* Bring up the parallel channel mux for this connection. */
    channelMuxInit(&c->channelMux);
    c->channelFramesRx = 0;

    /* Optional runtime network impairment from WB_NETIMPAIR
     * (e.g. WB_NETIMPAIR=delay=75,jitter=30,loss=2,burst=2). Same spec
     * grammar as the dedicated server's -netimpair. */
    netImpairInit(&c->impairIn);
    netImpairInit(&c->impairOut);
#if WB_ENABLE_NETIMPAIR
    {
        const char *impairSpec = getenv("WB_NETIMPAIR");
        NetImpairConfig impairCfg;
        if (impairSpec != NULL && impairSpec[0] != '\0' &&
            netImpairParseConfig(impairSpec, &impairCfg)) {
            netImpairEnable(&c->impairIn, &impairCfg);
            netImpairEnable(&c->impairOut, &impairCfg);
            mpDiagLog("[cli] netimpair enabled: delay=%ums jitter=%ums "
                      "loss=%u%% burst=%u",
                      (unsigned)impairCfg.baseDelayMs,
                      (unsigned)impairCfg.jitterMs,
                      (unsigned)impairCfg.lossPercent,
                      (unsigned)impairCfg.burstLossLen);
        }
    }
#endif

    t.recordInput = udpClientRecordInput;
    t.sendInput = udpClientSendInput;
    t.tick = udpClientTick;
    t.getSnapshot = udpClientGetSnapshotVtable;
    t.drainSnapshots = udpClientDrainSnapshotsVtable;
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
    mpDiagLog("[cli] transportUdpClientDestroy joinState=%d localTick=%u",
              (int)c->joinState, (unsigned)c->localTick);
    /* Mirror serverInstanceShutdown's disable.  Safe even if this is
     * the host's own loopback client — serverInstanceShutdown will
     * have already disabled, this is just an idempotent no-op then. */
    mpDiagLogEnable(0);
    if (c->clientSim != NULL) {
        clientSimSetTransportControlObserver(c->clientSim, NULL, NULL);
    }
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
    udpClientFreeResyncBuf(c);
    if (c->uploadBuf != NULL) {
        free(c->uploadBuf);
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

void pingMinWindowReset(PingMinWindow *w) {
    memset(w, 0, sizeof(*w));
}

uint16_t pingMinWindowPush(PingMinWindow *w, uint16_t sample) {
    uint16_t minVal;
    uint8_t i;
    w->samples[w->head] = sample;
    w->head = (uint8_t)((w->head + 1) % PING_MIN_WINDOW_LEN);
    if (w->count < PING_MIN_WINDOW_LEN) {
        w->count++;
    }
    minVal = w->samples[0];
    for (i = 1; i < w->count; i++) {
        if (w->samples[i] < minVal) {
            minVal = w->samples[i];
        }
    }
    return minVal;
}

void pingEwmaReset(PingEwma *e) {
    e->valueQ8 = 0;
    e->init = false;
}

uint16_t pingEwmaUpdate(PingEwma *e, uint16_t sample) {
    if (!e->init) {
        e->valueQ8 = (uint32_t)sample << 8;
        e->init = true;
    } else {
        /* valueQ8 += (sample*256 - valueQ8) / 4, signed so a falling RTT
         * decays the value as readily as a rising one. */
        int32_t delta = (int32_t)((uint32_t)sample << 8) - (int32_t)e->valueQ8;
        e->valueQ8 = (uint32_t)((int32_t)e->valueQ8 + delta / 4);
    }
    return (uint16_t)((e->valueQ8 + 128) >> 8);
}

uint16_t transportUdpClientGetPing(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return 0;
    c = (TransportUdpClientCtx *)t->ctx;
    return c->pingDisplayMs;
}

/* ── Test-only channel-mux scaffolding ───────────────────────────────────
 * Honest test access to the otherwise-silent parallel channel layer: drain
 * the next in-order message off a channel, and read back its receive/send
 * sequence state and the count of frames consumed.  Not used by shipping
 * code — only the loopback channel integration test drives these. */
bool transportUdpClientChannelTestReceive(Transport *t, uint8_t ch,
                                          uint8_t *out, uint16_t *outLen) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return false;
    c = (TransportUdpClientCtx *)t->ctx;
    return channelReceive(&c->channelMux, ch, out, outLen);
}

void transportUdpClientChannelTestStats(Transport *t, uint8_t ch,
                                        uint32_t *expectedSeq,
                                        uint32_t *ackedSeq,
                                        uint32_t *framesRx) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL || ch >= CHANNEL_COUNT) return;
    c = (TransportUdpClientCtx *)t->ctx;
    if (expectedSeq) *expectedSeq = c->channelMux.ch[ch].expectedSeq;
    if (ackedSeq)    *ackedSeq    = c->channelMux.ch[ch].ackedSeq;
    if (framesRx)    *framesRx    = c->channelFramesRx;
}

/* Test-only: begin a real map resync now, without waiting for a checksum
 * mismatch. Drives the genuine machinery — a fresh monotonic generation, the
 * resync request to the server, then (as the harness pumps) the server accept,
 * blob download, and install that advances installedMapGen. Lets the loopback
 * test exercise the generation gate through the real request/accept/install
 * path. Returns false if a resync is already outstanding. */
bool transportUdpClientTestBeginResync(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return false;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->resyncActive) return false;
    c->activeResyncGen = udpClientNextResyncGen(c);
    c->resyncActive = true;
    c->firstResyncChunkSeen = false;
    c->lastResyncRequestTick = c->localTick;
    c->lastResyncProgressTick = c->localTick;
    c->resyncAttempts++;
    udpClientSendMapResyncRequest(c, c->activeResyncGen);
    return true;
}

/* Test-only: read the client's map-generation state — installedMapGen (the
 * generation the gate compares against) and mapResyncCount (cumulative
 * successful installs, so a test can assert whether a resync actually ran). */
void transportUdpClientTestMapState(Transport *t, uint32_t *installedMapGen,
                                    uint32_t *mapResyncCount) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return;
    c = (TransportUdpClientCtx *)t->ctx;
    if (installedMapGen) *installedMapGen = c->installedMapGen;
    if (mapResyncCount)  *mapResyncCount  = c->mapResyncCount;
}

void transportUdpClientGetNetStats(Transport *t, int *ppsRecv, int *ppsSent,
                                   int *bpsRecv, int *bpsSent, int *numErrors,
                                   int *snapshotsRecv, int *snapshotsLost,
                                   int *snapshotsLostTotal) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) {
        *ppsRecv = 0;
        *ppsSent = 0;
        *bpsRecv = 0;
        *bpsSent = 0;
        *numErrors = 0;
        if (snapshotsRecv) *snapshotsRecv = 0;
        if (snapshotsLost) *snapshotsLost = 0;
        if (snapshotsLostTotal) *snapshotsLostTotal = 0;
        return;
    }
    c = (TransportUdpClientCtx *)t->ctx;
    *ppsRecv = (int)c->ppsRecv;
    *ppsSent = (int)c->ppsSent;
    *bpsRecv = (int)c->bpsRecv;
    *bpsSent = (int)c->bpsSent;
    *numErrors = (int)c->netErrors;
    if (snapshotsRecv) *snapshotsRecv = (int)c->snapshotsRecvLast;
    if (snapshotsLost) *snapshotsLost = (int)c->snapshotsLostLast;
    if (snapshotsLostTotal) *snapshotsLostTotal = (int)c->snapshotsLostTotal;
}

void transportUdpClientGetTimingStats(Transport *t, int *clockOffsetTicks,
                                      int *jitterMs, int *rttMs,
                                      int *pipelineDepthTicks) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) {
        if (clockOffsetTicks) *clockOffsetTicks = 0;
        if (jitterMs) *jitterMs = 0;
        if (rttMs) *rttMs = 0;
        if (pipelineDepthTicks) *pipelineDepthTicks = 0;
        return;
    }
    c = (TransportUdpClientCtx *)t->ctx;
    if (clockOffsetTicks) *clockOffsetTicks = (int)clientTimingClockOffsetTicks(&c->timing);
    if (jitterMs) *jitterMs = (int)clientTimingJitterMs(&c->timing);
    if (rttMs) *rttMs = (int)clientTimingRttMs(&c->timing);
    if (pipelineDepthTicks) *pipelineDepthTicks = (int)clientTimingPipelineDepthTicks(&c->timing);
}

const char *transportUdpClientGetJoinRejectReason(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return NULL;
    c = (TransportUdpClientCtx *)t->ctx;
    return c->joinRejectReason;
}

uint32_t transportUdpClientGetMapResyncCount(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return 0;
    c = (TransportUdpClientCtx *)t->ctx;
    return c->mapResyncCount;
}

/* Called once per full-sync snapshot with the result of the map-checksum
 * compare. Owns the whole resync state machine: on a mismatch it starts a
 * resync (subject to suppression + an outstanding-request guard) or, once the
 * backoff cap is hit, disconnects with a localized reason; on a match it clears
 * the backoff counter. The detection itself lives in clientApplySnapshot. */
void transportUdpClientReportMapChecksum(Transport *t, bool matched) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return;
    c = (TransportUdpClientCtx *)t->ctx;

    /* Only meaningful for a connected, playing client. */
    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    if (matched) {
        /* Converged (or never diverged): clear the backoff once no resync is
         * in flight, so a later genuine divergence starts fresh. */
        if (!c->resyncActive) {
            c->resyncAttempts = 0;
            c->resyncSuppressUntilTick = 0;
        }
        return;
    }

    /* Mismatch. One outstanding request at a time, and stay quiet during the
     * post-install grace window — otherwise every full-sync would re-request. */
    if (c->resyncActive) return;
    if (c->localTick < c->resyncSuppressUntilTick) return;

    if (c->resyncAttempts >= MAP_RESYNC_MAX_ATTEMPTS) {
        /* Repeated full resyncs didn't fix it — almost certainly a checksum
         * disagreement (a bug), not real divergence. Give up cleanly rather
         * than loop a 64KB transfer forever. Reuse the connection-lost path
         * the frontend already handles mid-game; carry a localized reason. */
        clientSimSetConnectErrorReason(c->clientSim, langGetText(STR_KICK_MAP_DESYNC));
        c->joinState = UDP_CLIENT_SERVER_SHUTDOWN;
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "map desync unresolved after %u resyncs -> disconnect",
            (unsigned)c->resyncAttempts);
        return;
    }

    /* Start a new resync. */
    c->activeResyncGen = udpClientNextResyncGen(c);
    c->resyncActive = true;
    c->firstResyncChunkSeen = false;
    c->lastResyncRequestTick = c->localTick;
    c->lastResyncProgressTick = c->localTick;
    c->resyncAttempts++;
    udpClientSendMapResyncRequest(c, c->activeResyncGen);
    WB_LOG_INFO(WB_LOG_CAT_NET,
        "map checksum mismatch -> resync request gen=%u attempt=%u",
        (unsigned)c->activeResyncGen, (unsigned)c->resyncAttempts);
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

uint8_t transportUdpClientGetMapDownloadPercent(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return 100;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->mapDownloadTotal == 0) return 100;
    uint32_t pct = (c->mapDownloadReceived * 100u) / c->mapDownloadTotal;
    return pct > 100 ? 100 : (uint8_t)pct;
}

void transportUdpClientSendWbnReauth(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    udpClientSendWbnReauth(c);
}

/* ── Layout A lobby commands — Client → Server ───────────────────── */

void transportUdpClientSendLobbyMapListRequest(Transport *t,
                                                const char *relPath) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1 + 256];
    int pathLen, len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (relPath == NULL) relPath = "";
    pathLen = (int)strlen(relPath);
    if (pathLen > 255) pathLen = 255;

    packHeader(buf, PACKET_LOBBY_MAP_LIST_REQ, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = (uint8_t)pathLen;
    if (pathLen > 0) memcpy(buf + PACKET_HEADER_SIZE + 1, relPath, pathLen);
    len = PACKET_HEADER_SIZE + 1 + pathLen;
    udpClientSendTo(c, buf, len);

    if (c->clientSim) {
        memset(c->clientSim->lobbyMapListReqPath, 0,
               sizeof(c->clientSim->lobbyMapListReqPath));
        if (pathLen > 0) {
            memcpy(c->clientSim->lobbyMapListReqPath, relPath,
                   (size_t)pathLen);
        }
        c->clientSim->lobbyMapListCount = 0;
        c->clientSim->lobbyMapListReady = false;
        c->clientSim->lobbyMapListInFlight = true;
    }
}

void transportUdpClientSendLobbyMapPreviewRequest(Transport *t,
                                                  const char *relPath) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1 + 256];
    int pathLen, len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (relPath == NULL) relPath = "";
    pathLen = (int)strlen(relPath);
    if (pathLen == 0 || pathLen > 255) return;

    packHeader(buf, PACKET_LOBBY_MAP_PREVIEW_REQ, c->outSequence++);
    buf[PACKET_HEADER_SIZE] = (uint8_t)pathLen;
    memcpy(buf + PACKET_HEADER_SIZE + 1, relPath, pathLen);
    len = PACKET_HEADER_SIZE + 1 + pathLen;
    udpClientSendTo(c, buf, len);

    if (c->clientSim) {
        ClientSim *cs = c->clientSim;
        memset(cs->lobbyMapPreviewReqPath, 0, sizeof(cs->lobbyMapPreviewReqPath));
        memcpy(cs->lobbyMapPreviewReqPath, relPath, (size_t)pathLen);
        cs->lobbyMapPreviewPath[0]   = '\0';
        cs->lobbyMapPreviewInFlight  = true;
        cs->lobbyMapPreviewReady     = false;
        cs->lobbyMapPreviewError     = false;
        cs->lobbyMapPreviewTotal     = 0;
        cs->lobbyMapPreviewReceived  = 0;
    }
}

void transportUdpClientSendLobbyMapSearchRequest(Transport *t,
                                                  const char *relPath,
                                                  const char *query) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE + 1 + 256 + 1 + 128];
    int pathLen, qLen, len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (relPath == NULL) relPath = "";
    if (query == NULL)   query   = "";
    pathLen = (int)strlen(relPath);
    if (pathLen > 255) pathLen = 255;
    qLen = (int)strlen(query);
    if (qLen > 127) qLen = 127;

    packHeader(buf, PACKET_LOBBY_MAP_SEARCH_REQ, c->outSequence++);
    int pos = PACKET_HEADER_SIZE;
    buf[pos++] = (uint8_t)pathLen;
    if (pathLen > 0) { memcpy(buf + pos, relPath, pathLen); pos += pathLen; }
    buf[pos++] = (uint8_t)qLen;
    if (qLen > 0)    { memcpy(buf + pos, query, qLen);     pos += qLen; }
    len = pos;
    udpClientSendTo(c, buf, len);

    if (c->clientSim) {
        memset(c->clientSim->lobbyMapSearchReqPath, 0,
               sizeof(c->clientSim->lobbyMapSearchReqPath));
        memset(c->clientSim->lobbyMapSearchReqQuery, 0,
               sizeof(c->clientSim->lobbyMapSearchReqQuery));
        if (pathLen > 0) {
            memcpy(c->clientSim->lobbyMapSearchReqPath, relPath,
                   (size_t)pathLen);
        }
        if (qLen > 0) {
            memcpy(c->clientSim->lobbyMapSearchReqQuery, query,
                   (size_t)qLen);
        }
        c->clientSim->lobbyMapSearchCount = 0;
        c->clientSim->lobbyMapSearchReady = false;
        c->clientSim->lobbyMapSearchInFlight = true;
    }
}

/* === Lobby map upload — packet emitters ============================
 *
 * The wire-side packet builders are factored as ctx-flavoured helpers
 * so both the public Transport*-flavoured entry points and the
 * transport-internal pump can drive the same packet layout. */

static void udpClientUploadSendBegin(TransportUdpClientCtx *c,
                                      uint32_t totalLen,
                                      const char *name) {
    uint8_t buf[PACKET_HEADER_SIZE + 4 + 1 + 255];
    int nameLen, len;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (name == NULL) name = "";
    nameLen = (int)strlen(name);
    if (nameLen > 255) nameLen = 255;

    packHeader(buf, PACKET_LOBBY_MAP_UPLOAD_BEGIN, c->outSequence++);
    buf[PACKET_HEADER_SIZE + 0] = (uint8_t)((totalLen >> 24) & 0xFF);
    buf[PACKET_HEADER_SIZE + 1] = (uint8_t)((totalLen >> 16) & 0xFF);
    buf[PACKET_HEADER_SIZE + 2] = (uint8_t)((totalLen >>  8) & 0xFF);
    buf[PACKET_HEADER_SIZE + 3] = (uint8_t)( totalLen        & 0xFF);
    buf[PACKET_HEADER_SIZE + 4] = (uint8_t)nameLen;
    if (nameLen > 0) memcpy(buf + PACKET_HEADER_SIZE + 5, name, nameLen);
    len = PACKET_HEADER_SIZE + 5 + nameLen;
    udpClientSendTo(c, buf, len);

    if (c->clientSim) {
        c->clientSim->lobbyMapUploadStatus = 1;
        c->clientSim->lobbyMapUploadRejectCode = 0;
        c->clientSim->lobbyMapUploadFinalPath[0] = '\0';
    }
}

static void udpClientUploadSendUseLocal(TransportUdpClientCtx *c,
                                         uint32_t totalLen,
                                         const char *name,
                                         const char *relPath,
                                         const uint8_t md5[16]) {
    /* Wire: [hdr 8][totalLen 4][nameLen 1][name N][relPathLen 1][relPath M][md5 16] */
    uint8_t buf[PACKET_HEADER_SIZE + 4 + 1 + 255 + 1 + 255 + 16];
    int nameLen, relLen, pos;

    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    if (name == NULL) name = "";
    if (relPath == NULL) relPath = "";
    nameLen = (int)strlen(name);
    if (nameLen > 255) nameLen = 255;
    relLen = (int)strlen(relPath);
    if (relLen > 255) relLen = 255;

    packHeader(buf, PACKET_LOBBY_MAP_USE_LOCAL, c->outSequence++);
    pos = PACKET_HEADER_SIZE;
    buf[pos++] = (uint8_t)((totalLen >> 24) & 0xFF);
    buf[pos++] = (uint8_t)((totalLen >> 16) & 0xFF);
    buf[pos++] = (uint8_t)((totalLen >>  8) & 0xFF);
    buf[pos++] = (uint8_t)( totalLen        & 0xFF);
    buf[pos++] = (uint8_t)nameLen;
    if (nameLen > 0) { memcpy(buf + pos, name, nameLen); pos += nameLen; }
    buf[pos++] = (uint8_t)relLen;
    if (relLen > 0) { memcpy(buf + pos, relPath, relLen); pos += relLen; }
    memcpy(buf + pos, md5, 16);
    pos += 16;
    udpClientSendTo(c, buf, pos);

    if (c->clientSim) {
        /* Mark "USE_LOCAL in flight" — clears to upload-or-done when
         * the server replies. */
        c->clientSim->lobbyMapUploadStatus = 1;
        c->clientSim->lobbyMapUploadRejectCode = 0;
        c->clientSim->lobbyMapUploadFinalPath[0] = '\0';
    }
}

static void udpClientUploadSendChunk(TransportUdpClientCtx *c,
                                      uint32_t offset,
                                      const uint8_t *data,
                                      uint16_t dataLen) {
    uint8_t buf[PACKET_HEADER_SIZE + 4 + 2 + 1024];
    int len;

    if (dataLen == 0 || data == NULL) return;
    if (dataLen > UPLOAD_CHUNK_SIZE) return;
    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_MAP_UPLOAD_CHUNK, c->outSequence++);
    /* Fixed header is generated; the data memcpy stays hand-written. */
    MapUploadChunkHeader hdr = { offset, dataLen };
    packMapUploadChunkHeader(buf + PACKET_HEADER_SIZE, &hdr);
    memcpy(buf + PACKET_HEADER_SIZE + 6, data, dataLen);
    len = PACKET_HEADER_SIZE + 6 + dataLen;
    udpClientSendTo(c, buf, len);
}

void transportUdpClientSendLobbyMapUploadBegin(Transport *t,
                                                uint32_t totalLen,
                                                const char *name) {
    udpClientUploadSendBegin((TransportUdpClientCtx *)t->ctx, totalLen, name);
}

void transportUdpClientSendLobbyMapUseLocal(Transport *t,
                                             uint32_t totalLen,
                                             const char *name,
                                             const char *relPath,
                                             const uint8_t md5[16]) {
    udpClientUploadSendUseLocal((TransportUdpClientCtx *)t->ctx, totalLen,
                                 name, relPath, md5);
}

void transportUdpClientSendLobbyMapUploadChunk(Transport *t,
                                                uint32_t offset,
                                                const uint8_t *data,
                                                uint16_t dataLen) {
    udpClientUploadSendChunk((TransportUdpClientCtx *)t->ctx, offset,
                              data, dataLen);
}

/* === Lobby map upload — state machine =============================
 *
 * Moved from imgui_lobby.cpp (lobbyUploadKick / lobbyUploadPump). The
 * transport owns the bytes, the BEGIN/USE_LOCAL handshake, the chunk
 * pump (paced from udpClientTick), and the watchdog. */

static void udpClientUploadCleanup(TransportUdpClientCtx *c) {
    if (c->uploadBuf != NULL) {
        free(c->uploadBuf);
        c->uploadBuf = NULL;
    }
    c->uploadActive          = false;
    c->uploadTotal           = 0;
    c->uploadOffset          = 0;
    c->uploadName[0]         = '\0';
    c->uploadUseLocalPending = false;
    c->uploadBeginSent       = false;
    c->uploadStartedMs       = 0;
    c->uploadPrevStatus      = 0;
    c->uploadPrevProgressMs  = 0;
    c->uploadPrevOffset      = 0;
}

/* Shared kick: stash the bytes on the transport, optionally try
 * USE_LOCAL first when the caller derived a data/maps-relative path,
 * else announce via BEGIN. `buf` is copied; caller retains ownership. */
static bool udpClientUploadStart(TransportUdpClientCtx *c,
                                  const uint8_t *buf, size_t len,
                                  const char *name,
                                  const char *relPath, /* nullable */
                                  const uint8_t *md5   /* required iff relPath */) {
    if (c == NULL || buf == NULL || name == NULL || name[0] == '\0') {
        return false;
    }
    if (len == 0 || len > LOBBY_MAP_UPLOAD_MAX_BYTES) return false;
    if (c->joinState != UDP_CLIENT_CONNECTED) return false;
    if (c->uploadActive) return false;

    udpClientUploadCleanup(c);

    c->uploadBuf = (uint8_t *)malloc(len);
    if (c->uploadBuf == NULL) return false;
    memcpy(c->uploadBuf, buf, len);
    c->uploadTotal  = (uint32_t)len;
    c->uploadOffset = 0;
    c->uploadName[0] = '\0';
    {
        size_t copy = strlen(name);
        if (copy >= sizeof(c->uploadName)) copy = sizeof(c->uploadName) - 1;
        memcpy(c->uploadName, name, copy);
        c->uploadName[copy] = '\0';
    }
    c->uploadActive = true;

    if (c->clientSim != NULL) {
        c->clientSim->lobbyMapUploadStatus     = 0;
        c->clientSim->lobbyMapUploadRejectCode = 0;
        c->clientSim->lobbyMapUploadFinalPath[0] = '\0';
        c->clientSim->lobbyMapUseLocalNeedsFallback = false;
    }

    if (relPath != NULL && relPath[0] != '\0' && md5 != NULL) {
        udpClientUploadSendUseLocal(c, c->uploadTotal, c->uploadName,
                                     relPath, md5);
        c->uploadUseLocalPending = true;
        c->uploadBeginSent       = false;
    } else {
        udpClientUploadSendBegin(c, c->uploadTotal, c->uploadName);
        c->uploadUseLocalPending = false;
        c->uploadBeginSent       = true;
    }
    return true;
}

/* Per-tick pump. Drives the upload through the USE_LOCAL → BEGIN →
 * CHUNK → DONE/REJECT lifecycle. */
static void udpClientUploadPump(TransportUdpClientCtx *c) {
    uint8_t st;
    uint64_t now, sinceProgress, sinceStart;
    bool advanced, timedOut;

    if (!c->uploadActive) return;
    if (c->clientSim == NULL) {
        udpClientUploadCleanup(c);
        return;
    }

    st = c->clientSim->lobbyMapUploadStatus;
    if (st == 3 || st == 4) {
        /* Done or rejected — drop the buffer so the next upload starts
         * fresh. The frontend still sees status/rejectCode/finalPath
         * because those live on the ClientSim. */
        udpClientUploadCleanup(c);
        return;
    }

    /* Watchdog. Treat the moment the last chunk goes out as one final
     * forward-progress event (lobby-misc item 10): after the last
     * CHUNK send, offset stays pinned at uploadTotal while we wait
     * for MAP_UPLOAD_DONE; without this reset the stall timer would
     * count against a server that's merely slow to load + reply. */
    now = SDL_GetTicks();
    advanced = (c->uploadPrevStatus != st) ||
               (c->uploadPrevOffset != c->uploadOffset);
    if (st >= 2 && c->uploadOffset == c->uploadTotal &&
        c->uploadPrevOffset < c->uploadTotal) {
        advanced = true;
    }
    if (advanced) {
        c->uploadPrevStatus     = st;
        c->uploadPrevOffset     = c->uploadOffset;
        c->uploadPrevProgressMs = now;
    }
    if (c->uploadStartedMs == 0) c->uploadStartedMs = now;
    sinceProgress = now - c->uploadPrevProgressMs;
    sinceStart    = now - c->uploadStartedMs;
    timedOut = false;
    if (st < 2 && sinceStart > UPLOAD_ACK_TIMEOUT_MS) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "upload watchdog: no BEGIN ACK in %llums — freeing",
            (unsigned long long)sinceStart);
        timedOut = true;
    } else if (st >= 2 && sinceProgress > UPLOAD_STALL_TIMEOUT_MS) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "upload watchdog: no chunk progress in %llums — freeing",
            (unsigned long long)sinceProgress);
        timedOut = true;
    }
    if (timedOut) {
        udpClientUploadCleanup(c);
        return;
    }

    /* USE_LOCAL was NACKed: the server doesn't have a matching file at
     * the relative path / MD5. Fall back to BEGIN + CHUNK against the
     * bytes already buffered. */
    if (c->uploadUseLocalPending &&
        clientSimConsumeUseLocalFallback(c->clientSim)) {
        udpClientUploadSendBegin(c, c->uploadTotal, c->uploadName);
        c->uploadUseLocalPending = false;
        c->uploadBeginSent       = true;
        return; /* wait one more tick for ACK */
    }

    /* Still waiting on ACK to BEGIN (status flips to 2 on ACK). */
    if (st != 2) return;

    /* Chunk pump — UPLOAD_CHUNKS_PER_TICK chunks per tick paces a 1 MB
     * upload to roughly 130 ticks (~2.5 s at 50 fps) without flooding
     * the server's receive window. */
    {
        int i;
        for (i = 0; i < UPLOAD_CHUNKS_PER_TICK &&
                    c->uploadOffset < c->uploadTotal; i++) {
            uint32_t remaining = c->uploadTotal - c->uploadOffset;
            uint16_t cur = (remaining > UPLOAD_CHUNK_SIZE)
                           ? UPLOAD_CHUNK_SIZE
                           : (uint16_t)remaining;
            udpClientUploadSendChunk(c, c->uploadOffset,
                                      c->uploadBuf + c->uploadOffset, cur);
            c->uploadOffset += cur;
        }
    }
}

bool transportUdpClientStartLobbyMapUploadFromBytes(Transport *t,
                                                     const uint8_t *buf,
                                                     size_t len,
                                                     const char *mapName) {
    return udpClientUploadStart((TransportUdpClientCtx *)t->ctx,
                                 buf, len, mapName,
                                 /*relPath=*/NULL, /*md5=*/NULL);
}

bool transportUdpClientStartLobbyMapUploadFromPath(Transport *t,
                                                    const char *localFilePath) {
    TransportUdpClientCtx *c;
    size_t fileLen = 0;
    void *fileData = NULL;
    char nameBuf[128];
    char relPath[256];
    bool haveRelPath;
    uint8_t md5[16];
    bool ok;

    if (t == NULL || localFilePath == NULL || localFilePath[0] == '\0') {
        return false;
    }
    c = (TransportUdpClientCtx *)t->ctx;

    /* Pre-flight validation. boloMapValidate opens, decodes against
     * scratch buffers, and discards — caller never sees the decoded
     * sim. Bail on any rejection (file-not-found, malformed map,
     * truncated). */
    if (!boloMapValidate(localFilePath, NULL, 0)) return false;

    fileData = SDL_LoadFile(localFilePath, &fileLen);
    if (fileData == NULL) return false;
    if (fileLen == 0 || fileLen > LOBBY_MAP_UPLOAD_MAX_BYTES) {
        SDL_free(fileData);
        return false;
    }

    /* Basename of the local path (drop directory components). */
    {
        const char *base = localFilePath;
        const char *p;
        for (p = localFilePath; *p; p++) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
        SDL_strlcpy(nameBuf, base, sizeof(nameBuf));
    }

    /* Derive a data/maps-relative path for the USE_LOCAL pre-check.
     * Local FS provider hands us paths like "data/maps/Foo/Bar.map";
     * on Windows the separators may be backslashes. Strip the prefix
     * to get a path like "Foo/Bar.map" — same scheme
     * PACKET_LOBBY_MAP_PREVIEW_REQ uses. If the path doesn't sit
     * under data/maps/, skip USE_LOCAL and go straight to BEGIN. */
    relPath[0] = '\0';
    {
        char normalized[FILENAME_MAX];
        char *p;
        const char *kPrefix = "data/maps/";
        const size_t kPrefixLen = 10;
        SDL_strlcpy(normalized, localFilePath, sizeof(normalized));
        for (p = normalized; *p; p++) {
            if (*p == '\\') *p = '/';
        }
        if (strncmp(normalized, kPrefix, kPrefixLen) == 0) {
            SDL_strlcpy(relPath, normalized + kPrefixLen, sizeof(relPath));
        }
    }
    haveRelPath = (relPath[0] != '\0');
    if (haveRelPath) {
        md5Compute(fileData, fileLen, md5);
    }

    ok = udpClientUploadStart(c, (const uint8_t *)fileData, fileLen, nameBuf,
                               haveRelPath ? relPath : NULL,
                               haveRelPath ? md5     : NULL);
    SDL_free(fileData);
    return ok;
}

uint8_t transportUdpClientGetLobbyMapUploadProgressPercent(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL) return 0;
    c = (TransportUdpClientCtx *)t->ctx;
    if (!c->uploadActive || c->uploadTotal == 0) return 0;
    {
        uint64_t pct = (uint64_t)c->uploadOffset * 100 /
                       (uint64_t)c->uploadTotal;
        if (pct > 100) pct = 100;
        return (uint8_t)pct;
    }
}

#ifdef WB_FUZZ
/* ================================================================
 * Fuzz-only client-snapshot seam (hardening plan §1.2)
 *
 * Drives udpClientProcessPacket's PACKET_STATE_SNAPSHOT path with attacker-
 * controlled bytes. udpClientProcessPacket and TransportUdpClientCtx have
 * internal linkage, so the seam lives in this TU. Compiled only under
 * -DWB_FUZZ; absent from every shipping build.
 *
 * The fuzz input is the snapshot *body* — the seam frames it with a valid
 * STATE_SNAPSHOT header so every input reaches the decoder. Crucially the
 * datagram is heap-allocated to its EXACT length: the over-read this target
 * hunts (the reliable-event loop's 1-byte guard vs. unpackGameEvent's
 * up-to-8-byte memcpy) only trips ASan's redzone when the buffer ends exactly
 * at len. An oversized buffer would silently absorb the read and hide the bug.
 * ================================================================ */
static TransportUdpClientCtx g_fuzzClientCtx;

void transportUdpClientFuzzInit(ClientSim *sim) {
    memset(&g_fuzzClientCtx, 0, sizeof(g_fuzzClientCtx));
    g_fuzzClientCtx.sock = INVALID_SOCKET;
    g_fuzzClientCtx.joinState = UDP_CLIENT_CONNECTED;
    g_fuzzClientCtx.clientSim = sim;
}

void transportUdpClientFuzzProcessSnapshot(const uint8_t *body, size_t size) {
    size_t n = (size_t)PACKET_HEADER_SIZE + size;
    uint8_t *buf = (uint8_t *)malloc(n);   /* exact size -> ASan-guarded tail */
    if (buf == NULL) return;
    buf[0] = (uint8_t)BOLO_NEW_MAGIC_0;
    buf[1] = (uint8_t)BOLO_NEW_MAGIC_1;
    buf[2] = (uint8_t)PACKET_STATE_SNAPSHOT;
    buf[3] = 0;
    buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 1; /* seq=1 (unpackU32 buf+4) */
    if (size > 0) memcpy(buf + PACKET_HEADER_SIZE, body, size);
    /* Re-arm the stale-snapshot high-water gate so each input decodes fresh. */
    g_fuzzClientCtx.lastSnapshotSeq = 0;
    g_fuzzClientCtx.lastSnapshotServerTick = 0;
    g_fuzzClientCtx.hasSnapshot = false;
    udpClientProcessPacket(&g_fuzzClientCtx, buf, (int)n);
    free(buf);
}

/* Drive the PACKET_JOIN_ACCEPT handler. The fuzz input is the accept body
 * ([playerNum][serverTick][mapSize][optional connId]); the seam frames the
 * header on an exact-size buffer and forces the JOINING state the handler
 * requires. Exercises the server-assigned-slot validation and the
 * mapSize/connId parsing — a path no other fuzz target reaches. */
void transportUdpClientFuzzProcessJoinAccept(const uint8_t *body, size_t size) {
    size_t n = (size_t)PACKET_HEADER_SIZE + size;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (buf == NULL) return;
    buf[0] = (uint8_t)BOLO_NEW_MAGIC_0;
    buf[1] = (uint8_t)BOLO_NEW_MAGIC_1;
    buf[2] = (uint8_t)PACKET_JOIN_ACCEPT;
    buf[3] = 0;
    buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0;
    if (size > 0) memcpy(buf + PACKET_HEADER_SIZE, body, size);
    /* JOIN_ACCEPT is only processed mid-handshake. */
    g_fuzzClientCtx.joinState = UDP_CLIENT_JOINING;
    udpClientProcessPacket(&g_fuzzClientCtx, buf, (int)n);
    free(buf);
}
#endif /* WB_FUZZ */
