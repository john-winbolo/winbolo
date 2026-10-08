/*
 * A script's details: the rules its manifest sets and what its callbacks do.
 *
 * The manifest's optional callbacks block says, one sentence each, what the
 * script's engine callbacks do. The load keeps the rows that name a callback
 * the engine calls and the script defines, and warns (never refuses) for a
 * defined callback the block does not describe and for a row it drops. The
 * scenario directory packs the kept rows with the file's own rules into one
 * details blob per file (scenario_details.h). A client asks the server for
 * one file's blob by name (PACKET_LOBBY_SCENARIO_DETAILS_REQ) and gets it
 * back on CHANNEL_BULK (BULK_KIND_SCENARIO_DETAILS), and the lobby's details
 * dialog builds the rules table and the "implements" table from that blob
 * alone, working out which script wins a rule from the list order it shows.
 * Each kept row carries its type (Event, Query or Trigger), which the server
 * reads off the catalogue and the script.
 *
 * run_scenario_callbacks_manifest       — a loose script's block keeps the
 *                                         good rows, each with its type,
 *                                         drops an unknown name and a
 *                                         callback the script never defines,
 *                                         and says so, and says which defined
 *                                         callback went undescribed; a
 *                                         trigger counts as defining its hook
 * run_scenario_callbacks_over_cap       — a block past the byte budget keeps
 *                                         the rows that fit, whole, and warns
 * run_scenario_callbacks_json           — a package's manifest.json block is
 *                                         listed the same way
 * run_scenario_details_blob             — the blob's writer, reader and checks
 * run_scenario_details_fetch_dir_mod    — a mod in the scenarios directory,
 *                                         asked for over the real loopback
 *                                         transport under loss, arrives with
 *                                         the bytes the directory read
 * run_scenario_details_fetch_survival   — the shipped Survival map committed:
 * run_scenario_details_fetch_soccer       its own script, which is in no
 *                                         directory, is found by the file
 *                                         name the lobby names it by, and
 *                                         arrives with every row of its
 *                                         callbacks block (Survival 11,
 *                                         Soccer 10); same for Soccer
 * run_scenario_details_fetch_not_found  — a file the server does not know is
 *                                         answered, on the first request,
 *                                         with a not-found the client stops
 *                                         waiting on
 * run_scenario_details_fetch_retry      — a request the server drops because
 *                                         this client's bulk stream is busy
 *                                         is asked again and answered
 * run_scenario_details_fetch_give_up    — a server that answers nothing: the
 *                                         client gives up after its tries,
 *                                         and the next opening of the dialog
 *                                         asks again and gets the answer
 * run_scenario_details_override_order   — two mods setting the same rule,
 *                                         fetched by the client: the lower
 *                                         one loses in either order, and a
 *                                         mod on a server with mods off wins
 *                                         nothing
 * run_scenario_details_reload_map_script — the committed map's own script
 *                                         edited on disk and reloaded: the
 *                                         sim answers the edited rules and
 *                                         callbacks, not the ones the commit
 *                                         read
 *
 * Reads the ClientSim and ServerSim structs directly; the unittests profile
 * permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"        /* the details slots and their tries */
#include "client_connect_state.h"
#include "client_net.h"                 /* clientSimGetConnectState */
#include "server_sim.h"
#include "server_sim_internal.h"        /* the console callback watched here */
#include "server_sim_lifecycle.h"       /* serverSimSetLobbyEnabled, and the
                                           commit the reload case makes */
#include "server_sim_scenario.h"        /* serverSimGetMapScript */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive */
#include "bulk_transfer.h"              /* the filler transfer in the retry case */
#include "transport_udp_server_internal.h" /* udpServer.bulkSend */
#include "scenario_defs.h"              /* ScnDirEntry */
#include "scenario_details.h"
#include "scenario_dir.h"
#include "scenario_host.h"
#include "scenario_package.h"
#include "sim_rules_names.h"            /* simRulesRuleIndex */
#include "everard_map.h"                /* the reload case's sim */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Where the shipped maps and their scripts live. */
#ifndef WB_DATA_MAPS_DIR
#define WB_DATA_MAPS_DIR "data/maps"
#endif

/* ── The directory and the console ────────────────────────────────── */

static char scDir[256];

static void scRemoveTree(const char *path) {
    char **names;
    int    count = 0;
    int    i;

    names = SDL_GlobDirectory(path, "*", 0, &count);
    if (names != NULL) {
        for (i = 0; i < count; i++) {
            char child[512];

            if (names[i] == NULL || names[i][0] == '\0') continue;
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
            remove(child);
        }
        SDL_free(names);
    }
    SDL_RemovePath(path);
}

/* A per-case name: ctest runs the cases as separate processes in one
 * directory. */
static bool scMakeDir(const char *tag) {
    snprintf(scDir, sizeof(scDir), "wbtest_scenario_callbacks_%s", tag);
    scRemoveTree(scDir);
    return SDL_CreateDirectory(scDir);
}

static bool scWrite(const char *name, const void *bytes, size_t len) {
    char  path[512];
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), "%s/%s", scDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    ok = (len == 0) || (fwrite(bytes, 1, len, f) == len);
    fclose(f);
    return ok;
}

static bool scWriteText(const char *name, const char *text) {
    return scWrite(name, text, strlen(text));
}

/* The details scnDirListDetails read beside the row with this file. */
static const ScnDirDetails *scFind(const ScnDirDetails *det, int count,
                                   const char *file) {
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(det[i].file, file) == 0) return &det[i];
    }
    return NULL;
}

static void (*scConsolePrev)(void *ctx, char *msg) = NULL;
static char scSaid[8192];

static void scConsoleCb(void *ctx, char *msg) {
    size_t have;
    size_t room;
    size_t n;

    if (scConsolePrev != NULL) scConsolePrev(ctx, msg);
    if (msg == NULL) return;
    have = strlen(scSaid);
    room = sizeof(scSaid) - 1 - have;
    n    = strlen(msg);
    if (n > room) n = room;
    memcpy(scSaid + have, msg, n);
    scSaid[have + n] = '\0';
}

/* A running sim made active, so serverSimConsoleMessage lands here. */
static ServerSim *scWatch(void) {
    ServerSim *sim = ut_make_running_sim("Host");

    if (sim == NULL) return NULL;
    serverSimSetActive(sim);
    scSaid[0]     = '\0';
    scConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = scConsoleCb;
    return sim;
}

static void scUnwatch(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = scConsolePrev;
    scConsolePrev = NULL;
    serverSimDestroy(sim);
}

/* The callbacks rows of a details blob, as "name/T=text;" rows, where T is
 * E, Q or T for the row's SCN_CB_TYPE_*. */
static int scCallbacks(const uint8_t *blob, size_t len, char *out,
                       size_t outLen) {
    const uint8_t *cb;
    size_t         cbLen;
    size_t         at = 0;
    int            rows;
    int            i;

    out[0] = '\0';
    cb   = scnDetailsCallbacks(blob, len, &cbLen);
    rows = scnCallbacksBlobCount(cb, cbLen);
    for (i = 0; i < rows && at + 1 < outLen; i++) {
        char    name[SCN_CALLBACK_NAME_LEN];
        char    text[SCN_CALLBACK_TEXT_LEN];
        uint8_t type;

        if (!scnCallbacksBlobRow(cb, cbLen, i, &type, name, sizeof(name), text,
                                 sizeof(text)) ||
            type >= SCN_CB_TYPE_COUNT) {
            return -1;
        }
        at += (size_t)snprintf(out + at, outLen - at, "%s/%c=%s;", name,
                               "EQT"[type], text);
    }
    return rows;
}

/* ── 1. A loose script's block ────────────────────────────────────── */

static const char kScScript[] =
    "scenario = {\n"
    "  name = \"Described\", api = 1, kind = \"mod\", bound = false,\n"
    "  rules = { tank_full_shells = 80 },\n"
    "  triggers = {\n"
    "    { when = \"on_pill_captured\", actions = { { \"message\", \"x\" } } },\n"
    "  },\n"
    "  callbacks = {\n"
    "    on_start = \"Lines the teams up.\",\n"
    "    on_pill_captured = \"Says who took it.\",\n"
    "    bogus = \"Not a callback.\",\n"
    "    can_die = \"Never defined.\",\n"
    "    allow_extra_teams = \"Keeps it to two teams.\",\n"
    "  },\n"
    "}\n"
    "function on_start() end\n"
    "function allow_extra_teams() return false end\n"
    "function on_tick(t) end\n";

int run_scenario_callbacks_manifest(void) {
    ScnDirEntry          list[4];
    ScnDirDetails        det[4];
    const ScnDirDetails *e;
    ServerSim           *sim;
    char                 rows[1024];
    double             value = 0.0;
    int                n;
    int                got;

    UT_ASSERT(scMakeDir("manifest"));
    UT_ASSERT(scWriteText("described.lua", kScScript));
    sim = scWatch();
    UT_ASSERT(sim != NULL);

    n = scnDirListDetails(scDir, list, det, 4);
    e = scFind(det, n, "described.lua");
    UT_ASSERT_MSG(e != NULL, "a script with a callbacks block was not listed, "
                  "so a warning became a refusal");

    /* The rules part: the file's own table. */
    UT_ASSERT_MSG(scnDetailsValid(e->bytes, e->len),
                  "the file's details are not a valid blob");
    UT_ASSERT_MSG(scnDetailsRuleCount(e->bytes, e->len) == 1,
                  "%d rules in the details, expected 1",
                  scnDetailsRuleCount(e->bytes, e->len));
    UT_ASSERT(scnDetailsFindRule(e->bytes, e->len,
                                 simRulesRuleIndex("tank_full_shells"),
                                 &value));
    UT_ASSERT_MSG(value == 80.0, "tank_full_shells read as %g", value);

    /* The callbacks part: catalogue order, the two good rows only. */
    got = scCallbacks(e->bytes, e->len, rows, sizeof(rows));
    /* A function hook is an Event, a hook only a trigger defines is a
       Trigger, and a policy is a Query. */
    UT_ASSERT_MSG(got == 3, "%d rows kept: %s", got, rows);
    UT_ASSERT_MSG(strcmp(rows, "on_start/E=Lines the teams up.;"
                               "on_pill_captured/T=Says who took it.;"
                               "allow_extra_teams/Q=Keeps it to two teams.;")
                      == 0,
                  "rows kept: %s", rows);

    /* The warnings, one per problem, on the console. */
    UT_ASSERT_MSG(strstr(scSaid, "does not say what on_tick does") != NULL,
                  "no warning for the undescribed on_tick: %s", scSaid);
    UT_ASSERT_MSG(strstr(scSaid, "bogus") != NULL,
                  "no warning for the unknown name: %s", scSaid);
    UT_ASSERT_MSG(strstr(scSaid, "can_die, which the script never defines")
                      != NULL,
                  "no warning for the undefined can_die: %s", scSaid);
    UT_ASSERT_MSG(strstr(scSaid, "on_pill_captured, which") == NULL,
                  "a trigger's hook was treated as undefined: %s", scSaid);

    scUnwatch(sim);
    scRemoveTree(scDir);
    return 0;
}

/* ── 2. Past the byte budget ──────────────────────────────────────── */

int run_scenario_callbacks_over_cap(void) {
    static const char *const names[] = {
        "on_setup", "on_start", "on_tick", "on_end", "on_lobby",
        "on_player_join", "on_player_leave", "on_chat", "on_ping",
        "on_tank_spawned", "on_tank_killed", "on_lgm_died",
        "on_lgm_landed", "on_team_changed", "on_base_captured",
        "on_base_neutralized", "on_pill_captured", "on_pill_placed",
        "on_pill_picked_up", "on_pill_killed",
    };
    const int count = (int)(sizeof(names) / sizeof(names[0]));
    ScnDirEntry          list[4];
    ScnDirDetails        det[4];
    const ScnDirDetails *e;
    ServerSim           *sim;
    char                 script[8192];
    char                 text[151];
    char                 rows[4096];
    size_t               cbLen;
    size_t               at = 0;
    int                  n;
    int                  got;
    int                  i;

    memset(text, 'w', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    at += (size_t)snprintf(script + at, sizeof(script) - at,
                           "scenario = { name = \"Wordy\", api = 1,\n"
                           "  callbacks = {\n");
    for (i = 0; i < count; i++) {
        at += (size_t)snprintf(script + at, sizeof(script) - at,
                               "    %s = \"%s\",\n", names[i], text);
    }
    at += (size_t)snprintf(script + at, sizeof(script) - at, "  },\n}\n");
    for (i = 0; i < count; i++) {
        at += (size_t)snprintf(script + at, sizeof(script) - at,
                               "function %s() end\n", names[i]);
    }
    UT_ASSERT(at < sizeof(script));

    UT_ASSERT(scMakeDir("over_cap"));
    UT_ASSERT(scWriteText("wordy.lua", script));
    sim = scWatch();
    UT_ASSERT(sim != NULL);

    n = scnDirListDetails(scDir, list, det, 4);
    e = scFind(det, n, "wordy.lua");
    UT_ASSERT(e != NULL);
    (void)scnDetailsCallbacks(e->bytes, e->len, &cbLen);
    got = scCallbacks(e->bytes, e->len, rows, sizeof(rows));
    /* Each row costs 3 + name + 150; twelve fit in 2048 and twenty do
       not. */
    UT_ASSERT_MSG(got >= 12 && got < count, "%d rows kept", got);
    UT_ASSERT_MSG(cbLen <= SCN_CALLBACKS_BLOB_MAX,
                  "the kept block is %u bytes, past the %d budget",
                  (unsigned)cbLen, SCN_CALLBACKS_BLOB_MAX);
    UT_ASSERT_MSG(strstr(scSaid, "dropped") != NULL,
                  "rows were dropped without a warning: %s", scSaid);

    scUnwatch(sim);
    scRemoveTree(scDir);
    return 0;
}

/* ── 3. A package's manifest.json ─────────────────────────────────── */

static const char kScManifest[] =
    "{\n"
    "  \"manifest\": 1, \"api\": 1, \"name\": \"Packed\",\n"
    "  \"bound\": false,\n"
    "  \"rules\": { \"speed_road\": 24 },\n"
    "  \"callbacks\": { \"on_setup\": \"Deals the bases.\",\n"
    "                  \"on_tick\": \"Counts down.\" },\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

int run_scenario_callbacks_json(void) {
    ScnPackageEntry      entries[2];
    ScnDirEntry          list[4];
    ScnDirDetails        det[4];
    const ScnDirDetails *e;
    uint8_t           *bytes = NULL;
    size_t             len   = 0;
    char               err[256];
    char               rows[512];
    double             value = 0.0;
    int                n;
    int                got;
    static const char  script[] = "function on_setup() end\n"
                                  "function on_tick(t) end\n";

    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)kScManifest;
    entries[0].len   = strlen(kScManifest);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)script;
    entries[1].len   = strlen(script);
    err[0] = '\0';
    UT_ASSERT_MSG(scnPackageWrite(entries, 2, &bytes, &len, err, sizeof(err)),
                  "the package could not be written: %s", err);

    UT_ASSERT(scMakeDir("json"));
    UT_ASSERT(scWrite("packed.scenario", bytes, len));
    free(bytes);

    n = scnDirListDetails(scDir, list, det, 4);
    e = scFind(det, n, "packed.scenario");
    UT_ASSERT(e != NULL);
    UT_ASSERT(scnDetailsFindRule(e->bytes, e->len,
                                 simRulesRuleIndex("speed_road"), &value));
    UT_ASSERT_MSG(value == 24.0, "speed_road read as %g", value);
    got = scCallbacks(e->bytes, e->len, rows, sizeof(rows));
    UT_ASSERT_MSG(got == 2 && strcmp(rows, "on_setup/E=Deals the bases.;"
                                           "on_tick/E=Counts down.;") == 0,
                  "rows listed: %s", rows);

    scRemoveTree(scDir);
    return 0;
}

/* ── 4. The blob ──────────────────────────────────────────────────── */

int run_scenario_details_blob(void) {
    uint8_t       blob[SCN_DETAILS_MAX];
    uint8_t       cb[SCN_CALLBACKS_BLOB_MAX];
    uint8_t       buf[SCN_DETAILS_MAX];
    size_t        len;
    size_t        cbLen = 0;
    size_t        got;
    int           rule;
    double        value;
    const uint8_t *tail;

    /* Nothing at all is no bytes, and valid. */
    scnDetailsBegin(blob, &len);
    scnDetailsFinish(blob, &len);
    UT_ASSERT(len == 0);
    UT_ASSERT(scnDetailsValid(blob, 0));
    UT_ASSERT(scnDetailsRuleCount(blob, 0) == 0);

    /* Two rules, then two callbacks. */
    scnDetailsBegin(blob, &len);
    UT_ASSERT(scnDetailsAddRule(blob, sizeof(blob), &len, 3, 80.0));
    UT_ASSERT(scnDetailsAddRule(blob, sizeof(blob), &len, 7, 0.25));
    UT_ASSERT(!scnDetailsAddRule(blob, sizeof(blob), &len, SIM_RULE_COUNT,
                                 1.0));
    UT_ASSERT(scnCallbacksBlobAppend(cb, sizeof(cb), &cbLen,
                                     SCN_CB_TYPE_EVENT, "on_start", "Starts."));
    UT_ASSERT(scnCallbacksBlobAppend(cb, sizeof(cb), &cbLen,
                                     SCN_CB_TYPE_QUERY, "can_die", "Never."));
    /* A type past the list is no row. */
    UT_ASSERT(!scnCallbacksBlobAppend(cb, sizeof(cb), &cbLen,
                                      SCN_CB_TYPE_COUNT, "on_end", "Ends."));
    UT_ASSERT(scnDetailsAddCallbacks(blob, sizeof(blob), &len, cb, cbLen));
    /* A rule after the callbacks would be read as callback bytes. */
    UT_ASSERT(!scnDetailsAddRule(blob, sizeof(blob), &len, 9, 1.0));
    scnDetailsFinish(blob, &len);
    UT_ASSERT(len == 1 + 2 * SCN_DETAILS_RULE_BYTES + cbLen);
    UT_ASSERT(scnDetailsValid(blob, len));
    UT_ASSERT(scnDetailsRuleAt(blob, len, 1, &rule, &value));
    UT_ASSERT(rule == 7 && value == 0.25);
    UT_ASSERT(scnDetailsFindRule(blob, len, 3, &value) && value == 80.0);
    UT_ASSERT(!scnDetailsFindRule(blob, len, 4, &value));
    tail = scnDetailsCallbacks(blob, len, &got);
    UT_ASSERT(got == cbLen && memcmp(tail, cb, cbLen) == 0);
    UT_ASSERT(scnCallbacksBlobCount(tail, got) == 2);
    {
        char    name[SCN_CALLBACK_NAME_LEN];
        char    text[SCN_CALLBACK_TEXT_LEN];
        uint8_t type = 0xFF;

        UT_ASSERT(scnCallbacksBlobRow(tail, got, 1, &type, name, sizeof(name),
                                      text, sizeof(text)));
        UT_ASSERT(type == SCN_CB_TYPE_QUERY && strcmp(name, "can_die") == 0 &&
                  strcmp(text, "Never.") == 0);
        /* A type byte past the list makes the bytes no blob. */
        memcpy(buf, tail, got);
        buf[1] = SCN_CB_TYPE_COUNT;
        UT_ASSERT(scnCallbacksBlobCount(buf, got) == -1);
    }

    /* Bad bytes: a rule past the list, and a cut callbacks tail. */
    buf[0] = 1;
    buf[1] = (uint8_t)SIM_RULE_COUNT;
    memset(buf + 2, 0, 8);
    UT_ASSERT(!scnDetailsValid(buf, 10));
    UT_ASSERT(!scnDetailsValid(blob, len - 1));

    return 0;
}

/* ── 5. Asked for over the wire ───────────────────────────────────── */

#define SC_CONNECT_MAX 2000
#define SC_FETCH_MAX   3000   /* re-asks and bulk resends under loss */

static bool scPredConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The file has an answer: its details, or word that there are none. */
static bool scPredSettled(LoopbackHarness *h, void *user) {
    ClientScnDetailsState st =
        clientSimGetLobbyScenarioDetails(h->cs, (const char *)user, NULL, NULL);

    return st == CLIENT_SCN_DETAILS_FOUND || st == CLIENT_SCN_DETAILS_NONE;
}

/* The lobby names the committed map's own script. */
static bool scPredLobbyNames(LoopbackHarness *h, void *user) {
    return strcmp(clientSimGetLobbyScenarioFileName(h->cs),
                  (const char *)user) == 0;
}

/* A lobby server and a client joined to it over the real loopback transport,
 * with the server's scenarios directory at scDir and a scenario host that
 * follows map commits, the way a hosted lobby is set up. */
static bool scNetStart(LoopbackHarness *h, ScenarioHost **slot,
                       const char *impair, uint64_t seed) {
    *slot = NULL;
    if (!loopbackHarnessStart(h, "Asker", /*lobbyMode*/ true, impair, seed)) {
        return false;
    }
    if (loopbackHarnessPumpUntil(h, SC_CONNECT_MAX, scPredConnected, NULL) <
        0) {
        loopbackHarnessStop(h);
        return false;
    }
    threadsWaitForMutex();
    serverSimSetScenarioDir(h->sim, scDir);
    scenarioHostRegisterScenarioLister(h->sim);
    scenarioHostFollowMap(h->sim, slot);
    threadsReleaseMutex();
    return true;
}

static void scNetStop(LoopbackHarness *h, ScenarioHost **slot) {
    threadsWaitForMutex();
    if (*slot != NULL) scenarioHostDetach(*slot);
    *slot = NULL;
    threadsReleaseMutex();
    loopbackHarnessStop(h);
}

/* Ask for a file the way the details dialog does, and pump until it has an
 * answer. The pump count, or -1 when none came within max pumps. */
static int scFetch(LoopbackHarness *h, const char *file, int max) {
    clientSimLobbyScenarioDetailsWant(h->cs, file);
    return loopbackHarnessPumpUntil(h, max, scPredSettled, (void *)file);
}

/* How many requests the client sent for a file. */
static int scTries(const ClientSim *cs, const char *file) {
    int i;

    for (i = 0; i < LOBBY_SCN_DETAILS_SLOTS; i++) {
        if (cs->lobbyScnDetails[i].state != LOBBY_SCN_DETAILS_EMPTY &&
            strcmp(cs->lobbyScnDetails[i].file, file) == 0) {
            return cs->lobbyScnDetails[i].tries;
        }
    }
    return -1;
}

/* What a server in this process answers for a file, the listen host's path:
 * the length, or -1 for not found. */
static int scDirect(LoopbackHarness *h, const char *file, uint8_t *out,
                    size_t cap) {
    int got;

    threadsWaitForMutex();
    got = serverSimScenarioDetails(h->sim, file, out, cap);
    threadsReleaseMutex();
    return got;
}

int run_scenario_details_fetch_dir_mod(void) {
    LoopbackHarness      h;
    ScenarioHost        *slot;
    ScnDirEntry          list[4];
    ScnDirDetails        det[4];
    const ScnDirDetails *want;
    uint8_t              direct[SCN_DETAILS_MAX];
    const uint8_t       *got;
    size_t               gotLen;
    char                 rows[1024];
    int                  n;
    int                  at;

    UT_ASSERT(scMakeDir("fetch_dir"));
    UT_ASSERT(scWriteText("DetailsProbe.lua", kScScript));
    n    = scnDirListDetails(scDir, list, det, 4);
    want = scFind(det, n, "DetailsProbe.lua");
    UT_ASSERT(want != NULL && want->len > 0);

    UT_ASSERT_MSG(scNetStart(&h, &slot, "loss=5,burst=2", 0xD37A11u),
                  "the harness did not come up");
    at = scFetch(&h, "DetailsProbe.lua", SC_FETCH_MAX);
    fprintf(stderr, "  details fetch (dir mod, loss=5): settled@%d tries=%d\n",
            at, scTries(h.cs, "DetailsProbe.lua"));
    UT_ASSERT_MSG(at >= 0, "no answer within %d pumps", SC_FETCH_MAX);
    UT_ASSERT_MSG(clientSimGetLobbyScenarioDetails(h.cs, "DetailsProbe.lua",
                                                   &got, &gotLen) ==
                      CLIENT_SCN_DETAILS_FOUND,
                  "a mod in the directory was answered as not found");
    UT_ASSERT_MSG(gotLen == want->len && memcmp(got, want->bytes, gotLen) == 0,
                  "the details arrived as %u bytes, not the %u the directory "
                  "read", (unsigned)gotLen, (unsigned)want->len);
    UT_ASSERT(scCallbacks(got, gotLen, rows, sizeof(rows)) == 3);

    /* A server in this process answers the same bytes straight off the
       sim, which is what the listen host's dialog reads. */
    UT_ASSERT(scDirect(&h, "DetailsProbe.lua", direct, sizeof(direct)) ==
              (int)want->len);
    UT_ASSERT(memcmp(direct, want->bytes, want->len) == 0);

    UT_ASSERT(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_CONNECTED);
    scNetStop(&h, &slot);
    scRemoveTree(scDir);
    return 0;
}

/* The committed map's own script, through the whole path the dialog takes:
 * the server resolves the map's script by the name the lobby gives it, the
 * answer crosses the wire, and the client's lookup by that name finds it. */
static int scFetchMapScript(const char *tag, const char *map, int wantRows,
                            const char *wantMethod) {
    LoopbackHarness    h;
    ScenarioHost      *slot;
    char               mapPath[512];
    char               file[SCN_DIR_FILE_LEN];
    const ScnDirEntry *row;
    uint8_t            direct[SCN_DETAILS_MAX];
    const uint8_t     *got;
    const uint8_t     *cb;
    size_t             gotLen;
    size_t             cbLen;
    char               rows[4096];
    int                directLen;
    int                at;
    int                count;
    bool               committed;

    UT_ASSERT(scMakeDir(tag));   /* an empty scenarios directory */
    UT_ASSERT_MSG(scNetStart(&h, &slot, "loss=5,burst=2", 0x5C0CC3Au),
                  "the harness did not come up");

    /* The host commits the map, the way the lobby's map chooser does. */
    snprintf(mapPath, sizeof(mapPath), "%s/%s.map", WB_DATA_MAPS_DIR, map);
    file[0] = '\0';
    threadsWaitForMutex();
    committed = serverSimReloadMap(h.sim, mapPath);
    row = serverSimGetMapScript(h.sim);
    if (row != NULL) SDL_strlcpy(file, row->file, sizeof(file));
    threadsReleaseMutex();
    UT_ASSERT_MSG(committed, "the lobby would not commit %s", mapPath);
    UT_ASSERT_MSG(file[0] != '\0', "%s brought no script of its own", map);

    /* The dialog opens on the file name the lobby names the script by. */
    at = loopbackHarnessPumpUntil(&h, SC_FETCH_MAX, scPredLobbyNames,
                                  (void *)file);
    UT_ASSERT_MSG(at >= 0, "the lobby never named %s (it says \"%s\")", file,
                  clientSimGetLobbyScenarioFileName(h.cs));

    clientSimLobbyScenarioDetailsForget(h.cs);   /* as the dialog opens */
    at = scFetch(&h, clientSimGetLobbyScenarioFileName(h.cs), SC_FETCH_MAX);
    fprintf(stderr, "  details fetch (%s, loss=5): settled@%d tries=%d\n", file,
            at, scTries(h.cs, file));
    UT_ASSERT_MSG(at >= 0, "no answer for %s within %d pumps", file,
                  SC_FETCH_MAX);
    UT_ASSERT_MSG(clientSimGetLobbyScenarioDetails(h.cs, file, &got, &gotLen) ==
                      CLIENT_SCN_DETAILS_FOUND,
                  "the map's own script %s was answered as not found", file);

    cb    = scnDetailsCallbacks(got, gotLen, &cbLen);
    count = scCallbacks(got, gotLen, rows, sizeof(rows));
    UT_ASSERT_MSG(count == wantRows && cb != NULL,
                  "%s arrived with %d callback rows, expected %d: %s", file,
                  count, wantRows, rows);
    UT_ASSERT_MSG(strstr(rows, wantMethod) != NULL,
                  "%s arrived without its %s row: %s", file, wantMethod, rows);

    /* The listen host reads the same bytes straight off the sim. */
    directLen = scDirect(&h, file, direct, sizeof(direct));
    UT_ASSERT(directLen == (int)gotLen);
    UT_ASSERT(memcmp(direct, got, gotLen) == 0);

    UT_ASSERT(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_CONNECTED);
    scNetStop(&h, &slot);
    scRemoveTree(scDir);
    return 0;
}

int run_scenario_details_fetch_survival(void) {
    return scFetchMapScript("fetch_survival", "Survival", 11, "on_tick/E=");
}

int run_scenario_details_fetch_soccer(void) {
    return scFetchMapScript("fetch_soccer", "Soccer", 11, "can_capture/Q=");
}

int run_scenario_details_fetch_not_found(void) {
    LoopbackHarness h;
    ScenarioHost   *slot;
    uint8_t         direct[SCN_DETAILS_MAX];
    int             at;

    UT_ASSERT(scMakeDir("fetch_missing"));
    UT_ASSERT_MSG(scNetStart(&h, &slot, NULL, 0x0F0F0Fu),
                  "the harness did not come up");
    at = scFetch(&h, "NoSuchScript.lua", SC_FETCH_MAX);
    UT_ASSERT_MSG(at >= 0, "no answer within %d pumps", SC_FETCH_MAX);
    UT_ASSERT(clientSimGetLobbyScenarioDetails(h.cs, "NoSuchScript.lua", NULL,
                                               NULL) ==
              CLIENT_SCN_DETAILS_NONE);
    /* An answer, not a timeout: one request, settled before the client
       would have asked again. */
    UT_ASSERT_MSG(scTries(h.cs, "NoSuchScript.lua") == 1 &&
                      at < LOBBY_SCN_DETAILS_TIMEOUT_TICKS,
                  "not-found took %d requests and %d pumps",
                  scTries(h.cs, "NoSuchScript.lua"), at);
    UT_ASSERT(scDirect(&h, "NoSuchScript.lua", direct, sizeof(direct)) == -1);

    scNetStop(&h, &slot);
    scRemoveTree(scDir);
    return 0;
}

int run_scenario_details_fetch_retry(void) {
    /* Past the channel's stream buffer, so the sender stays busy while the
       rest of it waits to be handed over. */
    enum { BIG = 3 * CHANNEL_STREAM_BUF };
    LoopbackHarness  h;
    ScenarioHost    *slot;
    BulkStreamHeader sh;
    uint8_t         *big;
    bool             staged;
    int              at;

    UT_ASSERT(scMakeDir("fetch_retry"));
    UT_ASSERT(scWriteText("RetryProbe.lua", kScScript));
    UT_ASSERT_MSG(scNetStart(&h, &slot, NULL, 0x7E7121u),
                  "the harness did not come up");

    /* Another transfer holds this client's bulk stream, as a map preview or
       a round log would, so the server drops the details request that
       arrives behind it. A kind the client does not know: it reads the body
       off the stream and throws it away. */
    big = (uint8_t *)calloc(1, BIG);
    UT_ASSERT(big != NULL);
    memset(&sh, 0, sizeof(sh));
    sh.kind      = 0xEE;
    sh.gen       = 1;
    sh.totalSize = BIG;
    threadsWaitForMutex();
    staged = bulkSenderBegin(&udpServer.bulkSend[0], &sh, big, BIG);
    threadsReleaseMutex();
    free(big);
    UT_ASSERT_MSG(staged, "the filler transfer was not staged");

    at = scFetch(&h, "RetryProbe.lua", SC_FETCH_MAX);
    fprintf(stderr, "  details fetch (behind a busy stream): settled@%d tries=%d\n",
            at, scTries(h.cs, "RetryProbe.lua"));
    UT_ASSERT_MSG(at >= 0, "no answer within %d pumps", SC_FETCH_MAX);
    UT_ASSERT(clientSimGetLobbyScenarioDetails(h.cs, "RetryProbe.lua", NULL,
                                               NULL) ==
              CLIENT_SCN_DETAILS_FOUND);
    UT_ASSERT_MSG(scTries(h.cs, "RetryProbe.lua") >= 2,
                  "the first request was answered, so nothing was dropped and "
                  "the case tested no retry");

    scNetStop(&h, &slot);
    scRemoveTree(scDir);
    return 0;
}

int run_scenario_details_fetch_give_up(void) {
    LoopbackHarness h;
    ScenarioHost   *slot;
    int             at;

    UT_ASSERT(scMakeDir("fetch_give_up"));
    UT_ASSERT(scWriteText("GiveUpProbe.lua", kScScript));
    UT_ASSERT_MSG(scNetStart(&h, &slot, NULL, 0x61BEu),
                  "the harness did not come up");

    /* The server does not tick, so nothing the client asks is answered:
       every try times out and the client stops waiting, inside the link's
       own dead-server timeout. */
    clientSimLobbyScenarioDetailsWant(h.cs, "GiveUpProbe.lua");
    loopbackHarnessPumpClientOnly(
        &h, 1 + LOBBY_SCN_DETAILS_TRIES * LOBBY_SCN_DETAILS_TIMEOUT_TICKS + 5);
    UT_ASSERT_MSG(clientSimGetLobbyScenarioDetails(h.cs, "GiveUpProbe.lua",
                                                   NULL, NULL) ==
                      CLIENT_SCN_DETAILS_NONE,
                  "the client was still waiting after every try timed out");
    UT_ASSERT(scTries(h.cs, "GiveUpProbe.lua") == LOBBY_SCN_DETAILS_TRIES);

    /* The server catches up on the queued requests; the dialog opens again,
       forgets and asks, and this time gets the answer. */
    loopbackHarnessPumpUntil(&h, 50, NULL, NULL);
    UT_ASSERT(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_CONNECTED);
    clientSimLobbyScenarioDetailsForget(h.cs);
    at = scFetch(&h, "GiveUpProbe.lua", SC_FETCH_MAX);
    UT_ASSERT_MSG(at >= 0, "no answer on the next opening");
    UT_ASSERT(clientSimGetLobbyScenarioDetails(h.cs, "GiveUpProbe.lua", NULL,
                                               NULL) ==
              CLIENT_SCN_DETAILS_FOUND);

    scNetStop(&h, &slot);
    scRemoveTree(scDir);
    return 0;
}

/* ── 6. Who wins a rule ───────────────────────────────────────────── */

static const char kScModA[] =
    "scenario = { name = \"Rules Test A\", api = 1, kind = \"mod\",\n"
    "  bound = false, rules = { tank_full_shells = 80, speed_road = 24 } }\n";
static const char kScModB[] =
    "scenario = { name = \"Rules Test B\", api = 1, kind = \"mod\",\n"
    "  bound = false, rules = { tank_full_shells = 20, speed_grass = 10 } }\n";

int run_scenario_details_override_order(void) {
    LoopbackHarness h;
    ScenarioHost   *slot;
    const uint8_t  *blobs[2];
    size_t          lens[2];
    const uint8_t  *a;
    const uint8_t  *b;
    size_t          aLen;
    size_t          bLen;
    double          value  = 0.0;
    int             shells = simRulesRuleIndex("tank_full_shells");
    int             road   = simRulesRuleIndex("speed_road");

    UT_ASSERT(shells >= 0 && road >= 0);
    UT_ASSERT(scMakeDir("override"));
    UT_ASSERT(scWriteText("RulesTestA.scenario.lua", kScModA));
    UT_ASSERT(scWriteText("RulesTestB.scenario.lua", kScModB));
    UT_ASSERT_MSG(scNetStart(&h, &slot, "loss=5,burst=2", 0x0DDE4u),
                  "the harness did not come up");

    /* Both files reach the client with their own tables: a catalogue row, a
       draft row and a committed row are the same lookup by file name. */
    UT_ASSERT(scFetch(&h, "RulesTestA.scenario.lua", SC_FETCH_MAX) >= 0);
    UT_ASSERT(scFetch(&h, "RulesTestB.scenario.lua", SC_FETCH_MAX) >= 0);
    UT_ASSERT(clientSimGetLobbyScenarioDetails(h.cs, "RulesTestA.scenario.lua",
                                               &a, &aLen) ==
              CLIENT_SCN_DETAILS_FOUND);
    UT_ASSERT(clientSimGetLobbyScenarioDetails(h.cs, "RulesTestB.scenario.lua",
                                               &b, &bLen) ==
              CLIENT_SCN_DETAILS_FOUND);
    UT_ASSERT_MSG(scnDetailsRuleCount(a, aLen) == 2,
                  "Rules Test A arrived with %d rules",
                  scnDetailsRuleCount(a, aLen));
    UT_ASSERT_MSG(scnDetailsRuleCount(b, bLen) == 2,
                  "Rules Test B arrived with %d rules",
                  scnDetailsRuleCount(b, bLen));

    /* A over B: B's shells lose to A's 80, and A loses nothing. */
    blobs[0] = a; lens[0] = aLen;
    blobs[1] = b; lens[1] = bLen;
    UT_ASSERT(scnDetailsRuleWinner(blobs, lens, 1, shells, &value) == 0);
    UT_ASSERT_MSG(value == 80.0, "the winning value read as %g", value);
    UT_ASSERT(scnDetailsRuleWinner(blobs, lens, 0, shells, &value) == -1);
    /* A rule only one of them sets is nobody else's to win. */
    UT_ASSERT(scnDetailsRuleWinner(blobs, lens, 1,
                                   simRulesRuleIndex("speed_grass"),
                                   &value) == -1);

    /* B over A, as a host reordering the draft sees it. */
    blobs[0] = b; lens[0] = bLen;
    blobs[1] = a; lens[1] = aLen;
    UT_ASSERT(scnDetailsRuleWinner(blobs, lens, 1, shells, &value) == 0);
    UT_ASSERT_MSG(value == 20.0, "the winning value read as %g", value);
    UT_ASSERT(scnDetailsRuleWinner(blobs, lens, 1, road, &value) == -1);

    /* Mods off: a mod loads nothing, so it wins nothing. */
    blobs[0] = NULL; lens[0] = 0;
    UT_ASSERT(scnDetailsRuleWinner(blobs, lens, 1, shells, &value) == -1);

    /* A file not on the list is overridden by nothing. */
    UT_ASSERT(scnDetailsRuleWinner(blobs, lens, -1, shells, &value) == -1);

    scNetStop(&h, &slot);
    scRemoveTree(scDir);
    return 0;
}

/* ── 7. The map's own script, reloaded ────────────────────────────── */

/* The script beside the reload case's map, before and after the edit: the
 * rule value and the on_start sentence both change. */
static const char kScMapBefore[] =
    "scenario = { name = \"Reloaded\", api = 1, bound = true,\n"
    "  rules = { tank_full_shells = 40 },\n"
    "  callbacks = { on_start = \"Before the edit.\" } }\n"
    "function on_start() end\n";
static const char kScMapAfter[] =
    "scenario = { name = \"Reloaded\", api = 1, bound = true,\n"
    "  rules = { tank_full_shells = 60 },\n"
    "  callbacks = { on_start = \"After the edit.\" } }\n"
    "function on_start() end\n";

/* Write text to the script that sits beside mapPath. */
static bool scPutBeside(const char *mapPath, const char *text) {
    char   path[512];
    size_t n = strlen(mapPath);
    FILE  *f;

    if (n > 4) n -= 4;                 /* drop ".map" */
    snprintf(path, sizeof(path), "%.*s%s", (int)n, mapPath,
             SCN_SCRIPT_SUFFIX);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

/* What the sim answers for file: the shells value and the callbacks rows.
 * False when it answers nothing or the blob sets no shells value. */
static bool scMapDetails(ServerSim *sim, const char *file, double *shells,
                         char *rows, size_t rowsLen) {
    uint8_t blob[SCN_DETAILS_MAX];
    int     len;

    len = serverSimScenarioDetails(sim, file, blob, sizeof(blob));
    if (len <= 0) return false;
    if (!scnDetailsFindRule(blob, (size_t)len,
                            simRulesRuleIndex("tank_full_shells"), shells)) {
        return false;
    }
    return scCallbacks(blob, (size_t)len, rows, rowsLen) >= 0;
}

int run_scenario_details_reload_map_script(void) {
    BYTE               emap[6000] = E_MAP;
    ServerSim         *sim;
    ScenarioHost      *slot = NULL;
    const ScnDirEntry *row;
    char               mapPath[512];
    char               file[SCN_DIR_FILE_LEN];
    char               rows[1024];
    char               err[512];
    double             shells = 0.0;
    bool               reloaded;

    UT_ASSERT(scMakeDir("reload_map"));   /* an empty scenarios directory */
    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath),
                            "wbtest_scn_cb_reload.map"));
    UT_ASSERT(scPutBeside(mapPath, kScMapBefore));

    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island", gameOpen,
                                    false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetState(sim, serverStateLobby);
    serverSimSetScenarioDir(sim, scDir);
    scenarioHostRegisterScenarioLister(sim);
    scenarioHostFollowMap(sim, &slot);

    /* A map commit, the way the sim makes it after loading the new map. */
    SDL_strlcpy(sim->mapFilePath, mapPath, sizeof(sim->mapFilePath));
    serverSimScenarioOnMapChanged(sim, mapPath);
    UT_ASSERT_MSG(slot != NULL, "the map's own script was not attached");
    row = serverSimGetMapScript(sim);
    UT_ASSERT_MSG(row != NULL && row->file[0] != '\0',
                  "the commit published no row for the map's own script");
    SDL_strlcpy(file, row->file, sizeof(file));

    UT_ASSERT_MSG(scMapDetails(sim, file, &shells, rows, sizeof(rows)),
                  "the commit left no details for %s", file);
    UT_ASSERT_MSG(shells == 40.0, "the commit's shells read %g", shells);
    UT_ASSERT_MSG(strstr(rows, "on_start/E=Before the edit.;") != NULL,
                  "the commit's callbacks read %s", rows);

    /* The host edits the script on disk and reloads. */
    UT_ASSERT(scPutBeside(mapPath, kScMapAfter));
    err[0] = '\0';
    threadsWaitForMutex();
    reloaded = scenarioHostReload(slot, err, sizeof(err));
    threadsReleaseMutex();
    UT_ASSERT_MSG(reloaded, "the reload was refused: %s", err);

    /* The row keeps its name, and the details asked for by that name are the
       edited file's. */
    row = serverSimGetMapScript(sim);
    UT_ASSERT(row != NULL && strcmp(row->file, file) == 0);
    shells = 0.0;
    UT_ASSERT_MSG(scMapDetails(sim, file, &shells, rows, sizeof(rows)),
                  "the reload left no details for %s", file);
    UT_ASSERT_MSG(shells == 60.0,
                  "the details still say shells %g after the reload, wanted "
                  "the edited 60", shells);
    UT_ASSERT_MSG(strstr(rows, "on_start/E=After the edit.;") != NULL,
                  "the details still carry the old callbacks: %s", rows);

    scenarioHostDetach(slot);
    serverSimDestroy(sim);
    /* The script beside the map is under utScratchPath's directory, which
       the process removes at exit. */
    scRemoveTree(scDir);
    return 0;
}
