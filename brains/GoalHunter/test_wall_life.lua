-- =========================================================================
-- test_wall_life.lua -- standalone tests: wall shots follow building_life.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_wall_life.lua
--
-- src/bolo/building.c buildingAddItem: a full wall falls on hit
-- building_life + 1; a half wall has 1..building_life hits left, and the
-- brain counts the middle, rounded down: floor((building_life + 1) / 2).
-- live_physics.lua group WALL rewrites C.WALL_HP_FULL, C.WALL_HP_HALF and
-- C.DECOY_GETAWAY_WALL_LIFE from info.rules.building_life.
--
-- 1. building_life 1, 4 and 7: full and half wall shots, the getaway's
--    wall life and full-wall shots, the C pathfinder wall cost, and the
--    wall_hp_between line sums.
-- 2. No info.rules / no building_life / 0: the classic 4 numbers.
-- 3. LIVE_PHYSICS off (keel) or LIVE_PHYSICS_WALL off: the classic numbers.
-- 4. Shield cover stays at SHIELD_WALL_COVER_SHOTS 5 (matches the C scorer).
-- =========================================================================

package.path = "./?.lua;" .. package.path

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

local C  = require("constants")
local LP = require("live_physics")
local GA = require("decoy_getaway")
local U  = require("util")
local PF = require("pathfinder")
local PC = require("pill_cover")

-- A line of tiles along y = 10: x = 11 a full wall, x = 13 a half wall.
local TILES = { [11] = C.T_BUILDING, [13] = C.T_HALFBUILD }
U.in_map = function() return true end
U.ttype = function(x, y)
  if y == 10 and TILES[x] then return TILES[x] end
  return C.T_GRASS
end

local function at(life)
  LP.apply({ rules = { building_life = life } })
end

print("1. building_life 1, 4, 7")
local CASES = {
  -- life, full, half, getaway life, getaway full-wall shots, cpf wall cost
  { 1, 2, 1, 1, 2, 12 },
  { 4, 5, 2, 4, 5, 30 },
  { 7, 8, 4, 7, 8, 48 },
}
for _, c in ipairs(CASES) do
  local life, full, half, gl, gs, cost = c[1], c[2], c[3], c[4], c[5], c[6]
  at(life)
  local tag = "building_life " .. life .. ": "
  check(tag .. "full wall " .. full .. " shots", C.WALL_HP_FULL == full, C.WALL_HP_FULL)
  check(tag .. "half wall " .. half .. " shots", C.WALL_HP_HALF == half, C.WALL_HP_HALF)
  check(tag .. "half wall is at least 1 and at most building_life",
        C.WALL_HP_HALF >= 1 and C.WALL_HP_HALF <= life, C.WALL_HP_HALF)
  check(tag .. "getaway wall life " .. gl, C.DECOY_GETAWAY_WALL_LIFE == gl, C.DECOY_GETAWAY_WALL_LIFE)
  check(tag .. "getaway full wall " .. gs .. " shots, damaged wall 1",
        GA.shots_left("wall") == gs and GA.shots_left("wall_damaged") == 1, GA.shots_left("wall"))
  check(tag .. "C pathfinder wall cost " .. cost, LP.wall_shoot_cost() == cost, LP.wall_shoot_cost())
  check(tag .. "line through one full wall: " .. full,
        PF.wall_hp_between(10, 10, 12, 10) == full, PF.wall_hp_between(10, 10, 12, 10))
  check(tag .. "line through a full and a half wall: " .. (full + half),
        PF.wall_hp_between(10, 10, 14, 10) == full + half, PF.wall_hp_between(10, 10, 14, 10))
  check(tag .. "clear line: 0", PF.wall_hp_between(14, 10, 20, 10) == 0, PF.wall_hp_between(14, 10, 20, 10))
end
check("WALL_SHOOT_COST itself is not rewritten (base overlays keep it)", C.WALL_SHOOT_COST == 30,
      C.WALL_SHOOT_COST)

print("2. no rule sent: the classic numbers")
for _, info in ipairs({ {}, { rules = {} }, { rules = { building_life = 0 } } }) do
  at(7)
  LP.apply(info)
  check("fallback: full 5, half 2, getaway life 4, cost 30",
        C.WALL_HP_FULL == 5 and C.WALL_HP_HALF == 2 and C.DECOY_GETAWAY_WALL_LIFE == 4
        and LP.wall_shoot_cost() == 30, C.WALL_HP_FULL)
end

print("3. LIVE_PHYSICS off / LIVE_PHYSICS_WALL off")
at(7)
C.LIVE_PHYSICS = false
at(7)
check("LIVE_PHYSICS off (keel): full 5, half 2, getaway life 4",
      C.WALL_HP_FULL == 5 and C.WALL_HP_HALF == 2 and C.DECOY_GETAWAY_WALL_LIFE == 4, C.WALL_HP_FULL)
check("keel preset has LIVE_PHYSICS off", C.PRESETS.keel.LIVE_PHYSICS == false, C.PRESETS.keel.LIVE_PHYSICS)
C.LIVE_PHYSICS = true
C.LIVE_PHYSICS_WALL = false
at(1)
check("LIVE_PHYSICS_WALL off: full 5, half 2, getaway life 4",
      C.WALL_HP_FULL == 5 and C.WALL_HP_HALF == 2 and C.DECOY_GETAWAY_WALL_LIFE == 4, C.WALL_HP_FULL)
C.LIVE_PHYSICS_WALL = true
at(1)
check("LIVE_PHYSICS_WALL back on: building_life 1 again", C.WALL_HP_FULL == 2, C.WALL_HP_FULL)

print("4. shield cover units")
for _, life in ipairs({ 1, 4, 7 }) do
  at(life)
  check("building_life " .. life .. ": a wall slot is 5 cover shots",
        PC.slot_cover_shots({}, 1, 1, C.T_BUILDING) == 5 and C.SHIELD_WALL_COVER_SHOTS == 5,
        PC.slot_cover_shots({}, 1, 1, C.T_BUILDING))
end
at(4)

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
