-- shell_and_take_neutral_pillbox.lua - Comes ashore, drives east until the
-- pillbox in the road stops it, flattens it and drives over it to carry it
-- off.
--
-- Written for Pill Yard: the tank spawns in a boat at (116,126) facing
-- east, road runs east from square 120 along row 126, and a neutral pillbox
-- on one armour sits on road square 134. A pill with armour left is solid
-- and shoots at anything that is not its own, so the run is shot at on the
-- way in and stopped a square short; one shell flattens it, and a pill on
-- no armour is neither solid nor a shooter but something a tank picks up by
-- driving over it, which takes it off the player who had it.
--
-- The tank drives into the pill, holds the throttle against it and holds
-- the trigger as well, then brakes once it is inside the pill's square; three
-- road squares past it leave room to stop. The trigger is held rather than
-- tapped because a tap is dropped unless it lands on a tick the tank can
-- fire on. Every step is keyed on the tank's position or speed, never on
-- the tick.

local brain = {}

local PILL_X = 134                       -- the neutral pillbox in the road

local phase = "run"                      -- run, shell, brake, parked

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }
  if info.dead then
    phase = "parked"
    return out
  end

  if phase == "run" then
    -- Square 133 is as far as a live pill lets the tank get.
    if info.tankx >= (PILL_X - 1) * 256 then
      phase = "shell"
    else
      out.holdkeys = KEY_FASTER
      return out
    end
  end

  if phase == "shell" then
    if info.tankx >= PILL_X * 256 then
      phase = "brake"
    else
      out.holdkeys = KEY_FASTER + KEY_SHOOT
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
