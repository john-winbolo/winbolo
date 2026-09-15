-- patrol_ns.lua -- the moving-danger source for
-- tests/refuel_lowstock_test.py.
--
-- It does exactly one thing: keep a hostile tank MOVING up and down a lane
-- that runs past two friendly bases, so goals.lua contested_penalty (which
-- only counts enemy tanks with speed > 0) charges first one base and then the
-- other.  That alternating surcharge keeps both bases' prices LIVE, so the
-- low-stock markup under test sits on top of a real, moving price.
--
-- It lives on an island across an uncrossable moat (see
-- tests/generate_refuel_lowstock_map.py), and the arena is TOURNAMENT with
-- zero neutral bases, so it holds no shells at all.  It therefore cannot be
-- reached, cannot kill the bot, cannot shoot a base down, and cannot capture
-- anything.  Pure motion.
--
-- Phases:
--   leave    drive SOUTH off the one-tile spawn pond until we are inside the
--            lane (the pond sits NORTH of Y0, so once we are past it we never
--            drive over it again -- a tank without a boat drowns on deep sea);
--   patrol   pace between Y0 and Y1, turning around at each end.
--
-- Y0/Y1 are the PATROL_Y0 / PATROL_Y1 of the generator.  Keep them in step.
--
-- Bolo angle convention: 0 = North, 64 = East, 128 = South, 192 = West.
-- TURNRIGHT increases the angle, TURNLEFT decreases it (tank.c tankTurn).

local brain = {}

local Y0     = 116        -- north turnaround
local Y1     = 133        -- south turnaround
local NORTH  = 0
local SOUTH  = 128
local TOL    = 4          -- heading tolerance, in bolo angle units

local phase  = "leave"
local target = SOUTH      -- current patrol heading
local t      = 0

function brain.open(info)
  print(string.format("[patrol_ns] open pos=(%d,%d) dir=%d",
                      info.tankx, info.tanky, info.direction))
end

-- Accelerate, adding at most one turn key while the heading is off target.
-- '+' not '|': the keys are distinct bits and at most one turn key is added,
-- so the sum equals the bitwise-or -- and it parses on both PUC-Lua 5.4 and
-- LuaJIT (5.1, which has no '|' operator).
local function drive(info, heading)
  local d = (heading - info.direction) % 256
  if d > 128 then d = d - 256 end
  local keys = KEY_FASTER
  if d > TOL then
    keys = keys + KEY_TURNRIGHT
  elseif d < -TOL then
    keys = keys + KEY_TURNLEFT
  end
  return { holdkeys = keys, tapkeys = 0 }
end

function brain.think(info)
  t = t + 1
  local mx = math.floor(info.tankx / 256)
  local my = math.floor(info.tanky / 256)

  if phase == "leave" then
    if my >= Y0 then
      phase = "patrol"
      target = SOUTH
      print(string.format("[patrol_ns] t=%d reached lane at (%d,%d) -> PATROL",
                          t, mx, my))
    else
      return drive(info, SOUTH)
    end
  end

  -- Turn around at each end of the lane.
  if my >= Y1 then target = NORTH end
  if my <= Y0 then target = SOUTH end
  return drive(info, target)
end

function brain.close(info)
  print(string.format("[patrol_ns] close t=%d phase=%s pos=(%d,%d)",
                      t, phase, math.floor(info.tankx / 256),
                      math.floor(info.tanky / 256)))
end

return brain
