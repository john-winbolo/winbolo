/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The hit rules the engine and the Log Viewer share (hit_rules.h) give the
 * same answers as the sums the engine made before they moved there.
 *
 *   hit_rules_builder_matches_engine - hitRulesBuilderOpenGround against
 *       the old lgmBlastCatches sum: the distance from utilIsItemInRange
 *       with pill_range as its limit, caught when it is no more than 128.
 *       Every whole-number place within 200 of the builder, for pill
 *       ranges from 0 to 65535, near the map's edge too.
 *
 *   hit_rules_tank_matches_engine - hitRulesTankCircle and
 *       hitRulesTankSquare against the old tankShellInHitZone sums, for
 *       every place within 300 of the tank and every radius the rule
 *       allows.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "global.h"
#include "util.h"
#include "hit_rules.h"
#include "test_harness.h"

/* The open-ground part of lgmBlastCatches as it was. */
static bool oldBuilderOpenGround(WORLD lgmX, WORLD lgmY, WORLD wx, WORLD wy, WORLD pillRange) {
  double distance;
  utilIsItemInRange(lgmX, lgmY, wx, wy, pillRange, &distance);
  return distance <= 128;
}

int run_hit_rules_builder_matches_engine(void) {
  static const int ranges[] = {0, 1, 50, 100, 127, 128, 129, 181, 200, 2048, 65535};
  static const int places[][2] = {{1000, 1000}, {100, 60}, {65400, 65500}};
  size_t r, p;
  int dx, dy;

  for (p = 0; p < sizeof(places) / sizeof(places[0]); p++) {
    WORLD lx = (WORLD)places[p][0], ly = (WORLD)places[p][1];
    for (r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
      for (dy = -200; dy <= 200; dy++) {
        for (dx = -200; dx <= 200; dx++) {
          int wx = (int)lx + dx, wy = (int)ly + dy;
          bool want, got;
          if (wx < 0 || wy < 0 || wx > 65535 || wy > 65535) continue;
          want = oldBuilderOpenGround(lx, ly, (WORLD)wx, (WORLD)wy, (WORLD)ranges[r]);
          got = hitRulesBuilderOpenGround((double)(wx - (int)lx), (double)(wy - (int)ly),
                                          ranges[r]);
          UT_ASSERT_MSG(want == got,
                        "builder at %d,%d, explosion %d,%d off, pill range %d: was %d, now %d",
                        (int)lx, (int)ly, dx, dy, ranges[r], (int)want, (int)got);
        }
      }
    }
  }
  /* The solid squares: only his own. */
  UT_ASSERT(hitRulesBuilderCaught(true, true, 500.0, 500.0, 2048));
  UT_ASSERT(!hitRulesBuilderCaught(true, false, 0.0, 0.0, 2048));
  UT_ASSERT(hitRulesBuilderCaught(false, false, 0.0, 128.0, 2048));
  UT_ASSERT(!hitRulesBuilderCaught(false, true, 0.0, 129.0, 2048));
  return 0;
}

int run_hit_rules_tank_matches_engine(void) {
  int radius, dx, dy;

  for (dy = -300; dy <= 300; dy++) {
    for (dx = -300; dx <= 300; dx++) {
      bool oldSquare = (abs(dx) < 128 && abs(dy) < 128);
      UT_ASSERT_MSG(hitRulesTankSquare(dx, dy) == oldSquare, "square, %d,%d", dx, dy);
    }
  }
  for (radius = 1; radius <= 255; radius++) {
    for (dy = -300; dy <= 300; dy += (radius % 7 == 0) ? 1 : 3) {
      for (dx = -300; dx <= 300; dx++) {
        bool oldCircle = (abs(dx) < radius && abs(dy) < radius &&
                          (dx * dx + dy * dy) < radius * radius);
        UT_ASSERT_MSG(hitRulesTankCircle(dx, dy, radius) == oldCircle,
                      "circle of %d, %d,%d", radius, dx, dy);
      }
    }
  }
  /* Far apart, where the squares would not fit in an int without the test
     on each axis first. */
  UT_ASSERT(!hitRulesTankCircle(65535, 65535, 112));
  UT_ASSERT(!hitRulesTankCircle(-65535, 3, 112));
#ifdef BOLO_LEGACY_SQUARE_COLLISION
  UT_ASSERT(hitRulesTankShellInZone(120, 120, 112));
#else
  UT_ASSERT(!hitRulesTankShellInZone(120, 120, 112));
#endif
  return 0;
}
