/*
 * manifest.json (src/scenario_io/scenario_manifest_json.c): the schema decoded
 * into ScenarioManifest, written back out with the keys this build does not
 * read still in it, and the comparison that holds a package's manifest
 * against the table its script declared.
 *
 * run_scenario_manifest_json_round_trip
 *      — every key of the version 1 schema into the struct, then out to
 *        text and back in unchanged; an unknown key at the root and an
 *        unknown key inside lobby both survive the trip
 * run_scenario_manifest_json_refusals
 *      — bytes that are not JSON, a root that is not an object, and a
 *        manifest version that is absent, not a number or not 1, each
 *        refused with its own message; an unknown rule name and a tag index
 *        past the map arrive as parse issues with the doc still parsing
 * run_scenario_manifest_agrees
 *      — two forms of the same scenario agree whatever order their rules
 *        and regions are in, and each field that can differ is refused
 *        with the key an author would look for; triggers are compared in
 *        order, having no name to be matched by, so the key naming one is
 *        its position
 * run_scenario_manifest_from_values
 *      — a struct with no JSON behind it goes out as text and comes back
 *        as the same struct
 * run_scenario_manifest_json_team_init
 *      — a team's init table: a number stored as the digits the Lua reader
 *        would have stored, a value of the wrong type stopping the walk with
 *        the pairs before it kept and the key named, a pair too long for the
 *        table named the same way, and an init that is not an object left
 *        empty with no pair named
 * run_scenario_manifest_json_number_range
 *      — a number past what its field can hold is held at the end of the
 *        range rather than cast out of it, one with no finite value takes
 *        the default, an api that is negative or has no finite value is
 *        refused outright, and a manifest whose numbers are in range is
 *        untouched
 * run_scenario_manifest_json_kind
 *      — the key that says whether the package may decide the round: both
 *        words in and out, a package that states none read as the scenario
 *        every package written before the key was one, and a word that is
 *        neither reported and read the same way
 * run_scenario_manifest_json_triggers
 *      — two triggers holding all four value kinds, an in and an eq, and an
 *        action whose long line lands on the action rather than in the
 *        argument slot, out to text and back unchanged; a triggers key that
 *        is not an array and an entry that is not an object are reported
 *        rather than refused; and each of the four caps drops what is past
 *        it and says which row went
 * run_scenario_manifest_workshop_keys
 *      — workshop_id and workshop_author read as digit strings, past 2^53
 *        without losing a digit, and written back as strings; absent and "0"
 *        read as no item without a report; a string that is not digits, one
 *        past 64 bits, and a JSON number read as 0 and are reported under
 *        their key; and a manifest naming no item writes no key for one
 * run_scenario_manifest_agrees_workshop
 *      — a script's table that states no Workshop item or author agrees
 *        with a manifest that names one, one that restates it agrees, and
 *        one that states a different one is refused under its key
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sim_rules_names.h" /* simRulesRuleIndex */
#include "scenario_manifest_json.h"
#include "scenario_table.h" /* scnTableGet, scnTableSet */
#include "test_harness.h"

/* Every key of the schema, plus two this build has no use for: editor_notes
 * at the root and seating_hint inside lobby. */
static const char kFullManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"api\": 1,\n"
    "  \"name\": \"Survival\",\n"
    "  \"description\": \"Hold the centre through five waves.\",\n"
    "  \"kind\": \"scenario\",\n"
    "  \"game\": \"tournament\",\n"
    "  \"bound\": true,\n"
    "  \"lobby\": {\n"
    "    \"max_players\": 6,\n"
    "    \"extra_teams\": false,\n"
    "    \"seating_hint\": \"clockwise\",\n"
    "    \"teams\": [\n"
    "      { \"id\": 2, \"bots\": 10, \"max_bots\": 12, \"fielded\": false,\n"
    "        \"brain\": \"package:raiders\",\n"
    "        \"mode\": \"turtle\", \"difficulty\": \"hard\",\n"
    "        \"init\": { \"stance\": \"hold\", \"deprive\": 100 } },\n"
    "      { \"id\": 3, \"bots\": 1, \"max_bots\": 4, \"fielded\": true,\n"
    "        \"brain\": \"\" }\n"
    "    ]\n"
    "  },\n"
    "  \"rules\": { \"tank_reload_ticks\": 8, \"shell_damage\": 2.5 },\n"
    "  \"tags\": { \"pills\": { \"3\": [\"outer\"] },\n"
    "              \"bases\": { \"1\": [\"keep\", \"hq\"] },\n"
    "              \"starts\": {} },\n"
    "  \"regions\": { \"keep\": { \"x\": 100, \"y\": 100, \"w\": 12, \"h\": 12 },\n"
    "                 \"outer\": { \"x\": 4, \"y\": 5, \"w\": 6, \"h\": 7 } },\n"
    "  \"triggers\": [],\n"
    "  \"script\": \"main.lua\",\n"
    "  \"brains\": [\"raiders\"],\n"
    "  \"editor_notes\": \"kept by a build that does not read it\"\n"
    "}\n";

/* Does the table hold this key with this value? */
static bool mjInitIs(const ScnTable *t, const char *key, const char *want) {
    const char *got = scnTableGet(t, key);
    return got != NULL && strcmp(got, want) == 0;
}

static ScnManifestDoc *parseText(const char *text, ScnParseReport *rep,
                                 char *err, size_t errLen) {
    return scnManifestParse((const uint8_t *)text, strlen(text), rep, err,
                            errLen);
}

/* Where the manifest sets one rule, or -1. */
static int ruleAt(const ScenarioManifest *m, const char *name) {
    int idx = simRulesRuleIndex(name);
    int i;

    if (idx < 0) {
        return -1;
    }
    for (i = 0; i < (int)m->numRules; i++) {
        if ((int)m->rules[i].rule == idx) {
            return i;
        }
    }
    return -1;
}

static int regionAt(const ScenarioManifest *m, const char *name) {
    int i;
    for (i = 0; i < (int)m->numRegions; i++) {
        if (strcmp(m->regions[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Everything the round-trip case asserts about a decoded manifest, so the
 * first parse and the re-parse are held to the same list. */
static int fullManifestIsRight(const ScnManifestDoc *d) {
    const ScenarioManifest *m = scnManifestValues(d);
    int                     r;

    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(scnManifestSchemaVersion(d) == 1, "schema is %d",
                  scnManifestSchemaVersion(d));
    UT_ASSERT_MSG(m->api == 1, "api is %d", m->api);
    UT_ASSERT(strcmp(m->name, "Survival") == 0);
    UT_ASSERT(strcmp(m->description,
                     "Hold the centre through five waves.") == 0);
    UT_ASSERT_MSG(m->kind == scnKindScenario, "kind is %d", (int)m->kind);
    UT_ASSERT(strcmp(m->game, "tournament") == 0);
    UT_ASSERT(m->bound);

    UT_ASSERT_MSG(m->lobby.maxPlayers == 6, "max_players is %d",
                  (int)m->lobby.maxPlayers);
    UT_ASSERT(!m->lobby.extraTeams);
    UT_ASSERT_MSG(m->lobby.numTeams == 2, "team count is %d",
                  (int)m->lobby.numTeams);
    UT_ASSERT(m->lobby.teams[0].id == 2);
    UT_ASSERT(m->lobby.teams[0].bots == 10);
    UT_ASSERT(m->lobby.teams[0].maxBots == 12);
    UT_ASSERT(!m->lobby.teams[0].fielded);
    UT_ASSERT(strcmp(m->lobby.teams[0].brain, "package:raiders") == 0);
    UT_ASSERT(strcmp(m->lobby.teams[0].mode, "turtle") == 0);
    UT_ASSERT(strcmp(m->lobby.teams[0].difficulty, "hard") == 0);
    UT_ASSERT_MSG(m->lobby.teams[0].init.count == 2, "team 0 holds %d pairs",
                  (int)m->lobby.teams[0].init.count);
    UT_ASSERT(mjInitIs(&m->lobby.teams[0].init, "stance", "hold"));
    /* A JSON number reaches the table as the digits lua_tostring would have
       given the same number written in the script. */
    UT_ASSERT(mjInitIs(&m->lobby.teams[0].init, "deprive", "100"));
    UT_ASSERT(m->lobby.teams[0].initBadKey[0] == '\0');
    UT_ASSERT(m->lobby.teams[1].id == 3);
    UT_ASSERT(m->lobby.teams[1].bots == 1);
    UT_ASSERT(m->lobby.teams[1].maxBots == 4);
    UT_ASSERT(m->lobby.teams[1].fielded);
    UT_ASSERT(m->lobby.teams[1].brain[0] == '\0');
    /* A team that names neither keeps neither, which is what says "leave
       the lobby's" rather than any particular mode. */
    UT_ASSERT(m->lobby.teams[1].mode[0] == '\0');
    UT_ASSERT(m->lobby.teams[1].difficulty[0] == '\0');
    UT_ASSERT_MSG(m->lobby.teams[1].init.count == 0, "team 1 holds %d pairs",
                  (int)m->lobby.teams[1].init.count);

    UT_ASSERT_MSG(m->numRules == 2, "rule count is %d", (int)m->numRules);
    r = ruleAt(m, "tank_reload_ticks");
    UT_ASSERT_MSG(r >= 0, "tank_reload_ticks is not set");
    UT_ASSERT_MSG(m->rules[r].value == 8.0, "tank_reload_ticks is %g",
                  m->rules[r].value);
    r = ruleAt(m, "shell_damage");
    UT_ASSERT_MSG(r >= 0, "shell_damage is not set");
    UT_ASSERT_MSG(m->rules[r].value == 2.5, "shell_damage is %g",
                  m->rules[r].value);

    UT_ASSERT_MSG(m->pillTags[3].count == 1, "pill 3 carries %d tags",
                  (int)m->pillTags[3].count);
    UT_ASSERT(strcmp(m->pillTags[3].tag[0], "outer") == 0);
    UT_ASSERT_MSG(m->baseTags[1].count == 2, "base 1 carries %d tags",
                  (int)m->baseTags[1].count);
    UT_ASSERT(strcmp(m->baseTags[1].tag[0], "keep") == 0);
    UT_ASSERT(strcmp(m->baseTags[1].tag[1], "hq") == 0);
    UT_ASSERT(m->startTags[1].count == 0);

    UT_ASSERT_MSG(m->numRegions == 2, "region count is %d",
                  (int)m->numRegions);
    r = regionAt(m, "keep");
    UT_ASSERT_MSG(r >= 0, "the keep region is missing");
    UT_ASSERT(m->regions[r].x == 100 && m->regions[r].y == 100 &&
              m->regions[r].w == 12 && m->regions[r].h == 12);
    r = regionAt(m, "outer");
    UT_ASSERT_MSG(r >= 0, "the outer region is missing");
    UT_ASSERT(m->regions[r].x == 4 && m->regions[r].y == 5 &&
              m->regions[r].w == 6 && m->regions[r].h == 7);

    UT_ASSERT(strcmp(scnManifestScriptEntry(d), "main.lua") == 0);
    UT_ASSERT_MSG(scnManifestBrainCount(d) == 1, "brain count is %d",
                  scnManifestBrainCount(d));
    UT_ASSERT(strcmp(scnManifestBrainName(d, 0), "raiders") == 0);
    UT_ASSERT(scnManifestBrainName(d, 1) == NULL);
    return 0;
}

int run_scenario_manifest_json_round_trip(void) {
    char            err[256];
    char            key[SCN_VALIDATE_KEY_LEN];
    ScnManifestDoc *first;
    ScnManifestDoc *again;
    char           *text;
    int             rc;

    first = parseText(kFullManifest, NULL, err, sizeof(err));
    UT_ASSERT_MSG(first != NULL, "the manifest was refused: %s", err);

    rc = fullManifestIsRight(first);
    if (rc != 0) {
        scnManifestFree(first);
        return rc;
    }

    text = scnManifestWrite(first, err, sizeof(err));
    if (text == NULL) {
        scnManifestFree(first);
        UT_FAIL("the manifest could not be written: %s", err);
    }

    /* The two keys nothing here decodes, and the triggers list, are still in
     * the text this build produced. */
    if (strstr(text, "editor_notes") == NULL) {
        free(text);
        scnManifestFree(first);
        UT_FAIL("the unknown root key was dropped");
    }
    if (strstr(text, "seating_hint") == NULL) {
        free(text);
        scnManifestFree(first);
        UT_FAIL("the unknown key inside lobby was dropped");
    }
    if (strstr(text, "triggers") == NULL) {
        free(text);
        scnManifestFree(first);
        UT_FAIL("triggers was dropped");
    }

    again = parseText(text, NULL, err, sizeof(err));
    free(text);
    if (again == NULL) {
        scnManifestFree(first);
        UT_FAIL("what was written would not parse: %s", err);
    }

    rc = fullManifestIsRight(again);
    if (rc == 0 &&
        !scnManifestAgrees(scnManifestValues(first), scnManifestValues(again),
                           key, sizeof(key), err, sizeof(err))) {
        fprintf(stderr, "FAIL %s:%d: the round trip changed %s: %s\n",
                __FILE__, __LINE__, key, err);
        rc = 1;
    }

    scnManifestFree(again);
    scnManifestFree(first);
    return rc;
}

int run_scenario_manifest_json_refusals(void) {
    static const char *const kBad[] = {
        "{ this is not json",
        "[1, 2, 3]",
        "{ \"api\": 1 }",
        "{ \"manifest\": \"1\" }",
        "{ \"manifest\": 2 }",
    };
    static const char kSoftProblems[] =
        "{ \"manifest\": 1,"
        "  \"rules\": { \"tank_reload_ticks\": 8, \"no_such_rule\": 3 },"
        "  \"tags\": { \"pills\": { \"99\": [\"far\"] } } }";

    char              err[5][256];
    char              soft[256];
    ScnValidateResult sink;
    ScnParseReport    rep;
    ScnManifestDoc   *d;
    int               i;
    int               j;
    bool              sawRule = false;
    bool              sawTag = false;

    for (i = 0; i < 5; i++) {
        d = parseText(kBad[i], NULL, err[i], sizeof(err[i]));
        UT_ASSERT_MSG(d == NULL, "refusal %d parsed instead", i);
        UT_ASSERT_MSG(err[i][0] != '\0', "refusal %d said nothing", i);
    }
    for (i = 0; i < 5; i++) {
        for (j = i + 1; j < 5; j++) {
            UT_ASSERT_MSG(strcmp(err[i], err[j]) != 0,
                          "refusals %d and %d share one message: %s", i, j,
                          err[i]);
        }
    }

    /* A rule name that names no rule and a pill index past the map are
     * reported and dropped; everything else still decodes. */
    memset(&sink, 0, sizeof(sink));
    soft[0] = '\0';
    rep.soft = soft;
    rep.softLen = sizeof(soft);
    rep.sink = &sink;

    d = parseText(kSoftProblems, &rep, err[0], sizeof(err[0]));
    UT_ASSERT_MSG(d != NULL, "a manifest with soft problems was refused: %s",
                  err[0]);
    UT_ASSERT_MSG(scnManifestValues(d)->numRules == 1, "rule count is %d",
                  (int)scnManifestValues(d)->numRules);
    UT_ASSERT(scnManifestValues(d)->pillTags[3].count == 0);
    UT_ASSERT_MSG(sink.count >= 2, "%d issues were collected",
                  (int)sink.count);
    for (i = 0; i < (int)sink.count; i++) {
        if (strcmp(sink.issues[i].key, "rules.no_such_rule") == 0) {
            sawRule = true;
        }
        if (strcmp(sink.issues[i].key, "tags.pills[99]") == 0) {
            sawTag = true;
        }
    }
    UT_ASSERT_MSG(sawRule, "the unknown rule name was not reported");
    UT_ASSERT_MSG(sawTag, "the out-of-range pill index was not reported");
    UT_ASSERT(soft[0] != '\0');

    scnManifestFree(d);
    return 0;
}

/* The pair of triggers both forms hold. Filled in the Triggers section
   below, beside the JSON stating the same two. */
static void fillTriggers(ScenarioManifest *m);

/* One scenario, as both forms would hold it. */
static void fillBase(ScenarioManifest *m) {
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "Survival");
    snprintf(m->description, sizeof(m->description),
             "Hold the centre through five waves.");
    m->api = 1;
    m->kind = scnKindScenario;
    snprintf(m->game, sizeof(m->game), "tournament");
    m->bound = true;

    m->lobby.maxPlayers = 6;
    m->lobby.extraTeams = false;
    m->lobby.numTeams = 2;
    m->lobby.teams[0].id = 2;
    m->lobby.teams[0].bots = 10;
    m->lobby.teams[0].maxBots = 12;
    m->lobby.teams[0].fielded = false;
    snprintf(m->lobby.teams[0].brain, sizeof(m->lobby.teams[0].brain),
             "package:raiders");
    snprintf(m->lobby.teams[0].mode, sizeof(m->lobby.teams[0].mode),
             "turtle");
    snprintf(m->lobby.teams[0].difficulty,
             sizeof(m->lobby.teams[0].difficulty), "hard");
    scnTableSet(&m->lobby.teams[0].init, "stance", "hold");
    scnTableSet(&m->lobby.teams[0].init, "deprive", "100");
    m->lobby.teams[1].id = 3;
    m->lobby.teams[1].bots = 1;
    m->lobby.teams[1].maxBots = 4;
    m->lobby.teams[1].fielded = true;

    m->numRules = 2;
    m->rules[0].rule = (uint16_t)simRulesRuleIndex("tank_reload_ticks");
    m->rules[0].value = 8.0;
    m->rules[1].rule = (uint16_t)simRulesRuleIndex("shell_damage");
    m->rules[1].value = 2.5;

    m->pillTags[3].count = 1;
    snprintf(m->pillTags[3].tag[0], SCN_TAG_LEN, "outer");
    m->baseTags[1].count = 2;
    snprintf(m->baseTags[1].tag[0], SCN_TAG_LEN, "keep");
    snprintf(m->baseTags[1].tag[1], SCN_TAG_LEN, "hq");

    m->numRegions = 2;
    snprintf(m->regions[0].name, SCN_REGION_NAME_LEN, "keep");
    m->regions[0].x = 100;
    m->regions[0].y = 100;
    m->regions[0].w = 12;
    m->regions[0].h = 12;
    snprintf(m->regions[1].name, SCN_REGION_NAME_LEN, "outer");
    m->regions[1].x = 4;
    m->regions[1].y = 5;
    m->regions[1].w = 6;
    m->regions[1].h = 7;

    fillTriggers(m);
}

int run_scenario_manifest_agrees(void) {
    ScenarioManifest a;
    ScenarioManifest b;
    char             key[SCN_VALIDATE_KEY_LEN];
    char             err[256];

    /* The same scenario twice, with the two keyed tables walked in the other
     * order — which is what lua_next does from one build to the next. */
    fillBase(&a);
    fillBase(&b);
    b.rules[0] = a.rules[1];
    b.rules[1] = a.rules[0];
    b.regions[0] = a.regions[1];
    b.regions[1] = a.regions[0];
    UT_ASSERT_MSG(scnManifestAgrees(&a, &b, key, sizeof(key), err,
                                    sizeof(err)),
                  "a reordered copy was refused at %s: %s", key, err);

    /* And each field that can differ, named the way the JSON spells it. */
    fillBase(&b);
    snprintf(b.name, sizeof(b.name), "Something Else");
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "name") == 0, "the key was '%s'", key);
    UT_ASSERT(err[0] != '\0');

    /* The kind is the one field where the two forms disagreeing changes
       what the round may do rather than only how it reads, so it is held to
       agreeing like the rest of them. */
    fillBase(&b);
    b.kind = scnKindKeepsWinCondition;
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "kind") == 0, "the key was '%s'", key);
    UT_ASSERT_MSG(strstr(err, "scenario") != NULL && strstr(err, "mod") != NULL,
                  "the message does not say which form said which: %s", err);

    fillBase(&b);
    b.bound = false;
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "bound") == 0, "the key was '%s'", key);

    fillBase(&b);
    b.lobby.maxPlayers = 8;
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "lobby.max_players") == 0, "the key was '%s'",
                  key);

    fillBase(&b);
    snprintf(b.lobby.teams[0].brain, sizeof(b.lobby.teams[0].brain),
             "package:swarm");
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "lobby.teams[0].brain") == 0,
                  "the key was '%s'", key);

    /* The mode and the level travel with the brain and are held to
       agreeing the same way. The second of these is a team dropping a key
       the other side states: "" is a value here — it says "leave the
       lobby's" — so the two forms disagree rather than one of them
       simply saying less. */
    fillBase(&b);
    snprintf(b.lobby.teams[0].mode, sizeof(b.lobby.teams[0].mode),
             "assault");
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "lobby.teams[0].mode") == 0,
                  "the key was '%s'", key);

    fillBase(&b);
    b.lobby.teams[0].difficulty[0] = '\0';
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "lobby.teams[0].difficulty") == 0,
                  "the key was '%s'", key);

    /* An init table is keyed, so the order its pairs sit in is Lua's own
       business and a reordered copy is the same table. */
    fillBase(&b);
    b.lobby.teams[0].init.kv[0] = a.lobby.teams[0].init.kv[1];
    b.lobby.teams[0].init.kv[1] = a.lobby.teams[0].init.kv[0];
    UT_ASSERT_MSG(scnManifestAgrees(&a, &b, key, sizeof(key), err,
                                    sizeof(err)),
                  "a reordered init was refused at %s: %s", key, err);

    fillBase(&b);
    scnTableSet(&b.lobby.teams[0].init, "deprive", "50");
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "lobby.teams[0].init.deprive") == 0,
                  "the key was '%s'", key);

    /* A pair one side holds and the other does not, named from either side. */
    fillBase(&b);
    scnTableSet(&b.lobby.teams[0].init, "extra", "1");
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "lobby.teams[0].init.extra") == 0,
                  "the key was '%s'", key);
    UT_ASSERT(!scnManifestAgrees(&b, &a, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "lobby.teams[0].init.extra") == 0,
                  "the key was '%s'", key);

    /* initBadKey says what a reader could not take rather than what the team
       is, so the two forms stopping in different places is not a conflict. */
    fillBase(&b);
    snprintf(b.lobby.teams[0].initBadKey,
             sizeof(b.lobby.teams[0].initBadKey), "toolong");
    UT_ASSERT_MSG(scnManifestAgrees(&a, &b, key, sizeof(key), err,
                                    sizeof(err)),
                  "a named bad pair was refused at %s: %s", key, err);

    /* The teams array keeps its order, so swapping two teams is a conflict
     * even though the same two teams are named. */
    fillBase(&b);
    b.lobby.teams[0] = a.lobby.teams[1];
    b.lobby.teams[1] = a.lobby.teams[0];
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strncmp(key, "lobby.teams[0]", 14) == 0,
                  "the key was '%s'", key);

    fillBase(&b);
    b.rules[1].value = 3.0;
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "rules.shell_damage") == 0, "the key was '%s'",
                  key);

    fillBase(&b);
    b.regions[1].w = 9;
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "regions.outer") == 0, "the key was '%s'", key);

    fillBase(&b);
    b.pillTags[3].count = 0;
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "tags.pills[3]") == 0, "the key was '%s'", key);

    /* Triggers. A trigger has no name to match one against another by, so
       the two forms are compared in order and the key is the position —
       which is the position both readers report under. */
    fillBase(&b);
    b.numTriggers = 1;
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "triggers") == 0, "the key was '%s'", key);

    fillBase(&b);
    snprintf(b.triggers[1].when, SCN_TRIGGER_NAME_LEN, "on_round_start");
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "triggers[1]") == 0, "the key was '%s'", key);

    fillBase(&b);
    snprintf(b.triggers[0].where[1].value.text, SCN_TRIGGER_NAME_LEN,
             "inner_base");
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "triggers[0].where[1]") == 0,
                  "the key was '%s'", key);

    fillBase(&b);
    b.triggers[0].actions[1].args[1].num = 11.0;
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "triggers[0].actions[1][1]") == 0,
                  "the key was '%s'", key);

    /* The long line an action carries is the action's, not the argument
       slot's, and is held against the other form there. */
    fillBase(&b);
    snprintf(b.triggers[0].actions[0].text, SCN_TRIGGER_TEXT_LEN,
             "The ring gives and the wave is through the keep before dawn");
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "triggers[0].actions[0]") == 0,
                  "the key was '%s'", key);

    /* A value differing only in the kind it was read as, which a comparison
       reading num alone would pass: 0 as a number and false as a boolean
       both carry 0.0. */
    fillBase(&b);
    b.triggers[1].where[0].value.kind = SCN_TRIG_VAL_NUMBER;
    UT_ASSERT(!scnManifestAgrees(&a, &b, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "triggers[1].where[0]") == 0,
                  "the key was '%s'", key);

    return 0;
}

int run_scenario_manifest_from_values(void) {
    ScenarioManifest base;
    ScnManifestDoc  *made;
    ScnManifestDoc  *back;
    char            *text;
    char             err[256];
    char             key[SCN_VALIDATE_KEY_LEN];
    int              rc = 0;

    fillBase(&base);

    made = scnManifestFromValues(&base, err, sizeof(err));
    UT_ASSERT_MSG(made != NULL, "a doc could not be built: %s", err);
    UT_ASSERT(scnManifestSchemaVersion(made) == 1);
    UT_ASSERT(strcmp(scnManifestScriptEntry(made), "main.lua") == 0);
    UT_ASSERT_MSG(scnManifestBrainCount(made) == 0, "brain count is %d",
                  scnManifestBrainCount(made));

    text = scnManifestWrite(made, err, sizeof(err));
    if (text == NULL) {
        scnManifestFree(made);
        UT_FAIL("the manifest could not be written: %s", err);
    }

    back = parseText(text, NULL, err, sizeof(err));
    free(text);
    if (back == NULL) {
        scnManifestFree(made);
        UT_FAIL("what was written would not parse: %s", err);
    }

    if (!scnManifestAgrees(&base, scnManifestValues(back), key, sizeof(key),
                           err, sizeof(err))) {
        fprintf(stderr, "FAIL %s:%d: the trip through text changed %s: %s\n",
                __FILE__, __LINE__, key, err);
        rc = 1;
    }
    if (rc == 0 && strcmp(scnManifestScriptEntry(back), "main.lua") != 0) {
        fprintf(stderr, "FAIL %s:%d: the script entry came back '%s'\n",
                __FILE__, __LINE__, scnManifestScriptEntry(back));
        rc = 1;
    }

    scnManifestFree(back);
    scnManifestFree(made);
    return rc;
}

/* The one team of a manifest whose team carries this init text. The doc is
 * freed here and the team handed back by value, so a case that fails leaves
 * nothing behind. */
static int readInit(const char *initText, ScnManifestTeam *out) {
    char            text[1024];
    char            err[256];
    ScnManifestDoc *d;

    snprintf(text, sizeof(text),
             "{ \"manifest\": 1, \"api\": 1, \"name\": \"T\",\n"
             "  \"lobby\": { \"teams\": [ { \"id\": 2, \"init\": %s } ] } }",
             initText);
    d = parseText(text, NULL, err, sizeof(err));
    UT_ASSERT_MSG(d != NULL, "the manifest was refused: %s", err);
    UT_ASSERT_MSG(scnManifestValues(d)->lobby.numTeams == 1,
                  "the manifest named %d teams",
                  (int)scnManifestValues(d)->lobby.numTeams);
    *out = scnManifestValues(d)->lobby.teams[0];
    scnManifestFree(d);
    return 0;
}

int run_scenario_manifest_json_team_init(void) {
    ScnManifestTeam t;
    ScenarioManifest m;
    ScnManifestDoc  *made;
    char             err[256];
    char             tooLong[SCN_TABLE_VALUE_LEN + 8];
    char             text[256];
    char            *out;
    int              rc;
    size_t           i;

    /* A number is the digits the Lua reader would have stored for the same
       number written in a script: a whole one with no point, and one that is
       not whole as it reads. */
    rc = readInit("{ \"deprive\": 100, \"rate\": 2.5 }", &t);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(t.init.count == 2, "the team holds %d pairs",
                  (int)t.init.count);
    UT_ASSERT(mjInitIs(&t.init, "deprive", "100"));
    UT_ASSERT(mjInitIs(&t.init, "rate", "2.5"));
    UT_ASSERT_MSG(t.initBadKey[0] == '\0', "'%s' was named", t.initBadKey);

    /* A value that is neither a string nor a number stops the walk where the
       Lua reader stops on the same value. The pair before it stays, the pair
       after it does not, and the key is named. */
    rc = readInit("{ \"stance\": \"hold\", \"ready\": true, \"after\": \"x\" }",
                  &t);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(t.init.count == 1, "the team holds %d pairs",
                  (int)t.init.count);
    UT_ASSERT(mjInitIs(&t.init, "stance", "hold"));
    UT_ASSERT(scnTableGet(&t.init, "after") == NULL);
    UT_ASSERT_MSG(strcmp(t.initBadKey, "ready") == 0, "'%s' was named",
                  t.initBadKey);

    /* A value too long for the table is named the same way. */
    for (i = 0; i < sizeof(tooLong) - 1; i++) {
        tooLong[i] = 'a';
    }
    tooLong[sizeof(tooLong) - 1] = '\0';
    snprintf(text, sizeof(text), "{ \"stance\": \"hold\", \"long\": \"%s\" }",
             tooLong);
    rc = readInit(text, &t);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(t.init.count == 1, "the team holds %d pairs",
                  (int)t.init.count);
    UT_ASSERT_MSG(strcmp(t.initBadKey, "long") == 0, "'%s' was named",
                  t.initBadKey);

    /* An init that is not an object at all leaves the table empty and names
       no pair, the way the Lua reader leaves a field of the wrong type. */
    rc = readInit("5", &t);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(t.init.count == 0, "the team holds %d pairs",
                  (int)t.init.count);
    UT_ASSERT_MSG(t.initBadKey[0] == '\0', "'%s' was named", t.initBadKey);

    /* And a team with no pairs writes no init key at all. */
    fillBase(&m);
    scnTableClear(&m.lobby.teams[0].init);
    made = scnManifestFromValues(&m, err, sizeof(err));
    UT_ASSERT_MSG(made != NULL, "a doc could not be built: %s", err);
    out = scnManifestWrite(made, err, sizeof(err));
    scnManifestFree(made);
    UT_ASSERT_MSG(out != NULL, "the manifest could not be written: %s", err);
    if (strstr(out, "\"init\"") != NULL) {
        free(out);
        UT_FAIL("a team holding no init pairs wrote an init key");
    }
    free(out);
    return 0;
}

/* ── Numbers a byte cannot hold ───────────────────────────────────── */

/* cJSON keeps every number as a double, and casting one outside the
 * destination's range to a uint8_t or an int is undefined behaviour rather
 * than a large number — "max_players": 1e30 is whatever the compiler felt
 * like that day. The reader holds each number to its field's range before the
 * cast and reports what it did, and takes the default for a value that is no
 * number at all.
 *
 * JSON has no literal for NaN or infinity, so a decoder cannot be handed one
 * as text; what it can be handed is a number too large for a double to keep
 * finite, which is where cJSON's own parse produces one. 1e999 is that. */
int run_scenario_manifest_json_number_range(void) {
    static const char kWide[] =
        "{\n"
        "  \"manifest\": 1,\n"
        "  \"name\": \"Wide\",\n"
        "  \"api\": 1,\n"
        "  \"lobby\": {\n"
        "    \"max_players\": 1e30,\n"
        "    \"teams\": [ { \"id\": -5, \"bots\": 1e30, \"max_bots\": 2 } ]\n"
        "  },\n"
        "  \"regions\": { \"pit\": { \"x\": 300, \"y\": -1, \"w\": 4,\n"
        "                            \"h\": 1e999 } }\n"
        "}\n";

    ScnManifestDoc         *d;
    const ScenarioManifest *m;
    char                    err[512];

    err[0] = '\0';
    d = parseText(kWide, NULL, err, sizeof(err));
    UT_ASSERT_MSG(d != NULL,
                  "a manifest with numbers out of range would not parse: %s",
                  err);
    m = scnManifestValues(d);
    UT_ASSERT(m != NULL);

    /* Each one held at the end of the range it went past, rather than
       wrapped onto whatever the cast produced. */
    UT_ASSERT_MSG(m->lobby.maxPlayers == 255,
                  "max_players of 1e30 decoded as %u, expected the range's "
                  "255", (unsigned)m->lobby.maxPlayers);
    UT_ASSERT_MSG(m->lobby.numTeams == 1,
                  "the lobby holds %u teams, expected 1",
                  (unsigned)m->lobby.numTeams);
    UT_ASSERT_MSG(m->lobby.teams[0].id == 0,
                  "a team id of -5 decoded as %u, expected 0",
                  (unsigned)m->lobby.teams[0].id);
    UT_ASSERT_MSG(m->lobby.teams[0].bots == 255,
                  "bots of 1e30 decoded as %u, expected 255",
                  (unsigned)m->lobby.teams[0].bots);
    UT_ASSERT_MSG(m->lobby.teams[0].maxBots == 2,
                  "max_bots of 2 decoded as %u, and it is in range",
                  (unsigned)m->lobby.teams[0].maxBots);

    UT_ASSERT_MSG(m->numRegions == 1,
                  "the manifest holds %u regions, expected 1",
                  (unsigned)m->numRegions);
    UT_ASSERT_MSG(m->regions[0].x == 255,
                  "a region x of 300 decoded as %u, expected 255",
                  (unsigned)m->regions[0].x);
    UT_ASSERT_MSG(m->regions[0].y == 0,
                  "a region y of -1 decoded as %u, expected 0",
                  (unsigned)m->regions[0].y);
    UT_ASSERT_MSG(m->regions[0].w == 4,
                  "a region w of 4 decoded as %u, and it is in range",
                  (unsigned)m->regions[0].w);
    /* 1e999 has no finite value, so the field takes its default rather than
       a bound: being told "0" is more use than being told "clamped to 255". */
    UT_ASSERT_MSG(m->regions[0].h == 0,
                  "a region h with no finite value decoded as %u, expected "
                  "the default 0", (unsigned)m->regions[0].h);

    scnManifestFree(d);

    /* api is the one field a clamp is wrong for: it decides whether this
       build understands the content at all, so a value it cannot read is a
       refusal. A negative api held at 0 would pass every server's check and
       run content written against nothing. */
    {
        static const char kNegative[] =
            "{ \"manifest\": 1, \"name\": \"Back\", \"api\": -1 }\n";
        static const char kWideApi[] =
            "{ \"manifest\": 1, \"name\": \"Vast\", \"api\": 1e999 }\n";

        err[0] = '\0';
        d = parseText(kNegative, NULL, err, sizeof(err));
        UT_ASSERT_MSG(d == NULL, "a manifest with api -1 was decoded");
        UT_ASSERT_MSG(strstr(err, "api") != NULL,
                      "the refusal does not name the field: %s", err);
        UT_ASSERT_MSG(strstr(err, "-1") != NULL,
                      "the refusal does not name the value: %s", err);

        err[0] = '\0';
        d = parseText(kWideApi, NULL, err, sizeof(err));
        UT_ASSERT_MSG(d == NULL,
                      "a manifest whose api has no finite value was decoded");
        UT_ASSERT_MSG(strstr(err, "api") != NULL,
                      "the refusal does not name the field: %s", err);
        UT_ASSERT_MSG(strstr(err, "inf") != NULL,
                      "the refusal does not name the value as it was read: %s",
                      err);
    }

    /* And a manifest whose numbers are all in range is untouched by any of
       it: the clamp is a bound, not a rewrite. */
    {
        static const char kFine[] =
            "{\n"
            "  \"manifest\": 1,\n"
            "  \"name\": \"Fine\",\n"
            "  \"api\": 2,\n"
            "  \"lobby\": {\n"
            "    \"max_players\": 8,\n"
            "    \"teams\": [ { \"id\": 3, \"bots\": 2, \"max_bots\": 4 } ]\n"
            "  }\n"
            "}\n";

        err[0] = '\0';
        d = parseText(kFine, NULL, err, sizeof(err));
        UT_ASSERT_MSG(d != NULL, "an ordinary manifest would not parse: %s",
                      err);
        m = scnManifestValues(d);
        UT_ASSERT(m != NULL);
        UT_ASSERT_MSG(m->api == 2, "api is %d, expected 2", m->api);
        UT_ASSERT_MSG(m->lobby.maxPlayers == 8,
                      "max_players is %u, expected 8",
                      (unsigned)m->lobby.maxPlayers);
        UT_ASSERT_MSG(m->lobby.teams[0].id == 3 &&
                      m->lobby.teams[0].bots == 2 &&
                      m->lobby.teams[0].maxBots == 4,
                      "an in-range team decoded as %u/%u/%u",
                      (unsigned)m->lobby.teams[0].id,
                      (unsigned)m->lobby.teams[0].bots,
                      (unsigned)m->lobby.teams[0].maxBots);
        scnManifestFree(d);
    }
    return 0;
}

/* ── Triggers ─────────────────────────────────────────────────────── */

/* The long line an announce carries: past SCN_TRIGGER_NAME_LEN, so it lands
   on the action's text rather than in the argument slot. */
static const char kLongLine[] =
    "The ring holds and the wave is turned back short of the keep";

/* Two triggers between them holding all four value kinds, an in and an eq,
   and an action whose first argument is too long for an argument slot. */
static const char kTriggerManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"api\": 1,\n"
    "  \"name\": \"Trigger trial\",\n"
    "  \"triggers\": [\n"
    "    { \"when\": \"on_base_captured\",\n"
    "      \"where\": [ [\"new_team\", \"eq\", 1],\n"
    "                   [\"tag\", \"in\", \"outer_base\"] ],\n"
    "      \"actions\": [ [\"announce\",\n"
    "                   \"The ring holds and the wave is turned back short "
    "of the keep\", 5],\n"
    "                  [\"set_score\", { \"field\": \"new\" }, 10] ] },\n"
    "    { \"when\": \"on_tick\",\n"
    "      \"where\": [ [\"scripted\", \"eq\", false] ],\n"
    "      \"actions\": [ [\"log\", \"tick\"] ] }\n"
    "  ]\n"
    "}\n";

/* The same two triggers again, straight into the struct: what
   kTriggerManifest above decodes to, for the cases that start from a struct
   rather than from text. */
static void fillTriggers(ScenarioManifest *m) {
    ScnTrigger *t;
    ScnTrigAct *a;

    m->numTriggers = 2;

    t = &m->triggers[0];
    snprintf(t->when, SCN_TRIGGER_NAME_LEN, "on_base_captured");
    t->numWhere = 2;
    snprintf(t->where[0].field, SCN_TRIGGER_NAME_LEN, "new_team");
    t->where[0].op         = SCN_TRIG_CMP_EQ;
    t->where[0].value.kind = SCN_TRIG_VAL_NUMBER;
    t->where[0].value.num  = 1.0;
    snprintf(t->where[1].field, SCN_TRIGGER_NAME_LEN, "tag");
    t->where[1].op         = SCN_TRIG_CMP_IN;
    t->where[1].value.kind = SCN_TRIG_VAL_STRING;
    snprintf(t->where[1].value.text, SCN_TRIGGER_NAME_LEN, "outer_base");

    t->numActions = 2;
    a = &t->actions[0];
    snprintf(a->op, SCN_TRIGGER_NAME_LEN, "announce");
    a->numArgs        = 2;
    a->args[0].kind   = SCN_TRIG_VAL_STRING;
    a->args[0].inText = true;          /* the line is on the action below */
    snprintf(a->text, SCN_TRIGGER_TEXT_LEN, "%s", kLongLine);
    a->args[1].kind = SCN_TRIG_VAL_NUMBER;
    a->args[1].num  = 5.0;
    a = &t->actions[1];
    snprintf(a->op, SCN_TRIGGER_NAME_LEN, "set_score");
    a->numArgs      = 2;
    a->args[0].kind = SCN_TRIG_VAL_FIELD;
    snprintf(a->args[0].text, SCN_TRIGGER_NAME_LEN, "new");
    a->args[1].kind = SCN_TRIG_VAL_NUMBER;
    a->args[1].num  = 10.0;

    t = &m->triggers[1];
    snprintf(t->when, SCN_TRIGGER_NAME_LEN, "on_tick");
    t->numWhere = 1;
    snprintf(t->where[0].field, SCN_TRIGGER_NAME_LEN, "scripted");
    t->where[0].op         = SCN_TRIG_CMP_EQ;
    t->where[0].value.kind = SCN_TRIG_VAL_BOOL;
    t->where[0].value.num  = 0.0;
    t->numActions          = 1;
    a                      = &t->actions[0];
    snprintf(a->op, SCN_TRIGGER_NAME_LEN, "log");
    a->numArgs      = 1;
    a->args[0].kind = SCN_TRIG_VAL_STRING;
    snprintf(a->args[0].text, SCN_TRIGGER_NAME_LEN, "tick");
}

/* Is a key among the issues the parse collected? */
static bool sawIssue(const ScnValidateResult *sink, const char *key) {
    int i;
    for (i = 0; i < (int)sink->count; i++) {
        if (strcmp(sink->issues[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

/* And what it said, for a key that can carry more than one reading. */
static bool issueSays(const ScnValidateResult *sink, const char *key,
                      const char *want) {
    int i;
    for (i = 0; i < (int)sink->count; i++) {
        if (strcmp(sink->issues[i].key, key) == 0 &&
            strstr(sink->issues[i].message, want) != NULL) {
            return true;
        }
    }
    return false;
}

/* What the two triggers of kTriggerManifest have to decode to. Held apart
   from the round trip so the same reading is made of the text this build
   wrote and of the text it was given. */
static int triggerManifestIsRight(const ScenarioManifest *m) {
    const ScnTrigger *t;
    const ScnTrigAct *a;

    UT_ASSERT_MSG(m->numTriggers == 2, "trigger count is %u",
                  (unsigned)m->numTriggers);

    t = &m->triggers[0];
    UT_ASSERT(strcmp(t->when, "on_base_captured") == 0);
    UT_ASSERT_MSG(t->numWhere == 2, "trigger 0 holds %u tests",
                  (unsigned)t->numWhere);
    UT_ASSERT_MSG(t->numActions == 2, "trigger 0 holds %u actions",
                  (unsigned)t->numActions);

    /* A number against eq. */
    UT_ASSERT(strcmp(t->where[0].field, "new_team") == 0);
    UT_ASSERT(t->where[0].op == SCN_TRIG_CMP_EQ);
    UT_ASSERT(t->where[0].value.kind == SCN_TRIG_VAL_NUMBER);
    UT_ASSERT_MSG(t->where[0].value.num == 1.0, "new_team is %g",
                  t->where[0].value.num);

    /* A short string against in: short enough for the slot's own text. */
    UT_ASSERT(strcmp(t->where[1].field, "tag") == 0);
    UT_ASSERT(t->where[1].op == SCN_TRIG_CMP_IN);
    UT_ASSERT(t->where[1].value.kind == SCN_TRIG_VAL_STRING);
    UT_ASSERT(!t->where[1].value.inText);
    UT_ASSERT(strcmp(t->where[1].value.text, "outer_base") == 0);

    /* The long line is on the action, and the slot that holds it says so
       rather than carrying the bytes. */
    a = &t->actions[0];
    UT_ASSERT(strcmp(a->op, "announce") == 0);
    UT_ASSERT_MSG(a->numArgs == 2, "announce took %u arguments",
                  (unsigned)a->numArgs);
    UT_ASSERT(a->args[0].kind == SCN_TRIG_VAL_STRING);
    UT_ASSERT_MSG(a->args[0].inText,
                  "the long line was not moved to the action's text");
    UT_ASSERT(a->args[0].text[0] == '\0');
    UT_ASSERT_MSG(strcmp(a->text, kLongLine) == 0,
                  "the action's text is '%s'", a->text);
    UT_ASSERT(a->args[1].kind == SCN_TRIG_VAL_NUMBER);
    UT_ASSERT(a->args[1].num == 5.0);

    /* A field reference, and a number beside it. */
    a = &t->actions[1];
    UT_ASSERT(strcmp(a->op, "set_score") == 0);
    UT_ASSERT_MSG(a->numArgs == 2, "set_score took %u arguments",
                  (unsigned)a->numArgs);
    UT_ASSERT(a->args[0].kind == SCN_TRIG_VAL_FIELD);
    UT_ASSERT(strcmp(a->args[0].text, "new") == 0);
    UT_ASSERT(a->args[1].kind == SCN_TRIG_VAL_NUMBER);
    UT_ASSERT(a->args[1].num == 10.0);
    UT_ASSERT_MSG(a->text[0] == '\0',
                  "an action with no long string carries '%s'", a->text);

    /* A boolean. */
    t = &m->triggers[1];
    UT_ASSERT(strcmp(t->when, "on_tick") == 0);
    UT_ASSERT_MSG(t->numWhere == 1, "trigger 1 holds %u tests",
                  (unsigned)t->numWhere);
    UT_ASSERT(strcmp(t->where[0].field, "scripted") == 0);
    UT_ASSERT(t->where[0].value.kind == SCN_TRIG_VAL_BOOL);
    UT_ASSERT_MSG(t->where[0].value.num == 0.0, "false decoded as %g",
                  t->where[0].value.num);
    UT_ASSERT_MSG(t->numActions == 1, "trigger 1 holds %u actions",
                  (unsigned)t->numActions);
    UT_ASSERT(strcmp(t->actions[0].op, "log") == 0);
    UT_ASSERT(t->actions[0].args[0].kind == SCN_TRIG_VAL_STRING);
    UT_ASSERT(strcmp(t->actions[0].args[0].text, "tick") == 0);
    return 0;
}

int run_scenario_manifest_json_triggers(void) {
    char               err[256];
    char               soft[256];
    ScnManifestDoc    *first;
    ScnManifestDoc    *again;
    ScnValidateResult *sink;
    ScnParseReport     rep;
    char              *text;
    char              *big;
    size_t             used;
    int                i;
    int                rc;

    /* ── The round trip ──────────────────────────────────────────── */

    first = parseText(kTriggerManifest, NULL, err, sizeof(err));
    UT_ASSERT_MSG(first != NULL, "the manifest was refused: %s", err);

    rc = triggerManifestIsRight(scnManifestValues(first));
    if (rc != 0) {
        scnManifestFree(first);
        return rc;
    }

    text = scnManifestWrite(first, err, sizeof(err));
    if (text == NULL) {
        scnManifestFree(first);
        UT_FAIL("the manifest could not be written: %s", err);
    }
    again = parseText(text, NULL, err, sizeof(err));
    free(text);
    if (again == NULL) {
        scnManifestFree(first);
        UT_FAIL("what was written would not parse: %s", err);
    }

    rc = triggerManifestIsRight(scnManifestValues(again));
    if (rc == 0 &&
        memcmp(scnManifestValues(first)->triggers,
               scnManifestValues(again)->triggers,
               sizeof(ScnTrigger) * 2) != 0) {
        fprintf(stderr, "FAIL %s:%d: the round trip changed the triggers\n",
                __FILE__, __LINE__);
        rc = 1;
    }
    scnManifestFree(first);
    scnManifestFree(again);
    if (rc != 0) {
        return rc;
    }

    /* ── A triggers key that is not a list of objects ─────────────── */

    sink = (ScnValidateResult *)malloc(sizeof(*sink));
    UT_ASSERT(sink != NULL);
    memset(sink, 0, sizeof(*sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = sink;

    first = parseText("{ \"manifest\": 1, \"triggers\": 7 }", &rep, err,
                      sizeof(err));
    if (first == NULL) {
        free(sink);
        UT_FAIL("a triggers key that is not an array was refused: %s", err);
    }
    if (scnManifestValues(first)->numTriggers != 0 ||
        !sawIssue(sink, "triggers")) {
        scnManifestFree(first);
        free(sink);
        UT_FAIL("a triggers key that is not an array was not reported");
    }
    scnManifestFree(first);

    memset(sink, 0, sizeof(*sink));
    first = parseText("{ \"manifest\": 1, \"triggers\": [ 7, \"no\" ] }", &rep,
                      err, sizeof(err));
    if (first == NULL) {
        free(sink);
        UT_FAIL("a triggers entry that is not an object was refused: %s", err);
    }
    if (scnManifestValues(first)->numTriggers != 0 ||
        !sawIssue(sink, "triggers[0]") || !sawIssue(sink, "triggers[1]")) {
        scnManifestFree(first);
        free(sink);
        UT_FAIL("a triggers entry that is not an object was not reported");
    }
    scnManifestFree(first);

    /* ── Each of the four caps ───────────────────────────────────── */

    /* One more trigger than the struct holds. */
    big = (char *)malloc(1 << 16);
    if (big == NULL) {
        free(sink);
        UT_FAIL("out of memory building the over-cap manifest");
    }
    used = (size_t)snprintf(big, 1 << 16, "{ \"manifest\": 1, \"triggers\": [");
    /* Two past the cap, and one line said: a file decides how long this array
       is, and a line for each excess row would fill the issue list. */
    for (i = 0; i <= SCN_TRIGGERS_MAX + 1; i++) {
        used += (size_t)snprintf(big + used, (1 << 16) - used,
                                 "%s{ \"when\": \"on_tick\" }",
                                 (i > 0) ? ", " : "");
    }
    snprintf(big + used, (1 << 16) - used, "] }");

    memset(sink, 0, sizeof(*sink));
    first = parseText(big, &rep, err, sizeof(err));
    if (first == NULL) {
        free(big);
        free(sink);
        UT_FAIL("more triggers than the cap was refused: %s", err);
    }
    if (scnManifestValues(first)->numTriggers != SCN_TRIGGERS_MAX) {
        rc = (int)scnManifestValues(first)->numTriggers;
        scnManifestFree(first);
        free(big);
        free(sink);
        UT_FAIL("%d triggers were kept, expected %d", rc, SCN_TRIGGERS_MAX);
    }
    {
        char want[SCN_VALIDATE_KEY_LEN];
        snprintf(want, sizeof(want), "triggers[%d]", SCN_TRIGGERS_MAX);
        if (!sawIssue(sink, want)) {
            scnManifestFree(first);
            free(big);
            free(sink);
            UT_FAIL("the trigger past the cap was dropped without a word");
        }
        snprintf(want, sizeof(want), "triggers[%d]", SCN_TRIGGERS_MAX + 1);
        if (sawIssue(sink, want) || sink->count != 1) {
            rc = (int)sink->count;
            scnManifestFree(first);
            free(big);
            free(sink);
            UT_FAIL("the cap was said %d times, expected once", rc);
        }
    }
    scnManifestFree(first);

    /* One more test, one more action and one more argument than a trigger
       holds, all on the one trigger. */
    used = (size_t)snprintf(
        big, 1 << 16,
        "{ \"manifest\": 1, \"triggers\": [ { \"when\": \"on_tick\","
        " \"where\": [");
    for (i = 0; i <= SCN_TRIGGER_CONDS_MAX; i++) {
        used += (size_t)snprintf(big + used, (1 << 16) - used,
                                 "%s[\"n\", \"eq\", %d]", (i > 0) ? ", " : "",
                                 i);
    }
    used += (size_t)snprintf(big + used, (1 << 16) - used, "], \"actions\": [");
    for (i = 0; i <= SCN_TRIGGER_ACTIONS_MAX; i++) {
        int j;
        used += (size_t)snprintf(big + used, (1 << 16) - used, "%s[\"log\"",
                                 (i > 0) ? ", " : "");
        /* The first action is the one given more arguments than it holds. */
        for (j = 0; j < ((i == 0) ? SCN_TRIGGER_ARGS_MAX + 1 : 1); j++) {
            used += (size_t)snprintf(big + used, (1 << 16) - used, ", %d", j);
        }
        used += (size_t)snprintf(big + used, (1 << 16) - used, "]");
    }
    snprintf(big + used, (1 << 16) - used, "] } ] }");

    memset(sink, 0, sizeof(*sink));
    first = parseText(big, &rep, err, sizeof(err));
    free(big);
    if (first == NULL) {
        free(sink);
        UT_FAIL("a trigger past its caps was refused: %s", err);
    }
    {
        const ScnTrigger *t = &scnManifestValues(first)->triggers[0];
        bool              ok = true;
        char              want[SCN_VALIDATE_KEY_LEN];

        if (t->numWhere != SCN_TRIGGER_CONDS_MAX) {
            fprintf(stderr, "FAIL %s:%d: %u tests were kept, expected %d\n",
                    __FILE__, __LINE__, (unsigned)t->numWhere,
                    SCN_TRIGGER_CONDS_MAX);
            ok = false;
        }
        if (t->numActions != SCN_TRIGGER_ACTIONS_MAX) {
            fprintf(stderr, "FAIL %s:%d: %u actions were kept, expected %d\n",
                    __FILE__, __LINE__, (unsigned)t->numActions,
                    SCN_TRIGGER_ACTIONS_MAX);
            ok = false;
        }
        if (t->actions[0].numArgs != SCN_TRIGGER_ARGS_MAX) {
            fprintf(stderr, "FAIL %s:%d: %u arguments were kept, expected %d\n",
                    __FILE__, __LINE__, (unsigned)t->actions[0].numArgs,
                    SCN_TRIGGER_ARGS_MAX);
            ok = false;
        }
        snprintf(want, sizeof(want), "triggers[0].where[%d]",
                 SCN_TRIGGER_CONDS_MAX);
        if (!sawIssue(sink, want)) {
            fprintf(stderr, "FAIL %s:%d: the test past the cap said nothing\n",
                    __FILE__, __LINE__);
            ok = false;
        }
        snprintf(want, sizeof(want), "triggers[0].actions[%d]",
                 SCN_TRIGGER_ACTIONS_MAX);
        if (!sawIssue(sink, want)) {
            fprintf(stderr,
                    "FAIL %s:%d: the action past the cap said nothing\n",
                    __FILE__, __LINE__);
            ok = false;
        }
        if (!sawIssue(sink, "triggers[0].actions[0]")) {
            fprintf(stderr,
                    "FAIL %s:%d: the argument past the cap said nothing\n",
                    __FILE__, __LINE__);
            ok = false;
        }
        scnManifestFree(first);
        if (!ok) {
            free(sink);
            return 1;
        }
    }

    free(sink);
    return 0;
}

/* ── A where that is not an array ─────────────────────────────────── */

/* A trigger holding no tests runs on every occurrence of its hook, so a
   where that is there and is not an array cannot leave the trigger
   standing: what the file states as a conditional would run
   unconditionally. The trigger goes with its tests, and the issue is filed
   under the position the file wrote it at.

   Three triggers: a bad one, a sound one, and a bad one. The sound one is
   what says the drop costs that trigger and no more. The second bad one is
   what says which number the issue carries — with the first already gone it
   sits in slot 1 and at position 2, so the two possible keys are different
   numbers and the case can tell them apart. A file whose only bad trigger
   is the first cannot: both are 0. */
int run_scenario_manifest_json_trigger_where_type(void) {
    static const char kBadWhere[] =
        "{\n"
        "  \"manifest\": 1,\n"
        "  \"api\": 1,\n"
        "  \"triggers\": [\n"
        "    { \"when\": \"on_tick\", \"where\": {},\n"
        "      \"actions\": [ [\"log\", \"never\"] ] },\n"
        "    { \"when\": \"on_player_join\",\n"
        "      \"actions\": [ [\"log\", \"kept\"] ] },\n"
        "    { \"when\": \"on_tick\", \"where\": {},\n"
        "      \"actions\": [ [\"log\", \"nor this\"] ] }\n"
        "  ]\n"
        "}\n";
    char                    err[256];
    char                    soft[256];
    ScnManifestDoc         *doc;
    ScnValidateResult      *sink;
    ScnParseReport          rep;
    const ScenarioManifest *m;

    sink = (ScnValidateResult *)malloc(sizeof(*sink));
    UT_ASSERT(sink != NULL);
    memset(sink, 0, sizeof(*sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = sink;

    doc = parseText(kBadWhere, &rep, err, sizeof(err));
    if (doc == NULL) {
        free(sink);
        UT_FAIL("a trigger whose where is not an array was refused: %s", err);
    }
    m = scnManifestValues(doc);
    if (m->numTriggers != 1) {
        int kept = (int)m->numTriggers;
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("%d triggers were kept, expected the 1 whose where reads",
                kept);
    }
    if (strcmp(m->triggers[0].when, "on_player_join") != 0) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the trigger kept runs on '%s'", m->triggers[0].when);
    }
    if (!sawIssue(sink, "triggers[0]")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the first trigger was dropped without a word under its "
                "position");
    }
    /* The second drop, keyed by where the file wrote it rather than by the
       slot it would have landed in once the first had gone. */
    if (!sawIssue(sink, "triggers[2]")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the third trigger was dropped without a word under its "
                "position");
    }
    if (sawIssue(sink, "triggers[1]")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("an issue was filed against triggers[1], which is the slot "
                "the third trigger fell in and the position of the one kept");
    }
    scnManifestFree(doc);
    free(sink);
    return 0;
}

/* ── A trigger that names no hook ─────────────────────────────────── */

/* A trigger is put on the router by the hook it names, so one naming none is
   never reached: keeping it spends a slot against the trigger cap and writes
   a dead entry back out on the next pack. The trigger goes, and the issue is
   filed under the position the file wrote it at.

   Both spellings of naming none: a trigger with no when at all, and one
   whose when is a string with nothing in it.

   Three triggers: a bad one, a sound one, and a bad one. The sound one is
   what says the drop costs that trigger and no more. The second bad one is
   what says which number the issue carries — with the first already gone it
   sits in slot 1 and at position 2, so the two possible keys are different
   numbers and the case can tell them apart. The Lua reader is held to the
   same wording under the same key by a case of its own. */
int run_scenario_manifest_json_trigger_no_when(void) {
    static const char kNoWhen[] =
        "{\n"
        "  \"manifest\": 1,\n"
        "  \"api\": 1,\n"
        "  \"triggers\": [\n"
        "    { \"actions\": [ [\"log\", \"never\"] ] },\n"
        "    { \"when\": \"on_player_join\",\n"
        "      \"actions\": [ [\"log\", \"kept\"] ] },\n"
        "    { \"when\": \"\", \"actions\": [ [\"log\", \"nor this\"] ] }\n"
        "  ]\n"
        "}\n";
    char                    err[256];
    char                    soft[256];
    ScnManifestDoc         *doc;
    ScnValidateResult      *sink;
    ScnParseReport          rep;
    const ScenarioManifest *m;

    sink = (ScnValidateResult *)malloc(sizeof(*sink));
    UT_ASSERT(sink != NULL);
    memset(sink, 0, sizeof(*sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = sink;

    doc = parseText(kNoWhen, &rep, err, sizeof(err));
    if (doc == NULL) {
        free(sink);
        UT_FAIL("a trigger naming no hook was refused: %s", err);
    }
    m = scnManifestValues(doc);
    if (m->numTriggers != 1) {
        int kept = (int)m->numTriggers;
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("%d triggers were kept, expected the 1 that names a hook",
                kept);
    }
    if (strcmp(m->triggers[0].when, "on_player_join") != 0) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the trigger kept runs on '%s'", m->triggers[0].when);
    }
    if (!issueSays(sink, "triggers[0]", "names no hook")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the first trigger was dropped without a word under its "
                "position");
    }
    /* The second drop, keyed by where the file wrote it rather than by the
       slot it would have landed in once the first had gone. */
    if (!issueSays(sink, "triggers[2]", "names no hook")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the third trigger was dropped without a word under its "
                "position");
    }
    if (sawIssue(sink, "triggers[1]")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("an issue was filed against triggers[1], which is the slot "
                "the third trigger fell in and the position of the one kept");
    }
    scnManifestFree(doc);
    free(sink);
    return 0;
}

/* ── An action that names no op ───────────────────────────────────── */

/* The editor makes room for an action before the author has said what it
   does, and a pack made before they say it writes the empty name out. An
   empty name is a string, so the reader took it and said nothing, and
   scnCheckTrigAct leaves an action naming no op to this report — between
   them a row reached a server unmentioned and did nothing there.

   Two actions on the one trigger, one naming an op and one naming none, so
   the key says which of the two the issue is about. The Lua reader is held
   to the same wording under the same key by a case of its own. */
int run_scenario_manifest_json_trigger_action_no_op(void) {
    static const char kNoOp[] =
        "{\n"
        "  \"manifest\": 1,\n"
        "  \"api\": 1,\n"
        "  \"triggers\": [\n"
        "    { \"when\": \"on_tick\",\n"
        "      \"actions\": [ [\"log\", \"kept\"], [\"\", \"unsaid\"] ] }\n"
        "  ]\n"
        "}\n";
    char                    err[256];
    char                    soft[256];
    ScnManifestDoc         *doc;
    ScnValidateResult      *sink;
    ScnParseReport          rep;
    const ScenarioManifest *m;

    sink = (ScnValidateResult *)malloc(sizeof(*sink));
    UT_ASSERT(sink != NULL);
    memset(sink, 0, sizeof(*sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = sink;

    doc = parseText(kNoOp, &rep, err, sizeof(err));
    if (doc == NULL) {
        free(sink);
        UT_FAIL("an action naming no op was refused: %s", err);
    }
    if (!issueSays(sink, "triggers[0].actions[1]", "names no op")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("an empty op was taken without a word under the action");
    }
    if (sawIssue(sink, "triggers[0].actions[0]")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the action that names an op was reported against as well");
    }

    /* The row is kept, empty name and all. An issue is not a refusal: the
       trigger stands and the action beside it still runs. */
    m = scnManifestValues(doc);
    if (m->numTriggers != 1 || m->triggers[0].numActions != 2) {
        int kept = (int)m->triggers[0].numActions;
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("%d actions were kept, expected both", kept);
    }
    if (strcmp(m->triggers[0].actions[0].op, "log") != 0 ||
        m->triggers[0].actions[1].op[0] != '\0') {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the two actions came back naming '%s' and '%s'",
                m->triggers[0].actions[0].op, m->triggers[0].actions[1].op);
    }
    scnManifestFree(doc);
    free(sink);
    return 0;
}

/* ── A where-row that names no field ──────────────────────────────── */

/* The same shape one row over. The editor makes room for a test before the
   author has said what it looks at, and a pack made before they say it
   writes the empty name out. An empty name is a string, so the reader took
   it and said nothing, and scnCheckTrigCond leaves a row naming no field to
   this report — between them a test reached a server unmentioned and never
   held.

   Two rows on the one trigger, one naming a field and one naming none, so
   the key says which of the two the issue is about. The Lua reader is held
   to the same wording under the same key by a case of its own. */
int run_scenario_manifest_json_trigger_where_no_field(void) {
    static const char kNoField[] =
        "{\n"
        "  \"manifest\": 1,\n"
        "  \"api\": 1,\n"
        "  \"triggers\": [\n"
        "    { \"when\": \"on_tick\",\n"
        "      \"where\": [ [\"tick\", \"eq\", 1], [\"\", \"eq\", 1] ],\n"
        "      \"actions\": [ [\"log\", \"kept\"] ] }\n"
        "  ]\n"
        "}\n";
    char                    err[256];
    char                    soft[256];
    ScnManifestDoc         *doc;
    ScnValidateResult      *sink;
    ScnParseReport          rep;
    const ScenarioManifest *m;

    sink = (ScnValidateResult *)malloc(sizeof(*sink));
    UT_ASSERT(sink != NULL);
    memset(sink, 0, sizeof(*sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = sink;

    doc = parseText(kNoField, &rep, err, sizeof(err));
    if (doc == NULL) {
        free(sink);
        UT_FAIL("a test naming no field was refused: %s", err);
    }
    if (!issueSays(sink, "triggers[0].where[1]", "names no field")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("an empty field name was taken without a word under the row");
    }
    if (sawIssue(sink, "triggers[0].where[0]")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the row that names a field was reported against as well");
    }

    /* The row is kept, empty name and all. An issue is not a refusal: the
       trigger stands and the test beside it still holds. */
    m = scnManifestValues(doc);
    if (m->numTriggers != 1 || m->triggers[0].numWhere != 2) {
        int kept = (int)m->triggers[0].numWhere;
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("%d tests were kept, expected both", kept);
    }
    if (strcmp(m->triggers[0].where[0].field, "tick") != 0 ||
        m->triggers[0].where[1].field[0] != '\0') {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the two rows came back naming '%s' and '%s'",
                m->triggers[0].where[0].field, m->triggers[0].where[1].field);
    }
    scnManifestFree(doc);
    free(sink);
    return 0;
}

/* ── A line too long for the action's text ────────────────────────── */

/* An argument past the argument slot goes on the action's own text, which is
   wider but is still a fixed width. A line past that width was cut with
   nothing said, so a package could reach a server holding a shorter line than
   its author wrote and no word anywhere about where the rest went.

   It is the same fault as a name past the argument slot and says the same
   sentence, with the other number in it. The Lua reader is held to that
   wording under the same key by a case of its own: a package whose two forms
   were cut differently would not survive scnManifestAgrees.

   The line is built rather than typed, so the cap it is measured against is
   the one the build holds. */
int run_scenario_manifest_json_trigger_text_cut(void) {
    char                    line[SCN_TRIGGER_TEXT_LEN + 32];
    char                    text[SCN_TRIGGER_TEXT_LEN + 256];
    char                    err[256];
    char                    soft[256];
    ScnManifestDoc         *doc;
    ScnValidateResult      *sink;
    ScnParseReport          rep;
    const ScenarioManifest *m;

    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    UT_ASSERT(strlen(line) >= SCN_TRIGGER_TEXT_LEN);

    snprintf(text, sizeof(text),
             "{\n"
             "  \"manifest\": 1,\n"
             "  \"api\": 1,\n"
             "  \"triggers\": [\n"
             "    { \"when\": \"on_tick\",\n"
             "      \"actions\": [ [\"log\", \"%s\"] ] }\n"
             "  ]\n"
             "}\n", line);

    sink = (ScnValidateResult *)malloc(sizeof(*sink));
    UT_ASSERT(sink != NULL);
    memset(sink, 0, sizeof(*sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = sink;

    doc = parseText(text, &rep, err, sizeof(err));
    if (doc == NULL) {
        free(sink);
        UT_FAIL("a line past the action's text was refused: %s", err);
    }
    if (!issueSays(sink, "triggers[0].actions[0][0]", "is cut to fit")) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("a line past the action's text was cut without a word under "
                "the argument");
    }

    /* Cut and kept, not dropped: the action still runs, with as much of the
       line as there is room for. */
    m = scnManifestValues(doc);
    if (m->numTriggers != 1 || m->triggers[0].numActions != 1) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the trigger did not survive the cut");
    }
    if (!m->triggers[0].actions[0].args[0].inText) {
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the line did not move to the action's text");
    }
    if (strlen(m->triggers[0].actions[0].text) != SCN_TRIGGER_TEXT_LEN - 1) {
        int held = (int)strlen(m->triggers[0].actions[0].text);
        scnManifestFree(doc);
        free(sink);
        UT_FAIL("the action's text holds %d bytes", held);
    }
    scnManifestFree(doc);
    free(sink);
    return 0;
}

/* ── An operator that is none of the seven ────────────────────────── */

/* A word the table cannot place, and a row that names no operator at all,
   both read as SCN_TRIG_CMP_UNKNOWN rather than as eq. Reading them as eq
   is what used to make a typo silently test something the author did not
   write, and left the check nothing to refuse.

   The round trip is the other half: an operator the table cannot name is
   written as "", which comes back as UNKNOWN. A row that is wrong stays
   wrong through a file rather than settling into eq on the way. */
int run_scenario_manifest_json_trigger_operator(void) {
    static const char kBadOp[] =
        "{\n"
        "  \"manifest\": 1,\n"
        "  \"api\": 1,\n"
        "  \"triggers\": [\n"
        "    { \"when\": \"on_tick\",\n"
        "      \"where\": [ [\"tick\", \"equalz\", 1],\n"
        "                   [\"tick\", 7, 2],\n"
        "                   [\"tick\", \"eq\", 3] ],\n"
        "      \"actions\": [ [\"log\", \"tick\"] ] }\n"
        "  ]\n"
        "}\n";
    /* Every one of the seven, to say that moving the enum left each word
       reading as the value it always did. */
    static const struct {
        const char    *name;
        ScnTrigCompare op;
    } kSeven[] = {
        { "eq",  SCN_TRIG_CMP_EQ  }, { "ne",  SCN_TRIG_CMP_NE  },
        { "lt",  SCN_TRIG_CMP_LT  }, { "lte", SCN_TRIG_CMP_LTE },
        { "gt",  SCN_TRIG_CMP_GT  }, { "gte", SCN_TRIG_CMP_GTE },
        { "in",  SCN_TRIG_CMP_IN  },
    };
    char               err[256];
    char               soft[256];
    ScnManifestDoc    *first;
    ScnManifestDoc    *again;
    ScnValidateResult *sink;
    ScnParseReport     rep;
    const ScnTrigger  *t;
    char              *text;
    size_t             i;

    /* ── The words, both ways ────────────────────────────────────── */

    for (i = 0; i < sizeof(kSeven) / sizeof(kSeven[0]); i++) {
        const char *name = scnManifestTrigOpName(kSeven[i].op);

        UT_ASSERT_MSG(scnManifestTrigOpFrom(kSeven[i].name) == kSeven[i].op,
                      "'%s' read as %d", kSeven[i].name,
                      (int)scnManifestTrigOpFrom(kSeven[i].name));
        UT_ASSERT_MSG(name != NULL, "%d has no name, expected '%s'",
                      (int)kSeven[i].op, kSeven[i].name);
        UT_ASSERT_MSG(strcmp(name, kSeven[i].name) == 0,
                      "%d named as '%s', expected '%s'", (int)kSeven[i].op,
                      name, kSeven[i].name);
    }
    UT_ASSERT_MSG(scnManifestTrigOpFrom("equalz") == SCN_TRIG_CMP_UNKNOWN,
                  "a word none of the seven match read as %d",
                  (int)scnManifestTrigOpFrom("equalz"));
    UT_ASSERT_MSG(scnManifestTrigOpFrom("") == SCN_TRIG_CMP_UNKNOWN,
                  "the empty word read as %d",
                  (int)scnManifestTrigOpFrom(""));
    UT_ASSERT_MSG(scnManifestTrigOpFrom(NULL) == SCN_TRIG_CMP_UNKNOWN,
                  "no word at all read as %d",
                  (int)scnManifestTrigOpFrom(NULL));
    UT_ASSERT_MSG(scnManifestTrigOpName(SCN_TRIG_CMP_UNKNOWN) == NULL,
                  "an operator the table cannot name was named '%s'",
                  scnManifestTrigOpName(SCN_TRIG_CMP_UNKNOWN));

    /* ── Out of a manifest ───────────────────────────────────────── */

    sink = (ScnValidateResult *)malloc(sizeof(*sink));
    UT_ASSERT(sink != NULL);
    memset(sink, 0, sizeof(*sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = sink;

    first = parseText(kBadOp, &rep, err, sizeof(err));
    if (first == NULL) {
        free(sink);
        UT_FAIL("a manifest with a bad operator was refused: %s", err);
    }
    t = &scnManifestValues(first)->triggers[0];
    if (t->numWhere != 3) {
        scnManifestFree(first);
        free(sink);
        UT_FAIL("%u tests were kept, expected 3", (unsigned)t->numWhere);
    }
    /* The typo. The decode takes the row and keeps the field and the value;
       it is the operator alone that could not be placed. */
    if (t->where[0].op != SCN_TRIG_CMP_UNKNOWN ||
        strcmp(t->where[0].field, "tick") != 0 ||
        t->where[0].value.num != 1.0) {
        scnManifestFree(first);
        free(sink);
        UT_FAIL("'equalz' decoded as operator %d", (int)t->where[0].op);
    }
    /* A row naming no operator at all, which the decode reports and leaves
       as the same value. */
    if (t->where[1].op != SCN_TRIG_CMP_UNKNOWN) {
        scnManifestFree(first);
        free(sink);
        UT_FAIL("a row naming no operator decoded as %d",
                (int)t->where[1].op);
    }
    if (!sawIssue(sink, "triggers[0].where[1]")) {
        scnManifestFree(first);
        free(sink);
        UT_FAIL("a row naming no operator was not reported");
    }
    /* A word the table does know is untouched by any of it, and the decode
       says nothing about the row. */
    if (t->where[2].op != SCN_TRIG_CMP_EQ) {
        scnManifestFree(first);
        free(sink);
        UT_FAIL("'eq' decoded as %d", (int)t->where[2].op);
    }
    if (sawIssue(sink, "triggers[0].where[2]")) {
        scnManifestFree(first);
        free(sink);
        UT_FAIL("a row the decode could take was reported against");
    }

    /* ── And back through a file ─────────────────────────────────── */

    text = scnManifestWrite(first, err, sizeof(err));
    if (text == NULL) {
        scnManifestFree(first);
        free(sink);
        UT_FAIL("the manifest could not be written: %s", err);
    }
    memset(sink, 0, sizeof(*sink));
    again = parseText(text, &rep, err, sizeof(err));
    free(text);
    scnManifestFree(first);
    if (again == NULL) {
        free(sink);
        UT_FAIL("what was written would not parse: %s", err);
    }
    t = &scnManifestValues(again)->triggers[0];
    if (t->where[0].op != SCN_TRIG_CMP_UNKNOWN ||
        t->where[1].op != SCN_TRIG_CMP_UNKNOWN) {
        scnManifestFree(again);
        free(sink);
        UT_FAIL("the round trip made the operators %d and %d",
                (int)t->where[0].op, (int)t->where[1].op);
    }
    if (t->where[2].op != SCN_TRIG_CMP_EQ) {
        scnManifestFree(again);
        free(sink);
        UT_FAIL("the round trip made 'eq' %d", (int)t->where[2].op);
    }
    /* What went out for an operator with no name is a string, so the row
       comes back as one the decode could take whole. The refusal is the
       validator's, once, rather than this reader's a second time. */
    if (sawIssue(sink, "triggers[0].where[0]") ||
        sawIssue(sink, "triggers[0].where[1]")) {
        scnManifestFree(again);
        free(sink);
        UT_FAIL("a written-out unnamed operator came back as a row the "
                "decode could not take");
    }
    scnManifestFree(again);

    free(sink);
    return 0;
}

/* ── What the package may decide ──────────────────────────────────── */

/* The key a package states what it may decide with. Four things it can say:
 * the two words, nothing at all, and something that is neither.
 *
 * Nothing at all is the case that carries the weight. Every package written
 * before the key existed states none, and all of them have to go on reading
 * as the scenario they have always been, so an absent key is not a default
 * chosen for tidiness but the one answer that keeps them working.
 *
 * Something that is neither is reported and read as a scenario rather than
 * refused, for the same reason: the round the package describes is one an
 * author has been running, and a server that cannot read one word of the
 * file should not be the thing that stops it.
 *
 * The write is checked as well as the read, because the kind is one of the
 * fields the host holds the two forms of a package to agreeing on, and a
 * writer that dropped the key would turn every package it wrote into a
 * scenario the next time the file was opened. */
int run_scenario_manifest_json_kind(void) {
    /* The smallest package that parses, with the kind written in by the
       case. Nothing else in it reads on the kind, so the rest is the same
       across all five. */
    static const char *const kFmt =
        "{ \"manifest\": 1, \"api\": 1, \"name\": \"Kinds\"%s }\n";
    static const struct {
        const char     *line;     /* what goes where the kind would */
        ScnManifestKind want;     /* and what the read has to make of it */
        const char     *says;     /* a word the report carries, or NULL for
                                   * a case that is not reported at all */
    } kCases[] = {
        { ", \"kind\": \"scenario\"", scnKindScenario, NULL },
        { ", \"kind\": \"mod\"", scnKindKeepsWinCondition, NULL },
        { "", scnKindScenario, NULL },
        { ", \"kind\": \"banana\"", scnKindScenario, "banana" },
        { ", \"kind\": 7", scnKindScenario, "not a word" },
    };
    size_t i;

    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        char                    text[256];
        char                    err[256];
        char                    soft[256];
        ScnParseReport          rep;
        ScnValidateResult      *sink;
        ScnManifestDoc         *d;
        const ScenarioManifest *m;
        ScnManifestKind         got;
        bool                    said;

        /* The result is a big struct and this is a stack in a loop, so it is
           taken from the heap the way the other reporting cases here take
           it. */
        sink = (ScnValidateResult *)malloc(sizeof(*sink));
        UT_ASSERT(sink != NULL);
        memset(sink, 0, sizeof(*sink));
        soft[0]     = '\0';
        rep.soft    = soft;
        rep.softLen = sizeof(soft);
        rep.sink    = sink;

        snprintf(text, sizeof(text), kFmt, kCases[i].line);
        err[0] = '\0';
        d      = parseText(text, &rep, err, sizeof(err));
        if (d == NULL) {
            free(sink);
            UT_FAIL("case %d was refused outright: %s", (int)i, err);
        }
        m = scnManifestValues(d);
        if (m == NULL) {
            scnManifestFree(d);
            free(sink);
            UT_FAIL("case %d parsed to no values at all", (int)i);
        }
        got  = m->kind;
        said = (kCases[i].says == NULL)
                   ? !sawIssue(sink, "kind")
                   : issueSays(sink, "kind", kCases[i].says);
        scnManifestFree(d);
        free(sink);

        UT_ASSERT_MSG(got == kCases[i].want,
                      "case %d ('%s') read as kind %d, expected %d", (int)i,
                      kCases[i].line, (int)got, (int)kCases[i].want);
        if (kCases[i].says == NULL) {
            UT_ASSERT_MSG(said, "case %d ('%s') was reported against", (int)i,
                          kCases[i].line);
        } else {
            UT_ASSERT_MSG(said,
                          "case %d ('%s') was not reported under 'kind' "
                          "saying '%s'", (int)i, kCases[i].line,
                          kCases[i].says);
        }
    }

    /* And out to text and back, for both words. What the writer writes is
       what the struct holds rather than what the file it came from said, so
       a package that stated no kind comes back stating the scenario the read
       made of it. */
    for (i = 0; i < 2; i++) {
        ScenarioManifest        in;
        char                    err[256];
        char                   *text;
        ScnManifestDoc         *made;
        ScnManifestDoc         *back;
        const ScenarioManifest *m;
        ScnManifestKind         got;
        bool                    wrote;

        memset(&in, 0, sizeof(in));
        in.api  = 1;
        in.kind = (i == 0) ? scnKindScenario : scnKindKeepsWinCondition;
        snprintf(in.name, sizeof(in.name), "Kinds");

        made = scnManifestFromValues(&in, err, sizeof(err));
        UT_ASSERT_MSG(made != NULL, "a doc could not be built: %s", err);
        text = scnManifestWrite(made, err, sizeof(err));
        scnManifestFree(made);
        UT_ASSERT_MSG(text != NULL, "the manifest could not be written: %s",
                      err);

        wrote = strstr(text, "\"kind\"") != NULL;
        back  = parseText(text, NULL, err, sizeof(err));
        free(text);
        UT_ASSERT_MSG(wrote, "the writer dropped the key for kind %d",
                      (int)in.kind);
        UT_ASSERT_MSG(back != NULL, "what was written would not parse: %s",
                      err);
        m = scnManifestValues(back);
        got = (m == NULL) ? scnKindUnknown : m->kind;
        scnManifestFree(back);

        UT_ASSERT_MSG(got == in.kind,
                      "kind %d went out and came back as %d", (int)in.kind,
                      (int)got);
    }
    return 0;
}

/* ── The Workshop item ─────────────────────────────────────────────── */

/* Both past 2^53, so a value that went through a double on the way would
   come back with its last digits changed. */
#define MJ_WS_ID     76561198000000001ULL
#define MJ_WS_AUTHOR 76561198000000123ULL

int run_scenario_manifest_workshop_keys(void) {
    /* The smallest package that parses, with the two keys written in by the
       case. */
    static const char *const kFmt =
        "{ \"manifest\": 1, \"api\": 1, \"name\": \"Published\"%s }\n";
    static const struct {
        const char *line;      /* what goes where the keys would */
        uint64_t    id;        /* what the read has to make of it */
        uint64_t    author;
        const char *reported;  /* the key a report is filed under, or NULL
                                * for a case that is not reported */
    } kCases[] = {
        { ", \"workshop_id\": \"76561198000000001\","
          " \"workshop_author\": \"76561198000000123\"",
          MJ_WS_ID, MJ_WS_AUTHOR, NULL },
        { "", 0, 0, NULL },
        { ", \"workshop_id\": \"0\", \"workshop_author\": \"0\"", 0, 0, NULL },
        { ", \"workshop_id\": \"7656x\"", 0, 0, "workshop_id" },
        { ", \"workshop_id\": \"\"", 0, 0, "workshop_id" },
        { ", \"workshop_id\": \"-1\"", 0, 0, "workshop_id" },
        { ", \"workshop_id\": \"18446744073709551616\"", 0, 0, "workshop_id" },
        { ", \"workshop_id\": 76561198000000001", 0, 0, "workshop_id" },
        { ", \"workshop_author\": 5", 0, 0, "workshop_author" },
    };
    size_t i;

    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        char               text[512];
        char               err[256];
        char               soft[256];
        ScnParseReport     rep;
        ScnValidateResult *sink;
        ScnManifestDoc    *d;
        uint64_t           id;
        uint64_t           author;
        bool               said;
        bool               anyWorkshop;

        sink = (ScnValidateResult *)malloc(sizeof(*sink));
        UT_ASSERT(sink != NULL);
        memset(sink, 0, sizeof(*sink));
        soft[0]     = '\0';
        rep.soft    = soft;
        rep.softLen = sizeof(soft);
        rep.sink    = sink;

        snprintf(text, sizeof(text), kFmt, kCases[i].line);
        err[0] = '\0';
        d      = parseText(text, &rep, err, sizeof(err));
        if (d == NULL) {
            free(sink);
            UT_FAIL("case %d was refused outright: %s", (int)i, err);
        }
        id          = scnManifestValues(d)->workshopId;
        author      = scnManifestValues(d)->workshopAuthor;
        anyWorkshop = sawIssue(sink, "workshop_id") ||
                      sawIssue(sink, "workshop_author");
        said        = (kCases[i].reported == NULL)
                          ? !anyWorkshop
                          : sawIssue(sink, kCases[i].reported);
        scnManifestFree(d);
        free(sink);

        UT_ASSERT_MSG(id == kCases[i].id && author == kCases[i].author,
                      "case %d ('%s') read as %llu / %llu", (int)i,
                      kCases[i].line, (unsigned long long)id,
                      (unsigned long long)author);
        UT_ASSERT_MSG(said,
                      "case %d ('%s') was %s", (int)i, kCases[i].line,
                      (kCases[i].reported == NULL)
                          ? "reported against"
                          : "not reported under its key");
    }

    /* Out to text and back with both set: the digits survive, as a string. */
    {
        ScenarioManifest *in;
        char              err[256];
        char             *text;
        ScnManifestDoc   *made;
        ScnManifestDoc   *back;
        bool              asString;
        uint64_t          id;
        uint64_t          author;

        in = (ScenarioManifest *)calloc(1, sizeof(*in));
        UT_ASSERT(in != NULL);
        in->api            = 1;
        in->workshopId     = MJ_WS_ID;
        in->workshopAuthor = MJ_WS_AUTHOR;
        snprintf(in->name, sizeof(in->name), "Published");

        made = scnManifestFromValues(in, err, sizeof(err));
        free(in);
        UT_ASSERT_MSG(made != NULL, "a doc could not be built: %s", err);
        text = scnManifestWrite(made, err, sizeof(err));
        scnManifestFree(made);
        UT_ASSERT_MSG(text != NULL, "the manifest could not be written: %s",
                      err);

        asString = strstr(text, "\"76561198000000001\"") != NULL &&
                   strstr(text, "\"76561198000000123\"") != NULL;
        back = parseText(text, NULL, err, sizeof(err));
        free(text);
        UT_ASSERT_MSG(asString, "the ids were not written as digit strings");
        UT_ASSERT_MSG(back != NULL, "what was written would not parse: %s",
                      err);
        id     = scnManifestValues(back)->workshopId;
        author = scnManifestValues(back)->workshopAuthor;
        scnManifestFree(back);
        UT_ASSERT_MSG(id == MJ_WS_ID && author == MJ_WS_AUTHOR,
                      "the ids came back as %llu / %llu",
                      (unsigned long long)id, (unsigned long long)author);
    }

    /* A manifest that names no item writes no key for one, whether it never
       had one, stated "0", or had one set back to 0. */
    {
        static const char *const kWithId =
            "{ \"manifest\": 1, \"name\": \"Published\","
            " \"workshop_id\": \"76561198000000001\","
            " \"workshop_author\": \"76561198000000123\","
            " \"editor_notes\": \"kept\" }\n";
        static const char *const kZero =
            "{ \"manifest\": 1, \"name\": \"Published\","
            " \"workshop_id\": \"0\" }\n";
        ScenarioManifest *in;
        ScnManifestDoc   *docs[3];
        char              err[256];
        const char       *fault = NULL;
        int               bad   = -1;
        int               k;

        in = (ScenarioManifest *)calloc(1, sizeof(*in));
        UT_ASSERT(in != NULL);
        in->api = 1;
        snprintf(in->name, sizeof(in->name), "Published");
        err[0]  = '\0';
        docs[0] = scnManifestFromValues(in, err, sizeof(err));
        free(in);
        docs[1] = parseText(kZero, NULL, err, sizeof(err));
        docs[2] = parseText(kWithId, NULL, err, sizeof(err));
        scnManifestSetWorkshop(docs[2], 0, 0);

        for (k = 0; k < 3 && fault == NULL; k++) {
            char *text;

            if (docs[k] == NULL) {
                fault = "could not be made";
            } else if ((text = scnManifestWrite(docs[k], err,
                                                sizeof(err))) == NULL) {
                fault = "could not be written";
            } else {
                if (strstr(text, "workshop_") != NULL) {
                    fault = "wrote a workshop_ key for no item";
                } else if (k == 2 && strstr(text, "editor_notes") == NULL) {
                    fault = "lost a key it does not read";
                }
                free(text);
            }
            if (fault != NULL) {
                bad = k;
            }
        }
        for (k = 0; k < 3; k++) {
            scnManifestFree(docs[k]);
        }
        UT_ASSERT_MSG(fault == NULL, "doc %d %s (%s)", bad,
                      fault != NULL ? fault : "", err);
    }
    return 0;
}

int run_scenario_manifest_agrees_workshop(void) {
    ScenarioManifest *fromJson;
    ScenarioManifest *fromLua;
    char              key[SCN_VALIDATE_KEY_LEN];
    char              err[256];
    bool              same;

    fromJson = (ScenarioManifest *)calloc(1, sizeof(*fromJson));
    fromLua  = (ScenarioManifest *)calloc(1, sizeof(*fromLua));
    if (fromJson == NULL || fromLua == NULL) {
        free(fromJson);
        free(fromLua);
        UT_FAIL("no memory for two manifests");
    }
    fillBase(fromJson);
    fillBase(fromLua);
    fromJson->workshopId     = MJ_WS_ID;
    fromJson->workshopAuthor = MJ_WS_AUTHOR;

    /* A table that states neither: the ids were written into the manifest
       after publishing, and the script was never rewritten. */
    same = scnManifestAgrees(fromJson, fromLua, key, sizeof(key), err,
                             sizeof(err));
    if (!same) {
        free(fromJson);
        free(fromLua);
        UT_FAIL("a table stating no id was refused at %s: %s", key, err);
    }

    /* A table that restates both. */
    fromLua->workshopId     = MJ_WS_ID;
    fromLua->workshopAuthor = MJ_WS_AUTHOR;
    same = scnManifestAgrees(fromJson, fromLua, key, sizeof(key), err,
                             sizeof(err));
    if (!same) {
        free(fromJson);
        free(fromLua);
        UT_FAIL("a table restating the ids was refused at %s: %s", key, err);
    }

    /* A table that states a different item. */
    fromLua->workshopId = MJ_WS_ID + 1;
    same = scnManifestAgrees(fromJson, fromLua, key, sizeof(key), err,
                             sizeof(err));
    if (same || strcmp(key, "workshop_id") != 0) {
        free(fromJson);
        free(fromLua);
        UT_FAIL("a different item %s (key '%s')",
                same ? "was agreed with" : "was refused under the wrong key",
                key);
    }

    /* And the author, the same three ways. */
    fromLua->workshopId     = 0;
    fromLua->workshopAuthor = 0;
    same = scnManifestAgrees(fromJson, fromLua, key, sizeof(key), err,
                             sizeof(err));
    if (!same) {
        free(fromJson);
        free(fromLua);
        UT_FAIL("a table stating no author was refused at %s: %s", key, err);
    }
    fromLua->workshopAuthor = MJ_WS_AUTHOR;
    same = scnManifestAgrees(fromJson, fromLua, key, sizeof(key), err,
                             sizeof(err));
    if (!same) {
        free(fromJson);
        free(fromLua);
        UT_FAIL("a table restating the author was refused at %s: %s", key,
                err);
    }
    fromLua->workshopAuthor = MJ_WS_AUTHOR + 1;
    same = scnManifestAgrees(fromJson, fromLua, key, sizeof(key), err,
                             sizeof(err));
    free(fromJson);
    free(fromLua);
    UT_ASSERT_MSG(!same, "a different author was agreed with");
    UT_ASSERT_MSG(strcmp(key, "workshop_author") == 0,
                  "a different author was refused under '%s'", key);
    return 0;
}
