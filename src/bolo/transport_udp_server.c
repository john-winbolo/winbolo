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
#include "screen.h"
#include "messages.h"
#include "util.h"
#include "game_sim.h"
#include "../server/geolookup.h"
#include "../server/server_sim.h"
#include "../server/server_lifecycle.h"
#include "../winbolonet/winbolonet.h"
#include "../server/threads.h"
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
    winbolonetServerRequestBalance(btd->totalPlayers, btd->teamSize, &sim->balanceProposal);

    free(btd);

    /* If the server is shutting down, signal completion and exit without
     * acquiring the mutex (the main thread may have already torn it down). */
    if (SDL_GetAtomicInt(&sim->balanceProposal.shutdownFlag)) {
        sim->balanceProposal.requestInFlight = false;
        return 0;
    }

    /* Write results back under the game mutex */
    threadsWaitForMutex();
    sim->balanceProposal.requestInFlight = false;
    if (sim->balanceProposal.pending) {
        sim->balanceProposal.broadcastNeeded = true;
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
    udpSendTo(udpServer.sock, buf, sizeof(buf),
              &udpServer.clients[clientIdx].addr);
}

/* Send a player joined/left notification to all connected clients */
static void serverBroadcastPlayerEvent(uint8_t eventType, uint8_t playerNum,
                                       const char *playerName) {
    uint8_t buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 2];
    int i;
    int pktLen = PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 2;

    packHeader(buf, eventType, 0);
    buf[PACKET_HEADER_SIZE] = playerNum;
    memset(buf + PACKET_HEADER_SIZE + 1, 0, PACKET_MAX_PLAYER_NAME);
    if (playerName != NULL) {
        strncpy((char *)(buf + PACKET_HEADER_SIZE + 1), playerName,
                PACKET_MAX_PLAYER_NAME - 1);
    }
    /* Append country code */
    buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME] =
        (uint8_t)udpServer.clients[playerNum].countryCode[0];
    buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME + 1] =
        (uint8_t)udpServer.clients[playerNum].countryCode[1];

    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, pktLen, &udpServer.clients[i].addr);
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
    packU32(acceptBuf + pos, sim->tick);
    pos += 4;
    acceptBuf[pos++] = (uint8_t)gameTypeGet(&sim->sim.game);
    acceptBuf[pos++] = sim->sim.hiddenMines ? 1 : 0;
    packU32(acceptBuf + pos, (uint32_t)sim->startDelay);
    pos += 4;
    packU32(acceptBuf + pos, (uint32_t)sim->gameLength);
    pos += 4;
    packU32(acceptBuf + pos, udpServer.compressedMapSize);
    pos += 4;

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

/* Forward declarations for lobby broadcast helpers (defined after serverRecv) */
static void transportUdpServerSendLobbyStateToClient(ServerSim *sim, int clientIdx);
void transportUdpServerBroadcastLobbyUpdate(ServerSim *sim, BYTE playerNum);
static void serverSendServerMessage(langid id, int argCount,
                                    const char *const args[]);
static void serverSendServerEnglishBroadcast(const char *message);

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
    char originalName[PACKET_MAX_PLAYER_NAME];
    int j;

    strncpy(originalName, udpServer.clients[victimSlot].playerName,
            PACKET_MAX_PLAYER_NAME - 1);
    originalName[PACKET_MAX_PLAYER_NAME - 1] = '\0';

    /* Update the per-slot transport-side name. */
    strncpy(udpServer.clients[victimSlot].playerName, chosenName,
            PACKET_MAX_PLAYER_NAME - 1);
    udpServer.clients[victimSlot].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    udpServer.clients[victimSlot].nameStickySuffix = true;

    /* Update the gameSim player record — same path PACKET_NAME_CHANGE
     * already uses (transport_udp_server.c PACKET_NAME_CHANGE handler). */
    {
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        strncpy(nameBuf, chosenName, PACKET_MAX_PLAYER_NAME - 1);
        nameBuf[PACKET_MAX_PLAYER_NAME - 1] = '\0';
        playersSetPlayerName(NULL, &sim->sim, &sim->sim.plyrs, NEUTRAL,
                             (BYTE)victimSlot, nameBuf, TRUE);
    }

    /* Broadcast PACKET_NAME_CHANGE to all connected clients (including
     * the victim — they need to update their own record too). */
    {
        uint8_t outBuf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME];
        packHeader(outBuf, PACKET_NAME_CHANGE, 0);
        outBuf[PACKET_HEADER_SIZE] = (uint8_t)victimSlot;
        memset(outBuf + PACKET_HEADER_SIZE + 1, 0, PACKET_MAX_PLAYER_NAME);
        strncpy((char *)(outBuf + PACKET_HEADER_SIZE + 1), chosenName,
                PACKET_MAX_PLAYER_NAME - 1);
        for (j = 0; j < MAX_TANKS; j++) {
            if (udpServer.clients[j].connected) {
                udpSendTo(udpServer.sock, outBuf, sizeof(outBuf),
                          &udpServer.clients[j].addr);
            }
        }
    }

    /* Broadcast a single-slot lobby update so other surfaces (lobby
     * table, players panel) refresh. */
    transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)victimSlot);

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
        args.playerFlags = playersGetAccountFlags(&sim->sim.plyrs,
                                                  (BYTE)victimSlot);
        playersGetCountryCode(&sim->sim.plyrs, (BYTE)victimSlot,
                              args.playerCountry);
        /* Incoming joiner has a slot allocated but isn't in the players
         * struct yet; pull country from the caller-supplied lookup and
         * mark the WBN flag manually since we only get here when they're
         * verified. */
        args.otherFlags = (uint8_t)MESSAGE_FLAG_WBN;
        if (incomingCountry) {
            args.otherCountry[0] = incomingCountry[0];
            args.otherCountry[1] = incomingCountry[1];
            args.otherCountry[2] = '\0';
        }
        sim->sim.callbacks.messageAdd(sim->sim.callbacks.ctx,
                                      globalMessage, MESSAGE_NEWSWIRE,
                                      STR_NAME_RENAMED_BY_VERIFIED, &args);
    }

    /* Phase 5.1: surface the announcement to clients via the server-message
     * broadcast path (the messageAdd callback above drops on the server). */
    {
        const char *renameArgs[2];
        renameArgs[0] = originalName;
        renameArgs[1] = incomingName;
        serverSendServerMessage(STR_NAME_RENAMED_BY_VERIFIED, 2, renameArgs);
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
    if (len < pos + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "join request malformed: need=%d got=%d from=%s:%u",
            pos + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3, len,
            inet_ntoa(fromAddr->sin_addr),
            (unsigned)ntohs(fromAddr->sin_port));
        fprintf(stderr, "[UDP SERVER] Join request malformed (need %d, got %d)\n",
                pos + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3, len);
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
    if (wbnToken[0] != '\0' && winbolonetIsRunning()) {
        char errorMsg[512];
        errorMsg[0] = '\0';
        if (winbolonetServerVerifyToken(wbnToken, (BYTE)slot, errorMsg, &wbnHasSteam)) {
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
                playersGetWbnParticipant(&sim->sim.plyrs, (BYTE)i);

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
    strncpy(udpServer.clients[slot].playerName, name,
            PACKET_MAX_PLAYER_NAME - 1);
    udpServer.clients[slot].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    udpServer.clients[slot].lastReceivedTick = udpServer.tickCount;
    udpServer.clients[slot].outSequence = 1;
    udpServer.clients[slot].lastPingTime = udpServer.tickCount;
    udpServer.clients[slot].pingMs = 0;
    udpServer.clients[slot].wantRejoin = wantRejoin;

    /* Persist the early GeoIP lookup result. */
    udpServer.clients[slot].countryCode[0] = incomingCountry[0];
    udpServer.clients[slot].countryCode[1] = incomingCountry[1];
    udpServer.clients[slot].countryCode[2] = '\0';

    /* Initialize reliable event queues for this client */
    udpServer.eventQueues[slot].nextSeq = 1;
    udpServer.eventQueues[slot].ackedSeq = 1;
    memset(udpServer.eventQueues[slot].buffer, 0, sizeof(udpServer.eventQueues[slot].buffer));
    udpServer.mapEventQueues[slot].nextSeq = 1;
    udpServer.mapEventQueues[slot].ackedSeq = 1;
    memset(udpServer.mapEventQueues[slot].buffer, 0, sizeof(udpServer.mapEventQueues[slot].buffer));

    /* Set WBN/Steam participant flags before serverSimAddPlayer so the
     * log_PlayerJoined event recorded inside it captures the correct
     * accountFlags byte. Always written (cleared when not WBN) so a
     * recycled slot doesn't inherit a previous occupant's flags. */
    playersSetWbnParticipant(&sim->sim.plyrs, (BYTE)slot, incomingIsWBN);
    playersSetSteamParticipant(&sim->sim.plyrs, (BYTE)slot,
                               incomingIsWBN ? wbnHasSteam : FALSE);
    if (incomingIsWBN) {
        WB_LOG_INFO(WB_LOG_CAT_NET, "[WBN] Set player %d wbn=1 steam=%d", slot, wbnHasSteam ? 1 : 0);
    }

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

    /* Notify all players about the new player */
    serverBroadcastPlayerEvent(PACKET_PLAYER_JOINED, (uint8_t)slot, name);
    winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                       (BYTE)slot, WINBOLO_NET_NO_PLAYER);

    /* Send lobby state or game start signal BEFORE map chunks so the
     * client enters the lobby immediately and downloads the map in the
     * background (with the lobby progress bar visible). */
    if (sim->state == serverStateLobby || sim->state == serverStateCountdown) {
        /* Send full lobby snapshot to the joining client */
        transportUdpServerSendLobbyStateToClient(sim, slot);
        /* Broadcast lobby update to existing clients about the new player */
        transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)slot);
        /* Dismiss any pending balance proposal — player composition changed */
        if (sim->balanceProposal.pending) {
            uint8_t zeros[MAX_TANKS];
            memset(zeros, 0, sizeof(zeros));
            memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
            transportUdpServerBroadcastBalanceProposal(sim, zeros);
        }
    } else if (sim->state == serverStateRunning) {
        /* No-lobby mode or mid-game join: send game start signal */
        uint8_t startBuf[PACKET_HEADER_SIZE];
        packHeader(startBuf, PACKET_GAME_START,
                   udpServer.clients[slot].outSequence++);
        udpSendTo(udpServer.sock, startBuf, sizeof(startBuf),
                  &udpServer.clients[slot].addr);
    }

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
    if (sim->state != serverStateRunning) {
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
        if (pkt.tick > sim->lastProcessedInput[clientIdx]) {
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

    /* Pack tank snapshots */
    for (i = 0; i < hdr.tankCount; i++) {
        if (pos + TANK_SNAPSHOT_WIRE_SIZE > (int)sizeof(buf)) {
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

    udpSendTo(udpServer.sock, buf, pos, &client->addr);
    if (sim->tick % 50 == 0) {
        fprintf(stderr, "[UDP SERVER] Send SNAPSHOT to slot %d: tick=%u tanks=%u shells=%u bases=%u pills=%u events=%u(ack=%u next=%u) mapEvts=%u(ack=%u next=%u) len=%d\n",
                clientIdx, sim->tick, (unsigned)hdr.tankCount, (unsigned)hdr.shellCount,
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

    serverBroadcastPlayerEvent(PACKET_PLAYER_LEFT,
                               (uint8_t)idx,
                               udpServer.clients[idx].playerName);
    udpServer.clients[idx].connected = false;
    udpServer.clients[idx].nameStickySuffix = false;
    memset(udpServer.clients[idx].playerName, 0, PACKET_MAX_PLAYER_NAME);
    udpServer.clientLocked[idx] = false;
}

/* Send a localized server-originated message to all connected clients
 * via PACKET_CHAT_BROADCAST.  Uses fromPlayer=0xFF, destPlayer=0xFF to
 * mark the localized variant; client decodes langid + args and renders
 * via langGetTextFmt. */
static void serverSendServerMessage(langid id, int argCount,
                                    const char *const args[]) {
    /* 8 hdr + 1 from + 1 dest + 2 langid + 1 argCount + 4*(1+64) = 273. */
    uint8_t outBuf[PACKET_HEADER_SIZE + 2 + 3 + 4 * (1 + PLAYER_NAME_LEN - 1)];
    int pos = PACKET_HEADER_SIZE;
    int j;

    packHeader(outBuf, PACKET_CHAT_BROADCAST, 0);
    outBuf[pos++] = 0xFF;     /* fromPlayer = server localized */
    outBuf[pos++] = 0xFF;     /* destPlayer  = all */
    if (!packLocalizedPayload(outBuf, &pos, sizeof(outBuf), id, argCount, args)) {
        fprintf(stderr,
                "[UDP SERVER] serverSendServerMessage: pack failed id=%u argc=%d\n",
                (unsigned)id, argCount);
        return;
    }

    for (j = 0; j < MAX_TANKS; j++) {
        if (udpServer.clients[j].connected) {
            udpSendTo(udpServer.sock, outBuf, pos,
                      &udpServer.clients[j].addr);
        }
    }
}

/* Send a raw English server-originated message to all connected clients.
 * Uses fromPlayer=0xFE to mark the legacy English variant — used by
 * server-ops broadcasts (admin "say", lock toggle, ping enforcement)
 * that don't yet have dedicated langids.  As individual messages are
 * localized they should migrate to serverSendServerMessage above. */
static void serverSendServerEnglishBroadcast(const char *message) {
    uint8_t outBuf[PACKET_HEADER_SIZE + 2 + 200];
    int msgLen = (int)strlen(message);
    int outLen;
    int j;

    if (msgLen > 200) msgLen = 200;
    packHeader(outBuf, PACKET_CHAT_BROADCAST, 0);
    outBuf[PACKET_HEADER_SIZE]     = 0xFE; /* fromPlayer = server raw English */
    outBuf[PACKET_HEADER_SIZE + 1] = 0xFF; /* destPlayer = all */
    memcpy(outBuf + PACKET_HEADER_SIZE + 2, message, msgLen);
    outLen = PACKET_HEADER_SIZE + 2 + msgLen;

    for (j = 0; j < MAX_TANKS; j++) {
        if (udpServer.clients[j].connected) {
            udpSendTo(udpServer.sock, outBuf, outLen,
                      &udpServer.clients[j].addr);
        }
    }
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
            serverSendServerMessage(STR_KICK_ANNOUNCE, 1, kickArgs);
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (sim->lobbyEnabled &&
                (sim->state == serverStateLobby || sim->state == serverStateCountdown)) {
                transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)i);
            }
            return;
        }
    }
    serverSimConsoleMessage("Player not found.");
}

void transportUdpServerEnforcePing(ServerSim *sim) {
    int i;
    if (sim->state != serverStateRunning) return;

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
                serverSendServerEnglishBroadcast(msg);
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
                serverSendServerEnglishBroadcast(msg);
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

    udpServer.sock = createUdpSocket();
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
    sim->serverPort = port;
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

    /* Broadcast shutdown packet to all connected clients before closing */
    if (udpServer.sock != INVALID_SOCKET) {
        uint8_t shutdownBuf[PACKET_HEADER_SIZE];
        packHeader(shutdownBuf, PACKET_SERVER_SHUTDOWN, 0);
        for (i = 0; i < MAX_TANKS; i++) {
            if (udpServer.clients[i].connected) {
                udpSendTo(udpServer.sock, shutdownBuf, PACKET_HEADER_SIZE,
                          &udpServer.clients[i].addr);
            }
        }
        closesocket(udpServer.sock);
        udpServer.sock = INVALID_SOCKET;
    }
    for (i = 0; i < MAX_TANKS; i++) {
        udpServer.clients[i].connected = false;
        udpServer.clients[i].nameStickySuffix = false;
        serverCleanupMapDownload(i);
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
        pkt.gameid.serverport = sim->serverPort;
    }
    pkt.gameid.start_time = htonl(sim->timeCreated);

    /* Map name as Pascal string */
    utilCtoPString(sim->mapName, pkt.mapname);

    /* Game settings */
    pkt.gametype = (BYTE)sim->sim.game;
    pkt.allow_mines = sim->sim.hiddenMines ? HIDDEN_MINES : ALL_MINES_VISIBLE;
    pkt.allow_AI = 0;  /* AI type not tracked in new sim — report as none */
    pkt.spare1 = 0;
    pkt.start_delay = sim->startDelay;
    pkt.time_limit = sim->gameLength;

    /* Count connected players */
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) numPlayers++;
    }
    pkt.num_players = numPlayers;

    /* Neutral pills and bases */
    pkt.free_pills = pillsGetNumNeutral(&sim->sim.pb);
    pkt.free_bases = basesGetNumNeutral(&sim->sim.bs);

    pkt.has_password = udpServer.password[0] != '\0' ? 1 : 0;
    pkt.spare2 = 0;

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

/* ---- Lobby broadcast helpers ---- */

/* Lobby state wire format (variable length per slot):
 *   [header 8] [serverState 1]
 *   For each of 16 slots:
 *     [connected 1]
 *     If connected:
 *       [nameLen 1] [name nameLen UTF-8 bytes (no NUL)] [teamNumber 1]
 *       [ready 1] [isBot 1] [pingMs 2 (big-endian)] [countryCode 2]
 *       [wbn 1] [steam 1]   (11 + nameLen bytes for the slot)
 *   Game settings tail:
 *     [mapName 36] [gameType 1] [hiddenMines 1] [aiType 1]
 *     [gameLength 4 (big-endian)] [pillCount 1] [baseCount 1] [startCount 1]
 *     [mapSkipAvailable 1]
 * Returns the number of bytes written into buf.
 */

static int serverBuildLobbyStatePayload(ServerSim *sim, uint8_t *buf) {
    int pos = 0;
    int i;
    buf[pos++] = (uint8_t)sim->state;
    for (i = 0; i < MAX_TANKS; i++) {
        bool connected = sim->playerConnected[i];
        buf[pos++] = connected ? 1 : 0;
        if (!connected) {
            continue;
        }
        {
            const char *name = udpServer.clients[i].playerName;
            size_t nameLen = strnlen(name, PACKET_MAX_PLAYER_NAME - 1);
            buf[pos++] = (uint8_t)nameLen;
            if (nameLen > 0) {
                memcpy(buf + pos, name, nameLen);
                pos += (int)nameLen;
            }
        }
        buf[pos++] = sim->lobbyPlayers[i].teamNumber;
        buf[pos++] = sim->lobbyPlayers[i].ready ? 1 : 0;
        buf[pos++] = sim->lobbyPlayers[i].isBot ? 1 : 0;
        /* Ping (big-endian) */
        buf[pos++] = (uint8_t)(udpServer.clients[i].pingMs >> 8);
        buf[pos++] = (uint8_t)(udpServer.clients[i].pingMs & 0xFF);
        /* Country code (2 chars, or zeros if unknown) */
        buf[pos++] = (uint8_t)udpServer.clients[i].countryCode[0];
        buf[pos++] = (uint8_t)udpServer.clients[i].countryCode[1];
        /* WBN/Steam participant flags */
        buf[pos++] = playersGetWbnParticipant(&sim->sim.plyrs, (BYTE)i) ? 1 : 0;
        buf[pos++] = playersGetSteamParticipant(&sim->sim.plyrs, (BYTE)i) ? 1 : 0;
    }
    /* Game settings tail */
    memset(buf + pos, 0, MAP_STR_SIZE);
    strncpy((char *)(buf + pos), sim->mapName, MAP_STR_SIZE - 1);
    pos += MAP_STR_SIZE;
    buf[pos++] = (uint8_t)gameTypeGet(&sim->sim.game);
    buf[pos++] = sim->sim.hiddenMines ? 1 : 0;
    buf[pos++] = (uint8_t)sim->botAiType;
    packU32(buf + pos, (uint32_t)sim->gameLength);
    pos += 4;
    buf[pos++] = pillsGetNumPills(&sim->sim.pb);
    buf[pos++] = basesGetNumBases(&sim->sim.bs);
    buf[pos++] = startsGetNumStarts(&sim->sim.ss);
    buf[pos++] = (sim->mapDirCount > 1 || sim->randomMapEnabled) ? 1 : 0;
    return pos;
}

void transportUdpServerBroadcastLobbyState(ServerSim *sim) {
    uint8_t buf[PACKET_HEADER_SIZE + LOBBY_STATE_PAYLOAD];
    int payloadLen;
    int sendLen;
    int i;
    packHeader(buf, PACKET_LOBBY_STATE, 0);
    payloadLen = serverBuildLobbyStatePayload(sim, buf + PACKET_HEADER_SIZE);
    sendLen = PACKET_HEADER_SIZE + payloadLen;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sendLen,
                      &udpServer.clients[i].addr);
        }
    }
}

static void transportUdpServerSendLobbyStateToClient(ServerSim *sim, int clientIdx) {
    uint8_t buf[PACKET_HEADER_SIZE + LOBBY_STATE_PAYLOAD];
    int payloadLen;
    packHeader(buf, PACKET_LOBBY_STATE, udpServer.clients[clientIdx].outSequence++);
    payloadLen = serverBuildLobbyStatePayload(sim, buf + PACKET_HEADER_SIZE);
    udpSendTo(udpServer.sock, buf, PACKET_HEADER_SIZE + payloadLen,
              &udpServer.clients[clientIdx].addr);
}

void transportUdpServerBroadcastLobbyUpdate(ServerSim *sim, BYTE playerNum) {
    uint8_t buf[PACKET_HEADER_SIZE + LOBBY_UPDATE_PAYLOAD];
    int pos = PACKET_HEADER_SIZE;
    bool connected = sim->playerConnected[playerNum];
    int i;

    packHeader(buf, PACKET_LOBBY_UPDATE, 0);
    buf[pos++] = playerNum;
    buf[pos++] = connected ? 1 : 0;
    if (connected) {
        const char *name = udpServer.clients[playerNum].playerName;
        size_t nameLen = strnlen(name, PACKET_MAX_PLAYER_NAME - 1);
        buf[pos++] = (uint8_t)nameLen;
        if (nameLen > 0) {
            memcpy(buf + pos, name, nameLen);
            pos += (int)nameLen;
        }
        buf[pos++] = sim->lobbyPlayers[playerNum].teamNumber;
        buf[pos++] = sim->lobbyPlayers[playerNum].ready ? 1 : 0;
        buf[pos++] = sim->lobbyPlayers[playerNum].isBot ? 1 : 0;
        buf[pos++] = (uint8_t)(udpServer.clients[playerNum].pingMs >> 8);
        buf[pos++] = (uint8_t)(udpServer.clients[playerNum].pingMs & 0xFF);
        buf[pos++] = (uint8_t)udpServer.clients[playerNum].countryCode[0];
        buf[pos++] = (uint8_t)udpServer.clients[playerNum].countryCode[1];
        buf[pos++] = playersGetWbnParticipant(&sim->sim.plyrs, playerNum) ? 1 : 0;
        buf[pos++] = playersGetSteamParticipant(&sim->sim.plyrs, playerNum) ? 1 : 0;
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, pos, &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerBroadcastCountdown(ServerSim *sim, uint8_t secondsRemaining) {
    uint8_t buf[PACKET_HEADER_SIZE + 1];
    int i;
    packHeader(buf, PACKET_COUNTDOWN, 0);
    buf[PACKET_HEADER_SIZE] = secondsRemaining;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerBroadcastGameStart(ServerSim *sim) {
    uint8_t buf[PACKET_HEADER_SIZE];
    int i;
    (void)sim;
    packHeader(buf, PACKET_GAME_START, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
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

void transportUdpServerBroadcastGameOver(ServerSim *sim) {
    uint8_t buf[PACKET_HEADER_SIZE];
    int i;
    (void)sim;
    packHeader(buf, PACKET_GAME_OVER, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerNotifyMapChange(ServerSim *sim) {
    int i;
    int mapLen;
    uint8_t notifyBuf[PACKET_HEADER_SIZE];

    /* 1. Refresh the server's compressed map from the sim */
    mapLen = serverSimGetCompressedMap(sim, udpServer.compressedMap);
    if (mapLen <= 0) {
        fprintf(stderr, "[UDP SERVER] Map change: failed to compress new map\n");
        return;
    }
    udpServer.compressedMapSize = (uint32_t)mapLen;

    /* 2. Send PACKET_LOBBY_MAP_CHANGE to all connected clients */
    packHeader(notifyBuf, PACKET_LOBBY_MAP_CHANGE, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        udpSendTo(udpServer.sock, notifyBuf, sizeof(notifyBuf),
                  &udpServer.clients[i].addr);

        /* 3. Re-send join accept so client gets the new map size */
        serverSendJoinAccept(i, sim, &udpServer.clients[i].addr);

        /* 4. Reset map download tracking and start sending new chunks */
        serverInitMapDownload(i);
    }

    fprintf(stderr, "[UDP SERVER] Map change broadcast: %u bytes compressed map\n",
            udpServer.compressedMapSize);
}

void transportUdpServerBroadcastBalanceProposal(ServerSim *sim, uint8_t teamForSlot[MAX_TANKS]) {
    uint8_t buf[PACKET_HEADER_SIZE + MAX_TANKS];
    int i;
    (void)sim;
    packHeader(buf, PACKET_BALANCE_PROPOSAL, 0);
    memcpy(buf + PACKET_HEADER_SIZE, teamForSlot, MAX_TANKS);
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerBroadcastMapSkipState(ServerSim *sim) {
    uint8_t buf[PACKET_HEADER_SIZE + MAX_TANKS];
    int i;
    packHeader(buf, PACKET_MAP_SKIP_STATE, 0);
    for (i = 0; i < MAX_TANKS; i++) {
        buf[PACKET_HEADER_SIZE + i] = sim->mapSkipVotes[i] ? 1 : 0;
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            udpSendTo(udpServer.sock, buf, sizeof(buf),
                      &udpServer.clients[i].addr);
        }
    }
}

void transportUdpServerSetBotName(BYTE playerNum, const char *name) {
    if (playerNum >= MAX_TANKS) return;
    strncpy(udpServer.clients[playerNum].playerName, name,
            PACKET_MAX_PLAYER_NAME - 1);
    udpServer.clients[playerNum].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
}

const char *transportUdpServerGetPlayerName(BYTE playerNum) {
    if (playerNum >= MAX_TANKS || !udpServer.clients[playerNum].connected) {
        return NULL;
    }
    return udpServer.clients[playerNum].playerName;
}

/* Send an INFO_RESPONSE packet to the tracker so the game is listed. */
void transportUdpServerSendTrackerUpdate(ServerSim *sim,
                                         const char *trackerAddr,
                                         unsigned short trackerPort) {
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
        pkt.gameid.serverport = sim->serverPort;
    }
    pkt.gameid.start_time = htonl(sim->timeCreated);

    utilCtoPString(sim->mapName, pkt.mapname);

    pkt.gametype = (BYTE)sim->sim.game;
    pkt.allow_mines = sim->sim.hiddenMines ? HIDDEN_MINES : ALL_MINES_VISIBLE;
    pkt.allow_AI = 0;
    pkt.spare1 = 0;
    pkt.start_delay = sim->startDelay;
    pkt.time_limit = sim->gameLength;

    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) numPlayers++;
    }
    pkt.num_players = numPlayers;
    pkt.free_pills = pillsGetNumNeutral(&sim->sim.pb);
    pkt.free_bases = basesGetNumNeutral(&sim->sim.bs);
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
    /* 4-byte game token = sim->timeCreated, big-endian. Tracker's exact-
     * match path uses (sourceIp, starttime) so multiple games behind one
     * NAT each refresh their own entry. */
    packU32(buf + 4, (uint32_t)sim->timeCreated);

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

                /* Build broadcast packet:
                 *   [header 8] [fromPlayer 1] [destPlayer 1] [message] */
                {
                    uint8_t outBuf[PACKET_HEADER_SIZE + 2 + PACKET_MAX_CHAT_MESSAGE];
                    int outLen;
                    packHeader(outBuf, PACKET_CHAT_BROADCAST, 0);
                    outBuf[PACKET_HEADER_SIZE] = (uint8_t)clientIdx;
                    outBuf[PACKET_HEADER_SIZE + 1] = destPlayer;
                    memcpy(outBuf + PACKET_HEADER_SIZE + 2,
                           buf + PACKET_HEADER_SIZE + 1, msgLen);
                    outLen = PACKET_HEADER_SIZE + 2 + msgLen;

                    if (destPlayer == 0xFF) {
                        /* Broadcast to all connected clients except sender */
                        int j;
                        for (j = 0; j < MAX_TANKS; j++) {
                            if (udpServer.clients[j].connected && j != clientIdx) {
                                udpSendTo(udpServer.sock, outBuf, outLen,
                                          &udpServer.clients[j].addr);
                            }
                        }
                    } else {
                        /* Send to specific player only */
                        if (destPlayer < MAX_TANKS &&
                            udpServer.clients[destPlayer].connected) {
                            udpSendTo(udpServer.sock, outBuf, outLen,
                                      &udpServer.clients[destPlayer].addr);
                        }
                    }
                    /* Log chat message */
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
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Player '%s' changed name to '%s'.",
                             udpServer.clients[clientIdx].playerName, newName);
                    serverSimConsoleMessage(msg);

                    /* Update server-side name */
                    strncpy(udpServer.clients[clientIdx].playerName, newName,
                            PACKET_MAX_PLAYER_NAME - 1);
                    udpServer.clients[clientIdx].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';

                    /* Update in players struct */
                    {
                        ServerSim *ssim = serverSimGetActive();
                        playersSetPlayerName(NULL, &ssim->sim, &ssim->sim.plyrs, NEUTRAL, (BYTE)clientIdx, newName, TRUE);
                    }

                    /* Broadcast to all other clients */
                    {
                        uint8_t outBuf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME];
                        packHeader(outBuf, PACKET_NAME_CHANGE, 0);
                        outBuf[PACKET_HEADER_SIZE] = (uint8_t)clientIdx;
                        memset(outBuf + PACKET_HEADER_SIZE + 1, 0,
                               PACKET_MAX_PLAYER_NAME);
                        strncpy((char *)(outBuf + PACKET_HEADER_SIZE + 1),
                                newName, PACKET_MAX_PLAYER_NAME - 1);
                        for (j = 0; j < MAX_TANKS; j++) {
                            if (udpServer.clients[j].connected &&
                                j != clientIdx) {
                                udpSendTo(udpServer.sock, outBuf,
                                          sizeof(outBuf),
                                          &udpServer.clients[j].addr);
                            }
                        }
                    }
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
                if (sim->lobbyEnabled &&
                    (sim->state == serverStateLobby || sim->state == serverStateCountdown)) {
                    transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
                    /* Dismiss any pending balance proposal — player composition changed */
                    if (sim->balanceProposal.pending) {
                        uint8_t zeros[MAX_TANKS];
                        memset(zeros, 0, sizeof(zeros));
                        memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
                        transportUdpServerBroadcastBalanceProposal(sim, zeros);
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
                serverSendServerEnglishBroadcast(msg);

                /* Check if consensus lock state changed */
                {
                    bool nowLocked = serverClientsAllLocked();
                    if (nowLocked && !wasLocked) {
                        serverSendServerEnglishBroadcast(
                            "This game is now locked to new players (client lock)");
                        serverSimConsoleMessage(
                            "Game locked by client consensus.");
                        winboloNetSendLock(TRUE);
                    } else if (!nowLocked && wasLocked) {
                        serverSendServerEnglishBroadcast(
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
                /* Forward as ALLIANCE_UPDATE (REQUEST) to target player only */
                if (toPlayer < MAX_TANKS &&
                    udpServer.clients[toPlayer].connected) {
                    uint8_t outBuf[PACKET_HEADER_SIZE + 3];
                    packHeader(outBuf, PACKET_ALLIANCE_UPDATE, 0);
                    outBuf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_REQUEST;
                    outBuf[PACKET_HEADER_SIZE + 1] = (uint8_t)clientIdx;
                    outBuf[PACKET_HEADER_SIZE + 2] = toPlayer;
                    udpSendTo(udpServer.sock, outBuf, sizeof(outBuf),
                              &udpServer.clients[toPlayer].addr);
                    fprintf(stderr, "[UDP SERVER] Alliance update forwarded to player %d\n", toPlayer);
                    logAddEvent(log_AllyRequest, (BYTE)clientIdx, toPlayer, 0, 0, 0, NULL);
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
                /* Apply alliance on server-side players struct */
                ServerSim *ssim = serverSimGetActive();
                playersAcceptAlliance(&ssim->sim, &ssim->sim.plyrs, NEUTRAL,
                                     (BYTE)clientIdx, newMember, TRUE);
                winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_JOIN, TRUE,
                                   (BYTE)clientIdx, newMember);
                logAddEvent(log_AllyAccept, (BYTE)clientIdx, newMember, 0, 0, 0, NULL);
                /* Broadcast ALLIANCE_UPDATE (ACCEPT) to all clients */
                {
                    uint8_t outBuf[PACKET_HEADER_SIZE + 3];
                    int j;
                    packHeader(outBuf, PACKET_ALLIANCE_UPDATE, 0);
                    outBuf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_ACCEPT;
                    outBuf[PACKET_HEADER_SIZE + 1] = (uint8_t)clientIdx;
                    outBuf[PACKET_HEADER_SIZE + 2] = newMember;
                    for (j = 0; j < MAX_TANKS; j++) {
                        if (udpServer.clients[j].connected) {
                            udpSendTo(udpServer.sock, outBuf, sizeof(outBuf),
                                      &udpServer.clients[j].addr);
                        }
                    }
                }
            }
            break;
        }
        case PACKET_ALLIANCE_LEAVE: {
            /* Wire: [header 8] [playerNum 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && len >= PACKET_HEADER_SIZE + 1) {
                /* Apply on server-side players struct */
                {
                    ServerSim *ssim = serverSimGetActive();
                    playersLeaveAlliance(&ssim->sim, &ssim->sim.plyrs, NEUTRAL, (BYTE)clientIdx, TRUE);
                }
                winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_LEAVE, TRUE,
                                   (BYTE)clientIdx, WINBOLO_NET_NO_PLAYER);
                logAddEvent(log_AllyLeave, (BYTE)clientIdx, 0, 0, 0, 0, NULL);
                /* Broadcast ALLIANCE_UPDATE (LEAVE) to all clients */
                {
                    uint8_t outBuf[PACKET_HEADER_SIZE + 3];
                    int j;
                    packHeader(outBuf, PACKET_ALLIANCE_UPDATE, 0);
                    outBuf[PACKET_HEADER_SIZE] = ALLIANCE_EVENT_LEAVE;
                    outBuf[PACKET_HEADER_SIZE + 1] = (uint8_t)clientIdx;
                    outBuf[PACKET_HEADER_SIZE + 2] = 0; /* unused for leave */
                    for (j = 0; j < MAX_TANKS; j++) {
                        if (udpServer.clients[j].connected) {
                            udpSendTo(udpServer.sock, outBuf, sizeof(outBuf),
                                      &udpServer.clients[j].addr);
                        }
                    }
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
        case PACKET_LOBBY_TEAM_SET: {
            /* Wire: [header 8] [playerNum 1] [teamNumber 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                len >= PACKET_HEADER_SIZE + 2) {
                uint8_t teamNum = buf[PACKET_HEADER_SIZE + 1];
                if (teamNum <= 16) {
                    sim->lobbyPlayers[clientIdx].teamNumber = teamNum;
                    logAddEvent(log_TeamSet, (BYTE)clientIdx, teamNum, 0, 0, 0, NULL);
                    transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
                }
            }
            break;
        }
        case PACKET_LOBBY_READY: {
            /* Wire: [header 8] [playerNum 1] [ready 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && sim->lobbyEnabled &&
                len >= PACKET_HEADER_SIZE + 2) {
                bool ready = buf[PACKET_HEADER_SIZE + 1] != 0;

                if (sim->state == serverStateLobby) {
                    sim->lobbyPlayers[clientIdx].ready = ready;
                    logAddEvent(ready ? log_PlayerReady : log_PlayerUnready, (BYTE)clientIdx, 0, 0, 0, 0, NULL);
                    transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
                    serverSimLobbyCheckAllReady(sim);
                    /* If all-ready check triggered countdown, broadcast it */
                    if (sim->state == serverStateCountdown) {
                        logAddEvent(log_CountdownStart, 0, 0, 0, 0, 0, NULL);
                        uint8_t secs = (uint8_t)(sim->countdownTicks / 50);
                        transportUdpServerBroadcastCountdown(sim, secs);
                    }
                } else if (sim->state == serverStateCountdown && !ready) {
                    /* Someone unreadied during countdown — revert to lobby */
                    sim->lobbyPlayers[clientIdx].ready = FALSE;
                    sim->state = serverStateLobby;
                    sim->countdownTicks = 0;
                    logAddEvent(log_PlayerUnready, (BYTE)clientIdx, 0, 0, 0, 0, NULL);
                    logAddEvent(log_CountdownCancel, 0, 0, 0, 0, 0, NULL);
                    serverSimConsoleMessage("Countdown cancelled — player unreadied.");
                    transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
                }
            }
            break;
        }
        case PACKET_LOBBY_ADD_BOT: {
            /* Wire: [header 8] — no payload needed, server uses its own brain config */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                sim->botAiType != aiNone &&
                sim->botBrainPath[0] != '\0') {
                /* Find first free player slot */
                BYTE slot;
                bool found = false;
                for (slot = 0; slot < MAX_TANKS; slot++) {
                    if (!sim->playerConnected[slot]) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    char botName[64];
                    snprintf(botName, sizeof(botName), "Bot %d", slot + 1);
                    if (botManagerAddBot(sim, slot, sim->botBrainPath, botName,
                                         sim->botAiType,
                                         gameTypeGet(&sim->sim.game),
                                         sim->sim.hiddenMines)) {
                        transportUdpServerBroadcastLobbyUpdate(sim, slot);
                    }
                }
            }
            break;
        }
        case PACKET_LOBBY_REMOVE_BOT: {
            /* Wire: [header 8] [playerNum 1] */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                len >= PACKET_HEADER_SIZE + 1) {
                uint8_t targetSlot = buf[PACKET_HEADER_SIZE];
                if (targetSlot < MAX_TANKS && botManagerIsBot(targetSlot)) {
                    botManagerRemoveBot(sim, targetSlot);
                    transportUdpServerBroadcastLobbyUpdate(sim, targetSlot);
                }
            }
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
                    errorMsg[0] = '\0';
                    if (winbolonetServerVerifyToken(token, (BYTE)clientIdx, errorMsg, &hasSteam)) {
                        playersSetWbnParticipant(&sim->sim.plyrs, (BYTE)clientIdx, TRUE);
                        playersSetSteamParticipant(&sim->sim.plyrs, (BYTE)clientIdx, hasSteam);
                        fprintf(stderr, "[UDP SERVER] Player %d WBN re-authenticated (steam=%d)\n",
                                clientIdx, hasSteam ? 1 : 0);
                        /* If game is already running, send the join event now */
                        if (sim->state == serverStateRunning) {
                            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                               (BYTE)clientIdx, WINBOLO_NET_NO_PLAYER);
                        }
                        /* Broadcast updated flags so other clients see WBN badge */
                        if (sim->state == serverStateLobby || sim->state == serverStateCountdown) {
                            transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)clientIdx);
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
            if (clientIdx == 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                len >= PACKET_HEADER_SIZE + 1 &&
                !sim->balanceProposal.requestInFlight &&
                !sim->balanceProposal.pending &&
                winbolonetIsRunning()) {
                BalanceThreadData *btd = malloc(sizeof(BalanceThreadData));
                if (btd) {
                    SDL_Thread *t;
                    int i;
                    btd->sim = sim;
                    btd->teamSize = buf[PACKET_HEADER_SIZE];
                    btd->totalPlayers = 0;
                    for (i = 0; i < MAX_TANKS; i++) {
                        if (sim->playerConnected[i]) btd->totalPlayers++;
                    }
                    sim->balanceProposal.requestInFlight = true;
                    t = SDL_CreateThread(balanceThreadFunc, "WbnBalance", btd);
                    if (t) {
                        SDL_DetachThread(t);
                    } else {
                        sim->balanceProposal.requestInFlight = false;
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
            if (clientIdx == 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                sim->balanceProposal.pending) {
                int i;
                for (i = 0; i < MAX_TANKS; i++) {
                    if (sim->balanceProposal.teamForSlot[i] != 0) {
                        sim->lobbyPlayers[i].teamNumber = sim->balanceProposal.teamForSlot[i];
                    }
                }
                memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
                logAddEvent(log_BalanceApplied, 0, 0, 0, 0, 0, NULL);
                transportUdpServerBroadcastLobbyState(sim);
                serverSimConsoleMessage("Team balance applied");
            }
            break;
        }
        case PACKET_BALANCE_DISMISS: {
            /* Wire: [header 8] (no payload) */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx == 0 && sim->lobbyEnabled &&
                sim->state == serverStateLobby &&
                sim->balanceProposal.pending) {
                uint8_t zeros[MAX_TANKS];
                memset(zeros, 0, sizeof(zeros));
                memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
                transportUdpServerBroadcastBalanceProposal(sim, zeros);
            }
            break;
        }
        case PACKET_MAP_SKIP_VOTE: {
            /* Wire: [header 8] (no payload — server identifies player by source) */
            int clientIdx = serverFindClient(fromAddr);
            if (clientIdx >= 0) {
                serverSimMapSkipVoteToggle(sim, (uint8_t)clientIdx);
                transportUdpServerBroadcastMapSkipState(sim);
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
    if (sim->eventCount == 0 && sim->mapEventCount == 0) return;

    fprintf(stderr, "[UDP SERVER] Enqueuing %d events + %d map events from tick=%u\n",
            sim->eventCount, sim->mapEventCount, sim->tick);

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
            for (i = 0; i < (int)sim->mapEventCount; i++) {
                if (!eventQueueHasSpace(mq)) {
                    fprintf(stderr, "[UDP SERVER] Map event queue full for client %d, dropping %d events\n",
                            c, (int)sim->mapEventCount - i);
                    break;
                }
                uint32_t idx = mq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                mq->buffer[idx].event = sim->mapEvents[i];
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

        for (i = 0; i < (int)sim->eventCount; i++) {
            uint8_t evType = sim->events[i].type;
            if ((evType == EVENT_SOUND || evType == EVENT_SOUND_TANK_HIT || evType == EVENT_SOUND_SHOOT) && hasPos) {
                uint8_t soundId = sim->events[i].data[0];
                uint8_t mx = sim->events[i].data[1];
                uint8_t my = sim->events[i].data[2];
                int dx = (clientMX > mx) ? (clientMX - mx) : (mx - clientMX);
                int dy = (clientMY > my) ? (clientMY - my) : (my - clientMY);

                /* Skip own shoot sound — client plays shootSelf via prediction */
                if (evType == EVENT_SOUND_SHOOT && sim->events[i].data[3] == (uint8_t)c) {
                    continue;
                }

                /* Always send tank hit to the hit player (plays hitTankSelf at full volume) */
                if (evType == EVENT_SOUND_TANK_HIT && sim->events[i].data[3] == (uint8_t)c) {
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
        for (i = 0; i < (int)sim->eventCount; i++) {
            uint8_t evType = sim->events[i].type;
            if (evType != EVENT_SOUND && evType != EVENT_SOUND_TANK_HIT && evType != EVENT_SOUND_SHOOT) {
                /* Filter EVENT_MINE_VISIBLE: tank mines (bit 7 set) go to all,
                 * LGM mines go only to the placer and their allies */
                if (evType == EVENT_MINE_VISIBLE) {
                    BYTE sourcePlayer = sim->events[i].data[2];
                    if (sourcePlayer & 0x80) {
                        /* Tank mine — broadcast to all */
                    } else {
                        /* LGM mine — only placer and allies */
                        if (c != sourcePlayer && !playersIsAllie(&sim->sim.plyrs, (BYTE)c, sourcePlayer)) {
                            continue;
                        }
                    }
                }
                /* Distance-cull explosion events */
                if (evType == EVENT_EXPLOSION && hasPos) {
                    int dx = (clientMX > sim->events[i].data[0]) ? (clientMX - sim->events[i].data[0]) : (sim->events[i].data[0] - clientMX);
                    int dy = (clientMY > sim->events[i].data[1]) ? (clientMY - sim->events[i].data[1]) : (sim->events[i].data[1] - clientMY);
                    if (dx >= SDIST_NONE || dy >= SDIST_NONE) continue;
                }
                if (!eventQueueHasSpace(cq)) {
                    fprintf(stderr, "[UDP SERVER] Game event queue full for client %d\n", c);
                    break;
                }
                uint32_t idx = cq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                cq->buffer[idx].event = sim->events[i];
                cq->buffer[idx].seq = cq->nextSeq;
                cq->nextSeq++;
            }
        }
        for (s = 0; s < MAX_SOUND_TYPES; s++) {
            if (bestSoundIdx[s] >= 0) {
                if (!eventQueueHasSpace(cq)) break;
                uint32_t idx = cq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
                cq->buffer[idx].event = sim->events[bestSoundIdx[s]];
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
         *   [playerNum 1][name 32][numAllies 1][ally0 1][ally1 1]...
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
                if (!udpServer.clients[j].connected && !sim->playerConnected[j]) continue;

                numAllies = playersMakeNetAlliences(
                    &serverSimGetActive()->sim.plyrs, (BYTE)j, allies);

                /* Check we have room: 1 + 32 + 2 + 1 + numAllies */
                if (plPos + 1 + PACKET_MAX_PLAYER_NAME + 2 + 1 + numAllies
                    > (int)sizeof(plBuf))
                    break;

                plBuf[plPos++] = (uint8_t)j;
                memset(plBuf + plPos, 0, PACKET_MAX_PLAYER_NAME);
                /* Get name from players struct (works for both UDP clients and bots) */
                memset(playerName, 0, sizeof(playerName));
                playersGetPlayerName(&serverSimGetActive()->sim.plyrs, (BYTE)j, playerName, TRUE);
                strncpy((char *)(plBuf + plPos), playerName, PACKET_MAX_PLAYER_NAME - 1);
                plPos += PACKET_MAX_PLAYER_NAME;
                /* Country code (2 bytes) */
                plBuf[plPos++] = (uint8_t)udpServer.clients[j].countryCode[0];
                plBuf[plPos++] = (uint8_t)udpServer.clients[j].countryCode[1];
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
            if (sim->lobbyEnabled &&
                (sim->state == serverStateLobby || sim->state == serverStateCountdown)) {
                transportUdpServerBroadcastLobbyUpdate(sim, (BYTE)i);
                /* Dismiss any pending balance proposal — player composition changed */
                if (sim->balanceProposal.pending) {
                    uint8_t zeros[MAX_TANKS];
                    memset(zeros, 0, sizeof(zeros));
                    memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
                    transportUdpServerBroadcastBalanceProposal(sim, zeros);
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
     * yet, so route through the legacy English passthrough wire format. */
    serverSendServerEnglishBroadcast(message);
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
