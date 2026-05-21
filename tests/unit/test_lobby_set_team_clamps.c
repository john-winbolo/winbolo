/*
 * Bound check on serverSimSetTeam.
 *
 * The lobby team value is used as an index into teams[MAX_TANKS]
 * (TeamMetadata teams[MAX_TANKS] in the sim). The old guard rejected
 * teamNumber > 16, which accepts teamNumber == 16 — one struct past
 * the end of teams[] and roughly 32 bytes of adjacent heap that can
 * be exfiltrated through the chat broadcast paths that copy team
 * metadata. These tests pin the corrected bound (>= MAX_TANKS gets
 * clamped) and the legal boundaries on either side of it so a future
 * loosening of the guard is caught.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "everard_map.h"
#include "test_harness.h"

static ServerSim *make_lobby_sim_with_player(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Alice", false);
    return sim;
}

static uint8_t team_of(const ServerSim *sim, BYTE slot) {
    const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, slot);
    return lp ? lp->teamNumber : 0xFFu;
}

/* teamNumber == MAX_TANKS (16) is the OOB write; it must be clamped
 * to 1 (the same fallback the old > 16 branch produced). */
int run_lobby_set_team_clamps_max_tanks(void) {
    ServerSim *sim = make_lobby_sim_with_player();
    UT_ASSERT(sim != NULL);

    serverSimSetTeam(sim, 0, (BYTE)MAX_TANKS);
    UT_ASSERT_MSG(team_of(sim, 0) != (uint8_t)MAX_TANKS,
                  "teamNumber == MAX_TANKS must not survive the setter "
                  "(would index teams[] one past the end)");
    UT_ASSERT_MSG(team_of(sim, 0) == 1,
                  "MAX_TANKS clamp should fall back to team 1, got %u",
                  (unsigned)team_of(sim, 0));

    serverSimDestroy(sim);
    return 0;
}

/* Boundary regression: the highest legal team (MAX_TANKS - 1 = 15)
 * must round-trip unchanged so the new guard hasn't accidentally
 * narrowed the legal range. */
int run_lobby_set_team_accepts_max_legal(void) {
    ServerSim *sim = make_lobby_sim_with_player();
    UT_ASSERT(sim != NULL);

    serverSimSetTeam(sim, 0, (BYTE)(MAX_TANKS - 1));
    UT_ASSERT_MSG(team_of(sim, 0) == (uint8_t)(MAX_TANKS - 1),
                  "team %u (MAX_TANKS - 1) must round-trip — it's the "
                  "highest legal index into teams[]",
                  (unsigned)(MAX_TANKS - 1));

    serverSimDestroy(sim);
    return 0;
}

/* teamNumber == 0 is the Unassigned team and stays legal — only the
 * upper bound is the OOB risk. The surrender-vote path has its own
 * rejection for Unassigned callers (test_vote_surrender.c); the
 * setter itself does not block 0. */
int run_lobby_set_team_accepts_unassigned(void) {
    ServerSim *sim = make_lobby_sim_with_player();
    UT_ASSERT(sim != NULL);

    serverSimSetTeam(sim, 0, 5);
    UT_ASSERT(team_of(sim, 0) == 5);

    serverSimSetTeam(sim, 0, 0);
    UT_ASSERT_MSG(team_of(sim, 0) == 0,
                  "team 0 (Unassigned) must remain a legal value through "
                  "the setter");

    serverSimDestroy(sim);
    return 0;
}

/* Pre-existing clamp behaviour for teamNumber > MAX_TANKS. Locks the
 * regression: tightening the guard from > 16 to >= MAX_TANKS must
 * still catch values strictly above the upper bound. */
int run_lobby_set_team_clamps_above_max(void) {
    ServerSim *sim = make_lobby_sim_with_player();
    UT_ASSERT(sim != NULL);

    serverSimSetTeam(sim, 0, 17);
    UT_ASSERT_MSG(team_of(sim, 0) == 1,
                  "teamNumber 17 should clamp to 1, got %u",
                  (unsigned)team_of(sim, 0));

    serverSimSetTeam(sim, 0, 255);
    UT_ASSERT_MSG(team_of(sim, 0) == 1,
                  "teamNumber 0xFF should clamp to 1, got %u",
                  (unsigned)team_of(sim, 0));

    serverSimDestroy(sim);
    return 0;
}
