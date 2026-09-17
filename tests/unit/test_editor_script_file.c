/*
 * The map editor's scenario script file: where a script sits beside a map,
 * and the read and write the pane's Save and Reload stand on.
 *
 * meScenarioScriptPathForMap mirrors scnScriptPath in
 * src/scenario/scenario_host.c, which is the rule the server follows when it
 * looks for a script beside a map. The editor cannot call that function — it
 * links neither scenario_static nor scenario_io_static — so the two are held
 * together by the derivation case below rather than by the compiler. A
 * script the editor writes has to be one the server finds.
 *
 * Each case names its own fixture files. CTest runs cases as separate
 * processes in one directory, so a shared fixture name is a race rather than
 * a fixture, and every file written here is removed again.
 *
 * Nothing here draws anything: the pane is C++ over ImGui and is the human
 * half of this work. This is the half that can be asserted.
 *
 * run_editor_script_path       — .map, .MAP and .Map all give up the
 *                                extension; a name without one keeps it all;
 *                                a buffer too small and an empty path are
 *                                refused
 * run_editor_script_round_trip — a script written through Save is the script
 *                                read back when the map is opened again
 * run_editor_script_missing    — a map with no script beside it opens empty,
 *                                which is not a failure
 * run_editor_script_over_cap   — a script too big to open is not written over
 *                                by the empty buffer standing in for it
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mapeditor_scenario.h"
#include "test_harness.h"

/* Writes a file, so a case can set up a script the editor did not write. */
static bool esPut(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(text, f);
    fclose(f);
    return true;
}

/* Writes a file of exactly len bytes, for a script past the size the editor
 * will open. */
static bool esPutSize(const char *path, size_t len) {
    char   chunk[4096];
    FILE  *f;
    size_t left = len;

    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    memset(chunk, '-', sizeof(chunk));
    while (left > 0) {
        size_t n = (left < sizeof(chunk)) ? left : sizeof(chunk);
        if (fwrite(chunk, 1, n, f) != n) {
            fclose(f);
            return false;
        }
        left -= n;
    }
    return fclose(f) == 0;
}

/* The length of a file, or -1 when it is not there. */
static long esSize(const char *path) {
    FILE *f = fopen(path, "rb");
    long  n;

    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    n = ftell(f);
    fclose(f);
    return n;
}

int run_editor_script_path(void) {
    char out[64];
    char small[8];

    /* A trailing .map goes, whatever its case, and the suffix takes its
     * place. */
    UT_ASSERT(meScenarioScriptPathForMap("maps/foo.map", out, sizeof(out)));
    UT_ASSERT_MSG(strcmp(out, "maps/foo.scenario.lua") == 0, "got '%s'", out);

    UT_ASSERT(meScenarioScriptPathForMap("maps/foo.MAP", out, sizeof(out)));
    UT_ASSERT_MSG(strcmp(out, "maps/foo.scenario.lua") == 0, "got '%s'", out);

    UT_ASSERT(meScenarioScriptPathForMap("maps/foo.Map", out, sizeof(out)));
    UT_ASSERT_MSG(strcmp(out, "maps/foo.scenario.lua") == 0, "got '%s'", out);

    /* A name that does not end in .map keeps the whole of it and takes the
     * suffix as it is, so a path with no extension still resolves. */
    UT_ASSERT(meScenarioScriptPathForMap("maps/foo", out, sizeof(out)));
    UT_ASSERT_MSG(strcmp(out, "maps/foo.scenario.lua") == 0, "got '%s'", out);

    UT_ASSERT(meScenarioScriptPathForMap("maps/foo.bmap", out, sizeof(out)));
    UT_ASSERT_MSG(strcmp(out, "maps/foo.bmap.scenario.lua") == 0, "got '%s'",
                  out);

    /* A path that is nothing but the extension gives all of it up. */
    UT_ASSERT(meScenarioScriptPathForMap(".map", out, sizeof(out)));
    UT_ASSERT_MSG(strcmp(out, ".scenario.lua") == 0, "got '%s'", out);

    /* No room for the result, and no path at all. */
    UT_ASSERT(!meScenarioScriptPathForMap("maps/foo.map", small, sizeof(small)));
    UT_ASSERT(!meScenarioScriptPathForMap("", out, sizeof(out)));

    return 0;
}

int run_editor_script_round_trip(void) {
    static const char kMap[] = "ut_editor_script_round_trip.map";
    static const char kScript[] =
        "local scenario = {}\n"
        "function scenario.on_round_start()\n"
        "  game.message(\"hello\")\n"
        "end\n"
        "return scenario\n";
    MEScenarioState st;
    char            expected[128];
    int             rc = 0;

    UT_ASSERT(meScenarioScriptPathForMap(kMap, expected, sizeof(expected)));
    /* A run that died before its cleanup must not decide this one. */
    remove(expected);

    meScenarioInit(&st);

    /* A map with no script yet, then a script typed into the pane. */
    meScenarioSetMap(&st, kMap);
    UT_ASSERT(!st.fileOnDisk);
    UT_ASSERT(!meScenarioDirty(&st));

    meScenarioSetText(&st, kScript, strlen(kScript));
    UT_ASSERT(meScenarioDirty(&st));

    if (!meScenarioSaveForMap(&st, kMap)) {
        meScenarioFree(&st);
        UT_FAIL("the script would not write to '%s'", expected);
    }
    /* Saved: no longer dirty, on disk, and pointed at the derived path. */
    UT_ASSERT(!meScenarioDirty(&st));
    UT_ASSERT(st.fileOnDisk);
    UT_ASSERT_MSG(strcmp(st.scriptPath, expected) == 0, "got '%s'",
                  st.scriptPath);

    /* Opening the map again reads back what was written, byte for byte. */
    meScenarioSetText(&st, "throw this away", 15);
    meScenarioSetMap(&st, kMap);
    if (st.script == NULL || st.scriptLen != strlen(kScript) ||
        memcmp(st.script, kScript, st.scriptLen) != 0) {
        rc = 1;
    }
    UT_ASSERT_MSG(rc == 0, "the script read back is not the script written");
    UT_ASSERT(!meScenarioDirty(&st));
    UT_ASSERT(st.fileOnDisk);
    /* The pane has to re-seed itself from the buffer it was just handed. */
    UT_ASSERT(st.pushToWidget);

    meScenarioFree(&st);
    remove(expected);
    return 0;
}

int run_editor_script_missing(void) {
    static const char kMap[] = "ut_editor_script_missing.map";
    static const char kOther[] = "ut_editor_script_missing_other.map";
    MEScenarioState   st;
    char              otherScript[128];
    char              mapScript[128];

    /* Neither fixture may be left over from an earlier run. */
    UT_ASSERT(meScenarioScriptPathForMap(kOther, otherScript,
                                         sizeof(otherScript)));
    UT_ASSERT(meScenarioScriptPathForMap(kMap, mapScript, sizeof(mapScript)));
    remove(otherScript);
    remove(mapScript);

    meScenarioInit(&st);

    /* No script beside this map: an empty buffer, and no fault reported. */
    meScenarioSetMap(&st, kMap);
    UT_ASSERT(!st.fileOnDisk);
    UT_ASSERT(!meScenarioDirty(&st));
    UT_ASSERT(st.scriptLen == 0);
    UT_ASSERT(st.script == NULL || st.script[0] == '\0');
    UT_ASSERT(st.scriptPath[0] != '\0');

    /* A map that does have one loads it, so the empty case above is the
     * absence of a file and not a read that never happens. */
    if (!esPut(otherScript, "-- a script\n")) {
        meScenarioFree(&st);
        UT_FAIL("could not write the fixture '%s'", otherScript);
    }
    meScenarioSetMap(&st, kOther);
    UT_ASSERT(st.fileOnDisk);
    UT_ASSERT(st.script != NULL);
    UT_ASSERT_MSG(strcmp(st.script, "-- a script\n") == 0, "got '%s'",
                  st.script);

    /* An empty map path clears the state rather than resolving to a bare
     * suffix: a map with no file has nowhere to keep a script. */
    meScenarioSetMap(&st, "");
    UT_ASSERT(st.scriptPath[0] == '\0');
    UT_ASSERT(!st.fileOnDisk);
    UT_ASSERT(st.scriptLen == 0);

    meScenarioFree(&st);
    remove(otherScript);
    return 0;
}

/* Every way out of the case below takes its fixtures with it, the failing
 * ways included: a 1 MB file left behind would be read by the next run. */
#define OC_CLEANUP()         \
    do {                     \
        meScenarioFree(&st); \
        remove(bigScript);   \
        remove(otherScript); \
    } while (0)

#define OC_ASSERT(cond)                            \
    do {                                           \
        if (!(cond)) {                             \
            OC_CLEANUP();                          \
            UT_FAIL("assertion failed: %s", #cond); \
        }                                          \
    } while (0)

#define OC_ASSERT_MSG(cond, fmt, ...)                             \
    do {                                                          \
        if (!(cond)) {                                            \
            OC_CLEANUP();                                         \
            UT_FAIL("%s — " fmt, #cond, ##__VA_ARGS__);           \
        }                                                         \
    } while (0)

int run_editor_script_over_cap(void) {
    static const char   kMap[] = "ut_editor_script_over_cap.map";
    static const char   kOther[] = "ut_editor_script_over_cap_other.map";
    static const char   kTyped[] = "-- replaced\n";
    /* One byte past the largest script the editor opens. */
    static const size_t kOverCap = (size_t)(1024 * 1024) + 1;
    MEScenarioState     st;
    char                bigScript[128];
    char                otherScript[128];

    UT_ASSERT(meScenarioScriptPathForMap(kMap, bigScript, sizeof(bigScript)));
    UT_ASSERT(
        meScenarioScriptPathForMap(kOther, otherScript, sizeof(otherScript)));
    /* A run that died before its cleanup must not decide this one. */
    remove(bigScript);
    remove(otherScript);

    meScenarioInit(&st);

    if (!esPutSize(bigScript, kOverCap)) {
        OC_CLEANUP();
        UT_FAIL("could not write the fixture '%s'", bigScript);
    }

    /* Too big to open: an empty buffer, but the file is there and is marked
     * as one that was not read. */
    meScenarioSetMap(&st, kMap);
    OC_ASSERT(st.scriptLen == 0);
    OC_ASSERT(st.script == NULL || st.script[0] == '\0');
    OC_ASSERT(st.fileOnDisk);
    OC_ASSERT(st.readRefused);

    /* Typing into that empty buffer and saving must not put it over the file
     * the editor would not show. */
    meScenarioSetText(&st, kTyped, strlen(kTyped));
    OC_ASSERT(meScenarioDirty(&st));
    OC_ASSERT(!meScenarioSaveForMap(&st, kMap));
    OC_ASSERT(st.status[0] != '\0');
    OC_ASSERT_MSG(esSize(bigScript) == (long)kOverCap,
                  "the script on disk is now %ld bytes", esSize(bigScript));
    /* Refused, so the edit is still unsaved and the file still unread. */
    OC_ASSERT(meScenarioDirty(&st));
    OC_ASSERT(st.readRefused);

    /* Under another name it is another file, so that save goes ahead. */
    OC_ASSERT(meScenarioSaveForMap(&st, kOther));
    OC_ASSERT(!meScenarioDirty(&st));
    OC_ASSERT(!st.readRefused);
    OC_ASSERT_MSG(strcmp(st.scriptPath, otherScript) == 0, "got '%s'",
                  st.scriptPath);
    OC_ASSERT_MSG(esSize(otherScript) == (long)strlen(kTyped),
                  "wrote %ld bytes", esSize(otherScript));
    /* And the one that would not open is still the length it was. */
    OC_ASSERT_MSG(esSize(bigScript) == (long)kOverCap,
                  "the script on disk is now %ld bytes", esSize(bigScript));

    OC_CLEANUP();
    return 0;
}

#undef OC_ASSERT_MSG
#undef OC_ASSERT
#undef OC_CLEANUP
