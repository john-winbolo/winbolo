/*
 * The map editor writing a scenario out, and the server's own reader reading
 * it back.
 *
 * Both forms go through the same writer: a chunk appended to a map file, and a
 * standalone .scenario file, which is the same container held on its own. What
 * the editor writes is what scenarioHostAttach opens, so these cases stop at
 * the container and the manifest — the reader the server uses, called the way
 * the server calls it — rather than starting a round on it.
 *
 * The editor has no ServerSim and makes none, so nothing here builds one
 * either. The map fixture is a copy of a committed map under data/maps, taken
 * per case under that case's own scratch directory: ctest -j runs cases as
 * separate processes in one directory, so a shared fixture name is a race
 * rather than a fixture.
 *
 * run_editor_pack_mod_round_trip
 *      — a manifest built by hand is written as a mod, and the container reads
 *        back with its name, description, rules, team and script intact, bound
 *        false, and the tags and regions of the bound form it came from gone
 * run_editor_pack_map_round_trip
 *      — the same manifest packed on to a map copy reads back the same way
 *        through scnPackageFindInMap, and the map still parses as a map
 * run_editor_pack_twice_identical
 *      — packing the same map twice gives the same bytes, which is the chunk
 *        writer replacing the container rather than appending a second one
 * run_editor_pack_refuses_unbound
 *      — a manifest that is not built for its map is refused by the map form
 *        with the file untouched: that scenario is a mod
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "bases.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "starts.h"
#include "sim_rules_names.h" /* simRulesRuleName / simRulesRuleCount */

#include "scenario_manifest.h"
#include "scenario_manifest_json.h"
#include "scenario_package.h"

#include "mapeditor_scenario_pack.h"
#include "test_harness.h"

/* The repository, so the fixture is found whatever the working directory is.
 * CMake passes the absolute path; the fallback is the path from the source
 * root, for a run started there. */
#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

/* The map that is copied. Any committed map would do — the manifest below says
 * nothing about the entities on it — and this is the one the engine ships as
 * its own stock map. */
#define EP_SOURCE_MAP "data/maps/Everard Island.map"

/* What the forms hold. */
#define EP_NAME  "Editor Wave"
#define EP_DESC  "Written by the map editor"
#define EP_GAME  "tournament"
#define EP_BRAIN "GoalHunter_1.7"
#define EP_MAX_PLAYERS 6
#define EP_TEAM_ID     2
#define EP_TEAM_BOTS   3
#define EP_TEAM_MAX    5
#define EP_RULE_A_VALUE 7.0
#define EP_RULE_B_VALUE 400.0

/* The script in the pane. It restates the manifest for a reader of the source,
 * which is what an author who wants the table in the file writes; nothing here
 * runs it, and the cases only ask that the bytes survive the container. */
static const char kEpScript[] =
    "scenario = {\n"
    "  name = \"" EP_NAME "\",\n"
    "  description = \"" EP_DESC "\",\n"
    "  api = 1,\n"
    "}\n"
    "function on_setup() end\n";

/* ── Fixtures ─────────────────────────────────────────────────────── */

static bool epReadWhole(const char *path, uint8_t **out, size_t *outLen) {
    FILE    *f;
    long     size;
    uint8_t *buf;
    size_t   got;

    *out    = NULL;
    *outLen = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return false;
    }
    *out    = buf;
    *outLen = got;
    return true;
}

/* One committed map copied under this case's own scratch directory. The path
 * it landed at goes into mapPath. */
static bool epCopyMap(const char *leaf, char *mapPath, size_t mapPathLen) {
    char     source[1024];
    uint8_t *bytes = NULL;
    size_t   len   = 0;
    FILE    *f;
    bool     ok;

    if (!utScratchPath(mapPath, mapPathLen, leaf)) {
        return false;
    }
    snprintf(source, sizeof(source), "%s/%s", WB_REPO_ROOT_DIR, EP_SOURCE_MAP);
    if (!epReadWhole(source, &bytes, &len)) {
        return false;
    }
    f = fopen(mapPath, "wb");
    if (f == NULL) {
        free(bytes);
        return false;
    }
    ok = fwrite(bytes, 1, len, f) == len;
    if (fclose(f) != 0) {
        ok = false;
    }
    free(bytes);
    return ok;
}

/* The two rules the forms set, by index: that is what the manifest holds and
 * what a rule row in the editor edits, and the names the JSON carries are the
 * codec's own business. */
#define EP_RULE_A 0
#define EP_RULE_B 1

/* A manifest the way the forms leave one: built for its map, with a game, a
 * lobby team, two rules — and a tag and a region, which only a bound scenario
 * may carry and which the mod form has to drop. */
static void epFillManifest(ScenarioManifest *m) {
    memset(m, 0, sizeof(*m));

    snprintf(m->name, sizeof(m->name), "%s", EP_NAME);
    snprintf(m->description, sizeof(m->description), "%s", EP_DESC);
    m->api = 1;
    snprintf(m->game, sizeof(m->game), "%s", EP_GAME);
    m->bound      = true;
    m->fillToCaps = true;

    m->lobby.maxPlayers = EP_MAX_PLAYERS;
    m->lobby.extraTeams = false;
    m->lobby.numTeams   = 1;
    m->lobby.teams[0].id      = EP_TEAM_ID;
    m->lobby.teams[0].bots    = EP_TEAM_BOTS;
    m->lobby.teams[0].maxBots = EP_TEAM_MAX;
    m->lobby.teams[0].fielded = false;
    snprintf(m->lobby.teams[0].brain, sizeof(m->lobby.teams[0].brain), "%s",
             EP_BRAIN);

    m->numRules      = 2;
    m->rules[0].rule  = (uint16_t)EP_RULE_A;
    m->rules[0].value = EP_RULE_A_VALUE;
    m->rules[1].rule  = (uint16_t)EP_RULE_B;
    m->rules[1].value = EP_RULE_B_VALUE;

    m->baseTags[1].count = 1;
    snprintf(m->baseTags[1].tag[0], SCN_TAG_LEN, "%s", "keep");
    m->numRegions = 1;
    snprintf(m->regions[0].name, SCN_REGION_NAME_LEN, "%s", "keep");
    m->regions[0].x = 100;
    m->regions[0].y = 100;
    m->regions[0].w = 12;
    m->regions[0].h = 12;
}

/* Where a rule sits in what came back, or -1. The codec writes a rule by name
 * and reads it back by name, so the order is the JSON's rather than the
 * form's. */
static int epFindRule(const ScenarioManifest *m, int rule) {
    int i;

    for (i = 0; i < (int)m->numRules; i++) {
        if (m->rules[i].rule == (uint16_t)rule) {
            return i;
        }
    }
    return -1;
}

/* Everything the forms set, back out of the manifest the reader parsed.
 * bound says which form wrote it: a mod carries bound false and neither the
 * tag nor the region, whatever the form it was written from held. Returns 0
 * when it all survived. */
static int epAssertValues(const ScenarioManifest *m, bool bound) {
    int a;
    int b;

    /* The fixture names two rules by index, and the codec writes a rule by the
       name that index has. */
    UT_ASSERT_MSG(simRulesRuleCount() > EP_RULE_B,
                  "this build has %d rules, so the fixture's two are not both "
                  "real", simRulesRuleCount());

    UT_ASSERT_MSG(strcmp(m->name, EP_NAME) == 0,
                  "the name came back as '%s'", m->name);
    UT_ASSERT_MSG(strcmp(m->description, EP_DESC) == 0,
                  "the description came back as '%s'", m->description);
    UT_ASSERT_MSG(m->api == 1, "api came back as %d", m->api);
    UT_ASSERT_MSG(strcmp(m->game, EP_GAME) == 0,
                  "the game came back as '%s'", m->game);
    UT_ASSERT_MSG(m->bound == bound, "bound came back as %s",
                  m->bound ? "true" : "false");
    UT_ASSERT_MSG(m->fillToCaps, "fill_to_caps came back off");

    UT_ASSERT_MSG(m->lobby.maxPlayers == EP_MAX_PLAYERS,
                  "the lobby seats %d players", (int)m->lobby.maxPlayers);
    UT_ASSERT_MSG(m->lobby.numTeams == 1, "%d teams came back",
                  (int)m->lobby.numTeams);
    UT_ASSERT_MSG(m->lobby.teams[0].id == EP_TEAM_ID, "the team is team %d",
                  (int)m->lobby.teams[0].id);
    UT_ASSERT_MSG(m->lobby.teams[0].bots == EP_TEAM_BOTS,
                  "the team asks for %d bots", (int)m->lobby.teams[0].bots);
    UT_ASSERT_MSG(m->lobby.teams[0].maxBots == EP_TEAM_MAX,
                  "the team caps at %d bots", (int)m->lobby.teams[0].maxBots);
    UT_ASSERT_MSG(!m->lobby.teams[0].fielded, "the team came back fielded");
    UT_ASSERT_MSG(strcmp(m->lobby.teams[0].brain, EP_BRAIN) == 0,
                  "the team's brain came back as '%s'",
                  m->lobby.teams[0].brain);

    UT_ASSERT_MSG(m->numRules == 2, "%d rules came back", (int)m->numRules);
    a = epFindRule(m, EP_RULE_A);
    b = epFindRule(m, EP_RULE_B);
    UT_ASSERT_MSG(a >= 0, "the rule '%s' is not in what came back",
                  simRulesRuleName(EP_RULE_A));
    UT_ASSERT_MSG(b >= 0, "the rule '%s' is not in what came back",
                  simRulesRuleName(EP_RULE_B));
    UT_ASSERT_MSG(m->rules[a].value == EP_RULE_A_VALUE,
                  "the first rule came back as %g", m->rules[a].value);
    UT_ASSERT_MSG(m->rules[b].value == EP_RULE_B_VALUE,
                  "the second rule came back as %g", m->rules[b].value);

    if (bound) {
        UT_ASSERT_MSG(m->baseTags[1].count == 1,
                      "the base's tag did not survive the pack");
        UT_ASSERT_MSG(strcmp(m->baseTags[1].tag[0], "keep") == 0,
                      "the base's tag came back as '%s'",
                      m->baseTags[1].tag[0]);
        UT_ASSERT_MSG(m->numRegions == 1, "%d regions came back",
                      (int)m->numRegions);
    } else {
        /* A mod plays over a map it has never seen, and the validator refuses
           a package with bound false that names either of these. */
        UT_ASSERT_MSG(m->baseTags[1].count == 0,
                      "a mod came back carrying a base tag");
        UT_ASSERT_MSG(m->numRegions == 0,
                      "a mod came back carrying %d regions",
                      (int)m->numRegions);
    }
    return 0;
}

/* ── 1. The mod form ──────────────────────────────────────────────── */

#define MR_CLEANUP()               \
    do {                           \
        free(json);                \
        free(lua);                 \
        free(file);                \
        scnManifestFree(doc);      \
        scnPackageClose(p);        \
    } while (0)

#define MR_ASSERT_MSG(cond, fmt, ...)                   \
    do {                                                \
        if (!(cond)) {                                  \
            MR_CLEANUP();                               \
            UT_FAIL("%s — " fmt, #cond, ##__VA_ARGS__); \
        }                                               \
    } while (0)

int run_editor_pack_mod_round_trip(void) {
    char                    modPath[1024];
    char                    err[ME_SCENARIO_PACK_ERR_LEN];
    ScenarioManifest        m;
    uint8_t                *file    = NULL;
    size_t                  fileLen = 0;
    uint8_t                *json    = NULL;
    size_t                  jsonLen = 0;
    uint8_t                *lua     = NULL;
    size_t                  luaLen  = 0;
    ScnPackage             *p       = NULL;
    ScnManifestDoc         *doc     = NULL;
    const ScenarioManifest *back;

    UT_ASSERT(utScratchPath(modPath, sizeof(modPath), "editor_mod.scenario"));
    remove(modPath);
    epFillManifest(&m);

    err[0] = '\0';
    UT_ASSERT_MSG(meScenarioWriteMod(&m, kEpScript, strlen(kEpScript), modPath,
                                     err, sizeof(err)),
                  "the mod was not written: %s", err);
    /* The form's own manifest is never touched by a write. */
    UT_ASSERT_MSG(m.bound, "the write cleared bound on the form's manifest");
    UT_ASSERT_MSG(m.numRegions == 1,
                  "the write cleared the form's own regions");

    /* A .scenario is the same container the trailer holds, so it opens with
       the same reader and no map to find it in. */
    UT_ASSERT(epReadWhole(modPath, &file, &fileLen));
    p = scnPackageOpen(file, fileLen, err, sizeof(err));
    MR_ASSERT_MSG(p != NULL, "the mod does not open: %s", err);
    MR_ASSERT_MSG(scnPackageEntryCount(p) == 2,
                  "the mod holds %d entries, expected the manifest and the "
                  "script", scnPackageEntryCount(p));
    MR_ASSERT_MSG(scnPackageBrainCount(p) == 0, "%d brains were written",
                  scnPackageBrainCount(p));

    MR_ASSERT_MSG(scnPackageReadEntry(p, SCN_PACKAGE_MANIFEST_ENTRY,
                                      SCN_PACKAGE_MANIFEST_MAX_BYTES, &json,
                                      &jsonLen, err, sizeof(err)),
                  "%s could not be read back: %s", SCN_PACKAGE_MANIFEST_ENTRY,
                  err);
    doc = scnManifestParse(json, jsonLen, NULL, err, sizeof(err));
    MR_ASSERT_MSG(doc != NULL, "the manifest does not parse: %s", err);
    back = scnManifestValues(doc);
    MR_ASSERT_MSG(back != NULL, "the manifest parsed to nothing");
    if (epAssertValues(back, false) != 0) {
        MR_CLEANUP();
        return 1;
    }

    /* And the script went in byte for byte. */
    MR_ASSERT_MSG(scnPackageReadEntry(p, SCN_PACKAGE_SCRIPT_ENTRY,
                                      (size_t)(1024 * 1024), &lua, &luaLen,
                                      err, sizeof(err)),
                  "%s could not be read back: %s", SCN_PACKAGE_SCRIPT_ENTRY,
                  err);
    MR_ASSERT_MSG(luaLen == strlen(kEpScript),
                  "the packed script is %lu bytes and the pane held %lu",
                  (unsigned long)luaLen, (unsigned long)strlen(kEpScript));
    MR_ASSERT_MSG(memcmp(lua, kEpScript, luaLen) == 0,
                  "the packed script is not the script that was written");

    MR_CLEANUP();
    remove(modPath);
    return 0;
}

#undef MR_ASSERT_MSG
#undef MR_CLEANUP

/* ── 2. The map form ──────────────────────────────────────────────── */

#define PR_CLEANUP()          \
    do {                      \
        free(json);           \
        free(lua);            \
        free(file);           \
        scnManifestFree(doc); \
        scnPackageClose(p);   \
    } while (0)

#define PR_ASSERT_MSG(cond, fmt, ...)                   \
    do {                                                \
        if (!(cond)) {                                  \
            PR_CLEANUP();                               \
            UT_FAIL("%s — " fmt, #cond, ##__VA_ARGS__); \
        }                                               \
    } while (0)

int run_editor_pack_map_round_trip(void) {
    char                    mapPath[1024];
    char                    err[ME_SCENARIO_PACK_ERR_LEN];
    ScenarioManifest        m;
    uint8_t                *file     = NULL;
    size_t                  fileLen  = 0;
    uint8_t                *json     = NULL;
    size_t                  jsonLen  = 0;
    uint8_t                *lua      = NULL;
    size_t                  luaLen   = 0;
    const uint8_t          *chunk    = NULL;
    size_t                  chunkLen = 0;
    ScnPackage             *p        = NULL;
    ScnManifestDoc         *doc      = NULL;
    const ScenarioManifest *back;
    map                     mp;
    pillboxes               pb;
    bases                   bs;
    starts                  ss;
    bool                    reread;

    UT_ASSERT_MSG(epCopyMap("editor_pack.map", mapPath, sizeof(mapPath)),
                  "the map fixture could not be copied from %s/%s",
                  WB_REPO_ROOT_DIR, EP_SOURCE_MAP);
    epFillManifest(&m);

    err[0] = '\0';
    UT_ASSERT_MSG(meScenarioPackIntoMap(&m, kEpScript, strlen(kEpScript),
                                        mapPath, err, sizeof(err)),
                  "the map was not packed: %s", err);

    UT_ASSERT(epReadWhole(mapPath, &file, &fileLen));
    UT_ASSERT_MSG(scnPackageFindInMap(file, fileLen, &chunk, &chunkLen),
                  "the packed map carries no container");

    p = scnPackageOpen(chunk, chunkLen, err, sizeof(err));
    PR_ASSERT_MSG(p != NULL, "the container does not open: %s", err);
    PR_ASSERT_MSG(scnPackageReadEntry(p, SCN_PACKAGE_MANIFEST_ENTRY,
                                      SCN_PACKAGE_MANIFEST_MAX_BYTES, &json,
                                      &jsonLen, err, sizeof(err)),
                  "%s could not be read back: %s", SCN_PACKAGE_MANIFEST_ENTRY,
                  err);
    doc = scnManifestParse(json, jsonLen, NULL, err, sizeof(err));
    PR_ASSERT_MSG(doc != NULL, "the manifest does not parse: %s", err);
    back = scnManifestValues(doc);
    PR_ASSERT_MSG(back != NULL, "the manifest parsed to nothing");
    /* Packed on to the map it was written for, so the tag and the region it
       names are its own map's and stay. */
    if (epAssertValues(back, true) != 0) {
        PR_CLEANUP();
        return 1;
    }

    PR_ASSERT_MSG(scnPackageReadEntry(p, SCN_PACKAGE_SCRIPT_ENTRY,
                                      (size_t)(1024 * 1024), &lua, &luaLen,
                                      err, sizeof(err)),
                  "%s could not be read back: %s", SCN_PACKAGE_SCRIPT_ENTRY,
                  err);
    PR_ASSERT_MSG(luaLen == strlen(kEpScript),
                  "the packed script is %lu bytes and the pane held %lu",
                  (unsigned long)luaLen, (unsigned long)strlen(kEpScript));
    PR_ASSERT_MSG(memcmp(lua, kEpScript, luaLen) == 0,
                  "the packed script is not the script that was written");

    PR_CLEANUP();

    /* And it is still a map. Every reader parses the map data and stops at the
       terminator, so a chunk that broke that is the worst this could do. */
    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    reread = mapRead(mapPath, &mp, &pb, &bs, &ss);
    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    UT_ASSERT_MSG(reread, "the packed map no longer reads as a map");

    remove(mapPath);
    return 0;
}

#undef PR_ASSERT_MSG
#undef PR_CLEANUP

/* ── 3. Packing twice gives the same bytes ────────────────────────── */

#define TW_CLEANUP()  \
    do {              \
        free(first);  \
        free(second); \
    } while (0)

#define TW_ASSERT_MSG(cond, fmt, ...)                   \
    do {                                                \
        if (!(cond)) {                                  \
            TW_CLEANUP();                               \
            UT_FAIL("%s — " fmt, #cond, ##__VA_ARGS__); \
        }                                               \
    } while (0)

int run_editor_pack_twice_identical(void) {
    char             mapPath[1024];
    char             err[ME_SCENARIO_PACK_ERR_LEN];
    ScenarioManifest m;
    uint8_t         *first     = NULL;
    size_t           firstLen  = 0;
    uint8_t         *second    = NULL;
    size_t           secondLen = 0;

    UT_ASSERT_MSG(epCopyMap("editor_twice.map", mapPath, sizeof(mapPath)),
                  "the map fixture could not be copied from %s/%s",
                  WB_REPO_ROOT_DIR, EP_SOURCE_MAP);
    epFillManifest(&m);

    err[0] = '\0';
    UT_ASSERT_MSG(meScenarioPackIntoMap(&m, kEpScript, strlen(kEpScript),
                                        mapPath, err, sizeof(err)),
                  "the first pack failed: %s", err);
    UT_ASSERT(epReadWhole(mapPath, &first, &firstLen));

    err[0] = '\0';
    if (!meScenarioPackIntoMap(&m, kEpScript, strlen(kEpScript), mapPath, err,
                               sizeof(err))) {
        TW_CLEANUP();
        UT_FAIL("the second pack failed: %s", err);
    }
    if (!epReadWhole(mapPath, &second, &secondLen)) {
        TW_CLEANUP();
        UT_FAIL("the twice-packed map could not be read back");
    }

    /* The second container replaces the first rather than being written after
       it, which is what keeps a map from growing a chunk per save. */
    TW_ASSERT_MSG(firstLen == secondLen,
                  "one pack gives %lu bytes and two give %lu",
                  (unsigned long)firstLen, (unsigned long)secondLen);
    TW_ASSERT_MSG(memcmp(first, second, firstLen) == 0,
                  "packing twice did not give the same bytes");

    TW_CLEANUP();
    remove(mapPath);
    return 0;
}

#undef TW_ASSERT_MSG
#undef TW_CLEANUP

/* ── 4. The map form refuses a mod ────────────────────────────────── */

/* A chunk on a map is that map's scenario by definition. A manifest that says
 * it is not built for its map is a mod, and a mod is a file of its own. */
int run_editor_pack_refuses_unbound(void) {
    char             mapPath[1024];
    char             err[ME_SCENARIO_PACK_ERR_LEN];
    ScenarioManifest m;
    uint8_t         *before    = NULL;
    size_t           beforeLen = 0;
    uint8_t         *after     = NULL;
    size_t           afterLen  = 0;
    bool             same;

    UT_ASSERT_MSG(epCopyMap("editor_unbound.map", mapPath, sizeof(mapPath)),
                  "the map fixture could not be copied from %s/%s",
                  WB_REPO_ROOT_DIR, EP_SOURCE_MAP);
    UT_ASSERT(epReadWhole(mapPath, &before, &beforeLen));

    epFillManifest(&m);
    m.bound = false;

    err[0] = '\0';
    if (meScenarioPackIntoMap(&m, kEpScript, strlen(kEpScript), mapPath, err,
                              sizeof(err))) {
        free(before);
        UT_FAIL("a manifest with bound false was packed on to a map anyway");
    }
    if (err[0] == '\0') {
        free(before);
        UT_FAIL("the refusal said nothing");
    }

    /* Refused before a byte was written, so the map is the file it was. */
    if (!epReadWhole(mapPath, &after, &afterLen)) {
        free(before);
        UT_FAIL("the map could not be read back after the refusal");
    }
    same = (beforeLen == afterLen) && memcmp(before, after, beforeLen) == 0;
    free(before);
    free(after);
    UT_ASSERT_MSG(same, "the refused pack changed the map file");

    remove(mapPath);
    return 0;
}
