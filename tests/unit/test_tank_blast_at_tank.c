/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A shell that hits a tank explodes at its own place, on its path
 * (test_tank_blast_at_tank.c).
 *
 * The server tests a shell against where its target was on the shooter's
 * screen: the target's position some history entries ago (lag compensation,
 * shellsCalcCollision). That decision is not what these cases change. What
 * they pin is where the effects of a hit go: the shell's own place on the
 * server on the hit tick, as in the 2.1 release, whether the hit was rewound
 * or not. That point is on the shell's path. The hit explosion, the hit
 * sound, the shell-death position, the start of a killed tank's explosion
 * and the mine check all use it.
 *
 * Where the blast is is one thing; whether it kills a builder is another.
 * For a shell fired from a player's tank the shooter's screen decides that,
 * whatever the shell hit: the builder at the place the shooter's screen
 * showed him, against the blast. The server's present position of the
 * builder does not come into it. A pillbox's shell, and a player's shell with
 * no record as old as the shooter's screen, test the builder where he is now.
 *
 *   run_tank_blast_zero_rewind_unchanged       - with no rewind the blast is
 *       the shell's own point, 96 world units short of the tank's centre in
 *       this lane, for a hit and for a kill; a builder on the hit side dies
 *       and one on the far side lives
 *   run_tank_blast_at_shell_place              - a hit, rewound and not
 *       rewound, puts the hit explosion (sent and in the server's own list),
 *       the hit sound, the shell-death position and the mine check (when
 *       the hit makes one) at the shell's own place, worked out from its
 *       launch point and its steps, on the lane it flies along; the rewound
 *       tank has left the lane
 *   run_tank_blast_kill_at_shell_place         - a killing rewound hit starts
 *       the dead tank's moving explosion at that same point
 *   run_tank_blast_hit_decision_unchanged      - hit or miss is still decided
 *       against the rewound record, on the same tick as before: a tank whose
 *       record is on the lane is hit though it has left the lane, and a tank
 *       that stands on the lane now is missed when its record is off it
 *   run_tank_blast_builder_on_shooter_screen   - after a rewound hit, a
 *       builder the shooter's screen showed inside the blast dies though he
 *       is far from it now, and one it showed outside the blast lives though
 *       he stands inside it now; the shooter's own builder is treated the
 *       same
 *   run_tank_blast_absent_builder_lives        - a builder who was inside
 *       his tank on the frame the shooter saw is not killed by the blast on
 *       that tank, though he stands beside the hit side now; with the record
 *       having him out inside the blast, he dies
 *   run_tank_blast_builder_back_in_tank        - the hit tank's own builder,
 *       seen out inside the blast and back in the tank now, dies on the
 *       tank; seen outside the blast he lives; and when this same shell
 *       kills the tank he is inside, he is not killed as well
 *   run_tank_blast_pill_shell_builder_now      - a pillbox's shell that hits
 *       a tank tests the builder where he is now, whatever the record says
 *   run_tank_blast_no_record_builder_now       - a player's shell with no
 *       builder record as old as the shooter's screen tests him where he is
 *       now
 *   run_shell_blast_pill_hit_on_shooter_screen - a player's shell that hits
 *       a pillbox tests the builder where the shooter's screen showed him
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
#include "explosions.h"
#include "minesexp.h"
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

/* Whether the newest item in the server's own explosion list is the one a
   blast centred on wx, wy makes, as tbtExplosionAt. */
static bool tbtServerExplosionAt(ServerSim *sim, WORLD wx, WORLD wy) {
    WORLD      cx = (WORLD)(wx - TANK_SUBTRACT);
    WORLD      cy = (WORLD)(wy - TANK_SUBTRACT);
    explosions ex = sim->sim.expl;

    return ex != NULL &&
           ex->mx == (BYTE)(cx >> TANK_SHIFT_MAPSIZE) &&
           ex->my == (BYTE)(cy >> TANK_SHIFT_MAPSIZE) &&
           ex->px == (BYTE)((cx & 0xFF) >> 4) &&
           ex->py == (BYTE)((cy & 0xFF) >> 4);
}

/* The shell's own place on the lane after game tick t: worked out from the
   launch point and the steps, not read back from the game. The shell goes
   east along the lane. shellsAddItem starts it shell_start_add steps of
   shell_speed east of the shooter's square middle, and it goes one step of
   shell_speed a game tick. */
static WORLD tbtShellX(const GameSim *gs, BYTE lx, int t) {
    int step = (int)gs->rules.shell_speed;

    return (WORLD)(tbtMiddle(lx) + (int)gs->rules.shell_start_add * step +
                   t * step);
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
    UT_ASSERT_MSG(tbtServerExplosionAt(sim, bx, by),
                  "the server's own hit explosion is not centred on the blast "
                  "point (%u,%u)", (unsigned)bx, (unsigned)by);
    /* A tank hit may make no mine check (tank_hit_sets_off_mines, #560);
       when it does, it is on the blast's square. */
    if (sim->sim.minesExplosions != NULL) {
        UT_ASSERT_MSG(sim->sim.minesExplosions->x ==
                          (BYTE)(bx >> TANK_SHIFT_MAPSIZE) &&
                          sim->sim.minesExplosions->y ==
                              (BYTE)(by >> TANK_SHIFT_MAPSIZE),
                      "the mine check is not on the blast's square (%u,%u)",
                      (unsigned)(bx >> TANK_SHIFT_MAPSIZE),
                      (unsigned)(by >> TANK_SHIFT_MAPSIZE));
    }
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
    int        rc = 0;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;
    UT_CHECK(gs->lgmen[TBT_SHOOTER] != NULL && gs->lgmen[TBT_TARGET] != NULL);
    UT_CHECK(TBT_NEAR_WEST - TBT_SHELL_STOP_SHORT <= MAP_SQUARE_MIDDLE);
    UT_CHECK(TBT_FAR_EAST + TBT_SHELL_STOP_SHORT > MAP_SQUARE_MIDDLE);
    UT_CHECK(TBT_FAR_EAST <= MAP_SQUARE_MIDDLE);

    /* (a) A hit. The blast point is the shell's point: on the lane, 96 short
       of the tank's centre, on the west side of the tank. */
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
    UT_CHECK_MSG(tbtFly(sim) == TBT_HIT_TICK,
                 "the zero-rewind hit moved off game tick %d", TBT_HIT_TICK);
    UT_CHECK(tankGetArmour(&gs->tanks[TBT_TARGET]) ==
             (BYTE)(armourBefore - gs->rules.shell_damage));
    if (tbtCheckBlast(sim, bx, by, SHELL_OUTCOME_TANK_HIT) != 0) {
        rc = 1;
        goto done;
    }
    /* Not the tank's centre: that is 6 of a square's 16 pixels further on. */
    UT_CHECK(!tbtExplosionAt(tbtOnlyEvent(sim, EVENT_EXPLOSION), tx, ty));
    UT_CHECK(gs->tankExplosions == NULL);
    UT_CHECK_MSG(gs->lgmen[TBT_SHOOTER]->isDead == TRUE,
                 "a builder 100 units from the shell's point, on the hit side "
                 "of the tank, survived");
    UT_CHECK_MSG(gs->lgmen[TBT_TARGET]->isDead == FALSE,
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
    UT_CHECK(tbtFly(sim) == TBT_HIT_TICK);
    UT_CHECK(tankIsDestroyed(&gs->tanks[TBT_TARGET]));
    if (tbtCheckBlast(sim, bx, by, SHELL_OUTCOME_TANK_KILL) != 0 ||
        tbtCheckTankExplosion(sim, bx, by) != 0) {
        rc = 1;
        goto done;
    }
done:
    tbtEnd(sim);
    return rc;
}

/* ================================================================
 * 2. A hit, rewound and not rewound: every effect at the shell's own place.
 *
 * Pass 0 rewinds nothing: the tank stands on the lane. Pass 1 rewinds: the
 * record the shooter saw is on the lane, and the tank has since gone north,
 * off the lane and into the next row of squares. Both hit on the same tick,
 * and both blasts are the shell's own place on that tick, on the lane: the
 * launch point plus one step a tick, 96 short of the record's centre. In
 * pass 1 that is 224 from where the tank is now.
 * ================================================================ */
int run_tank_blast_at_shell_place(void) {
    int pass;

    for (pass = 0; pass < 2; pass++) {
        BYTE       lx = 0, ly = 0;
        ServerSim *sim = tbtSim(&lx, &ly);
        GameSim   *gs;
        WORLD      oldX, oldY, nowX, nowY, shellX, shellY;
        BYTE       armourBefore;
        int        diedOn;
        const GameEvent *ev;
        int        rc = 0;

        UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
        gs = &sim->sim;

        oldX = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
        oldY = tbtMiddle(ly);
        nowX = oldX;
        nowY = (pass == 0) ? oldY : (WORLD)(oldY - TBT_MOVED);
        tbtPlace(sim, TBT_TARGET, nowX, nowY);
        if (pass == 1) {
            UT_CHECK((nowY >> TANK_SHIFT_MAPSIZE) !=
                     (oldY >> TANK_SHIFT_MAPSIZE));
            tbtHistory(&sim->posHistory[TBT_TARGET], oldX, oldY, nowX, nowY);
        }
        armourBefore = tankGetArmour(&gs->tanks[TBT_TARGET]);

        tbtFire(sim, lx, ly, (pass == 0) ? 0 : TBT_COMP);
        diedOn = tbtFly(sim);

        UT_CHECK_MSG(tankGetArmour(&gs->tanks[TBT_TARGET]) ==
                         (BYTE)(armourBefore - gs->rules.shell_damage),
                     "pass %d: setup: the shell did not hit (armour %d, was "
                     "%d)", pass, (int)tankGetArmour(&gs->tanks[TBT_TARGET]),
                     (int)armourBefore);
        UT_CHECK_MSG(diedOn == TBT_HIT_TICK,
                     "pass %d: the hit moved off game tick %d to %d", pass,
                     TBT_HIT_TICK, diedOn);

        /* The shell's own place on the hit tick, from its launch point and
           its steps. It is on the lane, the shell's path, between the
           shooter and the record the hit was tested against. */
        shellX = tbtShellX(gs, lx, diedOn);
        shellY = tbtMiddle(ly);
        UT_CHECK(shellX == (WORLD)(oldX - TBT_SHELL_STOP_SHORT));
        UT_CHECK(shellX > tbtMiddle(lx) && shellX < oldX);

        /* The shell death, the sent and the server's hit explosion, the hit
           sound and the mine check agree on it. */
        if (tbtCheckBlast(sim, shellX, shellY, SHELL_OUTCOME_TANK_HIT) != 0) {
            rc = 1;
            goto done;
        }
        UT_CHECK_MSG(tbtDeathY == oldY,
                     "pass %d: the blast is off the shell's path: y %u, the "
                     "lane is %u", pass, (unsigned)tbtDeathY, (unsigned)oldY);
        /* Not the tank's centre, then or now. */
        ev = tbtOnlyEvent(sim, EVENT_EXPLOSION);
        UT_CHECK(!tbtExplosionAt(ev, oldX, oldY));
        UT_CHECK(!tbtExplosionAt(ev, nowX, nowY));

        /* A hit that does not kill starts no tank explosion. */
        UT_CHECK(gs->tankExplosions == NULL);

done:
        tbtEnd(sim);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* ================================================================
 * 3. A killing rewound hit: the dead tank's explosion starts at the shell's
 *    own place too.
 * ================================================================ */
int run_tank_blast_kill_at_shell_place(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim = tbtSim(&lx, &ly);
    GameSim   *gs;
    WORLD      oldX, oldY, nowX, nowY, bx, by;
    int        rc = 0;

    UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
    gs = &sim->sim;

    oldX = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    oldY = tbtMiddle(ly);
    nowX = oldX;
    nowY = (WORLD)(oldY - TBT_MOVED);
    bx   = tbtShellX(gs, lx, TBT_HIT_TICK);
    by   = oldY;
    UT_CHECK(bx == (WORLD)(oldX - TBT_SHELL_STOP_SHORT));
    tbtPlace(sim, TBT_TARGET, nowX, nowY);
    tbtHistory(&sim->posHistory[TBT_TARGET], oldX, oldY, nowX, nowY);
    /* No armour left: any shell kills. */
    (*gs->tanks[TBT_TARGET]).armour = 0;

    tbtFire(sim, lx, ly, TBT_COMP);
    UT_CHECK(tbtFly(sim) == TBT_HIT_TICK);

    UT_CHECK_MSG(tankIsDestroyed(&gs->tanks[TBT_TARGET]),
                 "setup: the shell did not kill the tank");
    /* The hit explosion, the sound, the shell death, the mine check and the
       moving explosion of the dead tank are all at the one point, on the
       lane. */
    if (tbtCheckBlast(sim, bx, by, SHELL_OUTCOME_TANK_KILL) != 0 ||
        tbtCheckTankExplosion(sim, bx, by) != 0) {
        rc = 1;
        goto done;
    }

done:
    tbtEnd(sim);
    return rc;
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
    int        rc = 0;

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
    UT_CHECK(TBT_MOVED >= gs->rules.tank_hit_radius);
    tbtFire(sim, lx, ly, TBT_COMP);
    diedOn = tbtFly(sim);
    UT_CHECK_MSG(tankGetArmour(&gs->tanks[TBT_TARGET]) ==
                     (BYTE)(armourBefore - gs->rules.shell_damage),
                 "a shell through the rewound record must hit");
    UT_CHECK_MSG(diedOn == TBT_HIT_TICK,
                 "rewound hit on game tick %d, expected %d", diedOn,
                 TBT_HIT_TICK);
    UT_CHECK(tbtDeaths == 1 && tbtDeathOutcome == SHELL_OUTCOME_TANK_HIT);
    /* The tank is where it was: a hit does not move it at once. */
    UT_CHECK((*gs->tanks[TBT_TARGET]).x == laneX &&
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
    UT_CHECK_MSG(tankGetArmour(&gs->tanks[TBT_TARGET]) == armourBefore,
                 "a shell that misses the rewound record must miss");
    UT_CHECK(tbtDeaths == 1 && tbtDeathOutcome == SHELL_OUTCOME_EXPIRED);
    UT_CHECK(diedOn > TBT_HIT_TICK);
    UT_CHECK(tbtOnlyEvent(sim, EVENT_SOUND_TANK_HIT) == NULL);
    /* A shell that ran out blows up where the shell is, east of the tank. */
    UT_CHECK(tbtDeathY == laneY && tbtDeathX > laneX);
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
    UT_CHECK(tbtFly(sim) == TBT_HIT_TICK);
    UT_CHECK(tankGetArmour(&gs->tanks[TBT_TARGET]) ==
             (BYTE)(armourBefore - gs->rules.shell_damage));
done:
    tbtEnd(sim);

    return rc;
}

/* ================================================================
 * The builder cases after a rewound hit. One layout serves them all.
 * ================================================================ */

/* A builder this far from a blast is inside it (the kill radius is half a
   square, 128), and one this far is well outside. */
#define TBT_IN_BLAST  100
#define TBT_OUT_BLAST 300

/* One rewound hit. */
typedef struct {
    WORLD oldX, oldY;      /* the target tank on the shooter's screen */
    WORLD nowX, nowY;      /* the target tank now */
    WORLD blastX, blastY;  /* the shell's own place on the hit tick, where it
                              reached the hit circle at the old place: the blast,
                              drawn, heard and on the shooter's screen */
} TbtHit;

/* The target tank stood on the lane on the frame the shooter saw and has gone
   north off it since, by two hit radii. The shell stops on the lane, and the
   blast is there: on the lane, 96 west of the old place and 224 south of the
   present tank's row. */
static void tbtRewoundHit(ServerSim *sim, BYTE lx, BYTE ly, TbtHit *g) {
    g->oldX   = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
    g->oldY   = tbtMiddle(ly);
    g->nowX   = g->oldX;
    g->nowY   = (WORLD)(g->oldY - TBT_MOVED);
    g->blastX  = (WORLD)(g->oldX - TBT_SHELL_STOP_SHORT);
    g->blastY  = g->oldY;
    tbtPlace(sim, TBT_TARGET, g->nowX, g->nowY);
    tbtHistory(&sim->posHistory[TBT_TARGET], g->oldX, g->oldY, g->nowX,
               g->nowY);
}

/* A seat's builder as he is now and as the shooter's screen showed him. A
   player's shell rewinds a builder one entry less than a tank, TBT_COMP - 1,
   so the record that many entries back is the shooter's screen: it and every
   older one say the "seen" state, and the TBT_COMP - 1 newer ones, the
   present included, say the "now" state. Out is on the ground and alive; not
   out is inside his tank. The frames are not a walk the game would record:
   the "seen" and "now" places can be squares apart with no steps between.
   The code reads one frame, so only that frame matters. */
static void tbtBuilder(ServerSim *sim, BYTE slot,
                       bool seenOut, WORLD seenX, WORLD seenY,
                       bool nowOut, WORLD nowX, WORLD nowY) {
    lgm        *l = &sim->sim.lgmen[slot];
    PosHistory *h = &sim->lgmPosHistory[slot];
    int         i;

    if (nowOut) {
        tbtBuilderOut(sim, slot, nowX, nowY);
    } else {
        (*l)->inTank = TRUE;
        (*l)->isDead = FALSE;
    }
    posHistoryInit(h);
    for (i = 0; i < POSITION_HISTORY_SIZE - (TBT_COMP - 1); i++) {
        posHistoryRecord(h, seenX, seenY, seenOut);
    }
    for (i = 0; i < TBT_COMP - 1; i++) {
        posHistoryRecord(h, nowX, nowY, nowOut);
    }
}

/* How many builder losses the server has queued; the last is copied out. */
static int tbtLostEvents(ServerSim *sim, GameEvent *last) {
    int n = 0;
    int i;

    for (i = 0; i < (int)sim->eventCount; i++) {
        if (sim->events[i].type == EVENT_LGM_LOST) {
            if (last != NULL) {
                *last = sim->events[i];
            }
            n++;
        }
    }
    return n;
}

/* The rewound hit landed and its blast is at the shell's own place. */
static int tbtCheckRewoundHit(const TbtHit *g, int diedOn, uint8_t outcome) {
    UT_ASSERT_MSG(diedOn == TBT_HIT_TICK,
                  "setup: the shell ended on game tick %d, expected %d", diedOn,
                  TBT_HIT_TICK);
    UT_ASSERT_MSG(tbtDeaths == 1 && tbtDeathOutcome == outcome,
                  "setup: %d shell deaths, outcome %u, expected one of %u",
                  tbtDeaths, (unsigned)tbtDeathOutcome, (unsigned)outcome);
    UT_ASSERT_MSG(tbtDeathX == g->blastX && tbtDeathY == g->blastY,
                  "the blast is at (%u,%u), expected the shell's own place "
                  "(%u,%u)", (unsigned)tbtDeathX, (unsigned)tbtDeathY,
                  (unsigned)g->blastX, (unsigned)g->blastY);
    return 0;
}

/* ================================================================
 * 5. After a rewound hit the shooter's screen decides who the blast kills.
 *
 * The four builders are placed so that where the shooter's screen showed
 * each one and where he stands now give opposite answers against the one
 * blast, which checks that the builder test reads the shooter's screen.
 * ================================================================ */
int run_tank_blast_builder_on_shooter_screen(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    GameSim   *gs;
    TbtHit     g;
    int        pass;
    int        rc = 0;

    UT_ASSERT(TBT_IN_BLAST <= MAP_SQUARE_MIDDLE);
    UT_ASSERT(TBT_OUT_BLAST > MAP_SQUARE_MIDDLE);

    for (pass = 0; pass < 2; pass++) {
        sim = tbtSim(&lx, &ly);
        UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
        gs = &sim->sim;
        UT_CHECK(gs->lgmen[TBT_SHOOTER] != NULL &&
                 gs->lgmen[TBT_TARGET] != NULL);
        tbtRewoundHit(sim, lx, ly, &g);

        if (pass == 0) {
            /* The shooter's screen showed both builders inside the blast:
               the target's 100 west of where the shell stopped, the
               shooter's own 100 south of it. Both have walked 300 east
               since, and neither is anywhere near the blast now. */
            tbtBuilder(sim, TBT_TARGET,
                       true, (WORLD)(g.blastX - TBT_IN_BLAST), g.blastY,
                       true, (WORLD)(g.blastX + TBT_OUT_BLAST), g.blastY);
            tbtBuilder(sim, TBT_SHOOTER,
                       true, g.blastX, (WORLD)(g.blastY + TBT_IN_BLAST),
                       true, (WORLD)(g.blastX + TBT_OUT_BLAST),
                       (WORLD)(g.blastY + TBT_IN_BLAST));
        } else {
            /* The shooter's screen showed both 300 east of where the shell
               stopped. Both stand inside the blast now: the target's 100
               west of it, the shooter's own 100 from it to the north
               east. */
            tbtBuilder(sim, TBT_TARGET,
                       true, (WORLD)(g.blastX + TBT_OUT_BLAST), g.blastY,
                       true, (WORLD)(g.blastX - TBT_IN_BLAST), g.blastY);
            tbtBuilder(sim, TBT_SHOOTER,
                       true, (WORLD)(g.blastX + TBT_OUT_BLAST),
                       (WORLD)(g.blastY + TBT_IN_BLAST),
                       true, (WORLD)(g.blastX + 60), (WORLD)(g.blastY - 80));
        }

        tbtFire(sim, lx, ly, TBT_COMP);
        if (tbtCheckRewoundHit(&g, tbtFly(sim), SHELL_OUTCOME_TANK_HIT) != 0) {
            rc = 1;
            goto done;
        }

        if (pass == 0) {
            UT_CHECK_MSG(gs->lgmen[TBT_TARGET]->isDead == TRUE,
                         "the shooter's screen showed the builder 100 from "
                         "where the shell hit, and he lived");
            UT_CHECK_MSG(gs->lgmen[TBT_SHOOTER]->isDead == TRUE,
                         "the shooter's screen showed his own builder 100 "
                         "from where the shell hit, and he lived");
        } else {
            UT_CHECK_MSG(gs->lgmen[TBT_TARGET]->isDead == FALSE,
                         "the shooter's screen showed the builder 300 from "
                         "where the shell hit, but he was killed because he "
                         "stands by the blast now");
            UT_CHECK_MSG(gs->lgmen[TBT_SHOOTER]->isDead == FALSE,
                         "the shooter's screen showed his own builder 300 "
                         "from where the shell hit, but he was killed because "
                         "he stands by the blast now");
        }
done:
        tbtEnd(sim);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* ================================================================
 * 6. A blast on a hit tank does not kill a builder the shooter's screen
 *    did not show.
 * ================================================================ */
int run_tank_blast_absent_builder_lives(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    GameSim   *gs;
    WORLD      oldX, oldY, nowX, nowY, bx, by, manX;
    PosHistory *h;
    int        pass, i;
    int        rc = 0;

    /* A player's shell rewinds a builder one entry less than a tank. */
    UT_ASSERT(TBT_COMP - 1 >= 2);

    for (pass = 0; pass < 2; pass++) {
        sim = tbtSim(&lx, &ly);
        UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
        gs = &sim->sim;
        UT_CHECK(gs->lgmen[TBT_TARGET] != NULL);

        oldX = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
        oldY = tbtMiddle(ly);
        nowX = oldX;
        nowY = (WORLD)(oldY - TBT_MOVED);
        bx   = (WORLD)(oldX - TBT_SHELL_STOP_SHORT);
        by   = oldY;
        tbtPlace(sim, TBT_TARGET, nowX, nowY);
        tbtHistory(&sim->posHistory[TBT_TARGET], oldX, oldY, nowX, nowY);

        /* The target's builder is out now, on the lane west of the blast,
           100 world units from it. */
        manX = (WORLD)(oldX - TBT_NEAR_WEST);
        tbtBuilderOut(sim, TBT_TARGET, manX, oldY);
        h = &sim->lgmPosHistory[TBT_TARGET];
        posHistoryInit(h);
        if (pass == 0) {
            /* Inside the tank up to and on the frame the shooter saw,
               TBT_COMP - 1 entries back; out on the entries since. */
            for (i = 0; i < POSITION_HISTORY_SIZE - (TBT_COMP - 1); i++) {
                posHistoryRecord(h, nowX, nowY, false);
            }
        } else {
            /* Out on the frame the shooter saw as well, and inside the blast
               as the shooter's screen had it: 100 west of where the shell
               stopped, which is 96 west of the tank's old place. */
            for (i = 0; i < POSITION_HISTORY_SIZE - (TBT_COMP - 1); i++) {
                posHistoryRecord(h,
                                 (WORLD)(oldX - TBT_SHELL_STOP_SHORT -
                                         TBT_IN_BLAST),
                                 oldY, true);
            }
        }
        for (i = 0; i < TBT_COMP - 1; i++) {
            posHistoryRecord(h, manX, oldY, true);
        }

        tbtFire(sim, lx, ly, TBT_COMP);
        UT_CHECK(tbtFly(sim) == TBT_HIT_TICK);
        UT_CHECK(tbtDeaths == 1 && tbtDeathOutcome == SHELL_OUTCOME_TANK_HIT);
        UT_CHECK(tbtDeathX == bx && tbtDeathY == by);

        if (pass == 0) {
            UT_CHECK_MSG(gs->lgmen[TBT_TARGET]->isDead == FALSE,
                         "the shooter's screen showed no builder, but the "
                         "blast 100 from where he stands now killed him");
        } else {
            UT_CHECK_MSG(gs->lgmen[TBT_TARGET]->isDead == TRUE,
                         "the shooter's screen showed the builder out, 100 "
                         "units from where the shell hit, and he lived");
        }
done:
        tbtEnd(sim);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* ================================================================
 * 7. The hit tank's own builder: seen out, back in the tank now.
 * ================================================================ */
int run_tank_blast_builder_back_in_tank(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    GameSim   *gs;
    TbtHit     g;
    GameEvent  ev;
    lgm        man;
    int        pass;
    int        rc = 0;

    /* Pass 0: seen out inside the blast, the tank survives the hit. He dies,
               on the tank.
       Pass 1: seen out, outside the blast. He lives, in the tank.
       Pass 2: seen out inside the blast, and this shell kills the tank he is
               in. He went down with the tank and is not killed as well.
       Pass 3: the same killing shell, and he is still out now. He dies. */
    for (pass = 0; pass < 4; pass++) {
        bool seenIn  = (pass != 1);
        bool kills   = (pass >= 2);
        bool outNow  = (pass == 3);
        WORLD seenManX;

        sim = tbtSim(&lx, &ly);
        UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
        gs = &sim->sim;
        UT_CHECK(gs->lgmen[TBT_TARGET] != NULL);
        man = gs->lgmen[TBT_TARGET];
        tbtRewoundHit(sim, lx, ly, &g);
        if (kills) {
            /* No armour left: any shell kills. */
            (*gs->tanks[TBT_TARGET]).armour = 0;
        }

        seenManX = seenIn ? (WORLD)(g.blastX - TBT_IN_BLAST)
                          : (WORLD)(g.blastX + TBT_OUT_BLAST);
        /* Where he is when he is out now: 300 east of where the shell
           stopped, out of reach of the blast. */
        tbtBuilder(sim, TBT_TARGET, true, seenManX, g.blastY,
                   outNow, outNow ? (WORLD)(g.blastX + TBT_OUT_BLAST) : g.nowX,
                   outNow ? g.blastY : g.nowY);
        UT_CHECK(man->isDead == FALSE && man->inTank == (outNow ? FALSE : TRUE));
        sim->eventCount = 0;

        tbtFire(sim, lx, ly, TBT_COMP);
        if (tbtCheckRewoundHit(&g, tbtFly(sim),
                               kills ? SHELL_OUTCOME_TANK_KILL
                                     : SHELL_OUTCOME_TANK_HIT) != 0) {
            rc = 1;
            goto done;
        }
        UT_CHECK(tankIsDestroyed(&gs->tanks[TBT_TARGET]) ==
                 (kills ? TRUE : FALSE));

        memset(&ev, 0, sizeof(ev));
        if (pass == 0) {
            UT_CHECK_MSG(man->isDead == TRUE,
                         "the shooter's screen showed the hit tank's builder "
                         "out, 100 from where the shell hit, and he lived "
                         "because he is back in the tank");
            UT_CHECK_MSG(man->inTank == FALSE,
                         "the dead builder still reads as in the tank");
            UT_CHECK_MSG(tbtLostEvents(sim, &ev) == 1,
                         "%d builder losses were sent, expected 1",
                         tbtLostEvents(sim, NULL));
            UT_CHECK_MSG(ev.data[0] == TBT_TARGET && ev.data[1] == TBT_SHOOTER,
                         "the loss names victim %u and killer %u",
                         (unsigned)ev.data[0], (unsigned)ev.data[1]);
            /* He dies on the tank, where it is now. */
            UT_CHECK_MSG(ev.data[3] == (BYTE)(g.nowX >> TANK_SHIFT_MAPSIZE) &&
                             ev.data[4] == (BYTE)(g.nowY >> TANK_SHIFT_MAPSIZE),
                         "the loss is on square (%u,%u); the tank is on "
                         "(%u,%u) now", (unsigned)ev.data[3],
                         (unsigned)ev.data[4],
                         (unsigned)(g.nowX >> TANK_SHIFT_MAPSIZE),
                         (unsigned)(g.nowY >> TANK_SHIFT_MAPSIZE));
            UT_CHECK_MSG(man->destX == g.nowX && man->destY == g.nowY,
                         "the helicopter is not flying to the tank");
        } else if (pass == 1) {
            UT_CHECK_MSG(man->isDead == FALSE && man->inTank == TRUE,
                         "the shooter's screen showed the builder 300 from "
                         "where the shell hit, but the blast on his tank "
                         "killed him inside it");
            UT_CHECK(tbtLostEvents(sim, NULL) == 0);
        } else if (pass == 2) {
            UT_CHECK_MSG(man->isDead == FALSE && man->inTank == TRUE,
                         "the shell that killed the tank also killed the "
                         "builder inside it");
            UT_CHECK(tbtLostEvents(sim, NULL) == 0);
        } else {
            UT_CHECK_MSG(man->isDead == TRUE,
                         "the shooter's screen showed the builder out, 100 "
                         "from where the shell hit, he is still out, and he "
                         "lived because the shell also killed his tank");
            UT_CHECK(tbtLostEvents(sim, NULL) == 1);
        }
done:
        tbtEnd(sim);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* One shell east along the lane from the shooter's square that is a
   pillbox's: nobody's own, and it rewinds nothing. */
static void tbtFirePill(ServerSim *sim, BYTE lx, BYTE ly) {
    sim->sim.lagCompTicks = 0;
    shellsAddItem(&sim->sim, &sim->sim.shs, tbtMiddle(lx), tbtMiddle(ly),
                  (TURNTYPE)BRADIANS_EAST, (TURNTYPE)TBT_SHELL_LEN, NEUTRAL,
                  TBT_TARGET, 0, FALSE);
}

/* ================================================================
 * 8. A pillbox's shell that hits a tank: the builder where he is now.
 * ================================================================ */
int run_tank_blast_pill_shell_builder_now(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    GameSim   *gs;
    WORLD      tx, ty, bx, by;
    lgm        man;
    int        pass;
    int        rc = 0;

    /* Pass 0: the record has him out 300 from the blast, he is 100 from it
               now. He dies.
       Pass 1: the record has him out 100 from the blast, he is 300 from it
               now. He lives.
       Pass 2: the record has him in his tank, he is 100 from it now. He
               dies.
       A player's shell would give each the other fate. */
    for (pass = 0; pass < 3; pass++) {
        sim = tbtSim(&lx, &ly);
        UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
        gs = &sim->sim;
        UT_CHECK(gs->lgmen[TBT_TARGET] != NULL);
        man = gs->lgmen[TBT_TARGET];

        /* The tank stands on the lane now. Its record says it was north of
           the lane a moment ago, which a shell that rewinds nothing does not
           read: the shell hits it where it is, 96 short of its centre. */
        tx = tbtMiddle((BYTE)(lx + TBT_AT_TARGET));
        ty = tbtMiddle(ly);
        bx = (WORLD)(tx - TBT_SHELL_STOP_SHORT);
        by = ty;
        tbtPlace(sim, TBT_TARGET, tx, ty);
        tbtHistory(&sim->posHistory[TBT_TARGET], tx, (WORLD)(ty - TBT_MOVED),
                   tx, ty);
        /* The shooter's tank is not this shell's owner: out of its way. */
        tbtPlace(sim, TBT_SHOOTER, tbtMiddle(lx), tbtMiddle((BYTE)(ly + 2)));

        if (pass == 0) {
            tbtBuilder(sim, TBT_TARGET,
                       true, (WORLD)(bx + TBT_OUT_BLAST), by,
                       true, (WORLD)(bx - TBT_IN_BLAST), by);
        } else if (pass == 1) {
            tbtBuilder(sim, TBT_TARGET,
                       true, (WORLD)(bx - TBT_IN_BLAST), by,
                       true, (WORLD)(bx + TBT_OUT_BLAST), by);
        } else {
            tbtBuilder(sim, TBT_TARGET,
                       false, tx, ty,
                       true, (WORLD)(bx - TBT_IN_BLAST), by);
        }

        tbtFirePill(sim, lx, ly);
        UT_CHECK_MSG(tbtFly(sim) == TBT_HIT_TICK,
                     "setup: the pill's shell did not hit the tank where it "
                     "is now");
        UT_CHECK(tbtDeaths == 1 && tbtDeathOutcome == SHELL_OUTCOME_TANK_HIT);
        UT_CHECK(tbtDeathX == bx && tbtDeathY == by);

        if (pass == 1) {
            UT_CHECK_MSG(man->isDead == FALSE,
                         "a pill's shell killed a builder who is 300 from "
                         "its blast now");
        } else {
            UT_CHECK_MSG(man->isDead == TRUE,
                         "pass %d: a pill's shell did not kill a builder who "
                         "is 100 from its blast now", pass);
        }
done:
        tbtEnd(sim);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* ================================================================
 * 9. A player's shell, a rewound tank hit, and no builder record as old as
 *    the shooter's screen: the builder where he is now.
 * ================================================================ */
int run_tank_blast_no_record_builder_now(void) {
    BYTE        lx = 0, ly = 0;
    ServerSim  *sim;
    GameSim    *gs;
    TbtHit      g;
    PosHistory *h;
    lgm         man;
    int         pass, i;
    int         rc = 0;

    /* Pass 0: he stands, and every record he has stands, 300 from the
               blast: he lives.
       Pass 1: he stands, and every record he has stands, 100 from the
               blast: he dies.
       Pass 2: pass 1 with one record more, which is the shooter's screen and
               has him 300 from the blast: he lives. So pass 1 used where he
               stands for want of a record. */
    for (pass = 0; pass < 3; pass++) {
        WORLD manX, manY;

        sim = tbtSim(&lx, &ly);
        UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
        gs = &sim->sim;
        UT_CHECK(gs->lgmen[TBT_TARGET] != NULL);
        man = gs->lgmen[TBT_TARGET];
        tbtRewoundHit(sim, lx, ly, &g);

        if (pass == 0) {
            manX = (WORLD)(g.blastX + TBT_OUT_BLAST);
            manY = g.blastY;
        } else {
            manX = (WORLD)(g.blastX - TBT_IN_BLAST);
            manY = g.blastY;
        }
        tbtBuilderOut(sim, TBT_TARGET, manX, manY);
        h = &sim->lgmPosHistory[TBT_TARGET];
        posHistoryInit(h);
        if (pass == 2) {
            posHistoryRecord(h, (WORLD)(g.blastX + TBT_OUT_BLAST), g.blastY, true);
        }
        /* TBT_COMP - 1 records: the newest is 0 entries back, so none is the
           TBT_COMP - 1 back the shell asks for. */
        for (i = 0; i < TBT_COMP - 1; i++) {
            posHistoryRecord(h, manX, manY, true);
        }

        tbtFire(sim, lx, ly, TBT_COMP);
        if (tbtCheckRewoundHit(&g, tbtFly(sim), SHELL_OUTCOME_TANK_HIT) != 0) {
            rc = 1;
            goto done;
        }

        if (pass == 0) {
            UT_CHECK_MSG(man->isDead == FALSE,
                         "with no record as old as the shooter's screen, a "
                         "builder 300 from the blast was killed");
        } else if (pass == 1) {
            UT_CHECK_MSG(man->isDead == TRUE,
                         "with no record as old as the shooter's screen, a "
                         "builder 100 from the blast lived");
        } else {
            UT_CHECK_MSG(man->isDead == FALSE,
                         "the oldest record is the shooter's screen and has "
                         "the builder 300 from where the shell hit, but he "
                         "was killed");
        }
done:
        tbtEnd(sim);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* ================================================================
 * 10. A player's shell that hits a pillbox: the shooter's screen decides.
 *
 * A blast on a pillbox catches a builder on the pillbox's own square and
 * nobody else. There is one blast point here, the middle of that square.
 * ================================================================ */
int run_shell_blast_pill_hit_on_shooter_screen(void) {
    BYTE       lx = 0, ly = 0;
    ServerSim *sim;
    GameSim   *gs;
    pillbox    item;
    BYTE       px, py;
    WORLD      onX, onY, offX, offY;
    lgm        man;
    int        pass;
    int        rc = 0;

    /* Pass 0: the shooter's screen showed him on the pillbox's square; he is
               two squares east of it now. He dies.
       Pass 1: the shooter's screen showed him two squares east; he is on the
               pillbox's square now. He lives. */
    for (pass = 0; pass < 2; pass++) {
        sim = tbtSim(&lx, &ly);
        UT_ASSERT_MSG(sim != NULL, "setup: no sim, tanks or clear lane");
        gs = &sim->sim;
        UT_CHECK(gs->lgmen[TBT_TARGET] != NULL);
        man = gs->lgmen[TBT_TARGET];

        /* The map's first pillbox, moved onto the lane. */
        px = (BYTE)(lx + TBT_AT_TARGET);
        py = ly;
        UT_CHECK(pillsGetNumPills(&gs->pb) >= 1);
        pillsGetPill(&gs->pb, &item, 1);
        item.x = px;
        item.y = py;
        item.inTank = FALSE;
        pillsSetPill(gs, &gs->pb, &item, 1);
        UT_CHECK_MSG(pillsExistPos(&gs->pb, px, py) == TRUE &&
                         pillsGetArmourPos(&gs->pb, px, py) > 0,
                     "setup: no live pillbox on the lane");

        /* The target tank is off the lane and has not moved. */
        tbtPlace(sim, TBT_TARGET, tbtMiddle((BYTE)(lx + TBT_LANE_LEN - 1)),
                 tbtMiddle((BYTE)(ly + 2)));
        tbtHistory(&sim->posHistory[TBT_TARGET],
                   tbtMiddle((BYTE)(lx + TBT_LANE_LEN - 1)),
                   tbtMiddle((BYTE)(ly + 2)),
                   tbtMiddle((BYTE)(lx + TBT_LANE_LEN - 1)),
                   tbtMiddle((BYTE)(ly + 2)));

        onX  = (WORLD)(tbtMiddle(px) + 40);
        onY  = (WORLD)(tbtMiddle(py) + 40);
        offX = tbtMiddle((BYTE)(px + 2));
        offY = tbtMiddle(py);
        if (pass == 0) {
            tbtBuilder(sim, TBT_TARGET, true, onX, onY, true, offX, offY);
        } else {
            tbtBuilder(sim, TBT_TARGET, true, offX, offY, true, onX, onY);
        }

        tbtFire(sim, lx, ly, TBT_COMP);
        (void)tbtFly(sim);
        UT_CHECK_MSG(tbtDeaths == 1 && tbtDeathOutcome == SHELL_OUTCOME_IMPACT,
                     "setup: the shell did not hit the pillbox (deaths %d, "
                     "outcome %u)", tbtDeaths, (unsigned)tbtDeathOutcome);
        UT_CHECK_MSG(tbtDeathX == tbtMiddle(px) && tbtDeathY == tbtMiddle(py),
                     "the blast is at (%u,%u), not the middle of the "
                     "pillbox's square", (unsigned)tbtDeathX,
                     (unsigned)tbtDeathY);

        if (pass == 0) {
            UT_CHECK_MSG(man->isDead == TRUE,
                         "the shooter's screen showed the builder on the "
                         "pillbox's square when the shell hit it, and he "
                         "lived");
        } else {
            UT_CHECK_MSG(man->isDead == FALSE,
                         "the shooter's screen showed the builder two "
                         "squares from the pillbox, but he was killed "
                         "because he is on its square now");
        }
done:
        tbtEnd(sim);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}
