-- =========================================================================
-- test_shot_tracker_lag.lua — standalone unit tests (2026-11-25) for
-- C.QUICK_REPAIR_LAG_AWARE in shot_tracker.lua and quick_repair.lua.
--
-- A bot that plays through the network predicts its own tank: the shells
-- count drops at the press, so the tracker makes the shot at once, but the
-- real shell shows in the view a round trip later, that many steps behind
-- the predicted shot.
--
--   1. Lag 0 (a bot inside the server): the shot matches at lag 0 and its
--      record is the old one, knob on or off.
--   2. Lag 3 and 4 (60 ms): the knob keeps the shot in flight with lag 3, walks
--      the real shell on by 3 steps, and puts its death where the shell was
--      seen; the old rule loses it.
--   3. Lag 8 (150 ms): the shell shows first at age 8. The knob finds it;
--      the old rule kills the shot at age 8 (INITIAL_MATCH_TICKS) before
--      it shows. With no lag measured yet the knob waits LGM_AIM_DELAY_MAX
--      more thinks.
--   4. quick_repair.lag_d is 1 with the knob off, and with no measure.
--   5. quick_repair.kill_tick with d = 1 is the old record; with d = 3 a
--      dead target counts as dead 2 thinks after it was first seen dead,
--      and the first shot is now + 3.
--
-- Run from this directory:
--   ../../build-xc/_deps/luajit_src-src/src/luajit.exe test_shot_tracker_lag.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

OBJECT_SHOT    = OBJECT_SHOT or 1
OBJECT_HOSTILE = OBJECT_HOSTILE or 0x10
OBJECT_NEUTRAL = OBJECT_NEUTRAL or 0x20

local C  = require("constants")
local ST = require("shot_tracker")
local QR = require("quick_repair")

local pass, fail = 0, 0
local function check(name, ok, msg)
  if ok then pass = pass + 1
  else fail = fail + 1; print("  FAIL " .. name .. (msg and ("  (" .. msg .. ")") or "")) end
end

-- One shell fired east (direction 64) from (1000, 1000) at tick 100. The
-- engine's shell is `lag` steps behind the predicted shot in the view, and
-- shows only once it is `lag` steps old. dies_at: the tick the real shell
-- hits (in the predicted frame); it leaves the view `lag` ticks later.
local function run(lag, knob, dies_at, ticks)
  C.QUICK_REPAIR_LAG_AWARE = knob
  ST.reset()
  local info = { shells = 40, direction = 64, tank_angle = 64,
                 tankx = 1000, tanky = 1000, objects = {} }
  ST.update(info, 99)
  info.shells = 39
  ST.update(info, 100)
  local shot = ST.shots[1]
  local x0 = shot.fire_fx
  local log = {}
  for t = 101, ticks do
    local objs = {}
    local age = t - 99
    local seen = t - lag            -- the predicted tick the view shows
    if age >= lag + 1 and (not dies_at or seen < dies_at) then
      objs[1] = { type = OBJECT_SHOT, info = 0,
                  x = x0 + 32 * (seen - 99), y = 1000, life = 51 - (seen - 99) }
    end
    info.objects = objs
    ST.update(info, t)
    log[t] = { status = shot.status, lag = shot.lag, matched = shot.matched }
  end
  return shot, log
end

-- 1. lag 0
do
  local a = run(0, false, 120, 125)
  local b = run(0, true, 120, 125)
  check("lag0 old dies", a.status == "dead" and a.dead_tick == 120)
  check("lag0 knob same death tick", b.status == "dead" and b.dead_tick == a.dead_tick)
  check("lag0 knob same death place", b.dead_fx == a.dead_fx and b.dead_fy == a.dead_fy)
  check("lag0 knob lag 0", (b.lag or 0) == 0)
  check("lag0 view_lag 0", ST.view_lag == 0)
end

-- 2. lag 3
do
  -- The old rule keeps lag 3 only at the edge of its 96 wu; lag 4 it loses.
  local a, la = run(4, false, 120, 125)
  check("lag4 old loses the shell in flight", la[110].status == "dead",
        tostring(la[110].status))
  local b4, lb4 = run(4, true, 120, 125)
  check("lag4 knob in flight at 110", lb4[110].status == "in_flight" and b4.lag == 4)
  local b, lb = run(3, true, 120, 125)
  check("lag3 knob in flight at 110", lb[110].status == "in_flight" and lb[110].matched)
  check("lag3 knob lag 3", b.lag == 3, tostring(b.lag))
  check("lag3 knob view_lag 3", ST.view_lag == 3)
  check("lag3 knob dies when the view loses it", b.status == "dead" and b.dead_tick == 123,
        tostring(b.dead_tick))
  -- The shell was last seen at predicted tick 119 (x0 + 19 * 32); the
  -- death place is one step on from it (cur at t = x0 + 32 * (t - 99)).
  check("lag3 knob death place", b.dead_fx == b.fire_fx + 21 * 32, tostring(b.dead_fx))
end

-- 2b. the real shell walked on to now
do
  C.QUICK_REPAIR_LAG_AWARE = true
  local b, lb = run(3, true, nil, 110)
  check("lag3 real place is now", b.real_fx == b.cur_fx, tostring(b.real_fx) .. " vs " .. tostring(b.cur_fx))
  check("lag3 real life walked on", b.real_life == 51 - 11, tostring(b.real_life))
end

-- 3. lag 8
do
  local a, la = run(8, false, 130, 135)
  local b, lb = run(8, true, 130, 135)
  check("lag8 old never matches", la[108].status == "dead", tostring(la[108].status))
  check("lag8 knob matched at 108", lb[108].matched == true)
  check("lag8 knob lag 8", b.lag == 8, tostring(b.lag))
  check("lag8 knob in flight at 125", lb[125].status == "in_flight")
end

-- 4. lag_d
do
  C.QUICK_REPAIR_LAG_AWARE = false
  check("lag_d off = 1", QR.lag_d({}) == 1)
  C.QUICK_REPAIR_LAG_AWARE = true
  check("lag_d no measure = default", QR.lag_d({}) == (C.LGM_AIM_DELAY_DEFAULT or 1))
end

-- 5. kill_tick
do
  local goal = { mx = 10, my = 10 }
  local k1 = QR.kill_tick(goal, { health = 0 }, {}, 500, 1)
  check("kill_tick d1 dead now", k1.t == 500 and k1.src == "dead")
  local k0 = QR.kill_tick(goal, { health = 0 }, {}, 500)
  check("kill_tick no d dead now", k0.t == 500)
  goal = { mx = 10, my = 10 }
  local a = QR.kill_tick(goal, { health = 0 }, {}, 500, 3)
  local b = QR.kill_tick(goal, { health = 0 }, {}, 501, 3)
  local c = QR.kill_tick(goal, { health = 0 }, {}, 503, 3)
  check("kill_tick d3 dead held", a.t == 502 and b.t == 502 and c.t == 503,
        string.format("%s %s %s", a.t, b.t, c.t))
end

print(string.format("%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
