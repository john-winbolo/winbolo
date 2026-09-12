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
 *  non-public surface. It is on the include path of
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
