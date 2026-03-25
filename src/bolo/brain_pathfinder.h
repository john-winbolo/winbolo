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
 *Filename:      brain_pathfinder.h
 *Author:        John Morrison
 *Purpose:
 *  C-accelerated A* pathfinder for Lua brains.
 *  Each brain instance gets its own BrainPathfinder
 *  with isolated state (danger grid, config, A* working
 *  memory). No per-search malloc — all arrays are
 *  pre-allocated in the struct.
 *********************************************************/

#ifndef BRAIN_PATHFINDER_H
#define BRAIN_PATHFINDER_H

#include "global.h"
#include "brain.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A* open-set heap entry */
typedef struct {
  float f;
  uint16_t x;
  uint16_t y;
  uint8_t boat;   /* 0 = on foot, 1 = in boat */
  uint8_t _pad;
} BrainPFHeapEntry;

/* Per-instance pathfinder state */
typedef struct {
  /* Terrain map pointer (set each tick, not owned) */
  const BYTE *map;

  /* Spatial grids */
  uint16_t danger_grid[65536];   /* pill danger values */
  int16_t  overlay_grid[65536];  /* modder-extensible custom cost layer */
  int16_t  influence_grid[65536]; /* territorial influence: +friendly, -hostile */

  /* Per-terrain-type tables (indexed 0..15) */
  float terrain_cost_table[16];      /* land mode costs */
  float terrain_cost_boat_table[16]; /* boat mode costs */
  float terrain_speed_table[16];

  /* Config scalars */
  float turn_cost;
  float wall_shoot_cost;
  float wall_shoot_shells;
  float shell_reserve;
  float road_build_cost;
  float tree_reserve;
  float mine_penalty;
  float estimate_samples;

  /* Resource drain config */
  float water_drain_rate;    /* shells+mines lost per river tile on foot (default 6) */
  float shell_loss_cost;     /* A* cost per shell lost to water drain (default 3) */
  float mine_loss_cost;      /* A* cost per mine lost to water drain (default 2) */
  float armour_drain_rate;   /* armour lost per unit of danger-exposure (default 0.02) */
  float min_shells;          /* prune paths arriving with fewer shells (default 0) */
  float min_mines;           /* prune paths arriving with fewer mines (default 0) */
  float min_armour;          /* prune paths arriving with less armour (default 0) */

  /* A* working state — doubled for boat/land state pairs.
   * Node index = (boat ? 65536 : 0) + y*256 + x
   * NODE_COUNT = 131072 = 256*256*2 */
  float    g_cost[131072];
  uint32_t parent[131072];
  uint8_t  closed[16384];      /* bitset: 131072 / 8 */
  int16_t  shells_at[131072];
  int16_t  trees_at[131072];
  int16_t  mines_at[131072];
  int16_t  armour_at[131072];
  uint8_t  dir_at[131072];

  /* Binary min-heap */
  BrainPFHeapEntry *heap;
  int heap_capacity;
  int open_count;

  /* Search metadata */
  int status;   /* 0=running, 1=done, -1=failed */
  int dest_x, dest_y;
  int src_x, src_y;
  int next_x, next_y;
  int age;
  int in_boat;
} BrainPathfinder;

/*********************************************************
 * Public API
 *********************************************************/

BrainPathfinder *brainPathfinderCreate(void);
void brainPathfinderDestroy(BrainPathfinder *pf);
void brainPathfinderSetMap(BrainPathfinder *pf, const BYTE *map);

/* Configuration */
void brainPathfinderSetTerrainCost(BrainPathfinder *pf, int type, float cost);
void brainPathfinderSetBoatCost(BrainPathfinder *pf, int type, float cost);
void brainPathfinderSetTerrainSpeed(BrainPathfinder *pf, int type, float speed);
void brainPathfinderSetConfig(BrainPathfinder *pf, const char *key, float value);

/* Danger overlay */
void brainPathfinderClearDanger(BrainPathfinder *pf);
/* Stamp pill danger in a Manhattan-distance circle.
 * Formula: penalty = (base_danger + anger) * proximity_falloff
 * base_danger: base pill danger (e.g. PILL_DANGER_BASE = 15)
 * anger:       additive anger bonus (e.g. PILL_DANGER_ANGER * anger_value)
 * Multiple stamps accumulate (stack). */
void brainPathfinderStampPill(BrainPathfinder *pf, int cx, int cy,
                               int radius, float base_danger, float anger);
void brainPathfinderSetDanger(BrainPathfinder *pf, int x, int y, float value);

/* Influence grid (territorial control: positive=friendly, negative=hostile) */
void brainPathfinderClearInfluence(BrainPathfinder *pf);
void brainPathfinderStampInfluence(BrainPathfinder *pf, int cx, int cy,
                                    int radius, int strength);
int16_t brainPathfinderInfluenceAt(BrainPathfinder *pf, int x, int y);

/* Custom overlay (modder extension point) */
void brainPathfinderSetOverlay(BrainPathfinder *pf, int x, int y, float value);
void brainPathfinderClearOverlay(BrainPathfinder *pf);

/* Pathfinding — returns: 0=running, 1=done, -1=failed */
int brainPathfinderPathTo(BrainPathfinder *pf,
                           int sx, int sy, int dx, int dy,
                           int in_boat, int shells, int trees,
                           int mines, int armour,
                           int budget,
                           int *next_x, int *next_y);

/* One-shot A* cost query (clobbers main search state) */
float brainPathfinderCostTo(BrainPathfinder *pf,
                             int sx, int sy, int dx, int dy,
                             int in_boat, int shells, int trees,
                             int mines, int armour, int budget);

/* Cost estimation (straight-line sample) */
float brainPathfinderEstimateCost(BrainPathfinder *pf,
                                   int sx, int sy, int dx, int dy, int in_boat);

/* Query */
float brainPathfinderDangerAt(BrainPathfinder *pf, int x, int y);

/* LGM travel-time estimation */
int brainPathfinderLgmTravelTicks(BrainPathfinder *pf,
                                   WORLD sx, WORLD sy, WORLD dx, WORLD dy,
                                   BYTE blessX, BYTE blessY,
                                   int maxTicks, int stuckTicks);
int brainPathfinderLgmTravelTicksMap(BrainPathfinder *pf,
                                      BYTE smx, BYTE smy, BYTE dmx, BYTE dmy,
                                      BYTE blessX, BYTE blessY,
                                      int maxTicks, int stuckTicks);

/* Tank travel-time estimation (straight-line, tick-by-tick simulation) */
int brainPathfinderEstimateTankTravelTicks(BrainPathfinder *pf,
                                            int sx, int sy,
                                            int dx, int dy,
                                            int in_boat,
                                            int maxTicks, int stuckTicks);

/* Debug: trace full path after a completed search.
 * Fills path_x/path_y arrays, returns number of steps (0 if no path). */
int brainPathfinderTracePath(BrainPathfinder *pf,
                              int *path_x, int *path_y, int max_steps);

/* Scan influence grid for front-line cells (where positive/negative neighbors meet).
 * Writes up to max_points pairs into out_x[], out_y[].
 * Skips cells where both values are 0 (unclaimed vs unclaimed).
 * Returns number of front-line points found. */
int brainPathfinderFindFrontLine(BrainPathfinder *pf,
                                  int *out_x, int *out_y, int max_points);

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_PATHFINDER_H */
