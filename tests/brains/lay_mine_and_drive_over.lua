-- lay_mine_and_drive_over.lua - Comes ashore, stops on the road, has the
-- man lay one mine on the swamp square beside it, waits for him to get
-- back in, then turns and drives over the mine into the minefield beyond.
--
-- Written for Road Spit Minefield: the tank spawns in a boat at (116,124)
-- facing east and the road runs east from square 120 along row 124. The
-- mined swamp patch fills squares 120..124 on rows 125..127; its top-row
-- square (122,125) is unmined and is where the mine goes. The tank keeps
-- clear of that square until the man is back aboard, then drives south
-- into it: the laid mine goes off under the tank and its mined neighbours
-- follow. Every step is keyed on the tank's square, speed, heading or the
-- man's status, never on the tick.

local brain = {}

local STOP_X = 122            -- road square to stop on, directly north of the mine square
local MINE_X, MINE_Y = 122, 125

local phase = "ashore"        -- ashore, brake, lay, wait, turn, drive, done
local man_left = false

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
      phase = "lay"
    else
      out.holdkeys = KEY_SLOWER
      return out
    end
  end

  if phase == "lay" then
    -- Aiming the man at (122,125), the swamp square south of the tank.
    out.build = { x = MINE_X, y = MINE_Y, action = BUILDMODE_MINE }
    phase = "wait"
    return out
  end

  if phase == "wait" then
    -- man_status is 0 while the man is in the tank; wait for him to leave
    -- and come back before moving.
    if info.man_status ~= 0 then
      man_left = true
    elseif man_left then
      phase = "turn"
    end
    if phase == "wait" then
      return out
    end
  end

  if phase == "turn" then
    -- Facing east is heading 64; turn right until heading 128 (south).
    if info.direction >= 128 then
      phase = "drive"
    else
      out.holdkeys = KEY_TURNRIGHT
      return out
    end
  end

  if phase == "drive" then
    -- Aiming for (122,125) and the mined rows 126..127 beyond it.
    out.holdkeys = KEY_FASTER
  end
  return out
end

return brain
