/*
 * Pure-math tests for the render-only remote-shell forward-projection
 * (clientShellProject / clientShellProjectAgeTicks). These exercise the
 * projection equation, the velocity-derivation basis, the age cap, and the
 * zero-age identity without needing a ClientSim.
 *
 * The projection anchors a snapshot shell at snap + velocity*ageTicks, where
 * velocity is the per-game-tick (20ms) step derived from angle and the shell
 * speed the caller hands in — the same basis the predicted (own) shells
 * advance on. A capped age bounds a wild ping estimate.
 *
 * The speed is a parameter rather than a constant, so the last case here
 * raises it and watches the projection fly further: a sim that tunes
 * shell_speed moves its remote shells with it.
 */

#include <math.h>
#include <stdint.h>

#include "global.h"        /* BRADIANS_EAST */
#include "sim_rules.h"     /* simRulesClassic — the speed under test */
#include "client_sim.h"    /* clientShellProject, clientShellProjectAgeTicks, PROJECTION_MAX_TICKS */
#include "test_harness.h"

static float vmag(float vx, float vy) { return sqrtf(vx * vx + vy * vy); }

int run_shell_projection(void) {
  SimRules classic;
  float EXPECTED_SHELL_SPEED;

  simRulesClassic(&classic);
  EXPECTED_SHELL_SPEED = (float) classic.shell_speed;

  /* (a) Velocity magnitude matches the shell speed per tick, and the anchored
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
        clientShellProject(snapX, snapY, angles[a], age, classic.shell_speed, &fx, &fy, &vx, &vy);
        UT_ASSERT_MSG(fabsf(vmag(vx, vy) - EXPECTED_SHELL_SPEED) < 0.5f,
                      "angle=%u vel magnitude %.3f", angles[a], vmag(vx, vy));
        UT_ASSERT_MSG(fabsf(fx - ((float)snapX + vx * (float)age)) < 0.001f,
                      "angle=%u age=%d fx mismatch", angles[a], age);
        UT_ASSERT_MSG(fabsf(fy - ((float)snapY + vy * (float)age)) < 0.001f,
                      "angle=%u age=%d fy mismatch", angles[a], age);
      }
    }
  }

  /* Axis-aligned sanity: angle == BRADIANS_EAST → +X only (vx=+the shell
   * speed, vy=0), so the shell projects straight along X by vel*age. */
  {
    float fx, fy, vx, vy;
    int age = 5;
    clientShellProject(10000, 20000, (uint8_t)BRADIANS_EAST, age, classic.shell_speed, &fx, &fy, &vx, &vy);
    UT_ASSERT_MSG(fabsf(vx - EXPECTED_SHELL_SPEED) < 0.5f, "east vx=%.3f", vx);
    UT_ASSERT_MSG(fabsf(vy) < 0.5f, "east vy=%.3f", vy);
    UT_ASSERT(fabsf(fx - (10000.0f + EXPECTED_SHELL_SPEED * age)) < 1.0f);
    UT_ASSERT(fabsf(fy - 20000.0f) < 1.0f);
  }

  /* (b) Age cap: a wild ping clamps to PROJECTION_MAX_TICKS, and the
   * projected distance never exceeds the speed * PROJECTION_MAX_TICKS. */
  {
    int capped = clientShellProjectAgeTicks(60000);  /* absurd ping */
    UT_ASSERT_MSG(capped == PROJECTION_MAX_TICKS, "capped=%d", capped);

    /* Basis check: one-way latency / 20ms. ping=400 → 200ms → 10 ticks. */
    UT_ASSERT(clientShellProjectAgeTicks(400) == 10);
    UT_ASSERT(clientShellProjectAgeTicks(80) == 2);

    {
      float fx, fy, vx, vy;
      float dist;
      clientShellProject(10000, 20000, 96, capped, classic.shell_speed, &fx, &fy, &vx, &vy);
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
      clientShellProject(12345, 23456, angles[a], 0, classic.shell_speed, &fx, &fy, &vx, &vy);
      UT_ASSERT_MSG(fx == 12345.0f, "angle=%u fx=%.3f", angles[a], fx);
      UT_ASSERT_MSG(fy == 23456.0f, "angle=%u fy=%.3f", angles[a], fy);
    }
  }

  /* Zero ping → zero age → no projection. */
  UT_ASSERT(clientShellProjectAgeTicks(0) == 0);

  /* (d) The speed is the rule's, not a constant. Double it and one tick of
   * flight doubles with it — the projection follows shell_speed rather than
   * the 32 the classic table happens to carry. Along east so the whole step
   * lands on X and the comparison is the speed itself. */
  {
    float fx1, fy1, vx1, vy1;
    float fx2, fy2, vx2, vy2;
    const int fast = classic.shell_speed * 2;
    clientShellProject(10000, 20000, (uint8_t)BRADIANS_EAST, 1,
                       classic.shell_speed, &fx1, &fy1, &vx1, &vy1);
    clientShellProject(10000, 20000, (uint8_t)BRADIANS_EAST, 1,
                       fast, &fx2, &fy2, &vx2, &vy2);
    UT_ASSERT_MSG(fabsf(vx2 - (float) fast) < 0.5f,
                  "a shell at speed %d steps %.3f per tick", fast, vx2);
    UT_ASSERT_MSG(vx2 > vx1 + 1.0f,
                  "raising the speed did not move the step: %.3f then %.3f",
                  vx1, vx2);
    UT_ASSERT_MSG(fx2 - 10000.0f > fx1 - 10000.0f + 1.0f,
                  "raising the speed did not move the shell: %.3f then %.3f",
                  fx1, fx2);
  }

  return 0;
}
