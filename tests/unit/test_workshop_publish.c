/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * What the Settings dialog's Workshop section reads and writes, below the
 * dialog itself. There is no case for the dialog: it is ImGui drawing over
 * these calls.
 *
 *   workshop_map_package_info   a map carrying a scenario reads back its
 *                               name, kind, id and author; a plain map
 *                               answers false
 *   workshop_pack_loose_script  Mods/X.lua packs to Mods/X.scenario and moves
 *                               to Mods/Sources, the listing shows one X, a
 *                               repack keeps the package's Workshop id, and
 *                               a pack that fails leaves X.lua where it was
 *   workshop_sync_index_rows    the sync's index comes back in id order
 *   local_rows_carry_author     a local listing row carries the author the
 *                               package names
 *
 * WB_MOD_DIR_USER, WB_MOD_DIR_WORKSHOP and WB_MOD_DIR_SHIPPED point the
 * directories at this case's scratch path, and are set only for the length
 * of each call so a failed case leaves the environment as it found it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "server_sim.h"
#include "scenario_chunk.h"      /* scnIoWriteMapChunk, scnIoSetWorkshopId */
#include "scenario_host.h"
#include "scenario_manifest.h"
#include "scenario_pack.h"       /* scnPackScript */
#include "workshop_sync.h"
#include "test_harness.h"

/* The repository, so the map fixture is found whatever the working directory
 * is. CMake passes the absolute path. */
#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

#define WP_SOURCE_MAP "data/maps/Everard Island.map"

/* Both past 2^53, so a value that went through a double on the way would
   come back with its last digits changed. */
#define WP_ID     76561198000000077ULL
#define WP_AUTHOR 76561198000000321ULL

#define WP_ROWS_MAX 16

static char wpModDir[512];
static char wpWorkshopDir[512];
static char wpShippedDir[512];

static const char kWpMod[] =
    "scenario = {\n"
    "  name = \"Packed Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n";

static const char kWpModEdited[] =
    "scenario = {\n"
    "  name = \"Packed Mod\",\n"
    "  description = \"Edited after the first publish\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n";

/* A table the checks refuse: an api no build implements. */
static const char kWpBad[] =
    "scenario = { name = \"Too New\", api = 9999 }\n";

/* ── The environment ─────────────────────────────────────────────── */

static void wpSetEnv(const char *key, const char *val) {
#ifdef _WIN32
    _putenv_s(key, val != NULL ? val : "");
#else
    if (val != NULL) setenv(key, val, 1);
    else unsetenv(key);
#endif
}

static void wpDirsOn(void) {
    wpSetEnv("WB_MOD_DIR_USER", wpModDir);
    wpSetEnv("WB_MOD_DIR_WORKSHOP", wpWorkshopDir);
    wpSetEnv("WB_MOD_DIR_SHIPPED", wpShippedDir);
}

static void wpDirsOff(void) {
    wpSetEnv("WB_MOD_DIR_USER", NULL);
    wpSetEnv("WB_MOD_DIR_WORKSHOP", NULL);
    wpSetEnv("WB_MOD_DIR_SHIPPED", NULL);
}

/* Mods, and an empty Workshop and shipped directory beside it, under this
   case's scratch path. */
static bool wpMakeDirs(void) {
    return utScratchPath(wpModDir, sizeof(wpModDir), "Mods") &&
           SDL_CreateDirectory(wpModDir) &&
           utScratchPath(wpWorkshopDir, sizeof(wpWorkshopDir), "Workshop") &&
           SDL_CreateDirectory(wpWorkshopDir) &&
           utScratchPath(wpShippedDir, sizeof(wpShippedDir), "Shipped") &&
           SDL_CreateDirectory(wpShippedDir);
}

static bool wpPack(const char *lua, char *out, size_t outLen, char *err,
                   size_t errLen) {
    bool ok;

    wpDirsOn();
    ok = scenarioHostPackLooseScript(lua, out, outLen, err, errLen);
    wpDirsOff();
    return ok;
}

static int wpList(ServerScenarioEntry *out, int max) {
    int n;

    wpDirsOn();
    n = scenarioHostListLocalScripts(out, max);
    wpDirsOff();
    return n;
}

/* ── Files ───────────────────────────────────────────────────────── */

static bool wpWrite(const char *path, const void *bytes, size_t len) {
    FILE *f = fopen(path, "wb");
    bool  ok;

    if (f == NULL) return false;
    ok = fwrite(bytes, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool wpExists(const char *path) {
    return SDL_GetPathInfo(path, NULL);
}

/* Whether the file at path holds exactly text. */
static bool wpHolds(const char *path, const char *text) {
    size_t len = 0;
    void  *got = SDL_LoadFile(path, &len);
    bool   same;

    if (got == NULL) return false;
    same = len == strlen(text) && memcmp(got, text, len) == 0;
    SDL_free(got);
    return same;
}

/* One committed map copied to this case's scratch directory. */
static bool wpCopyMap(const char *leaf, char *mapPath, size_t mapPathLen) {
    char   source[1024];
    size_t len = 0;
    void  *bytes;
    bool   ok;

    if (!utScratchPath(mapPath, mapPathLen, leaf)) return false;
    snprintf(source, sizeof(source), "%s/%s", WB_REPO_ROOT_DIR, WP_SOURCE_MAP);
    bytes = SDL_LoadFile(source, &len);
    if (bytes == NULL) return false;
    ok = wpWrite(mapPath, bytes, len);
    SDL_free(bytes);
    return ok;
}

/* The listing's row for file, or NULL. */
static const ServerScenarioEntry *wpRow(const ServerScenarioEntry *rows, int n,
                                        const char *file) {
    int i;

    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].file, file) == 0) return &rows[i];
    }
    return NULL;
}

/* ── Cases ───────────────────────────────────────────────────────── */

int run_workshop_map_package_info(void) {
    ServerScenarioEntry e;
    ScenarioManifest   *m;
    char                path[1024];
    char                err[512];
    bool                packed;

    UT_ASSERT_MSG(wpCopyMap("info.map", path, sizeof(path)),
                  "the map fixture could not be copied from %s",
                  WB_REPO_ROOT_DIR);

    /* A plain map carries no scenario. */
    memset(&e, 0xAB, sizeof(e));
    UT_ASSERT_MSG(!scenarioHostMapPackageInfo(path, &e),
                  "a plain map answered as if it carried a scenario");
    UT_ASSERT_MSG(e.file[0] == '\0' && e.workshopId == 0,
                  "the row was not cleared for a plain map");

    m = (ScenarioManifest *)calloc(1, sizeof(*m));
    UT_ASSERT(m != NULL);
    m->api   = 1;
    m->bound = true;
    snprintf(m->name, sizeof(m->name), "Packed Map");
    snprintf(m->description, sizeof(m->description), "Written for the test");
    err[0] = '\0';
    packed = scnIoWriteMapChunk(path, m, "function on_setup() end\n",
                                strlen("function on_setup() end\n"), err,
                                sizeof(err));
    free(m);
    UT_ASSERT_MSG(packed, "the chunk could not be packed on: %s", err);
    err[0] = '\0';
    UT_ASSERT_MSG(scnIoSetWorkshopId(path, WP_ID, WP_AUTHOR, err, sizeof(err)),
                  "the map was not stamped: %s", err);

    UT_ASSERT_MSG(scenarioHostMapPackageInfo(path, &e),
                  "a map carrying a scenario answered false");
    UT_ASSERT_MSG(strcmp(e.file, "info.map") == 0, "the file reads '%s'",
                  e.file);
    UT_ASSERT_MSG(strcmp(e.name, "Packed Map") == 0, "the name reads '%s'",
                  e.name);
    UT_ASSERT_MSG(strcmp(e.description, "Written for the test") == 0,
                  "the description reads '%s'", e.description);
    UT_ASSERT(e.bound);
    UT_ASSERT_MSG(!e.keepsWinCondition, "a scenario read back as a mod");
    UT_ASSERT_MSG(e.workshopId == WP_ID && e.workshopAuthor == WP_AUTHOR,
                  "the id and author read %llu / %llu",
                  (unsigned long long)e.workshopId,
                  (unsigned long long)e.workshopAuthor);

    /* And a file that is not there is false too. */
    UT_ASSERT(utScratchPath(path, sizeof(path), "missing.map"));
    UT_ASSERT(!scenarioHostMapPackageInfo(path, &e));
    return 0;
}

int run_workshop_pack_loose_script(void) {
    ServerScenarioEntry        rows[WP_ROWS_MAX];
    const ServerScenarioEntry *row;
    char                       lua[1024];
    char                       packed[1024];
    char                       moved[1024];
    char                       out[1024];
    char                       err[512];
    int                        n;
    int                        i;
    int                        xRows = 0;

    UT_ASSERT(wpMakeDirs());
    snprintf(lua, sizeof(lua), "%s/X.lua", wpModDir);
    snprintf(packed, sizeof(packed), "%s/X.scenario", wpModDir);
    snprintf(moved, sizeof(moved), "%s/Sources/X.lua", wpModDir);

    /* The first pack: the package is written and the script moved. */
    UT_ASSERT(wpWrite(lua, kWpMod, strlen(kWpMod)));
    err[0] = '\0';
    UT_ASSERT_MSG(wpPack(lua, out, sizeof(out), err, sizeof(err)),
                  "the loose script was not packed: %s", err);
    UT_ASSERT_MSG(strcmp(out, packed) == 0, "the package is at %s", out);
    UT_ASSERT_MSG(wpExists(packed), "no X.scenario beside where X.lua was");
    UT_ASSERT_MSG(!wpExists(lua), "X.lua is still in Mods");
    UT_ASSERT_MSG(wpHolds(moved, kWpMod), "Mods/Sources/X.lua is not the "
                  "script that was packed");

    /* The mod list shows the package and not the script beside it. */
    n = wpList(rows, WP_ROWS_MAX);
    for (i = 0; i < n; i++) {
        if (strncmp(rows[i].file, "X.", 2) == 0) xRows++;
    }
    UT_ASSERT_MSG(xRows == 1, "%d rows named X in %d listed", xRows, n);
    row = wpRow(rows, n, "X.scenario");
    UT_ASSERT_MSG(row != NULL && strcmp(row->name, "Packed Mod") == 0,
                  "X.scenario is not listed under its manifest's name");

    /* The package is published: it now carries an item. An edited script
       packed again keeps that item, and replaces the copy in Sources. */
    err[0] = '\0';
    UT_ASSERT_MSG(scnIoSetWorkshopId(packed, WP_ID, WP_AUTHOR, err,
                                     sizeof(err)),
                  "the package was not stamped: %s", err);
    UT_ASSERT(wpWrite(lua, kWpModEdited, strlen(kWpModEdited)));
    err[0] = '\0';
    UT_ASSERT_MSG(wpPack(lua, out, sizeof(out), err, sizeof(err)),
                  "the edited script was not packed: %s", err);
    UT_ASSERT_MSG(wpHolds(moved, kWpModEdited),
                  "Mods/Sources/X.lua was not replaced by the edited script");
    n   = wpList(rows, WP_ROWS_MAX);
    row = wpRow(rows, n, "X.scenario");
    UT_ASSERT_MSG(row != NULL, "X.scenario is not listed after the repack");
    UT_ASSERT_MSG(row->workshopId == WP_ID && row->workshopAuthor == WP_AUTHOR,
                  "the repack lost the item: %llu / %llu",
                  (unsigned long long)row->workshopId,
                  (unsigned long long)row->workshopAuthor);
    UT_ASSERT_MSG(strcmp(row->description,
                         "Edited after the first publish") == 0,
                  "the package is not the edited script: '%s'",
                  row->description);

    /* A script the pack refuses stays where it was, and Sources keeps the
       copy it had. */
    UT_ASSERT(wpWrite(lua, kWpBad, strlen(kWpBad)));
    err[0] = '\0';
    UT_ASSERT_MSG(!wpPack(lua, out, sizeof(out), err, sizeof(err)),
                  "a script with a problem was packed");
    UT_ASSERT_MSG(err[0] != '\0', "the refusal said nothing");
    UT_ASSERT_MSG(wpHolds(lua, kWpBad), "the refused X.lua was moved");
    UT_ASSERT_MSG(wpHolds(moved, kWpModEdited),
                  "a refused pack replaced the copy in Sources");
    return 0;
}

int run_workshop_sync_index_rows(void) {
    static const char kIndex[] =
        "[\n"
        "  {\"id\": \"300\", \"file\": \"c.lua\", \"size\": 1, \"mtime\": 1},\n"
        "  {\"id\": \"100\", \"file\": \"a.scenario\", \"size\": 1, "
        "\"mtime\": 1},\n"
        "  {\"id\": \"200\", \"file\": \"b.map\", \"size\": 1, \"mtime\": 1}\n"
        "]\n";
    WorkshopSyncRow rows[8];
    char            path[1024];
    int             n;

    UT_ASSERT(utScratchPath(wpWorkshopDir, sizeof(wpWorkshopDir),
                            "Workshop"));
    UT_ASSERT(SDL_CreateDirectory(wpWorkshopDir));
    snprintf(path, sizeof(path), "%s/%s", wpWorkshopDir, WORKSHOP_SYNC_INDEX);
    UT_ASSERT(wpWrite(path, kIndex, strlen(kIndex)));

    wpSetEnv("WB_MOD_DIR_WORKSHOP", wpWorkshopDir);
    n = workshopSyncIndexRows(rows, 8);
    wpSetEnv("WB_MOD_DIR_WORKSHOP", NULL);
    UT_ASSERT_MSG(n == 3, "%d rows, wanted the index's three", n);
    UT_ASSERT_MSG(rows[0].id == 100 && strcmp(rows[0].file, "a.scenario") == 0,
                  "the first row is %llu %s", (unsigned long long)rows[0].id,
                  rows[0].file);
    UT_ASSERT_MSG(rows[1].id == 200 && strcmp(rows[1].file, "b.map") == 0,
                  "the second row is %llu %s", (unsigned long long)rows[1].id,
                  rows[1].file);
    UT_ASSERT_MSG(rows[2].id == 300 && strcmp(rows[2].file, "c.lua") == 0,
                  "the third row is %llu %s", (unsigned long long)rows[2].id,
                  rows[2].file);

    /* Held to max, from the lowest id up. */
    wpSetEnv("WB_MOD_DIR_WORKSHOP", wpWorkshopDir);
    n = workshopSyncIndexRows(rows, 2);
    wpSetEnv("WB_MOD_DIR_WORKSHOP", NULL);
    UT_ASSERT_MSG(n == 2 && rows[0].id == 100 && rows[1].id == 200,
                  "%d rows under a cap of two", n);
    return 0;
}

int run_local_rows_carry_author(void) {
    ServerScenarioEntry        rows[WP_ROWS_MAX];
    const ServerScenarioEntry *row;
    char                       lua[1024];
    char                       pkg[1024];
    char                       err[512];
    int                        n;

    UT_ASSERT(wpMakeDirs());
    /* The script is written outside Mods, so only the package is listed. */
    UT_ASSERT(utScratchPath(lua, sizeof(lua), "Y.lua"));
    snprintf(pkg, sizeof(pkg), "%s/Y.scenario", wpModDir);
    UT_ASSERT(wpWrite(lua, kWpMod, strlen(kWpMod)));
    err[0] = '\0';
    UT_ASSERT_MSG(scnPackScript(lua, pkg, err, sizeof(err)),
                  "the fixture package could not be packed: %s", err);
    err[0] = '\0';
    UT_ASSERT_MSG(scnIoSetWorkshopId(pkg, WP_ID, WP_AUTHOR, err, sizeof(err)),
                  "the fixture package was not stamped: %s", err);
    serverSimNoteScriptDirsChanged();

    n   = wpList(rows, WP_ROWS_MAX);
    row = wpRow(rows, n, "Y.scenario");
    UT_ASSERT_MSG(row != NULL, "Y.scenario is not in the %d listed rows", n);
    UT_ASSERT_MSG(row->source == SERVER_SCENARIO_SOURCE_SERVER,
                  "the row's source is %u", (unsigned)row->source);
    UT_ASSERT_MSG(row->workshopId == WP_ID, "the row's id is %llu",
                  (unsigned long long)row->workshopId);
    UT_ASSERT_MSG(row->workshopAuthor == WP_AUTHOR,
                  "the row's author is %llu",
                  (unsigned long long)row->workshopAuthor);
    return 0;
}
