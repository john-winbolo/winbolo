/*
 * In-game voting is disabled entirely on lobby-less servers.
 *
 * A back-to-lobby / surrender vote has no lobby to return to: on a
 * -nolobby server a passed vote would terminate the process, and on a
 * -maprotate server it would blindly rotate the map. serverSimGameVoteToggle
 * guards on sim->lobbyEnabled and no-ops when there is no lobby, regardless
 * of every other precondition (running phase, two-team surrender rule, ...).
 *
 * These are the negative counterparts to test_vote_toggle_modes.c /
 * test_vote_surrender.c, whose make_sim_running() helpers enable the lobby.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h" /* serverSimGameVoteToggle — T2, exposed to tests/unit profile */
#include "server_sim_lifecycle.h"
#include "client_sim.h"     /* GAME_VOTE_KIND_*, GAME_VOTE_TOGGLE_* */
#include "everard_map.h"
#include "test_harness.h"

/* A running server with NO lobby (the -nolobby / -maprotate shape):
 * StartGame drops straight into the running phase. */
static ServerSim *make_sim_running_nolobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    return sim;
}

/* A back-to-lobby TOGGLE_YES that would open a vote on a lobby server
 * must do nothing when there is no lobby. */
int run_vote_back_to_lobby_blocked_without_lobby(void) {
    ServerSim *sim = make_sim_running_nolobby();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 1);
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetTeam(sim, 1, 2);

    serverSimGameVoteToggle(sim, 0,
                            GAME_VOTE_KIND_BACK_TO_LOBBY,
                            GAME_VOTE_TOGGLE_YES);

    UT_ASSERT_MSG(!serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY),
                  "back-to-lobby vote must not open on a lobby-less server");

    serverSimDestroy(sim);
    return 0;
}

/* Surrender's own preconditions (exactly two teams, caller on a real
 * team) are satisfied here, so this proves the lobby guard wins over
 * an otherwise-valid surrender. */
int run_vote_surrender_blocked_without_lobby(void) {
    ServerSim *sim = make_sim_running_nolobby();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 1);
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetTeam(sim, 1, 2);

    serverSimGameVoteToggle(sim, 0,
                            GAME_VOTE_KIND_SURRENDER,
                            GAME_VOTE_TOGGLE_OPEN_ONLY);

    UT_ASSERT_MSG(!serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_SURRENDER),
                  "surrender vote must not open on a lobby-less server");

    serverSimDestroy(sim);
    return 0;
}
