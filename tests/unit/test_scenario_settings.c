/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 *   scenario_settings_bool_blob       — bool rows (type = "bool") and int
 *       rows in one block, each read back with its type, and a bool row
 *       with any range but 0..1 step 1 refused.
 *   scenario_settings_bool_manifest_lua
 *                                     — a loose script's bool rows with a
 *       true and a false default kept beside an int row, and a bool row
 *       with a numeric default, no default, or a min, max or step reported
 *       and dropped.
 *   scenario_settings_bool_manifest_json
 *                                     — the same for manifest.json, and a
 *       bool row written from the struct and read back unchanged.
 *   scenario_settings_bool_server     — the host's command for a bool
 *       setting refused for anything but 0 or 1, where an int is clamped.
 *   scenario_settings_bool_game_setting
 *                                     — game.setting answers true or false
 *       for a bool setting, on its default and on the host's pick, and a
 *       number for an int setting in the same script.
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

static ScnSetting ssBool(const char *id, const char *label, bool def);
static ScnSetting ssChoice(const char *id, const char *label,
                           const char *const *words, int count, int def);

/* Three files: "knobs.lua", declaring armour 100..200 step 10 default 120;
 * "flags.lua", declaring the bool fog, default on, and the same armour; and
 * "picks.lua", declaring the choice teams, "Free For All" or "Use Lobby
 * Teams", default the first, and the same armour. */
static int ssReader(void *ctx, const char *dir, const char *file,
                    uint8_t *out, size_t cap) {
    ScnSetting a   = ssRow("armour", "Base armour", 100, 200, 10, 120);
    ScnSetting fog = ssBool("fog", "Fog", true);
    static const char *const words[] = { "Free For All", "Use Lobby Teams" };
    ScnSetting teams = ssChoice("teams", "Teams", words, 2, 0);
    size_t     len = 0;

    (void)ctx;
    (void)dir;
    if (strcmp(file, "flags.lua") == 0) {
        if (!scnSettingsBlobAppend(out, cap, &len, &fog)) return -1;
    } else if (strcmp(file, "picks.lua") == 0) {
        if (!scnSettingsBlobAppend(out, cap, &len, &teams)) return -1;
    } else if (strcmp(file, "knobs.lua") != 0) {
        return -1;
    }
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
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
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
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
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
    serverSimSetBotBrainPath(sim, "brains/GoalHunter/init.lua");
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

/* ── 11. On or off ────────────────────────────────────────────────── */

/* A bool row as both readers store it: 0..1 step 1, default 1 or 0. */
static ScnSetting ssBool(const char *id, const char *label, bool def) {
    ScnSetting s = ssRow(id, label, 0, 1, 1, def ? 1 : 0);

    s.type = SCN_SETTING_TYPE_BOOL;
    return s;
}

int run_scenario_settings_bool_blob(void) {
    uint8_t    blob[SCN_SETTINGS_BLOB_MAX];
    ScnSetting rows[SCN_SETTINGS_MAX];
    ScnSetting a    = ssRow("armour", "Base armour", 100, 200, 10, 120);
    ScnSetting fog  = ssBool("fog", "Fog", true);
    ScnSetting b    = ssRow("gap", "Gap", 0, 100, 25, 50);
    ScnSetting wall = ssBool("walls", "Walls", false);
    ScnSetting bad;
    size_t     len = 0;
    int        n;

    /* A mix of the two types, in order, each keeping its type. */
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &a));
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &fog));
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &b));
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &wall));
    n = scnSettingsBlobRead(blob, len, rows, SCN_SETTINGS_MAX);
    UT_ASSERT_MSG(n == 4, "%d rows read back, wanted 4", n);
    UT_ASSERT(ssSame(&rows[0], &a) && rows[0].type == SCN_SETTING_TYPE_INT);
    UT_ASSERT_MSG(ssSame(&rows[1], &fog) &&
                      rows[1].type == SCN_SETTING_TYPE_BOOL,
                  "fog came back as type %d %d..%d/%d def %d",
                  (int)rows[1].type, (int)rows[1].min, (int)rows[1].max,
                  (int)rows[1].step, (int)rows[1].def);
    UT_ASSERT(ssSame(&rows[2], &b) && rows[2].type == SCN_SETTING_TYPE_INT);
    UT_ASSERT(ssSame(&rows[3], &wall) &&
              rows[3].type == SCN_SETTING_TYPE_BOOL && rows[3].def == 0);

    /* A bool row is on or off and nothing wider. */
    UT_ASSERT(scnSettingProblem(&fog) == NULL);
    UT_ASSERT(scnSettingChoices(&fog) == 2);
    bad      = fog;
    bad.max  = 2;
    UT_ASSERT(scnSettingProblem(&bad) != NULL);
    UT_ASSERT(!scnSettingsBlobAppend(blob, sizeof(blob), &len, &bad));
    bad      = fog;
    bad.min  = -1;
    bad.def  = 0;
    UT_ASSERT(scnSettingProblem(&bad) != NULL);
    bad      = fog;
    bad.def  = 2;
    UT_ASSERT(scnSettingProblem(&bad) != NULL);

    /* A bool row whose range was changed on the wire does not read. */
    {
        uint8_t odd[SCN_SETTINGS_BLOB_MAX];
        size_t  oddLen = 0;
        size_t  at;

        UT_ASSERT(scnSettingsBlobAppend(odd, sizeof(odd), &oddLen, &fog));
        at = oddLen - 16 + 4;   /* max */
        odd[at + 3] = 2;
        UT_ASSERT(scnSettingsBlobRead(odd, oddLen, rows, SCN_SETTINGS_MAX) <
                  0);
    }
    return 0;
}

/* Two good bool rows either side of an int row, and a bool row breaking
 * each of its rules. game.setting at the top level answers a boolean under
 * the checker too, or the file raises there. */
static const char kSsBoolScript[] =
    "scenario = {\n"
    "  name = \"Flags\", api = 1, kind = \"mod\", bound = false,\n"
    "  settings = {\n"
    "    { id = \"fog\", label = \"Fog\", type = \"bool\", default = true },\n"
    "    { id = \"armour\", label = \"Base armour\", min = 100, max = 200,\n"
    "      step = 10, default = 120 },\n"
    "    { id = \"walls\", label = \"Walls\", type = \"bool\",\n"
    "      default = false },\n"
    "    { id = \"numeric\", label = \"Numeric\", type = \"bool\",\n"
    "      default = 1 },\n"
    "    { id = \"nodef\", label = \"No default\", type = \"bool\" },\n"
    "    { id = \"ranged\", label = \"Ranged\", type = \"bool\", min = 0,\n"
    "      max = 1, default = true },\n"
    "    { id = \"stepped\", label = \"Stepped\", type = \"bool\", step = 1,\n"
    "      default = false },\n"
    "  },\n"
    "}\n"
    "local F = game.setting(\"fog\")\n"
    "if type(F) ~= \"boolean\" then error(\"fog is a \" .. type(F)) end\n"
    "local A = game.setting(\"armour\")\n"
    "if type(A) ~= \"number\" then error(\"armour is a \" .. type(A)) end\n";

int run_scenario_settings_bool_manifest_lua(void) {
    static ScnValidateResult r;
    char                     path[512];
    const ScenarioManifest  *m = &r.manifest;
    ScnSetting               fog   = ssBool("fog", "Fog", true);
    ScnSetting               arm   = ssRow("armour", "Base armour", 100, 200,
                                           10, 120);
    ScnSetting               walls = ssBool("walls", "Walls", false);

    UT_ASSERT(ssMakeDir("bool_lua"));
    UT_ASSERT(ssWriteText("flags.lua", kSsBoolScript));
    snprintf(path, sizeof(path), "%s/flags.lua", ssDir);

    memset(&r, 0, sizeof(r));
    (void)scenarioValidateScript(NULL, path, &r);
    UT_ASSERT_MSG(r.haveManifest, "the script did not run under the "
                  "validator (game.setting answered the wrong type?)");
    UT_ASSERT_MSG(m->numSettings == 3, "%d settings kept, wanted 3",
                  (int)m->numSettings);
    UT_ASSERT_MSG(ssSame(&m->settings[0], &fog),
                  "fog read as type %d %d..%d/%d def %d",
                  (int)m->settings[0].type, (int)m->settings[0].min,
                  (int)m->settings[0].max, (int)m->settings[0].step,
                  (int)m->settings[0].def);
    UT_ASSERT(ssSame(&m->settings[1], &arm));
    UT_ASSERT(ssSame(&m->settings[2], &walls));
    UT_ASSERT_MSG(ssIssue(&r, "settings.numeric"),
                  "a numeric default on a bool row was not reported");
    UT_ASSERT(ssIssue(&r, "settings.nodef"));
    UT_ASSERT_MSG(ssIssue(&r, "settings.ranged"),
                  "a bool row with min and max was not reported");
    UT_ASSERT(ssIssue(&r, "settings.stepped"));
    UT_ASSERT(!ssIssue(&r, "settings.fog"));
    UT_ASSERT(!ssIssue(&r, "settings.walls"));

    ssRemoveTree(ssDir);
    return 0;
}

static const char kSsBoolJson[] =
    "{\n"
    "  \"manifest\": 1, \"api\": 1, \"name\": \"Packed\", \"bound\": false,\n"
    "  \"settings\": [\n"
    "    { \"id\": \"fog\", \"label\": \"Fog\", \"type\": \"bool\",\n"
    "      \"default\": true },\n"
    "    { \"id\": \"laps\", \"label\": \"Laps\", \"min\": 1, \"max\": 9,\n"
    "      \"default\": 3 },\n"
    "    { \"id\": \"walls\", \"label\": \"Walls\", \"type\": \"bool\",\n"
    "      \"default\": false },\n"
    "    { \"id\": \"numeric\", \"label\": \"Numeric\", \"type\": \"bool\",\n"
    "      \"default\": 1 },\n"
    "    { \"id\": \"ranged\", \"label\": \"Ranged\", \"type\": \"bool\",\n"
    "      \"min\": 0, \"max\": 1, \"default\": true }\n"
    "  ],\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

int run_scenario_settings_bool_manifest_json(void) {
    static ScnValidateResult sink;
    ScnParseReport           rep;
    ScnManifestDoc          *d;
    ScnManifestDoc          *back;
    char                     soft[1024];
    char                     err[256];
    char                     key[64];
    char                     why[256];
    char                    *text;
    const ScenarioManifest  *m;
    const ScenarioManifest  *mb;
    ScnSetting               fog   = ssBool("fog", "Fog", true);
    ScnSetting               laps  = ssRow("laps", "Laps", 1, 9, 1, 3);
    ScnSetting               walls = ssBool("walls", "Walls", false);

    memset(&sink, 0, sizeof(sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = &sink;
    d = scnManifestParse((const uint8_t *)kSsBoolJson, strlen(kSsBoolJson),
                         &rep, err, sizeof(err));
    UT_ASSERT_MSG(d != NULL, "the manifest was refused: %s", err);
    m = scnManifestValues(d);
    UT_ASSERT_MSG(m->numSettings == 3, "%d settings kept, wanted 3",
                  (int)m->numSettings);
    UT_ASSERT(ssSame(&m->settings[0], &fog));
    UT_ASSERT(ssSame(&m->settings[1], &laps));
    UT_ASSERT(ssSame(&m->settings[2], &walls));
    UT_ASSERT_MSG(ssIssue(&sink, "settings.numeric"),
                  "a numeric default on a bool row was not reported");
    UT_ASSERT(ssIssue(&sink, "settings.ranged"));

    /* Written from the struct and read again: a bool row goes out as
       "type": "bool" with a true or false default and no range, and comes
       back the same. */
    back = scnManifestFromValues(m, err, sizeof(err));
    UT_ASSERT(back != NULL);
    text = scnManifestWrite(back, err, sizeof(err));
    scnManifestFree(back);
    UT_ASSERT_MSG(text != NULL, "the manifest did not write: %s", err);
    UT_ASSERT_MSG(strstr(text, "\"bool\"") != NULL,
                  "the written manifest has no bool row:\n%s", text);
    back = scnManifestParse((const uint8_t *)text, strlen(text), NULL, err,
                            sizeof(err));
    free(text);
    UT_ASSERT_MSG(back != NULL, "the written manifest was refused: %s", err);
    mb = scnManifestValues(back);
    UT_ASSERT(mb->numSettings == 3);
    UT_ASSERT(ssSame(&mb->settings[0], &fog));
    UT_ASSERT(ssSame(&mb->settings[2], &walls));
    UT_ASSERT(scnManifestAgrees(mb, m, key, sizeof(key), why, sizeof(why)));

    scnManifestFree(back);
    scnManifestFree(d);
    return 0;
}

int run_scenario_settings_bool_server(void) {
    ServerSim *sim;
    int32_t    v   = 0;
    int32_t    got = -1;

    sim = ssLobbySim();
    UT_ASSERT(sim != NULL);

    /* Off, the other value from the default, is kept; on again, the
       default, is kept as nothing. */
    UT_ASSERT(serverSimSetScriptSetting(sim, "flags.lua", "fog", 0, &got));
    UT_ASSERT(got == 0);
    UT_ASSERT(serverSimGetScriptSetting(sim, "flags.lua", "fog", &v) &&
              v == 0);
    UT_ASSERT(serverSimSetScriptSetting(sim, "flags.lua", "fog", 1, &got));
    UT_ASSERT(got == 1);
    UT_ASSERT(!serverSimGetScriptSetting(sim, "flags.lua", "fog", &v));

    /* Through the host's command: anything but 0 or 1 is refused and
       changes nothing, where an int setting would be clamped. */
    UT_ASSERT(ssApply(sim, 0, "flags.lua", "fog", 0) == CMD_OK);
    UT_ASSERT(ssApply(sim, 0, "flags.lua", "fog", 2) == CMD_REJECT_INVALID);
    UT_ASSERT(ssApply(sim, 0, "flags.lua", "fog", -1) == CMD_REJECT_INVALID);
    UT_ASSERT(ssApply(sim, 0, "flags.lua", "fog", 1000) ==
              CMD_REJECT_INVALID);
    UT_ASSERT_MSG(serverSimGetScriptSetting(sim, "flags.lua", "fog", &v) &&
                      v == 0,
                  "a refused value changed what was kept");
    UT_ASSERT(ssApply(sim, 0, "flags.lua", "fog", 1) == CMD_OK);
    UT_ASSERT(!serverSimGetScriptSetting(sim, "flags.lua", "fog", &v));
    /* The int row in the same file still clamps. */
    UT_ASSERT(ssApply(sim, 0, "flags.lua", "armour", 9999) == CMD_OK);
    UT_ASSERT(serverSimGetScriptSetting(sim, "flags.lua", "armour", &v) &&
              v == 200);

    serverSimDestroy(sim);
    return 0;
}

/* A mod with two bool settings and an int one. on_setup writes what
 * game.setting answered into three rules: shells 11 for fog on, 10 for off;
 * mines 7 for walls on, 6 for off; either 1 when the answer was not a
 * boolean; and armour as the number it read, or 100 when it was not a
 * number. fog is read at the file's top level, the others in the hook. */
static const char kSsFlagsMod[] =
    "scenario = {\n"
    "  name = \"Flags\", api = 1, kind = \"mod\", bound = false,\n"
    "  settings = {\n"
    "    { id = \"fog\", label = \"Fog\", type = \"bool\", default = true },\n"
    "    { id = \"walls\", label = \"Walls\", type = \"bool\",\n"
    "      default = false },\n"
    "    { id = \"armour\", label = \"Base armour\", min = 100, max = 200,\n"
    "      step = 10, default = 120 },\n"
    "  },\n"
    "}\n"
    "local F = game.setting(\"fog\")\n"
    "function on_setup()\n"
    "  local W = game.setting(\"walls\")\n"
    "  local A = game.setting(\"armour\")\n"
    "  local s, m = 1, 1\n"
    "  if type(F) == \"boolean\" then s = F and 11 or 10 end\n"
    "  if type(W) == \"boolean\" then m = W and 7 or 6 end\n"
    "  if type(A) ~= \"number\" then A = 100 end\n"
    "  game.set_rule(\"tank_full_shells\", s)\n"
    "  game.set_rule(\"tank_full_mines\", m)\n"
    "  game.set_rule(\"base_full_armour\", A)\n"
    "end\n";

static int ssFlagsRound(bool chosen, int32_t *shells, int32_t *mines,
                        int32_t *armour) {
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    char          err[512];

    sim = ssPlainSim();
    UT_ASSERT(sim != NULL);
    serverSimSetScenarioDir(sim, ssDir);
    scenarioHostRegisterScenarioLister(sim);
    if (chosen) {
        UT_ASSERT(serverSimSetScriptSetting(sim, "flags.lua", "fog", 0, NULL));
        UT_ASSERT(serverSimSetScriptSetting(sim, "flags.lua", "walls", 1,
                                            NULL));
        UT_ASSERT(serverSimSetScriptSetting(sim, "flags.lua", "armour", 170,
                                            NULL));
    }
    err[0] = '\0';
    h = scenarioHostAttachMod(sim, ssDir, "flags.lua", err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the mod was refused: %s", err);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(scenarioHostLastError(h)[0] == '\0',
                  "the round complained: %s", scenarioHostLastError(h));
    gs      = serverSimGetGameSim(sim);
    *shells = gs->rules.tank_full_shells;
    *mines  = gs->rules.tank_full_mines;
    *armour = gs->rules.base_full_armour;
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

int run_scenario_settings_bool_game_setting(void) {
    int32_t shells = 0;
    int32_t mines  = 0;
    int32_t armour = 0;

    UT_ASSERT(ssMakeDir("bool_game_setting"));
    UT_ASSERT(ssWriteText("flags.lua", kSsFlagsMod));

    /* Nothing chosen: fog true and walls false, as booleans. */
    UT_ASSERT(ssFlagsRound(false, &shells, &mines, &armour) == 0);
    UT_ASSERT_MSG(shells == 11, "fog read as %d, wanted 11 (true)",
                  (int)shells);
    UT_ASSERT_MSG(mines == 6, "walls read as %d, wanted 6 (false)",
                  (int)mines);
    UT_ASSERT_MSG(armour == 120, "armour read as %d, wanted the number 120",
                  (int)armour);

    /* The host's picks: the other value of each. */
    UT_ASSERT(ssFlagsRound(true, &shells, &mines, &armour) == 0);
    UT_ASSERT_MSG(shells == 10, "fog read as %d, wanted 10 (false)",
                  (int)shells);
    UT_ASSERT_MSG(mines == 7, "walls read as %d, wanted 7 (true)",
                  (int)mines);
    UT_ASSERT_MSG(armour == 170, "armour read as %d, wanted the number 170",
                  (int)armour);

    ssRemoveTree(ssDir);
    return 0;
}

/* ── 12. One of a list of words ───────────────────────────────────── */

/* A choice row as both readers store it: its words, 0..count-1 step 1, and
 * the default word's index. */
static ScnSetting ssChoice(const char *id, const char *label,
                           const char *const *words, int count, int def) {
    ScnSetting s = ssRow(id, label, 0, count - 1, 1, def);
    int        i;

    s.type       = SCN_SETTING_TYPE_CHOICE;
    s.numChoices = (uint8_t)count;
    for (i = 0; i < count && i < SCN_SETTING_CHOICES_WORDS_MAX; i++) {
        SDL_strlcpy(s.choices[i], words[i], sizeof(s.choices[i]));
    }
    return s;
}

static bool ssSameWords(const ScnSetting *a, const ScnSetting *b) {
    int i;

    if (!ssSame(a, b) || a->numChoices != b->numChoices) return false;
    for (i = 0; i < (int)a->numChoices; i++) {
        if (strcmp(a->choices[i], b->choices[i]) != 0) return false;
    }
    return true;
}

static const char *const kSsTeamWords[] = { "Free For All",
                                            "Use Lobby Teams" };

int run_scenario_settings_choice_blob(void) {
    static const char *const three[] = { "Small", "Medium", "Large" };
    static const char *const eight[] = { "a", "b", "c", "d",
                                         "e", "f", "g", "h" };
    uint8_t    blob[SCN_SETTINGS_BLOB_MAX];
    ScnSetting rows[SCN_SETTINGS_MAX];
    ScnSetting a     = ssRow("armour", "Base armour", 100, 200, 10, 120);
    ScnSetting teams = ssChoice("teams", "Teams", kSsTeamWords, 2, 0);
    ScnSetting fog   = ssBool("fog", "Fog", true);
    ScnSetting size  = ssChoice("size", "Size", three, 3, 2);
    ScnSetting bad;
    size_t     len = 0;
    int        n;
    int        i;

    /* A mix of the three types, in order, each keeping its type and a
       choice keeping its words. */
    UT_ASSERT(scnSettingProblem(&teams) == NULL);
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &a));
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &teams));
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &fog));
    UT_ASSERT(scnSettingsBlobAppend(blob, sizeof(blob), &len, &size));
    n = scnSettingsBlobRead(blob, len, rows, SCN_SETTINGS_MAX);
    UT_ASSERT_MSG(n == 4, "%d rows read back, wanted 4", n);
    UT_ASSERT(ssSame(&rows[0], &a) && rows[0].numChoices == 0);
    UT_ASSERT_MSG(ssSameWords(&rows[1], &teams) &&
                      rows[1].type == SCN_SETTING_TYPE_CHOICE,
                  "teams came back as type %d %d..%d def %d, %d words",
                  (int)rows[1].type, (int)rows[1].min, (int)rows[1].max,
                  (int)rows[1].def, (int)rows[1].numChoices);
    UT_ASSERT(ssSame(&rows[2], &fog) && rows[2].type == SCN_SETTING_TYPE_BOOL);
    UT_ASSERT(ssSameWords(&rows[3], &size) && rows[3].def == 2);
    UT_ASSERT(strcmp(scnSettingChoiceText(&rows[3], 1), "Medium") == 0);
    UT_ASSERT(scnSettingChoiceText(&rows[3], 3) == NULL);
    UT_ASSERT(scnSettingChoiceText(&rows[3], -1) == NULL);
    UT_ASSERT(scnSettingChoiceText(&rows[0], 0) == NULL);
    UT_ASSERT(scnSettingChoiceIndex(&rows[1], "Use Lobby Teams") == 1);
    UT_ASSERT(scnSettingChoiceIndex(&rows[1], "use lobby teams") == -1);
    UT_ASSERT(scnSettingChoices(&teams) == 2);

    /* The rules a choice row is held to. */
    bad            = teams;
    bad.numChoices = 1;
    bad.max        = 0;
    UT_ASSERT_MSG(scnSettingProblem(&bad) != NULL, "one word was taken");
    bad = teams;
    SDL_strlcpy(bad.choices[1], "Free For All", sizeof(bad.choices[1]));
    UT_ASSERT_MSG(scnSettingProblem(&bad) != NULL, "a repeated word was taken");
    bad               = teams;
    bad.choices[0][0] = '\0';
    UT_ASSERT(scnSettingProblem(&bad) != NULL);
    bad     = teams;
    bad.max = 5;
    UT_ASSERT_MSG(scnSettingProblem(&bad) != NULL,
                  "a range wider than the words was taken");
    bad     = teams;
    bad.def = 2;
    UT_ASSERT(scnSettingProblem(&bad) != NULL);
    bad            = a;
    bad.numChoices = 2;
    UT_ASSERT_MSG(scnSettingProblem(&bad) != NULL,
                  "an int row with words was taken");
    UT_ASSERT(!scnSettingsBlobAppend(blob, sizeof(blob), &len, &bad));

    /* The byte budget: eight one-letter words fit; eight words of 31
       letters are far past the widest int body and are refused. */
    {
        ScnSetting wide = ssChoice("w", "W", eight, 8, 0);

        UT_ASSERT(scnSettingProblem(&wide) == NULL);
        for (i = 0; i < 8; i++) {
            memset(wide.choices[i], 'a' + i, SCN_SETTING_CHOICE_LEN - 1);
            wide.choices[i][SCN_SETTING_CHOICE_LEN - 1] = '\0';
        }
        UT_ASSERT_MSG(scnSettingProblem(&wide) != NULL,
                      "a choice row past the byte budget was taken");
    }

    /* Sixteen choice rows as wide as the budget allows still make a blob
       no bigger than SCN_SETTINGS_BLOB_MAX, the cap a build from before
       choices checks every blob against. */
    {
        uint8_t    full[SCN_SETTINGS_BLOB_MAX];
        size_t     fullLen = 0;
        ScnSetting w;
        char       id[SCN_SETTING_ID_LEN];
        char       label[SCN_SETTING_LABEL_LEN];

        for (i = 0; i < SCN_SETTINGS_MAX; i++) {
            snprintf(id, sizeof(id), "row%02d", i);
            memset(label, 'L', sizeof(label));
            label[sizeof(label) - 1] = '\0';
            /* Shorten the label until the row fits its byte budget. */
            for (;;) {
                w = ssChoice(id, label, eight, 8, 0);
                if (scnSettingProblem(&w) == NULL || label[0] == '\0') break;
                label[strlen(label) - 1] = '\0';
            }
            UT_ASSERT(scnSettingProblem(&w) == NULL);
            UT_ASSERT_MSG(scnSettingsBlobAppend(full, sizeof(full), &fullLen,
                                                &w),
                          "row %d did not fit the blob", i);
        }
        UT_ASSERT(fullLen <= SCN_SETTINGS_BLOB_MAX);
        UT_ASSERT(scnSettingsBlobRead(full, fullLen, rows, SCN_SETTINGS_MAX) ==
                  SCN_SETTINGS_MAX);
    }

    /* A choice row whose default index was changed on the wire to name no
       word does not read. */
    {
        uint8_t odd[SCN_SETTINGS_BLOB_MAX];
        size_t  oddLen = 0;
        size_t  at;

        UT_ASSERT(scnSettingsBlobAppend(odd, sizeof(odd), &oddLen, &teams));
        /* [count][type][len] then [idLen][id][labelLen][label][count][def] */
        at = 1 + 2 + 1 + strlen(teams.id) + 1 + strlen(teams.label) + 1;
        UT_ASSERT(odd[at - 1] == 2 && odd[at] == 0);
        odd[at] = 2;
        UT_ASSERT(scnSettingsBlobRead(odd, oddLen, rows, SCN_SETTINGS_MAX) <
                  0);
    }

    /* A reader that does not know a type skips its row by its length and
       keeps the rest. That is how a build from before choices reads a blob
       that holds one; here an unknown type 9 plays that part. */
    {
        uint8_t skip[SCN_SETTINGS_BLOB_MAX];
        size_t  skipLen = 0;
        size_t  pos     = 1;

        UT_ASSERT(scnSettingsBlobAppend(skip, sizeof(skip), &skipLen, &a));
        UT_ASSERT(scnSettingsBlobAppend(skip, sizeof(skip), &skipLen,
                                        &teams));
        UT_ASSERT(scnSettingsBlobAppend(skip, sizeof(skip), &skipLen, &fog));
        pos += 2 + skip[pos + 1];   /* past the armour row */
        UT_ASSERT(skip[pos] == SCN_SETTING_TYPE_CHOICE);
        skip[pos] = 9;
        n = scnSettingsBlobRead(skip, skipLen, rows, SCN_SETTINGS_MAX);
        UT_ASSERT_MSG(n == 2, "%d rows read around an unknown type, wanted 2",
                      n);
        UT_ASSERT(ssSame(&rows[0], &a) && ssSame(&rows[1], &fog));
    }
    return 0;
}

/* One good choice row between an int row and a bool row, and a choice row
 * breaking each of its rules. game.setting at the top level answers the
 * default word under the checker too, or the file raises there. */
static const char kSsChoiceScript[] =
    "scenario = {\n"
    "  name = \"Picks\", api = 1, kind = \"mod\", bound = false,\n"
    "  settings = {\n"
    "    { id = \"armour\", label = \"Base armour\", min = 100, max = 200,\n"
    "      step = 10, default = 120 },\n"
    "    { id = \"teams\", label = \"Teams\", type = \"choice\",\n"
    "      choices = { \"Free For All\", \"Use Lobby Teams\" },\n"
    "      default = \"Free For All\" },\n"
    "    { id = \"fog\", label = \"Fog\", type = \"bool\", default = true },\n"
    "    { id = \"notword\", label = \"Not a word\", type = \"choice\",\n"
    "      choices = { \"A\", \"B\" }, default = \"C\" },\n"
    "    { id = \"numeric\", label = \"Numeric\", type = \"choice\",\n"
    "      choices = { \"A\", \"B\" }, default = 1 },\n"
    "    { id = \"lonely\", label = \"Lonely\", type = \"choice\",\n"
    "      choices = { \"A\" }, default = \"A\" },\n"
    "    { id = \"nowords\", label = \"No words\", type = \"choice\",\n"
    "      default = \"A\" },\n"
    "    { id = \"ranged\", label = \"Ranged\", type = \"choice\", min = 0,\n"
    "      choices = { \"A\", \"B\" }, default = \"A\" },\n"
    "    { id = \"twice\", label = \"Twice\", type = \"choice\",\n"
    "      choices = { \"A\", \"A\" }, default = \"A\" },\n"
    "    { id = \"number\", label = \"Number word\", type = \"choice\",\n"
    "      choices = { \"A\", 2 }, default = \"A\" },\n"
    "  },\n"
    "}\n"
    "local T = game.setting(\"teams\")\n"
    "if T ~= \"Free For All\" then error(\"teams is \" .. tostring(T)) end\n";

int run_scenario_settings_choice_manifest_lua(void) {
    static ScnValidateResult r;
    char                     path[512];
    const ScenarioManifest  *m = &r.manifest;
    ScnSetting               arm   = ssRow("armour", "Base armour", 100, 200,
                                           10, 120);
    ScnSetting               teams = ssChoice("teams", "Teams", kSsTeamWords,
                                              2, 0);
    ScnSetting               fog   = ssBool("fog", "Fog", true);

    UT_ASSERT(ssMakeDir("choice_lua"));
    UT_ASSERT(ssWriteText("picks.lua", kSsChoiceScript));
    snprintf(path, sizeof(path), "%s/picks.lua", ssDir);

    memset(&r, 0, sizeof(r));
    (void)scenarioValidateScript(NULL, path, &r);
    UT_ASSERT_MSG(r.haveManifest, "the script did not run under the "
                  "validator (game.setting answered the wrong value?)");
    UT_ASSERT_MSG(m->numSettings == 3, "%d settings kept, wanted 3",
                  (int)m->numSettings);
    UT_ASSERT(ssSame(&m->settings[0], &arm));
    UT_ASSERT_MSG(ssSameWords(&m->settings[1], &teams),
                  "teams read as type %d %d..%d def %d, %d words",
                  (int)m->settings[1].type, (int)m->settings[1].min,
                  (int)m->settings[1].max, (int)m->settings[1].def,
                  (int)m->settings[1].numChoices);
    UT_ASSERT(ssSame(&m->settings[2], &fog));
    UT_ASSERT_MSG(ssIssue(&r, "settings.notword"),
                  "a default that is not a word was not reported");
    UT_ASSERT(ssIssue(&r, "settings.numeric"));
    UT_ASSERT(ssIssue(&r, "settings.lonely"));
    UT_ASSERT(ssIssue(&r, "settings.nowords"));
    UT_ASSERT(ssIssue(&r, "settings.ranged"));
    UT_ASSERT(ssIssue(&r, "settings.twice"));
    UT_ASSERT(ssIssue(&r, "settings.number"));
    UT_ASSERT(!ssIssue(&r, "settings.teams"));

    ssRemoveTree(ssDir);
    return 0;
}

static const char kSsChoiceJson[] =
    "{\n"
    "  \"manifest\": 1, \"api\": 1, \"name\": \"Packed\", \"bound\": false,\n"
    "  \"settings\": [\n"
    "    { \"id\": \"teams\", \"label\": \"Teams\", \"type\": \"choice\",\n"
    "      \"choices\": [\"Free For All\", \"Use Lobby Teams\"],\n"
    "      \"default\": \"Use Lobby Teams\" },\n"
    "    { \"id\": \"laps\", \"label\": \"Laps\", \"min\": 1, \"max\": 9,\n"
    "      \"default\": 3 },\n"
    "    { \"id\": \"notword\", \"label\": \"Not a word\",\n"
    "      \"type\": \"choice\", \"choices\": [\"A\", \"B\"],\n"
    "      \"default\": \"C\" },\n"
    "    { \"id\": \"stepped\", \"label\": \"Stepped\", \"type\": \"choice\",\n"
    "      \"step\": 1, \"choices\": [\"A\", \"B\"], \"default\": \"A\" }\n"
    "  ],\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

int run_scenario_settings_choice_manifest_json(void) {
    static ScnValidateResult sink;
    static ScenarioManifest  other;
    ScnParseReport           rep;
    ScnManifestDoc          *d;
    ScnManifestDoc          *back;
    char                     soft[1024];
    char                     err[256];
    char                     key[64];
    char                     why[256];
    char                    *text;
    const ScenarioManifest  *m;
    const ScenarioManifest  *mb;
    ScnSetting teams = ssChoice("teams", "Teams", kSsTeamWords, 2, 1);
    ScnSetting laps  = ssRow("laps", "Laps", 1, 9, 1, 3);

    memset(&sink, 0, sizeof(sink));
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = &sink;
    d = scnManifestParse((const uint8_t *)kSsChoiceJson,
                         strlen(kSsChoiceJson), &rep, err, sizeof(err));
    UT_ASSERT_MSG(d != NULL, "the manifest was refused: %s", err);
    m = scnManifestValues(d);
    UT_ASSERT_MSG(m->numSettings == 2, "%d settings kept, wanted 2",
                  (int)m->numSettings);
    UT_ASSERT(ssSameWords(&m->settings[0], &teams));
    UT_ASSERT(ssSame(&m->settings[1], &laps));
    UT_ASSERT(ssIssue(&sink, "settings.notword"));
    UT_ASSERT(ssIssue(&sink, "settings.stepped"));

    /* Written from the struct and read again: a choice row goes out as its
       words and the default word, and comes back the same. */
    back = scnManifestFromValues(m, err, sizeof(err));
    UT_ASSERT(back != NULL);
    text = scnManifestWrite(back, err, sizeof(err));
    scnManifestFree(back);
    UT_ASSERT_MSG(text != NULL, "the manifest did not write: %s", err);
    UT_ASSERT_MSG(strstr(text, "\"choice\"") != NULL &&
                      strstr(text, "\"Use Lobby Teams\"") != NULL,
                  "the written manifest has no choice row:\n%s", text);
    back = scnManifestParse((const uint8_t *)text, strlen(text), NULL, err,
                            sizeof(err));
    free(text);
    UT_ASSERT_MSG(back != NULL, "the written manifest was refused: %s", err);
    mb = scnManifestValues(back);
    UT_ASSERT(mb->numSettings == 2);
    UT_ASSERT(ssSameWords(&mb->settings[0], &teams));
    UT_ASSERT(scnManifestAgrees(mb, m, key, sizeof(key), why, sizeof(why)));

    /* A manifest and a script that name different words disagree. */
    other = *m;
    SDL_strlcpy(other.settings[0].choices[1], "Teams",
                sizeof(other.settings[0].choices[1]));
    UT_ASSERT_MSG(!scnManifestAgrees(&other, m, key, sizeof(key), why,
                                     sizeof(why)),
                  "different words were taken as agreeing");

    scnManifestFree(back);
    scnManifestFree(d);
    return 0;
}

int run_scenario_settings_choice_server(void) {
    ServerSim *sim;
    SsSeen     sync;
    int32_t    v   = 0;
    int32_t    got = -1;

    sim = ssLobbySim();
    UT_ASSERT(sim != NULL);

    /* The second word, not the default, is kept as its index; the first,
       the default, is kept as nothing. */
    UT_ASSERT(serverSimSetScriptSetting(sim, "picks.lua", "teams", 1, &got));
    UT_ASSERT(got == 1);
    UT_ASSERT(serverSimGetScriptSetting(sim, "picks.lua", "teams", &v) &&
              v == 1);
    UT_ASSERT(serverSimSetScriptSetting(sim, "picks.lua", "teams", 0, &got));
    UT_ASSERT(got == 0);
    UT_ASSERT(!serverSimGetScriptSetting(sim, "picks.lua", "teams", &v));

    /* Through the host's command: an index that names no word is refused
       and changes nothing, where an int setting would be clamped. */
    UT_ASSERT(ssApply(sim, 0, "picks.lua", "teams", 1) == CMD_OK);
    UT_ASSERT(ssApply(sim, 0, "picks.lua", "teams", 2) == CMD_REJECT_INVALID);
    UT_ASSERT(ssApply(sim, 0, "picks.lua", "teams", -1) ==
              CMD_REJECT_INVALID);
    UT_ASSERT_MSG(serverSimGetScriptSetting(sim, "picks.lua", "teams", &v) &&
                      v == 1,
                  "a refused index changed what was kept");

    /* A joiner is given the kept index back: a CLEAR, then the one value. */
    memset(&sync, 0, sizeof(sync));
    (void)serverSimRegisterSubscriber(sim, ssWatch, &sync);
    UT_ASSERT_MSG(sync.events == 2 && sync.clears == 1 &&
                      sync.lastOp == LOBBY_SCRIPT_SETTING_SET &&
                      strcmp(sync.lastId, "teams") == 0 &&
                      sync.lastValue == 1,
                  "the join sync sent %d events, last %s=%d", sync.events,
                  sync.lastId, (int)sync.lastValue);

    serverSimDestroy(sim);
    return 0;
}

/* A mod with a choice setting. on_setup writes what game.setting answered
 * into tank_full_shells: 11 for "Free For All", 12 for "Use Lobby Teams",
 * 1 for anything else. It is read at the file's top level and in the hook,
 * and the two must agree. */
static const char kSsPicksMod[] =
    "scenario = {\n"
    "  name = \"Picks\", api = 1, kind = \"mod\", bound = false,\n"
    "  settings = {\n"
    "    { id = \"teams\", label = \"Teams\", type = \"choice\",\n"
    "      choices = { \"Free For All\", \"Use Lobby Teams\" },\n"
    "      default = \"Free For All\" },\n"
    "  },\n"
    "}\n"
    "local TOP = game.setting(\"teams\")\n"
    "function on_setup()\n"
    "  local T = game.setting(\"teams\")\n"
    "  local s = 1\n"
    "  if T == TOP and T == \"Free For All\" then s = 11 end\n"
    "  if T == TOP and T == \"Use Lobby Teams\" then s = 12 end\n"
    "  game.set_rule(\"tank_full_shells\", s)\n"
    "end\n";

static int ssPicksRound(bool chosen, int32_t *shells) {
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    char          err[512];

    sim = ssPlainSim();
    UT_ASSERT(sim != NULL);
    serverSimSetScenarioDir(sim, ssDir);
    scenarioHostRegisterScenarioLister(sim);
    if (chosen) {
        UT_ASSERT(serverSimSetScriptSetting(sim, "picks.lua", "teams", 1,
                                            NULL));
    }
    err[0] = '\0';
    h = scenarioHostAttachMod(sim, ssDir, "picks.lua", err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the mod was refused: %s", err);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(scenarioHostLastError(h)[0] == '\0',
                  "the round complained: %s", scenarioHostLastError(h));
    gs      = serverSimGetGameSim(sim);
    *shells = gs->rules.tank_full_shells;
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

int run_scenario_settings_choice_game_setting(void) {
    int32_t shells = 0;

    UT_ASSERT(ssMakeDir("choice_game_setting"));
    UT_ASSERT(ssWriteText("picks.lua", kSsPicksMod));

    /* Nothing chosen: the default word. */
    UT_ASSERT(ssPicksRound(false, &shells) == 0);
    UT_ASSERT_MSG(shells == 11, "teams read as %d, wanted 11 "
                  "(\"Free For All\")", (int)shells);
    /* The host's pick: the second word, as a word. */
    UT_ASSERT(ssPicksRound(true, &shells) == 0);
    UT_ASSERT_MSG(shells == 12, "teams read as %d, wanted 12 "
                  "(\"Use Lobby Teams\")", (int)shells);

    ssRemoveTree(ssDir);
    return 0;
}
