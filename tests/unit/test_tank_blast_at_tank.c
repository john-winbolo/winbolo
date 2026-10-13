/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A shell that hits a tank blows up ON THE TANK, where the tank is now
 * (test_tank_blast_at_tank.c).
 *
 * The server tests a shell against where its target was on the shooter's
 * screen: the target's position some history entries ago (lag compensation,
 * shellsCalcCollision). That decision is not what these cases change. What
 * they pin is where the effects of a hit go. They used to start at the shell,
 * which is at the OLD place the tank has since left. They now start at the
 * point on the hull the shell reached, carried to where the tank is now: the
 * tank's present position plus the shell's offset from the position the hit
 * was tested against. The hit is decided on the shooter's screen, but the
 * blast belongs to the tank: one client's lag must not move an explosion away
 * from where the tank is for everybody.
 *
 * Which side of the tank the shell landed on still counts. A builder on that
 * side is caught and a builder on the other side is not, and a hit that was
 * not rewound is exactly where the shell is, as it always was.
 *
 *   run_tank_blast_zero_rewind_unchanged       - with no rewind the blast is
 *       the shell's own point, 96 world units short of the tank's centre in
 *       this lane, for a hit and for a kill; a builder on the hit side dies
 *       and one on the far side lives
 *   run_tank_blast_rewound_hit_at_present_tank - a lag-compensated hit puts
 *       the hit explosion, the hit sound and the shell-death position at the
 *       present tank plus the shell's offset from the rewound record
 *   run_tank_blast_kill_starts_at_present_tank - a killing hit starts the
 *       dead tank's moving explosion at that same point
 *   run_tank_blast_hit_decision_unchanged      - hit or miss is still decided
 *       against the rewound record, on the same tick as before: a tank whose
 *       record is on the lane is hit though it has left the lane, and a tank
 *       that stands on the lane now is missed when its record is off it
 *   run_tank_blast_builder_at_present_tank     - after a rewound hit, a
 *       builder beside the present tank on the hit side dies, one on the far
 *       side lives, and one where the tank used to be lives
 *
 * Every case steps the shell list the way the tick steps it (shellsUpdate over
 * the compacted tank and builder arrays) and writes the position history
 * itself, so the rewound and present positions are exact and known.
 *
 * Reads the ServerSim, tank, lgm and shell structs directly; the unittests
 * profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, posHistory, events[] */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "input_packet.h"          /* EVENT_EXPLOSION, EVENT_SOUND_TANK_HIT */
#include "game_sim.h"
#include "bases.h"
#include "bolo_map.h"
#include "lgm.h"
#include "mines.h"
#include "pillbox.h"
#include "position_history.h"
#include "shells.h"
#include "tank.h"
#include "tankexp.h"
#include "everard_map.h"
#include "test_harness.h"

#define TBT_SHOOTER 0
#define TBT_TARGET  1

/* The lane: the shooter on square 0, the target's lane square three on. */
#define TBT_LANE_LEN  8
#define TBT_AT_TARGET 3

/* A full-range tank shell, in map squares. */
#define TBT_SHELL_LEN 7

/* More game ticks than any shell lives. */
#define TBT_SHELL_TICKS 80

/* History entries the shooter's shell rewinds its target by: a 120 ms ping
   with the interpolation delay is 4 or 5, and 6 keeps clear of both ends. */
#define TBT_COMP 6

/* How far the target has moved off the lane since the rewound record: two
   hit radii, so a shell on the lane cannot reach the present tank at all. */
#define TBT_MOVED (2 * TANK_HIT_RADIUS)

/* The game tick, counted from 1, on which the shell reaches a tank standing
   on the middle of square TBT_AT_TARGET. The shell starts SHELL_START_ADD
   steps out from the shooter (160 world units) and goes 32 a tick; the tank
   is 768 away and the hit circle is 112, so it is inside after 16 moves, at
   672, which is 96 short of the tank's centre. */
#define TBT_HIT_TICK        16
#define TBT_SHELL_STOP_SHORT 96

/* Builders beside the hit tank, in world units along the lane from the
   tank's centre. A blast kills a builder on open ground within half a square
   (MAP_SQUARE_MIDDLE, 128) of it, and the blast is TBT_SHELL_STOP_SHORT west
   of the centre. The near builder is 196 west of the centre, 100 from the
   blast: he dies. The far builder is 48 east of the centre, 144 from the
   blast: he lives. He is only 48 from the centre, so a blast at the centre
   would kill him. */
#define TBT_NEAR_WEST 196
#define TBT_FAR_EAST  48

/* What the shell-death callback was told. */
static int     tbtDeaths;
static WORLD   tbtDeathX;
static WORLD   tbtDeathY;
static uint8_t tbtDeathOutcome;

static void (*tbtDeathPrev)(void *ctx, uint32_t fireTick,
                            uint32_t serverFireTick, BYTE owner,
                            WORLD impactWX, WORLD impactWY, uint8_t outcome);

static void tbtDeathCb(void *ctx, uint32_t fireTick, uint32_t serverFireTick,
                       BYTE owner, WORLD impactWX, WORLD impactWY,
                       uint8_t outcome) {
    tbtDeaths++;
    tbtDeathX       = impactWX;
    tbtDeathY       = impactWY;
    tbtDeathOutcome = outcome;
    if (tbtDeathPrev != NULL) {
        tbtDeathPrev(ctx, fireTick, serverFireTick, owner, impactWX, impactWY,
                     outcome);
    }
}

static WORLD tbtMiddle(BYTE m) {
    return (WORLD)(((WORLD)m << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* A run of squares with nothing on it and room around it, laid to road, two
   rows either side so the target has somewhere to have moved to. */
static bool tbtLane(ServerSim *sim, BYTE *ox, BYTE *oy) {
    int x, y, dx, dy;

    for (y = 30; y < 220; y++) {
        for (x = 30; x < 200; x++) {
            bool clear = true;
            for (dy = -3; dy <= 3 && clear; dy++) {
                for (dx = -2; dx <= TBT_LANE_LEN + 2 && clear; dx++) {
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
                for (dx = -1; dx <= TBT_LANE_LEN; dx++) {
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
static void tbtPlace(ServerSim *sim, BYTE slot, WORLD wx, WORLD wy) {
    tank *t = &sim->sim.tanks[slot];

    tankSetWorld(&sim->sim, t, wx, wy, (TURNTYPE)BRADIANS_EAST, FALSE);
    tankSetSpeed(t, 0);
    tankSetOnBoat(t, FALSE);
    tankClearBoatTrail(t);
    (*t)->bumpX = 0;
    (*t)->bumpY = 0;
}

/* Write a history in which the item stood at (oldX, oldY) up to TBT_COMP
   entries ago and has gone in a straight line to (nowX, nowY) since. The
   newest entry is the present, as it is in the tick, which records before it
   steps the shells. */
static void tbtHistory(PosHistory *h, WORLD oldX, WORLD oldY,
                       WORLD nowX, WORLD nowY) {
    int i;

    posHistoryInit(h);
    for (i = 0; i < POSITION_HISTORY_SIZE; i++) {
        posHistoryRecord(h, oldX, oldY, true);
    }
    for (i = 1; i <= TBT_COMP; i++) {
        int x = (int)oldX + (((int)nowX - (int)oldX) * i) / TBT_COMP;
        int y = (int)oldY + (((int)nowY - (int)oldY) * i) / TBT_COMP;
        posHistoryRecord(h, (WORLD)x, (WORLD)y, true);
    }
}

/* One round with the two seats in, a clear lane, the shooter on its first
   square and the shell-death callback recorded. */
static ServerSim *tbtSim(BYTE *lx, BYTE *ly) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);

    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, TBT_SHOOTER, "Shooter", false);
    serverSimAddPlayer(sim, TBT_TARGET, "Target", false);
    if (sim->sim.tanks[TBT_SHOOTER] == NULL ||
        sim->sim.tanks[TBT_TARGET] == NULL ||
        sim->sim.posHistoryPtr != sim->posHistory ||
        !tbtLane(sim, lx, ly)) {
        serverSimDestroy(sim);
        return NULL;
    }
    tbtPlace(sim, TBT_SHOOTER, tbtMiddle(*lx), tbtMiddle(*ly));
    tbtDeaths       = 0;
    tbtDeathX       = 0;
    tbtDeathY       = 0;
    tbtDeathOutcome = 0;
    tbtDeathPrev    = sim->sim.callbacks.shellDeath;
    sim->sim.callbacks.shellDeath = tbtDeathCb;
    sim->eventCount = 0;
    return sim;
}

static void tbtEnd(ServerSim *sim) {
    sim->sim.callbacks.shellDeath = tbtDeathPrev;
    tbtDeathPrev = NULL;
    serverSimDestroy(sim);
}

/* One shell from the shooter, east along the lane, that rewinds its target by
   comp history entries, as a player's shell fired at that view age does. */
static void tbtFire(ServerSim *sim, BYTE lx, BYTE ly, uint8_t comp) {
    sim->sim.lagCompTicks = comp;
    shellsAddItem(&sim->sim, &sim->sim.shs, tbtMiddle(lx), tbtMiddle(ly),
                  (TURNTYPE)BRADIANS_EAST, (TURNTYPE)TBT_SHELL_LEN, TBT_SHOOTER,
                  NEUTRAL, DMG_NO_PILL, FALSE);
    sim->sim.lagCompTicks = 0;
}

/* Step every shell in flight until none is left. Returns the game tick,
   counted from 1, on which the shell-death callback was first told. */
static int tbtFly(ServerSim *sim) {
    tank tanks[MAX_TANKS];
    lgm *lgms[MAX_TANKS];
    int  t;
    int  diedOn = 0;

    for (t = 1; t <= TBT_SHELL_TICKS && sim->sim.shs != NULL; t++) {
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
        if (diedOn == 0 && tbtDeaths > 0) {
            diedOn = t;
        }
    }
    return diedOn;
}

/* The only event of this type in the tick's buffer, or NULL for none or for
   more than one. */
static const GameEvent *tbtOnlyEvent(ServerSim *sim, uint8_t type) {
    const GameEvent *found = NULL;
    int              i;

    for (i = 0; i < sim->eventCount; i++) {
        if (sim->events[i].type == type) {
            if (found != NULL) {
                return NULL;
            }
            found = &sim->events[i];
        }
    }
    return found;
}

/* Whether an EVENT_EXPLOSION is the one a blast centred on wx, wy sends: the
   square and the pixel inside it of the blast's top left corner, half a
   square up and left of the centre. One pixel is 16 world units. */
static bool tbtExplosionAt(const GameEvent *ev, WORLD wx, WORLD wy) {
    WORLD cx = (WORLD)(wx - TANK_SUBTRACT);
    WORLD cy = (WORLD)(wy - TANK_SUBTRACT);

    return ev != NULL &&
           ev->data[0] == (BYTE)(cx >> TANK_SHIFT_MAPSIZE) &&
           ev->data[1] == (BYTE)(cy >> TANK_SHIFT_MAPSIZE) &&
           ev->data[2] == (BYTE)((cx & 0xFF) >> 4) &&
           ev->data[3] == (BYTE)((cy & 0xFF) >> 4);
}

/* Stand a seat's builder on the ground at wx, wy, out of the tank and alive. */
static void tbtBuilderOut(ServerSim *sim, BYTE slot, WORLD wx, WORLD wy) {
    lgm *l = &sim->sim.lgmen[slot];

    (*l)->inTank = FALSE;
    (*l)->isDead = FALSE;
    (*l)->x      = wx;
    (*l)->y      = wy;
}

/* The hit effects a shell left behind, checked against one blast point:
   the shell death, the one hit explosion, and the hit sound on the blast's
   square naming the target. */
static int tbtCheckBlast(ServerSim *sim, WORLD bx, WORLD by, uint8_t outcome) {
    const GameEvent *ev;

    UT_ASSERT(tbtDeaths == 1 && tbtDeathOutcome == outcome);
    UT_ASSERT_MSG(tbtDeathX == bx && tbtDeathY == by,
                  "shell death at (%u,%u), expected the blast point (%u,%u)",
                  (unsigned)tbtDeathX, (unsigned)tbtDeathY, (unsigned)bx,
                  (unsigned)by);
    ev = tbtOnlyEvent(sim, EVENT_EXPLOSION);
    UT_ASSERT_MSG(ev != NULL, "expected exactly one hit explosion");
    UT_ASSERT_MSG(tbtExplosionAt(ev, bx, by),
                  "hit explosion at square (%d,%d) pixel (%d,%d), not centred "
                  "on the blast point (%u,%u)",
                  (int)ev->data[0], (int)ev->data[1], (int)ev->data[2],
                  (int)ev->data[3], (unsigned)bx, (unsigned)by);
    ev = tbtOnlyEvent(sim, EVENT_SOUND_TANK_HIT);
    UT_ASSERT_MSG(ev != NULL, "expected exactly one tank-hit sound");
    UT_ASSERT_MSG(ev->data[1] == (BYTE)(bx >> TANK_SHIFT_MAPSIZE) &&
                      ev->data[2] == (BYTE)(by >> TANK_SHIFT_MAPSIZE),
                  "hit sound on square (%d,%d); the blast is on (%d,%d)",
                  (int)ev->data[1], (int)ev->data[2],
                  (int)(bx >> TANK_SHIFT_MAPSIZE),
                  (int)(by >> TANK_SHIFT_MAPSIZE));
    UT_ASSERT(ev->data[3] == TBT_TARGET);
    return 0;
}

/* The dead tank's moving explosion, as the server holds it and as it is
   sent, checked against the blast point. */
static int tbtCheckTankExplosion(ServerSim *sim, WORLD bx, WORLD by) {
    GameSim         *gs = &sim->sim;
    const GameEvent *ev;
    WORLD            tkX, tkY;

    UT_ASSERT_MSG(gs->tankExplosions != NULL, "no tank explosion was started");
    UT_ASSERT_MSG(gs->tankExplosions->x == bx && gs->tankExplosions->y == by,
                  "tank explosion starts at (%u,%u), expected the blast point "
                  "(%u,%u)",
                  (unsigned)gs->tankExplosions->x,
                  (unsigned)gs->tankExplosions->y, (unsigned)bx, (unsigned)by);
    UT_ASSERT(gs->tankExplosions->creator == TBT_TARGET);
    ev = tbtOnlyEvent(sim, EVENT_TK_EXPLOSION);
    UT_ASSERT_MSG(ev != NULL, "expected exactly one tank explosion event");
    tkX = (WORLD)(((WORLD)ev->data[0] << 8) | ev->data[1]);
    tkY = (WORLD)(((WORLD)ev->data[2] << 8) | ev->data[3]);
    UT_ASSERT_MSG(tkX == bx && tkY == by,
                  "tank explosion event at (%u,%u), blast point (%u,%u)",
                  (unsigned)tkX, (unsigned)tkY, (unsigned)bx, (unsigned)by);
    return 0;
}

/* ================================================================
 * 1. No rewind: everything is where the shell is, as it always was.
 * ================================================================ */
int run_tank_blast_zero_rewind_unchanged(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tbtSim(&lx, &ly);
    GameSim   *gs;
    WORLD      tx, ty, bx, by;
    BYTE       armourBefore;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    UT_ASSERT(gs->lgmen[TBT_SHOOTER] != NULL && gs->lgmen[TBT_TARGET] != NULL);
    UT_ASSERT(TBT_NEAR_WEST - TBT_SHELL_STOP_SHORT <= MAP_SQUARE_MIDDLE);
    UT_ASSERT(TBT_FAR_EAST + TBT_SHELL_STOP_SHORT > MAP_SQUARE_MIDDLE);
    UT_ASSERT(TBT_FAR_EAST <= MAP_SQUARE_MIDDLE);

    /* (a) A hit. The blast point is the shell's point: on the lane, 96 short
       of the tank's centre, on the west side of the hull. */
    tx = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    ty = tbtMiddle(ly);
    bx = (WORLD)(tx - TBT_SHELL_STOP_SHORT);
    by = ty;
    tbtPlace(sim, TBT_TARGET, tx, ty);
    armourBefore = tankGetArmour(&gs->tanks[TBT_TARGET]);
    /* One builder each side of the tank. The histories say both were on the
       other side a moment ago; a shell that rewinds nothing does not look. */
    tbtBuilderOut(sim, TBT_SHOOTER, (WORLD)(tx - TBT_NEAR_WEST), ty);
    tbtHistory(&sim->lgmPosHistory[TBT_SHOOTER],
               (WORLD)(tx + TBT_FAR_EAST), ty, (WORLD)(tx - TBT_NEAR_WEST), ty);
    tbtBuilderOut(sim, TBT_TARGET, (WORLD)(tx + TBT_FAR_EAST), ty);
    tbtHistory(&sim->lgmPosHistory[TBT_TARGET],
               (WORLD)(tx - TBT_NEAR_WEST), ty, (WORLD)(tx + TBT_FAR_EAST), ty);

    tbtFire(sim, lx, ly, 0);
    UT_ASSERT_MSG(tbtFly(sim) == TBT_HIT_TICK,
                  "the zero-rewind hit moved off game tick %d", TBT_HIT_TICK);
    UT_ASSERT(tankGetArmour(&gs->tanks[TBT_TARGET]) ==
              (BYTE)(armourBefore - gs->rules.shell_damage));
    if (tbtCheckBlast(sim, bx, by, SHELL_OUTCOME_TANK_HIT) != 0) {
        return 1;
    }
    /* Not the tank's centre: that is 6 of a square's 16 pixels further on. */
    UT_ASSERT(!tbtExplosionAt(tbtOnlyEvent(sim, EVENT_EXPLOSION), tx, ty));
    UT_ASSERT(gs->tankExplosions == NULL);
    UT_ASSERT_MSG(gs->lgmen[TBT_SHOOTER]->isDead == TRUE,
                  "a builder 100 units from the shell's point, on the hit side "
                  "of the tank, survived");
    UT_ASSERT_MSG(gs->lgmen[TBT_TARGET]->isDead == FALSE,
                  "a builder on the far side of the tank, 144 units from the "
                  "shell's point, was killed");
    tbtEnd(sim);

    /* (b) A kill. The dead tank's explosion starts at the shell's point too. */
    sim = tbtSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    tx = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    ty = tbtMiddle(ly);
    bx = (WORLD)(tx - TBT_SHELL_STOP_SHORT);
    by = ty;
    tbtPlace(sim, TBT_TARGET, tx, ty);
    (*gs->tanks[TBT_TARGET]).armour = 0;
    tbtFire(sim, lx, ly, 0);
    UT_ASSERT(tbtFly(sim) == TBT_HIT_TICK);
    UT_ASSERT(tankIsDestroyed(&gs->tanks[TBT_TARGET]));
    if (tbtCheckBlast(sim, bx, by, SHELL_OUTCOME_TANK_KILL) != 0 ||
        tbtCheckTankExplosion(sim, bx, by) != 0) {
        return 1;
    }
    tbtEnd(sim);
    return 0;
}

/* ================================================================
 * 2. A rewound hit: explosion, sound and shell death on the present tank.
 * ================================================================ */
int run_tank_blast_rewound_hit_at_present_tank(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tbtSim(&lx, &ly);
    GameSim   *gs;
    WORLD      oldX, oldY, nowX, nowY, shellX, shellY, bx, by;
    BYTE       armourBefore;
    int        diedOn;
    const GameEvent *ev;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    /* The record the shooter saw is on the lane; the tank has since gone
       north, off the lane and into the next row of squares. */
    oldX = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    oldY = tbtMiddle(ly);
    nowX = oldX;
    nowY = (WORLD)(oldY - TBT_MOVED);
    UT_ASSERT((nowY >> TANK_SHIFT_MAPSIZE) != (oldY >> TANK_SHIFT_MAPSIZE));
    /* The shell stops on the lane, 96 west of the record it was tested
       against. The blast is the present tank plus that offset: 96 west of
       the present tank. */
    shellX = (WORLD)(oldX - TBT_SHELL_STOP_SHORT);
    shellY = oldY;
    bx = (WORLD)((int)nowX + ((int)shellX - (int)oldX));
    by = (WORLD)((int)nowY + ((int)shellY - (int)oldY));
    UT_ASSERT(bx == (WORLD)(nowX - TBT_SHELL_STOP_SHORT) && by == nowY);
    tbtPlace(sim, TBT_TARGET, nowX, nowY);
    tbtHistory(&sim->posHistory[TBT_TARGET], oldX, oldY, nowX, nowY);
    armourBefore = tankGetArmour(&gs->tanks[TBT_TARGET]);

    tbtFire(sim, lx, ly, TBT_COMP);
    diedOn = tbtFly(sim);

    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[TBT_TARGET]) ==
                      (BYTE)(armourBefore - gs->rules.shell_damage),
                  "setup: the rewound shell did not hit (armour %d, was %d)",
                  (int)tankGetArmour(&gs->tanks[TBT_TARGET]), (int)armourBefore);
    UT_ASSERT(diedOn == TBT_HIT_TICK);

    /* The shell death, the hit explosion and the hit sound agree on it. */
    if (tbtCheckBlast(sim, bx, by, SHELL_OUTCOME_TANK_HIT) != 0) {
        return 1;
    }
    /* It is not where the shell stopped, not the rewound record, and not the
       present tank's centre. */
    ev = tbtOnlyEvent(sim, EVENT_EXPLOSION);
    UT_ASSERT(!tbtExplosionAt(ev, shellX, shellY));
    UT_ASSERT(!tbtExplosionAt(ev, oldX, oldY));
    UT_ASSERT(!tbtExplosionAt(ev, nowX, nowY));

    /* A hit that does not kill starts no tank explosion. */
    UT_ASSERT(gs->tankExplosions == NULL);

    tbtEnd(sim);
    return 0;
}

/* ================================================================
 * 3. A killing hit: the dead tank's explosion starts at the same point.
 * ================================================================ */
int run_tank_blast_kill_starts_at_present_tank(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tbtSim(&lx, &ly);
    GameSim   *gs;
    WORLD      oldX, oldY, nowX, nowY, bx, by;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    oldX = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    oldY = tbtMiddle(ly);
    nowX = oldX;
    nowY = (WORLD)(oldY - TBT_MOVED);
    bx   = (WORLD)(nowX - TBT_SHELL_STOP_SHORT);
    by   = nowY;
    tbtPlace(sim, TBT_TARGET, nowX, nowY);
    tbtHistory(&sim->posHistory[TBT_TARGET], oldX, oldY, nowX, nowY);
    /* No armour left: any shell kills. */
    (*gs->tanks[TBT_TARGET]).armour = 0;

    tbtFire(sim, lx, ly, TBT_COMP);
    UT_ASSERT(tbtFly(sim) == TBT_HIT_TICK);

    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[TBT_TARGET]),
                  "setup: the shell did not kill the tank");
    /* The hit explosion, the sound, the shell death and the moving explosion
       of the dead tank are all at the one point. */
    if (tbtCheckBlast(sim, bx, by, SHELL_OUTCOME_TANK_KILL) != 0 ||
        tbtCheckTankExplosion(sim, bx, by) != 0) {
        return 1;
    }
    /* Not left behind on the lane, where the rewound record is. */
    UT_ASSERT(gs->tankExplosions->y != oldY);

    tbtEnd(sim);
    return 0;
}

/* ================================================================
 * 4. Hit or miss is decided against the rewound record, as before.
 *
 * Nothing here looks at where an effect went, so this case passes with and
 * without the change it sits beside: it is the fence round the hit test.
 * ================================================================ */
int run_tank_blast_hit_decision_unchanged(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    GameSim   *gs;
    WORLD      laneX, laneY, offY;
    BYTE       armourBefore;
    int        diedOn;

    /* (a) Record on the lane, tank off it: a hit, on the tick a tank
       standing on the record would be hit. */
    sim = tbtSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs    = &sim->sim;
    laneX = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    laneY = tbtMiddle(ly);
    offY  = (WORLD)(laneY - TBT_MOVED);
    tbtPlace(sim, TBT_TARGET, laneX, offY);
    tbtHistory(&sim->posHistory[TBT_TARGET], laneX, laneY, laneX, offY);
    armourBefore = tankGetArmour(&gs->tanks[TBT_TARGET]);
    /* The present tank is out of the shell's reach all the way down the
       lane, so only the record can account for a hit. */
    UT_ASSERT(TBT_MOVED >= gs->rules.tank_hit_radius);
    tbtFire(sim, lx, ly, TBT_COMP);
    diedOn = tbtFly(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[TBT_TARGET]) ==
                      (BYTE)(armourBefore - gs->rules.shell_damage),
                  "a shell through the rewound record must hit");
    UT_ASSERT_MSG(diedOn == TBT_HIT_TICK,
                  "rewound hit on game tick %d, expected %d", diedOn,
                  TBT_HIT_TICK);
    UT_ASSERT(tbtDeaths == 1 && tbtDeathOutcome == SHELL_OUTCOME_TANK_HIT);
    /* The tank is where it was: a hit does not move it at once. */
    UT_ASSERT((*gs->tanks[TBT_TARGET]).x == laneX &&
              (*gs->tanks[TBT_TARGET]).y == offY);
    tbtEnd(sim);

    /* (b) The other way round: the tank stands on the lane now, and its
       record is off it. The shell goes through the tank and runs out. */
    sim = tbtSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs    = &sim->sim;
    laneX = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    laneY = tbtMiddle(ly);
    offY  = (WORLD)(laneY - TBT_MOVED);
    tbtPlace(sim, TBT_TARGET, laneX, laneY);
    tbtHistory(&sim->posHistory[TBT_TARGET], laneX, offY, laneX, laneY);
    armourBefore = tankGetArmour(&gs->tanks[TBT_TARGET]);
    tbtFire(sim, lx, ly, TBT_COMP);
    diedOn = tbtFly(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[TBT_TARGET]) == armourBefore,
                  "a shell that misses the rewound record must miss");
    UT_ASSERT(tbtDeaths == 1 && tbtDeathOutcome == SHELL_OUTCOME_EXPIRED);
    UT_ASSERT(diedOn > TBT_HIT_TICK);
    UT_ASSERT(tbtOnlyEvent(sim, EVENT_SOUND_TANK_HIT) == NULL);
    /* A shell that ran out blows up where the shell is, east of the tank. */
    UT_ASSERT(tbtDeathY == laneY && tbtDeathX > laneX);
    tbtEnd(sim);

    /* (c) The same shell with no rewind and the tank on the lane: the hit
       the rewound one in (a) has to match, tick for tick. */
    sim = tbtSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    tbtPlace(sim, TBT_TARGET, tbtMiddle((BYTE)(lx + TBT_AT_TARGET)),
             tbtMiddle(ly));
    armourBefore = tankGetArmour(&gs->tanks[TBT_TARGET]);
    tbtFire(sim, lx, ly, 0);
    UT_ASSERT(tbtFly(sim) == TBT_HIT_TICK);
    UT_ASSERT(tankGetArmour(&gs->tanks[TBT_TARGET]) ==
              (BYTE)(armourBefore - gs->rules.shell_damage));
    tbtEnd(sim);

    return 0;
}

/* ================================================================
 * 5. After a rewound hit the blast catches builders by where the tank is now.
 * ================================================================ */
int run_tank_blast_builder_at_present_tank(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    GameSim   *gs;
    WORLD      oldX, oldY, nowX, nowY, bx, by;

    /* (a) The hit side, and the place the tank used to be. */
    sim = tbtSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    UT_ASSERT(gs->lgmen[TBT_SHOOTER] != NULL && gs->lgmen[TBT_TARGET] != NULL);

    oldX = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    oldY = tbtMiddle(ly);
    nowX = oldX;
    nowY = (WORLD)(oldY - TBT_MOVED);
    bx   = (WORLD)(nowX - TBT_SHELL_STOP_SHORT);
    by   = nowY;
    tbtPlace(sim, TBT_TARGET, nowX, nowY);
    tbtHistory(&sim->posHistory[TBT_TARGET], oldX, oldY, nowX, nowY);

    /* The target's builder stands beside the tank as it is now, on the side
       the shell landed on: 100 world units from the blast, inside the half
       square (128) a blast kills in. His own record says he was far down the
       lane a moment ago, which must not save him: the blast is in the
       present, so he is tested in the present. */
    tbtBuilderOut(sim, TBT_TARGET, (WORLD)(nowX - TBT_NEAR_WEST), nowY);
    tbtHistory(&sim->lgmPosHistory[TBT_TARGET],
               tbtMiddle((BYTE)(lx + TBT_LANE_LEN)), oldY,
               (WORLD)(nowX - TBT_NEAR_WEST), nowY);

    /* The shooter's builder stands where the shell stopped, 96 short of the
       rewound record: the middle of the old blast, 224 from the new one. */
    tbtBuilderOut(sim, TBT_SHOOTER, (WORLD)(oldX - TBT_SHELL_STOP_SHORT), oldY);
    tbtHistory(&sim->lgmPosHistory[TBT_SHOOTER],
               (WORLD)(oldX - TBT_SHELL_STOP_SHORT), oldY,
               (WORLD)(oldX - TBT_SHELL_STOP_SHORT), oldY);

    tbtFire(sim, lx, ly, TBT_COMP);
    UT_ASSERT(tbtFly(sim) == TBT_HIT_TICK);
    UT_ASSERT(tbtDeaths == 1 && tbtDeathOutcome == SHELL_OUTCOME_TANK_HIT);
    UT_ASSERT(tbtDeathX == bx && tbtDeathY == by);

    UT_ASSERT_MSG(gs->lgmen[TBT_TARGET]->isDead == TRUE,
                  "a builder 100 units from the blast, on the hit side of the "
                  "tank, survived");
    UT_ASSERT_MSG(gs->lgmen[TBT_SHOOTER]->isDead == FALSE,
                  "a builder where the tank used to be was killed by a blast "
                  "that is now at the tank");
    tbtEnd(sim);

    /* (b) The far side. The target's builder stands 48 east of the tank's
       centre, 144 from the blast on the west of the hull: he lives. His
       record puts him on the blast point itself a moment ago, which must not
       kill him either. */
    sim = tbtSim(&lx, &ly);
    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    oldX = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    oldY = tbtMiddle(ly);
    nowX = oldX;
    nowY = (WORLD)(oldY - TBT_MOVED);
    bx   = (WORLD)(nowX - TBT_SHELL_STOP_SHORT);
    by   = nowY;
    tbtPlace(sim, TBT_TARGET, nowX, nowY);
    tbtHistory(&sim->posHistory[TBT_TARGET], oldX, oldY, nowX, nowY);
    tbtBuilderOut(sim, TBT_TARGET, (WORLD)(nowX + TBT_FAR_EAST), nowY);
    tbtHistory(&sim->lgmPosHistory[TBT_TARGET], bx, by,
               (WORLD)(nowX + TBT_FAR_EAST), nowY);

    tbtFire(sim, lx, ly, TBT_COMP);
    UT_ASSERT(tbtFly(sim) == TBT_HIT_TICK);
    UT_ASSERT(tbtDeaths == 1 && tbtDeathOutcome == SHELL_OUTCOME_TANK_HIT);
    UT_ASSERT(tbtDeathX == bx && tbtDeathY == by);
    UT_ASSERT_MSG(gs->lgmen[TBT_TARGET]->isDead == FALSE,
                  "a builder on the far side of the tank, 144 units from the "
                  "blast, was killed");
    tbtEnd(sim);
    return 0;
}
