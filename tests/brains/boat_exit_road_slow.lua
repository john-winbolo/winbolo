-- boat_exit_road_slow.lua - Runs the ford lane east at half the boat's
-- top speed, which is not enough to get out of the water onto the road.
--
-- Written for Boat Bank: the tank spawns in a boat at (120,130) facing
-- east, river runs east along row 130 on squares 121..124, road on
-- 125..127, a parked boat sits on square 128 and road runs on to 133.
-- The tank accelerates until it is at half speed and then coasts, which
-- leaves it well below the speed a boat needs to drive itself ashore.
-- Road takes that speed like any other land (as in the original WinBolo,
-- WinBolo 1.17 and Mac Bolo), so the boat is held a quarter square short
-- of road square 125 and the tank ends the run afloat on square 124. The
-- brain still counts boat changes and would go on to the parked boat and
-- brake on the road past it, so a run that came ashore at this speed
-- again would show up as a difference from the expected output. Every step
-- is keyed on the tank's speed and boat state, never on the tick.

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
    -- At half speed the tank never gets past the first of these, so it
    -- coasts against the bank for the rest of the run. Were it let ashore:
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
