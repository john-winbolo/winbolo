/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include "control_event.h"  /* BOT_HOST_BRAIN_NAME_LEN, BOT_HOST_INIT_ARG_LEN */
#include "client_command.h" /* CmdBotStats */
#include "bulk_transfer.h"  /* BulkSender, BulkReceiver */
#include "brain_sync.h"     /* BrainSyncManifest — the Bot Host brain sync */
#include "bot_host_link.h"  /* BotHostAllow, BotHostAnnounce — -bothosts */
#include "upload_policy.h"  /* UploadPolicy, ScriptUploadPolicy */
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

/* Ceiling on how long the anonymous fallback above defers to an outstanding
 * client/verify.  The verify runs on the WinBolo.net worker, and
 * wbn_api_post_impl caps one call at 30 s of transfer; after that the worker
 * pushes a result either way and the next drain delivers it.  1750 ticks is
 * 35 s @ 50 Hz: the 30 s cap, plus five for the worker to wake on a queued
 * job, connect, and for the tick to drain the result.
 *
 * It is a backstop, not a proof that no result arrives later — the worker is
 * single-threaded and oldest-first, so a verify queued behind another slow
 * post can take longer.  What it guarantees is that a result which is lost or
 * arbitrarily delayed cannot take the announcement with it: past here the
 * join is announced un-keyed, which is what would have happened with no hold
 * at all. */
#define WBN_REAUTH_HOLD_TICKS 1750

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

/* Standalone PACKET_CHANNEL frames a running client gets after its snapshot,
 * on top of the one frame the snapshot trailer carries. The trailer takes
 * whatever is left of the datagram once the snapshot is packed, which on a
 * busy tick is a few dozen bytes; these carry what did not fit. Reliable
 * channels were never lost to that budget — they stay queued — but the
 * best-effort effect and voice rings are shallow and drop what they cannot
 * hand over, so a burst of sounds, explosions or voice was thrown away on the
 * tick it happened.
 *
 * Read by serverSendSnapshot in src/server/udp/udp_server_send.c. */
#define SNAPSHOT_EXTRA_CHANNEL_FRAMES 3

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

/* A Bot Host control connection. It holds no tank seat and is never shown to
 * players; the server sends it the assign, unassign, init table, hint and team
 * message events on its own CHANNEL_CONTROL. */
typedef struct {
    struct sockaddr_in addr;
    /* Where the host listens for QUERY and INVITE: the join's address with
     * the listen port the join carries (0 from a host that sent none). */
    struct sockaddr_in listenAddr;
    bool     connected;
    uint64_t connId;
    char     name[JOIN_BOT_HOST_NAME_LEN];
    /* Bot seats this server may give the host: the join's count, then each
     * CTRL_BOT_HOST_SLOTS. A host shared with other servers lowers it as
     * they take its slots. */
    uint8_t  slots;
    /* A player's own Bot Host (the one inside that player's client): it
     * joined with the player's ticket, and it goes when that player's
     * connection (ownerConnId) goes. */
    bool     playerHost;
    uint8_t  ownerSlot;
    uint64_t ownerConnId;
    uint32_t lastReceivedTick;
    uint32_t outSequence;
    uint16_t pingMs;
    ChannelMux channelMux;

    /* Brain sync. The manifest and the file pieces go out on this
     * connection's CHANNEL_BULK through bulk. manifestOwed: the current
     * manifest is still to be sent. */
    BulkSender bulk;
    bool     manifestOwed;
    uint32_t syncSentTick;       /* tickCount the current manifest went out */
    uint32_t syncGen;            /* manifest generation the host reported on */
    uint32_t syncDone;           /* bytes the host has downloaded */
    uint32_t syncTotal;          /* bytes it has to download */
    bool     syncReady;          /* it holds every file of syncGen */
    bool     syncLate;           /* past the sync time limit for this gen */
    /* After the sync time limit the host takes no bots for that game: not
     * while botManagerGameStarts is at most skipUntilStarts. */
    bool     skipGame;
    uint32_t skipUntilStarts;
    /* Files asked for, as (brain << 16) | file, oldest first. */
    uint32_t fileReq[32];
    int      fileReqCount;
    /* The file being sent, a piece per bulk transfer. */
    bool     fileSending;
    uint32_t fileId;
    uint32_t fileOffset;
    uint32_t fileSize;
    uint8_t *fileData;

    /* Brain recordings of the host's bots (udp_server_botrec.c). */
    uint32_t recSentSeq;      /* the block state last told to the host */
    uint32_t recClearSent;    /* the cleardebug count last told to it */
    bool     recUploadOwed;   /* an UPLOAD is to be sent */
    bool     recUploadPaced;
    uint8_t  recState;        /* BOT_HOST_REC_STATE_* the host reported */
    uint8_t  recPercent;
    uint8_t  recBotsDone;     /* UPLOADING: bots whose files are sent */
    uint8_t  recBots;         /* UPLOADING: bots with files to send */
    bool     recLowDiskLogged;
    bool     recMergeWant;    /* a finished upload waits for the merge */
    BulkReceiver recRecv;
    /* The block being received: its staging folder, and the file open. */
    char     recBlock[BOT_HOST_REC_BLOCK_LEN];
    char     recStage[600];
    bool     recBeside;       /* staged beside the session folder: no merge */
    bool     recBad;          /* a piece was refused: the upload fails */
    FILE    *recFile;
    char     recFileName[96];
    uint32_t recFileOffset;
    uint32_t recFiles;        /* files finished */
    uint64_t recBytes;        /* bytes written */
    bool     recDoneOwed;     /* the host's DONE came; the bytes may not have */
    uint32_t recDoneFiles;
    uint32_t recDoneBytes;
    uint32_t recDoneTick;
} BotHostConn;

/* Where one bot seat's brain runs.
 *   LOCAL   - in-process.
 *   PENDING - an assign went to a host; the brain still runs in-process
 *             until the host's bot connection binds with the token.
 *   BOUND   - the bot connection is bound; the brain still runs in-process
 *             until the first input arrives from the host.
 *   REMOTE  - the brain runs on the host. */
enum {
    BOT_SEAT_LOCAL = 0,
    BOT_SEAT_PENDING,
    BOT_SEAT_BOUND,
    BOT_SEAT_REMOTE
};

typedef struct {
    uint8_t  state;
    int8_t   host;               /* index into botHosts[], -1 when LOCAL */
    uint8_t  token[JOIN_BOT_SLOT_TOKEN_LEN];
    uint16_t gen;                /* rises with every assign for the seat */
    uint32_t sinceTick;          /* tickCount the state was entered */
    uint32_t lastPollTick;       /* tickCount of the last settings check */
    /* The settings the host was last sent, compared by the settings check. */
    char     brain[BOT_HOST_BRAIN_NAME_LEN];
    uint8_t  ai;
    uint8_t  team;
    char     initArg[BOT_HOST_INIT_ARG_LEN];
    /* A round started: the host is owed a reload once the bot connection
     * holds the round's map. */
    bool     reloadOwed;
    /* The seat fell back to in-process: it goes back to a host only at the
     * next round start or in the lobby. */
    bool     noReturn;
    /* Full-view seats: the server's mines are sent to the bot connection
     * once its map download is done, a column range per tick. */
    bool     mineSeedDone;
    uint16_t mineSeedColumn;
    /* The gen of the assign that placed the seat on its host (the one with
     * the token): the hosted marker in brainrec.btr names it, and the
     * host's recording file of the seat carries it too. */
    uint16_t placeGen;
} BotHostSeat;

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
    /* Datagrams put on the wire from the server socket since create. A
     * loopback test that runs client and server in lockstep compares it with
     * what its client has read, so it knows when everything sent has arrived.
     * Read through transportUdpServerWireSentCount. Counted only in the test
     * build of the library (WB_ENABLE_NETIMPAIR); the field stays in every
     * build so the struct has one layout whatever includes this header. */
    uint32_t wireSent;
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
     * UPLOAD_MAX_BYTES the bulk receiver reassembles a map into. */
    bool     clientUploadActive[MAX_TANKS];
    uint32_t clientUploadTotal[MAX_TANKS];
    uint64_t upload_last_progress_ms[MAX_TANKS];
    uint8_t  clientUploadBuf[MAX_TANKS][UPLOAD_MAX_BYTES];
    /* What the slot's upload carries: UPLOAD_KIND_MAP or UPLOAD_KIND_SCRIPT,
     * from BEGIN's first byte. Back to MAP whenever the slot clears. */
    uint8_t  clientUploadKind[MAX_TANKS];
    /* A script upload's receive buffer, malloc'd at BEGIN for totalLen (up to
     * LOBBY_PACKAGE_UPLOAD_MAX_BYTES, too big for a static slot per client).
     * NULL when none. Freed at completion, idle expiry, clear/disconnect and
     * transport destroy, always after bulkRecvUp[i].dst stops pointing at it. */
    uint8_t *clientScriptUploadBuf[MAX_TANKS];
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
    /* The same for player-uploaded scripts. Zero-init = ALLOW; the caps
     * default to 32 files / 64 MiB. Where a script lands is the directory the
     * sim resolved (serverSimGetScriptUploadDir), which is what the caps
     * count and the accept callback writes to. */
    ScriptUploadPolicy scriptUploadPolicy;
    uint8_t      scriptUploadMaxFiles;
    uint32_t     scriptUploadMaxStorageBytes;

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
     * could hear them actually asks. The four reasons share one counter above
     * and mean four different things: a server with voice off, a sender past
     * the per-tick cap, a malformed segment, and a repack that failed — the
     * last being the only one that means the server itself is at fault. A
     * slot's map download is not one of them: voice is carried for every
     * connected slot, map or no map. forwarded and capped count this slot as
     * a recipient rather than a sender. Cleared when the slot disconnects. */
    struct {
        uint32_t accepted;
        uint32_t dropVoiceOff;
        uint32_t dropPerTickCap;
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

    /* Bot Host. botHostEnabled is set by -bothostkey or -bothosts; with it
     * off every Bot Host join is refused and no seat ever leaves the
     * process. With a list only the listed hosts are heard; with a key a
     * host must give it; with both, both. */
    bool        botHostEnabled;
    /* A key or a list is set: hosts other than the players' own may join.
     * Without it the only hosts are the players' own. */
    bool        botHostOpen;
    /* -noplayerbothosts: the players are sent no ticket. */
    bool        noPlayerBotHosts;
    /* Each player's ticket, made for its connection (playerTicketConnId),
     * and the tickCount and game start count it last went out at. */
    uint8_t     playerTicket[MAX_TANKS][BOT_HOST_TICKET_LEN];
    uint64_t    playerTicketConnId[MAX_TANKS];
    uint32_t    playerTicketTick[MAX_TANKS];
    uint32_t    playerTicketStarts[MAX_TANKS];
    char        botHostKey[JOIN_BOT_HOST_KEY_LEN + 1];
    int          botHostAllowCount;
    BotHostAllow botHostAllow[BOT_HOST_LINK_MAX_ADDRS];
    char         botHostAllowText[BOT_HOST_LINK_MAX_ADDRS][BOT_HOST_LINK_HOST_LEN + 8];
    /* The last ANNOUNCE from each listed host and when it came (tickCount,
     * 0: none yet). */
    BotHostAnnounce botHostAnnounce[BOT_HOST_LINK_MAX_ADDRS];
    uint32_t     botHostAnnounceTick[BOT_HOST_LINK_MAX_ADDRS];
    /* Ask every listed host for free slots at the next service. */
    const char  *botHostQueryWhy;
    int          botHostLastHumans;
    /* tickCount since which no bots were wanted, 0 while they are. */
    uint32_t     botHostUnwantedSince;
    /* Hosts not on the list that were already logged, so each is logged
     * once. */
    struct sockaddr_in botHostIgnored[32];
    int          botHostIgnoredCount;
    BotHostConn botHosts[MAX_BOT_HOSTS];
    BotHostSeat botSeats[MAX_TANKS];
    int         botHostLastState; /* serverState seen last tick, -1 at start */
    uint32_t    botHostLastGameStarts; /* botManagerGameStarts seen last tick */
    /* The brain files a Bot Host must hold before it takes bots, and the
     * generation that rises each time they change. Gen 0 is no manifest. */
    BrainSyncManifest brainManifest;
    uint32_t    brainManifestGen;
    uint8_t    *brainManifestBlob;
    size_t      brainManifestBlobLen;
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

/* The server transport's clock: SDL_GetTicks, or in the test build of the
 * library the clock a caller set with transportUdpServerSetClock. The ping
 * measurement and the impairment layer read it. Owned by
 * src/server/transport_udp_server.c. */
uint64_t srvNow(void);

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
/* The script bytes the info-request reply carries after the INFO_PACKET, laid
 * out as netpacks.h describes above INFO_SCRIPT_TAIL_MAX, then the server's
 * name and description when it has either, as netpacks.h describes above
 * INFO_SERVER_NAME_MAX, cut to fit what is left of cap. Returns the bytes
 * written, or 0 with nothing written when cap < INFO_SCRIPT_TAIL_MAX. The
 * reply passes INFO_REPLY_TAIL_CAP. */
size_t buildInfoScriptTail(struct ServerSim *sim, uint8_t *out, size_t cap);
/* How many bytes of the server's description that reply has room for
 * beside the scripts the sim runs now and its server name, at most
 * INFO_SERVER_DESC_MAX. The dedicated server prints a note when the
 * description is longer than this. */
size_t infoReplyServerDescRoom(struct ServerSim *sim);
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

/* True when a datagram to or from addr goes through the impairment layer
 * ni. Always true unless the spec has lanonly=1; then only loopback and
 * private IPv4 peers are impaired, and never a -bothosts address. */
bool srvImpairPeer(const NetImpair *ni, const struct sockaddr_in *addr);

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

/* Bot Host connections and the seats they run. Owned by
 * src/server/udp/udp_server_bothost.c. The join handler hands it the two Bot
 * Host join kinds; the dispatcher hands it the host's ping, channel frame and
 * quit and a bot connection's first input and its two Bot Host commands; the
 * timeout sweep services it once per tick. serverBotSlotLost is what every
 * path that would tear a player down calls instead for a bot connection: the
 * seat goes back in-process without a word to anyone. */
bool serverFillRandomBytes(uint8_t *buf, size_t n);
int  serverFindBotHost(const struct sockaddr_in *addr);
void serverSendBotHostAccept(int h, struct ServerSim *sim);
void serverAcceptBotHost(struct ServerSim *sim,
                         const struct sockaddr_in *fromAddr,
                         const char *name, const uint8_t *extra,
                         int extraLen);
void serverBindBotSlot(struct ServerSim *sim,
                       const struct sockaddr_in *fromAddr,
                       const uint8_t *extra, int extraLen,
                       uint8_t clientType);
bool serverBotHostHandlePing(const uint8_t *buf, int len,
                             const struct sockaddr_in *fromAddr);
bool serverBotHostHandleChannel(const uint8_t *buf, int len,
                                const struct sockaddr_in *fromAddr);
bool serverBotHostHandleQuit(struct ServerSim *sim,
                             const struct sockaddr_in *fromAddr);
/* PACKET_BOT_HOST_ANNOUNCE from any address: invite a listed host with free
 * slots while this server wants bots run. */
void serverBotHostHandleAnnounce(struct ServerSim *sim, const uint8_t *buf,
                                 int len, const struct sockaddr_in *fromAddr);
bool serverBotSlotOnInput(struct ServerSim *sim, int slot);
void serverBotSlotLost(struct ServerSim *sim, int slot, const char *why);
/* Player slot is leaving: the Bot Host inside its client is dropped and its
 * bots come back in-process. */
void serverBotHostPlayerGone(struct ServerSim *sim, int slot, const char *why);
/* The server as one Bot Host: its slots (its own capacity answer) and the
 * bots whose brains it runs itself now. */
int serverBotHostServerSlots(struct ServerSim *sim);
int serverBotHostServerBots(struct ServerSim *sim);
void serverBotSlotTeamMsg(struct ServerSim *sim, int slot, const char *text);
void serverBotSlotStats(struct ServerSim *sim, int slot,
                        const CmdBotStats *st);
bool serverBotSlotPingAllowed(struct ServerSim *sim, int slot);
void serverServiceBotHosts(struct ServerSim *sim);
void serverBotHostsReset(void);
/* One CTRL_BOT_HOST_STATUS per connected Bot Host through deliver: the join
 * replay's part of the lobby's Bot Host list. serverEnumSpectatorRoster
 * calls it. */
void serverEnumBotHostRows(void (*deliver)(void *, const ControlEvent *),
                           void *deliverCtx);
/* Queue one control event on Bot Host h's control channel. */
bool serverBotHostSendEvent(int h, const ControlEvent *evt);

/* Bot Host brain recordings (udp_server_botrec.c). */
/* CTRL_BOT_HOST_REC from host h. */
void serverBotRecOnControl(int h, const ControlEvent *evt);
/* Drain host h's CHANNEL_BULK: its recording pieces. */
void serverBotRecDrainBulk(int h);
/* Per tick, under the tick mutex: tell the hosts the block state, run and
 * finish merges. inLobby: the server is in its lobby now. */
void serverBotRecService(struct ServerSim *sim, bool inLobby);
/* The server went back to its lobby: hosts send their finished blocks. */
void serverBotRecOnLobby(void);
/* A game started: uploads go on slowly. */
void serverBotRecOnGameStart(void);
/* Host h connected (inLobby: the server is in its lobby) or is going. */
void serverBotRecHostConnected(int h, bool inLobby);
void serverBotRecHostGone(int h);
/* The rec state and percent for host h's lobby row. */
void serverBotRecRow(int h, uint8_t *state, uint8_t *percent,
                     uint8_t *botsDone, uint8_t *bots);
/* True while host h sends or waits for a merge: it is not released. */
bool serverBotRecBusy(int h);
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
 * deliver a player-addressed event. Both log event names through
 * controlEventDiagName (control_event.h). */
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
void udpServerFreeScriptUploadBufs(void);
void udpServerResetMapReaskLimit(int idx);
/* Whether one more script upload, name at len bytes, fits the persist caps
 * (scriptUploadMaxFiles, scriptUploadMaxStorageBytes) counted over the
 * .scenario and .lua files directly in dir. A file already there under name
 * counts by its change in size, not as one more file. Asked at BEGIN and
 * again when the bytes are in. */
bool udpServerScriptUploadFitsCaps(const char *dir, const char *name,
                                   uint32_t len);

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
/* Send a lobby event to every connected player client (bot connections of a
 * Bot Host left out) and every live spectator, as the spectator roster rows
 * go. */
void serverBroadcastLobbyControl(const ControlEvent *evt);
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

/* Is a client/verify still outstanding for this slot, as of nowTick?
 *
 * The grace sweep's anonymous PLAYER_JOIN and the keyed one a verify result
 * publishes are not the same event.  winbolonetAddEvent resolves the slot's
 * key when it is called, and winbolonetServerUpdate leaves player_a out when
 * that key is empty, so the sweep announces an unidentified join and the
 * result announces the account's.  Exactly one of the two may go out, and
 * while a verify can still answer, the announcement is the verify's to make:
 * publishing the sweep's would leave the account with keyed kills, wins and a
 * leave against no join.
 *
 * The verify used to run on the tick, so nowTick could not advance while one
 * was outstanding and the sweep could not reach a slot waiting on one.  This
 * states that rule rather than leaving it to depend on the call blocking.
 *
 * nowTick bounds the deference: past WBN_REAUTH_HOLD_TICKS the hold lapses
 * and the sweep announces as it otherwise would.  The sweep asks; it does not
 * read the table. */
bool udpServerReauthVerifyOutstanding(BYTE slot, uint32_t nowTick);

/* Drop any outstanding verify recorded for this slot, releasing the hold
 * above.  The disconnect path runs it beside wbnJoinClear: the slot is about
 * to be handed to somebody else, and a stale entry would defer the new
 * occupant's announcement. */
void udpServerClearReauthPending(BYTE slot);

/* Same, for every slot.  transportUdpServerCreate runs it: tickCount restarts
 * at 0 there, so an entry left by an earlier server on this process would
 * carry a deadline the new tick counter cannot reach for a long time. */
void udpServerClearAllReauthPending(void);

#endif /* TRANSPORT_UDP_SERVER_INTERNAL_H */
