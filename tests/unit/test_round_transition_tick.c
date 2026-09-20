/*
 * The round transition does not run WinBolo.net on the tick thread
 * (test_round_transition_tick.c).
 *
 * A round boundary used to do three HTTPS calls inside serverInstanceTick,
 * under the tick lock: server/quit, a multipart upload of the whole round's
 * log with a 60s timeout, and server/register. Every player in the lobby sat
 * through all three, and a slow WinBolo.net was enough to time them out with
 * nobody quitting.
 *
 * The transition now queues the three for the WinBolo.net worker and returns.
 * The worker sends them in the order they were queued, which is the order the
 * tracker requires — quit before the upload, upload before the key swap — and
 * the tail that needs the new key waits for the register's reply, which comes
 * back through winbolonetThreadDrainResults at the top of a later tick.
 *
 * What this case pins, driving a real lobby game over a loopback transport:
 *   - the tick that performs the transition returns in under 50ms
 *   - quit, upload and register are queued, in that order
 *   - the rotation window stays open and no rekey reaches the client while
 *     the register is still outstanding
 *   - delivering the register result closes the window and sends the rekey,
 *     within two pumps
 *
 * The third is the one that matters: it is what says the tail belongs to the
 * completion handler rather than to the tick that queued the work.
 *
 * The rekey is observed through the deferred-join arm the broadcast sets on
 * each rekeyed slot (wbnJoinArm, udp_server_wbn.c). It is the server-side
 * mark of "a rekey went out to this client", it is cleared here before the
 * transition so only a new one can set it, and nothing else arms it once the
 * slot has joined.
 *
 * The WinBolo.net stub (test_stubs.c) records what the lifecycle queued and
 * holds the register result until this test releases it, so the handoff
 * happens on the test's schedule rather than a live host's.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* wbnSessionRotating, countdownTicks */
#include "server_sim_lifecycle.h"  /* serverSimEnterGameOver */
#include "server_lifecycle.h"      /* serverLifecycleSetRoundLogHooks */
#include "transport_udp.h"
#include "transport_udp_server_internal.h" /* udpServer */
#include "threads.h"
#include "../../src/winbolonet/winbolonet_core.h"   /* WINBOLONET_KEY_LEN */
#include "../../src/winbolonet/winbolonetthread.h"  /* winbolonetThreadAddUpload */
#include "test_harness.h"
#include "loopback_harness.h"

/* WinBolo.net stub surface, defined in test_stubs.c. */
extern bool     wbnStubRunning;
extern char     wbnStubServerKey[];
extern int      wbnStubJobCount;
extern char     wbnStubJobs[][16];
extern bool     wbnStubRegisterResultReady;
extern int      wbnStubApplyRegisterCalls;
extern bool     wbnStubApplyRegisterOk;

/* A key for the finished round. transportUdpServerSendWbnRekey drops the
 * broadcast when the server holds no key, so the stub has to answer with one
 * for the rekey to be observable at all. */
#define RT_SERVER_KEY "0123456789abcdef0123456789abcdef"

/* The tick that queues the transition has to come back in well under a frame.
 * A single synchronous WinBolo.net call could not. */
#define RT_TICK_MAX_MS 50.0

/* Round-log hooks. The dedicated-log module installs its own pair on a real
 * server; this is what its flush now does, which is what puts the upload
 * between the quit and the register. */
static int s_stashCalls = 0;

static void rtRoundLogStash(void) {
  s_stashCalls++;
}

static void rtRoundLogFlush(void) {
  winbolonetThreadAddUpload("round_transition_tick.log", wbnStubServerKey);
}

static bool rtStateIsRunning(LoopbackHarness *h, void *user) {
  (void)user;
  return serverSimGetState(h->sim) == serverStateRunning;
}

static bool rtStateIsLobby(LoopbackHarness *h, void *user) {
  (void)user;
  return serverSimGetState(h->sim) == serverStateLobby;
}

int run_round_transition_tick_does_not_block(void) {
  LoopbackHarness h;
  Uint64 freq = SDL_GetPerformanceFrequency();
  Uint64 started;
  double transitionMs = 0.0;
  int slot;
  int i;
  int transitionPump = -1;

  memset(&h, 0, sizeof(h));
  UT_ASSERT(loopbackHarnessStart(&h, "Rotator", /*lobbyMode*/ true, NULL,
                                 /*seed*/ 4242));

  /* Let the join settle and read back the slot the client landed in. */
  UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, 400, NULL, NULL) > 0,
                "the client never finished joining");
  slot = clientSimGetMyPlayerNum(h.cs);
  UT_ASSERT_MSG(slot >= 0 && slot < MAX_TANKS,
                "the client has no server slot (got %d)", slot);
  UT_ASSERT_MSG(udpServer.clients[slot].connected,
                "slot %d is not connected on the server", slot);

  wbnStubRunning = TRUE;
  SDL_strlcpy(wbnStubServerKey, RT_SERVER_KEY, WINBOLONET_KEY_LEN);
  serverLifecycleSetRoundLogHooks(rtRoundLogStash, rtRoundLogFlush);

  /* Play a round: lobby -> countdown -> running. */
  UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                "the client's slot was never ready for game start");
  UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, 600, rtStateIsRunning, NULL) > 0,
                "the game never reached the running state");

  /* The rekey broadcast only targets a slot that was WBN-verified last round,
   * and the join already armed this slot's deferred join. Set the one and
   * clear the other so the arm below can only come from a new rekey. */
  threadsWaitForMutex();
  udpServer.clients[slot].wbnWasVerified = true;
  udpServer.clients[slot].wbnJoin.pending = false;
  threadsReleaseMutex();

  wbnStubJobCount = 0;
  wbnStubApplyRegisterCalls = 0;
  wbnStubApplyRegisterOk = TRUE;
  wbnStubRegisterResultReady = FALSE;
  s_stashCalls = 0;

  /* End the round and let the game-over countdown expire inside
   * serverInstanceTick, which is what runs the transition. */
  threadsWaitForMutex();
  serverSimEnterGameOver(h.sim);
  h.sim->countdownTicks = 1;
  threadsReleaseMutex();

  for (i = 0; i < 40 && transitionPump < 0; i++) {
    started = SDL_GetPerformanceCounter();
    loopbackHarnessPump(&h);
    if (wbnStubJobCount > 0) {
      transitionMs = (double)(SDL_GetPerformanceCounter() - started) * 1000.0 /
                     (double)freq;
      transitionPump = i;
    }
  }

  UT_ASSERT_MSG(transitionPump >= 0,
                "the round transition never ran: state %d, %d jobs queued",
                (int)serverSimGetState(h.sim), wbnStubJobCount);
  UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, 200, rtStateIsLobby, NULL) > 0,
                "the game never reached the lobby");

  UT_ASSERT_MSG(transitionMs < RT_TICK_MAX_MS,
                "the transition tick took %.1fms, expected under %.1fms - the "
                "WinBolo.net calls are back on the tick thread",
                transitionMs, RT_TICK_MAX_MS);

  /* Queued, in the order WinBolo.net requires. */
  UT_ASSERT_MSG(wbnStubJobCount == 3,
                "the transition queued %d jobs, expected 3 (quit, upload, "
                "register)", wbnStubJobCount);
  UT_ASSERT_MSG(strcmp(wbnStubJobs[0], "quit") == 0,
                "job 0 was '%s', expected 'quit'", wbnStubJobs[0]);
  UT_ASSERT_MSG(strcmp(wbnStubJobs[1], "upload") == 0,
                "job 1 was '%s', expected 'upload'", wbnStubJobs[1]);
  UT_ASSERT_MSG(strcmp(wbnStubJobs[2], "register") == 0,
                "job 2 was '%s', expected 'register'", wbnStubJobs[2]);

  /* Nothing that needs the new key may have happened yet. */
  loopbackHarnessPump(&h);
  loopbackHarnessPump(&h);
  UT_ASSERT_MSG(wbnStubApplyRegisterCalls == 0,
                "the register result was applied %d times before the test "
                "delivered it", wbnStubApplyRegisterCalls);
  UT_ASSERT_MSG(h.sim->wbnSessionRotating == TRUE,
                "the rotation window closed before the register result came "
                "back");
  UT_ASSERT_MSG(udpServer.clients[slot].wbnJoin.pending == false,
                "a rekey reached slot %d before the register result came back",
                slot);

  /* Deliver it: the handler owns everything that needed the new key. */
  wbnStubRegisterResultReady = TRUE;
  loopbackHarnessPump(&h);
  loopbackHarnessPump(&h);

  UT_ASSERT_MSG(wbnStubApplyRegisterCalls == 1,
                "the register result was applied %d times, expected once",
                wbnStubApplyRegisterCalls);
  UT_ASSERT_MSG(h.sim->wbnSessionRotating == FALSE,
                "the register result must close the rotation window");
  UT_ASSERT_MSG(udpServer.clients[slot].wbnJoin.pending == true,
                "no rekey reached slot %d within two pumps of the register "
                "result", slot);

  serverLifecycleSetRoundLogHooks(NULL, NULL);
  wbnStubRunning = FALSE;
  wbnStubServerKey[0] = '\0';
  wbnStubJobCount = 0;
  wbnStubRegisterResultReady = FALSE;
  loopbackHarnessStop(&h);
  return 0;
}
