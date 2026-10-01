/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The scripts.json member a scripted round's recording carries.
 *
 *   scripts_record_scripted_round — the committed map's own scenario and one
 *       mod behind it, booted and recorded. The .wbv holds scripts.json with
 *       version 1, the map's file name, the rule the scenario set, its
 *       region under the scenario's file, and two script rows in load order
 *       with the right source and kind.
 *   scripts_record_plain_round — a round with no host has no member, and
 *       attribution.trk is still there.
 *   scripts_record_scripted_then_plain — the same sim plays a scripted round
 *       and then, with the script gone and the list empty, a plain one. The
 *       second recording has no member: logStop cleared the text once the
 *       first recording held it, and the detach did not put it back.
 *   scripts_record_detach_before_stop — the host detaches while the round's
 *       recording is still open, which is the order a desktop host leaving
 *       mid-round takes, and the recording still carries the member.
 *   scripts_record_setter_cap — the sim refuses text over
 *       SCN_RECORD_TEXT_MAX, keeps text at the cap, and hands a short text
 *       back byte for byte.
 *   scripts_record_json_write — scnRecordJsonWrite on a hand-built
 *       description, read back key by key.
 *
 * The recordings are read with minizip directly rather than through the log
 * viewer. The scripted rounds drive the real path: a scratch scenarios
 * directory, the host's own lister, a map commit and the lobby's list
 * command, then the in-place round start, which boots the round without
 * reloading the map — the map named here is never written, only the script
 * beside it.
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cJSON.h"
#include "unzip.h"

#include "global.h"
#include "client_command.h"        /* CMD_SET_SCRIPT_LIST */
#include "server_sim.h"
#include "server_sim_internal.h"   /* mapFilePath, scenarioPickTick */
#include "server_sim_lifecycle.h"  /* serverSimStartGameInPlace,
                                    * serverSimScenarioOnMapChanged */
#include "attribution_track.h"     /* ATTRIBUTION_TRACK_MEMBER */
#include "scripts_record.h"        /* SCRIPTS_RECORD_MEMBER, the cap */
#include "scenario_host.h"
#include "scenario_validate.h"     /* scenarioHostScriptCount */
#include "scenario_record_json.h"
#include "everard_map.h"
#include "threads.h"
#include "replay_harness.h"
#include "test_harness.h"

/* ── The scenarios directory and the map's own script ─────────────── */

#define SR_MAP_LEAF    "wbtest_scn_rec.map"
#define SR_MAP_SCRIPT  "wbtest_scn_rec.scenario.lua"
#define SR_MOD_FILE    "recmod.lua"

static char srDir[256];

/* The scratch directory the scenarios live in, one per case: the cases are
   separate ctest tests and run at the same time. */
static bool srMakeDir(const char *tag) {
    char leaf[128];

    snprintf(leaf, sizeof(leaf), "wbtest_scn_rec_%s", tag);
    if (!utScratchPath(srDir, sizeof(srDir), leaf)) return false;
    (void)SDL_RemovePath(srDir);
    return SDL_CreateDirectory(srDir);
}

static bool srPutFile(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static bool srWriteMod(const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", srDir, SR_MOD_FILE);
    return srPutFile(path, text);
}

/* The map sits in the scratch directory too, so its script is a file of
   this case's own. */
static void srMapPath(char *out, size_t outLen) {
    snprintf(out, outLen, "%s/%s", srDir, SR_MAP_LEAF);
}

static bool srPutMapScript(const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", srDir, SR_MAP_SCRIPT);
    return srPutFile(path, text);
}

static void srDropMapScript(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", srDir, SR_MAP_SCRIPT);
    remove(path);
}

static void srDropDir(void) {
    char path[512];

    srDropMapScript();
    snprintf(path, sizeof(path), "%s/%s", srDir, SR_MOD_FILE);
    remove(path);
    SDL_RemovePath(srDir);
}

/* The map's own scenario: one rule and one region. */
static const char kSrScenario[] =
    "scenario = {\n"
    "  name = \"Record Scenario\",\n"
    "  api = 1,\n"
    "  rules = { tank_death_ticks = 400 },\n"
    "  regions = { keep = { x = 10, y = 12, w = 6, h = 4 } },\n"
    "}\n";

/* A mod behind it, from the scenarios directory. */
static const char kSrMod[] =
    "scenario = {\n"
    "  name = \"Record Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n";

/* ── The sim ──────────────────────────────────────────────────────── */

static ScenarioHost *srSlot;

static ServerSim *srSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetState(sim, serverStateLobby);
    srSlot = NULL;
    serverSimSetScenarioDir(sim, srDir);
    scenarioHostRegisterScenarioLister(sim);
    scenarioHostFollowMap(sim, &srSlot);
    return sim;
}

static void srDestroy(ServerSim *sim) {
    /* The host holds registrations on the sim, so it goes first. */
    scenarioHostDetach(srSlot);
    srSlot = NULL;
    serverSimDestroy(sim);
}

/* A map commit, the way the sim makes it after loading the new map. */
static void srCommit(ServerSim *sim) {
    char mapPath[512];

    srMapPath(mapPath, sizeof(mapPath));
    SDL_strlcpy(sim->mapFilePath, mapPath, sizeof(sim->mapFilePath));
    serverSimScenarioOnMapChanged(sim, mapPath);
    serverSimScenarioApplyLobbyRules(sim);
}

/* The host's whole list, through the lobby's command. */
static CmdResult srPick(ServerSim *sim, const char *const *files, int count) {
    ClientCommand cmd;
    CmdResult     r;
    int           i;

    sim->scenarioPickTick = 0;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_SET_SCRIPT_LIST;
    cmd.cmdSeq = 1;
    cmd.u.setScriptList.count = (uint8_t)count;
    for (i = 0; i < count; i++) {
        snprintf(cmd.u.setScriptList.files[i],
                 sizeof(cmd.u.setScriptList.files[i]), "%s", files[i]);
    }
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Out of the lobby and into a round, booting whatever host is attached. */
static void srStartRound(ServerSim *sim) {
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGameInPlace(sim);
}

/* Record a few ticks of the sim's round to a scratch .wbv through the
   replay harness. The harness is given the sim rather than making one, so
   h->sim is put back to NULL before the Stop that removes the file: the
   caller's host is still registered on that sim and srDestroy takes both
   down in order. */
static bool srRecord(ServerSim *sim, const char *tag, ReplayHarness *h) {
    char leaf[128];

    memset(h, 0, sizeof(*h));
    snprintf(leaf, sizeof(leaf), "wbtest_scn_rec_%s.wbv", tag);
    if (!utScratchPath(h->path, sizeof(h->path), leaf)) return false;
    remove(h->path);
    h->sim = sim;
    if (!replayHarnessBeginRecording(h)) return false;
    replayHarnessTick(h, 6);
    return replayHarnessStopRecording(h);
}

static void srRecordDone(ReplayHarness *h) {
    h->sim = NULL;
    replayHarnessStop(h);
}

/* One member of the archive, NUL-terminated, in a buffer the caller frees.
   False with *out NULL when the archive has no member of that name. */
static bool srReadMember(const char *path, const char *name, char **out,
                         size_t *outLen) {
    unzFile        uf;
    unz_file_info  info;
    char          *buf;
    size_t         got = 0;

    *out = NULL;
    if (outLen != NULL) *outLen = 0;
    uf = unzOpen(path);
    if (uf == NULL) return false;
    if (unzLocateFile(uf, name, 0) != UNZ_OK ||
        unzGetCurrentFileInfo(uf, &info, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK ||
        unzOpenCurrentFile(uf) != UNZ_OK) {
        unzClose(uf);
        return false;
    }
    buf = (char *)malloc((size_t)info.uncompressed_size + 1);
    if (buf == NULL) {
        unzCloseCurrentFile(uf);
        unzClose(uf);
        return false;
    }
    while (got < (size_t)info.uncompressed_size) {
        int n = unzReadCurrentFile(uf, buf + got,
                                   (unsigned)((size_t)info.uncompressed_size - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    buf[got] = '\0';
    unzCloseCurrentFile(uf);
    unzClose(uf);
    *out = buf;
    if (outLen != NULL) *outLen = got;
    return true;
}

static bool srHasMember(const char *path, const char *name) {
    char *buf = NULL;
    bool  found = srReadMember(path, name, &buf, NULL);
    free(buf);
    return found;
}

static const char *srStr(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool srStrIs(const cJSON *obj, const char *key, const char *want) {
    const char *s = srStr(obj, key);
    return s != NULL && strcmp(s, want) == 0;
}

/* ── 1. A scripted round's recording ───────────────────────────────── */

/* The scripted round of cases 1 and 3: the map's scenario first, the mod
   behind it. NULL when a step was refused. */
static ServerSim *srScriptedSim(void) {
    const char *picks[1] = { SR_MOD_FILE };
    ServerSim  *sim;

    if (!srWriteMod(kSrMod) || !srPutMapScript(kSrScenario)) return NULL;
    sim = srSim();
    if (sim == NULL) return NULL;
    srCommit(sim);
    if (srPick(sim, picks, 1) != CMD_OK || srSlot == NULL ||
        scenarioHostScriptCount(srSlot) != 2) {
        srDestroy(sim);
        return NULL;
    }
    srStartRound(sim);
    return sim;
}

int run_scripts_record_scripted_round(void) {
    ServerSim     *sim;
    ReplayHarness  h;
    char          *text = NULL;
    size_t         len  = 0;
    cJSON         *root;
    const cJSON   *v;
    const cJSON   *rows;
    const cJSON   *row;
    const cJSON   *reg;

    UT_ASSERT(srMakeDir("scripted"));
    sim = srScriptedSim();
    UT_ASSERT_MSG(sim != NULL, "the scenario and the mod did not attach");
    UT_ASSERT_MSG(serverSimGetScenarioRecordText(sim, &len) != NULL && len > 0,
                  "the round boot left the sim no text");

    UT_ASSERT_MSG(srRecord(sim, "scripted", &h), "could not record the round");
    UT_ASSERT_MSG(srReadMember(h.path, SCRIPTS_RECORD_MEMBER, &text, &len),
                  "the recording has no %s", SCRIPTS_RECORD_MEMBER);
    UT_ASSERT_MSG(srHasMember(h.path, ATTRIBUTION_TRACK_MEMBER),
                  "the recording lost %s", ATTRIBUTION_TRACK_MEMBER);
    srRecordDone(&h);
    srDestroy(sim);
    srDropDir();

    root = cJSON_ParseWithLength(text, len);
    free(text);
    UT_ASSERT_MSG(root != NULL && cJSON_IsObject(root),
                  "%s is not a JSON object", SCRIPTS_RECORD_MEMBER);

    v = cJSON_GetObjectItemCaseSensitive(root, "version");
    UT_ASSERT_MSG(cJSON_IsNumber(v) && v->valueint == 1, "version is not 1");
    UT_ASSERT_MSG(srStrIs(root, "map", SR_MAP_LEAF), "map is \"%s\", wanted %s",
                  srStr(root, "map") ? srStr(root, "map") : "(none)",
                  SR_MAP_LEAF);
    UT_ASSERT_MSG(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,
                                                                "mods_enabled")),
                  "mods_enabled is not true");

    v = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "rules"), "tank_death_ticks");
    UT_ASSERT_MSG(cJSON_IsNumber(v) && v->valuedouble == 400.0,
                  "rules.tank_death_ticks is not 400");

    rows = cJSON_GetObjectItemCaseSensitive(root, "regions");
    UT_ASSERT_MSG(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 1,
                  "regions is not one row");
    reg = cJSON_GetArrayItem(rows, 0);
    UT_ASSERT_MSG(srStrIs(reg, "name", "keep") &&
                      srStrIs(reg, "file", SR_MAP_SCRIPT),
                  "the region is not keep from %s", SR_MAP_SCRIPT);
    UT_ASSERT_MSG(cJSON_GetObjectItemCaseSensitive(reg, "x")->valueint == 10 &&
                      cJSON_GetObjectItemCaseSensitive(reg, "y")->valueint == 12 &&
                      cJSON_GetObjectItemCaseSensitive(reg, "w")->valueint == 6 &&
                      cJSON_GetObjectItemCaseSensitive(reg, "h")->valueint == 4,
                  "the region's rectangle is not 10,12 6x4");

    rows = cJSON_GetObjectItemCaseSensitive(root, "scripts");
    UT_ASSERT_MSG(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 2,
                  "scripts is not two rows");
    row = cJSON_GetArrayItem(rows, 0);
    UT_ASSERT_MSG(srStrIs(row, "file", SR_MAP_SCRIPT) &&
                      srStrIs(row, "source", "map") &&
                      srStrIs(row, "kind", "scenario"),
                  "row 0 is not the map's own scenario");
    UT_ASSERT_MSG(srStrIs(cJSON_GetObjectItemCaseSensitive(row, "manifest"),
                          "name", "Record Scenario"),
                  "row 0's manifest is not the scenario's");
    row = cJSON_GetArrayItem(rows, 1);
    UT_ASSERT_MSG(srStrIs(row, "file", SR_MOD_FILE) &&
                      srStrIs(row, "source", "server") &&
                      srStrIs(row, "kind", "mod"),
                  "row 1 is not the server's mod");
    UT_ASSERT_MSG(srStrIs(cJSON_GetObjectItemCaseSensitive(row, "manifest"),
                          "name", "Record Mod"),
                  "row 1's manifest is not the mod's");

    cJSON_Delete(root);
    return 0;
}

/* ── 2. A plain round's recording ──────────────────────────────────── */

int run_scripts_record_plain_round(void) {
    ReplayHarness h;
    bool          scripts;
    bool          track;

    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scriptsRecordPlain",
                                              "Tester"),
                  "could not start recording");
    replayHarnessTick(&h, 6);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");
    scripts = srHasMember(h.path, SCRIPTS_RECORD_MEMBER);
    track   = srHasMember(h.path, ATTRIBUTION_TRACK_MEMBER);
    replayHarnessStop(&h);

    UT_ASSERT_MSG(!scripts, "a plain round wrote %s", SCRIPTS_RECORD_MEMBER);
    UT_ASSERT_MSG(track, "a plain round lost %s", ATTRIBUTION_TRACK_MEMBER);
    return 0;
}

/* ── 3. A scripted round, then a plain one on the same sim ─────────── */

int run_scripts_record_scripted_then_plain(void) {
    ServerSim     *sim;
    ReplayHarness  h;
    bool           first;
    bool           second;
    size_t         len = 1;

    UT_ASSERT(srMakeDir("then_plain"));
    sim = srScriptedSim();
    UT_ASSERT_MSG(sim != NULL, "the scenario and the mod did not attach");
    UT_ASSERT_MSG(srRecord(sim, "then_plain_1", &h),
                  "could not record the scripted round");
    first = srHasMember(h.path, SCRIPTS_RECORD_MEMBER);
    srRecordDone(&h);
    UT_ASSERT_MSG(first, "the scripted round wrote no %s",
                  SCRIPTS_RECORD_MEMBER);
    UT_ASSERT_MSG(serverSimGetScenarioRecordText(sim, &len) == NULL && len == 0,
                  "logStop left the recorded round's text on the sim");

    /* Back to the lobby, the map's script gone and the list emptied: the
       decision finds nothing to play and detaches the host. */
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetState(sim, serverStateLobby);
    srDropMapScript();
    UT_ASSERT_MSG(srPick(sim, NULL, 0) == CMD_OK, "the empty list was refused");
    UT_ASSERT_MSG(srSlot == NULL, "a host is still attached");
    len = 1;
    UT_ASSERT_MSG(serverSimGetScenarioRecordText(sim, &len) == NULL && len == 0,
                  "the sim has text for a round with no host");

    srStartRound(sim);
    UT_ASSERT_MSG(srRecord(sim, "then_plain_2", &h),
                  "could not record the plain round");
    second = srHasMember(h.path, SCRIPTS_RECORD_MEMBER);
    srRecordDone(&h);
    srDestroy(sim);
    srDropDir();

    UT_ASSERT_MSG(!second, "the plain round after a scripted one wrote %s",
                  SCRIPTS_RECORD_MEMBER);
    return 0;
}

/* ── 3b. The host detaches before the recording closes ─────────────── */

/* A desktop host leaving mid-round detaches its scenario host under the sim
   lock and only then closes the round's log, so the text has to outlive the
   detach for logStop to find it. */
int run_scripts_record_detach_before_stop(void) {
    ServerSim     *sim;
    ReplayHarness  h;
    bool           scripts;
    size_t         len = 1;

    UT_ASSERT(srMakeDir("detach_stop"));
    sim = srScriptedSim();
    UT_ASSERT_MSG(sim != NULL, "the scenario and the mod did not attach");

    memset(&h, 0, sizeof(h));
    UT_ASSERT(utScratchPath(h.path, sizeof(h.path),
                            "wbtest_scn_rec_detach_stop.wbv"));
    remove(h.path);
    h.sim = sim;
    UT_ASSERT_MSG(replayHarnessBeginRecording(&h), "could not start recording");
    replayHarnessTick(&h, 6);

    scenarioHostDetach(srSlot);
    srSlot = NULL;
    UT_ASSERT_MSG(serverSimGetScenarioRecordText(sim, &len) != NULL && len > 0,
                  "the detach cleared the text of a round still recording");

    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");
    scripts = srHasMember(h.path, SCRIPTS_RECORD_MEMBER);
    srRecordDone(&h);
    srDestroy(sim);
    srDropDir();

    UT_ASSERT_MSG(scripts, "a round whose host detached before the log closed "
                  "wrote no %s", SCRIPTS_RECORD_MEMBER);
    return 0;
}

/* ── 4. The setter's cap ───────────────────────────────────────────── */

int run_scripts_record_setter_cap(void) {
    static const char kShort[] = "{\"version\":1}";
    ServerSim  *sim;
    char       *big;
    const char *got;
    size_t      len = 1;

    sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");
    big = (char *)malloc((size_t)SCN_RECORD_TEXT_MAX + 1);
    UT_ASSERT_MSG(big != NULL, "no memory for the over-cap text");
    memset(big, 'x', (size_t)SCN_RECORD_TEXT_MAX + 1);

    /* One byte over: nothing stored, and what was there is gone too. */
    serverSimSetScenarioRecordText(sim, kShort, sizeof(kShort) - 1);
    serverSimSetScenarioRecordText(sim, big, (size_t)SCN_RECORD_TEXT_MAX + 1);
    got = serverSimGetScenarioRecordText(sim, &len);
    UT_ASSERT_MSG(got == NULL && len == 0,
                  "text over the cap was kept (%zu bytes)", len);

    /* At the cap exactly: kept. */
    serverSimSetScenarioRecordText(sim, big, (size_t)SCN_RECORD_TEXT_MAX);
    got = serverSimGetScenarioRecordText(sim, &len);
    UT_ASSERT_MSG(got != NULL && len == (size_t)SCN_RECORD_TEXT_MAX,
                  "text at the cap was refused");
    free(big);

    /* A short text back byte for byte, and a copy rather than the caller's
       pointer. */
    serverSimSetScenarioRecordText(sim, kShort, sizeof(kShort) - 1);
    got = serverSimGetScenarioRecordText(sim, &len);
    UT_ASSERT_MSG(got != NULL && len == sizeof(kShort) - 1 &&
                      memcmp(got, kShort, len) == 0 && got != kShort,
                  "the short text did not come back as it went in");

    /* NULL clears. */
    serverSimSetScenarioRecordText(sim, NULL, 0);
    UT_ASSERT_MSG(serverSimGetScenarioRecordText(sim, &len) == NULL && len == 0,
                  "NULL did not clear the text");

    serverSimDestroy(sim);
    return 0;
}

/* ── 5. The writer on a hand-built description ─────────────────────── */

int run_scripts_record_json_write(void) {
    static ScnRecordDesc d;
    char        err[256];
    char       *text;
    cJSON      *root;
    const cJSON *v;
    const cJSON *rows;
    const cJSON *row;

    memset(&d, 0, sizeof(d));
    d.map         = "Hand.map";
    d.modsEnabled = false;
    d.numRules    = 1;
    d.rules[0].name  = "tank_death_ticks";
    d.rules[0].value = 400.0;
    d.numRegions  = 1;
    d.regions[0].name = "keep";
    d.regions[0].x = 1;
    d.regions[0].y = 2;
    d.regions[0].w = 3;
    d.regions[0].h = 4;
    d.regions[0].file = "base.lua";
    d.numScripts  = 2;
    d.scripts[0].file   = "base.lua";
    d.scripts[0].source = "map";
    d.scripts[0].kind   = "scenario";
    d.scripts[0].manifestJson = "{\"name\":\"Base\",\"api\":1}";
    d.scripts[1].file   = "extra.lua";
    d.scripts[1].source = "server";
    d.scripts[1].kind   = "mod";
    d.scripts[1].manifestJson = "{\"name\":\"Extra\",\"kind\":\"mod\"}";

    err[0] = '\0';
    text = scnRecordJsonWrite(&d, err, sizeof(err));
    UT_ASSERT_MSG(text != NULL, "the writer refused: %s", err);
    /* A whole number goes out as an integer. */
    UT_ASSERT_MSG(strstr(text, "\"tank_death_ticks\":400,") != NULL ||
                      strstr(text, "\"tank_death_ticks\":400}") != NULL,
                  "the rule is not written as 400: %s", text);
    /* Unformatted: no line breaks. */
    UT_ASSERT_MSG(strchr(text, '\n') == NULL, "the text is formatted");

    root = cJSON_Parse(text);
    free(text);
    UT_ASSERT_MSG(root != NULL, "the writer's text does not parse");

    v = cJSON_GetObjectItemCaseSensitive(root, "version");
    UT_ASSERT_MSG(cJSON_IsNumber(v) && v->valueint == SCN_RECORD_VERSION,
                  "version is not %d", SCN_RECORD_VERSION);
    UT_ASSERT_MSG(srStrIs(root, "map", "Hand.map"), "map is wrong");
    UT_ASSERT_MSG(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root,
                                                                 "mods_enabled")),
                  "mods_enabled is not false");

    v = cJSON_GetObjectItemCaseSensitive(root, "rules");
    UT_ASSERT_MSG(cJSON_IsObject(v) && cJSON_GetArraySize(v) == 1,
                  "rules is not one key");
    v = cJSON_GetObjectItemCaseSensitive(v, "tank_death_ticks");
    UT_ASSERT_MSG(cJSON_IsNumber(v) && v->valuedouble == 400.0,
                  "rules.tank_death_ticks is not 400");

    rows = cJSON_GetObjectItemCaseSensitive(root, "regions");
    UT_ASSERT_MSG(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 1,
                  "regions is not one row");
    row = cJSON_GetArrayItem(rows, 0);
    UT_ASSERT_MSG(srStrIs(row, "name", "keep") &&
                      srStrIs(row, "file", "base.lua") &&
                      cJSON_GetObjectItemCaseSensitive(row, "x")->valueint == 1 &&
                      cJSON_GetObjectItemCaseSensitive(row, "y")->valueint == 2 &&
                      cJSON_GetObjectItemCaseSensitive(row, "w")->valueint == 3 &&
                      cJSON_GetObjectItemCaseSensitive(row, "h")->valueint == 4,
                  "the region row is wrong");

    rows = cJSON_GetObjectItemCaseSensitive(root, "scripts");
    UT_ASSERT_MSG(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 2,
                  "scripts is not two rows");
    row = cJSON_GetArrayItem(rows, 0);
    UT_ASSERT_MSG(srStrIs(row, "file", "base.lua") &&
                      srStrIs(row, "source", "map") &&
                      srStrIs(row, "kind", "scenario"),
                  "script row 0 is wrong");
    v = cJSON_GetObjectItemCaseSensitive(row, "manifest");
    UT_ASSERT_MSG(cJSON_IsObject(v) && srStrIs(v, "name", "Base"),
                  "script row 0's manifest is not an object named Base");
    row = cJSON_GetArrayItem(rows, 1);
    UT_ASSERT_MSG(srStrIs(row, "file", "extra.lua") &&
                      srStrIs(row, "source", "server") &&
                      srStrIs(row, "kind", "mod"),
                  "script row 1 is wrong");
    v = cJSON_GetObjectItemCaseSensitive(row, "manifest");
    UT_ASSERT_MSG(cJSON_IsObject(v) && srStrIs(v, "name", "Extra"),
                  "script row 1's manifest is not an object named Extra");
    cJSON_Delete(root);

    /* A manifest that is not a JSON object is refused with a reason. */
    d.scripts[1].manifestJson = "not json";
    err[0] = '\0';
    text = scnRecordJsonWrite(&d, err, sizeof(err));
    UT_ASSERT_MSG(text == NULL && err[0] != '\0',
                  "a broken manifest was not refused");
    return 0;
}
