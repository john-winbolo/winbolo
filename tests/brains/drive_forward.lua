-- drive_forward.lua - Test brain that accelerates forward
-- Used for test 0.2: verify tank movement and position logging

local brain = {}

local tick_count = 0
local initial_x = 0
local initial_y = 0

function brain.open(info)
  initial_x = info.tankx
  initial_y = info.tanky
  print(string.format("[drive_forward] Start position: (%d, %d)", initial_x, initial_y))
end

function brain.think(info)
  tick_count = tick_count + 1

  local keys = 0

  -- Accelerate forward for the first 100 ticks, then stop
  if tick_count <= 100 then
    keys = KEY_FASTER
  end

  -- Log position every 25 ticks
  if tick_count % 25 == 0 then
    local dx = info.tankx - initial_x
    local dy = info.tanky - initial_y
    print(string.format("[drive_forward] tick=%d pos=(%d,%d) delta=(%d,%d) speed=%d",
      tick_count, info.tankx, info.tanky, dx, dy, info.speed))
  end

  return {
    holdkeys = keys,
    tapkeys = 0,
  }
end

function brain.close(info)
  local dx = info.tankx - initial_x
  local dy = info.tanky - initial_y
  print(string.format("[drive_forward] Final position: (%d, %d), moved: (%d, %d) over %d ticks",
    info.tankx, info.tanky, dx, dy, tick_count))

  -- Verify the tank actually moved
  if dx == 0 and dy == 0 then
    print("[drive_forward] FAIL: tank did not move")
  else
    print("[drive_forward] PASS: tank moved")
  end
end

return brain
