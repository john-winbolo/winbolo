-- =========================================================================
-- pt2_start.lua -- PT2_START_WHILE_BUILDING (Andrew's yes, Nov 23, E4):
-- may the take start while the builder still builds the planned walls?
--
--   exposure_ticks = the tick the last planned wall stands (E4-1, the
--                    engine's builder, src/bolo/lgm.c): the builder
--                    builds on arrival (lgmMoveAway: lgmDoWork puts the
--                    wall on the map when he arrives), waits LGM_BUILD_TIME
--                    there, walks back, and the next order starts in the
--                    tank (attack.lua dispatches each wall from the tank:
--                    LAST_WALL_EARLY's to_wall + build + back).
--                      T1 = trip(start, w1)
--                      Ti = T(i-1) + build + trip(w(i-1), tank) + trip(tank, wi)
--                      exposure = Tn
--   interval       = the pill's reload ticks at its anger now, as the bot
--                    sees it (world.lua's anger proxy): PILLBOX_ATTACK_NORMAL
--                    (100) halved once per landed hit, floored at
--                    PILLBOX_MAX_FIRERATE (6). Classic: 100 50 25 12 6.
--                    The same ladder as pt2_kill_lock.pill_reload.
--   shells_through = max(0, ceil(exposure / interval) - cover_now_shots)
--                    per pill whose cover needs a planned wall; summed.
--                    ceil: a loaded pill fires its first shell at once.
--   start now when armour - shells_through * 5 >= ATTACK_RUSH_MIN_ARMOUR.
--
-- Units: ticks everywhere. A trip is lgm_ticks(ox, oy, mx, my) (the
-- builder's own path cost, cpathfinder.lgm_travel_ticks_map, from the
-- caller) when given, else the straight line in squares x 16 ticks:
-- 256 wu per square / MAP_MANSPEED_TGRASS (16 wu per tick, bolo_map.h).
-- One shell takes 5 armour off the tank (the spec's 5).
-- cover_now_shots: the shots of the cover on that pill's path that stands
-- now (no planned blocker); each absorbs one shell.
-- =========================================================================

local floor, ceil, sqrt, max = math.floor, math.ceil, math.sqrt, math.max

local M = {}

M.LGM_BUILD_TIME     = 20   -- src/bolo/internal/lgm.h LGM_BUILD_TIME
M.LGM_TICKS_PER_SQ   = 16   -- 256 wu / MAP_MANSPEED_TGRASS 16 wu per tick
M.SHELL_ARMOUR       = 5    -- armour one pill shell takes off the tank
M.HEAT_MAX_HITS      = 4    -- 100 -> 50 -> 25 -> 12 -> 6 (attack.lua)

-- Reload ticks of a pill at anger `anger` (0..1, world.lua's proxy; each
-- landed hit adds bump). attack_ticks / min_ticks: the live rules
-- (pill_attack_ticks, pill_attack_min_ticks), else 100 and 6.
function M.pill_interval(anger, bump, attack_ticks, min_ticks)
  bump = bump or 0.3333
  local hits = 0
  if bump > 0 then hits = floor((anger or 0) / bump + 0.5) end
  if hits < 0 then hits = 0 elseif hits > M.HEAT_MAX_HITS then hits = M.HEAT_MAX_HITS end
  local r = attack_ticks or 100
  local lo = min_ticks or 6
  for _ = 1, hits do r = floor(r / 2) end
  if r < lo then r = lo end
  if r < 1 then r = 1 end
  return r
end

-- Builder trip ticks between two squares.
function M.trip_ticks(ox, oy, mx, my, lgm_ticks)
  if lgm_ticks then
    local t = lgm_ticks(ox, oy, mx, my)
    if t and t >= 0 then return t end
  end
  local dx, dy = mx - ox, my - oy
  return sqrt(dx * dx + dy * dy) * M.LGM_TICKS_PER_SQ
end

-- Ticks from the builder leaving at (ox, oy) to the last wall of `walls`
-- ({ mx, my } in build order) standing (E4-1: Tn above). (tx, ty): the
-- tank's square, where each later order starts; default (ox, oy).
function M.exposure_ticks(ox, oy, walls, lgm_ticks, build_time, tx, ty)
  build_time = build_time or M.LGM_BUILD_TIME
  tx, ty = tx or ox, ty or oy
  local t = 0
  for i, w in ipairs(walls) do
    if i == 1 then
      t = M.trip_ticks(ox, oy, w.mx, w.my, lgm_ticks)
    else
      local p = walls[i - 1]
      t = t + build_time + M.trip_ticks(p.mx, p.my, tx, ty, lgm_ticks)
            + M.trip_ticks(tx, ty, w.mx, w.my, lgm_ticks)
    end
  end
  return t
end

-- Shells of one pill that get through while the walls go up.
function M.shells_through(exposure, interval, cover_now_shots)
  if exposure <= 0 then return 0 end
  return max(0, ceil(exposure / interval) - (cover_now_shots or 0))
end

function M.can_start(armour, shells, min_armour)
  return (armour or 0) - shells * M.SHELL_ARMOUR >= (min_armour or 5)
end

return M
