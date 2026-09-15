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
--   3. THE WATCH.  The scenario sandbox has no io, so the trace file this
--      arena used to write is gone.  It was only ever a way of asking the
--      ENGINE rather than the brain, and reading game.map_tile under the tank
--      from on_tick asks the same question: DEEP_SEA under a boatless tank is
--      tank.c's own drowning condition.  The base owner is 255 while the base
--      is still neutral and 0 once our bot has taken it, which is the "did it
--      get there" half.

local SPAWN = { 132, 122 }       -- generate_drowning_fix_map.py, recentred
local GRASS = 7
local DEEP_SEA = 0xFF
local FILL_TICK = 80             -- earliest tick the pond may be filled
                                 -- (doubled for the 100/s clock)
-- A start square HAS to be deep sea (starts.c startsIsValidSquare) and the
-- tank sits on it, afloat, until its pond is filled, so wet tiles before this
-- are the pond and not a drowning.
local DRY_FROM = 800
local p0 = 0

local filled = false
local wet_at = nil
local died = false
local captured_at = nil

function on_setup(g)
  for i = 1, g.num_bases() do
    g.set_base_owner(i, g.NEUTRAL)
  end
  g.set_team(p0, 0)
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

  -- What the trace was for.  Watched only once the pond is gone: until then
  -- the tank sits afloat on a deep-sea start square, which is neither a
  -- drowning nor a death.
  if filled then
    if tk.dead then died = true end
    if not wet_at and tick > DRY_FROM and not tk.dead and not tk.boat
       and g.map_tile(tk.mx, tk.my) == DEEP_SEA then
      wet_at = tick
    end
  end
  if not captured_at then
    local b = g.base(1)
    if b and b.owner == p0 then captured_at = tick end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/drowning_fix_test.py arena A3.  Three of
-- its assertions are world state and are this verdict: the tank never stood
-- on a deep-sea tile, it never died, and IT STILL CAPTURED THE BASE.  The
-- driver said the last one matters most for C.CLIFF_BRAKE_STICKY, because a
-- sticky "no forward throttle" latch is exactly the kind of rule that pins a
-- tank at a shoreline forever, and a ledge with water on both sides is the
-- shape it would pin on.
--
-- The rest did not move: the knob's own CLIFF_STICKY lines, the CLIFF_BRAKE
-- lines the creep distance was measured against, and the count of both that
-- separated the defaults from preset=keel.  All of that is print2_bot0.log,
-- which a scenario cannot read, and so is the preset=keel control the driver
-- ran beside every arena -- it needed a second run with a -bot-init token
-- the gate does not pass.  This arena now says the ground is survivable and
-- says nothing about the knob.
--
-- GATE: ticks=16000 bots=1 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function()
  if wet_at then
    return false, string.format("tank stood on deep sea at t=%d", wet_at)
  end
  if died then return false, "tank died" end
  if not captured_at then
    return false, "base never captured -- staying dry only counts if it got there"
  end
  return true, string.format("dry, alive, base taken t=%d", captured_at)
end
