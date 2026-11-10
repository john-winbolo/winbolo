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
#include "machine_id.h"     /* machineIdGet, the same-computer salt */
#include "bot_host_machine.h" /* the same-computer groups, the slot override */
#include "bot_host_guard.h" /* what a Bot Host may run: official brains, tokens */
#include "bot_host_lockstep.h" /* the wait for the server's own child */
#ifndef _WIN32
#include <ifaddrs.h>        /* getifaddrs: the server's own addresses */
#endif

/* Ticks without a datagram from a Bot Host before it is dropped. The host
 * pings every 500 ms. */
#define BOT_HOST_TIMEOUT_TICKS   250
/* Ticks a seat waits for its bot connection after the assign. */
/* The pending timeout: botHostPendingExpired (bot_host_lockstep.h). */
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
 * when a place over the target rate with no better host was last logged
 * (at most once each step time). */
static const char *botHostOvrRefuseWhy[1 + MAX_BOT_HOSTS];
static bool        botHostOvrNoBetterHave[1 + MAX_BOT_HOSTS];
static uint32_t    botHostOvrNoBetterTick[1 + MAX_BOT_HOSTS];
/* Each seat's voluntary moves (m) in this game and its last one. */
static BotHostOvrMoves botHostOvrMv;
/* The round log records (-record-bothost-data): whether each place's HOST
 * record is in the log, and whether the log recorded at the last probe. */
static bool botHostRecordData;
static bool botHostRecHostSent[1 + MAX_BOT_HOSTS];
static bool botHostRecWasOn;
/* Tests only: WINBOLO_BOTHOST_TEST_OTHER_COMPUTER=1 makes every host count
 * as another computer than the server's, so one desktop can play a server
 * with a remote player host. Read once in serverBotHostsReset. */
static bool botHostTestOtherComputer;
/* With no list, the last announce the server answered (at most one in
 * each half second), and whether the placement rule was printed. File
 * statics, so serverBotHostsReset clears them. */
static uint32_t botHostOpenAnnounceTick;
static bool     botHostOpenAnnounceAny;
static bool     botHostRuleShown;

/* The state of the server's own slots and of the placement steps. They
 * are file statics, not function statics, so serverBotHostsReset clears
 * them for the next server in the same process. */
static struct {
    int         known;       /* the slots last known, -1 for none */
    const char *knownFrom;   /* where they came from, for the log */
    uint32_t    lastTick;    /* the tick of the last capacity look */
    bool        polled;
    int         logged;      /* the slots last logged, -1 for none */
} botHostSrvSlots = { -1, "", 0, false, -1 };
static struct {
    uint32_t    lastMoveTick;
    uint32_t    runStartTick;
    ServerState lastSt;
    uint32_t    lastProbeTick;
    bool        probed;
} botHostPlace = { 0, 0, serverStateLobby, 0, false };

static void botHostPlaceStateReset(void) {
    botHostSrvSlots.known = -1;
    botHostSrvSlots.knownFrom = "";
    botHostSrvSlots.lastTick = 0;
    botHostSrvSlots.polled = false;
    botHostSrvSlots.logged = -1;
    memset(&botHostPlace, 0, sizeof(botHostPlace));
    botHostPlace.lastSt = serverStateLobby;
}

/* -bothost-nomove: no bot moves while a game runs. */
static BotHostNoMove botHostNoMove;
/* The place each seat last left by a placement decision, so a bot does
 * not go back there within the stay time (BotHostTrail). */
static BotHostTrail botHostTrail;
static bool botHostTrailReady;
/* -bothost-spread-even: the old proportional spread, which also moves bots
 * between good hosts to even them out. Off: bots stay where they run
 * well (botHostPlaceRebalanceKeep). */
static bool botHostSpreadEven;
/* -bothost-enforce-server-limit (on by default): the server runs no more
 * in-process bots than its own slots. A bot with no slot there and none on
 * a good Bot Host is held (botManagerHoldNoRoom) until a place has one.
 * botHostHeldSince: the tick each held bot was held, so the one held
 * longest runs again first. botHostHeldTold: the players were told the
 * bot is paused, so they are told when it runs again. */
static bool botHostEnforceServerLimit = true;
/* -bothost-code official|custom. Official (the default): the server sends
 * no brain file to any Bot Host, and places a bot only on a host whose own
 * install holds the bot's brain, file for file. Custom: hosts download the
 * server's brain files, as before (a player's built-in host built with
 * WINBOLO_BOTHOST_OFFICIAL_ONLY still takes none). */
static bool botHostCodeCustom;
/* The most files a host may ask for again in one list, past one request
 * for each file (the host's own limit is 3: BOT_HOST_SYNC_MAX_RETRIES in
 * bothost_main.c). */
#define BOT_HOST_FILE_REQ_EXTRA 4
/* The tick each remote seat was last checked against its host's rules. */
static uint32_t botSeatFitTick[MAX_TANKS];
static uint32_t botHostHeldSince[MAX_TANKS];
static bool botHostHeldTold[MAX_TANKS];
/* The tick each place last gave a bot for being bad (the spread). */
static uint32_t botHostBadGaveTick[1 + MAX_BOT_HOSTS];
static bool botHostBadGave[1 + MAX_BOT_HOSTS];

/* Bot Hosts on one computer (bot_host_machine.h). The salt round now
 * (botHostSaltSeq counts the rounds) and the server's own answer to it;
 * each host's group; the server's own addresses, read again every
 * BOT_HOST_OWN_IP_TICKS. */
#define BOT_HOST_MACHINE_WAIT_TICKS 250   /* 5 s for the answers */
#define BOT_HOST_OWN_IP_TICKS       500
#define BOT_HOST_OWN_IP_MAX         16
static uint8_t           botHostSalt[MACHINE_SALT_LEN];
static bool              botHostSaltHave;
static uint32_t          botHostSaltSeq;
static uint32_t          botHostSaltTick;
static uint8_t           botHostServerHash[MACHINE_HASH_LEN];
static bool              botHostServerHashHave;
static uint32_t          botHostJoinSeqNext = 1;
static BotHostMachineOut botHostMachine[MAX_BOT_HOSTS];
/* The host's group is decided. Until it is, the host gets no bot: a
 * player's host on the server's computer must never take one. */
static bool              botHostMachineKnown[MAX_BOT_HOSTS];
static char              botHostMachineLogged[1024];
static uint32_t          botHostOwnIp[BOT_HOST_OWN_IP_MAX];
static int               botHostOwnIpCount;
static bool              botHostOwnIpHave;
static uint32_t          botHostOwnIpTick;
/* -bothost-slots. */
static BotHostSlotFlags  botHostSlotFlags;
/* The CTRL_BOT_HOST_ROW_SLOTS values last sent, one per Bot Host row. */
static bool              botHostRowSlotsSentHave[MAX_BOT_HOSTS];
static ControlEvent      botHostRowSlotsSent[MAX_BOT_HOSTS];
/* The game bot limit last logged. */
static int               botHostLimitLogged = -100;

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
    botHostOvrNoBetterHave[i] = false;
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
        botHostOvrNoBetterHave[i] = false;
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

/* Drop the file being sent, the files asked for, and the packed stream
 * and its requests. */
static void botHostFileReset(BotHostConn *hc) {
    SDL_free(hc->fileData);
    hc->fileData = NULL;
    hc->fileSending = false;
    hc->fileOffset = 0;
    hc->fileSize = 0;
    hc->fileReqCount = 0;
    hc->fileReqTaken = 0;
    brainPackWriterFree(hc->packW);
    hc->packW = NULL;
    SDL_free(hc->packData);
    hc->packData = NULL;
    hc->packFileOpen = false;
    hc->packQueue = 0;
    hc->packReqs = 0;
    memset(hc->packReqSeen, 0, sizeof(hc->packReqSeen));
    hc->packFilesAll = hc->packFilesSent = 0;
    hc->packFromInstall = hc->packFromCache = 0;
    hc->packRaw = hc->packComp = 0;
    hc->packLogged = false;
    memset(hc->brainInPlace, 0, sizeof(hc->brainInPlace));
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

static bool botHostSendEvent(int h, const ControlEvent *evt);

/* The slots host h may be given: what it offers, lowered by its row's
 * override, and 0 when another host on its computer (or the server, for a
 * player's host) has them. */
static int botHostEffSlots(int h) {
    const BotHostConn *hc = &udpServer.botHosts[h];
    if (!hc->connected) return 0;
    return botHostMachineSlots(botHostMachineKnown[h], &botHostMachine[h],
                               botHostSlotsEffective((int)hc->slots,
                                                     hc->slotOvr.override));
}

/* The server's own IPv4 addresses (network order), for the address check
 * of the same-computer groups. */
static void botHostOwnIpsRefresh(void) {
    if (botHostOwnIpHave &&
        udpServer.tickCount - botHostOwnIpTick < BOT_HOST_OWN_IP_TICKS) {
        return;
    }
    botHostOwnIpHave = true;
    botHostOwnIpTick = udpServer.tickCount;
    botHostOwnIpCount = 0;
#ifdef _WIN32
    {
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock != INVALID_SOCKET) {
            INTERFACE_INFO list[BOT_HOST_OWN_IP_MAX];
            DWORD got = 0;
            if (WSAIoctl(sock, SIO_GET_INTERFACE_LIST, NULL, 0, list,
                         sizeof(list), &got, NULL, NULL) != SOCKET_ERROR) {
                int n = (int)(got / sizeof(INTERFACE_INFO)), i;
                for (i = 0; i < n && botHostOwnIpCount < BOT_HOST_OWN_IP_MAX;
                     i++) {
                    const struct sockaddr_in *a =
                        (const struct sockaddr_in *)&list[i].iiAddress;
                    if (a->sin_family != AF_INET) continue;
                    botHostOwnIp[botHostOwnIpCount++] = a->sin_addr.s_addr;
                }
            }
            closesocket(sock);
        }
    }
#else
    {
        struct ifaddrs *ifap = NULL, *ifa;
        if (getifaddrs(&ifap) == 0) {
            for (ifa = ifap; ifa != NULL &&
                             botHostOwnIpCount < BOT_HOST_OWN_IP_MAX;
                 ifa = ifa->ifa_next) {
                if (ifa->ifa_addr == NULL ||
                    ifa->ifa_addr->sa_family != AF_INET) {
                    continue;
                }
                botHostOwnIp[botHostOwnIpCount++] =
                    ((const struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
            }
            freeifaddrs(ifap);
        }
    }
#endif
}

/* A new salt round: every host is sent a new salt, and the server works
 * out its own answer. A host keeps its group until the answers are in. */
static void botHostMachineRound(const char *why) {
    ControlEvent evt;
    uint8_t id[MACHINE_ID_LEN];
    int h, sent = 0;
    if (!machineRandomBytes(botHostSalt, sizeof(botHostSalt))) {
        botHostSaltHave = false;
        return;
    }
    botHostSaltHave = true;
    botHostSaltSeq++;
    botHostSaltTick = udpServer.tickCount;
    {
        static bool dirLogged = false;
        botHostServerHashHave = machineIdGet(id);
        if (!dirLogged) {
            dirLogged = true;
            fprintf(stderr, "[bothost] same-computer check: machine id %s "
                            "(folder \"%s\")\n",
                    botHostServerHashHave ? "read" : "missing", machineIdDir());
        }
    }
    if (botHostServerHashHave) {
        machineIdHashSalt(id, botHostSalt, botHostServerHash);
    }
    memset(id, 0, sizeof(id));
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_MACHINE_SALT;
    memcpy(evt.u.botHostMachineSalt.salt, botHostSalt, sizeof(botHostSalt));
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (!udpServer.botHosts[h].connected) continue;
        (void)botHostSendEvent(h, &evt);
        sent++;
    }
    if (sent > 0) {
        WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] same-computer check: new salt "
                    "for %d hosts (%s)", sent, why);
    }
}

static void botHostMachineName(int h, char *out, size_t sz) {
    const BotHostConn *hc = &udpServer.botHosts[h];
    if (hc->playerHost) {
        SDL_snprintf(out, sz, "player %u's Bot Host \"%s\"",
                     (unsigned)hc->ownerSlot, hc->name);
    } else {
        SDL_snprintf(out, sz, "Bot Host \"%s\"", hc->name);
    }
}

static const char *botHostMachineHow(int h) {
    return botHostMachine[h].how == BOT_HOST_MACHINE_HOW_ADDRESS
               ? "by its address" : "by its machine token";
}

/* One console line per group, when the groups change. */
static void botHostMachineLog(void) {
    char text[sizeof(botHostMachineLogged)];
    char nm[96], part[256];
    int h, k;
    text[0] = '\0';
    {
        bool any = false;
        char line[700] = "[bothost] same computer as the server:";
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            if (!udpServer.botHosts[h].connected ||
                botHostMachine[h].kind != BOT_HOST_MACHINE_SERVER) {
                continue;
            }
            botHostMachineName(h, nm, sizeof(nm));
            SDL_snprintf(part, sizeof(part), "%s %s (%s, %s)", any ? "," : "",
                         nm, botHostMachineHow(h),
                         botHostMachine[h].zero ? "0 bots"
                                                : "a listed host keeps its slots");
            SDL_strlcat(line, part, sizeof(line));
            any = true;
        }
        if (any) {
            SDL_strlcat(text, line, sizeof(text));
            SDL_strlcat(text, "\n", sizeof(text));
        }
    }
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        bool any = false;
        char line[700];
        if (!udpServer.botHosts[h].connected ||
            botHostMachine[h].kind != BOT_HOST_MACHINE_ALONE ||
            botHostMachine[h].how != BOT_HOST_MACHINE_HOW_TOKEN) {
            continue;
        }
        botHostMachineName(h, nm, sizeof(nm));
        SDL_snprintf(line, sizeof(line), "[bothost] same computer: %s (by its "
                     "machine token, keeps its slots)", nm);
        for (k = 0; k < MAX_BOT_HOSTS; k++) {
            if (!udpServer.botHosts[k].connected ||
                botHostMachine[k].kind != BOT_HOST_MACHINE_HOST ||
                botHostMachine[k].other != h) {
                continue;
            }
            botHostMachineName(k, nm, sizeof(nm));
            SDL_snprintf(part, sizeof(part), ", %s (%s, 0 bots)", nm,
                         botHostMachineHow(k));
            SDL_strlcat(line, part, sizeof(line));
            any = true;
        }
        if (any) {
            SDL_strlcat(text, line, sizeof(text));
            SDL_strlcat(text, "\n", sizeof(text));
        }
    }
    if (strcmp(text, botHostMachineLogged) == 0) return;
    SDL_strlcpy(botHostMachineLogged, text, sizeof(botHostMachineLogged));
    if (text[0] == '\0') {
        fprintf(stderr, "[bothost] same computer: no Bot Hosts share a "
                        "computer now\n");
    } else {
        fputs(text, stderr);
    }
}

/* The groups of hosts on one computer, from the address check and the
 * answers to the salt round. While answers are still to come (at most
 * BOT_HOST_MACHINE_WAIT_TICKS) each known host keeps its group, and a new
 * host is decided at once only when it is on the server's computer; a new
 * host not decided gets no bot. After the wait a host with no answer
 * counts as another computer. */
static void botHostMachineUpdate(void) {
    BotHostMachineIn in[MAX_BOT_HOSTS];
    bool all = true, otherComputer, waiting;
    int h;
    botHostOwnIpsRefresh();
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        const BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected || hc->localChild) continue;
        if (!hc->machineHave || hc->machineSeq != botHostSaltSeq) all = false;
    }
    waiting = !all && botHostSaltHave &&
              udpServer.tickCount - botHostSaltTick <
                  BOT_HOST_MACHINE_WAIT_TICKS;
    memset(in, 0, sizeof(in));
    otherComputer = botHostTestOtherComputer;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        const BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected) continue;
        if (hc->localChild) continue;   /* set after the step */
        in[h].present = true;
        in[h].playerHost = hc->playerHost;
        in[h].addrServer = !otherComputer && botHostAddrIsServerMachine(
            hc->addr.sin_addr.s_addr, botHostOwnIp, botHostOwnIpCount);
        in[h].hashHave = hc->machineHave && hc->machineSeq == botHostSaltSeq;
        memcpy(in[h].hash, hc->machineHash, sizeof(in[h].hash));
        in[h].joinSeq = hc->joinSeq;
        in[h].offered = botHostSlotsEffective((int)hc->slots,
                                              hc->slotOvr.override);
    }
    botHostMachineStep(in, MAX_BOT_HOSTS,
                       botHostServerHashHave && !otherComputer
                           ? botHostServerHash : NULL,
                       waiting, botHostMachine, botHostMachineKnown);
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        /* The server's own child is the server's own process: never in a
         * group, never 0 slots. */
        if (!udpServer.botHosts[h].connected ||
            !udpServer.botHosts[h].localChild) {
            continue;
        }
        botHostMachine[h].kind = BOT_HOST_MACHINE_SERVER;
        botHostMachine[h].other = -1;
        botHostMachine[h].how = BOT_HOST_MACHINE_HOW_ADDRESS;
        botHostMachine[h].zero = false;
        botHostMachineKnown[h] = true;
    }
    if (!waiting) botHostMachineLog();
}

/* A player's host found on the server's computer is told so, once a
 * connection (CTRL_BOT_HOST_SAME_SERVER): it stops for this join, and the
 * server sends it no more brain files. */
static void botHostTellSameServer(void) {
    int h;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        ControlEvent evt;
        if (!hc->connected || !hc->playerHost) continue;
        if (!botHostMachineTellSameServer(botHostMachineKnown[h],
                                          &botHostMachine[h],
                                          hc->sameServerSent)) {
            continue;
        }
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_BOT_HOST_SAME_SERVER;
        evt.u.botHostSameServer.kind = BOT_HOST_MACHINE_SERVER;
        if (!botHostSendEvent(h, &evt)) continue;   /* again next tick */
        hc->sameServerSent = true;
        botHostFileReset(hc);
        hc->manifestOwed = false;
        fprintf(stderr, "[bothost] player %u's Bot Host \"%s\" is on the "
                        "server's computer (%s): it was told to stop for "
                        "this join\n",
                (unsigned)hc->ownerSlot, hc->name,
                botHostMachine[h].how == BOT_HOST_MACHINE_HOW_ADDRESS
                    ? "by its address" : "by its machine token");
    }
}

/* The game bot limit: the server's bot slots plus each host that takes
 * bots; a host on a computer another counts, an opted-out host and one
 * still getting ready count 0. */
static void botHostUpdateGameBotLimit(ServerSim *sim) {
    int counted[MAX_BOT_HOSTS], h, limit, serverCap;
    bool any = false;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        counted[h] = 0;
        if (!udpServer.botHosts[h].connected) continue;
        any = true;
        /* The server's own child runs on the server's own slots. */
        if (udpServer.botHosts[h].localChild) continue;
        if (botHostTakesBots(sim, h)) counted[h] = botHostEffSlots(h);
    }
    serverCap = serverSimGetHostBotCapacity(sim);
    limit = botHostGameBotLimit(serverCap, counted, MAX_BOT_HOSTS);
    serverSimSetGameBotLimit(sim, limit, any);
    if (!any) limit = serverCap;
    if (limit != botHostLimitLogged) {
        char hosts[256] = "";
        botHostLimitLogged = limit;
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            char part[64];
            if (!udpServer.botHosts[h].connected) continue;
            SDL_snprintf(part, sizeof(part), "%s\"%s\" %d", hosts[0] ? ", " : "",
                         udpServer.botHosts[h].name, counted[h]);
            SDL_strlcat(hosts, part, sizeof(hosts));
        }
        if (limit == SERVER_HOST_BOT_CAPACITY_NONE) {
            fprintf(stderr, "[bothost] game bot limit: none (the server has no "
                            "bot limit)\n");
        } else if (limit == SERVER_HOST_BOT_CAPACITY_MEASURING) {
            fprintf(stderr, "[bothost] game bot limit: held while the CPU test "
                            "runs\n");
        } else {
            fprintf(stderr, "[bothost] game bot limit: %d (the server %d%s%s)\n",
                    limit, serverCap < 0 ? 0 : serverCap,
                    hosts[0] ? ", hosts: " : "", hosts);
        }
    }
}

static void botHostFillRowSlots(int h, ControlEvent *evt) {
    const BotHostConn *hc = &udpServer.botHosts[h];
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_BOT_HOST_ROW_SLOTS;
    evt->u.botHostRowSlots.idx = (uint8_t)h;
    evt->u.botHostRowSlots.machineKind = botHostMachine[h].kind;
    evt->u.botHostRowSlots.machineOther =
        botHostMachine[h].other >= 0 ? (uint8_t)botHostMachine[h].other
                                     : (uint8_t)BOT_HOST_MACHINE_OTHER_NONE;
    evt->u.botHostRowSlots.zero = botHostMachine[h].zero ? 1u : 0u;
    evt->u.botHostRowSlots.slotOverride =
        hc->slotOvr.override < 0 ? (uint8_t)BOT_HOST_SLOT_OVERRIDE_NONE
                                 : (uint8_t)hc->slotOvr.override;
    evt->u.botHostRowSlots.slotLocked = hc->slotOvr.locked ? 1u : 0u;
}

bool transportUdpServerAddBotHostSlots(const char *arg, char *err,
                                       size_t errSz) {
    return botHostSlotFlagsAdd(&botHostSlotFlags, arg, err, errSz);
}

int serverBotHostSetSlotOverride(ServerSim *sim, int row, int value,
                                 int player) {
    BotHostConn *hc;
    (void)sim;
    if (row < 0 || row >= MAX_BOT_HOSTS) return BOT_HOST_SLOT_SET_INVALID;
    hc = &udpServer.botHosts[row];
    if (!hc->connected) return BOT_HOST_SLOT_SET_INVALID;
    if (hc->slotOvr.locked) {
        fprintf(stderr, "[bothost] Bot Host \"%s\": slot change by player %d "
                        "refused (set by -bothost-slots)\n",
                hc->name, player);
        return BOT_HOST_SLOT_SET_LOCKED;
    }
    if (!botHostSlotOverrideSet(&hc->slotOvr, (int)hc->slots, value)) {
        fprintf(stderr, "[bothost] Bot Host \"%s\": slot change to %d by "
                        "player %d refused (it offers %u)\n",
                hc->name, value, player, (unsigned)hc->slots);
        return BOT_HOST_SLOT_SET_INVALID;
    }
    if (hc->slotOvr.override < 0) {
        fprintf(stderr, "[bothost] Bot Host \"%s\": player %d takes off its "
                        "slot limit (it offers %u)\n",
                hc->name, player, (unsigned)hc->slots);
    } else {
        fprintf(stderr, "[bothost] Bot Host \"%s\": player %d limits it to %d "
                        "slots (it offers %u)\n",
                hc->name, player, hc->slotOvr.override,
                (unsigned)hc->slots);
    }
    return BOT_HOST_SLOT_SET_OK;
}

/* Make the brain manifest again from the brain folders. When it differs
 * from the one the hosts have, it gets the next generation and every
 * connected host is sent it. */
static void botHostRefreshManifest(ServerSim *sim, const char *why) {
    BrainSyncManifest next;
    uint8_t *blob, *blob2;
    size_t blobLen = 0, blob2Len = 0;
    uint32_t files = 0, bytes = 0;
    char hex[17];
    int b, h;

    brainSyncBuildManifest(&next, botManagerDefaultDebugMode(sim),
                           &udpServer.brainManifest);
    next.noCode = !botHostCodeCustom;
    if (udpServer.brainManifestGen != 0 &&
        next.version == udpServer.brainManifest.version) {
        /* Same files. Keep the new list for its change times. */
        brainSyncFree(&udpServer.brainManifest);
        udpServer.brainManifest = next;
        return;
    }
    blob = brainSyncManifestEncode(&next, &blobLen);
    blob2 = brainSyncManifestEncodeV2(&next, &blob2Len);
    if (blob == NULL || blob2 == NULL) {
        fprintf(stderr, "[bothost] the brain file list is too large to send; "
                        "Bot Hosts keep the last one\n");
        free(blob);
        free(blob2);
        brainSyncFree(&next);
        return;
    }
    brainSyncFree(&udpServer.brainManifest);
    udpServer.brainManifest = next;
    free(udpServer.brainManifestBlob);
    udpServer.brainManifestBlob = blob;
    udpServer.brainManifestBlobLen = blobLen;
    free(udpServer.brainManifestBlob2);
    udpServer.brainManifestBlob2 = blob2;
    udpServer.brainManifestBlob2Len = blob2Len;
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

/* "1.42 MB" or "812.0 KB" (1 MB = 1,000,000 bytes). */
static void botHostBytesText(uint64_t n, char *out, size_t sz) {
    if (n >= 1000000u) {
        SDL_snprintf(out, sz, "%.2f MB", (double)n / 1e6);
    } else {
        SDL_snprintf(out, sz, "%.1f KB", (double)n / 1e3);
    }
}

/* The one DS line per host and generation, once every brain's request is
 * in and every stream has gone to the sender. */
static void botHostPackLog(int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    if (hc->packLogged || hc->packW != NULL || hc->packQueue > 0) return;
    if (hc->packReqs < udpServer.brainManifest.brainCount) return;
    hc->packLogged = true;
    if (hc->packFilesSent == 0) {
        if (hc->packFromInstall == hc->packFilesAll) {
            fprintf(stderr, "[bothost] brain files for \"%s\": 0 of %u files "
                            "sent (all match its install)\n",
                    hc->name, (unsigned)hc->packFilesAll);
        } else if (hc->packFromCache == hc->packFilesAll) {
            fprintf(stderr, "[bothost] brain files for \"%s\": 0 of %u files "
                            "sent (all in its cache)\n",
                    hc->name, (unsigned)hc->packFilesAll);
        } else {
            fprintf(stderr, "[bothost] brain files for \"%s\": 0 of %u files "
                            "sent (%u match its install, %u in its cache)\n",
                    hc->name, (unsigned)hc->packFilesAll,
                    (unsigned)hc->packFromInstall,
                    (unsigned)hc->packFromCache);
        }
        return;
    }
    {
        char comp[32], raw[32];
        botHostBytesText(hc->packComp, comp, sizeof(comp));
        botHostBytesText(hc->packRaw, raw, sizeof(raw));
        fprintf(stderr, "[bothost] brain files for \"%s\": %u of %u files, "
                        "%s compressed (%s raw; %llu and %llu bytes), %u "
                        "match its install, %u in its cache, %.1f s\n",
                hc->name, (unsigned)hc->packFilesSent,
                (unsigned)hc->packFilesAll, comp, raw,
                (unsigned long long)hc->packComp,
                (unsigned long long)hc->packRaw,
                (unsigned)hc->packFromInstall, (unsigned)hc->packFromCache,
                (double)(udpServer.tickCount - hc->packStartTick) / 50.0);
    }
}

/* The packed stream: start the next one asked for, feed it files for up to
 * about 2 ms, and send a piece when one is full or the stream has ended.
 * True when it used the bulk sender or still has work (so the per-file
 * requests wait). */
#define BOT_HOST_PACK_FEED_NS   (2u * 1000u * 1000u)
#define BOT_HOST_PACK_STEP      (64u * 1024u)
static bool botHostServicePack(ServerSim *sim, int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    const BrainSyncManifest *man = &udpServer.brainManifest;
    const BrainSyncBrain *br;
    BulkStreamHeader hdr;
    uint64_t t0 = SDL_GetTicksNS();

    while (hc->packW == NULL) {
        int count;
        if (hc->packQueue == 0) {
            botHostPackLog(h);
            return false;
        }
        hc->packBrain = hc->packQBrain[0];
        memcpy(hc->packBits, hc->packQBits[0], sizeof(hc->packBits));
        hc->packQueue--;
        memmove(hc->packQBrain, hc->packQBrain + 1, (size_t)hc->packQueue);
        memmove(hc->packQBits, hc->packQBits + 1,
                sizeof(hc->packQBits[0]) * (size_t)hc->packQueue);
        if (hc->packBrain >= man->brainCount) continue;
        count = brainPackBitCount(hc->packBits,
                                  man->brains[hc->packBrain].fileCount);
        if (count == 0) continue;
        hc->packW = brainPackWriterNew(BRAIN_PACK_LEVEL, (uint8_t)hc->packBrain,
                                       (uint16_t)count);
        if (hc->packW == NULL) return false;
        hc->packNext = 0;
        hc->packSeq = 0;
        hc->packFileOpen = false;
    }
    br = &man->brains[hc->packBrain];
    while (!brainPackWriterFinished(hc->packW) &&
           brainPackWriterPending(hc->packW) < BRAIN_PACK_PIECE_MAX) {
        bool finished = false;
        if (!hc->packFileOpen) {
            int i;
            for (i = hc->packNext; i < br->fileCount; i++) {
                if (brainPackBitGet(hc->packBits, i)) break;
            }
            if (i >= br->fileCount) {
                if (!brainPackWriterFinish(hc->packW)) goto failed;
                finished = true;
            } else {
                const BrainSyncFile *f = &br->files[i];
                char path[1024];
                size_t sz = 0;
                void *data;
                SDL_snprintf(path, sizeof(path), "%s/%s", br->dir, f->path);
                data = brainSyncLoadFile(path, &sz);
                if ((data == NULL && f->size != 0) || sz != f->size ||
                    brainSyncHash(BRAIN_SYNC_HASH_SEED, data, sz) != f->hash) {
                    SDL_free(data);
                    fprintf(stderr, "[bothost] brain file \"%s\" changed since "
                                    "the list was made\n", path);
                    botHostRefreshManifest(sim, "a brain file changed");
                    return true;
                }
                if (!brainPackWriterBeginFile(hc->packW, (uint16_t)i,
                                              (uint32_t)sz)) {
                    SDL_free(data);
                    goto failed;
                }
                hc->packData = (uint8_t *)data;
                hc->packSize = (uint32_t)sz;
                hc->packOff = 0;
                hc->packNext = i + 1;
                hc->packFileOpen = true;
                hc->packFilesSent++;
                hc->packRaw += sz;
            }
        }
        if (!finished && hc->packFileOpen) {
            uint32_t n = hc->packSize - hc->packOff;
            if (n > BOT_HOST_PACK_STEP) n = BOT_HOST_PACK_STEP;
            if (n > 0 &&
                !brainPackWriterPut(hc->packW, hc->packData + hc->packOff, n)) {
                goto failed;
            }
            hc->packOff += n;
            if (hc->packOff >= hc->packSize) {
                SDL_free(hc->packData);
                hc->packData = NULL;
                hc->packFileOpen = false;
            }
        }
        if (finished) break;
        if (SDL_GetTicksNS() - t0 > BOT_HOST_PACK_FEED_NS) break;
    }
    if (bulkSenderBusy(&hc->bulk)) return true;
    /* A full piece, or the last bytes once the stream has ended. */
    if (brainPackWriterPending(hc->packW) > 0 &&
        (brainPackWriterPending(hc->packW) >= BRAIN_PACK_PIECE_MAX ||
         brainPackWriterFinished(hc->packW))) {
        static uint8_t piece[BRAIN_PACK_PIECE_MAX];
        size_t n = brainPackWriterTake(hc->packW, piece, sizeof(piece));
        memset(&hdr, 0, sizeof(hdr));
        hdr.kind = BULK_KIND_BRAIN_PACK;
        hdr.gen = udpServer.brainManifestGen;
        hdr.totalSize = (uint32_t)n;
        hdr.pathLen = (uint8_t)SDL_snprintf(hdr.path, sizeof(hdr.path), "%d/%u",
                                            hc->packBrain,
                                            (unsigned)hc->packSeq);
        if (!bulkSenderBegin(&hc->bulk, &hdr, piece, (uint32_t)n)) goto failed;
        hc->packSeq++;
    }
    if (brainPackWriterDone(hc->packW)) {
        hc->packComp += brainPackWriterOutBytes(hc->packW);
        brainPackWriterFree(hc->packW);
        hc->packW = NULL;
        botHostPackLog(h);
    }
    return true;

failed:
    fprintf(stderr, "[bothost] the packed brain files for \"%s\" could not "
                    "be made; it asks for them one at a time\n", hc->name);
    brainPackWriterFree(hc->packW);
    hc->packW = NULL;
    SDL_free(hc->packData);
    hc->packData = NULL;
    hc->packFileOpen = false;
    return false;
}

/* Start the next bulk transfer for a host: the manifest when it is owed,
 * else the next piece of the packed stream, else the next piece of the
 * file being sent, else the next file asked for. One transfer at a time,
 * so a piece is compressed only when the last one has gone. */
static void botHostServiceFiles(ServerSim *sim, int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    BulkStreamHeader hdr;
    uint8_t *blob;
    uint32_t blobLen = 0, n;

    /* A player's host on the server's computer is stopped: no files. */
    if (hc->sameServerSent) return;
    if (hc->manifestOwed) {
        BrainPackManifestChoice ch;
        if (bulkSenderBusy(&hc->bulk)) return;
        ch = brainPackManifestChoice(hc->capsSeen, hc->caps,
                                     udpServer.tickCount - hc->joinTick);
        if (ch == BRAIN_PACK_MANIFEST_WAIT) return;
        memset(&hdr, 0, sizeof(hdr));
        hdr.kind = BULK_KIND_BRAIN_MANIFEST;
        hdr.gen = udpServer.brainManifestGen;
        if (ch == BRAIN_PACK_MANIFEST_V2) {
            hdr.totalSize = (uint32_t)udpServer.brainManifestBlob2Len;
            if (bulkSenderBegin(&hc->bulk, &hdr, udpServer.brainManifestBlob2,
                                (uint32_t)udpServer.brainManifestBlob2Len)) {
                hc->manifestOwed = false;
                hc->manifestV2 = true;
                botHostResetLimitInit(&hc->resetLimit);
            }
        } else {
            hdr.totalSize = (uint32_t)udpServer.brainManifestBlobLen;
            if (bulkSenderBegin(&hc->bulk, &hdr, udpServer.brainManifestBlob,
                                (uint32_t)udpServer.brainManifestBlobLen)) {
                hc->manifestOwed = false;
                hc->manifestV2 = false;
                botHostResetLimitInit(&hc->resetLimit);
            }
        }
        return;
    }
    if (botHostServicePack(sim, h)) return;
    if (bulkSenderBusy(&hc->bulk)) return;
    memset(&hdr, 0, sizeof(hdr));
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
        data = brainSyncLoadFile(path, &sz);
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
/* The last measured per-bot value of each host by name, for this server
 * session: a host that leaves and comes back shows it until it measures
 * again. */
#define BOT_HOST_CPU_LAST_MAX (MAX_BOT_HOSTS * 2)
static struct {
    char     name[JOIN_BOT_HOST_NAME_LEN];
    uint32_t milli;
} botHostCpuLast[BOT_HOST_CPU_LAST_MAX];
static int botHostCpuLastNext;

static void botHostCpuLastPut(const char *name, uint32_t milli) {
    int i;
    if (name == NULL || name[0] == '\0') return;
    for (i = 0; i < BOT_HOST_CPU_LAST_MAX; i++) {
        if (strcmp(botHostCpuLast[i].name, name) == 0) {
            botHostCpuLast[i].milli = milli;
            return;
        }
    }
    i = botHostCpuLastNext;
    botHostCpuLastNext = (botHostCpuLastNext + 1) % BOT_HOST_CPU_LAST_MAX;
    SDL_strlcpy(botHostCpuLast[i].name, name, sizeof(botHostCpuLast[i].name));
    botHostCpuLast[i].milli = milli;
}

static uint32_t botHostCpuLastGet(const char *name) {
    int i;
    if (name == NULL || name[0] == '\0') return BOT_HOST_CPU_MILLI_UNKNOWN;
    for (i = 0; i < BOT_HOST_CPU_LAST_MAX; i++) {
        if (strcmp(botHostCpuLast[i].name, name) == 0) {
            return botHostCpuLast[i].milli;
        }
    }
    return BOT_HOST_CPU_MILLI_UNKNOWN;
}

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
        msg[0] != CTRL_BOT_HOST_CPU && msg[0] != CTRL_BOT_HOST_CPU_FREE &&
        msg[0] != CTRL_BOT_HOST_CPU_BOT &&
        msg[0] != CTRL_BOT_HOST_CPU_BUDGET &&
        msg[0] != CTRL_BOT_HOST_MACHINE_ANSWER &&
        msg[0] != CTRL_BOT_HOST_SYNC_CAPS && msg[0] != CTRL_BOT_HOST_PACK_REQ) {
        return;
    }
    dec = transportControlCodecBodyDecoder((ControlEventType)msg[0]);
    if (dec == NULL || !dec(msg + 3, bodyLen, &evt)) return;
    if (evt.type == CTRL_BOT_HOST_CPU) {
        /* Only the percent: the host never says what its machine is. */
        hc->cpu = evt.u.botHostCpu.cpu;
        return;
    }
    if (evt.type == CTRL_BOT_HOST_CPU_BOT) {
        /* One bot's part, only the number. An unknown keeps the last. */
        if (!hc->cpuBotHave ||
            evt.u.botHostCpuBot.milli != BOT_HOST_CPU_MILLI_UNKNOWN) {
            hc->cpuBot = evt.u.botHostCpuBot.milli;
        }
        hc->cpuBotHave = true;
        if (hc->cpuBot != BOT_HOST_CPU_MILLI_UNKNOWN) {
            botHostCpuLastPut(hc->name, hc->cpuBot);
        }
        return;
    }
    if (evt.type == CTRL_BOT_HOST_CPU_BUDGET) {
        /* The full-budget estimate, only the number. */
        if (evt.u.botHostCpuBudget.milli != BOT_HOST_CPU_MILLI_UNKNOWN) {
            hc->cpuBudget = evt.u.botHostCpuBudget.milli;
            hc->cpuBudgetHave = true;
        }
        return;
    }
    if (evt.type == CTRL_BOT_HOST_MACHINE_ANSWER) {
        /* Only an answer to the salt round now counts. */
        if (botHostSaltHave &&
            memcmp(evt.u.botHostMachineAnswer.salt, botHostSalt,
                   sizeof(botHostSalt)) == 0) {
            hc->machineHave = true;
            hc->machineSeq = botHostSaltSeq;
            memcpy(hc->machineHash, evt.u.botHostMachineAnswer.hash,
                   sizeof(hc->machineHash));
        }
        return;
    }
    if (evt.type == CTRL_BOT_HOST_SYNC_CAPS) {
        bool late = !hc->capsSeen && !hc->manifestOwed && !hc->manifestV2;
        uint8_t caps = evt.u.botHostSyncCaps.caps;
        if (caps & BRAIN_PACK_CAP_RESET) {
            /* The host dropped its download (its player turned off "run
             * bots on this computer"): stop sending, keep the caps it
             * had. It asks again when it wants the files. Past a few
             * resets of one list, the requests it already made stay
             * counted, so a reset loop cannot make the tick thread load
             * the brain files again and again. A new list clears all. */
            bool clearReqs = botHostResetLimitTake(&hc->resetLimit);
            int keepTaken = hc->fileReqTaken;
            bool keepSeen[BRAIN_SYNC_MAX_BRAINS];
            memcpy(keepSeen, hc->packReqSeen, sizeof(keepSeen));
            botHostFileReset(hc);
            if (!clearReqs) {
                hc->fileReqTaken = keepTaken;
                memcpy(hc->packReqSeen, keepSeen, sizeof(keepSeen));
            }
            if (hc->syncGen == udpServer.brainManifestGen) {
                hc->syncReady = false;
            }
            caps = (uint8_t)(caps & ~BRAIN_PACK_CAP_RESET);
            hc->caps = caps;
            hc->capsSeen = true;
            return;
        }
        hc->capsSeen = true;
        hc->caps = caps;
        if (late && (hc->caps & BRAIN_PACK_CAP_PACKED) &&
            udpServer.brainManifestGen != 0) {
            /* The v1 list went out before the caps came: send the v2 list
             * of the same generation, and the host starts again with it. */
            botHostFileReset(hc);
            hc->manifestOwed = true;
        }
        return;
    }
    if (evt.type == CTRL_BOT_HOST_PACK_REQ) {
        const BrainSyncManifest *man = &udpServer.brainManifest;
        int b = evt.u.botHostPackReq.brain;
        if (!hc->manifestV2 || evt.u.botHostPackReq.gen != udpServer.brainManifestGen ||
            b >= man->brainCount ||
            evt.u.botHostPackReq.fileCount != (uint16_t)man->brains[b].fileCount ||
            hc->packQueue >= BRAIN_SYNC_MAX_BRAINS) {
            return;
        }
        /* One request a brain for each list the host is sent: a repeat
         * would load and hash the brain's files again on the tick thread.
         * botHostFileReset clears it with each new list. */
        if (hc->packReqSeen[b]) return;
        hc->packReqSeen[b] = true;
        hc->brainInPlace[b] = botHostPackReqAllInstall(
            evt.u.botHostPackReq.bits, sizeof(evt.u.botHostPackReq.bits),
            evt.u.botHostPackReq.fileCount, evt.u.botHostPackReq.fromInstall);
        if (hc->packReqs == 0) hc->packStartTick = udpServer.tickCount;
        hc->packReqs++;
        hc->packFilesAll += evt.u.botHostPackReq.fileCount;
        hc->packFromInstall += evt.u.botHostPackReq.fromInstall;
        hc->packFromCache += evt.u.botHostPackReq.fromCache;
        if (!botHostCodeCustom) {
            /* -bothost-code official: no brain file goes to a host. The
             * host runs the brain from its install, or not at all. */
            return;
        }
        hc->packQBrain[hc->packQueue] = (uint8_t)b;
        memcpy(hc->packQBits[hc->packQueue], evt.u.botHostPackReq.bits,
               sizeof(hc->packQBits[0]));
        hc->packQueue++;
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
        /* -bothost-code official: no brain file goes to a host. */
        if (!botHostCodeCustom) return;
        if (hc->fileReqCount >= (int)(sizeof(hc->fileReq) / sizeof(hc->fileReq[0]))) {
            return;
        }
        /* At most each file of the list once, and a few asked again (the
         * host itself asks again at most 3 times a list):
         * each request loads and hashes a file on the tick thread. */
        {
            int files = 0, b;
            for (b = 0; b < udpServer.brainManifest.brainCount; b++) {
                files += udpServer.brainManifest.brains[b].fileCount;
            }
            if (hc->fileReqTaken >= files + BOT_HOST_FILE_REQ_EXTRA) return;
            hc->fileReqTaken++;
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
    if (hc->sameServerSent) {
        /* Stopped on the server's computer: never "syncing", so the
         * lobby's Ready does not wait for it. */
        evt->u.botHostStatus.state = BOT_HOST_SYNC_WAITING;
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

/* The server's full-budget estimate for one more bot of its own
 * (botHostCpuMilliBudget): the think budget the bot runners would give it
 * (botManagerComputePerBotTargetMs with one more bot) over the 20 ms
 * between thinks, over the logical cores. Worked out every
 * BOT_HOST_SERVER_BUDGET_TICKS so the lobby row does not change each tick. */
#define BOT_HOST_SERVER_BUDGET_TICKS 100u   /* 2 s */
#define BOT_HOST_THINK_EVERY_MS      20.0   /* one think per tick */
static uint32_t botHostServerBudgetMilli = BOT_HOST_CPU_MILLI_UNKNOWN;
static uint32_t botHostServerBudgetTick;
static int      botHostServerBotsNow;

static void botHostSampleServerCpu(ServerSim *sim, int bots) {
    if (!botHostServerCpuInit) {
        botHostCpuMeterInit(&botHostServerCpu);
        botHostServerCpuInit = true;
    }
    botHostServerCpuValue = botHostCpuMeterSample(
        &botHostServerCpu, SDL_GetTicksNS(), botManagerThinkMsTotal(sim),
        SDL_GetNumLogicalCPUCores(), bots);
    botHostServerBotsNow = bots;
    if (botHostServerBudgetMilli == BOT_HOST_CPU_MILLI_UNKNOWN ||
        udpServer.tickCount - botHostServerBudgetTick >=
            BOT_HOST_SERVER_BUDGET_TICKS) {
        botHostServerBudgetMilli = botHostCpuMilliBudget(
            botManagerComputePerBotTargetMs(sim, bots + 1),
            BOT_HOST_THINK_EVERY_MS, SDL_GetNumLogicalCPUCores());
        botHostServerBudgetTick = udpServer.tickCount;
    }
}

/* The per-bot value the lobby shows for row h (MAX_BOT_HOSTS: the server's
 * own row) and where it comes from (BOT_HOST_CPU_SRC_*): the measured
 * value while its bots think in a game; the last measured value of that
 * host in this server session; the full-budget estimate; else none, with
 * *milli BOT_HOST_CPU_MILLI_UNKNOWN. *budget (may be NULL) gets the
 * full-budget estimate, or BOT_HOST_CPU_MILLI_UNKNOWN. */
static uint8_t botHostCpuShown(int h, uint32_t *milli, uint32_t *budget) {
    uint32_t meas, bud;
    int bots;
    bool running = botHostSim != NULL &&
                   serverSimGetState(botHostSim) == serverStateRunning;
    if (h == MAX_BOT_HOSTS) {
        meas = botHostServerCpuInit ? botHostServerCpu.perBotMilli
                                    : BOT_HOST_CPU_MILLI_UNKNOWN;
        bud = botHostServerBudgetMilli;
        bots = botHostServerBotsNow;
    } else {
        const BotHostConn *hc = &udpServer.botHosts[h];
        meas = hc->cpuBotHave ? hc->cpuBot : BOT_HOST_CPU_MILLI_UNKNOWN;
        if (meas == BOT_HOST_CPU_MILLI_UNKNOWN) {
            meas = botHostCpuLastGet(hc->name);
        }
        bud = hc->cpuBudgetHave ? hc->cpuBudget : BOT_HOST_CPU_MILLI_UNKNOWN;
        bots = botHostUsedSlots(h);
    }
    if (budget != NULL) *budget = bud;
    if (meas != BOT_HOST_CPU_MILLI_UNKNOWN) {
        *milli = meas;
        return (running && bots > 0) ? (uint8_t)BOT_HOST_CPU_SRC_MEASURED
                                     : (uint8_t)BOT_HOST_CPU_SRC_LAST;
    }
    if (bud != BOT_HOST_CPU_MILLI_UNKNOWN) {
        *milli = bud;
        return (uint8_t)BOT_HOST_CPU_SRC_BUDGET;
    }
    *milli = BOT_HOST_CPU_MILLI_UNKNOWN;
    return (uint8_t)BOT_HOST_CPU_SRC_NONE;
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

/* Row h's per-bot value and owner for CTRL_BOT_HOST_CPU_BOT. False for a
 * row from an older host, which then shows the CTRL_BOT_HOST_CPU text. */
static bool botHostRowCpuBot(int h, const ControlEvent *row, uint32_t *milli,
                             uint8_t *owner, uint8_t *source) {
    const BotHostConn *hc;
    if (!row->u.botHostStatus.connected) return false;
    *owner = (uint8_t)BOT_HOST_CPU_OWNER_NONE;
    if (h == MAX_BOT_HOSTS) {
        *source = botHostCpuShown(h, milli, NULL);
        return true;
    }
    hc = &udpServer.botHosts[h];
    /* An older host sends neither value; its row shows the byte. */
    if (!hc->cpuBotHave && !hc->cpuBudgetHave) return false;
    *source = botHostCpuShown(h, milli, NULL);
    if (hc->playerHost && hc->ownerSlot < MAX_TANKS) *owner = hc->ownerSlot;
    return true;
}

static void botHostFillCpuBot(int h, uint32_t milli, uint8_t owner,
                              uint8_t source, ControlEvent *evt) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_BOT_HOST_CPU_BOT;
    evt->u.botHostCpuBot.idx = (uint8_t)h;
    evt->u.botHostCpuBot.owner = owner;
    evt->u.botHostCpuBot.milli = milli;
    evt->u.botHostCpuBot.source = source;
}

/* The game ping of a player host's player (the player list's value), or
 * 0 when there is none yet or the host is not a player's own. */
static int botHostOwnerPing(const BotHostConn *hc) {
    const UdpServerClient *c;
    if (!hc->playerHost || hc->ownerSlot >= MAX_TANKS) return 0;
    c = &udpServer.clients[hc->ownerSlot];
    return c->connected ? (int)c->pingMs : 0;
}

/* Row h's lag for CTRL_BOT_HOST_LAG (botHostRowPing): a player host's
 * player's game ping, else the placement's smoothed lag in whole ms (at
 * least 1), or BOT_HOST_LAG_UNKNOWN. */
static uint16_t botHostRowLag(int h, const ControlEvent *row) {
    const BotHostConn *hc;
    if (h >= MAX_BOT_HOSTS || !row->u.botHostStatus.connected) {
        return (uint16_t)BOT_HOST_LAG_UNKNOWN;
    }
    hc = &udpServer.botHosts[h];
    return botHostRowPing(hc->playerHost, botHostOwnerPing(hc), hc->lagKnown,
                          hc->lagMs);
}

/* The lags last sent to the lobby, one per Bot Host row, and when. */
#define BOT_HOST_LAG_SEND_TICKS 50   /* at most once each second */
static bool     botHostLagSentHave[MAX_BOT_HOSTS];
static uint16_t botHostLagSent[MAX_BOT_HOSTS];
static uint32_t botHostLagSentTick[MAX_BOT_HOSTS];

static void botHostFillLag(int h, uint16_t lag, ControlEvent *evt) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_BOT_HOST_LAG;
    evt->u.botHostLag.idx = (uint8_t)h;
    evt->u.botHostLag.lagMs = lag;
}

/* The per-bot values last sent to the lobby, one per row. */
static bool     botHostCpuBotSentHave[MAX_BOT_HOSTS + 1];
static uint32_t botHostCpuBotSent[MAX_BOT_HOSTS + 1];
static uint8_t  botHostCpuBotOwnerSent[MAX_BOT_HOSTS + 1];
static uint8_t  botHostCpuBotSrcSent[MAX_BOT_HOSTS + 1];

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
        {
            /* One bot's part, after the byte above, when it changes. */
            uint32_t milli = BOT_HOST_CPU_MILLI_UNKNOWN;
            uint8_t owner = (uint8_t)BOT_HOST_CPU_OWNER_NONE;
            uint8_t source = (uint8_t)BOT_HOST_CPU_SRC_NONE;
            if (!botHostRowCpuBot(h, &evt, &milli, &owner, &source)) {
                botHostCpuBotSentHave[h] = false;
            } else if (!botHostCpuBotSentHave[h] ||
                       milli != botHostCpuBotSent[h] ||
                       owner != botHostCpuBotOwnerSent[h] ||
                       source != botHostCpuBotSrcSent[h]) {
                ControlEvent ce;
                botHostCpuBotSentHave[h] = true;
                botHostCpuBotSent[h] = milli;
                botHostCpuBotOwnerSent[h] = owner;
                botHostCpuBotSrcSent[h] = source;
                botHostFillCpuBot(h, milli, owner, source, &ce);
                serverBroadcastLobbyControl(&ce);
            }
        }
        if (h < MAX_BOT_HOSTS) {
            /* The row's computer and slot override, when they change. */
            if (!evt.u.botHostStatus.connected) {
                botHostRowSlotsSentHave[h] = false;
            } else {
                ControlEvent re;
                botHostFillRowSlots(h, &re);
                if (!botHostRowSlotsSentHave[h] ||
                    memcmp(&re.u.botHostRowSlots,
                           &botHostRowSlotsSent[h].u.botHostRowSlots,
                           sizeof(re.u.botHostRowSlots)) != 0) {
                    botHostRowSlotsSentHave[h] = true;
                    botHostRowSlotsSent[h] = re;
                    serverBroadcastLobbyControl(&re);
                }
            }
        }
        if (h < MAX_BOT_HOSTS) {
            /* The host's lag, the lobby's ping for the row: when it changes
             * by BOT_HOST_LAG_SEND_STEP_MS or more, at most once a second. */
            uint16_t lag = botHostRowLag(h, &evt);
            if (!evt.u.botHostStatus.connected) {
                botHostLagSentHave[h] = false;
            } else if (lag == BOT_HOST_LAG_UNKNOWN && !botHostLagSentHave[h]) {
                /* Nothing measured yet: the row shows no ping. */
            } else if (!botHostLagSentHave[h] ||
                       (udpServer.tickCount - botHostLagSentTick[h] >=
                            BOT_HOST_LAG_SEND_TICKS &&
                        ((lag == BOT_HOST_LAG_UNKNOWN) !=
                             (botHostLagSent[h] == BOT_HOST_LAG_UNKNOWN) ||
                         abs((int)lag - (int)botHostLagSent[h]) >=
                             BOT_HOST_LAG_SEND_STEP_MS))) {
                ControlEvent le;
                botHostLagSentHave[h] = true;
                botHostLagSent[h] = lag;
                botHostLagSentTick[h] = udpServer.tickCount;
                botHostFillLag(h, lag, &le);
                serverBroadcastLobbyControl(&le);
            }
        }
    }
}

/* CTRL_BOT_TIMING: every bot's think timing, the bots in the server's
 * process and the bots on each Bot Host, for the System Info of every
 * player and spectator. Once a second while the game has bots, then once
 * with no bots when the last bot goes, so a client stops showing them. */
#define BOT_TIMING_SEND_TICKS 50   /* once a second */
static bool     botTimingSentHave;
static bool     botTimingSentBots;
static uint32_t botTimingSentTick;

void serverServiceBotTiming(ServerSim *sim) {
    OverrunRecord rec;
    ControlEvent evt;
    bool bots;
    int len;

    if (sim == NULL) return;
    bots = serverSimHasAnyBot(sim);
    if (!bots && !botTimingSentBots) return;
    if (bots && botTimingSentHave && botTimingSentBots &&
        udpServer.tickCount - botTimingSentTick < BOT_TIMING_SEND_TICKS) {
        return;
    }
    serverSimFillOverrunRecord(sim, 0, false, &rec);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_TIMING;
    len = overrunRecordEncode(&rec, evt.u.botTiming.bytes,
                              (int)sizeof(evt.u.botTiming.bytes));
    if (len <= 0) return;
    evt.u.botTiming.len = (uint16_t)len;
    serverBroadcastLobbyControl(&evt);
    botTimingSentHave = true;
    botTimingSentBots = bots;
    botTimingSentTick = udpServer.tickCount;
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
        {
            uint32_t milli;
            uint8_t owner, source;
            if (botHostRowCpuBot(h, &evt, &milli, &owner, &source)) {
                ControlEvent ce;
                botHostFillCpuBot(h, milli, owner, source, &ce);
                deliver(deliverCtx, &ce);
            }
        }
        if (botHostRowLag(h, &evt) != BOT_HOST_LAG_UNKNOWN) {
            ControlEvent le;
            botHostFillLag(h, botHostRowLag(h, &evt), &le);
            deliver(deliverCtx, &le);
        }
        {
            ControlEvent re;
            botHostFillRowSlots(h, &re);
            deliver(deliverCtx, &re);
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
            {
                uint32_t milli;
                uint8_t owner, source;
                if (botHostRowCpuBot(MAX_BOT_HOSTS, &evt, &milli, &owner,
                                     &source)) {
                    ControlEvent ce;
                    botHostFillCpuBot(MAX_BOT_HOSTS, milli, owner, source, &ce);
                    deliver(deliverCtx, &ce);
                }
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
 * ("Brains/GoalHunter/init.lua" gives "GoalHunter"). The production copy in
 * opt/ gives its parent's name ("Brains/GoalHunter/opt/init.lua" gives
 * "GoalHunter", not "opt"). The host finds the brain by that name in its
 * own brain folders (brainListResolve), as a scenario names a brain. A
 * path with no folder gives its file name without ".lua". The same rule as
 * the capacity cache's brain name (brainCapacityNameFromPath). */
void serverBotHostBrainName(const char *path, char *out, size_t sz) {
    if (out == NULL || sz == 0) return;
    brainCapacityNameFromPath(path, out, sz);
}

/* The brain file's version stamp: a CRC-32 of its bytes as brainSyncLoadFile
 * reads them (LF line endings), 0 when it cannot be read. The host warns
 * when its copy differs. */
static uint32_t botHostBrainVersion(const char *path) {
    size_t size = 0;
    void *data;
    uint32_t crc;
    if (path == NULL || path[0] == '\0') return 0;
    data = brainSyncLoadFile(path, &size);
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
    serverBotHostBrainName(path, brain, brainSz);
    *ai = (uint8_t)botManagerGetBotAi(sim, (BYTE)s);
    *team = (lp != NULL) ? lp->teamNumber : 0;
    initArg[0] = '\0';
    if (path != NULL) {
        botManagerBuildInitArg(sim, (BYTE)s, path, NULL, initArg, argSz);
    }
}

/* The index of brain in the current manifest, or -1. */
static int botHostManifestIndex(const char *brain) {
    const BrainSyncManifest *man = &udpServer.brainManifest;
    int b;
    if (brain == NULL || brain[0] == '\0') return -1;
    for (b = 0; b < man->brainCount; b++) {
        if (strcmp(man->brains[b].name, brain) == 0) return b;
    }
    return -1;
}

/* What decides where a seat may run (bot_host_guard.h), worked out once:
 * its brain's index in the manifest, whether its view is culled, and
 * whether its init arg and table are on the list a player's official-only
 * host takes. */
typedef struct {
    int  brain;
    bool culled;
    bool tokensAllowed;
} BotSeatFitInfo;

static void botSeatFitInfo(ServerSim *sim, int s, BotSeatFitInfo *out) {
    char brain[BOT_HOST_BRAIN_NAME_LEN];
    char initArg[BOT_HOST_INIT_ARG_LEN];
    uint8_t ai = 0, team = 0;
    botSeatCurrent(sim, s, brain, sizeof(brain), &ai, &team, initArg,
                   sizeof(initArg));
    out->brain = botHostManifestIndex(brain);
    out->culled = ai == (uint8_t)aiYes;
    out->tokensAllowed =
        botHostInitArgAllowed(initArg) &&
        botHostInitTableAllowed(botManagerGetBotInitTable(sim, (BYTE)s));
}

/* True when a host runs only official brains: the server sends none
 * (-bothost-code official), or the host said so (a player's built-in host
 * built with WINBOLO_BOTHOST_OFFICIAL_ONLY). */
static bool botHostOfficialOnly(int h) {
    return !botHostCodeCustom ||
           (udpServer.botHosts[h].caps & BRAIN_PACK_CAP_OFFICIAL_ONLY) != 0;
}

/* True when a seat with info may run on Bot Host h: a bot with an unculled
 * view runs on a player's computer only when that host said it takes one
 * (BRAIN_PACK_CAP_FULL_MAP; a host that has not sent its caps yet, or an
 * older host, has not); a host that runs only official
 * brains takes a bot only when its install holds the bot's brain; and a
 * player's official-only host takes only the init tokens on the list. */
static bool botSeatFitsHost(const BotSeatFitInfo *info, int h) {
    const BotHostConn *hc;
    BotHostSeatFit fit;
    if (h < 0 || h >= MAX_BOT_HOSTS) return false;
    hc = &udpServer.botHosts[h];
    memset(&fit, 0, sizeof(fit));
    fit.playerHost = hc->playerHost;
    fit.officialOnly = botHostOfficialOnly(h);
    fit.culled = info->culled;
    fit.brainInPlace = info->brain >= 0 && info->brain < BRAIN_SYNC_MAX_BRAINS &&
                       hc->brainInPlace[info->brain];
    fit.tokensAllowed = info->tokensAllowed;
    fit.fullMapOk = (hc->caps & BRAIN_PACK_CAP_FULL_MAP) != 0;
    return botHostSeatMayRunOn(&fit);
}

/* True when seat s may run on Bot Host h now. */
static bool botSeatHostMayRun(ServerSim *sim, int s, int h) {
    BotSeatFitInfo info;
    if (sim == NULL || h < 0 || h >= MAX_BOT_HOSTS) return false;
    botSeatFitInfo(sim, s, &info);
    return botSeatFitsHost(&info, h);
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

static int botHostServerSlots(ServerSim *sim);
static void botHostHoldSeat(ServerSim *sim, int s, int from,
                            const char *why);

/* True while the server's own child Bot Host is linked. Then no bot is held
 * for no room: a bot the child does not run thinks in the server's process
 * (the DS rule), never with its tank standing still. */
static bool botHostDsChildLinked(void) {
    int h;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (udpServer.botHosts[h].connected &&
            udpServer.botHosts[h].localChild) {
            return true;
        }
    }
    return false;
}

/* A seat leaves its host and its brain comes back in-process. */
static void botSeatFallBack(ServerSim *sim, int s, const char *why,
                            bool tellHost) {
    BotHostSeat *seat = &udpServer.botSeats[s];
    bool wasRemote;
    const char *hostName = "?";
    int fromPlace = seat->host >= 0 ? seat->host + 1 : -1;

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
    /* A fall-back has no penalty: the seat's m stays (an overrun move
     * counts itself as voluntary after this). */
    botHostOvrMovesNote(&botHostOvrMv, s, BOT_HOST_MOVE_FORCED,
                        udpServer.tickCount);
    if (wasRemote &&
        botHostServerFallBackHolds(botHostEnforceServerLimit &&
                                       !botHostDsChildLinked(),
                                   serverBotHostServerBots(sim),
                                   botHostServerSlots(sim))) {
        /* -bothost-enforce-server-limit: the server has no slot for it. It
         * is held until a place has one (botHostHeldStep). */
        char why2[96];
        snprintf(why2, sizeof(why2), "back from Bot Host \"%s\" (%s), and "
                 "the server's %d slots are full", hostName, why,
                 botHostServerSlots(sim));
        botHostHoldSeat(sim, s, fromPlace, why2);
        return;
    }
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
    memset(&botHostMachine[h], 0, sizeof(botHostMachine[h]));
    botHostMachine[h].other = -1;
    botHostMachineKnown[h] = false;
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
        extra[baseLen + 2] == JOIN_BOT_HOST_DS_CHILD_MARK) {
        /* The server's own Bot Host child (src/server/ds_bot_child.c): the
         * server's secret, from this computer. It counts as the server's
         * own process: lag 0, no same-computer rule, no game bot limit of
         * its own. The key, the list and -noplayerbothosts do not apply. */
        const uint8_t *pm = extra + baseLen + 2;
        botHostOwnIpsRefresh();
        if (!udpServer.botHostEnabled || !udpServer.dsChildHaveSecret ||
            !botHostAddrIsServerMachine(fromAddr->sin_addr.s_addr,
                                        botHostOwnIp, botHostOwnIpCount) ||
            !botHostBytesEqual(pm + 2, udpServer.dsChildSecret,
                               BOT_HOST_TICKET_LEN)) {
            fprintf(stderr, "[bothost] server Bot Host child join from %s "
                            "refused: no such child\n", addr);
            WB_LOG_WARN(WB_LOG_CAT_NET, "[bothost] server Bot Host child "
                        "join from %s refused", addr);
            serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0,
                                 NULL);
            return;
        }
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            /* One child: a new one replaces the old. */
            if (udpServer.botHosts[h].connected &&
                udpServer.botHosts[h].localChild) {
                botHostDrop(sim, h, "the server's Bot Host child joined "
                                    "again");
            }
        }
        slots = extra[JOIN_BOT_HOST_KEY_LEN];
        if (slots > BOT_HOST_MAX_SLOTS) slots = BOT_HOST_MAX_SLOTS;
        memcpy(hostName, extra + JOIN_BOT_HOST_KEY_LEN + 1, sizeof(hostName));
        hostName[sizeof(hostName) - 1] = '\0';
        if (hostName[0] == '\0') {
            SDL_strlcpy(hostName, "server child", sizeof(hostName));
        }
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            if (!udpServer.botHosts[h].connected) {
                freeIdx = h;
                break;
            }
        }
        if (freeIdx < 0) {
            fprintf(stderr, "[bothost] server Bot Host child refused: %d Bot "
                            "Hosts already connected\n", MAX_BOT_HOSTS);
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
            hc->lagKnown = true;
            hc->lagMs = 0.0;
            hc->lagConnTick = udpServer.tickCount;
            hc->localChild = true;
            hc->lastReceivedTick = udpServer.tickCount;
            hc->outSequence = 1;
            channelMuxInit(&hc->channelMux);
            bulkSenderInit(&hc->bulk);
        }
        fprintf(stderr, "[bothost] the server's Bot Host child \"%s\" "
                        "connected from %s with %u slots\n",
                hostName, addr, (unsigned)slots);
        WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] server Bot Host child \"%s\" "
                    "connected from %s slots=%u", hostName, addr,
                    (unsigned)slots);
        goto accepted;
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
        /* Shown in the lobby's Bot Host rows: the player's name. A player
         * with no name sends an empty one, and each client shows its own
         * text for the row. */
        SDL_strlcpy(hostName, oc->playerName, sizeof(hostName));
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
    {
        /* Its join order, its slot override (none, or -bothost-slots), and
         * a new salt round for every host. */
        BotHostConn *hc = &udpServer.botHosts[freeIdx];
        const char *player = hc->playerHost
            ? transportUdpServerGetPlayerName((BYTE)hc->ownerSlot) : NULL;
        hc->joinSeq = botHostJoinSeqNext++;
        botHostSlotOverrideJoin(&hc->slotOvr, &botHostSlotFlags, hc->name,
                                player);
        if (hc->slotOvr.locked) {
            fprintf(stderr, "[bothost] Bot Host \"%s\": at most %d slots "
                            "(-bothost-slots)\n",
                    hc->name, hc->slotOvr.override);
        }
        memset(&botHostMachine[freeIdx], 0, sizeof(botHostMachine[freeIdx]));
        botHostMachine[freeIdx].other = -1;
        /* No bot until its group is known (botHostMachineUpdate). */
        botHostMachineKnown[freeIdx] = false;
    }
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
        /* A new host sends its caps at once; the list waits for them a
         * little (BRAIN_PACK_CAPS_WAIT_TICKS). */
        hc->joinTick = udpServer.tickCount;
    }
    botHostMachineRound("a Bot Host joined");
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
        if (botHostOpenAnnounceAny &&
            udpServer.tickCount - botHostOpenAnnounceTick < 25) {
            return;
        }
        botHostOpenAnnounceAny = true;
        botHostOpenAnnounceTick = udpServer.tickCount;
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
    {
        /* The host connects its bots one by one (each brain loads first).
         * A bind is progress: the host's other pending seats wait again
         * from now, so a long queue does not time out behind it. */
        int o;
        for (o = 0; o < MAX_TANKS; o++) {
            BotHostSeat *other = &udpServer.botSeats[o];
            if (o != s && other->state == BOT_SEAT_PENDING &&
                other->host == seat->host) {
                other->sinceTick = udpServer.tickCount;
                other->sinceMs = SDL_GetTicks();
            }
        }
    }
    seat->mineSeedDone = false;
    seat->mineSeedColumn = 0;
    fprintf(stderr, "[bothost] bot %d \"%s\" bound to Bot Host \"%s\" from %s\n",
            s, botSeatName(sim, s),
            seat->host >= 0 ? udpServer.botHosts[seat->host].name : "?", addr);
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] bot %d bound from %s", s, addr);
}

static void botHostHeldRunsAgain(ServerSim *sim, int s);

bool serverBotSlotOnInput(ServerSim *sim, int slot) {
    BotHostSeat *seat = &udpServer.botSeats[slot];
    if (seat->state == BOT_SEAT_REMOTE) return true;
    if (seat->state != BOT_SEAT_BOUND) return false;
    {
        bool wasHeld = botManagerIsHeldNoRoom(sim, (BYTE)slot);
        if (!botManagerSetRemote(sim, (BYTE)slot)) {
            botSeatFallBack(sim, slot, "seat cannot move", true);
            return false;
        }
        seat->state = BOT_SEAT_REMOTE;
        seat->sinceTick = udpServer.tickCount;
        seat->lastPollTick = udpServer.tickCount;
        fprintf(stderr, "[bothost] bot %d \"%s\" now thinks on Bot Host "
                        "\"%s\" (game tick %u)\n",
                slot, botSeatName(sim, slot),
                seat->host >= 0 ? udpServer.botHosts[seat->host].name : "?",
                (unsigned)serverSimGetTick(sim));
        if (wasHeld) botHostHeldRunsAgain(sim, slot);
    }
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
    if (!botSeatHostMayRun(sim, (int)seat, bs->host)) {
        /* The new table holds tokens its host may not take (a player's
         * official-only host, bot_host_guard.h): the bot comes back to the
         * server and reads the table there. */
        botSeatFallBack(sim, (int)seat, "its new init table is not for its "
                        "host", true);
        udpServer.botSeats[seat].noReturn = false;
        return;
    }
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

/* The name of the host a seat's brain runs on: the lobby row's name. */
static bool botHookHostName(void *ctx, BYTE seat, char *out, size_t cap) {
    const BotHostSeat *bs;
    (void)ctx;
    if (seat >= MAX_TANKS || out == NULL || cap == 0) return false;
    bs = &udpServer.botSeats[seat];
    if (bs->state == BOT_SEAT_LOCAL || bs->host < 0 ||
        bs->host >= MAX_BOT_HOSTS || !udpServer.botHosts[bs->host].connected) {
        return false;
    }
    SDL_strlcpy(out, udpServer.botHosts[bs->host].name, cap);
    return out[0] != '\0';
}

/* Turn Bot Hosts on or off to match the key and the list. */
static void botHostApplySettings(ServerSim *sim) {
    BotRemoteHooks hooks;
    int s;
    bool want;

    udpServer.botHostOpen = botHostOpenToAll();
    want = udpServer.botHostOpen || !udpServer.noPlayerBotHosts ||
           udpServer.dsChildWanted;
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
    hooks.hostName   = botHookHostName;
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

/* ── The server's own Bot Host child ─────────────────────────────────── */

bool transportUdpServerDsChildBegin(ServerSim *sim,
                                    uint8_t *secretOut) {
    if (sim == NULL || secretOut == NULL) return false;
    botHostSim = sim;
    if (!serverFillRandomBytes(udpServer.dsChildSecret, BOT_HOST_TICKET_LEN)) {
        return false;
    }
    memcpy(secretOut, udpServer.dsChildSecret, BOT_HOST_TICKET_LEN);
    udpServer.dsChildHaveSecret = true;
    udpServer.dsChildWanted = true;
    botHostApplySettings(sim);
    return udpServer.botHostEnabled;
}

void transportUdpServerDsChildEnd(ServerSim *sim, const char *why) {
    int h;
    udpServer.dsChildHaveSecret = false;
    memset(udpServer.dsChildSecret, 0, sizeof(udpServer.dsChildSecret));
    if (sim == NULL) return;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (udpServer.botHosts[h].connected &&
            udpServer.botHosts[h].localChild) {
            botHostDrop(sim, h, why != NULL ? why
                                            : "the server's Bot Host child "
                                              "stopped");
        }
    }
    /* dsChildWanted stays: a restart begins again with a new secret. */
}

bool transportUdpServerDsChildConnected(void) {
    int h;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (udpServer.botHosts[h].connected &&
            udpServer.botHosts[h].localChild) {
            return true;
        }
    }
    return false;
}

/* The lockstep wait (bot_host_lockstep.h). */
static BotHostLockstep dsLock;
static bool dsLockArmed;
static int dsLockWaitMs = TRANSPORT_UDP_LOCKSTEP_WAIT_MS_DEFAULT;
static uint32_t dsLockStatArmed, dsLockStatMet, dsLockStatLate;
static double dsLockStatWaitMs;

void transportUdpServerSetLockstepWaitMs(int ms) {
    dsLockWaitMs = ms < 0 ? 0 : ms;
}

int transportUdpServerGetLockstepWaitMs(void) {
    return dsLockWaitMs;
}

static void dsLockCounts(uint32_t *cnt) {
    int s;
    for (s = 0; s < MAX_TANKS; s++) {
        cnt[s] = udpServer.clients[s].inputNewCount;
    }
}

void transportUdpServerLockstepArm(ServerSim *sim) {
    bool on[MAX_TANKS];
    uint32_t cnt[MAX_TANKS];
    int s;
    dsLockArmed = false;
    if (sim == NULL || dsLockWaitMs <= 0 || !udpServer.botHostEnabled ||
        serverSimGetState(sim) != serverStateRunning) {
        return;
    }
    for (s = 0; s < MAX_TANKS; s++) {
        const BotHostSeat *seat = &udpServer.botSeats[s];
        on[s] = seat->state == BOT_SEAT_REMOTE && seat->host >= 0 &&
                seat->host < MAX_BOT_HOSTS &&
                udpServer.botHosts[seat->host].connected &&
                udpServer.botHosts[seat->host].localChild &&
                udpServer.clients[s].connected && udpServer.clients[s].botSlot;
    }
    dsLockCounts(cnt);
    botHostLockstepArm(&dsLock, on, cnt, MAX_TANKS);
    dsLockArmed = dsLock.wanted > 0;
}

double transportUdpServerLockstepWait(ServerSim *sim) {
    Uint64 start, freq;
    double waited = 0.0;
    uint32_t cnt[MAX_TANKS];
    BotHostLockstepStep step;
    if (!dsLockArmed || sim == NULL) return 0.0;
    dsLockArmed = false;
    if (serverSimGetState(sim) != serverStateRunning) return 0.0;
    dsLockStatArmed++;
    start = SDL_GetPerformanceCounter();
    freq = SDL_GetPerformanceFrequency();
    for (;;) {
        dsLockCounts(cnt);
        waited = (double)(SDL_GetPerformanceCounter() - start) * 1000.0 /
                 (double)freq;
        step = botHostLockstepStep(
            botHostLockstepMissing(&dsLock, cnt, MAX_TANKS), dsLock.wanted,
            waited, dsLockWaitMs);
        if (step != BOT_HOST_LOCKSTEP_WAIT) break;
        SDL_DelayNS(200000);   /* 0.2 ms */
        /* The packets only: this is still the same tick. */
        transportUdpServerRecvPacketsOnly(sim);
    }
    if (step == BOT_HOST_LOCKSTEP_MET) dsLockStatMet++;
    if (step == BOT_HOST_LOCKSTEP_LATE) dsLockStatLate++;
    dsLockStatWaitMs += waited;
    return waited;
}

void transportUdpServerLockstepStats(uint32_t *armed, uint32_t *met,
                                     uint32_t *late, double *waitMs) {
    if (armed) *armed = dsLockStatArmed;
    if (met) *met = dsLockStatMet;
    if (late) *late = dsLockStatLate;
    if (waitMs) *waitMs = dsLockStatWaitMs;
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
        serverBotHostBrainName(path, brain, sizeof(brain));
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
    memset(botHostCpuBotSentHave, 0, sizeof(botHostCpuBotSentHave));
    memset(botHostLagSentHave, 0, sizeof(botHostLagSentHave));
    memset(botHostRowSlotsSentHave, 0, sizeof(botHostRowSlotsSentHave));
    memset(botHostMachine, 0, sizeof(botHostMachine));
    for (h = 0; h < MAX_BOT_HOSTS; h++) botHostMachine[h].other = -1;
    memset(botHostMachineKnown, 0, sizeof(botHostMachineKnown));
    botHostMachineLogged[0] = '\0';
    botHostSaltHave = false;
    botHostLimitLogged = -100;
    botHostServerCpuInit = false;
    botHostServerCpuValue = (uint8_t)BOT_HOST_CPU_UNKNOWN;
    for (h = 0; h <= MAX_BOT_HOSTS; h++) botHostOvrResetHost(h);
    botHostServerFreeHave = false;
    botHostServerFreeValue = (uint8_t)BOT_HOST_FREE_UNKNOWN;
    botHostRecWasOn = false;
    botHostOpenAnnounceTick = 0;
    botHostOpenAnnounceAny = false;
    botHostRuleShown = false;
    botHostTestOtherComputer =
        SDL_getenv("WINBOLO_BOTHOST_TEST_OTHER_COMPUTER") != NULL;
    botHostPlaceStateReset();
    /* The per-seat placement bookkeeping starts again with the seats. */
    memset(botSeatFitTick, 0, sizeof(botSeatFitTick));
    memset(botHostHeldSince, 0, sizeof(botHostHeldSince));
    memset(botHostHeldTold, 0, sizeof(botHostHeldTold));
    memset(botHostBadGaveTick, 0, sizeof(botHostBadGaveTick));
    memset(botHostBadGave, 0, sizeof(botHostBadGave));
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
    if (!botHostSrvSlots.polled ||
        udpServer.tickCount - botHostSrvSlots.lastTick >=
            BOT_HOST_CAP_POLL_TICKS) {
        BotPoolStats ps;
        botHostSrvSlots.polled = true;
        botHostSrvSlots.lastTick = udpServer.tickCount;
        memset(&ps, 0, sizeof(ps));
        botManagerGetPoolStats(sim, &ps);
        /* The host bot capacity (-botcapacity, or the CPU speed test with
           the bot brain's answer) comes first. -1 (no cap) and -2 (the test
           still runs) fall back to the active brains' answer. */
        if (ps.hostBotCapacity >= 0) {
            botHostSrvSlots.known = ps.hostBotCapacity;
            botHostSrvSlots.knownFrom = "the server's host bot capacity";
        } else if (ps.brainCapacity > 0) {
            botHostSrvSlots.known = ps.brainCapacity;
            botHostSrvSlots.knownFrom = "the brain's capacity answer";
        }
    }
    if (serverSimGetServerBotSlots(sim) >= 0) {
        /* The lobby's Server bot slots: a change counts at once. */
        botHostSrvSlots.known = serverSimGetServerBotSlots(sim);
        botHostSrvSlots.knownFrom = serverSimIsServerBotSlotsLocked(sim)
                        ? "-botcapacity" : "the lobby's Server bot slots";
    }
    {
        int slots = botHostSrvSlots.known;
        const char *from = botHostSrvSlots.knownFrom;
        if (slots < 0) {
            BrainMachineInfo machine;
            memset(&machine, 0, sizeof(machine));
            brainCapacityGetMachine(&machine);
            slots = brainCapacityDefault(&machine);
            from = "this machine's default capacity";
        }
        if (slots != botHostSrvSlots.logged) {
            botHostSrvSlots.logged = slots;
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
        /* A bot held for no room runs nowhere. */
        if (botManagerIsHeldNoRoom(sim, (BYTE)s)) continue;
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
    serverBotHostBrainName(path, brainName, sizeof(brainName));
    return brainSyncFindBrain(&udpServer.brainManifest, brainName) != NULL;
}

/* Offers in-process seat s to host h. */
static bool botSeatOffer(ServerSim *sim, int s, int h, const char *why) {
    BotHostSeat *seat = &udpServer.botSeats[s];
    if (h < 0 || h >= MAX_BOT_HOSTS || botHostEffSlots(h) <= 0) {
        /* A host with 0 slots (on the server's computer, its group not
         * known yet, or set to 0) never gets a bot, by any path. */
        return false;
    }
    if (!botSeatHostMayRun(sim, s, h)) {
        /* Not this bot on this host (bot_host_guard.h), by any path. */
        return false;
    }
    if (!serverFillRandomBytes(seat->token, sizeof(seat->token))) {
        WB_LOG_ERROR(WB_LOG_CAT_NET, "[bothost] no random bytes for a token");
        return false;
    }
    seat->state = BOT_SEAT_PENDING;
    seat->host = (int8_t)h;
    seat->sinceTick = udpServer.tickCount;
    seat->sinceMs = SDL_GetTicks();
    seat->reloadOwed = false;
    botSeatSendAssign(sim, s, false);
    fprintf(stderr, "[bothost] bot %d \"%s\" offered to Bot Host \"%s\" "
                    "(%s, %d/%u slots used)\n",
            s, botSeatName(sim, s), udpServer.botHosts[h].name, why,
            botHostUsedSlots(h), (unsigned)botHostEffSlots(h));
    return true;
}

/* Ticks after a join when a round trip of 0 ms still means "not measured
 * yet". */
#define BOT_HOST_LAG_WAIT_TICKS    250

/* Brings the smoothed lag of host h up to date, once a tick. */
static void botHostUpdateLag(int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    uint16_t raw;
    if (hc->localChild) {
        /* The server's own child: the same computer, lag 0. */
        hc->lagMs = 0.0;
        hc->lagKnown = true;
        return;
    }
    /* A player host counts its player's game ping, which is not smoothed
     * (one round trip per pong), so the smoothing below stays; with no
     * player ping yet it counts its own measured lag. */
    raw = botHostLagRaw(hc->playerHost, botHostOwnerPing(hc), hc->pingMs);
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
        SDL_snprintf(cost, sizeof(cost), "%.2f%%%s", rep->oneBotCost,
                     rep->costFromBudget ? " (full budget)" : "");
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

/* A DECISION record. mv (may be NULL) is the overrun move it is about:
 * its rates, m, rule and stay go into the record. */
static void botHostRecDecision(int i, uint8_t decision,
                               const BotHostOverrunReport *rep, int seat,
                               int toHost, const char *reason,
                               const BotHostOvrMove *mv) {
    BotHostRecord r;
    if (!botHostRecOn()) return;
    memset(&r, 0, sizeof(r));
    r.kind = BOTHOST_RECORD_DECISION;
    r.host = (uint8_t)i;
    r.decision = decision;
    r.limitBefore = botHostRecordByte(rep->limitBefore);
    r.limitAfter = botHostRecordByte(rep->newLimit);
    r.badProbes = (uint8_t)BOTHOST_RECORD_NONE;
    r.probes = botHostRecordByte(rep->probes);
    r.flags = (uint8_t)((rep->costKnown ? BOTHOST_RECORD_DEC_COST : 0u) |
                        (rep->freeKnown ? BOTHOST_RECORD_DEC_FREE : 0u) |
                        (rep->costFromBudget ? BOTHOST_RECORD_DEC_BUDGET
                                             : 0u));
    r.predictedTenths = botHostRecordTenthsS(rep->predicted);
    r.costTenths = botHostRecordTenthsU(rep->oneBotCost);
    r.freeAvgTenths = botHostRecordTenthsU(rep->freeAvg);
    r.lowestClean = botHostRecordByte(rep->lowestClean);
    r.highestBad = botHostRecordByte(rep->highestBad);
    r.seat = botHostRecordByte(seat);
    r.toHost = botHostRecordByte(toHost);
    SDL_strlcpy(r.reason, reason != NULL ? reason : "", sizeof(r.reason));
    r.srcRate = (uint16_t)BOTHOST_RECORD_RATE_NONE;
    r.dstRate = (uint16_t)BOTHOST_RECORD_RATE_NONE;
    r.moves = (uint8_t)BOTHOST_RECORD_NONE;
    r.rule = (uint8_t)BOTHOST_RULE_NONE;
    r.staySecs = (uint16_t)BOTHOST_RECORD_SECS_NONE;
    r.needStaySecs = (uint16_t)BOTHOST_RECORD_SECS_NONE;
    if (mv != NULL) {
        r.srcRate = botHostRecordHundredthsU(mv->srcPct);
        if (mv->seat >= 0) {
            if (mv->dstKnown) r.dstRate = botHostRecordHundredthsU(mv->dstPct);
            r.moves = botHostRecordByte(mv->moves);
            r.rule = (uint8_t)mv->rule;
            r.staySecs = botHostRecordSecs(mv->staySecs);
            r.needStaySecs = botHostRecordSecs(mv->needStaySecs);
        }
    }
    botHostRecPut(&r);
}

/* The full-budget estimate of host i (0: the server) as the free CPU
 * check's one-bot cost while no measured cost is known. */
static void botHostOvrSetFallback(int i) {
    uint32_t milli, budget;
    (void)botHostCpuShown(i == 0 ? MAX_BOT_HOSTS : i - 1, &milli, &budget);
    botHostOvr[i].costFallbackKnown = budget != BOT_HOST_CPU_MILLI_UNKNOWN;
    botHostOvr[i].costFallback =
        botHostOvr[i].costFallbackKnown ? (double)budget / 1000.0 : 0.0;
}

/* A spread decision (BOTHOST_DECISION_PLACE or _SPREAD_MOVE) for the round
 * log, with the free CPU check of the host the bot goes to (to: 0 the
 * server, h + 1 Bot Host h), so the record holds the predicted free CPU the
 * move was made against. reason: why the bot moved (botHostSpreadReason). */
static void botHostRecSpread(uint8_t decision, int from, int to, int seat,
                             int bots, const char *reason) {
    const BotHostOverrunCfg *cfg = botHostOvrConfig();
    BotHostOverrunReport rep;
    if (!botHostRecOn() || to < 0 || to > MAX_BOT_HOSTS) return;
    botHostOvrEnsure();
    botHostOvrSetFallback(to);
    memset(&rep, 0, sizeof(rep));
    (void)botHostOverrunMayTake(&botHostOvr[to], cfg, bots,
                                botHostOvrFree(to), &rep);
    botHostRecDecision(from, decision, &rep, seat, to, reason, NULL);
}

/* The stay time of the placement in ticks (-bothost-overrun-stay-secs),
 * 0: none. */
static uint32_t botHostStayTicks(void) {
    const BotHostOverrunCfg *cfg = botHostOvrConfig();
    return cfg->staySecs > 0 ? (uint32_t)cfg->staySecs * 50u : 0u;
}

static void botHostTrailEnsure(void) {
    if (botHostTrailReady) return;
    botHostTrailReset(&botHostTrail);
    botHostTrailReady = true;
}

/* Marks each place whose bots run badly now (BotHostPlaceHost.bad): its
 * window overrun rate at or above the target rate, or its free CPU too
 * low for the bots it has (botHostOverrunFreeLow). A rate or free CPU not
 * known yet counts as good. why[i] gets the reason for the log. A place
 * that gave a bot for being bad within its wait is held (badHold): for a
 * bad rate the wait of that rate (botHostOverrunWaitSecs, also counted
 * from the last overrun move or raise while the wait scales), for low free
 * CPU the step time. short[i] gets the bots its free CPU is short by when
 * the free CPU batch may give them now (-bothost-free-batch, and no bot
 * given within the step time, so the free CPU numbers show the last one),
 * else 0. ratePct[i] gets its rate, below 0 when not known. Only while the
 * game runs, with the overrun check on. */
static void botHostPlaceMarkBad(BotHostPlaceHost *hosts, const char **why,
                                int *shortBy, double *ratePct) {
    const BotHostOverrunCfg *cfg = botHostOvrConfig();
    uint32_t step = (uint32_t)(cfg->stepSecs > 0 ? cfg->stepSecs : 1) * 50u;
    int i;
    for (i = 0; i <= MAX_BOT_HOSTS; i++) {
        why[i] = NULL;
        shortBy[i] = 0;
        ratePct[i] = -1.0;
    }
    if (!(cfg->pct > 0.0)) return;
    botHostOvrEnsure();
    for (i = 0; i <= MAX_BOT_HOSTS; i++) {
        double pct = 0.0, wait;
        bool rateBad, freshFree;
        int lack;
        uint32_t since, hold;
        if (i > 0 && !udpServer.botHosts[i - 1].connected) continue;
        if (hosts[i].bots <= 0) continue;
        rateBad = false;
        if (botHostOverrunRate(&botHostOvr[i], cfg, &pct)) {
            ratePct[i] = pct;
            rateBad = pct >= cfg->pct;
        }
        botHostOvrSetFallback(i);
        lack = botHostOverrunFreeShort(&botHostOvr[i], cfg, hosts[i].bots,
                                       botHostOvrFree(i), NULL);
        if (rateBad) {
            why[i] = "overrun rate";
        } else if (lack > 0) {
            why[i] = "low free CPU";
        }
        if (why[i] == NULL) continue;
        hosts[i].bad = true;
        since = udpServer.tickCount - botHostBadGaveTick[i];
        wait = rateBad ? botHostOverrunWaitSecs(cfg, pct)
                       : (double)(cfg->stepSecs > 0 ? cfg->stepSecs : 1);
        hold = (uint32_t)(wait * 50.0 + 0.5);
        hosts[i].badHold = botHostBadGave[i] && since < hold;
        if (rateBad && botHostOverrunScaled(cfg) &&
            (double)botHostOvr[i].sinceChange < wait) {
            /* An overrun move or a raise within the wait: the probes since
             * then are all the rate knows. */
            hosts[i].badHold = true;
        }
        /* The free CPU average covers the last window: after a bot left
         * (by this placement or by the overrun move) it needs the step
         * time to show that. */
        freshFree = (!botHostBadGave[i] || since >= step) &&
                    botHostOvr[i].sinceChange >= cfg->stepSecs;
        shortBy[i] = botHostOverrunBatchSize(cfg, lack, freshFree);
    }
}

/* The reason a spread decision records and the console shows. */
static const char *botHostSpreadReason(BotHostSpreadWhy why, int from,
                                       const char *const *badWhy) {
    switch (why) {
    case BOT_HOST_SPREAD_OVER_SLOTS: return "over its slots";
    case BOT_HOST_SPREAD_BAD:
        return from >= 0 && from <= MAX_BOT_HOSTS && badWhy[from] != NULL
                   ? badWhy[from] : "overrun rate";
    default: return "the spread";
    }
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
        if (hosts[i].bots >= hosts[i].slots &&
            (!hosts[i].unlimited || hosts[i].capped)) {
            continue;
        }
        botHostOvrSetFallback(i);
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
                               -1, rep.why, NULL);
        }
    }
}

/* The overruns and thinks of each host since its last CPU record. */
static uint32_t botHostOvrAccOver[1 + MAX_BOT_HOSTS];
static uint32_t botHostOvrAccThinks[1 + MAX_BOT_HOSTS];

/* "the server" or "Bot Host \"name\"" for the console. */
static void botHostOvrPlaceText(int i, char *out, size_t sz) {
    if (i == 0) {
        SDL_strlcpy(out, "the server", sz);
    } else {
        SDL_snprintf(out, sz, "Bot Host \"%s\"", udpServer.botHosts[i - 1].name);
    }
}

/* A rate for the console: "1.52%", or "none" for a host with no rate. */
static void botHostOvrRateText(bool known, double pct, char *out, size_t sz) {
    if (known) {
        SDL_snprintf(out, sz, "%.2f%%", pct);
    } else {
        SDL_strlcpy(out, "none", sz);
    }
}

/* One probe of every host, then the overrun move off each host whose rate
 * is above the target (botHostOvrPickMove). applied is the host table
 * after the lowered limits and the free CPU check (where a bot may go);
 * raw the table before them. Returns true when a bot moved. */
static bool botHostOvrProbeAll(ServerSim *sim, const BotHostPlaceHost *raw,
                               const BotHostPlaceHost *applied,
                               bool running) {
    const BotHostOverrunCfg *cfg = botHostOvrConfig();
    uint32_t over[1 + MAX_BOT_HOSTS], thinks[1 + MAX_BOT_HOSTS];
    int bots[1 + MAX_BOT_HOSTS];
    static BotHostOvrBot movable[1 + MAX_BOT_HOSTS][MAX_TANKS];
    int movableCount[1 + MAX_BOT_HOSTS];
    BotHostOverrunReport reps[1 + MAX_BOT_HOSTS];
    BotHostOverrunAction acts[1 + MAX_BOT_HOSTS];
    BotHostOvrHost oh[1 + MAX_BOT_HOSTS];
    bool recOn, moved = false;
    int i, s;

    botHostOvrEnsure();
    memset(over, 0, sizeof(over));
    memset(thinks, 0, sizeof(thinks));
    memset(bots, 0, sizeof(bots));
    memset(movableCount, 0, sizeof(movableCount));
    memset(acts, 0, sizeof(acts));

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
     * start) starts again from its counters now. The bots that may move
     * off their place now are listed per place. */
    for (s = 0; s < MAX_TANKS; s++) {
        BotInfo bi;
        int owner = -1;
        bool mayGo = false;
        memset(&bi, 0, sizeof(bi));
        if (botManagerIsBot(sim, (BYTE)s) &&
            botManagerGetBotInfo(sim, (BYTE)s, &bi)) {
            if (botManagerIsRemote(sim, (BYTE)s)) {
                int h = udpServer.botSeats[s].host;
                if (h >= 0 && h < MAX_BOT_HOSTS &&
                    udpServer.botHosts[h].connected) {
                    owner = h + 1;
                    mayGo = udpServer.botSeats[s].state == BOT_SEAT_REMOTE &&
                            botSeatMayMove(sim, s, running);
                }
            } else {
                owner = 0;
                mayGo = botSeatLocalOfferable(sim, s) &&
                        botSeatMayMove(sim, s, running);
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
        if (mayGo) {
            BotHostOvrBot *b = &movable[owner][movableCount[owner]++];
            b->seat = s;
            b->moves = botHostOvrMovesCount(&botHostOvrMv, s);
            b->staySecs = botHostOvrMovesStay(&botHostOvrMv, s,
                                              udpServer.tickCount, 50);
            b->backTo = botHostTrailBackTo(&botHostTrail, s,
                                           udpServer.tickCount,
                                           botHostStayTicks());
            b->back = b->backTo >= 0;
        }
    }

    recOn = botHostRecOn();
    if (recOn && !botHostRecWasOn) {
        /* A new log: each host is named again first. */
        memset(botHostRecHostSent, 0, sizeof(botHostRecHostSent));
    }
    botHostRecWasOn = recOn;

    /* The probes. */
    for (i = 0; i <= MAX_BOT_HOSTS; i++) {
        BotHostOverrunProbe in;
        BotHostOverrunReport *rep = &reps[i];
        BotHostOverrunAction act;
        char nums[200], before[32], after[32];

        memset(rep, 0, sizeof(*rep));
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

        act = botHostOverrunProbe(&botHostOvr[i], cfg, &in, rep);
        acts[i] = act;

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
            r.flags = (in.thinks > 0 && rep->ratePct > cfg->pct)
                          ? (uint8_t)BOTHOST_RECORD_PROBE_BAD : (uint8_t)0;
            r.badProbes = (uint8_t)BOTHOST_RECORD_NONE;
            r.probes = botHostRecordByte(rep->probes);
            r.windowRate = rep->windowKnown
                               ? botHostRecordHundredthsU(rep->windowPct)
                               : (uint16_t)BOTHOST_RECORD_RATE_NONE;
            botHostRecPut(&r);
            botHostOvrAccOver[i] += in.overruns;
            botHostOvrAccThinks[i] += in.thinks;
            if (rep->sampled) {
                uint32_t shown, budget;
                uint8_t src = botHostCpuShown(i == 0 ? MAX_BOT_HOSTS : i - 1,
                                              &shown, &budget);
                memset(&r, 0, sizeof(r));
                r.kind = BOTHOST_RECORD_CPU;
                r.host = (uint8_t)i;
                r.shownMilli = shown;
                r.source = src;
                r.budgetMilli = budget;
                r.botCpu = in.botCpu;
                r.bots = botHostRecordByte(in.bots);
                r.overrunTenths =
                    botHostOvrAccThinks[i] > 0
                        ? botHostRecordU16((uint32_t)(
                              (double)botHostOvrAccOver[i] * 1000.0 /
                                  (double)botHostOvrAccThinks[i] + 0.5))
                        : (uint16_t)BOTHOST_RECORD_RATE_NONE;
                r.freePct = in.freePct;
                r.slots = botHostRecordByte(in.slots);
                botHostRecPut(&r);
                botHostOvrAccOver[i] = 0;
                botHostOvrAccThinks[i] = 0;
                memset(&r, 0, sizeof(r));
                r.kind = BOTHOST_RECORD_READING;
                r.host = (uint8_t)i;
                r.freePct = rep->sample.freePct;
                r.botCpu = rep->sample.botCpu;
                r.bots = rep->sample.bots;
                r.flags = (uint8_t)(
                    (rep->sample.ran ? BOTHOST_RECORD_READING_RAN : 0u) |
                    (rep->sample.bad ? BOTHOST_RECORD_READING_BAD : 0u));
                botHostRecPut(&r);
            }
        }

        if (act != BOT_HOST_OVR_RAISE && act != BOT_HOST_OVR_REFUSE) continue;
        botHostOvrNumbers(rep, nums, sizeof(nums));
        botHostOvrLimitText(rep->limitBefore, in.slots, before, sizeof(before));
        botHostOvrLimitText(rep->newLimit, in.slots, after, sizeof(after));
        if (act == BOT_HOST_OVR_RAISE) {
            fprintf(stderr, "[bothost] overruns: %s\"%s\" limit goes from %s "
                            "to %s (rate %.2f%% over %d s; %s)\n",
                    botHostOvrKind(i), botHostOvrName(i), before, after,
                    rep->windowPct, rep->probes, nums);
            botHostRecDecision(i, BOTHOST_DECISION_RAISE, rep, -1, -1,
                               rep->why, NULL);
        } else {
            fprintf(stderr, "[bothost] overruns: %s\"%s\" limit stays %s: %s "
                            "(%s)\n",
                    botHostOvrKind(i), botHostOvrName(i), before, rep->why,
                    nums);
            botHostRecDecision(i, BOTHOST_DECISION_RAISE_REFUSED, rep, -1,
                               -1, rep->why, NULL);
        }
    }

    /* What the move knows of each place: its window rate, its bots and
     * whether it may take one more bot now (the free CPU check and its
     * slots, from applied). */
    for (i = 0; i <= MAX_BOT_HOSTS; i++) {
        const BotHostPlaceHost *a = &applied[i];
        memset(&oh[i], 0, sizeof(oh[i]));
        if (i > 0 && !udpServer.botHosts[i - 1].connected) continue;
        oh[i].rateKnown = botHostOverrunRate(&botHostOvr[i], cfg,
                                             &oh[i].ratePct);
        oh[i].bots = bots[i];
        oh[i].mayTake = a->open && a->slots > 0 &&
                        ((a->unlimited && !a->capped) ||
                         a->bots < a->slots);
        oh[i].lagMs = a->lagMs;
        oh[i].slots = a->slots;
    }

    /* The overrun move off each place over the target rate. */
    for (i = 0; i <= MAX_BOT_HOSTS; i++) {
        BotHostOverrunReport *rep = &reps[i];
        BotHostOvrMove mv;
        char from[64], to[64], srcRate[16], dstRate[16], nums[200];
        char before[32], after[32];
        int seat, dest, newLimit;

        if (acts[i] != BOT_HOST_OVR_OVER) continue;
        if (!botHostOvrPickMove(cfg, oh, 1 + MAX_BOT_HOSTS, i, movable[i],
                                movableCount[i], &botHostLag, &mv)) {
            /* Every host about as bad, or no bot may move: at most one
             * line each step time. */
            uint32_t gap = (uint32_t)(cfg->stepSecs > 0 ? cfg->stepSecs : 1) *
                           50u;
            if (botHostOvrNoBetterHave[i] &&
                udpServer.tickCount - botHostOvrNoBetterTick[i] < gap) {
                continue;
            }
            botHostOvrNoBetterHave[i] = true;
            botHostOvrNoBetterTick[i] = udpServer.tickCount;
            fprintf(stderr, "[bothost] overruns: %s\"%s\" rate %.2f%% over "
                            "%d s is over %.2f%%, but %s; no bot moves\n",
                    botHostOvrKind(i), botHostOvrName(i), rep->windowPct,
                    rep->probes, cfg->pct, mv.why);
            botHostRecDecision(i, BOTHOST_DECISION_NO_BETTER, rep, -1, -1,
                               mv.why, &mv);
            continue;
        }
        botHostOvrNoBetterHave[i] = false;
        seat = mv.seat;
        dest = mv.to;
        botHostOvrPlaceText(i, from, sizeof(from));
        botHostOvrPlaceText(dest, to, sizeof(to));
        botHostOvrRateText(true, mv.srcPct, srcRate, sizeof(srcRate));
        botHostOvrRateText(mv.dstKnown, mv.dstPct, dstRate, sizeof(dstRate));

        if (botHostNoMoveOverrunDecision(&botHostNoMove, running) ==
            BOTHOST_DECISION_WOULD_MOVE) {
            /* -bothost-nomove: the bot stays; the decision is kept, and
             * the next one waits a step time. m does not change. */
            botHostOverrunHeld(&botHostOvr[i]);
            fprintf(stderr, "[bothost] overruns: bot %d \"%s\" would move "
                            "from %s to %s: source %s, target %s, m=%d, %s "
                            "(-bothost-nomove)\n",
                    seat, botSeatName(sim, seat), from, to, srcRate, dstRate,
                    mv.moves, botHostOvrRuleName(mv.rule));
            botHostRecDecision(i, BOTHOST_DECISION_WOULD_MOVE, rep, seat,
                               dest, "would move (moves off)", &mv);
            continue;
        }

        newLimit = botHostOverrunMoved(&botHostOvr[i], bots[i]);
        rep->limitBefore = rep->newLimit;
        rep->newLimit = newLimit;
        botHostOvrNumbers(rep, nums, sizeof(nums));
        botHostOvrLimitText(rep->limitBefore, raw[i].slots, before,
                            sizeof(before));
        botHostOvrLimitText(newLimit, raw[i].slots, after, sizeof(after));
        fprintf(stderr, "[bothost] overruns: bot %d \"%s\" moves from %s to "
                        "%s: source %s, target %s, m=%d, %s (severity %.1f, "
                        "wait %.0f s; stay %.0f s, needs %.0f s; limit of %s "
                        "goes from %s to %s for this game; %s)\n",
                seat, botSeatName(sim, seat), from, to, srcRate, dstRate,
                mv.moves, botHostOvrRuleName(mv.rule), rep->severity,
                rep->waitSecs, mv.staySecs, mv.needStaySecs, from, before,
                after, nums);
        botHostRecDecision(i, BOTHOST_DECISION_MOVE, rep, seat, dest,
                           "overruns", &mv);
        botHostRecDecision(i, BOTHOST_DECISION_CUT, rep, -1, -1, "overruns",
                           NULL);
        if (i == 0) {
            (void)botSeatOffer(sim, seat, dest - 1, "overruns on the server");
        } else {
            botSeatFallBack(sim, seat, "overruns on its Bot Host", true);
            udpServer.botSeats[seat].noReturn = false;
            if (dest > 0 && botSeatLocalOfferable(sim, seat)) {
                (void)botSeatOffer(sim, seat, dest - 1,
                                   "overruns on its Bot Host");
            }
        }
        botHostOvrMovesNote(&botHostOvrMv, seat, BOT_HOST_MOVE_VOLUNTARY,
                            udpServer.tickCount);
        botHostTrailNote(&botHostTrail, seat, i, udpServer.tickCount);
        /* The target took a bot: it takes no other in this pass (its free
         * CPU check waits for a reading with that bot). */
        oh[dest].bots++;
        oh[dest].mayTake = false;
        oh[i].bots--;
        moved = true;
    }
    return moved;
}

void transportUdpServerSetBotHostOverrun(const BotHostOverrunFlags *f) {
    BotHostOverrunCfg *cfg = &botHostOvrCfg;
    (void)botHostOvrConfig();
    if (f == NULL) return;
    if (f->pct >= 0.0) cfg->pct = f->pct > 100.0 ? 100.0 : f->pct;
    if (f->emergencyPct >= 0.0) {
        cfg->emergencyPct = f->emergencyPct > 100.0 ? 100.0 : f->emergencyPct;
    }
    if (f->gain >= 0.0) {
        cfg->gain = f->gain < 0.01 ? 0.01 : f->gain > 1.0 ? 1.0 : f->gain;
    }
    if (f->staySecs >= 0) {
        cfg->staySecs = f->staySecs > 3600 ? 3600 : f->staySecs;
    }
    if (f->stayFactor >= 0.0) {
        cfg->stayFactor = f->stayFactor < 1.0 ? 1.0
                        : f->stayFactor > 10.0 ? 10.0 : f->stayFactor;
    }
    if (f->windowSecs >= 0) {
        cfg->windowSecs = f->windowSecs < 1 ? 1
                        : f->windowSecs > BOT_HOST_OVR_MAX_PROBES
                              ? BOT_HOST_OVR_MAX_PROBES : f->windowSecs;
    }
    if (f->stepSecs >= 0) {
        cfg->stepSecs = f->stepSecs < 1 ? 1
                      : f->stepSecs > 3600 ? 3600 : f->stepSecs;
    }
    if (f->stepMinSecs >= 0) {
        cfg->stepMinSecs = f->stepMinSecs < 1 ? 1
                         : f->stepMinSecs > 3600 ? 3600 : f->stepMinSecs;
    }
    if (f->freeBatch >= 0) cfg->freeBatch = f->freeBatch != 0;
    if (f->freeSampleSecs >= 0) {
        cfg->freeSampleSecs = f->freeSampleSecs < 1 ? 1
                            : f->freeSampleSecs > 60 ? 60 : f->freeSampleSecs;
    }
    if (f->freeHistorySecs >= 0) cfg->freeHistorySecs = f->freeHistorySecs;
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
    {
        char wait[96];
        if (botHostOverrunScaled(cfg)) {
            SDL_snprintf(wait, sizeof(wait),
                         "each %d s / (rate / %.2f%%), never below %d s",
                         cfg->stepSecs, cfg->pct, cfg->stepMinSecs);
        } else {
            SDL_snprintf(wait, sizeof(wait), "each %d s", cfg->stepSecs);
        }
        fprintf(stderr, "[bothost] overruns: a host over %.2f%% overruns in "
                        "%d s gives one bot to a better host, at most one "
                        "%s; the target at most %.2f x the source rate for "
                        "each move so far; stay %d s x %.2f per move; at "
                        "%.2f%% the emergency rule; free CPU each %d s, %d s "
                        "kept; low free CPU gives %s\n",
                cfg->pct, cfg->windowSecs, wait, cfg->gain, cfg->staySecs,
                cfg->stayFactor, cfg->emergencyPct, cfg->freeSampleSecs,
                cfg->freeHistorySecs,
                cfg->freeBatch ? "the bots it is short by in one step"
                               : "one bot a step");
    }
}

void transportUdpServerSetBotHostSpreadEven(bool on) {
    botHostSpreadEven = on;
    if (on) {
        fprintf(stderr, "[bothost] placement: the even spread "
                        "(-bothost-spread-even): bots also move between "
                        "good hosts to even out their counts and lag\n");
    }
}

void transportUdpServerSetBotHostEnforceServerLimit(bool on) {
    botHostEnforceServerLimit = on;
    fprintf(stderr, "[bothost] placement: the server's bot limit "
                    "(-bothost-enforce-server-limit %d): %s\n", on ? 1 : 0,
            on ? "the server runs no more bots than its own slots; a bot "
                 "with no slot there or on a good Bot Host is held"
               : "a bot back from a Bot Host runs on the server past its "
                 "slots");
}

void transportUdpServerSetBotHostCode(bool custom) {
    botHostCodeCustom = custom;
    fprintf(stderr, "[bothost] brain code (-bothost-code %s): %s\n",
            custom ? "custom" : "official",
            custom ? "Bot Hosts download this server's brain files; a "
                     "player's built-in host built to run only official "
                     "brains still takes none"
                   : "no brain file goes to a Bot Host; a bot runs on a "
                     "host only when the host's install holds its brain, "
                     "file for file");
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

/* Moves bot s from place from to place to by the placement (0 the server,
 * h + 1 Bot Host h), with its record and console line. toBots: the bots
 * of to before the move, for the record. False when the offer of a server
 * bot was not made. */
static bool botHostPlaceGive(ServerSim *sim, int from, int to, int s,
                             int toBots, const char *reason) {
    /* Not a bot the target host may run (bot_host_guard.h): no move. */
    if (to > 0 && !botSeatHostMayRun(sim, s, to - 1)) return false;
    if (from == 0) {
        if (to <= 0) return false;
        if (!botSeatOffer(sim, s, to - 1, reason)) return false;
        botHostTrailNote(&botHostTrail, s, 0, udpServer.tickCount);
        botHostRecSpread(BOTHOST_DECISION_PLACE, 0, to, s, toBots, reason);
        return true;
    }
    fprintf(stderr, "[bothost] bot %d \"%s\" moves from Bot Host \"%s\" to "
                    "%s%s%s (%s)\n",
            s, botSeatName(sim, s), udpServer.botHosts[from - 1].name,
            to == 0 ? "the server" : "Bot Host \"",
            to == 0 ? "" : udpServer.botHosts[to - 1].name,
            to == 0 ? "" : "\"", reason);
    botHostRecSpread(BOTHOST_DECISION_SPREAD_MOVE, from, to, s, toBots,
                     reason);
    botSeatFallBack(sim, s, reason, true);
    udpServer.botSeats[s].noReturn = false;
    botHostTrailNote(&botHostTrail, s, from, udpServer.tickCount);
    if (to > 0 && botSeatLocalOfferable(sim, s)) {
        (void)botSeatOffer(sim, s, to - 1, reason);
    }
    return true;
}

/* The reason each bot of a free CPU batch records. */
static const char BOT_HOST_BATCH_REASON[] = "low free CPU, batch";

/* The free CPU batch (-bothost-free-batch): bad place from, whose free CPU
 * is short by want bots, gives up to want bots in this one step, each to
 * the place botHostPlaceBadBatch picks (the server first, then the best
 * good host). Each place takes as many as its free slots and its free CPU
 * room (botHostOverrunFreeRoom) let it, then no more of this batch. It
 * stops when no good place is left or no bot may go. Its limit then
 * becomes its bots less the bots given, for this game, and its probes
 * start again. */
static void botHostPlaceGiveBatch(ServerSim *sim,
                                  const BotHostPlaceHost *hosts,
                                  const BotHostPlaceHost *raw, int from,
                                  int want, const int *cand, int candCount) {
    const BotHostOverrunCfg *cfg = botHostOvrConfig();
    BotHostPlaceHost tbl[1 + MAX_BOT_HOSTS];
    BotHostOverrunReport rep;
    int dests[MAX_TANKS], left[MAX_TANKS];
    int room[1 + MAX_BOT_HOSTS], took[1 + MAX_BOT_HOSTS];
    bool used[MAX_TANKS];
    int planned, given = 0, k, j, nLeft, botsBefore, lack, newLimit;
    char place[64], before[32], after[32], nums[200], each[400];
    size_t eachLen = 0;

    if (want > MAX_TANKS) want = MAX_TANKS;
    memcpy(tbl, hosts, sizeof(tbl));
    botsBefore = hosts[from].bots;
    botHostOvrSetFallback(from);
    memset(&rep, 0, sizeof(rep));
    lack = botHostOverrunFreeShort(&botHostOvr[from], cfg, botsBefore,
                                   botHostOvrFree(from), &rep);
    /* The bots each place's free CPU has room for now. */
    for (k = 0; k <= MAX_BOT_HOSTS; k++) {
        took[k] = 0;
        room[k] = 0;
        if (k == from || !hosts[k].open) continue;
        if (k > 0 && !udpServer.botHosts[k - 1].connected) continue;
        botHostOvrSetFallback(k);
        room[k] = botHostOverrunFreeRoom(&botHostOvr[k], cfg, hosts[k].bots,
                                         botHostOvrFree(k), want);
    }
    planned = botHostPlaceBadBatch(tbl, 1 + MAX_BOT_HOSTS, botHostSmallSlots,
                                   botHostSmallBias, &botHostLag, from, want,
                                   room, dests);
    memset(used, 0, sizeof(used));
    for (k = 0; k < planned; k++) {
        int s;
        nLeft = 0;
        for (j = 0; j < candCount; j++) {
            if (!used[j]) left[nLeft++] = cand[j];
        }
        s = botHostTrailPickSeat(&botHostTrail, left, nLeft, dests[k],
                                 udpServer.tickCount, botHostStayTicks(),
                                 false);
        if (s < 0) continue;
        for (j = 0; j < candCount; j++) {
            if (cand[j] == s) used[j] = true;
        }
        if (!botHostPlaceGive(sim, from, dests[k], s,
                              raw[dests[k]].bots + took[dests[k]],
                              BOT_HOST_BATCH_REASON)) {
            continue;
        }
        took[dests[k]]++;
        given++;
    }
    if (given <= 0) return;
    /* How many each place took, in the order of the batch. */
    each[0] = '\0';
    for (k = 0; k < planned; k++) {
        int d = dests[k];
        char name[64];
        if (took[d] <= 0) continue;
        for (j = 0; j < k; j++) {
            if (dests[j] == d) break;
        }
        if (j < k) continue;
        botHostOvrPlaceText(d, name, sizeof(name));
        SDL_snprintf(each + eachLen, sizeof(each) - eachLen, "%s%s took %d",
                     eachLen > 0 ? ", " : "", name, took[d]);
        eachLen = SDL_strlen(each);
    }
    botHostBadGave[from] = true;
    botHostBadGaveTick[from] = udpServer.tickCount;
    rep.limitBefore = botHostOvr[from].limit;
    newLimit = botHostOverrunMovedN(&botHostOvr[from], botsBefore, given);
    rep.newLimit = newLimit;
    botHostRecDecision(from, BOTHOST_DECISION_CUT, &rep, -1, -1,
                       BOT_HOST_BATCH_REASON, NULL);
    botHostOvrPlaceText(from, place, sizeof(place));
    botHostOvrLimitText(rep.limitBefore, raw[from].slots, before,
                        sizeof(before));
    botHostOvrLimitText(newLimit, raw[from].slots, after, sizeof(after));
    botHostOvrNumbers(&rep, nums, sizeof(nums));
    fprintf(stderr, "[bothost] bad place: %s free CPU is short by %d bots "
                    "(%s): gave %d of %d bots in one step (%s); limit goes "
                    "from %s to %s for this game; wait %d s before it gives "
                    "another\n",
            place, lack, nums, given, want, each, before, after,
            cfg->stepSecs);
}

/* A hold or a resume (BOTHOST_DECISION_HOLD or _RESUME) for the round log.
 * host: where the bot was (a hold) or where it goes (a resume), as
 * botHostRecDecision counts it; toHost: where it goes, -1 for a hold. The
 * record carries the free CPU check of the server for a hold (the place
 * with no slot) and of toHost for a resume. */
static void botHostRecHeld(uint8_t decision, int host, int toHost, int seat,
                           int bots, const char *reason) {
    const BotHostOverrunCfg *cfg = botHostOvrConfig();
    BotHostOverrunReport rep;
    int check = toHost >= 0 ? toHost : 0;
    if (!botHostRecOn() || check > MAX_BOT_HOSTS) return;
    botHostOvrEnsure();
    botHostOvrSetFallback(check);
    memset(&rep, 0, sizeof(rep));
    (void)botHostOverrunMayTake(&botHostOvr[check], cfg, bots,
                                botHostOvrFree(check), &rep);
    botHostRecDecision(host < 0 ? 0 : host, decision, &rep, seat, toHost,
                       reason, NULL);
}

/* The players and the spectators are told, and the round log gets the
 * line as a server message, made from the same string id. */
static void botHostHeldTell(ServerSim *sim, int s, langid id) {
    const char *args[1];
    MessageArgs fmt;
    const char *text;
    char pstr[256];
    size_t len;
    args[0] = botSeatName(sim, s);
    serverSendServerMessage(sim, id, 1, args);
    if (!logIsRecording()) return;
    memset(&fmt, 0, sizeof(fmt));
    SDL_strlcpy(fmt.playerName, botSeatName(sim, s), sizeof(fmt.playerName));
    text = langGetTextFmt(id, &fmt);
    if (text == NULL) return;
    len = strlen(text);
    if (len > 255) len = 255;
    pstr[0] = (char)(unsigned char)len;
    memcpy(pstr + 1, text, len);
    logAddEvent(log_MessageServer, 0, 0, 0, 0, 0, pstr);
}

/* Holds bot s: no place has a slot for it. from is the place it ran on
 * (0 the server, h + 1 Bot Host h). */
static void botHostHoldSeat(ServerSim *sim, int s, int from,
                            const char *why) {
    if (!botManagerHoldNoRoom(sim, (BYTE)s)) return;
    botHostHeldSince[s] = udpServer.tickCount;
    botHostHeldTold[s] = false;
    fprintf(stderr, "[bothost] bot %d \"%s\" held: %s; its brain does not "
                    "run and its tank stands still until a place has a "
                    "slot (game tick %u)\n",
            s, botSeatName(sim, s), why, (unsigned)serverSimGetTick(sim));
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] bot %d held (%s)", s, why);
    botHostRecHeld(BOTHOST_DECISION_HOLD, from, -1, s,
                   serverBotHostServerBots(sim), "no room");
}

/* A held bot runs again (on the server, or a Bot Host took it). */
static void botHostHeldRunsAgain(ServerSim *sim, int s) {
    if (botHostHeldTold[s]) {
        botHostHeldTell(sim, s, STR_BOTHOST_BOT_RUNS_AGAIN);
    }
    botHostHeldTold[s] = false;
}

/* -bothost-enforce-server-limit, each placement step: a server over its
 * own slots with no good Bot Host to take a bot holds one; else the bot
 * held longest runs again when a place has a slot (the server first, then
 * the good Bot Hosts), one bot a step. raw: the slots as the places offer
 * them; hosts: the table the spread uses (the lowered limits, the bad
 * places). True when it held or placed a bot. */
static bool botHostHeldStep(ServerSim *sim, const BotHostPlaceHost *raw,
                            const BotHostPlaceHost *hosts, bool running,
                            bool anyPending) {
    bool held[MAX_TANKS], any = false;
    int s, tgt, n = 1 + MAX_BOT_HOSTS;

    for (s = 0; s < MAX_TANKS; s++) {
        held[s] = udpServer.botSeats[s].state == BOT_SEAT_LOCAL &&
                  botManagerIsHeldNoRoom(sim, (BYTE)s);
        if (held[s]) {
            /* A held seat did not come back from a host by a decision: it
             * may go to one at once. */
            udpServer.botSeats[s].noReturn = false;
            any = true;
        }
    }

    tgt = botHostPlaceHeldTarget(hosts, n, botHostSmallSlots,
                                 botHostSmallBias, &botHostLag);
    if (botHostDsChildLinked()) {
        /* The server's own child: nothing is held, and a bot held before
         * the child linked runs again in the server's process. */
        tgt = 0;
    } else if (raw[0].bots > raw[0].slots) {
        /* Over its slots (they went down, or a bot was added). While a good
         * Bot Host can take a bot, or an offer waits for its answer, the
         * spread moves one there. Else the last bot that may move is held,
         * else the last one. */
        int pick = -1;
        if (tgt >= 1 || anyPending) return false;
        for (s = MAX_TANKS - 1; s >= 0; s--) {
            if (udpServer.botSeats[s].state != BOT_SEAT_LOCAL) continue;
            if (!botManagerIsBot(sim, (BYTE)s)) continue;
            if (botManagerIsRemote(sim, (BYTE)s) || held[s]) continue;
            if (udpServer.clients[s].connected) continue;
            if (botSeatMayMove(sim, s, running)) {
                pick = s;
                break;
            }
            if (pick < 0) pick = s;
        }
        if (pick >= 0) {
            char why[96];
            snprintf(why, sizeof(why), "the server runs %d bots for its %d "
                     "slots and no Bot Host has a slot",
                     raw[0].bots, raw[0].slots);
            botHostHoldSeat(sim, pick, 0, why);
            return true;
        }
        return false;
    }
    if (!any) return false;

    if (tgt >= 1 && anyPending) {
        /* One offer at a time: the host's answer first. */
        return false;
    }
    if (tgt >= 0) {
        /* A Bot Host runs only brains whose files it was sent. */
        for (s = 0; s < MAX_TANKS; s++) {
            if (held[s] && tgt >= 1 &&
                (!botSeatLocalOfferable(sim, s) ||
                 !botSeatHostMayRun(sim, s, tgt - 1))) {
                held[s] = false;
            }
        }
        s = botHostHeldOldest(held, botHostHeldSince, MAX_TANKS,
                              udpServer.tickCount);
        if (s >= 0) {
            uint32_t secs = (udpServer.tickCount - botHostHeldSince[s]) / 50u;
            if (tgt == 0) {
                fprintf(stderr, "[bothost] bot %d \"%s\" resumed on the "
                                "server: it has a slot (%d/%d slots used, "
                                "held %u s)\n",
                        s, botSeatName(sim, s), hosts[0].bots,
                        hosts[0].slots, (unsigned)secs);
                botHostRecHeld(BOTHOST_DECISION_RESUME, 0, 0, s,
                               hosts[0].bots, "room on the server");
                if (botManagerResumeHeld(sim, (BYTE)s)) {
                    botHostHeldRunsAgain(sim, s);
                } else {
                    fprintf(stderr, "[bothost] bot %d held: its brain could "
                                    "not be built in-process\n", s);
                }
                return true;
            }
            fprintf(stderr, "[bothost] bot %d \"%s\" resumed: Bot Host "
                            "\"%s\" has a slot (held %u s)\n",
                    s, botSeatName(sim, s), udpServer.botHosts[tgt - 1].name,
                    (unsigned)secs);
            botHostRecHeld(BOTHOST_DECISION_RESUME, tgt, tgt, s,
                           hosts[tgt].bots, "room on a Bot Host");
            if (botSeatOffer(sim, s, tgt - 1, "a held bot")) return true;
        }
    }
    /* No place has a slot: the players are told once. */
    for (s = 0; s < MAX_TANKS; s++) {
        if (udpServer.botSeats[s].state != BOT_SEAT_LOCAL) continue;
        if (!botManagerIsHeldNoRoom(sim, (BYTE)s) || botHostHeldTold[s]) {
            continue;
        }
        botHostHeldTold[s] = true;
        fprintf(stderr, "[bothost] bot %d \"%s\" is paused: no room to "
                        "run its bot\n", s, botSeatName(sim, s));
        botHostHeldTell(sim, s, STR_BOTHOST_BOT_PAUSED);
    }
    return false;
}

/* One placement move: index 0 of the host table is the server itself, index
 * h + 1 is Bot Host h. */
static void botHostPlaceStep(ServerSim *sim, ServerState st) {
    BotHostPlaceHost hosts[1 + MAX_BOT_HOSTS], raw[1 + MAX_BOT_HOSTS];
    /* The bots that may move off each place now, in seat order: the
     * server's own first, then each host's. */
    int localSeats[MAX_TANKS], localCount = 0;
    int hostSeats[MAX_BOT_HOSTS][MAX_TANKS], hostCount[MAX_BOT_HOSTS];
    const char *badWhy[1 + MAX_BOT_HOSTS];
    int badShort[1 + MAX_BOT_HOSTS];
    double badRate[1 + MAX_BOT_HOSTS];
    const int *cand;
    int candCount;
    int candTo[MAX_TANKS], candToCount = 0;
    BotHostSpreadWhy why = BOT_HOST_SPREAD_NONE;
    const char *reason;
    bool running = (st == serverStateRunning);
    bool mayMove;
    bool anyPending = false, anyHost = false;
    int s, h, from = -1, to = -1;

    botHostTrailEnsure();
    for (h = 0; h <= MAX_BOT_HOSTS; h++) {
        badWhy[h] = NULL;
        badShort[h] = 0;
        badRate[h] = -1.0;
    }

    if (running && botHostPlace.lastSt != serverStateRunning) {
        botHostPlace.runStartTick = udpServer.tickCount;
    }
    botHostPlace.lastSt = st;

    memset(hosts, 0, sizeof(hosts));
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        hostCount[h] = 0;
        if (!hc->connected) continue;
        /* Lowered by the row's override; 0 for a host on a computer
         * another counts. */
        hosts[h + 1].slots = (uint8_t)botHostEffSlots(h);
        hosts[h + 1].bots = botHostUsedSlots(h);
        hosts[h + 1].open = botHostTakesBots(sim, h);
        botHostUpdateLag(h);
        hosts[h + 1].lagMs = hc->lagKnown ? hc->lagMs : -1.0;
        if (hosts[h + 1].open) anyHost = true;
    }

    hosts[0].slots = botHostServerSlots(sim);
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        /* The server's own child takes the server's slots: the server's
         * process keeps only what the child does not take. */
        if (udpServer.botHosts[h].connected &&
            udpServer.botHosts[h].localChild && hosts[h + 1].open) {
            int left = (int)hosts[0].slots - (int)hosts[h + 1].slots;
            hosts[0].slots = left > 0 ? left : 0;
        }
    }
    hosts[0].bots = serverBotHostServerBots(sim);
    hosts[0].open = true;
    hosts[0].unlimited = true;
    /* -bothost-enforce-server-limit: no bot past its slots after all. Not
     * with the server's own child: the child has the server's slots, and a
     * bot the child cannot take now runs in the server's process (the DS
     * rule), never held with its tank standing still. */
    hosts[0].capped = botHostEnforceServerLimit && !botHostDsChildLinked();
    hosts[0].lagMs = 0.0;  /* the server's own process */
    for (s = 0; s < MAX_TANKS; s++) {
        BotHostSeat *seat = &udpServer.botSeats[s];
        botHostNoMoveSeat(&botHostNoMove, s, serverSimIsBot(sim, (BYTE)s),
                          seat->state == BOT_SEAT_BOUND ||
                              seat->state == BOT_SEAT_REMOTE,
                          udpServer.tickCount);
        botHostOvrMovesSeat(&botHostOvrMv, s, serverSimIsBot(sim, (BYTE)s),
                            udpServer.tickCount);
        if (!serverSimIsBot(sim, (BYTE)s)) botHostTrailClear(&botHostTrail, s);
        if (seat->state == BOT_SEAT_PENDING) anyPending = true;
        if (seat->state == BOT_SEAT_LOCAL) {
            /* The spread looks only at a bot that may still be placed
             * (-bothost-nomove). A held bot runs nowhere: the held step
             * finds it a place. */
            if (botSeatLocalOfferable(sim, s) &&
                !botManagerIsHeldNoRoom(sim, (BYTE)s) &&
                botSeatMayMove(sim, s, running) &&
                botHostNoMoveMayPlace(&botHostNoMove, s, running,
                                      udpServer.tickCount,
                                      BOT_HOST_START_PLACE_TICKS)) {
                localSeats[localCount++] = s;
            }
        } else if (seat->state == BOT_SEAT_REMOTE && seat->host >= 0 &&
                   seat->host < MAX_BOT_HOSTS &&
                   botSeatMayMove(sim, s, running)) {
            hostSeats[seat->host][hostCount[seat->host]++] = s;
        }
    }
    /* A host that may run none of the bots (bot_host_guard.h: its install
     * lacks their brains, or it is a player's host and they see the whole
     * map or carry tokens off the list) takes none. */
    {
        BotSeatFitInfo fitInfo[MAX_TANKS];
        bool fitHave[MAX_TANKS];
        for (s = 0; s < MAX_TANKS; s++) {
            fitHave[s] = serverSimIsBot(sim, (BYTE)s) &&
                         !udpServer.clients[s].connected;
            if (fitHave[s]) botSeatFitInfo(sim, s, &fitInfo[s]);
        }
        anyHost = false;
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            bool fits = false;
            if (!hosts[h + 1].open) continue;
            for (s = 0; s < MAX_TANKS && !fits; s++) {
                fits = fitHave[s] && botSeatFitsHost(&fitInfo[s], h);
            }
            hosts[h + 1].open = fits;
            if (fits) anyHost = true;
        }
    }
    mayMove = botHostNoMoveMayMove(&botHostNoMove, running);
    hosts[0].movable = (localCount > 0);
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        /* A host's bots stay put while any offer waits for its answer, and
         * all game long with -bothost-nomove. */
        hosts[h + 1].movable = (hostCount[h] > 0) && !anyPending && mayMove;
    }

    /* The overrun check: a probe each second, then the lowered limits and
     * the free CPU check on the table the spread uses. Only while the game
     * runs: in the lobby no bot thinks, so there is no reading to judge, and
     * the lobby fills each host up to its slots (the CPU test, the
     * override, the same-computer groups and the opt-out already set
     * those). */
    memcpy(raw, hosts, sizeof(raw));
    if (running) botHostOvrApply(hosts, true);
    if (running && (!botHostPlace.probed ||
                    udpServer.tickCount - botHostPlace.lastProbeTick >=
                        BOT_HOST_OVR_PROBE_TICKS)) {
        botHostPlace.probed = true;
        botHostPlace.lastProbeTick = udpServer.tickCount;
        if (botHostOvrConfig()->pct > 0.0 &&
            botHostOvrProbeAll(sim, raw, hosts, running)) {
            botHostPlace.lastMoveTick = udpServer.tickCount;
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
            if (hostCount[h] <= 0 || anyPending) continue;
            if (!botHostNoMoveGiveBack(&botHostNoMove, running,
                                       raw[h + 1].bots, raw[h + 1].slots)) {
                continue;
            }
            if (udpServer.tickCount - botHostPlace.lastMoveTick <
                BOT_HOST_MOVE_TICKS) {
                break;
            }
            s = hostSeats[h][0];
            fprintf(stderr, "[bothost] bot %d \"%s\" goes back to the server: "
                            "Bot Host \"%s\" offers %d slots for %d bots\n",
                    s, botSeatName(sim, s), udpServer.botHosts[h].name,
                    raw[h + 1].slots, raw[h + 1].bots);
            botSeatFallBack(sim, s, "its host offers fewer slots", true);
            botHostPlace.lastMoveTick = udpServer.tickCount;
            return;
        }
    }

    /* While the game runs, which places run their bots badly. In the lobby
     * no bot thinks, so every place counts as good there. */
    if (running) botHostPlaceMarkBad(hosts, badWhy, badShort, badRate);
    if (botHostEnforceServerLimit &&
        botHostHeldStep(sim, raw, hosts, running, anyPending)) {
        return;
    }
    if (!anyHost) return;
    if (!botHostPlaceRebalanceKeep(hosts, 1 + MAX_BOT_HOSTS,
                                   botHostSmallSlots, botHostSmallBias,
                                   &botHostLag, botHostSpreadEven, &from,
                                   &to, &why)) {
        return;
    }
    if (running) {
        bool early = udpServer.tickCount - botHostPlace.runStartTick <
                     BOT_HOST_START_PLACE_TICKS;
        bool spaced = udpServer.tickCount - botHostPlace.lastMoveTick >=
                      BOT_HOST_MOVE_TICKS;
        if (!spaced && !(early && from == 0)) return;
    }
    /* Which bot: the first that did not leave the target place within the
     * stay time. A move off a place over its slots is forced: with no
     * other bot, that one goes. */
    if (from == 0) {
        cand = localSeats;
        candCount = localCount;
    } else {
        cand = hostSeats[from - 1];
        candCount = hostCount[from - 1];
    }
    if (why == BOT_HOST_SPREAD_BAD && badShort[from] > 0) {
        botHostPlaceGiveBatch(sim, hosts, raw, from, badShort[from], cand,
                              candCount);
        botHostPlace.lastMoveTick = udpServer.tickCount;
        return;
    }
    if (to > 0) {
        /* Only the bots that may run on the target host. */
        int k;
        candToCount = 0;
        for (k = 0; k < candCount; k++) {
            if (botSeatHostMayRun(sim, cand[k], to - 1)) {
                candTo[candToCount++] = cand[k];
            }
        }
        cand = candTo;
        candCount = candToCount;
    }
    s = botHostTrailPickSeat(&botHostTrail, cand, candCount, to,
                             udpServer.tickCount, botHostStayTicks(),
                             why == BOT_HOST_SPREAD_OVER_SLOTS);
    if (s < 0) return;
    reason = botHostSpreadReason(why, from, badWhy);
    if (!botHostPlaceGive(sim, from, to, s, raw[to].bots, reason)) return;
    botHostPlace.lastMoveTick = udpServer.tickCount;
    if (why != BOT_HOST_SPREAD_BAD) return;
    botHostBadGave[from] = true;
    botHostBadGaveTick[from] = udpServer.tickCount;
    {
        const BotHostOverrunCfg *cfg = botHostOvrConfig();
        char place[64];
        botHostOvrPlaceText(from, place, sizeof(place));
        if (badRate[from] >= cfg->pct) {
            /* Its next rate is measured from the thinks since this bot
             * left, so a short wait judges on a short window. */
            if (botHostOverrunScaled(cfg)) {
                botHostOverrunGave(&botHostOvr[from]);
            }
            fprintf(stderr, "[bothost] bad place: %s gave bot %d: rate "
                            "%.2f%% is severity %.1f (target %.2f%%); wait "
                            "%.0f s before it gives another\n",
                    place, s, badRate[from], badRate[from] / cfg->pct,
                    cfg->pct, botHostOverrunWaitSecs(cfg, badRate[from]));
        } else {
            fprintf(stderr, "[bothost] bad place: %s gave bot %d: %s; wait "
                            "%d s before it gives another\n",
                    place, s, reason, cfg->stepSecs);
        }
    }
}

void serverServiceBotHosts(ServerSim *sim) {
    int h, s;
    ServerState st;

    if (sim == NULL) return;
    st = serverSimGetState(sim);
    if (udpServer.botHostEnabled && !botHostRuleShown) {
        botHostRuleShown = true;
        botHostPrintRule();
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
    botHostMachineUpdate();
    botHostTellSameServer();
    botHostUpdateGameBotLimit(sim);

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
            /* Back in the lobby: the game a late host sat out is over, and
             * a slot override from the last lobby goes (one from
             * -bothost-slots stays). */
            for (h = 0; h < MAX_BOT_HOSTS; h++) {
                udpServer.botHosts[h].skipGame = false;
                if (udpServer.botHostLastState != -1) {
                    botHostSlotOverrideNewLobby(&udpServer.botHosts[h].slotOvr);
                }
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
        if ((st == serverStateLobby || st == serverStateRunning) &&
            udpServer.botHostEnabled) {
            /* A new salt for each lobby and each game. */
            botHostMachineRound(st == serverStateLobby ? "lobby" : "game");
        }
        if (st == serverStateLobby || st == serverStateRunning) {
            for (s = 0; s < MAX_TANKS; s++) {
                udpServer.botSeats[s].noReturn = false;
            }
            /* The overrun limits are for one game only. */
            botHostOvrNewGame();
            /* Each bot's voluntary moves start at 0 again when the game
             * ends. */
            if (st == serverStateLobby) {
                botHostOvrMovesReset(&botHostOvrMv);
                botHostTrailReset(&botHostTrail);
                botHostTrailReady = true;
                memset(botHostBadGave, 0, sizeof(botHostBadGave));
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
        if (!botHostSynced(seat->host)) {
            /* The brain files changed and the host does not hold the new
             * ones yet: the seat runs in-process until it does, and then
             * goes back to a host. */
            botSeatFallBack(sim, s, "brain files changed", true);
            udpServer.botSeats[s].noReturn = false;
            continue;
        }
        if (seat->state == BOT_SEAT_PENDING) {
            /* The server's own child has no pending timeout: it loads its
             * brains one by one at low priority, and the link timeout
             * ("gone") covers a child that hangs. */
            if (!udpServer.botHosts[seat->host].localChild &&
                botHostPendingExpired(udpServer.tickCount, seat->sinceTick,
                                      SDL_GetTicks(), seat->sinceMs)) {
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
        if (seat->reloadOwed ||
            udpServer.tickCount - botSeatFitTick[s] >= BOT_SEAT_POLL_TICKS) {
            /* The bot's brain, AI level or tokens changed so that its host
             * may not run it (bot_host_guard.h): it comes back. */
            botSeatFitTick[s] = udpServer.tickCount;
            if (!botSeatHostMayRun(sim, s, seat->host)) {
                botSeatFallBack(sim, s, "its host may not run this bot now",
                                true);
                udpServer.botSeats[s].noReturn = false;
                continue;
            }
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
        if (!botHostSynced(h) && !hc->syncLate && !hc->sameServerSent &&
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
        fprintf(stdout, "Bot Host \"%s\"%s: %d/%d slots used (Ping:%ums Addr:%s)\n",
                hc->name, hc->playerHost ? " (a player's own)" : "",
                botHostUsedSlots(h), botHostEffSlots(h),
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
