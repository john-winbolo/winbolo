-- Scenario sidecar for tests/drowning_fix_A3.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/drowning_fix_test.py arena A3.
--
-- ARENA A3 -- THE NOTCH LEDGE.  Arena A's two-tile staircase descending
-- south-west (the shape that makes the cliff brake fire: sea due south, land
-- to the south-west, tank hugging a column boundary) ending in a ONE-TILE-WIDE
-- ledge with deep sea both north and south of every tile, the map's only base
-- at its tip.  Under test is C.CLIFF_BRAKE_STICKY: the guards are re-decided
-- every tick off a heading ray, so on the ticks the ray's jitter misses the sea
-- tile navigate re-issues KEY_FASTER and the tank creeps forward between
-- brakes (20260906_184924_1_drown13 bot0 t=29523).
--
-- This file does three things and nothing else:
--
--   1. OWNERSHIP.  The base is made NEUTRAL, so capture_base is the tank's one
--      and only reason to move, and the staircase plus the ledge is the only
--      way to it.  That base is also the DEADLOCK assertion: a sticky "no
--      forward throttle" latch that pins the tank on the ledge would show up
--      as a base that never changes hands.
--
--   2. THE POND.  A start square has to be DEEP SEA at map load (starts.c
--      startsIsValidSquare).  The pond is buried in the middle of a 5x5 block
--      so all eight neighbours are land, and it is filled with GRASS as soon
--      as the tank is ashore, which destroys the boat left on it.  There is no
--      RIVER on the map, so no replacement boat can ever be built.
--
--   3. THE TRACE.  drowning_fix_A3_trace.log, in SIM ticks, so "did it drown"
--      and "did it creep" are asked of the ENGINE and not of the brain's
--      opinion of itself.  One row per GAME tick (they are what the brain
--      thinks on), columns:
--
--          tick wx wy dir mx my terrain dead base_owner boat
--
--      terrain is game.map_tile under the TANK, so terrain=255 (DEEP_SEA) on
--      any row is the drowning itself, read from the ground.  wx/wy are world
--      units, which is what the creep is measured in: the test takes the
--      closest the tank's centre ever gets to the sea tile a brake named.

local TRACE = "drowning_fix_A3_trace.log"
local SPAWN = { 132, 122 }       -- generate_drowning_fix_map.py, recentred
local GRASS = 7
local FILL_TICK = 40             -- earliest tick the pond may be filled
local p0 = 0

local filled = false
local buf = {}
local nbuf = 0
local last_owner = -2

local function flush()
  if nbuf == 0 then return end
  local f = io.open(TRACE, "a")
  if f then
    f:write(table.concat(buf, "", 1, nbuf))
    f:close()
  end
  nbuf = 0
end

function on_setup(g)
  for i = 1, g.num_bases() do
    g.set_base_owner(i, g.NEUTRAL)
  end
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then
    f:write("# tick wx wy dir mx my terrain dead base_owner boat\n")
    f:close()
  end
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_tick(g, tick)
  local tk = g.tank(p0)
  if not tk then return end

  -- Fill the pond once the tank is ASHORE and off its boat.  Filling it while
  -- the tank still sits on it leaves the boat state stuck and the bot never
  -- becomes a land tank at all.
  if not filled and tick >= FILL_TICK and not tk.boat and not tk.dead
     and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
    filled = true
    g.set_tile(SPAWN[1], SPAWN[2], GRASS)
    g.message(string.format(
      "DROWNING_FIX_A3 filled the spawn pond at (%d,%d) with grass at t=%d",
      SPAWN[1], SPAWN[2], tick))
  end

  -- One row per GAME tick (sim ticks are 10 ms half-steps; the brain thinks
  -- once per 20 ms frame), buffered so 4000 rows is not 4000 file opens.
  -- There is no shutdown hook (scenario.c caches on_setup / on_tick /
  -- on_choose_start and nothing else), so anything still in the buffer when
  -- -ticks expires is lost.  The two facts the test cannot afford to lose --
  -- the tank died, the base changed hands -- force a flush of their own; the
  -- rest ride a small buffer.
  if (tick % 2) == 0 then
    local b = g.base(1)
    local owner = b and b.owner or -1
    nbuf = nbuf + 1
    buf[nbuf] = string.format("%d %d %d %d %d %d %d %d %d %d\n",
                              tick, tk.wx, tk.wy, tk.dir, tk.mx, tk.my,
                              g.map_tile(tk.mx, tk.my) or -1,
                              tk.dead and 1 or 0, owner,
                              tk.boat and 1 or 0)
    if nbuf >= 50 or tk.dead or owner ~= last_owner then flush() end
    last_owner = owner
  end
end
