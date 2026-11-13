-- =========================================================================
-- test_pill_cover.lua — standalone unit tests (2026-11-03) for pill_cover.lua,
-- the one rule for how much shield cover a pill on a square gives:
--   walls = floor(health / 5) for a friendly (own or allied) pill, else 0.
-- And the protected-take build loop helpers built on it (attack.lua
-- build_walls): a slot with any pill on it is never waited on, a friendly
-- pill's cover is 5 * floor(health / 5) shots, a 0-wall pill gives 0.
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pill_cover.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C  = require("constants")
local PC = require("pill_cover")

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

local ROAD = 4
local T_WALL, T_HALF = C.T_BUILDING, C.T_HALFBUILD

-- A world with pills on squares. pills: list of { id, mx, my, owner, health,
-- in_tank }.
local function make_world(pills)
  local w = { pills = {}, pill_at = {} }
  for _, p in ipairs(pills) do
    w.pills[p.id] = p
    local k = p.my * 256 + p.mx
    w.pill_at[k] = w.pill_at[k] or {}
    table.insert(w.pill_at[k], { id = p.id, pill = p })
  end
  return w
end

print("walls_for_health")
for _, c in ipairs({ { 15, 3 }, { 14, 2 }, { 10, 2 }, { 9, 1 }, { 7, 1 }, { 5, 1 },
                     { 4, 0 }, { 3, 0 }, { 1, 0 }, { 0, 0 }, { -2, 0 } }) do
  check(string.format("health %d -> %d walls", c[1], c[2]),
        PC.walls_for_health(c[1]) == c[2], PC.walls_for_health(c[1]))
end
check("health nil -> 0 walls", PC.walls_for_health(nil) == 0)

print("pill_walls (owner)")
check("own/ally pill 'friendly' 15 -> 3", PC.pill_walls({ owner = "friendly", health = 15 }) == 3)
check("'allied' 10 -> 2", PC.pill_walls({ owner = "allied", health = 10 }) == 2)
check("enemy 'hostile' 15 -> 0", PC.pill_walls({ owner = "hostile", health = 15 }) == 0)
check("'neutral' 15 -> 0", PC.pill_walls({ owner = "neutral", health = 15 }) == 0)
check("friendly in a tank -> 0", PC.pill_walls({ owner = "friendly", health = 15, in_tank = true }) == 0)
check("no pill -> 0", PC.pill_walls(nil) == 0)

print("pill_on_square")
do
  local w = make_world({ { id = 4, mx = 140, my = 127, owner = "friendly", health = 15 } })
  check("pill on its square", PC.pill_on_square(w, 140, 127) == w.pills[4])
  check("no pill on the next square", PC.pill_on_square(w, 141, 127) == nil)
  -- Old entry: the live table says the pill has moved (picked up and put
  -- down elsewhere).
  w.pills[4] = { id = 4, mx = 150, my = 127, owner = "friendly", health = 15 }
  check("old pill_at entry for a moved pill -> nil", PC.pill_on_square(w, 140, 127) == nil)
  w.pills[4] = { id = 4, mx = 140, my = 127, owner = "friendly", health = 15, in_tank = true }
  check("pill now in a tank -> nil", PC.pill_on_square(w, 140, 127) == nil)
  local d = make_world({ { id = 2, mx = 10, my = 10, owner = "neutral", health = 0 } })
  check("dead pill still stands on its square", PC.pill_on_square(d, 10, 10) ~= nil)
end

print("build loop: slot_done (never wait on a pill slot)")
do
  local w = make_world({
    { id = 1, mx = 1, my = 1, owner = "friendly", health = 15 },
    { id = 2, mx = 2, my = 1, owner = "friendly", health = 3 },
    { id = 3, mx = 3, my = 1, owner = "hostile",  health = 15 },
    { id = 4, mx = 4, my = 1, owner = "neutral",  health = 0 },
  })
  check("friendly 15 on road square -> done", PC.slot_done(w, 1, 1, ROAD))
  check("friendly 3 (0 walls) -> done, not waited on", PC.slot_done(w, 2, 1, ROAD))
  check("enemy pill -> done (no wall can go there)", PC.slot_done(w, 3, 1, ROAD))
  check("dead neutral pill -> done", PC.slot_done(w, 4, 1, ROAD))
  check("empty road -> not done", not PC.slot_done(w, 5, 1, ROAD))
  check("wall -> done", PC.slot_done(w, 5, 1, T_WALL))
  check("half-built wall -> done", PC.slot_done(w, 5, 1, T_HALF))
end

print("build loop: slot_cover_shots and the cover sum")
do
  local w = make_world({
    { id = 1, mx = 1, my = 1, owner = "friendly", health = 15 },
    { id = 2, mx = 2, my = 1, owner = "friendly", health = 10 },
    { id = 3, mx = 3, my = 1, owner = "friendly", health = 7 },
    { id = 4, mx = 4, my = 1, owner = "friendly", health = 3 },
    { id = 5, mx = 5, my = 1, owner = "hostile",  health = 15 },
    { id = 6, mx = 6, my = 1, owner = "neutral",  health = 15 },
  })
  check("friendly 15 -> 15 shots", PC.slot_cover_shots(w, 1, 1, ROAD) == 15, PC.slot_cover_shots(w, 1, 1, ROAD))
  check("friendly 10 -> 10 shots", PC.slot_cover_shots(w, 2, 1, ROAD) == 10)
  check("friendly 7 -> 5 shots", PC.slot_cover_shots(w, 3, 1, ROAD) == 5)
  check("friendly 3 -> 0 shots", PC.slot_cover_shots(w, 4, 1, ROAD) == 0)
  check("enemy 15 -> 0 shots", PC.slot_cover_shots(w, 5, 1, ROAD) == 0)
  check("neutral 15 -> 0 shots", PC.slot_cover_shots(w, 6, 1, ROAD) == 0)
  check("wall -> 5 shots", PC.slot_cover_shots(w, 9, 1, T_WALL) == 5)
  check("half-built wall -> 5 shots", PC.slot_cover_shots(w, 9, 1, T_HALF) == 5)
  check("empty road -> 0", PC.slot_cover_shots(w, 9, 1, ROAD) == 0)

  local target = C.PPT_COVER_TARGET_SHOTS or 15
  local function sum(slots)
    local s = 0
    for _, sl in ipairs(slots) do s = s + PC.slot_cover_shots(w, sl[1], 1, sl[2]) end
    return s
  end
  check("a 15-health pill alone ends the build", sum({ { 1, ROAD }, { 9, ROAD } }) >= target)
  check("a 10-health pill alone does not", sum({ { 2, ROAD }, { 9, ROAD } }) < target)
  check("a 10-health pill plus one wall ends it", sum({ { 2, ROAD }, { 9, T_WALL } }) >= target)
  check("a 3-health pill plus two walls does not", sum({ { 4, ROAD }, { 8, T_WALL }, { 9, T_WALL } }) < target)
  check("an enemy pill plus two walls does not", sum({ { 5, ROAD }, { 8, T_WALL }, { 9, T_WALL } }) < target)
end

print("build loop: slot_blocker (newly built count)")
do
  local w = make_world({
    { id = 1, mx = 1, my = 1, owner = "friendly", health = 7 },
    { id = 2, mx = 2, my = 1, owner = "friendly", health = 4 },
    { id = 3, mx = 3, my = 1, owner = "hostile",  health = 15 },
  })
  check("friendly 7 (1 wall) is a blocker", PC.slot_blocker(w, 1, 1, ROAD))
  check("friendly 4 (0 walls) is not", not PC.slot_blocker(w, 2, 1, ROAD))
  check("enemy pill is not", not PC.slot_blocker(w, 3, 1, ROAD))
  check("full wall is", PC.slot_blocker(w, 9, 1, T_WALL))
  check("half-built wall is not (unchanged)", not PC.slot_blocker(w, 9, 1, T_HALF))
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
