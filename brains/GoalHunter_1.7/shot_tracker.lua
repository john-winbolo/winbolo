local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/shot_tracker.lua — track our own fired shots until impact
--
-- Each tick we:
--   1. Detect shells fired this tick (info.shells decrement) and create a
--      tracked shot record with the tank position + crosshair location at
--      the moment of fire.
--   2. Advance every in-flight shot's simulated position by one shell step
--      (step = bsin/bcos(dir) * SHELL_SPEED / 128, matching the engine).
--   3. Cross-reference each in-flight shot against info.objects: if no
--      OBJECT_SHOT entry is within MATCH_TOLERANCE_WU of our simulated
--      position, the shot has died (impact, expiry, or missed-snapshot).
--      Mark it dead and record the death tick + position.
--   4. Trim the queue to MAX_SHOTS records (oldest dropped).
--
-- This gives the brain a verifiable answer to "did my shot hit the pill"
-- — for each fired shot we know where it died and at what tick. No
-- shooter-id is provided by the engine, but info.shells decrements only
-- on OUR fires so the source attribution is reliable.
--
-- Module state lives on M.* (not closure-private) so the snapshot
-- serializer captures it for trace replay.
-- =========================================================================

local C = require("constants")
local U = require("util")
local viz = require("viz")

local M = {}

-- Queue of recent shots (oldest first). Each entry:
--   {
--     fire_tick    = tick fired,
--     fire_fx      = tank world X at fire time (wu),
--     fire_fy      = tank world Y at fire time (wu),
--     dir          = bolo angle 0-255 at fire time,
--     step_x       = per-tick velocity X (wu)
--     step_y       = per-tick velocity Y (wu)
--     cross_fx     = crosshair world X at fire time (wu, where the tank was aiming)
--     cross_fy     = crosshair world Y at fire time (wu)
--     cur_fx       = current simulated X (wu)
--     cur_fy       = current simulated Y (wu)
--     status       = "in_flight" | "dead",
--     dead_tick    = tick when status became dead (nil while in flight)
--     dead_fx      = X where it died (nil while in flight)
--     dead_fy      = Y where it died (nil while in flight)
--   }
M.shots = {}

-- Last seen shell count, used to detect "fired this tick"
M.prev_shells = nil

-- Cap the queue at this many entries
M.MAX_SHOTS = 30

-- A simulated shot is considered alive if some OBJECT_SHOT in info.objects
-- is within this many world units of its predicted position. SHELL_SPEED
-- is 32 wu/tick so a shot moves ~32 wu per tick — generous tolerance for
-- rounding / sub-tick differences.
M.MATCH_TOLERANCE_WU = 96

-- Maximum brain ticks a shot can live before we declare it expired. Shell
-- gun range is ~8 tiles = 2048 wu, step is 32 wu/brain-tick, so ~64 ticks
-- covers the full range. A bit of slack avoids cutting off late impacts.
M.MAX_LIFE_TICKS = 72

-- If a shot has NEVER gotten a green match (was_matched) within this many
-- ticks, clear the blue bubble — prediction diverged or the engine didn't
-- spawn the OBJECT_SHOT. Give a few ticks for the snapshot to catch up.
M.INITIAL_MATCH_TICKS = 8

function M.reset()
  for i = #M.shots, 1, -1 do M.shots[i] = nil end
  M.prev_shells = nil
end

-- Build a new shot record for a fire that happened this tick.
local function make_shot(info, tick)
  local dir = info.direction or 0
  -- Replicate the engine's utilCalcDistance(angle, SHELL_SPEED) exactly:
  --   a   = (angle - 64) mod 256                  -- BRADIANS_EAST = 64
  --   rad = a * 2π / 256
  --   xAdd = round(SHELL_SPEED * cos(rad))
  --   yAdd = round(SHELL_SPEED * sin(rad))
  -- and the engine spawns at (tank + SHELL_START_ADD * (xAdd, yAdd)) where
  -- SHELL_START_ADD = 5 (override in shells.c). Per-tick advance uses the
  -- same (xAdd, yAdd). Doing it this way gives us a bit-exact match with
  -- the engine's cs->sim shell positions.
  local SHELL_START_ADD = 5
  local a = dir - 64
  if a < 0 then a = a + 256 end
  local rad = a * (2 * math.pi / 256)
  -- roundDouble: round-half-away-from-zero (matches util.c:629).
  local function rd(x)
    if x >= 0 then return math.floor(x + 0.5) end
    return -math.floor(-x + 0.5)
  end
  local rad_step_x = rd(C.SHELL_SPEED * math.cos(rad))
  local rad_step_y = rd(C.SHELL_SPEED * math.sin(rad))
  local fx = info.tankx + SHELL_START_ADD * rad_step_x
  local fy = info.tanky + SHELL_START_ADD * rad_step_y
  -- Use the same gun_range (7.0) as the always-on yellow crosshair so the
  -- recorded destination matches what the user sees on screen.
  local cross_fx, cross_fy = U.crosshair_at(
    info.tankx, info.tanky, dir, 7.0)
  -- crosshair_at returns float TILE coords; convert to world wu.
  cross_fx = cross_fx * 256
  cross_fy = cross_fy * 256
  return {
    fire_tick = tick,
    fire_fx   = fx,
    fire_fy   = fy,
    dir       = dir,
    step_x    = rad_step_x,
    step_y    = rad_step_y,
    cross_fx  = cross_fx,
    cross_fy  = cross_fy,
    cur_fx    = fx,
    cur_fy    = fy,
    status    = "in_flight",
  }
end

-- Advance an in-flight shot by one step.
local function advance(shot)
  shot.cur_fx = shot.cur_fx + shot.step_x
  shot.cur_fy = shot.cur_fy + shot.step_y
end

-- Verify an in-flight shot against info.objects. Returns true if a
-- friendly OBJECT_SHOT is found near our simulated position. Also
-- records the closest-shell distance + position on the shot record so
-- callers can inspect how off the prediction is.
local function verify_alive(shot, info)
  local tol = M.MATCH_TOLERANCE_WU
  local tol2 = tol * tol
  local best2 = math.huge
  local best_x, best_y, best_info, best_type
  local seen_any_shot = false
  for _, ob in ipairs(info.objects or {}) do
    if ob.type == OBJECT_SHOT then
      seen_any_shot = true
      local hostile = bit.band((ob.info or 0), OBJECT_HOSTILE)
      if hostile == 0 then
        local dx = ob.x - shot.cur_fx
        local dy = ob.y - shot.cur_fy
        local d2 = dx * dx + dy * dy
        if d2 < best2 then
          best2     = d2
          best_x    = ob.x
          best_y    = ob.y
          best_info = ob.info
          best_type = ob.type
        end
      end
    end
  end
  shot.closest_d   = (best2 < math.huge) and math.sqrt(best2) or nil
  shot.closest_fx  = best_x
  shot.closest_fy  = best_y
  shot.closest_info = best_info
  shot._saw_any_shot = seen_any_shot
  return best2 <= tol2
end

function M.update(info, tick)
  local cur_shells = info.shells or 0
  local prev = M.prev_shells

  -- 1. Detect new fires (shells decreased since last tick).
  -- info.shells is per-tank, so any decrement is OUR shot.
  if prev ~= nil and cur_shells < prev then
    local fired = prev - cur_shells
    for _ = 1, fired do
      local shot = make_shot(info, tick)
      M.shots[#M.shots + 1] = shot
    end
  end
  M.prev_shells = cur_shells

  -- 2. Advance all in-flight shots one step, then 3. verify each.
  --    A shot that has just been fired this tick gets one initial step
  --    so its cur_fx/fy moves off the tank center before the verify.
  for _, shot in ipairs(M.shots) do
    if shot.status == "in_flight" then
      advance(shot)
      shot.age = (shot.age or 0) + 1
      shot.matched = false
      -- Don't verify a shot fired this tick — the engine's OBJECT_SHOT
      -- may not appear in info.objects until the next snapshot.
      if shot.fire_tick < tick then
        if verify_alive(shot, info) then
          shot.matched = true
          shot.was_matched = true
        elseif shot.was_matched then
          -- Green stopped matching — shot impacted or left perception. Kill it.
          shot.status   = "dead"
          shot.dead_tick = tick
          shot.dead_fx  = shot.cur_fx
          shot.dead_fy  = shot.cur_fy
        elseif shot.age >= M.INITIAL_MATCH_TICKS then
          -- Never got a single green match — prediction diverged or snapshot
          -- missed the spawn. Clear the blue bubble rather than letting it
          -- drift for the full lifetime.
          shot.status   = "dead"
          shot.dead_tick = tick
          shot.dead_fx  = shot.cur_fx
          shot.dead_fy  = shot.cur_fy
        end
      end
      -- Hard lifetime cap as final backstop.
      if shot.status == "in_flight" and shot.age >= M.MAX_LIFE_TICKS then
        shot.status   = "dead"
        shot.dead_tick = tick
        shot.dead_fx  = shot.cur_fx
        shot.dead_fy  = shot.cur_fy
      end
    end
  end

  -- 4. Trim old entries from the front of the queue.
  while #M.shots > M.MAX_SHOTS do
    table.remove(M.shots, 1)
  end
end

-- Lookup helpers --------------------------------------------------------

-- Returns the most recent N shots (newest last). Includes both in-flight
-- and dead.
function M.recent(n)
  n = n or M.MAX_SHOTS
  local out = {}
  local start = math.max(1, #M.shots - n + 1)
  for i = start, #M.shots do out[#out + 1] = M.shots[i] end
  return out
end

-- Count the in-flight shots whose simulated death is expected within the
-- next `lookahead` ticks (i.e. shots that should impact soon).
function M.in_flight_count()
  local n = 0
  for _, s in ipairs(M.shots) do
    if s.status == "in_flight" then n = n + 1 end
  end
  return n
end

-- Look up the most recent shot fired (any status). Useful for "did my
-- last shot hit?" style queries.
function M.last_shot()
  return M.shots[#M.shots]
end

-- Draw a cyan circle at every in-flight shot's simulated position. Dead
-- shots get a faint magenta marker at their last predicted location for
-- a few ticks so you can see where the verify failed. Coordinates are
-- in tile units so divide wu by 256.
function M.draw_overlay(tick)
  if not BRAIN_DEBUG_MODE then return end
  for _, s in ipairs(M.shots) do
    if s.status == "in_flight" then
      local fx = s.cur_fx / 256.0
      local fy = s.cur_fy / 256.0
      local sfx = s.fire_fx / 256.0
      local sfy = s.fire_fy / 256.0
      local dx = s.cross_fx / 256.0
      local dy = s.cross_fy / 256.0
      -- Local-sim trajectory (cyan, fire -> current).
      viz.line("shot_tracker_viz", sfx, sfy, fx, fy, 0, 200, 220, 100)
      -- Recorded destination (yellow X), matches the always-on crosshair.
      viz.line("shot_tracker_viz", dx - 0.25, dy, dx + 0.25, dy, 200, 200, 0, 180)
      viz.line("shot_tracker_viz", dx, dy - 0.25, dx, dy + 0.25, 200, 200, 0, 180)
      if s.matched then
        -- Verified against a real OBJECT_SHOT this tick — bright green.
        viz.circle("shot_tracker_viz", fx, fy, 0.30, 0, 255, 0, 255)
        viz.circle("shot_tracker_viz", fx, fy, 0.18, 80, 255, 80, 220)
      else
        -- Predicted but not yet verified (e.g. fire tick) — cyan.
        viz.circle("shot_tracker_viz", fx, fy, 0.30, 0, 255, 255, 255)
        viz.circle("shot_tracker_viz", fx, fy, 0.18, 0, 200, 220, 200)
      end
    elseif tick and s.dead_tick and (tick - s.dead_tick) < 30 then
      viz.circle("shot_tracker_viz", s.dead_fx / 256.0, s.dead_fy / 256.0,
                 0.22, 255, 0, 200, 180)
    end
  end
  -- ── Color legend ──────────────────────────────────────────────────
  -- One viz.hud_text row per marker so the user doesn't have to dig
  -- through code to know what each color means.  Anchored top-right
  -- so it doesn't fight with the kill_lgm / replan HUD on the left.
  if viz.hud_text then
    local x  = -10           -- right-anchored (negative = from right edge)
    local y0 = 220           -- below the existing top-right HUD stack
    local dy = 14
    viz.hud_text("shot_tracker_viz", x, y0,             "shot_tracker:",            "topright", 220, 220, 220, 230)
    viz.hud_text("shot_tracker_viz", x, y0 + dy * 1,    "● matched (engine-OK)",    "topright",   0, 255,   0, 230)
    viz.hud_text("shot_tracker_viz", x, y0 + dy * 2,    "● predicted (unverified)", "topright",   0, 255, 255, 230)
    viz.hud_text("shot_tracker_viz", x, y0 + dy * 3,    "● dead (sim diverged)",    "topright", 255,   0, 200, 230)
    viz.hud_text("shot_tracker_viz", x, y0 + dy * 4,    "— trajectory line",        "topright",   0, 200, 220, 230)
    viz.hud_text("shot_tracker_viz", x, y0 + dy * 5,    "✕ recorded crosshair",     "topright", 200, 200,   0, 230)
  end
end

return M
