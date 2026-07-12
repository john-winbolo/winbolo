-- drive_east.lua — hard-coded "victim" brain for the lead-targeting test.
-- Patrols back and forth (east<->west) across the shooter's front at full speed,
-- never shoots, so the shooter must LEAD it to connect. Logs its own per-tick
-- state (incl. armour) to victim_log.csv in the debug-session dir and its death
-- count in the trailer, so the runner can measure the damage the lead-aim landed.
--
-- Bolo angle convention: 0 = North, 64 = East, 128 = South, 192 = West.
-- TURNRIGHT increases the angle, TURNLEFT decreases it (tank.c tankTurn).

local brain = {}

local EAST      = 64    -- east heading
local WEST      = 192   -- west heading
local TOL       = 2     -- heading tolerance
-- Patrol back and forth across the shooter's front so the shooter gets repeated
-- crossings and (over the run) repeated kills — the death count is the metric.
local WEST_MAPX = 108   -- west turnaround
local EAST_MAPX = 158   -- east turnaround
local target    = EAST  -- current heading goal
local t = 0
local fh = nil
-- Death count = how many times the shooter killed us. info.newtank pulses true
-- on (re)spawn; count its rising edges, seeded true so the initial spawn isn't
-- miscounted as a death. This is the test's clean lead-aim metric.
local deaths = 0
local prev_newtank = true

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
  local nt = info.newtank and true or false
  if nt and not prev_newtank then deaths = deaths + 1 end
  prev_newtank = nt
  local mapx = math.floor(info.tankx / 256)
  local mapy = math.floor(info.tanky / 256)
  if fh then
    fh:write(string.format("%d,%d,%d,%d,%d,%d,%d,%s,%d\n",
      t, info.tankx, info.tanky, mapx, mapy, info.direction, info.speed,
      tostring(info.inboat), info.armour))
    if t % 50 == 0 then fh:flush() end
  end

  -- Turn around at each end so we keep re-crossing the shooter's front.
  if mapx >= EAST_MAPX then target = WEST end
  if mapx <= WEST_MAPX then target = EAST end

  -- Always accelerate; add a turn while the heading isn't yet on target so the
  -- tank reaches cruising speed immediately instead of pivoting in place first.
  local d = (target - info.direction) % 256
  if d > 128 then d = d - 256 end
  -- '+' not '|': KEY_FASTER and the turn keys are distinct bits and at most one
  -- turn key is added, so the sum equals the bitwise-or — and it parses on both
  -- PUC-Lua 5.4 and LuaJIT (5.1, which has no '|' operator).
  local keys = KEY_FASTER
  if d > TOL then
    keys = keys + KEY_TURNRIGHT
  elseif d < -TOL then
    keys = keys + KEY_TURNLEFT
  end
  return { holdkeys = keys, tapkeys = 0 }
end

function brain.close(info)
  if fh then
    fh:write(string.format("# end t=%d pos=(%d,%d) arm=%d deaths=%d\n",
      t, info.tankx, info.tanky, info.armour, deaths))
    fh:close()
  end
end

return brain
