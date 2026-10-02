-- =========================================================================
-- test_removed_items.lua -- standalone tests for pills and bases that a
-- scenario removes from the map (game.remove_pill / game.remove_base).
--
-- The engine keeps a removed item's slot, so the ids above it do not move,
-- and drops the item from the object scan. It says which ids are still on
-- the map in info.pills_on_map / info.bases_on_map (bit n = id n). The brain
-- keeps its own records of every item it has seen, so without the masks a
-- removed item stays in its lists for the rest of the round.
--
-- Checks:
--   * world.update drops the record and the tile index of a removed pill and
--     a removed base, including one the bot cannot see;
--   * with no masks (an older engine) nothing is dropped;
--   * an ally's known-world record, an ally's carry advert and an engine
--     event cannot bring a removed item back;
--   * a pill put back on the map is seen again.
-- The orders side (a held order on a removed item lapses) is covered by the
-- ROOST tests tests/roost/removed_*.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_removed_items.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

-- The engine constants world.lua reads as globals (braincore.c sets them).
OBJECT_TANK, OBJECT_SHOT, OBJECT_PILLBOX, OBJECT_REFBASE = 0, 1, 2, 3
OBJECT_HOSTILE, OBJECT_NEUTRAL = 1, 2
NEUTRAL_PLAYER = 0xFF
EVENT_PILL_CAPTURED, EVENT_BASE_CAPTURED, EVENT_TANK_KILLED = 1, 2, 3
EVENT_PILL_UPDATE, EVENT_BASE_UPDATE, EVENT_BASE_STOCK = 5, 6, 7
EVENT_PLAYER_LEAVE, EVENT_LGM_LOST = 8, 10

local W = require("world")

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

local function new_world()
  local world = { pills = {}, bases = {} }
  W.reset(world)
  return world
end

-- One map object as the engine hands it over: world units, 256 to a square.
local function obj(kind, id, mx, my, health, info_bits)
  return { type = kind, idnum = id, x = mx * 256 + 128, y = my * 256 + 128,
           direction = health, info = info_bits or OBJECT_NEUTRAL }
end

local function info_with(objects, pills_mask, bases_mask)
  return { objects = objects, events = {}, player_number = 0, allies = 0,
           pills_on_map = pills_mask, bases_on_map = bases_mask }
end

-- Two pills and two bases, all in view on tick 1.
local function seeded()
  local world = new_world()
  W.update(world, info_with({
    obj(OBJECT_PILLBOX, 0, 10, 10, 15), obj(OBJECT_PILLBOX, 1, 20, 20, 15),
    obj(OBJECT_REFBASE, 0, 30, 30, 0),  obj(OBJECT_REFBASE, 1, 40, 40, 0),
  }, 3, 3), 1)
  return world
end

print("world.update drops removed items")
do
  local world = seeded()
  check("both pills known", world.pills[0] and world.pills[1])
  check("both bases known", world.bases[0] and world.bases[1])

  -- Tick 2: nothing in view (the bot drove away); pill 1 and base 0 removed.
  W.update(world, info_with({}, 1, 2), 2)
  check("pill 0 (out of sight, on the map) kept", world.pills[0] ~= nil)
  check("pill 1 (removed) dropped", world.pills[1] == nil, world.pills[1])
  check("pill 1 tile index dropped", W.pill_at(world, 20, 20) == nil)
  check("pill 0 tile index kept", W.pill_at(world, 10, 10) ~= nil)
  check("base 0 (removed) dropped", world.bases[0] == nil, world.bases[0])
  check("base 0 tile index dropped", W.base_at(world, 30, 30) == nil)
  check("base 1 kept", world.bases[1] ~= nil and W.base_at(world, 40, 40) ~= nil)
end

print("no masks: nothing dropped")
do
  local world = seeded()
  W.update(world, info_with({}, nil, nil), 2)
  check("pill 1 kept without masks", world.pills[1] ~= nil)
  check("base 0 kept without masks", world.bases[0] ~= nil)
end

print("a removed base does not take a live base's tile entry")
do
  local world = seeded()
  -- A stale record for base 0 that happens to sit on base 1's square.
  world.bases[0].mx, world.bases[0].my = 40, 40
  W.update(world, info_with({}, 3, 2), 2)
  check("base 0 dropped", world.bases[0] == nil)
  check("base 1 still indexed on its square", W.base_at(world, 40, 40) == world.bases[1])
end

print("allies and events cannot bring a removed item back")
do
  local world = seeded()
  W.update(world, info_with({}, 1, 2), 2)
  -- An ally still has the old records and relays them.
  W.sync_ally_world(world, {
    { kind = "p", id = 1, cls = "n", tick = 3, mx = 20, my = 20, hp = 15, intank = 0, from = 2 },
    { kind = "b", id = 0, cls = "n", tick = 3, mx = 30, my = 30, hp = 0, from = 2 },
    { kind = "p", id = 0, cls = "h", tick = 3, mx = 10, my = 10, hp = 15, intank = 0, from = 2 },
  }, 3, 0)
  check("known-world relay does not re-add removed pill 1", world.pills[1] == nil, world.pills[1])
  check("known-world relay does not re-add removed base 0", world.bases[0] == nil, world.bases[0])
  check("known-world relay still updates pill 0", world.pills[0] and world.pills[0].owner == "hostile")

  W.sync_ally_carried(world, { [1] = 2 }, 4)
  check("carry advert does not re-add removed pill 1", world.pills[1] == nil, world.pills[1])

  local state = { tick = 5 }
  local ev_info = info_with({}, 1, 2)
  ev_info.events = {
    { type = EVENT_PILL_UPDATE, data = { 1, 20, 20, NEUTRAL_PLAYER, 0, 15 } },
    { type = EVENT_BASE_UPDATE, data = { 0, NEUTRAL_PLAYER, 0, 0, 0 } },
  }
  W.process_events(world, ev_info, state)
  check("pill event does not re-add removed pill 1", world.pills[1] == nil, world.pills[1])
  check("base event does not re-add removed base 0", world.bases[0] == nil, world.bases[0])
end

print("a pill put back on the map is seen again")
do
  local world = seeded()
  W.update(world, info_with({}, 1, 3), 2)
  check("pill 1 dropped", world.pills[1] == nil)
  W.update(world, info_with({ obj(OBJECT_PILLBOX, 1, 22, 22, 15) }, 3, 3), 3)
  check("pill 1 back at its new square", world.pills[1] and world.pills[1].mx == 22)
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
