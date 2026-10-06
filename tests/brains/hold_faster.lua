-- hold_faster.lua - Test brain that holds the accelerator down and never
-- turns, so the tank drives straight the way it faces at the cap of each
-- square it crosses. A scenario points it with game.teleport.

local brain = {}

function brain.open(info)
  print("[hold_faster] Brain opened. Player " .. info.player_number)
end

function brain.think(info)
  return {
    holdkeys = KEY_FASTER,
    tapkeys = 0,
  }
end

function brain.close(info)
end

return brain
