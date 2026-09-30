/*
 * What serverSimRefreshWbnLobbyInfo tells WinBolo.net about the scripts a
 * round runs.
 *
 * The fill reads the scenario identity the sim holds and the lobby's script
 * list, and hands the struct to winbolonetSetLobbyInfo, which the unit
 * binary stubs to keep a copy in wbnStubLastLobbyInfo. The bodies built from
 * that struct are tested against a listener in WinBoloWbnTests; this is the
 * sim half.
 *
 * run_wbn_lobby_info_scenario — each case below on one sim:
 *   - a plain round: no scenario, no mods;
 *   - a scenario identity and two mod rows: the name, the cap and both mod
 *     names in list order, the scenario's own row not among the mods;
 *   - an identity that is a mod: no scenario;
 *   - Mods Enabled off: no mods, though the list still names them;
 *   - a mod row with an empty name: its file name instead;
 *   - a file name past 63 bytes: cut to 63 on a UTF-8 character boundary.
 *
 * No scenario host is registered, so the identity and the list a case sets
 * by hand are what the fill reads.
 */

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"          /* lobbyScenarioMod, lobbyScenarioNone */
#include "server_sim.h"
#include "server_sim_internal.h"    /* serverSimSetScriptList */
#include "server_sim_lifecycle.h"   /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"    /* serverSimSetScenarioIdentity,
                                       serverSimSetScenarioLobbyTemplate */
#include "scenario_defs.h"          /* ScnDirEntry, ScnLobbyTemplate */
#include "../../src/winbolonet/winbolonet_server.h" /* WbnLobbyInfo */
#include "everard_map.h"
#include "test_harness.h"

/* The last lobby info the sim handed over, kept by the stub in
 * test_stubs.c. */
extern WbnLobbyInfo wbnStubLastLobbyInfo;

/* A lobby nobody is in, on the inbuilt map. */
static ServerSim *wliLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

static void wliRow(ScnDirEntry *row, const char *file, const char *name,
                   bool keepsWinCondition) {
    memset(row, 0, sizeof(*row));
    snprintf(row->file, sizeof(row->file), "%s", file);
    snprintf(row->name, sizeof(row->name), "%s", name);
    row->keepsWinCondition = keepsWinCondition;
}

/* Runs the fill and returns what it handed over. */
static const WbnLobbyInfo *wliRefresh(ServerSim *sim) {
    memset(&wbnStubLastLobbyInfo, 0xAB, sizeof(wbnStubLastLobbyInfo));
    serverSimRefreshWbnLobbyInfo(sim);
    return &wbnStubLastLobbyInfo;
}

int run_wbn_lobby_info_scenario(void) {
    ServerSim          *sim = wliLobbySim();
    ScnDirEntry         rows[3];
    ScnLobbyTemplate    t;
    const WbnLobbyInfo *info;

    UT_ASSERT(sim != NULL);

    /* A plain round. */
    info = wliRefresh(sim);
    UT_ASSERT_MSG(!info->hasScenario, "a plain round reported a scenario");
    UT_ASSERT_MSG(info->modCount == 0, "a plain round reported %u mods",
                  (unsigned)info->modCount);

    /* A scenario, its cap, and two mods behind it. */
    memset(&t, 0, sizeof(t));
    t.maxPlayers = 6;
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimSetScenarioIdentity(sim, lobbyScenarioMod, "Survival",
                                 "survival.lua", "Last tank standing", false,
                                 false, false, false, false);
    wliRow(&rows[0], "survival.lua", "Survival", false);
    wliRow(&rows[1], "infection.lua", "Infection", true);
    wliRow(&rows[2], "pilltag.lua", "Pillbox Tag", true);
    serverSimSetScriptList(sim, rows, 3);
    info = wliRefresh(sim);
    UT_ASSERT_MSG(info->hasScenario, "the scenario was not reported");
    UT_ASSERT_MSG(strcmp(info->scenarioName, "Survival") == 0,
                  "the scenario was reported as '%s'", info->scenarioName);
    UT_ASSERT_MSG(info->scenarioMaxPlayers == 6,
                  "the cap was reported as %u",
                  (unsigned)info->scenarioMaxPlayers);
    UT_ASSERT_MSG(info->modCount == 2, "%u mods were reported, expected 2",
                  (unsigned)info->modCount);
    UT_ASSERT_MSG(strcmp(info->modNames[0], "Infection") == 0,
                  "mod 0 was '%s'", info->modNames[0]);
    UT_ASSERT_MSG(strcmp(info->modNames[1], "Pillbox Tag") == 0,
                  "mod 1 was '%s'", info->modNames[1]);

    /* The identity is a mod, so no scenario decides the round. */
    serverSimSetScenarioIdentity(sim, lobbyScenarioMod, "Infection",
                                 "infection.lua", "", false,
                                 true, false, false, false);
    wliRow(&rows[0], "infection.lua", "Infection", true);
    serverSimSetScriptList(sim, rows, 1);
    info = wliRefresh(sim);
    UT_ASSERT_MSG(!info->hasScenario,
                  "a mod as the identity was reported as a scenario");
    UT_ASSERT_MSG(info->modCount == 1, "%u mods were reported, expected 1",
                  (unsigned)info->modCount);

    /* Mods Enabled off: the list still names the mod, and none is sent. */
    serverSimSetModsOff(sim, true);
    info = wliRefresh(sim);
    UT_ASSERT_MSG(info->modCount == 0,
                  "%u mods were reported with Mods Enabled off",
                  (unsigned)info->modCount);
    serverSimSetModsOff(sim, false);

    /* A mod whose manifest named nothing reads as its file, as the lobby
     * draws it. */
    wliRow(&rows[0], "unnamed.lua", "", true);
    serverSimSetScriptList(sim, rows, 1);
    info = wliRefresh(sim);
    UT_ASSERT_MSG(info->modCount == 1, "%u mods were reported, expected 1",
                  (unsigned)info->modCount);
    UT_ASSERT_MSG(strcmp(info->modNames[0], "unnamed.lua") == 0,
                  "the unnamed mod was reported as '%s'", info->modNames[0]);

    /* A file name past 63 bytes is cut on a character boundary: 61 ASCII
     * bytes and an é fill the 63, and the à behind them would straddle the
     * last byte, so it goes whole. */
    {
        char longFile[80];
        size_t len;

        memset(longFile, 'a', 61);
        memcpy(longFile + 61, "\xc3\xa9\xc3\xa0.lua",
               sizeof("\xc3\xa9\xc3\xa0.lua"));
        wliRow(&rows[0], longFile, "", true);
        serverSimSetScriptList(sim, rows, 1);
        info = wliRefresh(sim);
        UT_ASSERT_MSG(info->modCount == 1, "%u mods were reported, expected 1",
                      (unsigned)info->modCount);
        len = strlen(info->modNames[0]);
        UT_ASSERT_MSG(len == 63, "the long file name was cut to %u bytes, "
                      "expected 63", (unsigned)len);
        UT_ASSERT_MSG(memcmp(info->modNames[0] + 61, "\xc3\xa9", 2) == 0,
                      "the long file name does not end in a whole \xc3\xa9");
    }

    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false, false, false, false, false);
    serverSimDestroy(sim);
    return 0;
}
