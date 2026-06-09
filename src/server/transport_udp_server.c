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

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

/* ---- Simulated server latency (set to 0 to disable) ----
 * Delays packet delivery on the server receive path by this many ms.
 * For localhost testing, set to 100 to simulate ~200ms RTT (100ms each way
 * since the server delays both receiving client inputs and sending snapshots
 * back through the same delayed receive path on the client's next poll).
 * Only compiled into the dedicated server target (WinBoloDS). */
#define SIM_LATENCY_SERVER_MS  0

#if SIM_LATENCY_SERVER_MS > 0 && !defined(HAVE_SCREEN_C)
typedef struct {
    uint8_t  data[2048];
    int      len;
    struct sockaddr_in from;
    uint64_t deliverAt;  /* SDL tick (ms) when this packet should be released */
} DelayedPacket;

#define DELAY_QUEUE_SIZE 512
static DelayedPacket serverDelayQueue[DELAY_QUEUE_SIZE];
static int serverDelayHead = 0;
static int serverDelayCount = 0;

/* Receive with simulated latency — buffers packets and releases after delayMs. */
static int udpRecvFromDelayed(SOCKET sock, uint8_t *buf, int maxLen,
                               struct sockaddr_in *fromAddr,
                               int delayMs, DelayedPacket *queue,
                               int *head, int *count) {
    /* Drain socket into delay queue */
    while (*count < DELAY_QUEUE_SIZE) {
        struct sockaddr_in tmpAddr;
        socklen_t tmpLen = sizeof(tmpAddr);
        int n = recvfrom(sock, (char *)queue[(*head + *count) % DELAY_QUEUE_SIZE].data,
                         sizeof(queue[0].data), 0,
                         (struct sockaddr *)&tmpAddr, &tmpLen);
        if (n == SOCKET_ERROR) break;
        int idx = (*head + *count) % DELAY_QUEUE_SIZE;
        queue[idx].len = n;
        queue[idx].from = tmpAddr;
        queue[idx].deliverAt = SDL_GetTicks() + delayMs;
        (*count)++;
    }

    /* Release oldest packet if its delay has elapsed */
    if (*count > 0 && SDL_GetTicks() >= queue[*head].deliverAt) {
        int copyLen = queue[*head].len;
        if (copyLen > maxLen) copyLen = maxLen;
        memcpy(buf, queue[*head].data, copyLen);
        *fromAddr = queue[*head].from;
        *head = (*head + 1) % DELAY_QUEUE_SIZE;
        (*count)--;
        return copyLen;
    }
    return -1;
}
#endif

/* ---- Server dedicated recv thread ----
 * A background thread continuously polls the server socket and queues
 * packets into an SPSC ring buffer.  The timer callback drains the
 * queue each tick, keeping packet processing on the main thread.
 * Skipped when simulated latency is enabled (udpRecvFromDelayed). */

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

/* Per-client map download tracking */
typedef struct {
    BYTE    *compressedMap;     /* Per-client copy of compressed map (owned, must free) */
    uint32_t mapSize;          /* Total compressed map size */
    uint16_t totalChunks;      /* Total number of chunks */
    bool    *chunkAcked;       /* Which chunks the client has acked */
    uint16_t chunksAcked;      /* Number of acked chunks */
    bool     downloadComplete; /* True when all chunks acked */
    uint32_t lastSendTick;     /* Last tick we sent chunks (for resend timing) */
} ClientMapDownload;

#define UPLOAD_MAX_BYTES (64u * 1024u)
#define LOBBY_REQ_COOLDOWN_TICKS 25  /* ~0.5s at 50 Hz */

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

bool wbnRekeyTargetSelected(bool connected, uint8_t clientFlags) {
    return connected && (clientFlags & PLAYER_FLAG_WBN_VERIFIED) != 0;
}

JoinCollisionVerdict joinCollisionDecide(bool incomingWillAuth,
                                         bool existingIsVerified) {
    if (existingIsVerified) return JOIN_COLLISION_REJECT_VERIFIED;
    if (!incomingWillAuth)  return JOIN_COLLISION_REJECT_IN_USE;
    return JOIN_COLLISION_ADMIT_PROVISIONAL;
}

/* Server-side global state */
static struct {
    SOCKET sock;
    bool running;
    UdpServerClient clients[MAX_TANKS];
    uint32_t tickCount;

    /* Compressed map buffer for sending to joining clients */
    BYTE     compressedMap[MAP_DOWNLOAD_MAX_SIZE];
    uint32_t compressedMapSize;

    /* Per-client map download state */
    ClientMapDownload mapDownload[MAX_TANKS];

    /* Per-client reliable event queues (game events: sounds, kills, etc.) */
    ClientEventQueue eventQueues[MAX_TANKS];

    /* Per-client reliable map event queues (EVENT_MAP_CHANGE only) */
    ClientEventQueue mapEventQueues[MAX_TANKS];

    /* Per-client reliable control event queues — every control event
     * (lobby, chat, alliance, game phase, etc.) lands here so the
     * carrier path can retransmit it until ACKed.  Phase 3 keeps the
     * legacy udpSendTo running in parallel; nothing reads back yet. */
    ClientControlEventQueue controlEventQueues[MAX_TANKS];
    /* Suppress immediate-send-on-enqueue during the sync-replay burst
     * fired by serverSimRegisterSubscriber, so one carrier datagram
     * packs all replayed events instead of one per event.  Set/cleared
     * by the UDP server around the register call; consumed by the
     * carrier path (Phase 5).  Unused in Phase 3. */
    bool                    controlSyncInProgress[MAX_TANKS];
    /* Slots that were force-disconnected from inside a control-event
     * deliver callback (queue overflow) and still need their sim-side
     * teardown (serverSimRemovePlayer). That call publishes events, so it
     * can't run mid-publish; it is deferred to
     * transportUdpServerDrainPendingRemovals at a safe point in the tick. */
    bool                    pendingSimRemove[MAX_TANKS];
    /* Most recent tick at which controlEventQueues[i].ackedSeq advanced
     * (or the tick the slot connected, for a fresh slot).  Bounds how
     * long the queue may sit unacked before the per-client retransmit
     * timeout in transportUdpServerCheckTimeouts disconnects the slot. */
    uint32_t                controlEventLastAckProgressTick[MAX_TANKS];

    /* Game lock — prevents new players from joining */
    bool gameLocked;           /* Server admin lock */
    bool clientLocked[MAX_TANKS]; /* Per-player lock votes */

    /* Per-client map upload state. clientUploadActive=true between
     * PACKET_LOBBY_MAP_UPLOAD_BEGIN and the final write-out at
     * MAP_UPLOAD_DONE. clientUploadHave tracks the highest contiguous
     * byte received. clientUploadBuf is a fixed slot of UPLOAD_MAX_BYTES. */
    bool     clientUploadActive[MAX_TANKS];
    uint32_t clientUploadTotal[MAX_TANKS];
    uint32_t clientUploadHave[MAX_TANKS];
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
} udpServer;


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
    udpSendTo(udpServer.sock, buf, pos, addr);
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
    default:                    return "<unknown>";
    }
}

/* Pack and send a PACKET_CONTROL_TICK to one client containing every
 * still-unacked event in that client's control queue, up to UDP_MAX_PAYLOAD.
 * Per-event wire layout matches the snapshot control-event tail:
 * type(1) + bodyLen(2 BE) + body(N).  No-op when the queue is fully
 * acked or the client is disconnected. */
static void transportUdpServerSendControlTick(int clientIdx) {
    UdpServerClient *client;
    ClientControlEventQueue *q;
    uint8_t buf[UDP_MAX_PAYLOAD];
    int pos;
    int countOffset;
    int count = 0;
    uint32_t seq;
    uint32_t baseSeq;

    if (clientIdx < 0 || clientIdx >= MAX_TANKS) return;
    client = &udpServer.clients[clientIdx];
    q = &udpServer.controlEventQueues[clientIdx];
    if (!client->connected) return;
    if (q->ackedSeq == q->nextSeq) return;  /* nothing to send */

    packHeader(buf, PACKET_CONTROL_TICK, client->outSequence++);
    pos = PACKET_HEADER_SIZE;
    baseSeq = q->ackedSeq;
    packU32(buf + pos, baseSeq);
    pos += 4;
    countOffset = pos;
    pos += 1;  /* count byte — backfilled after the loop */

    for (seq = q->ackedSeq; seq < q->nextSeq; seq++) {
        uint32_t idx = seq % CONTROL_EVENT_QUEUE_SIZE;
        ControlEncodeBodyFn enc;
        size_t bodyLen = 0;
        if (q->buffer[idx].seq != seq) break;  /* wrapped — slot reused */
        enc = transportControlCodecBodyEncoder(q->buffer[idx].event.type);
        if (enc == NULL) continue;
        if (pos + 3 > (int)sizeof(buf)) break;
        if (enc(&q->buffer[idx].event, client,
                buf + pos + 3, sizeof(buf) - pos - 3, &bodyLen) != ENCODE_OK) {
            break;
        }
        buf[pos]   = (uint8_t)q->buffer[idx].event.type;
        packU16(buf + pos + 1, (uint16_t)bodyLen);
        pos += 3 + (int)bodyLen;
        count++;
        if (count >= 255) break;  /* cap to uint8_t */
    }

    buf[countOffset] = (uint8_t)count;
    if (count > 0) {
        udpSendTo(udpServer.sock, buf, pos, &client->addr);
        {
            char typesBuf[256];
            int tbPos = 0;
            uint32_t s;
            typesBuf[0] = '\0';
            for (s = baseSeq; s < baseSeq + (uint32_t)count && tbPos < (int)sizeof(typesBuf) - 32; s++) {
                uint32_t idx2 = s % CONTROL_EVENT_QUEUE_SIZE;
                tbPos += snprintf(typesBuf + tbPos, sizeof(typesBuf) - tbPos,
                                  "%s%s(seq=%u)", tbPos == 0 ? "" : ",",
                                  mpDiagCtrlName((int)q->buffer[idx2].event.type),
                                  (unsigned)s);
            }
            mpDiagLog("[srv] CONTROL_TICK send slot=%d baseSeq=%u count=%d bytes=%d ackedSeq=%u nextSeq=%u types=[%s]",
                      clientIdx, (unsigned)baseSeq, count, pos,
                      (unsigned)q->ackedSeq, (unsigned)q->nextSeq, typesBuf);
        }
    } else {
        mpDiagLog("[srv] CONTROL_TICK send slot=%d count=0 (no encodable events; ackedSeq=%u nextSeq=%u)",
                  clientIdx, (unsigned)q->ackedSeq, (unsigned)q->nextSeq);
    }
}

/* Retransmit unacked control events to every connected client.  Driven
 * by server_lifecycle.c at a 4-tick (~80ms) cadence during
 * lobby/countdown/gameover — running phases get retransmit for free via
 * the snapshot tail. */
void transportUdpServerRetransmitUnackedControl(void) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected &&
            udpServer.controlEventQueues[i].ackedSeq <
            udpServer.controlEventQueues[i].nextSeq) {
            transportUdpServerSendControlTick(i);
        }
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
    ClientControlEventQueue *q;
    uint32_t seq;

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

    /* Enqueue into this client's reliable control queue. */
    q = &udpServer.controlEventQueues[idx];
    if (!controlEventQueueHasSpace(q)) {
        /* Don't silently drop — every control event carries state-sync
         * semantics.  Log and disconnect.  The 500-tick unacked-control
         * timeout (Phase 7) catches the offending client first in
         * practice; this is the belt-and-braces. */
        WB_LOG_ERROR(WB_LOG_CAT_NET,
                     "control queue overflow for slot %d, disconnecting",
                     idx);
        mpDiagLog("[srv] OVERFLOW slot=%d type=%s ackedSeq=%u nextSeq=%u -> disconnecting",
                  idx, mpDiagCtrlName((int)evt->type),
                  (unsigned)q->ackedSeq, (unsigned)q->nextSeq);
        serverDisconnectClient(serverSimGetActive(), idx, false);
        /* serverDisconnectClient handles the transport teardown and stops
         * the recursion (connected=false), but the sim-side removal
         * (serverSimRemovePlayer) publishes events and so can't run inside
         * this deliver callback. Defer it; otherwise the slot keeps
         * playerConnected/inUse set and leaks as a phantom. */
        udpServer.pendingSimRemove[idx] = true;
        return;
    }
    /* If the queue was empty (ackedSeq == nextSeq) we have to restart the
     * unacked-control timeout clock — controlEventLastAckProgressTick was
     * last touched on the previous ack, which could be many seconds ago
     * during a quiet lobby.  Without this reset, the very first event
     * after a long idle period gets compared against a stale baseline and
     * the next checkTimeouts call fires CONTROL_UNACKED_TIMEOUT_TICKS
     * immediately, kicking the client before its ACK has a chance to
     * round-trip back.  Observed in mp-logging-90800.txt:60→87. */
    if (q->ackedSeq == q->nextSeq) {
        udpServer.controlEventLastAckProgressTick[idx] = udpServer.tickCount;
    }
    seq = q->nextSeq;
    q->buffer[seq % CONTROL_EVENT_QUEUE_SIZE].seq   = seq;
    q->buffer[seq % CONTROL_EVENT_QUEUE_SIZE].event = *evt;
    q->nextSeq++;
    controlEventQueueAssertValid(q, "enqueue");
    {
        int qDepth = (int)(q->nextSeq - q->ackedSeq);
        int phase = (int)serverSimGetState(serverSimGetActive());
        int syncInProg = udpServer.controlSyncInProgress[idx] ? 1 : 0;
        const char *extra = "";
        char extraBuf[128];
        extraBuf[0] = '\0';
        if (evt->type == CTRL_LOBBY_SLOT) {
            snprintf(extraBuf, sizeof(extraBuf),
                     " lobbySlot[player=%d team=%d ready=%d connected=%d isBot=%d name='%.12s']",
                     (int)evt->u.lobbySlot.playerNum,
                     (int)evt->u.lobbySlot.slot.teamNumber,
                     (int)evt->u.lobbySlot.slot.ready,
                     (int)evt->u.lobbySlot.slot.connected,
                     (int)evt->u.lobbySlot.slot.isBot,
                     evt->u.lobbySlot.slot.playerName);
            extra = extraBuf;
        } else if (evt->type == CTRL_PLAYER_JOIN) {
            snprintf(extraBuf, sizeof(extraBuf),
                     " playerJoin[player=%d name='%.16s']",
                     (int)evt->u.playerJoin.playerNum,
                     evt->u.playerJoin.name);
            extra = extraBuf;
        }
        mpDiagLog("[srv] ENQ slot=%d seq=%u type=%s qDepth=%d phase=%d syncInProg=%d%s",
                  idx, (unsigned)seq, mpDiagCtrlName((int)evt->type),
                  qDepth, phase, syncInProg, extra);
    }

    /* Sync-replay coalescing: suppress immediate sends during the
     * subscriber's synchronous replay burst.  The subscriber-registration
     * wrapper fires one TICK after the burst completes. */
    if (udpServer.controlSyncInProgress[idx]) return;

    if (serverSimGetState(serverSimGetActive()) == serverStateRunning) {
        /* Snapshot tail picks it up automatically on the next tick. */
        return;
    }
    /* Non-running phase: emit a PACKET_CONTROL_TICK now carrying the
     * new event plus any other still-unacked events in the queue. */
    transportUdpServerSendControlTick(idx);
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
    udpServer.clientUploadHave[idx]     = 0;
    udpServer.clientUploadName[idx][0]  = '\0';
    udpServer.clientReqCooldownTicks[idx] = 0;
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
    uint8_t acceptBuf[PACKET_HEADER_SIZE + 9];
    int pos;

    packHeader(acceptBuf, PACKET_JOIN_ACCEPT,
               udpServer.clients[slot].outSequence++);
    pos = PACKET_HEADER_SIZE;
    acceptBuf[pos++] = (uint8_t)slot;
    packU32(acceptBuf + pos, serverSimGetTick(sim));
    pos += 4;
    packU32(acceptBuf + pos, udpServer.compressedMapSize);
    pos += 4;

    /* wire-only: per-client handshake (response to a single client's request) */
    udpSendTo(udpServer.sock, acceptBuf, pos, addr);
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
    udpSendTo(udpServer.sock, buf, sizeof(buf), &c->addr);
}

/* Broadcast PACKET_WBN_REKEY to every connected client that was
 * WBN-verified last round, after the server rotates its server_key
 * (post-returnToLobby).  Each client mints a fresh player_key against
 * the new key and re-auths, re-registering for the new session.
 *
 * The gate is the sim-side PLAYER_FLAG_WBN_VERIFIED, NOT the per-slot
 * WBN key: this runs right after winbolonetEndSession, which has already
 * wiped every key, so a key-based gate (winboloNetIsPlayerParticipant)
 * would match nobody and silently strand every player un-keyed for the
 * new round.  The verified flag survives serverSimResetGameWorld, so it
 * is the durable cross-round signal.  Re-arm the deferred-join state for
 * each rekeyed slot so the incoming reauth fires a fresh keyed
 * PLAYER_JOIN for the new game (or the grace sweep an anonymous one if
 * the reauth never lands). */
void transportUdpServerBroadcastWbnRekey(ServerSim *sim) {
    int i;
    if (!winbolonetIsRunning()) return;
    for (i = 0; i < MAX_TANKS; i++) {
        uint8_t flags =
            playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, (BYTE)i);
        if (!wbnRekeyTargetSelected(udpServer.clients[i].connected, flags))
            continue;
        transportUdpServerSendWbnRekey(&udpServer.clients[i]);
        wbnJoinArm(&udpServer.clients[i].wbnJoin,
                   udpServer.tickCount, WBN_JOIN_REGISTER_GRACE_TICKS);
    }
}

/* Send map chunks to a client that is downloading */
static void serverSendMapChunks(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    UdpServerClient *client = &udpServer.clients[slot];
    uint16_t i;

    if (dl->downloadComplete) return;

    for (i = 0; i < dl->totalChunks; i++) {
        uint8_t chunkBuf[PACKET_HEADER_SIZE + 4 + MAP_DOWNLOAD_CHUNK_SIZE];
        uint32_t offset;
        uint16_t chunkSize;
        int pktLen;

        if (dl->chunkAcked[i]) continue;

        offset = (uint32_t)i * MAP_DOWNLOAD_CHUNK_SIZE;
        chunkSize = (uint16_t)(dl->mapSize - offset);
        if (chunkSize > MAP_DOWNLOAD_CHUNK_SIZE) {
            chunkSize = MAP_DOWNLOAD_CHUNK_SIZE;
        }

        packHeader(chunkBuf, PACKET_MAP_DOWNLOAD, client->outSequence++);
        packU16(chunkBuf + PACKET_HEADER_SIZE, i);
        packU16(chunkBuf + PACKET_HEADER_SIZE + 2, chunkSize);
        memcpy(chunkBuf + PACKET_HEADER_SIZE + 4,
               dl->compressedMap + offset, chunkSize);
        pktLen = PACKET_HEADER_SIZE + 4 + chunkSize;

        /* wire-only: per-client reliability (acked / per-tick to one slot) */
        udpSendTo(udpServer.sock, chunkBuf, pktLen, &client->addr);
    }

    dl->lastSendTick = udpServer.tickCount;
}

/* Initialize map download tracking for a client */
static void serverInitMapDownload(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];

    if (dl->compressedMap != NULL) {
        free(dl->compressedMap);
    }
    dl->compressedMap = (BYTE *)malloc(udpServer.compressedMapSize);
    memcpy(dl->compressedMap, udpServer.compressedMap, udpServer.compressedMapSize);
    dl->mapSize = udpServer.compressedMapSize;
    dl->totalChunks = (uint16_t)((dl->mapSize + MAP_DOWNLOAD_CHUNK_SIZE - 1) / MAP_DOWNLOAD_CHUNK_SIZE);
    dl->chunksAcked = 0;
    dl->downloadComplete = FALSE;
    dl->lastSendTick = 0;

    if (dl->chunkAcked != NULL) {
        free(dl->chunkAcked);
    }
    dl->chunkAcked = (bool *)calloc(dl->totalChunks, sizeof(bool));
}

/* Clean up map download tracking for a client */
static void serverCleanupMapDownload(int slot) {
    ClientMapDownload *dl = &udpServer.mapDownload[slot];
    if (dl->compressedMap != NULL) {
        free(dl->compressedMap);
        dl->compressedMap = NULL;
    }
    if (dl->chunkAcked != NULL) {
        free(dl->chunkAcked);
        dl->chunkAcked = NULL;
    }
    dl->downloadComplete = FALSE;
    dl->chunksAcked = 0;
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
    fprintf(stderr, "[UDP SERVER] Join request received, len=%d\n", len);
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
        fprintf(stderr, "[UDP SERVER] Join request malformed (need %d, got %d)\n",
                joinReqMin, len);
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
        /* Resend map chunks if download not complete */
        if (!udpServer.mapDownload[slot].downloadComplete) {
            serverSendMapChunks(slot);
        }
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

    /* Skip version bytes */
    pos += 3;

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
    if (len > pos) {
        wantRejoin       = (buf[pos] & JOIN_FLAG_WANT_REJOIN) != 0;
        incomingWillAuth = (buf[pos] & JOIN_FLAG_WILL_AUTHENTICATE) != 0;
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

    /* Initialize reliable event queues for this client */
    udpServer.eventQueues[slot].nextSeq = 1;
    udpServer.eventQueues[slot].ackedSeq = 1;
    memset(udpServer.eventQueues[slot].buffer, 0, sizeof(udpServer.eventQueues[slot].buffer));
    udpServer.mapEventQueues[slot].nextSeq = 1;
    udpServer.mapEventQueues[slot].ackedSeq = 1;
    memset(udpServer.mapEventQueues[slot].buffer, 0, sizeof(udpServer.mapEventQueues[slot].buffer));
    udpServer.controlEventQueues[slot].nextSeq = 1;
    udpServer.controlEventQueues[slot].ackedSeq = 1;
    memset(udpServer.controlEventQueues[slot].buffer, 0, sizeof(udpServer.controlEventQueues[slot].buffer));
    udpServer.controlEventLastAckProgressTick[slot] = udpServer.tickCount;
    controlEventQueueAssertValid(&udpServer.controlEventQueues[slot], "join-init");

    /* Merge client-supplied hints with server-determined WBN trust into a
     * single clientFlags byte, then run the four-step join sequence so a
     * single CTRL_PLAYER_JOIN fans out with name, country, clientType,
     * and clientFlags all populated. */
    {
        uint8_t flags = clientHints & PLAYER_CLIENT_HINT_MASK;
        if (incomingIsWBN)                  flags |= PLAYER_FLAG_WBN_VERIFIED;
        if (incomingIsWBN && wbnHasSteam)   flags |= PLAYER_FLAG_WBN_STEAM_LINKED;
        if (incomingIsWBN && wbnIsSupporter) flags |= PLAYER_FLAG_SUPPORTER;
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
     * controlSyncInProgress brackets the synchronous replay burst
     * (~10-15 events) so udpClientDeliverControl skips its immediate-
     * send-on-enqueue path; the post-burst flush below packs the whole
     * burst into a single PACKET_CONTROL_TICK when non-running.  During
     * running, no explicit flush is needed — the snapshot tail naturally
     * bundles the queued events into the next outgoing snapshot. */
    mpDiagLog("[srv] SYNC START slot=%d phase=%d (about to register subscriber + replay)",
              slot, (int)serverSimGetState(sim));
    udpServer.controlSyncInProgress[slot] = true;
    udpServer.clients[slot].controlSub =
        serverSimRegisterSubscriber(sim, udpClientDeliverControl,
                                    &udpServer.clients[slot]);
    udpServer.controlSyncInProgress[slot] = false;
    {
        ClientControlEventQueue *qd = &udpServer.controlEventQueues[slot];
        mpDiagLog("[srv] SYNC END slot=%d queuedEvents=%u (ackedSeq=%u nextSeq=%u) phase=%d -> %s",
                  slot,
                  (unsigned)(qd->nextSeq - qd->ackedSeq),
                  (unsigned)qd->ackedSeq, (unsigned)qd->nextSeq,
                  (int)serverSimGetState(sim),
                  serverSimGetState(sim) == serverStateRunning
                      ? "deferring flush to next snapshot"
                      : "flushing via PACKET_CONTROL_TICK");
    }
    if (serverSimGetState(sim) != serverStateRunning) {
        transportUdpServerSendControlTick(slot);
    }

    /* Announce the join to WBN.  If the slot's key already rode the JOIN
     * field and verified inline (incomingIsWBN), the player is already a
     * participant — register keyed right now.  Otherwise, when WBN is
     * running, defer: the rekey we sent above prompts a reauth that fills
     * the key and fires a keyed join (transportUdpServerHandleWbnReauth);
     * if no reauth lands within the grace window the per-tick sweep in
     * transportUdpServerCheckTimeouts fires an anonymous one.  On a
     * non-WBN server there is nothing to defer (and winbolonetAddEvent is
     * a no-op anyway). */
    wbnJoinClear(&udpServer.clients[slot].wbnJoin);
    if (incomingIsWBN) {
        winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                           (BYTE)slot, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
    } else if (winbolonetIsRunning()) {
        wbnJoinArm(&udpServer.clients[slot].wbnJoin,
                   udpServer.tickCount, WBN_JOIN_REGISTER_GRACE_TICKS);
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

    /* Initialize map download and send first batch of chunks */
    serverInitMapDownload(slot);
    fprintf(stderr, "[UDP SERVER] Sending %u map chunks to slot %d\n",
            udpServer.mapDownload[slot].totalChunks, slot);
    serverSendMapChunks(slot);

    /* The sync-replay just enqueued a CTRL_PLAYER_JOIN for every in-use
     * player into this client's controlEventQueue, so the JOIN-time
     * roster is covered by the reliable bus path.  The needsPlayerList
     * flag is set here only so the same per-tick resync that catches
     * the game-start race (see transportUdpServerOnGameStart) also fires
     * once for fresh joiners — belt-and-braces; harmless overlap. */
    udpServer.clients[slot].needsPlayerList = true;

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

/* Handle input packet from a connected client */
static void serverHandleInput(const uint8_t *buf, int len,
                              const struct sockaddr_in *fromAddr,
                              ServerSim *sim) {
    int clientIdx;
    int pos = PACKET_HEADER_SIZE;
    uint8_t inputCount;
    int i;

    clientIdx = serverFindClient(fromAddr);
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

        /* Advance reliable event ACKs from this client */
        if (pkt.eventAck > udpServer.eventQueues[clientIdx].ackedSeq) {
            udpServer.eventQueues[clientIdx].ackedSeq = pkt.eventAck;
        }
        if (pkt.mapEventAck > udpServer.mapEventQueues[clientIdx].ackedSeq) {
            udpServer.mapEventQueues[clientIdx].ackedSeq = pkt.mapEventAck;
        }
        {
            ClientControlEventQueue *cq = &udpServer.controlEventQueues[clientIdx];
            uint32_t newAck = pkt.controlEventAck;
            /* Ignore acks beyond nextSeq — they come from the client's
             * view of the OLD sequence space, after the server has
             * already wiped its queue at game-start.  Accepting a
             * stale-future ack would set ackedSeq > nextSeq, breaking
             * the snapshot pack loop's `seq < nextSeq` condition and
             * silently stranding every subsequent event (CTRL_GAME_PHASE_RUNNING
             * being the canonical victim).  The client will send a fresh
             * ack from the new sequence space on its next round-trip. */
            if (newAck > cq->nextSeq) {
                mpDiagLog("[srv] ACK-input STALE slot=%d ack=%u > nextSeq=%u (post-wipe stale, ignoring)",
                          clientIdx, (unsigned)newAck, (unsigned)cq->nextSeq);
            } else if (newAck > cq->ackedSeq) {
                uint32_t oldAck = cq->ackedSeq;
                cq->ackedSeq = newAck;
                udpServer.controlEventLastAckProgressTick[clientIdx] = udpServer.tickCount;
                controlEventQueueAssertValid(cq, "ack-advance(input)");
                mpDiagLog("[srv] ACK-advance(input) slot=%d %u -> %u (nextSeq=%u)",
                          clientIdx, (unsigned)oldAck, (unsigned)newAck,
                          (unsigned)cq->nextSeq);
            }
        }

        /* Only apply if this is a newer input than what we last processed */
        if (pkt.tick > serverSimGetLastProcessedInput(sim, clientIdx)) {
            if (udpServer.clients[clientIdx].inputsThisTick >= 4) break;
            serverSimApplyInput(sim, &pkt);
            udpServer.clients[clientIdx].inputsThisTick++;
        }
    }
}

/* Handle PACKET_CONTROL_ACK from a connected client — advance the
 * per-client control-event ackedSeq.  Carries the client's next-expected
 * control seq; never goes backwards.  Also bumps lastReceivedTick so
 * the no-traffic timeout stays satisfied while only the ACK channel is
 * flowing (e.g. quiet lobby). */
static void serverHandleControlAck(const uint8_t *buf, int len,
                                   const struct sockaddr_in *fromAddr) {
    int clientIdx;
    uint32_t ack;
    if (len < PACKET_HEADER_SIZE + 4) return;
    clientIdx = serverFindClient(fromAddr);
    if (clientIdx < 0) return;
    ack = unpackU32(buf + PACKET_HEADER_SIZE);
    {
        ClientControlEventQueue *cq = &udpServer.controlEventQueues[clientIdx];
        /* Reject stale-future acks (see serverHandleInput's matching
         * branch).  A client whose ACK was in flight at the moment of
         * a game-start queue wipe will look like ack=<old nextSeq>
         * arriving at a server with nextSeq=2.  Accepting that ack
         * would push ackedSeq past nextSeq and silently strand every
         * subsequent event in the new sequence space. */
        if (ack > cq->nextSeq) {
            mpDiagLog("[srv] ACK-CTRL_ACK STALE slot=%d ack=%u > nextSeq=%u (post-wipe stale, ignoring)",
                      clientIdx, (unsigned)ack, (unsigned)cq->nextSeq);
        } else if (ack > cq->ackedSeq) {
            uint32_t oldAck = cq->ackedSeq;
            cq->ackedSeq = ack;
            udpServer.controlEventLastAckProgressTick[clientIdx] = udpServer.tickCount;
            controlEventQueueAssertValid(cq, "ack-advance(CTRL_ACK)");
            mpDiagLog("[srv] ACK-advance(CTRL_ACK pkt) slot=%d %u -> %u (nextSeq=%u)",
                      clientIdx, (unsigned)oldAck, (unsigned)ack,
                      (unsigned)cq->nextSeq);
        } else {
            mpDiagLog("[srv] ACK pkt no-op slot=%d ack=%u currentAcked=%u",
                      clientIdx, (unsigned)ack, (unsigned)cq->ackedSeq);
        }
    }
    udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;
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
    udpSendTo(udpServer.sock, pongBuf, sizeof(pongBuf), fromAddr);
}

/* Build and send a snapshot to one client.
 * Uses serverSimBuildSnapshot() for all game state, then serializes
 * and appends reliable events from the per-client queue. */
static void serverSendSnapshot(ServerSim *sim, int clientIdx) {
    uint8_t buf[2048];
    int pos;
    int i;
    int countsPos;
    int reliableEventCount = 0;
    uint32_t reliableBaseSeq = 0;
    int mapEventCount = 0;
    uint32_t mapEventBaseSeq = 0;
    int controlEventCount = 0;
    uint32_t controlEventBaseSeq = 0;
    SnapshotHeader hdr;
    TankSnapshot tankSnaps[MAX_TANKS];
    ShellSnapshot shellSnaps[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot tkExplSnaps[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot baseSnaps[MAX_SNAPSHOT_BASES];
    PillSnapshot pillSnaps[MAX_SNAPSHOT_PILLS];
    GameEvent eventSnaps[MAX_SNAPSHOT_EVENTS];
    ClientEventQueue *evQ = &udpServer.eventQueues[clientIdx];
    ClientEventQueue *mapQ = &udpServer.mapEventQueues[clientIdx];
    ClientControlEventQueue *controlQ = &udpServer.controlEventQueues[clientIdx];
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
     * + reliableEventCount(1) + reliableBaseSeq(4)
     * + mapEventCount(1) + mapEventBaseSeq(4)
     * + controlEventCount(1) + controlEventBaseSeq(4)
     * + mapChecksum(2) + returnToLobbyTicks(2) = 32 bytes */
    packU32(buf + pos, hdr.serverTick);
    pos += 4;
    packU32(buf + pos, hdr.lastProcessedInput);
    pos += 4;
    countsPos = pos;
    pos += 24; /* 8 count bytes + 4 byte reliableBaseSeq + 4 byte mapEventBaseSeq + 4 byte controlEventBaseSeq + 2 byte mapChecksum + 2 byte returnToLobbyTicks */

    /* Pack tank snapshots — variable length: stubs are 1 byte, full
     * entries are TANK_SNAPSHOT_WIRE_SIZE bytes. */
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

    /* Pack reliable game events from per-client queue (all unacked events).
     * These are transport-specific — serverSimBuildSnapshot() produces
     * per-tick events, but the reliable queue handles retransmission. */
    reliableBaseSeq = evQ->ackedSeq;
    {
        uint32_t seq;
        for (seq = evQ->ackedSeq; seq < evQ->nextSeq; seq++) {
            uint32_t idx = seq % RELIABLE_EVENT_BUFFER_SIZE;
            if (pos + GAME_EVENT_MAX_WIRE_SIZE > (int)sizeof(buf)) break;
            if (evQ->buffer[idx].seq != seq) break; /* Buffer wrapped — stop */
            pos += packGameEvent(buf + pos, &evQ->buffer[idx].event);
            reliableEventCount++;
            if (reliableEventCount >= 255) break; /* Cap to uint8_t max */
        }
    }

    /* Pack reliable map events from dedicated per-client queue */
    mapEventBaseSeq = mapQ->ackedSeq;
    {
        uint32_t seq;
        for (seq = mapQ->ackedSeq; seq < mapQ->nextSeq; seq++) {
            uint32_t idx = seq % RELIABLE_EVENT_BUFFER_SIZE;
            if (pos + GAME_EVENT_MAX_WIRE_SIZE > (int)sizeof(buf)) break;
            if (mapQ->buffer[idx].seq != seq) break; /* Buffer wrapped — stop */
            pos += packGameEvent(buf + pos, &mapQ->buffer[idx].event);
            mapEventCount++;
            if (mapEventCount >= 255) break; /* Cap to uint8_t max */
        }
    }

    /* Pack reliable control events from dedicated per-client queue.
     * Per-event wire layout: type(1) + bodyLen(2) + body(N). The
     * receiver dispatches each event to its decoder via the body-only
     * codec table (transportControlCodecBodyDecoder). */
    controlEventBaseSeq = controlQ->ackedSeq;
    {
        uint32_t seq;
        char typesBuf[256];
        int tbPos = 0;
        typesBuf[0] = '\0';
        for (seq = controlQ->ackedSeq; seq < controlQ->nextSeq; seq++) {
            uint32_t idx = seq % CONTROL_EVENT_QUEUE_SIZE;
            ControlEncodeBodyFn enc;
            size_t bodyLen = 0;
            if (controlQ->buffer[idx].seq != seq) break; /* wrapped — slot got reused */
            enc = transportControlCodecBodyEncoder(controlQ->buffer[idx].event.type);
            if (enc == NULL) continue; /* no body codec — silently skip */
            if (pos + 3 > (int)sizeof(buf)) break;
            if (enc(&controlQ->buffer[idx].event, client,
                    buf + pos + 3, sizeof(buf) - pos - 3, &bodyLen) != ENCODE_OK) {
                break;
            }
            buf[pos]   = (uint8_t)controlQ->buffer[idx].event.type;
            packU16(buf + pos + 1, (uint16_t)bodyLen);
            pos += 3 + (int)bodyLen;
            if (tbPos < (int)sizeof(typesBuf) - 32) {
                tbPos += snprintf(typesBuf + tbPos, sizeof(typesBuf) - tbPos,
                                  "%s%s(seq=%u)", tbPos == 0 ? "" : ",",
                                  mpDiagCtrlName((int)controlQ->buffer[idx].event.type),
                                  (unsigned)seq);
            }
            controlEventCount++;
            if (controlEventCount >= 255) break; /* Cap to uint8_t max */
        }
        if (controlEventCount > 0) {
            mpDiagLog("[srv] SNAPSHOT-tail slot=%d baseSeq=%u count=%u (ackedSeq=%u nextSeq=%u) types=[%s]",
                      clientIdx, (unsigned)controlEventBaseSeq,
                      controlEventCount,
                      (unsigned)controlQ->ackedSeq, (unsigned)controlQ->nextSeq,
                      typesBuf);
        }
    }

    /* Fill in counts */
    buf[countsPos]     = hdr.tankCount;
    buf[countsPos + 1] = hdr.shellCount;
    buf[countsPos + 2] = hdr.tkExplosionCount;
    buf[countsPos + 3] = hdr.baseCount;
    buf[countsPos + 4] = hdr.pillCount;
    buf[countsPos + 5] = (uint8_t)reliableEventCount;
    packU32(buf + countsPos + 6, reliableBaseSeq);
    buf[countsPos + 10] = (uint8_t)mapEventCount;
    packU32(buf + countsPos + 11, mapEventBaseSeq);
    buf[countsPos + 15] = (uint8_t)controlEventCount;
    packU32(buf + countsPos + 16, controlEventBaseSeq);
    packU16(buf + countsPos + 20, hdr.mapChecksum);
    packU16(buf + countsPos + 22, hdr.returnToLobbyTicks);

    /* wire-only: per-tick snapshot — high-volume delta-encoded path with its own reliability discipline */
    udpSendTo(udpServer.sock, buf, pos, &client->addr);
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
        mpDiagLog("[srv] DISCONNECT slot=%d name='%s' graceful=%d tickDiff=%u (timeout=%d) control(ack=%u next=%u)",
                  idx, c->playerName, (int)graceful,
                  (unsigned)(udpServer.tickCount - c->lastReceivedTick),
                  (int)CLIENT_TIMEOUT_TICKS,
                  (unsigned)udpServer.controlEventQueues[idx].ackedSeq,
                  (unsigned)udpServer.controlEventQueues[idx].nextSeq);
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
    udpServer.clients[idx].inboundCmdSeq = 0;
    memset(udpServer.clients[idx].playerName, 0, PACKET_MAX_PLAYER_NAME);
    udpServer.clientLocked[idx] = false;
    /* Drop any owed PLAYER_JOIN — the player left before it resolved, so
     * no orphan anonymous join (and no leave it would need to pair with). */
    wbnJoinClear(&udpServer.clients[idx].wbnJoin);

    /* Reset the control event queue so a re-using slot starts fresh. */
    udpServer.controlEventQueues[idx].nextSeq = 1;
    udpServer.controlEventQueues[idx].ackedSeq = 1;
    memset(udpServer.controlEventQueues[idx].buffer, 0,
           sizeof(udpServer.controlEventQueues[idx].buffer));
    udpServer.controlSyncInProgress[idx] = false;
    udpServer.controlEventLastAckProgressTick[idx] = udpServer.tickCount;
    controlEventQueueAssertValid(&udpServer.controlEventQueues[idx], "disconnect-reset");

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
        /* Skip if the slot was reused by a fresh join since the overflow —
         * serverDisconnectClient cleared connected; a reconnect sets it
         * again, and we must not tear the new player down. */
        if (udpServer.clients[i].connected) continue;
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
                sendto(udpServer.sock, (const char *)kbuf, sizeof(kbuf), 0,
                       (const struct sockaddr *)&udpServer.clients[i].addr,
                       sizeof(udpServer.clients[i].addr));
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
        udpServer.clients[i].inboundCmdSeq = 0;
        udpServer.clients[i].controlSub = SUBSCRIBER_HANDLE_INVALID;
        memset(&udpServer.mapDownload[i], 0, sizeof(ClientMapDownload));
        udpServer.controlEventQueues[i].nextSeq = 1;
        udpServer.controlEventQueues[i].ackedSeq = 1;
        udpServer.controlSyncInProgress[i] = false;
        controlEventQueueAssertValid(&udpServer.controlEventQueues[i], "server-boot");
    }

    /* Start dedicated recv thread (skip under simulated latency) */
#if SIM_LATENCY_SERVER_MS > 0 && !defined(HAVE_SCREEN_C)
    recvThread = NULL;
#else
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
#endif

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
            serverCleanupMapDownload(i);
        }
    }
    udpServer.running = false;

    udpServerPublicIp[0] = '\0';
    udpServerPublicPort  = 0;
    memset(punchQueue, 0, sizeof(punchQueue));
}

/* Handle an old-protocol info request (server browser compatibility).
 * Builds a 76-byte INFO_PACKET response from the current sim state. */
static void serverHandleInfoRequest(const struct sockaddr_in *fromAddr,
                                    ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    INFO_PACKET pkt;
    int i;
    BYTE numPlayers = 0;
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
    pkt.spare1 = 0;
    pkt.start_delay = serverSimGetStartDelay(sim);
    pkt.time_limit = serverSimGetGameLength(sim);

    /* Count connected players */
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(sim, i)) numPlayers++;
    }
    pkt.num_players = numPlayers;

    /* Neutral pills and bases */
    pkt.free_pills = pillsGetNumNeutral(&gs->pb);
    pkt.free_bases = basesGetNumNeutral(&gs->bs);

    pkt.has_password = serverSimGetPassword(sim)[0] != '\0' ? 1 : 0;
    pkt.spare2 = 0;

    /* wire-only: tracker / external reply (no in-process audience) */
    udpSendTo(udpServer.sock, (uint8_t *)&pkt, sizeof(pkt), fromAddr);

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
    mpDiagLog("[srv] GAME_START wipe BEGIN (about to reset all queues + set needsPlayerList)");
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            mpDiagLog("[srv] GAME_START wipe slot=%d pre control(ack=%u next=%u) ev(ack=%u next=%u) mapEv(ack=%u next=%u)",
                      i,
                      (unsigned)udpServer.controlEventQueues[i].ackedSeq,
                      (unsigned)udpServer.controlEventQueues[i].nextSeq,
                      (unsigned)udpServer.eventQueues[i].ackedSeq,
                      (unsigned)udpServer.eventQueues[i].nextSeq,
                      (unsigned)udpServer.mapEventQueues[i].ackedSeq,
                      (unsigned)udpServer.mapEventQueues[i].nextSeq);
        }
        if (udpServer.clients[i].connected) {
            /* The controlEventQueues memset below destroys any un-ACKed
               CTRL_PLAYER_JOIN still in flight from a late-countdown
               joiner.  Flag this client for an unsolicited PLAYER_LIST
               resync so its roster catches up after the reset; the new
               game's first control event will be CTRL_GAME_PHASE_RUNNING
               at seq=1, with no retransmit path back to the dropped
               JOIN events. */
            udpServer.clients[i].needsPlayerList = true;
            /* Ensure map download is considered complete so snapshots
             * are sent during the game even if a final chunk ack was lost. */
            udpServer.mapDownload[i].downloadComplete = TRUE;
        }
        /* Reset reliable event queues — stale events from the previous game
         * must not be resent after clients load the fresh map.  Reset all
         * three queues here, BEFORE the caller publishes
         * CTRL_GAME_PHASE_RUNNING, so that event enters every slot's
         * control queue at seq 1 as the first event of the new game. */
        udpServer.eventQueues[i].nextSeq = 1;
        udpServer.eventQueues[i].ackedSeq = 1;
        memset(udpServer.eventQueues[i].buffer, 0,
               sizeof(udpServer.eventQueues[i].buffer));
        udpServer.mapEventQueues[i].nextSeq = 1;
        udpServer.mapEventQueues[i].ackedSeq = 1;
        memset(udpServer.mapEventQueues[i].buffer, 0,
               sizeof(udpServer.mapEventQueues[i].buffer));
        udpServer.controlEventQueues[i].nextSeq = 1;
        udpServer.controlEventQueues[i].ackedSeq = 1;
        memset(udpServer.controlEventQueues[i].buffer, 0,
               sizeof(udpServer.controlEventQueues[i].buffer));
        /* C3: also restart the unacked-control timer baseline.  The
         * enqueue-into-empty fix at the deliver site catches this
         * transitively when the next event lands, but resetting here
         * makes the contract explicit and removes the brief window
         * where the stale baseline is still observable. */
        udpServer.controlEventLastAckProgressTick[i] = udpServer.tickCount;
        controlEventQueueAssertValid(&udpServer.controlEventQueues[i], "game-start-wipe");
    }
    WB_LOG_INFO(WB_LOG_CAT_NET, "ctrl queue reset all slots (game start)");
    mpDiagLog("[srv] GAME_START wipe END (all connected slots flagged needsPlayerList)");
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
        udpSendTo(udpServer.sock, notifyBuf, sizeof(notifyBuf),
                  &udpServer.clients[i].addr);
        /* Re-send JOIN_ACCEPT so the client picks up the new compressed
         * map size. */
        serverSendJoinAccept(i, sim, &udpServer.clients[i].addr);
        /* Reset chunk tracking; subsequent ticks resume sending chunks. */
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

            if (holder < 0) {
                /* Bare name free — the squatter left during grace.  Promote
                 * straight to the bare name. */
                serverSimSetPlayerName(sim, slot, desired);
                serverSimPublishLobbySlot(sim, slot);
                snprintf(udpServer.clients[slot].playerName,
                         PACKET_MAX_PLAYER_NAME, "%s", desired);
                udpServer.clients[slot].nameStickySuffix = false;
            } else if ((playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs,
                                              (BYTE)holder)
                        & PLAYER_FLAG_WBN_VERIFIED) == 0) {
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
            }
            /* else: a verified slot won the bare name (a second reclaimer won
             * the race); keep this slot on its temp name permanently.  WBN
             * attribution is already correct since verify ran under the bare
             * name; only the local display stays suffixed. */

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

/* Send an INFO_RESPONSE packet to the tracker so the game is listed. */
void transportUdpServerSendTrackerUpdate(ServerSim *sim,
                                         const char *trackerAddr,
                                         unsigned short trackerPort) {
    GameSim *gs = serverSimGetGameSim(sim);
    INFO_PACKET pkt;
    struct sockaddr_in dest;
    struct hostent *he;
    int i;
    BYTE numPlayers = 0;

    he = gethostbyname(trackerAddr);
    if (he == NULL) {
        fprintf(stderr, "[TRACKER] Failed to resolve %s\n", trackerAddr);
        return;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    memcpy(&dest.sin_addr, he->h_addr_list[0], he->h_length);
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
    pkt.spare1 = 0;
    pkt.start_delay = serverSimGetStartDelay(sim);
    pkt.time_limit = serverSimGetGameLength(sim);

    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(sim, i)) numPlayers++;
    }
    pkt.num_players = numPlayers;
    pkt.free_pills = pillsGetNumNeutral(&gs->pb);
    pkt.free_bases = basesGetNumNeutral(&gs->bs);
    pkt.has_password = serverSimGetPassword(sim)[0] != '\0' ? 1 : 0;
    pkt.spare2 = 0;

    sendto(udpServer.sock, (const char *)&pkt, sizeof(pkt), 0,
           (const struct sockaddr *)&dest, sizeof(dest));
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
    struct hostent *he;
    uint8_t buf[8];

    if (udpServer.sock == INVALID_SOCKET) return;
    if (trackerAddr == NULL || trackerAddr[0] == '\0') return;

    he = gethostbyname(trackerAddr);
    if (he == NULL) return;

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
    memcpy(&dest.sin_addr, he->h_addr_list[0], he->h_length);
    dest.sin_port = htons(trackerPort);

    sendto(udpServer.sock, (const char *)buf, sizeof(buf), 0,
           (const struct sockaddr *)&dest, sizeof(dest));
}

void transportUdpServerSendPunchProbe(const char *trackerAddr,
                                      unsigned short trackerPort) {
    struct sockaddr_in dest;
    struct hostent *he;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (udpServer.sock == INVALID_SOCKET) return;
    if (trackerAddr == NULL || trackerAddr[0] == '\0') return;

    he = gethostbyname(trackerAddr);
    if (he == NULL) return;

    packHeader(buf, PACKET_PUNCH_PROBE_REQUEST, 0);

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    memcpy(&dest.sin_addr, he->h_addr_list[0], he->h_length);
    dest.sin_port = htons(trackerPort);

    sendto(udpServer.sock, (const char *)buf, sizeof(buf), 0,
           (const struct sockaddr *)&dest, sizeof(dest));
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
        sendto(udpServer.sock, (const char *)&sentinel, 1, 0,
               (const struct sockaddr *)&e->addr, sizeof(e->addr));
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
            break;
        case PACKET_CONTROL_ACK:
            serverHandleControlAck(buf, len, fromAddr);
            break;
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
            sendto(udpServer.sock, (const char *)ackBuf, sizeof(ackBuf), 0,
                   (struct sockaddr *)&client->addr, sizeof(client->addr));
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
        case PACKET_MAP_ACK: {
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 2) {
                uint16_t chunkIdx = unpackU16(buf + PACKET_HEADER_SIZE);
                ClientMapDownload *dl = &udpServer.mapDownload[clientIdx];

                udpServer.clients[clientIdx].lastReceivedTick = udpServer.tickCount;

                if (chunkIdx == 0xFFFF) {
                    fprintf(stderr, "[UDP SERVER] Client %d ready for map\n", clientIdx);
                    if (!dl->downloadComplete) {
                        serverSendMapChunks(clientIdx);
                    }
                } else if (chunkIdx < dl->totalChunks && !dl->downloadComplete) {
                    if (!dl->chunkAcked[chunkIdx]) {
                        dl->chunkAcked[chunkIdx] = TRUE;
                        dl->chunksAcked++;
                        fprintf(stderr, "[UDP SERVER] Client %d acked chunk %u/%u\n",
                                clientIdx, dl->chunksAcked, dl->totalChunks);
                    }
                    if (dl->chunksAcked >= dl->totalChunks) {
                        dl->downloadComplete = TRUE;
                        fprintf(stderr, "[UDP SERVER] Client %d map download complete\n", clientIdx);
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
                udpSendTo(udpServer.sock, rsp, rpos, fromAddr);
            } while (i < got);
            break;
        }
        case PACKET_LOBBY_MAP_USE_LOCAL: {
            /* [header 8] [totalLen 4] [nameLen 1] [name N]
             *           [relPathLen 1] [relPath M] [md5 16]
             *
             * Pre-upload optimisation: if our local data/maps/<relPath>
             * matches the supplied MD5, install it directly and reply
             * UPLOAD_DONE — no byte transfer needed. On any miss
             * (permission, path/name unsafe, file missing, MD5
             * mismatch) reply MAP_USE_LOCAL_NACK and let the client
             * fall back to the regular UPLOAD_BEGIN/CHUNK flow. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 4 + 1 + 1 + 16) break;
            int rpos = PACKET_HEADER_SIZE;
            uint32_t totalLen =
                ((uint32_t)buf[rpos + 0] << 24) |
                ((uint32_t)buf[rpos + 1] << 16) |
                ((uint32_t)buf[rpos + 2] <<  8) |
                ((uint32_t)buf[rpos + 3]);
            rpos += 4;
            uint8_t nameLen = buf[rpos++];
            if (nameLen == 0 || nameLen > 127 ||
                rpos + nameLen + 1 + 16 > (int)len) break;
            char nameBuf[128];
            memset(nameBuf, 0, sizeof(nameBuf));
            memcpy(nameBuf, buf + rpos, nameLen);
            rpos += nameLen;
            uint8_t relLen = buf[rpos++];
            if (relLen == 0 || relLen > 255 ||
                rpos + relLen + 16 > (int)len) break;
            char relBuf[256];
            memset(relBuf, 0, sizeof(relBuf));
            memcpy(relBuf, buf + rpos, relLen);
            rpos += relLen;
            uint8_t wantMd5[16];
            memcpy(wantMd5, buf + rpos, 16);

            /* NACK helper for every miss path: server echoes the
             * announce name so the client correlates the reply to
             * the right in-flight USE_LOCAL. */
            #define SEND_USE_LOCAL_NACK() do { \
                uint8_t nack[PACKET_HEADER_SIZE + 1 + 128]; \
                int npos = PACKET_HEADER_SIZE; \
                packHeader(nack, PACKET_LOBBY_MAP_USE_LOCAL_NACK, 0); \
                nack[npos++] = (uint8_t)nameLen; \
                if (nameLen > 0) { memcpy(nack + npos, nameBuf, nameLen); npos += nameLen; } \
                udpSendTo(udpServer.sock, nack, npos, fromAddr); \
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
            if (memcmp(haveMd5, wantMd5, 16) != 0) {
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
            udpSendTo(udpServer.sock, done, dpos, fromAddr);
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
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
                break;
            }
            udpServer.clientReqCooldownTicks[clientIdx] = LOBBY_REQ_COOLDOWN_TICKS;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_NOT_HOST;
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
                break;
            }
            if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_LOCKED;
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
                break;
            }
            if (udpServer.uploadPolicy == UPLOAD_POLICY_OFF) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_UPLOAD_DISABLED;
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
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
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
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
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
                break;
            }
            char nameBuf[128];
            memset(nameBuf, 0, sizeof(nameBuf));
            memcpy(nameBuf, buf + PACKET_HEADER_SIZE + 5, nameLen);

            if (!uploadFilenameIsSafe(nameBuf, nameLen)) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_INVALID;
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
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
                    udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
                    break;
                }
            }

            udpServer.clientUploadActive[clientIdx] = true;
            udpServer.clientUploadTotal[clientIdx]  = totalLen;
            udpServer.clientUploadHave[clientIdx]   = 0;
            SDL_strlcpy(udpServer.clientUploadName[clientIdx], nameBuf,
                        sizeof(udpServer.clientUploadName[clientIdx]));

            uint8_t ack[PACKET_HEADER_SIZE + 1];
            packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
            ack[PACKET_HEADER_SIZE] = 0;
            udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
            break;
        }
        case PACKET_LOBBY_MAP_UPLOAD_CHUNK: {
            /* [header 8] [offset 4] [dataLen 2] [data N]. Server
             * accumulates into the per-client buffer and on completion
             * hands the bytes to the sim, then replies with
             * MAP_UPLOAD_DONE. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 ||
                !udpServer.clientUploadActive[clientIdx]) break;
            if (len < PACKET_HEADER_SIZE + 6) break;
            uint32_t offset =
                ((uint32_t)buf[PACKET_HEADER_SIZE + 0] << 24) |
                ((uint32_t)buf[PACKET_HEADER_SIZE + 1] << 16) |
                ((uint32_t)buf[PACKET_HEADER_SIZE + 2] <<  8) |
                ((uint32_t)buf[PACKET_HEADER_SIZE + 3]);
            uint16_t dataLen =
                ((uint16_t)buf[PACKET_HEADER_SIZE + 4] << 8) |
                ((uint16_t)buf[PACKET_HEADER_SIZE + 5]);
            uint32_t total = udpServer.clientUploadTotal[clientIdx];
            if (dataLen == 0 || dataLen > 1024 ||
                offset + dataLen > total ||
                len < PACKET_HEADER_SIZE + 6 + dataLen) break;
            memcpy(udpServer.clientUploadBuf[clientIdx] + offset,
                   buf + PACKET_HEADER_SIZE + 6, dataLen);
            if (offset + dataLen > udpServer.clientUploadHave[clientIdx]) {
                udpServer.clientUploadHave[clientIdx] = offset + dataLen;
            }

            if (udpServer.clientUploadHave[clientIdx] == total) {
                const char *origName = udpServer.clientUploadName[clientIdx];

                char displayName[MAP_STR_SIZE];
                SDL_strlcpy(displayName, origName, sizeof(displayName));
                {
                    size_t dlen = SDL_strlen(displayName);
                    if (dlen >= 4 &&
                        SDL_strcasecmp(displayName + dlen - 4,
                                       ".map") == 0) {
                        displayName[dlen - 4] = '\0';
                    }
                }

                bool previewed = serverSimReloadCompressedInMemory(
                    sim,
                    udpServer.clientUploadBuf[clientIdx],
                    (int)total,
                    displayName);

                if (previewed && udpServer.uploadPolicy == UPLOAD_POLICY_PERSIST) {
                    memcpy(udpServer.pendingPersistBytes,
                           udpServer.clientUploadBuf[clientIdx], total);
                    udpServer.pendingPersistLen = total;
                    SDL_strlcpy(udpServer.pendingPersistName, displayName,
                                sizeof(udpServer.pendingPersistName));
                    udpServer.pendingPersistActive = true;
                }

                udpServer.clientUploadActive[clientIdx] = false;
                udpServer.clientUploadHave[clientIdx]   = 0;
                udpServer.clientUploadTotal[clientIdx]  = 0;

                char relReturn[256];
                SDL_snprintf(relReturn, sizeof(relReturn), "Uploads/%s",
                             origName);
                int relLen = (int)SDL_strlen(relReturn);
                if (relLen > 255) relLen = 255;
                uint8_t done[PACKET_HEADER_SIZE + 2 + 256];
                int dpos = PACKET_HEADER_SIZE;
                packHeader(done, PACKET_LOBBY_MAP_UPLOAD_DONE, 0);
                done[dpos++] = previewed ? 0 : LOBBY_REJECT_INVALID;
                done[dpos++] = (uint8_t)relLen;
                memcpy(done + dpos, relReturn, relLen);
                dpos += relLen;
                udpSendTo(udpServer.sock, done, dpos, fromAddr);
            }
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
                udpSendTo(udpServer.sock, rsp, wpos, fromAddr);
            } while (i < got);
            break;
        }
        case PACKET_LOBBY_MAP_PREVIEW_REQ: {
            /* [header 8] [pathLen 1] [path N]. Reads data/maps/<path>
             * from the server's filesystem and streams the bytes back
             * in chunks. Client rasterises locally — server has no
             * dep on a renderer or image encoder, and the protocol
             * is the same shape in SP-host (loopback) and MP. */
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
                udpSendTo(udpServer.sock, err, wpos, fromAddr);
                break;
            }

            /* Monotonic per-(client) sequence so the receiver can
             * tell stale chunks from a prior request for the same
             * path apart from current ones. udpServer is a single
             * global so a process-wide counter is fine; collisions
             * across long sessions wrap around harmlessly. */
            static uint8_t s_previewSeq = 0;
            uint8_t seq = ++s_previewSeq;

            /* BEGIN announces the total transfer size + the seq
             * id. Sent first; chunks reference seq + offset. */
            {
                uint8_t hdr[PACKET_HEADER_SIZE + 1 + 256 + 1 + 4];
                packHeader(hdr, PACKET_LOBBY_MAP_PREVIEW_BEGIN, 0);
                int wpos = PACKET_HEADER_SIZE;
                hdr[wpos++] = pathLen;
                memcpy(hdr + wpos, relPath, pathLen);
                wpos += pathLen;
                hdr[wpos++] = seq;
                uint32_t total = (uint32_t)mapLen;
                hdr[wpos++] = (uint8_t)((total >> 24) & 0xFF);
                hdr[wpos++] = (uint8_t)((total >> 16) & 0xFF);
                hdr[wpos++] = (uint8_t)((total >>  8) & 0xFF);
                hdr[wpos++] = (uint8_t)( total        & 0xFF);
                udpSendTo(udpServer.sock, hdr, wpos, fromAddr);
            }

            /* Stream chunks. ~1200 bytes per chunk keeps each UDP
             * datagram comfortably under the typical 1400-byte
             * Ethernet MTU after header / IP / UDP overhead. */
            const size_t kChunkBytes = 1200;
            uint8_t chunk[PACKET_HEADER_SIZE + 1 + 4 + 2 + 1200];
            for (size_t off = 0; off < mapLen; off += kChunkBytes) {
                size_t n = mapLen - off;
                if (n > kChunkBytes) n = kChunkBytes;
                packHeader(chunk, PACKET_LOBBY_MAP_PREVIEW_CHUNK, 0);
                int wpos = PACKET_HEADER_SIZE;
                chunk[wpos++] = seq;
                uint32_t o = (uint32_t)off;
                chunk[wpos++] = (uint8_t)((o >> 24) & 0xFF);
                chunk[wpos++] = (uint8_t)((o >> 16) & 0xFF);
                chunk[wpos++] = (uint8_t)((o >>  8) & 0xFF);
                chunk[wpos++] = (uint8_t)( o        & 0xFF);
                chunk[wpos++] = (uint8_t)((n >>  8) & 0xFF);
                chunk[wpos++] = (uint8_t)( n        & 0xFF);
                memcpy(chunk + wpos, mapBytes + off, n);
                wpos += (int)n;
                udpSendTo(udpServer.sock, chunk, wpos, fromAddr);
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

#if SIM_LATENCY_SERVER_MS > 0 && !defined(HAVE_SCREEN_C)
    while ((len = udpRecvFromDelayed(udpServer.sock, buf, sizeof(buf), &fromAddr,
            SIM_LATENCY_SERVER_MS, serverDelayQueue,
            &serverDelayHead, &serverDelayCount)) > 0) {
#else
    while ((len = udpRecvFrom(udpServer.sock, buf, sizeof(buf), &fromAddr)) > 0) {
#endif
        serverProcessPacket(sim, buf, len, &fromAddr);
    }
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
        serverProcessPacket(sim, entry->data, entry->len, &entry->fromAddr);
        tail = (tail + 1) % RECV_QUEUE_SIZE;
        SDL_SetAtomicInt(&recvQueueTail, tail);
        /* Re-read head in case more packets arrived during processing */
        head = SDL_GetAtomicInt(&recvQueueHead);
    }
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
    if (serverSimGetEventCount(sim) == 0 && serverSimGetMapEventCount(sim) == 0) return;

    for (c = 0; c < MAX_TANKS; c++) {
        ClientEventQueue *cq;
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
                    fprintf(stderr, "[UDP SERVER] Map event queue full for client %d, dropping %d events\n",
                            c, (int)serverSimGetMapEventCount(sim) - i);
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
        cq = &udpServer.eventQueues[c];

        hasPos = serverSimGetTankState(sim, (BYTE)c, &cwx, &cwy);
        if (hasPos) {
            clientMX = (BYTE)(cwx >> 8);
            clientMY = (BYTE)(cwy >> 8);
        }

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
                } else if (dx >= SDIST_NONE || dy >= SDIST_NONE) {
                    /* Cull beyond audible range */
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

        /* Pass 2: enqueue non-sound game events into game queue,
         * then deduplicated sounds into game queue */
        for (i = 0; i < (int)serverSimGetEventCount(sim); i++) {
            uint8_t evType = serverSimGetEvents(sim)[i].type;
            if (evType != EVENT_SOUND && evType != EVENT_SOUND_TANK_HIT && evType != EVENT_SOUND_SHOOT) {
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
                    int dx = (clientMX > serverSimGetEvents(sim)[i].data[0]) ? (clientMX - serverSimGetEvents(sim)[i].data[0]) : (serverSimGetEvents(sim)[i].data[0] - clientMX);
                    int dy = (clientMY > serverSimGetEvents(sim)[i].data[1]) ? (clientMY - serverSimGetEvents(sim)[i].data[1]) : (serverSimGetEvents(sim)[i].data[1] - clientMY);
                    if (dx >= SDIST_NONE || dy >= SDIST_NONE) continue;
                }
                if (!eventQueueHasSpace(cq)) {
                    fprintf(stderr, "[UDP SERVER] Game event queue full for client %d\n", c);
                    break;
                }
                uint32_t idx = cq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                cq->buffer[idx].event = serverSimGetEvents(sim)[i];
                cq->buffer[idx].seq = cq->nextSeq;
                cq->nextSeq++;
            }
        }
        for (s = 0; s < MAX_SOUND_TYPES; s++) {
            if (bestSoundIdx[s] >= 0) {
                if (!eventQueueHasSpace(cq)) break;
                uint32_t idx = cq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                cq->buffer[idx].event = serverSimGetEvents(sim)[bestSoundIdx[s]];
                cq->buffer[idx].seq = cq->nextSeq;
                cq->nextSeq++;
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
        if (!udpServer.mapDownload[i].downloadComplete) {
            /* Still downloading map — resend un-acked chunks every 25 ticks (0.5s) */
            if (udpServer.tickCount - udpServer.mapDownload[i].lastSendTick >= 25) {
                serverSendMapChunks(i);
            }
            continue;
        }

        /* Send existing player list before the first snapshot.
         * This must happen after map download so the client's players
         * struct (recreated during map load) is ready.
         * Format: [header][count]
         *   [playerNum 1][name 32][cc 2][clientType 1][clientFlags 1]
         *   [numAllies 1][ally0 1][ally1 1]...
         * Each player entry is variable-length due to allies. */
        if (udpServer.clients[i].needsPlayerList) {
            uint8_t plBuf[UDP_MAX_PAYLOAD];
            int plPos = PACKET_HEADER_SIZE;
            uint8_t plCount = 0;
            int j;

            plPos++; /* reserve byte for count */
            for (j = 0; j < MAX_TANKS; j++) {
                BYTE allies[MAX_TANKS];
                BYTE numAllies;
                char playerName[PACKET_MAX_PLAYER_NAME];
                if (j == i) continue;
                /* Include both UDP clients and bot players (sim-connected but no UDP client) */
                if (!udpServer.clients[j].connected && !serverSimIsPlayerConnected(sim, j)) continue;

                numAllies = playersMakeNetAlliences(
                    &serverSimGetGameSim(sim)->plyrs, (BYTE)j, allies);

                /* Check we have room: 1 + 32 + 2 + 2 + 1 + numAllies */
                if (plPos + 1 + PACKET_MAX_PLAYER_NAME + 2 + 2 + 1 + numAllies
                    > (int)sizeof(plBuf))
                    break;

                plBuf[plPos++] = (uint8_t)j;
                memset(plBuf + plPos, 0, PACKET_MAX_PLAYER_NAME);
                /* Get name from players struct (works for both UDP clients and bots) */
                memset(playerName, 0, sizeof(playerName));
                playersGetPlayerName(&serverSimGetGameSim(sim)->plyrs, (BYTE)j, playerName, TRUE);
                snprintf((char *)(plBuf + plPos), PACKET_MAX_PLAYER_NAME, "%s", playerName);
                plPos += PACKET_MAX_PLAYER_NAME;
                /* Country code (2 bytes) */
                plBuf[plPos++] = (uint8_t)udpServer.clients[j].countryCode[0];
                plBuf[plPos++] = (uint8_t)udpServer.clients[j].countryCode[1];
                plBuf[plPos++] = playersGetClientType(&serverSimGetGameSim(sim)->plyrs, (BYTE)j);
                plBuf[plPos++] = playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, (BYTE)j);
                plBuf[plPos++] = numAllies;
                if (numAllies > 0) {
                    memcpy(plBuf + plPos, allies, numAllies);
                    plPos += numAllies;
                }
                plCount++;
            }
            packHeader(plBuf, PACKET_PLAYER_LIST, 0);
            plBuf[PACKET_HEADER_SIZE] = plCount;
            mpDiagLog("[srv] PLAYER_LIST send slot=%d count=%u bytes=%d",
                      i, (unsigned)plCount, plPos);
            if (plCount > 0) {
                /* wire-only: per-client handshake (response to a single client's request) */
                udpSendTo(udpServer.sock, plBuf, plPos,
                          &udpServer.clients[i].addr);
            }
            udpServer.clients[i].needsPlayerList = false;
        }

        serverSendSnapshot(sim, i);
    }

    transportUdpServerCheckTimeouts(sim);
}

/* Second per-client timeout: covers the failure mode where the client is
 * reachable (sending pings / inputs, so lastReceivedTick keeps advancing)
 * but is not ACKing control events.  Well above the worst-case retransmit
 * budget and well below CLIENT_TIMEOUT_TICKS (1000). */
#define CONTROL_UNACKED_TIMEOUT_TICKS 500   /* ~10 s @ 50 Hz */

/* Check for client timeouts — call from any server state (lobby, running, etc.) */
void transportUdpServerCheckTimeouts(ServerSim *sim) {
    int i;

    if (!udpServer.running) return;

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;

        /* Anonymous-fallback for a deferred WBN PLAYER_JOIN: the joiner's
         * reauth never landed within the grace window (direct-IP, not
         * signed in, or WBN unreachable), so announce the join un-keyed. */
        if (wbnJoinOnTick(&udpServer.clients[i].wbnJoin, udpServer.tickCount)) {
            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                               (BYTE)i, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
        }

        /* Unacked-control disconnect — fires when the queue has events
         * in flight (ackedSeq < nextSeq) and the ack hasn't advanced for
         * CONTROL_UNACKED_TIMEOUT_TICKS.  Independent of the no-traffic
         * timeout below, which only watches lastReceivedTick. */
        if (udpServer.controlEventQueues[i].ackedSeq <
                udpServer.controlEventQueues[i].nextSeq &&
            (udpServer.tickCount -
             udpServer.controlEventLastAckProgressTick[i]) >
                CONTROL_UNACKED_TIMEOUT_TICKS) {
            WB_LOG_ERROR(WB_LOG_CAT_NET,
                "control queue stuck unacked for slot %d "
                "(%u ticks since last progress), disconnecting",
                i,
                (unsigned)(udpServer.tickCount -
                           udpServer.controlEventLastAckProgressTick[i]));
            mpDiagLog("[srv] TIMEOUT(unacked-control) slot=%d ticksSinceProgress=%u ackedSeq=%u nextSeq=%u -> disconnect",
                      i,
                      (unsigned)(udpServer.tickCount -
                                 udpServer.controlEventLastAckProgressTick[i]),
                      (unsigned)udpServer.controlEventQueues[i].ackedSeq,
                      (unsigned)udpServer.controlEventQueues[i].nextSeq);
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            if (serverSimIsLobbyEnabled(sim) &&
                (serverSimGetState(sim) == serverStateLobby ||
                 serverSimGetState(sim) == serverStateCountdown)) {
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
            continue;
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
}


int transportUdpServerGetClientCount(void) {
    int count = 0;
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) count++;
    }
    return count;
}

uint16_t transportUdpServerGetClientPing(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) return 0;
    if (!udpServer.clients[playerNum].connected) return 0;
    return udpServer.clients[playerNum].pingMs;
}

void transportUdpServerSetLock(ServerSim *sim, bool locked) {
    (void)sim;
    if (udpServer.gameLocked == locked) return;
    udpServer.gameLocked = locked;
    serverSimConsoleMessage(locked
        ? "This game is now locked to new players (server lock)"
        : "This game is now unlocked to new players (server unlock)");
    winboloNetSendLock(locked);
    /* Enqueue directly into per-client reliable queues.
     * We can't use serverSimAddEvent() because the sim's event buffer
     * gets cleared at the start of each tick — this runs from the
     * console thread between ticks so the event would be lost. */
    {
        GameEvent ev;
        int c;
        ev.type = EVENT_SERVER_MSG;
        memset(ev.data, 0, sizeof(ev.data));
        ev.data[0] = locked ? SERVER_MSG_GAME_LOCKED : SERVER_MSG_GAME_UNLOCKED;
        for (c = 0; c < MAX_TANKS; c++) {
            ClientEventQueue *q;
            uint32_t idx;
            if (!udpServer.clients[c].connected) continue;
            if (!udpServer.mapDownload[c].downloadComplete) continue;
            q = &udpServer.eventQueues[c];
            if (!eventQueueHasSpace(q)) continue;
            idx = q->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
            q->buffer[idx].event = ev;
            q->buffer[idx].seq = q->nextSeq;
            q->nextSeq++;
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

