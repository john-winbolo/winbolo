/*
 * A -braindebug server records each round into its own session dir
 * (test_braindbg_round_dir.c).
 *
 * On the first running tick of a round the server lifecycle opens a recording
 * block, debug_sessions/<timestamp>_1, unless the host has already named a
 * dir of its own. brainRecordEndGame keeps the dir name on purpose, so the
 * lifecycle has to give back the name it set when the round ends. If it
 * does not, round 2's first running tick finds round 1's dir still set,
 * takes it for one the host named, opens no new block, and the recorder
 * reopens round 1's brainrec.btr for writing and records round 2 over it.
 *
 * This case plays two rounds over a loopback transport and checks that the
 * dir is cleared at game over and that round 2 records into a dir of its
 * own. The block name carries the wall-clock second the round started, so
 * the case waits just over a second between the rounds.
 *
 * The lifecycle will not start a recording with less than 50 GB free on the
 * disk. On such a disk it switches recording off instead, and this case has
 * nothing to check, so it says so and passes.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* returnToLobbyTicks, countdownTicks */
#include "brain_record.h"
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

static bool brdIsRunning(LoopbackHarness *h, void *user) {
    (void)user;
    return serverSimGetState(h->sim) == serverStateRunning;
}

static bool brdIsNotRunning(LoopbackHarness *h, void *user) {
    (void)user;
    return serverSimGetState(h->sim) != serverStateRunning;
}

static bool brdIsLobby(LoopbackHarness *h, void *user) {
    (void)user;
    return serverSimGetState(h->sim) == serverStateLobby;
}

/* Ends the running round from inside the running tick, the way a passed
 * return-to-lobby vote does, so the game-over hook runs. */
static void brdEndRound(LoopbackHarness *h) {
    threadsWaitForMutex();
    h->sim->returnToLobbyReason = RETURN_REASON_MANUAL_VOTE;
    h->sim->returnToLobbyTicks = 1;
    threadsReleaseMutex();
}

/* Removes a block dir this case made and the files the recorder may have
 * left in it. */
static void brdRemoveBlock(const char *dir) {
    char path[FILENAME_MAX];

    if (dir == NULL || dir[0] == '\0') return;
    snprintf(path, sizeof(path), "%s/brainrec.btr", dir);
    SDL_RemovePath(path);
    snprintf(path, sizeof(path), "%s/braindbg_perf.log", dir);
    SDL_RemovePath(path);
    SDL_RemovePath(dir);
}

static void brdRecorderOff(void) {
    brainRecordEndGame();
    brainRecordSetEnabled(false);
    brainRecordSetSessionDir(NULL);
}

int run_braindbg_second_round_new_dir(void) {
    LoopbackHarness h;
    char dir1[FILENAME_MAX] = "";
    char dir2[FILENAME_MAX] = "";
    const char *dir;
    int i;

    memset(&h, 0, sizeof(h));
    UT_ASSERT(loopbackHarnessStart(&h, "Recorder", /*lobbyMode*/ true, NULL,
                                   /*seed*/ 5151));
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, 400, NULL, NULL) > 0,
                  "the client never finished joining");

    /* Armed with no dir named: the lifecycle owns the blocks. */
    brainRecordSetSessionDir(NULL);
    brainRecordSetEnabled(true);

    /* Round 1. */
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "the client's slot was never ready for game start");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, 600, brdIsRunning, NULL) > 0,
                  "round 1 never reached the running state");
    for (i = 0; i < 2; i++) loopbackHarnessPump(&h);

    dir = brainRecordGetSessionDir();
    if (dir == NULL && !brainRecordIsEnabled()) {
        fprintf(stderr, "  less than 50 GB free: the lifecycle did not start "
                        "a recording, nothing to check\n");
        brdRecorderOff();
        loopbackHarnessStop(&h);
        return 0;
    }
    UT_ASSERT_MSG(dir != NULL, "round 1 opened no recording block");
    SDL_strlcpy(dir1, dir, sizeof(dir1));

    brdEndRound(&h);
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, 200, brdIsNotRunning, NULL) > 0,
                  "round 1 never ended");
    dir = brainRecordGetSessionDir();
    UT_ASSERT_MSG(dir == NULL,
                  "round 1's dir %s is still set after game over: round 2 "
                  "will take it for a dir the host named and record over "
                  "round 1", dir ? dir : "");

    threadsWaitForMutex();
    h.sim->countdownTicks = 1;
    threadsReleaseMutex();
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, 240, brdIsLobby, NULL) > 0,
                  "round 1 never returned to the lobby");

    /* The block name has one-second resolution. */
    SDL_Delay(1100);

    /* Round 2. */
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "the client's slot was never ready for the second start");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, 600, brdIsRunning, NULL) > 0,
                  "round 2 never reached the running state");
    for (i = 0; i < 2; i++) loopbackHarnessPump(&h);

    dir = brainRecordGetSessionDir();
    if (dir != NULL) SDL_strlcpy(dir2, dir, sizeof(dir2));

    /* Close round 2 the same way, so the lifecycle's block state is back
     * where it started for whatever runs next in this process. */
    brdEndRound(&h);
    loopbackHarnessPumpUntil(&h, 200, brdIsNotRunning, NULL);
    brdRecorderOff();
    loopbackHarnessStop(&h);
    brdRemoveBlock(dir1);
    if (strcmp(dir1, dir2) != 0) brdRemoveBlock(dir2);

    UT_ASSERT_MSG(dir2[0] != '\0', "round 2 opened no recording block");
    UT_ASSERT_MSG(strcmp(dir1, dir2) != 0,
                  "round 2 recorded into round 1's dir %s", dir1);
    return 0;
}
