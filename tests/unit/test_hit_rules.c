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
 *       places within 200 of the builder, with the shapes' edges (127,
 *       128, 129, 144 and 145 on each axis, and either side of the two
 *       circles and the box's corner on the diagonals).
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
  /* The edges on an axis: the box's 128 (edge out), the half-square
     circle's 128 and the area circle's 144 (edge in). */
  static const int axisEdges[] = {127, 128, 129, 144, 145};
  /* On the diagonal: 90,91 is in the circle of 128 and 91,91 out; 101,102
     is in the circle of 144 and 102,102 out; 127,127 is in the box and
     127,128 and 128,128 out. */
  static const int diagEdges[][2] = {{90, 90}, {90, 91}, {91, 91}, {101, 102},
                                     {102, 102}, {127, 127}, {127, 128}, {128, 128}};
  static int offsets[81 * 81 + 5 * 4 + 8 * 8][2];
  ServerSim *sim;
  BYTE cx = 0, cy = 0;
  int shape, building, dx, dy, sx, sy, o, count = 0;
  size_t r, p, e;
  long checked = 0;

  /* The grid every 5 world units, then the edges. */
  for (dy = -200; dy <= 200; dy += 5) {
    for (dx = -200; dx <= 200; dx += 5) {
      offsets[count][0] = dx;
      offsets[count][1] = dy;
      count++;
    }
  }
  for (e = 0; e < sizeof(axisEdges) / sizeof(axisEdges[0]); e++) {
    for (sx = -1; sx <= 1; sx += 2) {
      offsets[count][0] = sx * axisEdges[e];
      offsets[count][1] = 0;
      count++;
      offsets[count][0] = 0;
      offsets[count][1] = sx * axisEdges[e];
      count++;
    }
  }
  for (e = 0; e < sizeof(diagEdges) / sizeof(diagEdges[0]); e++) {
    for (sy = -1; sy <= 1; sy += 2) {
      for (sx = -1; sx <= 1; sx += 2) {
        offsets[count][0] = sx * diagEdges[e][0];
        offsets[count][1] = sy * diagEdges[e][1];
        count++;
        offsets[count][0] = sx * diagEdges[e][1];
        offsets[count][1] = sy * diagEdges[e][0];
        count++;
      }
    }
  }
  UT_ASSERT_MSG(count == (int)(sizeof(offsets) / sizeof(offsets[0])), "%d offsets", count);

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
          for (o = 0; o < count; o++) {
            dx = offsets[o][0];
            dy = offsets[o][1];
            {
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
  /* The engine's own answer at each shape's edge, below the builder (open
     ground) with a pill range that cuts nothing: in at the last world unit
     of the shape, out at the next. */
  {
    static const int edge[3][2] = {{128, 129}, {127, 128}, {144, 145}};
    WORLD lx = (WORLD)(((WORLD)cx << M_W_SHIFT_SIZE) + 128);
    WORLD ly = (WORLD)(((WORLD)cy << M_W_SHIFT_SIZE) + 128);
    int bad = -1;
    sim->sim.rules.pill_range = 2048;
    for (shape = 0; shape <= 2 && bad < 0; shape++) {
      sim->sim.rules.lgm_blast_shape = shape;
      if (!hrEngineKills(sim, lx, ly, lx, (WORLD)(ly + edge[shape][0])) ||
          hrEngineKills(sim, lx, ly, lx, (WORLD)(ly + edge[shape][1])) ||
          !hrEngineKills(sim, lx, ly, (WORLD)(lx - edge[shape][0]), ly) ||
          hrEngineKills(sim, lx, ly, (WORLD)(lx - edge[shape][1]), ly)) {
        bad = shape;
      }
    }
    /* The box's corner: 127,127 in, 127,128 out. */
    sim->sim.rules.lgm_blast_shape = LGM_BLAST_SHAPE_MAC_BOX;
    if (bad < 0 && (!hrEngineKills(sim, lx, ly, (WORLD)(lx - 127), (WORLD)(ly + 127)) ||
                    hrEngineKills(sim, lx, ly, (WORLD)(lx - 127), (WORLD)(ly + 128)))) {
      bad = LGM_BLAST_SHAPE_MAC_BOX;
    }
    serverSimDestroy(sim);
    UT_ASSERT_MSG(bad < 0, "shape %d: the engine's edge is not where the shape says", bad);
  }
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
