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
#include "bolo_map.h"      /* mapGetPos and mapSetPos — what the map arms write through */
#include "pillbox.h"       /* the pill arms read and write through these */
#include "bases.h"         /* the base arms mutate through these */
#include "mines.h"         /* the mine list the map arms add to and clear */
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

/* What every pill arm starts with: the op's index turned into the number the
 * pill list wants, and the pill's own record, which each arm edits one field
 * of and writes back. Op indices count from zero and the list counts from one.
 *
 * No state check: a pill belongs to the map rather than to a round, so dealing
 * the estate out before the round starts is as legal as re-dealing it mid-round
 * and the lobby's own map is what a scenario sets up against. */
static ScnOpResult scenarioPillFor(ServerSim *sim, BYTE pill, BYTE *outNum,
                                   pillbox *out) {
    if (pill >= pillsGetNumPills(&sim->sim.pb)) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    *outNum = (BYTE)(pill + 1);
    memset(out, 0, sizeof(*out));
    pillsGetPill(&sim->sim.pb, out, *outNum);
    return SCN_OP_OK;
}

/* A slot or nobody. Every seat is a legal owner for a pill or a base, whether
 * or not somebody is sitting in it: a scenario hands the wave's guns back to
 * empty seats so the map is fought against them with nobody behind them. */
static bool scenarioOwnerIsLegal(BYTE owner) {
    return owner < MAX_TANKS || owner == NEUTRAL;
}

/* Whether a square will take a pill. This is the test the builder's place-pill
 * arm makes in lgmDoWork before it puts one down: no pill, no base, no mine
 * the players can see, and ground that is not a building, a half-building, a
 * river, a boat or deep sea. It is written inline there and lgm.c has no call
 * that asks it on its own, so it is repeated here. The mine subtraction is the
 * builder's too — a mined square is tested on the terrain underneath it. */
static bool scenarioSquareTakesPill(ServerSim *sim, BYTE mx, BYTE my) {
    GameSim *gs = &sim->sim;
    BYTE terrain = mapGetPos(&gs->mp, mx, my);

    if (terrain >= MINE_START && terrain <= MINE_END) {
        terrain = (BYTE)(terrain - MINE_SUBTRACT);
    }
    return pillsExistPos(&gs->pb, mx, my) == FALSE &&
           basesExistPos(&gs->bs, mx, my) == FALSE &&
           minesExistPos(&gs->mns, &gs->mp, mx, my) == FALSE &&
           terrain != BUILDING && terrain != HALFBUILDING &&
           terrain != RIVER && terrain != BOAT && terrain != DEEP_SEA;
}

/* Hand a pill to a slot. The capture goes through the setter a tank driving
 * over one reaches, so the newswire line, the capture event and the record are
 * that capture's. migrate is false because this is a hand-over players are
 * meant to hear about, not an alliance tidying itself up when someone quits. */
static ScnOpResult scenarioOpPillSetOwner(ServerSim *sim,
                                          const ScnOpPillSetOwner *p) {
    pillbox item;
    BYTE pillNum = 0;
    ScnOpResult r = scenarioPillFor(sim, p->pill, &pillNum, &item);

    if (r != SCN_OP_OK) {
        return r;
    }
    if (!scenarioOwnerIsLegal(p->owner)) {
        return SCN_OP_RANGE;
    }
    /* A pill in a tank already answers to whoever is carrying it, and dropping
       it is what hands it on. */
    if (item.inTank) {
        return SCN_OP_CARRIED;
    }

    pillsSetPillOwner(&sim->sim, &sim->sim.pb, pillNum, p->owner, FALSE);
    return SCN_OP_OK;
}

/* Write a pill's armour. Zero is a dead pill lying on the ground, which is
 * what a script wanting a wreck to repair asks for. */
static ScnOpResult scenarioOpPillSetArmour(ServerSim *sim,
                                           const ScnOpPillSetArmour *p) {
    pillbox item;
    BYTE pillNum = 0;
    ScnOpResult r = scenarioPillFor(sim, p->pill, &pillNum, &item);

    if (r != SCN_OP_OK) {
        return r;
    }
    /* pillsSetPill clamps, because a map file may carry anything. A script is
       told instead: asking for more armour than a pill can hold is a mistake
       worth reporting, the same answer the tank's own stock op gives. */
    if (p->armour > PILLS_MAX_ARMOUR) {
        return SCN_OP_RANGE;
    }
    if (item.inTank) {
        return SCN_OP_CARRIED;
    }

    item.armour = p->armour;
    pillsSetPill(&sim->sim.pb, &item, pillNum);
    return SCN_OP_OK;
}

/* Write how often a pill fires. A carried pill is allowed: it takes the new
 * rate with it and fires at it when it is put down. */
static ScnOpResult scenarioOpPillSetSpeed(ServerSim *sim,
                                          const ScnOpPillSetSpeed *p) {
    pillbox item;
    BYTE pillNum = 0;
    ScnOpResult r = scenarioPillFor(sim, p->pill, &pillNum, &item);

    if (r != SCN_OP_OK) {
        return r;
    }
    /* The attack interval runs from the fastest a hurt pill fires to the rate
       an untouched one sits at. pillsSetPill clamps into that pair and arms the
       cooldown for anything under the top of it. */
    if (p->speed < PILLBOX_MAX_FIRERATE || p->speed > PILLBOX_ATTACK_NORMAL) {
        return SCN_OP_RANGE;
    }

    item.speed = p->speed;
    pillsSetPill(&sim->sim.pb, &item, pillNum);
    return SCN_OP_OK;
}

/* Put a pill on another square. */
static ScnOpResult scenarioOpPillMove(ServerSim *sim, const ScnOpPillMove *p) {
    pillbox item;
    BYTE pillNum = 0;
    ScnOpResult r = scenarioPillFor(sim, p->pill, &pillNum, &item);

    if (r != SCN_OP_OK) {
        return r;
    }
    /* A pill in a tank is nowhere on the map, so there is no move to make. */
    if (item.inTank) {
        return SCN_OP_CARRIED;
    }
    if (p->x <= MAP_MINE_EDGE_LEFT || p->x >= MAP_MINE_EDGE_RIGHT ||
        p->y <= MAP_MINE_EDGE_TOP || p->y >= MAP_MINE_EDGE_BOTTOM) {
        return SCN_OP_BAD_SQUARE;
    }
    if (!scenarioSquareTakesPill(sim, p->x, p->y)) {
        return SCN_OP_BAD_TERRAIN;
    }

    item.x = p->x;
    item.y = p->y;
    pillsSetPill(&sim->sim.pb, &item, pillNum);
    return SCN_OP_OK;
}

/* Hand a base to a slot. The capture is the one a tank driving on to a base
 * makes, so the event and the record are that capture's, and taking a base off
 * another player empties it as a capture does. keepStock is how a script
 * re-deals the map without stripping what it deals. */
static ScnOpResult scenarioOpBaseSetOwner(ServerSim *sim,
                                          const ScnOpBaseSetOwner *p) {
    if (p->base >= basesGetNumBases(&sim->sim.bs)) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    if (!scenarioOwnerIsLegal(p->owner)) {
        return SCN_OP_RANGE;
    }

    /* migrate false, so the hand-over announces itself. The base list counts
       from one. */
    basesSetBaseOwner(&sim->sim, (BYTE)(p->base + 1), p->owner, FALSE,
                      p->keepStock ? TRUE : FALSE);
    return SCN_OP_OK;
}

/* Write what a base is holding. */
static ScnOpResult scenarioOpBaseSetStock(ServerSim *sim,
                                          const ScnOpBaseSetStock *p) {
    if (p->base >= basesGetNumBases(&sim->sim.bs)) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    /* -1 is the payload's "leave this one alone"; any other negative is a
       script that has worked something out wrong. An amount past a base's full
       load is not: basesSetStock caps it, so "fill it up" can be written as a
       number bigger than the cap. */
    if (p->armour < -1 || p->shells < -1 || p->mines < -1) {
        return SCN_OP_RANGE;
    }

    /* basesSetStock counts from zero, as basesServerRefuel beside it does. */
    basesSetStock(&sim->sim, p->base, p->armour, p->shells, p->mines);
    return SCN_OP_OK;
}

/* Where a map op may write. mapGetPos answers DEEP_SEA for anything outside
 * this box whatever the array underneath holds, so a write outside it would
 * reach every client's copy of the map and none of the sim's own reads — the
 * players would see ground the server does not believe is there. The bounds
 * are the ones the tank, builder and pill arms already refuse on. */
static bool scenarioSquareOnMap(BYTE x, BYTE y) {
    return x > MAP_MINE_EDGE_LEFT && x < MAP_MINE_EDGE_RIGHT &&
           y > MAP_MINE_EDGE_TOP && y < MAP_MINE_EDGE_BOTTOM;
}

/* Terrain a scenario may write: the sixteen map codes, the mined variants
 * among them, and deep sea. Deep sea is the one that has to be said out loud —
 * it is not a code in the 0..15 run, and a script putting open water back under
 * a pill it has taken away needs it. */
static bool scenarioTerrainIsLegal(BYTE terrain) {
    return terrain <= MINE_END || terrain == DEEP_SEA;
}

/* Write one square. Any mine under it goes first: the terrain byte being
 * written carries no mine, so a visible-mine record left behind would mark a
 * square nothing can clear and nothing would set off. mapSetPos does the rest —
 * its registered callback queues the map event and its own logAddEvent is the
 * record, so neither is repeated here. */
static void scenarioWriteTile(ServerSim *sim, BYTE x, BYTE y, BYTE terrain) {
    minesRemoveItem(&sim->sim.mns, x, y);
    mapSetPos(&sim->sim, &sim->sim.mp, x, y, terrain, TRUE, FALSE);
}

/* Put one square's terrain where the op says. */
static ScnOpResult scenarioOpMapSetTile(ServerSim *sim,
                                        const ScnOpMapSetTile *p) {
    if (!scenarioSquareOnMap(p->x, p->y)) {
        return SCN_OP_BAD_SQUARE;
    }
    if (!scenarioTerrainIsLegal(p->terrain)) {
        return SCN_OP_RANGE;
    }

    scenarioWriteTile(sim, p->x, p->y, p->terrain);
    return SCN_OP_OK;
}

/* Carry the pending fill along for as much of this tick's budget as is left.
 * Only a square whose terrain differs is written, so both the budget and the
 * publish count are counted in squares changed rather than squares looked at:
 * painting grass over grass costs a scan and nothing else.
 *
 * The tile budget is the only thing that stops the walk. Reading the frame's
 * map event buffer as a second bound looked safer and was not: that counter is
 * only cleared on a running frame, so a round that ended with it full left
 * every lobby fill writing nothing, answering queued, and never releasing the
 * funnel — after which every later fill is refused behind it, for good. The
 * buffer has its own guard at the point events are recorded, and one producer
 * second-guessing it bought nothing.
 *
 * Returns whether the rectangle is finished, and through `wrote` how many
 * squares this call changed. */
static bool scenarioFillStep(ServerSim *sim, uint16_t *wrote) {
    BYTE x = sim->scenarioFillX;
    BYTE y = sim->scenarioFillY;
    uint16_t before = sim->scenarioFillSpent;
    bool finished = true;

    while (y <= sim->scenarioFillY1) {
        bool budgetGone = false;
        while (x <= sim->scenarioFillX1) {
            if (sim->scenarioFillSpent >= SCN_TILES_PER_TICK) {
                budgetGone = true;
                break;
            }
            if (mapGetPos(&sim->sim.mp, x, y) != sim->scenarioFillTerrain) {
                scenarioWriteTile(sim, x, y, sim->scenarioFillTerrain);
                sim->scenarioFillSpent++;
            }
            x++;
        }
        if (budgetGone) {
            sim->scenarioFillX = x;
            sim->scenarioFillY = y;
            finished = false;
            break;
        }
        x = sim->scenarioFillX0;
        y++;
    }

    if (finished) {
        sim->scenarioFillPending = false;
    }
    if (wrote != NULL) {
        *wrote = (uint16_t)(sim->scenarioFillSpent - before);
    }
    return finished;
}

/* Paint a rectangle. A rectangle bigger than one tick's budget applies what it
 * can and leaves the rest on the sim, which carries it on later ticks — so the
 * answer is SCN_OP_QUEUED rather than SCN_OP_OK and the script knows the work
 * is not finished.
 *
 * One at a time: a second fill arriving while one is outstanding is refused, so
 * that the squares the first still owes are never dropped for it. A script that
 * wants both waits for the first to finish.
 *
 * A fill that cannot write a single square is refused rather than queued.
 * SCN_OP_QUEUED says the work has started and the rest is coming; answering it
 * for a fill that wrote nothing tells a script something it cannot act on, and
 * leaves a rectangle on the sim that every later fill is then refused behind.
 * SCN_OP_RATE is the same answer a second fill gets, and means the same thing:
 * the funnel had no room this tick, ask again on the next one. */
static ScnOpResult scenarioOpMapFillRect(ServerSim *sim,
                                         const ScnOpMapFillRect *p) {
    uint16_t wrote = 0;
    /* Corners either way round name the same rectangle, which is what a caller
       handing over two points it read off the map means by them. */
    BYTE x0 = (p->x0 <= p->x1) ? p->x0 : p->x1;
    BYTE x1 = (p->x0 <= p->x1) ? p->x1 : p->x0;
    BYTE y0 = (p->y0 <= p->y1) ? p->y0 : p->y1;
    BYTE y1 = (p->y0 <= p->y1) ? p->y1 : p->y0;

    if (!scenarioSquareOnMap(x0, y0) || !scenarioSquareOnMap(x1, y1)) {
        return SCN_OP_BAD_SQUARE;
    }
    if (!scenarioTerrainIsLegal(p->terrain)) {
        return SCN_OP_RANGE;
    }
    if (sim->scenarioFillPending) {
        return SCN_OP_RATE;
    }
    /* The budget this frame is already gone — spent by a fill the same hook
       finished earlier in it. Refused here, before anything is written down,
       so there is no half-started rectangle to unwind. */
    if (sim->scenarioFillSpent >= SCN_TILES_PER_TICK) {
        return SCN_OP_RATE;
    }

    sim->scenarioFillPending = true;
    sim->scenarioFillX0 = x0;
    sim->scenarioFillX1 = x1;
    sim->scenarioFillY1 = y1;
    sim->scenarioFillTerrain = p->terrain;
    sim->scenarioFillX = x0;
    sim->scenarioFillY = y0;

    if (scenarioFillStep(sim, &wrote)) {
        return SCN_OP_OK;
    }
    if (wrote == 0) {
        /* The check above is the only way a first step writes nothing while
           the budget is the only bound, so this cannot fire today. It stays
           so that a bound added to the walk later cannot quietly leave a fill
           on the sim that never moves. */
        sim->scenarioFillPending = false;
        return SCN_OP_RATE;
    }
    return SCN_OP_QUEUED;
}

void serverSimScenarioDrainFill(ServerSim *sim) {
    if (sim == NULL) {
        return;
    }
    if (sim->scenarioFillPending) {
        (void)scenarioFillStep(sim, NULL);
    }
    /* The budget belongs to the frame and is shared with any fill a hook
       started in it, so it is handed back here — after the hook has had its
       turn at it — rather than at the top of the tick. An op issued from
       outside a tick altogether spends the budget the next frame would have
       had, so its remainder waits a frame longer than a hook's would; that
       costs a fill one frame and never lets a frame carry more squares than
       its map event buffer holds, which is the way round to be wrong. */
    sim->scenarioFillSpent = 0;
}

void serverSimScenarioResetFill(ServerSim *sim) {
    if (sim == NULL) {
        return;
    }
    sim->scenarioFillPending = false;
    sim->scenarioFillX0 = 0;
    sim->scenarioFillX1 = 0;
    sim->scenarioFillY1 = 0;
    sim->scenarioFillTerrain = 0;
    sim->scenarioFillX = 0;
    sim->scenarioFillY = 0;
    sim->scenarioFillSpent = 0;
}

/* Put a mine on a square. The pairing is the one tankLayMine and the builder's
 * mine order make: the terrain byte gains MINE_SUBTRACT and the mine list is
 * told who laid it, so a later detonation credits somebody. The ground it will
 * go on is the builder's list.
 *
 * visible is the difference between a mine every client draws and one that is
 * there to be driven over: the mine list learns of a visible one and the event
 * carries it to every client, while a hidden one is known only to the map byte,
 * which is what a laid mine is to everyone but its owner. */
static ScnOpResult scenarioOpMapPlaceMine(ServerSim *sim,
                                          const ScnOpMapPlaceMine *p) {
    GameSim *gs = &sim->sim;
    BYTE terrain;

    if (!scenarioSquareOnMap(p->x, p->y)) {
        return SCN_OP_BAD_SQUARE;
    }
    if (!scenarioOwnerIsLegal(p->owner)) {
        return SCN_OP_RANGE;
    }
    terrain = mapGetPos(&gs->mp, p->x, p->y);
    if (terrain >= MINE_START && terrain <= MINE_END) {
        return SCN_OP_ALREADY;
    }
    if (terrain != SWAMP && terrain != CRATER && terrain != ROAD &&
        terrain != FOREST && terrain != RUBBLE && terrain != GRASS) {
        return SCN_OP_BAD_TERRAIN;
    }
    /* A pill or a base owns its square and writes the terrain back itself. */
    if (pillsExistPos(&gs->pb, p->x, p->y) == TRUE ||
        basesExistPos(&gs->bs, p->x, p->y) == TRUE) {
        return SCN_OP_BAD_TERRAIN;
    }

    /* The mine list first, so the brain map mapSetPos refreshes underneath is
       told about a finished square rather than a mined byte nobody can see. */
    if (p->visible) {
        minesAddItem(&gs->mns, p->x, p->y);
    }
    minesSetOwner(&gs->mns, p->x, p->y, p->owner);
    mapSetPos(gs, &gs->mp, p->x, p->y, (BYTE)(terrain + MINE_SUBTRACT), TRUE,
              FALSE);
    if (p->visible) {
        /* Bit 7 is what the per-client event filter reads as "everyone", the
           same bit a tank's own mine sets. Without it the event reaches the
           named owner and their allies alone. */
        gs->callbacks.mineVisible(gs->callbacks.ctx, p->x, p->y,
                                  (BYTE)(p->owner | 0x80));
    }
    return SCN_OP_OK;
}

/* Take a mine off a square without setting it off. The pairing is the one a
 * mine explosion and a flood both make: the mine list forgets the square, then
 * the terrain byte loses MINE_SUBTRACT. No explosion, so nothing standing on it
 * is hurt and no crater is left — the ground comes back as it was under the
 * mine. */
static ScnOpResult scenarioOpMapRemoveMine(ServerSim *sim,
                                           const ScnOpMapRemoveMine *p) {
    GameSim *gs = &sim->sim;
    BYTE terrain;

    if (!scenarioSquareOnMap(p->x, p->y)) {
        return SCN_OP_BAD_SQUARE;
    }
    terrain = mapGetPos(&gs->mp, p->x, p->y);
    if (terrain < MINE_START || terrain > MINE_END) {
        return SCN_OP_NO_SUCH_ITEM;
    }

    minesRemoveItem(&gs->mns, p->x, p->y);
    mapSetPos(gs, &gs->mp, p->x, p->y, (BYTE)(terrain - MINE_SUBTRACT), TRUE,
              FALSE);
    return SCN_OP_OK;
}

/* ── Entities ────────────────────────────────────────────────────────
 *
 * The three item lists are the map's, not the round's, so these arms make no
 * state check for the reason the pill and base arms above make none: dealing
 * the estate out before a round starts is as legal as re-dealing it mid-round.
 *
 * An add lets the list choose the slot — the lowest removed one, or the end —
 * and reports it back, because the index is the item's identity everywhere
 * else and only the list knows which one is free. A remove is a tombstone: the
 * slot and the count stay, so every index above it goes on naming the same
 * item.
 */

/* Put the change in the recording as well as on the bus. A control event never
 * reaches the .wbv — the stream carries logitem records only — so without this
 * a replay would go on showing an item the round had taken off the map until
 * the next snapshot, and would never show one that had been added. The record
 * bytes are read back out of the event that has just been filled, so the two
 * channels cannot drift: what a live client applies and what a replay applies
 * are the same six or three bytes. */
static void scenarioRecordEntityChange(const ControlEvent *evt,
                                       const BYTE *rec, BYTE recLen) {
    char blob[8];
    blob[0] = (char) recLen;
    memcpy(blob + 1, rec, recLen);
    logAddEvent(log_EntityChange, evt->u.entityChange.kind,
                evt->u.entityChange.index, evt->u.entityChange.added,
                0, 0, blob);
}

/* A lobby shows the map's pill, base and start counts, and it learns them from
 * the settings event. An add raises one of the three, so the event goes out
 * again; a removal leaves the count where it is — the slot is a tombstone — and
 * republishing an unchanged count is the cheaper answer than working out which
 * ops move it. Only while there is a lobby to tell: a running round takes its
 * counts from the per-tick sync and the entity event. */
static void scenarioRepublishLobbyCounts(ServerSim *sim) {
    if (sim->state == serverStateLobby || sim->state == serverStateCountdown) {
        serverSimPublishLobbySettings(sim);
    }
}

/* Tell every subscriber that one pillbox has joined the map or left it. The
 * index counts from zero, as the snapshots, the game events and the brain API
 * do, while the list counts from one, so the arm passes the op's own index.
 * Published after the list has been written, so a subscriber that reads the
 * sim from its callback sees the change the event describes. The record is the
 * item's map data: reload, coolDown and justSeen are this server's per-tick
 * working state and no client rebuilds them. */
static void scenarioPublishPill(ServerSim *sim, BYTE index0,
                                const pillbox *item, bool added) {
    ControlEvent evt;
    BYTE rec[6];
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ENTITY_CHANGE;
    evt.u.entityChange.kind   = ENTITY_KIND_PILL;
    evt.u.entityChange.index  = index0;
    evt.u.entityChange.added  = added ? 1u : 0u;
    evt.u.entityChange.rec.pill.x      = item->x;
    evt.u.entityChange.rec.pill.y      = item->y;
    evt.u.entityChange.rec.pill.owner  = item->owner;
    evt.u.entityChange.rec.pill.armour = item->armour;
    evt.u.entityChange.rec.pill.speed  = item->speed;
    evt.u.entityChange.rec.pill.inTank = item->inTank ? 1u : 0u;
    serverSimPublishControl(sim, &evt);
    rec[0] = evt.u.entityChange.rec.pill.x;
    rec[1] = evt.u.entityChange.rec.pill.y;
    rec[2] = evt.u.entityChange.rec.pill.owner;
    rec[3] = evt.u.entityChange.rec.pill.armour;
    rec[4] = evt.u.entityChange.rec.pill.speed;
    rec[5] = evt.u.entityChange.rec.pill.inTank;
    scenarioRecordEntityChange(&evt, rec, (BYTE) sizeof(rec));
    scenarioRepublishLobbyCounts(sim);
}

/* The same for a base. refuelTime, baseTime and justStopped stay behind for
 * the same reason the pillbox's three do. */
static void scenarioPublishBase(ServerSim *sim, BYTE index0,
                                const base *item, bool added) {
    ControlEvent evt;
    BYTE rec[6];
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ENTITY_CHANGE;
    evt.u.entityChange.kind   = ENTITY_KIND_BASE;
    evt.u.entityChange.index  = index0;
    evt.u.entityChange.added  = added ? 1u : 0u;
    evt.u.entityChange.rec.base.x      = item->x;
    evt.u.entityChange.rec.base.y      = item->y;
    evt.u.entityChange.rec.base.owner  = item->owner;
    evt.u.entityChange.rec.base.armour = item->armour;
    evt.u.entityChange.rec.base.shells = item->shells;
    evt.u.entityChange.rec.base.mines  = item->mines;
    serverSimPublishControl(sim, &evt);
    rec[0] = evt.u.entityChange.rec.base.x;
    rec[1] = evt.u.entityChange.rec.base.y;
    rec[2] = evt.u.entityChange.rec.base.owner;
    rec[3] = evt.u.entityChange.rec.base.armour;
    rec[4] = evt.u.entityChange.rec.base.shells;
    rec[5] = evt.u.entityChange.rec.base.mines;
    scenarioRecordEntityChange(&evt, rec, (BYTE) sizeof(rec));
    scenarioRepublishLobbyCounts(sim);
}

/* And for a start, whose whole record is its square and the way it faces. */
static void scenarioPublishStart(ServerSim *sim, BYTE index0,
                                 const start *item, bool added) {
    ControlEvent evt;
    BYTE rec[3];
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ENTITY_CHANGE;
    evt.u.entityChange.kind   = ENTITY_KIND_START;
    evt.u.entityChange.index  = index0;
    evt.u.entityChange.added  = added ? 1u : 0u;
    evt.u.entityChange.rec.start.x   = item->x;
    evt.u.entityChange.rec.start.y   = item->y;
    evt.u.entityChange.rec.start.dir = item->dir;
    serverSimPublishControl(sim, &evt);
    rec[0] = evt.u.entityChange.rec.start.x;
    rec[1] = evt.u.entityChange.rec.start.y;
    rec[2] = evt.u.entityChange.rec.start.dir;
    scenarioRecordEntityChange(&evt, rec, (BYTE) sizeof(rec));
    scenarioRepublishLobbyCounts(sim);
}

/* Whether a square will take a start. This is startsIsValidSquare, the test
 * the placement passes make before they put a tank on a start: deep sea with
 * no mine. It is the reverse of the pill and base rule above, and it is not a
 * mistake — a start record names open water and the scatter search walks out
 * from it to the nearest square a tank can sit on, which is why startsIsUsable
 * asks the same thing of a record before the pickers will choose it. A start
 * put on land is a start no picker will ever use. startsIsValidSquare is
 * static to starts.c with no caller that asks it on its own, so it is repeated
 * here the way scenarioSquareTakesPill repeats the builder's test. */
static bool scenarioSquareTakesStart(ServerSim *sim, BYTE mx, BYTE my) {
    GameSim *gs = &sim->sim;
    return mapGetPos(&gs->mp, mx, my) == DEEP_SEA &&
           mapIsMine(&gs->mp, mx, my) == FALSE;
}

/* How many starts are on the map. A map with none is one no tank can be
 * placed on: startsGetStart leaves its caller's square untouched when there
 * is nothing to choose, so the last one is not the scenario's to take away. */
static BYTE scenarioLiveStarts(ServerSim *sim) {
    BYTE n = startsGetNumStarts(&sim->sim.ss);
    BYTE live = 0;
    BYTE i;
    for (i = 1; i <= n; i++) {
        if (startsIsActive(&sim->sim.ss, i) != FALSE) {
            live++;
        }
    }
    return live;
}

/* Put a pillbox on the map. The square test is the builder's place-pill test,
 * the same one the move arm uses, so a script may only put one where a man
 * could have built it. The stocks are checked rather than clamped: pillsAddItem
 * stores the record as handed to it, so a value past its range would stay there
 * where pillsSetPill would have folded it away. */
static ScnOpResult scenarioOpEntityAddPill(ServerSim *sim,
                                           const ScnOpEntityAddPill *p,
                                           ScnOpOut *out) {
    pillbox item;
    BYTE pillNum = 0;

    if (!scenarioSquareOnMap(p->x, p->y)) {
        return SCN_OP_BAD_SQUARE;
    }
    if (!scenarioOwnerIsLegal(p->owner)) {
        return SCN_OP_RANGE;
    }
    if (p->armour > PILLS_MAX_ARMOUR) {
        return SCN_OP_RANGE;
    }
    if (p->speed < PILLBOX_MAX_FIRERATE || p->speed > PILLBOX_ATTACK_NORMAL) {
        return SCN_OP_RANGE;
    }
    if (!scenarioSquareTakesPill(sim, p->x, p->y)) {
        return SCN_OP_BAD_TERRAIN;
    }

    memset(&item, 0, sizeof(item));
    item.x      = p->x;
    item.y      = p->y;
    item.owner  = p->owner;
    item.armour = p->armour;
    item.speed  = p->speed;
    item.inTank = FALSE;
    /* Loaded, which is what pillsCreate leaves a slot nothing has fired from:
       the firing pass counts reload up to speed and shoots at the top of it. */
    item.reload = p->speed;

    /* The last refusal, and it writes nothing on its way out: every slot in
       the count is live and the count is already MAX_PILLS. */
    if (!pillsAddItem(&sim->sim.pb, &item, &pillNum)) {
        return SCN_OP_FULL;
    }

    if (out != NULL) {
        out->index = (BYTE)(pillNum - 1);
    }
    scenarioPublishPill(sim, (BYTE)(pillNum - 1), &item, true);
    return SCN_OP_OK;
}

/* Take a pillbox off the map. The slot and every index above it stay where
 * they are; the record goes out with the event so a script that means to put
 * it back has it. */
static ScnOpResult scenarioOpEntityRemovePill(ServerSim *sim,
                                              const ScnOpEntityRemovePill *p) {
    pillbox item;
    BYTE pillNum = 0;
    ScnOpResult r = scenarioPillFor(sim, p->pill, &pillNum, &item);

    if (r != SCN_OP_OK) {
        return r;
    }
    /* In range but already off the map is the same answer as out of range:
       there is no pillbox of that number to take away. */
    if (pillsIsActive(&sim->sim.pb, pillNum) == FALSE) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    /* A carried pill is on some tank's list of what it is carrying. Taking it
       off the map would strand that list, so the script drops it first. */
    if (item.inTank) {
        return SCN_OP_CARRIED;
    }

    pillsRemoveItem(&sim->sim.pb, pillNum);
    scenarioPublishPill(sim, p->pill, &item, false);
    return SCN_OP_OK;
}

/* Put a base on the map. The square test is the pill's: a base owns its square
 * the way a pill owns its own, and the ground under it has to be ground. */
static ScnOpResult scenarioOpEntityAddBase(ServerSim *sim,
                                           const ScnOpEntityAddBase *p,
                                           ScnOpOut *out) {
    base item;
    BYTE baseNum = 0;

    if (!scenarioSquareOnMap(p->x, p->y)) {
        return SCN_OP_BAD_SQUARE;
    }
    if (!scenarioOwnerIsLegal(p->owner)) {
        return SCN_OP_RANGE;
    }
    /* Checked rather than clamped, for the reason the pill add checks: the
       list stores what it is handed. */
    if (p->armour > BASE_FULL_ARMOUR || p->shells > BASE_FULL_SHELLS ||
        p->mines > BASE_FULL_MINES) {
        return SCN_OP_RANGE;
    }
    if (!scenarioSquareTakesPill(sim, p->x, p->y)) {
        return SCN_OP_BAD_TERRAIN;
    }

    memset(&item, 0, sizeof(item));
    item.x      = p->x;
    item.y      = p->y;
    item.owner  = p->owner;
    item.armour = p->armour;
    item.shells = p->shells;
    item.mines  = p->mines;

    if (!basesAddItem(&sim->sim.bs, &item, &baseNum)) {
        return SCN_OP_FULL;
    }

    if (out != NULL) {
        out->index = (BYTE)(baseNum - 1);
    }
    scenarioPublishBase(sim, (BYTE)(baseNum - 1), &item, true);
    return SCN_OP_OK;
}

/* Take a base off the map. Nothing carries a base, so there is no carried
 * case to refuse. */
static ScnOpResult scenarioOpEntityRemoveBase(ServerSim *sim,
                                              const ScnOpEntityRemoveBase *p) {
    base item;
    BYTE baseNum;

    if (p->base >= basesGetNumBases(&sim->sim.bs)) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    baseNum = (BYTE)(p->base + 1);
    if (basesIsActive(&sim->sim.bs, baseNum) == FALSE) {
        return SCN_OP_NO_SUCH_ITEM;
    }

    memset(&item, 0, sizeof(item));
    basesGetBase(&sim->sim.bs, &item, baseNum);
    basesRemoveItem(&sim->sim.bs, baseNum);
    scenarioPublishBase(sim, p->base, &item, false);
    return SCN_OP_OK;
}

/* Put a start on the map. */
static ScnOpResult scenarioOpEntityAddStart(ServerSim *sim,
                                            const ScnOpEntityAddStart *p,
                                            ScnOpOut *out) {
    start item;
    BYTE startNum = 0;

    if (!scenarioSquareOnMap(p->x, p->y)) {
        return SCN_OP_BAD_SQUARE;
    }
    /* The direction towards land, in sixteenths of a turn. startsSetStart
       folds anything above the range to zero; a script is told instead. */
    if (p->dir > 15) {
        return SCN_OP_RANGE;
    }
    if (!scenarioSquareTakesStart(sim, p->x, p->y)) {
        return SCN_OP_BAD_TERRAIN;
    }

    memset(&item, 0, sizeof(item));
    item.x   = p->x;
    item.y   = p->y;
    item.dir = p->dir;

    if (!startsAddItem(&sim->sim.ss, &item, &startNum)) {
        return SCN_OP_FULL;
    }

    if (out != NULL) {
        out->index = (BYTE)(startNum - 1);
    }
    scenarioPublishStart(sim, (BYTE)(startNum - 1), &item, true);
    return SCN_OP_OK;
}

/* Take a start off the map, unless it is the only one left. */
static ScnOpResult scenarioOpEntityRemoveStart(ServerSim *sim,
                                               const ScnOpEntityRemoveStart *p) {
    start item;
    BYTE startNum;

    if (p->start >= startsGetNumStarts(&sim->sim.ss)) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    startNum = (BYTE)(p->start + 1);
    if (startsIsActive(&sim->sim.ss, startNum) == FALSE) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    /* A map with nowhere to put a tank is not a state the sim can be left in,
       so the last one stays whatever the script asks. */
    if (scenarioLiveStarts(sim) <= 1) {
        return SCN_OP_RANGE;
    }

    memset(&item, 0, sizeof(item));
    startsGetStartStruct(&sim->sim.ss, &item, startNum);
    startsRemoveItem(&sim->sim.ss, startNum);
    scenarioPublishStart(sim, p->start, &item, false);
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
        case SCN_OP_PILL_SET_OWNER:
            return scenarioOpPillSetOwner(sim, &op->u.pillSetOwner);
        case SCN_OP_PILL_SET_ARMOUR:
            return scenarioOpPillSetArmour(sim, &op->u.pillSetArmour);
        case SCN_OP_PILL_SET_SPEED:
            return scenarioOpPillSetSpeed(sim, &op->u.pillSetSpeed);
        case SCN_OP_PILL_MOVE:
            return scenarioOpPillMove(sim, &op->u.pillMove);
        case SCN_OP_BASE_SET_OWNER:
            return scenarioOpBaseSetOwner(sim, &op->u.baseSetOwner);
        case SCN_OP_BASE_SET_STOCK:
            return scenarioOpBaseSetStock(sim, &op->u.baseSetStock);
        case SCN_OP_ENTITY_ADD_PILL:
            return scenarioOpEntityAddPill(sim, &op->u.entityAddPill, out);
        case SCN_OP_ENTITY_REMOVE_PILL:
            return scenarioOpEntityRemovePill(sim, &op->u.entityRemovePill);
        case SCN_OP_ENTITY_ADD_BASE:
            return scenarioOpEntityAddBase(sim, &op->u.entityAddBase, out);
        case SCN_OP_ENTITY_REMOVE_BASE:
            return scenarioOpEntityRemoveBase(sim, &op->u.entityRemoveBase);
        case SCN_OP_ENTITY_ADD_START:
            return scenarioOpEntityAddStart(sim, &op->u.entityAddStart, out);
        case SCN_OP_ENTITY_REMOVE_START:
            return scenarioOpEntityRemoveStart(sim, &op->u.entityRemoveStart);
        case SCN_OP_MAP_SET_TILE:
            return scenarioOpMapSetTile(sim, &op->u.mapSetTile);
        case SCN_OP_MAP_FILL_RECT:
            return scenarioOpMapFillRect(sim, &op->u.mapFillRect);
        case SCN_OP_MAP_PLACE_MINE:
            return scenarioOpMapPlaceMine(sim, &op->u.mapPlaceMine);
        case SCN_OP_MAP_REMOVE_MINE:
            return scenarioOpMapRemoveMine(sim, &op->u.mapRemoveMine);
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
