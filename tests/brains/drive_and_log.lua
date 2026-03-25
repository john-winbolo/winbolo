-- drive_and_log.lua - Drives forward and logs full state each tick
-- Combines drive_forward behaviour with log_state output.
-- Used for validation tests where one client moves and the other observes.

local brain = {}

local tick_count = 0

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

local function sample_terrain(mx, my, radius)
  local samples = {}
  for dy = -radius, radius do
    for dx = -radius, radius do
      local tx, ty = mx + dx, my + dy
      if tx >= 0 and tx <= 255 and ty >= 0 and ty <= 255 then
        local t = get_terrain(tx, ty)
        samples[#samples+1] = string.format('[%d,%d,%d]', tx, ty, t)
      end
    end
  end
  return "[" .. table.concat(samples, ",") .. "]"
end

function brain.open(info)
  io.write(string.format(
    '{"event":"open","player_number":%d,"num_players":%d,"tankx":%d,"tanky":%d}\n',
    info.player_number, info.num_players, info.tankx, info.tanky))
  io.flush()
end

function brain.think(info)
  tick_count = tick_count + 1

  local keys = 0
  -- Accelerate forward for the first 100 ticks
  if tick_count <= 100 then
    keys = KEY_FASTER
  end

  local mx = math.floor(info.tankx / 256)
  local my = math.floor(info.tanky / 256)

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
    holdkeys = keys,
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
