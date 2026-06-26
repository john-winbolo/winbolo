/*
 * Render-only error smoothing (test_error_smoothing.c).
 *
 * Covers the pure offset math used by reconciliation to slide corrections
 * instead of snapping them: accumulation composes, exponential decay drains
 * toward zero at the 40ms time constant, a per-component clamp exceed zeroes
 * everything (so a genuine teleport snaps), angle deltas wrap across the
 * 0/256 boundary, and a zero delta is a no-op. Plus one sim-level case:
 * clientSimResetWorld zeroes the offset fields.
 *
 * Always built (no WB_NETDEBUG gate) — the math is sim-independent.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"   /* err fields + pure-math declarations */
#include "test_harness.h"

#define APPROX(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (float)(eps))

int run_error_smoothing(void) {
  /* Accumulate composes: two deltas sum on each axis. */
  {
    float ex = 0.0f, ey = 0.0f, ea = 0.0f;
    bool zeroed;
    zeroed = clientErrSmoothAccumulate(&ex, &ey, &ea, 10.0f, -4.0f, 2.0f,
                                       CLIENT_ERR_POS_CLAMP);
    UT_ASSERT_MSG(!zeroed, "first small accumulate should not clamp");
    zeroed = clientErrSmoothAccumulate(&ex, &ey, &ea, 5.0f, -1.0f, 1.0f,
                                       CLIENT_ERR_POS_CLAMP);
    UT_ASSERT_MSG(!zeroed, "second small accumulate should not clamp");
    UT_ASSERT_MSG(APPROX(ex, 15.0f, 0.001f), "errX compose: %f != 15", ex);
    UT_ASSERT_MSG(APPROX(ey, -5.0f, 0.001f), "errY compose: %f != -5", ey);
    UT_ASSERT_MSG(APPROX(ea, 3.0f, 0.001f), "errAngle compose: %f != 3", ea);
  }

  /* Zero-input no-op: an all-zero delta leaves the offset untouched. */
  {
    float ex = 7.0f, ey = -2.0f, ea = 1.5f;
    bool zeroed = clientErrSmoothAccumulate(&ex, &ey, &ea, 0.0f, 0.0f, 0.0f,
                                            CLIENT_ERR_POS_CLAMP);
    UT_ASSERT_MSG(!zeroed, "zero delta should not clamp");
    UT_ASSERT_MSG(APPROX(ex, 7.0f, 0.001f) && APPROX(ey, -2.0f, 0.001f) &&
                      APPROX(ea, 1.5f, 0.001f),
                  "zero delta changed offset: (%f,%f,%f)", ex, ey, ea);
  }

  /* Decay halves toward zero at the expected rate: 28ms ≈ e^-0.7 ≈ 0.5×. */
  {
    float ex = 100.0f, ey = -80.0f, ea = 8.0f;
    clientErrSmoothDecay(&ex, &ey, &ea, 28.0f);
    UT_ASSERT_MSG(APPROX(ex, 50.0f, 1.0f), "decay 28ms errX: %f !~ 50", ex);
    UT_ASSERT_MSG(APPROX(ey, -40.0f, 1.0f), "decay 28ms errY: %f !~ -40", ey);
    UT_ASSERT_MSG(APPROX(ea, 4.0f, 0.2f), "decay 28ms errAngle: %f !~ 4", ea);
  }

  /* Decay reaches ~0 within ~120ms of 20ms display-tick steps. */
  {
    float ex = 200.0f, ey = 120.0f, ea = 12.0f;
    int i;
    for (i = 0; i < 6; i++) { /* 6 × 20ms = 120ms */
      clientErrSmoothDecay(&ex, &ey, &ea, 20.0f);
    }
    UT_ASSERT_MSG(fabsf(ex) < 10.0f && fabsf(ey) < 10.0f && fabsf(ea) < 1.0f,
                  "120ms decay residue too large: (%f,%f,%f)", ex, ey, ea);
  }

  /* Per-axis clamp exceed zeroes everything and returns true. */
  {
    float ex = 0.0f, ey = 0.0f, ea = 0.0f;
    bool zeroed =
        clientErrSmoothAccumulate(&ex, &ey, &ea, CLIENT_ERR_POS_CLAMP + 1.0f,
                                  0.0f, 0.0f, CLIENT_ERR_POS_CLAMP);
    UT_ASSERT_MSG(zeroed, "position over-clamp should report zeroed");
    UT_ASSERT_MSG(ex == 0.0f && ey == 0.0f && ea == 0.0f,
                  "position over-clamp left residue: (%f,%f,%f)", ex, ey, ea);
  }
  {
    /* A standing offset plus a delta that pushes the angle past its clamp
     * zeroes ALL components, not just the angle. */
    float ex = 30.0f, ey = -20.0f, ea = 10.0f;
    bool zeroed = clientErrSmoothAccumulate(&ex, &ey, &ea, 0.0f, 0.0f, 10.0f,
                                            CLIENT_ERR_POS_CLAMP);
    UT_ASSERT_MSG(zeroed, "angle over-clamp should report zeroed");
    UT_ASSERT_MSG(ex == 0.0f && ey == 0.0f && ea == 0.0f,
                  "angle over-clamp left residue: (%f,%f,%f)", ex, ey, ea);
  }

  /* Angle wrap: a delta across the 0/256 boundary is the small rotation it
   * represents, not ~256. predAngle 255, postAngle 1 → delta 254 wraps to
   * -2, which is inside the clamp and accumulates as -2. */
  {
    float ex = 0.0f, ey = 0.0f, ea = 0.0f;
    bool zeroed = clientErrSmoothAccumulate(&ex, &ey, &ea, 0.0f, 0.0f, 254.0f,
                                            CLIENT_ERR_POS_CLAMP);
    UT_ASSERT_MSG(!zeroed, "wrapped small angle delta should not clamp");
    UT_ASSERT_MSG(APPROX(ea, -2.0f, 0.001f), "angle wrap: %f != -2", ea);
  }

  /* posClamp parameter governs the position threshold: a 300u single-axis
   * delta snaps (zeroes) under the normal clamp but slides under the enlarged
   * base-unblock clamp, accumulating intact. */
  {
    float ex = 0.0f, ey = 0.0f, ea = 0.0f;
    bool zeroed = clientErrSmoothAccumulate(&ex, &ey, &ea, 300.0f, 0.0f, 0.0f,
                                            CLIENT_ERR_POS_CLAMP);
    UT_ASSERT_MSG(zeroed, "300u delta should snap under normal clamp");
    UT_ASSERT_MSG(ex == 0.0f && ey == 0.0f && ea == 0.0f,
                  "300u normal-clamp snap left residue: (%f,%f,%f)", ex, ey, ea);

    ex = 0.0f; ey = 0.0f; ea = 0.0f;
    zeroed = clientErrSmoothAccumulate(&ex, &ey, &ea, 300.0f, 0.0f, 0.0f,
                                       CLIENT_ERR_POS_CLAMP_BASE_UNBLOCK);
    UT_ASSERT_MSG(!zeroed, "300u delta should slide under base-unblock clamp");
    UT_ASSERT_MSG(APPROX(ex, 300.0f, 0.001f),
                  "base-unblock clamp errX: %f != 300", ex);
  }

  /* Sim-level: clientSimResetWorld zeroes the offset fields. */
  {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    clientSimCreate(cs);
    cs->errX = 12.5f;
    cs->errY = -7.25f;
    cs->errAngle = 3.0f;
    clientSimResetWorld(cs);
    UT_ASSERT_MSG(cs->errX == 0.0f && cs->errY == 0.0f && cs->errAngle == 0.0f,
                  "resetWorld left offset: (%f,%f,%f)", cs->errX, cs->errY,
                  cs->errAngle);
    clientSimDestroy(cs);
  }

  return 0;
}
