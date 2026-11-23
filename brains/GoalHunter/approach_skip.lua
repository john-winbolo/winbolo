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
-- blocker pill included). With C.APPROACH_SKIP_RUNUP_SHIELD_CHECK (Nov 22,
-- Andrew) the take's own cover test must pass here too (M.shield_check:
-- pilltake_v2_old.judge at the tank's place with the take's plan). Then the
-- standoff and the run-up point both move to where the tank is, so the
-- take fires from here. When a test fails, nothing changes (the old way).
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
local clock_us = rawget(_G, "clock_us") or function() return floor(os.clock() * 1e6) end

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

-- C.APPROACH_SKIP_RUNUP_SHIELD_CHECK (Nov 22, Andrew): is the tank covered
-- at (tx, ty), its place past the standoff? Andrew: "can the tank hit the
-- pill in a corner or its center aim WHILE still not hitting any of the
-- blockers AND the pill hitting those blockers as it shoots at the tank".
-- This is the take's own test, pilltake_v2_old.judge, run at the tank's place:
--   * inp: pt2_attack.build_inp (knob_inp + world_inp, the same inp the
--     take's plan was made with), so only what this bot sees;
--   * extra: the take's planned blockers (pt2_attack.v1_view of the take's
--     shield scan: a planned pill drop 15 shots, a planned wall one wall),
--     as pt2_attack's PT2_CMP line judges v1's point;
--   * judge goes through M.AIMS in order (centre, then the corners): the
--     first aim whose shot reaches the pill without a blocker in the way
--     is used. Then each hostile pill in range shoots at the tank's place
--     (the shadow ray from its centre), and its ray must meet its cover bar
--     (A's 15, a chain pill's 10, a bystander one wall), as at the standoff.
--     A pill's ray does not depend on our aim, so the first clear aim
--     gives the same answer as each aim in turn.
-- A take with no shield plan (no goal._shield_scan.best) has no cover to
-- keep: "-" (the old way).
-- Returns why ("-" = covered, else judge's first failing test), the aim
-- index used, the cost in us, and "plan" or "no_plan".
function M.shield_check(state, goal, info, world, now, tx, ty)
  local t0 = clock_us()
  local sscan = goal._shield_scan
  if not (sscan and sscan.best) then return "-", nil, clock_us() - t0, "no_plan" end
  local PA = require("pt2_attack")
  local V2 = require("pilltake_v2_old")
  local pill = (goal.target_id and world.pills and world.pills[goal.target_id])
               or { mx = goal.mx, my = goal.my }
  local npb = sscan.num_pill_blockers or 0
  local inp = PA.build_inp(state or {}, goal, info, world, pill,
                           info.man_status == C.LGM_DEAD, npb, nil, now)
  if not inp then return "NO_TARGET", nil, clock_us() - t0, "plan" end
  local v = PA.v1_view(world, sscan, npb)
  local why, rec = V2.judge(inp, floor(tx), floor(ty), v and v.extra or nil, nil)
  return why or "-", rec and rec.aim_idx, clock_us() - t0, "plan"
end

-- Called every approach tick from attack.lua, after the first-tick init.
-- obs_fn: attack.lua's shot_path_obstacle_count. state: the brain state
-- (for the shield check). Returns true when it moved the run-up point (and
-- maybe the standoff).
function M.check(goal, info, world, now, obs_fn, state)
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
    goal._runup_shield = nil
    if C.APPROACH_SKIP_RUNUP_SHIELD_CHECK then
      local why, aim, us, plan = M.shield_check(state, goal, info, world, now, tx, ty)
      goal._runup_shield = string.format("shield=%s aim=%s plan=%s cost_us=%d",
                                         why, tostring(aim), plan, us)
      if why ~= "-" then
        print2(string.format("APPROACH_SKIP_RUNUP t=%d pill=(%d,%d) tank=(%d,%d) past=%.0f side=%.0f -> no, not covered here: %s",
          now, goal.mx, goal.my, tx, ty, -along, side, goal._runup_shield))
        restore()
        return false
      end
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
  print2(string.format("APPROACH_SKIP_RUNUP t=%d pill=(%d,%d) tank=(%d,%d) along=%.0f side=%.0f -> %s%s",
    now, goal.mx, goal.my, tx, ty, along, side,
    kind == "past" and "fire from here (standoff moved here)" or "run-up point moved here",
    (kind == "past" and goal._runup_shield) and (" " .. goal._runup_shield) or ""))
  return true
end

return M
