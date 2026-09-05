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

#ifndef BRAINPATHFINDER_TYPEDEF
#define BRAINPATHFINDER_TYPEDEF
typedef struct BrainPathfinder BrainPathfinder;
#endif

/* A* open-set heap entry */
typedef struct {
  float f;
  uint16_t x;
  uint16_t y;
  uint8_t boat;   /* 0 = on foot, 1 = in boat */
  uint8_t _pad;
} BrainPFHeapEntry;

/* ── Incremental Dijkstra slate ──
 * One slate holds all the state of a single full-map Dijkstra search.
 * The pathfinder owns N slates (see DIJKSTRA_NUM_SLATES) so the brain
 * can run multiple parallel searches with different parameters
 * (e.g. normal-danger vs lowered-pill-danger) and double-buffer the
 * recompute cycle so lookups always have a finished result available. */
#define DIJKSTRA_NUM_SLATES 4

typedef struct {
  float    *g_cost;       /* per-node best cost-from-source (NODE_COUNT) */
  uint8_t  *closed;       /* closed bitset (NODE_COUNT/8) */
  uint8_t  *dir_at;       /* per-node arrival direction (NODE_COUNT) */
  int16_t  *shells_at;    /* per-node remaining shells (NODE_COUNT) — exact mode only */
  BrainPFHeapEntry *heap;
  int32_t  *heap_pos;     /* per-node heap index (NODE_COUNT); -1 if not in heap */
  int       heap_capacity;
  int       open_count;

  int   done;         /* 1 if the search has emptied its heap */
  int   exact;        /* 1 = track per-node shell budget for wall_shoot */
  int   allow_boat;   /* 0 = never transition to boat nodes (land-only search) */
  int   kind;         /* user-defined tag for the freshness/lookup matcher */
  int   src_x, src_y;
  int   in_boat;
  int   src_shells;
  int   expanded;     /* nodes expanded since Start */
  int   peak_open;    /* peak heap size since Start */
  float max_cost;     /* hard cap on f; 0 = unlimited */
  float danger_scale; /* per-slate danger weighting (overrides pf->danger_scale) */
  uint32_t version;
  uint32_t started_tick;   /* tick passed to Start; identifies "recency" for lookups */
  uint32_t completed_tick; /* tick when done flipped true; 0 while running */
} DijkstraSlate;

/* Per-instance pathfinder state */
struct BrainPathfinder {
  /* Terrain map pointer (set each tick, not owned) */
  const BYTE *map;

  /* Spatial grids */
  uint16_t danger_grid[65536];        /* pill danger values */
  int16_t  overlay_grid[65536];       /* modder-extensible custom cost layer */
  int16_t  influence_grid[65536];     /* territorial influence: +friendly, -hostile */
  /* Influence tail (brainPathfinderRebuildInfluenceTail): the stamped cores
   * grown outward over passable ground, weakening with distance and slowed
   * inside a live neutral pill's range. expand_grid is the cached net tail
   * (friendly - hostile); influence_base_grid is the stamps alone, kept by
   * the merge so a viz can tell stamped ground from tail-claimed ground. */
  int16_t  influence_base_grid[65536];
  int16_t  expand_grid[65536];
  uint8_t  neutral_zone[65536];       /* 1 = inside a live neutral pill's range */
  uint8_t  tail_dist[65536];          /* scratch: BFS step distance, 255 = unreached */
  /* Deep-water margin for the tail: 1 = this tile is within `deep_margin`
   * king-moves of a deep-sea tile or of the map edge (off-map counts as deep
   * sea). Masked tiles never receive a grown tail value and never pass one on,
   * so the tail cannot paint a front line out over the sea or along the map
   * border. Scratch, not a cache: the map the brain hands us is its own
   * fogged view, one buffer whose contents change in place as tiles are
   * discovered, so the mask is rebuilt on every tail rebuild (~0.3ms, and the
   * rebuild only runs on a stamp-set change or every EXPAND_REFRESH_TICKS). */
  uint8_t  deep_margin_mask[65536];
  int16_t  danger_offset_grid[65536]; /* per-search danger adjustment (negative = subtract) */
  /* Coastal boat band: 1 where a water/boat tile lies within
   * BRAINPF_COASTAL_BAND euclidean tiles of land. Lets the land-only SHORT
   * Dijkstra slate take near-shore boat shortcuts without expanding the open
   * ocean. Computed once per map in brainPathfinderSetMap. */
  uint8_t  coastal_boat_mask[65536];
  const BYTE *coastal_mask_map;       /* map ptr the mask was computed for (recompute on change) */
  /* LGM-impassable tiles (1 = blocked). Stamped by the brain each tick with
   * enemy bases the LGM can't walk onto — the engine's mapGetManSpeed returns
   * speed 0 there (basesCantDrive: non-ally, non-neutral, armour > capture),
   * but the brain LGM sim only sees terrain type (refbase = walkable), so it
   * would otherwise march the LGM straight into an enemy base. */
  uint8_t  lgm_block[65536];

  /* Per-terrain-type tables (indexed 0..15) */
  float terrain_cost_table[16];      /* land mode costs */
  float terrain_cost_boat_table[16]; /* boat mode costs */
  float terrain_speed_table[16];

  /* Config scalars */
  float turn_cost;
  float wall_shoot_cost;
  float wall_shoot_shells;
  float wall_escalate_free;    /* walls charged plain wall_shoot_cost (default 1) */
  float wall_escalate_factor;  /* cost multiplier per wall past that (default 2) */
  float shell_reserve;
  float road_build_cost;
  float tree_reserve;
  float mine_penalty;
  float estimate_samples;
  float danger_scale;          /* multiplier for danger component in cost (default 1.0) */

  /* Resource drain config */
  float water_drain_rate;    /* shells+mines lost per river tile on foot (default 6) */
  float shell_loss_cost;     /* A* cost per shell lost to water drain (default 3) */
  float mine_loss_cost;      /* A* cost per mine lost to water drain (default 2) */
  float armour_drain_rate;   /* armour lost per unit of danger-exposure (default 0.02) */
  float road_build_danger_max; /* don't assume road-build when danger >= this (default 10) */
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

  /* Epoch versioning: a node's per-search state (g_cost / closed /
   * parent / dir_at / shells_at / ...) is valid only when
   * epoch[i] == current_epoch. A fresh search bumps current_epoch,
   * making all nodes uniformly stale in O(1) rather than memset'ing
   * the 2 MB of per-node arrays.
   *
   * Epoch 0 is reserved as "never touched" so a zero-initialized
   * struct reads as fully stale. current_epoch starts at 1. */
  uint32_t epoch[131072];
  uint32_t current_epoch;

  /* Precomputed per-tile per-direction edge base cost.
   * edge_cost[ni*8 + d] = base terrain cost of stepping from ni in direction d
   *                       (or COST_INF if blocked / out of bounds / diagonal
   *                       through impassable corner). Lazily allocated by
   * brainPathfinderRebuildEdgeCosts() so empty/test pathfinders don't pay
   * the 4MB allocation up front. Reused across many searches.
   *
   * Note: this is the STATIC portion only. Danger, overlay, mine penalty,
   * water-drain etc. still get applied in the inner loop at runtime. */
  float   *edge_cost;          /* malloc'd: 131072 * 8 floats = 4MB */
  int      edge_cost_valid;    /* 0 = stale (rebuild needed), 1 = current */
  const BYTE *edge_cost_map;   /* map pointer the cache was built from */

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

  /* Short-circuit cache for brainPathfinderPathTo. When a completed search
   * (status == 1) is asked again with identical inputs and nothing that
   * feeds the A* cost has changed since, return the stored next_x/next_y
   * without re-running A*. Mutators that can change an A* result set this
   * flag; a fresh search clears it. */
  uint8_t cache_dirty;

  /* Debug logging — when non-NULL, A* writes verbose trace to this file */
  void *astarLog;  /* FILE* — void* to avoid stdio include in header */
  const uint32_t *astarLogTickPtr;  /* pointer to current tick counter */

  /* Cooperative abort flag (SDL_AtomicInt *). When non-NULL, A* /
   * Dijkstra inner loops poll it at outer-iteration checkpoints; on
   * non-zero they break and return whatever they've computed so far.
   * Set once at bot creation by botManagerAddBot to point at the
   * BotContext's abort_flag. void * so this header doesn't pull in
   * SDL3 — the .c file casts on read. */
  void *abort_flag;

  /* ── Incremental Dijkstra slates ──
   * Each slate holds the full state of one Dijkstra search. The brain
   * can use them however it wants — typical pattern is double-buffered
   * pairs (current + previous fallback) for two danger-scale variants
   * (normal vs lowered-pill-danger). All slates are lazy-allocated on
   * first use (~700 KB each, ~960 KB with exact mode). */
  DijkstraSlate dij_slates[DIJKSTRA_NUM_SLATES];
};

/*********************************************************
 * Public API
 *********************************************************/

BrainPathfinder *brainPathfinderCreate(void);
void brainPathfinderDestroy(BrainPathfinder *pf);
void brainPathfinderSetMap(BrainPathfinder *pf, const BYTE *map);

/* Set the cooperative abort flag the inner A* / Dijkstra loops poll.
 * `flag` is an SDL_AtomicInt * (void * here so callers without SDL
 * available don't need to depend on it). NULL disables polling. */
void brainPathfinderSetAbortFlag(BrainPathfinder *pf, void *flag);

/*
 * Eagerly allocate the per-slate working arrays for every slate AND touch
 * every page so the OS commits backing pages now instead of lazy-faulting
 * them on the first dijkstra_start call. Without this, tick 1 of the
 * first dijkstra_start pays ~1-2 ms of page-fault cost on a fresh process.
 *
 * Idempotent: arrays already allocated are left in place. Safe to call
 * multiple times. Call once after brainPathfinderCreate() at brain
 * instance setup.
 */
void brainPathfinderDijkstraPreheat(BrainPathfinder *pf);

/* Debug logging — writes detailed A* info to astar_costto.log */
void brainPathfinderEnableLog(int enable);
void brainPathfinderEnableLogPath(const char *path);
int brainPathfinderIsLogEnabled(void);
void brainPathfinderSetLogTick(int tick);
void brainPathfinderSetLogCaller(const char *caller);
/* Independent toggle for the per-step incremental Dijkstra log
 * (dijkstra.log). Off by default; the per-tick STEP entries are
 * high-frequency and add measurable overhead, so this isn't piggy-
 * backed on the cost_to debug toggle. */
void brainPathfinderEnableDijkstraLog(int enable);

/* Incremental cost_to: reset clears accumulated state,
 * then subsequent cost_to calls reuse closed nodes from prior searches */
void brainPathfinderCostToReset(BrainPathfinder *pf,
                                 int sx, int sy, int in_boat,
                                 int shells, int trees, int mines, int armour);
float brainPathfinderCostToIncremental(BrainPathfinder *pf,
                                        int dx, int dy, int budget);

/* Full Dijkstra from (sx,sy). Expands every reachable node — no heuristic,
 * no destination, no budget. After it returns, the per-node g_cost array
 * holds the minimum cost to every reachable tile.
 *
 * Returns wall-clock microseconds taken. Out-params (if non-NULL) report
 * how many nodes were expanded and the peak open-list size. */
double brainPathfinderDijkstraFrom(BrainPathfinder *pf,
                                    int sx, int sy, int in_boat,
                                    int shells, int trees, int mines, int armour,
                                    int *out_expanded, int *out_peak_open);

/* ── Incremental (split-across-ticks) Dijkstra, multi-slate ──
 * Each slate (0..DIJKSTRA_NUM_SLATES-1) is an independent search work
 * area with its own g_cost, closed set, heap, etc. Pattern:
 *
 *   brainPathfinderDijkstraStart(pf, slate, tick, sx, sy, ...,
 *                                max_cost, exact, danger_scale, kind);
 *   // ... each tick:
 *   int done = brainPathfinderDijkstraStep(pf, slate, tick, budget);
 *   // lookup, by-kind so brain doesn't need to know slate indices:
 *   float c = brainPathfinderDijkstraLookupByKind(pf, kind, x, y, boat);
 *
 * tick: a monotonically-increasing counter the caller provides. Used to
 *       determine slate "recency" — newer slates are preferred for
 *       lookups even if still running.
 * kind: caller-defined matcher tag. LookupByKind only considers slates
 *       with the same kind, picking the one with the highest started_tick.
 *       If that slate hasn't reached the destination yet, the next-newest
 *       slate of the same kind is tried, and so on. Lets the brain run
 *       multiple parallel searches with different parameters and have a
 *       single lookup pick the freshest valid result.
 */
void  brainPathfinderDijkstraStart(BrainPathfinder *pf, int slate, uint32_t tick,
                                    int sx, int sy, int in_boat,
                                    int shells, int trees, int mines, int armour,
                                    float max_cost, int exact,
                                    float danger_scale, int kind, int allow_boat);
int   brainPathfinderDijkstraStep(BrainPathfinder *pf, int slate, uint32_t tick, int budget);
float brainPathfinderDijkstraCostAt(BrainPathfinder *pf, int slate,
                                     int x, int y, int boat);
int   brainPathfinderDijkstraStatus(BrainPathfinder *pf, int slate,
                                     int *out_expanded, int *out_peak_open,
                                     int *out_done);
void  brainPathfinderDijkstraCopySlate(BrainPathfinder *pf, int src, int dst);

/* High-level lookup: iterate all active slates of matching kind in
 * started_tick descending order, return the first one that has a finite
 * g_cost for the destination. The newest slate wins even if still
 * running; the older slate (or older still) serves as fallback. Returns
 * COST_INF if no slate of the kind has reached the destination yet. */
float brainPathfinderDijkstraLookupByKind(BrainPathfinder *pf, int kind,
                                           int x, int y, int boat);

/* Same as above, but subtracts a target pill's danger contribution along
 * the slate's realized path. After picking a slate (same recency rule
 * as LookupByKind), walks the parent chain from (x,y) back toward the
 * source. For each non-source tile, calls pcontrib_lookup(user, tile_key)
 * to get the pill's per-tile danger contribution, scales it by the
 * slate's danger_scale and the per-tile inv_speed (matching the slate's
 * own expansion formula), and accumulates a subtraction from the
 * returned cost. Result is the "as-if-the-target-pill-were-dead" cost
 * for the realized path — exact, no path-walk approximation in Lua.
 * Returns COST_INF if no slate has the destination, or the unmodified
 * lookup cost if pcontrib_lookup is NULL. Result is clamped at 0. */
typedef float (*BrainPFTileLookupFn)(void *user, int tile_key);
float brainPathfinderDijkstraLookupSubtractByKind(BrainPathfinder *pf, int kind,
                                                   int x, int y, int boat,
                                                   BrainPFTileLookupFn pcontrib_lookup,
                                                   void *user);

/* Trace the Dijkstra parent chain from (dx,dy) back to the source.
 * Returns the first step on the optimal path.
 * Returns 1 on success (out_next_x/y populated), 0 if unreachable.
 *
 * obstacles/n_obstacles/penalty: an OPTIONAL live obstacle set evaluated at
 * trace time (no slate recompute). Each entry is a packed tile key (y*256+x).
 * When the optimal next tile is an obstacle, the step veers to the cheapest
 * non-obstacle neighbour by effective cost (g_cost + penalty), so moving
 * allies are dodged instantly without baking anything into the slate. Pass
 * obstacles=NULL / n_obstacles=0 for the plain optimal-path behaviour. */
int brainPathfinderDijkstraNextStep(BrainPathfinder *pf, int kind,
                                     int sx, int sy,
                                     int dx, int dy,
                                     const int *obstacles, int n_obstacles,
                                     float penalty,
                                     int *out_next_x, int *out_next_y);

/* Find the slate index that the brain should reuse next when starting a
 * search of the given kind. Picks the slate with the LOWEST started_tick
 * among slates of matching kind (LRU within kind), so the freshest
 * matching slate is preserved as a fallback. If no slate has the kind
 * yet, picks the oldest slate of any kind (or an unused slot). */
int brainPathfinderDijkstraPickReuseSlate(BrainPathfinder *pf, int kind);

/* Find the freshest slate of given kind. Returns -1 if none. */
int brainPathfinderDijkstraFindBest(BrainPathfinder *pf, int kind);

/* Read-only access to a slate's metadata so the brain can mirror state
 * for debugging / scheduler decisions. */
const DijkstraSlate *brainPathfinderDijkstraGetSlate(BrainPathfinder *pf, int slate);

/* Trace the path from the slate's search source to (dx, dy). */
int brainPathfinderDijkstraTracePath(BrainPathfinder *pf, int slate,
                                      int dx, int dy,
                                      int *path_x, int *path_y,
                                      int max_steps);

/* Multi-slate trace: walks slates of `kind` in started_tick descending
 * order, returns the trace from the first slate where (dx, dy) has a
 * finite cost. Mirrors brainPathfinderDijkstraLookupByKind's slate
 * selection so a cost found via fallback to an older slate is matched
 * by a path traced from THAT slate. Use this instead of
 * DijkstraTracePath(FindBest(...), ...) when consumers need the path
 * to correspond to whichever slate actually has the destination. */
int brainPathfinderDijkstraTracePathByKind(BrainPathfinder *pf, int kind,
                                            int dx, int dy,
                                            int *path_x, int *path_y,
                                            int max_steps);

/* Precomputed neighbor edge cost grid. Recomputes the static portion of
 * compute_cost (terrain base + diagonal corner blocking) for every tile,
 * for every direction. Speeds up Dijkstra/A* inner loops by ~30-40%.
 * Call this whenever the map changes (which is rare for the brain). */
void  brainPathfinderRebuildEdgeCosts(BrainPathfinder *pf);

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

/* Influence tail. Rebuild reads the stamped influence_grid (cells with
 * |v| >= seed_min are the cores), grows each side outward over passable
 * ground with a step-cost BFS (land 1, shallow water water_step, cells in
 * neutral_zone neutral_step; buildings / deep sea / pillboxes block), capped
 * at `radius` steps, valued start*(1 - d/(radius+1)), and caches the net
 * (friendly - hostile; an exact tie goes to the hostile side, -1, so the
 * front line never has a signless zero seam) in expand_grid. Merge writes
 * influence_grid = stamped where |stamped| >= |tail|, else tail, keeping the
 * stamps in influence_base_grid. Call Merge every tick after stamping;
 * Rebuild only when the stamped set changes.
 * deep_margin > 0 additionally forbids the tail from claiming any tile within
 * that many king-moves of deep sea or of the map edge (off-map counts as deep
 * sea): such a tile gets no tail value and passes none on, so no front line is
 * drawn out over the water or along the border. Stamped cores still seed, so a
 * core standing near the shore keeps growing inland. 0 = old behaviour.
 * enemy_tail selects WHICH sides grow. 1 = old behaviour, both: the friendly
 * pass adds and the hostile pass subtracts in the one signed expand_grid, so
 * the two tails cancel and the front line settles midway between the sides.
 * 0 = only our cores grow; hostile stamps keep their raw discs but spread no
 * further, so nothing cancels the friendly tail and the front line forms at
 * the edge of the enemy's disc. The tail can still only overwrite a stamp it
 * outweighs (Merge compares magnitudes), so the enemy's footprint is intact
 * and only its outer, weakest ring can be claimed. */
void brainPathfinderClearNeutralZones(BrainPathfinder *pf);
void brainPathfinderStampNeutralZone(BrainPathfinder *pf, int cx, int cy, int radius);
void brainPathfinderRebuildInfluenceTail(BrainPathfinder *pf, int seed_min, int radius,
                                         int start, int neutral_step, int water_step,
                                         int deep_margin, int enemy_tail);
void brainPathfinderMergeInfluenceTail(BrainPathfinder *pf);
/* The tail value at (x,y) if the tail won the last merge there, else 0. */
int16_t brainPathfinderInfluenceTailAt(BrainPathfinder *pf, int x, int y);
/* Diagnostics after a merge: out[7] = { tail cells +, tail cells -, cells
 * where the tail WON the merge +, -, front-line cells on the stamps alone,
 * front-line cells on the merged grid, cells at the -1 tie value }. */
void brainPathfinderInfluenceTailStats(BrainPathfinder *pf, int *out);

/* Custom overlay (modder extension point) */
void brainPathfinderSetOverlay(BrainPathfinder *pf, int x, int y, float value);
void brainPathfinderClearOverlay(BrainPathfinder *pf);
float brainPathfinderGetOverlay(const BrainPathfinder *pf, int x, int y);
float brainPathfinderGetDanger(const BrainPathfinder *pf, int x, int y);

/* Per-search danger offset — subtracted from danger on the fly during A*.
 * Use to model a specific pill as dead without touching the danger grid.
 * Set negative values to reduce effective danger; clear after the search. */
void brainPathfinderSetDangerOffset(BrainPathfinder *pf, int x, int y, int16_t value);
void brainPathfinderClearDangerOffset(BrainPathfinder *pf);

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

/* Same as CostTo but with allow_boat control. When allow_boat=0,
 * tiles that would put the tank in a boat (TT_BOAT or water entered
 * from land in a boat) are treated as impassable. Halves the search
 * space when boat exploration isn't needed. */
float brainPathfinderCostToEx(BrainPathfinder *pf,
                               int sx, int sy, int dx, int dy,
                               int in_boat, int shells, int trees,
                               int mines, int armour, int budget,
                               int allow_boat);

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

/* LGM-impassable overlay: clear all, then mark (mx,my) tiles the LGM can't
 * cross (enemy bases). Brain stamps these each tick before LGM reach checks. */
void brainPathfinderClearLgmBlock(BrainPathfinder *pf);
void brainPathfinderSetLgmBlock(BrainPathfinder *pf, BYTE mx, BYTE my);

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

/* Like brainPathfinderTracePath but takes explicit destination and
 * skips the pf->status check. Use this after a cost_to call: cost_to
 * resets status/dest to -1 at the end (so the next path_to starts
 * fresh) but the closed/parent state is still readable in the current
 * epoch, which is enough to walk the path. Returns 0 if dest isn't in
 * the closed set (i.e., the search didn't reach it). */
int brainPathfinderTraceLastSearchPath(BrainPathfinder *pf,
                                        int dx, int dy,
                                        int *path_x, int *path_y,
                                        int max_steps);

/* Scan influence grid for front-line cells (where positive/negative neighbors meet).
 * Writes up to max_points pairs into out_x[], out_y[].
 * Skips cells where both values are 0 (unclaimed vs unclaimed).
 * Returns number of front-line points found. */
int brainPathfinderFindFrontLine(BrainPathfinder *pf,
                                  int *out_x, int *out_y, int max_points);

/* ── Shot path simulation ──────────────────────────────────────
 * Quick geometric simulation of a shell flying from (origin_wx,
 * origin_wy) toward (target_wx, target_wy), as if fired by a tank
 * or pillbox. Mirrors the real shell physics from shellsAddItem +
 * shellsUpdate (SHELL_SPEED, SHELL_START_ADD initial offset, the
 * 24.8 fixed-point per-tick step) so tile crossings match what an
 * actual fired shell would touch.
 *
 * Does NOT short-circuit on collision — the full geometric flight
 * to the shooter's max range is recorded. Callers can intersect
 * the returned tiles against terrain/objects themselves. */

#define BRAIN_SHOT_SHOOTER_TANK 0
#define BRAIN_SHOT_SHOOTER_PILL 1

/* hit_type: 0 = map tile traversal, 1 = tank hit at this position.
 * When hit_type==1, hit_id is the player number of the tank hit
 * and mx/my is the tile the shell was on when the hit occurred. */
typedef struct {
  uint8_t mx;
  uint8_t my;
  uint8_t hit_type;
  uint8_t hit_id;
} BrainShotTile;

#define BRAIN_SHOT_HIT_TILE 0
#define BRAIN_SHOT_HIT_TANK 1

/* Tank position for simulate_shot_with_tanks. */
typedef struct {
  WORLD wx, wy;
  uint8_t player_num;
} BrainShotTankPos;

/* Returns the number of unique tiles written to out_tiles
 * (de-duplicated against the previous tile, never against earlier
 * tiles — a shot that loops would record both visits, but real
 * shells fly straight so this is a non-issue).
 *
 * shooter_type: BRAIN_SHOT_SHOOTER_TANK or BRAIN_SHOT_SHOOTER_PILL.
 * sight_len:    tank's sightLen (1..GUNSIGHT_MAX). Pass 0 to use
 *               GUNSIGHT_MAX. Ignored when shooter_type is PILL.
 *
 * Returns 0 if origin == target, out_tiles is NULL, or max_tiles<=0.
 * Stops early (returning max_tiles) if the buffer fills. */
int brainPathfinderSimulateShot(WORLD origin_wx, WORLD origin_wy,
                                 WORLD target_wx, WORLD target_wy,
                                 int shooter_type, int sight_len,
                                 BrainShotTile *out_tiles, int max_tiles);

/* Same as brainPathfinderSimulateShot but takes the firing angle
 * directly (0..255 bradians, FLOAT) instead of deriving it from
 * origin → target geometry. Use this when you want a bit-exact
 * match to a real shell — the engine stores tank.angle as a float
 * and shellsAddItem fires at that exact value, so a brain that has
 * the float angle (BrainInfo.tank_angle) gets sub-brad precision by
 * passing it here. Integer callers can promote freely. */
int brainPathfinderSimulateShotAngle(WORLD origin_wx, WORLD origin_wy,
                                     float angle,
                                     int shooter_type, int sight_len,
                                     BrainShotTile *out_tiles, int max_tiles);

/* Same as brainPathfinderSimulateShot but also checks for tank hits.
 * Tank positions are passed via tanks/num_tanks. When the shell enters
 * the 128 wu hitbox of a tank, a hit_type=1 entry is emitted at that
 * point in the sequence (interspersed with tile entries). The shell
 * stops on the first tank hit (same as the engine). owner_player is
 * the firing player — own tank is excluded from hit checks. */
int brainPathfinderSimulateShotWithTanks(WORLD origin_wx, WORLD origin_wy,
                                          WORLD target_wx, WORLD target_wy,
                                          int shooter_type, int sight_len,
                                          const BrainShotTankPos *tanks, int num_tanks,
                                          uint8_t owner_player,
                                          BrainShotTile *out_tiles, int max_tiles);

/* ── Serialization (for exact trace replay) ────────────────── */

/* Serialize the full pathfinder state (grids, config, Dijkstra slates)
 * into a malloc'd binary blob. Returns the blob and sets *out_size.
 * Caller frees with free(). Returns NULL on failure. */
unsigned char *brainPathfinderSerialize(BrainPathfinder *pf, size_t *out_size);

/* Restore pathfinder state from a blob previously returned by
 * brainPathfinderSerialize. Returns 1 on success, 0 on failure.
 * Does NOT allocate the pathfinder itself — caller must pass an
 * existing (created) BrainPathfinder. */
int brainPathfinderDeserialize(BrainPathfinder *pf,
                                const unsigned char *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_PATHFINDER_H */
