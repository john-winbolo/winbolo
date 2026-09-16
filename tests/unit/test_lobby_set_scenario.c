/*
 * Coverage for the CMD_LOBBY_SET_SCENARIO case in
 * server_command_dispatch.c — the lobby host picking one of the scenarios
 * the server offers on its own, independently of any map.
 *
 * The sim cannot read the scenarios directory itself: that belongs to the
 * scenario library and the dependency points one way. So the handler asks
 * through the lister the host registers, and these cases register one of
 * their own over a fixed set of names rather than writing files to disk.
 * What they pin is the handler's contract: who may pick, when, which names
 * are accepted, and that a refusal leaves the previous pick alone.
 *
 * run_lobby_set_scenario_selects        — a file the directory holds is
 *                                         accepted and reads back
 * run_lobby_set_scenario_none           — an empty path clears the selection
 * run_lobby_set_scenario_refuses_unknown— a name the directory does not hold
 *                                         is refused and changes nothing
 * run_lobby_set_scenario_refuses_shape  — "..", a leading separator and a
 *                                         drive letter are refused, as are a
 *                                         non-host sender, a running round
 *                                         and a server with no lobby
 *
 * Nothing here checks what the selection does, because it does nothing yet:
 * applying it, and the settings event that says where the scenario in play
 * came from, are still to come.
 *
 * Drives serverSimApplyCommand directly, holding the threads mutex the
 * dispatcher asserts.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_SET_SCENARIO, CmdResult */
#include "everard_map.h"
#include "scenario_defs.h"         /* ScnDirEntry — what the lister fills */
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"   /* serverSimSetScenarioLister */
#include "threads.h"
#include "test_harness.h"

/* The directory these cases offer, and how many times it was read. */
typedef struct {
    int         calls;
    int         count;
    const char *files[4];
} SsDir;

static int ssList(void *ctx, const char *dir, ScnDirEntry *out, int max) {
    SsDir *d = (SsDir *)ctx;
    int    n = 0;

    (void)dir;
    d->calls++;
    while (n < d->count && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].file, sizeof(out[n].file), "%s", d->files[n]);
        snprintf(out[n].name, sizeof(out[n].name), "Scenario %d", n);
        n++;
    }
    return n;
}

/* Two scenarios on offer. A host in slot 0 — lobbyClientMayEdit(sim, 0) is
 * true — and a non-host human in slot 1. */
static ServerSim *ssLobby(SsDir *d) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 1, "Bob", false);

    memset(d, 0, sizeof(*d));
    d->count    = 2;
    d->files[0] = "wave.scenario";
    d->files[1] = "fastreload.lua";
    serverSimSetScenarioLister(sim, ssList, d);
    return sim;
}

static CmdResult ssApply(ServerSim *sim, int senderSlot, const char *relPath) {
    ClientCommand cmd;
    CmdResult     r;
    size_t        pl = (relPath == NULL) ? 0 : strlen(relPath);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_SET_SCENARIO;
    cmd.cmdSeq = 1;
    cmd.u.lobbySetScenario.relPathLen = (uint8_t)pl;
    if (pl > 0) memcpy(cmd.u.lobbySetScenario.relPath, relPath, pl);
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* ── A file the directory holds ───────────────────────────────────── */

int run_lobby_set_scenario_selects(void) {
    ServerSim *sim;
    SsDir      d;

    sim = ssLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(serverSimGetSelectedScenario(sim)[0] == '\0',
                  "setup: a new sim already had a scenario selected");

    UT_ASSERT_MSG(ssApply(sim, 0, "wave.scenario") == CMD_OK,
                  "a scenario the directory holds was refused");
    UT_ASSERT_MSG(strcmp(serverSimGetSelectedScenario(sim),
                         "wave.scenario") == 0,
                  "the sim read back \"%s\", wanted the file that was picked",
                  serverSimGetSelectedScenario(sim));
    UT_ASSERT_MSG(d.calls == 1,
                  "the directory was read %d times for one pick", d.calls);

    /* The other one, over the top of the first: picking is a commit, so the
       second replaces the first rather than adding to it. */
    UT_ASSERT_MSG(ssApply(sim, 0, "fastreload.lua") == CMD_OK,
                  "a second pick was refused");
    UT_ASSERT_MSG(strcmp(serverSimGetSelectedScenario(sim),
                         "fastreload.lua") == 0,
                  "the second pick read back \"%s\"",
                  serverSimGetSelectedScenario(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ── An empty path selects none ───────────────────────────────────── */

int run_lobby_set_scenario_none(void) {
    ServerSim *sim;
    SsDir      d;

    sim = ssLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(ssApply(sim, 0, "wave.scenario") == CMD_OK);
    UT_ASSERT(serverSimGetSelectedScenario(sim)[0] != '\0');

    UT_ASSERT_MSG(ssApply(sim, 0, "") == CMD_OK,
                  "an empty path was refused rather than selecting none");
    UT_ASSERT_MSG(serverSimGetSelectedScenario(sim)[0] == '\0',
                  "an empty path left \"%s\" selected",
                  serverSimGetSelectedScenario(sim));

    /* The one value that needs no directory: an empty path names nothing to
       look for, so the lister is never asked. */
    UT_ASSERT_MSG(d.calls == 1,
                  "selecting none read the directory: %d reads, wanted the "
                  "one from the pick before it", d.calls);

    /* And none over none is still none rather than a refusal. */
    UT_ASSERT_MSG(ssApply(sim, 0, "") == CMD_OK,
                  "selecting none twice was refused the second time");
    UT_ASSERT(serverSimGetSelectedScenario(sim)[0] == '\0');

    serverSimDestroy(sim);
    return 0;
}

/* ── A name the directory does not hold ───────────────────────────── */

int run_lobby_set_scenario_refuses_unknown(void) {
    ServerSim *sim;
    SsDir      d;
    CmdResult  r;

    sim = ssLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(ssApply(sim, 0, "wave.scenario") == CMD_OK);

    r = ssApply(sim, 0, "nosuch.scenario");
    UT_ASSERT_MSG(r == CMD_REJECT_INVALID,
                  "a name the directory does not hold answered %d, wanted "
                  "CMD_REJECT_INVALID (%d)", (int)r, (int)CMD_REJECT_INVALID);
    UT_ASSERT_MSG(strcmp(serverSimGetSelectedScenario(sim),
                         "wave.scenario") == 0,
                  "a refused pick changed the selection to \"%s\"",
                  serverSimGetSelectedScenario(sim));

    /* A server with no lister offers nothing, so every name is unknown —
       which is what a build with no scenario library answers. */
    serverSimSetScenarioLister(sim, NULL, NULL);
    UT_ASSERT_MSG(ssApply(sim, 0, "wave.scenario") == CMD_REJECT_INVALID,
                  "a server offering no scenarios accepted one");
    UT_ASSERT_MSG(strcmp(serverSimGetSelectedScenario(sim),
                         "wave.scenario") == 0,
                  "that refusal changed the selection");

    /* Selecting none still works with no lister: it asks the directory
       nothing. */
    UT_ASSERT_MSG(ssApply(sim, 0, "") == CMD_OK,
                  "selecting none needed a lister");
    UT_ASSERT(serverSimGetSelectedScenario(sim)[0] == '\0');

    serverSimDestroy(sim);
    return 0;
}

/* ── Malformed paths, a non-host sender, and a round in progress ──── */

int run_lobby_set_scenario_refuses_shape(void) {
    ServerSim  *sim;
    SsDir       d;
    const char *bad[] = {
        "../wave.scenario",   /* climbs out of the directory */
        "/wave.scenario",     /* absolute */
        "\\wave.scenario",    /* absolute, the other separator */
        "C:wave.scenario",    /* a Windows drive letter */
        "sub/../wave.scenario" /* a ".." that is not the first segment */
    };
    size_t i;

    sim = ssLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(ssApply(sim, 0, "wave.scenario") == CMD_OK);

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CmdResult r = ssApply(sim, 0, bad[i]);
        UT_ASSERT_MSG(r == CMD_REJECT_INVALID,
                      "\"%s\" answered %d, wanted CMD_REJECT_INVALID (%d)",
                      bad[i], (int)r, (int)CMD_REJECT_INVALID);
    }
    UT_ASSERT_MSG(strcmp(serverSimGetSelectedScenario(sim),
                         "wave.scenario") == 0,
                  "a refused shape changed the selection to \"%s\"",
                  serverSimGetSelectedScenario(sim));
    /* The shape is refused before the directory is read, which is what says
       a malformed name never reaches the lister. */
    UT_ASSERT_MSG(d.calls == 1,
                  "the directory was read %d times over one pick and five "
                  "malformed names", d.calls);

    /* A player who is not the host cannot pick. */
    {
        CmdResult r = ssApply(sim, 1, "fastreload.lua");
        UT_ASSERT_MSG(r == CMD_REJECT_NOT_HOST,
                      "a non-host's pick answered %d, wanted "
                      "CMD_REJECT_NOT_HOST (%d)",
                      (int)r, (int)CMD_REJECT_NOT_HOST);
    }
    UT_ASSERT_MSG(strcmp(serverSimGetSelectedScenario(sim),
                         "wave.scenario") == 0,
                  "a non-host's pick changed the selection");

    /* And nobody can pick once the round is running: it plays by what it
       started with. */
    serverSimSetState(sim, serverStateRunning);
    {
        CmdResult r = ssApply(sim, 0, "fastreload.lua");
        UT_ASSERT_MSG(r == CMD_REJECT_BAD_STATE,
                      "a pick during a running round answered %d, wanted "
                      "CMD_REJECT_BAD_STATE (%d)",
                      (int)r, (int)CMD_REJECT_BAD_STATE);
    }
    UT_ASSERT_MSG(strcmp(serverSimGetSelectedScenario(sim),
                         "wave.scenario") == 0,
                  "a pick during a running round changed the selection");

    /* Same answer from a server that runs no lobby at all: there is nowhere
       to pick from. */
    serverSimSetState(sim, serverStateLobby);
    serverSimSetLobbyEnabled(sim, false);
    UT_ASSERT_MSG(ssApply(sim, 0, "fastreload.lua") == CMD_REJECT_BAD_STATE,
                  "a pick on a server with no lobby was not refused");
    UT_ASSERT_MSG(strcmp(serverSimGetSelectedScenario(sim),
                         "wave.scenario") == 0,
                  "that refusal changed the selection");

    serverSimDestroy(sim);
    return 0;
}
