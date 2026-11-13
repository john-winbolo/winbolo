-- =========================================================================
-- test_clear_turn_progress.lua — standalone unit tests (2026-11-03) for the
-- finetune turn-in limit (knob C.CLEAR_TURN_TAP_PROGRESS):
--   * aim_lane.scan / nearest — the clear lane around the gun.
--   * aim_lane.turn_in_step — a tick that brings the gun closer to the clear
--     lane does not count against CLEAR_TURN_IN_TAPS; hard cap 2x.
--   * attack.finetune_lane_target — the lane from the tank, with the shell
--     simulator patched so only a lane narrower than one tap step is clear.
-- With the knob off attack.lua never calls these: the old count (every tick
-- since the shot reached the pill, give up at 6) is the "no progress" case.
-- No engine: terrain is all road and the shell simulator is a stand-in.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_clear_turn_progress.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C   = require("constants")
local U   = require("util")
local cpf = require("cpathfinder")
local A   = require("attack")
local AL  = require("aim_lane")

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
local LIMIT = 6   -- attack.lua CLEAR_TURN_IN_TAPS

-- ── Knobs ────────────────────────────────────────────────────────────────
print("knobs")
local keel = C.PRESETS and C.PRESETS.keel or {}
check("CLEAR_TURN_TAP_PROGRESS default true", C.CLEAR_TURN_TAP_PROGRESS == true)
check("PRESETS.keel CLEAR_TURN_TAP_PROGRESS = false", keel.CLEAR_TURN_TAP_PROGRESS == false)

-- ── aim_lane.scan / nearest ──────────────────────────────────────────────
print("aim_lane.scan / nearest")
do
  -- Clear only in [100.40, 100.45]: narrower than one tap step (0.125).
  local function clear_at(a) return a >= 100.40 and a <= 100.45 end
  local lane = AL.scan(clear_at, 100.0)
  local a, e = AL.nearest(lane, 100.0)
  check("nearest clear angle from 100.0 is 100.4375", a and math.abs(a - 100.4375) < 1e-9, a)
  check("error from 100.0 is 0.4375", e and math.abs(e - 0.4375) < 1e-9, e)
  local a2, e2 = AL.nearest(lane, 100.6)
  check("from 100.6 (past the lane) the nearest is back at 100.4375",
        a2 and math.abs(a2 - 100.4375) < 1e-9, a2)
  check("error from 100.6 is 0.1625", e2 and math.abs(e2 - 0.1625) < 1e-9, e2)
  local none = AL.scan(function() return false end, 100.0)
  check("no clear angle -> nil", AL.nearest(none, 100.0) == nil)
  -- Lane across the 0/256 wrap.
  local wl = AL.scan(function(a) return a >= 255.9 or a <= 0.05 end, 0.5)
  local wa, we = AL.nearest(wl, 0.5)
  check("lane across the wrap: nearest from 0.5 is 0.0", wa and math.abs(wa) < 1e-9, wa)
  check("lane across the wrap: error 0.5", we and math.abs(we - 0.5) < 1e-9, we)
end

-- ── aim_lane.turn_in_step ────────────────────────────────────────────────
print("aim_lane.turn_in_step")
-- Run a finetune from the tick the shot first reaches the pill. errs[i] is
-- the error to the clear lane on the i-th tick after (nil = no clear angle).
-- Returns the tick number (raw count) at which it gives up, or nil.
local function run(errs, n)
  local goal = { _finetune_start = 1000, _finetune_reached_tap = 4 }
  for i = 0, (n or #errs) - 1 do
    local _, raw, give_up = AL.turn_in_step(goal, 4 + i, errs[i + 1], LIMIT)
    if give_up then return raw, goal end
  end
  return nil, goal
end
do
  local flat = {}
  for i = 1, 20 do flat[i] = 0.3 end
  check("no progress: gives up at 6 (same as the old count)", run(flat) == LIMIT, run(flat))
  check("no clear angle at all: gives up at 6", run({}, 20) == LIMIT)
  local down = {}
  for i = 1, 20 do down[i] = 2.0 - i * 0.1 end
  check("error falls every tick: hard cap at 12", run(down) == 2 * LIMIT, run(down))
  -- Recorded-game shape: a tap jumps past a lane narrower than one tap, then
  -- turns back. Progress ticks 1-2, then the error never beats 0.05 again.
  local over = { 0.40, 0.25, 0.05, 0.08, 0.06, 0.08, 0.06, 0.08, 0.06, 0.08, 0.06, 0.08 }
  -- ticks: raw 0 (first, sets best), 1 free, 2 free, then 3.. count.
  check("overshoot: two free ticks, gives up at raw 8", run(over) == LIMIT + 2, run(over))
  -- Idle ticks (no turn) between taps do not move the gun: they count.
  local idle = { 0.40, 0.30, 0.30, 0.20, 0.20, 0.10, 0.10, 0.10, 0.10, 0.10, 0.10, 0.10 }
  check("idle ticks between progress count: gives up at raw 9", run(idle) == 9, run(idle))
  -- A new finetune (new _finetune_start) starts a new count.
  local _, goal = run(flat)
  goal._finetune_start = 2000
  goal._finetune_reached_tap = 10
  local c, raw, g = AL.turn_in_step(goal, 10, 0.3, LIMIT)
  check("new finetune: count restarts at 0", c == 0 and raw == 0 and g == false, c)
end

-- ── attack.finetune_lane_target ──────────────────────────────────────────
print("attack.finetune_lane_target")
do
  -- Tank at (100,127) facing east-ish; pill at (107,127); a friendly pill at
  -- (104,127). Shells at angle a in [64.40, 64.45] pass the blocker; other
  -- angles in [63, 66] cross the blocker square first; the rest miss.
  local PMX, PMY, BMX, BMY = 107, 127, 104, 127
  local calls = 0
  cpf.simulate_shot_angle = function(_, _, a)
    calls = calls + 1
    if a >= 64.40 and a <= 64.45 then
      return { { mx = 101, my = 127 }, { mx = 104, my = 126 }, { mx = PMX, my = PMY } }
    elseif a >= 63 and a <= 66 then
      return { { mx = 101, my = 127 }, { mx = BMX, my = BMY }, { mx = PMX, my = PMY } }
    end
    return { { mx = 101, my = 127 }, { mx = 104, my = 130 }, { mx = 107, my = 131 } }
  end
  local bp = { mx = BMX, my = BMY, health = 15, owner = "friendly" }
  local world = { pills = { [4] = bp },
                  pill_at = { [BMY * 256 + BMX] = { { id = 4, pill = bp } } } }
  local info = { tankx = 100 * 256 + 128, tanky = 127 * 256 + 128, gunrange = 14,
                 tank_angle = 64.0 }
  local goal = { mx = PMX, my = PMY }
  local a, e = A.finetune_lane_target(info, goal, world, 64.0)
  check("lane found at 64.4375", a and math.abs(a - 64.4375) < 1e-9, a)
  check("error 0.4375", e and math.abs(e - 0.4375) < 1e-9, e)
  local n1 = calls
  local a2 = A.finetune_lane_target(info, goal, world, 64.2)
  check("same tank spot: lane reused (no new shell sims)", calls == n1, calls - n1)
  check("same lane from 64.2", a2 and math.abs(a2 - 64.4375) < 1e-9, a2)
  info.tankx = info.tankx + 1
  A.finetune_lane_target(info, goal, world, 64.2)
  check("tank moved: lane sampled again", calls > n1, calls - n1)
  -- A dead blocker does not block: the whole [63,66] band is clear.
  bp.health = 0
  goal._ft_lane = nil
  local a3, e3 = A.finetune_lane_target(info, goal, world, 64.0)
  check("dead blocker: gun angle itself is clear", a3 and e3 == 0, e3)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
