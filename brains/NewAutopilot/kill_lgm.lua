-- =========================================================================
-- NewAutopilot/kill_lgm.lua — enemy LGM targeting helpers.
--
-- Pure math + per-LGM history tracking.  No viz, no I/O.  Used by:
--   * perception.lua  — updates velocity per enemy LGM each tick, runs
--                       phase detection (outgoing / built / unknown),
--                       and stashes the lead-predicted aim point on the
--                       enemy_lgms[] entry so steering + fire both read
--                       a single source of truth.
--   * steering.lua    — kill_lgm block aims tank heading at the predicted
--                       tile (lateral lead).
--   * init.lua        — kill_lgm fire block reads the predicted aim and
--                       drives gunrange via KEY_MORERANGE/KEY_LESSRANGE
--                       so the shell explodes ON the LGM tile, not past it.
--
-- LGMs die when a shell explodes within MAP_SQUARE_MIDDLE (128 wu) of
-- their position on non-solid terrain, or on the exact tile on solid
-- terrain (lgm.c:1321).
--
-- Predictor tiers (predict_aim picks the best available per call):
--   1. RETURN + owning tank visible → engine-sim toward tank.wx/wy
--   2. 3 matching 10-tick windows  → engine-sim toward projected
--                                      ray-to-map-edge destination
--   3. fallback                    → naive linear pos + v*T
-- =========================================================================

local M = {}

local C = require("constants")
local U = require("util")

-- Velocity sampling window: 10 brain ticks.  Velocity = (cur_pos -
-- pos_10_ticks_ago) / 10.  Window is long enough to wash out 1-tick
-- position quantization jitter but short enough to track direction
-- changes within a few ticks.
local V_WINDOW_TICKS = 10

-- Phase detection.  We want THREE non-overlapping 10-tick windows of
-- matching velocity to confirm "LGM is walking straight" — that's 30
-- ticks (~0.6s @ 50Hz) of uninterrupted motion before we trust the
-- destination projection.  Samples retained = enough for that.
local NUM_WINDOWS_FOR_MATCH = 3
local V_HISTORY_TICKS = V_WINDOW_TICKS * NUM_WINDOWS_FOR_MATCH + 2

-- Matching tolerance between windows: same direction within ±MATCH_DEG
-- and magnitude within ±MATCH_MAG_FRAC of each other.
local MATCH_DEG       = 8
local MATCH_MAG_FRAC  = 0.25

-- Build/stop detection: LGM stationary for this many ticks → it built
-- (or got cancelled / blocked), flip to RETURN phase.  "Stationary"
-- means cumulative movement over the window < STILL_THRESHOLD_WU.
local STILL_WINDOW_TICKS  = 40
local STILL_THRESHOLD_WU  = 24      -- < ~⅒ tile total drift over 40 ticks

-- Tank-proximity tolerance for "exit detected" (first sighting near a
-- known enemy tank → phase=outgoing, owning_tank set).
local EXIT_TANK_TOL_WU    = 384     -- 1.5 tiles Manhattan

-- Drop history entries we haven't seen for this many ticks.
local STALE_TICKS = 250  -- 5s @ 50 Hz

-- Splash tolerance for "good enough" range alignment.  Half a tile.
local SPLASH_WU = 128

-- Map bounds in wu (256 tiles × 256 wu/tile).
local MAP_WU = 256 * 256

-- --------------------------------------------------------------------------
-- Wall-sim helpers (resurrected from WIP_kill_lgm_wall_aware_predict.md,
-- but destination-driven this time).
-- --------------------------------------------------------------------------

local function man_speed_at(mx, my)
  local tt = U.ttype(mx, my)
  return C.MAN_SPEED[tt] or 0
end

local function is_blocked(mx, my)
  if not U.in_map(mx, my) then return true end
  return man_speed_at(mx, my) <= 0
end

-- --------------------------------------------------------------------------
-- M.project_to_edge(wx, wy, vx, vy) → dest_wx, dest_wy (clamped to map)
--
-- Cast a ray from (wx, wy) in direction (vx, vy) until it hits the map's
-- outer box (0..MAP_WU on each axis).  Returns the intersection point.
-- No tile-walking — pure parametric math.  When v is ~zero, returns the
-- start point unchanged (caller should fall back to linear predictor).
-- --------------------------------------------------------------------------
function M.project_to_edge(wx, wy, vx, vy)
  if math.abs(vx) < 1e-3 and math.abs(vy) < 1e-3 then
    return wx, wy
  end
  local tx = math.huge
  if vx > 0  then tx = (MAP_WU - wx) / vx
  elseif vx < 0  then tx = (0      - wx) / vx end
  local ty = math.huge
  if vy > 0  then ty = (MAP_WU - wy) / vy
  elseif vy < 0  then ty = (0      - wy) / vy end
  local t = math.min(tx, ty)
  return wx + vx * t, wy + vy * t
end

-- --------------------------------------------------------------------------
-- M.sim_forward_to_dest(lgm_wx, lgm_wy, dest_wx, dest_wy, T) → end_wx, end_wy
--
-- Engine-faithful T-tick LGM simulation toward a fixed destination.
-- Mirrors lgm.c:668-730:
--   1. angle = utilCalcAngle(cur, dest)
--   2. (xAdd, yAdd) = v_max * (cos, sin)  [v_max from cur tile]
--   3. Try diagonal move; on blocked axis, slide along the other axis;
--      both blocked = stop.
-- --------------------------------------------------------------------------
function M.sim_forward_to_dest(lgm_wx, lgm_wy, dest_wx, dest_wy, T)
  local wx, wy = lgm_wx, lgm_wy
  for _ = 1, T do
    local cur_mx = math.floor(wx) >> 8
    local cur_my = math.floor(wy) >> 8
    local v_max = man_speed_at(cur_mx, cur_my)
    if v_max <= 0 then break end
    local ddx = dest_wx - wx
    local ddy = dest_wy - wy
    local dist = math.sqrt(ddx * ddx + ddy * ddy)
    if dist < 1 then break end  -- arrived
    local svx = (ddx / dist) * v_max
    local svy = (ddy / dist) * v_max
    local nx = wx + svx
    local ny = wy + svy
    local new_mx = math.floor(nx) >> 8
    local new_my = math.floor(ny) >> 8
    local x_blocked = (new_mx ~= cur_mx) and is_blocked(new_mx, cur_my)
    local y_blocked = (new_my ~= cur_my) and is_blocked(cur_mx, new_my)
    if x_blocked and y_blocked then break end
    if not x_blocked then wx = nx end
    if not y_blocked then wy = ny end
  end
  return wx, wy
end

-- --------------------------------------------------------------------------
-- Velocity tracking + phase detection
-- --------------------------------------------------------------------------

-- Inspect samples; return v1, v2, v3 = three non-overlapping 10-tick
-- window velocities (latest first), or nil if not enough history.
local function three_window_velocities(samples, now)
  local function find_sample_at_least_age(min_age)
    -- Latest sample whose age is >= min_age.
    for i = #samples, 1, -1 do
      if (now - samples[i].tick) >= min_age then return samples[i] end
    end
    return nil
  end
  local cur = samples[#samples]
  local s10 = find_sample_at_least_age(V_WINDOW_TICKS)
  local s20 = find_sample_at_least_age(V_WINDOW_TICKS * 2)
  local s30 = find_sample_at_least_age(V_WINDOW_TICKS * 3)
  if not (cur and s10 and s20 and s30) then return nil end
  local function vel(a, b)
    local dt = a.tick - b.tick
    if dt <= 0 then return 0, 0, 0 end
    local vx = (a.wx - b.wx) / dt
    local vy = (a.wy - b.wy) / dt
    return vx, vy, dt
  end
  local v1x, v1y = vel(cur, s10)
  local v2x, v2y = vel(s10, s20)
  local v3x, v3y = vel(s20, s30)
  return v1x, v1y, v2x, v2y, v3x, v3y
end

local function vels_match(v1x, v1y, v2x, v2y)
  local m1 = math.sqrt(v1x * v1x + v1y * v1y)
  local m2 = math.sqrt(v2x * v2x + v2y * v2y)
  -- Both effectively zero = not "matching motion", just both stuck.
  if m1 < 0.5 or m2 < 0.5 then return false end
  -- Magnitude tolerance.
  local mhi, mlo = math.max(m1, m2), math.min(m1, m2)
  if mlo < mhi * (1 - MATCH_MAG_FRAC) then return false end
  -- Angle tolerance via dot product.  cos(MATCH_DEG) threshold.
  local cos_thresh = math.cos(math.rad(MATCH_DEG))
  local dot = (v1x * v2x + v1y * v2y) / (m1 * m2)
  return dot >= cos_thresh
end

-- Detect "LGM has been stationary" by summing per-sample drift over
-- STILL_WINDOW_TICKS.  If total drift < STILL_THRESHOLD_WU AND we have
-- enough samples to span the window, treat as built/stopped.
local function detect_stopped(samples, now)
  if #samples < 2 then return false end
  -- Find earliest sample within the still-window.
  local earliest = nil
  for i = 1, #samples do
    if (now - samples[i].tick) <= STILL_WINDOW_TICKS then
      earliest = samples[i]; break
    end
  end
  if not earliest or (now - earliest.tick) < STILL_WINDOW_TICKS - 5 then
    return false  -- window not yet filled
  end
  local cur = samples[#samples]
  local d = math.abs(cur.wx - earliest.wx) + math.abs(cur.wy - earliest.wy)
  return d < STILL_THRESHOLD_WU
end

-- --------------------------------------------------------------------------
-- M.update_velocity(state, lgm, now, enemy_tanks) → vx, vy
--
-- Maintains per-LGM history + phase.  Phase transitions:
--   * First sighting near a known enemy tank → phase = "outgoing"
--     (owning_tank_idnum = that tank's id).
--   * Stationary for STILL_WINDOW_TICKS → phase = "return" (assumes
--     LGM built/blocked/gave-up).
--   * Otherwise phase carries over (or stays "unknown" if no exit seen).
--
-- Additionally tries to cache an inferred destination:
--   * When three 10-tick windows have matching v, project to map edge
--     and stash as h.dest_wx/wy.  Cleared when matching fails (LGM
--     changed direction).
-- --------------------------------------------------------------------------
function M.update_velocity(state, lgm, now, enemy_tanks)
  if not lgm.idnum then
    return lgm.vx or 0, lgm.vy or 0
  end
  state._enemy_lgm_history = state._enemy_lgm_history or {}
  local hist = state._enemy_lgm_history
  local h = hist[lgm.idnum]
  if not h then
    h = {
      samples = { { wx = lgm.wx, wy = lgm.wy, tick = now } },
      last_tick = now,
      v_ema_x = lgm.vx or 0, v_ema_y = lgm.vy or 0,
      phase = "unknown",
      owning_tank_idnum = nil,
      dest_wx = nil, dest_wy = nil,
    }
    -- Exit detection: first sighting near an enemy tank → outgoing.
    if enemy_tanks then
      local best_d = EXIT_TANK_TOL_WU
      local best_id = nil
      for _, et in ipairs(enemy_tanks) do
        local d = math.abs(et.wx - lgm.wx) + math.abs(et.wy - lgm.wy)
        if d < best_d then best_d = d; best_id = et.id end
      end
      if best_id then
        h.phase = "outgoing"
        h.owning_tank_idnum = best_id
      end
    end
    hist[lgm.idnum] = h
    return h.v_ema_x, h.v_ema_y
  end

  -- Append sample + trim to V_HISTORY_TICKS+1 of retention.
  local samples = h.samples
  samples[#samples + 1] = { wx = lgm.wx, wy = lgm.wy, tick = now }
  while samples[1] and (now - samples[1].tick) > V_HISTORY_TICKS do
    if samples[2] and (now - samples[2].tick) >= V_HISTORY_TICKS then
      table.remove(samples, 1)
    else
      break
    end
  end

  -- Primary velocity = latest 10-tick window.
  local oldest_in_window
  for i = 1, #samples do
    if (now - samples[i].tick) <= V_WINDOW_TICKS then
      oldest_in_window = samples[i]; break
    end
  end
  oldest_in_window = oldest_in_window or samples[1]
  local dt = now - oldest_in_window.tick
  if dt > 0 then
    h.v_ema_x = (lgm.wx - oldest_in_window.wx) / dt
    h.v_ema_y = (lgm.wy - oldest_in_window.wy) / dt
  end

  -- Three-window match → cache projected destination.  Re-check every
  -- tick; the dest stays cached as long as v keeps matching.  When the
  -- LGM changes direction (windows diverge), invalidate.
  local v1x, v1y, v2x, v2y, v3x, v3y = three_window_velocities(samples, now)
  if v1x and vels_match(v1x, v1y, v2x, v2y) and vels_match(v2x, v2y, v3x, v3y) then
    -- Project ray from CURRENT position along v1 to map edge.
    h.dest_wx, h.dest_wy = M.project_to_edge(lgm.wx, lgm.wy, v1x, v1y)
    h.dest_locked = true
  else
    -- If we had a dest from a prior straight-line walk, keep it as a
    -- stale hint but mark it unlocked (predictor still prefers it over
    -- linear, since stale-straight is usually better than no
    -- direction).  Caller can decide based on phase.
    h.dest_locked = false
  end

  -- Stop detection → flip phase to return.
  if h.phase == "outgoing" or h.phase == "unknown" then
    if detect_stopped(samples, now) then
      h.phase = "return"
      h.dest_wx, h.dest_wy = nil, nil  -- old outgoing dest is irrelevant
      h.dest_locked = false
    end
  end

  h.last_tick = now
  return h.v_ema_x, h.v_ema_y
end

-- --------------------------------------------------------------------------
-- M.purge_stale(state, now) — drop history rows older than STALE_TICKS.
-- --------------------------------------------------------------------------
function M.purge_stale(state, now)
  local hist = state._enemy_lgm_history
  if not hist then return end
  for idnum, h in pairs(hist) do
    if now - h.last_tick > STALE_TICKS then
      hist[idnum] = nil
    end
  end
end

-- --------------------------------------------------------------------------
-- M.sightlen_for(distance_wu) / M.flight_ticks(sightLen)
-- --------------------------------------------------------------------------
function M.sightlen_for(distance_wu)
  local s = math.floor(distance_wu / 128 + 0.5)
  if s < 2  then return 2  end
  if s > 14 then return 14 end
  return s
end

function M.flight_ticks(sightLen)
  -- shells.c:133: shellLifeTicks(len) = SHELL_LIFE(8)*len - SHELL_START_ADD(6),
  -- where tank passes `sightLen / 2` as len (engine's integer divide,
  -- see brain_pathfinder.c:3221).  So actual ticks-to-impact at
  -- sightLen-distance = 8*(sightLen/2) - 6 = 4*sightLen - 6.  The
  -- −6 is the spawn offset (shell appears in front of the tank, not
  -- at its center); without it we over-lead by ~6 ticks of LGM motion.
  local t = 4 * sightLen - 6
  if t < 1 then return 1 end
  return t
end

-- --------------------------------------------------------------------------
-- M.predict_aim(tank_wx, tank_wy, lgm, enemy_tanks, history_entry)
--   → aim_wx, aim_wy, sightLen, flight_ticks, distance_wu, tier
--
-- Tier dispatcher.  Picks the best available predictor:
--   tier 1: phase==return AND owning tank visible → sim toward tank
--   tier 2: dest_wx/wy cached (3-match active) → sim toward dest
--   tier 3: linear fallback (pos + v*T, two-pass refit)
-- All three use the same two-pass arrival-time refit on the OUTPUT to
-- pick the final sightLen.
-- --------------------------------------------------------------------------
function M.predict_aim(tank_wx, tank_wy, lgm, enemy_tanks, h)
  local lgm_wx = lgm.wx
  local lgm_wy = lgm.wy
  local vx = (h and h.v_ema_x) or lgm.v_ema_x or 0
  local vy = (h and h.v_ema_y) or lgm.v_ema_y or 0
  local tier = "linear"
  local dest_wx, dest_wy

  -- Tier 1: return phase + owning tank visible.
  if h and h.phase == "return" and h.owning_tank_idnum and enemy_tanks then
    for _, et in ipairs(enemy_tanks) do
      if et.id == h.owning_tank_idnum then
        dest_wx, dest_wy = et.wx, et.wy
        tier = "return_tank"
        break
      end
    end
  end

  -- Tier 2: cached outgoing/unknown destination (3-match).
  if not dest_wx and h and h.dest_locked and h.dest_wx and h.dest_wy then
    dest_wx, dest_wy = h.dest_wx, h.dest_wy
    tier = "dest_lock"
  end

  -- Compute T1, T2 against initial linear pos1 in all cases (cheap).
  local dx = tank_wx - lgm_wx
  local dy = tank_wy - lgm_wy
  local D  = math.sqrt(dx * dx + dy * dy)
  local T1 = M.flight_ticks(M.sightlen_for(D))

  local pos1_wx, pos1_wy
  if dest_wx then
    pos1_wx, pos1_wy = M.sim_forward_to_dest(lgm_wx, lgm_wy, dest_wx, dest_wy, T1)
  else
    pos1_wx = lgm_wx + vx * T1
    pos1_wy = lgm_wy + vy * T1
  end

  local pdx = tank_wx - pos1_wx
  local pdy = tank_wy - pos1_wy
  local pD  = math.sqrt(pdx * pdx + pdy * pdy)
  local T2  = M.flight_ticks(M.sightlen_for(pD))
  local aim_wx, aim_wy = pos1_wx, pos1_wy
  if T2 > T1 then
    local extra = T2 - T1
    if dest_wx then
      aim_wx, aim_wy = M.sim_forward_to_dest(pos1_wx, pos1_wy, dest_wx, dest_wy, extra)
    else
      aim_wx = pos1_wx + vx * extra
      aim_wy = pos1_wy + vy * extra
    end
  end

  local fdx = tank_wx - aim_wx
  local fdy = tank_wy - aim_wy
  local fD  = math.sqrt(fdx * fdx + fdy * fdy)
  local sightLen = M.sightlen_for(fD)
  return aim_wx, aim_wy, sightLen, M.flight_ticks(sightLen), fD, tier
end

-- --------------------------------------------------------------------------
-- M.gunrange_key(current_sightLen, target_sightLen) → bitmask
-- --------------------------------------------------------------------------
function M.gunrange_key(current_sightLen, target_sightLen)
  if not current_sightLen or not target_sightLen then return 0 end
  if current_sightLen < target_sightLen then return KEY_MORERANGE end
  if current_sightLen > target_sightLen then return KEY_LESSRANGE end
  return 0
end

M.SPLASH_WU = SPLASH_WU

return M
