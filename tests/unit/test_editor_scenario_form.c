/*
 * The manifest behind the scenario panel's metadata, lobby and rules forms.
 *
 * The forms themselves are C++ over ImGui and are the human half of this
 * work. This is the half that can be asserted: the list handling under the
 * Add and Remove buttons, which is where a rule could quietly gain a second
 * row or an array could be walked off the end.
 *
 * Nothing here reads or writes a file, so no case needs a fixture.
 *
 * run_editor_form_init        — an empty manifest states the API version and
 *                               is bound to its map, with nothing set
 * run_editor_form_rules       — a rule appends once and then updates in
 *                               place; find answers for a rule that is set
 *                               and refuses one that is not; remove keeps the
 *                               rest packed
 * run_editor_form_rules_full  — the list fills to its bound and refuses the
 *                               next rule rather than overrunning
 * run_editor_form_teams       — teams add up to the bound with team numbers
 *                               that do not repeat, and remove keeps the rest
 *                               packed
 * run_editor_form_rule_change — the answer the rules form shows beside a
 *                               reload of 2: faster, by 6.5 against the
 *                               classic 13. The kind and the number rather
 *                               than the words, because langGetText is a stub
 *                               in this binary and the words would be the
 *                               stub's.
 */

#include <math.h>
#include <string.h>

#include "mapeditor_scenario_form.h"
#include "scenario_host.h"
#include "sim_rules_names.h"
#include "test_harness.h"

int run_editor_form_init(void) {
    MEScenarioForm f;

    meScenarioFormInit(&f);

    UT_ASSERT_MSG(f.manifest.api == SCENARIO_API_VERSION, "api is %d",
                  f.manifest.api);
    UT_ASSERT(f.manifest.bound);
    UT_ASSERT(f.manifest.numRules == 0);
    UT_ASSERT(f.manifest.lobby.numTeams == 0);
    UT_ASSERT(f.manifest.name[0] == '\0');
    UT_ASSERT(f.manifest.description[0] == '\0');
    UT_ASSERT(f.manifest.game[0] == '\0');
    UT_ASSERT(!f.manifest.fillToCaps);
    UT_ASSERT(!f.manifest.lobby.extraTeams);
    UT_ASSERT(f.manifest.lobby.maxPlayers == 0);
    UT_ASSERT(!meScenarioFormDirty(&f));

    return 0;
}

int run_editor_form_rules(void) {
    MEScenarioForm f;
    int            at;

    meScenarioFormInit(&f);

    /* A rule nobody has set is not in the list. */
    UT_ASSERT(meScenarioFormFindRule(&f, SIM_RULE_tank_reload_ticks) == -1);

    /* The first set appends a row and marks the form edited. */
    UT_ASSERT(meScenarioFormSetRule(&f, SIM_RULE_tank_reload_ticks, 2.0));
    UT_ASSERT_MSG(f.manifest.numRules == 1, "numRules is %u",
                  (unsigned)f.manifest.numRules);
    UT_ASSERT(meScenarioFormDirty(&f));

    at = meScenarioFormFindRule(&f, SIM_RULE_tank_reload_ticks);
    UT_ASSERT_MSG(at == 0, "found at %d", at);
    UT_ASSERT(f.manifest.rules[0].value == 2.0);

    /* The same rule again moves the row it already has rather than adding a
     * second one for it. */
    UT_ASSERT(meScenarioFormSetRule(&f, SIM_RULE_tank_reload_ticks, 5.0));
    UT_ASSERT_MSG(f.manifest.numRules == 1, "numRules is %u",
                  (unsigned)f.manifest.numRules);
    UT_ASSERT(f.manifest.rules[0].value == 5.0);

    /* Two more rules, so a removal has something to close up over. */
    UT_ASSERT(meScenarioFormSetRule(&f, SIM_RULE_shell_damage, 9.0));
    UT_ASSERT(meScenarioFormSetRule(&f, SIM_RULE_pill_range, 3.0));
    UT_ASSERT(f.manifest.numRules == 3);

    /* Removing the first leaves the other two in order and counted. */
    meScenarioFormRemoveRule(&f, 0);
    UT_ASSERT_MSG(f.manifest.numRules == 2, "numRules is %u",
                  (unsigned)f.manifest.numRules);
    UT_ASSERT(meScenarioFormFindRule(&f, SIM_RULE_tank_reload_ticks) == -1);
    UT_ASSERT(f.manifest.rules[0].rule == SIM_RULE_shell_damage);
    UT_ASSERT(f.manifest.rules[0].value == 9.0);
    UT_ASSERT(f.manifest.rules[1].rule == SIM_RULE_pill_range);
    UT_ASSERT(f.manifest.rules[1].value == 3.0);

    /* An index off either end changes nothing. */
    meScenarioFormRemoveRule(&f, -1);
    meScenarioFormRemoveRule(&f, 2);
    UT_ASSERT(f.manifest.numRules == 2);

    /* A rule typed back to the value the classic game plays it at stays in
     * the list: the manifest records what the author set. */
    UT_ASSERT(meScenarioFormSetRule(
        &f, SIM_RULE_shell_damage,
        simRulesClassicValue(SIM_RULE_shell_damage)));
    UT_ASSERT(f.manifest.numRules == 2);
    UT_ASSERT(meScenarioFormFindRule(&f, SIM_RULE_shell_damage) == 0);

    return 0;
}

int run_editor_form_rules_full(void) {
    MEScenarioForm f;
    int            i;

    meScenarioFormInit(&f);

    /* The bound is above the number of rules there are, so the list is
     * filled with indices rather than with real rules: what is under test is
     * the array, not the rule table. */
    for (i = 0; i < SCN_MANIFEST_RULES_MAX; i++) {
        UT_ASSERT_MSG(meScenarioFormSetRule(&f, i, (double)i),
                      "rule %d refused below the bound", i);
    }
    UT_ASSERT_MSG(f.manifest.numRules == SCN_MANIFEST_RULES_MAX,
                  "numRules is %u", (unsigned)f.manifest.numRules);

    /* One more is refused, and the full list is left as it was. */
    UT_ASSERT(!meScenarioFormSetRule(&f, SCN_MANIFEST_RULES_MAX, 1.0));
    UT_ASSERT(f.manifest.numRules == SCN_MANIFEST_RULES_MAX);

    /* A rule already in the full list still moves, because that writes no
     * new row. */
    UT_ASSERT(meScenarioFormSetRule(&f, 0, 42.0));
    UT_ASSERT(f.manifest.numRules == SCN_MANIFEST_RULES_MAX);
    UT_ASSERT(f.manifest.rules[0].value == 42.0);

    /* And with one row gone there is room again. */
    meScenarioFormRemoveRule(&f, 0);
    UT_ASSERT(f.manifest.numRules == SCN_MANIFEST_RULES_MAX - 1);
    UT_ASSERT(meScenarioFormSetRule(&f, SCN_MANIFEST_RULES_MAX, 1.0));
    UT_ASSERT(f.manifest.numRules == SCN_MANIFEST_RULES_MAX);

    return 0;
}

int run_editor_form_teams(void) {
    MEScenarioForm f;
    int            i;
    int            j;

    meScenarioFormInit(&f);

    /* A team number runs 1 to MAX_TANKS - 1, so that many teams fill the
     * template and teams[] keeps its last slot spare. */
    for (i = 0; i < MAX_TANKS - 1; i++) {
        UT_ASSERT_MSG(meScenarioFormAddTeam(&f), "team %d refused below the bound",
                      i);
    }
    UT_ASSERT_MSG(f.manifest.lobby.numTeams == MAX_TANKS - 1, "numTeams is %u",
                  (unsigned)f.manifest.lobby.numTeams);
    UT_ASSERT(meScenarioFormDirty(&f));

    /* One more is refused rather than written off the end, and rather than
     * taking a number the validator would flag. */
    UT_ASSERT(!meScenarioFormAddTeam(&f));
    UT_ASSERT(f.manifest.lobby.numTeams == MAX_TANKS - 1);

    /* No two teams share a team number, every number is one the engine will
     * seat, and each team starts empty. */
    for (i = 0; i < (int)f.manifest.lobby.numTeams; i++) {
        const ScnManifestTeam *t = &f.manifest.lobby.teams[i];
        UT_ASSERT_MSG(t->id >= 1 && t->id <= MAX_TANKS - 1,
                      "team %d holds team number %u, outside 1 to %d", i,
                      (unsigned)t->id, MAX_TANKS - 1);
        UT_ASSERT(t->bots == 0);
        UT_ASSERT(t->maxBots == 0);
        UT_ASSERT(!t->fielded);
        UT_ASSERT(t->brain[0] == '\0');
        UT_ASSERT(t->init.count == 0);
        for (j = 0; j < i; j++) {
            UT_ASSERT_MSG(f.manifest.lobby.teams[j].id != t->id,
                          "teams %d and %d both hold team number %u", j, i,
                          (unsigned)t->id);
        }
    }

    /* Removing from the middle closes the list up over it. */
    {
        const uint8_t third = f.manifest.lobby.teams[2].id;
        const uint8_t fourth = f.manifest.lobby.teams[3].id;

        meScenarioFormRemoveTeam(&f, 2);
        UT_ASSERT_MSG(f.manifest.lobby.numTeams == MAX_TANKS - 2,
                      "numTeams is %u", (unsigned)f.manifest.lobby.numTeams);
        UT_ASSERT(f.manifest.lobby.teams[2].id == fourth);
        for (i = 0; i < (int)f.manifest.lobby.numTeams; i++) {
            UT_ASSERT(f.manifest.lobby.teams[i].id != third);
        }

        /* The number that was freed is the one the next team takes. */
        UT_ASSERT(meScenarioFormAddTeam(&f));
        UT_ASSERT(f.manifest.lobby.numTeams == MAX_TANKS - 1);
        UT_ASSERT_MSG(
            f.manifest.lobby.teams[MAX_TANKS - 2].id == third,
            "took team number %u, not %u",
            (unsigned)f.manifest.lobby.teams[MAX_TANKS - 2].id,
            (unsigned)third);
    }

    /* An index off either end changes nothing. */
    meScenarioFormRemoveTeam(&f, -1);
    meScenarioFormRemoveTeam(&f, MAX_TANKS);
    UT_ASSERT(f.manifest.lobby.numTeams == MAX_TANKS - 1);

    return 0;
}

int run_editor_form_rule_change(void) {
    SimRuleChange ch;
    const double  classic = simRulesClassicValue(SIM_RULE_tank_reload_ticks);

    /* The rules form asks this question for every row it draws, and turns the
     * answer into words with simRulesPhrase. The words are a gui thing and
     * langGetText is a stub here, so this asserts the answer rather than the
     * sentence: a reload of 2 against the classic 13 is faster, by 6.5.
     *
     * The classic value is asserted as well, because the multiple is only
     * 6.5 while that value is 13. */
    UT_ASSERT_MSG(classic == 13.0, "classic reload is %g", classic);

    ch = simRulesDescribeChange(SIM_RULE_tank_reload_ticks, 2.0);
    UT_ASSERT_MSG(ch.kind == SIM_RULE_CHANGE_FASTER, "kind is %d", (int)ch.kind);
    UT_ASSERT_MSG(fabs(ch.number - 6.5) < 0.0001, "multiple is %g", ch.number);

    return 0;
}
