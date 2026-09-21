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
 *
 *   The rest of the file reads the catalogue's rows rather
 *   than the lists behind them, and writes out the text a
 *   list of functions needs: the name, the parameter list
 *   and the stub an author starts a function from.
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

/* ── The parameter types, held to the catalogue's ───────────────────── */

/* One assert per member, pasted onto the same name at both ends, so a type
 * renamed in the catalogue does not compile here and one renumbered or
 * inserted does not pass. */
#define ME_SCN_PARAM_ASSERT(id)                                              \
    BOLO_STATIC_ASSERT((int)ME_SCN_PARAM_##id == (int)SCN_PARAM_##id,        \
                       scenario_param_##id##_is_the_editors_own_value);

ME_SCN_PARAM_LIST(ME_SCN_PARAM_ASSERT)

#undef ME_SCN_PARAM_ASSERT

MEScnParamType meScnParamType(int catalogueType) {
    /* The values are equal member for member, so this is a rename and not a
       mapping. What it is here for is the range: a type the catalogue has
       grown since comes back as NONE, which no field of a real parameter
       is. */
    if (catalogueType < 0 || catalogueType >= (int)ME_SCN_PARAM_COUNT) {
        return ME_SCN_PARAM_NONE;
    }
    return (MEScnParamType)catalogueType;
}

/* ── The text a list of functions is drawn from ─────────────────────── */

/* The catalogue row at that index, or NULL for an index it does not hold.
 * Every accessor below goes through here, so a row off the end answers
 * emptily in one place rather than in five. */
static const ScnLuaFnRow *meScnFnRowAt(size_t row) {
    const ScnLuaFnRow *rows;
    size_t             count = 0;

    rows = scenarioLuaFunctions(&count);
    if (rows == NULL || row >= count) {
        return NULL;
    }
    return &rows[row];
}

/* Adds src to what is being written, and adds its length to *want whether
 * there was room for it or not. That is what lets the answer say how much a
 * complete one would have taken once the buffer has run out: *want walks
 * past outLen and keeps counting. The last byte of out is left for the
 * terminator meScnFnEnd puts there. */
static void meScnFnAdd(char *out, size_t outLen, size_t *want,
                       const char *src) {
    size_t i;

    for (i = 0; src[i] != '\0'; i++) {
        if (out != NULL && *want + i + 1 < outLen) {
            out[*want + i] = src[i];
        }
    }
    *want += i;
}

/* Terminates what was written, at the end of it or at the last byte of the
 * buffer, whichever came first. */
static void meScnFnEnd(char *out, size_t outLen, size_t want) {
    if (out == NULL || outLen == 0) {
        return;
    }
    out[want < outLen ? want : outLen - 1] = '\0';
}

/* The parameter list of one row, added to what is already there. The names
 * are the author's own, and the trailing scripted flag belongs to an event
 * hook and to nothing else: the kind is what says there is one, because the
 * catalogue's names stop short of it. */
static void meScnFnAddParams(const ScnLuaFnRow *r, char *out, size_t outLen,
                             size_t *want) {
    size_t i;

    meScnFnAdd(out, outLen, want, "(");
    for (i = 0; i < r->paramCount; i++) {
        if (i > 0) {
            meScnFnAdd(out, outLen, want, ", ");
        }
        meScnFnAdd(out, outLen, want, r->params[i].name);
    }
    if (r->kind == SCN_FN_EVENT_HOOK) {
        if (r->paramCount > 0) {
            meScnFnAdd(out, outLen, want, ", ");
        }
        meScnFnAdd(out, outLen, want, "scripted");
    }
    meScnFnAdd(out, outLen, want, ")");
}

size_t meScnFnCount(void) {
    size_t count = 0;

    (void)scenarioLuaFunctions(&count);
    return count;
}

const char *meScnFnName(size_t row) {
    const ScnLuaFnRow *r = meScnFnRowAt(row);

    return (r != NULL && r->name != NULL) ? r->name : "";
}

const char *meScnFnReturns(size_t row) {
    const ScnLuaFnRow *r = meScnFnRowAt(row);

    return (r != NULL && r->returns != NULL) ? r->returns : "";
}

bool meScnFnIsHook(size_t row) {
    const ScnLuaFnRow *r = meScnFnRowAt(row);

    /* The kind rather than the name: the three hook kinds are what the host
       dispatches and a policy is what it questions, and a function that moved
       between the two lists would keep its name. */
    return r != NULL && r->kind != SCN_FN_POLICY;
}

size_t meScnFnParamList(size_t row, char *out, size_t outLen) {
    const ScnLuaFnRow *r    = meScnFnRowAt(row);
    size_t             want = 0;

    if (out != NULL && outLen > 0) {
        out[0] = '\0';
    }
    if (r == NULL) {
        return 0;
    }
    meScnFnAddParams(r, out, outLen, &want);
    meScnFnEnd(out, outLen, want);
    return want;
}

size_t meScnFnStub(size_t row, char *out, size_t outLen) {
    const ScnLuaFnRow *r    = meScnFnRowAt(row);
    size_t             want = 0;

    if (out != NULL && outLen > 0) {
        out[0] = '\0';
    }
    if (r == NULL) {
        return 0;
    }
    meScnFnAdd(out, outLen, &want, "function ");
    meScnFnAdd(out, outLen, &want, r->name);
    meScnFnAddParams(r, out, outLen, &want);
    /* Two lines and no third. A body would be this file writing the
       author's function for them, and for a policy it would be an answer
       they have not given. */
    meScnFnAdd(out, outLen, &want, "\nend");
    meScnFnEnd(out, outLen, want);
    return want;
}

/* ── The fields a trigger may test ──────────────────────────────────── */

BOLO_STATIC_ASSERT(ME_SCN_FIELD_NAME_LEN == SCN_FN_FIELD_NAME_LEN,
                   scenario_field_name_fits_the_editors_buffer);

/* As many fields as one row reaches, with room over. Ten is the most any of
 * them has, and scenario_validate.c holds its own copy against a bound of the
 * same size for the same reason. */
#define ME_SCN_FN_FIELDS_MAX 16

/* The fields of one row into out, and how many of them there are — never more
 * than out holds, so the count one accessor answers is the count the other
 * indexes. */
static size_t meScnFnFieldsOf(size_t row, ScnLuaFnField *out) {
    const size_t max = (size_t)ME_SCN_FN_FIELDS_MAX;
    const size_t n   = scenarioLuaFnFields(row, out, max);

    return (n > max) ? max : n;
}

size_t meScnFnFieldCount(size_t row) {
    ScnLuaFnField fields[ME_SCN_FN_FIELDS_MAX];

    return meScnFnFieldsOf(row, fields);
}

bool meScnFnFieldAt(size_t row, size_t index, char *name, size_t nameLen,
                    MEScnParamType *type, bool *derived) {
    ScnLuaFnField fields[ME_SCN_FN_FIELDS_MAX];
    const size_t  count = meScnFnFieldsOf(row, fields);
    size_t        want  = 0;

    if (index >= count) {
        return false;
    }
    if (name != NULL && nameLen > 0) {
        name[0] = '\0';
        meScnFnAdd(name, nameLen, &want, fields[index].name);
        meScnFnEnd(name, nameLen, want);
    }
    if (type != NULL) {
        *type = meScnParamType((int)fields[index].type);
    }
    if (derived != NULL) {
        *derived = fields[index].derived;
    }
    return true;
}
