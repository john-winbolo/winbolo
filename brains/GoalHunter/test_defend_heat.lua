-- =========================================================================
-- test_defend_heat.lua — standalone unit tests (2026-11-18) for
-- C.DEFEND_ALARM_HEAT: the defend_pill alarm heat target choice
-- (attack.defend_heat_pick / defend_heat_engaged / defend_heat_steer).
--
-- Checked here:
--   * knob defaults (on; keel and mast off)
--   * engaged (enemy hit in the window, or a hostile shell at D now)
--   * engaged -> D is never picked, a neighbour with E in pill range is
--   * engaged, neighbour with E out of pill range -> nothing
--   * not engaged + enemy within 9 of the pill -> volley capped at 2
--   * not engaged + enemy beyond 9 -> hp cap only
--   * choice: closest to E wins
--   * a volley on D aborts when D turns engaged
-- No engine: LOS is a stub that is always clear.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_defend_heat.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

_G.print2 = _G.print2 or function() end
_G.gh_threat = _G.gh_threat or setmetatable({}, { __index = function()
  return function() return 0 end
end })

local C = require("constants")
local U = require("util")
local A = require("attack")

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

local function los_clear() return true end

-- Tank at tile (100,100), full gun range (7 tiles), plenty of shells.
local function mk_info()
  return { tankx = U.m2w(100), tanky = U.m2w(100), gunrange = 14,
           shells = 40, speed = 0, direction = 0, inboat = false }
end

local function mk_pill(mx, my, hp, anger)
  return { mx = mx, my = my, owner = "friendly", health = hp or 15,
           anger = anger or 0 }
end

print("== knobs ==")
check("DEFEND_ALARM_HEAT default on", C.DEFEND_ALARM_HEAT == true, C.DEFEND_ALARM_HEAT)
check("near tiles 9", C.DEFEND_HEAT_NEAR_ENEMY_TILES == 9, C.DEFEND_HEAT_NEAR_ENEMY_TILES)
check("near max shots 2", C.DEFEND_HEAT_NEAR_ENEMY_MAX_SHOTS == 2, C.DEFEND_HEAT_NEAR_ENEMY_MAX_SHOTS)
check("keel off", C.PRESETS.keel.DEFEND_ALARM_HEAT == false, C.PRESETS.keel.DEFEND_ALARM_HEAT)
check("mast off", C.PRESETS.mast.DEFEND_ALARM_HEAT == false, C.PRESETS.mast.DEFEND_ALARM_HEAT)

print("== engaged ==")
do
  local D = mk_pill(104, 100)
  local e = A.defend_heat_engaged(D, 1000)
  check("quiet pill not engaged", e == false, e)
  D._alarm_hits = { { t = 900 } }
  local e2, n2 = A.defend_heat_engaged(D, 1000)
  check("hit 100 ticks ago -> engaged", e2 == true and n2 == 1, n2)
  local e3 = A.defend_heat_engaged(D, 1000 + 251)
  check("hit 351 ticks ago (window 250) -> not engaged", e3 == false, e3)
  D._alarm_hits = nil
  D._hostile_shell_tick = 1000
  local e4, _, s4 = A.defend_heat_engaged(D, 1000)
  check("hostile shell this tick -> engaged", e4 == true and s4 == true, e4)
  local e5 = A.defend_heat_engaged(D, 1001)
  check("hostile shell last tick only -> not engaged", e5 == false, e5)
end

print("== engaged: never D ==")
do
  -- D at (104,100), N at (102,103); enemy E at (108,104).
  -- d(D,E)=5.7, d(N,E)=6.1 (inside pill range 8). Both in our gun range.
  local world = { pills = { [1] = mk_pill(104, 100), [2] = mk_pill(102, 103) } }
  local E = { id = 7, mx = 108, my = 104 }
  local state = { perc = { enemy_tanks = { E } } }
  local best, rows = A.defend_heat_pick(state, world, mk_info(), 1, E, true, 1000, los_clear, true)
  check("engaged picks N", best and best.id == 2 and best.role == "N", best and best.id)
  local rD
  for _, r in ipairs(rows) do if r.id == 1 then rD = r end end
  check("D row reason engaged_never_D", rD and rD.reason == "engaged_never_D", rD and rD.reason)
  check("engaged: no near cap (4 shots)", best and best.shots == 4, best and best.shots)

  -- Neighbour with E outside its pill range: nothing at all.
  world.pills[2] = mk_pill(96, 100)   -- d(N,E) = sqrt(144+16) = 12.6 > 8
  local b2 = A.defend_heat_pick(state, world, mk_info(), 1, E, true, 1000, los_clear, true)
  check("engaged, N out of pill range -> none", b2 == nil, b2 and b2.id)
end

print("== not engaged ==")
do
  local world = { pills = { [1] = mk_pill(104, 100), [2] = mk_pill(102, 103) } }
  local E = { id = 7, mx = 108, my = 104 }
  local state = { perc = { enemy_tanks = { E } } }
  local best = A.defend_heat_pick(state, world, mk_info(), 1, E, false, 1000, los_clear, true)
  check("not engaged picks D (closer to E)", best and best.id == 1 and best.role == "D", best and best.id)
  check("enemy 5.7 <= 9 -> capped at 2", best and best.shots == 2 and best.near_cap, best and best.shots)

  -- hp cap below the near cap: hp 7 buys 1 shot, min(1, 2) = 1.
  world.pills[1] = mk_pill(104, 100, 7)
  local b2 = A.defend_heat_pick(state, world, mk_info(), 1, E, false, 1000, los_clear, true)
  check("hp 7 -> 1 shot (hp cap under near cap)", b2 and b2.id == 1 and b2.shots == 1 and not b2.near_cap,
        b2 and b2.shots)

  -- Enemy far: D at (104,100), N at (105,100); E at (114,100), 10 from D
  -- and 9.0 from N.  N is closer to E but must keep E in its 8-tile range
  -- -> N rejected; D has no near cap (10 > 9) -> 4 shots.
  local w3 = { pills = { [1] = mk_pill(104, 100), [2] = mk_pill(105, 100) } }
  local E3 = { id = 7, mx = 114, my = 100 }
  local s3 = { perc = { enemy_tanks = { E3 } } }
  local b3, rows3 = A.defend_heat_pick(s3, w3, mk_info(), 1, E3, false, 1000, los_clear, true)
  check("far enemy: D picked", b3 and b3.id == 1, b3 and b3.id)
  check("far enemy (10 > 9): no near cap, 4 shots", b3 and b3.shots == 4 and not b3.near_cap, b3 and b3.shots)
  local rN
  for _, r in ipairs(rows3) do if r.id == 2 then rN = r end end
  check("N with E 9 tiles away: enemy_out_of_pill_range", rN and rN.reason == "enemy_out_of_pill_range",
        rN and rN.reason)

  -- Choice: two neighbours, the one closer to E wins.
  local w4 = { pills = { [1] = mk_pill(104, 100, 15, 0.9), [2] = mk_pill(103, 103), [3] = mk_pill(105, 104) } }
  local b4 = A.defend_heat_pick(state, w4, mk_info(), 1, E, false, 1000, los_clear, true)
  check("D already hot; closest-to-E neighbour #3 wins", b4 and b4.id == 3, b4 and b4.id)

  -- Out of our gun range: no detour.
  local w5 = { pills = { [1] = mk_pill(110, 100) } }   -- 10 tiles from tank
  local b5 = A.defend_heat_pick(state, w5, mk_info(), 1, E, false, 1000, los_clear, true)
  check("D out of our gun range -> none", b5 == nil, b5 and b5.id)
end

-- Engine key bits (globals the C side sets; values do not matter here).
_G.KEY_TURNLEFT, _G.KEY_TURNRIGHT, _G.KEY_FASTER, _G.KEY_SLOWER, _G.KEY_SHOOT =
  _G.KEY_TURNLEFT or 0x04, _G.KEY_TURNRIGHT or 0x08, _G.KEY_FASTER or 0x10,
  _G.KEY_SLOWER or 0x20, _G.KEY_SHOOT or 0x01
_G.KEY_MORERANGE = _G.KEY_MORERANGE or 0x40
_G.KEY_LESSRANGE = _G.KEY_LESSRANGE or 0x80

print("== steer: volley on D aborts when D turns engaged ==")
do
  _G.BRAIN_DEBUG_MODE = false
  local D = mk_pill(104, 100)
  local world = { pills = { [1] = D } }
  local E = { id = 7, mx = 108, my = 104 }
  local state = { tick = 1000, perc = { enemy_tanks = { E } } }
  local goal = { kind = "defend_pill", alarm = true, target_id = 1 }
  local k = A.defend_heat_steer(state, world, mk_info(), goal, 1000, los_clear)
  check("volley starts on D", k ~= nil and goal._heat_pid == 1 and goal._heat_role == "D",
        goal._heat_pid)
  check("substate defend_heat_D", goal.substate == "defend_heat_D", goal.substate)
  check("capped at 2 (enemy 5.7 from D)", goal._heat_shots == 2, goal._heat_shots)
  check("near cap names the exit", goal._heat_cap_why == "near_enemy_cap", goal._heat_cap_why)
  check("state mirror carries role", state._heat_active and state._heat_active.defend == "D",
        state._heat_active and state._heat_active.defend)
  D._hostile_shell_tick = 1001
  local k2 = A.defend_heat_steer(state, world, mk_info(), goal, 1001, los_clear)
  check("engaged flip -> nil keys", k2 == nil, k2)
  check("volley cleared", goal._heat_pid == nil and state._heat_active == nil, goal._heat_pid)
  check("substate cleared", goal.substate == nil, goal.substate)
  check("retry stamped", state._heat_pill_retry and state._heat_pill_retry[1] == 1001,
        state._heat_pill_retry and state._heat_pill_retry[1])
end

print("== steer: knob off ==")
do
  local saved = C.DEFEND_ALARM_HEAT
  C.DEFEND_ALARM_HEAT = false
  local world = { pills = { [1] = mk_pill(104, 100) } }
  local state = { tick = 1000, perc = { enemy_tanks = { { id = 7, mx = 108, my = 104 } } } }
  local goal = { kind = "defend_pill", alarm = true, target_id = 1 }
  local k = A.defend_heat_steer(state, world, mk_info(), goal, 1000, los_clear)
  check("off -> nil, no volley", k == nil and goal._heat_pid == nil, k)
  C.DEFEND_ALARM_HEAT = saved
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
