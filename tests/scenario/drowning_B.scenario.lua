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
-- The pond, the boat and the watch work exactly as in drowning_A.scenario.lua;
-- its header explains all three.

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
local FILL_TICK = 80              -- doubled for the 100/s clock
-- The tank sits afloat on its deep-sea start square until the pond is filled,
-- so wet tiles before this are the pond and not a drowning.  The driver's own
-- 300 sim ticks, doubled.
local DRY_FROM = 600
local p0 = 0

local filled = false
local cut_tick = nil
local last_wx = nil
local wet_at = nil
local died = false
local captured_at = nil

local function on_main_row(my)
  for _, y in ipairs(MAIN_ROWS) do
    if my == y then return true end
  end
  return false
end

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
  end

  -- What the trace was for.  Watched only once the pond is gone: until then
  -- the tank sits afloat on a deep-sea start square, which is neither a
  -- drowning nor a death.
  if filled then
    if tk.dead then died = true end
    if not wet_at and tick > DRY_FROM and not tk.dead
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
-- PORTED (2026-09-15) from tests/drowning_test.py check_B, and all of it
-- came: arena B is the DEADLOCK guard and every one of its assertions is
-- world state.  The notch was cut, the tank never stood on deep sea, it
-- never died, and it captured the base AFTER the cut -- that last clause is
-- the whole arena.  A rule that strips KEY_FASTER at a shoreline could pin a
-- tank in front of the water forever; what this says is that it turns round,
-- walks the forty tiles of ring and gets there anyway.
--
-- The driver ran this twice, once on preset=keel, and printed the two side
-- by side.  That control needed a -bot-init token the gate does not pass, so
-- only the defaults run here -- and the driver's own docstring says the two
-- configurations left byte-identical traces on this arena, so the control
-- was never what arena B proved.
--
-- GATE: ticks=10000 bots=1 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function()
  if not cut_tick then
    return false, "the notch was never cut -- the tank never approached slowly"
  end
  if wet_at then
    return false, string.format("tank stood on deep sea at t=%d", wet_at)
  end
  if died then return false, "tank died" end
  if not captured_at or captured_at <= cut_tick then
    return false, string.format("base not captured after the cut (cut=%d)", cut_tick)
  end
  return true, string.format("cut t=%d, dry, base taken t=%d", cut_tick, captured_at)
end
