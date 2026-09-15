/*
 * A scenario carried inside the .map file rather than beside it.
 *
 * The host takes its script from the container appended to the map when there
 * is no loose X.scenario.lua next to it, fills the scenario global from the
 * container's manifest before the chunk runs, and holds whatever the chunk
 * left behind against that manifest afterwards.
 *
 * Each case writes its own fixture under its own scratch directory and
 * removes it afterwards: ctest -j runs cases as separate processes, so a
 * shared fixture name is a race rather than a fixture.
 *
 * The .map file here is a synthetic BMAP built byte by byte, the way
 * test_scenario_map_body.c builds one, with a container written after it.
 * scenarioHostAttach only ever uses the map path to find a script, so the
 * file on disk needs to be a map the container can be found at the end of and
 * nothing more; the sims are built from the built-in map as everywhere else.
 *
 * run_scenario_packed_map_script_runs
 *      — a packed map with no loose script attaches, its main.lua runs, and
 *        the manifest the host holds carries the package's name and its rule
 * run_scenario_packed_map_loose_overrides
 *      — a loose script beside a packed map is what runs, the manifest is the
 *        loose script's, and the console says which of the two is playing
 * run_scenario_packed_map_script_omits_table
 *      — a package whose main.lua declares no scenario attaches, and the
 *        host's manifest is the container's
 * run_scenario_packed_map_table_disagrees
 *      — a package whose main.lua restates the table and says something else
 *        is refused, and the refusal names the key
 * run_scenario_packed_map_round_start_keeps_it
 *      — a round started on a packed map whose script omits the table still
 *        gets it in the round's own VM: the round runs and plays under the
 *        package's rule
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "everard_map.h"           /* E_MAP — the map the sims are built from */
#include "gametype.h"              /* gameOpen */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.rules, and the console callback
                                    * the override case watches */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive, which is what
                                           * the console routes through */
#include "scenario_host.h"
#include "scenario_manifest.h"
#include "scenario_package.h"
#include "test_harness.h"

/* ── What the package says it is ──────────────────────────────────── */

#define PM_NAME  "Packed Wave"
#define PM_DESC  "Out of the container"
#define PM_DEATH 400

/* The mark a script prints from its top level, so a case can tell that the
 * chunk in the container actually ran rather than that the pushed table was
 * simply read back. */
#define PM_MARK "packed:ran"

static const char kPmManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"name\": \"" PM_NAME "\",\n"
    "  \"description\": \"" PM_DESC "\",\n"
    "  \"api\": 1,\n"
    "  \"bound\": true,\n"
    "  \"script\": \"main.lua\",\n"
    "  \"rules\": { \"tank_death_ticks\": 400 }\n"
    "}\n";

/* The manifest's own table, written out for a reader of the source. The two
 * agree field for field, which is what the comparison asks of them. */
static const char kPmRestates[] =
    "print(\"" PM_MARK "\")\n"
    "scenario = {\n"
    "  name = \"" PM_NAME "\",\n"
    "  description = \"" PM_DESC "\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  rules = { tank_death_ticks = 400 },\n"
    "}\n";

/* No scenario table at all: what the host pushed is what the chunk leaves
 * behind, and the comparison has nothing to do. */
static const char kPmSilent[] =
    "print(\"" PM_MARK "\")\n"
    "function on_setup() end\n";

/* A table that says something else. name is the first field compared, so it
 * is the key the refusal should name. */
static const char kPmDisagrees[] =
    "scenario = { name = \"Other\" }\n";

/* And a loose script, which says a third thing and sets no rules, so a case
 * can tell which of the two ran from the manifest alone. */
static const char kPmLoose[] =
    "scenario = { name = \"Loose Wave\", api = 1 }\n";

/* ── Fixtures ─────────────────────────────────────────────────────── */

/* A map with entities and two runs in it, built byte by byte so the walk
 * that measures it steps over a datalen rather than meeting the terminator
 * straight away. The entity and run data is filler: nothing decodes it. */
static size_t pmMapBytes(uint8_t *out) {
    size_t n = 0;

    memcpy(out + n, "BMAPBOLO", 8);
    n += 8;
    out[n++] = 1;   /* version */
    out[n++] = 1;   /* one pill */
    out[n++] = 2;   /* two bases */
    out[n++] = 1;   /* one start */
    memset(out + n, 0x11, 5);
    n += 5;
    memset(out + n, 0x22, 12);
    n += 12;
    memset(out + n, 0x33, 3);
    n += 3;

    out[n++] = 10;  /* four header bytes and six of data */
    out[n++] = 3;
    out[n++] = 0;
    out[n++] = 20;
    memset(out + n, 0xAB, 6);
    n += 6;

    out[n++] = 9;   /* four header bytes and five of data */
    out[n++] = 4;
    out[n++] = 0;
    out[n++] = 20;
    memset(out + n, 0xCD, 5);
    n += 5;

    out[n++] = 4;
    out[n++] = 255;
    out[n++] = 255;
    out[n++] = 255;
    return n;
}

/* One map file with a container after it, holding the manifest and the
 * script. This is what --pack will write; there is no writer in the server
 * yet, so the fixture builds its own in memory. */
static bool pmWritePacked(const char *path, const char *manifest,
                          const char *lua) {
    uint8_t         map[128];
    uint8_t        *container = NULL;
    size_t          containerLen = 0;
    size_t          mapLen;
    ScnPackageEntry entries[2];
    char            err[256];
    FILE           *f;
    bool            ok;

    mapLen = pmMapBytes(map);

    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)manifest;
    entries[0].len   = strlen(manifest);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)lua;
    entries[1].len   = strlen(lua);
    if (!scnPackageWrite(entries, 2, &container, &containerLen, err,
                         sizeof(err))) {
        return false;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        free(container);
        return false;
    }
    ok = fwrite(map, 1, mapLen, f) == mapLen &&
         fwrite(container, 1, containerLen, f) == containerLen;
    fclose(f);
    free(container);
    return ok;
}

/* .../X.map is accompanied by .../X.scenario.lua. */
static void pmLoosePath(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);

    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

static bool pmWriteLoose(const char *loosePath, const char *lua) {
    FILE *f = fopen(loosePath, "wb");

    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

/* A sim that has not started, ready to be attached to and then started. */
static ServerSim *pmSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);

    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    /* serverSimConsoleMessage writes through the active sim's callback and
       falls back to stdout when there is none, and creating a sim does not
       make it the active one. The cases below watch that console for a line a
       script printed and for the one the attach writes about an override, and
       both are said before a round has made this sim active. */
    serverSimSetActive(sim);
    return sim;
}

/* ── What the console was told ────────────────────────────────────── */

static void (*pmConsolePrev)(void *ctx, char *msg) = NULL;
static char pmSaid[8192];

static void pmConsoleCb(void *ctx, char *msg) {
    size_t have;
    size_t room;
    size_t n;

    if (pmConsolePrev != NULL) {
        pmConsolePrev(ctx, msg);
    }
    if (msg == NULL) {
        return;
    }
    have = strlen(pmSaid);
    room = sizeof(pmSaid) - 1 - have;
    n    = strlen(msg);
    if (n > room) {
        n = room;
    }
    memcpy(pmSaid + have, msg, n);
    pmSaid[have + n] = '\0';
}

/* Only consoleMessage is replaced, never the ctx beside it, which the sim's
   other callbacks read. */
static void pmWatchConsole(ServerSim *sim) {
    pmSaid[0]     = '\0';
    pmConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = pmConsoleCb;
}

static void pmUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = pmConsolePrev;
    pmConsolePrev = NULL;
}

/* ── 1. The container's script runs ───────────────────────────────── */

int run_scenario_packed_map_script_runs(void) {
    char                    mapPath[1024];
    char                    loose[1024];
    ServerSim              *sim;
    ScenarioHost           *h;
    const ScenarioManifest *m;
    char                    err[512];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "packed_runs.map"));
    pmLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);                  /* nothing beside the map but the map */
    UT_ASSERT_MSG(pmWritePacked(mapPath, kPmManifest, kPmRestates),
                  "the packed map fixture could not be written");

    sim = pmSim();
    UT_ASSERT(sim != NULL);
    pmWatchConsole(sim);

    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the packed map was refused: %s", err);
    UT_ASSERT_MSG(scenarioHostIsActive(h), "the host reports no scenario");

    /* The chunk in the container ran, rather than the pushed table simply
       being read back out of a state nothing touched. */
    UT_ASSERT_MSG(strstr(pmSaid, PM_MARK) != NULL,
                  "the console never heard '%s': the container's script did "
                  "not run", PM_MARK);

    UT_ASSERT_MSG(strcmp(scenarioHostName(h), PM_NAME) == 0,
                  "the scenario is called '%s', expected the package's '%s'",
                  scenarioHostName(h), PM_NAME);
    UT_ASSERT_MSG(strcmp(scenarioHostDescription(h), PM_DESC) == 0,
                  "the description read as '%s'", scenarioHostDescription(h));

    m = scenarioHostManifest(h);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(m->numRules == 1, "%u rules, expected the package's 1",
                  (unsigned)m->numRules);
    UT_ASSERT_MSG(m->rules[0].value == (double)PM_DEATH,
                  "the rule was read as %g, expected %d", m->rules[0].value,
                  (int)PM_DEATH);

    pmUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    remove(mapPath);
    return 0;
}

/* ── 2. A loose script beside a packed map wins ───────────────────── */

int run_scenario_packed_map_loose_overrides(void) {
    char                    mapPath[1024];
    char                    loose[1024];
    ServerSim              *sim;
    ScenarioHost           *h;
    const ScenarioManifest *m;
    char                    err[512];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "packed_loose.map"));
    pmLoosePath(mapPath, loose, sizeof(loose));
    UT_ASSERT_MSG(pmWritePacked(mapPath, kPmManifest, kPmRestates),
                  "the packed map fixture could not be written");
    UT_ASSERT_MSG(pmWriteLoose(loose, kPmLoose),
                  "the loose script fixture could not be written");

    sim = pmSim();
    UT_ASSERT(sim != NULL);
    pmWatchConsole(sim);

    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the loose script was refused: %s", err);

    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Loose Wave") == 0,
                  "the scenario is called '%s', expected the loose script's "
                  "'Loose Wave'", scenarioHostName(h));

    /* And it is the loose script's table all the way down: the container's
       carries a rule and a description, and neither is in play. */
    m = scenarioHostManifest(h);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(m->numRules == 0,
                  "%u rules, expected none: the container's table is in play",
                  (unsigned)m->numRules);
    UT_ASSERT_MSG(m->description[0] == '\0',
                  "the description read as '%s', expected none", m->description);

    /* The operator is told which of the two is running, once. */
    UT_ASSERT_MSG(strstr(pmSaid, "packed into") != NULL,
                  "the console was not told the map's own scenario is not the "
                  "one running");

    /* The chunk that did not run printed nothing. */
    UT_ASSERT_MSG(strstr(pmSaid, PM_MARK) == NULL,
                  "the container's script printed '%s': it ran as well as the "
                  "loose one", PM_MARK);

    pmUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    remove(loose);
    remove(mapPath);
    return 0;
}

/* ── 3. A package whose script declares no table ──────────────────── */

int run_scenario_packed_map_script_omits_table(void) {
    char                    mapPath[1024];
    char                    loose[1024];
    ServerSim              *sim;
    ScenarioHost           *h;
    const ScenarioManifest *m;
    char                    err[512];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "packed_silent.map"));
    pmLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);
    UT_ASSERT_MSG(pmWritePacked(mapPath, kPmManifest, kPmSilent),
                  "the packed map fixture could not be written");

    sim = pmSim();
    UT_ASSERT(sim != NULL);
    pmWatchConsole(sim);

    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL,
                  "a package whose script declares no scenario was refused: %s",
                  err);
    UT_ASSERT_MSG(strstr(pmSaid, PM_MARK) != NULL,
                  "the console never heard '%s': the container's script did "
                  "not run", PM_MARK);

    /* What the host holds is the container's manifest, read back out of the
       state it was pushed on to. */
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), PM_NAME) == 0,
                  "the scenario is called '%s', expected the package's '%s'",
                  scenarioHostName(h), PM_NAME);
    m = scenarioHostManifest(h);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(strcmp(m->description, PM_DESC) == 0,
                  "the description read as '%s'", m->description);
    UT_ASSERT_MSG(m->api == 1, "api read as %d", m->api);
    UT_ASSERT_MSG(m->bound, "bound read as false");
    UT_ASSERT_MSG(m->numRules == 1, "%u rules, expected the package's 1",
                  (unsigned)m->numRules);
    UT_ASSERT_MSG(m->rules[0].value == (double)PM_DEATH,
                  "the rule was read as %g, expected %d", m->rules[0].value,
                  (int)PM_DEATH);

    pmUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    remove(mapPath);
    return 0;
}

/* ── 4. A package whose script disagrees with it ──────────────────── */

int run_scenario_packed_map_table_disagrees(void) {
    char          mapPath[1024];
    char          loose[1024];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "packed_conflict.map"));
    pmLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);
    UT_ASSERT_MSG(pmWritePacked(mapPath, kPmManifest, kPmDisagrees),
                  "the packed map fixture could not be written");

    sim = pmSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h == NULL,
                  "a script that restates the table and says '%s' was loaded "
                  "anyway", scenarioHostName(h));
    UT_ASSERT_MSG(err[0] != '\0', "the refusal said nothing");
    /* The key is what sends an author to the line at fault: the sentence says
       the two differ, and the key says where. */
    UT_ASSERT_MSG(strstr(err, "'name'") != NULL,
                  "the refusal does not name the key: %s", err);

    serverSimDestroy(sim);
    remove(mapPath);
    return 0;
}

/* ── 5. And the round start gets the table too ────────────────────── */

/* A round boots a fresh VM and runs the cached bytes again in it, so a
 * packaged script that declares no table needs the manifest pushed there as
 * well as at the attach. Without it the round would read no scenario table,
 * drop the one the attach found and play classic. */
int run_scenario_packed_map_round_start_keeps_it(void) {
    char          mapPath[1024];
    char          loose[1024];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "packed_round.map"));
    pmLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);
    UT_ASSERT_MSG(pmWritePacked(mapPath, kPmManifest, kPmSilent),
                  "the packed map fixture could not be written");

    sim = pmSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the packed map was refused: %s", err);

    serverSimStartGame(sim);

    /* A round that lost its table reads the host back as having no scenario
       at all, so the name is the whole of the question. */
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), PM_NAME) == 0,
                  "after the round start the scenario is called '%s', expected "
                  "'%s': the round's own VM did not get the table (%s)",
                  scenarioHostName(h), PM_NAME, scenarioHostLastError(h));

    /* And the round is playing under the package's rule, which is the round
       start having applied the table rather than merely held it. */
    UT_ASSERT_MSG(sim->sim.rules.tank_death_ticks == PM_DEATH,
                  "the round's tank_death_ticks is %d, expected the package's "
                  "%d", (int)sim->sim.rules.tank_death_ticks, (int)PM_DEATH);

    /* The round keeps ticking with it. */
    serverSimTick(sim);
    serverSimTick(sim);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), PM_NAME) == 0,
                  "two ticks in the scenario is called '%s'",
                  scenarioHostName(h));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    remove(mapPath);
    return 0;
}
