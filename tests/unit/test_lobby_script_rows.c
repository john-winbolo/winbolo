/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Mods chooser's left column, which lists what the server offers and the
 * scripts on the player's own computer as one column.
 *
 * lobbyScriptRowsClassify says where each row is: the server alone, both, or
 * this computer alone. The rules it holds are the matching (a Workshop id on
 * both sides decides, a file name ignoring case otherwise), the order (the
 * server's rows first, then the files only this computer has), and that a
 * server in this process makes no row of the third kind.
 *
 * The last case is the listing that feeds it: scenarioHostListLocalScripts
 * reading the player's Mods directory with no sim, and
 * scenarioHostLocalScriptPath naming a file it listed. WB_MOD_DIR_USER points
 * the directory at the scratch path, and is set only for the length of each
 * call so a failed case leaves the environment as it found it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "server_sim.h"
#include "lobby_script_rows.h"
#include "scenario_host.h"
#include "test_harness.h"

#define LSR_MAX 16

/* A row with a file name and a Workshop id, and nothing else the classifier
 * reads. */
static void lsrRow(ServerScenarioEntry *e, const char *file, uint64_t id) {
    memset(e, 0, sizeof(*e));
    snprintf(e->file, sizeof(e->file), "%s", file);
    e->workshopId = id;
}

/* ── The three states ─────────────────────────────────────────────── */

/* One file on the server alone, one on both, one on this computer alone. */
int run_lobby_script_rows_states_by_name(void) {
    ServerScenarioEntry server[2];
    ServerScenarioEntry localRows[2];
    LobbyScriptRow      out[LSR_MAX];
    int                 n;

    lsrRow(&server[0], "theirs.lua", 0);
    lsrRow(&server[1], "shared.lua", 0);
    lsrRow(&localRows[0], "shared.lua", 0);
    lsrRow(&localRows[1], "mine.lua", 0);

    n = lobbyScriptRowsClassify(server, 2, localRows, 2, false, out, LSR_MAX);
    UT_ASSERT_MSG(n == 3, "%d rows, wanted 3", n);

    UT_ASSERT(out[0].state == LOBBY_SCRIPT_ROW_SERVER_ONLY);
    UT_ASSERT(out[0].serverIdx == 0 && out[0].localIdx == -1);

    UT_ASSERT(out[1].state == LOBBY_SCRIPT_ROW_BOTH);
    UT_ASSERT(out[1].serverIdx == 1 && out[1].localIdx == 0);

    UT_ASSERT(out[2].state == LOBBY_SCRIPT_ROW_LOCAL_ONLY);
    UT_ASSERT(out[2].serverIdx == -1 && out[2].localIdx == 1);
    return 0;
}

/* ── Matching ─────────────────────────────────────────────────────── */

/* Two names that differ only in case are the same file. */
int run_lobby_script_rows_case_only_match(void) {
    ServerScenarioEntry server[1];
    ServerScenarioEntry localRows[1];
    LobbyScriptRow      out[LSR_MAX];
    int                 n;

    lsrRow(&server[0], "Waves.lua", 0);
    lsrRow(&localRows[0], "waves.LUA", 0);

    n = lobbyScriptRowsClassify(server, 1, localRows, 1, false, out, LSR_MAX);
    UT_ASSERT_MSG(n == 1, "%d rows, wanted the one both hold", n);
    UT_ASSERT(out[0].state == LOBBY_SCRIPT_ROW_BOTH);
    UT_ASSERT(out[0].localIdx == 0);
    return 0;
}

/* Equal Workshop ids are the same item whatever the files are called. */
int run_lobby_script_rows_workshop_id_matches(void) {
    ServerScenarioEntry server[1];
    ServerScenarioEntry localRows[1];
    LobbyScriptRow      out[LSR_MAX];
    int                 n;

    lsrRow(&server[0], "renamed.lua", 4242ull);
    lsrRow(&localRows[0], "original.lua", 4242ull);

    n = lobbyScriptRowsClassify(server, 1, localRows, 1, false, out, LSR_MAX);
    UT_ASSERT_MSG(n == 1, "%d rows, wanted the one both hold", n);
    UT_ASSERT(out[0].state == LOBBY_SCRIPT_ROW_BOTH);
    return 0;
}

/* Two different Workshop items are two files even under one name. */
int run_lobby_script_rows_workshop_id_differs(void) {
    ServerScenarioEntry server[1];
    ServerScenarioEntry localRows[1];
    LobbyScriptRow      out[LSR_MAX];
    int                 n;

    lsrRow(&server[0], "same.lua", 1ull);
    lsrRow(&localRows[0], "same.lua", 2ull);

    n = lobbyScriptRowsClassify(server, 1, localRows, 1, false, out, LSR_MAX);
    UT_ASSERT_MSG(n == 2, "%d rows, wanted two different items", n);
    UT_ASSERT(out[0].state == LOBBY_SCRIPT_ROW_SERVER_ONLY);
    UT_ASSERT(out[1].state == LOBBY_SCRIPT_ROW_LOCAL_ONLY);
    return 0;
}

/* ── In process ───────────────────────────────────────────────────── */

/* A server in this process already lists this computer's files, so the local
   list is not read: no row of this computer's alone, and none marked both. */
int run_lobby_script_rows_in_process(void) {
    ServerScenarioEntry server[1];
    ServerScenarioEntry localRows[2];
    LobbyScriptRow      out[LSR_MAX];
    int                 n;
    int                 i;

    lsrRow(&server[0], "shared.lua", 0);
    lsrRow(&localRows[0], "shared.lua", 0);
    lsrRow(&localRows[1], "mine.lua", 0);

    n = lobbyScriptRowsClassify(server, 1, localRows, 2, true, out, LSR_MAX);
    UT_ASSERT_MSG(n == 1, "%d rows, wanted the server's one", n);
    for (i = 0; i < n; i++) {
        UT_ASSERT(out[i].state == LOBBY_SCRIPT_ROW_SERVER_ONLY);
        UT_ASSERT(out[i].localIdx == -1);
    }
    return 0;
}

/* ── Order and the cap ────────────────────────────────────────────── */

/* The server's rows in the server's order, whatever the local order, then
   the local-only rows in the local order. */
int run_lobby_script_rows_order(void) {
    ServerScenarioEntry server[3];
    ServerScenarioEntry localRows[4];
    LobbyScriptRow      out[LSR_MAX];
    int                 n;

    lsrRow(&server[0], "c.lua", 0);
    lsrRow(&server[1], "a.lua", 0);
    lsrRow(&server[2], "b.lua", 0);
    lsrRow(&localRows[0], "z.lua", 0);
    lsrRow(&localRows[1], "b.lua", 0);
    lsrRow(&localRows[2], "y.lua", 0);
    lsrRow(&localRows[3], "c.lua", 0);

    n = lobbyScriptRowsClassify(server, 3, localRows, 4, false, out, LSR_MAX);
    UT_ASSERT_MSG(n == 5, "%d rows, wanted 5", n);

    UT_ASSERT(out[0].serverIdx == 0 && out[0].localIdx == 3);
    UT_ASSERT(out[1].serverIdx == 1 && out[1].localIdx == -1);
    UT_ASSERT(out[2].serverIdx == 2 && out[2].localIdx == 1);
    UT_ASSERT(out[3].state == LOBBY_SCRIPT_ROW_LOCAL_ONLY &&
              out[3].localIdx == 0);
    UT_ASSERT(out[4].state == LOBBY_SCRIPT_ROW_LOCAL_ONLY &&
              out[4].localIdx == 2);
    return 0;
}

/* No more rows than max, the server's first, and nothing for no room. */
int run_lobby_script_rows_truncated(void) {
    ServerScenarioEntry server[2];
    ServerScenarioEntry localRows[2];
    LobbyScriptRow      out[LSR_MAX];
    int                 n;

    lsrRow(&server[0], "a.lua", 0);
    lsrRow(&server[1], "b.lua", 0);
    lsrRow(&localRows[0], "c.lua", 0);
    lsrRow(&localRows[1], "d.lua", 0);

    /* A sentinel past the cap, which a write past it would change. */
    out[3].state     = LOBBY_SCRIPT_ROW_BOTH;
    out[3].serverIdx = 99;
    out[3].localIdx  = 99;

    n = lobbyScriptRowsClassify(server, 2, localRows, 2, false, out, 3);
    UT_ASSERT_MSG(n == 3, "%d rows, wanted the cap of 3", n);
    UT_ASSERT(out[0].serverIdx == 0 && out[1].serverIdx == 1);
    UT_ASSERT(out[2].state == LOBBY_SCRIPT_ROW_LOCAL_ONLY &&
              out[2].localIdx == 0);
    UT_ASSERT(out[3].serverIdx == 99 && out[3].localIdx == 99);

    n = lobbyScriptRowsClassify(server, 2, localRows, 2, false, out, 1);
    UT_ASSERT_MSG(n == 1, "%d rows, wanted the cap of 1", n);
    UT_ASSERT(out[0].serverIdx == 0);

    UT_ASSERT(lobbyScriptRowsClassify(server, 2, localRows, 2, false, out, 0) == 0);
    UT_ASSERT(lobbyScriptRowsClassify(server, 2, localRows, 2, false, NULL,
                                      LSR_MAX) == 0);
    return 0;
}

/* ── This computer's listing ──────────────────────────────────────── */

static char lsrModDir[512];
static char lsrWorkshopDir[512];   /* empty, so the real one is never read */

static void lsrSetEnv(const char *key, const char *val) {
#ifdef _WIN32
    _putenv_s(key, val != NULL ? val : "");
#else
    if (val != NULL) setenv(key, val, 1);
    else unsetenv(key);
#endif
}

static bool lsrWriteMod(const char *file, const char *name) {
    char  path[768];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", lsrModDir, file);
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

static int lsrList(ServerScenarioEntry *out, int max) {
    int n;

    lsrSetEnv("WB_MOD_DIR_USER", lsrModDir);
    lsrSetEnv("WB_MOD_DIR_WORKSHOP", lsrWorkshopDir);
    n = scenarioHostListLocalScripts(out, max);
    lsrSetEnv("WB_MOD_DIR_USER", NULL);
    lsrSetEnv("WB_MOD_DIR_WORKSHOP", NULL);
    return n;
}

static bool lsrPath(const char *file, char *out, size_t outLen) {
    bool ok;

    lsrSetEnv("WB_MOD_DIR_USER", lsrModDir);
    lsrSetEnv("WB_MOD_DIR_WORKSHOP", lsrWorkshopDir);
    ok = scenarioHostLocalScriptPath(file, out, outLen);
    lsrSetEnv("WB_MOD_DIR_USER", NULL);
    lsrSetEnv("WB_MOD_DIR_WORKSHOP", NULL);
    return ok;
}

/* A mod in the player's Mods directory is listed with no sim anywhere, its
   path is answered, and a file dropped in after the first listing is in the
   second. */
int run_scenario_local_scripts_listed(void) {
    ServerScenarioEntry list[LSR_MAX];
    char                path[2400];
    char                want[768];
    int                 n;

    UT_ASSERT(utScratchPath(lsrModDir, sizeof(lsrModDir), "Mods"));
    UT_ASSERT(SDL_CreateDirectory(lsrModDir));
    UT_ASSERT(utScratchPath(lsrWorkshopDir, sizeof(lsrWorkshopDir),
                            "Workshop"));
    UT_ASSERT(SDL_CreateDirectory(lsrWorkshopDir));
    UT_ASSERT(lsrWriteMod("mine.lua", "Mine"));

    n = lsrList(list, LSR_MAX);
    UT_ASSERT_MSG(n == 1, "%d rows, wanted the one mod", n);
    UT_ASSERT_MSG(strcmp(list[0].file, "mine.lua") == 0, "listed '%s'",
                  list[0].file);
    UT_ASSERT_MSG(strcmp(list[0].name, "Mine") == 0, "named '%s'",
                  list[0].name);
    UT_ASSERT(list[0].keepsWinCondition);
    UT_ASSERT(list[0].source == SERVER_SCENARIO_SOURCE_SERVER);
    UT_ASSERT(list[0].workshopId == 0);

    snprintf(want, sizeof(want), "%s/mine.lua", lsrModDir);
    UT_ASSERT(lsrPath("mine.lua", path, sizeof(path)));
    UT_ASSERT_MSG(strcmp(path, want) == 0, "path '%s', wanted '%s'", path,
                  want);
    UT_ASSERT(!lsrPath("absent.lua", path, sizeof(path)));
    UT_ASSERT(path[0] == '\0');
    UT_ASSERT(!lsrPath("../mine.lua", path, sizeof(path)));

    /* The directory's modify time may not move for a file written this
       soon after the read, which is what the process-wide count is for. */
    UT_ASSERT(lsrWriteMod("second.lua", "Second"));
    serverSimNoteScriptDirsChanged();

    n = lsrList(list, LSR_MAX);
    UT_ASSERT_MSG(n == 2, "%d rows after the second file, wanted 2", n);
    UT_ASSERT(strcmp(list[0].file, "mine.lua") == 0);
    UT_ASSERT(strcmp(list[1].file, "second.lua") == 0);
    return 0;
}
