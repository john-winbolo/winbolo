/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Whether a shell that hits a tank sets off the mines on its square, the
 * rule tank_hit_sets_off_mines (test_tank_hit_mines.c).
 *
 * When a shell ends, shellsUpdate starts a mine fuse (minesExpAddItem) on the
 * square where it burst. With the rule at 1, the classic table, that is so
 * whatever the shell hit, a tank included: a tank that lays mines as it runs
 * has them set off by the shots of the tank chasing it. With the rule at 0, a
 * shell that hits or kills a tank starts no fuse; a shell that hits terrain,
 * or that runs out, still does.
 *
 *   run_tank_hit_mines_rule_on_sets_off   - rule 1: a hit and a kill on a
 *       tank standing on a mine each start a fuse on that square
 *   run_tank_hit_mines_rule_off_spares    - rule 0: the same hit and kill
 *       start no fuse and leave the mine on the map; the hit still does its
 *       damage, is still drawn and still reports a tank hit or kill
 *   run_tank_hit_mines_rule_off_ground    - rule 0: a shell that hits a
 *       mined forest square, and one that runs out over a mined road, each
 *       start a fuse on that square
 *
 * Every case steps the shell list the way the tick steps it (shellsUpdate
 * over the compacted tank and builder arrays). The shells rewind nothing, so
 * the shell bursts where it is.
 *
 * Reads the ServerSim, tank and mine structs directly; the unittests profile
 * permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, events[] */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "input_packet.h"          /* EVENT_EXPLOSION */
#include "game_sim.h"
#include "bases.h"
#include "bolo_map.h"
#include "mines.h"
#include "minesexp.h"
#include "pillbox.h"
#include "shells.h"
#include "tank.h"
#include "everard_map.h"
#include "test_harness.h"

#define THM_SHOOTER 0
#define THM_TARGET  1

/* The lane: the shooter on square 0, the target three squares on. */
#define THM_LANE_LEN  8
#define THM_AT_TARGET 3

/* A full-range tank shell. */
#define THM_SHELL_LEN 7

/* More game ticks than any shell lives. */
#define THM_SHELL_TICKS 80

/* What the shell-death callback was told. */
static int     thmDeaths;
static WORLD   thmDeathX;
static WORLD   thmDeathY;
static uint8_t thmDeathOutcome;

static void (*thmDeathPrev)(void *ctx, uint32_t fireTick,
                            uint32_t serverFireTick, BYTE owner,
                            WORLD impactWX, WORLD impactWY, uint8_t outcome);

static void thmDeathCb(void *ctx, uint32_t fireTick, uint32_t serverFireTick,
                       BYTE owner, WORLD impactWX, WORLD impactWY,
                       uint8_t outcome) {
    thmDeaths++;
    thmDeathX       = impactWX;
    thmDeathY       = impactWY;
    thmDeathOutcome = outcome;
    if (thmDeathPrev != NULL) {
        thmDeathPrev(ctx, fireTick, serverFireTick, owner, impactWX, impactWY,
                     outcome);
    }
}

static WORLD thmMiddle(BYTE m) {
    return (WORLD)(((WORLD)m << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* A run of squares with nothing on it and room around it, laid to road, two
   rows either side so the target has somewhere to stand off the lane. */
static bool thmLane(ServerSim *sim, BYTE *ox, BYTE *oy) {
    int x, y, dx, dy;

    for (y = 30; y < 220; y++) {
        for (x = 30; x < 200; x++) {
            bool clear = true;
            for (dy = -3; dy <= 3 && clear; dy++) {
                for (dx = -2; dx <= THM_LANE_LEN + 2 && clear; dx++) {
                    BYTE sx = (BYTE)(x + dx);
                    BYTE sy = (BYTE)(y + dy);
                    if (pillsExistPos(&sim->sim.pb, sx, sy) ||
                        basesExistPos(&sim->sim.bs, sx, sy) ||
                        minesExistPos(&sim->sim.mns, &sim->sim.mp, sx, sy)) {
                        clear = false;
                    }
                }
            }
            if (!clear) {
                continue;
            }
            for (dy = -2; dy <= 2; dy++) {
                for (dx = -1; dx <= THM_LANE_LEN; dx++) {
                    mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)(x + dx),
                              (BYTE)(y + dy), ROAD, FALSE, FALSE);
                }
            }
            *ox = (BYTE)x;
            *oy = (BYTE)y;
            return true;
        }
    }
    return false;
}

/* A tank at rest, ashore, facing east, with its centre at wx, wy. */
static void thmPlace(ServerSim *sim, BYTE slot, WORLD wx, WORLD wy) {
    tank *t = &sim->sim.tanks[slot];

    tankSetWorld(&sim->sim, t, wx, wy, (TURNTYPE)BRADIANS_EAST, FALSE);
    tankSetSpeed(t, 0);
    tankSetOnBoat(t, FALSE);
    tankClearBoatTrail(t);
    (*t)->bumpX = 0;
    (*t)->bumpY = 0;
}

/* A live mine on one square, on the terrain given. Mined terrain is the
   plain terrain plus MINE_SUBTRACT, which is what both lay paths write. */
static void thmLayMine(ServerSim *sim, BYTE mx, BYTE my, BYTE terrain) {
    GameSim *gs = &sim->sim;

    mapSetPos(gs, &gs->mp, mx, my, (BYTE)(terrain + MINE_SUBTRACT), FALSE,
              FALSE);
    minesAddItem(&gs->mns, mx, my);
    minesSetOwner(&gs->mns, mx, my, THM_TARGET);
}

/* One round with the two seats in, a clear lane, the shooter on its first
   square, the target two rows off the lane, the rule set to rule, no mine
   fuse burning and the shell-death callback recorded. */
static ServerSim *thmSim(BYTE *lx, BYTE *ly, int32_t rule) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);

    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, THM_SHOOTER, "Shooter", false);
    serverSimAddPlayer(sim, THM_TARGET, "Target", false);
    if (sim->sim.tanks[THM_SHOOTER] == NULL ||
        sim->sim.tanks[THM_TARGET] == NULL ||
        !thmLane(sim, lx, ly)) {
        serverSimDestroy(sim);
        return NULL;
    }
    thmPlace(sim, THM_SHOOTER, thmMiddle(*lx), thmMiddle(*ly));
    thmPlace(sim, THM_TARGET, thmMiddle((BYTE)(*lx + THM_AT_TARGET)),
             thmMiddle((BYTE)(*ly + 2)));
    sim->sim.rules.tank_hit_sets_off_mines = rule;
    minesExpDestroy(&sim->sim.minesExplosions);
    thmDeaths       = 0;
    thmDeathX       = 0;
    thmDeathY       = 0;
    thmDeathOutcome = 0;
    thmDeathPrev    = sim->sim.callbacks.shellDeath;
    sim->sim.callbacks.shellDeath = thmDeathCb;
    sim->eventCount = 0;
    return sim;
}

static void thmEnd(ServerSim *sim) {
    sim->sim.callbacks.shellDeath = thmDeathPrev;
    thmDeathPrev = NULL;
    serverSimDestroy(sim);
}

/* One shell from the shooter, east along the lane, with no rewind. Then step
   every shell in flight until none is left. */
static void thmFireAndFly(ServerSim *sim, BYTE lx, BYTE ly) {
    tank tanks[MAX_TANKS];
    lgm *lgms[MAX_TANKS];
    int  t;

    sim->sim.lagCompTicks = 0;
    shellsAddItem(&sim->sim, &sim->sim.shs, thmMiddle(lx), thmMiddle(ly),
                  (TURNTYPE)BRADIANS_EAST, (TURNTYPE)THM_SHELL_LEN, THM_SHOOTER,
                  NEUTRAL, DMG_NO_PILL, FALSE);
    for (t = 1; t <= THM_SHELL_TICKS && sim->sim.shs != NULL; t++) {
        BYTE n = 0;
        BYTE c;

        for (c = 0; c < MAX_TANKS; c++) {
            if (sim->playerConnected[c] && sim->sim.tanks[c] != NULL) {
                tanks[n] = sim->sim.tanks[c];
                lgms[n]  = &sim->sim.lgmen[c];
                n++;
            }
        }
        shellsUpdate(&sim->sim, tanks, n, lgms, &sim->sim.ss);
    }
}

/* How many mine fuses are burning, and whether one is on mx, my. */
static int thmFuses(ServerSim *sim, BYTE mx, BYTE my, bool *onSquare) {
    minesExp q = sim->sim.minesExplosions;
    int      n = 0;

    *onSquare = false;
    while (q != NULL) {
        if (q->x == mx && q->y == my) {
            *onSquare = true;
        }
        n++;
        q = q->next;
    }
    return n;
}

/* Whether the tick's buffer holds an EVENT_EXPLOSION. */
static bool thmHasExplosion(ServerSim *sim) {
    int i;

    for (i = 0; i < sim->eventCount; i++) {
        if (sim->events[i].type == EVENT_EXPLOSION) {
            return true;
        }
    }
    return false;
}

/* One shell at a target standing on a mined road square, with the rule at
   rule. kill leaves the target no armour, so the shell kills it. Checks the
   hit happened as it always does, and whether the fuse started. */
static int thmTankCase(int32_t rule, bool kill) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = thmSim(&lx, &ly, rule);
    GameSim   *gs;
    BYTE       mx, my;
    BYTE       armourBefore;
    bool       onSquare = false;
    int        fuses;
    int        rc = 0;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    mx = (BYTE)(lx + THM_AT_TARGET);
    my = ly;
    thmLayMine(sim, mx, my, ROAD);
    thmPlace(sim, THM_TARGET, thmMiddle(mx), thmMiddle(my));
    if (kill) {
        (*gs->tanks[THM_TARGET]).armour = 0;
    }
    armourBefore = tankGetArmour(&gs->tanks[THM_TARGET]);

    thmFireAndFly(sim, lx, ly);

    /* The hit itself is the same with either value. */
    UT_CHECK_MSG(thmDeaths == 1, "the shell ended %d times", thmDeaths);
    UT_CHECK_MSG(thmDeathOutcome == (kill ? SHELL_OUTCOME_TANK_KILL
                                          : SHELL_OUTCOME_TANK_HIT),
                 "the shell's outcome was %d", (int)thmDeathOutcome);
    UT_CHECK_MSG((BYTE)(thmDeathX >> TANK_SHIFT_MAPSIZE) == mx &&
                     (BYTE)(thmDeathY >> TANK_SHIFT_MAPSIZE) == my,
                 "the shell burst on square (%d,%d), not the mined (%d,%d)",
                 (int)(thmDeathX >> TANK_SHIFT_MAPSIZE),
                 (int)(thmDeathY >> TANK_SHIFT_MAPSIZE), (int)mx, (int)my);
    UT_CHECK_MSG(thmHasExplosion(sim), "the hit drew no explosion");
    if (kill) {
        UT_CHECK(tankIsDestroyed(&gs->tanks[THM_TARGET]));
    } else {
        UT_CHECK(tankGetArmour(&gs->tanks[THM_TARGET]) ==
                 (BYTE)(armourBefore - gs->rules.shell_damage));
    }

    fuses = thmFuses(sim, mx, my, &onSquare);
    if (rule != 0) {
        UT_CHECK_MSG(fuses == 1 && onSquare,
                     "with tank_hit_sets_off_mines 1 a tank %s left %d fuses, "
                     "%s on the mined square",
                     kill ? "kill" : "hit", fuses, onSquare ? "one" : "none");
    } else {
        UT_CHECK_MSG(fuses == 0,
                     "with tank_hit_sets_off_mines 0 a tank %s started %d "
                     "fuses", kill ? "kill" : "hit", fuses);
        UT_CHECK_MSG(mapIsMine(&gs->mp, mx, my) == TRUE,
                     "the mine under the hit tank is gone");
    }
done:
    thmEnd(sim);
    return rc;
}

/* ================================================================
 * 1. Rule 1, the classic table: a tank hit sets off the mine.
 * ================================================================ */
int run_tank_hit_mines_rule_on_sets_off(void) {
    SimRules classic;

    simRulesClassic(&classic);
    UT_ASSERT_MSG(classic.tank_hit_sets_off_mines == 1,
                  "the classic table has tank_hit_sets_off_mines %d",
                  (int)classic.tank_hit_sets_off_mines);
    UT_ASSERT(thmTankCase(1, false) == 0);
    UT_ASSERT(thmTankCase(1, true) == 0);
    return 0;
}

/* ================================================================
 * 2. Rule 0: a tank hit or kill leaves the mine alone.
 * ================================================================ */
int run_tank_hit_mines_rule_off_spares(void) {
    UT_ASSERT(thmTankCase(0, false) == 0);
    UT_ASSERT(thmTankCase(0, true) == 0);
    return 0;
}

/* ================================================================
 * 3. Rule 0: a terrain hit and a shell that runs out still set one off.
 * ================================================================ */
int run_tank_hit_mines_rule_off_ground(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    BYTE       mx, my;
    BYTE       i;
    bool       onSquare = false;
    int        fuses;
    int        rc = 0;

    /* (a) A mined forest square on the lane, the target off it. The shell
       stops on the forest, which is terrain, and sets the mine off. */
    sim = thmSim(&lx, &ly, 0);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    mx = (BYTE)(lx + THM_AT_TARGET);
    my = ly;
    thmLayMine(sim, mx, my, FOREST);
    thmFireAndFly(sim, lx, ly);
    UT_CHECK_MSG(thmDeaths == 1 && thmDeathOutcome == SHELL_OUTCOME_IMPACT,
                 "the shell ended %d times, last outcome %d", thmDeaths,
                 (int)thmDeathOutcome);
    UT_CHECK_MSG((BYTE)(thmDeathX >> TANK_SHIFT_MAPSIZE) == mx &&
                     (BYTE)(thmDeathY >> TANK_SHIFT_MAPSIZE) == my,
                 "the shell stopped on square (%d,%d), not the forest (%d,%d)",
                 (int)(thmDeathX >> TANK_SHIFT_MAPSIZE),
                 (int)(thmDeathY >> TANK_SHIFT_MAPSIZE), (int)mx, (int)my);
    fuses = thmFuses(sim, mx, my, &onSquare);
    UT_CHECK_MSG(fuses == 1 && onSquare,
                 "a terrain hit with tank_hit_sets_off_mines 0 left %d fuses, "
                 "%s on the mined forest", fuses, onSquare ? "one" : "none");
    thmEnd(sim);

    /* (b) Mined road the whole way along the lane past the shooter, the
       target off it. The shell runs out over one of those mines and sets it
       off. */
    sim = thmSim(&lx, &ly, 0);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    for (i = 1; i <= THM_LANE_LEN; i++) {
        thmLayMine(sim, (BYTE)(lx + i), ly, ROAD);
    }
    thmFireAndFly(sim, lx, ly);
    UT_CHECK_MSG(thmDeaths == 1 && thmDeathOutcome == SHELL_OUTCOME_EXPIRED,
                 "the shell ended %d times, last outcome %d", thmDeaths,
                 (int)thmDeathOutcome);
    mx = (BYTE)(thmDeathX >> TANK_SHIFT_MAPSIZE);
    my = (BYTE)(thmDeathY >> TANK_SHIFT_MAPSIZE);
    UT_CHECK_MSG(my == ly && mx > lx && mx <= (BYTE)(lx + THM_LANE_LEN),
                 "the shell ran out on square (%d,%d), off the mined lane",
                 (int)mx, (int)my);
    fuses = thmFuses(sim, mx, my, &onSquare);
    UT_CHECK_MSG(fuses == 1 && onSquare,
                 "a shell that ran out with tank_hit_sets_off_mines 0 left %d "
                 "fuses, %s on its square", fuses, onSquare ? "one" : "none");
done:
    thmEnd(sim);
    return rc;
}
