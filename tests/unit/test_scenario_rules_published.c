/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * When CTRL_SCENARIO_RULES is written, and when it is not.
 *
 *   scenario_rules_published — a scenario attaching publishes the set its
 *       manifest holds; one detaching publishes an empty set, which is how a
 *       client is told the set it was shown has gone; and a map that has
 *       never had a scenario publishes nothing at all. The last of those is
 *       the one worth stating: an empty set and no event are different
 *       answers, and a plain map that wrote an empty one every commit would
 *       put a record into every recording that has never carried this type.
 *       Asserted by counting events of the type rather than by finding one,
 *       because what is being checked is the absence.
 *
 * Drives the real path: a scratch scenarios directory with real files in it,
 * the host's own lister on the sim, and the pick made by applying
 * CMD_LOBBY_SET_SCENARIO — so what the case proves is that the attach and the
 * detach reach the publish, not that the publish works when called.
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

static char srDir[256];
static char srFile[128];

static bool srMakeDir(const char *tag) {
    snprintf(srDir, sizeof(srDir), "wbtest_scn_rules_%s", tag);
    srFile[0] = '\0';
    /* A directory left over from a run that was killed is not a failure: the
       file below is written over whatever is in it. */
    (void)SDL_RemovePath(srDir);
    return SDL_CreateDirectory(srDir);
}

static bool srWriteMod(const char *name, const char *text) {
    char  path[512];
    FILE *f;

    snprintf(srFile, sizeof(srFile), "%s", name);
    snprintf(path, sizeof(path), "%s/%s", srDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void srDropDir(void) {
    char path[512];
    if (srFile[0] != '\0') {
        snprintf(path, sizeof(path), "%s/%s", srDir, srFile);
        remove(path);
        srFile[0] = '\0';
    }
    SDL_RemovePath(srDir);
}

/* A mod with nothing in it but a name and a rules table — the case this
 * popup exists for, because nothing else about it says what it does. Two
 * rules of obviously different kinds: an interval that gets shorter and a
 * stock that gets bigger. */
static const char kSrRulesOnly[] =
    "scenario = {\n"
    "  name = \"Fast Reload\",\n"
    "  description = \"Shells come back quicker\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  rules = {\n"
    "    tank_reload_ticks = 5,\n"
    "    tank_full_shells = 80,\n"
    "  },\n"
    "}\n";

/* A map with no script beside it: the plain map every one of these cases
 * starts on. Named but never written — the attach looks for the script, and
 * there is none. */
#define SR_PLAIN_MAP "wbtest_scn_rules_plain.map"

/* ── What a publish looks like ────────────────────────────────────── */

typedef struct {
    int          count;
    ControlEvent last;
} SrCapture;

static void srCaptureCb(void *ctx, const ControlEvent *evt) {
    SrCapture *c = (SrCapture *)ctx;
    if (evt->type == CTRL_SCENARIO_RULES) {
        c->count++;
        c->last = *evt;
    }
}

/* ── The sim ──────────────────────────────────────────────────────── */

static ScenarioHost *srSlot;

static ServerSim *srSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetState(sim, serverStateLobby);

    srSlot = NULL;
    serverSimSetScenarioDir(sim, srDir);
    scenarioHostRegisterScenarioLister(sim);
    scenarioHostFollowMap(sim, &srSlot);
    return sim;
}

static void srDestroy(ServerSim *sim) {
    scenarioHostDetach(srSlot);
    srSlot = NULL;
    serverSimDestroy(sim);
}

/* A map commit, the way the sim makes one after loading a map. */
static void srCommit(ServerSim *sim, const char *mapPath) {
    SDL_strlcpy(sim->mapFilePath, mapPath, sizeof(sim->mapFilePath));
    serverSimScenarioOnMapChanged(sim, mapPath);
    serverSimScenarioApplyLobbyRules(sim);
}

/* The host picking a scenario, through the command the lobby sends. "" is the
 * pick that selects none, which is what detaches. */
static CmdResult srSelect(ServerSim *sim, const char *file) {
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

/* Where a rule sits in the published set, or -1 when the set does not name
 * it. The manifest's rows come out of a Lua table, whose order is the
 * traversal's rather than the file's, so a case that wanted row 0 to be one
 * particular rule would be testing Lua. */
static int srRowOf(const ControlEvent *evt, int rule) {
    int i;
    for (i = 0; i < (int)evt->u.scenarioRules.count; i++) {
        if (evt->u.scenarioRules.rule[i] == (uint8_t)rule) return i;
    }
    return -1;
}

int run_scenario_rules_published(void) {
    ServerSim      *sim;
    SubscriberHandle handle;
    SrCapture       cap;
    int             row;

    UT_ASSERT(srMakeDir("published"));
    UT_ASSERT(srWriteMod("fastreload.lua", kSrRulesOnly));
    sim = srSim();
    UT_ASSERT(sim != NULL);

    /* Registration replays the state as it stands, so the capture is cleared
       after it and counts only what happens next. Nothing is attached yet, so
       the replay has nothing of this type in it either — which the count
       below is the first thing to say. */
    memset(&cap, 0, sizeof(cap));
    handle = serverSimRegisterSubscriber(sim, srCaptureCb, &cap);
    UT_ASSERT_MSG(handle != SUBSCRIBER_HANDLE_INVALID,
                  "the capture could not register");
    UT_ASSERT_MSG(cap.count == 0,
                  "a sim with no scenario replayed %d rules event(s) into a "
                  "new subscriber", cap.count);

    /* ── a plain map, which has never had a scenario ── */
    srCommit(sim, SR_PLAIN_MAP);
    UT_ASSERT_MSG(cap.count == 0,
                  "a plain map with no scenario published %d rules event(s); "
                  "a map that never had one has nothing to say", cap.count);

    /* ── the attach ── */
    UT_ASSERT_MSG(srSelect(sim, "fastreload.lua") == CMD_OK,
                  "the mod was refused");
    UT_ASSERT_MSG(sim->scenarioIdentity.source != lobbyScenarioNone,
                  "the mod did not attach");
    UT_ASSERT_MSG(cap.count == 1,
                  "attaching published %d rules event(s), wanted 1",
                  cap.count);
    UT_ASSERT_MSG(cap.last.u.scenarioRules.count == 2,
                  "the attach published %u row(s), wanted the manifest's 2",
                  (unsigned)cap.last.u.scenarioRules.count);

    row = srRowOf(&cap.last, (int)SIM_RULE_tank_reload_ticks);
    UT_ASSERT_MSG(row >= 0, "the set does not name tank_reload_ticks");
    UT_ASSERT_MSG(cap.last.u.scenarioRules.value[row] == 5.0,
                  "tank_reload_ticks was published as %f, wanted 5",
                  cap.last.u.scenarioRules.value[row]);

    row = srRowOf(&cap.last, (int)SIM_RULE_tank_full_shells);
    UT_ASSERT_MSG(row >= 0, "the set does not name tank_full_shells");
    UT_ASSERT_MSG(cap.last.u.scenarioRules.value[row] == 80.0,
                  "tank_full_shells was published as %f, wanted 80",
                  cap.last.u.scenarioRules.value[row]);

    /* ── the detach ── */
    cap.count = 0;
    UT_ASSERT_MSG(srSelect(sim, "") == CMD_OK, "selecting none was refused");
    UT_ASSERT_MSG(sim->scenarioIdentity.source == lobbyScenarioNone,
                  "the mod did not detach");
    UT_ASSERT_MSG(cap.count == 1,
                  "detaching published %d rules event(s), wanted 1",
                  cap.count);
    UT_ASSERT_MSG(cap.last.u.scenarioRules.count == 0,
                  "the detach published %u row(s), wanted an empty set",
                  (unsigned)cap.last.u.scenarioRules.count);

    /* ── and a plain map again, with the scenario already gone ── */
    cap.count = 0;
    srCommit(sim, SR_PLAIN_MAP);
    UT_ASSERT_MSG(cap.count == 0,
                  "committing a plain map with no scenario attached published "
                  "%d rules event(s)", cap.count);

    serverSimUnregisterSubscriber(sim, handle);
    srDestroy(sim);
    srDropDir();
    return 0;
}
