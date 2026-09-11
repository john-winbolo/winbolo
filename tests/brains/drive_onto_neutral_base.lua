-- drive_onto_neutral_base.lua - Comes ashore, drives east onto the first
-- base on the road and parks on it.
--
-- Written for Base Yard: the tank spawns in a boat at (116,126) facing
-- east, road runs east from square 120 along row 126, and a neutral base
-- with full stocks sits on road square 124. A neutral base is never solid,
-- so the tank drives straight onto it and takes it by arriving; nothing
-- has to be shot first.
--
-- Braking begins two squares short of the middle of the target square,
-- which from full road speed leaves the tank at rest a little past that
-- middle and well inside the square. Every step is keyed on the tank's
-- position or speed, never on the tick.

local brain = {}

local TARGET_X = 124                     -- the base to park on
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
