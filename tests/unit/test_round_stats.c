/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Per-round stats accumulator semantics (PlayerRoundStats on ServerSim).
 *
 * Drives a running sim with synthetic GameEvents through the single
 * server-side funnel (serverSimAddEvent) and asserts the resulting
 * counters. These pin the intended contract for the funnel logic that
 * populates roundStats; until that logic lands, every case here except
 * the zeroed-on-fresh-sim test is expected to fail.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "input_packet.h"
#include "server_sim.h"
#include "server_sim_internal.h" /* PlayerRoundStats, serverSimGetRoundStats — T2 */
#include "server_sim_lifecycle.h"
#include "everard_map.h"
#include "test_harness.h"

/* Capture-classification byte carried in EVENT_PILL_CAPTURED /
 * EVENT_BASE_CAPTURED data[2] (server-internal; never serialized). 1B/1d
 * must use this same encoding at the capture site. */
#define CAP_FROM_NEUTRAL 0
#define CAP_FROM_ENEMY   1
#define CAP_FROM_ALLY    2

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

static void inject(ServerSim *sim, uint8_t type, const uint8_t d[8]) {
    GameEvent ev; ev.type = type; memcpy(ev.data, d, 8);
    serverSimAddEvent(sim, &ev);
}

int run_round_stats_zeroed_on_fresh_sim(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    PlayerRoundStats zero;
    memset(&zero, 0, sizeof(zero));

    for (int slot = 0; slot < MAX_TANKS; slot++) {
        const PlayerRoundStats *rs = serverSimGetRoundStats(sim, (BYTE)slot);
        UT_ASSERT_MSG(rs != NULL, "slot %d should have round stats", slot);
        UT_ASSERT_MSG(memcmp(rs, &zero, sizeof(zero)) == 0,
                      "slot %d round stats must be all-zero on a fresh sim", slot);
    }

    UT_ASSERT_MSG(serverSimGetRoundStats(sim, MAX_TANKS) == NULL,
                  "out-of-range slot must return NULL");

    serverSimDestroy(sim);
    return 0;
}

int run_round_stats_kill_basic(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    const uint8_t d[8] = { 0, 1, LAST_DEATH_BY_SHELL, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, d);

    const PlayerRoundStats *k = serverSimGetRoundStats(sim, 0);
    const PlayerRoundStats *v = serverSimGetRoundStats(sim, 1);
    UT_ASSERT(k != NULL && v != NULL);

    UT_ASSERT_MSG(k->kills == 1, "killer kills, got %u", k->kills);
    UT_ASSERT_MSG(v->deaths == 1, "victim deaths, got %u", v->deaths);
    UT_ASSERT_MSG(k->killsOf[1] == 1, "killsOf[victim], got %u", k->killsOf[1]);
    UT_ASSERT_MSG(v->killedBy[0] == 1, "killedBy[killer], got %u", v->killedBy[0]);
    UT_ASSERT_MSG(k->suicides == 0, "kill is not a suicide, got %u", k->suicides);

    serverSimDestroy(sim);
    return 0;
}

int run_round_stats_drown_not_suicide(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    /* killer==killed, as the real drown path emits it. */
    const uint8_t d[8] = { 1, 1, LAST_DEATH_BY_DEEPSEA, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, d);

    const PlayerRoundStats *s = serverSimGetRoundStats(sim, 1);
    UT_ASSERT(s != NULL);
    UT_ASSERT_MSG(s->deaths == 1, "drown counts a death, got %u", s->deaths);
    UT_ASSERT_MSG(s->drowns == 1, "drown counted, got %u", s->drowns);
    UT_ASSERT_MSG(s->suicides == 0, "drown is not a suicide, got %u", s->suicides);
    UT_ASSERT_MSG(s->kills == 0, "drown credits no kill, got %u", s->kills);

    serverSimDestroy(sim);
    return 0;
}

int run_round_stats_suicide_shell(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    const uint8_t d[8] = { 2, 2, LAST_DEATH_BY_SHELL, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, d);

    const PlayerRoundStats *s = serverSimGetRoundStats(sim, 2);
    UT_ASSERT(s != NULL);
    UT_ASSERT_MSG(s->deaths == 1, "suicide counts a death, got %u", s->deaths);
    UT_ASSERT_MSG(s->suicides == 1, "suicide counted, got %u", s->suicides);
    UT_ASSERT_MSG(s->kills == 0, "suicide credits no kill, got %u", s->kills);
    UT_ASSERT_MSG(s->drowns == 0, "shell suicide is not a drown, got %u", s->drowns);

    serverSimDestroy(sim);
    return 0;
}

int run_round_stats_mine_death(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    /* 0xFF = no valid killer (environmental). */
    const uint8_t d[8] = { 0xFF, 3, LAST_DEATH_BY_MINES, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, d);

    const PlayerRoundStats *s = serverSimGetRoundStats(sim, 3);
    UT_ASSERT(s != NULL);
    UT_ASSERT_MSG(s->deaths == 1, "mine death counts a death, got %u", s->deaths);
    UT_ASSERT_MSG(s->mineDeaths == 1, "mine death counted, got %u", s->mineDeaths);

    for (int slot = 0; slot < MAX_TANKS; slot++) {
        const PlayerRoundStats *rs = serverSimGetRoundStats(sim, (BYTE)slot);
        UT_ASSERT_MSG(rs->kills == 0,
                      "no-killer mine death credits no kill (slot %d), got %u",
                      slot, rs->kills);
    }

    serverSimDestroy(sim);
    return 0;
}

int run_round_stats_trees_wasted_and_pills_dropped(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    /* data[3] = carriedPills, data[4] = treesCarried (server-internal). */
    const uint8_t d1[8] = { 0, 4, LAST_DEATH_BY_SHELL, 2, 7, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, d1);

    const PlayerRoundStats *s = serverSimGetRoundStats(sim, 4);
    UT_ASSERT(s != NULL);
    UT_ASSERT_MSG(s->treesWasted == 7, "trees wasted, got %u", s->treesWasted);
    UT_ASSERT_MSG(s->mostPillsDropped == 2,
                  "most pills dropped, got %u", s->mostPillsDropped);

    const uint8_t d2[8] = { 0, 4, LAST_DEATH_BY_SHELL, 1, 3, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, d2);

    UT_ASSERT_MSG(s->treesWasted == 10,
                  "trees wasted accumulates, got %u", s->treesWasted);
    UT_ASSERT_MSG(s->mostPillsDropped == 2,
                  "most pills dropped is a per-death max, got %u",
                  s->mostPillsDropped);

    serverSimDestroy(sim);
    return 0;
}

int run_round_stats_capture_steal(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    const PlayerRoundStats *s = serverSimGetRoundStats(sim, 0);
    UT_ASSERT(s != NULL);

    /* Neutral capture. */
    const uint8_t cap[8] = { 0, 0xFF, CAP_FROM_NEUTRAL, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_PILL_CAPTURED, cap);
    UT_ASSERT_MSG(s->pillCaptures == 1, "neutral capture, got %u", s->pillCaptures);
    UT_ASSERT_MSG(s->steals == 0, "neutral capture is no steal, got %u", s->steals);

    /* Steal from an enemy — counts as both a capture and a steal. */
    const uint8_t steal[8] = { 0, 1, CAP_FROM_ENEMY, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_PILL_CAPTURED, steal);
    UT_ASSERT_MSG(s->pillCaptures == 2, "steal counts as capture, got %u", s->pillCaptures);
    UT_ASSERT_MSG(s->steals == 1, "steal counted, got %u", s->steals);

    /* Allied take — counts for nobody. */
    const uint8_t ally[8] = { 0, 2, CAP_FROM_ALLY, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_PILL_CAPTURED, ally);
    UT_ASSERT_MSG(s->pillCaptures == 2, "allied take is no capture, got %u", s->pillCaptures);
    UT_ASSERT_MSG(s->steals == 1, "allied take is no steal, got %u", s->steals);

    serverSimDestroy(sim);
    return 0;
}

int run_round_stats_base_capture_steal(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    const PlayerRoundStats *s = serverSimGetRoundStats(sim, 0);
    UT_ASSERT(s != NULL);

    const uint8_t cap[8] = { 0, 0xFF, CAP_FROM_NEUTRAL, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_BASE_CAPTURED, cap);
    UT_ASSERT_MSG(s->baseCaptures == 1, "neutral base capture, got %u", s->baseCaptures);
    UT_ASSERT_MSG(s->steals == 0, "neutral base capture is no steal, got %u", s->steals);

    const uint8_t steal[8] = { 0, 1, CAP_FROM_ENEMY, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_BASE_CAPTURED, steal);
    UT_ASSERT_MSG(s->baseCaptures == 2, "base steal counts as capture, got %u", s->baseCaptures);
    UT_ASSERT_MSG(s->steals == 1, "base steal counted, got %u", s->steals);

    const uint8_t ally[8] = { 0, 2, CAP_FROM_ALLY, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_BASE_CAPTURED, ally);
    UT_ASSERT_MSG(s->baseCaptures == 2, "allied base take is no capture, got %u", s->baseCaptures);
    UT_ASSERT_MSG(s->steals == 1, "allied base take is no steal, got %u", s->steals);

    serverSimDestroy(sim);
    return 0;
}

int run_round_stats_lgm(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    /* EVENT_LGM_LOST data: [victim, killer]. */
    const uint8_t killed[8] = { 5, 0, 0, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_LGM_LOST, killed);

    const PlayerRoundStats *victim = serverSimGetRoundStats(sim, 5);
    const PlayerRoundStats *killer = serverSimGetRoundStats(sim, 0);
    UT_ASSERT(victim != NULL && killer != NULL);
    UT_ASSERT_MSG(victim->lgmDeaths == 1, "lgm death counted, got %u", victim->lgmDeaths);
    UT_ASSERT_MSG(killer->lgmKills == 1, "lgm kill credited, got %u", killer->lgmKills);

    /* No valid killer — death counts, no kill credited. */
    const uint8_t nokill[8] = { 6, 0xFF, 0, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_LGM_LOST, nokill);

    const PlayerRoundStats *victim2 = serverSimGetRoundStats(sim, 6);
    UT_ASSERT(victim2 != NULL);
    UT_ASSERT_MSG(victim2->lgmDeaths == 1, "second lgm death counted, got %u", victim2->lgmDeaths);
    UT_ASSERT_MSG(killer->lgmKills == 1,
                  "no-killer lgm loss credits no kill, got %u", killer->lgmKills);

    serverSimDestroy(sim);
    return 0;
}
