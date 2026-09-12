-- place_carried_pillbox.lua - Comes ashore, drives over the flattened
-- pillbox lying in the road to pick it up, stops, and sends the man out to
-- set it down on the grass beside the tank.
--
-- Written for Builder Yard: the tank spawns in a boat at (116,126) facing
-- east, road runs east from square 120 along row 126, and a neutral pillbox
-- with no armour left sits on road square 124. A pillbox on zero armour is
-- the one a tank can carry off, and it is picked up by driving over it
-- rather than by any order, so the run only starts braking once the tank is
-- on that square; from full speed it runs two squares on and comes to rest
-- on square 126. The man then walks back to grass square (125,125) and puts
-- the pillbox down there, which costs trees on top of the pillbox itself.
-- Every step is keyed on the tank's square or speed or the man's status,
-- never on the tick.

local brain = {}

local PILL_X = 124            -- flattened pillbox in the road; braking here stops on 126

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
      { 125, 125, BUILDMODE_PBOX },
    }
  end

  if phase == "ashore" then
    if math.floor(info.tankx / 256) >= PILL_X then
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
