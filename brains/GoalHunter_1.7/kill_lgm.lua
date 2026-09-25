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
-- Predictor — tiers, picked per call:
--   0. dest_pill / dest_pill_lead (LGM_DEST_AIM) — his recent positions lie
--                  on one clean line that runs into a dead hostile pill
--                  (find_dest_pill).  Engine sim toward the PILL CENTRE,
--                  held there on arrival; an impact inside the pill tile
--                  snaps to its centre (tier dest_pill), else the lead
--                  point on the way in (dest_pill_lead).
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
-- M.sim_forward_to_dest(lgm_wx, lgm_wy, dest_wx, dest_wy, T [, bless_mx, bless_my])
--   → end_wx, end_wy
--
-- Engine-faithful T-tick LGM simulation toward a fixed destination.
-- Mirrors lgm.c:668-730:
--   1. angle = utilCalcAngle(cur, dest)
--   2. (xAdd, yAdd) = v_max * (cos, sin)  [v_max from cur tile]
--   3. Try diagonal move; on blocked axis, slide along the other axis;
--      both blocked = stop.
--
-- Optional bless_mx/bless_my (LGM_DEST_AIM only; nil = the old sim exactly):
-- the tile the man was sent to.  lgmMoveAway walks him at
-- man_speed_refuel_base (16) while he stands on it and lets him step onto it
-- (a dead pill: pillsDeadPos -> terrain speed; the brain map only says
-- T_PILLBOX there, speed 0, so the plain sim would stall at its edge).  He
-- stops once within lgm_arrive_tolerance (LGM_MAX_GOAL, 16 wu) of the dest
-- on both axes (lgm.c:838), and the sim stops there too, AT the dest.
-- --------------------------------------------------------------------------
local LGM_ARRIVE_WU = 16  -- lgm.h LGM_MAX_GOAL (sim_rules lgm_arrive_tolerance)

function M.sim_forward_to_dest(lgm_wx, lgm_wy, dest_wx, dest_wy, T, bless_mx, bless_my)
  local wx, wy = lgm_wx, lgm_wy
  for _ = 1, T do
    local cur_mx = bit.rshift(math.floor(wx), 8)
    local cur_my = bit.rshift(math.floor(wy), 8)
    local v_max
    if bless_mx and cur_mx == bless_mx and cur_my == bless_my then
      v_max = C.MAN_SPEED_BLESSED
    else
      v_max = man_speed_at(cur_mx, cur_my)
    end
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
    local x_blocked = (new_mx ~= cur_mx)
                      and not (bless_mx and new_mx == bless_mx and cur_my == bless_my)
                      and is_blocked(new_mx, cur_my)
    local y_blocked = (new_my ~= cur_my)
                      and not (bless_mx and cur_mx == bless_mx and new_my == bless_my)
                      and is_blocked(cur_mx, new_my)
    if x_blocked and y_blocked then break end
    if not x_blocked then wx = nx end
    if not y_blocked then wy = ny end
    if bless_mx and math.abs(wx - dest_wx) <= LGM_ARRIVE_WU
                and math.abs(wy - dest_wy) <= LGM_ARRIVE_WU then
      return dest_wx, dest_wy
    end
  end
  return wx, wy
end

-- --------------------------------------------------------------------------
-- M.find_dest_pill(h, lgm, pills, tanks, now) -> pill, ray, reason
--
-- LGM_DEST_AIM: is the man walking a clean straight line at a dead hostile
-- pill (or, with LGM_DEST_LIVE_PILLS, a live hostile pill below full armour:
-- he walks to both to repair them)?  Reads the per-LGM history samples update_velocity keeps (one per
-- brain tick, 16-wu quantized positions).
--
--   pill   = { id, mx, my, cx, cy, along, perp, tol, health, live } or nil
--   ray    = the fitted heading, for the overlay and the logs:
--            { ax, ay (oldest sample in the window), ox, oy (newest = ray
--              origin), ux, uy (unit), len (wu), perp_wu, perp_tan,
--              speed } -- nil when no heading was accepted
--   reason = "lock" or why nothing was locked
--
-- A heading is accepted only when, over the last LGM_DEST_WINDOW_TICKS:
--   * there are >= LGM_DEST_MIN_SAMPLES samples
--   * every sample is within LGM_DEST_COLLINEAR_WU of the oldest->newest
--     chord                                                    (else zigzag)
--   * no sample falls more than LGM_DEST_BACKSTEP_WU behind the furthest one
--     before it, along the chord                          (else turned_back)
--   * speed over the window and over the last 4 ticks >= LGM_DEST_MIN_SPEED
--                                                              (else stopped)
--   * speed >= LGM_DEST_MIN_SPEED_FRAC x the mean terrain man-speed of the
--     tiles he stood on (engine slides along a blocked axis at a fraction of
--     it: stuck on a wall / water edge)                        (else sliding)
--   * his own tank (near_tank_idnum), if visible, is not ahead on the ray
--     within 1 tile of it (walking home, not to a job)       (else returning)
-- Then the nearest candidate pill whose centre is ahead of him, no further
-- than LGM_DEST_RAY_TILES along the ray, and within
-- max(LGM_DEST_PERP_WU, along * tan(LGM_DEST_PERP_DEG)) of it is the lock
-- (no pill -> no_pill).  A second candidate within 128 wu of the same depth
-- along the ray -> ambiguous, nothing locked.
-- --------------------------------------------------------------------------
local RECENT_TICKS   = 4     -- "still moving right now" check span
local RETURN_PERP_WU = 256   -- his tank this close to the ray, ahead = returning
local AMBIG_ALONG_WU = 128   -- two candidates this close in depth = ambiguous

function M.find_dest_pill(h, lgm, pills, tanks, now)
  local samples = h and h.samples
  if not samples or #samples == 0 then return nil, nil, "no_history" end
  local first
  for i = 1, #samples do
    if (now - samples[i].tick) <= C.LGM_DEST_WINDOW_TICKS then first = i; break end
  end
  if not first or (#samples - first + 1) < C.LGM_DEST_MIN_SAMPLES then
    return nil, nil, "warming"
  end
  local a, b = samples[first], samples[#samples]
  local span = b.tick - a.tick
  local cdx, cdy = b.wx - a.wx, b.wy - a.wy
  local clen = math.sqrt(cdx * cdx + cdy * cdy)
  if span <= 0 then return nil, nil, "warming" end
  local speed = clen / span
  if speed < C.LGM_DEST_MIN_SPEED or clen < 1 then return nil, nil, "stopped" end
  local ux, uy = cdx / clen, cdy / clen

  local far_along = -math.huge
  for i = first, #samples do
    local s = samples[i]
    local rx, ry = s.wx - a.wx, s.wy - a.wy
    local along = rx * ux + ry * uy
    local perp  = math.abs(rx * uy - ry * ux)
    if perp > C.LGM_DEST_COLLINEAR_WU then return nil, nil, "zigzag" end
    if along < far_along - C.LGM_DEST_BACKSTEP_WU then return nil, nil, "turned_back" end
    if along > far_along then far_along = along end
  end

  -- Moving RIGHT NOW: the window can be straight and fast overall while the
  -- man stopped a few ticks ago (arrived, or walked into something).
  local r
  for i = #samples - 1, first, -1 do
    if (b.tick - samples[i].tick) >= RECENT_TICKS then r = samples[i]; break end
  end
  if r then
    local rdx, rdy = b.wx - r.wx, b.wy - r.wy
    if math.sqrt(rdx * rdx + rdy * rdy) / (b.tick - r.tick) < C.LGM_DEST_MIN_SPEED then
      return nil, nil, "stopped"
    end
  end

  if (C.LGM_DEST_MIN_SPEED_FRAC or 0) > 0 then
    local sum, n = 0, 0
    for i = first, #samples - 1 do
      local s = samples[i]
      local v = man_speed_at(bit.rshift(math.floor(s.wx), 8), bit.rshift(math.floor(s.wy), 8))
      if v > 0 then sum = sum + v; n = n + 1 end  -- 0 = pill/unknown tile: no opinion
    end
    if n > 0 and speed < C.LGM_DEST_MIN_SPEED_FRAC * (sum / n) then
      return nil, nil, "sliding"
    end
  end

  local ray = {
    ax = a.wx, ay = a.wy, ox = b.wx, oy = b.wy, ux = ux, uy = uy,
    len = C.LGM_DEST_RAY_TILES * 256,
    perp_wu = C.LGM_DEST_PERP_WU,
    perp_tan = math.tan(math.rad(C.LGM_DEST_PERP_DEG)),
    speed = speed,
  }

  if lgm and lgm.near_tank_idnum ~= nil and tanks then
    for _, t in ipairs(tanks) do
      if t.id == lgm.near_tank_idnum and t.wx and t.wy then
        local rx, ry = t.wx - b.wx, t.wy - b.wy
        local along = rx * ux + ry * uy
        if along > 0 and math.abs(rx * uy - ry * ux) <= RETURN_PERP_WU then
          return nil, ray, "returning"
        end
        break
      end
    end
  end

  local best, second
  for id, p in pairs(pills or {}) do
    -- Dead (armour 0), or with LGM_DEST_LIVE_PILLS live but below full
    -- armour: the engine refuses a repair walk to a full pill
    -- (lgm.c:460-462 LGM_PILL_NO_NEED_REPAIR), so a full pill is never his job.
    local hp = p.health or 1
    if (hp == 0 or (C.LGM_DEST_LIVE_PILLS and hp > 0 and hp < C.PILLS_MAX_HEALTH))
       and p.owner == "hostile"
       and not (p.in_tank or p.carrier or p._synth_carry) then
      local cx, cy = p.mx * 256 + 128, p.my * 256 + 128
      local rx, ry = cx - b.wx, cy - b.wy
      local along = rx * ux + ry * uy
      if along > 0 and along <= ray.len then
        local perp = math.abs(rx * uy - ry * ux)
        local tol = math.max(ray.perp_wu, along * ray.perp_tan)
        if perp <= tol then
          local c = { id = id, mx = p.mx, my = p.my, cx = cx, cy = cy,
                      along = along, perp = perp, tol = tol,
                      health = hp, live = hp > 0 }
          -- Order by depth, ties by id: pairs() order must not decide.
          if not best or along < best.along
             or (along == best.along and id < best.id) then
            best, second = c, best
          elseif not second or along < second.along
                 or (along == second.along and id < second.id) then
            second = c
          end
        end
      end
    end
  end
  if not best then return nil, ray, "no_pill" end
  if second and (second.along - best.along) < AMBIG_ALONG_WU then
    return nil, ray, "ambiguous"
  end
  return best, ray, "lock"
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

-- --------------------------------------------------------------------------
-- LGM_DEST_HOLD_FIRE: fire at a locked destination pill only when the shell
-- goes off while the man stands on the pill tile.
--
-- Engine facts the timeline uses:
--   * shell position after t flight ticks = tank + 32*(t+5) wu along the
--     line (SHELL_SPEED 32, the start push; the same clock flight_ticks uses:
--     4*sl - 5 ticks to 128*sl wu).
--   * a LIVE pill (armour > 0) stops the shell on the first tick it is inside
--     the pill tile and the blast goes off at the tile centre
--     (shellsCalcCollision -> pillsIsPillHit, shells.c:722-730).
--   * a DEAD pill does not (pillsIsPillHit needs armour > 0,
--     pillbox.c:652): the shell flies on and ends its life at 128*sl.  The
--     repair on his arrival makes it live at once, so a shell that is inside
--     the tile at or after his arrival tick goes off then.
--   * the tile is solid for the builder death test either way (pillsExistPos
--     has no armour check, lgm.c:1504): the blast kills him only if he is ON
--     the pill tile.
--   * he stands on it for LGM_BUILD_TIME (20) game ticks after arriving
--     (lgm.c:843), then walks home.  The walk off the tile is not counted.
-- Time units: flight ticks x DEST_LEAD_SCALE = man sim steps, the same
-- conversion predict_aim uses for the lead.
-- --------------------------------------------------------------------------
local DWELL_STEPS      = 20  -- lgm.h LGM_BUILD_TIME (sim_rules lgm_build_ticks)
local SHELL_WU_TICK    = 32  -- shells.h SHELL_SPEED
local SHELL_START_TICK = 5   -- flight_ticks: 128*sl wu reached at tick 4*sl - 5
M.DEST_DWELL_STEPS = DWELL_STEPS

-- First flight tick (>= 1) at which a shell fired from the tank at the tile
-- centre is inside tile (mx, my).  Capped at T_full.
function M.shell_tile_entry_ticks(tank_wx, tank_wy, mx, my, T_full)
  local cx, cy = mx * 256 + 128, my * 256 + 128
  local dx, dy = cx - tank_wx, cy - tank_wy
  local D = math.sqrt(dx * dx + dy * dy)
  if D < 1 then return 1 end
  local ux, uy = dx / D, dy / D
  for t = 1, T_full do
    local d = SHELL_WU_TICK * (t + SHELL_START_TICK)
    local x, y = tank_wx + ux * d, tank_wy + uy * d
    if bit.rshift(math.floor(x), 8) == mx and bit.rshift(math.floor(y), 8) == my then
      return t
    end
  end
  return T_full
end

-- Walk the man toward the pill centre one engine-faithful step at a time.
--   s_enter  = first step his tile is the pill tile (0 = on it now), nil if
--              not within max_steps
--   s_arrive = step he arrives (sim holds him at the centre), nil if not
--              within max_steps
--   stalled  = true when the sim stopped him before he reached the tile
--              (a wall, or another pill on the way: the brain map reads
--              T_PILLBOX there, speed 0)
function M.dest_pill_timeline(lgm_wx, lgm_wy, pill, max_steps)
  local wx, wy = lgm_wx + 0.0, lgm_wy + 0.0
  local s_enter
  if bit.rshift(math.floor(wx), 8) == pill.mx and bit.rshift(math.floor(wy), 8) == pill.my then
    s_enter = 0
  end
  if math.abs(wx - pill.cx) <= LGM_ARRIVE_WU and math.abs(wy - pill.cy) <= LGM_ARRIVE_WU then
    return s_enter or 0, 0, false
  end
  for s = 1, max_steps do
    local nx, ny = M.sim_forward_to_dest(wx, wy, pill.cx, pill.cy, 1, pill.mx, pill.my)
    if nx == wx and ny == wy then return s_enter, nil, s_enter == nil end
    wx, wy = nx, ny
    if not s_enter and bit.rshift(math.floor(wx), 8) == pill.mx
                   and bit.rshift(math.floor(wy), 8) == pill.my then
      s_enter = s
    end
    if wx == pill.cx and wy == pill.cy then return s_enter or s, s, false end
  end
  return s_enter, nil, false
end

-- predict_aim's LGM_DEST_HOLD_FIRE branch.  Aims at the pill centre (the gun
-- is ready when the timing comes right) and says whether that shot kills:
--   tier "dest_pill"      -> the blast goes off while he is on the tile: fire
--   tier "dest_pill_hold" -> it does not ("early": he is not on the tile yet;
--                            "late": he has left): hold the trigger
-- Writes the whole timeline to h.dest_timing (overlay, logs).  Returns nil
-- when the sim cannot walk him to the tile (stalled): predict_aim then uses
-- the old predictor with no hold.
function M.predict_dest_hold(tank_wx, tank_wy, lgm, h, pill)
  local dx, dy = tank_wx - pill.cx, tank_wy - pill.cy
  local D = math.sqrt(dx * dx + dy * dy)
  local sl = M.sightlen_for(D)
  local T_full = M.flight_ticks(sl)
  local T_entry = M.shell_tile_entry_ticks(tank_wx, tank_wy, pill.mx, pill.my, T_full)
  local s_full  = math.floor(T_full * DEST_LEAD_SCALE + 0.5)
  if s_full < 1 then s_full = 1 end
  local s_entry = math.floor(T_entry * DEST_LEAD_SCALE + 0.5)
  if s_entry < 1 then s_entry = 1 end
  local s_enter, s_arrive, stalled = M.dest_pill_timeline(lgm.wx, lgm.wy, pill, s_full)
  if stalled then return nil end
  -- When does the shell go off?
  local T_x, s_x, how
  if pill.live then
    T_x, s_x, how = T_entry, s_entry, "live: collides entering the tile"
  elseif s_arrive and s_arrive <= s_full then
    -- Dead now, repaired (live) on his arrival: goes off at the first tick
    -- it is in the tile with the pill live.
    if s_arrive <= s_entry then
      T_x, s_x = T_entry, s_entry
    else
      s_x = s_arrive
      T_x = math.floor(s_arrive / DEST_LEAD_SCALE + 0.5)
    end
    how = "dead: repaired before the shell leaves the tile"
  else
    T_x, s_x, how = T_full, s_full, "dead: flies over, ends at range"
  end
  local verdict
  if s_enter == nil or s_x < s_enter then
    verdict = "early"
  elseif s_arrive and s_x > s_arrive + DWELL_STEPS then
    verdict = "late"
  else
    verdict = "fire"
  end
  if h then
    h.dest_timing = { verdict = verdict, how = how, live = pill.live,
                      T_full = T_full, T_entry = T_entry, T_x = T_x,
                      s_full = s_full, s_entry = s_entry, s_x = s_x,
                      s_enter = s_enter, s_arrive = s_arrive,
                      dwell = DWELL_STEPS, sl = sl, D = D }
  end
  return pill.cx + 0.0, pill.cy + 0.0, sl, T_x, D,
         (verdict == "fire") and "dest_pill" or "dest_pill_hold"
end

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
  -- LGM_DEST_AIM: a dead pill on his straight-line heading (find_dest_pill,
  -- run by perception) is his destination.  The sim walks him to the pill
  -- centre and holds him there; bless_mx/my lets it step onto the pill tile.
  local pill = C.LGM_DEST_AIM and h and h.dest_pill or nil
  if h and h.dest_timing then h.dest_timing = nil end
  -- LGM_DEST_HOLD_FIRE: aim at the pill, fire only for a blast while he is
  -- on its tile.  nil = the sim cannot walk him there: no pill, old aim.
  if pill and C.LGM_DEST_HOLD_FIRE then
    local a_wx, a_wy, a_sl, a_ft, a_d, a_tier = M.predict_dest_hold(tank_wx, tank_wy, lgm, h, pill)
    if a_wx then return a_wx, a_wy, a_sl, a_ft, a_d, a_tier end
    pill = nil
  end
  local bless_mx, bless_my
  if pill then
    dest_wx, dest_wy = pill.cx, pill.cy
    bless_mx, bless_my = pill.mx, pill.my
    tier = "dest_pill_lead"
  elseif h and h.dest_locked and h.dest_wx and h.dest_wy then
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
    pos1_wx, pos1_wy = M.sim_forward_to_dest(lgm_wx, lgm_wy, dest_wx, dest_wy, sT1,
                                             bless_mx, bless_my)
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
        aim_wx, aim_wy = M.sim_forward_to_dest(pos1_wx, pos1_wy, dest_wx, dest_wy, sExtra,
                                               bless_mx, bless_my)
      end
    else
      aim_wx = pos1_wx + vx * extra * LINEAR_LEAD_SCALE
      aim_wy = pos1_wy + vy * extra * LINEAR_LEAD_SCALE
    end
  end

  -- LGM_DEST_AIM: he is on the pill tile when the shell lands -> aim at the
  -- tile CENTRE.  The dead pill does not stop the shell (pillsIsPillHit needs
  -- armour > 0), so it must end its life in this tile; the tile is SOLID for
  -- lgmDeathCheckAtPosition (pillsExistPos ignores armour), so the blast kills
  -- a man anywhere on this tile and nobody off it -- the centre has the most
  -- margin.  Once he repairs it (armour > 0 on arrival) any shell entering
  -- the tile explodes at its centre anyway (shellsCalcCollision).  A LIVE
  -- damaged pill (LGM_DEST_LIVE_PILLS) stops the shell as it enters the tile,
  -- blast at the centre: the centre aim puts that blast on the tile too.
  if pill and bit.rshift(math.floor(aim_wx), 8) == pill.mx
          and bit.rshift(math.floor(aim_wy), 8) == pill.my then
    aim_wx, aim_wy = pill.cx + 0.0, pill.cy + 0.0
    tier = "dest_pill"
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
