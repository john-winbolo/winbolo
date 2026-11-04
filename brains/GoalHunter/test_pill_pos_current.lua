-- =========================================================================
-- test_pill_pos_current.lua -- standalone tests for bit 0x20 of the pill
-- update flags byte (PILL_POS_CURRENT, pillbox.h).
--
-- The server tells a player the square of a pill only while that player can
-- see it. An update for a pill out of view carries the last square the player
-- was given, with 0x20 clear. Owner, armour and the in-tank bit (0x10) are
-- real. When such an update says a pill we held as carried is out of the tank,
-- the pill was put down somewhere we were not told about.
--
-- The case: player 3 put pill 12 down at (101,127) on tick 9288. Player 2 was
-- out of view and got ev10[12,130,139,3,0,15]: the old square, 0x20 clear.
-- It used to record a friendly pill at (130,139), relay it to allies as
-- first-hand, and refuse player 3's true record on the equal tick.
--
-- Checks:
--   * 0x20 clear on a carried pill: no square, off the tile index, not
--     relayed, a stale carry advert does not put it back in the tank;
--   * an ally's record with the true square fills it on an equal tick, and
--     an ally's record from before the drop does not;
--   * 0x20 set, the object scan, or an in-tank update give the square back;
--   * 0x20 set and in-tank updates behave as before;
--   * a normal update for an unmoved pill out of view keeps its square.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pill_pos_current.lua
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

local IN_TANK, POS_CURRENT = 0x10, 0x20
local ME, ALLY = 2, 3

local function new_world()
  local world = { pills = {}, bases = {} }
  W.reset(world)
  return world
end

local function info_with(objects, events)
  return { objects = objects or {}, events = events or {},
           player_number = ME, allies = 0 }
end

-- Player 2 is allied with player 3 (bit n = player n).
local function ally_info(objects, events)
  local i = info_with(objects, events)
  i.allies = 2 ^ ALLY
  return i
end

-- One pill update event: [pillIndex, x, y, owner, pillFlags, armour].
local function pill_ev(id, mx, my, owner, flags, armour)
  return { type = EVENT_PILL_UPDATE, data = { id, mx, my, owner, flags, armour } }
end

local function events(world, tick, ...)
  W.process_events(world, ally_info(nil, { ... }), { tick = tick })
end

local function obj(id, mx, my, health)
  return { type = OBJECT_PILLBOX, idnum = id, x = mx * 256 + 128, y = my * 256 + 128,
           direction = health, info = 0 }
end

-- Player 2's view before the drop: pill 12 in player 3's tank, known only
-- from player 3's carry advert.
local function carried_by_ally(tick)
  local world = new_world()
  W.sync_ally_carried(world, { [12] = ALLY }, tick)
  return world
end

local function relayed(world, now)
  world._kw_dirty = nil
  W.collect_kw_changes(world, now)
  return world._kw_dirty and world._kw_dirty["p12"]
end

print("pill 12: put down out of view (0x20 clear)")
do
  local world = carried_by_ally(9280)
  check("before: carried by the ally", world.pills[12] and world.pills[12].in_tank)
  events(world, 9288, pill_ev(12, 130, 139, ALLY, 0, 15))
  local u = world.pills_unk and world.pills_unk[12]
  check("not in world.pills", world.pills[12] == nil, world.pills[12])
  check("held without a square", u and u.pos_unknown == true)
  check("square lost on tick 9288", u and u.pos_unknown_tick == 9288, u and u.pos_unknown_tick)
  check("owner and armour kept", u and u.owner == "friendly" and u.health == 15 and u.owner_player == ALLY)
  check("out of the tank", u and u.in_tank == false and not u._synth_carry)
  check("not on the tile index at the old square", W.pill_at(world, 130, 139) == nil)
  check("not relayed first-hand", relayed(world, 9288) == nil)
  world._kw_dirty = nil
  W.kw_queue_all(world)
  check("not in a resync dump", world._kw_dirty["p12"] == nil)

  -- Player 3's carry advert has no tick and can still name pill 12.
  W.sync_ally_carried(world, { [12] = ALLY }, 9288)
  check("stale carry advert does not put it back in the tank",
        world.pills[12] == nil and world.pills_unk[12] ~= nil)
  W.sync_ally_carried(world, {}, 9289)
  check("advert ending does not drop the record", world.pills_unk[12] ~= nil)

  -- A second update without the square changes nothing about the square.
  events(world, 9290, pill_ev(12, 130, 139, ALLY, 0, 14))
  check("later 0x20-clear update: still no square, armour updated",
        world.pills[12] == nil and world.pills_unk[12].health == 14)

  -- Player 3's own record, same tick as the drop.
  W.sync_ally_world(world, {
    { kind = "p", id = 12, cls = "f", tick = 9288, mx = 101, my = 127, hp = 15, intank = 0, from = ALLY },
  }, 9296, ME)
  local p = world.pills[12]
  check("ally record on an equal tick fills the square", p and p.mx == 101 and p.my == 127)
  check("no longer held without a square", world.pills_unk[12] == nil and p and not p.pos_unknown)
  check("on the tile index at the true square", W.pill_at(world, 101, 127) == p)
  check("deployed and friendly", p and p.in_tank == false and p.owner == "friendly")
  check("not relayed again (ally data)", relayed(world, 9296) == nil)
end

print("ally record from before the drop does not fill the square")
do
  local world = carried_by_ally(9280)
  events(world, 9288, pill_ev(12, 130, 139, ALLY, 0, 15))
  W.sync_ally_world(world, {
    { kind = "p", id = 12, cls = "f", tick = 9287, mx = 130, my = 139, hp = 15, intank = 0, from = 1 },
  }, 9290, ME)
  check("older record skipped", world.pills[12] == nil and world.pills_unk[12] ~= nil)
  check("still not on the tile index", W.pill_at(world, 130, 139) == nil)
end

print("0x20 set: behaves as before")
do
  local world = carried_by_ally(9280)
  events(world, 9288, pill_ev(12, 101, 127, ALLY, POS_CURRENT, 15))
  local p = world.pills[12]
  check("square taken", p and p.mx == 101 and p.my == 127)
  check("last_seen moved", p and p.last_seen == 9288)
  check("placement stamped", p and p.placed_tick == 9288)
  check("on the tile index", W.pill_at(world, 101, 127) == p)
  check("synth carry cleared", p and not p._synth_carry and not p.in_tank)
  check("no record without a square", world.pills_unk[12] == nil)
  local r = relayed(world, 9288)
  check("relayed first-hand with the true square", r and r.mx == 101 and r.my == 127)
end

print("0x20 set after the square was lost gives it back")
do
  local world = carried_by_ally(9280)
  events(world, 9288, pill_ev(12, 130, 139, ALLY, 0, 15))
  events(world, 9300, pill_ev(12, 101, 127, ALLY, POS_CURRENT, 15))
  local p = world.pills[12]
  check("back in world.pills at the sent square", p and p.mx == 101 and p.my == 127)
  check("no longer held without a square", world.pills_unk[12] == nil and not p.pos_unknown)
  check("on the tile index", W.pill_at(world, 101, 127) == p)
  check("last_seen moved", p and p.last_seen == 9300)
end

print("object scan seeing it gives the square back")
do
  local world = carried_by_ally(9280)
  events(world, 9288, pill_ev(12, 130, 139, ALLY, 0, 15))
  W.update(world, ally_info({ obj(12, 101, 127, 15) }), 9301)
  local p = world.pills[12]
  check("back in world.pills at the seen square", p and p.mx == 101 and p.my == 127)
  check("no longer held without a square", world.pills_unk[12] == nil and not p.pos_unknown)
  check("on the tile index", W.pill_at(world, 101, 127) == p)
end

print("in-tank update: behaves as before")
do
  -- A deployed pill we can see, then picked up out of view by player 3.
  local world = new_world()
  W.update(world, ally_info({ obj(12, 130, 139, 15) }), 100)
  check("seen deployed", world.pills[12] and W.pill_at(world, 130, 139) == world.pills[12])
  events(world, 200, pill_ev(12, 130, 139, ALLY, IN_TANK, 15))
  local p = world.pills[12]
  check("in the tank", p and p.in_tank == true and p.owner == "allied")
  check("last_seen moved", p and p.last_seen == 200)
  check("off the tile index", W.pill_at(world, 130, 139) == nil)
  check("not held without a square", world.pills_unk[12] == nil)

  -- Re-picked after the square was lost: back in world.pills, in the tank.
  events(world, 300, pill_ev(12, 130, 139, ALLY, 0, 15))
  check("put down out of view: no square", world.pills[12] == nil and world.pills_unk[12] ~= nil)
  events(world, 400, pill_ev(12, 130, 139, ALLY, IN_TANK, 15))
  p = world.pills[12]
  check("picked up again: back in world.pills in the tank", p and p.in_tank == true and world.pills_unk[12] == nil)
  check("still off the tile index", W.pill_at(world, 130, 139) == nil)
end

print("unmoved pill out of view keeps its square (0x20 clear, never carried)")
do
  local world = new_world()
  W.update(world, ally_info({ obj(5, 40, 40, 15) }), 100)
  -- Out of view; an enemy shoots it. Same square as we hold, 0x20 clear.
  events(world, 150, pill_ev(5, 40, 40, 7, 0, 12))
  local p = world.pills[5]
  check("still in world.pills", p ~= nil and world.pills_unk[5] == nil)
  check("square kept", p and p.mx == 40 and p.my == 40)
  check("still on the tile index", W.pill_at(world, 40, 40) == p)
  check("armour and owner updated", p and p.health == 12 and p.owner == "hostile")
  check("last_seen not moved (no sighting)", p and p.last_seen == 100, p and p.last_seen)
  -- A different square in the update is not news either: keep ours.
  events(world, 160, pill_ev(5, 44, 44, 7, 0, 11))
  check("sent square ignored without 0x20", p.mx == 40 and p.my == 40 and W.pill_at(world, 40, 40) == p)
end

print("new pill first heard of without a square")
do
  local world = new_world()
  events(world, 500, pill_ev(9, 60, 60, 7, 0, 15))
  local u = world.pills_unk[9]
  check("held without a square", world.pills[9] == nil and u and u.pos_unknown)
  check("never located (tick 0)", u and u.pos_unknown_tick == 0)
  check("not on the tile index", W.pill_at(world, 60, 60) == nil)
  check("owner kept", u and u.owner == "hostile")
  -- Any ally sighting can fill a pill we never had a square for.
  W.sync_ally_world(world, {
    { kind = "p", id = 9, cls = "h", tick = 450, mx = 61, my = 62, hp = 15, intank = 0, from = ALLY },
  }, 510, ME)
  check("older ally sighting fills it", world.pills[9] and world.pills[9].mx == 61
        and W.pill_at(world, 61, 62) == world.pills[9])
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
