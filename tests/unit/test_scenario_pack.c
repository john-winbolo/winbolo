/*
 * The server writing a map's scenario into the map file.
 *
 * scnPackMap reads the script beside a map, derives the manifest from the
 * table that script declares, and writes the map followed by a WBSC container
 * holding the two. What it writes is what scenarioHostAttach reads back, so
 * the case that matters most here is the one that holds the manifest it wrote
 * against the script's own table: a package whose two halves disagree is
 * refused at load, and a packer is the only thing that can put them out of
 * step.
 *
 * The map is a copy of one of the baseline maps rather than a synthetic BMAP:
 * the pack builds a ServerSim from the file, so the fixture has to be a map
 * that loads and not merely one that measures. The script beside it is the
 * fixture's own, so the case does not move when the shipped scenario does.
 *
 * Each case copies its map under its own scratch directory, so `ctest -j`
 * running cases as separate processes cannot have two of them packing one
 * file.
 *
 * run_scenario_pack_writes_container
 *      — a map with a loose script packs, the container opens, and it holds
 *        the manifest and a main.lua byte-identical to the script on disk
 * run_scenario_pack_manifest_agrees
 *      — the manifest that was written, parsed back, agrees with the script's
 *        own table: a package this wrote cannot refuse itself at load
 * run_scenario_pack_replaces_trailer
 *      — packing twice gives the same bytes, and the second container replaces
 *        the first rather than being written after it
 * run_scenario_pack_refuses_unscripted
 *      — a map with no script beside it is refused with a reason, and the file
 *        is left as it was
 * run_scenario_pack_script_mod
 *      — a loose mod script packs to a .scenario of its own: the container
 *        opens, its manifest says what the table said, main.lua is the script
 *        byte for byte, and the directory lister reads it
 * run_scenario_pack_script_refusals
 *      — a script with a problem, one with no scenario table and a bound one
 *        are refused and leave nothing behind; a second pack replaces the
 *        first
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "gametype.h"              /* gameOpen */
#include "server_sim.h"            /* serverSimCreate / serverSimDestroy */
#include "scenario_dir.h"          /* scnDirReadPackage */
#include "scenario_host.h"         /* SCN_SCRIPT_SUFFIX */
#include "scenario_manifest.h"
#include "scenario_manifest_json.h"
#include "scenario_pack.h"
#include "scenario_package.h"
#include "scenario_validate.h"
#include "test_harness.h"

/* The maps the baseline runs play. CMake passes the absolute path; the
 * fallback is the path from the source root, for a run started there. */
#ifndef WB_BASELINE_MAPS_DIR
#define WB_BASELINE_MAPS_DIR "tests/baseline/maps"
#endif

/* The one that is copied. Any map would do — the script below says nothing
 * about the entities on it — and this is the map the branch's own scenario
 * plays on. */
#define SP_SOURCE_MAP "Wave Defense.map"

/* ── What is packed ───────────────────────────────────────────────── */

/* A table with nothing in it that depends on the map: no tags and no regions,
 * so it checks out against whichever map the copy was made from. */
static const char kSpScript[] =
    "scenario = {\n"
    "  name = \"Packed By The Server\",\n"
    "  description = \"Written into the map by -pack\",\n"
    "  api = 1,\n"
    "  rules = { tank_reload_ticks = 7 },\n"
    "  lobby = {\n"
    "    max_players = 8,\n"
    "    teams = { { id = 1, bots = 2, max_bots = 4 } },\n"
    "  },\n"
    "}\n";

/* A table the checks refuse: an api no build implements. */
static const char kSpBadScript[] =
    "scenario = { name = \"Too New\", api = 9999 }\n";

/* ── Fixtures ─────────────────────────────────────────────────────── */

static const char *spMapsDir(void) {
    const char *dir = getenv("WB_BASELINE_MAPS_DIR");

    if (dir == NULL || dir[0] == '\0') {
        dir = WB_BASELINE_MAPS_DIR;
    }
    return dir;
}

static bool spReadWhole(const char *path, uint8_t **out, size_t *outLen) {
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

/* .../X.map is accompanied by .../X.scenario.lua. */
static void spScriptPath(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);

    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

static bool spPutScript(const char *scriptPath, const char *lua) {
    FILE *f = fopen(scriptPath, "wb");

    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

/* One baseline map copied under this test's own scratch directory, with
 * whatever the last run left beside it removed. The path it landed at goes
 * into mapPath. */
static bool spCopyMap(const char *leaf, char *mapPath, size_t mapPathLen) {
    char     source[1024];
    char     script[1024];
    uint8_t *bytes = NULL;
    size_t   len   = 0;
    FILE    *f;
    bool     ok;

    if (!utScratchPath(mapPath, mapPathLen, leaf)) {
        return false;
    }
    snprintf(source, sizeof(source), "%s/%s", spMapsDir(), SP_SOURCE_MAP);
    if (!spReadWhole(source, &bytes, &len)) {
        return false;
    }
    f = fopen(mapPath, "wb");
    if (f == NULL) {
        free(bytes);
        return false;
    }
    ok = fwrite(bytes, 1, len, f) == len;
    fclose(f);
    free(bytes);

    spScriptPath(mapPath, script, sizeof(script));
    remove(script);
    return ok;
}

static void spClean(const char *mapPath) {
    char script[1024];

    spScriptPath(mapPath, script, sizeof(script));
    remove(script);
    remove(mapPath);
}

/* The archive length the container's header declares, so a case can say the
 * container runs to the end of the file and nothing follows it. */
static size_t spArchiveLen(const uint8_t *chunk) {
    return (size_t)chunk[6] | ((size_t)chunk[7] << 8) |
           ((size_t)chunk[8] << 16) | ((size_t)chunk[9] << 24);
}

/* ── 1. A map with a script beside it packs ───────────────────────── */

int run_scenario_pack_writes_container(void) {
    char           mapPath[1024];
    char           script[1024];
    char           err[512];
    uint8_t       *file     = NULL;
    size_t         fileLen  = 0;
    uint8_t       *onDisk   = NULL;
    size_t         onDiskLen = 0;
    uint8_t       *lua      = NULL;
    size_t         luaLen   = 0;
    const uint8_t *chunk    = NULL;
    size_t         chunkLen = 0;
    ScnPackage    *p;

    UT_ASSERT_MSG(spCopyMap("pack_writes.map", mapPath, sizeof(mapPath)),
                  "the map fixture could not be copied from %s", spMapsDir());
    spScriptPath(mapPath, script, sizeof(script));
    UT_ASSERT(spPutScript(script, kSpScript));

    err[0] = '\0';
    UT_ASSERT_MSG(scnPackMap(mapPath, err, sizeof(err)),
                  "a map with a script beside it was not packed: %s", err);

    UT_ASSERT(spReadWhole(mapPath, &file, &fileLen));
    UT_ASSERT_MSG(scnPackageFindInMap(file, fileLen, &chunk, &chunkLen),
                  "the packed map carries no container");

    p = scnPackageOpen(chunk, chunkLen, err, sizeof(err));
    UT_ASSERT_MSG(p != NULL, "the container does not open: %s", err);
    UT_ASSERT_MSG(scnPackageHasEntry(p, SCN_PACKAGE_MANIFEST_ENTRY),
                  "the container holds no %s", SCN_PACKAGE_MANIFEST_ENTRY);
    UT_ASSERT_MSG(scnPackageHasEntry(p, SCN_PACKAGE_SCRIPT_ENTRY),
                  "the container holds no %s", SCN_PACKAGE_SCRIPT_ENTRY);
    UT_ASSERT_MSG(scnPackageEntryCount(p) == 2,
                  "the container holds %d entries, expected the manifest and "
                  "the script", scnPackageEntryCount(p));
    /* Brains are not packed yet, and nothing should have invented one. */
    UT_ASSERT_MSG(scnPackageBrainCount(p) == 0, "%d brains were packed",
                  scnPackageBrainCount(p));

    /* The script went in as it is on disk, byte for byte. */
    UT_ASSERT_MSG(scnPackageReadEntry(p, SCN_PACKAGE_SCRIPT_ENTRY,
                                      (size_t)SCN_SCRIPT_MAX_BYTES, &lua,
                                      &luaLen, NULL, 0),
                  "%s could not be read back", SCN_PACKAGE_SCRIPT_ENTRY);
    UT_ASSERT(spReadWhole(script, &onDisk, &onDiskLen));
    UT_ASSERT_MSG(luaLen == onDiskLen,
                  "the packed script is %lu bytes and the one on disk is %lu",
                  (unsigned long)luaLen, (unsigned long)onDiskLen);
    UT_ASSERT_MSG(memcmp(lua, onDisk, luaLen) == 0,
                  "the packed script is not the script on disk");

    free(lua);
    free(onDisk);
    scnPackageClose(p);
    free(file);
    spClean(mapPath);
    return 0;
}

/* ── 2. And the manifest it wrote agrees with the script ──────────── */

/* The host holds a package's manifest against the table its script declares
   and refuses the package when the two differ. A manifest with a field the
   packer filled in, or one missing a field the table set, would therefore make
   a package that refuses itself — which is a failure nothing sees until a
   round will not start. */
int run_scenario_pack_manifest_agrees(void) {
    char                    mapPath[1024];
    char                    script[1024];
    char                    err[512];
    char                    key[SCN_VALIDATE_KEY_LEN];
    uint8_t                *file     = NULL;
    size_t                  fileLen  = 0;
    uint8_t                *json     = NULL;
    size_t                  jsonLen  = 0;
    const uint8_t          *chunk    = NULL;
    size_t                  chunkLen = 0;
    ScnPackage             *p;
    ScnManifestDoc         *doc;
    const ScenarioManifest *fromJson;
    ServerSim              *sim;
    ScnValidateResult       result;

    UT_ASSERT_MSG(spCopyMap("pack_agrees.map", mapPath, sizeof(mapPath)),
                  "the map fixture could not be copied from %s", spMapsDir());
    spScriptPath(mapPath, script, sizeof(script));
    UT_ASSERT(spPutScript(script, kSpScript));

    err[0] = '\0';
    UT_ASSERT_MSG(scnPackMap(mapPath, err, sizeof(err)),
                  "a map with a script beside it was not packed: %s", err);

    UT_ASSERT(spReadWhole(mapPath, &file, &fileLen));
    UT_ASSERT(scnPackageFindInMap(file, fileLen, &chunk, &chunkLen));
    p = scnPackageOpen(chunk, chunkLen, err, sizeof(err));
    UT_ASSERT_MSG(p != NULL, "the container does not open: %s", err);
    UT_ASSERT(scnPackageReadEntry(p, SCN_PACKAGE_MANIFEST_ENTRY,
                                  SCN_PACKAGE_MANIFEST_MAX_BYTES, &json,
                                  &jsonLen, NULL, 0));

    doc = scnManifestParse(json, jsonLen, NULL, err, sizeof(err));
    UT_ASSERT_MSG(doc != NULL, "the manifest that was written will not parse: "
                  "%s", err);
    fromJson = scnManifestValues(doc);
    UT_ASSERT(fromJson != NULL);

    /* The table the script declares, read the way the host reads it. */
    sim = serverSimCreate(mapPath, gameOpen, false, 0, -1);
    UT_ASSERT_MSG(sim != NULL, "the packed map will not load: %s", mapPath);
    UT_ASSERT_MSG(scenarioValidateMap(sim, mapPath, &result),
                  "the script stopped checking out once the map was packed");
    UT_ASSERT(result.haveManifest);

    key[0] = '\0';
    err[0] = '\0';
    UT_ASSERT_MSG(scnManifestAgrees(fromJson, &result.manifest, key,
                                    sizeof(key), err, sizeof(err)),
                  "the manifest that was packed disagrees with the script at "
                  "'%s': %s", key, err);

    /* And it says what the table said rather than a default for what the table
       left out: the script states no game, so neither does the manifest. */
    UT_ASSERT_MSG(fromJson->game[0] == '\0',
                  "the manifest asks for game '%s', which the script never "
                  "said", fromJson->game);
    UT_ASSERT_MSG(strcmp(fromJson->name, "Packed By The Server") == 0,
                  "the manifest calls this '%s'", fromJson->name);
    UT_ASSERT_MSG(fromJson->numRules == 1, "%u rules were packed",
                  (unsigned)fromJson->numRules);

    /* The manifest names the entry the script actually went in as. */
    UT_ASSERT_MSG(strcmp(scnManifestScriptEntry(doc),
                         SCN_PACKAGE_SCRIPT_ENTRY) == 0,
                  "the manifest points at '%s'", scnManifestScriptEntry(doc));
    UT_ASSERT_MSG(scnManifestBrainCount(doc) == 0,
                  "the manifest lists %d brains", scnManifestBrainCount(doc));

    serverSimDestroy(sim);
    scnManifestFree(doc);
    free(json);
    scnPackageClose(p);
    free(file);
    spClean(mapPath);
    return 0;
}

/* ── 3. A second pack replaces the first ──────────────────────────── */

int run_scenario_pack_replaces_trailer(void) {
    char           mapPath[1024];
    char           script[1024];
    char           err[512];
    uint8_t       *first     = NULL;
    size_t         firstLen  = 0;
    uint8_t       *second    = NULL;
    size_t         secondLen = 0;
    const uint8_t *chunk     = NULL;
    size_t         chunkLen  = 0;

    UT_ASSERT_MSG(spCopyMap("pack_twice.map", mapPath, sizeof(mapPath)),
                  "the map fixture could not be copied from %s", spMapsDir());
    spScriptPath(mapPath, script, sizeof(script));
    UT_ASSERT(spPutScript(script, kSpScript));

    err[0] = '\0';
    UT_ASSERT_MSG(scnPackMap(mapPath, err, sizeof(err)),
                  "the first pack was refused: %s", err);
    UT_ASSERT(spReadWhole(mapPath, &first, &firstLen));

    /* The second pack is handed a map that already carries a container. */
    err[0] = '\0';
    UT_ASSERT_MSG(scnPackMap(mapPath, err, sizeof(err)),
                  "a map that was already packed could not be packed again: "
                  "%s", err);
    UT_ASSERT(spReadWhole(mapPath, &second, &secondLen));

    UT_ASSERT_MSG(secondLen == firstLen,
                  "the map is %lu bytes after one pack and %lu after two: the "
                  "second container was written after the first",
                  (unsigned long)firstLen, (unsigned long)secondLen);
    UT_ASSERT_MSG(memcmp(first, second, firstLen) == 0,
                  "packing twice gave two different files");

    /* And the container the file ends on is the whole of what follows the map:
       its own header says so, so nothing is hiding behind it. */
    UT_ASSERT(scnPackageFindInMap(second, secondLen, &chunk, &chunkLen));
    UT_ASSERT_MSG(chunkLen == SCN_PACKAGE_HEADER_LEN + spArchiveLen(chunk),
                  "the container declares %lu bytes of archive and the file "
                  "holds %lu after the map",
                  (unsigned long)spArchiveLen(chunk),
                  (unsigned long)(chunkLen - SCN_PACKAGE_HEADER_LEN));

    free(first);
    free(second);
    spClean(mapPath);
    return 0;
}

/* ── 4. A map with nothing to pack ────────────────────────────────── */

int run_scenario_pack_refuses_unscripted(void) {
    char     mapPath[1024];
    char     script[1024];
    char     err[512];
    uint8_t *before    = NULL;
    size_t   beforeLen = 0;
    uint8_t *after     = NULL;
    size_t   afterLen  = 0;

    UT_ASSERT_MSG(spCopyMap("pack_unscripted.map", mapPath, sizeof(mapPath)),
                  "the map fixture could not be copied from %s", spMapsDir());
    spScriptPath(mapPath, script, sizeof(script));
    UT_ASSERT(spReadWhole(mapPath, &before, &beforeLen));

    err[0] = '\0';
    UT_ASSERT_MSG(!scnPackMap(mapPath, err, sizeof(err)),
                  "a map with no script beside it was packed anyway");
    UT_ASSERT_MSG(err[0] != '\0', "the refusal said nothing");

    UT_ASSERT(spReadWhole(mapPath, &after, &afterLen));
    UT_ASSERT_MSG(afterLen == beforeLen && memcmp(before, after, beforeLen) == 0,
                  "the map was written to anyway");
    free(after);

    /* A script that does not check out is the same answer: the map is left
       alone and the reason sends the author to -validate. */
    UT_ASSERT(spPutScript(script, kSpBadScript));
    err[0] = '\0';
    UT_ASSERT_MSG(!scnPackMap(mapPath, err, sizeof(err)),
                  "a script with problems against it was packed anyway");
    UT_ASSERT_MSG(strstr(err, "-validate") != NULL,
                  "the refusal does not say where to see the problems: %s",
                  err);

    after    = NULL;
    afterLen = 0;
    UT_ASSERT(spReadWhole(mapPath, &after, &afterLen));
    UT_ASSERT_MSG(afterLen == beforeLen && memcmp(before, after, beforeLen) == 0,
                  "the map was written to anyway");

    free(before);
    free(after);
    spClean(mapPath);
    return 0;
}

/* ── 5. A loose script packs to a .scenario of its own ────────────── */

/* A mod: kind "mod" and not bound, which is the file a player publishes
   without a map. */
static const char kSpModScript[] =
    "scenario = {\n"
    "  name = \"Packed Mod\",\n"
    "  description = \"Packed from a loose script\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  rules = { tank_reload_ticks = 7 },\n"
    "}\n";

/* The same mod under another name, so a second pack can be told apart from
   the first. */
static const char kSpModScriptRenamed[] =
    "scenario = {\n"
    "  name = \"Packed Mod Again\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n";

/* Lua that runs and declares no scenario table. */
static const char kSpNoTableScript[] = "local unused = 1\n";

/* A table that says it belongs to one map. */
static const char kSpBoundScript[] =
    "scenario = { name = \"Bound\", api = 1, bound = true }\n";

static bool spExists(const char *path) {
    return SDL_GetPathInfo(path, NULL);
}

/* The temporary file a pack of outPath writes through. */
static void spPackingPath(const char *outPath, char *out, size_t outLen) {
    snprintf(out, outLen, "%s.packing", outPath);
}

/* The name and kind the package at path carries in its manifest.json, read
   straight out of the container. */
static bool spPackageManifest(const char *path, ScenarioManifest *out) {
    uint8_t        *file    = NULL;
    size_t          fileLen = 0;
    uint8_t        *json    = NULL;
    size_t          jsonLen = 0;
    ScnPackage     *p;
    ScnManifestDoc *doc;
    bool            ok = false;

    if (!spReadWhole(path, &file, &fileLen)) {
        return false;
    }
    p = scnPackageOpen(file, fileLen, NULL, 0);
    if (p != NULL &&
        scnPackageReadEntry(p, SCN_PACKAGE_MANIFEST_ENTRY,
                            SCN_PACKAGE_MANIFEST_MAX_BYTES, &json, &jsonLen,
                            NULL, 0)) {
        doc = scnManifestParse(json, jsonLen, NULL, NULL, 0);
        if (doc != NULL && scnManifestValues(doc) != NULL) {
            *out = *scnManifestValues(doc);
            ok   = true;
        }
        scnManifestFree(doc);
    }
    free(json);
    scnPackageClose(p);
    free(file);
    return ok;
}

int run_scenario_pack_script_mod(void) {
    char              luaPath[1024];
    char              outPath[1024];
    char              packing[1100];
    char              err[512];
    uint8_t          *file      = NULL;
    size_t            fileLen   = 0;
    uint8_t          *lua       = NULL;
    size_t            luaLen    = 0;
    uint8_t          *onDisk    = NULL;
    size_t            onDiskLen = 0;
    ScnPackage       *p;
    ScenarioManifest *m;

    UT_ASSERT(utScratchPath(luaPath, sizeof(luaPath), "Packed Mod.lua"));
    UT_ASSERT(utScratchPath(outPath, sizeof(outPath), "Packed Mod.scenario"));
    spPackingPath(outPath, packing, sizeof(packing));
    remove(outPath);
    remove(packing);
    UT_ASSERT(spPutScript(luaPath, kSpModScript));

    err[0] = '\0';
    UT_ASSERT_MSG(scnPackScript(luaPath, outPath, err, sizeof(err)),
                  "a loose mod script was not packed: %s", err);
    UT_ASSERT_MSG(!spExists(packing), "the temporary file was left behind");

    /* The file is the container on its own, and holds the two entries. */
    UT_ASSERT(spReadWhole(outPath, &file, &fileLen));
    p = scnPackageOpen(file, fileLen, err, sizeof(err));
    UT_ASSERT_MSG(p != NULL, "the package does not open: %s", err);
    UT_ASSERT_MSG(scnPackageEntryCount(p) == 2,
                  "the package holds %d entries, expected the manifest and "
                  "the script", scnPackageEntryCount(p));
    UT_ASSERT(scnPackageHasEntry(p, SCN_PACKAGE_MANIFEST_ENTRY));
    UT_ASSERT(scnPackageHasEntry(p, SCN_PACKAGE_SCRIPT_ENTRY));

    /* The script went in as it is on disk, byte for byte. */
    UT_ASSERT_MSG(scnPackageReadEntry(p, SCN_PACKAGE_SCRIPT_ENTRY,
                                      (size_t)SCN_SCRIPT_MAX_BYTES, &lua,
                                      &luaLen, NULL, 0),
                  "%s could not be read back", SCN_PACKAGE_SCRIPT_ENTRY);
    UT_ASSERT(spReadWhole(luaPath, &onDisk, &onDiskLen));
    UT_ASSERT_MSG(luaLen == onDiskLen && memcmp(lua, onDisk, luaLen) == 0,
                  "the packed script is not the script on disk");

    /* The manifest says what the table said. */
    m = (ScenarioManifest *)malloc(sizeof(*m));
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(spPackageManifest(outPath, m),
                  "the packed manifest.json will not parse");
    UT_ASSERT_MSG(strcmp(m->name, "Packed Mod") == 0,
                  "the manifest calls this '%s'", m->name);
    UT_ASSERT_MSG(m->kind == scnKindKeepsWinCondition,
                  "the manifest's kind is %d, not mod", (int)m->kind);
    UT_ASSERT(!m->bound);

    /* And the directory lister reads it as it reads any .scenario. */
    memset(m, 0, sizeof(*m));
    UT_ASSERT_MSG(scnDirReadPackage(outPath, m),
                  "the lister could not read the package");
    UT_ASSERT_MSG(strcmp(m->name, "Packed Mod") == 0,
                  "the lister calls this '%s'", m->name);
    UT_ASSERT(m->kind == scnKindKeepsWinCondition);

    free(m);
    free(onDisk);
    free(lua);
    scnPackageClose(p);
    free(file);
    remove(outPath);
    remove(luaPath);
    return 0;
}

/* ── 6. What a loose script is refused for ────────────────────────── */

/* One refusal: the script at luaPath holding lua is not packed, the reason
   says `says`, and neither outPath nor the file it would have been written
   through is there after. */
static int spExpectRefused(const char *luaPath, const char *outPath,
                           const char *lua, const char *what,
                           const char *says) {
    char packing[1100];
    char err[512];

    spPackingPath(outPath, packing, sizeof(packing));
    UT_ASSERT(spPutScript(luaPath, lua));
    err[0] = '\0';
    UT_ASSERT_MSG(!scnPackScript(luaPath, outPath, err, sizeof(err)),
                  "%s was packed anyway", what);
    UT_ASSERT_MSG(strstr(err, says) != NULL,
                  "the refusal of %s does not say \"%s\": %s", what, says, err);
    UT_ASSERT_MSG(!spExists(outPath), "%s left a package behind", what);
    UT_ASSERT_MSG(!spExists(packing), "%s left the temporary file behind",
                  what);
    return 0;
}

int run_scenario_pack_script_refusals(void) {
    char             luaPath[1024];
    char             outPath[1024];
    char             packing[1100];
    char             err[512];
    ScenarioManifest *m;

    UT_ASSERT(utScratchPath(luaPath, sizeof(luaPath), "Refused.lua"));
    UT_ASSERT(utScratchPath(outPath, sizeof(outPath), "Refused.scenario"));
    spPackingPath(outPath, packing, sizeof(packing));
    remove(outPath);
    remove(packing);

    UT_ASSERT(spExpectRefused(luaPath, outPath, kSpBadScript,
                              "a script with a problem", "-validate") == 0);
    UT_ASSERT(spExpectRefused(luaPath, outPath, kSpNoTableScript,
                              "a script with no scenario table",
                              "no scenario table") == 0);
    UT_ASSERT(spExpectRefused(luaPath, outPath, kSpBoundScript,
                              "a bound script", "bound") == 0);

    /* A pack over a package already there replaces it. */
    UT_ASSERT(spPutScript(luaPath, kSpModScript));
    err[0] = '\0';
    UT_ASSERT_MSG(scnPackScript(luaPath, outPath, err, sizeof(err)),
                  "the first pack was refused: %s", err);
    UT_ASSERT(spPutScript(luaPath, kSpModScriptRenamed));
    err[0] = '\0';
    UT_ASSERT_MSG(scnPackScript(luaPath, outPath, err, sizeof(err)),
                  "a pack over an existing package was refused: %s", err);
    UT_ASSERT_MSG(!spExists(packing), "the temporary file was left behind");

    m = (ScenarioManifest *)malloc(sizeof(*m));
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(spPackageManifest(outPath, m),
                  "the replaced package will not read");
    UT_ASSERT_MSG(strcmp(m->name, "Packed Mod Again") == 0,
                  "the package still says '%s' after the second pack",
                  m->name);

    /* A refusal leaves the package from before alone. */
    UT_ASSERT(spPutScript(luaPath, kSpBoundScript));
    UT_ASSERT(!scnPackScript(luaPath, outPath, err, sizeof(err)));
    UT_ASSERT(spPackageManifest(outPath, m));
    UT_ASSERT_MSG(strcmp(m->name, "Packed Mod Again") == 0,
                  "a refused pack changed the package already there");

    free(m);
    remove(outPath);
    remove(luaPath);
    return 0;
}
