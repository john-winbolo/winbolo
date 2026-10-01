/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * CTRL_SIM_RULES over a real loopback connection: the table a server runs
 * on reaching the clients that have to agree with it.
 *
 *   run_loopback_sim_rules_change  — a rule changed mid-round reaches a
 *       connected client's own table, and a change to a rule only the
 *       server reads publishes nothing at all
 *   run_loopback_sim_rules_join    — a client that joins after the change
 *       is given the changed value, not the classic one
 *   run_loopback_sim_rules_reclamp — a table that caps lower than the
 *       records a client is already holding clamps them as it lands
 *   run_loopback_sim_rules_agree   — a cap lowered by the op mid-round, and
 *       both sides reading the same value afterwards
 *
 * The publish count comes from an in-process subscriber on the server's own
 * bus, so "published nothing" is an observation rather than an inference
 * from the client not changing — a client that never changes proves only
 * that nothing arrived, which is also what a lost packet looks like.
 *
 * The re-clamp case applies the event to the client directly rather than
 * pumping it across: the records it caps are snapshot state, so a pumped
 * event would race the next snapshot restating them and the case would pass
 * or fail on timing. Applying it in place asks the one question the case is
 * about — what the apply does to the records already there.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"          /* clientSimGetConnectState */
#include "client_connect_state.h"
#include "client_sim_control.h"  /* clientSimApplyControl */
#include "control_event.h"
#include "game_sim.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_scenario.h"
#include "scenario_defs.h"
#include "pillbox.h"
#include "bases.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX 2000   /* join + map download                      */
#define SETTLE_MAX   200   /* a few running ticks after the download    */
#define CHANGE_MAX   600   /* the event crossing to the client          */

/* A carried rule and a value nothing else in the table objects to: the row
 * is 0..255 and no pair mentions it. */
#define SR_RELOAD_SET 7

/* A carried rule with a pair behind it, so the case also shows the write
 * going through the whole check rather than past it. gunsight_min is 2 and
 * shell_life x gunsight_max / 2 - shell_start_add + 1 stays well under 255. */
#define SR_GUNSIGHT_SET 20

/* A rule no client reads: the builder's road cost, well under the tank's
 * tree capacity so the pair holds. */
#define SR_LGM_ROAD_SET 3

/* Caps below anything the map's own records carry. Case 3 writes these into
 * an event by hand, so neither has to pass the table's own check. */
#define SR_BASE_SHELLS_CAP 5
#define SR_PILL_ARMOUR_CAP 1

/* The same two caps for a case that sets them through the op, which does
 * check: a pill cap has to stay at or above pill_repair_amount, which is 4,
 * and a shells cap above base_min_shells plus base_shells_give, which is 1.
 * Both are still well under what the map's records carry. */
#define SR_OP_PILL_ARMOUR_CAP 8
#define SR_OP_BASE_SHELLS_CAP 40

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_connected2(LoopbackHarness *h, void *user) {
    (void)user;
    return h->cs2 != NULL &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_client_reload(LoopbackHarness *h, void *user) {
    GameSim *gs = clientSimGetGameSim(h->cs);
    (void)user;
    return gs != NULL && gs->rules.tank_reload_ticks == SR_RELOAD_SET;
}

static bool pred_client_gunsight(LoopbackHarness *h, void *user) {
    GameSim *gs = clientSimGetGameSim(h->cs);
    (void)user;
    return gs != NULL && gs->rules.gunsight_max == SR_GUNSIGHT_SET;
}

static bool pred_client_mac_collision(LoopbackHarness *h, void *user) {
    GameSim *gs = clientSimGetGameSim(h->cs);
    return gs != NULL && gs->rules.tank_collision_mac == *(int *)user;
}

/* Counts CTRL_SIM_RULES publishes on the server's bus. */
typedef struct {
    int count;
} SrPublishCount;

static void srCountCb(void *ctx, const ControlEvent *evt) {
    SrPublishCount *c = (SrPublishCount *)ctx;
    if (evt->type == CTRL_SIM_RULES) c->count++;
}

static bool pred_client_caps(LoopbackHarness *h, void *user) {
    GameSim *gs = clientSimGetGameSim(h->cs);
    (void)user;
    return gs != NULL &&
           gs->rules.pill_max_armour == SR_OP_PILL_ARMOUR_CAP &&
           gs->rules.base_full_shells == SR_OP_BASE_SHELLS_CAP;
}

static ScnOpResult srSetRule(ServerSim *sim, uint16_t rule, double value) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_SET_RULE;
    op.u.setRule.rule  = rule;
    op.u.setRule.value = value;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* ================================================================
 * 1. A change reaches a connected client; a server-only change does not
 *    leave the server.
 * ================================================================ */
int run_loopback_sim_rules_change(void) {
    LoopbackHarness h;
    SrPublishCount  pub;
    GameSim        *cl;
    int             at;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "RuleWatcher", false, NULL, 90410),
                  "could not start the loopback harness");

    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    UT_ASSERT_MSG(at > 0, "the client never finished connecting");
    loopbackHarnessPumpUntil(&h, SETTLE_MAX, NULL, NULL);

    cl = clientSimGetGameSim(h.cs);
    UT_ASSERT_MSG(cl != NULL, "the client has no GameSim");
    UT_ASSERT_MSG(cl->rules.tank_reload_ticks != SR_RELOAD_SET,
                  "setup: the client already reads %d for tank_reload_ticks, "
                  "so the change below would prove nothing", SR_RELOAD_SET);

    /* Count publishes from here on. Registration replays current state to
       the new subscriber, so the count is cleared afterwards. */
    memset(&pub, 0, sizeof(pub));
    (void)serverSimRegisterSubscriber(h.sim, srCountCb, &pub);
    pub.count = 0;

    UT_ASSERT_MSG(srSetRule(h.sim, SCN_RULE_tank_reload_ticks,
                            (double)SR_RELOAD_SET) == SCN_OP_OK,
                  "the server refused a value inside tank_reload_ticks' row");
    UT_ASSERT_MSG(pub.count == 1,
                  "a change to a carried rule published %d event(s), expected 1",
                  pub.count);

    at = loopbackHarnessPumpUntil(&h, CHANGE_MAX, pred_client_reload, NULL);
    if (at < 0) {
        int got = (int)cl->rules.tank_reload_ticks;
        loopbackHarnessStop(&h);
        UT_ASSERT_MSG(at > 0,
                      "the client still reads %d for tank_reload_ticks after "
                      "%d pumps, expected %d — the change never reached its "
                      "table", got, CHANGE_MAX, SR_RELOAD_SET);
    }

    /* A rule no client reads: the server takes it and tells nobody. */
    UT_ASSERT_MSG(srSetRule(h.sim, SCN_RULE_lgm_cost_road,
                            (double)SR_LGM_ROAD_SET) == SCN_OP_OK,
                  "the server refused a value inside lgm_cost_road's row");
    UT_ASSERT_MSG(h.sim->sim.rules.lgm_cost_road == SR_LGM_ROAD_SET,
                  "setup: the server holds %ld for lgm_cost_road, expected %d "
                  "— the write itself did not happen",
                  (long)h.sim->sim.rules.lgm_cost_road, SR_LGM_ROAD_SET);
    UT_ASSERT_MSG(pub.count == 1,
                  "a change to a server-only rule published %d event(s) in "
                  "total, expected the 1 from the carried rule above",
                  pub.count);

    /* The optional wire tail must apply in both directions: turning the
     * mod off emits the old body, which must clear the client's flag. */
    {
        int enabled;
        for (enabled = 1; enabled >= 0; enabled--) {
            UT_ASSERT(srSetRule(h.sim, SCN_RULE_tank_collision_mac, enabled) == SCN_OP_OK);
            at = loopbackHarnessPumpUntil(&h, CHANGE_MAX, pred_client_mac_collision, &enabled);
            UT_ASSERT_MSG(at > 0, "Mac collision setting %d never reached the client", enabled);
        }
    }
    loopbackHarnessStop(&h);
    return 0;
}

/* ================================================================
 * 2. A client that joins after the change gets the changed value.
 * ================================================================ */
int run_loopback_sim_rules_join(void) {
    LoopbackHarness h;
    GameSim        *cl2;
    int             at;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "RuleFirst", false, NULL, 90411),
                  "could not start the loopback harness");

    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    UT_ASSERT_MSG(at > 0, "the first client never finished connecting");
    loopbackHarnessPumpUntil(&h, SETTLE_MAX, NULL, NULL);

    UT_ASSERT_MSG(srSetRule(h.sim, SCN_RULE_gunsight_max,
                            (double)SR_GUNSIGHT_SET) == SCN_OP_OK,
                  "the server refused a value inside gunsight_max' row");
    UT_ASSERT(srSetRule(h.sim, SCN_RULE_tank_collision_mac, 1) == SCN_OP_OK);
    at = loopbackHarnessPumpUntil(&h, CHANGE_MAX, pred_client_gunsight, NULL);
    UT_ASSERT_MSG(at > 0,
                  "the first client never took the change, so the joiner "
                  "below would be joining a server nothing had changed on");

    /* Now somebody arrives. Their table starts classic and the sync replay
       is the only thing that can correct it. */
    UT_ASSERT_MSG(loopbackHarnessAddClient(&h, "RuleJoiner"),
                  "could not join a second client");
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected2, NULL);
    UT_ASSERT_MSG(at > 0, "the second client never finished connecting");
    loopbackHarnessPumpUntil(&h, SETTLE_MAX, NULL, NULL);

    cl2 = clientSimGetGameSim(h.cs2);
    UT_ASSERT_MSG(cl2 != NULL, "the second client has no GameSim");
    UT_ASSERT_MSG(cl2->rules.tank_collision_mac == 1,
                  "joining client did not receive Mac collision geometry");
    if (cl2->rules.gunsight_max != SR_GUNSIGHT_SET) {
        int got = (int)cl2->rules.gunsight_max;
        loopbackHarnessStop(&h);
        UT_ASSERT_MSG(got == SR_GUNSIGHT_SET,
                      "the joiner reads %d for gunsight_max, expected %d — it "
                      "joined on the classic table", got, SR_GUNSIGHT_SET);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* ================================================================
 * 3. A new table clamps the records the client is already holding.
 * ================================================================ */
int run_loopback_sim_rules_reclamp(void) {
    LoopbackHarness h;
    ControlEvent    evt;
    GameSim        *cl;
    BYTE            nBases, nPills, i;
    bool            sawBaseAbove = false;
    bool            sawPillAbove = false;
    int             at;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "RuleClamper", false, NULL, 90412),
                  "could not start the loopback harness");

    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    UT_ASSERT_MSG(at > 0, "the client never finished connecting");
    loopbackHarnessPumpUntil(&h, SETTLE_MAX, NULL, NULL);

    cl = clientSimGetGameSim(h.cs);
    UT_ASSERT_MSG(cl != NULL && cl->bs != NULL && cl->pb != NULL,
                  "the client has no world to clamp");

    /* The map has to be carrying records above the caps below, or the case
       would pass on a client that ignored the event entirely. */
    nBases = basesGetNumBases(&cl->bs);
    nPills = pillsGetNumPills(&cl->pb);
    for (i = 0; i < nBases; i++) {
        if ((*cl->bs).item[i].shells > SR_BASE_SHELLS_CAP) sawBaseAbove = true;
    }
    for (i = 0; i < nPills; i++) {
        if ((*cl->pb).item[i].armour > SR_PILL_ARMOUR_CAP) sawPillAbove = true;
    }
    if (!sawBaseAbove || !sawPillAbove) {
        loopbackHarnessStop(&h);
        UT_ASSERT_MSG(sawBaseAbove && sawPillAbove,
                      "setup: the client holds no base above %d shells or no "
                      "pillbox above %d armour, so the clamp would have "
                      "nothing to do", SR_BASE_SHELLS_CAP, SR_PILL_ARMOUR_CAP);
    }

    /* The server's own table with two caps pulled under what the client is
       holding, applied the way the wire would apply it. */
    serverSimFillSimRulesEvent(h.sim, &evt);
    evt.u.simRules.base_full_shells = SR_BASE_SHELLS_CAP;
    evt.u.simRules.pill_max_armour  = SR_PILL_ARMOUR_CAP;
    clientSimApplyControl(h.cs, &evt);

    UT_ASSERT_MSG(cl->rules.base_full_shells == SR_BASE_SHELLS_CAP,
                  "the client's table reads %ld for base_full_shells, "
                  "expected %d", (long)cl->rules.base_full_shells,
                  SR_BASE_SHELLS_CAP);

    for (i = 0; i < nBases; i++) {
        BYTE got = (*cl->bs).item[i].shells;
        if (got > SR_BASE_SHELLS_CAP) {
            loopbackHarnessStop(&h);
            UT_ASSERT_MSG(got <= SR_BASE_SHELLS_CAP,
                          "base %u still holds %u shells against a cap of %d "
                          "— the new table was written without re-clamping "
                          "what the client already had",
                          (unsigned)i, (unsigned)got, SR_BASE_SHELLS_CAP);
        }
    }
    for (i = 0; i < nPills; i++) {
        BYTE got = (*cl->pb).item[i].armour;
        if (got > SR_PILL_ARMOUR_CAP) {
            loopbackHarnessStop(&h);
            UT_ASSERT_MSG(got <= SR_PILL_ARMOUR_CAP,
                          "pillbox %u still holds %u armour against a cap of "
                          "%d", (unsigned)i, (unsigned)got,
                          SR_PILL_ARMOUR_CAP);
        }
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* ================================================================
 * 4. A cap lowered through the op mid-round, and the two sides after it.
 *
 * The whole path this time rather than an event written by hand: the server
 * commits the table, clamps its own records against it and publishes, and the
 * client writes the table and clamps what it is holding. The case then pumps
 * on past the arrival, so whatever the server goes on to say about those
 * records — the next snapshot restating them — has been said before the two
 * are compared.
 *
 * A server that committed the table and left its records alone fails the
 * server-side cap below on its own, and would then undo the client's clamp
 * with that snapshot.
 *
 * Pill armour is compared exactly: nothing in an idle round moves it. Base
 * stocks are compared against the cap on both sides and not to each other —
 * the round goes on refuelling and draining them, so the client's copy is
 * allowed to be a snapshot behind the server's.
 * ================================================================ */
int run_loopback_sim_rules_agree(void) {
    LoopbackHarness h;
    GameSim        *cl;
    GameSim        *sv;
    BYTE            nBases, nPills, i;
    bool            sawBaseAbove = false;
    bool            sawPillAbove = false;
    int             at;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "RuleAgree", false, NULL, 90413),
                  "could not start the loopback harness");

    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    UT_ASSERT_MSG(at > 0, "the client never finished connecting");
    loopbackHarnessPumpUntil(&h, SETTLE_MAX, NULL, NULL);

    cl = clientSimGetGameSim(h.cs);
    sv = &h.sim->sim;
    UT_ASSERT_MSG(cl != NULL && cl->bs != NULL && cl->pb != NULL,
                  "the client has no world to compare");

    nPills = pillsGetNumPills(&sv->pb);
    nBases = basesGetNumBases(&sv->bs);
    for (i = 0; i < nPills; i++) {
        if ((*sv->pb).item[i].armour > SR_OP_PILL_ARMOUR_CAP) {
            sawPillAbove = true;
        }
    }
    for (i = 0; i < nBases; i++) {
        if ((*sv->bs).item[i].shells > SR_OP_BASE_SHELLS_CAP) {
            sawBaseAbove = true;
        }
    }
    if (!sawPillAbove || !sawBaseAbove) {
        loopbackHarnessStop(&h);
        UT_ASSERT_MSG(sawPillAbove && sawBaseAbove,
                      "setup: the server holds no pillbox above %d armour or "
                      "no base above %d shells, so the caps below would strand "
                      "nothing", SR_OP_PILL_ARMOUR_CAP, SR_OP_BASE_SHELLS_CAP);
    }

    UT_ASSERT_MSG(srSetRule(h.sim, SCN_RULE_pill_max_armour,
                            (double)SR_OP_PILL_ARMOUR_CAP) == SCN_OP_OK,
                  "the server refused a pill armour cap of %d",
                  SR_OP_PILL_ARMOUR_CAP);
    UT_ASSERT_MSG(srSetRule(h.sim, SCN_RULE_base_full_shells,
                            (double)SR_OP_BASE_SHELLS_CAP) == SCN_OP_OK,
                  "the server refused a base shells cap of %d",
                  SR_OP_BASE_SHELLS_CAP);

    at = loopbackHarnessPumpUntil(&h, CHANGE_MAX, pred_client_caps, NULL);
    UT_ASSERT_MSG(at > 0, "neither cap reached the client's own table");
    loopbackHarnessPumpUntil(&h, SETTLE_MAX, NULL, NULL);

    for (i = 0; i < nPills; i++) {
        BYTE onServer = (*sv->pb).item[i].armour;
        BYTE onClient = (*cl->pb).item[i].armour;
        if (onServer > SR_OP_PILL_ARMOUR_CAP || onServer != onClient) {
            loopbackHarnessStop(&h);
            UT_ASSERT_MSG(onServer <= SR_OP_PILL_ARMOUR_CAP,
                          "pillbox %u still holds %u armour on the server "
                          "against a cap of %d — the server committed the "
                          "table without clamping its own records",
                          (unsigned)i, (unsigned)onServer,
                          SR_OP_PILL_ARMOUR_CAP);
            UT_ASSERT_MSG(onServer == onClient,
                          "pillbox %u holds %u armour on the server and %u on "
                          "the client", (unsigned)i, (unsigned)onServer,
                          (unsigned)onClient);
        }
    }
    for (i = 0; i < nBases; i++) {
        BYTE onServer = (*sv->bs).item[i].shells;
        BYTE onClient = (*cl->bs).item[i].shells;
        if (onServer > SR_OP_BASE_SHELLS_CAP ||
            onClient > SR_OP_BASE_SHELLS_CAP) {
            loopbackHarnessStop(&h);
            UT_ASSERT_MSG(onServer <= SR_OP_BASE_SHELLS_CAP,
                          "base %u still holds %u shells on the server against "
                          "a cap of %d — the server committed the table "
                          "without clamping its own records",
                          (unsigned)i, (unsigned)onServer,
                          SR_OP_BASE_SHELLS_CAP);
            UT_ASSERT_MSG(onClient <= SR_OP_BASE_SHELLS_CAP,
                          "base %u holds %u shells on the client against a cap "
                          "of %d", (unsigned)i, (unsigned)onClient,
                          SR_OP_BASE_SHELLS_CAP);
        }
    }

    loopbackHarnessStop(&h);
    return 0;
}
