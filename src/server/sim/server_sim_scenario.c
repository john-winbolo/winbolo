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
 */

/*********************************************************
 *Name:          Server Simulation Scenario Funnel
 *Filename:      server_sim_scenario.c
 *Author:        John Morrison
 *Purpose:
 *  The one door a scenario writes the world through, plus
 *  the registrations that attach a scenario to a sim: the
 *  policy vtable, the per-tick callback and the host's
 *  opaque state.
 *
 *  Every op arrives here, is checked against the two
 *  re-entrancy rules, and is then dispatched to its arm.
 *  An op whose arm has not been written is refused with
 *  SCN_OP_UNSUPPORTED rather than quietly doing nothing.
 *********************************************************/

#include <assert.h>

#include "server_sim_internal.h"
#include "server_sim_scenario.h"
#include "channel_mux.h"   /* CHANNEL_CONTROL_SEG — the panel cap is derived from it */
#include "tank.h"          /* the tank arms mutate through these */
#include "lgm.h"           /* the builder arms mutate through these */
#include "bolo_map.h"      /* mapGetPos — the terrain the tank arms test */
#include "pillbox.h"       /* the pill reads the give and drop arms make */
#include "starts.h"        /* startsGetStart — the teleport arm's start mode */
#include "gametype.h"      /* TANK_FULL_* — the stock caps */
#include "log.h"           /* logAddEvent — the arm's record */

/* SCN_PANEL_MAX is written as a literal on the scenario surface, which
 * cannot see the channel sizes. This is where the two meet: one panel
 * list plus the panel event's header plus the segment's own framing has
 * to fit a single control segment, which is what channel_mux rejects a
 * larger message against. */
BOLO_STATIC_ASSERT(
    1 + 2 + 1 + 1 + 2 + SCN_PANEL_MAX <= CHANNEL_CONTROL_SEG,
    scn_panel_max_fits_one_control_segment);

/* The two checks every tank arm makes before it looks at its own payload:
 * the round is running, and the slot names a connected player with a tank in
 * the world. Hands the tank back so the arm does not look it up again.
 * Checked before anything else so the two refusals do not depend on whether a
 * tank happens to exist in a state that has none. */
static ScnOpResult scenarioTankFor(ServerSim *sim, BYTE slot, tank **out) {
    if (sim->state != serverStateRunning) {
        return SCN_OP_WRONG_STATE;
    }
    if (slot >= MAX_TANKS || !sim->playerConnected[slot] ||
        sim->sim.tanks[slot] == NULL) {
        return SCN_OP_NO_SUCH_PLAYER;
    }
    *out = &sim->sim.tanks[slot];
    return SCN_OP_OK;
}

/* What every builder arm needs: the checks above, plus the builder object
 * itself. A connected player between rounds has a seat and no man in it, so
 * the two are asked for separately and the arm gets both or neither. */
static ScnOpResult scenarioBuilderFor(ServerSim *sim, BYTE slot, lgm **outMan,
                                      tank **outTank) {
    ScnOpResult r = scenarioTankFor(sim, slot, outTank);
    if (r != SCN_OP_OK) {
        return r;
    }
    if (sim->sim.lgmen[slot] == NULL) {
        return SCN_OP_NO_SUCH_PLAYER;
    }
    *outMan = &sim->sim.lgmen[slot];
    return SCN_OP_OK;
}

/* What one stock should end up at, or -1 to leave it alone. Absolute takes
 * the value as written and refuses one past the cap, because a script that
 * asks for 500 shells has made a mistake worth telling it about. A delta is
 * allowed to aim outside the range and lands on the nearest end of it, which
 * is what "give them ten more" should do to a nearly full tank. */
static int scenarioStockTarget(BYTE mode, int16_t asked, BYTE current,
                               BYTE cap, bool *bad) {
    int want;
    if (mode == SCN_STOCK_ABSOLUTE) {
        if (asked == -1) {
            return -1;
        }
        if (asked < 0 || asked > (int)cap) {
            *bad = true;
            return -1;
        }
        return (int)asked;
    }
    if (asked == 0) {
        return -1;
    }
    want = (int)current + (int)asked;
    if (want < 0) {
        want = 0;
    } else if (want > (int)cap) {
        want = cap;
    }
    return want;
}

/* Write a tank's stocks, either as the values to hold or as amounts to add.
 * Nothing is published: the next snapshot carries the group, and the tick's
 * own pass writes the stock record. */
static ScnOpResult scenarioOpTankSetStocks(ServerSim *sim,
                                           const ScnOpTankSetStocks *p) {
    tank *t = NULL;
    ScnOpResult r = scenarioTankFor(sim, p->slot, &t);
    int shells, mines, armour, trees;
    bool bad = false;

    if (r != SCN_OP_OK) {
        return r;
    }
    if (p->mode != SCN_STOCK_ABSOLUTE && p->mode != SCN_STOCK_DELTA) {
        return SCN_OP_RANGE;
    }

    shells = scenarioStockTarget(p->mode, p->shells, tankGetShells(t),
                                 TANK_FULL_SHELLS, &bad);
    mines  = scenarioStockTarget(p->mode, p->mines,  tankGetMines(t),
                                 TANK_FULL_MINES,  &bad);
    armour = scenarioStockTarget(p->mode, p->armour, tankGetArmour(t),
                                 TANK_FULL_ARMOUR, &bad);
    trees  = scenarioStockTarget(p->mode, p->trees,  tankGetTrees(t),
                                 TANK_FULL_TREES,  &bad);
    if (bad) {
        return SCN_OP_RANGE;
    }
    /* A destroyed tank takes no armour, the same answer tankAddArmour gives.
       Asked before the first write so the other three do not land on a tank
       the op is about to refuse. */
    if (armour >= 0 && tankIsDestroyed(t)) {
        return SCN_OP_TANK_DEAD;
    }

    if (shells >= 0) {
        tankSetShells(t, (BYTE)shells);
    }
    if (mines >= 0) {
        tankSetMines(t, (BYTE)mines);
    }
    if (armour >= 0) {
        tankSetArmour(t, (BYTE)armour);
    }
    if (trees >= 0) {
        tankSetTrees(t, (BYTE)trees);
    }
    return SCN_OP_OK;
}

/* Kill a tank where it stands. The death goes through tankKillNow, so the
 * kill event, the pills it was carrying and the wait before it comes back
 * are the ones a drowning produces. */
static ScnOpResult scenarioOpTankKill(ServerSim *sim,
                                      const ScnOpTankKill *p) {
    tank *t = NULL;
    ScnOpResult r = scenarioTankFor(sim, p->slot, &t);
    BYTE killer;

    if (r != SCN_OP_OK) {
        return r;
    }
    if (tankIsDestroyed(t)) {
        return SCN_OP_TANK_DEAD;
    }
    /* A death nobody caused is credited to the dying tank, as drowning is, so
       the kill event and the WinBolo.net report never carry a slot that is
       not a player. A named killer has to be one. */
    killer = (p->killer == SCN_NONE) ? p->slot : p->killer;
    if (killer >= MAX_TANKS || !sim->playerConnected[killer]) {
        return SCN_OP_NO_SUCH_PLAYER;
    }

    tankSetLastTankDeath(t, p->cause);
    tankKillNow(&sim->sim, t, killer, p->cause);
    return SCN_OP_OK;
}

/* Put a tank on a square, or on one of the map's starts. The position lands
 * at the middle of the square, as a respawn's does, and the tank arrives
 * stopped and afloat or not according to what it is standing on.
 *
 * The two modes are checked differently on purpose. A square named by a
 * script is a square nobody has vetted, so the terrain is tested. A start is
 * resolved by the engine into a square it would itself spawn a tank on, so
 * there is nothing left to test — and testing it anyway would refuse every
 * start there is, because a start record sits on deep sea. */
static ScnOpResult scenarioOpTankTeleport(ServerSim *sim,
                                          const ScnOpTankTeleport *p) {
    tank *t = NULL;
    ScnOpResult r = scenarioTankFor(sim, p->slot, &t);
    BYTE mx, my, pos;
    TURNTYPE angle;
    bool resolved = false;   /* the destination came from the start resolver */

    if (r != SCN_OP_OK) {
        return r;
    }
    if (tankIsDestroyed(t)) {
        return SCN_OP_TANK_DEAD;
    }

    if (p->mode == SCN_TELEPORT_SQUARE) {
        mx = p->x;
        my = p->y;
        angle = (p->dir == SCN_NONE) ? tankGetAngle(t) : (TURNTYPE)p->dir;
    } else if (p->mode == SCN_TELEPORT_START) {
        BYTE sx = 0;
        BYTE sy = 0;
        TURNTYPE sdir = 0.0f;

        if (startsGetNumStarts(&sim->sim.ss) == 0) {
            return SCN_OP_NO_SUCH_ITEM;
        }
        if (p->start != SCN_NONE &&
            p->start >= startsGetNumStarts(&sim->sim.ss)) {
            return SCN_OP_NO_SUCH_ITEM;
        }

        /* startsGetStart is the only thing that turns a start record into a
           square a tank can stand on: the record itself names deep sea, and
           the square the tank arrives at is the nearest free one the scatter
           search finds around it. inStartFind keeps the rest of the sim from
           acting on a tank that is between places, as the respawn path does. */
        if (p->start == SCN_NONE) {
            sim->sim.inStartFind = TRUE;
            startsGetStart(&sim->sim, &sim->sim.ss, &sx, &sy, &sdir, p->slot);
            sim->sim.inStartFind = FALSE;
        } else {
            /* Naming a start is asking for that same resolution on a chosen
               record, which is what the reserved-start slot already means to
               startsGetStart. The slot is borrowed and handed back: a start
               reserved for this player's next respawn is not this op's to
               spend. */
            BYTE reserved = sim->sim.pendingStartIdx[p->slot];
            sim->sim.pendingStartIdx[p->slot] = p->start;
            sim->sim.inStartFind = TRUE;
            startsGetStart(&sim->sim, &sim->sim.ss, &sx, &sy, &sdir, p->slot);
            sim->sim.inStartFind = FALSE;
            sim->sim.pendingStartIdx[p->slot] = reserved;
        }

        mx = sx;
        my = sy;
        angle = (p->dir == SCN_NONE) ? sdir : (TURNTYPE)p->dir;
        resolved = true;
    } else {
        return SCN_OP_RANGE;
    }

    pos = mapGetPos(&sim->sim.mp, mx, my);
    if (!resolved) {
        if (mx <= MAP_MINE_EDGE_LEFT || mx >= MAP_MINE_EDGE_RIGHT ||
            my <= MAP_MINE_EDGE_TOP || my >= MAP_MINE_EDGE_BOTTOM) {
            return SCN_OP_BAD_SQUARE;
        }
        if (pos == DEEP_SEA || pos == BUILDING || pos == HALFBUILDING) {
            return SCN_OP_BAD_TERRAIN;
        }
        /* A dead pill is drivable and can be picked up; a live one is solid,
           the same split mapGetSpeed makes. */
        if (pillsExistPos(&sim->sim.pb, mx, my) == TRUE &&
            pillsDeadPos(&sim->sim.pb, mx, my) == FALSE) {
            return SCN_OP_BAD_TERRAIN;
        }
    }

    tankSetWorld(&sim->sim, t,
                 (WORLD)(((WORLD)mx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                 (WORLD)(((WORLD)my << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                 angle, FALSE);
    tankSetSpeed(t, 0);
    tankClearResidualSpeed(t);
    /* Water of any kind carries a boat, and a tank left standing on deep sea
       without one drowns on the next tick. A start always lands on deep sea,
       so this is what puts the arriving tank in its boat the way a respawn
       does. */
    tankSetOnBoat(t, (pos == DEEP_SEA || pos == RIVER || pos == BOAT));
    tankClearBoatTrail(t);
    return SCN_OP_OK;
}

/* Put a tank on a boat or take it off one. Going afloat needs water under it,
 * because a boat on grass is a tank that can drive anywhere at boat speed. */
static ScnOpResult scenarioOpTankSetBoat(ServerSim *sim,
                                         const ScnOpTankSetBoat *p) {
    tank *t = NULL;
    ScnOpResult r = scenarioTankFor(sim, p->slot, &t);

    if (r != SCN_OP_OK) {
        return r;
    }
    if (tankIsDestroyed(t)) {
        return SCN_OP_TANK_DEAD;
    }
    if (p->onBoat) {
        BYTE pos = mapGetPos(&sim->sim.mp, tankGetMX(t), tankGetMY(t));
        if (pos != RIVER && pos != BOAT) {
            return SCN_OP_BAD_TERRAIN;
        }
    }

    tankSetOnBoat(t, p->onBoat);
    /* The square the tank was last afloat on turns back into a boat when it
       lands. It never sailed from wherever it is now, so forget it. */
    tankClearBoatTrail(t);
    return SCN_OP_OK;
}

/* Hand a pill to a tank. Unlike driving over one this does not ask whether
 * the pill is capturable: a script may take an armoured pill with a man
 * standing on it, which is the point of being able to hand one over. */
static ScnOpResult scenarioOpTankGivePill(ServerSim *sim,
                                          const ScnOpTankGivePill *p) {
    tank *t = NULL;
    ScnOpResult r = scenarioTankFor(sim, p->slot, &t);
    pillbox item;
    BYTE pillNum;

    if (r != SCN_OP_OK) {
        return r;
    }
    if (tankIsDestroyed(t)) {
        return SCN_OP_TANK_DEAD;
    }
    if (p->pill >= pillsGetNumPills(&sim->sim.pb)) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    pillNum = (BYTE)(p->pill + 1);   /* the pill list counts from 1 */
    memset(&item, 0, sizeof(item));
    pillsGetPill(&sim->sim.pb, &item, pillNum);
    if (item.inTank) {
        return SCN_OP_CARRIED;
    }
    /* A map holds at most as many pills as the carry list does, so today one
       tank can hold every pill there is and this never fires. It stays
       because the answer is the arm's to give if the two caps ever part. */
    if (tankGetNumCarriedPills(t) >= MAX_PILLS) {
        return SCN_OP_FULL;
    }

    tankTakePill(&sim->sim, t, pillNum);
    return SCN_OP_OK;
}

/* Drop a pill the tank is carrying onto a square, or onto the one it is
 * standing on. */
static ScnOpResult scenarioOpTankDropPill(ServerSim *sim,
                                          const ScnOpTankDropPill *p) {
    tank *t = NULL;
    ScnOpResult r = scenarioTankFor(sim, p->slot, &t);
    BYTE pillNum, mx, my;

    if (r != SCN_OP_OK) {
        return r;
    }
    if (p->pill >= pillsGetNumPills(&sim->sim.pb)) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    pillNum = (BYTE)(p->pill + 1);   /* the pill list counts from 1 */
    if (!tankIsCarryingPill(t, pillNum)) {
        return SCN_OP_NO_SUCH_ITEM;
    }

    if (p->x == SCN_NONE && p->y == SCN_NONE) {
        mx = tankGetMX(t);
        my = tankGetMY(t);
    } else {
        mx = p->x;
        my = p->y;
    }
    if (mx <= MAP_MINE_EDGE_LEFT || mx >= MAP_MINE_EDGE_RIGHT ||
        my <= MAP_MINE_EDGE_TOP || my >= MAP_MINE_EDGE_BOTTOM) {
        return SCN_OP_BAD_SQUARE;
    }
    /* tankDropPillAt makes the same square test the drop-on-death path makes
       and changes nothing when the square will not hold a pill. */
    if (!tankDropPillAt(&sim->sim, t, pillNum, mx, my)) {
        return SCN_OP_BAD_TERRAIN;
    }
    return SCN_OP_OK;
}

/* Replace a tank's whole modifier set. The op carries every value, so a
 * script that wants to change one reads the tank first. Nothing is published:
 * the next snapshot carries the group to the owning client, which is within a
 * tick. The record goes out beside the write so a replay shows the change at
 * the tick it happened. */
static ScnOpResult scenarioOpTankSetModifiers(ServerSim *sim,
                                              const ScnOpTankSetModifiers *p) {
    /* Checked before the slot so the two refusals do not depend on whether a
     * tank happens to exist in a state that has none. */
    if (sim->state != serverStateRunning) {
        return SCN_OP_WRONG_STATE;
    }
    if (p->slot >= MAX_TANKS || !sim->playerConnected[p->slot] ||
        sim->sim.tanks[p->slot] == NULL) {
        return SCN_OP_NO_SUCH_PLAYER;
    }

    tankSetModifiers(sim->sim.tanks[p->slot], &p->mods);

    {
        /* [len][speed][accel][turn][reload][dealt][taken] — the six do not fit
           logAddEvent's four opt bytes and its short. */
        char blob[7];
        blob[0] = 6;
        blob[1] = (char)p->mods.speed;
        blob[2] = (char)p->mods.accel;
        blob[3] = (char)p->mods.turn;
        blob[4] = (char)p->mods.reload;
        blob[5] = (char)p->mods.dealt;
        blob[6] = (char)p->mods.taken;
        logAddEvent(log_TankSetModifiers, p->slot, 0, 0, 0, 0, blob);
    }
    return SCN_OP_OK;
}

/* Send the builder out on a job, which is what a player does with one click.
 * The order goes through lgmAddRequest, the call that click reaches from the
 * tick, so the wood and mines it costs and the substitutions the game makes —
 * a road ordered on forest is a tree harvest, a wall ordered on a river is a
 * boat — are the ones lgmCheckNewRequest applies and are not repeated here.
 *
 * The refusal code comes from asking the same question first, without acting
 * on it. Asking is quiet: lgmRequestRefusal passes perform FALSE so nothing is
 * spent and announce FALSE so lgmAssist sends the player none of the lines a
 * refused click would earn them. */
static ScnOpResult scenarioOpLgmDispatch(ServerSim *sim,
                                         const ScnOpLgmDispatch *p) {
    lgm *l = NULL;
    tank *t = NULL;
    ScnOpResult r = scenarioBuilderFor(sim, p->slot, &l, &t);
    BYTE action;

    if (r != SCN_OP_OK) {
        return r;
    }
    if (tankIsDestroyed(t)) {
        return SCN_OP_TANK_DEAD;
    }
    /* The op names the job as a BuilderJob, whose members carry the request
       codes the engine's own path takes. builderJobNone is the absence of a
       job, so it is the one member that is not an order. */
    if (p->action >= (BYTE)builderJobNone) {
        return SCN_OP_RANGE;
    }
    action = (BYTE)(BuilderJob)p->action;
    if (p->x <= MAP_MINE_EDGE_LEFT || p->x >= MAP_MINE_EDGE_RIGHT ||
        p->y <= MAP_MINE_EDGE_TOP || p->y >= MAP_MINE_EDGE_BOTTOM) {
        return SCN_OP_BAD_SQUARE;
    }
    /* A click arriving while the man is out is held and re-asked when he is
       back in the tank. The op says no instead: a script told SCN_OP_OK for an
       order that will not start until the man has walked home, and may be
       thrown away when he does, has been told something untrue. */
    if ((*l)->isDead || !lgmIsIdle(l)) {
        return SCN_OP_ALREADY;
    }

    switch (lgmRequestRefusal(&sim->sim, l, t, p->x, p->y, action)) {
        case LGM_REFUSE_NONE:
            break;
        case LGM_REFUSE_STOCK:
            return SCN_OP_NO_STOCK;
        default:
            return SCN_OP_BAD_TERRAIN;
    }

    lgmAddRequest(&sim->sim, l, t, p->x, p->y, action);
    return SCN_OP_OK;
}

/* Call the builder back. He turns round where he stands and walks home with
 * whatever he is carrying, which unloads into the tank when he arrives. */
static ScnOpResult scenarioOpLgmRecall(ServerSim *sim,
                                       const ScnOpLgmRecall *p) {
    lgm *l = NULL;
    tank *t = NULL;
    ScnOpResult r = scenarioBuilderFor(sim, p->slot, &l, &t);

    if (r != SCN_OP_OK) {
        return r;
    }
    /* Nobody out there to call: he is in the tank, or in the air. */
    if ((*l)->inTank || (*l)->isDead) {
        return SCN_OP_ALREADY;
    }

    lgmRecall(&sim->sim, l);
    return SCN_OP_OK;
}

/* SCN_NONE and NEUTRAL are the same byte and the kill arm leans on it: a
 * builder death nobody caused is the one a mine produces, which names NEUTRAL
 * as the owner and so credits no kill. */
BOLO_STATIC_ASSERT(SCN_NONE == NEUTRAL, scn_none_is_neutral_for_an_unowned_kill);

/* Kill the builder where he stands. The death goes through lgmKill, so the
 * pill he was carrying, the record, the newswire event and the reports to
 * WinBolo.net are the ones an explosion beside him produces.
 *
 * Those reports fire on every builder death with no test of who the killer
 * is, so the arm makes sure a named killer is a player rather than putting a
 * condition inside a path the ordinary death shares. */
static ScnOpResult scenarioOpLgmKill(ServerSim *sim, const ScnOpLgmKill *p) {
    lgm *l = NULL;
    tank *t = NULL;
    ScnOpResult r = scenarioBuilderFor(sim, p->slot, &l, &t);

    if (r != SCN_OP_OK) {
        return r;
    }
    /* The two states an explosion cannot kill him in either. */
    if ((*l)->isDead || (*l)->inTank) {
        return SCN_OP_ALREADY;
    }
    if (p->killer != SCN_NONE &&
        (p->killer >= MAX_TANKS || !sim->playerConnected[p->killer])) {
        return SCN_OP_NO_SUCH_PLAYER;
    }

    lgmKill(&sim->sim, l, t, p->killer);
    return SCN_OP_OK;
}

/* Aim a builder who is already in the air. A death drops him on a random
 * start and the helicopter flies the whole way, which is most of a minute
 * across a map. Putting him on the edge nearest where he is going leaves him
 * a few ticks out, and it is still the flight the engine already knows how to
 * land — lgmParchutingIn walks him down from wherever he is. */
static ScnOpResult scenarioOpLgmParachute(ServerSim *sim,
                                          const ScnOpLgmParachute *p) {
    lgm *l = NULL;
    tank *t = NULL;
    ScnOpResult r = scenarioBuilderFor(sim, p->slot, &l, &t);
    WORLD dx, dy;
    BYTE mx, my, ex, ey;
    int toLeft, toRight, toTop, toBottom, nearest;

    if (r != SCN_OP_OK) {
        return r;
    }
    /* This arm moves a flight; it does not start one. A man on the ground is
       already where he is going. */
    if (!(*l)->isDead) {
        return SCN_OP_ALREADY;
    }

    if (p->x == SCN_NONE && p->y == SCN_NONE) {
        tankGetWorld(t, &dx, &dy);
    } else {
        if (p->x <= MAP_MINE_EDGE_LEFT || p->x >= MAP_MINE_EDGE_RIGHT ||
            p->y <= MAP_MINE_EDGE_TOP || p->y >= MAP_MINE_EDGE_BOTTOM) {
            return SCN_OP_BAD_SQUARE;
        }
        dx = (WORLD)(((WORLD)p->x << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
        dy = (WORLD)(((WORLD)p->y << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    }

    mx = (BYTE)(dx >> TANK_SHIFT_MAPSIZE);
    my = (BYTE)(dy >> TANK_SHIFT_MAPSIZE);
    toLeft   = (int)mx - MAP_MINE_EDGE_LEFT;
    toRight  = MAP_MINE_EDGE_RIGHT - (int)mx;
    toTop    = (int)my - MAP_MINE_EDGE_TOP;
    toBottom = MAP_MINE_EDGE_BOTTOM - (int)my;

    ex = (BYTE)MAP_MINE_EDGE_LEFT;
    ey = my;
    nearest = toLeft;
    if (toRight < nearest) {
        nearest = toRight;
        ex = (BYTE)MAP_MINE_EDGE_RIGHT;
        ey = my;
    }
    if (toTop < nearest) {
        nearest = toTop;
        ex = mx;
        ey = (BYTE)MAP_MINE_EDGE_TOP;
    }
    if (toBottom < nearest) {
        ex = mx;
        ey = (BYTE)MAP_MINE_EDGE_BOTTOM;
    }

    (*l)->x = (WORLD)(((WORLD)ex << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    (*l)->y = (WORLD)(((WORLD)ey << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    (*l)->destX = dx;
    (*l)->destY = dy;
    return SCN_OP_OK;
}

/* Write what the builder is carrying. Nothing is published: the counts ride
 * the snapshot's builder group and unload into the tank when he gets back. */
static ScnOpResult scenarioOpLgmSetCarried(ServerSim *sim,
                                           const ScnOpLgmSetCarried *p) {
    lgm *l = NULL;
    tank *t = NULL;
    ScnOpResult r = scenarioBuilderFor(sim, p->slot, &l, &t);

    if (r != SCN_OP_OK) {
        return r;
    }
    /* Everything he carries unloads into a tank, so a tank's caps are his.
       Asking for more is a mistake worth reporting rather than clamping, the
       same answer the tank's own stock op gives. Both are asked before either
       is written, so a bad pair leaves him as he was. */
    if (p->trees != SCN_NONE && p->trees > TANK_FULL_TREES) {
        return SCN_OP_RANGE;
    }
    if (p->mines != SCN_NONE && p->mines > TANK_FULL_MINES) {
        return SCN_OP_RANGE;
    }

    lgmSetCarried(l,
                  (p->trees == SCN_NONE) ? (*l)->numTrees : p->trees,
                  (p->mines == SCN_NONE) ? (*l)->numMines : p->mines);
    return SCN_OP_OK;
}

ScnOpResult serverSimApplyScenarioOp(ServerSim *sim, const ScenarioOp *op,
                                     ScnOpOut *out) {
    assert(sim != NULL);
    assert(op != NULL);

    /* A policy callback is a question the engine asks mid-operation. It
     * answers and nothing else: an op from inside one would mutate state
     * the caller is halfway through reading. */
    if (sim->inScenarioPolicy) {
        return SCN_OP_IN_POLICY;
    }

    /* A start is not a settled point. The roster, the tanks and the
     * state are all being rebuilt, so nothing may be written until it
     * finishes. */
    if (sim->startInProgress) {
        return SCN_OP_WRONG_STATE;
    }

    (void)out;

    switch (op->type) {
        case SCN_OP_NONE:                return SCN_OP_UNSUPPORTED;
        case SCN_OP_TANK_SET_STOCKS:
            return scenarioOpTankSetStocks(sim, &op->u.tankSetStocks);
        case SCN_OP_TANK_KILL:
            return scenarioOpTankKill(sim, &op->u.tankKill);
        case SCN_OP_TANK_TELEPORT:
            return scenarioOpTankTeleport(sim, &op->u.tankTeleport);
        case SCN_OP_TANK_SET_BOAT:
            return scenarioOpTankSetBoat(sim, &op->u.tankSetBoat);
        case SCN_OP_TANK_GIVE_PILL:
            return scenarioOpTankGivePill(sim, &op->u.tankGivePill);
        case SCN_OP_TANK_DROP_PILL:
            return scenarioOpTankDropPill(sim, &op->u.tankDropPill);
        case SCN_OP_TANK_SET_MODIFIERS:
            return scenarioOpTankSetModifiers(sim, &op->u.tankSetModifiers);
        case SCN_OP_LGM_DISPATCH:
            return scenarioOpLgmDispatch(sim, &op->u.lgmDispatch);
        case SCN_OP_LGM_RECALL:
            return scenarioOpLgmRecall(sim, &op->u.lgmRecall);
        case SCN_OP_LGM_KILL:
            return scenarioOpLgmKill(sim, &op->u.lgmKill);
        case SCN_OP_LGM_PARACHUTE:
            return scenarioOpLgmParachute(sim, &op->u.lgmParachute);
        case SCN_OP_LGM_SET_CARRIED:
            return scenarioOpLgmSetCarried(sim, &op->u.lgmSetCarried);
        case SCN_OP_PILL_SET_OWNER:      return SCN_OP_UNSUPPORTED;
        case SCN_OP_PILL_SET_ARMOUR:     return SCN_OP_UNSUPPORTED;
        case SCN_OP_PILL_SET_SPEED:      return SCN_OP_UNSUPPORTED;
        case SCN_OP_PILL_MOVE:           return SCN_OP_UNSUPPORTED;
        case SCN_OP_BASE_SET_OWNER:      return SCN_OP_UNSUPPORTED;
        case SCN_OP_BASE_SET_STOCK:      return SCN_OP_UNSUPPORTED;
        case SCN_OP_ENTITY_ADD_PILL:     return SCN_OP_UNSUPPORTED;
        case SCN_OP_ENTITY_REMOVE_PILL:  return SCN_OP_UNSUPPORTED;
        case SCN_OP_ENTITY_ADD_BASE:     return SCN_OP_UNSUPPORTED;
        case SCN_OP_ENTITY_REMOVE_BASE:  return SCN_OP_UNSUPPORTED;
        case SCN_OP_ENTITY_ADD_START:    return SCN_OP_UNSUPPORTED;
        case SCN_OP_ENTITY_REMOVE_START: return SCN_OP_UNSUPPORTED;
        case SCN_OP_MAP_SET_TILE:        return SCN_OP_UNSUPPORTED;
        case SCN_OP_MAP_FILL_RECT:       return SCN_OP_UNSUPPORTED;
        case SCN_OP_MAP_PLACE_MINE:      return SCN_OP_UNSUPPORTED;
        case SCN_OP_MAP_REMOVE_MINE:     return SCN_OP_UNSUPPORTED;
        case SCN_OP_ROSTER_SPAWN_BOT:    return SCN_OP_UNSUPPORTED;
        case SCN_OP_ROSTER_REMOVE_BOT:   return SCN_OP_UNSUPPORTED;
        case SCN_OP_ROSTER_SET_TEAM:     return SCN_OP_UNSUPPORTED;
        case SCN_OP_LOBBY_ADD_BOT:       return SCN_OP_UNSUPPORTED;
        case SCN_OP_LOBBY_REMOVE_BOT:    return SCN_OP_UNSUPPORTED;
        case SCN_OP_LOBBY_SET_TEAM:      return SCN_OP_UNSUPPORTED;
        case SCN_OP_BOT_HINT:            return SCN_OP_UNSUPPORTED;
        case SCN_OP_MSG_ALL:             return SCN_OP_UNSUPPORTED;
        case SCN_OP_MSG_TEAM:            return SCN_OP_UNSUPPORTED;
        case SCN_OP_MSG_PLAYER:          return SCN_OP_UNSUPPORTED;
        case SCN_OP_SOUND:               return SCN_OP_UNSUPPORTED;
        case SCN_OP_LOG:                 return SCN_OP_UNSUPPORTED;
        case SCN_OP_PANEL:               return SCN_OP_UNSUPPORTED;
        case SCN_OP_SCORE:               return SCN_OP_UNSUPPORTED;
        case SCN_OP_ANNOUNCE:            return SCN_OP_UNSUPPORTED;
        case SCN_OP_MARKER:              return SCN_OP_UNSUPPORTED;
        case SCN_OP_END_ROUND:           return SCN_OP_UNSUPPORTED;
        case SCN_OP_SET_GAME_TIME:       return SCN_OP_UNSUPPORTED;
        case SCN_OP_SET_RULE:            return SCN_OP_UNSUPPORTED;
    }

    /* A value that is not a member of the enum at all. */
    return SCN_OP_UNSUPPORTED;
}

void serverSimSetScenarioPolicy(ServerSim *sim, const ScenarioPolicy *p) {
    if (sim == NULL) return;
    sim->scenarioPolicy = p;
}

void serverSimSetScenarioTick(ServerSim *sim, void (*tick)(void *ctx),
                              void *ctx) {
    if (sim == NULL) return;
    sim->scenarioTick = tick;
    sim->scenarioTickCtx = ctx;
}

void serverSimSetScenarioState(ServerSim *sim, void *state) {
    if (sim == NULL) return;
    sim->scenario = state;
}

void *serverSimGetScenarioState(const ServerSim *sim) {
    if (sim == NULL) return NULL;
    return sim->scenario;
}

void serverSimScenarioPolicyEnter(ServerSim *sim) {
    if (sim == NULL) return;
    sim->inScenarioPolicy = true;
}

void serverSimScenarioPolicyLeave(ServerSim *sim) {
    if (sim == NULL) return;
    sim->inScenarioPolicy = false;
}
