-- shell_and_take_enemy_base.lua - Comes ashore, drives east until the base
-- at the end of the road stops it, shells it until the square opens, and
-- rolls on to take it.
--
-- Written for Base Yard: the tank spawns in a boat at (116,126) facing
-- east, road runs east from square 120 along row 126, neutral bases sit on
-- squares 124 and 130, the road is mined on square 127, and a base owned
-- by a player who is not in the game sits on square 134. The run takes the
-- two neutral bases in passing and loses ten armour to the mine on the way
-- through: a neutral base is never solid and costs nothing to take, which
-- is the opposite of what waits at the end.
--
-- An enemy base is solid until its armour is down to MIN_ARMOUR_CAPTURE,
-- so the tank drives into it, holds the throttle against it and holds the
-- trigger as well. Each shell takes DAMAGE off, and the square opens under
-- the third; the tank is already leaning on it, so it rolls straight in
-- and the brain then brakes. The trigger is held rather than tapped
-- because a tap is dropped unless it lands on a tick the tank can fire on.
-- Every step is keyed on the tank's position or speed, never on the tick.

local brain = {}

local BASE_X = 134                       -- the enemy base at the end

local phase = "run"                      -- run, shell, brake, parked

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }
  if info.dead then
    phase = "parked"
    return out
  end

  if phase == "run" then
    -- Square 134 is as far as a solid base lets the tank get.
    if info.tankx >= (BASE_X - 1) * 256 then
      phase = "shell"
    else
      out.holdkeys = KEY_FASTER
      return out
    end
  end

  if phase == "shell" then
    if info.tankx >= BASE_X * 256 then
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
