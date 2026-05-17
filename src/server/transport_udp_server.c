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

#include <assert.h>

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
#include "server_lifecycle.h"
#include "control_event.h"
#include "mapgen.h"
#include "transport_control_codec.h"
#include "../winbolonet/winbolonet.h"
#include "threads.h"
#include "sounddist.h"
#include "bot_manager.h"
#include "log.h"
#include "playername_validate.h"
#include "../common/wb_log.h"

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

/* Server-side global state */
static struct {
    SOCKET sock;
    bool running;
    char password[MAP_STR_SIZE];
    BYTE maxPlayers;
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

    /* Game lock — prevents new players from joining */
    bool gameLocked;           /* Server admin lock */
    bool clientLocked[MAX_TANKS]; /* Per-player lock votes */

    /* Per-client map upload state. clientUploadActive=true between
     * PACKET_LOBBY_MAP_UPLOAD_BEGIN and the final write-out at
     * MAP_UPLOAD_DONE. clientUploadBuf grows up to UPLOAD_MAX_BYTES;
     * clientUploadHave tracks the highest contiguous byte received. */
    bool     clientUploadActive[MAX_TANKS];
    uint32_t clientUploadTotal[MAX_TANKS];
    uint32_t clientUploadHave[MAX_TANKS];
    uint8_t *clientUploadBuf[MAX_TANKS];
    char     clientUploadName[MAX_TANKS][128];
} udpServer;

#define UPLOAD_MAX_BYTES (1u * 1024u * 1024u)

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
} BalanceThreadData;

/* Background thread: calls WBN balance API (blocks on HTTP) then writes
 * results back under the game mutex so the timer can broadcast them. */
static int balanceThreadFunc(void *data) {
    BalanceThreadData *btd = (BalanceThreadData *)data;
    ServerSim *sim = btd->sim;

    /* This blocks on HTTP — runs outside the game mutex */
    serverSimRequestBalanceProposal(sim, btd->totalPlayers, btd->teamSize);

    free(btd);

    /* If the server is shutting down, signal completion and exit without
     * acquiring the mutex (the main thread may have already torn it down). */
    if (serverSimBalanceShutdownRequested(sim)) {
        serverSimSetBalanceRequestInFlight(sim, false);
        return 0;
    }

    /* Write results back under the game mutex */
    threadsWaitForMutex();
    serverSimSetBalanceRequestInFlight(sim, false);
    if (serverSimGetBalanceProposal(sim)->pending) {
        serverSimSetBalanceBroadcastNeeded(sim, true);
    }
    threadsReleaseMutex();
    return 0;
}

/* Find a free player slot. Returns index or -1. */
static int serverFindFreeSlot(void) {
    int i;
    BYTE limit = (udpServer.maxPlayers > 0) ? udpServer.maxPlayers : MAX_TANKS;
    for (i = 0; i < limit; i++) {
        if (!udpServer.clients[i].connected && !botManagerIsBot((BYTE)i)) {
            return i;
        }
    }
    return -1;
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

/* Send a PACKET_NAME_CHANGE_REJECT to a specific connected client.
 * Payload is a single reasonCode byte (NAME_REJECT_*). The client maps
 * it to a localized langid and surfaces it in the chat ring. */
static void serverSendNameChangeReject(int clientIdx, uint8_t reasonCode) {
    uint8_t buf[PACKET_HEADER_SIZE + 1];
    if (clientIdx < 0 || clientIdx >= MAX_TANKS) return;
    if (!udpServer.clients[clientIdx].connected) return;
    packHeader(buf, PACKET_NAME_CHANGE_REJECT, 0);
    buf[PACKET_HEADER_SIZE] = reasonCode;
    /* wire-only: per-client handshake (response to a single client's request) */
    udpSendTo(udpServer.sock, buf, sizeof(buf),
              &udpServer.clients[clientIdx].addr);
}

/* Per-client subscriber deliver callback.  Runs each ControlEvent
 * through the codec table and unicasts the encoded bytes to this
 * one client.  ENCODE_SKIP is the normal "no wire form for this
 * recipient" case (also produced by every encoder while the codec
 * table is still scaffolding).  ENCODE_OVERFLOW means an encoder
 * exceeded MAX_CONTROL_PACKET — a programmer bug; surface it in
 * debug builds and silently drop in release. */
static void udpClientDeliverControl(void *ctx, const ControlEvent *evt) {
    UdpServerClient *client = (UdpServerClient *)ctx;
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t len = 0;
    ControlEncodeFn enc;
    EncodeResult r;

    if (!client->connected) return;

    /* Per-recipient filtering for single-target variants.  The codec
     * stays UdpServerClient-agnostic; the slot comparison lives here
     * where the recipient's player number is in scope. */
    if (evt->type == CTRL_ALLIANCE_REQUEST &&
        evt->u.allianceRequest.toPlayer != client->playerNum) {
        return;
    }
    if (evt->type == CTRL_CHAT) {
        BYTE from = evt->u.chat.fromPlayer;
        BYTE dest = evt->u.chat.destPlayer;
        if (dest == 0xFF) {
            /* Broadcast: skip the original sender if it's a real player. */
            if (from < MAX_TANKS && client->playerNum == from) return;
        } else {
            /* Unicast: only the addressed slot receives. */
            if (client->playerNum != dest) return;
        }
    }

    enc = transportControlCodecEncoder(evt->type);
    if (enc == NULL) return;
    r = enc(evt, client, buf, sizeof(buf), &len);
    if (r == ENCODE_SKIP) return;
    assert(r == ENCODE_OK);
    if (r != ENCODE_OK) return;
    udpSendTo(udpServer.sock, buf, (int)len, &client->addr);
}

/* Publish a single CTRL_LOBBY_SLOT — the codec encoder fans out
 * PACKET_LOBBY_UPDATE to each connected client via its subscriber. */
static void publishLobbySlot(ServerSim *sim, BYTE slot) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySlotEvent(sim, slot, &evt);
    serverSimPublishControl(sim, &evt);
}

/* Publish CTRL_LOBBY_SETTINGS + CTRL_LOBBY_SLOT for every connected
 * slot — equivalent to the old composite PACKET_LOBBY_STATE broadcast,
 * but each event flows through the per-variant codec encoder. */
static void publishLobbyStateAll(ServerSim *sim) {
    BYTE i;
    ControlEvent evt;
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(sim, i)) {
            publishLobbySlot(sim, i);
        }
    }
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    serverSimPublishControl(sim, &evt);
}

/* Per-recipient reject: sent only to the originator of a rejected
 * lobby command. Reason codes in netpacks.h (LOBBY_REJECT_*). */
static void lobbyRejectTo(struct sockaddr_in *addr, uint8_t origPacket,
                          uint8_t reasonCode) {
    uint8_t buf[PACKET_HEADER_SIZE + 2];
    packHeader(buf, PACKET_LOBBY_REJECT, 0);
    buf[PACKET_HEADER_SIZE]     = origPacket;
    buf[PACKET_HEADER_SIZE + 1] = reasonCode;
    udpSendTo(udpServer.sock, buf, sizeof(buf), addr);
}

/* Authority check used by every lobby command handler.
 * Returns TRUE if the sender at clientIdx is allowed to issue the
 * command (host, OR open-host is on and they're an active player,
 * OR they're an admin). */
static bool lobbyClientMayEdit(ServerSim *sim, int clientIdx) {
    if (clientIdx < 0 || clientIdx >= MAX_TANKS) return FALSE;
    if (clientIdx == 0) return TRUE;  /* slot 0 = host */
    if (serverSimIsPlayerConnected(sim, clientIdx) &&
        (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, (BYTE)clientIdx)
         & PLAYER_FLAG_ADMIN)) {
        return TRUE;
    }
    return serverSimGetOpenHost(sim) && serverSimIsPlayerConnected(sim, clientIdx);
}

/* Auto-unready: any meaningful lobby change clears every human's
 * ready flag and aborts an in-flight countdown. State changes are
 * written through T1 setters; the per-slot CTRL_LOBBY_SLOT publishes
 * (plus the CTRL_GAME_PHASE publish if the countdown was aborted)
 * fan out to both in-process subscribers and remote UDP clients via
 * the codec — no wire-only blast needed. Bots stay permanently ready
 * by design (set in botManagerAddBot) so the next all-ready check
 * still triggers a countdown when the human re-confirms. */
static void lobbyAutoUnreadyOnChange(ServerSim *sim) {
    BYTE i;
    bool countdownWasRunning = (serverSimGetState(sim) == serverStateCountdown);
    bool toggled[MAX_TANKS];

    for (i = 0; i < MAX_TANKS; i++) {
        const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, i);
        toggled[i] = false;
        if (lp == NULL) continue;
        if (lp->isBot) continue;
        if (lp->ready) {
            serverSimSetReady(sim, i, false);
            toggled[i] = true;
        }
    }

    if (countdownWasRunning) {
        serverSimAbortCountdown(sim);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (toggled[i]) {
            serverSimPublishLobbySlot(sim, i);
        }
    }

    if (countdownWasRunning) {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        serverSimFillGamePhaseEvent(sim, &evt);
        serverSimPublishControl(sim, &evt);
    }
}

/* Wire-only fan-out for the cosmetic ping/country refresh fired
 * every 25 ticks while in lobby/countdown.  Drives the same codec
 * encoders the bus path uses, but bypasses serverSimPublishControl
 * so in-process subscribers (bots, SP, replay-log writers) don't
 * wake up for cosmetic data they ignore.  Remote UDP clients still
 * receive the same wire packets they would have via the bus path. */
void transportUdpServerSendPeriodicLobbyRefresh(ServerSim *sim) {
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t len;
    ControlEncodeFn slotEnc = transportControlCodecEncoder(CTRL_LOBBY_SLOT);
    ControlEncodeFn settingsEnc = transportControlCodecEncoder(CTRL_LOBBY_SETTINGS);
    BYTE i;
    int j;

    if (slotEnc != NULL) {
        for (i = 0; i < MAX_TANKS; i++) {
            ControlEvent evt;
            if (!serverSimIsPlayerConnected(sim, i)) continue;
            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbySlotEvent(sim, i, &evt);
            for (j = 0; j < MAX_TANKS; j++) {
                if (!udpServer.clients[j].connected) continue;
                if (slotEnc(&evt, &udpServer.clients[j], buf, sizeof(buf), &len) == ENCODE_OK) {
                    /* wire-only: cosmetic ping/country refresh — remote audiences only */
                    udpSendTo(udpServer.sock, buf, (int)len,
                              &udpServer.clients[j].addr);
                }
            }
        }
    }

    if (settingsEnc != NULL) {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        serverSimFillLobbySettingsEvent(sim, &evt);
        for (j = 0; j < MAX_TANKS; j++) {
            if (!udpServer.clients[j].connected) continue;
            if (settingsEnc(&evt, &udpServer.clients[j], buf, sizeof(buf), &len) == ENCODE_OK) {
                /* wire-only: cosmetic ping/country refresh — remote audiences only */
                udpSendTo(udpServer.sock, buf, (int)len,
                          &udpServer.clients[j].addr);
            }
        }
    }
}

/* Build and send the join accept packet with game settings and map size */
static void serverSendJoinAccept(int slot, ServerSim *sim,
                                 const struct sockaddr_in *addr) {
    uint8_t acceptBuf[PACKET_HEADER_SIZE + 19];
    int pos;

    packHeader(acceptBuf, PACKET_JOIN_ACCEPT,
               udpServer.clients[slot].outSequence++);
    pos = PACKET_HEADER_SIZE;
    acceptBuf[pos++] = (uint8_t)slot;
    packU32(acceptBuf + pos, serverSimGetTick(sim));
    pos += 4;
    acceptBuf[pos++] = (uint8_t)gameTypeGet(&serverSimGetGameSim(sim)->game);
    acceptBuf[pos++] = serverSimGetGameSim(sim)->hiddenMines ? 1 : 0;
    packU32(acceptBuf + pos, (uint32_t)serverSimGetStartDelay(sim));
    pos += 4;
    packU32(acceptBuf + pos, (uint32_t)serverSimGetGameLength(sim));
    pos += 4;
    packU32(acceptBuf + pos, udpServer.compressedMapSize);
    pos += 4;

    /* wire-only: per-client handshake (response to a single client's request) */
    udpSendTo(udpServer.sock, acceptBuf, pos, addr);
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
    publishLobbySlot(sim, (BYTE)victimSlot);

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

/* Handle a join request from a new client */
static void serverHandleJoinRequest(const uint8_t *buf, int len,
                                    const struct sockaddr_in *fromAddr,
                                    ServerSim *sim) {
    int pos = PACKET_HEADER_SIZE;
    char name[PACKET_MAX_PLAYER_NAME];
    char pass[MAP_STR_SIZE];
    char wbnToken[WBN_TOKEN_WIRE_LEN];
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
                     + WBN_TOKEN_WIRE_LEN + 1 + 2;
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
    memset(wbnToken, 0, WBN_TOKEN_WIRE_LEN);
    if (len >= pos + WBN_TOKEN_WIRE_LEN) {
        memcpy(wbnToken, buf + pos, WBN_TOKEN_WIRE_LEN);
        wbnToken[WBN_TOKEN_WIRE_LEN - 1] = '\0';
        pos += WBN_TOKEN_WIRE_LEN;
    }

    /* Read flags byte if present (backwards compatible — older clients default to 0) */
    bool wantRejoin = false;
    if (len > pos) {
        wantRejoin = (buf[pos] & 0x01) != 0;
        pos++;
    }

    /* Read clientType + clientHints (length already gated above). */
    uint8_t clientType  = buf[pos++];
    uint8_t clientHints = buf[pos++];
    if (clientType >= CLIENT_TYPE_COUNT) clientType = CLIENT_TYPE_UNKNOWN;
    /* Drop reserved/server-only bits — clients are never trusted to set
     * WBN_VERIFIED or WBN_STEAM_LINKED. */
    clientHints &= PLAYER_CLIENT_HINT_MASK;

    /* Check password */
    if (udpServer.password[0] != '\0') {
        if (strcmp(pass, udpServer.password) != 0) {
            char consoleMsg[128];
            snprintf(consoleMsg, sizeof(consoleMsg),
                     "Join rejected for '%s': Incorrect password", name);
            serverSimConsoleMessage(consoleMsg);
            serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0, NULL);
            return;
        }
    }

    /* Check game lock (server admin OR all clients voted to lock) */
    if (udpServer.gameLocked || serverClientsAllLocked()) {
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
    slot = serverFindFreeSlot();
    if (slot < 0) {
        serverSimConsoleMessage("Join rejected: Server full");
        serverSendJoinReject(fromAddr, STR_REJECT_SERVER_FULL, 0, NULL);
        return;
    }

    /* GeoIP country lookup for the incoming player.  Done early so the
     * preempt path can include it in the rename newswire. */
    char incomingCountry[3];
    {
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &fromAddr->sin_addr, ipStr, sizeof(ipStr));
        if (!geoLookupCountry(ipStr, incomingCountry)) {
            incomingCountry[0] = '\0';
            incomingCountry[1] = '\0';
        }
        incomingCountry[2] = '\0';
    }

    /* Verify WBN token if provided.  Result drives the collision policy
     * below.  On verification failure we reject without touching the
     * slot (it's still unconnected at this point). */
    bool incomingIsWBN = false;
    bool wbnHasSteam = false;
    bool wbnIsSupporter = false;
    if (wbnToken[0] != '\0' && winbolonetIsRunning()) {
        char errorMsg[512];
        errorMsg[0] = '\0';
        if (winbolonetServerVerifyToken(wbnToken, (BYTE)slot, errorMsg,
                                        &wbnHasSteam, &wbnIsSupporter)) {
            fprintf(stderr, "[UDP SERVER] Player '%s' verified with WinBolo.net\n", name);
            incomingIsWBN = true;
        } else {
            /* Truncate the WBN reason to the per-arg wire cap so
             * packLocalizedPayload doesn't reject the packet. */
            char wbnReason[PLAYER_NAME_LEN];
            /* Pad with two empty args so the reason lands in args.string1
             * (positional mapping: arg0->playerName, arg1->otherName,
             * arg2->string1).  STR_REJECT_WBN_VERIFY_FAILED uses {string1}. */
            const char *wbnArgs[3];
            fprintf(stderr,
                    "[UDP SERVER] WinBolo.net verification failed: %s\n",
                    errorMsg);
            strncpy(wbnReason, errorMsg, sizeof(wbnReason) - 1);
            wbnReason[sizeof(wbnReason) - 1] = '\0';
            wbnArgs[0] = "";
            wbnArgs[1] = "";
            wbnArgs[2] = wbnReason;
            serverSendJoinReject(fromAddr, STR_REJECT_WBN_VERIFY_FAILED, 3,
                                 wbnArgs);
            return;
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

            if (!incomingIsWBN && !existingIsWBN) {
                /* Both unverified (incl. Steam, bot): existing behavior. */
                char consoleMsg[128];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "Join rejected for '%s': Name already in use", name);
                serverSimConsoleMessage(consoleMsg);
                serverSendJoinReject(fromAddr, STR_DLGSETNAME_INUSE_ERR, 0, NULL);
                return;
            }
            if (!incomingIsWBN && existingIsWBN) {
                /* Unverified joiner can't take a verified player's name. */
                char consoleMsg[160];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "Join rejected for '%s': name taken by verified player",
                         name);
                serverSimConsoleMessage(consoleMsg);
                serverSendJoinReject(fromAddr, STR_NAME_TAKEN_BY_VERIFIED, 0, NULL);
                return;
            }
            if (incomingIsWBN && existingIsWBN) {
                /* Two verified users with the same display name — Decision 7
                 * says reject the second joiner rather than preempt. */
                char consoleMsg[200];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "WARNING: verified-vs-verified collision for '%s'; "
                         "rejecting joiner", name);
                serverSimConsoleMessage(consoleMsg);
                /* Roll back the WBN client/join we recorded above. */
                if (incomingIsWBN) {
                    winboloNetClientLeaveGame(
                        (BYTE)slot, serverSimGetNumPlayers(sim),
                        serverSimGetNumNeutralBases(sim),
                        serverSimGetNumNeutralPills(sim));
                }
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
            char baseName[PACKET_MAX_PLAYER_NAME];
            char chosenName[PACKET_MAX_PLAYER_NAME];
            bool chosenFound = false;
            int suffixIdx;

            strncpy(baseName, udpServer.clients[i].playerName,
                    PACKET_MAX_PLAYER_NAME - 1);
            baseName[PACKET_MAX_PLAYER_NAME - 1] = '\0';

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
                 * slots (not just the victim's slot). */
                bool clash = false;
                int k;
                for (k = 0; k < MAX_TANKS; k++) {
                    if (!udpServer.clients[k].connected) continue;
                    if (k == i) continue;
                    if (k == slot) continue;
                    if (playerNameCompare(udpServer.clients[k].playerName,
                                          candidate) == 0) {
                        clash = true;
                        break;
                    }
                }
                if (clash) continue;

                strncpy(chosenName, candidate, PACKET_MAX_PLAYER_NAME - 1);
                chosenName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
                chosenFound = true;
                break;
            }

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
                if (incomingIsWBN) {
                    winboloNetClientLeaveGame(
                        (BYTE)slot, serverSimGetNumPlayers(sim),
                        serverSimGetNumNeutralBases(sim),
                        serverSimGetNumNeutralPills(sim));
                }
                serverSendJoinReject(fromAddr,
                                     STR_REJECT_NAME_POOL_EXHAUSTED, 0, NULL);
                return;
            }

            /* Apply the rename + broadcasts + newswire. */
            serverPreemptRename(sim, i, chosenName, name, incomingCountry);
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

    /* Merge client-supplied hints with server-determined WBN trust into a
     * single clientFlags byte before serverSimAddPlayer so the
     * log_PlayerJoined event captures the right value.  Always written
     * (cleared when not WBN) so a recycled slot doesn't inherit a previous
     * occupant's flags. */
    {
        uint8_t flags = clientHints & PLAYER_CLIENT_HINT_MASK;
        if (incomingIsWBN)                  flags |= PLAYER_FLAG_WBN_VERIFIED;
        if (incomingIsWBN && wbnHasSteam)   flags |= PLAYER_FLAG_WBN_STEAM_LINKED;
        if (incomingIsWBN && wbnIsSupporter) flags |= PLAYER_FLAG_SUPPORTER;
        playersSetClientFlags(&serverSimGetGameSim(sim)->plyrs, (BYTE)slot, flags);
        playersSetClientType (&serverSimGetGameSim(sim)->plyrs, (BYTE)slot, clientType);
    }
    WB_LOG_INFO(WB_LOG_CAT_NET,
                "join accept: slot=%d clientType=%u clientHints=0x%02x",
                slot, (unsigned)clientType, (unsigned)clientHints);

    /* Initialize player in the simulation */
    serverSimAddPlayer(sim, (BYTE)slot, udpServer.clients[slot].playerName,
                       udpServer.clients[slot].wantRejoin);

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

    /* Subscribe this client to the server's control-event bus so
     * future events can be encoded and unicast to it via the codec
     * table.  Register's sync-replay walks the lobby settings, every
     * connected slot, and every player-join and feeds them through
     * the codec encoder to this client's socket — replacing the old
     * composite PACKET_LOBBY_STATE handshake. */
    udpServer.clients[slot].controlSub =
        serverSimRegisterSubscriber(sim, udpClientDeliverControl,
                                    &udpServer.clients[slot]);

    winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                       (BYTE)slot, WINBOLO_NET_NO_PLAYER);

    if (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown) {
        /* Publish a lobby-slot update for the new player so existing
         * clients pick up the joiner's team/ready/ping fields (the
         * CTRL_PLAYER_JOIN already fanned out from serverSimAddPlayer
         * carries name/country/clientType, but not the lobby-slot
         * extras). */
        publishLobbySlot(sim, (BYTE)slot);
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
     * subscriber's sync replay: CTRL_GAME_PHASE(RUNNING) flows through
     * the codec encoder to this client's socket as part of
     * serverSimRegisterSubscriber above. */

    /* Initialize map download and send first batch of chunks */
    serverInitMapDownload(slot);
    fprintf(stderr, "[UDP SERVER] Sending %u map chunks to slot %d\n",
            udpServer.mapDownload[slot].totalChunks, slot);
    serverSendMapChunks(slot);

    /* Defer sending existing player list until after map download completes.
     * Sending now would be wiped by screenDestroy()/screenLoadCompressedMap()
     * on the client side when it finishes downloading the map. */
    udpServer.clients[slot].needsPlayerList = true;
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

        /* Only apply if this is a newer input than what we last processed */
        if (pkt.tick > serverSimGetLastProcessedInput(sim, clientIdx)) {
            if (udpServer.clients[clientIdx].inputsThisTick >= 4) break;
            serverSimApplyInput(sim, &pkt);
            udpServer.clients[clientIdx].inputsThisTick++;
        }
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
    SnapshotHeader hdr;
    TankSnapshot tankSnaps[MAX_TANKS];
    ShellSnapshot shellSnaps[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot tkExplSnaps[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot baseSnaps[MAX_SNAPSHOT_BASES];
    PillSnapshot pillSnaps[MAX_SNAPSHOT_PILLS];
    GameEvent eventSnaps[MAX_SNAPSHOT_EVENTS];
    ClientEventQueue *evQ = &udpServer.eventQueues[clientIdx];
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
     * + reliableEventCount(1) + reliableBaseSeq(4)
     * + mapEventCount(1) + mapEventBaseSeq(4)
     * + mapChecksum(2) = 25 bytes */
    packU32(buf + pos, hdr.serverTick);
    pos += 4;
    packU32(buf + pos, hdr.lastProcessedInput);
    pos += 4;
    countsPos = pos;
    pos += 17; /* 7 count bytes + 4 byte reliableBaseSeq + 4 byte mapEventBaseSeq + 2 byte mapChecksum */

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
    packU16(buf + countsPos + 15, hdr.mapChecksum);

    /* wire-only: per-tick snapshot — high-volume delta-encoded path with its own reliability discipline */
    udpSendTo(udpServer.sock, buf, pos, &client->addr);
    if (serverSimGetTick(sim) % 50 == 0) {
        WB_LOG_DEBUG(WB_LOG_CAT_NET, "[UDP SERVER] Send SNAPSHOT to slot %d: tick=%u tanks=%u shells=%u bases=%u pills=%u events=%u(ack=%u next=%u) mapEvts=%u(ack=%u next=%u) len=%d",
                     clientIdx, serverSimGetTick(sim), (unsigned)hdr.tankCount, (unsigned)hdr.shellCount,
                     (unsigned)hdr.baseCount,
                     (unsigned)hdr.pillCount, reliableEventCount,
                     evQ->ackedSeq, evQ->nextSeq,
                     mapEventCount, mapQ->ackedSeq, mapQ->nextSeq, pos);
    }
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
    }

    if (graceful) {
        snprintf(msg, sizeof(msg), "%s is quitting.",
                 udpServer.clients[idx].playerName);
        winbolonetAddEvent(WINBOLO_NET_EVENT_QUITTING, TRUE,
                           (BYTE)idx, WINBOLO_NET_NO_PLAYER);
    } else {
        snprintf(msg, sizeof(msg), "%s timed out.",
                 udpServer.clients[idx].playerName);
    }
    WB_LOG_INFO(WB_LOG_CAT_NET, "%s", msg);
    fprintf(stderr, "[UDP SERVER] %s\n", msg);
    serverSimConsoleMessage(msg);

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
    udpServer.clients[idx].connected = false;
    udpServer.clients[idx].nameStickySuffix = false;
    memset(udpServer.clients[idx].playerName, 0, PACKET_MAX_PLAYER_NAME);
    udpServer.clientLocked[idx] = false;
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
    int msgLen = (int)strlen(message);

    if (msgLen > 200) msgLen = 200;

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 0xFE; /* server raw English */
    evt.u.chat.destPlayer = 0xFF; /* broadcast */
    evt.u.chat.bodyLen = (uint16_t)msgLen;
    if (msgLen > 0) memcpy(evt.u.chat.body, message, msgLen);
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
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (serverSimIsLobbyEnabled(sim) &&
                (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown)) {
                publishLobbySlot(sim, (BYTE)i);
            }
            return;
        }
    }
    serverSimConsoleMessage("Player not found.");
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
                              const char *password,
                              BYTE maxPlayers) {
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

    memset(udpServer.password, 0, MAP_STR_SIZE);
    if (password != NULL) {
        strncpy(udpServer.password, password, MAP_STR_SIZE - 1);
    }

    udpServer.maxPlayers = maxPlayers;
    udpServer.running = true;
    udpServer.tickCount = 0;
    serverSimSetServerPort(sim, port);
    WB_LOG_INFO(WB_LOG_CAT_NET,
        "server created: port=%u bindAddr=%s maxPlayers=%u password=%s",
        port,
        (addrToUse && *addrToUse) ? addrToUse : "0.0.0.0",
        (unsigned)maxPlayers,
        (password && *password) ? "yes" : "no");
    fprintf(stderr, "[UDP SERVER] Created, bound to port %u\n", port);
    udpServer.compressedMapSize = 0;

    for (i = 0; i < MAX_TANKS; i++) {
        udpServer.clients[i].connected = false;
        udpServer.clients[i].nameStickySuffix = false;
        udpServer.clients[i].controlSub = SUBSCRIBER_HANDLE_INVALID;
        memset(&udpServer.mapDownload[i], 0, sizeof(ClientMapDownload));
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
        fprintf(stderr, "[UDP SERVER] Recv thread started\n");
    } else {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "failed to create recv thread, using polled fallback: %s",
            SDL_GetError());
        fprintf(stderr, "[UDP SERVER] WARNING: Failed to create recv thread, using polled fallback\n");
        SDL_SetAtomicInt(&recvThreadRunning, 0);
    }
#endif

    return true;
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

    pkt.has_password = udpServer.password[0] != '\0' ? 1 : 0;
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
 * Old protocol: 8-byte BOLOHEADER with "Bolo" signature + version + type. */
static bool isOldProtocolInfoRequest(const uint8_t *buf, int len) {
    return len == BOLOPACKET_REQUEST_SIZE &&
           memcmp(buf, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE) == 0 &&
           buf[BOLO_VERSION_MAJORPOS] == BOLO_VERSION_MAJOR &&
           buf[BOLO_VERSION_MINORPOS] == BOLO_VERSION_MINOR &&
           buf[BOLO_VERSION_REVISIONPOS] == BOLO_VERSION_REVISION &&
           buf[BOLOPACKET_REQUEST_TYPEPOS] == BOLOPACKET_INFOREQUEST;
}

void transportUdpServerOnGameStart(ServerSim *sim) {
    int i;
    (void)sim;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            /* Clients reload the map on game start which wipes their player
               data.  Re-send the player list so names are restored. */
            udpServer.clients[i].needsPlayerList = true;
            /* Ensure map download is considered complete so snapshots
             * are sent during the game even if a final chunk ack was lost. */
            udpServer.mapDownload[i].downloadComplete = TRUE;
        }
        /* Reset reliable event queues — stale events from the previous game
         * must not be resent after clients load the fresh map. */
        udpServer.eventQueues[i].nextSeq = 1;
        udpServer.eventQueues[i].ackedSeq = 1;
        udpServer.mapEventQueues[i].nextSeq = 1;
        udpServer.mapEventQueues[i].ackedSeq = 1;
    }
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

    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_LOBBY_MAP_CHANGE;
        serverSimPublishControl(sim, &evt);
    }

    fprintf(stderr, "[UDP SERVER] Map change prep: %u bytes compressed map\n",
            udpServer.compressedMapSize);

    /* Any meaningful change auto-unreadies every human in lobby state;
     * mid-game map swaps (random regeneration etc.) skip the unready
     * since everyone's mid-round. */
    if (serverSimGetState(sim) == serverStateLobby) {
        lobbyAutoUnreadyOnChange(sim);
    }
}

void transportUdpServerSetBotName(BYTE playerNum, const char *name) {
    if (playerNum >= MAX_TANKS) return;
    strncpy(udpServer.clients[playerNum].playerName, name,
            PACKET_MAX_PLAYER_NAME - 1);
    udpServer.clients[playerNum].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
}

const char *transportUdpServerGetPlayerName(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) {
        return NULL;
    }
    /* Bots have no UDP connection but their name was set via
     * transportUdpServerSetBotName; treat them as valid name owners. */
    if (!udpServer.clients[playerNum].connected && !botManagerIsBot(playerNum)) {
        return NULL;
    }
    return udpServer.clients[playerNum].playerName;
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
    pkt.has_password = udpServer.password[0] != '\0' ? 1 : 0;
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
    if (pktType != PACKET_INPUT && pktType != PACKET_MAP_ACK) {
        WB_LOG_TRACE(WB_LOG_CAT_NET,
            "recv %s (%u) len=%d from %s:%u",
            packetTypeName(pktType), pktType, len,
            inet_ntoa(fromAddr->sin_addr), ntohs(fromAddr->sin_port));
        fprintf(stderr, "[UDP SERVER] Recv %s (%u) len=%d from %s:%u\n",
                packetTypeName(pktType), pktType, len,
                inet_ntoa(fromAddr->sin_addr), ntohs(fromAddr->sin_port));
    }
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
        case PACKET_CHAT_MESSAGE: {
            /* Chat message format:
             *   [header 8] [destPlayer 1] [message up to PACKET_MAX_CHAT_MESSAGE] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len > PACKET_HEADER_SIZE + 1) {
                uint8_t destPlayer = buf[PACKET_HEADER_SIZE];
                int msgLen = len - PACKET_HEADER_SIZE - 1;
                if (msgLen > PACKET_MAX_CHAT_MESSAGE) msgLen = PACKET_MAX_CHAT_MESSAGE;

                {
                    ControlEvent evt;
                    memset(&evt, 0, sizeof(evt));
                    evt.type = CTRL_CHAT;
                    evt.u.chat.fromPlayer = (BYTE)clientIdx;
                    evt.u.chat.destPlayer = destPlayer;
                    evt.u.chat.bodyLen = (uint16_t)msgLen;
                    if (msgLen > 0) {
                        memcpy(evt.u.chat.body,
                               buf + PACKET_HEADER_SIZE + 1, msgLen);
                    }
                    serverSimPublishControl(sim, &evt);

                    {
                        char pstr[256];
                        int pLen = msgLen;
                        if (pLen > 255) pLen = 255;
                        pstr[0] = (char)pLen;
                        memcpy(pstr + 1, buf + PACKET_HEADER_SIZE + 1, pLen);
                        if (destPlayer == 0xFF) {
                            logAddEvent(log_MessageAll, (BYTE)clientIdx, 0, 0, 0, 0, pstr);
                        } else {
                            logAddEvent(log_MessagePlayers, (BYTE)clientIdx, destPlayer, 0, 0, 0, pstr);
                        }
                    }
                }
            }
            break;
        }
        case PACKET_NAME_CHANGE: {
            /* Name change format:
             *   [header 8] [playerNum 1] [newName PACKET_MAX_PLAYER_NAME] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 &&
                len >= PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME) {
                char newName[PACKET_MAX_PLAYER_NAME];
                int j;
                bool nameTaken = FALSE;

                /* WBN participants are not allowed to change their name */
                if (winboloNetIsPlayerParticipant((BYTE)clientIdx)) {
                    break;
                }

                memcpy(newName, buf + PACKET_HEADER_SIZE + 1,
                       PACKET_MAX_PLAYER_NAME);
                newName[PACKET_MAX_PLAYER_NAME - 1] = '\0';

                /* Validate the new name before any further checks.  On
                 * rejection, send PACKET_NAME_CHANGE_REJECT so the
                 * client can surface a localized reason — the client
                 * also validates locally, so this is the defensive
                 * fallback against tampered or stale clients. */
                {
                    char validated[PACKET_MAX_PLAYER_NAME];
                    PlayerNameValidationError vErr = PLAYER_NAME_OK;
                    if (!playerNameValidate(newName, validated,
                                            sizeof(validated), &vErr)) {
                        uint8_t reasonCode;
                        switch (vErr) {
                            case PLAYER_NAME_ERR_EMPTY:
                                reasonCode = NAME_REJECT_EMPTY;
                                break;
                            case PLAYER_NAME_ERR_RESERVED_PREFIX:
                                reasonCode = NAME_REJECT_RESERVED_PREFIX;
                                break;
                            case PLAYER_NAME_ERR_RESERVED_SUFFIX:
                                reasonCode = NAME_REJECT_RESERVED_SUFFIX;
                                break;
                            case PLAYER_NAME_ERR_MIXED_SCRIPTS:
                                reasonCode = NAME_REJECT_MIXED_SCRIPTS;
                                break;
                            case PLAYER_NAME_ERR_INVALID_UTF8:
                            case PLAYER_NAME_ERR_DISALLOWED_CHAR:
                            case PLAYER_NAME_ERR_TOO_LONG:
                            default:
                                reasonCode = NAME_REJECT_INVALID;
                                break;
                        }
                        serverSendNameChangeReject(clientIdx, reasonCode);
                        break;
                    }
                    strncpy(newName, validated, PACKET_MAX_PLAYER_NAME - 1);
                    newName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
                }

                /* Check name uniqueness */
                for (j = 0; j < MAX_TANKS; j++) {
                    if (j != clientIdx && udpServer.clients[j].connected &&
                        playerNameCompare(udpServer.clients[j].playerName, newName) == 0) {
                        nameTaken = TRUE;
                        break;
                    }
                }

                if (nameTaken) {
                    serverSendNameChangeReject(clientIdx, NAME_REJECT_TAKEN);
                    break;
                }

                if (newName[0] != '\0') {
                    char msg[30 + 2 * PACKET_MAX_PLAYER_NAME];
                    snprintf(msg, sizeof(msg), "Player '%s' changed name to '%s'.",
                             udpServer.clients[clientIdx].playerName, newName);
                    serverSimConsoleMessage(msg);

                    /* Update server-side name */
                    snprintf(udpServer.clients[clientIdx].playerName,
                             PACKET_MAX_PLAYER_NAME, "%s", newName);

                    /* Update the players struct and publish CTRL_PLAYER_NAME. */
                    serverSimSetPlayerName(serverSimGetActive(),
                                           (BYTE)clientIdx, newName);
                }
            }
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
                    publishLobbySlot(sim, (BYTE)clientIdx);
                    /* Dismiss any pending balance proposal — player composition changed */
                    if (serverSimGetBalanceProposal(sim)->pending) {
                        ControlEvent evt;
                        serverSimClearBalanceProposal(sim);
                        memset(&evt, 0, sizeof(evt));
                        evt.type = CTRL_BALANCE_PROPOSAL;
                        serverSimPublishControl(sim, &evt);
                    }
                }
            }
            break;
        }
        case PACKET_LOCK_TOGGLE: {
            /* Wire: [header 8] [allow 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 1) {
                bool allow = buf[PACKET_HEADER_SIZE] != 0;
                bool wasLocked = serverClientsAllLocked();
                char msg[128];

                udpServer.clientLocked[clientIdx] = !allow;

                snprintf(msg, sizeof(msg), "%s is now %s players to join.",
                         udpServer.clients[clientIdx].playerName,
                         allow ? "allowing" : "not allowing");
                serverSimConsoleMessage(msg);
                serverSendServerEnglishBroadcast(sim, msg);

                /* Check if consensus lock state changed */
                {
                    bool nowLocked = serverClientsAllLocked();
                    if (nowLocked && !wasLocked) {
                        serverSendServerEnglishBroadcast(sim,
                            "This game is now locked to new players (client lock)");
                        serverSimConsoleMessage(
                            "Game locked by client consensus.");
                        winboloNetSendLock(TRUE);
                    } else if (!nowLocked && wasLocked) {
                        serverSendServerEnglishBroadcast(sim,
                            "This game is now unlocked to new players (client unlock)");
                        serverSimConsoleMessage(
                            "Game unlocked by client consensus.");
                        if (!udpServer.gameLocked) {
                            winboloNetSendLock(FALSE);
                        }
                    }
                }
            }
            break;
        }
        case PACKET_ALLIANCE_REQUEST: {
            /* Wire: [header 8] [fromPlayer 1] [toPlayer 1] */
            int clientIdx = serverFindClient(fromAddr);
            fprintf(stderr, "[UDP SERVER] Alliance request: clientIdx=%d len=%d\n",
                    clientIdx, len);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 2) {
                uint8_t toPlayer = buf[PACKET_HEADER_SIZE + 1];
                fprintf(stderr, "[UDP SERVER] Alliance request from=%d to=%d connected=%d\n",
                        clientIdx, toPlayer,
                        (toPlayer < MAX_TANKS) ? udpServer.clients[toPlayer].connected : -1);
                if (toPlayer < MAX_TANKS &&
                    udpServer.clients[toPlayer].connected) {
                    ControlEvent reqEvt;
                    logAddEvent(log_AllyRequest, (BYTE)clientIdx, toPlayer, 0, 0, 0, NULL);
                    memset(&reqEvt, 0, sizeof(reqEvt));
                    reqEvt.type = CTRL_ALLIANCE_REQUEST;
                    reqEvt.u.allianceRequest.fromPlayer = (BYTE)clientIdx;
                    reqEvt.u.allianceRequest.toPlayer   = toPlayer;
                    serverSimPublishControl(serverSimGetActive(), &reqEvt);
                }
            }
            break;
        }
        case PACKET_ALLIANCE_ACCEPT: {
            /* Wire: [header 8] [fromPlayer 1] [toPlayer 1]
             * fromPlayer = the accepter, toPlayer = who requested */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 2) {
                uint8_t newMember = buf[PACKET_HEADER_SIZE + 1];
                serverSimAcceptAlliance(serverSimGetActive(),
                                        (BYTE)clientIdx, newMember);
                winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_JOIN, TRUE,
                                   (BYTE)clientIdx, newMember);
                logAddEvent(log_AllyAccept, (BYTE)clientIdx, newMember, 0, 0, 0, NULL);
            }
            break;
        }
        case PACKET_ALLIANCE_LEAVE: {
            /* Wire: [header 8] [playerNum 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 1) {
                serverSimLeaveAlliance(serverSimGetActive(), (BYTE)clientIdx);
                winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_LEAVE, TRUE,
                                   (BYTE)clientIdx, WINBOLO_NET_NO_PLAYER);
                logAddEvent(log_AllyLeave, (BYTE)clientIdx, 0, 0, 0, 0, NULL);
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
        case PACKET_LOBBY_TEAM_SET: {
            /* Wire: [header 8] [playerNum 1] [teamNumber 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && serverSimIsLobbyEnabled(sim) &&
                serverSimGetState(sim) == serverStateLobby &&
                len >= PACKET_HEADER_SIZE + 2) {
                uint8_t teamNum = buf[PACKET_HEADER_SIZE + 1];
                if (teamNum <= 16) {
                    serverSimSetTeam(sim, (BYTE)clientIdx, teamNum);
                    logAddEvent(log_TeamSet, (BYTE)clientIdx, teamNum, 0, 0, 0, NULL);
                    publishLobbySlot(sim, (BYTE)clientIdx);
                }
            }
            break;
        }
        case PACKET_LOBBY_READY: {
            /* Wire: [header 8] [playerNum 1] [ready 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && serverSimIsLobbyEnabled(sim) &&
                len >= PACKET_HEADER_SIZE + 2) {
                bool ready = buf[PACKET_HEADER_SIZE + 1] != 0;

                if (serverSimGetState(sim) == serverStateLobby) {
                    serverSimSetReady(sim, (BYTE)clientIdx, ready);
                    logAddEvent(ready ? log_PlayerReady : log_PlayerUnready, (BYTE)clientIdx, 0, 0, 0, 0, NULL);
                    publishLobbySlot(sim, (BYTE)clientIdx);
                    serverSimLobbyCheckAllReady(sim);
                    /* If all-ready check triggered countdown, broadcast it */
                    if (serverSimGetState(sim) == serverStateCountdown) {
                        logAddEvent(log_CountdownStart, 0, 0, 0, 0, 0, NULL);
                        uint8_t secs = (uint8_t)((serverSimGetCountdownTicks(sim) + 49) / 50);
                        ControlEvent evt;
                        memset(&evt, 0, sizeof(evt));
                        evt.type = CTRL_GAME_PHASE;
                        evt.u.gamePhase.phase = CTRL_PHASE_COUNTDOWN;
                        evt.u.gamePhase.countdownSeconds = secs;
                        serverSimPublishControl(sim, &evt);
                    }
                } else if (serverSimGetState(sim) == serverStateCountdown && !ready) {
                    /* Someone unreadied during countdown — revert to lobby */
                    serverSimSetReady(sim, (BYTE)clientIdx, false);
                    serverSimAbortCountdown(sim);
                    logAddEvent(log_PlayerUnready, (BYTE)clientIdx, 0, 0, 0, 0, NULL);
                    logAddEvent(log_CountdownCancel, 0, 0, 0, 0, 0, NULL);
                    serverSimConsoleMessage("Countdown cancelled — player unreadied.");
                    publishLobbySlot(sim, (BYTE)clientIdx);
                }
            }
            break;
        }
        case PACKET_LOBBY_ADD_BOT: {
            /* Wire: [header 8] [teamNumber 1] [pathLen 1] [path N]
             *       [nameLen 1] [name M]. Server uses its own stored
             *       brain config; client-picked teamNumber and botName
             *       come from the team-aware Add Bot button. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && serverSimIsLobbyEnabled(sim) &&
                serverSimGetState(sim) == serverStateLobby &&
                serverSimGetBotAiType(sim) != aiNone &&
                serverSimGetBotBrainPath(sim)[0] != '\0') {
                /* Parse wire payload — bounds-check each length so we
                 * never read past the received bytes. teamNumber, name,
                 * and path are all optional; missing or zero-length name
                 * falls back to "Bot N". */
                uint8_t teamNumber = 0;
                char clientBotName[PACKET_MAX_PLAYER_NAME] = "";
                int pos = PACKET_HEADER_SIZE;
                if (len >= pos + 1) {
                    teamNumber = buf[pos++];
                    if (len >= pos + 1) {
                        uint8_t pathLen = buf[pos++];
                        if (pathLen < BRAIN_LIST_PATH_LEN &&
                            len >= pos + pathLen) {
                            pos += pathLen;  /* skip path; server uses its own */
                            if (len >= pos + 1) {
                                uint8_t nameLen = buf[pos++];
                                if (nameLen < sizeof(clientBotName) &&
                                    len >= pos + nameLen) {
                                    memcpy(clientBotName, buf + pos, nameLen);
                                    clientBotName[nameLen] = '\0';
                                }
                            }
                        }
                    }
                }

                /* Find first free player slot */
                BYTE slot;
                bool found = false;
                for (slot = 0; slot < MAX_TANKS; slot++) {
                    if (!serverSimIsPlayerConnected(sim, slot)) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    char botName[64];
                    if (clientBotName[0] != '\0') {
                        SDL_strlcpy(botName, clientBotName, sizeof(botName));
                    } else {
                        snprintf(botName, sizeof(botName), "Bot %d", slot + 1);
                    }
                    if (botManagerAddBot(sim, slot, serverSimGetBotBrainPath(sim), botName,
                                         serverSimGetBotAiType(sim),
                                         gameTypeGet(&serverSimGetGameSim(sim)->game),
                                         serverSimGetGameSim(sim)->hiddenMines)) {
                        transportUdpServerSetBotName(slot, botName);
                        if (teamNumber > 0 && teamNumber < MAX_TANKS) {
                            serverSimSetTeam(sim, slot, teamNumber);
                        }
                        publishLobbySlot(sim, slot);
                    }
                }
            }
            break;
        }
        case PACKET_LOBBY_REMOVE_BOT: {
            /* Wire: [header 8] [playerNum 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && serverSimIsLobbyEnabled(sim) &&
                serverSimGetState(sim) == serverStateLobby &&
                len >= PACKET_HEADER_SIZE + 1) {
                uint8_t targetSlot = buf[PACKET_HEADER_SIZE];
                if (targetSlot < MAX_TANKS && botManagerIsBot(targetSlot)) {
                    botManagerRemoveBot(sim, targetSlot);
                    publishLobbySlot(sim, targetSlot);
                }
            }
            break;
        }
        case PACKET_LOBBY_SET_SETTING: {
            /* Wire: [header 8] [settingType 1] [valueLen 1] [value N] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 2) break;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_SETTING,
                              LOBBY_REJECT_NOT_HOST);
                break;
            }
            uint8_t settingType = buf[PACKET_HEADER_SIZE];
            uint8_t valueLen    = buf[PACKET_HEADER_SIZE + 1];
            if (len < PACKET_HEADER_SIZE + 2 + valueLen ||
                valueLen > 32) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_SETTING,
                              LOBBY_REJECT_INVALID);
                break;
            }
            const uint8_t *value = buf + PACKET_HEADER_SIZE + 2;

            uint16_t lockBit = 0;
            switch (settingType) {
                case LST_GAME_TYPE:         lockBit = LOBBY_LOCK_GAME_TYPE; break;
                case LST_HIDDEN_MINES:      lockBit = LOBBY_LOCK_MINES; break;
                case LST_AI_POLICY:         lockBit = LOBBY_LOCK_AI_POLICY; break;
                case LST_TIME_LIMIT:        lockBit = LOBBY_LOCK_TIME_LIMIT; break;
                case LST_TIME_MINUTES:      lockBit = LOBBY_LOCK_TIME_LIMIT; break;
                case LST_AUTO_LOCK_ON_GAME: lockBit = LOBBY_LOCK_AUTO_LOCK_ON_GAME; break;
                default:                    lockBit = 0xFFFF; break;
            }
            if (lockBit == 0xFFFF) {
                /* Unknown setting — silently drop (forward-compat). */
                break;
            }
            if (serverSimGetServerLocks(sim) & lockBit) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_SETTING,
                              LOBBY_REJECT_LOCKED);
                break;
            }

            switch (settingType) {
                case LST_GAME_TYPE:
                    if (valueLen == 1 && value[0] >= 1 && value[0] <= 3) {
                        serverSimGetGameSim(sim)->game = (gameType)value[0];
                    } else { lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_SETTING,
                                           LOBBY_REJECT_INVALID); break; }
                    break;
                case LST_HIDDEN_MINES:
                    if (valueLen == 1) serverSimGetGameSim(sim)->hiddenMines = value[0] != 0;
                    break;
                case LST_AI_POLICY:
                    if (valueLen == 1 && value[0] <= 3) {
                        serverSimSetAiPolicy(sim, value[0]);
                        /* botAiType gates the AddBot handler and rides
                         * in CTRL_LOBBY_SETTINGS — keep them in sync or
                         * the next settings publish snaps clients back
                         * to the CLI startup value. */
                        serverSimSetBotAiType(sim, (aiType)value[0]);
                        if ((aiType)value[0] == aiNone) {
                            for (BYTE bi = 0; bi < MAX_TANKS; bi++) {
                                if (botManagerIsBot(bi)) {
                                    botManagerRemoveBot(sim, bi);
                                    publishLobbySlot(sim, bi);
                                }
                            }
                        }
                    } else { lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_SETTING,
                                           LOBBY_REJECT_INVALID); break; }
                    break;
                case LST_TIME_LIMIT:
                    if (valueLen == 1) {
                        bool tl = value[0] != 0;
                        serverSimSetTimeLimit(sim, tl);
                        if (tl) {
                            uint16_t mins = serverSimGetTimeMinutes(sim) > 0
                                ? serverSimGetTimeMinutes(sim) : 30;
                            serverSimSetGameLength(sim,
                                (int32_t)mins * 60 * GAME_NUMGAMETICKS_SEC);
                        } else {
                            serverSimSetGameLength(sim, UNLIMITED_GAME_TIME);
                        }
                    }
                    break;
                case LST_TIME_MINUTES:
                    if (valueLen == 2) {
                        uint16_t mins =
                            (uint16_t)((value[0] << 8) | value[1]);
                        serverSimSetTimeMinutes(sim, mins);
                        if (serverSimGetTimeLimit(sim)) {
                            serverSimSetGameLength(sim,
                                (int32_t)mins * 60 * GAME_NUMGAMETICKS_SEC);
                        }
                    } else { lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_SETTING,
                                           LOBBY_REJECT_INVALID); break; }
                    break;
                case LST_AUTO_LOCK_ON_GAME:
                    if (valueLen == 1) serverSimSetAutoLockOnGameStart(sim, value[0] != 0);
                    break;
            }

            serverSimPublishLobbySettings(sim);
            lobbyAutoUnreadyOnChange(sim);
            break;
        }
        case PACKET_LOBBY_OPEN_HOST: {
            /* Wire: [header 8] [bool 1]. Host-only. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx != 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 1) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_OPEN_HOST,
                              LOBBY_REJECT_NOT_HOST);
                break;
            }
            serverSimSetOpenHost(sim, buf[PACKET_HEADER_SIZE] != 0);
            serverSimPublishLobbySettings(sim);
            lobbyAutoUnreadyOnChange(sim);
            break;
        }
        case PACKET_LOBBY_TEAM_META: {
            /* Wire: [header 8] [teamId 1] [color 1] [namingPool 1]
             *       [nameLen 1] [name N] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 4) break;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_TEAM_META,
                              LOBBY_REJECT_NOT_HOST); break;
            }
            uint8_t teamId     = buf[PACKET_HEADER_SIZE + 0];
            uint8_t color      = buf[PACKET_HEADER_SIZE + 1];
            uint8_t namingPool = buf[PACKET_HEADER_SIZE + 2];
            uint8_t nameLen    = buf[PACKET_HEADER_SIZE + 3];
            if (teamId == 0 || teamId >= MAX_TANKS ||
                nameLen > LOBBY_TEAM_NAME_LEN - 1 ||
                len < PACKET_HEADER_SIZE + 4 + nameLen) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_TEAM_META,
                              LOBBY_REJECT_INVALID); break;
            }
            TeamMetadata *t = serverSimGetTeamMetaMut(sim, teamId);
            if (t == NULL) break;
            t->in_use = 1;
            t->color = color;
            /* Per-team uniqueness on namingPool: if another in_use team
             * already owns this pool, pick the lowest pool index not
             * used by any other team. Falls back to the requested value
             * if every pool is taken. */
            {
                int poolCount = lobbyBotPoolCount();
                bool poolTaken = false;
                for (BYTE other = 1; other < MAX_TANKS; other++) {
                    if (other == teamId) continue;
                    const TeamMetadata *ot = serverSimGetTeamMetaMut(sim, other);
                    if (ot && ot->in_use && ot->namingPool == namingPool) {
                        poolTaken = true;
                        break;
                    }
                }
                if (poolTaken && poolCount > 0) {
                    for (int p = 0; p < poolCount; p++) {
                        bool used = false;
                        for (BYTE other = 1; other < MAX_TANKS; other++) {
                            if (other == teamId) continue;
                            const TeamMetadata *ot = serverSimGetTeamMetaMut(sim, other);
                            if (ot && ot->in_use && ot->namingPool == p) {
                                used = true;
                                break;
                            }
                        }
                        if (!used) { namingPool = (uint8_t)p; break; }
                    }
                }
            }
            t->namingPool = namingPool;
            memset(t->name, 0, LOBBY_TEAM_NAME_LEN);
            if (nameLen > 0) {
                memcpy(t->name, buf + PACKET_HEADER_SIZE + 4, nameLen);
            }
            serverSimPublishLobbyTeamMeta(sim, teamId);
            lobbyAutoUnreadyOnChange(sim);
            break;
        }
        case PACKET_LOBBY_TEAM_CLEAR: {
            /* Wire: [header 8] [teamId 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 1) break;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_TEAM_CLEAR,
                              LOBBY_REJECT_NOT_HOST); break;
            }
            uint8_t teamId = buf[PACKET_HEADER_SIZE];
            if (teamId == 0 || teamId >= MAX_TANKS) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_TEAM_CLEAR,
                              LOBBY_REJECT_INVALID); break;
            }
            {
                TeamMetadata *t = serverSimGetTeamMetaMut(sim, teamId);
                if (t != NULL) {
                    memset(t, 0, sizeof(TeamMetadata));
                }
            }
            serverSimPublishLobbyTeamMeta(sim, teamId);
            lobbyAutoUnreadyOnChange(sim);
            break;
        }
        case PACKET_LOBBY_BOT_CONFIG: {
            /* Wire: [header 8] [slot 1] [difficulty 1] [personality 1]
             *       [nameLen 1] [name N] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 4) break;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_BOT_CONFIG,
                              LOBBY_REJECT_NOT_HOST); break;
            }
            uint8_t slot        = buf[PACKET_HEADER_SIZE + 0];
            uint8_t difficulty  = buf[PACKET_HEADER_SIZE + 1];
            uint8_t personality = buf[PACKET_HEADER_SIZE + 2];
            uint8_t nameLen     = buf[PACKET_HEADER_SIZE + 3];
            if (slot >= MAX_TANKS || difficulty > 2 || personality > 3 ||
                nameLen > 31 ||
                len < PACKET_HEADER_SIZE + 4 + nameLen ||
                !botManagerIsBot(slot)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_BOT_CONFIG,
                              LOBBY_REJECT_INVALID); break;
            }
            {
                LobbyBotConfig *bc = serverSimGetBotConfigMut(sim, slot);
                if (bc) {
                    bc->difficulty  = difficulty;
                    bc->personality = personality;
                }
            }
            if (nameLen > 0) {
                char name[PACKET_MAX_PLAYER_NAME];
                memset(name, 0, sizeof(name));
                memcpy(name, buf + PACKET_HEADER_SIZE + 4, nameLen);
                transportUdpServerSetBotName(slot, name);
            }
            serverSimPublishLobbyBotConfig(sim, slot);
            lobbyAutoUnreadyOnChange(sim);
            break;
        }
        case PACKET_LOBBY_KICK: {
            /* Wire: [header 8] [slot 1]. Host-only. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx != 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 1) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_KICK,
                              LOBBY_REJECT_NOT_HOST); break;
            }
            uint8_t slot = buf[PACKET_HEADER_SIZE];
            if (slot >= MAX_TANKS || slot == 0 /* can't kick host */) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_KICK,
                              LOBBY_REJECT_INVALID); break;
            }
            const char *name = transportUdpServerGetPlayerName(slot);
            if (name) transportUdpServerKickPlayer(sim, name);
            /* No explicit broadcast — the kick path itself fires the
             * existing player-left flow. */
            lobbyAutoUnreadyOnChange(sim);
            break;
        }
        case PACKET_LOBBY_SET_BOT_BRAIN: {
            /* Wire: [header 8] [slot 1] [pathLen 1] [path N]. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 2) break;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_BOT_BRAIN,
                              LOBBY_REJECT_NOT_HOST); break;
            }
            uint8_t slot    = buf[PACKET_HEADER_SIZE + 0];
            uint8_t pathLen = buf[PACKET_HEADER_SIZE + 1];
            if (slot >= MAX_TANKS || pathLen >= BRAIN_LIST_PATH_LEN ||
                len < PACKET_HEADER_SIZE + 2 + pathLen ||
                !botManagerIsBot(slot)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_BOT_BRAIN,
                              LOBBY_REJECT_INVALID); break;
            }
            {
                char pathBuf[260];
                memset(pathBuf, 0, sizeof(pathBuf));
                if (pathLen > 0 && pathLen < sizeof(pathBuf)) {
                    memcpy(pathBuf, buf + PACKET_HEADER_SIZE + 2, pathLen);
                }
                serverSimSetBotBrainPathFor(sim, slot, pathBuf);
                botManagerSetBrainPath(slot, pathBuf);
            }
            serverSimPublishLobbyBotBrain(sim, slot);
            lobbyAutoUnreadyOnChange(sim);
            break;
        }
        case PACKET_LOBBY_SET_MAP: {
            /* Wire: [header 8] [pathLen 1] [path N]. Host / openHost
             * / admin only, lobby state only. Path is relative to
             * data/maps/ — reject absolute, Windows drive, and any
             * ".." segment before opening the file. */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 1) break;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_MAP,
                              LOBBY_REJECT_NOT_HOST);
                break;
            }
            uint8_t pathLen = buf[PACKET_HEADER_SIZE];
            if (pathLen == 0 || pathLen > 255 ||
                len < PACKET_HEADER_SIZE + 1 + pathLen) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_MAP,
                              LOBBY_REJECT_INVALID);
                break;
            }
            char relPath[256];
            memcpy(relPath, buf + PACKET_HEADER_SIZE + 1, pathLen);
            relPath[pathLen] = '\0';

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
            if (!safe) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_MAP,
                              LOBBY_REJECT_INVALID);
                WB_LOG_INFO(WB_LOG_CAT_NET,
                            "[LOBBY] SET_MAP rejected: unsafe path '%s'",
                            relPath);
                break;
            }

            char fullPath[FILENAME_MAX];
            SDL_snprintf(fullPath, sizeof(fullPath), "data/maps/%s",
                         relPath);

            if (!serverSimReloadMap(sim, fullPath)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_SET_MAP,
                              LOBBY_REJECT_INVALID);
                WB_LOG_INFO(WB_LOG_CAT_NET,
                            "[LOBBY] SET_MAP failed: '%s'", fullPath);
                break;
            }

            transportUdpServerOnLobbyMapChange(sim);
            serverSimPublishLobbySettings(sim);
            WB_LOG_INFO(WB_LOG_CAT_NET,
                        "[LOBBY] SET_MAP ok: '%s'", fullPath);
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

            ServerMapEntry entries[64];
            int got = -1;
            if (safe) {
                got = serverSimEnumerateMapDir(sim,
                    relPath[0] == '\0' ? NULL : relPath,
                    entries, 64);
            }
            if (got < 0) got = 0;

            uint8_t rsp[PACKET_HEADER_SIZE + 1 + 256 + 1
                        + 64 * (1 + 128 + 1 + 8)];
            int rpos = PACKET_HEADER_SIZE;
            packHeader(rsp, PACKET_LOBBY_MAP_LIST_RSP, 0);
            rsp[rpos++] = pathLen;
            if (pathLen > 0) {
                memcpy(rsp + rpos, relPath, pathLen);
                rpos += pathLen;
            }
            int countPos = rpos;
            rsp[rpos++] = 0;
            int written = 0;
            for (int i = 0; i < got; i++) {
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
            udpSendTo(udpServer.sock, rsp, rpos, fromAddr);
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
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_NOT_HOST;
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
                totalLen == 0 || totalLen > UPLOAD_MAX_BYTES) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_INVALID;
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
                break;
            }
            char nameBuf[128];
            memset(nameBuf, 0, sizeof(nameBuf));
            memcpy(nameBuf, buf + PACKET_HEADER_SIZE + 5, nameLen);

            /* Name safety: must end in ".map", no path separators,
             * no leading dot. */
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
            if (!nameSafe) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_INVALID;
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
                break;
            }

            if (udpServer.clientUploadBuf[clientIdx]) {
                free(udpServer.clientUploadBuf[clientIdx]);
                udpServer.clientUploadBuf[clientIdx] = NULL;
            }
            udpServer.clientUploadBuf[clientIdx] = (uint8_t *)malloc(totalLen);
            if (!udpServer.clientUploadBuf[clientIdx]) {
                uint8_t ack[PACKET_HEADER_SIZE + 1];
                packHeader(ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
                ack[PACKET_HEADER_SIZE] = LOBBY_REJECT_INVALID;
                udpSendTo(udpServer.sock, ack, sizeof(ack), fromAddr);
                break;
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
             * writes to data/maps/.pending_upload.map, then replies
             * with MAP_UPLOAD_DONE. */
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
                SDL_CreateDirectory("data/maps/Uploads");

                const char *origName = udpServer.clientUploadName[clientIdx];
                char baseName[128];
                char extName[16];
                {
                    const char *dot = strrchr(origName, '.');
                    if (dot && dot != origName) {
                        size_t baseLen = (size_t)(dot - origName);
                        if (baseLen >= sizeof(baseName)) {
                            baseLen = sizeof(baseName) - 1;
                        }
                        memcpy(baseName, origName, baseLen);
                        baseName[baseLen] = '\0';
                        SDL_strlcpy(extName, dot, sizeof(extName));
                    } else {
                        SDL_strlcpy(baseName, origName, sizeof(baseName));
                        extName[0] = '\0';
                    }
                }

                char finalName[128];
                char outPath[FILENAME_MAX];
                SDL_strlcpy(finalName, origName, sizeof(finalName));
                SDL_snprintf(outPath, sizeof(outPath),
                             "data/maps/Uploads/%s", finalName);
                {
                    SDL_PathInfo info;
                    for (int n = 1; n < 1000; n++) {
                        if (!SDL_GetPathInfo(outPath, &info)) break;
                        SDL_snprintf(finalName, sizeof(finalName),
                                     "%s (%d)%s", baseName, n, extName);
                        SDL_snprintf(outPath, sizeof(outPath),
                                     "data/maps/Uploads/%s", finalName);
                    }
                }

                /* Stage the upload at a temp path during preview;
                 * PREVIEW_COMMIT moves it into Uploads/, CANCEL
                 * deletes it. */
                char tempPath[FILENAME_MAX];
                SDL_snprintf(tempPath, sizeof(tempPath),
                             "data/maps/.pending_upload.map");
                FILE *fp = fopen(tempPath, "wb");
                bool wrote = false;
                if (fp) {
                    size_t w = fwrite(udpServer.clientUploadBuf[clientIdx],
                                      1, total, fp);
                    fclose(fp);
                    wrote = (w == total);
                }

                free(udpServer.clientUploadBuf[clientIdx]);
                udpServer.clientUploadBuf[clientIdx] = NULL;
                udpServer.clientUploadActive[clientIdx] = false;
                udpServer.clientUploadHave[clientIdx]   = 0;
                udpServer.clientUploadTotal[clientIdx]  = 0;

                bool previewed = false;
                if (wrote) {
                    if (serverSimReloadMap(sim, tempPath)) {
                        /* serverSimReloadMap sets mapName from the
                         * basename of mapFileName — for an upload
                         * that yields ".pending_upload"; override
                         * with the user-picked basename. */
                        char displayName[MAP_STR_SIZE];
                        SDL_strlcpy(displayName, finalName,
                                    sizeof(displayName));
                        {
                            size_t dlen = SDL_strlen(displayName);
                            if (dlen >= 4 &&
                                SDL_strcasecmp(displayName + dlen - 4,
                                               ".map") == 0) {
                                displayName[dlen - 4] = '\0';
                            }
                        }
                        serverSimSetMapName(sim, displayName);
                        char relReturnEarly[256];
                        SDL_snprintf(relReturnEarly, sizeof(relReturnEarly),
                                     "Uploads/%s", finalName);
                        serverSimSetPendingUpload(sim, tempPath, outPath,
                                                  relReturnEarly);
                        transportUdpServerOnLobbyMapChange(sim);
                        serverSimPublishLobbySettings(sim);
                        previewed = true;
                    } else {
                        SDL_RemovePath(tempPath);
                    }
                } else if (tempPath[0] != '\0') {
                    SDL_RemovePath(tempPath);
                }

                char relReturn[256];
                SDL_snprintf(relReturn, sizeof(relReturn), "Uploads/%s",
                             finalName);
                int relLen = (int)SDL_strlen(relReturn);
                if (relLen > 255) relLen = 255;
                uint8_t done[PACKET_HEADER_SIZE + 2 + 256];
                int dpos = PACKET_HEADER_SIZE;
                packHeader(done, PACKET_LOBBY_MAP_UPLOAD_DONE, 0);
                done[dpos++] = (wrote && previewed) ? 0 : LOBBY_REJECT_INVALID;
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

            ServerMapEntry entries[64];
            int got = serverSimSearchMapDir(sim,
                relPath[0] == '\0' ? NULL : relPath,
                query, entries, 64);
            if (got < 0) got = 0;

            uint8_t rsp[PACKET_HEADER_SIZE + 1 + 256 + 1 + 128 + 1
                        + 64 * (1 + 256 + 1 + 8)];
            packHeader(rsp, PACKET_LOBBY_MAP_SEARCH_RSP, 0);
            int wpos = PACKET_HEADER_SIZE;
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
            int countPos = wpos;
            rsp[wpos++] = 0;
            int written = 0;
            for (int i = 0; i < got; i++) {
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
            udpSendTo(udpServer.sock, rsp, wpos, fromAddr);
            break;
        }
        case PACKET_LOBBY_PREVIEW_CANCEL: {
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby) break;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_PREVIEW_CANCEL,
                              LOBBY_REJECT_NOT_HOST);
                break;
            }
            if (serverSimRevertPreview(sim)) {
                transportUdpServerOnLobbyMapChange(sim);
                serverSimPublishLobbySettings(sim);
                WB_LOG_INFO(WB_LOG_CAT_NET,
                            "[LOBBY] PREVIEW_CANCEL: rolled back");
            }
            break;
        }
        case PACKET_LOBBY_PREVIEW_COMMIT: {
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby) break;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_PREVIEW_COMMIT,
                              LOBBY_REJECT_NOT_HOST);
                break;
            }
            serverSimCommitPreview(sim);
            WB_LOG_INFO(WB_LOG_CAT_NET,
                        "[LOBBY] PREVIEW_COMMIT: kept current map");
            break;
        }
        case PACKET_LOBBY_PREVIEW_RANDOM: {
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx < 0 || !serverSimIsLobbyEnabled(sim) ||
                serverSimGetState(sim) != serverStateLobby ||
                len < PACKET_HEADER_SIZE + 1) break;
            if (!lobbyClientMayEdit(sim, clientIdx)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_PREVIEW_RANDOM,
                              LOBBY_REJECT_NOT_HOST);
                break;
            }
            uint8_t seedLen = buf[PACKET_HEADER_SIZE];
            if (seedLen == 0 || seedLen > 63 ||
                len < PACKET_HEADER_SIZE + 1 + seedLen) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_PREVIEW_RANDOM,
                              LOBBY_REJECT_INVALID);
                break;
            }
            char seedStr[64];
            memset(seedStr, 0, sizeof(seedStr));
            memcpy(seedStr, buf + PACKET_HEADER_SIZE + 1, seedLen);

            MapGenConfig cfg;
            if (!mapGenSeedToConfig(seedStr, &cfg)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_PREVIEW_RANDOM,
                              LOBBY_REJECT_INVALID);
                WB_LOG_INFO(WB_LOG_CAT_NET,
                            "[LOBBY] PREVIEW_RANDOM rejected: bad seed '%s'",
                            seedStr);
                break;
            }
            /* Seeds may have been generated against a different region;
             * pin to the standard playable area. */
            cfg.x1 = MAP_MINE_EDGE_LEFT + 1;
            cfg.y1 = MAP_MINE_EDGE_TOP + 1;
            cfg.x2 = MAP_MINE_EDGE_RIGHT - 1;
            cfg.y2 = MAP_MINE_EDGE_BOTTOM - 1;

            if (!serverSimReloadRandomMap(sim, &cfg)) {
                lobbyRejectTo(fromAddr, PACKET_LOBBY_PREVIEW_RANDOM,
                              LOBBY_REJECT_INVALID);
                break;
            }
            transportUdpServerOnLobbyMapChange(sim);
            serverSimPublishLobbySettings(sim);
            WB_LOG_INFO(WB_LOG_CAT_NET,
                        "[LOBBY] PREVIEW_RANDOM ok: '%s'", seedStr);
            break;
        }
        case PACKET_WBN_REAUTH: {
            /* Wire: [header 8] [wbnToken 65] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + WBN_TOKEN_WIRE_LEN) {
                char token[WBN_TOKEN_WIRE_LEN];
                memcpy(token, buf + PACKET_HEADER_SIZE, WBN_TOKEN_WIRE_LEN);
                token[WBN_TOKEN_WIRE_LEN - 1] = '\0';

                if (winbolonetIsRunning() && token[0] != '\0') {
                    char errorMsg[512];
                    bool hasSteam = FALSE;
                    bool wbnIsSupporter = FALSE;
                    errorMsg[0] = '\0';
                    if (winbolonetServerVerifyToken(token, (BYTE)clientIdx, errorMsg,
                                                    &hasSteam, &wbnIsSupporter)) {
                        /* Re-merge using the clientHints captured at JOIN_REQUEST
                         * (the client doesn't re-send them on REAUTH; we re-verify
                         * against WBN, not the network). */
                        uint8_t storedHints = udpServer.clients[clientIdx].clientHints;
                        uint8_t flags = storedHints & PLAYER_CLIENT_HINT_MASK;
                        flags |= PLAYER_FLAG_WBN_VERIFIED;
                        if (hasSteam) flags |= PLAYER_FLAG_WBN_STEAM_LINKED;
                        if (wbnIsSupporter) flags |= PLAYER_FLAG_SUPPORTER;
                        playersSetClientFlags(&serverSimGetGameSim(sim)->plyrs, (BYTE)clientIdx, flags);
                        playersSetClientType (&serverSimGetGameSim(sim)->plyrs, (BYTE)clientIdx,
                                              udpServer.clients[clientIdx].clientType);
                        fprintf(stderr, "[UDP SERVER] Player %d WBN re-authenticated (steam=%d)\n",
                                clientIdx, hasSteam ? 1 : 0);
                        /* If game is already running, send the join event now */
                        if (serverSimGetState(sim) == serverStateRunning) {
                            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                               (BYTE)clientIdx, WINBOLO_NET_NO_PLAYER);
                        }
                        /* Broadcast updated flags so other clients see WBN badge */
                        if (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown) {
                            publishLobbySlot(sim, (BYTE)clientIdx);
                        }
                    } else {
                        fprintf(stderr, "[UDP SERVER] Player %d WBN re-auth failed: %s\n",
                                clientIdx, errorMsg);
                    }
                }
            }
            break;
        }
        case PACKET_BALANCE_REQUEST: {
            /* Wire: [header 8] [teamSize 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx == 0 && serverSimIsLobbyEnabled(sim) &&
                serverSimGetState(sim) == serverStateLobby &&
                len >= PACKET_HEADER_SIZE + 1 &&
                !serverSimGetBalanceProposal(sim)->requestInFlight &&
                !serverSimGetBalanceProposal(sim)->pending &&
                winbolonetIsRunning()) {
                BalanceThreadData *btd = malloc(sizeof(BalanceThreadData));
                if (btd) {
                    SDL_Thread *t;
                    int i;
                    btd->sim = sim;
                    btd->teamSize = buf[PACKET_HEADER_SIZE];
                    btd->totalPlayers = 0;
                    for (i = 0; i < MAX_TANKS; i++) {
                        if (serverSimIsPlayerConnected(sim, i)) btd->totalPlayers++;
                    }
                    serverSimSetBalanceRequestInFlight(sim, true);
                    t = SDL_CreateThread(balanceThreadFunc, "WbnBalance", btd);
                    if (t) {
                        SDL_DetachThread(t);
                    } else {
                        serverSimSetBalanceRequestInFlight(sim, false);
                        free(btd);
                        serverSimConsoleMessage("Failed to start balance thread");
                    }
                }
            }
            break;
        }
        case PACKET_BALANCE_APPLY: {
            /* Wire: [header 8] (no payload) */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx == 0 && serverSimIsLobbyEnabled(sim) &&
                serverSimGetState(sim) == serverStateLobby &&
                serverSimGetBalanceProposal(sim)->pending) {
                int i;
                for (i = 0; i < MAX_TANKS; i++) {
                    if (serverSimGetBalanceProposal(sim)->teamForSlot[i] != 0) {
                        serverSimSetTeam(sim, (BYTE)i, serverSimGetBalanceProposal(sim)->teamForSlot[i]);
                    }
                }
                serverSimClearBalanceProposal(sim);
                logAddEvent(log_BalanceApplied, 0, 0, 0, 0, 0, NULL);
                publishLobbyStateAll(sim);
                serverSimConsoleMessage("Team balance applied");
            }
            break;
        }
        case PACKET_BALANCE_DISMISS: {
            /* Wire: [header 8] (no payload) */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx == 0 && serverSimIsLobbyEnabled(sim) &&
                serverSimGetState(sim) == serverStateLobby &&
                serverSimGetBalanceProposal(sim)->pending) {
                ControlEvent evt;
                serverSimClearBalanceProposal(sim);
                memset(&evt, 0, sizeof(evt));
                evt.type = CTRL_BALANCE_PROPOSAL;
                serverSimPublishControl(sim, &evt);
            }
            break;
        }
        case PACKET_MAP_SKIP_VOTE: {
            /* Wire: [header 8] (no payload — server identifies player by source) */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0) {
                ControlEvent evt;
                int i;
                serverSimMapSkipVoteToggle(sim, (uint8_t)clientIdx);
                memset(&evt, 0, sizeof(evt));
                evt.type = CTRL_MAP_SKIP_STATE;
                for (i = 0; i < MAX_TANKS; i++) {
                    evt.u.mapSkipState.votes[i] = serverSimIsMapSkipVote(sim, i) ? 1 : 0;
                }
                serverSimPublishControl(sim, &evt);
            }
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

/* Receive all pending packets from clients (polled fallback) */
void transportUdpServerRecv(ServerSim *sim) {
    uint8_t buf[UDP_MAX_PAYLOAD];
    struct sockaddr_in fromAddr;
    int len;
    int c;

    if (!udpServer.running) return;

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

    WB_LOG_DEBUG(WB_LOG_CAT_NET, "[UDP SERVER] Enqueuing %d events + %d map events from tick=%u",
                 serverSimGetEventCount(sim), serverSimGetMapEventCount(sim), serverSimGetTick(sim));

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

/* Check for client timeouts — call from any server state (lobby, running, etc.) */
void transportUdpServerCheckTimeouts(ServerSim *sim) {
    int i;

    if (!udpServer.running) return;

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
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
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (serverSimIsLobbyEnabled(sim) &&
                (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown)) {
                publishLobbySlot(sim, (BYTE)i);
                /* Dismiss any pending balance proposal — player composition changed */
                if (serverSimGetBalanceProposal(sim)->pending) {
                    ControlEvent evt;
                    serverSimClearBalanceProposal(sim);
                    memset(&evt, 0, sizeof(evt));
                    evt.type = CTRL_BALANCE_PROPOSAL;
                    serverSimPublishControl(sim, &evt);
                }
            }
        }
    }
}

/* Combined receive + tick + send (for callers that don't need split) */
void transportUdpServerTick(ServerSim *sim) {
    if (recvThread) {
        transportUdpServerDrainRecvQueue(sim);
    } else {
        transportUdpServerRecv(sim);
    }
    transportUdpServerDrainEvents(sim);
    transportUdpServerSend(sim);
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

BYTE transportUdpServerGetMaxPlayers(void) {
    return udpServer.maxPlayers;
}
