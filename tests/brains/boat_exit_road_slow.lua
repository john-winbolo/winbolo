-- boat_exit_road_slow.lua - Runs the ford lane east at half the boat's
-- top speed, crossing three water/road boundaries on the way.
--
-- Written for Boat Bank: the tank spawns in a boat at (120,130) facing
-- east, river runs east along row 130 on squares 121..124, road on
-- 125..127, a parked boat sits on square 128 and road runs on to 133.
-- The tank accelerates until it is at half speed and then coasts, which
-- leaves it well below the speed a boat needs to drive itself ashore;
-- road is a hard surface, so it lands on square 125 anyway. Every step is
-- keyed on the tank's speed and boat state, never on the tick.

local brain = {}

local HALF_SPEED = 32         -- half of the boat's top speed of 64

local shifts = 0              -- boat-state changes seen so far
local was_in_boat = nil       -- boat state on the previous think

local phase = "accel"         -- accel, coast, brake, done

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }
  if info.dead then
    phase = "done"
    return out
  end

  if was_in_boat ~= nil and info.inboat ~= was_in_boat then
    shifts = shifts + 1
  end
  was_in_boat = info.inboat

  if phase == "accel" then
    -- Releasing the key takes a step to land, so the coast settles a
    -- notch above the half-speed mark rather than on it.
    if info.speed >= HALF_SPEED then
      phase = "coast"
    else
      out.holdkeys = KEY_FASTER
      return out
    end
  end

  if phase == "coast" then
    -- Pressing nothing holds the speed: the tank only slows for terrain
    -- it cannot go that fast over, and river, road and boat all allow
    -- more than half speed.
    -- 1: ashore on road square 125. 2: aboard the parked boat on square
    -- 128. 3: ashore again on road square 129.
    if shifts >= 3 then
      phase = "brake"
    else
      return out
    end
  end

  if phase == "brake" then
    if info.speed == 0 then
      phase = "done"
    else
      out.holdkeys = KEY_SLOWER
    end
  end
  return out
end

return brain
