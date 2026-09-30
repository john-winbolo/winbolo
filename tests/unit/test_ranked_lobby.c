/*
 * Ranked-lobby behaviour tests.
 *
 * Locks the rule the host wants visible to the tracker:
 *   - Ranked can be flagged on a one-player lobby. The server must
 *     accept and remember the flag so the tracker / games-list can
 *     advertise "Ranked" before anyone joins. Eligibility-by-shape
 *     (1v1 / 2v2 / 3v3) is enforced at Ready time, not at toggle time.
 *
 *   - serverSimRankedShapeReady(sim) returns false for malformed
 *     ranked shapes (1 player, uneven teams, bots) and true for
 *     1v1 / 2v2 / 3v3 equal-size human teams. This is the same
 *     predicate the server's PACKET_LOBBY_READY handler uses to
 *     silently drop Ready requests on un-qualifying ranked lobbies.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "everard_map.h"
#include "test_harness.h"

static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

int run_ranked_flag_persists_with_one_player(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    /* One human, slot 0, team 1. Shape doesn't qualify (1 team only). */
    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 1);

    /* Setting the Ranked flag must succeed regardless of shape — the
     * tracker / games-list needs to advertise the intent so other
     * players know whether to join a ranked match. */
    serverSimSetRanked(sim, true);
    UT_ASSERT_MSG(serverSimGetRanked(sim) == true,
                  "ranked flag must persist on a one-player lobby");

    /* And it must stick across the shape-check (which would have
     * forbidden Ready, but never the flag itself). */
    UT_ASSERT_MSG(!serverSimRankedShapeReady(sim),
                  "one-player lobby cannot satisfy ranked shape — "
                  "Ready should be blocked even though the flag is set");

    /* Toggle off then on again — still freely settable. */
    serverSimSetRanked(sim, false);
    UT_ASSERT(serverSimGetRanked(sim) == false);
    serverSimSetRanked(sim, true);
    UT_ASSERT(serverSimGetRanked(sim) == true);

    serverSimDestroy(sim);
    return 0;
}

int run_ranked_shape_gate(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    /* Empty lobby — no teams. */
    UT_ASSERT(!serverSimRankedShapeReady(sim));

    /* One player on team 1 — 1 team, fails the 2-teams-in-use rule. */
    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimSetTeam(sim, 0, 1);
    UT_ASSERT(!serverSimRankedShapeReady(sim));

    /* Two players, same team — still 1 team in use. */
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetTeam(sim, 1, 1);
    UT_ASSERT(!serverSimRankedShapeReady(sim));

    /* 1v1 — qualifies. */
    serverSimSetTeam(sim, 1, 2);
    UT_ASSERT_MSG(serverSimRankedShapeReady(sim),
                  "1v1 should qualify");

    /* Add a third on team 1 → uneven (2v1). */
    serverSimAddPlayer(sim, 2, "Carol", false);
    serverSimSetTeam(sim, 2, 1);
    UT_ASSERT(!serverSimRankedShapeReady(sim));

    /* Make it 2v2 by adding a fourth on team 2. */
    serverSimAddPlayer(sim, 3, "Dave", false);
    serverSimSetTeam(sim, 3, 2);
    UT_ASSERT_MSG(serverSimRankedShapeReady(sim),
                  "2v2 should qualify");

    /* 3v3 — qualifies. */
    serverSimAddPlayer(sim, 4, "Eve",   false);
    serverSimSetTeam(sim, 4, 1);
    serverSimAddPlayer(sim, 5, "Frank", false);
    serverSimSetTeam(sim, 5, 2);
    UT_ASSERT_MSG(serverSimRankedShapeReady(sim),
                  "3v3 should qualify");

    /* 4v4 exceeds the ranked cap. */
    serverSimAddPlayer(sim, 6, "Grace", false);
    serverSimSetTeam(sim, 6, 1);
    serverSimAddPlayer(sim, 7, "Heidi", false);
    serverSimSetTeam(sim, 7, 2);
    UT_ASSERT_MSG(!serverSimRankedShapeReady(sim),
                  "4v4 is above the ranked size cap (1/2/3)");

    serverSimDestroy(sim);
    return 0;
}
