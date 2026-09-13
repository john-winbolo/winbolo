/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Server Simulation Scenario Surface
 *Filename:      server_sim_scenario.h
 *Author:        John Morrison
 *Purpose:
 *  The write and policy door a scenario reaches the sim
 *  through: one typed op funnel, the policy vtable
 *  registration, the per-tick callback the host drains its
 *  event queue from, and the opaque state pointer the host
 *  hangs its own data off.
 *
 *  Reads are public accessors on server_sim.h and events
 *  are the control bus, so this header is the whole of the
 *  non-public surface. The one read here is the rules
 *  table, because the index that names a rule is this
 *  surface's own. It is on the include path of
 *  scenario_host and of the privileged profiles, and not of
 *  gui or runtime_only.
 *********************************************************/

#ifndef SERVER_SIM_SCENARIO_H
#define SERVER_SIM_SCENARIO_H

#include "server_sim.h"
#include "scenario_defs.h"

/*********************************************************
 *NAME:          serverSimApplyScenarioOp
 *PURPOSE:
 *  Applies one scenario op and returns what happened. An op
 *  either applies in full or not at all: validation finishes
 *  before the first mutation. Ops issued from inside a policy
 *  callback, or while a game start is running, are refused
 *  before the op is looked at.
 *
 *  out may be NULL. When it is not, an entity add writes the
 *  index it took and a spawn writes the seat it took.
 *********************************************************/
ScnOpResult serverSimApplyScenarioOp(ServerSim *sim, const ScenarioOp *op,
                                     ScnOpOut *out);

/*********************************************************
 *NAME:          serverSimGetScenarioRule
 *PURPOSE:
 *  What one rule of the simulation's table is set to, as the
 *  double the set-rule op carries a value in. rule is a
 *  ScnRuleIndex; an index that names no rule returns false
 *  and leaves *out alone.
 *
 *  The double is exact for every integer rule in the table
 *  and for every value a float rule can hold, so this reads
 *  back what a set-rule op wrote rather than an
 *  approximation of it.
 *********************************************************/
bool serverSimGetScenarioRule(const ServerSim *sim, uint16_t rule,
                              double *out);

/*********************************************************
 *NAME:          serverSimSetScenarioPolicy
 *PURPOSE:
 *  Registers the vtable the sim asks its scenario decisions
 *  through. NULL clears it and returns the sim to classic
 *  rules. The sim stores the pointer and neither copies nor
 *  owns the struct, so it must outlive the registration.
 *********************************************************/
void serverSimSetScenarioPolicy(ServerSim *sim, const ScenarioPolicy *p);

/*********************************************************
 *NAME:          serverSimSetScenarioTick
 *PURPOSE:
 *  Registers the callback serverSimTick invokes at the end
 *  of each frame, in both the running and the non-running
 *  branch. The host drains its queued events from it. NULL
 *  clears it.
 *********************************************************/
void serverSimSetScenarioTick(ServerSim *sim, void (*tick)(void *ctx),
                              void *ctx);

/*********************************************************
 *NAME:          serverSimSetScenarioRoundStart
 *PURPOSE:
 *  Registers the callback both authoritative round starts
 *  invoke, after the world and the roster are built and
 *  before the round's CTRL_SIM_RULES publish. The setup
 *  window is open across the call, so the funnel takes the
 *  ops it issues although the start is still in progress.
 *  NULL clears it.
 *********************************************************/
void serverSimSetScenarioRoundStart(ServerSim *sim, void (*roundStart)(void *ctx),
                                    void *ctx);

/*********************************************************
 *NAME:          serverSimSetScenarioState
 *PURPOSE:
 *  Stores the host's opaque state pointer on the sim. The
 *  sim never dereferences it; it is the one place scenario
 *  state lives so no engine struct carries any.
 *********************************************************/
void serverSimSetScenarioState(ServerSim *sim, void *state);

/*********************************************************
 *NAME:          serverSimGetScenarioState
 *PURPOSE:
 *  Returns the pointer serverSimSetScenarioState stored, or
 *  NULL when no scenario is attached.
 *********************************************************/
void *serverSimGetScenarioState(const ServerSim *sim);

#endif /* SERVER_SIM_SCENARIO_H */
