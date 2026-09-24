-- boat_exit_grass_fast.lua - Turns north out of the start and drives the
-- bank lane at full boat speed, up onto the grass headland and back into
-- the water on the far side of it.
--
-- Written for Boat Bank: the tank spawns in a boat at (120,130) facing
-- east, river runs north up column 120 on rows 126..129, grass covers
-- rows 124..125 and a second parked boat sits on (120,123). Grass is soft
-- ground, which a boat only drives out onto at the boat's top speed; four
-- river rows is more than the two the tank needs to get there. Every step
-- is keyed on the tank's heading, boat state and speed, never on the tick.

local brain = {}

local shifts = 0              -- boat-state changes seen so far
local was_in_boat = nil       -- boat state on the previous think

local phase = "turn"          -- turn, run, brake, done

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

  if phase == "turn" then
    -- Facing east is heading 64; turn left towards 0 (north). Heading is
    -- quantised to sixteen directions for movement, sixteen apart, so
    -- anything within half of one (7 or less) moves due north. Keep turning
    -- until then; one think per frame leaves almost no coast after release.
    if info.direction <= 7 or info.direction >= 128 then
      phase = "run"
    else
      out.holdkeys = KEY_TURNLEFT
      return out
    end
  end

  if phase == "run" then
    -- 1: ashore on grass row 125. 2: aboard the parked boat on (120,123).
    if shifts >= 2 then
      phase = "brake"
    else
      out.holdkeys = KEY_FASTER
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
