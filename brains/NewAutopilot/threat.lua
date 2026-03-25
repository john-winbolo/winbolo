-- =========================================================================
-- NewAutopilot/threat.lua — unified spatial threat grid
--
-- Two layers:
--
--   1. Pill threat (persistent, dirty-flag rebuild) — hostile/neutral pills
--      stamp a circular area of radius PILL_RANGE_MAP.  Cost falls off with
--      distance.  Includes anger scaling, tree cover, and wall shielding.
--      Only rebuilt when a pill's state changes or terrain near a pill changes.
--
--   2. Enemy tank presence (rebuilt every tick) — visible hostile tanks stamp
--      a threat zone.  Gives pathfinder and goal scorer awareness of enemy
--      tanks without warping long-range A* (pill_at excludes this layer).
--
-- Shell trajectories are NOT in the grid — they live in danger.lua:shell_map
-- and are summed at read time by danger.danger_at().
--
-- Consumers:
--   threat.at(mx, my)      — combined pill + tank threat (O(1) lookup)
--   threat.pill_at(mx, my) — pill-only threat for A* terrain cost
-- =========================================================================

local C       = require("constants")
local U       = require("util")
local changes = require("changes")
local metrics = require("metrics")

local M = {}

-- Sparse grids: only tiles near threat sources get entries.
-- pill_grid[mkey]  = pill threat value
-- tank_grid[mkey]  = tank threat value
local pill_grid = {}
local tank_grid = {}

-- Dirty-flag state for pill grid: only rebuild when something changed.
-- prev_pills[id] = {owner, health, anger_q, mx, my}
local prev_pills = {}
local pill_dirty = true  -- force first build

-- -------------------------------------------------------------------------
-- Constants for tank threat layer
-- -------------------------------------------------------------------------
local TANK_THREAT_RADIUS = 6    -- tiles around each enemy tank
local TANK_THREAT_BASE   = 10   -- threat value at point-blank

-- Quantize anger to 4 levels so tiny decay doesn't trigger rebuild.
-- Anger decays over ~3000 ticks; 4 levels = change every ~750 ticks.
local function anger_q(a)
  return math.floor((a or 0) * 4)
end

-- -------------------------------------------------------------------------
-- Internal: raw terrain read — bypasses U.ttype() change detection so the
-- pill grid rebuild doesn't pollute changes.terrain and cascade rebuilds.
-- -------------------------------------------------------------------------
local function raw_tt(mx, my)
  return get_terrain(mx, my) & TERRAIN_MASK
end

-- -------------------------------------------------------------------------
-- Internal: check if a tile is "in trees" (tile + 4 cardinal neighbors)
-- Mirrors engine's utilIsTankInTrees().
-- Uses raw_tt to avoid change-detection overhead during rebuilds.
-- -------------------------------------------------------------------------
local function tile_in_trees(mx, my)
  if raw_tt(mx, my) ~= C.T_FOREST then return false end
  if not U.in_map(mx-1, my) or raw_tt(mx-1, my) ~= C.T_FOREST then return false end
  if not U.in_map(mx+1, my) or raw_tt(mx+1, my) ~= C.T_FOREST then return false end
  if not U.in_map(mx, my-1) or raw_tt(mx, my-1) ~= C.T_FOREST then return false end
  if not U.in_map(mx, my+1) or raw_tt(mx, my+1) ~= C.T_FOREST then return false end
  return true
end

-- -------------------------------------------------------------------------
-- Internal: count wall HP on line between two map tiles (Bresenham).
-- Each T_BUILDING = WALL_HP_FULL, T_HALFBUILD = WALL_HP_HALF.
-- Uses raw_tt to avoid change-detection overhead during rebuilds.
-- -------------------------------------------------------------------------
local function count_wall_hp_between(x0, y0, x1, y1)
  local wall_hp = 0
  U.bresenham(x0, y0, x1, y1, function(cx, cy)
    if U.in_map(cx, cy) then
      local tt = raw_tt(cx, cy)
      if tt == C.T_BUILDING then
        wall_hp = wall_hp + C.WALL_HP_FULL
      elseif tt == C.T_HALFBUILD then
        wall_hp = wall_hp + C.WALL_HP_HALF
      end
    end
  end)
  return wall_hp
end

-- -------------------------------------------------------------------------
-- Internal: stamp pill threat into pill_grid for one pill
-- -------------------------------------------------------------------------
local function stamp_pill(pm)
  local px, py = pm.mx, pm.my
  local anger = pm.anger or 0
  local R = C.PILL_RANGE_MAP

  for dy = -R, R do
    local ny = py + dy
    if ny >= 0 and ny <= 255 then
      for dx = -R, R do
        local nx = px + dx
        if nx >= 0 and nx <= 255 then
          local dist = math.abs(dx) + math.abs(dy)
          if dist <= R then
            local proximity = 1.0 - dist / (R + 1)
            local penalty = (C.PILL_DANGER_BASE + C.PILL_DANGER_ANGER * anger) * proximity

            -- Tree cover: reduce penalty if hidden in trees
            local in_trees = tile_in_trees(nx, ny)
            if in_trees then
              if dist >= C.MIN_TREEHIDE_DIST_MAP then
                penalty = penalty * 0.1
              else
                penalty = penalty * 0.7
              end
            end

            -- Wall cover: walls between pill and target absorb shots.
            -- Only check close range (dist <= 4) where wall shielding matters
            -- most and penalty is large.  At long range, proximity falloff
            -- already makes the penalty small so wall shielding is negligible.
            -- This cuts Bresenham calls by ~80%.
            if dist <= 4 then
              local wall_hp = count_wall_hp_between(px, py, nx, ny)
              if wall_hp > 0 then
                local fire_rate = 1 + (1 - anger) * 31
                local wall_time = wall_hp * fire_rate
                local shielding = math.min(1.0, wall_time / C.WALL_SHIELD_TIME)
                penalty = penalty * (1.0 - shielding)
              end
            end

            if penalty > 0 then
              local k = U.mkey(nx, ny)
              pill_grid[k] = (pill_grid[k] or 0) + penalty
            end
          end
        end
      end
    end
  end
end

-- -------------------------------------------------------------------------
-- Internal: stamp tank threat into tank_grid for one enemy tank
-- -------------------------------------------------------------------------
local function stamp_tank(tmx, tmy)
  local R = TANK_THREAT_RADIUS
  for dy = -R, R do
    local ny = tmy + dy
    if ny >= 0 and ny <= 255 then
      for dx = -R, R do
        local nx = tmx + dx
        if nx >= 0 and nx <= 255 then
          local dist = math.abs(dx) + math.abs(dy)
          if dist <= R then
            local proximity = 1.0 - dist / (R + 1)
            local penalty = TANK_THREAT_BASE * proximity
            if penalty > 0 then
              local k = U.mkey(nx, ny)
              tank_grid[k] = (tank_grid[k] or 0) + penalty
            end
          end
        end
      end
    end
  end
end

-- -------------------------------------------------------------------------
-- Internal: check pill state changes against prev_pills snapshot.
-- Sets pill_dirty = true if any pill changed.
-- -------------------------------------------------------------------------
local function check_pill_dirty(world)
  local seen = {}
  for id, pm in pairs(world.pills) do
    if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
      seen[id] = true
      local aq = anger_q(pm.anger)
      local prev = prev_pills[id]
      if prev == nil
         or prev.owner ~= pm.owner
         or prev.health ~= pm.health
         or prev.anger_q ~= aq
         or prev.mx ~= pm.mx
         or prev.my ~= pm.my then
        pill_dirty = true
        return  -- one change is enough, skip rest
      end
    end
  end
  -- Check for pills that disappeared or became friendly/dead
  for id in pairs(prev_pills) do
    if not seen[id] then
      pill_dirty = true
      return
    end
  end
end

-- -------------------------------------------------------------------------
-- Internal: check terrain changes near any active pill.
-- -------------------------------------------------------------------------
local function check_terrain_dirty(world)
  local tc = changes.terrain
  if #tc == 0 then return end

  local R = C.PILL_RANGE_MAP
  for idx = 1, #tc do
    local key = tc[idx]
    local tx = key % 256
    local ty = key // 256
    for _, pm in pairs(world.pills) do
      if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
        if math.abs(tx - pm.mx) <= R and math.abs(ty - pm.my) <= R then
          pill_dirty = true
          return  -- one hit is enough
        end
      end
    end
  end
end

-- -------------------------------------------------------------------------
-- Internal: snapshot current pill state into prev_pills.
-- -------------------------------------------------------------------------
local function snapshot_pills(world)
  prev_pills = {}
  for id, pm in pairs(world.pills) do
    if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
      prev_pills[id] = {
        owner = pm.owner,
        health = pm.health,
        anger_q = anger_q(pm.anger),
        mx = pm.mx,
        my = pm.my,
      }
    end
  end
end

-- -------------------------------------------------------------------------
-- M.update(state, world, info)
-- Rebuild threat grids as needed.  Call once per tick after
-- danger.update() and before percept.update().
-- -------------------------------------------------------------------------
function M.update(state, world, info)
  -- Check if pill grid needs rebuilding
  if not pill_dirty then
    check_pill_dirty(world)
  end
  if not pill_dirty then
    check_terrain_dirty(world)
  end

  -- Consume terrain changes (must happen regardless of dirty flag)
  local tc = changes.terrain
  for idx = #tc, 1, -1 do tc[idx] = nil end

  -- Rebuild pill grid only when dirty
  if pill_dirty then
    metrics.inc("threat_rebuild")
    pill_grid = {}
    for _, pm in pairs(world.pills) do
      if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
        stamp_pill(pm)
      end
    end
    snapshot_pills(world)
    pill_dirty = false
  end

  -- Always rebuild tank grid (cheap, tanks move every tick)
  tank_grid = {}
  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_TANK and (ob.info & OBJECT_HOSTILE) ~= 0 then
      local omx = ob.x >> 8
      local omy = ob.y >> 8
      stamp_tank(omx, omy)
    end
  end
end

-- -------------------------------------------------------------------------
-- M.pill_at(mx, my) — pill-only threat (O(1) lookup)
-- Use for A* terrain cost where tank positions shouldn't warp long-range
-- pathfinding.
-- -------------------------------------------------------------------------
function M.pill_at(mx, my)
  return pill_grid[U.mkey(mx, my)] or 0
end

-- -------------------------------------------------------------------------
-- M.at(mx, my) — combined pill + tank threat (O(1) lookup)
-- Does NOT include shell trajectories — those are summed by danger.lua
-- at read time since they live in a separate fast-expiring map.
-- -------------------------------------------------------------------------
function M.at(mx, my)
  local k = U.mkey(mx, my)
  return (pill_grid[k] or 0) + (tank_grid[k] or 0)
end

-- -------------------------------------------------------------------------
-- M.reset() — clear all state (called on Brain.open)
-- -------------------------------------------------------------------------
function M.reset()
  pill_grid = {}
  tank_grid = {}
  prev_pills = {}
  pill_dirty = true  -- force rebuild on first tick
end

return M
