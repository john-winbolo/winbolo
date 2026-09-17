/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_check.c
 * Purpose:
 *   Runs the scenario validator over the pane's text and
 *   reads the binding registry for the completion list.
 *
 *   These two calls, scenarioValidateSource and
 *   scenarioLuaRows, are what the editor asks of
 *   scenario_static, along with the script path
 *   mapeditor_scenario.c derives through scnScriptPath.
 *   Nothing here creates a ServerSim, attaches a host or
 *   ticks anything.
 *********************************************************/

#include <stddef.h>
#include <string.h>

#include "scenario_lua.h"      /* scenarioLuaRows — the completion list */
#include "scenario_validate.h" /* scenarioValidateSource */

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
}

void meScenarioCheckClear(MEScenarioCheck *c) {
    meScenarioCheckInit(c);
}

void meScenarioCheckRun(MEScenarioCheck *c, const char *text, size_t len,
                        const char *name, const ScenarioManifest *manifest) {
    if (c == NULL) {
        return;
    }

    memset(&c->result, 0, sizeof(c->result));
    c->hasRun       = true;
    c->pushToWidget = true;

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
