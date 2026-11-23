-- fire_north_sights.lua — probe brain for shells_probe.scenario.lua.
--
-- The tank never moves. It turns to face due north, then fires two shells
-- at each sight in SIGHTS, one sight after the other. The scenario tracks
-- each shell with game.shells() and checks where it bursts against the
-- engine's shell model: speed x (start_add + L) wu from the gun, with
-- L = max(0, 1 + floor(life x sight / 2) - start_add) (tank.c, shells.c).
--
-- Bolo angle convention: 0 = North, 64 = East. KEY_TURNRIGHT increases the
-- angle; KEY_MORERANGE / KEY_LESSRANGE move the sight one step.

local brain = {}

local SIGHTS = { 14, 8, 4, 2, 11, 7 }
local PER_SIGHT = 2
local TOL = 0          -- brads: dead on north, so the burst is on one axis

local fired, last_shells = 0, nil

function brain.open(info)
end

function brain.think(info)
  if last_shells and info.shells < last_shells then
    fired = fired + (last_shells - info.shells)
  end
  last_shells = info.shells
  local diff = (info.direction - 0) % 256
  if diff > 128 then diff = diff - 256 end
  if diff > TOL then
    return { holdkeys = KEY_TURNLEFT, tapkeys = 0 }
  elseif diff < -TOL then
    return { holdkeys = KEY_TURNRIGHT, tapkeys = 0 }
  end
  local want = SIGHTS[math.floor(fired / PER_SIGHT) + 1]
  if not want then return { holdkeys = 0, tapkeys = 0 } end
  local sl = info.gunrange or 7
  if sl < want then return { holdkeys = 0, tapkeys = KEY_MORERANGE } end
  if sl > want then return { holdkeys = 0, tapkeys = KEY_LESSRANGE } end
  return { holdkeys = 0, tapkeys = KEY_SHOOT }
end

function brain.close(info)
end

return brain
