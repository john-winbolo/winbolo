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
-- M.sightlen_for(distance_wu) → integer sightLen in [2, 14]
--
-- Returns the GUNSIGHT step that lands a shell closest to distance_wu.
-- sightLen is in HALF-TILES per the brain's convention (see GUNSIGHT_MAX
-- comment in constants.lua: max=14 → 7 map tiles), so per-unit shell
-- travel = 128 wu.  Inverse: sightLen = round(distance_wu / 128).
-- --------------------------------------------------------------------------
function M.sightlen_for(distance_wu)
  local s = math.floor(distance_wu / 128 + 0.5)
  if s < 2  then return 2  end
  if s > 14 then return 14 end
  return s
end

-- --------------------------------------------------------------------------
-- M.flight_ticks(sightLen) → ticks until shell explodes
--
-- Each sightLen unit of shell travel = 128 wu (½ tile, per the brain's
-- half-tile sightLen convention).  At SHELL_SPEED ≈ 32 wu/tick effective,
-- that's 4 ticks of flight per sightLen unit.
-- --------------------------------------------------------------------------
function M.flight_ticks(sightLen)
  return sightLen * 4
end

-- --------------------------------------------------------------------------
-- M.predict_aim(tank_wx, tank_wy, lgm_wx, lgm_wy, vx, vy)
--   → aim_wx, aim_wy, sightLen, flight_ticks, distance_wu
--
-- Single-pass lead predictor (no convergence loop).  Computes the
-- shell's flight time from current distance, projects the LGM along
-- v_ema by that many ticks, returns the projected aim point + the
-- sightLen needed to reach it.  Replaces the older 3-iter fixed-point
-- version (archived in kill_lgm_convergence_DELETEME.lua).
-- --------------------------------------------------------------------------
function M.predict_aim(tank_wx, tank_wy, lgm_wx, lgm_wy, vx, vy)
  vx = vx or 0
  vy = vy or 0
  local dx = tank_wx - lgm_wx
  local dy = tank_wy - lgm_wy
  local D = math.sqrt(dx * dx + dy * dy)
  local sightLen = M.sightlen_for(D)
  local T = M.flight_ticks(sightLen)
  local aim_wx = lgm_wx + vx * T
  local aim_wy = lgm_wy + vy * T
  -- Re-fit sightLen to the projected distance so the caller sees a
  -- gun-range that actually lands on the lead point, not on the
  -- current LGM tile.
  local pdx = tank_wx - aim_wx
  local pdy = tank_wy - aim_wy
  local pD  = math.sqrt(pdx * pdx + pdy * pdy)
  sightLen = M.sightlen_for(pD)
  return aim_wx, aim_wy, sightLen, M.flight_ticks(sightLen), pD
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
