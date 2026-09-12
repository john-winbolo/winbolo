/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Brain Pathfinder
 *Filename:      brain_pathfinder.c
 *Author:        John Morrison
 *Purpose:
 *  C-accelerated A* pathfinder for Lua brains.
 *  Implements grid-based A* with danger/overlay grids,
 *  wall-shooting, road-building, cost estimation,
 *  boat/land state transitions, and resource drain
 *  tracking (shells/mines lost to water, armour lost
 *  to pill danger).
 *
 *  The A* state space is (x, y, boat) where boat is 0 or 1.
 *  Transitions: boat->land when stepping off water onto land,
 *  land->boat when stepping onto a T_BOAT pickup tile.
 *
 *  Resource tracking: the search tracks shells, trees, mines,
 *  and armour at each node.  Paths that would arrive with
 *  fewer resources than the configured minimums are pruned.
 *  Resource losses also add to the A* cost so the search
 *  prefers paths with less drain.
 *********************************************************/

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <SDL3/SDL.h>
#include <zlib.h>

#include "brain_pathfinder.h"
#include "util.h"
#include "lgm.h"
#include "sim_rules.h"
#include "tank.h"
#include "shells.h"
#include "pillbox.h"

/* High-res microseconds via SDL — same source as the Lua-side clock_us */
static double bp_now_us(void) {
  return (double)SDL_GetPerformanceCounter() /
         (double)SDL_GetPerformanceFrequency() * 1000000.0;
}

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAP_SIZE 256
#define GRID_SIZE 65536
#define NODE_COUNT 131072          /* GRID_SIZE * 2 (land + boat layers) */
#define BOAT_OFFSET 65536          /* node index offset for boat=1 */
#define HEAP_INITIAL_CAPACITY 131072

/* 8-directional neighbor offsets: N, NE, E, SE, S, SW, W, NW */
static const int DX8[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
static const int DY8[8] = {-1, -1,  0,  1,  1,  1,  0, -1 };
/* Cost multiplier: 1.0 for cardinal, ~1.41 for diagonal */
static const float DMUL8[8] = { 1.0f, 1.41f, 1.0f, 1.41f, 1.0f, 1.41f, 1.0f, 1.41f };

/* Sentinel for g_cost (infinity) */
#define COST_INF 1e30f

/* Sentinel parent value for "no parent" / source node */
#define PARENT_NONE 0xFFFFFFFFu

/* Wall cost threshold — terrain costs >= this are treated as walls */
#define WALL_THRESHOLD 9999.0f

/* Terrain type constants */
#define TT_BUILDING  0
#define TT_RIVER     1
#define TT_SWAMP     2
#define TT_CRATER    3
#define TT_ROAD      4
#define TT_FOREST    5
#define TT_RUBBLE    6
#define TT_GRASS     7
#define TT_HALFBUILD 8
#define TT_BOAT      9
#define TT_DEEPSEA  10

/* ------------------------------------------------------------------ */
/* Node index helpers                                                  */
/* ------------------------------------------------------------------ */

/* Encode (x, y, boat) into a single node index (0..131071) */
static inline int node_idx(int x, int y, int boat) {
  return (boat ? BOAT_OFFSET : 0) + y * MAP_SIZE + x;
}

/* Extract spatial map index from a node index (strip boat bit) */
static inline int map_idx(int ni) {
  return ni & 0xFFFF;
}

/* Extract tile x from a node index */
static inline int node_x(int ni) {
  return (ni & 0xFFFF) % MAP_SIZE;
}

/* Extract tile y from a node index */
static inline int node_y(int ni) {
  return (ni & 0xFFFF) / MAP_SIZE;
}

/* Is this a water tile where the boat stays afloat? */
static inline int is_water_tile(int type) {
  return type == TT_RIVER || type == TT_DEEPSEA;
}

/* Determine the boat state after stepping onto a tile.
 * cur_boat: current boat state (0 or 1)
 * type: terrain type of the destination tile
 * Returns: new boat state (0 or 1) */
static inline int next_boat_state(int cur_boat, int type) {
  if (type == TT_BOAT) return 1;                      /* pick up boat */
  if (cur_boat && is_water_tile(type)) return 1;       /* stay in boat on water */
  return 0;                                            /* on land (or lost boat) */
}

/* ------------------------------------------------------------------ */
/* Heap operations (array-based 4-ary min-heap)                         */
/*                                                                      */
/* 4-ary heap: each node has 4 children. Tree height is half a binary   */
/* heap, sift_down does ~1.3Ã— the comparisons but ~0.5Ã— the cache       */
/* misses since 4 siblings live in adjacent memory. Net ~25% faster on  */
/* the heap-heavy phases of A-star and Dijkstra.                       */
/* ------------------------------------------------------------------ */

#define HEAP_ARITY 4
#define HEAP_PARENT(i) (((i) - 1) / HEAP_ARITY)
#define HEAP_FIRST_CHILD(i) ((i) * HEAP_ARITY + 1)

static void heap_clear(BrainPathfinder *pf) {
  pf->open_count = 0;
}

static void heap_push(BrainPathfinder *pf, float f, uint16_t x, uint16_t y, uint8_t boat) {
  int i, parent;
  BrainPFHeapEntry *h;

  if (pf->open_count >= pf->heap_capacity) {
    return;
  }

  h = pf->heap;
  i = pf->open_count++;
  h[i].f = f;
  h[i].x = x;
  h[i].y = y;
  h[i].boat = boat;

  /* Sift up — 4-ary parent */
  while (i > 0) {
    parent = HEAP_PARENT(i);
    if (h[parent].f <= h[i].f) break;
    BrainPFHeapEntry tmp = h[parent];
    h[parent] = h[i];
    h[i] = tmp;
    i = parent;
  }
}

static BrainPFHeapEntry heap_pop(BrainPathfinder *pf) {
  BrainPFHeapEntry *h = pf->heap;
  BrainPFHeapEntry result = h[0];
  int n = --pf->open_count;
  int i;

  if (n <= 0) return result;

  h[0] = h[n];

  /* Sift down — 4-ary: pick the smallest of up to 4 children */
  i = 0;
  for (;;) {
    int c0 = HEAP_FIRST_CHILD(i);
    if (c0 >= n) break;
    int best = c0;
    /* Unrolled comparison of the 4 children. Each c is bounds-checked. */
    int c1 = c0 + 1;
    int c2 = c0 + 2;
    int c3 = c0 + 3;
    if (c1 < n && h[c1].f < h[best].f) best = c1;
    if (c2 < n && h[c2].f < h[best].f) best = c2;
    if (c3 < n && h[c3].f < h[best].f) best = c3;
    if (h[i].f <= h[best].f) break;
    BrainPFHeapEntry tmp = h[best];
    h[best] = h[i];
    h[i] = tmp;
    i = best;
  }

  return result;
}

/* ------------------------------------------------------------------ */
/* Per-slate heap operations for the incremental Dijkstra              */
/*                                                                      */
/* 4-ary heap operating on a specific DijkstraSlate's heap so each      */
/* slate is fully independent and cost_to / path_to running between     */
/* Dijkstra steps can't trample state.                                  */
/* ------------------------------------------------------------------ */

static void slate_heap_clear(DijkstraSlate *s) {
  s->open_count = 0;
  /* 0xFF byte pattern gives int32 = -1 via two's complement, marking
   * every slot as "not in heap" in a single memset. */
  if (s->heap_pos) memset(s->heap_pos, 0xFF, NODE_COUNT * sizeof(int32_t));
}

static void slate_heap_push(DijkstraSlate *s, float f, uint16_t x, uint16_t y, uint8_t boat) {
  int i, parent;
  BrainPFHeapEntry *h = s->heap;
  int32_t *pos = s->heap_pos;
  int ni = node_idx(x, y, boat);
  int existing = pos[ni];
  if (existing < 0) {
    if (s->open_count >= s->heap_capacity) return;
    i = s->open_count++;
    h[i].f = f; h[i].x = x; h[i].y = y; h[i].boat = boat;
    pos[ni] = i;
  } else {
    /* Decrease-key: caller guarantees new f <= current f, so sift-up alone suffices. */
    i = existing;
    h[i].f = f;
  }
  while (i > 0) {
    parent = HEAP_PARENT(i);
    if (h[parent].f <= h[i].f) break;
    BrainPFHeapEntry tmp = h[parent];
    h[parent] = h[i];
    h[i] = tmp;
    pos[node_idx(h[i].x, h[i].y, h[i].boat)] = i;
    pos[node_idx(h[parent].x, h[parent].y, h[parent].boat)] = parent;
    i = parent;
  }
}

static BrainPFHeapEntry slate_heap_pop(DijkstraSlate *s) {
  BrainPFHeapEntry *h = s->heap;
  int32_t *pos = s->heap_pos;
  BrainPFHeapEntry result = h[0];
  int n = --s->open_count;
  int i;
  pos[node_idx(result.x, result.y, result.boat)] = -1;
  if (n <= 0) return result;
  h[0] = h[n];
  pos[node_idx(h[0].x, h[0].y, h[0].boat)] = 0;
  i = 0;
  for (;;) {
    int c0 = HEAP_FIRST_CHILD(i);
    if (c0 >= n) break;
    int best = c0;
    int c1 = c0 + 1, c2 = c0 + 2, c3 = c0 + 3;
    if (c1 < n && h[c1].f < h[best].f) best = c1;
    if (c2 < n && h[c2].f < h[best].f) best = c2;
    if (c3 < n && h[c3].f < h[best].f) best = c3;
    if (h[i].f <= h[best].f) break;
    BrainPFHeapEntry tmp = h[best];
    h[best] = h[i];
    h[i] = tmp;
    pos[node_idx(h[i].x, h[i].y, h[i].boat)] = i;
    pos[node_idx(h[best].x, h[best].y, h[best].boat)] = best;
    i = best;
  }
  return result;
}

/* ------------------------------------------------------------------ */
/* Closed bitset operations                                            */
/* ------------------------------------------------------------------ */

static inline void closed_set(uint8_t *closed, int idx) {
  closed[idx >> 3] |= (1u << (idx & 7));
}

static inline void closed_clear(uint8_t *closed, int idx) {
  closed[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}

static inline int closed_test(const uint8_t *closed, int idx) {
  return (closed[idx >> 3] >> (idx & 7)) & 1;
}

/* ------------------------------------------------------------------ */
/* Epoch versioning for the main A* working arrays                     */
/*                                                                      */
/* A fresh A* search calls epoch_bump() instead of zeroing the 2 MB of */
/* per-node arrays. stamp_node() tags a node with the current epoch    */
/* the first time this search touches it, and clears the node's old    */
/* closed bit so a leftover set bit from a prior search can't leak     */
/* through get_closed(). Same-epoch restamps are a cheap no-op — this   */
/* is the decrease-key hot path.                                        */
/* ------------------------------------------------------------------ */

static void epoch_bump(BrainPathfinder *pf) {
  /* Epoch 0 is reserved as "never touched". If the counter wraps back
   * to 0, reset every node to stale explicitly so the invariant holds. */
  uint32_t next = pf->current_epoch + 1u;
  if (next == 0u) {
    memset(pf->epoch, 0, sizeof(pf->epoch));
    next = 1u;
  }
  pf->current_epoch = next;
}

static inline void stamp_node(BrainPathfinder *pf, int i) {
  if (pf->epoch[i] != pf->current_epoch) {
    pf->epoch[i] = pf->current_epoch;
    /* closed[] is no longer zeroed between searches; clear any leftover
     * bit from a prior search before this node is used in this one. */
    closed_clear(pf->closed, i);
  }
}

static inline float get_g_cost(const BrainPathfinder *pf, int i) {
  return (pf->epoch[i] == pf->current_epoch) ? pf->g_cost[i] : COST_INF;
}

static inline int get_closed(const BrainPathfinder *pf, int i) {
  if (pf->epoch[i] != pf->current_epoch) return 0;
  return closed_test(pf->closed, i);
}

static inline uint32_t get_parent(const BrainPathfinder *pf, int i) {
  return (pf->epoch[i] == pf->current_epoch) ? pf->parent[i] : PARENT_NONE;
}

static inline uint8_t get_dir_at(const BrainPathfinder *pf, int i) {
  return (pf->epoch[i] == pf->current_epoch) ? pf->dir_at[i] : (uint8_t)0xFF;
}

/* ------------------------------------------------------------------ */
/* Direction helpers                                                    */
/* ------------------------------------------------------------------ */

/* Minimum angular steps (0-4) between two direction indices (0-7) */
static inline int dir_steps(int d1, int d2) {
  int diff = abs(d1 - d2);
  if (diff > 4) diff = 8 - diff;
  return diff;
}

/* ------------------------------------------------------------------ */
/* Heuristic                                                           */
/* ------------------------------------------------------------------ */

/* Chebyshev distance (8-directional consistent heuristic) */
static inline float heuristic(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0);
  int dy = abs(y1 - y0);
  /* min(dx,dy) * 1.41 + |dx-dy| * 1.0 */
  int mn = dx < dy ? dx : dy;
  return (float)mn * 1.41f + (float)(dx + dy - 2 * mn);
}

/* ------------------------------------------------------------------ */
/* Create / Destroy                                                    */
/* ------------------------------------------------------------------ */

BrainPathfinder *brainPathfinderCreate(void) {
  BrainPathfinder *pf = (BrainPathfinder *)calloc(1, sizeof(BrainPathfinder));
  if (!pf) return NULL;

  pf->heap = (BrainPFHeapEntry *)malloc(sizeof(BrainPFHeapEntry) * HEAP_INITIAL_CAPACITY);
  if (!pf->heap) {
    free(pf);
    return NULL;
  }
  pf->heap_capacity = HEAP_INITIAL_CAPACITY;
  pf->open_count = 0;

  /* Default config */
  pf->turn_cost = 2.0f;
  pf->wall_shoot_cost = 30.0f;
  pf->wall_shoot_shells = 5.0f;
  pf->shell_reserve = 10.0f;
  pf->road_build_cost = 12.0f;
  pf->tree_reserve = 4.0f;
  pf->mine_penalty = 40.0f;
  pf->estimate_samples = 60.0f;
  pf->danger_scale = 1.0f;

  /* The classic rules, until a think pushes this sim's own. Taken from the
     defaults table rather than from the constants, so there is one place the
     classic numbers are written down. Read again for the terrain speeds
     below. */
  SimRules classic;
  simRulesClassic(&classic);
  pf->brake_rate = classic.tank_brake_rate;
  pf->terrain_decel_rate = classic.tank_decel_rate;
  pf->min_move = classic.tank_min_move;

  /* Resource drain config */
  /* A tuned A* cost, not a copy of tank_water_ticks: ~85 ticks/tile at
   * speed 3 with a drain every 15 ticks is ~5.7, rounded up. It does not
   * follow the rule — recomputing it from one is a change to how the
   * pathfinder scores water, not a conversion. */
  pf->water_drain_rate = 6.0f;
  pf->shell_loss_cost = 3.0f;      /* A* cost per shell lost to water */
  pf->mine_loss_cost = 2.0f;       /* A* cost per mine lost to water */
  pf->armour_drain_rate = 0.02f;   /* armour lost per danger-exposure unit */
  pf->road_build_danger_max = 10.0f; /* don't assume road-build when danger >= 10 -- LGM is at risk by fire */
  pf->min_shells = 0.0f;           /* no arrival constraint by default */
  pf->min_mines = 0.0f;
  pf->min_armour = 0.0f;

  /* Default terrain costs (matching constants.lua TERRAIN_COST_LAND) */
  /* 0=building, 1=river, 2=swamp, 3=crater, 4=road, 5=forest,
     6=rubble, 7=grass, 8=halfbuild, 9=boat, 10=deepsea,
     11=refbase, 12=pillbox, 13=unknown, 14-15=unused */
  pf->terrain_cost_table[0]  = 9999.0f; /* building (wall) */
  pf->terrain_cost_table[1]  = 8.0f;    /* river (on foot = slow, drain model handles penalty) */
  pf->terrain_cost_table[2]  = 8.0f;    /* swamp */
  pf->terrain_cost_table[3]  = 8.0f;    /* crater */
  pf->terrain_cost_table[4]  = 1.0f;    /* road */
  pf->terrain_cost_table[5]  = 3.0f;    /* forest */
  pf->terrain_cost_table[6]  = 8.0f;    /* rubble */
  pf->terrain_cost_table[7]  = 2.0f;    /* grass */
  pf->terrain_cost_table[8]  = 9999.0f; /* halfbuild (wall) */
  pf->terrain_cost_table[9]  = 2.0f;    /* boat */
  pf->terrain_cost_table[10] = 9999.0f; /* deepsea */
  pf->terrain_cost_table[11] = 1.0f;    /* refbase */
  pf->terrain_cost_table[12] = 2.0f;    /* pillbox (dead = grass-like) */
  pf->terrain_cost_table[13] = 3.0f;    /* unknown */
  pf->terrain_cost_table[14] = 9999.0f;
  pf->terrain_cost_table[15] = 9999.0f;

  /* Default boat-mode terrain costs (matching constants.lua TERRAIN_COST_BOAT) */
  pf->terrain_cost_boat_table[0]  = 9999.0f; /* building (wall) */
  pf->terrain_cost_boat_table[1]  = 2.0f;    /* river (open water) */
  pf->terrain_cost_boat_table[2]  = 8.0f;    /* swamp */
  pf->terrain_cost_boat_table[3]  = 8.0f;    /* crater */
  pf->terrain_cost_boat_table[4]  = 1.0f;    /* road */
  pf->terrain_cost_boat_table[5]  = 3.0f;    /* forest */
  pf->terrain_cost_boat_table[6]  = 8.0f;    /* rubble */
  pf->terrain_cost_boat_table[7]  = 2.0f;    /* grass */
  pf->terrain_cost_boat_table[8]  = 9999.0f; /* halfbuild (wall) */
  pf->terrain_cost_boat_table[9]  = 2.0f;    /* boat (transition) */
  pf->terrain_cost_boat_table[10] = 2.0f;    /* deepsea (open water) */
  pf->terrain_cost_boat_table[11] = 1.0f;    /* refbase */
  pf->terrain_cost_boat_table[12] = 1.0f;    /* pillbox */
  pf->terrain_cost_boat_table[13] = 3.0f;    /* unknown */
  pf->terrain_cost_boat_table[14] = 9999.0f;
  pf->terrain_cost_boat_table[15] = 9999.0f;

  /* Default terrain speeds. The ten a rule names come from the classic
     table read above; the rest are the brain's own reading of a square the
     engine has no cap for. A brain that pushes its own table through
     cpf_set_terrain_speed overwrites every slot. */
  pf->terrain_speed_table[0]  = 0.0f;                            /* building */
  pf->terrain_speed_table[1]  = (float) classic.speed_river;
  pf->terrain_speed_table[2]  = (float) classic.speed_swamp;
  pf->terrain_speed_table[3]  = (float) classic.speed_crater;
  pf->terrain_speed_table[4]  = (float) classic.speed_road;
  pf->terrain_speed_table[5]  = (float) classic.speed_forest;
  pf->terrain_speed_table[6]  = (float) classic.speed_rubble;
  pf->terrain_speed_table[7]  = (float) classic.speed_grass;
  pf->terrain_speed_table[8]  = 0.0f;                            /* halfbuild */
  pf->terrain_speed_table[9]  = (float) classic.speed_boat;
  pf->terrain_speed_table[10] = (float) classic.speed_deep_sea;
  pf->terrain_speed_table[11] = (float) classic.speed_refuel_base;
  pf->terrain_speed_table[12] = 16.0f;  /* pillbox (dead = grass-like) */
  pf->terrain_speed_table[13] = 3.0f;   /* unknown */
  pf->terrain_speed_table[14] = 0.0f;
  pf->terrain_speed_table[15] = 0.0f;

  pf->status = -1;  /* no search active */
  pf->next_x = -1;
  pf->next_y = -1;

  return pf;
}

void brainPathfinderDestroy(BrainPathfinder *pf) {
  if (!pf) return;
  free(pf->heap);
  free(pf->edge_cost);
  for (int i = 0; i < DIJKSTRA_NUM_SLATES; i++) {
    DijkstraSlate *s = &pf->dij_slates[i];
    free(s->g_cost);
    free(s->closed);
    free(s->dir_at);
    free(s->shells_at);
    free(s->heap);
    free(s->heap_pos);
  }
  free(pf);
}

/* Coastal boat band radius (euclidean tiles). A water/boat tile within this
 * distance of land is marked so the land-only SHORT slate may still take
 * near-shore boat shortcuts. Open ocean (farther than this from any land)
 * stays excluded so SHORT's node budget isn't spent crossing the sea. */
#define BRAINPF_COASTAL_BAND 5

/* Rebuild coastal_boat_mask from the current map. For every water (river/
 * deep-sea) or boat-pickup tile, mark it if ANY land tile lies within
 * BRAINPF_COASTAL_BAND euclidean tiles. One-time per map (deep sea is
 * immutable and coastlines are stable, so it never needs mid-game refresh). */
static void brainPathfinderBuildCoastalMask(BrainPathfinder *pf, const BYTE *map) {
  const int R = BRAINPF_COASTAL_BAND;
  const int R2 = R * R;
  memset(pf->coastal_boat_mask, 0, sizeof(pf->coastal_boat_mask));
  if (!map) return;

  /* "land" = anything that isn't open/deep water (boat tiles count as shore).
   * First pass: bounding box of all land. On the common island map this is a
   * small region inside a big ocean, so everything outside (box + R) is more
   * than R from any land — guaranteed not coastal — and we skip it entirely.
   * (A water tile INSIDE the box can still be far from land, e.g. a big inland
   * lake, so the per-tile check below is still required inside the box.) */
  int min_x = MAP_SIZE, min_y = MAP_SIZE, max_x = -1, max_y = -1;
  for (int y = 0; y < MAP_SIZE; y++) {
    for (int x = 0; x < MAP_SIZE; x++) {
      int t = map[y * MAP_SIZE + x] & 0x0F;
      if (t != TT_RIVER && t != TT_DEEPSEA) {
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
      }
    }
  }
  if (max_x < 0) return;   /* no land at all → nothing is coastal */

  /* Clamp the land box expanded by R to the map; only water within it can be
   * coastal. */
  int lo_x = min_x - R; if (lo_x < 0) lo_x = 0;
  int hi_x = max_x + R; if (hi_x >= MAP_SIZE) hi_x = MAP_SIZE - 1;
  int lo_y = min_y - R; if (lo_y < 0) lo_y = 0;
  int hi_y = max_y + R; if (hi_y >= MAP_SIZE) hi_y = MAP_SIZE - 1;

  for (int y = lo_y; y <= hi_y; y++) {
    for (int x = lo_x; x <= hi_x; x++) {
      int idx = y * MAP_SIZE + x;
      int type = map[idx] & 0x0F;
      /* Only water / boat-pickup tiles are candidates for the band. */
      if (type != TT_RIVER && type != TT_DEEPSEA && type != TT_BOAT) continue;
      int found = 0;
      for (int dy = -R; dy <= R && !found; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= MAP_SIZE) continue;
        for (int dx = -R; dx <= R; dx++) {
          if (dx * dx + dy * dy > R2) continue;   /* euclidean disc */
          int xx = x + dx;
          if (xx < 0 || xx >= MAP_SIZE) continue;
          int t2 = map[yy * MAP_SIZE + xx] & 0x0F;
          if (t2 != TT_RIVER && t2 != TT_DEEPSEA) { found = 1; break; }
        }
      }
      if (found) pf->coastal_boat_mask[idx] = 1;
    }
  }
}

void brainPathfinderSetMap(BrainPathfinder *pf, const BYTE *map) {
  if (pf) {
    /* If the map pointer changed, the precomputed edge cache is stale.
     * Cheap pointer compare avoids spurious rebuilds when the same map
     * is set every tick. */
    if (pf->edge_cost_map != map) {
      pf->edge_cost_valid = 0;
      pf->edge_cost_map = NULL;
      pf->cache_dirty = 1;
    }
    /* Recompute the coastal boat band when the map pointer changes. */
    if (pf->coastal_mask_map != map) {
      brainPathfinderBuildCoastalMask(pf, map);
      pf->coastal_mask_map = map;
    }
    pf->map = map;
  }
}

void brainPathfinderSetAbortFlag(BrainPathfinder *pf, void *flag) {
  if (pf) pf->abort_flag = flag;
}

void brainPathfinderSetAccelPct(BrainPathfinder *pf, uint8_t pct) {
  if (pf) pf->accel_pct = pct;
}

void brainPathfinderSetMoveRules(BrainPathfinder *pf, float brakeRate,
                                 float terrainDecelRate, int32_t minMove) {
  if (pf == NULL) return;
  pf->brake_rate = brakeRate;
  pf->terrain_decel_rate = terrainDecelRate;
  pf->min_move = minMove;
}

/* One-line check used by the inner search loops. NULL flag → never
 * abort. The cast lets the header stay SDL3-free. Polled per outer
 * iteration only — never inside per-neighbour loops, where the cost
 * adds up. */
#define BRAIN_PF_ABORTED(pf) \
  ((pf)->abort_flag && SDL_GetAtomicInt((SDL_AtomicInt *)(pf)->abort_flag))

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

void brainPathfinderSetTerrainCost(BrainPathfinder *pf, int type, float cost) {
  if (pf && type >= 0 && type < 16) {
    pf->terrain_cost_table[type] = cost;
  }
}

void brainPathfinderSetBoatCost(BrainPathfinder *pf, int type, float cost) {
  if (pf && type >= 0 && type < 16) {
    pf->terrain_cost_boat_table[type] = cost;
  }
}

void brainPathfinderSetTerrainSpeed(BrainPathfinder *pf, int type, float speed) {
  if (pf && type >= 0 && type < 16) {
    pf->terrain_speed_table[type] = speed;
  }
}

void brainPathfinderSetConfig(BrainPathfinder *pf, const char *key, float value) {
  if (!pf || !key) return;
  if (strcmp(key, "turn_cost") == 0)              pf->turn_cost = value;
  else if (strcmp(key, "wall_shoot_cost") == 0)   pf->wall_shoot_cost = value;
  else if (strcmp(key, "wall_shoot_shells") == 0) pf->wall_shoot_shells = value;
  else if (strcmp(key, "shell_reserve") == 0)     pf->shell_reserve = value;
  else if (strcmp(key, "road_build_cost") == 0)   pf->road_build_cost = value;
  else if (strcmp(key, "tree_reserve") == 0)      pf->tree_reserve = value;
  else if (strcmp(key, "mine_penalty") == 0)      pf->mine_penalty = value;
  else if (strcmp(key, "estimate_samples") == 0)  pf->estimate_samples = value;
  else if (strcmp(key, "danger_scale") == 0)      pf->danger_scale = value;
  else if (strcmp(key, "water_drain_rate") == 0)  pf->water_drain_rate = value;
  else if (strcmp(key, "shell_loss_cost") == 0)   pf->shell_loss_cost = value;
  else if (strcmp(key, "mine_loss_cost") == 0)    pf->mine_loss_cost = value;
  else if (strcmp(key, "armour_drain_rate") == 0) pf->armour_drain_rate = value;
  else if (strcmp(key, "min_shells") == 0)        pf->min_shells = value;
  else if (strcmp(key, "min_mines") == 0)         pf->min_mines = value;
  else if (strcmp(key, "min_armour") == 0)        pf->min_armour = value;
  else if (strcmp(key, "road_build_danger_max") == 0) pf->road_build_danger_max = value;
}

/* ------------------------------------------------------------------ */
/* Danger overlay                                                      */
/* ------------------------------------------------------------------ */

void brainPathfinderClearDanger(BrainPathfinder *pf) {
  if (pf) {
    memset(pf->danger_grid, 0, sizeof(pf->danger_grid));
    pf->cache_dirty = 1;
  }
}

void brainPathfinderStampPill(BrainPathfinder *pf, int cx, int cy,
                               int radius, float base_danger, float anger) {
  int dx, dy, nx, ny;
  float proximity, penalty;
  int idx;
  int r2 = radius * radius;

  if (!pf) return;
  pf->cache_dirty = 1;

  for (dy = -radius; dy <= radius; dy++) {
    ny = cy + dy;
    if (ny < 0 || ny > 255) continue;
    for (dx = -radius; dx <= radius; dx++) {
      nx = cx + dx;
      if (nx < 0 || nx > 255) continue;
      int d2 = dx * dx + dy * dy;
      if (d2 > r2) continue;
      float dist = sqrtf((float)d2);

      proximity = 1.0f - dist / (float)(radius + 1);
      penalty = (base_danger + anger) * proximity;

      /* Terrain exposure: slow terrain = more time under fire.
         Scale by (max_speed / terrain_speed). Road=1x, swamp=5.3x.
         Tree cover: dense forest hides the tank and absorbs shots.
         Also +50% if any cardinal neighbor is water/rubble/swamp. */
      if (pf->map && penalty > 0.0f) {
        idx = ny * MAP_SIZE + nx;
        int tt = pf->map[idx] & 0x0F;
        float spd = pf->terrain_speed_table[tt];
        if (spd > 0.0f) {
          penalty *= 16.0f / spd;
        }
        /* Tree cover: forest absorbs shots and provides concealment.
         * Dense forest (all 4 neighbors also forest) gets stronger reduction.
         * Edge forest (at least one non-forest neighbor) still helps. */
        if (tt == TT_FOREST) {
          int forest_neighbors = 0;
          if (ny > 0   && (pf->map[(ny-1)*MAP_SIZE+nx] & 0x0F) == TT_FOREST) forest_neighbors++;
          if (ny < 255 && (pf->map[(ny+1)*MAP_SIZE+nx] & 0x0F) == TT_FOREST) forest_neighbors++;
          if (nx > 0   && (pf->map[ny*MAP_SIZE+(nx-1)] & 0x0F) == TT_FOREST) forest_neighbors++;
          if (nx < 255 && (pf->map[ny*MAP_SIZE+(nx+1)] & 0x0F) == TT_FOREST) forest_neighbors++;
          float dist_f = sqrtf((float)(dx*dx + dy*dy));
          if (forest_neighbors == 4) {
            /* Dense forest: strong cover */
            penalty *= (dist_f >= 3.0f) ? 0.3f : 0.7f;
          } else {
            /* Edge forest: partial cover, still absorbs some shots */
            penalty *= (dist_f >= 3.0f) ? 0.5f : 0.8f;
          }
        }
        /* Wall shielding: walls between pill and this tile absorb shots.
         * Only check close range (dist <= 5) where shielding matters most
         * and penalty is large. At long range proximity falloff already
         * makes the penalty small. Each T_BUILDING = 5 HP, T_HALFBUILD = 2 HP.
         * Shielding = min(1.0, wall_hp * fire_rate / 200). */
        {
          float dist_f = sqrtf((float)(dx*dx + dy*dy));
          if (dist_f <= 5.0f && dist_f > 0.5f) {
            int wall_hp = 0;
            /* Bresenham from pill (cx,cy) to target (nx,ny) counting walls */
            int bx0 = cx, by0 = cy, bx1 = nx, by1 = ny;
            int bdx = abs(bx1 - bx0), bdy = abs(by1 - by0);
            int bsx = bx0 < bx1 ? 1 : -1, bsy = by0 < by1 ? 1 : -1;
            int berr = bdx - bdy;
            int bbx = bx0, bby = by0;
            int bsteps = 0;
            while (bsteps++ < 20) {
              if (bbx == bx1 && bby == by1) break;
              int e2 = 2 * berr;
              if (e2 > -bdy) { berr -= bdy; bbx += bsx; }
              if (e2 <  bdx) { berr += bdx; bby += bsy; }
              if (bbx == bx1 && bby == by1) break;  /* don't count dest tile */
              if (bbx >= 0 && bbx <= 255 && bby >= 0 && bby <= 255) {
                int btt = pf->map[bby * MAP_SIZE + bbx] & 0x0F;
                if (btt == TT_BUILDING)  wall_hp += 5;
                if (btt == TT_HALFBUILD) wall_hp += 2;
              }
            }
            if (wall_hp > 0) {
              /* fire_rate depends on anger: angry pill fires every tick,
               * calm pill fires every ~32 ticks */
              float fire_rate_f = 1.0f + (1.0f - anger / (base_danger + anger + 0.001f)) * 31.0f;
              float wall_time = (float)wall_hp * fire_rate_f;
              float shielding = wall_time / 200.0f;
              if (shielding > 1.0f) shielding = 1.0f;
              penalty *= (1.0f - shielding);
            }
          }
        }
        /* Adjacent hazard: bordering bad terrain makes escape harder */
        int adj_hazard = 0;
        if (ny > 0) {
          int at = pf->map[(ny-1)*MAP_SIZE+nx] & 0x0F;
          if (at == TT_RIVER || at == TT_DEEPSEA || at == TT_RUBBLE || at == TT_SWAMP)
            adj_hazard = 1;
        }
        if (!adj_hazard && ny < 255) {
          int at = pf->map[(ny+1)*MAP_SIZE+nx] & 0x0F;
          if (at == TT_RIVER || at == TT_DEEPSEA || at == TT_RUBBLE || at == TT_SWAMP)
            adj_hazard = 1;
        }
        if (!adj_hazard && nx > 0) {
          int at = pf->map[ny*MAP_SIZE+(nx-1)] & 0x0F;
          if (at == TT_RIVER || at == TT_DEEPSEA || at == TT_RUBBLE || at == TT_SWAMP)
            adj_hazard = 1;
        }
        if (!adj_hazard && nx < 255) {
          int at = pf->map[ny*MAP_SIZE+(nx+1)] & 0x0F;
          if (at == TT_RIVER || at == TT_DEEPSEA || at == TT_RUBBLE || at == TT_SWAMP)
            adj_hazard = 1;
        }
        if (adj_hazard) {
          penalty *= 1.5f;
        }
      }

      if (penalty > 0.0f) {
        idx = ny * MAP_SIZE + nx;
        /* Accumulate — multiple pills stack. Clamp to uint16_t max. */
        int val = (int)pf->danger_grid[idx] + (int)(penalty + 0.5f);
        if (val > 65535) val = 65535;
        pf->danger_grid[idx] = (uint16_t)val;
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* Influence grid                                                      */
/* ------------------------------------------------------------------ */

void brainPathfinderClearInfluence(BrainPathfinder *pf) {
  if (pf) memset(pf->influence_grid, 0, sizeof(pf->influence_grid));
}

void brainPathfinderStampInfluence(BrainPathfinder *pf, int cx, int cy,
                                    int radius, int strength) {
  int dx, dy, nx, ny;
  float proximity;
  int delta, idx;
  int r2 = radius * radius;

  if (!pf) return;

  for (dy = -radius; dy <= radius; dy++) {
    ny = cy + dy;
    if (ny < 0 || ny > 255) continue;
    for (dx = -radius; dx <= radius; dx++) {
      nx = cx + dx;
      if (nx < 0 || nx > 255) continue;
      int d2 = dx * dx + dy * dy;
      if (d2 > r2) continue;
      float dist = sqrtf((float)d2);

      proximity = 1.0f - dist / (float)(radius + 1);
      delta = (int)(strength * proximity);

      if (delta != 0) {
        idx = ny * MAP_SIZE + nx;
        /* Accumulate — multiple stamps stack. Clamp to int16_t range. */
        int val = (int)pf->influence_grid[idx] + delta;
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        pf->influence_grid[idx] = (int16_t)val;
      }
    }
  }
}

int16_t brainPathfinderInfluenceAt(BrainPathfinder *pf, int x, int y) {
  if (pf && x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE) {
    return pf->influence_grid[y * MAP_SIZE + x];
  }
  return 0;
}

void brainPathfinderSetDanger(BrainPathfinder *pf, int x, int y, float value) {
  if (pf && x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE) {
    int v = (int)(value + 0.5f);
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    pf->danger_grid[y * MAP_SIZE + x] = (uint16_t)v;
    pf->cache_dirty = 1;
  }
}

/* ------------------------------------------------------------------ */
/* Custom overlay                                                      */
/* ------------------------------------------------------------------ */

void brainPathfinderSetDangerOffset(BrainPathfinder *pf, int x, int y, int16_t value) {
  if (pf && x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE)
    pf->danger_offset_grid[y * MAP_SIZE + x] = value;
}

void brainPathfinderClearDangerOffset(BrainPathfinder *pf) {
  if (pf) memset(pf->danger_offset_grid, 0, sizeof(pf->danger_offset_grid));
}

void brainPathfinderSetOverlay(BrainPathfinder *pf, int x, int y, float value) {
  if (pf && x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE) {
    int v = (int)value;
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    pf->overlay_grid[y * MAP_SIZE + x] = (int16_t)v;
    pf->cache_dirty = 1;
  }
}

void brainPathfinderClearOverlay(BrainPathfinder *pf) {
  if (pf) {
    memset(pf->overlay_grid, 0, sizeof(pf->overlay_grid));
    pf->cache_dirty = 1;
  }
}

float brainPathfinderGetOverlay(const BrainPathfinder *pf, int x, int y) {
  if (!pf || x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return 0.0f;
  return (float)pf->overlay_grid[y * MAP_SIZE + x];
}

float brainPathfinderGetDanger(const BrainPathfinder *pf, int x, int y) {
  if (!pf || x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return 0.0f;
  return (float)pf->danger_grid[y * MAP_SIZE + x];
}

/* ------------------------------------------------------------------ */
/* Cost computation (inner loop)                                       */
/* ------------------------------------------------------------------ */

/* Compute the cost to enter tile (nx, ny) given the current boat state.
 * Returns the movement cost, fills resource usage outputs and new_boat.
 * Returns COST_INF for impassable tiles or resource-constrained tiles. */
static float compute_cost(BrainPathfinder *pf, int nx, int ny,
                           int cur_boat,
                           int shells_left, int trees_left,
                           int mines_left, int armour_left,
                           int *shells_used, int *trees_used,
                           int *mines_used, int *armour_used,
                           int *new_boat) {
  int midx = ny * MAP_SIZE + nx;
  uint8_t raw = pf->map[midx];
  uint8_t type = raw & 0x0F;
  int onBoat = next_boat_state(cur_boat, type);
  float base, speed, danger, overlay, cost;

  int wall_shoot_shells;
  int water_drain;

  *shells_used = 0;
  *trees_used = 0;
  *mines_used = 0;
  *armour_used = 0;
  *new_boat = onBoat;

  /* Select cost table based on the state we'll be in on this tile */
  base = onBoat ? pf->terrain_cost_boat_table[type]
                : pf->terrain_cost_table[type];

  /* Walls: shootable only on foot */
  if (base >= WALL_THRESHOLD) {
    if (!onBoat && (type == TT_BUILDING || type == TT_HALFBUILD)) {
      wall_shoot_shells = (int)pf->wall_shoot_shells;
      if (shells_left >= wall_shoot_shells) {
        /* Add danger at the wall tile — you take pill fire while shooting it.
         * Use speed=3 (stopped/slow while shooting). */
        float wall_danger = (float)pf->danger_grid[midx] * pf->danger_scale
                          * (16.0f / 3.0f);
        *shells_used = wall_shoot_shells;
        return pf->wall_shoot_cost + wall_danger;
      }
    }
    return COST_INF;
  }

  /* Mine penalty */
  if (raw & 0x80) { /* TERRAIN_MINE bit */
    base += pf->mine_penalty;
  }

  /* Danger scaling by terrain speed.
   * In a boat on water, we move at full speed (like road), so danger
   * exposure time is much lower than the base terrain speed suggests. */
  danger = (float)pf->danger_grid[midx] + (float)pf->danger_offset_grid[midx];
  if (danger < 0.0f) danger = 0.0f;
  speed = pf->terrain_speed_table[type];
  if (onBoat && is_water_tile(type)) {
    speed = 16.0f; /* boat speed matches road speed */
  }
  overlay = (float)pf->overlay_grid[midx];

  cost = base + danger * pf->danger_scale * (16.0f / fmaxf(speed, 0.1f)) + overlay;

  /* Water resource drain: on foot in river, tank loses shells and mines.
   * Game mechanic: every tank_water_ticks (15 classic) ticks at speed <= 3,
   * lose 1 shell + 1 mine.  At speed 3, ~85 ticks per tile = ~6 drains.
   * In a boat there is no drain. */
  if (!onBoat && type == TT_RIVER && pf->water_drain_rate > 0.0f) {
    int shell_drain, mine_drain;
    water_drain = (int)pf->water_drain_rate;
    /* Clamp drain to what the tank actually carries — can't lose resources
     * you don't have.  Without this, mi=0 makes river tiles COST_INF. */
    shell_drain = water_drain < shells_left ? water_drain : shells_left;
    mine_drain  = water_drain < mines_left  ? water_drain : mines_left;
    *shells_used += shell_drain;
    *mines_used = mine_drain;
    /* Add the resource loss as A* cost so the search prefers drier paths */
    cost += (float)shell_drain * pf->shell_loss_cost;
    cost += (float)mine_drain  * pf->mine_loss_cost;
  }

  /* Armour drain estimate from pill danger.
   * danger_exposure = danger * (16/speed) is the cost component from danger.
   * We convert a fraction of that to estimated armour hits. */
  if (danger > 0.0f && pf->armour_drain_rate > 0.0f) {
    float danger_exposure = danger * (16.0f / fmaxf(speed, 0.1f));
    int armour_drain = (int)(danger_exposure * pf->armour_drain_rate + 0.5f);
    if (armour_drain > 0) {
      *armour_used = armour_drain;
    }
  }

  /* Check minimum resource constraints */
  {
    int new_shells = shells_left - *shells_used;
    int new_mines = mines_left - *mines_used;
    int new_armour = armour_left - *armour_used;
    if (new_shells < (int)pf->min_shells) return COST_INF;
    if (new_mines < (int)pf->min_mines) return COST_INF;
    if (new_armour < (int)pf->min_armour) return COST_INF;
  }

  /* Road-building: only when on foot (can't build from a boat).
   * Skip when danger is high — LGM is at risk by fire. */
  if (!onBoat && trees_left >= 2 && danger < pf->road_build_danger_max) {
    /* Types 2=swamp, 3=crater, 6=rubble, 1=river are worth paving */
    if (type == TT_SWAMP || type == TT_CRATER || type == TT_RUBBLE || type == TT_RIVER) {
      if (cost > pf->road_build_cost) {
        *trees_used = 2;
        /* Road-building bypasses water drain: LGM builds a road, no wading */
        *shells_used -= (type == TT_RIVER) ? (int)pf->water_drain_rate : 0;
        if (*shells_used < 0) *shells_used = 0;
        *mines_used = 0;
        *armour_used = 0;
        return pf->road_build_cost;
      }
    }
  }

  return cost;
}

/* ------------------------------------------------------------------ */
/* A* search                                                           */
/* ------------------------------------------------------------------ */

int brainPathfinderPathTo(BrainPathfinder *pf,
                           int sx, int sy, int dx, int dy,
                           int in_boat, int shells, int trees,
                           int mines, int armour,
                           int budget,
                           int *next_x, int *next_y) {
  int src_ni, dest_tile;
  int expanded;
  int shell_budget, tree_budget, mine_budget, armour_budget;

  if (!pf || !pf->map) {
    if (next_x) *next_x = -1;
    if (next_y) *next_y = -1;
    return -1;
  }

  FILE *alog = (FILE *)pf->astarLog;
  uint32_t alogTick = pf->astarLogTickPtr ? *pf->astarLogTickPtr / 2 : 0;

  /* Clamp coordinates */
  if (sx < 0) sx = 0;
  if (sx > 255) sx = 255;
  if (sy < 0) sy = 0;
  if (sy > 255) sy = 255;
  if (dx < 0) dx = 0;
  if (dx > 255) dx = 255;
  if (dy < 0) dy = 0;
  if (dy > 255) dy = 255;

  dest_tile = dy * MAP_SIZE + dx;

  /* Short-circuit: completed prior search with identical inputs and no
   * invalidating mutation since — return the cached next step. Must come
   * before the resume check (resume is for partial searches, status == 0). */
  if (pf->status == 1 && pf->dest_x == dx && pf->dest_y == dy
      && pf->src_x == sx && pf->src_y == sy
      && pf->in_boat == in_boat && !pf->cache_dirty
      && pf->next_x >= 0) {
    if (next_x) *next_x = pf->next_x;
    if (next_y) *next_y = pf->next_y;
    return 1;
  }

  /* Check if we can resume an existing search */
  if (pf->status == 0 && pf->dest_x == dx && pf->dest_y == dy
      && pf->src_x == sx && pf->src_y == sy
      && pf->in_boat == in_boat) {
    /* Resume existing search */
    if (alog) {
      fprintf(alog, "\n=== A* RESUME tick=%u src=(%d,%d) dest=(%d,%d) boat=%d budget=%d ===\n",
                      alogTick, sx, sy, dx, dy, in_boat, budget);
      fflush(alog);
    }
    goto do_search;
  }

  /* Start fresh search: bump current_epoch so every node reads as
   * stale via the accessors. Per-node arrays are left untouched and
   * repopulated lazily as stamp_node() tags nodes during relaxation.
   * Resume (above) must skip this bump so it still sees the live
   * arrays from the search it is resuming. */
  epoch_bump(pf);

  pf->status = 0;
  pf->cache_dirty = 0;
  pf->dest_x = dx;
  pf->dest_y = dy;
  pf->src_x = sx;
  pf->src_y = sy;
  pf->next_x = -1;
  pf->next_y = -1;
  pf->age = 0;
  pf->in_boat = in_boat;

  src_ni = node_idx(sx, sy, in_boat);
  stamp_node(pf, src_ni);
  pf->g_cost[src_ni] = 0.0f;
  pf->parent[src_ni] = PARENT_NONE;

  shell_budget = shells - (int)pf->shell_reserve;
  if (shell_budget < 0) shell_budget = 0;
  tree_budget = trees - (int)pf->tree_reserve;
  if (tree_budget < 0) tree_budget = 0;
  mine_budget = mines;   /* no reserve for mines */
  armour_budget = armour; /* no reserve for armour */

  pf->shells_at[src_ni] = (int16_t)shell_budget;
  pf->trees_at[src_ni] = (int16_t)tree_budget;
  pf->mines_at[src_ni] = (int16_t)mine_budget;
  pf->armour_at[src_ni] = (int16_t)armour_budget;
  pf->dir_at[src_ni] = 0xFF; /* source: no direction */

  heap_clear(pf);
  heap_push(pf, heuristic(sx, sy, dx, dy), (uint16_t)sx, (uint16_t)sy, (uint8_t)in_boat);

  if (alog) {
    fprintf(alog, "\n=== A* NEW SEARCH tick=%u src=(%d,%d) dest=(%d,%d) boat=%d budget=%d ===\n",
            alogTick, sx, sy, dx, dy, in_boat, budget);
    fprintf(alog, "resources: shells=%d trees=%d mines=%d armour=%d\n", shells, trees, mines, armour);
    fprintf(alog, "reserves: shell=%d tree=%d mine_penalty=%.1f\n",
            (int)pf->shell_reserve, (int)pf->tree_reserve, pf->mine_penalty);
    /* Dump terrain in a 50-tile radius around midpoint */
    int midX = (sx + dx) / 2, midY = (sy + dy) / 2;
    int mapR = 25;
    fprintf(alog, "--- MAP terrain (brainMap values) center=(%d,%d) r=%d ---\n", midX, midY, mapR);
    for (int my = midY - mapR; my <= midY + mapR; my++) {
      if (my < 0 || my > 255) continue;
      fprintf(alog, "y=%3d: ", my);
      for (int mx = midX - mapR; mx <= midX + mapR; mx++) {
        if (mx < 0 || mx > 255) { fprintf(alog, ".. "); continue; }
        uint8_t raw = pf->map[my * MAP_SIZE + mx];
        fprintf(alog, "%02x ", raw);
      }
      fprintf(alog, "\n");
    }
    /* Dump non-zero danger in the area */
    fprintf(alog, "--- DANGER (non-zero) ---\n");
    for (int my = midY - mapR; my <= midY + mapR; my++) {
      if (my < 0 || my > 255) continue;
      for (int mx = midX - mapR; mx <= midX + mapR; mx++) {
        if (mx < 0 || mx > 255) continue;
        uint16_t d = pf->danger_grid[my * MAP_SIZE + mx];
        if (d > 0) fprintf(alog, "  danger(%d,%d)=%d\n", mx, my, d);
      }
    }
    /* Dump non-zero influence in the area */
    fprintf(alog, "--- INFLUENCE (non-zero) ---\n");
    for (int my = midY - mapR; my <= midY + mapR; my++) {
      if (my < 0 || my > 255) continue;
      for (int mx = midX - mapR; mx <= midX + mapR; mx++) {
        if (mx < 0 || mx > 255) continue;
        int16_t inf = pf->influence_grid[my * MAP_SIZE + mx];
        if (inf != 0) fprintf(alog, "  influence(%d,%d)=%d\n", mx, my, inf);
      }
    }
    fprintf(alog, "--- BEGIN SEARCH ---\n");
    fflush(alog);
  }

do_search:
  expanded = 0;

  while (pf->open_count > 0 && expanded < budget) {
    BrainPFHeapEntry entry = heap_pop(pf);
    int cx = entry.x;
    int cy = entry.y;
    int cur_boat = entry.boat;
    int ci = node_idx(cx, cy, cur_boat);
    int cur_dir, cur_shells, cur_trees, cur_mines, cur_armour;
    int d;

    /* Cooperative abort: poll once per node-expand. The brain's count
     * hook fires within ~1000 instructions of returning to Lua, so a
     * partial path here is acceptable — same shape as a budget-
     * exhausted search (status = -1 below if we don't hit dest). */
    if (BRAIN_PF_ABORTED(pf)) break;

    if (get_closed(pf, ci)) continue;
    closed_set(pf->closed, ci);
    expanded++;

    if (alog) {
      fprintf(alog, "EXPAND #%d (%d,%d) boat=%d g=%.2f sh=%d tr=%d mi=%d arm=%d\n",
              expanded, cx, cy, cur_boat, pf->g_cost[ci],
              pf->shells_at[ci], pf->trees_at[ci],
              pf->mines_at[ci], pf->armour_at[ci]);
    }

    /* Destination reached (in either boat state) */
    if (map_idx(ci) == dest_tile) {
      /* Trace parent chain to extract first step */
      int cur = ci;
      int prev = -1;
      int steps = 0;
      while (cur >= 0 && cur < NODE_COUNT && steps++ < NODE_COUNT) {
        uint32_t p = get_parent(pf, cur);
        if (p == PARENT_NONE) {
          /* Reached source */
          if (prev >= 0) {
            pf->next_x = node_x(prev);
            pf->next_y = node_y(prev);
          } else {
            /* Already at destination */
            pf->next_x = -1;
            pf->next_y = -1;
          }
          break;
        }
        prev = cur;
        cur = (int)p;
      }
      pf->status = 1; /* done */
      pf->age = 0;
      if (alog) {
        fprintf(alog, "=== A* DONE expanded=%d cost=%.2f next=(%d,%d) ===\n",
                expanded, pf->g_cost[ci], pf->next_x, pf->next_y);
        fflush(alog);
      }
      if (next_x) *next_x = pf->next_x;
      if (next_y) *next_y = pf->next_y;
      return 1;
    }

    cur_dir = pf->dir_at[ci];
    cur_shells = pf->shells_at[ci];
    cur_trees = pf->trees_at[ci];
    cur_mines = pf->mines_at[ci];
    cur_armour = pf->armour_at[ci];

    for (d = 0; d < 8; d++) {
      int nx = cx + DX8[d];
      int ny = cy + DY8[d];
      int ni, shells_used, trees_used, mines_used, armour_used, onBoat;
      float tc, new_g, f;

      if (nx < 0 || nx > 255 || ny < 0 || ny > 255) continue;

      /* Block diagonal moves through impassable corners.
       * On foot a full-tile tank cannot squeeze diagonally past ANY solid
       * corner tile, so block the diagonal if EITHER cardinal corner is
       * impassable (speed 0 = building/halfbuild). The old rule required
       * BOTH corners solid, which let the tank cut the corner between a
       * wall and a passable-but-overlaid tile (e.g. a live pill, which is
       * grass-like terrain + a danger overlay, not solid in the grid).
       * Boats keep the both-zero rule plus the land-clip checks below. */
      if (DX8[d] != 0 && DY8[d] != 0) {
        int adj_x_type = pf->map[(cy * MAP_SIZE) + (cx + DX8[d])] & 0x0F;
        int adj_y_type = pf->map[((cy + DY8[d]) * MAP_SIZE) + cx] & 0x0F;
        float speed_x = pf->terrain_speed_table[adj_x_type];
        float speed_y = pf->terrain_speed_table[adj_y_type];
        if (!cur_boat) {
          if (speed_x == 0.0f || speed_y == 0.0f) continue;
        } else {
          if (speed_x == 0.0f && speed_y == 0.0f) continue;
        }
        /* On foot, block diagonals that clip through deep sea */
        if (!cur_boat && (adj_x_type == TT_DEEPSEA || adj_y_type == TT_DEEPSEA)) continue;
        /* In boat, block diagonals that clip through land */
        if (cur_boat && (!is_water_tile(adj_x_type) && adj_x_type != TT_BOAT)) continue;
        if (cur_boat && (!is_water_tile(adj_y_type) && adj_y_type != TT_BOAT)) continue;
      }

      tc = compute_cost(pf, nx, ny, cur_boat,
                         cur_shells, cur_trees, cur_mines, cur_armour,
                         &shells_used, &trees_used, &mines_used, &armour_used,
                         &onBoat);
      if (tc >= COST_INF) continue;

      /* Escalating wall-break penalty:
       *   walls 1-2: normal cost
       *   wall 3+: cost doubles for each additional wall
       * Makes thick walls exponentially more expensive. */
      if (shells_used > 0) {
        int initial_shells = pf->shells_at[node_idx(pf->src_x, pf->src_y, pf->in_boat)];
        int walls_so_far = (initial_shells > 0 && pf->wall_shoot_shells > 0)
                         ? (initial_shells - cur_shells) / (int)pf->wall_shoot_shells : 0;
        if (walls_so_far >= 2) {
          int extra = walls_so_far - 2;
          tc *= (float)(1 << (extra + 1)); /* 2x, 4x, 8x, 16x... */
        }
      }

      ni = node_idx(nx, ny, onBoat);
      if (get_closed(pf, ni)) continue;

      /* Turn penalty */
      if (cur_dir != 0xFF && cur_dir != d) {
        int tsteps = dir_steps(cur_dir, d);
        if (tsteps > 0) {
          tc += (float)tsteps * pf->turn_cost;
        }
      }

      new_g = pf->g_cost[ci] + tc * DMUL8[d];
      if (new_g >= get_g_cost(pf, ni)) continue;

      if (alog) {
        uint8_t nraw = pf->map[ny * MAP_SIZE + nx];
        fprintf(alog, "  -> (%d,%d) t=%d boat=%d tc=%.2f g=%.2f d=%d dgr=%d\n",
                nx, ny, nraw & 0x0F, onBoat, tc, new_g, d,
                pf->danger_grid[ny * MAP_SIZE + nx]);
      }

      stamp_node(pf, ni);
      pf->g_cost[ni] = new_g;
      pf->parent[ni] = (uint32_t)ci;
      pf->shells_at[ni] = (int16_t)(cur_shells - shells_used);
      pf->trees_at[ni] = (int16_t)(cur_trees - trees_used);
      pf->mines_at[ni] = (int16_t)(cur_mines - mines_used);
      pf->armour_at[ni] = (int16_t)(cur_armour - armour_used);
      pf->dir_at[ni] = (uint8_t)d;

      f = new_g + heuristic(nx, ny, dx, dy);
      heap_push(pf, f, (uint16_t)nx, (uint16_t)ny, (uint8_t)onBoat);
    }
  }

  /* Budget exhausted or heap empty */
  if (pf->open_count <= 0) {
    if (alog) {
      fprintf(alog, "=== A* FAILED (heap empty) expanded=%d ===\n", expanded);
      fflush(alog);
    }
    /* No path found — find closest explored node as partial result */
    int best_ni = -1;
    int best_h = 0x7FFFFFFF;
    int i;
    for (i = 0; i < NODE_COUNT; i++) {
      if (get_closed(pf, i)) {
        int ix = node_x(i);
        int iy = node_y(i);
        int h = abs(ix - dx) + abs(iy - dy);
        if (h < best_h) {
          best_h = h;
          best_ni = i;
        }
      }
    }
    if (best_ni >= 0 && map_idx(best_ni) != (sy * MAP_SIZE + sx)) {
      /* Trace parent chain from best node */
      int cur = best_ni;
      int prev = -1;
      int steps = 0;
      while (cur >= 0 && cur < NODE_COUNT && steps++ < NODE_COUNT) {
        uint32_t p = get_parent(pf, cur);
        if (p == PARENT_NONE) {
          if (prev >= 0) {
            pf->next_x = node_x(prev);
            pf->next_y = node_y(prev);
          }
          break;
        }
        prev = cur;
        cur = (int)p;
      }
    }
    pf->status = -1; /* failed */
    if (next_x) *next_x = pf->next_x;
    if (next_y) *next_y = pf->next_y;
    return -1;
  }

  /* Budget exhausted but search still running */
  pf->age++;
  if (alog) {
    fprintf(alog, "=== A* BUDGET EXHAUSTED expanded=%d open=%d age=%d next=(%d,%d) ===\n",
            expanded, pf->open_count, pf->age, pf->next_x, pf->next_y);
    fflush(alog);
  }
  if (next_x) *next_x = pf->next_x;
  if (next_y) *next_y = pf->next_y;
  return 0;
}

/* ------------------------------------------------------------------ */
/* One-shot A* cost query                                              */
/* ------------------------------------------------------------------ */

/* Run a complete bounded A* and return the true path cost to (dx,dy).
 * Clobbers the main search arrays — caller must not expect path_to()
 * state to survive.  Returns COST_INF on failure or budget exhaustion.
 * Simplified: no resource tracking, no parent chain, no path extraction.
 * This keeps the inner loop fast for bulk candidate evaluation. */
static FILE *astar_log = NULL;
static int astar_log_enabled = 0;
static int astar_log_tick = 0;  /* current tick prefix for ASTAR log lines */
static char astar_log_caller[2048] = "";  /* Lua call context for next cost_to */

/* Separate log for the incremental Dijkstra. NOT enabled together with
 * the cost_to log because the per-tick STEP entries are high-frequency
 * and add measurable overhead. Toggle independently via
 * brainPathfinderEnableDijkstraLog(). */
static FILE *dijkstra_log = NULL;

void brainPathfinderSetLogTick(int tick) {
  astar_log_tick = tick;
}

void brainPathfinderSetLogCaller(const char *caller) {
  if (caller) {
    size_t n = strlen(caller);
    if (n >= sizeof(astar_log_caller)) n = sizeof(astar_log_caller) - 1;
    memcpy(astar_log_caller, caller, n);
    astar_log_caller[n] = '\0';
  } else {
    astar_log_caller[0] = '\0';
  }
}

void brainPathfinderEnableLogPath(const char *path) {
  if (astar_log) {
    fflush(astar_log);
    fclose(astar_log);
    astar_log = NULL;
  }
  if (path) {
    astar_log = fopen(path, "w");
    if (astar_log) {
      /* Line-buffer so writes appear without needing fclose. The
       * default fully-buffered mode hides recent activity from anyone
       * tailing the file while the brain is still running. */
      setvbuf(astar_log, NULL, _IONBF, 0);  /* unbuffered — writes appear immediately */
      fprintf(astar_log, "=== cost_to debug log ===\n");
      fflush(astar_log);
      astar_log_enabled = 1;
    } else {
      astar_log_enabled = 0;
    }
  } else {
    astar_log_enabled = 0;
  }
}

int brainPathfinderIsLogEnabled(void) {
  return astar_log_enabled;
}

void brainPathfinderEnableLog(int enable) {
  astar_log_enabled = enable;
  if (enable && !astar_log) {
    astar_log = fopen("astar_costto.log", "w");
    if (astar_log) fprintf(astar_log, "=== cost_to debug log ===\n");
  }
  if (!enable && astar_log) {
    fflush(astar_log);
    fclose(astar_log);
    astar_log = NULL;
  }
}

void brainPathfinderEnableDijkstraLog(int enable) {
  if (enable && !dijkstra_log) {
    dijkstra_log = fopen("dijkstra.log", "w");
    if (dijkstra_log) fprintf(dijkstra_log, "=== dijkstra debug log ===\n");
  }
  if (!enable && dijkstra_log) {
    fflush(dijkstra_log);
    fclose(dijkstra_log);
    dijkstra_log = NULL;
  }
}

float brainPathfinderCostTo(BrainPathfinder *pf,
                             int sx, int sy, int dx, int dy,
                             int in_boat, int shells, int trees,
                             int mines, int armour, int budget) {
  return brainPathfinderCostToEx(pf, sx, sy, dx, dy, in_boat,
                                  shells, trees, mines, armour,
                                  budget, 1 /*allow_boat*/);
}

float brainPathfinderCostToEx(BrainPathfinder *pf,
                               int sx, int sy, int dx, int dy,
                               int in_boat, int shells, int trees,
                               int mines, int armour, int budget,
                               int allow_boat) {
  int src_ni, dest_tile, expanded;
  float result;
  FILE *alog = astar_log;

  /* â”€â”€ Performance counters (always tallied; only printed if alog) â”€â”€ */
  int n_pops = 0;          /* heap_pop calls */
  int n_stale_pops = 0;    /* popped a node that was already closed */
  int n_pushes = 0;        /* heap_push calls */
  int n_neigh_total = 0;   /* total neighbor edges considered */
  int n_neigh_oob = 0;     /* rejected: out of bounds */
  int n_neigh_diag = 0;    /* rejected: blocked diagonal corner */
  int n_neigh_inf = 0;     /* rejected: compute_cost returned COST_INF */
  int n_neigh_closed = 0;  /* rejected: target node already closed */
  int n_neigh_worse = 0;   /* rejected: new_g >= existing g_cost */
  int n_neigh_relaxed = 0; /* accepted: pushed to heap */
  int peak_open = 0;       /* peak open_count during the search */
  double t_init_us, t_search_us;
  double t0 = 0.0, t1 = 0.0;

  if (!pf || !pf->map) {
    if (alog) fprintf(alog, "cost_to(%d,%d)->(%d,%d): ABORT pf=%p map=%p\n",
                      sx,sy,dx,dy,(void*)pf,pf?(void*)pf->map:NULL);
    return COST_INF;
  }

  if (alog) {
    int src_type = pf->map[sy * MAP_SIZE + sx] & 0x0F;
    int dst_type = pf->map[dy * MAP_SIZE + dx] & 0x0F;
    fprintf(alog, "=== cost_to(%d,%d)->(%d,%d) boat=%d sh=%d tr=%d mn=%d arm=%d budget=%d src_terrain=%d dst_terrain=%d ===\n",
            sx,sy,dx,dy,in_boat,shells,trees,mines,armour,budget,src_type,dst_type);
    if (astar_log_caller[0]) {
      fprintf(alog, "CALLER:\n%s\n", astar_log_caller);
      astar_log_caller[0] = '\0';  /* consume — caller sets per-call */
    }
  }

  /* Clamp */
  if (sx < 0) sx = 0; if (sx > 255) sx = 255;
  if (sy < 0) sy = 0; if (sy > 255) sy = 255;
  if (dx < 0) dx = 0; if (dx > 255) dx = 255;
  if (dy < 0) dy = 0; if (dy > 255) dy = 255;

  if (sx == dx && sy == dy) {
    if (alog) fprintf(alog, "  same tile -> 0\n");
    return 0.0f;
  }

  dest_tile = dy * MAP_SIZE + dx;

  t0 = bp_now_us();

  /* Bump the epoch instead of memset'ing the per-node arrays; only
   * the source needs to be stamped up front. */
  epoch_bump(pf);

  src_ni = node_idx(sx, sy, in_boat);
  stamp_node(pf, src_ni);
  pf->g_cost[src_ni] = 0.0f;
  pf->dir_at[src_ni] = 0xFF;
  pf->parent[src_ni] = PARENT_NONE;

  /* Track resources per-node using the existing arrays */
  {
    int shell_budget = shells;
    int tree_budget = trees;
    int mine_budget = mines;
    int armour_budget = armour;
    pf->shells_at[src_ni] = (int16_t)shell_budget;
    pf->trees_at[src_ni] = (int16_t)tree_budget;
    pf->mines_at[src_ni] = (int16_t)mine_budget;
    pf->armour_at[src_ni] = (int16_t)armour_budget;
  }

  heap_clear(pf);
  heap_push(pf, heuristic(sx, sy, dx, dy), (uint16_t)sx, (uint16_t)sy, (uint8_t)in_boat);
  n_pushes++;
  if (pf->open_count > peak_open) peak_open = pf->open_count;

  t1 = bp_now_us();
  t_init_us = t1 - t0;

  expanded = 0;
  result = COST_INF;

  while (pf->open_count > 0 && expanded < budget) {
    BrainPFHeapEntry entry = heap_pop(pf);
    int cx = entry.x;
    int cy = entry.y;
    int cur_boat = entry.boat;
    int ci = node_idx(cx, cy, cur_boat);
    int cur_dir, cur_shells, cur_trees, cur_mines, cur_armour;
    int d;

    /* Cooperative abort: same checkpoint as PathTo's main A* loop. On
     * hit we fall through to the post-loop block which returns the
     * best partial cost we found (or COST_INF if we never reached). */
    if (BRAIN_PF_ABORTED(pf)) break;

    n_pops++;
    if (get_closed(pf, ci)) { n_stale_pops++; continue; }
    closed_set(pf->closed, ci);
    expanded++;

    if (alog) {
      int tile_type = pf->map[cy * MAP_SIZE + cx] & 0x0F;
      uint16_t ov = pf->overlay_grid[cy * MAP_SIZE + cx];
      uint16_t dg = pf->danger_grid[cy * MAP_SIZE + cx];
      fprintf(alog, "ASTAR_DBG t=%d expand #%d: (%d,%d) boat=%d g=%.2f terrain=%d ov=%d d=%d f=%.2f\n",
              astar_log_tick, expanded, cx, cy, cur_boat, pf->g_cost[ci], tile_type, ov, dg, entry.f);
    }

    /* Destination reached */
    if (map_idx(ci) == dest_tile) {
      result = pf->g_cost[ci];
      if (alog) fprintf(alog, "  FOUND dest at expand #%d, cost=%.2f\n", expanded, result);
      break;
    }

    cur_dir = pf->dir_at[ci];
    cur_shells = pf->shells_at[ci];
    cur_trees = pf->trees_at[ci];
    cur_mines = pf->mines_at[ci];
    cur_armour = pf->armour_at[ci];

    for (d = 0; d < 8; d++) {
      int nx = cx + DX8[d];
      int ny = cy + DY8[d];
      int ni, shells_used, trees_used, mines_used, armour_used, onBoat;
      float tc, new_g, f;

      n_neigh_total++;

      if (nx < 0 || nx > 255 || ny < 0 || ny > 255) { n_neigh_oob++; continue; }

      /* Block diagonal moves through impassable corners — same rule as
       * brainPathfinderPathTo: on foot, EITHER solid cardinal corner
       * blocks the diagonal (a full-tile tank can't cut it); boats keep
       * the both-zero rule. This copy had the old BOTH-corners rule. */
      if (DX8[d] != 0 && DY8[d] != 0) {
        int adj_x_type = pf->map[(cy * MAP_SIZE) + (cx + DX8[d])] & 0x0F;
        int adj_y_type = pf->map[((cy + DY8[d]) * MAP_SIZE) + cx] & 0x0F;
        float speed_x = pf->terrain_speed_table[adj_x_type];
        float speed_y = pf->terrain_speed_table[adj_y_type];
        if (!cur_boat) {
          if (speed_x == 0.0f || speed_y == 0.0f) { n_neigh_diag++; continue; }
        } else {
          if (speed_x == 0.0f && speed_y == 0.0f) { n_neigh_diag++; continue; }
        }
        if (!cur_boat && (adj_x_type == TT_DEEPSEA || adj_y_type == TT_DEEPSEA)) { n_neigh_diag++; continue; }
        if (cur_boat && (!is_water_tile(adj_x_type) && adj_x_type != TT_BOAT)) { n_neigh_diag++; continue; }
        if (cur_boat && (!is_water_tile(adj_y_type) && adj_y_type != TT_BOAT)) { n_neigh_diag++; continue; }
      }

      tc = compute_cost(pf, nx, ny, cur_boat,
                         cur_shells, cur_trees, cur_mines, cur_armour,
                         &shells_used, &trees_used, &mines_used, &armour_used,
                         &onBoat);
      if (alog) {
        int n_type = pf->map[ny * MAP_SIZE + nx] & 0x0F;
        uint16_t n_ov = pf->overlay_grid[ny * MAP_SIZE + nx];
        uint16_t n_dg = pf->danger_grid[ny * MAP_SIZE + nx];
        fprintf(alog, "ASTAR_DBG t=%d   neigh(%d,%d) d=%d terrain=%d ov=%d dg=%d cost=%.2f%s\n",
                astar_log_tick, nx, ny, d, n_type, n_ov, n_dg, tc, tc >= COST_INF ? " INF" : "");
      }
      if (tc >= COST_INF) { n_neigh_inf++; continue; }
      /* allow_boat=0: skip nodes that would transition into boat state */
      if (!allow_boat && onBoat) { n_neigh_inf++; continue; }

      ni = node_idx(nx, ny, onBoat);
      if (get_closed(pf, ni)) { n_neigh_closed++; continue; }

      /* Turn penalty */
      if (cur_dir != 0xFF && cur_dir != d) {
        int tsteps = dir_steps(cur_dir, d);
        if (tsteps > 0) {
          tc += (float)tsteps * pf->turn_cost;
        }
      }

      new_g = pf->g_cost[ci] + tc * DMUL8[d];
      if (new_g >= get_g_cost(pf, ni)) { n_neigh_worse++; continue; }

      stamp_node(pf, ni);
      pf->g_cost[ni] = new_g;
      pf->shells_at[ni] = (int16_t)(cur_shells - shells_used);
      pf->trees_at[ni] = (int16_t)(cur_trees - trees_used);
      pf->mines_at[ni] = (int16_t)(cur_mines - mines_used);
      pf->armour_at[ni] = (int16_t)(cur_armour - armour_used);
      pf->dir_at[ni] = (uint8_t)d;
      pf->parent[ni] = (uint32_t)ci;

      f = new_g + heuristic(nx, ny, dx, dy);
      heap_push(pf, f, (uint16_t)nx, (uint16_t)ny, (uint8_t)onBoat);
      n_pushes++;
      n_neigh_relaxed++;
      if (pf->open_count > peak_open) peak_open = pf->open_count;
    }
  }

  t_search_us = bp_now_us() - t1;

  if (alog) {
    const char *term;
    if (result < COST_INF / 2.0f) term = "FOUND";
    else if (expanded >= budget)  term = "BUDGET_EXHAUSTED";
    else if (pf->open_count == 0) term = "HEAP_EMPTY (unreachable)";
    else                          term = "UNKNOWN";
    fprintf(alog,
      "  result=%.2f expanded=%d open_remaining=%d term=%s\n"
      "  timing: init=%.1fus search=%.1fus total=%.1fus  per-expand=%.2fus\n"
      "  heap: pushes=%d pops=%d stale_pops=%d peak_open=%d\n"
      "  neigh: total=%d oob=%d diag_blocked=%d cost_inf=%d already_closed=%d worse_g=%d relaxed=%d\n\n",
      result, expanded, pf->open_count, term,
      t_init_us, t_search_us, t_init_us + t_search_us,
      expanded > 0 ? t_search_us / (double)expanded : 0.0,
      n_pushes, n_pops, n_stale_pops, peak_open,
      n_neigh_total, n_neigh_oob, n_neigh_diag, n_neigh_inf, n_neigh_closed, n_neigh_worse, n_neigh_relaxed);
    fflush(alog);
  }

  /* Invalidate main search so next path_to() starts fresh */
  pf->status = -1;
  pf->dest_x = -1;
  pf->dest_y = -1;

  /* Flush so the log is readable while the brain is still running
   * (Windows MSVCRT treats _IOLBF as _IOFBF for files). */
  if (alog) fflush(alog);

  return result;
}

/* â”€â”€ Full Dijkstra from a source â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/* â”€â”€ Precomputed neighbor edge cost grid â”€â”€
 *
 * For each (tile, direction) pair, store the static base cost of stepping
 * from that tile in that direction:
 *   - terrain_cost_table[neighbor_type] (or boat table if onBoat)
 *   - COST_INF if neighbor is out of bounds, deep sea on foot, or
 *     blocked-corner diagonal
 *
 * Two layers: index = (boat ? NODE_COUNT/2 : 0) + ny*MAP_SIZE + nx
 *             multiplied by 8 for the 8 directions out from that node.
 *
 * The dynamic portion (danger, overlay, mine penalty, water drain,
 * resource minimums) still gets computed in the inner loop.
 */
void brainPathfinderRebuildEdgeCosts(BrainPathfinder *pf) {
  if (!pf || !pf->map) return;

  if (!pf->edge_cost) {
    pf->edge_cost = (float *)malloc(sizeof(float) * NODE_COUNT * 8);
    if (!pf->edge_cost) return;
  }

  /* For each (cx, cy, cur_boat) and each direction d, store the cost of
   * stepping to the neighbor. Mirrors the static portion of compute_cost
   * exactly. */
  for (int boat_layer = 0; boat_layer < 2; boat_layer++) {
    for (int cy = 0; cy < MAP_SIZE; cy++) {
      for (int cx = 0; cx < MAP_SIZE; cx++) {
        int ci = node_idx(cx, cy, boat_layer);
        for (int d = 0; d < 8; d++) {
          int nx = cx + DX8[d];
          int ny = cy + DY8[d];
          float ec;

          if (nx < 0 || nx > 255 || ny < 0 || ny > 255) {
            pf->edge_cost[ci * 8 + d] = COST_INF;
            continue;
          }

          /* Diagonal corner blocking — must match brainPathfinderPathTo's
           * rule (see the comment there): on foot a full-tile tank cannot
           * squeeze diagonally past ANY solid corner tile, so block the
           * diagonal if EITHER cardinal corner is impassable (speed 0 =
           * building/halfbuild). This copy kept the old BOTH-corners rule
           * long after the A* was fixed, so the nav Dijkstra planned
           * diagonals the tank physically cannot drive (it ground a
           * halfbuilding corner at full throttle for ~110 ticks —
           * 20260713_233008_1 t=11467). Boats keep the both-zero rule
           * plus the land-clip checks below. */
          if (DX8[d] != 0 && DY8[d] != 0) {
            int adj_x_type = pf->map[(cy * MAP_SIZE) + (cx + DX8[d])] & 0x0F;
            int adj_y_type = pf->map[((cy + DY8[d]) * MAP_SIZE) + cx] & 0x0F;
            float speed_x = pf->terrain_speed_table[adj_x_type];
            float speed_y = pf->terrain_speed_table[adj_y_type];
            if (!boat_layer) {
              if (speed_x == 0.0f || speed_y == 0.0f) { pf->edge_cost[ci * 8 + d] = COST_INF; continue; }
            } else {
              if (speed_x == 0.0f && speed_y == 0.0f) { pf->edge_cost[ci * 8 + d] = COST_INF; continue; }
            }
            if (!boat_layer && (adj_x_type == TT_DEEPSEA || adj_y_type == TT_DEEPSEA)) {
              pf->edge_cost[ci * 8 + d] = COST_INF; continue;
            }
            if (boat_layer && (!is_water_tile(adj_x_type) && adj_x_type != TT_BOAT)) {
              pf->edge_cost[ci * 8 + d] = COST_INF; continue;
            }
            if (boat_layer && (!is_water_tile(adj_y_type) && adj_y_type != TT_BOAT)) {
              pf->edge_cost[ci * 8 + d] = COST_INF; continue;
            }
          }

          /* Determine the boat state ON the destination tile (post-step) */
          int n_type = pf->map[ny * MAP_SIZE + nx] & 0x0F;
          int next_boat = next_boat_state(boat_layer, n_type);
          float base = next_boat ? pf->terrain_cost_boat_table[n_type]
                                 : pf->terrain_cost_table[n_type];
          /* Walls (BUILDING / HALFBUILD): traversable only on foot via
           * wall_shoot. Bake the wall_shoot cost into the edge cost when
           * we'd be on foot, so the Dijkstra plans through them. The
           * shell-budget gate (`shells_left >= wall_shoot_shells`) is
           * skipped here — Dijkstra is for cost ranking, not path
           * execution; the steering layer enforces the shell reserve
           * when actually committing to the move. */
          if (base >= WALL_THRESHOLD) {
            if (!next_boat && (n_type == TT_BUILDING || n_type == TT_HALFBUILD)) {
              ec = pf->wall_shoot_cost;
            } else {
              ec = COST_INF;
            }
          } else {
            ec = base;
          }
          pf->edge_cost[ci * 8 + d] = ec;
        }
      }
    }
  }
  pf->edge_cost_valid = 1;
  pf->edge_cost_map = pf->map;
  pf->cache_dirty = 1;
}

/* â”€â”€ Incremental Dijkstra â”€â”€
 *
 * Splits one full Dijkstra search across multiple ticks. Brain calls
 * Start once when it wants a fresh search, then Step every tick with a
 * per-tick budget. The g_cost array fills in as the wavefront expands;
 * lookups via DijkstraCostAt return COST_INF for not-yet-reached tiles.
 *
 * Note: shares the per-pf working arrays with the regular cost_to and
 * path_to searches. Calling Start clobbers any in-progress A* search.
 */

void brainPathfinderDijkstraPreheat(BrainPathfinder *pf) {
  if (!pf) return;
  for (int i = 0; i < DIJKSTRA_NUM_SLATES; i++) {
    DijkstraSlate *s = &pf->dij_slates[i];
    /* Same allocation set as brainPathfinderDijkstraStart's lazy block,
     * but unconditionally — call sites won't pay the malloc on tick 1. */
    if (!s->g_cost)    s->g_cost    = (float *)  malloc(sizeof(float)  * NODE_COUNT);
    if (!s->closed)    s->closed    = (uint8_t *)malloc(NODE_COUNT / 8);
    if (!s->dir_at)    s->dir_at    = (uint8_t *)malloc(NODE_COUNT);
    if (!s->heap) {
      s->heap_capacity = HEAP_INITIAL_CAPACITY;
      s->heap          = (BrainPFHeapEntry *)
                          malloc(sizeof(BrainPFHeapEntry) * s->heap_capacity);
    }
    if (!s->heap_pos)  s->heap_pos  = (int32_t *)malloc(sizeof(int32_t) * NODE_COUNT);
    /* Allocate the exact (shells_at) array too — most callers pass exact=1
     * (DIJKSTRA_EXACT), so eagerly reserve it. ~256 KB per slate. */
    if (!s->shells_at) s->shells_at = (int16_t *)malloc(sizeof(int16_t) * NODE_COUNT);

    /* Touch every page so the OS commits backing memory now. The other
     * arrays can stay zero (their default is meaningful: closed=0=not
     * closed, heap_pos=0=invalid pos, dir_at=0=no direction). g_cost
     * MUST initialize to COST_INF instead of zero, otherwise unstarted
     * backup slates would report "cost = 0" for every tile the moment
     * they show up in dijkstra_lookup_by_kind's slate walk — that's a
     * "this destination is free" lie that wrecks pool cost compares. */
    if (s->g_cost) {
      for (int j = 0; j < NODE_COUNT; j++) s->g_cost[j] = COST_INF;
    }
    if (s->closed)    memset(s->closed,   0, NODE_COUNT / 8);
    if (s->dir_at)    memset(s->dir_at,   0, NODE_COUNT);
    if (s->heap_pos)  memset(s->heap_pos, 0, sizeof(int32_t) * NODE_COUNT);
    if (s->shells_at) memset(s->shells_at,0, sizeof(int16_t) * NODE_COUNT);
    /* heap is small (HEAP_INITIAL_CAPACITY * 12 bytes ≈ 1.5 MB), let
     * dijkstra_start populate it lazily — most heap entries land in cache. */
  }
}

void brainPathfinderDijkstraStart(BrainPathfinder *pf, int slate, uint32_t tick,
                                   int sx, int sy, int in_boat,
                                   int shells, int trees, int mines, int armour,
                                   float max_cost, int exact,
                                   float danger_scale, int kind, int allow_boat) {
  (void)trees; (void)mines; (void)armour;
  if (!pf || !pf->map) return;
  if (slate < 0 || slate >= DIJKSTRA_NUM_SLATES) return;
  /* On-demand dijkstra logging: touch "log_next_dijkstra" to trigger.
   * Only logs for player 0's pathfinder. Deletes the trigger file and
   * logs one full slate start+expansion, then closes the log. */
  {
    FILE *trigger = fopen("log_next_dijkstra", "r");
    if (trigger) {
      fclose(trigger);
      remove("log_next_dijkstra");
      if (!dijkstra_log) brainPathfinderEnableDijkstraLog(1);
    }
  }
  DijkstraSlate *s = &pf->dij_slates[slate];

  /* Lazy edge-cost rebuild */
  if (!pf->edge_cost_valid || pf->edge_cost_map != pf->map) {
    brainPathfinderRebuildEdgeCosts(pf);
  }

  /* Lazy-allocate this slate's working arrays.
   * Per-slate: ~700 KB without exact, ~960 KB with. */
  if (!s->g_cost) s->g_cost = (float *)malloc(sizeof(float) * NODE_COUNT);
  if (!s->closed) s->closed = (uint8_t *)malloc(NODE_COUNT / 8);
  if (!s->dir_at) s->dir_at = (uint8_t *)malloc(NODE_COUNT);
  if (!s->heap) {
    s->heap_capacity = HEAP_INITIAL_CAPACITY;
    s->heap = (BrainPFHeapEntry *)malloc(sizeof(BrainPFHeapEntry) * s->heap_capacity);
  }
  if (!s->heap_pos) s->heap_pos = (int32_t *)malloc(sizeof(int32_t) * NODE_COUNT);
  if (exact && !s->shells_at) {
    s->shells_at = (int16_t *)malloc(sizeof(int16_t) * NODE_COUNT);
  }
  if (!s->g_cost || !s->closed || !s->dir_at || !s->heap || !s->heap_pos
      || (exact && !s->shells_at)) {
    if (dijkstra_log) {
      fprintf(dijkstra_log, "START slate=%d: ALLOC FAILED\n", slate);
      fflush(dijkstra_log);
    }
    return;
  }

  if (sx < 0) sx = 0; if (sx > 255) sx = 255;
  if (sy < 0) sy = 0; if (sy > 255) sy = 255;

  /* Init this slate's state — fully isolated from other slates and from
   * the shared cost_to/path_to working arrays. */
  for (int i = 0; i < NODE_COUNT; i++) s->g_cost[i] = COST_INF;
  memset(s->closed, 0, NODE_COUNT / 8);
  memset(s->dir_at, 0xFF, NODE_COUNT);

  int src_ni = node_idx(sx, sy, in_boat);
  s->g_cost[src_ni] = 0.0f;
  if (exact) {
    s->shells_at[src_ni] = (int16_t)shells;
  }

  slate_heap_clear(s);
  slate_heap_push(s, 0.0f, (uint16_t)sx, (uint16_t)sy, (uint8_t)in_boat);

  s->done           = 0;
  s->exact          = exact ? 1 : 0;
  s->allow_boat     = allow_boat ? 1 : 0;
  s->kind           = kind;
  s->src_x          = sx;
  s->src_y          = sy;
  s->in_boat        = in_boat;
  s->src_shells     = shells;
  s->expanded       = 0;
  s->peak_open      = s->open_count;
  s->max_cost       = (max_cost > 0.0f) ? max_cost : 0.0f;
  s->danger_scale   = danger_scale;
  s->version++;
  s->started_tick   = tick;
  s->completed_tick = 0;

  if (dijkstra_log) {
    fprintf(dijkstra_log,
      "START slate=%d v=%u tick=%u kind=%d src=(%d,%d) boat=%d max_cost=%.0f exact=%d shells=%d danger_scale=%.2f\n",
      slate, s->version, tick, kind, sx, sy, in_boat,
      s->max_cost, exact, shells, danger_scale);
    fflush(dijkstra_log);
  }
}

int brainPathfinderDijkstraStep(BrainPathfinder *pf, int slate, uint32_t tick, int budget) {
  if (!pf) return 1;
  if (slate < 0 || slate >= DIJKSTRA_NUM_SLATES) return 1;
  DijkstraSlate *s = &pf->dij_slates[slate];
  if (!s->g_cost || s->done) return 1;
  if (!s->g_cost || !s->closed || !s->dir_at || !s->heap || !s->heap_pos) return 1;

  int expanded_this_step = 0;
  float max_cost = s->max_cost;
  int has_cap = (max_cost > 0.0f);
  int exact = s->exact;
  int wall_shoot_shells = (int)pf->wall_shoot_shells;
  int min_shells = (int)pf->min_shells;

  /* Hot-path local pointers — slate's dedicated arrays. */
  float *g = s->g_cost;
  uint8_t *closed = s->closed;
  uint8_t *dir_at = s->dir_at;
  int16_t *shells_at = s->shells_at; /* may be NULL when !exact */
  const float *edge_cost = pf->edge_cost;
  const uint16_t *danger_grid = pf->danger_grid;
  const int16_t *overlay_grid = pf->overlay_grid;
  const BYTE *map = pf->map;
  float danger_scale = s->danger_scale; /* per-slate, NOT pf->danger_scale */
  float turn_cost = pf->turn_cost;

  float inv_speed_foot[16];
  float inv_speed_boat[16];
  for (int t = 0; t < 16; t++) {
    float spd_foot = pf->terrain_speed_table[t];
    inv_speed_foot[t] = 16.0f / fmaxf(spd_foot, 0.1f);
    float spd_boat = is_water_tile(t) ? 16.0f : spd_foot;
    inv_speed_boat[t] = 16.0f / fmaxf(spd_boat, 0.1f);
  }
  float turn_lut[8][8];
  for (int a = 0; a < 8; a++) {
    for (int b = 0; b < 8; b++) {
      int diff = abs(a - b);
      if (diff > 4) diff = 8 - diff;
      turn_lut[a][b] = (float)diff * turn_cost;
    }
  }

  while (s->open_count > 0 && expanded_this_step < budget) {
    BrainPFHeapEntry entry = slate_heap_pop(s);
    int cx = entry.x;
    int cy = entry.y;
    int cur_boat = entry.boat;
    int ci = node_idx(cx, cy, cur_boat);

    /* Cooperative abort: incremental Dijkstra is already designed to
     * stop mid-search and resume next call, so an early break here is
     * exactly the existing budget-exhausted path. s->done stays 0 so
     * the brain sees an in-progress slate next tick. */
    if (BRAIN_PF_ABORTED(pf)) break;

    if (closed_test(closed, ci)) continue;
    closed_set(closed, ci);
    expanded_this_step++;
    s->expanded++;

    /* Optional max-cost cap: stop expanding once we exceed the budget. */
    if (has_cap && g[ci] > max_cost) continue;

    /* dir_at packs (direction | parent_boat<<3), so mask bits 0-2 to get
     * the actual arrival direction for turn-penalty math. The 0xFF
     * source sentinel passes through unchanged. */
    int dir_raw = dir_at[ci];
    int cur_dir = (dir_raw == 0xFF) ? 0xFF : (dir_raw & 0x07);
    int cur_shells = exact ? shells_at[ci] : 0;
    int base_edge = ci * 8;
    for (int d = 0; d < 8; d++) {
      float ec = edge_cost[base_edge + d];
      if (ec >= COST_INF) continue;

      int nx = cx + DX8[d];
      int ny = cy + DY8[d];
      int nm = ny * MAP_SIZE + nx;
      int n_type = map[nm] & 0x0F;
      int next_boat = next_boat_state(cur_boat, n_type);
      /* Land-only slate (SHORT) normally skips any boat transition / deep sea.
       * Exception: near-shore tiles in the coastal band are allowed, so SHORT
       * can take short boat hops along the coast without expanding open ocean. */
      if (!s->allow_boat && (next_boat || n_type == TT_DEEPSEA)
          && !pf->coastal_boat_mask[nm]) continue;
      int new_shells = cur_shells;

      float tc;
      /* Wall-shoot edges: base wall_shoot_cost PLUS danger at the tile.
       * You still take pill fire while standing there shooting the wall. */
      if (!next_boat && (n_type == TT_BUILDING || n_type == TT_HALFBUILD)) {
        float wall_danger = (float)danger_grid[nm] * danger_scale
                          * (16.0f / fmaxf(3.0f, 0.1f)); /* speed~3 while stopped shooting */
        tc = ec + wall_danger;
        if (exact) {
          new_shells = cur_shells - wall_shoot_shells;
          if (new_shells < min_shells) continue; /* not enough shells */
          /* Escalating wall-break penalty:
           *   walls 1-2: normal cost
           *   wall 3+: cost doubles for each additional wall
           * Makes thick walls exponentially more expensive. */
          int walls_broken = (s->src_shells - cur_shells) / (wall_shoot_shells > 0 ? wall_shoot_shells : 1);
          if (walls_broken >= 2) {
            int extra = walls_broken - 2; /* 0 for wall #3, 1 for #4, etc */
            tc *= (float)(1 << (extra + 1)); /* 2x, 4x, 8x, 16x... */
          }
        }
      } else {
        /* Normal tile: add danger * scale * (16/speed) + overlay +
         * mine penalty. Same formula as compute_cost(). */
        float danger = (float)danger_grid[nm] + (float)pf->danger_offset_grid[nm];
        if (danger < 0.0f) danger = 0.0f;
        float inv_spd = next_boat ? inv_speed_boat[n_type] : inv_speed_foot[n_type];
        float overlay = (float)overlay_grid[nm];
        float dynamic_extra = danger * danger_scale * inv_spd + overlay;
        if (map[nm] & 0x80) dynamic_extra += pf->mine_penalty;
        tc = ec + dynamic_extra;

        /* Water drain: walking through a river tile on foot loses shells
         * and mines. compute_cost adds shell_loss_cost*drain +
         * mine_loss_cost*drain to the cost (and depletes shells). The
         * road-build branch below overrides this for low-danger tiles
         * with trees available, matching compute_cost's behavior. */
        if (!next_boat && n_type == TT_RIVER && pf->water_drain_rate > 0.0f) {
          float wd = pf->water_drain_rate;
          tc += wd * pf->shell_loss_cost;
          tc += wd * pf->mine_loss_cost;
          if (exact) {
            int drain = (int)wd;
            if (drain > new_shells) drain = new_shells; /* clamp */
            new_shells -= drain;
            if (new_shells < min_shells) continue;
          }
        }

        /* Road-build short-circuit: when on foot in slow terrain
         * (swamp/crater/rubble/river) and danger is low, compute_cost
         * returns road_build_cost (default 12) directly — assuming the
         * LGM will pave the tile. We don't track trees, so we assume
         * trees are always available. When road build fires, it also
         * undoes the water drain we just added (compute_cost does the
         * same: trees_used=2, shells_used -= drain). */
        if (!next_boat && danger < pf->road_build_danger_max
            && (n_type == TT_SWAMP || n_type == TT_CRATER
                || n_type == TT_RUBBLE || n_type == TT_RIVER)) {
          if (tc > pf->road_build_cost) {
            tc = pf->road_build_cost;
            if (exact && n_type == TT_RIVER) {
              /* Refund the shells we just drained — road build paves
               * the river, no wading required. */
              int refund = (int)pf->water_drain_rate;
              new_shells += refund;
              if (new_shells > 32767) new_shells = 32767;
            }
          }
        }
      }

      int ni = node_idx(nx, ny, next_boat);
      if (closed_test(closed, ni)) continue;

      /* Turn penalty — LUT entry for d==cur_dir is 0, so no explicit guard needed. */
      if (cur_dir != 0xFF) tc += turn_lut[cur_dir][d];

      float new_g = g[ci] + tc * DMUL8[d];
      if (has_cap && new_g > max_cost) continue;
      if (new_g >= g[ni]) continue;

      if (dijkstra_log) {
        float danger = (float)danger_grid[nm];
        float overlay = (float)overlay_grid[nm];
        fprintf(dijkstra_log,
          "TILE s=%d t=%u (%d,%d) d=%d ec=%.1f tc=%.1f g=%.1f->%.1f type=%d dng=%.0f ovl=%.0f boat=%d\n",
          slate, tick, nx, ny, d, ec, tc, new_g - g[ci], new_g, n_type,
          danger, overlay, next_boat);
      }

      g[ni] = new_g;
      /* Encode (direction | parent_boat<<3) so the trace function can
       * jump to the correct boat layer at the parent. Bits 0-2 = dir,
       * bit 3 = parent's boat state. 0xFF stays the source sentinel. */
      dir_at[ni] = (uint8_t)(d | (cur_boat << 3));
      if (exact) shells_at[ni] = (int16_t)new_shells;

      slate_heap_push(s, new_g, (uint16_t)nx, (uint16_t)ny, (uint8_t)next_boat);
      if (s->open_count > s->peak_open) s->peak_open = s->open_count;
    }
  }

  if (s->open_count == 0 && !s->done) {
    s->done = 1;
    s->completed_tick = tick;
  }

  if (dijkstra_log) {
    fprintf(dijkstra_log,
      "STEP slate=%d v=%u tick=%u this=%d total=%d open=%d peak_open=%d done=%d completed_tick=%u\n",
      slate, s->version, tick, expanded_this_step, s->expanded,
      s->open_count, s->peak_open, s->done, s->completed_tick);
    fflush(dijkstra_log);
    /* Auto-close after slate completes so we only capture one cycle */
    if (s->done) {
      brainPathfinderEnableDijkstraLog(0);
    }
  }

  return s->done;
}

float brainPathfinderDijkstraCostAt(BrainPathfinder *pf, int slate,
                                     int x, int y, int boat) {
  if (!pf) return COST_INF;
  if (slate < 0 || slate >= DIJKSTRA_NUM_SLATES) return COST_INF;
  DijkstraSlate *s = &pf->dij_slates[slate];
  if (!s->g_cost) return COST_INF;
  if (x < 0 || x > 255 || y < 0 || y > 255) return COST_INF;
  /* Source tile is stored as g_cost = 0 by construction — required by
   * Dijkstra so the algorithm has a starting node. But external
   * lookups treating "the tile we're standing on" as cheapest skews
   * goal selection (any candidate at the source position wins on
   * cost = 0). Return COST_INF for the source tile so cost-comparison
   * code sees it as not-a-target. Internal expansion is unaffected;
   * it reads g_cost directly. */
  int b = boat ? 1 : 0;
  if (x == s->src_x && y == s->src_y && b == s->in_boat) return COST_INF;
  return s->g_cost[node_idx(x, y, b)];
}

/* Copy src slate's cost arrays and metadata to dst so dst can serve as a
 * read-only fallback while src is being recomputed.  dst is marked done=1
 * so the step loop never advances it.  The heap is not copied — dst is
 * purely a lookup snapshot. */
void brainPathfinderDijkstraCopySlate(BrainPathfinder *pf, int src, int dst) {
  if (!pf) return;
  if (src < 0 || src >= DIJKSTRA_NUM_SLATES) return;
  if (dst < 0 || dst >= DIJKSTRA_NUM_SLATES) return;
  if (src == dst) return;

  DijkstraSlate *s = &pf->dij_slates[src];
  DijkstraSlate *d = &pf->dij_slates[dst];
  if (!s->g_cost) return; /* nothing to copy */

  /* Lazy-allocate dst arrays */
  if (!d->g_cost)   d->g_cost = (float *)malloc(sizeof(float) * NODE_COUNT);
  if (!d->closed)   d->closed = (uint8_t *)malloc(NODE_COUNT / 8);
  if (!d->dir_at)   d->dir_at = (uint8_t *)malloc(NODE_COUNT);
  if (!d->g_cost || !d->closed || !d->dir_at) return;

  memcpy(d->g_cost,  s->g_cost,  sizeof(float) * NODE_COUNT);
  memcpy(d->closed,  s->closed,  NODE_COUNT / 8);
  memcpy(d->dir_at,  s->dir_at,  NODE_COUNT);

  if (s->shells_at) {
    if (!d->shells_at) d->shells_at = (int16_t *)malloc(sizeof(int16_t) * NODE_COUNT);
    if (d->shells_at) memcpy(d->shells_at, s->shells_at, sizeof(int16_t) * NODE_COUNT);
  }

  d->done           = 1; /* backup is never stepped */
  d->exact          = s->exact;
  d->allow_boat     = s->allow_boat;
  d->kind           = s->kind;
  d->src_x          = s->src_x;
  d->src_y          = s->src_y;
  d->in_boat        = s->in_boat;
  d->src_shells     = s->src_shells;
  d->expanded       = s->expanded;
  d->peak_open      = s->peak_open;
  d->max_cost       = s->max_cost;
  d->danger_scale   = s->danger_scale;
  d->version        = s->version;
  d->started_tick   = s->started_tick;
  d->completed_tick = s->completed_tick;

  /* Clear the heap — dst won't be stepped so we don't need it */
  slate_heap_clear(d);
}

/* Sort slate indices for the given kind by started_tick descending
 * (most recently started first). If the newest slate hasn't reached a
 * tile yet, the caller falls through to the next-newest. Used for both
 * lookup and trace so all operations prefer the freshest source position.
 * Slates without matching kind or with no g_cost yet are excluded. */
static int slate_indices_by_recency(BrainPathfinder *pf, int kind,
                                     int out[DIJKSTRA_NUM_SLATES]) {
  int n = 0;
  for (int i = 0; i < DIJKSTRA_NUM_SLATES; i++) {
    DijkstraSlate *s = &pf->dij_slates[i];
    if (!s->g_cost || s->kind != kind) continue;
    out[n++] = i;
  }
  /* Insertion sort by started_tick descending — n is at most 4. */
  for (int i = 1; i < n; i++) {
    int key = out[i];
    uint32_t kt = pf->dij_slates[key].started_tick;
    int j = i - 1;
    while (j >= 0 && pf->dij_slates[out[j]].started_tick < kt) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = key;
  }
  return n;
}

float brainPathfinderDijkstraLookupByKind(BrainPathfinder *pf, int kind,
                                           int x, int y, int boat) {
  if (!pf) return COST_INF;
  if (x < 0 || x > 255 || y < 0 || y > 255) return COST_INF;
  (void)boat; /* both layers are always considered (we take the cheaper) */

  int order[DIJKSTRA_NUM_SLATES];
  int n = slate_indices_by_recency(pf, kind, order);

  /* Walk slates freshest-completed-first. If the freshest finished slate
   * hasn't reached this tile, fall through to the next. Running slates
   * (completed_tick=0) are last-resort fallback for tiles already expanded. */
  for (int i = 0; i < n; i++) {
    DijkstraSlate *s = &pf->dij_slates[order[i]];
    /* Same source-tile rationale as DijkstraCostAt: don't report 0 for
     * the slate's own start position. If this tile IS the source, treat
     * it as not-a-target on this slate and try the next slate
     * (different src_x/src_y might give a real cost). */
    if (x == s->src_x && y == s->src_y) continue;
    float land = s->g_cost[node_idx(x, y, 0)];
    float boatv = s->g_cost[node_idx(x, y, 1)];
    float c = (boatv < land) ? boatv : land;
    if (c < COST_INF) return c;
  }
  return COST_INF;
}

float brainPathfinderDijkstraLookupSubtractByKind(BrainPathfinder *pf, int kind,
                                                   int x, int y, int boat,
                                                   BrainPFTileLookupFn pcontrib_lookup,
                                                   void *user) {
  if (!pf) return COST_INF;
  if (x < 0 || x > 255 || y < 0 || y > 255) return COST_INF;
  (void)boat;

  int order[DIJKSTRA_NUM_SLATES];
  int n = slate_indices_by_recency(pf, kind, order);

  DijkstraSlate *chosen = NULL;
  int chosen_layer = 0;
  float base_cost = COST_INF;
  for (int i = 0; i < n; i++) {
    DijkstraSlate *s = &pf->dij_slates[order[i]];
    if (!s->g_cost || !s->dir_at) continue;
    if (x == s->src_x && y == s->src_y) continue;
    float land = s->g_cost[node_idx(x, y, 0)];
    float boatv = s->g_cost[node_idx(x, y, 1)];
    if (land >= COST_INF && boatv >= COST_INF) continue;
    chosen = s;
    if (boatv < land) { chosen_layer = 1; base_cost = boatv; }
    else              { chosen_layer = 0; base_cost = land;  }
    break;
  }
  if (!chosen) return COST_INF;
  if (!pcontrib_lookup) return base_cost;

  /* Inv-speed tables matching the slate's expansion formula
   * (line ~1832: inv_spd = next_boat ? boat[t] : foot[t]). */
  float inv_speed_foot[16];
  float inv_speed_boat[16];
  for (int t = 0; t < 16; t++) {
    float spd = pf->terrain_speed_table[t];
    inv_speed_foot[t] = 16.0f / fmaxf(spd, 0.1f);
    float spd_b = is_water_tile(t) ? 16.0f : spd;
    inv_speed_boat[t] = 16.0f / fmaxf(spd_b, 0.1f);
  }

  /* Walk parent chain from dest back to source via dir_at. Source tile
   * has dir_at = 0xFF and contributes 0 to g_cost (no danger added at
   * start), so we exclude it from the subtraction. Each step's
   * subtract is scaled by DMUL8[d] to match the slate's expansion. */
  float subtract = 0.0f;
  float dscale = chosen->danger_scale;
  int cur = node_idx(x, y, chosen_layer);
  int cur_boat = chosen_layer;
  int safety = 2048;
  while (safety-- > 0) {
    uint8_t dval = chosen->dir_at[cur];
    if (dval == 0xFF) break;
    int cx = node_x(cur);
    int cy = node_y(cur);
    int tile_key = cy * MAP_SIZE + cx;
    float p = pcontrib_lookup(user, tile_key);
    int d = dval & 0x07;
    int parent_boat = (dval & 0x08) ? 1 : 0;
    if (p > 0.0f) {
      int tt = pf->map[tile_key] & 0x0F;
      /* Mirror the three slate-expansion branches exactly so the
       * subtract removes the same per-pill contribution the slate
       * actually added. See the matching code around lines
       * 1837-1903 (wall-shoot, normal, road-build short-circuit). */
      float p_term;
      int is_wall_tile = !cur_boat && (tt == TT_BUILDING || tt == TT_HALFBUILD);
      if (is_wall_tile) {
        /* Wall-shoot expansion: tc = wall_shoot_cost + danger*dscale*16/3
         * (the LGM stops to shoot at speed ~3 so inv_spd = 16/3, not
         * the terrain table's value which would be ~160 for a wall).
         * Per-pill contribution to that danger term is the same
         * formula with pcontrib in place of total danger. */
        p_term = p * dscale * (16.0f / 3.0f);
      } else {
        /* Normal tile: tc = ec + danger*dscale*inv_spd + overlay + mine_pen.
         * Per-pill contribution: p*dscale*inv_spd. */
        float inv_spd = cur_boat ? inv_speed_boat[tt] : inv_speed_foot[tt];
        p_term = p * dscale * inv_spd;
        /* Road-build short-circuit: on foot in slow terrain
         * (swamp/crater/rubble/river) with total tile danger below
         * threshold AND the uncapped tc exceeding road_build_cost,
         * the slate clamps tc flat at road_build_cost — effectively
         * discarding the part of (danger + water_drain) that was
         * over budget. Reconstruct the uncapped tc here; if the
         * clamp would have fired, replace p_term with pcontrib's
         * proportional share of the SURVIVING danger budget
         * (road_build_cost - ec - overlay - mine_pen, floored at 0).
         * Pre-existing under-subtract before this branch was added. */
        if (!cur_boat
            && (tt == TT_SWAMP || tt == TT_CRATER
                || tt == TT_RUBBLE || tt == TT_RIVER)) {
          float total_danger = (float)pf->danger_grid[tile_key]
                             + (float)pf->danger_offset_grid[tile_key];
          if (total_danger < 0.0f) total_danger = 0.0f;
          if (total_danger < pf->road_build_danger_max) {
            float ec       = pf->terrain_cost_table[tt];
            float overlay  = (float)pf->overlay_grid[tile_key];
            float mine_pen = (pf->map[tile_key] & 0x80) ? pf->mine_penalty : 0.0f;
            float danger_term = total_danger * dscale * inv_spd;
            float water_drain = 0.0f;
            if (tt == TT_RIVER && pf->water_drain_rate > 0.0f) {
              water_drain = pf->water_drain_rate
                          * (pf->shell_loss_cost + pf->mine_loss_cost);
            }
            float tc_uncapped = ec + danger_term + overlay + mine_pen + water_drain;
            if (tc_uncapped > pf->road_build_cost) {
              /* Clamp fired. Effective danger contribution that
               * survived in tc = road_build_cost minus the
               * non-danger fixed components (water_drain is also
               * clamped away with the rest). Pcontrib's share is
               * proportional to its fraction of total_danger. */
              float effective_danger = pf->road_build_cost - ec - overlay - mine_pen;
              if (effective_danger < 0.0f) effective_danger = 0.0f;
              p_term = (total_danger > 0.0f)
                       ? (p / total_danger) * effective_danger
                       : 0.0f;
            }
          }
        }
      }
      subtract += p_term * DMUL8[d];
    }
    int px = cx - DX8[d];
    int py = cy - DY8[d];
    if (px < 0 || px > 255 || py < 0 || py > 255) break;
    cur = node_idx(px, py, parent_boat);
    cur_boat = parent_boat;
  }

  float corrected = base_cost - subtract;
  if (corrected < 0.0f) corrected = 0.0f;
  return corrected;
}

/* Trace the Dijkstra parent chain from (dx,dy) back toward the source,
 * find the tank's current position (sx,sy) in the chain, and return
 * the next step from there toward the destination.
 * Returns 1 on success, 0 if unreachable or tank not on path. */
/* Live-obstacle membership: small linear scan over packed (y*256+x) keys.
 * n is tiny (a handful of ally tank tiles), so a scan beats any structure. */
static int obs_contains(const int *obs, int n, int x, int y) {
  if (!obs || n <= 0) return 0;
  int key = y * 256 + x;
  for (int i = 0; i < n; i++) if (obs[i] == key) return 1;
  return 0;
}

int brainPathfinderDijkstraNextStep(BrainPathfinder *pf, int kind,
                                     int sx, int sy,
                                     int dx, int dy,
                                     const int *obstacles, int n_obstacles,
                                     float penalty,
                                     int *out_next_x, int *out_next_y) {
  if (!pf) return 0;
  if (dx < 0 || dx > 255 || dy < 0 || dy > 255) return 0;
  if (out_next_x) *out_next_x = -1;
  if (out_next_y) *out_next_y = -1;

  int order[DIJKSTRA_NUM_SLATES];
  int n = slate_indices_by_recency(pf, kind, order);

  for (int si = 0; si < n; si++) {
    DijkstraSlate *s = &pf->dij_slates[order[si]];
    if (!s->g_cost || !s->dir_at) continue;

    /* Pick the cheaper boat layer at the destination */
    int ni_land = node_idx(dx, dy, 0);
    int ni_boat = node_idx(dx, dy, 1);
    float c_land = s->g_cost[ni_land];
    float c_boat = s->g_cost[ni_boat];
    if (c_land >= COST_INF && c_boat >= COST_INF) continue;

    int cur_ni = (c_boat < c_land) ? ni_boat : ni_land;

    /* Trace backwards from destination toward source.
     * Build a reversed chain and find where (sx,sy) appears. */
    int chain[512];
    int chain_len = 0;
    int steps = 0;
    while (steps++ < NODE_COUNT && chain_len < 512) {
      chain[chain_len++] = cur_ni;
      int dir_raw = s->dir_at[cur_ni];
      if (dir_raw == 0xFF) break; /* reached Dijkstra source */
      int d = dir_raw & 0x07;
      int parent_boat = (dir_raw >> 3) & 1;
      int px = node_x(cur_ni) - DX8[d];
      int py = node_y(cur_ni) - DY8[d];
      if (px < 0 || px > 255 || py < 0 || py > 255) break;
      cur_ni = node_idx(px, py, parent_boat);
    }

    /* chain[0] = destination, chain[chain_len-1] = source (or near it).
     * Find (sx,sy) in the chain and return the immediate next tile
     * (chain[i-1]). Previously this compacted straight runs into a
     * single multi-tile jump; that produced pf.next values cheb > 1
     * from source and caused the steering viz / logic to see "next"
     * as a far waypoint instead of the adjacent tile. The Lua-side
     * path_lookahead already handles LOS-based skipping in steering,
     * so this C-side compaction was redundant — removed. */
    for (int i = chain_len - 1; i >= 0; i--) {
      if (node_x(chain[i]) == sx && node_y(chain[i]) == sy) {
        if (i <= 0) return 0; /* Already at destination */
        int nnx = node_x(chain[i - 1]);
        int nny = node_y(chain[i - 1]);
        /* Live-obstacle veer: the optimal next tile is occupied (e.g. an ally
         * tank). Pick the cheapest non-obstacle neighbour of (sx,sy) by
         * effective cost (g_cost + penalty), still descending the field, so we
         * dodge around it this tick and rejoin the gradient. */
        if (obs_contains(obstacles, n_obstacles, nnx, nny)) {
          float best_eff = COST_INF;
          float best_d2  = 1e30f;
          for (int d = 0; d < 8; d++) {
            int ax = sx + DX8[d], ay = sy + DY8[d];
            if (ax < 0 || ax > 255 || ay < 0 || ay > 255) continue;
            float gl = s->g_cost[node_idx(ax, ay, 0)];
            float gb = s->g_cost[node_idx(ax, ay, 1)];
            float g  = (gb < gl) ? gb : gl;
            if (g >= COST_INF) continue;
            float eff = g + (obs_contains(obstacles, n_obstacles, ax, ay) ? penalty : 0.0f);
            /* Goal-directed tie-break: on (near-)equal effective cost prefer the
             * tile nearer the destination, so a flat field drifts TOWARD the goal
             * instead of toward whatever compass dir the scan (N,NE,E,SE,S,SW,W,NW)
             * happens to hit first. Without this, ties silently resolve E-before-SW
             * etc., sending the tank away from the goal. */
            float d2 = (float)((ax - dx) * (ax - dx) + (ay - dy) * (ay - dy));
            if (eff < best_eff - 1e-3f ||
                (eff < best_eff + 1e-3f && d2 < best_d2)) {
              best_eff = eff; best_d2 = d2; nnx = ax; nny = ay;
            }
          }
        }
        if (out_next_x) *out_next_x = nnx;
        if (out_next_y) *out_next_y = nny;
        return 1;
      }
    }

    /* Tank not on the traced path — it may have drifted off.
     * Fall back: find the neighbor of (sx,sy) with the lowest g_cost
     * that is closer to the destination than (sx,sy) itself. */
    float my_land = s->g_cost[node_idx(sx, sy, 0)];
    float my_boat = s->g_cost[node_idx(sx, sy, 1)];
    float my_g = (my_boat < my_land) ? my_boat : my_land;
    if (my_g >= COST_INF) continue; /* tank position not reached by this slate */

    float best_g = my_g;
    float best_d2 = 1e30f;
    int best_nx = -1, best_ny = -1;
    for (int d = 0; d < 8; d++) {
      int nx = sx + DX8[d];
      int ny = sy + DY8[d];
      if (nx < 0 || nx > 255 || ny < 0 || ny > 255) continue;
      /* Check both boat layers, take cheaper */
      float ng_land = s->g_cost[node_idx(nx, ny, 0)];
      float ng_boat = s->g_cost[node_idx(nx, ny, 1)];
      float ng = (ng_boat < ng_land) ? ng_boat : ng_land;
      /* Live-obstacle veer (same as the on-path case): treat occupied tiles as
       * far more expensive so the drifted tank routes around them too. */
      float ng_eff = ng + (obs_contains(obstacles, n_obstacles, nx, ny) ? penalty : 0.0f);
      /* Goal-directed tie-break (same rationale as the on-path case): among
       * neighbours cheaper than our own tile, prefer the one nearest the goal so
       * ties don't resolve by fixed compass order. */
      float d2 = (float)((nx - dx) * (nx - dx) + (ny - dy) * (ny - dy));
      if (ng_eff < best_g - 1e-3f ||
          (best_nx >= 0 && ng_eff < best_g + 1e-3f && d2 < best_d2)) {
        best_g = ng_eff;
        best_d2 = d2;
        best_nx = nx;
        best_ny = ny;
      }
    }
    if (best_nx >= 0) {
      if (out_next_x) *out_next_x = best_nx;
      if (out_next_y) *out_next_y = best_ny;
      return 1;
    }
  }
  return 0;
}

int brainPathfinderDijkstraFindBest(BrainPathfinder *pf, int kind) {
  if (!pf) return -1;
  int best = -1;
  uint32_t best_tick = 0;
  for (int i = 0; i < DIJKSTRA_NUM_SLATES; i++) {
    DijkstraSlate *s = &pf->dij_slates[i];
    if (!s->g_cost || s->kind != kind) continue;
    if (best < 0 || s->started_tick > best_tick) {
      best = i;
      best_tick = s->started_tick;
    }
  }
  return best;
}

int brainPathfinderDijkstraPickReuseSlate(BrainPathfinder *pf, int kind) {
  if (!pf) return 0;
  /* First preference: a never-used slate (no g_cost allocated). */
  for (int i = 0; i < DIJKSTRA_NUM_SLATES; i++) {
    if (!pf->dij_slates[i].g_cost) return i;
  }
  /* Second: the OLDEST slate of matching kind (preserves the freshest
   * same-kind slate as a fallback for in-flight lookups). */
  int oldest_same = -1;
  uint32_t oldest_same_tick = 0xFFFFFFFFu;
  for (int i = 0; i < DIJKSTRA_NUM_SLATES; i++) {
    DijkstraSlate *s = &pf->dij_slates[i];
    if (s->kind != kind) continue;
    if (oldest_same < 0 || s->started_tick < oldest_same_tick) {
      oldest_same = i;
      oldest_same_tick = s->started_tick;
    }
  }
  if (oldest_same >= 0) return oldest_same;
  /* Third: oldest slate of ANY kind. */
  int oldest = 0;
  for (int i = 1; i < DIJKSTRA_NUM_SLATES; i++) {
    if (pf->dij_slates[i].started_tick < pf->dij_slates[oldest].started_tick) {
      oldest = i;
    }
  }
  return oldest;
}

const DijkstraSlate *brainPathfinderDijkstraGetSlate(BrainPathfinder *pf, int slate) {
  if (!pf || slate < 0 || slate >= DIJKSTRA_NUM_SLATES) return NULL;
  return &pf->dij_slates[slate];
}

/* Walks slates by recency, picks the first where (dx, dy) is reachable
 * (cheaper boat layer < INF), then traces from that one. O(slates) for
 * the slate selection (cheap; O(1) cost-at lookup per slate), O(path)
 * for the trace. Mirrors LookupByKind's selection so the chosen slate
 * is the same one that returned the cost. */
int brainPathfinderDijkstraTracePathByKind(BrainPathfinder *pf, int kind,
                                            int dx, int dy,
                                            int *path_x, int *path_y,
                                            int max_steps) {
  if (!pf) return 0;
  if (dx < 0 || dx > 255 || dy < 0 || dy > 255) return 0;
  int order[DIJKSTRA_NUM_SLATES];
  int n = slate_indices_by_recency(pf, kind, order);
  for (int i = 0; i < n; i++) {
    int slate = order[i];
    DijkstraSlate *s = &pf->dij_slates[slate];
    if (!s->g_cost || !s->dir_at) continue;
    float c_land = s->g_cost[node_idx(dx, dy, 0)];
    float c_boat = s->g_cost[node_idx(dx, dy, 1)];
    if (c_land >= COST_INF && c_boat >= COST_INF) continue;
    int got = brainPathfinderDijkstraTracePath(pf, slate, dx, dy,
                                                path_x, path_y, max_steps);
    if (got > 0) return got;
  }
  return 0;
}

int brainPathfinderDijkstraTracePath(BrainPathfinder *pf, int slate,
                                      int dx, int dy,
                                      int *path_x, int *path_y,
                                      int max_steps) {
  if (!pf) return 0;
  if (slate < 0 || slate >= DIJKSTRA_NUM_SLATES) return 0;
  DijkstraSlate *s = &pf->dij_slates[slate];
  if (!s->g_cost || !s->dir_at) return 0;
  if (dx < 0 || dx > 255 || dy < 0 || dy > 255) return 0;
  if (max_steps <= 0) return 0;

  int land_ni = node_idx(dx, dy, 0);
  int boat_ni = node_idx(dx, dy, 1);
  float land_g = s->g_cost[land_ni];
  float boat_g = s->g_cost[boat_ni];
  int cur = (boat_g < land_g) ? boat_ni : land_ni;
  if (s->g_cost[cur] >= COST_INF) return 0;

  int stack_x[2048];
  int stack_y[2048];
  int count = 0;

  while (count < 2048) {
    int cx = node_x(cur);
    int cy = node_y(cur);
    stack_x[count] = cx;
    stack_y[count] = cy;
    count++;

    uint8_t dval = s->dir_at[cur];
    if (dval == 0xFF) break;

    int d = dval & 0x07;
    int parent_boat = (dval & 0x08) ? 1 : 0;
    int px = cx - DX8[d];
    int py = cy - DY8[d];
    if (px < 0 || px > 255 || py < 0 || py > 255) break;

    cur = node_idx(px, py, parent_boat);
    if (s->g_cost[cur] >= COST_INF) break;
  }

  int n = count < max_steps ? count : max_steps;
  for (int i = 0; i < n; i++) {
    path_x[i] = stack_x[count - 1 - i];
    path_y[i] = stack_y[count - 1 - i];
  }
  return n;
}

int brainPathfinderDijkstraStatus(BrainPathfinder *pf, int slate,
                                   int *out_expanded, int *out_peak_open,
                                   int *out_done) {
  if (!pf || slate < 0 || slate >= DIJKSTRA_NUM_SLATES) {
    if (out_expanded)  *out_expanded = 0;
    if (out_peak_open) *out_peak_open = 0;
    if (out_done)      *out_done = 0;
    return 0;
  }
  DijkstraSlate *s = &pf->dij_slates[slate];
  if (!s->g_cost) {
    if (out_expanded)  *out_expanded = 0;
    if (out_peak_open) *out_peak_open = 0;
    if (out_done)      *out_done = 0;
    return 0;
  }
  if (out_expanded)  *out_expanded  = s->expanded;
  if (out_peak_open) *out_peak_open = s->peak_open;
  if (out_done)      *out_done      = s->done;
  return 1;
}

double brainPathfinderDijkstraFrom(BrainPathfinder *pf,
                                    int sx, int sy, int in_boat,
                                    int shells, int trees, int mines, int armour,
                                    int *out_expanded, int *out_peak_open) {
  double t0, t1;
  int src_ni, expanded = 0, peak_open = 0;

  if (out_expanded) *out_expanded = 0;
  if (out_peak_open) *out_peak_open = 0;
  if (!pf || !pf->map) return 0.0;

  /* Clamp */
  if (sx < 0) sx = 0; if (sx > 255) sx = 255;
  if (sy < 0) sy = 0; if (sy > 255) sy = 255;

  t0 = bp_now_us();

  /* Init g_cost / closed / dir_at — same as cost_to */
  {
    int i;
    for (i = 0; i < NODE_COUNT; i++) pf->g_cost[i] = COST_INF;
  }
  memset(pf->closed, 0, sizeof(pf->closed));
  memset(pf->dir_at, 0xFF, sizeof(pf->dir_at));

  src_ni = node_idx(sx, sy, in_boat);
  pf->g_cost[src_ni] = 0.0f;
  pf->shells_at[src_ni] = (int16_t)shells;
  pf->trees_at[src_ni]  = (int16_t)trees;
  pf->mines_at[src_ni]  = (int16_t)mines;
  pf->armour_at[src_ni] = (int16_t)armour;

  heap_clear(pf);
  /* No heuristic — push with f = g = 0 */
  heap_push(pf, 0.0f, (uint16_t)sx, (uint16_t)sy, (uint8_t)in_boat);
  if (pf->open_count > peak_open) peak_open = pf->open_count;

  while (pf->open_count > 0) {
    BrainPFHeapEntry entry = heap_pop(pf);
    int cx = entry.x;
    int cy = entry.y;
    int cur_boat = entry.boat;
    int ci = node_idx(cx, cy, cur_boat);
    int cur_dir, cur_shells, cur_trees, cur_mines, cur_armour;
    int d;

    /* Cooperative abort. The full-Dijkstra loop has no budget and
     * walks the whole reachable map; without this poll a brain that
     * calls dijkstra_from on a wide-open map spends its entire budget
     * here. On hit we return the partially-filled g_cost array — the
     * brain sees COST_INF for any tile we didn't reach, same as today
     * for genuinely-unreachable tiles. */
    if (BRAIN_PF_ABORTED(pf)) break;

    if (closed_test(pf->closed, ci)) continue;
    closed_set(pf->closed, ci);
    expanded++;

    cur_dir = pf->dir_at[ci];
    cur_shells = pf->shells_at[ci];
    cur_trees = pf->trees_at[ci];
    cur_mines = pf->mines_at[ci];
    cur_armour = pf->armour_at[ci];

    for (d = 0; d < 8; d++) {
      int nx = cx + DX8[d];
      int ny = cy + DY8[d];
      int ni, shells_used, trees_used, mines_used, armour_used, onBoat;
      float tc, new_g;

      if (nx < 0 || nx > 255 || ny < 0 || ny > 255) continue;

      /* Block diagonal moves through impassable corners — same rule as
       * brainPathfinderPathTo: on foot, EITHER solid cardinal corner
       * blocks the diagonal (a full-tile tank can't cut it); boats keep
       * the both-zero rule plus the water/land clip checks. This copy
       * had drifted back to the old BOTH-corners rule. */
      if (DX8[d] != 0 && DY8[d] != 0) {
        int adj_x_type = pf->map[(cy * MAP_SIZE) + (cx + DX8[d])] & 0x0F;
        int adj_y_type = pf->map[((cy + DY8[d]) * MAP_SIZE) + cx] & 0x0F;
        float speed_x = pf->terrain_speed_table[adj_x_type];
        float speed_y = pf->terrain_speed_table[adj_y_type];
        if (!cur_boat) {
          if (speed_x == 0.0f || speed_y == 0.0f) continue;
        } else {
          if (speed_x == 0.0f && speed_y == 0.0f) continue;
        }
        if (!cur_boat && (adj_x_type == TT_DEEPSEA || adj_y_type == TT_DEEPSEA)) continue;
        if (cur_boat && (!is_water_tile(adj_x_type) && adj_x_type != TT_BOAT)) continue;
        if (cur_boat && (!is_water_tile(adj_y_type) && adj_y_type != TT_BOAT)) continue;
      }

      tc = compute_cost(pf, nx, ny, cur_boat,
                         cur_shells, cur_trees, cur_mines, cur_armour,
                         &shells_used, &trees_used, &mines_used, &armour_used,
                         &onBoat);
      if (tc >= COST_INF) continue;

      ni = node_idx(nx, ny, onBoat);
      if (closed_test(pf->closed, ni)) continue;

      /* Turn penalty */
      if (cur_dir != 0xFF && cur_dir != d) {
        int tsteps = dir_steps(cur_dir, d);
        if (tsteps > 0) tc += (float)tsteps * pf->turn_cost;
      }

      new_g = pf->g_cost[ci] + tc * DMUL8[d];
      if (new_g >= pf->g_cost[ni]) continue;

      pf->g_cost[ni] = new_g;
      pf->shells_at[ni] = (int16_t)(cur_shells - shells_used);
      pf->trees_at[ni] = (int16_t)(cur_trees - trees_used);
      pf->mines_at[ni] = (int16_t)(cur_mines - mines_used);
      pf->armour_at[ni] = (int16_t)(cur_armour - armour_used);
      pf->dir_at[ni] = (uint8_t)d;

      /* Dijkstra: f = g (no heuristic) */
      heap_push(pf, new_g, (uint16_t)nx, (uint16_t)ny, (uint8_t)onBoat);
      if (pf->open_count > peak_open) peak_open = pf->open_count;
    }
  }

  t1 = bp_now_us();

  /* Invalidate the active path_to search since we've trampled g_cost. */
  pf->status = -1;
  pf->dest_x = -1;
  pf->dest_y = -1;

  if (out_expanded)  *out_expanded  = expanded;
  if (out_peak_open) *out_peak_open = peak_open;
  return t1 - t0;
}

/* â”€â”€ Incremental cost_to â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/* Reset: initialize g_cost/closed/heap from a source position.
 * Call once at the start of a replan cycle. */
void brainPathfinderCostToReset(BrainPathfinder *pf,
                                 int sx, int sy, int in_boat,
                                 int shells, int trees, int mines, int armour) {
  int i, src_ni;
  if (!pf) return;
  for (i = 0; i < NODE_COUNT; i++) pf->g_cost[i] = COST_INF;
  memset(pf->closed, 0, sizeof(pf->closed));
  memset(pf->dir_at, 0xFF, sizeof(pf->dir_at));

  if (sx < 0) sx = 0; if (sx > 255) sx = 255;
  if (sy < 0) sy = 0; if (sy > 255) sy = 255;

  src_ni = node_idx(sx, sy, in_boat);
  pf->g_cost[src_ni] = 0.0f;
  pf->shells_at[src_ni] = (int16_t)shells;
  pf->trees_at[src_ni] = (int16_t)trees;
  pf->mines_at[src_ni] = (int16_t)mines;
  pf->armour_at[src_ni] = (int16_t)armour;
  pf->dir_at[src_ni] = 0xFF;

  heap_clear(pf);
  heap_push(pf, 0.0f, (uint16_t)sx, (uint16_t)sy, (uint8_t)in_boat);
}

/* Incremental: if target is already closed, return its cost immediately.
 * Otherwise continue expanding from current state toward the target. */
float brainPathfinderCostToIncremental(BrainPathfinder *pf,
                                        int dx, int dy, int budget) {
  int dest_tile, expanded, ci;
  if (!pf || !pf->map) return COST_INF;

  if (dx < 0) dx = 0; if (dx > 255) dx = 255;
  if (dy < 0) dy = 0; if (dy > 255) dy = 255;

  dest_tile = dy * MAP_SIZE + dx;

  /* Check both land and boat layers — if already closed, return immediately */
  {
    int ni_land = dest_tile;          /* land layer */
    int ni_boat = 65536 + dest_tile;  /* boat layer */
    if (closed_test(pf->closed, ni_land) && pf->g_cost[ni_land] < COST_INF)
      return pf->g_cost[ni_land];
    if (closed_test(pf->closed, ni_boat) && pf->g_cost[ni_boat] < COST_INF)
      return pf->g_cost[ni_boat];
    /* If both closed but INF, target is truly unreachable from prior searches */
    if (closed_test(pf->closed, ni_land) && closed_test(pf->closed, ni_boat))
      return COST_INF;
  }

  /* Continue expanding from current open set toward this target */
  expanded = 0;
  while (pf->open_count > 0 && expanded < budget) {
    BrainPFHeapEntry entry = heap_pop(pf);
    int cx = entry.x;
    int cy = entry.y;
    int cur_boat = entry.boat;
    int cur_dir, cur_shells, cur_trees, cur_mines, cur_armour;
    int d;

    /* Cooperative abort. Incremental cost_to is designed to be called
     * across multiple ticks; an early break leaves the open set in a
     * resumable state — same shape as `expanded == budget`. The
     * destination check below returns COST_INF if we never reached
     * it, which the brain handles. */
    if (BRAIN_PF_ABORTED(pf)) break;

    ci = node_idx(cx, cy, cur_boat);
    if (closed_test(pf->closed, ci)) continue;
    closed_set(pf->closed, ci);
    expanded++;

    /* Destination reached? */
    if (map_idx(ci) == dest_tile)
      return pf->g_cost[ci];

    cur_dir = pf->dir_at[ci];
    cur_shells = pf->shells_at[ci];
    cur_trees = pf->trees_at[ci];
    cur_mines = pf->mines_at[ci];
    cur_armour = pf->armour_at[ci];

    for (d = 0; d < 8; d++) {
      int nx = cx + DX8[d];
      int ny = cy + DY8[d];
      int ni, shells_used, trees_used, mines_used, armour_used, onBoat;
      float tc, new_g, f;

      if (nx < 0 || nx > 255 || ny < 0 || ny > 255) continue;

      /* Diagonal corner rule — on foot EITHER solid corner blocks (see
       * brainPathfinderPathTo); boats keep the both-zero rule. This copy
       * had the old BOTH-corners rule. */
      if (DX8[d] != 0 && DY8[d] != 0) {
        int adj_x_type = pf->map[(cy * MAP_SIZE) + (cx + DX8[d])] & 0x0F;
        int adj_y_type = pf->map[((cy + DY8[d]) * MAP_SIZE) + cx] & 0x0F;
        float speed_x = pf->terrain_speed_table[adj_x_type];
        float speed_y = pf->terrain_speed_table[adj_y_type];
        if (!cur_boat) {
          if (speed_x == 0.0f || speed_y == 0.0f) continue;
        } else {
          if (speed_x == 0.0f && speed_y == 0.0f) continue;
        }
        if (!cur_boat && (adj_x_type == TT_DEEPSEA || adj_y_type == TT_DEEPSEA)) continue;
        if (cur_boat && (!is_water_tile(adj_x_type) && adj_x_type != TT_BOAT)) continue;
        if (cur_boat && (!is_water_tile(adj_y_type) && adj_y_type != TT_BOAT)) continue;
      }

      tc = compute_cost(pf, nx, ny, cur_boat,
                         cur_shells, cur_trees, cur_mines, cur_armour,
                         &shells_used, &trees_used, &mines_used, &armour_used,
                         &onBoat);
      if (tc >= COST_INF) continue;

      ni = node_idx(nx, ny, onBoat);
      if (closed_test(pf->closed, ni)) continue;

      if (cur_dir != 0xFF && cur_dir != d) {
        int tsteps = dir_steps(cur_dir, d);
        if (tsteps > 0) tc += (float)tsteps * pf->turn_cost;
      }

      new_g = pf->g_cost[ci] + tc * DMUL8[d];
      if (new_g >= pf->g_cost[ni]) continue;

      pf->g_cost[ni] = new_g;
      pf->shells_at[ni] = (int16_t)(cur_shells - shells_used);
      pf->trees_at[ni] = (int16_t)(cur_trees - trees_used);
      pf->mines_at[ni] = (int16_t)(cur_mines - mines_used);
      pf->armour_at[ni] = (int16_t)(cur_armour - armour_used);
      pf->dir_at[ni] = (uint8_t)d;

      f = new_g + heuristic(nx, ny, dx, dy);
      heap_push(pf, f, (uint16_t)nx, (uint16_t)ny, (uint8_t)onBoat);
    }
  }

  /* Check if we found it during this expansion */
  {
    int ni_land = dest_tile;
    int ni_boat = 65536 + dest_tile;
    float land_cost = closed_test(pf->closed, ni_land) ? pf->g_cost[ni_land] : COST_INF;
    float boat_cost = closed_test(pf->closed, ni_boat) ? pf->g_cost[ni_boat] : COST_INF;
    return (land_cost < boat_cost) ? land_cost : boat_cost;
  }
}

/* ------------------------------------------------------------------ */
/* Cost estimation (straight-line sample)                              */
/* ------------------------------------------------------------------ */

float brainPathfinderEstimateCost(BrainPathfinder *pf,
                                   int sx, int sy, int dx, int dy,
                                   int in_boat) {
  int dist, samples, i, idx;
  float total, t;
  int mx, my;
  int cur_boat;

  if (!pf || !pf->map) return COST_INF;

  dist = abs(dx - sx) + abs(dy - sy);
  if (dist == 0) return 0.0f;

  samples = (int)pf->estimate_samples;
  if (samples <= 0) samples = 60;
  if (dist < samples) samples = dist;

  cur_boat = in_boat;
  total = 0.0f;
  for (i = 1; i <= samples; i++) {
    t = (float)i / (float)samples;
    mx = (int)((float)sx + (float)(dx - sx) * t + 0.5f);
    my = (int)((float)sy + (float)(dy - sy) * t + 0.5f);

    /* Clamp */
    if (mx < 0) mx = 0;
    if (mx > 255) mx = 255;
    if (my < 0) my = 0;
    if (my > 255) my = 255;

    idx = my * MAP_SIZE + mx;
    {
      uint8_t raw = pf->map[idx];
      uint8_t type = raw & 0x0F;
      int onBoat = next_boat_state(cur_boat, type);
      float base = onBoat ? pf->terrain_cost_boat_table[type]
                          : pf->terrain_cost_table[type];
      float speed = pf->terrain_speed_table[type];
      float danger = (float)pf->danger_grid[idx];
      float overlay = (float)pf->overlay_grid[idx];
      float cost;

      if (onBoat && is_water_tile(type)) {
        speed = 16.0f; /* boat speed matches road speed */
      }

      if (base >= WALL_THRESHOLD) {
        total += 200.0f;
        cur_boat = onBoat;
        continue;
      }

      if (raw & 0x80) {
        base += pf->mine_penalty;
      }

      cost = base + danger * pf->danger_scale * (16.0f / fmaxf(speed, 0.1f)) + overlay;

      /* Include water drain cost in estimate */
      if (!onBoat && type == TT_RIVER && pf->water_drain_rate > 0.0f) {
        int drain = (int)pf->water_drain_rate;
        cost += (float)drain * pf->shell_loss_cost;
        cost += (float)drain * pf->mine_loss_cost;
      }

      total += cost;
      cur_boat = onBoat;
    }
  }

  /* Extrapolate if distance > samples */
  if (dist > samples) {
    float avg = total / (float)samples;
    total += avg * (float)(dist - samples);
  }

  return total;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

float brainPathfinderDangerAt(BrainPathfinder *pf, int x, int y) {
  if (!pf || x < 0 || x > 255 || y < 0 || y > 255) return 0.0f;
  return (float)pf->danger_grid[y * MAP_SIZE + x];
}

/* ------------------------------------------------------------------ */
/* Debug: trace full path                                              */
/* ------------------------------------------------------------------ */

int brainPathfinderTracePath(BrainPathfinder *pf,
                              int *path_x, int *path_y, int max_steps) {
  int cur, count, i;
  int stack_x[512], stack_y[512];

  if (!pf || pf->status != 1) return 0; /* no completed path */

  /* Find dest node in closed set. Per-node arrays now persist across
   * searches, so go through get_closed / get_parent so stale entries
   * from an earlier search don't falsely match. */
  {
    int dest_tile = pf->dest_y * MAP_SIZE + pf->dest_x;
    int dest_ni = -1;
    /* Check both land and boat layers */
    if (get_closed(pf, dest_tile)) dest_ni = dest_tile;
    else if (get_closed(pf, dest_tile + BOAT_OFFSET)) dest_ni = dest_tile + BOAT_OFFSET;
    if (dest_ni < 0) return 0;

    /* Trace parent chain from dest to src, building reversed path */
    cur = dest_ni;
    count = 0;
    while (cur >= 0 && cur < NODE_COUNT && count < 512) {
      uint32_t p;
      stack_x[count] = node_x(cur);
      stack_y[count] = node_y(cur);
      count++;
      p = get_parent(pf, cur);
      if (p == PARENT_NONE) break;
      cur = (int)p;
    }

    /* Reverse into output arrays */
    for (i = 0; i < count && i < max_steps; i++) {
      path_x[i] = stack_x[count - 1 - i];
      path_y[i] = stack_y[count - 1 - i];
    }
    return count < max_steps ? count : max_steps;
  }
}

/* Variant of brainPathfinderTracePath that takes explicit (dx, dy) and
 * walks the parent chain regardless of pf->status. Useful immediately
 * after a one-shot cost_to call: cost_to leaves the closed/parent
 * state in the current epoch but resets pf->status = -1 and
 * pf->dest_x/y = -1 so the regular trace_path fails. The parent
 * chain is still readable via the epoch-aware get_parent/get_closed
 * helpers. Returns 0 if the dest tile isn't in the closed set
 * (search didn't reach it). */
int brainPathfinderTraceLastSearchPath(BrainPathfinder *pf,
                                        int dx, int dy,
                                        int *path_x, int *path_y,
                                        int max_steps) {
  int cur, count, i;
  int stack_x[512], stack_y[512];

  if (!pf) return 0;
  if (dx < 0 || dx > 255 || dy < 0 || dy > 255) return 0;

  int dest_tile = dy * MAP_SIZE + dx;
  int dest_ni = -1;
  if (get_closed(pf, dest_tile)) dest_ni = dest_tile;
  else if (get_closed(pf, dest_tile + BOAT_OFFSET)) dest_ni = dest_tile + BOAT_OFFSET;
  if (dest_ni < 0) return 0;

  cur = dest_ni;
  count = 0;
  while (cur >= 0 && cur < NODE_COUNT && count < 512) {
    uint32_t p;
    stack_x[count] = node_x(cur);
    stack_y[count] = node_y(cur);
    count++;
    p = get_parent(pf, cur);
    if (p == PARENT_NONE) break;
    cur = (int)p;
  }

  for (i = 0; i < count && i < max_steps; i++) {
    path_x[i] = stack_x[count - 1 - i];
    path_y[i] = stack_y[count - 1 - i];
  }
  return count < max_steps ? count : max_steps;
}

/* ------------------------------------------------------------------ */
/* Front line detection (influence sign-change boundaries)              */
/* ------------------------------------------------------------------ */

int brainPathfinderFindFrontLine(BrainPathfinder *pf,
                                  int *out_x, int *out_y, int max_points) {
  int count = 0;
  int x, y;
  if (!pf || !out_x || !out_y || max_points <= 0) return 0;

  for (y = 1; y < MAP_SIZE - 1 && count < max_points; y++) {
    for (x = 1; x < MAP_SIZE - 1 && count < max_points; x++) {
      int idx = y * MAP_SIZE + x;
      int16_t val = pf->influence_grid[idx];
      if (val == 0) continue;  /* unclaimed cells aren't front line */

      /* Check 4 neighbors for sign change */
      {
        int16_t n = pf->influence_grid[(y-1) * MAP_SIZE + x];
        int16_t s = pf->influence_grid[(y+1) * MAP_SIZE + x];
        int16_t w = pf->influence_grid[y * MAP_SIZE + (x-1)];
        int16_t e = pf->influence_grid[y * MAP_SIZE + (x+1)];

        /* Front line = cell has influence AND at least one neighbor has opposite sign */
        if ((val > 0 && (n < 0 || s < 0 || w < 0 || e < 0)) ||
            (val < 0 && (n > 0 || s > 0 || w > 0 || e > 0))) {
          out_x[count] = x;
          out_y[count] = y;
          count++;
        }
      }
    }
  }
  return count;
}

/* ------------------------------------------------------------------ */
/* LGM travel-time estimation                                          */
/* ------------------------------------------------------------------ */

/* LGM man speed by terrain type (indices 0-15).
 * Matches MAP_MANSPEED_T* constants from bolo_map.h. */
static const BYTE lgm_man_speed[16] = {
  0,  /* 0  building */
  0,  /* 1  river */
  4,  /* 2  swamp */
  4,  /* 3  crater */
  16, /* 4  road */
  8,  /* 5  forest */
  4,  /* 6  rubble */
  16, /* 7  grass */
  0,  /* 8  halfbuilding */
  16, /* 9  boat */
  0,  /* 10 deepsea */
  16, /* 11 refbase */
  0,  /* 12 pillbox */
  8,  /* 13 unknown */
  0,  /* 14 unused */
  0   /* 15 unused */
};

static BYTE lgmGetBrainManSpeed(BrainPathfinder *pf, BYTE mx, BYTE my) {
  /* Enemy bases the brain stamped as impassable: the terrain type is "refbase"
   * (walkable) but the engine's mapGetManSpeed returns 0 there for a non-ally,
   * non-neutral base above capture armour. Mirror that so the LGM sim doesn't
   * march straight into an enemy base. */
  if (pf->lgm_block[my * MAP_SIZE + mx]) return 0;
  uint8_t type = pf->map[my * MAP_SIZE + mx] & 0x0F;
  return lgm_man_speed[type];
}

void brainPathfinderClearLgmBlock(BrainPathfinder *pf) {
  if (!pf) return;
  memset(pf->lgm_block, 0, sizeof(pf->lgm_block));
}

void brainPathfinderSetLgmBlock(BrainPathfinder *pf, BYTE mx, BYTE my) {
  if (!pf) return;
  pf->lgm_block[my * MAP_SIZE + mx] = 1;
}

int brainPathfinderLgmTravelTicks(BrainPathfinder *pf,
                                   WORLD sx, WORLD sy, WORLD dx, WORLD dy,
                                   BYTE blessX, BYTE blessY,
                                   int maxTicks, int stuckTicks) {
  WORLD x, y;
  BYTE localBlessX, localBlessY;
  BYTE bmx, bmy;
  int tick, sameCount;
  BYTE lastBmx, lastBmy;

  if (!pf || !pf->map) return -1;

  x = sx;
  y = sy;
  localBlessX = blessX;
  localBlessY = blessY;

  bmx = (BYTE)(x >> TANK_SHIFT_MAPSIZE);
  bmy = (BYTE)(y >> TANK_SHIFT_MAPSIZE);
  lastBmx = bmx;
  lastBmy = bmy;
  sameCount = 0;

  for (tick = 1; tick <= maxTicks; tick++) {
    BYTE speed;
    TURNTYPE angle;
    int xAdd, yAdd;
    BYTE newbmx, newbmy;
    int wasOnBlessed;

    /* Cooperative abort. -1 means "couldn't determine within budget";
     * brains already handle that as "use the straight-line estimate". */
    if (BRAIN_PF_ABORTED(pf)) return -1;

    bmx = (BYTE)(x >> TANK_SHIFT_MAPSIZE);
    bmy = (BYTE)(y >> TANK_SHIFT_MAPSIZE);
    wasOnBlessed = (localBlessX != 0 || localBlessY != 0) &&
                   bmx == localBlessX && bmy == localBlessY;

    /* Speed: blessed tile gets full speed, otherwise terrain-based */
    if (bmx == localBlessX && bmy == localBlessY) {
      speed = MAP_MANSPEED_TREFBASE;
    } else {
      speed = lgmGetBrainManSpeed(pf, bmx, bmy);
    }
    if (speed == 0) return -1;

    angle = utilCalcAngle(x, y, dx, dy);
    utilCalcDistance(&xAdd, &yAdd, angle, speed);

    /* Y movement check */
    newbmy = (BYTE)((WORLD)(y + yAdd) >> TANK_SHIFT_MAPSIZE);
    if (lgmGetBrainManSpeed(pf, bmx, newbmy) > 0 ||
        (bmx == localBlessX && newbmy == localBlessY)) {
      y = (WORLD)(y + yAdd);
    } else {
      newbmy = bmy;
    }

    /* X movement check — uses updated bmy from Y step */
    newbmx = (BYTE)((WORLD)(x + xAdd) >> TANK_SHIFT_MAPSIZE);
    bmy = (BYTE)(y >> TANK_SHIFT_MAPSIZE);
    if (lgmGetBrainManSpeed(pf, newbmx, bmy) > 0 ||
        (newbmx == localBlessX && bmy == localBlessY)) {
      x = (WORLD)(x + xAdd);
    }

    /* Clear blessed square only after LGM has been on it and moved off */
    if (wasOnBlessed) {
      if ((x >> TANK_SHIFT_MAPSIZE) != localBlessX ||
          (y >> TANK_SHIFT_MAPSIZE) != localBlessY) {
        localBlessX = 0;
        localBlessY = 0;
      }
    }

    /* Stuck detection */
    bmx = (BYTE)(x >> TANK_SHIFT_MAPSIZE);
    bmy = (BYTE)(y >> TANK_SHIFT_MAPSIZE);
    if (bmx == lastBmx && bmy == lastBmy) {
      sameCount++;
      if (sameCount >= stuckTicks) return -1;
    } else {
      sameCount = 0;
      lastBmx = bmx;
      lastBmy = bmy;
    }

    /* Arrival check */
    {
      int diffx = (int)x - (int)dx;
      int diffy = (int)y - (int)dy;
      if (diffx >= LGM_MIN_GOAL && diffx <= LGM_MAX_GOAL &&
          diffy >= LGM_MIN_GOAL && diffy <= LGM_MAX_GOAL) {
        return tick;
      }
    }
  }

  return -1;
}

int brainPathfinderLgmTravelTicksMap(BrainPathfinder *pf,
                                      BYTE smx, BYTE smy, BYTE dmx, BYTE dmy,
                                      BYTE blessX, BYTE blessY,
                                      int maxTicks, int stuckTicks) {
  WORLD sx = ((WORLD)smx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
  WORLD sy = ((WORLD)smy << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
  WORLD dx = ((WORLD)dmx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
  WORLD dy = ((WORLD)dmy << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
  return brainPathfinderLgmTravelTicks(pf, sx, sy, dx, dy,
                                        blessX, blessY, maxTicks, stuckTicks);
}

/* ------------------------------------------------------------------ */
/* Tank travel-time estimation (straight-line, tick-by-tick)            */
/* ------------------------------------------------------------------ */

/**
 * Estimate the number of game ticks for a tank to travel in a straight
 * line from map tile (sx,sy) to map tile (dx,dy).
 *
 * Simulates tick-by-tick movement in world coordinates using
 * utilCalcAngle/utilCalcDistance with the brain's terrain speed table.
 * Tracks boat state transitions (pick up boat on T_BOAT, stay in boat
 * on water, lose boat on land) matching the real game model.
 *
 * This is a rough estimate for timing overlaps — it assumes straight-
 * line travel with no obstacle avoidance. If the straight line crosses
 * an impassable tile (speed 0), returns -1.
 *
 * Uses pf->terrain_speed_table (brain's view), NOT the real game map.
 *
 * NO side effects — works on local copies of all state.
 *
 * @param pf         Pointer to BrainPathfinder (for terrain speed table
 *                   and brain's map view)
 * @param sx, sy     Start position in MAP coordinates
 * @param dx, dy     Destination position in MAP coordinates
 * @param in_boat    Whether the tank starts in a boat (0 or 1)
 * @param maxTicks   Maximum ticks to simulate before giving up
 * @param stuckTicks Ticks on same map tile before declaring stuck
 * @return           Estimated ticks to arrive, or -1 if blocked/stuck
 */
int brainPathfinderEstimateTankTravelTicks(BrainPathfinder *pf,
                                            int sx, int sy,
                                            int dx, int dy,
                                            int in_boat,
                                            int maxTicks, int stuckTicks) {
  WORLD x, y, destx, desty;
  int curBoat;
  int tick, sameCount;
  BYTE lastBmx, lastBmy;

  if (!pf || !pf->map) return -1;
  if (sx == dx && sy == dy) return 0;

  /* Convert map coords to world coords (tile centers) */
  x     = ((WORLD)sx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
  y     = ((WORLD)sy << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
  destx = ((WORLD)dx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
  desty = ((WORLD)dy << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;

  curBoat = in_boat;
  lastBmx = (BYTE)(x >> TANK_SHIFT_MAPSIZE);
  lastBmy = (BYTE)(y >> TANK_SHIFT_MAPSIZE);
  sameCount = 0;

  for (tick = 1; tick <= maxTicks; tick++) {
    BYTE bmx, bmy;
    uint8_t type;
    float fspeed;
    int speed;
    TURNTYPE angle;
    int xAdd, yAdd;

    /* Cooperative abort. -1 fits the existing "blocked/stuck" return
     * contract — brains already treat it as a non-fatal "couldn't
     * estimate" signal and either pick a different goal or wait. */
    if (BRAIN_PF_ABORTED(pf)) return -1;

    bmx  = (BYTE)(x >> TANK_SHIFT_MAPSIZE);
    bmy  = (BYTE)(y >> TANK_SHIFT_MAPSIZE);
    type = pf->map[bmy * MAP_SIZE + bmx] & 0x0F;

    /* Update boat state: pick up boat tile, stay in boat on water,
     * lose boat on land */
    curBoat = next_boat_state(curBoat, type);

    /* Get speed from terrain speed table.
     * In a boat on water, use full boat speed (16). */
    if (curBoat && is_water_tile(type)) {
      fspeed = 16.0f;
    } else {
      fspeed = pf->terrain_speed_table[type];
    }

    /* Impassable terrain — path is blocked */
    speed = (int)fspeed;
    if (speed <= 0) return -1;

    /* Calculate movement vector toward destination */
    angle = utilCalcAngle(x, y, destx, desty);
    utilCalcDistance(&xAdd, &yAdd, angle, speed);

    /* Apply movement (no collision checks — straight-line estimate) */
    x = (WORLD)(x + xAdd);
    y = (WORLD)(y + yAdd);

    /* Stuck detection: same map tile for too many consecutive ticks */
    bmx = (BYTE)(x >> TANK_SHIFT_MAPSIZE);
    bmy = (BYTE)(y >> TANK_SHIFT_MAPSIZE);
    if (bmx == lastBmx && bmy == lastBmy) {
      sameCount++;
      if (sameCount >= stuckTicks) return -1;
    } else {
      sameCount = 0;
      lastBmx = bmx;
      lastBmy = bmy;
    }

    /* Arrival: reached the destination map tile */
    if (bmx == (BYTE)dx && bmy == (BYTE)dy) {
      return tick;
    }
  }

  return -1;
}

/* Internal: walk the shell trajectory tile-by-tile from (origin, angle)
 * out for the configured shell life. Bit-identical to the engine's
 * shellsAddItem + per-tick shellsUpdate when called with the same
 * angle, sight_len, and origin the engine sees. */
static int simulate_shot_walk(WORLD origin_wx, WORLD origin_wy,
                               TURNTYPE angle,
                               int shooter_type, int sight_len,
                               BrainShotTile *out_tiles, int max_tiles) {
  if (out_tiles == NULL || max_tiles <= 0) return 0;

  /* Shell length in map units. Tank passes sightLen/2 to shellsAddItem
   * as TURNTYPE/float — float division, so odd sight_len contributes a
   * half-tile. For PILL shooter the engine passes PILLBOX_FIRE_DISTANCE
   * (8.5) directly. Keep len_units float so shellLifeTicks sees the
   * same fractional value the engine does. */
  float len_units;
  if (shooter_type == BRAIN_SHOT_SHOOTER_PILL) {
    len_units = PILLBOX_FIRE_DISTANCE;
  } else {
    int sl = (sight_len > 0) ? sight_len : GUNSIGHT_MAX;
    len_units = sl / 2.0f;
  }

  /* Spawn position + lifetime budget come from shells.c so the
   * simulator can never drift from the engine's actual shell. */
  WORLD x, y;
  shellSpawnPos(origin_wx, origin_wy, angle, &x, &y);

  int ticks = shellLifeTicks(len_units);

  /* High-precision per-tick step (24.8 fixed point), same as shellsUpdate. */
  int32_t xStep = 0, yStep = 0;
  utilCalcDistanceHP(&xStep, &yStep, angle, SHELL_SPEED);
  int32_t xAcc = 0, yAcc = 0;

  int count = 0;
  int last_mx = -1, last_my = -1;

  /* Record the origin tile (the shooter's tile) first. */
  {
    int mx = (int)((unsigned)origin_wx >> TANK_SHIFT_MAPSIZE);
    int my = (int)((unsigned)origin_wy >> TANK_SHIFT_MAPSIZE);
    out_tiles[count].mx = (uint8_t)mx;
    out_tiles[count].my = (uint8_t)my;
    out_tiles[count].hit_type = BRAIN_SHOT_HIT_TILE;
    out_tiles[count].hit_id = 0;
    last_mx = mx;
    last_my = my;
    count++;
  }

  /* Then the post-offset starting tile, if it differs. */
  {
    int mx = (int)((unsigned)x >> TANK_SHIFT_MAPSIZE);
    int my = (int)((unsigned)y >> TANK_SHIFT_MAPSIZE);
    if ((mx != last_mx || my != last_my) && count < max_tiles) {
      out_tiles[count].mx = (uint8_t)mx;
      out_tiles[count].my = (uint8_t)my;
      out_tiles[count].hit_type = BRAIN_SHOT_HIT_TILE;
      out_tiles[count].hit_id = 0;
      last_mx = mx;
      last_my = my;
      count++;
    }
  }

  for (int t = 0; t < ticks && count < max_tiles; t++) {
    xAcc += xStep;
    yAcc += yStep;
    int xMove = xAcc >> 8;
    int yMove = yAcc >> 8;
    xAcc -= xMove << 8;
    yAcc -= yMove << 8;
    x = (WORLD)((int)x + xMove);
    y = (WORLD)((int)y + yMove);

    int mx = (int)((unsigned)x >> TANK_SHIFT_MAPSIZE);
    int my = (int)((unsigned)y >> TANK_SHIFT_MAPSIZE);
    if (mx != last_mx || my != last_my) {
      out_tiles[count].mx = (uint8_t)mx;
      out_tiles[count].my = (uint8_t)my;
      out_tiles[count].hit_type = BRAIN_SHOT_HIT_TILE;
      out_tiles[count].hit_id = 0;
      last_mx = mx;
      last_my = my;
      count++;
    }
  }

  return count;
}

/* Tank-aware shot simulation. Same tile walk as simulate_shot_walk but
 * also checks for tank hitbox intersections (128 wu box, same as
 * tankIsTankHit in tank.c) at every sub-tick position. When a tank is
 * hit, a hit_type=BRAIN_SHOT_HIT_TANK entry is emitted and the walk
 * stops (shell consumed). Owner tank is excluded from hit checks. */
static int simulate_shot_walk_tanks(WORLD origin_wx, WORLD origin_wy,
                                     TURNTYPE angle,
                                     int shooter_type, int sight_len,
                                     const BrainShotTankPos *tanks, int num_tanks,
                                     uint8_t owner_player,
                                     BrainShotTile *out_tiles, int max_tiles) {
  if (out_tiles == NULL || max_tiles <= 0) return 0;

  int len_units;
  if (shooter_type == BRAIN_SHOT_SHOOTER_PILL) {
    len_units = (int)PILLBOX_FIRE_DISTANCE;
  } else {
    int sl = (sight_len > 0) ? sight_len : GUNSIGHT_MAX;
    len_units = sl / 2;
  }

  WORLD x, y;
  shellSpawnPos(origin_wx, origin_wy, angle, &x, &y);
  int ticks = shellLifeTicks(len_units);

  int32_t xStep = 0, yStep = 0;
  utilCalcDistanceHP(&xStep, &yStep, angle, SHELL_SPEED);
  int32_t xAcc = 0, yAcc = 0;

  int count = 0;
  int last_mx = -1, last_my = -1;

  /* Origin tile */
  {
    int mx = (int)((unsigned)origin_wx >> TANK_SHIFT_MAPSIZE);
    int my = (int)((unsigned)origin_wy >> TANK_SHIFT_MAPSIZE);
    out_tiles[count].mx = (uint8_t)mx;
    out_tiles[count].my = (uint8_t)my;
    out_tiles[count].hit_type = BRAIN_SHOT_HIT_TILE;
    out_tiles[count].hit_id = 0;
    last_mx = mx; last_my = my;
    count++;
  }

  /* Post-offset tile */
  {
    int mx = (int)((unsigned)x >> TANK_SHIFT_MAPSIZE);
    int my = (int)((unsigned)y >> TANK_SHIFT_MAPSIZE);
    if ((mx != last_mx || my != last_my) && count < max_tiles) {
      out_tiles[count].mx = (uint8_t)mx;
      out_tiles[count].my = (uint8_t)my;
      out_tiles[count].hit_type = BRAIN_SHOT_HIT_TILE;
      out_tiles[count].hit_id = 0;
      last_mx = mx; last_my = my;
      count++;
    }
  }

  for (int t = 0; t < ticks && count < max_tiles; t++) {
    xAcc += xStep;
    yAcc += yStep;
    int xMove = xAcc >> 8;
    int yMove = yAcc >> 8;
    xAcc -= xMove << 8;
    yAcc -= yMove << 8;
    x = (WORLD)((int)x + xMove);
    y = (WORLD)((int)y + yMove);

    /* Tank hitbox check — 128 wu box, same as tankIsTankHit */
    for (int i = 0; i < num_tanks; i++) {
      if (tanks[i].player_num == owner_player) continue;
      if (abs((int)tanks[i].wx - (int)x) < 128 &&
          abs((int)tanks[i].wy - (int)y) < 128) {
        int mx = (int)((unsigned)x >> TANK_SHIFT_MAPSIZE);
        int my = (int)((unsigned)y >> TANK_SHIFT_MAPSIZE);
        out_tiles[count].mx = (uint8_t)mx;
        out_tiles[count].my = (uint8_t)my;
        out_tiles[count].hit_type = BRAIN_SHOT_HIT_TANK;
        out_tiles[count].hit_id = tanks[i].player_num;
        count++;
        return count;  /* shell consumed by tank hit */
      }
    }

    int mx = (int)((unsigned)x >> TANK_SHIFT_MAPSIZE);
    int my = (int)((unsigned)y >> TANK_SHIFT_MAPSIZE);
    if (mx != last_mx || my != last_my) {
      out_tiles[count].mx = (uint8_t)mx;
      out_tiles[count].my = (uint8_t)my;
      out_tiles[count].hit_type = BRAIN_SHOT_HIT_TILE;
      out_tiles[count].hit_id = 0;
      last_mx = mx; last_my = my;
      count++;
    }
  }

  return count;
}

/* Public entry: derive angle from origin → target geometry. Uses
 * lroundf to avoid the truncation-by-1-brad bug; even so, the round
 * trip atan2 → integer is approximate, so use the *Angle variant
 * when bit-exact engine match matters. */
int brainPathfinderSimulateShot(WORLD origin_wx, WORLD origin_wy,
                                 WORLD target_wx, WORLD target_wy,
                                 int shooter_type, int sight_len,
                                 BrainShotTile *out_tiles, int max_tiles) {
  if (out_tiles == NULL || max_tiles <= 0) return 0;
  if (origin_wx == target_wx && origin_wy == target_wy) return 0;
  /* Angle conversion lives in shells.c so both engine and brain use
   * the same int rounding. */
  TURNTYPE angle = shellAngleFromTarget(origin_wx, origin_wy,
                                        target_wx, target_wy);
  return simulate_shot_walk(origin_wx, origin_wy, angle,
                            shooter_type, sight_len,
                            out_tiles, max_tiles);
}

/* Public entry: take the firing angle directly. Bit-exact match to a
 * real shell when called with the engine's tank.direction. */
int brainPathfinderSimulateShotAngle(WORLD origin_wx, WORLD origin_wy,
                                     float angle,
                                     int shooter_type, int sight_len,
                                     BrainShotTile *out_tiles, int max_tiles) {
  if (out_tiles == NULL || max_tiles <= 0) return 0;
  /* Wrap to [0, 256) keeping the fractional part — utilCalcDistance
   * uses a 256-entry sin/cos table internally but interpolates at
   * the call site for sub-brad accuracy. */
  float a = fmodf(fmodf(angle, 256.0f) + 256.0f, 256.0f);
  return simulate_shot_walk(origin_wx, origin_wy, (TURNTYPE)a,
                            shooter_type, sight_len,
                            out_tiles, max_tiles);
}

/* Public entry: shot simulation with tank hitbox checking. */
int brainPathfinderSimulateShotWithTanks(WORLD origin_wx, WORLD origin_wy,
                                          WORLD target_wx, WORLD target_wy,
                                          int shooter_type, int sight_len,
                                          const BrainShotTankPos *tanks, int num_tanks,
                                          uint8_t owner_player,
                                          BrainShotTile *out_tiles, int max_tiles) {
  if (out_tiles == NULL || max_tiles <= 0) return 0;
  if (origin_wx == target_wx && origin_wy == target_wy) return 0;
  TURNTYPE angle = shellAngleFromTarget(origin_wx, origin_wy,
                                        target_wx, target_wy);
  return simulate_shot_walk_tanks(origin_wx, origin_wy, angle,
                                  shooter_type, sight_len,
                                  tanks, num_tanks, owner_player,
                                  out_tiles, max_tiles);
}
