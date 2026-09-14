/*
 * The scenario host: a Lua script beside a map, read into the manifest and
 * applied to a round.
 *
 * Each case writes its own fixture script and removes it afterwards. The
 * names are per-case on purpose: CTest runs cases as separate processes in
 * one directory, so a shared fixture name is a race rather than a fixture.
 *
 * The map file itself is never written. The host reads the script beside a
 * map path and nothing else, so a path that names no real map is enough to
 * drive it, and the sims here are built from the built-in map.
 *
 * run_scenario_host_metadata          — name, description, api, game and the
 *                                       lobby block arrive as written
 * run_scenario_host_rules_change_round— a script's rules change how a round
 *                                       plays, observed on a tank
 * run_scenario_host_syntax_error_line — a broken script is refused, naming
 *                                       the file and the line
 * run_scenario_host_unknown_rule_key  — a key that names no rule is refused
 *                                       by name and the rest still applies
 * run_scenario_host_api_too_new       — an api above this server's is
 *                                       refused; equal and below are not
 * run_scenario_host_no_script        — no script is not an error
 * run_scenario_host_manifest_roundtrip— tags, regions and several teams
 * run_scenario_host_seed_reproducible — math.random is seeded from the
 *                                       process PRNG state, reproducibly
 * run_scenario_host_edit_after_attach — the script is read once, so an
 *                                       edit does not reach the next round
 * run_scenario_host_reload_picks_up_edit
 *                                     — and reload is what reads it again
 * run_scenario_host_reload_bad_syntax — a reload that does not parse is
 *                                       refused and changes nothing
 * run_scenario_host_reload_bad_api    — a reload written for a newer server
 *                                       is refused and changes nothing
 * run_scenario_host_reload_applies_nothing
 *                                     — the write rows refuse while the
 *                                       edited file is checked, so its top
 *                                       level cannot reach the round
 *
 * and the round's lifecycle:
 *
 * run_scenario_host_fresh_globals_per_round
 *                                     — a global set in on_setup is there
 *                                       for on_start and gone by the next
 *                                       round
 * run_scenario_host_setup_in_window   — on_setup runs, and the rules the
 *                                       script set are in the round's
 *                                       table by the time it does
 * run_scenario_host_start_on_first_running_tick
 *                                     — on_start on the first running tick,
 *                                       once, and not on a lobby tick
 * run_scenario_host_error_limit_boundary
 *                                     — the twentieth error in a row
 *                                       switches the scenario off and says
 *                                       so; the nineteenth and a success
 *                                       does not
 * run_scenario_host_disabled_stops_hooks
 *                                     — a switched-off round runs no hooks
 *                                       and answers classic; the next round
 *                                       runs them again
 * run_scenario_host_vm_lock_same_thread
 *                                     — a policy asked from inside a VM
 *                                       entry on the same thread answers
 *                                       rather than stopping dead
 * run_scenario_host_vm_lock_second_thread
 *                                     — a policy asked from another thread
 *                                       while the first is inside the VM
 *                                       gets through once it leaves
 * run_scenario_host_audit_human_lost  — a round start that loses a human
 *                                       switches the scenario off and tells
 *                                       the players
 * run_scenario_host_failed_start_drops_manifest
 *                                     — a start whose chunk raises leaves the
 *                                       host answering for no scenario at
 *                                       all, not for the round before it
 *
 * and the switch that decides whether a script is loaded at all:
 *
 * run_scenario_host_disabled_refuses_script
 *                                     — scripts off, and a map that has one
 *                                       attaches nothing and says which file
 * run_scenario_host_disabled_plain_map— scripts off, and a map with none
 *                                       says nothing
 * run_scenario_host_enabled_again     — the switch back on, and the attach
 *                                       is as it was
 *
 * and what a metatable on the script's own tables reaches and does not:
 *
 * run_scenario_host_metatable_raises  — an __index that raises, on _G and on
 *                                       the scenario table: the host says
 *                                       what it found and the process lives
 * run_scenario_host_metatable_not_read— an __index that answers: the parse
 *                                       takes nothing from it
 * run_scenario_host_hook_via_global_metatable
 *                                     — a hook name only a _G metatable
 *                                       answers is not resolved
 *
 * A script has no way to reach the engine yet, so the lifecycle cases read
 * what ran two ways: a hook appends a character to this case's record file,
 * and allow_extra_teams — the one policy the host registers — answers from
 * the globals the hooks left behind.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "bolo_rand.h"             /* bolo_srand — the seeding case */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.rules, tanks[], the policy
                                    * pointer and its depth bracket */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "server_sim_scenario.h"   /* the kill op the rules case uses */
#include "control_event.h"         /* CTRL_SERVER_TEXT — where a client reads
                                    * the line a switched-off round sends */
#include "game_sim.h"
#include "tank.h"                  /* tankIsDestroyed, TANK_DEATH_WAIT */
#include "everard_map.h"
#include "scenario_host.h"
#include "scenario_manifest.h"
#include "test_harness.h"

/* ── Fixtures ─────────────────────────────────────────────────────── */

/* A script sits beside the map: X.map is accompanied by X.scenario.lua. */
static void shScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

static bool shPut(const char *mapPath, const char *lua) {
    char  path[512];
    FILE *f;
    shScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static void shDrop(const char *mapPath) {
    char path[512];
    shScriptFor(mapPath, path, sizeof(path));
    remove(path);
}

/* A sim that has not started, ready to be attached to and then started. */
static ServerSim *shSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    return sim;
}

static tank *shTank(ServerSim *sim, BYTE slot) {
    return &sim->sim.tanks[slot];
}

/* Kill slot 0 the way a scenario would, so the wait the rules set is the
   wait the tank starts. */
static ScnOpResult shKill(ServerSim *sim, BYTE slot) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type              = SCN_OP_TANK_KILL;
    op.u.tankKill.slot   = slot;
    op.u.tankKill.killer = SCN_NONE;
    op.u.tankKill.cause  = LAST_DEATH_BY_SCRIPT;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* ── What the whole game was told ─────────────────────────────────── */

/* A line a scenario sends the players is a CTRL_SERVER_TEXT on the control
 * bus, and every client's delivery reads it from there: the wire path
 * encodes it onto the reliable control channel and the in-process path
 * applies it to a ClientSim. Catching it at the bus is catching it where a
 * client would. */
typedef struct {
    int  count;
    char last[SCN_TEXT_MAX];
} ShText;

static void shTextCb(void *ctx, const ControlEvent *evt) {
    ShText *c = (ShText *)ctx;
    if (evt->type != CTRL_SERVER_TEXT) {
        return;
    }
    c->count++;
    snprintf(c->last, sizeof(c->last), "%s", evt->u.serverText.text);
}

/* Registration replays the server's current state to the new subscriber, so
   the record is cleared afterwards and counts only what happens next. */
static void shWatchText(ServerSim *sim, ShText *c) {
    memset(c, 0, sizeof(*c));
    (void)serverSimRegisterSubscriber(sim, shTextCb, c);
    memset(c, 0, sizeof(*c));
}

/* ── The one question the sim asks ────────────────────────────────── */

/* allow_extra_teams, asked the way the lobby asks it: through the
   registered vtable with the sim's policy depth held across the call. */
static bool shAskExtraTeams(ServerSim *sim) {
    bool allow;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->allowExtraTeams == NULL) {
        return true;
    }
    serverSimScenarioPolicyEnter(sim);
    allow = sim->scenarioPolicy->allowExtraTeams(sim->scenarioPolicy->ctx);
    serverSimScenarioPolicyLeave(sim);
    return allow;
}

/* ── What a hook left behind ──────────────────────────────────────── */

/* No Lua function reaches the engine yet, so a hook says it ran by
 * appending a character to a file. The name is the case's own: CTest runs
 * cases as separate processes in one directory, so a shared name is a race
 * rather than a fixture. */
static void shScript(char *out, size_t outLen, const char *record,
                     const char *body) {
    snprintf(out, outLen,
             "local function note(s)\n"
             "  local f = io.open(\"%s\", \"a\")\n"
             "  if f then f:write(s) f:close() end\n"
             "end\n"
             "%s", record, body);
}

/* The record so far, or "" when no hook has written one. */
static void shRead(const char *record, char *out, size_t outLen) {
    FILE  *f = fopen(record, "rb");
    size_t n;

    out[0] = '\0';
    if (f == NULL) {
        return;
    }
    n = fread(out, 1, outLen - 1, f);
    fclose(f);
    out[n] = '\0';
}

/* ── A moment inside the round start ──────────────────────────────── */

/* The host holds its VM lock across the whole round start, so a case that
 * wants to reach the lock from inside one needs a moment of its own while
 * the start is running. A console line the host writes during the start is
 * that moment: it runs on the start's own thread, with the lock held and
 * the round's VM already in place.
 *
 * Two lines are used. "scenario: rule" is written when the funnel refuses a
 * rule, which is after the round's VM is installed and before on_setup.
 * "scenario: on_setup raised" is written when on_setup raises, which is
 * after the roster snapshot the audit holds the roster against and before
 * the audit itself.
 *
 * Only consoleMessage is replaced, never the ctx beside it, which the sim's
 * other callbacks read. */
static void (*shConsolePrev)(void *ctx, char *msg) = NULL;
static void (*shConsoleThen)(const char *msg)       = NULL;

static void shConsoleCb(void *ctx, char *msg) {
    if (shConsolePrev != NULL) {
        shConsolePrev(ctx, msg);
    }
    if (shConsoleThen != NULL && msg != NULL) {
        shConsoleThen(msg);
    }
}

static void shWatchConsole(ServerSim *sim, void (*then)(const char *msg)) {
    shConsolePrev = sim->sim.callbacks.consoleMessage;
    shConsoleThen = then;
    sim->sim.callbacks.consoleMessage = shConsoleCb;
}

static void shUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = shConsolePrev;
    shConsolePrev = NULL;
    shConsoleThen = NULL;
}

/* ── 1. Metadata ──────────────────────────────────────────────────── */

int run_scenario_host_metadata(void) {
    static const char *const kMap = "scnhost_meta.map";
    static const char *const kLua =
        "scenario = {\n"
        "  name = \"Survival\",\n"
        "  description = \"Hold the keep\",\n"
        "  api = 1,\n"
        "  game = \"tournament\",\n"
        "  bound = true,\n"
        "  lobby = {\n"
        "    max_players = 6,\n"
        "    extra_teams = false,\n"
        "    teams = { { id = 2, bots = 10, max_bots = 12, fielded = false,\n"
        "                brain = \"package:horde\" } },\n"
        "  },\n"
        "}\n";
    ServerSim              *sim;
    ScenarioHost           *h;
    const ScenarioManifest *m;
    char                    err[512];

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    UT_ASSERT_MSG(scenarioHostIsActive(h), "the host reports no scenario");
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Survival") == 0,
                  "name read as '%s'", scenarioHostName(h));
    UT_ASSERT_MSG(strcmp(scenarioHostDescription(h), "Hold the keep") == 0,
                  "description read as '%s'", scenarioHostDescription(h));

    m = scenarioHostManifest(h);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(m->api == 1, "api read as %d", m->api);
    UT_ASSERT_MSG(strcmp(m->game, "tournament") == 0, "game read as '%s'",
                  m->game);
    UT_ASSERT_MSG(m->bound, "bound read as false");
    UT_ASSERT_MSG(m->lobby.maxPlayers == 6, "max_players read as %u",
                  (unsigned)m->lobby.maxPlayers);
    UT_ASSERT_MSG(!m->lobby.extraTeams, "extra_teams read as true");
    UT_ASSERT_MSG(m->lobby.numTeams == 1, "%u teams, expected 1",
                  (unsigned)m->lobby.numTeams);
    UT_ASSERT_MSG(m->lobby.teams[0].id == 2, "team id read as %u",
                  (unsigned)m->lobby.teams[0].id);
    UT_ASSERT_MSG(m->lobby.teams[0].bots == 10, "bots read as %u",
                  (unsigned)m->lobby.teams[0].bots);
    UT_ASSERT_MSG(m->lobby.teams[0].maxBots == 12, "max_bots read as %u",
                  (unsigned)m->lobby.teams[0].maxBots);
    UT_ASSERT_MSG(!m->lobby.teams[0].fielded, "fielded read as true");
    UT_ASSERT_MSG(strcmp(m->lobby.teams[0].brain, "package:horde") == 0,
                  "brain read as '%s'", m->lobby.teams[0].brain);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 2. A script's rules change a round ───────────────────────────── */

/* Well above the classic wait, and inside tank_death_ticks' 0..65535 row. */
#define SH_DEATH_SET       400
#define SH_PAST_CLASSIC    (TANK_DEATH_WAIT + 20)
#define SH_RESPAWN_GIVE_UP (SH_DEATH_SET * 4)

/* The field the script sets is checked by watching a tank rather than by
   reading the table back: reading it back would say only that a write
   happened, not that the round plays differently for it. */
int run_scenario_host_rules_change_round(void) {
    static const char *const kMap = "scnhost_rules.map";
    static const char *const kLua =
        "scenario = {\n"
        "  name = \"Long Death\",\n"
        "  api = 1,\n"
        "  rules = { tank_death_ticks = 400 },\n"
        "}\n";
    ServerSim    *sim;
    ScenarioHost *h;
    tank         *t;
    char          err[512];
    int           frames;
    int           respawnedAt = 0;

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    /* The round start invokes the host, which applies the table. */
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Tester", false);
    t = shTank(sim, 0);
    UT_ASSERT_MSG(*t != NULL, "the player has no tank");

    UT_ASSERT_MSG(shKill(sim, 0) == SCN_OP_OK, "the kill was refused");
    UT_ASSERT_MSG(tankIsDestroyed(t), "the kill left the tank alive");
    UT_ASSERT_MSG(tankGetDeathWait(t) == SH_DEATH_SET,
                  "the tank started a wait of %u, expected the script's %d",
                  (unsigned)tankGetDeathWait(t), SH_DEATH_SET);

    for (frames = 0; frames < SH_PAST_CLASSIC; frames++) {
        serverSimTick(sim);
        if (!tankIsDestroyed(t)) {
            respawnedAt = frames + 1;
            break;
        }
    }
    UT_ASSERT_MSG(respawnedAt == 0,
                  "the tank came back on frame %d — under the script's %d it "
                  "should still be down past the classic wait of %d",
                  respawnedAt, SH_DEATH_SET, TANK_DEATH_WAIT);

    for (; frames < SH_RESPAWN_GIVE_UP; frames++) {
        serverSimTick(sim);
        if (!tankIsDestroyed(t)) {
            respawnedAt = frames + 1;
            break;
        }
    }
    UT_ASSERT_MSG(respawnedAt != 0,
                  "the tank was still down after %d frames with the script's "
                  "tank_death_ticks of %d", SH_RESPAWN_GIVE_UP, SH_DEATH_SET);
    UT_ASSERT_MSG(respawnedAt >= SH_DEATH_SET,
                  "the tank came back on frame %d, short of the %d the "
                  "script asked for", respawnedAt, SH_DEATH_SET);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 3. A syntax error, with its line ─────────────────────────────── */

/* The error is on its own line so the line number is not a judgement about
   where a parser decides a bad expression began: line 5 holds a statement
   that cannot be one, and nothing else is wrong with the file. */
int run_scenario_host_syntax_error_line(void) {
    static const char *const kMap = "scnhost_syntax.map";
    static const char *const kLua =
        "-- 1\n"
        "-- 2\n"
        "-- 3\n"
        "-- 4\n"
        "this is not lua\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h == NULL, "a script that does not parse was accepted");
    UT_ASSERT_MSG(err[0] != '\0', "a broken script produced no operator line");
    UT_ASSERT_MSG(strstr(err, "scnhost_syntax") != NULL,
                  "the line does not name the file: %s", err);
    UT_ASSERT_MSG(strstr(err, ":5:") != NULL,
                  "the line does not carry the line number: %s", err);

    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 4. A key that names no rule ──────────────────────────────────── */

int run_scenario_host_unknown_rule_key(void) {
    static const char *const kMap = "scnhost_badkey.map";
    static const char *const kLua =
        "scenario = {\n"
        "  name = \"Typo\",\n"
        "  api = 1,\n"
        "  rules = { tank_death_tick = 400, tank_reload_ticks = 7 },\n"
        "}\n";
    ServerSim              *sim;
    ScenarioHost           *h;
    const ScenarioManifest *m;
    char                    err[512];

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL,
                  "one bad key cost the whole script: %s", err);
    UT_ASSERT_MSG(strstr(scenarioHostLastError(h), "tank_death_tick") != NULL,
                  "the refusal does not name the key: '%s'",
                  scenarioHostLastError(h));

    m = scenarioHostManifest(h);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(m->numRules == 1,
                  "%u rules kept, expected the 1 that names a rule",
                  (unsigned)m->numRules);
    UT_ASSERT_MSG(m->rules[0].rule == (uint16_t)SCN_RULE_tank_reload_ticks,
                  "the rule kept is index %u, expected tank_reload_ticks",
                  (unsigned)m->rules[0].rule);

    /* And the key that does name a rule still reaches the round. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(sim->sim.rules.tank_reload_ticks == 7,
                  "tank_reload_ticks is %ld after the start, expected the 7 "
                  "the script set beside the bad key",
                  (long)sim->sim.rules.tank_reload_ticks);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 5. An api this server does not implement ─────────────────────── */

int run_scenario_host_api_too_new(void) {
    static const char *const kMap = "scnhost_api.map";
    char          lua[256];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    sim = shSim();
    UT_ASSERT(sim != NULL);

    /* Above: refused, and the line says why. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Future\", api = %d }\n",
             SCENARIO_API_VERSION + 1);
    UT_ASSERT(shPut(kMap, lua));
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h == NULL, "a script written for a newer server was run");
    UT_ASSERT_MSG(err[0] != '\0', "the refusal produced no operator line");
    UT_ASSERT_MSG(strstr(err, "api") != NULL,
                  "the line does not mention the api: %s", err);

    /* Equal: accepted. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Current\", api = %d }\n",
             SCENARIO_API_VERSION);
    UT_ASSERT(shPut(kMap, lua));
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "a script at this server's api was refused: %s",
                  err);
    scenarioHostDetach(h);

    /* Below: accepted too — an older file runs on a newer server. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Older\", api = %d }\n",
             SCENARIO_API_VERSION - 1);
    UT_ASSERT(shPut(kMap, lua));
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "a script at an older api was refused: %s", err);
    scenarioHostDetach(h);

    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 6. No script at all ──────────────────────────────────────────── */

int run_scenario_host_no_script(void) {
    static const char *const kMap = "scnhost_none.map";
    ServerSim    *sim;
    ScenarioHost *h;
    tank         *t;
    char          err[512];

    /* Nothing written, and anything a previous run left behind removed. */
    shDrop(kMap);

    sim = shSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h == NULL, "a map with no script produced a host");
    UT_ASSERT_MSG(err[0] == '\0',
                  "a map with no script produced an operator line: %s", err);
    UT_ASSERT_MSG(!scenarioHostIsActive(h),
                  "a NULL host reports a scenario as active");

    /* And the round is the classic one. */
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Tester", false);
    t = shTank(sim, 0);
    UT_ASSERT_MSG(*t != NULL, "the player has no tank");
    UT_ASSERT_MSG(shKill(sim, 0) == SCN_OP_OK, "the kill was refused");
    UT_ASSERT_MSG(tankGetDeathWait(t) == TANK_DEATH_WAIT,
                  "the tank waits %u with no scenario, expected the classic %d",
                  (unsigned)tankGetDeathWait(t), TANK_DEATH_WAIT);

    serverSimDestroy(sim);
    return 0;
}

/* ── 7. The whole table ───────────────────────────────────────────── */

/* Regions are keyed by name, and the two Lua builds iterate a table in
   different orders, so a reader looks one up by name rather than by
   position. */
static const ScnManifestRegion *shRegion(const ScenarioManifest *m,
                                         const char *name) {
    uint8_t i;
    for (i = 0; i < m->numRegions; i++) {
        if (strcmp(m->regions[i].name, name) == 0) {
            return &m->regions[i];
        }
    }
    return NULL;
}

int run_scenario_host_manifest_roundtrip(void) {
    static const char *const kMap = "scnhost_full.map";
    static const char *const kLua =
        "scenario = {\n"
        "  name = \"Full\", api = 1, bound = true,\n"
        "  lobby = {\n"
        "    max_players = 8, extra_teams = true,\n"
        "    teams = {\n"
        "      { id = 2, bots = 3, max_bots = 5, fielded = false,\n"
        "        brain = \"package:horde\" },\n"
        "      { id = 3, bots = 1, max_bots = 2, fielded = true,\n"
        "        brain = \"brains/idle.lua\" },\n"
        "    },\n"
        "  },\n"
        "  tags = {\n"
        "    pills = { [3] = { \"outer\", \"north\" } },\n"
        "    bases = { [1] = \"keep\" },\n"
        "    starts = {},\n"
        "  },\n"
        "  regions = {\n"
        "    keep = { x = 100, y = 100, w = 12, h = 12 },\n"
        "    moat = { x = 90, y = 90, w = 40, h = 40 },\n"
        "  },\n"
        "  triggers = {},\n"
        "}\n";
    ServerSim               *sim;
    ScenarioHost            *h;
    const ScenarioManifest  *m;
    const ScnManifestRegion *reg;
    char                     err[512];

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    m = scenarioHostManifest(h);
    UT_ASSERT(m != NULL);

    /* The lobby block, both teams. */
    UT_ASSERT_MSG(m->lobby.maxPlayers == 8, "max_players read as %u",
                  (unsigned)m->lobby.maxPlayers);
    UT_ASSERT_MSG(m->lobby.extraTeams, "extra_teams read as false");
    UT_ASSERT_MSG(m->lobby.numTeams == 2, "%u teams, expected 2",
                  (unsigned)m->lobby.numTeams);
    UT_ASSERT_MSG(m->lobby.teams[0].id == 2 && m->lobby.teams[0].bots == 3 &&
                      m->lobby.teams[0].maxBots == 5 &&
                      !m->lobby.teams[0].fielded,
                  "first team read as id %u bots %u max %u fielded %d",
                  (unsigned)m->lobby.teams[0].id,
                  (unsigned)m->lobby.teams[0].bots,
                  (unsigned)m->lobby.teams[0].maxBots,
                  (int)m->lobby.teams[0].fielded);
    UT_ASSERT_MSG(m->lobby.teams[1].id == 3 && m->lobby.teams[1].bots == 1 &&
                      m->lobby.teams[1].maxBots == 2 &&
                      m->lobby.teams[1].fielded,
                  "second team read as id %u bots %u max %u fielded %d",
                  (unsigned)m->lobby.teams[1].id,
                  (unsigned)m->lobby.teams[1].bots,
                  (unsigned)m->lobby.teams[1].maxBots,
                  (int)m->lobby.teams[1].fielded);
    UT_ASSERT_MSG(strcmp(m->lobby.teams[1].brain, "brains/idle.lua") == 0,
                  "second team's brain read as '%s'", m->lobby.teams[1].brain);

    /* Tags: an array of two on pill 3, and a bare string on base 1, which is
       accepted where an array is expected. */
    UT_ASSERT_MSG(m->pillTags[3].count == 2, "pill 3 carries %u tags",
                  (unsigned)m->pillTags[3].count);
    UT_ASSERT_MSG(strcmp(m->pillTags[3].tag[0], "outer") == 0,
                  "pill 3's first tag read as '%s'", m->pillTags[3].tag[0]);
    UT_ASSERT_MSG(strcmp(m->pillTags[3].tag[1], "north") == 0,
                  "pill 3's second tag read as '%s'", m->pillTags[3].tag[1]);
    UT_ASSERT_MSG(m->baseTags[1].count == 1, "base 1 carries %u tags",
                  (unsigned)m->baseTags[1].count);
    UT_ASSERT_MSG(strcmp(m->baseTags[1].tag[0], "keep") == 0,
                  "base 1's tag read as '%s'", m->baseTags[1].tag[0]);

    /* Lua's keys are 1-based and stay 1-based: the tag written at 3 is at 3
       here, and 1 and 2 are untouched. */
    UT_ASSERT_MSG(m->pillTags[1].count == 0 && m->pillTags[2].count == 0,
                  "a tag written at index 3 reached index 1 or 2 — the keys "
                  "were shifted");
    UT_ASSERT_MSG(m->baseTags[2].count == 0, "base 2 gained a tag");
    UT_ASSERT_MSG(m->startTags[1].count == 0, "an empty tag table added one");

    /* Regions. */
    UT_ASSERT_MSG(m->numRegions == 2, "%u regions, expected 2",
                  (unsigned)m->numRegions);
    reg = shRegion(m, "keep");
    UT_ASSERT_MSG(reg != NULL, "no region is named 'keep'");
    UT_ASSERT_MSG(reg->x == 100 && reg->y == 100 && reg->w == 12 &&
                      reg->h == 12,
                  "'keep' read as x %u y %u w %u h %u", (unsigned)reg->x,
                  (unsigned)reg->y, (unsigned)reg->w, (unsigned)reg->h);
    reg = shRegion(m, "moat");
    UT_ASSERT_MSG(reg != NULL, "no region is named 'moat'");
    UT_ASSERT_MSG(reg->x == 90 && reg->y == 90 && reg->w == 40 &&
                      reg->h == 40,
                  "'moat' read as x %u y %u w %u h %u", (unsigned)reg->x,
                  (unsigned)reg->y, (unsigned)reg->w, (unsigned)reg->h);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 8. The seed math.random is given ─────────────────────────────── */

/* The host seeds math.random from the process PRNG's state as it stands at
 * the boot, read rather than drawn. Two things follow, and the case pins
 * both: under a fixed process seed two boots see the same draw, and under a
 * different process seed they do not — which is what separates a real seed
 * from the fixed sequence a VM with no randomseed call would hand out.
 *
 * The draw is carried out through the name, because that is a value the
 * host already exposes and the chunk can compute. */
int run_scenario_host_seed_reproducible(void) {
    static const char *const kMap = "scnhost_seed.map";
    static const char *const kLua =
        "scenario = { name = tostring(math.random(1, 1000000000)), api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    char          first[SCN_SCENARIO_NAME_LEN];
    char          again[SCN_SCENARIO_NAME_LEN];
    char          other[SCN_SCENARIO_NAME_LEN];

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    /* Seeded immediately before each attach, so the PRNG state the host
       reads is identical on both and nothing in between can move it. */
    bolo_srand(0xA11CEULL);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    snprintf(first, sizeof(first), "%s", scenarioHostName(h));
    scenarioHostDetach(h);
    UT_ASSERT_MSG(first[0] != '\0', "the chunk produced no draw");

    bolo_srand(0xA11CEULL);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused on the second boot: %s",
                  err);
    snprintf(again, sizeof(again), "%s", scenarioHostName(h));
    scenarioHostDetach(h);
    UT_ASSERT_MSG(strcmp(first, again) == 0,
                  "two boots under one process seed drew '%s' then '%s' — the "
                  "round does not replay", first, again);

    bolo_srand(0xB0BULL);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused on the third boot: %s",
                  err);
    snprintf(other, sizeof(other), "%s", scenarioHostName(h));
    scenarioHostDetach(h);
    UT_ASSERT_MSG(strcmp(first, other) != 0,
                  "a different process seed drew '%s' as well — math.random is "
                  "running on a constant, not on the seed the host gives it",
                  other);

    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 9. The script is read once ───────────────────────────────────── */

/* Two values apart, both inside tank_death_ticks' row: what the file held
   when the host read it, and what it was edited to afterwards. */
#define SH_EDIT_CACHED   400
#define SH_EDIT_ON_DISK  300

/* The host reads the file at attach and keeps the bytes, so a round start
 * runs what it read and touches no disk. An edit made while the server is
 * up therefore changes nothing on its own — it takes something asking the
 * host to read the file again, and nothing does yet. This case is what
 * holds that until something does. */
int run_scenario_host_edit_after_attach(void) {
    static const char *const kMap = "scnhost_edit.map";
    static const char *const kFirst =
        "scenario = { name = \"Cached\", api = 1,\n"
        "             rules = { tank_death_ticks = 400 } }\n";
    static const char *const kSecond =
        "scenario = { name = \"Edited\", api = 1,\n"
        "             rules = { tank_death_ticks = 300 } }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    tank         *t;
    char          err[512];

    UT_ASSERT(shPut(kMap, kFirst));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Cached") == 0,
                  "name read as '%s' before the edit", scenarioHostName(h));

    /* Edited on disk, after the host has read it. */
    UT_ASSERT(shPut(kMap, kSecond));

    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Tester", false);
    t = shTank(sim, 0);
    UT_ASSERT_MSG(*t != NULL, "the player has no tank");
    UT_ASSERT_MSG(shKill(sim, 0) == SCN_OP_OK, "the kill was refused");

    UT_ASSERT_MSG(tankGetDeathWait(t) == SH_EDIT_CACHED,
                  "the tank waits %u: %d is what the file held when the host "
                  "read it and %d what it was edited to, so the round went "
                  "back to the file", (unsigned)tankGetDeathWait(t),
                  SH_EDIT_CACHED, SH_EDIT_ON_DISK);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Cached") == 0,
                  "the name read as '%s' after the round started, so the "
                  "edited file was read", scenarioHostName(h));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 10. Reload reads the file again ──────────────────────────────── */

/* The value a round starts on, and the value reload is expected to put in
   its place. Both inside tank_death_ticks' row and far enough apart that a
   pump loop tells them apart at a glance. */
#define SH_RELOAD_BEFORE 400
#define SH_RELOAD_AFTER  520

/* Start a round on `sim` and hand back slot 0's tank, killed, so the caller
   can read the wait the rules gave it. Returns NULL when the round did not
   produce a tank. */
static tank *shKilledTankAfterStart(ServerSim *sim) {
    tank *t;
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Tester", false);
    t = shTank(sim, 0);
    if (*t == NULL) {
        return NULL;
    }
    if (shKill(sim, 0) != SCN_OP_OK) {
        return NULL;
    }
    return t;
}

/* The inverse of scenario_host_edit_after_attach: the same edit, with a
 * reload in between, does reach the next round. The two cases together are
 * what say reload is the thing that re-reads the file, rather than the
 * round start doing it anyway. */
int run_scenario_host_reload_picks_up_edit(void) {
    static const char *const kMap = "scnhost_reload_edit.map";
    static const char *const kFirst =
        "scenario = { name = \"Before\", api = 1,\n"
        "             rules = { tank_death_ticks = 400 } }\n";
    static const char *const kSecond =
        "scenario = { name = \"After\", api = 1,\n"
        "             rules = { tank_death_ticks = 520 } }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    tank         *t;
    char          err[512];

    UT_ASSERT(shPut(kMap, kFirst));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    UT_ASSERT(shPut(kMap, kSecond));
    err[0] = '\0';
    UT_ASSERT_MSG(scenarioHostReload(h, err, sizeof(err)),
                  "the reload was refused: %s", err);

    /* The round that has not started yet is the one that changes. */
    t = shKilledTankAfterStart(sim);
    UT_ASSERT_MSG(t != NULL, "the round produced no tank to kill");
    UT_ASSERT_MSG(tankGetDeathWait(t) == SH_RELOAD_AFTER,
                  "the tank waits %u: %d is the edited value the reload read "
                  "and %d the one it replaced",
                  (unsigned)tankGetDeathWait(t), SH_RELOAD_AFTER,
                  SH_RELOAD_BEFORE);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "After") == 0,
                  "the name reads '%s' after the round started, so the "
                  "reloaded table did not reach the manifest",
                  scenarioHostName(h));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* A reload of a file that does not parse is refused with the line Lua
 * reports, and the scenario carries on exactly as it was — the round after
 * it still runs the table the host was holding. */
int run_scenario_host_reload_bad_syntax(void) {
    static const char *const kMap = "scnhost_reload_syntax.map";
    static const char *const kGood =
        "scenario = { name = \"Before\", api = 1,\n"
        "             rules = { tank_death_ticks = 400 } }\n";
    static const char *const kBroken =
        "-- 1\n"
        "-- 2\n"
        "this is not lua\n";
    ServerSim    *sim;
    ScenarioHost *h;
    tank         *t;
    char          err[512];

    UT_ASSERT(shPut(kMap, kGood));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    UT_ASSERT(shPut(kMap, kBroken));
    err[0] = '\0';
    UT_ASSERT_MSG(!scenarioHostReload(h, err, sizeof(err)),
                  "a script that does not parse was reloaded");
    UT_ASSERT_MSG(err[0] != '\0', "the refusal produced no operator line");
    UT_ASSERT_MSG(strstr(err, "scnhost_reload_syntax") != NULL,
                  "the line does not name the file: %s", err);
    UT_ASSERT_MSG(strstr(err, ":3:") != NULL,
                  "the line does not carry the line number: %s", err);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Before") == 0,
                  "the refused reload changed the name to '%s'",
                  scenarioHostName(h));

    t = shKilledTankAfterStart(sim);
    UT_ASSERT_MSG(t != NULL, "the round produced no tank to kill");
    UT_ASSERT_MSG(tankGetDeathWait(t) == SH_RELOAD_BEFORE,
                  "the tank waits %u after a refused reload, expected the %d "
                  "the scenario was already running on",
                  (unsigned)tankGetDeathWait(t), SH_RELOAD_BEFORE);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Before") == 0,
                  "the round read '%s', so the broken file reached it anyway",
                  scenarioHostName(h));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* And the same for a file this server is too old to run: refused, with the
 * running scenario left alone. */
int run_scenario_host_reload_bad_api(void) {
    static const char *const kMap = "scnhost_reload_api.map";
    static const char *const kGood =
        "scenario = { name = \"Before\", api = 1,\n"
        "             rules = { tank_death_ticks = 400 } }\n";
    char          tooNew[256];
    ServerSim    *sim;
    ScenarioHost *h;
    tank         *t;
    char          err[512];

    UT_ASSERT(shPut(kMap, kGood));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    snprintf(tooNew, sizeof(tooNew),
             "scenario = { name = \"After\", api = %d,\n"
             "             rules = { tank_death_ticks = 520 } }\n",
             SCENARIO_API_VERSION + 1);
    UT_ASSERT(shPut(kMap, tooNew));
    err[0] = '\0';
    UT_ASSERT_MSG(!scenarioHostReload(h, err, sizeof(err)),
                  "a script written for a newer server was reloaded");
    UT_ASSERT_MSG(err[0] != '\0', "the refusal produced no operator line");
    UT_ASSERT_MSG(strstr(err, "api") != NULL,
                  "the line does not mention the api: %s", err);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Before") == 0,
                  "the refused reload changed the name to '%s'",
                  scenarioHostName(h));

    t = shKilledTankAfterStart(sim);
    UT_ASSERT_MSG(t != NULL, "the round produced no tank to kill");
    UT_ASSERT_MSG(tankGetDeathWait(t) == SH_RELOAD_BEFORE,
                  "the tank waits %u after a refused reload, expected the %d "
                  "the scenario was already running on",
                  (unsigned)tankGetDeathWait(t), SH_RELOAD_BEFORE);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 13. A fresh set of globals every round ───────────────────────── */

/* on_setup writes whether the global it is about to set was already there,
 * and on_start writes whether it is still there. Within one round the
 * second reads what the first wrote; across rounds the VM is new, so the
 * first finds nothing.
 *
 * "a" is absent at setup and "p" present; "c" is carried as far as the
 * start and "x" lost. Two rounds therefore read "acac", and a "p" in the
 * third place is the first round's global reaching the second. */
int run_scenario_host_fresh_globals_per_round(void) {
    static const char *const kMap    = "scnhost_fresh.map";
    static const char *const kRecord = "scnhost_fresh.record";
    static const char *const kBody =
        "scenario = { name = \"Fresh\", api = 1 }\n"
        "function on_setup()\n"
        "  note(marker == nil and \"a\" or \"p\")\n"
        "  marker = true\n"
        "end\n"
        "function on_start()\n"
        "  note(marker == true and \"c\" or \"x\")\n"
        "end\n";
    char          lua[1024];
    char          got[64];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    remove(kRecord);
    shScript(lua, sizeof(lua), kRecord, kBody);
    UT_ASSERT(shPut(kMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    serverSimTick(sim);
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "ac") == 0,
                  "the first round recorded '%s', expected 'ac': the setup "
                  "found no global and the start found the one it set", got);

    serverSimStartGame(sim);
    serverSimTick(sim);
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "acac") == 0,
                  "two rounds recorded '%s', expected 'acac': a 'p' in the "
                  "third place is the first round's global still there in "
                  "the second", got);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 14. on_setup inside the window, after the rules ──────────────── */

/* The wait the scripts below set, well above the classic one. */
#define SH_WINDOW_DEATH 400

/* Two halves. The first: on_setup runs, and the rule its own script set is
 * in the round's table. The rule is what says the call sits inside the
 * setup window — the funnel refuses every op while a start is in progress
 * and the window is the one thing that lets a SCN_OP_SET_RULE through, so a
 * classic wait here would mean the window was shut.
 *
 * The second: the same script with a setup that raises. The rule is in the
 * table all the same, so the table is not something the setup produces —
 * it is there before the setup is called, which is the point of calling the
 * setup after it. */
int run_scenario_host_setup_in_window(void) {
    static const char *const kMap    = "scnhost_window.map";
    static const char *const kRecord = "scnhost_window.record";
    static const char *const kBody =
        "scenario = { name = \"Window\", api = 1,\n"
        "             rules = { tank_death_ticks = 400 } }\n"
        "function on_setup() note(\"s\") end\n";
    static const char *const kRaiser =
        "scenario = { name = \"Raiser\", api = 1,\n"
        "             rules = { tank_death_ticks = 400 } }\n"
        "function on_setup() error(\"no\") end\n";
    static const char *const kRaiserMap = "scnhost_window_raise.map";
    char          lua[1024];
    char          got[64];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    remove(kRecord);
    shScript(lua, sizeof(lua), kRecord, kBody);
    UT_ASSERT(shPut(kMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "s") == 0,
                  "the round recorded '%s', expected 's': on_setup did not "
                  "run from the round start", got);
    UT_ASSERT_MSG(sim->sim.rules.tank_death_ticks == SH_WINDOW_DEATH,
                  "the round's tank_death_ticks is %d, expected the "
                  "script's %d: the rule op was refused, so the setup "
                  "window was shut across the call",
                  (int)sim->sim.rules.tank_death_ticks, SH_WINDOW_DEATH);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    remove(kRecord);

    /* The second half, on its own sim and its own script. */
    UT_ASSERT(shPut(kRaiserMap, kRaiser));
    sim = shSim();
    UT_ASSERT(sim != NULL);
    h = scenarioHostAttach(sim, kRaiserMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    UT_ASSERT_MSG(sim->sim.rules.tank_death_ticks == SH_WINDOW_DEATH,
                  "a setup that raised left tank_death_ticks at %d, expected "
                  "the script's %d: the table is applied before the setup "
                  "is called, so a setup that fails cannot take it away",
                  (int)sim->sim.rules.tank_death_ticks, SH_WINDOW_DEATH);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kRaiserMap);
    return 0;
}

/* ── 15. on_start on the first running tick ───────────────────────── */

/* A freshly created sim sits in the lobby state, so the ticks before the
 * start take the non-running branch of serverSimTick — which calls the
 * host's per-tick callback just as the running branch does. on_start
 * belongs to neither those nor the start itself: it belongs to the first
 * tick after the round is running, and to that one alone. */
int run_scenario_host_start_on_first_running_tick(void) {
    static const char *const kMap    = "scnhost_first_tick.map";
    static const char *const kRecord = "scnhost_first_tick.record";
    static const char *const kBody =
        "scenario = { name = \"First\", api = 1 }\n"
        "function on_start() note(\"s\") end\n";
    char          lua[1024];
    char          got[64];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    remove(kRecord);
    shScript(lua, sizeof(lua), kRecord, kBody);
    UT_ASSERT(shPut(kMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "the sim is in state %d, expected the lobby",
                  (int)serverSimGetState(sim));

    for (i = 0; i < 5; i++) {
        serverSimTick(sim);
    }
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(got[0] == '\0',
                  "five lobby ticks recorded '%s': on_start ran before the "
                  "round did", got);

    serverSimStartGame(sim);
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(got[0] == '\0',
                  "the round start recorded '%s': on_start belongs to the "
                  "first running tick, not to the start", got);

    serverSimTick(sim);
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "s") == 0,
                  "the first running tick recorded '%s', expected 's'", got);

    for (i = 0; i < 10; i++) {
        serverSimTick(sim);
    }
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "s") == 0,
                  "eleven running ticks recorded '%s', expected the one 's': "
                  "on_start ran more than once", got);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 16. The error limit, on both sides of it ─────────────────────── */

/* Both halves count in SCN_ERROR_LIMIT rather than in twenty, so the
 * constant and the boundary cannot drift apart.
 *
 * The line a switched-off round sends is read off the control bus, which is
 * where every client's delivery reads it: the wire path encodes it onto the
 * reliable control channel from there and the in-process path applies it to
 * a ClientSim from there. Counting the publish is counting the arrival. */
int run_scenario_host_error_limit_boundary(void) {
    static const char *const kHitMap  = "scnhost_limit_hit.map";
    static const char *const kBackMap = "scnhost_limit_back.map";
    char          lua[1024];
    char          needle[32];
    ServerSim    *sim;
    ScenarioHost *h;
    ShText        text;
    char          err[512];
    int           i;

    /* The first half: the script raises on every call up to the limit and
       would answer false after it. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Boom\", api = 1 }\n"
             "n = 0\n"
             "function allow_extra_teams()\n"
             "  n = n + 1\n"
             "  if n <= %d then error(\"boom\") end\n"
             "  return false\n"
             "end\n", SCN_ERROR_LIMIT);
    UT_ASSERT(shPut(kHitMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);
    h = scenarioHostAttach(sim, kHitMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    shWatchText(sim, &text);

    for (i = 1; i < SCN_ERROR_LIMIT; i++) {
        UT_ASSERT_MSG(shAskExtraTeams(sim),
                      "error %d of %d answered anything but the classic yes",
                      i, SCN_ERROR_LIMIT);
    }
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 0,
                  "%d errors in a row sent %d lines to the game; the limit "
                  "is %d", SCN_ERROR_LIMIT - 1, text.count, SCN_ERROR_LIMIT);

    UT_ASSERT_MSG(shAskExtraTeams(sim), "the error at the limit answered "
                                        "anything but the classic yes");
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 1,
                  "the %dth error sent %d lines to the game, expected one",
                  SCN_ERROR_LIMIT, text.count);
    UT_ASSERT_MSG(strstr(text.last, "Boom") != NULL,
                  "the line does not name the scenario: %s", text.last);
    snprintf(needle, sizeof(needle), "%d", SCN_ERROR_LIMIT);
    UT_ASSERT_MSG(strstr(text.last, needle) != NULL,
                  "the line does not say how many errors it took: %s",
                  text.last);

    /* The script answers false from here on; a switched-off round runs none
       of it and answers the classic yes. */
    UT_ASSERT_MSG(shAskExtraTeams(sim),
                  "the switched-off round ran the script's answer");
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 1, "the line was sent %d times, expected once",
                  text.count);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kHitMap);

    /* The second half: one call short of the limit, then a call that
       returns, then the same run of errors again. The success in the middle
       is what makes the second run start from nothing. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Recovers\", api = 1 }\n"
             "n = 0\n"
             "function allow_extra_teams()\n"
             "  n = n + 1\n"
             "  if n == %d or n == %d then return false end\n"
             "  error(\"boom\")\n"
             "end\n", SCN_ERROR_LIMIT, SCN_ERROR_LIMIT * 2);
    UT_ASSERT(shPut(kBackMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);
    h = scenarioHostAttach(sim, kBackMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    shWatchText(sim, &text);

    for (i = 1; i < SCN_ERROR_LIMIT; i++) {
        UT_ASSERT(shAskExtraTeams(sim));
    }
    UT_ASSERT_MSG(!shAskExtraTeams(sim),
                  "call %d returns false in the script and the host answered "
                  "the classic yes, so %d errors were enough to switch the "
                  "scenario off", SCN_ERROR_LIMIT, SCN_ERROR_LIMIT - 1);

    for (i = 1; i < SCN_ERROR_LIMIT; i++) {
        UT_ASSERT(shAskExtraTeams(sim));
    }
    UT_ASSERT_MSG(!shAskExtraTeams(sim),
                  "a second run of %d errors switched the scenario off, so "
                  "the call that returned in between did not put the count "
                  "back to zero", SCN_ERROR_LIMIT - 1);

    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 0,
                  "%d lines reached the game, expected none: nothing here "
                  "ever reached %d errors in a row",
                  text.count, SCN_ERROR_LIMIT);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kBackMap);
    return 0;
}

/* ── 17. A switched-off round, and the next one ───────────────────── */

/* Switched off is switched off for the round: no hook runs, the policy
 * answers classic, and the round plays on. It is not switched off for the
 * attachment, so the round after it boots a fresh VM and runs the hooks
 * again — which is also what says the first round's silence was the scenario
 * being off rather than the hook never working. */
int run_scenario_host_disabled_stops_hooks(void) {
    static const char *const kMap    = "scnhost_off.map";
    static const char *const kRecord = "scnhost_off.record";
    static const char *const kBody =
        "scenario = { name = \"Off\", api = 1 }\n"
        "function allow_extra_teams() error(\"boom\") end\n"
        "function on_start() note(\"s\") end\n";
    char          lua[1024];
    char          got[64];
    ServerSim    *sim;
    ScenarioHost *h;
    ShText        text;
    char          err[512];
    int           i;

    remove(kRecord);
    shScript(lua, sizeof(lua), kRecord, kBody);
    UT_ASSERT(shPut(kMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    shWatchText(sim, &text);

    /* Off before the first running tick, so on_start is the hook that has
       not run yet when the scenario stops. */
    for (i = 0; i < SCN_ERROR_LIMIT; i++) {
        (void)shAskExtraTeams(sim);
    }
    for (i = 0; i < 10; i++) {
        serverSimTick(sim);
    }

    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(got[0] == '\0',
                  "ten running ticks recorded '%s': a switched-off round ran "
                  "on_start", got);
    UT_ASSERT_MSG(text.count == 1,
                  "the round sent %d lines to the game, expected the one that "
                  "says the scenario is off", text.count);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(shAskExtraTeams(sim),
                  "the switched-off round still ran the script's policy");

    /* The next round: a fresh VM, the count at zero, the hooks running. */
    serverSimStartGame(sim);
    serverSimTick(sim);
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "s") == 0,
                  "the round after recorded '%s', expected 's': being off is "
                  "the round's, not the attachment's", got);
    UT_ASSERT_MSG(text.count == 1,
                  "%d lines reached the game, expected the one from the first "
                  "round", text.count);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 18. The same thread, arriving at the lock twice ──────────────── */

/* The scripts for the two lock cases set one rule far outside any row's
 * range. The funnel refuses it and the host writes a console line saying so
 * — on the round start's own thread, with the VM lock held and the round's
 * VM already in place. That line is the moment each case needs. */
#define SH_LOCK_BODY                                                   \
    "scenario = { name = \"%s\", api = 1,\n"                            \
    "             rules = { tank_death_ticks = 1e12 } }\n"              \
    "function allow_extra_teams() return false end\n"

static ServerSim *shReentrySim    = NULL;
static int        shReentrySeen   = 0;
static int        shReentryAnswer = -1;

static void shReentryOnLine(const char *msg) {
    if (shReentrySeen != 0 || strstr(msg, "scenario: rule") == NULL) {
        return;
    }
    shReentrySeen   = 1;
    shReentryAnswer = shAskExtraTeams(shReentrySim) ? 1 : 0;
}

/* A policy asked from inside a VM entry the same thread is already in. A
 * plain mutex would stop dead here, which is why the lock counts depth
 * against an owning thread instead. A lock that got this wrong hangs rather
 * than fails, and CTest's per-case timeout is what catches it. */
int run_scenario_host_vm_lock_same_thread(void) {
    static const char *const kMap = "scnhost_lock_same.map";
    char          lua[1024];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    snprintf(lua, sizeof(lua), SH_LOCK_BODY, "Reenter");
    UT_ASSERT(shPut(kMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    shReentrySim    = sim;
    shReentrySeen   = 0;
    shReentryAnswer = -1;
    shWatchConsole(sim, shReentryOnLine);
    serverSimStartGame(sim);
    shUnwatchConsole(sim);
    shReentrySim = NULL;

    UT_ASSERT_MSG(shReentrySeen == 1,
                  "the refused rule wrote no console line, so the case never "
                  "reached the lock from inside the round start");
    UT_ASSERT_MSG(shReentryAnswer == 0,
                  "the policy asked from inside the start answered %d; the "
                  "script answers false, so anything else means the second "
                  "arrival did not reach the VM", shReentryAnswer);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Reenter") == 0,
                  "the round start did not finish: the name reads '%s'",
                  scenarioHostName(h));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 19. A second thread waiting for the lock ─────────────────────── */

typedef struct {
    ServerSim     *sim;
    SDL_Semaphore *entered;   /* posted once the thread is about to ask */
    SDL_AtomicInt  arrived;   /* the ask returned */
    SDL_AtomicInt  answer;    /* and what it said */
} ShAsker;

static ShAsker     shAsker;
static SDL_Thread *shAskerThread  = NULL;
static int         shAskerStarted = 0;

static int SDLCALL shAskerFn(void *data) {
    ShAsker *a = (ShAsker *)data;
    bool     allow;

    /* Says it is about to ask, then asks. The sim's own policy depth is not
       held across this one: that counter is the sim's and is written from
       one thread, and what this case is about is the host's lock. */
    SDL_SignalSemaphore(a->entered);
    allow = a->sim->scenarioPolicy->allowExtraTeams(a->sim->scenarioPolicy->ctx);
    SDL_SetAtomicInt(&a->answer, allow ? 1 : 0);
    SDL_SetAtomicInt(&a->arrived, 1);
    return 0;
}

static void shAskerOnLine(const char *msg) {
    if (shAskerStarted != 0 || strstr(msg, "scenario: rule") == NULL) {
        return;
    }
    shAskerStarted = 1;
    shAskerThread  = SDL_CreateThread(shAskerFn, "scn-policy-asker", &shAsker);
    if (shAskerThread != NULL) {
        /* Wait for the thread to say it is about to ask rather than for a
           stretch of time: the case is about what arrives, and a case that
           waited on the clock would fail on a loaded machine. */
        SDL_WaitSemaphore(shAsker.entered);
    }
}

/* A second thread asks the policy while the first is inside the VM. What is
 * asserted is the arrival, once the first thread has left: the thread
 * records that its ask returned and what it returned, and the record is read
 * after SDL_WaitThread, which is the only synchronisation the case needs. */
int run_scenario_host_vm_lock_second_thread(void) {
    static const char *const kMap = "scnhost_lock_second.map";
    char          lua[1024];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    snprintf(lua, sizeof(lua), SH_LOCK_BODY, "Waiter");
    UT_ASSERT(shPut(kMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    shAsker.sim     = sim;
    shAsker.entered = SDL_CreateSemaphore(0);
    UT_ASSERT(shAsker.entered != NULL);
    SDL_SetAtomicInt(&shAsker.arrived, 0);
    SDL_SetAtomicInt(&shAsker.answer, -1);
    shAskerThread  = NULL;
    shAskerStarted = 0;

    shWatchConsole(sim, shAskerOnLine);
    serverSimStartGame(sim);
    shUnwatchConsole(sim);

    UT_ASSERT_MSG(shAskerStarted == 1,
                  "the refused rule wrote no console line, so no second "
                  "thread ever asked");
    UT_ASSERT_MSG(shAskerThread != NULL, "the second thread did not start");

    /* The round start has returned, so the lock is free. */
    SDL_WaitThread(shAskerThread, NULL);
    shAskerThread = NULL;
    UT_ASSERT_MSG(SDL_GetAtomicInt(&shAsker.arrived) == 1,
                  "the second thread never recorded getting through");
    UT_ASSERT_MSG(SDL_GetAtomicInt(&shAsker.answer) == 0,
                  "the second thread read %d; the script answers false, so "
                  "anything else means it did not reach the VM",
                  SDL_GetAtomicInt(&shAsker.answer));

    SDL_DestroySemaphore(shAsker.entered);
    shAsker.entered = NULL;
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 20. The audit, on a round that lost a human ──────────────────── */

static ServerSim *shLossSim       = NULL;
static int        shLossTaken     = 0;
static int        shLossGivenBack = 0;

/* A human gone between the roster the round start noted and the audit that
 * follows the setup call. The ops refuse to remove a human, so the loss is
 * made here rather than asked for: the seat is marked empty, which is the
 * shape a connection that has gone leaves behind.
 *
 * Two console lines drive it, both written on the round start's own thread.
 * The first is the one the host writes when on_setup raises, which comes
 * after the roster was noted and before the audit reads it again. The second
 * is the audit's own line, by which point the audit has seen what it needed
 * to see — so the seat is given back, and the rest of the round runs on a
 * roster that is whole. */
static void shLossOnLine(const char *msg) {
    if (shLossSim == NULL) {
        return;
    }
    if (shLossTaken == 0 && strstr(msg, "on_setup raised") != NULL) {
        shLossTaken = 1;
        shLossSim->playerConnected[0] = false;
        return;
    }
    if (shLossTaken == 1 && shLossGivenBack == 0 &&
        strstr(msg, "is off for the rest of the round") != NULL) {
        shLossGivenBack = 1;
        shLossSim->playerConnected[0] = true;
    }
}

int run_scenario_host_audit_human_lost(void) {
    static const char *const kMap = "scnhost_audit.map";
    static const char *const kLua =
        "scenario = { name = \"Loser\", api = 1 }\n"
        "function on_setup() error(\"no\") end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    ShText        text;
    char          err[512];

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    /* A human in the round before it starts, which is the roster the audit
       holds the one after the setup call against. */
    serverSimAddPlayer(sim, 0, "Tester", false);
    UT_ASSERT_MSG(serverSimIsPlayerConnected(sim, 0),
                  "the case seated nobody to lose");

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    shLossSim       = sim;
    shLossTaken     = 0;
    shLossGivenBack = 0;
    shWatchConsole(sim, shLossOnLine);
    serverSimStartGame(sim);
    shUnwatchConsole(sim);
    shLossSim = NULL;

    UT_ASSERT_MSG(shLossTaken == 1,
                  "the setup that raised wrote no console line, so the case "
                  "never took the player out");
    UT_ASSERT_MSG(shLossGivenBack == 1,
                  "the audit wrote no line, so it did not notice the player "
                  "was gone");
    UT_ASSERT_MSG(serverSimIsPlayerConnected(sim, 0),
                  "the case left the seat empty");

    /* And the line reaches the bus every client's delivery reads from. */
    shWatchText(sim, &text);
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 1,
                  "the audit sent %d lines to the game, expected one",
                  text.count);
    UT_ASSERT_MSG(strstr(text.last, "Loser") != NULL,
                  "the line does not name the scenario: %s", text.last);
    UT_ASSERT_MSG(strstr(text.last, "dropped player 0") != NULL,
                  "the line does not say which seat was lost: %s", text.last);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 21. A metatable that raises ──────────────────────────────────── */

/* Two files, both of which ended the process before the reads below went
 * raw: a __index that raises runs with no lua_pcall between it and the VM,
 * and Lua's answer to that is to end the process.
 *
 * The first puts it on _G, which is what the host's global reads went
 * through. The second puts it on the scenario table, which is what the
 * field reads and the hook fallback went through. Neither file is anything
 * but an ordinary script with a metatable on it. */
int run_scenario_host_metatable_raises(void) {
    static const char *const kGlobalMap = "scnhost_mt_globals.map";
    static const char *const kGlobalLua =
        "setmetatable(_G, { __index = function(t, k)\n"
        "  error(\"_G.\" .. tostring(k) .. \" ran a metamethod\")\n"
        "end })\n";
    static const char *const kTableMap = "scnhost_mt_table.map";
    static const char *const kTableLua =
        "scenario = setmetatable({}, { __index = function(t, k)\n"
        "  error(\"scenario.\" .. tostring(k) .. \" ran a metamethod\")\n"
        "end })\n";
    ServerSim              *sim;
    ScenarioHost           *h;
    const ScenarioManifest *m;
    char                    err[512];
    int                     i;

    /* One: the scenario global is only reachable through the metatable, so
       the raw read finds nothing and the attach is refused in words. */
    UT_ASSERT(shPut(kGlobalMap, kGlobalLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, kGlobalMap, err, sizeof(err));
    UT_ASSERT_MSG(h == NULL,
                  "a file whose scenario table is only behind a _G metatable "
                  "was accepted");
    UT_ASSERT_MSG(err[0] != '\0', "the refusal produced no operator line");
    UT_ASSERT_MSG(strstr(err, "scnhost_mt_globals") != NULL,
                  "the line does not name the file: %s", err);

    serverSimDestroy(sim);
    shDrop(kGlobalMap);

    /* Two: the table is there and every field read of it would have run the
       metamethod. The attach comes back, and what the metamethod would have
       handed over is not in the manifest. */
    UT_ASSERT(shPut(kTableMap, kTableLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, kTableMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    m = scenarioHostManifest(h);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(m->name[0] == '\0', "name read as '%s'", m->name);

    /* And again through the round start, which re-runs the chunk and is
       where the hook fallback reads the same table. */
    serverSimStartGame(sim);
    for (i = 0; i < 5; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(scenarioHostIsActive(h),
                  "the round start left the host with no scenario");

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kTableMap);
    return 0;
}

/* ── 22. A metatable that answers ─────────────────────────────────── */

/* The other half of the same change. This __index raises nothing and hands
 * back a value for every key the table does not carry; a raw read never
 * asks it, so the manifest holds what the file wrote and nothing else. */
int run_scenario_host_metatable_not_read(void) {
    static const char *const kMap = "scnhost_mt_answers.map";
    static const char *const kLua =
        "local ghost = { max_players = 9, teams = { { id = 3 } } }\n"
        "scenario = setmetatable({ name = \"Declared\", api = 1 }, {\n"
        "  __index = function(t, k)\n"
        "    if k == \"lobby\" then return ghost end\n"
        "    return \"ghost\"\n"
        "  end,\n"
        "})\n";
    ServerSim              *sim;
    ScenarioHost           *h;
    const ScenarioManifest *m;
    char                    err[512];

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    m = scenarioHostManifest(h);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(strcmp(m->name, "Declared") == 0,
                  "name read as '%s': a key the file carries is still read",
                  m->name);
    UT_ASSERT_MSG(m->description[0] == '\0',
                  "description read as '%s', which the file does not carry",
                  m->description);
    UT_ASSERT_MSG(m->game[0] == '\0',
                  "game read as '%s', which the file does not carry", m->game);
    UT_ASSERT_MSG(m->lobby.maxPlayers == 0,
                  "max_players read as %u from a lobby the file does not "
                  "carry", (unsigned)m->lobby.maxPlayers);
    UT_ASSERT_MSG(m->lobby.numTeams == 0, "%u teams, expected none",
                  (unsigned)m->lobby.numTeams);
    UT_ASSERT_MSG(m->numRules == 0, "%u rules, expected none",
                  (unsigned)m->numRules);
    UT_ASSERT_MSG(m->numRegions == 0, "%u regions, expected none",
                  (unsigned)m->numRegions);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 23. A hook behind a _G metatable ─────────────────────────────── */

/* A hook name the script never wrote as a global, answered by a metatable
 * on _G. The resolution reads the globals table itself, so the name is not
 * there and the hook is not called. on_start is written as a global in the
 * same file and still runs, which is what says the round got as far as the
 * hooks at all. */
int run_scenario_host_hook_via_global_metatable(void) {
    static const char *const kMap    = "scnhost_mt_hook.map";
    static const char *const kRecord = "scnhost_mt_hook.record";
    static const char *const kBody =
        "scenario = { name = \"Hooked\", api = 1 }\n"
        "function on_start() note(\"T\") end\n"
        "setmetatable(_G, { __index = function(t, k)\n"
        "  if k == \"on_setup\" then return function() note(\"S\") end end\n"
        "  return nil\n"
        "end })\n";
    char          lua[2048];
    char          got[64];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    remove(kRecord);
    shScript(lua, sizeof(lua), kRecord, kBody);
    UT_ASSERT(shPut(kMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(got[0] == '\0',
                  "the round start recorded '%s': on_setup was taken from the "
                  "_G metatable", got);

    serverSimTick(sim);
    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(strcmp(got, "T") == 0,
                  "the first running tick recorded '%s', expected the one 'T' "
                  "from the hook the file writes as a global", got);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 24. Scripts off, with a script beside the map ────────────────── */

/* The switch is file-scope state on the library and CTest runs cases as
 * separate processes, but a case is not the only thing in its process: the
 * three cases below put it back to enabled before their first assert, so a
 * failure leaves it where the rest of the run expects it rather than off. */
int run_scenario_host_disabled_refuses_script(void) {
    static const char *const kMap = "scnhost_off_script.map";
    static const char *const kLua =
        "scenario = { name = \"Refused\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    scenarioHostSetEnabled(false);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));

    /* Everything the case has to put back, ahead of the first assert. */
    scenarioHostSetEnabled(true);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);

    UT_ASSERT_MSG(h == NULL,
                  "a script was attached with scripts off");
    UT_ASSERT_MSG(err[0] != '\0',
                  "the refusal produced no operator line");
    UT_ASSERT_MSG(strstr(err, "scnhost_off_script") != NULL,
                  "the line does not name the map's script: %s", err);
    UT_ASSERT_MSG(strstr(err, SCN_SCRIPT_SUFFIX) != NULL,
                  "the line does not name the file: %s", err);
    return 0;
}

/* ── 25. Scripts off, with nothing beside the map ─────────────────── */

/* The ordinary map, which must stay as quiet as it was: an operator
 * rotating through a directory of plain maps should see nothing at all. */
int run_scenario_host_disabled_plain_map(void) {
    static const char *const kMap = "scnhost_off_plain.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    shDrop(kMap);                /* whatever an earlier run left beside it */
    sim = shSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    scenarioHostSetEnabled(false);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));

    scenarioHostSetEnabled(true);
    scenarioHostDetach(h);
    serverSimDestroy(sim);

    UT_ASSERT_MSG(h == NULL, "a map with no script beside it attached one");
    UT_ASSERT_MSG(err[0] == '\0',
                  "a map with no script beside it said '%s'", err);
    return 0;
}

/* ── 26. And the switch back on ───────────────────────────────────── */

int run_scenario_host_enabled_again(void) {
    static const char *const kMap = "scnhost_off_then_on.map";
    static const char *const kLua =
        "scenario = { name = \"Back\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    char          name[SCN_SCENARIO_NAME_LEN];
    bool          refusedWhileOff;
    bool          attachedWhenOn;

    UT_ASSERT(shPut(kMap, kLua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    scenarioHostSetEnabled(false);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    refusedWhileOff = (h == NULL);
    scenarioHostDetach(h);

    err[0] = '\0';
    scenarioHostSetEnabled(true);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    attachedWhenOn = (h != NULL);
    snprintf(name, sizeof(name), "%s", scenarioHostName(h));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);

    UT_ASSERT_MSG(refusedWhileOff, "a script was attached with scripts off");
    UT_ASSERT_MSG(attachedWhenOn,
                  "the script was refused with scripts on: %s", err);
    UT_ASSERT_MSG(strcmp(name, "Back") == 0, "name read as '%s'", name);
    return 0;
}

/* ── 27. A round start that fails drops the round before it ───────── */

/* A script that loaded at the attach and raises from its own top level at a
 * later round start. That round plays classic, which the host has always
 * done; what it must not do is go on answering for a scenario that is not
 * running — the name and the description the lobby shows, and the regions
 * every tick would walk for a round with no script to tell about them.
 *
 * The marker file is what makes the same bytes load once and raise the next
 * time. The host reads the file at the attach and runs those bytes at every
 * start, so the difference between the two rounds has to be something the
 * chunk itself can see. */
int run_scenario_host_failed_start_drops_manifest(void) {
    static const char *const kMap    = "scnhost_failed_start.map";
    static const char *const kMarker = "scnhost_failed_start.marker";
    char                    lua[1024];
    ServerSim              *sim;
    ScenarioHost           *h;
    const ScenarioManifest *m;
    FILE                   *f;
    char                    err[512];

    remove(kMarker);
    snprintf(lua, sizeof(lua),
             "local stop = io.open(\"%s\", \"r\")\n"
             "if stop then stop:close() error(\"not this round\") end\n"
             "scenario = { name = \"Standing\", api = 1,\n"
             "             description = \"Hold the keep\",\n"
             "             regions = { keep = { x = 100, y = 100,\n"
             "                                  w = 12, h = 12 } } }\n"
             "function allow_extra_teams() return false end\n", kMarker);
    UT_ASSERT(shPut(kMap, lua));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    /* The round the script does run, so the next one has something to
       forget. */
    serverSimStartGame(sim);
    serverSimTick(sim);
    m = scenarioHostManifest(h);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Standing") == 0,
                  "the first round's name read as '%s'", scenarioHostName(h));
    UT_ASSERT_MSG(m->numRegions == 1,
                  "the first round holds %u regions, expected the one the "
                  "file declares", (unsigned)m->numRegions);
    UT_ASSERT_MSG(!shAskExtraTeams(sim),
                  "the first round answered the classic yes, so its script "
                  "never ran at all");

    /* Written between the rounds, so the next start is the one that
       raises. */
    f = fopen(kMarker, "wb");
    UT_ASSERT_MSG(f != NULL, "the marker file could not be written");
    fclose(f);

    serverSimStartGame(sim);
    serverSimTick(sim);

    UT_ASSERT_MSG(scenarioHostName(h)[0] == '\0',
                  "the failed start left the name as '%s': the lobby would "
                  "still be showing the round before it", scenarioHostName(h));
    UT_ASSERT_MSG(scenarioHostDescription(h)[0] == '\0',
                  "the failed start left the description as '%s'",
                  scenarioHostDescription(h));
    UT_ASSERT_MSG(m->numRegions == 0,
                  "the failed start left %u regions for every tick of a round "
                  "with no script to walk", (unsigned)m->numRegions);
    UT_ASSERT_MSG(shAskExtraTeams(sim),
                  "the round without a scenario still ran the script's "
                  "policy");

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    remove(kMarker);
    return 0;
}

/* ── 28. A reload checks the file and applies nothing ─────────────── */

/* The edited file is loaded in a state of its own to find out whether it can
 * be used, and loading it runs its top level. That state reads the live sim,
 * because a file that reads the sim as it loads has to be able to — but
 * every row that writes refuses while it does, or a file whose top level
 * ends a round or seats a bot would do it to the round that is playing, from
 * a console command that says it changed nothing until the next round.
 *
 * What the two rows answered is recorded from inside the chunk, since the
 * state they ran in is closed before the reload returns. The lobby row would
 * have been refused by the round's state in any case, so the sentence it
 * carries is what says which refusal it was. */
int run_scenario_host_reload_applies_nothing(void) {
    static const char *const kMap    = "scnhost_reload_writes.map";
    static const char *const kRecord = "scnhost_reload_writes.record";
    static const char *const kFirst =
        "scenario = { name = \"Quiet\", api = 1 }\n";
    static const char *const kBody =
        "local a, msg = game.message(\"x\")\n"
        "note(\"msg=\" .. tostring(msg) .. \";\")\n"
        "local b, bot, detail = game.lobby_add_bot({ team = 2,"
        " name = \"Ghost\" })\n"
        "note(\"bot=\" .. tostring(bot) .. \";\")\n"
        "note(\"detail=\" .. tostring(detail) .. \";\")\n"
        "scenario = { name = \"Loud\", api = 1 }\n";
    char          lua[1024];
    char          got[256];
    ServerSim    *sim;
    ScenarioHost *h;
    ShText        text;
    char          err[512];
    BYTE          before;

    remove(kRecord);
    UT_ASSERT(shPut(kMap, kFirst));
    sim = shSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    serverSimTick(sim);
    shWatchText(sim, &text);
    before = serverSimGetNumPlayers(sim);

    shScript(lua, sizeof(lua), kRecord, kBody);
    UT_ASSERT(shPut(kMap, lua));
    err[0] = '\0';
    UT_ASSERT_MSG(scenarioHostReload(h, err, sizeof(err)),
                  "the reload was refused: %s", err);

    shRead(kRecord, got, sizeof(got));
    UT_ASSERT_MSG(strstr(got, "msg=SCN_OP_WRONG_STATE") != NULL,
                  "the top level's line to the players answered '%s', and a "
                  "reload sends none", got);
    UT_ASSERT_MSG(strstr(got, "bot=SCN_OP_WRONG_STATE") != NULL,
                  "the top level's lobby seat answered '%s'", got);
    UT_ASSERT_MSG(strstr(got, "applies nothing") != NULL,
                  "the refusal does not say it was the reload's: '%s'", got);

    UT_ASSERT_MSG(text.count == 0,
                  "%d lines reached the game from a file that was only being "
                  "checked", text.count);
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == before,
                  "the roster went from %u to %u over a reload",
                  (unsigned)before, (unsigned)serverSimGetNumPlayers(sim));

    /* And the file is the one the host now holds, so the check really did
       run the edited bytes rather than stopping short of them. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(strcmp(scenarioHostName(h), "Loud") == 0,
                  "the round after the reload reads '%s'", scenarioHostName(h));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    shDrop(kMap);
    remove(kRecord);
    return 0;
}
