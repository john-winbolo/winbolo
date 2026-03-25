-- drive_terrain_speed.lua - Drives forward and logs speed vs terrain type
-- Used for test 1.2: verify terrain speed limits are respected.
--
-- Logs the terrain type under the tank and the current speed each tick.
-- The test runner checks that speed never exceeds the terrain's max speed.
--
-- Terrain type constants (from tilenum.h):
--   DEEP_SEA=0, BUILDING=1, SWAMP=2, CRATER=3, ROAD=4, FOREST=5,
--   RUBBLE=6, GRASS=7, HALFBUILDING=8, BOAT=9, RIVER=10-13,
--   REFBASE=14, PILLBOX=15

local brain = {}

local tick_count = 0

function brain.open(info)
  io.write(string.format(
    '{"event":"open","tankx":%d,"tanky":%d}\n',
    info.tankx, info.tanky))
  io.flush()
end

function brain.think(info)
  tick_count = tick_count + 1

  local keys = 0
  -- Accelerate forward continuously
  if tick_count <= 200 then
    keys = KEY_FASTER
  end

  -- Turn right slowly to traverse different terrain types
  if tick_count > 50 and tick_count % 30 < 15 then
    keys = keys | KEY_TURNRIGHT
  end

  -- Get terrain under tank
  local mx = math.floor(info.tankx / 256)
  local my = math.floor(info.tanky / 256)
  local terrain = get_terrain(mx, my)

  -- Log every tick for speed analysis
  io.write(string.format(
    '{"event":"tick","tick":%d,"tankx":%d,"tanky":%d,"speed":%d,"terrain":%d,"mx":%d,"my":%d,"inboat":%s}\n',
    tick_count, info.tankx, info.tanky, info.speed, terrain, mx, my, tostring(info.inboat)))
  io.flush()

  return {
    holdkeys = keys,
    tapkeys = 0,
  }
end

function brain.close(info)
  io.write(string.format(
    '{"event":"close","tick":%d,"tankx":%d,"tanky":%d}\n',
    tick_count, info.tankx, info.tanky))
  io.flush()
end

return brain
