/*
 * The start sides a fresh lobby opens with (test_lobby_default_sides.c).
 *
 * A lobby server puts team 1 on north and team 2 on south whenever its lobby
 * is fresh: when the server starts up, and when the last human leaves, which
 * is what the next person to join an empty lobby finds. Both sides are marked
 * filled-in, so the two-team mirror treats them as each other's answer. At
 * any other time a side is the host's to choose and nothing writes over it.
 * These tests pin that:
 *
 *   (1) a lobby start-up, the dedicated server's zeroed config included,
 *       opens on north/south for teams 1 and 2 and no side for the rest,
 *       and the join-time replay carries the pair; a -nolobby start stays
 *       with no sides;
 *   (2) a side the host picks survives a new joiner, a round played and
 *       the return to the lobby with people still in it; the default pair
 *       follows the host's first change the way a filled-in side does;
 *   (3) the last human leaving puts north/south back, and the first
 *       joiner into the empty lobby finds it there.
 *
 * Commands go through serverSimApplyCommand under the threads mutex, as in
 * test_lobby_team_side_dispatch.c.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_TEAM_META, CmdResult */
#include "control_event.h"         /* ControlEvent, CTRL_LOBBY_TEAM_META */
#include "everard_map.h"
#include "start_sides.h"           /* START_SIDE_* */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->teams */
#include "server_sim_lifecycle.h"  /* serverSimApplyInstanceConfig, serverSimSetTeam,
                                    * serverSimStartGame, serverSimReturnToLobby,
                                    * serverSimFillLobbyTeamMetaEvent */
#include "server_lifecycle.h"      /* ServerInstanceConfig */
#include "threads.h"
#include "test_harness.h"

/* An Everard Island ServerSim, before any boot. */
static ServerSim *make_sim(void) {
    BYTE emap[6000] = E_MAP;
    return serverSimCreateCompressed(emap, 5097, "Everard Island",
                                     gameOpen, false, 0, -1);
}

/* The operator's startup config. lobby = the flag the GUI host sets;
 * skipLobby = -nolobby; neither = what servermain hands a lobby server. */
static void boot_sim(ServerSim *sim, bool lobby, bool skipLobby) {
    ServerInstanceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.lobbyEnabled = lobby;
    cfg.skipLobby    = skipLobby;
    cfg.botAiType    = (BYTE)aiNone;
    serverSimApplyInstanceConfig(sim, &cfg);
}

static void add_human(ServerSim *sim, BYTE slot, BYTE team) {
    char name[16];
    snprintf(name, sizeof(name), "P%u", (unsigned)slot);
    serverSimAddPlayer(sim, slot, name, false);
    serverSimSetTeam(sim, slot, team);
}

static CmdResult apply_team_side(ServerSim *sim, int senderSlot, BYTE teamId, BYTE side) {
    ClientCommand cmd;
    CmdResult r;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_TEAM_META;
    cmd.cmdSeq = 1;
    cmd.u.lobbyTeamMeta.teamId     = teamId;
    cmd.u.lobbyTeamMeta.color      = 0;
    cmd.u.lobbyTeamMeta.namingPool = 0;
    cmd.u.lobbyTeamMeta.startSide  = side;
    cmd.u.lobbyTeamMeta.nameLen    = 0;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Teams 1 and 2 on sides a and b, and every other team on no side. */
static bool sides_are(const ServerSim *sim, BYTE a, BYTE b) {
    BYTE t;
    if (sim->teams[1].startSide != a) return false;
    if (sim->teams[2].startSide != b) return false;
    for (t = 3; t < MAX_TANKS; t++) {
        if (sim->teams[t].startSide != START_SIDE_ANY) return false;
    }
    return true;
}

/* (1) Start-up. */
int run_lobby_default_sides_on_startup(void) {
    ServerSim *sim;
    ControlEvent evt;

    /* The GUI host's lobby. */
    sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, /*lobby*/ true, /*skipLobby*/ false);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby, "setup: lobby boot");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "a lobby start-up must open on team 1 north, team 2 south "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);
    UT_ASSERT_MSG(sim->teams[1].sideAutoFilled && sim->teams[2].sideAutoFilled,
                  "the default pair is filled-in, not named");

    /* What the first joiner is told: the replay fill for teams 1 and 2. */
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbyTeamMetaEvent(sim, 1, &evt);
    UT_ASSERT(evt.type == CTRL_LOBBY_TEAM_META);
    UT_ASSERT_MSG(evt.u.lobbyTeamMeta.startSide == START_SIDE_N,
                  "the join replay must carry team 1 north");
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbyTeamMetaEvent(sim, 2, &evt);
    UT_ASSERT_MSG(evt.u.lobbyTeamMeta.startSide == START_SIDE_S,
                  "the join replay must carry team 2 south");
    serverSimDestroy(sim);

    /* The dedicated server's lobby: neither flag set, the sim stays in the
     * lobby serverSimCreate* made. */
    sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, false, false);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby, "setup: DS lobby boot");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "a dedicated-server lobby must open on north/south too "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);
    serverSimDestroy(sim);

    /* -nolobby: no lobby, so no default, and placement is as it was. */
    sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, false, /*skipLobby*/ true);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning, "setup: no-lobby boot");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_ANY, START_SIDE_ANY),
                  "a -nolobby start must leave every team with no side");
    serverSimDestroy(sim);
    return 0;
}

/* (2) The host's choice stands. */
int run_lobby_default_sides_host_choice_survives(void) {
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);

    add_human(sim, 0, 1);   /* the host */
    add_human(sim, 1, 2);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "joining must not change the default pair");

    /* The host moves team 1 to east. Team 2's south was filled in, so it
     * follows to west, the same answer a mirrored side gives. */
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_E) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "team 2's default south must follow team 1 to west "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);

    /* A new joiner does not bring the default back. */
    add_human(sim, 2, 1);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a join into an occupied lobby must keep the host's sides");

    /* Nor does a round played and the return to the lobby with people in it. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "setup: the game must start");
    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "setup: back in the lobby");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a new round in an occupied lobby must keep the host's sides "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);

    /* Team 1 back to no side takes the filled-in west with it: one click
     * gets custom starts, as the compass gives. */
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_ANY) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_ANY, START_SIDE_ANY),
                  "dropping team 1's side must drop the filled-in side too");

    /* A side the host names is never written over by the other team. */
    UT_ASSERT(apply_team_side(sim, 0, 2, START_SIDE_N) == CMD_OK);
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_W) == CMD_OK);
    UT_ASSERT_MSG(sim->teams[2].startSide == START_SIDE_N,
                  "a side the host named must stand");
    serverSimDestroy(sim);
    return 0;
}

/* (3) An emptied lobby is a fresh lobby again. */
int run_lobby_default_sides_reapplied_when_empty(void) {
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);

    add_human(sim, 0, 1);
    add_human(sim, 1, 2);
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_W) == CMD_OK);
    UT_ASSERT(apply_team_side(sim, 0, 2, START_SIDE_E) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_W, START_SIDE_E), "setup: host's sides");

    /* One leaves: somebody is still here, so the host's sides stay. */
    serverSimRemovePlayer(sim, 1);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_W, START_SIDE_E),
                  "a lobby with a human left in it keeps its sides");

    /* The last one leaves. */
    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0, "setup: lobby empty");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "the last human leaving must put north/south back "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);
    UT_ASSERT_MSG(sim->teams[1].sideAutoFilled && sim->teams[2].sideAutoFilled,
                  "the restored pair is filled-in, not named");

    /* The first joiner into the empty lobby finds it. */
    add_human(sim, 3, 1);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "the first join into an empty lobby must find north/south");
    serverSimDestroy(sim);
    return 0;
}
