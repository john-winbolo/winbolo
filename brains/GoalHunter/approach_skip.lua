-- =========================================================================
-- approach_skip.lua -- skip the attack_pill run-up point when the tank is
-- already on the firing lane at the standoff (C.APPROACH_SKIP_RUNUP_ON_LANE,
-- Andrew, Nov 20: "yes fix it").
--
-- The bug: the approach step drives to the run-up point, ATTACK_APPROACH_
-- OFFSET tiles behind the standoff on the line away from the pill. When the
-- tank is already at the standoff or a little past it (for example after a
-- rescue_lgm trip took it forward), that point is behind the tank. A tank
-- cannot back up, so it drove a full circle to get there and then drove the
-- same lane in again (grass demo try3, Nov 20: about 3 s and 375 degrees of
-- turning).
--
-- The fix, once per approach entry, when the tank is on the lane:
--   * side offset from the standoff-pill line <= APPROACH_SKIP_RUNUP_LANE_WU
--     (128 wu, the approach step's own spot tolerance);
--   * at most APPROACH_SKIP_RUNUP_LANE_WU behind the standoff, and at most
--     APPROACH_SKIP_RUNUP_MAX_PAST_WU past it toward the pill.
-- Behind or on the standoff: the run-up point moves to where the tank is.
-- The take then goes on as usual and creeps forward to the standoff.
-- Past the standoff: only when the shot test from here passes (fast_setup's
-- shot_ok: the shell sim reaches the pill and nothing is in the way, our
-- blocker pill included). Then the standoff and the run-up point both move
-- to where the tank is, so the take fires from here. When the test fails,
-- nothing changes (the old way).
-- While the tank still moves on the lane, the run-up point is held on the
-- tank so it brakes there; the choice above is made once it has stopped
-- (speed <= APPROACH_SKIP_RUNUP_STOP_SPEED, the approach's own SPEED_TOL).
-- Either way fast setup is turned off for this goal, because its plan
-- drives back to its own setup point behind the standoff.
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local print2 = require("print2")

local M = {}

local sqrt, abs, floor = math.sqrt, math.abs, math.floor

-- Where tank (tx, ty) is against the lane from pill centre (pcx, pcy) out
-- through standoff (sx, sy); all world units. along > 0: behind the
-- standoff (away from the pill); along < 0: past it toward the pill.
-- side: distance from the line. nil when the standoff is on the pill.
function M.lane_pos(tx, ty, sx, sy, pcx, pcy)
  local dx, dy = sx - pcx, sy - pcy
  local d = sqrt(dx * dx + dy * dy)
  if d < 1 then return nil end
  local ux, uy = dx / d, dy / d
  local rx, ry = tx - sx, ty - sy
  return rx * ux + ry * uy, abs(rx * uy - ry * ux)
end

-- "behind" (on the standoff or behind it, inside the lane), "past" (past
-- it, inside the lane), or nil and why not.
function M.classify(along, side)
  if not along then return nil, "no_lane" end
  local lane = C.APPROACH_SKIP_RUNUP_LANE_WU or 128
  local past = C.APPROACH_SKIP_RUNUP_MAX_PAST_WU or 256
  if side > lane then return nil, "off_lane" end
  if along > lane then return nil, "far_behind" end
  if along < -past then return nil, "too_far_past" end
  if along >= 0 then return "behind" end
  return "past"
end

-- Called every approach tick from attack.lua, after the first-tick init.
-- obs_fn: attack.lua's shot_path_obstacle_count. Returns true when it moved
-- the run-up point (and maybe the standoff).
function M.check(goal, info, world, now, obs_fn)
  if not C.APPROACH_SKIP_RUNUP_ON_LANE then return false end
  if not (goal.standoff_mx and goal.mx and info.tankx) then return false end
  -- Once per approach entry: _approach_start is new on each entry.
  if goal._runup_skip_at and goal._runup_skip_at == goal._approach_start then return false end
  local sfx = goal.standoff_fx or (goal.standoff_mx + 0.5)
  local sfy = goal.standoff_fy or (goal.standoff_my + 0.5)
  local swx, swy = floor(sfx * 256 + 0.5), floor(sfy * 256 + 0.5)
  local pcx, pcy = goal.mx * 256 + 128, goal.my * 256 + 128
  local tx, ty = info.tankx, info.tanky
  local along, side = M.lane_pos(tx, ty, swx, swy, pcx, pcy)
  local kind = M.classify(along, side)
  local function restore()
    local sv = goal._runup_saved
    if sv then
      goal.approach_fx, goal.approach_fy = sv.fx, sv.fy
      goal.approach_mx, goal.approach_my = sv.mx, sv.my
    end
    goal._runup_saved = nil
  end
  if not kind then
    if goal._runup_saved then restore() end
    return false
  end
  -- Still moving: hold the run-up point on the tank so the approach brakes
  -- here, and decide once it has stopped (where it stops is where it fires
  -- from or creeps forward from).
  if (info.speed or 0) > (C.APPROACH_SKIP_RUNUP_STOP_SPEED or 4) then
    if not goal._runup_saved then
      goal._runup_saved = { fx = goal.approach_fx, fy = goal.approach_fy,
                            mx = goal.approach_mx, my = goal.approach_my }
    end
    goal.approach_fx, goal.approach_fy = tx / 256, ty / 256
    goal.approach_mx = U.mclamp(floor(goal.approach_fx))
    goal.approach_my = U.mclamp(floor(goal.approach_fy))
    return true
  end
  goal._runup_skip_at = goal._approach_start
  if kind == "past" then
    local FS = require("fast_setup")
    local env = FS.world_env(info, goal, world, obs_fn, C.GUNSIGHT_MAX)
    local axw = goal.aim_wx or floor((goal.aim_mx or (goal.mx + 0.5)) * 256 + 0.5)
    local ayw = goal.aim_wy or floor((goal.aim_my or (goal.my + 0.5)) * 256 + 0.5)
    local a0 = U.aim_at_f(tx / 256, ty / 256, axw / 256, ayw / 256) % 256
    local ac = U.aim_at_f(tx / 256, ty / 256, pcx / 256, pcy / 256) % 256
    local ang = FS.final_angle(env, tx, ty, a0, ac)
    if not ang then
      print2(string.format("APPROACH_SKIP_RUNUP t=%d pill=(%d,%d) tank=(%d,%d) past=%.0f side=%.0f -> no, no clear shot from here",
        now, goal.mx, goal.my, tx, ty, -along, side))
      restore()
      return false
    end
    goal.standoff_fx, goal.standoff_fy = tx / 256, ty / 256
    goal.standoff_mx = U.mclamp(floor(goal.standoff_fx))
    goal.standoff_my = U.mclamp(floor(goal.standoff_fy))
  end
  goal.approach_fx, goal.approach_fy = tx / 256, ty / 256
  goal.approach_mx = U.mclamp(floor(goal.approach_fx))
  goal.approach_my = U.mclamp(floor(goal.approach_fy))
  goal._fs_off = true
  goal._fs_appr = nil
  goal._runup_saved = nil
  goal._runup_skip = kind
  print2(string.format("APPROACH_SKIP_RUNUP t=%d pill=(%d,%d) tank=(%d,%d) along=%.0f side=%.0f -> %s",
    now, goal.mx, goal.my, tx, ty, along, side,
    kind == "past" and "fire from here (standoff moved here)" or "run-up point moved here"))
  return true
end

return M
