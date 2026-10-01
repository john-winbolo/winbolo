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
 * What a client does with a CTRL_SIM_RULES event it should not trust
 * (test_sim_rules_client_check.c).
 *
 * The event is bytes off the wire, and the server that sent them is not this
 * process. A table that fails the check has to leave the client's own table
 * exactly where it was — not partly written, not written and then repaired —
 * because the numbers in it reach the tank movement, the shell flight and the
 * pill and base clamps, and one of them reaching those as a NaN is undefined
 * behaviour rather than a wrong game.
 *
 * The cases:
 *   - every_carried_field_is_checked: one field at a time, driven outside its
 *     own row, and the client's table unchanged byte for byte afterwards. The
 *     fields come from CTRL_SIM_RULES' own X-macro lists, so a rule added to
 *     the event joins this case with it rather than slipping past.
 *   - nan_and_inf_rates_refused: the two float values no range test catches
 *     unless it is written as a negated in-range test.
 *   - attack_pair_refused: a pair whose two sides the event carries is asked
 *     on the client, not only on the server.
 *   - server_only_pair_not_asked: a table a real server could be running,
 *     whose carried half disagrees with a rule the event leaves behind, is
 *     taken rather than refused — the reason the client asks the carried
 *     arms and not the whole check.
 *   - valid_table_applies: the ordinary path still lands, and still reclamps
 *     the records the client is holding against the new caps.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "game_sim.h"
#include "sim_rules.h"
#include "pillbox.h"
#include "bases.h"
#include "test_harness.h"

/* Out of range for every integer rule the event carries: each of their rows
 * starts at 0 or 1, so one value below zero misses all of them. */
#define SR_INT_BAD INT32_MIN

/* Out of range for every float rule: the turn rates start at 0.0 and the
 * tank rates at 0.01, so a negative rate misses both. */
#define SR_FLT_BAD (-1.0f)

/* ── Fixtures ────────────────────────────────────────────────────── */

/* A CTRL_SIM_RULES event carrying the classic value of every rule it
 * carries — the event a freshly started classic server would publish. */
static void srClassicEvent(ControlEvent *evt) {
    SimRules classic;

    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_SIM_RULES;
    simRulesClassic(&classic);
#define SR_FILL_FIELD(name) evt->u.simRules.name = classic.name;
    CTRL_SIM_RULES_ALL_FIELDS(SR_FILL_FIELD)
#undef SR_FILL_FIELD
}

/* A client with a pillbox and a base on its lists, both at their classic
 * caps, so the reclamp a new table runs has records to act on. */
static ClientSim *srClient(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim   *gs;
    pillbox    pill;
    base       bse;

    if (cs == NULL) {
        return NULL;
    }
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);

    pillsSetNumPills(&gs->pb, 1);
    memset(&pill, 0, sizeof(pill));
    pill.x = 41;
    pill.y = 40;
    pill.owner = NEUTRAL;
    pill.armour = (BYTE)gs->rules.pill_max_armour;
    pill.speed = (BYTE)gs->rules.pill_attack_ticks;
    pillsSetPill(gs, &gs->pb, &pill, 1);

    basesSetNumBases(&gs->bs, 1);
    memset(&bse, 0, sizeof(bse));
    bse.x = 61;
    bse.y = 60;
    bse.owner = NEUTRAL;
    bse.armour = (BYTE)gs->rules.base_full_armour;
    bse.shells = (BYTE)gs->rules.base_full_shells;
    bse.mines  = (BYTE)gs->rules.base_full_mines;
    basesSetBase(&gs->bs, &bse, 1);

    return cs;
}

/* Apply evt and say whether the client's table came through untouched. The
 * whole struct is compared, not the one field the case moved: a check that
 * refused after writing half the table would leave the other rules changed,
 * and that is the failure this asks about. */
static bool srTableUnchanged(ClientSim *cs, const ControlEvent *evt) {
    GameSim  *gs = clientSimGetGameSim(cs);
    SimRules  before = gs->rules;

    clientSimApplyControl(cs, evt);
    return memcmp(&gs->rules, &before, sizeof(SimRules)) == 0;
}

/* ── Cases ───────────────────────────────────────────────────────── */

/* Every rule the event carries, one at a time, driven outside its own row.
 * The four lists between them are every carried rule, so a rule added to any
 * of them is a rule this case starts driving on its own. */
int run_sim_rules_client_check_every_carried_field(void) {
    ClientSim *cs = srClient();

    UT_ASSERT_MSG(cs != NULL, "could not build a client");

#define SR_DRIVE_INT(name)                                                   \
    do {                                                                     \
        ControlEvent evt;                                                    \
        srClassicEvent(&evt);                                                \
        evt.u.simRules.name = SR_INT_BAD;                                    \
        if (!srTableUnchanged(cs, &evt)) {                                   \
            clientSimDestroy(cs);                                            \
            UT_ASSERT_MSG(false,                                             \
                          "the client took a table whose %s was %ld — the "  \
                          "rule is outside its own row and the whole table " \
                          "should have been refused", #name,                 \
                          (long)SR_INT_BAD);                                 \
        }                                                                    \
    } while (0);

#define SR_DRIVE_FLT(name)                                                   \
    do {                                                                     \
        ControlEvent evt;                                                    \
        srClassicEvent(&evt);                                                \
        evt.u.simRules.name = SR_FLT_BAD;                                    \
        if (!srTableUnchanged(cs, &evt)) {                                   \
            clientSimDestroy(cs);                                            \
            UT_ASSERT_MSG(false,                                             \
                          "the client took a table whose %s was %g — the "   \
                          "rate is outside its own row and the whole table " \
                          "should have been refused", #name,                 \
                          (double)SR_FLT_BAD);                               \
        }                                                                    \
    } while (0);

    CTRL_SIM_RULES_U8_FIELDS(SR_DRIVE_INT)
    CTRL_SIM_RULES_U16_FIELDS(SR_DRIVE_INT)
    CTRL_SIM_RULES_U32_FIELDS(SR_DRIVE_INT)
    CTRL_SIM_RULES_F32_FIELDS(SR_DRIVE_FLT)
    CTRL_SIM_RULES_EXT_U8_FIELDS(SR_DRIVE_INT)

#undef SR_DRIVE_INT
#undef SR_DRIVE_FLT

    clientSimDestroy(cs);
    return 0;
}

/* A NaN and an infinity in the float rates. Neither is caught by a range
 * test written as two comparisons that pass on "not less than the floor",
 * and a NaN speed is what the tank code later casts to a byte. */
int run_sim_rules_client_check_nan_and_inf(void) {
    ClientSim   *cs = srClient();
    ControlEvent evt;
    bool         heldNan;
    bool         heldInf;

    UT_ASSERT_MSG(cs != NULL, "could not build a client");

    srClassicEvent(&evt);
    evt.u.simRules.turn_grass = (float)NAN;
    heldNan = srTableUnchanged(cs, &evt);

    srClassicEvent(&evt);
    evt.u.simRules.tank_accel_rate = (float)INFINITY;
    heldInf = srTableUnchanged(cs, &evt);

    clientSimDestroy(cs);
    UT_ASSERT_MSG(heldNan,
                  "the client took a table whose turn_grass was a NaN");
    UT_ASSERT_MSG(heldInf,
                  "the client took a table whose tank_accel_rate was an "
                  "infinity");
    return 0;
}

/* A pair whose two sides the event both carries: a hurt pillbox's fastest
 * fire cannot be slower than an untouched one's rate, or the clamp the
 * pillbox code applies has no window to land in. Both values are inside
 * their own rows, so only the pair can refuse this table. */
int run_sim_rules_client_check_attack_pair(void) {
    ClientSim   *cs = srClient();
    ControlEvent evt;
    bool         held;

    UT_ASSERT_MSG(cs != NULL, "could not build a client");

    srClassicEvent(&evt);
    evt.u.simRules.pill_attack_ticks     = 20;
    evt.u.simRules.pill_attack_min_ticks = 40;
    held = srTableUnchanged(cs, &evt);

    clientSimDestroy(cs);
    UT_ASSERT_MSG(held,
                  "the client took a table whose pill_attack_min_ticks (40) "
                  "was above its pill_attack_ticks (20)");
    return 0;
}

/* The other half of the same question: a table a legitimate server could be
 * running, whose carried rules argue with a rule the event does not carry.
 *
 * tank_full_trees is carried and lgm_cost_pill_new is not, and the server's
 * whole check holds the cost at or under the capacity. A server that lowered
 * both would publish the lowered capacity and keep the lowered cost to
 * itself; a client asking the whole check would compare the new capacity
 * against the classic cost it still reads and refuse a table that is fine.
 */
int run_sim_rules_client_check_server_only_pair(void) {
    ClientSim   *cs = srClient();
    GameSim     *gs;
    ControlEvent evt;
    SimRules     whole;
    char         why[SIM_RULES_WHY_LEN];
    int32_t      gotTrees;
    int32_t      gotShells;

    UT_ASSERT_MSG(cs != NULL, "could not build a client");
    gs = clientSimGetGameSim(cs);

    srClassicEvent(&evt);
    evt.u.simRules.tank_full_trees  = 3;   /* under the classic pill cost */
    evt.u.simRules.tank_full_shells = 60;  /* and one plain change beside it */

    /* Setup: the merged table really would fail the whole check, or the case
       would prove nothing about which arms the client asks. */
    whole = gs->rules;
    whole.tank_full_trees  = 3;
    whole.tank_full_shells = 60;
    UT_ASSERT_MSG(simRulesCheck(&whole, why, sizeof(why)) != SIM_RULES_OK,
                  "setup: the whole check passes this table, so the client "
                  "asking every arm would have taken it anyway");

    clientSimApplyControl(cs, &evt);
    gotTrees  = gs->rules.tank_full_trees;
    gotShells = gs->rules.tank_full_shells;
    clientSimDestroy(cs);

    UT_ASSERT_MSG(gotTrees == 3,
                  "the client reads %ld for tank_full_trees, expected 3 — it "
                  "refused a table over a pair whose other side it is never "
                  "sent", (long)gotTrees);
    UT_ASSERT_MSG(gotShells == 60,
                  "the client reads %ld for tank_full_shells, expected 60",
                  (long)gotShells);
    return 0;
}

/* The ordinary path: a table that passes lands, and the pill and base records
 * the client is already holding are clamped to the caps it brought. */
int run_sim_rules_client_check_valid_applies(void) {
    ClientSim   *cs = srClient();
    GameSim     *gs;
    ControlEvent evt;
    int32_t      gotReload;
    BYTE         gotArmour;
    BYTE         gotShells;

    UT_ASSERT_MSG(cs != NULL, "could not build a client");
    gs = clientSimGetGameSim(cs);

    UT_ASSERT_MSG((*gs->pb).item[0].armour > 3 &&
                      (*gs->bs).item[0].shells > 5,
                  "setup: the client holds no pillbox above 3 armour or no "
                  "base above 5 shells, so the reclamp would have nothing "
                  "to do");

    srClassicEvent(&evt);
    evt.u.simRules.tank_reload_ticks = 7;
    evt.u.simRules.pill_max_armour   = 3;
    evt.u.simRules.base_full_shells  = 5;
    clientSimApplyControl(cs, &evt);

    gotReload = gs->rules.tank_reload_ticks;
    gotArmour = (*gs->pb).item[0].armour;
    gotShells = (*gs->bs).item[0].shells;
    clientSimDestroy(cs);

    UT_ASSERT_MSG(gotReload == 7,
                  "the client reads %ld for tank_reload_ticks, expected 7 — "
                  "a table inside every carried row was refused",
                  (long)gotReload);
    UT_ASSERT_MSG(gotArmour <= 3,
                  "the pillbox still holds %u armour against a cap of 3",
                  (unsigned)gotArmour);
    UT_ASSERT_MSG(gotShells <= 5,
                  "the base still holds %u shells against a cap of 5",
                  (unsigned)gotShells);
    return 0;
}
