/*
 * Coverage for the CMD_LOBBY_RELOAD_SCENARIO case in
 * server_command_dispatch.c — the lobby's own version of the dedicated
 * server console's `reload`, for a host with no console.
 *
 * The sim cannot re-read a script itself: that belongs to the scenario
 * library and the dependency points one way. So the handler asks through a
 * callback the host registers, and these cases register one of their own
 * rather than standing up a Lua VM. What they pin is the handler's contract:
 * who may ask, when, what a lobby with no scenario answers, and that a
 * refusal never reaches the callback at all.
 *
 * run_lobby_reload_scenario_needs_host    — a non-host is refused
 * run_lobby_reload_scenario_needs_lobby   — outside a lobby it is refused
 * run_lobby_reload_scenario_no_scenario   — a plain map answers rather than
 *                                           crashing, and says so
 * run_lobby_reload_scenario_calls_back    — the host's callback runs, and
 *                                           what it answers is what the
 *                                           player is told
 *
 * Drives serverSimApplyCommand directly, holding the threads mutex the
 * dispatcher asserts.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_RELOAD_SCENARIO, CmdResult */
#include "everard_map.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"   /* serverSimSetScenarioReload */
#include "threads.h"
#include "test_harness.h"

/* What the fixture callback was asked and what it answers back. */
typedef struct {
    int  calls;
    bool answer;
    char err[128];
} RsCtx;

static bool rsReload(void *ctx, char *err, size_t errLen) {
    RsCtx *c = (RsCtx *)ctx;

    c->calls++;
    if (!c->answer && err != NULL && errLen > 0) {
        snprintf(err, errLen, "%s", c->err);
    }
    return c->answer;
}

/* Host in slot 0 — lobbyClientMayEdit(sim, 0) is true — and a non-host
 * human in slot 1. */
static ServerSim *rsLobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 1, "Bob", false);
    return sim;
}

static CmdResult rsApply(ServerSim *sim, int senderSlot) {
    ClientCommand cmd;
    CmdResult r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_RELOAD_SCENARIO;
    cmd.cmdSeq = 1;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* ── Only the host may ask ────────────────────────────────────────── */

int run_lobby_reload_scenario_needs_host(void) {
    ServerSim *sim = rsLobby();
    RsCtx      c;

    UT_ASSERT(sim != NULL);
    memset(&c, 0, sizeof(c));
    c.answer = true;
    serverSimSetScenarioReload(sim, rsReload, &c);

    UT_ASSERT_MSG(rsApply(sim, 1) == CMD_REJECT_NOT_HOST,
                  "a non-host's reload was not refused");
    UT_ASSERT_MSG(c.calls == 0,
                  "a refused reload reached the callback %d times", c.calls);

    /* And the host's does go through, so the refusal above is the authority
       check and not the command being inert. */
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK, "the host's reload was refused");
    UT_ASSERT_MSG(c.calls == 1,
                  "the host's reload reached the callback %d times, wanted 1",
                  c.calls);

    serverSimDestroy(sim);
    return 0;
}

/* ── And only in a lobby ──────────────────────────────────────────── */

int run_lobby_reload_scenario_needs_lobby(void) {
    ServerSim *sim = rsLobby();
    RsCtx      c;

    UT_ASSERT(sim != NULL);
    memset(&c, 0, sizeof(c));
    c.answer = true;
    serverSimSetScenarioReload(sim, rsReload, &c);

    /* A server that runs no lobby at all has nowhere to ask from. */
    serverSimSetLobbyEnabled(sim, false);
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_REJECT_BAD_STATE,
                  "a reload outside a lobby was not refused");
    UT_ASSERT_MSG(c.calls == 0,
                  "a reload refused for state reached the callback %d times",
                  c.calls);

    serverSimDestroy(sim);
    return 0;
}

/* ── A lobby on a plain map answers ───────────────────────────────── */

int run_lobby_reload_scenario_no_scenario(void) {
    ServerSim *sim = rsLobby();

    UT_ASSERT(sim != NULL);
    /* Nothing registered, which is every lobby on a map with no script. */
    UT_ASSERT_MSG(sim->scenarioReload == NULL,
                  "setup: this case wants a sim with no reload registered");

    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_REJECT_INVALID,
                  "a reload with no scenario attached was not refused");

    /* And the sim is still usable afterwards — the point of the case is
       that asking is answered rather than fatal. */
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_REJECT_INVALID,
                  "asking twice did not answer the same way");
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 2,
                  "the roster changed across a refused reload");

    /* The helper the handler calls says why, for the line the player gets. */
    {
        char err[128];
        err[0] = 'x';
        UT_ASSERT_MSG(!serverSimScenarioReload(sim, err, sizeof(err)),
                      "a reload with nothing registered answered true");
        UT_ASSERT_MSG(err[0] != '\0' && err[0] != 'x',
                      "a reload with nothing registered left no reason");
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── What the callback answers is what comes back ─────────────────── */

int run_lobby_reload_scenario_calls_back(void) {
    ServerSim *sim = rsLobby();
    RsCtx      c;

    UT_ASSERT(sim != NULL);
    memset(&c, 0, sizeof(c));
    c.answer = true;
    serverSimSetScenarioReload(sim, rsReload, &c);

    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK,
                  "a reload the host's callback accepted was refused");
    UT_ASSERT(c.calls == 1);

    /* A script that will not read answers false with a reason, and the
       handler turns that into a refusal rather than a quiet success. */
    c.answer = false;
    snprintf(c.err, sizeof(c.err), "scenario: wave.lua:3: unexpected symbol");
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_REJECT_INVALID,
                  "a reload the callback turned down was answered CMD_OK");
    UT_ASSERT_MSG(c.calls == 2,
                  "the callback ran %d times over two reloads", c.calls);

    /* Cleared the way a detach clears it: back to answering for itself. */
    serverSimSetScenarioReload(sim, NULL, NULL);
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_REJECT_INVALID,
                  "a cleared reload was not refused");
    UT_ASSERT_MSG(c.calls == 2,
                  "a cleared reload still reached the callback");

    serverSimDestroy(sim);
    return 0;
}
