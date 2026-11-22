-- =========================================================================
-- test_haul_setup_hyst.lua — standalone unit tests (2026-11-18) for
-- C.TAKE_COVER_HAUL_SETUP_NO_HYST's set-up test, goals.attack_pill_in_setup:
--   * every step in M.ATTACK_PILL_SETUP_SUBS qualifies with clear shot marks
--   * the shooting steps (charge, shoot_pill, engage, swerve, ...)
--     never qualify
--   * any shot mark (_attack_start_tick, _fired > 0, _charge_shells) stops a
--     set-up step from qualifying
--   * the defend close interrupt still accepts ONLY plan_position / approach /
--     blitz_wait (build_walls is a haul set-up step but not a CI one)
--   * the knob is on by default and off in PRESETS.keel and PRESETS.mast
-- The hysteresis loop itself is not driven here (it needs a whole pool).
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_haul_setup_hyst.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")
local G = require("goals")

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

local function atk(sub, extra)
  local g = { kind = "attack_pill", substate = sub, mx = 10, my = 10 }
  for k, v in pairs(extra or {}) do g[k] = v end
  return g
end

print("1. set-up steps qualify")
local SETUP = { "plan_position", "approach", "blitz_wait", "gather_trees",
                "build_walls", "aim", "in_range_position", "in_range_aim_pre",
                "in_range_aim", "in_range_aim_finetune", "detree" }
for _, s in ipairs(SETUP) do
  check("setup " .. s, G.attack_pill_in_setup(atk(s)) == s,
        G.attack_pill_in_setup(atk(s)))
end
local n = 0
for _ in pairs(G.ATTACK_PILL_SETUP_SUBS) do n = n + 1 end
check("set has exactly the listed steps", n == #SETUP, n)

print("2. shooting steps never qualify")
for _, s in ipairs({ "kill_hardline", "charge", "engage",
                     "shoot_pill", "swerve", "post_engage", "loiter", "rush",
                     "disengage", "heat_pill" }) do
  check("not setup " .. s, G.attack_pill_in_setup(atk(s)) == nil,
        G.attack_pill_in_setup(atk(s)))
end

print("3. shot marks")
check("_attack_start_tick", G.attack_pill_in_setup(atk("build_walls", { _attack_start_tick = 5 })) == nil)
check("_fired 1",           G.attack_pill_in_setup(atk("in_range_aim", { _fired = 1 })) == nil)
check("_fired 0 ok",        G.attack_pill_in_setup(atk("in_range_aim", { _fired = 0 })) == "in_range_aim")
check("_charge_shells",     G.attack_pill_in_setup(atk("aim", { _charge_shells = 20 })) == nil)
check("other goal kind",    G.attack_pill_in_setup({ kind = "attack_tank", substate = "approach" }) == nil)
check("nil goal",           G.attack_pill_in_setup(nil) == nil)
check("no substate",        G.attack_pill_in_setup(atk(nil)) == nil)

print("4. close interrupt keeps its own set")
do
  local saved = C.DEFEND_ALARM_CLOSE_INTERRUPT
  C.DEFEND_ALARM_CLOSE_INTERRUPT = true
  local p = { mx = 10, my = 10 }
  for _, s in ipairs(SETUP) do
    local want = (s == "plan_position" or s == "approach" or s == "blitz_wait")
                 and ("attack_pill/" .. s) or nil
    local got = G.defend_close_interrupt({ goal = atk(s) }, p)
    check("CI " .. s .. " -> " .. tostring(want), got == want, got)
  end
  check("CI approach with _fired", G.defend_close_interrupt(
    { goal = atk("approach", { _fired = 2 }) }, p) == nil)
  C.DEFEND_ALARM_CLOSE_INTERRUPT = saved
end

print("5. knob and presets")
check("default on", C.TAKE_COVER_HAUL_SETUP_NO_HYST == true, C.TAKE_COVER_HAUL_SETUP_NO_HYST)
check("keel off", C.PRESETS.keel.TAKE_COVER_HAUL_SETUP_NO_HYST == false,
      C.PRESETS.keel.TAKE_COVER_HAUL_SETUP_NO_HYST)
check("mast off", C.PRESETS.mast.TAKE_COVER_HAUL_SETUP_NO_HYST == false,
      C.PRESETS.mast.TAKE_COVER_HAUL_SETUP_NO_HYST)

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
