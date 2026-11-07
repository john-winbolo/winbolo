/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Bot Host
 *Filename:      udp_server_bothost.c
 *Purpose:
 *  The server side of the Bot Host: a separate program that
 *  runs bot brains for this server on another machine.
 *
 *  Finding each other (bot_host_link.h): a Bot Host sends an
 *  ANNOUNCE to each server on its --servers list. The server
 *  takes ANNOUNCEs only from hosts on its -bothosts list (or,
 *  with no list, from any host, which must then give the
 *  -bothostkey key). It sends a QUERY to each listed host at
 *  start, after each game, and when a player joins an empty
 *  lobby; a host answers with an ANNOUNCE. To an ANNOUNCE with
 *  free slots, while the server wants bots run, it answers
 *  with an INVITE. The host then connects, so the connection
 *  always starts from the host's side. A host that holds no
 *  seats for 60 s while nothing wants bots is released.
 *
 *  A Bot Host opens one control connection (JOIN_FLAG_BOT_HOST
 *  with the -bothostkey key, when set). It holds no tank seat and no
 *  player ever sees it. Through it the server hands the host
 *  bot seats: CTRL_BOT_HOST_ASSIGN carries what the host needs
 *  to build the seat's brain and a one-time token, and the
 *  host then opens a bot connection for the seat
 *  (JOIN_FLAG_BOT_SLOT with the token). The bot connection is
 *  a normal client connection on the seat's own slot, with no
 *  roster join: the seat, its tank, its name and its score
 *  stay the bot manager's.
 *
 *  A seat's brain moves to the host at the first input the
 *  bot connection sends, so the tank is driven without a gap.
 *  When the host or the bot connection goes, the seat's brain
 *  comes back in-process without a word to the players.
 *
 *  Placement rule: the server itself counts as one host, with
 *  its own capacity answer as its slots and no limit. Each bot
 *  goes where (bots on the host + 1) / effective slots is
 *  lowest (botHostPlacePick in bot_host_link.c): every host is
 *  about equally full, and a host of 1 or 2 slots (-bothost-
 *  small-slots) counts -bothost-small-bias fewer slots, so it
 *  fills a little later. No host goes over its slots. Lag
 *  counts too (botHostPlaceRebalanceLag): the score is
 *  multiplied by 1 + lag / R (-bothost-lag-ref-ms, 100 ms; the
 *  server's lag is 0), a host over L (-bothost-lag-max-ms,
 *  250 ms) gets no new bot while one at or under L has room,
 *  and a bot moves only for a score at least 33% lower. In the
 *  lobby one bot moves each tick; in the game one bot moves at
 *  most each 3 s (each tick in the first 10 s of a game), and
 *  never a bot that carries a pill or has its builder out.
 *  A host's slots are shared by every server it serves, so it
 *  reports its count (CTRL_BOT_HOST_SLOTS) and turns down an
 *  assign that finds its pool full; that seat stays in-process.
 *  A seat that came back from a host on a fault is offered
 *  again only at the next round start or in the lobby.
 *
 *  The players' own Bot Hosts: unless -noplayerbothosts, each
 *  player is sent a ticket (CTRL_BOT_HOST_TICKET) made for that
 *  player's connection. The client then runs a Bot Host of its
 *  own (src/bothost/bothost_embed.h), which joins with the
 *  ticket in place of the key. The server takes it only from
 *  the player's own address while that connection lasts, one
 *  per player, and drops it when the player leaves: its bots
 *  come back in-process, and may go at once to another host.
 *********************************************************/

#include <stdio.h>   /* fprintf, snprintf, stderr, stdout */
#include <stdlib.h>  /* free */
#include <string.h>  /* memcpy, memset, memmove, strcmp, strlen */

#include <SDL3/SDL.h> /* SDL_LoadFile, SDL_crc32, SDL_free, SDL_strlcpy */

#include "transport_udp_internal.h"        /* packHeader, packU32, unpackU32,
                                            * packU16, packConnId, packGameEvent */
#include "transport_udp_server_internal.h" /* udpServer, srvSendTo, BotHostConn,
                                            * BotHostSeat, serverFindClient */
#include "transport_udp.h"   /* UdpServerClient, JOIN_BOT_*, MAX_BOT_HOSTS */
#include "netpacks.h"        /* PACKET_*, PACKET_HEADER_SIZE */
#include "global.h"          /* BYTE, MAX_TANKS */
#include "input_packet.h"    /* GameEvent, EVENT_MINE_VISIBLE */
#include "game_sim.h"        /* GameSim, and via lang.h the STR_ message ids */
#include "mines.h"           /* struct minesObj */
#include "players.h"         /* players, allie */
#include "server_sim.h"      /* ServerSim, serverSimGetState, serverSimIsBot */
#include "server_sim_internal.h" /* serverSimShadowSeed, serverSimSetShadowCulled,
                                  * serverSimGetCompressedMapFor */
#include "control_event.h"   /* ControlEvent, CTRL_BOT_* */
#include "client_command.h"  /* CmdBotStats */
#include "transport_control_codec.h" /* transportControlCodecBodyEncoder */
#include "channel_mux.h"     /* channelMuxInit, channelSend, channelTick,
                              * channelBuildFrame, channelRecvFrame */
#include "bulk_transfer.h"   /* bulkSenderInit, bulkSenderReset, bulkReceiverInit */
#include "brain_sync.h"      /* brainSyncBuildManifest, the Bot Host brain sync */
#include "bot_manager.h"     /* botManager*, BotRemoteHooks */
#include "brain_record.h"    /* brainRecordSetHostedInfoFn */
#include "../../common/wb_log.h" /* WB_LOG_* */
#include "tank.h"            /* tankGetNumCarriedPills */
#include "lgm.h"             /* lgmIsOut */
#include "brain_capacity.h"  /* brainCapacityDefault, brainCapacityGetMachine */
#include "bot_host_cpu.h"    /* BotHostCpuMeter, the lobby's CPU estimates */
#include "machine_cpu_free.h" /* MachineCpuFree, the server's free CPU */
#include "bothost_record.h" /* BotHostRecord, the log_BotHostData payload */
#include "log.h"            /* logAddEvent, logIsRecording, log_BotHostData */

/* Ticks without a datagram from a Bot Host before it is dropped. The host
 * pings every 500 ms. */
#define BOT_HOST_TIMEOUT_TICKS   250
/* Ticks a seat waits for its bot connection after the assign. */
#define BOT_SEAT_PENDING_TICKS   500
/* Ticks between checks of a remote seat's brain, AI level, team and init
 * argument against what its host was last sent. */
#define BOT_SEAT_POLL_TICKS      50
/* Mine reveals a full-view seat is sent per tick while its mines are seeded. */
#define BOT_SEAT_MINES_PER_TICK  32

/* The sim the hooks were installed on. */
static ServerSim *botHostSim = NULL;

/* The overrun check of the placement (botHostOvrProbeAll), one state per
 * place in the host table: 0 the server, h + 1 Bot Host h. */
static BotHostOverrunCfg   botHostOvrCfg;
static bool                botHostOvrCfgSet;
static bool                botHostOvrInit;
static BotHostOverrunState botHostOvr[1 + MAX_BOT_HOSTS];
/* Each seat's counters at the last probe and where its brain ran then
 * (the table place; -1: not counted). */
static struct {
    int      owner;
    uint32_t thinks;
    uint32_t overruns;
} botHostOvrSeat[MAX_TANKS];
/* The server's own free CPU (machine_cpu_free.h). */
static MachineCpuFree botHostServerFree;
static bool           botHostServerFreeHave;
static uint8_t        botHostServerFreeValue = (uint8_t)BOT_HOST_FREE_UNKNOWN;
static uint32_t       botHostServerFreeTick;
/* The last placement refusal logged per place, so a refusal logs once; and
 * whether a wait for a movable bot was logged. */
static const char *botHostOvrRefuseWhy[1 + MAX_BOT_HOSTS];
static bool        botHostOvrWaitLogged[1 + MAX_BOT_HOSTS];
/* The round log records (-record-bothost-data): whether each place's HOST
 * record is in the log, and whether the log recorded at the last probe. */
static bool botHostRecordData;
static bool botHostRecHostSent[1 + MAX_BOT_HOSTS];
static bool botHostRecWasOn;
/* -bothost-nomove: no bot moves while a game runs. */
static BotHostNoMove botHostNoMove;

static void botHostOvrEnsure(void) {
    int i;
    if (botHostOvrInit) return;
    botHostOvrInit = true;
    for (i = 0; i <= MAX_BOT_HOSTS; i++) botHostOverrunReset(&botHostOvr[i]);
    for (i = 0; i < MAX_TANKS; i++) botHostOvrSeat[i].owner = -1;
}

/* Place i starts again: a host joined or left. */
static void botHostOvrResetHost(int i) {
    if (i < 0 || i > MAX_BOT_HOSTS) return;
    botHostOvrEnsure();
    botHostOverrunReset(&botHostOvr[i]);
    botHostOvrRefuseWhy[i] = NULL;
    botHostOvrWaitLogged[i] = false;
    botHostRecHostSent[i] = false;
}

/* A new game, the lobby or the next round: the lowered limits go. The free
 * CPU history and the one-bot cost stay (botHostOverrunNewGame). */
static void botHostOvrNewGame(void) {
    int i;
    botHostOvrEnsure();
    for (i = 0; i <= MAX_BOT_HOSTS; i++) {
        botHostOverrunNewGame(&botHostOvr[i]);
        botHostOvrRefuseWhy[i] = NULL;
        botHostOvrWaitLogged[i] = false;
        botHostRecHostSent[i] = false;
    }
    for (i = 0; i < MAX_TANKS; i++) botHostOvrSeat[i].owner = -1;
    botHostNoMoveNewGame(&botHostNoMove);
}

static void botSeatReset(int s, bool noReturn);
static void botHostDrop(ServerSim *sim, int h, const char *why);
static void botSeatFallBack(ServerSim *sim, int s, const char *why,
                            bool tellHost);

static bool botHostAddrEqual(const struct sockaddr_in *a,
                             const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

static const char *botHostAddrStr(const struct sockaddr_in *a,
                                  char *buf, size_t sz) {
    snprintf(buf, sz, "%s:%u", inet_ntoa(a->sin_addr),
             (unsigned)ntohs(a->sin_port));
    return buf;
}

/* Compare two byte strings in a time that does not depend on where they
 * first differ. */
static bool botHostBytesEqual(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

int serverFindBotHost(const struct sockaddr_in *addr) {
    int h;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (udpServer.botHosts[h].connected &&
            botHostAddrEqual(&udpServer.botHosts[h].addr, addr)) {
            return h;
        }
    }
    return -1;
}

/* ── The -bothosts list ────────────────────────────────────────────────── */

/* Ticks with no bots wanted (no player, no bot) before a connected host
 * with no seats is let go: 60 s. It announces again, and is invited again
 * once bots are wanted. */
#define BOT_HOST_IDLE_RELEASE_TICKS 3000

/* True when a host at ip, listening on port, may be heard. *entry is its
 * list entry, or -1 when the server has no list (a key only). */
static bool botHostListed(uint32_t ip, uint16_t port, int *entry) {
    *entry = -1;
    if (udpServer.botHostAllowCount == 0) return true;
    *entry = botHostAllowFind(udpServer.botHostAllow,
                              udpServer.botHostAllowCount, ip, port);
    return *entry >= 0;
}

/* Any entry with this address, whatever its port: a host's bot connections
 * come from ports of their own. */
static bool botHostIpListed(uint32_t ip) {
    int i;
    if (udpServer.botHostAllowCount == 0) return true;
    for (i = 0; i < udpServer.botHostAllowCount; i++) {
        if (udpServer.botHostAllow[i].ip == ip) return true;
    }
    return false;
}

/* A key or a list is set: hosts other than the players' own may join and
 * announce. */
static bool botHostOpenToAll(void) {
    return udpServer.botHostKey[0] != '\0' || udpServer.botHostAllowCount > 0;
}

/* The player that owns host h is still on the connection it had when the
 * host joined. */
static bool botHostOwnerHere(int h) {
    const BotHostConn *hc = &udpServer.botHosts[h];
    const UdpServerClient *c;
    if (!hc->playerHost) return true;
    if (hc->ownerSlot >= MAX_TANKS) return false;
    c = &udpServer.clients[hc->ownerSlot];
    return c->connected && !c->botSlot && c->connId == hc->ownerConnId;
}

/* Log a host that is not on the list, once per address. */
static void botHostLogIgnored(const struct sockaddr_in *from, const char *what) {
    char addr[48];
    int i;
    for (i = 0; i < udpServer.botHostIgnoredCount; i++) {
        if (botHostAddrEqual(&udpServer.botHostIgnored[i], from)) return;
    }
    if (udpServer.botHostIgnoredCount <
        (int)(sizeof(udpServer.botHostIgnored) /
              sizeof(udpServer.botHostIgnored[0]))) {
        udpServer.botHostIgnored[udpServer.botHostIgnoredCount++] = *from;
    }
    fprintf(stderr, "[bothost] %s from %s ignored: not on the -bothosts list "
                    "(logged once)\n", what,
            botHostAddrStr(from, addr, sizeof(addr)));
    WB_LOG_WARN(WB_LOG_CAT_NET, "[bothost] %s from %s ignored: not listed",
                what, addr);
}

/* The connected host that listens at addr, or -1. */
static int botHostFindByListen(const struct sockaddr_in *addr) {
    int h;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (udpServer.botHosts[h].connected &&
            udpServer.botHosts[h].listenAddr.sin_port != 0 &&
            botHostAddrEqual(&udpServer.botHosts[h].listenAddr, addr)) {
            return h;
        }
    }
    return -1;
}

/* Where list entry i listens. */
static void botHostEntryAddr(int i, struct sockaddr_in *out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = udpServer.botHostAllow[i].ip;
    out->sin_port = htons(udpServer.botHostAllow[i].port != 0
                              ? udpServer.botHostAllow[i].port
                              : BOT_HOST_DEFAULT_LISTEN_PORT);
}

/* The server wants bots run when it is in the lobby or a game and holds a
 * player or a bot. *humans is the players connected. */
static bool botHostWanted(ServerSim *sim, int *humans) {
    ServerState st = serverSimGetState(sim);
    int s, people = 0, bots = 0;
    for (s = 0; s < MAX_TANKS; s++) {
        if (udpServer.clients[s].connected && !udpServer.clients[s].botSlot) {
            people++;
        }
        if (botManagerIsBot(sim, (BYTE)s)) bots++;
    }
    if (humans != NULL) *humans = people;
    if (st != serverStateLobby && st != serverStateCountdown &&
        st != serverStateRunning) {
        return false;
    }
    return people > 0 || bots > 0;
}

/* Ask every listed host that is not connected whether it has free slots.
 * A host with free slots answers with an ANNOUNCE, which the server
 * answers with an INVITE. */
static void botHostSendQueries(const char *why) {
    int i, sent = 0;
    for (i = 0; i < udpServer.botHostAllowCount; i++) {
        struct sockaddr_in to;
        uint8_t buf[PACKET_HEADER_SIZE + BOT_HOST_LINK_SHORT_LEN];
        botHostEntryAddr(i, &to);
        if (botHostFindByListen(&to) >= 0) continue;
        packHeader(buf, PACKET_BOT_HOST_QUERY, 0);
        buf[PACKET_HEADER_SIZE] = BOT_HOST_LINK_PROTO;
        srvSendTo(buf, sizeof(buf), &to);
        sent++;
    }
    if (sent > 0) {
        fprintf(stderr, "[bothost] asking %d listed Bot Host%s for free slots "
                        "(%s)\n", sent, sent == 1 ? "" : "s", why);
    }
}

static int botHostUsedSlots(int h) {
    int s, used = 0;
    for (s = 0; s < MAX_TANKS; s++) {
        if (udpServer.botSeats[s].state != BOT_SEAT_LOCAL &&
            udpServer.botSeats[s].host == h) {
            used++;
        }
    }
    return used;
}

/* ── Brain sync ────────────────────────────────────────────────────────── */

/* A Bot Host's sync time limit: 60 s at 50 ticks a second. A host still
 * downloading after it no longer holds the lobby, and takes no bots for
 * that game. */
#define BOT_HOST_SYNC_LIMIT_TICKS   3000
/* Ticks between looks at the brain files in the lobby, so a file changed
 * there reaches the hosts before the game starts. */
#define BOT_HOST_MANIFEST_POLL_TICKS 250

/* Drop the file being sent and the files asked for. */
static void botHostFileReset(BotHostConn *hc) {
    SDL_free(hc->fileData);
    hc->fileData = NULL;
    hc->fileSending = false;
    hc->fileOffset = 0;
    hc->fileSize = 0;
    hc->fileReqCount = 0;
}

/* The host holds every file of the current manifest. */
static bool botHostSynced(int h) {
    const BotHostConn *hc = &udpServer.botHosts[h];
    return hc->connected && udpServer.brainManifestGen != 0 &&
           hc->syncReady && hc->syncGen == udpServer.brainManifestGen;
}

/* The host may be given bots now: it holds the brain files and is not
 * skipped for this game. */
static bool botHostTakesBots(ServerSim *sim, int h) {
    const BotHostConn *hc = &udpServer.botHosts[h];
    if (!botHostSynced(h)) return false;
    if (hc->skipGame && botManagerGameStarts(sim) <= hc->skipUntilStarts) {
        return false;
    }
    return true;
}

/* Make the brain manifest again from the brain folders. When it differs
 * from the one the hosts have, it gets the next generation and every
 * connected host is sent it. */
static void botHostRefreshManifest(ServerSim *sim, const char *why) {
    BrainSyncManifest next;
    uint8_t *blob;
    size_t blobLen = 0;
    uint32_t files = 0, bytes = 0;
    char hex[17];
    int b, h;

    brainSyncBuildManifest(&next, botManagerDefaultDebugMode(sim),
                           &udpServer.brainManifest);
    if (udpServer.brainManifestGen != 0 &&
        next.version == udpServer.brainManifest.version) {
        /* Same files. Keep the new list for its change times. */
        brainSyncFree(&udpServer.brainManifest);
        udpServer.brainManifest = next;
        return;
    }
    blob = brainSyncManifestEncode(&next, &blobLen);
    if (blob == NULL) {
        fprintf(stderr, "[bothost] the brain file list is too large to send; "
                        "Bot Hosts keep the last one\n");
        brainSyncFree(&next);
        return;
    }
    brainSyncFree(&udpServer.brainManifest);
    udpServer.brainManifest = next;
    free(udpServer.brainManifestBlob);
    udpServer.brainManifestBlob = blob;
    udpServer.brainManifestBlobLen = blobLen;
    udpServer.brainManifestGen++;
    if (udpServer.brainManifestGen == 0) udpServer.brainManifestGen = 1;
    for (b = 0; b < next.brainCount; b++) {
        files += (uint32_t)next.brains[b].fileCount;
        bytes += next.brains[b].totalBytes;
    }
    brainSyncVersionHex(next.version, hex);
    fprintf(stderr, "[bothost] brain files (%s): %d brains, %u files, %u "
                    "bytes, version %s, generation %u\n",
            why, next.brainCount, (unsigned)files, (unsigned)bytes, hex,
            (unsigned)udpServer.brainManifestGen);
    for (b = 0; b < next.brainCount; b++) {
        brainSyncVersionHex(next.brains[b].version, hex);
        fprintf(stderr, "[bothost]   brain \"%s\": %d files, %u bytes, "
                        "version %s\n",
                next.brains[b].name, next.brains[b].fileCount,
                (unsigned)next.brains[b].totalBytes, hex);
    }
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected) continue;
        botHostFileReset(hc);
        hc->manifestOwed = true;
        hc->syncSentTick = udpServer.tickCount;
        hc->syncLate = false;
        hc->syncDone = 0;
        hc->syncTotal = 0;
    }
}

/* Start the next bulk transfer for a host: the manifest when it is owed,
 * else the next piece of the file being sent, else the next file asked
 * for. One transfer at a time, so a piece is compressed only when the last
 * one has gone. */
static void botHostServiceFiles(ServerSim *sim, int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    BulkStreamHeader hdr;
    uint8_t *blob;
    uint32_t blobLen = 0, n;

    if (bulkSenderBusy(&hc->bulk)) return;
    memset(&hdr, 0, sizeof(hdr));
    if (hc->manifestOwed) {
        hdr.kind = BULK_KIND_BRAIN_MANIFEST;
        hdr.gen = udpServer.brainManifestGen;
        hdr.totalSize = (uint32_t)udpServer.brainManifestBlobLen;
        if (bulkSenderBegin(&hc->bulk, &hdr, udpServer.brainManifestBlob,
                            (uint32_t)udpServer.brainManifestBlobLen)) {
            hc->manifestOwed = false;
        }
        return;
    }
    if (!hc->fileSending) {
        const BrainSyncBrain *br;
        const BrainSyncFile *f;
        char path[1024];
        size_t sz = 0;
        uint32_t id, bi, fi;
        void *data;

        if (hc->fileReqCount == 0) return;
        id = hc->fileReq[0];
        memmove(hc->fileReq, hc->fileReq + 1,
                sizeof(hc->fileReq[0]) * (size_t)(hc->fileReqCount - 1));
        hc->fileReqCount--;
        bi = id >> 16;
        fi = id & 0xffffu;
        if (bi >= (uint32_t)udpServer.brainManifest.brainCount) return;
        br = &udpServer.brainManifest.brains[bi];
        if (fi >= (uint32_t)br->fileCount) return;
        f = &br->files[fi];
        SDL_snprintf(path, sizeof(path), "%s/%s", br->dir, f->path);
        data = SDL_LoadFile(path, &sz);
        if ((data == NULL && f->size != 0) || sz != f->size ||
            brainSyncHash(BRAIN_SYNC_HASH_SEED, data, sz) != f->hash) {
            SDL_free(data);
            fprintf(stderr, "[bothost] brain file \"%s\" changed since the "
                            "list was made\n", path);
            botHostRefreshManifest(sim, "a brain file changed");
            return;
        }
        hc->fileData = (uint8_t *)data;
        hc->fileSize = (uint32_t)sz;
        hc->fileOffset = 0;
        hc->fileId = id;
        hc->fileSending = true;
    }
    n = hc->fileSize - hc->fileOffset;
    if (n > BRAIN_SYNC_CHUNK_BYTES) n = BRAIN_SYNC_CHUNK_BYTES;
    blob = brainSyncChunkPack(hc->fileData != NULL ? hc->fileData + hc->fileOffset
                                                   : (const uint8_t *)"",
                              n, &blobLen);
    if (blob == NULL) {
        botHostFileReset(hc);
        return;
    }
    hdr.kind = BULK_KIND_BRAIN_CHUNK;
    hdr.gen = udpServer.brainManifestGen;
    hdr.totalSize = blobLen;
    hdr.pathLen = (uint8_t)SDL_snprintf(hdr.path, sizeof(hdr.path), "%u/%u/%u",
                                        (unsigned)(hc->fileId >> 16),
                                        (unsigned)(hc->fileId & 0xffffu),
                                        (unsigned)hc->fileOffset);
    if (!bulkSenderBegin(&hc->bulk, &hdr, blob, blobLen)) {
        free(blob);
        return;
    }
    free(blob);
    hc->fileOffset += n;
    if (hc->fileOffset >= hc->fileSize) {
        SDL_free(hc->fileData);
        hc->fileData = NULL;
        hc->fileSending = false;
    }
}

/* A control event from a Bot Host: a file request or a sync report. */
static void botHostOnControl(int h, const uint8_t *msg, uint16_t len) {
    BotHostConn *hc = &udpServer.botHosts[h];
    ControlDecodeBodyFn dec;
    ControlEvent evt;
    uint16_t bodyLen;

    if (len < 3) return;
    bodyLen = unpackU16(msg + 1);
    if ((size_t)bodyLen + 3 > len) return;
    if (msg[0] != CTRL_BOT_HOST_FILE_REQ && msg[0] != CTRL_BOT_HOST_SYNC &&
        msg[0] != CTRL_BOT_HOST_SLOTS && msg[0] != CTRL_BOT_HOST_REC &&
        msg[0] != CTRL_BOT_HOST_CPU && msg[0] != CTRL_BOT_HOST_CPU_FREE) {
        return;
    }
    dec = transportControlCodecBodyDecoder((ControlEventType)msg[0]);
    if (dec == NULL || !dec(msg + 3, bodyLen, &evt)) return;
    if (evt.type == CTRL_BOT_HOST_CPU) {
        /* Only the percent: the host never says what its machine is. */
        hc->cpu = evt.u.botHostCpu.cpu;
        return;
    }
    if (evt.type == CTRL_BOT_HOST_CPU_FREE) {
        /* The whole machine's free CPU, only the percent. */
        hc->cpuFree = evt.u.botHostCpuFree.free;
        hc->cpuFreeTick = udpServer.tickCount;
        return;
    }
    if (evt.type == CTRL_BOT_HOST_REC) {
        serverBotRecOnControl(h, &evt);
        return;
    }
    if (evt.type == CTRL_BOT_HOST_SLOTS) {
        uint8_t rs = evt.u.botHostSlots.refuseSeat;
        if (evt.u.botHostSlots.slots != hc->slots) {
            fprintf(stderr, "[bothost] Bot Host \"%s\" now offers this server "
                            "%u slots\n", hc->name,
                    (unsigned)evt.u.botHostSlots.slots);
        }
        hc->slots = evt.u.botHostSlots.slots;
        {
            /* 0 slots: the host gives every bot back at once, as when it
             * leaves (a player turned off "run bots on this computer" for
             * this game). The host stays connected, so it takes bots again
             * when it offers slots again. */
            int seatHost[MAX_TANKS], s2;
            uint32_t back;
            for (s2 = 0; s2 < MAX_TANKS; s2++) {
                seatHost[s2] = udpServer.botSeats[s2].state != BOT_SEAT_LOCAL
                                   ? udpServer.botSeats[s2].host : -1;
            }
            back = botHostSim == NULL ? 0
                   : botHostSeatsToGiveBack(seatHost, MAX_TANKS, h,
                                            evt.u.botHostSlots.slots);
            for (s2 = 0; s2 < MAX_TANKS; s2++) {
                if ((back & (1u << s2)) == 0) continue;
                botSeatFallBack(botHostSim, s2, "its host offers 0 slots",
                                true);
                /* Not the seat's fault: it may go to another host at once,
                 * or back to this one when it offers slots again. */
                udpServer.botSeats[s2].noReturn = false;
            }
        }
        if (rs < MAX_TANKS) {
            BotHostSeat *seat = &udpServer.botSeats[rs];
            if (seat->state == BOT_SEAT_PENDING && seat->host == h &&
                seat->gen == evt.u.botHostSlots.refuseGen) {
                /* Another server took the host's last slot first. The seat
                 * stays in-process; it may go to a host again when one has
                 * a free slot. */
                fprintf(stderr, "[bothost] bot %d: Bot Host \"%s\" is full "
                                "(another server was first); it stays "
                                "in-process\n", (int)rs, hc->name);
                botSeatReset(rs, false);
            }
        }
        return;
    }
    if (evt.type == CTRL_BOT_HOST_FILE_REQ) {
        if (evt.u.botHostFileReq.gen != udpServer.brainManifestGen) return;
        if (hc->fileReqCount >= (int)(sizeof(hc->fileReq) / sizeof(hc->fileReq[0]))) {
            return;
        }
        hc->fileReq[hc->fileReqCount++] =
            ((uint32_t)evt.u.botHostFileReq.brain << 16) |
            evt.u.botHostFileReq.file;
        return;
    }
    if (evt.u.botHostSync.gen != udpServer.brainManifestGen) return;
    {
        bool wasReady = hc->syncReady && hc->syncGen == evt.u.botHostSync.gen;
        hc->syncGen = evt.u.botHostSync.gen;
        hc->syncDone = evt.u.botHostSync.done;
        hc->syncTotal = evt.u.botHostSync.total;
        hc->syncReady = evt.u.botHostSync.ready != 0;
        if (hc->syncReady && !wasReady) {
            fprintf(stderr, "[bothost] Bot Host \"%s\" holds the brain files "
                            "(generation %u, %u bytes downloaded)\n",
                    hc->name, (unsigned)hc->syncGen, (unsigned)hc->syncDone);
        }
    }
}

/* The lobby's row for host h. */
static void botHostFillRow(int h, ControlEvent *evt) {
    const BotHostConn *hc = &udpServer.botHosts[h];
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_BOT_HOST_STATUS;
    evt->u.botHostStatus.idx = (uint8_t)h;
    if (!hc->connected) return;
    evt->u.botHostStatus.connected = 1;
    evt->u.botHostStatus.used = (uint8_t)botHostUsedSlots(h);
    {
        int s;
        for (s = 0; s < MAX_TANKS; s++) {
            if (udpServer.botSeats[s].state != BOT_SEAT_LOCAL &&
                udpServer.botSeats[s].host == h) {
                evt->u.botHostStatus.seats |= (uint16_t)(1u << s);
            }
        }
    }
    evt->u.botHostStatus.slots = hc->slots;
    SDL_strlcpy(evt->u.botHostStatus.name, hc->name,
                sizeof(evt->u.botHostStatus.name));
    serverBotRecRow(h, &evt->u.botHostStatus.recState,
                    &evt->u.botHostStatus.recPercent,
                    &evt->u.botHostStatus.recBotsDone,
                    &evt->u.botHostStatus.recBots);
    if (botHostSynced(h)) {
        evt->u.botHostStatus.state = BOT_HOST_SYNC_READY;
        evt->u.botHostStatus.percent = 100;
        return;
    }
    if (hc->syncLate) {
        evt->u.botHostStatus.state = BOT_HOST_SYNC_LATE;
    } else if (hc->syncGen == udpServer.brainManifestGen &&
               udpServer.brainManifestGen != 0) {
        evt->u.botHostStatus.state = BOT_HOST_SYNC_SYNCING;
    } else {
        evt->u.botHostStatus.state = BOT_HOST_SYNC_WAITING;
    }
    if (hc->syncGen == udpServer.brainManifestGen && hc->syncTotal > 0) {
        uint64_t pct = (uint64_t)hc->syncDone * 100u / hc->syncTotal;
        evt->u.botHostStatus.percent = (uint8_t)(pct > 99 ? 99 : pct);
    }
}

/* The server's own row, below the hosts: the bots it runs itself against
 * its own capacity answer. Not a connection: it has no kick or move. */
static int botHostServerSlotsRow(ServerSim *sim);
static void botHostSampleServerCpu(ServerSim *sim, int bots);
static void botHostFillServerRow(ControlEvent *evt) {
    ServerSim *sim = botHostSim;
    int s, n = 0, slots;
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_BOT_HOST_STATUS;
    evt->u.botHostStatus.idx = (uint8_t)MAX_BOT_HOSTS;
    if (sim == NULL || !udpServer.botHostEnabled) return;
    evt->u.botHostStatus.connected = 1;
    evt->u.botHostStatus.state = BOT_HOST_SYNC_READY;
    evt->u.botHostStatus.percent = 100;
    for (s = 0; s < MAX_TANKS; s++) {
        if (udpServer.botSeats[s].state != BOT_SEAT_LOCAL) continue;
        if (!botManagerIsBot(sim, (BYTE)s)) continue;
        if (botManagerIsRemote(sim, (BYTE)s)) continue;
        evt->u.botHostStatus.seats |= (uint16_t)(1u << s);
        n++;
    }
    botHostSampleServerCpu(sim, n);
    slots = botHostServerSlotsRow(sim);
    evt->u.botHostStatus.used = (uint8_t)n;
    evt->u.botHostStatus.slots = (uint8_t)(slots > MAX_TANKS ? MAX_TANKS : slots);
    SDL_strlcpy(evt->u.botHostStatus.name, serverSimGetServerName(sim),
                sizeof(evt->u.botHostStatus.name));
}

/* The rows last sent to the lobby, so a row goes out only when it changes. */
static ControlEvent botHostRowSent[MAX_BOT_HOSTS + 1];

/* The server's own CPU estimate for the bots it runs itself, measured the
 * way a Bot Host measures its own (bot_host_cpu.h). */
static BotHostCpuMeter botHostServerCpu;
static bool            botHostServerCpuInit;
static uint8_t         botHostServerCpuValue = (uint8_t)BOT_HOST_CPU_UNKNOWN;

static void botHostSampleServerCpu(ServerSim *sim, int bots) {
    if (!botHostServerCpuInit) {
        botHostCpuMeterInit(&botHostServerCpu);
        botHostServerCpuInit = true;
    }
    botHostServerCpuValue = botHostCpuMeterSample(
        &botHostServerCpu, SDL_GetTicksNS(), botManagerThinkMsTotal(sim),
        SDL_GetNumLogicalCPUCores(), bots);
}

/* The CPU byte of row h (MAX_BOT_HOSTS: the server's own row). */
static uint8_t botHostRowCpu(int h, const ControlEvent *row) {
    if (!row->u.botHostStatus.connected) return (uint8_t)BOT_HOST_CPU_UNKNOWN;
    if (h == MAX_BOT_HOSTS) return botHostServerCpuValue;
    return udpServer.botHosts[h].cpu;
}

/* The CPU bytes last sent to the lobby, one per row. */
static uint8_t botHostCpuSent[MAX_BOT_HOSTS + 1];
static bool    botHostCpuSentInit;

/* The log line for a row's CPU estimate: the first one, then a change at
 * most each BOT_HOST_CPU_LOG_TICKS, so the log does not fill. */
#define BOT_HOST_CPU_LOG_TICKS 500
static uint32_t botHostCpuLogTick[MAX_BOT_HOSTS + 1];
static bool     botHostCpuLogged[MAX_BOT_HOSTS + 1];

static void botHostLogCpu(int h, const ControlEvent *row, uint8_t cpu) {
    char pct[8];
    if (cpu == BOT_HOST_CPU_UNKNOWN) return;
    if (botHostCpuLogged[h] &&
        udpServer.tickCount - botHostCpuLogTick[h] < BOT_HOST_CPU_LOG_TICKS) {
        return;
    }
    botHostCpuLogged[h] = true;
    botHostCpuLogTick[h] = udpServer.tickCount;
    if (cpu == BOT_HOST_CPU_UNDER_ONE) {
        SDL_strlcpy(pct, "<1%", sizeof(pct));
    } else {
        SDL_snprintf(pct, sizeof(pct), "%u%%", (unsigned)cpu);
    }
    fprintf(stderr, "[bothost] CPU estimate: %s \"%s\" %s of its machine's "
                    "CPU, %u bots\n",
            h == MAX_BOT_HOSTS ? "the server" : "Bot Host",
            row->u.botHostStatus.name, pct,
            (unsigned)row->u.botHostStatus.used);
}

static void botHostFillCpu(int h, uint8_t cpu, ControlEvent *evt) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_BOT_HOST_CPU;
    evt->u.botHostCpu.idx = (uint8_t)h;
    evt->u.botHostCpu.cpu = cpu;
}

static void botHostBroadcastRows(void) {
    int h;
    if (!botHostCpuSentInit) {
        memset(botHostCpuSent, BOT_HOST_CPU_UNKNOWN, sizeof(botHostCpuSent));
        botHostCpuSentInit = true;
    }
    for (h = 0; h <= MAX_BOT_HOSTS; h++) {
        ControlEvent evt;
        if (h < MAX_BOT_HOSTS) {
            botHostFillRow(h, &evt);
        } else {
            botHostFillServerRow(&evt);
        }
        if (memcmp(&evt.u.botHostStatus, &botHostRowSent[h].u.botHostStatus,
                   sizeof(evt.u.botHostStatus)) != 0) {
            botHostRowSent[h] = evt;
            serverBroadcastLobbyControl(&evt);
        }
        {
            /* The CPU estimate after its row, when it changes. A client
             * forgets it when the row goes, so a gone row's unknown is
             * only noted here. */
            uint8_t cpu = botHostRowCpu(h, &evt);
            if (cpu != botHostCpuSent[h]) {
                botHostCpuSent[h] = cpu;
                if (evt.u.botHostStatus.connected) {
                    ControlEvent ce;
                    botHostFillCpu(h, cpu, &ce);
                    serverBroadcastLobbyControl(&ce);
                    botHostLogCpu(h, &evt, cpu);
                }
            }
        }
    }
}

void serverEnumBotHostRows(void (*deliver)(void *, const ControlEvent *),
                           void *deliverCtx) {
    int h;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        ControlEvent evt;
        if (!udpServer.botHosts[h].connected) continue;
        botHostFillRow(h, &evt);
        deliver(deliverCtx, &evt);
        if (botHostRowCpu(h, &evt) != BOT_HOST_CPU_UNKNOWN) {
            ControlEvent ce;
            botHostFillCpu(h, botHostRowCpu(h, &evt), &ce);
            deliver(deliverCtx, &ce);
        }
    }
    {
        ControlEvent evt;
        botHostFillServerRow(&evt);
        if (evt.u.botHostStatus.connected) {
            deliver(deliverCtx, &evt);
            if (botHostRowCpu(MAX_BOT_HOSTS, &evt) != BOT_HOST_CPU_UNKNOWN) {
                ControlEvent ce;
                botHostFillCpu(MAX_BOT_HOSTS,
                               botHostRowCpu(MAX_BOT_HOSTS, &evt), &ce);
                deliver(deliverCtx, &ce);
            }
        }
    }
}

/* Queue one control event on a host's control channel. False when the
 * channel is full: the host is then too far behind to keep. */
static bool botHostSendEvent(int h, const ControlEvent *evt) {
    BotHostConn *hc = &udpServer.botHosts[h];
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(evt->type);
    uint8_t msg[CHANNEL_CONTROL_SEG];
    size_t bodyLen = 0;

    if (!hc->connected || enc == NULL) return false;
    if (enc(evt, NULL, msg + 3, sizeof(msg) - 3, &bodyLen) != ENCODE_OK) {
        WB_LOG_ERROR(WB_LOG_CAT_NET, "[bothost] encode failed type=%s",
                     controlEventDiagName(evt->type));
        return false;
    }
    msg[0] = (uint8_t)evt->type;
    packU16(msg + 1, (uint16_t)bodyLen);
    if (!channelSend(&hc->channelMux, CHANNEL_CONTROL, msg,
                     (uint16_t)(3 + bodyLen))) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[bothost] control channel full for Bot Host \"%s\"",
                    hc->name);
        return false;
    }
    return true;
}

bool serverBotHostSendEvent(int h, const ControlEvent *evt) {
    if (h < 0 || h >= MAX_BOT_HOSTS) return false;
    return botHostSendEvent(h, evt);
}

/* Send a whole table as one event per entry; an empty table is one event
 * with count 0. */
static void botHostSendTable(int h, ControlEventType type, BYTE seat,
                             const ScnTable *t) {
    ControlEvent evt;
    uint8_t count = (t != NULL) ? t->count : 0;
    uint8_t i;

    if (count > SCN_TABLE_MAX) count = SCN_TABLE_MAX;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.u.botTableEntry.seat  = seat;
    evt.u.botTableEntry.count = count;
    if (count == 0) {
        (void)botHostSendEvent(h, &evt);
        return;
    }
    for (i = 0; i < count; i++) {
        evt.u.botTableEntry.idx = i;
        SDL_strlcpy(evt.u.botTableEntry.key, t->kv[i].key,
                    sizeof(evt.u.botTableEntry.key));
        SDL_strlcpy(evt.u.botTableEntry.value, t->kv[i].value,
                    sizeof(evt.u.botTableEntry.value));
        (void)botHostSendEvent(h, &evt);
    }
}

/* The brain's name from its path: the folder that holds its init.lua
 * ("Brains/GoalHunter/init.lua" gives "GoalHunter"). The host finds the
 * brain by that name in its own brain folders (brainListResolve), as a
 * scenario names a brain. A path with no folder gives its file name
 * without ".lua". */
static void botHostBrainName(const char *path, char *out, size_t sz) {
    const char *last = NULL, *prev = NULL, *p;
    const char *start, *stop;
    size_t n;

    out[0] = '\0';
    if (path == NULL || sz == 0) return;
    for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            prev = last;
            last = p;
        }
    }
    if (last != NULL) {
        start = (prev != NULL) ? prev + 1 : path;
        stop = last;
    } else {
        start = path;
        stop = path + strlen(path);
        if (stop - start > 4 && SDL_strcasecmp(stop - 4, ".lua") == 0) stop -= 4;
    }
    n = (size_t)(stop - start);
    if (n >= sz) n = sz - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

/* The brain file's version stamp: a CRC-32 of its bytes, 0 when it cannot
 * be read. The host warns when its copy differs. */
static uint32_t botHostBrainVersion(const char *path) {
    size_t size = 0;
    void *data;
    uint32_t crc;
    if (path == NULL || path[0] == '\0') return 0;
    data = SDL_LoadFile(path, &size);
    if (data == NULL) return 0;
    crc = SDL_crc32(0, data, size);
    SDL_free(data);
    return crc;
}

/* The seat's player name, from the roster. A bot seat that runs
 * in-process has no UDP client, so udpServer.clients[s] has no name.
 * The last name read is kept so that the operator listing, which has no
 * ServerSim, can print it too. */
static char botSeatNames[MAX_TANKS][PLAYER_NAME_LEN];

static const char *botSeatName(ServerSim *sim, int s) {
    GameSim *gs = serverSimGetGameSim(sim);
    if (gs != NULL) {
        botSeatNames[s][0] = '\0';
        playersGetPlayerName(&gs->plyrs, (BYTE)s, botSeatNames[s],
                             sizeof(botSeatNames[s]), true);
    }
    return botSeatNames[s];
}

/* What a seat's brain is built from now: brain, AI level, team and init
 * argument. */
static void botSeatCurrent(ServerSim *sim, int s, char *brain, size_t brainSz,
                           uint8_t *ai, uint8_t *team,
                           char *initArg, size_t argSz) {
    const char *path = botManagerGetBrainPath(sim, (BYTE)s);
    const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, (BYTE)s);
    botHostBrainName(path, brain, brainSz);
    *ai = (uint8_t)botManagerGetBotAi(sim, (BYTE)s);
    *team = (lp != NULL) ? lp->teamNumber : 0;
    initArg[0] = '\0';
    if (path != NULL) {
        botManagerBuildInitArg(sim, (BYTE)s, path, NULL, initArg, argSz);
    }
}

static bool botHostTokenEmpty(const uint8_t *token) {
    int i;
    for (i = 0; i < BOT_HOST_TOKEN_LEN; i++) {
        if (token[i] != 0) return false;
    }
    return true;
}

/* Send a seat's assign and then its init table. reload asks the host for a
 * new brain now. */
static void botSeatSendAssign(ServerSim *sim, int s, bool reload) {
    BotHostSeat *seat = &udpServer.botSeats[s];
    ControlEvent evt;

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_ASSIGN;
    seat->gen++;
    botSeatCurrent(sim, s, seat->brain, sizeof(seat->brain), &seat->ai,
                   &seat->team, seat->initArg, sizeof(seat->initArg));
    evt.u.botHostAssign.seat   = (uint8_t)s;
    evt.u.botHostAssign.reload = reload ? 1 : 0;
    evt.u.botHostAssign.ai     = seat->ai;
    evt.u.botHostAssign.team   = seat->team;
    evt.u.botHostAssign.gen    = seat->gen;
    if (!botHostTokenEmpty(seat->token)) {
        /* The assign that places the seat on its host. */
        seat->placeGen = seat->gen;
    }
    evt.u.botHostAssign.startTick = (uint32_t)serverSimGetTick(sim);
    evt.u.botHostAssign.brainVersion =
        botHostBrainVersion(botManagerGetBrainPath(sim, (BYTE)s));
    memcpy(evt.u.botHostAssign.token, seat->token, BOT_HOST_TOKEN_LEN);
    SDL_strlcpy(evt.u.botHostAssign.name, botSeatName(sim, s),
                sizeof(evt.u.botHostAssign.name));
    SDL_strlcpy(evt.u.botHostAssign.brain, seat->brain,
                sizeof(evt.u.botHostAssign.brain));
    SDL_strlcpy(evt.u.botHostAssign.initArg, seat->initArg,
                sizeof(evt.u.botHostAssign.initArg));
    (void)botHostSendEvent(seat->host, &evt);
    botHostSendTable(seat->host, CTRL_BOT_INIT_TABLE, (BYTE)s,
                     botManagerGetBotInitTable(sim, (BYTE)s));
}

static void botSeatSendUnassign(int s) {
    BotHostSeat *seat = &udpServer.botSeats[s];
    ControlEvent evt;
    if (seat->host < 0 || !udpServer.botHosts[seat->host].connected) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_UNASSIGN;
    evt.u.botHostUnassign.seat = (uint8_t)s;
    (void)botHostSendEvent(seat->host, &evt);
}

/* Take a seat's bot connection down without a word to anyone: no console
 * line for the players, no WBN event, no leave message. The seat's name
 * stays, because it is the bot's. */
static void botSlotRelease(ServerSim *sim, int s) {
    UdpServerClient *c = &udpServer.clients[s];

    if (!c->connected || !c->botSlot) return;
    {
        uint8_t kbuf[PACKET_HEADER_SIZE];
        packHeader(kbuf, PACKET_KICKED, 0);
        srvSendTo(kbuf, sizeof(kbuf), &c->addr);
    }
    c->connected = false;
    c->botSlot = false;
    serverSimUnregisterSubscriber(sim, c->controlSub);
    c->controlSub = SUBSCRIBER_HANDLE_INVALID;
    c->connId = 0;
    c->inboundCmdSeq = 0;
    c->pingMs = 0;
    udpServer.pendingSimRemove[s] = false;
    udpServer.controlSyncInProgress[s] = false;
    serverCleanupMapDownload(s);
    channelMuxInit(&udpServer.channelMux[s]);
    udpServer.channelFramesRx[s] = 0;
    udpServer.mapGen[s] = 0;
    udpServer.mapEventQueues[s].nextSeq = 1;
    udpServer.mapEventQueues[s].ackedSeq = 1;
    udpServer.mapEventQueueDrops[s] = 0;
    udpServer.mapChannelStalled[s] = false;
    bulkSenderReset(&udpServer.bulkSend[s]);
    bulkReceiverInit(&udpServer.bulkRecvUp[s]);
    udpServerResetRoundLogLimits(s);
    udpServerResetMapReaskLimit(s);
    /* The slot's copy of the terrain goes back to the tick's, which every
     * in-process bot reads. */
    serverSimSetShadowCulled(sim, (BYTE)s, false);
    serverSimShadowSeed(sim, (BYTE)s);
}

static void botSeatReset(int s, bool noReturn) {
    BotHostSeat *seat = &udpServer.botSeats[s];
    uint16_t gen = seat->gen;
    memset(seat, 0, sizeof(*seat));
    seat->state = BOT_SEAT_LOCAL;
    seat->host = -1;
    seat->gen = gen;
    seat->noReturn = noReturn;
}

/* A seat leaves its host and its brain comes back in-process. */
static void botSeatFallBack(ServerSim *sim, int s, const char *why,
                            bool tellHost) {
    BotHostSeat *seat = &udpServer.botSeats[s];
    bool wasRemote;
    const char *hostName = "?";

    if (seat->state == BOT_SEAT_LOCAL) return;
    if (seat->host >= 0) hostName = udpServer.botHosts[seat->host].name;
    botSlotRelease(sim, s);
    wasRemote = botManagerIsRemote(sim, (BYTE)s);
    if (tellHost) botSeatSendUnassign(s);
    fprintf(stderr, "[bothost] bot %d \"%s\" back in-process from Bot Host "
                    "\"%s\" (%s, game tick %u)\n",
            s, botSeatName(sim, s), hostName, why,
            (unsigned)serverSimGetTick(sim));
    WB_LOG_INFO(WB_LOG_CAT_NET,
                "[bothost] bot %d back in-process from Bot Host \"%s\" (%s)",
                s, hostName, why);
    botSeatReset(s, true);
    if (wasRemote && !botManagerClearRemote(sim, (BYTE)s)) {
        fprintf(stderr, "[bothost] bot %d held: its brain could not be "
                        "built in-process\n", s);
    }
}

void serverBotSlotLost(ServerSim *sim, int slot, const char *why) {
    if (slot < 0 || slot >= MAX_TANKS) return;
    if (udpServer.botSeats[slot].state == BOT_SEAT_LOCAL) {
        /* No seat behind it any more: just let the connection go. */
        botSlotRelease(sim, slot);
        return;
    }
    botSeatFallBack(sim, slot, why, true);
}

static void botHostDrop(ServerSim *sim, int h, const char *why) {
    BotHostConn *hc = &udpServer.botHosts[h];
    char addr[48];
    int s;

    if (!hc->connected) return;
    for (s = 0; s < MAX_TANKS; s++) {
        if (udpServer.botSeats[s].state != BOT_SEAT_LOCAL &&
            udpServer.botSeats[s].host == h) {
            botSeatFallBack(sim, s, why, false);
            if (hc->playerHost) {
                /* A player's host goes when the player does, which is no
                 * fault of the seat: it may go to another host at once, or
                 * back to this player's host when the player rejoins. */
                udpServer.botSeats[s].noReturn = false;
            }
        }
    }
    serverBotRecHostGone(h);
    fprintf(stderr, "[bothost] Bot Host \"%s\" at %s gone (%s)\n", hc->name,
            botHostAddrStr(&hc->addr, addr, sizeof(addr)), why);
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] Bot Host \"%s\" gone (%s)",
                hc->name, why);
    botHostFileReset(hc);
    bulkSenderReset(&hc->bulk);
    memset(hc, 0, sizeof(*hc));
    botHostOvrResetHost(h + 1);
}

void serverBotHostPlayerGone(ServerSim *sim, int slot, const char *why) {
    int h;
    if (sim == NULL || slot < 0 || slot >= MAX_TANKS) return;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        if (hc->connected && hc->playerHost && hc->ownerSlot == slot) {
            uint8_t kbuf[PACKET_HEADER_SIZE];
            packHeader(kbuf, PACKET_KICKED, 0);
            srvSendTo(kbuf, sizeof(kbuf), &hc->addr);
            botHostDrop(sim, h, why);
        }
    }
    udpServer.playerTicketConnId[slot] = 0;
}

void serverSendBotHostAccept(int h, ServerSim *sim) {
    BotHostConn *hc = &udpServer.botHosts[h];
    uint8_t buf[PACKET_HEADER_SIZE + 9 + 8];
    int pos = PACKET_HEADER_SIZE;

    packHeader(buf, PACKET_JOIN_ACCEPT, hc->outSequence++);
    buf[pos++] = (uint8_t)BOT_HOST_ACCEPT_NO_SLOT;
    packU32(buf + pos, serverSimGetTick(sim));
    pos += 4;
    packU32(buf + pos, 0);  /* no map for a control connection */
    pos += 4;
    packConnId(buf + pos, hc->connId);
    pos += 8;
    srvSendTo(buf, pos, &hc->addr);
}

void serverAcceptBotHost(ServerSim *sim, const struct sockaddr_in *fromAddr,
                         const char *name, const uint8_t *extra,
                         int extraLen) {
    uint8_t key[JOIN_BOT_HOST_KEY_LEN];
    uint8_t want[JOIN_BOT_HOST_KEY_LEN];
    char hostName[JOIN_BOT_HOST_NAME_LEN];
    char addr[48];
    uint8_t slots;
    uint16_t listenPort = 0;
    int h, freeIdx = -1, entry;
    const int baseLen = JOIN_BOT_HOST_KEY_LEN + 1 + JOIN_BOT_HOST_NAME_LEN;

    botHostAddrStr(fromAddr, addr, sizeof(addr));
    if (extraLen < baseLen) {
        fprintf(stderr, "[bothost] Bot Host join from %s malformed\n", addr);
        return;
    }
    if (extraLen >= baseLen + 2) {
        listenPort = unpackU16(extra + baseLen);
    }
    if (extraLen >= baseLen + 2 + JOIN_BOT_HOST_PLAYER_LEN &&
        extra[baseLen + 2] == JOIN_BOT_HOST_PLAYER_MARK) {
        /* A player's own host: the ticket, not the key or the list. */
        const uint8_t *pm = extra + baseLen + 2;
        uint8_t owner = pm[1];
        const UdpServerClient *oc;
        if (!udpServer.botHostEnabled || udpServer.noPlayerBotHosts) {
            fprintf(stderr, "[bothost] player Bot Host join from %s refused: "
                            "this server takes no player Bot Host\n", addr);
            serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0,
                                 NULL);
            return;
        }
        oc = owner < MAX_TANKS ? &udpServer.clients[owner] : NULL;
        if (oc == NULL || !oc->connected || oc->botSlot ||
            oc->connId == 0 ||
            udpServer.playerTicketConnId[owner] != oc->connId ||
            oc->addr.sin_addr.s_addr != fromAddr->sin_addr.s_addr ||
            !botHostBytesEqual(pm + 2, udpServer.playerTicket[owner],
                               BOT_HOST_TICKET_LEN)) {
            fprintf(stderr, "[bothost] player Bot Host join from %s refused: "
                            "no player %u with that ticket at that address\n",
                    addr, (unsigned)owner);
            WB_LOG_WARN(WB_LOG_CAT_NET, "[bothost] player Bot Host join from "
                        "%s refused (player %u)", addr, (unsigned)owner);
            serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0,
                                 NULL);
            return;
        }
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            /* One host per player: a new one replaces the old. */
            if (udpServer.botHosts[h].connected &&
                udpServer.botHosts[h].playerHost &&
                udpServer.botHosts[h].ownerSlot == owner) {
                botHostDrop(sim, h, "the player's Bot Host joined again");
            }
        }
        slots = extra[JOIN_BOT_HOST_KEY_LEN];
        if (slots > BOT_HOST_MAX_SLOTS) slots = BOT_HOST_MAX_SLOTS;
        /* Shown in the lobby's Bot Host rows: the player's name. */
        SDL_strlcpy(hostName, oc->playerName, sizeof(hostName));
        if (hostName[0] == '\0') {
            SDL_snprintf(hostName, sizeof(hostName), "player %u",
                         (unsigned)owner);
        }
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            if (!udpServer.botHosts[h].connected) {
                freeIdx = h;
                break;
            }
        }
        if (freeIdx < 0) {
            fprintf(stderr, "[bothost] player %u's Bot Host refused: %d Bot "
                            "Hosts already connected\n",
                    (unsigned)owner, MAX_BOT_HOSTS);
            serverSendJoinReject(fromAddr, STR_REJECT_SERVER_FULL, 0, NULL);
            return;
        }
        {
            BotHostConn *hc = &udpServer.botHosts[freeIdx];
            memset(hc, 0, sizeof(*hc));
            hc->addr = *fromAddr;
            hc->connected = true;
            hc->connId = serverNextConnId();
            SDL_strlcpy(hc->name, hostName, sizeof(hc->name));
            hc->slots = slots;
            hc->cpu = (uint8_t)BOT_HOST_CPU_UNKNOWN;
            hc->cpuFree = (uint8_t)BOT_HOST_FREE_UNKNOWN;
            botHostOvrResetHost(freeIdx + 1);
            hc->lagKnown = false;
            hc->lagConnTick = udpServer.tickCount;
            hc->playerHost = true;
            hc->ownerSlot = owner;
            hc->ownerConnId = oc->connId;
            hc->lastReceivedTick = udpServer.tickCount;
            hc->outSequence = 1;
            channelMuxInit(&hc->channelMux);
            bulkSenderInit(&hc->bulk);
        }
        fprintf(stderr, "[bothost] player %u \"%s\" Bot Host connected from "
                        "%s with %u slots\n",
                (unsigned)owner, hostName, addr, (unsigned)slots);
        WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] player %u Bot Host \"%s\" "
                    "connected from %s slots=%u", (unsigned)owner, hostName,
                    addr, (unsigned)slots);
        goto accepted;
    }
    if (!udpServer.botHostEnabled || !udpServer.botHostOpen) {
        fprintf(stderr, "[bothost] Bot Host join from %s refused: this server "
                        "takes no Bot Host\n", addr);
        serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0, NULL);
        return;
    }
    if (!botHostListed(fromAddr->sin_addr.s_addr, listenPort, &entry)) {
        /* Not on the list: ignored, as its announces are. */
        botHostLogIgnored(fromAddr, "Bot Host join");
        return;
    }
    memcpy(key, extra, JOIN_BOT_HOST_KEY_LEN);
    memset(want, 0, sizeof(want));
    memcpy(want, udpServer.botHostKey,
           strlen(udpServer.botHostKey) < sizeof(want)
               ? strlen(udpServer.botHostKey) : sizeof(want));
    if (udpServer.botHostKey[0] != '\0' &&
        !botHostBytesEqual(key, want, JOIN_BOT_HOST_KEY_LEN)) {
        fprintf(stderr, "[bothost] Bot Host join from %s refused: wrong key\n",
                addr);
        WB_LOG_WARN(WB_LOG_CAT_NET, "[bothost] Bot Host join from %s refused",
                    addr);
        serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0, NULL);
        return;
    }
    slots = extra[JOIN_BOT_HOST_KEY_LEN];
    if (slots > BOT_HOST_MAX_SLOTS) slots = BOT_HOST_MAX_SLOTS;
    memcpy(hostName, extra + JOIN_BOT_HOST_KEY_LEN + 1, sizeof(hostName));
    hostName[sizeof(hostName) - 1] = '\0';
    if (hostName[0] == '\0') {
        SDL_strlcpy(hostName, name, sizeof(hostName));
    }
    if (listenPort != 0) {
        /* The same host joining again on a new socket: the old connection
         * is gone. */
        struct sockaddr_in la = *fromAddr;
        la.sin_port = htons(listenPort);
        h = botHostFindByListen(&la);
        if (h >= 0) botHostDrop(sim, h, "it joined again");
    }
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (!udpServer.botHosts[h].connected) {
            freeIdx = h;
            break;
        }
    }
    if (freeIdx < 0) {
        fprintf(stderr, "[bothost] Bot Host \"%s\" from %s refused: %d Bot "
                        "Hosts already connected\n",
                hostName, addr, MAX_BOT_HOSTS);
        serverSendJoinReject(fromAddr, STR_REJECT_SERVER_FULL, 0, NULL);
        return;
    }
    {
        BotHostConn *hc = &udpServer.botHosts[freeIdx];
        memset(hc, 0, sizeof(*hc));
        hc->addr = *fromAddr;
        if (listenPort != 0) {
            hc->listenAddr = *fromAddr;
            hc->listenAddr.sin_port = htons(listenPort);
        }
        hc->connected = true;
        hc->connId = serverNextConnId();
        SDL_strlcpy(hc->name, hostName, sizeof(hc->name));
        hc->slots = slots;
        hc->cpu = (uint8_t)BOT_HOST_CPU_UNKNOWN;
        hc->cpuFree = (uint8_t)BOT_HOST_FREE_UNKNOWN;
        botHostOvrResetHost(freeIdx + 1);
        hc->lagKnown = false;
        hc->lagConnTick = udpServer.tickCount;
        hc->lastReceivedTick = udpServer.tickCount;
        hc->outSequence = 1;
        channelMuxInit(&hc->channelMux);
        bulkSenderInit(&hc->bulk);
    }
    fprintf(stderr, "[bothost] Bot Host \"%s\" connected from %s with %u "
                    "slots\n", hostName, addr, (unsigned)slots);
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] Bot Host \"%s\" connected from %s "
                "slots=%u", hostName, addr, (unsigned)slots);
accepted:
    serverSendBotHostAccept(freeIdx, sim);
    serverBotRecHostConnected(freeIdx,
                              serverSimGetState(sim) == serverStateLobby);
    /* The brain files go out now: the list first, then each file the host
     * asks for. */
    botHostRefreshManifest(sim, "Bot Host connected");
    {
        BotHostConn *hc = &udpServer.botHosts[freeIdx];
        hc->manifestOwed = true;
        hc->syncSentTick = udpServer.tickCount;
    }
}

/* "GoalHunter 1a2b3c4d5e6f7a8b (current), ..." for the announce log line. */
static void botHostCacheText(const BotHostAnnounce *a, char *out, size_t sz) {
    int b;
    size_t n = 0;
    out[0] = '\0';
    if (a->brainCount == 0) {
        SDL_strlcpy(out, "none", sz);
        return;
    }
    for (b = 0; b < a->brainCount && n + 1 < sz; b++) {
        const BrainSyncBrain *cur =
            brainSyncFindBrain(&udpServer.brainManifest, a->brains[b].name);
        char hex[17];
        brainSyncVersionHex(a->brains[b].version, hex);
        n += (size_t)SDL_snprintf(out + n, sz - n, "%s%s %s%s",
                                  b > 0 ? ", " : "", a->brains[b].name, hex,
                                  (cur != NULL &&
                                   cur->version == a->brains[b].version)
                                      ? " (current)" : "");
    }
}

void serverBotHostHandleAnnounce(ServerSim *sim, const uint8_t *buf, int len,
                                 const struct sockaddr_in *fromAddr) {
    BotHostAnnounce a;
    char addr[48];
    int entry, h, freeIdx = -1;
    bool changed = true;

    if (sim == NULL || !udpServer.botHostEnabled || !udpServer.botHostOpen) {
        return;
    }
    if (len < PACKET_HEADER_SIZE ||
        !botHostAnnounceDecode(buf + PACKET_HEADER_SIZE,
                               (size_t)(len - PACKET_HEADER_SIZE), &a)) {
        return;
    }
    if (!botHostListed(fromAddr->sin_addr.s_addr, ntohs(fromAddr->sin_port),
                       &entry)) {
        botHostLogIgnored(fromAddr, "Bot Host announce");
        return;
    }
    /* A host already connected tells its slots over its connection. */
    if (botHostFindByListen(fromAddr) >= 0) return;
    botHostAddrStr(fromAddr, addr, sizeof(addr));
    if (entry >= 0) {
        changed = udpServer.botHostAnnounceTick[entry] == 0 ||
                  memcmp(&udpServer.botHostAnnounce[entry], &a, sizeof(a)) != 0;
        udpServer.botHostAnnounce[entry] = a;
        udpServer.botHostAnnounceTick[entry] = udpServer.tickCount;
        if (udpServer.botHostAnnounceTick[entry] == 0) {
            udpServer.botHostAnnounceTick[entry] = 1;
        }
    } else {
        /* No list: anyone may announce, so the server answers at most one
         * announce in each half second. */
        static uint32_t lastOpenTick = 0;
        static bool any = false;
        if (any && udpServer.tickCount - lastOpenTick < 25) return;
        any = true;
        lastOpenTick = udpServer.tickCount;
    }
    if (changed) {
        char cached[512];
        botHostCacheText(&a, cached, sizeof(cached));
        fprintf(stderr, "[bothost] Bot Host \"%s\" at %s announces %u of %u "
                        "slots free; cached brains: %s\n",
                a.name, addr, (unsigned)a.freeSlots, (unsigned)a.slots,
                cached);
    }
    if (a.freeSlots == 0) return;
    if (!botHostWanted(sim, NULL)) {
        if (changed) {
            fprintf(stderr, "[bothost] Bot Host \"%s\" not invited: no "
                            "player and no bot here now\n", a.name);
        }
        return;
    }
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (!udpServer.botHosts[h].connected) {
            freeIdx = h;
            break;
        }
    }
    if (freeIdx < 0) {
        if (changed) {
            fprintf(stderr, "[bothost] Bot Host \"%s\" not invited: %d Bot "
                            "Hosts already connected\n", a.name,
                    MAX_BOT_HOSTS);
        }
        return;
    }
    {
        uint8_t inv[PACKET_HEADER_SIZE + BOT_HOST_LINK_SHORT_LEN];
        packHeader(inv, PACKET_BOT_HOST_INVITE, 0);
        inv[PACKET_HEADER_SIZE] = BOT_HOST_LINK_PROTO;
        srvSendTo(inv, sizeof(inv), fromAddr);
    }
    fprintf(stderr, "[bothost] Bot Host \"%s\" at %s invited\n", a.name, addr);
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] Bot Host \"%s\" at %s invited",
                a.name, addr);
}

/* The seat's own alliance row and every other seat's, straight onto the bot
 * connection: a connection that joins mid-round cannot learn the matrix
 * from the join replay. */
static void botSlotSendAlliances(ServerSim *sim, int s) {
    ControlEvent evt;
    int i;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_RESET;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        evt.u.allianceReset.allies[i] =
            (uint16_t)(sim->sim.plyrs->item[i].allie | (1u << i));
    }
    udpClientDeliverControl(&udpServer.clients[s], &evt);
}

void serverBindBotSlot(ServerSim *sim, const struct sockaddr_in *fromAddr,
                       const uint8_t *extra, int extraLen,
                       uint8_t clientType) {
    char addr[48];
    int s, found = -1;
    BotHostSeat *seat;
    UdpServerClient *c;
    bool culled;

    (void)clientType;
    botHostAddrStr(fromAddr, addr, sizeof(addr));
    if (extraLen < JOIN_BOT_SLOT_TOKEN_LEN) {
        fprintf(stderr, "[bothost] bot connection from %s malformed\n", addr);
        return;
    }
    for (s = 0; s < MAX_TANKS; s++) {
        seat = &udpServer.botSeats[s];
        if (seat->state != BOT_SEAT_PENDING) continue;
        if (botHostBytesEqual(seat->token, extra, JOIN_BOT_SLOT_TOKEN_LEN)) {
            found = s;
        }
    }
    if (found >= 0 && udpServer.botSeats[found].host >= 0 &&
        udpServer.botHosts[udpServer.botSeats[found].host].playerHost) {
        /* A player's host: its bot connections come from its own address. */
        const BotHostConn *hc =
            &udpServer.botHosts[udpServer.botSeats[found].host];
        if (hc->addr.sin_addr.s_addr != fromAddr->sin_addr.s_addr) {
            botHostLogIgnored(fromAddr, "bot connection");
            return;
        }
    } else if (!botHostIpListed(fromAddr->sin_addr.s_addr)) {
        botHostLogIgnored(fromAddr, "bot connection");
        return;
    }
    if (found < 0 || udpServer.clients[found].connected ||
        !serverSimIsBot(sim, (BYTE)found)) {
        fprintf(stderr, "[bothost] bot connection from %s refused: no seat "
                        "waits for its token\n", addr);
        serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0, NULL);
        return;
    }
    s = found;
    seat = &udpServer.botSeats[s];
    c = &udpServer.clients[s];

    /* The token is good for one bind. */
    memset(seat->token, 0, sizeof(seat->token));

    /* Taken from a normal accept, without anything a player would see:
     * no roster join, no announce, no WBN event, no lobby change. The name,
     * country and client type stay the bot's. */
    c->connected = true;
    c->botSlot = true;
    c->connId = serverNextConnId();
    c->nameStickySuffix = false;
    c->addr = *fromAddr;
    c->playerNum = (uint8_t)s;
    c->lastReceivedTick = udpServer.tickCount;
    c->outSequence = 1;
    c->inboundCmdSeq = 0;
    c->lastPingTime = udpServer.tickCount;
    c->pingMs = 0;
    c->wantRejoin = false;
    c->wbnWasVerified = false;
    wbnJoinClear(&c->wbnJoin);
    udpServer.pendingSimRemove[s] = false;
    udpServer.mapEventQueues[s].nextSeq = 1;
    udpServer.mapEventQueues[s].ackedSeq = 1;
    memset(udpServer.mapEventQueues[s].buffer, 0,
           sizeof(udpServer.mapEventQueues[s].buffer));
    udpServer.mapEventQueueDrops[s] = 0;
    udpServer.mapChannelStalled[s] = false;
    udpServer.mapGen[s] = 0;
    channelMuxInit(&udpServer.channelMux[s]);
    udpServer.channelFramesRx[s] = 0;
    bulkSenderInit(&udpServer.bulkSend[s]);
    bulkReceiverInit(&udpServer.bulkRecvUp[s]);
    udpServerResetRoundLogLimits(s);
    udpServerResetMapReaskLimit(s);

    /* The live map, as an in-process bot reads it. A plain bot is then fed
     * the changes inside its view like any player; a full-view bot gets
     * every change. */
    culled = (seat->ai == (uint8_t)aiYes);
    serverSimSetShadowCulled(sim, (BYTE)s, false);
    serverSimShadowSeed(sim, (BYTE)s);
    serverSimSetShadowCulled(sim, (BYTE)s, culled);
    fprintf(stderr, "[bothost] bot %d \"%s\" map view: culled=%d ai=%u\n",
            s, botSeatName(sim, s), culled ? 1 : 0, (unsigned)seat->ai);
    {
        int mapLen = serverSimGetCompressedMapFor(sim, (BYTE)s,
                                                  udpServer.compressedMap,
                                                  (int)sizeof(udpServer.compressedMap));
        if (mapLen <= 0) {
            c->connected = false;
            c->botSlot = false;
            serverSimSetShadowCulled(sim, (BYTE)s, false);
            serverSimShadowSeed(sim, (BYTE)s);
            serverSendJoinReject(fromAddr, NETERR_MAPSERIALIZE, 0, NULL);
            return;
        }
        udpServer.compressedMapSize = (uint32_t)mapLen;
    }
    serverSendJoinAccept(s, sim, fromAddr);

    udpServer.controlSyncInProgress[s] = true;
    c->controlSub = serverSimRegisterSubscriber(sim, udpClientDeliverControl, c);
    udpServer.controlSyncInProgress[s] = false;
    botSlotSendAlliances(sim, s);
    if (serverSimGetState(sim) != serverStateRunning) {
        transportUdpServerFlushChannel(s);
    }
    serverInitMapDownload(s);

    seat->state = BOT_SEAT_BOUND;
    seat->sinceTick = udpServer.tickCount;
    seat->mineSeedDone = false;
    seat->mineSeedColumn = 0;
    fprintf(stderr, "[bothost] bot %d \"%s\" bound to Bot Host \"%s\" from %s\n",
            s, botSeatName(sim, s),
            seat->host >= 0 ? udpServer.botHosts[seat->host].name : "?", addr);
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] bot %d bound from %s", s, addr);
}

bool serverBotSlotOnInput(ServerSim *sim, int slot) {
    BotHostSeat *seat = &udpServer.botSeats[slot];
    if (seat->state == BOT_SEAT_REMOTE) return true;
    if (seat->state != BOT_SEAT_BOUND) return false;
    if (!botManagerSetRemote(sim, (BYTE)slot)) {
        botSeatFallBack(sim, slot, "seat cannot move", true);
        return false;
    }
    seat->state = BOT_SEAT_REMOTE;
    seat->sinceTick = udpServer.tickCount;
    seat->lastPollTick = udpServer.tickCount;
    fprintf(stderr, "[bothost] bot %d \"%s\" now thinks on Bot Host \"%s\" "
                    "(game tick %u)\n",
            slot, botSeatName(sim, slot),
            seat->host >= 0 ? udpServer.botHosts[seat->host].name : "?",
            (unsigned)serverSimGetTick(sim));
    return true;
}

void serverBotSlotTeamMsg(ServerSim *sim, int slot, const char *text) {
    if (!botManagerIsRemote(sim, (BYTE)slot)) return;
    botManagerDeliverInternalMessage(sim, (BYTE)slot, text);
}

void serverBotSlotStats(ServerSim *sim, int slot, const CmdBotStats *st) {
    botManagerSetRemoteStats(sim, (BYTE)slot, st);
}

bool serverBotSlotPingAllowed(ServerSim *sim, int slot) {
    return botManagerRemotePingAllowed(sim, (BYTE)slot);
}

bool serverBotHostHandlePing(const uint8_t *buf, int len,
                             const struct sockaddr_in *fromAddr) {
    int h = serverFindBotHost(fromAddr);
    BotHostConn *hc;
    uint32_t clientTime, echoed, now;
    uint8_t pong[PACKET_HEADER_SIZE + 8];

    if (h < 0) return false;
    hc = &udpServer.botHosts[h];
    hc->lastReceivedTick = udpServer.tickCount;
    if (len < PACKET_HEADER_SIZE + 8) return true;
    clientTime = unpackU32(buf + PACKET_HEADER_SIZE);
    echoed = unpackU32(buf + PACKET_HEADER_SIZE + 4);
    now = (uint32_t)srvNow();
    if (echoed > 0) hc->pingMs = (uint16_t)(now - echoed);
    if (clientTime == 0) return true;
    packHeader(pong, PACKET_PONG, 0);
    packU32(pong + PACKET_HEADER_SIZE, clientTime);
    packU32(pong + PACKET_HEADER_SIZE + 4, now);
    srvSendTo(pong, sizeof(pong), fromAddr);
    return true;
}

bool serverBotHostHandleChannel(const uint8_t *buf, int len,
                                const struct sockaddr_in *fromAddr) {
    int h = serverFindBotHost(fromAddr);
    BotHostConn *hc;
    uint8_t out[CHANNEL_MAX_SEG];
    uint16_t outLen;

    if (h < 0) return false;
    hc = &udpServer.botHosts[h];
    hc->lastReceivedTick = udpServer.tickCount;
    if (len > PACKET_HEADER_SIZE) {
        (void)channelRecvFrame(&hc->channelMux, buf + PACKET_HEADER_SIZE,
                               len - PACKET_HEADER_SIZE);
    }
    /* The host sends its file requests and sync reports on CHANNEL_CONTROL
     * and nothing else. */
    outLen = sizeof(out);
    while (channelReceive(&hc->channelMux, CHANNEL_CONTROL, out, &outLen)) {
        botHostOnControl(h, out, outLen);
        if (!hc->connected) return true;
        outLen = sizeof(out);
    }
    /* Its brain recordings come on CHANNEL_BULK. */
    serverBotRecDrainBulk(h);
    return true;
}

bool serverBotHostHandleQuit(ServerSim *sim, const struct sockaddr_in *fromAddr) {
    int h = serverFindBotHost(fromAddr);
    if (h < 0) return false;
    botHostDrop(sim, h, "quit");
    return true;
}

/* ── Bot manager hooks ─────────────────────────────────────────────────── */

static void botHookTeamMsg(void *ctx, BYTE seat, BYTE fromPlayer,
                           const char *msg) {
    BotHostSeat *bs;
    ControlEvent evt;
    (void)ctx;
    if (seat >= MAX_TANKS || msg == NULL) return;
    bs = &udpServer.botSeats[seat];
    if (bs->state != BOT_SEAT_REMOTE || bs->host < 0) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_TEAM_MSG;
    evt.u.botTeamMsg.seat = seat;
    evt.u.botTeamMsg.fromPlayer = fromPlayer;
    SDL_strlcpy(evt.u.botTeamMsg.text, msg, sizeof(evt.u.botTeamMsg.text));
    (void)botHostSendEvent(bs->host, &evt);
}

static void botHookHint(void *ctx, BYTE seat, const ScnTable *hint) {
    BotHostSeat *bs;
    (void)ctx;
    if (seat >= MAX_TANKS) return;
    bs = &udpServer.botSeats[seat];
    if (bs->state != BOT_SEAT_REMOTE || bs->host < 0) return;
    botHostSendTable(bs->host, CTRL_BOT_HINT, seat, hint);
}

static void botHookInitTable(void *ctx, BYTE seat) {
    ServerSim *sim = (ServerSim *)ctx;
    BotHostSeat *bs;
    if (seat >= MAX_TANKS) return;
    bs = &udpServer.botSeats[seat];
    if (bs->state == BOT_SEAT_LOCAL || bs->host < 0) return;
    botHostSendTable(bs->host, CTRL_BOT_INIT_TABLE, seat,
                     botManagerGetBotInitTable(sim, seat));
}

static bool botHookRoundStart(void *ctx, BYTE seat) {
    ServerSim *sim = (ServerSim *)ctx;
    BotHostSeat *bs;
    if (seat >= MAX_TANKS) return false;
    bs = &udpServer.botSeats[seat];
    if (bs->state == BOT_SEAT_REMOTE && bs->host >= 0 &&
        udpServer.botHosts[bs->host].connected &&
        udpServer.clients[seat].connected && udpServer.clients[seat].botSlot) {
        /* The host makes the round's new brain once the bot connection holds
         * the round's map. */
        bs->reloadOwed = true;
        bs->mineSeedDone = false;
        bs->mineSeedColumn = 0;
        return true;
    }
    /* No host to take it: the seat is built here. */
    botSlotRelease(sim, seat);
    botSeatSendUnassign(seat);
    botSeatReset(seat, false);
    return false;
}

static void botHookReleased(void *ctx, BYTE seat) {
    ServerSim *sim = (ServerSim *)ctx;
    if (seat >= MAX_TANKS) return;
    if (udpServer.botSeats[seat].state == BOT_SEAT_LOCAL) return;
    botSlotRelease(sim, seat);
    botSeatSendUnassign(seat);
    botSeatReset(seat, false);
}

static bool botHostRecHostedInfo(BYTE slot, uint16_t *placeGen,
                                 char *hostName);

/* Turn Bot Hosts on or off to match the key and the list. */
static void botHostApplySettings(ServerSim *sim) {
    BotRemoteHooks hooks;
    int s;
    bool want;

    udpServer.botHostOpen = botHostOpenToAll();
    want = udpServer.botHostOpen || !udpServer.noPlayerBotHosts;
    if (!want) {
        udpServer.botHostEnabled = false;
        botManagerSetRemoteHooks(sim, NULL);
        brainRecordSetHostedInfoFn(NULL);
        return;
    }
    if (udpServer.botHostEnabled) return;
    udpServer.botHostEnabled = true;
    udpServer.botHostLastState = -1;
    for (s = 0; s < MAX_TANKS; s++) {
        botSeatReset(s, false);
    }
    memset(&hooks, 0, sizeof(hooks));
    hooks.ctx        = sim;
    hooks.teamMsg    = botHookTeamMsg;
    hooks.hint       = botHookHint;
    hooks.initTable  = botHookInitTable;
    hooks.roundStart = botHookRoundStart;
    hooks.released   = botHookReleased;
    botManagerSetRemoteHooks(sim, &hooks);
    brainRecordSetHostedInfoFn(botHostRecHostedInfo);
    fprintf(stderr, "[bothost] Bot Host connections enabled\n");
    botHostRefreshManifest(sim, "start");
    udpServer.botHostQueryWhy = "server start";
}

/* brainrec.btr asks this per bot: a bot whose brain runs on a host gets the
 * hosted marker, with the gen of the assign that placed it there. */
static bool botHostRecHostedInfo(BYTE slot, uint16_t *placeGen,
                                 char *hostName) {
    const BotHostSeat *seat;
    if (slot >= MAX_TANKS || botHostSim == NULL) return false;
    seat = &udpServer.botSeats[slot];
    if (seat->state != BOT_SEAT_REMOTE || seat->host < 0 ||
        !botManagerIsRemote(botHostSim, slot)) {
        return false;
    }
    *placeGen = seat->placeGen;
    SDL_strlcpy(hostName, udpServer.botHosts[seat->host].name, 32);
    return true;
}

/* Say who may connect: the key and the list combine. */
static void botHostPrintRule(void) {
    if (!udpServer.botHostEnabled) return;
    if (udpServer.botHostAllowCount > 0 && udpServer.botHostKey[0] != '\0') {
        fprintf(stderr, "[bothost] Bot Hosts taken: the %d listed, and each "
                        "must give the key\n", udpServer.botHostAllowCount);
    } else if (udpServer.botHostAllowCount > 0) {
        fprintf(stderr, "[bothost] Bot Hosts taken: the %d listed (no key)\n",
                udpServer.botHostAllowCount);
    } else if (udpServer.botHostKey[0] != '\0') {
        fprintf(stderr, "[bothost] Bot Hosts taken: any that gives the key "
                        "(no list, so none is asked for free slots)\n");
    }
    if (!udpServer.noPlayerBotHosts) {
        fprintf(stderr, "[bothost] each player's client runs a Bot Host for "
                        "this server's bots (-noplayerbothosts turns it off)\n");
    }
}

bool transportUdpServerSetBotHostKey(ServerSim *sim, const char *key) {
    if (sim == NULL) return false;
    botHostSim = sim;
    memset(udpServer.botHostKey, 0, sizeof(udpServer.botHostKey));
    if (key != NULL && strlen(key) > JOIN_BOT_HOST_KEY_LEN) {
        fprintf(stderr, "[bothost] -bothostkey is longer than %d characters\n",
                JOIN_BOT_HOST_KEY_LEN);
        return false;
    }
    if (key != NULL) {
        SDL_strlcpy(udpServer.botHostKey, key, sizeof(udpServer.botHostKey));
    }
    botHostApplySettings(sim);
    return true;
}

bool transportUdpServerSetBotHostList(ServerSim *sim, const char *arg) {
    BotHostAddrList list;
    char err[256];
    int i, n = 0;

    if (sim == NULL) return false;
    botHostSim = sim;
    if (!botHostAddrListLoad(arg, &list, err, sizeof(err))) {
        fprintf(stderr, "[bothost] -bothosts not used: %s\n", err);
        return false;
    }
    for (i = 0; i < list.count; i++) {
        struct in_addr ip;
        ip.s_addr = inet_addr(list.e[i].host);
        if (ip.s_addr == INADDR_NONE &&
            bolo_resolve_ipv4(list.e[i].host, &ip) != 0) {
            fprintf(stderr, "[bothost] -bothosts: cannot resolve \"%s\"; "
                            "list not used\n", list.e[i].host);
            return false;
        }
        udpServer.botHostAllow[n].ip = ip.s_addr;
        udpServer.botHostAllow[n].port = list.e[i].port;
        SDL_snprintf(udpServer.botHostAllowText[n],
                     sizeof(udpServer.botHostAllowText[n]), "%s:%u",
                     inet_ntoa(ip),
                     (unsigned)(list.e[i].port != 0
                                    ? list.e[i].port
                                    : BOT_HOST_DEFAULT_LISTEN_PORT));
        n++;
    }
    udpServer.botHostAllowCount = n;
    memset(udpServer.botHostAnnounce, 0, sizeof(udpServer.botHostAnnounce));
    memset(udpServer.botHostAnnounceTick, 0,
           sizeof(udpServer.botHostAnnounceTick));
    for (i = 0; i < n; i++) {
        fprintf(stderr, "[bothost] listed Bot Host: %s\n",
                udpServer.botHostAllowText[i]);
    }
    botHostApplySettings(sim);
    return true;
}

void transportUdpServerSetPlayerBotHosts(ServerSim *sim, bool on) {
    if (sim == NULL) return;
    botHostSim = sim;
    udpServer.noPlayerBotHosts = !on;
    if (!on) {
        int h;
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            if (udpServer.botHosts[h].connected &&
                udpServer.botHosts[h].playerHost) {
                botHostDrop(sim, h, "player Bot Hosts turned off");
            }
        }
    }
    botHostApplySettings(sim);
}

/* ── Placement settings ───────────────────────────────────────────────── */

static int botHostSmallSlots = BOT_HOST_SMALL_SLOTS_DEFAULT;
static double botHostSmallBias = BOT_HOST_SMALL_BIAS_DEFAULT;
static BotHostPlaceLag botHostLag = {
    BOT_HOST_LAG_REF_MS_DEFAULT, BOT_HOST_LAG_MAX_MS_DEFAULT,
    BOT_HOST_LAG_GAIN_DEFAULT
};

void transportUdpServerSetBotHostLag(int refMs, int maxMs) {
    if (refMs < 0) refMs = BOT_HOST_LAG_REF_MS_DEFAULT;
    if (maxMs < 0) maxMs = BOT_HOST_LAG_MAX_MS_DEFAULT;
    if (refMs > 10000) refMs = 10000;
    if (maxMs > 60000) maxMs = 60000;
    botHostLag.refMs = (double)refMs;
    botHostLag.maxMs = (double)maxMs;
    if (refMs == 0) {
        fprintf(stderr, "[bothost] placement: lag not counted\n");
    } else if (maxMs == 0) {
        fprintf(stderr, "[bothost] placement: lag counts 1 + lag / %d ms, "
                        "no lag ceiling\n", refMs);
    } else {
        fprintf(stderr, "[bothost] placement: lag counts 1 + lag / %d ms; "
                        "a host over %d ms gets no new bots while one at "
                        "or under it has room\n", refMs, maxMs);
    }
}

void transportUdpServerSetBotHostPlacement(int smallSlots, double bias) {
    if (smallSlots < 0) smallSlots = BOT_HOST_SMALL_SLOTS_DEFAULT;
    if (smallSlots > 255) smallSlots = 255;
    if (!(bias >= 0.0)) bias = BOT_HOST_SMALL_BIAS_DEFAULT; /* also a NaN */
    if (bias > 1.0) bias = 1.0;
    botHostSmallSlots = smallSlots;
    botHostSmallBias = bias;
    fprintf(stderr, "[bothost] placement: a host of %d slot%s or fewer counts "
                    "%.2f slot%s fewer\n",
            smallSlots, smallSlots == 1 ? "" : "s", bias,
            bias == 1.0 ? "" : "s");
}

/* Each player gets a ticket for the host inside its client: once a second
 * after its connection is made (so the join's own events are done), again
 * at each game start and in the lobby, since a client may begin a new
 * session for a round. The ticket is the same for the whole connection;
 * a new connection (a rejoin) gets a new one. */
#define BOT_HOST_TICKET_DELAY_TICKS 50

static void botHostServiceTickets(ServerSim *sim) {
    int s;
    uint32_t starts = botManagerGameStarts(sim);
    char brain[BOT_HOST_BRAIN_NAME_LEN];

    if (!udpServer.botHostEnabled || udpServer.noPlayerBotHosts) return;
    brain[0] = '\0';
    for (s = 0; s < MAX_TANKS; s++) {
        /* The brain the server's bots run, for the host's capacity. */
        const char *path;
        if (!botManagerIsBot(sim, (BYTE)s)) continue;
        path = botManagerGetBrainPath(sim, (BYTE)s);
        if (path == NULL || path[0] == '\0') continue;
        botHostBrainName(path, brain, sizeof(brain));
        break;
    }
    for (s = 0; s < MAX_TANKS; s++) {
        UdpServerClient *c = &udpServer.clients[s];
        ControlEvent evt;
        if (!c->connected || c->botSlot || c->connId == 0) continue;
        if (udpServer.controlSyncInProgress[s]) continue;
        if (udpServer.playerTicketConnId[s] != c->connId) {
            if (!serverFillRandomBytes(udpServer.playerTicket[s],
                                       BOT_HOST_TICKET_LEN)) {
                continue;
            }
            udpServer.playerTicketConnId[s] = c->connId;
            /* First send after the delay. */
            udpServer.playerTicketTick[s] = udpServer.tickCount;
            udpServer.playerTicketStarts[s] = 0xFFFFFFFFu;
            continue;
        }
        if (udpServer.playerTicketStarts[s] == starts) continue;
        if (udpServer.tickCount - udpServer.playerTicketTick[s] <
            BOT_HOST_TICKET_DELAY_TICKS) {
            continue;
        }
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_BOT_HOST_TICKET;
        evt.u.botHostTicket.player = (uint8_t)s;
        memcpy(evt.u.botHostTicket.ticket, udpServer.playerTicket[s],
               BOT_HOST_TICKET_LEN);
        SDL_strlcpy(evt.u.botHostTicket.brain, brain,
                    sizeof(evt.u.botHostTicket.brain));
        udpClientDeliverControl(c, &evt);
        if (udpServer.playerTicketStarts[s] == 0xFFFFFFFFu) {
            fprintf(stderr, "[bothost] player %d \"%s\" sent a Bot Host "
                            "ticket\n", s, c->playerName);
        }
        udpServer.playerTicketStarts[s] = starts;
        udpServer.playerTicketTick[s] = udpServer.tickCount;
    }
}

void serverBotHostsReset(void) {
    int s, h;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        botHostFileReset(&udpServer.botHosts[h]);
        bulkSenderReset(&udpServer.botHosts[h].bulk);
        serverBotRecHostGone(h);
    }
    memset(udpServer.botHosts, 0, sizeof(udpServer.botHosts));
    memset(botHostRowSent, 0, sizeof(botHostRowSent));
    memset(botHostCpuSent, BOT_HOST_CPU_UNKNOWN, sizeof(botHostCpuSent));
    botHostCpuSentInit = true;
    memset(botHostCpuLogged, 0, sizeof(botHostCpuLogged));
    botHostServerCpuInit = false;
    botHostServerCpuValue = (uint8_t)BOT_HOST_CPU_UNKNOWN;
    for (h = 0; h <= MAX_BOT_HOSTS; h++) botHostOvrResetHost(h);
    botHostServerFreeHave = false;
    botHostServerFreeValue = (uint8_t)BOT_HOST_FREE_UNKNOWN;
    botHostRecWasOn = false;
    for (s = 0; s < MAX_TANKS; s++) {
        memset(&udpServer.botSeats[s], 0, sizeof(udpServer.botSeats[s]));
        udpServer.botSeats[s].host = -1;
    }
    udpServer.botHostLastState = -1;
}

/* ── Per-tick service ──────────────────────────────────────────────────── */

/* A full-view seat reads every mine on the map, as an in-process full-view
 * bot does from the server map. Its bot connection is sent the server's mine
 * list a few columns per tick once its map download is done; later mines
 * reach it through the event drain. */
static void botSeatSeedMines(ServerSim *sim, int s) {
    BotHostSeat *seat = &udpServer.botSeats[s];
    GameSim *gs = serverSimGetGameSim(sim);
    int sent = 0;

    if (seat->mineSeedDone) return;
    if (serverSimIsShadowCulled(sim, (BYTE)s) || gs == NULL ||
        gs->mns == NULL || !gs->mns->minesHiddenMines) {
        seat->mineSeedDone = true;
        return;
    }
    while (seat->mineSeedColumn < MINES_ARRAY_SIZE &&
           sent < BOT_SEAT_MINES_PER_TICK) {
        int x = seat->mineSeedColumn;
        int y;
        bool full = false;
        for (y = 0; y < MINES_ARRAY_SIZE; y++) {
            GameEvent ev;
            uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
            int evLen;
            if (!gs->mns->pos[x][y]) continue;
            memset(&ev, 0, sizeof(ev));
            ev.type = EVENT_MINE_VISIBLE;
            ev.data[0] = (BYTE)x;
            ev.data[1] = (BYTE)y;
            ev.data[2] = 0x80;
            evLen = packGameEvent(evBuf, &ev);
            if (!channelSend(&udpServer.channelMux[s], CHANNEL_GAME, evBuf,
                             (uint16_t)evLen)) {
                full = true;
                break;
            }
            sent++;
        }
        if (full) return;  /* the column again next tick */
        seat->mineSeedColumn++;
    }
    if (seat->mineSeedColumn >= MINES_ARRAY_SIZE) seat->mineSeedDone = true;
}

static void botHostPump(int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    int frames;
    bulkSenderPump(&hc->bulk, &hc->channelMux);
    channelTick(&hc->channelMux, udpServer.tickCount, hc->pingMs);
    for (frames = 0; frames < MAP_DOWNLOAD_FRAMES_PER_TICK; frames++) {
        uint8_t cbuf[UDP_MAX_PAYLOAD];
        int frameLen = channelBuildFrame(&hc->channelMux,
                                         cbuf + PACKET_HEADER_SIZE,
                                         UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
        if (frameLen <= 2) break;
        packHeader(cbuf, PACKET_CHANNEL, hc->outSequence++);
        srvSendTo(cbuf, PACKET_HEADER_SIZE + frameLen, &hc->addr);
    }
}

/* ── Placement ─────────────────────────────────────────────────────────── */

/* Ticks between two moves while a game runs, so bots do not jump around:
 * 3 s. In the first 10 s of a game a move may happen each tick, so the bots
 * of a game with no lobby reach their hosts at once. */
#define BOT_HOST_MOVE_TICKS        150
#define BOT_HOST_START_PLACE_TICKS 500
/* Ticks between looks at the server's own capacity answer. */
#define BOT_HOST_CAP_POLL_TICKS    50

/* The server's own slots: the smallest capacity answer among its bots'
 * brains, or the last one known, or the default for this machine (logical
 * cores - 1). */
static int botHostServerSlots(ServerSim *sim) {
    static int known = -1;
    static const char *knownFrom = "";
    static uint32_t lastTick = 0;
    static bool polled = false;
    if (!polled || udpServer.tickCount - lastTick >= BOT_HOST_CAP_POLL_TICKS) {
        BotPoolStats ps;
        polled = true;
        lastTick = udpServer.tickCount;
        memset(&ps, 0, sizeof(ps));
        botManagerGetPoolStats(sim, &ps);
        /* The host bot capacity (-botcapacity, or the CPU speed test with
           the bot brain's answer) comes first. -1 (no cap) and -2 (the test
           still runs) fall back to the active brains' answer. */
        if (ps.hostBotCapacity >= 0) {
            known = ps.hostBotCapacity;
            knownFrom = "the server's host bot capacity";
        } else if (ps.brainCapacity > 0) {
            known = ps.brainCapacity;
            knownFrom = "the brain's capacity answer";
        }
    }
    {
        static int logged = -1;
        int slots = known;
        const char *from = knownFrom;
        if (slots < 0) {
            BrainMachineInfo machine;
            memset(&machine, 0, sizeof(machine));
            brainCapacityGetMachine(&machine);
            slots = brainCapacityDefault(&machine);
            from = "this machine's default capacity";
        }
        if (slots != logged) {
            logged = slots;
            fprintf(stderr, "[bothost] the server counts as a host with %d "
                    "slots (%s)\n", slots, from);
        }
        return slots;
    }
}

static int botHostServerSlotsRow(ServerSim *sim) {
    return botHostServerSlots(sim);
}

int serverBotHostServerSlots(ServerSim *sim) {
    if (sim == NULL) return 0;
    return botHostServerSlots(sim);
}

/* The bots the server runs itself now (in-process brains). */
int serverBotHostServerBots(ServerSim *sim) {
    int s, n = 0;
    if (sim == NULL) return 0;
    for (s = 0; s < MAX_TANKS; s++) {
        if (udpServer.botSeats[s].state != BOT_SEAT_LOCAL) continue;
        if (!botManagerIsBot(sim, (BYTE)s)) continue;
        if (botManagerIsRemote(sim, (BYTE)s)) continue;
        n++;
    }
    return n;
}

/* The bot may change where its brain runs now: in a running game not while
 * it carries a pill or its builder is out. */
static bool botSeatMayMove(ServerSim *sim, int s, bool running) {
    if (!running) return true;
    if (sim->sim.tanks[s] != NULL &&
        tankGetNumCarriedPills(&sim->sim.tanks[s]) > 0) {
        return false;
    }
    if (sim->sim.lgmen[s] != NULL && lgmIsOut(&sim->sim.lgmen[s])) {
        return false;
    }
    return true;
}

/* An in-process bot seat that may be offered to a host. */
static bool botSeatLocalOfferable(ServerSim *sim, int s) {
    const BotHostSeat *seat = &udpServer.botSeats[s];
    const char *path;
    char brainName[BOT_HOST_BRAIN_NAME_LEN];
    if (seat->state != BOT_SEAT_LOCAL || seat->noReturn) return false;
    if (!botManagerIsBot(sim, (BYTE)s)) return false;
    if (botManagerIsRemote(sim, (BYTE)s)) return false;
    if (udpServer.clients[s].connected) return false;
    path = botManagerGetBrainPath(sim, (BYTE)s);
    if (path == NULL || path[0] == '\0') return false;
    /* A host runs only brains whose files it was sent. */
    botHostBrainName(path, brainName, sizeof(brainName));
    return brainSyncFindBrain(&udpServer.brainManifest, brainName) != NULL;
}

/* Offers in-process seat s to host h. */
static bool botSeatOffer(ServerSim *sim, int s, int h, const char *why) {
    BotHostSeat *seat = &udpServer.botSeats[s];
    if (!serverFillRandomBytes(seat->token, sizeof(seat->token))) {
        WB_LOG_ERROR(WB_LOG_CAT_NET, "[bothost] no random bytes for a token");
        return false;
    }
    seat->state = BOT_SEAT_PENDING;
    seat->host = (int8_t)h;
    seat->sinceTick = udpServer.tickCount;
    seat->reloadOwed = false;
    botSeatSendAssign(sim, s, false);
    fprintf(stderr, "[bothost] bot %d \"%s\" offered to Bot Host \"%s\" "
                    "(%s, %d/%u slots used)\n",
            s, botSeatName(sim, s), udpServer.botHosts[h].name, why,
            botHostUsedSlots(h), (unsigned)udpServer.botHosts[h].slots);
    return true;
}

/* Ticks after a join when a round trip of 0 ms still means "not measured
 * yet". */
#define BOT_HOST_LAG_WAIT_TICKS    250

/* Brings the smoothed lag of host h up to date, once a tick. */
static void botHostUpdateLag(int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    uint16_t raw;
    if (hc->playerHost) {
        const UdpServerClient *c;
        if (hc->ownerSlot >= MAX_TANKS) return;
        c = &udpServer.clients[hc->ownerSlot];
        if (!c->connected) return;
        raw = c->pingMs;
    } else {
        raw = hc->pingMs;
    }
    /* 0 is the value before the first pong: count it only once the host
     * has been here long enough to have one (a LAN round trip can be
     * under 1 ms). */
    if (raw == 0 && udpServer.tickCount - hc->lagConnTick <
                    BOT_HOST_LAG_WAIT_TICKS) {
        return;
    }
    botHostLagSmooth(&hc->lagMs, &hc->lagKnown, (double)raw,
                     BOT_HOST_LAG_SMOOTH_TICKS);
}

/* ── Bot overruns on a host ────────────────────────────────────────────── */

/* The overrun rules (botHostOverrunProbe in bot_host_link.c): once a second
 * each host, the server's own process too, is probed for its bots' new
 * overruns over their new thinks. Index i is the place in the placement's
 * host table: 0 the server, h + 1 Bot Host h. */
#define BOT_HOST_OVR_PROBE_TICKS 50

static const BotHostOverrunCfg *botHostOvrConfig(void) {
    if (!botHostOvrCfgSet) {
        botHostOverrunCfgDefaults(&botHostOvrCfg);
        botHostOvrCfgSet = true;
    }
    return &botHostOvrCfg;
}

/* The newest free CPU percent of host i, or BOT_HOST_FREE_UNKNOWN when it is
 * older than two readings (a Bot Host sends one each 5 s). */
static uint8_t botHostOvrFree(int i) {
    const BotHostOverrunCfg *cfg = botHostOvrConfig();
    int secs = cfg->freeSampleSecs > 5 ? cfg->freeSampleSecs : 5;
    uint32_t fresh = (uint32_t)secs * 2u * 50u;
    if (i == 0) {
        if (botHostServerFreeValue > 100 ||
            udpServer.tickCount - botHostServerFreeTick > fresh) {
            return (uint8_t)BOT_HOST_FREE_UNKNOWN;
        }
        return botHostServerFreeValue;
    }
    {
        const BotHostConn *hc = &udpServer.botHosts[i - 1];
        if (!hc->connected || hc->cpuFree > 100 ||
            udpServer.tickCount - hc->cpuFreeTick > fresh) {
            return (uint8_t)BOT_HOST_FREE_UNKNOWN;
        }
        return hc->cpuFree;
    }
}

static const char *botHostOvrName(int i) {
    return i == 0 ? "the server" : udpServer.botHosts[i - 1].name;
}

static const char *botHostOvrKind(int i) {
    return i == 0 ? "" : "Bot Host ";
}

/* The numbers a decision used, for the console. */
static void botHostOvrNumbers(const BotHostOverrunReport *rep, char *out,
                              size_t sz) {
    char avg[16], cost[16], pred[16], lc[16], hb[16];
    if (rep->freeKnown) {
        SDL_snprintf(avg, sizeof(avg), "%.1f%%", rep->freeAvg);
    } else {
        SDL_strlcpy(avg, "unknown", sizeof(avg));
    }
    if (rep->costKnown) {
        SDL_snprintf(cost, sizeof(cost), "%.2f%%", rep->oneBotCost);
    } else {
        SDL_strlcpy(cost, "unknown", sizeof(cost));
    }
    if (rep->freeKnown && rep->costKnown) {
        SDL_snprintf(pred, sizeof(pred), "%.1f%%", rep->predicted);
    } else {
        SDL_strlcpy(pred, "unknown", sizeof(pred));
    }
    if (rep->lowestClean >= 0) {
        SDL_snprintf(lc, sizeof(lc), "%d%%", rep->lowestClean);
    } else {
        SDL_strlcpy(lc, "none", sizeof(lc));
    }
    if (rep->highestBad >= 0) {
        SDL_snprintf(hb, sizeof(hb), "%d%%", rep->highestBad);
    } else {
        SDL_strlcpy(hb, "none", sizeof(hb));
    }
    SDL_snprintf(out, sz, "free CPU average %s, one-bot cost %s, predicted "
                 "free %s, lowest clean free %s, highest bad free %s",
                 avg, cost, pred, lc, hb);
}

static void botHostOvrLimitText(int limit, int slots, char *out, size_t sz) {
    if (limit < 0) {
        SDL_snprintf(out, sz, "none (%d slots)", slots);
    } else {
        SDL_snprintf(out, sz, "%d", limit);
    }
}

/* The round log (.wbv) records: only with -record-bothost-data (or the
 * client's "Record Bot Host Data" setting) and while the log records. */
static bool botHostRecOn(void) {
    return botHostRecordData && logIsRecording();
}

static void botHostRecPut(const BotHostRecord *rec) {
    uint8_t buf[BOTHOST_RECORD_MAX_BYTES];
    int n = botHostRecordEncode(rec, buf, (int)sizeof(buf));
    if (n > 0) {
        logAddEvent(log_BotHostData, 0, 0, 0, 0, (unsigned short)n,
                    (char *)buf);
    }
}

bool serverBotHostRecordPut(const BotHostRecord *rec) {
    if (rec == NULL || !botHostRecOn()) return false;
    botHostRecPut(rec);
    return true;
}

static void botHostRecHost(int i) {
    BotHostRecord r;
    memset(&r, 0, sizeof(r));
    r.kind = BOTHOST_RECORD_HOST;
    r.host = (uint8_t)i;
    r.player = (uint8_t)BOTHOST_RECORD_NONE;
    if (i == 0) {
        SDL_strlcpy(r.name, "Server", sizeof(r.name));
    } else {
        const BotHostConn *hc = &udpServer.botHosts[i - 1];
        if (hc->playerHost && hc->ownerSlot < MAX_TANKS) {
            r.player = hc->ownerSlot;
        }
        SDL_strlcpy(r.name, hc->name, sizeof(r.name));
    }
    botHostRecPut(&r);
    botHostRecHostSent[i] = true;
}

static void botHostRecDecision(int i, uint8_t decision,
                               const BotHostOverrunReport *rep, int seat,
                               int toHost, const char *reason) {
    BotHostRecord r;
    if (!botHostRecOn()) return;
    memset(&r, 0, sizeof(r));
    r.kind = BOTHOST_RECORD_DECISION;
    r.host = (uint8_t)i;
    r.decision = decision;
    r.limitBefore = botHostRecordByte(rep->limitBefore);
    r.limitAfter = botHostRecordByte(rep->newLimit);
    r.badProbes = botHostRecordByte(rep->badProbes);
    r.probes = botHostRecordByte(rep->probes);
    r.flags = (uint8_t)((rep->costKnown ? BOTHOST_RECORD_DEC_COST : 0u) |
                        (rep->freeKnown ? BOTHOST_RECORD_DEC_FREE : 0u));
    r.predictedTenths = botHostRecordTenthsS(rep->predicted);
    r.costTenths = botHostRecordTenthsU(rep->oneBotCost);
    r.freeAvgTenths = botHostRecordTenthsU(rep->freeAvg);
    r.lowestClean = botHostRecordByte(rep->lowestClean);
    r.highestBad = botHostRecordByte(rep->highestBad);
    r.seat = botHostRecordByte(seat);
    r.toHost = botHostRecordByte(toHost);
    SDL_strlcpy(r.reason, reason != NULL ? reason : "", sizeof(r.reason));
    botHostRecPut(&r);
}

/* The lowered limits and the free CPU check, as part of the placement: a
 * host at its lowered limit has no room, and a host whose free CPU does not
 * show room for one more bot takes none. The server stays the last
 * fall-back: this only changes where the spread puts bots. */
static void botHostOvrApply(BotHostPlaceHost *hosts, bool log) {
    const BotHostOverrunCfg *cfg = botHostOvrConfig();
    int i;
    if (!(cfg->pct > 0.0)) return;
    botHostOvrEnsure();
    for (i = 0; i <= MAX_BOT_HOSTS; i++) {
        BotHostOverrunReport rep;
        int lim;
        if (i > 0 && !udpServer.botHosts[i - 1].connected) continue;
        lim = botHostOverrunLimit(&botHostOvr[i], hosts[i].slots);
        if (lim < hosts[i].slots) {
            hosts[i].slots = lim;
            if (i == 0) hosts[0].unlimited = false;
        }
        if (!hosts[i].open) continue;
        if (hosts[i].bots >= hosts[i].slots && !hosts[i].unlimited) continue;
        if (botHostOverrunMayTake(&botHostOvr[i], cfg, hosts[i].bots,
                                  botHostOvrFree(i), &rep)) {
            if (log) botHostOvrRefuseWhy[i] = NULL;
            continue;
        }
        hosts[i].open = false;
        if (log && botHostOvrRefuseWhy[i] != rep.why) {
            char nums[200];
            botHostOvrRefuseWhy[i] = rep.why;
            botHostOvrNumbers(&rep, nums, sizeof(nums));
            fprintf(stderr, "[bothost] placement: %s\"%s\" takes no new bot "
                            "now (%s; %d bots; %s)\n",
                    botHostOvrKind(i), botHostOvrName(i), rep.why,
                    hosts[i].bots, nums);
            botHostRecDecision(i, BOTHOST_DECISION_PLACE_REFUSED, &rep, -1,
                               -1, rep.why);
        }
    }
}

/* One probe of every host. raw is the host table before the limits, applied
 * the table after them (where a bot moves to). Returns true when a bot
 * moved. */
static bool botHostOvrProbeAll(ServerSim *sim, const BotHostPlaceHost *raw,
                               const BotHostPlaceHost *applied,
                               int localSeat, const int *hostSeat,
                               bool running) {
    const BotHostOverrunCfg *cfg = botHostOvrConfig();
    uint32_t over[1 + MAX_BOT_HOSTS], thinks[1 + MAX_BOT_HOSTS];
    int bots[1 + MAX_BOT_HOSTS];
    bool recOn, moved = false;
    int i, s;

    botHostOvrEnsure();
    memset(over, 0, sizeof(over));
    memset(thinks, 0, sizeof(thinks));
    memset(bots, 0, sizeof(bots));

    /* The server's own free CPU, each sample. */
    {
        int secs = cfg->freeSampleSecs > 0 ? cfg->freeSampleSecs : 1;
        if (!botHostServerFreeHave ||
            udpServer.tickCount - botHostServerFreeTick >=
                (uint32_t)secs * 50u) {
            if (!botHostServerFreeHave) machineCpuFreeInit(&botHostServerFree);
            botHostServerFreeHave = true;
            botHostServerFreeValue = machineCpuFreeSample(&botHostServerFree);
            botHostServerFreeTick = udpServer.tickCount;
        }
    }

    /* Each bot's new thinks and overruns, counted to where its brain ran.
     * A bot that changed place (or whose counters went back, at a round
     * start) starts again from its counters now. */
    for (s = 0; s < MAX_TANKS; s++) {
        BotInfo bi;
        int owner = -1;
        memset(&bi, 0, sizeof(bi));
        if (botManagerIsBot(sim, (BYTE)s) &&
            botManagerGetBotInfo(sim, (BYTE)s, &bi)) {
            if (botManagerIsRemote(sim, (BYTE)s)) {
                int h = udpServer.botSeats[s].host;
                if (h >= 0 && h < MAX_BOT_HOSTS &&
                    udpServer.botHosts[h].connected) {
                    owner = h + 1;
                }
            } else {
                owner = 0;
            }
        }
        if (owner < 0) {
            botHostOvrSeat[s].owner = -1;
            continue;
        }
        bots[owner]++;
        if (botHostOvrSeat[s].owner == owner &&
            bi.thinkCount >= botHostOvrSeat[s].thinks &&
            bi.overrunCount >= botHostOvrSeat[s].overruns) {
            thinks[owner] += bi.thinkCount - botHostOvrSeat[s].thinks;
            over[owner] += bi.overrunCount - botHostOvrSeat[s].overruns;
        }
        botHostOvrSeat[s].owner = owner;
        botHostOvrSeat[s].thinks = bi.thinkCount;
        botHostOvrSeat[s].overruns = bi.overrunCount;
    }

    recOn = botHostRecOn();
    if (recOn && !botHostRecWasOn) {
        /* A new log: each host is named again first. */
        memset(botHostRecHostSent, 0, sizeof(botHostRecHostSent));
    }
    botHostRecWasOn = recOn;

    for (i = 0; i <= MAX_BOT_HOSTS; i++) {
        BotHostOverrunProbe in;
        BotHostOverrunReport rep;
        BotHostOverrunAction act;
        int seat, dest = -1;
        char nums[200], before[32], after[32];

        if (i > 0 && !udpServer.botHosts[i - 1].connected) continue;
        if (recOn && !botHostRecHostSent[i]) botHostRecHost(i);

        memset(&in, 0, sizeof(in));
        in.overruns = over[i];
        in.thinks = thinks[i];
        in.bots = bots[i];
        in.slots = raw[i].slots;
        in.botCpu = i == 0 ? botHostServerCpuValue
                           : udpServer.botHosts[i - 1].cpu;
        in.freePct = botHostOvrFree(i);
        seat = i == 0 ? localSeat : hostSeat[i - 1];
        if (seat >= 0) {
            dest = botHostPlacePickOther(applied, 1 + MAX_BOT_HOSTS,
                                         botHostSmallSlots, botHostSmallBias,
                                         &botHostLag, i);
            /* The server is the last fall-back for a Bot Host's bot. */
            if (dest < 0 && i > 0) dest = 0;
            if (dest < 0) seat = -1;
        }
        in.canMove = seat >= 0;

        act = botHostOverrunProbe(&botHostOvr[i], cfg, &in, &rep);

        if (recOn) {
            BotHostRecord r;
            memset(&r, 0, sizeof(r));
            r.kind = BOTHOST_RECORD_PROBE;
            r.host = (uint8_t)i;
            r.bots = botHostRecordByte(in.bots);
            r.slots = botHostRecordByte(in.slots);
            r.limit = botHostRecordByte(botHostOvr[i].limit);
            r.thinks = botHostRecordU16(in.thinks);
            r.overruns = botHostRecordU16(in.overruns);
            r.flags = (in.thinks > 0 && rep.ratePct > cfg->pct)
                          ? (uint8_t)BOTHOST_RECORD_PROBE_BAD : (uint8_t)0;
            r.badProbes = botHostRecordByte(rep.badProbes);
            r.probes = botHostRecordByte(rep.probes);
            botHostRecPut(&r);
            if (rep.sampled) {
                memset(&r, 0, sizeof(r));
                r.kind = BOTHOST_RECORD_READING;
                r.host = (uint8_t)i;
                r.freePct = rep.sample.freePct;
                r.botCpu = rep.sample.botCpu;
                r.bots = rep.sample.bots;
                r.flags = (uint8_t)(
                    (rep.sample.ran ? BOTHOST_RECORD_READING_RAN : 0u) |
                    (rep.sample.bad ? BOTHOST_RECORD_READING_BAD : 0u));
                botHostRecPut(&r);
            }
        }

        if (act == BOT_HOST_OVR_NONE) continue;
        botHostOvrNumbers(&rep, nums, sizeof(nums));
        botHostOvrLimitText(rep.limitBefore, in.slots, before, sizeof(before));
        botHostOvrLimitText(rep.newLimit, in.slots, after, sizeof(after));

        if (act == BOT_HOST_OVR_WAIT) {
            if (botHostOvrWaitLogged[i]) continue;
            botHostOvrWaitLogged[i] = true;
            fprintf(stderr, "[bothost] overruns: %s\"%s\" had %d of %d probes "
                            "over %.1f%% (%.1f%% now), but %s; it waits "
                            "(%s)\n",
                    botHostOvrKind(i), botHostOvrName(i), rep.badProbes,
                    rep.probes, cfg->pct, rep.ratePct, rep.why, nums);
            botHostRecDecision(i, BOTHOST_DECISION_TRIGGER, &rep, -1, -1,
                               rep.why);
            continue;
        }
        botHostOvrWaitLogged[i] = false;

        if (act == BOT_HOST_OVR_LOWER) {
            fprintf(stderr, "[bothost] overruns: %s\"%s\" had %d of %d probes "
                            "over %.1f%% (%.1f%% now); its limit goes from %s "
                            "to %s for this game (%s)\n",
                    botHostOvrKind(i), botHostOvrName(i), rep.badProbes,
                    rep.probes, cfg->pct, rep.ratePct, before, after, nums);
            botHostRecDecision(i, BOTHOST_DECISION_TRIGGER, &rep, -1, -1,
                               "overruns");
            botHostRecDecision(i, BOTHOST_DECISION_CUT, &rep, -1, -1,
                               "overruns");
            if (botHostNoMoveOverrunDecision(&botHostNoMove, running) ==
                BOTHOST_DECISION_WOULD_MOVE) {
                /* -bothost-nomove: the bot stays; the decision is kept. */
                fprintf(stderr, "[bothost] overruns: bot %d \"%s\" would "
                                "move (moves off) from %s\"%s\" to %s%s%s\n",
                        seat, botSeatName(sim, seat), botHostOvrKind(i),
                        botHostOvrName(i),
                        dest == 0 ? "the server" : "Bot Host \"",
                        dest == 0 ? "" : udpServer.botHosts[dest - 1].name,
                        dest == 0 ? "" : "\"");
                botHostRecDecision(i, BOTHOST_DECISION_WOULD_MOVE, &rep, seat,
                                   dest, "would move (moves off)");
                continue;
            }
            fprintf(stderr, "[bothost] overruns: bot %d \"%s\" moves from "
                            "%s\"%s\" to %s%s%s\n",
                    seat, botSeatName(sim, seat), botHostOvrKind(i),
                    botHostOvrName(i),
                    dest == 0 ? "the server" : "Bot Host \"",
                    dest == 0 ? "" : udpServer.botHosts[dest - 1].name,
                    dest == 0 ? "" : "\"");
            botHostRecDecision(i, BOTHOST_DECISION_MOVE, &rep, seat, dest,
                               "overruns");
            if (i == 0) {
                (void)botSeatOffer(sim, seat, dest - 1,
                                   "overruns on the server");
            } else {
                botSeatFallBack(sim, seat, "overruns on its Bot Host", true);
                udpServer.botSeats[seat].noReturn = false;
                if (dest > 0 && botSeatLocalOfferable(sim, seat)) {
                    (void)botSeatOffer(sim, seat, dest - 1,
                                       "overruns on its Bot Host");
                }
            }
            moved = true;
        } else if (act == BOT_HOST_OVR_RAISE) {
            fprintf(stderr, "[bothost] overruns: %s\"%s\" limit goes from %s "
                            "to %s (%d of %d probes bad; %s)\n",
                    botHostOvrKind(i), botHostOvrName(i), before, after,
                    rep.badProbes, rep.probes, nums);
            botHostRecDecision(i, BOTHOST_DECISION_RAISE, &rep, -1, -1,
                               rep.why);
        } else if (act == BOT_HOST_OVR_REFUSE) {
            fprintf(stderr, "[bothost] overruns: %s\"%s\" limit stays %s: %s "
                            "(%d of %d probes bad; %s)\n",
                    botHostOvrKind(i), botHostOvrName(i), before, rep.why,
                    rep.badProbes, rep.probes, nums);
            botHostRecDecision(i, BOTHOST_DECISION_RAISE_REFUSED, &rep, -1,
                               -1, rep.why);
        }
    }
    return moved;
}

void transportUdpServerSetBotHostOverrun(double pct, int badShare,
                                         int windowSecs, int stepSecs,
                                         int freeSampleSecs,
                                         int freeHistorySecs) {
    BotHostOverrunCfg *cfg = &botHostOvrCfg;
    (void)botHostOvrConfig();
    if (pct >= 0.0) cfg->pct = pct > 100.0 ? 100.0 : pct;
    if (badShare >= 0) {
        cfg->badShare = badShare < 1 ? 1 : badShare > 100 ? 100 : badShare;
    }
    if (windowSecs >= 0) {
        cfg->windowSecs = windowSecs < 1 ? 1
                        : windowSecs > BOT_HOST_OVR_MAX_PROBES
                              ? BOT_HOST_OVR_MAX_PROBES : windowSecs;
    }
    if (stepSecs >= 0) {
        cfg->stepSecs = stepSecs < 1 ? 1 : stepSecs > 3600 ? 3600 : stepSecs;
    }
    if (freeSampleSecs >= 0) {
        cfg->freeSampleSecs = freeSampleSecs < 1 ? 1
                            : freeSampleSecs > 60 ? 60 : freeSampleSecs;
    }
    if (freeHistorySecs >= 0) cfg->freeHistorySecs = freeHistorySecs;
    if (cfg->freeHistorySecs < cfg->freeSampleSecs) {
        cfg->freeHistorySecs = cfg->freeSampleSecs;
    }
    if (cfg->freeHistorySecs >
        cfg->freeSampleSecs * BOT_HOST_FREE_MAX_READINGS) {
        cfg->freeHistorySecs = cfg->freeSampleSecs * BOT_HOST_FREE_MAX_READINGS;
    }
    if (!(cfg->pct > 0.0)) {
        fprintf(stderr, "[bothost] overruns: the check is off\n");
        return;
    }
    fprintf(stderr, "[bothost] overruns: a probe each second is bad over "
                    "%.1f%%; %d%% bad in %d s moves one bot, at most one "
                    "step each %d s; free CPU each %d s, %d s kept\n",
            cfg->pct, cfg->badShare, cfg->windowSecs, cfg->stepSecs,
            cfg->freeSampleSecs, cfg->freeHistorySecs);
}

void transportUdpServerSetBotHostNoMove(bool on) {
    botHostNoMoveInit(&botHostNoMove, on);
    if (on) {
        fprintf(stderr, "Bot moves during the game are off "
                        "(-bothost-nomove)\n");
    }
}

void transportUdpServerSetBotHostRecord(bool on) {
    botHostRecordData = on;
    if (on) {
        fprintf(stderr, "[bothost] the bot placement data goes into the "
                        "round log (.wbv)\n");
    }
}

/* One placement move: index 0 of the host table is the server itself, index
 * h + 1 is Bot Host h. */
static void botHostPlaceStep(ServerSim *sim, ServerState st) {
    static uint32_t lastMoveTick = 0;
    static uint32_t runStartTick = 0;
    static ServerState lastSt = serverStateLobby;
    static uint32_t lastProbeTick = 0;
    static bool probed = false;
    BotHostPlaceHost hosts[1 + MAX_BOT_HOSTS], raw[1 + MAX_BOT_HOSTS];
    int localSeat = -1, localAny = -1, hostSeat[MAX_BOT_HOSTS];
    bool running = (st == serverStateRunning);
    bool mayMove;
    bool anyPending = false, anyHost = false;
    int s, h, from = -1, to = -1;

    if (running && lastSt != serverStateRunning) {
        runStartTick = udpServer.tickCount;
    }
    lastSt = st;

    memset(hosts, 0, sizeof(hosts));
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        hostSeat[h] = -1;
        if (!hc->connected) continue;
        hosts[h + 1].slots = hc->slots;
        hosts[h + 1].bots = botHostUsedSlots(h);
        hosts[h + 1].open = botHostTakesBots(sim, h);
        botHostUpdateLag(h);
        hosts[h + 1].lagMs = hc->lagKnown ? hc->lagMs : -1.0;
        if (hosts[h + 1].open) anyHost = true;
    }

    hosts[0].slots = botHostServerSlots(sim);
    hosts[0].bots = serverBotHostServerBots(sim);
    hosts[0].open = true;
    hosts[0].unlimited = true;
    hosts[0].lagMs = 0.0;  /* the server's own process */
    for (s = 0; s < MAX_TANKS; s++) {
        BotHostSeat *seat = &udpServer.botSeats[s];
        botHostNoMoveSeat(&botHostNoMove, s, serverSimIsBot(sim, (BYTE)s),
                          seat->state == BOT_SEAT_BOUND ||
                              seat->state == BOT_SEAT_REMOTE,
                          udpServer.tickCount);
        if (seat->state == BOT_SEAT_PENDING) anyPending = true;
        if (seat->state == BOT_SEAT_LOCAL) {
            if (botSeatLocalOfferable(sim, s) &&
                botSeatMayMove(sim, s, running)) {
                /* The overrun check looks at any bot; the spread only at
                 * one that may still be placed (-bothost-nomove). */
                if (localAny < 0) localAny = s;
                if (localSeat < 0 &&
                    botHostNoMoveMayPlace(&botHostNoMove, s, running,
                                          udpServer.tickCount,
                                          BOT_HOST_START_PLACE_TICKS)) {
                    localSeat = s;
                }
            }
        } else if (seat->state == BOT_SEAT_REMOTE && seat->host >= 0 &&
                   seat->host < MAX_BOT_HOSTS && hostSeat[seat->host] < 0 &&
                   botSeatMayMove(sim, s, running)) {
            hostSeat[seat->host] = s;
        }
    }
    mayMove = botHostNoMoveMayMove(&botHostNoMove, running);
    hosts[0].movable = (localSeat >= 0);
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        /* A host's bots stay put while any offer waits for its answer, and
         * all game long with -bothost-nomove. */
        hosts[h + 1].movable = (hostSeat[h] >= 0) && !anyPending && mayMove;
    }

    /* The overrun check: a probe each second, then the lowered limits and
     * the free CPU check on the table the spread uses. */
    memcpy(raw, hosts, sizeof(raw));
    botHostOvrApply(hosts, true);
    if (!probed || udpServer.tickCount - lastProbeTick >=
                       BOT_HOST_OVR_PROBE_TICKS) {
        probed = true;
        lastProbeTick = udpServer.tickCount;
        if (botHostOvrConfig()->pct > 0.0 &&
            botHostOvrProbeAll(sim, raw, hosts, localAny, hostSeat,
                               running)) {
            lastMoveTick = udpServer.tickCount;
            return;
        }
        memcpy(hosts, raw, sizeof(hosts));
        botHostOvrApply(hosts, false);
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            hosts[h + 1].movable = hosts[h + 1].movable && mayMove;
        }
    }
    if (!mayMove) {
        /* -bothost-nomove while the game runs: a host that offers fewer
         * slots than it has bots gives one back to the server (a
         * fall-back); no other bot leaves its place. */
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            if (hostSeat[h] < 0 || anyPending) continue;
            if (!botHostNoMoveGiveBack(&botHostNoMove, running,
                                       raw[h + 1].bots, raw[h + 1].slots)) {
                continue;
            }
            if (udpServer.tickCount - lastMoveTick < BOT_HOST_MOVE_TICKS) {
                break;
            }
            s = hostSeat[h];
            fprintf(stderr, "[bothost] bot %d \"%s\" goes back to the server: "
                            "Bot Host \"%s\" offers %d slots for %d bots\n",
                    s, botSeatName(sim, s), udpServer.botHosts[h].name,
                    raw[h + 1].slots, raw[h + 1].bots);
            botSeatFallBack(sim, s, "its host offers fewer slots", true);
            lastMoveTick = udpServer.tickCount;
            return;
        }
    }
    if (!anyHost) return;

    if (!botHostPlaceRebalanceLag(hosts, 1 + MAX_BOT_HOSTS,
                                  botHostSmallSlots, botHostSmallBias,
                                  &botHostLag, &from, &to)) {
        return;
    }
    if (running) {
        bool early = udpServer.tickCount - runStartTick <
                     BOT_HOST_START_PLACE_TICKS;
        bool spaced = udpServer.tickCount - lastMoveTick >= BOT_HOST_MOVE_TICKS;
        if (!spaced && !(early && from == 0)) return;
    }
    if (from == 0) {
        if (to <= 0) return;
        if (botSeatOffer(sim, localSeat, to - 1, "the spread")) {
            lastMoveTick = udpServer.tickCount;
        }
        return;
    }
    s = hostSeat[from - 1];
    fprintf(stderr, "[bothost] bot %d \"%s\" moves from Bot Host \"%s\" to "
                    "%s%s%s (the spread)\n",
            s, botSeatName(sim, s), udpServer.botHosts[from - 1].name,
            to == 0 ? "the server" : "Bot Host \"",
            to == 0 ? "" : udpServer.botHosts[to - 1].name,
            to == 0 ? "" : "\"");
    botSeatFallBack(sim, s, "the spread", true);
    udpServer.botSeats[s].noReturn = false;
    lastMoveTick = udpServer.tickCount;
    if (to > 0 && botSeatLocalOfferable(sim, s)) {
        (void)botSeatOffer(sim, s, to - 1, "the spread");
    }
}

void serverServiceBotHosts(ServerSim *sim) {
    int h, s;
    ServerState st;

    if (sim == NULL) return;
    st = serverSimGetState(sim);
    {
        static bool ruleShown = false;
        if (udpServer.botHostEnabled && !ruleShown) {
            ruleShown = true;
            botHostPrintRule();
        }
    }

    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected) continue;
        if (udpServer.tickCount - hc->lastReceivedTick > BOT_HOST_TIMEOUT_TICKS) {
            botHostDrop(sim, h, "timed out");
        } else if (!botHostOwnerHere(h)) {
            uint8_t kbuf[PACKET_HEADER_SIZE];
            packHeader(kbuf, PACKET_KICKED, 0);
            srvSendTo(kbuf, sizeof(kbuf), &hc->addr);
            botHostDrop(sim, h, "its player left");
        }
    }
    botHostServiceTickets(sim);

    /* A new round or the lobby: a seat that came back from a host may be
     * offered again, and a bound seat's host makes the round's brain. A map
     * rotation goes from one round to the next inside one tick, so the
     * state can read Running on both sides of it; the game start count
     * shows that round start too. */
    if ((int)st != udpServer.botHostLastState ||
        botManagerGameStarts(sim) != udpServer.botHostLastGameStarts) {
        udpServer.botHostLastGameStarts = botManagerGameStarts(sim);
        if ((st == serverStateLobby &&
             udpServer.botHostLastState != (int)serverStateLobby &&
             udpServer.botHostLastState != -1) ||
            (st == serverStateRunning &&
             udpServer.botHostLastState == (int)serverStateRunning)) {
            /* Back to the lobby, or a map rotation into the next round:
             * a game ended, and hosts may have slots free again. */
            udpServer.botHostQueryWhy = "a game ended";
        }
        if (st == serverStateLobby &&
            udpServer.botHostLastState != (int)serverStateLobby) {
            /* Back in the lobby: the game a late host sat out is over. */
            for (h = 0; h < MAX_BOT_HOSTS; h++) {
                udpServer.botHosts[h].skipGame = false;
            }
            /* The hosts send their recordings of it now, at full speed. */
            if (udpServer.botHostLastState != -1) serverBotRecOnLobby();
            /* The players are sent their tickets again. */
            for (s = 0; s < MAX_TANKS; s++) {
                udpServer.playerTicketStarts[s] = 0xFFFFFFFEu;
            }
        }
        if (st == serverStateRunning &&
            (udpServer.botHostLastState == (int)serverStateLobby ||
             udpServer.botHostLastState == (int)serverStateCountdown)) {
            /* A game starts: a host still sending goes on slowly. */
            serverBotRecOnGameStart();
        }
        if (udpServer.botHostEnabled &&
            (st == serverStateLobby || st == serverStateRunning)) {
            /* A new round or the lobby: the brain files may have changed. */
            botHostRefreshManifest(sim, st == serverStateLobby ? "lobby"
                                                               : "round start");
        }
        if (st == serverStateLobby || st == serverStateRunning) {
            for (s = 0; s < MAX_TANKS; s++) {
                udpServer.botSeats[s].noReturn = false;
            }
            /* The overrun limits are for one game only. */
            botHostOvrNewGame();
        }
        if (st == serverStateRunning) {
            for (s = 0; s < MAX_TANKS; s++) {
                BotHostSeat *seat = &udpServer.botSeats[s];
                if (seat->state == BOT_SEAT_BOUND) {
                    seat->reloadOwed = true;
                    seat->mineSeedDone = false;
                    seat->mineSeedColumn = 0;
                }
            }
        }
        udpServer.botHostLastState = (int)st;
    }

    for (s = 0; s < MAX_TANKS; s++) {
        BotHostSeat *seat = &udpServer.botSeats[s];
        if (seat->state == BOT_SEAT_LOCAL) continue;

        if (!serverSimIsBot(sim, (BYTE)s)) {
            /* The bot is gone; the bot manager's hook missed it. */
            botSlotRelease(sim, s);
            botSeatSendUnassign(s);
            botSeatReset(s, false);
            continue;
        }
        if (seat->host < 0 || !udpServer.botHosts[seat->host].connected) {
            botSeatFallBack(sim, s, "Bot Host gone", false);
            continue;
        }
        if (!botHostSynced(seat->host)) {
            /* The brain files changed and the host does not hold the new
             * ones yet: the seat runs in-process until it does, and then
             * goes back to a host. */
            botSeatFallBack(sim, s, "brain files changed", true);
            udpServer.botSeats[s].noReturn = false;
            continue;
        }
        if (seat->state == BOT_SEAT_PENDING) {
            if (udpServer.tickCount - seat->sinceTick > BOT_SEAT_PENDING_TICKS) {
                fprintf(stderr, "[bothost] bot %d: Bot Host \"%s\" did not "
                                "connect it; it stays in-process\n",
                        s, udpServer.botHosts[seat->host].name);
                botSeatSendUnassign(s);
                botSeatReset(s, true);
            }
            continue;
        }
        if (!udpServer.clients[s].connected || !udpServer.clients[s].botSlot) {
            botSeatFallBack(sim, s, "bot connection gone", true);
            continue;
        }
        if (st != serverStateRunning) continue;
        if (!udpServer.mapDownload[s].downloadComplete) continue;

        if (seat->reloadOwed) {
            seat->reloadOwed = false;
            botSeatSendAssign(sim, s, true);
            seat->lastPollTick = udpServer.tickCount;
        } else if (seat->state == BOT_SEAT_REMOTE &&
                   udpServer.tickCount - seat->lastPollTick >= BOT_SEAT_POLL_TICKS) {
            char brain[BOT_HOST_BRAIN_NAME_LEN];
            char initArg[BOT_HOST_INIT_ARG_LEN];
            uint8_t ai, team;
            seat->lastPollTick = udpServer.tickCount;
            botSeatCurrent(sim, s, brain, sizeof(brain), &ai, &team,
                           initArg, sizeof(initArg));
            if (strcmp(brain, seat->brain) != 0 || ai != seat->ai) {
                /* An in-process bot swaps its brain at once
                 * (botManagerSetBrainIdx), so the host does too. */
                fprintf(stderr, "[bothost] bot %d: brain or AI level changed, "
                                "Bot Host \"%s\" makes a new brain\n",
                        s, udpServer.botHosts[seat->host].name);
                botSeatSendAssign(sim, s, true);
            } else if (strcmp(initArg, seat->initArg) != 0 ||
                       team != seat->team) {
                /* An in-process bot reads its init arg when its brain is
                 * next built, at the round start, so the host gets it now
                 * for that brain and the running one is left alone. */
                fprintf(stderr, "[bothost] bot %d: init arg or team changed, "
                                "Bot Host \"%s\" keeps it for the next "
                                "brain\n",
                        s, udpServer.botHosts[seat->host].name);
                botSeatSendAssign(sim, s, false);
            }
        }
        botSeatSeedMines(sim, s);
    }

    /* Placement: the proportional spread over the hosts and the server
     * itself, one move at a time (botHostPlaceStep). */
    if (udpServer.botHostEnabled &&
        (st == serverStateLobby || st == serverStateCountdown ||
         st == serverStateRunning)) {
        botHostPlaceStep(sim, st);
    }
    /* Brain sync: look at the files again now and then in the lobby, give
     * up waiting on a host past the time limit, and feed each host its
     * next transfer. */
    if (udpServer.botHostEnabled && st == serverStateLobby &&
        udpServer.tickCount % BOT_HOST_MANIFEST_POLL_TICKS == 0) {
        botHostRefreshManifest(sim, "brain files changed");
    }
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected) continue;
        if (!botHostSynced(h) && !hc->syncLate &&
            udpServer.tickCount - hc->syncSentTick > BOT_HOST_SYNC_LIMIT_TICKS) {
            hc->syncLate = true;
            hc->skipGame = true;
            hc->skipUntilStarts = botManagerGameStarts(sim) +
                ((st == serverStateLobby || st == serverStateCountdown) ? 1u : 0u);
            fprintf(stderr, "[bothost] Bot Host \"%s\" is still getting the "
                            "brain files after %d s; the lobby does not wait "
                            "for it and it takes no bots this game\n",
                    hc->name, BOT_HOST_SYNC_LIMIT_TICKS / 50);
        }
        botHostServiceFiles(sim, h);
    }
    serverBotRecService(sim, st == serverStateLobby);
    botHostBroadcastRows();

    /* Finding hosts: ask the listed ones for free slots when the first
     * player comes into an empty lobby (and at the start and after each
     * game, set above), and let a connected host with no seats go once no
     * bots have been wanted for a while. */
    if (udpServer.botHostEnabled) {
        int humans = 0;
        bool wanted = botHostWanted(sim, &humans);
        /* A server with no lobby (-nolobby, -maprotate) gets the same
         * query when the first player joins its empty game. */
        if (humans > 0 && udpServer.botHostLastHumans == 0) {
            if (st == serverStateLobby) {
                udpServer.botHostQueryWhy = "a player joined the empty lobby";
            } else if (st == serverStateCountdown || st == serverStateRunning) {
                udpServer.botHostQueryWhy = "a player joined the empty game";
            }
        }
        udpServer.botHostLastHumans = humans;
        if (wanted) {
            udpServer.botHostUnwantedSince = 0;
        } else if (udpServer.botHostUnwantedSince == 0) {
            udpServer.botHostUnwantedSince = udpServer.tickCount | 1u;
        } else if (udpServer.tickCount - udpServer.botHostUnwantedSince >
                   BOT_HOST_IDLE_RELEASE_TICKS) {
            for (h = 0; h < MAX_BOT_HOSTS; h++) {
                uint8_t kbuf[PACKET_HEADER_SIZE];
                if (!udpServer.botHosts[h].connected ||
                    udpServer.botHosts[h].playerHost ||
                    botHostUsedSlots(h) > 0 || serverBotRecBusy(h)) {
                    /* A host sending recordings stays until it is done. */
                    continue;
                }
                packHeader(kbuf, PACKET_KICKED, 0);
                srvSendTo(kbuf, sizeof(kbuf), &udpServer.botHosts[h].addr);
                botHostDrop(sim, h, "not needed now: no player and no bot");
            }
        }
        if (udpServer.botHostQueryWhy != NULL) {
            botHostSendQueries(udpServer.botHostQueryWhy);
            udpServer.botHostQueryWhy = NULL;
        }
    }

    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (udpServer.botHosts[h].connected) botHostPump(h);
    }
}

void transportUdpServerPrintBotHosts(void) {
    int h, s;
    bool any = false;
    char addr[48];

    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected) continue;
        if (!any) {
            fprintf(stdout, "\nBot Hosts:\n");
            any = true;
        }
        fprintf(stdout, "Bot Host \"%s\"%s: %d/%u slots used (Ping:%ums Addr:%s)\n",
                hc->name, hc->playerHost ? " (a player's own)" : "",
                botHostUsedSlots(h), (unsigned)hc->slots,
                (unsigned)hc->pingMs,
                botHostAddrStr(&hc->addr, addr, sizeof(addr)));
        {
            ControlEvent row;
            static const char *const states[] = {
                "waiting for its brain file report", "getting brain files",
                "holds the brain files", "late with the brain files"
            };
            botHostFillRow(h, &row);
            fprintf(stdout, "    brain files: %s (%u%%)\n",
                    states[row.u.botHostStatus.state & 3],
                    (unsigned)row.u.botHostStatus.percent);
        }
        for (s = 0; s < MAX_TANKS; s++) {
            const BotHostSeat *seat = &udpServer.botSeats[s];
            const char *what;
            if (seat->state == BOT_SEAT_LOCAL || seat->host != h) continue;
            what = seat->state == BOT_SEAT_REMOTE ? "thinking"
                 : seat->state == BOT_SEAT_BOUND  ? "connected"
                                                  : "offered";
            fprintf(stdout, "    player %d \"%s\": %s\n", s,
                    botSeatNames[s], what);
        }
    }
    if (!any && udpServer.botHostEnabled) {
        fprintf(stdout, "\nBot Hosts: none connected\n");
    }
    for (h = 0; h < udpServer.botHostAllowCount; h++) {
        struct sockaddr_in la;
        const BotHostAnnounce *a = &udpServer.botHostAnnounce[h];
        botHostEntryAddr(h, &la);
        if (h == 0) fprintf(stdout, "Bot Host list (-bothosts):\n");
        if (botHostFindByListen(&la) >= 0) {
            fprintf(stdout, "    %s: connected\n", udpServer.botHostAllowText[h]);
        } else if (udpServer.botHostAnnounceTick[h] == 0) {
            fprintf(stdout, "    %s: not heard from\n",
                    udpServer.botHostAllowText[h]);
        } else {
            fprintf(stdout, "    %s: \"%s\" announced %u of %u slots free, "
                            "%u s ago\n",
                    udpServer.botHostAllowText[h], a->name,
                    (unsigned)a->freeSlots, (unsigned)a->slots,
                    (unsigned)((udpServer.tickCount -
                                udpServer.botHostAnnounceTick[h]) / 50));
        }
    }
}
