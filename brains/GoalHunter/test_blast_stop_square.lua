-- =========================================================================
-- test_blast_stop_square.lua -- standalone tests for the capture shot's
-- forest and boat rule (danger.lua forest_boat_stop, capture_shot_end,
-- capture_man_killable).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_blast_stop_square.lua
-- or with any lua on PATH.
--
-- The engine decides a builder's death by the square where the shell
-- stops, not by the builder's own square. A shell stops on the first forest
-- or boat square on its line, and that stop is a hit on its square: only a
-- builder on that square dies.
--
-- 1. A builder two squares deep in a forest, the line entering the forest
--    two squares before him: the shell stops on the first forest square, so
--    he cannot be killed from this heading and the shot is not taken.
-- 2. A builder on the first forest square on the line: the stop is on his
--    square, so he can be killed and the shot is taken.
-- 3. A builder on a lone forest square next to grass, the line on the
--    grass within 128 of him: no forest on the line, open ground, so he can
--    be killed.
-- 4. KEEL (SHELL_STOPS_ON_FOREST_BOAT off) gives the old answers: the
--    shell flies on over a forest, open ground everywhere.
-- 5. The stop is a hit on its square: a builder on the next square, 128
--    from the stop square's middle, cannot be killed. A builder on the far
--    edge of the stop square can.
-- 6. A boat stops the shell as a forest does.
-- 7. A tank on a forest square: its own square does not stop its shell
--    (the engine first tests it 192 wu out), so a builder 3 squares out
--    on open ground can be killed.
-- 8. A tank whose line clips a forest within 160 wu: the engine never
--    tests there, so the same builder can be killed.
-- 9. The far end: the burst place (128 * sight + 32) is tested.
-- 10. lgm_shell_gate tests our hull before a forest, as the engine does.
-- =========================================================================

package.path = "./?.lua;" .. package.path
_G.print2 = _G.print2 or function() end

local bit = require("bitcompat")
local C = require("constants")
local U = require("util")
local D = require("danger")

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

-- ── Engine stand-ins: a map of grass with the squares set below ─────────
local map = {}
local function set_map(t) map = t end
local function key(mx, my) return my * 256 + mx end
U.ttype = function(mx, my) return map[key(mx, my)] or C.T_GRASS end
U.ttype_peek = U.ttype

-- stop: C.SHELL_STOPS_ON_FOREST_BOAT.
local function knobs(stop)
  C.SHELL_STOPS_ON_FOREST_BOAT = stop
end

-- The tank on square (100, 100), heading east.
local OX, HX, HY = 100 * 256 + 128, 1, 0
local MAX_ALONG = 14 * 128

-- killable for a man at world (mwx, mwy), open ground, line at height oy.
local function killable(oy, mwx, mwy)
  local cmx, cmy = bit.rshift(mwx, 8), bit.rshift(mwy, 8)
  local along = mwx - OX
  local perp = math.abs(mwy - oy)
  return D.capture_man_killable(OX, oy, HX, HY, MAX_ALONG, cmx, cmy, mwx, mwy,
                                perp, along, false)
end

-- Case 1, 2, 4, 5: forest on squares 104..106 of row 100.
local forest_row = {}
for x = 104, 106 do forest_row[key(x, 100)] = C.T_FOREST end
local OY1 = 100 * 256 + 128

print("1. builder two squares deep in a forest")
set_map(forest_row)
knobs(true)
do
  local mwx, mwy = 106 * 256 + 128, OY1
  local t, smx, smy = D.forest_boat_stop(OX, OY1, HX, HY, MAX_ALONG)
  check("the line stops on the first forest square (104,100)",
        smx == 104 and smy == 100, tostring(smx) .. "," .. tostring(smy))
  check("the stop is where the line enters it", t == 104 * 256 - OX, t)
  check("he cannot be killed from this heading", not killable(OY1, mwx, mwy))
  local ex, ey, sqx, sqy = D.capture_shot_end(OX, OY1, HX, HY, mwx - OX)
  check("a shot aimed at him blows up on (104,100)'s middle",
        ex == 104 * 256 + 128 and ey == 100 * 256 + 128, ex)
  check("and is a hit on (104,100), not on his square",
        sqx == 104 and sqy == 100, sqx)
end

print("2. builder on the first forest square on the line")
do
  local mwx, mwy = 104 * 256 + 200, OY1 + 40
  check("he can be killed", killable(OY1, mwx, mwy))
  local _, _, sqx, sqy = D.capture_shot_end(OX, OY1, HX, HY, mwx - OX)
  check("the shot is a hit on his square", sqx == 104 and sqy == 100, sqx)
  -- His far corner, 171 from the stop square's middle: still on the square.
  check("in the far corner of that square too",
        killable(OY1, 104 * 256 + 250, OY1 + 120))
end

print("3. builder on a lone forest square next to grass")
do
  -- Forest only on (106, 101). The line runs along row 100 at 200 down the
  -- square, so it never enters row 101; he stands 40 into the forest
  -- square, 96 off the line.
  set_map({ [key(106, 101)] = C.T_FOREST })
  local oy = 100 * 256 + 200
  local mwx, mwy = 106 * 256 + 128, 101 * 256 + 40
  check("no forest or boat on the line",
        D.forest_boat_stop(OX, oy, HX, HY, MAX_ALONG) == nil)
  check("he can be killed (open ground, 96 off the line)",
        killable(oy, mwx, mwy))
  local ex, ey, sqx = D.capture_shot_end(OX, oy, HX, HY, mwx - OX)
  check("the shot lands where it is aimed, on the grass",
        ex == mwx and ey == oy and sqx == nil, ex)
end

print("4. keel: the old answers")
set_map(forest_row)
knobs(false)
do
  check("no stop is modelled", D.forest_boat_stop(OX, OY1, HX, HY, MAX_ALONG) == nil)
  check("the man two squares deep is killable, as before",
        killable(OY1, 106 * 256 + 128, OY1))
  local ex, ey, sqx = D.capture_shot_end(OX, OY1, HX, HY, 6 * 256)
  check("the shot lands at the end of the line",
        ex == OX + 6 * 256 and ey == OY1 and sqx == nil, ex)
  set_map({ [key(106, 101)] = C.T_FOREST })
  check("the lone-forest man is killable, as before",
        killable(100 * 256 + 200, 106 * 256 + 128, 101 * 256 + 40))
end

print("5. the stop is a hit on its square")
set_map(forest_row)
knobs(true)
do
  local ex, ey, sqx, sqy = D.capture_shot_end(OX, OY1, HX, HY, 6 * 256)
  check("the shot blows up on (104,100)'s middle, a hit on that square",
        ex == 104 * 256 + 128 and ey == OY1 and sqx == 104 and sqy == 100, ex)
  check("a man on the far edge of that square can be killed",
        killable(OY1, 104 * 256 + 255, OY1))
  check("a man on the next square, 128 from that middle, cannot",
        not killable(OY1, 105 * 256, OY1))
end

print("6. a boat stops the shell as a forest does")
set_map({ [key(103, 100)] = C.T_BOAT })
knobs(true)
do
  local _, smx = D.forest_boat_stop(OX, OY1, HX, HY, MAX_ALONG)
  check("the line stops on the boat (103,100)", smx == 103, smx)
  local _, _, sqx = D.capture_shot_end(OX, OY1, HX, HY, 6 * 256)
  check("and the stop is a hit on the boat's square", sqx == 103, sqx)
  check("a man past the boat cannot be killed",
        not killable(OY1, 105 * 256 + 128, OY1))
end

-- 7..9: where the engine first and last tests a tank's shell (shells.c).
-- shellsAddItem puts it shell_start_add steps out (5 x 32 = 160 wu), and
-- shellsUpdate moves it one step before each test, so the first test is at
-- 192 wu and the last at 128 * sight + 32 (the burst, kill_lgm.shell_travel
-- with LGM_SHELL_MODEL_RULES on). Nothing nearer than 192 is tested.
local KL = require("kill_lgm")

print("7. tank on a forest square, open ground ahead")
set_map({ [key(100, 100)] = C.T_FOREST })
knobs(true)
do
  check("the first test is 192 wu out", D.tank_shell_first_test_wu() == 192,
        D.tank_shell_first_test_wu())
  check("the tank's own forest square does not stop its shell",
        D.forest_boat_stop(OX, OY1, HX, HY, MAX_ALONG) == nil)
  check("an enemy builder 3 squares out can be killed",
        killable(OY1, 103 * 256 + 128, OY1))
  local ex, ey, sqx = D.capture_shot_end(OX, OY1, HX, HY, 3 * 256)
  check("a shot aimed at him lands at him, not on the tank's square",
        ex == OX + 3 * 256 and ey == OY1 and sqx == nil, ex)
  local t = D.forest_boat_stop(OX, OY1, HX, HY, MAX_ALONG, 32)
  check("a walk told to start at 32 still finds the tank's square", t == 32, t)
end

print("8. tank within 160 wu of a forest, open ground past it")
do
  -- Heading north-east from 28 above the middle of (100,100). The line is
  -- in (100,99) from t = 141 to t = 181 only: the old walk tested it at
  -- t = 160, the engine's first test at t = 192 is already in (101,99).
  set_map({ [key(100, 99)] = C.T_FOREST })
  local s = math.sqrt(0.5)
  local hx, hy = s, -s
  local ox, oy = 100 * 256 + 128, 100 * 256 + 100
  check("the line clips (100,99) at t = 160",
        bit.rshift(math.floor(ox + hx * 160), 8) == 100
        and bit.rshift(math.floor(oy + hy * 160), 8) == 99)
  check("no stop is found: the engine never tests (100,99)",
        D.forest_boat_stop(ox, oy, hx, hy, MAX_ALONG) == nil)
  local along = 3 * 256
  local mwx = math.floor(ox + hx * along)
  local mwy = math.floor(oy + hy * along)
  local cmx, cmy = bit.rshift(mwx, 8), bit.rshift(mwy, 8)
  check("an enemy builder 3 squares out can be killed",
        D.capture_man_killable(ox, oy, hx, hy, MAX_ALONG, cmx, cmy, mwx, mwy,
                               0, along, false))
end

print("9. the far end: the burst place is tested")
do
  -- Sight 14: the burst is 128 * 14 + 32 = 1824 out. From 230 into
  -- (100,100), 1792 out is in (107,100) and 1824 out is in (108,100).
  local ox = 100 * 256 + 230
  local burst = KL.shell_travel(14, true)
  check("the burst is 1824 out", burst == 1824, burst)
  set_map({ [key(108, 100)] = C.T_FOREST })
  local t, smx = D.forest_boat_stop(ox, OY1, HX, HY, burst)
  check("a forest on the burst square stops the shell", smx == 108 and t == burst, t)
  local _, _, sqx = D.capture_shot_end(ox, OY1, HX, HY, burst)
  check("and the shot is a hit on (108,100)", sqx == 108, sqx)
end

-- 10. danger.lgm_shell_gate tests the tanks before the terrain, as
-- shellsCalcCollision does. An enemy shell that reaches our hull over a
-- forest is a tank hit: an open-ground blast at the shell's place, which
-- catches a builder near it on another square.
print("10. lgm_shell_gate: the hull before the forest")
do
  _G.OBJECT_SHOT = _G.OBJECT_SHOT or 1
  local walk_x, walk_y
  _G.cpf_lgm_walk_path = function(_, _, _, _, _, _, _, _, out)
    out[1], out[2] = walk_x, walk_y
    return 1
  end
  C.BUILDER_POOL_SHELL_GATE = true
  local Y = 100 * 256 + 128
  -- One shell that is at (nx, Y) after its next step, angle a.
  local function gate(tank_x, owner, a, nx)
    local vx = U.bsin_f(a) * C.SHELL_SPEED
    local info = {
      tankx = tank_x, tanky = Y, player_number = 0,
      objects = { { type = OBJECT_SHOT, owner = owner, angle = a, life = 5,
                    x = nx - vx, y = Y } },
    }
    return D.lgm_shell_gate({}, info, 104, 100)
  end
  -- a) Tank on grass 72 from the forest (101,100); the builder steps to
  -- 52 short of the forest. An enemy shell flying west is at 20 into the
  -- forest: 76 from the tank (a hull hit), 96 from him.
  set_map({ [key(101, 100)] = C.T_FOREST })
  walk_x, walk_y = 100 * 256 + 180, Y
  local h = gate(100 * 256 + 200, 3, 192, 101 * 256 + 20)
  check("a) the hull hit over the forest catches him",
        h ~= nil and h.how == "tank", h and h.how)
  -- b) Tank on a forest square (100,100), 60 into it; the builder has
  -- walked west onto the grass (99,100). An enemy shell flying east is 10
  -- into the forest: 50 from the tank, 36 from him.
  set_map({ [key(100, 100)] = C.T_FOREST })
  walk_x, walk_y = 99 * 256 + 230, Y
  h = gate(100 * 256 + 60, 3, 64, 100 * 256 + 10)
  check("b) tank on forest: the hull hit catches him off the square",
        h ~= nil and h.how == "tank", h and h.how)
  -- c) Our own shell does not hit our hull: it stops on the forest, a
  -- hit on that square, and he is not on it.
  h = gate(100 * 256 + 60, 0, 64, 100 * 256 + 10)
  check("c) our own shell stops on the forest and spares him", h == nil,
        h and h.how)
  -- d) The forest far from the tank still stops the shell first.
  set_map({ [key(102, 100)] = C.T_FOREST })
  walk_x, walk_y = 100 * 256 + 180, Y
  h = gate(100 * 256 + 128, 3, 192, 102 * 256 + 20)
  check("d) a forest stop away from the hull spares him", h == nil,
        h and h.how)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
