-- boat_exit_road_fast.lua - Runs the ford lane east at full boat speed,
-- crossing the water/road boundary three times without ever lifting off
-- the accelerator.
--
-- Written for Boat Bank: the tank spawns in a boat at (120,130) facing
-- east, river runs east along row 130 on squares 121..124, road on
-- 125..127, a parked boat sits on square 128 and road runs on to 133.
-- Held at full throttle the tank is at the boat's top speed well before
-- the first road square, comes ashore there, picks the parked boat up and
-- is put ashore again on the square past it. It brakes on the third
-- change so it stops on the road instead of running out into the deep sea
-- past square 133. Every step is keyed on the tank's boat state and
-- speed, never on the tick.

local brain = {}

local shifts = 0              -- boat-state changes seen so far
local was_in_boat = nil       -- boat state on the previous think

local phase = "run"           -- run, brake, done

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

  if phase == "run" then
    -- 1: ashore on road square 125. 2: aboard the parked boat on square
    -- 128. 3: ashore again on road square 129.
    if shifts >= 3 then
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
