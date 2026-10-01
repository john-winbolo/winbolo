/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_fnscan.h
 * Purpose:
 *   Which functions a scenario script defines, and where.
 *
 *   The catalogue says what an author may write; this says
 *   what this file has written already, so the editor can
 *   tell one from the other and put the cursor on a
 *   definition the author asks for.
 *
 *   It also says how each definition was spelled, because
 *   the host reaches only two of the spellings and a row
 *   that says "defined" about the other three would be
 *   telling the author a hook runs when it never will.
 *
 *   A line scan over the buffer, not a parse: it links no
 *   Lua, loads no chunk and calls no validator. What that
 *   costs is set out at the top of the .c.
 *********************************************************/

#ifndef MAPEDITOR_SCENARIO_FNSCAN_H
#define MAPEDITOR_SCENARIO_FNSCAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Room for a name and its terminator. The longest function the catalogue
 * holds is on_base_neutralized at nineteen bytes, so this is far above
 * anything the editor matches against and still leaves an author's own
 * helpers room to be named properly. A name that does not fit is left out
 * of the answer rather than cut down to size: a cut name would match a
 * catalogue row that is not the one the file defines. */
#define ME_SCN_FN_NAME_MAX 64

/* How a definition was written, which is what says whether the host will
 * ever call it. The host looks a hook up as a global and then as a field of
 * the scenario table, and nowhere else, so only the first two of these are
 * resolved at all — the other three are definitions the author can see in
 * the script and the game will never run.
 *
 * A qualified name is classified by its table whatever else is on the line.
 * local function scenario.on_tick is not Lua, so a dot or a colon means the
 * definition is a field of some table, and the table's name is the only
 * thing that decides whether the host reaches it. What that cannot tell is
 * a local table from a global one: a script that writes local scenario = {}
 * and then function scenario.on_tick is reported as reachable and is not.
 * Seeing that needs scope, which a line scan has not got. */
typedef enum {
    ME_SCN_FORM_GLOBAL,    /* function on_tick(...), on_tick = function:
                              the host resolves it as a global */
    ME_SCN_FORM_SCENARIO,  /* function scenario.on_tick(...),
                              scenario.on_tick = function: the host resolves
                              it off the scenario table */
    ME_SCN_FORM_COLON,     /* function scenario:on_tick(...): resolved, but
                              self takes the first argument and the rest
                              shift along behind it */
    ME_SCN_FORM_LOCAL,     /* local function on_tick(...), local on_tick =
                              function: never resolved, since the host reads
                              the globals and a local is not one */
    ME_SCN_FORM_TABLE      /* function M.on_tick(...) on any table that is
                              not scenario, with a dot or a colon: never
                              resolved. M:on_tick answers this rather than
                              the colon form, since a hook that never runs
                              at all is the first thing wrong with it */
} MEScnFnForm;

/* One function definition found in a script, the line it is on and how it
 * was spelled. */
typedef struct {
    char        name[ME_SCN_FN_NAME_MAX];
    int         line;   /* 1-based, as the editor numbers lines */
    MEScnFnForm form;
} MEScnFoundFn;

/* Every top-level function definition in text, in the order they appear.
 * Answers how many were written to out, which is at most outMax; a script
 * with more than that is truncated rather than refused. A NULL text or a
 * NULL out answers 0. */
size_t meScnScanFunctions(const char *text, MEScnFoundFn *out, size_t outMax);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SCENARIO_FNSCAN_H */
