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
 *Name:          Brain World Simulator
 *Filename:      brain_worldsim.c
 *Author:        John Morrison
 *Purpose:
 *  Forward state simulator: predicts armor damage, pill
 *  anger escalation, crossfire, knockback, and LGM
 *  survival along a candidate path.
 *********************************************************/

#include "brain_worldsim.h"
#include "sim_rules.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL3/SDL.h>

/* Game constants matching tank.h / global.h */
#define WSIM_SLIDE_INITIAL_SPEED 26.0f  /* WU/tick initial knockback speed */
#define WSIM_SLIDE_FRICTION      0.80f /* velocity multiplier per tick */
#define WSIM_SLIDE_STOP_THRESH   0.5f  /* stop sliding below this speed */
#define WSIM_PILL_FOREST_RANGE 768 /* 3 tiles — pills can't see into forest beyond this */
#define WSIM_LGM_ARRIVE_DIST 32   /* WU threshold for LGM arrival */
#define WSIM_WU_PER_TILE    256

/* Bolo direction table (256 entries): dx, dy per unit.
 * Direction 0 = North (dy=-1), 64 = East (dx=1), etc.
 * We use a small lookup for sin/cos of bolo angles. */
static float wsim_sin_table[256];
static float wsim_cos_table[256];
static int wsim_tables_initialized = 0;

void wsim_init_tables(void) {
  if (wsim_tables_initialized) return;
  for (int i = 0; i < 256; i++) {
    double angle = (double)i * (2.0 * 3.14159265358979323846 / 256.0);
    wsim_sin_table[i] = (float)sin(angle);
    wsim_cos_table[i] = (float)-cos(angle);  /* bolo: 0=North=up=-Y */
  }
  wsim_tables_initialized = 1;
}

/* Distance squared between two world-coordinate points */
static int wsim_dist_sq(int x1, int y1, int x2, int y2) {
  int dx = x1 - x2;
  int dy = y1 - y2;
  return dx * dx + dy * dy;
}

/* Map tile lookup from world coordinates */
static int wsim_terrain_at(const BYTE *map, int wx, int wy) {
  int mx, my;
  if (!map) return 0;
  mx = wx >> 8;
  my = wy >> 8;
  if (mx < 0) mx = 0;
  if (mx > 255) mx = 255;
  if (my < 0) my = 0;
  if (my > 255) my = 255;
  return map[(my << 8) | mx] & 0x0F;
}

/* Check if terrain is forest */
static int wsim_is_forest(const BYTE *map, int wx, int wy) {
  return wsim_terrain_at(map, wx, wy) == 5; /* T_FOREST */
}

/* Bresenham line check: does the line from (ax,ay) to (bx,by) pass through
 * any alive pill's tile (other than the shooter)? Returns pill index or -1. */
static int wsim_line_hits_pill(BrainWorldSim *sim, int ax, int ay,
                               int bx, int by, int shooter_idx) {
  int amx = ax >> 8, amy = ay >> 8;
  int bmx = bx >> 8, bmy = by >> 8;
  int dx = abs(bmx - amx), dy = abs(bmy - amy);
  int sx = amx < bmx ? 1 : -1;
  int sy = amy < bmy ? 1 : -1;
  int err = dx - dy;
  int cx = amx, cy = amy;

  /* Skip the starting tile (shooter's tile) */
  while (cx != bmx || cy != bmy) {
    int e2 = err * 2;
    if (e2 > -dy) { err -= dy; cx += sx; }
    if (e2 <  dx) { err += dx; cy += sy; }

    /* Check if any pill occupies this tile */
    for (int i = 0; i < sim->num_pills; i++) {
      if (i == shooter_idx) continue;
      if (sim->pills[i].health > 0 &&
          sim->pills[i].mx == (uint8_t)cx &&
          sim->pills[i].my == (uint8_t)cy) {
        return i;
      }
    }
  }
  return -1;
}

/* Anger escalation: halve fire interval, start cooldown chain */
/* One shell hits a pill. pill_hit_damage is 1 under classic rules, which
   makes this the old health-- exactly. */
static void wsim_hit_pill(const BrainWorldSim *sim, WSimPill *pill) {
  if (pill->health > sim->pill_hit_damage) {
    pill->health = (uint8_t)(pill->health - sim->pill_hit_damage);
  } else {
    pill->health = 0;
  }
}

static void wsim_anger_pill(const BrainWorldSim *sim, WSimPill *pill) {
  if (pill->speed > sim->pill_attack_min_ticks) {
    pill->speed = (uint8_t)(pill->speed / 2);
    if (pill->speed < sim->pill_attack_min_ticks)
      pill->speed = (uint8_t)sim->pill_attack_min_ticks;
  }
  pill->cooldown = (uint8_t)sim->pill_cooldown_ticks;
}

/* Apply knockback slide to a tank for one tick (exponential decay) */
static void wsim_apply_slide(BrainWorldSim *sim, WSimTank *tank) {
  int dx, dy;
  int new_wx, new_wy;
  int terrain;
  float spd;

  if (tank->slide_vx == 0.0f && tank->slide_vy == 0.0f) return;

  dx = (int)roundf(tank->slide_vx);
  dy = (int)roundf(tank->slide_vy);

  /* Check X axis */
  new_wx = tank->wx + dx;
  if (new_wx < 0) new_wx = 0;
  if (new_wx > 65535) new_wx = 65535;
  terrain = wsim_terrain_at(sim->map, new_wx, tank->wy);
  spd = sim->terrain_speed[terrain];
  if (spd > 0) {
    tank->wx = new_wx;
  } else {
    tank->slide_vx = 0.0f;
  }

  /* Check Y axis */
  new_wy = tank->wy + dy;
  if (new_wy < 0) new_wy = 0;
  if (new_wy > 65535) new_wy = 65535;
  terrain = wsim_terrain_at(sim->map, tank->wx, new_wy);
  spd = sim->terrain_speed[terrain];
  if (spd > 0) {
    tank->wy = new_wy;
  } else {
    tank->slide_vy = 0.0f;
  }

  /* Friction decay */
  tank->slide_vx *= WSIM_SLIDE_FRICTION;
  tank->slide_vy *= WSIM_SLIDE_FRICTION;

  /* Stop when below threshold */
  if (fabsf(tank->slide_vx) < WSIM_SLIDE_STOP_THRESH &&
      fabsf(tank->slide_vy) < WSIM_SLIDE_STOP_THRESH) {
    tank->slide_vx = 0.0f;
    tank->slide_vy = 0.0f;
  }
}

BrainWorldSim *brainWorldSimCreate(void) {
  BrainWorldSim *sim = (BrainWorldSim *)calloc(1, sizeof(BrainWorldSim));
  SimRules classic;
  if (!sim) return NULL;
  wsim_init_tables();
  sim->attack_target = -1;
  sim->tank_shoot_interval = 8;
  sim->tank_shoot_interval_base = 8;
  sim->pill_hit_damage = 1;
  sim->forest_range = WSIM_PILL_FOREST_RANGE;
  sim->dwell_ticks = 0;

  /* The shell damage, the four pill numbers and the ten terrain speeds a rule
     names come from the classic rules table, so the forward model starts on
     the same numbers the engine does; a brain that pushes its own through
     wsim_set_terrain_speed overwrites the speeds. */
  simRulesClassic(&classic);
  sim->shell_damage          = classic.shell_damage;
  sim->pill_range            = classic.pill_range;
  sim->pill_attack_ticks     = classic.pill_attack_ticks;
  sim->pill_attack_min_ticks = classic.pill_attack_min_ticks;
  sim->pill_cooldown_ticks   = classic.pill_cooldown_ticks;
  sim->terrain_speed[0]  =  0; /* BUILDING */
  sim->terrain_speed[1]  = (float) classic.speed_river;
  sim->terrain_speed[2]  = (float) classic.speed_swamp;
  sim->terrain_speed[3]  = (float) classic.speed_crater;
  sim->terrain_speed[4]  = (float) classic.speed_road;
  sim->terrain_speed[5]  = (float) classic.speed_forest;
  sim->terrain_speed[6]  = (float) classic.speed_rubble;
  sim->terrain_speed[7]  = (float) classic.speed_grass;
  sim->terrain_speed[8]  =  0; /* HALFBUILD */
  sim->terrain_speed[9]  = (float) classic.speed_boat;
  sim->terrain_speed[10] = (float) classic.speed_deep_sea;
  sim->terrain_speed[11] = (float) classic.speed_refuel_base;
  sim->terrain_speed[12] = 16; /* PILLBOX */
  sim->terrain_speed[13] =  0;
  sim->terrain_speed[14] =  0;
  sim->terrain_speed[15] =  0;

  return sim;
}

void brainWorldSimDestroy(BrainWorldSim *sim) {
  free(sim);
}

void brainWorldSimClear(BrainWorldSim *sim) {
  const BYTE *saved_map = sim->map;
  float saved_speed[16];
  /* The rules the sim was built with survive a clear, the same way the
     terrain speeds beside them do — a clear drops the world, not the
     numbers the world runs on. */
  int saved_shell_damage = sim->shell_damage;
  int saved_pill_range = sim->pill_range;
  int saved_pill_attack_ticks = sim->pill_attack_ticks;
  int saved_pill_attack_min_ticks = sim->pill_attack_min_ticks;
  int saved_pill_cooldown_ticks = sim->pill_cooldown_ticks;
  memcpy(saved_speed, sim->terrain_speed, sizeof(saved_speed));

  sim->num_pills = 0;
  sim->num_tanks = 0;
  sim->our_tank_idx = -1;
  sim->num_path = 0;
  sim->attack_target = -1;
  sim->tank_shoot_interval = sim->tank_shoot_interval_base;
  sim->shell_damage = saved_shell_damage;
  sim->pill_range = saved_pill_range;
  sim->pill_attack_ticks = saved_pill_attack_ticks;
  sim->pill_attack_min_ticks = saved_pill_attack_min_ticks;
  sim->pill_cooldown_ticks = saved_pill_cooldown_ticks;
  sim->dwell_ticks = 0;

  memset(&sim->lgm, 0, sizeof(sim->lgm));
  sim->lgm.dispatch_tick = -1;

  sim->map = saved_map;
  memcpy(sim->terrain_speed, saved_speed, sizeof(saved_speed));
}

void brainWorldSimSetMap(BrainWorldSim *sim, const BYTE *map) {
  sim->map = map;
}

void brainWorldSimSetAbortFlag(BrainWorldSim *sim, void *flag) {
  if (sim) sim->abort_flag = flag;
}

void brainWorldSimSetTerrainSpeed(BrainWorldSim *sim, int type, float speed) {
  if (type >= 0 && type < 16) {
    sim->terrain_speed[type] = speed;
  }
}

void brainWorldSimSetRules(BrainWorldSim *sim, int shell_damage, int pill_range,
                           int pill_attack_ticks, int pill_attack_min_ticks,
                           int pill_cooldown_ticks, int pill_hit_damage,
                           int shoot_interval, int forest_range) {
  if (!sim) return;
  /* Both ranges are squared into an int by the run loop. The rule bound is
     65535, whose square does not fit; 46340 is the largest value that does. */
  if (pill_range > 46340)   pill_range = 46340;
  if (forest_range > 46340) forest_range = 46340;
  if (shell_damage > 0)          sim->shell_damage = shell_damage;
  if (pill_range > 0)            sim->pill_range = pill_range;
  if (pill_attack_ticks > 0)     sim->pill_attack_ticks = pill_attack_ticks;
  if (pill_attack_min_ticks > 0) sim->pill_attack_min_ticks = pill_attack_min_ticks;
  if (pill_cooldown_ticks > 0)   sim->pill_cooldown_ticks = pill_cooldown_ticks;
  if (pill_hit_damage > 0)       sim->pill_hit_damage = pill_hit_damage;
  if (shoot_interval > 0) {
    sim->tank_shoot_interval_base = shoot_interval;
    sim->tank_shoot_interval = shoot_interval;
  }
  if (forest_range > 0)          sim->forest_range = forest_range;
}

void brainWorldSimAddPill(BrainWorldSim *sim, int mx, int my,
                          int health, float anger, int owner, int pill_id) {
  WSimPill *p;
  if (sim->num_pills >= WSIM_MAX_PILLS) return;
  p = &sim->pills[sim->num_pills];
  p->mx = (uint8_t)mx;
  p->my = (uint8_t)my;
  p->health = (uint8_t)health;
  p->owner = (uint8_t)owner;
  p->pill_id = (uint8_t)pill_id;
  p->shots_at_us = 0;
  p->just_seen = 0;
  p->reload = 0;

  /* Convert anger (0..1 float) to fire speed.
   * anger=0 → the calm interval, anger=1 → the interval at full anger */
  if (anger <= 0.0f) {
    p->speed = (uint8_t)sim->pill_attack_ticks;
    p->cooldown = 0;
  } else {
    int spd = (int)(sim->pill_attack_ticks
                    - anger * (sim->pill_attack_ticks - sim->pill_attack_min_ticks));
    if (spd < sim->pill_attack_min_ticks) spd = sim->pill_attack_min_ticks;
    if (spd > sim->pill_attack_ticks) spd = sim->pill_attack_ticks;
    p->speed = (uint8_t)spd;
    /* If already angry, assume some cooldown remains */
    p->cooldown = (uint8_t)(anger * sim->pill_cooldown_ticks);
  }

  sim->num_pills++;
}

void brainWorldSimAddTank(BrainWorldSim *sim, int wx, int wy,
                          int dir, int speed, int is_ours,
                          int owner, int armour) {
  WSimTank *t;
  if (sim->num_tanks >= WSIM_MAX_TANKS) return;
  t = &sim->tanks[sim->num_tanks];
  t->wx = wx;
  t->wy = wy;
  t->direction = (uint8_t)dir;
  t->speed = (uint8_t)speed;
  t->is_ours = (uint8_t)is_ours;
  t->owner = (uint8_t)owner;
  t->armour = (int16_t)armour;
  t->slide_vx = 0.0f;
  t->slide_vy = 0.0f;

  if (is_ours) {
    sim->our_tank_idx = sim->num_tanks;
  }
  sim->num_tanks++;
}

void brainWorldSimSetPath(BrainWorldSim *sim,
                          const WSimPathPoint *pts, int count) {
  if (count > WSIM_MAX_PATH) count = WSIM_MAX_PATH;
  memcpy(sim->path, pts, count * sizeof(WSimPathPoint));
  sim->num_path = count;
}

void brainWorldSimSetAttackTarget(BrainWorldSim *sim, int pill_index) {
  sim->attack_target = pill_index;
}

void brainWorldSimSetLGM(BrainWorldSim *sim, int dispatch_tick,
                         int dest_mx, int dest_my, int speed) {
  sim->lgm.dispatch_tick = (int16_t)dispatch_tick;
  sim->lgm.dest_wx = (dest_mx << 8) + 128;
  sim->lgm.dest_wy = (dest_my << 8) + 128;
  sim->lgm.active = 0;
  sim->lgm.speed = (uint8_t)(speed > 0 ? speed : 4);
  sim->lgm.wx = 0;
  sim->lgm.wy = 0;
}

void brainWorldSimSetDwell(BrainWorldSim *sim, int dwell_ticks) {
  if (!sim) return;
  if (dwell_ticks < 0) dwell_ticks = 0;
  sim->dwell_ticks = dwell_ticks;
}

/* ------------------------------------------------------------------ */
/* Simulation loop                                                     */
/* ------------------------------------------------------------------ */

WSimResult brainWorldSimRun(BrainWorldSim *sim, int max_ticks) {
  WSimResult result;
  int tick;
  int path_idx = 0;  /* current waypoint index */
  WSimTank *our;
  int our_shoot_timer = 0;
  int initial_armour;
  int dwell_damage = 0;   /* damage our tank took while parked at the dest */

  memset(&result, 0, sizeof(result));
  result.arrival_tick = -1;
  result.lgm_arrival_tick = -1;
  result.lgm_death_tick = -1;

  if (sim->our_tank_idx < 0 || sim->our_tank_idx >= sim->num_tanks) {
    return result;
  }

  our = &sim->tanks[sim->our_tank_idx];
  initial_armour = our->armour;

  for (tick = 1; tick <= max_ticks; tick++) {
    int i;
    int our_wx, our_wy;
    int our_parked;   /* tank stationary at its destination this tick */

    /* Cooperative abort. Per-tick header is the natural checkpoint —
     * the rest of the body assumes a complete tick. Jumping to `done`
     * lets the existing post-loop block fill in result.armour /
     * pill_final_health from whatever state we already simulated; the
     * brain sees a shorter prediction (smaller ticks_simulated, no
     * arrival_tick) which is exactly the existing "ran out of budget"
     * shape callers already handle. A dwell window makes runs longer,
     * so this checkpoint is what keeps a big dwell_ticks from eating the
     * brain's per-tick budget — it just degrades to a truncated result. */
    if (sim->abort_flag &&
        SDL_GetAtomicInt((SDL_AtomicInt *)sim->abort_flag)) {
      result.truncated = 1;  /* aborted early == unknown, not safe */
      goto done;
    }

    /* ============================================================= */
    /* 1. MOVE OUR TANK along path waypoints at terrain speed        */
    /* ============================================================= */
    if (path_idx < sim->num_path && (our->slide_vx == 0.0f && our->slide_vy == 0.0f)) {
      int dest_wx = (sim->path[path_idx].mx << 8) + 128;
      int dest_wy = (sim->path[path_idx].my << 8) + 128;
      int terrain = wsim_terrain_at(sim->map, our->wx, our->wy);
      float spd = sim->terrain_speed[terrain];
      int dx = dest_wx - our->wx;
      int dy = dest_wy - our->wy;
      int dist_sq = dx * dx + dy * dy;

      if (spd > 0 && dist_sq > 0) {
        float dist = sqrtf((float)dist_sq);
        float move = spd;
        if (move >= dist) {
          /* Arrived at waypoint */
          our->wx = dest_wx;
          our->wy = dest_wy;
          path_idx++;
          if (path_idx >= sim->num_path && result.arrival_tick < 0) {
            result.arrival_tick = (int16_t)tick;
            /* With no dwell requested, stop simulating — wsim then only
             * evaluates travel survivability, not prolonged combat at the
             * destination. With a dwell, fall through and keep running:
             * the tank now stands on the spot and the pills keep firing,
             * which is the part of the trip that actually kills bots that
             * park next to a hostile pillbox. */
            if (sim->dwell_ticks <= 0) goto done;
          }
        } else {
          our->wx += (int)(dx * move / dist);
          our->wy += (int)(dy * move / dist);
        }
      } else if (spd <= 0) {
        /* Impassable terrain — skip to next waypoint */
        path_idx++;
      }
    } else if (sim->dwell_ticks > 0 && result.arrival_tick >= 0 &&
               path_idx >= sim->num_path && sim->num_path > 0 &&
               our->slide_vx == 0.0f && our->slide_vy == 0.0f) {
      /* Dwell hold. The tank has arrived and is standing on the spot, but
       * pill knockback keeps shoving it off the tile. A real parked bot
       * drives back onto its spot, so pull the tank home at terrain speed
       * once the slide has decayed. Without this, repeated knockback would
       * walk the tank out of pill range over a long dwell and standing
       * still would read far safer than it is. */
      int hold_wx = (sim->path[sim->num_path - 1].mx << 8) + 128;
      int hold_wy = (sim->path[sim->num_path - 1].my << 8) + 128;
      int terrain = wsim_terrain_at(sim->map, our->wx, our->wy);
      float spd = sim->terrain_speed[terrain];
      int dx = hold_wx - our->wx;
      int dy = hold_wy - our->wy;
      int dist_sq = dx * dx + dy * dy;
      if (spd > 0 && dist_sq > 0) {
        float dist = sqrtf((float)dist_sq);
        if (spd >= dist) {
          our->wx = hold_wx;
          our->wy = hold_wy;
        } else {
          our->wx += (int)(dx * spd / dist);
          our->wy += (int)(dy * spd / dist);
        }
      }
    }

    /* ============================================================= */
    /* 2. APPLY KNOCKBACK SLIDE                                      */
    /* ============================================================= */
    wsim_apply_slide(sim, our);

    /* 2b. Proximity arrival — pill knockback can prevent exact waypoint
     * arrival, trapping the tank in a push-approach loop until death.
     * If we're within 1 tile of the final waypoint, count as arrived. */
    if (path_idx < sim->num_path && result.arrival_tick < 0) {
      int last_idx = sim->num_path - 1;
      int final_wx = (sim->path[last_idx].mx << 8) + 128;
      int final_wy = (sim->path[last_idx].my << 8) + 128;
      int fdx = final_wx - our->wx;
      int fdy = final_wy - our->wy;
      if (fdx * fdx + fdy * fdy <= 128 * 128) {
        result.arrival_tick = (int16_t)tick;
        if (sim->dwell_ticks <= 0) goto done;
        /* Dwelling: retire the rest of the path so the tank stops
         * advancing and holds here (section 1's dwell-hold branch keys
         * off path_idx >= num_path). Close enough is arrived. */
        path_idx = sim->num_path;
      }
    }

    /* ============================================================= */
    /* 3. MOVE ENEMY TANKS (straight-line extrapolation)             */
    /* ============================================================= */
    for (i = 0; i < sim->num_tanks; i++) {
      WSimTank *t = &sim->tanks[i];
      if (t->is_ours) continue;
      wsim_apply_slide(sim, t);
      if ((t->slide_vx == 0.0f && t->slide_vy == 0.0f) && t->speed > 0) {
        t->wx += (int)(wsim_sin_table[t->direction] * t->speed);
        t->wy += (int)(wsim_cos_table[t->direction] * t->speed);
        /* Clamp */
        if (t->wx < 0) t->wx = 0;
        if (t->wx > 32767) t->wx = 32767;
        if (t->wy < 0) t->wy = 0;
        if (t->wy > 32767) t->wy = 32767;
      }
    }

    our_wx = our->wx;
    our_wy = our->wy;

    /* Are we standing still at the end of the path this tick? Used below to
     * drop the moving-target damage discount and to attribute damage to the
     * dwell rather than the drive.
     *
     * All three terms are load-bearing, and dwell_ticks > 0 most of all: it
     * is what makes a no-dwell run bit-identical to the pre-dwell code on
     * EVERY route. Without it, `path_idx >= num_path` is also true when the
     * path was exhausted by the impassable-terrain skip above without the
     * tank ever arriving — that tick would fire pills at full damage and
     * credit it to damage_during_dwell, contradicting the documented
     * contract that dwell damage is 0 when no dwell was asked for. Worse, it
     * is unbounded rather than one tick: section 8 does not break while an
     * LGM is still pending, so a tank stranded mid-route would take full
     * undiscounted damage for the rest of max_ticks.
     *
     * arrival_tick >= 0 alone would imply path_idx >= num_path today (both
     * arrival sites retire the path), but the pair is stated explicitly so
     * "parked" cannot drift away from "at the end of the path". */
    our_parked = (sim->dwell_ticks > 0 &&
                  result.arrival_tick >= 0 &&
                  path_idx >= sim->num_path);

    /* ============================================================= */
    /* 4. DISPATCH LGM                                               */
    /* ============================================================= */
    if (sim->lgm.active == 0 && sim->lgm.dispatch_tick >= 0 &&
        tick >= sim->lgm.dispatch_tick) {
      sim->lgm.wx = our_wx;
      sim->lgm.wy = our_wy;
      sim->lgm.active = 1;
    }

    /* ============================================================= */
    /* 5. MOVE LGM toward destination                                */
    /* ============================================================= */
    if (sim->lgm.active == 1) {
      int ldx = sim->lgm.dest_wx - sim->lgm.wx;
      int ldy = sim->lgm.dest_wy - sim->lgm.wy;
      int ldist_sq = ldx * ldx + ldy * ldy;
      if (ldist_sq <= WSIM_LGM_ARRIVE_DIST * WSIM_LGM_ARRIVE_DIST) {
        sim->lgm.active = 2;
        result.lgm_arrival_tick = (int16_t)tick;
      } else {
        float ldist = sqrtf((float)ldist_sq);
        float lmove = (float)sim->lgm.speed;
        sim->lgm.wx += (int)(ldx * lmove / ldist);
        sim->lgm.wy += (int)(ldy * lmove / ldist);
      }
    }

    /* ============================================================= */
    /* 6. PILL FIRING                                                */
    /* ============================================================= */
    for (i = 0; i < sim->num_pills; i++) {
      WSimPill *pill = &sim->pills[i];
      int best_target = -1;  /* -1 = no target, 0..WSIM_MAX_TANKS = tank idx, 100 = lgm */
      int best_dist_sq = sim->pill_range * sim->pill_range + 1;
      int pill_wx, pill_wy;
      int j;

      if (pill->health == 0) continue;

      /* Advance reload timer */
      pill->reload++;

      /* Cooldown: anger decays over time */
      if (pill->cooldown > 0) {
        pill->cooldown--;
        if (pill->cooldown == 0 && pill->speed < sim->pill_attack_ticks) {
          /* Anger expired: speed drifts back up (simplified) */
          pill->speed = (uint8_t)(pill->speed * 2);
          if (pill->speed > sim->pill_attack_ticks)
            pill->speed = (uint8_t)sim->pill_attack_ticks;
        }
      }

      pill_wx = (pill->mx << 8) + 128;
      pill_wy = (pill->my << 8) + 128;

      /* Find nearest non-allied target in range */
      for (j = 0; j < sim->num_tanks; j++) {
        WSimTank *t = &sim->tanks[j];
        int dsq;
        if (t->armour <= 0) continue;
        /* Pill won't fire at own team */
        if (pill->owner != 0xFF && pill->owner == t->owner) continue;
        dsq = wsim_dist_sq(pill_wx, pill_wy, t->wx, t->wy);
        if (dsq < best_dist_sq) {
          /* Tree cover: target in forest and >= 3 tiles away → hidden */
          if (wsim_is_forest(sim->map, t->wx, t->wy) &&
              dsq > sim->forest_range * sim->forest_range) {
            continue;
          }
          best_dist_sq = dsq;
          best_target = j;
        }
      }

      /* Check LGM as target.
       *
       * WARNING — THIS DOES NOT MATCH THE GAME. Real pillboxes never shoot
       * at LGMs: pillsUpdate (pillbox.c:358) picks "the closest non-allied
       * TANK in range" and pillbox.c never mentions LGMs at all. Real
       * builders die only to explosion splash, via lgmDeathCheck (called
       * from shells.c:397,402,481,486, minesexp.c:248, tankexp.c:320).
       *
       * So result.lgm_survived / lgm_death_tick from this branch are NOT a
       * real signal — do not build decisions on them. Kept as-is on purpose:
       * removing it is a separate call, and nothing calls
       * brainWorldSimSetLGM today (lgm.dispatch_tick stays -1), so in
       * practice this branch is dead. */
      if (sim->lgm.active == 1) {
        int dsq = wsim_dist_sq(pill_wx, pill_wy,
                               sim->lgm.wx, sim->lgm.wy);
        if (dsq < best_dist_sq) {
          if (!wsim_is_forest(sim->map, sim->lgm.wx, sim->lgm.wy) ||
              dsq <= sim->forest_range * sim->forest_range) {
            best_dist_sq = dsq;
            best_target = 100; /* sentinel: LGM */
          }
        }
      }

      if (best_target >= 0) {
        /* First-sight delay */
        if (!pill->just_seen) {
          pill->just_seen = 1;
          pill->reload = 0;
          continue;
        }

        /* Fire if reloaded */
        if (pill->reload >= pill->speed) {
          int target_wx, target_wy;
          int is_lgm_target, is_our_tank, intercepted;
          pill->reload = 0;

          /* Determine target position for line trace */
          is_lgm_target = (best_target == 100);
          is_our_tank = 0;
          if (is_lgm_target) {
            target_wx = sim->lgm.wx;
            target_wy = sim->lgm.wy;
          } else {
            target_wx = sim->tanks[best_target].wx;
            target_wy = sim->tanks[best_target].wy;
            is_our_tank = sim->tanks[best_target].is_ours;
          }

          /* Trace line for crossfire: does the shell hit another pill? */
          intercepted = wsim_line_hits_pill(sim, pill_wx, pill_wy,
                                            target_wx, target_wy, i);
          if (intercepted >= 0) {
            /* Shell hits another pill instead */
            WSimPill *hit_pill = &sim->pills[intercepted];
            if (hit_pill->health > 0) {
              wsim_hit_pill(sim, hit_pill);
              wsim_anger_pill(sim, hit_pill);
            }
          } else if (is_lgm_target) {
            /* LGM instant kill */
            sim->lgm.active = 3; /* dead */
            result.lgm_death_tick = (int16_t)tick;
          } else {
            /* Hit the target tank.
             * Distance-scaled hit: wsim has no shell travel time, so pills
             * hit the tank instantly regardless of range. In reality, shells
             * take ~64 ticks to cover 8 tiles and moving tanks frequently
             * duck the shot. Scale damage by 1 - dist/range so close shots
             * still do full damage but far-range shots contribute less.
             * TODO: proper shell-flight model (option B) for better fidelity.
             *
             * The discount is bought by movement, so it expires the moment
             * we stop moving. A tank parked on its drop spot ducks nothing:
             * every shell the pill leads onto that tile lands, at whatever
             * range. Give our tank full shell_damage while parked. Enemy
             * tanks keep the discount — we don't track where they're headed,
             * so we can't tell whether they're stationary. */
            WSimTank *t = &sim->tanks[best_target];
            int dsq = wsim_dist_sq(pill_wx, pill_wy, t->wx, t->wy);
            int scaled;
            if (is_our_tank && our_parked) {
              scaled = sim->shell_damage;
            } else {
              float dist_frac = sqrtf((float)dsq) / (float)sim->pill_range;
              if (dist_frac > 1.0f) dist_frac = 1.0f;
              scaled = (int)(sim->shell_damage * (1.0f - dist_frac) + 0.5f);
              if (scaled < 1 && dsq <= (sim->pill_range/2) * (sim->pill_range/2)) {
                /* floor: very close shots always do at least 1 dmg */
                scaled = 1;
              }
            }
            t->armour -= scaled;
            if (is_our_tank && our_parked) {
              dwell_damage += scaled;
            }

            /* Record hit position if this is our tank */
            if (t->is_ours && result.num_hits < WSIM_MAX_HITS) {
              WSimHitRecord *hr = &result.hits[result.num_hits++];
              hr->mx = (int16_t)(t->wx >> 8);
              hr->my = (int16_t)(t->wy >> 8);
              hr->armour_after = t->armour;
              hr->tick = (int16_t)tick;
              hr->pill_idx = (uint8_t)i;
            }

            /* Knockback — compute initial velocity from pill toward tank */
            {
              float adx = (float)(t->wx - pill_wx);
              float ady = (float)(t->wy - pill_wy);
              float ang = atan2f(adx, -ady);  /* bolo coords */
              t->slide_vx = WSIM_SLIDE_INITIAL_SPEED * sinf(ang);
              t->slide_vy = WSIM_SLIDE_INITIAL_SPEED * (-cosf(ang));
            }

            if (is_our_tank) {
              pill->shots_at_us++;
            }
          }
        }
      } else {
        /* No target: clear first-sight flag */
        pill->just_seen = 0;
      }
    }

    /* ============================================================= */
    /* 7. OUR TANK SHOOTS (if attack_target set)                     */
    /* ============================================================= */
    if (sim->attack_target >= 0 && sim->attack_target < sim->num_pills) {
      our_shoot_timer++;
      if (our_shoot_timer >= sim->tank_shoot_interval) {
        WSimPill *target = &sim->pills[sim->attack_target];
        int target_wx = (target->mx << 8) + 128;
        int target_wy = (target->my << 8) + 128;
        int range_sq = wsim_dist_sq(our_wx, our_wy, target_wx, target_wy);

        if (range_sq <= sim->pill_range * sim->pill_range && target->health > 0) {
          int intercepted;
          our_shoot_timer = 0;

          /* Trace line for interception */
          intercepted = wsim_line_hits_pill(sim, our_wx, our_wy,
                                            target_wx, target_wy, -1);
          if (intercepted >= 0 && intercepted != sim->attack_target) {
            /* Hit a different pill */
            WSimPill *hit_pill = &sim->pills[intercepted];
            if (hit_pill->health > 0) {
              wsim_hit_pill(sim, hit_pill);
              wsim_anger_pill(sim, hit_pill);
            }
          } else {
            /* Hit the target pill */
            if (target->health > 0) {
              wsim_hit_pill(sim, target);
              wsim_anger_pill(sim, target);
            }
          }
        }
      }
    }

    /* ============================================================= */
    /* 8. CHECK TERMINATION                                          */
    /* ============================================================= */
    if (our->armour <= 0) {
      result.killed = 1;
      result.ticks_simulated = (int16_t)tick;
      break;
    }

    /* Path complete, dwell served, and no pending LGM.
     * The dwell test is skipped when arrival_tick is unset — that means the
     * path ran out under us (impassable-terrain skips) rather than being
     * walked to its end, and there is no arrival to count a dwell from.
     *
     * The `+ 1` makes a dwell of N simulate exactly N parked ticks. The
     * arrival tick is itself a parked tick — the tank is already standing on
     * the spot when section 6 runs that tick — so the parked ticks are
     * arrival_tick .. arrival_tick + N - 1. Without it, N would buy N+1.
     * With dwell_ticks == 0 the test stays the tautology `tick - arrival
     * + 1 >= 0`, so the no-dwell condition is unchanged. */
    if (path_idx >= sim->num_path &&
        (result.arrival_tick < 0 ||
         tick - result.arrival_tick + 1 >= sim->dwell_ticks) &&
        (sim->lgm.dispatch_tick < 0 || sim->lgm.active >= 2)) {
      result.ticks_simulated = (int16_t)tick;
      break;
    }

    result.ticks_simulated = (int16_t)tick;
  }

  /* Ran the clock out instead of reaching a natural end (death, or
   * arrival + dwell). `break` leaves tick <= max_ticks; only loop
   * exhaustion gets here with tick > max_ticks. Truncated == unknown. */
  if (tick > max_ticks) {
    result.truncated = 1;
  }

done:
  /* Fill result */
  if (result.ticks_simulated == 0) {
    result.ticks_simulated = (int16_t)((result.arrival_tick >= 0) ? result.arrival_tick : 0);
  }
  result.armour_remaining = our->armour;
  result.damage_taken = (int16_t)(initial_armour - our->armour);
  result.damage_during_dwell = (int16_t)dwell_damage;

  /* LGM outcome */
  if (sim->lgm.dispatch_tick < 0) {
    result.lgm_survived = 0; /* not dispatched */
  } else if (sim->lgm.active == 3) {
    result.lgm_survived = 2; /* killed */
  } else if (sim->lgm.active == 2) {
    result.lgm_survived = 1; /* arrived OK */
  } else {
    result.lgm_survived = 1; /* still walking, assume ok */
  }

  /* Per-pill stats */
  for (int i = 0; i < sim->num_pills; i++) {
    result.pill_shots[i] = sim->pills[i].shots_at_us;
    result.pill_final_health[i] = sim->pills[i].health;
    result.pill_final_speed[i] = sim->pills[i].speed;
  }

  return result;
}
