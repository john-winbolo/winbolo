-- log_state.lua - Brain that logs full game state each tick as JSON to stdout
-- Used by validation tests to capture each client's view of the world
-- for cross-client comparison.
--
-- Output: one JSON line per tick on stdout, containing:
--   tick, player_number, tankx, tanky, direction, speed,
--   num_players, objects (visible tanks/pills/bases),
--   terrain samples around the tank, shells, mines, armour, trees

local brain = {}

local tick_count = 0

-- Simple JSON encoding for our flat data
local function json_encode_objects(objs)
  if not objs or #objs == 0 then return "[]" end
  local parts = {}
  for i = 1, #objs do
    local o = objs[i]
    parts[#parts+1] = string.format(
      '{"type":%d,"x":%d,"y":%d,"id":%d,"dir":%d,"info":%d}',
      o.type, o.x, o.y, o.idnum, o.direction, o.info)
  end
  return "[" .. table.concat(parts, ",") .. "]"
end

-- Sample terrain in a grid around the tank (map coords)
local function sample_terrain(mx, my, radius)
  local samples = {}
  for dy = -radius, radius do
    for dx = -radius, radius do
      local tx, ty = mx + dx, my + dy
      if tx >= 0 and tx <= 255 and ty >= 0 and ty <= 255 then
        local t = get_terrain(tx, ty)
        -- Only log non-deep-sea tiles to keep output manageable
        samples[#samples+1] = string.format('[%d,%d,%d]', tx, ty, t)
      end
    end
  end
  return "[" .. table.concat(samples, ",") .. "]"
end

function brain.open(info)
  -- Print header info
  io.write(string.format(
    '{"event":"open","player_number":%d,"num_players":%d,"tankx":%d,"tanky":%d}\n',
    info.player_number, info.num_players, info.tankx, info.tanky))
  io.flush()
end

function brain.think(info)
  tick_count = tick_count + 1

  -- Convert world coords to map coords for terrain sampling
  local mx = math.floor(info.tankx / 256)
  local my = math.floor(info.tanky / 256)

  -- Log every tick
  io.write(string.format(
    '{"event":"tick","tick":%d,"player":%d,"num_players":%d,' ..
    '"tankx":%d,"tanky":%d,"dir":%d,"speed":%d,' ..
    '"shells":%d,"mines":%d,"armour":%d,"trees":%d,' ..
    '"inboat":%s,"hidden":%s,' ..
    '"objects":%s,' ..
    '"terrain":%s}\n',
    tick_count, info.player_number, info.num_players,
    info.tankx, info.tanky, info.direction, info.speed,
    info.shells, info.mines, info.armour, info.trees,
    tostring(info.inboat), tostring(info.hidden),
    json_encode_objects(info.objects),
    sample_terrain(mx, my, 5)
  ))
  io.flush()

  return {
    holdkeys = 0,
    tapkeys = 0,
  }
end

function brain.close(info)
  io.write(string.format(
    '{"event":"close","tick":%d,"player":%d,"tankx":%d,"tanky":%d}\n',
    tick_count, info.player_number, info.tankx, info.tanky))
  io.flush()
end

return brain
