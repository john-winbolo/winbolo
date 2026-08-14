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
#include "ping_display.h"            /* PingDisplay — player-row conditioning */
#include "test_harness.h"

/* Drive the display conditioner to `target` the way the snapshot stream
 * does: alternating target/target+1 so every push is a *changed* sample
 * (repeats are deliberately ignored — see ping_display.h). Returns the
 * settled display value. */
static uint16_t displaySettle(PingDisplay *d, uint16_t target, int steps) {
  uint16_t shown = 0;
  int i;
  for (i = 0; i < steps; i++) {
    shown = pingDisplayPush(d, (uint16_t)(target + (i & 1)));
  }
  return shown;
}

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

  /* (3a) Display conditioner ignores repeats. The server re-stamps the same
   * RTT into every snapshot (50Hz) and only re-measures every ~0.4s; folding
   * the repeats would walk the average straight onto each sample and smooth
   * nothing. A long run of identical samples must not move the value. */
  {
    PingDisplay d;
    uint16_t seeded, got;
    int i;
    pingDisplayReset(&d);
    seeded = pingDisplayPush(&d, 100);
    UT_ASSERT_MSG(seeded == 100, "first sample not seeded: %u", seeded);
    for (i = 0; i < 200; i++) {
      got = pingDisplayPush(&d, 100);
      UT_ASSERT_MSG(got == seeded, "repeat sample moved value: %u", got);
    }
    /* One changed sample is damped, not taken whole. */
    got = pingDisplayPush(&d, 300);
    UT_ASSERT_MSG(got > 100 && got < 300, "step taken undamped: %u", got);
  }

  /* (3b) Deadband: jitter that keeps the smoothed value within a step of
   * what is on screen must not repaint the digits at all. */
  {
    PingDisplay d;
    uint16_t shown, got;
    int i;
    pingDisplayReset(&d);
    shown = pingDisplayPush(&d, 100);
    for (i = 0; i < 64; i++) {
      got = pingDisplayPush(&d, (uint16_t)((i & 1) ? 104 : 96));
      UT_ASSERT_MSG(got == shown, "deadband breached by +-4ms jitter: %u", got);
    }
  }

  /* (3c) A real latency change still gets through, and lands near it. */
  {
    PingDisplay d;
    uint16_t got;
    pingDisplayReset(&d);
    displaySettle(&d, 40, 32);
    got = displaySettle(&d, 240, 64);
    UT_ASSERT_MSG(got >= 235 && got <= 245, "step did not track: %u", got);
  }

  /* (3d) Sub-step values render exactly rather than rounding to the step or
   * into the 0 sentinel the wire uses for "no measurement". */
  {
    PingDisplay d;
    pingDisplayReset(&d);
    UT_ASSERT(pingDisplayPush(&d, 3) == 3);
  }

  /* (3e) A zero sample (no measurement) clears the state, so a slot reused
   * by another player starts clean instead of inheriting an average. */
  {
    PingDisplay d;
    pingDisplayReset(&d);
    displaySettle(&d, 200, 32);
    UT_ASSERT(pingDisplayPush(&d, 0) == 0);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_NONE);
    /* Next real sample seeds fresh rather than averaging with the old 200. */
    UT_ASSERT(pingDisplayPush(&d, 30) == 30);
  }

  /* (3f) Colour-band hysteresis: a player parked on a threshold holds its
   * band until the value clears it by the margin, in both directions. */
  {
    PingDisplay d;
    pingDisplayReset(&d);

    displaySettle(&d, 40, 32);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_GOOD);
    /* Past the 50ms threshold but inside the margin — still green. */
    displaySettle(&d, 55, 32);
    UT_ASSERT_MSG(pingDisplayBand(&d) == PING_BAND_GOOD,
                  "band flipped inside the rising margin (%u)",
                  pingDisplayValue(&d));
    displaySettle(&d, 65, 32);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_FAIR);
    /* Back under 50 but inside the margin — stays yellow. */
    displaySettle(&d, 45, 32);
    UT_ASSERT_MSG(pingDisplayBand(&d) == PING_BAND_FAIR,
                  "band flipped inside the falling margin (%u)",
                  pingDisplayValue(&d));
    displaySettle(&d, 35, 32);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_GOOD);

    /* Same margins at the fair/poor boundary. */
    displaySettle(&d, 155, 64);
    UT_ASSERT_MSG(pingDisplayBand(&d) == PING_BAND_FAIR,
                  "poor band entered early (%u)", pingDisplayValue(&d));
    displaySettle(&d, 170, 64);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_POOR);
    displaySettle(&d, 145, 64);
    UT_ASSERT_MSG(pingDisplayBand(&d) == PING_BAND_POOR,
                  "poor band left early (%u)", pingDisplayValue(&d));
    displaySettle(&d, 130, 64);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_FAIR);
  }

  /* (3g) Stateless classification (used by the lobby, which has no per-slot
   * state to carry hysteresis) matches the documented thresholds. */
  {
    UT_ASSERT(pingBandClassify(0) == PING_BAND_NONE);
    UT_ASSERT(pingBandClassify(49) == PING_BAND_GOOD);
    UT_ASSERT(pingBandClassify(50) == PING_BAND_FAIR);
    UT_ASSERT(pingBandClassify(149) == PING_BAND_FAIR);
    UT_ASSERT(pingBandClassify(150) == PING_BAND_POOR);
  }

  return 0;
}
