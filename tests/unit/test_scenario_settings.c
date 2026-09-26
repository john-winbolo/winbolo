/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * A script's own settings: choices the host makes in the lobby, declared in
 * the script's scenario table (settings = { { id, label, type, min, max,
 * step, default }, ... }), read without running the script, chosen in the
 * details dialog, checked by the server, sent to every client, and read by
 * the script with game.setting(id).
 *
 *   scenario_settings_blob            — the packed block (scenario_settings.h):
 *       rows written and read back, a row of a type this build does not know
 *       skipped whole, cut and padded bytes refused, and the three rules a
 *       value is held to (resolve, clamp, problem).
 *   scenario_settings_manifest_lua    — a loose script checked by the
 *       validator: the good rows kept in order, every bad row reported by
 *       key and dropped, and the directory listing carrying the same rows.
 *   scenario_settings_manifest_json   — the same rules for a package's
 *       manifest.json, a manifest written from the struct keeping its
 *       settings, and scnManifestAgrees naming a setting the two disagree on.
 *   scenario_settings_server_clamp    — serverSimSetScriptSetting: a value in
 *       range kept, one above clamped to the top entry, one below to the
 *       bottom, one off the step and the default itself stored as nothing,
 *       an id or a file with no declaration refused, and each change
 *       published; the join sync sends a CLEAR and then every kept value.
 *   scenario_settings_codec           — CTRL_LOBBY_SCRIPT_SETTING and
 *       CMD_SET_SCRIPT_SETTING through their encoders and decoders, and the
 *       bodies they refuse.
 *   scenario_settings_client_apply    — the ClientSim side: a CLEAR and SETs
 *       applied, the flag that says the server takes a change, and the
 *       declaration kept beside the details and cleared by a new answer.
 *   scenario_settings_game_setting    — game.setting(id) at a file's top
 *       level and inside a hook: the default when nothing is chosen, the
 *       host's value when one is, and a raise for an id never declared.
 *   scenario_settings_wire            — the whole trip over the loopback
 *       transport under loss: the join sync marks the server as one that
 *       takes a change, the details answer carries the declaration, and the
 *       host's pick comes back clamped from the server.
 *   scenario_settings_survival_decl   — the shipped Survival map committed:
 *       its script declares "round_minutes" 1..10 default 4 and "rounds"
 *       1..5 default 5, which is the round it always played.
 *   scenario_settings_survival_short  — Survival started through the lobby
 *       with rounds = 1 and round_minutes = 1: one wave of one minute, then
 *       the defenders win.
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
#include "channel_mux.h"                /* CHANNEL_CONTROL_SEG */
#include "client_command.h"
#include "client_connect_state.h"
#include "client_net.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"         /* clientSimApplyControl */
#include "control_event.h"
#include "everard_map.h"
#include "game_sim.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "server_sim_scenario.h"
#include "server_sim_join.h"            /* serverSimFindFreeSlot */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive */
#include "scenario_defs.h"
#include "scenario_dir.h"
#include "scenario_host.h"
#include "scenario_manifest_json.h"
#include "scenario_settings.h"
#include "scenario_validate.h"
#include "threads.h"
#include "transport_command_codec.h"
#include "transport_control_codec.h"
#include "test_harness.h"
#include "loopback_harness.h"

#ifndef WB_DATA_MAPS_DIR
#define WB_DATA_MAPS_DIR "data/maps"
#endif

/* ── The directory ────────────────────────────────────────────────── */

static char ssDir[256];

static void ssRemoveTree(const char *path) {
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
static bool ssMakeDir(const char *tag) {
    snprintf(ssDir, sizeof(ssDir), "wbtest_scenario_settings_%s", tag);
    ssRemoveTree(ssDir);
    return SDL_CreateDirectory(ssDir);
}

static bool ssWriteText(const char *name, const char *text) {
    char  path[512];
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), "%s/%s", ssDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    fclose(f);
    return ok;
}

static ScnSetting ssRow(const char *id, const char *label, int32_t min,
                        int32_t max, int32_t step, int32_t def) {
    ScnSetting s;

    memset(&s, 0, sizeof(s));
    SDL_strlcpy(s.id, id, sizeof(s.id));
    SDL_strlcpy(s.label, label, sizeof(s.label));
    s.type = SCN_SETTING_TYPE_INT;
    s.min  = min;
    s.max  = max;
    s.step = step;
    s.def  = def;
    return s;
}

static bool ssSame(const ScnSetting *a, const ScnSetting *b) {
    return strcmp(a->id, b->id) == 0 && strcmp(a->label, b->label) == 0 &&
           a->type == b->type && a->min == b->min && a->max == b->max &&
           a->step == b->step && a->def == b->def;
}

/* ── 1. The block ─────────────────────────────────────────────────── */

int run_scenario_settings_blob(void) {
    uint8_t    blob[SCN_SETTINGS_BLOB_MAX];
    uint8_t    odd[SCN_SETTINGS_BLOB_MAX];
    ScnSetting rows[SCN_SETTINGS_MAX];
    ScnSetting a = ssRow("round_minutes", "Round length (minutes)", 1, 10, 1, 4);
    ScnSetting b = ssRow("gap", "Gap", 0, 100, 25, 50);
    ScnSetting bad;
    size_t     len = 0;
    size_t     oddLen;
    int        n;

    /* No rows is no bytes. */
    UT_ASSERT(scnSettingsBlobRead(blob, 0, rows, SCN_SETTINGS_MAX) == 0);

    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &a));
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &b));
    /* A row that breaks a rule is not written. */
    bad = ssRow("bad", "Bad", 1, 10, 0, 4);
    UT_ASSERT(!scnSettingsBlobAppend(blob, sizeof(blob), &len, &bad));
    UT_ASSERT(blob[0] == 2);
    n = scnSettingsBlobRead(blob, len, rows, SCN_SETTINGS_MAX);
    UT_ASSERT_MSG(n == 2, "%d rows read back, wanted 2", n);
    UT_ASSERT(ssSame(&rows[0], &a) && ssSame(&rows[1], &b));

    /* A row of a type this build does not know is skipped whole, so a newer
       server's bool or choice row leaves the int rows around it readable. */
    memcpy(odd, blob, len);
    oddLen = len;
    odd[0] = 3;
    odd[oddLen++] = 7;      /* type */
    odd[oddLen++] = 3;      /* body length */
    odd[oddLen++] = 'x';
    odd[oddLen++] = 'y';
    odd[oddLen++] = 'z';
    n = scnSettingsBlobRead(odd, oddLen, rows, SCN_SETTINGS_MAX);
    UT_ASSERT_MSG(n == 2, "%d rows read past an unknown type, wanted 2", n);

    /* Bytes that are not a block. */
    UT_ASSERT(scnSettingsBlobRead(blob, len - 1, rows, SCN_SETTINGS_MAX) < 0);
    memcpy(odd, blob, len);
    odd[len] = 0;
    UT_ASSERT(scnSettingsBlobRead(odd, len + 1, rows, SCN_SETTINGS_MAX) < 0);
    odd[0] = SCN_SETTINGS_MAX + 1;
    UT_ASSERT(scnSettingsBlobRead(odd, len, rows, SCN_SETTINGS_MAX) < 0);

    /* The value rules. */
    UT_ASSERT(scnSettingResolve(&a, false, 9) == 4);
    UT_ASSERT(scnSettingResolve(&a, true, 9) == 9);
    UT_ASSERT(scnSettingResolve(&a, true, 11) == 4);
    UT_ASSERT(scnSettingResolve(&b, true, 30) == 50);
    UT_ASSERT(scnSettingClamp(&a, 99) == 10);
    UT_ASSERT(scnSettingClamp(&a, -5) == 1);
    UT_ASSERT(scnSettingClamp(&a, 7) == 7);
    UT_ASSERT(scnSettingClamp(&b, 30) == 50);    /* off the step */
    UT_ASSERT(scnSettingClamp(&b, 101) == 100);
    {
        /* The top entry is the last one on the step, not max. */
        ScnSetting c = ssRow("c", "C", 1, 10, 4, 5);
        UT_ASSERT(scnSettingClamp(&c, 50) == 9);
        UT_ASSERT(scnSettingChoices(&c) == 3);
    }

    /* The problems, one per rule. */
    UT_ASSERT(scnSettingProblem(&a) == NULL);
    bad = ssRow("has space", "L", 1, 2, 1, 1);
    UT_ASSERT(scnSettingProblem(&bad) != NULL);
    bad = ssRow("x", "", 1, 2, 1, 1);
    UT_ASSERT(scnSettingProblem(&bad) != NULL);
    bad = ssRow("x", "L", 5, 2, 1, 3);
    UT_ASSERT(scnSettingProblem(&bad) != NULL);
    bad = ssRow("x", "L", 1, 5, 1, 9);
    UT_ASSERT(scnSettingProblem(&bad) != NULL);
    bad = ssRow("x", "L", 0, 10, 5, 3);
    UT_ASSERT(scnSettingProblem(&bad) != NULL);
    bad = ssRow("x", "L", 1, 101, 1, 1);
    UT_ASSERT_MSG(scnSettingProblem(&bad) != NULL,
                  "a range of 101 entries was allowed");
    bad = ssRow("x", "L", 1, 100, 1, 1);
    UT_ASSERT_MSG(scnSettingProblem(&bad) == NULL,
                  "a range of exactly 100 entries was refused");
    return 0;
}

/* ── 2. A loose script ────────────────────────────────────────────── */

/* The head of the table and its two good rows. */
#define SS_SCRIPT_HEAD                                                 \
    "scenario = {\n"                                                   \
    "  name = \"Knobs\", api = 1, kind = \"mod\", bound = false,\n"     \
    "  settings = {\n"                                                 \
    "    { id = \"armour\", label = \"Base armour\", type = \"int\",\n" \
    "      min = 100, max = 200, step = 10, default = 120 },\n"        \
    "    { id = \"shells\", label = \"Tank shells\",\n"                \
    "      min = 10, max = 60, default = 20 },\n"

/* The end of the table and the code that reads the two good rows: armour at
 * the file's top level, shells inside on_setup, and an id never declared
 * under pcall (tank_full_mines 2 when it raised). */
#define SS_SCRIPT_TAIL                                                 \
    "  },\n"                                                           \
    "}\n"                                                              \
    "local A = game.setting(\"armour\")\n"                             \
    "local S = 0\n"                                                    \
    "function on_setup()\n"                                            \
    "  S = game.setting(\"shells\")\n"                                 \
    "  game.set_rule(\"base_full_armour\", A)\n"                       \
    "  game.set_rule(\"tank_full_shells\", S)\n"                       \
    "  local ok = pcall(game.setting, \"nope\")\n"                     \
    "  game.set_rule(\"tank_full_mines\", ok and 1 or 2)\n"            \
    "end\n"

/* The two good rows only: a round of it plays with no complaint. */
static const char kSsCleanScript[] = SS_SCRIPT_HEAD SS_SCRIPT_TAIL;

/* The two good rows and one bad row for every rule, each dropped. */
static const char kSsScript[] =
    SS_SCRIPT_HEAD
    "    { id = \"armour\", label = \"Again\", min = 1, max = 2, default = 1 },\n"
    "    { id = \"far\", label = \"Far\", min = 1, max = 5, default = 9 },\n"
    "    { id = \"flat\", label = \"Flat\", min = 1, max = 5, step = 0,\n"
    "      default = 1 },\n"
    "    { id = \"big\", label = \"Big\", min = 1, max = 1000, default = 1 },\n"
    "    { id = \"flag\", label = \"Flag\", type = \"bool\", min = 0,\n"
    "      max = 1, default = 0 },\n"
    "    { id = \"nomax\", label = \"No max\", min = 1, default = 1 },\n"
    SS_SCRIPT_TAIL;

static bool ssIssue(const ScnValidateResult *r, const char *key) {
    int i;

    for (i = 0; i < (int)r->count; i++) {
        if (strcmp(r->issues[i].key, key) == 0) return true;
    }
    return false;
}

int run_scenario_settings_manifest_lua(void) {
    static ScnValidateResult r;
    ScnDirEntry              list[4];
    ScnDirDetails            det[4];
    ScnSetting               rows[SCN_SETTINGS_MAX];
    char                     path[512];
    const ScenarioManifest  *m = &r.manifest;
    int                      n;
    int                      i;
    int                      found = -1;

    UT_ASSERT(ssMakeDir("lua"));
    UT_ASSERT(ssWriteText("knobs.lua", kSsScript));
    snprintf(path, sizeof(path), "%s/knobs.lua", ssDir);

    memset(&r, 0, sizeof(r));
    (void)scenarioValidateScript(NULL, path, &r);
    UT_ASSERT_MSG(r.haveManifest, "the script did not run under the "
                  "validator (game.setting at its top level?)");
    UT_ASSERT_MSG(m->numSettings == 2, "%d settings kept, wanted 2",
                  (int)m->numSettings);
    {
        ScnSetting wantA = ssRow("armour", "Base armour", 100, 200, 10, 120);
        ScnSetting wantS = ssRow("shells", "Tank shells", 10, 60, 1, 20);
        UT_ASSERT_MSG(ssSame(&m->settings[0], &wantA),
                      "armour read as %s %d..%d/%d def %d", m->settings[0].id,
                      (int)m->settings[0].min, (int)m->settings[0].max,
                      (int)m->settings[0].step, (int)m->settings[0].def);
        UT_ASSERT_MSG(ssSame(&m->settings[1], &wantS),
                      "shells did not take step 1 by default");
    }
    UT_ASSERT_MSG(ssIssue(&r, "settings.armour"),
                  "the duplicate id was not reported");
    UT_ASSERT(ssIssue(&r, "settings.far"));
    UT_ASSERT(ssIssue(&r, "settings.flat"));
    UT_ASSERT(ssIssue(&r, "settings.big"));
    UT_ASSERT(ssIssue(&r, "settings.flag"));
    UT_ASSERT(ssIssue(&r, "settings.nomax"));

    /* The directory listing carries the same two rows. */
    n = scnDirListDetails(ssDir, list, det, 4);
    for (i = 0; i < n; i++) {
        if (strcmp(det[i].file, "knobs.lua") == 0) found = i;
    }
    UT_ASSERT_MSG(found >= 0, "the script was not listed");
    UT_ASSERT(scnSettingsBlobRead(det[found].settings, det[found].settingsLen,
                                  rows, SCN_SETTINGS_MAX) == 2);
    UT_ASSERT(ssSame(&rows[0], &m->settings[0]));
    UT_ASSERT(ssSame(&rows[1], &m->settings[1]));

    ssRemoveTree(ssDir);
    return 0;
}

/* ── 3. manifest.json ─────────────────────────────────────────────── */

static const char kSsJson[] =
    "{\n"
    "  \"manifest\": 1, \"api\": 1, \"name\": \"Packed\", \"bound\": false,\n"
    "  \"settings\": [\n"
    "    { \"id\": \"laps\", \"label\": \"Laps\", \"type\": \"int\",\n"
    "      \"min\": 1, \"max\": 9, \"step\": 2, \"default\": 3 },\n"
    "    { \"id\": \"laps\", \"label\": \"Twice\", \"min\": 1, \"max\": 2,\n"
    "      \"default\": 1 },\n"
    "    { \"id\": \"odd\", \"label\": \"Odd\", \"min\": 1, \"max\": 9,\n"
    "      \"step\": 2, \"default\": 2 },\n"
    "    { \"id\": \"word\", \"label\": \"Word\", \"type\": \"choice\",\n"
    "      \"min\": 1, \"max\": 2, \"default\": 1 }\n"
    "  ],\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

int run_scenario_settings_manifest_json(void) {
    static ScnValidateResult sink;
    ScnParseReport           rep;
    ScnManifestDoc          *d;
    ScnManifestDoc          *back;
    ScenarioManifest         other;
    char                     soft[1024];
    char                     err[256];
    char                     key[64];
    char                     why[256];
    char                    *text;
    const ScenarioManifest  *m;

    memset(&sink, 0, sizeof(sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = &sink;
    d = scnManifestParse((const uint8_t *)kSsJson, strlen(kSsJson), &rep, err,
                         sizeof(err));
    UT_ASSERT_MSG(d != NULL, "the manifest was refused: %s", err);
    m = scnManifestValues(d);
    UT_ASSERT_MSG(m->numSettings == 1, "%d settings kept, wanted 1",
                  (int)m->numSettings);
    {
        ScnSetting want = ssRow("laps", "Laps", 1, 9, 2, 3);
        UT_ASSERT(ssSame(&m->settings[0], &want));
    }
    UT_ASSERT(ssIssue(&sink, "settings.laps"));
    UT_ASSERT(ssIssue(&sink, "settings.odd"));
    UT_ASSERT(ssIssue(&sink, "settings.word"));

    /* Written from the struct and read again, the setting survives. */
    back = scnManifestFromValues(m, err, sizeof(err));
    UT_ASSERT(back != NULL);
    text = scnManifestWrite(back, err, sizeof(err));
    scnManifestFree(back);
    UT_ASSERT_MSG(text != NULL, "the manifest did not write: %s", err);
    back = scnManifestParse((const uint8_t *)text, strlen(text), NULL, err,
                            sizeof(err));
    free(text);
    UT_ASSERT(back != NULL);
    UT_ASSERT(scnManifestValues(back)->numSettings == 1);
    UT_ASSERT(ssSame(&scnManifestValues(back)->settings[0], &m->settings[0]));
    UT_ASSERT(scnManifestAgrees(scnManifestValues(back), m, key, sizeof(key),
                                why, sizeof(why)));

    /* A script whose table says another default disagrees, by key. */
    other = *m;
    other.settings[0].def = 5;
    key[0] = '\0';
    UT_ASSERT(!scnManifestAgrees(m, &other, key, sizeof(key), why,
                                 sizeof(why)));
    UT_ASSERT_MSG(strncmp(key, "settings", 8) == 0,
                  "the disagreement was named as \"%s\"", key);

    scnManifestFree(back);
    scnManifestFree(d);
    return 0;
}

/* ── 4. The server's store ────────────────────────────────────────── */

/* One file, "knobs.lua", declaring armour 100..200 step 10 default 120. */
static int ssReader(void *ctx, const char *dir, const char *file,
                    uint8_t *out, size_t cap) {
    ScnSetting a = ssRow("armour", "Base armour", 100, 200, 10, 120);
    size_t     len = 0;

    (void)ctx;
    (void)dir;
    if (strcmp(file, "knobs.lua") != 0) return -1;
    if (!scnSettingsBlobAppend(out, cap, &len, &a)) return -1;
    return (int)len;
}

typedef struct {
    int     events;
    int     clears;
    uint8_t lastOp;
    char    lastId[SCN_SETTING_ID_LEN];
    int32_t lastValue;
} SsSeen;

static void ssWatch(void *ctx, const ControlEvent *evt) {
    SsSeen *s = (SsSeen *)ctx;

    if (evt->type != CTRL_LOBBY_SCRIPT_SETTING) return;
    s->events++;
    if (evt->u.lobbyScriptSetting.op == LOBBY_SCRIPT_SETTING_CLEAR) {
        s->clears++;
    }
    s->lastOp    = evt->u.lobbyScriptSetting.op;
    s->lastValue = evt->u.lobbyScriptSetting.value;
    SDL_strlcpy(s->lastId, evt->u.lobbyScriptSetting.id, sizeof(s->lastId));
}

static ServerSim *ssLobbySim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetScenarioSettingsReader(sim, ssReader, NULL);
    return sim;
}

static CmdResult ssApply(ServerSim *sim, int slot, const char *file,
                         const char *id, int32_t value) {
    ClientCommand cmd;
    CmdResult     r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_SET_SCRIPT_SETTING;
    cmd.cmdSeq = 1;
    SDL_strlcpy(cmd.u.setScriptSetting.file, file,
                sizeof(cmd.u.setScriptSetting.file));
    SDL_strlcpy(cmd.u.setScriptSetting.id, id,
                sizeof(cmd.u.setScriptSetting.id));
    cmd.u.setScriptSetting.value = value;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, slot, &cmd);
    threadsReleaseMutex();
    return r;
}

int run_scenario_settings_server_clamp(void) {
    ServerSim *sim;
    SsSeen     seen;
    SsSeen     sync;
    int32_t    v = 0;
    int32_t    got;

    sim = ssLobbySim();
    UT_ASSERT(sim != NULL);
    memset(&seen, 0, sizeof(seen));
    (void)serverSimRegisterSubscriber(sim, ssWatch, &seen);
    UT_ASSERT_MSG(seen.clears == 1 && seen.events == 1,
                  "the join sync sent %d events (%d clears), wanted one CLEAR",
                  seen.events, seen.clears);

    /* Nothing kept yet: the default. */
    UT_ASSERT(!serverSimGetScriptSetting(sim, "knobs.lua", "armour", &v));

    /* In range and on the step: kept and published as it was sent. */
    UT_ASSERT(serverSimSetScriptSetting(sim, "knobs.lua", "armour", 150, &got));
    UT_ASSERT(got == 150);
    UT_ASSERT(serverSimGetScriptSetting(sim, "knobs.lua", "armour", &v) &&
              v == 150);
    UT_ASSERT(seen.lastOp == LOBBY_SCRIPT_SETTING_SET && seen.lastValue == 150 &&
              strcmp(seen.lastId, "armour") == 0);

    /* Above the range: the top entry. Below: the bottom one. */
    UT_ASSERT(serverSimSetScriptSetting(sim, "knobs.lua", "armour", 9999, &got));
    UT_ASSERT_MSG(got == 200 && seen.lastValue == 200,
                  "9999 was kept as %d", (int)got);
    UT_ASSERT(serverSimSetScriptSetting(sim, "knobs.lua", "armour", -7, &got));
    UT_ASSERT_MSG(got == 100, "-7 was kept as %d", (int)got);

    /* Off the step: the default, which is kept as nothing. */
    UT_ASSERT(serverSimSetScriptSetting(sim, "knobs.lua", "armour", 155, &got));
    UT_ASSERT_MSG(got == 120 && seen.lastValue == 120,
                  "155 was kept as %d", (int)got);
    UT_ASSERT_MSG(!serverSimGetScriptSetting(sim, "knobs.lua", "armour", &v),
                  "the default was stored as a value");

    /* No such setting, no such file, no published event. */
    {
        int before = seen.events;
        UT_ASSERT(!serverSimSetScriptSetting(sim, "knobs.lua", "nope", 1, NULL));
        UT_ASSERT(!serverSimSetScriptSetting(sim, "other.lua", "armour", 150,
                                             NULL));
        UT_ASSERT(!serverSimSetScriptSetting(sim, "knobs.lua", "bad id", 150,
                                             NULL));
        UT_ASSERT(seen.events == before);
    }

    /* Through the command: the host may, a player who is not may not, and
       a refused id is refused. */
    UT_ASSERT(ssApply(sim, 1, "knobs.lua", "armour", 180) ==
              CMD_REJECT_NOT_HOST);
    UT_ASSERT(!serverSimGetScriptSetting(sim, "knobs.lua", "armour", &v));
    UT_ASSERT(ssApply(sim, 0, "knobs.lua", "nope", 180) == CMD_REJECT_INVALID);
    UT_ASSERT(ssApply(sim, 0, "../knobs.lua", "armour", 180) != CMD_OK);
    UT_ASSERT(ssApply(sim, 0, "knobs.lua", "armour", 180) == CMD_OK);
    UT_ASSERT(serverSimGetScriptSetting(sim, "knobs.lua", "armour", &v) &&
              v == 180);

    /* A second subscriber joins: a CLEAR, then the one kept value. */
    memset(&sync, 0, sizeof(sync));
    (void)serverSimRegisterSubscriber(sim, ssWatch, &sync);
    UT_ASSERT_MSG(sync.events == 2 && sync.clears == 1 &&
                      sync.lastOp == LOBBY_SCRIPT_SETTING_SET &&
                      sync.lastValue == 180,
                  "the join sync sent %d events, last %d=%d", sync.events,
                  (int)sync.lastOp, (int)sync.lastValue);

    serverSimDestroy(sim);
    return 0;
}

/* ── 5. The codecs ────────────────────────────────────────────────── */

int run_scenario_settings_codec(void) {
    ControlEncodeBodyFn enc;
    ControlDecodeBodyFn dec;
    ControlEvent        evt;
    ControlEvent        back;
    ClientCommand       cmd;
    ClientCommand       cmdBack;
    uint8_t             buf[CHANNEL_CONTROL_SEG];
    size_t              len = 0;

    enc = transportControlCodecBodyEncoder(CTRL_LOBBY_SCRIPT_SETTING);
    dec = transportControlCodecBodyDecoder(CTRL_LOBBY_SCRIPT_SETTING);
    UT_ASSERT(enc != NULL && dec != NULL);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_SETTING;
    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_SET;
    SDL_strlcpy(evt.u.lobbyScriptSetting.file, "Survival.scenario.lua",
                sizeof(evt.u.lobbyScriptSetting.file));
    SDL_strlcpy(evt.u.lobbyScriptSetting.id, "round_minutes",
                sizeof(evt.u.lobbyScriptSetting.id));
    evt.u.lobbyScriptSetting.value = -123456;
    UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(len == 1 + 1 + 21 + 1 + 13 + 4);
    UT_ASSERT(dec(buf, len, &back));
    UT_ASSERT(back.type == CTRL_LOBBY_SCRIPT_SETTING &&
              back.u.lobbyScriptSetting.op == LOBBY_SCRIPT_SETTING_SET &&
              strcmp(back.u.lobbyScriptSetting.file,
                     "Survival.scenario.lua") == 0 &&
              strcmp(back.u.lobbyScriptSetting.id, "round_minutes") == 0 &&
              back.u.lobbyScriptSetting.value == -123456);
    /* Cut short, or a byte over. */
    UT_ASSERT(!dec(buf, len - 1, &back));
    buf[len] = 0;
    UT_ASSERT(!dec(buf, len + 1, &back));
    /* An id length past its field. */
    buf[1 + 1 + 21] = SCN_SETTING_ID_LEN;
    UT_ASSERT(!dec(buf, len, &back));

    /* A CLEAR is the same shape with nothing in it. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_SETTING;
    UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(len == 7);
    UT_ASSERT(dec(buf, len, &back) &&
              back.u.lobbyScriptSetting.op == LOBBY_SCRIPT_SETTING_CLEAR);

    /* The command, up the wire. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_SET_SCRIPT_SETTING;
    SDL_strlcpy(cmd.u.setScriptSetting.file, "knobs.lua",
                sizeof(cmd.u.setScriptSetting.file));
    SDL_strlcpy(cmd.u.setScriptSetting.id, "armour",
                sizeof(cmd.u.setScriptSetting.id));
    cmd.u.setScriptSetting.value = 170;
    UT_ASSERT(commandCodecEncode(&cmd, buf, sizeof(buf), &len));
    memset(&cmdBack, 0, sizeof(cmdBack));
    UT_ASSERT(commandCodecDecode(buf, len, &cmdBack));
    UT_ASSERT(cmdBack.type == CMD_SET_SCRIPT_SETTING &&
              strcmp(cmdBack.u.setScriptSetting.file, "knobs.lua") == 0 &&
              strcmp(cmdBack.u.setScriptSetting.id, "armour") == 0 &&
              cmdBack.u.setScriptSetting.value == 170);
    UT_ASSERT(!commandCodecDecode(buf, len - 1, &cmdBack));
    buf[len] = 0;
    UT_ASSERT(!commandCodecDecode(buf, len + 1, &cmdBack));
    /* An empty id is no setting. */
    cmd.u.setScriptSetting.id[0] = '\0';
    UT_ASSERT(!commandCodecEncode(&cmd, buf, sizeof(buf), &len));
    return 0;
}

/* ── 6. The client ────────────────────────────────────────────────── */

int run_scenario_settings_client_apply(void) {
    ClientSim     *cs = clientSimAlloc();
    ControlEvent   evt;
    uint8_t        blob[SCN_SETTINGS_BLOB_MAX];
    const uint8_t *bytes = NULL;
    size_t         blen  = 0;
    size_t         got   = 0;
    int32_t        v     = 0;
    ScnSetting     a = ssRow("armour", "Base armour", 100, 200, 10, 120);

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);

    UT_ASSERT_MSG(!clientSimLobbyScriptSettingsSupported(cs),
                  "a fresh client thinks the server takes settings");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_SETTING;
    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_CLEAR;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimLobbyScriptSettingsSupported(cs));

    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_SET;
    SDL_strlcpy(evt.u.lobbyScriptSetting.file, "knobs.lua",
                sizeof(evt.u.lobbyScriptSetting.file));
    SDL_strlcpy(evt.u.lobbyScriptSetting.id, "armour",
                sizeof(evt.u.lobbyScriptSetting.id));
    evt.u.lobbyScriptSetting.value = 150;
    clientSimApplyControl(cs, &evt);
    evt.u.lobbyScriptSetting.value = 170;   /* the same key again: replaced */
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetLobbyScriptSetting(cs, "knobs.lua", "armour", &v) &&
              v == 170);
    UT_ASSERT(cs->lobbyScriptSettingCount == 1);
    UT_ASSERT(!clientSimGetLobbyScriptSetting(cs, "knobs.lua", "shells", &v));
    UT_ASSERT(!clientSimGetLobbyScriptSetting(cs, "other.lua", "armour", &v));

    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_CLEAR;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(!clientSimGetLobbyScriptSetting(cs, "knobs.lua", "armour", &v));

    /* The declaration rides beside the details. None known until the answer
       carries one, as from an older server. */
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &blen, &a));
    clientSimLobbyScenarioDetailsPut(cs, "knobs.lua", true, NULL, 0);
    UT_ASSERT(!clientSimGetLobbyScenarioSettings(cs, "knobs.lua", &bytes, &got));
    clientSimLobbyScenarioSettingsPut(cs, "knobs.lua", blob, blen);
    UT_ASSERT(clientSimGetLobbyScenarioSettings(cs, "knobs.lua", &bytes, &got));
    UT_ASSERT(got == blen && memcmp(bytes, blob, blen) == 0);
    /* A new answer for the file clears it until its own block comes. */
    clientSimLobbyScenarioDetailsPut(cs, "knobs.lua", true, NULL, 0);
    UT_ASSERT(!clientSimGetLobbyScenarioSettings(cs, "knobs.lua", &bytes, &got));
    /* Bytes that are not a block are kept as no settings. */
    clientSimLobbyScenarioSettingsPut(cs, "knobs.lua", blob, blen - 1);
    UT_ASSERT(clientSimGetLobbyScenarioSettings(cs, "knobs.lua", &bytes, &got));
    UT_ASSERT(got == 0);
    /* Forgotten with the details. */
    clientSimLobbyScenarioDetailsForget(cs);
    UT_ASSERT(!clientSimGetLobbyScenarioSettings(cs, "knobs.lua", &bytes, &got));

    clientSimDestroy(cs);
    return 0;
}

/* ── 7. game.setting ──────────────────────────────────────────────── */

static ServerSim *ssPlainSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimSetActive(sim);
    return sim;
}

/* One round of kSsScript, with the host's picks set first when chosen: the
 * three rules the script's on_setup writes. */
static int ssRound(bool chosen, int32_t *armour, int32_t *shells,
                   int32_t *mines) {
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    char          err[512];

    sim = ssPlainSim();
    UT_ASSERT(sim != NULL);
    serverSimSetScenarioDir(sim, ssDir);
    scenarioHostRegisterScenarioLister(sim);
    if (chosen) {
        UT_ASSERT(serverSimSetScriptSetting(sim, "knobs.lua", "armour", 170,
                                            NULL));
        UT_ASSERT(serverSimSetScriptSetting(sim, "knobs.lua", "shells", 45,
                                            NULL));
    }
    err[0] = '\0';
    h = scenarioHostAttachMod(sim, ssDir, "knobs.lua", err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the mod was refused: %s", err);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(scenarioHostLastError(h)[0] == '\0',
                  "the round complained: %s", scenarioHostLastError(h));
    gs = serverSimGetGameSim(sim);
    *armour = gs->rules.base_full_armour;
    *shells = gs->rules.tank_full_shells;
    *mines  = gs->rules.tank_full_mines;
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

int run_scenario_settings_game_setting(void) {
    int32_t armour = 0;
    int32_t shells = 0;
    int32_t mines  = 0;

    UT_ASSERT(ssMakeDir("game_setting"));
    UT_ASSERT(ssWriteText("knobs.lua", kSsCleanScript));

    /* Nothing chosen: the declared defaults, read at the top level (armour)
       and inside a hook (shells). */
    UT_ASSERT(ssRound(false, &armour, &shells, &mines) == 0);
    UT_ASSERT_MSG(armour == 120, "the top-level read gave %d, wanted the "
                  "default 120", (int)armour);
    UT_ASSERT_MSG(shells == 20, "the hook's read gave %d, wanted the default "
                  "20", (int)shells);
    UT_ASSERT_MSG(mines == 2, "game.setting(\"nope\") did not raise");

    /* The host's picks. */
    UT_ASSERT(ssRound(true, &armour, &shells, &mines) == 0);
    UT_ASSERT_MSG(armour == 170, "the top-level read gave %d, wanted the "
                  "host's 170", (int)armour);
    UT_ASSERT_MSG(shells == 45, "the hook's read gave %d, wanted the host's "
                  "45", (int)shells);

    ssRemoveTree(ssDir);
    return 0;
}

/* ── 8. Over the wire ─────────────────────────────────────────────── */

#define SS_CONNECT_MAX 2000
#define SS_FETCH_MAX   3000

static bool ssPredConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           clientSimLobbyScriptSettingsSupported(h->cs);
}

static bool ssPredSettled(LoopbackHarness *h, void *user) {
    ClientScnDetailsState st =
        clientSimGetLobbyScenarioDetails(h->cs, (const char *)user, NULL, NULL);

    return st == CLIENT_SCN_DETAILS_FOUND || st == CLIENT_SCN_DETAILS_NONE;
}

static bool ssPredValue(LoopbackHarness *h, void *user) {
    int32_t v = 0;

    return clientSimGetLobbyScriptSetting(h->cs, "knobs.lua", "armour", &v) &&
           v == *(const int32_t *)user;
}

int run_scenario_settings_wire(void) {
    LoopbackHarness h;
    ScenarioHost   *slot = NULL;
    ScnSetting      rows[SCN_SETTINGS_MAX];
    const uint8_t  *bytes = NULL;
    size_t          len   = 0;
    int32_t         want;
    int32_t         v = 0;
    int             at;

    UT_ASSERT(ssMakeDir("wire"));
    UT_ASSERT(ssWriteText("knobs.lua", kSsCleanScript));

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Asker", true, "loss=5,burst=2",
                                       0x5E771u),
                  "the harness did not come up");
    at = loopbackHarnessPumpUntil(&h, SS_CONNECT_MAX, ssPredConnected, NULL);
    UT_ASSERT_MSG(at >= 0, "the join sync never said the server takes "
                  "settings");
    threadsWaitForMutex();
    serverSimSetScenarioDir(h.sim, ssDir);
    scenarioHostRegisterScenarioLister(h.sim);
    scenarioHostFollowMap(h.sim, &slot);
    threadsReleaseMutex();

    /* The details answer carries the declaration. */
    clientSimLobbyScenarioDetailsWant(h.cs, "knobs.lua");
    at = loopbackHarnessPumpUntil(&h, SS_FETCH_MAX, ssPredSettled,
                                  (void *)"knobs.lua");
    UT_ASSERT_MSG(at >= 0, "no details answer within %d pumps", SS_FETCH_MAX);
    UT_ASSERT_MSG(clientSimGetLobbyScenarioSettings(h.cs, "knobs.lua", &bytes,
                                                    &len),
                  "the answer came without the settings block");
    UT_ASSERT(scnSettingsBlobRead(bytes, len, rows, SCN_SETTINGS_MAX) == 2);
    UT_ASSERT(strcmp(rows[0].id, "armour") == 0 && rows[0].def == 120);

    /* The host's pick, above the range, comes back as the top entry. */
    clientSimNetSendSetScriptSetting(h.cs, "knobs.lua", "armour", 999);
    want = 200;
    at = loopbackHarnessPumpUntil(&h, SS_FETCH_MAX, ssPredValue, &want);
    UT_ASSERT_MSG(at >= 0, "the clamped value never came back");
    threadsWaitForMutex();
    UT_ASSERT(serverSimGetScriptSetting(h.sim, "knobs.lua", "armour", &v));
    threadsReleaseMutex();
    UT_ASSERT(v == 200);

    /* And one in range, as sent. */
    clientSimNetSendSetScriptSetting(h.cs, "knobs.lua", "armour", 130);
    want = 130;
    at = loopbackHarnessPumpUntil(&h, SS_FETCH_MAX, ssPredValue, &want);
    UT_ASSERT_MSG(at >= 0, "the value 130 never came back");

    UT_ASSERT(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_CONNECTED);
    threadsWaitForMutex();
    if (slot != NULL) scenarioHostDetach(slot);
    threadsReleaseMutex();
    loopbackHarnessStop(&h);
    ssRemoveTree(ssDir);
    return 0;
}

/* ── 9. Survival's declaration ────────────────────────────────────── */

/* The shipped Survival map in a lobby, committed the way the host's map
 * chooser commits it: the scenario host follows map commits, the host
 * browses away to a plain map and back, and the commit publishes the map
 * script's row, details and settings. The template is seated first, as the
 * DS seats it. NULL, with the reason in err, when any step is refused. */
static ServerSim *ssSurvivalLobby(ScenarioHost **host, char *mapPath,
                                  size_t mapLen, char *err, size_t errLen) {
    ServerSim *sim;
    char       plain[512];

    *host = NULL;
    snprintf(mapPath, mapLen, "%s/Survival.map", WB_DATA_MAPS_DIR);
    snprintf(plain, sizeof(plain), "%s/Everard Island.map", WB_DATA_MAPS_DIR);
    sim = serverSimCreate(mapPath, gameOpen, false, 0, -1);
    if (sim == NULL) {
        snprintf(err, errLen, "the map will not load: %s", mapPath);
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, "brains/GoalHunter_1.7/init.lua");
    err[0] = '\0';
    *host = scenarioHostAttach(sim, mapPath, err, errLen);
    if (*host == NULL) {
        serverSimDestroy(sim);
        return NULL;
    }
    ut_brain_stub_arm(true);
    serverSimScenarioSeatLobby(sim);
    scenarioHostFollowMap(sim, host);
    if (!serverSimReloadMap(sim, plain) || !serverSimReloadMap(sim, mapPath) ||
        *host == NULL || serverSimGetMapScript(sim) == NULL) {
        snprintf(err, errLen, "the map did not commit with its script");
        if (*host != NULL) scenarioHostDetach(*host);
        *host = NULL;
        serverSimDestroy(sim);
        return NULL;
    }
    return sim;
}

int run_scenario_settings_survival_decl(void) {
    ServerSim    *sim;
    ScenarioHost *host;
    char          mapPath[512];
    char          err[512];
    char          file[SCN_DIR_FILE_LEN];
    uint8_t       blob[SCN_SETTINGS_BLOB_MAX];
    ScnSetting    rows[SCN_SETTINGS_MAX];
    int           len;
    int           n;

    sim = ssSurvivalLobby(&host, mapPath, sizeof(mapPath), err, sizeof(err));
    UT_ASSERT_MSG(sim != NULL, "%s", err);
    SDL_strlcpy(file, serverSimGetMapScript(sim)->file, sizeof(file));

    len = serverSimScenarioSettingsDecl(sim, file, blob, sizeof(blob));
    UT_ASSERT_MSG(len > 0, "%s declares no settings", file);
    n = scnSettingsBlobRead(blob, (size_t)len, rows, SCN_SETTINGS_MAX);
    UT_ASSERT_MSG(n == 2, "%s declares %d settings, wanted 2", file, n);
    {
        ScnSetting mins   = ssRow("round_minutes", "Round length (minutes)", 1,
                                  10, 1, 4);
        ScnSetting rounds = ssRow("rounds", "Rounds", 1, 5, 1, 5);
        UT_ASSERT(ssSame(&rows[0], &mins));
        UT_ASSERT(ssSame(&rows[1], &rounds));
    }

    scenarioHostDetach(host);
    serverSimDestroy(sim);
    return 0;
}

/* ── 10. A short Survival round ───────────────────────────────────── */

#define SS_GRACE_TICKS 1000    /* GRACE_S = 10 at 100 a second */
#define SS_WAVE_TICKS  6000    /* round_minutes = 1 */
#define SS_ROUND_MAX   (SS_GRACE_TICKS + SS_WAVE_TICKS + 4000)

int run_scenario_settings_survival_short(void) {
    ServerSim    *sim;
    ScenarioHost *host;
    char          mapPath[512];
    char          err[512];
    char          file[SCN_DIR_FILE_LEN];
    uint32_t      startTick;
    uint32_t      endTick = 0;

    sim = ssSurvivalLobby(&host, mapPath, sizeof(mapPath), err, sizeof(err));
    UT_ASSERT_MSG(sim != NULL, "%s", err);
    SDL_strlcpy(file, serverSimGetMapScript(sim)->file, sizeof(file));

    /* The host's two picks, as the details dialog sends them. */
    UT_ASSERT(serverSimSetScriptSetting(sim, file, "rounds", 1, NULL));
    UT_ASSERT(serverSimSetScriptSetting(sim, file, "round_minutes", 1, NULL));

    /* The one defender, above the template's seats. */
    {
        int slot = serverSimFindFreeSlot(sim, false);
        UT_ASSERT_MSG(slot >= 0, "no free slot for the defender");
        serverSimAddPlayer(sim, (BYTE)slot, "Def", false);
        sim->lobbyPlayers[slot].ready      = true;
        sim->lobbyPlayers[slot].teamNumber = 1;
    }

    sim->worldPreLoaded = TRUE;
    serverSimLobbyCheckAllReady(sim);
    while (sim->state == serverStateCountdown) serverSimTick(sim);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the round did not start: state %d", (int)sim->state);
    startTick = sim->tick;

    while (sim->state == serverStateRunning &&
           sim->tick < startTick + SS_ROUND_MAX) {
        serverSimTick(sim);
        if (sim->returnToLobbyReason == RETURN_REASON_SCENARIO &&
            endTick == 0) {
            endTick = sim->tick;
        }
    }
    UT_ASSERT_MSG(scenarioHostLastError(host)[0] == '\0',
                  "the round complained: %s", scenarioHostLastError(host));
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_SCENARIO,
                  "the round did not end by the script within %d ticks "
                  "(reason %d, state %d)", SS_ROUND_MAX,
                  (int)sim->returnToLobbyReason, (int)sim->state);
    UT_ASSERT_MSG(sim->returnToLobbyTeamId == 1,
                  "the round ended with team %d winning, not the defenders",
                  (int)sim->returnToLobbyTeamId);
    UT_ASSERT_MSG(strstr(sim->pendingWinMessage, "All 1 waves survived") !=
                      NULL,
                  "the win line was \"%s\"", sim->pendingWinMessage);
    /* One minute of wave after the grace, and not five waves of four. */
    UT_ASSERT_MSG(endTick >= startTick + SS_GRACE_TICKS + SS_WAVE_TICKS,
                  "the round ended %u ticks in, before a one-minute wave "
                  "could have run", (unsigned)(endTick - startTick));
    fprintf(stderr, "  survival short round: ended %u ticks in\n",
            (unsigned)(endTick - startTick));

    scenarioHostDetach(host);
    serverSimDestroy(sim);
    return 0;
}
