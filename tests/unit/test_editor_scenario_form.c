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
 * run_editor_form_tag_indices — editor entity i is manifest entry i + 1, at
 *                               both ends of all three ranges, read out of the
 *                               manifest rather than back through the accessor
 * run_editor_form_tags        — the per-entity bound, a name longer than the
 *                               field, and a remove that keeps the rest packed
 * run_editor_form_regions     — add, edit and remove keeping the array packed,
 *                               the rectangle clamped on to the map, and the
 *                               SCN_REGIONS_MAX bound
 * run_editor_form_dirty_flag  — the flag the editor's unsaved-changes prompt
 *                               reads: clear on an empty form, set by a rule, a
 *                               team, a tag, a region and a trigger alike, and
 *                               clear again after the reset a map change does
 * run_editor_form_team_ids    — what the lobby form draws under a team's
 *                               number: nothing for the numbers Add Team hands
 *                               out, a duplicate against the later of two teams
 *                               sharing one, and out of range at either end
 * run_editor_form_triggers    — a trigger is made naming a hook and never
 *                               blank; a policy, an unknown name and an index
 *                               the table has not got are all refused; remove
 *                               closes the list up; each mutator marks the
 *                               form edited
 * run_editor_form_triggers_full — the list fills to its bound and refuses the
 *                               next trigger rather than overrunning
 * run_editor_form_trigger_rows — the tests and actions inside a trigger: each
 *                               fills to its own bound, a new one is made
 *                               against the hook rather than blank, remove
 *                               closes the list up, and every index off either
 *                               end is refused
 * run_editor_form_trigger_row_values — a whole test and a whole action written
 *                               back unchanged, the argument bound, and the
 *                               two names the form passes through for the
 *                               validator to report
 * run_editor_form_trigger_vocabulary — what the view draws a row from: the
 *                               seven operators, a hook's fields and an op's
 *                               arguments, each held against the catalogue the
 *                               accessor read it out of
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mapeditor_scenario_check.h"
#include "mapeditor_scenario_fndesc.h"
#include "mapeditor_scenario_form.h"
#include "scenario_host.h"
/* The catalogue itself, so the accessors the editor reaches it through can be
 * held against what they are reading. */
#include "scenario_lua.h"
#include "scenario_manifest_json.h"
#include "sim_rules_names.h"
#include "test_harness.h"

/* The nth hook the catalogue carries, counting from 0, and "" past the end.
 * The names are read out of the catalogue rather than written in here, so a
 * function renamed there does not leave a case naming one nothing carries. */
static const char *efHookAt(int nth) {
    const size_t count = meScnFnCount();
    size_t       row;
    int          seen = 0;

    for (row = 0; row < count; row++) {
        if (!meScnFnIsHook(row)) {
            continue;
        }
        if (seen == nth) {
            return meScnFnName(row);
        }
        seen++;
    }
    return "";
}

/* The first policy, which is a real name on the wrong half of the surface: the
 * host asks a policy a question and reads the answer, and a list of actions has
 * none to give it. */
static const char *efPolicy(void) {
    const size_t count = meScnFnCount();
    size_t       row;

    for (row = 0; row < count; row++) {
        if (!meScnFnIsHook(row)) {
            return meScnFnName(row);
        }
    }
    return "";
}

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

int run_editor_form_tag_indices(void) {
    static const struct {
        MEScenarioTagKind kind;
        const char       *what;
    } kKinds[] = {
        {ME_SCENARIO_TAG_PILL, "pill"},
        {ME_SCENARIO_TAG_BASE, "base"},
        {ME_SCENARIO_TAG_START, "start"},
    };
    MEScenarioForm f;
    int            k;

    meScenarioFormInit(&f);

    /* The conversion, at both ends of every kind's range. The editor counts
     * entities from 0 and the manifest's arrays are 1-based, so entity 0 is
     * entry 1 and the last entity a map can hold is the last entry the array
     * has. This is the off-by-one the whole accessor exists to hold in one
     * place, so the entries are read out of the manifest directly rather than
     * back through the accessor that wrote them. */
    for (k = 0; k < (int)(sizeof(kKinds) / sizeof(kKinds[0])); k++) {
        const MEScenarioTagKind kind = kKinds[k].kind;
        const int               cap  = meScenarioFormEntityCap(kind);
        const ScnManifestTags  *arr;

        UT_ASSERT_MSG(cap > 0, "%s has a cap of %d", kKinds[k].what, cap);

        UT_ASSERT(meScenarioFormAddTag(&f, kind, 0, "first"));
        UT_ASSERT(meScenarioFormAddTag(&f, kind, cap - 1, "last"));

        switch (kind) {
            case ME_SCENARIO_TAG_PILL:
                arr = f.manifest.pillTags;
                UT_ASSERT(cap == MAX_PILLS);
                break;
            case ME_SCENARIO_TAG_BASE:
                arr = f.manifest.baseTags;
                UT_ASSERT(cap == MAX_BASES);
                break;
            default:
                arr = f.manifest.startTags;
                UT_ASSERT(cap == MAX_STARTS);
                break;
        }

        /* Entry 0 is the unused one the file never writes. */
        UT_ASSERT_MSG(arr[0].count == 0,
                      "%s entry 0 holds %u tags and the file never writes it",
                      kKinds[k].what, (unsigned)arr[0].count);
        UT_ASSERT_MSG(arr[1].count == 1 &&
                          strcmp(arr[1].tag[0], "first") == 0,
                      "%s editor index 0 did not land in entry 1",
                      kKinds[k].what);
        UT_ASSERT_MSG(arr[cap].count == 1 &&
                          strcmp(arr[cap].tag[0], "last") == 0,
                      "%s editor index %d did not land in entry %d",
                      kKinds[k].what, cap - 1, cap);

        /* And the accessor reads back the entries it wrote. */
        UT_ASSERT(meScenarioFormTags(&f, kind, 0) == &arr[1]);
        UT_ASSERT(meScenarioFormTags(&f, kind, cap - 1) == &arr[cap]);

        /* An index off either end has no entry and writes nothing. */
        UT_ASSERT(meScenarioFormTags(&f, kind, -1) == NULL);
        UT_ASSERT(meScenarioFormTags(&f, kind, cap) == NULL);
        UT_ASSERT(!meScenarioFormAddTag(&f, kind, -1, "nowhere"));
        UT_ASSERT(!meScenarioFormAddTag(&f, kind, cap, "nowhere"));
    }

    UT_ASSERT(meScenarioFormDirty(&f));
    return 0;
}

int run_editor_form_tags(void) {
    char                   oversize[SCN_TAG_LEN * 2];
    MEScenarioForm         f;
    const ScnManifestTags *tags;
    int                    i;

    meScenarioFormInit(&f);

    /* An entity carries SCN_TAGS_PER_ENTITY of them and no more. */
    for (i = 0; i < SCN_TAGS_PER_ENTITY; i++) {
        char name[SCN_TAG_LEN];
        snprintf(name, sizeof(name), "tag%d", i);
        UT_ASSERT_MSG(meScenarioFormAddTag(&f, ME_SCENARIO_TAG_BASE, 3, name),
                      "tag %d refused below the bound", i);
    }
    tags = meScenarioFormTags(&f, ME_SCENARIO_TAG_BASE, 3);
    UT_ASSERT(tags != NULL);
    UT_ASSERT_MSG(tags->count == SCN_TAGS_PER_ENTITY, "count is %u",
                  (unsigned)tags->count);

    UT_ASSERT(!meScenarioFormAddTag(&f, ME_SCENARIO_TAG_BASE, 3, "fifth"));
    UT_ASSERT(tags->count == SCN_TAGS_PER_ENTITY);

    /* An empty name is not a tag. */
    UT_ASSERT(!meScenarioFormAddTag(&f, ME_SCENARIO_TAG_BASE, 4, ""));
    UT_ASSERT(meScenarioFormTags(&f, ME_SCENARIO_TAG_BASE, 4)->count == 0);

    /* Removing the first closes the rest up and leaves the count right. */
    meScenarioFormRemoveTag(&f, ME_SCENARIO_TAG_BASE, 3, 0);
    UT_ASSERT_MSG(tags->count == SCN_TAGS_PER_ENTITY - 1, "count is %u",
                  (unsigned)tags->count);
    for (i = 0; i < (int)tags->count; i++) {
        char want[SCN_TAG_LEN];
        snprintf(want, sizeof(want), "tag%d", i + 1);
        UT_ASSERT_MSG(strcmp(tags->tag[i], want) == 0,
                      "tag %d reads '%s', expected '%s'", i, tags->tag[i],
                      want);
    }
    /* The slot past the end is cleared rather than left holding the last
     * name twice. */
    UT_ASSERT(tags->tag[tags->count][0] == '\0');

    /* An index off either end changes nothing. */
    meScenarioFormRemoveTag(&f, ME_SCENARIO_TAG_BASE, 3, -1);
    meScenarioFormRemoveTag(&f, ME_SCENARIO_TAG_BASE, 3,
                            SCN_TAGS_PER_ENTITY);
    UT_ASSERT(tags->count == SCN_TAGS_PER_ENTITY - 1);

    /* A name longer than the field is cut to fit, not written past it. */
    memset(oversize, 'x', sizeof(oversize) - 1);
    oversize[sizeof(oversize) - 1] = '\0';
    UT_ASSERT(meScenarioFormAddTag(&f, ME_SCENARIO_TAG_START, 0, oversize));
    tags = meScenarioFormTags(&f, ME_SCENARIO_TAG_START, 0);
    UT_ASSERT(tags != NULL);
    UT_ASSERT_MSG(strlen(tags->tag[0]) == SCN_TAG_LEN - 1,
                  "the cut name is %u bytes, expected %d",
                  (unsigned)strlen(tags->tag[0]), SCN_TAG_LEN - 1);

    return 0;
}

int run_editor_form_regions(void) {
    MEScenarioForm f;
    int            i;

    meScenarioFormInit(&f);
    UT_ASSERT(meScenarioFormRegionCount(&f) == 0);

    /* An ordinary rectangle is stored as it was given. */
    UT_ASSERT(meScenarioFormAddRegion(&f, "keep", 10, 20, 8, 4));
    UT_ASSERT(meScenarioFormRegionCount(&f) == 1);
    UT_ASSERT(strcmp(f.manifest.regions[0].name, "keep") == 0);
    UT_ASSERT(f.manifest.regions[0].x == 10);
    UT_ASSERT(f.manifest.regions[0].y == 20);
    UT_ASSERT(f.manifest.regions[0].w == 8);
    UT_ASSERT(f.manifest.regions[0].h == 4);
    UT_ASSERT(meScenarioFormDirty(&f));

    /* A name with nothing in it is not a region. */
    UT_ASSERT(!meScenarioFormAddRegion(&f, "", 0, 0, 4, 4));
    UT_ASSERT(meScenarioFormRegionCount(&f) == 1);

    /* Editing one moves it. */
    UT_ASSERT(meScenarioFormSetRegionRect(&f, 0, 1, 2, 3, 4));
    UT_ASSERT(f.manifest.regions[0].x == 1 && f.manifest.regions[0].y == 2 &&
              f.manifest.regions[0].w == 3 && f.manifest.regions[0].h == 4);
    UT_ASSERT(!meScenarioFormSetRegionRect(&f, 1, 0, 0, 1, 1));
    UT_ASSERT(!meScenarioFormSetRegionRect(&f, -1, 0, 0, 1, 1));

    /* A rectangle that runs off the map is brought back on to it, and one with
     * no squares in it is given one: the validator refuses both. */
    UT_ASSERT(meScenarioFormAddRegion(&f, "corner", 250, 250, 40, 40));
    UT_ASSERT_MSG((int)f.manifest.regions[1].x +
                          (int)f.manifest.regions[1].w <= MAP_ARRAY_SIZE,
                  "the region runs to %d and a map is %d square",
                  (int)f.manifest.regions[1].x + (int)f.manifest.regions[1].w,
                  MAP_ARRAY_SIZE);
    UT_ASSERT((int)f.manifest.regions[1].y + (int)f.manifest.regions[1].h <=
              MAP_ARRAY_SIZE);
    UT_ASSERT(f.manifest.regions[1].w > 0 && f.manifest.regions[1].h > 0);

    UT_ASSERT(meScenarioFormAddRegion(&f, "empty", 5, 5, 0, -3));
    UT_ASSERT_MSG(f.manifest.regions[2].w == 1 &&
                      f.manifest.regions[2].h == 1,
                  "an empty rectangle came out %u by %u",
                  (unsigned)f.manifest.regions[2].w,
                  (unsigned)f.manifest.regions[2].h);

    /* A negative corner is pulled back on to the map too. */
    UT_ASSERT(meScenarioFormSetRegionRect(&f, 2, -4, -9, 6, 6));
    UT_ASSERT(f.manifest.regions[2].x == 0 && f.manifest.regions[2].y == 0);

    /* Removing from the middle closes the list up over it. */
    meScenarioFormRemoveRegion(&f, 1);
    UT_ASSERT_MSG(meScenarioFormRegionCount(&f) == 2, "%d regions left",
                  meScenarioFormRegionCount(&f));
    UT_ASSERT(strcmp(f.manifest.regions[0].name, "keep") == 0);
    UT_ASSERT(strcmp(f.manifest.regions[1].name, "empty") == 0);
    UT_ASSERT(f.manifest.regions[2].name[0] == '\0');

    /* An index off either end changes nothing. */
    meScenarioFormRemoveRegion(&f, -1);
    meScenarioFormRemoveRegion(&f, 2);
    UT_ASSERT(meScenarioFormRegionCount(&f) == 2);

    /* The list fills to its bound and refuses the next one. */
    for (i = meScenarioFormRegionCount(&f); i < SCN_REGIONS_MAX; i++) {
        char name[SCN_REGION_NAME_LEN];
        snprintf(name, sizeof(name), "r%d", i);
        UT_ASSERT_MSG(meScenarioFormAddRegion(&f, name, 0, 0, 2, 2),
                      "region %d refused below the bound", i);
    }
    UT_ASSERT(meScenarioFormRegionCount(&f) == SCN_REGIONS_MAX);
    UT_ASSERT(!meScenarioFormAddRegion(&f, "one too many", 0, 0, 2, 2));
    UT_ASSERT(meScenarioFormRegionCount(&f) == SCN_REGIONS_MAX);

    /* And what Save as Mod says it will leave out counts both. */
    UT_ASSERT(meScenarioFormTagCount(&f) == 0);
    UT_ASSERT(meScenarioFormAddTag(&f, ME_SCENARIO_TAG_PILL, 0, "a"));
    UT_ASSERT(meScenarioFormAddTag(&f, ME_SCENARIO_TAG_PILL, 0, "b"));
    UT_ASSERT(meScenarioFormAddTag(&f, ME_SCENARIO_TAG_START, 2, "c"));
    UT_ASSERT_MSG(meScenarioFormTagCount(&f) == 3, "%d tags counted",
                  meScenarioFormTagCount(&f));

    return 0;
}

int run_editor_form_dirty_flag(void) {
    MEScenarioForm f;

    /* The editor asks this flag whether a New, an Open or an Exit would throw
     * the forms away, so an empty form has to answer no and every kind of edit
     * has to answer yes. Each one is checked from a form of its own, so a set
     * flag is the edit above it and not one left over. */
    meScenarioFormInit(&f);
    UT_ASSERT(!meScenarioFormDirty(&f));

    /* A rule, from the rules form. */
    UT_ASSERT(meScenarioFormSetRule(&f, SIM_RULE_tank_reload_ticks, 2.0));
    UT_ASSERT(meScenarioFormDirty(&f));

    /* The reset a map change does puts it back. */
    meScenarioFormReset(&f);
    UT_ASSERT(!meScenarioFormDirty(&f));

    /* A team, from the lobby form. */
    UT_ASSERT(meScenarioFormAddTeam(&f));
    UT_ASSERT(meScenarioFormDirty(&f));

    meScenarioFormReset(&f);
    UT_ASSERT(!meScenarioFormDirty(&f));

    /* A tag, from the tags view. */
    UT_ASSERT(meScenarioFormAddTag(&f, ME_SCENARIO_TAG_PILL, 0, "spawn"));
    UT_ASSERT(meScenarioFormDirty(&f));

    meScenarioFormReset(&f);
    UT_ASSERT(!meScenarioFormDirty(&f));

    /* And a region, from the same view. */
    UT_ASSERT(meScenarioFormAddRegion(&f, "north", 10, 10, 4, 4));
    UT_ASSERT(meScenarioFormDirty(&f));

    meScenarioFormReset(&f);
    UT_ASSERT(!meScenarioFormDirty(&f));

    /* A trigger, from the triggers view. */
    UT_ASSERT(meScenarioFormAddTrigger(&f, efHookAt(0)));
    UT_ASSERT(meScenarioFormDirty(&f));

    meScenarioFormReset(&f);
    UT_ASSERT(!meScenarioFormDirty(&f));

    return 0;
}

int run_editor_form_team_ids(void) {
    MEScenarioForm f;
    uint8_t        first;

    /* The lobby form's id field takes any byte, and the validator refuses
     * three of them when the scenario is packed: 0, MAX_TANKS and above, and a
     * number another team already holds. This is what the form says under the
     * field so the author reads it while typing rather than at the pack. */
    meScenarioFormInit(&f);

    UT_ASSERT(meScenarioFormAddTeam(&f));
    UT_ASSERT(meScenarioFormAddTeam(&f));
    UT_ASSERT_MSG(f.manifest.lobby.numTeams == 2, "numTeams is %u",
                  (unsigned)f.manifest.lobby.numTeams);

    /* Add Team hands out numbers in range that nobody else holds, so neither
     * team is reported. */
    UT_ASSERT(meScenarioFormTeamIdProblem(&f, 0) == ME_SCENARIO_TEAM_ID_OK);
    UT_ASSERT(meScenarioFormTeamIdProblem(&f, 1) == ME_SCENARIO_TEAM_ID_OK);

    /* The second team typed on to the first's number. The report goes against
     * the later of the two, the way scenario_validate.c reports it, so the
     * first still reads clean and one shared number draws one hint. */
    first                        = f.manifest.lobby.teams[0].id;
    f.manifest.lobby.teams[1].id = first;
    UT_ASSERT(meScenarioFormTeamIdProblem(&f, 1) == ME_SCENARIO_TEAM_ID_TAKEN);
    UT_ASSERT(meScenarioFormTeamIdProblem(&f, 0) == ME_SCENARIO_TEAM_ID_OK);

    /* Either end of the range a game seats. */
    f.manifest.lobby.teams[1].id = 0;
    UT_ASSERT(meScenarioFormTeamIdProblem(&f, 1) == ME_SCENARIO_TEAM_ID_RANGE);
    f.manifest.lobby.teams[1].id = MAX_TANKS;
    UT_ASSERT(meScenarioFormTeamIdProblem(&f, 1) == ME_SCENARIO_TEAM_ID_RANGE);

    /* And back on to a number in range that the other team does not hold. Add
     * Team seats the first team as team 1, so one above it is both. */
    f.manifest.lobby.teams[1].id = (uint8_t)(first + 1);
    UT_ASSERT_MSG(meScenarioFormTeamIdProblem(&f, 1) == ME_SCENARIO_TEAM_ID_OK,
                  "team number %u reported",
                  (unsigned)f.manifest.lobby.teams[1].id);

    /* A row the template does not seat has nothing said about it. */
    UT_ASSERT(meScenarioFormTeamIdProblem(&f, 2) == ME_SCENARIO_TEAM_ID_OK);
    UT_ASSERT(meScenarioFormTeamIdProblem(&f, -1) == ME_SCENARIO_TEAM_ID_OK);

    return 0;
}

int run_editor_form_triggers(void) {
    char           oversize[SCN_TRIGGER_NAME_LEN * 2];
    MEScenarioForm f;
    const char    *first  = efHookAt(0);
    const char    *second = efHookAt(1);
    const char    *third  = efHookAt(2);
    const char    *policy = efPolicy();
    size_t         row;

    /* The two halves of the catalogue are a partition, and the side a row is
     * on is the side its kind puts it on: a policy is the row that says what
     * it answers, a hook the row that answers nothing. That is what makes the
     * view's combo offerable — it lists the hooks and leaves the policies out
     * — and what the refusals below rest on. */
    for (row = 0; row < meScnFnCount(); row++) {
        UT_ASSERT_MSG(meScnFnIsHook(row) == (meScnFnReturns(row)[0] == '\0'),
                      "row %d is %s and answers '%s'", (int)row,
                      meScnFnIsHook(row) ? "a hook" : "a policy",
                      meScnFnReturns(row));
    }
    UT_ASSERT(!meScnFnIsHook(meScnFnCount()));

    UT_ASSERT_MSG(first[0] != '\0' && second[0] != '\0' && third[0] != '\0',
                  "the catalogue carries fewer than three hooks");
    UT_ASSERT_MSG(policy[0] != '\0', "the catalogue carries no policy");
    UT_ASSERT(strcmp(first, second) != 0 && strcmp(second, third) != 0);

    meScenarioFormInit(&f);
    UT_ASSERT(meScenarioFormTriggerCount(&f) == 0);

    /* A trigger is created already naming the hook it was given, never blank:
     * the validator refuses one that names no hook. */
    UT_ASSERT(meScenarioFormAddTrigger(&f, first));
    UT_ASSERT_MSG(meScenarioFormTriggerCount(&f) == 1, "%d triggers",
                  meScenarioFormTriggerCount(&f));
    UT_ASSERT_MSG(strcmp(f.manifest.triggers[0].when, first) == 0,
                  "the trigger runs on '%s', not '%s'",
                  f.manifest.triggers[0].when, first);
    UT_ASSERT(f.manifest.triggers[0].numWhere == 0);
    UT_ASSERT(f.manifest.triggers[0].numActions == 0);
    UT_ASSERT(meScenarioFormDirty(&f));

    /* A policy is a name the catalogue carries and a trigger cannot run on. */
    UT_ASSERT(!meScenarioFormAddTrigger(&f, policy));
    /* And these are no hook the catalogue has: one nothing is called, nothing
     * at all, and one too long for the field to hold whole. */
    UT_ASSERT(!meScenarioFormAddTrigger(&f, "on_nothing_at_all"));
    UT_ASSERT(!meScenarioFormAddTrigger(&f, ""));
    UT_ASSERT(!meScenarioFormAddTrigger(&f, NULL));
    memset(oversize, 'x', sizeof(oversize) - 1);
    oversize[sizeof(oversize) - 1] = '\0';
    UT_ASSERT(!meScenarioFormAddTrigger(&f, oversize));
    UT_ASSERT_MSG(meScenarioFormTriggerCount(&f) == 1, "%d triggers",
                  meScenarioFormTriggerCount(&f));

    /* Two more, each on a hook of its own, so a removal has something to close
     * up over and the survivors can be told apart. */
    UT_ASSERT(meScenarioFormAddTrigger(&f, second));
    UT_ASSERT(meScenarioFormAddTrigger(&f, third));
    UT_ASSERT(meScenarioFormTriggerCount(&f) == 3);

    /* What the middle one carries, so the close-up is checked over the whole
     * row rather than over its hook name alone. */
    f.manifest.triggers[1].numWhere   = SCN_TRIGGER_CONDS_MAX;
    f.manifest.triggers[1].numActions = SCN_TRIGGER_ACTIONS_MAX;

    /* Which hook a trigger runs on is the author's to change. */
    f.dirty = false;
    UT_ASSERT(meScenarioFormSetTriggerWhen(&f, 0, third));
    UT_ASSERT(strcmp(f.manifest.triggers[0].when, third) == 0);
    UT_ASSERT(meScenarioFormDirty(&f));
    UT_ASSERT(meScenarioFormSetTriggerWhen(&f, 0, first));

    /* An index the table has not got, and every name Add refuses, leave the
     * trigger running on what it ran on before. */
    UT_ASSERT(!meScenarioFormSetTriggerWhen(&f, -1, second));
    UT_ASSERT(!meScenarioFormSetTriggerWhen(&f, 3, second));
    UT_ASSERT(!meScenarioFormSetTriggerWhen(&f, 0, policy));
    UT_ASSERT(!meScenarioFormSetTriggerWhen(&f, 0, "on_nothing_at_all"));
    UT_ASSERT(!meScenarioFormSetTriggerWhen(&f, 0, ""));
    UT_ASSERT(!meScenarioFormSetTriggerWhen(&f, 0, NULL));
    UT_ASSERT(!meScenarioFormSetTriggerWhen(&f, 0, oversize));
    UT_ASSERT_MSG(strcmp(f.manifest.triggers[0].when, first) == 0,
                  "a refused name left the trigger on '%s'",
                  f.manifest.triggers[0].when);

    /* Removing the first closes the other two up over it, whole. */
    f.dirty = false;
    meScenarioFormRemoveTrigger(&f, 0);
    UT_ASSERT_MSG(meScenarioFormTriggerCount(&f) == 2, "%d triggers left",
                  meScenarioFormTriggerCount(&f));
    UT_ASSERT(meScenarioFormDirty(&f));
    UT_ASSERT_MSG(strcmp(f.manifest.triggers[0].when, second) == 0,
                  "the survivor runs on '%s', not '%s'",
                  f.manifest.triggers[0].when, second);
    UT_ASSERT(f.manifest.triggers[0].numWhere == SCN_TRIGGER_CONDS_MAX);
    UT_ASSERT(f.manifest.triggers[0].numActions == SCN_TRIGGER_ACTIONS_MAX);
    UT_ASSERT(strcmp(f.manifest.triggers[1].when, third) == 0);

    /* The slot past the end is cleared rather than left holding the last row
     * twice. */
    UT_ASSERT(f.manifest.triggers[2].when[0] == '\0');
    UT_ASSERT(f.manifest.triggers[2].numWhere == 0);
    UT_ASSERT(f.manifest.triggers[2].numActions == 0);

    /* An index off either end changes nothing. */
    meScenarioFormRemoveTrigger(&f, -1);
    meScenarioFormRemoveTrigger(&f, 2);
    UT_ASSERT(meScenarioFormTriggerCount(&f) == 2);

    /* And a form with no triggers answers for one nobody added. */
    UT_ASSERT(meScenarioFormTriggerCount(NULL) == 0);
    UT_ASSERT(!meScenarioFormAddTrigger(NULL, first));
    UT_ASSERT(!meScenarioFormSetTriggerWhen(NULL, 0, first));

    return 0;
}

int run_editor_form_triggers_full(void) {
    MEScenarioForm f;
    const char    *hook  = efHookAt(0);
    const char    *other = efHookAt(1);
    int            i;

    UT_ASSERT(hook[0] != '\0' && other[0] != '\0');

    meScenarioFormInit(&f);

    for (i = 0; i < SCN_TRIGGERS_MAX; i++) {
        UT_ASSERT_MSG(meScenarioFormAddTrigger(&f, hook),
                      "trigger %d refused below the bound", i);
    }
    UT_ASSERT_MSG(meScenarioFormTriggerCount(&f) == SCN_TRIGGERS_MAX,
                  "%d triggers", meScenarioFormTriggerCount(&f));

    /* One more is refused, and the full list is left as it was. */
    UT_ASSERT(!meScenarioFormAddTrigger(&f, hook));
    UT_ASSERT(meScenarioFormTriggerCount(&f) == SCN_TRIGGERS_MAX);

    /* A trigger already in the full list still changes hook, because that
     * writes no new row. */
    UT_ASSERT(meScenarioFormSetTriggerWhen(&f, SCN_TRIGGERS_MAX - 1, other));
    UT_ASSERT(meScenarioFormTriggerCount(&f) == SCN_TRIGGERS_MAX);
    UT_ASSERT(strcmp(f.manifest.triggers[SCN_TRIGGERS_MAX - 1].when, other) ==
              0);

    /* And with one row gone there is room again. */
    meScenarioFormRemoveTrigger(&f, 0);
    UT_ASSERT(meScenarioFormTriggerCount(&f) == SCN_TRIGGERS_MAX - 1);
    UT_ASSERT(meScenarioFormAddTrigger(&f, hook));
    UT_ASSERT(meScenarioFormTriggerCount(&f) == SCN_TRIGGERS_MAX);

    return 0;
}

/* The nth hook the catalogue carries that is handed something, counting from
 * 0, and "" past the end. A test names a field of the hook's payload, so the
 * rows below are written on a hook that has one. */
static const char *efHookWithFields(int nth) {
    const size_t count = meScnFnCount();
    size_t       row;
    int          seen = 0;

    for (row = 0; row < count; row++) {
        if (!meScnFnIsHook(row) || meScnFnFieldCount(row) == 0) {
            continue;
        }
        if (seen == nth) {
            return meScnFnName(row);
        }
        seen++;
    }
    return "";
}

/* A hook the host hands nothing at all, and "" if every one of them carries
 * something. on_setup, on_start and on_end are called with no payload, so a
 * trigger on one of them has nothing to test. */
static const char *efHookWithoutFields(void) {
    const size_t count = meScnFnCount();
    size_t       row;

    for (row = 0; row < count; row++) {
        if (meScnFnIsHook(row) && meScnFnFieldCount(row) == 0) {
            return meScnFnName(row);
        }
    }
    return "";
}

/* The first field of a hook, as the catalogue spells it. */
static void efFirstField(const char *hook, char *out, size_t outLen) {
    const size_t count = meScnFnCount();
    size_t       row;

    out[0] = '\0';
    for (row = 0; row < count; row++) {
        if (strcmp(meScnFnName(row), hook) == 0) {
            (void)meScnFnFieldAt(row, 0, out, outLen, NULL, NULL);
            return;
        }
    }
}

int run_editor_form_trigger_rows(void) {
    char           field[ME_SCN_FIELD_NAME_LEN];
    MEScenarioForm f;
    const char    *hook = efHookWithFields(0);
    const char    *bare = efHookWithoutFields();
    ScnTrigCond    cond;
    ScnTrigAct     act;
    int            i;

    UT_ASSERT_MSG(hook[0] != '\0', "no hook the catalogue carries has a field");
    efFirstField(hook, field, sizeof(field));
    UT_ASSERT(field[0] != '\0');

    meScenarioFormInit(&f);

    /* Nothing answers for a trigger the table has not got, and nothing is
     * written into one either. */
    UT_ASSERT(meScenarioFormCondCount(&f, 0) == 0);
    UT_ASSERT(meScenarioFormActionCount(&f, 0) == 0);
    UT_ASSERT(!meScenarioFormAddCond(&f, 0));
    UT_ASSERT(!meScenarioFormAddAction(&f, 0));
    memset(&cond, 0, sizeof(cond));
    memset(&act, 0, sizeof(act));
    UT_ASSERT(!meScenarioFormSetCond(&f, 0, 0, &cond));
    UT_ASSERT(!meScenarioFormSetAction(&f, 0, 0, &act));
    meScenarioFormRemoveCond(&f, 0, 0);
    meScenarioFormRemoveAction(&f, 0, 0);
    UT_ASSERT(!meScenarioFormDirty(&f));

    /* A form nobody passed answers the same way. */
    UT_ASSERT(meScenarioFormCondCount(NULL, 0) == 0);
    UT_ASSERT(meScenarioFormActionCount(NULL, 0) == 0);
    UT_ASSERT(!meScenarioFormAddCond(NULL, 0));
    UT_ASSERT(!meScenarioFormAddAction(NULL, 0));
    UT_ASSERT(!meScenarioFormSetCond(NULL, 0, 0, &cond));
    UT_ASSERT(!meScenarioFormSetAction(NULL, 0, 0, &act));
    meScenarioFormRemoveCond(NULL, 0, 0);
    meScenarioFormRemoveAction(NULL, 0, 0);

    UT_ASSERT(meScenarioFormAddTrigger(&f, hook));

    /* The tests, up to the bound. A new one is made against the trigger's own
     * hook and is never blank. */
    for (i = 0; i < SCN_TRIGGER_CONDS_MAX; i++) {
        f.dirty = false;
        UT_ASSERT_MSG(meScenarioFormAddCond(&f, 0),
                      "test %d refused below the bound", i);
        UT_ASSERT(meScenarioFormDirty(&f));
        UT_ASSERT_MSG(strcmp(f.manifest.triggers[0].where[i].field, field) == 0,
                      "test %d names '%s', not '%s'", i,
                      f.manifest.triggers[0].where[i].field, field);
        UT_ASSERT(f.manifest.triggers[0].where[i].op != SCN_TRIG_CMP_UNKNOWN);
    }
    UT_ASSERT(meScenarioFormCondCount(&f, 0) == SCN_TRIGGER_CONDS_MAX);

    /* One more is refused and the full list is left as it was. */
    UT_ASSERT(!meScenarioFormAddCond(&f, 0));
    UT_ASSERT(meScenarioFormCondCount(&f, 0) == SCN_TRIGGER_CONDS_MAX);

    /* Each one marked apart, so what a removal closes up over can be told
     * from what it left. */
    for (i = 0; i < SCN_TRIGGER_CONDS_MAX; i++) {
        cond            = f.manifest.triggers[0].where[i];
        cond.value.kind = SCN_TRIG_VAL_NUMBER;
        cond.value.num  = (double)i;
        f.dirty         = false;
        UT_ASSERT(meScenarioFormSetCond(&f, 0, i, &cond));
        UT_ASSERT(meScenarioFormDirty(&f));
    }

    /* A row the trigger has not got, at either end. */
    UT_ASSERT(!meScenarioFormSetCond(&f, 0, -1, &cond));
    UT_ASSERT(!meScenarioFormSetCond(&f, 0, SCN_TRIGGER_CONDS_MAX, &cond));
    UT_ASSERT(!meScenarioFormSetCond(&f, 0, 0, NULL));

    /* Removing the second closes the rest up over it, in order and whole. */
    f.dirty = false;
    meScenarioFormRemoveCond(&f, 0, 1);
    UT_ASSERT(meScenarioFormDirty(&f));
    UT_ASSERT_MSG(meScenarioFormCondCount(&f, 0) == SCN_TRIGGER_CONDS_MAX - 1,
                  "%d tests left", meScenarioFormCondCount(&f, 0));
    UT_ASSERT(f.manifest.triggers[0].where[0].value.num == 0.0);
    UT_ASSERT_MSG(f.manifest.triggers[0].where[1].value.num == 2.0,
                  "the survivor carries %f",
                  f.manifest.triggers[0].where[1].value.num);
    UT_ASSERT(f.manifest.triggers[0].where[2].value.num == 3.0);
    UT_ASSERT(strcmp(f.manifest.triggers[0].where[1].field, field) == 0);

    /* The slot past the end is cleared rather than left holding the last row
     * twice. */
    UT_ASSERT(f.manifest.triggers[0]
                  .where[SCN_TRIGGER_CONDS_MAX - 1]
                  .field[0] == '\0');

    /* An index off either end changes nothing. */
    meScenarioFormRemoveCond(&f, 0, -1);
    meScenarioFormRemoveCond(&f, 0, SCN_TRIGGER_CONDS_MAX - 1);
    UT_ASSERT(meScenarioFormCondCount(&f, 0) == SCN_TRIGGER_CONDS_MAX - 1);

    /* And with one gone there is room for another. */
    UT_ASSERT(meScenarioFormAddCond(&f, 0));
    UT_ASSERT(meScenarioFormCondCount(&f, 0) == SCN_TRIGGER_CONDS_MAX);

    /* The actions, the same way. A new one names an op rather than nothing. */
    for (i = 0; i < SCN_TRIGGER_ACTIONS_MAX; i++) {
        f.dirty = false;
        UT_ASSERT_MSG(meScenarioFormAddAction(&f, 0),
                      "action %d refused below the bound", i);
        UT_ASSERT(meScenarioFormDirty(&f));
        UT_ASSERT_MSG(f.manifest.triggers[0].actions[i].op[0] != '\0',
                      "action %d names no op", i);
        UT_ASSERT(f.manifest.triggers[0].actions[i].numArgs == 0);
    }
    UT_ASSERT(meScenarioFormActionCount(&f, 0) == SCN_TRIGGER_ACTIONS_MAX);
    UT_ASSERT(!meScenarioFormAddAction(&f, 0));
    UT_ASSERT(meScenarioFormActionCount(&f, 0) == SCN_TRIGGER_ACTIONS_MAX);

    for (i = 0; i < SCN_TRIGGER_ACTIONS_MAX; i++) {
        act = f.manifest.triggers[0].actions[i];
        snprintf(act.text, sizeof(act.text), "action %d", i);
        f.dirty = false;
        UT_ASSERT(meScenarioFormSetAction(&f, 0, i, &act));
        UT_ASSERT(meScenarioFormDirty(&f));
    }

    UT_ASSERT(!meScenarioFormSetAction(&f, 0, -1, &act));
    UT_ASSERT(!meScenarioFormSetAction(&f, 0, SCN_TRIGGER_ACTIONS_MAX, &act));
    UT_ASSERT(!meScenarioFormSetAction(&f, 0, 0, NULL));

    f.dirty = false;
    meScenarioFormRemoveAction(&f, 0, 1);
    UT_ASSERT(meScenarioFormDirty(&f));
    UT_ASSERT(meScenarioFormActionCount(&f, 0) == SCN_TRIGGER_ACTIONS_MAX - 1);
    UT_ASSERT(strcmp(f.manifest.triggers[0].actions[0].text, "action 0") == 0);
    UT_ASSERT_MSG(
        strcmp(f.manifest.triggers[0].actions[1].text, "action 2") == 0,
        "the survivor carries '%s'", f.manifest.triggers[0].actions[1].text);
    UT_ASSERT(strcmp(f.manifest.triggers[0].actions[2].text, "action 3") == 0);
    UT_ASSERT(
        f.manifest.triggers[0].actions[SCN_TRIGGER_ACTIONS_MAX - 1].op[0] ==
        '\0');

    meScenarioFormRemoveAction(&f, 0, -1);
    meScenarioFormRemoveAction(&f, 0, SCN_TRIGGER_ACTIONS_MAX - 1);
    UT_ASSERT(meScenarioFormActionCount(&f, 0) == SCN_TRIGGER_ACTIONS_MAX - 1);

    /* A trigger the table has not got, at either end, over all eight. */
    UT_ASSERT(meScenarioFormCondCount(&f, 1) == 0);
    UT_ASSERT(meScenarioFormActionCount(&f, -1) == 0);
    UT_ASSERT(!meScenarioFormAddCond(&f, 1));
    UT_ASSERT(!meScenarioFormAddAction(&f, 1));
    UT_ASSERT(!meScenarioFormSetCond(&f, 1, 0, &cond));
    UT_ASSERT(!meScenarioFormSetAction(&f, 1, 0, &act));
    f.dirty = false;
    meScenarioFormRemoveCond(&f, 1, 0);
    meScenarioFormRemoveAction(&f, -1, 0);
    UT_ASSERT(!meScenarioFormDirty(&f));

    /* A trigger on a hook the host hands nothing has no test to write, and
     * still has actions to take. */
    if (bare[0] != '\0') {
        UT_ASSERT(meScenarioFormAddTrigger(&f, bare));
        UT_ASSERT_MSG(!meScenarioFormAddCond(&f, 1),
                      "a test was written against '%s', which carries none",
                      bare);
        UT_ASSERT(meScenarioFormCondCount(&f, 1) == 0);
        UT_ASSERT(meScenarioFormAddAction(&f, 1));
        UT_ASSERT(meScenarioFormActionCount(&f, 1) == 1);
    }

    return 0;
}

int run_editor_form_trigger_row_values(void) {
    char           field[ME_SCN_FIELD_NAME_LEN];
    MEScenarioForm f;
    const char    *hook = efHookWithFields(0);
    ScnTrigCond    cond;
    ScnTrigAct     act;
    int            i;

    UT_ASSERT(hook[0] != '\0');
    efFirstField(hook, field, sizeof(field));

    meScenarioFormInit(&f);
    UT_ASSERT(meScenarioFormAddTrigger(&f, hook));
    UT_ASSERT(meScenarioFormAddCond(&f, 0));
    UT_ASSERT(meScenarioFormAddAction(&f, 0));

    /* A whole test written over the made one, a reference on its right rather
     * than a literal: the form takes the row it is handed and keeps it. */
    memset(&cond, 0, sizeof(cond));
    snprintf(cond.field, sizeof(cond.field), "%s", field);
    cond.op         = SCN_TRIG_CMP_NE;
    cond.value.kind = SCN_TRIG_VAL_FIELD;
    snprintf(cond.value.text, sizeof(cond.value.text), "some_other_field");
    UT_ASSERT(meScenarioFormSetCond(&f, 0, 0, &cond));
    UT_ASSERT(strcmp(f.manifest.triggers[0].where[0].field, field) == 0);
    UT_ASSERT(f.manifest.triggers[0].where[0].op == SCN_TRIG_CMP_NE);
    UT_ASSERT(f.manifest.triggers[0].where[0].value.kind ==
              SCN_TRIG_VAL_FIELD);
    UT_ASSERT_MSG(strcmp(f.manifest.triggers[0].where[0].value.text,
                         "some_other_field") == 0,
                  "the value names '%s'",
                  f.manifest.triggers[0].where[0].value.text);

    /* An action whole, text and arguments and all, filled to the bound. */
    memset(&act, 0, sizeof(act));
    snprintf(act.op, sizeof(act.op), "%s", ME_SCENARIO_CALL_OP);
    snprintf(act.text, sizeof(act.text), "a line the action carries");
    act.numArgs = SCN_TRIGGER_ARGS_MAX;
    for (i = 0; i < SCN_TRIGGER_ARGS_MAX; i++) {
        act.args[i].kind = SCN_TRIG_VAL_NUMBER;
        act.args[i].num  = (double)(i + 1);
    }
    act.args[0].kind = SCN_TRIG_VAL_STRING;
    snprintf(act.args[0].text, sizeof(act.args[0].text), "a_function");
    act.args[1].kind = SCN_TRIG_VAL_FIELD;
    snprintf(act.args[1].text, sizeof(act.args[1].text), "%s", field);
    UT_ASSERT(meScenarioFormSetAction(&f, 0, 0, &act));
    UT_ASSERT(strcmp(f.manifest.triggers[0].actions[0].op,
                     ME_SCENARIO_CALL_OP) == 0);
    UT_ASSERT_MSG(strcmp(f.manifest.triggers[0].actions[0].text,
                         "a line the action carries") == 0,
                  "the action carries '%s'",
                  f.manifest.triggers[0].actions[0].text);
    UT_ASSERT(f.manifest.triggers[0].actions[0].numArgs ==
              SCN_TRIGGER_ARGS_MAX);
    UT_ASSERT(strcmp(f.manifest.triggers[0].actions[0].args[0].text,
                     "a_function") == 0);
    UT_ASSERT(f.manifest.triggers[0].actions[0].args[1].kind ==
              SCN_TRIG_VAL_FIELD);
    UT_ASSERT(strcmp(f.manifest.triggers[0].actions[0].args[1].text, field) ==
              0);
    for (i = 2; i < SCN_TRIGGER_ARGS_MAX; i++) {
        UT_ASSERT_MSG(f.manifest.triggers[0].actions[0].args[i].num ==
                          (double)(i + 1),
                      "argument %d came back as %f", i,
                      f.manifest.triggers[0].actions[0].args[i].num);
    }

    /* One argument more than there is room for is refused, and the action is
     * left as it was. */
    act.numArgs = SCN_TRIGGER_ARGS_MAX + 1;
    UT_ASSERT(!meScenarioFormSetAction(&f, 0, 0, &act));
    UT_ASSERT(f.manifest.triggers[0].actions[0].numArgs ==
              SCN_TRIGGER_ARGS_MAX);

    /* And what the form does not refuse: a field that is no field of the hook
     * and an op the game table has not got. Both are scnCheckTriggers' to
     * report, in a sentence the author reads in the issues list, so a change
     * that moves either refusal here fails this. */
    memset(&cond, 0, sizeof(cond));
    snprintf(cond.field, sizeof(cond.field), "no_field_of_any_hook");
    cond.op = SCN_TRIG_CMP_EQ;
    UT_ASSERT_MSG(meScenarioFormSetCond(&f, 0, 0, &cond),
                  "the form refused a field name the validator reports");
    UT_ASSERT(strcmp(f.manifest.triggers[0].where[0].field,
                     "no_field_of_any_hook") == 0);

    memset(&act, 0, sizeof(act));
    snprintf(act.op, sizeof(act.op), "no_op_the_table_carries");
    UT_ASSERT_MSG(meScenarioFormSetAction(&f, 0, 0, &act),
                  "the form refused an op name the validator reports");
    UT_ASSERT(strcmp(f.manifest.triggers[0].actions[0].op,
                     "no_op_the_table_carries") == 0);

    return 0;
}

int run_editor_form_trigger_vocabulary(void) {
    const ScnLuaRow *rows;
    char             name[ME_SCN_FIELD_NAME_LEN];
    size_t           opCount     = 0;
    size_t           row;
    int              i;
    int              j;
    int              scalars     = 0;
    int              nonScalars  = 0;
    int              derivedSeen = 0;
    int              setValued   = 0;

    /* The seven operators, and not the eighth: a row that named none is left
     * as SCN_TRIG_CMP_UNKNOWN, which is no choice to offer. */
    UT_ASSERT_MSG(meScenarioFormCompareCount() == 7, "%d operators",
                  meScenarioFormCompareCount());
    for (i = 0; i < meScenarioFormCompareCount(); i++) {
        const ScnTrigCompare op   = meScenarioFormCompareAt(i);
        const char          *word = scnManifestTrigOpName(op);

        UT_ASSERT(op != SCN_TRIG_CMP_UNKNOWN);
        /* The word is the readers' own, and it reads back as the operator it
           was written for. */
        UT_ASSERT_MSG(word != NULL && word[0] != '\0', "operator %d has no "
                                                       "word", i);
        UT_ASSERT_MSG(strcmp(meScenarioFormCompareName(op), word) == 0,
                      "operator %d is '%s', not '%s'", i,
                      meScenarioFormCompareName(op), word);
        UT_ASSERT(scnManifestTrigOpFrom(word) == op);
        for (j = 0; j < i; j++) {
            UT_ASSERT(meScenarioFormCompareAt(j) != op);
        }
    }
    UT_ASSERT(meScenarioFormCompareAt(-1) == SCN_TRIG_CMP_UNKNOWN);
    UT_ASSERT(meScenarioFormCompareAt(meScenarioFormCompareCount()) ==
              SCN_TRIG_CMP_UNKNOWN);
    UT_ASSERT(meScenarioFormCompareName(SCN_TRIG_CMP_UNKNOWN)[0] == '\0');

    /* The fields, held against the catalogue row by row: the editor's answer
     * is the catalogue's answer, name for name and type for type. A type the
     * catalogue has grown and this build has no name for lands as NONE, which
     * no real field is, so this is where that drift is caught. */
    for (row = 0; row < meScnFnCount(); row++) {
        /* As many as any row reaches, with room over — the same bound the
         * editor's own copy of a row's fields is held to. A row that outgrew
         * it is the first assert below. */
        ScnLuaFnField fields[16];
        const size_t  count = scenarioLuaFnFields(row, fields,
                                                  sizeof(fields) /
                                                      sizeof(fields[0]));
        size_t        at;

        UT_ASSERT_MSG(count <= sizeof(fields) / sizeof(fields[0]),
                      "row %d carries %d fields", (int)row, (int)count);
        UT_ASSERT_MSG(meScnFnFieldCount(row) == count, "row %d: %d against %d",
                      (int)row, (int)meScnFnFieldCount(row), (int)count);

        for (at = 0; at < count; at++) {
            MEScnParamType type    = ME_SCN_PARAM_COUNT;
            bool           derived = false;

            UT_ASSERT(meScnFnFieldAt(row, at, name, sizeof(name), &type,
                                     &derived));
            UT_ASSERT_MSG(strcmp(name, fields[at].name) == 0,
                          "row %d field %d is '%s', not '%s'", (int)row,
                          (int)at, name, fields[at].name);
            UT_ASSERT_MSG((int)type == (int)fields[at].type,
                          "row %d field %s holds %d, not %d", (int)row, name,
                          (int)type, (int)fields[at].type);
            UT_ASSERT_MSG(type != ME_SCN_PARAM_NONE,
                          "row %d field %s holds a type this build has no "
                          "name for", (int)row, name);
            UT_ASSERT(derived == fields[at].derived);
            if (derived) {
                derivedSeen++;
                if (type == ME_SCN_PARAM_TAG || type == ME_SCN_PARAM_REGION) {
                    setValued++;
                }
            }
        }
        /* One past the end of the row answers nothing. */
        UT_ASSERT(!meScnFnFieldAt(row, count, name, sizeof(name), NULL, NULL));
    }
    UT_ASSERT(!meScnFnFieldAt(meScnFnCount(), 0, name, sizeof(name), NULL,
                              NULL));
    UT_ASSERT(meScnFnFieldCount(meScnFnCount()) == 0);
    UT_ASSERT_MSG(derivedSeen > 0 && setValued > 0,
                  "%d derived fields, %d of them set-valued", derivedSeen,
                  setValued);

    /* The ops, the same way: the editor's answer is the registry's, and the
     * scalar flag is scenarioLuaOpIsScalar's rather than a list of names. */
    rows = scenarioLuaRows(&opCount);
    UT_ASSERT(rows != NULL && opCount > 0);
    UT_ASSERT(meScenarioCompletionCount() == opCount);
    for (row = 0; row < opCount; row++) {
        const bool scalar = scenarioLuaOpIsScalar(&rows[row]);
        size_t     at;

        UT_ASSERT_MSG(meScenarioOpIsScalar(row) == scalar, "op %s",
                      rows[row].name);
        if (scalar) {
            scalars++;
        } else {
            nonScalars++;
        }
        UT_ASSERT_MSG(meScenarioOpParamCount(row) == rows[row].paramCount,
                      "op %s takes %d, not %d", rows[row].name,
                      (int)meScenarioOpParamCount(row),
                      (int)rows[row].paramCount);
        for (at = 0; at < rows[row].paramCount; at++) {
            const char    *argName  = NULL;
            MEScnParamType type     = ME_SCN_PARAM_COUNT;
            bool           optional = true;

            UT_ASSERT(meScenarioOpParamAt(row, at, &argName, &type,
                                          &optional));
            UT_ASSERT(strcmp(argName, rows[row].params[at].name) == 0);
            UT_ASSERT_MSG((int)type == (int)rows[row].params[at].type,
                          "op %s argument %s holds %d, not %d", rows[row].name,
                          argName, (int)type,
                          (int)rows[row].params[at].type);
            UT_ASSERT(type != ME_SCN_PARAM_NONE);
            UT_ASSERT(optional == rows[row].params[at].optional);
        }
        UT_ASSERT(!meScenarioOpParamAt(row, rows[row].paramCount, NULL, NULL,
                                       NULL));
        /* call is a trigger action and no row of the game table, so the two
           lists differ by exactly that word. */
        UT_ASSERT_MSG(strcmp(rows[row].name, ME_SCENARIO_CALL_OP) != 0,
                      "the registry carries '%s'", ME_SCENARIO_CALL_OP);
    }
    /* Both halves of the flag are real: an op an action can state, and one
       taking a table or a function that it cannot. */
    UT_ASSERT_MSG(scalars > 0 && nonScalars > 0, "%d scalar, %d not", scalars,
                  nonScalars);

    UT_ASSERT(!meScenarioOpIsScalar(opCount));
    UT_ASSERT(meScenarioOpParamCount(opCount) == 0);
    UT_ASSERT(!meScenarioOpParamAt(opCount, 0, NULL, NULL, NULL));

    return 0;
}
