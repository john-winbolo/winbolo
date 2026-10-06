-- lead_dummy.lua -- the target for the lead_aim_* and acquire_* arenas.
--
-- It never shoots. What it does depends only on the way it faces, which the
-- arena sets with game.teleport(p, x, y, dir):
--
--   facing east or west (dir within 32 of 64 or 192): drive flat out and hold
--     that exact heading (64 or 192), so the arena gets long straight legs at
--     a steady speed. The arena teleports it back to the start of the lane
--     at the end of each pass.
--   facing north or south: press nothing and sit still. That is the stopped
--     control in lead_aim_land and the pop-up target in the acquire arenas.
--
-- Bolo angles: 0 = north, 64 = east. TURNRIGHT raises the angle.

local brain = {}

local function want_heading(dir)
  local d = dir % 256
  if d >= 32 and d < 96 then return 64 end
  if d >= 160 and d < 224 then return 192 end
  return nil
end

function brain.think(info)
  local want = want_heading(info.direction)
  if want == nil then
    return { holdkeys = 0, tapkeys = 0 }
  end
  local d = (want - info.direction) % 256
  if d > 128 then d = d - 256 end
  local keys = KEY_FASTER
  if d > 1 then
    keys = keys + KEY_TURNRIGHT
  elseif d < -1 then
    keys = keys + KEY_TURNLEFT
  end
  return { holdkeys = keys, tapkeys = 0 }
end

return brain
