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
#include "control_event.h"  /* LobbyScenarioSource — the identity setter's source */

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
 *NAME:          scenarioCheckRulesFromClassic
 *PURPOSE:
 *  The same question as above, asked against the classic
 *  table rather than against a round in progress. A rule's
 *  bounds and the pairs it sits in belong to the table the
 *  field is declared in, not to a game, so a set of values
 *  can be checked with no sim to check it against.
 *
 *  This is what a check with no game running has to use: a
 *  map editor holds a script and no ServerSim, and the
 *  entry above answers SCN_OP_BAD_CALL to a NULL one.
 *
 *  The answers are the entry above's, less the refusal for
 *  a NULL sim: SCN_OP_OK, SCN_OP_RANGE, SCN_OP_PAIR,
 *  SCN_OP_NO_SUCH_ITEM, and SCN_OP_BAD_CALL for a count
 *  with no arrays behind it. why takes the reason the check
 *  gave on a fault and "" otherwise; it may be NULL only
 *  when whyLen is 0.
 *********************************************************/
ScnOpResult scenarioCheckRulesFromClassic(const uint16_t *rules,
                                          const double *values,
                                          uint16_t count,
                                          char *why, size_t whyLen);

/*********************************************************
 *NAME:          serverSimScenarioFillWorldToRules
 *PURPOSE:
 *  Starts every pill and base at the caps the sim's table
 *  holds, instead of at the numbers the map file holds.
 *
 *  A map states a number for each pill's armour and each
 *  base's stocks and cannot state "full", so a scenario
 *  that raises a cap gets a map still carrying its author's
 *  numbers. This is what a scenario asking fill_to_caps is
 *  answered with, and it is called once the scenario's own
 *  rules are in the table: run before them it would fill to
 *  the caps that are on their way out.
 *
 *  Raising only — anything at or above a cap is left alone,
 *  and bringing what is above one down is the clamp every
 *  rule change already runs. Idempotent. What moves is
 *  recorded the way that clamp records it, so a replay
 *  reads the world the round opened on.
 *
 *  A pill's firing rate is not touched: an attack interval
 *  is a rate rather than a stock.
 *********************************************************/
void serverSimScenarioFillWorldToRules(ServerSim *sim);

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
 *NAME:          serverSimSetScenarioRoundBoot
 *PURPOSE:
 *  Registers the callback both authoritative round starts
 *  invoke before the start batch and the tanks, which is
 *  where the round's own Lua state, its chunk and its rules
 *  come into force. The policies asked while the opening
 *  tanks are built are therefore the round's own.
 *
 *  The setup window is open across the call, as it is across
 *  the round start below, so the funnel takes the ops it
 *  issues and the rules it sets wait for the one publish the
 *  start makes at its end. NULL clears it.
 *********************************************************/
void serverSimSetScenarioRoundBoot(ServerSim *sim, void (*roundBoot)(void *ctx),
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
 *NAME:          serverSimSetScenarioReload
 *PURPOSE:
 *  Registers what a lobby host's reload request runs. The
 *  sim cannot re-read a script itself — that is the scenario
 *  library's, and the dependency points one way — so the
 *  host leaves this behind the way it leaves the round start.
 *
 *  Answers true when the script was re-read, and false with
 *  err saying why when it was not. With nothing registered
 *  it answers false and err says there is no scenario, which
 *  is what a lobby on a plain map gets.
 *
 *  What a reload changes takes effect at the next round; the
 *  round in progress keeps what it started with.
 *
 *  NULL clears it.
 *********************************************************/
void serverSimSetScenarioReload(ServerSim *sim,
                                bool (*reload)(void *ctx, char *err,
                                               size_t errLen),
                                void *ctx);

/*********************************************************
 *NAME:          serverSimScenarioReload
 *PURPOSE:
 *  Asks whoever owns the scenario to read its script again,
 *  through the callback above. False with err filled when
 *  there is nothing registered or the re-read failed.
 *********************************************************/
bool serverSimScenarioReload(ServerSim *sim, char *err, size_t errLen);

/*********************************************************
 *NAME:          serverSimSetScenarioMapScripted
 *PURPOSE:
 *  Registers the question the map lister asks of each map it
 *  finds: would a round on this one here play by a script.
 *  The answer tags an entry so a player can see which maps
 *  are scripted before picking one.
 *
 *  A callback rather than a call, for the reason the round
 *  start is one: finding a script is the scenario library's
 *  to know and the sim is below it. NULL clears it, and with
 *  nothing registered every map answers unscripted — which is
 *  what a build with no scenario library reports.
 *
 *  Registered once, where the process decides whether it runs
 *  scripts at all, not where a scenario attaches: an attach
 *  answers NULL for a map with no script, so registering
 *  there would leave a plain map's server reporting every
 *  scripted map in its directory as plain.
 *
 *  mapPath is the full path to the .map file, which is what
 *  the lister holds and the wire layer does not.
 *********************************************************/
void serverSimSetScenarioMapScripted(ServerSim *sim,
                                     bool (*mapScripted)(void *ctx,
                                                         const char *mapPath),
                                     void *ctx);

/*********************************************************
 *NAME:          serverSimSetScenarioLister
 *PURPOSE:
 *  Registers the read of the server's scenarios directory:
 *  what a client is told is on offer when it asks for the
 *  list. The lister answers how many entries it wrote, or
 *  -1 for a directory it could not read.
 *
 *  A callback rather than a call, for the reason the map
 *  question above is one: reading a package and running a
 *  script's top level are the scenario library's to do and
 *  the sim is below it. src/server/ names nothing under
 *  src/scenario/, and the link order is what says so — the
 *  scenario library is listed ahead of the server group
 *  because it calls into the group, so a call the other way
 *  would not resolve.
 *
 *  NULL clears it, and with nothing registered the directory
 *  reads empty — which is what a build with no scenario
 *  library offers, exactly as every map reads unscripted
 *  above.
 *
 *  Registered once, where the process decides whether it
 *  runs scripts at all, not where a scenario attaches: the
 *  list is what a server offers instead of the map's own
 *  scenario, so a server with no scenario attached is
 *  precisely the one that needs it answered.
 *
 *  dir is the directory to read, which the sim holds and
 *  hands over per call (serverSimGetScenarioDir), so the
 *  lister keeps no path of its own.
 *********************************************************/
void serverSimSetScenarioLister(ServerSim *sim,
                                int (*list)(void *ctx, const char *dir,
                                            ScnDirEntry *out, int max),
                                void *ctx);

/*********************************************************
 *NAME:          serverSimScenarioListDir
 *PURPOSE:
 *  The scenarios this server offers, read through whatever
 *  was registered above and against the directory the sim
 *  holds. Answers how many entries were written, and 0 for
 *  a server with no lister, no directory, or nothing in it
 *  — all three of which are the ordinary case rather than a
 *  fault, so none of them is told apart here.
 *********************************************************/
int serverSimScenarioListDir(const ServerSim *sim, ScnDirEntry *out, int max);

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
 *
 *  The template's base game type is kept on the game the
 *  sim runs, where the spawn and start paths read it while
 *  the round is gameScripted. Clearing the template clears
 *  that too.
 *********************************************************/
void serverSimSetScenarioLobbyTemplate(ServerSim *sim,
                                       const ScnLobbyTemplate *t);

/*********************************************************
 *NAME:          serverSimSetScenarioIdentity
 *PURPOSE:
 *  Tells the sim what the attached scenario is called and
 *  where it came from, so the lobby can say so without
 *  asking the host anything. The sim copies the strings,
 *  truncating any that are longer than the lobby carries.
 *
 *  Separate from the lobby template: that is seating, and
 *  it is re-applied at points that have nothing to do with
 *  what the scenario is called.
 *
 *  source lobbyScenarioNone clears it, whatever the other
 *  arguments say, and is what a detach passes. NULL for any
 *  string is the empty one.
 *
 *ARGUMENTS:
 *  sim         - The sim being told
 *  source      - Where the scenario came from
 *  name        - The scenario's name
 *  fileName    - The file it came from, a name and not a path
 *  description - What it says about itself
 *  extraTeams  - Whether it lets a host add teams of its own
 *********************************************************/
void serverSimSetScenarioIdentity(ServerSim *sim,
                                  LobbyScenarioSource source,
                                  const char *name,
                                  const char *fileName,
                                  const char *description,
                                  bool extraTeams);

/*********************************************************
 *NAME:          serverSimSetScenarioRules
 *PURPOSE:
 *  Tells the sim which rules the attached scenario's own
 *  manifest sets, and publishes the set, so the lobby can
 *  say what a mod changes without opening the file. The
 *  author's table, not the table the round is running on:
 *  a rule a scenario changes mid-round moves the second
 *  and leaves this alone.
 *
 *  Goes beside the identity, at the same two points: an
 *  attach states its set and a detach states an empty one,
 *  which is what tells a client the scenario has gone. A
 *  map that never had a scenario reaches neither call, so
 *  nothing is published there at all.
 *
 *  A row naming no rule is dropped, and rows past the
 *  event's cap with it — a manifest names each rule at
 *  most once, so a set inside the cap holds every rule
 *  there is.
 *
 *ARGUMENTS:
 *  sim   - The sim being told
 *  rules - The rule/value pairs; NULL for none
 *  count - How many of them; 0 empties the set
 *********************************************************/
void serverSimSetScenarioRules(ServerSim *sim, const ScnOpSetRule *rules,
                               int count);

/*********************************************************
 *NAME:          serverSimAddUnfieldedSeat
 *PURPOSE:
 *  Seat a bot in the lobby without putting it on the
 *  field: the roster gains a connected bot seat with the
 *  name and team given, and nothing else is built — no
 *  brain, no ClientSim, no tank. The seat shows in the
 *  roster, counts as ready, is skipped by the start
 *  sequence, and is fielded later by a spawn naming it.
 *  Returns false for an out-of-range or occupied seat.
 *********************************************************/
bool serverSimAddUnfieldedSeat(ServerSim *sim, BYTE playerNum,
                               const char *name, BYTE teamNumber);

/*********************************************************
 *NAME:          serverSimUnfieldBot
 *PURPOSE:
 *  Take a seat off the field without taking it out of the
 *  game: the bot, the tank, the man and the base timer go;
 *  the roster entry, the identity, the team and the
 *  alliance stay. One CTRL_LOBBY_SLOT says the seat is no
 *  longer fielded — no leave event goes out, nothing the
 *  seat owns changes hands and no client is resynced. A
 *  spawn naming the seat fields it again.
 *  No-op for an empty seat or one already off the field.
 *
 *  Taking the seat itself out is serverSimRemoveBot, which
 *  is what a host's remove, a kick and a disconnect use.
 *********************************************************/
void serverSimUnfieldBot(ServerSim *sim, BYTE playerNum);

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
