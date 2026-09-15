-- Scenario sidecar for tests/drowning_fix_A4.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/drowning_fix_test.py arena A4.
--
-- ARENA A4 -- THE BOAT-SEEDED LEDGE.  A one-tile-wide grass ledge running south
-- from the spawn block to the base, deep sea on BOTH flanks, and a line of
-- BOAT tiles in the water cardinally adjacent to it.  Under test is
-- C.PF_NEXTSTEP_FOOT_SEA_RULE: brainPathfinderDijkstraNextStep's 8-neighbour
-- fallbacks picked a neighbour by min(g_land, g_boat) with no on-foot
-- passability test, so a boatless tank could be handed a deep-sea tile.
--
-- Two things the ground has to provide, and this file does not:
--   * the BOAT tiles, which are what make the boat layer reachable at all --
--     deep sea is a wall on the LAND layer, so without them every sea tile
--     carries g = COST_INF and the fallback skips it before any rule applies;
--   * the one-tile ledge, so every 8-neighbour scan the tank makes has sea
--     tiles in it.
--
-- This file does four things and nothing else:
--
--   1. OWNERSHIP.  The base is made NEUTRAL, so capture_base is the only
--      reason to move and the ledge is the only way to it.
--
--   2. TWO ALLIES.  Both bots are put on team 0 and given a pond each.  The
--      obstacle veer -- one of the two fallbacks under test -- only fires when
--      an ALLY is standing on the chain's next tile, and a one-wide ledge with
--      two bots queueing down it is that, every few ticks.
--
--   3. THE PONDS.  A start square has to be DEEP SEA at map load (starts.c
--      startsIsValidSquare).  Both are buried in the spawn block and filled
--      with GRASS once their tank is ashore, which destroys the boat left
--      behind.  There is no RIVER on the map; the BOAT tiles beside the ledge
--      are open water, not a river, so no new boat can be built.
--
--   4. THE WATCH.  The scenario sandbox has no io, so the trace file this
--      arena used to write is gone.  It was only ever a way of asking the
--      ENGINE rather than the brain, and reading game.map_tile under the tank
--      from on_tick asks the same question: DEEP_SEA under a boatless tank is
--      tank.c's own drowning condition.  The base owner is 255 while the base
--      is still neutral and 0 once our bot has taken it, which is the "did it
--      get there" half.

local PONDS = { { 124, 118 } }   -- generate_drowning_fix_map.py, recentred
local GRASS = 7
local DEEP_SEA = 0xFF
local FILL_TICK = 80             -- earliest tick a pond may be filled
                                 -- (doubled for the 100/s clock)
-- A start square HAS to be deep sea (starts.c startsIsValidSquare) and the
-- tank sits on it, afloat, until its pond is filled, so wet tiles before this
-- are the pond and not a drowning.
local DRY_FROM = 800
local p0 = 0

local filled = { false }
local wet_at = nil
local died = false
local captured_at = nil

function on_setup(g)
  for i = 1, g.num_bases() do
    g.set_base_owner(i, g.NEUTRAL)
  end
  g.set_team(0, 0)
end

function on_choose_start(g, p)
  if p == 0 then return 1 end

  return nil
end

function on_tick(g, tick)
  -- Fill each pond once ITS tank is ashore and off its boat.  Filling one
  -- while its tank still sits on it leaves the boat state stuck and that bot
  -- never becomes a land tank at all.
  if tick >= FILL_TICK then
    for i = 1, #PONDS do
      if not filled[i] then
        local tk = g.tank(i - 1)
        if tk and not tk.boat and not tk.dead
           and (tk.mx ~= PONDS[i][1] or tk.my ~= PONDS[i][2]) then
          filled[i] = true
          g.set_tile(PONDS[i][1], PONDS[i][2], GRASS)
          g.message(string.format(
            "DROWNING_FIX_A4 filled pond %d at (%d,%d) with grass at t=%d",
            i, PONDS[i][1], PONDS[i][2], tick))
        end
      end
    end
  end

  local tk = g.tank(p0)
  if not tk then return end

  -- What the trace was for.  Watched only once the pond is gone: until then
  -- the tank sits afloat on a deep-sea start square, which is neither a
  -- drowning nor a death.
  if filled[1] then
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
-- PORTED (2026-09-15) from tests/drowning_fix_test.py arena A4.  Three of
-- its assertions are world state and are this verdict: the tank never stood
-- on a deep-sea tile, it never died, and it still captured the base.  The
-- ledge is one tile wide with deep sea on both flanks and boat tiles seeded
-- in the water beside it, which is the ground a boatless tank used to be
-- handed a sea tile on, so "it walked the whole ledge and stayed dry" is a
-- real thing to have measured.
--
-- The assertion the arena was BUILT for did not move: that no
-- `nav: dij ... next=` line ever names a deep-sea tile while the tank is on
-- land, and the PF_SEA_VETO count beside it.  Both are print2_bot0.log,
-- which a scenario cannot read, and so is the preset=keel control the driver
-- ran beside every arena -- it needed a second run with a -bot-init token
-- the gate does not pass.  This arena now says the ground is survivable and
-- says nothing about C.PF_NEXTSTEP_FOOT_SEA_RULE.
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
