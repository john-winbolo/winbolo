/*
 * Bot Host coverage: the wire messages between a WinBoloDS server and a
 * WinBoloBotHost, and the bot manager's side of a seat whose brain runs
 * on a Bot Host.
 *
 *   1-4. Each control event (CTRL_BOT_HOST_ASSIGN, CTRL_BOT_HOST_UNASSIGN,
 *        CTRL_BOT_INIT_TABLE / CTRL_BOT_HINT, CTRL_BOT_TEAM_MSG) keeps every
 *        field through its body codec, and a short body is refused.
 *   5.   CMD_BOT_TEAM_MSG and CMD_BOT_STATS keep every field through the
 *        command codec.
 *   6.   A bot team message reaches an allied in-process bot's inbox and
 *        an allied Bot Host bot's teamMsg hook, and no enemy.
 *   7.   A Bot Host seat is held to the same smart-ping gap as an
 *        in-process bot.
 *   8.   A seat handed to a Bot Host stays a bot seat; taking it back with
 *        no brain to build holds it.
 *   9.   The per-bot think budget is the 20 ms formula.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "allience.h"
#include "bot_manager.h"
#include "client_command.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "control_event.h"
#include "game_sim.h"
#include "internal/messages.h"
#include "players.h"
#include "scenario_table.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "transport_command_codec.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* ----------------------------------------------------------------
 * Codec helpers
 * ---------------------------------------------------------------- */

static int bh_body_roundtrip(const ControlEvent *in, ControlEvent *out,
                             size_t *bodyLen) {
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t len = 0;
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(in->type);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(in->type);
    if (enc == NULL) return -1;
    if (dec == NULL) return -2;
    if (enc(in, NULL, buf, sizeof(buf), &len) != ENCODE_OK) return -3;
    memset(out, 0, sizeof(*out));
    if (!dec(buf, len, out)) return -4;
    if (out->type != in->type) return -5;
    /* One byte short must be refused. */
    if (len > 0) {
        ControlEvent shortOut;
        if (dec(buf, len - 1, &shortOut)) return -6;
    }
    if (bodyLen != NULL) *bodyLen = len;
    return 0;
}

/* ================================================================
 * 1. CTRL_BOT_HOST_ASSIGN
 * ================================================================ */
int run_bot_host_assign_codec(void) {
    ControlEvent in, out;
    int r;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_ASSIGN;
    in.u.botHostAssign.seat = 7;
    in.u.botHostAssign.reload = 1;
    in.u.botHostAssign.ai = 3;
    in.u.botHostAssign.team = 2;
    in.u.botHostAssign.gen = 0xBEEF;
    in.u.botHostAssign.startTick = 0x01020304u;
    in.u.botHostAssign.brainVersion = 0xA1B2C3D4u;
    for (int i = 0; i < BOT_HOST_TOKEN_LEN; i++) {
        in.u.botHostAssign.token[i] = (uint8_t)(0x10 + i);
    }
    SDL_strlcpy(in.u.botHostAssign.name, "Robo Seven",
                sizeof(in.u.botHostAssign.name));
    SDL_strlcpy(in.u.botHostAssign.brain, "GoalHunter",
                sizeof(in.u.botHostAssign.brain));
    SDL_strlcpy(in.u.botHostAssign.initArg, "difficulty=hard;role=attack",
                sizeof(in.u.botHostAssign.initArg));

    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT_MSG(r == 0, "assign round trip failed (%d)", r);
    UT_ASSERT(out.u.botHostAssign.seat == 7);
    UT_ASSERT(out.u.botHostAssign.reload == 1);
    UT_ASSERT(out.u.botHostAssign.ai == 3);
    UT_ASSERT(out.u.botHostAssign.team == 2);
    UT_ASSERT(out.u.botHostAssign.gen == 0xBEEF);
    UT_ASSERT(out.u.botHostAssign.startTick == 0x01020304u);
    UT_ASSERT(out.u.botHostAssign.brainVersion == 0xA1B2C3D4u);
    UT_ASSERT(memcmp(out.u.botHostAssign.token, in.u.botHostAssign.token,
                     BOT_HOST_TOKEN_LEN) == 0);
    UT_ASSERT(strcmp(out.u.botHostAssign.name, "Robo Seven") == 0);
    UT_ASSERT(strcmp(out.u.botHostAssign.brain, "GoalHunter") == 0);
    UT_ASSERT(strcmp(out.u.botHostAssign.initArg,
                     "difficulty=hard;role=attack") == 0);

    /* A config update carries a zero token; it must stay zero. */
    memset(in.u.botHostAssign.token, 0, BOT_HOST_TOKEN_LEN);
    in.u.botHostAssign.reload = 0;
    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT_MSG(r == 0, "update round trip failed (%d)", r);
    for (int i = 0; i < BOT_HOST_TOKEN_LEN; i++) {
        UT_ASSERT(out.u.botHostAssign.token[i] == 0);
    }
    UT_ASSERT(out.u.botHostAssign.reload == 0);
    return 0;
}

/* ================================================================
 * 2. CTRL_BOT_HOST_UNASSIGN
 * ================================================================ */
int run_bot_host_unassign_codec(void) {
    ControlEvent in, out;
    int r;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_UNASSIGN;
    in.u.botHostUnassign.seat = 13;
    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT_MSG(r == 0, "unassign round trip failed (%d)", r);
    UT_ASSERT(out.u.botHostUnassign.seat == 13);
    return 0;
}

/* ================================================================
 * 3. CTRL_BOT_INIT_TABLE and CTRL_BOT_HINT: one entry each, and the
 *    empty table (count 0).
 * ================================================================ */
static int bh_table_case(ControlEventType type) {
    ControlEvent in, out;
    int r;
    memset(&in, 0, sizeof(in));
    in.type = type;
    in.u.botTableEntry.seat = 4;
    in.u.botTableEntry.idx = 2;
    in.u.botTableEntry.count = 5;
    SDL_strlcpy(in.u.botTableEntry.key, "role",
                sizeof(in.u.botTableEntry.key));
    SDL_strlcpy(in.u.botTableEntry.value, "defend the north base",
                sizeof(in.u.botTableEntry.value));
    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT_MSG(r == 0, "%s round trip failed (%d)",
                  controlEventDiagName(type), r);
    UT_ASSERT(out.u.botTableEntry.seat == 4);
    UT_ASSERT(out.u.botTableEntry.idx == 2);
    UT_ASSERT(out.u.botTableEntry.count == 5);
    UT_ASSERT(strcmp(out.u.botTableEntry.key, "role") == 0);
    UT_ASSERT(strcmp(out.u.botTableEntry.value, "defend the north base") == 0);

    memset(&in.u.botTableEntry, 0, sizeof(in.u.botTableEntry));
    in.u.botTableEntry.seat = 9;
    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT_MSG(r == 0, "%s empty table round trip failed (%d)",
                  controlEventDiagName(type), r);
    UT_ASSERT(out.u.botTableEntry.seat == 9);
    UT_ASSERT(out.u.botTableEntry.count == 0);
    UT_ASSERT(out.u.botTableEntry.key[0] == '\0');
    return 0;
}

int run_bot_host_table_codec(void) {
    if (bh_table_case(CTRL_BOT_INIT_TABLE) != 0) return 1;
    if (bh_table_case(CTRL_BOT_HINT) != 0) return 1;
    return 0;
}

/* ================================================================
 * 4. CTRL_BOT_TEAM_MSG
 * ================================================================ */
int run_bot_host_team_msg_codec(void) {
    ControlEvent in, out;
    int r;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_TEAM_MSG;
    in.u.botTeamMsg.seat = 5;
    in.u.botTeamMsg.fromPlayer = 2;
    SDL_strlcpy(in.u.botTeamMsg.text, "/info state goal=defend",
                sizeof(in.u.botTeamMsg.text));
    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT_MSG(r == 0, "team message round trip failed (%d)", r);
    UT_ASSERT(out.u.botTeamMsg.seat == 5);
    UT_ASSERT(out.u.botTeamMsg.fromPlayer == 2);
    UT_ASSERT(strcmp(out.u.botTeamMsg.text, "/info state goal=defend") == 0);
    return 0;
}

/* ================================================================
 * 5. CMD_BOT_TEAM_MSG and CMD_BOT_STATS through the command codec.
 * ================================================================ */
static int bh_cmd_roundtrip(const ClientCommand *in, ClientCommand *out) {
    uint8_t buf[COMMAND_MAX_WIRE_BYTES];
    size_t len = 0;
    if (!commandCodecEncode(in, buf, sizeof(buf), &len)) return -1;
    memset(out, 0, sizeof(*out));
    if (!commandCodecDecode(buf, len, out)) return -2;
    return 0;
}

int run_bot_host_commands_codec(void) {
    ClientCommand in, out;

    memset(&in, 0, sizeof(in));
    in.type = CMD_BOT_TEAM_MSG;
    in.cmdSeq = 41;
    SDL_strlcpy(in.u.botTeamMsg.text, "/info carry pill 3",
                sizeof(in.u.botTeamMsg.text));
    UT_ASSERT_MSG(bh_cmd_roundtrip(&in, &out) == 0, "CMD_BOT_TEAM_MSG");
    UT_ASSERT(out.type == CMD_BOT_TEAM_MSG);
    UT_ASSERT(out.cmdSeq == 41);
    UT_ASSERT(strcmp(out.u.botTeamMsg.text, "/info carry pill 3") == 0);

    memset(&in, 0, sizeof(in));
    in.type = CMD_BOT_STATS;
    in.cmdSeq = 42;
    in.u.botStats.lastThinkTenths = 31;
    in.u.botStats.avgThinkTenths  = 27;
    in.u.botStats.maxThinkTenths  = 140;
    in.u.botStats.targetTenths    = 120;
    in.u.botStats.thinkCount      = 123456u;
    in.u.botStats.overrunCount    = 17u;
    in.u.botStats.killCount       = 2u;
    in.u.botStats.tier            = 0;
    SDL_strlcpy(in.u.botStats.hostName, "LAPTOP",
                sizeof(in.u.botStats.hostName));
    UT_ASSERT_MSG(bh_cmd_roundtrip(&in, &out) == 0, "CMD_BOT_STATS");
    UT_ASSERT(out.type == CMD_BOT_STATS);
    UT_ASSERT(out.cmdSeq == 42);
    UT_ASSERT(out.u.botStats.lastThinkTenths == 31);
    UT_ASSERT(out.u.botStats.avgThinkTenths == 27);
    UT_ASSERT(out.u.botStats.maxThinkTenths == 140);
    UT_ASSERT(out.u.botStats.targetTenths == 120);
    UT_ASSERT(out.u.botStats.thinkCount == 123456u);
    UT_ASSERT(out.u.botStats.overrunCount == 17u);
    UT_ASSERT(out.u.botStats.killCount == 2u);
    UT_ASSERT(out.u.botStats.tier == 0);
    UT_ASSERT(strcmp(out.u.botStats.hostName, "LAPTOP") == 0);
    return 0;
}

/* ----------------------------------------------------------------
 * Bot manager fixture: slot 0 human; bots 1, 2 and 3 allied with 0;
 * bot 4 on the other side. Bot 2 runs on a Bot Host.
 * ---------------------------------------------------------------- */

typedef struct {
    ServerSim *sim;
    ClientSim *cs[5];
} BhFixture;

typedef struct {
    int  calls;
    BYTE seat[MAX_TANKS];
    BYTE from[MAX_TANKS];
    char msg[MAX_TANKS][BRAIN_INBOX_MSG_LEN];
} BhHookLog;

static void bh_hook_team_msg(void *ctx, BYTE seat, BYTE fromPlayer,
                             const char *msg) {
    BhHookLog *log = (BhHookLog *)ctx;
    if (log->calls >= MAX_TANKS) return;
    log->seat[log->calls] = seat;
    log->from[log->calls] = fromPlayer;
    SDL_strlcpy(log->msg[log->calls], msg, BRAIN_INBOX_MSG_LEN);
    log->calls++;
}

static int bh_fixture_setup(BhFixture *f) {
    GameSim *gs;
    players *plrs;
    memset(f, 0, sizeof(*f));
    f->sim = ut_make_running_sim("Human");
    if (f->sim == NULL) return 1;
    serverSimAddPlayer(f->sim, 1, "Bot1", false);
    serverSimAddPlayer(f->sim, 2, "Bot2", false);
    serverSimAddPlayer(f->sim, 3, "Bot3", false);
    serverSimAddPlayer(f->sim, 4, "Enemy", false);
    gs = serverSimGetGameSim(f->sim);
    plrs = &gs->plyrs;
    for (BYTE b = 1; b <= 4; b++) {
        (*plrs)->item[b].clientFlags |= PLAYER_FLAG_BOT;
    }
    for (BYTE a = 0; a <= 3; a++) {
        for (BYTE b = 0; b <= 3; b++) {
            if (a != b) allienceAdd(&(*plrs)->item[a].allie, b);
        }
    }
    for (BYTE b = 1; b <= 4; b++) {
        f->cs[b] = clientSimAlloc();
        if (f->cs[b] == NULL) return 1;
        clientSimCreate(f->cs[b]);
        clientSimSetPlayerNum(f->cs[b], b);
        f->sim->botMgr.bots[b].active = true;
        f->sim->botMgr.bots[b].cs = f->cs[b];
    }
    return 0;
}

static void bh_fixture_teardown(BhFixture *f) {
    if (f->sim != NULL) {
        botManagerSetRemoteHooks(f->sim, NULL);
        for (BYTE b = 1; b <= 4; b++) {
            f->sim->botMgr.bots[b].active = false;
            f->sim->botMgr.bots[b].remote = false;
            f->sim->botMgr.bots[b].cs = NULL;
        }
    }
    for (BYTE b = 1; b <= 4; b++) {
        if (f->cs[b] != NULL) clientSimDestroy(f->cs[b]);
    }
    if (f->sim != NULL) serverSimDestroy(f->sim);
}

/* ================================================================
 * 6. A team message from bot 1 reaches bot 3's inbox (in-process) and
 *    bot 2's teamMsg hook (Bot Host), and not bot 4 (the other side).
 *    Bot 2's own ClientSim inbox stays empty: its brain is elsewhere.
 * ================================================================ */
int run_bot_host_team_msg_fanout(void) {
    BhFixture f;
    BhHookLog log;
    BotRemoteHooks hooks;

    if (bh_fixture_setup(&f) != 0) {
        bh_fixture_teardown(&f);
        return 1;
    }
    memset(&log, 0, sizeof(log));
    memset(&hooks, 0, sizeof(hooks));
    hooks.ctx = &log;
    hooks.teamMsg = bh_hook_team_msg;
    botManagerSetRemoteHooks(f.sim, &hooks);
    f.sim->botMgr.bots[2].remote = true;

    botManagerDeliverInternalMessage(f.sim, 1, "/info state goal=attack");

    UT_ASSERT_MSG(log.calls == 1, "hook calls: got %d want 1", log.calls);
    UT_ASSERT(log.seat[0] == 2);
    UT_ASSERT(log.from[0] == 1);
    UT_ASSERT(strcmp(log.msg[0], "/info state goal=attack") == 0);
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.cs[2])) == 0,
                  "the Bot Host seat's own inbox must stay empty");
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.cs[3])) == 1,
                  "allied in-process bot 3 must get the message");
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.cs[4])) == 0,
                  "bot 4 is on the other side");
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.cs[1])) == 0,
                  "the sender gets no copy");

    bh_fixture_teardown(&f);
    return 0;
}

/* ================================================================
 * 7. Smart ping gap: a Bot Host seat gets one ping per gap, and only a
 *    Bot Host seat is asked this way.
 * ================================================================ */
int run_bot_host_ping_gap(void) {
    BhFixture f;
    if (bh_fixture_setup(&f) != 0) {
        bh_fixture_teardown(&f);
        return 1;
    }
    UT_ASSERT_MSG(!botManagerRemotePingAllowed(f.sim, 2),
                  "an in-process seat must not take the remote path");
    f.sim->botMgr.bots[2].remote = true;
    UT_ASSERT(botManagerRemotePingAllowed(f.sim, 2));
    UT_ASSERT_MSG(!botManagerRemotePingAllowed(f.sim, 2),
                  "a second ping in the same tick must wait for the gap");
    UT_ASSERT(!botManagerRemotePingAllowed(f.sim, 0));
    bh_fixture_teardown(&f);
    return 0;
}

/* ================================================================
 * 8. A seat handed to a Bot Host stays a bot seat. Taking it back with
 *    no brain path holds it (no crash, still a bot, no longer remote).
 * ================================================================ */
int run_bot_host_remote_seat_stays_bot(void) {
    BhFixture f;
    if (bh_fixture_setup(&f) != 0) {
        bh_fixture_teardown(&f);
        return 1;
    }
    UT_ASSERT(botManagerSetRemote(f.sim, 3));
    UT_ASSERT(botManagerIsRemote(f.sim, 3));
    UT_ASSERT_MSG(serverSimIsBot(f.sim, 3),
                  "a Bot Host seat is still a bot to the server");
    UT_ASSERT_MSG(!botManagerSetRemote(f.sim, 3),
                  "a seat already on a Bot Host is not handed over twice");
    UT_ASSERT(!botManagerSetRemote(f.sim, 0));

    /* Detach the ClientSim so the take-back does not try to load a map
     * into it; with no brain path the seat is held. */
    f.sim->botMgr.bots[3].cs = NULL;
    f.sim->botMgr.bots[3].brainPath[0] = '\0';
    UT_ASSERT(!botManagerClearRemote(f.sim, 3));
    UT_ASSERT(!botManagerIsRemote(f.sim, 3));
    UT_ASSERT(f.sim->botMgr.bots[3].held);
    UT_ASSERT(serverSimIsBot(f.sim, 3));
    f.sim->botMgr.bots[3].held = false;

    bh_fixture_teardown(&f);
    return 0;
}

/* ================================================================
 * 9. The think budget: 20 ms less the serial part, 6 ms for the sim and
 *    2 ms of margin, shared across the runners, never more than the
 *    whole budget and never under 1 ms.
 * ================================================================ */
static int bh_near(double a, double b) {
    double d = a - b;
    return d < 1e-9 && d > -1e-9;
}

int run_bot_host_target_ms(void) {
    UT_ASSERT(bh_near(botRunnerTargetMs(0.0, 1, 0), 20.0));
    UT_ASSERT(bh_near(botRunnerTargetMs(2.0, 1, 1), 10.0));
    UT_ASSERT(bh_near(botRunnerTargetMs(2.0, 4, 8), 5.0));
    UT_ASSERT(bh_near(botRunnerTargetMs(2.0, 8, 4), 10.0));
    UT_ASSERT(bh_near(botRunnerTargetMs(30.0, 1, 2), 0.5));
    return 0;
}

/* ================================================================
 * 10. The brain sync events: CTRL_BOT_HOST_STATUS (the lobby's Bot Host
 *     row), CTRL_BOT_HOST_FILE_REQ and CTRL_BOT_HOST_SYNC keep every
 *     field, and out-of-range values are refused.
 * ================================================================ */
int run_bot_host_sync_codec(void) {
    ControlEvent in, out;
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t len = 0;
    int r;

    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_STATUS;
    in.u.botHostStatus.idx = 5;
    in.u.botHostStatus.connected = 1;
    in.u.botHostStatus.state = BOT_HOST_SYNC_SYNCING;
    in.u.botHostStatus.percent = 42;
    in.u.botHostStatus.used = 3;
    in.u.botHostStatus.slots = 6;
    SDL_strlcpy(in.u.botHostStatus.name, "Laptop",
                sizeof(in.u.botHostStatus.name));
    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT_MSG(r == 0, "status round trip failed (%d)", r);
    UT_ASSERT(out.u.botHostStatus.idx == 5);
    UT_ASSERT(out.u.botHostStatus.connected == 1);
    UT_ASSERT(out.u.botHostStatus.state == BOT_HOST_SYNC_SYNCING);
    UT_ASSERT(out.u.botHostStatus.percent == 42);
    UT_ASSERT(out.u.botHostStatus.used == 3);
    UT_ASSERT(out.u.botHostStatus.slots == 6);
    UT_ASSERT(strcmp(out.u.botHostStatus.name, "Laptop") == 0);

    /* A bad row index, state or percent is refused. */
    {
        ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_STATUS);
        ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_STATUS);
        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
        buf[0] = BOT_HOST_STATUS_ROWS;
        UT_ASSERT(!dec(buf, len, &out));
        buf[0] = 5;
        buf[2] = BOT_HOST_SYNC_LATE + 1;
        UT_ASSERT(!dec(buf, len, &out));
        buf[2] = BOT_HOST_SYNC_SYNCING;
        buf[3] = 101;
        UT_ASSERT(!dec(buf, len, &out));
    }

    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_FILE_REQ;
    in.u.botHostFileReq.gen = 0x01020304u;
    in.u.botHostFileReq.brain = 9;
    in.u.botHostFileReq.file = 0x0405;
    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT_MSG(r == 0, "file request round trip failed (%d)", r);
    UT_ASSERT(out.u.botHostFileReq.gen == 0x01020304u);
    UT_ASSERT(out.u.botHostFileReq.brain == 9);
    UT_ASSERT(out.u.botHostFileReq.file == 0x0405);

    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_SYNC;
    in.u.botHostSync.gen = 7;
    in.u.botHostSync.done = 2700000u;
    in.u.botHostSync.total = 18800000u;
    in.u.botHostSync.ready = 0;
    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT_MSG(r == 0, "sync round trip failed (%d)", r);
    UT_ASSERT(out.u.botHostSync.gen == 7);
    UT_ASSERT(out.u.botHostSync.done == 2700000u);
    UT_ASSERT(out.u.botHostSync.total == 18800000u);
    UT_ASSERT(out.u.botHostSync.ready == 0);
    in.u.botHostSync.ready = 1;
    in.u.botHostSync.done = in.u.botHostSync.total;
    r = bh_body_roundtrip(&in, &out, NULL);
    UT_ASSERT(r == 0 && out.u.botHostSync.ready == 1);

    /* More done than total is refused. */
    in.u.botHostSync.done = in.u.botHostSync.total + 1;
    {
        ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_SYNC);
        ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_SYNC);
        if (enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK) {
            UT_ASSERT(!dec(buf, len, &out));
        }
    }
    return 0;
}
