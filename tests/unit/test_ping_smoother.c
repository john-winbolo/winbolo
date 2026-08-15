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

/* Snapshot arrival interval — the conditioner is offered a sample this
 * often and folds on its own, much slower, clock. */
#define SNAP_MS 20u

/* Wall-clock the display tests advance by hand. The conditioner takes the
 * clock as an argument precisely so this is deterministic. */
static uint32_t s_displayClock;

/* Drive the conditioner for `ms` of wall clock at the snapshot rate, with
 * a sample of `value(i)` each arrival. Returns the settled display value. */
static uint16_t displayRun(PingDisplay *d, uint16_t (*value)(int), int ms) {
  uint16_t shown = pingDisplayValue(d);
  int steps = ms / (int)SNAP_MS;
  int i;
  for (i = 0; i < steps; i++) {
    shown = pingDisplayPush(d, value(i), s_displayClock);
    s_displayClock += SNAP_MS;
  }
  return shown;
}

/* Sample generators. The constant one is the important case: a link stable
 * to the millisecond repeats its measurement forever, and the conditioner
 * must still converge — an earlier fold-on-change design froze partway
 * through a step here and showed a permanently wrong number. */
static uint16_t s_target;
static uint16_t sampleConstant(int i) { (void)i; return s_target; }
static uint16_t sampleAlternating(int i) {
  return (uint16_t)(s_target + (i & 1));
}
static uint16_t sampleJitter(int i) {
  /* +-4ms around the target: inside the deadband, must never repaint. */
  static const int off[] = {0, 4, -3, 2, -4, 3, -2, 1};
  return (uint16_t)((int)s_target + off[i & 7]);
}

static uint16_t displaySettle(PingDisplay *d, uint16_t target, int ms) {
  s_target = target;
  return displayRun(d, sampleConstant, ms);
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

  /* (3a) Fold rate. The server re-stamps the same measurement into every
   * snapshot (50Hz) and only re-measures every ~0.4s, so the conditioner
   * must fold on its own clock — a fold per arrival would walk the average
   * onto each sample and smooth nothing. Over one fold interval the value
   * moves at most once, however many samples arrive. */
  {
    PingDisplay d;
    uint16_t seeded, got;
    int repaints = 0, i;
    pingDisplayReset(&d);
    s_displayClock = 1000;

    seeded = pingDisplayPush(&d, 100, s_displayClock);
    UT_ASSERT_MSG(seeded == 100, "first sample not seeded: %u", seeded);

    /* Just over one fold interval of arrivals, all offering a big step:
     * exactly one of them may move the value. */
    got = seeded;
    for (i = 0; i <= (int)(PING_DISPLAY_FOLD_INTERVAL_MS / SNAP_MS); i++) {
      uint16_t prev = got;
      s_displayClock += SNAP_MS;
      got = pingDisplayPush(&d, 300, s_displayClock);
      if (got != prev) repaints++;
    }
    UT_ASSERT_MSG(repaints == 1, "%d repaints over one fold interval",
                  repaints);
    /* And when it does fold, the step is damped rather than taken whole. */
    UT_ASSERT_MSG(got > 100 && got < 300, "step taken undamped: %u", got);
  }

  /* (3b) A link stable to the millisecond repeats its measurement forever.
   * The value must still converge: folding only on *changed* samples froze
   * the average partway through the step and left the row showing a
   * permanently wrong number. */
  {
    PingDisplay d;
    uint16_t got;
    pingDisplayReset(&d);
    s_displayClock = 1000;
    displaySettle(&d, 40, 3000);
    /* 6s covers the ~4s the alpha=1/4 fold needs to close a 200ms step to
     * within a display step (see ping_display.h). */
    got = displaySettle(&d, 240, 6000);
    UT_ASSERT_MSG(got >= 235 && got <= 245,
                  "constant sample stream stalled at %u (want ~240)", got);
  }

  /* (3c) Same step over a jittery link, for contrast — both converge. */
  {
    PingDisplay d;
    uint16_t got;
    pingDisplayReset(&d);
    s_displayClock = 1000;
    s_target = 40;
    displayRun(&d, sampleAlternating, 3000);
    s_target = 240;
    got = displayRun(&d, sampleAlternating, 6000);
    UT_ASSERT_MSG(got >= 235 && got <= 245, "step did not track: %u", got);
  }

  /* (3d) Deadband: jitter that keeps the smoothed value within a step of
   * what is on screen must not repaint the digits at all — across many
   * fold intervals, not just many arrivals. */
  {
    PingDisplay d;
    uint16_t shown, got;
    int i;
    pingDisplayReset(&d);
    s_displayClock = 1000;
    shown = displaySettle(&d, 100, 2000);
    s_target = 100;
    for (i = 0; i < 500; i++) {   /* 10s of +-4ms jitter */
      got = pingDisplayPush(&d, sampleJitter(i), s_displayClock);
      s_displayClock += SNAP_MS;
      UT_ASSERT_MSG(got == shown, "deadband breached by +-4ms jitter: %u",
                    got);
    }
  }

  /* (3e) Sub-step values render exactly rather than rounding to the step or
   * into the 0 sentinel the wire uses for "no measurement". */
  {
    PingDisplay d;
    pingDisplayReset(&d);
    s_displayClock = 1000;
    UT_ASSERT(pingDisplayPush(&d, 3, s_displayClock) == 3);
  }

  /* (3f) A zero sample (no measurement) clears the state, so a slot reused
   * by another player starts clean instead of inheriting an average. */
  {
    PingDisplay d;
    pingDisplayReset(&d);
    s_displayClock = 1000;
    displaySettle(&d, 200, 2000);
    UT_ASSERT(pingDisplayPush(&d, 0, s_displayClock) == 0);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_NONE);
    /* Next real sample seeds fresh rather than averaging with the old 200,
     * and seeds immediately — the fold clock reset with the state. */
    s_displayClock += SNAP_MS;
    UT_ASSERT(pingDisplayPush(&d, 30, s_displayClock) == 30);
  }

  /* (3g) Colour-band hysteresis: a player parked on a threshold holds its
   * band until the value clears it by the margin, in both directions. */
  {
    PingDisplay d;
    pingDisplayReset(&d);
    s_displayClock = 1000;

    displaySettle(&d, 40, 3000);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_GOOD);
    /* Past the 50ms threshold but inside the margin — still green. */
    displaySettle(&d, 55, 3000);
    UT_ASSERT_MSG(pingDisplayBand(&d) == PING_BAND_GOOD,
                  "band flipped inside the rising margin (%u)",
                  pingDisplayValue(&d));
    displaySettle(&d, 65, 3000);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_FAIR);
    /* Back under 50 but inside the margin — stays yellow. */
    displaySettle(&d, 45, 3000);
    UT_ASSERT_MSG(pingDisplayBand(&d) == PING_BAND_FAIR,
                  "band flipped inside the falling margin (%u)",
                  pingDisplayValue(&d));
    displaySettle(&d, 35, 3000);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_GOOD);

    /* Same margins at the fair/poor boundary. */
    displaySettle(&d, 155, 6000);
    UT_ASSERT_MSG(pingDisplayBand(&d) == PING_BAND_FAIR,
                  "poor band entered early (%u)", pingDisplayValue(&d));
    displaySettle(&d, 170, 6000);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_POOR);
    displaySettle(&d, 145, 6000);
    UT_ASSERT_MSG(pingDisplayBand(&d) == PING_BAND_POOR,
                  "poor band left early (%u)", pingDisplayValue(&d));
    displaySettle(&d, 130, 6000);
    UT_ASSERT(pingDisplayBand(&d) == PING_BAND_FAIR);
  }

  /* (3h) Stateless classification (used by the lobby, which has no per-slot
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
