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
 *  The actor mark serverSimIsScenarioActing answers is held
 *  across the whole call, so everything the op publishes
 *  reaches a host's subscriber marked as the script's.
 *
 *  out may be NULL. When it is not, an entity add writes the
 *  index it took and a spawn writes the seat it took.
 *********************************************************/
ScnOpResult serverSimApplyScenarioOp(ServerSim *sim, const ScenarioOp *op,
                                     ScnOpOut *out);

/*********************************************************
 *NAME:          serverSimCheckScenarioRules
 *PURPOSE:
 *  Answers what setting these rules would do, without
 *  setting any of them. Each (rule, value) pair is written
 *  into a copy of the sim's table through the same write
 *  cases the set-rule op uses, and the copy is checked once
 *  when they are all in: SCN_OP_OK for a table that stands,
 *  SCN_OP_RANGE for a value outside its row's bounds,
 *  SCN_OP_PAIR for one that breaks a pair, SCN_OP_NO_SUCH_ITEM
 *  for an index that names no rule, and SCN_OP_BAD_CALL for a
 *  NULL sim or a count with no arrays behind it.
 *
 *  Several at once is the point: two values that each pass on
 *  their own can break the pair they share, and the whole set
 *  is written before the check reads it.
 *
 *  The sim is not touched. Nothing is published, nothing is
 *  recorded and no operator line is written. why takes the
 *  reason the check gave on a fault and "" otherwise; it may
 *  be NULL only when whyLen is 0.
 *********************************************************/
ScnOpResult serverSimCheckScenarioRules(const ServerSim *sim,
                                        const uint16_t *rules,
                                        const double *values,
                                        uint16_t count,
                                        char *why, size_t whyLen);

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
 *NAME:          serverSimSetScenarioLobbyTemplate
 *PURPOSE:
 *  Hands the sim the lobby a scenario asks for. The sim
 *  copies it, so the caller's struct need not outlive the
 *  call, and then owns seating and reconciling it at every
 *  point a lobby is built or rebuilt — with no call back
 *  into whoever read it. NULL clears it, which returns the
 *  lobby to an ordinary one.
 *
 *  Setting it does not seat anything by itself. The map
 *  commit seats it; a caller that wants the seats without a
 *  map change asks for them.
 *********************************************************/
void serverSimSetScenarioLobbyTemplate(ServerSim *sim,
                                       const ScnLobbyTemplate *t);

/*********************************************************
 *NAME:          serverSimSetScenarioMapChanged
 *PURPOSE:
 *  Registers the callback a committed map change invokes,
 *  with the new map's file path, before the sim seats the
 *  lobby. Whoever registers it is expected to drop the
 *  scenario the previous map had, look for one beside the
 *  new map, and set or clear the lobby template accordingly;
 *  the sim reads the template again the moment the call
 *  returns. NULL clears it.
 *
 *  mapPath is "" when the new map came from bytes rather
 *  than a file — an upload, a generated random map, or a
 *  preview rolled back — which is the case where there is
 *  nothing to look beside.
 *
 *  The sim goes down the call beside the path so the
 *  context can be something that outlives any one scenario:
 *  the callback is where a scenario is torn down and
 *  replaced, so it cannot be the scenario itself.
 *********************************************************/
void serverSimSetScenarioMapChanged(ServerSim *sim,
                                    void (*mapChanged)(void *ctx,
                                                       ServerSim *sim,
                                                       const char *mapPath),
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
