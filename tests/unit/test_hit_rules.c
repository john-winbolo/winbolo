/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The hit rules the engine and the Log Viewer share (hit_rules.h) give the
 * same answers as the engine.
 *
 *   hit_rules_builder_matches_engine - hitRulesBuilderCaught, the rule the
 *       Log Viewer draws the builder's hit box from, against the engine's
 *       own builder death test today (lgmDeathCheckAtPosition on a running
 *       sim). Every lgm_blast_shape, pill ranges from 0 to 2048, open
 *       ground and a building beside him, and explosions on a grid of
 *       places within 200 of the builder.
 *
 *   hit_rules_tank_matches_engine - hitRulesTankCircle and
 *       hitRulesTankSquare against the old tankShellInHitZone sums, for
 *       every place within 300 of the tank and every radius the rule
 *       allows.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "global.h"
#include "util.h"
#include "hit_rules.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim */
#include "game_sim.h"              /* GameSim: mp/bs/pb, tanks[], lgmen[] */
#include "bolo_map.h"              /* mapSetPos */
#include "pillbox.h"               /* pillsExistPos */
#include "bases.h"                 /* basesExistPos */
#include "lgm.h"                   /* lgmDeathCheckAtPosition */
#include "test_harness.h"

/* The grass round the builder's square: 2 squares each way, more than the
   200 world units the explosions go out to. */
#define HR_PATCH 2

/* A running sim with grass round a square that has no pillbox and no base
   near it. The square is written to *cx, *cy. */
static ServerSim *hrMakeSim(BYTE *cx, BYTE *cy) {
  ServerSim *sim = ut_make_running_sim("HitRules");
  GameSim *gs;
  int x, y, dx, dy;

  if (sim == NULL) return NULL;
  gs = &sim->sim;
  if (gs->lgmen[0] == NULL || gs->tanks[0] == NULL) {
    fprintf(stderr, "  the round built no builder for slot 0\n");
    serverSimDestroy(sim);
    return NULL;
  }
  for (y = 30; y < 220; y++) {
    for (x = 30; x < 220; x++) {
      bool clear = true;
      for (dy = -HR_PATCH; dy <= HR_PATCH && clear; dy++) {
        for (dx = -HR_PATCH; dx <= HR_PATCH && clear; dx++) {
          if (pillsExistPos(&gs->pb, (BYTE)(x + dx), (BYTE)(y + dy)) == TRUE ||
              basesExistPos(&gs->bs, (BYTE)(x + dx), (BYTE)(y + dy)) == TRUE) {
            clear = false;
          }
        }
      }
      if (clear) {
        for (dy = -HR_PATCH; dy <= HR_PATCH; dy++) {
          for (dx = -HR_PATCH; dx <= HR_PATCH; dx++) {
            mapSetPos(gs, &gs->mp, (BYTE)(x + dx), (BYTE)(y + dy), GRASS, FALSE, FALSE);
          }
        }
        *cx = (BYTE)x;
        *cy = (BYTE)y;
        return sim;
      }
    }
  }
  fprintf(stderr, "  no square on the map is clear of pillboxes and bases\n");
  serverSimDestroy(sim);
  return NULL;
}

/* Stand the builder of slot 0 out on (lx, ly), set off an explosion at
   (wx, wy), and say whether the engine killed him. */
static bool hrEngineKills(ServerSim *sim, WORLD lx, WORLD ly, WORLD wx, WORLD wy) {
  GameSim *gs = &sim->sim;
  gs->lgmen[0]->inTank = FALSE;
  gs->lgmen[0]->isDead = FALSE;
  gs->lgmen[0]->x = lx;
  gs->lgmen[0]->y = ly;
  lgmDeathCheckAtPosition(gs, &gs->lgmen[0], lx, ly, wx, wy, NEUTRAL, NEUTRAL,
                          DMG_SRC_UNKNOWN, DMG_NO_PILL, &gs->tanks[0], FALSE);
  return gs->lgmen[0]->isDead != FALSE;
}

int run_hit_rules_builder_matches_engine(void) {
  static const int ranges[] = {0, 100, 128, 140, 160, 2048};
  /* The builder's place in his square: the middle, and off it. */
  static const int places[][2] = {{128, 128}, {30, 200}, {250, 5}};
  ServerSim *sim;
  BYTE cx = 0, cy = 0;
  int shape, building, dx, dy;
  size_t r, p;
  long checked = 0;

  sim = hrMakeSim(&cx, &cy);
  UT_ASSERT_MSG(sim != NULL, "could not build a running sim");
  for (building = 0; building < 2; building++) {
    /* The second pass puts a building on the square to his right. */
    mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)(cx + 1), cy, building ? BUILDING : GRASS,
              FALSE, FALSE);
    for (shape = 0; shape <= 2; shape++) {
      sim->sim.rules.lgm_blast_shape = shape;
      for (r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
        sim->sim.rules.pill_range = ranges[r];
        for (p = 0; p < sizeof(places) / sizeof(places[0]); p++) {
          WORLD lx = (WORLD)(((WORLD)cx << M_W_SHIFT_SIZE) + places[p][0]);
          WORLD ly = (WORLD)(((WORLD)cy << M_W_SHIFT_SIZE) + places[p][1]);
          for (dy = -200; dy <= 200; dy += 5) {
            for (dx = -200; dx <= 200; dx += 5) {
              WORLD wx = (WORLD)((long)lx + dx), wy = (WORLD)((long)ly + dy);
              BYTE mx = (BYTE)(wx >> M_W_SHIFT_SIZE), my = (BYTE)(wy >> M_W_SHIFT_SIZE);
              bool solid = building && mx == (BYTE)(cx + 1) && my == cy;
              bool want = hrEngineKills(sim, lx, ly, wx, wy);
              bool got = hitRulesBuilderCaught(solid, mx == cx && my == cy, (double)dx,
                                               (double)dy, shape, ranges[r]);
              if (want != got) {
                serverSimDestroy(sim);
                UT_ASSERT_MSG(false,
                              "shape %d, pill range %d, building %d, builder at %d,%d in his "
                              "square, explosion %d,%d off: engine %d, hit_rules.h %d",
                              shape, ranges[r], building, places[p][0], places[p][1], dx, dy,
                              (int)want, (int)got);
              }
              checked++;
            }
          }
        }
      }
    }
  }
  serverSimDestroy(sim);
  UT_ASSERT_MSG(checked > 0, "no places checked");

  /* The solid squares: only his own. */
  UT_ASSERT(hitRulesBuilderCaught(true, true, 500.0, 500.0, LGM_BLAST_SHAPE_HALF_SQUARE, 2048));
  UT_ASSERT(!hitRulesBuilderCaught(true, false, 0.0, 0.0, LGM_BLAST_SHAPE_HALF_SQUARE, 2048));
  UT_ASSERT(hitRulesBuilderCaught(false, false, 0.0, 128.0, LGM_BLAST_SHAPE_HALF_SQUARE, 2048));
  UT_ASSERT(!hitRulesBuilderCaught(false, true, 0.0, 129.0, LGM_BLAST_SHAPE_HALF_SQUARE, 2048));
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
