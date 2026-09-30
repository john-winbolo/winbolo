-- shell_tank_then_flee.lua -- the shooter for the builder_pool B2 arena.
--
-- Sibling of shell_pill_then_flee.lua, with one difference that is the whole
-- point of it: this one shoots at the enemy TANK, not at a pill.  B2 is about
-- danger.tank_fire_age -- the builder pool's own under-fire clock, which stamps
-- on "our armour dropped" or "a hostile shell will connect with US".  A
-- shooter aimed at a pill exercises the pill's last_hit_tick instead, which is
-- a different interlock (the repair hold) with a different constant.
--
-- Phases:
--   approach  drive to STANDOFF (a fixed tile, not a chase -- a chase makes
--             the shelling window depend on pathing luck) and brake there;
--   shell     every tick, pick the nearest visible HOSTILE tank from
--             info.objects, aim at where it actually is, and fire on
--             FIRE_PERIOD.  Firing is paced so the target is under fire for a
--             stretch rather than losing its armour in two seconds -- the test
--             measures a DENIAL that has to last long enough to be seen.
--   flee      drive away for good once the budget or the deadline is spent, so
--             the arena also contains a quiet window afterwards.
--
-- Bolo angles: 0 = North, 64 = East, 128 = South, 192 = West. The standoff is
-- SOUTH of our field across the moat, so 'flee' drives further south.

local brain = {}

local atan2 = math.atan2 or function(y, x) return math.atan(y, x) end

-- tests/generate_builder_pool_map.py, B2_FOE_STANDOFF
local STANDOFF_MX, STANDOFF_MY = 120, 131
local MAX_SHOTS   = 14
local FIRE_PERIOD = 22          -- think-frames between shells: fast enough that
                                -- the gaps stay under BUILDER_POOL_UNDER_FIRE_TICKS
                                -- (100 game ticks = 50 think-frames), so the
                                -- clock never goes quiet mid-barrage
local FLEE_T      = 900
local TOL         = 2
local SOUTH       = 128

local t = 0
local shots = 0
local phase = "approach"
local last_fire_t = -1000

local function aim_keys(info, target_angle)
  local d = (target_angle - info.direction) % 256
  if d > 128 then d = d - 256 end
  if d > TOL then return KEY_TURNRIGHT, false end
  if d < -TOL then return KEY_TURNLEFT, false end
  return 0, true
end

local function angle_to(info, wx, wy)
  local dx = wx - info.tankx
  local dy = wy - info.tanky
  return (atan2(dx, -dy) * (128.0 / math.pi)) % 256
end

-- Nearest visible tank other than ourselves, in world units; nil when none is
-- in view. No hostility test: this arena has exactly two tanks, and a scripted
-- brain has no bit library to decode ob.info's flag bits with. A tank sitting
-- exactly on us is us, so anything at a real distance is the target.
local function nearest_enemy(info)
  local bx, by, bd2 = nil, nil, math.huge
  for _, ob in ipairs(info.objects or {}) do
    if ob.type == OBJECT_TANK then
      local dx, dy = ob.x - info.tankx, ob.y - info.tanky
      local d2 = dx * dx + dy * dy
      if d2 > 64 and d2 < bd2 then bd2, bx, by = d2, ob.x, ob.y end
    end
  end
  return bx, by
end

function brain.open(info)
  print(string.format("[shell_tank_then_flee] open pos=(%d,%d) dir=%d",
                      info.tankx, info.tanky, info.direction))
end

function brain.think(info)
  t = t + 1
  local mx = math.floor(info.tankx / 256)
  local my = math.floor(info.tanky / 256)

  if phase ~= "flee" and t >= FLEE_T then
    phase = "flee"
    print(string.format("[shell_tank_then_flee] t=%d deadline -> FLEE (shots=%d)",
                        t, shots))
  end

  if phase == "approach" then
    if math.abs(mx - STANDOFF_MX) + math.abs(my - STANDOFF_MY) <= 1 then
      phase = "shell"
      print(string.format("[shell_tank_then_flee] t=%d at (%d,%d) -> SHELL",
                          t, mx, my))
    else
      local ang = angle_to(info, STANDOFF_MX * 256 + 128, STANDOFF_MY * 256 + 128)
      local turn = select(1, aim_keys(info, ang))
      return { holdkeys = KEY_FASTER + turn, tapkeys = 0 }
    end
  end

  if phase == "shell" then
    if shots >= MAX_SHOTS then
      phase = "flee"
      print(string.format("[shell_tank_then_flee] t=%d shots=%d -> FLEE", t, shots))
    else
      local ex, ey = nearest_enemy(info)
      if not ex then
        -- Nothing in view: hold station facing the field and wait.
        return { holdkeys = KEY_SLOWER, tapkeys = 0 }
      end
      local turn, aimed = aim_keys(info, angle_to(info, ex, ey))
      local keys = KEY_SLOWER + turn
      if aimed and (t - last_fire_t) >= FIRE_PERIOD then
        last_fire_t = t
        shots = shots + 1
        print(string.format("[shell_tank_then_flee] t=%d shot %d/%d at (%d,%d)",
                            t, shots, MAX_SHOTS,
                            math.floor(ex / 256), math.floor(ey / 256)))
        return { holdkeys = keys, tapkeys = KEY_SHOOT }
      end
      return { holdkeys = keys, tapkeys = 0 }
    end
  end

  local turn = select(1, aim_keys(info, SOUTH))
  return { holdkeys = KEY_FASTER + turn, tapkeys = 0 }
end

function brain.close(info)
  print(string.format("[shell_tank_then_flee] close t=%d phase=%s shots=%d arm=%s",
                      t, phase, shots, tostring(info.armour)))
end

return brain
