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
--   3. THE WATCH.  The three facts the old python driver read out of
--      drowning_A_trace.log are watched here instead: the terrain under the
--      tank, whether it died, and who owns the base.  The scenario sandbox has
--      no io, so the trace file itself is gone -- but it was only ever a way of
--      asking the ENGINE rather than the brain, and reading game.map_tile under
--      the tank from on_tick asks exactly the same question.
--
--      DEEP_SEA under the tank is the drowning, read from the ground rather
--      than inferred.  The base owner is 255 while the base is still neutral
--      and 0 once our bot has captured it, which is the "did it get there"
--      half.

local SPAWN = { 131, 120 }
local GRASS = 7
local DEEP_SEA = 0xFF
local FILL_TICK = 80             -- earliest tick the pond may be filled
                                 -- (doubled for the 100/s clock)
-- The start square HAS to be deep sea (starts.c startsIsValidSquare) and the
-- tank sits on it, afloat, until the pond is filled, so wet tiles before this
-- are the spawn pond and not a drowning.  The driver's own 400 sim ticks,
-- doubled for the 100/s clock.
local DRY_FROM = 800
local p0 = 0

local filled = false
local wet_at = nil               -- first tick the tank stood on deep sea
local died = false

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
      "DROWNING_A filled the spawn pond at (%d,%d) with grass at t=%d",
      SPAWN[1], SPAWN[2], tick))
  end

  -- Both watched only from the moment the pond is gone: until then the tank is
  -- sitting afloat on a deep-sea start square, which is neither a drowning nor
  -- a death.
  if filled then
    if tk.dead then died = true end
    if not wet_at and tick > DRY_FROM and not tk.dead
       and g.map_tile(tk.mx, tk.my) == DEEP_SEA then
      wet_at = tick
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/drowning_test.py check_A, which ran this
-- arena twice -- once on the defaults and once on preset=keel -- and made
-- five assertions about each run.  Three of them are world state and are
-- this verdict: the tank never stood on a deep-sea tile, it never died, and
-- it captured the base (a tank that refuses to move also never drowns, so
-- staying dry only counts if it got there).
--
-- The other two did not move.  They are counts of steering.lua's own
-- CLIFF_BRAKE corner=true and CLIFF_STOP_MASK lines in print2_bot0.log,
-- which is what tied the result to C.CLIFF_RAY_CORNER_CHECK and
-- C.CLIFF_STOP_MASK_ALL_GOALS rather than to the arena -- and with them goes
-- the preset=keel control, which needed a second run with a -bot-init token
-- the gate does not pass.  So this arena now says the ground is survivable
-- and says nothing about either knob.
--
-- GATE: ticks=16000 bots=1 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function(g)
  if wet_at then
    return false, string.format("tank stood on deep sea at t=%d", wet_at)
  end
  if died then return false, "tank died" end
  local b = g.base(1)
  if not b or b.owner ~= p0 then
    return false, "base never captured -- staying dry only counts if it got there"
  end
  return true, "dry, alive, base taken"
end
