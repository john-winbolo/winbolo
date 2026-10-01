/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The three directories a mod can come from, and what one listing makes of
 * them.
 *
 * scnModDirs builds the list: the directory this host was given, then the
 * player's own under SDL_GetPrefPath, then the mods that ship beside the
 * executable. A listing reads each in turn and keeps the first copy of a
 * file name it meets, so the order of that list is the precedence, and the
 * rows come back in file-name order however many directories made them.
 *
 * The middle and the last are SDL's answers in a build, which a case cannot
 * write to: the preferences directory is the home directory of whoever ran
 * the test, and the shipped one is inside a signed bundle. WB_MOD_DIR_USER
 * and WB_MOD_DIR_SHIPPED name them instead, and every case here points them
 * at directories of its own under the scratch path.
 *
 * The Workshop directory sits between the player's own and the shipped one,
 * and is also under the preferences directory: WB_MOD_DIR_WORKSHOP names it
 * in every case, empty unless the case is about it. The last cases are about
 * it: its place in the precedence, the rows it is marked on, this computer's
 * own listing, and an upload under a name it holds.
 *
 * The seams are set for the length of the listing call and taken away again
 * before anything is asserted, so a case that fails leaves the environment
 * as it found it — the cases in every other file run in the same process
 * when the binary is run bare.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_scenario.h"   /* serverSimScenarioListDir */
#include "scenario_defs.h"         /* ScnDirEntry */
#include "scenario_host.h"
#include "server/sim/server_sim_shared.h" /* serverSimSetActive */
#include "test_harness.h"

#define MD_MAX 32

/* ── The three directories ────────────────────────────────────────── */

static char mdConfigured[512];
static char mdUser[512];
static char mdWorkshop[512];
static char mdShipped[512];

/* All three under this test's own scratch directory. Not all of them are
 * created: a case that wants a directory to be missing passes false for it,
 * because "not there" is the ordinary case for every one of them and the
 * listing has to say nothing about it. The Workshop directory is always
 * made, and holds nothing until a case writes to it. */
static bool mdMakeDirs(bool configured, bool user, bool shipped) {
    if (!utScratchPath(mdConfigured, sizeof(mdConfigured), "configured") ||
        !utScratchPath(mdUser, sizeof(mdUser), "Mods") ||
        !utScratchPath(mdWorkshop, sizeof(mdWorkshop), "Workshop") ||
        !utScratchPath(mdShipped, sizeof(mdShipped), "shipped")) {
        return false;
    }
    if (!SDL_CreateDirectory(mdWorkshop)) return false;
    if (configured && !SDL_CreateDirectory(mdConfigured)) return false;
    if (user && !SDL_CreateDirectory(mdUser)) return false;
    if (shipped && !SDL_CreateDirectory(mdShipped)) return false;
    return true;
}

/* A loose mod script, named by its manifest so a case can tell which copy of
 * a file name it was handed. */
static bool mdWriteMod(const char *dir, const char *file, const char *name) {
    char  path[768];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fprintf(f,
            "scenario = {\n"
            "  name = \"%s\",\n"
            "  api = 1,\n"
            "  kind = \"mod\",\n"
            "  bound = false,\n"
            "}\n",
            name);
    fclose(f);
    return true;
}

/* ── The seams ────────────────────────────────────────────────────── */

static void mdSetEnv(const char *key, const char *val) {
#ifdef _WIN32
    _putenv_s(key, val != NULL ? val : "");
#else
    if (val != NULL) setenv(key, val, 1);
    else unsetenv(key);
#endif
}

static void mdEnvUp(void) {
    mdSetEnv("WB_MOD_DIR_USER", mdUser);
    mdSetEnv("WB_MOD_DIR_WORKSHOP", mdWorkshop);
    mdSetEnv("WB_MOD_DIR_SHIPPED", mdShipped);
}

static void mdEnvDown(void) {
    mdSetEnv("WB_MOD_DIR_USER", NULL);
    mdSetEnv("WB_MOD_DIR_WORKSHOP", NULL);
    mdSetEnv("WB_MOD_DIR_SHIPPED", NULL);
}

/* The list, with the seams up only while it is taken. */
static int mdList(ServerSim *sim, ScnDirEntry *out, int max) {
    int n;

    mdEnvUp();
    n = serverSimScenarioListDir(sim, out, max);
    mdEnvDown();
    return n;
}

/* ── The sim that asks for one ────────────────────────────────────── */

static ServerSim *mdSim(const char *configuredDir) {
    ServerSim *sim = ut_make_running_sim("Host");

    if (sim == NULL) return NULL;
    serverSimSetActive(sim);
    serverSimSetScenarioDir(sim, configuredDir);
    scenarioHostRegisterScenarioLister(sim);
    return sim;
}

/* The entry with this file name, or NULL when the list does not hold one. */
static const ScnDirEntry *mdFind(const ScnDirEntry *list, int count,
                                 const char *file) {
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(list[i].file, file) == 0) return &list[i];
    }
    return NULL;
}

/* Every file name on one line, so a failure says what was listed rather than
 * only what was missing. */
static void mdNames(const ScnDirEntry *list, int count, char *out,
                    size_t outLen) {
    int    i;
    size_t at = 0;

    out[0] = '\0';
    for (i = 0; i < count && at + 1 < outLen; i++) {
        at += (size_t)snprintf(out + at, outLen - at, "[%s] ", list[i].file);
    }
}

/* ── 1. The player's own directory ────────────────────────────────── */

/* A file the player dropped in their preferences directory is offered,
   without the host naming that directory anywhere: that is the whole point
   of the middle entry. */
int run_scenario_mod_dirs_user_dir_offered(void) {
    ServerSim   *sim;
    ScnDirEntry  list[MD_MAX];
    char         seen[1024];
    int          n;

    UT_ASSERT(mdMakeDirs(true, true, true));
    UT_ASSERT(mdWriteMod(mdUser, "mine.lua", "Mine"));

    sim = mdSim(mdConfigured);
    UT_ASSERT(sim != NULL);
    n = mdList(sim, list, MD_MAX);
    mdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));

    UT_ASSERT_MSG(n == 1, "the listing holds %d rows, wanted the one in the "
                          "player's directory: %s", n, seen);
    UT_ASSERT_MSG(mdFind(list, n, "mine.lua") != NULL,
                  "the player's own file is not in the list: %s", seen);

    serverSimDestroy(sim);
    return 0;
}

/* ── 2. The shipped directory ─────────────────────────────────────── */

/* A host that has named nothing and dropped nothing is still offered what
   the build came with. */
int run_scenario_mod_dirs_shipped_offered(void) {
    ServerSim   *sim;
    ScnDirEntry  list[MD_MAX];
    char         seen[1024];
    int          n;

    UT_ASSERT(mdMakeDirs(false, false, true));
    UT_ASSERT(mdWriteMod(mdShipped, "shipped.lua", "Shipped"));

    sim = mdSim(mdConfigured);   /* named, but not there */
    UT_ASSERT(sim != NULL);
    n = mdList(sim, list, MD_MAX);
    mdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));

    UT_ASSERT_MSG(n == 1, "the listing holds %d rows, wanted the shipped "
                          "one: %s", n, seen);
    UT_ASSERT_MSG(mdFind(list, n, "shipped.lua") != NULL,
                  "the shipped mod is not in the list: %s", seen);

    serverSimDestroy(sim);
    return 0;
}

/* ── 3. The order is the precedence ───────────────────────────────── */

/* One file name in all three directories. The host's own copy is the one
   offered, and it is offered once. */
int run_scenario_mod_dirs_configured_wins(void) {
    ServerSim         *sim;
    ScnDirEntry        list[MD_MAX];
    const ScnDirEntry *row;
    char               seen[1024];
    int                n;

    UT_ASSERT(mdMakeDirs(true, true, true));
    UT_ASSERT(mdWriteMod(mdConfigured, "same.lua", "From Configured"));
    UT_ASSERT(mdWriteMod(mdUser, "same.lua", "From User"));
    UT_ASSERT(mdWriteMod(mdShipped, "same.lua", "From Shipped"));

    sim = mdSim(mdConfigured);
    UT_ASSERT(sim != NULL);
    n = mdList(sim, list, MD_MAX);
    mdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));

    UT_ASSERT_MSG(n == 1, "one file name in three directories came back as "
                          "%d rows: %s", n, seen);
    row = mdFind(list, n, "same.lua");
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(strcmp(row->name, "From Configured") == 0,
                  "the name resolved to \"%s\", wanted the host's own copy",
                  row->name);

    serverSimDestroy(sim);
    return 0;
}

/* The same question one step down: with nothing in the host's own
   directory, the player's copy wins over the shipped one. That is the way
   round a player can act on — replacing a shipped mod means putting a file
   of that name somewhere above it. */
int run_scenario_mod_dirs_user_beats_shipped(void) {
    ServerSim         *sim;
    ScnDirEntry        list[MD_MAX];
    const ScnDirEntry *row;
    char               seen[1024];
    int                n;

    UT_ASSERT(mdMakeDirs(true, true, true));
    UT_ASSERT(mdWriteMod(mdUser, "same.lua", "From User"));
    UT_ASSERT(mdWriteMod(mdShipped, "same.lua", "From Shipped"));

    sim = mdSim(mdConfigured);
    UT_ASSERT(sim != NULL);
    n = mdList(sim, list, MD_MAX);
    mdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));

    UT_ASSERT_MSG(n == 1, "one file name in two directories came back as %d "
                          "rows: %s", n, seen);
    row = mdFind(list, n, "same.lua");
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(strcmp(row->name, "From User") == 0,
                  "the name resolved to \"%s\", wanted the player's copy",
                  row->name);

    serverSimDestroy(sim);
    return 0;
}

/* ── 4. The union, in one order ───────────────────────────────────── */

/* A file in each directory: all three are offered, and the rows are in
   file-name order rather than directory order, because a chooser draws them
   in the order it is handed. */
int run_scenario_mod_dirs_merged_and_sorted(void) {
    ServerSim   *sim;
    ScnDirEntry  list[MD_MAX];
    char         seen[1024];
    int          n;
    int          i;

    UT_ASSERT(mdMakeDirs(true, true, true));
    UT_ASSERT(mdWriteMod(mdConfigured, "mid.lua", "Mid"));
    UT_ASSERT(mdWriteMod(mdUser, "zed.lua", "Zed"));
    UT_ASSERT(mdWriteMod(mdShipped, "alpha.lua", "Alpha"));

    sim = mdSim(mdConfigured);
    UT_ASSERT(sim != NULL);
    n = mdList(sim, list, MD_MAX);
    mdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));

    UT_ASSERT_MSG(n == 3, "the listing holds %d rows, wanted one from each "
                          "directory: %s", n, seen);
    UT_ASSERT_MSG(mdFind(list, n, "mid.lua") != NULL, "no mid.lua: %s", seen);
    UT_ASSERT_MSG(mdFind(list, n, "zed.lua") != NULL, "no zed.lua: %s", seen);
    UT_ASSERT_MSG(mdFind(list, n, "alpha.lua") != NULL,
                  "no alpha.lua: %s", seen);
    for (i = 1; i < n; i++) {
        UT_ASSERT_MSG(SDL_strcasecmp(list[i - 1].file, list[i].file) <= 0,
                      "the merged list is not in file-name order: %s", seen);
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── 5. One directory named twice ─────────────────────────────────── */

/* A host whose own directory is the player's — which is what the desktop
   default is — reads it once and is offered each file once. */
int run_scenario_mod_dirs_same_dir_once(void) {
    ServerSim   *sim;
    ScnDirEntry  list[MD_MAX];
    char         seen[1024];
    int          n;

    UT_ASSERT(mdMakeDirs(false, true, true));
    UT_ASSERT(mdWriteMod(mdUser, "only.lua", "Only"));

    sim = mdSim(mdUser);         /* the same path, as the default gives it */
    UT_ASSERT(sim != NULL);
    n = mdList(sim, list, MD_MAX);
    mdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));

    UT_ASSERT_MSG(n == 1, "the file in the directory named twice came back "
                          "as %d rows: %s", n, seen);

    serverSimDestroy(sim);
    return 0;
}

/* ── 6. None of them there ────────────────────────────────────────── */

/* Three directories that are not there is not a failure and says nothing:
   it means a host with no mods, which is the ordinary case. */
int run_scenario_mod_dirs_all_missing_is_quiet(void) {
    ServerSim   *sim;
    ScnDirEntry  list[MD_MAX];
    char         seen[1024];
    int          n;

    UT_ASSERT(mdMakeDirs(false, false, false));

    sim = mdSim(mdConfigured);
    UT_ASSERT(sim != NULL);
    n = mdList(sim, list, MD_MAX);
    mdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));

    UT_ASSERT_MSG(n == 0, "three directories that are not there answered %d "
                          "rows: %s", n, seen);

    serverSimDestroy(sim);
    return 0;
}

/* ── 7. What the list offers, the host can load ───────────────────── */

/* The resolution half reads the same directories in the same order, so a
   mod that only the shipped directory holds attaches by its file name with
   the host's own directory named alongside it. A listing that offers a file
   nothing can load would be worse than not offering it. */
int run_scenario_mod_dirs_attach_reads_shipped(void) {
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(mdMakeDirs(true, true, true));
    UT_ASSERT(mdWriteMod(mdShipped, "shipped.lua", "Shipped"));

    sim = mdSim(mdConfigured);
    UT_ASSERT(sim != NULL);

    mdEnvUp();
    h = scenarioHostAttachMod(sim, mdConfigured, "shipped.lua", err,
                              sizeof(err));
    mdEnvDown();

    UT_ASSERT_MSG(h != NULL, "a mod only the shipped directory holds was "
                             "refused: %s", err);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

/* ── 8. The Workshop directory ────────────────────────────────────── */

/* A file the player wrote wins over a subscribed item of the same name, and
   a subscribed item wins over a shipped mod. The Workshop directory's own
   rows say so; a name the player's directory took from it does not. */
int run_scenario_mod_dirs_workshop_precedence(void) {
    ServerSim         *sim;
    ScnDirEntry        list[MD_MAX];
    const ScnDirEntry *row;
    char               seen[1024];
    int                n;

    UT_ASSERT(mdMakeDirs(true, true, true));
    UT_ASSERT(mdWriteMod(mdUser, "same.lua", "From User"));
    UT_ASSERT(mdWriteMod(mdWorkshop, "same.lua", "From Workshop"));
    UT_ASSERT(mdWriteMod(mdShipped, "same.lua", "From Shipped"));
    UT_ASSERT(mdWriteMod(mdWorkshop, "two.lua", "Two From Workshop"));
    UT_ASSERT(mdWriteMod(mdShipped, "two.lua", "Two From Shipped"));
    UT_ASSERT(mdWriteMod(mdWorkshop, "sub.lua", "Subscribed"));

    sim = mdSim(mdConfigured);
    UT_ASSERT(sim != NULL);
    n = mdList(sim, list, MD_MAX);
    mdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));

    UT_ASSERT_MSG(n == 3, "the listing holds %d rows, wanted same, two and "
                          "sub once each: %s", n, seen);

    row = mdFind(list, n, "same.lua");
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(strcmp(row->name, "From User") == 0,
                  "same.lua resolved to \"%s\", wanted the player's copy",
                  row->name);
    UT_ASSERT_MSG(row->source == SCN_DIR_SOURCE_SERVER,
                  "the player's same.lua says source %d", (int)row->source);

    row = mdFind(list, n, "two.lua");
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(strcmp(row->name, "Two From Workshop") == 0,
                  "two.lua resolved to \"%s\", wanted the Workshop copy",
                  row->name);
    UT_ASSERT_MSG(row->source == SCN_DIR_SOURCE_WORKSHOP,
                  "the Workshop's two.lua says source %d", (int)row->source);

    row = mdFind(list, n, "sub.lua");
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(row->source == SCN_DIR_SOURCE_WORKSHOP,
                  "a file only the Workshop holds says source %d",
                  (int)row->source);

    serverSimDestroy(sim);
    return 0;
}

/* This computer's own listing reads the Workshop directory after Mods: its
   rows say SERVER_SCENARIO_SOURCE_WORKSHOP, a name Mods also holds is Mods'
   file, its files have a path, and a copy is never saved over a name it
   holds. */
int run_scenario_mod_dirs_workshop_local(void) {
    ServerScenarioEntry     list[MD_MAX];
    ScenarioLocalSaveResult saved;
    char                    path[1200];
    char                    want[768];
    bool                    found;
    int                     n;
    int                     i;
    bool                    sawMine = false;
    bool                    sawSub  = false;
    bool                    sawBoth = false;

    UT_ASSERT(mdMakeDirs(false, true, false));
    UT_ASSERT(mdWriteMod(mdUser, "mine.lua", "Mine"));
    UT_ASSERT(mdWriteMod(mdUser, "both.lua", "Both From Mods"));
    UT_ASSERT(mdWriteMod(mdWorkshop, "both.lua", "Both From Workshop"));
    UT_ASSERT(mdWriteMod(mdWorkshop, "sub.lua", "Subscribed"));

    mdEnvUp();
    n     = scenarioHostListLocalScripts(list, MD_MAX);
    found = scenarioHostLocalScriptPath("sub.lua", path, sizeof(path));
    saved = scenarioHostSaveLocalScript("SUB.lua", (const uint8_t *)"-- x\n", 5);
    mdEnvDown();

    UT_ASSERT_MSG(n == 3, "%d local rows, wanted mine, both and sub", n);
    for (i = 0; i < n; i++) {
        if (strcmp(list[i].file, "mine.lua") == 0) {
            sawMine = true;
            UT_ASSERT_MSG(list[i].source == SERVER_SCENARIO_SOURCE_SERVER,
                          "mine.lua says source %d", (int)list[i].source);
        } else if (strcmp(list[i].file, "sub.lua") == 0) {
            sawSub = true;
            UT_ASSERT_MSG(list[i].source == SERVER_SCENARIO_SOURCE_WORKSHOP,
                          "sub.lua says source %d", (int)list[i].source);
        } else if (strcmp(list[i].file, "both.lua") == 0) {
            sawBoth = true;
            UT_ASSERT_MSG(strcmp(list[i].name, "Both From Mods") == 0,
                          "both.lua resolved to \"%s\", wanted Mods' copy",
                          list[i].name);
            UT_ASSERT_MSG(list[i].source == SERVER_SCENARIO_SOURCE_SERVER,
                          "both.lua says source %d", (int)list[i].source);
        }
    }
    UT_ASSERT(sawMine && sawSub && sawBoth);

    snprintf(want, sizeof(want), "%s/sub.lua", mdWorkshop);
    UT_ASSERT_MSG(found, "no path for a file only the Workshop holds");
    UT_ASSERT_MSG(strcmp(path, want) == 0, "path '%s', wanted '%s'", path,
                  want);

    UT_ASSERT_MSG(saved == SCENARIO_LOCAL_SAVE_EXISTS,
                  "a copy under a name the Workshop holds answered %d",
                  (int)saved);
    snprintf(want, sizeof(want), "%s/SUB.lua", mdUser);
    UT_ASSERT_MSG(!SDL_GetPathInfo(want, NULL),
                  "the refused copy landed in Mods anyway");
    return 0;
}

/* An upload under a name the Workshop directory holds is refused, as one
   under a name the player's own directory holds is: the Workshop directory
   is above the landing one. */
int run_scenario_mod_dirs_workshop_upload_clash(void) {
    static const char kMod[] =
        "scenario = {\n"
        "  name = \"Uploaded\",\n"
        "  api = 1,\n"
        "  kind = \"mod\",\n"
        "  bound = false,\n"
        "}\n";
    ServerSim          *sim;
    ScriptUploadRefusal why;
    char                landing[512];
    char                path[768];
    bool                clash;
    bool                other;

    UT_ASSERT(mdMakeDirs(true, true, true));
    UT_ASSERT(utScratchPath(landing, sizeof(landing), "landing"));
    UT_ASSERT(mdWriteMod(mdWorkshop, "clash.lua", "Subscribed"));

    sim = mdSim(mdConfigured);
    UT_ASSERT(sim != NULL);
    serverSimSetScriptUploadDir(sim, landing);

    mdEnvUp();
    clash = serverSimScriptUploadAccept(sim, landing, "clash.lua",
                                        (const uint8_t *)kMod,
                                        sizeof(kMod) - 1, &why);
    mdEnvDown();

    UT_ASSERT_MSG(!clash, "an upload under a name the Workshop holds was "
                          "taken");
    UT_ASSERT_MSG(why.reason == SCRIPT_REFUSE_NAME_TAKEN, "reason %d '%s'",
                  (int)why.reason, why.text);
    snprintf(path, sizeof(path), "%s/clash.lua", landing);
    UT_ASSERT_MSG(!SDL_GetPathInfo(path, NULL),
                  "the refused upload landed anyway");

    /* A name nothing above holds still lands. */
    mdEnvUp();
    other = serverSimScriptUploadAccept(sim, landing, "other.lua",
                                        (const uint8_t *)kMod,
                                        sizeof(kMod) - 1, &why);
    mdEnvDown();
    UT_ASSERT_MSG(other, "a name nothing above holds was refused: %s",
                  why.text);

    serverSimDestroy(sim);
    return 0;
}
