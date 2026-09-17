/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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

/* One function definition found in a script, and the line it is on. */
typedef struct {
    char name[ME_SCN_FN_NAME_MAX];
    int  line;   /* 1-based, as the editor numbers lines */
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
