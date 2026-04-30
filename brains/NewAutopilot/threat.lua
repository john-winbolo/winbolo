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
local print2  = require("print2")
-- clock_us is registered as a global by braincore.c (l_clock_us). Cache
-- it locally so the timing prints below don't pay a global lookup per
-- tick. Falls back to a no-op if for some reason it isn't available.
local clock_us = clock_us or function() return 0 end

local M = {}

-- Sparse grids: only tiles near threat sources get entries.
-- M.pill_grid[mkey]  = pill threat value
-- M.tank_grid[mkey]  = tank threat value
-- Live on M (not closure-private locals) so the state serializer can include
-- them in snapshots and replays see the same threat grids.
M.pill_grid = {}
M.tank_grid = {}
-- Per-tile count of distinct hostile/neutral pills that can fire on this
-- tile. Rebuilt only when pills change (via pill_dirty). Used by Scan A
-- in attack.evaluate_pill_difficulty for pill-take maneuver scoring,
-- where one extra incoming shot can knock the tank off target.
--
-- Strictly Euclidean disk at PILL_FIRE_RANGE (= 9), NOT the
-- PILL_RANGE_MAP (= 10) buffered radius used by the danger-smoothing
-- pill_grid. We want "tile literally within the fire cone of N pills",
-- not the +1 tile danger fade.
M.coverage_grid = {}

-- Precomputed Euclidean disk stamp at PILL_RANGE_MAP. Built once at
-- module load. Stored as four parallel flat arrays for tight indexed
-- access in stamp_pill's inner loop:
--
--   DISK_DX[i]  = column offset relative to the pill center
--   DISK_DY[i]  = row offset
--   DISK_OFF[i] = dy * 256 + dx, the index for the proximity / fullhide
--                 caches (saves a multiply-add per iteration)
--   DISK_LEN    = number of in-disk tiles (~314 for R=10)
--
-- Same radius (= PILL_RANGE_MAP = 10) and shape (Euclidean) the
-- original threat.lua used, so the crossfire multiplier on pill_grid
-- and the exposed coverage_at() return identical values. No
-- pathfinding behavior change vs. the original code.
local DISK_DX  = {}
local DISK_DY  = {}
local DISK_OFF = {}
local DISK_LEN = 0
do
  local R = C.PILL_RANGE_MAP
  local R2 = R * R
  for dy = -R, R do
    for dx = -R, R do
      if dx * dx + dy * dy <= R2 then
        DISK_LEN = DISK_LEN + 1
        DISK_DX[DISK_LEN]  = dx
        DISK_DY[DISK_LEN]  = dy
        DISK_OFF[DISK_LEN] = dy * 256 + dx
      end
    end
  end
end

-- Dirty-flag state for pill grid: only rebuild when something changed.
-- M.prev_pills[id] = {owner, health, anger_q, mx, my}
M.prev_pills = {}
M.pill_dirty = true  -- force first build

-- Dirty-flag state for the C overlay grid (driven from init.lua):
-- All live pills + hostile bases are stamped as impassable / expensive.
-- Rebuild when any pill's alive-state or any base's owner changes.
-- Snapshot values are the minimum fields that affect the stamps:
--   prev_friendly_pills[id] = {mx, my}       -- owner "friendly" & health>0
--   prev_hostile_pills[id]  = {mx, my}       -- owner "hostile"/"neutral" & health>0
--   prev_hostile_bases[id]  = {mx, my}       -- owner "hostile"
M.prev_friendly_pills = {}
M.prev_hostile_pills  = {}
M.prev_hostile_bases  = {}
M.overlay_dirty       = true  -- force first build

-- -------------------------------------------------------------------------
-- Constants for tank threat layer
-- -------------------------------------------------------------------------
local TANK_THREAT_RADIUS = 6    -- tiles around each enemy tank
local TANK_THREAT_BASE   = 10   -- threat value at point-blank

-- Quantize anger to 4 levels so tiny decay doesn't trigger rebuild.
-- Anger decays over ~3000 ticks; 4 levels = change every ~750 ticks.
local function anger_q(a)
  return math.floor((a or 0) * 20)  -- 20 buckets = ~5% steps, finer granularity
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

-- Count occluding tiles between (x0,y0) and (x1,y1) along the LOS line.
-- Returns: wall_count, tree_count, friendly_pill_count
-- Skips the endpoint tiles (the pill itself and the target tile).
local function count_occlusion_between(x0, y0, x1, y1, friendly_pill_set)
  local wall_count = 0
  local tree_count = 0
  local fpill_count = 0
  U.bresenham(x0, y0, x1, y1, function(cx, cy)
    if (cx == x0 and cy == y0) or (cx == x1 and cy == y1) then return end
    if U.in_map(cx, cy) then
      local tt = raw_tt(cx, cy)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
        wall_count = wall_count + 1
      elseif tt == C.T_FOREST then
        tree_count = tree_count + 1
      end
      if friendly_pill_set then
        local k = U.mkey(cx, cy)
        if friendly_pill_set[k] then
          fpill_count = fpill_count + 1
        end
      end
    end
  end)
  return wall_count, tree_count, fpill_count
end

-- -------------------------------------------------------------------------
-- Internal: stamp pill threat into pill_grid for one pill
-- -------------------------------------------------------------------------
local HAZARD_TERRAIN = {
  [C.T_RIVER] = true, [C.T_DEEPSEA] = true,
  [C.T_RUBBLE] = true, [C.T_SWAMP] = true,
}

-- ── Precomputed per-offset disk lookups ──
--
-- Indexed by dy * 256 + dx for (dx, dy) inside the PILL_RANGE_MAP disk.
-- Built once at module load.
--
--   _proximity_cache[off]   = 1 - PILL_DANGER_EDGE_FALLOFF * (sqrt(dx*dx + dy*dy) / R)
--                              the radial falloff term in stamp_pill;
--                              identical for every pill, so a pure
--                              function of (dx, dy). Was previously
--                              1 - d / (R + 1), which dropped to ~9%
--                              at the rim and made boundary tiles read
--                              as nearly-safe; the rim is still in the
--                              pill's range, so we stay strong out there.
--                              Now: 1.0 at center → (1 - PILL_DANGER_EDGE_FALLOFF)
--                              at the rim (default 0.25 → 75% strength
--                              at the rim).
--   _tree_fullhide_cache[off] = true if sqrt(dx*dx + dy*dy) >= MIN_TREEHIDE_DIST_MAP
--                               (the "far enough from the pill that
--                               tree cover gives the full 0.1× hide
--                               instead of the 0.7× partial") test.
--                               Sparse — absent means partial range.
local _proximity_cache    = {}
local _tree_fullhide_cache = {}
do
  local R = C.PILL_RANGE_MAP
  local inv_R = 1.0 / R
  local edge_fall = C.PILL_DANGER_EDGE_FALLOFF
  local min_th = C.MIN_TREEHIDE_DIST_MAP
  for dy = -R, R do
    for dx = -R, R do
      local d = math.sqrt(dx * dx + dy * dy)
      local off = dy * 256 + dx
      _proximity_cache[off] = 1.0 - edge_fall * (d * inv_R)
      if d >= min_th then
        _tree_fullhide_cache[off] = true
      end
    end
  end
end

-- ── Precomputed terrain factor grids ──
--
-- stamp_pill needs two terrain-only values per tile:
--   1. A speed/hazard multiplier:  (16/terrain_speed) * (1.5 if any
--      cardinal neighbor is river/swamp/rubble/deepsea).
--   2. A boolean: is this tile a "in trees" tile (matches engine's
--      utilIsTankInTrees — center + 4 cardinals all forest).
--
-- Both depend only on map terrain. The full grid is built once on the
-- first M.update, then maintained INCREMENTALLY: every entry in
-- changes.terrain triggers a recompute of the changed tile plus its
-- 4 cardinal neighbors (the only tiles whose factors can possibly
-- depend on the changed tile — both the hazard-neighbor check and
-- tile_in_trees only read cardinal neighbors). So a single wall
-- being destroyed costs 5 tile recomputes, not 65k.
local _terrain_mult  = nil  -- mkey -> number (multiplier per tile)
local _terrain_trees = nil  -- mkey -> true (sparse — absent = not in trees)

-- Recompute the cached factors for one tile. Reads the live terrain
-- via raw_tt — caller must invoke this for every tile whose terrain
-- changed AND for each of that tile's 4 cardinal neighbors.
local function compute_terrain_factor_at(mx, my)
  local tt = raw_tt(mx, my)
  local spd = C.TERRAIN_SPEED[tt] or 3
  local m = 1.0
  if spd > 0 then m = 16 / spd end
  -- Forest: override speed-based danger. Trees are slow but provide
  -- cover (pills can't see you) and destroy to grass (fast). Treat
  -- forest danger exposure like slightly-worse-than-grass, not 2.67×.
  if tt == C.T_FOREST then m = 1.5 end
  if (mx > 0   and HAZARD_TERRAIN[raw_tt(mx - 1, my)])
     or (mx < 255 and HAZARD_TERRAIN[raw_tt(mx + 1, my)])
     or (my > 0   and HAZARD_TERRAIN[raw_tt(mx, my - 1)])
     or (my < 255 and HAZARD_TERRAIN[raw_tt(mx, my + 1)]) then
    m = m * 1.5
  end
  local k = my * 256 + mx
  _terrain_mult[k] = m
  if tile_in_trees(mx, my) then
    _terrain_trees[k] = true
  else
    _terrain_trees[k] = nil
  end
end

-- Full first-time build: walk the entire map once. Only called on the
-- very first M.update (or after M.reset). After that the incremental
-- 5-tile updates in M.update keep the grids in sync.
local function rebuild_terrain_factors()
  _terrain_mult  = {}
  _terrain_trees = {}
  for my = 0, 255 do
    for mx = 0, 255 do
      compute_terrain_factor_at(mx, my)
    end
  end
end

-- Stamp one pill into pill_grid AND simultaneously update the coverage
-- table (one entry per disk tile, regardless of penalty value) so the
-- separate cov pass in M.update can be skipped.
local function stamp_pill(pm, coverage)
  local px, py = pm.mx, pm.my
  local anger = pm.anger or 0
  local base_strength = C.PILL_DANGER_BASE + C.PILL_DANGER_ANGER * anger
  -- Low-HP pills hit less hard (one or two more shots and they're
  -- gone) — discount their stamped danger across the whole disk so
  -- the bot is willing to push closer when an easy kill is in reach.
  local hp = pm.health or 0
  if hp == 1 then
    base_strength = base_strength * 0.8
  elseif hp == 2 then
    base_strength = base_strength * 0.9
  end
  local pill_grid = M.pill_grid
  local mult_grid = _terrain_mult
  local tree_grid = _terrain_trees
  local prox_cache = _proximity_cache
  local fullhide   = _tree_fullhide_cache
  local disk_dx  = DISK_DX
  local disk_dy  = DISK_DY
  local disk_off = DISK_OFF
  local n        = DISK_LEN

  for i = 1, n do
    local nx = px + disk_dx[i]
    local ny = py + disk_dy[i]
    if nx >= 0 and nx <= 255 and ny >= 0 and ny <= 255 then
      local k = ny * 256 + nx

      -- Coverage: count this pill regardless of terrain/penalty value.
      -- coverage_at() consumers want raw "how many pills can fire here".
      coverage[k] = (coverage[k] or 0) + 1

      local off = disk_off[i]
      local penalty = base_strength * prox_cache[off]

      -- Tree cover: reduce penalty if this tile is in trees. Whether
      -- the offset is "far enough" for full cover is a pure function
      -- of (dx, dy), so it's a precomputed lookup.
      if tree_grid[k] then
        if fullhide[off] then
          penalty = penalty * 0.1
        else
          penalty = penalty * 0.7
        end
      end

      -- LOS occlusion is applied in a second pass below (apply_occlusion_to_pill).

      -- Precomputed terrain multiplier:
      --   (16 / terrain_speed) * (1.5 if any cardinal hazard neighbor)
      penalty = penalty * mult_grid[k]

      if penalty > 0 then
        pill_grid[k] = (pill_grid[k] or 0) + penalty
      end
    end
  end
end

-- ── Precomputed Bresenham predecessor table ──
--
-- For each (dx, dy) integer offset inside the PILL_RANGE_MAP disk, store:
--   1. Its Bresenham predecessor offset (pdx, pdy) — the tile one step
--      back toward the origin along the Bresenham line from (0,0).
--   2. A topological order (Chebyshev-distance ascending) so a single
--      pass can compute LOS occlusion via dynamic programming:
--         counts(dx, dy) = counts(pred) + obstruction_at(pred)
--      Each tile is visited O(1) times instead of running a full
--      Bresenham trace per tile.
--
-- 21×21 = 441 disk slots; ~314 are inside the radius. Built once.
local PRED_DISK_SIZE = (C.PILL_RANGE_MAP * 2 + 1)  -- 21
local PRED_DISK_R    = C.PILL_RANGE_MAP            -- 10
local PRED_OFF_DX = {}  -- index by (dx + R) * SIZE + (dy + R)
local PRED_OFF_DY = {}
local PRED_ORDER   = {} -- list of (dx + R, dy + R) in Chebyshev order
do
  -- Reuse U.bresenham via a closure that captures the line tiles
  local function bresenham_line(x0, y0, x1, y1)
    local pts = {}
    U.bresenham(x0, y0, x1, y1, function(cx, cy)
      pts[#pts + 1] = { cx, cy }
    end)
    return pts
  end

  -- For each tile in the disk, compute its predecessor on the line
  -- from (0, 0) to itself.
  for dy = -PRED_DISK_R, PRED_DISK_R do
    for dx = -PRED_DISK_R, PRED_DISK_R do
      if dx * dx + dy * dy <= PRED_DISK_R * PRED_DISK_R then
        local idx = (dx + PRED_DISK_R) * PRED_DISK_SIZE + (dy + PRED_DISK_R)
        if dx == 0 and dy == 0 then
          PRED_OFF_DX[idx] = 0
          PRED_OFF_DY[idx] = 0
        else
          local line = bresenham_line(0, 0, dx, dy)
          -- The line ends at (dx, dy); predecessor is the second-to-last point.
          if #line >= 2 then
            local pred = line[#line - 1]
            PRED_OFF_DX[idx] = pred[1]
            PRED_OFF_DY[idx] = pred[2]
          else
            PRED_OFF_DX[idx] = 0
            PRED_OFF_DY[idx] = 0
          end
        end
      end
    end
  end

  -- Topological order: Chebyshev distance ascending. Within a Cheby
  -- ring, order is irrelevant since dependencies are at distance d-1.
  for d = 0, PRED_DISK_R do
    for dy = -PRED_DISK_R, PRED_DISK_R do
      for dx = -PRED_DISK_R, PRED_DISK_R do
        if math.max(math.abs(dx), math.abs(dy)) == d
           and dx * dx + dy * dy <= PRED_DISK_R * PRED_DISK_R then
          PRED_ORDER[#PRED_ORDER + 1] = (dx + PRED_DISK_R) * PRED_DISK_SIZE + (dy + PRED_DISK_R)
        end
      end
    end
  end
end

-- Per-pill scratch arrays for the occlusion DP. Reused across calls
-- to avoid allocator churn. Sized to the disk's bounding-box flat
-- index range.
local _occ_walls  = {}
local _occ_trees  = {}
local _occ_fpills = {}

-- -------------------------------------------------------------------------
-- Internal: apply LOS occlusion reduction to all tiles in a pill's radius.
--
-- Uses dynamic programming over a precomputed Bresenham predecessor
-- table. For each tile in the disk, the wall / tree / friendly_pill
-- counts along its line of sight to the pill are computed in O(1) by
-- adding the obstruction at the predecessor tile to the predecessor's
-- counts. Total work per pill is O(disk_size) instead of
-- O(disk_size × line_length).
--
-- Reduction per occluder in line of sight (unchanged):
--   wall:           20% per tile
--   tree:            3% per tile
--   friendly pill:  40% per tile
-- Capped at a maximum of 80% reduction.
-- -------------------------------------------------------------------------
local function apply_occlusion_to_pill(pm, friendly_pill_set)
  local px, py = pm.mx, pm.my
  local R = PRED_DISK_R
  local SIZE = PRED_DISK_SIZE

  -- Local aliases for inner loop perf.
  local walls  = _occ_walls
  local trees  = _occ_trees
  local fpills = _occ_fpills
  local pred_dx = PRED_OFF_DX
  local pred_dy = PRED_OFF_DY
  local order   = PRED_ORDER

  -- Walk tiles in Chebyshev-distance order so each tile's predecessor
  -- (always at smaller Cheby distance) is already filled in.
  --
  -- Every iteration writes walls[idx]/trees[idx]/fpills[idx] before any
  -- later iteration reads it as a predecessor — that means we don't
  -- need to clear the scratch arrays between calls. Stale data from
  -- previous pills is overwritten in dependency order.
  for i = 1, #order do
    local idx = order[i]
    local dx = (idx // SIZE) - R
    local dy = (idx %  SIZE) - R

    local w_total, t_total, f_total
    if dx == 0 and dy == 0 then
      w_total, t_total, f_total = 0, 0, 0
    else
      local pdx = pred_dx[idx]
      local pdy = pred_dy[idx]
      local pred_idx = (pdx + R) * SIZE + (pdy + R)
      local prev_w = walls[pred_idx]
      local prev_t = trees[pred_idx]
      local prev_f = fpills[pred_idx]
      -- Obstruction at the PREDECESSOR tile (not the current tile —
      -- count_occlusion_between's semantics exclude both endpoints, so
      -- we add the predecessor as an "in-between" tile when extending
      -- the line by one step). Out-of-map predecessor → 0 obstructions.
      local pnx = px + pdx
      local pny = py + pdy
      local add_w, add_t, add_f = 0, 0, 0
      if pnx >= 0 and pnx <= 255 and pny >= 0 and pny <= 255 then
        local tt = raw_tt(pnx, pny)
        if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
          add_w = 1
        elseif tt == C.T_FOREST then
          add_t = 1
        end
        -- Skip the pill itself when counting friendly pills as occluders.
        if friendly_pill_set and not (pdx == 0 and pdy == 0) then
          local pk = U.mkey(pnx, pny)
          if friendly_pill_set[pk] then
            add_f = 1
          end
        end
      end
      w_total = prev_w + add_w
      t_total = prev_t + add_t
      f_total = prev_f + add_f
    end

    -- Always store, even for out-of-map current tiles, so later
    -- iterations using this slot as a predecessor see fresh data.
    walls[idx]  = w_total
    trees[idx]  = t_total
    fpills[idx] = f_total

    -- Apply the reduction to pill_grid only if in map and beyond
    -- immediate neighbors (matches the original `d2 >= 4` skip).
    if dx * dx + dy * dy >= 4 then
      local nx = px + dx
      local ny = py + dy
      if nx >= 0 and nx <= 255 and ny >= 0 and ny <= 255 then
        local k = U.mkey(nx, ny)
        local cur = M.pill_grid[k]
        if cur and cur > 0 then
          -- The front-most wall (no walls between it and the pill) is
          -- fully exposed — the pill shoots it directly. It gets no
          -- wall occlusion benefit. Walls BEHIND other walls are
          -- legitimately shielded.
          local effective_walls = w_total
          local tt = raw_tt(nx, ny)
          if (tt == C.T_BUILDING or tt == C.T_HALFBUILD) and w_total == 0 then
            effective_walls = 0  -- front-most wall: no self-occlusion
          end
          local reduction = effective_walls * 0.20 + t_total * 0.03 + f_total * 0.40
          if reduction > 0.80 then reduction = 0.80 end
          if reduction > 0 then
            M.pill_grid[k] = cur * (1.0 - reduction)
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
          local dist = math.sqrt(dx * dx + dy * dy)
          if dist <= R then
            local proximity = 1.0 - dist / (R + 1)
            local penalty = TANK_THREAT_BASE * proximity
            if penalty > 0 then
              local k = U.mkey(nx, ny)
              M.tank_grid[k] = (M.tank_grid[k] or 0) + penalty
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
      local prev = M.prev_pills[id]
      if prev == nil
         or prev.owner ~= pm.owner
         or prev.health ~= pm.health
         or prev.anger_q ~= aq
         or prev.mx ~= pm.mx
         or prev.my ~= pm.my then
        M.pill_dirty = true
        return  -- one change is enough, skip rest
      end
    end
  end
  -- Check for pills that disappeared or became friendly/dead
  for id in pairs(M.prev_pills) do
    if not seen[id] then
      M.pill_dirty = true
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
          M.pill_dirty = true
          return  -- one hit is enough
        end
      end
    end
  end
end

-- -------------------------------------------------------------------------
-- M.check_overlay_dirty(world)
-- Set M.overlay_dirty if the friendly-pill set or hostile-base set has
-- changed since the last snapshot. Mirrors the pill-grid dirty pattern
-- but keyed on the state that init.lua stamps into the C overlay.
-- -------------------------------------------------------------------------
function M.check_overlay_dirty(world)
  if M.overlay_dirty then return end

  -- Friendly pills: membership = owner "friendly" and health > 0.
  local seen_fp = {}
  for id, pm in pairs(world.pills) do
    if pm.owner == "friendly" and pm.health > 0 then
      seen_fp[id] = true
      local prev = M.prev_friendly_pills[id]
      if prev == nil or prev.mx ~= pm.mx or prev.my ~= pm.my then
        M.overlay_dirty = true
        return
      end
    end
  end
  for id in pairs(M.prev_friendly_pills) do
    if not seen_fp[id] then
      M.overlay_dirty = true
      return
    end
  end

  -- Hostile/neutral pills: membership = (hostile or neutral) and health > 0.
  local seen_hp = {}
  for id, pm in pairs(world.pills) do
    if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
      seen_hp[id] = true
      local prev = M.prev_hostile_pills[id]
      if prev == nil or prev.mx ~= pm.mx or prev.my ~= pm.my then
        M.overlay_dirty = true
        return
      end
    end
  end
  for id in pairs(M.prev_hostile_pills) do
    if not seen_hp[id] then
      M.overlay_dirty = true  -- a hostile pill died or was captured
      return
    end
  end

  -- Hostile bases: membership = owner "hostile".
  local seen_hb = {}
  for id, b in pairs(world.bases) do
    if b.owner == "hostile" then
      seen_hb[id] = true
      local prev = M.prev_hostile_bases[id]
      if prev == nil or prev.mx ~= b.mx or prev.my ~= b.my then
        M.overlay_dirty = true
        return
      end
    end
  end
  for id in pairs(M.prev_hostile_bases) do
    if not seen_hb[id] then
      M.overlay_dirty = true
      return
    end
  end
end

-- -------------------------------------------------------------------------
-- M.snapshot_overlay(world)
-- Snapshot the current friendly-pill and hostile-base sets so the next
-- check_overlay_dirty can detect changes. Called by init.lua after it
-- rebuilds the C overlay.
-- -------------------------------------------------------------------------
function M.snapshot_overlay(world)
  for id in pairs(M.prev_friendly_pills) do M.prev_friendly_pills[id] = nil end
  for id in pairs(M.prev_hostile_pills)  do M.prev_hostile_pills[id]  = nil end
  for id in pairs(M.prev_hostile_bases)  do M.prev_hostile_bases[id]  = nil end
  for id, pm in pairs(world.pills) do
    if pm.owner == "friendly" and pm.health > 0 then
      M.prev_friendly_pills[id] = { mx = pm.mx, my = pm.my }
    elseif (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
      M.prev_hostile_pills[id] = { mx = pm.mx, my = pm.my }
    end
  end
  for id, b in pairs(world.bases) do
    if b.owner == "hostile" then
      M.prev_hostile_bases[id] = { mx = b.mx, my = b.my }
    end
  end
end

-- -------------------------------------------------------------------------
-- Internal: snapshot current pill state into prev_pills.
-- -------------------------------------------------------------------------
local function snapshot_pills(world)
  -- Mutate in place so any cached references stay valid.
  for id in pairs(M.prev_pills) do M.prev_pills[id] = nil end
  for id, pm in pairs(world.pills) do
    if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
      M.prev_pills[id] = {
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
M.rebuilt_this_tick = false

function M.update(state, world, info)
  M.rebuilt_this_tick = false
  -- Check if pill grid needs rebuilding
  if not M.pill_dirty then
    check_pill_dirty(world)
  end
  if not M.pill_dirty then
    check_terrain_dirty(world)
  end

  -- Ensure precomputed terrain factor grids exist (first call only).
  if _terrain_mult == nil then
    rebuild_terrain_factors()
  end

  -- Consume terrain changes (must happen regardless of dirty flag).
  -- For every changed tile, recompute its cached factors plus those of
  -- its 4 cardinal neighbors — the only other tiles whose values can
  -- depend on the changed tile. Cost is ~5 tile recomputes per change
  -- instead of a 65k full-grid rebuild.
  local tc = changes.terrain
  for idx = 1, #tc do
    local key = tc[idx]
    local tx = key % 256
    local ty = key // 256
    compute_terrain_factor_at(tx, ty)
    if tx > 0   then compute_terrain_factor_at(tx - 1, ty) end
    if tx < 255 then compute_terrain_factor_at(tx + 1, ty) end
    if ty > 0   then compute_terrain_factor_at(tx, ty - 1) end
    if ty < 255 then compute_terrain_factor_at(tx, ty + 1) end
  end
  for idx = #tc, 1, -1 do tc[idx] = nil end

  -- Rebuild pill grid only when dirty
  if M.pill_dirty then
    metrics.inc("threat_rebuild")
    local _t0 = clock_us()
    -- Mutate in place so any cached references stay valid.
    for k in pairs(M.pill_grid) do M.pill_grid[k] = nil end
    local _t_clear = clock_us() - _t0
    -- Pass 1: stamp raw danger from each pill AND build the coverage
    -- table in the same disk-walk pass. coverage[k] counts how many
    -- pills can fire on tile k regardless of penalty value — used by
    -- the crossfire multiplier below and exposed as M.coverage_grid
    -- for attack.evaluate_pill_difficulty (Scan A).
    local coverage = {}
    local _t_stamp0 = clock_us()
    for _, pm in pairs(world.pills) do
      if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
        stamp_pill(pm, coverage)
      end
    end
    M.coverage_grid = coverage
    local _t_stamp = clock_us() - _t_stamp0
    -- Build set of friendly pill tile keys for occlusion check
    local friendly_pill_set = {}
    for _, pm in pairs(world.pills) do
      if pm.owner == "friendly" and pm.health > 0 then
        friendly_pill_set[U.mkey(pm.mx, pm.my)] = true
      end
    end
    -- Pass 2: apply LOS occlusion (walls + trees + friendly pills)
    local _t_occl0 = clock_us()
    local _occl_pills = 0
    for _, pm in pairs(world.pills) do
      if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
        apply_occlusion_to_pill(pm, friendly_pill_set)
        _occl_pills = _occl_pills + 1
      end
    end
    local _t_occl = clock_us() - _t_occl0

    -- Pass 3 (xfire only): apply crossfire multiplier using the
    -- coverage table that was built in pass 1.
    local _t_cov0 = clock_us()
    if C.CROSSFIRE_MULTIPLIER_ENABLED then
      for k, v in pairs(M.pill_grid) do
        local n = coverage[k] or 1
        if n > 1 then
          M.pill_grid[k] = v * n
        end
      end
    end
    local _t_cov = clock_us() - _t_cov0

    snapshot_pills(world)
    M.pill_dirty = false
    M.rebuilt_this_tick = true

    -- Per-section timing breakdown for diagnosing slow rebuilds. Only
    -- prints when the total exceeded ~5 ms (otherwise too noisy).
    local _t_total = clock_us() - _t0
    if _t_total > 5000 then
      print2(string.format(
        "  threat REBUILD %.2f ms  pills=%d  clear=%.2f stamp=%.2f occl=%.2f cov+xfire=%.2f",
        _t_total / 1000, _occl_pills,
        _t_clear / 1000, _t_stamp / 1000, _t_occl / 1000, _t_cov / 1000))
    end
  end

  -- Always rebuild tank grid (cheap, tanks move every tick).
  -- Mutate in place so any cached references stay valid.
  for k in pairs(M.tank_grid) do M.tank_grid[k] = nil end
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
  return M.pill_grid[U.mkey(mx, my)] or 0
end

-- -------------------------------------------------------------------------
-- M.coverage_at(mx, my) — number of distinct hostile/neutral pills that
-- can fire on this tile (O(1) lookup). Used by Scan A in the pill-take
-- difficulty scorer where one extra incoming shot can knock the tank
-- off-target mid-attack.
-- -------------------------------------------------------------------------
function M.coverage_at(mx, my)
  return M.coverage_grid[U.mkey(mx, my)] or 0
end

-- -------------------------------------------------------------------------
-- M.at(mx, my) — combined pill + tank threat (O(1) lookup)
-- Does NOT include shell trajectories — those are summed by danger.lua
-- at read time since they live in a separate fast-expiring map.
-- -------------------------------------------------------------------------
function M.at(mx, my)
  local k = U.mkey(mx, my)
  return (M.pill_grid[k] or 0) + (M.tank_grid[k] or 0)
end

-- -------------------------------------------------------------------------
-- M.for_each_pill_danger(callback) — iterate all non-zero pill danger entries
-- callback(mx, my, value) called for each tile with danger > 0
-- -------------------------------------------------------------------------
function M.for_each_pill_danger(callback)
  for k, v in pairs(M.pill_grid) do
    if v > 0 then
      local mx = k & 255
      local my = k >> 8
      callback(mx, my, v)
    end
  end
end

-- -------------------------------------------------------------------------
-- M.reset() — clear all state (called on Brain.open)
-- -------------------------------------------------------------------------
function M.reset()
  -- Mutate in place so any cached references stay valid.
  for k in pairs(M.pill_grid)  do M.pill_grid[k]  = nil end
  for k in pairs(M.tank_grid)  do M.tank_grid[k]  = nil end
  for k in pairs(M.prev_pills) do M.prev_pills[k] = nil end
  for k in pairs(M.prev_friendly_pills) do M.prev_friendly_pills[k] = nil end
  for k in pairs(M.prev_hostile_bases)  do M.prev_hostile_bases[k]  = nil end
  M.pill_dirty    = true  -- force rebuild on first tick
  M.overlay_dirty = true  -- force overlay rebuild on first tick
  _terrain_mult  = nil
  _terrain_trees = nil
end

return M
