/*
 * The Workshop item stamped into a scenario file that already exists
 * (scnIoSetWorkshopId in src/scenario_io/scenario_chunk.c).
 *
 * The fixtures are written under this case's own scratch directory: ctest -j
 * runs cases as separate processes in one directory, so a shared fixture name
 * is a race rather than a fixture. The map is a copy of a committed map under
 * data/maps.
 *
 * run_scenario_io_set_workshop_id
 *      — on a .scenario package the id and author read back, a manifest key
 *        this build does not read is still there, and a brain's file and
 *        main.lua are the bytes they were; on a map carrying a chunk the id
 *        reads back and the map data in front of the chunk is unchanged; a
 *        loose .lua and a map with no chunk are refused with the file left
 *        as it was
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bolo_map_validate.h" /* boloMapBodyLength */

#include "scenario_chunk.h"
#include "scenario_manifest.h"
#include "scenario_manifest_json.h"
#include "scenario_package.h"
#include "test_harness.h"

/* The repository, so the map fixture is found whatever the working directory
 * is. CMake passes the absolute path; the fallback is the path from the
 * source root, for a run started there. */
#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

#define WS_SOURCE_MAP "data/maps/Everard Island.map"

/* Both past 2^53, so a value that went through a double on the way would
   come back with its last digits changed. */
#define WS_ID     76561198000000001ULL
#define WS_AUTHOR 76561198000000123ULL

static const char kWsManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"api\": 1,\n"
    "  \"name\": \"Published\",\n"
    "  \"bound\": false,\n"
    "  \"script\": \"main.lua\",\n"
    "  \"brains\": [\"x\"],\n"
    "  \"editor_notes\": \"kept by a build that does not read it\"\n"
    "}\n";

static const char kWsScript[] =
    "function on_setup() end\n";

static const char kWsBrain[] =
    "-- a packaged brain, carried as bytes\n"
    "function think() return 0 end\n";

/* ── Fixtures ─────────────────────────────────────────────────────── */

static bool wsReadWhole(const char *path, uint8_t **out, size_t *outLen) {
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

static bool wsWriteWhole(const char *path, const void *bytes, size_t len) {
    FILE *f = fopen(path, "wb");
    bool  ok;

    if (f == NULL) {
        return false;
    }
    ok = fwrite(bytes, 1, len, f) == len;
    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

/* One committed map copied to this case's scratch directory. */
static bool wsCopyMap(const char *leaf, char *mapPath, size_t mapPathLen) {
    char     source[1024];
    uint8_t *bytes = NULL;
    size_t   len   = 0;
    bool     ok;

    if (!utScratchPath(mapPath, mapPathLen, leaf)) {
        return false;
    }
    snprintf(source, sizeof(source), "%s/%s", WB_REPO_ROOT_DIR, WS_SOURCE_MAP);
    if (!wsReadWhole(source, &bytes, &len)) {
        return false;
    }
    ok = wsWriteWhole(mapPath, bytes, len);
    free(bytes);
    return ok;
}

/* Is the file at path exactly these bytes? */
static bool wsFileIs(const char *path, const uint8_t *want, size_t wantLen) {
    uint8_t *got    = NULL;
    size_t   gotLen = 0;
    bool     same;

    if (!wsReadWhole(path, &got, &gotLen)) {
        return false;
    }
    same = gotLen == wantLen && memcmp(got, want, wantLen) == 0;
    free(got);
    return same;
}

/* One entry of an open container, compared with what it should hold. */
static bool wsEntryIs(ScnPackage *p, const char *name, const char *want) {
    uint8_t *bytes = NULL;
    size_t   len   = 0;
    bool     same;

    if (!scnPackageReadEntry(p, name, 1024u * 1024u, &bytes, &len, NULL, 0)) {
        return false;
    }
    same = len == strlen(want) && memcmp(bytes, want, len) == 0;
    free(bytes);
    return same;
}

/* The manifest of an open container, as text the caller frees, with its
   values copied out. */
static char *wsManifest(ScnPackage *p, uint64_t *id, uint64_t *author) {
    uint8_t        *json = NULL;
    size_t          len  = 0;
    ScnManifestDoc *doc;
    char            err[256];

    *id     = 0;
    *author = 0;
    if (!scnPackageReadEntry(p, SCN_PACKAGE_MANIFEST_ENTRY,
                             SCN_PACKAGE_MANIFEST_MAX_BYTES, &json, &len, err,
                             sizeof(err))) {
        return NULL;
    }
    doc = scnManifestParse(json, len, NULL, err, sizeof(err));
    if (doc != NULL) {
        *id     = scnManifestValues(doc)->workshopId;
        *author = scnManifestValues(doc)->workshopAuthor;
        scnManifestFree(doc);
    }
    /* scnPackageReadEntry leaves a 0 past the content, so the buffer is
       already the text. */
    return (char *)json;
}

/* ── The cases ────────────────────────────────────────────────────── */

static int wsOnPackage(void) {
    ScnPackageEntry entries[3];
    uint8_t        *container = NULL;
    size_t          length    = 0;
    uint8_t        *file      = NULL;
    size_t          fileLen   = 0;
    ScnPackage     *p;
    char            path[1024];
    char            err[512];
    char           *text;
    uint64_t        id;
    uint64_t        author;
    bool            kept;

    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)kWsManifest;
    entries[0].len   = strlen(kWsManifest);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)kWsScript;
    entries[1].len   = strlen(kWsScript);
    entries[2].name    = "brains/x/brain.lua";
    entries[2].bytes   = (const uint8_t *)kWsBrain;
    entries[2].len     = strlen(kWsBrain);
    entries[2].deflate = true;
    err[0] = '\0';
    UT_ASSERT_MSG(scnPackageWrite(entries, 3, &container, &length, err,
                                  sizeof(err)),
                  "the fixture could not be built: %s", err);
    UT_ASSERT(utScratchPath(path, sizeof(path), "workshop_id.scenario"));
    if (!wsWriteWhole(path, container, length)) {
        free(container);
        UT_FAIL("the fixture could not be written to %s", path);
    }
    free(container);

    err[0] = '\0';
    UT_ASSERT_MSG(scnIoSetWorkshopId(path, WS_ID, WS_AUTHOR, err,
                                     sizeof(err)),
                  "the package was not stamped: %s", err);

    UT_ASSERT_MSG(wsReadWhole(path, &file, &fileLen),
                  "the stamped package could not be read back");
    p = scnPackageOpen(file, fileLen, err, sizeof(err));
    if (p == NULL) {
        free(file);
        UT_FAIL("the stamped package will not open: %s", err);
    }
    text = wsManifest(p, &id, &author);
    kept = text != NULL && strstr(text, "editor_notes") != NULL;
    free(text);
    if (id != WS_ID || author != WS_AUTHOR || !kept) {
        scnPackageClose(p);
        free(file);
        UT_FAIL("the package's manifest read back as %llu / %llu, unknown "
                "key %s", (unsigned long long)id, (unsigned long long)author,
                kept ? "kept" : "lost");
    }
    if (!wsEntryIs(p, "brains/x/brain.lua", kWsBrain) ||
        !wsEntryIs(p, SCN_PACKAGE_SCRIPT_ENTRY, kWsScript)) {
        scnPackageClose(p);
        free(file);
        UT_FAIL("an entry other than the manifest changed");
    }
    scnPackageClose(p);
    free(file);
    return 0;
}

static int wsOnMap(void) {
    ScenarioManifest *m;
    char              path[1024];
    char              err[512];
    uint8_t          *before    = NULL;
    size_t            beforeLen = 0;
    size_t            body      = 0;
    uint8_t          *after     = NULL;
    size_t            afterLen  = 0;
    const uint8_t    *chunk     = NULL;
    size_t            chunkLen  = 0;
    ScnPackage       *p;
    char             *text;
    uint64_t          id;
    uint64_t          author;

    UT_ASSERT_MSG(wsCopyMap("workshop_id.map", path, sizeof(path)),
                  "the map fixture could not be copied");
    m = (ScenarioManifest *)calloc(1, sizeof(*m));
    UT_ASSERT(m != NULL);
    m->api   = 1;
    m->bound = true;
    snprintf(m->name, sizeof(m->name), "Published");
    err[0] = '\0';
    if (!scnIoWriteMapChunk(path, m, kWsScript, strlen(kWsScript), err,
                            sizeof(err))) {
        free(m);
        UT_FAIL("the chunk could not be packed on: %s", err);
    }
    free(m);

    UT_ASSERT(wsReadWhole(path, &before, &beforeLen));
    if (!boloMapBodyLength(before, beforeLen, &body)) {
        free(before);
        UT_FAIL("the packed fixture does not measure as a map");
    }

    err[0] = '\0';
    if (!scnIoSetWorkshopId(path, WS_ID, WS_AUTHOR, err, sizeof(err))) {
        free(before);
        UT_FAIL("the map was not stamped: %s", err);
    }

    if (!wsReadWhole(path, &after, &afterLen)) {
        free(before);
        UT_FAIL("the stamped map could not be read back");
    }
    if (!scnPackageFindInMap(after, afterLen, &chunk, &chunkLen) ||
        (size_t)(chunk - after) != body ||
        memcmp(after, before, body) != 0) {
        free(before);
        free(after);
        UT_FAIL("the map data in front of the chunk changed");
    }
    free(before);
    p = scnPackageOpen(chunk, chunkLen, err, sizeof(err));
    if (p == NULL) {
        free(after);
        UT_FAIL("the stamped chunk will not open: %s", err);
    }
    text = wsManifest(p, &id, &author);
    free(text);
    if (id != WS_ID || author != WS_AUTHOR ||
        !wsEntryIs(p, SCN_PACKAGE_SCRIPT_ENTRY, kWsScript)) {
        scnPackageClose(p);
        free(after);
        UT_FAIL("the chunk read back as %llu / %llu, or its script changed",
                (unsigned long long)id, (unsigned long long)author);
    }
    scnPackageClose(p);
    free(after);
    return 0;
}

static int wsRefusals(void) {
    char     path[1024];
    char     err[512];
    uint8_t *before    = NULL;
    size_t   beforeLen = 0;
    bool     stamped;
    bool     same;

    /* A loose script has no manifest to put the id in. */
    UT_ASSERT(utScratchPath(path, sizeof(path), "workshop_id.lua"));
    UT_ASSERT(wsWriteWhole(path, kWsScript, strlen(kWsScript)));
    err[0]  = '\0';
    stamped = scnIoSetWorkshopId(path, WS_ID, WS_AUTHOR, err, sizeof(err));
    UT_ASSERT_MSG(!stamped, "a loose script was stamped");
    UT_ASSERT_MSG(strstr(err, "pack it first") != NULL,
                  "a loose script was refused with '%s'", err);
    UT_ASSERT_MSG(wsFileIs(path, (const uint8_t *)kWsScript,
                           strlen(kWsScript)),
                  "the refused script was changed");

    /* A map with nothing after it has no manifest either. */
    UT_ASSERT_MSG(wsCopyMap("workshop_id_plain.map", path, sizeof(path)),
                  "the map fixture could not be copied");
    UT_ASSERT(wsReadWhole(path, &before, &beforeLen));
    err[0]  = '\0';
    stamped = scnIoSetWorkshopId(path, WS_ID, WS_AUTHOR, err, sizeof(err));
    same    = wsFileIs(path, before, beforeLen);
    free(before);
    UT_ASSERT_MSG(!stamped, "a map with no chunk was stamped");
    UT_ASSERT_MSG(err[0] != '\0', "a map with no chunk was refused silently");
    UT_ASSERT_MSG(same, "the refused map was changed");
    return 0;
}

int run_scenario_io_set_workshop_id(void) {
    int rc;

    rc = wsOnPackage();
    if (rc != 0) {
        return rc;
    }
    rc = wsOnMap();
    if (rc != 0) {
        return rc;
    }
    return wsRefusals();
}
