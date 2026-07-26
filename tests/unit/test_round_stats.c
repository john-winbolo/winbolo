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
#include "client_sim.h"          /* ClientSim, clientSimGetLastRoundStats accessor */
#include "client_sim_control.h"  /* clientSimApplyControl */
#include "server_sim.h"
#include "server_sim_internal.h" /* PlayerRoundStats, serverSimGetRoundStats — T2 */
#include "server_sim_lifecycle.h"
#include "round_stats.h"         /* AwardId, AwardResult, PlayerRoundStats */
#include "round_stats_derive.h"  /* computeAwards, roundStatsApplyRecord, roundStatsRecordSize */
#include "control_event.h"       /* ControlEvent, CTRL_ROUND_STATS */
#include "transport_control_codec.h"
#include "transport_udp_internal.h" /* PACKET_HEADER_SIZE, MAX_CONTROL_PACKET */
#include "netpacks.h"            /* PACKET_ROUND_STATS */
#include "everard_map.h"
#include "test_harness.h"

/* Locate a computed award by id; NULL if it was omitted. Keeps the
 * award-test assertions terse. */
static const AwardResult *find_award(const AwardResult *res, int n, int id) {
    for (int i = 0; i < n; i++) {
        if (res[i].awardId == id) {
            return &res[i];
        }
    }
    return NULL;
}

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
    const uint8_t cap[8] = { 0, 0xFF, CAPTURE_CLASS_NEUTRAL, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_PILL_CAPTURED, cap);
    UT_ASSERT_MSG(s->pillCaptures == 1, "neutral capture, got %u", s->pillCaptures);
    UT_ASSERT_MSG(s->steals == 0, "neutral capture is no steal, got %u", s->steals);

    /* Steal from an enemy — counts as both a capture and a steal. */
    const uint8_t steal[8] = { 0, 1, CAPTURE_CLASS_ENEMY, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_PILL_CAPTURED, steal);
    UT_ASSERT_MSG(s->pillCaptures == 2, "steal counts as capture, got %u", s->pillCaptures);
    UT_ASSERT_MSG(s->steals == 1, "steal counted, got %u", s->steals);

    /* Allied take — counts for nobody. */
    const uint8_t ally[8] = { 0, 2, CAPTURE_CLASS_ALLY, 0, 0, 0, 0, 0 };
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

    const uint8_t cap[8] = { 0, 0xFF, CAPTURE_CLASS_NEUTRAL, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_BASE_CAPTURED, cap);
    UT_ASSERT_MSG(s->baseCaptures == 1, "neutral base capture, got %u", s->baseCaptures);
    UT_ASSERT_MSG(s->steals == 0, "neutral base capture is no steal, got %u", s->steals);

    const uint8_t steal[8] = { 0, 1, CAPTURE_CLASS_ENEMY, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_BASE_CAPTURED, steal);
    UT_ASSERT_MSG(s->baseCaptures == 2, "base steal counts as capture, got %u", s->baseCaptures);
    UT_ASSERT_MSG(s->steals == 1, "base steal counted, got %u", s->steals);

    const uint8_t ally[8] = { 0, 2, CAPTURE_CLASS_ALLY, 0, 0, 0, 0, 0 };
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

/* Stats are round-scoped: they survive the game-over hold but are cleared
 * when the next round starts. */
int run_round_stats_lifecycle_reset(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    const uint8_t d[8] = { 0, 1, LAST_DEATH_BY_SHELL, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, d);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->kills == 1,
                  "kill recorded during running, got %u",
                  serverSimGetRoundStats(sim, 0)->kills);

    serverSimEnterGameOver(sim);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->kills == 1,
                  "kill must survive the game-over hold, got %u",
                  serverSimGetRoundStats(sim, 0)->kills);

    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->kills == 0,
                  "next round start must clear stats, got %u",
                  serverSimGetRoundStats(sim, 0)->kills);

    serverSimDestroy(sim);
    return 0;
}

/* The notable-event timeline is reset alongside the counters at round start. */
int run_round_stats_reset_clears_notables(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    const uint8_t kill[8]    = { 0, 1, LAST_DEATH_BY_SHELL, 0, 0, 0, 0, 0 };
    const uint8_t capture[8] = { 0, 0xFF, CAPTURE_CLASS_NEUTRAL, 0, 0, 0, 0, 0 };
    const uint8_t lgm[8]     = { 5, 0, 0, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, kill);
    inject(sim, EVENT_PILL_CAPTURED, capture);
    inject(sim, EVENT_LGM_LOST, lgm);
    UT_ASSERT_MSG(sim->notableEventCount == 3,
                  "three notable events recorded, got %u",
                  (unsigned)sim->notableEventCount);

    serverSimStartGame(sim);
    UT_ASSERT_MSG(sim->notableEventCount == 0,
                  "next round start must clear the notable timeline, got %u",
                  (unsigned)sim->notableEventCount);

    serverSimDestroy(sim);
    return 0;
}

/* The timeline preserves event order and records doer/target plus the
 * running round tick for each entry. */
int run_round_stats_notables_ordered(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    const uint8_t kill[8]    = { 0, 1, LAST_DEATH_BY_SHELL, 0, 0, 0, 0, 0 };
    const uint8_t pillCap[8] = { 0, 0xFF, CAPTURE_CLASS_NEUTRAL, 0, 0, 0, 0, 0 };
    const uint8_t baseCap[8] = { 2, 0xFF, CAPTURE_CLASS_NEUTRAL, 0, 0, 0, 0, 0 };
    const uint8_t lgm[8]     = { 5, 3, 0, 0, 0, 0, 0, 0 };  /* victim 5, killer 3 */
    inject(sim, EVENT_TANK_KILLED, kill);
    inject(sim, EVENT_PILL_CAPTURED, pillCap);
    inject(sim, EVENT_BASE_CAPTURED, baseCap);
    inject(sim, EVENT_LGM_LOST, lgm);

    UT_ASSERT_MSG(sim->notableEventCount == 4,
                  "four notable events recorded, got %u",
                  (unsigned)sim->notableEventCount);

    UT_ASSERT(sim->notableEvents[0].type == NOTABLE_KILL);
    UT_ASSERT(sim->notableEvents[1].type == NOTABLE_PILL_CAPTURE);
    UT_ASSERT(sim->notableEvents[2].type == NOTABLE_BASE_CAPTURE);
    UT_ASSERT(sim->notableEvents[3].type == NOTABLE_LGM_LOST);

    /* Kill records doer then target; LGM records killer then victim. */
    UT_ASSERT(sim->notableEvents[0].actorA == 0 && sim->notableEvents[0].actorB == 1);
    UT_ASSERT(sim->notableEvents[3].actorA == 3 && sim->notableEvents[3].actorB == 5);

    for (int i = 0; i < sim->notableEventCount; i++) {
        UT_ASSERT_MSG(sim->notableEvents[i].tick == sim->tick,
                      "entry %d tick should match the round tick", i);
    }

    serverSimDestroy(sim);
    return 0;
}

/* A player removed mid-round vanishes from the round summary: their own
 * accumulator row is zeroed, the kill matrix loses every reference to them,
 * and their notable-event entries are pruned — while other players' aggregate
 * counters that happened to involve them are left untouched. */
int run_round_stats_leaver_dropped(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);

    const uint8_t kill01[8]  = { 0, 1, LAST_DEATH_BY_SHELL, 0, 0, 0, 0, 0 };
    const uint8_t kill12[8]  = { 1, 2, LAST_DEATH_BY_SHELL, 0, 0, 0, 0, 0 };
    const uint8_t capture[8] = { 2, 0xFF, CAPTURE_CLASS_NEUTRAL, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, kill01);
    inject(sim, EVENT_TANK_KILLED, kill12);
    inject(sim, EVENT_PILL_CAPTURED, capture);

    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 1)->kills == 1,
                  "P1 kill recorded, got %u", serverSimGetRoundStats(sim, 1)->kills);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 1)->deaths == 1,
                  "P1 death recorded, got %u", serverSimGetRoundStats(sim, 1)->deaths);
    UT_ASSERT_MSG(sim->notableEventCount == 3,
                  "three notable events recorded, got %u",
                  (unsigned)sim->notableEventCount);

    serverSimRemovePlayer(sim, 1);

    /* Leaver's own row is zeroed. */
    PlayerRoundStats zero;
    memset(&zero, 0, sizeof(zero));
    UT_ASSERT_MSG(memcmp(serverSimGetRoundStats(sim, 1), &zero, sizeof(zero)) == 0,
                  "leaver's round stats must be all-zero");

    /* Kill matrix column referencing the leaver is cleared. */
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->killsOf[1] == 0,
                  "killsOf[leaver] cleared, got %u",
                  serverSimGetRoundStats(sim, 0)->killsOf[1]);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 2)->killedBy[1] == 0,
                  "killedBy[leaver] cleared, got %u",
                  serverSimGetRoundStats(sim, 2)->killedBy[1]);

    /* Other players' aggregates are not unwound. */
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->kills == 1,
                  "P0's kill of the leaver is retained, got %u",
                  serverSimGetRoundStats(sim, 0)->kills);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 2)->pillCaptures == 1,
                  "P2's capture is retained, got %u",
                  serverSimGetRoundStats(sim, 2)->pillCaptures);

    /* Notable timeline pruned of the leaver's two kills; the capture survives. */
    UT_ASSERT_MSG(sim->notableEventCount == 1,
                  "timeline pruned to one entry, got %u",
                  (unsigned)sim->notableEventCount);
    UT_ASSERT(sim->notableEvents[0].type == NOTABLE_PILL_CAPTURE);

    serverSimDestroy(sim);
    return 0;
}

/* The recordDamage / recordPlayerAction attribution callbacks bump the
 * round accumulator directly. Drive them through the installed pointers
 * (ctx is the ServerSim) and assert the per-slot counters; a NEUTRAL
 * attacker is ignored. */
int run_round_stats_attribution_callbacks(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    void *ctx = sim->sim.callbacks.ctx;
    UT_ASSERT(sim->sim.callbacks.recordDamage != NULL);
    UT_ASSERT(sim->sim.callbacks.recordPlayerAction != NULL);

    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_TANK, 0, DMG_SRC_SHELL, 10, false, 0, 0);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToPlayers == 10,
                  "tank damage, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToPlayers);

    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_PILL, 0, DMG_SRC_SHELL, 5, false, 0, 0);
    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_PILL, 0, DMG_SRC_SHELL, 3, true, 0, 0);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToPills == 8,
                  "pill damage accumulates, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToPills);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->pillKills == 1,
                  "destroyed blow credits a pill kill, got %u",
                  serverSimGetRoundStats(sim, 0)->pillKills);

    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_BASE, 0, DMG_SRC_SHELL, 7, false, 0, 0);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToBases == 7,
                  "base damage, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToBases);

    /* NEUTRAL attacker (0xFF) is out of range and must be ignored. */
    sim->sim.callbacks.recordDamage(ctx, 0xFF, DMG_TARGET_TANK, 0, DMG_SRC_SHELL, 99, false, 0, 0);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToPlayers == 10,
                  "NEUTRAL attacker ignored, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToPlayers);

    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_FARM, 0, 0);
    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_BUILD, 0, 0);
    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_MINE, 0, 0);
    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_SHELL, 0, 0);

    const PlayerRoundStats *p1 = serverSimGetRoundStats(sim, 1);
    UT_ASSERT(p1 != NULL);
    UT_ASSERT_MSG(p1->treesFarmed == 1, "farm counted, got %u", p1->treesFarmed);
    UT_ASSERT_MSG(p1->pillsBuilt == 1, "build counted, got %u", p1->pillsBuilt);
    UT_ASSERT_MSG(p1->minesLaid == 1, "mine counted, got %u", p1->minesLaid);
    UT_ASSERT_MSG(p1->shellsFired == 1, "shell counted, got %u", p1->shellsFired);

    serverSimDestroy(sim);
    return 0;
}

/* End-to-end: a direct shell hit on a live tank credits effective tank
 * damage to the shooter through the real combat path. Covers both the
 * current-position and lag-compensated hit functions. */
int run_round_stats_direct_damage(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, false);
    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimStartGame(sim);

    /* If tanks aren't created for connected players, this test can't run. */
    UT_ASSERT(sim->sim.tanks[1] != NULL);

    WORLD wx = 0, wy = 0;
    UT_ASSERT(serverSimGetTankState(sim, 1, &wx, &wy));

    tankIsTankHit(&sim->sim, &sim->sim.tanks[1], wx, wy, 0, 0);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToPlayers == DAMAGE,
                  "current-position hit credits one DAMAGE, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToPlayers);

    tankIsTankHitAtPosition(&sim->sim, &sim->sim.tanks[1], wx, wy, wx, wy, 0, 0);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToPlayers == 2 * DAMAGE,
                  "lag-compensated hit credits a second DAMAGE, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToPlayers);

    serverSimDestroy(sim);
    return 0;
}

/* The mine-ownership grid records the layer per cell, releases a cell when
 * the mine is removed, and clears every cell owned by one player. */
int run_round_stats_mine_owner_api(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(minesGetOwner(&sim->sim.mns, 0, 0) == NEUTRAL,
                  "cells default to NEUTRAL, got %u",
                  minesGetOwner(&sim->sim.mns, 0, 0));

    minesSetOwner(&sim->sim.mns, 5, 6, 3);
    minesSetOwner(&sim->sim.mns, 7, 8, 3);
    minesSetOwner(&sim->sim.mns, 9, 10, 4);

    minesClearOwner(&sim->sim.mns, 3);
    UT_ASSERT_MSG(minesGetOwner(&sim->sim.mns, 5, 6) == NEUTRAL,
                  "cleared owner's cell is NEUTRAL, got %u",
                  minesGetOwner(&sim->sim.mns, 5, 6));
    UT_ASSERT_MSG(minesGetOwner(&sim->sim.mns, 7, 8) == NEUTRAL,
                  "cleared owner's other cell is NEUTRAL, got %u",
                  minesGetOwner(&sim->sim.mns, 7, 8));
    UT_ASSERT_MSG(minesGetOwner(&sim->sim.mns, 9, 10) == 4,
                  "another owner's cell is untouched, got %u",
                  minesGetOwner(&sim->sim.mns, 9, 10));

    minesRemoveItem(&sim->sim.mns, 9, 10);
    UT_ASSERT_MSG(minesGetOwner(&sim->sim.mns, 9, 10) == NEUTRAL,
                  "removing a mine releases the cell, got %u",
                  minesGetOwner(&sim->sim.mns, 9, 10));

    serverSimDestroy(sim);
    return 0;
}

/* A mine detonation credits effective tank damage to the player who laid it. */
int run_round_stats_mine_damage(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, false);
    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimStartGame(sim);

    UT_ASSERT(sim->sim.tanks[0] != NULL);

    BYTE mx = tankGetMX(&sim->sim.tanks[0]);
    BYTE my = tankGetMY(&sim->sim.tanks[0]);
    minesSetOwner(&sim->sim.mns, mx, my, 1);
    tankMineDamage(&sim->sim, &sim->sim.tanks[0], mx, my, 1);

    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 1)->dmgToPlayers == MINE_DAMAGE,
                  "mine credits the layer effective damage, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 1)->dmgToPlayers);

    serverSimDestroy(sim);
    return 0;
}

/* A player removed mid-round has their mine cells released. */
int run_round_stats_leaver_clears_mines(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, false);
    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimStartGame(sim);

    minesSetOwner(&sim->sim.mns, 12, 13, 1);
    UT_ASSERT(minesGetOwner(&sim->sim.mns, 12, 13) == 1);

    serverSimRemovePlayer(sim, 1);
    UT_ASSERT_MSG(minesGetOwner(&sim->sim.mns, 12, 13) == NEUTRAL,
                  "leaver's mine cell is released, got %u",
                  minesGetOwner(&sim->sim.mns, 12, 13));

    serverSimDestroy(sim);
    return 0;
}

/* Every award resolves to a single deterministic winner. Stats are laid out
 * so each of the 18 awards has an unambiguous best slot; the ratio awards
 * (Best K/D, Sharpshooter, Survivor) carry their ×100 value and Warmonger's
 * value is the raw player damage, not its (penalised) ranking score. */
int run_awards_basic_winners(void) {
    PlayerRoundStats stats[MAX_TANKS];
    bool isBot[MAX_TANKS];
    memset(stats, 0, sizeof(stats));
    memset(isBot, 0, sizeof(isBot));

    /* slot 0 — fighter: kills, K/D, nemesis (owns slot 1), warmonger. */
    stats[0].kills = 10;
    stats[0].deaths = 2;            /* K/D = 10*100/2 = 500 */
    stats[0].killsOf[1] = 4;        /* nemesis victim = 1 */
    stats[0].killsOf[2] = 1;
    stats[0].dmgToPlayers = 1000;   /* warmonger key 1000, no captures */

    /* slot 1 — victim: deaths, fish food. */
    stats[1].kills = 1;
    stats[1].deaths = 9;
    stats[1].drowns = 5;

    /* slot 2 — builder/capper: captures, engineer, sapper, lumberjack, survivor. */
    stats[2].baseCaptures = 3;
    stats[2].pillCaptures = 5;
    stats[2].minesLaid = 8;
    stats[2].pillsBuilt = 4;
    stats[2].treesFarmed = 6;       /* engineer 10, survivor (6+4+8)*100/1 = 1800 */

    /* slot 3 — gunner: demolition, sharpshooter. */
    stats[3].dmgToPills = 200;
    stats[3].dmgToBases = 300;      /* demolition 500 */
    stats[3].dmgToPlayers = 400;
    stats[3].shellsFired = 20;      /* sharpshooter 400*100/20 = 2000 */

    /* slot 4 — the rest of the fun awards. */
    stats[4].lgmKills = 7;
    stats[4].lgmDeaths = 6;
    stats[4].treesWasted = 9;
    stats[4].mostPillsDropped = 3;

    AwardResult out[AWARD_COUNT];
    int n = 0;
    computeAwards(stats, 5, true, isBot, out, &n);

    UT_ASSERT_MSG(n == AWARD_COUNT, "all 18 awards won, got %d", n);

    const AwardResult *a;
    #define EXPECT(id, slot, val)                                         \
        do {                                                              \
            a = find_award(out, n, (id));                                 \
            UT_ASSERT_MSG(a != NULL, #id " should be present");           \
            UT_ASSERT_MSG(a->winnerSlot == (slot),                       \
                          #id " winner slot, got %u", a->winnerSlot);     \
            UT_ASSERT_MSG(a->value == (val),                              \
                          #id " value, got %u", a->value);                \
        } while (0)

    EXPECT(AWARD_MOST_KILLS, 0, 10);
    EXPECT(AWARD_MOST_DEATHS, 1, 9);
    EXPECT(AWARD_BEST_KD, 0, 500);
    EXPECT(AWARD_MOST_BASE_CAPTURES, 2, 3);
    EXPECT(AWARD_MOST_PILL_CAPTURES, 2, 5);
    EXPECT(AWARD_NEMESIS, 0, 4);
    EXPECT(AWARD_DEMOLITION, 3, 500);
    EXPECT(AWARD_SHARPSHOOTER, 3, 2000);
    EXPECT(AWARD_WARMONGER, 0, 1000);   /* value == dmgToPlayers, not the key */
    EXPECT(AWARD_SURVIVOR, 2, 1800);
    EXPECT(AWARD_ENGINEER, 2, 10);
    EXPECT(AWARD_SAPPER, 2, 8);
    EXPECT(AWARD_FISH_FOOD, 1, 5);
    EXPECT(AWARD_LGM_HUNTER, 4, 7);
    EXPECT(AWARD_CANNON_FODDER, 4, 6);
    EXPECT(AWARD_LUMBERJACK, 2, 6);
    EXPECT(AWARD_WASTEFUL, 4, 9);
    EXPECT(AWARD_BIGGEST_FUMBLE, 4, 3);
    #undef EXPECT

    /* Nemesis names the victim; everyone else leaves it NEUTRAL. */
    a = find_award(out, n, AWARD_NEMESIS);
    UT_ASSERT_MSG(a->subjectSlot == 1, "nemesis victim, got %u", a->subjectSlot);
    a = find_award(out, n, AWARD_MOST_KILLS);
    UT_ASSERT_MSG(a->subjectSlot == NEUTRAL,
                  "non-nemesis subject is NEUTRAL, got %u", a->subjectSlot);

    return 0;
}

/* On a tie the lowest slot index wins, so output is deterministic. */
int run_awards_tiebreak(void) {
    PlayerRoundStats stats[MAX_TANKS];
    bool isBot[MAX_TANKS];
    memset(stats, 0, sizeof(stats));
    memset(isBot, 0, sizeof(isBot));

    stats[2].kills = 5;
    stats[3].kills = 5;

    AwardResult out[AWARD_COUNT];
    int n = 0;
    computeAwards(stats, 5, true, isBot, out, &n);

    const AwardResult *a = find_award(out, n, AWARD_MOST_KILLS);
    UT_ASSERT_MSG(a != NULL, "most kills present");
    UT_ASSERT_MSG(a->winnerSlot == 2, "lower slot wins the tie, got %u",
                  a->winnerSlot);

    return 0;
}

/* No qualifying player for any award → nothing is emitted. */
int run_awards_omission(void) {
    PlayerRoundStats stats[MAX_TANKS];
    bool isBot[MAX_TANKS];
    memset(stats, 0, sizeof(stats));
    memset(isBot, 0, sizeof(isBot));

    AwardResult out[AWARD_COUNT];
    int n = -1;
    computeAwards(stats, 5, true, isBot, out, &n);

    UT_ASSERT_MSG(n == 0, "all-zero stats win no awards, got %d", n);

    return 0;
}

/* Per-award floors gate the ratio/damage awards: a nonzero but sub-floor
 * stat does not win Best K/D, Sharpshooter, or Warmonger. */
int run_awards_floors(void) {
    PlayerRoundStats stats[MAX_TANKS];
    bool isBot[MAX_TANKS];
    memset(stats, 0, sizeof(stats));
    memset(isBot, 0, sizeof(isBot));

    stats[0].kills = 2;             /* < AWARD_MIN_KILLS_KD (3) */
    stats[0].deaths = 1;
    stats[0].shellsFired = 10;      /* < AWARD_MIN_SHELLS_ACC (20) */
    stats[0].dmgToPlayers = 30;     /* < AWARD_MIN_DMG_WARMONGER (40) */

    AwardResult out[AWARD_COUNT];
    int n = 0;
    computeAwards(stats, 4, true, isBot, out, &n);

    UT_ASSERT_MSG(find_award(out, n, AWARD_BEST_KD) == NULL,
                  "sub-floor kills win no Best K/D");
    UT_ASSERT_MSG(find_award(out, n, AWARD_SHARPSHOOTER) == NULL,
                  "sub-floor shells win no Sharpshooter");
    UT_ASSERT_MSG(find_award(out, n, AWARD_WARMONGER) == NULL,
                  "sub-floor damage wins no Warmonger");

    /* The non-floored kills award is still won. */
    const AwardResult *a = find_award(out, n, AWARD_MOST_KILLS);
    UT_ASSERT_MSG(a != NULL && a->winnerSlot == 0 && a->value == 2,
                  "Most Kills still awarded");

    return 0;
}

/* includeBots gates eligibility only: with it on a bot can win; with it off
 * the award falls to the best human, or is omitted when only a bot qualifies. */
int run_awards_include_bots(void) {
    PlayerRoundStats stats[MAX_TANKS];
    bool isBot[MAX_TANKS];
    memset(stats, 0, sizeof(stats));
    memset(isBot, 0, sizeof(isBot));

    isBot[0] = true;
    stats[0].kills = 10;            /* top scorer is a bot */
    stats[0].drowns = 3;            /* only the bot drowned */
    stats[1].kills = 5;             /* best human */

    AwardResult out[AWARD_COUNT];
    int n = 0;

    /* Bots eligible: the bot wins Most Kills. */
    computeAwards(stats, 2, true, isBot, out, &n);
    const AwardResult *a = find_award(out, n, AWARD_MOST_KILLS);
    UT_ASSERT_MSG(a != NULL && a->winnerSlot == 0, "bot wins Most Kills when included");
    UT_ASSERT_MSG(a->winnerIsBot == 1, "winnerIsBot flagged, got %u", a->winnerIsBot);
    UT_ASSERT_MSG(a->value == 10, "bot value, got %u", a->value);

    /* Bots excluded: the award falls to the best human. */
    computeAwards(stats, 2, false, isBot, out, &n);
    a = find_award(out, n, AWARD_MOST_KILLS);
    UT_ASSERT_MSG(a != NULL && a->winnerSlot == 1, "human wins Most Kills when bots excluded");
    UT_ASSERT_MSG(a->winnerIsBot == 0, "human winner not flagged, got %u", a->winnerIsBot);
    UT_ASSERT_MSG(a->value == 5, "human value, got %u", a->value);

    /* An award only a bot qualifies for is omitted when bots are excluded. */
    UT_ASSERT_MSG(find_award(out, n, AWARD_FISH_FOOD) == NULL,
                  "bot-only Fish Food omitted when bots excluded");

    return 0;
}

/* Encode a CTRL_ROUND_STATS event through the full encoder, confirm it
 * carries PACKET_ROUND_STATS, then decode the body back. Mirrors the
 * alliance-reset codec helper. */
static int codec_roundtrip_round_stats(const ControlEvent *in, ControlEvent *out) {
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;
    ControlEncodeFn enc = transportControlCodecEncoder(CTRL_ROUND_STATS);
    if (enc == NULL) return -1;
    EncodeResult r = enc(in, NULL, buf, sizeof(buf), &outLen);
    if (r != ENCODE_OK) return -2;
    if (outLen < PACKET_HEADER_SIZE) return -3;
    uint8_t packetType = buf[2];   /* packHeader writes packet type at byte 2 */
    if (packetType != PACKET_ROUND_STATS) return -4;
    ControlDecodeFn dec = transportControlCodecDecoder(packetType);
    if (dec == NULL) return -5;
    if (!dec(buf + PACKET_HEADER_SIZE, outLen - PACKET_HEADER_SIZE, out)) return -6;
    return 0;
}

/* Every field of a populated RoundStatsSummary survives encode→decode. */
int run_round_stats_codec_roundtrip(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.type = CTRL_ROUND_STATS;
    RoundStatsSummary *s = &in.u.roundStats;

    s->playerCount = 3;
    s->players[0].slot = 0;  s->players[0].isBot = 0;  s->players[0].kills = 12;
    s->players[0].deaths = 3; s->players[0].baseCaptures = 2;
    s->players[0].pillCaptures = 5; s->players[0].dmgDealt = 123456u;
    s->players[0].builds = 7;
    s->players[0].lgmKills = 11; s->players[0].lgmDeaths = 4;
    s->players[1].slot = 4;  s->players[1].isBot = 1;  s->players[1].kills = 0;
    s->players[1].deaths = 9; s->players[1].baseCaptures = 0;
    s->players[1].pillCaptures = 1; s->players[1].dmgDealt = 42u;
    s->players[1].builds = 0;
    s->players[1].lgmKills = 0; s->players[1].lgmDeaths = 2;
    /* Max-ish values to catch byte-order/width bugs. */
    s->players[2].slot = 9;  s->players[2].isBot = 0;  s->players[2].kills = 300;
    s->players[2].deaths = 301; s->players[2].baseCaptures = 65535u;
    s->players[2].pillCaptures = 1000; s->players[2].dmgDealt = 4000000000u;
    s->players[2].builds = 65535u;
    s->players[2].lgmKills = 65535u; s->players[2].lgmDeaths = 54321u;

    s->awardCount = 3;
    s->awards[0].awardId = AWARD_MOST_KILLS; s->awards[0].winnerSlot = 0;
    s->awards[0].subjectSlot = NEUTRAL; s->awards[0].winnerIsBot = 0;
    s->awards[0].value = 12;
    s->awards[1].awardId = AWARD_NEMESIS; s->awards[1].winnerSlot = 9;
    s->awards[1].subjectSlot = 4;  /* non-NEUTRAL subject */
    s->awards[1].winnerIsBot = 0; s->awards[1].value = 7;
    s->awards[2].awardId = AWARD_FISH_FOOD; s->awards[2].winnerSlot = 4;
    s->awards[2].subjectSlot = NEUTRAL; s->awards[2].winnerIsBot = 1;
    s->awards[2].value = 9;

    strncpy(s->wbnLogKey, "abc123DEF456", ROUND_STATS_LOGKEY_LEN - 1);

    UT_ASSERT_MSG(codec_roundtrip_round_stats(&in, &out) == 0,
                  "round-stats codec round-trip failed");
    UT_ASSERT(out.type == CTRL_ROUND_STATS);

    const RoundStatsSummary *d = &out.u.roundStats;
    UT_ASSERT_MSG(d->playerCount == 3, "playerCount, got %u", d->playerCount);
    for (int i = 0; i < 3; i++) {
        const RoundPlayerSummary *a = &s->players[i];
        const RoundPlayerSummary *b = &d->players[i];
        UT_ASSERT_MSG(b->slot == a->slot, "player %d slot", i);
        UT_ASSERT_MSG(b->isBot == a->isBot, "player %d isBot", i);
        UT_ASSERT_MSG(b->kills == a->kills, "player %d kills", i);
        UT_ASSERT_MSG(b->deaths == a->deaths, "player %d deaths", i);
        UT_ASSERT_MSG(b->baseCaptures == a->baseCaptures, "player %d baseCaptures", i);
        UT_ASSERT_MSG(b->pillCaptures == a->pillCaptures, "player %d pillCaptures", i);
        UT_ASSERT_MSG(b->dmgDealt == a->dmgDealt,
                      "player %d dmgDealt, got %u", i, b->dmgDealt);
        UT_ASSERT_MSG(b->builds == a->builds, "player %d builds", i);
        UT_ASSERT_MSG(b->lgmKills == a->lgmKills, "player %d lgmKills", i);
        UT_ASSERT_MSG(b->lgmDeaths == a->lgmDeaths, "player %d lgmDeaths", i);
    }

    UT_ASSERT_MSG(d->awardCount == 3, "awardCount, got %u", d->awardCount);
    for (int i = 0; i < 3; i++) {
        const AwardResult *a = &s->awards[i];
        const AwardResult *b = &d->awards[i];
        UT_ASSERT_MSG(b->awardId == a->awardId, "award %d id", i);
        UT_ASSERT_MSG(b->winnerSlot == a->winnerSlot, "award %d winnerSlot", i);
        UT_ASSERT_MSG(b->subjectSlot == a->subjectSlot, "award %d subjectSlot", i);
        UT_ASSERT_MSG(b->winnerIsBot == a->winnerIsBot, "award %d winnerIsBot", i);
        UT_ASSERT_MSG(b->value == a->value, "award %d value, got %u", i, b->value);
    }

    UT_ASSERT_MSG(strcmp(d->wbnLogKey, "abc123DEF456") == 0,
                  "wbnLogKey round-trips, got '%s'", d->wbnLogKey);

    return 0;
}

/* The worst case — every slot present, every award won, a full-length key —
 * encodes successfully and stays within one control packet. */
int run_round_stats_codec_worstcase(void) {
    ControlEvent in;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_ROUND_STATS;
    RoundStatsSummary *s = &in.u.roundStats;

    s->playerCount = MAX_TANKS;
    for (int i = 0; i < MAX_TANKS; i++) {
        RoundPlayerSummary *p = &s->players[i];
        p->slot = (uint8_t)i; p->isBot = 0;
        p->kills = (uint16_t)i; p->deaths = (uint16_t)i;
        p->baseCaptures = (uint16_t)i; p->pillCaptures = (uint16_t)i;
        p->dmgDealt = 0xFFFFFFFFu; p->builds = (uint16_t)i;
    }
    s->awardCount = AWARD_COUNT;
    for (int i = 0; i < AWARD_COUNT; i++) {
        AwardResult *a = &s->awards[i];
        a->awardId = (uint8_t)(i + 1); a->winnerSlot = (uint8_t)i;
        a->subjectSlot = NEUTRAL; a->winnerIsBot = 0; a->value = 0xFFFFFFFFu;
    }
    memset(s->wbnLogKey, 'K', ROUND_STATS_LOGKEY_LEN - 1);
    s->wbnLogKey[ROUND_STATS_LOGKEY_LEN - 1] = '\0';

    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;
    ControlEncodeFn enc = transportControlCodecEncoder(CTRL_ROUND_STATS);
    UT_ASSERT(enc != NULL);
    EncodeResult r = enc(&in, NULL, buf, sizeof(buf), &outLen);
    UT_ASSERT_MSG(r == ENCODE_OK, "worst case must encode, got %d", (int)r);
    UT_ASSERT_MSG(outLen <= MAX_CONTROL_PACKET,
                  "worst case fits one packet, got %zu", outLen);

    return 0;
}

/* The server builds the curated summary from the accumulator: one row per
 * connected slot, the dmgDealt total and builds sum derived, and awards
 * filled via computeAwards. */
int run_round_stats_build_summary(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 1, "P1", false);

    sim->roundStats[0].kills        = 5;
    sim->roundStats[0].deaths       = 1;
    sim->roundStats[0].baseCaptures = 2;
    sim->roundStats[0].dmgToPlayers = 100;
    sim->roundStats[0].dmgToPills   = 10;
    sim->roundStats[0].pillsBuilt   = 3;
    sim->roundStats[0].treesFarmed  = 4;
    sim->roundStats[0].lgmKills     = 6;
    sim->roundStats[0].lgmDeaths    = 2;
    sim->roundStats[1].kills        = 1;

    RoundStatsSummary summary;
    serverSimBuildRoundStatsSummary(sim, &summary);

    UT_ASSERT_MSG(summary.playerCount == 2, "playerCount, got %u", summary.playerCount);
    UT_ASSERT_MSG(summary.players[0].slot == 0, "row 0 slot, got %u", summary.players[0].slot);
    UT_ASSERT_MSG(summary.players[0].dmgDealt == 110,
                  "dmgDealt = players+pills+bases, got %u", summary.players[0].dmgDealt);
    UT_ASSERT_MSG(summary.players[0].builds == 7,
                  "builds = pillsBuilt+treesFarmed, got %u", summary.players[0].builds);
    UT_ASSERT_MSG(summary.players[0].lgmKills == 6,
                  "lgmKills carried, got %u", summary.players[0].lgmKills);
    UT_ASSERT_MSG(summary.players[0].lgmDeaths == 2,
                  "lgmDeaths carried, got %u", summary.players[0].lgmDeaths);
    UT_ASSERT_MSG(summary.players[1].slot == 1, "row 1 slot, got %u", summary.players[1].slot);

    const AwardResult *mk = find_award(summary.awards, summary.awardCount,
                                       AWARD_MOST_KILLS);
    UT_ASSERT_MSG(mk != NULL, "Most Kills award present");
    UT_ASSERT_MSG(mk->winnerSlot == 0, "Most Kills winner, got %u", mk->winnerSlot);
    UT_ASSERT_MSG(mk->value == 5, "Most Kills value, got %u", mk->value);

    serverSimDestroy(sim);
    return 0;
}

/* The client stores a received summary, exposes it via the accessor, and
 * clears it when the next countdown starts. */
int run_round_stats_client_ingest(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);

    UT_ASSERT_MSG(clientSimGetLastRoundStats(cs) == NULL,
                  "no stats before any are received");

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ROUND_STATS;
    evt.u.roundStats.playerCount = 1;
    evt.u.roundStats.players[0].slot = 3;
    evt.u.roundStats.players[0].kills = 9;
    evt.u.roundStats.awardCount = 1;
    evt.u.roundStats.awards[0].awardId = AWARD_MOST_KILLS;
    evt.u.roundStats.awards[0].winnerSlot = 3;
    evt.u.roundStats.awards[0].value = 9;

    clientSimApplyControl(cs, &evt);

    const RoundStatsSummary *got = clientSimGetLastRoundStats(cs);
    UT_ASSERT_MSG(got != NULL, "stats stored after ingest");
    UT_ASSERT_MSG(got->playerCount == 1, "playerCount, got %u", got->playerCount);
    UT_ASSERT_MSG(got->players[0].kills == 9, "kills field, got %u", got->players[0].kills);

    /* The next countdown clears the previous round's panel (round-only scope). */
    ControlEvent cd;
    memset(&cd, 0, sizeof(cd));
    cd.type = CTRL_GAME_PHASE_COUNTDOWN;
    clientSimApplyControl(cs, &cd);

    UT_ASSERT_MSG(clientSimGetLastRoundStats(cs) == NULL,
                  "countdown clears the stored stats");

    clientSimDestroy(cs);
    return 0;
}

/* The attribution hooks append a packed record to the per-round track buffer
 * in append order. Drive one damage, one action, and one kill event, then
 * parse the raw buffer back and check the tags, tick, and payload of each. */
int run_round_stats_track_records(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    void *ctx = sim->sim.callbacks.ctx;

    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_TANK, 3, DMG_SRC_SHELL, 10, false, 8, 9);
    sim->sim.callbacks.recordPlayerAction(ctx, 0, PLAYER_ACTION_FARM, 6, 7);

    /* Kill event: data[5]/data[6] carry the dying tank's map cell (stashed by
     * serverSimCbTankKill; the funnel copies them to the record's mapX/mapY). */
    const uint8_t kd[8] = { 2, 5, LAST_DEATH_BY_SHELL, 4, 7, 11, 22, 0 };
    inject(sim, EVENT_TANK_KILLED, kd);

    /* Capture event: data = [newOwner, prevOwner, class, index, mapX, mapY]. */
    const uint8_t cd[8] = { 1, NEUTRAL, CAPTURE_CLASS_NEUTRAL, 3, 33, 44, 0, 0 };
    inject(sim, EVENT_PILL_CAPTURED, cd);

    sim->sim.callbacks.recordPillPickup(ctx, 2, 5, 55, 66);

    size_t expectLen = sizeof(AttrDamageRecord) + sizeof(AttrActionRecord) +
                       sizeof(AttrKillRecord) + sizeof(AttrCaptureRecord) +
                       sizeof(AttrPickupRecord);
    UT_ASSERT_MSG(sim->trackRecordCount == 5, "record count, got %u",
                  sim->trackRecordCount);
    UT_ASSERT_MSG(sim->trackLen == expectLen, "track length, got %zu",
                  sim->trackLen);
    UT_ASSERT(sim->trackBuf != NULL);

    size_t off = 0;

    AttrDamageRecord dr;
    memcpy(&dr, sim->trackBuf + off, sizeof dr);
    off += sizeof dr;
    UT_ASSERT_MSG(dr.type == ATTR_REC_DAMAGE, "damage tag, got %u", dr.type);
    UT_ASSERT(dr.tick == sim->tick);
    UT_ASSERT(dr.target == DMG_TARGET_TANK);
    UT_ASSERT(dr.source == DMG_SRC_SHELL);
    UT_ASSERT(dr.targetIndex == 3);
    UT_ASSERT(dr.attacker == 0);
    UT_ASSERT_MSG(dr.amount == 10, "damage amount, got %u", dr.amount);
    UT_ASSERT(dr.destroyed == 0);
    UT_ASSERT_MSG(dr.mapX == 8 && dr.mapY == 9, "damage cell, got %u,%u", dr.mapX, dr.mapY);

    AttrActionRecord ar;
    memcpy(&ar, sim->trackBuf + off, sizeof ar);
    off += sizeof ar;
    UT_ASSERT_MSG(ar.type == ATTR_REC_ACTION, "action tag, got %u", ar.type);
    UT_ASSERT(ar.tick == sim->tick);
    UT_ASSERT(ar.player == 0);
    UT_ASSERT(ar.action == PLAYER_ACTION_FARM);
    UT_ASSERT_MSG(ar.mapX == 6 && ar.mapY == 7, "action cell, got %u,%u", ar.mapX, ar.mapY);

    AttrKillRecord kr;
    memcpy(&kr, sim->trackBuf + off, sizeof kr);
    off += sizeof kr;
    UT_ASSERT_MSG(kr.type == ATTR_REC_KILL, "kill tag, got %u", kr.type);
    UT_ASSERT(kr.tick == sim->tick);
    UT_ASSERT(kr.killer == 2);
    UT_ASSERT(kr.killed == 5);
    UT_ASSERT(kr.deathCause == LAST_DEATH_BY_SHELL);
    UT_ASSERT_MSG(kr.carriedPills == 4, "carried pills, got %u", kr.carriedPills);
    UT_ASSERT_MSG(kr.treesWasted == 7, "trees wasted, got %u", kr.treesWasted);
    UT_ASSERT_MSG(kr.mapX == 11 && kr.mapY == 22, "kill cell, got %u,%u", kr.mapX, kr.mapY);

    AttrCaptureRecord cr;
    memcpy(&cr, sim->trackBuf + off, sizeof cr);
    off += sizeof cr;
    UT_ASSERT_MSG(cr.type == ATTR_REC_CAPTURE, "capture tag, got %u", cr.type);
    UT_ASSERT(cr.tick == sim->tick);
    UT_ASSERT(cr.target == ATTR_CAP_TGT_PILL);
    UT_ASSERT_MSG(cr.targetIndex == 3, "capture index, got %u", cr.targetIndex);
    UT_ASSERT(cr.newOwner == 1);
    UT_ASSERT(cr.captureClass == CAPTURE_CLASS_NEUTRAL);
    UT_ASSERT_MSG(cr.mapX == 33 && cr.mapY == 44, "capture cell, got %u,%u", cr.mapX, cr.mapY);

    AttrPickupRecord pr;
    memcpy(&pr, sim->trackBuf + off, sizeof pr);
    UT_ASSERT_MSG(pr.type == ATTR_REC_PICKUP, "pickup tag, got %u", pr.type);
    UT_ASSERT(pr.tick == sim->tick);
    UT_ASSERT(pr.picker == 2);
    UT_ASSERT(pr.pillIndex == 5);
    UT_ASSERT_MSG(pr.mapX == 55 && pr.mapY == 66, "pickup cell, got %u,%u", pr.mapX, pr.mapY);

    serverSimDestroy(sim);
    return 0;
}

/* When the per-round cap is (near) reached, an append that would overflow it
 * latches trackTruncated and is refused before any allocation, leaving the
 * length untouched; further appends stay refused. Poking trackLen up to the
 * cap exercises this without a 64 MB allocation. */
int run_round_stats_track_cap(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    void *ctx = sim->sim.callbacks.ctx;

    sim->trackLen = ATTRIBUTION_TRACK_CAP_BYTES - 2;   /* a 12-byte record won't fit */
    size_t before = sim->trackLen;

    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_TANK, 0, DMG_SRC_SHELL, 10, false, 0, 0);
    UT_ASSERT_MSG(sim->trackTruncated, "overflowing append must truncate");
    UT_ASSERT_MSG(sim->trackLen == before,
                  "refused append must not grow the buffer, got %zu", sim->trackLen);
    UT_ASSERT(sim->trackBuf == NULL);   /* refused before any allocation */

    /* A subsequent append is also refused. */
    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_TANK, 0, DMG_SRC_SHELL, 10, false, 0, 0);
    UT_ASSERT_MSG(sim->trackLen == before, "still refused, got %zu", sim->trackLen);
    UT_ASSERT(sim->trackTruncated);

    serverSimDestroy(sim);
    return 0;
}

/* The shared-derivation guarantee: the live accumulator and the accumulator
 * rebuilt by replaying the serialized attribution track through
 * roundStatsApplyRecord are byte-identical. Both paths call the same function
 * over the same records, so they can only differ if record *production* drops
 * something — exactly what this pins. Drives a mixed round (damage, builder
 * actions, kills incl. drown/suicide, capture/steal/ally, an LGM loss, and a
 * pickup) with no leaver, so the track is a complete input for the replay. */
int run_round_stats_derive_equivalence(void) {
    ServerSim *sim = make_sim_running();
    UT_ASSERT(sim != NULL);

    void *ctx = sim->sim.callbacks.ctx;

    /* Damage: tank, a pill-destroying blow, a base, and a NEUTRAL splash. */
    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_TANK, 1, DMG_SRC_SHELL, 12, false, 4, 5);
    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_PILL, 2, DMG_SRC_SHELL, 8, true, 6, 7);
    sim->sim.callbacks.recordDamage(ctx, 2, DMG_TARGET_BASE, 0, DMG_SRC_SHELL, 5, false, 1, 2);
    sim->sim.callbacks.recordDamage(ctx, 0xFF, DMG_TARGET_TANK, 3, DMG_SRC_MINE, 9, false, 0, 0);

    /* Builder / shell actions. */
    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_FARM, 3, 3);
    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_BUILD, 3, 4);
    sim->sim.callbacks.recordPlayerAction(ctx, 2, PLAYER_ACTION_MINE, 5, 6);
    sim->sim.callbacks.recordPlayerAction(ctx, 0, PLAYER_ACTION_SHELL, 7, 8);

    /* Kills: a shell kill (0->1) carrying pills/trees, a drown, a suicide. */
    const uint8_t kill[8]    = { 0, 1, LAST_DEATH_BY_SHELL, 2, 3, 9, 9, 0 };
    const uint8_t drown[8]   = { 3, 3, LAST_DEATH_BY_DEEPSEA, 1, 0, 0, 0, 0 };
    const uint8_t suicide[8] = { 4, 4, LAST_DEATH_BY_SHELL, 0, 0, 0, 0, 0 };
    inject(sim, EVENT_TANK_KILLED, kill);
    inject(sim, EVENT_TANK_KILLED, drown);
    inject(sim, EVENT_TANK_KILLED, suicide);

    /* Captures: neutral pill, enemy steal, neutral base. */
    const uint8_t pillCap[8] = { 0, 0xFF, CAPTURE_CLASS_NEUTRAL, 1, 10, 11, 0, 0 };
    const uint8_t steal[8]   = { 2, 5, CAPTURE_CLASS_ENEMY, 2, 12, 13, 0, 0 };
    const uint8_t baseCap[8] = { 2, 0xFF, CAPTURE_CLASS_NEUTRAL, 0, 14, 15, 0, 0 };
    inject(sim, EVENT_PILL_CAPTURED, pillCap);
    inject(sim, EVENT_PILL_CAPTURED, steal);
    inject(sim, EVENT_BASE_CAPTURED, baseCap);

    const uint8_t lgm[8] = { 6, 3, 0, 0, 0, 0, 0, 0 };  /* victim 6, killer 3 */
    inject(sim, EVENT_LGM_LOST, lgm);

    sim->sim.callbacks.recordPillPickup(ctx, 2, 7, 20, 21);

    /* Walk the serialized track record-by-record into a fresh accumulator. */
    size_t len = 0;
    const uint8_t *buf = serverSimGetTrackBuffer(sim, &len, NULL, NULL);
    UT_ASSERT(buf != NULL);
    UT_ASSERT(len > 0);

    PlayerRoundStats replay[MAX_TANKS];
    memset(replay, 0, sizeof(replay));

    size_t off = 0;
    while (off < len) {
        size_t sz = roundStatsRecordSize(buf[off]);
        UT_ASSERT_MSG(sz > 0, "unknown record tag %u at offset %zu",
                      buf[off], off);
        UT_ASSERT_MSG(off + sz <= len, "record at %zu overruns the track", off);
        roundStatsApplyRecord(replay, NULL, NULL, 0, buf + off);
        off += sz;
    }
    UT_ASSERT_MSG(off == len, "walk consumed the whole track, %zu of %zu",
                  off, len);

    /* Every slot's replayed stats must equal the live accumulator, byte-for-byte. */
    for (int slot = 0; slot < MAX_TANKS; slot++) {
        const PlayerRoundStats *live = serverSimGetRoundStats(sim, (BYTE)slot);
        UT_ASSERT(live != NULL);
        UT_ASSERT_MSG(memcmp(live, &replay[slot], sizeof(PlayerRoundStats)) == 0,
                      "slot %d: live and track-replayed stats differ", slot);
    }

    serverSimDestroy(sim);
    return 0;
}

/* Build one synthetic timeline entry, so the highlight tests read as data. */
static NotableEvent mkEvent(uint32_t tick, uint8_t mapX, uint8_t mapY, uint8_t type,
                            uint8_t actorA, uint8_t actorB, uint8_t captureClass,
                            uint8_t deathCause, uint8_t carriedPills) {
    NotableEvent e;
    e.tick = tick;
    e.mapX = mapX;
    e.mapY = mapY;
    e.type = type;
    e.actorA = actorA;
    e.actorB = actorB;
    e.captureClass = captureClass;
    e.deathCause = deathCause;
    e.carriedPills = carriedPills;
    return e;
}

/* A tight cluster of kills close in tick and space collapses to one wipe whose
 * value is the death count; scattering those same kills past the window and
 * radius yields no wipe at all. */
int run_highlights_cluster_wipe(void) {
    NotableEvent tl[3];
    tl[0] = mkEvent(100, 20, 20, NOTABLE_KILL, 0, 3, 0, LAST_DEATH_BY_SHELL, 0);
    tl[1] = mkEvent(110, 21, 20, NOTABLE_KILL, 0, 4, 0, LAST_DEATH_BY_SHELL, 0);
    tl[2] = mkEvent(120, 20, 21, NOTABLE_KILL, 1, 5, 0, LAST_DEATH_BY_SHELL, 0);

    HighlightWindow out[HIGHLIGHTS_MAX];
    int n = -1;
    computeHighlights(tl, 3, NULL, NULL, NULL, 0, NULL, 0, out, &n, HIGHLIGHTS_MAX);

    UT_ASSERT_MSG(n == 1, "one wipe window, got %d", n);
    UT_ASSERT_MSG(out[0].type == HL_CLUSTER_WIPE, "type wipe, got %u", out[0].type);
    UT_ASSERT_MSG(out[0].value == 3, "wipe counts three deaths, got %u", out[0].value);
    /* killer 0 kills twice, killer 1 once -> most frequent killer is slot 0 */
    UT_ASSERT_MSG(out[0].actorA == 0, "most frequent killer, got %u", out[0].actorA);

    NotableEvent spread[3];
    spread[0] = mkEvent(100, 20, 20, NOTABLE_KILL, 0, 3, 0, LAST_DEATH_BY_SHELL, 0);
    spread[1] = mkEvent(400, 40, 40, NOTABLE_KILL, 0, 4, 0, LAST_DEATH_BY_SHELL, 0);
    spread[2] = mkEvent(800, 60, 60, NOTABLE_KILL, 1, 5, 0, LAST_DEATH_BY_SHELL, 0);
    n = -1;
    computeHighlights(spread, 3, NULL, NULL, NULL, 0, NULL, 0, out, &n,
                      HIGHLIGHTS_MAX);
    UT_ASSERT_MSG(n == 0, "scattered deaths form no wipe, got %d", n);

    return 0;
}

/* A wipe whose dead all share a team scores strictly higher than an otherwise
 * identical wipe with a mixed-team casualty list. */
int run_highlights_wipe_team_bonus(void) {
    NotableEvent tl[3];
    tl[0] = mkEvent(100, 20, 20, NOTABLE_KILL, 0, 3, 0, LAST_DEATH_BY_SHELL, 0);
    tl[1] = mkEvent(110, 20, 20, NOTABLE_KILL, 0, 4, 0, LAST_DEATH_BY_SHELL, 0);
    tl[2] = mkEvent(120, 20, 20, NOTABLE_KILL, 0, 5, 0, LAST_DEATH_BY_SHELL, 0);

    uint8_t teamSame[MAX_TANKS], teamMixed[MAX_TANKS];
    memset(teamSame, 0, sizeof(teamSame));
    memset(teamMixed, 0, sizeof(teamMixed));
    teamSame[3] = teamSame[4] = teamSame[5] = 1;      /* all dead on one team  */
    teamMixed[3] = teamMixed[4] = 1; teamMixed[5] = 2; /* one on another team  */

    HighlightWindow shared[HIGHLIGHTS_MAX], mixed[HIGHLIGHTS_MAX];
    int ns = -1, nm = -1;
    computeHighlights(tl, 3, NULL, teamSame, NULL, 0, NULL, 0, shared, &ns,
                      HIGHLIGHTS_MAX);
    computeHighlights(tl, 3, NULL, teamMixed, NULL, 0, NULL, 0, mixed, &nm,
                      HIGHLIGHTS_MAX);

    UT_ASSERT_MSG(ns == 1 && nm == 1, "one wipe each, got %d and %d", ns, nm);
    UT_ASSERT_MSG(shared[0].value == 3 && mixed[0].value == 3,
                  "both count three deaths");
    UT_ASSERT_MSG(shared[0].score > mixed[0].score,
                  "team-shared wipe scores higher: %u vs %u",
                  shared[0].score, mixed[0].score);

    return 0;
}

/* An enemy-owned capture is a steal; a neutral land-grab is not; and a steal
 * closer to the final tick scores higher than an earlier identical one. */
int run_highlights_objective_steal(void) {
    NotableEvent tl[3];
    tl[0] = mkEvent(100, 10, 10, NOTABLE_PILL_CAPTURE, 2, NEUTRAL, ATTR_CAP_NEUTRAL, 0, 0);
    tl[1] = mkEvent(300, 12, 12, NOTABLE_PILL_CAPTURE, 2, 5, ATTR_CAP_ENEMY, 0, 0);
    tl[2] = mkEvent(900, 14, 14, NOTABLE_BASE_CAPTURE, 3, 6, ATTR_CAP_ENEMY, 0, 0);

    HighlightWindow out[HIGHLIGHTS_MAX];
    int n = -1;
    computeHighlights(tl, 3, NULL, NULL, NULL, 0, NULL, 0, out, &n, HIGHLIGHTS_MAX);

    UT_ASSERT_MSG(n == 2, "two steals, neutral capture excluded, got %d", n);
    for (int i = 0; i < n; i++)
        UT_ASSERT_MSG(out[i].type == HL_OBJECTIVE_STEAL, "type steal, got %u", out[i].type);
    UT_ASSERT_MSG(out[0].startTick == 300 && out[1].startTick == 900,
                  "chronological steals, got %u then %u",
                  out[0].startTick, out[1].startTick);
    UT_ASSERT_MSG(out[0].actorA == 2 && out[0].actorB == 5, "steal names new/prev owner");
    UT_ASSERT_MSG(out[0].value == 1 && out[1].value == 1, "steal value is one");
    UT_ASSERT_MSG(out[1].score > out[0].score,
                  "later steal scores higher: %u vs %u", out[1].score, out[0].score);

    return 0;
}

/* A Nemesis award anchors on the winner's last kill of the victim; a builder
 * award (Sapper) has no timeline moment and produces no window. */
int run_highlights_award_anchor(void) {
    NotableEvent tl[3];
    tl[0] = mkEvent(100, 5, 5, NOTABLE_KILL, 0, 1, 0, LAST_DEATH_BY_SHELL, 0);
    tl[1] = mkEvent(200, 7, 7, NOTABLE_KILL, 0, 2, 0, LAST_DEATH_BY_SHELL, 0);
    tl[2] = mkEvent(300, 9, 9, NOTABLE_KILL, 0, 1, 0, LAST_DEATH_BY_SHELL, 0);

    AwardResult aw[2];
    aw[0].awardId = AWARD_NEMESIS; aw[0].winnerSlot = 0; aw[0].subjectSlot = 1;
    aw[0].winnerIsBot = 0; aw[0].value = 2;
    aw[1].awardId = AWARD_SAPPER;  aw[1].winnerSlot = 0; aw[1].subjectSlot = NEUTRAL;
    aw[1].winnerIsBot = 0; aw[1].value = 8;

    HighlightWindow out[HIGHLIGHTS_MAX];
    int n = -1;
    computeHighlights(tl, 3, NULL, NULL, aw, 2, NULL, 0, out, &n, HIGHLIGHTS_MAX);

    UT_ASSERT_MSG(n == 1, "only the nemesis award anchors, got %d", n);
    UT_ASSERT_MSG(out[0].type == HL_AWARD, "type award, got %u", out[0].type);
    UT_ASSERT_MSG(out[0].awardId == AWARD_NEMESIS, "award id, got %u", out[0].awardId);
    UT_ASSERT_MSG(out[0].actorA == 0 && out[0].actorB == 1, "winner then victim");
    /* anchored on the LAST winner->victim kill (tick 300, cell 9,9) */
    UT_ASSERT_MSG(out[0].startTick == 300, "anchor at the last kill, got %u",
                  out[0].startTick);
    UT_ASSERT_MSG(out[0].mapX == 9 && out[0].mapY == 9, "anchor cell, got %u,%u",
                  out[0].mapX, out[0].mapY);

    return 0;
}

/* Overlapping candidates collapse to a non-overlapping set; maxOut caps the
 * reel; output is chronological and deterministic across runs. */
int run_highlights_selection(void) {
    NotableEvent tl[5];
    tl[0] = mkEvent(100, 20, 20, NOTABLE_KILL, 1, 3, 0, LAST_DEATH_BY_SHELL, 0);
    tl[1] = mkEvent(150, 20, 20, NOTABLE_KILL, 0, 3, 0, LAST_DEATH_BY_SHELL, 0);
    tl[2] = mkEvent(200, 20, 20, NOTABLE_KILL, 0, 4, 0, LAST_DEATH_BY_SHELL, 0);
    tl[3] = mkEvent(900, 40, 40, NOTABLE_BASE_CAPTURE, 5, 6, ATTR_CAP_ENEMY, 0, 0);
    tl[4] = mkEvent(950, 41, 41, NOTABLE_KILL, 2, 7, 0, LAST_DEATH_BY_SHELL, 0);

    AwardResult aw[1];
    aw[0].awardId = AWARD_NEMESIS; aw[0].winnerSlot = 0; aw[0].subjectSlot = 3;
    aw[0].winnerIsBot = 0; aw[0].value = 1;

    HighlightWindow out[HIGHLIGHTS_MAX];
    int n = -1;
    computeHighlights(tl, 5, NULL, NULL, aw, 1, NULL, 0, out, &n, HIGHLIGHTS_MAX);

    /* The wipe (tick 100-200) wins its span; the nemesis kill at tick 150 falls
     * inside it and is dropped; the far-off steal survives. */
    UT_ASSERT_MSG(n == 2, "wipe + steal accepted, award collapsed, got %d", n);
    UT_ASSERT_MSG(out[0].type == HL_CLUSTER_WIPE, "first is the wipe, got %u", out[0].type);
    UT_ASSERT_MSG(out[1].type == HL_OBJECTIVE_STEAL, "second is the steal, got %u",
                  out[1].type);
    UT_ASSERT_MSG(out[0].startTick + out[0].durationTicks < out[1].startTick,
                  "accepted windows do not overlap");
    UT_ASSERT_MSG(out[0].startTick <= out[1].startTick, "chronological output");

    int capped = -1;
    computeHighlights(tl, 5, NULL, NULL, aw, 1, NULL, 0, out, &capped, 1);
    UT_ASSERT_MSG(capped == 1, "maxOut caps the reel to one, got %d", capped);
    UT_ASSERT_MSG(out[0].type == HL_CLUSTER_WIPE, "kept window is the strongest");

    HighlightWindow a[HIGHLIGHTS_MAX], b[HIGHLIGHTS_MAX];
    int na = -1, nb = -1;
    computeHighlights(tl, 5, NULL, NULL, aw, 1, NULL, 0, a, &na, HIGHLIGHTS_MAX);
    computeHighlights(tl, 5, NULL, NULL, aw, 1, NULL, 0, b, &nb, HIGHLIGHTS_MAX);
    UT_ASSERT_MSG(na == nb && na == 2, "same count across runs, got %d and %d", na, nb);
    UT_ASSERT_MSG(memcmp(a, b, sizeof(HighlightWindow) * na) == 0,
                  "identical output across runs");

    return 0;
}

/* Lead-in extends a window's start back over a contiguous earlier event, but
 * stops at one beyond the gap bound or outside the tile radius. */
int run_highlights_lead_in(void) {
    NotableEvent tl[3];
    tl[0] = mkEvent(400, 11, 11, NOTABLE_PICKUP, 0, NEUTRAL, 0, 0, 0);
    tl[1] = mkEvent(470, 11, 11, NOTABLE_PICKUP, 0, NEUTRAL, 0, 0, 0);
    tl[2] = mkEvent(500, 10, 10, NOTABLE_KILL, 0, 1, 0, LAST_DEATH_BY_SHELL, 0);

    AwardResult aw[1];
    aw[0].awardId = AWARD_NEMESIS; aw[0].winnerSlot = 0; aw[0].subjectSlot = 1;
    aw[0].winnerIsBot = 0; aw[0].value = 1;

    HighlightWindow out[HIGHLIGHTS_MAX];
    int n = -1;
    computeHighlights(tl, 3, NULL, NULL, aw, 1, NULL, 0, out, &n, HIGHLIGHTS_MAX);

    const HighlightWindow *w = NULL;
    for (int i = 0; i < n; i++) if (out[i].type == HL_AWARD) w = &out[i];
    UT_ASSERT_MSG(w != NULL, "nemesis award present");
    /* reaches the tick-470 event (gap 30) but stops before tick 400 (gap 70) */
    UT_ASSERT_MSG(w->startTick == 470, "lead-in extends to tick 470, got %u",
                  w->startTick);

    NotableEvent far[2];
    far[0] = mkEvent(470, 40, 40, NOTABLE_PICKUP, 0, NEUTRAL, 0, 0, 0);
    far[1] = mkEvent(500, 10, 10, NOTABLE_KILL, 0, 1, 0, LAST_DEATH_BY_SHELL, 0);
    n = -1;
    computeHighlights(far, 2, NULL, NULL, aw, 1, NULL, 0, out, &n, HIGHLIGHTS_MAX);
    w = NULL;
    for (int i = 0; i < n; i++) if (out[i].type == HL_AWARD) w = &out[i];
    UT_ASSERT_MSG(w != NULL, "nemesis award present in radius case");
    UT_ASSERT_MSG(w->startTick == 500,
                  "out-of-radius earlier event yields no lead-in, got %u", w->startTick);

    return 0;
}

/* An empty or zero-length timeline produces no highlights. */
int run_highlights_empty(void) {
    HighlightWindow out[HIGHLIGHTS_MAX];
    int n = -1;
    computeHighlights(NULL, 0, NULL, NULL, NULL, 0, NULL, 0, out, &n,
                      HIGHLIGHTS_MAX);
    UT_ASSERT_MSG(n == 0, "null timeline yields no highlights, got %d", n);

    NotableEvent tl[1];
    tl[0] = mkEvent(100, 5, 5, NOTABLE_KILL, 0, 1, 0, LAST_DEATH_BY_SHELL, 0);
    n = -1;
    computeHighlights(tl, 0, NULL, NULL, NULL, 0, NULL, 0, out, &n,
                      HIGHLIGHTS_MAX);
    UT_ASSERT_MSG(n == 0, "zero count yields no highlights, got %d", n);

    return 0;
}

/* Append one capture record to a synthetic stream; returns the new offset. */
static size_t putCapture(uint8_t *buf, size_t off, uint32_t tick, uint8_t target,
                         uint8_t index, uint8_t newOwner, uint8_t prevOwner,
                         uint8_t mapX, uint8_t mapY) {
    AttrCaptureRecord r;
    memset(&r, 0, sizeof r);
    r.type = ATTR_REC_CAPTURE;
    r.tick = tick;
    r.target = target;
    r.targetIndex = index;
    r.newOwner = newOwner;
    r.prevOwner = prevOwner;
    r.captureClass = (prevOwner == NEUTRAL) ? ATTR_CAP_NEUTRAL : ATTR_CAP_ENEMY;
    r.mapX = mapX;
    r.mapY = mapY;
    memcpy(buf + off, &r, sizeof r);
    return off + sizeof r;
}

/* Append one pill-destroying damage record; returns the new offset. */
static size_t putPillKill(uint8_t *buf, size_t off, uint32_t tick, uint8_t index,
                          uint8_t attacker, uint8_t mapX, uint8_t mapY) {
    AttrDamageRecord r;
    memset(&r, 0, sizeof r);
    r.type = ATTR_REC_DAMAGE;
    r.tick = tick;
    r.source = ATTR_SRC_SHELL;
    r.target = ATTR_TGT_PILL;
    r.targetIndex = index;
    r.attacker = attacker;
    r.amount = 15;
    r.destroyed = 1;
    r.mapX = mapX;
    r.mapY = mapY;
    memcpy(buf + off, &r, sizeof r);
    return off + sizeof r;
}

/* A territory shift counts only ground taken off a team that already held it.
 * A first claim of unheld ground (the round populating an empty map) emits
 * nothing; a contested recapture and a held pill shot dead both do. An empty
 * stream yields no shifts at all. */
int run_territory_shift_basic(void) {
    AttrSlotIdentity slots[MAX_TANKS];
    uint8_t buf[256];
    size_t len = 0;
    TerritoryShift shifts[TERRITORY_SHIFTS_MAX];
    int n = -1;

    memset(slots, 0, sizeof slots);
    slots[0].team = 1;
    slots[1].team = 2;
    for (int s = 2; s < MAX_TANKS; s++) slots[s].team = 1;

    /* base 0: first claimed for team 1 off nobody (no shift), then taken by
     * team 2 (a real reclaim). pill 4: first placed for team 1 off nobody (no
     * shift), then shot dead (gives its held ground back to no one). */
    len = putCapture(buf, len, 1000, ATTR_CAP_TGT_BASE, 0, 0, NEUTRAL, 60, 60);
    len = putCapture(buf, len, 2000, ATTR_CAP_TGT_BASE, 0, 1, 0, 60, 60);
    len = putCapture(buf, len, 3000, ATTR_CAP_TGT_PILL, 4, 0, NEUTRAL, 120, 120);
    len = putPillKill(buf, len, 4000, 4, 1, 120, 120);

    computeTerritoryShifts(buf, len, slots, MAX_TANKS, shifts, &n,
                           TERRITORY_SHIFTS_MAX);

    /* Only the two events that took ground off a team are emitted; both
     * first-claims off NEUTRAL are dropped. */
    UT_ASSERT_MSG(n == 2, "only reclaims shift ground, got %d", n);
    for (int i = 0; i < n; i++)
        UT_ASSERT_MSG(shifts[i].tick != 1000 && shifts[i].tick != 3000,
                      "no shift for a first claim off nobody, got tick %u",
                      shifts[i].tick);

    /* The contested base recapture: team 1's ground goes to team 2. */
    UT_ASSERT_MSG(shifts[0].tick == 2000, "recapture shift tick, got %u",
                  shifts[0].tick);
    UT_ASSERT_MSG(shifts[0].gainTeam == 2 && shifts[0].loseTeam == 1,
                  "base changes hands 1 -> 2, got %u -> %u", shifts[0].loseTeam,
                  shifts[0].gainTeam);
    UT_ASSERT_MSG(shifts[0].cellsFlipped > 0, "reclaim moves ground, got %u",
                  shifts[0].cellsFlipped);
    UT_ASSERT_MSG(shifts[0].mapX == 60 && shifts[0].mapY == 60,
                  "shift cell, got %u,%u", shifts[0].mapX, shifts[0].mapY);

    /* The held pill shot dead: team 1's ground falls to no one. */
    UT_ASSERT_MSG(shifts[1].tick == 4000, "dead-pill shift tick, got %u",
                  shifts[1].tick);
    UT_ASSERT_MSG(shifts[1].gainTeam == NEUTRAL && shifts[1].loseTeam == 1,
                  "a dead pill gives its ground back, got %u -> %u",
                  shifts[1].loseTeam, shifts[1].gainTeam);
    UT_ASSERT_MSG(shifts[1].cellsFlipped > 0, "the pill gives up held ground, got %u",
                  shifts[1].cellsFlipped);

    n = -1;
    computeTerritoryShifts(buf, 0, slots, MAX_TANKS, shifts, &n,
                           TERRITORY_SHIFTS_MAX);
    UT_ASSERT_MSG(n == 0, "an empty stream yields no shifts, got %d", n);

    return 0;
}

/* The densest one-team swing becomes a single turning-point window, and it is
 * kept even though a higher-scoring wipe covers the same ticks. Drop the shift
 * series and that wipe is what the reel shows instead. */
int run_highlights_turning_point(void) {
    NotableEvent tl[3];
    TerritoryShift shifts[4];
    uint8_t team[MAX_TANKS];
    HighlightWindow out[HIGHLIGHTS_MAX];
    int n = -1;
    int turns = 0, wipes = 0;
    const HighlightWindow *turn = NULL;

    /* A wipe worth 3 * HL_WIPE_WEIGHT — well above the turning point's own
     * ranking score — sitting inside the swing's ticks. */
    tl[0] = mkEvent(1000, 20, 20, NOTABLE_KILL, 0, 3, 0, LAST_DEATH_BY_SHELL, 0);
    tl[1] = mkEvent(1050, 20, 20, NOTABLE_KILL, 0, 4, 0, LAST_DEATH_BY_SHELL, 0);
    tl[2] = mkEvent(1100, 20, 20, NOTABLE_KILL, 0, 5, 0, LAST_DEATH_BY_SHELL, 0);

    memset(team, 0, sizeof team);
    team[3] = team[4] = team[5] = 1;   /* the swing's team, and the wipe's dead */

    shifts[0].tick = 1000; shifts[0].mapX = 30; shifts[0].mapY = 30;
    shifts[0].gainTeam = 1; shifts[0].loseTeam = 2; shifts[0].cellsFlipped = 200;
    shifts[1].tick = 1200; shifts[1].mapX = 31; shifts[1].mapY = 31;
    shifts[1].gainTeam = 1; shifts[1].loseTeam = 2; shifts[1].cellsFlipped = 300;
    shifts[2].tick = 1400; shifts[2].mapX = 32; shifts[2].mapY = 32;
    shifts[2].gainTeam = 1; shifts[2].loseTeam = 2; shifts[2].cellsFlipped = 100;
    /* A lone later shift for the other team: real, but a smaller swing. */
    shifts[3].tick = 4000; shifts[3].mapX = 90; shifts[3].mapY = 90;
    shifts[3].gainTeam = 2; shifts[3].loseTeam = 1; shifts[3].cellsFlipped = 150;

    computeHighlights(tl, 3, NULL, team, NULL, 0, shifts, 4, out, &n,
                      HIGHLIGHTS_MAX);

    for (int i = 0; i < n; i++) {
        if (out[i].type == HL_BREAKTHROUGH) { turns++; turn = &out[i]; }
        if (out[i].type == HL_CLUSTER_WIPE)   wipes++;
    }
    UT_ASSERT_MSG(turns == 1, "exactly one turning point, got %d", turns);
    UT_ASSERT_MSG(wipes == 0,
                  "the overlapping wipe yields to the seeded turning point, got %d",
                  wipes);
    UT_ASSERT_MSG(turn->startTick == 1000, "swing starts at tick 1000, got %u",
                  turn->startTick);
    UT_ASSERT_MSG(turn->startTick + turn->durationTicks == 1400,
                  "swing ends at tick 1400, got %u",
                  turn->startTick + turn->durationTicks);
    UT_ASSERT_MSG(turn->value == 600, "swing sums the window's cells, got %u",
                  turn->value);
    UT_ASSERT_MSG(turn->mapX == 31 && turn->mapY == 31,
                  "swing points at the biggest single flip, got %u,%u",
                  turn->mapX, turn->mapY);
    UT_ASSERT_MSG(turn->actorA == 3, "gaining team's lowest slot, got %u",
                  turn->actorA);

    /* Same timeline with no territory series: the wipe is back and there is no
     * turning point at all. */
    n = -1;
    computeHighlights(tl, 3, NULL, team, NULL, 0, NULL, 0, out, &n,
                      HIGHLIGHTS_MAX);
    UT_ASSERT_MSG(n == 1, "one window without the shifts, got %d", n);
    UT_ASSERT_MSG(out[0].type == HL_CLUSTER_WIPE, "the wipe, got type %u",
                  out[0].type);

    return 0;
}
