/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * pill_shell_cap and pill_max_shells_at_tank — whether pillboxes limit the
 * shells in the air at one tank, and the limit. Driven through pillsUpdate on a running server sim, with
 * the shells already "in the air" seeded straight into the list: nothing
 * here runs shellsUpdate, so a seeded shell never moves or lands and the
 * count pillsUpdate takes is exactly the one each case set up.
 *
 *   1. The switch off is no limit: a pill fires however many are coming.
 *   2. A tank at the cap is passed over for the next nearest one.
 *   3. With no other target the pill holds its shot, keeps justSeen and a
 *      full reload, and fires on the first update a shell comes free.
 *   4. A capped tank that leaves range clears justSeen as any other does.
 *   5. Shots fired in the same update count against the cap.
 *   6. A tank's own shells are not pill shells and do not count.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "game_sim.h"
#include "pillbox.h"
#include "shells.h"
#include "tank.h"
#include "test_harness.h"

#define PSC_PILL     0      /* the pill every case fires */
#define PSC_NEAR     1      /* the nearest tank */
#define PSC_FAR      2      /* a tank further off, still in range */
#define PSC_CAP      12

typedef struct {
    ServerSim *sim;
    GameSim   *gs;
    bool       conn[MAX_TANKS];
} PscFixture;

/* A running sim with three unallied players, every pill but PSC_PILL taken
 * off the map so no other pill adds shells, and the woods made to hide
 * nothing so where the tanks stand on the map cannot matter. */
static int psc_setup(PscFixture *f) {
    BYTE i;

    memset(f, 0, sizeof(*f));
    f->sim = ut_make_running_sim("Host");
    UT_ASSERT(f->sim != NULL);
    serverSimAddPlayer(f->sim, PSC_NEAR, "Near", false);
    serverSimAddPlayer(f->sim, PSC_FAR, "Far", false);
    f->gs = serverSimGetGameSim(f->sim);
    UT_ASSERT(f->gs != NULL);
    UT_ASSERT_MSG(pillsGetNumPills(&f->gs->pb) >= 2,
                  "the map should carry at least two pillboxes");
    UT_ASSERT(f->gs->tanks[PSC_NEAR] != NULL && f->gs->tanks[PSC_FAR] != NULL);

    for (i = 0; i < f->gs->pb->numPills; i++) {
        f->gs->pb->active[i] = FALSE;
    }
    f->gs->pb->active[PSC_PILL] = TRUE;
    f->gs->rules.tree_hide_distance = 65535;
    f->gs->rules.pill_shell_cap = 1;
    f->gs->rules.pill_max_shells_at_tank = PSC_CAP;
    return 0;
}

/* Ready pill num to fire this update: armed, out of the tank, reloaded and
 * already looking at a target. */
static void psc_arm_pill(PscFixture *f, BYTE num) {
    f->gs->pb->item[num].owner    = NEUTRAL;
    f->gs->pb->item[num].armour   = 15;
    f->gs->pb->item[num].inTank   = FALSE;
    f->gs->pb->item[num].reload   = f->gs->pb->item[num].speed;
    f->gs->pb->item[num].justSeen = TRUE;
    f->gs->pb->active[num]        = TRUE;
}

/* Stand tank t squares map squares east of pill num, or west of it on the
 * east half of the map, so the spot stays on the map. */
static void psc_place_tank(PscFixture *f, BYTE t, BYTE num, int squares) {
    int mx = f->gs->pb->item[num].x;
    int my = f->gs->pb->item[num].y;

    mx += (mx < 128) ? squares : -squares;
    f->gs->tanks[t]->x = (WORLD) ((mx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    f->gs->tanks[t]->y = (WORLD) ((my << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* Put n shells in the air at target, fired by owner. They are placed far
 * from everything and never updated. */
static void psc_seed(PscFixture *f, int n, BYTE owner, BYTE target) {
    int i;

    for (i = 0; i < n; i++) {
        shellsAddItem(f->gs, &f->gs->shs, 100, 100, 0, 1, owner, target, FALSE);
    }
}

static int psc_count(PscFixture *f) {
    int    n = 0;
    shells q;

    for (q = f->gs->shs; q != NULL; q = q->next) {
        n++;
    }
    return n;
}

/* One pillsUpdate with only the tanks in the list connected. */
static void psc_update(PscFixture *f, bool nearOn, bool farOn) {
    memset(f->conn, 0, sizeof(f->conn));
    f->conn[PSC_NEAR] = nearOn;
    f->conn[PSC_FAR]  = farOn;
    pillsUpdate(f->gs, f->gs->tanks, f->conn, MAX_TANKS);
}

int run_pill_shell_cap_off_is_no_limit(void) {
    PscFixture f;
    int        before;

    UT_ASSERT(psc_setup(&f) == 0);
    f.gs->rules.pill_shell_cap = 0;
    psc_arm_pill(&f, PSC_PILL);
    psc_place_tank(&f, PSC_NEAR, PSC_PILL, 1);
    psc_seed(&f, 40, NEUTRAL, PSC_NEAR);
    before = psc_count(&f);

    psc_update(&f, true, false);

    UT_ASSERT_MSG(psc_count(&f) == before + 1,
                  "with the cap off the pill should fire at a tank with 40 "
                  "shells coming");
    UT_ASSERT(f.gs->shs->target == PSC_NEAR);

    serverSimDestroy(f.sim);
    return 0;
}

int run_pill_shell_cap_retargets_next_nearest(void) {
    PscFixture f;
    int        before;

    UT_ASSERT(psc_setup(&f) == 0);
    psc_arm_pill(&f, PSC_PILL);
    psc_place_tank(&f, PSC_NEAR, PSC_PILL, 1);
    psc_place_tank(&f, PSC_FAR, PSC_PILL, 3);

    /* One under the cap: the nearest tank is still the target. */
    psc_seed(&f, PSC_CAP - 1, NEUTRAL, PSC_NEAR);
    before = psc_count(&f);
    psc_update(&f, true, true);
    UT_ASSERT(psc_count(&f) == before + 1);
    UT_ASSERT_MSG(f.gs->shs->target == PSC_NEAR,
                  "one under the cap the pill fired at %d, not the nearest",
                  f.gs->shs->target);

    /* That shot made twelve, so the next one goes to the tank further off. */
    psc_arm_pill(&f, PSC_PILL);
    before = psc_count(&f);
    psc_update(&f, true, true);
    UT_ASSERT(psc_count(&f) == before + 1);
    UT_ASSERT_MSG(f.gs->shs->target == PSC_FAR,
                  "at the cap the pill fired at %d, not the next nearest",
                  f.gs->shs->target);

    serverSimDestroy(f.sim);
    return 0;
}

int run_pill_shell_cap_holds_then_fires_when_freed(void) {
    PscFixture f;
    int        before;
    BYTE       speed;

    UT_ASSERT(psc_setup(&f) == 0);
    psc_arm_pill(&f, PSC_PILL);
    psc_place_tank(&f, PSC_NEAR, PSC_PILL, 1);
    psc_seed(&f, PSC_CAP, NEUTRAL, PSC_NEAR);
    speed  = f.gs->pb->item[PSC_PILL].speed;
    before = psc_count(&f);

    psc_update(&f, true, false);

    UT_ASSERT_MSG(psc_count(&f) == before,
                  "the pill fired at a tank already at the cap");
    UT_ASSERT_MSG(f.gs->pb->item[PSC_PILL].justSeen == TRUE,
                  "holding for the cap cleared justSeen");
    UT_ASSERT_MSG(f.gs->pb->item[PSC_PILL].reload >= speed,
                  "holding for the cap reset the reload to %d",
                  f.gs->pb->item[PSC_PILL].reload);

    /* A shell lands. The list keeps it until the next shellsUpdate, but a
       dead shell is no longer coming, so the pill fires straight away. */
    f.gs->shs->shellDead = TRUE;
    psc_update(&f, true, false);

    UT_ASSERT_MSG(psc_count(&f) == before + 1,
                  "the pill did not fire on the update a shell came free");
    UT_ASSERT(f.gs->shs->target == PSC_NEAR);

    serverSimDestroy(f.sim);
    return 0;
}

int run_pill_shell_cap_out_of_range_clears_just_seen(void) {
    PscFixture f;
    int        before;

    UT_ASSERT(psc_setup(&f) == 0);
    psc_arm_pill(&f, PSC_PILL);
    psc_place_tank(&f, PSC_NEAR, PSC_PILL, 1);
    psc_seed(&f, PSC_CAP, NEUTRAL, PSC_NEAR);

    psc_update(&f, true, false);
    UT_ASSERT(f.gs->pb->item[PSC_PILL].justSeen == TRUE);

    /* Twenty squares is well past the classic range of eight. */
    UT_ASSERT(f.gs->rules.pill_range < (20 << TANK_SHIFT_MAPSIZE));
    psc_place_tank(&f, PSC_NEAR, PSC_PILL, 20);
    psc_update(&f, true, false);
    UT_ASSERT_MSG(f.gs->pb->item[PSC_PILL].justSeen == FALSE,
                  "a capped tank that left range kept justSeen set");

    /* Back in range with a shell free: it is seen again before it is shot,
       the same as a tank that had never been capped. */
    f.gs->shs->shellDead = TRUE;
    psc_place_tank(&f, PSC_NEAR, PSC_PILL, 1);
    before = psc_count(&f);
    psc_update(&f, true, false);
    UT_ASSERT_MSG(psc_count(&f) == before,
                  "the pill fired without seeing the tank again first");
    UT_ASSERT(f.gs->pb->item[PSC_PILL].justSeen == TRUE);

    serverSimDestroy(f.sim);
    return 0;
}

int run_pill_shell_cap_counts_same_update_shots(void) {
    PscFixture f;
    int        before;
    BYTE       second = PSC_PILL + 1;

    UT_ASSERT(psc_setup(&f) == 0);
    f.gs->rules.pill_max_shells_at_tank = 1;
    f.gs->pb->item[second].x = f.gs->pb->item[PSC_PILL].x;
    f.gs->pb->item[second].y = (BYTE) (f.gs->pb->item[PSC_PILL].y + 1);
    psc_arm_pill(&f, PSC_PILL);
    psc_arm_pill(&f, second);
    psc_place_tank(&f, PSC_NEAR, PSC_PILL, 1);
    before = psc_count(&f);

    psc_update(&f, true, false);

    UT_ASSERT_MSG(psc_count(&f) == before + 1,
                  "two pills fired %d shells in one update at a cap of one",
                  psc_count(&f) - before);
    UT_ASSERT_MSG(f.gs->pb->item[second].justSeen == TRUE,
                  "the pill held back in the same update lost justSeen");

    serverSimDestroy(f.sim);
    return 0;
}

int run_pill_shell_cap_ignores_tank_shells(void) {
    PscFixture f;
    int        before;

    UT_ASSERT(psc_setup(&f) == 0);
    psc_arm_pill(&f, PSC_PILL);
    psc_place_tank(&f, PSC_NEAR, PSC_PILL, 1);
    /* Shells fired by another tank carry no pill target. */
    psc_seed(&f, PSC_CAP * 2, PSC_FAR, NEUTRAL);
    before = psc_count(&f);

    psc_update(&f, true, false);

    UT_ASSERT_MSG(psc_count(&f) == before + 1,
                  "a tank's own shells counted against the pill cap");
    UT_ASSERT(f.gs->shs->target == PSC_NEAR);

    serverSimDestroy(f.sim);
    return 0;
}
