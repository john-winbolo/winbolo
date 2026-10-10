/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include "upload_policy.h"  /* ScriptUploadRefusal — the accept callback's answer */

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
 *
 *  Every op through here is a script's, and is counted
 *  against the tick's allowances: the one past
 *  SCN_OPS_PER_TICK, or past SCN_MSGS_PER_TICK for a message
 *  or a sound, is refused SCN_OP_RATE. An op the prelude
 *  refuses is not counted.
 *********************************************************/
ScnOpResult serverSimApplyScenarioOp(ServerSim *sim, const ScenarioOp *op,
                                     ScnOpOut *out);

/*********************************************************
 *NAME:          serverSimApplyScenarioHostOp
 *PURPOSE:
 *  serverSimApplyScenarioOp for an op the host sends on its
 *  own account: the scenario file's rules at the round start,
 *  and the line saying a script was switched off. Everything
 *  is the same — the actor mark, the prelude, the handler —
 *  except that the op is not counted against the tick's
 *  allowances and spends none of them.
 *
 *  Never for anything a script asked for. A script's op
 *  through here would be a way round the allowances.
 *********************************************************/
ScnOpResult serverSimApplyScenarioHostOp(ServerSim *sim, const ScenarioOp *op,
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
 *NAME:          serverSimApplyScenarioRules
 *PURPOSE:
 *  Sets a whole table of rules at once, or none of them.
 *  The set is written into a copy of the sim's table and
 *  checked once, as serverSimCheckScenarioRules checks it,
 *  and only a copy that passes is committed. A pair two of
 *  the values move together is judged with both of them in,
 *  so a table that raises a cap and the rule it caps lands
 *  whichever order the two are listed in.
 *
 *  This is how a round's own rules table is applied at its
 *  boot. game.set_rule during a round is the funnel's
 *  SCN_OP_SET_RULE and sets one rule at a time.
 *
 *  It is refused as the funnel refuses an op: SCN_OP_IN_POLICY
 *  from inside a policy call, and SCN_OP_WRONG_STATE while a
 *  start is in progress with the setup window shut. A set
 *  that fails the check answers as serverSimCheckScenarioRules
 *  does — SCN_OP_RANGE, SCN_OP_PAIR, SCN_OP_NO_SUCH_ITEM, or
 *  SCN_OP_BAD_CALL for a NULL sim or a count with no arrays
 *  behind it — and leaves the sim's table byte for byte as it
 *  was.
 *
 *  A set that passes is committed whole, with one record per
 *  rule, one clamp of the world to the new table, and one
 *  publish when any rule in it is one clients read and the
 *  setup window is shut. A count of 0 answers SCN_OP_OK and
 *  changes nothing. why takes the reason the check gave on a
 *  fault and "" otherwise; it may be NULL only when whyLen
 *  is 0.
 *********************************************************/
ScnOpResult serverSimApplyScenarioRules(ServerSim *sim,
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
 *NAME:          serverSimSetScenarioTickStats
 *PURPOSE:
 *  Records what the scenario's own work cost in the tick
 *  that has just run, for the dedicated server's info: the
 *  instructions the tick's calls were charged, the total
 *  they may spend between them, whether they ran out of it,
 *  and the wall-clock time the whole of the tick callback
 *  took.
 *
 *  Called once per tick by the tick callback registered
 *  above, from inside that tick. The sim keeps the last
 *  time, its average and the round's worst, the last and
 *  worst instruction counts, and how many ticks ran out; all
 *  of them start again at each round start.
 *
 *  budget is passed rather than known because the sim cannot
 *  see the header that sets it.
 *********************************************************/
void serverSimSetScenarioTickStats(ServerSim *sim, uint32_t instr,
                                   uint32_t budget, bool tripped, double ms);

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
 *NAME:          serverSimSetScriptUploadAccept
 *PURPOSE:
 *  Registers what takes a script a player uploaded: a
 *  .scenario or .lua that arrived whole over
 *  PACKET_LOBBY_MAP_UPLOAD_BEGIN with UPLOAD_KIND_SCRIPT.
 *  The callback checks the bytes and writes them under
 *  dir/name, answering true. Otherwise it answers false and
 *  fills why: a SCRIPT_REFUSE_* code and its numbers, which
 *  go back to the sender in the DONE reply for the client to
 *  say in its own language, and one line of text for the
 *  operator's console.
 *
 *  A callback rather than a call, for the reason the lister
 *  above is one: reading a package and running a script are
 *  the scenario library's to do, and src/server/ cannot call
 *  src/scenario/.
 *
 *  NULL clears it. With nothing registered the server
 *  refuses a script upload at BEGIN, before any bytes are
 *  sent, and serverSimScriptUploadAccept answers false with
 *  SCRIPT_REFUSE_SCRIPTS_OFF.
 *
 *  dir is where the upload lands, chosen by the server's
 *  script upload policy; the callback creates it if it is
 *  not there.
 *********************************************************/
typedef bool (*ScriptUploadAcceptFn)(void *ctx, const char *dir,
                                     const char *name,
                                     const uint8_t *bytes, uint32_t len,
                                     ScriptUploadRefusal *why);
void serverSimSetScriptUploadAccept(ServerSim *sim, ScriptUploadAcceptFn fn,
                                    void *ctx);

/*********************************************************
 *NAME:          serverSimHasScriptUploadAccept
 *PURPOSE:
 *  Whether a script upload callback is registered, which is
 *  whether a script upload can be taken at all.
 *********************************************************/
bool serverSimHasScriptUploadAccept(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimScriptUploadAccept
 *PURPOSE:
 *  Hands an uploaded script to the registered callback and
 *  answers what it answers. With nothing registered, answers
 *  false with SCRIPT_REFUSE_SCRIPTS_OFF in why. why is
 *  cleared first either way and may be NULL.
 *********************************************************/
bool serverSimScriptUploadAccept(const ServerSim *sim, const char *dir,
                                 const char *name, const uint8_t *bytes,
                                 uint32_t len, ScriptUploadRefusal *why);

/*********************************************************
 *NAME:          serverSimSetScenarioDetailsReader
 *PURPOSE:
 *  Registers the read of one directory file's details
 *  (scenario_details.h) for serverSimScenarioDetails. A
 *  callback for the reason the lister above is one, and
 *  registered beside it. The reader writes the blob into
 *  out and answers its length, or -1 for a file the
 *  directory does not hold. dir is the sim's, handed over
 *  per call as the lister's is.
 *
 *  NULL clears it, and with nothing registered only the
 *  committed map's own script has details to give.
 *********************************************************/
void serverSimSetScenarioDetailsReader(ServerSim *sim,
                                       int (*read)(void *ctx, const char *dir,
                                                   const char *file,
                                                   uint8_t *out, size_t cap),
                                       void *ctx);

/*********************************************************
 *NAME:          serverSimSetScriptFileReader
 *PURPOSE:
 *  Registers the read of one directory file's raw bytes
 *  for serverSimScriptFileRead, which a player's request
 *  for a copy of a script is answered from. A callback for
 *  the reason the lister above is one, and registered
 *  beside it. The reader finds the file among the names
 *  its listing holds, refuses one over cap before reading
 *  it, and on SERVER_SCRIPT_READ_FOUND hands back a
 *  malloc'd buffer the caller frees. dir is the sim's,
 *  handed over per call as the lister's is.
 *
 *  NULL clears it, and with nothing registered every name
 *  answers SERVER_SCRIPT_READ_NOT_FOUND.
 *********************************************************/
void serverSimSetScriptFileReader(ServerSim *sim,
                                  ServerScriptReadResult (*read)(
                                      void *ctx, const char *dir,
                                      const char *file, uint8_t **outBytes,
                                      uint32_t *outLen, uint32_t cap),
                                  void *ctx);

/*********************************************************
 *NAME:          serverSimSetScenarioSettingsReader
 *PURPOSE:
 *  Registers the read of one directory file's settings
 *  block (scenario_settings.h) for
 *  serverSimScenarioSettingsDecl, on the terms
 *  serverSimSetScenarioDetailsReader registers the details
 *  read. NULL clears it.
 *********************************************************/
void serverSimSetScenarioSettingsReader(ServerSim *sim,
                                        int (*read)(void *ctx,
                                                    const char *dir,
                                                    const char *file,
                                                    uint8_t *out, size_t cap),
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
 *  keepsWinCondition - True when the script declared itself a
 *                mod, so it changes how the game plays and
 *                leaves winning and losing alone. False for a
 *                scenario, which may end the round.
 *  bound       - True when the script is tied to the one map it
 *                was written against. A lobby cannot take a
 *                bound script off on its own: changing it means
 *                changing the map, which is what a chooser reads
 *                this to know.
 *  needsBots   - True when the script, or any script composed
 *                with it, said needs_bots: it fields its own
 *                bots, so the lobby must allow them while it is
 *                attached. False leaves the host's bot setting
 *                alone.
 *  unsafe      - True when this server runs every script with
 *                the full Lua library and no limits
 *                (-allow-unsafe-scripts). The sim cannot ask the
 *                host, so the lobby learns it here.
 *********************************************************/
void serverSimSetScenarioIdentity(ServerSim *sim,
                                  LobbyScenarioSource source,
                                  const char *name,
                                  const char *fileName,
                                  const char *description,
                                  bool extraTeams,
                                  bool keepsWinCondition,
                                  bool bound,
                                  bool needsBots,
                                  bool unsafe);

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

/*********************************************************
 *NAME:          serverSimGetScriptCount / serverSimGetScript
 *PURPOSE:
 *  The list the lobby host wrote, in the order it wrote it,
 *  as CMD_SET_SCRIPT_LIST recorded it. serverSimGetScript
 *  answers NULL for an index outside the count.
 *
 *  This is what whoever owns the scenario reads when it
 *  decides what a round plays.
 *
 *  One row of it may be the committed map's own script,
 *  and that row is the host saying where on the list the
 *  map's script is composed. bound is what marks it: every
 *  other row is a file out of the scenarios directory and
 *  the command bus refuses a bound name that is not the
 *  committed map's. A list that carries no bound row is a
 *  host who never said, and the map's own script composes
 *  at the front of these, which is where it has always
 *  composed.
 *
 *  serverSimSetMapScript keeps that row and the map's own
 *  row in step, so a caller reading this never finds a
 *  bound row naming a script the committed map does not
 *  bring.
 *
 *  The whole ScnDirEntry and not the file name alone,
 *  because the chooser's row carries the manifest's name
 *  and its two flags and re-reading the directory to
 *  answer would open every file in it.
 *********************************************************/
int  serverSimGetScriptCount(const ServerSim *sim);
const ScnDirEntry *serverSimGetScript(const ServerSim *sim, int i);

/*********************************************************
 *NAME:          serverSimSetMapScript
 *PURPOSE:
 *  Records the committed map's own script as a row of the
 *  published list, so a chooser can show it above the
 *  host's picks and say it came with the map.
 *
 *  Called by whoever owns the scenario as it decides what
 *  plays, which is the one caller that knows both that the
 *  map had a script and that it loaded. entry NULL clears
 *  it, and a clear is what a map with no script of its own
 *  passes.
 *
 *  bound on the row means what it means on the attached
 *  scenario: this one came with the map and is not the
 *  host's to remove. A map's own script is bound whether or
 *  not its manifest says so, because a host who wants it
 *  gone changes the map.
 *
 *  The host's list is brought into line at the same time,
 *  because the map's row may also sit on that list and the
 *  two are one row said twice. Where it does, the new
 *  script replaces what was in that place and the place
 *  itself is left alone; a clear takes the row off and
 *  closes the list up behind it. A host who put the map's
 *  script third on the list and then committed another
 *  scripted map therefore still has it third.
 *
 *  Recording only. The publish is the caller's, as it is
 *  for the picks, so a map commit sends one list rather
 *  than one per step.
 *********************************************************/
void serverSimSetMapScript(ServerSim *sim, const ScnDirEntry *entry);

/*********************************************************
 *NAME:          serverSimHoldMapScript
 *PURPOSE:
 *  The committed map still brings its own script, but the
 *  round is not playing it because Mods/Scenario is off.
 *  The row is cleared the way a NULL serverSimSetMapScript
 *  clears it, so the lobby draws no row for a script that
 *  is not playing; but where the host's list held the row,
 *  that place is remembered, and the next
 *  serverSimSetMapScript with a row puts it back there
 *  rather than at the front. A hold with no row on the list
 *  keeps whatever place an earlier hold remembered.
 *********************************************************/
void serverSimHoldMapScript(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimGetMapScriptHeldAt
 *PURPOSE:
 *  The place serverSimHoldMapScript kept for the map's own
 *  row, as an index into the host's list, or -1 when no
 *  place is held. The decision composes the map's own
 *  script there when the box is on again.
 *********************************************************/
int  serverSimGetMapScriptHeldAt(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimScenarioMapIsNewer
 *PURPOSE:
 *  True while a lobby map commit is being decided: the map
 *  is the host's newest choice, so a scenario of its own
 *  that loads plays in place of a scenario picked before
 *  it. False for every other decision, a cancelled preview
 *  and a round with no lobby among them.
 *********************************************************/
bool serverSimScenarioMapIsNewer(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimDropPickedScenarios
 *PURPOSE:
 *  Takes every picked scenario off the host's list and
 *  keeps the rest, the mods, the map's own row and the
 *  operator's rows (serverSimIsOperatorMod), in the host's
 *  order. Answers how many rows went. Called by
 *  whoever owns the scenario once the map's own scenario
 *  has loaded in their place.
 *********************************************************/
int  serverSimDropPickedScenarios(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimGetMapScript
 *PURPOSE:
 *  The row serverSimSetMapScript recorded, or NULL when the
 *  committed map brought no script.
 *********************************************************/
const ScnDirEntry *serverSimGetMapScript(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimSetMapScriptDetails
 *PURPOSE:
 *  The details (scenario_details.h) of the row
 *  serverSimSetMapScript just recorded: the rules the map's
 *  own script sets and what its callbacks do. This is what
 *  serverSimScenarioDetails answers for that script, which
 *  is not in the scenarios directory and so has nowhere
 *  else to be read from.
 *
 *  After serverSimSetMapScript, which forgets the last
 *  row's details. Ignored when no row is recorded, and for
 *  a blob longer than SCN_DETAILS_MAX.
 *********************************************************/
void serverSimSetMapScriptDetails(ServerSim *sim, const uint8_t *details,
                                  size_t len);

/*********************************************************
 *NAME:          serverSimSetMapScriptSettings
 *PURPOSE:
 *  The settings block (scenario_settings.h) of the row
 *  serverSimSetMapScript just recorded, on the terms
 *  serverSimSetMapScriptDetails takes the details. Ignored
 *  when no row is recorded, and for a blob longer than
 *  SCN_SETTINGS_BLOB_MAX.
 *********************************************************/
void serverSimSetMapScriptSettings(ServerSim *sim, const uint8_t *settings,
                                   size_t len);

/*********************************************************
 *NAME:          serverSimGetLobbyScriptCount /
 *               serverSimGetLobbyScript
 *PURPOSE:
 *  The list as the lobby is told it, in load order. This is
 *  what CTRL_LOBBY_SCRIPT_LIST carries and what a chooser
 *  draws.
 *
 *  It is the host's own list where that list carries the
 *  map's own row, because the host has already said where
 *  the map's script goes and prepending a second copy would
 *  draw the same script twice. Where the list does not
 *  carry it, the map's own script comes first and the
 *  host's list follows, which is where the round composes
 *  it for a host who never said otherwise.
 *
 *  Held at LOBBY_SCRIPT_LIST_MAX rows, which is the whole
 *  list a client can take in: a client that is sent more
 *  keeps the list it already had and shows nothing new.
 *  The map's own row is the one that cannot be dropped, so
 *  a host with a full list of picks loses the last of them
 *  on a map that brings a script of its own.
 *
 *  serverSimGetScriptCount above is the other question —
 *  what the host wrote — and the two answers are the same
 *  list except where the map's script is playing and the
 *  host's list does not name it. A caller that wants the
 *  rows the host may reorder asks that one; a caller
 *  drawing the lobby asks this one.
 *********************************************************/
int  serverSimGetLobbyScriptCount(const ServerSim *sim);
const ScnDirEntry *serverSimGetLobbyScript(const ServerSim *sim, int i);

#endif /* SERVER_SIM_SCENARIO_H */
