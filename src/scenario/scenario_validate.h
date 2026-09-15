/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Validate
 *Filename:      scenario_validate.h
 *Author:        John Morrison
 *Purpose:
 *  Reads the script beside a map and says what is wrong
 *  with it: the api it asks for, the lobby it seats, the
 *  rules it sets, the entities it tags and the rectangles it
 *  names, each problem as a key, a line and a message.
 *
 *  Nothing here runs a round. The chunk is loaded and run
 *  once, so the scenario table it declares exists, against a
 *  game table whose calls do nothing and answer nothing; no
 *  hook is called and no sim is ever ticked.
 *
 *  The manifest the checks ran against comes back beside
 *  them. It is the same struct the host reads at attach and
 *  the same one a package is written from, so a script is
 *  read one way whether a round is being started on it or an
 *  author is being told about a typo in it.
 *
 *  That parse lives in scenario_host.c and is declared at
 *  the foot of this file, which is where the host and the
 *  validator meet.
 *********************************************************/

#ifndef SCENARIO_VALIDATE_H
#define SCENARIO_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "server_sim.h"        /* ServerSim, and the entity counts */
#include "scenario_manifest.h" /* ScenarioManifest */

#define SCN_VALIDATE_ISSUES_MAX 64
#define SCN_VALIDATE_KEY_LEN    64
#define SCN_VALIDATE_MSG_LEN    192

/* One problem the validator found. key is the dotted path of the thing at
 * fault ("api", "lobby.teams[2].bots", "rules.tank_reload_ticks",
 * "regions.keep"), "" where no key applies. line is 1-based and 0 when the
 * key could not be found in the source. */
typedef struct {
    char key[SCN_VALIDATE_KEY_LEN];
    int  line;
    char message[SCN_VALIDATE_MSG_LEN];
} ScnValidateIssue;

typedef struct {
    bool             haveManifest;   /* false when no script, or it failed to run */
    uint16_t         count;
    uint16_t         dropped;        /* problems past SCN_VALIDATE_ISSUES_MAX */
    ScnValidateIssue issues[SCN_VALIDATE_ISSUES_MAX];
    ScenarioManifest manifest;       /* what the table parsed to; also what
                                      * a package is written from */
} ScnValidateResult;

/*********************************************************
 *NAME:          scenarioValidateMap
 *PURPOSE:
 *  Looks for a script beside mapPath, runs its top-level
 *  chunk in a VM with a stub game table, reads the scenario
 *  table it declares into out->manifest, and checks that
 *  table against the map sim holds, the lobby it asks for
 *  and the rule catalogue.
 *
 *  Returns true for a map that is acceptable: one with no
 *  script beside it at all — haveManifest false, no issues,
 *  which is the ordinary case rather than a fault — or one
 *  whose script produced no issue. False for a file that
 *  could not be read, a chunk that did not run, and any
 *  script with an issue against it. A NULL out or a NULL
 *  mapPath returns false.
 *
 *  The manifest is filled for every script that parsed,
 *  whether or not issues were found, so a caller that wants
 *  what the table says has it from the same call.
 *
 *  A NULL sim leaves out the two checks that read a map —
 *  the rules against the catalogue and the tags against the
 *  entity lists — and makes every other one as usual.
 *
 *  Each issue carries the line Lua gave where Lua gave one,
 *  and otherwise the first line of the source holding the
 *  last name in the issue's key as a whole word. That is a
 *  search rather than a position the parse kept, so it can
 *  be led astray in two ways an author should know about: a
 *  name written earlier in a comment or a string takes the
 *  line, and a key two tables both carry — a second team's
 *  bots — takes the first one's line.
 *********************************************************/
bool scenarioValidateMap(const ServerSim *sim, const char *mapPath,
                         ScnValidateResult *out);

/*********************************************************
 *NAME:          scnIssueAdd
 *PURPOSE:
 *  Appends one problem to the list, with line 0 for the
 *  attribution pass to fill in. key may be "" where no key
 *  applies. A NULL out is a no-op; a list already full
 *  counts the problem in dropped and keeps the ones it has.
 *********************************************************/
void scnIssueAdd(ScnValidateResult *out, const char *key,
                 const char *fmt, ...);

/* ── The parse, which the host and the validator share ──────────────── */

/* Where a problem the parse found is said: one line to the operator and a
 * copy in soft for whoever asked, and an issue on sink when a validator is
 * collecting them. Each of the three is optional — the host passes a soft
 * buffer and no sink, and a caller that wants neither passes nothing. */
typedef struct {
    char              *soft;
    size_t             softLen;
    ScnValidateResult *sink;
} ScnParseReport;

/* Named rather than included: nothing else on this header names a Lua type,
 * and a frontend reading the result of a validation has no Lua headers on its
 * include path. */
struct lua_State;

/*********************************************************
 *NAME:          scnScriptPath
 *PURPOSE:
 *  The script a map is looked for beside: .../X.map is
 *  accompanied by .../X.scenario.lua. A path that does not
 *  end in .map keeps its whole name and takes the suffix as
 *  it is. False when the result would not fit.
 *********************************************************/
bool scnScriptPath(const char *mapPath, char *out, size_t outLen);

/*********************************************************
 *NAME:          scnReadFile
 *PURPOSE:
 *  The whole script, into a buffer the caller owns and
 *  frees. False with err set when the file is there but
 *  cannot be used — too large to be a script, or unreadable
 *  — and false with err left empty when there is no file at
 *  all, which is not a fault.
 *********************************************************/
bool scnReadFile(const char *path, char **out, size_t *outLen,
                 char *err, size_t errLen);

/*********************************************************
 *NAME:          scnNewVm
 *PURPOSE:
 *  A Lua state opened on the sandbox's own library rather
 *  than on the standard one — no io, no package, no debug —
 *  counted against SCN_VM_MEMORY_MAX for the memory it holds
 *  and SCN_BUDGET_CALL_INSTR for the instructions any one
 *  call into it may spend, and with math.random seeded from
 *  the process PRNG's state. No game table:
 *  whoever boots the state installs the surface it is to
 *  have. NULL when there is no memory for one.
 *********************************************************/
struct lua_State *scnNewVm(void);

/*********************************************************
 *NAME:          scnCloseVm
 *PURPOSE:
 *  Closes a state scnNewVm made. Every close goes through
 *  here, because a state carries its memory count beside it
 *  and lua_close would leave that behind. NULL is nothing to
 *  close.
 *********************************************************/
void scnCloseVm(struct lua_State *L);

/*********************************************************
 *NAME:          scnRunChunk
 *PURPOSE:
 *  Loads and runs one chunk. Lua's own message already names
 *  the source and the line for both a syntax error and one
 *  the chunk raises, so err carries it through rather than
 *  summarising it. chunkName carries the leading '@' that
 *  tells Lua the name is a file.
 *********************************************************/
bool scnRunChunk(struct lua_State *L, const char *src, size_t srcLen,
                 const char *chunkName, char *err, size_t errLen);

/*********************************************************
 *NAME:          scnReadManifest
 *PURPOSE:
 *  The scenario table the chunk declared, into the struct. A
 *  state that declares no such table is refused with one
 *  line in err, because it ran but it is not a scenario.
 *
 *  A key that names no rule, an entity index the map could
 *  not hold, more tags than an entity carries and more
 *  regions than a round has are each said through rep and
 *  the rest of the table is still read: one bad line should
 *  not cost an author every other line.
 *
 *  rep may be NULL, which says the problems nowhere.
 *********************************************************/
bool scnReadManifest(struct lua_State *L, ScenarioManifest *m,
                     const char *path, char *err, size_t errLen,
                     ScnParseReport *rep);

#endif /* SCENARIO_VALIDATE_H */
