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
#include "server_sim_lifecycle.h"  /* serverSimSetBotBrainPath/AiType,
                                    * serverSimSetLobbyEnabled */
#include "threads.h"
#include "test_harness.h"

/* Lobby-enabled ServerSim with all four CMD_LOBBY_ADD_BOT
 * preconditions satisfied by default — tests then knock one guard
 * off-axis to drive a specific rejection. */
static ServerSim *make_addbot_ready_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
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
