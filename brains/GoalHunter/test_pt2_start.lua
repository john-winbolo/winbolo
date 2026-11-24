-- =========================================================================
-- test_pt2_start.lua -- standalone unit tests for pt2_start.lua
-- (PT2_START_WHILE_BUILDING): the reload ladder, the exposure ticks, the
-- shells that get through and the start test.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_start.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local S = require("pt2_start")

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

print("reload ladder (PILLBOX_ATTACK_NORMAL 100, halved per hit, floor 6)")
do
  local b = 1 / 3
  local want = { [0] = 100, 50, 25, 12, 6 }
  for hits = 0, 4 do
    local r = S.pill_interval(hits * b, b)
    check(string.format("%d hits: %d ticks", hits, want[hits]), r == want[hits], r)
  end
  check("a fifth hit changes nothing", S.pill_interval(1.0 + 2 * b, b) == 6, S.pill_interval(1.0 + 2 * b, b))
  check("anger 0.15 rounds to 0 hits", S.pill_interval(0.15, b) == 100, S.pill_interval(0.15, b))
  check("anger 0.2 rounds to 1 hit", S.pill_interval(0.2, b) == 50, S.pill_interval(0.2, b))
  check("no anger", S.pill_interval(nil, b) == 100, S.pill_interval(nil, b))
  check("live rules: 80 halved twice, floor 30", S.pill_interval(2 * b, b, 80, 30) == 30, S.pill_interval(2 * b, b, 80, 30))
end

print("exposure ticks")
do
  -- straight line: 3 squares = 48 ticks; the wall stands on arrival
  local e = S.exposure_ticks(10, 10, { { mx = 13, my = 10 } })
  check("one wall 3 squares off: 3*16 = 48", e == 48, e)
  -- two walls: 3 squares, build, back to the tank (3), out to the second
  -- wall (5 squares: (13,14) is 3,4 off the tank)
  local e2 = S.exposure_ticks(10, 10, { { mx = 13, my = 10 }, { mx = 13, my = 14 } })
  check("two walls: 48 + 20 + 48 + 80 = 196", e2 == 196, e2)
  -- the builder's own trip cost wins when given
  local fn = function(ox, oy, mx, my) return 7 end
  local e3 = S.exposure_ticks(10, 10, { { mx = 13, my = 10 }, { mx = 13, my = 14 } }, fn)
  check("lgm_ticks given: 7 + 20 + 7 + 7 = 41", e3 == 41, e3)
  -- the builder starts away from the tank: later orders start at the tank
  local e5 = S.exposure_ticks(12, 10, { { mx = 13, my = 10 }, { mx = 13, my = 14 } }, nil, nil, 10, 10)
  check("from (12,10), tank (10,10): 16 + 20 + 48 + 80 = 164", e5 == 164, e5)
  -- an unreachable answer (-1): no exposure (E4-4: no start)
  local e4 = S.exposure_ticks(10, 10, { { mx = 13, my = 10 } }, function() return -1 end)
  check("lgm_ticks -1: nil", e4 == nil, e4)
  local e6 = S.exposure_ticks(10, 10, { { mx = 13, my = 10 }, { mx = 13, my = 14 } },
    function(ox, oy, mx, my) if mx == 13 and my == 14 then return -1 end return 7 end)
  check("the second wall unreachable: nil", e6 == nil, e6)
  -- no answer (nil): the straight line
  local e7 = S.exposure_ticks(10, 10, { { mx = 13, my = 10 } }, function() return nil end)
  check("lgm_ticks nil: the straight line", e7 == 48, e7)
  -- the walk back is blessed on the wall he leaves
  local bl = {}
  S.exposure_ticks(10, 10, { { mx = 13, my = 10 }, { mx = 13, my = 14 } },
    function(ox, oy, mx, my, bx, by) bl[#bl + 1] = string.format("%d,%d", bx, by); return 7 end)
  check("bless: the wall, the wall he leaves, the next wall", table.concat(bl, " ") == "13,10 13,10 13,14",
    table.concat(bl, " "))
  check("no walls: 0", S.exposure_ticks(10, 10, {}) == 0, S.exposure_ticks(10, 10, {}))
end

print("shells through and the start test")
do
  check("68 ticks at 100: 1 shell, no cover", S.shells_through(68, 100, 0) == 1, S.shells_through(68, 100, 0))
  check("68 ticks at 100, 5 shots of cover: 0", S.shells_through(68, 100, 5) == 0, S.shells_through(68, 100, 5))
  check("152 ticks at 25: 7 shells", S.shells_through(152, 25, 0) == 7, S.shells_through(152, 25, 0))
  check("152 ticks at 25, 1 forest: 6", S.shells_through(152, 25, 1) == 6, S.shells_through(152, 25, 1))
  check("100 ticks at 100: 1 shell", S.shells_through(100, 100, 0) == 1, S.shells_through(100, 100, 0))
  check("0 ticks: 0", S.shells_through(0, 6, 0) == 0, S.shells_through(0, 6, 0))
  check("armour 40, 7 shells: 40 - 35 = 5 >= 5, start", S.can_start(40, 7, 5) == true)
  check("armour 40, 8 shells: 0 < 5, wait", S.can_start(40, 8, 5) == false)
  check("armour 10, 1 shell: 5 >= 5, start", S.can_start(10, 1, 5) == true)
  check("armour 9, 1 shell: 4 < 5, wait", S.can_start(9, 1, 5) == false)
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
