-- Scenario sidecar for tests/drowning_B.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/drowning_test.py arena B.
--
-- ARENA B -- WATER IN FRONT OF THE NOSE, THE ONLY WAY OUT BEHIND.  A three-row
-- grass corridor running east to a NEUTRAL BASE, and a long way round -- a
-- second corridor far to the north, joined at both ends -- that the tank has no
-- reason to use.  The tank drives east down the main corridor at its goal; when
-- its centre is CUT_MARGIN_WU short of the next column this sidecar turns that
-- WHOLE column into deep sea, two tiles short of the base.  The tank is then
-- nose-on to open water with its goal on the far side of it and its only
-- remaining route ~40 tiles BACKWARDS round the ring.
--
-- THIS IS THE DEADLOCK GUARD, NOT A DROWNING CONTROL.  C.CLIFF_STOP_MASK_ALL_
-- GOALS strips KEY_FASTER whenever deep sea is one tile ahead, and the obvious
-- way for a rule like that to go wrong is a tank pinned at a shoreline forever,
-- never able to accelerate and never getting round.  This arena is exactly that
-- shape, and what it asserts is that the tank turns round, walks the forty
-- tiles and captures the base anyway.  It does NOT drown a preset=keel tank --
-- measured, no warning distance separates the knobs here; tests/drowning_test.py
-- says so at length and the CUT_MARGIN_WU note below carries the sweep.
--
-- WHY THE COLUMN IS CUT AT RUN TIME.  The Dijkstra navigator plans the whole
-- route.  A gap written into the .map is one the tank routes around from the
-- far end of the corridor: it would take the long way from the start, having
-- never faced the water.  Cutting it under the tank's nose is the only way to
-- get the approach the incident had -- the scenario API has no hook that places
-- a tank (src/server/scenario.c scBuildGameTable), so the drive itself has to
-- produce it.
--
-- WHY THE WHOLE COLUMN.  An earlier version of this arena cut ONE tile and left
-- the diagonal step past it open.  The tank took the diagonal at speed, the
-- evasive turn carried it round, and nothing was proved either way.  Cutting
-- the column removes the sidestep and leaves only the u-turn.
--
-- The pond, the boat and the trace work exactly as in drowning_A.scenario.lua;
-- its header explains all three.  The columns are
--
--     tick wx wy dir mx my terrain dead base_owner notch_cut

local TRACE = "drowning_B_trace.log"
local SPAWN = { 118, 131 }
local NOTCH_X = 131               -- every MAIN_ROWS tile in this column is cut
local TRIGGER_X = 130             -- ...once the tank's own column is this
local MAIN_ROWS = { 130, 131, 132 }
-- HOW CLOSE THE WATER OPENS.  Cutting the column the moment the tank entered
-- the trigger TILE gave it 250 world units of warning and it simply turned
-- round, with the default and preset=keel leaving byte-identical traces.  So
-- the cut waits until the tank's centre is within CUT_MARGIN_WU of the
-- boundary.  MEASURED, on this arena, at seed 1 (the sweep that set this
-- number):
--     160 wu  -- inside the stopping distance of a tank cruising grass at
--                speed 48 (about 5 wu a sim tick).  BOTH the default and
--                preset=keel slid in and drowned: the arena was measuring
--                momentum, not the knobs.
--     192 wu  -- both stop, both turn round, both take the ring.  Identical
--                traces again.
--     224 wu  -- the same, with more room.
-- There is no warning distance in between at which the knobs decide the run,
-- which is why this arena is the DEADLOCK guard and not a drowning control
-- (tests/drowning_test.py says so at length).  192 is kept: it is the smallest
-- warning at which the tank is genuinely stopped by the water rather than
-- carried through it, which is the state the mask exists for.
local CUT_MARGIN_WU = 192
-- ...and only while it is moving slowly enough for that to be true. on_tick
-- runs once per BRAIN tick (every second sim tick), so this is world units per
-- two sim ticks and a cruising tank measures 8-12.  The sidecar has no speed
-- field to read -- l_tank does not expose one -- so it is taken from the
-- position delta.
local MAX_APPROACH_WU = 14        -- world units per BRAIN tick (two sim ticks)
local GRASS = 7
local DEEP_SEA = 0xFF             -- global.h; game.set_tile accepts it
local SAMPLE_EVERY = 50
local FILL_TICK = 40
local p0 = 0

local filled = false
local cut_tick = nil
local last = nil
local last_row_tick = -1000
local last_wx = nil

local function on_main_row(my)
  for _, y in ipairs(MAIN_ROWS) do
    if my == y then return true end
  end
  return false
end

local function trace_row(g, tick, tk)
  local terr = g.map_tile(tk.mx, tk.my) or -1
  local b = g.base(1)
  local f = io.open(TRACE, "a")
  if f then
    f:write(string.format("%d %d %d %d %d %d %d %d %d %d\n",
                          tick, tk.wx, tk.wy, tk.dir, tk.mx, tk.my, terr,
                          tk.dead and 1 or 0, b and b.owner or -1,
                          cut_tick and 1 or 0))
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
    f:write("# tick wx wy dir mx my terrain dead base_owner notch_cut\n")
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

  if not filled and tick >= FILL_TICK and not tk.boat and not tk.dead
     and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
    filled = true
    g.set_tile(SPAWN[1], SPAWN[2], GRASS)
    g.message(string.format(
      "DROWNING_B filled the spawn pond at (%d,%d) with grass at t=%d",
      SPAWN[1], SPAWN[2], tick))
  end

  -- The notch.  Cut once, only while the tank is on foot in the main corridor,
  -- one column short of it, within CUT_MARGIN_WU of the boundary and slow
  -- enough to stop in that -- so the water opens right in front of its nose.
  local step = last_wx and math.abs(tk.wx - last_wx) or 0
  last_wx = tk.wx
  if not cut_tick and filled and not tk.boat and not tk.dead
     and tk.mx == TRIGGER_X and on_main_row(tk.my)
     and tk.wx >= NOTCH_X * 256 - CUT_MARGIN_WU
     and step <= MAX_APPROACH_WU then
    cut_tick = tick
    for _, y in ipairs(MAIN_ROWS) do
      g.set_tile(NOTCH_X, y, DEEP_SEA)
    end
    g.message(string.format(
      "DROWNING_B cut column %d (rows %d-%d) to deep sea at t=%d "
      .. "(tank at (%d,%d) wx=%d dir=%d gap=%d wu step=%d wu)",
      NOTCH_X, MAIN_ROWS[1], MAIN_ROWS[#MAIN_ROWS], tick,
      tk.mx, tk.my, tk.wx, tk.dir, NOTCH_X * 256 - tk.wx, step))
    trace_row(g, tick, tk)
  end

  -- Every tick while the tank is in the trigger column, so a run where the cut
  -- never armed shows exactly which of the three conditions was missing.
  local in_window = (tk.mx == TRIGGER_X and on_main_row(tk.my))
  local sig = tk.mx .. "/" .. tk.my .. "/" .. (tk.dead and 1 or 0)
  if in_window or sig ~= last or (tick - last_row_tick) >= SAMPLE_EVERY then
    last = sig
    trace_row(g, tick, tk)
  end
end
