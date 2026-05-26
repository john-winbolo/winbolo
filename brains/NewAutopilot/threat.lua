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
-- M.pill_grid removed: pill danger values live in C (s_pill_grid_c in na_threat.c).
-- Access via na_threat.pill_grid_at(k), threat.pill_at(mx,my), threat.at(mx,my).
M.tank_grid = {}
-- Per-pill contribution cache: M.pill_contrib[pill_pos_key][tile_key] =
-- this pill's final stamped penalty at that tile, after tree/terrain/LOS
-- reductions. Same units as pill_grid. Lets attack-side code subtract a
-- specific pill's danger from cumulative cost when the bot is committed
-- to killing it. Keyed by mkey(pm.mx, pm.my) since pills with health > 0
-- and hostile/neutral owner are stationary.
M.pill_contrib = {}  -- per-pill contribution sub-tables (still Lua, used by pillcontrib_add_all)
-- Per-tile count of distinct hostile/neutral pills that can fire on this
-- tile. Rebuilt only when pills change (via pill_dirty). Used by Scan A
-- in attack.evaluate_pill_difficulty for pill-take maneuver scoring,
-- where one extra incoming shot can knock the tank off target.
--
-- Strictly Euclidean disk at PILL_FIRE_RANGE (= 9), NOT the
-- PILL_RANGE_MAP (= 10) buffered radius used by the danger-smoothing
-- pill_grid. We want "tile literally within the fire cone of N pills",
-- not the +1 tile danger fade.
-- M.coverage_grid removed: coverage counts live in C (s_cov_grid_c in na_threat.c).
-- Access via na_threat.cov_grid_at(k) or threat.coverage_at(mx,my).

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
  [C.T_RIVER] = true,
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

-- Terrain factor cache lives entirely on the C side (na_threat).
-- We keep these locals as truthy/nil sentinels so the existing
-- "lazy first build" check (`if _terrain_mult == nil then rebuild`)
-- still works — but the actual data lookups happen via na_threat
-- inside stamp_pill (also in C).
--
-- Configure runs once and caches every tunable + the disk geometry
-- on the C side. Cloners tweaking constants.lua just rerun configure
-- (called automatically on first rebuild_terrain_factors).
local function na_threat_configure_once()
  na_threat.configure({
    PILL_RANGE_MAP           = C.PILL_RANGE_MAP,
    MIN_TREEHIDE_DIST_MAP    = C.MIN_TREEHIDE_DIST_MAP,
    PILL_DANGER_EDGE_FALLOFF = C.PILL_DANGER_EDGE_FALLOFF,
    PILL_DANGER_BASE         = C.PILL_DANGER_BASE,
    PILL_DANGER_ANGER        = C.PILL_DANGER_ANGER,
    -- Damaged-pill stamp scaling. C uses the formula:
    --   mult = HP_DAMAGE_FLOOR + HP_DAMAGE_SCALE * (hp / FULL_HP)^HP_DAMAGE_EXP
    -- Default (0.60 floor, 0.40 scale, 0.6 exp) gives:
    --   hp=1 → 0.69, hp=5 → 0.81, hp=10 → 0.90, hp=15 → 1.00.
    -- "It's still a pill that shoots" — wounded penalty is real but mild;
    -- a 1-HP pill is still 69% as scary as a fresh one. Replaces the old
    -- LOW_HP1_MULT / LOW_HP2_MULT ladder which only touched hp==1/hp==2.
    HP_DAMAGE_FLOOR          = 0.60,
    HP_DAMAGE_SCALE          = 0.40,
    HP_DAMAGE_EXP            = 0.6,
    PILLS_MAX_HEALTH         = C.PILLS_MAX_HEALTH,
    TREE_FULL_HIDE_MULT      = 0.1,
    TREE_PARTIAL_HIDE_MULT   = 0.7,
    FOREST_TERRAIN_MULT      = 1.5,
    HAZARD_NEIGHBOR_MULT     = 1.5,
    T_BUILDING  = C.T_BUILDING,
    T_RIVER     = C.T_RIVER,
    T_SWAMP     = C.T_SWAMP,
    T_FOREST    = C.T_FOREST,
    T_RUBBLE    = C.T_RUBBLE,
    T_HALFBUILD = C.T_HALFBUILD,
    T_DEEPSEA   = C.T_DEEPSEA,
    TERRAIN_SPEED  = C.TERRAIN_SPEED,
    HAZARD_TERRAIN = HAZARD_TERRAIN,
  })
end

-- Single-tile recompute — passes through to C, which handles the
-- 5-tile cross update internally. Caller no longer needs to invoke
-- this once per cardinal neighbor.
local function compute_terrain_factor_at(mx, my)
  na_threat.terrain_update_around(mx, my)
end

-- Full first-time build runs on the C side: ~0.5 ms instead of ~20 ms.
local function rebuild_terrain_factors()
  local _t0 = clock_us()
  na_threat_configure_once()
  local _t1 = clock_us()
  na_threat.terrain_rebuild()
  local _t2 = clock_us()
  -- Gated: in a live game (no --perf-log) this diagnostic line skips
  -- the file write entirely. Synchronous io.open is multi-hundred-µs
  -- on Windows and can spike a tick.
  if BRAIN_PROFILE_LOG then
    local f = io.open((_G.DEBUG_SESSION_DIR or ".") .. "/optimize.log", "a")
    if f then
      f:write(string.format(
        "  [diag] rebuild_terrain_factors: configure=%.2f ms rebuild=%.2f ms\n",
        (_t1 - _t0) / 1000, (_t2 - _t1) / 1000))
      f:close()
    end
  end
  -- Truthy sentinels so `if _terrain_mult == nil` skips re-running.
  _terrain_mult  = true
  _terrain_trees = true
end

-- Stamp one pill directly into C-side s_pill_grid_c and s_cov_grid_c.
-- No Lua table I/O for pill_grid or coverage — zero GC pressure.
-- Returns a contrib table stored in M.pill_contrib for attack-side use.
local function stamp_pill(pm)
  local pill_key = pm.my * 256 + pm.mx
  M.pill_contrib[pill_key] = na_threat.stamp_pill(
    pm.mx, pm.my,
    pm.anger or 0, pm.health or 0)
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

-- Pre-allocated scratch lists for passing pill positions to
-- na_threat.apply_occlusion_all. Sized to max pills (16); avoids
-- per-rebuild table allocation.
local _hp_mx = {}
local _hp_my = {}
local _fp_mx = {}
local _fp_my = {}

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
-- Reduction per occluder in line of sight:
--   wall:           20% per tile
--   tree:           10% per tile
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
          local reduction = effective_walls * 0.20 + t_total * 0.10 + f_total * 0.40
          if reduction > 0.80 then reduction = 0.80 end
          if reduction > 0 then
            local factor = 1.0 - reduction
            M.pill_grid[k] = cur * factor
            -- Mirror the same LOS reduction onto this pill's per-tile
            -- contribution cache so attack-side subtractions match the
            -- value actually present in pill_grid.
            local contrib = M.pill_contrib[py * 256 + px]
            if contrib and contrib[k] then
              contrib[k] = contrib[k] * factor
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
  -- na_threat.terrain_update_around handles the 5-tile cross internally
  -- (changed tile + 4 cardinal neighbors), so one call per change tile
  -- is enough. Cost is ~5 raw_tt + write per change, all C-side.
  local tc = changes.terrain
  for idx = 1, #tc do
    local key = tc[idx]
    local tx = key % 256
    local ty = key // 256
    na_threat.terrain_update_around(tx, ty)
  end
  for idx = #tc, 1, -1 do tc[idx] = nil end

  -- Rebuild pill grid only when dirty
  if M.pill_dirty then
    metrics.inc("threat_rebuild")
    local _t0 = clock_us()
    -- Zero C-side pill_grid and coverage arrays (memset, no Lua GC).
    na_threat.pill_rebuild_begin()
    -- Clear per-pill contributions; stamp_pill repopulates them.
    for k in pairs(M.pill_contrib) do M.pill_contrib[k] = nil end
    local _t_clear = clock_us() - _t0
    -- Pass 1: stamp raw danger from each pill into C-side arrays.
    -- coverage is now also C-side (s_cov_grid_c) — no Lua table needed.
    local _t_stamp0 = clock_us()
    local _stamp_pills = 0
    local hp_n = 0
    local fp_n = 0
    for _, pm in pairs(world.pills) do
      if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
        stamp_pill(pm)
        _stamp_pills = _stamp_pills + 1
        hp_n = hp_n + 1; _hp_mx[hp_n] = pm.mx; _hp_my[hp_n] = pm.my
      elseif pm.owner == "friendly" and pm.health > 0 then
        fp_n = fp_n + 1; _fp_mx[fp_n] = pm.mx; _fp_my[fp_n] = pm.my
      end
    end
    local _t_stamp = clock_us() - _t_stamp0
    -- Direct diagnostic line: write to optimize.log so we can see the
    -- subsection breakdown without needing print2 enabled.
    if BRAIN_PROFILE_LOG then
      local f = io.open((_G.DEBUG_SESSION_DIR or ".") .. "/optimize.log", "a")
      if f then
        f:write(string.format("  [diag] threat REBUILD pills=%d clear=%.2f stamp=%.2f\n",
          _stamp_pills, _t_clear / 1000, _t_stamp / 1000))
        f:close()
      end
    end
    -- Pass 2: apply LOS occlusion in C (reads/writes s_pill_grid_c directly).
    -- Lua fallback: apply_occlusion_to_pill (kept below) for debugging.
    local _t_occl0 = clock_us()
    na_threat.apply_occlusion_all(M.pill_contrib,
      _fp_mx, _fp_my, fp_n, _hp_mx, _hp_my, hp_n)
    local _t_occl = clock_us() - _t_occl0
    if BRAIN_PROFILE_LOG then
      local f = io.open((_G.DEBUG_SESSION_DIR or ".") .. "/optimize.log", "a")
      if f then
        f:write(string.format("  [diag] threat REBUILD occl=%.2f cov_pass coming\n", _t_occl / 1000))
        f:close()
      end
    end

    -- Pass 3: crossfire multiplier — multiply each tile by its coverage count.
    -- Done in C on s_pill_grid_c; no Lua table iteration.
    local _t_cov0 = clock_us()
    if C.CROSSFIRE_MULTIPLIER_ENABLED then
      na_threat.apply_crossfire()
    end
    local _t_cov = clock_us() - _t_cov0

    snapshot_pills(world)
    M.pill_dirty = false
    M.rebuilt_this_tick = true
    -- Sync C-side pill_grid/cov_grid into na_attack's arrays (memcpy).
    if na_attack then na_attack.sync_grids() end

    -- Per-section timing breakdown for diagnosing slow rebuilds. Only
    -- prints when the total exceeded ~5 ms (otherwise too noisy).
    if BRAIN_DEBUG_MODE then
      local _t_total = clock_us() - _t0
      if _t_total > 5000 then
        print2(string.format(
          "  threat REBUILD %.2f ms  pills=%d  clear=%.2f stamp=%.2f occl=%.2f cov+xfire=%.2f",
          _t_total / 1000, hp_n,
          _t_clear / 1000, _t_stamp / 1000, _t_occl / 1000, _t_cov / 1000))
      end
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
  return na_threat.pill_grid_at(my * 256 + mx)
end

-- -------------------------------------------------------------------------
-- M.coverage_at(mx, my) — number of distinct hostile/neutral pills that
-- can fire on this tile (O(1) lookup). Used by Scan A in the pill-take
-- difficulty scorer where one extra incoming shot can knock the tank
-- off-target mid-attack.
-- -------------------------------------------------------------------------
function M.coverage_at(mx, my)
  return na_threat.cov_grid_at(my * 256 + mx)
end

-- -------------------------------------------------------------------------
-- M.at(mx, my) — combined pill + tank threat (O(1) lookup)
-- Does NOT include shell trajectories — those are summed by danger.lua
-- at read time since they live in a separate fast-expiring map.
-- -------------------------------------------------------------------------
function M.at(mx, my)
  local k = my * 256 + mx
  return na_threat.pill_grid_at(k) + (M.tank_grid[k] or 0)
end

-- -------------------------------------------------------------------------
-- M.for_each_pill_danger(callback) — iterate all non-zero pill danger entries
-- callback(mx, my, value) called for each tile with danger > 0
-- -------------------------------------------------------------------------
function M.for_each_pill_danger(callback)
  na_threat.for_each_pill_danger(callback)
end

-- Pre-warm the C terrain factor cache (terrain_rebuild + configure).
-- Call from Brain.open() so the first game tick doesn't pay this cost.
function M.prewarm_terrain()
  if _terrain_mult == nil then
    rebuild_terrain_factors()
  end
end

-- -------------------------------------------------------------------------
-- M.reset() — clear all state (called on Brain.open)
-- -------------------------------------------------------------------------
function M.reset()
  -- pill_grid and coverage_grid live in C (s_pill_grid_c / s_cov_grid_c);
  -- pill_rebuild_begin() will zero them at the next rebuild.
  for k in pairs(M.tank_grid)  do M.tank_grid[k]  = nil end
  for k in pairs(M.prev_pills) do M.prev_pills[k] = nil end
  for k in pairs(M.prev_friendly_pills) do M.prev_friendly_pills[k] = nil end
  for k in pairs(M.prev_hostile_pills)  do M.prev_hostile_pills[k]  = nil end
  for k in pairs(M.prev_hostile_bases)  do M.prev_hostile_bases[k]  = nil end
  M.pill_dirty    = true  -- force rebuild on first tick
  M.overlay_dirty = true  -- force overlay rebuild on first tick
  _terrain_mult  = nil
  _terrain_trees = nil
end

return M
