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
#include "voice_segment.h"
#include "bulk_transfer.h"
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

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Notify JS whenever the live WBN server_key is set or rotates, so the web
 * client can keep its shareable /join/<key> URL pointed at the game this
 * connection is currently playing (the backend mints join codes against this
 * key, and it rotates each return-to-lobby). JS uses it purely to
 * history.replaceState — no reconnect, no re-mint. */
EM_JS(void, wbOnGameKey, (const char *key), {
    if (Module.wbOnGameKey) Module.wbOnGameKey(UTF8ToString(key));
});
#endif

/* ================================================================
 * CLIENT SIDE
 * ================================================================ */

#define OUT_CMD_QUEUE_CAP 64

typedef struct {
    ClientCommand cmd;          /* cmd.cmdSeq matches this entry's seq */
    uint32_t lastSentMs;        /* 0 = never sent yet; eager send sets it */
} OutCmdEntry;

/* Voice frames received from the server wait here until the frontend pops
 * them.  One tick's worth of talkers is a handful of frames, so the ring is
 * sized well past that and drops its oldest entry if the frontend somehow
 * falls behind — a stale voice frame has no value once its moment passed. */
#define CLIENT_VOICE_RX_RING 16

typedef struct {
    uint8_t  fromPlayer;
    uint8_t  seq;
    uint8_t  flags;
    uint16_t len;
    uint8_t  data[VOICE_SEG_MAX_OPUS];
} VoiceRxFrame;

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

    /* Reliable map-event dedup */
    uint32_t mapEventAck;       /* Next expected reliable map event seq (init to 1) */

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
    /* Suppress the periodic ping below. Written only by the WB_NOPING read at
     * ctx create, which is compiled in solely where WB_ENABLE_NETIMPAIR is on
     * (the unit-test target's own build of this file), so it stays false for
     * the life of every shipping connection. Lets a test hold a connected
     * client whose only outbound traffic is what the test itself sends. */
    bool suppressPing;
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

    /* Map download state. The compressed map arrives on CHANNEL_BULK behind a
     * bulk-transfer stream header; the BulkReceiver (c->bulkRecv) reassembles it
     * into mapDownloadBuf, which JOIN_ACCEPT sized from the accept's mapSize.
     * mapDownloadReceived reaches mapDownloadTotal only on full reassembly. */
    BYTE    *mapDownloadBuf;     /* Buffer for reassembling compressed map */
    uint32_t mapDownloadTotal;   /* Total expected bytes (from JOIN_ACCEPT) */
    uint32_t mapDownloadReceived;/* Bytes reassembled (== total on completion) */
    /* True once the buffered map has been installed onto the ClientSim
     * (mp/pb/bs/ss populated). Distinct from mapDownloadComplete on the
     * ClientSim (bytes-received) — this tracks "applied". Reset to false
     * when a fresh download begins (JOIN_ACCEPT reallocates the buffer)
     * so a mid-lobby map swap re-gates snapshots until the new map is
     * installed. */
    bool     mapInstalled;

    /* Monotonic count of installed maps thrown away — incremented on each
     * mapInstalled true -> false transition, i.e. each time a (re-)accept
     * arms a fresh download over a map this client had already installed.
     * The initial join does not count: nothing was installed to discard.
     *
     * Exists because the invalidation is a transient. The window between
     * dropping the old map and finishing the new one can close inside a
     * single test pump, so a test that samples clientSimGetServerMapData
     * for NULL can miss it entirely and wrongly conclude the map change
     * never re-armed the download. A monotonic counter cannot be missed
     * however the sampling falls. */
    uint32_t mapInvalidateCount;

    /* Join-download readiness/watchdog. The server streams the map only after
     * this client's PACKET_MAP_DL_READY (sent when JOIN_ACCEPT arms the
     * buffers), so the stream can never race the accept. The watchdog re-sends
     * the READY when the stream doesn't start (the ask was lost) or stops
     * making progress (the in-flight transfer's head was missed — e.g. a
     * duplicate accept was processed mid-stream); the server answers a re-ask
     * with a full restart behind a CHANNEL_BULK re-base. */
    uint32_t dlProgressBytes;    /* bulk bytes observed at the last watchdog check */
    uint32_t dlProgressTick;     /* localTick when dlProgressBytes last advanced
                                  * (also re-armed by each READY send) */
    uint8_t  dlReadyResends;     /* restart asks this download; capped → ERROR */

    /* Map desync recovery (resync) — a parallel download that runs while the
     * client keeps playing (joinState stays CONNECTED) and hot-swaps the map
     * in on completion. Mirrors the join-download fields above. */
    BYTE    *mapResyncBuf;             /* Parallel reassembly buffer (owned),
                                        * allocated by the bulk receiver's onBegin
                                        * from the resync stream header's size */
    uint32_t mapResyncTotal;           /* Total expected bytes (from the header) */
    uint32_t activeResyncGen;          /* gen of the in-flight resync (0 = none) */
    uint32_t resyncGenCounter;         /* Monotonic source for fresh nonzero gens */
    bool     resyncActive;             /* A resync request is outstanding/installing */
    bool     firstResyncChunkSeen;     /* Stop request retransmit once chunks arrive */
    uint32_t lastResyncRequestTick;    /* localTick of last request send (retransmit) */
    uint32_t resyncAttempts;           /* Resyncs that did not resolve the mismatch */
    uint32_t mapMismatchStreak;        /* Consecutive full-sync checksum mismatches not
                                        * yet acted on; debounces a transient mismatch
                                        * (in-flight CHANNEL_MAP events) into a resync
                                        * only once it persists */
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
    /* Tankless spectator connect. Set at create from clientSimConnectUdp's
     * spectator arg. When true the JOIN carries JOIN_FLAG_SPECTATOR and the
     * accept handler takes the spectator branch (slot==SPECTATOR_ACCEPT_NO_SLOT
     * → UDP_CLIENT_SPECTATING, no tank slot, no map download). It also gates
     * that branch: a player-join client (false) treats the 0xFF slot as an
     * invalid slot and rejects, so the branch can never fire by accident. */
    bool spectator;
    /* Spectator dual-mode bit (authoritative home; mirrored one-way onto the
     * ClientSim). True = the server is feeding the live lobby control bus
     * (lobby/countdown); false = the delayed ring feed. Seeded from the accept
     * packet's mode byte, then flipped by which source is feeding: false on the
     * first delayed frame (cold-start countdown or bulk seed/record), true when
     * live lobby control resumes (the server re-subscribed at return-to-lobby). */
    bool specLiveLobby;
    /* A live-lobby spectator is downloading the current lobby map over
     * CHANNEL_BULK (BULK_KIND_DOWNLOAD), armed from a spectator JOIN_ACCEPT with a
     * non-zero map size. Gates the bulk receiver to accept that stream while
     * staying UDP_CLIENT_SPECTATING (not the player game-start pipeline); cleared
     * once the map installs. The delayed-ring seed is a separate source. */
    bool specLobbyMapDownloading;

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

    /* Lobby map upload — the PACKET_LOBBY_MAP_UPLOAD_* state machine that
     * used to live in imgui_lobby's per-frame pump. The frontend kicks it
     * off via transportUdpClientStartLobbyMapUpload* and reads progress back
     * via clientSimGetLobbyMapUpload* status + Percent getters. Pump fires
     * from udpClientTick once per tick. After the BEGIN/USE_LOCAL handshake
     * is ACKed the map bytes ride CHANNEL_BULK via uploadSend (a BulkSender),
     * reliably reassembled by the server's bulk receiver. */
    bool      uploadActive;
    uint8_t  *uploadBuf;                  /* malloc'd, sized to uploadTotal */
    uint32_t  uploadTotal;
    uint32_t  uploadOffset;               /* blob bytes handed to the channel,
                                           * for the progress-percent getter   */
    char      uploadName[128];            /* wire-side filename announced to server */
    BulkSender uploadSend;                /* feeds the map bytes onto CHANNEL_BULK */
    bool      uploadBulkStarted;          /* bulkSenderBegin issued post-ACK */
    bool      uploadFedDone;              /* whole blob handed to the channel */
    /* USE_LOCAL pre-check: when the source path resolves under
     * data/maps/, the kick computes md5 + the data/maps-relative
     * filename and sends PACKET_LOBBY_MAP_USE_LOCAL first. On
     * USE_LOCAL_NACK the pump transitions to the BEGIN + bulk-stream flow
     * using the bytes already buffered. */
    bool      uploadUseLocalPending;      /* USE_LOCAL sent, awaiting ACK/NACK */
    bool      uploadBeginSent;            /* BEGIN sent (USE_LOCAL never tried, or NACKed) */
    /* Watchdog timestamps (SDL ticks ms). Reset on forward progress:
     * status flip, a bulk-channel ack advance, or the transition to
     * awaiting-DONE once the whole blob has been handed to the channel. */
    uint64_t  uploadStartedMs;
    uint8_t   uploadPrevStatus;
    uint64_t  uploadPrevProgressMs;
    uint32_t  uploadPrevAcked;            /* last observed CHANNEL_BULK ackedSeq */

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

    /* Voice (CHANNEL_VOICE). voiceSeq stamps outgoing frames and wraps at
     * 256, which is what the receiver's jitter buffer orders on. Inbound
     * frames are parsed off the channel as they arrive and wait in the ring
     * for the frontend to pop them. */
    uint8_t      voiceSeq;
    VoiceRxFrame voiceRx[CLIENT_VOICE_RX_RING];
    uint32_t     voiceRxHead;   /* index of the oldest pending frame */
    uint32_t     voiceRxCount;  /* frames pending, <= CLIENT_VOICE_RX_RING */

    /* Reassembly state machine for sized blobs arriving on CHANNEL_BULK (map
     * preview today). Fed from the stream fragments channelReceive pops; on a
     * completed transfer it dispatches by kind into the client preview state. */
    BulkReceiver bulkRecv;

    /* In-flight spectator bulk blob (SPEC_SEED / SPEC_RECORD) being reassembled
     * on CHANNEL_BULK. onBegin mallocs it and returns it as the receiver's dst;
     * onComplete moves it into the ClientSim spectator feed and clears this.
     * Held on the ctx so a teardown mid-transfer frees it (no leak). At most one
     * in flight — bulk transfers are serialized on the stream. */
    uint8_t *specRecvBuf;
    uint32_t specRecvTotal;

    /* In-flight lobby-chat backlog blob (BULK_KIND_LOBBY_CHAT_BACKLOG) the
     * server sends a returning spectator at the drain-flip. onBegin mallocs it;
     * onComplete walks its [type][bodyLen][body] records and applies each chat
     * event. Held on the ctx so a teardown mid-transfer frees it. */
    uint8_t *lobbyChatBacklogBuf;
    uint32_t lobbyChatBacklogTotal;

    /* Last completed round's replay log (BULK_KIND_ROUND_LOG), pulled with
     * PACKET_ROUND_LOG_REQ. onBegin mallocs roundLogBuf sized to the stream
     * header and onComplete parks it here; it stays owned by this context
     * until transportUdpClientTakeRoundLog hands it out. Plain malloc, not
     * SDL_malloc: the buffer's next owner is lvEmbedBegin, which releases it
     * with free(). The zip is never parsed here — lvEmbedBegin validates it
     * and frees it on failure. */
    uint8_t *roundLogBuf;
    size_t   roundLogLen;             /* roundLogBuf's size (0 when none)     */
    int      roundLogState;           /* ClientRoundLogState (client_net.h)   */
    uint32_t roundLogReqSeq;          /* reqSeq of the outstanding request    */
    uint32_t roundLogSeqCounter;      /* monotonic source for fresh reqSeqs   */
    uint32_t roundLogRequestTick;     /* localTick the first-byte clock started */
    uint32_t roundLogProgressTick;    /* localTick body bytes last advanced   */
    uint32_t roundLogRetries;         /* re-requests after silence            */
    uint32_t roundLogTransientRetries;/* re-requests after a "not now" refusal */
    uint32_t roundLogRetryAtTick;     /* earliest localTick to re-ask after a
                                       * transient refusal (0 = none parked)  */
    uint32_t roundLogWatchdogBytes;   /* bodyReceived at the last stall check;
                                       * a witness for the no-progress timer,
                                       * never the source of the percentage   */

#if WB_ENABLE_NETIMPAIR
    uint8_t test_drop_upload_packet;
#endif

#ifdef __EMSCRIPTEN__
    /* WS↔UDP relay metadata frame (type 0x01). The relay sends exactly one
     * as the first datagram, before any game traffic. Consumed once at the
     * top of udpClientProcessPacket while JOINING; only the prefs blob it
     * carries is acted on (handed to the frontend), so nothing is stashed
     * beyond this consumed-once latch. Zeroed with the rest of the ctx on
     * connect (memset in the connect path). */
    bool     proxyMetaConsumed;
#endif
} TransportUdpClientCtx;

#define UPLOAD_ACK_TIMEOUT_MS   5000   /* BEGIN/USE_LOCAL → ACK */
#define UPLOAD_STALL_TIMEOUT_MS 10000  /* no bulk-ack progress */

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

    if (c->wbnApiToken[0] == '\0' || c->wbnServerKey[0] == '\0') return;

    memset(playerKey, 0, sizeof(playerKey));
#ifdef __EMSCRIPTEN__
    /* WASM has no libcurl, so it cannot mint a player_key
     * (winbolonetClientJoinSession is a stub). Instead it presents its
     * join_code — carried in wbnApiToken for the web build — raw in the
     * reauth token slot. The server's CLIENT_TYPE_WEB branch verifies the
     * join_code read-only. The token rides raw through the command codec (no
     * wbnKeyEncode), matching the server's raw read of the slot. */
    strncpy(playerKey, c->wbnApiToken, sizeof(playerKey) - 1);
#else
    char errMsg[256];
    errMsg[0] = '\0';
    if (!winbolonetClientJoinSession(c->wbnApiToken, c->wbnServerKey,
                                     playerKey, errMsg)) {
        WB_LOG_WARN(WB_LOG_CAT_NET, "[WBN] re-auth exchange failed: %s",
                    errMsg[0] ? errMsg : "(no detail)");
        return;
    }
#endif

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
static void udpClientUploadPump(TransportUdpClientCtx *c, uint64_t now);

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
        stamped.mapEventAck = c->mapEventAck;
        /* Control events ride reliable channel 2; their ack travels on the
         * channel-frame trailer, not an InputPacket field. */
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
    /* A tankless spectator may originate CMD_CHAT; its command carrier runs in
     * the SPECTATING state as well as CONNECTED. The server's spectator inbound
     * branch is the gate that rejects anything but chat. */
    if (c->joinState != UDP_CLIENT_CONNECTED &&
        c->joinState != UDP_CLIENT_SPECTATING) return;
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
    /* SPECTATING is admitted alongside CONNECTED so a spectator's CMD_CHAT
     * reaches the wire; the server rejects any non-chat spectator command. */
    if (c->joinState != UDP_CLIENT_CONNECTED &&
        c->joinState != UDP_CLIENT_SPECTATING) return;
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
 *   per entry: [nameLen 1][name M][isFolder 1][modTime 8 BE]
 *              [scripted 1].
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
            pos + nameLen + 1 + 8 + 1 > len) break;
        if (cs->lobbyMapListCount >= LOBBY_MAP_LIST_MAX) {
            pos += nameLen + 1 + 8 + 1;
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
        cs->lobbyMapListScripted[idx] = buf[pos++] ? true : false;
    }
    if (finalFlag) {
        cs->lobbyMapListReady = true;
        cs->lobbyMapListInFlight = false;
        cs->lobbyMapListSeq++;
    }
}

/* One length-prefixed string into a fixed buffer, cut to fit it. False when
 * the packet runs out before the string does, which stops the entry rather
 * than reading past the buffer. The out buffer is always NUL-terminated. */
static bool udpClientReadLenStr(const uint8_t *buf, int len, int *pos,
                                char *out, size_t outSz) {
    uint8_t n;
    size_t  keep;

    if (*pos + 1 > len) return false;
    n = buf[(*pos)++];
    if (*pos + (int)n > len) return false;
    keep = n;
    if (keep >= outSz) keep = outSz - 1;
    memset(out, 0, outSz);
    if (keep > 0) {
        memcpy(out, buf + *pos, keep);
    }
    *pos += (int)n;
    return true;
}

/* Apply one PACKET_LOBBY_SCENARIO_LIST_RSP chunk to the client's accumulator.
 * Wire format:
 *   [header 8] [final 1] [count 1]
 *   per entry: [fileLen 1][file M][nameLen 1][name N][descLen 1][desc D]
 *              [maxPlayers 1][bots 1][bound 1]
 *
 * No path, unlike the map list: the scenarios directory is flat, so there is
 * nothing to ask about and nothing to recognise a stale response by. The
 * server may emit several chunks per request — entries append and only the
 * final chunk flips Ready/InFlight.
 *
 * Declared in transport_udp.h so unit tests can drive the accumulator
 * directly, as the map list's is. */

/* The three buffers a scenario's file name passes through are the same width,
 * and this is the one translation unit that can see all three names:
 *
 *   SERVER_SCENARIO_FILE_LEN  (server_sim.h)        what the server's own
 *                                                   enumeration hands a
 *                                                   frontend, pinned against
 *                                                   SCN_DIR_FILE_LEN in
 *                                                   server_sim_maps.c
 *   LOBBY_SCENARIO_LIST_FILE_LEN (client_sim_internal.h)  the row this
 *                                                   accumulator fills
 *   LOBBY_SCENARIO_FILE_LEN   (control_event.h)     the name of the scenario
 *                                                   in play on the settings
 *                                                   event
 *
 * Holding them together here means a name that a listing shows in full is a
 * name the chooser can send back and the settings event can carry back, with
 * no site along the way cutting it. */
BOLO_STATIC_ASSERT(LOBBY_SCENARIO_LIST_FILE_LEN == SERVER_SCENARIO_FILE_LEN,
                   scenario_list_file_matches_the_server_entry);
BOLO_STATIC_ASSERT(LOBBY_SCENARIO_LIST_FILE_LEN == LOBBY_SCENARIO_FILE_LEN,
                   scenario_list_file_matches_the_settings_event);

void udpClientHandleLobbyScenarioListRsp(ClientSim *cs,
                                         const uint8_t *buf, int len) {
    int     pos = PACKET_HEADER_SIZE;
    uint8_t finalFlag;
    uint8_t cnt;
    int     i;

    if (!cs) return;
    if (len < PACKET_HEADER_SIZE + 2) return;
    /* Nothing was asked for, so this answers nothing. There is no path in the
       response to tell a stale chunk from a current one the way the map list
       does, so the request being in flight is the whole of what makes a chunk
       this client's: a chunk arriving after the final one — a duplicate, or
       the tail of a request that has since timed out — is dropped rather than
       appended to a finished list. */
    if (!cs->lobbyScenarioListInFlight) return;
    finalFlag = buf[pos++];
    cnt       = buf[pos++];

    /* The accumulator is emptied here rather than where the request is sent,
       so the rows a response builds up are its own and a chunk delivered
       twice cannot double them: the second copy of a first chunk clears and
       refills, and a second copy of a later chunk is dropped above, the
       final flag having taken the request out of flight. */
    if (!cs->lobbyScenarioListStarted) {
        cs->lobbyScenarioListCount   = 0;
        cs->lobbyScenarioListStarted = true;
    }

    for (i = 0; i < cnt; i++) {
        char file[LOBBY_SCENARIO_LIST_FILE_LEN];
        char name[LOBBY_SCENARIO_NAME_LEN];
        char desc[LOBBY_SCENARIO_DESC_LEN];
        int  idx;

        if (!udpClientReadLenStr(buf, len, &pos, file, sizeof(file)) ||
            !udpClientReadLenStr(buf, len, &pos, name, sizeof(name)) ||
            !udpClientReadLenStr(buf, len, &pos, desc, sizeof(desc))) {
            break;
        }
        if (pos + 3 > len) break;
        /* Read into locals first, so a chunk that arrives past the cap is
           still walked to its end rather than leaving the position stranded
           mid-entry. */
        if (cs->lobbyScenarioListCount >= LOBBY_SCENARIO_LIST_MAX) {
            pos += 3;
            continue;
        }
        idx = cs->lobbyScenarioListCount++;
        SDL_strlcpy(cs->lobbyScenarioListFiles[idx], file,
                    LOBBY_SCENARIO_LIST_FILE_LEN);
        SDL_strlcpy(cs->lobbyScenarioListNames[idx], name,
                    LOBBY_SCENARIO_NAME_LEN);
        SDL_strlcpy(cs->lobbyScenarioListDescs[idx], desc,
                    LOBBY_SCENARIO_DESC_LEN);
        cs->lobbyScenarioListMaxPlayers[idx] = buf[pos++];
        cs->lobbyScenarioListBots[idx]       = buf[pos++];
        cs->lobbyScenarioListBound[idx]      = buf[pos++] ? true : false;
    }

    if (finalFlag) {
        cs->lobbyScenarioListReady    = true;
        cs->lobbyScenarioListInFlight = false;
        cs->lobbyScenarioListSeq++;
    }
}

/* Unified bulk-receiver sink for CHANNEL_BULK. onBegin dispatches the parsed
 * stream header by kind (preview / join download / live resync) to the matching
 * receive buffer; onComplete finalises that kind. Defined after the resync
 * helpers it relies on (udpClientFreeResyncBuf, the grace-window constant), so
 * forward-declared here for clientDrainBulk. */
static uint8_t *clientBulkOnBegin(void *ctx, const BulkStreamHeader *h);
static void     clientBulkOnComplete(void *ctx, const BulkStreamHeader *h,
                                     uint8_t *buf);

/* Drain every stream fragment waiting on CHANNEL_BULK through the bulk
 * receiver, dispatching completed transfers (preview / download / resync) by
 * kind into the matching client state. */
static void clientDrainBulk(TransportUdpClientCtx *c) {
    if (c->clientSim == NULL) return;
    BulkRecvSink sink;
    sink.onBegin    = clientBulkOnBegin;
    sink.onComplete = clientBulkOnComplete;
    sink.ctx        = c;
    uint8_t chanBuf[CHANNEL_MAX_SEG];
    uint16_t chanLen;
    while (channelReceive(&c->channelMux, CHANNEL_BULK, chanBuf, &chanLen)) {
        bulkReceiverFeed(&c->bulkRecv, chanBuf, chanLen, &sink);
    }
}

/* Drain every voice frame waiting on CHANNEL_VOICE into the receive ring,
 * where clientSimNetReceiveVoice pops them.  A segment that will not parse
 * is dropped and the drain continues: voice is best-effort and unvalidated
 * on arrival, so a malformed one costs a frame of audio, never the
 * connection. */
static void clientDrainVoice(TransportUdpClientCtx *c) {
    uint8_t chanBuf[CHANNEL_MAX_SEG];
    uint16_t chanLen;

    while (channelReceiveBestEffort(&c->channelMux, CHANNEL_VOICE,
                                    chanBuf, &chanLen)) {
        uint8_t fromPlayer, seq, flags;
        const uint8_t *opus;
        int opusLen;
        uint32_t idx;
        VoiceRxFrame *slot;

        if (!voiceSegmentUnpackDown(chanBuf, (int)chanLen, &fromPlayer, &seq,
                                    &flags, &opus, &opusLen)) {
            continue;
        }

        if (c->voiceRxCount == CLIENT_VOICE_RX_RING) {
            /* Full — retire the oldest to make room for the newest. */
            c->voiceRxHead = (c->voiceRxHead + 1) % CLIENT_VOICE_RX_RING;
            c->voiceRxCount--;
        }
        idx = (c->voiceRxHead + c->voiceRxCount) % CLIENT_VOICE_RX_RING;
        slot = &c->voiceRx[idx];
        slot->fromPlayer = fromPlayer;
        slot->seq = seq;
        slot->flags = flags;
        slot->len = (uint16_t)opusLen;
        memcpy(slot->data, opus, (size_t)opusLen);
        c->voiceRxCount++;
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
        cs->lobbyMapListSeq++;
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
    case CTRL_LOBBY_BRAIN_DOCS_CHUNK: return "LOBBY_BRAIN_DOCS_CHUNK";
    case CTRL_GAME_VOTE_STATE:  return "GAME_VOTE_STATE";
    case CTRL_SERVER_TEXT:      return "SERVER_TEXT";
    case CTRL_COMMAND_REJECTED: return "COMMAND_REJECTED";
    case CTRL_BALANCE_FAILED:   return "BALANCE_FAILED";
    case CTRL_SHELL_DEATH:      return "SHELL_DEATH";
    case CTRL_CHANNEL_RESET:    return "CHANNEL_RESET";
    case CTRL_VOICE_TALKING:    return "VOICE_TALKING";
    case CTRL_ENTITY_CHANGE:    return "ENTITY_CHANGE";
    case CTRL_ENTITY_SYNC:      return "ENTITY_SYNC";
    case CTRL_SIM_RULES:        return "SIM_RULES";
    case CTRL_SCN_PANEL:        return "SCN_PANEL";
    case CTRL_SCN_SCORE:        return "SCN_SCORE";
    case CTRL_SCN_ANNOUNCE:     return "SCN_ANNOUNCE";
    case CTRL_SCN_MARKER:       return "SCN_MARKER";
    case CTRL_SCENARIO_RULES:   return "SCENARIO_RULES";
    default:                    return "<unknown>";
    }
}

/* Apply a CTRL_CHANNEL_RESET: lift the game (channel 0) and map (channel 1)
 * receive baselines to the server's new game-start floors, so a previous-game
 * straggler (seq below the baseline) dedup-drops instead of replaying in the
 * new game. The reset rides the in-order control channel ahead of
 * CTRL_GAME_PHASE_RUNNING, so the lift lands before the running flip. Reads
 * each channel's current expectedSeq / window at the call site (channel_mux
 * exposes them as plain fields): when the stale tail being skipped is wide
 * enough that a new-game event past the live window could fall outside it, the
 * recovery is a retransmit, not a drop — flag that visibility. This event has
 * no sim semantics and must never reach clientSimApplyControl. */
static void udpClientFreeResyncBuf(TransportUdpClientCtx *c);     /* defined below */
static void udpClientFreeRoundLogBuf(TransportUdpClientCtx *c);   /* defined below */
static void clientApplyChannelReset(TransportUdpClientCtx *c,
                                    const ControlEvent *evt) {
    static const struct { uint8_t ch; const char *name; } kChans[3] = {
        { CHANNEL_GAME, "game" },
        { CHANNEL_MAP,  "map"  },
        { CHANNEL_BULK, "bulk" },
    };
    uint32_t baselines[3] = { evt->u.channelReset.ch0Baseline,
                              evt->u.channelReset.ch1Baseline,
                              evt->u.channelReset.ch3Baseline };
    uint8_t mask = evt->u.channelReset.channelMask;
    int i;
    for (i = 0; i < 3; i++) {
        uint8_t ch = kChans[i].ch;
        uint32_t baseline = baselines[i];
        uint32_t expected, window;
        if (!(mask & (1u << ch))) continue;   /* this reset doesn't re-base ch */
        expected = c->channelMux.ch[ch].expectedSeq;
        window   = c->channelMux.ch[ch].window;
        if (baseline > expected && window > 8 &&
            (baseline - expected) > (window - 8)) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                "channel %s baseline reset gap %u nears window %u — an event "
                "past the live window recovers by retransmit, not drop",
                kChans[i].name, (unsigned)(baseline - expected),
                (unsigned)window);
        }
        /* A hostile server can forward-jump this receive baseline via a
         * CTRL_CHANNEL_RESET (skipping the client past buffered seqs). That is
         * memory-safe — channelResetExpected's discard loop is window-bounded —
         * and not a real exposure: the server is already authoritative over its
         * own clients' game/map/bulk streams, and the control channel carrying
         * the reset is connId-authenticated, so a third party can't inject it.
         * Recorded so the forward-jump isn't re-flagged as a finding. */
        channelResetExpected(&c->channelMux, ch, baseline);
        /* A bulk re-base abandons any in-flight transfer: drop the receiver's
         * mid-body partial (so its dst can't dangle) and discard a half-built
         * resync buffer. The new transfer's stream starts at the new baseline
         * and re-allocates via onBegin. mapDownloadBuf is re-pointed by the next
         * download's onBegin, so nothing to free there. */
        if (ch == CHANNEL_BULK) {
            /* A round-log body still being filled rides this channel, so it is
             * abandoned with it. Test that before re-initing the receiver,
             * while its dst still identifies the partial as ours. A blob that
             * already completed is not associated with the channel any more —
             * the transfer is over and a re-base says nothing about it — so it
             * is left alone, buffer, length and state. */
            if (c->roundLogBuf != NULL && c->bulkRecv.dst == c->roundLogBuf) {
                udpClientFreeRoundLogBuf(c);
                c->roundLogState = CLIENT_ROUND_LOG_IDLE;
                c->roundLogReqSeq = 0;
                c->roundLogRetries = 0;
                c->roundLogTransientRetries = 0;
                c->roundLogRetryAtTick = 0;
            }
            bulkReceiverInit(&c->bulkRecv);
            if (c->mapResyncBuf != NULL) {
                udpClientFreeResyncBuf(c);
                c->resyncActive = false;
                c->firstResyncChunkSeen = false;
                c->activeResyncGen = 0;
            }
        }
    }
}

/* Intercept the spectator cold-start countdown on CHANNEL_CONTROL. The server
 * sends it raw ([u8 SPEC_CTRL_COUNTDOWN][u32 remainingTicks BE], 5 bytes) while
 * a tankless spectator waits for its delayed seed — NOT wrapped in the
 * type(1)+bodyLen(2)+body ControlEvent envelope the normal decode assumes. It
 * must be consumed here, before the envelope bodyLen parse, or unpackU16 would
 * mis-read the high half of remainingTicks as a body length. Returns TRUE when
 * the frame was a countdown (the caller skips the envelope decode and consumes
 * the frame). Gated on UDP_CLIENT_SPECTATING so a non-spectator's control decode
 * is untouched. */
/* Set the spectator dual-mode bit and mirror it onto the ClientSim (one-way:
 * the transport owns the bit). The session host reads the mirror via
 * clientSimSpectatorIsLiveLobby / the spectator_drain seam. */
static void udpClientSetSpecLiveLobby(TransportUdpClientCtx *c, bool live) {
    bool wasLive = c->specLiveLobby;
    c->specLiveLobby = live;
    if (c->clientSim != NULL) {
        clientSimSpectatorSetLiveLobby(c->clientSim, live);
        /* Leaving the live lobby for the delayed game is a spectator's
         * equivalent of game start (it never receives CTRL_GAME_PHASE_RUNNING —
         * the server unsubscribes it before that publish, so the normal lobby-
         * history clear never reaches it). Clear the lobby chat here so the
         * pre-game chat doesn't linger behind the delayed game and mix with the
         * post-game catch-up the server replays on return. */
        if (wasLive && !live) {
            clientSimClearLobbyChatHistory(c->clientSim);
        }
    }
}

static bool udpClientInterceptSpecCountdown(TransportUdpClientCtx *c,
                                            const uint8_t *frame, uint16_t len) {
    if (c->joinState != UDP_CLIENT_SPECTATING) return FALSE;
    if (len != SPEC_CTRL_COUNTDOWN_LEN || frame[0] != SPEC_CTRL_COUNTDOWN) {
        return FALSE;
    }
    if (c->clientSim != NULL) {
        clientSimSpectatorSetCountdown(c->clientSim, unpackU32(frame + 1));
    }
    /* The cold-start countdown is the first delayed-ring frame in a short-lobby
     * game (it precedes the seed), so leaving live-lobby mode here keeps the
     * stale lobby from freezing on screen during the countdown. */
    if (c->specLiveLobby) {
        udpClientSetSpecLiveLobby(c, false);
    }
    return TRUE;
}

/* Snapshot-time ordered dispatch for control events arriving on the
 * snapshot tail. Almost all variants forward to clientSimApplyControl;
 * the lobby→running flip carries side-effects that previously lived
 * inside the standalone PACKET_GAME_START handler (install buffered
 * map, reset all three reliable-event acks, clear the input ring, drop
 * any pre-flip snapshot) and they must fire BEFORE the same snapshot's
 * game-event and map-event tails are applied. Those side effects only
 * apply on a real lobby→running flip;
 * a no-lobby joiner's first event is also CTRL_GAME_PHASE_RUNNING (a
 * sync-replay echo from serverSimFillGamePhaseEvent), and for that
 * joiner the same snapshot's tails are current-game state that must
 * not be dropped. We capture wasInLobby up front and gate on it. */
static void clientSimApplyControlOrdered(TransportUdpClientCtx *c,
                                         const ControlEvent *evt,
                                         uint32_t evSeq) {
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
                     " settings[map='%.16s' gameType=%d hiddenMines=%d aiType=%d timeLimit=%d startDelay=%d open=%d autoLock=%d ranked=%d allowNew=%d locks=0x%08x]",
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
                                 (int)c->mapDownloadTotal, NULL,
                                 /*initViewport=*/true);
            c->mapInstalled = true;
        }
        if (wasInLobby) {
            /* The reliable map-event ack is NOT reset at the lobby→running
             * flip.  The server keeps its reliable-queue sequence counters
             * monotonic across game start — it drops the previous game's
             * unacked events (ackedSeq = nextSeq) but never reuses low seq
             * numbers — so mapEventAck stays valid in the
             * same sequence space.  A stale in-flight lobby packet now carries
             * seq numbers below the continuing ack and dedups harmlessly
             * instead of jumping the ack back into the dead lobby space. */
            /* Reset input ring so stale inputs from the previous game are
             * not sent as redundant packets in the new game. */
            c->inputRingCount = 0;
            /* Drop any pre-flip snapshot still buffered in hasSnapshot. */
            c->hasSnapshot = false;
        }
        /* Dispatch the event itself — flips netStat to running, clears
         * inLobby on the sim, etc. The no-lobby joiner still needs this
         * to flip netStat → netRunning even though wasInLobby is false.
         * Pre-flip game/map tails no longer need a skip signal here: the
         * CTRL_CHANNEL_RESET that precedes RUNNING on the control channel has
         * already lifted the game/map receive baselines, so a previous-game
         * straggler is dedup-dropped at the channel before it reaches a drain. */
        clientSimApplyControl(c->clientSim, evt);
        return;
    }
    /* Default path — identical to the legacy direct-dispatch route. */
    clientSimApplyControl(c->clientSim, evt);
}

/* Map resync (desync recovery) timing/limits. localTick runs at 100/s. */
#define MAP_RESYNC_REQUEST_RESEND_TICKS 75   /* ~0.75s between request resends */
#define MAP_RESYNC_GRACE_TICKS         1000  /* ~10s = 2 full-sync intervals */
#define MAP_RESYNC_MAX_ATTEMPTS        10    /* give up + disconnect after this */
/* Require this many consecutive full-sync checksum mismatches before requesting
 * a resync. Full-syncs are FULL_SYNC_INTERVAL ticks apart (~5s), while a
 * transient divergence from in-flight reliable CHANNEL_MAP events self-resolves
 * within ~1 RTT — far inside a single full-sync — so a short debounce drops the
 * spurious resync yet still recovers a divergence that genuinely persists. */
#define MAP_RESYNC_MISMATCH_DEBOUNCE   3
#define MAP_RESYNC_STALL_TICKS         500   /* ~5s of no chunk progress -> abandon
                                              * the in-flight resync (server stopped
                                              * sending / state lost) so the next
                                              * checksum mismatch re-arms */

/* Free the parallel resync reassembly buffer. The bulk receiver holds a pointer
 * to this buffer (its dst) only between onBegin and onComplete; callers that
 * free it mid-transfer (the stall watchdog, a CHANNEL_BULK re-base) must first
 * bulkReceiverInit so no dangling dst remains. */
static void udpClientFreeResyncBuf(TransportUdpClientCtx *c) {
    if (c->mapResyncBuf != NULL) {
        free(c->mapResyncBuf);
        c->mapResyncBuf = NULL;
    }
    c->mapResyncTotal = 0;
}

/* Abandon any in-flight resync (e.g. a wholesale map change supersedes it).
 * Keeps mapResyncCount (cumulative) and resyncGenCounter (monotonic). */
static void udpClientResetResync(TransportUdpClientCtx *c) {
    udpClientFreeResyncBuf(c);
    c->resyncActive = false;
    c->firstResyncChunkSeen = false;
    c->activeResyncGen = 0;
    c->resyncAttempts = 0;
    c->mapMismatchStreak = 0;
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

/* ---- Join-download readiness/watchdog. localTick runs at 100/s. ----
 *
 * Two thresholds, matching the round-log transfer's shape: a READY that drew
 * no stream at all is re-asked quickly (the datagram is cheap and the server
 * treats a pre-stream re-ask as a plain re-arm), while a stream that started
 * and then went silent gets the longer stall window first — the channel's own
 * retransmits recover ordinary loss well inside it, so a stall this long means
 * the transfer is unrecoverable at the channel level (its head was consumed
 * before the buffers were armed, or the server already finished sending) and
 * only a restart re-ask can complete it. */
#define MAP_DL_READY_RESEND_TICKS 100  /* ~1s: no stream yet — re-ask */
#define MAP_DL_STALL_TICKS        500  /* ~5s of zero stream progress — restart */
#define MAP_DL_MAX_RESTARTS        10  /* give up + ERROR after this many re-asks */

/* Tell the server this client's download buffers are armed. The server begins
 * (or, for a re-ask, restarts behind a CHANNEL_BULK re-base) the map stream.
 * connId lets the server drop an address-spoofed READY, which could otherwise
 * reset a healthy client's in-flight transfer. Also re-arms the watchdog's
 * quiet timer so the next threshold measures from this ask. */
static void udpClientSendMapDlReady(TransportUdpClientCtx *c) {
    uint8_t reqBuf[PACKET_HEADER_SIZE + 8];
    packHeader(reqBuf, PACKET_MAP_DL_READY, c->outSequence++);
    packConnId(reqBuf + PACKET_HEADER_SIZE, c->connId);
    udpClientSendTo(c, reqBuf, sizeof(reqBuf));
    c->dlProgressTick = c->localTick;
}

/* ---- Round-log transfer (BULK_KIND_ROUND_LOG). localTick runs at 100/s. ----
 *
 * Two independent deadlines, not one total budget. A request has FIRST_BYTE
 * ticks to produce the head of its stream, which buys one silent re-request
 * (the request datagram itself can be lost); once bytes are arriving, the
 * transfer is only abandoned after NO_PROGRESS ticks with nothing further
 * received — a 4 MB blob on a slow link makes progress the whole way and must
 * never be killed on elapsed time alone.
 *
 * The server stamps its own per-client 2 s interval for every request it
 * considers, refused ones included, so re-asking sooner than RETRY ticks after
 * a transient refusal only earns a second refusal.
 *
 * A refusal is not silence — it proves the server is alive and answering — so
 * it restarts the first-byte deadline and is bounded by a count of its own.
 * That ceiling is sized to outwait the server's concurrency cap: two full-size
 * transfers can be ahead of this one, and at RETRY ticks apart 16 tries is
 * about 40 s of asking. */
#define ROUND_LOG_FIRST_BYTE_TICKS   1000  /* 10s request -> head of stream   */
#define ROUND_LOG_NO_PROGRESS_TICKS  1000  /* 10s of a stalled transfer       */
#define ROUND_LOG_RETRY_TICKS        250   /* 2.5s after a transient refusal  */
#define ROUND_LOG_FIRST_BYTE_RETRIES 1     /* re-requests after silence       */
#define ROUND_LOG_MAX_TRANSIENT_RETRIES 16 /* re-requests after "not now"     */

/* Drop the round-log blob this context owns — a completed-but-untaken one or a
 * body still being filled. The bulk receiver holds the buffer as its dst
 * between onBegin and onComplete: clear that first, and clear it by NULLing
 * rather than re-initing the receiver, so the rest of the body is consumed and
 * discarded and the byte stream stays aligned for the next transfer. */
static void udpClientFreeRoundLogBuf(TransportUdpClientCtx *c) {
    if (c->roundLogBuf != NULL) {
        if (c->bulkRecv.dst == c->roundLogBuf) {
            c->bulkRecv.dst = NULL;
        }
        free(c->roundLogBuf);
        c->roundLogBuf = NULL;
    }
    c->roundLogLen = 0;
    c->roundLogWatchdogBytes = 0;
}

/* Put PACKET_ROUND_LOG_REQ on the wire under a fresh reqSeq. The server echoes
 * it as the stream header's gen (and in any refusal), which is what lets a
 * reply to a superseded request be recognised and dropped. */
static void udpClientSendRoundLogReq(TransportUdpClientCtx *c) {
    uint8_t reqBuf[PACKET_HEADER_SIZE + 4];
    c->roundLogSeqCounter++;
    if (c->roundLogSeqCounter == 0) c->roundLogSeqCounter = 1;
    c->roundLogReqSeq = c->roundLogSeqCounter;
    packHeader(reqBuf, PACKET_ROUND_LOG_REQ, c->outSequence++);
    packU32(reqBuf + PACKET_HEADER_SIZE, c->roundLogReqSeq);
    udpClientSendTo(c, reqBuf, sizeof(reqBuf));
}

/* PACKET_ROUND_LOG_ERR: the server refused. DISABLED / NONE / TOO_LARGE hold
 * for as long as the round does and each becomes its own state so the caller
 * can word them apart. BUSY and RATE_LIMITED are the same thing to us — "not
 * now" — and leave the state at WAITING with a retry parked; an unrecognised
 * code is treated the same way. Each refusal restarts the first-byte deadline,
 * which is testing for silence and has just been answered, and counts against
 * the transient ceiling instead. */
static void udpClientHandleRoundLogErr(TransportUdpClientCtx *c,
                                       uint32_t reqSeq, uint8_t code) {
    if (c->roundLogState != CLIENT_ROUND_LOG_WAITING) return;
    if (reqSeq != c->roundLogReqSeq) return;   /* answers a superseded request */
    switch (code) {
    case ROUND_LOG_ERR_DISABLED:
        c->roundLogState = CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED;
        c->roundLogRetryAtTick = 0;
        break;
    case ROUND_LOG_ERR_NONE:
        c->roundLogState = CLIENT_ROUND_LOG_UNAVAILABLE_NONE;
        c->roundLogRetryAtTick = 0;
        break;
    case ROUND_LOG_ERR_TOO_LARGE:
        c->roundLogState = CLIENT_ROUND_LOG_UNAVAILABLE_TOO_LARGE;
        c->roundLogRetryAtTick = 0;
        break;
    default:
        c->roundLogTransientRetries++;
        if (c->roundLogTransientRetries > ROUND_LOG_MAX_TRANSIENT_RETRIES) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                "round log refused %u times running -> unavailable",
                (unsigned)c->roundLogTransientRetries);
            c->roundLogState = CLIENT_ROUND_LOG_UNAVAILABLE_NONE;
            c->roundLogRetryAtTick = 0;
            break;
        }
        c->roundLogRequestTick = c->localTick;
        c->roundLogRetryAtTick = c->localTick + ROUND_LOG_RETRY_TICKS;
        break;
    }
}

/* Per-tick deadlines for an outstanding round-log request. */
static void udpClientRoundLogTick(TransportUdpClientCtx *c) {
    if (c->roundLogState == CLIENT_ROUND_LOG_WAITING) {
        if (c->roundLogRetryAtTick != 0 &&
            c->localTick >= c->roundLogRetryAtTick) {
            c->roundLogRetryAtTick = 0;
            udpClientSendRoundLogReq(c);
            return;   /* one request per tick; the deadline below waits a tick */
        }
        if ((uint32_t)(c->localTick - c->roundLogRequestTick) >=
            ROUND_LOG_FIRST_BYTE_TICKS) {
            if (c->roundLogRetries < ROUND_LOG_FIRST_BYTE_RETRIES) {
                c->roundLogRetries++;
                c->roundLogRequestTick = c->localTick;
                c->roundLogRetryAtTick = 0;
                udpClientSendRoundLogReq(c);
            } else {
                WB_LOG_WARN(WB_LOG_CAT_NET,
                    "round log request produced no bytes -> unavailable");
                c->roundLogState = CLIENT_ROUND_LOG_UNAVAILABLE_NONE;
                c->roundLogRetryAtTick = 0;
            }
        }
        return;
    }

    if (c->roundLogState == CLIENT_ROUND_LOG_DOWNLOADING) {
        uint32_t have = (c->roundLogBuf != NULL &&
                         c->bulkRecv.dst == c->roundLogBuf)
                            ? c->bulkRecv.bodyReceived : 0u;
        if (have != c->roundLogWatchdogBytes) {
            c->roundLogWatchdogBytes = have;
            c->roundLogProgressTick  = c->localTick;
            return;
        }
        if ((uint32_t)(c->localTick - c->roundLogProgressTick) >=
            ROUND_LOG_NO_PROGRESS_TICKS) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                "round log transfer stalled at %u bytes -> abandon",
                (unsigned)have);
            udpClientFreeRoundLogBuf(c);
            c->roundLogState = CLIENT_ROUND_LOG_UNAVAILABLE_NONE;
            c->roundLogRetryAtTick = 0;
        }
    }
}

/* Bulk-receiver onBegin (CHANNEL_BULK): a full stream header parsed. Dispatch by
 * kind to the matching receive buffer; return NULL to reject (the body is then
 * consumed and discarded so the stream stays aligned). */
static uint8_t *clientBulkOnBegin(void *ctx, const BulkStreamHeader *h) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;
    ClientSim *cs = c ? c->clientSim : NULL;
    if (cs == NULL) return NULL;

    switch (h->kind) {
    case BULK_KIND_PREVIEW:
        /* A preview whose path doesn't match the in-flight request is dropped —
         * the user navigated away before this arrived. */
        if (strncmp(h->path, cs->lobbyMapPreviewReqPath,
                    sizeof(cs->lobbyMapPreviewReqPath)) != 0) {
            return NULL;
        }
        if (h->totalSize == 0 || h->totalSize > LOBBY_MAP_UPLOAD_MAX_BYTES) {
            cs->lobbyMapPreviewError    = true;
            cs->lobbyMapPreviewInFlight = false;
            return NULL;
        }
        cs->lobbyMapPreviewTotal    = h->totalSize;
        cs->lobbyMapPreviewReceived = 0;
        cs->lobbyMapPreviewReady    = false;
        cs->lobbyMapPreviewError    = false;
        cs->lobbyMapPreviewInFlight = true;
        memset(cs->lobbyMapPreviewPath, 0, sizeof(cs->lobbyMapPreviewPath));
        SDL_strlcpy(cs->lobbyMapPreviewPath, h->path,
                    sizeof(cs->lobbyMapPreviewPath));
        return cs->lobbyMapPreviewBytes;

    case BULK_KIND_DOWNLOAD:
        /* Join download (player) OR live-lobby map download (spectator):
         * reassemble into the buffer JOIN_ACCEPT sized from the accept's mapSize.
         * The header's totalSize must match it. A spectator stays SPECTATING and
         * is gated on specLobbyMapDownloading so the seed stream can't be mistaken
         * for a join download. */
        if (c->joinState != UDP_CLIENT_DOWNLOADING_MAP &&
            !(c->joinState == UDP_CLIENT_SPECTATING &&
              c->specLobbyMapDownloading)) return NULL;
        if (c->mapDownloadBuf == NULL) return NULL;
        if (h->totalSize != c->mapDownloadTotal) return NULL;
        c->mapDownloadReceived = 0;
        return c->mapDownloadBuf;

    case BULK_KIND_RESYNC:
        /* Live resync: accept only the request currently outstanding; a stale
         * gen from a superseded resync is dropped (the gen gate). Allocate the
         * parallel buffer from the header's self-describing size. */
        if (!c->resyncActive || h->gen != c->activeResyncGen) return NULL;
        if (h->totalSize == 0 || h->totalSize > MAP_DOWNLOAD_MAX_SIZE) return NULL;
        udpClientFreeResyncBuf(c);          /* drop any prior partial */
        c->mapResyncBuf = (BYTE *)malloc(h->totalSize);
        if (c->mapResyncBuf == NULL) return NULL;
        c->mapResyncTotal = h->totalSize;
        /* The stream started: stop resending the request and mark progress for
         * the stall watchdog. */
        c->firstResyncChunkSeen = true;
        c->lastResyncProgressTick = c->localTick;
        return c->mapResyncBuf;

    case BULK_KIND_SPEC_SEED:
    case BULK_KIND_SPEC_RECORD:
        /* Spectator feed: the seed (raw delayed keyframe) and the forward
         * records both arrive here while connected as a spectator. Allocate a
         * fresh buffer sized to the wire totalSize; onComplete moves it into the
         * ClientSim feed. A SPEC_RECORD carries the server's 9-byte header, so
         * its body must be at least that long. totalSize is attacker-controlled
         * (see the sink contract) — bound it by the same cap the map blobs use. */
        if (c->joinState != UDP_CLIENT_SPECTATING) return NULL;
        if (h->totalSize == 0 || h->totalSize > MAP_DOWNLOAD_MAX_SIZE) return NULL;
        if (h->kind == BULK_KIND_SPEC_RECORD && h->totalSize < SPEC_RECORD_HEADER_LEN) {
            return NULL;
        }
        if (c->specRecvBuf != NULL) free(c->specRecvBuf);  /* drop any stale partial */
        c->specRecvBuf = (uint8_t *)malloc(h->totalSize);
        if (c->specRecvBuf == NULL) return NULL;
        c->specRecvTotal = h->totalSize;
        return c->specRecvBuf;

    case BULK_KIND_LOBBY_CHAT_BACKLOG:
        /* The returning spectator's lobby-chat catch-up. Only meaningful while
         * SPECTATING; bound the attacker-controlled totalSize by the same cap the
         * other spectator blobs use. onComplete walks the records and applies the
         * chat through the normal lobby-chat path. */
        if (c->joinState != UDP_CLIENT_SPECTATING) return NULL;
        if (h->totalSize == 0 || h->totalSize > MAP_DOWNLOAD_MAX_SIZE) return NULL;
        if (c->lobbyChatBacklogBuf != NULL) free(c->lobbyChatBacklogBuf);
        c->lobbyChatBacklogBuf = (uint8_t *)malloc(h->totalSize);
        if (c->lobbyChatBacklogBuf == NULL) return NULL;
        c->lobbyChatBacklogTotal = h->totalSize;
        return c->lobbyChatBacklogBuf;

    case BULK_KIND_ROUND_LOG:
        /* The last completed round's .wbv, answering this client's
         * PACKET_ROUND_LOG_REQ. Accept only while a request is outstanding and
         * only when the header's gen echoes that request's reqSeq — anything
         * else answers a superseded ask. totalSize is attacker-controlled (see
         * the sink contract), so bound it by the wire cap before allocating.
         * malloc, not SDL_malloc: the buffer leaves through
         * transportUdpClientTakeRoundLog for lvEmbedBegin, which frees it with
         * plain free(). h->path is the log's basename and is a label only —
         * nothing here opens it. */
        if (c->roundLogState != CLIENT_ROUND_LOG_WAITING) return NULL;
        if (h->gen != c->roundLogReqSeq) return NULL;
        if (h->totalSize == 0 || h->totalSize > ROUND_LOG_MAX_BYTES) return NULL;
        udpClientFreeRoundLogBuf(c);        /* drop anything held for an older ask */
        c->roundLogBuf = (uint8_t *)malloc(h->totalSize);
        if (c->roundLogBuf == NULL) return NULL;
        c->roundLogLen           = (size_t)h->totalSize;
        c->roundLogState         = CLIENT_ROUND_LOG_DOWNLOADING;
        c->roundLogProgressTick  = c->localTick;
        c->roundLogWatchdogBytes = 0;
        c->roundLogRetryAtTick   = 0;
        return c->roundLogBuf;

    default:
        return NULL;
    }
}

/* Finalise a reassembled map resync. mapResyncBuf/mapResyncTotal hold the blob
 * for generation activeResyncGen. A resync only ever follows a successful first
 * map install, so the viewport's view buffers already exist and the install
 * runs with initViewport=false to keep the player's camera over the swap.
 *
 * On a successful install: advance installedMapGen to this resync's gen (older
 * map-channel events then drop on the drain — the blob already carries every
 * change up to the cut), open the grace window, and count it. On a FAILED
 * install: keep the prior map and re-arm WITHOUT advancing the generation, the
 * grace window, or the count, so the next full-sync checksum mismatch
 * re-requests (MAP_RESYNC_MAX_ATTEMPTS still bounds the loop). Either way the
 * resync buffer and the outstanding-request flags are cleared. */
static void udpClientFinalizeResync(TransportUdpClientCtx *c) {
    ClientSim *cs = c ? c->clientSim : NULL;
    if (cs == NULL) return;

    if (!installCompressedMap(cs, c->mapResyncBuf, (int)c->mapResyncTotal, NULL,
                              /*initViewport=*/false)) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "map resync install FAILED gen=%u (%u bytes) - keeping prior map, will retry",
            (unsigned)c->activeResyncGen, (unsigned)c->mapResyncTotal);
        c->resyncActive = false;
        c->firstResyncChunkSeen = false;
        c->activeResyncGen = 0;
        udpClientFreeResyncBuf(c);
        return;
    }

    c->mapResyncCount++;
    c->resyncActive = false;
    c->firstResyncChunkSeen = false;
    c->installedMapGen = c->activeResyncGen;
    c->activeResyncGen = 0;
    c->resyncSuppressUntilTick = c->localTick + MAP_RESYNC_GRACE_TICKS;
    udpClientFreeResyncBuf(c);
    WB_LOG_INFO(WB_LOG_CAT_NET,
        "map resync install complete (count=%u)", (unsigned)c->mapResyncCount);
}

/* Bulk-receiver onComplete (CHANNEL_BULK): the whole blob for `buf` (the buffer
 * onBegin returned) has landed. Finalise by kind — the same finishes the old
 * per-chunk paths drove. */
static void clientBulkOnComplete(void *ctx, const BulkStreamHeader *h,
                                 uint8_t *buf) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)ctx;
    ClientSim *cs = c ? c->clientSim : NULL;
    (void)buf;
    if (cs == NULL) return;

    switch (h->kind) {
    case BULK_KIND_PREVIEW:
        cs->lobbyMapPreviewReceived = h->totalSize;
        cs->lobbyMapPreviewReady    = true;
        cs->lobbyMapPreviewInFlight = false;
        break;

    case BULK_KIND_DOWNLOAD:
        /* Whole map reassembled. Install immediately (lobby or running) and
         * publish CTRL_MAP_DOWNLOAD_COMPLETE — the finish the old final-chunk path
         * drove. A live-lobby spectator installs the lobby map for its preview but
         * STAYS UDP_CLIENT_SPECTATING (it never joined a tank); a player flips to
         * CONNECTED. Order: length -> state -> install -> flag -> event. */
        c->mapDownloadReceived = h->totalSize;
        if (c->joinState == UDP_CLIENT_SPECTATING) {
            installCompressedMap(cs, c->mapDownloadBuf,
                                 (int)c->mapDownloadTotal, NULL,
                                 /*initViewport=*/true);
            c->mapInstalled = true;
            c->specLobbyMapDownloading = false;
            {
                ControlEvent evt = { .type = CTRL_MAP_DOWNLOAD_COMPLETE };
                clientSimApplyControl(cs, &evt);
            }
            WB_LOG_INFO(WB_LOG_CAT_NET,
                "spectator lobby map installed (%u bytes), staying SPECTATING",
                (unsigned)h->totalSize);
            break;
        }
        c->joinState = UDP_CLIENT_CONNECTED;
        installCompressedMap(cs, c->mapDownloadBuf,
                             (int)c->mapDownloadTotal, NULL,
                             /*initViewport=*/true);
        c->mapInstalled = true;
        {
            ControlEvent evt = { .type = CTRL_MAP_DOWNLOAD_COMPLETE };
            clientSimApplyControl(cs, &evt);
        }
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "map download complete via bulk -> CONNECTED (playerNum=%u)",
            (unsigned)c->playerNum);
        break;

    case BULK_KIND_RESYNC:
        /* Atomic terrain swap — joinState never changed, so snapshots and
         * entities kept flowing. NULL name keeps the current map name. The
         * server-held map events (seq >= cut) flow on the next snapshots and
         * apply on top of the freshly installed blob. initViewport=false (inside
         * the finalize): a mid-game resync keeps the player's camera. A failed
         * install keeps the prior map and does not advance the generation. */
        udpClientFinalizeResync(c);
        break;

    case BULK_KIND_SPEC_SEED:
        /* The whole seed blob (raw delayed keyframe) has landed. Move it into
         * the ClientSim feed, transferring ownership; the session translates and
         * loads it later. */
        clientSimSpectatorPushSeed(cs, c->specRecvBuf, h->totalSize);
        c->specRecvBuf = NULL;
        c->specRecvTotal = 0;
        /* A delayed-ring frame landed — leave live-lobby mode (covers the
         * delay=0 path where the seed arrives with no preceding countdown). */
        udpClientSetSpecLiveLobby(c, false);
        break;

    case BULK_KIND_SPEC_RECORD:
        /* One forward record. Strip the 9-byte transport header and queue the
         * parsed fields + the raw ring payload (which may be empty for an idle
         * tick). onBegin guaranteed totalSize >= SPEC_RECORD_HEADER_LEN. */
        if (c->specRecvBuf != NULL && h->totalSize >= SPEC_RECORD_HEADER_LEN) {
            bool     isKf      = c->specRecvBuf[0] != 0;
            uint32_t gameTick  = unpackU32(c->specRecvBuf + 1);
            uint32_t segment   = unpackU32(c->specRecvBuf + 5);
            uint32_t payloadLen = h->totalSize - SPEC_RECORD_HEADER_LEN;
            clientSimSpectatorPushRecord(cs, isKf, gameTick, segment,
                                         c->specRecvBuf + SPEC_RECORD_HEADER_LEN,
                                         payloadLen);
        }
        free(c->specRecvBuf);
        c->specRecvBuf = NULL;
        c->specRecvTotal = 0;
        /* A delayed-ring frame landed — leave live-lobby mode. */
        udpClientSetSpecLiveLobby(c, false);
        break;

    case BULK_KIND_LOBBY_CHAT_BACKLOG: {
        /* The whole backlog blob has landed: a run of [type][bodyLen BE][body]
         * control-event records (oldest first). Decode each with the same codec
         * the control channel uses and apply the chat through clientSimApplyControl
         * — broadcast CTRL_CHAT and CTRL_SPECTATOR_CHAT only (what the server
         * buffered). The sync replay that re-registered this spectator already set
         * inLobby, so the chat lands in lobbyChatHistory. */
        const uint8_t *p = c->lobbyChatBacklogBuf;
        uint32_t remaining = c->lobbyChatBacklogTotal;
        while (p != NULL && remaining >= 3) {
            uint8_t  type    = p[0];
            uint16_t bodyLen = unpackU16(p + 1);
            uint32_t recLen  = 3u + bodyLen;
            ControlEvent evt;
            ControlDecodeBodyFn dec;
            if (recLen > remaining) break;       /* truncated tail — stop */
            dec = transportControlCodecBodyDecoder((ControlEventType)type);
            if (dec != NULL && dec(p + 3, bodyLen, &evt) &&
                (evt.type == CTRL_CHAT || evt.type == CTRL_SPECTATOR_CHAT)) {
                clientSimApplyControl(cs, &evt);
            }
            p += recLen;
            remaining -= recLen;
        }
        free(c->lobbyChatBacklogBuf);
        c->lobbyChatBacklogBuf = NULL;
        c->lobbyChatBacklogTotal = 0;
        break;
    }

    case BULK_KIND_ROUND_LOG:
        /* The whole .wbv has landed in roundLogBuf. Park it and stop there:
         * the zip is neither parsed nor validated here — lvEmbedBegin does
         * that once the caller takes ownership, and frees the buffer itself if
         * it refuses. */
        c->roundLogLen           = (size_t)h->totalSize;
        c->roundLogState         = CLIENT_ROUND_LOG_READY;
        c->roundLogWatchdogBytes = 0;
        c->roundLogRetryAtTick   = 0;
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "round log received (%u bytes)", (unsigned)h->totalSize);
        break;

    default:
        break;
    }
}

/* Parse the WS↔UDP relay's 0x01 metadata frame. Layout (Phase 0):
 *   [0]      0x01
 *   [1]      N           name length
 *   [2..]    name        N bytes, UTF-8, not NUL-terminated
 *   [2+N]    wbnFlag     0x00 / 0x01  (is_logged_in)
 *   [3+N]    country0    ASCII / '?'
 *   [4+N]    country1    ASCII / ' ' / '?'
 *   [5+N]    prefsLen    uint16 big-endian (L)
 *   [7+N]    prefs       L bytes, raw JSON
 * Compiled on every platform (only the consume site below is
 * emscripten-gated) so tests/unit/test_proxy_meta_parse.c can drive it.
 * See transport_udp_internal.h for the tolerance contract. */
void transportUdpParseProxyMeta(const uint8_t *buf, int len,
                                ProxyMetaFrame *out) {
    memset(out, 0, sizeof(*out));

    int pos = 1;  /* past the 0x01 type byte */
    if (pos >= len) return;
    int nameLen = buf[pos++];
    if (pos + nameLen > len) return;  /* truncated */
    {
        /* Clamp only the copy; pos advances by the wire length so the
         * fields after an oversized name stay correctly framed. */
        int copyLen = nameLen;
        if (copyLen > (int)sizeof(out->name) - 1) copyLen = (int)sizeof(out->name) - 1;
        memcpy(out->name, buf + pos, (size_t)copyLen);
        out->name[copyLen] = '\0';
    }
    pos += nameLen;

    if (pos >= len) return;
    out->wbn = (buf[pos++] != 0);

    if (pos + 2 > len) return;
    out->country[0] = (char)buf[pos++];
    out->country[1] = (char)buf[pos++];
    out->country[2] = '\0';

    if (pos + 2 > len) return;
    int prefsLen = (buf[pos] << 8) | buf[pos + 1];  /* big-endian */
    pos += 2;
    if (prefsLen > (int)sizeof(out->prefs) - 1) prefsLen = (int)sizeof(out->prefs) - 1;
    if (pos + prefsLen > len) prefsLen = len - pos;  /* clamp to available */
    if (prefsLen < 0) prefsLen = 0;
    memcpy(out->prefs, buf + pos, (size_t)prefsLen);
    out->prefs[prefsLen] = '\0';
    out->prefsLen = prefsLen;
}

/* Process a single incoming packet (used by both direct and delayed paths) */
#ifdef __EMSCRIPTEN__
/* Consume the relay's one-shot metadata frame: log it and hand the prefs
 * blob to the front-end (main_wasm.c), which owns the live keys and menu
 * globals; the transport just forwards the JSON. */
static void udpClientConsumeProxyMeta(const uint8_t *buf, int len) {
    ProxyMetaFrame m;
    transportUdpParseProxyMeta(buf, len, &m);

    WB_LOG_INFO(WB_LOG_CAT_NET,
                "[WASM] proxy metadata: name='%s' wbn=%d country=%.2s prefsLen=%d",
                m.name, m.wbn ? 1 : 0, m.country, m.prefsLen);

    if (m.prefsLen > 0) {
        extern void wasmApplyJoinPrefs(const char *prefsJson, int len);
        wasmApplyJoinPrefs(m.prefs, m.prefsLen);
    }
}
#endif

static void udpClientProcessPacket(TransportUdpClientCtx *c,
                                   const uint8_t *buf, int len) {
#ifdef __EMSCRIPTEN__
    /* The WS↔UDP relay sends one 0x01 metadata frame as the first datagram,
     * before any game traffic. Real game packets always begin with the 'W''B'
     * magic (getPacketType), so a 0x01 first byte unambiguously marks the
     * frame — no game packet can collide. Consume it once while JOINING —
     * applying the forwarded prefs blob to the frontend — and never hand it
     * to the game-packet path. */
    if (c->joinState == UDP_CLIENT_JOINING && !c->proxyMetaConsumed &&
        len >= 1 && buf[0] == PROXY_META_FRAME_TYPE) {
        c->proxyMetaConsumed = true;
        udpClientConsumeProxyMeta(buf, len);
        return;
    }
#endif
    uint8_t pktType = getPacketType(buf, len);

#if WB_ENABLE_NETIMPAIR
    if (pktType != 0 && pktType == c->test_drop_upload_packet) return;
#endif

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
             c->joinState == UDP_CLIENT_DOWNLOADING_MAP ||
             c->joinState == UDP_CLIENT_SPECTATING) &&
            len >= PACKET_HEADER_SIZE + 9) {
            int pos = PACKET_HEADER_SIZE;
            uint32_t mapSize;
            BYTE assignedSlot = buf[pos++];

            /* Tankless spectator accept. The server answers a
             * JOIN_FLAG_SPECTATOR join with the SPECTATOR_ACCEPT_NO_SLOT
             * sentinel: no tank slot is claimed. The map size is real in
             * lobby/countdown (the live-lobby map is downloaded below for the
             * preview) and 0 while running (the game-time map rides the seed).
             * Intercept here — before the slot>=MAX_TANKS reject the 0xFF
             * sentinel would otherwise trip — and skip the tank-slot funnel
             * (clientSimOnAssignedSlot) and the live snapshot-apply pipeline;
             * land in UDP_CLIENT_SPECTATING. Gated on c->spectator so a
             * player-join client never takes this path: for it, a 0xFF slot
             * falls through to the out-of-range reject below. */
            if (c->spectator && assignedSlot == SPECTATOR_ACCEPT_NO_SLOT) {
                /* serverTick seeds the timing estimator's clock offset (no
                 * round-trip sample — same rationale as the player path). */
                clientTimingSeedFromJoin(&c->timing, unpackU32(buf + pos), 0);
                pos += 4;
                /* mapSize: 0 while a game runs (the map rides the delayed seed),
                 * non-zero in lobby/countdown — the current lobby map, downloaded
                 * below over CHANNEL_BULK for the lobby preview. Read it here
                 * (the player path's funnel below is skipped for spectators). */
                uint32_t specMapSize = unpackU32(buf + pos);
                pos += 4;
                /* Optional connId trailer, read exactly as the player path so
                 * the server can re-home this spectator after a NAT rebind. */
                if (len >= PACKET_HEADER_SIZE + 9 + 8) {
                    c->connId = unpackConnId(buf + pos);
                    pos += 8;
                }
                /* Initial spectator mode byte, appended after the connId
                 * trailer: 1 = the server was in lobby/countdown at accept time
                 * (watch the live lobby), 0 = a running game (delayed ring).
                 * Length-gated like the connId trailer; a short accept that
                 * omits it defaults to delayed. Seeds the dual-mode bit so the
                 * session host picks the right view before any feed arrives. */
                {
                    bool liveLobby = false;
                    if (len >= PACKET_HEADER_SIZE + 9 + 8 + 1) {
                        liveLobby = (buf[pos] != 0);
                        pos++;
                    }
                    udpClientSetSpecLiveLobby(c, liveLobby);
                }
                /* Live-lobby map download. A non-zero size means the server is
                 * streaming the current lobby map (BULK_KIND_DOWNLOAD) so the
                 * lobby preview/starts render. Allocate the receive buffer and arm
                 * the bulk gate, but stay UDP_CLIENT_SPECTATING — this is NOT the
                 * player game-start pipeline. A re-accept (mid-lobby map change /
                 * return-to-lobby) re-allocates for the new size and re-arms; the
                 * old buffer is freed first so nothing leaks. The server pairs the
                 * re-accept with a CTRL_CHANNEL_RESET, so the bulk receiver re-bases
                 * cleanly. mapInstalled goes false until the new map lands, which
                 * holds the preview on its prior frame (no half-map). */
                /* Duplicate spectator accept for the lobby-map fetch already in
                 * flight: same wipe hazard as the player dup-accept guard — a
                 * mid-stream bulkReceiverInit loses the body framing and the
                 * rest of the stream is swallowed. Same size while still
                 * downloading means this accept describes the fetch we are
                 * already receiving; keep the armed state. A finished fetch
                 * (specLobbyMapDownloading false) re-arms below as before —
                 * that is the mid-lobby map-change re-accept. */
                if (specMapSize != 0 &&
                    c->joinState == UDP_CLIENT_SPECTATING &&
                    c->specLobbyMapDownloading &&
                    c->mapDownloadBuf != NULL &&
                    specMapSize == c->mapDownloadTotal) {
                    break;
                }
                if (specMapSize != 0 && specMapSize <= MAP_DOWNLOAD_MAX_SIZE) {
                    if (c->mapDownloadBuf != NULL) {
                        free(c->mapDownloadBuf);
                        c->mapDownloadBuf = NULL;
                    }
                    c->mapDownloadBuf = (BYTE *)malloc(specMapSize);
                    if (c->mapDownloadBuf != NULL) {
                        memset(c->mapDownloadBuf, 0, specMapSize);
                        c->mapDownloadTotal = specMapSize;
                        c->mapDownloadReceived = 0;
                        if (c->mapInstalled) c->mapInvalidateCount++;
                        c->mapInstalled = false;
                        c->specLobbyMapDownloading = true;
                        bulkReceiverInit(&c->bulkRecv);
                    }
                }
                c->joinState = UDP_CLIENT_SPECTATING;
                WB_LOG_INFO(WB_LOG_CAT_NET,
                    "spectator JOIN_ACCEPT: tankless connect, mapSize=%u",
                    (unsigned)specMapSize);
                break;
            }

            /* A valid server only ever assigns slots 0..MAX_TANKS-1. An
             * out-of-range slot from a hostile or buggy server would make
             * myPlayerNum index the player/tank/lobby arrays out of bounds
             * throughout the client — reject the join instead. */
            if (assignedSlot >= MAX_TANKS) {
                c->joinState = UDP_CLIENT_ERROR;
                break;
            }

            /* Duplicate accept for the download already in flight. The server
             * re-sends the accept for every JOIN it hears from a connected
             * address, and the client's JOIN retries make a second accept
             * routine under lag. Re-running the re-arm below mid-stream would
             * reset the bulk receiver's framing in the middle of a body — the
             * remaining stream bytes would then parse as a garbage stream
             * header and every byte after them would be silently swallowed,
             * wedging the download with no recovery (the channel has already
             * acked the bytes, so the server never re-sends them). Same slot
             * and same size mean the accept describes the download we are
             * already receiving; drop it. A different size falls through — the
             * map changed under us, and the full re-arm (plus the READY-driven
             * restart) is exactly what recovers that. */
            if (c->joinState == UDP_CLIENT_DOWNLOADING_MAP &&
                c->mapDownloadBuf != NULL &&
                assignedSlot == c->playerNum &&
                unpackU32(buf + pos + 4) == c->mapDownloadTotal) {
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
            if (c->mapInstalled) c->mapInvalidateCount++;
            c->mapInstalled = false;

            /* A fresh full download (initial join or a wholesale map change)
             * supersedes any in-flight resync — drop its buffer and state so a
             * stale resync can't install over the new map or wedge detection. */
            udpClientResetResync(c);

            /* Re-base the bulk receiver for the fresh download stream: drop any
             * mid-body partial so its dst can't dangle into a freed buffer and
             * the next stream header (the new download) parses clean. On a
             * wholesale map change the server also re-bases CHANNEL_BULK and
             * carries the new baseline in a CTRL_CHANNEL_RESET; this is the
             * app-side half of that re-base. */
            bulkReceiverInit(&c->bulkRecv);

            c->joinState = UDP_CLIENT_DOWNLOADING_MAP;

            /* Readiness round-trip: the server holds the map stream until this
             * client's PACKET_MAP_DL_READY, so the stream head can never arrive
             * before the buffers above exist (an unsolicited stream drained
             * while still JOINING was consumed with nowhere to put it, and the
             * channel's acks meant the server never re-sent it — the wedged
             * "Downloading map…" lobby). The per-tick standalone PACKET_CHANNEL
             * (sent during DOWNLOADING_MAP) acks the stream as it arrives, and
             * the watchdog in transportUdpClientTick re-asks if the READY is
             * lost or the stream stalls. */
            c->dlProgressBytes = 0;
            c->dlReadyResends = 0;
            udpClientSendMapDlReady(c);
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
        uint8_t baseCount, pillCount;
        int newEventCount = (c->hasSnapshot) ? c->snapshotHdr.reliableEventCount : 0;
        /* Index into snapshotEvents where this snapshot's channel-drained
         * game-event tail begins — the splice point for the map tail staged
         * behind it (game-then-map order). */
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
         * + mapChecksum(2) + returnToLobbyTicks(2) = SNAPSHOT_HEADER_WIRE_SIZE */
        if (len < pos + SNAPSHOT_HEADER_WIRE_SIZE) { c->netErrors++; break; }

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

        /* No snapshot reliable game-tail to unpack — game events arrive via the
         * CHANNEL_GAME drain below and merge through spliceGameEventsBeforeTail. */

        /* Ingest this snapshot's channel trailer up front, then drain reliable
         * control events from channel 2 (CHANNEL_CONTROL) EAGERLY — before the
         * map-install gate and the ch0/ch1 game/map drain below.  Each event is
         * applied via clientSimApplyControlOrdered so a lobby→running flip can
         * install the new map ahead of the game/map tails, preserving the
         * "control before game/map" ordering; the CTRL_CHANNEL_RESET that
         * precedes the flip lifts the game/map receive baselines here so a
         * previous-game straggler is dropped before those drains run.  Per-
         * message wire layout: type(1) + bodyLen(2) + body(N); the channel
         * guarantees in-order exactly-once delivery, so no per-event dedup or
         * ack is applied. */
        if (pos < len &&
            channelRecvFrame(&c->channelMux, buf + pos, len - pos) >= 0) {
            c->channelFramesRx++;
        }
        /* Voice is pulled off the channel here rather than with the game
         * events below, so it keeps flowing while the map-install gate is
         * holding this snapshot back. */
        clientDrainVoice(c);
        if (c->clientSim != NULL) {
            uint8_t ctlBuf[CHANNEL_MAX_SEG];
            uint16_t ctlLen;
            while (channelReceive(&c->channelMux, CHANNEL_CONTROL,
                                  ctlBuf, &ctlLen)) {
                uint8_t type;
                uint16_t bodyLen;
                ControlEvent evt;
                ControlDecodeBodyFn dec;
                /* Raw spectator countdown (un-enveloped) — consume before the
                 * bodyLen parse so its BE u32 isn't mis-read as a length. */
                if (udpClientInterceptSpecCountdown(c, ctlBuf, ctlLen)) continue;
                if (ctlLen < 3) continue;
                type = ctlBuf[0];
                bodyLen = unpackU16(ctlBuf + 1);
                if ((size_t)(3 + bodyLen) > (size_t)ctlLen) continue;
                dec = transportControlCodecBodyDecoder((ControlEventType)type);
                if (dec == NULL || !dec(ctlBuf + 3, bodyLen, &evt)) {
                    mpDiagLog("[cli] ch2 control decode SKIP type=%s reason=%s",
                              mpDiagCtrlName((int)type),
                              dec == NULL ? "no decoder" : "decode failed");
                    continue;
                }
                /* The game-start baseline reset lifts the game/map receive
                 * baselines (dropping previous-game stragglers) and carries no
                 * sim semantics — apply it here and never forward it to the sim
                 * dispatcher. It precedes the running flip on this channel, so
                 * the lift lands before the ch0/ch1 drains below. */
                if (evt.type == CTRL_CHANNEL_RESET) {
                    clientApplyChannelReset(c, &evt);
                    continue;
                }
                /* Live lobby control reaching a spectator that was on the
                 * delayed feed means the server re-subscribed it at
                 * return-to-lobby — re-enter live-lobby mode. Flip only on the
                 * actual live-lobby markers the re-subscribe sync replay carries
                 * (a phase event opens the burst, CTRL_LOBBY_SYNC_COMPLETE closes
                 * it), not on any control frame, so a stray/late frame can't trip
                 * the flip early. */
                if (c->joinState == UDP_CLIENT_SPECTATING && !c->specLiveLobby
                    && (evt.type == CTRL_GAME_PHASE_LOBBY
                        || evt.type == CTRL_GAME_PHASE_COUNTDOWN
                        || evt.type == CTRL_LOBBY_SYNC_COMPLETE)) {
                    udpClientSetSpecLiveLobby(c, true);
                }
                clientSimApplyControlOrdered(c, &evt, 0);
            }
        }

        /* Drain any bulk-channel stream fragments (map preview) regardless of
         * the map-install gate below — a bulk transfer is independent of the
         * game/map tails and must not be stranded by an un-installed map. */
        clientDrainBulk(c);

        /* Map-install gate, moved past the control drain so
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

        /* Drain reliable game events from channel 0 into the game-tail slot —
         * ahead of the map tail staged above, so they apply in the same
         * game-then-map order the snapshot game tail used.  The channel trailer
         * was ingested above (before the control drain), where any game-start
         * baseline lift already ran, so a previous-game straggler is gone and
         * only current-game events drain here.  The channel guarantees in-order
         * exactly-once delivery, so no per-event ack or dedup is applied.
         * Ephemeral events arrive on the best-effort channel and merge into the
         * same game-event set: order between the reliable and best-effort sets
         * does not affect correctness, so they share chanGameEv[] and the
         * splice below. */
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
            /* Drain the best-effort game-effect channel into the same array. */
            while (chanCount < MAX_SNAPSHOT_EVENTS &&
                   channelReceiveBestEffort(&c->channelMux, CHANNEL_GAME_EFFECT,
                                            chanBuf, &chanLen)) {
                if (unpackGameEvent(chanBuf, chanLen, &chanGameEv[chanCount]) > 0) {
                    chanCount++;
                }
            }
            /* Drain channel 1 (map) events: payload [gen u32][GameEvent]. Stage
             * survivors onto the map tail so they sit behind the game events the
             * splice places ahead of them (game-then-map order). Drop any event
             * tagged older than the installed map generation — a stale change a
             * resync already superseded. */
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

        /* No pre-flip game/map tail rollback is needed: any previous-game
         * straggler on the game/map channels has already been dedup-dropped by
         * the CTRL_CHANNEL_RESET baseline lift that precedes the running flip on
         * the control channel, so only current-game events reach the drains
         * above. */

        c->snapshotHdr.reliableEventCount = (uint8_t)newEventCount;

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
            /* This is the carrier voice rides in the lobby, where no
             * snapshot flows. */
            clientDrainVoice(c);
            /* Drain reliable control events from channel 2 first, then game
             * (channel 0) and map (channel 1) events, applying them directly.
             * Control is applied ordered ahead of game/map to match the
             * snapshot path's "control before game/map tails".  A standalone
             * frame is only sent while the game is not running, so it never
             * coincides with an in-frame running-flip and needs no map-install
             * gating.  The channel guarantees in-order exactly-once delivery, so
             * no dedup is added. */
            if (c->clientSim != NULL) {
                uint8_t chanBuf[CHANNEL_MAX_SEG];
                uint16_t chanLen;
                GameEvent gev;
                while (channelReceive(&c->channelMux, CHANNEL_CONTROL,
                                      chanBuf, &chanLen)) {
                    uint8_t type;
                    uint16_t bodyLen;
                    ControlEvent evt;
                    ControlDecodeBodyFn dec;
                    /* Raw spectator countdown (un-enveloped) — consume before the
                     * bodyLen parse so its BE u32 isn't mis-read as a length. */
                    if (udpClientInterceptSpecCountdown(c, chanBuf, chanLen)) continue;
                    if (chanLen < 3) continue;
                    type = chanBuf[0];
                    bodyLen = unpackU16(chanBuf + 1);
                    if ((size_t)(3 + bodyLen) > (size_t)chanLen) continue;
                    dec = transportControlCodecBodyDecoder((ControlEventType)type);
                    if (dec == NULL || !dec(chanBuf + 3, bodyLen, &evt)) {
                        mpDiagLog("[cli] ch2 control decode SKIP type=%s reason=%s",
                                  mpDiagCtrlName((int)type),
                                  dec == NULL ? "no decoder" : "decode failed");
                        continue;
                    }
                    /* Baseline reset: lift game/map receive baselines, never
                     * forward to the sim (see the snapshot path above). */
                    if (evt.type == CTRL_CHANNEL_RESET) {
                        clientApplyChannelReset(c, &evt);
                        continue;
                    }
                    /* Live lobby control reaching a delayed spectator means the
                     * server re-subscribed it at return-to-lobby — re-enter
                     * live-lobby mode. Flip only on the actual live-lobby markers
                     * the re-subscribe sync replay carries (a phase event opens
                     * the burst, CTRL_LOBBY_SYNC_COMPLETE closes it), not on any
                     * control frame, so a stray/late frame can't trip it early. */
                    if (c->joinState == UDP_CLIENT_SPECTATING && !c->specLiveLobby
                        && (evt.type == CTRL_GAME_PHASE_LOBBY
                            || evt.type == CTRL_GAME_PHASE_COUNTDOWN
                            || evt.type == CTRL_LOBBY_SYNC_COMPLETE)) {
                        udpClientSetSpecLiveLobby(c, true);
                    }
                    clientSimApplyControlOrdered(c, &evt, 0);
                }
                while (channelReceive(&c->channelMux, CHANNEL_GAME,
                                      chanBuf, &chanLen)) {
                    if (unpackGameEvent(chanBuf, chanLen, &gev) > 0) {
                        clientSimApplyGameEvents(c->clientSim, &gev, 1,
                                                 c->playerNum);
                    }
                }
                /* Best-effort game-effect channel (ephemeral events). Order
                 * relative to the reliable game/map drains does not matter. */
                while (channelReceiveBestEffort(&c->channelMux,
                                                CHANNEL_GAME_EFFECT,
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
                /* Bulk-channel stream fragments (map preview) ride the same
                 * standalone frame in a quiet lobby — reassemble them too. */
                clientDrainBulk(c);
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
                /* Deliberately NOT written into the players struct: the
                 * snapshot path owns the displayed ping for every slot,
                 * ours included, so all rows read one measurement basis
                 * conditioned one way (ping_display.h). Writing here too
                 * put an unquantised value in front of the menubar and
                 * mobile rows for the tick before the next snapshot
                 * overwrote it. pingDisplayMs stays as the transport's own
                 * RTT estimate; shell projection reads the min-over-window
                 * value below. */
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
                 * not-self so the joiner doesn't announce themselves, and on
                 * the announce policy's quiet byte the same way the in-process
                 * arm in client_sim_control.c is. */
                if (evt.u.playerJoin.playerNum != c->playerNum &&
                    c->clientSim->inLobby && evt.u.playerJoin.quiet == 0) {
                    char joinMsg[PACKET_MAX_PLAYER_NAME + 16];
                    snprintf(joinMsg, sizeof(joinMsg), "%s has joined.",
                             evt.u.playerJoin.name);
                    clientSimAppendLobbyChat(c->clientSim, "***", joinMsg);
                }
            }
        }
        break;
    }

    case PACKET_PLAYER_LEFT: {
        /* Route through the codec so in-process subscribers see the
         * CTRL_PLAYER_LEAVE event, then keep the "X has left" lobby chat
         * rendering at the wire boundary — display is the transport's
         * job, same precedent as the chat-rendering migration. */
        ControlDecodeFn dec = transportControlCodecDecoder(pktType);
        ControlEvent evt;
        bool decoded = false;
        memset(&evt, 0, sizeof(evt));
        if (dec != NULL) {
            if (dec(buf + PACKET_HEADER_SIZE,
                    (size_t)(len - PACKET_HEADER_SIZE), &evt)) {
                decoded = true;
                clientSimApplyControl(c->clientSim, &evt);
            }
        }
        /* The chat line comes off the decoded event so it can read the
         * announce policy's quiet byte; a packet the codec refused draws no
         * line, as it applied no departure either. */
        if (decoded && evt.u.playerLeave.quiet == 0) {
            uint8_t pNum = evt.u.playerLeave.playerNum;
            if (pNum != c->playerNum && c->clientSim->inLobby) {
                char leaveMsg[PACKET_MAX_PLAYER_NAME + 16];
                snprintf(leaveMsg, sizeof(leaveMsg), "%s has left.",
                         evt.u.playerLeave.name);
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
                                         pName, sizeof(pName), FALSE);
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

    case PACKET_LOBBY_SYNC_COMPLETE: {
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
                    /* Human only. Bots never own a UDP transport observer
                     * today, so this is unreachable for them — but guard at
                     * the call site anyway so the bot-credit leak can't
                     * silently return if that ever changes (see #152). */
                    if (elapsed >= 3600000 && !c->clientSim->isBot) {
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

    /* Control events ride CHANNEL_CONTROL: drained in the PACKET_STATE_SNAPSHOT
     * and PACKET_CHANNEL paths above, and acked by the channel-frame trailer. */

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
                /* Do NOT clear lobbySyncSettled here. A map change is a live
                 * re-broadcast, not a subscriber attach: the forced re-JOIN
                 * below hits the server's already-connected branch, which only
                 * re-sends JOIN_ACCEPT — no roster re-announce, hence no fresh
                 * sync replay and no terminating CTRL_LOBBY_SYNC_COMPLETE to
                 * re-arm the guard. Clearing it would strand every lobby event
                 * cue silent for the rest of the lobby (only the ungated
                 * countdown cue would still play). There is no roster burst to
                 * suppress, so the guard correctly stays settled. It is still
                 * cleared on genuine (re)joins where a real replay follows. */
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
    case PACKET_LOBBY_BRAIN_LIST:
    case PACKET_LOBBY_BOT_POOL_CHUNK:
    case PACKET_LOBBY_BRAIN_DOCS_CHUNK: {
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

    case PACKET_LOBBY_SCENARIO_LIST_RSP:
        udpClientHandleLobbyScenarioListRsp(c->clientSim, buf, len);
        break;

    case PACKET_LOBBY_MAP_PREVIEW_ERR:
        udpClientHandleLobbyMapPreviewErr(c->clientSim, buf, len);
        break;

    case PACKET_ROUND_LOG_ERR:
        /* [header 8] [reqSeq 4 BE] [code 1] — the server turned down a round-log
         * request. Every refusal is answered, so this is how a "no" is told
         * apart from a lost request. */
        if (len < PACKET_HEADER_SIZE + 5) break;
        udpClientHandleRoundLogErr(c, unpackU32(buf + PACKET_HEADER_SIZE),
                                   buf[PACKET_HEADER_SIZE + 4]);
        break;

    case PACKET_LOBBY_MAP_UPLOAD_ACK: {
        /* [header 8] [status 1]. 0 = ok, non-zero = reject. */
        if (!c->clientSim || len < PACKET_HEADER_SIZE + 1) break;
        if (c->clientSim->lobbyMapUploadStatus != 1) break;
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
        if (c->clientSim->lobbyMapUploadStatus != 1) break;
        /* nameLen + name bytes are informational here (echoes the
         * client's announce), we just flip the fallback flag. */
        c->clientSim->lobbyMapUseLocalNeedsFallback = true;
        break;
    }

    case PACKET_LOBBY_MAP_UPLOAD_DONE: {
        /* [header 8] [status 1] [pathLen 1] [path N] */
        if (!c->clientSim || len < PACKET_HEADER_SIZE + 2) break;
        if (c->clientSim->lobbyMapUploadStatus != 1 &&
            c->clientSim->lobbyMapUploadStatus != 2) break;
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
            /* The server's map directory just gained a file, so whatever
             * listing the chooser has cached is now short by one. Drop the
             * ready flag so the next enumerate re-asks, and tick the
             * counter the chooser watches so an enumerate happens. */
            c->clientSim->lobbyMapListReady = false;
            c->clientSim->lobbyMapListSeq++;
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
#ifdef __EMSCRIPTEN__
        bool wasEmpty = (c->wbnServerKey[0] == '\0');
#endif
        memcpy(c->wbnServerKey, newKey, sizeof(c->wbnServerKey));
        if (keyChanged) {
#ifdef __EMSCRIPTEN__
            /* Web slot: reauth re-presents the single-use join_code, so only do
             * it for the initial key (the one delivery that verifies the code).
             * On later return-to-lobby rotations the server re-stamps the web
             * identity itself, so we just adopt the new key and hand it to JS to
             * refresh the shareable URL — no reauth, no re-mint. */
            if (wasEmpty) {
                udpClientSendWbnReauth(c);
            }
            wbOnGameKey(c->wbnServerKey);
#else
            udpClientSendWbnReauth(c);
#endif
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
    struct in_addr trackerIp;
    uint8_t buf[PACKET_HEADER_SIZE + 6];

    if (c->sock == INVALID_SOCKET) return;
    if (c->trackerAddr[0] == '\0') return;
    if (bolo_resolve_ipv4(c->trackerAddr, &trackerIp) != 0) return;

    packHeader(buf, PACKET_PUNCH_REQUEST, c->outSequence++);
    memcpy(buf + PACKET_HEADER_SIZE, &c->targetIp.s_addr, 4);
    packU16(buf + PACKET_HEADER_SIZE + 4, c->targetPort);

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr = trackerIp;
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
     * drained onto an input this tick leaves nothing to send here.
     *
     * Also fire during DOWNLOADING_MAP: the join map now streams on CHANNEL_BULK
     * and this standalone frame is the only carrier of the client's CHANNEL_BULK
     * acks during the download. Without it the server's send window stalls one
     * window in and a larger map wedges the join.
     *
     * Likewise during SPECTATING: the spectator seed and forward records stream
     * on CHANNEL_BULK, and this standalone frame carries the spectator's acks.
     * Without it the server's seed transfer never completes. */
    if (c->joinState == UDP_CLIENT_CONNECTED ||
        c->joinState == UDP_CLIENT_DOWNLOADING_MAP ||
        c->joinState == UDP_CLIENT_SPECTATING) {
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

    /* Map-resync stall watchdog.  Once the stream starts the request retransmit
     * above stops, so a transfer that then stalls — the server dropped its
     * resync state on a game restart, or the link went quiet — would leave
     * resyncActive stuck forever: clientSimNetReportMapChecksum early-returns
     * while a resync is "active", so neither a fresh resync nor the disconnect
     * cap ever fires and the client renders wrong terrain indefinitely.  If
     * there's been no forward progress (request sent at start, or the stream
     * header arrived) for a while, abandon the in-flight resync.  Keep
     * resyncAttempts so the next checksum mismatch re-arms and eventually trips
     * MAP_RESYNC_MAX_ATTEMPTS. Re-init the bulk receiver first: it may hold the
     * resync buffer as its dst mid-body, and freeing that buffer without
     * dropping the dst would dangle it. */
    if (c->joinState == UDP_CLIENT_CONNECTED && c->resyncActive &&
        (c->localTick - c->lastResyncProgressTick) >= MAP_RESYNC_STALL_TICKS) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "map resync stalled (no progress for %u ticks) -> abandon (attempt %u)",
            (unsigned)(c->localTick - c->lastResyncProgressTick),
            (unsigned)c->resyncAttempts);
        bulkReceiverInit(&c->bulkRecv);
        udpClientFreeResyncBuf(c);
        c->resyncActive = false;
        c->firstResyncChunkSeen = false;
        c->activeResyncGen = 0;
        /* resyncAttempts intentionally retained — this failed attempt counts
         * toward the disconnect cap. */
    }

    /* Deadlines on an outstanding round-log request: a parked retry after a
     * transient refusal, the first-byte timeout, and the stalled-transfer
     * watchdog. A no-op unless a request is in flight. */
    if (c->joinState == UDP_CLIENT_CONNECTED) {
        udpClientRoundLogTick(c);
    }

    /* Control-event acks now ride the channel-frame trailer (the per-tick
     * standalone PACKET_CHANNEL below, or an input trailer during running),
     * so the dedicated coalesced control-ack emitter is retired. */

    /* Join-download watchdog. Progress is read the same way the percent
     * accessor reads it: the reassembled count, or the bulk receiver's live
     * body counter while its dst is our buffer. While the ask is unanswered
     * (no stream started) the READY is re-sent on the short threshold; once
     * the stream is visibly ours, only a hard stall (the channel's own
     * retransmits exhausted their reach) re-asks, which the server answers
     * with a restart behind a CHANNEL_BULK re-base. Capped so a server that
     * can never complete the transfer surfaces as a connect error instead of
     * an endless silent restart loop. */
    if (c->joinState == UDP_CLIENT_DOWNLOADING_MAP) {
        uint32_t have = c->mapDownloadReceived;
        bool streaming = (c->mapDownloadBuf != NULL &&
                          c->bulkRecv.dst == c->mapDownloadBuf);
        if (streaming && c->bulkRecv.bodyReceived > have) {
            have = c->bulkRecv.bodyReceived;
        }
        if (have != c->dlProgressBytes) {
            c->dlProgressBytes = have;
            c->dlProgressTick  = c->localTick;
        } else if (c->localTick - c->dlProgressTick >=
                   (streaming ? (uint32_t)MAP_DL_STALL_TICKS
                              : (uint32_t)MAP_DL_READY_RESEND_TICKS)) {
            if (c->dlReadyResends >= MAP_DL_MAX_RESTARTS) {
                WB_LOG_WARN(WB_LOG_CAT_NET,
                    "map download unrecoverable: %u restart asks, "
                    "%u/%u bytes -> ERROR",
                    (unsigned)c->dlReadyResends, (unsigned)have,
                    (unsigned)c->mapDownloadTotal);
                c->joinState = UDP_CLIENT_ERROR;
            } else {
                c->dlReadyResends++;
                WB_LOG_INFO(WB_LOG_CAT_NET,
                    "map download quiet (%u/%u bytes, streaming=%d) -> "
                    "re-sending READY (ask %u/%u)",
                    (unsigned)have, (unsigned)c->mapDownloadTotal,
                    (int)streaming, (unsigned)c->dlReadyResends,
                    (unsigned)MAP_DL_MAX_RESTARTS);
                udpClientSendMapDlReady(c);
            }
        }
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
                if (c->spectator) {
                    /* Spectator: mint a spectator_key instead of a player_key
                     * (a player_key won't pass verify_spectator). Logged-in
                     * viewers authenticate with their token; anonymous ones
                     * supply only a name. The server_key comes from the WBN
                     * rekey, so a LAN/offline server never sends one and this
                     * is skipped — the viewer then joins anonymously. */
                    if (c->wbnServerKey[0] != '\0' &&
                        (c->wbnApiToken[0] != '\0' || c->playerName[0] != '\0')) {
                        char errMsg[256];
                        errMsg[0] = '\0';
                        if (!winbolonetClientJoinSpectatorSession(c->wbnApiToken,
                                                                  c->wbnServerKey,
                                                                  c->playerName,
                                                                  playerKey, errMsg)) {
                            WB_LOG_WARN(WB_LOG_CAT_NET,
                                    "[WBN] spectator join exchange failed: %s",
                                    errMsg[0] ? errMsg : "(no detail)");
                            /* Degraded: ship empty key, admitted anonymously. */
                            playerKey[0] = '\0';
                        }
                    }
                } else if (c->wbnApiToken[0] != '\0' && c->wbnServerKey[0] != '\0') {
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
                /* Tankless spectator join: the server branches to the
                 * spectator-accept path (0xFF slot, no map). */
                if (c->spectator)              joinFlags |= JOIN_FLAG_SPECTATOR;
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
        if (!c->suppressPing &&
            c->localTick - c->lastPingSentTick >= PING_INTERVAL_TICKS) {
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
        udpClientUploadPump(c, SDL_GetTicks());

        /* A scenario-list request that was never answered. Nothing else ends
           one — the response has no path to recognise it by and the server
           may send several chunks — so without this a dropped answer leaves
           the request in flight and every later chunk dropped, and the
           chooser has no way to ask again. The list itself is left alone:
           what times out is the asking. */
        if (c->clientSim != NULL && c->clientSim->lobbyScenarioListInFlight) {
            if (c->clientSim->lobbyScenarioListWaited <
                LOBBY_SCENARIO_LIST_TIMEOUT_TICKS) {
                c->clientSim->lobbyScenarioListWaited++;
            } else {
                c->clientSim->lobbyScenarioListInFlight = false;
                c->clientSim->lobbyScenarioListStarted  = false;
            }
        }

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

    /* A spectator runs only the command-queue retransmit (its sole outbound
     * traffic is CMD_CHAT); the ping/timeout/upload pumps above are player-only.
     * Without this a spectator's chat would send once and never retry on loss. */
    if (c->joinState == UDP_CLIENT_SPECTATING &&
        c->outHeadSeq != c->outTailSeq) {
        uint32_t now = (uint32_t)SDL_GetTicks();
        OutCmdEntry *head = &c->outCmdQueue[c->outHeadSeq % OUT_CMD_QUEUE_CAP];
        if (head->lastSentMs != 0 && (now - head->lastSentMs) > 80) {
            udpClientDrainCommandQueue(c);
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
            playersGetPlayerName(&c->clientSim->sim.plyrs, fromPN, pName,
                                 sizeof(pName), FALSE);
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
            /* Do NOT clear lobbySyncSettled here. A map change is a live
             * re-broadcast, not a subscriber attach: the forced re-JOIN hits
             * the server's already-connected branch, which only re-sends
             * JOIN_ACCEPT — no roster re-announce, hence no fresh sync replay
             * and no terminating CTRL_LOBBY_SYNC_COMPLETE to re-arm the guard.
             * Clearing it would strand every lobby event cue silent for the
             * rest of the lobby. There is no roster burst to suppress, so the
             * guard correctly stays settled. (For UDP clients this observer
             * also runs off the PACKET_LOBBY_MAP_CHANGE handler's
             * clientSimApplyControl, so it must not undo that fix either.) */
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
                    /* Human only. Bots never own a UDP transport observer
                     * today, so this is unreachable for them — but guard at
                     * the call site anyway so the bot-credit leak can't
                     * silently return if that ever changes (see #152). */
                    if (elapsed >= 3600000 && !c->clientSim->isBot) {
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
                                   unsigned short trackerPort,
                                   bool spectator) {
    Transport t;
    TransportUdpClientCtx *c;

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
        if (bolo_resolve_ipv4(serverAddr, &c->serverAddr.sin_addr) == 0) {
            WB_LOG_DEBUG(WB_LOG_CAT_NET,
                "client connect: resolved %s -> %s",
                serverAddr,
                inet_ntoa(c->serverAddr.sin_addr));
        } else {
            WB_LOG_ERROR(WB_LOG_CAT_NET,
                "client connect: DNS lookup for '%s' failed",
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
    c->spectator = spectator;

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
    /* Arm the lobby-sound guard for the initial join: the server's sync replay
     * ends with CTRL_LOBBY_SYNC_COMPLETE, which re-sets it true. */
    if (c->clientSim != NULL) {
        c->clientSim->lobbySyncSettled = false;
    }
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
    c->mapEventAck = 1;       /* First valid map event seq is 1 */
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
    bulkReceiverInit(&c->bulkRecv);
    bulkSenderInit(&c->uploadSend);

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
    /* Optional ping suppression from WB_NOPING (any non-empty value). Read
     * once here, beside WB_NETIMPAIR and under the same compile-time
     * condition, so a test can hold a connection whose only outbound traffic
     * is what it sends itself — a ping every PING_INTERVAL_TICKS would
     * otherwise keep the server's liveness clock fresh on its own. */
    {
        const char *noPing = getenv("WB_NOPING");
        if (noPing != NULL && noPing[0] != '\0') {
            c->suppressPing = true;
            mpDiagLog("[cli] periodic ping suppressed (WB_NOPING)");
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
    udpClientFreeResyncBuf(c);
    if (c->uploadBuf != NULL) {
        free(c->uploadBuf);
    }
    if (c->specRecvBuf != NULL) {
        free(c->specRecvBuf);   /* in-flight spectator blob, if teardown mid-transfer */
    }
    if (c->lobbyChatBacklogBuf != NULL) {
        free(c->lobbyChatBacklogBuf);  /* in-flight backlog blob, if teardown mid-transfer */
    }
    if (c->roundLogBuf != NULL) {
        free(c->roundLogBuf);   /* round log nobody took, or a partial one */
    }
    bulkSenderReset(&c->uploadSend);
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

/* Test-only: overwrite the map-event ack stamped into every InputPacket from
 * here on. Nothing in the client advances it past its initial 1, so this is
 * the only way a value the server has never handed to the map channel can
 * reach the wire — which is exactly what the test needs the server to
 * ignore. */
void transportUdpClientTestSetMapEventAck(Transport *t, uint32_t ack) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return;
    c = (TransportUdpClientCtx *)t->ctx;
    c->mapEventAck = ack;
}

/* Test-only: run the map-resync finalize on a caller-supplied blob, as if a
 * resync of a fresh generation had just reassembled `buf`. Arms the resync
 * precondition (resyncActive, a fresh activeResyncGen, buf copied into the
 * owned resync buffer) and drives the real udpClientFinalizeResync, so a test
 * can prove a corrupt blob leaves installedMapGen/mapResyncCount untouched (and
 * re-arms resyncActive) while a valid blob advances them. Returns false if the
 * precondition can't be set (bad args or out of memory). */
bool transportUdpClientTestFinalizeResync(Transport *t, const BYTE *buf, int len) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL || buf == NULL || len <= 0) return false;
    c = (TransportUdpClientCtx *)t->ctx;
    udpClientFreeResyncBuf(c);
    c->mapResyncBuf = (BYTE *)malloc((size_t)len);
    if (c->mapResyncBuf == NULL) return false;
    memcpy(c->mapResyncBuf, buf, (size_t)len);
    c->mapResyncTotal = (uint32_t)len;
    c->resyncActive = true;
    c->firstResyncChunkSeen = true;
    c->activeResyncGen = udpClientNextResyncGen(c);
    udpClientFinalizeResync(c);
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

/* Test-only: read the resync debounce state — whether a resync is currently in
 * flight (resyncActive) and the consecutive-mismatch streak that gates a new
 * request (mapMismatchStreak). */
void transportUdpClientTestResyncState(Transport *t, bool *resyncActive,
                                       uint32_t *mismatchStreak) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return;
    c = (TransportUdpClientCtx *)t->ctx;
    if (resyncActive)   *resyncActive   = c->resyncActive;
    if (mismatchStreak) *mismatchStreak = c->mapMismatchStreak;
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
        /* Converged (or never diverged): the streak is broken, and clear the
         * backoff once no resync is in flight so a later genuine divergence
         * starts fresh. */
        c->mapMismatchStreak = 0;
        if (!c->resyncActive) {
            c->resyncAttempts = 0;
            c->resyncSuppressUntilTick = 0;
        }
        return;
    }

    /* Mismatch. One outstanding request at a time, and stay quiet during the
     * post-install grace window — otherwise every full-sync would re-request.
     * Mismatches in those states don't count toward the streak. */
    if (c->resyncActive) return;
    if (c->localTick < c->resyncSuppressUntilTick) return;

    /* Debounce: a single transient mismatch is usually in-flight reliable map
     * events that haven't arrived yet and will self-deliver before the next
     * full-sync. Only act once the divergence persists for several full-syncs. */
    c->mapMismatchStreak++;
    if (c->mapMismatchStreak < MAP_RESYNC_MISMATCH_DEBOUNCE) return;

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

    /* Start a new resync. Reset the streak so the next divergence re-counts. */
    c->mapMismatchStreak = 0;
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

uint32_t transportUdpClientGetMapInvalidateCount(Transport *t) {
    if (t == NULL || t->ctx == NULL) return 0;
    return ((TransportUdpClientCtx *)t->ctx)->mapInvalidateCount;
}

const BYTE *transportUdpClientGetMapData(Transport *t, int *outLen) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return NULL;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->mapDownloadBuf == NULL) return NULL;
    /* Connected player, or a live-lobby spectator whose lobby map has finished
     * installing. Gated on mapInstalled (not merely SPECTATING) so a mid-download
     * spectator never feeds half a map to the lobby preview. */
    if (c->joinState != UDP_CLIENT_CONNECTED &&
        !(c->joinState == UDP_CLIENT_SPECTATING && c->mapInstalled)) {
        return NULL;
    }
    if (outLen != NULL) {
        *outLen = (int)c->mapDownloadTotal;
    }
    return c->mapDownloadBuf;
}

uint8_t transportUdpClientGetMapDownloadPercent(Transport *t) {
    TransportUdpClientCtx *c;
    uint32_t have;
    if (t == NULL || t->ctx == NULL) return 100;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->mapDownloadTotal == 0) return 100;
    /* mapDownloadReceived only reaches total on full reassembly, so read live
     * progress from the bulk receiver's body counter while the download stream
     * is in flight (the receiver's dst is mapDownloadBuf during a join download). */
    have = c->mapDownloadReceived;
    if (have < c->mapDownloadTotal &&
        c->bulkRecv.dst == c->mapDownloadBuf && c->mapDownloadBuf != NULL) {
        have = c->bulkRecv.bodyReceived;
    }
    uint32_t pct = (have * 100u) / c->mapDownloadTotal;
    return pct > 100 ? 100 : (uint8_t)pct;
}

bool transportUdpClientSendRoundLogRequest(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return false;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->joinState != UDP_CLIENT_CONNECTED) return false;
    /* A fresh ask supersedes whatever the last one left behind — a blob nobody
     * took, or a body still arriving. Freeing it here is one of the three
     * places that keeps it from leaking or dangling (the others are transport
     * teardown and a CHANNEL_BULK re-base). */
    udpClientFreeRoundLogBuf(c);
    c->roundLogState             = CLIENT_ROUND_LOG_WAITING;
    c->roundLogRequestTick       = c->localTick;
    c->roundLogProgressTick      = c->localTick;
    c->roundLogRetries           = 0;
    c->roundLogTransientRetries  = 0;
    c->roundLogRetryAtTick       = 0;
    udpClientSendRoundLogReq(c);
    return true;
}

int transportUdpClientGetRoundLogState(Transport *t) {
    TransportUdpClientCtx *c;
    if (t == NULL || t->ctx == NULL) return CLIENT_ROUND_LOG_IDLE;
    c = (TransportUdpClientCtx *)t->ctx;
    return c->roundLogState;
}

uint8_t transportUdpClientGetRoundLogPercent(Transport *t) {
    TransportUdpClientCtx *c;
    uint32_t total, pct;
    if (t == NULL || t->ctx == NULL) return 0;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->roundLogState != CLIENT_ROUND_LOG_DOWNLOADING) return 0;
    /* Live from the receiver, the way the map-download percent reads it: the
     * body counter against the stream header's announced size, both valid only
     * while this transfer is the one the receiver is filling. */
    if (c->roundLogBuf == NULL || c->bulkRecv.dst != c->roundLogBuf) return 0;
    total = c->bulkRecv.hdr.totalSize;
    if (total == 0) return 0;
    pct = (c->bulkRecv.bodyReceived * 100u) / total;
    return pct > 100 ? 100 : (uint8_t)pct;
}

uint8_t *transportUdpClientTakeRoundLog(Transport *t, size_t *outLen) {
    TransportUdpClientCtx *c;
    uint8_t *blob;
    if (outLen != NULL) *outLen = 0;
    if (t == NULL || t->ctx == NULL) return NULL;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->roundLogState != CLIENT_ROUND_LOG_READY ||
        c->roundLogBuf == NULL) {
        return NULL;
    }
    /* Ownership leaves here. The receiver dropped its dst at completion, so
     * clearing the pointer is enough to put the buffer out of reach of every
     * free site and of the progress read. */
    blob = c->roundLogBuf;
    if (outLen != NULL) *outLen = c->roundLogLen;
    c->roundLogBuf              = NULL;
    c->roundLogLen              = 0;
    c->roundLogState            = CLIENT_ROUND_LOG_IDLE;
    c->roundLogReqSeq           = 0;
    c->roundLogRetries          = 0;
    c->roundLogTransientRetries = 0;
    c->roundLogRetryAtTick      = 0;
    c->roundLogWatchdogBytes    = 0;
    return blob;
}

void transportUdpClientSendWbnReauth(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    if (c->joinState != UDP_CLIENT_CONNECTED) return;
    udpClientSendWbnReauth(c);
}

/* ── Voice (CHANNEL_VOICE) ───────────────────────────────────────── */

/* client_net.h states the largest frame a voice segment can carry as a
 * plain number, because the client frontends cannot see the wire bound it
 * mirrors. This is the one translation unit that sees both, so it is where
 * the two are held together: a change to CHANNEL_VOICE_SEG that leaves
 * CLIENT_VOICE_MAX_FRAME_BYTES behind fails to compile here rather than
 * silently costing frames at run time. */
BOLO_STATIC_ASSERT(CLIENT_VOICE_MAX_FRAME_BYTES == VOICE_SEG_MAX_OPUS,
                   client_voice_max_frame_bytes_drift);

void transportUdpClientSendVoice(Transport *t, const uint8_t *opus,
                                 int opusLen, uint8_t flags) {
    TransportUdpClientCtx *c;
    uint8_t seg[CHANNEL_VOICE_SEG];
    int segLen;

    if (t == NULL || t->ctx == NULL) return;
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    /* The caller's flags byte goes out as it stands.  The only bit defined
     * is VOICE_FLAG_END_OF_UTTERANCE, which the sender sets on the frame
     * that ends a run of speech and the server forwards untouched. */
    segLen = voiceSegmentPackUp(seg, (int)sizeof(seg), c->voiceSeq, flags,
                                opus, opusLen);
    if (segLen <= 0) return;
    c->voiceSeq++;

    /* The queued segment goes out on whichever carrier runs next — the
     * input-packet trailer while playing, a standalone frame otherwise. */
    channelSendBestEffort(&c->channelMux, CHANNEL_VOICE, seg,
                          (uint16_t)segLen);
}

void transportUdpClientGetVoiceChannelStats(Transport *t, uint32_t *outSent,
                                            uint32_t *outRingDropped,
                                            uint32_t *outBudgetSkipped) {
    TransportUdpClientCtx *c;

    if (t == NULL || t->ctx == NULL) {
        channelGetBestEffortStats(NULL, CHANNEL_VOICE, outSent, outRingDropped,
                                  outBudgetSkipped);
        return;
    }
    c = (TransportUdpClientCtx *)t->ctx;
    channelGetBestEffortStats(&c->channelMux, CHANNEL_VOICE, outSent,
                              outRingDropped, outBudgetSkipped);
}

int transportUdpClientReceiveVoice(Transport *t, uint8_t *fromPlayer,
                                   uint8_t *seq, uint8_t *flags,
                                   uint8_t *out, int outCap) {
    TransportUdpClientCtx *c;
    const VoiceRxFrame *frame;

    if (t == NULL || t->ctx == NULL || fromPlayer == NULL || seq == NULL ||
        flags == NULL || out == NULL) {
        return 0;
    }
    c = (TransportUdpClientCtx *)t->ctx;
    if (c->voiceRxCount == 0) return 0;

    frame = &c->voiceRx[c->voiceRxHead];
    if ((int)frame->len > outCap) {
        /* Caller's buffer cannot hold it — discard rather than stall the
         * ring behind a frame that will never fit. */
        c->voiceRxHead = (c->voiceRxHead + 1) % CLIENT_VOICE_RX_RING;
        c->voiceRxCount--;
        return 0;
    }

    *fromPlayer = frame->fromPlayer;
    *seq = frame->seq;
    *flags = frame->flags;
    memcpy(out, frame->data, frame->len);
    c->voiceRxHead = (c->voiceRxHead + 1) % CLIENT_VOICE_RX_RING;
    c->voiceRxCount--;
    return (int)frame->len;
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

/* Ask what scenarios the server offers on their own. The request carries
 * nothing but its header — the directory is flat, so there is no path to ask
 * about — and the accumulator is cleared here so the response appends to an
 * empty list, as the map list's does. */
void transportUdpClientSendLobbyScenarioListRequest(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (c->joinState != UDP_CLIENT_CONNECTED) return;

    packHeader(buf, PACKET_LOBBY_SCENARIO_LIST_REQ, c->outSequence++);
    udpClientSendTo(c, buf, PACKET_HEADER_SIZE);

    if (c->clientSim) {
        /* The rows the last response left are kept until this one's first
           chunk lands, which is where they are cleared. A chooser reading
           while the answer is on its way sees the old list rather than an
           empty one, and a request that times out leaves the last good
           listing in place. */
        c->clientSim->lobbyScenarioListReady    = false;
        c->clientSim->lobbyScenarioListInFlight = true;
        c->clientSim->lobbyScenarioListStarted  = false;
        c->clientSim->lobbyScenarioListWaited   = 0;
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
    uint8_t buf[PACKET_HEADER_SIZE + 4 + 1 + 255 + 4];
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
    /* Drop any abandoned upload tail without reusing its sequence numbers.
     * The receiver adopts this boundary before acknowledging BEGIN. */
    packU32(buf + len, channelResetSend(&c->channelMux, CHANNEL_BULK));
    len += 4;
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
                                         const char md5Hex[32]) {
    /* Wire: [hdr 8][totalLen 4][nameLen 1][name N][relPathLen 1][relPath M][md5 32] */
    uint8_t buf[PACKET_HEADER_SIZE + 4 + 1 + 255 + 1 + 255 + 32];
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
    memcpy(buf + pos, md5Hex, 32);
    pos += 32;
    udpClientSendTo(c, buf, pos);

    if (c->clientSim) {
        /* Mark "USE_LOCAL in flight" — clears to upload-or-done when
         * the server replies. */
        c->clientSim->lobbyMapUploadStatus = 1;
        c->clientSim->lobbyMapUploadRejectCode = 0;
        c->clientSim->lobbyMapUploadFinalPath[0] = '\0';
    }
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
                                             const char md5Hex[32]) {
    udpClientUploadSendUseLocal((TransportUdpClientCtx *)t->ctx, totalLen,
                                 name, relPath, md5Hex);
}

/* === Lobby map upload — state machine =============================
 *
 * Moved from imgui_lobby.cpp (lobbyUploadKick / lobbyUploadPump). The
 * transport owns the bytes, the BEGIN/USE_LOCAL handshake, the bulk-stream
 * send (paced from udpClientTick), and the watchdog. */

static void udpClientUploadCleanup(TransportUdpClientCtx *c) {
    if (c->uploadBuf != NULL) {
        free(c->uploadBuf);
        c->uploadBuf = NULL;
    }
    bulkSenderReset(&c->uploadSend);
    c->uploadActive          = false;
    c->uploadTotal           = 0;
    c->uploadOffset          = 0;
    c->uploadName[0]         = '\0';
    c->uploadBulkStarted     = false;
    c->uploadFedDone         = false;
    c->uploadUseLocalPending = false;
    c->uploadBeginSent       = false;
    c->uploadStartedMs       = 0;
    c->uploadPrevStatus      = 0;
    c->uploadPrevProgressMs  = 0;
    c->uploadPrevAcked       = 0;
}

/* Shared kick: stash the bytes on the transport, optionally try
 * USE_LOCAL first when the caller derived a data/maps-relative path,
 * else announce via BEGIN. `buf` is copied; caller retains ownership. */
/* useLocalLen is what the USE_LOCAL pre-check reports and hashes over, which
 * for a packed map is its map body rather than the whole file: the server
 * answers that check from serverSimReadMapFile, which trims a map at its
 * terminator, so comparing whole files would NACK every packed map both
 * sides already have. len stays the whole file — that is what a fallback
 * upload sends, container and all. */
static bool udpClientUploadStart(TransportUdpClientCtx *c,
                                  const uint8_t *buf, size_t len,
                                  const char *name,
                                  const char *relPath, /* nullable */
                                  const char *md5Hex,  /* 32 hex chars + NUL, required iff relPath */
                                  size_t useLocalLen   /* bytes the pre-check names; 0 for len */) {
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

    if (relPath != NULL && relPath[0] != '\0' && md5Hex != NULL) {
        if (useLocalLen == 0 || useLocalLen > len) {
            useLocalLen = len;
        }
        udpClientUploadSendUseLocal(c, (uint32_t)useLocalLen, c->uploadName,
                                     relPath, md5Hex);
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
 * bulk-stream → DONE/REJECT lifecycle. */
static void udpClientUploadPump(TransportUdpClientCtx *c, uint64_t now) {
    uint8_t st;
    uint64_t sinceProgress, sinceStart;
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

    /* Watchdog. Progress is measured by the bulk channel's ack advancing
     * (the server confirming delivered upload segments) rather than a local
     * offset walk. Treat the moment the whole blob has been handed to the
     * channel as one final forward-progress event: thereafter we only wait
     * for MAP_UPLOAD_DONE, and the stall timer must not count against a
     * server merely slow to load + reply. */
    {
        uint32_t acked = c->channelMux.ch[CHANNEL_BULK].ackedSeq;
        advanced = (c->uploadPrevStatus != st) ||
                   (c->uploadPrevAcked != acked);
        if (c->uploadBulkStarted && !c->uploadFedDone &&
            !bulkSenderBusy(&c->uploadSend)) {
            c->uploadFedDone = true;
            advanced = true;
        }
        if (advanced) {
            c->uploadPrevStatus     = st;
            c->uploadPrevAcked      = acked;
            c->uploadPrevProgressMs = now;
        }
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
            "upload watchdog: no bulk-ack progress in %llums — freeing",
            (unsigned long long)sinceProgress);
        timedOut = true;
    }
    if (timedOut) {
        c->clientSim->lobbyMapUploadStatus = 4;
        c->clientSim->lobbyMapUploadRejectCode = LOBBY_REJECT_INVALID;
        c->clientSim->lobbyMapUploadFinalPath[0] = '\0';
        c->clientSim->lobbyMapUseLocalNeedsFallback = false;
        channelResetSend(&c->channelMux, CHANNEL_BULK);
        udpClientUploadCleanup(c);
        return;
    }

    /* USE_LOCAL was NACKed: the server doesn't have a matching file at
     * the relative path / MD5. Fall back to BEGIN + the bulk-stream flow
     * against the bytes already buffered. */
    if (c->uploadUseLocalPending &&
        clientSimConsumeUseLocalFallback(c->clientSim)) {
        udpClientUploadSendBegin(c, c->uploadTotal, c->uploadName);
        c->uploadUseLocalPending = false;
        c->uploadBeginSent       = true;
        return; /* wait one more tick for ACK */
    }

    /* Still waiting on ACK to BEGIN (status flips to 2 on ACK). */
    if (st != 2) return;

    /* Approved: stage the map bytes onto CHANNEL_BULK once, then feed the
     * stream as the window drains. The channel handles fragmentation and
     * retransmit, so a lost middle segment recovers instead of corrupting. */
    if (!c->uploadBulkStarted) {
        BulkStreamHeader h;
        size_t nameLen = strlen(c->uploadName);
        if (nameLen > BULK_PATH_MAX) nameLen = BULK_PATH_MAX;
        memset(&h, 0, sizeof(h));
        h.kind = BULK_KIND_UPLOAD;
        h.gen = 0;
        h.totalSize = c->uploadTotal;
        h.pathLen = (uint8_t)nameLen;
        if (nameLen > 0) memcpy(h.path, c->uploadName, nameLen);
        h.path[nameLen] = '\0';
        if (bulkSenderBegin(&c->uploadSend, &h, c->uploadBuf, c->uploadTotal)) {
            c->uploadBulkStarted = true;
        }
    }
    if (c->uploadBulkStarted) {
        bulkSenderPump(&c->uploadSend, &c->channelMux);
        /* Mirror fed-bytes into uploadOffset for the progress getter. */
        if (bulkSenderBusy(&c->uploadSend)) {
            uint32_t headerLen = c->uploadSend.total - c->uploadTotal;
            c->uploadOffset = (c->uploadSend.offset > headerLen)
                              ? (c->uploadSend.offset - headerLen) : 0;
        } else {
            c->uploadOffset = c->uploadTotal;
        }
    }
}

#if WB_ENABLE_NETIMPAIR
/* Drive the production watchdog without sleeping, and drop only the chosen
 * upload reply so tests can keep connection liveness traffic flowing. */
void transportUdpClientTestUploadTimeout(Transport *t) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    uint64_t now_ms = SDL_GetTicks();
    udpClientUploadPump(c, now_ms);
    udpClientUploadPump(c, now_ms);
    udpClientUploadPump(c, now_ms + UPLOAD_STALL_TIMEOUT_MS + 1);
}

void transportUdpClientTestDropUploadReply(Transport *t, uint8_t packet_type) {
    TransportUdpClientCtx *c = (TransportUdpClientCtx *)t->ctx;
    c->test_drop_upload_packet = packet_type;
}
#endif

bool transportUdpClientStartLobbyMapUploadFromBytes(Transport *t,
                                                     const uint8_t *buf,
                                                     size_t len,
                                                     const char *mapName) {
    return udpClientUploadStart((TransportUdpClientCtx *)t->ctx,
                                 buf, len, mapName,
                                 /*relPath=*/NULL, /*md5=*/NULL,
                                 /*useLocalLen=*/0);
}

bool transportUdpClientStartLobbyMapUploadFromPath(Transport *t,
                                                    const char *localFilePath) {
    TransportUdpClientCtx *c;
    size_t fileLen = 0;
    size_t bodyLen = 0;
    void *fileData = NULL;
    char nameBuf[128];
    char relPath[256];
    bool haveRelPath;
    uint8_t md5[16];
    char md5Hex[33];
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
    /* What the pre-check asks about is the map, not the file. A packed map
       carries a scenario container after its terminator, and the server
       answers from serverSimReadMapFile, which trims there — so a whole-file
       hash and length would miss on every packed map both sides already have
       and the client would upload one it did not need to. A file with no map
       in it keeps its whole length, which is what the check has always
       compared. */
    bodyLen = fileLen;
    if (haveRelPath) {
        size_t trimmed = 0;
        if (boloMapBodyLength((const unsigned char *)fileData, fileLen,
                              &trimmed) &&
            trimmed > 0 && trimmed <= fileLen) {
            bodyLen = trimmed;
        }
        md5Compute(fileData, bodyLen, md5);
        md5ToHex(md5, md5Hex);
    }

    ok = udpClientUploadStart(c, (const uint8_t *)fileData, fileLen, nameBuf,
                               haveRelPath ? relPath : NULL,
                               haveRelPath ? md5Hex  : NULL,
                               haveRelPath ? bodyLen : 0);
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
    /* The seam starts already CONNECTED, so it never runs
     * transportUdpClientCreate and the mux would stay zeroed. Every channel's
     * window is 0 then, and the first channelReceive on a snapshot's trailer
     * divides by it. */
    channelMuxInit(&g_fuzzClientCtx.channelMux);
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
