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
 *       joiner into the empty lobby finds it there;
 *   (4) a joiner's start reservation is on its team's default side;
 *   (5) a balance accepted from the lobby re-picks the moved players'
 *       starts, so every tank starts the round on its new team's side;
 *   (6) the dedicated server's bot seating (bots join on no team, then the
 *       batch moves them) ends with every start on its team's side;
 *   (7) a rename resends the side the team already has, and that keeps a
 *       filled-in side filled in;
 *   (8) a round that had a human and ends with none left resets to
 *       the default pair on the way back to the lobby (east/west there,
 *       because the round restores the wide map from the file);
 *   (9) the reset puts the default back before the scenario seats its
 *       teams, so each seat reserves a start on its team's side;
 *  (10) a host alone on team 1 who drops its side leaves team 2 on south
 *       (no one is on team 2, so the two-team mirror does not act);
 *  (11) a balance keeps an unmoved player's hand-picked start that is
 *       still on its team's side, and re-picks only the moved players;
 *  (12) the shape rule on synthetic grids: east/west when the squares that
 *       are not deep sea span more columns than rows, north/south for a
 *       tall or square map, the sea around an island not counted;
 *  (13) the live map picks the pair at start-up and at the reset;
 *  (14) a map change re-picks the untouched default pair for the new map
 *       and keeps every pair the host chose;
 *  (15) the untouched default follows every map change, tall and wide in
 *       turn, not only the first;
 *  (16) the host picking the side a default team already shows names it,
 *       so a map change keeps the pair; a pool pick does not.
 *
 * Everard Island is wider than tall, so tests (1) to (11), (14) to (16) make it
 * tall first (make_map_tall: two grass squares out in the sea, the island
 * untouched) and run on a north/south lobby.
 *
 * Commands go through serverSimApplyCommand under the threads mutex, as in
 * test_lobby_team_side_dispatch.c. Start layouts are injected the way that
 * file does it: each start square forced to deep sea, and a start's side
 * read back through startsGetMaxs + startSideMaskFor. Rounds start through
 * serverSimStartGameInPlace, which keeps the injected layout.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"              /* mapSetPos */
#include "bolo_rand.h"             /* bolo_srand */
#include "client_command.h"        /* CMD_LOBBY_TEAM_META, CMD_BALANCE_APPLY,
                                    * CmdResult */
#include "control_event.h"         /* ControlEvent, CTRL_LOBBY_TEAM_META */
#include "everard_map.h"
#include "starts.h"                /* startsGetNumStarts, startsGetMaxs */
#include "start_sides.h"           /* START_SIDE_*, startSideMaskFor */
#include "tank.h"                  /* tankGetWorld */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->teams */
#include "server_sim_lifecycle.h"  /* serverSimApplyInstanceConfig, serverSimSetTeam,
                                    * serverSimStartGame, serverSimReturnToLobby,
                                    * serverSimFillLobbyTeamMetaEvent */
#include "server_sim_join.h"       /* serverSimRepickAllLobbyStarts */
#include "server_sim_scenario.h"   /* serverSimSetScenarioLobbyTemplate */
#include "server_lifecycle.h"      /* ServerInstanceConfig */
#include "threads.h"
#include "test_harness.h"

/* An Everard Island ServerSim as the map file has it, before any boot.
 * Its squares that are not deep sea span more columns than rows, so a
 * lobby on it opens east/west. */
static ServerSim *make_wide_sim(void) {
    BYTE emap[6000] = E_MAP;
    return serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                     gameOpen, false, 0, -1);
}

/* The bounding box of the live map's squares that are not deep sea, read
 * the way the lobby reads it. */
static bool land_bounds(const ServerSim *sim, int *minX, int *minY,
                        int *maxX, int *maxY) {
    static BYTE terrain[SERVER_SIM_TERRAIN_BYTES];
    if (!serverSimGetMapTerrainBuffer(sim, terrain, sizeof(terrain))) return false;
    return startSideLandBounds(terrain, MAP_ARRAY_SIZE, MAP_ARRAY_SIZE,
                               MAP_ARRAY_SIZE, minX, minY, maxX, maxY);
}

/* One grass square at (x, y), deep sea or not before. */
static void put_land(ServerSim *sim, int x, int y) {
    GameSim *gs = &sim->sim;
    mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, GRASS, FALSE, TRUE);
}

/* Two grass squares out in the sea, one near the top edge and one near the
 * bottom, in a column the island already spans. The island is not touched
 * and gets no wider, but the land now spans 205 rows, more than its width,
 * so the lobby's default is north/south. The two rows are centred on 126,
 * where mapRead recentres a map's land, so a tall map saved and loaded
 * again keeps both squares inside the map. */
#define TALL_TOP_Y    24
#define TALL_BOTTOM_Y 228
static void make_map_tall(ServerSim *sim) {
    int minX;
    int minY;
    int maxX;
    int maxY;
    if (!land_bounds(sim, &minX, &minY, &maxX, &maxY)) return;
    put_land(sim, minX, TALL_TOP_Y);
    put_land(sim, minX, TALL_BOTTOM_Y);
}

/* An Everard Island ServerSim made tall (make_map_tall), before any boot.
 * The tests below were written for a north/south lobby. */
static ServerSim *make_sim(void) {
    ServerSim *sim = make_wide_sim();
    if (sim != NULL) make_map_tall(sim);
    return sim;
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

/* Column layout: four due-north starts (y=40) and four due-south starts
 * (y=210) near x=125, so the sector test gives N or S alone. bbox x
 * 100..150, y 40..210, so the halves split at y=125. */
static const BYTE k_col_x[4] = { 100, 115, 135, 150 };
#define COL_NORTH_Y 40
#define COL_SOUTH_Y 210
#define COL_MID_Y   125

/* Put the column layout in place of the map's own starts. Pills and bases
 * go too, so nothing else shapes the start bounding box. */
static void inject_columns(ServerSim *sim) {
    GameSim *gs = &sim->sim;
    int i;
    gs->pb->numPills  = 0;
    gs->bs->numBases  = 0;
    gs->ss->numStarts = 0;
    for (i = 0; i < 8; i++) {
        BYTE idx = gs->ss->numStarts++;
        BYTE x = k_col_x[i % 4];
        BYTE y = (i < 4) ? COL_NORTH_Y : COL_SOUTH_Y;
        mapSetPos(gs, &gs->mp, x, y, DEEP_SEA, FALSE, TRUE);
        gs->ss->item[idx].x   = x;
        gs->ss->item[idx].y   = y;
        gs->ss->item[idx].dir = 0;
    }
}

/* Cross layout: two starts at each compass point, west and east listed
 * first (bbox x 40..210, y 60..190). With no side set, the first seat
 * takes a west start, its teammates cluster there and the other team goes
 * to the far side, east; only the north/south default puts them on the
 * other axis. */
static const BYTE k_cross[8][2] = {
    {  40, 120 }, {  40, 130 },     /* west  */
    { 210, 120 }, { 210, 130 },     /* east  */
    { 120,  60 }, { 130,  60 },     /* north */
    { 120, 190 }, { 130, 190 },     /* south */
};

static void inject_cross(ServerSim *sim) {
    GameSim *gs = &sim->sim;
    int i;
    gs->pb->numPills  = 0;
    gs->bs->numBases  = 0;
    gs->ss->numStarts = 0;
    for (i = 0; i < 8; i++) {
        BYTE idx = gs->ss->numStarts++;
        mapSetPos(gs, &gs->mp, k_cross[i][0], k_cross[i][1], DEEP_SEA, FALSE, TRUE);
        gs->ss->item[idx].x   = k_cross[i][0];
        gs->ss->item[idx].y   = k_cross[i][1];
        gs->ss->item[idx].dir = 0;
    }
}

/* Side mask of a 1-based start, computed the way the batch does it. */
static BYTE mask_of(ServerSim *sim, BYTE idx1) {
    int leftPos;
    int rightPos;
    int topPos;
    int bottomPos;
    GameSim *gs = &sim->sim;
    startsGetMaxs(&gs->ss, &leftPos, &rightPos, &topPos, &bottomPos);
    return startSideMaskFor(gs->ss->item[idx1 - 1].x, gs->ss->item[idx1 - 1].y,
                            leftPos, topPos, rightPos, bottomPos);
}

/* A slot's reservation is a real start with the given side bit. */
static bool start_on(ServerSim *sim, BYTE slot, BYTE sideBit) {
    BYTE idx1 = sim->lobbyPlayers[slot].startIdx;
    if (idx1 < 1 || idx1 > startsGetNumStarts(&sim->sim.ss)) return false;
    return (mask_of(sim, idx1) & sideBit) != 0;
}

/* A slot's reservation is on the side of the team it is on now: team 1
 * north, team 2 south. */
static bool start_on_team_side(ServerSim *sim, BYTE slot) {
    BYTE team = sim->lobbyPlayers[slot].teamNumber;
    return start_on(sim, slot, team == 1 ? START_SIDE_BIT_N : START_SIDE_BIT_S);
}

/* A slot's tank is in the half of the layout that belongs to its team. */
static bool tank_on_team_side(ServerSim *sim, BYTE slot) {
    WORLD wx;
    WORLD wy;
    int my;
    BYTE team = sim->lobbyPlayers[slot].teamNumber;
    if (sim->sim.tanks[slot] == NULL) return false;
    tankGetWorld(&sim->sim.tanks[slot], &wx, &wy);
    my = (int)(wy >> M_W_SHIFT_SIZE);
    return team == 1 ? (my < COL_MID_Y) : (my > COL_MID_Y);
}

/* Join a slot flagged as a bot the way the lobby marks one, so no brain
 * file is needed. */
static void add_bot(ServerSim *sim, BYTE slot, BYTE team) {
    add_human(sim, slot, team);
    sim->lobbyPlayers[slot].isBot = true;
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

/* (4) A joiner's reservation is on its team's default side. */
int run_lobby_default_sides_joiner_start_on_side(void) {
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    inject_columns(sim);

    add_human(sim, 0, 1);
    add_human(sim, 1, 2);
    add_human(sim, 2, 1);
    add_human(sim, 3, 2);
    UT_ASSERT_MSG(start_on(sim, 0, START_SIDE_BIT_N) && start_on(sim, 2, START_SIDE_BIT_N),
                  "team 1 joiners must reserve north starts (got %u, %u)",
                  sim->lobbyPlayers[0].startIdx, sim->lobbyPlayers[2].startIdx);
    UT_ASSERT_MSG(start_on(sim, 1, START_SIDE_BIT_S) && start_on(sim, 3, START_SIDE_BIT_S),
                  "team 2 joiners must reserve south starts (got %u, %u)",
                  sim->lobbyPlayers[1].startIdx, sim->lobbyPlayers[3].startIdx);
    serverSimDestroy(sim);
    return 0;
}

/* (5) A balance accepted from the lobby swaps the teams. Every moved
 *     player's start follows it to the new team's side, and the round puts
 *     every tank there. */
int run_lobby_default_sides_balance_repicks(void) {
    ClientCommand cmd;
    CmdResult r;
    BYTE i;
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    inject_columns(sim);

    add_human(sim, 0, 1);   /* the host */
    add_human(sim, 1, 1);
    add_human(sim, 2, 2);
    add_human(sim, 3, 2);
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(start_on_team_side(sim, i), "setup: slot %u on side", i);
    }

    /* The proposal WBN would send back: the two teams swapped. */
    memset(sim->balanceProposal.teamForSlot, 0, sizeof(sim->balanceProposal.teamForSlot));
    sim->balanceProposal.teamForSlot[0] = 2;
    sim->balanceProposal.teamForSlot[1] = 2;
    sim->balanceProposal.teamForSlot[2] = 1;
    sim->balanceProposal.teamForSlot[3] = 1;
    sim->balanceProposal.includeBots    = true;
    sim->balanceProposal.pending        = true;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_BALANCE_APPLY;
    cmd.cmdSeq = 1;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    UT_ASSERT_MSG(r == CMD_OK, "the host's balance accept must apply (got %d)", (int)r);
    UT_ASSERT_MSG(sim->lobbyPlayers[0].teamNumber == 2 &&
                  sim->lobbyPlayers[2].teamNumber == 1, "setup: teams swapped");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "a balance must not change the sides");
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(start_on_team_side(sim, i),
                      "after a balance, slot %u (team %u) must hold a start on "
                      "its new team's side (holds %u)", i,
                      sim->lobbyPlayers[i].teamNumber, sim->lobbyPlayers[i].startIdx);
    }

    bolo_srand(11);
    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(tank_on_team_side(sim, i),
                      "slot %u (team %u) must start the round on its team's side", i,
                      sim->lobbyPlayers[i].teamNumber);
    }
    serverSimDestroy(sim);
    return 0;
}

/* (6) The dedicated server adds its -allybots / -teams bots on no team and
 *     then moves them with the batch. The call servermain makes after the
 *     moves puts every start on its team's side. */
int run_lobby_default_sides_ds_bot_batch_repicks(void) {
    BYTE i;
    int offSide = 0;
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, false, false);   /* the DS config: neither flag */
    inject_columns(sim);

    for (i = 0; i < 4; i++) add_bot(sim, i, 0);
    serverSimSetTeamBatch(sim, 0, 1);
    serverSimSetTeamBatch(sim, 1, 2);
    serverSimSetTeamBatch(sim, 2, 2);
    serverSimSetTeamBatch(sim, 3, 2);
    for (i = 0; i < 4; i++) {
        if (!start_on_team_side(sim, i)) offSide++;
    }
    /* What the batch alone leaves: the picks made on no team. */
    UT_ASSERT_MSG(offSide > 0,
                  "setup: the batch alone should leave a start off its team's side");

    serverSimRepickAllLobbyStarts(sim);
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(start_on_team_side(sim, i),
                      "bot %u (team %u) must hold a start on its team's side "
                      "(holds %u)", i, sim->lobbyPlayers[i].teamNumber,
                      sim->lobbyPlayers[i].startIdx);
    }

    bolo_srand(11);
    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(tank_on_team_side(sim, i),
                      "bot %u (team %u) must start the round on its team's side", i,
                      sim->lobbyPlayers[i].teamNumber);
    }
    serverSimDestroy(sim);
    return 0;
}

/* (7) A rename resends the side the team already shows. That is not a
 *     choice of side, so the default north stays filled in and still
 *     follows team 2. */
int run_lobby_default_sides_rename_keeps_fill(void) {
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    add_human(sim, 1, 2);

    serverSimSetTeamMeta(sim, 1, sim->teams[1].color, sim->teams[1].namingPool,
                         START_SIDE_N, (const uint8_t *)"Reds", 4);
    UT_ASSERT_MSG(sim->teams[1].startSide == START_SIDE_N &&
                  sim->teams[1].sideAutoFilled,
                  "a rename must keep team 1's side filled in");

    UT_ASSERT(apply_team_side(sim, 0, 2, START_SIDE_E) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_W, START_SIDE_E),
                  "after a rename, team 1 must still follow team 2 to west "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);
    serverSimDestroy(sim);
    return 0;
}

/* (8) A round that had a human and ends with none left goes back to a
 *     fresh lobby, the default pair included. The round puts the map back
 *     the way the file has it, and Everard Island as the file has it is
 *     wide, so the reset's pair is east/west. The host's own north/south
 *     (both named) must not survive it. */
int run_lobby_default_sides_round_end_reset(void) {
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    /* Both named: team 2 has no players, so no mirror fills it in. East
     * first, so north is a change and clears team 1's mark. */
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_E) == CMD_OK);
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_N) == CMD_OK);
    UT_ASSERT(apply_team_side(sim, 0, 2, START_SIDE_E) == CMD_OK);
    UT_ASSERT(apply_team_side(sim, 0, 2, START_SIDE_S) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S), "setup: host's sides");
    UT_ASSERT_MSG(!sim->teams[1].sideAutoFilled && !sim->teams[2].sideAutoFilled,
                  "setup: both sides named");

    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "setup: the game must start");
    /* The tick sets this while a human is in the round. */
    sim->roundHadHuman = true;
    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "a departure mid-round must not touch the sides");

    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby, "setup: back in the lobby");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a round that ends with no human left must reset to the "
                  "pair for the restored (wide) map, east/west (got %u/%u)",
                  sim->teams[1].startSide, sim->teams[2].startSide);
    UT_ASSERT_MSG(sim->teams[1].sideAutoFilled && sim->teams[2].sideAutoFilled,
                  "the restored pair is filled-in, not named");
    serverSimDestroy(sim);
    return 0;
}

/* (9) The reset seats the scenario's teams again. The default goes back in
 *     before that, so every seat reserves a start on its team's side. On
 *     the cross layout, seats made with no side would split west/east, and
 *     nothing re-picks them once the sides are set. */
int run_lobby_default_sides_reset_before_seating(void) {
    ScnLobbyTemplate t;
    BYTE i;
    int seats = 0;
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    inject_cross(sim);
    add_human(sim, 0, 1);
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_W) == CMD_OK);
    UT_ASSERT(apply_team_side(sim, 0, 2, START_SIDE_E) == CMD_OK);

    /* Two held (unfielded) seats on each of teams 1 and 2, so no brain. */
    memset(&t, 0, sizeof(t));
    t.numTeams = 2;
    t.teams[0].id      = 1;
    t.teams[0].bots    = 2;
    t.teams[0].maxBots = 2;
    t.teams[0].fielded = false;
    t.teams[1].id      = 2;
    t.teams[1].bots    = 2;
    t.teams[1].maxBots = 2;
    t.teams[1].fielded = false;
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);

    /* The last human leaves, which is what reaches the reset. */
    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0, "setup: lobby empty");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "setup: the reset restores north/south");

    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || !sim->lobbyPlayers[i].keepSeat) continue;
        seats++;
        UT_ASSERT_MSG(start_on_team_side(sim, i),
                      "seat %u (team %u) must reserve a start on its team's "
                      "default side (holds %u)", i,
                      sim->lobbyPlayers[i].teamNumber, sim->lobbyPlayers[i].startIdx);
    }
    UT_ASSERT_MSG(seats == 4, "setup: the reset re-seats 4 seats (got %d)", seats);
    serverSimDestroy(sim);
    return 0;
}

/* (10) The rule as it stands for a host alone on team 1: dropping team 1's
 *      side leaves team 2 on its default south. The two-team mirror only
 *      acts when exactly one other team has players, and team 2 has none. */
int run_lobby_default_sides_solo_host_any_keeps_south(void) {
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);

    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_ANY) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_ANY, START_SIDE_S),
                  "a solo host dropping team 1's side leaves team 2 on south "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);
    UT_ASSERT_MSG(sim->teams[2].sideAutoFilled,
                  "team 2's south is still the filled-in default");
    serverSimDestroy(sim);
    return 0;
}

/* (11) A balance keeps a start a player chose by hand when the player did
 *      not move and the start is still on the team's side. Only the moved
 *      players' starts are picked again. */
int run_lobby_default_sides_balance_keeps_hand_pick(void) {
    ClientCommand cmd;
    CmdResult r;
    BYTE i;
    BYTE handPick = 0;
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    inject_columns(sim);

    add_human(sim, 0, 1);   /* the host */
    add_human(sim, 1, 1);
    add_human(sim, 2, 2);
    add_human(sim, 3, 2);
    add_human(sim, 4, 2);

    /* Slot 1 claims the north start that nobody holds and that sits
     * farthest along the row, not the one the picker gave it. */
    for (i = 4; i >= 1 && handPick == 0; i--) {
        BYTE k;
        bool held = false;
        for (k = 0; k < 5; k++) {
            if (sim->lobbyPlayers[k].startIdx == i) held = true;
        }
        if (!held) handPick = i;
    }
    UT_ASSERT_MSG(handPick != 0, "setup: a free north start");
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_CLAIM_START;
    cmd.cmdSeq = 1;
    cmd.u.lobbyClaimStart.targetSlot = 1;
    cmd.u.lobbyClaimStart.startIdx   = handPick;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 1, &cmd);
    threadsReleaseMutex();
    UT_ASSERT_MSG(r == CMD_OK && sim->lobbyPlayers[1].startIdx == handPick,
                  "setup: slot 1 claims start %u (got %d, holds %u)", handPick,
                  (int)r, sim->lobbyPlayers[1].startIdx);

    /* The balance moves slot 4 to team 1 and leaves everyone else. */
    memset(sim->balanceProposal.teamForSlot, 0, sizeof(sim->balanceProposal.teamForSlot));
    sim->balanceProposal.teamForSlot[0] = 1;
    sim->balanceProposal.teamForSlot[1] = 1;
    sim->balanceProposal.teamForSlot[2] = 2;
    sim->balanceProposal.teamForSlot[3] = 2;
    sim->balanceProposal.teamForSlot[4] = 1;
    sim->balanceProposal.includeBots    = true;
    sim->balanceProposal.pending        = true;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_BALANCE_APPLY;
    cmd.cmdSeq = 2;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    UT_ASSERT_MSG(r == CMD_OK, "the host's balance accept must apply (got %d)", (int)r);
    UT_ASSERT_MSG(sim->lobbyPlayers[1].startIdx == handPick,
                  "an unmoved player's hand-picked start must survive a "
                  "balance (had %u, holds %u)", handPick,
                  sim->lobbyPlayers[1].startIdx);
    for (i = 0; i < 5; i++) {
        UT_ASSERT_MSG(start_on_team_side(sim, i),
                      "after a balance, slot %u (team %u) must hold a start on "
                      "its team's side (holds %u)", i,
                      sim->lobbyPlayers[i].teamNumber, sim->lobbyPlayers[i].startIdx);
    }

    bolo_srand(11);
    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    for (i = 0; i < 5; i++) {
        UT_ASSERT_MSG(tank_on_team_side(sim, i),
                      "slot %u (team %u) must start the round on its team's side", i,
                      sim->lobbyPlayers[i].teamNumber);
    }
    serverSimDestroy(sim);
    return 0;
}

/* (12) The shape rule on synthetic grids. Everything is deep sea except
 *      the rectangles painted in: more columns than rows gives east/west,
 *      more rows than columns or a tie gives north/south, and the sea
 *      around an island does not count. */
static BYTE s_grid[MAP_ARRAY_SIZE * MAP_ARRAY_SIZE];

static void grid_clear(void) {
    memset(s_grid, DEEP_SEA, sizeof(s_grid));
}

/* Paint terrain over columns x0..x1 and rows y0..y1, inclusive. */
static void grid_paint(int x0, int y0, int x1, int y1, BYTE terrain) {
    int x;
    int y;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            s_grid[(y * MAP_ARRAY_SIZE) + x] = terrain;
        }
    }
}

static bool grid_is_east_west(void) {
    return startSideDefaultIsEastWest(s_grid, MAP_ARRAY_SIZE, MAP_ARRAY_SIZE,
                                      MAP_ARRAY_SIZE);
}

int run_lobby_default_sides_shape_rule(void) {
    int minX;
    int minY;
    int maxX;
    int maxY;

    /* A wide island in open sea: 81 columns by 31 rows. */
    grid_clear();
    grid_paint(90, 110, 170, 140, GRASS);
    UT_ASSERT_MSG(startSideLandBounds(s_grid, MAP_ARRAY_SIZE, MAP_ARRAY_SIZE,
                                      MAP_ARRAY_SIZE, &minX, &minY, &maxX, &maxY),
                  "a grid with land has bounds");
    UT_ASSERT_MSG(minX == 90 && maxX == 170 && minY == 110 && maxY == 140,
                  "the bounds are the island's alone, not the sea around it "
                  "(got x %d..%d, y %d..%d)", minX, maxX, minY, maxY);
    UT_ASSERT_MSG(grid_is_east_west(), "a wide island must give east/west");

    /* A tall island: 31 columns by 81 rows. */
    grid_clear();
    grid_paint(110, 90, 140, 170, GRASS);
    UT_ASSERT_MSG(!grid_is_east_west(), "a tall island must give north/south");

    /* A square island: 61 by 61. The tie keeps north/south. */
    grid_clear();
    grid_paint(100, 100, 160, 160, GRASS);
    UT_ASSERT_MSG(!grid_is_east_west(), "a square island must give north/south");

    /* One column more than rows tips it to east/west. */
    grid_clear();
    grid_paint(100, 100, 161, 160, GRASS);
    UT_ASSERT_MSG(grid_is_east_west(), "one column wider than tall must give east/west");

    /* Every terrain but deep sea counts: shallow river, mined grass and a
     * boat stretch the box as land does. */
    grid_clear();
    grid_paint(120, 120, 130, 130, GRASS);
    grid_paint(40, 125, 40, 125, RIVER);
    grid_paint(210, 125, 210, 125, MINE_GRASS);
    UT_ASSERT_MSG(grid_is_east_west(), "river and mined squares count as playable");
    grid_clear();
    grid_paint(120, 120, 130, 130, GRASS);
    grid_paint(125, 30, 125, 30, BOAT);
    UT_ASSERT_MSG(!grid_is_east_west(), "a boat square counts as playable");

    /* A deep lake inside a wide island changes nothing. */
    grid_clear();
    grid_paint(60, 120, 200, 135, SWAMP);
    grid_paint(100, 124, 160, 131, DEEP_SEA);
    UT_ASSERT_MSG(grid_is_east_west(), "a deep lake inside a wide island changes nothing");

    /* All deep sea: no bounds, and north/south. */
    grid_clear();
    UT_ASSERT_MSG(!startSideLandBounds(s_grid, MAP_ARRAY_SIZE, MAP_ARRAY_SIZE,
                                       MAP_ARRAY_SIZE, NULL, NULL, NULL, NULL),
                  "an all-sea grid has no bounds");
    UT_ASSERT_MSG(!grid_is_east_west(), "an all-sea grid must give north/south");
    return 0;
}

/* (13) The live map's shape picks the pair at start-up and at the reset:
 *      Everard Island as the file has it is wide and opens with team 1 east
 *      and team 2 west, the order the compass sets that axis in; made tall
 *      it opens north/south; made exactly square it opens north/south. */
int run_lobby_default_sides_map_shape(void) {
    ServerSim *sim;
    int minX;
    int minY;
    int maxX;
    int maxY;

    sim = make_wide_sim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(land_bounds(sim, &minX, &minY, &maxX, &maxY));
    UT_ASSERT_MSG(maxX - minX > maxY - minY,
                  "setup: Everard Island is wider than tall (x %d..%d, y %d..%d)",
                  minX, maxX, minY, maxY);
    boot_sim(sim, true, false);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a wide map must open on team 1 east, team 2 west (got %u/%u)",
                  sim->teams[1].startSide, sim->teams[2].startSide);
    UT_ASSERT_MSG(sim->teams[1].sideAutoFilled && sim->teams[2].sideAutoFilled,
                  "the east/west default is filled-in, not named");

    /* The reset reads the map again. Change the squares directly (no
     * map-change path), then let the last human leave. */
    add_human(sim, 0, 1);
    make_map_tall(sim);
    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "the reset must pick the pair for the map as it is now "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);
    serverSimDestroy(sim);

    /* The dedicated server's zeroed config on a wide map. */
    sim = make_wide_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, false, false);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a dedicated-server lobby on a wide map opens east/west");
    serverSimDestroy(sim);

    /* Exactly square: one land square below the island makes the rows
     * span match the columns span. */
    sim = make_wide_sim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(land_bounds(sim, &minX, &minY, &maxX, &maxY));
    UT_ASSERT_MSG(minY + (maxX - minX) < MAP_MINE_EDGE_BOTTOM,
                  "setup: room for the square's bottom row");
    put_land(sim, minX, minY + (maxX - minX));
    UT_ASSERT(land_bounds(sim, &minX, &minY, &maxX, &maxY));
    UT_ASSERT_MSG(maxX - minX == maxY - minY, "setup: the land is square");
    boot_sim(sim, true, false);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "a square map keeps north/south (got %u/%u)",
                  sim->teams[1].startSide, sim->teams[2].startSide);
    serverSimDestroy(sim);

    /* No lobby, no sides, whatever the shape. */
    sim = make_wide_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, false, true);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_ANY, START_SIDE_ANY),
                  "a -nolobby start on a wide map keeps no sides");
    serverSimDestroy(sim);
    return 0;
}

/* Reload Everard Island (wide) into a lobby through the map-change path. */
static bool reload_wide(ServerSim *sim) {
    BYTE emap[6000] = E_MAP;
    bool ok;
    threadsWaitForMutex();
    ok = serverSimReloadCompressedInMemory(sim, emap, E_MAP_LEN, "Everard Island") ? true : false;
    threadsReleaseMutex();
    return ok;
}

/* (14) A map change in the lobby: the untouched default pair follows the
 *      new map's shape; a pair the host chose, one the mirror filled in
 *      against a named side included, is kept. */
int run_lobby_default_sides_map_change(void) {
    ServerSim *sim;
    BYTE i;

    /* Untouched default: tall map, north/south, then a wide map. */
    sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    add_human(sim, 1, 2);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S), "setup: tall map north/south");
    UT_ASSERT_MSG(reload_wide(sim), "setup: the map reload must succeed");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "the untouched default must follow a wide new map to "
                  "east/west (got %u/%u)",
                  sim->teams[1].startSide, sim->teams[2].startSide);
    UT_ASSERT_MSG(sim->teams[1].sideAutoFilled && sim->teams[2].sideAutoFilled,
                  "the re-picked default stays filled-in");
    /* Every reservation is a live start its team's new side allows. */
    for (i = 0; i < 2; i++) {
        BYTE idx1 = sim->lobbyPlayers[i].startIdx;
        BYTE side = sim->teams[sim->lobbyPlayers[i].teamNumber].startSide;
        UT_ASSERT_MSG(idx1 >= 1 && idx1 <= startsGetNumStarts(&sim->sim.ss),
                      "slot %u keeps a start after the map change", (unsigned)i);
        UT_ASSERT_MSG(startSideAccepts(mask_of(sim, idx1), side),
                      "slot %u's start is on its team's new side", (unsigned)i);
    }
    /* The mirror still treats the re-picked pair as a pair. */
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_N) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "team 2's filled-in west must follow team 1 to south");
    serverSimDestroy(sim);

    /* The host chose north/south: team 1 to east and back to north names
     * team 1's side, and the mirror fills team 2 in at south. Kept. */
    sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    add_human(sim, 1, 2);
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_E) == CMD_OK);
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_N) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S), "setup: host's north/south");
    UT_ASSERT_MSG(!sim->teams[1].sideAutoFilled && sim->teams[2].sideAutoFilled,
                  "setup: team 1 named, team 2 filled in");
    UT_ASSERT_MSG(reload_wide(sim), "setup: the map reload must succeed");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "a pair the host chose must survive a map change "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);
    serverSimDestroy(sim);

    /* Both named by the host: kept. */
    sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_W) == CMD_OK);
    UT_ASSERT(apply_team_side(sim, 0, 2, START_SIDE_E) == CMD_OK);
    UT_ASSERT_MSG(reload_wide(sim), "setup: the map reload must succeed");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_W, START_SIDE_E),
                  "two named sides must survive a map change");
    serverSimDestroy(sim);

    /* Both taken back to no side by the host: kept at no side. */
    sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    add_human(sim, 1, 2);
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_ANY) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_ANY, START_SIDE_ANY), "setup: custom starts");
    UT_ASSERT_MSG(reload_wide(sim), "setup: the map reload must succeed");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_ANY, START_SIDE_ANY),
                  "custom starts must survive a map change");
    serverSimDestroy(sim);
    return 0;
}

/* Everard Island made tall (make_map_tall), saved the way a map upload
 * sends it. *outLen is the compressed length; false when it failed. */
static bool tall_map_bytes(BYTE *out, int cap, int *outLen) {
    ServerSim *src = make_sim();
    GameSim *gs;
    int len;
    if (src == NULL) return false;
    gs = &src->sim;
    len = mapSaveCompressedMap(&gs->mp, &gs->pb, &gs->bs, &gs->ss, out, cap);
    serverSimDestroy(src);
    if (len <= 0) return false;
    *outLen = len;
    return true;
}

/* Load compressed map bytes into a lobby through the map-change path. */
static bool reload_bytes(ServerSim *sim, const BYTE *bytes, int len, const char *name) {
    bool ok;
    threadsWaitForMutex();
    ok = serverSimReloadCompressedInMemory(sim, bytes, len, name) ? true : false;
    threadsReleaseMutex();
    return ok;
}

/* Every connected slot in 0..n-1 holds a live start its team's side allows. */
static bool starts_follow_sides(ServerSim *sim, BYTE n) {
    BYTE i;
    for (i = 0; i < n; i++) {
        BYTE idx1 = sim->lobbyPlayers[i].startIdx;
        BYTE side = sim->teams[sim->lobbyPlayers[i].teamNumber].startSide;
        if (idx1 < 1 || idx1 > startsGetNumStarts(&sim->sim.ss)) return false;
        if (!startSideAccepts(mask_of(sim, idx1), side)) return false;
    }
    return true;
}

/* (15) The default follows every map change while it is untouched, not
 *      only the first: tall, wide, tall, wide, and a second wide map that
 *      leaves the pair as it is. Each re-pick keeps both sides filled-in,
 *      so the next change can re-pick again. Once the host names a side,
 *      later changes of either shape keep the host's pair. */
int run_lobby_default_sides_map_change_repeated(void) {
    static BYTE tallBytes[MAP_COMPRESSED_MAX_SIZE];
    int tallLen = 0;
    ServerSim *sim;
    int step;

    UT_ASSERT_MSG(tall_map_bytes(tallBytes, (int)sizeof(tallBytes), &tallLen),
                  "setup: the tall map must save");

    sim = make_wide_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    add_human(sim, 1, 2);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W), "setup: wide map east/west");

    for (step = 0; step < 4; step++) {
        bool tall = (step % 2) == 0;
        if (tall) {
            UT_ASSERT_MSG(reload_bytes(sim, tallBytes, tallLen, "Tall Everard"),
                          "setup: change %d to the tall map must succeed", step + 1);
            UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                          "change %d to a tall map must give north/south (got %u/%u)",
                          step + 1, sim->teams[1].startSide, sim->teams[2].startSide);
        } else {
            UT_ASSERT_MSG(reload_wide(sim),
                          "setup: change %d to the wide map must succeed", step + 1);
            UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                          "change %d to a wide map must give east/west (got %u/%u)",
                          step + 1, sim->teams[1].startSide, sim->teams[2].startSide);
        }
        UT_ASSERT_MSG(sim->teams[1].sideAutoFilled && sim->teams[2].sideAutoFilled,
                      "change %d must leave both sides filled-in", step + 1);
        UT_ASSERT_MSG(starts_follow_sides(sim, 2),
                      "change %d: every start is on its team's side", step + 1);
    }

    /* A second wide map: the pair is already east/west and stays so. */
    UT_ASSERT_MSG(reload_wide(sim), "setup: the second wide map must load");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a wide map after a wide map keeps east/west");
    UT_ASSERT_MSG(sim->teams[1].sideAutoFilled && sim->teams[2].sideAutoFilled,
                  "and keeps both sides filled-in");

    /* And after all that it is still the default: one more tall map. */
    UT_ASSERT_MSG(reload_bytes(sim, tallBytes, tallLen, "Tall Everard"),
                  "setup: the tall map must load again");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "the default still follows a tall map after five changes");

    /* The host names team 1 east (team 2 follows to west). From here both
     * shapes keep the host's pair. */
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_E) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W), "setup: host's east/west");
    UT_ASSERT_MSG(reload_bytes(sim, tallBytes, tallLen, "Tall Everard"),
                  "setup: the tall map must load");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a tall map must keep the host's east/west");
    UT_ASSERT_MSG(reload_wide(sim), "setup: the wide map must load");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a wide map must keep the host's east/west");
    UT_ASSERT_MSG(reload_bytes(sim, tallBytes, tallLen, "Tall Everard"),
                  "setup: the tall map must load");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a second tall map must keep the host's east/west");
    serverSimDestroy(sim);
    return 0;
}

/* A team-meta command that repeats every field the team has, bar the
 * pool when newPool is not 0xFF: the write the side combo sends when the
 * host picks the side already shown, and with a new pool the write a
 * pool pick sends. */
static CmdResult apply_team_meta_as_is(ServerSim *sim, int senderSlot,
                                       BYTE teamId, BYTE newPool) {
    ClientCommand cmd;
    CmdResult r;
    const TeamMetadata *t = &sim->teams[teamId];
    size_t nameLen = strlen(t->name);
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_TEAM_META;
    cmd.cmdSeq = 1;
    cmd.u.lobbyTeamMeta.teamId     = teamId;
    cmd.u.lobbyTeamMeta.color      = t->color;
    cmd.u.lobbyTeamMeta.namingPool = (newPool == 0xFF) ? t->namingPool : newPool;
    cmd.u.lobbyTeamMeta.startSide  = t->startSide;
    cmd.u.lobbyTeamMeta.nameLen    = (uint8_t)nameLen;
    memcpy(cmd.u.lobbyTeamMeta.name, t->name, nameLen);
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* (16) The host picks the side a default team already shows. That is a
 *      choice of side, so the pair is no longer the untouched default and
 *      a map change keeps it. A pool pick that repeats the side is not,
 *      and the default still follows the map. */
int run_lobby_default_sides_same_side_pick_kept(void) {
    ServerSim *sim;

    /* Team 1 re-picked at north on a tall map, then a wide map. */
    sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    add_human(sim, 1, 2);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S), "setup: tall map north/south");
    UT_ASSERT(apply_team_meta_as_is(sim, 0, 1, 0xFF) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "re-picking north must leave the sides as they are");
    UT_ASSERT_MSG(!sim->teams[1].sideAutoFilled,
                  "re-picking north names team 1's side");
    UT_ASSERT_MSG(sim->teams[2].sideAutoFilled,
                  "team 2's side stays filled in");
    UT_ASSERT_MSG(reload_wide(sim), "setup: the map reload must succeed");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "a side the host re-picked must survive a map change "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);
    UT_ASSERT_MSG(starts_follow_sides(sim, 2),
                  "every start is on its team's side after the map change");
    /* Team 2 still follows team 1, the way a filled-in side does. */
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_E) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "team 2's filled-in south must follow team 1 to west");
    serverSimDestroy(sim);

    /* A pool pick on team 1 repeats its side but is not a choice of side. */
    sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    add_human(sim, 1, 2);
    UT_ASSERT(apply_team_meta_as_is(sim, 0, 1,
                                    (BYTE)(sim->teams[1].namingPool + 1)) == CMD_OK);
    UT_ASSERT_MSG(sim->teams[1].sideAutoFilled && sim->teams[2].sideAutoFilled,
                  "a pool pick must keep both sides filled in");
    UT_ASSERT_MSG(reload_wide(sim), "setup: the map reload must succeed");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "after a pool pick the default must still follow a wide map "
                  "(got %u/%u)", sim->teams[1].startSide, sim->teams[2].startSide);
    serverSimDestroy(sim);
    return 0;
}
