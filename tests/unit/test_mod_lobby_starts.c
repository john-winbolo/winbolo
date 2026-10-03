/*
 * A mod's start pick and the lobby's (test_mod_lobby_starts.c).
 *
 * Players reported that picking starts on opposite sides in the lobby did
 * nothing: both spawned on the same side, as if the starts were random. The
 * Mac Bolo Rules mod answers on_choose_start with a random clear start, and
 * the engine asks that policy ahead of the lobby's batch slot, so with the
 * mod on, the round's opening tanks ignored every lobby pick and every team
 * side. The opening placement is the lobby's; the mod's pick is for the
 * spawns after it.
 *
 *   run_mod_lobby_starts_mac_bolo_keeps_opposite_sides
 *       two humans on teams 1 and 2, sides west and east, each holding a
 *       start picked by hand on their own side: with Mac Bolo Rules on,
 *       every round opens with each tank on its own pick, round after
 *       round, and once the round is running the mod still names the
 *       start for a later spawn.
 *
 * The mod is the shipped file, read from the data/mods directory the build
 * stages beside the test binary, attached through scenarioHostAttachMod as
 * the lobby's Mods setting attaches it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"              /* gameSimChooseStart */
#include "client_command.h"        /* CMD_LOBBY_TEAM_META, CMD_LOBBY_CLAIM_START */
#include "everard_map.h"
#include "starts.h"                /* startsGetNumStarts */
#include "start_sides.h"           /* START_SIDE_*, START_SIDE_BIT_* */
#include "tank.h"                  /* tankGetWorld */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, sim->lobbyPlayers */
#include "server_sim_lifecycle.h"  /* serverSimApplyInstanceConfig, StartGame,
                                    * serverSimReturnToLobby */
#include "server_sim_join.h"       /* serverSimLobbyStartSideMask */
#include "server_lifecycle.h"      /* ServerInstanceConfig */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive */
#include "scenario_host.h"         /* scenarioHostAttachMod */
#include "threads.h"
#include "test_harness.h"

#define MLS_MOD_FILE "MacBoloRules.scenario.lua"
#define MLS_ROUNDS   6

static CmdResult mlsApply(ServerSim *sim, int sender, ClientCommand *cmd) {
    CmdResult r;
    cmd->cmdSeq = 1;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, sender, cmd);
    threadsReleaseMutex();
    return r;
}

static CmdResult mlsTeamSide(ServerSim *sim, int sender, BYTE teamId, BYTE side) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_TEAM_META;
    cmd.u.lobbyTeamMeta.teamId    = teamId;
    cmd.u.lobbyTeamMeta.startSide = side;
    return mlsApply(sim, sender, &cmd);
}

static CmdResult mlsClaim(ServerSim *sim, int sender, BYTE target, BYTE idx1) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_CLAIM_START;
    cmd.u.lobbyClaimStart.targetSlot = target;
    cmd.u.lobbyClaimStart.startIdx   = idx1;
    return mlsApply(sim, sender, &cmd);
}

/* The first start (1-based) that lies on the one side `bit` and no other,
 * 0 when the map has none. */
static BYTE mlsStartOnSide(ServerSim *sim, BYTE bit) {
    BYTE n = startsGetNumStarts(&sim->sim.ss);
    BYTE k;
    for (k = 1; k <= n; k++) {
        if (serverSimLobbyStartSideMask(sim, k) == bit) return k;
    }
    return 0;
}

/* The start (0-based) nearest where slot's tank stands; -1 with no tank.
 * startsGetStart nudges a tank off an occupied square, so the square is
 * not the answer; the nearest start is. */
static int mlsTankStart(GameSim *gs, BYTE slot) {
    WORLD wx;
    WORLD wy;
    int   mx;
    int   my;
    int   best = -1;
    int   bestDist = 0;
    BYTE  n = startsGetNumStarts(&gs->ss);
    BYTE  i;
    if (gs->tanks[slot] == NULL) return -1;
    tankGetWorld(&gs->tanks[slot], &wx, &wy);
    mx = (int)(wx >> M_W_SHIFT_SIZE);
    my = (int)(wy >> M_W_SHIFT_SIZE);
    for (i = 0; i < n; i++) {
        int dx = (int)gs->ss->item[i].x - mx;
        int dy = (int)gs->ss->item[i].y - my;
        int d  = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (best < 0 || d < bestDist) {
            best = (int)i;
            bestDist = d;
        }
    }
    return best;
}

int run_mod_lobby_starts_mac_bolo_keeps_opposite_sides(void) {
    BYTE                 emap[6000] = E_MAP;
    ServerInstanceConfig cfg;
    ServerSim           *sim;
    ScenarioHost        *h;
    GameSim             *gs;
    const char          *base;
    char                 dir[512];
    char                 err[512];
    BYTE                 west;
    BYTE                 east;
    BYTE                 named;
    int                  round;

    base = SDL_GetBasePath();
    UT_ASSERT_MSG(base != NULL, "SDL cannot say where the executable is");
    snprintf(dir, sizeof(dir), "%sdata/mods", base);

    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetActive(sim);
    gs = &sim->sim;
    /* What servermain hands a lobby server. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.botAiType = (BYTE)aiNone;
    serverSimApplyInstanceConfig(sim, &cfg);

    h = scenarioHostAttachMod(sim, dir, MLS_MOD_FILE, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "%s under \"%s\" was refused: %s",
                  MLS_MOD_FILE, dir, err);

    /* Slot 0 is the host on team 1, slot 1 joins on team 2. */
    serverSimAddPlayer(sim, 0, "A", false);
    serverSimAddPlayer(sim, 1, "B", false);
    UT_ASSERT(sim->lobbyPlayers[0].teamNumber == 1);
    UT_ASSERT(sim->lobbyPlayers[1].teamNumber == 2);

    /* The host puts team 1 on west; team 2 follows to east. Then each
     * player picks a start on their own side by hand. */
    UT_ASSERT(mlsTeamSide(sim, 0, 1, START_SIDE_W) == CMD_OK);
    UT_ASSERT_MSG(sim->teams[1].startSide == START_SIDE_W &&
                  sim->teams[2].startSide == START_SIDE_E,
                  "setup: sides are %u/%u, wanted west/east",
                  sim->teams[1].startSide, sim->teams[2].startSide);
    west = mlsStartOnSide(sim, START_SIDE_BIT_W);
    east = mlsStartOnSide(sim, START_SIDE_BIT_E);
    UT_ASSERT_MSG(west != 0 && east != 0,
                  "setup: the map needs a start on the west and one on the east");
    UT_ASSERT(mlsClaim(sim, 0, 0, west) == CMD_OK);
    UT_ASSERT(mlsClaim(sim, 1, 1, east) == CMD_OK);
    UT_ASSERT(sim->lobbyPlayers[0].startIdx == west);
    UT_ASSERT(sim->lobbyPlayers[1].startIdx == east);

    /* The mod picks at random, so one round could land on the picks by
     * chance. Several rounds in a row cannot. */
    for (round = 0; round < MLS_ROUNDS; round++) {
        int a;
        int b;
        serverSimStartGame(sim);
        UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
        a = mlsTankStart(gs, 0);
        b = mlsTankStart(gs, 1);
        UT_ASSERT_MSG(a == (int)west - 1 && b == (int)east - 1,
                      "round %d with Mac Bolo Rules on: the tanks opened on "
                      "starts %d and %d, wanted the lobby picks %u (west) and "
                      "%u (east)", round + 1, a + 1, b + 1,
                      (unsigned)west, (unsigned)east);
        if (round + 1 < MLS_ROUNDS) {
            serverSimReturnToLobby(sim);
            UT_ASSERT(sim->lobbyPlayers[0].startIdx == west);
            UT_ASSERT(sim->lobbyPlayers[1].startIdx == east);
        }
    }

    /* The mod still owns the spawns after the opening: once the round's
     * first tick has run, it names a start for a respawning tank. */
    serverSimTick(sim);
    named = MAX_STARTS;
    UT_ASSERT_MSG(gameSimChooseStart(gs, 0, &named) && named < startsGetNumStarts(&gs->ss),
                  "after the opening, Mac Bolo Rules must still name a "
                  "respawn's start");

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}
