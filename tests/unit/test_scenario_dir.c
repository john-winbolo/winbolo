/*
 * The scenarios a server offers on their own, independently of any map.
 *
 * scnDirList reads a directory two ways. A .scenario is a WBSC container held
 * on its own, and its manifest.json is read straight out of it — scnPackageOpen
 * takes a buffer starting at the magic, which a package file does, so no Lua
 * runs to list one. A loose .lua is read through the validator's stub VM: the
 * chunk's top level runs once against a game table that answers nothing, and
 * the scenario table it declares is the manifest. Anything else is skipped.
 *
 * The list then travels as PACKET_LOBBY_SCENARIO_LIST_RSP, chunked the way the
 * map list is. The last case holds the production encoder's bytes against a
 * committed golden array and feeds the same bytes back through the production
 * accumulator, so a symmetric change to both halves still fails.
 *
 * Each case builds its own directory under a per-case name and removes it
 * afterwards: ctest runs the cases as separate processes in one directory, so
 * a shared fixture name is a race rather than a fixture.
 *
 * run_scenario_dir_lists_package       — a .scenario written by scnPackageWrite
 *                                        is listed with its manifest's name,
 *                                        description, cap and bot count
 * run_scenario_dir_lists_loose_script  — a loose .lua declaring a scenario
 *                                        table is listed with the same fields,
 *                                        derived through the stub VM
 * run_scenario_dir_skips_junk          — another extension, and a .lua that is
 *                                        no scenario, are both left out and the
 *                                        rest of the list is unaffected
 * run_scenario_dir_skips_subdirectory  — a .lua one directory down is not in
 *                                        the list, and no entry's file name
 *                                        carries a separator
 * run_scenario_dir_list_cached         — read through the lister the host
 *                                        registers, an unchanged directory is
 *                                        answered from the last result without
 *                                        booting a Lua state, and a file added
 *                                        to it makes the next read a fresh one
 * run_scenario_dir_list_cached_sees_edit — a file overwritten in place, which
 *                                        leaves the directory's time alone,
 *                                        still makes the next read a fresh one
 * run_scenario_dir_merges_shipped_mods — the same lister also reads the mods
 *                                        shipped beside the executable, and a
 *                                        name in both directories resolves to
 *                                        the player's file
 * run_scenario_dir_entry_roundtrip     — a list encoded into the RSP shape
 *                                        matches committed golden bytes and
 *                                        decodes back to the same entries,
 *                                        source and Workshop id included, and
 *                                        one whose description fills its
 *                                        length byte
 * run_scenario_dir_chunk_not_in_flight  — a final chunk delivered twice does
 *                                        not double the list, a chunk with
 *                                        nothing asked for is dropped, and a
 *                                        response's first chunk is what clears
 *                                        what the last one left
 *
 * Reads the ClientSim struct directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"        /* the scenario list accumulator */
#include "netpacks.h"                   /* PACKET_LOBBY_SCENARIO_LIST_RSP */
#include "server_sim_internal.h"        /* the console callback the cache case
                                         * watches for a VM boot */
#include "server_sim_scenario.h"        /* serverSimScenarioListDir — the read
                                         * that goes through the lister */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive, which is what
                                           * the console routes through */
#include "transport_udp.h"              /* udpClientHandleLobbyScenarioListRsp */
#include "transport_udp_internal.h"     /* PACKET_HEADER_SIZE, UDP_MAX_PAYLOAD */
#include "transport_udp_server_internal.h" /* udpServerPackScenarioListChunk */
#include "scenario_defs.h"              /* ScnDirEntry */
#include "scenario_dir.h"
#include "scenario_host.h"              /* scenarioHostRegisterScenarioLister */
#include "scenario_package.h"
#include "test_harness.h"

/* ── The directory ────────────────────────────────────────────────── */

static char sdDir[256];

/* Everything in path, and then path itself. One level down as well, because a
 * case builds a subdirectory in here. */
static void sdRemoveTree(const char *path) {
    char **names;
    int    count = 0;
    int    i;

    names = SDL_GlobDirectory(path, "*", 0, &count);
    if (names != NULL) {
        for (i = 0; i < count; i++) {
            char child[512];

            if (names[i] == NULL || names[i][0] == '\0') continue;
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
            if (remove(child) != 0) {
                /* A directory rather than a file: empty it and take it. */
                char **inner;
                int    innerCount = 0;
                int    j;

                inner = SDL_GlobDirectory(child, "*", 0, &innerCount);
                if (inner != NULL) {
                    for (j = 0; j < innerCount; j++) {
                        char grandchild[640];

                        if (inner[j] == NULL || inner[j][0] == '\0') continue;
                        snprintf(grandchild, sizeof(grandchild), "%s/%s", child,
                                 inner[j]);
                        remove(grandchild);
                    }
                    SDL_free(inner);
                }
                SDL_RemovePath(child);
            }
        }
        SDL_free(names);
    }
    SDL_RemovePath(path);
}

static bool sdMakeDir(const char *tag) {
    snprintf(sdDir, sizeof(sdDir), "wbtest_scenario_dir_%s", tag);
    /* Emptied rather than merely removed. A run that failed part way through
       never reaches its own cleanup, and SDL_RemovePath will not take a
       directory that still holds files — so without this a case that counts
       what it listed counts the previous run's leftovers too. */
    sdRemoveTree(sdDir);
    return SDL_CreateDirectory(sdDir);
}

/* One file in the directory. The cleanup takes whatever is in there rather
 * than a list of what was put there, so nothing is recorded here. */
static bool sdWrite(const char *name, const void *bytes, size_t len) {
    char  path[512];
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), "%s/%s", sdDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    ok = (len == 0) || (fwrite(bytes, 1, len, f) == len);
    fclose(f);
    return ok;
}

static bool sdWriteText(const char *name, const char *text) {
    return sdWrite(name, text, strlen(text));
}

static void sdCleanup(void) {
    sdRemoveTree(sdDir);
}

/* The entry with this file name, or NULL when the list does not hold one. */
static const ScnDirEntry *sdFind(const ScnDirEntry *list, int count,
                                 const char *file) {
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(list[i].file, file) == 0) return &list[i];
    }
    return NULL;
}

/* Every file name in the list on one line, so a failure says what was listed
 * rather than only what was missing. */
static void sdNames(const ScnDirEntry *list, int count, char *out,
                    size_t outLen) {
    int    i;
    size_t at = 0;

    out[0] = '\0';
    for (i = 0; i < count && at + 1 < outLen; i++) {
        at += (size_t)snprintf(out + at, outLen - at, "[%s] ", list[i].file);
    }
}

/* ── The two kinds of file ────────────────────────────────────────── */

/* A manifest naming two teams, so the bot count is a sum and not one team's
 * number. 4 + 2 = 6 seats, a cap of 6 humans, and not bound to any map. */
static const char kSdManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"api\": 1,\n"
    "  \"name\": \"Packed Raid\",\n"
    "  \"description\": \"Raiders from the north\",\n"
    "  \"bound\": false,\n"
    "  \"lobby\": {\n"
    "    \"max_players\": 6,\n"
    "    \"teams\": [\n"
    "      { \"id\": 2, \"bots\": 4 },\n"
    "      { \"id\": 3, \"bots\": 2 }\n"
    "    ]\n"
    "  },\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

static const char kSdPackedScript[] =
    "-- the script the container carries\n"
    "function on_setup() end\n";

/* A loose script declaring its own table: 2 + 3 = 5 seats and a cap of 4. */
static const char kSdLooseScript[] =
    "scenario = {\n"
    "  name = \"Loose Hold\",\n"
    "  description = \"Hold the keep\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  lobby = {\n"
    "    max_players = 4,\n"
    "    teams = {\n"
    "      { id = 1, bots = 2 },\n"
    "      { id = 2, bots = 3 },\n"
    "    },\n"
    "  },\n"
    "}\n";

/* Lua that runs and declares no scenario table, so it is no scenario. */
static const char kSdPlainScript[] =
    "local helper = {}\n"
    "function helper.add(a, b) return a + b end\n"
    "return helper\n";

/* One .scenario file, written the way -pack writes a container. */
static bool sdWritePackage(const char *name, const char *manifest,
                           const char *script) {
    ScnPackageEntry entries[2];
    uint8_t        *bytes = NULL;
    size_t          len   = 0;
    char            err[256];
    bool            ok;

    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)manifest;
    entries[0].len   = strlen(manifest);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)script;
    entries[1].len   = strlen(script);

    err[0] = '\0';
    if (!scnPackageWrite(entries, 2, &bytes, &len, err, sizeof(err))) {
        return false;
    }
    ok = sdWrite(name, bytes, len);
    free(bytes);
    return ok;
}

/* ── 1. A .scenario package ───────────────────────────────────────── */

int run_scenario_dir_lists_package(void) {
    ScnDirEntry        list[8];
    const ScnDirEntry *e;
    char               seen[512];
    int                n;

    UT_ASSERT(sdMakeDir("package"));
    UT_ASSERT_MSG(sdWritePackage("raid.scenario", kSdManifest,
                                 kSdPackedScript),
                  "the package fixture could not be written");

    n = scnDirList(sdDir, list, 8);
    sdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));
    UT_ASSERT_MSG(n == 1, "%d entries listed, expected 1: %s", n, seen);

    e = sdFind(list, n, "raid.scenario");
    UT_ASSERT_MSG(e != NULL, "the package is not in the list: %s", seen);
    UT_ASSERT_MSG(strcmp(e->name, "Packed Raid") == 0,
                  "name read as '%s'", e->name);
    UT_ASSERT_MSG(strcmp(e->description, "Raiders from the north") == 0,
                  "description read as '%s'", e->description);
    UT_ASSERT_MSG(e->maxPlayers == 6, "max_players read as %u",
                  (unsigned)e->maxPlayers);
    /* The sum over the teams, which is what says the count is not one team's
       own number. */
    UT_ASSERT_MSG(e->bots == 6, "bots read as %u, expected 4 + 2",
                  (unsigned)e->bots);
    UT_ASSERT_MSG(!e->bound, "a manifest saying bound false read as bound");

    sdCleanup();
    return 0;
}

/* ── 2. A loose script ────────────────────────────────────────────── */

int run_scenario_dir_lists_loose_script(void) {
    ScnDirEntry        list[8];
    const ScnDirEntry *e;
    char               seen[512];
    int                n;

    UT_ASSERT(sdMakeDir("loose"));
    UT_ASSERT(sdWriteText("hold.lua", kSdLooseScript));

    n = scnDirList(sdDir, list, 8);
    sdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));
    UT_ASSERT_MSG(n == 1, "%d entries listed, expected 1: %s", n, seen);

    e = sdFind(list, n, "hold.lua");
    UT_ASSERT_MSG(e != NULL, "the loose script is not in the list: %s", seen);
    UT_ASSERT_MSG(strcmp(e->name, "Loose Hold") == 0, "name read as '%s'",
                  e->name);
    UT_ASSERT_MSG(strcmp(e->description, "Hold the keep") == 0,
                  "description read as '%s'", e->description);
    UT_ASSERT_MSG(e->maxPlayers == 4, "max_players read as %u",
                  (unsigned)e->maxPlayers);
    UT_ASSERT_MSG(e->bots == 5, "bots read as %u, expected 2 + 3",
                  (unsigned)e->bots);
    UT_ASSERT_MSG(!e->bound, "a table saying bound false read as bound");

    sdCleanup();
    return 0;
}

/* ── 3. What is not a scenario ────────────────────────────────────── */

int run_scenario_dir_skips_junk(void) {
    ScnDirEntry list[8];
    char        seen[512];
    int         n;

    UT_ASSERT(sdMakeDir("junk"));
    /* Two that belong in the list and two that do not. */
    UT_ASSERT(sdWriteText("hold.lua", kSdLooseScript));
    UT_ASSERT(sdWritePackage("raid.scenario", kSdManifest, kSdPackedScript));
    UT_ASSERT(sdWriteText("notes.txt", "not a scenario, and not Lua either\n"));
    UT_ASSERT(sdWriteText("helper.lua", kSdPlainScript));

    n = scnDirList(sdDir, list, 8);
    sdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));
    UT_ASSERT_MSG(n == 2, "%d entries listed, expected 2: %s", n, seen);
    UT_ASSERT_MSG(sdFind(list, n, "hold.lua") != NULL,
                  "the loose script fell out of the list: %s", seen);
    UT_ASSERT_MSG(sdFind(list, n, "raid.scenario") != NULL,
                  "the package fell out of the list: %s", seen);
    UT_ASSERT_MSG(sdFind(list, n, "notes.txt") == NULL,
                  "a .txt was listed as a scenario: %s", seen);
    UT_ASSERT_MSG(sdFind(list, n, "helper.lua") == NULL,
                  "Lua that declares no scenario table was listed: %s", seen);

    /* And the two that were listed still carry what their files said, so the
       skipped ones cost the others nothing. */
    UT_ASSERT(strcmp(sdFind(list, n, "hold.lua")->name, "Loose Hold") == 0);
    UT_ASSERT(strcmp(sdFind(list, n, "raid.scenario")->name,
                     "Packed Raid") == 0);

    /* File-name order: "hold.lua" before "raid.scenario". */
    UT_ASSERT_MSG(strcmp(list[0].file, "hold.lua") == 0,
                  "the list is not in file-name order: %s", seen);

    sdCleanup();
    return 0;
}

/* ── 4. A file one directory down ─────────────────────────────────── */

/* SDL's match-everything walk descends into subdirectories and hands back what
 * it finds there as "sub/x.lua". ScnDirEntry.file is a name in the scenarios
 * directory and never a path, so such a file is not one this list offers. */
int run_scenario_dir_skips_subdirectory(void) {
    ScnDirEntry list[8];
    char        seen[512];
    char        subDir[512];
    char        subFile[640];
    FILE       *f;
    int         n;
    int         i;

    UT_ASSERT(sdMakeDir("subdir"));
    UT_ASSERT(sdWriteText("hold.lua", kSdLooseScript));

    snprintf(subDir, sizeof(subDir), "%s/sub", sdDir);
    UT_ASSERT_MSG(SDL_CreateDirectory(subDir),
                  "the subdirectory fixture could not be made: %s",
                  SDL_GetError());
    snprintf(subFile, sizeof(subFile), "%s/buried.lua", subDir);
    f = fopen(subFile, "wb");
    UT_ASSERT_MSG(f != NULL, "the buried script could not be written");
    fputs(kSdLooseScript, f);
    fclose(f);

    n = scnDirList(sdDir, list, 8);
    sdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));
    UT_ASSERT_MSG(n == 1,
                  "%d entries listed, expected the one file in the directory "
                  "itself: %s", n, seen);
    UT_ASSERT_MSG(sdFind(list, n, "hold.lua") != NULL,
                  "the loose script fell out of the list: %s", seen);
    UT_ASSERT_MSG(sdFind(list, n, "buried.lua") == NULL,
                  "a script one directory down was listed: %s", seen);

    /* And not under a path either, which is the shape it would arrive in. */
    for (i = 0; i < n; i++) {
        UT_ASSERT_MSG(strchr(list[i].file, '/') == NULL &&
                      strchr(list[i].file, '\\') == NULL,
                      "entry %d is \"%s\", which is a path and not a name in "
                      "the directory", i, list[i].file);
    }

    sdCleanup();
    return 0;
}

/* ── 5. The second read of a directory nothing moved ──────────────── */

/* A mark the script prints from its top level. The listing runs that top level
 * in a Lua state of its own, so the mark on the console is the VM boot: a read
 * that produces it did the work, and one that does not was answered from what
 * the last read left behind. */
#define SD_MARK "scenario-dir:ran"

static const char kSdMarkedScript[] =
    "print(\"" SD_MARK "\")\n"
    "scenario = { name = \"Marked\", api = 1 }\n";

static void (*sdConsolePrev)(void *ctx, char *msg) = NULL;
static char sdSaid[8192];

static void sdConsoleCb(void *ctx, char *msg) {
    size_t have;
    size_t room;
    size_t n;

    if (sdConsolePrev != NULL) {
        sdConsolePrev(ctx, msg);
    }
    if (msg == NULL) {
        return;
    }
    have = strlen(sdSaid);
    room = sizeof(sdSaid) - 1 - have;
    n    = strlen(msg);
    if (n > room) {
        n = room;
    }
    memcpy(sdSaid + have, msg, n);
    sdSaid[have + n] = '\0';
}

/* Only consoleMessage is replaced, never the ctx beside it, which the sim's
   other callbacks read. */
static void sdWatchConsole(ServerSim *sim) {
    sdSaid[0]     = '\0';
    sdConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = sdConsoleCb;
}

static void sdUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = sdConsolePrev;
    sdConsolePrev = NULL;
}

/* Touch the scenarios directory until its modify time reads something other
 * than was, and answer whether it did.
 *
 * A directory's modify time comes from a coarse kernel clock — around four
 * milliseconds on the filesystem these tests run from — so a file created
 * inside the same tick as the read that cached the listing leaves the stamp
 * where it was. An operator dropping a scenario in never lands inside that
 * window; a case that adds one microseconds after reading does, every other
 * run. So it waits for the stamp before asserting on what the stamp decides.
 *
 * The touch file is created and removed inside one pass, so no listing ever
 * sees it, and its name is not one the list would offer anyway. */
static bool sdWaitForDirChange(SDL_Time was) {
    char scratch[512];
    int  i;

    snprintf(scratch, sizeof(scratch), "%s/touch.tmp", sdDir);
    for (i = 0; i < 1000; i++) {
        SDL_PathInfo info;
        FILE        *f;

        if (SDL_GetPathInfo(sdDir, &info) && info.modify_time != was) {
            return true;
        }
        f = fopen(scratch, "wb");
        if (f != NULL) {
            fclose(f);
            remove(scratch);
        }
        SDL_Delay(1);
    }
    return false;
}

int run_scenario_dir_list_cached(void) {
    ScnDirEntry  list[8];
    ServerSim   *sim;
    SDL_PathInfo cached;
    int          n;

    UT_ASSERT(sdMakeDir("cached"));
    UT_ASSERT(sdWriteText("marked.lua", kSdMarkedScript));

    sim = ut_make_running_sim("Host");
    UT_ASSERT(sim != NULL);
    /* serverSimConsoleMessage writes through the active sim's callback, and
       what the script prints is what this case reads. */
    serverSimSetActive(sim);
    serverSimSetScenarioDir(sim, sdDir);
    /* Through the lister the host registers, which is where the cache is —
       scnDirList on its own reads the directory every time it is called. */
    scenarioHostRegisterScenarioLister(sim);
    sdWatchConsole(sim);

    /* Nothing touches the directory between here and the read below, so this
       is the stamp the cache is about to keep. */
    UT_ASSERT(SDL_GetPathInfo(sdDir, &cached));

    /* The lister merges the shipped mods directory in behind the player's
       own, so what comes back is this directory's files and whatever is
       installed beside the executable. The cases below therefore ask whether
       a file is in the list rather than how long the list is: an installed
       mod appearing or going would otherwise fail a case about caching. */
    n = serverSimScenarioListDir(sim, list, 8);
    UT_ASSERT_MSG(sdFind(list, n, "marked.lua") != NULL,
                  "the first read did not list the file in the directory");
    UT_ASSERT_MSG(strcmp(sdFind(list, n, "marked.lua")->name, "Marked") == 0,
                  "the first read named it '%s'",
                  sdFind(list, n, "marked.lua")->name);
    UT_ASSERT_MSG(strstr(sdSaid, SD_MARK) != NULL,
                  "the first read never ran the script's top level, so this "
                  "case cannot tell a fresh read from a cached one");

    /* Again, with nothing in the directory moved. */
    sdSaid[0] = '\0';
    memset(list, 0, sizeof(list));
    n = serverSimScenarioListDir(sim, list, 8);
    UT_ASSERT_MSG(sdFind(list, n, "marked.lua") != NULL,
                  "the second read did not list the file in the directory");
    UT_ASSERT_MSG(strcmp(sdFind(list, n, "marked.lua")->name, "Marked") == 0,
                  "the second read named it '%s', so the rows it handed back "
                  "are not the ones the first read found",
                  sdFind(list, n, "marked.lua")->name);
    UT_ASSERT_MSG(strstr(sdSaid, SD_MARK) == NULL,
                  "the second read booted a Lua state and ran the script "
                  "again: the listing was not answered from the last one");

    /* A file added moves the directory's own modify time, which is what the
       cache is keyed to, so the read after it is a fresh one. */
    sdSaid[0] = '\0';
    UT_ASSERT(sdWriteText("hold.lua", kSdLooseScript));
    UT_ASSERT_MSG(sdWaitForDirChange(cached.modify_time),
                  "the directory's modify time never moved off the one the "
                  "first read kept, so there is nothing here to invalidate "
                  "the cache with");
    n = serverSimScenarioListDir(sim, list, 8);
    UT_ASSERT_MSG(sdFind(list, n, "hold.lua") != NULL,
                  "the file that was added is not in the list, so the cache "
                  "was not invalidated");
    UT_ASSERT_MSG(sdFind(list, n, "marked.lua") != NULL,
                  "the file that was already there fell out of the list");
    UT_ASSERT_MSG(strstr(sdSaid, SD_MARK) != NULL,
                  "a directory that gained a file was still answered from the "
                  "cache");

    sdUnwatchConsole(sim);
    serverSimDestroy(sim);
    sdCleanup();
    return 0;
}

/* The same script with another name, and a different length, so an overwrite
 * moves the file's size even when its modify time lands in the same tick. */
static const char kSdRemarkedScript[] =
    "print(\"" SD_MARK "\")\n"
    "scenario = { name = \"Remarked\", api = 1 }\n";

/* A file overwritten in place leaves the directory's modify time alone, and
 * the cache also stamps each scenario file, so the read after the overwrite
 * is a fresh one and carries the file's new name. */
int run_scenario_dir_list_cached_sees_edit(void) {
    ScnDirEntry  list[8];
    ServerSim   *sim;
    SDL_PathInfo before;
    SDL_PathInfo after;
    int          n;

    UT_ASSERT(sdMakeDir("cached_edit"));
    UT_ASSERT(sdWriteText("marked.lua", kSdMarkedScript));

    sim = ut_make_running_sim("Host");
    UT_ASSERT(sim != NULL);
    serverSimSetActive(sim);
    serverSimSetScenarioDir(sim, sdDir);
    scenarioHostRegisterScenarioLister(sim);
    sdWatchConsole(sim);

    n = serverSimScenarioListDir(sim, list, 8);
    UT_ASSERT_MSG(sdFind(list, n, "marked.lua") != NULL,
                  "the first read did not list the file in the directory");

    /* Once more with nothing changed, so the cache is known to be holding
       the listing before the file is overwritten. */
    sdSaid[0] = '\0';
    (void)serverSimScenarioListDir(sim, list, 8);
    UT_ASSERT_MSG(strstr(sdSaid, SD_MARK) == NULL,
                  "the second read was not answered from the cache, so this "
                  "case cannot tell whether the overwrite invalidated it");

    sdSaid[0] = '\0';
    UT_ASSERT(SDL_GetPathInfo(sdDir, &before));
    UT_ASSERT(sdWriteText("marked.lua", kSdRemarkedScript));
    UT_ASSERT(SDL_GetPathInfo(sdDir, &after));
    UT_ASSERT_MSG(before.modify_time == after.modify_time,
                  "this filesystem moved the directory time, so this case "
                  "did not exercise the file stamps");
    /* A read inside SCN_DIR_STAMPS_REUSE_MS of the last walk is answered
       without stamping the files again, so wait it out: this case is about
       the stamps, not the window. */
    SDL_Delay(SCN_DIR_STAMPS_REUSE_MS + 50);
    memset(list, 0, sizeof(list));
    n = serverSimScenarioListDir(sim, list, 8);
    UT_ASSERT_MSG(sdFind(list, n, "marked.lua") != NULL,
                  "the overwritten file fell out of the list");
    UT_ASSERT_MSG(strcmp(sdFind(list, n, "marked.lua")->name, "Remarked") ==
                      0,
                  "the read after the overwrite named it '%s': the cache "
                  "kept the row from before the file changed",
                  sdFind(list, n, "marked.lua")->name);
    UT_ASSERT_MSG(strstr(sdSaid, SD_MARK) != NULL,
                  "a file overwritten in place was still answered from the "
                  "cache");

    sdUnwatchConsole(sim);
    serverSimDestroy(sim);
    sdCleanup();
    return 0;
}

/* ── 6. The player's directory and the shipped one, merged ───────── */

/* The lister the host registers reads two directories: the one the player
 * drops files into, and the mods shipped beside the executable. Both are
 * offered as one list, and a name in both resolves to the player's file —
 * that is how a player replaces a shipped mod with their own edit of it.
 *
 * The shipped directory is found here the same way the host finds it, from
 * SDL_GetBasePath, so a change to where the host looks fails this case rather
 * than quietly listing nothing from the second directory.
 */
/* Room for both directories at once. The other cases here list one small
 * fixture directory and take 8; this one also carries whatever the build
 * staged under data/mods. */
#define SD_MERGE_MAX 32

int run_scenario_dir_merges_shipped_mods(void) {
    ScnDirEntry  shipped[SD_MERGE_MAX];
    ScnDirEntry  list[SD_MERGE_MAX];
    char         shippedDir[512];
    char         workshopDir[512];
    const char  *base;
    ServerSim   *sim;
    char         seen[1024];
    int          shippedCount;
    int          n;
    int          i;

    base = SDL_GetBasePath();
    UT_ASSERT_MSG(base != NULL,
                  "SDL cannot say where the executable is, so this case "
                  "cannot find the directory the host would read");
    snprintf(shippedDir, sizeof(shippedDir), "%sdata/mods", base);
    shippedCount = scnDirList(shippedDir, shipped, SD_MERGE_MAX);
    if (shippedCount < 0) shippedCount = 0;
    UT_ASSERT_MSG(shippedCount > 0,
                  "no scenarios under \"%s\": the build did not stage "
                  "data/mods, so there is nothing to merge and this case "
                  "would pass without testing anything", shippedDir);

    UT_ASSERT(sdMakeDir("merge"));
    /* One file only the player has, and one under a shipped mod's own name. */
    UT_ASSERT(sdWriteText("hold.lua", kSdLooseScript));
    UT_ASSERT(sdWriteText(shipped[0].file, kSdLooseScript));

    sim = ut_make_running_sim("Host");
    UT_ASSERT(sim != NULL);
    serverSimSetActive(sim);
    serverSimSetScenarioDir(sim, sdDir);
    scenarioHostRegisterScenarioLister(sim);

    /* The player's own directory is one of the three a listing reads, and on
       a developer's machine it is a real one in their home directory.
       Pointed at this case's own directory, which the list then holds once,
       so what is listed is the two directories this case wrote and nothing
       the machine happens to hold. The third directory is tested on its own
       in test_scenario_mod_dirs.c. The Workshop directory sits beside the
       player's own and is pointed at an empty one of this case's for the
       same reason. */
    UT_ASSERT(utScratchPath(workshopDir, sizeof(workshopDir), "Workshop"));
    UT_ASSERT(SDL_CreateDirectory(workshopDir));
#ifdef _WIN32
    _putenv_s("WB_MOD_DIR_USER", sdDir);
    _putenv_s("WB_MOD_DIR_WORKSHOP", workshopDir);
#else
    setenv("WB_MOD_DIR_USER", sdDir, 1);
    setenv("WB_MOD_DIR_WORKSHOP", workshopDir, 1);
#endif
    n = serverSimScenarioListDir(sim, list, SD_MERGE_MAX);
#ifdef _WIN32
    _putenv_s("WB_MOD_DIR_USER", "");
    _putenv_s("WB_MOD_DIR_WORKSHOP", "");
#else
    unsetenv("WB_MOD_DIR_USER");
    unsetenv("WB_MOD_DIR_WORKSHOP");
#endif
    sdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));
    UT_ASSERT_MSG(sdFind(list, n, "hold.lua") != NULL,
                  "the player's own file fell out of the merged list: %s",
                  seen);
    for (i = 0; i < shippedCount; i++) {
        UT_ASSERT_MSG(sdFind(list, n, shipped[i].file) != NULL,
                      "the shipped mod \"%s\" is not in the merged list: %s",
                      shipped[i].file, seen);
    }

    /* The colliding name appears once, and it is the player's file. A merge
       that appended both would offer the same name twice and the lobby would
       pick whichever it reached first. */
    {
        int hits = 0;
        for (i = 0; i < n; i++) {
            if (strcmp(list[i].file, shipped[0].file) == 0) hits++;
        }
        UT_ASSERT_MSG(hits == 1,
                      "\"%s\" is in the merged list %d times: %s",
                      shipped[0].file, hits, seen);
    }
    UT_ASSERT_MSG(strcmp(sdFind(list, n, shipped[0].file)->name,
                         "Loose Hold") == 0,
                  "the name in both directories resolved to \"%s\", so the "
                  "shipped copy won over the player's",
                  sdFind(list, n, shipped[0].file)->name);

    /* And the merged list is in file-name order, not player-then-shipped: a
       chooser draws it in the order it is handed. */
    for (i = 1; i < n; i++) {
        UT_ASSERT_MSG(SDL_strcasecmp(list[i - 1].file, list[i].file) <= 0,
                      "the merged list is not in file-name order: %s", seen);
    }

    serverSimDestroy(sim);
    sdCleanup();
    return 0;
}

/* ── 7. The chunk, byte for byte and back ─────────────────────────── */

/* The two entries the golden bytes below describe. The first says a player
 * uploaded it and names a Workshop item whose eight bytes all differ, so a
 * byte written in the wrong place or order shows. */
#define SD_E0_FILE "alpha.scenario"
#define SD_E0_NAME "Alpha"
#define SD_E0_DESC "First"
#define SD_E1_FILE "beta.lua"
#define SD_E1_NAME "Beta"
#define SD_E1_DESC "Second"
#define SD_E0_WORKSHOP_ID 0x0102030405060708ull

/* What udpServerPackScenarioListChunk must write after the 8-byte packet
 * header for those two, in wire order:
 *   [final][count] then per entry
 *   [fileLen][file][nameLen][name][descLen][desc][maxPlayers][bots][bound]
 *   [keepsWinCondition][source][workshopId 8, most significant first]
 *
 * Committed rather than computed: a round trip passes even when both halves
 * change together, and these bytes are what catches a wire change nobody
 * meant. The packet header is another layer's and is not pinned here. */
static const uint8_t kSdGolden[] = {
    0x01, 0x02,                             /* final = 1, count = 2      */
    0x0E, 'a','l','p','h','a','.','s','c','e','n','a','r','i','o',
    0x05, 'A','l','p','h','a',
    0x05, 'F','i','r','s','t',
    0x08, 0x03, 0x00, 0x01,   /* maxPlayers, bots, bound, keepsWinCondition */
    0x01,                                   /* source = upload           */
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,   /* workshopId      */
    0x08, 'b','e','t','a','.','l','u','a',
    0x04, 'B','e','t','a',
    0x06, 'S','e','c','o','n','d',
    0x00, 0x00, 0x01, 0x00,   /* maxPlayers, bots, bound, keepsWinCondition */
    0x00,                                   /* source = server           */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00    /* workshopId = 0  */
};

static void sdFill(ScnDirEntry *e, const char *file, const char *name,
                   const char *desc, uint8_t maxPlayers, uint8_t bots,
                   bool bound, bool keepsWinCondition) {
    memset(e, 0, sizeof(*e));
    SDL_strlcpy(e->file, file, sizeof(e->file));
    SDL_strlcpy(e->name, name, sizeof(e->name));
    SDL_strlcpy(e->description, desc, sizeof(e->description));
    e->maxPlayers = maxPlayers;
    e->bots       = bots;
    e->bound      = bound;
    e->keepsWinCondition = keepsWinCondition;
}

/* A client with a scenario-list request in flight, which is what the
 * accumulator needs before it will take a chunk at all: the response carries
 * nothing to recognise a stale chunk by, so having asked is what makes a
 * chunk this client's. A case that wants the other state clears the flag for
 * itself. */
static ClientSim *sdFreshClientSim(void) {
    ClientSim *cs = clientSimAlloc();

    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    cs->lobbyScenarioListInFlight = true;
    cs->lobbyScenarioListStarted  = false;
    return cs;
}

int run_scenario_dir_entry_roundtrip(void) {
    ScnDirEntry entries[3];
    uint8_t     buf[UDP_MAX_PAYLOAD];
    char        longDesc[SCN_DIR_DESC_LEN];
    ClientSim  *cs;
    int         len;
    int         next = 0;
    int         i;

    sdFill(&entries[0], SD_E0_FILE, SD_E0_NAME, SD_E0_DESC, 8, 3, false, true);
    entries[0].source     = SCN_DIR_SOURCE_UPLOAD;
    entries[0].workshopId = SD_E0_WORKSHOP_ID;
    sdFill(&entries[1], SD_E1_FILE, SD_E1_NAME, SD_E1_DESC, 0, 0, true, false);

    /* ── The golden bytes ─────────────────────────────────────────── */
    memset(buf, 0, sizeof(buf));
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), entries, 2, 0,
                                         &next);
    UT_ASSERT_MSG(len == PACKET_HEADER_SIZE + (int)sizeof(kSdGolden),
                  "the chunk is %d bytes, expected %d", len,
                  PACKET_HEADER_SIZE + (int)sizeof(kSdGolden));
    UT_ASSERT_MSG(next == 2, "the encoder stopped at entry %d, expected 2",
                  next);
    for (i = 0; i < (int)sizeof(kSdGolden); i++) {
        UT_ASSERT_MSG(buf[PACKET_HEADER_SIZE + i] == kSdGolden[i],
                      "byte %d of the payload is 0x%02X, expected 0x%02X",
                      i, (unsigned)buf[PACKET_HEADER_SIZE + i],
                      (unsigned)kSdGolden[i]);
    }

    /* ── And back through the client's accumulator ────────────────── */
    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);

    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 2,
                  "the two-entry chunk read as %d entries",
                  cs->lobbyScenarioListCount);
    UT_ASSERT_MSG(cs->lobbyScenarioListReady,
                  "a final chunk did not make the list ready");
    UT_ASSERT_MSG(!cs->lobbyScenarioListInFlight,
                  "a final chunk left the request in flight");
    UT_ASSERT(strcmp(cs->lobbyScenarioListFiles[0], SD_E0_FILE) == 0);
    UT_ASSERT(strcmp(cs->lobbyScenarioListNames[0], SD_E0_NAME) == 0);
    UT_ASSERT(strcmp(cs->lobbyScenarioListDescs[0], SD_E0_DESC) == 0);
    UT_ASSERT_MSG(cs->lobbyScenarioListMaxPlayers[0] == 8,
                  "entry 0's max_players read as %u",
                  (unsigned)cs->lobbyScenarioListMaxPlayers[0]);
    UT_ASSERT_MSG(cs->lobbyScenarioListBots[0] == 3,
                  "entry 0's bots read as %u",
                  (unsigned)cs->lobbyScenarioListBots[0]);
    UT_ASSERT_MSG(!cs->lobbyScenarioListBound[0],
                  "entry 0 read as bound");
    UT_ASSERT_MSG(cs->lobbyScenarioListKeepsWin[0],
                  "entry 0 read as a scenario, and the fixture says a mod");
    UT_ASSERT(strcmp(cs->lobbyScenarioListFiles[1], SD_E1_FILE) == 0);
    UT_ASSERT(strcmp(cs->lobbyScenarioListNames[1], SD_E1_NAME) == 0);
    UT_ASSERT(strcmp(cs->lobbyScenarioListDescs[1], SD_E1_DESC) == 0);
    UT_ASSERT_MSG(cs->lobbyScenarioListBound[1],
                  "entry 1 read as unbound, and the fixture says bound");
    UT_ASSERT_MSG(!cs->lobbyScenarioListKeepsWin[1],
                  "entry 1 read as a mod, and the fixture says a scenario");
    UT_ASSERT_MSG(clientSimGetLobbyScenarioListSource(cs, 0) ==
                      SCN_DIR_SOURCE_UPLOAD,
                  "entry 0's source read as %u",
                  (unsigned)clientSimGetLobbyScenarioListSource(cs, 0));
    UT_ASSERT_MSG(clientSimGetLobbyScenarioListWorkshopId(cs, 0) ==
                      SD_E0_WORKSHOP_ID,
                  "entry 0's Workshop id read as 0x%016llX",
                  (unsigned long long)
                      clientSimGetLobbyScenarioListWorkshopId(cs, 0));
    UT_ASSERT(clientSimGetLobbyScenarioListSource(cs, 1) ==
              SCN_DIR_SOURCE_SERVER);
    UT_ASSERT(clientSimGetLobbyScenarioListWorkshopId(cs, 1) == 0);
    /* Out of range answers the server's own and no item. */
    UT_ASSERT(clientSimGetLobbyScenarioListSource(cs, 2) ==
              SCN_DIR_SOURCE_SERVER);
    UT_ASSERT(clientSimGetLobbyScenarioListWorkshopId(cs, -1) == 0);
    /* Read back through the public accessors too, which is how a chooser will
       see it. */
    UT_ASSERT(strcmp(clientSimGetLobbyScenarioListFile(cs, 0),
                     SD_E0_FILE) == 0);
    UT_ASSERT(clientSimGetLobbyScenarioListBots(cs, 0) == 3);
    /* The two flags answer different questions, which is why both are on the
       wire: entry 0 is unbound and a mod, entry 1 is bound and a scenario. */
    UT_ASSERT(clientSimGetLobbyScenarioListKeepsWinCondition(cs, 0));
    UT_ASSERT(!clientSimGetLobbyScenarioListBound(cs, 0));
    UT_ASSERT(!clientSimGetLobbyScenarioListKeepsWinCondition(cs, 1));
    UT_ASSERT(clientSimGetLobbyScenarioListBound(cs, 1));
    UT_ASSERT(!clientSimGetLobbyScenarioListKeepsWinCondition(cs, 2));
    UT_ASSERT(clientSimGetLobbyScenarioListCount(cs) == 2);
    UT_ASSERT(clientSimGetLobbyScenarioListReady(cs));
    /* An index off the end answers rather than reading past the list. */
    UT_ASSERT(clientSimGetLobbyScenarioListFile(cs, 2)[0] == '\0');
    UT_ASSERT(clientSimGetLobbyScenarioListFile(cs, -1)[0] == '\0');
    clientSimDestroy(cs);

    /* ── A description filling its length byte ────────────────────── */
    /* SCN_DIR_DESC_LEN is 256, so the longest description a scenario can
       hold is 255 bytes — exactly what one length byte carries. This is that
       boundary: the whole of it has to survive the trip. The encoder's cut to
       255 is the defence if that constant ever grows past what the byte can
       say, and it cannot be reached from here. */
    memset(longDesc, 'x', sizeof(longDesc) - 1);
    longDesc[sizeof(longDesc) - 1] = '\0';
    UT_ASSERT_MSG(strlen(longDesc) == 255,
                  "the fixture description is %d bytes, expected 255",
                  (int)strlen(longDesc));
    sdFill(&entries[2], "wide.lua", "Wide", longDesc, 16, 255, false, false);

    memset(buf, 0, sizeof(buf));
    next = 0;
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), &entries[2], 1,
                                         0, &next);
    UT_ASSERT(next == 1);
    UT_ASSERT_MSG(buf[PACKET_HEADER_SIZE + 1] == 1,
                  "the one-entry chunk says count %u",
                  (unsigned)buf[PACKET_HEADER_SIZE + 1]);

    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 1,
                  "the wide entry read as %d entries",
                  cs->lobbyScenarioListCount);
    UT_ASSERT_MSG(strlen(cs->lobbyScenarioListDescs[0]) == 255,
                  "the 255-byte description arrived as %d bytes",
                  (int)strlen(cs->lobbyScenarioListDescs[0]));
    UT_ASSERT(strcmp(cs->lobbyScenarioListDescs[0], longDesc) == 0);
    UT_ASSERT(cs->lobbyScenarioListBots[0] == 255);
    clientSimDestroy(cs);

    /* ── An empty list is one chunk that says so ──────────────────── */
    memset(buf, 0, sizeof(buf));
    next = -1;
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), entries, 0, 0,
                                         &next);
    UT_ASSERT_MSG(len == PACKET_HEADER_SIZE + 2,
                  "an empty chunk is %d bytes, expected %d", len,
                  PACKET_HEADER_SIZE + 2);
    UT_ASSERT_MSG(buf[PACKET_HEADER_SIZE] == 1,
                  "an empty chunk did not set final");
    UT_ASSERT_MSG(buf[PACKET_HEADER_SIZE + 1] == 0,
                  "an empty chunk says count %u",
                  (unsigned)buf[PACKET_HEADER_SIZE + 1]);
    UT_ASSERT(next == 0);

    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT(cs->lobbyScenarioListCount == 0);
    UT_ASSERT_MSG(cs->lobbyScenarioListReady,
                  "an empty list never became ready");
    UT_ASSERT(!cs->lobbyScenarioListInFlight);
    clientSimDestroy(cs);

    /* ── A chunk cut short keeps the entry out ────────────────────── */
    memset(buf, 0, sizeof(buf));
    next = 0;
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), entries, 2, 0,
                                         &next);
    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len - 1);
    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 1,
                  "a chunk one byte short read as %d entries, expected the "
                  "first alone", cs->lobbyScenarioListCount);
    clientSimDestroy(cs);
    return 0;
}

/* ── What the accumulator does with a chunk it did not ask for ────── */

/* The response has no path in it to tell a stale chunk from a current one —
 * the directory is flat, so there is nothing to ask about — which leaves the
 * request being in flight as the whole of what makes a chunk this client's.
 * Two things followed from not testing it: a chunk delivered twice appended
 * its rows twice, and a chunk arriving with nothing asked for was taken.
 *
 * Both are driven here through the production encoder and the production
 * accumulator, as the round trip above is. */
int run_scenario_dir_chunk_not_in_flight(void) {
    ScnDirEntry entries[2];
    uint8_t     buf[UDP_MAX_PAYLOAD];
    ClientSim  *cs;
    int         len;
    int         next = 0;

    sdFill(&entries[0], SD_E0_FILE, SD_E0_NAME, SD_E0_DESC, 8, 3, false, true);
    sdFill(&entries[1], SD_E1_FILE, SD_E1_NAME, SD_E1_DESC, 0, 0, true, false);

    memset(buf, 0, sizeof(buf));
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), entries, 2, 0,
                                         &next);
    UT_ASSERT(next == 2);

    /* ── A final chunk delivered twice ────────────────────────────── */
    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 2,
                  "the first copy read as %d entries",
                  cs->lobbyScenarioListCount);
    UT_ASSERT(!cs->lobbyScenarioListInFlight);

    /* The same bytes again — a retransmit, or a duplicate off the wire. The
       final flag took the request out of flight, so the second copy is
       dropped rather than doubling the list. */
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 2,
                  "a repeated final chunk left %d entries, wanted the two the "
                  "response carried", cs->lobbyScenarioListCount);
    UT_ASSERT_MSG(strcmp(cs->lobbyScenarioListFiles[0], SD_E0_FILE) == 0,
                  "the repeated chunk rewrote the first row as \"%s\"",
                  cs->lobbyScenarioListFiles[0]);
    clientSimDestroy(cs);

    /* ── A chunk with nothing asked for ───────────────────────────── */
    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    cs->lobbyScenarioListInFlight = false;
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 0,
                  "a chunk arriving with no request in flight read as %d "
                  "entries", cs->lobbyScenarioListCount);
    UT_ASSERT_MSG(!cs->lobbyScenarioListReady,
                  "a chunk arriving with no request in flight made the list "
                  "ready");
    clientSimDestroy(cs);

    /* ── The first chunk of a response clears what the last left ──── */
    /* Two responses in a row, the second carrying one entry. The accumulator
       is emptied by that response's first chunk rather than where the request
       was sent, so the second listing is its own rather than the first with
       one more row on the end. */
    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT(cs->lobbyScenarioListCount == 2);

    /* The chooser asks again: in flight, and nothing of this response seen
       yet — which is the state transportUdpClientSendLobbyScenarioListRequest
       leaves behind. The rows from last time are still there. */
    cs->lobbyScenarioListInFlight = true;
    cs->lobbyScenarioListStarted  = false;
    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 2,
                  "asking again emptied the list before the answer came");

    memset(buf, 0, sizeof(buf));
    next = 0;
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), entries, 1, 0,
                                         &next);
    UT_ASSERT(next == 1);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 1,
                  "the second response read as %d entries, wanted the one it "
                  "carried", cs->lobbyScenarioListCount);
    clientSimDestroy(cs);
    return 0;
}
