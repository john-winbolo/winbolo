/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_form.c
 * Purpose:
 *   The manifest the scenario panel's forms edit, and the
 *   list handling those forms need: setting a rule without
 *   ever giving one rule two rows, removing a row without
 *   leaving a hole, and adding a team with a team number
 *   nobody else has.
 *
 *   The struct is the whole state. Every mutator marks the
 *   form dirty and none of them touches a file.
 *********************************************************/

#include "mapeditor_scenario_form.h"

#include <stdint.h>
#include <string.h>

/* SCENARIO_API_VERSION: the version of the scenario surface this build
 * writes, so a new manifest states the one it was authored against. Only the
 * constant is taken from there — the editor links neither the scenario
 * runtime nor the container library, and calls nothing declared on that
 * header. gamefront.c takes the same header the same way. */
#include "../scenario/scenario_host.h"

void meScenarioFormReset(MEScenarioForm *f) {
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->manifest.api = SCENARIO_API_VERSION;
    /* A scenario is written for the map it is being edited beside until the
     * author says otherwise. */
    f->manifest.bound = true;
    f->dirty          = false;
}

void meScenarioFormInit(MEScenarioForm *f) { meScenarioFormReset(f); }

int meScenarioFormFindRule(const MEScenarioForm *f, int rule) {
    uint16_t i;

    if (f == NULL || rule < 0 || rule > UINT16_MAX) {
        return -1;
    }
    for (i = 0; i < f->manifest.numRules; i++) {
        if (f->manifest.rules[i].rule == (uint16_t)rule) {
            return (int)i;
        }
    }
    return -1;
}

bool meScenarioFormSetRule(MEScenarioForm *f, int rule, double value) {
    int at;

    if (f == NULL || rule < 0 || rule > UINT16_MAX) {
        return false;
    }

    at = meScenarioFormFindRule(f, rule);
    if (at >= 0) {
        f->manifest.rules[at].value = value;
        f->dirty                    = true;
        return true;
    }

    if (f->manifest.numRules >= SCN_MANIFEST_RULES_MAX) {
        return false;
    }
    f->manifest.rules[f->manifest.numRules].rule  = (uint16_t)rule;
    f->manifest.rules[f->manifest.numRules].value = value;
    f->manifest.numRules++;
    f->dirty = true;
    return true;
}

void meScenarioFormRemoveRule(MEScenarioForm *f, int index) {
    if (f == NULL || index < 0 || index >= (int)f->manifest.numRules) {
        return;
    }
    /* The rows after it close up, so the list stays the list the author sees
     * and numRules still counts it. */
    memmove(&f->manifest.rules[index], &f->manifest.rules[index + 1],
            (size_t)((int)f->manifest.numRules - index - 1) *
                sizeof(f->manifest.rules[0]));
    f->manifest.numRules--;
    memset(&f->manifest.rules[f->manifest.numRules], 0,
           sizeof(f->manifest.rules[0]));
    f->dirty = true;
}

bool meScenarioFormAddTeam(MEScenarioForm *f) {
    ScnManifestLobby *lob;
    ScnManifestTeam  *team;
    unsigned          id;

    if (f == NULL) {
        return false;
    }
    lob = &f->manifest.lobby;
    /* A team number runs 1 to MAX_TANKS - 1, the range scenario_validate.c
     * holds an id to: the engine drops a team at MAX_TANKS or above without
     * saying so. Team numbers are unique and start at 1, so one team past
     * that count would have to take a number no game will seat, and the form
     * does not offer what is always refused. teams[] therefore ends with a
     * slot the template never fills. */
    if (lob->numTeams >= MAX_TANKS - 1) {
        return false;
    }

    /* The lowest team number the template does not already seat, counting
     * from the first one a game has. */
    for (id = 1; id <= MAX_TANKS - 1; id++) {
        uint8_t i;
        bool    taken = false;
        for (i = 0; i < lob->numTeams; i++) {
            if (lob->teams[i].id == (uint8_t)id) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            break;
        }
    }

    team = &lob->teams[lob->numTeams];
    memset(team, 0, sizeof(*team));
    team->id      = (uint8_t)id;
    team->bots    = 0;
    team->maxBots = 0;
    team->fielded = false;
    lob->numTeams++;
    f->dirty = true;
    return true;
}

void meScenarioFormRemoveTeam(MEScenarioForm *f, int index) {
    ScnManifestLobby *lob;

    if (f == NULL) {
        return;
    }
    lob = &f->manifest.lobby;
    if (index < 0 || index >= (int)lob->numTeams) {
        return;
    }
    memmove(&lob->teams[index], &lob->teams[index + 1],
            (size_t)((int)lob->numTeams - index - 1) * sizeof(lob->teams[0]));
    lob->numTeams--;
    memset(&lob->teams[lob->numTeams], 0, sizeof(lob->teams[0]));
    f->dirty = true;
}

bool meScenarioFormDirty(const MEScenarioForm *f) {
    return f != NULL && f->dirty;
}
