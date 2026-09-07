-- Scenario sidecar for tests/drowning_A.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/drowning_test.py arena A.
--
-- ARENA A -- THE STAIRCASE SHORELINE.  A two-tile-wide staircase of grass
-- descending south-west through open deep sea, from the spawn block at the
-- north-east to a NEUTRAL BASE at the south-west.  Standing on the east tile of
-- any step is the 20260905_231835 bot3 drowning verbatim: deep sea directly
-- SOUTH, land to the SOUTH-WEST, and the tank hugging the column boundary on a
-- south-west heading, which is the geometry the brake ray corner-cuts.
--
-- This file does three things and nothing else:
--
--   1. OWNERSHIP.  The base is made NEUTRAL, so capture_base is the tank's one
--      and only reason to move and the staircase is the only way to it.
--
--   2. THE POND.  A start square has to be DEEP SEA at map load (starts.c
--      startsIsValidSquare).  The pond is buried in the middle of a 3x3 block
--      so all eight of its neighbours are land and the tank cannot sail out of
--      it -- and it is filled back in with GRASS the moment the tank is ashore,
--      which destroys the boat left on it.  There is no RIVER on the map, so
--      no replacement boat can ever be built.  Without both halves of that the
--      bot would simply sail to the base over the open sea and the staircase --
--      the entire arena -- would never be walked.
--
--   3. THE TRACE.  drowning_A_trace.log, in SIM ticks, so "did it drown" is
--      asked of the ENGINE and not of the brain's opinion of itself.  A row per
--      tile change (plus one every SAMPLE_EVERY ticks so a stationary tank still
--      leaves a pulse), and the columns are
--
--          tick wx wy dir mx my terrain dead base_owner
--
--      terrain is game.map_tile under the TANK, so terrain=255 (DEEP_SEA) on any
--      row is the drowning itself, recorded from the ground rather than
--      inferred.  base_owner is 255 while the base is still neutral and 0 once
--      our bot has captured it, which is the "did it get there" half.

local TRACE = "drowning_A_trace.log"
local SPAWN = { 131, 120 }
local GRASS = 7
local SAMPLE_EVERY = 50          -- sim ticks between keepalive rows
local FILL_TICK = 40             -- earliest tick the pond may be filled
local p0 = 0

local filled = false
local last = nil
local last_row_tick = -1000

local function trace_row(g, tick, tk)
  local terr = g.map_tile(tk.mx, tk.my) or -1
  local b = g.base(1)
  local f = io.open(TRACE, "a")
  if f then
    f:write(string.format("%d %d %d %d %d %d %d %d %d\n",
                          tick, tk.wx, tk.wy, tk.dir, tk.mx, tk.my, terr,
                          tk.dead and 1 or 0, b and b.owner or -1))
    f:close()
  end
  last_row_tick = tick
end

function on_setup(g)
  for i = 1, g.num_bases() do
    g.set_base_owner(i, g.NEUTRAL)
  end
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then
    f:write("# tick wx wy dir mx my terrain dead base_owner\n")
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
      "DROWNING_A filled the spawn pond at (%d,%d) with grass at t=%d",
      SPAWN[1], SPAWN[2], tick))
  end

  local sig = tk.mx .. "/" .. tk.my .. "/" .. (tk.dead and 1 or 0)
  if sig ~= last or (tick - last_row_tick) >= SAMPLE_EVERY then
    last = sig
    trace_row(g, tick, tk)
  end
end
