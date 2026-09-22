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
 * run_scenario_packed_map_upload_switch
 *      — a packed map sitting where this server's uploads land attaches with
 *        scripts in uploaded maps on and attaches nothing with them off,
 *        while the same bytes outside that directory attach either way
 * run_scenario_packed_map_team_init
 *      — a container whose team declares an init, a mode and a level, in a
 *        package whose script declares no table, attaches, and all three
 *        reach the template
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
#include "scenario_validate.h"     /* scenarioHostManifest */
#include "scenario_table.h"        /* scnTableGet — the team's init pairs */
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

/* ── 6. The switch for scripts inside uploaded maps ───────────────── */

/* A map a client uploads is written to the server's persist directory under
 * its own name and nothing is stamped on the file, so the path it sits at is
 * the only record of where it came from. This case builds that artefact
 * directly — serverFinishUpload's write, which is a fopen of
 * "<persist dir>/<name>.map" — because the receiving half of the upload is
 * driven by the UDP server's own state and has no seam a unit case can reach.
 *
 * Three attaches off one map: the switch on, the switch off, and the switch
 * off with the same bytes sitting outside the uploads directory, which is
 * what says the refusal is about where the file is and not about the file. */
int run_scenario_packed_map_upload_switch(void) {
    char          uploads[1024];
    char          mapPath[1024];
    char          outside[1024];
    char          loose[1024];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(utScratchPath(uploads, sizeof(uploads), "uploads"));
    (void)SDL_RemovePath(uploads);
    UT_ASSERT_MSG(SDL_CreateDirectory(uploads),
                  "the uploads directory could not be made: %s",
                  SDL_GetError());
    snprintf(mapPath, sizeof(mapPath), "%s/packed_upload.map", uploads);
    UT_ASSERT(utScratchPath(outside, sizeof(outside), "packed_own.map"));

    pmLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);
    UT_ASSERT_MSG(pmWritePacked(mapPath, kPmManifest, kPmRestates),
                  "the uploaded packed map could not be written");
    pmLoosePath(outside, loose, sizeof(loose));
    remove(loose);
    UT_ASSERT_MSG(pmWritePacked(outside, kPmManifest, kPmRestates),
                  "the operator's own packed map could not be written");

    sim = pmSim();
    UT_ASSERT(sim != NULL);
    /* Where this server's uploads land, which is what the switch measures a
       map path against. */
    serverSimSetUploadPersistDir(sim, uploads);
    /* And the lister's question, so the tag the map chooser draws can be held
       against what the attach does with the same file. */
    scenarioHostRegisterMapScripted(sim);
    pmWatchConsole(sim);

    /* On, which is the default: the uploaded map's own scenario runs, and
       the console says the round is being played by a script that came from
       an upload. */
    scenarioHostSetUploadScriptsEnabled(true);
    err[0] = '\0';
    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL,
                  "an uploaded packed map was refused with uploaded scripts "
                  "on: %s", err);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), PM_NAME) == 0,
                  "the uploaded map's scenario is called '%s', expected '%s'",
                  scenarioHostName(h), PM_NAME);
    UT_ASSERT_MSG(strstr(pmSaid, "came from an upload") != NULL,
                  "nothing on the console said the scenario came from an "
                  "upload; it heard: %s", pmSaid);
    UT_ASSERT_MSG(strstr(pmSaid, "packed_upload.map") != NULL,
                  "the console line did not name the file: %s", pmSaid);
    scenarioHostDetach(h);

    /* And the map chooser agrees with the attach while it is on. */
    UT_ASSERT_MSG(serverSimScenarioMapIsScripted(sim, mapPath),
                  "the lister calls an uploaded packed map plain with "
                  "uploaded scripts on");

    /* Off: nothing attaches, and the file that was turned down is named. */
    pmSaid[0] = '\0';
    scenarioHostSetUploadScriptsEnabled(false);
    err[0] = '\0';
    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h == NULL,
                  "an uploaded packed map attached '%s' with uploaded scripts "
                  "off", h != NULL ? scenarioHostName(h) : "");
    UT_ASSERT_MSG(strstr(pmSaid, "packed_upload.map") != NULL,
                  "the refusal did not name the upload on the console: %s",
                  pmSaid);
    UT_ASSERT_MSG(serverSimScenarioMapIsScripted(sim, mapPath) == false,
                  "the lister still tags an uploaded packed map scripted with "
                  "uploaded scripts off");

    /* The same bytes outside the uploads directory are the operator's own
       map and are unaffected. */
    err[0] = '\0';
    h = scenarioHostAttach(sim, outside, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL,
                  "a packed map outside the uploads directory was refused "
                  "with uploaded scripts off: %s", err);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), PM_NAME) == 0,
                  "the operator's own packed map is called '%s', expected "
                  "'%s'", scenarioHostName(h), PM_NAME);
    scenarioHostDetach(h);

    /* Back to the default, because the switch is one answer for the whole
       process and the cases run in sequence. */
    scenarioHostSetUploadScriptsEnabled(true);
    pmUnwatchConsole(sim);
    serverSimDestroy(sim);
    remove(mapPath);
    remove(outside);
    (void)SDL_RemovePath(uploads);
    return 0;
}

/* ── 7. A manifest team with an init, a mode and a level ──────────── */

/* The host pushes the container's manifest into the state as a `scenario`
 * global before the chunk runs, so a packaged script that declares no table
 * of its own is checked against what was pushed. Everything a team carries
 * has to go over. Two rounds of that have been missing: the init, which
 * made a manifest declaring one disagree with the script at every load, and
 * the mode and the level, which went the other way and said nothing — the
 * comparison did not read them either, so the two forms agreed and the team
 * simply reached the template with neither.
 *
 * The script here declares nothing, which is the whole point — the manifest
 * is the only place any of this is written, and the load has to carry it. */
int run_scenario_packed_map_team_init(void) {
    static const char kInitManifest[] =
        "{\n"
        "  \"manifest\": 1,\n"
        "  \"name\": \"" PM_NAME "\",\n"
        "  \"description\": \"" PM_DESC "\",\n"
        "  \"api\": 1,\n"
        "  \"bound\": true,\n"
        "  \"script\": \"main.lua\",\n"
        "  \"lobby\": {\n"
        "    \"teams\": [\n"
        "      { \"id\": 2, \"bots\": 1, \"max_bots\": 1, \"fielded\": false,\n"
        "        \"mode\": \"survival\", \"difficulty\": \"hard\",\n"
        "        \"init\": { \"waves\": \"3\", \"style\": \"rush\" } }\n"
        "    ]\n"
        "  }\n"
        "}\n";

    char                    mapPath[1024];
    char                    loose[1024];
    ServerSim              *sim;
    ScenarioHost           *h;
    char                    err[512];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "packed_init.map"));
    pmLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);                  /* the container is the only script */
    UT_ASSERT_MSG(pmWritePacked(mapPath, kInitManifest, kPmSilent),
                  "the packed map fixture could not be written");

    sim = pmSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL,
                  "a container whose team declares an init was refused: %s",
                  err);
    UT_ASSERT_MSG(scenarioHostIsActive(h), "the host reports no scenario");
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), PM_NAME) == 0,
                  "the scenario is called '%s', expected the package's '%s'",
                  scenarioHostName(h), PM_NAME);
    /* Nothing was reported about the team's init, which is what the refusal
       used to say. */
    UT_ASSERT_MSG(strstr(scenarioHostLastError(h), "init") == NULL,
                  "the load complained about an init: %s",
                  scenarioHostLastError(h));

    /* And the init reached the seat the team holds, so what went over was the
       table rather than an empty stand-in that merely agreed. */
    {
        bool seen = false;
        int  i;

        UT_ASSERT_MSG(sim->scenarioLobbyValid,
                      "the container's lobby never reached the sim");
        for (i = 0; i < (int)sim->scenarioLobby.numTeams; i++) {
            const ScnTable *init = &sim->scenarioLobby.teams[i].init;

            if (sim->scenarioLobby.teams[i].id != 2) continue;
            seen = true;
            UT_ASSERT_MSG(init->count == 2,
                          "the team's init reached the template with %u pairs, "
                          "expected 2", (unsigned)init->count);
            UT_ASSERT_MSG(scnTableGet(init, "waves") != NULL &&
                          strcmp(scnTableGet(init, "waves"), "3") == 0,
                          "the team's init lost 'waves'");
            UT_ASSERT_MSG(scnTableGet(init, "style") != NULL &&
                          strcmp(scnTableGet(init, "style"), "rush") == 0,
                          "the team's init lost 'style'");
            /* And the pair the seating reads to put this team's bots in
               a mode. A team that reaches the template with these empty
               plays whatever the lobby would have given it, which for a
               scenario built around one mode is the wrong game. */
            UT_ASSERT_MSG(
                strcmp(sim->scenarioLobby.teams[i].mode, "survival") == 0,
                "the team reached the template in mode '%s', expected "
                "'survival'", sim->scenarioLobby.teams[i].mode);
            UT_ASSERT_MSG(
                strcmp(sim->scenarioLobby.teams[i].difficulty, "hard") == 0,
                "the team reached the template at difficulty '%s', "
                "expected 'hard'",
                sim->scenarioLobby.teams[i].difficulty);
        }
        UT_ASSERT_MSG(seen, "the manifest's team never reached the template");
    }

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    remove(mapPath);
    return 0;
}
