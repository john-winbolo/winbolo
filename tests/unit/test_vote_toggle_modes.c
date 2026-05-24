/*
 * Wire-byte validation on serverSimGameVoteToggle.
 *
 * Locks two server-side rules independent of vote-kind preconditions:
 *   - toggleMode must be one of GAME_VOTE_TOGGLE_NO/YES/OPEN_ONLY; any
 *     other byte is dropped silently before any state mutation.
 *   - A standalone TOGGLE_NO while no vote is running has no effect.
 *     The vote-start branch would otherwise open a fresh vote and
 *     record the caller as NO+answered, which is meaningless.
 *
 * The happy-path counterparts (TOGGLE_YES opens a vote; TOGGLE_NO
 * during a running vote records the answered bit) are included as
 * regression guards so the two new gates can't grow to swallow them.
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

/* A solo NO against no running back-to-lobby vote must not open one. */
int run_vote_toggle_standalone_no_does_nothing(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 1);

    UT_ASSERT(!serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY));

    serverSimGameVoteToggle(sim, 0,
                            GAME_VOTE_KIND_BACK_TO_LOBBY,
                            GAME_VOTE_TOGGLE_NO);

    UT_ASSERT_MSG(!serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY),
                  "standalone NO must not start a back-to-lobby vote");

    serverSimDestroy(sim);
    return 0;
}

/* A toggleMode byte outside {NO, YES, OPEN_ONLY} is dropped silently. */
int run_vote_toggle_invalid_mode_dropped(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 1);

    UT_ASSERT(!serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY));

    serverSimGameVoteToggle(sim, 0,
                            GAME_VOTE_KIND_BACK_TO_LOBBY,
                            (uint8_t)99);

    UT_ASSERT_MSG(!serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY),
                  "invalid toggleMode byte must not start a vote");

    serverSimDestroy(sim);
    return 0;
}

/* Regression guard: TOGGLE_YES with no running vote still opens one. */
int run_vote_toggle_yes_opens_vote(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 1);
    /* Second human so the solo unanimity grace doesn't immediately
     * conclude the vote on the very first call. */
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetTeam(sim, 1, 2);

    serverSimGameVoteToggle(sim, 0,
                            GAME_VOTE_KIND_BACK_TO_LOBBY,
                            GAME_VOTE_TOGGLE_YES);

    UT_ASSERT_MSG(serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY),
                  "TOGGLE_YES with no running vote must open one");

    serverSimDestroy(sim);
    return 0;
}

/* Regression guard: TOGGLE_NO during a running vote records the
 * voter's answered bit but does not flip their yes bit. yesCount/
 * noCount are derived from votesMask / (answeredMask & ~votesMask),
 * so a non-zero noCount with the voter's bit absent from `votes`
 * confirms the answered-but-not-yes state. */
int run_vote_toggle_no_during_running_records_answered(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 1);
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetTeam(sim, 1, 2);

    /* Alice opens the vote (counts as YES). */
    serverSimGameVoteToggle(sim, 0,
                            GAME_VOTE_KIND_BACK_TO_LOBBY,
                            GAME_VOTE_TOGGLE_OPEN_ONLY);
    UT_ASSERT(serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY));

    /* Bob votes NO. */
    serverSimGameVoteToggle(sim, 1,
                            GAME_VOTE_KIND_BACK_TO_LOBBY,
                            GAME_VOTE_TOGGLE_NO);

    ServerGameVoteSnapshot snap;
    UT_ASSERT(serverSimGetGameVoteSnapshot(sim, GAME_VOTE_KIND_BACK_TO_LOBBY,
                                           &snap));
    UT_ASSERT_MSG((snap.votes & (uint16_t)(1u << 1)) == 0,
                  "Bob's NO must not appear in the yes-votes mask "
                  "(votes=0x%04x)", (unsigned)snap.votes);
    UT_ASSERT_MSG(snap.noCount >= 1,
                  "Bob's NO must be recorded in answeredMask "
                  "(noCount=%u)", (unsigned)snap.noCount);

    serverSimDestroy(sim);
    return 0;
}
