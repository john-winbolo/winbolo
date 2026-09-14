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
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "server_sim_internal.h"
#include "server_sim_shared.h"     /* publishServerMessage + the team variant, serverSimCbSoundDist */
#include "server_sim_scenario.h"
#include "server_sim_lifecycle.h"  /* serverSimSetTeam, lobbyAutoUnreadyOnChange, serverSimEnterGameOver */
#include "server_sim_join.h"       /* serverSimFindFreeSlot — the first free seat */
#include "netpacks.h"      /* lobbyBotNameAcceptable — the lobby's own name check */
#include "../../common/wb_log.h"   /* the line a dropped roster change leaves */
#include "channel_mux.h"   /* CHANNEL_CONTROL_SEG — the panel cap is derived from it */
#include "tank.h"          /* the tank arms mutate through these */
#include "lgm.h"           /* the builder arms mutate through these */
#include "bolo_map.h"      /* mapGetPos and mapSetPos — what the map arms write through */
#include "pillbox.h"       /* the pill arms read and write through these */
#include "bases.h"         /* the base arms mutate through these */
#include "mines.h"         /* the mine list the map arms add to and clear */
#include "starts.h"        /* startsGetStart — the teleport arm's start mode */
#include "gametype.h"      /* TANK_FULL_* — the stock caps */
#include "sim_rules.h"     /* the table the rule arm writes, and its check */
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
                                 sim->sim.rules.tank_full_shells, &bad);
    mines  = scenarioStockTarget(p->mode, p->mines,  tankGetMines(t),
                                 sim->sim.rules.tank_full_mines,  &bad);
    armour = scenarioStockTarget(p->mode, p->armour, tankGetArmour(t),
                                 sim->sim.rules.tank_full_armour, &bad);
    trees  = scenarioStockTarget(p->mode, p->trees,  tankGetTrees(t),
                                 sim->sim.rules.tank_full_trees,  &bad);
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
        tankSetShells(&sim->sim, t, (BYTE)shells);
    }
    if (mines >= 0) {
        tankSetMines(&sim->sim, t, (BYTE)mines);
    }
    if (armour >= 0) {
        tankSetArmour(t, (BYTE)armour);
    }
    if (trees >= 0) {
        tankSetTrees(&sim->sim, t, (BYTE)trees);
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
    /* The cause goes on the wire in the kill event and into the tank's own
       last-death field, so it is one of the four values those carry. */
    if (p->cause > LAST_DEATH_BY_SCRIPT) {
        return SCN_OP_RANGE;
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
        if (p->start != SCN_NONE &&
            startsIsActive(&sim->sim.ss, (BYTE)(p->start + 1)) == FALSE) {
            return SCN_OP_NO_SUCH_ITEM;
        }
        {
            /* Saved and restored: a start reserved for this player's next
               respawn by the batch placement is not this op's to spend. */
            BYTE reserved = sim->sim.pendingStartIdx[p->slot];
            bool wasFinding = sim->sim.inStartFind;
            /* Naming a start is asking for that same resolution on a chosen
               record. The scenario's own start slot is what startsGetStart
               honours first, ahead of the placement policy: the op is the
               scenario choosing, and the policy is asked only when the
               engine is. Consumed by the resolver. */
            sim->sim.scenarioStartIdx[p->slot] =
                (p->start == SCN_NONE) ? MAX_STARTS : p->start;
            sim->sim.inStartFind = TRUE;
            startsGetStart(&sim->sim, &sim->sim.ss, &sx, &sy, &sdir, p->slot);
            sim->sim.inStartFind = wasFinding;
            sim->sim.scenarioStartIdx[p->slot] = MAX_STARTS;
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
    /* A removed pillbox is off the map; there is nothing to pick up. */
    if (pillsIsActive(&sim->sim.pb, pillNum) == FALSE) {
        return SCN_OP_NO_SUCH_ITEM;
    }
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
    if (p->trees != SCN_NONE && p->trees > sim->sim.rules.tank_full_trees) {
        return SCN_OP_RANGE;
    }
    if (p->mines != SCN_NONE && p->mines > sim->sim.rules.tank_full_mines) {
        return SCN_OP_RANGE;
    }

    lgmSetCarried(&sim->sim, l,
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
    /* A slot a removal has emptied is not an item: the number is in range
       but there is nothing on the map behind it. */
    if (pillsIsActive(&sim->sim.pb, *outNum) == FALSE) {
        return SCN_OP_NO_SUCH_ITEM;
    }
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
    if (p->armour > sim->sim.rules.pill_max_armour) {
        return SCN_OP_RANGE;
    }
    if (item.inTank) {
        return SCN_OP_CARRIED;
    }

    item.armour = p->armour;
    pillsSetPill(&sim->sim, &sim->sim.pb, &item, pillNum);
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
    if (p->speed < sim->sim.rules.pill_attack_min_ticks ||
        p->speed > sim->sim.rules.pill_attack_ticks) {
        return SCN_OP_RANGE;
    }

    item.speed = p->speed;
    pillsSetPill(&sim->sim, &sim->sim.pb, &item, pillNum);
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
    pillsSetPill(&sim->sim, &sim->sim.pb, &item, pillNum);
    return SCN_OP_OK;
}

/* Hand a base to a slot. The capture is the one a tank driving on to a base
 * makes, so the event and the record are that capture's, and taking a base off
 * another player empties it as a capture does. keepStock is how a script
 * re-deals the map without stripping what it deals. */
static ScnOpResult scenarioOpBaseSetOwner(ServerSim *sim,
                                          const ScnOpBaseSetOwner *p) {
    if (p->base >= basesGetNumBases(&sim->sim.bs) ||
        basesIsActive(&sim->sim.bs, (BYTE)(p->base + 1)) == FALSE) {
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
    if (p->base >= basesGetNumBases(&sim->sim.bs) ||
        basesIsActive(&sim->sim.bs, (BYTE)(p->base + 1)) == FALSE) {
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
 * Two things stop the walk: the tile budget, and the room left in the frame's
 * map event buffer. The second matters because simMapChangeCallback drops a
 * change without a signal once the buffer is full, and a dropped change is a
 * permanent divergence for any client that is not view-culled: its shadow map
 * advances only from that buffer, the catch-up sweep runs for culled slots
 * only, and the header checksum is taken over the shadow, so the stale copy
 * matches itself and no resync is ever asked for. The world's own changes in
 * the half-steps take the buffer first; the fill yields to a full one and the
 * remainder waits, which is the safe direction. The counter is cleared at the
 * world reset and at the top of a lobby tick as well as a running one, so it
 * is never stale where this reads it.
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
            if (sim->scenarioFillSpent >= SCN_TILES_PER_TICK ||
                sim->mapEventCount >= MAX_MAP_EVENTS) {
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
       finished earlier in it — or the frame's map event buffer is already
       full. Refused here, before anything is written down, so there is no
       half-started rectangle to unwind. */
    if (sim->scenarioFillSpent >= SCN_TILES_PER_TICK ||
        sim->mapEventCount >= MAX_MAP_EVENTS) {
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
        /* Both bounds were checked above, so a first step that writes nothing
           cannot happen today. It stays so that a bound added to the walk
           later cannot quietly leave a fill on the sim that never moves. */
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
    if (p->armour > sim->sim.rules.pill_max_armour) {
        return SCN_OP_RANGE;
    }
    if (p->speed < sim->sim.rules.pill_attack_min_ticks ||
        p->speed > sim->sim.rules.pill_attack_ticks) {
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
    if (p->armour > sim->sim.rules.base_full_armour ||
        p->shells > sim->sim.rules.base_full_shells ||
        p->mines > sim->sim.rules.base_full_mines) {
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

/* ── Roster ──────────────────────────────────────────────────────────
 *
 * Six arms over two states: three change the roster of a running round and
 * three change a lobby's. The lobby three carry the validation of the
 * matching CMD_LOBBY_* arms in server_command_dispatch.c without the
 * sender's permission check, so a script and a host's click are refused
 * for the same reasons.
 *
 * Adding a bot builds a Lua VM and a ClientSim and removing one tears them
 * down. That is more than a running frame should do on demand, so the two
 * in-round arms queue and the sim makes one change a tick; the lobby is
 * between rounds and its three arms apply where they stand.
 *
 * The state each arm needs is its own business rather than the prelude's:
 * the arms already landed are ones a lobby and a round both take, and the
 * prelude has no per-op table to put this in. */

static ScnOpResult scenarioRequireRunning(ServerSim *sim) {
    return (sim->state == serverStateRunning) ? SCN_OP_OK : SCN_OP_WRONG_STATE;
}

/* The lobby's own two conditions, as CMD_LOBBY_ADD_BOT and its neighbours
 * read them: a lobby that is taking part, and the server still in it. */
static ScnOpResult scenarioRequireLobby(ServerSim *sim) {
    if (!serverSimIsLobbyEnabled(sim) || sim->state != serverStateLobby) {
        return SCN_OP_WRONG_STATE;
    }
    return SCN_OP_OK;
}

/* Whether a fixed-size text field out of an op ends inside its buffer. One
 * that does not is refused rather than read past the end of it. */
static bool scenarioTextTerminated(const char *buf, size_t cap) {
    size_t i;
    for (i = 0; i < cap; i++) {
        if (buf[i] == '\0') return true;
    }
    return false;
}

/* The name a new bot takes. A name in the op goes through the check the
 * lobby's Add Bot puts a host's typing through — the same characters
 * refused, and the same collision with a name already in the game — and an
 * op that carries none is given the lobby's own default for its seat.
 *
 * Called twice for a queued spawn: once as it is accepted, where the seat
 * may not be chosen yet and only the refusals matter, and again as it
 * lands, where the seat is known and the name is the one the bot joins
 * under. */
static ScnOpResult scenarioBotName(const char *asked, BYTE slot,
                                   char *out, size_t outCap) {
    char validated[PACKET_MAX_PLAYER_NAME];

    if (!scenarioTextTerminated(asked, PLAYER_NAME_LEN)) {
        return SCN_OP_TOO_BIG;
    }
    if (asked[0] == '\0') {
        SDL_snprintf(out, outCap, "Bot %d", (int)slot + 1);
        return SCN_OP_OK;
    }
    if (!lobbyBotNameAcceptable(asked, validated, sizeof(validated), -1,
                                transportUdpServerGetPlayerName, NULL, NULL)) {
        return SCN_OP_RANGE;
    }
    SDL_strlcpy(out, validated, outCap);
    return SCN_OP_OK;
}

/* The brain a new bot runs: the one the op names; failing that the one the
 * seat was written with, which is how a seat held for a team gets that
 * team's brain when something fields it; and failing both the server's own.
 *
 * A "package:NAME" brain is one carried by a scenario's package. Nothing on
 * the sim opens a package, so the name is refused here rather than handed to
 * the loader as a path — a file called "package:NAME" is not what the script
 * meant. The host that unpacks a scenario is what resolves these, and it
 * will resolve the name to a path before the op reaches this funnel. */
static ScnOpResult scenarioBrainPath(ServerSim *sim, const char *asked,
                                     BYTE slot, const char **out) {
    const char *path;
    SDL_PathInfo info;

    if (!scenarioTextTerminated(asked, SCN_PATH_MAX)) {
        return SCN_OP_TOO_BIG;
    }
    path = NULL;
    if (asked[0] != '\0') {
        path = asked;
    } else if (slot < MAX_TANKS && sim->seatBrain[slot][0] != '\0') {
        path = sim->seatBrain[slot];
    } else {
        path = serverSimGetBotBrainPath(sim);
    }
    if (path == NULL || path[0] == '\0') {
        return SCN_OP_NOT_FOUND;
    }
    if (SDL_strncmp(path, "package:", 8) == 0) {
        return SCN_OP_NOT_FOUND;
    }
    /* Read before the add rather than after it: botManagerAddBot registers
       the slot and then unwinds it when the brain will not load, and an arm
       that refuses should not have touched the roster on its way out. */
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) {
        return SCN_OP_NOT_FOUND;
    }
    *out = path;
    return SCN_OP_OK;
}

/* The seat a spawn takes. 0xFF asks for the first free one, which is
 * chosen as the spawn lands and not as it is queued: ten spawns asked for
 * in one tick would otherwise every one of them name the same seat. */
static ScnOpResult scenarioSpawnSeat(ServerSim *sim, BYTE asked, BYTE *out) {
    int freeSlot;

    if (asked != SCN_NONE) {
        if (asked >= MAX_TANKS) {
            return SCN_OP_RANGE;
        }
        /* A seat held for a bot that is not on the field is the one occupied
           seat a spawn may name: fielding it is what the seat is for. */
        if ((sim->playerConnected[asked] && sim->lobbyPlayers[asked].fielded) ||
            botManagerIsBot(sim, asked)) {
            return SCN_OP_ALREADY;
        }
        *out = asked;
        return SCN_OP_OK;
    }
    freeSlot = serverSimFindFreeSlot(sim, true);
    if (freeSlot < 0) {
        return SCN_OP_FULL;
    }
    *out = (BYTE)freeSlot;
    return SCN_OP_OK;
}

/* Put a bot in a seat, on its team.
 *
 * The team goes down the add with the rest of the config rather than being
 * written onto the slot afterwards. serverSimAddBot writes the team it is
 * handed and then picks the slot's lobby start from it, clustering the bot
 * near the reservations its team already holds; a team written after that
 * call has missed the pick, and the bot is placed as though it had no team.
 *
 * The alliance is the one thing the add does not carry, so a seat new to the
 * roster is allied with its team below. A seat that was already held is not:
 * it was allied when the round started and keeps that through every
 * unfielding, so accepting again would publish events for a state every
 * client already holds.
 *
 * One accept, and never a rebake of the whole matrix. The batched reset
 * exists so that rebaking every slot costs one event instead of N×(N-1)/2;
 * a reset here would make every receiver drop and rebuild all sixteen slots
 * to learn one pair. */
static bool scenarioAddBotInSeat(ServerSim *sim, BYTE slot, const char *brain,
                                 const char *name, BYTE team,
                                 const ScnTable *init) {
    /* A seat already held keeps the name and the team it was seated with: the
       add reads both off the roster rather than taking the op's, so a wave
       spawning by seat number does not have to restate them. */
    const bool wasHeld = sim->playerConnected[slot] &&
                         !sim->lobbyPlayers[slot].fielded;
    char seatName[PLAYER_NAME_LEN];

    seatName[0] = '\0';
    if (wasHeld) {
        playersGetPlayerName(&sim->sim.plyrs, slot, seatName, sizeof(seatName),
                             TRUE);
        name = seatName;
        team = sim->lobbyPlayers[slot].teamNumber;
    }
    if (!botManagerAddBot(sim, slot, brain, name,
                          serverSimGetBotAiType(sim),
                          gameTypeGet(&sim->sim.game),
                          sim->sim.hiddenMines, team, init)) {
        /* An add that gets part-way and then fails empties the seat on its
           way out. A seat that was being fielded goes back to being held, so
           a brain that will not load costs the wave its bot and not its
           seat. */
        if (wasHeld && !sim->playerConnected[slot]) {
            serverSimAddUnfieldedSeat(sim, slot, seatName, team);
        }
        return false;
    }
    transportUdpServerSetBotName(slot, name);
    if (!wasHeld && team > 0 && team < MAX_TANKS) {
        /* One accept, with the first member found. An accept merges the two
           sides' groups, so the new seat joins everyone that member is allied
           with, and a team seated this way is one group by construction: each
           bot was merged into it as it spawned. A team someone has since
           split — a leave, a set_team — gives the seat one of its pieces,
           which is still better than the nothing it used to get.

           Quiet: putting a bot on its team is seating, not something a
           player did. The lobby does the same job with a rebake, which
           announces nothing, and a script with something to say has its own
           message call. */
        BYTE i;
        for (i = 0; i < MAX_TANKS; i++) {
            if (i == slot) continue;
            if (!sim->playerConnected[i]) continue;
            if (sim->lobbyPlayers[i].teamNumber != team) continue;
            serverSimAcceptAllianceQuiet(sim, i, slot);
            break;
        }
    }
    serverSimPublishLobbySlot(sim, slot);
    return true;
}

/* The slot a remove names: a seat with somebody in it, and that somebody a
 * bot. Both remove arms ask the same two questions in the same order. */
static ScnOpResult scenarioRemovableBot(ServerSim *sim, BYTE slot) {
    if (slot >= MAX_TANKS || !sim->playerConnected[slot]) {
        return SCN_OP_NO_SUCH_PLAYER;
    }
    if (!serverSimIsBot(sim, slot)) {
        return SCN_OP_IS_HUMAN;
    }
    return SCN_OP_OK;
}

/* Take the bot out of a seat a script named. A seat that was seeded to be
 * held — the kind a wave fields and refields — goes back to being held
 * rather than being emptied, so the next wave still has it; every other seat
 * is emptied, which is what a remove has always done. */
static void scenarioTakeBotOut(ServerSim *sim, BYTE slot) {
    if (sim->lobbyPlayers[slot].keepSeat && sim->lobbyPlayers[slot].fielded) {
        /* Off the field where it sits. The seat keeps its name, its team and
           the brain it was written to run, so the next thing to field it
           starts the same bot the last one did. */
        serverSimUnfieldBot(sim, slot);
        return;
    }
    serverSimRemoveBot(sim, slot);
}

/* ── The lobby template ───────────────────────────────────────────────── */

/* The seats a scenario asked for, which are the ones keepSeat marks: the
 * template seeded them, or a script asked for one by hand, and either way
 * they belong to the scenario and not to the host. A seat the script has
 * since fielded still carries the mark, so this finds it too. */
static bool scenarioSeatIsTemplates(const ServerSim *sim, BYTE slot) {
    return sim->playerConnected[slot] && sim->lobbyPlayers[slot].keepSeat;
}

static BYTE scenarioSeatsOnTeam(const ServerSim *sim, BYTE team) {
    BYTE i;
    BYTE n = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (scenarioSeatIsTemplates(sim, i) &&
            sim->lobbyPlayers[i].teamNumber == team) {
            n++;
        }
    }
    return n;
}

/* Take a team's scenario seats down to want, highest seat first so the seats
 * the host has had longest are the ones that survive. A team already at or
 * below want is left alone — this only ever removes. */
static void scenarioTrimTeamTo(ServerSim *sim, BYTE team, BYTE want) {
    BYTE have = scenarioSeatsOnTeam(sim, team);
    BYTE i;
    for (i = MAX_TANKS; i > 0 && have > want; i--) {
        BYTE slot = (BYTE)(i - 1);
        if (!scenarioSeatIsTemplates(sim, slot)) continue;
        if (sim->lobbyPlayers[slot].teamNumber != team) continue;
        serverSimRemoveBot(sim, slot);
        have--;
    }
}

/* Empty every seat the scenario put there. The host's own seats and every
 * human are left alone. */
void serverSimScenarioClearSeats(ServerSim *sim) {
    BYTE i;
    if (sim == NULL) return;
    for (i = 0; i < MAX_TANKS; i++) {
        if (scenarioSeatIsTemplates(sim, i)) {
            serverSimRemoveBot(sim, i);
        }
    }
}

/* Seat one of a team's bots. An unfielded team gets the seat and no bot; a
 * fielded one gets both. Answers false when there was nowhere to put it,
 * which stops the team's loop rather than spinning on a full roster. */
static bool scenarioSeatOne(ServerSim *sim, const ScnLobbyTeam *team) {
    char name[PLAYER_NAME_LEN];
    int  slot;

    /* The seat rule the lobby's Add Bot and both spawn handlers already
       share, so a bot seats above the human cap here exactly as it does
       there and there is no second rule to drift from it. */
    slot = serverSimFindFreeSlot(sim, true);
    if (slot < 0) return false;
    if (scenarioBotName("", (BYTE)slot, name, sizeof(name)) != SCN_OP_OK) {
        return false;
    }

    if (!team->fielded) {
        if (!serverSimAddUnfieldedSeat(sim, (BYTE)slot, name, team->id)) {
            return false;
        }
    } else {
        const char *brain = (team->brain[0] != '\0')
                          ? team->brain : serverSimGetBotBrainPath(sim);
        if (brain == NULL || brain[0] == '\0') return false;
        if (!serverSimCreateBot(sim, (BYTE)slot, brain, name,
                                serverSimGetBotAiType(sim),
                                gameTypeGet(&sim->sim.game),
                                sim->sim.hiddenMines, team->id, NULL)) {
            return false;
        }
        sim->lobbyPlayers[slot].keepSeat = true;
    }
    /* Recorded whether the seat holds a bot yet or not: an unfielded seat is
       fielded later by a spawn that names no brain of its own, and this is
       where that spawn finds the one its team was written with. */
    SDL_strlcpy(sim->seatBrain[slot], team->brain,
                sizeof(sim->seatBrain[slot]));
    if (team->id > 0 && team->id < MAX_TANKS && !sim->teams[team->id].in_use) {
        sim->teams[team->id].in_use = 1;
        if (sim->teams[team->id].name[0] == '\0') {
            SDL_snprintf(sim->teams[team->id].name, LOBBY_TEAM_NAME_LEN,
                         "Team %d", (int)team->id);
        }
    }
    return true;
}

/* Build the lobby the attached scenario asks for, from whatever is there
 * now. Every seat the previous scenario left goes first, so committing a
 * plain map over a scenario one leaves no horde behind, and a scenario with
 * no template of its own leaves an ordinary lobby. */
void serverSimScenarioSeatLobby(ServerSim *sim) {
    BYTE t;
    if (sim == NULL) return;
    serverSimScenarioClearSeats(sim);
    if (!sim->scenarioLobbyValid) return;
    for (t = 0; t < sim->scenarioLobby.numTeams; t++) {
        const ScnLobbyTeam *team = &sim->scenarioLobby.teams[t];
        BYTE n;
        if (team->id == 0 || team->id >= MAX_TANKS) continue;
        for (n = 0; n < team->bots; n++) {
            if (!scenarioSeatOne(sim, team)) break;
        }
    }
}

/* Bring a lobby that has just come back from a round into line with the
 * template, keeping what the host did to it in between.
 *
 * bots is how many the engine seeds and not a number it keeps re-imposing:
 * a host who trimmed a horde of ten to six gets six back, because the point
 * of seating them where the host can see them is that the host may trim
 * them. maxBots is the one that still binds, so a host who added past it is
 * cut back to it. A team the host emptied altogether stays empty — that is
 * the same edit as the trim to six, only further, and a floor that appeared
 * only at zero would let a host reduce a horde to one but not to none.
 *
 * Then the other half: a seat the script fielded during the round goes back
 * to being held, so the next round starts from the lobby the template
 * describes rather than from wherever the last round's waves left it. */
void serverSimScenarioReconcileLobby(ServerSim *sim) {
    BYTE t, i;
    if (sim == NULL || !sim->scenarioLobbyValid) return;

    for (i = 0; i < MAX_TANKS; i++) {
        if (!scenarioSeatIsTemplates(sim, i)) continue;
        if (!sim->lobbyPlayers[i].fielded) continue;
        scenarioTakeBotOut(sim, i);
    }

    for (t = 0; t < sim->scenarioLobby.numTeams; t++) {
        const ScnLobbyTeam *team = &sim->scenarioLobby.teams[t];
        if (team->id == 0 || team->id >= MAX_TANKS) continue;
        if (team->maxBots == 0) continue;
        scenarioTrimTeamTo(sim, team->id, team->maxBots);
    }
}

/* How many template seats each team holds right now, indexed by team id, for
 * a caller that means to put these counts back later. Answers false and
 * writes nothing when no template is attached, so the caller can tell that
 * apart from a team recorded at zero: a host who emptied a team on purpose
 * has to come back to an empty one. */
bool serverSimScenarioSeatCounts(const ServerSim *sim, BYTE *out) {
    BYTE t;
    if (sim == NULL || out == NULL) return false;
    if (!sim->scenarioLobbyValid) return false;
    memset(out, 0, MAX_TANKS * sizeof(BYTE));
    for (t = 0; t < sim->scenarioLobby.numTeams; t++) {
        BYTE id = sim->scenarioLobby.teams[t].id;
        if (id == 0 || id >= MAX_TANKS) continue;
        out[id] = scenarioSeatsOnTeam(sim, id);
    }
    return true;
}

/* Put each of the template's teams back down to the count it was given.
 *
 * Trimming only. A team now holding fewer seats than its count is left where
 * it is rather than seated back up, and that half is deliberate: it is the
 * reconcile's rule that the ceiling binds and the floor does not, so a host
 * who added seats and then backed out of a map keeps the lobby they are
 * looking at instead of having the additions taken off them as well. */
void serverSimScenarioTrimSeatsTo(ServerSim *sim, const BYTE *counts) {
    BYTE t;
    if (sim == NULL || counts == NULL || !sim->scenarioLobbyValid) return;
    for (t = 0; t < sim->scenarioLobby.numTeams; t++) {
        BYTE id = sim->scenarioLobby.teams[t].id;
        if (id == 0 || id >= MAX_TANKS) continue;
        scenarioTrimTeamTo(sim, id, counts[id]);
    }
}

/* A different map has been committed. Whoever owns the scenario is told
 * first, so it can drop the one the previous map had and look for one beside
 * the new file; the template it leaves behind is what the seating below
 * reads. A map with no scenario clears the template, and the seating then
 * empties the seats the previous one left rather than carrying them into a
 * map that knows nothing about them. */
void serverSimScenarioOnMapChanged(ServerSim *sim, const char *mapPath) {
    if (sim == NULL) return;
    if (sim->scenarioMapChanged != NULL) {
        sim->scenarioMapChanged(sim->scenarioMapChangedCtx, sim,
                                mapPath != NULL ? mapPath : "");
    }
    serverSimScenarioSeatLobby(sim);
}

/* The lobby's own settings, brought into line with whatever scenario is
   attached now. Both points a lobby first learns its scenario need this: a
   map commit, which calls it straight after the map change above, and a
   server or headless run booting on a scripted map, which attaches and seats
   without any commit and would otherwise never reach it.

   The game type: a scripted round plays by what its scenario declared, which
   gameTypeResolve reads, so the type itself says scripted. The type the
   lobby was on is remembered rather than recomputed, because a host may have
   picked one by hand and the operator's startup default — what
   serverSimResetLobbyToDefaults restores — is not it. On a boot the type the
   lobby was on is the operator's own -gametype, which is what a plain map
   committed later gives back. Only the first scripted commit in a run of
   them remembers, so scripted map after scripted map still gives the lobby's
   own type back at the end.

   Ranked and a scenario do not go together: a scripted round is not a
   measured one. The settings handler refuses ranked while a scenario is
   attached, and this is the other direction — ranked already on when the
   scenario arrives.

   The AI policy: a scenario fields its own bots, and aiNone empties the
   roster of them, so a policy that allows none is moved up to the one that
   allows them plainly. A policy that already allows bots is the operator's
   or the host's and is left alone. */
void serverSimScenarioApplyLobbyRules(ServerSim *sim) {
    if (sim == NULL) return;
    if (sim->scenarioIdentity.source != lobbyScenarioNone) {
        if (gameTypeGet(&sim->sim.game) != gameScripted) {
            sim->preScenarioGameType = gameTypeGet(&sim->sim.game);
            serverSimSetGameType(sim, gameScripted);
        }
        if (serverSimGetRanked(sim)) {
            serverSimSetRanked(sim, false);
        }
        if (serverSimGetBotAiType(sim) == aiNone) {
            serverSimSetAiPolicy(sim, (uint8_t)aiYes);
            serverSimSetBotAiType(sim, aiYes);
        }
    } else if (gameTypeGet(&sim->sim.game) == gameScripted) {
        serverSimSetGameType(sim,
                             sim->preScenarioGameType != (gameType)0
                                 ? sim->preScenarioGameType
                                 : gameOpen);
        sim->preScenarioGameType = (gameType)0;
    }
}

/* Put one change on the queue. The one past the last is refused rather than
 * displacing something already accepted: a script told QUEUED has been
 * promised that change. */
static ScnOpResult scenarioRosterQueue(ServerSim *sim,
                                       const ScnRosterQueueEntry *entry) {
    uint8_t at;

    if (sim->scenarioRosterCount >= SCN_ROSTER_QUEUE_MAX) {
        return SCN_OP_FULL;
    }
    at = (uint8_t)((sim->scenarioRosterHead + sim->scenarioRosterCount) %
                   SCN_ROSTER_QUEUE_MAX);
    sim->scenarioRoster[at] = *entry;
    sim->scenarioRosterCount++;
    return SCN_OP_QUEUED;
}

/* Add a bot to a running round. What is checked here is the payload — the
 * team, the name, the brain and a named seat — so a script hears about its
 * own mistakes at once; what the world looks like is asked again as the
 * spawn lands, because by then it may be a different world. */
static ScnOpResult scenarioOpRosterSpawnBot(ServerSim *sim,
                                            const ScnOpRosterSpawnBot *p,
                                            ScnOpOut *out) {
    ScnRosterQueueEntry entry;
    const char *brain = NULL;
    char name[PLAYER_NAME_LEN];
    BYTE slot = SCN_NONE;
    ScnOpResult r;

    r = scenarioRequireRunning(sim);
    if (r != SCN_OP_OK) return r;
    /* A server with no bot AI runs no brains, which is the answer the lobby's
       own Add Bot gives a host on such a server. */
    if (serverSimGetBotAiType(sim) == aiNone) {
        return SCN_OP_WRONG_STATE;
    }
    /* Nothing downstream refuses a team past the end of the table: the add
       writes what the config holds straight onto the slot, and the setter a
       team change goes through coerces it to 1 instead. Either way a script
       would be told its bot joined the team it asked for when it had not. */
    if (p->team >= MAX_TANKS) {
        return SCN_OP_RANGE;
    }
    /* A named start must be one on the map; it is honoured through the
       reserved-start slot as the spawn lands. */
    if (p->start != SCN_NONE &&
        (p->start >= startsGetNumStarts(&sim->sim.ss) ||
         startsIsActive(&sim->sim.ss, (BYTE)(p->start + 1)) == FALSE)) {
        return SCN_OP_RANGE;
    }
    /* A named loadout is one of the game types the spawn's loadout words
       hold. Anything else is refused here rather than reaching the tank as a
       game type the engine has no amounts written for; 0 leaves the answer to
       the spawn-loadout policy. */
    if (p->loadout != 0 &&
        p->loadout != (BYTE)gameOpen &&
        p->loadout != (BYTE)gameTournament &&
        p->loadout != (BYTE)gameStrictTournament) {
        return SCN_OP_RANGE;
    }
    /* The init table reaches a Lua VM, so every string in it must end inside
       its own field, as the name and the brain must. */
    if (p->init.count > SCN_TABLE_MAX) {
        return SCN_OP_TOO_BIG;
    }
    {
        BYTE k;
        for (k = 0; k < p->init.count; k++) {
            if (!scenarioTextTerminated(p->init.kv[k].key, SCN_TABLE_KEY_LEN) ||
                !scenarioTextTerminated(p->init.kv[k].value, SCN_TABLE_VALUE_LEN)) {
                return SCN_OP_TOO_BIG;
            }
        }
    }
    r = scenarioSpawnSeat(sim, p->slot, &slot);
    if (r != SCN_OP_OK) return r;
    r = scenarioBotName(p->name, slot, name, sizeof(name));
    if (r != SCN_OP_OK) return r;
    r = scenarioBrainPath(sim, p->brain, slot, &brain);
    if (r != SCN_OP_OK) return r;

    memset(&entry, 0, sizeof(entry));
    entry.isSpawn = true;
    entry.spawn = *p;
    r = scenarioRosterQueue(sim, &entry);
    if (r != SCN_OP_QUEUED) return r;

    if (out != NULL) {
        /* The seat, when the op named one. A spawn that asked for the first
           free seat is not promised one of them yet — the seat it takes is
           chosen as it lands. */
        out->slot = p->slot;
    }
    return SCN_OP_QUEUED;
}

/* Take a bot out of a running round. */
static ScnOpResult scenarioOpRosterRemoveBot(ServerSim *sim,
                                             const ScnOpRosterRemoveBot *p) {
    ScnRosterQueueEntry entry;
    ScnOpResult r;

    r = scenarioRequireRunning(sim);
    if (r != SCN_OP_OK) return r;
    r = scenarioRemovableBot(sim, p->slot);
    if (r != SCN_OP_OK) return r;

    memset(&entry, 0, sizeof(entry));
    entry.isSpawn = false;
    entry.removeSlot = p->slot;
    return scenarioRosterQueue(sim, &entry);
}

/* Move a player to a team mid-round. The same three steps the lobby's own
 * team command makes, minus the auto-unready that only means something
 * while the lobby is still gathering. */
static ScnOpResult scenarioOpRosterSetTeam(ServerSim *sim,
                                           const ScnOpRosterSetTeam *p) {
    ScnOpResult r = scenarioRequireRunning(sim);
    if (r != SCN_OP_OK) return r;
    if (p->slot >= MAX_TANKS || !sim->playerConnected[p->slot]) {
        return SCN_OP_NO_SUCH_PLAYER;
    }
    if (p->team >= MAX_TANKS) {
        return SCN_OP_RANGE;
    }

    serverSimSetTeam(sim, p->slot, p->team);
    logAddEvent(log_TeamSet, p->slot, p->team, 0, 0, 0, NULL);
    serverSimPublishLobbySlot(sim, p->slot);
    return SCN_OP_OK;
}

/* Add a bot to the lobby. CMD_LOBBY_ADD_BOT's own checks in order — the
 * operator's bot cap, the name, a free seat — and then the same add.
 *
 * fielded false asks for the seat without the bot: the roster gains the
 * entry, the host can see and trim it, and no brain loads until a spawn
 * names the seat. Nothing else about the add changes — the cap counts it,
 * the name is checked the same way — except the brain, which such a seat has
 * no use for yet. */
static ScnOpResult scenarioOpLobbyAddBot(ServerSim *sim,
                                         const ScnOpLobbyAddBot *p,
                                         ScnOpOut *out) {
    const char *brain = NULL;
    char name[PLAYER_NAME_LEN];
    BYTE maxBots;
    BYTE slot = 0;
    ScnOpResult r;

    r = scenarioRequireLobby(sim);
    if (r != SCN_OP_OK) return r;
    if (serverSimGetBotAiType(sim) == aiNone) {
        return SCN_OP_WRONG_STATE;
    }
    if (p->team >= MAX_TANKS) {
        return SCN_OP_RANGE;
    }
    maxBots = serverSimGetMaxBots(sim);
    if (maxBots > 0 && serverSimGetLobbyBotCount(sim) >= maxBots) {
        return SCN_OP_FULL;
    }
    /* The name before the seat, which is the order the command arm asks in,
       so an op that is wrong about both hears about the same one a host
       would. The seat is not known yet and only the refusals matter here. */
    r = scenarioBotName(p->name, 0, name, sizeof(name));
    if (r != SCN_OP_OK) return r;
    /* A seat held without a bot in it loads no brain, so there is no path to
       resolve here: the spawn that fields the seat brings one. */
    if (p->fielded) {
        /* No seat yet, so no seat brain to prefer — the op's or the
           server's. */
        r = scenarioBrainPath(sim, p->brain, SCN_NONE, &brain);
        if (r != SCN_OP_OK) return r;
    }
    /* The seat the op names, or the first free one, by the rule the spawn
       arm and the lobby's Add Bot share. */
    r = scenarioSpawnSeat(sim, p->slot, &slot);
    if (r != SCN_OP_OK) return r;
    /* Again with the seat, because an op that named no name is given the
       lobby's default for the one it got. */
    (void)scenarioBotName(p->name, slot, name, sizeof(name));

    if (!p->fielded) {
        if (!serverSimAddUnfieldedSeat(sim, slot, name, p->team)) {
            /* The seat picked is one already being held. A spawn is allowed
               to land on one of those and this is not a spawn. */
            return SCN_OP_ALREADY;
        }
        lobbyAutoUnreadyOnChange(sim);
        if (out != NULL) {
            out->slot = slot;
        }
        return SCN_OP_OK;
    }

    if (!scenarioAddBotInSeat(sim, slot, brain, name, p->team, NULL)) {
        /* The path named a file and the file would not load as a brain. */
        return SCN_OP_NOT_FOUND;
    }
    serverSimPublishLobbyBotBrain(sim, slot);
    lobbyAutoUnreadyOnChange(sim);
    if (out != NULL) {
        out->slot = slot;
    }
    return SCN_OP_OK;
}

/* Take a bot out of the lobby, as CMD_LOBBY_REMOVE_BOT does. */
static ScnOpResult scenarioOpLobbyRemoveBot(ServerSim *sim,
                                            const ScnOpLobbyRemoveBot *p) {
    ScnOpResult r = scenarioRequireLobby(sim);
    if (r != SCN_OP_OK) return r;
    r = scenarioRemovableBot(sim, p->slot);
    if (r != SCN_OP_OK) return r;

    scenarioTakeBotOut(sim, p->slot);
    return SCN_OP_OK;
}

/* Move a lobby slot to a team, as CMD_TEAM_SET does — the auto-unready
 * included, so a roster change a script makes puts the lobby back to
 * gathering exactly as a host's click does. */
static ScnOpResult scenarioOpLobbySetTeam(ServerSim *sim,
                                          const ScnOpLobbySetTeam *p) {
    ScnOpResult r = scenarioRequireLobby(sim);
    if (r != SCN_OP_OK) return r;
    /* The command arm range-checks the slot and leaves it there, because a
       client can only send its own or the host's pick. A script names any
       slot it likes, so an empty one is refused here. */
    if (p->slot >= MAX_TANKS || !sim->playerConnected[p->slot]) {
        return SCN_OP_NO_SUCH_PLAYER;
    }
    if (p->team >= MAX_TANKS) {
        return SCN_OP_RANGE;
    }

    serverSimSetTeam(sim, p->slot, p->team);
    logAddEvent(log_TeamSet, p->slot, p->team, 0, 0, 0, NULL);
    serverSimPublishLobbySlot(sim, p->slot);
    lobbyAutoUnreadyOnChange(sim);
    return SCN_OP_OK;
}

/* Make a queued spawn.
 *
 * Every question the arm asked is asked again here. The seat may have been
 * taken, the brain file moved, the server's brain changed — and there is
 * nobody left to tell: the script was answered when it asked. A spawn that
 * no longer holds is dropped with a line in the log rather than stalling
 * the changes queued behind it. */
static void scenarioRosterSpawnNow(ServerSim *sim,
                                   const ScnOpRosterSpawnBot *p) {
    const char *brain = NULL;
    char name[PLAYER_NAME_LEN];
    BYTE slot = 0;

    if (scenarioSpawnSeat(sim, p->slot, &slot) != SCN_OP_OK ||
        scenarioBrainPath(sim, p->brain, slot, &brain) != SCN_OP_OK ||
        scenarioBotName(p->name, slot, name, sizeof(name)) != SCN_OP_OK) {
        WB_LOG_WARN(WB_LOG_CAT_SIM,
                    "scenario: queued bot spawn dropped, its seat or brain is gone");
        return;
    }
    /* A named start goes into the scenario's own start slot, which the tank
       create's resolver honours ahead of the placement policy and consumes;
       a start removed since the op was accepted is left to the engine. */
    if (p->start != SCN_NONE &&
        p->start < startsGetNumStarts(&sim->sim.ss) &&
        startsIsActive(&sim->sim.ss, (BYTE)(p->start + 1)) != FALSE) {
        sim->sim.scenarioStartIdx[slot] = p->start;
    }
    /* And a named loadout goes into the scenario's own loadout slot, which
       the spawn-loadout callback reads ahead of the policy and consumes: it
       is for the tank this spawn builds, not a property of the seat. */
    if (p->loadout != 0) {
        sim->sim.scenarioSpawnLoadout[slot] = p->loadout;
    }
    if (!scenarioAddBotInSeat(sim, slot, brain, name, p->team, &p->init)) {
        sim->sim.scenarioStartIdx[slot] = MAX_STARTS;
        sim->sim.scenarioSpawnLoadout[slot] = 0;
        WB_LOG_WARN(WB_LOG_CAT_SIM,
                    "scenario: queued bot spawn for slot %d would not start",
                    (int)slot);
        return;
    }
    if (p->team > 0) {
        logAddEvent(log_TeamSet, slot, p->team, 0, 0, 0, NULL);
    }
}

/* Make a queued removal. A bot that has gone in the meantime — it left, or
 * the round took it — leaves nothing to do. */
static void scenarioRosterRemoveNow(ServerSim *sim, BYTE slot) {
    if (scenarioRemovableBot(sim, slot) != SCN_OP_OK) {
        return;
    }
    scenarioTakeBotOut(sim, slot);
}

void serverSimScenarioDrainRoster(ServerSim *sim) {
    ScnRosterQueueEntry entry;
    bool                was;

    if (sim == NULL || sim->scenarioRosterCount == 0) {
        return;
    }
    /* Only into a running round. The hook that runs just before this drain
       can end the round, and a spawn landing in a round that has just ended
       would put a bot, its brain and its join event into the game-over
       state; the queue is dropped at the next start anyway. */
    if (sim->state != serverStateRunning) {
        return;
    }
    /* Off the queue before it runs, so what the change does to the roster
       cannot be read back out of the entry making it. */
    entry = sim->scenarioRoster[sim->scenarioRosterHead];
    sim->scenarioRosterHead =
        (uint8_t)((sim->scenarioRosterHead + 1) % SCN_ROSTER_QUEUE_MAX);
    sim->scenarioRosterCount--;

    /* Marked as the scenario's, because it is: this queue holds nothing but
       what a script asked for. A bot lands here rather than in the handler
       that asked for it, a tick later, and the join, the lobby slot and the
       tank spawn it publishes on the way in are all the script's doing. */
    was                 = sim->scenarioActing;
    sim->scenarioActing = true;
    if (entry.isSpawn) {
        scenarioRosterSpawnNow(sim, &entry.spawn);
    } else {
        scenarioRosterRemoveNow(sim, entry.removeSlot);
    }
    sim->scenarioActing = was;
}

void serverSimScenarioResetRoster(ServerSim *sim) {
    if (sim == NULL) {
        return;
    }
    sim->scenarioRosterHead = 0;
    sim->scenarioRosterCount = 0;
}

/* ── Comms ─────────────────────────────────────────────────────────────── */

/* The record every server line a script writes leaves behind: the destination
 * it was published with, then the line itself. The viewer has no other way to
 * tell a line the whole game saw from one held to a team or a player, because
 * neither destination byte goes on the wire. */
static void scenarioRecordServerText(BYTE destTeam, BYTE destPlayer,
                                     const char *text) {
    char pstr[1 + SCN_TEXT_MAX];
    size_t len = strlen(text);

    /* The arm has already refused a field with no terminator, so the line is
       inside SCN_TEXT_MAX and its length is inside the one length byte a
       pascal string has. The clamp states that rather than trusting it. */
    if (len > SCN_TEXT_MAX - 1) len = SCN_TEXT_MAX - 1;
    pstr[0] = (char)len;
    memcpy(pstr + 1, text, len);
    logAddEvent(log_ServerText, destTeam, destPlayer, 0, 0, 0, pstr);
}

/* Say something to the whole game, as the server says it. */
static ScnOpResult scenarioOpMsgAll(ServerSim *sim, const ScnOpMsgAll *p) {
    if (!scenarioTextTerminated(p->text, sizeof(p->text))) {
        return SCN_OP_TOO_BIG;
    }

    scenarioRecordServerText(0, 0xFF, p->text);
    publishServerMessage(sim, p->text);
    return SCN_OP_OK;
}

/* Say something to one team. */
static ScnOpResult scenarioOpMsgTeam(ServerSim *sim, const ScnOpMsgTeam *p) {
    /* Team 0 is "everyone" to both delivery filters and SCN_OP_MSG_ALL is the
       op for that, so a script that means the whole game says so rather than
       arriving here with a zero. The upper bound is the one ServerSim.teams[]
       is keyed by: teams run 1..MAX_TANKS-1. */
    if (p->team == 0 || p->team >= MAX_TANKS) {
        return SCN_OP_RANGE;
    }
    if (!scenarioTextTerminated(p->text, sizeof(p->text))) {
        return SCN_OP_TOO_BIG;
    }

    scenarioRecordServerText(p->team, 0xFF, p->text);
    publishServerMessageToTeam(sim, p->text, p->team);
    return SCN_OP_OK;
}

/* Say something to one player. */
static ScnOpResult scenarioOpMsgPlayer(ServerSim *sim,
                                       const ScnOpMsgPlayer *p) {
    ControlEvent evt;

    if (p->slot >= MAX_TANKS || !sim->playerConnected[p->slot]) {
        return SCN_OP_NO_SUCH_PLAYER;
    }
    if (!scenarioTextTerminated(p->text, sizeof(p->text))) {
        return SCN_OP_TOO_BIG;
    }

    scenarioRecordServerText(0, p->slot, p->text);
    /* There is no helper for one player — publishServerMessage and its team
       variant are the two that exist — so the event is built here the way
       those two build theirs, with the slot in destPlayer for the two
       delivery filters to read. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    SDL_strlcpy(evt.u.serverText.text, p->text, sizeof(evt.u.serverText.text));
    evt.u.serverText.destPlayer = p->slot;
    serverSimPublishControl(sim, &evt);
    return SCN_OP_OK;
}

/* Play a sound, at a square or at everyone.
 *
 * 0xFF, 0xFF is the square that is nowhere: soundPickOffer hands it to every
 * listener rather than measuring it. What "everywhere" reaches is every
 * recipient that has a tank, wherever that tank is and whether it is alive or
 * dead — both sound passes skip a recipient with no tank position at all, so a
 * spectator, an unfielded seat and a slot that has not spawned hear nothing.
 *
 * The record is the log_Sound* the callback writes for itself, so this arm
 * writes none of its own. */
static ScnOpResult scenarioOpSound(ServerSim *sim, const ScnOpSound *p) {
    bool everywhere = (p->x == 0xFF && p->y == 0xFF);

    if (p->sound > (BYTE)lobbyPlayerLeave) {
        return SCN_OP_RANGE;
    }
    /* bubbles, tankSinkNear and tankSinkFar reach only the player named in the
       event's fourth byte, and serverSimCbSoundDist fills that byte from
       sim->currentTickPlayer — for an op run from the host's drain, whatever
       the last player of the previous tick happened to be. The op carries no
       field naming a listener, so rather than play one of the three at an
       arbitrary slot, the three are refused. */
    if (p->sound == (BYTE)bubbles || p->sound == (BYTE)tankSinkNear ||
        p->sound == (BYTE)tankSinkFar) {
        return SCN_OP_RANGE;
    }
    if (!everywhere && !scenarioSquareOnMap(p->x, p->y)) {
        return SCN_OP_BAD_SQUARE;
    }

    serverSimCbSoundDist(sim, (sndEffects)p->sound, p->x, p->y);
    return SCN_OP_OK;
}

/* Write a line to the server's console. It goes nowhere else: no event, no
 * record. */
static ScnOpResult scenarioOpLog(ServerSim *sim, const ScnOpLog *p) {
    (void)sim;   /* the console message goes through the active sim */
    if (!scenarioTextTerminated(p->text, sizeof(p->text))) {
        return SCN_OP_TOO_BIG;
    }

    serverSimConsoleMessage(p->text);
    return SCN_OP_OK;
}

/* ── Flow ──────────────────────────────────────────────────────────────
 *
 * Two arms over the round itself: one ends it, one changes how much time is
 * left in it. Both are running-only — there is no round to end or to time
 * from a lobby, and the lobby has its own controls for the length of the
 * round it is about to start. */

/* End the round now, on the script's terms.
 *
 * The text is the line the returning lobby is given. It is held as the
 * pending win message and RETURN_REASON_SCENARIO is what keeps it: without
 * that reason serverSimResolveGameOver falls into the base sweep's arm,
 * which overwrites the line with the sweep's own and credits a WinBolo.net
 * win nobody asked for.
 *
 * The round-end records are the lifecycle's, so this arm writes none of its
 * own. */
static ScnOpResult scenarioOpEndRound(ServerSim *sim, const ScnOpEndRound *p) {
    ScnOpResult r = scenarioRequireRunning(sim);
    if (r != SCN_OP_OK) return r;
    if (!scenarioTextTerminated(p->text, sizeof(p->text))) {
        return SCN_OP_TOO_BIG;
    }
    /* 0 is no winner; otherwise a lobby team, which is a slot number. */
    if (p->winnerTeam >= MAX_TANKS) {
        return SCN_OP_RANGE;
    }

    sim->returnToLobbyReason = RETURN_REASON_SCENARIO;
    /* Nothing reads the team under this reason — its one reader is the
       surrender arm of serverSimResolveGameOver, which credits the side that
       did not give up. The op carries a winner, so it is written where the
       other reasons write theirs rather than dropped on the floor. */
    sim->returnToLobbyTeamId = p->winnerTeam;
    /* pendingWinMessage is 512 bytes and the op's field is SCN_TEXT_MAX, so
       a line that passed the terminator check above always fits. */
    SDL_strlcpy(sim->pendingWinMessage, p->text,
                sizeof(sim->pendingWinMessage));
    serverSimEnterGameOver(sim);
    return SCN_OP_OK;
}

/* Change how much time the running round has left, as a new length or as a
 * delta on the one it has. The setter is a bare write with no clamp of its
 * own, so every bound the length has is checked here.
 *
 * The new length reaches a client through the settings event, which carries
 * gameLength out as lobbyTimeLimit, and reaches a replay through the record
 * below — the settings blob is written once at the head of a round and never
 * restated. */
static ScnOpResult scenarioOpSetGameTime(ServerSim *sim,
                                         const ScnOpSetGameTime *p) {
    int64_t asked;
    ScnOpResult r = scenarioRequireRunning(sim);
    if (r != SCN_OP_OK) return r;

    if (p->relative) {
        /* An endless round has no length to add to: gameLength is -1, and a
           delta on top of that would quietly turn a round with no time limit
           into one of a few ticks. A script that wants to put a limit on such
           a round says what the limit is. */
        if (sim->gameLength == UNLIMITED_GAME_TIME) {
            return SCN_OP_RANGE;
        }
        /* Summed at 64 bits so a delta near the end of the range cannot wrap
           past the bounds below and land back inside them. */
        asked = (int64_t)sim->gameLength + (int64_t)p->ticks;
    } else {
        asked = (int64_t)p->ticks;
    }
    /* Zero is not "no time left". The running tick counts the length down
       only while it is above zero, so a round set to zero never reaches its
       time limit at all — it runs forever, the opposite of what a script
       asking for zero means. One tick is the shortest round that ends. */
    if (asked < 1) {
        return SCN_OP_RANGE;
    }
    /* The field is an int32; a length past the end of it is refused rather
       than truncated into a round of some other length. */
    if (asked > (int64_t)INT32_MAX) {
        return SCN_OP_RANGE;
    }

    serverSimSetGameLength(sim, (int32_t)asked);
    logAddEvent(log_GameTimeSet,
                (BYTE)((asked >> 24) & 0xFF), (BYTE)((asked >> 16) & 0xFF),
                (BYTE)((asked >> 8) & 0xFF), (BYTE)(asked & 0xFF), 0, NULL);
    serverSimPublishLobbySettings(sim);
    return SCN_OP_OK;
}

/* ── Rules ──────────────────────────────────────────────────────────────
 *
 * One arm over the table of gameplay numbers the simulation runs on. */

/* Every field in the table is four bytes wide and SCN_RULE_LIST has a line
 * for each of them, so the struct is exactly as many fields long as the list
 * is entries. A field added to SimRules without a line in the list, a line
 * written twice, or a field that is not four bytes, all fail here rather than
 * leaving an index quietly naming the wrong field. Which field each index
 * names, and that the list is in the struct's order, is the offsets case in
 * tests/unit/test_scenario_rule_arms.c. */
BOLO_STATIC_ASSERT(sizeof(SimRules) == SCN_RULE_COUNT * sizeof(int32_t),
                   scn_rule_list_covers_sim_rules);

/* One switch arm per rule, generated from the list the index enum is
 * generated from, so the two cannot name different fields. The assignment
 * converts the caller's double to whatever the field is declared as — an
 * integer rule takes the whole part of it, a float rule takes the value — and
 * reading it straight back out says what the field ended up holding, which is
 * what is checked below and what the record carries. */
#define SCN_RULE_WRITE_CASE(name)                                            \
    case SCN_RULE_##name:                                                    \
        copy->name = value;                                                  \
        *written   = (double)copy->name;                                     \
        break;

/* One rule into a table the caller owns, with the refusals a write can answer
 * on its own. The op that sets a rule and the call that asks what a set would
 * do both come through here, so a value becomes a field the same way whichever
 * of them is asking.
 *
 * A NaN, and anything past the int32 window, cannot be converted to a field's
 * type at all — that conversion is undefined rather than wrong — so both are
 * refused before the write instead of checked after it. Written as a negated
 * in-range test so a NaN fails it. No row's range comes near either end; every
 * bound a row actually has is stated by simRulesCheck and by nothing here. */
static ScnOpResult scenarioRuleWrite(SimRules *copy, uint16_t rule,
                                     double value, double *written) {
    /* An index past the end of the list names no rule, which is what every
       other arm answers SCN_OP_NO_SUCH_ITEM for. */
    if (rule >= SCN_RULE_COUNT) {
        return SCN_OP_NO_SUCH_ITEM;
    }
    if (!(value >= -2147483648.0 && value <= 2147483647.0)) {
        return SCN_OP_RANGE;
    }

    switch (rule) {
        SCN_RULE_LIST(SCN_RULE_WRITE_CASE)
        default:
            /* Unreachable: the bounds check above has already passed, and
               the cases come from the list the enum comes from. */
            return SCN_OP_NO_SUCH_ITEM;
    }
    return SCN_OP_OK;
}

/* What a fault the check answered is refused as. No table is not reachable
 * from either caller — the one checked is on the caller's own stack — and
 * there is no result code for it, so it answers as the range refusal it would
 * have to be reported as anyway. */
static ScnOpResult scenarioRuleFaultResult(SimRulesFault fault) {
    switch (fault) {
        case SIM_RULES_OK:
            return SCN_OP_OK;
        case SIM_RULES_FAULT_PAIR:
            return SCN_OP_PAIR;
        case SIM_RULES_FAULT_RANGE:
        case SIM_RULES_FAULT_NO_TABLE:
            return SCN_OP_RANGE;
    }
    return SCN_OP_RANGE;
}

/* The record: which rule, and the value the field ended up holding. */
BOLO_STATIC_ASSERT(sizeof(double) == 8, scn_rule_value_is_eight_bytes);

static void scenarioRecordRuleSet(uint16_t rule, double written) {
    uint64_t bits;
    char     blob[9];
    int      i;

    /* The value as the eight bytes of its IEEE-754 double, most significant
       first. Every rule fits one exactly — an int32_t field's whole range and
       every value a float field can hold — so the record states what the
       table holds rather than a scaled approximation of it. The bit pattern
       is serialised as an integer, so a host's own byte order does not reach
       the file. */
    memcpy(&bits, &written, sizeof(bits));
    blob[0] = 8;
    for (i = 0; i < 8; i++) {
        blob[1 + i] = (char)((bits >> (56 - 8 * i)) & 0xFF);
    }
    logAddEvent(log_RuleSet, 0, 0, 0, 0, rule, blob);
}

/* Whether a rule is one CTRL_SIM_RULES carries. A change to a rule only the
 * server reads publishes nothing: no client holds a value for it, so there
 * is nothing out there to correct. Written from the event's own field lists,
 * so a rule that starts being carried starts being republished here with no
 * second edit. */
static bool scenarioRuleIsCarried(uint16_t rule) {
    switch (rule) {
#define SCN_RULE_CARRIED_CASE(name) case SCN_RULE_##name: return true;
        CTRL_SIM_RULES_ALL_FIELDS(SCN_RULE_CARRIED_CASE)
#undef SCN_RULE_CARRIED_CASE
        default: return false;
    }
}

/* Bring the world back inside a table that has just changed. A new table
 * that lowers a cap leaves the records standing above it — a pill at 15
 * armour under a new pill_max_armour of 8, a base at 90 shells under a new
 * base_full_shells of 40 — and the pass that caps them is the one a sim runs
 * when it takes a map on, which is the same question asked the other way
 * round. It is called whatever rule changed rather than behind a test of
 * which rules bear on a cap: it is idempotent, it is sixteen pills and
 * sixteen bases, and a list of the rules that matter is one more list that
 * can drift from the clamp it guards.
 *
 * The records belong to this handler rather than to the pass. The pass is
 * shared with the load path, which has its records from elsewhere, so what
 * the clamp moved is read off either side of it here and stated with the
 * records those fields already have. A clamp that moved nothing writes
 * nothing.
 *
 * A removed slot is stated like any other. The viewer keeps a tombstone's
 * record as the list does, so a slot the clamp moved and nothing stated
 * would be the one place the two lists differ.
 *
 * A pill's speed and its cooldown have no record in the stream at all — no
 * path writes one and the viewer holds no field for either — so the clamp
 * can move those two and say nothing about them. That is how far the format
 * reaches rather than something this handler drops. */
static void scenarioClampWorldToRules(ServerSim *sim) {
    BYTE    pillArmour[MAX_PILLS];
    BYTE    baseArmour[MAX_BASES];
    BYTE    baseShells[MAX_BASES];
    BYTE    baseMines[MAX_BASES];
    BYTE    numPills = pillsGetNumPills(&sim->sim.pb);
    BYTE    numBases = basesGetNumBases(&sim->sim.bs);
    BYTE    i;
    pillbox pill;
    base    item;

    for (i = 0; i < numPills; i++) {
        memset(&pill, 0, sizeof(pill));
        pillsGetPill(&sim->sim.pb, &pill, (BYTE)(i + 1));
        pillArmour[i] = pill.armour;
    }
    for (i = 0; i < numBases; i++) {
        memset(&item, 0, sizeof(item));
        basesGetBase(&sim->sim.bs, &item, (BYTE)(i + 1));
        baseArmour[i] = item.armour;
        baseShells[i] = item.shells;
        baseMines[i]  = item.mines;
    }

    mapClampToRules(&sim->sim);

    /* Both records name their item counting from zero, which is what every
       other place that writes one passes. */
    for (i = 0; i < numPills; i++) {
        memset(&pill, 0, sizeof(pill));
        pillsGetPill(&sim->sim.pb, &pill, (BYTE)(i + 1));
        if (pill.armour != pillArmour[i]) {
            logAddEvent(log_PillSetHealth, i, pill.armour, 0, 0, 0, NULL);
        }
    }
    /* One record carries all three stocks, so a base whose clamp moved any of
       them is stated once. */
    for (i = 0; i < numBases; i++) {
        memset(&item, 0, sizeof(item));
        basesGetBase(&sim->sim.bs, &item, (BYTE)(i + 1));
        if (item.armour != baseArmour[i] || item.shells != baseShells[i] ||
            item.mines != baseMines[i]) {
            logAddEvent(log_BaseSetStock, i, item.shells, item.mines,
                        item.armour, 0, NULL);
        }
    }
}

/* Write one rule. The write lands in a copy of the sim's table, the copy is
 * checked whole, and only a copy that passes is committed: a refused op
 * leaves the sim's table byte for byte as it was rather than half-applied.
 *
 * A committed change to a rule clients read is published, so their tables
 * follow the server's within the tick. A change to a server-only rule
 * publishes nothing.
 *
 * The three things a commit is followed by are in the order a reader of
 * either channel wants them. The rule's own record goes first, so a replay
 * reads the change before what it stranded rather than after it. The clamp
 * comes next, which leaves the publish last: the event is the instruction to
 * every client to run that same clamp, and sending it once this server's own
 * records are already inside the table means a subscriber that reads the sim
 * from its callback — which is what the entity publishes above promise it —
 * never sees a world the event it is holding contradicts.
 *
 * No state check. A rule belongs to the simulation rather than to a round,
 * the way a pill belongs to the map, so a lobby setting its table up and a
 * round changing a number mid-play are both ordinary. */
static ScnOpResult scenarioOpSetRule(ServerSim *sim, const ScnOpSetRule *p) {
    SimRules      copy    = sim->sim.rules;
    double        written = 0.0;
    SimRulesFault fault;
    ScnOpResult   r;
    char          why[SIM_RULES_WHY_LEN];

    r = scenarioRuleWrite(&copy, p->rule, p->value, &written);
    if (r != SCN_OP_OK) {
        return r;
    }

    fault = simRulesCheck(&copy, why, sizeof(why));
    if (fault != SIM_RULES_OK) {
        /* The reason names the row and the bound it missed, which is the only
           place a script author is told which of the table's numbers it was
           and what it had to be. */
        char line[SIM_RULES_WHY_LEN + 32];
        SDL_snprintf(line, sizeof(line), "rule refused: %s", why);
        serverSimConsoleMessage(line);
        return scenarioRuleFaultResult(fault);
    }

    sim->sim.rules = copy;
    scenarioRecordRuleSet(p->rule, written);
    scenarioClampWorldToRules(sim);
    /* Held while the setup window is open, which covers both the boot the
       start makes ahead of its batch — where a script's own rules table is
       applied — and the round-start callback after it. The start publishes
       the table itself at its end, and that publish carries every field, so
       a scenario setting a dozen rules there would otherwise send a dozen
       control events where one says the same thing, and a round would state
       its table twice. Outside the window no publish follows, so the change
       states itself. */
    if (scenarioRuleIsCarried(p->rule) && !sim->scenarioSetupWindow) {
        serverSimPublishSimRules(sim);
    }
    return SCN_OP_OK;
}

/* The same question the arm asks, without the answer landing anywhere. The
 * whole set is written into the copy before the check reads it, so a pair two
 * of the values break together is found although each of them passes alone —
 * which is what a script's rules table needs asking of it before a round is
 * ever started on it. */
ScnOpResult serverSimCheckScenarioRules(const ServerSim *sim,
                                        const uint16_t *rules,
                                        const double *values,
                                        uint16_t count,
                                        char *why, size_t whyLen) {
    SimRules      copy;
    SimRulesFault fault;
    uint16_t      i;
    char          reason[SIM_RULES_WHY_LEN];

    if (why != NULL && whyLen > 0) {
        why[0] = '\0';
    }
    if (sim == NULL) {
        return SCN_OP_BAD_CALL;
    }
    if (count > 0 && (rules == NULL || values == NULL)) {
        return SCN_OP_BAD_CALL;
    }

    copy = sim->sim.rules;
    for (i = 0; i < count; i++) {
        double      written = 0.0;
        ScnOpResult r = scenarioRuleWrite(&copy, rules[i], values[i], &written);
        if (r != SCN_OP_OK) {
            return r;
        }
    }

    fault = simRulesCheck(&copy, reason, sizeof(reason));
    if (fault != SIM_RULES_OK && why != NULL && whyLen > 0) {
        SDL_snprintf(why, whyLen, "%s", reason);
    }
    return scenarioRuleFaultResult(fault);
}

#undef SCN_RULE_WRITE_CASE

/* Reading one back. Written from the list the write cases come from, so the
 * index a script sets a rule by and the index it reads the same rule by
 * cannot name different fields. Each field is converted to the double the op
 * carries, which holds every value any of them can. */
#define SCN_RULE_READ_CASE(name)                                             \
    case SCN_RULE_##name:                                                    \
        *out = (double)sim->sim.rules.name;                                  \
        return true;

bool serverSimGetScenarioRule(const ServerSim *sim, uint16_t rule,
                              double *out) {
    if (sim == NULL || out == NULL || rule >= SCN_RULE_COUNT) {
        return false;
    }
    switch (rule) {
        SCN_RULE_LIST(SCN_RULE_READ_CASE)
        default:
            /* Unreachable: the bounds test above has already passed, and the
               cases come from the list the enum comes from. */
            return false;
    }
}

#undef SCN_RULE_READ_CASE

/* The six ops that change who is in the round. The start-in-progress guard
 * below exists for exactly these: a roster edit made from inside a start
 * re-enters the all-ready detector with every player still ready, which
 * would begin a second round on top of the one being set up. They are
 * therefore the ops the setup window does not admit, and this is the one
 * place the set is written down. */
static bool scenarioOpIsRoster(ScenarioOpType t) {
    return t == SCN_OP_ROSTER_SPAWN_BOT ||
           t == SCN_OP_ROSTER_REMOVE_BOT ||
           t == SCN_OP_ROSTER_SET_TEAM ||
           t == SCN_OP_LOBBY_ADD_BOT ||
           t == SCN_OP_LOBBY_REMOVE_BOT ||
           t == SCN_OP_LOBBY_SET_TEAM;
}

/* The prelude and the handler for one op, with the caller holding the actor
 * mark across the whole of it. Split from the entry point below so the mark
 * goes on once and comes off once however the op ends: every case returns
 * where it stands. */
static ScnOpResult scenarioApplyOp(ServerSim *sim, const ScenarioOp *op,
                                   ScnOpOut *out) {
    /* A policy callback is a question the engine asks mid-operation. It
     * answers and nothing else: an op from inside one would mutate state
     * the caller is halfway through reading. A depth, not a flag, so a
     * question asked inside another does not open the funnel when the
     * inner one returns. */
    if (sim->inScenarioPolicy > 0) {
        return SCN_OP_IN_POLICY;
    }

    /* A start is not a settled point. The roster, the tanks and the
     * state are all being rebuilt, so nothing may be written until it
     * finishes.
     *
     * The setup window is the exception. It is open across each of the
     * two calls a start makes into the scenario — the boot, where the
     * round's state and its rules come into force ahead of the start
     * batch, and the round-start callback the start makes once the
     * world, the tanks and the roster are built and the state already
     * reads running. The six roster ops are the exception to that: what
     * the guard exists for is a roster edit re-entering the all-ready
     * detector, which is as true inside the window as outside it, so
     * they keep their refusal either way. */
    if (sim->startInProgress &&
        (!sim->scenarioSetupWindow || scenarioOpIsRoster(op->type))) {
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
        case SCN_OP_ROSTER_SPAWN_BOT:
            return scenarioOpRosterSpawnBot(sim, &op->u.rosterSpawnBot, out);
        case SCN_OP_ROSTER_REMOVE_BOT:
            return scenarioOpRosterRemoveBot(sim, &op->u.rosterRemoveBot);
        case SCN_OP_ROSTER_SET_TEAM:
            return scenarioOpRosterSetTeam(sim, &op->u.rosterSetTeam);
        case SCN_OP_LOBBY_ADD_BOT:
            return scenarioOpLobbyAddBot(sim, &op->u.lobbyAddBot, out);
        case SCN_OP_LOBBY_REMOVE_BOT:
            return scenarioOpLobbyRemoveBot(sim, &op->u.lobbyRemoveBot);
        case SCN_OP_LOBBY_SET_TEAM:
            return scenarioOpLobbySetTeam(sim, &op->u.lobbySetTeam);
        case SCN_OP_BOT_HINT:            return SCN_OP_UNSUPPORTED;
        case SCN_OP_MSG_ALL:
            return scenarioOpMsgAll(sim, &op->u.msgAll);
        case SCN_OP_MSG_TEAM:
            return scenarioOpMsgTeam(sim, &op->u.msgTeam);
        case SCN_OP_MSG_PLAYER:
            return scenarioOpMsgPlayer(sim, &op->u.msgPlayer);
        case SCN_OP_SOUND:
            return scenarioOpSound(sim, &op->u.sound);
        case SCN_OP_LOG:
            return scenarioOpLog(sim, &op->u.log);
        case SCN_OP_PANEL:               return SCN_OP_UNSUPPORTED;
        case SCN_OP_SCORE:               return SCN_OP_UNSUPPORTED;
        case SCN_OP_ANNOUNCE:            return SCN_OP_UNSUPPORTED;
        case SCN_OP_MARKER:              return SCN_OP_UNSUPPORTED;
        case SCN_OP_END_ROUND:
            return scenarioOpEndRound(sim, &op->u.endRound);
        case SCN_OP_SET_GAME_TIME:
            return scenarioOpSetGameTime(sim, &op->u.setGameTime);
        case SCN_OP_SET_RULE:
            return scenarioOpSetRule(sim, &op->u.setRule);
    }

    /* A value that is not a member of the enum at all. */
    return SCN_OP_UNSUPPORTED;
}

ScnOpResult serverSimApplyScenarioOp(ServerSim *sim, const ScenarioOp *op,
                                     ScnOpOut *out) {
    ScnOpResult r;
    bool        was;

    /* Loud in a development build, because either of these is a caller bug
     * and the host that made it should hear about it at once. */
    assert(sim != NULL);
    assert(op != NULL);
    /* And survivable in a shipped one, where the asserts above are gone:
     * RelWithDebInfo carries -DNDEBUG. This is the call an out-of-process
     * scenario host makes most, and a host that hands over a pointer it
     * failed to resolve would otherwise take the whole server down and every
     * player in the round with it. The other entry points below answer a
     * NULL sim the same way. */
    if (sim == NULL || op == NULL) {
        return SCN_OP_BAD_CALL;
    }

    /* Everything a handler publishes is the scenario's doing, and this is
     * what says so. Saved and put back rather than cleared, because the
     * funnel can be reached from inside itself — a handler's placement asks
     * a policy and a policy runs the script's own Lua — and the outer op is
     * still the script's whatever the inner call did. The mark is held
     * across the prelude's refusals as well, which costs nothing: a refused
     * op publishes nothing for it to reach. */
    was                 = sim->scenarioActing;
    sim->scenarioActing = true;
    r                   = scenarioApplyOp(sim, op, out);
    sim->scenarioActing = was;
    return r;
}

bool serverSimIsScenarioActing(const ServerSim *sim) {
    return sim != NULL && sim->scenarioActing;
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

void serverSimSetScenarioRoundBoot(ServerSim *sim, void (*roundBoot)(void *ctx),
                                   void *ctx) {
    if (sim == NULL) return;
    sim->scenarioRoundBoot = roundBoot;
    sim->scenarioRoundBootCtx = ctx;
}

void serverSimSetScenarioRoundStart(ServerSim *sim, void (*roundStart)(void *ctx),
                                    void *ctx) {
    if (sim == NULL) return;
    sim->scenarioRoundStart = roundStart;
    sim->scenarioRoundStartCtx = ctx;
}

void serverSimSetScenarioReload(ServerSim *sim,
                                bool (*reload)(void *ctx, char *err,
                                               size_t errLen),
                                void *ctx) {
    if (sim == NULL) return;
    sim->scenarioReload = reload;
    sim->scenarioReloadCtx = ctx;
}

bool serverSimScenarioReload(ServerSim *sim, char *err, size_t errLen) {
    if (err != NULL && errLen > 0) err[0] = '\0';
    if (sim == NULL || sim->scenarioReload == NULL) {
        if (err != NULL && errLen > 0) {
            snprintf(err, errLen, "No scenario is attached to this map");
        }
        return false;
    }
    return sim->scenarioReload(sim->scenarioReloadCtx, err, errLen);
}

void serverSimSetScenarioMapScripted(ServerSim *sim,
                                     bool (*mapScripted)(void *ctx,
                                                         const char *mapPath),
                                     void *ctx) {
    if (sim == NULL) return;
    sim->scenarioMapScripted = mapScripted;
    sim->scenarioMapScriptedCtx = ctx;
}

bool serverSimScenarioMapIsScripted(const ServerSim *sim, const char *mapPath) {
    if (sim == NULL || sim->scenarioMapScripted == NULL || mapPath == NULL) {
        return false;
    }
    return sim->scenarioMapScripted(sim->scenarioMapScriptedCtx, mapPath);
}

void serverSimSetScenarioMapChanged(ServerSim *sim,
                                    void (*mapChanged)(void *ctx,
                                                       ServerSim *sim,
                                                       const char *mapPath),
                                    void *ctx) {
    if (sim == NULL) return;
    sim->scenarioMapChanged = mapChanged;
    sim->scenarioMapChangedCtx = ctx;
}

void serverSimSetScenarioLobbyTemplate(ServerSim *sim,
                                       const ScnLobbyTemplate *t) {
    if (sim == NULL) return;
    if (t == NULL) {
        memset(&sim->scenarioLobby, 0, sizeof(sim->scenarioLobby));
        sim->scenarioLobbyValid = false;
        /* A plain map committed after a scripted one must not keep the old
           scenario's base game type. */
        sim->sim.scenarioBaseGame = (gameType)0;
        return;
    }
    sim->scenarioLobby = *t;
    if (sim->scenarioLobby.numTeams > MAX_TANKS) {
        sim->scenarioLobby.numTeams = MAX_TANKS;
    }
    sim->scenarioLobbyValid = true;
    /* The one value on the template the sim core reads directly, so it is
       kept where the spawn and start paths can see it without a ServerSim. */
    sim->sim.scenarioBaseGame = (gameType)t->baseGameType;
}

void serverSimSetScenarioIdentity(ServerSim *sim,
                                  LobbyScenarioSource source,
                                  const char *name,
                                  const char *fileName,
                                  const char *description,
                                  bool extraTeams) {
    if (sim == NULL) return;
    memset(&sim->scenarioIdentity, 0, sizeof(sim->scenarioIdentity));
    if (source == lobbyScenarioNone) {
        /* A detach, or a map with nothing beside it. Everything else the
           caller passed goes with it rather than being kept beside a source
           that says there is no scenario. */
        return;
    }
    sim->scenarioIdentity.source     = source;
    sim->scenarioIdentity.extraTeams = extraTeams;
    if (name != NULL) {
        strncpy(sim->scenarioIdentity.name, name,
                sizeof(sim->scenarioIdentity.name) - 1);
    }
    if (fileName != NULL) {
        strncpy(sim->scenarioIdentity.fileName, fileName,
                sizeof(sim->scenarioIdentity.fileName) - 1);
    }
    if (description != NULL) {
        strncpy(sim->scenarioIdentity.description, description,
                sizeof(sim->scenarioIdentity.description) - 1);
    }
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
    assert(sim->inScenarioPolicy < UINT8_MAX);
    sim->inScenarioPolicy++;
}

void serverSimScenarioPolicyLeave(ServerSim *sim) {
    if (sim == NULL) return;
    assert(sim->inScenarioPolicy > 0);
    if (sim->inScenarioPolicy > 0) {
        sim->inScenarioPolicy--;
    }
}
