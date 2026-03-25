-- drive_and_check_corrections.lua - Drives forward and detects position corrections
-- Used for test 1.1: verify normal movement is accepted without server corrections.
--
-- A correction is detected when the tank position jumps backwards (towards the
-- start) between ticks, which would indicate the server sent a position
-- correction to override the client's predicted position.

local brain = {}

local tick_count = 0
local prev_x = 0
local prev_y = 0
local corrections_detected = 0
local positions = {}

function brain.open(info)
  prev_x = info.tankx
  prev_y = info.tanky
  io.write(string.format(
    '{"event":"open","tankx":%d,"tanky":%d}\n',
    info.tankx, info.tanky))
  io.flush()
end

function brain.think(info)
  tick_count = tick_count + 1

  local keys = 0
  -- Accelerate forward for first 150 ticks
  if tick_count <= 150 then
    keys = KEY_FASTER
  end

  -- Detect backwards jumps in position (potential correction)
  if tick_count > 10 and prev_x ~= 0 and prev_y ~= 0 then
    local dx = info.tankx - prev_x
    local dy = info.tanky - prev_y
    local dist_sq = dx * dx + dy * dy
    -- A correction would cause a backwards jump - large negative displacement
    -- along the direction of travel. We detect this as a large distance between
    -- consecutive ticks in the wrong direction.
    if dist_sq > 256 * 256 then  -- More than 1 map square jump
      corrections_detected = corrections_detected + 1
    end
  end

  prev_x = info.tankx
  prev_y = info.tanky

  -- Log every 25 ticks
  if tick_count % 25 == 0 then
    io.write(string.format(
      '{"event":"tick","tick":%d,"tankx":%d,"tanky":%d,"speed":%d,"corrections":%d}\n',
      tick_count, info.tankx, info.tanky, info.speed, corrections_detected))
    io.flush()
  end

  return {
    holdkeys = keys,
    tapkeys = 0,
  }
end

function brain.close(info)
  io.write(string.format(
    '{"event":"close","tick":%d,"tankx":%d,"tanky":%d,"corrections":%d}\n',
    tick_count, info.tankx, info.tanky, corrections_detected))
  io.flush()
end

return brain
