-- refuel_on_neutral_base.lua - Comes ashore, drives east over the mined
-- road square and parks on the second base on the road to be restocked.
--
-- Written for Base Yard: the tank spawns in a boat at (116,126) facing
-- east, road runs east from square 120 along row 126, a neutral base sits
-- on square 124, the road is mined on square 127, and the base the run is
-- after — full armour, three shells, full mines — sits on square 130. The
-- run passes over the first base and takes it on the way; the mine takes
-- ten armour off the tank, which is what gives the base armour to hand
-- back.
--
-- Braking begins two squares short of the middle of the target square,
-- which from full road speed leaves the tank at rest a little past that
-- middle and well inside the square. Every step is keyed on the tank's
-- position or speed, never on the tick.

local brain = {}

local TARGET_X = 130                     -- the base to park on
local BRAKE_LEAD = 2 * 256               -- braking distance from full speed

local brake_at = TARGET_X * 256 + 128 - BRAKE_LEAD
local phase = "drive"                    -- drive, brake, parked

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }
  if info.dead then
    phase = "parked"
    return out
  end

  if phase == "drive" then
    if info.tankx >= brake_at then
      phase = "brake"
    else
      out.holdkeys = KEY_FASTER
      return out
    end
  end

  if phase == "brake" then
    if info.speed == 0 then
      phase = "parked"
    else
      out.holdkeys = KEY_SLOWER
    end
  end

  return out
end

return brain
