-- tutorial_shoot_north.lua -- probe brain for the Tutorial's 5B parking
-- square (tests/scenario/tutorial_park5b).
--
-- A player who does what the 5B status line says: stops on the parking
-- square and aims at the enemy pillbox, 4 squares west and 6 north of it
-- (the name is from when it was due north). The brain never accelerates. It
-- turns to face the target, AIM below, and raises the gun sight to its
-- longest range wherever it is, but it fires only while the tank is on the
-- parking square, PARK below. The arena keeps the tank off that square
-- until the player's blocker is built, then puts it there.
--
-- Bolo angle convention: 0 = North, 64 = East, 128 = South, 192 = West;
-- KEY_TURNRIGHT increases the angle, KEY_TURNLEFT decreases it.

local brain = {}

-- From the parking square's centre to the target's: (-4, -6) squares,
-- atan2(-4, 6) = -33.7 degrees from north, 232 of 256.
local AIM = 232
local TOL = 1                  -- bradians
local MAX_SIGHT = 14           -- half squares: the classic gunsight_max
local PARK = { 121, 107 }      -- LAYOUT.point.t5b_park in Tutorial.map

function brain.open(info)
end

function brain.think(info)
  local diff = (info.direction - AIM) % 256
  if diff > 128 then diff = diff - 256 end
  if diff > TOL then
    return { holdkeys = KEY_TURNLEFT, tapkeys = 0 }
  elseif diff < -TOL then
    return { holdkeys = KEY_TURNRIGHT, tapkeys = 0 }
  end
  if (info.gunrange or 0) < MAX_SIGHT then
    return { holdkeys = 0, tapkeys = KEY_MORERANGE }
  end
  local mx = math.floor((info.tankx or 0) / 256)
  local my = math.floor((info.tanky or 0) / 256)
  if mx ~= PARK[1] or my ~= PARK[2] then
    return { holdkeys = 0, tapkeys = 0 }
  end
  return { holdkeys = KEY_SHOOT, tapkeys = KEY_SHOOT }
end

function brain.close(info)
end

return brain
