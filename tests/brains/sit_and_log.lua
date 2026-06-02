-- sit_and_log.lua - Sits idle and logs full state each tick
-- Used as an observer/target in combat and robustness tests (2.1, 2.3, 2.10, 2.11).

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

function brain.open(info)
  io.write(string.format(
    '{"event":"open","player_number":%d,"num_players":%d,"tankx":%d,"tanky":%d,"server_tick":%d}\n',
    info.player_number, info.num_players, info.tankx, info.tanky, info.server_tick))
  io.flush()
end

function brain.think(info)
  tick_count = tick_count + 1

  local tank_count = 0
  local shell_count = 0
  if info.objects then
    for i = 1, #info.objects do
      if info.objects[i].type == OBJECT_TANK then
        tank_count = tank_count + 1
      elseif info.objects[i].type == OBJECT_SHOT then
        shell_count = shell_count + 1
      end
    end
  end

  io.write(string.format(
    '{"event":"tick","tick":%d,"server_tick":%d,"player":%d,"num_players":%d,' ..
    '"tankx":%d,"tanky":%d,"dir":%d,"speed":%d,' ..
    '"shells":%d,"mines":%d,"armour":%d,"trees":%d,' ..
    '"tank_count":%d,"shell_count":%d,' ..
    '"objects":%s}\n',
    tick_count, info.server_tick, info.player_number, info.num_players,
    info.tankx, info.tanky, info.direction, info.speed,
    info.shells, info.mines, info.armour, info.trees,
    tank_count, shell_count,
    json_encode_objects(info.objects)
  ))
  io.flush()

  return {
    holdkeys = 0,
    tapkeys = 0,
  }
end

function brain.close(info)
  io.write(string.format(
    '{"event":"close","tick":%d,"server_tick":%d,"player":%d,"tankx":%d,"tanky":%d}\n',
    tick_count, info.server_tick, info.player_number, info.tankx, info.tanky))
  io.flush()
end

return brain
