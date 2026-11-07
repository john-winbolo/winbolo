/*
 * WinBoloBotHost - runs WinBoloDS servers' bot brains on another machine.
 *
 * The host is given a list of servers (--servers) and listens on a UDP port
 * of its own (--listen). It tells each server it is open to take bots (a
 * PACKET_BOT_HOST_ANNOUNCE) at start, every BOT_HOST_ANNOUNCE_MS while it
 * has free slots, and each time its free slots change. A server that wants
 * bots run answers with an INVITE; a server may also ask first (a QUERY,
 * which the host answers with an ANNOUNCE). On an INVITE the host opens a
 * tankless control connection to that server. The connection always starts
 * from the host's side, so it works from behind a NAT.
 *
 * Over a control connection the server hands the host bot seats
 * (CTRL_BOT_HOST_ASSIGN with a one-time token). For each seat the host opens
 * a bot connection - a normal UDP client connection that joins with
 * JOIN_FLAG_BOT_SLOT and the token - and runs the seat's brain against that
 * connection's ClientSim, through the same think path the server's
 * in-process bot pool uses (botRunner*).
 *
 * Several servers at once: one control connection per server; the slots
 * (--slots) are one pool for all of them. A slot is taken when a server's
 * assign arrives and freed when the server takes the seat back, the round
 * ends without it, or the connection drops. Each server is told the slots
 * it may use (CTRL_BOT_HOST_SLOTS: its own seats plus the pool's free ones);
 * an assign that finds the pool full is turned down, and that seat stays
 * in-process on its server. Seats of every server think in the same worker
 * pool with the same per-slot budget, and the brain cache is shared: a brain
 * version is kept once, whichever server sent it.
 *
 * Players never see the host or its connections: the server binds each bot
 * connection to the existing bot seat without a roster change.
 *
 * Usage:
 *   WinBoloBotHost --servers ADDR[:PORT],... [--listen PORT] [--key KEY]
 *                  [--slots N] [--threads N] [--name NAME] [--cachedir DIR]
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <SDL3/SDL.h>
#ifndef WINBOLO_BOTHOST_EMBED
#include <SDL3/SDL_main.h>
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bolo_rand.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_command.h"
#include "client_connect_state.h"
#include "control_event.h"
#include "brain_list.h"
#include "gui_message.h"
#include "platform_net.h"
#include "server_sim.h"
#include "scenario_table.h"
#include "tank.h"
#include "messages.h"
#include "braincore.h"
#include "bot_manager.h"
#include "bot_worker_pool.h"
#include "channel_mux.h"
#include "bulk_transfer.h"
#include "brain_sync.h"
#include "bot_host_link.h"
#include "machine_cpu_free.h"
#include "client_sim_internal.h"
#include "transport_control_codec.h"
#include "machine_id.h"   /* machineIdInit, machineIdAnswerSalt */
#include "transport_udp.h"
#include "transport_udp_internal.h"
#include "../gui/lang.h"
#include "bothost_rec.h"
#include "brain_record_host.h"   /* brainRecMakeAbsolute */
#include "../gui/clientmutex.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../common/sentry_integration.h"
#include "../common/wb_log.h"
#include "../common/prefs.h"
#include "brain_capacity.h"
#include "bot_host_cpu.h"   /* BotHostCpuMeter */
#ifdef WINBOLO_BOTHOST_EMBED
#include "bothost_embed.h"
#endif

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

#define BOT_HOST_TICK_MS           20      /* one think per 20 ms */
#define BOT_HOST_PING_MS           500     /* control connection keepalive */
#define BOT_HOST_JOIN_RETRY_MS     1000    /* join resend while joining */
#define BOT_HOST_JOIN_GIVE_UP_MS   10000   /* no answer to a join this long */
#define BOT_HOST_SILENCE_MS        5000    /* no packet this long: dropped */
#define BOT_HOST_REANNOUNCE_WAIT_MS 5000   /* wait before announcing after a drop */
#define BOT_HOST_ANNOUNCE_MS       30000   /* announce again while slots are free */
#define BOT_HOST_STATS_TICKS       50      /* CMD_BOT_STATS about once a second */
#define BOT_HOST_CONNECT_MS        30000   /* a bot connection must join in this */
#define BOT_HOST_MAX_SERVERS       BOT_HOST_LINK_MAX_ADDRS

static uint16_t optListenPort  = BOT_HOST_DEFAULT_LISTEN_PORT;
static int      optSlots       = 4;
static int      optThreads     = 0;
static char     optKey[JOIN_BOT_HOST_KEY_LEN + 1];
static char     optName[JOIN_BOT_HOST_NAME_LEN];
/* The brain cache: the servers' brain files, one folder per version. */
static char     optCacheDir[512];
static BotHostAddrList optServers;

static volatile sig_atomic_t hostQuit = 0;

/* The Bot Host inside a WinBolo client (bothost_embed.c builds this file
 * with WINBOLO_BOTHOST_EMBED). It serves the one server the player is on,
 * listens on no port and joins with the player's ticket, not a key. */
#ifdef WINBOLO_BOTHOST_EMBED
#define HOST_EMBED 1
#else
#define HOST_EMBED 0
#endif
static uint8_t  embedPlayer;
static uint8_t  embedTicket[BOT_HOST_TICKET_LEN];
static char     embedBrain[BOT_HOST_BRAIN_NAME_LEN];
/* A fixed slot count (a test option); 0: the brain's capacity answer. */
static int      embedSlotsFixed;
/* The capacity answer was read for this manifest generation. */
static uint32_t embedCapacityGen;
/* The server refused the host: it does not try again. */
static bool     embedRefused;
/* The player turned off "run bots on this computer" for this game: the
 * host offers the server 0 slots and takes no new seat, but stays
 * connected, so it can offer its slots again. Set by the client's main
 * thread (botHostEmbedSetWithhold), read by the host's thread. */
static SDL_AtomicInt embedWithhold;

/* The host offers no slots now (the Bot Host inside a client only). */
static bool hostWithheld(void) {
    return HOST_EMBED && SDL_GetAtomicInt(&embedWithhold) != 0;
}

/* One line on stderr, and in the log file for the Bot Host inside a
 * client, which has no console of its own. */
#define HOST_EVENT(...)                                                    \
    do {                                                                   \
        fprintf(stderr, __VA_ARGS__);                                      \
        fputc('\n', stderr);                                               \
        if (HOST_EMBED) WB_LOG_INFO(WB_LOG_CAT_NET, __VA_ARGS__);          \
    } while (0)

#ifndef WINBOLO_BOTHOST_EMBED
static void signalHandler(int sig) {
    (void)sig;
    hostQuit = 1;
}

static void hostMessageHandler(const char *message, const char *title) {
    fprintf(stderr, "[bothost] [%s] %s\n", title ? title : "WinBolo",
            message ? message : "");
}
#endif

static uint32_t nowMs(void) {
    return (uint32_t)SDL_GetTicks();
}

/* ------------------------------------------------------------------ */
/* Seats                                                               */
/* ------------------------------------------------------------------ */

/* One bot seat a server handed this host. A seat belongs to one server
 * (srv) and is that server's seat number seat; two servers may each have a
 * seat 3. */
typedef struct {
    bool        used;
    int         srv;
    uint8_t     seat;
    uint16_t    gen;
    uint8_t     token[JOIN_BOT_SLOT_TOKEN_LEN];
    char        name[BOT_HOST_EVT_NAME_LEN];
    char        brainName[BOT_HOST_BRAIN_NAME_LEN];
    char        initArg[BOT_HOST_INIT_ARG_LEN];
    uint8_t     ai;
    uint8_t     team;
    uint32_t    startTick;
    uint32_t    brainVersion;

    /* The init table arriving after an assign, or a live game.bot_init
     * update. awaitTable is set by an assign until its table is complete. */
    ScnTable    tableIn;
    bool        awaitTable;
    /* A scenario hint arriving entry by entry. */
    ScnTable    hintIn;

    /* A brain is owed: built once the table is complete and the bot
     * connection is in the game. */
    bool        wantBuild;
    bool        buildFailed;

    ClientSim  *cs;
    uint32_t    connectStartMs;
    bool        lobbyNoted;
    BotContext  bot;
    BotJobCtx   job;
    ClientSimBrainRelay relay;

    /* The gen of the assign that placed the seat here: its recording file
     * is named after it, and the server's marker for the bot carries it. */
    uint16_t    placeGen;

    /* The bot connection's own input tick counter: two half-steps a think. */
    uint32_t    inputTick;
    /* The packet tick minus inputTick of the last input sent. A seat placed
     * mid-round has its packet ticks moved past the server's
     * lastProcessedInput (clientBuildInputPacket), and the server's hosted
     * marker carries that number, so the recording uses it too. */
    uint32_t    recSeqOffset;
    uint32_t    statsTick;
    double      thinkMsSum;
    uint32_t    killCount;
    uint32_t    teamMsgsOut;   /* CMD_BOT_TEAM_MSG sent */
    uint32_t    teamMsgsIn;    /* CTRL_BOT_TEAM_MSG received */

    /* Pings asked for during the think, sent after it (CMD_PING). */
    ClientCommand pings[BOT_PENDING_CMD_MAX];
    int           pingCount;

    /* The pool capture, made on the seat's worker right after its think.
     * The main thread sets the plan before the dispatch (poolExpr NULL:
     * no capture this tick); the worker sets poolDoneExpr and poolBuf;
     * the main thread reads them only after botWorkerPoolRun returns and
     * hands poolBuf to hostRecSeatThink the same tick. */
    const char   *poolExpr;
    uint8_t       poolMode;
    int           poolRrIndex;
    int           poolRrCount;
    const char   *poolDoneExpr;
    char         *poolBuf;
} HostSeat;

/* The pool: one entry per slot, whichever server holds it. */
static HostSeat seats[BOT_HOST_MAX_SLOTS];

/* ------------------------------------------------------------------ */
/* Servers                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CTL_IDLE,        /* announcing; waiting for an INVITE */
    CTL_JOINING,
    CTL_CONNECTED
} CtlState;

/* Brain sync with one server. The server sends the list of its brain files
 * (the manifest) when the host connects and each time the files change. The
 * host keeps each brain version in its own folder of the cache,
 * <cache>/<brain>/<version>/, asks for each file it does not hold, and
 * reports its progress. The server gives it bots only once it reports that
 * it holds every file. */
typedef struct {
    BulkReceiver      bulk;
    bool              haveManifest;
    uint32_t          gen;         /* the manifest's generation */
    BrainSyncManifest man;
    uint32_t         *need;        /* (brain << 16) | file, to download */
    int               needCount;
    int               nextReq;     /* need[] entries asked for so far */
    int               gotCount;    /* need[] entries written */
    uint32_t          done;        /* raw bytes written */
    uint32_t          total;       /* raw bytes to write */
    bool              ready;
    /* The file being received. */
    bool              fileActive;
    uint32_t          fileId;
    uint32_t          fileGot;
    uint8_t          *fileBuf;
    uint32_t          syncStartMs;
    int               retries;     /* files asked for again this manifest */
} HostSync;

typedef struct {
    char       host[BOT_HOST_LINK_HOST_LEN];
    uint16_t   port;
    char       label[BOT_HOST_LINK_HOST_LEN + 8];   /* "host:port" */
    struct sockaddr_in addr;
    bool       resolved;
    bool       resolveLogged;
    /* The server refused the key: it is not tried again. */
    bool       refused;

    /* The control connection. */
    SOCKET     sock;
    CtlState   state;
    uint8_t    cookie[JOIN_COOKIE_LEN];
    bool       haveCookie;
    uint32_t   outSeq;
    uint32_t   lastRecvMs;
    uint32_t   lastJoinMs;
    uint32_t   joinStartMs;
    uint32_t   lastPingMs;
    uint32_t   serverNow;      /* last PONG's server time, echoed back */
    uint16_t   rttMs;
    uint32_t   muxTick;
    ChannelMux mux;
    /* The slot count last sent in CTRL_BOT_HOST_SLOTS, -1 for none. */
    int        lastSlotsSent;
    /* The CPU byte last sent in CTRL_BOT_HOST_CPU, -1 for none. */
    int        lastCpuSent;
    /* The per-bot value last sent in CTRL_BOT_HOST_CPU_BOT; 0 for none, so
     * the first one (unknown too) goes out. */
    uint32_t   lastCpuBotSent;
    /* The free CPU reading last sent in CTRL_BOT_HOST_CPU_FREE (its
     * hostFreeSeq), 0 for none. */
    uint32_t   lastFreeSeqSent;

    HostSync   sync;
    /* Server-bound bulk: the brain recordings this host sends. */
    BulkSender recBulk;

    /* Announcing while idle. */
    bool       announceDue;
    uint32_t   announceAtMs;
    int        lastAnnouncedFree;
} HostServer;

static HostServer servers[BOT_HOST_MAX_SERVERS];
static int        serverCount;

/* This host's estimate of the machine's CPU its bots use (bot_host_cpu.h):
 * the think time of every brain think, sampled each think loop. Only the
 * percent goes to the servers. */
static double          hostThinkMsTotal;
static BotHostCpuMeter hostCpuMeter;
static uint8_t         hostCpuValue = (uint8_t)BOT_HOST_CPU_UNKNOWN;
/* The free CPU of this whole machine, read each BOT_HOST_FREE_MS
 * (machine_cpu_free.h). Only the percent goes to the servers. hostFreeSeq
 * counts the readings, so each one is sent once. */
#define BOT_HOST_FREE_MS 5000u
static MachineCpuFree  hostFreeMeter;
static uint8_t         hostFreeValue = (uint8_t)MACHINE_CPU_FREE_UNKNOWN;
static uint32_t        hostFreeSeq;
static uint32_t        hostFreeAtMs;
static SOCKET     listenSock = INVALID_SOCKET;

static int serverIndex(const HostServer *sv) {
    return (int)(sv - servers);
}

/* ------------------------------------------------------------------ */
/* The slot pool                                                       */
/* ------------------------------------------------------------------ */

static int poolUsed(void) {
    int i, n = 0;
    for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
        if (seats[i].used) n++;
    }
    return n;
}

static int poolFree(void) {
    int f = optSlots - poolUsed();
    return f > 0 ? f : 0;
}

static int poolUsedFor(int srv) {
    int i, n = 0;
    for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
        if (seats[i].used && seats[i].srv == srv) n++;
    }
    return n;
}

/* The slots server srv may use now: its own seats and the free ones;
 * none while the player withholds this computer (hostWithheld). */
static int poolSlotsFor(int srv) {
    if (hostWithheld()) return 0;
    return poolUsedFor(srv) + poolFree();
}

static HostSeat *seatFind(int srv, uint8_t seat) {
    int i;
    for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
        if (seats[i].used && seats[i].srv == srv && seats[i].seat == seat) {
            return &seats[i];
        }
    }
    return NULL;
}

static HostSeat *seatTakeFree(void) {
    int i;
    if (hostWithheld()) return NULL;
    if (poolUsed() >= optSlots) return NULL;
    for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
        if (!seats[i].used) return &seats[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Control connection                                                  */
/* ------------------------------------------------------------------ */

static void ctlCloseSocket(HostServer *sv) {
    if (sv->sock != INVALID_SOCKET) {
        closesocket(sv->sock);
        sv->sock = INVALID_SOCKET;
    }
}

static bool svResolve(HostServer *sv) {
    struct sockaddr_in *out = &sv->addr;
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(sv->port);
    out->sin_addr.s_addr = inet_addr(sv->host);
    if (out->sin_addr.s_addr == INADDR_NONE) {
        if (bolo_resolve_ipv4(sv->host, &out->sin_addr) != 0) {
            if (!sv->resolveLogged) {
                sv->resolveLogged = true;
                fprintf(stderr, "[bothost] cannot resolve server \"%s\"; "
                                "trying again later\n", sv->host);
            }
            sv->resolved = false;
            return false;
        }
    }
    sv->resolved = true;
    return true;
}

static void ctlSendJoin(HostServer *sv) {
    uint8_t buf[PACKET_HEADER_SIZE + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3 +
                WBN_JOIN_KEY_WIRE_LEN + 1 + 2 + 2 + JOIN_COOKIE_LEN +
                JOIN_BOT_HOST_KEY_LEN + 1 + JOIN_BOT_HOST_NAME_LEN + 2 +
                JOIN_BOT_HOST_PLAYER_LEN];
    int pos = PACKET_HEADER_SIZE;

    memset(buf, 0, sizeof(buf));
    packHeader(buf, PACKET_JOIN_REQUEST, sv->outSeq++);
    SDL_strlcpy((char *)buf + pos, optName, PACKET_MAX_PLAYER_NAME);
    pos += PACKET_MAX_PLAYER_NAME;
    pos += MAP_STR_SIZE;                    /* no game password */
    buf[pos++] = BOLO_VERSION_MAJOR;
    buf[pos++] = BOLO_VERSION_MINOR;
    buf[pos++] = BOLO_VERSION_REVISION;
    pos += WBN_JOIN_KEY_WIRE_LEN;           /* no WinBolo.net key */
    buf[pos++] = JOIN_FLAG_BOT_HOST;
    buf[pos++] = 0;                         /* client type: unknown */
    buf[pos++] = 0;                         /* client hints */
    pos += 2;                               /* no fallback country */
    if (sv->haveCookie) {
        memcpy(buf + pos, sv->cookie, JOIN_COOKIE_LEN);
    }
    pos += JOIN_COOKIE_LEN;
    memcpy(buf + pos, optKey, strlen(optKey));
    pos += JOIN_BOT_HOST_KEY_LEN;
    buf[pos++] = (uint8_t)poolSlotsFor(serverIndex(sv));
    SDL_strlcpy((char *)buf + pos, optName, JOIN_BOT_HOST_NAME_LEN);
    pos += JOIN_BOT_HOST_NAME_LEN;
    packU16(buf + pos, optListenPort);
    pos += 2;
    if (HOST_EMBED) {
        /* The player's own host: its ticket stands in for the key. */
        buf[pos++] = JOIN_BOT_HOST_PLAYER_MARK;
        buf[pos++] = embedPlayer;
        memcpy(buf + pos, embedTicket, BOT_HOST_TICKET_LEN);
        pos += BOT_HOST_TICKET_LEN;
    }
    udpSendTo(sv->sock, buf, pos, &sv->addr);
    sv->lastJoinMs = nowMs();
}

/* Start a join on a new socket, so the server sees a new address and never
 * mistakes it for a connection that went away. */
static void ctlStartJoin(HostServer *sv) {
    ctlCloseSocket(sv);
    if (!sv->resolved && !svResolve(sv)) return;
    sv->sock = createUdpSocket(false);
    if (sv->sock == INVALID_SOCKET) {
        fprintf(stderr, "[bothost] cannot open a UDP socket\n");
        return;
    }
    sv->state = CTL_JOINING;
    sv->haveCookie = false;
    sv->outSeq = 1;
    sv->lastRecvMs = nowMs();
    sv->joinStartMs = nowMs();
    channelMuxInit(&sv->mux);
    ctlSendJoin(sv);
}

static void ctlSendPing(HostServer *sv) {
    uint8_t buf[PACKET_HEADER_SIZE + 8];
    uint32_t t = nowMs();
    if (t == 0) t = 1;
    packHeader(buf, PACKET_PING, sv->outSeq++);
    packU32(buf + PACKET_HEADER_SIZE, t);
    packU32(buf + PACKET_HEADER_SIZE + 4, 0);
    udpSendTo(sv->sock, buf, sizeof(buf), &sv->addr);
    sv->lastPingMs = t;
}

static void ctlSendQuit(HostServer *sv) {
    uint8_t buf[PACKET_HEADER_SIZE];
    if (sv->sock == INVALID_SOCKET || sv->state != CTL_CONNECTED) return;
    packHeader(buf, PACKET_QUIT, sv->outSeq++);
    udpSendTo(sv->sock, buf, sizeof(buf), &sv->addr);
}

/* Send the channel's acks and the host's sync messages. */
static void ctlPump(HostServer *sv) {
    int frames;
    if (sv->state != CTL_CONNECTED) return;
    channelTick(&sv->mux, sv->muxTick++, sv->rttMs);
    bulkSenderPump(&sv->recBulk, &sv->mux);
    for (frames = 0; frames < (bulkSenderBusy(&sv->recBulk) ? 16 : 4);
         frames++) {
        uint8_t buf[UDP_MAX_PAYLOAD];
        int len = channelBuildFrame(&sv->mux, buf + PACKET_HEADER_SIZE,
                                    UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
        if (len <= 2) break;
        packHeader(buf, PACKET_CHANNEL, sv->outSeq++);
        udpSendTo(sv->sock, buf, PACKET_HEADER_SIZE + len, &sv->addr);
    }
}

static void ctlSendEvent(HostServer *sv, const ControlEvent *evt) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(evt->type);
    uint8_t msg[CHANNEL_CONTROL_SEG];
    size_t bodyLen = 0;
    if (enc == NULL ||
        enc(evt, NULL, msg + 3, sizeof(msg) - 3, &bodyLen) != ENCODE_OK) {
        return;
    }
    msg[0] = (uint8_t)evt->type;
    packU16(msg + 1, (uint16_t)bodyLen);
    if (!channelSend(&sv->mux, CHANNEL_CONTROL, msg, (uint16_t)(3 + bodyLen))) {
        fprintf(stderr, "[bothost] control channel to %s full; a message is "
                        "lost\n", sv->label);
    }
}

/* The recorder's way to the servers. */
static void recIoSendEvent(int srv, const ControlEvent *evt) {
    if (srv < 0 || srv >= serverCount) return;
    if (servers[srv].state != CTL_CONNECTED) return;
    ctlSendEvent(&servers[srv], evt);
}

static bool recIoBulkIdle(int srv) {
    if (srv < 0 || srv >= serverCount) return false;
    return servers[srv].state == CTL_CONNECTED &&
           !bulkSenderBusy(&servers[srv].recBulk);
}

static bool recIoBulkSend(int srv, const BulkStreamHeader *h,
                          const uint8_t *blob, uint32_t len) {
    if (!recIoBulkIdle(srv)) return false;
    return bulkSenderBegin(&servers[srv].recBulk, h, blob, len);
}

/* Tell the server the slots it may use, and turn down an assign when
 * refuseSeat is a seat. */
static void ctlSendSlots(HostServer *sv, uint8_t refuseSeat, uint16_t refuseGen) {
    ControlEvent evt;
    int slots = poolSlotsFor(serverIndex(sv));
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_SLOTS;
    evt.u.botHostSlots.slots = (uint8_t)slots;
    evt.u.botHostSlots.refuseSeat = refuseSeat;
    evt.u.botHostSlots.refuseGen = refuseGen;
    ctlSendEvent(sv, &evt);
    sv->lastSlotsSent = slots;
}

/* ------------------------------------------------------------------ */
/* Brain sync                                                          */
/* ------------------------------------------------------------------ */

/* Files asked for and not yet received, at most. */
#define BOT_HOST_SYNC_REQ_WINDOW 8
/* The largest packed chunk the server sends: zlib's worst case for one
 * BRAIN_SYNC_CHUNK_BYTES piece, the 4-byte length, and some room. */
#define BOT_HOST_SYNC_CHUNK_WIRE_MAX (BRAIN_SYNC_CHUNK_BYTES + \
                                      BRAIN_SYNC_CHUNK_BYTES / 1000u + 1024u)

/* The brain versions in the cache, for the announce. Read again after a
 * sync writes a new version. */
static BotHostLinkBrain cacheBrains[BOT_HOST_LINK_MAX_BRAINS];
static int              cacheBrainCount;
static bool             cacheListDirty = true;

static void syncSendProgress(HostServer *sv) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_SYNC;
    evt.u.botHostSync.gen = sv->sync.gen;
    evt.u.botHostSync.done = sv->sync.done;
    evt.u.botHostSync.total = sv->sync.total;
    evt.u.botHostSync.ready = sv->sync.ready ? 1 : 0;
    ctlSendEvent(sv, &evt);
}

static void syncRequestFile(HostServer *sv, uint32_t id) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_FILE_REQ;
    evt.u.botHostFileReq.gen = sv->sync.gen;
    evt.u.botHostFileReq.brain = (uint8_t)(id >> 16);
    evt.u.botHostFileReq.file = (uint16_t)(id & 0xffffu);
    ctlSendEvent(sv, &evt);
}

/* Ask for files until BOT_HOST_SYNC_REQ_WINDOW are on the way. */
static void syncRequestMore(HostServer *sv) {
    HostSync *hs = &sv->sync;
    while (hs->nextReq < hs->needCount &&
           hs->nextReq - hs->gotCount < BOT_HOST_SYNC_REQ_WINDOW) {
        syncRequestFile(sv, hs->need[hs->nextReq++]);
    }
}

static void syncDropFile(HostSync *hs) {
    free(hs->fileBuf);
    hs->fileBuf = NULL;
    hs->fileActive = false;
    hs->fileGot = 0;
}

/* A file came in damaged or could not be written: forget its bytes and ask
 * for it again, a few times a manifest at most. After that the host stays
 * not ready for this server, which runs its bots in-process. */
#define BOT_HOST_SYNC_MAX_RETRIES 3
static void syncRetryFile(HostServer *sv, uint32_t id) {
    HostSync *hs = &sv->sync;
    hs->done -= hs->fileGot;
    syncDropFile(hs);
    if (hs->retries >= BOT_HOST_SYNC_MAX_RETRIES) {
        if (hs->retries == BOT_HOST_SYNC_MAX_RETRIES) {
            hs->retries++;   /* the files still on the way are ignored */
            fprintf(stderr, "[bothost] too many bad brain files from %s; "
                            "this host waits for its next brain file list\n",
                    sv->label);
        }
        return;
    }
    hs->retries++;
    syncRequestFile(sv, id);
}

/* Forget the manifest and any transfer under way (connection lost). */
static void syncReset(HostSync *hs) {
    if (hs->bulk.state == BULK_RECV_BODY) free(hs->bulk.dst);
    bulkReceiverInit(&hs->bulk);
    syncDropFile(hs);
    brainSyncFree(&hs->man);
    free(hs->need);
    hs->need = NULL;
    hs->needCount = hs->nextReq = hs->gotCount = 0;
    hs->haveManifest = false;
    hs->ready = false;
    hs->gen = 0;
    hs->done = hs->total = 0;
}

#ifdef WINBOLO_BOTHOST_EMBED
static int embedCapacityFor(const HostServer *sv);
#else
/* Only the Bot Host inside a client asks the brain (HOST_EMBED is 0). */
static int embedCapacityFor(const HostServer *sv) {
    (void)sv;
    return optSlots;
}
#endif

static void syncFinishIfDone(HostServer *sv) {
    HostSync *hs = &sv->sync;
    if (hs->ready || hs->gotCount < hs->needCount) return;
    hs->ready = true;
    cacheListDirty = true;
    fprintf(stderr, "[bothost] brain files from %s ready (generation %u): %u "
                    "bytes downloaded in %u ms\n",
            sv->label, (unsigned)hs->gen, (unsigned)hs->done,
            (unsigned)(nowMs() - hs->syncStartMs));
    if (HOST_EMBED && embedSlotsFixed == 0 && embedCapacityGen != hs->gen) {
        /* The slots this computer offers: the server's bot brain's own
         * answer for this machine, now that its files are here. It is set
         * before the ready report, so the server never places more. */
        int cap = embedCapacityFor(sv);
        embedCapacityGen = hs->gen;
        if (cap != optSlots) {
            HOST_EVENT("[bothost] Bot Host \"%s\": brain \"%s\" can run %d "
                       "bots on this computer; slots %d -> %d", optName,
                       embedBrain, cap, optSlots, cap);
            optSlots = cap;
        }
    }
    syncSendProgress(sv);
}

static void syncOnManifest(HostServer *sv, const uint8_t *buf, uint32_t len,
                           uint32_t gen) {
    HostSync *hs = &sv->sync;
    BrainSyncManifest m;
    BrainSyncPlan plan;
    int b;

    if (!brainSyncManifestDecode(buf, len, &m)) {
        fprintf(stderr, "[bothost] the brain file list from %s is damaged; "
                        "ignored\n", sv->label);
        return;
    }
    syncDropFile(hs);
    brainSyncFree(&hs->man);
    free(hs->need);
    hs->need = NULL;
    hs->man = m;
    hs->gen = gen;
    hs->haveManifest = true;
    hs->ready = false;
    hs->nextReq = hs->gotCount = 0;
    hs->needCount = 0;
    hs->done = 0;
    hs->total = 0;
    hs->syncStartMs = nowMs();
    hs->retries = 0;
    memset(&plan, 0, sizeof(plan));
    if (!brainSyncPlanCache(&hs->man, optCacheDir, &plan)) {
        fprintf(stderr, "[bothost] cannot check the brain cache at \"%s\"\n",
                optCacheDir);
        free(plan.need);
        return;
    }
    hs->need = plan.need;
    hs->needCount = plan.needCount;
    hs->total = plan.needBytes;
    fprintf(stderr, "[bothost] brain file list from %s, generation %u: %d "
                    "brains; %d files held, %d copied from another version, "
                    "%d to download (%u bytes)\n",
            sv->label, (unsigned)gen, hs->man.brainCount, plan.haveCount,
            plan.copyCount, plan.needCount, (unsigned)plan.needBytes);
    for (b = 0; b < hs->man.brainCount; b++) {
        char hex[17];
        brainSyncVersionHex(hs->man.brains[b].version, hex);
        fprintf(stderr, "[bothost]   brain \"%s\" version %s\n",
                hs->man.brains[b].name, hex);
    }
    if (plan.copyCount > 0) cacheListDirty = true;
    syncSendProgress(sv);
    if (hs->needCount == 0) {
        syncFinishIfDone(sv);
        return;
    }
    syncRequestMore(sv);
}

/* Parse "<brain>/<file>/<offset>". */
static bool syncParseChunkPath(const char *p, uint32_t *bi, uint32_t *fi,
                               uint32_t *off) {
    unsigned long v[3];
    int i;
    for (i = 0; i < 3; i++) {
        char *end = NULL;
        if (*p < '0' || *p > '9') return false;
        v[i] = strtoul(p, &end, 10);
        if (end == p) return false;
        p = end;
        if (i < 2) {
            if (*p != '/') return false;
            p++;
        }
    }
    if (*p != '\0') return false;
    *bi = (uint32_t)v[0];
    *fi = (uint32_t)v[1];
    *off = (uint32_t)v[2];
    return true;
}

static void syncOnChunk(HostServer *sv, const BulkStreamHeader *h,
                        const uint8_t *buf) {
    HostSync *hs = &sv->sync;
    const BrainSyncBrain *br;
    const BrainSyncFile *f;
    uint32_t bi, fi, off, id;
    int64_t n;
    char path[1024];

    if (!hs->haveManifest || h->gen != hs->gen || hs->ready) return;
    if (hs->retries > BOT_HOST_SYNC_MAX_RETRIES) return;
    if (!syncParseChunkPath(h->path, &bi, &fi, &off)) return;
    if (bi >= (uint32_t)hs->man.brainCount) return;
    br = &hs->man.brains[bi];
    if (fi >= (uint32_t)br->fileCount) return;
    f = &br->files[fi];
    id = (bi << 16) | fi;
    if (off == 0) {
        syncDropFile(hs);
        hs->fileBuf = (uint8_t *)malloc(f->size > 0 ? f->size : 1);
        if (hs->fileBuf == NULL) return;
        hs->fileActive = true;
        hs->fileId = id;
        hs->fileGot = 0;
    }
    if (!hs->fileActive || hs->fileId != id || off != hs->fileGot) {
        return;
    }
    n = brainSyncChunkUnpack(buf, h->totalSize, hs->fileBuf + off,
                             f->size - off);
    if (n < 0 || (n == 0 && f->size != 0)) {
        fprintf(stderr, "[bothost] a piece of \"%s/%s\" is damaged\n",
                br->name, f->path);
        syncRetryFile(sv, id);
        return;
    }
    hs->fileGot += (uint32_t)n;
    hs->done += (uint32_t)n;
    if (hs->fileGot < f->size) {
        syncSendProgress(sv);
        return;
    }
    if (brainSyncHash(BRAIN_SYNC_HASH_SEED, hs->fileBuf, f->size) != f->hash ||
        !brainSyncCacheFile(path, sizeof(path), optCacheDir, br->name,
                            br->version, f->path) ||
        !brainSyncWriteFile(path, hs->fileBuf, f->size)) {
        fprintf(stderr, "[bothost] brain file \"%s/%s\" failed its check or "
                        "could not be written\n", br->name, f->path);
        syncRetryFile(sv, id);
        return;
    }
    syncDropFile(hs);
    hs->gotCount++;
    syncRequestMore(sv);
    syncSendProgress(sv);
    syncFinishIfDone(sv);
}

static uint8_t *syncBulkBegin(void *ctx, const BulkStreamHeader *h) {
    (void)ctx;
    if (h->kind == BULK_KIND_BRAIN_MANIFEST) {
        if (h->totalSize == 0 || h->totalSize > BRAIN_SYNC_MANIFEST_MAX) {
            return NULL;
        }
    } else if (h->kind == BULK_KIND_BRAIN_CHUNK) {
        if (h->totalSize < 4 || h->totalSize > BOT_HOST_SYNC_CHUNK_WIRE_MAX) {
            return NULL;
        }
    } else {
        return NULL;
    }
    return (uint8_t *)malloc(h->totalSize);
}

static void syncBulkComplete(void *ctx, const BulkStreamHeader *h,
                             uint8_t *buf) {
    HostServer *sv = (HostServer *)ctx;
    if (h->kind == BULK_KIND_BRAIN_MANIFEST) {
        syncOnManifest(sv, buf, h->totalSize, h->gen);
    } else if (h->kind == BULK_KIND_BRAIN_CHUNK) {
        syncOnChunk(sv, h, buf);
    }
    free(buf);
}

static void syncDrainBulk(HostServer *sv) {
    BulkRecvSink sink;
    uint8_t frag[CHANNEL_MAX_SEG];
    uint16_t fragLen = sizeof(frag);
    sink.onBegin = syncBulkBegin;
    sink.onComplete = syncBulkComplete;
    sink.ctx = sv;
    while (channelReceive(&sv->mux, CHANNEL_BULK, frag, &fragLen)) {
        bulkReceiverFeed(&sv->sync.bulk, frag, fragLen, &sink);
        fragLen = sizeof(frag);
    }
}

/* ------------------------------------------------------------------ */
/* Seat work                                                           */
/* ------------------------------------------------------------------ */

static void seatPrintCount(void) {
    int i;
    fprintf(stderr, "Bot Host \"%s\": %d/%d slots used", optName, poolUsed(),
            optSlots);
    for (i = 0; i < serverCount; i++) {
        int n = poolUsedFor(i);
        if (n > 0) fprintf(stderr, ", %d for %s", n, servers[i].label);
    }
    fprintf(stderr, "\n");
}

static HostSeat *seatForCs(ClientSim *cs) {
    int i;
    for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
        if (seats[i].used && seats[i].cs == cs) return &seats[i];
    }
    return NULL;
}

/* The brain's chat, queued during the think as an in-process bot's is and
 * sent after it. Runs on a worker thread; writes only this seat's job. */
static void seatChatSend(ClientSim *cs, uint8_t fromPlayer, uint8_t destPlayer,
                         const char *message) {
    HostSeat *hs;
    BotJobCtx *j;
    ClientCommand *cmd;
    size_t msgLen;

    (void)fromPlayer;
    if (cs == NULL || message == NULL || message[0] == '\0') return;
    hs = seatForCs(cs);
    if (hs == NULL) return;
    j = &hs->job;
    if (j->pendingCmdCount >= BOT_PENDING_CMD_MAX) return;
    cmd = &j->pendingCmds[j->pendingCmdCount++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = CMD_CHAT;
    cmd->u.chat.destPlayer = destPlayer;
    msgLen = strlen(message);
    if (msgLen > PACKET_MAX_CHAT_MESSAGE) msgLen = PACKET_MAX_CHAT_MESSAGE;
    cmd->u.chat.bodyLen = (uint16_t)msgLen;
    memcpy(cmd->u.chat.body, message, msgLen);
}

/* The brain's internal team message: queued like an in-process bot's, then
 * sent to the server as CMD_BOT_TEAM_MSG, which fans it out to the allied
 * bots. Worker thread; this seat's job only. */
static void seatRelayInternalMsg(void *ctx, BYTE fromPlayer, const char *msg) {
    HostSeat *hs = (HostSeat *)ctx;
    BotJobCtx *j = &hs->job;
    (void)fromPlayer;
    if (msg == NULL || msg[0] == '\0') return;
    if (j->pendingInternalMsgCount >= BOT_PENDING_INTERNAL_MSG_MAX) {
        memmove(j->pendingInternalMsg[0], j->pendingInternalMsg[1],
                (size_t)(BOT_PENDING_INTERNAL_MSG_MAX - 1) * BRAIN_INBOX_MSG_LEN);
        j->pendingInternalMsgCount = BOT_PENDING_INTERNAL_MSG_MAX - 1;
    }
    SDL_strlcpy(j->pendingInternalMsg[j->pendingInternalMsgCount], msg,
                BRAIN_INBOX_MSG_LEN);
    j->pendingInternalMsgCount++;
}

/* The brain's smart ping. The server holds a Bot Host seat to the same gap
 * between pings as an in-process bot. Worker thread; this seat only. */
static void seatRelayPing(void *ctx, BYTE fromPlayer, uint8_t kind,
                          uint16_t worldX, uint16_t worldY) {
    HostSeat *hs = (HostSeat *)ctx;
    ClientCommand *cmd;
    (void)fromPlayer;
    if (hs->pingCount >= BOT_PENDING_CMD_MAX) return;
    cmd = &hs->pings[hs->pingCount++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = CMD_PING;
    cmd->u.ping.kind = kind;
    cmd->u.ping.worldX = worldX;
    cmd->u.ping.worldY = worldY;
}

static void seatDestroyBrain(HostSeat *hs) {
    hostRecSeatGone((int)(hs - seats));
    free(hs->poolBuf);
    hs->poolBuf = NULL;
    hs->poolDoneExpr = NULL;
    if (hs->bot.brain.L != NULL || hs->bot.brain.running) {
        luaBrainInstanceDestroy(&hs->bot.brain);
    }
    memset(&hs->bot.brain, 0, sizeof(hs->bot.brain));
}

static void seatCloseConnection(HostSeat *hs) {
    seatDestroyBrain(hs);
    if (hs->cs != NULL) {
        clientSimDestroy(hs->cs);  /* sends PACKET_QUIT, frees the transport */
        hs->cs = NULL;
    }
}

static void seatClear(HostSeat *hs, const char *why) {
    if (!hs->used) return;
    HOST_EVENT("[bothost] bot %d \"%s\" of %s stopped (%s); team "
               "messages sent %u, received %u", (int)hs->seat, hs->name,
               servers[hs->srv].label, why, (unsigned)hs->teamMsgsOut,
               (unsigned)hs->teamMsgsIn);
    seatCloseConnection(hs);
    memset(hs, 0, sizeof(*hs));
    seatPrintCount();
}

/* Clear every seat of server srv, or of every server when srv is -1. */
static void clearSeats(int srv, const char *why) {
    int i;
    for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
        if (seats[i].used && (srv < 0 || seats[i].srv == srv)) {
            seatClear(&seats[i], why);
        }
    }
}

/* Open the seat's bot connection with its token. */
static bool seatOpenConnection(HostSeat *hs) {
    const HostServer *sv = &servers[hs->srv];
    hs->cs = clientSimAlloc();
    if (hs->cs == NULL) return false;
    clientSimCreate(hs->cs);
    clientSimSetIsBot(hs->cs, true);
    /* In a client this connection runs on the host's thread: keep it away
     * from the screen, the sound and the dialogs. */
    clientSimSetNoFrontEnd(hs->cs, true);
    clientSimSetMyLastPlayerName(hs->cs, hs->name);
    clientSimSetChatSendFunc(hs->cs, seatChatSend);
    hs->relay.ctx = hs;
    hs->relay.internalMsg = seatRelayInternalMsg;
    hs->relay.ping = seatRelayPing;
    clientSimSetBrainRelay(hs->cs, &hs->relay);
    *clientSimGetAllowComputerTanks(hs->cs) = (aiType)hs->ai;

    clientSimConnectUdp(hs->cs, sv->host, sv->port, hs->name, "", "", "", "",
                        false, NULL, 0, false);
    if (clientSimGetConnectState(hs->cs) == CLIENT_CONNECT_ERROR ||
        !clientSimHasTransport(hs->cs)) {
        clientSimDestroy(hs->cs);
        hs->cs = NULL;
        return false;
    }
    (void)transportUdpClientSetBotSlotToken(&hs->cs->transport, hs->token);
    hs->connectStartMs = nowMs();
    hs->lobbyNoted = false;
    hs->inputTick = 0;
    hs->recSeqOffset = 0;
    return true;
}

/* Build the seat's brain the way the bot pool builds an in-process one. */
static void seatBuildBrain(HostSeat *hs) {
    char path[512];
    const BrainSyncBrain *br;
    const HostSync *sync = &servers[hs->srv].sync;

    hs->wantBuild = false;
    seatDestroyBrain(hs);
    /* The brain runs from the cache: the server's own files, sent to this
     * host. */
    br = (sync->haveManifest && sync->ready)
             ? brainSyncFindBrain(&sync->man, hs->brainName) : NULL;
    if (br == NULL ||
        !brainSyncCacheFile(path, sizeof(path), optCacheDir, br->name,
                            br->version, "init.lua")) {
        fprintf(stderr, "[bothost] bot %d: the brain \"%s\" of %s is not in "
                        "the cache; it goes back to the server\n",
                (int)hs->seat, hs->brainName, servers[hs->srv].label);
        hs->buildFailed = true;
        return;
    }

    hs->bot.sim = NULL;
    hs->bot.cs = hs->cs;
    hs->bot.playerNum = hs->seat;
    hs->bot.ai = (aiType)hs->ai;
    hs->bot.active = true;
    SDL_strlcpy(hs->bot.brainPath, path, sizeof(hs->bot.brainPath));
    hs->bot.lastThinkMs = 0.0;
    hs->bot.maxThinkMs = 0.0;
    hs->bot.thinkCount = 0;
    hs->bot.overrunCount = 0;
    hs->bot.consecutiveCrashes = 0;
    hs->bot.lastPingTick = 0;
    hs->thinkMsSum = 0.0;
    hs->killCount = 0;
    *clientSimGetAllowComputerTanks(hs->cs) = (aiType)hs->ai;

    luaBrainsSetNextStartEngineTick((unsigned int)hs->startTick);
    luaBrainsSetNextInitArg(hs->initArg);
    fprintf(stderr, "Bot %d: brain init arg '%s'\n", (int)hs->seat,
            hs->initArg);
    if (!luaBrainInstanceCreate(&hs->bot.brain, path, hs->brainName, hs->cs,
                                (aiType)hs->ai, sync->man.debugMode,
                                (int)hs->seat,
                                &hs->bot.initTable)) {
        fprintf(stderr, "[bothost] bot %d: brain \"%s\" failed to load; it "
                        "goes back to the server\n", (int)hs->seat,
                hs->brainName);
        memset(&hs->bot.brain, 0, sizeof(hs->bot.brain));
        hs->buildFailed = true;
        return;
    }
    botRunnerWireBrain(&hs->bot);
    hs->bot.builtInit = hs->bot.initTable;
    hostRecSeatBuilt((int)(hs - seats), hs->srv, hs->bot.brain.L);
    HOST_EVENT("[bothost] bot %d \"%s\" of %s runs brain \"%s\" (ai %d) on "
               "Bot Host \"%s\"", (int)hs->seat, hs->name,
               servers[hs->srv].label, hs->brainName, (int)hs->ai, optName);
}

/* ------------------------------------------------------------------ */
/* Control events                                                      */
/* ------------------------------------------------------------------ */

static void onAssign(HostServer *sv, const ControlEvent *evt) {
    int srv = serverIndex(sv);
    uint8_t s = evt->u.botHostAssign.seat;
    HostSeat *hs;
    static const uint8_t zero[JOIN_BOT_SLOT_TOKEN_LEN] = {0};
    bool newPlacement;

    if (s >= MAX_TANKS) return;
    hs = seatFind(srv, s);
    newPlacement = memcmp(evt->u.botHostAssign.token, zero,
                          JOIN_BOT_SLOT_TOKEN_LEN) != 0;
    if (!newPlacement && hs == NULL) return;    /* an update for a seat gone */
    if (newPlacement && hs != NULL) {
        seatCloseConnection(hs);
    }
    if (newPlacement) {
        bool wasUsed = hs != NULL;
        if (hs == NULL) hs = seatTakeFree();
        if (hs == NULL) {
            /* Every slot is taken (another server was first): the seat
             * stays in-process on its server. */
            fprintf(stderr, "[bothost] bot %d of %s turned down: all %d "
                            "slots are taken\n", (int)s, sv->label, optSlots);
            ctlSendSlots(sv, s, evt->u.botHostAssign.gen);
            return;
        }
        memset(hs, 0, sizeof(*hs));
        hs->used = true;
        hs->srv = srv;
        hs->seat = s;
        memcpy(hs->token, evt->u.botHostAssign.token, JOIN_BOT_SLOT_TOKEN_LEN);
        hs->placeGen = evt->u.botHostAssign.gen;
        hs->wantBuild = true;
        if (!wasUsed) seatPrintCount();
    } else if (evt->u.botHostAssign.reload) {
        hs->wantBuild = true;
        hs->buildFailed = false;
    }
    hs->gen = evt->u.botHostAssign.gen;
    hs->ai = evt->u.botHostAssign.ai;
    hs->team = evt->u.botHostAssign.team;
    hs->startTick = evt->u.botHostAssign.startTick;
    hs->brainVersion = evt->u.botHostAssign.brainVersion;
    SDL_strlcpy(hs->name, evt->u.botHostAssign.name, sizeof(hs->name));
    SDL_strlcpy(hs->brainName, evt->u.botHostAssign.brain,
                sizeof(hs->brainName));
    SDL_strlcpy(hs->initArg, evt->u.botHostAssign.initArg,
                sizeof(hs->initArg));
    hs->awaitTable = true;
    scnTableClear(&hs->tableIn);

    if (newPlacement) {
        fprintf(stderr, "[bothost] bot %d \"%s\" of %s assigned (brain "
                        "\"%s\")\n", (int)s, hs->name, sv->label,
                hs->brainName);
        if (!seatOpenConnection(hs)) {
            fprintf(stderr, "[bothost] bot %d: cannot open its connection\n",
                    (int)s);
            memset(hs, 0, sizeof(*hs));
            seatPrintCount();
        }
    } else if (evt->u.botHostAssign.reload) {
        fprintf(stderr, "[bothost] bot %d \"%s\": new brain owed (brain "
                        "\"%s\")\n", (int)s, hs->name, hs->brainName);
    } else {
        fprintf(stderr, "[bothost] bot %d \"%s\": settings kept for the next "
                        "brain (team %u, init arg '%s')\n", (int)s, hs->name,
                (unsigned)hs->team, hs->initArg);
    }
}

static void onUnassign(HostServer *sv, const ControlEvent *evt) {
    HostSeat *hs = seatFind(serverIndex(sv), evt->u.botHostUnassign.seat);
    if (hs != NULL) seatClear(hs, "server took it back");
}

/* Collect one table entry; true when the table is complete. */
static bool tableTake(ScnTable *t, const ControlEvent *evt) {
    uint8_t idx = evt->u.botTableEntry.idx;
    uint8_t count = evt->u.botTableEntry.count;
    if (count == 0) {
        scnTableClear(t);
        return true;
    }
    if (idx == 0) scnTableClear(t);
    (void)scnTableSet(t, evt->u.botTableEntry.key, evt->u.botTableEntry.value);
    return idx + 1 >= count;
}

static void onInitTable(HostServer *sv, const ControlEvent *evt) {
    uint8_t s = evt->u.botTableEntry.seat;
    HostSeat *hs = seatFind(serverIndex(sv), s);
    char why[256];
    if (hs == NULL) return;
    if (!tableTake(&hs->tableIn, evt)) return;
    hs->bot.initTable = hs->tableIn;
    fprintf(stderr, "[bothost] bot %d \"%s\": init table with %d entries%s\n",
            (int)s, hs->name, evt->u.botTableEntry.count,
            hs->awaitTable ? " (for the next brain)" : "");
    if (hs->awaitTable) {
        /* The table that goes with an assign: the next brain reads it. */
        hs->awaitTable = false;
        return;
    }
    /* game.bot_init: the running brain is told, as in-process. */
    if (!hs->bot.brain.running || hs->bot.brain.L == NULL) return;
    why[0] = '\0';
    if (brainCoreUpdateInitTable(hs->bot.brain.L, &hs->bot.initTable, why,
                                 sizeof(why))) {
        WB_LOG_INFO(WB_LOG_CAT_LUA, "brain %d: init table replaced, on_init ran",
                    (int)s);
    } else if (why[0] != '\0') {
        WB_LOG_WARN(WB_LOG_CAT_LUA, "brain %d: on_init raised: %s", (int)s, why);
    }
}

static void onHint(HostServer *sv, const ControlEvent *evt) {
    uint8_t s = evt->u.botTableEntry.seat;
    HostSeat *hs = seatFind(serverIndex(sv), s);
    char err[256];
    if (hs == NULL) return;
    if (!tableTake(&hs->hintIn, evt)) return;
    fprintf(stderr, "[bothost] bot %d \"%s\": hint with %d entries\n", (int)s,
            hs->name, evt->u.botTableEntry.count);
    if (!hs->bot.brain.running || hs->bot.brain.L == NULL) return;
    err[0] = '\0';
    if (brainCoreCallScenarioHint(hs->bot.brain.L, &hs->hintIn, err,
                                  sizeof(err)) == BRAIN_HINT_ERROR) {
        WB_LOG_WARN(WB_LOG_CAT_LUA, "brain %d: on_scenario_hint error: %s",
                    (int)s, err);
    }
}

static void onTeamMsg(HostServer *sv, const ControlEvent *evt) {
    HostSeat *hs = seatFind(serverIndex(sv), evt->u.botTeamMsg.seat);
    MessageState *ms;
    if (hs == NULL || hs->cs == NULL) return;
    ms = clientSimGetMessages(hs->cs);
    if (ms == NULL) return;
    messageInboxPushLine(ms, evt->u.botTeamMsg.fromPlayer,
                         evt->u.botTeamMsg.text);
    hs->teamMsgsIn++;
}

/* The server's salt: the answer is SHA-256 of this computer's id and the
 * salt, so the server can find its hosts on one computer. A salt this
 * program answered before gets no answer. */
static void onMachineSalt(HostServer *sv, const ControlEvent *evt) {
    ControlEvent a;
    memset(&a, 0, sizeof(a));
    a.type = CTRL_BOT_HOST_MACHINE_ANSWER;
    memcpy(a.u.botHostMachineAnswer.salt, evt->u.botHostMachineSalt.salt,
           sizeof(a.u.botHostMachineAnswer.salt));
    if (!machineIdAnswerSalt(evt->u.botHostMachineSalt.salt,
                             a.u.botHostMachineAnswer.hash)) {
        fprintf(stderr, "[bothost] %s: a same-computer salt this program "
                        "answered before (or no machine id); no answer\n",
                sv->label);
        return;
    }
    ctlSendEvent(sv, &a);
}

static void ctlHandleControl(HostServer *sv, const uint8_t *msg, uint16_t len) {
    ControlEvent evt;
    ControlDecodeBodyFn dec;
    uint16_t bodyLen;

    if (len < 3) return;
    bodyLen = unpackU16(msg + 1);
    if ((size_t)bodyLen + 3 > len) return;
    dec = transportControlCodecBodyDecoder((ControlEventType)msg[0]);
    if (dec == NULL) return;
    memset(&evt, 0, sizeof(evt));
    if (!dec(msg + 3, bodyLen, &evt)) return;
    switch (evt.type) {
        case CTRL_BOT_HOST_ASSIGN:   onAssign(sv, &evt);    break;
        case CTRL_BOT_HOST_UNASSIGN: onUnassign(sv, &evt);  break;
        case CTRL_BOT_INIT_TABLE:    onInitTable(sv, &evt); break;
        case CTRL_BOT_HINT:          onHint(sv, &evt);      break;
        case CTRL_BOT_TEAM_MSG:      onTeamMsg(sv, &evt);   break;
        case CTRL_BOT_HOST_MACHINE_SALT: onMachineSalt(sv, &evt); break;
        case CTRL_BOT_HOST_REC:
            hostRecOnEvent(serverIndex(sv), &evt);
            break;
        default: break;
    }
}

/* Back to announcing after a wait. */
static void svGoIdle(HostServer *sv) {
    ctlCloseSocket(sv);
    sv->state = CTL_IDLE;
    /* The Bot Host inside a client has no listen port and gets no invite:
     * ctlService joins again once announceAtMs is reached. */
    sv->announceDue = true;
    sv->announceAtMs = nowMs() + BOT_HOST_REANNOUNCE_WAIT_MS;
    sv->lastAnnouncedFree = poolFree();
}

/* The control connection dropped: the server's seats go back to it, which
 * runs them in-process; the host announces again. */
static void ctlDrop(HostServer *sv, const char *why) {
    fprintf(stderr, "[bothost] connection to %s ended (%s)\n", sv->label, why);
    clearSeats(serverIndex(sv), "connection to the server ended");
    syncReset(&sv->sync);
    bulkSenderReset(&sv->recBulk);
    hostRecServerDown(serverIndex(sv));
    svGoIdle(sv);
}

static void ctlReceive(HostServer *sv) {
    uint8_t buf[UDP_MAX_PAYLOAD + 64];
    struct sockaddr_in from;
    int len;

    if (sv->sock == INVALID_SOCKET) return;
    while ((len = udpRecvFrom(sv->sock, buf, sizeof(buf), &from)) > 0) {
        uint8_t type;
        if (from.sin_addr.s_addr != sv->addr.sin_addr.s_addr ||
            from.sin_port != sv->addr.sin_port) {
            continue;
        }
        if (len < PACKET_HEADER_SIZE) continue;
        type = getPacketType(buf, len);
        sv->lastRecvMs = nowMs();
        switch (type) {
            case PACKET_JOIN_CHALLENGE:
                if (sv->state == CTL_JOINING &&
                    len >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                    memcpy(sv->cookie, buf + PACKET_HEADER_SIZE,
                           JOIN_COOKIE_LEN);
                    sv->haveCookie = true;
                    ctlSendJoin(sv);
                }
                break;
            case PACKET_JOIN_ACCEPT:
                if (sv->state == CTL_JOINING) {
                    sv->state = CTL_CONNECTED;
                    sv->lastPingMs = 0;
                    sv->lastSlotsSent = -1;
                    sv->lastCpuSent = -1;
                    sv->lastCpuBotSent = 0;
                    sv->lastFreeSeqSent = 0;
                    syncReset(&sv->sync);
                    bulkSenderInit(&sv->recBulk);
                    hostRecServerUp(serverIndex(sv), sv->label);
                    HOST_EVENT("[bothost] connected to %s as Bot Host "
                               "\"%s\" (%d slots free)",
                               sv->label, optName, poolFree());
                    seatPrintCount();
                }
                break;
            case PACKET_JOIN_REJECT: {
                uint16_t id = 0;
                if (len >= PACKET_HEADER_SIZE + 2) {
                    id = unpackU16(buf + PACKET_HEADER_SIZE);
                }
                if (sv->state != CTL_JOINING) break;
                if (id == STR_REJECT_INCORRECT_PASSWORD) {
                    fprintf(stderr, "[bothost] %s refused the key (or takes "
                                    "no Bot Host); it is not tried again\n",
                            sv->label);
                    sv->refused = true;
                    embedRefused = true;
                    ctlCloseSocket(sv);
                    sv->state = CTL_IDLE;
                    sv->announceDue = false;
                    return;
                }
                fprintf(stderr, "[bothost] %s refused the join (reason %u)\n",
                        sv->label, (unsigned)id);
                svGoIdle(sv);
                return;
            }
            case PACKET_PONG:
                if (len >= PACKET_HEADER_SIZE + 8) {
                    uint32_t sent = unpackU32(buf + PACKET_HEADER_SIZE);
                    sv->serverNow = unpackU32(buf + PACKET_HEADER_SIZE + 4);
                    sv->rttMs = (uint16_t)(nowMs() - sent);
                    /* Echo the server time back at once, with clientTime 0,
                     * so the server measures the round trip and not the
                     * gap to the next 500 ms PING. Same as a game client. */
                    {
                        uint8_t echo[PACKET_HEADER_SIZE + 8];
                        packHeader(echo, PACKET_PING, sv->outSeq++);
                        packU32(echo + PACKET_HEADER_SIZE, 0);
                        packU32(echo + PACKET_HEADER_SIZE + 4, sv->serverNow);
                        udpSendTo(sv->sock, echo, sizeof(echo), &sv->addr);
                    }
                }
                break;
            case PACKET_CHANNEL:
                if (sv->state == CTL_CONNECTED) {
                    uint8_t msg[CHANNEL_MAX_SEG];
                    uint16_t msgLen = sizeof(msg);
                    (void)channelRecvFrame(&sv->mux, buf + PACKET_HEADER_SIZE,
                                           len - PACKET_HEADER_SIZE);
                    while (channelReceive(&sv->mux, CHANNEL_CONTROL, msg,
                                          &msgLen)) {
                        ctlHandleControl(sv, msg, msgLen);
                        msgLen = sizeof(msg);
                    }
                    syncDrainBulk(sv);
                }
                break;
            case PACKET_KICKED:
                if (sv->state == CTL_CONNECTED) {
                    ctlDrop(sv, "the server closed it");
                    return;
                }
                break;
            default:
                break;
        }
    }
}

static void ctlService(HostServer *sv) {
    uint32_t t = nowMs();
    switch (sv->state) {
        case CTL_IDLE:
            if (HOST_EMBED && !sv->refused && sv->announceDue &&
                (int32_t)(t - sv->announceAtMs) >= 0) {
                sv->announceDue = false;
                ctlStartJoin(sv);
            }
            break;
        case CTL_JOINING:
            if (t - sv->joinStartMs >= BOT_HOST_JOIN_GIVE_UP_MS) {
                fprintf(stderr, "[bothost] %s did not answer the join\n",
                        sv->label);
                svGoIdle(sv);
                break;
            }
            if (t - sv->lastJoinMs >= BOT_HOST_JOIN_RETRY_MS) {
                ctlSendJoin(sv);
            }
            break;
        case CTL_CONNECTED:
            if (t - sv->lastRecvMs >= BOT_HOST_SILENCE_MS) {
                ctlDrop(sv, "no reply");
                break;
            }
            if (sv->lastPingMs == 0 || t - sv->lastPingMs >= BOT_HOST_PING_MS) {
                ctlSendPing(sv);
            }
            if (poolSlotsFor(serverIndex(sv)) != sv->lastSlotsSent) {
                ctlSendSlots(sv, BOT_HOST_SLOTS_NO_REFUSE, 0);
            }
            if ((int)hostCpuValue != sv->lastCpuSent) {
                ControlEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = CTRL_BOT_HOST_CPU;
                evt.u.botHostCpu.idx = (uint8_t)BOT_HOST_CPU_IDX_SELF;
                evt.u.botHostCpu.cpu = hostCpuValue;
                ctlSendEvent(sv, &evt);
                sv->lastCpuSent = (int)hostCpuValue;
            }
            if (hostCpuMeter.perBotMilli != sv->lastCpuBotSent) {
                /* One bot's part, finer than the byte above. It stays at
                 * the last game's value while no bot thinks. */
                ControlEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = CTRL_BOT_HOST_CPU_BOT;
                evt.u.botHostCpuBot.idx = (uint8_t)BOT_HOST_CPU_IDX_SELF;
                evt.u.botHostCpuBot.owner = (uint8_t)BOT_HOST_CPU_OWNER_NONE;
                evt.u.botHostCpuBot.milli = hostCpuMeter.perBotMilli;
                ctlSendEvent(sv, &evt);
                sv->lastCpuBotSent = hostCpuMeter.perBotMilli;
            }
            if (hostFreeSeq != 0 && hostFreeSeq != sv->lastFreeSeqSent) {
                ControlEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = CTRL_BOT_HOST_CPU_FREE;
                evt.u.botHostCpuFree.free = hostFreeValue;
                ctlSendEvent(sv, &evt);
                sv->lastFreeSeqSent = hostFreeSeq;
            }
            ctlPump(sv);
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Announce, query and invite                                          */
/* ------------------------------------------------------------------ */

static void announceSend(HostServer *sv, const struct sockaddr_in *to) {
    BotHostAnnounce a;
    uint8_t buf[PACKET_HEADER_SIZE + 1024];
    int len;

    if (cacheListDirty) {
        cacheListDirty = false;
        cacheBrainCount = botHostLinkListCache(optCacheDir, cacheBrains,
                                               BOT_HOST_LINK_MAX_BRAINS);
    }
    memset(&a, 0, sizeof(a));
    a.verMajor = BOLO_VERSION_MAJOR;
    a.verMinor = BOLO_VERSION_MINOR;
    a.verRevision = BOLO_VERSION_REVISION;
    a.listenPort = optListenPort;
    a.slots = (uint8_t)optSlots;
    a.freeSlots = (uint8_t)poolFree();
    SDL_strlcpy(a.name, optName, sizeof(a.name));
    a.brainCount = (uint8_t)cacheBrainCount;
    memcpy(a.brains, cacheBrains, sizeof(a.brains));
    len = botHostAnnounceEncode(&a, buf + PACKET_HEADER_SIZE,
                                sizeof(buf) - PACKET_HEADER_SIZE);
    if (len < 0) return;
    packHeader(buf, PACKET_BOT_HOST_ANNOUNCE, 0);
    udpSendTo(listenSock, buf, PACKET_HEADER_SIZE + len, to);
    (void)sv;
}

/* Announce to each idle server: at start, again every BOT_HOST_ANNOUNCE_MS
 * while slots are free, and at once when the free slots change. */
static void announceService(void) {
    int i, freeNow = poolFree();
    uint32_t t = nowMs();
    /* The Bot Host inside a client announces nothing: it joins its one
     * server with the player's ticket. */
    if (HOST_EMBED) return;
    for (i = 0; i < serverCount; i++) {
        HostServer *sv = &servers[i];
        bool due;
        if (sv->refused || sv->state != CTL_IDLE) continue;
        due = (sv->announceDue && (int32_t)(t - sv->announceAtMs) >= 0) ||
              freeNow != sv->lastAnnouncedFree;
        if (!due) continue;
        sv->lastAnnouncedFree = freeNow;
        if (freeNow > 0) {
            sv->announceDue = true;
            sv->announceAtMs = t + BOT_HOST_ANNOUNCE_MS;
        } else {
            sv->announceDue = false;
        }
        if (!sv->resolved && !svResolve(sv)) continue;
        announceSend(sv, &sv->addr);
    }
}

static HostServer *serverForAddr(const struct sockaddr_in *a) {
    int i;
    for (i = 0; i < serverCount; i++) {
        if (servers[i].resolved &&
            servers[i].addr.sin_addr.s_addr == a->sin_addr.s_addr &&
            servers[i].addr.sin_port == a->sin_port) {
            return &servers[i];
        }
    }
    return NULL;
}

/* QUERY and INVITE on the listen port. */
static void listenReceive(void) {
    static struct sockaddr_in logged[32];
    static int loggedCount = 0;
    uint8_t buf[UDP_MAX_PAYLOAD + 64];
    struct sockaddr_in from;
    int len;

    if (listenSock == INVALID_SOCKET) return;
    while ((len = udpRecvFrom(listenSock, buf, sizeof(buf), &from)) > 0) {
        uint8_t type = getPacketType(buf, len);
        HostServer *sv;
        if (type != PACKET_BOT_HOST_QUERY && type != PACKET_BOT_HOST_INVITE) {
            continue;
        }
        if (!botHostLinkShortValid(buf + PACKET_HEADER_SIZE,
                                   (size_t)(len - PACKET_HEADER_SIZE))) {
            continue;
        }
        sv = serverForAddr(&from);
        if (sv == NULL) {
            int k;
            bool seen = false;
            for (k = 0; k < loggedCount; k++) {
                if (logged[k].sin_addr.s_addr == from.sin_addr.s_addr &&
                    logged[k].sin_port == from.sin_port) {
                    seen = true;
                }
            }
            if (!seen) {
                if (loggedCount < 32) logged[loggedCount++] = from;
                fprintf(stderr, "[bothost] %s from %s:%u ignored: not on the "
                                "--servers list (logged once)\n",
                        type == PACKET_BOT_HOST_QUERY ? "query" : "invite",
                        inet_ntoa(from.sin_addr),
                        (unsigned)ntohs(from.sin_port));
            }
            continue;
        }
        if (sv->refused) continue;
        if (type == PACKET_BOT_HOST_QUERY) {
            fprintf(stderr, "[bothost] %s asks for free slots: %d free\n",
                    sv->label, poolFree());
            announceSend(sv, &from);
            if (sv->state == CTL_IDLE) sv->lastAnnouncedFree = poolFree();
            continue;
        }
        if (sv->state != CTL_IDLE) continue;
        if (poolFree() == 0) {
            fprintf(stderr, "[bothost] %s invites this host, but no slot is "
                            "free\n", sv->label);
            continue;
        }
        fprintf(stderr, "[bothost] %s invites this host; connecting\n",
                sv->label);
        ctlStartJoin(sv);
    }
}

/* ------------------------------------------------------------------ */
/* The think loop                                                      */
/* ------------------------------------------------------------------ */

/* The per-bot budget. serialMsEwma is the serial work in each tick that
 * runs before the brain workers and so delays them: the seats' network
 * ticks and the think setup. The work after the workers (sending the
 * inputs, the recorder, the stats) is not in it, and there is no reserve
 * for a game sim: this host runs none. --budget-legacy on puts back the
 * old formula: the work after the workers is in serialMsEwma, and the
 * server's 6 ms sim reserve comes off the budget. */
static double serialMsEwma = 0.0;
static double lastTargetMs = 0.0;
static bool   optBudgetLegacy = false;

/* Tick timing for the once-a-minute log line: ticks with bots, how many
 * took longer than one tick, the longest, and the averages of the parts.
 * startLateMax is how late the main loop started a tick (ms). */
typedef struct {
    uint32_t ticks, over;
    double   sumTotal, sumBefore, sumBrain, sumAfter, sumTarget;
    double   maxTotal, maxBrain, maxAfter;
    uint32_t startLateMax;
} HostTickStats;
static HostTickStats tickStats;
#define BOT_HOST_TICK_STATS_TICKS 3000   /* one line a minute */
static int    jobIndices[BOT_HOST_MAX_SLOTS];

static void hostThinkJob(int index, void *userData) {
    HostSeat *hs = &seats[index];
    GameSim *gs = clientSimGetGameSim(hs->cs);
    TankModifiers mods;
    (void)userData;

    memset(&mods, 0, sizeof(mods));
    if (MY_TANK(hs->cs) != NULL) {
        tankGetModifiers(MY_TANK(hs->cs), &mods);
    }
    botRunnerPrepareThink(&hs->bot, gs, &mods);
    botRunnerThink(&hs->job, lastTargetMs, hs->inputTick);

    /* The pool capture, here on the worker, as the server does it
     * (bot_manager.c runBotThinkJob). botRunnerThink has measured
     * lastThinkMs and taken off the budget hook before this, so the
     * capture can neither be stopped by the budget nor count as think
     * time. Skipped after a budget stop (the brain state is partial) and
     * after a crash (the seat goes). The seq is the one the recorder
     * uses: the think's own input packet sets the new offset. */
    hs->poolDoneExpr = NULL;
    free(hs->poolBuf);   /* NULL: the recorder takes it every tick */
    hs->poolBuf = NULL;
    if (hs->poolExpr != NULL && !hs->job.wasKilled && !hs->job.needRemove &&
        hs->bot.brain.L != NULL) {
        uint32_t seq = hs->job.hasInput ? hs->job.pkt1.tick
                                        : hs->inputTick + hs->recSeqOffset;
        if (hostRecPoolWanted(hs->poolMode, seq, hs->poolRrIndex,
                              hs->poolRrCount)) {
            hs->poolBuf = botBrainEvalLuaString(hs->bot.brain.L,
                                                hs->poolExpr);
            hs->poolDoneExpr = hs->poolExpr;
        }
    }

    /* Lua collection in the time left before the think deadline, as the
     * server does it (bot_manager.c runBotThinkJob): after the think and the
     * pool capture, outside lastThinkMs, ending 2 ms before the deadline. */
    if (!hs->job.needRemove) {
        botRunnerCollectGarbage(&hs->bot);
    }
}

static void seatSendStats(HostSeat *hs) {
    ClientCommand cmd;
    double avg;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_BOT_STATS;
    avg = hs->bot.thinkCount > 0 ? hs->thinkMsSum / hs->bot.thinkCount : 0.0;
#define TENTHS(ms) ((uint16_t)((ms) * 10.0 > 65535.0 ? 65535 : (ms) * 10.0))
    cmd.u.botStats.lastThinkTenths = TENTHS(hs->bot.lastThinkMs);
    cmd.u.botStats.avgThinkTenths  = TENTHS(avg);
    cmd.u.botStats.maxThinkTenths  = TENTHS(hs->bot.maxThinkMs);
    cmd.u.botStats.targetTenths    = TENTHS(lastTargetMs);
#undef TENTHS
    cmd.u.botStats.thinkCount   = hs->bot.thinkCount;
    cmd.u.botStats.overrunCount = hs->bot.overrunCount;
    cmd.u.botStats.killCount    = hs->killCount;
    cmd.u.botStats.tier         = 0;
    SDL_strlcpy(cmd.u.botStats.hostName, optName,
                sizeof(cmd.u.botStats.hostName));
    clientSimSubmitCommand(hs->cs, &cmd);
}

/* Bot connections: keep them ticking, notice when they end, and build the
 * brains that are owed. */
static bool seatsService(void) {
    int i;
    bool built = false;
    for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
        HostSeat *hs = &seats[i];
        ClientConnectState st;
        if (!hs->used || hs->cs == NULL) continue;
        clientSimNetTick(hs->cs);
        st = clientSimGetConnectState(hs->cs);
        if (st == CLIENT_CONNECT_JOINING || st == CLIENT_CONNECT_DOWNLOADING_MAP) {
            if (nowMs() - hs->connectStartMs > BOT_HOST_CONNECT_MS) {
                seatClear(hs, "its connection did not join");
            }
            continue;
        }
        if (st != CLIENT_CONNECT_CONNECTED) {
            const char *why = "its connection ended";
            if (st == CLIENT_CONNECT_KICKED) why = "the server released it";
            else if (st == CLIENT_CONNECT_ERROR) why = "its connection was refused";
            else if (st == CLIENT_CONNECT_SERVER_SHUTDOWN) why = "the server shut down";
            seatClear(hs, why);
            continue;
        }
        /* A new round sends the connection back through a map download.
         * The join limit counts from the last moment it was connected, not
         * from the first join, so a seat that has played for a while is not
         * dropped the moment the next round starts. */
        hs->connectStartMs = nowMs();
        if (!hs->lobbyNoted) {
            hs->lobbyNoted = true;
            if (clientSimIsInLobby(hs->cs)) {
                clientSimSetMapDownloadComplete(hs->cs, true);
                clientSimSetNetStatus(hs->cs, netLobby);
            }
        }
        if (hs->buildFailed) {
            /* Give the seat back: the server runs it in-process. */
            seatClear(hs, "no brain here");
            continue;
        }
        if (hs->wantBuild && !hs->awaitTable) {
            seatBuildBrain(hs);
            built = true;
        }
    }
    return built;
}

static void hostTick(void) {
    int i, active = 0, runners;
    bool anyFirst = false;
    bool built;
    Uint64 setupStart, brainStart, brainEnd, sendEnd;
    double freq = (double)SDL_GetPerformanceFrequency();

    setupStart = SDL_GetPerformanceCounter();
    built = seatsService();

    for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
        HostSeat *hs = &seats[i];
        netStatus ns;
        if (!hs->used || hs->cs == NULL) continue;
        if (!hs->bot.brain.running || hs->wantBuild || hs->awaitTable) continue;
        if (clientSimGetConnectState(hs->cs) != CLIENT_CONNECT_CONNECTED) continue;
        ns = clientSimGetNetStatus(hs->cs);
        if (ns == netLobby || ns == netLobbyCountdown) continue;
        if (clientSimIsInLobby(hs->cs)) continue;
        if (MY_TANK(hs->cs) == NULL) continue;

        hs->job.bot = &hs->bot;
        hs->job.sim = NULL;
        hs->job.needRemove = false;
        hs->job.hasInput = false;
        hs->job.wasKilled = false;
        hs->job.pendingCmdCount = 0;
        hs->job.pendingInternalMsgCount = 0;
        hs->pingCount = 0;
        hs->inputTick += 2;
        jobIndices[active++] = i;
    }
    botManagerSetHostActiveBots(poolUsed());

    runners = botWorkerPoolGetSize() + 1;
    if (optThreads > runners) runners = optThreads;
    if (optBudgetLegacy) {
        lastTargetMs = botRunnerTargetMs(serialMsEwma, runners, active);
    } else {
        lastTargetMs = botRunnerTargetMsReserve(serialMsEwma, 0.0, runners,
                                                active);
    }
    for (i = 0; i < active; i++) {
        HostSeat *hs = &seats[jobIndices[i]];
        int k;
        botRunnerBeginTick(&hs->bot, lastTargetMs);
        if (hs->bot.brain.isFirst) anyFirst = true;
        /* The pool capture plan for the worker: the recorder's state and
         * the seat's round-robin place, the same count as the recorder's
         * below. When a seat goes between the think and the recorder the
         * count there can differ; then the recorder frees the capture or
         * makes it itself. */
        hs->poolExpr = hostRecSeatPoolPlan(
            hs->srv, servers[hs->srv].sync.man.debugMode, &hs->poolMode);
        hs->poolRrIndex = 0;
        hs->poolRrCount = 0;
        for (k = 0; k < active && hs->poolExpr != NULL; k++) {
            if (seats[jobIndices[k]].srv != hs->srv) continue;
            if (k < i) hs->poolRrIndex++;
            hs->poolRrCount++;
        }
    }

    brainStart = SDL_GetPerformanceCounter();
    if (anyFirst) {
        for (i = 0; i < active; i++) hostThinkJob(jobIndices[i], NULL);
    } else if (active > 0) {
        botWorkerPoolRun(jobIndices, active, hostThinkJob, NULL);
    }
    brainEnd = SDL_GetPerformanceCounter();

    for (i = 0; i < active; i++) {
        HostSeat *hs = &seats[jobIndices[i]];
        BotJobCtx *j = &hs->job;
        int q;

        if (hs->bot.thinkCount > 0) hs->thinkMsSum += hs->bot.lastThinkMs;
        if (hs->bot.thinkCount != hs->bot.thinkCountSeen) {
            hs->bot.thinkCountSeen = hs->bot.thinkCount;
            hostThinkMsTotal += hs->bot.lastThinkMs;
        }

        for (q = 0; q < j->pendingInternalMsgCount; q++) {
            ClientCommand cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = CMD_BOT_TEAM_MSG;
            SDL_strlcpy(cmd.u.botTeamMsg.text, j->pendingInternalMsg[q],
                        sizeof(cmd.u.botTeamMsg.text));
            clientSimSubmitCommand(hs->cs, &cmd);
            hs->teamMsgsOut++;
        }
        j->pendingInternalMsgCount = 0;

        if (j->needRemove) {
            /* The in-process crash rule: the seat goes back to the server,
             * whose bot pool then kicks the bot as it would its own. */
            fprintf(stderr, "[bothost] bot %d: brain crashed %u ticks in a "
                            "row\n", (int)hs->seat,
                    (unsigned)hs->bot.consecutiveCrashes);
            seatClear(hs, "brain crashed");
            continue;
        }
        if (j->wasKilled) {
            hs->bot.wasKilled = true;
            hs->bot.overrunCount++;
            hs->killCount++;
            continue;
        }
        if (j->hasInput) {
            hs->recSeqOffset = j->pkt1.tick - hs->inputTick;
            clientSimNetRecordInput(hs->cs, &j->pkt1);
            clientSimNetSendInput(hs->cs, &j->pkt2);
            for (q = 0; q < j->pendingCmdCount; q++) {
                clientSimSubmitCommand(hs->cs, &j->pendingCmds[q]);
            }
            for (q = 0; q < hs->pingCount; q++) {
                clientSimSubmitCommand(hs->cs, &hs->pings[q]);
            }
            if (hs->bot.lastThinkMs > lastTargetMs * 1.5) {
                hs->bot.overrunCount++;
            }
        }
        j->pendingCmdCount = 0;
        hs->pingCount = 0;
    }

    /* The brain recordings: each seat that thought, in the order the
     * server would capture pools (round robin among one server's seats). */
    for (i = 0; i < active; i++) {
        HostSeat *hs = &seats[jobIndices[i]];
        int k, rrIndex = 0, rrCount = 0;
        char *poolBuf = hs->poolBuf;
        const char *poolDoneExpr = hs->poolDoneExpr;
        hs->poolBuf = NULL;
        hs->poolDoneExpr = NULL;
        if (!hs->used || hs->bot.brain.L == NULL || hs->job.wasKilled ||
            hs->job.needRemove) {
            free(poolBuf);
            continue;
        }
        for (k = 0; k < active; k++) {
            const HostSeat *o = &seats[jobIndices[k]];
            if (!o->used || o->srv != hs->srv) continue;
            if (k < i) rrIndex++;
            rrCount++;
        }
        hostRecSeatThink(jobIndices[i], hs->srv, hs->seat, hs->placeGen,
                         hs->inputTick + hs->recSeqOffset,
                         hs->startTick + hs->inputTick / 2u,
                         (float)hs->bot.lastThinkMs, hs->bot.brain.L,
                         &hs->bot.brain.overlay,
                         servers[hs->srv].sync.man.debugMode, rrIndex,
                         rrCount, poolBuf, poolDoneExpr);
    }
    hostRecService(nowMs());

    for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
        HostSeat *hs = &seats[i];
        if (!hs->used || hs->cs == NULL || !hs->bot.brain.running) continue;
        if (clientSimGetConnectState(hs->cs) != CLIENT_CONNECT_CONNECTED) continue;
        if (++hs->statsTick >= BOT_HOST_STATS_TICKS) {
            hs->statsTick = 0;
            seatSendStats(hs);
        }
    }

    hostCpuValue = botHostCpuMeterSample(&hostCpuMeter, SDL_GetTicksNS(),
                                         hostThinkMsTotal,
                                         SDL_GetNumLogicalCPUCores(),
                                         poolUsed());
    if (hostFreeSeq == 0 || nowMs() - hostFreeAtMs >= BOT_HOST_FREE_MS) {
        hostFreeAtMs = nowMs();
        hostFreeValue = machineCpuFreeSample(&hostFreeMeter);
        hostFreeSeq++;
        if (hostFreeSeq == 0) hostFreeSeq = 1;
    }

    sendEnd = SDL_GetPerformanceCounter();
    /* A tick that built a brain or ran a first think has one-time setup
     * in it (the brain build, the recording's first file). Such a sample
     * is not the normal serial work. As the first sample it would seed the
     * average high and hold the budget at its 1 ms floor for many ticks,
     * so those ticks are left out. */
    if (active > 0) {
        HostTickStats *ts = &tickStats;
        double before = (double)(brainStart - setupStart) * 1000.0 / freq;
        double brain = (double)(brainEnd - brainStart) * 1000.0 / freq;
        double after = (double)(sendEnd - brainEnd) * 1000.0 / freq;
        double total = before + brain + after;
        ts->ticks++;
        if (total > (double)BOT_HOST_TICK_MS) ts->over++;
        ts->sumTotal += total;
        ts->sumBefore += before;
        ts->sumBrain += brain;
        ts->sumAfter += after;
        ts->sumTarget += lastTargetMs;
        if (total > ts->maxTotal) ts->maxTotal = total;
        if (brain > ts->maxBrain) ts->maxBrain = brain;
        if (after > ts->maxAfter) ts->maxAfter = after;
        if (ts->ticks >= BOT_HOST_TICK_STATS_TICKS) {
            double n = (double)ts->ticks;
            fprintf(stderr, "[bothost] ticks %u: %u over %d ms, longest "
                            "%.1f ms, avg %.1f ms (before the brains %.2f, "
                            "brains %.1f max %.1f, after %.2f max %.1f), "
                            "target avg %.1f ms, latest start %u ms\n",
                    (unsigned)ts->ticks, (unsigned)ts->over,
                    BOT_HOST_TICK_MS, ts->maxTotal, ts->sumTotal / n,
                    ts->sumBefore / n, ts->sumBrain / n, ts->maxBrain,
                    ts->sumAfter / n, ts->maxAfter, ts->sumTarget / n,
                    (unsigned)ts->startLateMax);
            memset(ts, 0, sizeof(*ts));
        }
    }
    if (!built && !anyFirst) {
        double serialMs = (double)(brainStart - setupStart);
        if (optBudgetLegacy) serialMs += (double)(sendEnd - brainEnd);
        serialMs = serialMs * 1000.0 / freq;
        if (serialMsEwma <= 0.0) serialMsEwma = serialMs;
        else serialMsEwma = 0.1 * serialMs + 0.9 * serialMsEwma;
    }
}

/* ------------------------------------------------------------------ */
/* Arguments                                                           */
/* ------------------------------------------------------------------ */

#ifndef WINBOLO_BOTHOST_EMBED
static void printUsage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --servers ADDR[:PORT],... [options]\n"
            "Runs WinBoloDS servers' bots on this machine.\n"
            "  --servers LIST  the servers this host offers its slots to\n"
            "                  (required): ADDR[:PORT] entries split by commas,\n"
            "                  or @FILE, or a file name (one entry per line,\n"
            "                  # comments). PORT is the server's game port\n"
            "                  (default %d).\n"
            "  --listen PORT   UDP port for the servers' queries and invites\n"
            "                  (default %d). List it in the server's -bothosts.\n"
            "  --slots N       bots this host runs at most, for all servers\n"
            "                  together (default 4)\n"
            "  --threads N     brain threads (default: one per CPU core)\n"
            "  --key KEY       the servers' -bothostkey, when they have one\n"
            "  --name NAME     the name the server operators and players see\n"
            "  --cachedir DIR  where the servers' brain files are kept\n"
            "                  (default: bothost_cache next to this program)\n"
            "  --record-min-free-gb N  record the bots of a -brain-debug\n"
            "                  server only while the cache's disk has N GB\n"
            "                  free (default 50)\n"
            "  --budget-legacy on|off  on: the old think budget (the\n"
            "                  recorder's work and the server's 6 ms sim\n"
            "                  reserve come off each bot's budget). Default\n"
            "                  off\n",
            prog, BOT_HOST_DEFAULT_SERVER_PORT, BOT_HOST_DEFAULT_LISTEN_PORT);
}

static double optRecordMinFreeGb = 50.0;

static bool parseArgs(int argc, char **argv) {
    int i;
    bool haveServers = false;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printUsage(argv[0]);
            return false;
        }
        if (v == NULL) {
            fprintf(stderr, "%s needs a value\n", a);
            return false;
        }
        if (strcmp(a, "--servers") == 0) {
            char err[256];
            if (!botHostAddrListLoad(v, &optServers, err, sizeof(err))) {
                fprintf(stderr, "--servers: %s\n", err);
                return false;
            }
            haveServers = true;
        } else if (strcmp(a, "--server") == 0 || strcmp(a, "--port") == 0) {
            fprintf(stderr, "%s is gone: use --servers ADDR[:PORT],...\n", a);
            return false;
        } else if (strcmp(a, "--listen") == 0) {
            int p = atoi(v);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "bad port %s\n", v);
                return false;
            }
            optListenPort = (uint16_t)p;
        } else if (strcmp(a, "--slots") == 0) {
            optSlots = atoi(v);
            if (optSlots < 1 || optSlots > BOT_HOST_MAX_SLOTS) {
                fprintf(stderr, "--slots takes 1 to %d\n", BOT_HOST_MAX_SLOTS);
                return false;
            }
        } else if (strcmp(a, "--threads") == 0) {
            optThreads = atoi(v);
            if (optThreads < 0) optThreads = 0;
        } else if (strcmp(a, "--key") == 0) {
            if (strlen(v) > JOIN_BOT_HOST_KEY_LEN) {
                fprintf(stderr, "--key takes up to %d characters\n",
                        JOIN_BOT_HOST_KEY_LEN);
                return false;
            }
            SDL_strlcpy(optKey, v, sizeof(optKey));
        } else if (strcmp(a, "--name") == 0) {
            SDL_strlcpy(optName, v, sizeof(optName));
        } else if (strcmp(a, "--cachedir") == 0) {
            SDL_strlcpy(optCacheDir, v, sizeof(optCacheDir));
        } else if (strcmp(a, "--budget-legacy") == 0) {
            if (strcmp(v, "on") == 0 || strcmp(v, "1") == 0) {
                optBudgetLegacy = true;
            } else if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0) {
                optBudgetLegacy = false;
            } else {
                fprintf(stderr, "--budget-legacy takes on or off\n");
                return false;
            }
        } else if (strcmp(a, "--record-min-free-gb") == 0) {
            optRecordMinFreeGb = atof(v);
            if (optRecordMinFreeGb < 0.0) optRecordMinFreeGb = 0.0;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            printUsage(argv[0]);
            return false;
        }
        i++;
    }
    if (!haveServers) {
        fprintf(stderr, "--servers is required\n");
        printUsage(argv[0]);
        return false;
    }
    if (optCacheDir[0] == '\0') {
        const char *base = SDL_GetBasePath();
        SDL_snprintf(optCacheDir, sizeof(optCacheDir), "%sbothost_cache",
                     base != NULL ? base : "");
    }
    {
        /* Cache paths use '/' throughout; drop a trailing separator. */
        size_t n = strlen(optCacheDir);
        char *c;
        for (c = optCacheDir; *c != '\0'; c++) {
            if (*c == '\\') *c = '/';
        }
        while (n > 1 && optCacheDir[n - 1] == '/') optCacheDir[--n] = '\0';
    }
    if (!brainRecMakeAbsolute(optCacheDir, sizeof(optCacheDir))) {
        fprintf(stderr, "--cachedir %s: cannot make it a full path\n",
                optCacheDir);
        return false;
    }
    if (optName[0] == '\0') {
        const char *h = SDL_getenv("COMPUTERNAME");
        if (h == NULL || h[0] == '\0') h = SDL_getenv("HOSTNAME");
        SDL_strlcpy(optName, (h != NULL && h[0] != '\0') ? h : "Bot Host",
                    sizeof(optName));
    }
    return true;
}

#endif /* !WINBOLO_BOTHOST_EMBED */

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

#ifndef WINBOLO_BOTHOST_EMBED
static bool openListenSocket(void) {
    struct sockaddr_in a;
    listenSock = createUdpSocket(true);
    if (listenSock == INVALID_SOCKET) return false;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(optListenPort);
    if (bind(listenSock, (struct sockaddr *)&a, sizeof(a)) != 0) {
        closesocket(listenSock);
        listenSock = INVALID_SOCKET;
        return false;
    }
#ifdef _WIN32
    {
        /* An announce to a server that is not running yet comes back as an
         * ICMP "port unreachable", which Windows would report on the next
         * receive. It means nothing here. */
        BOOL off = FALSE;
        DWORD got = 0;
        (void)WSAIoctl(listenSock, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off),
                       NULL, 0, &got, NULL, NULL);
    }
#endif
    return true;
}

int main(int argc, char *argv[]) {
    uint32_t nextTick;
    int i;

    bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());
    sentryInit("WinBoloBotHost", argc, argv);
    atexit(sentryClose);

    if (!parseArgs(argc, argv)) return 1;
    /* This computer's id and the salts answered before, read now, so two
     * programs on one computer both answer a new salt. */
    (void)machineIdInit();

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#ifdef SIGBREAK
    signal(SIGBREAK, signalHandler);
#endif

    if (!SDL_Init(0)) {
        fprintf(stderr, "Error: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    wb_log_init("WinBolo", "WinBoloBotHost", "winbolo-bothost.log");
    atexit(wb_log_shutdown);

    botHostCpuMeterInit(&hostCpuMeter);
    machineCpuFreeInit(&hostFreeMeter);
    if (!serverSimBotPoolInit(optThreads)) {
        fprintf(stderr, "Error: cannot start %d brain threads\n", optThreads);
        return 1;
    }
    prefsInit("WinBolo.json");
    if (!clientMutexCreate()) {
        fprintf(stderr, "Error: failed to create client mutex\n");
        return 1;
    }
    guiMessageSetHandler(hostMessageHandler);
    langSetup();
    if (bolo_net_init() != 0) {
        fprintf(stderr, "Error: network start failed\n");
        return 1;
    }
    if (!openListenSocket()) {
        fprintf(stderr, "Error: cannot listen on UDP port %u (another "
                        "program may use it; see --listen)\n",
                (unsigned)optListenPort);
        return 1;
    }

    memset(seats, 0, sizeof(seats));
    memset(servers, 0, sizeof(servers));
    serverCount = optServers.count;
    for (i = 0; i < serverCount; i++) {
        HostServer *sv = &servers[i];
        SDL_strlcpy(sv->host, optServers.e[i].host, sizeof(sv->host));
        sv->port = optServers.e[i].port != 0 ? optServers.e[i].port
                                             : BOT_HOST_DEFAULT_SERVER_PORT;
        SDL_snprintf(sv->label, sizeof(sv->label), "%s:%u", sv->host,
                     (unsigned)sv->port);
        sv->sock = INVALID_SOCKET;
        sv->state = CTL_IDLE;
        sv->announceDue = true;
        sv->announceAtMs = nowMs();
        sv->lastAnnouncedFree = -1;
        sv->lastSlotsSent = -1;
        bulkReceiverInit(&sv->sync.bulk);
        (void)svResolve(sv);
    }

    fprintf(stderr, "[bothost] WinBoloBotHost \"%s\": %d slots for %d "
                    "server%s, listening on UDP port %u\n",
            optName, optSlots, serverCount, serverCount == 1 ? "" : "s",
            (unsigned)optListenPort);
    for (i = 0; i < serverCount; i++) {
        fprintf(stderr, "[bothost]   server %s\n", servers[i].label);
    }
    fprintf(stderr, "[bothost] brain cache: %s\n", optCacheDir);
    {
        /* Brain recordings for -brain-debug servers. print2 starts off and
         * is turned on per block (its log goes into the block's folder);
         * the jsonl log is not recorded here. */
        static const HostRecIo io = { recIoSendEvent, recIoBulkIdle,
                                      recIoBulkSend };
        char recRoot[600];
        SDL_snprintf(recRoot, sizeof(recRoot), "%s/recordings", optCacheDir);
        luaBrainsSetDebugParts(0, 1, 1, 0);
        hostRecInit(optCacheDir, optName, optRecordMinFreeGb, &io);
        luaBrainsSetExtraWriteRoot(recRoot);
        fprintf(stderr, "[bothost] brain recordings: %s (only while %.0f GB "
                        "are free)\n", recRoot, optRecordMinFreeGb);
    }

    nextTick = nowMs();
    while (!hostQuit) {
        uint32_t t;
        bool anyUsable = false;
        listenReceive();
        for (i = 0; i < serverCount; i++) {
            ctlReceive(&servers[i]);
            ctlService(&servers[i]);
            if (!servers[i].refused) anyUsable = true;
        }
        if (!anyUsable) {
            fprintf(stderr, "[bothost] every server refused this host\n");
            break;
        }
        t = nowMs();
        if ((int32_t)(t - nextTick) >= 0) {
            if (t - nextTick > tickStats.startLateMax) {
                tickStats.startLateMax = t - nextTick;
            }
            hostTick();
            nextTick += BOT_HOST_TICK_MS;
            /* After a long stall, start counting again from now. */
            if ((int32_t)(t - nextTick) > 10 * BOT_HOST_TICK_MS) {
                nextTick = t + BOT_HOST_TICK_MS;
            }
        }
        announceService();
        SDL_Delay(1);
    }

    fprintf(stderr, "[bothost] stopping\n");
    clearSeats(-1, "the host stops");
    hostRecShutdown();
    for (i = 0; i < serverCount; i++) {
        bulkSenderReset(&servers[i].recBulk);
        ctlSendQuit(&servers[i]);
        ctlCloseSocket(&servers[i]);
        syncReset(&servers[i].sync);
    }
    if (listenSock != INVALID_SOCKET) closesocket(listenSock);

    clientMutexDestroy();
    langCleanup();
    serverSimBotPoolDestroy();
    SDL_Quit();
    return hostQuit ? 0 : 2;
}
#endif /* !WINBOLO_BOTHOST_EMBED */

/* ------------------------------------------------------------------ */
/* The Bot Host inside a client                                        */
/* ------------------------------------------------------------------ */

#ifdef WINBOLO_BOTHOST_EMBED

static SDL_Thread   *embedThread;
static SDL_AtomicInt embedStop;
static SDL_AtomicInt embedAlive;      /* the thread runs its loop */
static SDL_AtomicInt embedConnected;  /* the control connection is up */
static SDL_AtomicInt embedSlotsNow;
static SDL_AtomicInt embedUsedNow;
static SDL_AtomicInt embedRefusedNow;
static bool          embedSendQuit;
static double        embedRecordMinFreeGb = 50.0;
static int           embedSavedParts[4];

static int embedCapacityFor(const HostServer *sv) {
    BrainMachineInfo machine;
    const BrainSyncBrain *br = NULL;
    char path[512];
    int cap = BRAIN_CAPACITY_UNKNOWN;

    brainCapacityGetMachine(&machine);
    if (sv->sync.haveManifest) {
        br = brainSyncFindBrain(&sv->sync.man, embedBrain);
        if (br == NULL && sv->sync.man.brainCount > 0) {
            br = &sv->sync.man.brains[0];
        }
    }
    if (br != NULL &&
        brainSyncCacheFile(path, sizeof(path), optCacheDir, br->name,
                           br->version, "init.lua")) {
        /* The answer is kept by brain name and version, so the brain is
         * asked once. */
        cap = luaBrainQueryCapacity(path, br->name);
    }
    if (cap < 0) cap = brainCapacityDefault(&machine);
    if (cap > BOT_HOST_MAX_SLOTS) cap = BOT_HOST_MAX_SLOTS;
    if (cap < 0) cap = 0;
    return cap;
}

static void embedPublish(void) {
    int i, connected = 0;
    for (i = 0; i < serverCount; i++) {
        if (servers[i].state == CTL_CONNECTED) connected = 1;
    }
    SDL_SetAtomicInt(&embedConnected, connected);
    SDL_SetAtomicInt(&embedSlotsNow, optSlots);
    SDL_SetAtomicInt(&embedUsedNow, poolUsed());
    SDL_SetAtomicInt(&embedRefusedNow, embedRefused ? 1 : 0);
}

static int SDLCALL embedThreadFn(void *arg) {
    uint32_t nextTick = nowMs();
    int i;
    (void)arg;

    for (i = 0; i < serverCount; i++) ctlStartJoin(&servers[i]);
    while (!SDL_GetAtomicInt(&embedStop)) {
        uint32_t t;
        bool anyUsable = false;
        for (i = 0; i < serverCount; i++) {
            ctlReceive(&servers[i]);
            ctlService(&servers[i]);
            if (!servers[i].refused) anyUsable = true;
        }
        if (!anyUsable) {
            HOST_EVENT("[bothost] Bot Host \"%s\": the server refused it",
                       optName);
            break;
        }
        t = nowMs();
        if ((int32_t)(t - nextTick) >= 0) {
            hostTick();
            nextTick += BOT_HOST_TICK_MS;
            if ((int32_t)(t - nextTick) > 10 * BOT_HOST_TICK_MS) {
                nextTick = t + BOT_HOST_TICK_MS;
            }
        }
        embedPublish();
        SDL_Delay(1);
    }

    /* Stop: give every bot back at once. A normal stop tells the server
     * (each bot connection's quit, then the host's); a vanish says
     * nothing, as a crash would, and the server times the host out. */
    if (embedSendQuit) {
        clearSeats(-1, "the player left");
    } else {
        /* Only for a program that ends next: the bot connections are
         * left as they are, so none sends its quit. */
        for (i = 0; i < BOT_HOST_MAX_SLOTS; i++) {
            if (seats[i].used) seatDestroyBrain(&seats[i]);
        }
    }
    hostRecShutdown();
    for (i = 0; i < serverCount; i++) {
        bulkSenderReset(&servers[i].recBulk);
        if (embedSendQuit) ctlSendQuit(&servers[i]);
        ctlCloseSocket(&servers[i]);
        syncReset(&servers[i].sync);
    }
    embedPublish();
    SDL_SetAtomicInt(&embedConnected, 0);
    SDL_SetAtomicInt(&embedUsedNow, 0);
    SDL_SetAtomicInt(&embedAlive, 0);
    return 0;
}

int botHostEmbedDefaultSlots(void) {
    BrainMachineInfo machine;
    int n;
    brainCapacityGetMachine(&machine);
    n = brainCapacityDefault(&machine);
    if (n > BOT_HOST_MAX_SLOTS) n = BOT_HOST_MAX_SLOTS;
    return n;
}

bool botHostEmbedStart(const BotHostEmbedConfig *cfg) {
    /* As main() does: the id and the answered salts, read once. */
    (void)machineIdInit();
    HostServer *sv;
    size_t n;
    char *c;

    if (cfg == NULL || cfg->serverHost == NULL || cfg->ticket == NULL) {
        return false;
    }
    if (embedThread != NULL) botHostEmbedStop(true);

    memset(seats, 0, sizeof(seats));
    memset(servers, 0, sizeof(servers));
    memset(optKey, 0, sizeof(optKey));
    optListenPort = 0;
    listenSock = INVALID_SOCKET;
    serialMsEwma = 0.0;
    lastTargetMs = 0.0;
    hostThinkMsTotal = 0.0;
    botHostCpuMeterInit(&hostCpuMeter);
    hostCpuValue = (uint8_t)BOT_HOST_CPU_UNKNOWN;
    machineCpuFreeInit(&hostFreeMeter);
    hostFreeValue = (uint8_t)MACHINE_CPU_FREE_UNKNOWN;
    hostFreeSeq = 0;
    hostFreeAtMs = 0;
    embedPlayer = cfg->player;
    memcpy(embedTicket, cfg->ticket, sizeof(embedTicket));
    SDL_strlcpy(embedBrain, cfg->brain != NULL ? cfg->brain : "",
                sizeof(embedBrain));
    embedSlotsFixed = cfg->slots > 0 ? cfg->slots : 0;
    if (embedSlotsFixed > BOT_HOST_MAX_SLOTS) embedSlotsFixed = BOT_HOST_MAX_SLOTS;
    embedCapacityGen = 0;
    embedRefused = false;
    embedSendQuit = true;
    if (cfg->recordMinFreeGb > 0.0) embedRecordMinFreeGb = cfg->recordMinFreeGb;
    /* Until the brain's own answer comes with its files: the default. */
    optSlots = embedSlotsFixed > 0 ? embedSlotsFixed : botHostEmbedDefaultSlots();
    SDL_strlcpy(optName, (cfg->name != NULL && cfg->name[0] != '\0')
                             ? cfg->name : "Player",
                sizeof(optName));
    SDL_strlcpy(optCacheDir, cfg->cacheDir != NULL ? cfg->cacheDir : "",
                sizeof(optCacheDir));
    for (c = optCacheDir; *c != '\0'; c++) {
        if (*c == '\\') *c = '/';
    }
    n = strlen(optCacheDir);
    while (n > 1 && optCacheDir[n - 1] == '/') optCacheDir[--n] = '\0';
    if (optCacheDir[0] == '\0' ||
        !brainRecMakeAbsolute(optCacheDir, sizeof(optCacheDir))) {
        WB_LOG_ERROR(WB_LOG_CAT_NET, "[bothost] no brain cache folder");
        return false;
    }
    (void)SDL_CreateDirectory(optCacheDir);

    serverCount = 1;
    sv = &servers[0];
    SDL_strlcpy(sv->host, cfg->serverHost, sizeof(sv->host));
    sv->port = cfg->serverPort != 0 ? cfg->serverPort
                                    : BOT_HOST_DEFAULT_SERVER_PORT;
    SDL_snprintf(sv->label, sizeof(sv->label), "%s:%u", sv->host,
                 (unsigned)sv->port);
    sv->sock = INVALID_SOCKET;
    sv->state = CTL_IDLE;
    sv->lastAnnouncedFree = -1;
    sv->lastSlotsSent = -1;
    bulkReceiverInit(&sv->sync.bulk);
    if (!svResolve(sv)) {
        WB_LOG_ERROR(WB_LOG_CAT_NET, "[bothost] cannot resolve %s", sv->label);
        return false;
    }

    {
        /* Brain recordings for a -brain-debug server, as the Bot Host
         * program makes them: print2 per block, no jsonl log. The settings
         * are the program's own, so they go back when the host stops. */
        static const HostRecIo io = { recIoSendEvent, recIoBulkIdle,
                                      recIoBulkSend };
        char recRoot[600];
        luaBrainsGetDebugParts(&embedSavedParts[0], &embedSavedParts[1],
                               &embedSavedParts[2], &embedSavedParts[3]);
        SDL_snprintf(recRoot, sizeof(recRoot), "%s/recordings", optCacheDir);
        luaBrainsSetDebugParts(0, 1, 1, 0);
        hostRecInit(optCacheDir, optName, embedRecordMinFreeGb, &io);
        luaBrainsSetExtraWriteRoot(recRoot);
    }

    SDL_SetAtomicInt(&embedStop, 0);
    SDL_SetAtomicInt(&embedAlive, 1);
    SDL_SetAtomicInt(&embedConnected, 0);
    SDL_SetAtomicInt(&embedSlotsNow, optSlots);
    SDL_SetAtomicInt(&embedUsedNow, 0);
    SDL_SetAtomicInt(&embedRefusedNow, 0);
    HOST_EVENT("[bothost] Bot Host \"%s\" starts in this program for %s: "
               "player %u, %d slots%s, brain cache %s", optName, sv->label,
               (unsigned)embedPlayer, optSlots,
               embedSlotsFixed > 0 ? " (fixed)" : "", optCacheDir);
    embedThread = SDL_CreateThread(embedThreadFn, "bothost", NULL);
    if (embedThread == NULL) {
        SDL_SetAtomicInt(&embedAlive, 0);
        hostRecShutdown();
        luaBrainsSetExtraWriteRoot("");
        luaBrainsSetDebugParts(embedSavedParts[0], embedSavedParts[1],
                               embedSavedParts[2], embedSavedParts[3]);
        WB_LOG_ERROR(WB_LOG_CAT_NET, "[bothost] cannot start its thread: %s",
                     SDL_GetError());
        return false;
    }
    return true;
}

void botHostEmbedStop(bool tellServer) {
    if (embedThread == NULL) return;
    embedSendQuit = tellServer;
    SDL_SetAtomicInt(&embedStop, 1);
    SDL_WaitThread(embedThread, NULL);
    embedThread = NULL;
    luaBrainsSetExtraWriteRoot("");
    luaBrainsSetDebugParts(embedSavedParts[0], embedSavedParts[1],
                           embedSavedParts[2], embedSavedParts[3]);
    HOST_EVENT("[bothost] Bot Host \"%s\" stopped (%s)", optName,
               tellServer ? "the server was told" : "without a word");
}

bool botHostEmbedRunning(void) {
    return embedThread != NULL;
}

void botHostEmbedSetWithhold(bool on) {
    int v = on ? 1 : 0;
    if (SDL_GetAtomicInt(&embedWithhold) == v) return;
    SDL_SetAtomicInt(&embedWithhold, v);
    if (embedThread == NULL) return;
    HOST_EVENT("[bothost] Bot Host \"%s\": %s", optName,
               on ? "the player turned bots off on this computer for this "
                    "game; it offers 0 slots"
                  : "the player turned bots on again; it offers its slots");
}

void botHostEmbedGetStatus(BotHostEmbedStatus *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->running   = embedThread != NULL && SDL_GetAtomicInt(&embedAlive) != 0;
    out->connected = SDL_GetAtomicInt(&embedConnected) != 0;
    out->refused   = SDL_GetAtomicInt(&embedRefusedNow) != 0;
    out->slots     = SDL_GetAtomicInt(&embedSlotsNow);
    out->bots      = SDL_GetAtomicInt(&embedUsedNow);
}

#endif /* WINBOLO_BOTHOST_EMBED */
