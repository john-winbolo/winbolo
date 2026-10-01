/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A client that registers after the attach is given the scenario's rules
 * set, the way it is given the panels a scenario has already put up.
 *
 *   scenario_rules_reaches_joiner — a second ClientSim registers on a sim
 *       with a mod attached and holds the set the manifest named; one that
 *       registers on a sim with no scenario holds nothing. Without the
 *       replay a joiner would see the empty popup the host sees a full one
 *       in, until the next attach — which on a server rotating one map is
 *       the rest of the session.
 *
 * Drives the real path: a scratch scenarios directory with a real file in
 * it, the host's own lister on the sim, and the pick made by applying
 * CMD_LOBBY_SET_SCENARIO.
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_SET_SCENARIO */
#include "client_sim.h"            /* ClientSim — the joiner the replay reaches */
#include "control_event.h"
#include "sim_rules_names.h"       /* SIM_RULE_* — the rules the mod sets */
#include "server_sim.h"
#include "server_sim_internal.h"   /* scenarioIdentity — what the sim kept */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, SetState */
#include "server_sim_scenario.h"
#include "scenario_host.h"
#include "everard_map.h"
#include "threads.h"
#include "test_harness.h"

/* ── The scratch scenarios directory ──────────────────────────────── */

static char sjDir[256];
static char sjFile[128];

static bool sjMakeDir(const char *tag) {
    snprintf(sjDir, sizeof(sjDir), "wbtest_scn_rulesj_%s", tag);
    sjFile[0] = '\0';
    (void)SDL_RemovePath(sjDir);
    return SDL_CreateDirectory(sjDir);
}

static bool sjWriteMod(const char *name, const char *text) {
    char  path[512];
    FILE *f;

    snprintf(sjFile, sizeof(sjFile), "%s", name);
    snprintf(path, sizeof(path), "%s/%s", sjDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void sjDropDir(void) {
    char path[512];
    if (sjFile[0] != '\0') {
        snprintf(path, sizeof(path), "%s/%s", sjDir, sjFile);
        remove(path);
        sjFile[0] = '\0';
    }
    SDL_RemovePath(sjDir);
}

/* A rules-only mod: nothing but a name and two rules of different kinds. */
static const char kSjRulesOnly[] =
    "scenario = {\n"
    "  name = \"Fast Reload\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  rules = {\n"
    "    tank_reload_ticks = 5,\n"
    "    tank_full_shells = 80,\n"
    "  },\n"
    "}\n";

#define SJ_PLAIN_MAP "wbtest_scn_rulesj_plain.map"
#define SJ_SLOT_JOIN 3

/* ── The sim ──────────────────────────────────────────────────────── */

static ScenarioHost *sjSlot;

static ServerSim *sjSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetState(sim, serverStateLobby);

    sjSlot = NULL;
    serverSimSetScenarioDir(sim, sjDir);
    scenarioHostRegisterScenarioLister(sim);
    scenarioHostFollowMap(sim, &sjSlot);
    SDL_strlcpy(sim->mapFilePath, SJ_PLAIN_MAP, sizeof(sim->mapFilePath));
    return sim;
}

static void sjDestroy(ServerSim *sim) {
    scenarioHostDetach(sjSlot);
    sjSlot = NULL;
    serverSimDestroy(sim);
}

static CmdResult sjSelect(ServerSim *sim, const char *file) {
    ClientCommand cmd;
    CmdResult     r;
    size_t        n = strlen(file);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_SET_SCENARIO;
    cmd.cmdSeq = 1;
    cmd.u.lobbySetScenario.relPathLen = (uint8_t)n;
    if (n > 0) memcpy(cmd.u.lobbySetScenario.relPath, file, n);
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    return r;
}

/* The row the joiner holds for one rule, or -1 when it holds none. The
 * manifest's rows come out of a Lua table, whose traversal order is not the
 * file's, so the set is searched rather than indexed. */
static int sjRowOf(const ClientSim *cs, int rule) {
    int i;
    for (i = 0; i < clientSimGetScenarioRulesCount(cs); i++) {
        if (clientSimGetScenarioRuleIndex(cs, i) == rule) return i;
    }
    return -1;
}

/* A client registered on the sim, the way a joiner arrives: the replay runs
 * inside the registration, so the set is on the client by the time this has
 * answered. */
static ClientSim *sjJoin(ServerSim *sim, SubscriberHandle *outHandle) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, SJ_SLOT_JOIN);
    *outHandle = serverSimRegisterClientSubscriber(sim, cs);
    return cs;
}

int run_scenario_rules_reaches_joiner(void) {
    ServerSim       *sim;
    ClientSim       *cs;
    SubscriberHandle handle;
    int              row;

    UT_ASSERT(sjMakeDir("joiner"));
    UT_ASSERT(sjWriteMod("fastreload.lua", kSjRulesOnly));
    sim = sjSim();
    UT_ASSERT(sim != NULL);

    /* ── a joiner arriving before there is a scenario ── */
    cs = sjJoin(sim, &handle);
    UT_ASSERT(cs != NULL);
    UT_ASSERT_MSG(handle != SUBSCRIBER_HANDLE_INVALID,
                  "the client could not register");
    UT_ASSERT_MSG(clientSimGetScenarioRulesCount(cs) == 0,
                  "a joiner on a sim with no scenario holds %d rule(s)",
                  clientSimGetScenarioRulesCount(cs));
    serverSimUnregisterSubscriber(sim, handle);
    clientSimDestroy(cs);

    /* ── and one arriving after the attach ── */
    UT_ASSERT_MSG(sjSelect(sim, "fastreload.lua") == CMD_OK,
                  "the mod was refused");
    UT_ASSERT_MSG(sim->scenarioIdentity.source != lobbyScenarioNone,
                  "the mod did not attach");

    cs = sjJoin(sim, &handle);
    UT_ASSERT(cs != NULL);
    UT_ASSERT_MSG(handle != SUBSCRIBER_HANDLE_INVALID,
                  "the client could not register");
    UT_ASSERT_MSG(clientSimGetScenarioRulesCount(cs) == 2,
                  "the joiner holds %d rule(s), wanted the manifest's 2",
                  clientSimGetScenarioRulesCount(cs));

    row = sjRowOf(cs, (int)SIM_RULE_tank_reload_ticks);
    UT_ASSERT_MSG(row >= 0, "the joiner's set does not name tank_reload_ticks");
    UT_ASSERT_MSG(clientSimGetScenarioRuleValue(cs, row) == 5.0,
                  "the joiner reads tank_reload_ticks as %f, wanted 5",
                  clientSimGetScenarioRuleValue(cs, row));

    row = sjRowOf(cs, (int)SIM_RULE_tank_full_shells);
    UT_ASSERT_MSG(row >= 0, "the joiner's set does not name tank_full_shells");
    UT_ASSERT_MSG(clientSimGetScenarioRuleValue(cs, row) == 80.0,
                  "the joiner reads tank_full_shells as %f, wanted 80",
                  clientSimGetScenarioRuleValue(cs, row));

    /* An index past the set names no rule, whatever the array behind it
       holds. */
    UT_ASSERT_MSG(clientSimGetScenarioRuleIndex(cs, 2) == -1,
                  "a row past the set named rule %d",
                  clientSimGetScenarioRuleIndex(cs, 2));

    serverSimUnregisterSubscriber(sim, handle);
    clientSimDestroy(cs);
    sjDestroy(sim);
    sjDropDir();
    return 0;
}
