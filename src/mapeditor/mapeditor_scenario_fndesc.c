/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_fndesc.c
 * Purpose:
 *   One description id per row of the function catalogue,
 *   read off the two lists themselves so a hook or a policy
 *   added without a line of its own does not compile.
 *********************************************************/

#include <stddef.h>

#include "platform_types.h"  /* BOLO_STATIC_ASSERT */
#include "scenario_lua.h"    /* SCN_HOOK_LIST, SCN_POLICY_LIST */
#include "../gui/lang.h"     /* langGetText, STR_SCNFN_DESC_* */

#include "mapeditor_scenario_fndesc.h"

/* The hooks first and then the policies, which is the order
 * scenarioLuaFunctions answers its rows in, because that accessor expands
 * the same two lists in the same order. Nothing here spells a function's
 * name: the id column is pasted onto, so a name changed in the catalogue
 * cannot leave a description pointing at the function it used to be. */

#define ME_SCN_FN_HOOK_DESC_ROW(id, name, kind, params) STR_SCNFN_DESC_##id,
#define ME_SCN_FN_POLICY_DESC_ROW(id, name, params, returns)                 \
    STR_SCNFN_DESC_##id,

static const langid meScnFnDescIds[] = {
    SCN_HOOK_LIST(ME_SCN_FN_HOOK_DESC_ROW)
    SCN_POLICY_LIST(ME_SCN_FN_POLICY_DESC_ROW)
};

#undef ME_SCN_FN_HOOK_DESC_ROW
#undef ME_SCN_FN_POLICY_DESC_ROW

/* The same tally scenario_lua.c holds its own table against, made here the
 * same way: a function added to either list arrives in both tables or in
 * neither. */
#define ME_SCN_FN_HOOK_TALLY(id, name, kind, params) +1
#define ME_SCN_FN_POLICY_TALLY(id, name, params, returns) +1

BOLO_STATIC_ASSERT(
    (int)(sizeof(meScnFnDescIds) / sizeof(meScnFnDescIds[0])) ==
        (0 SCN_HOOK_LIST(ME_SCN_FN_HOOK_TALLY)) +
        (0 SCN_POLICY_LIST(ME_SCN_FN_POLICY_TALLY)),
    scenario_function_descriptions_cover_every_row);

#undef ME_SCN_FN_HOOK_TALLY
#undef ME_SCN_FN_POLICY_TALLY

const char *meScnFnDescription(size_t row) {
    if (row >= sizeof(meScnFnDescIds) / sizeof(meScnFnDescIds[0])) {
        return "";
    }
    return langGetText(meScnFnDescIds[row]);
}
