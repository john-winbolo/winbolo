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
 *Name:          Brain World Simulator
 *Filename:      brain_worldsim.h
 *Author:        John Morrison
 *Purpose:
 *  C-accelerated forward state simulator for Lua brains.
 *  Predicts armor damage, pill anger escalation, and LGM
 *  survival along a candidate path. Each brain instance
 *  gets its own BrainWorldSim with isolated state.
 *  No per-run malloc — all arrays are pre-allocated.
 *********************************************************/

#ifndef BRAIN_WORLDSIM_H
#define BRAIN_WORLDSIM_H

#include "global.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WSIM_MAX_PILLS   16
#define WSIM_MAX_TANKS    8
#define WSIM_MAX_PATH   256

typedef struct {
  uint8_t  mx, my;
  uint8_t  health;          /* 0-15 */
  uint8_t  owner;           /* player id, 0xFF = neutral */
  uint8_t  pill_id;         /* original pill index for result mapping */
  uint8_t  speed;           /* current fire interval (6..100 ticks) */
  uint8_t  cooldown;        /* ticks of speed-halving chain remaining */
  uint8_t  just_seen;       /* first-sight delay flag */
  uint16_t reload;          /* ticks since last shot (fires when reload >= speed) */
  uint16_t shots_at_us;     /* output: shots fired at our tank */
} WSimPill;

typedef struct {
  int32_t  wx, wy;          /* world coordinates (0..65535) */
  uint8_t  direction;       /* bolo angle 0-255 */
  uint8_t  speed;           /* WU/tick */
  uint8_t  is_ours;
  uint8_t  owner;
  int16_t  armour;          /* signed for death detection */
  float    slide_vx;        /* knockback X velocity (WU/tick) */
  float    slide_vy;        /* knockback Y velocity (WU/tick) */
} WSimTank;

typedef struct {
  uint8_t mx, my;
} WSimPathPoint;

typedef struct {
  int32_t  wx, wy;          /* current world position (0..65535) */
  int32_t  dest_wx, dest_wy;/* destination world position */
  int16_t  dispatch_tick;   /* sim tick when LGM is spawned from tank */
  uint8_t  active;          /* 0=not dispatched yet, 1=walking, 2=arrived, 3=dead */
  uint8_t  speed;           /* WU/tick (~4) */
} WSimLGM;

#define WSIM_MAX_HITS 64

typedef struct {
  int16_t  mx, my;            /* map tile where hit occurred */
  int16_t  armour_after;      /* armour remaining after this hit */
  int16_t  tick;              /* sim tick of the hit */
  uint8_t  pill_idx;          /* which pill fired (index into sim pills) */
} WSimHitRecord;

typedef struct {
  int16_t  armour_remaining;
  int16_t  damage_taken;
  int16_t  ticks_simulated;
  int16_t  arrival_tick;      /* when tank reached destination, or -1 */
  uint8_t  killed;            /* did armor reach 0? */
  uint8_t  lgm_survived;     /* 0=not dispatched, 1=survived, 2=killed */
  int16_t  lgm_arrival_tick;  /* when LGM reached dest, or -1 */
  int16_t  lgm_death_tick;    /* when LGM died, or -1 */
  uint16_t pill_shots[WSIM_MAX_PILLS];
  uint8_t  pill_final_health[WSIM_MAX_PILLS];
  uint8_t  pill_final_speed[WSIM_MAX_PILLS];
  /* Per-hit position log */
  WSimHitRecord hits[WSIM_MAX_HITS];
  int16_t  num_hits;
} WSimResult;

typedef struct BrainWorldSim {
  const BYTE *map;
  float terrain_speed[16];

  WSimPill      pills[WSIM_MAX_PILLS];
  int           num_pills;
  WSimTank      tanks[WSIM_MAX_TANKS];
  int           num_tanks;
  int           our_tank_idx;
  WSimPathPoint path[WSIM_MAX_PATH];
  int           num_path;
  WSimLGM       lgm;

  int           attack_target;       /* pill index we're shooting, or -1 */
  int           tank_shoot_interval; /* ticks between our shots (default 8) */
  int           shell_damage;        /* armor per hit (default 5) */

  /* Cooperative abort flag (SDL_AtomicInt *). Polled at the per-tick
   * checkpoint in brainWorldSimRun. NULL disables polling. void * so
   * this header doesn't pull in SDL3 — the .c file casts on read. */
  void *abort_flag;
} BrainWorldSim;

/*********************************************************
 * Init (call once before any parallel use)
 *********************************************************/
void wsim_init_tables(void);

/*********************************************************
 * Lifecycle
 *********************************************************/
BrainWorldSim *brainWorldSimCreate(void);
void brainWorldSimDestroy(BrainWorldSim *sim);

/*********************************************************
 * Setup (call before each run)
 *********************************************************/
void brainWorldSimClear(BrainWorldSim *sim);
void brainWorldSimSetMap(BrainWorldSim *sim, const BYTE *map);
void brainWorldSimSetTerrainSpeed(BrainWorldSim *sim, int type, float speed);

/* Set the cooperative abort flag the per-tick sim loop polls.
 * `flag` is an SDL_AtomicInt * (void * here so callers without SDL
 * available don't need to depend on it). NULL disables polling. */
void brainWorldSimSetAbortFlag(BrainWorldSim *sim, void *flag);

void brainWorldSimAddPill(BrainWorldSim *sim, int mx, int my,
                          int health, float anger, int owner, int pill_id);
void brainWorldSimAddTank(BrainWorldSim *sim, int wx, int wy,
                          int dir, int speed, int is_ours,
                          int owner, int armour);
void brainWorldSimSetPath(BrainWorldSim *sim,
                          const WSimPathPoint *pts, int count);
void brainWorldSimSetAttackTarget(BrainWorldSim *sim, int pill_index);
void brainWorldSimSetLGM(BrainWorldSim *sim, int dispatch_tick,
                         int dest_mx, int dest_my, int speed);

/*********************************************************
 * Run simulation
 *********************************************************/
WSimResult brainWorldSimRun(BrainWorldSim *sim, int max_ticks);

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_WORLDSIM_H */
