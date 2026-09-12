/*
 * The scenario host: a Lua sidecar beside a map, read into the manifest and
 * applied to a round.
 *
 * Each case writes its own fixture sidecar and removes it afterwards. The
 * names are per-case on purpose: CTest runs cases as separate processes in
 * one directory, so a shared fixture name is a race rather than a fixture.
 *
 * The map file itself is never written. The host reads the sidecar beside a
 * map path and nothing else, so a path that names no real map is enough to
 * drive it, and the sims here are built from the built-in map.
 *
 * run_scenario_host_metadata          — name, description, api, game and the
 *                                       lobby block arrive as written
 * run_scenario_host_rules_change_round— a rules sidecar changes how a round
 *                                       plays, observed on a tank
 * run_scenario_host_syntax_error_line — a broken sidecar is refused, naming
 *                                       the file and the line
 * run_scenario_host_unknown_rule_key  — a key that names no rule is refused
 *                                       by name and the rest still applies
 * run_scenario_host_api_too_new       — an api above this server's is
 *                                       refused; equal and below are not
 * run_scenario_host_no_sidecar        — no sidecar is not an error
 * run_scenario_host_manifest_roundtrip— tags, regions and several teams
 * run_scenario_host_seed_reproducible — math.random is seeded from the
 *                                       process PRNG state, reproducibly
 * run_scenario_host_edit_after_attach — the sidecar is read once, so an
 *                                       edit does not reach the next round
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "bolo_rand.h"             /* bolo_srand — the seeding case */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.rules, tanks[] */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "server_sim_scenario.h"   /* the kill op the rules case uses */
#include "game_sim.h"
#include "tank.h"                  /* tankIsDestroyed, TANK_DEATH_WAIT */
#include "everard_map.h"
#include "scenario_host.h"
#include "scenario_manifest.h"
#include "test_harness.h"

/* ── Fixtures ─────────────────────────────────────────────────────── */

/* A sidecar sits beside the map: X.map is accompanied by X.scenario.lua. */
static void shSidecarFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SIDECAR_SUFFIX);
}

static bool shPut(const char *mapPath, const char *lua) {
    char  side[512];
    FILE *f;
    shSidecarFor(mapPath, side, sizeof(side));
    f = fopen(side, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static void shDrop(const char *mapPath) {
    char side[512];
    shSidecarFor(mapPath, side, sizeof(side));
    remove(side);
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
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
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

/* ── 2. A rules sidecar changes a round ───────────────────────────── */

/* Well above the classic wait, and inside tank_death_ticks' 0..65535 row. */
#define SH_DEATH_SET       400
#define SH_PAST_CLASSIC    (TANK_DEATH_WAIT + 20)
#define SH_RESPAWN_GIVE_UP (SH_DEATH_SET * 4)

/* The field the sidecar sets is checked by watching a tank rather than by
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
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);

    /* The round start invokes the host, which applies the table. */
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Tester", false);
    t = shTank(sim, 0);
    UT_ASSERT_MSG(*t != NULL, "the player has no tank");

    UT_ASSERT_MSG(shKill(sim, 0) == SCN_OP_OK, "the kill was refused");
    UT_ASSERT_MSG(tankIsDestroyed(t), "the kill left the tank alive");
    UT_ASSERT_MSG(tankGetDeathWait(t) == SH_DEATH_SET,
                  "the tank started a wait of %u, expected the sidecar's %d",
                  (unsigned)tankGetDeathWait(t), SH_DEATH_SET);

    for (frames = 0; frames < SH_PAST_CLASSIC; frames++) {
        serverSimTick(sim);
        if (!tankIsDestroyed(t)) {
            respawnedAt = frames + 1;
            break;
        }
    }
    UT_ASSERT_MSG(respawnedAt == 0,
                  "the tank came back on frame %d — under the sidecar's %d it "
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
                  "the tank was still down after %d frames with the sidecar's "
                  "tank_death_ticks of %d", SH_RESPAWN_GIVE_UP, SH_DEATH_SET);
    UT_ASSERT_MSG(respawnedAt >= SH_DEATH_SET,
                  "the tank came back on frame %d, short of the %d the "
                  "sidecar asked for", respawnedAt, SH_DEATH_SET);

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
    UT_ASSERT_MSG(h == NULL, "a sidecar that does not parse was accepted");
    UT_ASSERT_MSG(err[0] != '\0', "a broken sidecar produced no operator line");
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
                  "one bad key cost the whole sidecar: %s", err);
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
                  "the sidecar set beside the bad key",
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
    UT_ASSERT_MSG(h == NULL, "a sidecar written for a newer server was run");
    UT_ASSERT_MSG(err[0] != '\0', "the refusal produced no operator line");
    UT_ASSERT_MSG(strstr(err, "api") != NULL,
                  "the line does not mention the api: %s", err);

    /* Equal: accepted. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Current\", api = %d }\n",
             SCENARIO_API_VERSION);
    UT_ASSERT(shPut(kMap, lua));
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "a sidecar at this server's api was refused: %s",
                  err);
    scenarioHostDetach(h);

    /* Below: accepted too — an older file runs on a newer server. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Older\", api = %d }\n",
             SCENARIO_API_VERSION - 1);
    UT_ASSERT(shPut(kMap, lua));
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "a sidecar at an older api was refused: %s", err);
    scenarioHostDetach(h);

    serverSimDestroy(sim);
    shDrop(kMap);
    return 0;
}

/* ── 6. No sidecar at all ─────────────────────────────────────────── */

int run_scenario_host_no_sidecar(void) {
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
    UT_ASSERT_MSG(h == NULL, "a map with no sidecar produced a host");
    UT_ASSERT_MSG(err[0] == '\0',
                  "a map with no sidecar produced an operator line: %s", err);
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
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
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
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    snprintf(first, sizeof(first), "%s", scenarioHostName(h));
    scenarioHostDetach(h);
    UT_ASSERT_MSG(first[0] != '\0', "the chunk produced no draw");

    bolo_srand(0xA11CEULL);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused on the second boot: %s",
                  err);
    snprintf(again, sizeof(again), "%s", scenarioHostName(h));
    scenarioHostDetach(h);
    UT_ASSERT_MSG(strcmp(first, again) == 0,
                  "two boots under one process seed drew '%s' then '%s' — the "
                  "round does not replay", first, again);

    bolo_srand(0xB0BULL);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused on the third boot: %s",
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

/* ── 9. The sidecar is read once ──────────────────────────────────── */

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
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
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
