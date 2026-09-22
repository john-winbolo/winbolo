/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_check.c
 * Purpose:
 *   Runs the scenario validator over the pane's text and
 *   reads the binding registry, for the completion list and
 *   for the ops a trigger's actions are written against.
 *
 *   These calls — scenarioValidateSource, scenarioLuaRows,
 *   scenarioLuaOpIsScalar and scenarioLuaOpIsAction, and
 *   the two that say what a mod may not decide,
 *   scenarioLuaRoundDeciderAt and scenarioLuaFnDecidesRound
 *   — are what the editor asks of scenario_static, along
 *   with the script path mapeditor_scenario.c derives
 *   through scnScriptPath.
 *   Nothing here creates a ServerSim, attaches a host or
 *   ticks anything.
 *********************************************************/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "scenario_lua.h"      /* scenarioLuaRows — the completion list */
#include "scenario_validate.h" /* scenarioValidateSource */

/* The one sibling this asks anything of: which functions the script in the
   pane defines, for the half of the mod rule that is about what a file writes
   rather than what it calls. A line scan over the same text, linking nothing
   of its own. */
#include "mapeditor_scenario_fnscan.h"

#include "mapeditor_scenario_check.h"

void meScenarioCheckInit(MEScenarioCheck *c) {
    if (c == NULL) {
        return;
    }
    memset(&c->result, 0, sizeof(c->result));
    c->hasRun       = false;
    /* Set, not cleared: an empty result still has to reach the widget, or the
       markers of the run before it stay on the lines. */
    c->pushToWidget = true;
    c->stale        = false;
}

void meScenarioCheckClear(MEScenarioCheck *c) {
    meScenarioCheckInit(c);
}

/* Whether a byte starts or continues a Lua name, which is what tells
   end_round from end_rounds. */
static bool meScnNameByte(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

/* Whether the file being checked says it is a mod. The table the check read
   back is the one the round will read; the one handed in stands only where
   the script declared none of its own. Both passes below ask this, so which
   of the two answers is decided in one place. */
static bool meScnCheckIsMod(const MEScenarioCheck *c,
                            const ScenarioManifest *manifest) {
    return scnManifestKeepsWinCondition(
        c->result.haveManifest ? &c->result.manifest : manifest);
}

/* A mod's script read as text, looking for the ops a mod may not call.
 *
 * A guess, and said as one. This is a search through the source and not a
 * parse of it: a name it finds may be inside a comment, inside a string, or
 * on a line the round never reaches, and a script that builds the call some
 * other way is one this finds nothing in. What makes it worth having anyway
 * is that the alternative is silence until the round raises. The declared
 * half of a file is refused when it loads, because a trigger's action is on
 * the page; an ordinary call written in Lua is visible to nothing until it
 * runs.
 *
 * Only for a mod. A scenario may call every one of these. */
static void meScenarioCheckModCalls(MEScenarioCheck *c, const char *text,
                                    size_t len,
                                    const ScenarioManifest *manifest) {
    size_t i;
    size_t before;

    if (!meScnCheckIsMod(c, manifest) || text == NULL || len == 0) {
        return;
    }

    for (i = 0; ; i++) {
        const char *op = scenarioLuaRoundDeciderAt(i);
        size_t      opLen;
        size_t      at;
        int         line = 1;

        if (op == NULL) {
            break;
        }
        opLen = strlen(op);
        for (at = 0; at + opLen <= len; at++) {
            if (text[at] == '\n') {
                line++;
            }
            if (text[at] != op[0] || memcmp(text + at, op, opLen) != 0) {
                continue;
            }
            /* Reached through a table, which is the only way a script reaches
               one of these: game.end_round and never a bare end_round. That
               also keeps a local of the author's that happens to share the
               word out of the list. */
            if (at == 0 || text[at - 1] != '.') {
                continue;
            }
            /* And the whole name, not the front of a longer one. */
            if (at + opLen < len && meScnNameByte(text[at + opLen])) {
                continue;
            }
            before = c->result.count;
            scnIssueAdd(&c->result, "kind",
                        "scenario: this looks like a call to %s, which "
                        "decides the round, and the file says it is a mod. A "
                        "guess: the source is read here as text rather than "
                        "parsed, so a name in a comment or a string reads the "
                        "same as a call. The round itself raises on the call "
                        "if it is one.", op);
            /* Only where the add took. A full list drops the issue and leaves
               the count where it was, and stamping on that count would put
               this line number on whatever unrelated issue is last. */
            if (c->result.count > before) {
                c->result.issues[c->result.count - 1].line = line;
            }
            break;   /* one line per op is enough to say it */
        }
    }
}

/* As many definitions as one script is read for here. The catalogue is 35
   rows and the rest of what a file defines is the author's own helpers, so
   this is well above what a scenario reaches; the functions view scans with
   the same bound. A definition past the end is one this does not see. */
#define ME_SCN_CHECK_FOUND_MAX 128

/* Whether the host ever reaches a definition written that way. It reads a
   name off the globals and then off the scenario table and nowhere else, so
   those two spellings and the colon form of the second are the three it
   resolves; the forms are set out in mapeditor_scenario_fnscan.h. A local,
   or a field of a table of the author's own, is a name the round never asks
   for whatever the file says it is. */
static bool meScnFormResolved(MEScnFnForm form) {
    switch (form) {
    case ME_SCN_FORM_GLOBAL:
    case ME_SCN_FORM_SCENARIO:
    case ME_SCN_FORM_COLON:
        return true;
    case ME_SCN_FORM_LOCAL:
    case ME_SCN_FORM_TABLE:
        return false;
    }
    /* A form this does not know answers no, which leaves a definition
       unflagged rather than flagged for a reason nobody has checked. The five
       are named above and no default label stands with them, so a sixth added
       to the enum is a warning here rather than a silent no: a default label
       would take that warning away, which is the whole reason there is not
       one. This return is what the switch falls out to, and what a caller
       reaching it with a value that is in no case at all gets. */
    return false;
}

/* The other half of the same rule, on what a mod's script defines rather than
 * on what it calls: the function whose answer decides an ending.
 *
 * Worth saying for the opposite reason to the ops above. An op raises, so the
 * round tells the author itself; this one is ignored instead — the sim called
 * the script rather than the script calling the sim, and a raise would be
 * counted against a file whose only fault is having written a function this
 * round will not ask. The host says so once on its console, which an author
 * editing a map is not reading. Without this line they see a function they
 * wrote doing nothing.
 *
 * Only the spellings the host resolves, which is what makes the line true.
 * A local of the author's own, or a function on a module table of theirs, is
 * a name the round would pass over in a scenario exactly as it does in a mod:
 * the manifest is not what stops it, so a line saying the manifest stops it
 * would name a cause that is not the cause. What is wrong with those
 * spellings is the spelling, which is the functions view's line to say, on
 * the row itself. A private local is the author's to name what they like.
 *
 * A guess, and said as one, for the reasons at the top of fnscan: the source
 * is read a line at a time and not parsed, so a definition inside a block
 * comment or a string reads the same as a live one.
 *
 * Only for a mod. A scenario may define every function there is. */
static void meScenarioCheckModFns(MEScenarioCheck *c, const char *text,
                                  size_t len,
                                  const ScenarioManifest *manifest) {
    MEScnFoundFn found[ME_SCN_CHECK_FOUND_MAX];
    char        *copy;
    size_t       n;
    size_t       i;
    size_t       before;

    if (!meScnCheckIsMod(c, manifest) || text == NULL || len == 0) {
        return;
    }

    /* The scan reads to a terminator and this call is handed a length, so the
       text is copied rather than read past where the caller said it ends. One
       copy of the source is nothing beside the chunk the check has just
       loaded and run in a Lua state of its own; out of memory here leaves the
       issues the rest of the run found and says nothing more. */
    copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    n         = meScnScanFunctions(copy, found, ME_SCN_CHECK_FOUND_MAX);
    free(copy);

    for (i = 0; i < n; i++) {
        size_t j;
        bool   said = false;

        if (!meScnFormResolved(found[i].form) ||
            !scenarioLuaFnDecidesRound(found[i].name)) {
            continue;
        }
        /* One line per name rather than per definition: a name written twice
           is the one thing wrong said twice, and the line this stands on is
           the first of them, which is where the author goes to undo it. The
           loop runs on instead of stopping there, the way the op loop above
           carries on to the next op — a second deciding function, if the
           runtime's list comes to hold one, gets a line of its own. */
        for (j = 0; j < i; j++) {
            if (meScnFormResolved(found[j].form) &&
                strcmp(found[j].name, found[i].name) == 0) {
                said = true;
                break;
            }
        }
        if (said) {
            continue;
        }
        before = c->result.count;
        scnIssueAdd(&c->result, "kind",
                    "scenario: this file says it is a mod and looks like it "
                    "defines %s, so the round does not read it: a mod leaves "
                    "the win condition alone. A guess: the source is read as "
                    "text.", found[i].name);
        /* Only where the add took, for the reason the call pass above gives. */
        if (c->result.count > before) {
            c->result.issues[c->result.count - 1].line = found[i].line;
        }
    }
}

void meScenarioCheckRun(MEScenarioCheck *c, const char *text, size_t len,
                        const char *name, const ScenarioManifest *manifest) {
    if (c == NULL) {
        return;
    }

    memset(&c->result, 0, sizeof(c->result));
    c->hasRun       = true;
    c->pushToWidget = true;
    c->runs++;
    /* What follows was found in the text handed in here, so the issues stand
       on the script as it is until the next edit. */
    c->stale        = false;

    if (name == NULL || name[0] == '\0') {
        name = ME_SCENARIO_CHECK_UNNAMED;
    }
    if (text == NULL) {
        text = "";
        len  = 0;
    }

    /* A NULL sim on purpose: see the header. An empty buffer is checked as the
       empty script it is rather than skipped, which is what the source entry
       is for. The manifest goes on as the scenario global first, which is the
       one thing that made the editor's check stricter than the host's load. */
    scenarioValidateSource(NULL, text, len, name, manifest, &c->result);

    /* And last, the half no parse of the table can see: an ordinary call to
       one of those ops, and a definition of the one function whose answer
       would decide an ending, both written in the script's own Lua. */
    meScenarioCheckModCalls(c, text, len, manifest);
    meScenarioCheckModFns(c, text, len, manifest);
}

size_t meScenarioCompletionCount(void) {
    size_t n = 0;

    (void)scenarioLuaRows(&n);
    return n;
}

bool meScenarioCompletionAt(size_t index, const char **name,
                            const char **doc) {
    const ScnLuaRow *rows;
    size_t           n = 0;

    rows = scenarioLuaRows(&n);
    if (rows == NULL || index >= n) {
        return false;
    }
    if (name != NULL) {
        *name = rows[index].name;
    }
    if (doc != NULL) {
        *doc = rows[index].doc;
    }
    return true;
}

/* The registry row at that index, or NULL for one it does not hold. The four
 * below go through here, so an index off the end is answered for in one
 * place. */
static const ScnLuaRow *meScenarioOpAt(size_t index) {
    const ScnLuaRow *rows;
    size_t           n = 0;

    rows = scenarioLuaRows(&n);
    return (rows == NULL || index >= n) ? NULL : &rows[index];
}

bool meScenarioOpIsScalar(size_t index) {
    const ScnLuaRow *row = meScenarioOpAt(index);

    /* The accessor rather than a list of names: an op that grows a table
       argument drops out of what an action can state without anything here
       being told about it. */
    return row != NULL && scenarioLuaOpIsScalar(row);
}

bool meScenarioOpIsAction(size_t index) {
    const ScnLuaRow *row = meScenarioOpAt(index);

    /* Both halves off the registry: an op that stops changing anything, like
       one that grows a table argument, drops out of what a combo offers
       without anything here being told about it. */
    return row != NULL && scenarioLuaOpIsAction(row);
}

bool meScenarioOpDecidesRound(size_t index) {
    const ScnLuaRow *row = meScenarioOpAt(index);

    /* By name, because that is what the runtime's list holds: a trigger's
       action states the op it calls as text, so one list answers both the
       editor here and the host reading a file back. */
    return row != NULL && scenarioLuaOpDecidesRound(row->name);
}

size_t meScenarioOpParamCount(size_t index) {
    const ScnLuaRow *row = meScenarioOpAt(index);

    return (row != NULL) ? row->paramCount : 0;
}

bool meScenarioOpParamAt(size_t index, size_t param, const char **name,
                         MEScnParamType *type, bool *optional) {
    const ScnLuaRow *row = meScenarioOpAt(index);

    if (row == NULL || param >= row->paramCount) {
        return false;
    }
    if (name != NULL) {
        *name = row->params[param].name;
    }
    if (type != NULL) {
        *type = meScnParamType((int)row->params[param].type);
    }
    if (optional != NULL) {
        *optional = row->params[param].optional;
    }
    return true;
}
