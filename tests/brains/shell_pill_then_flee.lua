-- shell_pill_then_flee.lua -- the shooter for tests/defend_repair_test.py.
--
-- Job, in three phases:
--   approach  drive NORTH off the start pond until the tank is at
--             STANDOFF_MY, then brake.  (Start squares must be deep sea, so
--             the tank spawns afloat on a one-tile pond and beaches itself on
--             the first step.)
--   shell     aim at the pill's tile CENTRE from wherever we actually are --
--             computed every tick from info.tankx/tanky, so a metre of drift
--             does not turn into a miss -- and fire one shell every
--             FIRE_PERIOD frames until MAX_SHOTS have left the barrel.
--             Paced, not held: the test needs the pill to be under fire for a
--             stretch, not to lose 6 HP in two seconds.  In practice our own
--             pill (which the standoff is necessarily inside the range of --
--             a tank shell reaches ~7.1 tiles and a pillbox reaches 8) kills
--             this tank after four or five shells; that is fine, a dead
--             shooter is a quiet pill, which is what the test measures.
--   flee      drive SOUTH at full speed, for good.  This is the part the test
--             is really about: once the last shell lands, the pill goes quiet
--             and the defender is supposed to fix it.
--
-- Bolo angles: 0 = North, 64 = East, 128 = South, 192 = West.  TURNRIGHT
-- increases the angle, TURNLEFT decreases it.

local brain = {}

-- math.atan2 exists on LuaJIT (5.1) and was removed in 5.4, where math.atan
-- takes the second argument instead. The brain VM can be either.
local atan2 = math.atan2 or function(y, x) return math.atan(y, x) end

local PILL_MX, PILL_MY = 126, 126     -- tests/generate_defend_repair_map.py
local STANDOFF_MY      = 132          -- brake here: 6 tiles from the pill
local MAX_SHOTS        = 6            -- 15 HP pill -> ends around 9/15, near the
                                      -- health the field incident's pill was at
local FIRE_PERIOD      = 30           -- think-frames between shells
local FLEE_T           = 500          -- ...and stop for good by here whatever
                                      -- happened.  Our target pill is inside
                                      -- PILL_FIRE_RANGE of this standoff (it has
                                      -- to be: a tank shell only reaches ~7.1
                                      -- tiles and a pill reaches 8), so the pill
                                      -- shoots back and can kill us.  A respawn
                                      -- refills the magazine, so a shot budget
                                      -- alone would let a second life shell the
                                      -- pill to death; this deadline is what
                                      -- actually guarantees the quiet the test
                                      -- is measuring.
local TOL              = 2            -- aiming tolerance, bradians
local SOUTH            = 128

local t = 0
local shots = 0                       -- monotonic across respawns
local phase = "approach"
local last_fire_t = -1000

local function aim_keys(info, target_angle)
  local d = (target_angle - info.direction) % 256
  if d > 128 then d = d - 256 end
  if d > TOL then return KEY_TURNRIGHT, false end
  if d < -TOL then return KEY_TURNLEFT, false end
  return 0, true
end

function brain.open(info)
  print(string.format("[shell_pill_then_flee] open pos=(%d,%d) dir=%d",
                      info.tankx, info.tanky, info.direction))
end

function brain.think(info)
  t = t + 1
  local my = math.floor(info.tanky / 256)

  if phase ~= "flee" and t >= FLEE_T then
    phase = "flee"
    print(string.format("[shell_pill_then_flee] t=%d deadline -> FLEE (shots=%d)",
                        t, shots))
  end

  if phase == "approach" then
    if my <= STANDOFF_MY then
      phase = "shell"
      print(string.format("[shell_pill_then_flee] t=%d at my=%d -> SHELL", t, my))
    else
      -- North, full throttle.
      local turn = select(1, aim_keys(info, 0))
      return { holdkeys = KEY_FASTER + turn, tapkeys = 0 }
    end
  end

  if phase == "shell" then
    if shots >= MAX_SHOTS then
      phase = "flee"
      print(string.format("[shell_pill_then_flee] t=%d shots=%d -> FLEE", t, shots))
    else
      -- Aim at the pill tile's centre in WORLD units (tile*256 + 128).
      local dx = (PILL_MX * 256 + 128) - info.tankx
      local dy = (PILL_MY * 256 + 128) - info.tanky
      -- atan2(dx, -dy) -> radians clockwise from north -> bradians.
      local ang = atan2(dx, -dy) * (128.0 / math.pi)
      ang = ang % 256
      local turn, aimed = aim_keys(info, ang)
      -- KEY_SLOWER holds the tank still at the standoff; releasing the
      -- throttle alone lets it coast into the moat and drown.
      local keys = KEY_SLOWER + turn
      if aimed and (t - last_fire_t) >= FIRE_PERIOD then
        last_fire_t = t
        shots = shots + 1
        print(string.format("[shell_pill_then_flee] t=%d shot %d/%d at (%d,%d)",
                            t, shots, MAX_SHOTS, PILL_MX, PILL_MY))
        return { holdkeys = keys, tapkeys = KEY_SHOOT }
      end
      return { holdkeys = keys, tapkeys = 0 }
    end
  end

  -- flee
  local turn = select(1, aim_keys(info, SOUTH))
  return { holdkeys = KEY_FASTER + turn, tapkeys = 0 }
end

function brain.close(info)
  print(string.format("[shell_pill_then_flee] close t=%d phase=%s shells=%s arm=%s",
                      t, phase, tostring(info.shells), tostring(info.armour)))
end

return brain
