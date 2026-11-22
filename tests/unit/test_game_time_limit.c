/*
 * The round's time limit and start delay run in real time.
 *
 * Both are kept in 20 ms frames, 50 a second (GAME_NUMGAMETICKS_SEC): the
 * lobby, the dedicated server's -limit and -delay and every reader of the
 * value convert minutes and seconds at that rate. A running frame is two
 * half-steps, and each clock used to come down on both, so a round set to
 * 30 minutes ended after 15 and a 10-second delay lasted 5. These cases run
 * a sim frame by frame and pin the frame each clock runs out on:
 *
 *   1. A round of L frames ends on frame L, not L/2, and the time left reads
 *      L - n after n frames.
 *   2. A start delay of D frames holds the round for D frames, and the round
 *      it holds ends on frame D + L.
 *   3. The lobby's own time-limit settings give a round of minutes * 60 * 50
 *      frames, which is minutes * 60 seconds of frames, and the round start
 *      keeps that length instead of putting back the one the server was
 *      created with.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* gameLength, startDelay */
#include "server_sim_lifecycle.h"  /* serverSimStartGame */
#include "wire_limits.h"           /* LST_TIME_LIMIT, LST_TIME_MINUTES */
#include "game_sim.h"              /* GAME_NUMGAMETICKS_SEC */
#include "everard_map.h"           /* E_MAP */
#include "test_harness.h"

/* One minute of frames, and five seconds of them. */
#define GT_MINUTE_FRAMES (60 * GAME_NUMGAMETICKS_SEC)
#define GT_DELAY_FRAMES  (5 * GAME_NUMGAMETICKS_SEC)

/* A running round with no players, so nothing but the clock ends it. */
static ServerSim *gtRunningSim(int32_t startDelay, int32_t gameLen) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, startDelay,
                                               gameLen);
    if (sim == NULL) return NULL;
    serverSimSetQuitOnWin(sim, false);
    serverSimStartGame(sim);
    return sim;
}

/* Run frames until the round leaves the running state, at most limit of
 * them. Returns the number of the frame it ended on, or -1 if it never did. */
static int gtFramesUntilOver(ServerSim *sim, int limit) {
    int frame;
    for (frame = 1; frame <= limit; frame++) {
        serverSimTick(sim);
        if (serverSimGetState(sim) != serverStateRunning) {
            return frame;
        }
    }
    return -1;
}

/* ================================================================
 * 1. A one-minute round lasts a minute of frames.
 * ================================================================ */
int run_game_time_limit_counts_frames(void) {
    ServerSim *sim = gtRunningSim(0, GT_MINUTE_FRAMES);
    int i, ended;

    UT_ASSERT_MSG(sim != NULL, "gtRunningSim returned NULL");
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "setup: the round must be running, state %d",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == GT_MINUTE_FRAMES,
                  "setup: the round's length is %d, expected %d",
                  (int)serverSimGetGameLength(sim), GT_MINUTE_FRAMES);

    /* Half the round: the clock has lost half its frames, not all of them. */
    for (i = 0; i < GT_MINUTE_FRAMES / 2; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round ended within half its length (state %d)",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == GT_MINUTE_FRAMES / 2,
                  "after %d frames the time left is %d, expected %d — the "
                  "clock must lose one a frame",
                  GT_MINUTE_FRAMES / 2, (int)serverSimGetGameLength(sim),
                  GT_MINUTE_FRAMES / 2);

    ended = gtFramesUntilOver(sim, GT_MINUTE_FRAMES);
    UT_ASSERT_MSG(ended == GT_MINUTE_FRAMES / 2,
                  "the round ended %d frames after the half-way mark, "
                  "expected %d (-1 = never)",
                  ended, GT_MINUTE_FRAMES / 2);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "the time limit must put the round in game-over, state %d",
                  (int)serverSimGetState(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. The start delay holds the round for its own length of frames.
 * ================================================================ */
int run_game_time_start_delay_counts_frames(void) {
    ServerSim *sim = gtRunningSim(GT_DELAY_FRAMES, GT_MINUTE_FRAMES);
    int i, ended;

    UT_ASSERT_MSG(sim != NULL, "gtRunningSim returned NULL");
    UT_ASSERT_MSG(serverSimGetStartDelay(sim) == GT_DELAY_FRAMES,
                  "setup: the start delay is %d, expected %d",
                  (int)serverSimGetStartDelay(sim), GT_DELAY_FRAMES);

    /* One frame short of the delay: still held, and the round's clock has
       not moved while it was. */
    for (i = 0; i < GT_DELAY_FRAMES - 1; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(serverSimGetStartDelay(sim) == 1,
                  "after %d frames the start delay reads %d, expected 1 — "
                  "the delay must lose one a frame",
                  GT_DELAY_FRAMES - 1, (int)serverSimGetStartDelay(sim));
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == GT_MINUTE_FRAMES,
                  "the round's clock moved to %d during the start delay",
                  (int)serverSimGetGameLength(sim));

    serverSimTick(sim);
    UT_ASSERT_MSG(serverSimGetStartDelay(sim) == 0,
                  "the start delay reads %d after its own length of frames",
                  (int)serverSimGetStartDelay(sim));

    /* The delay and then the whole round: D + L frames in all. */
    ended = gtFramesUntilOver(sim, 2 * GT_MINUTE_FRAMES);
    UT_ASSERT_MSG(ended == GT_MINUTE_FRAMES,
                  "the round ended %d frames after the delay, expected %d "
                  "(-1 = never)", ended, GT_MINUTE_FRAMES);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. A time limit set in the lobby, in minutes, is that many minutes.
 * ================================================================ */
int run_game_time_lobby_minutes(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0,
                                               UNLIMITED_GAME_TIME);
    uint8_t on[1]    = { 1 };
    uint8_t mins[2]  = { 0, 2 };   /* two minutes, big-endian */
    const int expect = 2 * 60 * GAME_NUMGAMETICKS_SEC;
    int ended;

    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetQuitOnWin(sim, false);

    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_TIME_LIMIT, on, 1),
                  "the lobby must take a time limit");
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_TIME_MINUTES, mins, 2),
                  "the lobby must take a length of two minutes");
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == expect,
                  "two lobby minutes came to %d, expected %d",
                  (int)serverSimGetGameLength(sim), expect);

    /* The lobby's countdown ends in this; the round keeps the length the
       lobby set rather than the one the server was started with. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "setup: the round must be running, state %d",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == expect,
                  "the started round's length is %d, expected the lobby's %d",
                  (int)serverSimGetGameLength(sim), expect);

    ended = gtFramesUntilOver(sim, 2 * expect);
    UT_ASSERT_MSG(ended == expect,
                  "a two-minute round ended on frame %d, expected %d "
                  "(120 seconds at %d frames a second; -1 = never)",
                  ended, expect, GAME_NUMGAMETICKS_SEC);

    /* The next round starts on the lobby's length again, not on what the
       last one left or on the server's start-up length. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetGameLength(sim) == expect,
                  "the next round's length is %d, expected the lobby's %d",
                  (int)serverSimGetGameLength(sim), expect);

    serverSimDestroy(sim);
    return 0;
}
