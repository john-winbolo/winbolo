/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * The four combat policy pointers:
 *
 *   run_scenario_policy_damage_scale     — a boss at 50 takes more hits; a
 *       victim at 0 takes none and the armour bar never moves
 *   run_scenario_policy_invulnerable_tank — canDie false at all three tank
 *       sites: a hundred shells, a mine, and a drive into deep sea
 *   run_scenario_policy_protected_builder — a shell on the builder's square
 *       leaves him standing
 *   run_scenario_policy_protected_pill   — a pill held at one armour by each
 *       of the two zero tests, and a dead pill not raised by either
 *   run_scenario_policy_can_build        — a protected square refuses a road,
 *       with the refusal an invalid request gets and the tank's stores intact
 *   run_scenario_policy_can_capture      — a locked base is driven over and
 *       keeps its owner, and a locked pill is not picked up
 *   run_scenario_policy_kill_ops_ignore_can_die — the two kill ops kill under
 *       a policy that refuses every death, and never put the question
 *   run_scenario_policy_combat_null_is_classic — all four decisions with
 *       nothing registered
 *
 * Every case drives the real call site rather than the policy: a shell, a
 * mine, a deep-sea square, an explosion beside a builder, a build order, a
 * tank standing on a base, and the op funnel.
 *
 * Reads the ServerSim and lgm structs directly; the unittests profile
 * permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* scenarioPolicy, events[] / eventCount */
#include "server_sim_scenario.h"   /* serverSimSetScenarioPolicy, the op funnel */
#include "game_sim.h"
#include "bases.h"
#include "bolo_map.h"
#include "input_packet.h"          /* EVENT_ASSISTANT_MSG, ASSIST_MSG_NO_BUILD */
#include "lgm.h"
#include "mines.h"
#include "pillbox.h"
#include "tank.h"
#include "tankexp.h"               /* TK_DAMAGE */
#include "test_harness.h"

#define PC_SHOOTER 0
#define PC_TARGET  1

/* Trees comfortably above any single build cost, so a refused order is the
   only thing that can leave the tank's stores where they were. */
#define PC_TREES 20

/* ── The test policy ──────────────────────────────────────────────────────
 *
 * One vtable with all four filled and one context holding what each should
 * answer, so a case changes an answer between two runs of the same call site
 * and nothing else moves. The counters are what prove a question is asked
 * once per decision — and, for the kill ops, that it is not asked at all. */
typedef struct {
    int  scaleAsks;
    int  scalePct;
    int  buildAsks;
    bool build;
    int  captureAsks;
    bool capture;
    int  dieAsks;
    bool die;
    /* What the last question carried, so a case can prove the arguments as
       well as the answer. */
    BYTE lastDieKind, lastDieIndex, lastDieKiller, lastDieCause;
    BYTE lastBuildAction, lastBuildX, lastBuildY, lastBuildPillIdx;
    BYTE lastCaptureKind, lastCaptureIndex, lastCapturePlayer;
    BYTE lastScaleAttacker, lastScaleVictim, lastScaleCause;
    BYTE lastBuildPlayer;
} PcCtx;

static int pcDamageScale(void *ctx, BYTE attacker, BYTE victim, BYTE cause) {
    PcCtx *p = (PcCtx *)ctx;
    p->scaleAsks++;
    p->lastScaleAttacker = attacker;
    p->lastScaleVictim = victim;
    p->lastScaleCause = cause;
    return p->scalePct;
}

static bool pcCanBuild(void *ctx, BYTE player, BYTE action, BYTE mapX,
                       BYTE mapY, BYTE pillIdx) {
    PcCtx *p = (PcCtx *)ctx;
    p->buildAsks++;
    p->lastBuildPlayer = player;
    p->lastBuildAction = action;
    p->lastBuildX = mapX;
    p->lastBuildY = mapY;
    p->lastBuildPillIdx = pillIdx;
    return p->build;
}

static bool pcCanCapture(void *ctx, BYTE kind, BYTE index, BYTE player) {
    PcCtx *p = (PcCtx *)ctx;
    p->captureAsks++;
    p->lastCaptureKind = kind;
    p->lastCaptureIndex = index;
    p->lastCapturePlayer = player;
    return p->capture;
}

static bool pcCanDie(void *ctx, BYTE kind, BYTE index, BYTE killer,
                     BYTE cause) {
    PcCtx *p = (PcCtx *)ctx;
    p->dieAsks++;
    p->lastDieKind = kind;
    p->lastDieIndex = index;
    p->lastDieKiller = killer;
    p->lastDieCause = cause;
    return p->die;
}

/* Every answer starts classic, so a case sets only the one it is about. */
static void pcFillPolicy(ScenarioPolicy *pol, PcCtx *pc) {
    memset(pol, 0, sizeof(*pol));
    memset(pc, 0, sizeof(*pc));
    pc->scalePct = 100;
    pc->build    = true;
    pc->capture  = true;
    pc->die      = true;
    pol->damageScale = pcDamageScale;
    pol->canBuild    = pcCanBuild;
    pol->canCapture  = pcCanCapture;
    pol->canDie      = pcCanDie;
    pol->ctx         = pc;
}

/* ── Fixtures ─────────────────────────────────────────────────────────── */

/* A running round with a shooter in slot 0 and a target in slot 1. */
static ServerSim *pcTwoTankSim(void) {
    ServerSim *sim = ut_make_running_sim("Shooter");
    if (sim == NULL) return NULL;
    serverSimAddPlayer(sim, PC_TARGET, "Target", false);
    return sim;
}

/* Find a map tile of the wanted terrain carrying no base, pill or mine and
   not under the slot-0 tank. Returns false if the map has none. */
static bool pcFindTile(ServerSim *sim, BYTE want, BYTE *ox, BYTE *oy) {
    BYTE tx = tankGetMX(&sim->sim.tanks[PC_SHOOTER]);
    BYTE ty = tankGetMY(&sim->sim.tanks[PC_SHOOTER]);
    int x, y;

    for (y = 0; y < 256; y++) {
        for (x = 0; x < 256; x++) {
            if ((BYTE)x == tx && (BYTE)y == ty) {
                continue;
            }
            if (mapGetPos(&sim->sim.mp, (BYTE)x, (BYTE)y) == want &&
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

/* The world co-ordinate at the middle of a map square. */
static WORLD pcSquareMiddle(BYTE m) {
    return (WORLD)(((WORLD)m << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* How far west of the target a drive starts, and the ticks it is given to
   cover that square. Half a square of road takes under a dozen ticks, so the
   budget is loose enough that a slower surface still arrives and tight enough
   that a tank going nowhere returns rather than spinning. */
#define PC_APPROACH_SQUARES 1
#define PC_DRIVE_TICKS      600

/* A square with room to start the drive west of it and to carry on east
   past it, both still on the map. */
static bool pcHasApproach(BYTE x, BYTE y) {
    return x > (BYTE)(PC_APPROACH_SQUARES + 1) && x < 253 && y > 0 && y < 255;
}

/* Lay a short run of road through the target square, the square the drive
   starts from and one beyond, with a row either side, so the tank crosses
   known ground and the building nudge has nothing to push against. Terrain
   only: the base or pill standing on the square is untouched, and so is its
   owner and armour. */
static void pcPaveRun(ServerSim *sim, BYTE tx, BYTE ty) {
    int dx, dy;

    for (dy = -1; dy <= 1; dy++) {
        for (dx = -(PC_APPROACH_SQUARES + 1); dx <= 2; dx++) {
            mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)(tx + dx),
                      (BYTE)(ty + dy), ROAD, FALSE, FALSE);
        }
    }
}

/* Drive the slot-0 tank onto (tx,ty) under its own movement.
 *
 * Both capture blocks sit in the NotOnBoat arm of tankMoveUnified's last
 * step, so they are only reached by a tank that is ashore and being ticked
 * through the movement path. A tank put on the square by hand reaches
 * neither: tankSetWorld moves it and nothing else, and every tank is created
 * afloat, so the tick takes the InBoat arm instead and the squares it is
 * standing on are never tested.
 *
 * The tank therefore starts a square west at rest, ashore and facing east,
 * and holds accelerate until it crosses the boundary. The capture runs on the
 * tick that carries it over. Returns whether it arrived, so a caller can say
 * so rather than failing on an assertion further down. */
static bool pcDriveOnto(ServerSim *sim, BYTE tx, BYTE ty) {
    tank *t = &sim->sim.tanks[PC_SHOOTER];
    int ticks;

    pcPaveRun(sim, tx, ty);
    tankSetWorld(&sim->sim, t,
                 pcSquareMiddle((BYTE)(tx - PC_APPROACH_SQUARES)),
                 pcSquareMiddle(ty), (TURNTYPE)BRADIANS_EAST, FALSE);
    tankSetSpeed(t, 0);
    tankSetOnBoat(t, FALSE);
    tankClearBoatTrail(t);

    for (ticks = 0; ticks < PC_DRIVE_TICKS; ticks++) {
        if (tankGetMX(t) == tx && tankGetMY(t) == ty) {
            return true;
        }
        tankUpdate(&sim->sim, t, TACCEL, FALSE, FALSE);
    }
    return tankGetMX(t) == tx && tankGetMY(t) == ty;
}

/* Keep driving east until the tank is off the square it is on, or the budget
   runs out. What a refused capture must not do is stop it. */
static bool pcDriveOnPast(ServerSim *sim, BYTE tx) {
    tank *t = &sim->sim.tanks[PC_SHOOTER];
    int ticks;

    for (ticks = 0; ticks < PC_DRIVE_TICKS; ticks++) {
        if (tankGetMX(t) != tx) {
            return true;
        }
        tankUpdate(&sim->sim, t, TACCEL, FALSE, FALSE);
    }
    return tankGetMX(t) != tx;
}

/* One shell from slot 0 landing on the slot-1 tank. */
static void pcShoot(ServerSim *sim) {
    WORLD tx, ty;
    tankGetWorld(&sim->sim.tanks[PC_TARGET], &tx, &ty);
    tankIsTankHit(&sim->sim, &sim->sim.tanks[PC_TARGET], tx, ty, (TURNTYPE)0,
                  PC_SHOOTER);
}

/* Shells until the slot-1 tank is destroyed, capped so a tank that never
   dies returns rather than spinning. */
static int pcHitsToDestroy(ServerSim *sim) {
    int hits = 0;
    while (hits < 512 && !tankIsDestroyed(&sim->sim.tanks[PC_TARGET])) {
        pcShoot(sim);
        hits++;
    }
    return hits;
}

/* One mine going off under the slot-1 tank, laid by slot 0. */
static void pcMine(ServerSim *sim) {
    tankMineDamage(&sim->sim, &sim->sim.tanks[PC_TARGET],
                   tankGetMX(&sim->sim.tanks[PC_TARGET]),
                   tankGetMY(&sim->sim.tanks[PC_TARGET]), PC_SHOOTER);
}

/* Put the slot-1 tank on a deep-sea square and run one tick, which is the
   drowning arm's whole input.
 *
 * That arm sits in tankUpdate ahead of the movement branch, so unlike the
 * capture blocks it does not need the tank to drive anywhere. What it does
 * need is a tank that is not afloat, and every tank is created afloat, so the
 * boat comes off here rather than being left to whatever an earlier leg
 * happened to do to it. Returns false if the map has no deep sea, or if the
 * tank did not end up on the square. */
static bool pcIntoDeepSea(ServerSim *sim) {
    tank *t = &sim->sim.tanks[PC_TARGET];
    BYTE sx = 0, sy = 0;

    if (!pcFindTile(sim, DEEP_SEA, &sx, &sy)) {
        return false;
    }
    tankSetWorld(&sim->sim, t, pcSquareMiddle(sx), pcSquareMiddle(sy),
                 (TURNTYPE)0, FALSE);
    tankSetSpeed(t, 0);
    tankSetOnBoat(t, FALSE);
    tankClearBoatTrail(t);
    if (tankGetMX(t) != sx || tankGetMY(t) != sy) {
        return false;
    }
    tankUpdate(&sim->sim, t, TNONE, FALSE, FALSE);
    return true;
}

/* Stand the slot-0 builder on a square, out of the tank and alive. */
static bool pcBuilderOut(ServerSim *sim, BYTE *ox, BYTE *oy) {
    lgm *l = &sim->sim.lgmen[PC_SHOOTER];
    BYTE gx = 0, gy = 0;

    if (!pcFindTile(sim, GRASS, &gx, &gy)) {
        return false;
    }
    (*l)->inTank = FALSE;
    (*l)->isDead = FALSE;
    (*l)->x = pcSquareMiddle(gx);
    (*l)->y = pcSquareMiddle(gy);
    *ox = gx;
    *oy = gy;
    return true;
}

/* An explosion right on the builder, which is what every builder death in
   the engine comes down to. */
static void pcBlastBuilder(ServerSim *sim) {
    lgm *l = &sim->sim.lgmen[PC_SHOOTER];
    lgmDeathCheck(&sim->sim, l, (*l)->x, (*l)->y, NEUTRAL,
                  &sim->sim.tanks[PC_SHOOTER]);
}

/* The first live pill on the map with room to be driven onto, by index.
   Returns -1 if there is none. */
static int pcFirstPill(ServerSim *sim) {
    BYTE n = pillsGetNumPills(&sim->sim.pb);
    BYTE i;
    for (i = 0; i < n; i++) {
        if ((*sim->sim.pb).active[i] != FALSE &&
            (*sim->sim.pb).item[i].inTank == FALSE &&
            pcHasApproach((*sim->sim.pb).item[i].x,
                          (*sim->sim.pb).item[i].y)) {
            return (int)i;
        }
    }
    return -1;
}

/* The first live base on the map with room to be driven onto, handed to
   nobody so it is one a tank may drive over and take. Returns -1 if the map
   has none. */
static int pcNeutralBase(ServerSim *sim) {
    BYTE n = basesGetNumBases(&sim->sim.bs);
    BYTE i;
    for (i = 0; i < n; i++) {
        if ((*sim->sim.bs).active[i] != FALSE &&
            pcHasApproach((*sim->sim.bs).item[i].x,
                          (*sim->sim.bs).item[i].y)) {
            (*sim->sim.bs).item[i].owner = NEUTRAL;
            return (int)i;
        }
    }
    return -1;
}

/* True if slot 0 has been sent the given assistant message id. Nothing here
   runs a tick, so events accumulate across a whole case. */
static bool pcSawAssist(ServerSim *sim, uint8_t msgId) {
    uint8_t i;
    for (i = 0; i < sim->eventCount; i++) {
        if (sim->events[i].type == EVENT_ASSISTANT_MSG &&
            sim->events[i].data[1] == msgId) {
            return true;
        }
    }
    return false;
}

/* A build order the player's own click reaches, so the assistant speaks. */
static void pcOrder(ServerSim *sim, BYTE action, BYTE x, BYTE y) {
    lgmAddRequest(&sim->sim, &sim->sim.lgmen[PC_SHOOTER],
                  &sim->sim.tanks[PC_SHOOTER], x, y, action);
}

/* The same order asked as a question: no dispatch, no spending, no line. */
static bool pcAsk(ServerSim *sim, BYTE action, BYTE x, BYTE y) {
    return lgmRequestIsValid(&sim->sim, &sim->sim.lgmen[PC_SHOOTER],
                             &sim->sim.tanks[PC_SHOOTER], x, y, action);
}

static BYTE pcRefusal(ServerSim *sim, BYTE action, BYTE x, BYTE y) {
    return lgmRequestRefusal(&sim->sim, &sim->sim.lgmen[PC_SHOOTER],
                             &sim->sim.tanks[PC_SHOOTER], x, y, action);
}

/* ================================================================
 * 1. The damage scale is a factor on every blow.
 *
 * One helper prices all three damage paths, so the percent is read once
 * here and then watched doing its work through a shell and through a mine.
 * ================================================================ */
int run_scenario_policy_damage_scale(void) {
    ServerSim *sim = pcTwoTankSim();
    ScenarioPolicy pol;
    PcCtx pc;
    GameSim *gs;
    int classicHits, bossHits;
    BYTE armourBefore;

    UT_ASSERT_MSG(sim != NULL, "pcTwoTankSim returned NULL");
    gs = &sim->sim;
    UT_ASSERT(gs->tanks[PC_SHOOTER] != NULL && gs->tanks[PC_TARGET] != NULL);

    /* A hundred is the classic amount and must change nothing. */
    pcFillPolicy(&pol, &pc);
    serverSimSetScenarioPolicy(sim, &pol);
    UT_ASSERT_MSG(tankDamageAmount(gs, DAMAGE, PC_SHOOTER, PC_TARGET,
                                   LAST_DEATH_BY_SHELL) == DAMAGE,
                  "a scale of a hundred gave %d, expected the classic %d",
                  (int)tankDamageAmount(gs, DAMAGE, PC_SHOOTER, PC_TARGET,
                                        LAST_DEATH_BY_SHELL),
                  DAMAGE);
    UT_ASSERT_MSG(pc.scaleAsks > 0, "the scale must actually be asked");
    UT_ASSERT_MSG(pc.lastScaleAttacker == PC_SHOOTER &&
                  pc.lastScaleVictim == PC_TARGET,
                  "the scale was asked about attacker %u and victim %u, wanted"
                  " %u and %u", (unsigned)pc.lastScaleAttacker,
                  (unsigned)pc.lastScaleVictim, (unsigned)PC_SHOOTER,
                  (unsigned)PC_TARGET);
    classicHits = pcHitsToDestroy(sim);
    UT_ASSERT_MSG(classicHits > 1 && classicHits < 512,
                  "the classic run took %d hits", classicHits);
    serverSimDestroy(sim);

    /* Fifty halves the blow, so the boss takes more hits. DAMAGE is 5, and
       5 * 50 / 100 is 2.5 rounded half up. */
    sim = pcTwoTankSim();
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    pcFillPolicy(&pol, &pc);
    pc.scalePct = 50;
    serverSimSetScenarioPolicy(sim, &pol);
    UT_ASSERT_MSG(tankDamageAmount(gs, DAMAGE, PC_SHOOTER, PC_TARGET,
                                   LAST_DEATH_BY_SHELL) == 3,
                  "a scale of fifty gave %d, expected 3",
                  (int)tankDamageAmount(gs, DAMAGE, PC_SHOOTER, PC_TARGET,
                                        LAST_DEATH_BY_SHELL));
    bossHits = pcHitsToDestroy(sim);
    UT_ASSERT_MSG(bossHits > classicHits,
                  "the boss took %d hits, classic took %d",
                  bossHits, classicHits);
    serverSimDestroy(sim);

    /* Zero is a hit that does nothing at all, through both damage paths. */
    sim = pcTwoTankSim();
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    pcFillPolicy(&pol, &pc);
    pc.scalePct = 0;
    serverSimSetScenarioPolicy(sim, &pol);
    UT_ASSERT_MSG(tankDamageAmount(gs, DAMAGE, PC_SHOOTER, PC_TARGET,
                                   LAST_DEATH_BY_SHELL) == 0,
                  "a scale of zero must price a shell at nothing");
    UT_ASSERT_MSG(tankDamageAmount(gs, MINE_DAMAGE, PC_SHOOTER, PC_TARGET,
                                   LAST_DEATH_BY_MINES) == 0,
                  "a scale of zero must price a mine at nothing");

    armourBefore = tankGetArmour(&gs->tanks[PC_TARGET]);
    UT_ASSERT_MSG(armourBefore > 0, "setup: the target must start with armour");
    {
        int i;
        for (i = 0; i < 100; i++) {
            pcShoot(sim);
        }
    }
    pcMine(sim);
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[PC_TARGET]) == armourBefore,
                  "the armour bar moved from %u to %u under a scale of zero",
                  (unsigned)armourBefore,
                  (unsigned)tankGetArmour(&gs->tanks[PC_TARGET]));
    UT_ASSERT_MSG(!tankIsDestroyed(&gs->tanks[PC_TARGET]),
                  "a tank taking no damage must still be alive");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. An invulnerable tank, at all three of its death sites.
 *
 * A hundred shells and a mine reach the destroy branch of tankApplyDamage;
 * deep sea reaches the drowning arm, which is not damage and so has its own
 * question. Each leg then flips the answer, so the site is shown to be real
 * rather than merely unreached.
 * ================================================================ */
int run_scenario_policy_invulnerable_tank(void) {
    ServerSim *sim = pcTwoTankSim();
    ScenarioPolicy pol;
    PcCtx pc;
    GameSim *gs;
    int i;

    UT_ASSERT_MSG(sim != NULL, "pcTwoTankSim returned NULL");
    gs = &sim->sim;
    UT_ASSERT(gs->tanks[PC_SHOOTER] != NULL && gs->tanks[PC_TARGET] != NULL);

    pcFillPolicy(&pol, &pc);
    pc.die = false;
    serverSimSetScenarioPolicy(sim, &pol);

    for (i = 0; i < 100; i++) {
        pcShoot(sim);
    }
    UT_ASSERT_MSG(!tankIsDestroyed(&gs->tanks[PC_TARGET]),
                  "a hundred shells destroyed a tank that may not die");
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[PC_TARGET]) == 0,
                  "a refused death leaves the tank at zero armour, is %u",
                  (unsigned)tankGetArmour(&gs->tanks[PC_TARGET]));
    UT_ASSERT_MSG(pc.dieAsks > 0, "the shell site must ask");
    UT_ASSERT_MSG(pc.lastDieKind == DIE_KIND_TANK &&
                  pc.lastDieIndex == PC_TARGET &&
                  pc.lastDieCause == LAST_DEATH_BY_SHELL,
                  "the shell site asked with kind %u index %u cause %u",
                  (unsigned)pc.lastDieKind, (unsigned)pc.lastDieIndex,
                  (unsigned)pc.lastDieCause);

    pcMine(sim);
    UT_ASSERT_MSG(!tankIsDestroyed(&gs->tanks[PC_TARGET]),
                  "a mine destroyed a tank that may not die");
    UT_ASSERT_MSG(pc.lastDieCause == LAST_DEATH_BY_MINES,
                  "the mine site asked with cause %u",
                  (unsigned)pc.lastDieCause);

    UT_ASSERT_MSG(pcIntoDeepSea(sim),
                  "setup: the tank must end up on a deep sea tile, and the "
                  "map must carry one");
    UT_ASSERT_MSG(pc.lastDieCause == LAST_DEATH_BY_DEEPSEA,
                  "the tank sat in deep sea and the drowning arm did not "
                  "ask; the last question carried cause %u",
                  (unsigned)pc.lastDieCause);
    UT_ASSERT_MSG(!tankIsDestroyed(&gs->tanks[PC_TARGET]),
                  "deep sea drowned a tank that may not die");

    /* Surviving is not enough: a refused drowning falls through to the
       movement arm, so the tank can still be driven. Sixty ticks of throttle
       in deep sea, whose speed cap is three, must move it; a tank that only
       sat in the water alive would be wedged there for the round. */
    {
        WORLD bx = 0, by = 0, ax = 0, ay = 0;
        tankGetWorld(&gs->tanks[PC_TARGET], &bx, &by);
        for (i = 0; i < 60; i++) {
            tankUpdate(gs, &gs->tanks[PC_TARGET], TACCEL, FALSE, FALSE);
        }
        tankGetWorld(&gs->tanks[PC_TARGET], &ax, &ay);
        UT_ASSERT_MSG(!tankIsDestroyed(&gs->tanks[PC_TARGET]),
                      "driving in deep sea drowned a tank that may not die");
        UT_ASSERT_MSG(ax != bx || ay != by,
                      "a tank refused a drowning could not be driven: the "
                      "deep-sea arm swallowed its movement");
    }

    /* The same square, the same tick, with the answer turned round. */
    pc.die = true;
    UT_ASSERT_MSG(mapGetPos(&gs->mp, tankGetMX(&gs->tanks[PC_TARGET]),
                            tankGetMY(&gs->tanks[PC_TARGET])) == DEEP_SEA,
                  "setup: the drive must have kept the tank in deep sea");
    tankUpdate(gs, &gs->tanks[PC_TARGET], TNONE, FALSE, FALSE);
    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[PC_TARGET]),
                  "deep sea must drown the tank once the host allows it");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. A protected builder survives a shell on his square.
 * ================================================================ */
int run_scenario_policy_protected_builder(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    ScenarioPolicy pol;
    PcCtx pc;
    BYTE gx = 0, gy = 0;
    lgm *l;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    l = &sim->sim.lgmen[PC_SHOOTER];
    UT_ASSERT(*l != NULL);

    pcFillPolicy(&pol, &pc);
    pc.die = false;
    serverSimSetScenarioPolicy(sim, &pol);

    UT_ASSERT_MSG(pcBuilderOut(sim, &gx, &gy),
                  "setup: the map must carry a plain grass tile");
    pcBlastBuilder(sim);
    UT_ASSERT_MSG((*l)->isDead == FALSE,
                  "a shell killed a builder the host protected");
    UT_ASSERT_MSG((*l)->inTank == FALSE,
                  "a protected builder must be left untouched, not recalled");
    UT_ASSERT_MSG(pc.dieAsks == 1, "the builder site asked %d times",
                  pc.dieAsks);
    UT_ASSERT_MSG(pc.lastDieKind == DIE_KIND_BUILDER &&
                  pc.lastDieIndex == PC_SHOOTER,
                  "the builder site asked with kind %u index %u",
                  (unsigned)pc.lastDieKind, (unsigned)pc.lastDieIndex);

    /* The same blast with the answer turned round. */
    pc.die = true;
    pcBlastBuilder(sim);
    UT_ASSERT_MSG((*l)->isDead == TRUE,
                  "the blast must kill him once the host allows it");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. A protected pill fires on at one armour.
 *
 * Both zero tests: the one shell damage takes and the one an explosion
 * takes. A pill already dead is not raised by either.
 * ================================================================ */
int run_scenario_policy_protected_pill(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioPolicy pol;
    PcCtx pc;
    int idx;
    BYTE px, py;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    idx = pcFirstPill(sim);
    UT_ASSERT_MSG(idx >= 0, "setup: the map must carry a pillbox");
    px = (*sim->sim.pb).item[idx].x;
    py = (*sim->sim.pb).item[idx].y;

    pcFillPolicy(&pol, &pc);
    pc.die = false;
    serverSimSetScenarioPolicy(sim, &pol);

    /* pillsDamagePos: the last point of armour a shell would take. */
    (*sim->sim.pb).item[idx].armour = 1;
    pillsDamagePos(&sim->sim, px, py, TRUE, TRUE, PC_SHOOTER);
    UT_ASSERT_MSG((*sim->sim.pb).item[idx].armour == 1,
                  "a shell finished a protected pill, armour is %u",
                  (unsigned)(*sim->sim.pb).item[idx].armour);
    UT_ASSERT_MSG(pillsGetArmourPos(&sim->sim.pb, px, py) == 1,
                  "the engine's own read must agree the pill is standing, "
                  "gives %u", (unsigned)pillsGetArmourPos(&sim->sim.pb, px, py));
    UT_ASSERT_MSG(pc.lastDieKind == DIE_KIND_PILL &&
                  pc.lastDieIndex == (BYTE)idx,
                  "the shell pill site asked with kind %u index %u",
                  (unsigned)pc.lastDieKind, (unsigned)pc.lastDieIndex);

    /* pillsGetDamagePos: an explosion carrying more than the pill has left. */
    (*sim->sim.pb).item[idx].armour = 3;
    pillsGetDamagePos(&sim->sim, &sim->sim.pb, px, py, TK_DAMAGE);
    UT_ASSERT_MSG((*sim->sim.pb).item[idx].armour == 1,
                  "an explosion finished a protected pill, armour is %u",
                  (unsigned)(*sim->sim.pb).item[idx].armour);

    /* A pill already at zero is not dying, so neither site raises it. */
    (*sim->sim.pb).item[idx].armour = 0;
    pillsGetDamagePos(&sim->sim, &sim->sim.pb, px, py, TK_DAMAGE);
    UT_ASSERT_MSG((*sim->sim.pb).item[idx].armour == 0,
                  "a dead pill was raised to %u by a refusal",
                  (unsigned)(*sim->sim.pb).item[idx].armour);

    /* Both sites again with the answer turned round. */
    pc.die = true;
    (*sim->sim.pb).item[idx].armour = 1;
    pillsDamagePos(&sim->sim, px, py, TRUE, TRUE, PC_SHOOTER);
    UT_ASSERT_MSG((*sim->sim.pb).item[idx].armour == 0,
                  "a shell must finish the pill once the host allows it");
    (*sim->sim.pb).item[idx].armour = 3;
    pillsGetDamagePos(&sim->sim, &sim->sim.pb, px, py, TK_DAMAGE);
    UT_ASSERT_MSG((*sim->sim.pb).item[idx].armour == 0,
                  "an explosion must finish the pill once the host allows it");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. A protected square refuses a road.
 *
 * The refusal has to be the one an impossible square gets — the same code
 * to a caller asking, and the same line to the player — and it has to cost
 * the tank nothing, because the question is put before the order is priced.
 * ================================================================ */
int run_scenario_policy_can_build(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    ScenarioPolicy pol;
    PcCtx pc;
    tank *tnk;
    int pillIdx;
    BYTE gx = 0, gy = 0, sx = 0, sy = 0;
    BYTE classicRefusal;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    tnk = &sim->sim.tanks[PC_SHOOTER];
    UT_ASSERT_MSG(pcFindTile(sim, GRASS, &gx, &gy),
                  "setup: the map must carry a plain grass tile");
    UT_ASSERT_MSG(pcFindTile(sim, DEEP_SEA, &sx, &sy),
                  "setup: the map must carry a deep sea tile");

    /* What an impossible square answers, with nothing registered. */
    tankSetTrees(tnk, PC_TREES);
    classicRefusal = pcRefusal(sim, LGM_ROAD_REQUEST, sx, sy);
    UT_ASSERT_MSG(classicRefusal == LGM_REFUSE_SQUARE,
                  "a road on deep sea gave refusal %u, expected %u",
                  (unsigned)classicRefusal, (unsigned)LGM_REFUSE_SQUARE);
    UT_ASSERT_MSG(pcAsk(sim, LGM_ROAD_REQUEST, gx, gy),
                  "a road on grass with wood should be valid");

    pcFillPolicy(&pol, &pc);
    pc.build = false;
    serverSimSetScenarioPolicy(sim, &pol);

    UT_ASSERT_MSG(!pcAsk(sim, LGM_ROAD_REQUEST, gx, gy),
                  "a protected square must refuse a road");
    UT_ASSERT_MSG(pcRefusal(sim, LGM_ROAD_REQUEST, gx, gy) == classicRefusal,
                  "a protected square gave refusal %u, the invalid square "
                  "gives %u",
                  (unsigned)pcRefusal(sim, LGM_ROAD_REQUEST, gx, gy),
                  (unsigned)classicRefusal);
    UT_ASSERT_MSG(pc.buildAsks > 0, "the build site must ask");
    UT_ASSERT_MSG(pc.lastBuildAction == LGM_ROAD_REQUEST &&
                  pc.lastBuildX == gx && pc.lastBuildY == gy,
                  "the build site asked about action %u at (%u,%u)",
                  (unsigned)pc.lastBuildAction, (unsigned)pc.lastBuildX,
                  (unsigned)pc.lastBuildY);

    /* The acting path agrees with the question, spends nothing and leaves
       the man where he was. */
    tankSetTrees(tnk, PC_TREES);
    pcOrder(sim, LGM_ROAD_REQUEST, gx, gy);
    UT_ASSERT_MSG(tankGetTrees(tnk) == PC_TREES,
                  "a refused order spent wood: %u left of %u",
                  (unsigned)tankGetTrees(tnk), (unsigned)PC_TREES);
    UT_ASSERT_MSG(sim->sim.lgmen[PC_SHOOTER]->action == LGM_IDLE,
                  "a refused order sent the man out");
    UT_ASSERT_MSG(pcSawAssist(sim, ASSIST_MSG_NO_BUILD),
                  "a refused order must give the player the usual line");

    /* And the same order goes ahead once the host allows it. */
    pc.build = true;
    UT_ASSERT_MSG(pcAsk(sim, LGM_ROAD_REQUEST, gx, gy),
                  "the road must be valid again once the host allows it");

    /* A place-pill order names the pill it would put down, counted from
       zero. The question is put before the order is priced, so the dry run
       reaches it and spends nothing; a tank carrying none names no pill at
       all, and that has to stay recognisable rather than become an index. */
    pc.lastBuildPillIdx = 0;
    pcAsk(sim, LGM_PILL_REQUEST, gx, gy);
    UT_ASSERT_MSG(pc.lastBuildAction == LGM_PILL_REQUEST,
                  "the build site saw action %u, expected a place-pill order",
                  (unsigned)pc.lastBuildAction);
    UT_ASSERT_MSG(pc.lastBuildPillIdx == LGM_NO_PILL,
                  "a tank carrying nothing named pill %u, expected the none "
                  "value %u",
                  (unsigned)pc.lastBuildPillIdx, (unsigned)LGM_NO_PILL);

    pillIdx = pcFirstPill(sim);
    UT_ASSERT_MSG(pillIdx >= 0, "setup: the map must carry a pillbox");
    /* The carry list counts from one, which is what the engine puts on it. */
    tankPutPill(&sim->sim, tnk, (BYTE)(pillIdx + 1));
    pc.lastBuildPillIdx = 0;
    pcAsk(sim, LGM_PILL_REQUEST, gx, gy);
    UT_ASSERT_MSG(pc.lastBuildPillIdx == (BYTE)pillIdx,
                  "the build site named pill %u for the pill this test "
                  "counted %d",
                  (unsigned)pc.lastBuildPillIdx, pillIdx);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 6. A locked objective keeps its owner, and does not block the tank.
 * ================================================================ */
int run_scenario_policy_can_capture(void) {
    ServerSim *sim = ut_make_running_sim("Driver");
    ScenarioPolicy pol;
    PcCtx pc;
    GameSim *gs;
    int baseIdx, pillIdx;
    BYTE bx, by, px, py;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    gs = &sim->sim;
    baseIdx = pcNeutralBase(sim);
    UT_ASSERT_MSG(baseIdx >= 0,
                  "setup: the map must carry a base clear of its edges");
    bx = (*gs->bs).item[baseIdx].x;
    by = (*gs->bs).item[baseIdx].y;

    pcFillPolicy(&pol, &pc);
    pc.capture = false;
    serverSimSetScenarioPolicy(sim, &pol);

    /* Driven over, and the base stays where it was. */
    UT_ASSERT_MSG(pcDriveOnto(sim, bx, by),
                  "the tank never drove onto base square (%u,%u); it stopped "
                  "at (%u,%u) inside %d ticks, so the capture block was "
                  "never reached and nothing below was exercised",
                  (unsigned)bx, (unsigned)by,
                  (unsigned)tankGetMX(&gs->tanks[PC_SHOOTER]),
                  (unsigned)tankGetMY(&gs->tanks[PC_SHOOTER]),
                  PC_DRIVE_TICKS);
    UT_ASSERT_MSG(pc.captureAsks > 0,
                  "the tank drove onto the base square and the base site was "
                  "not asked");
    UT_ASSERT_MSG((*gs->bs).item[baseIdx].owner == NEUTRAL,
                  "a locked base changed hands to %u",
                  (unsigned)(*gs->bs).item[baseIdx].owner);
    UT_ASSERT_MSG(!tankIsDestroyed(&gs->tanks[PC_SHOOTER]),
                  "a refused capture must not harm the tank");
    UT_ASSERT_MSG(pc.lastCaptureKind == CAPTURE_KIND_BASE &&
                  pc.lastCaptureIndex == (BYTE)baseIdx &&
                  pc.lastCapturePlayer == PC_SHOOTER,
                  "the base site asked with kind %u index %u player %u",
                  (unsigned)pc.lastCaptureKind,
                  (unsigned)pc.lastCaptureIndex,
                  (unsigned)pc.lastCapturePlayer);

    /* The refusal touches the base and nothing else: the tank drives on. */
    UT_ASSERT_MSG(pcDriveOnPast(sim, bx),
                  "a refused capture held the tank on square (%u,%u)",
                  (unsigned)bx, (unsigned)by);

    /* The same drive with the answer turned round. */
    pc.capture = true;
    UT_ASSERT_MSG(pcDriveOnto(sim, bx, by),
                  "the tank never drove back onto base square (%u,%u)",
                  (unsigned)bx, (unsigned)by);
    UT_ASSERT_MSG((*gs->bs).item[baseIdx].owner == PC_SHOOTER,
                  "the base must change hands once the host allows it, owner "
                  "is %u", (unsigned)(*gs->bs).item[baseIdx].owner);

    /* A dead pill, driven over. The pickup sits in the same arm of the same
       step as the base capture, so it wants the same drive. */
    pillIdx = pcFirstPill(sim);
    UT_ASSERT_MSG(pillIdx >= 0,
                  "setup: the map must carry a pillbox clear of its edges");
    px = (*gs->pb).item[pillIdx].x;
    py = (*gs->pb).item[pillIdx].y;
    (*gs->pb).item[pillIdx].armour = 0;

    pc.capture = false;
    pc.captureAsks = 0;
    UT_ASSERT_MSG(pcDriveOnto(sim, px, py),
                  "the tank never drove onto pill square (%u,%u); it stopped "
                  "at (%u,%u) inside %d ticks, so the pickup was never "
                  "reached",
                  (unsigned)px, (unsigned)py,
                  (unsigned)tankGetMX(&gs->tanks[PC_SHOOTER]),
                  (unsigned)tankGetMY(&gs->tanks[PC_SHOOTER]),
                  PC_DRIVE_TICKS);
    UT_ASSERT_MSG(pc.captureAsks > 0,
                  "the tank drove onto the pill square and the pill site was "
                  "not asked");
    UT_ASSERT_MSG((*gs->pb).item[pillIdx].inTank == FALSE,
                  "a locked pill was picked up anyway");
    UT_ASSERT_MSG(pc.lastCaptureKind == CAPTURE_KIND_PILL &&
                  pc.lastCaptureIndex == (BYTE)pillIdx &&
                  pc.lastCapturePlayer == PC_SHOOTER,
                  "the pill site asked with kind %u index %u player %u",
                  (unsigned)pc.lastCaptureKind,
                  (unsigned)pc.lastCaptureIndex,
                  (unsigned)pc.lastCapturePlayer);

    pc.capture = true;
    UT_ASSERT_MSG(pcDriveOnto(sim, px, py),
                  "the tank never drove back onto pill square (%u,%u)",
                  (unsigned)px, (unsigned)py);
    UT_ASSERT_MSG((*gs->pb).item[pillIdx].inTank == TRUE,
                  "the pill must be picked up once the host allows it");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 7. The kill ops do not ask canDie.
 *
 * A script that kills what it protected meant it, so both kill ops reach
 * the death bodies without the question being put. This is the case that
 * catches the question being moved inside tankKillNow or lgmKill, where it
 * would turn a script's own order down.
 * ================================================================ */
int run_scenario_policy_kill_ops_ignore_can_die(void) {
    ServerSim *sim = ut_make_running_sim("Victim");
    ScenarioPolicy pol;
    PcCtx pc;
    ScenarioOp op;
    ScnOpResult r;
    BYTE gx = 0, gy = 0;
    lgm *l;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    l = &sim->sim.lgmen[PC_SHOOTER];
    UT_ASSERT(*l != NULL && sim->sim.tanks[PC_SHOOTER] != NULL);

    pcFillPolicy(&pol, &pc);
    pc.die = false;
    serverSimSetScenarioPolicy(sim, &pol);

    /* The builder first: the op wants him out of the tank and alive. */
    UT_ASSERT_MSG(pcBuilderOut(sim, &gx, &gy),
                  "setup: the map must carry a plain grass tile");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LGM_KILL;
    op.u.lgmKill.slot = PC_SHOOTER;
    op.u.lgmKill.killer = SCN_NONE;
    r = serverSimApplyScenarioOp(sim, &op, NULL);
    UT_ASSERT_MSG(r == SCN_OP_OK, "the builder kill op returned %d", (int)r);
    UT_ASSERT_MSG((*l)->isDead == TRUE,
                  "the builder kill op was turned down by the policy");

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_KILL;
    op.u.tankKill.slot = PC_SHOOTER;
    op.u.tankKill.killer = SCN_NONE;
    op.u.tankKill.cause = LAST_DEATH_BY_SCRIPT;
    r = serverSimApplyScenarioOp(sim, &op, NULL);
    UT_ASSERT_MSG(r == SCN_OP_OK, "the tank kill op returned %d", (int)r);
    UT_ASSERT_MSG(tankIsDestroyed(&sim->sim.tanks[PC_SHOOTER]),
                  "the tank kill op was turned down by the policy");

    UT_ASSERT_MSG(pc.dieAsks == 0,
                  "the kill ops put the death question %d times; they must "
                  "not put it at all", pc.dieAsks);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 8. With nothing registered every combat decision is the classic one.
 * ================================================================ */
int run_scenario_policy_combat_null_is_classic(void) {
    ServerSim *sim = pcTwoTankSim();
    GameSim *gs;
    tank *tnk;
    int baseIdx, pillIdx;
    BYTE gx = 0, gy = 0, bx, by, px, py;

    UT_ASSERT_MSG(sim != NULL, "pcTwoTankSim returned NULL");
    UT_ASSERT_MSG(sim->scenarioPolicy == NULL,
                  "setup: a fresh sim must carry no policy");
    gs = &sim->sim;
    tnk = &gs->tanks[PC_SHOOTER];

    /* The damage a blow does is the classic amount, both kinds. */
    UT_ASSERT_MSG(tankDamageAmount(gs, DAMAGE, PC_SHOOTER, PC_TARGET,
                                   LAST_DEATH_BY_SHELL) == DAMAGE,
                  "a shell must still do %d", DAMAGE);
    UT_ASSERT_MSG(tankDamageAmount(gs, MINE_DAMAGE, PC_SHOOTER, PC_TARGET,
                                   LAST_DEATH_BY_MINES) == MINE_DAMAGE,
                  "a mine must still do %d", MINE_DAMAGE);

    /* A build order on a square that can take it still can. */
    UT_ASSERT_MSG(pcFindTile(sim, GRASS, &gx, &gy),
                  "setup: the map must carry a plain grass tile");
    tankSetTrees(tnk, PC_TREES);
    UT_ASSERT_MSG(pcAsk(sim, LGM_ROAD_REQUEST, gx, gy),
                  "a road on grass with wood must still be valid");

    /* A neutral base driven over still changes hands. */
    baseIdx = pcNeutralBase(sim);
    UT_ASSERT_MSG(baseIdx >= 0,
                  "setup: the map must carry a base clear of its edges");
    bx = (*gs->bs).item[baseIdx].x;
    by = (*gs->bs).item[baseIdx].y;
    UT_ASSERT_MSG(pcDriveOnto(sim, bx, by),
                  "the tank never drove onto base square (%u,%u); it stopped "
                  "at (%u,%u) inside %d ticks, so the capture block was "
                  "never reached",
                  (unsigned)bx, (unsigned)by,
                  (unsigned)tankGetMX(tnk), (unsigned)tankGetMY(tnk),
                  PC_DRIVE_TICKS);
    UT_ASSERT_MSG((*gs->bs).item[baseIdx].owner == PC_SHOOTER,
                  "a neutral base driven over must still be captured, owner "
                  "is %u", (unsigned)(*gs->bs).item[baseIdx].owner);

    /* The last point of a pill's armour is still the end of it, and the
       husk it leaves is still picked up by a tank driving over it. One pill
       carries both, in the order the game itself puts them. */
    pillIdx = pcFirstPill(sim);
    UT_ASSERT_MSG(pillIdx >= 0,
                  "setup: the map must carry a pillbox clear of its edges");
    px = (*gs->pb).item[pillIdx].x;
    py = (*gs->pb).item[pillIdx].y;
    (*gs->pb).item[pillIdx].armour = 1;
    pillsDamagePos(gs, px, py, TRUE, TRUE, PC_SHOOTER);
    UT_ASSERT_MSG((*gs->pb).item[pillIdx].armour == 0,
                  "a shell must still finish a pill at one armour");

    UT_ASSERT_MSG(pcDriveOnto(sim, px, py),
                  "the tank never drove onto pill square (%u,%u); it stopped "
                  "at (%u,%u) inside %d ticks, so the pickup was never "
                  "reached",
                  (unsigned)px, (unsigned)py,
                  (unsigned)tankGetMX(tnk), (unsigned)tankGetMY(tnk),
                  PC_DRIVE_TICKS);
    UT_ASSERT_MSG((*gs->pb).item[pillIdx].inTank == TRUE,
                  "a dead pill driven over must still be picked up");

    /* And enough shells still destroy a tank. */
    UT_ASSERT_MSG(pcHitsToDestroy(sim) < 512,
                  "a tank must still be destroyable");

    serverSimDestroy(sim);
    return 0;
}
