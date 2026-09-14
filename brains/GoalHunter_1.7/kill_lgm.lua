local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/kill_lgm.lua — enemy LGM targeting helpers.
--
-- Pure math + per-LGM history tracking.  No viz, no I/O.  Used by:
--   * perception.lua  — updates velocity per enemy LGM each tick and
--                       stashes the lead-predicted aim point on the
--                       enemy_lgms[] entry so steering + fire both read
--                       a single source of truth.
--   * steering.lua    — kill_lgm block aims tank heading at the predicted
--                       tile (lateral lead).
--   * init.lua        — kill_lgm fire block reads the predicted aim and
--                       drives gunrange via KEY_MORERANGE/KEY_LESSRANGE.
--
-- LGMs die when a shell explodes within MAP_SQUARE_MIDDLE (128 wu) of
-- their position on non-solid terrain, or on the exact tile on solid
-- terrain (lgm.c:1321).
--
-- Predictor — two tiers, picked per call:
--   1. dest_lock — 3 consecutive 10-tick velocity windows have matching
--                  direction, so the LGM is currently walking a straight
--                  line.  Project the ray to the map's outer box, run an
--                  engine-faithful sim toward that point.  Works for
--                  both outgoing and return walks: the dest doesn't need
--                  to be the real target tile, just somewhere in the
--                  right direction so the per-step angle is correct.
--   2. linear   — fallback: pos + v*T two-pass refit.  Used while the
--                  LGM is accelerating, turning, building (zero-v), or
--                  otherwise not yet showing 30 ticks of straight-line
--                  motion.
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
-- and magnitude within ±MATCH_MAG_FRAC of each other.  Loosened by
-- ~15% (deg 8→9.2, mag 0.25→0.29) so a wall-sliding LGM that wobbles
-- a tile or two still registers as "on the same straight-line path."
local MATCH_DEG       = 9.2
local MATCH_MAG_FRAC  = 0.29

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
    local cur_mx = bit.rshift(math.floor(wx), 8)
    local cur_my = bit.rshift(math.floor(wy), 8)
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
    local new_mx = bit.rshift(math.floor(nx), 8)
    local new_my = bit.rshift(math.floor(ny), 8)
    local x_blocked = (new_mx ~= cur_mx) and is_blocked(new_mx, cur_my)
    local y_blocked = (new_my ~= cur_my) and is_blocked(cur_mx, new_my)
    if x_blocked and y_blocked then break end
    if not x_blocked then wx = nx end
    if not y_blocked then wy = ny end
  end
  return wx, wy
end

-- --------------------------------------------------------------------------
-- Velocity tracking + 3-window straight-line detection
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

-- Returns (matched, fail_reason_string, ang_deg, mag_ratio).
-- ang_deg is the angle between v1 and v2 (degrees); mag_ratio is
-- larger/smaller magnitude.  Surfaces diagnostic info to the viz.
local function vels_match_diag(v1x, v1y, v2x, v2y)
  local m1 = math.sqrt(v1x * v1x + v1y * v1y)
  local m2 = math.sqrt(v2x * v2x + v2y * v2y)
  if m1 < 0.5 or m2 < 0.5 then
    return false, "still", 0.0, 0.0
  end
  local mhi, mlo = math.max(m1, m2), math.min(m1, m2)
  local mag_ratio = mhi / math.max(mlo, 0.001)
  local cos_dot = (v1x * v2x + v1y * v2y) / (m1 * m2)
  if cos_dot >  1 then cos_dot =  1 end
  if cos_dot < -1 then cos_dot = -1 end
  local ang_deg = math.deg(math.acos(cos_dot))
  if mlo < mhi * (1 - MATCH_MAG_FRAC) then
    return false, "accel", ang_deg, mag_ratio
  end
  if ang_deg > MATCH_DEG then
    return false, "turn", ang_deg, mag_ratio
  end
  return true, "match", ang_deg, mag_ratio
end

-- --------------------------------------------------------------------------
-- M.update_velocity(state, lgm, now) → vx, vy
--
-- Maintains per-LGM history.  Each tick:
--   1. Append a (wx, wy, tick) sample; trim entries beyond
--      V_HISTORY_TICKS old.
--   2. Compute the latest 10-tick velocity (cur_pos − oldest_in_window) / dt.
--   3. Check the last three 10-tick windows: if all three agree on
--      direction (within MATCH_DEG) and magnitude (within MATCH_MAG_FRAC),
--      the LGM is currently walking a straight line — project the ray
--      from its current pos to the map's outer box and stash as
--      h.dest_wx/wy / h.dest_locked.
--   4. If the windows DON'T agree (LGM accelerating, turning, building,
--      etc.), unlock — predictor falls back to linear.
--
-- That's it.  No phase tracking, no exit-detection, no
-- build-detection, no divergence accounting.  The 3-window match is
-- the single signal that we have enough straight-line motion to trust
-- a destination-driven engine sim; everything else falls to linear.
-- --------------------------------------------------------------------------
function M.update_velocity(state, lgm, now)
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
      dest_wx = nil, dest_wy = nil, dest_locked = false,
    }
    hist[lgm.idnum] = h
    return h.v_ema_x, h.v_ema_y
  end

  -- Teleport detector: max legit per-tick LGM motion is v_max = 16 wu
  -- per axis (lgm.c:686 calls utilCalcDistance with speed ≤ 16).  An
  -- axis delta > 16 means the engine just moved the LGM
  -- non-continuously: tank queued multiple LGM jobs so it returned
  -- home then immediately got dispatched out again between our
  -- brain-tick observations, or the engine otherwise teleported it.
  -- The pre-jump samples must NOT influence post-jump velocity, so we
  -- full-reset the history.
  local samples = h.samples
  local prev = samples[#samples]
  if prev then
    local dwx = math.abs(lgm.wx - prev.wx)
    local dwy = math.abs(lgm.wy - prev.wy)
    if dwx > 16 or dwy > 16 then
      h.samples = { { wx = lgm.wx, wy = lgm.wy, tick = now } }
      h.dest_wx, h.dest_wy = nil, nil
      h.dest_locked = false
      h.v_ema_x, h.v_ema_y = 0.0, 0.0
      h.last_tick = now
      h.lock_status = string.format("RESET teleport Δ=(%d,%d)", dwx, dwy)
      return 0.0, 0.0
    end
  end
  samples[#samples + 1] = { wx = lgm.wx, wy = lgm.wy, tick = now }
  while samples[1] and (now - samples[1].tick) > V_HISTORY_TICKS do
    if samples[2] and (now - samples[2].tick) >= V_HISTORY_TICKS then
      table.remove(samples, 1)
    else
      break
    end
  end

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

  local v1x, v1y, v2x, v2y, v3x, v3y = three_window_velocities(samples, now)
  if not v1x then
    -- Not enough history yet.  Show how close we are to having three
    -- windows of samples (need 30 ticks).
    local oldest_age = (#samples > 0) and (now - samples[1].tick) or 0
    h.dest_wx, h.dest_wy = nil, nil
    h.dest_locked = false
    h.lock_status = string.format("warming %d/%d ticks", oldest_age, V_WINDOW_TICKS * 3)
  else
    local ok12, why12, ang12, mag12 = vels_match_diag(v1x, v1y, v2x, v2y)
    local ok23, why23, ang23, mag23 = vels_match_diag(v2x, v2y, v3x, v3y)
    if ok12 and ok23 then
      h.dest_wx, h.dest_wy = M.project_to_edge(lgm.wx, lgm.wy, v1x, v1y)
      h.dest_locked = true
      h.lock_status = string.format("LOCK Δ=%.0f° mag=%.2fx",
                                    math.max(ang12, ang23),
                                    math.max(mag12, mag23))
    else
      h.dest_wx, h.dest_wy = nil, nil
      h.dest_locked = false
      -- Surface whichever pair failed worst.
      local why  = ok12 and why23 or why12
      local ang  = ok12 and ang23 or ang12
      local mag  = ok12 and mag23 or mag12
      if why == "still" then
        h.lock_status = "still (|v|<0.5)"
      elseif why == "accel" then
        h.lock_status = string.format("accel mag=%.2fx (>%.2fx)",
                                      mag, 1.0 / (1.0 - MATCH_MAG_FRAC))
      elseif why == "turn" then
        h.lock_status = string.format("turn Δ=%.0f° (>%.0f°)", ang, MATCH_DEG)
      else
        h.lock_status = why or "?"
      end
    end
  end

  h.last_tick = now
  return h.v_ema_x, h.v_ema_y
end

-- --------------------------------------------------------------------------
-- M.purge_killed(state, kill_wx, kill_wy, tol_wu) — drop any history
-- whose last-seen position is within tol_wu of (kill_wx, kill_wy).
-- Called by perception when a hostile parachute is observed (the
-- engine spawns one at the dying LGM's position); without this the
-- engine's respawn of the same idnum after the death-then-rebirth
-- would inherit the previous life's samples / dest, producing bogus
-- predictions for the freshly-parachuted LGM.
-- --------------------------------------------------------------------------
function M.purge_killed(state, kill_wx, kill_wy, tol_wu)
  local hist = state._enemy_lgm_history
  if not hist then return end
  tol_wu = tol_wu or 384  -- 1.5 tiles
  for idnum, h in pairs(hist) do
    local samples = h.samples
    local last = samples and samples[#samples]
    if last then
      local d = math.abs(last.wx - kill_wx) + math.abs(last.wy - kill_wy)
      if d <= tol_wu then
        hist[idnum] = nil
      end
    end
  end
end

-- --------------------------------------------------------------------------
-- M.purge_stale(state, now, visible_lgms) — drop history for any LGM
-- not in the current frame's visible list, plus a fallback STALE_TICKS
-- catch-all for entries that somehow lingered.
--
-- An LGM disappearing from perception (went into its tank, died, walked
-- off screen, fog re-occluded it) needs to clear its history
-- IMMEDIATELY — otherwise a re-acquisition with the same idnum would
-- bleed in the previous trip's samples / phase / dest, producing
-- bogus predictions for the new walk.  `visible_lgms` is the perc
-- enemy_lgms array we just finished populating; anything not in it
-- gets dropped this tick.
-- --------------------------------------------------------------------------
function M.purge_stale(state, now, visible_lgms)
  local hist = state._enemy_lgm_history
  if not hist then return end
  local visible_set
  if visible_lgms then
    visible_set = {}
    for _, lgm in ipairs(visible_lgms) do
      if lgm.idnum then visible_set[lgm.idnum] = true end
    end
  end
  for idnum, h in pairs(hist) do
    if (visible_set and not visible_set[idnum])
       or (now - h.last_tick > STALE_TICKS) then
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
  -- shells.c:133 (post-PR-77 formula):
  --   shellLifeTicks(len) = 1 + SHELL_LIFE(8)*len - SHELL_START_ADD(6)
  -- Tank passes len = sightLen/2 (brain_pathfinder.c:3221), so:
  --   ticks-to-impact = 1 + 8*(sightLen/2) - 6 = 4*sightLen - 5
  -- The +1 was restored in PR #77 to match the WinBolo v1 formula
  -- (commit 63888991 had dropped it as a trajectory-dot display fix
  -- but that shortened all shell lifetimes by one tick).  Without
  -- PR #77 the formula is `4*sightLen - 6` — undo this `-5` back to
  -- `-6` if reverting.
  local t = 4 * sightLen - 5
  if t < 1 then return 1 end
  return t
end

-- --------------------------------------------------------------------------
-- M.predict_aim(tank_wx, tank_wy, lgm, h)   -- h = per-LGM history entry
--   → aim_wx, aim_wy, sightLen, flight_ticks, distance_wu, tier
--
-- Two-tier predictor:
--   dest_lock: 3-window straight-line match locked a map-edge dest;
--              sim_forward_to_dest produces engine-faithful motion
--              (terrain-aware v_max, wall sliding, corner blocks).
--   linear:    fallback when no 3-match yet — pos + v*T, two-pass refit.
-- Both use the same two-pass arrival-time refit on the OUTPUT to pick
-- the final sightLen.
-- --------------------------------------------------------------------------
-- Empirical scales on lead extrapolation.  predict_aim's T values are
-- sim-tick units (from flight_ticks).  Both the linear extrapolation
-- and the engine-sim path empirically over-lead in BrainTest by the
-- same factor — the brain↔sim tick ratio plus the engine's per-tick
-- motion budgets don't line up with the shell flight clock the way
-- the textbook math suggests.  Both scales tuned empirically (currently
-- 0.77) and kept separate so they can be tuned independently later.
local LINEAR_LEAD_SCALE = 0.77
local DEST_LEAD_SCALE   = 0.77
M.LINEAR_LEAD_SCALE = LINEAR_LEAD_SCALE
M.DEST_LEAD_SCALE   = DEST_LEAD_SCALE

function M.predict_aim(tank_wx, tank_wy, lgm, h)
  -- All math here is float-precision (Lua / always produces floats;
  -- integer operands get promoted on first float op).  No floor/round
  -- in the predictor path so partial-tile positions stay exact.
  local lgm_wx = lgm.wx + 0.0   -- explicit float promotion
  local lgm_wy = lgm.wy + 0.0
  local vx = (h and h.v_ema_x) or lgm.v_ema_x or 0.0
  local vy = (h and h.v_ema_y) or lgm.v_ema_y or 0.0
  local tier = "linear"
  local dest_wx, dest_wy
  if h and h.dest_locked and h.dest_wx and h.dest_wy then
    dest_wx, dest_wy = h.dest_wx, h.dest_wy
    tier = "dest_lock"
  end

  -- Linear tier predicts with whatever velocity we have — even on the
  -- very first frame after a fresh sighting (perception stamps a
  -- 1-tick delta as lgm.vx/vy until update_velocity has 2+ samples,
  -- then update_velocity returns (cur − oldest_in_window) / dt over
  -- however many ticks DO exist, not waiting for the full 10-tick
  -- window).  So we always have some velocity to project with from
  -- the moment we acquire the LGM.

  local dx = tank_wx - lgm_wx
  local dy = tank_wy - lgm_wy
  local D  = math.sqrt(dx * dx + dy * dy)
  local T1 = M.flight_ticks(M.sightlen_for(D))

  local pos1_wx, pos1_wy
  if dest_wx then
    -- Apply DEST_LEAD_SCALE by reducing the number of engine-sim
    -- iterations rather than scaling the final position — keeps the
    -- trajectory shape (wall-slides, terrain transitions) intact, just
    -- predicts fewer ticks ahead.
    local sT1 = math.floor(T1 * DEST_LEAD_SCALE + 0.5)
    if sT1 < 1 then sT1 = 1 end
    pos1_wx, pos1_wy = M.sim_forward_to_dest(lgm_wx, lgm_wy, dest_wx, dest_wy, sT1)
  else
    pos1_wx = lgm_wx + vx * T1 * LINEAR_LEAD_SCALE
    pos1_wy = lgm_wy + vy * T1 * LINEAR_LEAD_SCALE
  end

  local pdx = tank_wx - pos1_wx
  local pdy = tank_wy - pos1_wy
  local pD  = math.sqrt(pdx * pdx + pdy * pdy)
  local T2  = M.flight_ticks(M.sightlen_for(pD))
  local aim_wx, aim_wy = pos1_wx, pos1_wy
  if T2 > T1 then
    local extra = T2 - T1
    if dest_wx then
      local sExtra = math.floor(extra * DEST_LEAD_SCALE + 0.5)
      if sExtra >= 1 then
        aim_wx, aim_wy = M.sim_forward_to_dest(pos1_wx, pos1_wy, dest_wx, dest_wy, sExtra)
      end
    else
      aim_wx = pos1_wx + vx * extra * LINEAR_LEAD_SCALE
      aim_wy = pos1_wy + vy * extra * LINEAR_LEAD_SCALE
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

-- --------------------------------------------------------------------------
-- M.tuning() — the ONE place that reads the LGM-kill master switch.
--
-- C.LGM_KILL_IMPROVED gates the whole 2026-09-13 package: the wider fire gate,
-- the 10-tile candidate track radius, the nearest-to-pill pick and the sight
-- that follows the candidate from admission.  OFF (and PRESETS.keel sets it
-- off) every value below is the pre-package one, so the brain plays exactly as
-- it did before: fire gate 64 wu, candidate admission by the old along-the-ray
-- test, min-perp ranking, a held range key from 2 steps out.
--
-- The individual knobs stay the VALUES; the master decides whether they are
-- read at all.  Callers take ONE table and test one field, instead of asking
-- `if C.LGM_KILL_IMPROVED` at every gate.
--
-- The table is built once and cached: preset= / cfg= / level overrides are all
-- applied at chunk load (init.lua, right after require("constants")), long
-- before the first think, so nothing can change under us afterwards.  Each bot
-- has its own lua_State, so this cache is this bot's alone.
-- --------------------------------------------------------------------------
local _tuning

function M.tuning()
  if _tuning then return _tuning end
  local on = C.LGM_KILL_IMPROVED and true or false
  local fire_wu = on and (C.LGM_KILL_FIRE_WU or 256) or 64
  -- Capture-hunt shot: its own override when set (>= 0), else the shared gate.
  local cap_wu = fire_wu
  if on then
    local o = C.CAPTURE_LGM_HUNT_FIRE_WU or -1
    if o >= 0 then cap_wu = o end
  end
  -- Open-ground "can this heading kill him at all" test (perp distance from the
  -- shell ray to the man).  Never TIGHTER than the old 128: with the package off
  -- it is exactly 128, and with a wider fire gate it opens to match the gate, so
  -- the perp test can never refuse a shot the fire gate would have taken.
  local perp_wu = 128
  if on and fire_wu > perp_wu then perp_wu = fire_wu end
  _tuning = {
    on              = on,
    fire_wu         = fire_wu,
    capture_fire_wu = cap_wu,
    perp_wu         = perp_wu,
    engine_kill_wu  = C.LGM_ENGINE_KILL_WU or 128,
    -- Candidate admission: with the package ON, a straight-line radius from the
    -- TANK in wu; nil means the old along-the-ray admission (see init.lua).
    track_wu        = on and ((C.CAPTURE_LGM_HUNT_TRACK_TILES or 10) * 256) or nil,
    pick_near_pill  = on and (C.CAPTURE_LGM_HUNT_PICK_NEAREST_PILL and true or false) or false,
    hold_min_steps  = on and (C.CAPTURE_SIGHT_HOLD_MIN_STEPS or 2) or 2,
    -- Aim target.  nil with the package off, which switches the whole
    -- refinement search out and leaves the old aim exactly as it was.
    aim_wu          = on and (C.LGM_KILL_AIM_WU or 64) or nil,
    aim_throttle    = on and (C.LGM_KILL_AIM_THROTTLE and true or false) or false,
  }
  return _tuning
end

return M
