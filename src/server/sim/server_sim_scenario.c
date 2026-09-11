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
#include "tank.h"          /* tankSetModifiers — the set-modifiers arm */
#include "log.h"           /* logAddEvent — the arm's record */

/* SCN_PANEL_MAX is written as a literal on the scenario surface, which
 * cannot see the channel sizes. This is where the two meet: one panel
 * list plus the panel event's header plus the segment's own framing has
 * to fit a single control segment, which is what channel_mux rejects a
 * larger message against. */
BOLO_STATIC_ASSERT(
    1 + 2 + 1 + 1 + 2 + SCN_PANEL_MAX <= CHANNEL_CONTROL_SEG,
    scn_panel_max_fits_one_control_segment);

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
        case SCN_OP_TANK_SET_STOCKS:     return SCN_OP_UNSUPPORTED;
        case SCN_OP_TANK_KILL:           return SCN_OP_UNSUPPORTED;
        case SCN_OP_TANK_TELEPORT:       return SCN_OP_UNSUPPORTED;
        case SCN_OP_TANK_SET_BOAT:       return SCN_OP_UNSUPPORTED;
        case SCN_OP_TANK_GIVE_PILL:      return SCN_OP_UNSUPPORTED;
        case SCN_OP_TANK_DROP_PILL:      return SCN_OP_UNSUPPORTED;
        case SCN_OP_TANK_SET_MODIFIERS:
            return scenarioOpTankSetModifiers(sim, &op->u.tankSetModifiers);
        case SCN_OP_LGM_DISPATCH:        return SCN_OP_UNSUPPORTED;
        case SCN_OP_LGM_RECALL:          return SCN_OP_UNSUPPORTED;
        case SCN_OP_LGM_KILL:            return SCN_OP_UNSUPPORTED;
        case SCN_OP_LGM_PARACHUTE:       return SCN_OP_UNSUPPORTED;
        case SCN_OP_LGM_SET_CARRIED:     return SCN_OP_UNSUPPORTED;
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
