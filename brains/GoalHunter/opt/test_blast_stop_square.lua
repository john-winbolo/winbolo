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
-- or boat square on its line.
--
-- 1. A builder two squares deep in a forest, the line entering the forest
--    two squares before him: the shell stops on the first forest square, so
--    he cannot be killed from this heading and the shot is not taken.
-- 2. A builder on the first forest square on the line: the stop is on his
--    square, so he can be killed and the shot is taken.
-- 3. A builder on a lone forest square next to grass, the line on the
--    grass within 128 of him: no forest on the line, open ground, so he can
--    be killed.
-- 4. KEEL (SHELL_STOPS_ON_FOREST_BOAT off, LIVE_PHYSICS off) gives the old
--    answers: the shell flies on over a forest, open ground everywhere,
--    even in a game whose version has the square-hit rule.
-- 5. SHELL_STOPS_ON_FOREST_BOAT on, a game older than 2.2.2
--    (info.terrain_square_hit false): the stop is open ground on the stop
--    square's middle (the old engine rule).
-- 6. A boat stops the shell as a forest does.
-- 7. No info.terrain_square_hit (an old host): the old rule, as in 5.
--
-- The square-hit rule is not a knob: live_physics.apply sets
-- C.LGM_BLAST_TERRAIN_SQUARE_HIT from info.terrain_square_hit, which the
-- engine sets from the game's version (true from 2.2.2 on).
-- =========================================================================

package.path = "./?.lua;" .. package.path
_G.print2 = _G.print2 or function() end

local bit = require("bitcompat")
local C = require("constants")
local U = require("util")
local D = require("danger")
local LP = require("live_physics")

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

-- stop: C.SHELL_STOPS_ON_FOREST_BOAT. square_hit: info.terrain_square_hit
-- as the engine sends it (true for 2.2.2 and later, false before), or nil
-- for a host that does not send it.
local function knobs(stop, square_hit)
  C.SHELL_STOPS_ON_FOREST_BOAT = stop
  LP.apply({ terrain_square_hit = square_hit })
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
knobs(true, true)
do
  check("a 2.2.2 game turns the square-hit rule on",
        C.LGM_BLAST_TERRAIN_SQUARE_HIT == true, C.LGM_BLAST_TERRAIN_SQUARE_HIT)
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

print("4. keel: the old answers, even in a 2.2.2 game")
set_map(forest_row)
C.LIVE_PHYSICS = false
knobs(false, true)
do
  check("LIVE_PHYSICS off leaves the square-hit rule off",
        C.LGM_BLAST_TERRAIN_SQUARE_HIT == false, C.LGM_BLAST_TERRAIN_SQUARE_HIT)
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

C.LIVE_PHYSICS = true

print("5. a game older than 2.2.2: the shell stops, the stop is open ground")
set_map(forest_row)
knobs(true, false)
do
  check("an old game leaves the square-hit rule off",
        C.LGM_BLAST_TERRAIN_SQUARE_HIT == false, C.LGM_BLAST_TERRAIN_SQUARE_HIT)
  local ex, ey, sqx = D.capture_shot_end(OX, OY1, HX, HY, 6 * 256)
  check("the shot blows up on (104,100)'s middle, no square rule",
        ex == 104 * 256 + 128 and ey == OY1 and sqx == nil, ex)
  check("a man 122 from that middle can be killed",
        killable(OY1, 104 * 256 + 250, OY1))
  check("a man in that square's far corner, 171 away, cannot",
        not killable(OY1, 104 * 256 + 250, OY1 + 120))
  check("the man two squares deep cannot",
        not killable(OY1, 106 * 256 + 128, OY1))
end

print("6. a boat stops the shell as a forest does")
set_map({ [key(103, 100)] = C.T_BOAT })
knobs(true, true)
do
  local _, smx = D.forest_boat_stop(OX, OY1, HX, HY, MAX_ALONG)
  check("the line stops on the boat (103,100)", smx == 103, smx)
  check("a man past the boat cannot be killed",
        not killable(OY1, 105 * 256 + 128, OY1))
end

print("7. no info.terrain_square_hit: the old rule")
set_map(forest_row)
knobs(true, true)
knobs(true, nil)
do
  check("a missing field turns the square-hit rule off",
        C.LGM_BLAST_TERRAIN_SQUARE_HIT == false, C.LGM_BLAST_TERRAIN_SQUARE_HIT)
  local ex, ey, sqx = D.capture_shot_end(OX, OY1, HX, HY, 6 * 256)
  check("the shot blows up on (104,100)'s middle, no square rule",
        ex == 104 * 256 + 128 and ey == OY1 and sqx == nil, ex)
  check("a man 122 from that middle can be killed",
        killable(OY1, 104 * 256 + 250, OY1))
  check("a man in that square's far corner, 171 away, cannot",
        not killable(OY1, 104 * 256 + 250, OY1 + 120))
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
