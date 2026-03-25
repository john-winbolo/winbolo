-- watch_objects.lua - Test brain that watches for other tanks
-- Used for test 0.3: verify two clients can see each other

local brain = {}

local tick_count = 0
local saw_other_tank = false

function brain.open(info)
  print(string.format("[watch_objects] Start. Player %d, %d players in game",
    info.player_number, info.num_players))
end

function brain.think(info)
  tick_count = tick_count + 1

  -- Check visible objects for other tanks
  if info.objects then
    for i = 1, #info.objects do
      local obj = info.objects[i]
      if obj.type == OBJECT_TANK then
        if not saw_other_tank then
          print(string.format("[watch_objects] tick=%d: First saw tank at (%d, %d) id=%d",
            tick_count, obj.x, obj.y, obj.idnum))
          saw_other_tank = true
        end
      end
    end
  end

  -- Log player count periodically
  if tick_count % 50 == 0 then
    print(string.format("[watch_objects] tick=%d: %d players, saw_tank=%s",
      tick_count, info.num_players, tostring(saw_other_tank)))
  end

  return {
    holdkeys = 0,
    tapkeys = 0,
  }
end

function brain.close(info)
  if saw_other_tank then
    print("[watch_objects] PASS: saw another tank")
  else
    print("[watch_objects] FAIL: never saw another tank")
  end
  print(string.format("[watch_objects] Ran for %d ticks", tick_count))
end

return brain
