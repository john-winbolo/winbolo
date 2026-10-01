/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Regression (issue #340): a pillbox in the man's hands when his seat is torn
 * down mid-round was lost for the rest of the round.
 *
 * Ordering a pill built takes it off the tank's carry list and hands it to the
 * man, leaving the pillbox's record marked inTank the whole time he walks it
 * over. The teardown then dropped the tank's remaining cargo through
 * tankDestroy — which never sees the man's pill, it is not on that list any
 * more — and deleted the man outright, without the pill drop his death routine
 * does. The record survived still flagged as carried, by a man who no longer
 * existed: off the map, so nobody could shoot it, capture it or pick it up.
 *
 * Two teardowns delete a man mid-round, and both had it:
 *
 * run_lgm_quit_drops_carried_pill      — serverSimRemovePlayer, the leave path
 *                                        the issue was filed against. The
 *                                        ownership migration below the drop
 *                                        only changed whose name was on the
 *                                        record, so the pill has to go down
 *                                        ahead of it to change hands with the
 *                                        rest of the leaver's.
 * run_lgm_unfield_drops_carried_pill   — serverSimUnfieldBot, which takes a
 *                                        held seat off the field between waves
 *                                        and keeps the roster entry. No
 *                                        migration runs here at all, so a pill
 *                                        stranded on this path stayed under the
 *                                        name of a seat that was not on the
 *                                        field — worse than the leave, and
 *                                        reached by every scenario that removes
 *                                        a bot from a seat marked keepSeat.
 *
 * Both drive the real order path — tank picks the pill up, player orders it
 * built, man walks out with it — then tear the seat down mid-errand and read
 * the pillbox list back. Pre-fix the pill is still inTank and pillsExistPos
 * finds nothing at its square.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, playerConnected, lobbyPlayers */
#include "server_sim_scenario.h"   /* serverSimUnfieldBot — the between-waves path */
#include "game_sim.h"              /* GameSim: mp/bs/pb/mns, tanks[], lgmen[] */
#include "lgm.h"                   /* lgmAddRequest, lgmGetMX/MY, struct lgmObj */
#include "bolo_map.h"              /* mapGetPos and the terrain constants */
#include "pillbox.h"               /* pillsGetPill, pillsExistPos, pillsGetNumPills */
#include "bases.h"                 /* basesExistPos */
#include "mines.h"                 /* minesExistPos */
#include "tank.h"                  /* tankTakePill, tankSetTrees, tankGetMX/MY */
#include "test_harness.h"

#define LQP_SLOT 0

/* A grass square the man could be sent to build on: no base, pill or mine,
 * and not the one the tank is standing on (an order there is refused). */
static bool lqp_find_build_tile(ServerSim *sim, BYTE *ox, BYTE *oy) {
    BYTE tx = tankGetMX(&sim->sim.tanks[LQP_SLOT]);
    BYTE ty = tankGetMY(&sim->sim.tanks[LQP_SLOT]);
    int x, y;
    for (y = 0; y < 256; y++) {
        for (x = 0; x < 256; x++) {
            if ((BYTE)x == tx && (BYTE)y == ty) {
                continue;
            }
            if (mapGetPos(&sim->sim.mp, (BYTE)x, (BYTE)y) == GRASS &&
                !basesExistPos(&sim->sim.bs, (BYTE)x, (BYTE)y) &&
                !pillsExistPos(&sim->sim.pb, (BYTE)x, (BYTE)y) &&
                !minesExistPos(&sim->sim.mns, &sim->sim.mp, (BYTE)x, (BYTE)y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* Put the slot's man out on the map holding a map pillbox, the way a build
 * order does it, and hand back the pill number and the square he is on.
 * Returns 0 when he is really out there holding it — the pre-conditions are
 * asserted here, so neither case below can pass vacuously. */
static int lqp_send_man_out(ServerSim *sim, BYTE *outPill, BYTE *outManX,
                            BYTE *outManY) {
    GameSim *gs = &sim->sim;
    BYTE pillNum = 1;
    BYTE bx = 0, by = 0;
    pillbox p;

    UT_ASSERT_MSG(gs->tanks[LQP_SLOT] != NULL && gs->lgmen[LQP_SLOT] != NULL,
                  "slot %d joined without a tank or a man", LQP_SLOT);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "the map carries no pillboxes to hand the man");

    /* Pick a pill that is on the map, and put it in the tank the way a
     * capture does. */
    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, pillNum);
    UT_ASSERT_MSG(p.inTank == FALSE,
                  "pill %u starts carried — this test needs one on the map",
                  (unsigned)pillNum);
    tankTakePill(gs, &gs->tanks[LQP_SLOT], pillNum);

    /* Order it built somewhere the man has to walk to. The order costs wood,
     * so stock the tank first; the tank picks the pill off its own carry list
     * as it sends him. */
    UT_ASSERT_MSG(lqp_find_build_tile(sim, &bx, &by),
                  "no plain grass tile found to send the man to");
    tankSetTrees(gs, &gs->tanks[LQP_SLOT], (BYTE)TANK_FULL_TREES);
    lgmAddRequest(gs, &gs->lgmen[LQP_SLOT], &gs->tanks[LQP_SLOT], bx, by,
                  LGM_PILL_REQUEST);

    /* Not vacuous: he must really be out there holding it, and the pillbox
     * must really be off the map while he does. */
    UT_ASSERT_MSG(gs->lgmen[LQP_SLOT]->numPills == pillNum,
                  "the man is not carrying pill %u (numPills=%u) — the build "
                  "order never went out",
                  (unsigned)pillNum,
                  (unsigned)gs->lgmen[LQP_SLOT]->numPills);
    UT_ASSERT_MSG(gs->lgmen[LQP_SLOT]->inTank == FALSE,
                  "the man never left the tank");
    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, pillNum);
    UT_ASSERT_MSG(p.inTank == TRUE,
                  "pill %u is not marked carried while the man holds it",
                  (unsigned)pillNum);

    *outPill = pillNum;
    *outManX = lgmGetMX(&gs->lgmen[LQP_SLOT]);
    *outManY = lgmGetMY(&gs->lgmen[LQP_SLOT]);
    return 0;
}

/* The pillbox is back on the map, dead, on or just past the square he was
 * standing on, and no longer flagged as carried. Pre-fix inTank is still TRUE
 * and pillsExistPos finds nothing anywhere.
 *
 * The x window is one-sided and the y window is not: the drop searches down
 * and then east from the man's raw square, while lgmGetMY backs his position
 * off by two world units before shifting, so the square it names can sit one
 * below the one the search started from. */
static int lqp_assert_dropped(GameSim *gs, BYTE pillNum, BYTE manX, BYTE manY,
                              BYTE *outX, BYTE *outY) {
    pillbox p;
    int dx, dy;

    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, pillNum);
    UT_ASSERT_MSG(p.inTank == FALSE,
                  "pill %u is still marked carried after its carrier was torn "
                  "down — it is nobody's to pick up for the rest of the round",
                  (unsigned)pillNum);
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, p.x, p.y) == TRUE,
                  "pill %u is not on the map at its own square (%u,%u)",
                  (unsigned)pillNum, (unsigned)p.x, (unsigned)p.y);
    UT_ASSERT_MSG(p.armour == 0,
                  "pill %u landed with armour %u — a dropped pill lands dead, "
                  "the way a death lands it",
                  (unsigned)pillNum, (unsigned)p.armour);
    dx = (int)p.x - (int)manX;
    dy = (int)p.y - (int)manY;
    UT_ASSERT_MSG(dx >= 0 && dx <= 12 && dy >= -1 && dy <= 12,
                  "pill %u landed at (%u,%u), nowhere near the square the "
                  "man was on (%u,%u)",
                  (unsigned)pillNum, (unsigned)p.x, (unsigned)p.y,
                  (unsigned)manX, (unsigned)manY);
    *outX = p.x;
    *outY = p.y;
    return 0;
}

/* The drop is announced, and announced as the dead thing it is. Every way a
 * carried pillbox reaches the map raises EVENT_PILL_PLACED, so armour is the
 * only byte separating one a builder put up from one a corpse dropped — a
 * listener that read the player instead would credit a live gun to a slot on
 * its way out. Scans the tick's queue rather than taking the first event: the
 * teardown queues a player-leave and the pill diff alongside this one. */
static int lqp_assert_placed_event(ServerSim *sim, BYTE pillNum, BYTE px,
                                   BYTE py) {
    const GameEvent *evs = serverSimGetEvents(sim);
    uint8_t n = serverSimGetEventCount(sim);
    uint8_t i;

    for (i = 0; i < n; i++) {
        if (evs[i].type != EVENT_PILL_PLACED) {
            continue;
        }
        if (evs[i].data[1] != (BYTE)(pillNum - 1)) {
            continue;
        }
        UT_ASSERT_MSG(evs[i].data[4] == 0,
                      "pill %u was announced placed with armour %u — a "
                      "dropped pill is dead, and armour is the only thing "
                      "telling a listener it is not a gun that just came up",
                      (unsigned)pillNum, (unsigned)evs[i].data[4]);
        UT_ASSERT_MSG(evs[i].data[2] == px && evs[i].data[3] == py,
                      "pill %u was announced at (%u,%u) but landed at (%u,%u)",
                      (unsigned)pillNum, (unsigned)evs[i].data[2],
                      (unsigned)evs[i].data[3], (unsigned)px, (unsigned)py);
        return 0;
    }
    UT_ASSERT_MSG(0,
                  "no EVENT_PILL_PLACED for pill %u in the %u events the "
                  "teardown queued — the drop happened but nobody was told",
                  (unsigned)pillNum, (unsigned)n);
    return 1;
}

int run_lgm_quit_drops_carried_pill(void) {
    ServerSim *sim = ut_make_running_sim("Quitter");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = &sim->sim;
    BYTE pillNum = 0, manX = 0, manY = 0, pillX = 0, pillY = 0;
    int rc = lqp_send_man_out(sim, &pillNum, &manX, &manY);
    if (rc != 0) {
        serverSimDestroy(sim);
        return rc;
    }

    /* He quits mid-errand. */
    serverSimRemovePlayer(sim, LQP_SLOT);
    UT_ASSERT_MSG(gs->lgmen[LQP_SLOT] == NULL,
                  "the man survived the removal — nothing was torn down");

    rc = lqp_assert_dropped(gs, pillNum, manX, manY, &pillX, &pillY);
    if (rc == 0) {
        rc = lqp_assert_placed_event(sim, pillNum, pillX, pillY);
    }
    if (rc != 0) {
        serverSimDestroy(sim);
        return rc;
    }

    /* And it goes through the same ownership handover as the rest of the
     * leaver's pills: no connected ally here, so it turns neutral. That only
     * holds because the drop happens before the migration walks the list. */
    UT_ASSERT_MSG(pillsGetPillOwner(&gs->pb, pillNum) == NEUTRAL,
                  "dropped pill %u kept owner %u — the migration ran before "
                  "the drop and missed it",
                  (unsigned)pillNum,
                  (unsigned)pillsGetPillOwner(&gs->pb, pillNum));

    serverSimDestroy(sim);
    return 0;
}

/* The other teardown that deletes a man mid-round. A scenario removing a bot
 * from a seat marked keepSeat reaches serverSimUnfieldBot rather than emptying
 * the seat, and that path destroys the tank and the man exactly as the leave
 * does — so it lost the man's pillbox exactly as the leave did.
 *
 * The seat here is the harness's ordinary player rather than a fielded bot:
 * the wave machinery decides which teardown runs, and this is a test of the
 * teardown itself. serverSimUnfieldBot asks only that the seat is connected
 * and fielded, and the botManagerIsBot branch it opens with is the parked
 * runner, which has no part in what happens to the pillbox. */
int run_lgm_unfield_drops_carried_pill(void) {
    ServerSim *sim = ut_make_running_sim("Wave seat");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = &sim->sim;
    BYTE pillNum = 0, manX = 0, manY = 0, pillX = 0, pillY = 0;
    int rc = lqp_send_man_out(sim, &pillNum, &manX, &manY);
    if (rc != 0) {
        serverSimDestroy(sim);
        return rc;
    }

    /* The seat a wave holds: on the field, and kept when it comes off. */
    sim->lobbyPlayers[LQP_SLOT].fielded  = true;
    sim->lobbyPlayers[LQP_SLOT].keepSeat = true;

    serverSimUnfieldBot(sim, LQP_SLOT);
    UT_ASSERT_MSG(gs->lgmen[LQP_SLOT] == NULL,
                  "the man survived the unfield — nothing was torn down");
    UT_ASSERT_MSG(sim->playerConnected[LQP_SLOT] == TRUE,
                  "the unfield emptied the seat — this case is meant to cover "
                  "the path that keeps it");

    rc = lqp_assert_dropped(gs, pillNum, manX, manY, &pillX, &pillY);
    if (rc == 0) {
        rc = lqp_assert_placed_event(sim, pillNum, pillX, pillY);
    }
    if (rc != 0) {
        serverSimDestroy(sim);
        return rc;
    }

    /* Nothing the seat owns changes hands on an unfield — there is no
     * migration on this path at all — so the pillbox it leaves on the map is
     * still its own, waiting for the wave that fields the seat again. This is
     * what made the stranding worse here than on a leave: an undropped pill
     * stayed carried AND stayed under this name. */
    UT_ASSERT_MSG(pillsGetPillOwner(&gs->pb, pillNum) == LQP_SLOT,
                  "dropped pill %u came back owned by %u, not the seat that "
                  "was holding it — an unfield migrates nothing",
                  (unsigned)pillNum,
                  (unsigned)pillsGetPillOwner(&gs->pb, pillNum));

    serverSimDestroy(sim);
    return 0;
}
