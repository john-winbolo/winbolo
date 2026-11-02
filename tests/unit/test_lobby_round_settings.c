/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Lobby settings reach the round they are set for (test_lobby_round_settings.c).
 *
 * The lobby showed a setting the round then did not play. The time limit:
 * LST_TIME_LIMIT and LST_TIME_MINUTES wrote gameLength, and every round
 * start after a countdown (serverSimStartGame) and the return to the lobby
 * put gameLength back to originalGameLength, which only serverSimInit set.
 * A server created with no time limit played every round without one,
 * whatever the lobby said, and one created with a limit could not lose it.
 *
 *   run_lobby_time_limit_reaches_round
 *       a server created with unlimited time, set to 1 minute in the lobby:
 *       the round starts with 1 minute, the lobby it returns to still holds
 *       1 minute, and so does the round after it.
 *   run_lobby_time_limit_removed_reaches_round
 *       a server created with 10 minutes, the limit turned off in the
 *       lobby: the round starts with no limit.
 *   run_lobby_time_limit_empty_reset
 *       the host's 1 minute, then the last human leaves: the lobby is reset
 *       to the operator's settings, and the next round plays the operator's
 *       unlimited time, not the departed host's minute.
 *
 * Hidden mines: the setting lived in sim.hiddenMines, but what decides which
 * mines are drawn is the flag in the mines object, which was only built from
 * it when the world was reset. A client built its object with hidden mines
 * off and rebuilt it only on the return to the lobby, so its first round
 * showed every mine whatever the lobby said; the server's start with no
 * countdown never rebuilt its own.
 *
 *   run_lobby_hidden_mines_client_follows
 *       the client's mines object follows each lobby settings event, and a
 *       mine it has already seen stays seen when the event is sent again,
 *       as it is mid-round for other settings and to joiners.
 *   run_lobby_hidden_mines_in_place_start
 *       a server created without hidden mines, turned on in the lobby, and
 *       started with no countdown (the single-player first round): its
 *       mines object plays hidden mines.
 *   run_lobby_hidden_mines_empty_reset
 *       the host's hidden mines, then the last human leaves: the operator's
 *       visible mines come back to the mines object as well as the setting.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_command.h"
#include "client_sim.h"
#include "client_sim_control.h"    /* clientSimApplyControl */
#include "control_event.h"
#include "game_sim.h"
#include "mines.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"  /* serverSimApplyLobbySettingResult, ServerInstanceConfig */
#include "wire_limits.h"           /* LST_TIME_LIMIT, LST_TIME_MINUTES */
#include "everard_map.h"
#include "test_harness.h"

#define LRS_ONE_MINUTE ((int32_t)60 * GAME_NUMGAMETICKS_SEC)

/* A lobby server created with `gameLen` ticks of round, through the same
 * startup snapshot a real server takes (serverSimApplyInstanceConfig), with
 * one human seated so the round has someone to start for. */
static ServerSim *lrsLobbySim(int32_t gameLen) {
    BYTE emap[6000] = E_MAP;
    ServerInstanceConfig cfg;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, gameLen);
    if (sim == NULL) return NULL;
    memset(&cfg, 0, sizeof(cfg));
    cfg.lobbyEnabled = true;
    serverSimApplyInstanceConfig(sim, &cfg);
    serverSimAddPlayer(sim, 0, "Host", false);
    return sim;
}

static bool lrsSetTimeLimit(ServerSim *sim, bool on) {
    uint8_t v = on ? 1 : 0;
    return serverSimApplyLobbySettingResult(sim, LST_TIME_LIMIT, &v, 1) == CMD_OK;
}

static bool lrsSetMinutes(ServerSim *sim, uint16_t mins) {
    uint8_t v[2] = { (uint8_t)(mins >> 8), (uint8_t)(mins & 0xFF) };
    return serverSimApplyLobbySettingResult(sim, LST_TIME_MINUTES, v, 2) == CMD_OK;
}

int run_lobby_time_limit_reaches_round(void) {
    ServerSim *sim = lrsLobbySim(UNLIMITED_GAME_TIME);

    UT_ASSERT_MSG(sim != NULL, "lrsLobbySim returned NULL");
    UT_ASSERT_MSG(lrsSetMinutes(sim, 1), "1 minute was refused");
    UT_ASSERT_MSG(lrsSetTimeLimit(sim, true), "the time limit was refused");
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == LRS_ONE_MINUTE,
                  "setup: the lobby holds %d ticks, wanted %d",
                  (int)serverSimGetGameLength(sim), (int)LRS_ONE_MINUTE);

    /* The start every round after a countdown takes. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == LRS_ONE_MINUTE,
                  "the round started with %d ticks, wanted the lobby's 1 "
                  "minute (%d)", (int)serverSimGetGameLength(sim),
                  (int)LRS_ONE_MINUTE);

    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == LRS_ONE_MINUTE,
                  "back in the lobby the round length is %d ticks, wanted "
                  "the lobby's 1 minute (%d)", (int)serverSimGetGameLength(sim),
                  (int)LRS_ONE_MINUTE);

    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == LRS_ONE_MINUTE,
                  "the second round started with %d ticks, wanted %d",
                  (int)serverSimGetGameLength(sim), (int)LRS_ONE_MINUTE);

    serverSimDestroy(sim);
    return 0;
}

int run_lobby_time_limit_removed_reaches_round(void) {
    ServerSim *sim = lrsLobbySim(10 * LRS_ONE_MINUTE);

    UT_ASSERT_MSG(sim != NULL, "lrsLobbySim returned NULL");
    UT_ASSERT_MSG(lrsSetTimeLimit(sim, false), "turning the limit off was refused");

    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == UNLIMITED_GAME_TIME,
                  "the round started with %d ticks, wanted no limit (%d)",
                  (int)serverSimGetGameLength(sim), (int)UNLIMITED_GAME_TIME);

    serverSimDestroy(sim);
    return 0;
}

int run_lobby_time_limit_empty_reset(void) {
    ServerSim *sim = lrsLobbySim(UNLIMITED_GAME_TIME);

    UT_ASSERT_MSG(sim != NULL, "lrsLobbySim returned NULL");
    UT_ASSERT_MSG(lrsSetMinutes(sim, 1), "1 minute was refused");
    UT_ASSERT_MSG(lrsSetTimeLimit(sim, true), "the time limit was refused");

    /* The last human out resets the lobby to the operator's settings. */
    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0,
                  "setup: the host did not leave");
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == UNLIMITED_GAME_TIME,
                  "setup: the emptied lobby holds %d ticks, wanted the "
                  "operator's no limit", (int)serverSimGetGameLength(sim));

    serverSimAddPlayer(sim, 0, "Next", false);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == UNLIMITED_GAME_TIME,
                  "the next round started with %d ticks, wanted the "
                  "operator's no limit (%d), not the departed host's minute",
                  (int)serverSimGetGameLength(sim), (int)UNLIMITED_GAME_TIME);

    serverSimDestroy(sim);
    return 0;
}

static bool lrsSetHiddenMines(ServerSim *sim, bool on) {
    uint8_t v = on ? 1 : 0;
    return serverSimApplyLobbySettingResult(sim, LST_HIDDEN_MINES, &v, 1) == CMD_OK;
}

int run_lobby_hidden_mines_client_follows(void) {
    ClientSim *cs = clientSimAlloc();
    ControlEvent in;
    GameSim *gs;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT_MSG(minesGetAllowHiddenMines(&gs->mns) == FALSE,
                  "setup: a new client's mines object already hides mines");

    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyHiddenMines = true;
    clientSimApplyControl(cs, &in);
    UT_ASSERT_MSG(clientSimIsLobbyHiddenMines(cs),
                  "setup: the lobby did not take hidden mines on");
    UT_ASSERT_MSG(minesGetAllowHiddenMines(&gs->mns) == TRUE,
                  "the lobby shows hidden mines on, but the mines object that "
                  "draws them still has them off");

    /* A mine this client has been shown, then the event again, the way a
       round re-sends it for another setting or a joiner catches up on it. */
    minesAddItem(&gs->mns, 100, 100);
    clientSimApplyControl(cs, &in);
    UT_ASSERT_MSG(minesExistPos(&gs->mns, &gs->mp, 100, 100) == TRUE,
                  "a settings event sent again wiped a mine already seen");

    in.u.lobbySettings.lobbyHiddenMines = false;
    clientSimApplyControl(cs, &in);
    UT_ASSERT_MSG(minesGetAllowHiddenMines(&gs->mns) == FALSE,
                  "hidden mines turned off in the lobby stayed on in the "
                  "mines object");

    clientSimDestroy(cs);
    return 0;
}

int run_lobby_hidden_mines_in_place_start(void) {
    ServerSim *sim = lrsLobbySim(UNLIMITED_GAME_TIME);
    GameSim *gs;

    UT_ASSERT_MSG(sim != NULL, "lrsLobbySim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(minesGetAllowHiddenMines(&gs->mns) == FALSE,
                  "setup: the server was created with hidden mines");
    UT_ASSERT_MSG(lrsSetHiddenMines(sim, true), "hidden mines were refused");

    serverSimStartGameInPlace(sim);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(minesGetAllowHiddenMines(&gs->mns) == TRUE,
                  "the round started with no countdown has hidden mines off "
                  "in its mines object, though the lobby set them on");

    serverSimDestroy(sim);
    return 0;
}

int run_lobby_hidden_mines_empty_reset(void) {
    ServerSim *sim = lrsLobbySim(UNLIMITED_GAME_TIME);
    GameSim *gs;

    UT_ASSERT_MSG(sim != NULL, "lrsLobbySim returned NULL");
    UT_ASSERT_MSG(lrsSetHiddenMines(sim, true), "hidden mines were refused");

    /* The last human out resets the lobby to the operator's settings. */
    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0,
                  "setup: the host did not leave");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs->hiddenMines == FALSE,
                  "setup: the emptied lobby kept the host's hidden mines");
    UT_ASSERT_MSG(minesGetAllowHiddenMines(&gs->mns) == FALSE,
                  "the emptied lobby is back to the operator's visible mines, "
                  "but its mines object still hides them");

    serverSimDestroy(sim);
    return 0;
}
