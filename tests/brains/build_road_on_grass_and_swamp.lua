-- build_road_on_grass_and_swamp.lua - Comes ashore, stops on the road and
-- sends the man out to lay road on the two kinds of soft ground beside it:
-- grass first, then swamp.
--
-- Written for Builder Yard: the tank spawns in a boat at (116,126) facing
-- east, road runs east from square 120 along row 126, and braking on
-- reaching it brings the tank to rest on square 122. Grass square (121,125)
-- lies in the apron north of that, swamp square (122,127) in the apron
-- south. Both orders cost the same, because the road cost is a flat charge
-- on the order rather than anything to do with what is being paved over.
-- Every step is keyed on the tank's square or speed or the man's status,
-- never on the tick.

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
      { 121, 125, BUILDMODE_ROAD },
      { 122, 127, BUILDMODE_ROAD },
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
