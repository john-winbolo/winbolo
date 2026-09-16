/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * transport_udp_server_internal.h
 *
 * Cross-translation-unit contract for
 * src/server/transport_udp_server.c and its siblings.
 * Files that include this header see the whole of the
 * server transport's file-scope state — the UdpServerState
 * struct and the per-slot types stored in it — and may
 * read and write its fields directly.
 *
 * No file outside src/server/ may include it. Callers
 * elsewhere use the public transport API in
 * transport_udp.h.
 *********************************************************/
#ifndef TRANSPORT_UDP_SERVER_INTERNAL_H
#define TRANSPORT_UDP_SERVER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>   /* FILENAME_MAX — uploadPersistDir */

#include "global.h"         /* BYTE, MAX_TANKS, PlayerBitMap */
#include "lang_message.h"   /* langid — the localized server->client sends */
#include "platform_net.h"   /* SOCKET, struct sockaddr_in */
#include "netpacks.h"       /* MAP_DOWNLOAD_MAX_SIZE, PACKET_MAX_PLAYER_NAME */
#include "scenario_defs.h"  /* ScnDirEntry — the scenario list chunk's input */
#include "transport_udp.h"  /* UdpServerClient, MAX_SPECTATORS, SubscriberHandle */
#include "transport_udp_internal.h" /* ClientEventQueue, UDP_MAX_PAYLOAD */
#include "channel_mux.h"    /* ChannelMux */
#include "bulk_transfer.h"  /* BulkSender, BulkReceiver */
#include "upload_policy.h"  /* UploadPolicy */
#include "net_impair.h"     /* NetImpair */
#include "../../winbolonet/winbolonet_core.h" /* WINBOLONET_KEY_LEN */

#define RECV_QUEUE_SIZE 1024

#define LOBBY_REQ_COOLDOWN_TICKS 25  /* ~0.5s at 50 Hz */
#define SERVER_UPLOAD_IDLE_TIMEOUT_MS 15000 /* outlasts both client watchdogs (5s BEGIN-ACK, 10s bulk stall) */

/* Anonymous-fallback ceiling for a deferred WBN PLAYER_JOIN: how long
 * we wait for the joiner's rekey->reauth round-trip (two network hops
 * plus two blocking winbolo.net HTTP calls) to fill the slot's key
 * before announcing the join un-keyed.  The keyed path fires the moment
 * the reauth verifies, so this only bounds the never-reauth case
 * (direct-IP / not signed in / WBN unreachable).  ~5 s @ 50 Hz. */
#define WBN_JOIN_REGISTER_GRACE_TICKS 250

/* Standalone PACKET_CHANNEL frames a downloading client gets per tick while
 * snapshots are gated (no snapshot trailer to carry the bulk stream). One
 * frame carries ~5 segments under the datagram budget, so this clears a full
 * CHANNEL_BULK window (96 segments) in a tick rather than throttling the map to
 * ~one frame/tick; the unacked window then bounds bytes in flight.
 *
 * Read by the per-client carrier in the send path in
 * src/server/udp/udp_server_send.c, by the one in the timeout sweep in
 * src/server/transport_udp_server.c, and by the spectator carrier in
 * src/server/udp/udp_server_spectator.c. */
#define MAP_DOWNLOAD_FRAMES_PER_TICK 24

typedef struct {
    uint8_t data[UDP_MAX_PAYLOAD];
    int     len;
    struct sockaddr_in fromAddr;
} RecvQueueEntry;

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
    bool     readySeen;        /* client's PACKET_MAP_DL_READY arrived — a join
                                * download begins only after it, so the stream
                                * can never race the JOIN_ACCEPT that sizes the
                                * client's buffers. Resync transfers ignore it
                                * (their request is the readiness signal). */
} ClientMapDownload;

#define UPLOAD_MAX_BYTES (64u * 1024u)

#define JOIN_RL_MAX_SOURCES 64    /* LRU of recent source IPs */

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
    /* ISO 3166-1 alpha-2 + NUL; GeoIP-or-wire-fallback, "" if unknown. */
    char     countryCode[3];
    /* WBN spectator session. spectatorKey is the verify_spectator key kept for
     * the leave teardown ('' when anonymous / unverified, so no leave is sent).
     * wbnFlags carries PLAYER_FLAG_WBN_VERIFIED for a logged-in verified viewer;
     * Steam-linked / Supporter are not surfaced for spectators. */
    char     spectatorKey[WINBOLONET_KEY_LEN];
    uint8_t  wbnFlags;
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
    bool             live;       /* live lobby control-bus subscriber */
    SubscriberHandle controlSub; /* bus handle while live; INVALID otherwise */
    /* Live-lobby map download. While the viewer watches the live lobby it needs
     * the current lobby map (for the preview + start positions), delivered over
     * its own CHANNEL_BULK exactly like a player's join download — but installed
     * client-side without leaving spectator mode. lobbyMap is a spectator-owned
     * copy of the compressed map, armed at connect / re-join / map change and
     * freed once bulkSenderBegin copies it into the sender (or on disconnect).
     * Distinct from the delayed seed (game-time map): the two never overlap on
     * the channel because the seed arms only when !live and this only when live. */
    BYTE    *lobbyMap;
    uint32_t lobbyMapSize;
} SpectatorConn;

typedef struct {
    struct sockaddr_in addr;
    int packetsRemaining;   /* 0 = slot empty */
    int ticksUntilNext;
} PunchQueueEntry;

#define PUNCH_QUEUE_SIZE        8
#define PUNCH_BURST_PACKETS     5
#define PUNCH_BURST_INTERVAL    3   /* 3 ticks @ 50 Hz ≈ 60 ms */

/* Server-side global state */
typedef struct UdpServerState {
    SOCKET sock;
    /* The port bind() actually gave us, read back with getsockname(). Equal
     * to the requested port in the normal case; with a requested port of 0
     * it is the one the OS picked, which is the only place the real port
     * exists. Anything that advertises where the server can be reached
     * (mDNS, the tracker, WBN) must use this and not the request. */
    unsigned short boundPort;
    bool running;
    UdpServerClient clients[MAX_TANKS];
    SpectatorConn   spectators[MAX_SPECTATORS];
    uint32_t tickCount;
    /* recordSeq of the most recent running-game ring record. Captured every
     * running tick (below), it freezes at the game's last record X when the
     * game ends, since game-over ticks are not recorded — so it is the bar a
     * delayed spectator must reach before it has watched the whole game. */
    uint32_t lastRunningSeq;
    /* recordSeq of the current game's FIRST ring record, captured on the
     * non-running->running transition (serverSimTick records that record before
     * this tick's spectator service runs, so the head is it). The ring records
     * the pre-game lobby continuously, so a freshly cut-over delayed view sits at
     * head - delay inside that lobby; it must reach gameStartSeq before it is
     * showing game content rather than replaying the recorded lobby. Refreshed
     * each new game so a back-to-back round counts down to the latest start. */
    uint32_t gameStartSeq;
    /* Tracks whether the previous spectator-service tick saw the game running, so
     * the transition above is detected once per game (mirrors lastRunningSeq's
     * every-running-tick capture). */
    bool     specWasRunning;

    /* Compressed map buffer for sending to joining clients */
    BYTE     compressedMap[MAP_DOWNLOAD_MAX_SIZE];
    uint32_t compressedMapSize;

    /* Per-client map download state */
    ClientMapDownload mapDownload[MAX_TANKS];

    /* Per-client reliable map event queues (EVENT_MAP_CHANGE only).
     * Game events ride CHANNEL_GAME directly; there is no game-event queue. */
    ClientEventQueue mapEventQueues[MAX_TANKS];

    /* Cumulative count of EVENT_MAP_CHANGE events dropped per slot because the
     * map-event queue was full (client too far behind on its acks). A drop
     * leaves the slot's copy of the terrain unwritten, so the catch-up sweep
     * sees the square as still owed and re-sends it once the client can view
     * it — this counter says how often that recovery is being leaned on. */
    uint32_t mapEventQueueDrops[MAX_TANKS];

    /* Set while a slot's map-channel send window is full and the snapshot
     * drain is holding the remainder of its queue. Keeps that condition to one
     * log line per stall: it is cleared only once the queue has drained
     * completely, so a window that frees a slot at a time does not log on
     * every snapshot it spends catching up. */
    bool mapChannelStalled[MAX_TANKS];

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
    /* Per-client PACKET_ROUND_LOG_REQ bookkeeping. roundLogReqSeen +
     * roundLogLastReqTick hold the minimum interval between requests whatever
     * the answer was; roundLogServed counts transfers actually started and is
     * what the attempt ceiling bounds, so a client refused with BUSY can come
     * back without having spent one. All three clear at join, at disconnect,
     * and at game start — the game-start clear is what makes the ceiling
     * per-round. */
    bool                    roundLogReqSeen[MAX_TANKS];
    uint32_t                roundLogLastReqTick[MAX_TANKS];
    uint8_t                 roundLogServed[MAX_TANKS];
    /* Per-client map-download re-ask bookkeeping, the same shape as the
     * round-log limits above. mapReaskSeen + mapReaskLastTick hold the minimum
     * interval whatever the answer was; mapReaskThrottled counts the re-asks
     * refused for arriving inside it and is what the tests read. Cleared at
     * join and at disconnect, so a slot never inherits the last occupant's. */
    bool                    mapReaskSeen[MAX_TANKS];
    uint32_t                mapReaskLastTick[MAX_TANKS];
    uint32_t                mapReaskThrottled[MAX_TANKS];
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
    uint64_t upload_last_progress_ms[MAX_TANKS];
    uint8_t  clientUploadBuf[MAX_TANKS][UPLOAD_MAX_BYTES];
    char     clientUploadName[MAX_TANKS][128];
    uint8_t  clientReqCooldownTicks[MAX_TANKS];

    /* Operator-controlled upload handling — zero-init = ALLOW + defaults below. */
    UploadPolicy uploadPolicy;
    uint8_t      uploadMaxFiles;
    uint32_t     uploadMaxStorageBytes;
    /* Absolute directory PERSIST uploads are written to. Empty = unset →
     * writes fall back to "<mapDirRoot>/Uploads". Kept in lock-step with the
     * sim's copy (both set from cfg->uploadPersistDir in serverInstanceStartup). */
    char         uploadPersistDir[FILENAME_MAX];

    /* LRU token buckets for the per-source-IP JOIN rate limit. A zeroed
     * table reads as all-empty (srcAddr 0), so the existing
     * memset(&udpServer, 0, …) is the only reset needed. */
    JoinRateEntry joinRate[JOIN_RL_MAX_SOURCES];

    /* Voice forwarding switched off for this server (the dedicated server's
     * -no-voice).  Stored negated so a zeroed server forwards voice — every
     * host that never touches the setter keeps the default. */
    bool     voiceDisabled;

    /* Cumulative voice segments this server forwarded, meaning unpacked and
     * re-packed downstream, and segments pass 1 of serverPumpVoice drained
     * and did not forward.  A segment is dropped when voice is switched off
     * for the server, when the sender has no flood-control credit left, when the sender's map download is not yet complete, when
     * voiceSegmentUnpackUp rejects it, or when voiceSegmentPackDown fails.
     * The pre-download drop is expected: a client with voice on while still
     * taking the map produces a steady drop rate that indicates nothing
     * wrong.  Both count every slot together: they exist to give an operator
     * diagnosing a flooding or misbehaving client a number to look at, not
     * to carry per-slot state. */
    uint32_t voiceSegsAccepted;
    uint32_t voiceSegsDropped;
    /* Forwards withheld from a recipient by the concurrent-talker cap,
     * counted once per (recipient, frame) pair.  Deliberately not folded into
     * voiceSegsDropped: those segments were refused on arrival and went
     * nowhere, these were accepted and forwarded to everyone else. */
    uint32_t voiceSegsTalkerCapped;

    /* The same traffic again, per slot and with the drop reasons apart. The
     * totals above answer "is this server dropping voice"; these answer
     * "whose, and why", which is the question a player reporting that nobody
     * could hear them actually asks. The five reasons share one counter above
     * and mean five different things: a server with voice off, a sender past
     * the per-tick cap, a sender still taking the map, a malformed segment,
     * and a repack that failed — the last being the only one that means the
     * server itself is at fault. forwarded and capped count this slot as a
     * recipient rather than a sender. Cleared when the slot disconnects. */
    struct {
        uint32_t accepted;
        uint32_t dropVoiceOff;
        uint32_t dropPerTickCap;
        uint32_t dropNotInGame;
        uint32_t dropUnpack;
        uint32_t dropRepack;
        uint32_t forwarded;
        uint32_t capped;
    } voiceSlot[MAX_TANKS];

    /* Voice flood-control credit per sender, in segments. Refilled a tick's
     * worth per pump and spent one per segment accepted, so it bounds the
     * sustained rate while letting a sender's bunched frames through — see
     * VOICE_CREDIT_BURST. Cleared with the slot on disconnect. */
    uint8_t  voiceCredits[MAX_TANKS];

    /* Per-slot voice arrival bookkeeping, in tickCount ticks, feeding the
     * concurrent-talker cap: the tick this slot's last voice frame landed on,
     * and the tick its current utterance began.  A gap longer than
     * VOICE_ONSET_GAP_TICKS makes the next frame a new onset.  Both are
     * cleared on disconnect so a recycled slot starts silent.
     *
     * tickCount rather than the sim's tick because this is a wall-clock
     * measure of silence and has to keep running in every server state: the
     * sim's tick stands still for the whole countdown and the whole game-over
     * hold, and runs at twice the rate inside a game. tickCount advances once
     * per server frame regardless. Zero means the slot has never spoken, which
     * tickCount cannot collide with: it is incremented at the top of the frame
     * and so is at least 1 by the time voice is pumped. */
    uint32_t voiceLastFrameTick[MAX_TANKS];
    uint32_t voiceOnsetTick[MAX_TANKS];
    /* The talking set last sent as CTRL_VOICE_TALKING. Held so the event
     * goes out only when the set changes: a quiet lobby then costs nothing,
     * rather than one event per tick per client. */
    PlayerBitMap voiceTalkingPublished;
    /* Send the set again even though it has not changed. Someone has left and
     * the copy every other client holds still names them, so "the value is the
     * same as last time" and "there is nothing to say" have come apart —
     * without this the only case that matters, the leaver being the one talker,
     * computes back to the same empty set and is never sent. */
    bool voiceTalkingResend;
} UdpServerState;

extern UdpServerState udpServer;

/* Handle one received datagram. Owned by
 * src/server/udp/udp_server_dispatch.c, which holds the type switch and every
 * per-packet handler; src/server/transport_udp_server.c calls it from both the
 * polled fallback and the recv-thread drain path. */
void serverProcessPacket(struct ServerSim *sim, uint8_t *buf, int len,
                         struct sockaddr_in *fromAddr);

/* Outbound datagram wrapper every server->peer send routes through, so the
 * outbound impairment layer can delay/drop/reorder it. Owned by
 * src/server/transport_udp_server.c, which holds the impairment state it
 * reads. */
void srvSendTo(const uint8_t *buf, int len, const struct sockaddr_in *addr);

/* The outbound per-tick path to a connected player: an inbound input packet
 * applied to the sim, a ping answered with a pong, one client's filtered
 * snapshot built and sent, and the send loop over all clients. Owned by
 * src/server/udp/udp_server_send.c.
 * Only these two are reached from elsewhere in src/server/:
 * src/server/udp/udp_server_dispatch.c calls both from the packet-type switch.
 * The rest of the file is public API and is declared in transport_udp.h. */
void serverHandleInput(const uint8_t *buf, int len,
                       const struct sockaddr_in *fromAddr,
                       struct ServerSim *sim);
void serverHandlePing(const uint8_t *buf, int len,
                      const struct sockaddr_in *fromAddr);

/* The server-info query protocol: the INFO_PACKET filled from the current sim
 * state, the reply built from it, the old-protocol form of the request, and
 * the terrain-name helper the map-resync self-check prints. Owned by
 * src/server/udp/udp_server_query.c.
 * src/server/udp/udp_server_dispatch.c calls the terrain name from the resync
 * diagnostics and the request pair from the packet-type switch;
 * src/server/udp/udp_server_tracker.c builds its tracker update from
 * buildInfoPacket, which the reply above also shares. */
const char *resyncTerrainName(BYTE t);
void buildInfoPacket(struct ServerSim *sim, INFO_PACKET *pkt);
void serverHandleInfoRequest(const struct sockaddr_in *fromAddr,
                             struct ServerSim *sim);
bool isOldProtocolInfoRequest(const uint8_t *buf, int len);

/* Public-address override advertised in place of the internal port and a zero
 * address once a UPnP/NAT-PMP/PCP mapping is negotiated. Owned by
 * src/server/udp/udp_server_tracker.c. */
extern char           udpServerPublicIp[64];
extern unsigned short udpServerPublicPort;

/* Pending hole-punch bursts, one slot per joiner. Owned by
 * src/server/udp/udp_server_tracker.c. */
extern PunchQueueEntry punchQueue[PUNCH_QUEUE_SIZE];

/* Network impairment state for the server's inbound (client->server) and
 * outbound (server->client) datagram paths. Owned by
 * src/server/udp/udp_server_recv.c; srvSendTo and the transport's create /
 * fuzz-init entry points read and reset them from
 * src/server/transport_udp_server.c. */
extern NetImpair srvImpairIn;
extern NetImpair srvImpairOut;

/* Recv-thread lifecycle. The thread, its SPSC ring and the drop counter are
 * private to src/server/udp/udp_server_recv.c, which owns them; the transport's
 * create and destroy entry points start and stop the thread through these
 * three calls.
 * udpServerRecvThreadStart takes its own copy of the socket, so the socket can
 * be closed after udpServerRecvThreadStop has joined the thread. */
void     udpServerRecvThreadStart(SOCKET sock);
void     udpServerRecvThreadStop(void);
uint32_t udpServerRecvDropCount(void);

/* Move one tick's voice from each sender's channel to the recipients allowed
 * to hear it, and publish the talking set. Owned by
 * src/server/udp/udp_server_voice.c; src/server/udp/udp_server_send.c calls it
 * from the send path and src/server/transport_udp_server.c from the timeout
 * sweep, ahead of the carriers that put channel data on the wire. */
void serverPumpVoice(struct ServerSim *sim);

/* Join handshake and admission control. Owned by
 * src/server/udp/udp_server_join.c. src/server/transport_udp_server.c calls
 * these from serverProcessPacket, the spectator accept, the WBN reauth path
 * and the timeout sweep; the cookie pair and serverFindClient are also what
 * the WB_FUZZ harness at the end of that file reaches for.
 * The seven value-only decision helpers this file's join path also uses
 * (wbnJoinArm and friends) are declared in transport_udp.h, as is
 * transportUdpServerFindByConnId. */
int      serverFindClient(const struct sockaddr_in *addr);
void     serverHandleJoinRequest(const uint8_t *buf, int len,
                                 const struct sockaddr_in *fromAddr,
                                 struct ServerSim *sim);
void     serverSendJoinAccept(int slot, struct ServerSim *sim,
                              const struct sockaddr_in *addr);
void     serverSendJoinReject(const struct sockaddr_in *addr, langid id,
                              int argCount, const char *const args[]);
bool     packLocalizedPayload(uint8_t *buf, int *pos, int bufSize,
                              langid id, int argCount,
                              const char *const args[]);
void     packConnId(uint8_t *buf, uint64_t connId);
uint64_t unpackConnId(const uint8_t *buf);
uint64_t serverNextConnId(void);
uint64_t serverCookieCurrentWindow(void);
bool     serverCookieCompute(const struct sockaddr_in *addr, uint64_t window,
                             uint8_t out[JOIN_COOKIE_LEN]);
void     serverPreemptRename(struct ServerSim *sim, int victimSlot,
                             const char *chosenName,
                             const char *incomingName,
                             const char *incomingCountry);
bool     serverChooseUnverifiedSuffix(const char *baseName, int excludeSlot,
                                      char *out, size_t outLen);

/* What a host or a console does to a connected player, as opposed to the
 * per-tick flow: tearing one client down and draining the removals other paths
 * defer, kicking, disconnecting everyone, the server lock, ping enforcement,
 * server-originated messages, status reporting, bot naming and the background
 * team-balance request. Owned by src/server/udp/udp_server_admin.c.
 * Only these three are reached from elsewhere in src/server/:
 * src/server/transport_udp_server.c calls serverDisconnectClient from the QUIT
 * handler and the timeout sweep, and src/server/udp/udp_server_join.c sends the
 * rename notice and the join chat line through the two broadcast helpers. The
 * rest of the file is public API and is declared in transport_udp.h. */
void serverDisconnectClient(struct ServerSim *sim, int idx, bool graceful);
void serverSendServerEnglishBroadcast(struct ServerSim *sim,
                                      const char *message);
void serverSendServerMessage(struct ServerSim *sim, langid id, int argCount,
                             const char *const args[]);

/* Control-event delivery to one connected player: the per-recipient filter
 * that decides what a client is allowed to see, the queueing of an accepted
 * event onto that client's control channel, and the immediate flush of that
 * channel. Owned by src/server/udp/udp_server_control.c.
 * src/server/udp/udp_server_join.c hands udpClientDeliverControl to
 * serverSimRegisterSubscriber and flushes the replay burst that follows;
 * src/server/udp/udp_server_spectator.c calls udpClientDeliverControl to
 * deliver a player-addressed event, and its own deliver callback logs through
 * mpDiagCtrlName. */
const char *mpDiagCtrlName(int type);
void transportUdpServerFlushChannel(int clientIdx);
void udpClientDeliverControl(void *ctx, const ControlEvent *evt);

/* Map movement in both directions between the transport and one client: the
 * compressed map streamed down to a joining or resyncing client on
 * CHANNEL_BULK, and the lobby map upload reassembled back off it. Owned by
 * src/server/udp/udp_server_maptransfer.c.
 * src/server/transport_udp_server.c calls these from the packet handler and
 * the disconnect and map-change paths, and src/server/udp/udp_server_send.c
 * from the per-tick send path;
 * src/server/udp/udp_server_join.c arms a joiner's download through
 * serverInitMapDownload and clears its re-ask limit at join.
 * lobbyClientMayEdit is the lobby authority check the upload handlers share.
 * It has no other header declaration: server_command_dispatch.c, over in
 * server_sim_static, carries its own extern, and server_stubs.c stubs it for
 * the targets that link neither file. The two remaining helpers in the same
 * translation unit, lobbyAnyOtherUploadActive and uploadFilenameIsSafe, are
 * declared in transport_udp.h so the unit tests can reach them. */
bool lobbyClientMayEdit(struct ServerSim *sim, int clientIdx);
void serverCleanupMapDownload(int slot);
void serverDrainBulk(struct ServerSim *sim, int clientIdx);
void serverInitMapDownload(int slot);
void serverRebaseBulkAndRearmDownload(int i);
void serverServiceMapTransfer(struct ServerSim *sim, int slot);
void udpServerClearClientUploadState(int idx);
void udpServerExpireUploads(uint64_t now_ms);
void udpServerResetMapReaskLimit(int idx);

/* One PACKET_LOBBY_SCENARIO_LIST_RSP chunk, written into the caller's buffer.
 * Owned by src/server/udp/udp_server_dispatch.c, where the request handler
 * calls it in a loop and sends what it returns.
 *
 * Packs entries from `first` until the next will not fit in bufLen, stamps the
 * count and the final flag, and leaves *next at the first entry it did not
 * write — equal to count when this was the last chunk. Returns the chunk's
 * length in bytes, or 0 for a buffer too small to hold even an empty chunk.
 * A count of 0 is a whole answer: one chunk, final set, no entries.
 *
 * Non-static, and takes a buffer rather than a socket, so the unit tests can
 * hold what the server would send against committed golden bytes and feed the
 * same bytes back through the client's accumulator. The map list's encoder is
 * inline in its handler and needs a socket, which is why
 * test_lobby_map_list_chunked.c has to hand-roll the bytes it checks. */
int udpServerPackScenarioListChunk(uint8_t *buf, int bufLen,
                                   const ScnDirEntry *entries, int count,
                                   int first, int *next);

/* Tankless spectator support. Owned by
 * src/server/udp/udp_server_spectator.c.
 * src/server/transport_udp_server.c calls these from the packet handler, the
 * map-change and return-to-lobby paths and the timeout sweep, and
 * src/server/udp/udp_server_send.c from the per-tick send path;
 * serverEnumSpectatorRoster is not called directly, it is handed to
 * serverSimSetSpectatorRosterEnumerator in transportUdpServerCreate. */
void serverAcceptSpectator(struct ServerSim *sim,
                           const struct sockaddr_in *fromAddr,
                           const char *name,
                           uint8_t clientType, uint8_t clientHints,
                           const char *spectatorKey, uint8_t wbnFlags,
                           const char *country);
void serverArmSpectatorLobbyMap(int s, bool resetChannel);
void serverDisconnectSpectator(struct ServerSim *sim, int s, bool graceful);
void serverEnumSpectatorRoster(void *enumCtx,
                               void (*deliver)(void *, const ControlEvent *),
                               void *deliverCtx);
int  serverFindSpectator(const struct sockaddr_in *addr);
void serverSendSpectatorAccept(int s, struct ServerSim *sim,
                               const struct sockaddr_in *addr);
void serverServiceSpectators(struct ServerSim *sim);

/* The server's WinBolo.net-facing work: the session rekey sent to one client
 * and to every slot verified last round, the web join-code identity stamped
 * onto a slot, the reauth that follows a rekey, and the registered source a
 * completed round's log is served from. Owned by
 * src/server/udp/udp_server_wbn.c.
 * src/server/transport_udp_server.c reads the round-log source and calls
 * serverSendRoundLogErr from its PACKET_ROUND_LOG_REQ handler, and clears a
 * slot's request limits at disconnect and at game start;
 * src/server/udp/udp_server_join.c sends a joiner its first rekey and clears
 * the same limits at join. transportUdpServerSetRoundLogSource,
 * transportUdpServerBroadcastWbnRekey and transportUdpServerHandleWbnReauth
 * are public API and are declared in transport_udp.h.
 *
 * s_roundLogSource and s_roundLogSourceSet are file-scope in the owning
 * translation unit rather than fields of UdpServerState, so that a transport
 * create/destroy cycle does not drop the recorder's registration; the packet
 * handler reaches them through these externs. */
extern RoundLogSource s_roundLogSource;
extern bool           s_roundLogSourceSet;
void serverSendRoundLogErr(uint32_t reqSeq, uint8_t code,
                           const struct sockaddr_in *toAddr);
void transportUdpServerSendWbnRekey(UdpServerClient *c);
void udpServerResetRoundLogLimits(int idx);

#endif /* TRANSPORT_UDP_SERVER_INTERNAL_H */
