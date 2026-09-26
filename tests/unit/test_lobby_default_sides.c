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
 *       north/south on the way back to the lobby;
 *   (9) the reset puts the default back before the scenario seats its
 *       teams, so each seat reserves a start on its team's side;
 *  (10) a host alone on team 1 who drops its side leaves team 2 on south
 *       (no one is on team 2, so the two-team mirror does not act);
 *  (11) a balance keeps an unmoved player's hand-picked start that is
 *       still on its team's side, and re-picks only the moved players.
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
 *     fresh lobby, north/south included. */
int run_lobby_default_sides_round_end_reset(void) {
    ServerSim *sim = make_sim();
    UT_ASSERT(sim != NULL);
    boot_sim(sim, true, false);
    add_human(sim, 0, 1);
    /* Both named: team 2 has no players, so no mirror fills it in. */
    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_E) == CMD_OK);
    UT_ASSERT(apply_team_side(sim, 0, 2, START_SIDE_W) == CMD_OK);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W), "setup: host's sides");

    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "setup: the game must start");
    /* The tick sets this while a human is in the round. */
    sim->roundHadHuman = true;
    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_E, START_SIDE_W),
                  "a departure mid-round must not touch the sides");

    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby, "setup: back in the lobby");
    UT_ASSERT_MSG(sides_are(sim, START_SIDE_N, START_SIDE_S),
                  "a round that ends with no human left must reset to "
                  "north/south (got %u/%u)",
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
