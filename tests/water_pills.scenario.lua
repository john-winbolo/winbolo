-- Scenario sidecar for tests/water_pills.map (auto-loaded as <map>.scenario.lua).
--
-- The map loader "fixes" the terrain under every map-file pillbox to ROAD
-- (bolo_map.c: RIVER/DEEP_SEA/BUILDING/HALFBUILDING under a pill -> ROAD). So
-- the two dead pills this arena places in deep sea would really sit on 1-tile
-- road pedestals, and a boat driving onto one BEACHES (loses the boat) and
-- then drowns stepping off — which is not the field case: pills dropped at
-- sea by dying tanks lie on genuine deep sea and a boat picks them up while
-- still afloat. Restore deep sea under both pills before the first snapshot
-- so the test exercises the real situation.

local T_DEEP_SEA = 0xFF

local PILLS = { { 126, 136 }, { 128, 136 } }   -- must match generate_water_pills_map.py PILLS

function on_setup(game)
  for _, p in ipairs(PILLS) do
    game.set_tile(p[1], p[2], T_DEEP_SEA)
  end
end
