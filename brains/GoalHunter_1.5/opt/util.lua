-- =========================================================================
-- GoalHunter/util.lua — coordinate conversion, trig, distance helpers
-- =========================================================================

local C       = require("constants")
local metrics = require("metrics")
local changes = require("changes")
local print2  = require("print2")
-- viz is required for line_walk's optional debug-overlay drawing.
-- It must come AFTER changes/metrics to keep the load order stable
-- (no circular requires; viz doesn't pull anything from util).
local viz     = require("viz")

local M = {}

function M.w2m(w)   return w >> 8          end
function M.m2w(m)   return (m << 8) | 0x80 end

-- Terrain cache: detect changes and notify pathfinder via changes.terrain.
-- Exposed as M.terrain_prev for consumers that need "what terrain have we
-- seen at tile X" (fog-of-war lookup). Mutated in place on reset so the
-- exposed reference stays valid.
local terrain_prev = {}   -- mkey -> last seen terrain type
M.terrain_prev = terrain_prev

function M.reset()
  for k in pairs(terrain_prev) do terrain_prev[k] = nil end
end

function M.ttype(mx, my)
  metrics.inc("get_terrain")
  local raw = get_terrain(mx, my)
  local tt  = raw & TERRAIN_MASK
  local key = my * 256 + mx
  local prev = terrain_prev[key]
  if prev ~= nil and prev ~= tt then
    changes.terrain[#changes.terrain + 1] = key
  end
  terrain_prev[key] = tt
  return tt
end

function M.traw(mx, my)
  metrics.inc("get_terrain")
  local raw = get_terrain(mx, my)
  local tt  = raw & TERRAIN_MASK
  local key = my * 256 + mx
  local prev = terrain_prev[key]
  if prev ~= nil and prev ~= tt then
    changes.terrain[#changes.terrain + 1] = key
  end
  terrain_prev[key] = tt
  return raw
end

function M.in_map(mx, my)
  return mx >= 0 and mx <= 255 and my >= 0 and my <= 255
end

function M.mkey(mx, my)
  if my == nil or mx == nil then
    error(string.format("mkey: nil argument (mx=%s, my=%s)\n%s",
          tostring(mx), tostring(my), debug.traceback()), 2)
  end
  return my * C.MAP_W + mx
end
function M.mkey_x(k)     return k % C.MAP_W       end
function M.mkey_y(k)     return k // C.MAP_W      end

-- Set a tile-block (retry/no-build) cooldown WITH a debug breadcrumb, so
-- "why is (x,y) blocked for Nt?" is one grep in print2_bot*.log instead of
-- guessing from the duration. `src` is a short tag naming the call site/reason.
-- The print2 line is stripped from opt/, so production is just the table write.
function M.set_blocked(state, key, until_tick, src)
  state.blocked = state.blocked or {}
  state.blocked[key] = until_tick
end

function M.mdist(mx1, my1, mx2, my2)
  return math.abs(mx1 - mx2) + math.abs(my1 - my2)
end

-- Chebyshev-style heuristic for 8-connected A*: consistent with diagonal moves
-- costing 1.41x a cardinal move. Returns an admissible estimate.
function M.hdist(mx1, my1, mx2, my2)
  local dx = math.abs(mx1 - mx2)
  local dy = math.abs(my1 - my2)
  return (dx + dy) + (1.41 - 2) * math.min(dx, dy)
end

function M.wdist(x1, y1, x2, y2)
  local dx, dy = x1 - x2, y1 - y2
  return math.floor(math.sqrt(dx * dx + dy * dy))
end

function M.mclamp(v)
  return v < 0 and 0 or (v > 255 and 255 or v)
end

-- Bolo-angle trig: 0=N, 64=E, 128=S, 192=W (0-255 wrapping)
-- Returns integer in [-128, +128]
function M.bsin(a)
  a = a & 0xFF
  return math.floor(math.sin(a * C.TWO_PI / 256) * 128 + 0.5)
end

function M.bcos(a)
  a = a & 0xFF
  return math.floor(math.cos(a * C.TWO_PI / 256) * 128 + 0.5)
end

-- Float-precision bolo-angle trig (no rounding, for smooth visuals and aiming)
function M.bsin_f(a)
  return math.sin(a * C.TWO_PI / 256)
end

function M.bcos_f(a)
  return math.cos(a * C.TWO_PI / 256)
end

-- Compute crosshair position: (x,y) in tile coords at gun_range from tank
function M.crosshair_at(tankx, tanky, direction, gun_range)
  local twx = tankx / 256.0
  local twy = tanky / 256.0
  local rad = direction * C.TWO_PI / 256
  return twx + math.sin(rad) * gun_range,
         twy - math.cos(rad) * gun_range
end

-- Float-precision aim_at: returns exact bolo angle (float, not rounded)
function M.aim_at_f(sx, sy, tx, ty)
  return math.atan(tx - sx, -(ty - sy)) * 256 / C.TWO_PI
end

-- Bolo angle from (sx,sy) toward (tx,ty)
function M.aim_at(sx, sy, tx, ty)
  return math.floor(math.atan(tx - sx, -(ty - sy)) * 256 / C.TWO_PI + 0.5) & 0xFF
end

-- Signed angular difference a->b in [-128, +127]
-- Positive = b is clockwise of a
function M.adiff(a, b)
  local d = (b - a) % 256
  return d >= 128 and d - 256 or d
end

function M.is_water(tt)
  return tt == C.T_RIVER or tt == C.T_DEEPSEA
end

-- Aim-correction → key bits. For a correction (signed brads, +ve = need
-- to turn right), returns (hold_bit, tap_bit) suitable for OR'ing onto
-- the keys / taps masks. hold_thr is the magnitude above which we hold
-- the turn key continuously; tap_thr is where we switch to a single tap;
-- below tap_thr both bits are 0. Centralises a 4-line if-elseif-elseif-
-- elseif pattern that appeared in 8+ sites in steering.lua.
function M.aim_turn_bits(corr, hold_thr, tap_thr)
  if     corr >  hold_thr then return KEY_TURNRIGHT, 0
  elseif corr < -hold_thr then return KEY_TURNLEFT,  0
  elseif corr >  tap_thr  then return 0,             KEY_TURNRIGHT
  elseif corr < -tap_thr  then return 0,             KEY_TURNLEFT
  end
  return 0, 0
end

-- True if every intermediate tile between (x0,y0) and (x1,y1) is water.
-- Lets a boat-shell traveling over the corridor reach (x1,y1) without
-- being absorbed mid-flight by terrain. Hoisted from attack.lua and
-- steering.lua where it was duplicated verbatim.
function M.water_corridor_to(x0, y0, x1, y1)
  local blocked = M.bresenham(x0, y0, x1, y1, function(cx, cy)
    if not M.is_water(M.ttype(cx, cy)) then return true end
  end)
  return not blocked
end

-- Walk a Bresenham line from (x0,y0) to (x1,y1), calling fn(cx,cy) for each
-- intermediate tile (excluding start and end points).
-- If fn returns a non-nil, non-false value, stops early and returns that value.
-- Returns nil if the walk completes without early stop.
function M.bresenham(x0, y0, x1, y1, fn)
  local dx = math.abs(x1 - x0)
  local dy = math.abs(y1 - y0)
  local sx = x0 < x1 and 1 or -1
  local sy = y0 < y1 and 1 or -1
  local err = dx - dy
  local cx, cy = x0, y0
  while true do
    if cx == x1 and cy == y1 then break end
    local e2 = 2 * err
    if e2 > -dy then err = err - dy; cx = cx + sx end
    if e2 <  dx then err = err + dx; cy = cy + sy end
    if cx == x1 and cy == y1 then break end
    local result = fn(cx, cy)
    if result then return result end
  end
  return nil
end

-- Walk a precise float-coordinate line from (fx0,fy0) to (fx1,fy1), stepping
-- 0.5 units at a time, calling fn(tile_x, tile_y) for each unique tile visited.
-- This is symmetric (no Bresenham bias) and useful for LOS / cover checks
-- where rounding artifacts cause asymmetric results.
-- If fn returns a non-nil, non-false value, stops early and returns that value.
--
-- Optional `viz_color` = {r, g, b, a} draws an overlay_rect on each
-- visited tile via viz.rect. When viz_color is set, viz_id MUST also
-- be supplied (the V-dialog checkbox the rect is gated by); calling
-- with viz_color and no viz_id raises a Lua error from viz.rect.
function M.line_walk(fx0, fy0, fx1, fy1, fn, viz_color, viz_id)
  local ddx = fx1 - fx0
  local ddy = fy1 - fy0
  local dlen = math.sqrt(ddx * ddx + ddy * ddy)
  if dlen < 0.01 then return nil end
  local steps = math.ceil(dlen * 2)  -- 2 samples per tile = 0.5-unit steps
  local stepx, stepy = ddx / steps, ddy / steps
  local visited = {}
  for i = 0, steps do
    local fx = fx0 + stepx * i
    local fy = fy0 + stepy * i
    local bx = math.floor(fx)
    local by = math.floor(fy)
    local k = by * 256 + bx
    if not visited[k] then
      visited[k] = true
      local result = fn(bx, by)
      if result then return result end
    end
  end
  return nil
end

-- =========================================================================
-- nav_turn_speed — shared turn + proportional speed control
--
-- Replaces the repeated pattern of:
--   if corr > 10  → hold turn   elseif corr > 2  → tap turn
--   if abs_corr < 32 → accel    elseif abs_corr > 64 → brake
--
-- The old 32°/64° thresholds left a dead band where the tank coasted,
-- causing overshoots at A* waypoint turns.  This version ramps speed
-- proportionally: full speed when well-aimed, smooth deceleration as
-- the turn angle increases, hard brake when facing away.
--
-- max_speed: terrain-dependent cap (default 48).  Callers in combat
--   rushes can pass a higher value.
-- min_speed: floor for braking (default 4).  ws_retreat passes lower.
-- =========================================================================
function M.nav_turn_speed(corr, speed, max_speed, min_speed)
  local keys, taps = 0, 0
  local abs_corr = math.abs(corr)
  max_speed = max_speed or 48
  min_speed = min_speed or 4

  -- Turning: 3-tier hold/tap/none
  if     corr >  10 then keys = keys | KEY_TURNRIGHT
  elseif corr < -10 then keys = keys | KEY_TURNLEFT
  elseif corr >   2 then taps = taps | KEY_TURNRIGHT
  elseif corr <  -2 then taps = taps | KEY_TURNLEFT
  end

  -- Speed: proportional to aim quality
  --   < 16°  : accelerate up to max_speed (was: full throttle ignoring cap)
  --   16-80° : linear ramp from max_speed down to min_speed
  --   > 80°  : hard brake
  if abs_corr < 16 then
    -- Honor max_speed even when well-aimed. Without this, callers that
    -- want a slow creep (e.g. centering on a tile) get the engine's
    -- default ~48 wu/tick instead of their requested cap.
    if speed > max_speed + 1 then
      keys = keys | KEY_SLOWER
    elseif speed < max_speed then
      keys = keys | KEY_FASTER
    end
  elseif abs_corr > 80 then
    if speed > min_speed then keys = keys | KEY_SLOWER end
  else
    local factor = 1.0 - (abs_corr - 16) / 64.0
    local desired = math.max(min_speed, math.floor(factor * max_speed))
    if speed > desired + 4 then
      keys = keys | KEY_SLOWER
    elseif speed < desired then
      keys = keys | KEY_FASTER
    end
  end

  return keys, taps
end

-- =========================================================================
-- is_placeable — can a pillbox be placed at (mx, my)?
-- Checks terrain type and ensures no existing pill or base occupies it.
-- =========================================================================
function M.is_placeable(mx, my, world)
  local tt = M.ttype(mx, my)
  if not (tt == C.T_GRASS or tt == C.T_ROAD or tt == C.T_RUBBLE
          or tt == C.T_SWAMP or tt == C.T_CRATER or tt == C.T_FOREST) then
    return false
  end
  local k = my * C.MAP_W + mx
  if world.pill_at[k] then return false end
  if world.base_at[k] then return false end
  return true
end

-- =========================================================================
-- los_coverage — count clear LOS tiles from (mx, my) by sampling directions.
-- Traces rays outward; stops at walls, forests, and map edges.
-- Returns total count of clear tiles across all rays.
-- =========================================================================
function M.los_coverage(mx, my, num_dirs, max_range)
  num_dirs  = num_dirs  or 8
  max_range = max_range or 8
  local total = 0
  for i = 0, num_dirs - 1 do
    local angle = i * C.TWO_PI / num_dirs
    local dx = math.cos(angle)
    local dy = math.sin(angle)
    for r = 1, max_range do
      local cx = math.floor(mx + dx * r + 0.5)
      local cy = math.floor(my + dy * r + 0.5)
      if not M.in_map(cx, cy) then break end
      local tt = M.ttype(cx, cy)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then break end
      total = total + 1
    end
  end
  return total
end

return M
