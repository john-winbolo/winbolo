-- drive_east.lua — hard-coded "victim" brain for the lead-targeting test.
-- Drives due-east across the arena at a constant heading, never shoots, and
-- stops once it has crossed east of STOP_MAPX (so it never drives off the road
-- into deep sea and drowns). Logs its own per-tick state to victim_log.csv in
-- the debug-session dir so the test can verify the crossing + hits (armour).
--
-- Bolo angle convention: 0 = North, 64 = East, 128 = South, 192 = West.
-- TURNRIGHT increases the angle, TURNLEFT decreases it (tank.c tankTurn).

local brain = {}

local EAST      = 64    -- target heading
local TOL       = 2     -- heading tolerance
local STOP_MAPX = 160   -- stop once map-X reaches this (stay on road)
local t = 0
local fh = nil

function brain.open(info)
  local dir = _G.DEBUG_SESSION_DIR
  local path = (dir and (dir .. "/victim_log.csv")) or "victim_log.csv"
  local ok, f = pcall(io.open, path, "w")
  if ok and f then
    fh = f
    fh:write("t,tx,ty,mapx,mapy,dir,spd,inboat,arm\n")
  end
  print(string.format("[drive_east] open pos=(%d,%d) dir=%d", info.tankx, info.tanky, info.direction))
end

function brain.think(info)
  t = t + 1
  local mapx = math.floor(info.tankx / 256)
  local mapy = math.floor(info.tanky / 256)
  if fh then
    fh:write(string.format("%d,%d,%d,%d,%d,%d,%d,%s,%d\n",
      t, info.tankx, info.tanky, mapx, mapy, info.direction, info.speed,
      tostring(info.inboat), info.armour))
    if t % 50 == 0 then fh:flush() end
  end

  -- Crossing done: hold still on the road.
  if mapx >= STOP_MAPX then
    return { holdkeys = 0, tapkeys = 0 }
  end

  -- Always accelerate; add a turn while the heading isn't yet due-east so the
  -- tank reaches cruising speed immediately instead of pivoting in place first.
  local d = (EAST - info.direction) % 256
  if d > 128 then d = d - 256 end
  local keys = KEY_FASTER
  if d > TOL then
    keys = keys | KEY_TURNRIGHT
  elseif d < -TOL then
    keys = keys | KEY_TURNLEFT
  end
  return { holdkeys = keys, tapkeys = 0 }
end

function brain.close(info)
  if fh then
    fh:write(string.format("# end t=%d pos=(%d,%d) arm=%d\n", t, info.tankx, info.tanky, info.armour))
    fh:close()
  end
end

return brain
