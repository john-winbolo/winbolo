/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Host
 *Filename:      scenario_host.h
 *Author:        John Morrison
 *Purpose:
 *  What a frontend sees of a scenario: attach one to a sim
 *  from a map path, detach it, ask whether one is active
 *  and what it calls itself.
 *
 *  This header is included by binaries that compile under
 *  gui and runtime_only, which see src/bolo/public/ and
 *  nothing else, so it includes only public headers and
 *  names no type from src/bolo/scenario_api/. The write
 *  funnel, the op types and the manifest the host parses
 *  are the library's own business and stay inside it.
 *********************************************************/

#ifndef SCENARIO_HOST_H
#define SCENARIO_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "server_sim.h" /* ServerSim, and MAX_TANKS / MAX_PILLS / MAX_BASES /
                         * MAX_STARTS through global.h and types.h */

/* The API version this server implements. A script states the version it
 * was written against as scenario.api; one written against a newer server
 * than this is refused rather than half-understood. */
#define SCENARIO_API_VERSION 1

/* How many scripts one host composes into its round: one scenario deciding
 * the round and up to nine mods changing how it plays.
 *
 * Everything below it — the per-script environments, the load cycle, the
 * hook chain and the per-script error counts — is built against this number
 * rather than against a literal, so the array walks and the loop bounds
 * follow it without having to be found again.
 *
 * Ten rather than the four an earlier measurement suggested. That four was
 * the widest list that fits inside the CTRL_LOBBY_SETTINGS tail in one
 * control segment, and the list does not ride that tail: it has a chunked
 * control event of its own (CTRL_LOBBY_SCRIPT_LIST), bounded per chunk
 * rather than per list, and a command of its own measured against
 * COMMAND_MAX_WIRE_BYTES. The wire stopped being what binds the number.
 *
 * Held against the public LOBBY_SCRIPT_LIST_MAX in scenario_host.c, which
 * is the translation unit that sees both: the wire has to be able to name
 * every script a host can compose, and a public header cannot reach in here
 * to read this one. */
#define SCN_SCRIPTS_MAX 10

/* ── What two scripts of one list disagreed about ──────────────────
 *
 * Composing a list no longer refuses a rule two scripts both set or a region
 * two scripts both name. The earlier script wins the rule, both regions are
 * kept and told apart by which script named them, and neither is an error.
 * A host who ordered the list meant the order to decide, which is the whole
 * reason the list is ordered.
 *
 * Something decided quietly is still something a host wants to see, so each
 * one is written down here as it is resolved and a frontend can say so. The
 * record belongs to the composite: composing the list again fills it from
 * nothing.
 *
 * Ten rows, and a marker saying there were more. Ten is not a guess. The
 * whole list read out as text — one byte of kind, three strings and their
 * terminators — comes to at most 10 x (1 + 32 + 32 + 32 + 3) = 970 bytes,
 * which fits inside one control body with room over, so the list a host is
 * shown is the list the host holds rather than the first instalment of one.
 * A list that runs past ten says so and stops: a round where ten pairs of
 * scripts disagree has a larger problem than its eleventh pair.
 *
 * The lengths: 32 covers every region name (SCN_REGION_NAME_LEN in
 * scenario_manifest.h, which this header does not include) and every rule
 * name the sim has, the longest of which is 27 bytes. The file is the
 * script's own file name and not the path it was found at, for the reason
 * the lobby is told a file name — where a server keeps its scenarios is the
 * server's own business. A longer name is cut rather than dropped. */
#define SCN_CONFLICTS_MAX      10
#define SCN_CONFLICT_NAME_LEN  32
#define SCN_CONFLICT_FILE_LEN  32

typedef enum {
    scnConflictRule = 0,  /* two scripts set one rule in their tables */
    scnConflictRegion     /* two scripts named one region */
} ScnConflictKind;

typedef struct {
    ScnConflictKind kind;
    char            name[SCN_CONFLICT_NAME_LEN];    /* the rule or region */
    char            winner[SCN_CONFLICT_FILE_LEN];  /* the file that won */
    char            loser[SCN_CONFLICT_FILE_LEN];   /* the file that lost */
} ScnComposeConflict;

/* How many timers a round may have waiting at once. A timer holds a Lua
 * function across ticks, so the count is what bounds both the table the host
 * walks each tick and the functions a round can keep from collection. The
 * one past the last is refused rather than displacing one already set: a
 * script handed an id has been promised that call.
 *
 * Declared and defined regions share SCN_REGIONS_MAX (scenario_manifest.h)
 * — the sixty-four are the round's, however many of them the file wrote
 * down. */
#define SCN_TIMERS_MAX       64

/* A map path plus the script suffix. Map paths reach the server from a
 * command line, so this matches what those buffers hold. */
#define SCN_SCRIPT_PATH_MAX 2304

/* The script a map is looked for beside: X.map is accompanied by
 * X.scenario.lua. */
#define SCN_SCRIPT_SUFFIX ".scenario.lua"

/* The most a script may hold. It is hand-written Lua, so a megabyte is far
 * more than one ever needs and still well inside the four a whole packaged
 * scenario is allowed on the wire. A file above it is refused rather than
 * read, because at that size it is not a script. */
#define SCN_SCRIPT_MAX_BYTES (1024 * 1024)

/* How long, in milliseconds, one walk of a mods directory's file stamps
 * stands for the next read of it. A read inside this window, with the
 * directory's own time and this process's change count unmoved, is answered
 * from the kept listing without stamping each file again, so a file edited
 * in place is seen up to this much later. Here rather than in scenario_host.c
 * so a test can wait it out. */
#define SCN_DIR_STAMPS_REUSE_MS 250

/* How many hook or policy calls may raise in a row before the scenario is
 * switched off for the rest of the round. Any call that returns normally
 * puts the count back to zero, so this counts a script that is failing
 * every time rather than one that fails now and then.
 *
 * A round the scenario is off for runs no more of its Lua: hooks are
 * skipped, policies answer the classic rule, and the players are told once.
 * It is off for the round and not for the attachment — the next round start
 * boots a fresh VM and begins again at zero. */
#define SCN_ERROR_LIMIT 20

/* The most memory one scenario state may hold. Every allocation a state
 * makes is counted against this, and the one that would take it past is
 * refused — which Lua raises as an out-of-memory error rather than dying on,
 * so the call that asked for it lands in the host's own lua_pcall, counts one
 * toward SCN_ERROR_LIMIT and leaves the state to carry on: what the script
 * had abandoned by then is collected and the round keeps playing.
 *
 * 32 MB is far more than a scenario has any use for — the reference script
 * holds a few tables of numbers — and far less than a server can afford to
 * lose to one map's script. A build whose Lua will not take an allocator at
 * all counts nothing; scnSandboxMemoryCapped says which kind of build this
 * is. */
#define SCN_VM_MEMORY_MAX (32u * 1024u * 1024u)

/* The most VM instructions one call into a script may run for. A hook, a
 * timer or a policy answer that passes this is stopped where it stands with
 * a Lua error, so the call lands in the host's own lua_pcall, counts one
 * toward SCN_ERROR_LIMIT and leaves the round ticking: a script that loops
 * without end costs the tick it was called on rather than the server.
 *
 * It bounds one call and not a round or a tick. Each call starts again at
 * zero, so a script with real work to do spreads it across its on_tick calls
 * rather than looping inside one of them.
 *
 * A million is far more than a hook answering a question needs and small
 * enough that a runaway is stopped inside the tick that started it. What the
 * boundary test shows is a bounded loop well past it cut off, the line
 * naming the hook it was in, and the sim ticking afterwards. */
#define SCN_BUDGET_CALL_INSTR 1000000u

/* How often that count is taken, which is the count hook's own parameter.
 * Small enough that a runaway is stopped within a thousandth of the budget,
 * large enough that a script is not spending its time in the hook. */
#define SCN_BUDGET_STEP_INSTR 1000u

/* What a call that has already been stopped may still spend. The error raised
 * at the budget unwinds through whatever the script had standing, and a
 * metamethod running on the way out is Lua code like any other: cutting that
 * off where it stands is not what the budget is for, so the count is put back
 * to this far short of the budget rather than to zero and the unwind has room
 * to finish.
 *
 * Twenty hook steps is room enough for an unwind and far too little to be
 * worth catching the error for: a script that caught it and carried on
 * anyway is stopped again within that, and again after that, rather than
 * running on uncounted. */
#define SCN_BUDGET_GRACE_INSTR 20000u

/* The most VM instructions all of one tick's calls into scripts may run for
 * between them: every hook and timer the tick runs, and any policy asked from
 * inside one of those calls. A policy the engine asks while it steps the world
 * (choose_start, spawn_loadout, can_respawn, damage_scale) runs before the
 * tick's window opens, so it has only its own SCN_BUDGET_CALL_INSTR and is not
 * counted here.
 *
 * The per-call budget alone does not bound a tick. A script may hold sixty-four
 * timers and each of them gets a whole SCN_BUDGET_CALL_INSTR, so a tick of
 * legal calls could run for tens of millions of instructions without any one
 * of them being stopped.
 *
 * The call that passes this is stopped where it stands, as one past its own
 * budget is, and the rest of that tick's calls are skipped: the tick costs one
 * error however many calls were left. A timer skipped this way runs next tick;
 * an event or region change is not delivered again. on_end is not counted
 * against it and always runs.
 *
 * Twice the per-call budget, so one long call never trips it alone. A starting
 * value; a measurement may want it somewhere else. */
#define SCN_BUDGET_TICK_INSTR 2000000u

/* The console lines one call into a script may print.
 *
 * print goes to the server console rather than to the host's stdout, which is
 * what makes it worth bounding: a console line reaches the operator's message
 * log where one is configured, and that file is opened, written and closed for
 * every line. Two hundred thousand prints fit inside one call's instruction
 * budget, so a script that prints in a loop costs the tick thread that many
 * open and close cycles and grows the log without end.
 *
 * A line past this is dropped and the first drop says so, once, so an operator
 * missing output knows why rather than wondering. Sixty-four is far more than
 * a script telling an operator something needs and far too few to flood with.
 * A starting value; a measurement may want it somewhere else. */
#define SCN_PRINT_PER_CALL 64

/* And the console lines all of one tick's calls may print between them. A
 * script has an on_tick, the timers it set and whatever policies it answers
 * within the same tick, so the per-call bound on its own would multiply by
 * however many calls a script arranges to be made. Against the server's fifty
 * ticks a second this works out to a ceiling of 3,200 lines a second.
 *
 * Counted against the sim's own tick rather than against a wall clock: the
 * unit tests drive the sim as fast as the CPU allows, so a window measured in
 * seconds would cut a fixture that prints once a tick and fail it for a reason
 * that is not the one it is about. A tick is the same window on a live server
 * as it is in a test.
 *
 * A starting value, as the per-call one is. */
#define SCN_PRINT_PER_TICK 64

/* The longest string a script may have the library's C functions build or
 * search: string.rep, string.format and table.concat refuse a result longer
 * than this, and string.find, match, gmatch and gsub a subject longer than it.
 *
 * The count hook cannot see inside a C function, so one string.rep is one
 * instruction to it however much it builds, and a pattern run over a long
 * subject is the same. Without this, SCN_VM_MEMORY_MAX is all that stands
 * between a script and a string or pattern that holds the tick.
 *
 * 64 KiB is far longer than anything a scenario prints or matches. A starting
 * value; a measurement may want it somewhere else. */
#define SCN_STRING_MAX 65536u

/* How many steps the pattern matcher behind string.find, match, gmatch and
 * gsub takes between charges to the running call. One matcher step is charged
 * as one instruction, so a pattern that backtracks without end spends the
 * call's SCN_BUDGET_CALL_INSTR and the tick's SCN_BUDGET_TICK_INSTR as a loop
 * in Lua would, and is stopped the same way.
 *
 * A step is one entry into the matcher's recursive match, one turn of any of
 * its loops over the subject or a set, or one byte a back-reference or a plain
 * find compares. The count hook cannot see any of that, so the matcher keeps
 * its own count and hands it over in lots of this size: the same lot the hook
 * counts in, so a pattern is stopped within a thousandth of the budget as a
 * loop is. What is left over when a call ends is charged then. */
#define SCN_PATTERN_STEP_CHARGE 1000u

/* How many events the host holds between one tick and the next, across
 * both of the server's channels. Each of the two subscriber callbacks
 * copies an event in and returns; the one drain at the end of the tick
 * empties exactly what was waiting when it started.
 *
 * An event that arrives with the queue full is dropped rather than
 * overwriting one, and the tick that lost them says how many and counts one
 * error: a round that produces more events in one tick than this is
 * producing them faster than a script can answer them, and the numbers say
 * so rather than the oldest of them going quietly. A tick counts once
 * however many it lost, so a round that keeps overflowing reaches
 * SCN_ERROR_LIMIT over SCN_ERROR_LIMIT ticks rather than in one. */
#define SCN_EVENT_QUEUE_MAX 256

typedef struct ScenarioHost ScenarioHost;

/*********************************************************
 *NAME:          scenarioHostSetEnabled
 *PURPOSE:
 *  Whether scenarioHostAttach may load a script at all.
 *  Enabled by default, so a server that says nothing runs
 *  the scripts it always ran.
 *
 *  Off, every attach answers NULL before it reads anything
 *  and every host in the process plays plain maps — the
 *  dedicated server, the headless runner and the desktop
 *  client's single-player host, none of which tests this
 *  for itself.
 *
 *  A map that does have a script beside it is named through
 *  the attach's err buffer, so a caller that already prints
 *  a failed attach prints the refusal too. A map with no
 *  script beside it says nothing.
 *
 *  Set it before the first attach: it is one answer for the
 *  process, and a map committed later reads whatever it
 *  last said.
 *********************************************************/
void scenarioHostSetEnabled(bool enabled);

/*********************************************************
 *NAME:          scenarioHostSetUploadScriptsEnabled
 *PURPOSE:
 *  Whether a map a client uploaded to this server may bring
 *  a script with it. Enabled by default, so a server that
 *  says nothing behaves as it always did.
 *
 *  A .map file can carry a scenario container appended to
 *  it, and the upload path writes the bytes it was sent
 *  whole, container and all. Off, a map whose file sits in
 *  the server's uploads directory attaches neither that
 *  container nor a loose script beside it, and one console
 *  line names the file so the operator can see which upload
 *  was turned down. Every other map is unaffected: the
 *  operator's own map directory is the operator's own.
 *
 *  Narrower than scenarioHostSetEnabled, which turns every
 *  script off wherever the map came from. Both apply: with
 *  scripts off altogether this one is never reached.
 *
 *  Set it before the first attach, beside the switch above:
 *  it is one answer for the process, and a map committed
 *  later reads whatever it last said.
 *********************************************************/
void scenarioHostSetUploadScriptsEnabled(bool enabled);

/*********************************************************
 *NAME:          scenarioHostSetUnsafeScripts
 *PURPOSE:
 *  Whether scenario scripts run with nothing held back: the
 *  whole Lua standard library, no memory cap, no instruction
 *  budgets and precompiled chunks accepted. Off by default.
 *  The dedicated server's -allow-unsafe-scripts and the
 *  other hosts' --allow-unsafe-scripts set it.
 *
 *  It reaches every script the process runs: one beside a
 *  map on disk, one packed into an uploaded map, a mod, and
 *  the check -validate makes. An operator who turns it on
 *  has chosen to trust all of them.
 *
 *  Separate from scenarioHostSetUploadScriptsEnabled, which
 *  refuses an uploaded map's script outright and still does
 *  with this on.
 *
 *  Set it before the first attach: it is one answer for the
 *  process, and a state booted later reads whatever it last
 *  said.
 *********************************************************/
void scenarioHostSetUnsafeScripts(bool unsafe);

/*********************************************************
 *NAME:          scenarioHostUnsafeScripts
 *PURPOSE:
 *  What scenarioHostSetUnsafeScripts last said. For the
 *  lobby settings event, so a joiner can see the mode.
 *********************************************************/
bool scenarioHostUnsafeScripts(void);

/*********************************************************
 *NAME:          scenarioHostMapHasScript
 *PURPOSE:
 *  Whether picking the map at mapPath here would run a
 *  script: one is beside it on disk AND this process runs
 *  scripts. Asked without reading, parsing or running a byte
 *  of the file. This is the one question a lister asks to tag
 *  a map before anyone picks it, and the one place that knows
 *  how a script is found, so a later way of carrying one
 *  changes here and every caller follows.
 *
 *  False for every map while scripts are switched off. That
 *  is what the tag has to say then: the attach would refuse
 *  the file and the map would play plain, so tagging it
 *  scripted would promise a round nobody gets.
 *
 *ARGUMENTS:
 *  mapPath - Full path to the .map file
 *********************************************************/
bool scenarioHostMapHasScript(const char *mapPath);

/*********************************************************
 *NAME:          scenarioHostMapScriptOpens
 *PURPOSE:
 *  How many map files the question above has opened since
 *  the process started. The answer is kept per path, keyed
 *  on the file's size and modify time, so a second listing
 *  of an unchanged directory opens nothing; this is how a
 *  test says so, and it is of no use to a frontend.
 *
 *  Only ever rises.
 *********************************************************/
unsigned long scenarioHostMapScriptOpens(void);

/*********************************************************
 *NAME:          scenarioHostRegisterMapScripted
 *PURPOSE:
 *  Hands the sim's map lister the question above, so every
 *  entry it returns says whether that map is scripted.
 *
 *  Call it once where the process decides whether it runs
 *  scripts at all, beside scenarioHostSetEnabled — not at an
 *  attach. An attach answers NULL for a map with no script,
 *  so registering there would leave a server whose own map is
 *  plain reporting every scripted map in its directory as
 *  plain, and nothing would say so.
 *
 *  Registering is independent of the scripts switch, but what
 *  the question answers is not: with scripts off every entry
 *  reads plain, which is what those maps will play as here.
 *
 *ARGUMENTS:
 *  sim - The sim whose lister is being told
 *********************************************************/
void scenarioHostRegisterMapScripted(ServerSim *sim);

/*********************************************************
 *NAME:          scenarioHostRegisterScenarioLister
 *PURPOSE:
 *  Hands the sim the read of its scenarios directory, so a
 *  client asking what this server offers is answered.
 *
 *  Call it once, in the same place and for the same reason
 *  as the registration above: the list is what a server
 *  offers instead of a map's own scenario, so the server
 *  that needs it answered is exactly the one with nothing
 *  attached.
 *
 *  What is read is scnDirList; where it is read is the
 *  directory the sim holds, which an operator sets with
 *  -scenariodir or the "Scenario Dir" preference. A server
 *  that registers nothing offers an empty list.
 *
 *ARGUMENTS:
 *  sim - The sim being told where to send the question
 *********************************************************/
void scenarioHostRegisterScenarioLister(ServerSim *sim);

/*********************************************************
 *NAME:          scenarioHostListLocalScripts
 *PURPOSE:
 *  The scripts on this computer: the player's own Mods
 *  directory under SDL_GetPrefPath (WB_MOD_DIR_USER in the
 *  tests), then the Workshop directory beside it
 *  (WB_MOD_DIR_WORKSHOP), where a name Mods holds is Mods'
 *  file. What the lobby's Mods chooser offers to send to a
 *  server that does not have them.
 *
 *  Needs no sim, because a remote client has none, and reads
 *  through the same modify-time cache a server's listing
 *  does, so a loose script's VM boots once and not on every
 *  call. Still a directory read: call it when the chooser
 *  opens or a transfer ends, never per frame.
 *
 *  A row read from the Workshop directory says
 *  SERVER_SCENARIO_SOURCE_WORKSHOP and every other row
 *  SERVER_SCENARIO_SOURCE_SERVER. Its workshopId is the
 *  Workshop item the file's manifest names, 0 for none, and
 *  workshopAuthor the account the manifest says published
 *  it, 0 for none.
 *  File-name order, case-insensitive.
 *
 *ARGUMENTS:
 *  out - Rows written here
 *  max - How many out holds
 *
 *RETURNS:
 *  How many rows were written; 0 for a directory that is
 *  missing or empty.
 *********************************************************/
int scenarioHostListLocalScripts(ServerScenarioEntry *out, int max);

/*********************************************************
 *NAME:          scenarioHostLocalScriptPath
 *PURPOSE:
 *  The full path of a file scenarioHostListLocalScripts
 *  listed, so the chooser can hand it to the upload.
 *
 *ARGUMENTS:
 *  file   - A file name as a row gave it; a name with a
 *           directory separator in it is refused
 *  out    - The path is written here
 *  outLen - The size of out
 *
 *RETURNS:
 *  True when one of those directories holds the file and
 *  its path fits out. False otherwise, with out "".
 *********************************************************/
bool scenarioHostLocalScriptPath(const char *file, char *out, size_t outLen);

/*********************************************************
 *NAME:          scenarioHostWorkshopDir
 *PURPOSE:
 *  <prefpath>Workshop, where subscribed Workshop items are
 *  copied to, or what WB_MOD_DIR_WORKSHOP names. The same
 *  directory the mod listing and the local listing read,
 *  so the one that writes it and the ones that read it
 *  agree on the path and its override.
 *
 *ARGUMENTS:
 *  out    - The path is written here
 *  outLen - The size of out
 *
 *RETURNS:
 *  True with the path in out. False when SDL cannot name
 *  it or it does not fit, with out "".
 *********************************************************/
bool scenarioHostWorkshopDir(char *out, size_t outLen);

/*********************************************************
 *NAME:          scenarioHostMapPackageInfo
 *PURPOSE:
 *  The scenario packed into a map file: fills out's name,
 *  description, keepsWinCondition, bound, workshopId and
 *  workshopAuthor from the chunk's manifest, and file with
 *  the map's file name. Only the manifest is read; no
 *  script runs. The file is read under the cap the attach
 *  reads it under. A directory read per call: build a list
 *  with it when the list is asked for, never per frame.
 *
 *ARGUMENTS:
 *  mapPath - The map file
 *  out     - Filled on true, cleared otherwise
 *
 *RETURNS:
 *  False for a map with no chunk or one that does not read.
 *********************************************************/
bool scenarioHostMapPackageInfo(const char *mapPath, ServerScenarioEntry *out);

/*********************************************************
 *NAME:          scenarioHostPackLooseScript
 *PURPOSE:
 *  Pack Mods/<stem>.lua into Mods/<stem>.scenario for
 *  publishing, then move the .lua into Mods/Sources/ so the
 *  mod list shows one row. A <stem>.scenario already there
 *  that carries a Workshop id keeps it: the id and author
 *  are read before the pack and written back after it
 *  (scnIoSetWorkshopId). A <stem>.lua already in Sources is
 *  replaced. The listings are told after the pack and after
 *  the move.
 *
 *  A move that fails is logged and still answers true: the
 *  package is in place, and the script is left listed
 *  beside it.
 *
 *ARGUMENTS:
 *  luaPath         - The loose script
 *  outScenarioPath - The package's path, on true
 *  outLen          - The size of outScenarioPath
 *  err, errLen     - The reason, on false
 *
 *RETURNS:
 *  False with err set, and the .lua left where it was, when
 *  the pack fails or the kept id cannot be written back.
 *********************************************************/
bool scenarioHostPackLooseScript(const char *luaPath, char *outScenarioPath,
                                 size_t outLen, char *err, size_t errLen);

/* What scenarioHostSaveLocalScript made of one file. */
typedef enum {
    SCENARIO_LOCAL_SAVE_OK = 0,
    SCENARIO_LOCAL_SAVE_BAD_NAME,   /* not a bare .lua / .scenario file name */
    SCENARIO_LOCAL_SAVE_EXISTS,     /* this computer already has that name  */
    SCENARIO_LOCAL_SAVE_WRITE       /* the directory or the file could not be written */
} ScenarioLocalSaveResult;

/*********************************************************
 *NAME:          scenarioHostSaveLocalScript
 *PURPOSE:
 *  Puts a copy of a server's script in the player's own
 *  Mods directory, the first of the directories
 *  scenarioHostListLocalScripts reads, making it if it is
 *  not there yet.
 *
 *  The name came from a server, so it is held to a bare
 *  file name ending in .lua or .scenario before anything is
 *  written. A name any of those directories already holds,
 *  in whatever case, is refused: a copy never replaces a
 *  file of the player's own.
 *
 *  The bytes go to a dot file in the directory and are
 *  renamed onto the name, so a failed write leaves nothing
 *  behind. A saved file is listed on the next call to
 *  scenarioHostListLocalScripts.
 *
 *ARGUMENTS:
 *  file  - The bare file name, as the server gave it
 *  bytes - The file's contents; may be NULL only when len
 *          is 0
 *  len   - How many bytes; 0 writes an empty file
 *
 *RETURNS:
 *  SCENARIO_LOCAL_SAVE_OK once the file is in place, or
 *  the reason it is not.
 *********************************************************/
ScenarioLocalSaveResult scenarioHostSaveLocalScript(const char *file,
                                                    const uint8_t *bytes,
                                                    size_t len);

/*********************************************************
 *NAME:          scenarioHostAttach
 *PURPOSE:
 *  Finds the map's script, reads it, boots a VM, runs its
 *  chunk and reads its scenario table. Registers itself on the
 *  sim, so the round start that follows applies the scenario's
 *  rules.
 *
 *  Two places carry a script. A loose X.scenario.lua beside
 *  the map wins, and a map that carries a container as well
 *  says so on the console. Otherwise the container appended to
 *  the map file itself is read: its manifest fills the
 *  scenario global before the chunk runs, and the table the
 *  chunk leaves behind is held against that manifest — so a
 *  packaged script may omit the table or restate it, but one
 *  that restates it and disagrees is refused by key.
 *
 *  The file is read once, here. The host keeps the bytes and
 *  every later round runs those, so editing the file while a
 *  server is up changes nothing until something asks the host
 *  to read it again.
 *
 *  Returns NULL when there is no script, which is the
 *  ordinary case and not an error: err is left empty and the
 *  server runs a plain map. Returns NULL on a script that
 *  cannot be used — one too large to be a script, a syntax
 *  error, an error raised by the chunk, no scenario table, or
 *  an api newer than this server — and writes one operator
 *  line to err saying which, with the file and the line where
 *  Lua has one.
 *
 *  Returns NULL without reading the file when scripts are
 *  off, writing a line to err naming the script the map has
 *  and nothing at all for a map that has none.
 *
 *  err may be NULL only when errLen is 0.
 *********************************************************/
ScenarioHost *scenarioHostAttach(ServerSim *sim, const char *mapPath,
                                 char *err, size_t errLen);

/*********************************************************
 *NAME:          scenarioHostAttachMod
 *PURPOSE:
 *  The same attach, for a scenario the server offers on its
 *  own rather than one a map carries: a .scenario package or
 *  a loose .lua in the scenarios directory, named by the file
 *  the host picked. Everything past where the script came
 *  from is what scenarioHostAttach does — the same VM, the
 *  same manifest read, the same check of a package's table
 *  against its manifest, the same registrations.
 *
 *  It has no map. A mod plays on whichever map is committed,
 *  so nothing here reads one and the map may even be one that
 *  came from bytes rather than a file. The lobby is told
 *  lobbyScenarioMod, and the file name it carries is the
 *  mod's own.
 *
 *  Returns NULL with the reason in err for a file that is not
 *  there, is neither a package nor a script by its name, or
 *  cannot be used — and, unlike a map with no script, a
 *  missing file is a fault here: the host asked for this one
 *  by name.
 *
 *ARGUMENTS:
 *  sim    - The sim to attach to
 *  dir    - The scenarios directory
 *  file   - The file name in it, not a path
 *  err    - Where the reason goes
 *  errLen - Its size; err may be NULL only when this is 0
 *********************************************************/
ScenarioHost *scenarioHostAttachMod(ServerSim *sim, const char *dir,
                                    const char *file,
                                    char *err, size_t errLen);

/*********************************************************
 *NAME:          scenarioHostReload
 *PURPOSE:
 *  Reads the script from disk again and, if the new bytes
 *  are usable, keeps them in place of the ones the host was
 *  holding. Usable means: inside SCN_SCRIPT_MAX_BYTES, the
 *  chunk loads and runs, a scenario table comes out of it,
 *  its api is not above this server's, and where the script
 *  came out of a package, the table agrees with the manifest.
 *
 *  Where the script comes from is decided again rather than
 *  kept: a loose script dropped beside a packed map takes over
 *  at the reload, which is what makes editing one a loop
 *  rather than a re-pack.
 *
 *  Checked in a Lua state of its own before anything is
 *  swapped, so a bad edit changes nothing: on any failure
 *  this returns false, writes one operator line to err, and
 *  leaves the running scenario exactly as it was.
 *
 *  On success the new bytes take effect at the next round
 *  start. The round in progress keeps the table it began
 *  with, so a caller telling an operator what happened
 *  should say so.
 *********************************************************/
bool scenarioHostReload(ScenarioHost *h, char *err, size_t errLen);

/*********************************************************
 *NAME:          scenarioHostDetach
 *PURPOSE:
 *  Takes the host off the sim, closes its VM, drops the
 *  script's bytes it was holding and frees it.
 *  NULL is a no-op, so a caller that attached nothing can
 *  detach unconditionally on the way out.
 *********************************************************/
void scenarioHostDetach(ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostFollowMap
 *PURPOSE:
 *  Keeps *slot pointing at whichever scenario the sim's
 *  committed map has. Each time a map is committed the sim
 *  calls in here: the scenario the previous map had is
 *  detached, a script beside the new file is looked for,
 *  and *slot is set to the result or to NULL when the map
 *  has none. The lobby the new scenario asks for is handed
 *  to the sim as part of that, and the sim seats it.
 *
 *  slot is the caller's own pointer and must outlive the
 *  sim — it is read and written from inside the map change,
 *  which is why it cannot be the scenario itself. Call it
 *  once, after the first attach; pass a NULL slot to stop.
 *
 *  Each commit says what it did: the scenario it attached
 *  and the file it came from, the script it refused because
 *  scripts are off, or the one it could not use. All three
 *  are logged rather than returned, because there is nobody
 *  to answer at the point a map is committed and a bad
 *  script still leaves a playable map. A map with no script
 *  beside it says nothing, so a rotation over plain maps is
 *  as quiet as it was.
 *********************************************************/
void scenarioHostFollowMap(ServerSim *sim, ScenarioHost **slot);

/*********************************************************
 *NAME:          scenarioHostIsActive
 *PURPOSE:
 *  Whether a scenario is attached and running. False for a
 *  NULL host, so a caller need not test both.
 *********************************************************/
bool scenarioHostIsActive(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostName
 *PURPOSE:
 *  The scenario's name, as the lobby shows it. "" when no
 *  scenario is attached; never NULL.
 *********************************************************/
const char *scenarioHostName(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostDescription
 *PURPOSE:
 *  The scenario's description line. "" when no scenario is
 *  attached; never NULL.
 *********************************************************/
const char *scenarioHostDescription(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostScriptPath
 *PURPOSE:
 *  The script file the host read, for an operator asking
 *  which file is in play. "" when no scenario is attached;
 *  never NULL.
 *********************************************************/
const char *scenarioHostScriptPath(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostLastError
 *PURPOSE:
 *  The most recent operator line the host produced after
 *  attach — a rule the funnel refused, a key that names no
 *  rule, a round start the cached chunk failed on.
 *  "" when the scenario has had nothing to say; never NULL.
 *
 *  Attach reports through its own err buffer rather than
 *  here, because a failed attach returns no host to ask.
 *********************************************************/
const char *scenarioHostLastError(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostConflictCount
 *PURPOSE:
 *  How many things two scripts of the round's list
 *  disagreed about and the compose settled: a rule they
 *  both set, or a region they both named. Zero for a NULL
 *  host, for a round with no script and for the ordinary
 *  round where the scripts do not overlap.
 *
 *  Never more than SCN_CONFLICTS_MAX. A list that had more
 *  than that says so through scenarioHostConflictsOverflowed
 *  below, rather than by counting past the array.
 *********************************************************/
int scenarioHostConflictCount(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostConflict
 *PURPOSE:
 *  One of them, in the order the compose settled them:
 *  what kind of thing it was, what it was called, the file
 *  whose value the round runs, and the file whose value it
 *  does not. NULL for an index outside the count.
 *
 *  The row is the host's own and lives until the list is
 *  composed again, which is at every round start.
 *********************************************************/
const ScnComposeConflict *scenarioHostConflict(const ScenarioHost *h, int i);

/*********************************************************
 *NAME:          scenarioHostConflictsOverflowed
 *PURPOSE:
 *  Whether there were more of them than the host kept. The
 *  ones it kept are the first SCN_CONFLICTS_MAX in the
 *  order they were settled; anything past that was resolved
 *  the same way and simply not written down.
 *********************************************************/
bool scenarioHostConflictsOverflowed(const ScenarioHost *h);

#endif /* SCENARIO_HOST_H */
