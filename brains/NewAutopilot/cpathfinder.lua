-- =========================================================================
-- cpathfinder.lua -- C-accelerated pathfinding module
--
-- Wraps the C pathfinder engine for use by Lua brains. Each brain
-- instance gets its own isolated pathfinder with its own terrain map,
-- danger grid, and configuration.
--
-- QUICK START:
--   local cpf = require("cpathfinder")
--   cpf.configure()                    -- load defaults
--   cpf.clear_danger()                 -- reset danger grid
--   cpf.stamp_pill(px, py, 9, 15, 0.5) -- stamp hostile pill
--   local status, nx, ny = cpf.path_to(...)  -- find next waypoint
--   local cost = cpf.estimate_cost(...)      -- score a destination
--
-- CUSTOMIZATION:
--   Modders can change any terrain cost, speed, or config value.
--   Custom spatial costs go in the overlay grid.
--
-- CONFIGURABLE VALUES:
--   terrain_cost   - table of terrain_type -> movement cost
--   terrain_speed  - table of terrain_type -> max tank speed
--                    (used to scale danger: slow terrain = more exposure)
--   turn_cost      - cost per 45deg direction change in A* (default 2)
--   wall_shoot_cost - A* cost to path through a wall (default 30)
--   wall_shoot_shells - shells consumed per wall (default 5)
--   shell_reserve  - don't plan wall-shoot below this (default 10)
--   road_build_cost - A* cost for tile we plan to pave (default 12)
--   tree_reserve   - don't plan road-build below this (default 4)
--   mine_penalty   - extra cost for mined tiles (default 40)
--   estimate_samples - sample points for estimate_cost (default 60)
--   water_drain_rate - shells+mines lost per river tile on foot (default 6)
--   shell_loss_cost  - A* cost per shell lost to water drain (default 3)
--   mine_loss_cost   - A* cost per mine lost to water drain (default 2)
--   armour_drain_rate- armour lost per unit of danger-exposure (default 0.02)
--   min_shells       - prune paths arriving with fewer shells (default 0)
--   min_mines        - prune paths arriving with fewer mines (default 0)
--   min_armour       - prune paths arriving with less armour (default 0)
-- =========================================================================

local C = require("constants")
local print2 = require("print2")

local M = {}

-- Default terrain costs (matching constants.lua TERRAIN_COST_LAND)
local DEFAULT_TERRAIN_COST = {
  [C.T_BUILDING]  = 9999,
  [C.T_RIVER]     = 8,
  [C.T_SWAMP]     = 8,
  [C.T_CRATER]    = 8,
  [C.T_ROAD]      = 1,
  [C.T_FOREST]    = 3,
  [C.T_RUBBLE]    = 8,
  [C.T_GRASS]     = 2,
  [C.T_HALFBUILD] = 9999,
  [C.T_BOAT]      = 2,
  [C.T_DEEPSEA]   = 9999,
  [C.T_REFBASE]   = 1,
  [C.T_PILLBOX]   = 2,
  [C.T_UNKNOWN]   = 3,
}

-- Default terrain speeds (matching constants.lua TERRAIN_SPEED)
local DEFAULT_TERRAIN_SPEED = {
  [C.T_BUILDING]  = 0,
  [C.T_RIVER]     = 3,
  [C.T_SWAMP]     = 3,
  [C.T_CRATER]    = 3,
  [C.T_ROAD]      = 16,
  [C.T_FOREST]    = 6,
  [C.T_RUBBLE]    = 3,
  [C.T_GRASS]     = 12,
  [C.T_HALFBUILD] = 0,
  [C.T_BOAT]      = 16,
  [C.T_DEEPSEA]   = 3,
  [C.T_REFBASE]   = 16,
  [C.T_PILLBOX]   = 16,
}

-- Default config scalars
local DEFAULT_CONFIG = {
  turn_cost         = 2,
  wall_shoot_cost   = 30,
  wall_shoot_shells = 5,
  shell_reserve     = 0,    -- let tank use all shells to break walls
  road_build_cost   = 12,
  tree_reserve      = 4,
  mine_penalty      = 40,
  estimate_samples  = 60,
  water_drain_rate  = 6,
  shell_loss_cost   = 3,
  mine_loss_cost    = 2,
  armour_drain_rate = 0.02,
  min_shells        = 0,
  min_mines         = 0,
  min_armour        = 0,
}

--- Configure the C pathfinder with defaults, optionally overridden.
--- @param opts table|nil Optional overrides:
---   opts.terrain_cost  = { [type] = cost, ... }  -- sparse overrides
---   opts.terrain_speed = { [type] = speed, ... } -- sparse overrides
---   opts.<config_key>  = value                   -- any config scalar
function M.configure(opts)
  opts = opts or {}

  -- Terrain costs
  local tc = opts.terrain_cost or {}
  for type, cost in pairs(DEFAULT_TERRAIN_COST) do
    cpf_set_terrain_cost(type, tc[type] or cost)
  end
  -- Apply any extra types from opts not in defaults
  for type, cost in pairs(tc) do
    if DEFAULT_TERRAIN_COST[type] == nil then
      cpf_set_terrain_cost(type, cost)
    end
  end

  -- Terrain speeds
  local ts = opts.terrain_speed or {}
  for type, speed in pairs(DEFAULT_TERRAIN_SPEED) do
    cpf_set_terrain_speed(type, ts[type] or speed)
  end
  for type, speed in pairs(ts) do
    if DEFAULT_TERRAIN_SPEED[type] == nil then
      cpf_set_terrain_speed(type, speed)
    end
  end

  -- Config scalars
  for key, default in pairs(DEFAULT_CONFIG) do
    cpf_set_config(key, opts[key] or default)
  end
end

-- Thin wrappers over cpf_* globals

function M.set_terrain_cost(type, cost)
  cpf_set_terrain_cost(type, cost)
end

function M.set_terrain_speed(type, speed)
  cpf_set_terrain_speed(type, speed)
end

function M.set_config(key, value)
  cpf_set_config(key, value)
end

function M.clear_danger()
  cpf_clear_danger()
end

function M.stamp_pill(cx, cy, radius, base_danger, anger)
  cpf_stamp_pill(cx, cy, radius, base_danger, anger)
end

function M.set_danger(x, y, value)
  cpf_set_danger(x, y, value)
end

--- Batch-load the danger grid from a Lua table keyed by mkey (my*256 + mx).
--- Clears the grid first, then writes every entry in a single C call —
--- replaces a per-tile Lua->C loop when the whole grid is refreshed.
function M.load_danger(tbl)
  cpf_load_danger(tbl)
end

function M.danger_at(x, y)
  return cpf_danger_at(x, y)
end

function M.clear_influence()
  cpf_clear_influence()
end

function M.stamp_influence(cx, cy, radius, strength)
  cpf_stamp_influence(cx, cy, radius, strength)
end

function M.influence_at(x, y)
  return cpf_influence_at(x, y)
end

function M.set_overlay(x, y, value)
  cpf_set_overlay(x, y, value)
end

function M.clear_overlay()
  cpf_clear_overlay()
end

--- Run incremental A* toward (dx, dy).
--- @return status integer  0=running, 1=done, -1=failed
--- @return nx integer      next step x (-1 if no step yet)
--- @return ny integer      next step y (-1 if no step yet)
function M.path_to(sx, sy, dx, dy, in_boat, shells, trees, mines, armour, budget, skip_dijkstra)
  -- Try Dijkstra first — same cost surface, no duplicate A* search.
  -- Uses KIND_NORMAL (0) for general navigation.
  -- Passes current tank position so it finds the next step from HERE,
  -- not from the Dijkstra source (which may be stale).
  -- skip_dijkstra: set true when the slate is known-stale for the
  -- destination tile (e.g. capture_pill targeting a pill that JUST
  -- died — slate still treats it as alive/impassable). Forces a
  -- fresh A* search every tick instead of trusting the cached slate.
  if C.DIJKSTRA_USE_FOR_GOALS and not skip_dijkstra then
    local nx, ny = cpf_dijkstra_next_step(M.KIND_NORMAL, sx, sy, dx, dy)
    if nx then
      print2(string.format("nav: dij (%d,%d)->(%d,%d) next=(%d,%d)", sx, sy, dx, dy, nx, ny))
      return 1, nx, ny  -- status=done, next step
    end
    print2(string.format("nav: dij MISS (%d,%d)->(%d,%d) — falling back to A*", sx, sy, dx, dy))
  end
  -- Fallback to A* if Dijkstra hasn't reached the destination yet
  local status, nx, ny = cpf_path_to(sx, sy, dx, dy, in_boat, shells, trees, mines, armour, budget)
  print2(string.format("nav: A* (%d,%d)->(%d,%d) status=%d next=(%s,%s)",
    sx, sy, dx, dy, status, tostring(nx), tostring(ny)))
  return status, nx, ny
end

--- One-shot A* cost query. Returns true path cost to (dx,dy).
--- Clobbers the active path_to search state — call before path_to.
--- @return cost number  (COST_INF ~1e30 if unreachable/budget exhausted)
M._cost_to_budget = 16000  -- runtime-adjustable from BrainTest debug panel

function M.cost_to(sx, sy, dx, dy, in_boat, shells, trees, mines, armour, budget)
  return cpf_cost_to(sx, sy, dx, dy, in_boat, shells, trees, mines, armour, budget or M._cost_to_budget)
end

--- Reset incremental cost_to state. Call once at start of replan cycle.
function M.cost_to_reset(sx, sy, in_boat, shells, trees, mines, armour)
  cpf_cost_to_reset(sx, sy, in_boat, shells, trees, mines, armour)
end

--- Incremental cost_to: reuses prior search state, returns instantly if
--- target was already reached by a previous call this cycle.
function M.cost_to_incremental(dx, dy, budget)
  return cpf_cost_to_incremental(dx, dy, budget or M._cost_to_budget)
end

--- Full Dijkstra from (sx, sy). No heuristic, no destination, no budget —
--- expands every reachable tile and reports timing. After it returns, the
--- internal g_cost array holds min cost to every reachable node.
--- Returns: us_taken, expanded_nodes, peak_open_size
function M.dijkstra_from(sx, sy, in_boat, shells, trees, mines, armour)
  return cpf_dijkstra_from(sx, sy, in_boat, shells, trees, mines, armour)
end

--- Begin a new incremental Dijkstra search on the given slate.
---   slate: 0 .. DIJKSTRA_NUM_SLATES-1
---   tick:  caller-supplied recency counter (typically state.tick).
---          Newer searches win in lookup_by_kind even when still running.
---   exact: nil/true = exact wall_shoot shell tracking, false = optimistic
---   danger_scale: per-slate danger weighting (1.0 = standard)
---   kind: matcher tag — lookups search slates with the same kind
function M.dijkstra_start(slate, tick, sx, sy, in_boat, shells, trees, mines, armour,
                          max_cost, exact, danger_scale, kind)
  if exact == nil then exact = true end
  cpf_dijkstra_start(slate, tick, sx, sy, in_boat, shells, trees, mines, armour,
                     max_cost or 0, exact, danger_scale or 1.0, kind or 0)
end

--- Resume the slate's Dijkstra. tick lets C record completed_tick when
--- the search finishes this step. Returns: done, expanded, peak_open.
function M.dijkstra_step(slate, tick, budget)
  return cpf_dijkstra_step(slate, tick, budget)
end

--- O(1) raw lookup of one slate.
function M.dijkstra_cost_at(slate, x, y, boat)
  return cpf_dijkstra_cost_at(slate, x, y, boat or 0)
end

--- Smart lookup: searches all slates of matching kind in started_tick
--- descending order, returns first finite cost. Newer (running) slates
--- win over older completed ones; partial-result fallback to older
--- slates is automatic when the newer slate hasn't reached the tile yet.
function M.dijkstra_lookup_by_kind(kind, x, y, boat)
  return cpf_dijkstra_lookup_by_kind(kind, x, y, boat or 0)
end

--- Pick the slate index to overwrite next when starting a search of the
--- given kind. Prefers unused slots, then oldest of matching kind, then
--- oldest overall — preserving the freshest matching slate as fallback.
function M.dijkstra_pick_reuse_slate(kind)
  return cpf_dijkstra_pick_reuse_slate(kind)
end

--- Returns the slate index of the freshest matching kind, or -1.
function M.dijkstra_find_best(kind)
  return cpf_dijkstra_find_best(kind)
end

--- Returns: active, done, kind, started_tick, completed_tick,
---          expanded, peak_open, src_x, src_y, in_boat, danger_scale
function M.dijkstra_status(slate)
  return cpf_dijkstra_status(slate)
end

--- Smart cost lookup: try the dijkstra slate of the requested kind
--- first, fall back to a fresh A* cost_to if the slate has no finite
--- cost for the destination yet. Gated by C.DIJKSTRA_USE_FOR_GOALS so
--- the entire fast-path can be turned off for A/B testing.
---
---   kind:    KIND_NORMAL (0) or KIND_PILL (1) — must match the kind
---            the brain's scheduler used when starting the slate.
---   sx, sy:  source (only used by the cost_to fallback; dijkstra
---            ignores this — its source is whatever was passed to
---            dijkstra_start).
---   dx, dy:  destination
---   in_boat, shells, trees, mines, armour: passed to cost_to fallback
function M.smart_cost(kind, sx, sy, dx, dy, in_boat, shells, trees, mines, armour)
  if C.DIJKSTRA_USE_FOR_GOALS then
    local c = cpf_dijkstra_lookup_by_kind(kind, dx, dy, in_boat or 0)
    if c < 1e29 then return c end
  end
  return cpf_cost_to(sx, sy, dx, dy, in_boat, shells, trees, mines, armour)
end

-- Convenience constants for the kind parameter.
M.KIND_NORMAL = 0
M.KIND_PILL   = 1

--- Force a rebuild of the precomputed edge cost grid. Normally happens
--- lazily after the map pointer changes; call this manually after a
--- terrain edit (base capture, pillbox demolition).
function M.rebuild_edge_costs()
  cpf_rebuild_edge_costs()
end

--- Estimate travel cost along a straight line.
--- @return cost number
function M.estimate_cost(sx, sy, dx, dy, in_boat)
  return cpf_estimate_cost(sx, sy, dx, dy, in_boat)
end

--- Simulate a shell flight (real physics: SHELL_SPEED, SHELL_START_ADD,
--- 24.8 fixed-point step) from origin world coords toward target world
--- coords. Returns the unique tiles the shell would cross, in order,
--- as a list of { mx = ..., my = ... }.
---
--- shooter_type: M.SHOT_TANK (default) or M.SHOT_PILL.
--- sight_len:    tank sightLen 1..14; 0 (default) = GUNSIGHT_MAX.
---               Ignored for SHOT_PILL (uses PILLBOX_FIRE_DISTANCE).
function M.simulate_shot(ox, oy, tx, ty, shooter_type, sight_len)
  -- Defensive: cpf_simulate_shot's C binding uses luaL_checkinteger
  -- on the wu coords, which crashes the brain if a float slips
  -- through (e.g. from a sin/cos product). Round half-up to int
  -- here so individual call sites don't have to remember.
  return cpf_simulate_shot(
    math.floor(ox + 0.5), math.floor(oy + 0.5),
    math.floor(tx + 0.5), math.floor(ty + 0.5),
    shooter_type or M.SHOT_TANK,
    sight_len or 0)
end

--- Bit-exact shell trajectory simulation: pass the firing angle
--- directly (0..255 bradians, FLOAT) instead of inferring from a
--- target point. Use this with info.tank_angle for a bit-exact
--- match to the engine's actual shell flight — the engine stores
--- tank.angle as a float and shellsAddItem fires at that exact
--- value, so the BYTE-floored info.direction misses the actual
--- flight path by up to one brad (~3 game pixels at gun_range 7).
function M.simulate_shot_angle(ox, oy, angle, shooter_type, sight_len)
  return cpf_simulate_shot_angle(
    math.floor(ox + 0.5), math.floor(oy + 0.5),
    angle,                              -- pass float through
    shooter_type or M.SHOT_TANK,
    sight_len or 0)
end

M.SHOT_TANK = 0
M.SHOT_PILL = 1

--- Estimate LGM travel time in game ticks (world coordinates).
--- Returns ticks to arrive, or -1 if stuck/blocked.
function M.lgm_travel_ticks(sx, sy, dx, dy, bless_mx, bless_my, max_ticks, stuck_ticks)
  return cpf_lgm_travel_ticks(sx, sy, dx, dy, bless_mx, bless_my, max_ticks, stuck_ticks)
end

--- Estimate LGM travel time in game ticks (map coordinates).
--- Converts to tile centers internally. Returns ticks or -1.
function M.lgm_travel_ticks_map(smx, smy, dmx, dmy, bless_mx, bless_my, max_ticks, stuck_ticks)
  return cpf_lgm_travel_ticks_map(smx, smy, dmx, dmy, bless_mx, bless_my, max_ticks, stuck_ticks)
end

--- Estimate tank travel time in game ticks along a straight line.
--- Simulates tick-by-tick movement using the brain's terrain speed table,
--- tracking boat state transitions. No obstacle avoidance — if the line
--- crosses impassable terrain (speed 0), returns -1.
---
--- Use this for timing overlaps (e.g. "how long until the tank reaches
--- the standoff?") rather than for pathfinding.
---
--- @param sx        integer  Start map tile X
--- @param sy        integer  Start map tile Y
--- @param dx        integer  Destination map tile X
--- @param dy        integer  Destination map tile Y
--- @param in_boat   boolean  Whether the tank starts in a boat
--- @param max_ticks integer|nil  Simulation cutoff (default 4000)
--- @param stuck_ticks integer|nil  Same-tile timeout (default 200)
--- @return integer  Estimated ticks to arrive, or -1 if blocked/stuck
--- Trace the full Dijkstra path from source to (dx,dy).
--- Returns array of {x=, y=} waypoints (source first, dest last), or nil.
--- Uses the best slate of the given kind.
function M.dijkstra_trace_path(kind, dx, dy)
  return cpf_dijkstra_trace_path(kind, dx, dy)
end

function M.estimate_tank_travel_ticks(sx, sy, dx, dy, in_boat, max_ticks, stuck_ticks)
  return cpf_estimate_tank_travel_ticks(sx, sy, dx, dy, in_boat,
                                         max_ticks or 4000, stuck_ticks or 200)
end

--- Find front-line points (influence sign-change boundaries).
--- Returns flat array {x1, y1, x2, y2, ...} of map coordinates.
function M.find_front_line()
  return cpf_find_front_line()
end

--- Shells remaining on arrival at (x,y) from the freshest Dijkstra slate of
--- the given kind. Returns nil if the slate hasn't reached that tile yet.
--- Valid after smart_cost when Dijkstra was the cost source.
function M.dijkstra_shells_at(kind, x, y)
  return cpf_dijkstra_shells_at(kind, x, y)
end

--- Shells remaining on arrival at (x,y) from the last cost_to call.
--- Valid after smart_cost when A* was used (Dijkstra off or tile not reached).
function M.astar_shells_at(x, y)
  return cpf_astar_shells_at(x, y)
end

--- Debug: trace full path after a completed search.
--- @return table  Array of {x=, y=} steps from src to dest.
function M.trace_path()
  return cpf_trace_path()
end

return M
