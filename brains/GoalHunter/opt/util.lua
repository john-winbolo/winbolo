local function __idiv(a,b) return math.floor(a/b) end
local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/util.lua — coordinate conversion, trig, distance helpers
-- =========================================================================

local C       = require("constants")
local metrics = require("metrics")
local changes = require("changes")
local print2  = require("print2")
local LP      = require("live_physics")
-- viz is required for line_walk's optional debug-overlay drawing.
-- It must come AFTER changes/metrics to keep the load order stable
-- (no circular requires; viz doesn't pull anything from util).
local viz     = require("viz")

local M = {}

function M.w2m(w)   return bit.rshift(w, 8)          end
function M.m2w(m)   return bit.bor((bit.lshift(m, 8)), 0x80) end

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
  local tt  = bit.band(raw, TERRAIN_MASK)
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
  local tt  = bit.band(raw, TERRAIN_MASK)
  local key = my * 256 + mx
  local prev = terrain_prev[key]
  if prev ~= nil and prev ~= tt then
    changes.terrain[#changes.terrain + 1] = key
  end
  terrain_prev[key] = tt
  return raw
end

-- M.ttype_peek(mx, my) — terrain type with NO change detection.
--
-- M.ttype and M.traw above are DETECTORS, not readers: each call primes
-- terrain_prev for that tile and, when the tile has moved since the last call
-- ON THAT TILE, pushes its key into changes.terrain — which threat.lua's
-- check_terrain_dirty turns into a pill-danger recompute around the tile.
-- The set of tiles the brain has "looked at", and when, is therefore part of
-- the brain's state.
--
-- That makes ttype/traw ILLEGAL from debug-only or viz-gated code. The
-- recorded (-brain-debug) brain runs blocks that lua_strip deletes from opt/,
-- so it primes tiles the production brain never touches, and the two play
-- different games. Found 2026-09-06: ONE call, in init.lua's
-- stop_predict_live overlay, reading the tank tile's terrain for a speed cap.
-- It moved threat.pill_at under the tank (6.92 -> 6.15 at brain tick 205 on
-- seed 4242), which moved imdanger, which moved the steering.
--
-- Debug overlays, print2 arguments and anything else inside
-- `if BRAIN_DEBUG_MODE` or `viz.is_on(...)` read terrain through THIS.
-- threat.lua has its own copy of the same idea (raw_tt) for the same reason.
function M.ttype_peek(mx, my)
  return bit.band(get_terrain(mx, my), TERRAIN_MASK)
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
function M.mkey_y(k)     return __idiv(k, C.MAP_W)      end

-- Set a tile-block (retry/no-build) cooldown WITH a debug breadcrumb, so
-- "why is (x,y) blocked for Nt?" is one grep in print2_bot*.log instead of
-- guessing from the duration. `src` is a short tag naming the call site/reason.
-- The print2 line is stripped from opt/, so production is just the table write.
function M.set_blocked(state, key, until_tick, src)
  state.blocked = state.blocked or {}
  state.blocked[key] = until_tick
end

-- ── Goal-shape blacklist ──
-- Written by init.lua's tick-budget kill catch-all, read by goal selection in
-- goals.lua. Lives here so both sides build the key identically: a goal whose
-- shape budget-killed the think THINK_KILL_TRIES ticks running is held off the
-- pool, otherwise pick_goal re-selects the exact shape that was killing us and
-- the bot livelocks (kills unwind the whole think, so nothing ever completes).
-- Keyed on (kind, target): target_id when the goal has one, else its tile.
function M.goal_blacklist_key(kind, target_id, mx, my)
  if kind == nil then return nil end
  if target_id ~= nil then return kind .. ":#" .. tostring(target_id) end
  return kind .. ":" .. tostring(mx or 0) .. "," .. tostring(my or 0)
end

-- Add a (kind,target) pair, purging expired entries on the way through — the
-- map only grows on budget kills, so this is the natural (and cheap) place.
function M.goal_blacklist_add(state, kind, target_id, mx, my, until_tick)
  local key = M.goal_blacklist_key(kind, target_id, mx, my)
  if not key then return nil end
  local bl = state._goal_blacklist
  if bl then
    local now = state.tick or 0
    for k, exp in pairs(bl) do
      if exp <= now then bl[k] = nil end
    end
  else
    bl = {}
    state._goal_blacklist = bl
  end
  bl[key] = until_tick
  return key
end

function M.goal_blacklisted(state, goal, now)
  local bl = state and state._goal_blacklist
  if not bl or not goal then return false end
  local key = M.goal_blacklist_key(goal.kind, goal.target_id, goal.mx, goal.my)
  if not key then return false end
  local exp = bl[key]
  if not exp then return false end
  if exp > (now or state.tick or 0) then return true end
  bl[key] = nil
  return false
end

function M.mdist(mx1, my1, mx2, my2)
  return math.abs(mx1 - mx2) + math.abs(my1 - my2)
end

-- True straight-line tile distance. Ranges that model a physical reach --
-- a pillbox's 8 tiles, gun range, the odds rings -- must use this, not mdist:
-- Manhattan calls a diagonal neighbour distance 2, so a mdist <= 8 test
-- silently excludes a band of tiles the pill can actually shoot.
function M.edist(mx1, my1, mx2, my2)
  local dx, dy = mx1 - mx2, my1 - my2
  return math.sqrt(dx * dx + dy * dy)
end

-- Chebyshev: diagonal neighbours are distance 1. "Beside" in the sense the
-- pill-spacing rule means -- the 8 tiles touching a pill are all distance 1.
function M.cdist(mx1, my1, mx2, my2)
  local dx, dy = math.abs(mx1 - mx2), math.abs(my1 - my2)
  return (dx > dy) and dx or dy
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
  a = bit.band(a, 0xFF)
  return math.floor(math.sin(a * C.TWO_PI / 256) * 128 + 0.5)
end

function M.bcos(a)
  a = bit.band(a, 0xFF)
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
  return bit.band(math.floor(math.atan(tx - sx, -(ty - sy)) * 256 / C.TWO_PI + 0.5), 0xFF)
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
-- nav_turn_cap(nav_cap, turn_cap) — the speed a corner is taken at.
-- nav_cap is a top-speed cap (C.NAV_TOP_SPEED / C.NAV_CRUISE_SPEED) and
-- turn_cap its turn-radius twin (C.NAV_TURN_*). Normally the lower of the
-- two, so a slow turn rate lowers the cap. With C.STEER_TURN_SPEEDUP a turn
-- rate faster than our top speed raises it by the same ratio, at most
-- x C.STEER_TURN_SPEEDUP_MAX and never above our live top speed
-- (C.NAV_TOP_SPEED). Under classic rules this is math.min(nav_cap, turn_cap).
function M.nav_turn_cap(nav_cap, turn_cap)
  local r = LP.turn_speedup()
  if r then
    local up = C.STEER_TURN_SPEEDUP_MAX
    if type(up) ~= "number" or up < 1 then up = 1 end
    if r < up then up = r end
    local v = nav_cap * up
    if v > C.NAV_TOP_SPEED then v = C.NAV_TOP_SPEED end
    if v < nav_cap then v = nav_cap end
    return v
  end
  return math.min(nav_cap, turn_cap)
end

-- enemy_speed_scale() — nil, or the factor enemy terrain speed caps
-- (C.MAP_SPEED) take under C.ENEMY_SPEED_OWN_MODS (our speed modifier).
function M.enemy_speed_scale()
  return LP.enemy_speed_scale()
end

--- Whether a wall is worth shells under C.WALL_SHOOT_LIFE_MAX: always when
--- the knob is 0 (the old behaviour), otherwise only when the live
--- building_life (info.rules; classic 4 when the host does not send it) is
--- at most the knob. Joust walls take 255 hits and are never shot.
function M.wall_shootable(info)
  local cap = C.WALL_SHOOT_LIFE_MAX or 0
  if cap <= 0 then return true end
  local r = info and info.rules
  local life = r and r.building_life
  if type(life) ~= "number" or life <= 0 then life = 4 end
  return life <= cap
end

function M.nav_turn_speed(corr, speed, max_speed, min_speed)
  local keys, taps = 0, 0
  local abs_corr = math.abs(corr)
  max_speed = max_speed or C.NAV_CRUISE_SPEED   -- 48 classic
  min_speed = min_speed or 4

  -- Turning: 3-tier hold/tap/none
  if     corr >  10 then keys = bit.bor(keys, KEY_TURNRIGHT)
  elseif corr < -10 then keys = bit.bor(keys, KEY_TURNLEFT)
  elseif corr >   2 then taps = bit.bor(taps, KEY_TURNRIGHT)
  elseif corr <  -2 then taps = bit.bor(taps, KEY_TURNLEFT)
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
      keys = bit.bor(keys, KEY_SLOWER)
    elseif speed < max_speed then
      keys = bit.bor(keys, KEY_FASTER)
    end
  elseif abs_corr > 80 then
    if speed > min_speed then keys = bit.bor(keys, KEY_SLOWER) end
  else
    -- The ramp sets the speed a turn is taken at, so the nav caps give way to
    -- the turn-radius caps when our turn rate is slower than our top speed
    -- (live_physics.lua scales both; under classic rules they are equal).
    local ramp_max = max_speed
    if max_speed == C.NAV_CRUISE_SPEED then
      ramp_max = M.nav_turn_cap(max_speed, C.NAV_TURN_CRUISE_SPEED)
    elseif max_speed == C.NAV_TOP_SPEED then
      ramp_max = M.nav_turn_cap(max_speed, C.NAV_TURN_TOP_SPEED)
    end
    local factor = 1.0 - (abs_corr - 16) / 64.0
    local desired = math.max(min_speed, math.floor(factor * ramp_max))
    if speed > desired + 4 then
      keys = bit.bor(keys, KEY_SLOWER)
    elseif speed < desired then
      keys = bit.bor(keys, KEY_FASTER)
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

-- How many HUMAN players are on our team right now.
--
-- Both inputs are engine-authoritative, so this needs no broadcast heuristic
-- and has no join-lag window:
--   * info.allies      — the alliance bitmap over IN-USE slots. It INCLUDES
--                        our own bit (players.c playersGetAlliesBitMap sets
--                        the bit when count == playerNum).
--   * info.player_bots — per-slot PLAYER_FLAG_BOT bitmap. The flag is
--                        server-set at bot creation (server_sim.c stamps it
--                        BEFORE the join is published, so it arrives with the
--                        player), is documented "server-set, trusted — never
--                        honour from a client packet" (player_flags.h), and is
--                        deliberately kept OUT of client_snapshot.c's
--                        snapshotMask so it survives every snapshot tick
--                        instead of being clobbered to 0 by out-of-view stubs.
-- So allied humans = allies & ~player_bots — the same idiom init.lua already
-- uses to address the human-only chat path.
--
-- Our own bit is cleared FIRST. A brain always carries the BOT flag, so the
-- mask would drop us anyway; but if it ever failed to (a brain driven in a slot
-- the server never flagged), we would classify OURSELVES as a human ally, and
-- every human-ally-gated behaviour would silently flip for a lone bot. Being
-- explicit costs one operation.
function M.human_ally_count(info)
  if not info then return 0 end
  local allies = info.allies or 0
  local me     = info.player_number
  if me then allies = bit.band(allies, bit.bnot(bit.lshift(1, me))) end
  local mask   = bit.band(allies, bit.bnot(info.player_bots or 0))
  local n = 0
  while mask ~= 0 do
    if bit.band(mask, 1) ~= 0 then n = n + 1 end
    mask = bit.rshift(mask, 1)
  end
  return n
end

-- IS A HUMAN TEAM-MATE STANDING CLOSE TO US?
--
-- M.human_ally_count above answers "are there humans on the team", which is a
-- lobby question. This answers "is one of them RIGHT HERE", which is a map
-- question, and the two have different sources: a count comes off the two
-- bitmaps, a distance has to come off the engine's object scan.
--
-- The join is ob.idnum: an allied tank object carries the owner's PLAYER
-- NUMBER, so the same player_bots bitmap that names the humans on the team
-- also names which of the visible allied tanks belongs to one. A tank we
-- cannot see is not near us as far as any behaviour is concerned, so an
-- invisible human is simply absent here — no ghosting, no last-known guess.
--
-- Distance is CHEBYSHEV (the larger of the two axes), the way a screen is
-- measured and the way every other "within N tiles of me" rule in this brain
-- reads. Returns the distance to the NEAREST such human, or nil for none,
-- and that human's player number as a second value.
-- `tiles` is a cap: a human further off than that is not reported at all.
-- `only` (optional) is a set {[pn]=true}: a human not in it is skipped
-- (orders.human_near_suicide passes the humans seen shooting at the pill).
function M.human_ally_near(info, mx, my, tiles, only)
  if not info or not mx or not tiles or tiles <= 0 then return nil end
  local allies = info.allies or 0
  local bots   = info.player_bots or 0
  local me     = info.player_number
  local OT     = _G.OBJECT_TANK
  local OH     = _G.OBJECT_HOSTILE or 0
  local best, best_pn
  for _, ob in ipairs(info.objects or {}) do
    local pn = ob.idnum or -1
    if ob.type == OT and pn >= 0 and pn ~= me
       and bit.band(ob.info or 0, OH) == 0
       and bit.band(allies, bit.lshift(1, pn)) ~= 0
       and bit.band(bots, bit.lshift(1, pn)) == 0
       and (not only or only[pn]) then
      local dx = math.abs(bit.rshift(ob.x or 0, 8) - mx)
      local dy = math.abs(bit.rshift(ob.y or 0, 8) - my)
      local d  = (dx > dy) and dx or dy
      if d <= tiles and (not best or d < best) then best, best_pn = d, pn end
    end
  end
  return best, best_pn
end

-- ── DIFFICULTY (Stage 3 Pass B) — deterministic aim/fire handicaps ─────────
-- Both helpers are exact no-ops at their default (0) argument: they return
-- BEFORE touching any float / hash / state, so Hard (all difficulty knobs 0)
-- stays bit-for-bit identical to today's brain. The ONLY source of variation
-- is the deterministic (tick-block + target + per-bot) hash below — never
-- math.random (the determinism recipe forbids a fresh draw on a per-tick
-- decision path).

-- AIM_ERROR_BRADS: rotate an aim POINT (tx,ty) about the tank by a signed brad
-- offset in [-err_brads, err_brads]. The offset is deterministic and changes
-- only every AIM_ERROR_PERIOD ticks, so it reads as "this bot's aim is a bit
-- off" rather than per-tick jitter. Seeded by (tick/period + target id +
-- per-bot replan_offset) so different bots, different targets and different
-- time-windows miss in different directions, all reproducibly. Callers apply
-- this to the point BEFORE the shell-path check reads it, so the validated
-- path is the deflected path (a deflected shell can never hit a friendly a
-- clear-check thought clear).
local AIM_ERROR_PERIOD = 40
function M.aim_error_point(state, err_brads, tankx, tanky, tx, ty, tid)
  if not err_brads or err_brads == 0 then return tx, ty end   -- no-op: nothing computed
  local nid = (type(tid) == "number") and tid or 0
  local blk = math.floor((state.tick or 0) / AIM_ERROR_PERIOD)
  -- seed < ~3000 => seed*2654435761 < 2^43, exact in a double; low 16 bits mixed
  local seed = blk + nid + (state.replan_offset or 0)
  local h    = (seed * 2654435761) % 65536
  local off  = (h % (2 * err_brads + 1)) - err_brads   -- integer in [-E, E]
  if off == 0 then return tx, ty end
  local ang = off * (math.pi * 2 / 256)
  local s, c = math.sin(ang), math.cos(ang)
  local dx, dy = tx - tankx, ty - tanky
  return tankx + dx * c - dy * s, tanky + dx * s + dy * c
end

-- LGM_MISS_WU / LGM_HIT_PCT: a man-only aim handicap. Shots at a man (the
-- kill_lgm aim point, both the fire gate and the crosshair search) aim at
-- him only LGM_HIT_PCT percent of the time; otherwise the aim POINT moves
-- LGM_MISS_WU world units from him, so the shell bursts near him (a man dies
-- inside 128 wu of a burst) and the tank still turns toward him and fires.
-- A walking man (vx, vy: his velocity) is missed BEHIND him, within 45
-- degrees either side: a late shot, which he walks away from. A man
-- standing still is missed in one of 16 directions. Drawn per
-- (LGM_MISS_PERIOD-think block + target id + per-bot replan_offset) with the
-- same hash as aim_error_point, so both call sites in one think move by the
-- same amount and a seed replays exactly. No math.random. No-op (nothing
-- computed) at miss_wu <= 0, the default.
local LGM_MISS_PERIOD = 12
function M.lgm_miss_point(state, miss_wu, hit_pct, tx, ty, tid, vx, vy)
  if not miss_wu or miss_wu <= 0 then return tx, ty end   -- no-op
  local nid = (type(tid) == "number") and tid or 0
  local blk = math.floor((state.tick or 0) / LGM_MISS_PERIOD)
  local seed = blk + nid * 7 + (state.replan_offset or 0)
  local h    = (seed * 2654435761) % 65536
  if (h % 100) < (hit_pct or 0) then return tx, ty end    -- this block aims true
  local k = math.floor(h / 4096)                           -- 0..15
  local ang
  vx, vy = vx or 0, vy or 0
  if vx * vx + vy * vy > 1 then
    -- behind him: the bearing of -v, plus -2..2 steps of 22.5 degrees
    ang = math.atan(-vx, vy) + ((k % 5) - 2) * (math.pi * 2 / 16)
  else
    ang = k * (math.pi * 2 / 16)
  end
  return tx + math.sin(ang) * miss_wu, ty - math.cos(ang) * miss_wu
end

-- FIRE_HOLD_TICKS: brain-side reload gate. Returns true when a shot must be
-- SUPPRESSED this tick (a recent shot is still inside the hold window); else
-- stamps state._last_fire_tick and returns false so the caller fires. No-op
-- (never suppresses, never stamps, never writes state) at hold_ticks <= 0.
-- Suppressing KEY_SHOOT keeps shot_tracker's in-flight kill-lock accounting
-- consistent: the tracker keys off the real info.shells decrement, so a shot
-- we never fire is a shell that never leaves and is never counted.
function M.fire_hold_block(state, hold_ticks)
  if not hold_ticks or hold_ticks <= 0 then return false end   -- no-op
  local now  = state.tick or 0
  local last = state._last_fire_tick
  if last and (now - last) < hold_ticks then return true end
  state._last_fire_tick = now
  return false
end

-- Live game rules -> physics constants (see live_physics.lua). Reached
-- through U so Brain.think needs no new upvalue.
M.live_physics = require("live_physics")

return M
