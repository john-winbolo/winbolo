-- repair_own_pillbox.lua - Comes ashore, stops on the road and sends the
-- man out to patch up the damaged pillbox the tank already owns.
--
-- Written for Builder Yard: the tank spawns in a boat at (116,126) facing
-- east, road runs east from square 120 along row 126, and braking on
-- reaching it brings the tank to rest on square 122. The pillbox on
-- (126,125) belongs to player 0 from the moment the map loads and stands on
-- 8 of its 15 armour, so it never fires on the tank that owns it and the
-- man can walk the four squares to it unbothered. The man takes a full load
-- of trees out whatever the damage is; what the pill does not need comes
-- home again, which is why the run waits for him to climb back in. Every
-- step is keyed on the tank's square or speed or the man's status, never on
-- the tick.

local brain = {}

local STOP_X = 120            -- first road square; braking here stops on 122

local jobs = nil              -- built on the first think, once the globals exist
local job = 1
local phase = "ashore"        -- ashore, brake, order, work, done
local man_left = false

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }
  if info.dead then
    phase = "done"
    return out
  end
  if jobs == nil then
    jobs = {
      { 126, 125, BUILDMODE_PBOX },
    }
  end

  if phase == "ashore" then
    if math.floor(info.tankx / 256) >= STOP_X then
      phase = "brake"
    else
      out.holdkeys = KEY_FASTER
      return out
    end
  end

  if phase == "brake" then
    if info.speed == 0 then
      phase = "order"
    else
      out.holdkeys = KEY_SLOWER
      return out
    end
  end

  if phase == "order" then
    if job > #jobs then
      phase = "done"
      return out
    end
    local j = jobs[job]
    out.build = { x = j[1], y = j[2], action = j[3] }
    man_left = false
    phase = "work"
    return out
  end

  if phase == "work" then
    -- man_status is 0 while the man is in the tank; one job is done when he
    -- has left and come back, whatever he managed to do out there.
    if info.man_status ~= 0 then
      man_left = true
    elseif man_left then
      job = job + 1
      phase = "order"
    end
  end

  return out
end

return brain
