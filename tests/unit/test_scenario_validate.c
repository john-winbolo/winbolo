/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The scenario validator: a script read in a stub VM and checked against the
 * map, the lobby template and the rule catalogue, each problem reported as a
 * key, a line and a message.
 *
 * Each case writes its own fixture script and removes it afterwards. The
 * names are per-case on purpose: CTest runs cases as separate processes in one
 * directory, so a shared fixture name is a race rather than a fixture.
 *
 * The map file itself is never written. The validator derives the script's
 * name from a map path and reads nothing else from it; the entity counts the
 * tag checks read come from the sim, which is built from the built-in map.
 *
 * run_scenario_validate_clean            — a script with nothing wrong passes
 *                                          and hands back the table it parsed
 * run_scenario_validate_api_too_new      — an api above this server's
 * run_scenario_validate_lobby_shape      — the human cap, a team number used
 *                                          twice, bots above max_bots and a
 *                                          brain this server has not got
 * run_scenario_validate_team_init_reported
 *                                        — a team's init pair that will not
 *                                          fit, named under the team's key
 * run_scenario_validate_unknown_rule     — a key that names no rule
 * run_scenario_validate_rule_out_of_range— a value outside its row's bounds
 * run_scenario_validate_rule_pair        — two values that pass alone and
 *                                          break the pair they share
 * run_scenario_validate_tag_past_map     — an entity index past the map's own
 *                                          count
 * run_scenario_validate_fifth_tag        — a fifth tag on an entity that
 *                                          carries four
 * run_scenario_validate_region_off_map   — a rectangle running off the map
 * run_scenario_validate_bound_false_with_tags
 *                                        — a table that says it is not tied
 *                                          to its map and then tags one
 * run_scenario_validate_syntax_error_line— a chunk that does not load, on the
 *                                          line Lua named
 * run_scenario_validate_lines_point_at_the_key
 *                                        — the line each issue carries is the
 *                                          line its key is written on
 * run_scenario_validate_wave_defense     — the script that ships beside Wave
 *                                          Defense.map, against that map
 * run_scenario_validate_unknown_game     — a game type the engine has no word
 *                                          for, and one it does
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_scenario.h"   /* SCN_RULE_tank_reload_ticks */
#include "everard_map.h"
#include "scenario_host.h"
#include "scenario_manifest.h"
#include "scenario_validate.h"
#include "test_harness.h"

/* Where the maps the baseline runs play are kept, and the scripts beside
 * them. CMake passes the absolute path; the fallback is the path from the
 * source root, for a run started there. */
#ifndef WB_BASELINE_MAPS_DIR
#define WB_BASELINE_MAPS_DIR "tests/baseline/maps"
#endif

/* ── Fixtures ─────────────────────────────────────────────────────── */

/* A script sits beside the map: X.map is accompanied by X.scenario.lua. */
static void svScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

static bool svPut(const char *mapPath, const char *lua) {
    char  path[512];
    FILE *f;
    svScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static void svDrop(const char *mapPath) {
    char path[512];
    svScriptFor(mapPath, path, sizeof(path));
    remove(path);
}

static ServerSim *svSim(void) {
    BYTE emap[6000] = E_MAP;
    return serverSimCreateCompressed(emap, 5097, "Everard Island", gameOpen,
                                     false, 0, -1);
}

/* The issue under that key, or NULL where nothing was reported for it. */
static const ScnValidateIssue *svFind(const ScnValidateResult *r,
                                      const char *key) {
    uint16_t i;
    for (i = 0; i < r->count; i++) {
        if (strcmp(r->issues[i].key, key) == 0) {
            return &r->issues[i];
        }
    }
    return NULL;
}

/* Every issue on one line, for a failure message that says what was actually
 * reported rather than only what was missing. */
static void svList(const ScnValidateResult *r, char *out, size_t outLen) {
    uint16_t i;
    size_t   at = 0;

    out[0] = '\0';
    for (i = 0; i < r->count && at + 1 < outLen; i++) {
        at += (size_t)snprintf(out + at, outLen - at, "[%s:%d %s] ",
                               r->issues[i].key, r->issues[i].line,
                               r->issues[i].message);
    }
}

/* ── 1. A script with nothing wrong ───────────────────────────────── */

int run_scenario_validate_clean(void) {
    static const char *const kMap = "scnval_clean.map";
    static const char *const kLua =
        "scenario = {\n"
        "  name = \"Clean\",\n"
        "  api = 1,\n"
        "  rules = { tank_reload_ticks = 7 },\n"
        "  lobby = {\n"
        "    max_players = 8,\n"
        "    teams = { { id = 1, bots = 2, max_bots = 4 } },\n"
        "  },\n"
        "}\n";
    ServerSim        *sim;
    ScnValidateResult r;
    char              seen[1024];

    UT_ASSERT(svPut(kMap, kLua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    if (!scenarioValidateMap(sim, kMap, &r)) {
        svList(&r, seen, sizeof(seen));
        UT_FAIL("a clean script was refused: %s", seen);
    }
    UT_ASSERT_MSG(r.count == 0, "%u issues against a clean script",
                  (unsigned)r.count);
    UT_ASSERT_MSG(r.haveManifest, "a script that parsed left no manifest");

    /* The table comes back beside the verdict: this is the parse a package is
       written from as well as the one the checks ran against. */
    UT_ASSERT_MSG(strcmp(r.manifest.name, "Clean") == 0,
                  "the manifest name is '%s'", r.manifest.name);
    UT_ASSERT_MSG(r.manifest.api == 1, "the manifest api is %d",
                  r.manifest.api);
    UT_ASSERT_MSG(r.manifest.numRules == 1, "%u rules read",
                  (unsigned)r.manifest.numRules);
    UT_ASSERT(r.manifest.rules[0].rule == (uint16_t)SCN_RULE_tank_reload_ticks);
    UT_ASSERT(r.manifest.rules[0].value == 7.0);
    UT_ASSERT_MSG(r.manifest.lobby.numTeams == 1, "%u teams read",
                  (unsigned)r.manifest.lobby.numTeams);
    UT_ASSERT(r.manifest.lobby.teams[0].bots == 2);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 2. An api this server does not implement ─────────────────────── */

int run_scenario_validate_api_too_new(void) {
    static const char *const kMap = "scnval_api.map";
    char              lua[256];
    ServerSim        *sim;
    ScnValidateResult r;

    snprintf(lua, sizeof(lua), "scenario = { name = \"Future\", api = %d }\n",
             SCENARIO_API_VERSION + 1);
    UT_ASSERT(svPut(kMap, lua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "a script written for a newer server was accepted");
    UT_ASSERT_MSG(svFind(&r, "api") != NULL, "no issue under the api key");
    UT_ASSERT_MSG(r.haveManifest,
                  "a script that parsed left no manifest to read");

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 3. The lobby template ────────────────────────────────────────── */

/* The last two teams are the bound the engine seats: the highest id it takes
   and the first one it drops. Both are written from MAX_TANKS rather than
   spelled out, so the case asks about whatever the engine's range is. */
int run_scenario_validate_lobby_shape(void) {
    static const char *const kMap = "scnval_lobby.map";
    char              lua[512];
    ServerSim        *sim;
    ScnValidateResult r;
    char              seen[1024];

    snprintf(lua, sizeof(lua),
             "scenario = {\n"
             "  api = 1,\n"
             "  lobby = {\n"
             "    max_players = 99,\n"
             "    teams = {\n"
             "      { id = 1, bots = 5, max_bots = 2 },\n"
             "      { id = 1, brain = \"package:\" },\n"
             "      { id = %d },\n"
             "      { id = %d },\n"
             "    },\n"
             "  },\n"
             "}\n",
             MAX_TANKS - 1, MAX_TANKS);
    UT_ASSERT(svPut(kMap, lua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT(!scenarioValidateMap(sim, kMap, &r));
    svList(&r, seen, sizeof(seen));
    UT_ASSERT_MSG(svFind(&r, "lobby.max_players") != NULL,
                  "the human cap above the game's own was accepted: %s", seen);
    UT_ASSERT_MSG(svFind(&r, "lobby.teams[1].bots") != NULL,
                  "bots above max_bots was accepted: %s", seen);
    UT_ASSERT_MSG(svFind(&r, "lobby.teams[2].id") != NULL,
                  "a team number used twice was accepted: %s", seen);
    UT_ASSERT_MSG(svFind(&r, "lobby.teams[2].brain") != NULL,
                  "a brain no server has was accepted: %s", seen);
    UT_ASSERT_MSG(svFind(&r, "lobby.teams[3].id") == NULL,
                  "team %d is one the engine seats and was refused: %s",
                  MAX_TANKS - 1, seen);
    UT_ASSERT_MSG(svFind(&r, "lobby.teams[4].id") != NULL,
                  "team %d is above what the engine seats and was accepted: "
                  "%s", MAX_TANKS, seen);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 3b. An init pair a team could not keep ───────────────────────── */

/* The read of a team's `init` keeps the pairs it can and names the one it
 * stopped on, and this is where an author hears about it. The team's key is
 * 1-based over the teams the file lists, as every other team key here is, so
 * the first team in the file is `lobby.teams[1]`. */
int run_scenario_validate_team_init_reported(void) {
    static const char *const kMap = "scnval_team_init.map";
    char                    lua[512];
    char                    longValue[SCN_TABLE_VALUE_LEN + 8];
    ServerSim              *sim;
    ScnValidateResult       r;
    const ScnValidateIssue *issue;
    char                    seen[1024];

    /* A value six bytes longer than the longest one that fits. */
    memset(longValue, 'x', SCN_TABLE_VALUE_LEN + 6);
    longValue[SCN_TABLE_VALUE_LEN + 6] = '\0';
    snprintf(lua, sizeof(lua),
             "scenario = {\n"
             "  api = 1,\n"
             "  lobby = {\n"
             "    teams = { { id = 1, bots = 1,\n"
             "                init = { toolong = \"%s\" } } },\n"
             "  },\n"
             "}\n", longValue);

    UT_ASSERT(svPut(kMap, lua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "a team whose init pair did not fit was passed");
    svList(&r, seen, sizeof(seen));
    issue = svFind(&r, "lobby.teams[1].init");
    UT_ASSERT_MSG(issue != NULL,
                  "no issue under the first team's init key: %s", seen);
    UT_ASSERT_MSG(strstr(issue->message, "toolong") != NULL,
                  "the message does not name the pair: %s", issue->message);

    /* And the table is still read: the team keeps whatever fitted, which
       here is nothing, rather than the block being dropped. */
    UT_ASSERT_MSG(r.haveManifest, "a script that parsed left no manifest");
    UT_ASSERT_MSG(r.manifest.lobby.numTeams == 1,
                  "%u teams read past the bad pair, expected 1",
                  (unsigned)r.manifest.lobby.numTeams);
    UT_ASSERT_MSG(r.manifest.lobby.teams[0].bots == 1,
                  "the team past the bad pair reads %u bots, expected 1",
                  (unsigned)r.manifest.lobby.teams[0].bots);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 4. A key that names no rule ──────────────────────────────────── */

int run_scenario_validate_unknown_rule(void) {
    static const char *const kMap = "scnval_badkey.map";
    static const char *const kLua =
        "scenario = {\n"
        "  api = 1,\n"
        "  rules = { tank_death_tick = 400 },\n"
        "}\n";
    ServerSim              *sim;
    ScnValidateResult       r;
    const ScnValidateIssue *issue;

    UT_ASSERT(svPut(kMap, kLua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT(!scenarioValidateMap(sim, kMap, &r));
    issue = svFind(&r, "rules.tank_death_tick");
    UT_ASSERT_MSG(issue != NULL, "a key naming no rule was accepted");
    UT_ASSERT_MSG(strstr(issue->message, "tank_death_tick") != NULL,
                  "the message does not name the key: %s", issue->message);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 5. A value outside its rule's range ──────────────────────────── */

int run_scenario_validate_rule_out_of_range(void) {
    static const char *const kMap = "scnval_range.map";
    static const char *const kLua =
        "scenario = {\n"
        "  api = 1,\n"
        "  rules = { tank_reload_ticks = 999 },\n"
        "}\n";
    ServerSim              *sim;
    ScnValidateResult       r;
    const ScnValidateIssue *issue;

    UT_ASSERT(svPut(kMap, kLua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT(!scenarioValidateMap(sim, kMap, &r));
    issue = svFind(&r, "rules.tank_reload_ticks");
    UT_ASSERT_MSG(issue != NULL, "a value past the rule's range was accepted");
    UT_ASSERT_MSG(strstr(issue->message, "0..255") != NULL,
                  "the message does not carry the bounds: %s", issue->message);

    /* Said once, against the rule that carries it. The pass over the whole
       table answers the same range and reports nothing for it. */
    UT_ASSERT_MSG(svFind(&r, "rules") == NULL,
                  "the range was reported against the whole table as well");
    UT_ASSERT_MSG(r.count == 1, "%u issues against one rule out of range",
                  (unsigned)r.count);

    /* The check said what the table would do; it did not do it. */
    {
        double held = 0.0;
        UT_ASSERT(serverSimGetScenarioRule(
            sim, (uint16_t)SCN_RULE_tank_reload_ticks, &held));
        UT_ASSERT_MSG(held != 999.0,
                      "the validator wrote the rule it was asked about");
    }

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 6. A pair broken across two rules ────────────────────────────── */

/* Each of these passes on its own against the table the sim is holding — a
   floor of 20 under the default ceiling of 90, and a ceiling of 10 over the
   default floor of 0 — and together they put the floor above the ceiling. */
int run_scenario_validate_rule_pair(void) {
    static const char *const kMap = "scnval_pair.map";
    static const char *const kLua =
        "scenario = {\n"
        "  api = 1,\n"
        "  rules = { base_full_shells = 10, base_min_shells = 20 },\n"
        "}\n";
    ServerSim              *sim;
    ScnValidateResult       r;
    const ScnValidateIssue *issue;
    char                    seen[1024];

    UT_ASSERT(svPut(kMap, kLua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "two rules that break the pair they share were accepted");
    svList(&r, seen, sizeof(seen));
    issue = svFind(&r, "rules");
    UT_ASSERT_MSG(issue != NULL,
                  "the pair was not reported against the table: %s", seen);
    UT_ASSERT_MSG(strstr(issue->message, "base_min_shells") != NULL,
                  "the message does not name the pair: %s", issue->message);

    /* Neither of them is wrong on its own, so neither is named on its own. */
    UT_ASSERT_MSG(svFind(&r, "rules.base_full_shells") == NULL &&
                      svFind(&r, "rules.base_min_shells") == NULL,
                  "a rule that stands up alone was reported: %s", seen);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 7. A tag on an entity the map does not have ──────────────────── */

int run_scenario_validate_tag_past_map(void) {
    static const char *const kMap = "scnval_tagpast.map";
    char              lua[256];
    char              key[64];
    ServerSim        *sim;
    ScnValidateResult r;
    BYTE              have;

    sim = svSim();
    UT_ASSERT(sim != NULL);
    have = serverSimGetBaseCount(sim);
    UT_ASSERT_MSG(have > 0 && have < MAX_BASES,
                  "the map has %u bases, and this case needs room for one "
                  "more index the list could hold", (unsigned)have);

    snprintf(lua, sizeof(lua),
             "scenario = {\n"
             "  api = 1,\n"
             "  tags = { bases = { [%u] = \"keep\" } },\n"
             "}\n",
             (unsigned)(have + 1));
    UT_ASSERT(svPut(kMap, lua));

    UT_ASSERT(!scenarioValidateMap(sim, kMap, &r));
    snprintf(key, sizeof(key), "tags.bases[%u]", (unsigned)(have + 1));
    UT_ASSERT_MSG(svFind(&r, key) != NULL,
                  "a tag on base %u was accepted against a map with %u",
                  (unsigned)(have + 1), (unsigned)have);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 8. A fifth tag on one entity ─────────────────────────────────── */

int run_scenario_validate_fifth_tag(void) {
    static const char *const kMap = "scnval_fifthtag.map";
    static const char *const kLua =
        "scenario = {\n"
        "  api = 1,\n"
        "  tags = { bases = { [1] = { \"a\", \"b\", \"c\", \"d\", \"e\" } } },\n"
        "}\n";
    ServerSim              *sim;
    ScnValidateResult       r;
    const ScnValidateIssue *issue;

    UT_ASSERT(svPut(kMap, kLua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "a fifth tag went unreported");
    issue = svFind(&r, "tags.bases[1]");
    UT_ASSERT_MSG(issue != NULL, "no issue under the tagged entity's key");
    UT_ASSERT_MSG(strstr(issue->message, "dropped") != NULL,
                  "the message does not say the tag was dropped: %s",
                  issue->message);

    /* The four that fit are still there: one bad line does not cost the
       others. */
    UT_ASSERT_MSG(r.manifest.baseTags[1].count == SCN_TAGS_PER_ENTITY,
                  "%u tags kept on base 1",
                  (unsigned)r.manifest.baseTags[1].count);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 9. A region off the map ──────────────────────────────────────── */

int run_scenario_validate_region_off_map(void) {
    static const char *const kMap = "scnval_region.map";
    static const char *const kLua =
        "scenario = {\n"
        "  api = 1,\n"
        "  regions = { keep = { x = 250, y = 4, w = 10, h = 4 } },\n"
        "}\n";
    ServerSim              *sim;
    ScnValidateResult       r;
    const ScnValidateIssue *issue;

    UT_ASSERT(svPut(kMap, kLua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "a region running off the map was accepted");
    issue = svFind(&r, "regions.keep");
    UT_ASSERT_MSG(issue != NULL, "no issue under the region's own key");
    UT_ASSERT_MSG(strstr(issue->message, "keep") != NULL,
                  "the message does not name the region: %s", issue->message);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 10. Not tied to a map, and naming one ────────────────────────── */

int run_scenario_validate_bound_false_with_tags(void) {
    static const char *const kMap = "scnval_bound.map";
    static const char *const kLua =
        "scenario = {\n"
        "  api = 1,\n"
        "  bound = false,\n"
        "  tags = { bases = { [1] = \"keep\" } },\n"
        "}\n";
    ServerSim        *sim;
    ScnValidateResult r;

    UT_ASSERT(svPut(kMap, kLua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "a table that is not tied to its map tagged one of its "
                  "entities and was accepted");
    UT_ASSERT_MSG(svFind(&r, "bound") != NULL, "no issue under the bound key");

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 11. A chunk that does not load ───────────────────────────────── */

/* The error is on its own line so the line number is not a judgement about
   where a parser decides a bad expression began: line 5 holds a statement that
   cannot be one, and nothing else is wrong with the file. */
int run_scenario_validate_syntax_error_line(void) {
    static const char *const kMap = "scnval_syntax.map";
    static const char *const kLua =
        "-- 1\n"
        "-- 2\n"
        "-- 3\n"
        "-- 4\n"
        "this is not lua\n";
    ServerSim        *sim;
    ScnValidateResult r;

    UT_ASSERT(svPut(kMap, kLua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "a script that does not parse was accepted");
    UT_ASSERT_MSG(r.count == 1, "%u issues against one syntax error",
                  (unsigned)r.count);
    UT_ASSERT_MSG(!r.haveManifest,
                  "a chunk that never ran left a manifest behind");
    UT_ASSERT_MSG(r.issues[0].line == 5,
                  "the error is on line %d, expected the 5 Lua named: %s",
                  r.issues[0].line, r.issues[0].message);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 12. Lines point at the key ───────────────────────────────────── */

int run_scenario_validate_lines_point_at_the_key(void) {
    static const char *const kMap = "scnval_lines.map";
    /* Line by line: 1 scenario, 2 api, 3 rules, 4 the rule, 5 the close,
       6 regions, 7 the region, 8 the close, 9 the close. */
    static const char *const kLua =
        "scenario = {\n"
        "  api = 1,\n"
        "  rules = {\n"
        "    tank_reload_ticks = 999,\n"
        "  },\n"
        "  regions = {\n"
        "    keep = { x = 250, y = 0, w = 10, h = 4 },\n"
        "  },\n"
        "}\n";
    ServerSim              *sim;
    ScnValidateResult       r;
    const ScnValidateIssue *rule;
    const ScnValidateIssue *region;

    UT_ASSERT(svPut(kMap, kLua));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT(!scenarioValidateMap(sim, kMap, &r));

    rule = svFind(&r, "rules.tank_reload_ticks");
    UT_ASSERT_MSG(rule != NULL, "the rule was not reported");
    UT_ASSERT_MSG(rule->line == 4, "the rule is on line %d, expected 4",
                  rule->line);

    region = svFind(&r, "regions.keep");
    UT_ASSERT_MSG(region != NULL, "the region was not reported");
    UT_ASSERT_MSG(region->line == 7, "the region is on line %d, expected 7",
                  region->line);

    serverSimDestroy(sim);
    svDrop(kMap);
    return 0;
}

/* ── 13. The script that ships ────────────────────────────────────── */

/* The one case that writes no fixture of its own: it reads the content beside
   its own map, against a sim built from that map, which is what an author
   running the check on it would get. */
int run_scenario_validate_wave_defense(void) {
    const char       *dir = getenv("WB_BASELINE_MAPS_DIR");
    char              mapPath[512];
    ServerSim        *sim;
    ScnValidateResult r;
    char              seen[1024];

    if (dir == NULL || dir[0] == '\0') {
        dir = WB_BASELINE_MAPS_DIR;
    }
    snprintf(mapPath, sizeof(mapPath), "%s/Wave Defense.map", dir);

    sim = serverSimCreate(mapPath, gameOpen, false, 0, -1);
    if (sim == NULL) {
        UT_FAIL("the map is missing or will not load: %s", mapPath);
    }

    if (!scenarioValidateMap(sim, mapPath, &r)) {
        svList(&r, seen, sizeof(seen));
        UT_FAIL("the script beside %s was refused: %s", mapPath, seen);
    }
    UT_ASSERT_MSG(r.haveManifest, "no script was found beside %s", mapPath);
    UT_ASSERT_MSG(r.count == 0, "%u issues against the shipped script",
                  (unsigned)r.count);
    UT_ASSERT_MSG(strcmp(r.manifest.name, "Wave Defense") == 0,
                  "the script calls itself '%s'", r.manifest.name);

    serverSimDestroy(sim);
    return 0;
}

/* ── 14. A game type the engine has no word for ───────────────────── */

/* The attach reads scenario.game through the word set a spawn op's loadout
   takes and drops anything that set does not hold, so a typo plays strict
   tournament with nothing said. The check has to name the word and the three
   that work, and has to stay quiet for a word that does work. */
int run_scenario_validate_unknown_game(void) {
    static const char *const kBad  = "scnval_game_bad.map";
    static const char *const kGood = "scnval_game_good.map";
    ServerSim               *sim;
    ScnValidateResult        r;
    const ScnValidateIssue  *iss;
    char                     seen[1024];

    UT_ASSERT(svPut(kBad,
        "scenario = { name = \"Typo\", api = 1, game = \"tournement\" }\n"));
    sim = svSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!scenarioValidateMap(sim, kBad, &r),
                  "a game type the engine has no word for was accepted");
    svList(&r, seen, sizeof(seen));
    iss = svFind(&r, "game");
    UT_ASSERT_MSG(iss != NULL, "no issue under the game key: %s", seen);
    UT_ASSERT_MSG(strstr(iss->message, "tournement") != NULL,
                  "the issue does not name the word: %s", iss->message);
    UT_ASSERT_MSG(strstr(iss->message, "open") != NULL &&
                  strstr(iss->message, "tournament") != NULL &&
                  strstr(iss->message, "strict") != NULL,
                  "the issue does not name the three that work: %s",
                  iss->message);

    /* And the same field spelled the way the engine reads it. */
    UT_ASSERT(svPut(kGood,
        "scenario = { name = \"Fine\", api = 1, game = \"tournament\" }\n"));
    if (!scenarioValidateMap(sim, kGood, &r)) {
        svList(&r, seen, sizeof(seen));
        UT_FAIL("a game type the engine does read was refused: %s", seen);
    }

    serverSimDestroy(sim);
    svDrop(kBad);
    svDrop(kGood);
    return 0;
}
