/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_fndesc.h
 * Purpose:
 *   The one line the editor shows beside each hook and
 *   policy a scenario author can write.
 *
 *   The catalogue in scenario_lua.h says what the functions
 *   are, what kind each is and what it is handed; it does
 *   not say what any of them is for, because the scenario
 *   library has no lang table to say it in. This is that
 *   half, and it lives here rather than beside the rules'
 *   equivalent in src/gui/sdl3 because reaching the two list
 *   macros means including scenario_lua.h, which names Lua
 *   types. mapeditor_scenario_check.c already includes it
 *   and builds under both the standalone editor and the
 *   embedded one, so this file asks nothing new of either.
 *
 *   The rest of what a list needs is here for the same
 *   reason: the name, the parameter list and the stub an
 *   author starts from all come off the catalogue's rows,
 *   and the panel that draws the list is C++ while
 *   scenario_lua.h is a C header with no linkage guard on
 *   it. So the rows are read here and handed over as plain
 *   text.
 *
 *   docs/SCENARIO_API.md is where each function is set out
 *   at length. A line here is the short form for a list, not
 *   a replacement for that.
 *********************************************************/

#ifndef MAPEDITOR_SCENARIO_FNDESC_H
#define MAPEDITOR_SCENARIO_FNDESC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── What a parameter, a field or an argument holds ──────────────────
 *
 * ScnLuaParamType under the editor's own names. The catalogue's enum is
 * declared in scenario_lua.h, which names Lua types and carries no linkage
 * guard, so no C++ translation unit can include it; the view still has to
 * know whether a field takes a tag, a region, a team or a number, because
 * that is what decides the widget it draws beside one.
 *
 * So the members are spelled again here, one for one and in the catalogue's
 * order, and mapeditor_scenario_fndesc.c holds each of them against its twin
 * with an assert of its own. A member renamed, renumbered or inserted fails
 * the build rather than quietly turning one type into another.
 *
 * A member appended after the last is the one change those asserts cannot
 * see. meScnParamType answers NONE for a value this list has not got, so the
 * view gets a type it knows rather than a number it has to guess at, and
 * editor_form_trigger_vocabulary walks every field and every argument the
 * catalogue carries and refuses one that lands there — so the drift is a
 * failing case as soon as a row uses the new type. */
#define ME_SCN_PARAM_LIST(X)                                                 \
    X(NONE) X(SLOT) X(OWNER) X(TEAM) X(PILL) X(BASE) X(ITEM)                 \
    X(SQUARE_X) X(SQUARE_Y) X(WORD) X(TARGET) X(COLOUR) X(NUMBER) X(STRING)  \
    X(BOOL) X(TAG) X(REGION) X(TABLE) X(FUNCTION)

typedef enum {
#define ME_SCN_PARAM_ROW(id) ME_SCN_PARAM_##id,
    ME_SCN_PARAM_LIST(ME_SCN_PARAM_ROW)
#undef ME_SCN_PARAM_ROW
    ME_SCN_PARAM_COUNT /* how many there are, and no parameter's type */
} MEScnParamType;

/* One catalogue type under the name above. The argument is the catalogue's
 * own value as an int, because this header cannot name the enum it came out
 * of; every accessor here and in mapeditor_scenario_check.h has already been
 * through this, so the view has no reason to call it.
 *
 * ME_SCN_PARAM_NONE for a value the list above has not got. */
MEScnParamType meScnParamType(int catalogueType);

/* What the function does, one sentence. "" for a row that is not in the
 * catalogue. row indexes scenarioLuaFunctions, whose order this table
 * follows because both come from the same two lists. */
const char *meScnFnDescription(size_t row);

/* How many functions there are to list. Every row below takes an index
 * under this one and answers nothing for anything else. */
size_t meScnFnCount(void);

/* The name an author writes the function under — "on_tick", "can_build" —
 * as the catalogue spells it. "" for a row that is not in the catalogue. */
const char *meScnFnName(size_t row);

/* What a policy answers, in the words the catalogue puts it in. "" for a
 * hook, which answers nothing, and for a row that is not in the catalogue,
 * so this also tells a policy from a hook. */
const char *meScnFnReturns(size_t row);

/* Whether row names a hook rather than a policy. A trigger runs on a hook; a
 * policy is asked a question and a list of actions cannot answer one. False
 * for a row that is not in the catalogue.
 *
 * Read off the row's kind, so a function moved between the two lists changes
 * sides here with it. */
bool meScnFnIsHook(size_t row);

/* The parameter list in parentheses, in the author's own names:
 * "(p, mx, my, respawn, scripted)", or "()" for a function that takes none.
 * An event hook's list ends with the scripted flag the host hands it, which
 * the catalogue's own names stop short of; a lifecycle hook, a region hook
 * and a policy are handed no such flag and end where those names do.
 *
 * Answers how many bytes it wanted and writes at most outLen, the last of
 * them a terminator. out may be NULL, which asks for the length alone. */
size_t meScnFnParamList(size_t row, char *out, size_t outLen);

/* Room for any stub the catalogue can make. The longest is
 * on_tank_spawned's at 58 bytes, and this is far enough above that for a
 * function added to either list to fit without being thought about. */
#define ME_SCN_FN_STUB_MAX 256

/* The Lua a stub for this function looks like, without leading or trailing
 * blank lines: a "function name(params)" line and an "end" line. Answers
 * how many bytes it wanted, so a caller can tell a truncated answer from a
 * complete one; writes nothing for a row that is not in the catalogue.
 *
 * A policy's body is empty and stays that way. A policy that is absent or
 * answers nil means the ordinary rule, so an empty one behaves exactly as
 * if it had not been written and cannot change a round until the author
 * puts a return in it. Anything else here would be this file guessing at an
 * answer the author has not given.
 *
 * out may be NULL, which asks for the length alone. */
size_t meScnFnStub(size_t row, char *out, size_t outLen);

/* ── The fields a trigger on this function may test ──────────────────
 *
 * The function's own parameters, in order, and then the fields their types
 * earn: a team for a seat or an owner, a tag for a pillbox or a base, a
 * region for a square pair. What a where-row's field combo lists, and the
 * type is what the value beside it is drawn as. */

/* Room for a field's name and its terminator, which is the length the
 * catalogue builds one in; mapeditor_scenario_fndesc.c holds the two
 * equal. */
#define ME_SCN_FIELD_NAME_LEN 32

/* How many fields row carries. 0 for a row that is not in the catalogue and
 * for a function handed nothing at all — on_setup, on_start and on_end are
 * each called with no payload, and there is no test to write against one. */
size_t meScnFnFieldCount(size_t row);

/* One of those fields: its name into name, what it holds into type, and
 * whether it is derived rather than a parameter of the author's own into
 * derived. Any of the three may be NULL, and name is cut to fit nameLen
 * rather than written past it.
 *
 * The name is copied rather than pointed at because a derived one is built
 * from a parameter's and has nowhere of its own to live. False for a row or
 * an index the catalogue has not got, which leaves all three alone. */
bool meScnFnFieldAt(size_t row, size_t index, char *name, size_t nameLen,
                    MEScnParamType *type, bool *derived);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SCENARIO_FNDESC_H */
