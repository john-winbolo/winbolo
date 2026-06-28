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

    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_TANK, 10, false);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToPlayers == 10,
                  "tank damage, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToPlayers);

    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_PILL, 5, false);
    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_PILL, 3, true);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToPills == 8,
                  "pill damage accumulates, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToPills);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->pillKills == 1,
                  "destroyed blow credits a pill kill, got %u",
                  serverSimGetRoundStats(sim, 0)->pillKills);

    sim->sim.callbacks.recordDamage(ctx, 0, DMG_TARGET_BASE, 7, false);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToBases == 7,
                  "base damage, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToBases);

    /* NEUTRAL attacker (0xFF) is out of range and must be ignored. */
    sim->sim.callbacks.recordDamage(ctx, 0xFF, DMG_TARGET_TANK, 99, false);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->dmgToPlayers == 10,
                  "NEUTRAL attacker ignored, got %llu",
                  (unsigned long long)serverSimGetRoundStats(sim, 0)->dmgToPlayers);

    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_FARM);
    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_BUILD);
    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_MINE);
    sim->sim.callbacks.recordPlayerAction(ctx, 1, PLAYER_ACTION_SHELL);

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
