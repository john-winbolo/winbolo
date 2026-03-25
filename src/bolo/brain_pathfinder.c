/*
 * Copyright (c) 1998-2008 John Morrison.
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
#include <math.h>
#include <float.h>

#include "brain_pathfinder.h"
#include "util.h"
#include "lgm.h"
#include "tank.h"

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
/* Heap operations (array-based binary min-heap)                       */
/* ------------------------------------------------------------------ */

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

  /* Sift up */
  while (i > 0) {
    parent = (i - 1) >> 1;
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
  int i, child;

  if (n <= 0) return result;

  h[0] = h[n];

  /* Sift down */
  i = 0;
  for (;;) {
    child = 2 * i + 1;
    if (child >= n) break;
    if (child + 1 < n && h[child + 1].f < h[child].f) {
      child++;
    }
    if (h[i].f <= h[child].f) break;
    BrainPFHeapEntry tmp = h[child];
    h[child] = h[i];
    h[i] = tmp;
    i = child;
  }

  return result;
}

/* ------------------------------------------------------------------ */
/* Closed bitset operations                                            */
/* ------------------------------------------------------------------ */

static inline void closed_set(uint8_t *closed, int idx) {
  closed[idx >> 3] |= (1u << (idx & 7));
}

static inline int closed_test(const uint8_t *closed, int idx) {
  return (closed[idx >> 3] >> (idx & 7)) & 1;
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

  /* Resource drain config */
  pf->water_drain_rate = 6.0f;     /* ~85 ticks/tile at speed 3, drain every 15 = ~5.7 */
  pf->shell_loss_cost = 3.0f;      /* A* cost per shell lost to water */
  pf->mine_loss_cost = 2.0f;       /* A* cost per mine lost to water */
  pf->armour_drain_rate = 0.02f;   /* armour lost per danger-exposure unit */
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

  /* Default terrain speeds (matching constants.lua TERRAIN_SPEED) */
  pf->terrain_speed_table[0]  = 0.0f;   /* building */
  pf->terrain_speed_table[1]  = 3.0f;   /* river */
  pf->terrain_speed_table[2]  = 3.0f;   /* swamp */
  pf->terrain_speed_table[3]  = 3.0f;   /* crater */
  pf->terrain_speed_table[4]  = 16.0f;  /* road */
  pf->terrain_speed_table[5]  = 6.0f;   /* forest */
  pf->terrain_speed_table[6]  = 3.0f;   /* rubble */
  pf->terrain_speed_table[7]  = 12.0f;  /* grass */
  pf->terrain_speed_table[8]  = 0.0f;   /* halfbuild */
  pf->terrain_speed_table[9]  = 16.0f;  /* boat */
  pf->terrain_speed_table[10] = 3.0f;   /* deepsea */
  pf->terrain_speed_table[11] = 16.0f;  /* refbase */
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
  free(pf);
}

void brainPathfinderSetMap(BrainPathfinder *pf, const BYTE *map) {
  if (pf) pf->map = map;
}

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
  else if (strcmp(key, "water_drain_rate") == 0)  pf->water_drain_rate = value;
  else if (strcmp(key, "shell_loss_cost") == 0)   pf->shell_loss_cost = value;
  else if (strcmp(key, "mine_loss_cost") == 0)    pf->mine_loss_cost = value;
  else if (strcmp(key, "armour_drain_rate") == 0) pf->armour_drain_rate = value;
  else if (strcmp(key, "min_shells") == 0)        pf->min_shells = value;
  else if (strcmp(key, "min_mines") == 0)         pf->min_mines = value;
  else if (strcmp(key, "min_armour") == 0)        pf->min_armour = value;
}

/* ------------------------------------------------------------------ */
/* Danger overlay                                                      */
/* ------------------------------------------------------------------ */

void brainPathfinderClearDanger(BrainPathfinder *pf) {
  if (pf) memset(pf->danger_grid, 0, sizeof(pf->danger_grid));
}

void brainPathfinderStampPill(BrainPathfinder *pf, int cx, int cy,
                               int radius, float base_danger, float anger) {
  int dx, dy, nx, ny;
  float proximity, penalty;
  int idx;
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
      penalty = (base_danger + anger) * proximity;

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
  }
}

/* ------------------------------------------------------------------ */
/* Custom overlay                                                      */
/* ------------------------------------------------------------------ */

void brainPathfinderSetOverlay(BrainPathfinder *pf, int x, int y, float value) {
  if (pf && x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE) {
    int v = (int)value;
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    pf->overlay_grid[y * MAP_SIZE + x] = (int16_t)v;
  }
}

void brainPathfinderClearOverlay(BrainPathfinder *pf) {
  if (pf) memset(pf->overlay_grid, 0, sizeof(pf->overlay_grid));
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
        *shells_used = wall_shoot_shells;
        return pf->wall_shoot_cost;
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
  danger = (float)pf->danger_grid[midx];
  speed = pf->terrain_speed_table[type];
  if (onBoat && is_water_tile(type)) {
    speed = 16.0f; /* boat speed matches road speed */
  }
  overlay = (float)pf->overlay_grid[midx];

  cost = base + danger * (16.0f / fmaxf(speed, 0.1f)) + overlay;

  /* Water resource drain: on foot in river, tank loses shells and mines.
   * Game mechanic: every TANK_WATER_TIME (15) ticks at speed <= 3,
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

  /* Road-building: only when on foot (can't build from a boat) */
  if (!onBoat && trees_left >= 2) {
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

  /* Check if we can resume an existing search */
  if (pf->status == 0 && pf->dest_x == dx && pf->dest_y == dy
      && pf->src_x == sx && pf->src_y == sy
      && pf->in_boat == in_boat) {
    /* Resume existing search */
    goto do_search;
  }

  /* Start fresh search */
  {
    int i;
    for (i = 0; i < NODE_COUNT; i++) {
      pf->g_cost[i] = COST_INF;
    }
  }
  memset(pf->closed, 0, sizeof(pf->closed));
  memset(pf->shells_at, 0, sizeof(pf->shells_at));
  memset(pf->trees_at, 0, sizeof(pf->trees_at));
  memset(pf->mines_at, 0, sizeof(pf->mines_at));
  memset(pf->armour_at, 0, sizeof(pf->armour_at));
  memset(pf->dir_at, 0xFF, sizeof(pf->dir_at));    /* 0xFF = no direction */
  memset(pf->parent, 0xFF, sizeof(pf->parent));     /* PARENT_NONE */

  pf->status = 0;
  pf->dest_x = dx;
  pf->dest_y = dy;
  pf->src_x = sx;
  pf->src_y = sy;
  pf->next_x = -1;
  pf->next_y = -1;
  pf->age = 0;
  pf->in_boat = in_boat;

  src_ni = node_idx(sx, sy, in_boat);
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

    if (closed_test(pf->closed, ci)) continue;
    closed_set(pf->closed, ci);
    expanded++;

    /* Destination reached (in either boat state) */
    if (map_idx(ci) == dest_tile) {
      /* Trace parent chain to extract first step */
      int cur = ci;
      int prev = -1;
      int steps = 0;
      while (cur >= 0 && cur < NODE_COUNT && steps++ < NODE_COUNT) {
        uint32_t p = pf->parent[cur];
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

      /* Block diagonal moves through impassable corners:
       * If both adjacent cardinal tiles have speed 0, diagonal is blocked.
       * Also block if either cardinal neighbor is deep sea and we're not
       * in a boat — the tank would clip through the deep sea tile and die.
       * In a boat, block diagonals that clip through land — the game
       * physics would touch the land tile and disembark the tank. */
      if (DX8[d] != 0 && DY8[d] != 0) {
        int adj_x_type = pf->map[(cy * MAP_SIZE) + (cx + DX8[d])] & 0x0F;
        int adj_y_type = pf->map[((cy + DY8[d]) * MAP_SIZE) + cx] & 0x0F;
        float speed_x = pf->terrain_speed_table[adj_x_type];
        float speed_y = pf->terrain_speed_table[adj_y_type];
        if (speed_x == 0.0f && speed_y == 0.0f) continue;
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

      ni = node_idx(nx, ny, onBoat);
      if (closed_test(pf->closed, ni)) continue;

      /* Turn penalty */
      if (cur_dir != 0xFF && cur_dir != d) {
        int tsteps = dir_steps(cur_dir, d);
        if (tsteps > 0) {
          tc += (float)tsteps * pf->turn_cost;
        }
      }

      new_g = pf->g_cost[ci] + tc * DMUL8[d];
      if (new_g >= pf->g_cost[ni]) continue;

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
    /* No path found — find closest explored node as partial result */
    int best_ni = -1;
    int best_h = 0x7FFFFFFF;
    int i;
    for (i = 0; i < NODE_COUNT; i++) {
      if (closed_test(pf->closed, i)) {
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
        uint32_t p = pf->parent[cur];
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
float brainPathfinderCostTo(BrainPathfinder *pf,
                             int sx, int sy, int dx, int dy,
                             int in_boat, int shells, int trees,
                             int mines, int armour, int budget) {
  int src_ni, dest_tile, expanded;
  float result;

  if (!pf || !pf->map) return COST_INF;

  /* Clamp */
  if (sx < 0) sx = 0; if (sx > 255) sx = 255;
  if (sy < 0) sy = 0; if (sy > 255) sy = 255;
  if (dx < 0) dx = 0; if (dx > 255) dx = 255;
  if (dy < 0) dy = 0; if (dy > 255) dy = 255;

  if (sx == dx && sy == dy) return 0.0f;

  dest_tile = dy * MAP_SIZE + dx;

  /* Init g_cost and closed */
  {
    int i;
    for (i = 0; i < NODE_COUNT; i++) {
      pf->g_cost[i] = COST_INF;
    }
  }
  memset(pf->closed, 0, sizeof(pf->closed));
  memset(pf->dir_at, 0xFF, sizeof(pf->dir_at));

  src_ni = node_idx(sx, sy, in_boat);
  pf->g_cost[src_ni] = 0.0f;

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

    if (closed_test(pf->closed, ci)) continue;
    closed_set(pf->closed, ci);
    expanded++;

    /* Destination reached */
    if (map_idx(ci) == dest_tile) {
      result = pf->g_cost[ci];
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

      if (nx < 0 || nx > 255 || ny < 0 || ny > 255) continue;

      /* Block diagonal moves through impassable corners */
      if (DX8[d] != 0 && DY8[d] != 0) {
        int adj_x_type = pf->map[(cy * MAP_SIZE) + (cx + DX8[d])] & 0x0F;
        int adj_y_type = pf->map[((cy + DY8[d]) * MAP_SIZE) + cx] & 0x0F;
        float speed_x = pf->terrain_speed_table[adj_x_type];
        float speed_y = pf->terrain_speed_table[adj_y_type];
        if (speed_x == 0.0f && speed_y == 0.0f) continue;
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
        if (tsteps > 0) {
          tc += (float)tsteps * pf->turn_cost;
        }
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

  /* Invalidate main search so next path_to() starts fresh */
  pf->status = -1;
  pf->dest_x = -1;
  pf->dest_y = -1;

  return result;
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

      cost = base + danger * (16.0f / fmaxf(speed, 0.1f)) + overlay;

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

  /* Find dest node in closed set */
  {
    int dest_tile = pf->dest_y * MAP_SIZE + pf->dest_x;
    int dest_ni = -1;
    /* Check both land and boat layers */
    if (closed_test(pf->closed, dest_tile)) dest_ni = dest_tile;
    else if (closed_test(pf->closed, dest_tile + BOAT_OFFSET)) dest_ni = dest_tile + BOAT_OFFSET;
    if (dest_ni < 0) return 0;

    /* Trace parent chain from dest to src, building reversed path */
    cur = dest_ni;
    count = 0;
    while (cur >= 0 && cur < NODE_COUNT && count < 512) {
      stack_x[count] = node_x(cur);
      stack_y[count] = node_y(cur);
      count++;
      if (pf->parent[cur] == PARENT_NONE) break;
      cur = (int)pf->parent[cur];
    }

    /* Reverse into output arrays */
    for (i = 0; i < count && i < max_steps; i++) {
      path_x[i] = stack_x[count - 1 - i];
      path_y[i] = stack_y[count - 1 - i];
    }
    return count < max_steps ? count : max_steps;
  }
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
  uint8_t type = pf->map[my * MAP_SIZE + mx] & 0x0F;
  return lgm_man_speed[type];
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
