-- park_in_pill_range.lua - Comes ashore and parks on the road inside a
-- pillbox's reach, then does nothing for the rest of the run.
--
-- Written for Road Spit Minefield: the tank spawns in a boat at (116,124)
-- facing east, the road runs east from square 120 along row 124, and the
-- neutral pillbox at (131,121) reaches road squares 124..138. Every step
-- is keyed on the tank's map square, so the same sequence plays out under
-- any game type. Each hit shoves the parked tank south onto the grass pad
-- behind road square 131; the brain does nothing about that.

local brain = {}

local phase = "drive"         -- drive, brake, parked

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }
  if info.dead then
    -- Stay put in the boat after the respawn too.
    phase = "parked"
    return out
  end
  local tx = math.floor(info.tankx / 256)

  -- Aiming for road square 131: hold accelerate until entering square 129,
  -- then brake. A tank under a brain does not slow on its own, and braking
  -- from full speed takes a little over two squares.
  if phase == "drive" then
    if tx >= 129 then
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
