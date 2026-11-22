-- =========================================================================
-- test_approach_skip.lua -- standalone unit tests (2026-11-20) for
-- approach_skip.lua (C.APPROACH_SKIP_RUNUP_ON_LANE, Andrew: "yes fix it"):
--   * the knob is on by default and off in PRESETS.keel and PRESETS.mast
--   * lane_pos: along / side against the pill-standoff line
--   * classify: behind, past, off_lane, far_behind, too_far_past
--   * check: knob off does nothing; off the lane does nothing; moving on
--     the lane holds the run-up point on the tank; stopped behind moves only
--     the run-up point; stopped past with a clear shot moves the standoff
--     too; stopped past with no clear shot puts the old run-up point back;
--     once per approach entry
-- No engine: fast_setup.world_env / final_angle are stubbed.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_approach_skip.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local C  = require("constants")
local FS = require("fast_setup")
local AS = require("approach_skip")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end
local function near(a, b, tol) return math.abs(a - b) <= (tol or 1e-6) end

print("knob")
check("on by default", C.APPROACH_SKIP_RUNUP_ON_LANE == true)
check("keel off", C.PRESETS.keel.APPROACH_SKIP_RUNUP_ON_LANE == false)
check("mast off", C.PRESETS.mast.APPROACH_SKIP_RUNUP_ON_LANE == false)
check("lane 128", C.APPROACH_SKIP_RUNUP_LANE_WU == 128)
check("max past 256", C.APPROACH_SKIP_RUNUP_MAX_PAST_WU == 256)
check("stop speed 4", C.APPROACH_SKIP_RUNUP_STOP_SPEED == 4)

-- Pill (10,10): centre (2688,2688). Standoff 5 tiles south: (2688,3968).
-- The lane runs south from the pill; "behind" = further south.
local PCX, PCY, SX, SY = 2688, 2688, 2688, 3968
print("lane_pos")
do
  local a, s = AS.lane_pos(SX, SY + 100, SX, SY, PCX, PCY)
  check("100 behind", near(a, 100) and near(s, 0), a)
  a, s = AS.lane_pos(SX + 50, SY - 200, SX, SY, PCX, PCY)
  check("200 past, 50 side", near(a, -200) and near(s, 50), a .. " " .. s)
  check("standoff on pill: nil", AS.lane_pos(1, 1, PCX, PCY, PCX, PCY) == nil)
end

print("classify")
check("on standoff: behind", AS.classify(0, 0) == "behind")
check("128 behind: behind", AS.classify(128, 0) == "behind")
check("129 behind: far_behind", select(2, AS.classify(129, 0)) == "far_behind")
check("100 past: past", AS.classify(-100, 100) == "past")
check("256 past: past", AS.classify(-256, 0) == "past")
check("257 past: too_far_past", select(2, AS.classify(-257, 0)) == "too_far_past")
check("side 129: off_lane", select(2, AS.classify(0, 129)) == "off_lane")
check("nil: no_lane", select(2, AS.classify(nil, nil)) == "no_lane")

-- Stubs: final_angle returns SHOT (nil = no clear shot).
local SHOT = 0
FS.world_env = function() return {} end
FS.final_angle = function() return SHOT end

local function mkgoal()
  return { mx = 10, my = 10, standoff_mx = 10, standoff_my = 15,
           standoff_fx = SX / 256, standoff_fy = SY / 256,
           approach_fx = 10.5, approach_fy = 17.75, approach_mx = 10, approach_my = 17,
           _approach_start = 100 }
end

print("check")
do
  local g = mkgoal()
  C.APPROACH_SKIP_RUNUP_ON_LANE = false
  check("knob off: no change", AS.check(g, { tankx = SX, tanky = SY, speed = 0 }, {}, 110) == false
        and g.approach_fy == 17.75)
  C.APPROACH_SKIP_RUNUP_ON_LANE = true

  g = mkgoal()
  check("off lane: no change", AS.check(g, { tankx = SX + 300, tanky = SY, speed = 0 }, {}, 110) == false
        and g.approach_fy == 17.75 and not g._fs_off)

  g = mkgoal()
  check("far behind: no change", AS.check(g, { tankx = SX, tanky = SY + 400, speed = 0 }, {}, 110) == false
        and g.approach_fy == 17.75)

  -- moving on the lane: held on the tank, no decision yet
  g = mkgoal()
  local ok = AS.check(g, { tankx = SX, tanky = SY - 100, speed = 20 }, {}, 110)
  check("moving: held on tank", ok and near(g.approach_fy, (SY - 100) / 256) and g._runup_saved
        and not g._fs_off and not g._runup_skip_at)
  -- leaves the lane while moving: old run-up point back
  AS.check(g, { tankx = SX + 400, tanky = SY - 100, speed = 20 }, {}, 111)
  check("left lane: restored", g.approach_fy == 17.75 and g.approach_my == 17 and not g._runup_saved)

  -- stopped behind: run-up point on tank, standoff kept
  g = mkgoal()
  ok = AS.check(g, { tankx = SX + 20, tanky = SY + 60, speed = 0 }, {}, 110)
  check("behind: run-up moved", ok and near(g.approach_fy, (SY + 60) / 256) and g._runup_skip == "behind")
  check("behind: standoff kept", near(g.standoff_fy, SY / 256) and g.standoff_my == 15)
  check("behind: fast setup off", g._fs_off == true and g._fs_appr == nil)
  check("once per entry", AS.check(g, { tankx = SX, tanky = SY + 100, speed = 0 }, {}, 111) == false
        and near(g.approach_fy, (SY + 60) / 256))
  g._approach_start = 200
  check("new entry: checks again", AS.check(g, { tankx = SX, tanky = SY + 100, speed = 0 }, {}, 201) == true
        and near(g.approach_fy, (SY + 100) / 256))

  -- stopped past, clear shot: standoff and run-up both on tank
  g = mkgoal(); SHOT = 0
  ok = AS.check(g, { tankx = SX + 110, tanky = SY - 161, speed = 2 }, {}, 110)
  check("past clear: fires from here", ok and g._runup_skip == "past"
        and near(g.standoff_fy, (SY - 161) / 256) and g.standoff_my == 14
        and near(g.approach_fx, (SX + 110) / 256))

  -- moving past, then stopped past with no clear shot: old point back
  g = mkgoal(); SHOT = nil
  AS.check(g, { tankx = SX, tanky = SY - 150, speed = 16 }, {}, 110)
  ok = AS.check(g, { tankx = SX, tanky = SY - 160, speed = 0 }, {}, 112)
  check("past no shot: restored", ok == false and g.approach_fy == 17.75
        and near(g.standoff_fy, SY / 256) and not g._fs_off and not g._runup_skip)
  check("past no shot: once per entry", AS.check(g, { tankx = SX, tanky = SY - 160, speed = 0 }, {}, 113) == false)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
