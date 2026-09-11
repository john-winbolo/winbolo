-- farm_two_forest_squares.lua - Comes ashore, stops on the road and sends
-- the man out to cut down the two forest squares beside it, one after the
-- other.
--
-- Written for Builder Yard: the tank spawns in a boat at (116,126) facing
-- east, road runs east from square 120 along row 126, and forest sits on
-- (123,125) and (124,125) in the apron north of it. Braking on reaching the
-- road brings the tank to rest on square 122, a square or two short of
-- both. Run under strict rules, where a tank starts with no trees at all,
-- so each load the man carries home shows in the tank's stock whole rather
-- than disappearing into the 40-tree cap. Every step is keyed on the tank's
-- square or speed or the man's status, never on the tick.

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
      { 123, 125, BUILDMODE_FARM },
      { 124, 125, BUILDMODE_FARM },
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
