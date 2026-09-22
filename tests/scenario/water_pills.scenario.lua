-- Scenario script for tests/water_pills.map (auto-loaded as <map>.scenario.lua).
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

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/water_pills_test.py, which read the same
-- thing out of the final-state JSON: the bot uses the boat it spawned in to
-- collect BOTH dead pills, so at the deadline each is riding in a tank and
-- owned by player 0.
--
-- GATE: ticks=24000 bots=1 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  local got = 0
  for n = 1, 2 do
    local pi = g.pill(n)
    if pi and pi.in_tank and pi.owner == 0 then got = got + 1 end
  end
  if got == 2 then
    return true, "both sea pills collected"
  end
  return false, string.format("only %d of 2 sea pills collected", got)
end
