/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Live per-slot scoreboard (ClientSim::liveStats), accumulated in
 * clientSimApplyGameEvents from the four reliable game events that carry
 * a scoring outcome: EVENT_TANK_KILLED, EVENT_LGM_LOST,
 * EVENT_BASE_CAPTURED and EVENT_PILL_CAPTURED.
 *
 * The counters exist for every slot, not just the local player's, so the
 * tests drive events that never name the local slot and assert the rows
 * for the slots the events do name. Two behaviours have to match the
 * server's PlayerRoundStats or the live board drifts from the end-of-round
 * recap: a self-kill is a death with no kill, and killing your own LGM
 * credits no lgmKills.
 *
 * The counting is not gated on the human/bot flag — bots run the same
 * apply path. The kill cases therefore run on a human ClientSim and the
 * LGM, capture and reset cases on a bot one; the bot fixture also keeps
 * those three clear of the newswire emit the human path takes for an
 * event naming any slot, which would need a seeded player table.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "input_packet.h"     /* GameEvent, EVENT_* */
#include "test_harness.h"

/* Local slot for every fixture. Test events name slots 1..5 only, so the
 * local-player branches in the four arms (Steam stats, newswire, own-tank
 * kill count) never fire and the assertions see the accumulator alone. */
#define LOCAL_SLOT 0

static ClientSim *fresh_stats_sim(bool asBot) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, LOCAL_SLOT);
    clientSimSetIsBot(cs, asBot);
    return cs;
}

static GameEvent make_event(uint8_t type, uint8_t d0, uint8_t d1) {
    GameEvent e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.data[0] = d0;
    e.data[1] = d1;
    return e;
}

static void apply_one(ClientSim *cs, GameEvent e) {
    clientSimApplyGameEvents(cs, &e, 1, LOCAL_SLOT);
}

/* Every counter on every slot is zero. Used to prove an ignored event
 * touched nothing at all. */
static bool all_slots_zero(ClientSim *cs) {
    static const ClientPlayerStats k_zero = { 0, 0, 0, 0, 0, 0 };
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        const ClientPlayerStats *row = clientSimGetPlayerStats(cs, i);
        if (row == NULL) return false;
        if (memcmp(row, &k_zero, sizeof(k_zero)) != 0) return false;
    }
    return true;
}

/* A normal kill splits across two rows: the killer's kills and the
 * victim's deaths, with no cross-contamination between slots. */
int run_live_stats_kill_credits_killer_and_victim(void) {
    ClientSim *cs = fresh_stats_sim(false);
    UT_ASSERT(cs != NULL);
    UT_ASSERT_MSG(all_slots_zero(cs),
                  "a fresh ClientSim must start with an empty scoreboard");

    apply_one(cs, make_event(EVENT_TANK_KILLED, /*killer=*/1, /*killed=*/2));

    const ClientPlayerStats *killer = clientSimGetPlayerStats(cs, 1);
    const ClientPlayerStats *victim = clientSimGetPlayerStats(cs, 2);
    UT_ASSERT(killer != NULL && victim != NULL);
    UT_ASSERT_MSG(killer->kills == 1, "killer kills=%u, want 1",
                  (unsigned)killer->kills);
    UT_ASSERT_MSG(killer->deaths == 0, "killer must take no death, got %u",
                  (unsigned)killer->deaths);
    UT_ASSERT_MSG(victim->deaths == 1, "victim deaths=%u, want 1",
                  (unsigned)victim->deaths);
    UT_ASSERT_MSG(victim->kills == 0, "victim must take no kill, got %u",
                  (unsigned)victim->kills);

    /* A second slot pair's event leaves the first pair alone. */
    apply_one(cs, make_event(EVENT_TANK_KILLED, /*killer=*/3, /*killed=*/4));
    UT_ASSERT_MSG(killer->kills == 1 && victim->deaths == 1,
                  "an unrelated kill disturbed slots 1/2 (kills=%u deaths=%u)",
                  (unsigned)killer->kills, (unsigned)victim->deaths);
    UT_ASSERT_MSG(clientSimGetPlayerStats(cs, 3)->kills == 1,
                  "slot 3 kills=%u, want 1",
                  (unsigned)clientSimGetPlayerStats(cs, 3)->kills);
    UT_ASSERT_MSG(clientSimGetPlayerStats(cs, 4)->deaths == 1,
                  "slot 4 deaths=%u, want 1",
                  (unsigned)clientSimGetPlayerStats(cs, 4)->deaths);

    clientSimDestroy(cs);
    return 0;
}

/* Killer == killed is a suicide: the death counts, the kill does not.
 * The server's PlayerRoundStats keeps suicides out of the kill column,
 * so a live board that credited one would disagree with the recap. */
int run_live_stats_suicide_counts_death_only(void) {
    ClientSim *cs = fresh_stats_sim(false);
    UT_ASSERT(cs != NULL);

    apply_one(cs, make_event(EVENT_TANK_KILLED, /*killer=*/5, /*killed=*/5));

    const ClientPlayerStats *row = clientSimGetPlayerStats(cs, 5);
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(row->deaths == 1, "suicide deaths=%u, want 1",
                  (unsigned)row->deaths);
    UT_ASSERT_MSG(row->kills == 0, "suicide must credit no kill, got %u",
                  (unsigned)row->kills);

    clientSimDestroy(cs);
    return 0;
}

/* EVENT_LGM_LOST is [victim, killer]: the victim always takes an
 * lgmDeaths; the killer takes an lgmKills only when it is not the victim
 * shooting their own builder. */
int run_live_stats_lgm_loss_splits_victim_and_killer(void) {
    ClientSim *cs = fresh_stats_sim(true);
    UT_ASSERT(cs != NULL);

    apply_one(cs, make_event(EVENT_LGM_LOST, /*victim=*/1, /*killer=*/2));

    const ClientPlayerStats *victim = clientSimGetPlayerStats(cs, 1);
    const ClientPlayerStats *killer = clientSimGetPlayerStats(cs, 2);
    UT_ASSERT(victim != NULL && killer != NULL);
    UT_ASSERT_MSG(victim->lgmDeaths == 1, "victim lgmDeaths=%u, want 1",
                  (unsigned)victim->lgmDeaths);
    UT_ASSERT_MSG(victim->lgmKills == 0, "victim must take no lgmKills, got %u",
                  (unsigned)victim->lgmKills);
    UT_ASSERT_MSG(killer->lgmKills == 1, "killer lgmKills=%u, want 1",
                  (unsigned)killer->lgmKills);
    UT_ASSERT_MSG(killer->lgmDeaths == 0,
                  "killer must take no lgmDeaths, got %u",
                  (unsigned)killer->lgmDeaths);

    /* Self-inflicted: the loss counts, the kill does not. */
    apply_one(cs, make_event(EVENT_LGM_LOST, /*victim=*/1, /*killer=*/1));
    UT_ASSERT_MSG(victim->lgmDeaths == 2, "lgmDeaths=%u after self-kill, want 2",
                  (unsigned)victim->lgmDeaths);
    UT_ASSERT_MSG(victim->lgmKills == 0,
                  "killing your own LGM must credit no lgmKills, got %u",
                  (unsigned)victim->lgmKills);

    clientSimDestroy(cs);
    return 0;
}

/* Captures land on the new owner (data[0]); the previous owner's row is
 * untouched, including when it is NEUTRAL and indexes nothing. */
int run_live_stats_captures_credit_new_owner(void) {
    ClientSim *cs = fresh_stats_sim(true);
    UT_ASSERT(cs != NULL);

    apply_one(cs, make_event(EVENT_BASE_CAPTURED, /*newOwner=*/2, NEUTRAL));
    apply_one(cs, make_event(EVENT_BASE_CAPTURED, /*newOwner=*/2, /*prev=*/3));
    apply_one(cs, make_event(EVENT_PILL_CAPTURED, /*newOwner=*/2, /*prev=*/3));

    const ClientPlayerStats *owner = clientSimGetPlayerStats(cs, 2);
    const ClientPlayerStats *prev  = clientSimGetPlayerStats(cs, 3);
    UT_ASSERT(owner != NULL && prev != NULL);
    UT_ASSERT_MSG(owner->baseCaptures == 2, "baseCaptures=%u, want 2",
                  (unsigned)owner->baseCaptures);
    UT_ASSERT_MSG(owner->pillCaptures == 1, "pillCaptures=%u, want 1",
                  (unsigned)owner->pillCaptures);
    UT_ASSERT_MSG(prev->baseCaptures == 0 && prev->pillCaptures == 0,
                  "the previous owner must not be credited (base=%u pill=%u)",
                  (unsigned)prev->baseCaptures, (unsigned)prev->pillCaptures);

    clientSimDestroy(cs);
    return 0;
}

/* Slot bytes arrive off the wire. Anything outside 0..MAX_TANKS-1 is
 * dropped rather than indexing the array. */
int run_live_stats_out_of_range_slot_ignored(void) {
    ClientSim *cs = fresh_stats_sim(true);
    UT_ASSERT(cs != NULL);

    apply_one(cs, make_event(EVENT_TANK_KILLED, MAX_TANKS, (uint8_t)(MAX_TANKS + 1)));
    apply_one(cs, make_event(EVENT_LGM_LOST, 200, NEUTRAL));
    apply_one(cs, make_event(EVENT_BASE_CAPTURED, NEUTRAL, 4));
    apply_one(cs, make_event(EVENT_PILL_CAPTURED, 255, 4));

    UT_ASSERT_MSG(all_slots_zero(cs),
                  "an out-of-range slot byte must leave every counter at zero");
    UT_ASSERT_MSG(clientSimGetPlayerStats(cs, MAX_TANKS) == NULL,
                  "the accessor must reject an out-of-range slot");

    clientSimDestroy(cs);
    return 0;
}

/* The board is per-game state: CTRL_GAME_PHASE_RUNNING wipes it beside
 * the other per-game counters, so last round's totals never bleed into
 * the new round on a lobby-cycling server. */
int run_live_stats_cleared_on_running_phase(void) {
    ClientSim *cs = fresh_stats_sim(true);
    UT_ASSERT(cs != NULL);

    apply_one(cs, make_event(EVENT_TANK_KILLED, /*killer=*/1, /*killed=*/2));
    apply_one(cs, make_event(EVENT_LGM_LOST, /*victim=*/1, /*killer=*/2));
    apply_one(cs, make_event(EVENT_BASE_CAPTURED, /*newOwner=*/1, NEUTRAL));
    apply_one(cs, make_event(EVENT_PILL_CAPTURED, /*newOwner=*/1, NEUTRAL));
    UT_ASSERT_MSG(all_slots_zero(cs) == false,
                  "setup failed — nothing accumulated before the phase event");

    ControlEvent phase;
    memset(&phase, 0, sizeof(phase));
    phase.type = CTRL_GAME_PHASE_RUNNING;
    clientSimApplyControl(cs, &phase);

    UT_ASSERT_MSG(all_slots_zero(cs),
                  "the running-phase wipe must zero every scoreboard row");

    clientSimDestroy(cs);
    return 0;
}
