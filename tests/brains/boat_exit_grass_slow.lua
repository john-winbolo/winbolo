-- boat_exit_grass_slow.lua - Turns north out of the start and drives the
-- bank lane at half the boat's top speed, which is not enough to get out
-- of the water onto the grass.
--
-- Written for Boat Bank: the tank spawns in a boat at (120,130) facing
-- east, river runs north up column 120 on rows 126..129 and grass covers
-- rows 124..125. Below the boat's top speed a boat is held a quarter
-- square short of soft ground, so the tank ends the run afloat on row
-- 126, pressed against the bank at the speed it was still carrying, with
-- the accelerator untouched since it reached half speed. Every step is
-- keyed on the tank's heading and speed, never on the tick.

local brain = {}

local HALF_SPEED = 32         -- half of the boat's top speed of 64

local phase = "turn"          -- turn, accel, coast, done

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }
  if info.dead then
    phase = "done"
    return out
  end

  if phase == "turn" then
    -- Facing east is heading 64; turn left towards 0 (north). Heading is
    -- quantised to sixteen directions for movement, sixteen apart, so
    -- anything within half of one (7 or less) moves due north. Keep turning
    -- until then; one think per frame leaves almost no coast after release.
    if info.direction <= 7 or info.direction >= 128 then
      phase = "accel"
    else
      out.holdkeys = KEY_TURNLEFT
      return out
    end
  end

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

  -- coast: pressing nothing holds the speed, so the tank runs at the bank
  -- at half speed and stays there.
  return out
end

return brain
