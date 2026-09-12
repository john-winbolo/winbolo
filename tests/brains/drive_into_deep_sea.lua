-- drive_into_deep_sea.lua - Comes ashore, stops on the road, turns to face
-- the sea and drives straight into it.
--
-- Written for Road Spit Minefield: the tank spawns in a boat at (116,124)
-- facing east, the road runs east from square 120 along row 124, and the
-- square north of every road square is deep sea. The tank leaves its boat
-- at the bank, so driving north off road square 122 puts it in deep sea at
-- (122,123) with no boat under it. Every step is keyed on the tank's
-- square, speed or heading, never on the tick.

local brain = {}

local STOP_X = 122            -- road square to stop on

local phase = "ashore"        -- ashore, brake, turn, drive, done

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }
  if info.dead then
    phase = "done"
    return out
  end
  local tx = math.floor(info.tankx / 256)

  if phase == "ashore" then
    -- Aiming for road square 122: accelerate east until entering square 120,
    -- the first road square, then brake; from full speed the tank comes to
    -- rest a little over two squares on, in 122.
    if tx >= STOP_X - 2 then
      phase = "brake"
    else
      out.holdkeys = KEY_FASTER
      return out
    end
  end

  if phase == "brake" then
    if info.speed == 0 then
      phase = "turn"
    else
      out.holdkeys = KEY_SLOWER
      return out
    end
  end

  if phase == "turn" then
    -- Facing east is heading 64; turn left until heading 0 (north). A step
    -- past 0 wraps to the high end, which also counts as done.
    if info.direction == 0 or info.direction > 128 then
      phase = "drive"
    else
      out.holdkeys = KEY_TURNLEFT
      return out
    end
  end

  if phase == "drive" then
    -- Aiming for (122,123), the deep sea north of the road.
    out.holdkeys = KEY_FASTER
  end
  return out
end

return brain
