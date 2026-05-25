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

-- Velocity sampling window: 3 brain ticks.  Velocity = (cur_pos -
-- pos_3_ticks_ago) / 3.  Replaces the prior EMA-smoothed approach; the
-- finite-difference window is short enough to track wall-sliding
-- transitions but long enough to wash out 1-tick position quantization
-- jitter from the engine's integer wu math.  When the LGM has fewer
-- than 3 ticks of history, we use the oldest available sample.
local V_WINDOW_TICKS = 10

-- Drop history entries we haven't seen for this many ticks.  Covers LGMs
-- that went into a tank, were killed, or drifted out of perception.
local STALE_TICKS = 250  -- 5s @ 50 Hz

-- Splash tolerance for "good enough" range alignment.  Half a tile.
local SPLASH_WU = 128

-- Convergence step cap.  3 iterations gets within splash on every
-- realistic geometry; we early-exit when |ΔD| < SPLASH_WU anyway.
local MAX_ITERS = 3

-- --------------------------------------------------------------------------
-- M.update_velocity(state, lgm, now) → vx, vy (wu/tick)
--
-- Per-LGM finite-difference velocity keyed by lgm.idnum.  Maintains a
-- short ring of recent (wx, wy, tick) samples; returns (cur −
-- sample_3_ticks_ago) / Δticks.  Falls back to the oldest sample
-- available when fewer than V_WINDOW_TICKS of history have accumulated.
-- Call once per LGM per tick from perception, BEFORE consumers read
-- v_ema_x/y on the LGM entry.
--
-- Field names on the history record (h.v_ema_x / h.v_ema_y) are kept
-- for downstream compat — consumers don't care that it's no longer an
-- EMA.
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
    }
    hist[lgm.idnum] = h
    return h.v_ema_x, h.v_ema_y
  end
  -- Append current sample, drop entries older than V_WINDOW_TICKS+1.
  local samples = h.samples
  samples[#samples + 1] = { wx = lgm.wx, wy = lgm.wy, tick = now }
  while samples[1] and (now - samples[1].tick) > V_WINDOW_TICKS do
    -- Keep at least one sample older-than-or-equal to V_WINDOW_TICKS
    -- so we always have a reference point; drop only if the *second*
    -- sample is still at-or-past the window.
    if samples[2] and (now - samples[2].tick) >= V_WINDOW_TICKS then
      table.remove(samples, 1)
    else
      break
    end
  end
  -- Velocity = (cur - oldest-in-window) / Δticks.
  local oldest = samples[1]
  local dt = now - oldest.tick
  if dt > 0 then
    h.v_ema_x = (lgm.wx - oldest.wx) / dt
    h.v_ema_y = (lgm.wy - oldest.wy) / dt
  end
  h.last_tick = now
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
-- Two-pass lead predictor with simple linear extrapolation:
--   1. T1 = flight ticks for current tank↔LGM distance.
--      pos1 = lgm + v * T1
--   2. T2 = flight ticks for tank↔pos1 distance.
--      If T2 > T1 (LGM moved away → shell takes longer), advance
--      another (T2 − T1) ticks from pos1.
-- Half-tile splash (128 wu) makes a single re-pass good enough.
--
-- v is the lookback-window velocity from M.update_velocity
-- ((cur_pos - pos_N_ticks_ago) / N).  The N-tick window is long enough
-- that wall-sliding shows up as the actual sliding velocity, so naive
-- linear extrapolation tracks the LGM along the wall.
--
-- See WIP_kill_lgm_wall_aware_predict.md for a removed alternative
-- that recovered the LGM's TRUE destination angle from wall-cancelled
-- v_ema components — saved for revival if the linear predictor proves
-- insufficient on wall-heavy maps.
-- --------------------------------------------------------------------------
function M.predict_aim(tank_wx, tank_wy, lgm_wx, lgm_wy, vx, vy)
  vx = vx or 0
  vy = vy or 0

  local dx = tank_wx - lgm_wx
  local dy = tank_wy - lgm_wy
  local D  = math.sqrt(dx * dx + dy * dy)
  local T1 = M.flight_ticks(M.sightlen_for(D))
  local pos1_wx = lgm_wx + vx * T1
  local pos1_wy = lgm_wy + vy * T1

  local pdx = tank_wx - pos1_wx
  local pdy = tank_wy - pos1_wy
  local pD  = math.sqrt(pdx * pdx + pdy * pdy)
  local T2  = M.flight_ticks(M.sightlen_for(pD))
  local aim_wx, aim_wy = pos1_wx, pos1_wy
  if T2 > T1 then
    local extra = T2 - T1
    aim_wx = pos1_wx + vx * extra
    aim_wy = pos1_wy + vy * extra
  end

  local fdx = tank_wx - aim_wx
  local fdy = tank_wy - aim_wy
  local fD  = math.sqrt(fdx * fdx + fdy * fdy)
  local sightLen = M.sightlen_for(fD)
  return aim_wx, aim_wy, sightLen, M.flight_ticks(sightLen), fD
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
