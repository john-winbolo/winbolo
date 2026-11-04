/*
 * WinBoloBotHost - runs a WinBoloDS server's bot brains on another machine.
 *
 * The host keeps one tankless control connection to the server. The server
 * hands it bot seats over that connection (CTRL_BOT_HOST_ASSIGN with a
 * one-time token). For each seat the host opens a bot connection - a normal
 * UDP client connection that joins with JOIN_FLAG_BOT_SLOT and the token -
 * and runs the seat's brain against that connection's ClientSim, through the
 * same think path the server's in-process bot pool uses (botRunner*).
 *
 * Players never see the host or its connections: the server binds each bot
 * connection to the existing bot seat without a roster change.
 *
 * Usage:
 *   WinBoloBotHost --key KEY [--server ADDR] [--port N] [--slots N]
 *                  [--threads N] [--name NAME]
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
#include <SDL3/SDL_main.h>

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
#include "client_sim_internal.h"
#include "transport_control_codec.h"
#include "transport_udp.h"
#include "transport_udp_internal.h"
#include "../gui/lang.h"
#include "../gui/clientmutex.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../common/sentry_integration.h"
#include "../common/wb_log.h"
#include "../common/prefs.h"

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

#define BOT_HOST_DEFAULT_PORT      27500
#define BOT_HOST_TICK_MS           20      /* one think per 20 ms */
#define BOT_HOST_PING_MS           500     /* control connection keepalive */
#define BOT_HOST_JOIN_RETRY_MS     1000    /* join resend while joining */
#define BOT_HOST_SILENCE_MS        5000    /* no packet this long: reconnect */
#define BOT_HOST_REJOIN_WAIT_MS    5000    /* wait before a new join after a drop */
#define BOT_HOST_STATS_TICKS       50      /* CMD_BOT_STATS about once a second */
#define BOT_HOST_CONNECT_MS        30000   /* a bot connection must join in this */

static char     optServer[256] = "127.0.0.1";
static uint16_t optPort        = BOT_HOST_DEFAULT_PORT;
static int      optSlots       = 4;
static int      optThreads     = 0;
static char     optKey[JOIN_BOT_HOST_KEY_LEN + 1];
static char     optName[JOIN_BOT_HOST_NAME_LEN];

static volatile sig_atomic_t hostQuit = 0;

static void signalHandler(int sig) {
    (void)sig;
    hostQuit = 1;
}

static void hostMessageHandler(const char *message, const char *title) {
    fprintf(stderr, "[bothost] [%s] %s\n", title ? title : "WinBolo",
            message ? message : "");
}

/* ------------------------------------------------------------------ */
/* Seats                                                               */
/* ------------------------------------------------------------------ */

/* One bot seat the server handed this host. */
typedef struct {
    bool        used;
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

    /* The bot connection's own input tick counter: two half-steps a think. */
    uint32_t    inputTick;
    uint32_t    statsTick;
    double      thinkMsSum;
    uint32_t    killCount;
    uint32_t    teamMsgsOut;   /* CMD_BOT_TEAM_MSG sent */
    uint32_t    teamMsgsIn;    /* CTRL_BOT_TEAM_MSG received */

    /* Pings asked for during the think, sent after it (CMD_PING). */
    ClientCommand pings[BOT_PENDING_CMD_MAX];
    int           pingCount;
} HostSeat;

static HostSeat seats[MAX_TANKS];

/* ------------------------------------------------------------------ */
/* Control connection                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    CTL_IDLE,
    CTL_JOINING,
    CTL_CONNECTED
} CtlState;

typedef struct {
    SOCKET     sock;
    struct sockaddr_in addr;
    CtlState   state;
    uint8_t    cookie[JOIN_COOKIE_LEN];
    bool       haveCookie;
    uint32_t   outSeq;
    uint32_t   lastRecvMs;
    uint32_t   lastJoinMs;
    uint32_t   lastPingMs;
    uint32_t   idleSinceMs;
    uint32_t   serverNow;      /* last PONG's server time, echoed back */
    uint16_t   rttMs;
    uint32_t   muxTick;
    ChannelMux mux;
} HostCtl;

static HostCtl ctl;

static uint32_t nowMs(void) {
    return (uint32_t)SDL_GetTicks();
}

static void ctlCloseSocket(void) {
    if (ctl.sock != INVALID_SOCKET) {
        closesocket(ctl.sock);
        ctl.sock = INVALID_SOCKET;
    }
}

static bool ctlResolve(struct sockaddr_in *out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(optPort);
    out->sin_addr.s_addr = inet_addr(optServer);
    if (out->sin_addr.s_addr == INADDR_NONE) {
        if (bolo_resolve_ipv4(optServer, &out->sin_addr) != 0) {
            return false;
        }
    }
    return true;
}

static void ctlSendJoin(void) {
    uint8_t buf[PACKET_HEADER_SIZE + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3 +
                WBN_JOIN_KEY_WIRE_LEN + 1 + 2 + 2 + JOIN_COOKIE_LEN +
                JOIN_BOT_HOST_KEY_LEN + 1 + JOIN_BOT_HOST_NAME_LEN];
    int pos = PACKET_HEADER_SIZE;

    memset(buf, 0, sizeof(buf));
    packHeader(buf, PACKET_JOIN_REQUEST, ctl.outSeq++);
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
    if (ctl.haveCookie) {
        memcpy(buf + pos, ctl.cookie, JOIN_COOKIE_LEN);
    }
    pos += JOIN_COOKIE_LEN;
    memcpy(buf + pos, optKey, strlen(optKey));
    pos += JOIN_BOT_HOST_KEY_LEN;
    buf[pos++] = (uint8_t)optSlots;
    SDL_strlcpy((char *)buf + pos, optName, JOIN_BOT_HOST_NAME_LEN);
    pos += JOIN_BOT_HOST_NAME_LEN;
    udpSendTo(ctl.sock, buf, pos, &ctl.addr);
    ctl.lastJoinMs = nowMs();
}

/* Start a new join on a new socket, so the server sees a new address and
 * never mistakes it for the connection that went away. */
static void ctlStartJoin(void) {
    ctlCloseSocket();
    if (!ctlResolve(&ctl.addr)) {
        fprintf(stderr, "[bothost] cannot resolve server \"%s\"\n", optServer);
        ctl.state = CTL_IDLE;
        ctl.idleSinceMs = nowMs();
        return;
    }
    ctl.sock = createUdpSocket(false);
    if (ctl.sock == INVALID_SOCKET) {
        fprintf(stderr, "[bothost] cannot open a UDP socket\n");
        ctl.state = CTL_IDLE;
        ctl.idleSinceMs = nowMs();
        return;
    }
    ctl.state = CTL_JOINING;
    ctl.haveCookie = false;
    ctl.outSeq = 1;
    ctl.lastRecvMs = nowMs();
    channelMuxInit(&ctl.mux);
    ctlSendJoin();
}

static void ctlSendPing(void) {
    uint8_t buf[PACKET_HEADER_SIZE + 8];
    uint32_t t = nowMs();
    if (t == 0) t = 1;
    packHeader(buf, PACKET_PING, ctl.outSeq++);
    packU32(buf + PACKET_HEADER_SIZE, t);
    packU32(buf + PACKET_HEADER_SIZE + 4, 0);
    udpSendTo(ctl.sock, buf, sizeof(buf), &ctl.addr);
    ctl.lastPingMs = t;
}

static void ctlSendQuit(void) {
    uint8_t buf[PACKET_HEADER_SIZE];
    if (ctl.sock == INVALID_SOCKET || ctl.state != CTL_CONNECTED) return;
    packHeader(buf, PACKET_QUIT, ctl.outSeq++);
    udpSendTo(ctl.sock, buf, sizeof(buf), &ctl.addr);
}

/* Send the channel's acks (the host sends nothing else on it). */
static void ctlPump(void) {
    int frames;
    if (ctl.state != CTL_CONNECTED) return;
    channelTick(&ctl.mux, ctl.muxTick++, ctl.rttMs);
    for (frames = 0; frames < 4; frames++) {
        uint8_t buf[UDP_MAX_PAYLOAD];
        int len = channelBuildFrame(&ctl.mux, buf + PACKET_HEADER_SIZE,
                                    UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
        if (len <= 2) break;
        packHeader(buf, PACKET_CHANNEL, ctl.outSeq++);
        udpSendTo(ctl.sock, buf, PACKET_HEADER_SIZE + len, &ctl.addr);
    }
}

/* ------------------------------------------------------------------ */
/* Seat work                                                           */
/* ------------------------------------------------------------------ */

static int hostActiveSeats(void) {
    int i, n = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (seats[i].used) n++;
    }
    return n;
}

static void seatPrintCount(void) {
    fprintf(stderr, "Bot Host \"%s\": %d/%d slots used\n", optName,
            hostActiveSeats(), optSlots);
}

static HostSeat *seatForCs(ClientSim *cs) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
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
    fprintf(stderr, "[bothost] bot %d \"%s\" stopped (%s); team messages "
                    "sent %u, received %u\n", (int)hs->seat, hs->name, why,
            (unsigned)hs->teamMsgsOut, (unsigned)hs->teamMsgsIn);
    seatCloseConnection(hs);
    memset(hs, 0, sizeof(*hs));
    seatPrintCount();
}

static void clearAllSeats(const char *why) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (seats[i].used) seatClear(&seats[i], why);
    }
}

/* Open the seat's bot connection with its token. */
static bool seatOpenConnection(HostSeat *hs) {
    hs->cs = clientSimAlloc();
    if (hs->cs == NULL) return false;
    clientSimCreate(hs->cs);
    clientSimSetIsBot(hs->cs, true);
    clientSimSetMyLastPlayerName(hs->cs, hs->name);
    clientSimSetChatSendFunc(hs->cs, seatChatSend);
    hs->relay.ctx = hs;
    hs->relay.internalMsg = seatRelayInternalMsg;
    hs->relay.ping = seatRelayPing;
    clientSimSetBrainRelay(hs->cs, &hs->relay);
    *clientSimGetAllowComputerTanks(hs->cs) = (aiType)hs->ai;

    clientSimConnectUdp(hs->cs, optServer, optPort, hs->name, "", "", "", "",
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
    return true;
}

/* Build the seat's brain the way the bot pool builds an in-process one. */
static void seatBuildBrain(HostSeat *hs) {
    char path[512];
    uint32_t crc = 0;

    hs->wantBuild = false;
    seatDestroyBrain(hs);
    if (!brainListResolve(hs->brainName, path, sizeof(path))) {
        fprintf(stderr, "[bothost] bot %d: no brain \"%s\" here; it goes back "
                        "to the server\n", (int)hs->seat, hs->brainName);
        hs->buildFailed = true;
        return;
    }
    {
        size_t sz = 0;
        void *data = SDL_LoadFile(path, &sz);
        if (data != NULL) {
            crc = SDL_crc32(0, data, sz);
            SDL_free(data);
        }
    }
    if (hs->brainVersion != 0 && crc != hs->brainVersion) {
        fprintf(stderr, "[bothost] bot %d: note: brain \"%s\" here is not the "
                        "server's copy\n", (int)hs->seat, hs->brainName);
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
                                (aiType)hs->ai, false, (int)hs->seat,
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
    fprintf(stderr, "[bothost] bot %d \"%s\" runs brain \"%s\" (ai %d)\n",
            (int)hs->seat, hs->name, hs->brainName, (int)hs->ai);
}

/* ------------------------------------------------------------------ */
/* Control events                                                      */
/* ------------------------------------------------------------------ */

static void onAssign(const ControlEvent *evt) {
    uint8_t s = evt->u.botHostAssign.seat;
    HostSeat *hs;
    static const uint8_t zero[JOIN_BOT_SLOT_TOKEN_LEN] = {0};
    bool newPlacement;

    if (s >= MAX_TANKS) return;
    hs = &seats[s];
    newPlacement = memcmp(evt->u.botHostAssign.token, zero,
                          JOIN_BOT_SLOT_TOKEN_LEN) != 0;
    if (!newPlacement && !hs->used) return;     /* an update for a seat gone */
    if (newPlacement && hs->used) {
        seatCloseConnection(hs);
    }
    if (newPlacement) {
        bool wasUsed = hs->used;
        memset(hs, 0, sizeof(*hs));
        hs->used = true;
        hs->seat = s;
        memcpy(hs->token, evt->u.botHostAssign.token, JOIN_BOT_SLOT_TOKEN_LEN);
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
        fprintf(stderr, "[bothost] bot %d \"%s\" assigned (brain \"%s\")\n",
                (int)s, hs->name, hs->brainName);
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

static void onUnassign(const ControlEvent *evt) {
    uint8_t s = evt->u.botHostUnassign.seat;
    if (s >= MAX_TANKS) return;
    seatClear(&seats[s], "server took it back");
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

static void onInitTable(const ControlEvent *evt) {
    uint8_t s = evt->u.botTableEntry.seat;
    HostSeat *hs;
    char why[256];
    if (s >= MAX_TANKS) return;
    hs = &seats[s];
    if (!hs->used) return;
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

static void onHint(const ControlEvent *evt) {
    uint8_t s = evt->u.botTableEntry.seat;
    HostSeat *hs;
    char err[256];
    if (s >= MAX_TANKS) return;
    hs = &seats[s];
    if (!hs->used) return;
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

static void onTeamMsg(const ControlEvent *evt) {
    uint8_t s = evt->u.botTeamMsg.seat;
    HostSeat *hs;
    MessageState *ms;
    if (s >= MAX_TANKS) return;
    hs = &seats[s];
    if (!hs->used || hs->cs == NULL) return;
    ms = clientSimGetMessages(hs->cs);
    if (ms == NULL) return;
    messageInboxPushLine(ms, evt->u.botTeamMsg.fromPlayer,
                         evt->u.botTeamMsg.text);
    hs->teamMsgsIn++;
}

static void ctlHandleControl(const uint8_t *msg, uint16_t len) {
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
        case CTRL_BOT_HOST_ASSIGN:   onAssign(&evt);    break;
        case CTRL_BOT_HOST_UNASSIGN: onUnassign(&evt);  break;
        case CTRL_BOT_INIT_TABLE:    onInitTable(&evt); break;
        case CTRL_BOT_HINT:          onHint(&evt);      break;
        case CTRL_BOT_TEAM_MSG:      onTeamMsg(&evt);   break;
        default: break;
    }
}

/* The control connection dropped: every seat goes back to the server, which
 * runs them in-process until this host is back. */
static void ctlDrop(const char *why) {
    fprintf(stderr, "[bothost] connection to %s:%u lost (%s); joining again\n",
            optServer, (unsigned)optPort, why);
    clearAllSeats("connection to the server lost");
    ctlCloseSocket();
    ctl.state = CTL_IDLE;
    ctl.idleSinceMs = nowMs();
}

/* Returns false when the host must stop (the server refused the key). */
static bool ctlReceive(void) {
    uint8_t buf[UDP_MAX_PAYLOAD + 64];
    struct sockaddr_in from;
    int len;

    if (ctl.sock == INVALID_SOCKET) return true;
    while ((len = udpRecvFrom(ctl.sock, buf, sizeof(buf), &from)) > 0) {
        uint8_t type;
        if (from.sin_addr.s_addr != ctl.addr.sin_addr.s_addr ||
            from.sin_port != ctl.addr.sin_port) {
            continue;
        }
        if (len < PACKET_HEADER_SIZE) continue;
        type = getPacketType(buf, len);
        ctl.lastRecvMs = nowMs();
        switch (type) {
            case PACKET_JOIN_CHALLENGE:
                if (ctl.state == CTL_JOINING &&
                    len >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                    memcpy(ctl.cookie, buf + PACKET_HEADER_SIZE,
                           JOIN_COOKIE_LEN);
                    ctl.haveCookie = true;
                    ctlSendJoin();
                }
                break;
            case PACKET_JOIN_ACCEPT:
                if (ctl.state == CTL_JOINING) {
                    ctl.state = CTL_CONNECTED;
                    ctl.lastPingMs = 0;
                    fprintf(stderr, "[bothost] connected to %s:%u as Bot Host "
                                    "\"%s\" with %d slots\n",
                            optServer, (unsigned)optPort, optName, optSlots);
                    seatPrintCount();
                }
                break;
            case PACKET_JOIN_REJECT: {
                uint16_t id = 0;
                if (len >= PACKET_HEADER_SIZE + 2) {
                    id = unpackU16(buf + PACKET_HEADER_SIZE);
                }
                if (id == STR_REJECT_INCORRECT_PASSWORD) {
                    fprintf(stderr, "[bothost] the server refused the key\n");
                    return false;
                }
                fprintf(stderr, "[bothost] the server refused the join "
                                "(reason %u); trying again\n", (unsigned)id);
                ctlCloseSocket();
                ctl.state = CTL_IDLE;
                ctl.idleSinceMs = nowMs();
                return true;
            }
            case PACKET_PONG:
                if (len >= PACKET_HEADER_SIZE + 8) {
                    uint32_t sent = unpackU32(buf + PACKET_HEADER_SIZE);
                    ctl.serverNow = unpackU32(buf + PACKET_HEADER_SIZE + 4);
                    ctl.rttMs = (uint16_t)(nowMs() - sent);
                    /* Echo the server time back at once, with clientTime 0,
                     * so the server measures the round trip and not the
                     * gap to the next 500 ms PING. Same as a game client. */
                    {
                        uint8_t echo[PACKET_HEADER_SIZE + 8];
                        packHeader(echo, PACKET_PING, ctl.outSeq++);
                        packU32(echo + PACKET_HEADER_SIZE, 0);
                        packU32(echo + PACKET_HEADER_SIZE + 4, ctl.serverNow);
                        udpSendTo(ctl.sock, echo, sizeof(echo), &ctl.addr);
                    }
                }
                break;
            case PACKET_CHANNEL:
                if (ctl.state == CTL_CONNECTED) {
                    uint8_t msg[CHANNEL_MAX_SEG];
                    uint16_t msgLen = sizeof(msg);
                    (void)channelRecvFrame(&ctl.mux, buf + PACKET_HEADER_SIZE,
                                           len - PACKET_HEADER_SIZE);
                    while (channelReceive(&ctl.mux, CHANNEL_CONTROL, msg,
                                          &msgLen)) {
                        ctlHandleControl(msg, msgLen);
                        msgLen = sizeof(msg);
                    }
                }
                break;
            case PACKET_KICKED:
                if (ctl.state == CTL_CONNECTED) {
                    ctlDrop("the server closed it");
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return true;
}

static void ctlService(void) {
    uint32_t t = nowMs();
    switch (ctl.state) {
        case CTL_IDLE:
            if (t - ctl.idleSinceMs >= BOT_HOST_REJOIN_WAIT_MS ||
                ctl.idleSinceMs == 0) {
                ctlStartJoin();
            }
            break;
        case CTL_JOINING:
            if (t - ctl.lastJoinMs >= BOT_HOST_JOIN_RETRY_MS) {
                ctlSendJoin();
            }
            break;
        case CTL_CONNECTED:
            if (t - ctl.lastRecvMs >= BOT_HOST_SILENCE_MS) {
                ctlDrop("no reply");
                break;
            }
            if (ctl.lastPingMs == 0 || t - ctl.lastPingMs >= BOT_HOST_PING_MS) {
                ctlSendPing();
            }
            ctlPump();
            break;
    }
}

/* ------------------------------------------------------------------ */
/* The think loop                                                      */
/* ------------------------------------------------------------------ */

static double serialMsEwma = 0.0;
static double lastTargetMs = 0.0;
static int    jobIndices[MAX_TANKS];

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
    SDL_strlcpy(cmd.u.botStats.hostName, optName,
                sizeof(cmd.u.botStats.hostName));
    clientSimSubmitCommand(hs->cs, &cmd);
}

/* Bot connections: keep them ticking, notice when they end, and build the
 * brains that are owed. */
static void seatsService(void) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
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

    for (i = 0; i < MAX_TANKS; i++) {
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
    botManagerSetHostActiveBots(hostActiveSeats());

    runners = botWorkerPoolGetSize() + 1;
    if (optThreads > runners) runners = optThreads;
    lastTargetMs = botRunnerTargetMs(serialMsEwma, runners, active);
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

    for (i = 0; i < MAX_TANKS; i++) {
        HostSeat *hs = &seats[i];
        if (!hs->used || hs->cs == NULL || !hs->bot.brain.running) continue;
        if (clientSimGetConnectState(hs->cs) != CLIENT_CONNECT_CONNECTED) continue;
        if (++hs->statsTick >= BOT_HOST_STATS_TICKS) {
            hs->statsTick = 0;
            seatSendStats(hs);
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

static void printUsage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --key KEY [options]\n"
            "Runs a WinBoloDS server's bots on this machine.\n"
            "  --server ADDR   server address (default 127.0.0.1)\n"
            "  --port N        server port (default %d)\n"
            "  --slots N       bots this host runs at most (default 4)\n"
            "  --threads N     brain threads (default: one per CPU core)\n"
            "  --key KEY       the server's -bothostkey (required)\n"
            "  --name NAME     the name the server operator sees\n",
            prog, BOT_HOST_DEFAULT_PORT);
}

static bool parseArgs(int argc, char **argv) {
    int i;
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
        if (strcmp(a, "--server") == 0) {
            SDL_strlcpy(optServer, v, sizeof(optServer));
        } else if (strcmp(a, "--port") == 0) {
            int p = atoi(v);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "bad port %s\n", v);
                return false;
            }
            optPort = (uint16_t)p;
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
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            printUsage(argv[0]);
            return false;
        }
        i++;
    }
    if (optKey[0] == '\0') {
        fprintf(stderr, "--key is required\n");
        printUsage(argv[0]);
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

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    uint32_t nextTick;
    int result = 0;

    bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());
    sentryInit("WinBoloBotHost", argc, argv);
    atexit(sentryClose);

    if (!parseArgs(argc, argv)) return 1;

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

    memset(seats, 0, sizeof(seats));
    memset(&ctl, 0, sizeof(ctl));
    ctl.sock = INVALID_SOCKET;
    ctl.state = CTL_IDLE;
    ctl.idleSinceMs = 0;

    fprintf(stderr, "[bothost] WinBoloBotHost \"%s\": %d slots, server %s:%u\n",
            optName, optSlots, optServer, (unsigned)optPort);

    nextTick = nowMs();
    while (!hostQuit) {
        uint32_t t;
        if (!ctlReceive()) {
            result = 2;
            break;
        }
        ctlService();
        t = nowMs();
        if ((int32_t)(t - nextTick) >= 0) {
            hostTick();
            nextTick += BOT_HOST_TICK_MS;
            /* After a long stall, start counting again from now. */
            if ((int32_t)(t - nextTick) > 10 * BOT_HOST_TICK_MS) {
                nextTick = t + BOT_HOST_TICK_MS;
            }
        }
        SDL_Delay(1);
    }

    fprintf(stderr, "[bothost] stopping\n");
    clearAllSeats("the host stops");
    ctlSendQuit();
    ctlCloseSocket();

    clientMutexDestroy();
    langCleanup();
    serverSimBotPoolDestroy();
    SDL_Quit();
    return result;
}
