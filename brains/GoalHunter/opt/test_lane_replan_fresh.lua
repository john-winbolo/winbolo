-- =========================================================================
-- test_lane_replan_fresh.lua — standalone unit tests (2026-11-03) for the
-- replan after the shield scan lane check fails (C.BLOCKER_MIN_LANE_BRAD).
--
-- The recorded game (20261103_192602 part 2, bot2 t=5119..5127): the shield
-- scan winner (deg 325.25) was copied onto the goal, its lane was too narrow,
-- blocked_line_replan banned 320..330 and went back to plan_position. The
-- reset kept the old standoff_* / _chosen_deg / aim_*, and plan_position's
-- fresh-entry cleanup did not run again (goal._plan_position_cleared was
-- still set). The ban drops the sweep cache, so the new sweep took some
-- ticks; the 3-tick move to approach saw the old standoff and drove to the
-- banned spot with no shield scan, so no walls.
--
-- Checked here:
--   * blocked_line_replan on the lane-fail goal: angle ban, replan count and
--     the angle sweep cache are as before, and the cleanup flag is dropped.
--   * update_attack_substate on the next ticks, with the new sweep still in
--     progress: the goal stays in plan_position, never reaches approach, and
--     the old standoff / _chosen_deg / approach / aim are cleared.
--   * reset_to_plan_position from another substate: the same result.
-- No engine: the angle sweep is a stand-in that answers "in_progress".
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_lane_replan_fresh.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

_G.print2 = _G.print2 or function() end

local C = require("constants")
local U = require("util")
local A = require("attack")

U.ttype      = function() return 4 end       -- road everywhere
U.ttype_peek = function() return 4 end

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end

-- The pill and the lane-fail winner from the recorded game.
local PID, PMX, PMY = 6, 126, 141
local NOW = 5123

-- The goal as plan_position leaves it just before the lane check: the shield
-- scan winner copied on, the cleanup flag set for this plan_position visit.
local function lane_fail_goal()
  return {
    kind = "attack_pill", target_id = PID, mx = PMX, my = PMY,
    substate = "plan_position",
    _plan_position_cleared = true,
    _is_ppt = true,
    scan_spots = { { mx = 122, my = 135, has_los = true } },
    _shield_scan = { best = { mx = 122, my = 135, cx = 122.51, cy = 135.75,
                              deg = 325.25, score = 1032, best_aim_idx = 3,
                              aims = { [3] = { blockers = {}, potential_blockers = { {}, {} } } } } },
    standoff_mx = 122, standoff_my = 135, standoff_fx = 122.51, standoff_fy = 135.75,
    approach_mx = 121, approach_my = 133, approach_fx = 121.2, approach_fy = 133.9,
    _chosen_deg = 325.25,
    aim_mx = 127, aim_my = 141, aim_wx = PMX * 256 + 200, aim_wy = PMY * 256 + 56,
    aim_idx = 3,
    through_walls = true,
  }
end

local SPOT_FIELDS = {
  "standoff_mx", "standoff_my", "standoff_fx", "standoff_fy",
  "approach_mx", "approach_my", "approach_fx", "approach_fy",
  "_chosen_deg", "aim_wx", "aim_wy", "aim_idx", "through_walls",
  "_shield_scan", "scan_spots",
}
local function spot_left(goal)
  for _, k in ipairs(SPOT_FIELDS) do
    if goal[k] ~= nil then return k .. "=" .. tostring(goal[k]) end
  end
  return nil
end

-- ── blocked_line_replan on the lane-fail goal ─────────────────────────────
print("blocked_line_replan (lane fail)")
local state = { tick = NOW, phase = "mid",
                _pill_eval_cache = { [PID] = { spots = {}, best_score = 1 } },
                _pill_eval_progress = { [PID] = { deg_cursor = 0 } } }
local goal = lane_fail_goal()
state.goal = goal
local abandon, bucket, tries = A._blocked_line_replan(state, goal, PMX, PMY, NOW)
check("not abandoned on the first replan", abandon == false, abandon)
check("bucket 325 banned", bucket == 325, bucket)
check("_sanity_pill_replans = 1", tries == 1 and goal._sanity_pill_replans == 1, tries)
local bans = state.banned_pill_angles and state.banned_pill_angles[PMY * 256 + PMX] or {}
check("angles 320, 325, 330 banned", bans[320] and bans[325] and bans[330], tostring(bans[325]))
check("sweep cache dropped (the new sweep runs again)",
      state._pill_eval_cache[PID] == nil and state._pill_eval_progress[PID] == nil)
check("substate plan_position", goal.substate == "plan_position", goal.substate)
check("cleanup flag dropped", goal._plan_position_cleared == nil, goal._plan_position_cleared)

-- ── The next ticks: the new sweep is still in progress ────────────────────
print("update_attack_substate while the new sweep runs")
local sweep_calls = 0
A.advance_pill_eval_chunk = function() sweep_calls = sweep_calls + 1; return "in_progress" end
local pill  = { mx = PMX, my = PMY, health = 8, owner = "enemy", anger = 1 }
local world = { pills = { [PID] = pill }, pill_at = {}, bases = {}, tanks = {} }
local info  = { tankx = 118 * 256, tanky = 130 * 256, armour = 40, trees = 0,
                man_status = C.LGM_INTANK, player_number = 2, shells = 40 }
local reached, err = nil, nil
for t = NOW + 1, NOW + 8 do
  state.tick = t
  local ok, e = pcall(A.update_attack_substate, goal, state, world, info)
  if goal.substate == "approach" then reached = t; break end
  if not ok then err = e; break end
end
check("no error in update_attack_substate", err == nil, err)
check("the angle sweep is asked again", sweep_calls > 0, sweep_calls)
check("never reaches approach in 8 ticks", reached == nil, reached)
check("still in plan_position", goal.substate == "plan_position", goal.substate)
check("old spot cleared (standoff, approach, deg, aim, scan)", spot_left(goal) == nil, spot_left(goal))
check("_sanity_pill_replans kept", goal._sanity_pill_replans == 1, goal._sanity_pill_replans)
check("angle ban kept", bans[325] ~= nil)

-- ── reset_to_plan_position from another substate ──────────────────────────
print("reset_to_plan_position from approach")
do
  local g = lane_fail_goal()
  g.substate = "approach"
  g._plan_position_cleared = nil
  A._reset_to_plan_position(state, g)
  check("substate plan_position", g.substate == "plan_position", g.substate)
  check("cleanup flag nil", g._plan_position_cleared == nil)
  state.goal = g
  local err = nil
  for t = NOW + 20, NOW + 27 do
    state.tick = t
    local ok, e = pcall(A.update_attack_substate, g, state, world, info)
    if not ok then err = e; break end
  end
  check("no error", err == nil, err)
  check("stays in plan_position", g.substate == "plan_position", g.substate)
  check("old spot cleared", spot_left(g) == nil, spot_left(g))
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
