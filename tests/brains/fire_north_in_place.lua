-- fire_north_in_place.lua — probe brain for the respawn-loadout test.
--
-- The test needs a tank that SPENDS its ammunition and then dies, over and
-- over, without ever moving and without ever damaging the pillboxes that are
-- killing it. So this brain:
--
--   * never accelerates — the tank stays on its start tile, which on the
--     RespawnLoadoutTest map is inside the pillbox ring, so every life ends
--     the same way;
--   * turns to face due NORTH and holds there. The map deliberately places
--     its pillboxes in a west column and an east column, four tiles either
--     side, so a shell fired north flies up an empty lane and out over the
--     deep sea. The pillboxes are never damaged and the trap keeps working
--     for the whole run;
--   * fires as fast as the reload allows once it is aimed, so the tank is
--     visibly out of shells (or well down) by the time it dies. That is what
--     makes the assertion meaningful: an inert tank still holds 40 shells at
--     the moment it respawns whether or not the engine refuelled it, whereas
--     a tank that emptied its magazine can only read 40 again if the respawn
--     path actually handed it a fresh open-mode loadout.
--
-- Bolo angle convention: 0 = North, 64 = East, 128 = South, 192 = West;
-- KEY_TURNRIGHT increases the angle, KEY_TURNLEFT decreases it.

local brain = {}

local NORTH = 0
local TOL = 3        -- bradians; well inside the 4-tile lateral clearance

function brain.open(info)
end

function brain.think(info)
  local diff = (info.direction - NORTH) % 256
  if diff > 128 then diff = diff - 256 end

  if diff > TOL then
    return { holdkeys = KEY_TURNLEFT, tapkeys = 0 }
  elseif diff < -TOL then
    return { holdkeys = KEY_TURNRIGHT, tapkeys = 0 }
  end
  -- Aimed north: burn the magazine. Hold the fire key rather than tapping it,
  -- so the tank fires on every tick the reload allows instead of once per
  -- brain think.
  return { holdkeys = KEY_SHOOT, tapkeys = KEY_SHOOT }
end

function brain.close(info)
end

return brain
