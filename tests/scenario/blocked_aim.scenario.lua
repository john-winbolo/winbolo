-- Scenario script for tests/blocked_aim.map (auto-loaded as <map>.scenario.lua
-- by scenarioLoad — there is no command-line flag; it just has to sit beside
-- the .map).
--
-- The blocker has to be PERMANENT.  It is one of OUR OWN pills sitting one
-- tile diagonally SE of the take, purely so it stands in the shot line; if the
-- bot can reach it, it drives over it / repositions it and the shot line stops
-- being blocked long before the take (measured: gone by tick 1249, and the
-- test then passed on the pre-fix brain too).
--
-- Deep sea under the pill fixes that: a tank cannot drive onto deep sea, an
-- LGM cannot walk to it, and there is no boat — the map's only water is the
-- land-locked single-tile spawn pocket (the spawn boat beaches on the first
-- move) and there is no RIVER anywhere to build a new one.  The pill stays
-- deployed, alive and in the way for the whole run.
--
-- The map loader is what makes the script necessary: bolo_map.c replaces
-- RIVER/DEEP_SEA/BUILDING/HALFBUILDING under every map-file pillbox with ROAD,
-- so terrain written into the .map alone cannot do it.  Restore the sea in
-- on_setup, which runs before the first snapshot — same trick as
-- tests/water_pills.scenario.lua.
--
-- on_setup also pins the ownership the test asserts on, in case the map-file
-- owner bytes ever stop surviving the load: pill 1 (the TARGET) neutral so the
-- bot may attack it, pill 2+ (the BLOCKERs) ours so the bot never wants to.
-- Pills are 1-based in the scenario API and follow map-file order.

local T_DEEP_SEA = 0xFF
local OUR_PLAYER = 0

function on_setup(game)
  -- Pill 1 is the target: neutral, so it is hostile to the bot.
  if game.num_pills() >= 1 then
    game.set_pill_owner(1, nil)
  end
  -- Pills 2.. are ours, and each gets deep sea back underneath it.
  for i = 2, game.num_pills() do
    game.set_pill_owner(i, OUR_PLAYER)
    local p = game.pill(i)
    if p then game.set_tile(p.x, p.y, T_DEEP_SEA) end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/blocked_aim_test.py, PASS condition 1: the
-- target pill ends up dead or ours. The driver read it off the -snapjson
-- stream rather than off the final state alone, because a captured pill that
-- is later dropped stops reading as ours -- so it is latched here on the tick
-- it first becomes true.
--
-- LEFT BEHIND, because it is print2 and nothing else: conditions 2 and 3 --
-- no CHARGE_ABORT_OBSTACLE / SHOOT_PILL_ABORT_OBSTACLE ("shot path blocked")
-- clear, and no SANITY_ABANDON or BLITZ_GO_ABANDON anywhere. Those are what
-- made this arena DISCRIMINATE: the baseline brain also never took the pill,
-- so condition 1 alone still separates fixed from baseline here, but a brain
-- that took the pill the slow way by abandoning and re-planning would now
-- pass where the driver failed it.
--
-- GATE: ticks=10000 bots=1 ai=yesfull gametype=open

local TARGET = 1
local taken_at = nil

function on_tick(game, tick)
  if taken_at == nil then
    local p = game.pill(TARGET)
    if p and (p.armour == 0 or p.owner == 0) then taken_at = tick end
  end
end

VERDICT_CHECK = function(g)
  if taken_at then
    return true, string.format("target pill dead or ours at tick %d", taken_at)
  end
  local p = g.pill(TARGET)
  return false, string.format("target pill still standing, armour %s owner %s",
                              p and tostring(p.armour) or "gone",
                              p and tostring(p.owner) or "-")
end
