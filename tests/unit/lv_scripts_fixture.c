/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The sim side of the log viewer's scripts.json cases: a scripted round,
 * recorded through the replay harness. See lv_scripts_fixture.h.
 *
 * The same real path test_scripts_record.c drives: a scratch scenarios
 * directory, the host's own lister, a map commit, the lobby's list command
 * and the in-place round start, which boots the round without reloading the
 * map — the map named here is never written, only the script beside it.
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_command.h"        /* CMD_SET_SCRIPT_LIST */
#include "server_sim.h"
#include "server_sim_internal.h"   /* mapFilePath, scenarioPickTick */
#include "server_sim_lifecycle.h"  /* serverSimStartGameInPlace,
                                    * serverSimScenarioOnMapChanged */
#include "scenario_host.h"
#include "scenario_validate.h"     /* scenarioHostScriptCount */
#include "everard_map.h"
#include "threads.h"
#include "replay_harness.h"
#include "lv_scripts_fixture.h"
#include "test_harness.h"

static char lvsfDir[256];

static bool lvsfPutFile(const char *leaf, const char *text) {
    char  path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", lvsfDir, leaf);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void lvsfDropDir(void) {
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", lvsfDir, LVSF_MAP_SCRIPT);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", lvsfDir, LVSF_MOD_FILE);
    remove(path);
    SDL_RemovePath(lvsfDir);
}

/* The map's own scenario: one rule and one region. */
static const char kLvsfScenario[] =
    "scenario = {\n"
    "  name = \"" LVSF_SCENARIO_NAME "\",\n"
    "  api = 1,\n"
    "  rules = { tank_death_ticks = 400 },\n"
    "  regions = { keep = { x = 10, y = 12, w = 6, h = 4 } },\n"
    "}\n";

/* A mod behind it, from the scenarios directory. */
static const char kLvsfMod[] =
    "scenario = {\n"
    "  name = \"" LVSF_MOD_NAME "\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n";

static ScenarioHost *lvsfSlot;

/* The host's whole list, through the lobby's command. */
static CmdResult lvsfPick(ServerSim *sim, const char *file) {
    ClientCommand cmd;
    CmdResult     r;

    sim->scenarioPickTick = 0;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_SET_SCRIPT_LIST;
    cmd.cmdSeq = 1;
    cmd.u.setScriptList.count = 1;
    snprintf(cmd.u.setScriptList.files[0],
             sizeof(cmd.u.setScriptList.files[0]), "%s", file);
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    return r;
}

bool lvScriptsRecordScriptedRound(const char *tag, char *path, size_t pathLen) {
    BYTE          emap[6000] = E_MAP;
    char          leaf[128];
    char          mapPath[512];
    ServerSim    *sim;
    ReplayHarness h;
    bool          ok = false;

    if (path == NULL || pathLen == 0) return false;
    path[0] = '\0';

    snprintf(leaf, sizeof(leaf), "wbtest_lv_scripts_%s", tag);
    if (!utScratchPath(lvsfDir, sizeof(lvsfDir), leaf)) return false;
    (void)SDL_RemovePath(lvsfDir);
    if (!SDL_CreateDirectory(lvsfDir)) return false;
    if (!lvsfPutFile(LVSF_MOD_FILE, kLvsfMod) ||
        !lvsfPutFile(LVSF_MAP_SCRIPT, kLvsfScenario)) {
        lvsfDropDir();
        return false;
    }

    sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                    gameOpen, false, 0, -1);
    if (sim == NULL) {
        lvsfDropDir();
        return false;
    }
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetState(sim, serverStateLobby);
    lvsfSlot = NULL;
    serverSimSetScenarioDir(sim, lvsfDir);
    scenarioHostRegisterScenarioLister(sim);
    scenarioHostFollowMap(sim, &lvsfSlot);

    /* The map commit, the way the sim makes it after loading the new map. */
    snprintf(mapPath, sizeof(mapPath), "%s/%s", lvsfDir, LVSF_MAP_LEAF);
    SDL_strlcpy(sim->mapFilePath, mapPath, sizeof(sim->mapFilePath));
    serverSimScenarioOnMapChanged(sim, mapPath);
    serverSimScenarioApplyLobbyRules(sim);

    if (lvsfPick(sim, LVSF_MOD_FILE) == CMD_OK && lvsfSlot != NULL &&
        scenarioHostScriptCount(lvsfSlot) == 2) {
        serverSimSetLobbyEnabled(sim, false);
        serverSimStartGameInPlace(sim);

        /* The harness records this sim rather than making one of its own. */
        memset(&h, 0, sizeof(h));
        snprintf(leaf, sizeof(leaf), "wbtest_lv_scripts_%s.wbv", tag);
        if (utScratchPath(h.path, sizeof(h.path), leaf)) {
            remove(h.path);
            h.sim = sim;
            if (replayHarnessBeginRecording(&h)) {
                replayHarnessTick(&h, 6);
                ok = replayHarnessStopRecording(&h);
            }
            snprintf(path, pathLen, "%s", h.path);
            /* The sim is this function's and the file is the caller's, so the
               Stop frees the captures and touches neither. */
            h.sim     = NULL;
            h.path[0] = '\0';
            replayHarnessStop(&h);
        }
    }

    /* The host holds registrations on the sim, so it goes first. */
    scenarioHostDetach(lvsfSlot);
    lvsfSlot = NULL;
    serverSimDestroy(sim);
    lvsfDropDir();
    return ok;
}
