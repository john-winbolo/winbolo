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

print("reload ladder, knob off (PILLBOX_ATTACK_NORMAL 100, halved per hit, floor 6)")
do
  local b = 1 / 3
  local want = { [0] = 100, 50, 25, 12, 6 }
  for hits = 0, 4 do
    local r = S.pill_interval(hits * b, b, nil, nil, nil, false)
    check(string.format("%d hits: %d ticks", hits, want[hits]), r == want[hits], r)
  end
  local function pi(a, ...) return S.pill_interval(a, b, nil, nil, nil, false) end
  check("a fifth hit changes nothing", pi(1.0 + 2 * b) == 6, pi(1.0 + 2 * b))
  check("anger 0.15 rounds to 0 hits", pi(0.15) == 100, pi(0.15))
  check("anger 0.2 rounds to 1 hit", pi(0.2) == 50, pi(0.2))
  check("no anger", pi(nil) == 100, pi(nil))
  check("live rules: 80 halved twice, floor 30", S.pill_interval(2 * b, b, 80, 30, nil, false) == 30,
    S.pill_interval(2 * b, b, 80, 30, nil, false))
  -- the knob off is today's map: round(anger / bump), at most 4
  local h = S.anger_hits(1.0, 0.3333, false)
  check("knob off: anger 1.0 = 3 hits", h == 3, h)
end

print("PT2_START_ANGER_RISE: the anger to hits map (ceil, 3+ = the minimum)")
do
  local b = 0.3333
  local function h(a) local n, m = S.anger_hits(a, b, true); return n, m end
  check("anger 0: 0 hits", (h(0)) == 0, (h(0)))
  check("anger 0.1: 1 hit (ceil)", (h(0.1)) == 1, (h(0.1)))
  check("anger 0.3333: 1 hit (the 1e-6)", (h(0.3333)) == 1, (h(0.3333)))
  check("anger 0.6666: 2 hits (the 1e-6)", (h(0.6666)) == 2, (h(0.6666)))
  check("anger 0.7: 3 hits = the minimum", select(2, h(0.7)) == true and (h(0.7)) == S.HEAT_MAX_HITS, (h(0.7)))
  check("anger 1.0: the minimum", select(2, h(1.0)) == true, select(2, h(1.0)))
  local function pi(a, ...) return S.pill_interval(a, b, ...) end
  check("rise: anger 0 -> 100", pi(0, nil, nil, nil, true) == 100, pi(0, nil, nil, nil, true))
  check("rise: anger 0.1 -> 50", pi(0.1, nil, nil, nil, true) == 50, pi(0.1, nil, nil, nil, true))
  check("rise: anger 0.5 -> 25", pi(0.5, nil, nil, nil, true) == 25, pi(0.5, nil, nil, nil, true))
  check("rise: anger 0.8 -> minimum 6", pi(0.8, nil, nil, nil, true) == 6, pi(0.8, nil, nil, nil, true))
  check("rise: live minimum 9 at 3 hits", pi(1.0, 100, 9, 2, true) == 9, pi(1.0, 100, 9, 2, true))
  check("rise: divisor 3, 2 hits: 100 -> 33 -> 11", pi(0.6, 100, 6, 3, true) == 11, pi(0.6, 100, 6, 3, true))
  check("rise: divisor 4, 2 hits floor 10: 100 -> 25 -> 10", pi(0.6, 100, 10, 4, true) == 10, pi(0.6, 100, 10, 4, true))
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

print("PT2_START_ANGER_RISE: the tick walk")
do
  local o = { attack_ticks = 100, min_ticks = 6, divisor = 2, cooldown = 32, tank_reload = 13, flight = 8 }
  -- the review's example: calm A, 68 ticks, no cover. By hand: fire at 0
  -- (loaded); hits at 8 (50), 21 (25), 34 (12), 47 (6), 60; fires at 25,
  -- 37, 47, 53, 59, 65: 7 shells (the old sum: ceil(68 / 100) = 1).
  local n, per = S.rise_shells({ { speed = 100, count = true } }, 68, o)
  check("calm A, 68 ticks: 7 shells", n == 7, n)
  check("calm A, 68 ticks: per pill 7", per[1] == 7, per[1])
  -- no tank shell inside the window (flight 68): the old count
  local o2 = {}; for k, v in pairs(o) do o2[k] = v end; o2.flight = 68
  n = S.rise_shells({ { speed = 100, count = true } }, 68, o2)
  check("no hit in the window: 1 shell, as the old sum", n == 1, n)
  -- an angry pill never hit calms: 25 -> 26 (32), 27 (64), 28 (96), 29
  -- (128); fires at 0, 25, 51, 78, 106, 135: 6 (the old sum said 7)
  o2.flight = 1000
  n = S.rise_shells({ { speed = 25, count = true } }, 152, o2)
  check("never hit, 152 ticks at 25 (calming): 6 shells", n == 6, n)
  n = S.rise_shells({ { speed = 100, count = true } }, 152, o2)
  check("never hit, calm, 152 ticks at 100: 2 shells, as the old sum", n == 2, n)
  -- cover: 5 shots standing take the first 5 shells
  n = S.rise_shells({ { speed = 100, count = true, cover = { { t = 0, shots = 5 } } } }, 68, o)
  check("calm A, 68 ticks, 5 shots standing: 2 shells", n == 2, n)
  -- a planned wall that stands at tick 40 takes the shells at 47 .. 65
  n = S.rise_shells({ { speed = 100, count = true, cover = { { t = 40, shots = 5 } } } }, 68, o)
  check("calm A, 68 ticks, wall at 40: 3 shells (0, 25, 37)", n == 3, n)
  -- a two pill chain: A's shells hit B (the link), B's go to the tank.
  -- B is calm: its first shell at 0, then A's shells make it angry
  local ps = { { speed = 100, to = 2 }, { speed = 100, count = true } }
  local m, per2 = S.rise_shells(ps, 68, o)
  check("chain: A sends no shell to the tank", per2[1] == 0, per2[1])
  check("chain: B gets angry from A's shells (more than 1)", m > 1, m)
  -- A shot by the tank: B hit at 1, 26, 38, 48, 54, 60, 66 (A fires at
  -- 0, 25, 37, 47, 53, 59, 65; each lands the next tick). B: fire at 0;
  -- 100 -> 50 (1) -> 25 (26) -> 12 (38) -> 6 (48) ...; fires at 26? its
  -- count is 26 >= 25 at tick 26: yes; then 38, 48, 54, 60, 66.
  check("chain: B sends 7 shells", m == 7, m)
  -- a dead chain pill neither fires nor is hit
  n = S.rise_shells({ { speed = 100, off = true }, { speed = 100, count = true } }, 68, o)
  check("dead chain pill 1: B is not hit, 1 shell", n == 1, n)
  -- rise_chain: entries as the callers keep them
  n = S.rise_chain({ { speed = 100, count = true, cover_now = 0,
                       planned = { { mx = 3, my = 4, shots = 5 } } } }, 68,
                   function(mx, my) return (mx == 3 and my == 4) and 40 or nil end, o)
  check("rise_chain: wall at 40, 3 shells", n == 3, n)
  n = S.rise_chain({ { speed = 100, count = true, planned = { { mx = 3, my = 4, shots = 5 } } } }, 68,
                   function() return nil end, o)
  check("rise_chain: a wall it does not know never stands: 7", n == 7, n)
  check("flight: 2 tiles at 32 wu: 16 ticks", S.flight_ticks(128, 128, 2, 0, 32) == 16, S.flight_ticks(128, 128, 2, 0, 32))
  local wt = S.wall_ticks(0, 0, { { mx = 1, my = 0 } }, function() return 10 end)
  check("wall_ticks: one wall", wt and wt[1] == 10, wt and wt[1])
  check("exposure_ticks = the last wall tick", S.exposure_ticks(0, 0, { { mx = 1, my = 0 } }, function() return 10 end) == 10)
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
