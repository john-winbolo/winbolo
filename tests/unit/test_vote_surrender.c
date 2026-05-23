/*
 * Surrender-vote start-path preconditions on serverSimGameVoteToggle.
 *
 * Locks two server-side rules used by the in-game vote system:
 *   - Exactly two active human teams must be in play.
 *   - The caller themselves must be on a real team (teamNumber != 0).
 *     An Unassigned caller would otherwise broadcast a fake "team 0
 *     surrender" and chain a back-to-lobby vote against two unrelated
 *     teams that happen to be playing.
 *
 * Both gates fire before any state mutation, so the assertion is
 * simply that the vote slot stays inactive.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "client_sim.h"     /* GAME_VOTE_KIND_*, GAME_VOTE_TOGGLE_* */
#include "everard_map.h"
#include "test_harness.h"

static ServerSim *make_sim_running(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    return sim;
}

/* Caller on team 0 (Unassigned) tries to surrender while two other
 * teams are actively playing. The existing 2-team precondition would
 * pass (Unassigned doesn't count toward the team total), so this
 * exercises the new teamNumber-0 guard specifically. */
int run_vote_surrender_rejects_unassigned_caller(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    /* Slot 0 is the caller, Unassigned. */
    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 0);

    /* Two real teams in play (counts as 2 active teams). */
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetTeam(sim, 1, 1);
    serverSimAddPlayer(sim, 2, "Carol", false);
    serverSimSetTeam(sim, 2, 2);

    UT_ASSERT_MSG(serverSimCountActiveTeams(sim) == 2,
                  "Unassigned caller does not count — expected exactly "
                  "two active teams, got %u",
                  (unsigned)serverSimCountActiveTeams(sim));

    serverSimGameVoteToggle(sim, 0,
                            GAME_VOTE_KIND_SURRENDER,
                            GAME_VOTE_TOGGLE_OPEN_ONLY);

    UT_ASSERT_MSG(!serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_SURRENDER),
                  "Unassigned caller must not start a surrender vote");
    UT_ASSERT_MSG(!serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY),
                  "blocked surrender must not chain a back-to-lobby vote");

    serverSimDestroy(sim);
    return 0;
}

/* Regression: caller on a real team starts the vote normally when the
 * 2-team precondition is satisfied. */
int run_vote_surrender_starts_for_real_team_caller(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 5);
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetTeam(sim, 1, 6);

    UT_ASSERT(serverSimCountActiveTeams(sim) == 2);

    serverSimGameVoteToggle(sim, 0,
                            GAME_VOTE_KIND_SURRENDER,
                            GAME_VOTE_TOGGLE_OPEN_ONLY);

    UT_ASSERT_MSG(serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_SURRENDER),
                  "happy-path surrender vote should have started "
                  "(2 active teams, caller on team 5)");

    ServerGameVoteSnapshot snap;
    UT_ASSERT(serverSimGetGameVoteSnapshot(sim, GAME_VOTE_KIND_SURRENDER, &snap));
    UT_ASSERT_MSG(snap.teamId == 5,
                  "surrender vote should target caller's team (5), got %u",
                  (unsigned)snap.teamId);

    serverSimDestroy(sim);
    return 0;
}

/* Regression: pre-existing 2-team precondition still rejects when
 * three teams have humans in play, even with a real-team caller. */
int run_vote_surrender_rejects_three_active_teams(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 5);
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetTeam(sim, 1, 6);
    serverSimAddPlayer(sim, 2, "Carol", false);
    serverSimSetTeam(sim, 2, 7);

    UT_ASSERT(serverSimCountActiveTeams(sim) == 3);

    serverSimGameVoteToggle(sim, 0,
                            GAME_VOTE_KIND_SURRENDER,
                            GAME_VOTE_TOGGLE_OPEN_ONLY);

    UT_ASSERT_MSG(!serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_SURRENDER),
                  "3-active-team surrender must still be rejected");

    serverSimDestroy(sim);
    return 0;
}
