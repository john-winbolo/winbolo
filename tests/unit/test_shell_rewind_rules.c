/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * How far a shell rewinds what it is tested against
 * (test_shell_rewind_rules.c).
 *
 * Three rules in shells.c that decide which frame of the position history a
 * shell looks at. Each case here guards one line; the line is named at the
 * case.
 *
 *   run_shell_rewind_pill_shell_tank_present - a pill's shell does not
 *       rewind a tank. It misses the place the tank's record has and hits
 *       the place the tank is.
 *   run_shell_rewind_tank_full_comp          - a player's shell rewinds a
 *       tank by all of its compensationTicks: the record N back, not the
 *       record N - 1 or N + 1 back.
 *   run_shell_rewind_builder_one_less        - a player's shell rewinds a
 *       builder by one frame less than its compensationTicks
 *       (SHELL_LGM_REWIND_LESS): the record N - 1 back, not N and not N - 2.
 *       A shell that carries 1 still rewinds 1, never 0.
 *
 * A pill's shell against a builder is lgm_kill_pill_shell_present
 * (test_lgm_absent_on_shooter_screen.c) and is not repeated here.
 *
 * The histories are written directly, oldest frame first, with a different
 * position on every frame, so a test that reads the wrong frame reads a
 * place nothing is at. The shells are stepped the way the tick steps them
 * (shellsUpdate over the compacted tank and builder arrays); the history is
 * not written while a shell flies, so "N back" is one frame for the flight.
 *
 * Reads the ServerSim, tank, lgm and shell structs directly; the unittests
 * profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, posHistory, lgmPosHistory */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "game_sim.h"
#include "bases.h"
#include "bolo_map.h"
#include "lgm.h"
#include "mines.h"
#include "pillbox.h"
#include "position_history.h"
#include "shells.h"
#include "tank.h"
#include "everard_map.h"
#include "test_harness.h"

#define SRR_SHOOTER 0
#define SRR_TARGET  1

/* The lane: shells start on square 0, the target's lane square three on. */
#define SRR_LANE_LEN  8
#define SRR_AT_TARGET 3

#define SRR_SHELL_LEN   7     /* a full-range shell, in map squares */
#define SRR_SHELL_TICKS 80    /* more game ticks than any shell lives */

/* The rewind the player's shell carries in the tank and builder cases. */
#define SRR_COMP 6

/* The game tick, counted from 1, on which a shell from square 0 reaches a
   tank on the middle of square SRR_AT_TARGET: it starts 160 world units out
   and goes 32 a tick, the tank is 768 away and the hit circle is 112. */
#define SRR_HIT_TICK 16

/* World units between the builder's places on successive frames: more than
   two kill radii (2 x 128), so a blast on one frame's place cannot reach the
   next frame's. */
#define SRR_MAN_STEP 300

static int     srrDeaths;
static uint8_t srrDeathOutcome;

static void (*srrDeathPrev)(void *ctx, uint32_t fireTick,
                            uint32_t serverFireTick, BYTE owner,
                            WORLD impactWX, WORLD impactWY, uint8_t outcome);

static void srrDeathCb(void *ctx, uint32_t fireTick, uint32_t serverFireTick,
                       BYTE owner, WORLD impactWX, WORLD impactWY,
                       uint8_t outcome) {
    srrDeaths++;
    srrDeathOutcome = outcome;
    if (srrDeathPrev != NULL) {
        srrDeathPrev(ctx, fireTick, serverFireTick, owner, impactWX, impactWY,
                     outcome);
    }
}

static WORLD srrMiddle(BYTE m) {
    return (WORLD)(((WORLD)m << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* A run of squares with no pill, base or mine on or near it, laid to road,
   two rows either side. */
static bool srrLane(ServerSim *sim, BYTE *ox, BYTE *oy) {
    int x, y, dx, dy;

    for (y = 30; y < 220; y++) {
        for (x = 30; x < 200; x++) {
            bool clear = true;
            for (dy = -3; dy <= 3 && clear; dy++) {
                for (dx = -2; dx <= SRR_LANE_LEN + 2 && clear; dx++) {
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
                for (dx = -1; dx <= SRR_LANE_LEN; dx++) {
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
static void srrPlace(ServerSim *sim, BYTE slot, WORLD wx, WORLD wy) {
    tank *t = &sim->sim.tanks[slot];

    tankSetWorld(&sim->sim, t, wx, wy, (TURNTYPE)BRADIANS_EAST, FALSE);
    tankSetSpeed(t, 0);
    tankSetOnBoat(t, FALSE);
    tankClearBoatTrail(t);
    (*t)->bumpX = 0;
    (*t)->bumpY = 0;
}

/* Two seats in a running round and a clear lane. The shooter's tank stands
   two rows south of the lane's first square, out of every shell's way: a
   pill's shell is nobody's own and would be tested against it too. */
static ServerSim *srrSim(BYTE *lx, BYTE *ly) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);

    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, SRR_SHOOTER, "Shooter", false);
    serverSimAddPlayer(sim, SRR_TARGET, "Target", false);
    if (sim->sim.tanks[SRR_SHOOTER] == NULL ||
        sim->sim.tanks[SRR_TARGET] == NULL ||
        sim->sim.lgmen[SRR_TARGET] == NULL ||
        sim->sim.posHistoryPtr != sim->posHistory ||
        sim->sim.lgmPosHistoryPtr != sim->lgmPosHistory ||
        !srrLane(sim, lx, ly)) {
        serverSimDestroy(sim);
        return NULL;
    }
    srrPlace(sim, SRR_SHOOTER, srrMiddle(*lx), srrMiddle((BYTE)(*ly + 2)));
    posHistoryInit(&sim->posHistory[SRR_SHOOTER]);
    srrDeaths       = 0;
    srrDeathOutcome = 0;
    srrDeathPrev    = sim->sim.callbacks.shellDeath;
    sim->sim.callbacks.shellDeath = srrDeathCb;
    return sim;
}

static void srrEnd(ServerSim *sim) {
    sim->sim.callbacks.shellDeath = srrDeathPrev;
    srrDeathPrev = NULL;
    serverSimDestroy(sim);
}

/* Step every shell in flight until none is left. Returns the game tick,
   counted from 1, on which the shell-death callback was first told. */
static int srrFly(ServerSim *sim) {
    tank tanks[MAX_TANKS];
    lgm *lgms[MAX_TANKS];
    int  t;
    int  diedOn = 0;

    for (t = 1; t <= SRR_SHELL_TICKS && sim->sim.shs != NULL; t++) {
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
        if (diedOn == 0 && srrDeaths > 0) {
            diedOn = t;
        }
    }
    return diedOn;
}

/* The target tank's place on the frame k back, for the tank cases: on the
   lane on frame `onLane`, and on every other frame north of the lane by more
   than two hit radii, a little further each frame so no two are the same. */
static WORLD srrTankY(BYTE ly, int k, int onLane) {
    if (k == onLane) {
        return srrMiddle(ly);
    }
    return (WORLD)(srrMiddle(ly) - (2 * TANK_HIT_RADIUS + 60) - 10 * k);
}

/* Write the target tank's record, oldest frame first, and put the tank
   where the newest frame (0 back, the present) says it is. */
static void srrTankHistory(ServerSim *sim, BYTE lx, BYTE ly, int onLane) {
    PosHistory *h = &sim->posHistory[SRR_TARGET];
    WORLD       x = srrMiddle((BYTE)(lx + SRR_AT_TARGET));
    int         k;

    posHistoryInit(h);
    for (k = POSITION_HISTORY_SIZE - 1; k >= 0; k--) {
        posHistoryRecord(h, x, srrTankY(ly, k, onLane), true);
    }
    srrPlace(sim, SRR_TARGET, x, srrTankY(ly, 0, onLane));
}

/* One shell east along the lane from its first square. A player's shell has
   the shooter as owner and carries comp; a pill's shell has no owner, a pill
   number and carries nothing. */
static void srrFireLane(ServerSim *sim, BYTE lx, BYTE ly, bool fromPill,
                        uint8_t comp) {
    srrDeaths       = 0;
    srrDeathOutcome = 0;
    sim->sim.lagCompTicks = fromPill ? 0 : comp;
    shellsAddItem(&sim->sim, &sim->sim.shs, srrMiddle(lx), srrMiddle(ly),
                  (TURNTYPE)BRADIANS_EAST, (TURNTYPE)SRR_SHELL_LEN,
                  fromPill ? NEUTRAL : SRR_SHOOTER, NEUTRAL,
                  fromPill ? 0 : DMG_NO_PILL, FALSE);
    sim->sim.lagCompTicks = 0;
}

/* ================================================================
 * 1. A pill's shell does not rewind a tank.
 *
 * Guards shellsCalcCollision, the tank loop: the else branch that gives a
 * shell with compensationTicks 0 a rewind of 0 ("A pill's shell ... tests
 * the tank where it is now"). If a pill's shell is given a rewind there,
 * (a) hits the place the tank left and (b) misses the tank: both fail.
 * ================================================================ */
int run_shell_rewind_pill_shell_tank_present(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    GameSim   *gs;
    BYTE       armourBefore;
    WORLD      hx = 0, hy = 0;
    int        diedOn;
    int        rc = 0;

    /* (a) Every frame of the record but the present one has the tank on the
       lane. The tank is north of it now. A pill's shell down the lane goes
       past. */
    sim = srrSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    {
        PosHistory *h = &sim->posHistory[SRR_TARGET];
        WORLD x = srrMiddle((BYTE)(lx + SRR_AT_TARGET));
        WORLD offY = (WORLD)(srrMiddle(ly) - 2 * TANK_HIT_RADIUS);
        int   k;

        posHistoryInit(h);
        for (k = POSITION_HISTORY_SIZE - 1; k >= 1; k--) {
            posHistoryRecord(h, x, srrMiddle(ly), true);
        }
        posHistoryRecord(h, x, offY, true);
        srrPlace(sim, SRR_TARGET, x, offY);
    }
    /* Not vacuous: there is a record SRR_COMP back, and it is on the lane. */
    UT_CHECK(posHistoryGet(&sim->posHistory[SRR_TARGET], SRR_COMP, &hx, &hy));
    UT_CHECK(hy == srrMiddle(ly));
    armourBefore = tankGetArmour(&gs->tanks[SRR_TARGET]);
    srrFireLane(sim, lx, ly, true, 0);
    UT_CHECK(gs->shs != NULL && gs->shs->compensationTicks == 0 &&
             gs->shs->owner == NEUTRAL);
    diedOn = srrFly(sim);
    UT_CHECK_MSG(tankGetArmour(&gs->tanks[SRR_TARGET]) == armourBefore,
                 "a pill's shell hit the tank at the place its record has, "
                 "not where it is (shell ended on game tick %d)", diedOn);
    UT_CHECK(srrDeaths == 1 && srrDeathOutcome == SHELL_OUTCOME_EXPIRED);
    srrEnd(sim);

    /* (b) The other way round: the tank is on the lane now and every older
       frame has it north of the lane. The pill's shell hits it, on the tick
       a shell reaches a tank standing there. */
    sim = srrSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    srrTankHistory(sim, lx, ly, 0);
    UT_CHECK(posHistoryGet(&sim->posHistory[SRR_TARGET], SRR_COMP, &hx, &hy));
    UT_CHECK(hy != srrMiddle(ly));
    armourBefore = tankGetArmour(&gs->tanks[SRR_TARGET]);
    srrFireLane(sim, lx, ly, true, 0);
    diedOn = srrFly(sim);
    UT_CHECK_MSG(tankGetArmour(&gs->tanks[SRR_TARGET]) ==
                     (BYTE)(armourBefore - gs->rules.shell_damage),
                 "a pill's shell missed the tank where it is now");
    UT_CHECK(srrDeaths == 1 && srrDeathOutcome == SHELL_OUTCOME_TANK_HIT);
    UT_CHECK_MSG(diedOn == SRR_HIT_TICK, "hit on game tick %d, expected %d",
                 diedOn, SRR_HIT_TICK);
done:
    srrEnd(sim);

    return rc;
}

/* ================================================================
 * 2. A player's shell rewinds a tank by all of its compensationTicks.
 *
 * Guards shellsCalcCollision, the tank loop: "rewindTicks =
 * compensationTicks;" for a player's shell. The one-frame cut that the
 * builder test has (SHELL_LGM_REWIND_LESS) must not be made here. With the
 * cut made, the frame-N case below misses and the frame-N-1 case hits.
 * ================================================================ */
int run_shell_rewind_tank_full_comp(void) {
    static const struct {
        int  onLane;   /* the one frame, counted back, that is on the lane */
        bool hit;
    } cases[] = {
        { SRR_COMP,     true  },
        { SRR_COMP - 1, false },
        { SRR_COMP + 1, false },
    };
    BYTE lx = 0, ly = 0;
    int  c;
    int  rc = 0;

    for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
        ServerSim *sim = srrSim(&lx, &ly);
        GameSim   *gs;
        BYTE       armourBefore;
        int        diedOn;

        UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
        gs = &sim->sim;
        srrTankHistory(sim, lx, ly, cases[c].onLane);
        armourBefore = tankGetArmour(&gs->tanks[SRR_TARGET]);
        srrFireLane(sim, lx, ly, false, SRR_COMP);
        UT_CHECK(gs->shs != NULL && gs->shs->compensationTicks == SRR_COMP);
        diedOn = srrFly(sim);
        if (cases[c].hit) {
            UT_CHECK_MSG(tankGetArmour(&gs->tanks[SRR_TARGET]) ==
                             (BYTE)(armourBefore - gs->rules.shell_damage),
                         "a shell that rewinds %d did not hit the tank at "
                         "the frame %d back", SRR_COMP, cases[c].onLane);
            UT_CHECK(srrDeathOutcome == SHELL_OUTCOME_TANK_HIT);
            UT_CHECK(diedOn == SRR_HIT_TICK);
        } else {
            UT_CHECK_MSG(tankGetArmour(&gs->tanks[SRR_TARGET]) == armourBefore,
                         "a shell that rewinds %d hit the tank at the frame "
                         "%d back", SRR_COMP, cases[c].onLane);
            UT_CHECK(srrDeathOutcome == SHELL_OUTCOME_EXPIRED);
        }
done:
        srrEnd(sim);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* ================================================================
 * 3. A player's shell rewinds a builder by one frame less.
 *
 * Guards shellsBlastTestBuilder:
 *     if (rewindTicks > SHELL_LGM_REWIND_LESS) {
 *         rewindTicks -= SHELL_LGM_REWIND_LESS;
 *     }
 * Without the subtraction, the "N kills at frame N - 1" cases fail. With the
 * comparison loosened to >=, a shell that carries 1 rewinds 0 and the
 * "1 stays 1" cases fail.
 *
 * The builder's record has him SRR_MAN_STEP further east on every frame
 * back. The shell is given no life left and explodes where it is put, as a
 * shell at the end of its range does.
 * ================================================================ */
int run_shell_rewind_builder_one_less(void) {
    static const struct {
        uint8_t comp;      /* the rewind the shell carries */
        int     blastAt;   /* the frame, counted back, the blast is put on */
        bool    dies;
    } cases[] = {
        { SRR_COMP, SRR_COMP - 1, true  },   /* one less: the frame he is tested on */
        { SRR_COMP, SRR_COMP,     false },   /* not the full rewind */
        { SRR_COMP, SRR_COMP - 2, false },   /* and not two less */
        { 2,        1,            true  },
        { 2,        2,            false },
        { 1,        1,            true  },   /* 1 stays 1 */
        { 1,        0,            false },   /* never 0: not where he is now */
    };
    BYTE lx = 0, ly = 0;
    int  c;
    int  rc = 0;

    UT_ASSERT(SRR_MAN_STEP > 2 * MAP_SQUARE_MIDDLE);

    for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
        ServerSim  *sim = srrSim(&lx, &ly);
        GameSim    *gs;
        PosHistory *h;
        lgm         man;
        shells      q;
        tank        tanks[MAX_TANKS];
        lgm        *lgms[MAX_TANKS];
        BYTE        n = 0;
        BYTE        i;
        WORLD       x0, y0, bx;
        int         k;

        UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
        gs = &sim->sim;
        x0 = srrMiddle(lx);
        y0 = srrMiddle(ly);

        /* Both tanks off the lane; the blast is not on a tank. */
        srrPlace(sim, SRR_TARGET, srrMiddle((BYTE)(lx + SRR_LANE_LEN)),
                 srrMiddle((BYTE)(ly + 2)));

        /* The builder is out, at the place of frame 0. */
        man = gs->lgmen[SRR_TARGET];
        man->inTank = FALSE;
        man->isDead = FALSE;
        man->x = x0;
        man->y = y0;
        h = &sim->lgmPosHistory[SRR_TARGET];
        posHistoryInit(h);
        for (k = POSITION_HISTORY_SIZE - 1; k >= 0; k--) {
            posHistoryRecord(h, (WORLD)(x0 + SRR_MAN_STEP * k), y0, true);
        }

        bx = (WORLD)(x0 + SRR_MAN_STEP * cases[c].blastAt);
        /* The blast is on the lane's road, so it is not a blast on a wall. */
        UT_CHECK((bx >> TANK_SHIFT_MAPSIZE) <= (WORLD)(lx + SRR_LANE_LEN));

        srrDeaths       = 0;
        srrDeathOutcome = 0;
        shellsAddItem(gs, &gs->shs, bx, y0, (TURNTYPE)BRADIANS_EAST,
                      (TURNTYPE)SRR_SHELL_LEN, SRR_SHOOTER, NEUTRAL,
                      DMG_NO_PILL, FALSE);
        q = gs->shs;
        UT_CHECK(q != NULL);
        q->compensationTicks = cases[c].comp;
        q->x = bx;
        q->y = y0;
        q->xAcc = 0;
        q->yAcc = 0;
        q->length = SHELL_DEATH;

        for (i = 0; i < MAX_TANKS; i++) {
            if (sim->playerConnected[i] && gs->tanks[i] != NULL) {
                tanks[n] = gs->tanks[i];
                lgms[n]  = &gs->lgmen[i];
                n++;
            }
        }
        shellsUpdate(gs, tanks, n, lgms, &gs->ss);
        UT_CHECK_MSG(srrDeaths == 1 && srrDeathOutcome == SHELL_OUTCOME_EXPIRED,
                     "setup: the shell did not explode at the end of its "
                     "life (deaths %d, outcome %u)", srrDeaths,
                     (unsigned)srrDeathOutcome);

        if (cases[c].dies) {
            UT_CHECK_MSG(man->isDead == TRUE,
                         "a shell that carries a rewind of %u did not kill "
                         "the builder at his place %d frames back",
                         (unsigned)cases[c].comp, cases[c].blastAt);
        } else {
            UT_CHECK_MSG(man->isDead == FALSE,
                         "a shell that carries a rewind of %u killed the "
                         "builder at his place %d frames back",
                         (unsigned)cases[c].comp, cases[c].blastAt);
        }
done:
        srrEnd(sim);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}
