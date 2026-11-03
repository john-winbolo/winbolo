-- =========================================================================
-- test_blocker_min_lane.lua — standalone unit tests (2026-11-03) for the
-- minimum clear lane around a planned aim (knob C.BLOCKER_MIN_LANE_BRAD):
--   * aim_lane.wide_enough — the width test on a plain clear function.
--   * spot_margin.angle_line_trees — the shell test at a gun angle.
--   * attack.aim_lane_wide — the lane from a spot to an aim point.
--   * attack._clear_aim_from_world — with min_lane 0 (knob off) a narrow
--     centre lane is taken as before; with 0.5 it is dropped for a corner
--     whose lane is wide.
-- No engine: terrain is all road and the shell simulator is a stand-in that
-- decides by gun angle.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_blocker_min_lane.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

-- Same two-argument math.atan shim init.lua installs on LuaJIT (U.aim_at_f
-- needs it).
if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local A      = require("attack")
local AL     = require("aim_lane")
local SM     = require("spot_margin")
local shield = require("attack_shield")

_G.print2 = _G.print2 or function() end
U.ttype      = function() return 4 end       -- road everywhere
U.ttype_peek = function() return 4 end
cpf.SHOT_TANK = cpf.SHOT_TANK or 0

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

-- ── Knobs ────────────────────────────────────────────────────────────────
print("knobs")
local keel = C.PRESETS and C.PRESETS.keel or {}
check("BLOCKER_MIN_LANE_BRAD default 0.5", C.BLOCKER_MIN_LANE_BRAD == 0.5, C.BLOCKER_MIN_LANE_BRAD)
check("PRESETS.keel BLOCKER_MIN_LANE_BRAD = 0", keel.BLOCKER_MIN_LANE_BRAD == 0)

-- ── aim_lane.wide_enough ─────────────────────────────────────────────────
print("aim_lane.wide_enough")
do
  local function band(lo, hi) return function(a) return a >= lo and a <= hi end end
  local ok, l, r = AL.wide_enough(band(99.0, 101.0), 100.0, 0.5)
  check("lane +/-1.0 around 100 passes 0.5", ok and l == 0.5 and r == 0.5, tostring(l) .. "/" .. tostring(r))
  ok, l, r = AL.wide_enough(band(99.6, 101.0), 100.0, 0.5)
  check("left side only 0.375 clear -> fails", not ok and l == 0.375 and r == 0.5,
        tostring(l) .. "/" .. tostring(r))
  ok, l, r = AL.wide_enough(band(99.95, 100.05), 100.0, 0.5)
  check("0.1-brad lane (the recorded game) -> fails", not ok and l == 0 and r == 0,
        tostring(l) .. "/" .. tostring(r))
  ok = AL.wide_enough(band(99.5, 100.5), 100.0, 0.5)
  check("lane exactly +/-0.5 -> passes", ok)
  ok, l, r = AL.wide_enough(band(0, 0), 100.0, 0.5)
  check("aim itself blocked -> fails, 0/0", not ok and l == 0 and r == 0)
  ok = AL.wide_enough(function(a) return a >= 255.6 or a <= 0.4 end, 0.0, 0.5)
  check("lane across the wrap, 0.4 each side -> fails", not ok)
  ok = AL.wide_enough(function(a) return a >= 255.4 or a <= 0.6 end, 0.0, 0.5)
  check("lane across the wrap, 0.6 each side -> passes", ok)
end

-- ── Shell stand-in by gun angle ──────────────────────────────────────────
-- Spot (100.5,127.5), pill (107,127), a live pill at (104,127). A shell is
-- clear when its angle is within 0.05 brad of the centre aim, or within 1.0
-- brad of the top-right corner aim; any other angle in that sector hits the
-- blocker pill first. Angles far away miss the pill.
local PMX, PMY, BMX, BMY = 107, 127, 104, 127
local OX, OY = 100 * 256 + 128, 127 * 256 + 128
local function aim_angle(i)
  local off = shield.AIM_OFFSETS_TILE_FIRE[i]
  local awx = PMX * 256 + math.floor(off[1] * 256)
  local awy = PMY * 256 + math.floor(off[2] * 256)
  return U.aim_at_f(OX / 256.0, OY / 256.0, awx / 256.0, awy / 256.0) % 256, awx, awy
end
local A_C  = aim_angle(1)
local A_TR = aim_angle(3)
local function d(a, b) return math.abs(AL.adiff(a, b)) end
cpf.simulate_shot_angle = function(_, _, a)
  if d(a, A_C) <= 0.05 or d(a, A_TR) <= 1.0 then
    return { { mx = 101, my = 127 }, { mx = 104, my = 126 }, { mx = PMX, my = PMY } }
  elseif d(a, A_C) <= 6 then
    return { { mx = 101, my = 127 }, { mx = BMX, my = BMY }, { mx = PMX, my = PMY } }
  end
  return { { mx = 101, my = 127 }, { mx = 104, my = 131 }, { mx = 107, my = 133 } }
end
cpf.simulate_shot = function(ox, oy, awx, awy)
  return cpf.simulate_shot_angle(ox, oy,
    U.aim_at_f(ox / 256.0, oy / 256.0, awx / 256.0, awy / 256.0) % 256)
end
local bp = { mx = BMX, my = BMY, health = 15, owner = "friendly" }
local world = { pills = { [4] = bp },
                pill_at = { [BMY * 256 + BMX] = { { id = 4, pill = bp } } } }

print("spot_margin.angle_line_trees")
check("centre angle reaches the pill clear",
      SM.angle_line_trees(OX, OY, 100, 127, PMX, PMY, world, A_C) == 0)
check("centre + 0.25 brad hits the blocker pill",
      SM.angle_line_trees(OX, OY, 100, 127, PMX, PMY, world, A_C + 0.25) == nil)

print("attack.aim_lane_wide")
do
  local _, awx, awy = aim_angle(1)
  local ok, l, r = A.aim_lane_wide(OX, OY, PMX, PMY, world, awx, awy, 0.5)
  check("centre aim: lane too narrow", ok == false, tostring(l) .. "/" .. tostring(r))
  local _, twx, twy = aim_angle(3)
  ok = A.aim_lane_wide(OX, OY, PMX, PMY, world, twx, twy, 0.5)
  check("top-right aim: lane wide", ok == true)
end

print("attack._clear_aim_from_world")
do
  local idx = A._clear_aim_from_world(OX, OY, PMX, PMY, world, nil, 0)
  check("min_lane 0 (knob off): centre, as before", idx == 1, idx)
  idx = A._clear_aim_from_world(OX, OY, PMX, PMY, world)
  check("no min_lane (fire-time callers): centre, as before", idx == 1, idx)
  local idx2, awx, awy = A._clear_aim_from_world(OX, OY, PMX, PMY, world, nil, 0.5)
  check("min_lane 0.5: the narrow centre is dropped", idx2 ~= nil and idx2 ~= 1, idx2)
  check("min_lane 0.5: the aim it picks has a wide lane",
        idx2 and A.aim_lane_wide(OX, OY, PMX, PMY, world, awx, awy, 0.5) == true)
  -- A dead blocker: every angle in the sector is clear, the centre stands.
  bp.health = 0
  check("dead blocker, min_lane 0.5: centre",
        A._clear_aim_from_world(OX, OY, PMX, PMY, world, nil, 0.5) == 1)
  bp.health = 15
  -- No aim with a wide lane: nil, so the sweep drops the spot.
  local saved = cpf.simulate_shot_angle
  cpf.simulate_shot_angle = function(_, _, a)
    if d(a, A_C) <= 0.05 then
      return { { mx = 101, my = 127 }, { mx = 104, my = 126 }, { mx = PMX, my = PMY } }
    end
    return { { mx = 101, my = 127 }, { mx = BMX, my = BMY }, { mx = PMX, my = PMY } }
  end
  check("only a narrow lane anywhere, min_lane 0.5: no aim",
        A._clear_aim_from_world(OX, OY, PMX, PMY, world, nil, 0.5) == nil)
  check("only a narrow lane anywhere, min_lane 0: centre",
        A._clear_aim_from_world(OX, OY, PMX, PMY, world, nil, 0) == 1)
  cpf.simulate_shot_angle = saved
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
