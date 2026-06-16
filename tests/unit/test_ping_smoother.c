/*
 * Pure tests for the client ping RTT smoothers (pingMinWindowPush /
 * pingEwmaUpdate). Both are free functions with no transport-struct
 * dependency, so they run here without sockets or impairment.
 *
 * The min-over-window value feeds shell forward-projection and the
 * viewTick==0 lag-comp fallback; the EWMA value feeds the player-facing
 * HUD/scoreboard displays. The EWMA's monotonic, overshoot-free response to
 * a step is the deterministic stand-in for the "no oscillation at the higher
 * sample rate" behaviour.
 */

#include <stdint.h>

#include "transport_udp_internal.h"  /* PingMinWindow, PingEwma, helpers */
#include "test_harness.h"

/* Reference min over the last PING_MIN_WINDOW_LEN entries of seq[0..upTo]. */
static uint16_t windowMinRef(const uint16_t *seq, int upTo) {
  int start = upTo - (PING_MIN_WINDOW_LEN - 1);
  uint16_t m;
  int i;
  if (start < 0) start = 0;
  m = seq[start];
  for (i = start + 1; i <= upTo; i++) {
    if (seq[i] < m) m = seq[i];
  }
  return m;
}

int run_ping_smoother(void) {
  /* (1) Min-over-window: a dip (the 30 at index 3) is the running minimum
   * until it slides out of the window, then the min rises. The returned
   * value tracks the reference window minimum at every step. */
  {
    /* 8-sample dip followed by 8 samples all >= 60 so the dip slides out. */
    const uint16_t seq[] = {
        50, 40, 60, 30, 55, 45, 50, 52,
        60, 61, 62, 63, 64, 65, 66, 67
    };
    const int n = (int)(sizeof(seq) / sizeof(seq[0]));
    const int dipIdx = 3;  /* the 30 */
    PingMinWindow w;
    int i;

    pingMinWindowReset(&w);
    for (i = 0; i < n; i++) {
      uint16_t got = pingMinWindowPush(&w, seq[i]);
      uint16_t want = windowMinRef(seq, i);
      UT_ASSERT_MSG(got == want, "step %d: got %u want %u", i, got, want);

      if (i <= dipIdx) {
        /* Once the dip has been pushed it is the window minimum. */
        if (i == dipIdx) UT_ASSERT(got == 30);
      } else if (i - dipIdx >= PING_MIN_WINDOW_LEN) {
        /* Dip has slid fully out of the window — min must have risen. */
        UT_ASSERT_MSG(got > 30, "step %d still sees dip: %u", i, got);
      }
    }
  }

  /* (2a) EWMA constant input converges to that constant and stays there. */
  {
    PingEwma e;
    int i;
    uint16_t got = 0;
    pingEwmaReset(&e);
    for (i = 0; i < 64; i++) {
      got = pingEwmaUpdate(&e, 123);
    }
    UT_ASSERT_MSG(got == 123, "constant EWMA converged to %u", got);
  }

  /* (2b) EWMA step up: response is monotonically non-decreasing (no
   * oscillation), never overshoots the target, and approaches it. */
  {
    PingEwma e;
    const uint16_t lo = 50, hi = 250;
    uint16_t prev, got;
    int i;
    pingEwmaReset(&e);
    /* Settle at the low level first. */
    for (i = 0; i < 32; i++) prev = pingEwmaUpdate(&e, lo);
    UT_ASSERT(prev == lo);
    /* Step to the high level. */
    for (i = 0; i < 64; i++) {
      got = pingEwmaUpdate(&e, hi);
      UT_ASSERT_MSG(got >= prev, "step-up delta changed sign: %u -> %u",
                    prev, got);
      UT_ASSERT_MSG(got <= hi, "step-up overshot: %u", got);
      prev = got;
    }
    UT_ASSERT_MSG(got == hi, "step-up did not converge: %u", got);
  }

  /* (2c) EWMA step down: response is monotonically non-increasing and never
   * undershoots the target. */
  {
    PingEwma e;
    const uint16_t lo = 40, hi = 300;
    uint16_t prev, got;
    int i;
    pingEwmaReset(&e);
    for (i = 0; i < 32; i++) prev = pingEwmaUpdate(&e, hi);
    UT_ASSERT(prev == hi);
    for (i = 0; i < 64; i++) {
      got = pingEwmaUpdate(&e, lo);
      UT_ASSERT_MSG(got <= prev, "step-down delta changed sign: %u -> %u",
                    prev, got);
      UT_ASSERT_MSG(got >= lo, "step-down undershot: %u", got);
      prev = got;
    }
    UT_ASSERT_MSG(got == lo, "step-down did not converge: %u", got);
  }

  return 0;
}
