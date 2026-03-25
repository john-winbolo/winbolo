-- idle.lua - Test brain that does nothing
-- Used for test 0.1: verify headless client connects and enters game

local brain = {}

local tick_count = 0

function brain.open(info)
  print("[idle] Brain opened. Player " .. info.player_number)
end

function brain.think(info)
  tick_count = tick_count + 1
  return {
    holdkeys = 0,
    tapkeys = 0,
  }
end

function brain.close(info)
  print("[idle] Brain closed after " .. tick_count .. " ticks")
end

return brain
