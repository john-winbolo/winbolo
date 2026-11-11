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
 *                  [--slots N] [--slots-max N] [--cpu-reserve PCT]
 *                  [--threads N] [--name NAME] [--cachedir DIR]
 *
 * Dynamic slots: a server that knows them sends the CTRL_BOT_HOST_LIMITS
 * hello. The host then offers it its ceiling (--slots-max) and tells it
 * its start (--slots) and the free CPU it tries to keep (--cpu-reserve);
 * the server starts the host at --slots and raises it while it has free
 * CPU. An older server sends no hello and gets at most --slots.
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
#include "brain_pack.h"
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
#include "bot_host_guard.h" /* the init token list, the salt limit */
#ifdef WINBOLO_BOTHOST_EMBED
#include "bothost_embed.h"
#include "bothost_optout.h"   /* BotHostRun */
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
/* --slots-max: the ceiling for a server with dynamic slots; 0 for
 * max(--slots, logical CPUs - 1), at most BOT_HOST_MAX_SLOTS. */
static int      optSlotsMax    = 0;
/* --cpu-reserve: the free CPU percent this host tries to keep. The server
 * gives it no raise and no new bot that would take the free CPU under it;
 * the bots may still push the CPU past it when no other place has room. */
static int      optCpuReserve  = BOT_HOST_RAISE_RESERVE_PCT_DEFAULT;
static int      optThreads     = 0;
static char     optKey[JOIN_BOT_HOST_KEY_LEN + 1];
static char     optName[JOIN_BOT_HOST_NAME_LEN];
/* The name the bot stats carry. Empty for a player's own host when the
 * player has no name: each client then shows its own text for it, as the
 * lobby's Bot Host rows do. */
static char     optStatsName[JOIN_BOT_HOST_NAME_LEN];
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
/* The player's Max slots plus 1, so 0 (the start value) is
 * BOT_HOST_MAX_AUTO. Max 0: the host offers the server 0 slots and takes no
 * new seat, but stays connected, so it can offer its slots again. Set by
 * the client's main thread (botHostEmbedSetMax), read by the host's
 * thread. */
static SDL_AtomicInt embedMaxPlus1;
/* WINBOLO_BOTHOST_OFFICIAL_ONLY (a CMake option, on by default): the Bot
 * Host inside a client runs only the brains of its own install, checked
 * file by file with SHA-256. No brain file is taken from a server; a brain
 * that differs from the install is not run, and the host offers 0 slots.
 * The Bot Host program is never built with it: its operator chose to run
 * it, and a server's -bothost-code tells it what it may take. */
#if HOST_EMBED && defined(WINBOLO_BOTHOST_OFFICIAL_ONLY)
#define HOST_OFFICIAL_ONLY 1
#else
#define HOST_OFFICIAL_ONLY 0
#endif
/* The build's own table of the brain files it shipped, with the SHA-256 of
 * each (cmake/brain_shipped_table.cmake writes it). Only an official-only
 * client has it: its host runs a brain only when the install's files are
 * the shipped ones (syncShippedSame). */
#if HOST_EMBED && defined(WINBOLO_BOTHOST_OFFICIAL_ONLY)
#include "brain_shipped_table.h"
#define HOST_SHIPPED_TABLE 1
#else
#define HOST_SHIPPED_TABLE 0
#endif
/* WINBOLO_BOTHOST_ALLOW_FULL_MAP (a CMake option, on by default): this host
 * tells the server it takes bots that see the whole map
 * (BRAIN_PACK_CAP_FULL_MAP), and the host inside a client runs them. OFF:
 * the host inside a client sends such a bot back to the server. */
#if defined(WINBOLO_BOTHOST_ALLOW_FULL_MAP)
#define HOST_ALLOW_FULL_MAP 1
#else
#define HOST_ALLOW_FULL_MAP 0
#endif
/* The player's answer, for this join, to "this server wants to send its
 * own bot code" (BOT_HOST_EMBED_CONSENT_*): asked only by a client built
 * without WINBOLO_BOTHOST_OFFICIAL_ONLY, when a server in custom mode
 * sends brain files that differ from the install. Set by the client's main
 * thread (botHostEmbedConsentAnswer), read by the host's thread;
 * botHostEmbedStart clears it. */
static SDL_AtomicInt embedConsent;
/* The player started WinBolo with -bothost-record: the host inside the
 * client may record its bots' brains for a -brain-debug server. Off by
 * default. Set before the host starts (botHostEmbedSetRecord). */
static bool embedRecord;

#ifdef WINBOLO_BOTHOST_EMBED
/* The server is on this computer (CTRL_BOT_HOST_SAME_SERVER): the host
 * stops for this join (bothost_optout.h). The host's thread sets it;
 * botHostEmbedStart clears it. embedSameServerNow is the copy the client's
 * main thread reads (botHostEmbedGetStatus). */
static BotHostRun    embedRun;
static SDL_AtomicInt embedSameServerNow;
#endif

/* The host is stopped for this join: the server is on this computer. */
static bool hostStopped(void) {
#ifdef WINBOLO_BOTHOST_EMBED
    return botHostRunStopped(&embedRun);
#else
    return false;
#endif
}

/* The host offers no slots now (the Bot Host inside a client only): its
 * player set Max slots 0, or it stopped for this join. */
static bool hostWithheld(void) {
    return HOST_EMBED &&
           (SDL_GetAtomicInt(&embedMaxPlus1) == 1 || hostStopped());
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
    /* Where this manifest's received files go: the cache, or the session
     * folder of the Bot Host inside a client. */
    char              root[600];
    /* The packed sync (a v2 manifest, brain_pack.h). installRoot: the
     * host's own brain folder, only read; "" when none. */
    bool              packMode;
    char              installRoot[600];
    bool              inPlace[BRAIN_SYNC_MAX_BRAINS];
    bool              staged[BRAIN_SYNC_MAX_BRAINS];
    /* Per brain: the files asked for in the stream, and those that came
     * in and passed their checks. */
    uint8_t           packBits[BRAIN_SYNC_MAX_BRAINS][BRAIN_PACK_BITMAP_BYTES];
    uint8_t           packGot[BRAIN_SYNC_MAX_BRAINS][BRAIN_PACK_BITMAP_BYTES];
    BrainPackReader  *readers[BRAIN_SYNC_MAX_BRAINS];
    uint32_t          packSeq[BRAIN_SYNC_MAX_BRAINS];
    int               packPending;  /* streams not yet ended */
    uint32_t          packWire;     /* compressed bytes received */
    int               fromInstall;
    int               fromCache;
    /* installOnly: this manifest's brains run only from the install
     * (the server's -bothost-code official, or HOST_OFFICIAL_ONLY); no file
     * is asked for. refused: the manifest cannot be run that way (a v1 or
     * debug-mode list): no bot runs here. */
    bool              installOnly;
    bool              refused;
    /* The last manifest as it came, kept while it waits: for the player's
     * answer (embedConsent) or for the player to turn bots on again
     * (hostWithheld). pending: it waits now. */
    uint8_t          *lastBuf;
    uint32_t          lastLen;
    uint32_t          lastGen;
    bool              pending;
    int               pendWhy;     /* SYNC_PEND_* */
} HostSync;

/* Why a manifest waits (HostSync.pending). */
#define SYNC_PEND_WITHHELD 1   /* the player turned bots off on this computer */
#define SYNC_PEND_CONSENT  2   /* the player's answer to the server's code */

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
    /* The server sent the dynamic slots hello (CTRL_BOT_HOST_LIMITS): it
     * is offered the ceiling. lastLimitsSent: the answer last sent, packed
     * (hostLimitsPacked), -1 for none. */
    bool       limHello;
    int        lastLimitsSent;
    /* The CPU byte last sent in CTRL_BOT_HOST_CPU, -1 for none. */
    int        lastCpuSent;
    /* The per-bot value last sent in CTRL_BOT_HOST_CPU_BOT; 0 for none, so
     * the first one (unknown too) goes out. */
    uint32_t   lastCpuBotSent;
    /* The full-budget estimate last sent in CTRL_BOT_HOST_CPU_BUDGET; 0 for
     * none. */
    uint32_t   lastCpuBudgetSent;
    /* The free CPU reading last sent in CTRL_BOT_HOST_CPU_FREE (its
     * hostFreeSeq), 0 for none. */
    uint32_t   lastFreeSeqSent;

    HostSync   sync;
    /* The same-computer salts answered on this connection (a server may
     * not collect hashes; bot_host_guard.h). */
    BotHostSaltLimit saltLimit;
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
/* The full-budget estimate of one bot's part of this machine's CPU
 * (botHostCpuMilliBudget): the think budget a new bot would get here over
 * the time between thinks, over the logical cores. Worked out in the tick
 * loop every BOT_HOST_BUDGET_EVERY_MS. */
static uint32_t hostCpuBudgetMilli = BOT_HOST_CPU_MILLI_UNKNOWN;
static uint64_t hostCpuBudgetAtMs;
#define BOT_HOST_BUDGET_EVERY_MS 2000u
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

/* The index of brain name in the manifest of hs, or -1. */
static int syncBrainIndex(const HostSync *hs, const char *name) {
    int b;
    if (!hs->haveManifest || name == NULL) return -1;
    for (b = 0; b < hs->man.brainCount; b++) {
        if (strcmp(hs->man.brains[b].name, name) == 0) return b;
    }
    return -1;
}

/* The Bot Host inside a client offers this server no slot now: its brain
 * list was refused, it waits for the player's answer or for bots to be
 * turned on again, or (official brains only) its install does not hold
 * the server's bot brain. */
static bool syncOffersNone(const HostSync *hs) {
    int b;
    if (!HOST_EMBED) return false;
    if (hs->refused || hs->pending) return true;
    if (hs->haveManifest && hs->installOnly) {
        b = syncBrainIndex(hs, embedBrain);
        if (b < 0 || !hs->inPlace[b]) return true;
    }
    return false;
}

/* The most bots this host runs (its ceiling): the Bot Host program's
 * --slots-max, at least --slots; the host inside a client its player's
 * Max slots, or the brain's answer when the player set none. */
static int hostCeiling(void) {
#ifdef WINBOLO_BOTHOST_EMBED
    return botHostMaxCeiling(SDL_GetAtomicInt(&embedMaxPlus1) - 1, optSlots);
#else
    return optSlotsMax > optSlots ? optSlotsMax : optSlots;
#endif
}

/* The slots server srv counts on in all: the ceiling for a server that
 * sent the dynamic slots hello, else at most the start (--slots, or the
 * brain's answer). */
static int capFor(int srv) {
    bool hello = srv >= 0 && srv < serverCount && servers[srv].limHello;
    return botHostMaxOffer(hostCeiling(), optSlots, hello);
}

/* The slots server srv may use now: its own seats and the free ones, at
 * most capFor (a lowered Max shows the server its bots are over it); none
 * while the player set Max 0 or the host stopped (hostWithheld), or while
 * the host may run none of the server's bots (syncOffersNone). */
static int poolSlotsFor(int srv) {
    int cap, n, free;
    if (hostWithheld()) return 0;
    if (srv >= 0 && srv < serverCount && syncOffersNone(&servers[srv].sync)) {
        return 0;
    }
    cap = capFor(srv);
    free = cap - poolUsed();
    n = poolUsedFor(srv) + (free > 0 ? free : 0);
    return n < cap ? n : cap;
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

static HostSeat *seatTakeFree(int srv) {
    int i;
    if (hostWithheld()) return NULL;
    if (poolUsed() >= capFor(srv)) return NULL;
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

/* The dynamic slots answer, packed to see a change: start, ceiling and
 * reserve. The host inside a client leaves start and reserve to the server
 * (-bothost-client-start, -bothost-client-reserve-pct). */
static void hostLimits(uint8_t *start, uint8_t *ceiling, uint8_t *reserve) {
    int c = hostStopped() ? 0 : hostCeiling();
    *ceiling = (uint8_t)(c > BOT_HOST_MAX_SLOTS ? BOT_HOST_MAX_SLOTS : c);
    if (HOST_EMBED) {
        *start = (uint8_t)BOT_HOST_LIMIT_NONE;
        *reserve = (uint8_t)BOT_HOST_LIMIT_NONE;
    } else {
        *start = (uint8_t)(optSlots < c ? optSlots : c);
        *reserve = (uint8_t)optCpuReserve;
    }
}

static int hostLimitsPacked(void) {
    uint8_t s, c, r;
    hostLimits(&s, &c, &r);
    return (int)s | ((int)c << 8) | ((int)r << 16);
}

/* Tell a server with dynamic slots this host's start, ceiling and
 * reserve. */
static void ctlSendLimits(HostServer *sv) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_LIMITS;
    hostLimits(&evt.u.botHostLimits.start, &evt.u.botHostLimits.ceiling,
               &evt.u.botHostLimits.reserve);
    ctlSendEvent(sv, &evt);
    sv->lastLimitsSent = hostLimitsPacked();
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

/* The CTRL_BOT_HOST_SYNC_CAPS bits this host sends. */
static uint8_t hostSyncCaps(void) {
    uint8_t caps = BRAIN_PACK_CAP_PACKED;
    if (HOST_OFFICIAL_ONLY) caps |= BRAIN_PACK_CAP_OFFICIAL_ONLY;
    if (HOST_ALLOW_FULL_MAP) caps |= BRAIN_PACK_CAP_FULL_MAP;
    return caps;
}

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

static void syncDropReaders(HostSync *hs) {
    int b;
    for (b = 0; b < BRAIN_SYNC_MAX_BRAINS; b++) {
        brainPackReaderFree(hs->readers[b]);
        hs->readers[b] = NULL;
    }
    hs->packPending = 0;
}

/* Forget the manifest and any transfer under way (connection lost). */
static void syncReset(HostSync *hs) {
    free(hs->lastBuf);
    hs->lastBuf = NULL;
    hs->lastLen = hs->lastGen = 0;
    hs->pending = false;
    hs->installOnly = false;
    hs->refused = false;
    syncDropReaders(hs);
    hs->packMode = false;
    memset(hs->inPlace, 0, sizeof(hs->inPlace));
    memset(hs->staged, 0, sizeof(hs->staged));
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

/* The session folder of the Bot Host inside a client: the brain files it
 * receives from its server live there only while it is connected
 * (brain_pack.h). Made when the first file list comes. */
static BrainSession embedSession = { "", -1 };

static void syncSessionEnd(void) {
    if (!HOST_EMBED || embedSession.dir[0] == '\0') return;
    if (!brainSessionEnd(optCacheDir, &embedSession)) {
        fprintf(stderr, "[bothost] the brain session folder could not be "
                        "removed; the next start tries again\n");
    }
}

/* Where received brain files go: the per-version cache, or for the Bot
 * Host inside a client its session folder. False when it cannot be made. */
static bool syncRootFor(char *out, size_t sz) {
    if (!HOST_EMBED) {
        SDL_strlcpy(out, optCacheDir, sz);
        return true;
    }
    if (embedSession.dir[0] == '\0' &&
        !brainSessionCreate(optCacheDir, &embedSession)) {
        return false;
    }
    SDL_strlcpy(out, embedSession.dir, sz);
    return true;
}

/* The host's own brain folder, the release brains next to its program: a
 * source of files only, never written. WINBOLO_BOTHOST_INSTALL_DIR names
 * another one ("none": no install). "" when there is none. */
static void syncInstallRoot(char *out, size_t sz) {
    const char *env = SDL_getenv("WINBOLO_BOTHOST_INSTALL_DIR");
    const char *base;
    static const char *const names[] = { "Brains", "brains" };
    int i;
    out[0] = '\0';
    if (env != NULL && env[0] != '\0') {
        if (SDL_strcasecmp(env, "none") != 0) SDL_strlcpy(out, env, sz);
        return;
    }
    base = SDL_GetBasePath();
    if (base == NULL) return;
    for (i = 0; i < 2; i++) {
        SDL_PathInfo pi;
        char dir[600];
        SDL_snprintf(dir, sizeof(dir), "%s%s", base, names[i]);
        if (SDL_GetPathInfo(dir, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
            SDL_strlcpy(out, dir, sz);
            return;
        }
    }
}

/* The init.lua a seat of brain br runs: in the install when the brain runs
 * in place, else in the received files' folder. */
static bool syncBrainInitPath(const HostSync *hs, const BrainSyncBrain *br,
                              char *out, size_t sz) {
    ptrdiff_t b = br - hs->man.brains;
    if (b >= 0 && b < hs->man.brainCount && hs->inPlace[b]) {
        return (size_t)SDL_snprintf(out, sz, "%s/%s/init.lua", hs->installRoot,
                                    br->name) < sz;
    }
    return brainSyncCacheFile(out, sz, hs->root, br->name, br->version,
                              "init.lua");
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

/* The slots this computer offers: the server's bot brain's own answer for
 * this machine, once its files are here. It is set before the ready
 * report, so the server never places more. Not while the player has set
 * Max slots 0 on this computer, nor while the host may run none of the
 * server's bots: the brain's code does not run then. */
static void syncCapacityIfOwed(HostServer *sv) {
    HostSync *hs = &sv->sync;
    int cap;
    if (!HOST_EMBED || embedSlotsFixed != 0 || !hs->ready) return;
    if (embedCapacityGen == hs->gen) return;
    if (hostWithheld() || syncOffersNone(hs)) return;
    cap = embedCapacityFor(sv);
    embedCapacityGen = hs->gen;
    if (cap != optSlots) {
        HOST_EVENT("[bothost] Bot Host \"%s\": brain \"%s\" can run %d "
                   "bots on this computer; slots %d -> %d", optName,
                   embedBrain, cap, optSlots, cap);
        optSlots = cap;
    }
}

static void syncFinishIfDone(HostServer *sv) {
    HostSync *hs = &sv->sync;
    int b;
    if (hs->ready || hs->gotCount < hs->needCount) return;
    if (hs->packPending > 0) return;
    for (b = 0; b < hs->man.brainCount; b++) {
        if (!hs->staged[b]) continue;
        if (!brainSyncCommitStage(hs->root, &hs->man.brains[b])) {
            fprintf(stderr, "[bothost] the files of brain \"%s\" could not be "
                            "moved into place; this host waits for its next "
                            "brain file list\n", hs->man.brains[b].name);
            return;
        }
        hs->staged[b] = false;
    }
    hs->ready = true;
    cacheListDirty = true;
    if (hs->packMode) {
        fprintf(stderr, "[bothost] brain files from %s ready (generation %u): "
                        "%u bytes of files, %u bytes on the wire, %d from "
                        "the install, %d from the cache, %d received, in %u "
                        "ms\n",
                sv->label, (unsigned)hs->gen, (unsigned)hs->done,
                (unsigned)hs->packWire, hs->fromInstall, hs->fromCache,
                hs->needCount, (unsigned)(nowMs() - hs->syncStartMs));
    } else {
        fprintf(stderr, "[bothost] brain files from %s ready (generation %u): "
                        "%u bytes downloaded (%u bytes on the wire) in %u "
                        "ms\n",
                sv->label, (unsigned)hs->gen, (unsigned)hs->done,
                (unsigned)hs->packWire, (unsigned)(nowMs() - hs->syncStartMs));
    }
    syncCapacityIfOwed(sv);
    syncSendProgress(sv);
}

/* Take a new manifest m (its brains and files become hs->man) and start
 * its download, as the host always did. */
static void syncProcessManifest(HostServer *sv, BrainSyncManifest m,
                                uint32_t gen) {
    HostSync *hs = &sv->sync;
    BrainSyncPlan plan;
    int b;

    syncDropFile(hs);
    brainSyncFree(&hs->man);
    free(hs->need);
    hs->need = NULL;
    hs->installOnly = false;
    hs->refused = false;
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
    syncDropReaders(hs);
    hs->packWire = 0;
    hs->packMode = hs->man.haveSha;
    hs->fromInstall = hs->fromCache = 0;
    memset(hs->inPlace, 0, sizeof(hs->inPlace));
    memset(hs->staged, 0, sizeof(hs->staged));
    memset(&plan, 0, sizeof(plan));
    if (!syncRootFor(hs->root, sizeof(hs->root))) {
        fprintf(stderr, "[bothost] cannot make a brain session folder under "
                        "\"%s\"\n", optCacheDir);
        return;
    }
    if (hs->packMode) {
        syncInstallRoot(hs->installRoot, sizeof(hs->installRoot));
        if (!brainSyncPlanPacked(&hs->man, hs->root,
                                 hs->installRoot[0] != '\0' ? hs->installRoot
                                                            : NULL,
                                 &plan)) {
            fprintf(stderr, "[bothost] cannot check the brain cache at "
                            "\"%s\"\n", hs->root);
            free(plan.need);
            return;
        }
    } else if (!brainSyncPlanCache(&hs->man, hs->root, &plan)) {
        fprintf(stderr, "[bothost] cannot check the brain cache at \"%s\"\n",
                hs->root);
        free(plan.need);
        return;
    }
    hs->need = plan.need;
    hs->needCount = plan.needCount;
    hs->total = plan.needBytes;
    fprintf(stderr, "[bothost] brain file list from %s, generation %u (%s): "
                    "%d brains; %d files held, %d copied from another "
                    "version, %d from the install, %d to download (%u "
                    "bytes)\n",
            sv->label, (unsigned)gen, hs->packMode ? "packed" : "one by one",
            hs->man.brainCount, plan.haveCount, plan.copyCount,
            plan.installCount, plan.needCount, (unsigned)plan.needBytes);
    if (hs->packMode) {
        hs->fromInstall = plan.installCount;
        hs->fromCache = plan.haveCount + plan.copyCount;
        for (b = 0; b < hs->man.brainCount; b++) {
            const BrainSyncBrain *br = &hs->man.brains[b];
            ControlEvent evt;
            hs->inPlace[b] = plan.inPlace[b];
            hs->staged[b] = plan.staged[b];
            brainPackBitsFromPlan(&plan, b, hs->packBits[b]);
            memset(hs->packGot[b], 0, sizeof(hs->packGot[b]));
            hs->packSeq[b] = 0;
            if (brainPackBitCount(hs->packBits[b], br->fileCount) > 0) {
                hs->readers[b] = brainPackReaderNew(br, (uint8_t)b,
                                                    hs->packBits[b]);
                if (hs->readers[b] != NULL) hs->packPending++;
            }
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_BOT_HOST_PACK_REQ;
            evt.u.botHostPackReq.gen = gen;
            evt.u.botHostPackReq.brain = (uint8_t)b;
            evt.u.botHostPackReq.fileCount = (uint16_t)br->fileCount;
            evt.u.botHostPackReq.fromInstall = (uint16_t)plan.installOf[b];
            evt.u.botHostPackReq.fromCache = (uint16_t)plan.cacheOf[b];
            /* A reader that could not be made: an empty bitmap, and the
             * files come one at a time below. */
            if (hs->readers[b] != NULL) {
                memcpy(evt.u.botHostPackReq.bits, hs->packBits[b],
                       sizeof(evt.u.botHostPackReq.bits));
            }
            ctlSendEvent(sv, &evt);
        }
        /* Every file is asked for in the streams; one that fails is asked
         * for again the old way once every stream has ended. */
        hs->nextReq = hs->needCount;
        if (hs->packPending == 0) {
            hs->nextReq = 0;
        }
        /* Nothing to keep: the brains run from the install. */
        if (HOST_EMBED && hs->needCount == 0) {
            bool anyStaged = false;
            for (b = 0; b < hs->man.brainCount; b++) {
                if (hs->staged[b] || !hs->inPlace[b]) anyStaged = true;
            }
            if (!anyStaged) syncSessionEnd();
        }
    }
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

/* Forget the current manifest and its transfer, but not the bulk
 * receiver (this may run inside its callback). */
static void syncDropManifest(HostServer *sv) {
    HostSync *hs = &sv->sync;
    syncDropReaders(hs);
    syncDropFile(hs);
    brainSyncFree(&hs->man);
    free(hs->need);
    hs->need = NULL;
    hs->needCount = hs->nextReq = hs->gotCount = 0;
    hs->haveManifest = false;
    hs->ready = false;
    hs->installOnly = false;
    hs->refused = false;
    hs->done = hs->total = 0;
    memset(hs->inPlace, 0, sizeof(hs->inPlace));
    memset(hs->staged, 0, sizeof(hs->staged));
}

/* True when brain name in the install folder root is, file for file, the
 * one this build shipped (brainShippedTable). Else one line names the file
 * and the brain is not run here, as for a brain that differs from the
 * server's. A build without the table (not official-only) takes any. */
static bool syncShippedSame(const char *root, const char *name) {
#if HOST_SHIPPED_TABLE
    char file[BRAIN_SYNC_PATH_MAX];
    BrainSyncShippedResult r = brainSyncShippedCheck(
        brainShippedTable, brainShippedCount, root, name, file, sizeof(file));
    if (r == BRAIN_SYNC_SHIPPED_OK) return true;
    if (file[0] != '\0') {
        HOST_EVENT("[bothost] Bot Host \"%s\": brain \"%s\" is not run: "
                   "file \"%s\" is %s; this client runs only the brain "
                   "files it shipped with", optName, name, file,
                   brainSyncShippedResultText(r));
    } else {
        HOST_EVENT("[bothost] Bot Host \"%s\": brain \"%s\" is not run: "
                   "the brain is %s; this client runs only the brain files "
                   "it shipped with", optName, name,
                   brainSyncShippedResultText(r));
    }
    return false;
#else
    (void)root;
    (void)name;
    return true;
#endif
}

/* A manifest of the official brains only (the server's -bothost-code
 * official, or HOST_OFFICIAL_ONLY): no file is asked for. Each brain runs
 * from the install when the install holds it file for file (SHA-256), else
 * not at all. A v1 list (no SHA-256) or a debug-mode one (a debug brain
 * writes into its own folder) is refused: no bot runs here for it. */
static void syncInstallOnlyManifest(HostServer *sv, BrainSyncManifest m,
                                    uint32_t gen) {
    HostSync *hs = &sv->sync;
    int b, held = 0;

    syncDropManifest(sv);
    hs->man = m;
    hs->gen = gen;
    hs->haveManifest = true;
    hs->installOnly = true;
    hs->packMode = true;
    hs->syncStartMs = nowMs();
    hs->retries = 0;
    hs->packWire = 0;
    hs->fromInstall = hs->fromCache = 0;
    /* No server byte is written in this mode: no folder to write into. */
    hs->root[0] = '\0';
    if (!m.haveSha || m.debugMode) {
        /* Refused: nothing of this manifest is taken, not even the files a
         * server sends unasked (syncTakesData). */
        hs->refused = true;
        hs->packMode = false;
        HOST_EVENT("[bothost] Bot Host \"%s\": the brain file list from %s "
                   "is %s; this host runs only the brains of its own install, "
                   "so it runs no bot for this server", optName, sv->label,
                   !m.haveSha ? "the old form (no SHA-256)"
                              : "for a -brain-debug server");
        syncSendProgress(sv);
        return;
    }
    syncInstallRoot(hs->installRoot, sizeof(hs->installRoot));
    for (b = 0; b < hs->man.brainCount; b++) {
        const BrainSyncBrain *br = &hs->man.brains[b];
        ControlEvent evt;
        hs->inPlace[b] = hs->installRoot[0] != '\0' &&
                         brainSyncInstallMatches(hs->installRoot, br) &&
                         (!HOST_OFFICIAL_ONLY ||
                          syncShippedSame(hs->installRoot, br->name));
        if (hs->inPlace[b]) {
            held++;
            hs->fromInstall += br->fileCount;
        }
        /* No file is asked for: an empty bitmap. Every file from the
         * install tells the server this brain runs here. */
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_BOT_HOST_PACK_REQ;
        evt.u.botHostPackReq.gen = gen;
        evt.u.botHostPackReq.brain = (uint8_t)b;
        evt.u.botHostPackReq.fileCount = (uint16_t)br->fileCount;
        evt.u.botHostPackReq.fromInstall =
            hs->inPlace[b] ? (uint16_t)br->fileCount : 0;
        ctlSendEvent(sv, &evt);
        fprintf(stderr, "[bothost]   brain \"%s\": %s\n", br->name,
                hs->inPlace[b] ? "the same as this install; it runs from there"
                               : "not the same as this install; not run here");
    }
    HOST_EVENT("[bothost] Bot Host \"%s\": official brains only; %d of the "
               "%d brains of %s (generation %u) are the same as this install; "
               "no file is downloaded", optName, held, hs->man.brainCount,
               sv->label, (unsigned)gen);
    syncFinishIfDone(sv);
}

#ifdef WINBOLO_BOTHOST_EMBED
/* True when running manifest m means running code that is not the
 * install's: a v1 list, or a brain whose files differ from the install. */
static bool syncNeedsConsent(const BrainSyncManifest *m) {
    char root[600];
    int b;
    if (!m->haveSha) return true;
    syncInstallRoot(root, sizeof(root));
    if (root[0] == '\0') return m->brainCount > 0;
    for (b = 0; b < m->brainCount; b++) {
        if (!brainSyncInstallMatches(root, &m->brains[b])) return true;
    }
    return false;
}
#endif

/* Keep manifest m waiting (why: SYNC_PEND_*): no file is asked for and no
 * brain code runs; the host offers 0 slots (syncOffersNone). */
static void syncPend(HostServer *sv, BrainSyncManifest *m, uint32_t gen,
                     int why) {
    HostSync *hs = &sv->sync;
    syncDropManifest(sv);
    brainSyncFree(m);
    hs->gen = gen;
    hs->pending = true;
    hs->pendWhy = why;
    syncSendProgress(sv);
}

static void syncOnManifest(HostServer *sv, const uint8_t *buf, uint32_t len,
                           uint32_t gen) {
    HostSync *hs = &sv->sync;
    BrainSyncManifest m;

    if (!brainSyncManifestDecode(buf, len, &m)) {
        fprintf(stderr, "[bothost] the brain file list from %s is damaged; "
                        "ignored\n", sv->label);
        return;
    }
    if (HOST_EMBED && buf != hs->lastBuf) {
        /* Kept, so it can wait for the player. */
        uint8_t *copy = (uint8_t *)malloc(len > 0 ? len : 1);
        if (copy != NULL) memcpy(copy, buf, len);
        free(hs->lastBuf);
        hs->lastBuf = copy;
        hs->lastLen = copy != NULL ? len : 0;
        hs->lastGen = gen;
    }
    hs->pending = false;
    hs->pendWhy = 0;
    if (HOST_OFFICIAL_ONLY || m.noCode) {
        syncInstallOnlyManifest(sv, m, gen);
        return;
    }
    if (HOST_EMBED && hostWithheld()) {
        /* Bots are off on this computer: nothing is downloaded. */
        HOST_EVENT("[bothost] Bot Host \"%s\": bots are off on this "
                   "computer; the brain files of %s are not downloaded",
                   optName, sv->label);
        syncPend(sv, &m, gen, SYNC_PEND_WITHHELD);
        return;
    }
#ifdef WINBOLO_BOTHOST_EMBED
    if (syncNeedsConsent(&m)) {
        int c = SDL_GetAtomicInt(&embedConsent);
        if (c != BOT_HOST_EMBED_CONSENT_YES) {
            if (c == BOT_HOST_EMBED_CONSENT_NONE) {
                SDL_SetAtomicInt(&embedConsent, BOT_HOST_EMBED_CONSENT_ASKING);
                HOST_EVENT("[bothost] Bot Host \"%s\": %s wants to send its "
                           "own bot code; the player is asked first", optName,
                           sv->label);
            }
            syncPend(sv, &m, gen, SYNC_PEND_CONSENT);
            return;
        }
    }
#endif
    syncProcessManifest(sv, m, gen);
}

/* The Bot Host inside a client: a manifest that waits goes on once its
 * reason is gone, and a download stops when the player turns bots off. */
static void syncServicePending(HostServer *sv) {
#ifdef WINBOLO_BOTHOST_EMBED
    HostSync *hs = &sv->sync;
    if (hs->pending && hs->lastBuf != NULL) {
        bool go = false;
        if (hs->pendWhy == SYNC_PEND_WITHHELD) go = !hostWithheld();
        if (hs->pendWhy == SYNC_PEND_CONSENT) {
            go = !hostWithheld() && SDL_GetAtomicInt(&embedConsent) ==
                                        BOT_HOST_EMBED_CONSENT_YES;
        }
        if (go) {
            syncOnManifest(sv, hs->lastBuf, hs->lastLen, hs->lastGen);
        }
        return;
    }
    if (hostWithheld() && hs->haveManifest && !hs->ready &&
        !hs->installOnly && hs->lastBuf != NULL) {
        ControlEvent caps;
        HOST_EVENT("[bothost] Bot Host \"%s\": bots are off on this "
                   "computer; the brain file download from %s stops",
                   optName, sv->label);
        syncDropManifest(sv);
        hs->pending = true;
        hs->pendWhy = SYNC_PEND_WITHHELD;
        memset(&caps, 0, sizeof(caps));
        caps.type = CTRL_BOT_HOST_SYNC_CAPS;
        caps.u.botHostSyncCaps.caps =
            (uint8_t)(hostSyncCaps() | BRAIN_PACK_CAP_RESET);
        ctlSendEvent(sv, &caps);
        syncSendProgress(sv);
    }
#else
    (void)sv;
#endif
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

/* True when the host may take brain file data of kind
 * (BOT_HOST_SYNC_DATA_*) for generation gen from sv: never on a host that
 * runs only its install's brains (HOST_OFFICIAL_ONLY, or an install-only
 * or refused manifest). Checked before a piece is taken in and again
 * before it is written. */
static bool syncTakesData(const HostServer *sv, int kind, uint32_t gen) {
    const HostSync *hs = &sv->sync;
    BotHostSyncTake t;
    t.officialOnly = HOST_OFFICIAL_ONLY != 0;
    t.installOnly = hs->installOnly;
    t.refused = hs->refused;
    t.haveManifest = hs->haveManifest;
    t.ready = hs->ready;
    t.packMode = hs->packMode;
    t.gen = hs->gen;
    return botHostSyncTakesData(&t, kind, gen);
}

static void syncOnChunk(HostServer *sv, const BulkStreamHeader *h,
                        const uint8_t *buf) {
    HostSync *hs = &sv->sync;
    const BrainSyncBrain *br;
    const BrainSyncFile *f;
    uint32_t bi, fi, off, id;
    int64_t n;
    char path[1024];

    if (!syncTakesData(sv, BOT_HOST_SYNC_DATA_CHUNK, h->gen)) return;
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
    hs->packWire += h->totalSize;
    if (hs->fileGot < f->size) {
        syncSendProgress(sv);
        return;
    }
    if (hs->packMode) {
        char stage[1100];
        if (!brainSyncDataIs(f, hs->fileBuf, f->size) ||
            !brainSyncStageDir(stage, sizeof(stage), hs->root, br->name,
                               br->version) ||
            (size_t)SDL_snprintf(path, sizeof(path), "%s/%s", stage, f->path)
                >= sizeof(path) ||
            !brainSyncWriteFile(path, hs->fileBuf, f->size)) {
            fprintf(stderr, "[bothost] brain file \"%s/%s\" failed its check "
                            "or could not be written\n", br->name, f->path);
            syncRetryFile(sv, id);
            return;
        }
        hs->staged[bi] = true;
    } else if (brainSyncHash(BRAIN_SYNC_HASH_SEED, hs->fileBuf, f->size) != f->hash ||
        !brainSyncCacheFile(path, sizeof(path), hs->root, br->name,
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

typedef struct {
    HostServer *sv;
    int         brain;
} SyncPackCtx;

/* One whole file of a packed stream: check it against the v2 manifest and
 * write it into the brain's stage folder. A file that fails is left for
 * the one-at-a-time way. */
static bool syncPackFile(void *ctx, int file, const uint8_t *data,
                         uint32_t size) {
    SyncPackCtx *pc = (SyncPackCtx *)ctx;
    HostSync *hs = &pc->sv->sync;
    const BrainSyncBrain *br = &hs->man.brains[pc->brain];
    const BrainSyncFile *f;
    char stage[1100], path[1400];
    if (file < 0 || file >= br->fileCount) return false;
    f = &br->files[file];
    if (!brainSyncDataIs(f, data, size)) {
        fprintf(stderr, "[bothost] brain file \"%s/%s\" from %s failed its "
                        "check; it is asked for again\n", br->name, f->path,
                pc->sv->label);
        return false;
    }
    if (!brainSyncStageDir(stage, sizeof(stage), hs->root, br->name,
                           br->version) ||
        (size_t)SDL_snprintf(path, sizeof(path), "%s/%s", stage, f->path)
            >= sizeof(path) ||
        !brainSyncWriteFile(path, data, size)) {
        fprintf(stderr, "[bothost] brain file \"%s/%s\" could not be "
                        "written\n", br->name, f->path);
        return false;
    }
    brainPackBitSet(hs->packGot[pc->brain], file);
    hs->staged[pc->brain] = true;
    hs->gotCount++;
    hs->done += size;
    return true;
}

/* Every stream has ended: the files that did not come or failed their
 * checks go to the end of need[] and are asked for one at a time. */
static void syncPackFallback(HostServer *sv) {
    HostSync *hs = &sv->sync;
    int i, got = 0, missing;
    uint32_t *order = (uint32_t *)malloc(sizeof(uint32_t) *
                                         (size_t)(hs->needCount > 0 ? hs->needCount : 1));
    if (order == NULL) return;
    for (i = 0; i < hs->needCount; i++) {
        uint32_t id = hs->need[i];
        if (brainPackBitGet(hs->packGot[id >> 16], (int)(id & 0xffffu))) {
            order[got++] = id;
        }
    }
    missing = got;
    for (i = 0; i < hs->needCount; i++) {
        uint32_t id = hs->need[i];
        if (!brainPackBitGet(hs->packGot[id >> 16], (int)(id & 0xffffu))) {
            order[missing++] = id;
        }
    }
    memcpy(hs->need, order, sizeof(uint32_t) * (size_t)hs->needCount);
    free(order);
    hs->gotCount = got;
    hs->nextReq = got;
    if (got < hs->needCount) {
        fprintf(stderr, "[bothost] %d brain files from %s did not come in the "
                        "packed stream; they are asked for one at a time\n",
                hs->needCount - got, sv->label);
        syncRequestMore(sv);
    }
}

/* Parse "<brain>/<seq>". */
static bool syncParsePackPath(const char *p, uint32_t *bi, uint32_t *seq) {
    char *end = NULL;
    if (*p < '0' || *p > '9') return false;
    *bi = (uint32_t)strtoul(p, &end, 10);
    if (end == p || *end != '/') return false;
    p = end + 1;
    if (*p < '0' || *p > '9') return false;
    *seq = (uint32_t)strtoul(p, &end, 10);
    return end != p && *end == '\0';
}

static void syncOnPack(HostServer *sv, const BulkStreamHeader *h,
                       const uint8_t *buf) {
    HostSync *hs = &sv->sync;
    uint32_t bi, seq;
    BrainPackStatus st;
    SyncPackCtx pc;

    if (!syncTakesData(sv, BOT_HOST_SYNC_DATA_PACK, h->gen)) return;
    if (!syncParsePackPath(h->path, &bi, &seq) ||
        bi >= (uint32_t)hs->man.brainCount || hs->readers[bi] == NULL) {
        return;
    }
    hs->packWire += h->totalSize;
    pc.sv = sv;
    pc.brain = (int)bi;
    if (seq != hs->packSeq[bi]) {
        st = BRAIN_PACK_BAD;
    } else {
        hs->packSeq[bi]++;
        st = brainPackReaderFeed(hs->readers[bi], buf, h->totalSize,
                                 syncPackFile, &pc);
    }
    if (st != BRAIN_PACK_MORE) {
        if (st == BRAIN_PACK_BAD) {
            fprintf(stderr, "[bothost] the packed files of brain \"%s\" from "
                            "%s are damaged\n", hs->man.brains[bi].name,
                    sv->label);
        }
        brainPackReaderFree(hs->readers[bi]);
        hs->readers[bi] = NULL;
        hs->packPending--;
        if (hs->packPending == 0) syncPackFallback(sv);
    }
    syncSendProgress(sv);
    syncFinishIfDone(sv);
}

static uint8_t *syncBulkBegin(void *ctx, const BulkStreamHeader *h) {
    const HostServer *sv = (const HostServer *)ctx;
    /* Stopped for this join: no brain files. */
    if (hostStopped()) return NULL;
    if (h->kind == BULK_KIND_BRAIN_MANIFEST) {
        if (h->totalSize == 0 || h->totalSize > BRAIN_SYNC_MANIFEST_MAX) {
            return NULL;
        }
    } else if (h->kind == BULK_KIND_BRAIN_CHUNK) {
        if (h->totalSize < 4 || h->totalSize > BOT_HOST_SYNC_CHUNK_WIRE_MAX ||
            !syncTakesData(sv, BOT_HOST_SYNC_DATA_CHUNK, h->gen)) {
            return NULL;
        }
    } else if (h->kind == BULK_KIND_BRAIN_PACK) {
        if (h->totalSize == 0 || h->totalSize > BRAIN_PACK_PIECE_MAX ||
            !syncTakesData(sv, BOT_HOST_SYNC_DATA_PACK, h->gen)) {
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
    if (hostStopped()) {
        free(buf);
        return;
    }
    if (h->kind == BULK_KIND_BRAIN_MANIFEST) {
        syncOnManifest(sv, buf, h->totalSize, h->gen);
    } else if (h->kind == BULK_KIND_BRAIN_CHUNK) {
        syncOnChunk(sv, h, buf);
    } else if (h->kind == BULK_KIND_BRAIN_PACK) {
        syncOnPack(sv, h, buf);
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
            hostCeiling());
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
    if (br == NULL || !syncBrainInitPath(sync, br, path, sizeof(path))) {
        fprintf(stderr, "[bothost] bot %d: the brain \"%s\" of %s is not in "
                        "the cache; it goes back to the server\n",
                (int)hs->seat, hs->brainName, servers[hs->srv].label);
        hs->buildFailed = true;
        return;
    }
    if (sync->installOnly && !sync->inPlace[br - sync->man.brains]) {
        HOST_EVENT("[bothost] bot %d: brain \"%s\" of %s is not the same as "
                   "this install; it goes back to the server", (int)hs->seat,
                   hs->brainName, servers[hs->srv].label);
        hs->buildFailed = true;
        return;
    }
    if (HOST_EMBED && !HOST_ALLOW_FULL_MAP && (aiType)hs->ai != aiYes) {
        /* Built with WINBOLO_BOTHOST_ALLOW_FULL_MAP OFF: a bot that sees
         * the whole map never runs on a player's computer, because the
         * player could read what it sees. */
        HOST_EVENT("[bothost] bot %d: its AI level sees the whole map; it "
                   "goes back to the server", (int)hs->seat);
        hs->buildFailed = true;
        return;
    }
    if (HOST_EMBED && sync->installOnly &&
        (!botHostInitArgAllowed(hs->initArg) ||
         !botHostInitTableAllowed(&hs->bot.initTable))) {
        HOST_EVENT("[bothost] bot %d: its init tokens are not on the list "
                   "this host takes; it goes back to the server",
                   (int)hs->seat);
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

    /* Both slots are per thread (THREAD_LOCAL in luabrainshandler.c): the
     * game thread's own bots stage theirs on that thread, so the two never
     * meet. The create below takes them on this thread. */
    luaBrainsSetNextStartEngineTick((unsigned int)hs->startTick);
    luaBrainsSetNextInitArg(hs->initArg);
    fprintf(stderr, "Bot %d: brain init arg '%s'\n", (int)hs->seat,
            hs->initArg);
    if (!(HOST_EMBED
              /* A player's computer: never --allow-unsafe-brains, and a
               * debug brain only with -bothost-record. */
              ? luaBrainInstanceCreateSandboxed(
                    &hs->bot.brain, path, hs->brainName, hs->cs,
                    (aiType)hs->ai, sync->man.debugMode && embedRecord,
                    (int)hs->seat, &hs->bot.initTable)
              : luaBrainInstanceCreate(&hs->bot.brain, path, hs->brainName,
                                       hs->cs, (aiType)hs->ai,
                                       sync->man.debugMode, (int)hs->seat,
                                       &hs->bot.initTable))) {
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
    if (newPlacement && syncOffersNone(&sv->sync)) {
        /* This host may run none of the server's bots now. */
        fprintf(stderr, "[bothost] bot %d of %s turned down: this host "
                        "offers no slot now\n", (int)s, sv->label);
        if (hs != NULL) seatClear(hs, "this host offers no slot now");
        ctlSendSlots(sv, s, evt->u.botHostAssign.gen);
        return;
    }
    if (newPlacement) {
        bool wasUsed = hs != NULL;
        if (hs == NULL) hs = seatTakeFree(srv);
        if (hs == NULL) {
            /* Every slot is taken (another server was first): the seat
             * stays in-process on its server. */
            fprintf(stderr, "[bothost] bot %d of %s turned down: all %d "
                            "slots are taken\n", (int)s, sv->label,
                    capFor(srv));
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
    if (HOST_EMBED && servers[serverIndex(sv)].sync.installOnly &&
        !botHostInitTableAllowed(&hs->tableIn)) {
        /* Tokens off the list this host takes (bot_host_guard.h): the
         * brain does not read them; the bot goes back to the server. */
        HOST_EVENT("[bothost] bot %d \"%s\": its init table holds tokens "
                   "this host does not take; it goes back to the server",
                   (int)s, hs->name);
        scnTableClear(&hs->tableIn);
        hs->awaitTable = false;
        hs->buildFailed = true;
        return;
    }
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
    if (!botHostSaltLimitTake(&sv->saltLimit, nowMs())) {
        /* Too many, or too soon after the last: a server gets few hashes
         * of this computer (bot_host_guard.h). */
        fprintf(stderr, "[bothost] %s: a same-computer salt over the limit "
                        "for this connection; no answer\n", sv->label);
        return;
    }
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

/* The server knows dynamic slots: from now on it is offered the ceiling,
 * and it is sent the start, ceiling and reserve (the service sends them
 * again when one changes). */
static void onLimits(HostServer *sv, const ControlEvent *evt) {
    (void)evt;
    if (!sv->limHello) {
        HOST_EVENT("[bothost] Bot Host \"%s\": %s has dynamic slots; this "
                   "host starts at %d and goes up to %d", optName, sv->label,
                   optSlots < hostCeiling() ? optSlots : hostCeiling(),
                   hostCeiling());
    }
    sv->limHello = true;
    sv->lastLimitsSent = -1;
}

/* The server is on this computer: the host stops for this join. Its bots
 * (none should be here) go back, the brain files on the way are dropped
 * and the session folder goes; 0 slots go out on the next service
 * (poolSlotsFor). The connection stays, so the lobby keeps the row. */
static void onSameServer(HostServer *sv, const ControlEvent *evt) {
#ifdef WINBOLO_BOTHOST_EMBED
    if (!botHostRunSameServer(&embedRun, evt->u.botHostSameServer.kind)) {
        return;
    }
    HOST_EVENT("[bothost] Bot Host \"%s\": the server %s is on this "
               "computer; this Bot Host stops for this join (no brain files, "
               "no CPU readings, 0 slots)", optName, sv->label);
    clearSeats(serverIndex(sv), "same computer as the server");
    syncReset(&sv->sync);
    syncSessionEnd();
    SDL_SetAtomicInt(&embedSameServerNow, 1);
#else
    (void)sv;
    (void)evt;
#endif
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
        case CTRL_BOT_HOST_SAME_SERVER:  onSameServer(sv, &evt);  break;
        case CTRL_BOT_HOST_LIMITS:       onLimits(sv, &evt);      break;
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
    syncSessionEnd();
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
                    sv->limHello = false;
                    sv->lastLimitsSent = -1;
                    sv->lastCpuSent = -1;
                    sv->lastCpuBotSent = 0;
                    sv->lastCpuBudgetSent = 0;
                    sv->lastFreeSeqSent = 0;
                    syncReset(&sv->sync);
                    botHostSaltLimitInit(&sv->saltLimit);
                    bulkSenderInit(&sv->recBulk);
                    hostRecServerUp(serverIndex(sv), sv->label);
                    {
                        /* The packed brain sync; WINBOLO_BOTHOST_SYNC_V1=1
                         * keeps the old way (the server waits a moment for
                         * caps that never come, as from an older host). */
                        const char *v1 = SDL_getenv("WINBOLO_BOTHOST_SYNC_V1");
                        if (v1 == NULL || SDL_strcmp(v1, "1") != 0) {
                            ControlEvent caps;
                            memset(&caps, 0, sizeof(caps));
                            caps.type = CTRL_BOT_HOST_SYNC_CAPS;
                            caps.u.botHostSyncCaps.caps = hostSyncCaps();
                            ctlSendEvent(sv, &caps);
                        }
                    }
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
            syncServicePending(sv);
            syncCapacityIfOwed(sv);
            if (sv->limHello && hostLimitsPacked() != sv->lastLimitsSent) {
                ctlSendLimits(sv);
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
            if (hostCpuBudgetMilli != BOT_HOST_CPU_MILLI_UNKNOWN &&
                hostCpuBudgetMilli != sv->lastCpuBudgetSent) {
                /* The full-budget estimate, which the server shows until a
                 * measured value exists. Only the number goes out. */
                ControlEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = CTRL_BOT_HOST_CPU_BUDGET;
                evt.u.botHostCpuBudget.milli = hostCpuBudgetMilli;
                ctlSendEvent(sv, &evt);
                sv->lastCpuBudgetSent = hostCpuBudgetMilli;
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

static double serialMsEwma = 0.0;
static double lastTargetMs = 0.0;
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
    SDL_strlcpy(cmd.u.botStats.hostName, optStatsName,
                sizeof(cmd.u.botStats.hostName));
    clientSimSubmitCommand(hs->cs, &cmd);
}

/* Bot connections: keep them ticking, notice when they end, and build the
 * brains that are owed. */
static void seatsService(void) {
    int i;
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
        }
    }
}

static void hostTick(void) {
    int i, active = 0, runners;
    bool anyFirst = false;
    Uint64 setupStart, brainStart, brainEnd, sendEnd;
    double freq = (double)SDL_GetPerformanceFrequency();

    setupStart = SDL_GetPerformanceCounter();
    seatsService();

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
    lastTargetMs = botRunnerTargetMs(serialMsEwma, runners, active);
    {
        /* The budget a new bot would get: one more than the bots this host
         * runs now. The brain picks its tier from this budget; the budget
         * does not depend on the tier. */
        uint64_t tickMs = SDL_GetTicks();
        if (hostCpuBudgetMilli == BOT_HOST_CPU_MILLI_UNKNOWN ||
            tickMs - hostCpuBudgetAtMs >= BOT_HOST_BUDGET_EVERY_MS) {
            double newBotMs = botRunnerTargetMs(serialMsEwma, runners,
                                                poolUsed() + 1);
            hostCpuBudgetMilli = botHostCpuMilliBudget(
                newBotMs, (double)BOT_HOST_TICK_MS,
                SDL_GetNumLogicalCPUCores());
            hostCpuBudgetAtMs = tickMs;
        }
    }
    for (i = 0; i < active; i++) {
        HostSeat *hs = &seats[jobIndices[i]];
        botRunnerBeginTick(&hs->bot, lastTargetMs);
        if (hs->bot.brain.isFirst) anyFirst = true;
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
        if (!hs->used || hs->bot.brain.L == NULL || hs->job.wasKilled ||
            hs->job.needRemove) {
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
                         rrCount);
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

    /* Stopped for this join: no CPU readings. */
    if (!hostStopped()) {
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
    }

    sendEnd = SDL_GetPerformanceCounter();
    {
        double serialMs = ((double)(brainStart - setupStart) +
                           (double)(sendEnd - brainEnd)) * 1000.0 / freq;
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
            "                  together (default 4); with a server that has\n"
            "                  dynamic slots, the bots it starts a game with\n"
            "  --slots-max N   with dynamic slots, the most bots the server\n"
            "                  may raise this host to (default: --slots or\n"
            "                  logical CPUs - 1, the larger, at most %d)\n"
            "  --cpu-reserve P the free CPU percent this host tries to keep:\n"
            "                  the server gives it no raise and no new bot\n"
            "                  that would take it under P; the bots may still\n"
            "                  push the CPU past it when no other place has\n"
            "                  room (default %d)\n"
            "  --keel          --slots-max = --slots and --cpu-reserve 0\n"
            "                  (fixed slots, as before); a flag given as\n"
            "                  well still counts\n"
            "  --threads N     brain threads (default: one per CPU core)\n"
            "  --key KEY       the servers' -bothostkey, when they have one\n"
            "  --name NAME     the name the server operators and players see\n"
            "                  (default: Bot Host)\n"
            "  --cachedir DIR  where the servers' brain files are kept\n"
            "                  (default: bothost_cache next to this program)\n"
            "  --record-min-free-gb N  record the bots of a -brain-debug\n"
            "                  server only while the cache's disk has N GB\n"
            "                  free (default 50)\n",
            prog, BOT_HOST_DEFAULT_SERVER_PORT, BOT_HOST_DEFAULT_LISTEN_PORT,
            BOT_HOST_MAX_SLOTS, BOT_HOST_RAISE_RESERVE_PCT_DEFAULT);
}

static double optRecordMinFreeGb = 50.0;

static bool parseArgs(int argc, char **argv) {
    int i;
    bool haveServers = false, keel = false, haveReserve = false;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printUsage(argv[0]);
            return false;
        }
        if (strcmp(a, "--keel") == 0) {
            keel = true;
            continue;
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
        } else if (strcmp(a, "--slots-max") == 0) {
            optSlotsMax = atoi(v);
            if (optSlotsMax < 1 || optSlotsMax > BOT_HOST_MAX_SLOTS) {
                fprintf(stderr, "--slots-max takes 1 to %d\n",
                        BOT_HOST_MAX_SLOTS);
                return false;
            }
        } else if (strcmp(a, "--cpu-reserve") == 0) {
            if (!botHostParsePercent(v, &optCpuReserve)) {
                fprintf(stderr, "--cpu-reserve takes 0 to 100\n");
                return false;
            }
            haveReserve = true;
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
        /* Every player sees the name, so the computer's own name is used
         * only when the owner gives it with --name. */
        SDL_strlcpy(optName, "Bot Host", sizeof(optName));
    }
    SDL_strlcpy(optStatsName, optName, sizeof(optStatsName));
    /* The KEEL values: fixed slots, no reserve. */
    if (keel && !haveReserve) optCpuReserve = 0;
    optSlotsMax = botHostSlotsMaxDefault(optSlotsMax, optSlots,
                                         SDL_GetNumLogicalCPUCores(), keel);
    if (optSlotsMax < optSlots) optSlotsMax = optSlots;
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
                    "server%s, listening on UDP port %u; with dynamic slots "
                    "up to %d, trying to keep %d%% CPU free\n",
            optName, optSlots, serverCount, serverCount == 1 ? "" : "s",
            (unsigned)optListenPort, optSlotsMax, optCpuReserve);
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
/* embedRecord as it was when this host started: it set the recording up. */
static bool          embedRecordOn;
static int           embedSavedParts[4];

/* The longest a brain's chunk run and capacity() may take together when
 * this host asks a brain how many bots it can run. It runs on the host's
 * thread, not the game's, and a stop of the host ends it at once
 * (embedStop), so the limit is generous: a slow computer must not lose a
 * stock brain's answer. */
#define BOT_HOST_CAPACITY_BUDGET_MS 10000u

static int embedCapacityFor(const HostServer *sv) {
    BrainMachineInfo machine;
    const BrainSyncBrain *br = NULL;
    char path[512];
    int cap = BRAIN_CAPACITY_UNKNOWN;

    brainCapacityGetMachine(&machine);
    if (sv->sync.haveManifest) {
        /* Only the ticket's own brain: one not in the list gives the
         * default answer. */
        br = brainSyncFindBrain(&sv->sync.man, embedBrain);
    }
    if (br != NULL && syncBrainInitPath(&sv->sync, br, path, sizeof(path))) {
        /* The answer is kept by brain name and version, so the brain is
         * asked once. */
        cap = luaBrainQueryCapacityLimited(path, br->name,
                                           BOT_HOST_CAPACITY_BUDGET_MS,
                                           &embedStop);
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
    syncSessionEnd();
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
    /* A new join: the player is asked again if the server sends code. */
    SDL_SetAtomicInt(&embedConsent, BOT_HOST_EMBED_CONSENT_NONE);
    /* A new join: the host runs as normal, and the server checks again. */
    botHostRunJoin(&embedRun);
    SDL_SetAtomicInt(&embedSameServerNow, 0);
    embedSendQuit = true;
    if (cfg->recordMinFreeGb > 0.0) embedRecordMinFreeGb = cfg->recordMinFreeGb;
    /* Until the brain's own answer comes with its files: the default. */
    optSlots = embedSlotsFixed > 0 ? embedSlotsFixed : botHostEmbedDefaultSlots();
    SDL_strlcpy(optName, (cfg->name != NULL && cfg->name[0] != '\0')
                             ? cfg->name : "Player",
                sizeof(optName));
    SDL_strlcpy(optStatsName, cfg->name != NULL ? cfg->name : "",
                sizeof(optStatsName));
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
    /* Session folders a crash left (their lock is free); one another
     * running client holds is kept. */
    (void)brainSessionSweep(optCacheDir);

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
         * are the program's own, so they go back when the host stops.
         * Note: these luaBrains settings are for the whole process, not
         * for this host alone. While the host runs they also apply to any
         * other bots the client runs in its own process. The client runs
         * one Bot Host at a time; if other bots must ever record in
         * another way at the same time, these need per-host options. */
        static const HostRecIo io = { recIoSendEvent, recIoBulkIdle,
                                      recIoBulkSend };
        char recRoot[600];
        luaBrainsGetDebugParts(&embedSavedParts[0], &embedSavedParts[1],
                               &embedSavedParts[2], &embedSavedParts[3]);
        embedRecordOn = embedRecord;
        if (embedRecordOn) {
            /* Only with -bothost-record: a player's computer records
             * nothing by default. */
            SDL_snprintf(recRoot, sizeof(recRoot), "%s/recordings",
                         optCacheDir);
            luaBrainsSetDebugParts(0, 1, 1, 0);
            hostRecInit(optCacheDir, optName, embedRecordMinFreeGb, &io);
            luaBrainsSetExtraWriteRoot(recRoot);
        }
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
        if (embedRecordOn) {
            luaBrainsSetExtraWriteRoot("");
            luaBrainsSetDebugParts(embedSavedParts[0], embedSavedParts[1],
                                   embedSavedParts[2], embedSavedParts[3]);
        }
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
    if (embedRecordOn) {
        luaBrainsSetExtraWriteRoot("");
        luaBrainsSetDebugParts(embedSavedParts[0], embedSavedParts[1],
                               embedSavedParts[2], embedSavedParts[3]);
    }
    HOST_EVENT("[bothost] Bot Host \"%s\" stopped (%s)", optName,
               tellServer ? "the server was told" : "without a word");
}

bool botHostEmbedRunning(void) {
    return embedThread != NULL;
}

void botHostEmbedSweepSessions(const char *cacheDir) {
    int n;
    if (cacheDir == NULL || cacheDir[0] == '\0') return;
    n = brainSessionSweep(cacheDir);
    if (n > 0) {
        HOST_EVENT("[bothost] removed %d brain session folders left from an "
                   "earlier run", n);
    }
}

void botHostEmbedSetMax(int max) {
    int v;
    if (max < 0) max = BOT_HOST_MAX_AUTO;
    if (max > BOT_HOST_MAX_TOP) max = BOT_HOST_MAX_TOP;
    v = max + 1;
    if (SDL_GetAtomicInt(&embedMaxPlus1) == v) return;
    SDL_SetAtomicInt(&embedMaxPlus1, v);
    if (embedThread == NULL) return;
    if (max == 0) {
        HOST_EVENT("[bothost] Bot Host \"%s\": the player set Max slots 0; "
                   "it offers 0 slots", optName);
    } else if (max < 0) {
        HOST_EVENT("[bothost] Bot Host \"%s\": the player set Max slots to "
                   "the brain's answer", optName);
    } else {
        HOST_EVENT("[bothost] Bot Host \"%s\": the player set Max slots %d",
                   optName, max);
    }
}

int botHostEmbedGetMax(void) {
    return SDL_GetAtomicInt(&embedMaxPlus1) - 1;
}

void botHostEmbedGetStatus(BotHostEmbedStatus *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->running   = embedThread != NULL && SDL_GetAtomicInt(&embedAlive) != 0;
    out->connected = SDL_GetAtomicInt(&embedConnected) != 0;
    out->refused   = SDL_GetAtomicInt(&embedRefusedNow) != 0;
    out->slots     = SDL_GetAtomicInt(&embedSlotsNow);
    out->bots      = SDL_GetAtomicInt(&embedUsedNow);
    out->sameServer = SDL_GetAtomicInt(&embedSameServerNow) != 0;
    out->consentAsking = SDL_GetAtomicInt(&embedConsent) ==
                         BOT_HOST_EMBED_CONSENT_ASKING;
}

void botHostEmbedConsentAnswer(bool yes) {
    if (SDL_GetAtomicInt(&embedConsent) != BOT_HOST_EMBED_CONSENT_ASKING) {
        return;
    }
    SDL_SetAtomicInt(&embedConsent, yes ? BOT_HOST_EMBED_CONSENT_YES
                                        : BOT_HOST_EMBED_CONSENT_NO);
    HOST_EVENT("[bothost] Bot Host \"%s\": the player said %s to the "
               "server's own bot code for this join", optName,
               yes ? "yes" : "no");
}

void botHostEmbedSetRecord(bool on) {
    embedRecord = on;
}

bool botHostEmbedOfficialOnly(void) {
    return HOST_OFFICIAL_ONLY != 0;
}

#endif /* WINBOLO_BOTHOST_EMBED */
