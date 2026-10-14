/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A script that moves a tank erases the tank's lag-compensation record
 * (test_teleport_clears_rewind.c).
 *
 * A player's shell tests a tank where it was on the shooter's screen: the
 * position history some entries back. The history holds 16 game ticks. A
 * scenario teleport (scenarioOpTankTeleport, server_sim_scenario.c) moves the
 * tank in one step, so every entry in the history is the place it left. With
 * those entries kept, a shell at the OLD place hits a tank that is no longer
 * there, and a shell at the NEW place goes through it, for as many ticks as
 * the shell rewinds. The teleport therefore erases the record. An empty
 * record means "nothing that old", and the shell test falls back to where
 * the tank is now.
 *
 *   run_teleport_erases_tank_rewind_record - after a teleport the record is
 *       empty; a rewound shell through the old place does not hit; with the
 *       erase taken out of the teleport, that shell does hit
 *   run_teleport_shell_hits_new_place      - a rewound shell at the new place
 *       hits at once after the teleport
 *   run_teleport_rewind_refills            - the tick fills the record again,
 *       one entry a game tick, and a rewound shell then hits the tank where
 *       the record of that frame says it was
 *   run_teleport_builder_rewind_record     - the builder's record is erased
 *       with the tank's when he is inside the tank, and kept when he is out
 *
 * A player who takes a seat gets empty records too. The records are kept by
 * seat, nothing writes to them while the seat is empty, and so the frames of
 * the player who left are still there when the next one sits down
 * (addPlayerInternal, server_sim_players.c, erases them).
 *
 *   run_slot_join_erases_tank_rewind_record    - after a leave and a join on
 *       the same seat the tank's record is empty, and a rewound shell through
 *       the place the old tank stood does not hit the new one
 *   run_slot_join_shell_hits_new_tank          - a rewound shell at the new
 *       tank hits at once after the join
 *   run_slot_join_rewind_refills               - the tick fills both records
 *       again, one entry a game tick, with the new tank's place only
 *   run_slot_join_erases_builder_rewind_record - the builder's record is
 *       erased on the join as well
 *
 * The shell cases step the shell list the way the tick steps it (shellsUpdate
 * over the compacted tank and builder arrays).
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
#include "server_sim_scenario.h"   /* serverSimApplyScenarioOp */
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

#define TCR_SHOOTER 0
#define TCR_TARGET  1

/* The lane: the shooter on square 0, the target three squares on. */
#define TCR_LANE_LEN  8
#define TCR_AT_TARGET 3

/* The row the target is teleported to: two squares south of the lane, 512
   world units, more than four hit radii from a shell on the lane. */
#define TCR_ROW_AWAY 2

/* A full-range tank shell, in map squares, and more game ticks than it
   lives. */
#define TCR_SHELL_LEN   7
#define TCR_SHELL_TICKS 80

/* History entries the shooter's shell rewinds its target by. */
#define TCR_COMP 6

/* The game tick, counted from 1, on which a shell from the shooter's square
   reaches a tank standing on the middle of square TCR_AT_TARGET: it starts
   160 world units out and goes 32 a tick, the tank is 768 away and the hit
   circle is 112. */
#define TCR_HIT_TICK 16

static int     tcrDeaths;
static uint8_t tcrDeathOutcome;

static void (*tcrDeathPrev)(void *ctx, uint32_t fireTick,
                            uint32_t serverFireTick, BYTE owner,
                            WORLD impactWX, WORLD impactWY, uint8_t outcome);

static void tcrDeathCb(void *ctx, uint32_t fireTick, uint32_t serverFireTick,
                       BYTE owner, WORLD impactWX, WORLD impactWY,
                       uint8_t outcome) {
    tcrDeaths++;
    tcrDeathOutcome = outcome;
    if (tcrDeathPrev != NULL) {
        tcrDeathPrev(ctx, fireTick, serverFireTick, owner, impactWX, impactWY,
                     outcome);
    }
}

static WORLD tcrMiddle(BYTE m) {
    return (WORLD)(((WORLD)m << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* A block of squares with no pill, base or mine on or near it, laid to road:
   the lane and the rows down to the one the target is teleported to. */
static bool tcrLane(ServerSim *sim, BYTE *ox, BYTE *oy) {
    int x, y, dx, dy;

    for (y = 30; y < 220; y++) {
        for (x = 30; x < 200; x++) {
            bool clear = true;
            for (dy = -3; dy <= TCR_ROW_AWAY + 3 && clear; dy++) {
                for (dx = -2; dx <= TCR_LANE_LEN + 2 && clear; dx++) {
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
            for (dy = -2; dy <= TCR_ROW_AWAY + 2; dy++) {
                for (dx = -1; dx <= TCR_LANE_LEN; dx++) {
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

/* A tank at rest, ashore, facing east, with its centre at wx, wy. Not a
   scripted move: the record is not touched. */
static void tcrPlace(ServerSim *sim, BYTE slot, WORLD wx, WORLD wy) {
    tank *t = &sim->sim.tanks[slot];

    tankSetWorld(&sim->sim, t, wx, wy, (TURNTYPE)BRADIANS_EAST, FALSE);
    tankSetSpeed(t, 0);
    tankSetOnBoat(t, FALSE);
    tankClearBoatTrail(t);
    (*t)->bumpX = 0;
    (*t)->bumpY = 0;
}

/* Fill a record with the one place, as a tank that stood there for 16 game
   ticks has it. */
static void tcrStoodAt(PosHistory *h, WORLD wx, WORLD wy) {
    int i;

    posHistoryInit(h);
    for (i = 0; i < POSITION_HISTORY_SIZE; i++) {
        posHistoryRecord(h, wx, wy, true);
    }
}

/* Two seats in a running round, a clear block of road, the shooter on the
   lane's first square, the target on the lane with a full record of standing
   there. */
static ServerSim *tcrSim(BYTE *lx, BYTE *ly) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);

    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, TCR_SHOOTER, "Shooter", false);
    serverSimAddPlayer(sim, TCR_TARGET, "Target", false);
    if (sim->sim.tanks[TCR_SHOOTER] == NULL ||
        sim->sim.tanks[TCR_TARGET] == NULL ||
        sim->sim.lgmen[TCR_TARGET] == NULL ||
        sim->sim.posHistoryPtr != sim->posHistory ||
        !tcrLane(sim, lx, ly)) {
        serverSimDestroy(sim);
        return NULL;
    }
    tcrPlace(sim, TCR_SHOOTER, tcrMiddle(*lx), tcrMiddle(*ly));
    tcrPlace(sim, TCR_TARGET, tcrMiddle((BYTE)(*lx + TCR_AT_TARGET)),
             tcrMiddle(*ly));
    tcrStoodAt(&sim->posHistory[TCR_TARGET],
               tcrMiddle((BYTE)(*lx + TCR_AT_TARGET)), tcrMiddle(*ly));
    tcrDeaths       = 0;
    tcrDeathOutcome = 0;
    tcrDeathPrev    = sim->sim.callbacks.shellDeath;
    sim->sim.callbacks.shellDeath = tcrDeathCb;
    return sim;
}

static void tcrEnd(ServerSim *sim) {
    sim->sim.callbacks.shellDeath = tcrDeathPrev;
    tcrDeathPrev = NULL;
    serverSimDestroy(sim);
}

/* The script's teleport of the target to a square. */
static ScnOpResult tcrTeleport(ServerSim *sim, BYTE mx, BYTE my) {
    ScenarioOp op;

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_TELEPORT;
    op.u.tankTeleport.slot = TCR_TARGET;
    op.u.tankTeleport.mode = SCN_TELEPORT_SQUARE;
    op.u.tankTeleport.x = mx;
    op.u.tankTeleport.y = my;
    op.u.tankTeleport.dir = SCN_NONE;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* One shell east from the middle of square (mx, my), owned by the shooter,
   that rewinds its target by comp history entries. */
static void tcrFire(ServerSim *sim, BYTE mx, BYTE my, uint8_t comp) {
    tcrDeaths       = 0;
    tcrDeathOutcome = 0;
    sim->sim.lagCompTicks = comp;
    shellsAddItem(&sim->sim, &sim->sim.shs, tcrMiddle(mx), tcrMiddle(my),
                  (TURNTYPE)BRADIANS_EAST, (TURNTYPE)TCR_SHELL_LEN, TCR_SHOOTER,
                  NEUTRAL, DMG_NO_PILL, FALSE);
    sim->sim.lagCompTicks = 0;
}

/* Step every shell in flight until none is left. Returns the game tick,
   counted from 1, on which the shell-death callback was first told. The
   position history is not written here, so "N entries back" is the same
   entry for the whole flight. */
static int tcrFly(ServerSim *sim) {
    tank tanks[MAX_TANKS];
    lgm *lgms[MAX_TANKS];
    int  t;
    int  diedOn = 0;

    for (t = 1; t <= TCR_SHELL_TICKS && sim->sim.shs != NULL; t++) {
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
        if (diedOn == 0 && tcrDeaths > 0) {
            diedOn = t;
        }
    }
    return diedOn;
}

/* ================================================================
 * 1. The teleport erases the record; a shell at the old place misses.
 *
 * Guards the posHistoryInit of the tank's record at the end of
 * scenarioOpTankTeleport. With that line taken out, the 16 entries still
 * say "on the lane", the shell hits on game tick 16 and this case fails at
 * the armour check.
 * ================================================================ */
int run_teleport_erases_tank_rewind_record(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tcrSim(&lx, &ly);
    GameSim   *gs;
    WORLD      wx = 0, wy = 0, hx = 0, hy = 0;
    BYTE       armourBefore;
    int        diedOn;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    /* Before the move the record is full and says "on the lane". */
    UT_ASSERT(sim->posHistory[TCR_TARGET].count == POSITION_HISTORY_SIZE);
    UT_ASSERT(posHistoryGet(&sim->posHistory[TCR_TARGET], TCR_COMP, &hx, &hy));
    UT_ASSERT(hx == tcrMiddle((BYTE)(lx + TCR_AT_TARGET)) && hy == tcrMiddle(ly));

    UT_ASSERT_MSG(tcrTeleport(sim, (BYTE)(lx + TCR_AT_TARGET),
                              (BYTE)(ly + TCR_ROW_AWAY)) == SCN_OP_OK,
                  "setup: the teleport was refused");
    tankGetWorld(&gs->tanks[TCR_TARGET], &wx, &wy);
    UT_ASSERT(wx == tcrMiddle((BYTE)(lx + TCR_AT_TARGET)) &&
              wy == tcrMiddle((BYTE)(ly + TCR_ROW_AWAY)));

    /* A rewound shell down the lane, through the place the tank left. */
    armourBefore = tankGetArmour(&gs->tanks[TCR_TARGET]);
    tcrFire(sim, lx, ly, TCR_COMP);
    diedOn = tcrFly(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[TCR_TARGET]) == armourBefore,
                  "a shell at the place the tank was teleported FROM hit it "
                  "(armour %d, was %d, shell ended on game tick %d)",
                  (int)tankGetArmour(&gs->tanks[TCR_TARGET]), (int)armourBefore,
                  diedOn);
    UT_ASSERT(tcrDeaths == 1 && tcrDeathOutcome == SHELL_OUTCOME_EXPIRED);
    UT_ASSERT(diedOn > TCR_HIT_TICK);

    /* The record holds nothing: no entry of any age. */
    UT_ASSERT_MSG(sim->posHistory[TCR_TARGET].count == 0,
                  "the tank's record still holds %d entries after the teleport",
                  (int)sim->posHistory[TCR_TARGET].count);
    UT_ASSERT(posHistoryLookup(&sim->posHistory[TCR_TARGET], 0, &hx, &hy) ==
              POS_HISTORY_NO_RECORD);
    UT_ASSERT(posHistoryLookup(&sim->posHistory[TCR_TARGET], TCR_COMP, &hx,
                               &hy) == POS_HISTORY_NO_RECORD);

    tcrEnd(sim);
    return 0;
}

/* ================================================================
 * 2. A shell at the new place hits right after the teleport.
 * ================================================================ */
int run_teleport_shell_hits_new_place(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tcrSim(&lx, &ly);
    GameSim   *gs;
    BYTE       armourBefore;
    int        diedOn;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    UT_ASSERT(tcrTeleport(sim, (BYTE)(lx + TCR_AT_TARGET),
                          (BYTE)(ly + TCR_ROW_AWAY)) == SCN_OP_OK);

    /* The shooter on the new row, the same three squares west of the tank.
       Its shell rewinds TCR_COMP entries; there is no entry that old, so the
       tank is tested where it is. */
    tcrPlace(sim, TCR_SHOOTER, tcrMiddle(lx), tcrMiddle((BYTE)(ly + TCR_ROW_AWAY)));
    armourBefore = tankGetArmour(&gs->tanks[TCR_TARGET]);
    tcrFire(sim, lx, (BYTE)(ly + TCR_ROW_AWAY), TCR_COMP);
    diedOn = tcrFly(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[TCR_TARGET]) ==
                      (BYTE)(armourBefore - gs->rules.shell_damage),
                  "a shell at the place the tank was teleported TO missed it "
                  "(armour %d, was %d)",
                  (int)tankGetArmour(&gs->tanks[TCR_TARGET]), (int)armourBefore);
    UT_ASSERT(tcrDeaths == 1 && tcrDeathOutcome == SHELL_OUTCOME_TANK_HIT);
    UT_ASSERT_MSG(diedOn == TCR_HIT_TICK, "hit on game tick %d, expected %d",
                  diedOn, TCR_HIT_TICK);

    tcrEnd(sim);
    return 0;
}

/* ================================================================
 * 3. The tick fills the record again and the rewind works again.
 * ================================================================ */
int run_teleport_rewind_refills(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tcrSim(&lx, &ly);
    GameSim   *gs;
    WORLD      newX, newY, hx = 0, hy = 0, wx = 0, wy = 0;
    BYTE       armourBefore;
    int        i, diedOn;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    UT_ASSERT(tcrTeleport(sim, (BYTE)(lx + TCR_AT_TARGET),
                          (BYTE)(ly + TCR_ROW_AWAY)) == SCN_OP_OK);
    newX = tcrMiddle((BYTE)(lx + TCR_AT_TARGET));
    newY = tcrMiddle((BYTE)(ly + TCR_ROW_AWAY));
    UT_ASSERT(sim->posHistory[TCR_TARGET].count == 0);

    /* The real tick: one entry a game tick. One serverSimTick call is one
       game tick (two server ticks). After TCR_COMP + 1 game ticks there is
       an entry TCR_COMP back. */
    for (i = 0; i < TCR_COMP + 1; i++) {
        serverSimTick(sim);
        if (i == TCR_COMP - 1) {
            /* One game tick short: entries 0 .. TCR_COMP - 1, none older. */
            UT_ASSERT_MSG(sim->posHistory[TCR_TARGET].count == TCR_COMP,
                          "%d entries after %d game ticks, expected %d",
                          (int)sim->posHistory[TCR_TARGET].count, TCR_COMP,
                          TCR_COMP);
            UT_ASSERT(posHistoryLookup(&sim->posHistory[TCR_TARGET], TCR_COMP,
                                       &hx, &hy) == POS_HISTORY_NO_RECORD);
        }
    }
    UT_ASSERT_MSG(sim->posHistory[TCR_TARGET].count == TCR_COMP + 1,
                  "%d entries after %d game ticks, expected %d",
                  (int)sim->posHistory[TCR_TARGET].count, TCR_COMP + 1,
                  TCR_COMP + 1);
    tankGetWorld(&gs->tanks[TCR_TARGET], &wx, &wy);
    UT_ASSERT_MSG(wx == newX && wy == newY,
                  "setup: the tank did not stay where it was put (%u,%u)",
                  (unsigned)wx, (unsigned)wy);
    /* Every entry is the new place; none is the place before the teleport. */
    for (i = 0; i <= TCR_COMP; i++) {
        UT_ASSERT(posHistoryLookup(&sim->posHistory[TCR_TARGET], (uint8_t)i,
                                   &hx, &hy) == POS_HISTORY_PRESENT);
        UT_ASSERT_MSG(hx == newX && hy == newY,
                      "entry %d back is (%u,%u), not the new place (%u,%u)", i,
                      (unsigned)hx, (unsigned)hy, (unsigned)newX,
                      (unsigned)newY);
    }

    /* The rewind works again. The tank drives off the row (an ordinary move,
       which leaves the record alone); a shell along the row that rewinds
       TCR_COMP entries hits it where that entry says it was. */
    tcrPlace(sim, TCR_TARGET, newX, (WORLD)(newY + 2 * TANK_HIT_RADIUS));
    tcrPlace(sim, TCR_SHOOTER, tcrMiddle(lx), newY);
    armourBefore = tankGetArmour(&gs->tanks[TCR_TARGET]);
    tcrFire(sim, lx, (BYTE)(ly + TCR_ROW_AWAY), TCR_COMP);
    diedOn = tcrFly(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[TCR_TARGET]) ==
                      (BYTE)(armourBefore - gs->rules.shell_damage),
                  "with the record full again a rewound shell did not hit the "
                  "tank where the record has it");
    UT_ASSERT(tcrDeathOutcome == SHELL_OUTCOME_TANK_HIT);
    UT_ASSERT(diedOn == TCR_HIT_TICK);

    tcrEnd(sim);
    return 0;
}

/* ================================================================
 * 4. The builder's record: erased with the tank's when he is inside it,
 *    kept when he is out.
 * ================================================================ */
int run_teleport_builder_rewind_record(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    GameSim   *gs;
    WORLD      manX, manY, hx = 0, hy = 0;

    /* (a) Inside the tank. His record has him out beside the old place a
       moment ago. He goes with the tank, so the record goes. */
    sim = tcrSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    UT_ASSERT(gs->lgmen[TCR_TARGET]->inTank == TRUE);
    manX = (WORLD)(tcrMiddle((BYTE)(lx + TCR_AT_TARGET)) + 16);
    manY = tcrMiddle(ly);
    tcrStoodAt(&sim->lgmPosHistory[TCR_TARGET], manX, manY);
    UT_ASSERT(tcrTeleport(sim, (BYTE)(lx + TCR_AT_TARGET),
                          (BYTE)(ly + TCR_ROW_AWAY)) == SCN_OP_OK);
    UT_ASSERT_MSG(sim->lgmPosHistory[TCR_TARGET].count == 0,
                  "the record of a builder inside the teleported tank still "
                  "holds %d entries",
                  (int)sim->lgmPosHistory[TCR_TARGET].count);
    tcrEnd(sim);

    /* (b) Out of the tank. The tank is moved; he is not. His record is still
       true and is kept, entry for entry. */
    sim = tcrSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    manX = (WORLD)(tcrMiddle((BYTE)(lx + TCR_AT_TARGET)) + 16);
    manY = tcrMiddle(ly);
    gs->lgmen[TCR_TARGET]->inTank = FALSE;
    gs->lgmen[TCR_TARGET]->isDead = FALSE;
    gs->lgmen[TCR_TARGET]->x = manX;
    gs->lgmen[TCR_TARGET]->y = manY;
    tcrStoodAt(&sim->lgmPosHistory[TCR_TARGET], manX, manY);
    UT_ASSERT(tcrTeleport(sim, (BYTE)(lx + TCR_AT_TARGET),
                          (BYTE)(ly + TCR_ROW_AWAY)) == SCN_OP_OK);
    UT_ASSERT(sim->posHistory[TCR_TARGET].count == 0);
    UT_ASSERT_MSG(sim->lgmPosHistory[TCR_TARGET].count == POSITION_HISTORY_SIZE,
                  "the record of a builder who is out was erased (%d entries)",
                  (int)sim->lgmPosHistory[TCR_TARGET].count);
    UT_ASSERT(posHistoryLookup(&sim->lgmPosHistory[TCR_TARGET], TCR_COMP, &hx,
                               &hy) == POS_HISTORY_PRESENT);
    UT_ASSERT(hx == manX && hy == manY);
    UT_ASSERT(gs->lgmen[TCR_TARGET]->x == manX && gs->lgmen[TCR_TARGET]->y == manY);
    tcrEnd(sim);

    return 0;
}

/* The target leaves and another player takes its seat. The new tank is put
   on the row away from the lane, at rest; the old tank's record said "on the
   lane". FALSE when the seat did not come back with a tank and a builder. */
static bool tcrReseat(ServerSim *sim, BYTE lx, BYTE ly) {
    serverSimRemovePlayer(sim, TCR_TARGET);
    if (sim->playerConnected[TCR_TARGET] || sim->sim.tanks[TCR_TARGET] != NULL) {
        return false;
    }
    serverSimAddPlayer(sim, TCR_TARGET, "Newcomer", false);
    if (!sim->playerConnected[TCR_TARGET] ||
        sim->sim.tanks[TCR_TARGET] == NULL ||
        sim->sim.lgmen[TCR_TARGET] == NULL) {
        return false;
    }
    tcrPlace(sim, TCR_TARGET, tcrMiddle((BYTE)(lx + TCR_AT_TARGET)),
             tcrMiddle((BYTE)(ly + TCR_ROW_AWAY)));
    return true;
}

/* ================================================================
 * 5. A join erases the seat's tank record; a shell at the place the last
 *    occupant's tank stood misses the new tank.
 *
 * Guards the posHistoryInit of the tank's record in addPlayerInternal. With
 * that line taken out, the 16 entries still say "on the lane", the shell
 * hits the new tank on game tick 16 and this case fails at the armour check.
 * ================================================================ */
int run_slot_join_erases_tank_rewind_record(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tcrSim(&lx, &ly);
    GameSim   *gs;
    WORLD      hx = 0, hy = 0;
    BYTE       armourBefore;
    int        diedOn;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    /* The player who is about to leave: a full record of standing on the
       lane. */
    UT_ASSERT(sim->posHistory[TCR_TARGET].count == POSITION_HISTORY_SIZE);
    UT_ASSERT(posHistoryGet(&sim->posHistory[TCR_TARGET], TCR_COMP, &hx, &hy));
    UT_ASSERT(hx == tcrMiddle((BYTE)(lx + TCR_AT_TARGET)) && hy == tcrMiddle(ly));

    UT_ASSERT_MSG(tcrReseat(sim, lx, ly),
                  "setup: the seat did not empty and fill again");

    /* A rewound shell down the lane, through the place the old tank stood.
       The new tank is two squares off it. */
    armourBefore = tankGetArmour(&gs->tanks[TCR_TARGET]);
    tcrFire(sim, lx, ly, TCR_COMP);
    diedOn = tcrFly(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[TCR_TARGET]) == armourBefore,
                  "a shell at the place the LAST occupant's tank stood hit the "
                  "new one (armour %d, was %d, shell ended on game tick %d)",
                  (int)tankGetArmour(&gs->tanks[TCR_TARGET]), (int)armourBefore,
                  diedOn);
    UT_ASSERT(tcrDeaths == 1 && tcrDeathOutcome == SHELL_OUTCOME_EXPIRED);
    UT_ASSERT(diedOn > TCR_HIT_TICK);

    /* The record holds nothing: no entry of any age. */
    UT_ASSERT_MSG(sim->posHistory[TCR_TARGET].count == 0,
                  "the seat's tank record still holds %d entries after the join",
                  (int)sim->posHistory[TCR_TARGET].count);
    UT_ASSERT(posHistoryLookup(&sim->posHistory[TCR_TARGET], 0, &hx, &hy) ==
              POS_HISTORY_NO_RECORD);
    UT_ASSERT(posHistoryLookup(&sim->posHistory[TCR_TARGET], TCR_COMP, &hx,
                               &hy) == POS_HISTORY_NO_RECORD);

    tcrEnd(sim);
    return 0;
}

/* ================================================================
 * 6. A shell at the new tank hits right after the join.
 *
 * With the erase taken out of addPlayerInternal the shell is tested against
 * the lane, two squares from the row it flies along, and goes through the
 * new tank.
 * ================================================================ */
int run_slot_join_shell_hits_new_tank(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tcrSim(&lx, &ly);
    GameSim   *gs;
    BYTE       armourBefore;
    int        diedOn;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    UT_ASSERT_MSG(tcrReseat(sim, lx, ly),
                  "setup: the seat did not empty and fill again");

    /* The shooter on the new tank's row, three squares west of it. Its shell
       rewinds TCR_COMP entries; there is no entry that old, so the tank is
       tested where it is. */
    tcrPlace(sim, TCR_SHOOTER, tcrMiddle(lx), tcrMiddle((BYTE)(ly + TCR_ROW_AWAY)));
    armourBefore = tankGetArmour(&gs->tanks[TCR_TARGET]);
    tcrFire(sim, lx, (BYTE)(ly + TCR_ROW_AWAY), TCR_COMP);
    diedOn = tcrFly(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[TCR_TARGET]) ==
                      (BYTE)(armourBefore - gs->rules.shell_damage),
                  "a shell at the tank of the player who just joined missed it "
                  "(armour %d, was %d)",
                  (int)tankGetArmour(&gs->tanks[TCR_TARGET]), (int)armourBefore);
    UT_ASSERT(tcrDeaths == 1 && tcrDeathOutcome == SHELL_OUTCOME_TANK_HIT);
    UT_ASSERT_MSG(diedOn == TCR_HIT_TICK, "hit on game tick %d, expected %d",
                  diedOn, TCR_HIT_TICK);

    tcrEnd(sim);
    return 0;
}

/* ================================================================
 * 7. The tick fills the seat's records again after the join, with the new
 *    occupant's frames only.
 * ================================================================ */
int run_slot_join_rewind_refills(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tcrSim(&lx, &ly);
    GameSim   *gs;
    WORLD      newX, newY, hx = 0, hy = 0, wx = 0, wy = 0;
    BYTE       armourBefore;
    int        i, diedOn;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    UT_ASSERT_MSG(tcrReseat(sim, lx, ly),
                  "setup: the seat did not empty and fill again");
    newX = tcrMiddle((BYTE)(lx + TCR_AT_TARGET));
    newY = tcrMiddle((BYTE)(ly + TCR_ROW_AWAY));

    /* The real tick: one entry a game tick in each record. */
    for (i = 0; i < TCR_COMP + 1; i++) {
        serverSimTick(sim);
        if (i == TCR_COMP - 1) {
            /* One game tick short: entries 0 .. TCR_COMP - 1, none older. */
            UT_ASSERT_MSG(sim->posHistory[TCR_TARGET].count == TCR_COMP,
                          "%d entries after %d game ticks, expected %d",
                          (int)sim->posHistory[TCR_TARGET].count, TCR_COMP,
                          TCR_COMP);
            UT_ASSERT(posHistoryLookup(&sim->posHistory[TCR_TARGET], TCR_COMP,
                                       &hx, &hy) == POS_HISTORY_NO_RECORD);
        }
    }
    UT_ASSERT_MSG(sim->posHistory[TCR_TARGET].count == TCR_COMP + 1,
                  "%d tank entries after %d game ticks, expected %d",
                  (int)sim->posHistory[TCR_TARGET].count, TCR_COMP + 1,
                  TCR_COMP + 1);
    UT_ASSERT_MSG(sim->lgmPosHistory[TCR_TARGET].count == TCR_COMP + 1,
                  "%d builder entries after %d game ticks, expected %d",
                  (int)sim->lgmPosHistory[TCR_TARGET].count, TCR_COMP + 1,
                  TCR_COMP + 1);
    tankGetWorld(&gs->tanks[TCR_TARGET], &wx, &wy);
    UT_ASSERT_MSG(wx == newX && wy == newY,
                  "setup: the tank did not stay where it was put (%u,%u)",
                  (unsigned)wx, (unsigned)wy);
    /* Every entry is the new tank's place; none is the old tank's. */
    for (i = 0; i <= TCR_COMP; i++) {
        UT_ASSERT(posHistoryLookup(&sim->posHistory[TCR_TARGET], (uint8_t)i,
                                   &hx, &hy) == POS_HISTORY_PRESENT);
        UT_ASSERT_MSG(hx == newX && hy == newY,
                      "entry %d back is (%u,%u), not the new tank's place "
                      "(%u,%u)", i, (unsigned)hx, (unsigned)hy, (unsigned)newX,
                      (unsigned)newY);
    }

    /* The rewind works again. The tank drives off the row; a shell along the
       row that rewinds TCR_COMP entries hits it where that entry says it
       was. */
    tcrPlace(sim, TCR_TARGET, newX, (WORLD)(newY + 2 * TANK_HIT_RADIUS));
    tcrPlace(sim, TCR_SHOOTER, tcrMiddle(lx), newY);
    armourBefore = tankGetArmour(&gs->tanks[TCR_TARGET]);
    tcrFire(sim, lx, (BYTE)(ly + TCR_ROW_AWAY), TCR_COMP);
    diedOn = tcrFly(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[TCR_TARGET]) ==
                      (BYTE)(armourBefore - gs->rules.shell_damage),
                  "with the record full again a rewound shell did not hit the "
                  "tank where the record has it");
    UT_ASSERT(tcrDeathOutcome == SHELL_OUTCOME_TANK_HIT);
    UT_ASSERT(diedOn == TCR_HIT_TICK);

    tcrEnd(sim);
    return 0;
}

/* ================================================================
 * 8. A join erases the seat's builder record.
 *
 * Guards the posHistoryInit of the builder's record in addPlayerInternal.
 * ================================================================ */
int run_slot_join_erases_builder_rewind_record(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tcrSim(&lx, &ly);
    GameSim   *gs;
    WORLD      manX, manY, hx = 0, hy = 0;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    /* The player who is about to leave has his builder out beside the lane,
       with a full record of standing there. */
    manX = (WORLD)(tcrMiddle((BYTE)(lx + TCR_AT_TARGET)) + 16);
    manY = tcrMiddle(ly);
    gs->lgmen[TCR_TARGET]->inTank = FALSE;
    gs->lgmen[TCR_TARGET]->isDead = FALSE;
    gs->lgmen[TCR_TARGET]->x = manX;
    gs->lgmen[TCR_TARGET]->y = manY;
    tcrStoodAt(&sim->lgmPosHistory[TCR_TARGET], manX, manY);
    UT_ASSERT(posHistoryLookup(&sim->lgmPosHistory[TCR_TARGET], TCR_COMP, &hx,
                               &hy) == POS_HISTORY_PRESENT);

    UT_ASSERT_MSG(tcrReseat(sim, lx, ly),
                  "setup: the seat did not empty and fill again");

    /* The new builder is inside the new tank and has no frame of any age: a
       blast looks for him where he is now, not beside the lane. */
    UT_ASSERT(gs->lgmen[TCR_TARGET]->inTank == TRUE);
    UT_ASSERT_MSG(sim->lgmPosHistory[TCR_TARGET].count == 0,
                  "the seat's builder record still holds %d entries after the "
                  "join",
                  (int)sim->lgmPosHistory[TCR_TARGET].count);
    UT_ASSERT(posHistoryLookup(&sim->lgmPosHistory[TCR_TARGET], 0, &hx, &hy) ==
              POS_HISTORY_NO_RECORD);
    UT_ASSERT(posHistoryLookup(&sim->lgmPosHistory[TCR_TARGET], TCR_COMP, &hx,
                               &hy) == POS_HISTORY_NO_RECORD);

    tcrEnd(sim);
    return 0;
}
