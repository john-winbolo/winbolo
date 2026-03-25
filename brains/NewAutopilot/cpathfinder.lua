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
  shell_reserve     = 10,
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
function M.path_to(sx, sy, dx, dy, in_boat, shells, trees, mines, armour, budget)
  return cpf_path_to(sx, sy, dx, dy, in_boat, shells, trees, mines, armour, budget)
end

--- One-shot A* cost query. Returns true path cost to (dx,dy).
--- Clobbers the active path_to search state — call before path_to.
--- @return cost number  (COST_INF ~1e30 if unreachable/budget exhausted)
function M.cost_to(sx, sy, dx, dy, in_boat, shells, trees, mines, armour, budget)
  return cpf_cost_to(sx, sy, dx, dy, in_boat, shells, trees, mines, armour, budget or 4000)
end

--- Estimate travel cost along a straight line.
--- @return cost number
function M.estimate_cost(sx, sy, dx, dy, in_boat)
  return cpf_estimate_cost(sx, sy, dx, dy, in_boat)
end

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
function M.estimate_tank_travel_ticks(sx, sy, dx, dy, in_boat, max_ticks, stuck_ticks)
  return cpf_estimate_tank_travel_ticks(sx, sy, dx, dy, in_boat,
                                         max_ticks or 4000, stuck_ticks or 200)
end

--- Find front-line points (influence sign-change boundaries).
--- Returns flat array {x1, y1, x2, y2, ...} of map coordinates.
function M.find_front_line()
  return cpf_find_front_line()
end

--- Debug: trace full path after a completed search.
--- @return table  Array of {x=, y=} steps from src to dest.
function M.trace_path()
  return cpf_trace_path()
end

return M
