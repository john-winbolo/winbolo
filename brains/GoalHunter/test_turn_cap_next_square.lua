-- =========================================================================
-- test_turn_cap_next_square.lua — standalone unit tests for the turn cap from
-- the next route square (knob C.STEER_TURN_CAP_NEXT_SQUARE):
--   * steering.turn_rate_brads — the tank's full turn rate per think.
--   * steering.next_square_reach — 2 R sin(err) (sin = 1 at 90 degrees or
--     more): the farthest a square can be and still be inside the turning
--     circle.
--   * steering.turn_cap_on — the decision steer_core takes: does the
--     heading-error turn cap apply this think?
-- The recorded cases are from a game where player 2 drove circles in a forest
-- with its goal 6-12 tiles away (brain ticks 1524-2266): speed, heading error
-- and distance to the next route square as logged, turn rate 2 brads per
-- think as recorded. No engine: terrain is stubbed.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_turn_cap_next_square.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")
local U = require("util")
local S = require("steering")

local TT = C.T_GRASS
U.ttype_peek = function() return TT end

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
local function near(a, b, eps) return math.abs(a - b) <= (eps or 0.5) end

local DEFAULT_KNOB = C.STEER_TURN_CAP_NEXT_SQUARE
local RAMP = 10   -- steer_core's ramp_start for a non-plow goal

-- One think: speed, heading error (brads), goal distance (wu), square
-- distance (wu). Returns turn_cap_on's answer, as steer_core asks it.
local function cap_on(speed, err, goal_wu, sq_wu, info_extra)
  local info = { speed = speed, inboat = false }
  if info_extra then for k, v in pairs(info_extra) do info[k] = v end end
  return S.turn_cap_on(info, 106, 136, err, RAMP, goal_wu / 256.0, sq_wu)
end

print("turn rate")
do
  TT = C.T_ROAD
  check("classic road: 2 brads per think",
        S.turn_rate_brads({}, C.T_ROAD) == 2, S.turn_rate_brads({}, C.T_ROAD))
  check("classic forest: 1 brad per think",
        S.turn_rate_brads({}, C.T_FOREST) == 1, S.turn_rate_brads({}, C.T_FOREST))
  check("classic swamp: 0.5 brad per think",
        S.turn_rate_brads({}, C.T_SWAMP) == 0.5, S.turn_rate_brads({}, C.T_SWAMP))
  local live = C.LIVE_PHYSICS == true and C.LIVE_PHYSICS_TURN == true
  check("live turn rules are on by default", live, live)
  local r = S.turn_rate_brads({ rules = { turn_forest = 1 } }, C.T_FOREST)
  check("live forest rule 1: 2 brads per think", r == 2, r)
  r = S.turn_rate_brads({ mods = { turn = 150 } }, C.T_ROAD)
  check("turn modifier 150 on road: 3 brads per think", r == 3, r)
  r = S.turn_rate_brads({ inboat = true }, C.T_RIVER)
  check("afloat on river: boat rule, 2 brads per think", r == 2, r)
  r = S.turn_rate_brads({ inboat = false }, C.T_RIVER)
  check("on river without a boat: river rule, 0.5", r == 0.5, r)
  r = S.turn_rate_brads({ rules = { turn_forest = 0 } }, C.T_FOREST)
  check("live rule 0 is not used (classic forest)", r == 1, r)
  local saved = C.LIVE_PHYSICS_TURN
  C.LIVE_PHYSICS_TURN = false
  r = S.turn_rate_brads({ rules = { turn_forest = 1 }, mods = { turn = 150 } }, C.T_FOREST)
  check("live TURN group off: classic forest, no modifier", r == 1, r)
  C.LIVE_PHYSICS_TURN = saved
end

print("reach = 2 R sin(err)")
do
  local reach, R = S.next_square_reach(48, 64, 2)
  check("speed 48, rate 2: R = 244 wu", near(R, 244.46), R)
  check("err 64 (90 deg): reach = 2R = 489", near(reach, 488.92), reach)
  reach = S.next_square_reach(48, 110, 2)
  check("err 110 (behind): reach = 2R (sin taken as 1)", near(reach, 488.92), reach)
  reach = S.next_square_reach(48, 32, 2)
  check("err 32 (45 deg): reach = 2R x 0.707 = 346", near(reach, 345.72), reach)
  reach, R = S.next_square_reach(64, 64, 2)
  check("speed 64: R = 326 wu", near(R, 325.95), R)
  reach, R = S.next_square_reach(24, 64, 1)
  check("forest classic, speed 24, rate 1: R = 244 wu", near(R, 244.46), R)
  reach = S.next_square_reach(0, 64, 2)
  check("standing still: reach 0", reach == 0, reach)
  reach = S.next_square_reach(48, 64, 0)
  check("turn rate 0: reach 0 (no cap from this rule)", reach == 0, reach)
end

print("recorded circling ticks: cap applies (goal 6-12 tiles away)")
TT = C.T_GRASS
C.STEER_TURN_CAP_NEXT_SQUARE = true
do
  check("t=1652 spd 48 err 110 sq 346 goal 2664", cap_on(48, 110, 2664, 346) == true)
  check("t=1700 spd 48 err 82 sq 467 goal 2300", cap_on(48, 82, 2300, 467) == true)
  check("t=1766 spd 48 err 24 sq 240 goal 2950", cap_on(48, 24, 2950, 240) == true)
  check("t=2024 spd 48 err 40 sq 215 goal 2865", cap_on(48, 40, 2865, 215) == true)
  check("t=2150 spd 48 err 86 sq 265 goal 1701", cap_on(48, 86, 1701, 265) == true)
end

print("driving far from the goal: no cap")
do
  check("t=1622 spd 48 err 4 sq 356 (lined up)", cap_on(48, 4, 2864, 356) == false)
  check("t=448 spd 64 err 2 sq 714 (lined up)", cap_on(64, 2, 2848, 714) == false)
  check("t=1808 spd 48 err 22 sq 395 (outside circle 248)", cap_on(48, 22, 3093, 395) == false)
  check("t=1586 spd 48 err 30 sq 391 (outside circle 327)", cap_on(48, 30, 3029, 391) == false)
  check("t=301 spd 64 err 25 sq 828 (outside circle 378)", cap_on(64, 25, 1418, 828) == false)
  check("err at ramp_start: no cap even inside the circle", cap_on(48, 10, 3000, 20) == false)
  check("square distance unknown: no cap", cap_on(48, 110, 3000, nil) == false)
  check("slowed to 24 at err 65, sq 466: outside circle 245", cap_on(24, 65, 2194, 466) == false)
end

print("near the goal: old rule unchanged")
do
  check("goal 3 tiles, err 20, square far: cap", cap_on(48, 20, 768, 2000) == true)
  check("goal 3 tiles, err 10: no cap", cap_on(48, 10, 768, 100) == false)
  C.STEER_TURN_CAP_NEXT_SQUARE = false
  check("knob off, goal 3 tiles, err 20: cap", cap_on(48, 20, 768, 2000) == true)
end

print("knob off: old behaviour")
do
  C.STEER_TURN_CAP_NEXT_SQUARE = false
  check("t=1652 case: no cap past 4 tiles", cap_on(48, 110, 2664, 346) == false)
  check("t=1766 case: no cap past 4 tiles", cap_on(48, 24, 2950, 240) == false)
  local called = false
  local saved = S.turn_cap_next_square
  S.turn_cap_next_square = function() called = true; return true end
  cap_on(48, 110, 2664, 346)
  check("knob off: circle test not run", called == false, called)
  S.turn_cap_next_square = saved
end
C.STEER_TURN_CAP_NEXT_SQUARE = DEFAULT_KNOB

check("default knob is true", C.STEER_TURN_CAP_NEXT_SQUARE == true, C.STEER_TURN_CAP_NEXT_SQUARE)
check("PRESETS.keel pins the knob to false",
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.STEER_TURN_CAP_NEXT_SQUARE == false,
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.STEER_TURN_CAP_NEXT_SQUARE)

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
