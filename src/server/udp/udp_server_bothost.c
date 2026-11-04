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
 *  A Bot Host opens one control connection (JOIN_FLAG_BOT_HOST
 *  with the -bothostkey key). It holds no tank seat and no
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
 *  Placement rule: each tick, at most one in-process bot seat
 *  is offered to the connected Bot Host with the most free
 *  slots. A seat with no host free stays in-process. A seat
 *  that came back from a host is offered again only at the
 *  next round start or in the lobby.
 *********************************************************/

#include <stdio.h>   /* fprintf, snprintf, stderr, stdout */
#include <string.h>  /* memcpy, memset, strcmp, strlen */

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
#include "bot_manager.h"     /* botManager*, BotRemoteHooks */
#include "../../common/wb_log.h" /* WB_LOG_* */

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
                    "\"%s\" (%s)\n",
            s, botSeatName(sim, s), hostName, why);
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
        }
    }
    fprintf(stderr, "[bothost] Bot Host \"%s\" at %s gone (%s)\n", hc->name,
            botHostAddrStr(&hc->addr, addr, sizeof(addr)), why);
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] Bot Host \"%s\" gone (%s)",
                hc->name, why);
    memset(hc, 0, sizeof(*hc));
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
    int h, freeIdx = -1;

    botHostAddrStr(fromAddr, addr, sizeof(addr));
    if (extraLen < JOIN_BOT_HOST_KEY_LEN + 1 + JOIN_BOT_HOST_NAME_LEN) {
        fprintf(stderr, "[bothost] Bot Host join from %s malformed\n", addr);
        return;
    }
    memcpy(key, extra, JOIN_BOT_HOST_KEY_LEN);
    memset(want, 0, sizeof(want));
    memcpy(want, udpServer.botHostKey,
           strlen(udpServer.botHostKey) < sizeof(want)
               ? strlen(udpServer.botHostKey) : sizeof(want));
    if (!udpServer.botHostEnabled ||
        !botHostBytesEqual(key, want, JOIN_BOT_HOST_KEY_LEN)) {
        fprintf(stderr, "[bothost] Bot Host join from %s refused: %s\n", addr,
                udpServer.botHostEnabled ? "wrong key"
                                         : "this server takes no Bot Host");
        WB_LOG_WARN(WB_LOG_CAT_NET, "[bothost] Bot Host join from %s refused",
                    addr);
        serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0, NULL);
        return;
    }
    slots = extra[JOIN_BOT_HOST_KEY_LEN];
    if (slots < 1) slots = 1;
    if (slots > BOT_HOST_MAX_SLOTS) slots = BOT_HOST_MAX_SLOTS;
    memcpy(hostName, extra + JOIN_BOT_HOST_KEY_LEN + 1, sizeof(hostName));
    hostName[sizeof(hostName) - 1] = '\0';
    if (hostName[0] == '\0') {
        SDL_strlcpy(hostName, name, sizeof(hostName));
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
        hc->connected = true;
        hc->connId = serverNextConnId();
        SDL_strlcpy(hc->name, hostName, sizeof(hc->name));
        hc->slots = slots;
        hc->lastReceivedTick = udpServer.tickCount;
        hc->outSequence = 1;
        channelMuxInit(&hc->channelMux);
    }
    fprintf(stderr, "[bothost] Bot Host \"%s\" connected from %s with %u "
                    "slots\n", hostName, addr, (unsigned)slots);
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] Bot Host \"%s\" connected from %s "
                "slots=%u", hostName, addr, (unsigned)slots);
    serverSendBotHostAccept(freeIdx, sim);
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
            s, c->playerName,
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
    fprintf(stderr, "[bothost] bot %d \"%s\" now thinks on Bot Host \"%s\"\n",
            slot, botSeatName(sim, slot),
            seat->host >= 0 ? udpServer.botHosts[seat->host].name : "?");
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
    /* The host sends nothing on its channels but acks; drop anything else. */
    outLen = sizeof(out);
    while (channelReceive(&hc->channelMux, CHANNEL_CONTROL, out, &outLen)) {
        outLen = sizeof(out);
    }
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

bool transportUdpServerSetBotHostKey(ServerSim *sim, const char *key) {
    BotRemoteHooks hooks;
    int s;

    if (sim == NULL) return false;
    botHostSim = sim;
    memset(udpServer.botHostKey, 0, sizeof(udpServer.botHostKey));
    if (key == NULL || key[0] == '\0') {
        udpServer.botHostEnabled = false;
        botManagerSetRemoteHooks(sim, NULL);
        return true;
    }
    if (strlen(key) > JOIN_BOT_HOST_KEY_LEN) {
        fprintf(stderr, "[bothost] -bothostkey is longer than %d characters\n",
                JOIN_BOT_HOST_KEY_LEN);
        return false;
    }
    SDL_strlcpy(udpServer.botHostKey, key, sizeof(udpServer.botHostKey));
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
    fprintf(stderr, "[bothost] Bot Host connections enabled\n");
    return true;
}

void serverBotHostsReset(void) {
    int s;
    memset(udpServer.botHosts, 0, sizeof(udpServer.botHosts));
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

void serverServiceBotHosts(ServerSim *sim) {
    int h, s;
    ServerState st;
    bool placed = false;

    if (sim == NULL) return;
    st = serverSimGetState(sim);

    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected) continue;
        if (udpServer.tickCount - hc->lastReceivedTick > BOT_HOST_TIMEOUT_TICKS) {
            botHostDrop(sim, h, "timed out");
        }
    }

    /* A new round or the lobby: a seat that came back from a host may be
     * offered again, and a bound seat's host makes the round's brain. */
    if ((int)st != udpServer.botHostLastState) {
        if (st == serverStateLobby || st == serverStateRunning) {
            for (s = 0; s < MAX_TANKS; s++) {
                udpServer.botSeats[s].noReturn = false;
            }
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
            if (strcmp(brain, seat->brain) != 0 || ai != seat->ai ||
                strcmp(initArg, seat->initArg) != 0) {
                fprintf(stderr, "[bothost] bot %d: settings changed, Bot Host "
                                "\"%s\" makes a new brain\n",
                        s, udpServer.botHosts[seat->host].name);
                botSeatSendAssign(sim, s, true);
            } else if (team != seat->team) {
                botSeatSendAssign(sim, s, false);
            }
        }
        botSeatSeedMines(sim, s);
    }

    /* Placement: one seat per tick, to the host with the most free slots. */
    if (udpServer.botHostEnabled &&
        (st == serverStateLobby || st == serverStateCountdown ||
         st == serverStateRunning)) {
        for (s = 0; s < MAX_TANKS && !placed; s++) {
            BotHostSeat *seat = &udpServer.botSeats[s];
            int best = -1, bestFree = 0;
            if (seat->state != BOT_SEAT_LOCAL || seat->noReturn) continue;
            if (!botManagerIsBot(sim, (BYTE)s)) continue;
            if (botManagerIsRemote(sim, (BYTE)s)) continue;
            if (udpServer.clients[s].connected) continue;
            if (botManagerGetBrainPath(sim, (BYTE)s) == NULL ||
                botManagerGetBrainPath(sim, (BYTE)s)[0] == '\0') continue;
            for (h = 0; h < MAX_BOT_HOSTS; h++) {
                int freeSlots;
                if (!udpServer.botHosts[h].connected) continue;
                freeSlots = (int)udpServer.botHosts[h].slots - botHostUsedSlots(h);
                if (freeSlots > bestFree) {
                    bestFree = freeSlots;
                    best = h;
                }
            }
            if (best < 0) break;
            if (!serverFillRandomBytes(seat->token, sizeof(seat->token))) {
                WB_LOG_ERROR(WB_LOG_CAT_NET, "[bothost] no random bytes for a token");
                break;
            }
            seat->state = BOT_SEAT_PENDING;
            seat->host = (int8_t)best;
            seat->sinceTick = udpServer.tickCount;
            seat->reloadOwed = false;
            botSeatSendAssign(sim, s, false);
            fprintf(stderr, "[bothost] bot %d \"%s\" offered to Bot Host \"%s\"\n",
                    s, botSeatName(sim, s),
                    udpServer.botHosts[best].name);
            placed = true;
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
        fprintf(stdout, "Bot Host \"%s\": %d/%u slots used (Ping:%ums Addr:%s)\n",
                hc->name, botHostUsedSlots(h), (unsigned)hc->slots,
                (unsigned)hc->pingMs,
                botHostAddrStr(&hc->addr, addr, sizeof(addr)));
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
}
