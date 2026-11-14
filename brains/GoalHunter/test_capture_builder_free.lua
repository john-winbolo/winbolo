-- =========================================================================
-- test_capture_builder_free.lua — standalone unit tests (2026-11-04) for
-- C.CAPTURE_PILL_BUILDER_FREE:
--   * the knob is true by default and false in PRESETS.keel;
--   * a land capture_pill gives builder mode "opportunistic", and the builder
--     pool's eligibility does not answer mode_owned;
--   * a sea capture_pill in seek_trees is still "gather" with need_trees;
--   * knob off: a land capture_pill is "gather" and the pool answers
--     mode_owned (gather), as before.
-- No engine: the tank estimator and the fire clock are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_capture_builder_free.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

_G.cpf_estimate_tank_travel_ticks = function() return 50 end
_G.KIND_NORMAL = 0

local C      = require("constants")
local danger = require("danger")
local B      = require("builder")
local BP     = require("builder_pool")

-- The tank has not been hit: no fire age.
danger.tank_fire_age = function() return nil, nil end

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

local function info_at(mx, my)
  return { tankx = mx * 256 + 128, tanky = my * 256 + 128,
           carried_pills = 0, man_status = C.LGM_INTANK, inboat = false,
           armour = 40, shells = 40, trees = 12, mines = 0, player_number = 0 }
end

local function run(goal)
  local st = { tick = 10577, builder = {}, goal = goal }
  local w  = { pills = {}, bases = {}, pill_at = {} }
  local info = info_at(100, 100)
  B.set_mode(st, w, info, goal)
  local ok, why = BP.eligibility(st, w, info, st.tick)
  return st.builder, ok, why
end

local function land() return { kind = "capture_pill", mx = 104, my = 100 } end
local function sea()
  return { kind = "capture_pill", mx = 104, my = 100, substate = "seek_trees",
           sea = { trees_need = 21, S = { 103, 100 }, F = { 102, 100 } } }
end

-- ── 1. knob values ───────────────────────────────────────────────────────
print("1. knob")
check("default true", C.CAPTURE_PILL_BUILDER_FREE == true, C.CAPTURE_PILL_BUILDER_FREE)
check("KEEL false", C.PRESETS.keel.CAPTURE_PILL_BUILDER_FREE == false,
      C.PRESETS.keel.CAPTURE_PILL_BUILDER_FREE)

-- ── 2. knob on ───────────────────────────────────────────────────────────
print("2. knob on")
do
  local b, ok, why = run(land())
  check("land capture_pill -> opportunistic", b.mode == "opportunistic", b.mode)
  check("land capture_pill need_trees 0", b.need_trees == 0, b.need_trees)
  check("land capture_pill target is the pill", b.target and b.target.mx == 104
        and b.target.my == 100)
  check("pool eligible", ok == true, why)
  check("pool reason not mode_owned", not (why and why:find("mode_owned", 1, true)), why)

  b, ok, why = run(sea())
  check("sea seek_trees -> gather", b.mode == "gather", b.mode)
  check("sea seek_trees need_trees 21", b.need_trees == 21, b.need_trees)
  check("sea seek_trees pool mode_owned (gather)", ok == false
        and why == "mode_owned (gather)", why)
end

-- ── 3. knob off (KEEL) ───────────────────────────────────────────────────
print("3. knob off")
C.CAPTURE_PILL_BUILDER_FREE = false
do
  local b, ok, why = run(land())
  check("land capture_pill -> gather", b.mode == "gather", b.mode)
  check("land capture_pill need_trees 0", b.need_trees == 0, b.need_trees)
  check("pool mode_owned (gather)", ok == false and why == "mode_owned (gather)", why)

  b, ok, why = run(sea())
  check("sea seek_trees -> gather", b.mode == "gather", b.mode)
  check("sea seek_trees need_trees 21", b.need_trees == 21, b.need_trees)
end
C.CAPTURE_PILL_BUILDER_FREE = true

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
