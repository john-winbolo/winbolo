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
 */

#include <math.h>
#include <stdio.h>
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
