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
--   4. THE TRACE.  drowning_fix_A4_trace.log, in SIM ticks, for tank 0:
--
--          tick wx wy dir mx my terrain dead base_owner boat
--
--      terrain is game.map_tile under the TANK and boat is its onBoat flag, so
--      "terrain = 255 and boat = 0" is the engine's own drowning condition
--      (tank.c: onBoat == FALSE and mapGetPos == DEEP_SEA), read from the
--      ground rather than inferred.

local TRACE = "drowning_fix_A4_trace.log"
local PONDS = { { 124, 118 } }   -- generate_drowning_fix_map.py, recentred
local GRASS = 7
local FILL_TICK = 40             -- earliest tick a pond may be filled
local p0 = 0

local filled = { false }
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
  g.set_team(0, 0)

  local f = io.open(TRACE, "w")
  if f then
    f:write("# tick wx wy dir mx my terrain dead base_owner boat\n")
    f:close()
  end
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

  -- One row per GAME tick (sim ticks are 10 ms half-steps; the brain thinks
  -- once per 20 ms frame).  There is no shutdown hook (scenario.c caches
  -- on_setup / on_tick / on_choose_start and nothing else), so anything still
  -- buffered when -ticks expires is lost -- the two facts the test cannot
  -- afford to lose, the tank dying and the base changing hands, force a flush
  -- of their own.
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
