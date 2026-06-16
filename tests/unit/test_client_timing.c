/*
 * Pure tests for the client timing estimator (client_timing.{c,h}). The
 * estimator has no transport, socket, or SDL dependency, so it runs here on
 * synthetic deterministic sequences — no network.
 *
 * Model recap (see client_timing.h): clock offset = localArrivalTick -
 * serverTick (both 10ms/100Hz counters, no conversion), min-over-window;
 * pipeline depth = serverTick - lastProcessedInput; inter-arrival jitter =
 * max-min spread of the per-snapshot local-tick gap; RTT = min-over-window
 * of raw pong samples.
 *
 * A steady link advances the local arrival tick and serverTick by 2 each per
 * snapshot (~20ms apart at 10ms/tick), so localArrivalTick - serverTick is a
 * constant — the floor the min filter must converge to and hold through
 * jitter spikes.
 */

#include <stdint.h>

#include "client_timing.h"
#include "test_harness.h"

int run_client_timing(void) {
  /* (0) Reset returns a defined zero state. */
  {
    ClientTiming t;
    clientTimingReset(&t);
    UT_ASSERT(clientTimingClockOffsetTicks(&t) == 0);
    UT_ASSERT(clientTimingPipelineDepthTicks(&t) == 0);
    UT_ASSERT(clientTimingJitterMs(&t) == 0);
    UT_ASSERT(clientTimingRttMs(&t) == 0);
  }

  /* (1) Join seed is visible before any snapshot folds in, and a later
   * snapshot supersedes it. */
  {
    ClientTiming t;
    clientTimingReset(&t);
    clientTimingSeedFromJoin(&t, 200, 0);
    UT_ASSERT_MSG(clientTimingClockOffsetTicks(&t) == -200,
                  "seed offset %d", clientTimingClockOffsetTicks(&t));
    /* First snapshot at serverTick 100, localArrival 20 → offset -80
     * overrides the -200 seed. */
    clientTimingOnSnapshot(&t, 100, 94, 20);
    UT_ASSERT_MSG(clientTimingClockOffsetTicks(&t) == -80,
                  "post-snapshot offset %d", clientTimingClockOffsetTicks(&t));
  }

  /* (2) Clean sequence: constant spacing, constant depth, constant RTT.
   * Estimates converge to the expected constants. */
  {
    ClientTiming t;
    const int DEPTH = 6;
    const int N = 30;
    int i;
    clientTimingReset(&t);
    for (i = 0; i < N; i++) {
      uint32_t serverTick = (uint32_t)(100 + 2 * i);
      uint32_t lastInput = serverTick - (uint32_t)DEPTH;
      uint32_t localArrival = (uint32_t)(20 + 2 * i);
      clientTimingOnSnapshot(&t, serverTick, lastInput, localArrival);
      clientTimingOnRtt(&t, 50);
    }
    /* offset = (20+2i) - (100+2i) = -80, constant. */
    UT_ASSERT_MSG(clientTimingClockOffsetTicks(&t) == -80,
                  "clean offset %d", clientTimingClockOffsetTicks(&t));
    UT_ASSERT_MSG(clientTimingPipelineDepthTicks(&t) == DEPTH,
                  "clean depth %d", clientTimingPipelineDepthTicks(&t));
    /* Constant 2-tick spacing → zero spread → zero jitter. */
    UT_ASSERT_MSG(clientTimingJitterMs(&t) == 0,
                  "clean jitter %u", clientTimingJitterMs(&t));
    UT_ASSERT_MSG(clientTimingRttMs(&t) == 50,
                  "clean rtt %u", clientTimingRttMs(&t));
  }

  /* (3) Jittered sequence: a transient delay spike inflates a run of
   * arrivals, then drains back to the baseline. The min-over-window offset
   * holds the floor (-80) and never tracks the spike; jitter rises with the
   * gap spread; RTT min holds the floor delay. */
  {
    ClientTiming t;
    const int DEPTH = 6;
    /* spikeDelay drains 0,0,0,3,2,1,0,0,0,0 — local arrival is the baseline
     * schedule (20+2i, a 2-tick nominal gap) plus this transient, staying
     * monotonic. */
    const int spikeDelay[] = {0, 0, 0, 3, 2, 1, 0, 0, 0, 0};
    const uint16_t rtt[] = {40, 40, 90, 40, 120, 40, 40, 95, 40, 40};
    const int N = (int)(sizeof(spikeDelay) / sizeof(spikeDelay[0]));
    int i;
    bool sawJitter = false;
    clientTimingReset(&t);
    for (i = 0; i < N; i++) {
      uint32_t serverTick = (uint32_t)(100 + 2 * i);
      uint32_t lastInput = serverTick - (uint32_t)DEPTH;
      uint32_t localArrival = (uint32_t)(20 + 2 * i + spikeDelay[i]);
      clientTimingOnSnapshot(&t, serverTick, lastInput, localArrival);
      clientTimingOnRtt(&t, rtt[i]);
      if (clientTimingJitterMs(&t) > 0) sawJitter = true;
    }
    /* The floor is the un-delayed baseline; spikes raise individual offsets
     * but min rejects them. */
    UT_ASSERT_MSG(clientTimingClockOffsetTicks(&t) == -80,
                  "jittered offset floor %d", clientTimingClockOffsetTicks(&t));
    /* Depth is unaffected by arrival jitter. */
    UT_ASSERT_MSG(clientTimingPipelineDepthTicks(&t) == DEPTH,
                  "jittered depth %d", clientTimingPipelineDepthTicks(&t));
    /* The spike produced a measurable arrival-gap spread at some point. */
    UT_ASSERT(sawJitter);
    /* RTT min tracks the 40ms floor, not the 90/120ms spikes. */
    UT_ASSERT_MSG(clientTimingRttMs(&t) == 40,
                  "jittered rtt floor %u", clientTimingRttMs(&t));
  }

  return 0;
}
