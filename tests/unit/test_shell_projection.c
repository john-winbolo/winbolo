/*
 * Pure-math tests for the render-only remote-shell forward-projection
 * (clientShellProject / clientShellProjectAgeTicks). These exercise the
 * projection equation, the velocity-derivation basis, the age cap, and the
 * zero-age identity without needing a ClientSim.
 *
 * The projection anchors a snapshot shell at snap + velocity*ageTicks, where
 * velocity is the per-game-tick (20ms) step derived from angle+SHELL_SPEED —
 * the same basis the predicted (own) shells advance on. A capped age bounds a
 * wild ping estimate.
 */

#include <math.h>
#include <stdint.h>

#include "global.h"        /* BRADIANS_EAST */
#include "client_sim.h"    /* clientShellProject, clientShellProjectAgeTicks, PROJECTION_MAX_TICKS */
#include "test_harness.h"

/* Shells travel SHELL_SPEED world units per game tick (internal/shells.h);
 * the velocity magnitude must match it. */
#define EXPECTED_SHELL_SPEED 32.0f

static float vmag(float vx, float vy) { return sqrtf(vx * vx + vy * vy); }

int run_shell_projection(void) {
  /* (a) Velocity magnitude matches SHELL_SPEED per tick, and the anchored
   * position equals snap + vel*ageTicks, across several angles/ages. */
  {
    const uint8_t angles[] = { 0, 32, 64, 96, 128, 200, 255 };
    const int ages[] = { 1, 3, 7 };
    size_t a, g;
    for (a = 0; a < sizeof(angles) / sizeof(angles[0]); a++) {
      for (g = 0; g < sizeof(ages) / sizeof(ages[0]); g++) {
        float fx, fy, vx, vy;
        uint16_t snapX = 10000, snapY = 20000;
        int age = ages[g];
        clientShellProject(snapX, snapY, angles[a], age, &fx, &fy, &vx, &vy);
        UT_ASSERT_MSG(fabsf(vmag(vx, vy) - EXPECTED_SHELL_SPEED) < 0.5f,
                      "angle=%u vel magnitude %.3f", angles[a], vmag(vx, vy));
        UT_ASSERT_MSG(fabsf(fx - ((float)snapX + vx * (float)age)) < 0.001f,
                      "angle=%u age=%d fx mismatch", angles[a], age);
        UT_ASSERT_MSG(fabsf(fy - ((float)snapY + vy * (float)age)) < 0.001f,
                      "angle=%u age=%d fy mismatch", angles[a], age);
      }
    }
  }

  /* Axis-aligned sanity: angle == BRADIANS_EAST → +X only (vx=+SHELL_SPEED,
   * vy=0), so the shell projects straight along X by vel*age. */
  {
    float fx, fy, vx, vy;
    int age = 5;
    clientShellProject(10000, 20000, (uint8_t)BRADIANS_EAST, age, &fx, &fy, &vx, &vy);
    UT_ASSERT_MSG(fabsf(vx - EXPECTED_SHELL_SPEED) < 0.5f, "east vx=%.3f", vx);
    UT_ASSERT_MSG(fabsf(vy) < 0.5f, "east vy=%.3f", vy);
    UT_ASSERT(fabsf(fx - (10000.0f + EXPECTED_SHELL_SPEED * age)) < 1.0f);
    UT_ASSERT(fabsf(fy - 20000.0f) < 1.0f);
  }

  /* (b) Age cap: a wild ping clamps to PROJECTION_MAX_TICKS, and the
   * projected distance never exceeds SHELL_SPEED * PROJECTION_MAX_TICKS. */
  {
    int capped = clientShellProjectAgeTicks(60000);  /* absurd ping */
    UT_ASSERT_MSG(capped == PROJECTION_MAX_TICKS, "capped=%d", capped);

    /* Basis check: one-way latency / 20ms. ping=400 → 200ms → 10 ticks. */
    UT_ASSERT(clientShellProjectAgeTicks(400) == 10);
    UT_ASSERT(clientShellProjectAgeTicks(80) == 2);

    {
      float fx, fy, vx, vy;
      float dist;
      clientShellProject(10000, 20000, 96, capped, &fx, &fy, &vx, &vy);
      dist = sqrtf((fx - 10000.0f) * (fx - 10000.0f) +
                   (fy - 20000.0f) * (fy - 20000.0f));
      UT_ASSERT_MSG(dist <= EXPECTED_SHELL_SPEED * PROJECTION_MAX_TICKS + 1.0f,
                    "capped dist=%.3f", dist);
    }
  }

  /* (c) Zero age → position equals the raw snapshot position exactly. */
  {
    const uint8_t angles[] = { 0, 64, 128, 200 };
    size_t a;
    for (a = 0; a < sizeof(angles) / sizeof(angles[0]); a++) {
      float fx, fy, vx, vy;
      clientShellProject(12345, 23456, angles[a], 0, &fx, &fy, &vx, &vy);
      UT_ASSERT_MSG(fx == 12345.0f, "angle=%u fx=%.3f", angles[a], fx);
      UT_ASSERT_MSG(fy == 23456.0f, "angle=%u fy=%.3f", angles[a], fy);
    }
  }

  /* Zero ping → zero age → no projection. */
  UT_ASSERT(clientShellProjectAgeTicks(0) == 0);

  return 0;
}
