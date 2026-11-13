/*
 * Coverage for the CMD_LOBBY_ADD_BOT arm in server_command_dispatch.c.
 *
 * The dispatcher gates add-bot behind four BAD_STATE preconditions
 * (lobbyEnabled, state==lobby, botAiType!=aiNone, brainPath!=""), a
 * NOT_HOST sender check via lobbyClientMayEdit, and an INVALID free-
 * slot check. Each rejection path is silent on the wire — the client
 * gets CTRL_COMMAND_REJECTED but no fielded UI shows it for add-bot
 * yet, so the dispatcher choosing the wrong code is a regression no
 * end-to-end test catches today.
 *
 * Tests build a fresh lobby-enabled ServerSim and drive
 * serverSimApplyCommand with CMD_LOBBY_ADD_BOT, holding the threads
 * mutex (the dispatcher asserts it). The happy path is left to
 * higher-level coverage because it pulls in botManagerAddBot →
 * serverSimAddBot → ClientSim alloc, which needs a real brain file.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_ADD_BOT, CmdResult */
#include "everard_map.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->playerConnected, lobbyPlayers,
                                    * maxBots — seeded directly so the cap
                                    * tests don't need the real bot manager */
#include "server_sim_lifecycle.h"  /* serverSimSetBotBrainPath/AiType,
                                    * serverSimSetLobbyEnabled */
#include "threads.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "client_sim.h"
#include "wire_limits.h"
#include "transport_udp_server_internal.h"
#include "test_harness.h"

/* Lobby-enabled ServerSim with all four CMD_LOBBY_ADD_BOT
 * preconditions satisfied by default — tests then knock one guard
 * off-axis to drive a specific rejection. */
static ServerSim *make_addbot_ready_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                                "Everard Island",
                                                gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetBotAiType(sim, aiYes);
    serverSimSetBotBrainPath(sim, "brains/example.lua");
    /* Host sits in slot 0 so lobbyClientMayEdit(sim, 0) returns true. */
    serverSimAddPlayer(sim, 0, "Host", false);
    return sim;
}

static CmdResult apply_add_bot(ServerSim *sim, int senderSlot) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_ADD_BOT;
    cmd.cmdSeq = 1;
    cmd.u.lobbyAddBot.teamNumber = 0;
    cmd.u.lobbyAddBot.nameLen    = 0;
    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Empty brainPath is the regression the in-the-wild "press Add Bot,
 * nothing happens" symptom traces to: the client doesn't pre-check
 * it in the MP branch (only SP does), so the command reaches the
 * server and BAD_STATE silently. */
int run_lobby_add_bot_rejects_empty_brain_path(void) {
    ServerSim *sim = make_addbot_ready_sim();
    UT_ASSERT(sim != NULL);
    serverSimSetBotBrainPath(sim, "");
    UT_ASSERT_MSG(apply_add_bot(sim, 0) == CMD_REJECT_BAD_STATE,
                  "empty brainPath must return CMD_REJECT_BAD_STATE");
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_add_bot_rejects_ai_none(void) {
    ServerSim *sim = make_addbot_ready_sim();
    UT_ASSERT(sim != NULL);
    serverSimSetBotAiType(sim, aiNone);
    UT_ASSERT_MSG(apply_add_bot(sim, 0) == CMD_REJECT_BAD_STATE,
                  "aiNone must return CMD_REJECT_BAD_STATE");
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_add_bot_rejects_not_in_lobby(void) {
    ServerSim *sim = make_addbot_ready_sim();
    UT_ASSERT(sim != NULL);
    /* Game-started sims still have lobbyEnabled=true but state moves
     * past serverStateLobby — the second guard in the BAD_STATE
     * chain fires. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(apply_add_bot(sim, 0) == CMD_REJECT_BAD_STATE,
                  "state != lobby must return CMD_REJECT_BAD_STATE");
    serverSimDestroy(sim);
    return 0;
}

/* slot 5 is unconnected and not flagged admin, and openHost defaults
 * off → lobbyClientMayEdit returns false → NOT_HOST. */
int run_lobby_add_bot_rejects_non_host_sender(void) {
    ServerSim *sim = make_addbot_ready_sim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(apply_add_bot(sim, 5) == CMD_REJECT_NOT_HOST,
                  "non-host sender must return CMD_REJECT_NOT_HOST");
    serverSimDestroy(sim);
    return 0;
}

/* Seed N bot slots directly. Mirrors test_lobby_runtime_fixes.c — the
 * cap reads playerConnected + lobbyPlayers.isBot, so a real bot manager
 * (Lua brain + ClientSim alloc) isn't needed to populate the count. */
static void seed_lobby_bots(ServerSim *sim, BYTE first, BYTE count) {
    for (BYTE i = 0; i < count; i++) {
        BYTE slot = first + i;
        sim->playerConnected[slot]    = TRUE;
        sim->lobbyPlayers[slot].isBot = true;
    }
}

/* serverSimGetLobbyBotCount feeds the -maxbots comparison, so it must
 * count only connected bot slots — not the human host, not empty slots. */
int run_lobby_bot_count_counts_only_connected_bots(void) {
    ServerSim *sim = make_addbot_ready_sim();   /* human host in slot 0 */
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(serverSimGetLobbyBotCount(sim) == 0,
                  "no bots seeded yet → count 0 (host is human)");
    seed_lobby_bots(sim, 1, 3);                 /* slots 1,2,3 = bots */
    UT_ASSERT_MSG(serverSimGetLobbyBotCount(sim) == 3,
                  "three connected bot slots → count 3");
    serverSimDestroy(sim);
    return 0;
}

/* The exact predicate the dispatcher gates on: count >= maxBots. Tests
 * the boundary without depending on bot creation (which the unit build
 * stubs out), so it fails if the cap logic regresses. */
int run_lobby_maxbots_cap_boundary(void) {
    ServerSim *sim = make_addbot_ready_sim();
    UT_ASSERT(sim != NULL);
    sim->maxBots = 2;
    UT_ASSERT_MSG(serverSimGetMaxBots(sim) == 2, "maxBots accessor");

    seed_lobby_bots(sim, 1, 1);                 /* one bot, under the cap */
    UT_ASSERT_MSG(serverSimGetLobbyBotCount(sim) < serverSimGetMaxBots(sim),
                  "1 bot is below the cap of 2 → add allowed");

    seed_lobby_bots(sim, 2, 1);                 /* second bot, at the cap */
    UT_ASSERT_MSG(serverSimGetLobbyBotCount(sim) >= serverSimGetMaxBots(sim),
                  "2 bots reaches the cap of 2 → add rejected");
    serverSimDestroy(sim);
    return 0;
}

/* End-to-end through serverSimApplyCommand: with the cap reached, a host
 * add-bot is rejected. (maxBots == 0 means no cap, so the existing
 * rejection tests above run with the cap disabled.) */
int run_lobby_add_bot_rejects_when_at_maxbots(void) {
    ServerSim *sim = make_addbot_ready_sim();
    UT_ASSERT(sim != NULL);
    sim->maxBots = 2;
    seed_lobby_bots(sim, 1, 2);                 /* slots 1,2 fill the cap */
    UT_ASSERT_MSG(serverSimGetLobbyBotCount(sim) == 2,
                  "precondition: at the cap before the add attempt");
    UT_ASSERT_MSG(apply_add_bot(sim, 0) == CMD_REJECT_BOT_LIMIT,
                  "add-bot at the -maxbots cap must return CMD_REJECT_BOT_LIMIT");
    serverSimDestroy(sim);
    return 0;
}

/* The host bot capacity (the server's CPU speed test, or -botcapacity)
 * caps lobby adds as -maxbots does, and holds every add while the test
 * runs. -maxbots stays off here, so the refusal is the capacity's own. */
int run_lobby_add_bot_rejects_at_host_capacity(void) {
    ServerSim *sim = make_addbot_ready_sim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(serverSimGetHostBotCapacity(sim) ==
                      SERVER_HOST_BOT_CAPACITY_NONE,
                  "a fresh sim must have no host bot capacity");

    serverSimSetHostBotCapacity(sim, 2);
    seed_lobby_bots(sim, 1, 2);                 /* slots 1,2 fill it */
    UT_ASSERT_MSG(apply_add_bot(sim, 0) == CMD_REJECT_BOT_LIMIT,
                  "add-bot at the host bot capacity must return "
                  "CMD_REJECT_BOT_LIMIT");

    /* 0 is a server that runs no bots of its own. */
    serverSimSetHostBotCapacity(sim, 0);
    UT_ASSERT_MSG(apply_add_bot(sim, 0) == CMD_REJECT_BOT_LIMIT,
                  "a host bot capacity of 0 must refuse every add");
    serverSimDestroy(sim);

    /* While the test runs, no add, even into an empty lobby. */
    sim = make_addbot_ready_sim();
    UT_ASSERT(sim != NULL);
    serverSimSetHostBotCapacity(sim, SERVER_HOST_BOT_CAPACITY_MEASURING);
    UT_ASSERT_MSG(apply_add_bot(sim, 0) == CMD_REJECT_BOT_LIMIT,
                  "add-bot while the CPU speed test runs must return "
                  "CMD_REJECT_BOT_LIMIT");
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * The lobby's Server bot slots (LST_SERVER_BOT_SLOTS)
 * ================================================================ */
static CmdResult apply_server_bot_slots(ServerSim *sim, int senderSlot,
                                        const uint8_t *value, uint8_t len) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_SETTING;
    cmd.cmdSeq = 1;
    cmd.u.lobbySetting.settingType = LST_SERVER_BOT_SLOTS;
    cmd.u.lobbySetting.valueLen = len;
    memcpy(cmd.u.lobbySetting.value, value, len);
    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

int run_server_bot_slots_codec(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_SERVER_BOT_SLOTS);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_SERVER_BOT_SLOTS);
    ControlEvent in, out;
    uint8_t buf[8];
    size_t len = 0;
    ServerSim *sim;
    ClientSim *cs;
    ClientServerBotSlots sb;

    UT_ASSERT(enc != NULL && dec != NULL);
    UT_ASSERT(strcmp(controlEventTypeName(CTRL_SERVER_BOT_SLOTS),
                     "CTRL_SERVER_BOT_SLOTS") == 0);
    memset(&in, 0, sizeof(in));
    in.type = CTRL_SERVER_BOT_SLOTS;
    in.u.serverBotSlots.value = 6;
    in.u.serverBotSlots.recommended = 4;
    in.u.serverBotSlots.locked = 1;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(len == 3);
    UT_ASSERT(dec(buf, len, &out));
    UT_ASSERT(out.type == CTRL_SERVER_BOT_SLOTS);
    UT_ASSERT(out.u.serverBotSlots.value == 6);
    UT_ASSERT(out.u.serverBotSlots.recommended == 4);
    UT_ASSERT(out.u.serverBotSlots.locked == 1);
    buf[3] = 0x55;   /* later bytes skipped */
    UT_ASSERT(dec(buf, 4, &out));
    UT_ASSERT(!dec(buf, 2, &out));
    UT_ASSERT(enc(&in, NULL, buf, 2, &len) != ENCODE_OK);

    /* The sim's event: the test still runs, no choice yet. */
    sim = make_addbot_ready_sim();
    UT_ASSERT(sim != NULL);
    serverSimSetHostBotCapacity(sim, SERVER_HOST_BOT_CAPACITY_MEASURING);
    serverSimFillServerBotSlotsEvent(sim, &in);
    UT_ASSERT(in.u.serverBotSlots.value == LOBBY_HOST_BOT_CAPACITY_WIRE_MEASURING);
    UT_ASSERT(in.u.serverBotSlots.recommended ==
              LOBBY_HOST_BOT_CAPACITY_WIRE_MEASURING);

    /* A client: nothing until the event; then the values. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(!clientSimGetServerBotSlots(cs, &sb));
    serverSimSetHostBotCapacity(sim, 3);
    UT_ASSERT(serverSimSetServerBotSlots(sim, 5));
    serverSimFillServerBotSlotsEvent(sim, &in);
    clientSimApplyControl(cs, &in);
    UT_ASSERT(clientSimGetServerBotSlots(cs, &sb));
    UT_ASSERT(sb.value == 5 && sb.recommended == 3 && !sb.locked);
    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

/* The value in use is the lobby's choice: above the recommended value is
 * taken, past 16 is refused, and the server's placement slots follow at
 * once. */
int run_server_bot_slots_setting(void) {
    ServerSim *sim = make_addbot_ready_sim();
    uint8_t v;
    UT_ASSERT(sim != NULL);
    serverSimSetHostBotCapacity(sim, 3);
    UT_ASSERT(serverSimGetHostBotCapacity(sim) == 3);
    UT_ASSERT(serverSimGetServerBotSlots(sim) == -1);
    UT_ASSERT(serverBotHostServerSlots(sim) == 3);

    v = 5;   /* above the recommended 3 */
    UT_ASSERT(apply_server_bot_slots(sim, 0, &v, 1) == CMD_OK);
    UT_ASSERT(serverSimGetHostBotCapacity(sim) == 5);
    UT_ASSERT(serverSimGetHostBotCapacityRecommended(sim) == 3);
    UT_ASSERT_MSG(serverBotHostServerSlots(sim) == 5, "server slots %d",
                  serverBotHostServerSlots(sim));

    v = 17;
    UT_ASSERT(apply_server_bot_slots(sim, 0, &v, 1) == CMD_REJECT_INVALID);
    UT_ASSERT(serverSimGetHostBotCapacity(sim) == 5);
    v = 200;
    UT_ASSERT(apply_server_bot_slots(sim, 0, &v, 1) == CMD_REJECT_INVALID);
    UT_ASSERT(!serverSimSetServerBotSlots(sim, 17));
    UT_ASSERT(!serverSimSetServerBotSlots(sim, -1));
    {
        uint8_t two[2] = { 4, 0 };
        UT_ASSERT(apply_server_bot_slots(sim, 0, two, 2) == CMD_REJECT_INVALID);
    }
    /* Not one who may change the settings. */
    v = 1;   /* player 6: not the host, not an admin */
    UT_ASSERT(apply_server_bot_slots(sim, 5, &v, 1) == CMD_REJECT_NOT_HOST);
    UT_ASSERT(serverSimGetHostBotCapacity(sim) == 5);

    /* Lower, and 0, take at once; the Add Bot limit is the value too. */
    v = 2;
    UT_ASSERT(apply_server_bot_slots(sim, 0, &v, 1) == CMD_OK);
    UT_ASSERT(serverBotHostServerSlots(sim) == 2);
    seed_lobby_bots(sim, 2, 2);
    UT_ASSERT(apply_add_bot(sim, 0) == CMD_REJECT_BOT_LIMIT);
    v = 0;
    UT_ASSERT(apply_server_bot_slots(sim, 0, &v, 1) == CMD_OK);
    UT_ASSERT(serverSimGetHostBotCapacity(sim) == 0);
    UT_ASSERT(serverBotHostServerSlots(sim) == 0);
    serverSimDestroy(sim);
    return 0;
}

/* -botcapacity N locks it: every change refused, the value stays N. */
int run_server_bot_slots_locked(void) {
    ServerSim *sim = make_addbot_ready_sim();
    uint8_t v = 4;
    ControlEvent evt;
    UT_ASSERT(sim != NULL);
    serverSimSetHostBotCapacity(sim, 2);
    serverSimLockServerBotSlots(sim, 2);
    UT_ASSERT(serverSimIsServerBotSlotsLocked(sim));
    UT_ASSERT(apply_server_bot_slots(sim, 0, &v, 1) == CMD_REJECT_LOCKED);
    UT_ASSERT(serverSimGetHostBotCapacity(sim) == 2);
    UT_ASSERT(!serverSimSetServerBotSlots(sim, 1));
    UT_ASSERT(serverSimGetServerBotSlots(sim) == 2);
    serverSimFillServerBotSlotsEvent(sim, &evt);
    UT_ASSERT(evt.u.serverBotSlots.value == 2 && evt.u.serverBotSlots.locked);
    serverSimDestroy(sim);
    return 0;
}
