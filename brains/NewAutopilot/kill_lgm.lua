-- =========================================================================
-- NewAutopilot/kill_lgm.lua — enemy LGM targeting helpers.
--
-- Pure math + per-LGM history tracking.  No viz, no I/O.  Used by:
--   * perception.lua  — updates EMA velocity per enemy LGM each tick
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
-- terrain (lgm.c:1321).  Shell travel distance = SHELL_SPEED *
-- shellLifeTicks(sightLen) = 32 * (8*sightLen - 6) = 256*sightLen - 192 wu.
-- Inverse: sightLen = round((D + 192) / 256), clamped to [2, 13]
-- (GUNSIGHT_MAX is 13.875, so 13 is the practical max integer key step).
-- =========================================================================

local M = {}

local C = require("constants")

-- EMA mix: 0.4 = ~3-tick effective window.  Raw 1-tick velocities are
-- jittery; smoothing them lets the predictor see real direction even
-- when the LGM is dodging building corners.  Lower alpha = smoother but
-- laggier on direction changes; 0.4 was the sweet spot in pencil tests.
local V_EMA_ALPHA = 0.4

-- Drop history entries we haven't seen for this many ticks.  Covers LGMs
-- that went into a tank, were killed, or drifted out of perception.
local STALE_TICKS = 250  -- 5s @ 50 Hz

-- Splash tolerance for "good enough" range alignment.  Half a tile.
local SPLASH_WU = 128

-- Convergence step cap.  3 iterations gets within splash on every
-- realistic geometry; we early-exit when |ΔD| < SPLASH_WU anyway.
local MAX_ITERS = 3

-- --------------------------------------------------------------------------
-- M.update_velocity(state, lgm, now) → v_ema_x, v_ema_y (wu/tick)
--
-- Per-LGM EMA velocity keyed by lgm.idnum.  Call once per LGM per tick
-- from perception, BEFORE consumers read v_ema.  Falls back to the
-- caller-provided lgm.vx/vy when no history exists yet (first sighting).
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
      last_wx = lgm.wx, last_wy = lgm.wy, last_tick = now,
      v_ema_x = lgm.vx or 0, v_ema_y = lgm.vy or 0,
    }
    hist[lgm.idnum] = h
    return h.v_ema_x, h.v_ema_y
  end
  local dt = now - h.last_tick
  if dt > 0 and dt < 20 then
    local raw_vx = (lgm.wx - h.last_wx) / dt
    local raw_vy = (lgm.wy - h.last_wy) / dt
    h.v_ema_x = V_EMA_ALPHA * raw_vx + (1 - V_EMA_ALPHA) * h.v_ema_x
    h.v_ema_y = V_EMA_ALPHA * raw_vy + (1 - V_EMA_ALPHA) * h.v_ema_y
  end
  h.last_wx, h.last_wy, h.last_tick = lgm.wx, lgm.wy, now
  return h.v_ema_x, h.v_ema_y
end

-- --------------------------------------------------------------------------
-- M.purge_stale(state, now) — drop history rows older than STALE_TICKS.
-- Cheap pairs() walk; call once per tick.  Prevents the table from
-- growing unbounded as idnums churn over the match.
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
-- M.sightlen_for(distance_wu) → integer sightLen in [2, 13]
--
-- Returns the GUNSIGHT step that lands a shell closest to distance_wu.
-- Inverse of the shell-travel formula (256*sightLen - 192 wu).
-- Clamps to [2, 13] (engine limits min 2 max 14; 13 is the practical
-- integer max since GUNSIGHT_MAX is 13.875).
-- --------------------------------------------------------------------------
function M.sightlen_for(distance_wu)
  local s = math.floor((distance_wu + 192) / 256 + 0.5)
  if s < 2  then return 2  end
  if s > 13 then return 13 end
  return s
end

-- --------------------------------------------------------------------------
-- M.flight_ticks(sightLen) → ticks until shell explodes
--
-- shellLifeTicks(len) = SHELL_LIFE * len - SHELL_START_ADD = 8*len - 6.
-- Used by the convergence loop to walk the LGM forward by the same
-- number of ticks the shell takes to arrive.
-- --------------------------------------------------------------------------
function M.flight_ticks(sightLen)
  return 8 * sightLen - 6
end

-- --------------------------------------------------------------------------
-- M.predict_aim(tank_wx, tank_wy, lgm_wx, lgm_wy, vx, vy)
--   → aim_wx, aim_wy, sightLen, flight_ticks, distance_wu
--
-- Converges the lead-predict feedback loop:
--   T = flight_ticks(sightLen(D))
--   D' = wdist(tank, lgm + v*T)
-- repeat until |D' - D| < SPLASH_WU or MAX_ITERS hit.  Returns the
-- world-unit aim point + the integer sightLen we should drive crosshair
-- toward.  Cost: ~3 iters * a handful of FP ops + integer divs.
-- --------------------------------------------------------------------------
function M.predict_aim(tank_wx, tank_wy, lgm_wx, lgm_wy, vx, vy)
  vx = vx or 0
  vy = vy or 0
  local aim_wx, aim_wy = lgm_wx, lgm_wy
  local D = math.abs(tank_wx - aim_wx) + math.abs(tank_wy - aim_wy)
  local sightLen = M.sightlen_for(D)
  local T = M.flight_ticks(sightLen)
  for _ = 1, MAX_ITERS do
    local new_aim_wx = lgm_wx + vx * T
    local new_aim_wy = lgm_wy + vy * T
    local new_D = math.abs(tank_wx - new_aim_wx) + math.abs(tank_wy - new_aim_wy)
    local new_sightLen = M.sightlen_for(new_D)
    local new_T = M.flight_ticks(new_sightLen)
    -- Converged: distance change below splash tolerance.
    if math.abs(new_D - D) < SPLASH_WU then
      aim_wx, aim_wy, sightLen, T = new_aim_wx, new_aim_wy, new_sightLen, new_T
      break
    end
    aim_wx, aim_wy, sightLen, T, D = new_aim_wx, new_aim_wy, new_sightLen, new_T, new_D
  end
  return aim_wx, aim_wy, sightLen, T, D
end

-- --------------------------------------------------------------------------
-- M.gunrange_key(current_sightLen, target_sightLen) → bitmask
--
-- Returns the KEY_MORERANGE / KEY_LESSRANGE bit (or 0) needed to nudge
-- the engine's sightLen toward the target.  Each press steps by 1 per
-- tick (tank.c:941/964), so the caller should set this every tick the
-- LGM is being targeted.
-- --------------------------------------------------------------------------
function M.gunrange_key(current_sightLen, target_sightLen)
  if not current_sightLen or not target_sightLen then return 0 end
  if current_sightLen < target_sightLen then return KEY_MORERANGE end
  if current_sightLen > target_sightLen then return KEY_LESSRANGE end
  return 0
end

M.SPLASH_WU = SPLASH_WU

return M
