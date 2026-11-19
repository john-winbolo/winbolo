-- =========================================================================
-- test_spike_pen_skip.lua — standalone unit tests (2026-11-03) for the
-- SPIKE_PEN_SKIP_MAX_SHOTS knob (goals.spike_pen_skip_shots): a pill this
-- player's world model says dies in at most that many shells pays no spike
-- cross-penalty.
--   * shots = ceil(known armour / rules.pill_shell_damage), 1 when the rule
--     is not sent
--   * 0 = off (the KEEL value): never waived
--   * a rule of 0 (shells take nothing) never waives
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_spike_pen_skip.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

_G.gh_threat = _G.gh_threat or setmetatable({}, { __index = function()
  return function() return 0 end
end })

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

local function pill(hp) return { mx = 10, my = 10, owner = "hostile", health = hp } end

check("default is 5", C.SPIKE_PEN_SKIP_MAX_SHOTS == 5, C.SPIKE_PEN_SKIP_MAX_SHOTS)
check("keel is 0", C.PRESETS and C.PRESETS.keel
      and C.PRESETS.keel.SPIKE_PEN_SKIP_MAX_SHOTS == 0,
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.SPIKE_PEN_SKIP_MAX_SHOTS)

local saved = C.SPIKE_PEN_SKIP_MAX_SHOTS
C.SPIKE_PEN_SKIP_MAX_SHOTS = 5

-- classic rules (no info.rules): one armour per shell
check("armour 3, no rules: 3 shots", G.spike_pen_skip_shots(pill(3), {}) == 3,
      G.spike_pen_skip_shots(pill(3), {}))
check("armour 5: 5 shots, waived", G.spike_pen_skip_shots(pill(5), nil) == 5)
check("armour 6: not waived", G.spike_pen_skip_shots(pill(6), nil) == nil)
check("armour 15: not waived", G.spike_pen_skip_shots(pill(15), nil) == nil)
check("armour 0 (dead): not waived", G.spike_pen_skip_shots(pill(0), nil) == nil)

-- a scenario rule of 3 armour per shell: armour 15 -> 5 shots, 16 -> 6
local info3 = { rules = { pill_shell_damage = 3 } }
check("rule 3, armour 15: 5 shots", G.spike_pen_skip_shots(pill(15), info3) == 5,
      G.spike_pen_skip_shots(pill(15), info3))
check("rule 3, armour 16: 6 shots, not waived", G.spike_pen_skip_shots(pill(16), info3) == nil)
check("rule 0: never waived", G.spike_pen_skip_shots(pill(1), { rules = { pill_shell_damage = 0 } }) == nil)

C.SPIKE_PEN_SKIP_MAX_SHOTS = 0
check("knob 0 (keel): never waived", G.spike_pen_skip_shots(pill(1), nil) == nil)
C.SPIKE_PEN_SKIP_MAX_SHOTS = saved

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
