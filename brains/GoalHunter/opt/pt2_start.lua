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
--
-- PT2_START_ANGER_RISE (knob, default on): the pills get angrier while the
-- walls go up, so the ceil above counts too few shells. M.rise_shells
-- walks the ticks 0 .. exposure - 1 as the engine does (pillbox.c):
--   * tick 0: each pill is loaded; its reload is M.pill_interval at its
--     anger now, and its calm timer is full when it is angry.
--   * chain pill 1 is hit by the tank's shells: the first at the flight
--     time, then one per tank reload. Chain pill k+1 is hit only by the
--     shells of chain pill k that reach it (a used link). Bystanders are
--     not hit.
--   * a hit: reload = max(floor(reload / divisor), minimum), and the calm
--     timer starts again. Each full calm period adds 1 to the reload.
--   * the pill fires when its count reaches its reload. A shell stopped by
--     cover hits no pill and takes 1 shot off the cover; each planned wall
--     counts from its own finish tick (M.wall_ticks).
--   * the shells that reach the tank are summed over the counted pills.
-- The anger to hits map (M.anger_hits) is shared by M.pill_interval,
-- attack.heat_hits_landed and pt2_kill_lock.pill_reload. With the knob
-- off it gives today's numbers.
-- =========================================================================

local floor, ceil, sqrt, max = math.floor, math.ceil, math.sqrt, math.max

local M = {}

M.LGM_BUILD_TIME     = 20   -- src/bolo/internal/lgm.h LGM_BUILD_TIME
M.LGM_TICKS_PER_SQ   = 16   -- 256 wu / MAP_MANSPEED_TGRASS 16 wu per tick
M.SHELL_ARMOUR       = 5    -- armour one pill shell takes off the tank
M.HEAT_MAX_HITS      = 4    -- 100 -> 50 -> 25 -> 12 -> 6 (attack.lua)

-- The knob (constants.lua PT2_START_ANGER_RISE); a caller may pass its own.
function M.rise_on(rise)
  if rise ~= nil then return rise and true or false end
  local C = package.loaded["constants"]
  return not (C and C.PT2_START_ANGER_RISE == false)
end

-- Hits landed on a pill, from world.lua's anger proxy (each hit adds
-- bump). Returns hits (0 .. HEAT_MAX_HITS) and at_min: true when the pill
-- is taken to be at the engine's minimum reload.
--   knob off: floor(anger / bump + 0.5), at most HEAT_MAX_HITS (today).
--   knob on:  ceil(anger / bump - 1e-6) (any anger over a whole number of
--             hits is one more hit; the 1e-6 keeps 2 x bump at 2). At 3
--             or more hits the proxy is at its top (3 x 0.3333 = 1), so
--             the pill may have had more: the minimum reload, HEAT_MAX_HITS.
function M.anger_hits(anger, bump, rise)
  bump = bump or 0.3333
  if bump <= 0 then return 0, false end
  local a = anger or 0
  local hits
  if M.rise_on(rise) then
    hits = ceil(a / bump - 1e-6)
    if hits < 0 then hits = 0 end
    if hits >= 3 then return M.HEAT_MAX_HITS, true end
    return hits, false
  end
  hits = floor(a / bump + 0.5)
  if hits < 0 then hits = 0 elseif hits > M.HEAT_MAX_HITS then hits = M.HEAT_MAX_HITS end
  return hits, false
end

-- Reload ticks of a pill at anger `anger` (0..1, world.lua's proxy; each
-- landed hit adds bump). attack_ticks / min_ticks / divisor: the live
-- rules (pill_attack_ticks, pill_attack_min_ticks, pill_angry_divisor),
-- else 100, 6 and 2. Knob off: halved once per hit (today's ladder).
function M.pill_interval(anger, bump, attack_ticks, min_ticks, divisor, rise)
  local on = M.rise_on(rise)
  local hits, at_min = M.anger_hits(anger, bump, on)
  local r = attack_ticks or 100
  local lo = min_ticks or 6
  if at_min then
    r = lo
  elseif on then
    local div = divisor or 2
    if div < 1 then div = 1 end
    for _ = 1, hits do
      if r > lo then
        r = floor(r / div)
        if r < lo then r = lo end
      end
    end
  else
    for _ = 1, hits do r = floor(r / 2) end
  end
  if r < lo then r = lo end
  if r < 1 then r = 1 end
  return r
end

-- Builder trip ticks between two squares, or nil when the builder
-- cannot get there (E4-4: lgm_ticks says -1). (bmx, bmy): the square the
-- walk may enter although it blocks him (the wall he leaves, the wall he
-- builds); default the destination. lgm_ticks nil or no answer: the
-- straight line.
function M.trip_ticks(ox, oy, mx, my, lgm_ticks, bmx, bmy)
  if lgm_ticks then
    local t = lgm_ticks(ox, oy, mx, my, bmx or mx, bmy or my)
    if t then
      if t < 0 then return nil end
      return t
    end
  end
  local dx, dy = mx - ox, my - oy
  return sqrt(dx * dx + dy * dy) * M.LGM_TICKS_PER_SQ
end

-- Ticks from the builder leaving at (ox, oy) to the last wall of `walls`
-- ({ mx, my } in build order) standing (E4-1: Tn above). (tx, ty): the
-- tank's square, where each later order starts; default (ox, oy).
-- nil when a trip cannot be made (E4-4: then the take does not start).
-- The tick each wall of `walls` stands (T1 .. Tn above), or nil when a
-- trip cannot be made.
function M.wall_ticks(ox, oy, walls, lgm_ticks, build_time, tx, ty)
  build_time = build_time or M.LGM_BUILD_TIME
  tx, ty = tx or ox, ty or oy
  local t, out = 0, {}
  for i, w in ipairs(walls) do
    if i == 1 then
      t = M.trip_ticks(ox, oy, w.mx, w.my, lgm_ticks)
      if not t then return nil end
    else
      local p = walls[i - 1]
      -- the walk back starts on the wall he built
      local back = M.trip_ticks(p.mx, p.my, tx, ty, lgm_ticks, p.mx, p.my)
      local go = back and M.trip_ticks(tx, ty, w.mx, w.my, lgm_ticks)
      if not go then return nil end
      t = t + build_time + back + go
    end
    out[i] = t
  end
  return out
end

-- The builder's whole timeline for `walls` (the same trips as
-- wall_ticks, plus the walk home after the last wall): a list of legs
-- { t0, t1, x0, y0, x1, y1, kind = "out" | "build" | "back", i = wall index }
-- in ticks from now and square centres (tiles), and the tick he is home.
-- He walks each leg in a straight line at an even pace. nil when a trip
-- cannot be made.
function M.trip_timeline(ox, oy, walls, lgm_ticks, build_time, tx, ty)
  build_time = build_time or M.LGM_BUILD_TIME
  tx, ty = tx or ox, ty or oy
  local legs, t = {}, 0
  local sx, sy = ox, oy
  for i, w in ipairs(walls) do
    local go = M.trip_ticks(sx, sy, w.mx, w.my, lgm_ticks)
    if not go then return nil end
    legs[#legs + 1] = { t0 = t, t1 = t + go, x0 = sx + 0.5, y0 = sy + 0.5,
                        x1 = w.mx + 0.5, y1 = w.my + 0.5, kind = "out", i = i }
    t = t + go
    legs[#legs + 1] = { t0 = t, t1 = t + build_time, x0 = w.mx + 0.5, y0 = w.my + 0.5,
                        x1 = w.mx + 0.5, y1 = w.my + 0.5, kind = "build", i = i }
    t = t + build_time
    local back = M.trip_ticks(w.mx, w.my, tx, ty, lgm_ticks, w.mx, w.my)
    if not back then return nil end
    legs[#legs + 1] = { t0 = t, t1 = t + back, x0 = w.mx + 0.5, y0 = w.my + 0.5,
                        x1 = tx + 0.5, y1 = ty + 0.5, kind = "back", i = i }
    t = t + back
    sx, sy = tx, ty
  end
  return legs, t
end

-- Where the builder is at tick t of a trip_timeline (tiles), or nil when he
-- is in the tank (before the first leg, between trips or home).
function M.man_at(legs, t)
  for _, L in ipairs(legs) do
    if t >= L.t0 and t <= L.t1 then
      local d = L.t1 - L.t0
      local w = d > 0 and (t - L.t0) / d or 1
      return L.x0 + (L.x1 - L.x0) * w, L.y0 + (L.y1 - L.y0) * w, L
    end
  end
  return nil
end

function M.exposure_ticks(ox, oy, walls, lgm_ticks, build_time, tx, ty)
  local ts = M.wall_ticks(ox, oy, walls, lgm_ticks, build_time, tx, ty)
  if not ts then return nil end
  return ts[#ts] or 0
end

-- Shells of one pill that get through while the walls go up.
function M.shells_through(exposure, interval, cover_now_shots)
  if exposure <= 0 then return 0 end
  return max(0, ceil(exposure / interval) - (cover_now_shots or 0))
end

-- PT2_START_ANGER_RISE: the tick walk (see the header). ps: the chain
-- pills in chain order, each
--   { speed = reload at tick 0 (M.pill_interval), to = index in ps of the
--     pill its shells hit (a used link) or nil (the tank),
--     cover = { { t = tick it stands (0: now), shots } ... } on its ray,
--     count = true when its shells at the tank are summed,
--     off = true: a dead pill (it neither fires nor is hit) }
-- o: { attack_ticks, min_ticks, divisor, cooldown, tank_reload, flight }.
-- A link hit lands on the next tick. Returns the summed shells and the
-- shells per pill.
function M.rise_shells(ps, exposure, o)
  local attack = o.attack_ticks or 100
  local lo = o.min_ticks or 6
  local div = o.divisor or 2
  if div < 1 then div = 1 end
  local cool_t = o.cooldown or 32
  local reload = o.tank_reload or 13
  if reload < 1 then reload = 1 end
  local flight = o.flight or 0
  local n = #ps
  local speed, cnt, cool, used, out, link_hits = {}, {}, {}, {}, {}, {}
  for i, p in ipairs(ps) do
    speed[i] = p.speed
    cnt[i] = p.speed                      -- loaded at tick 0
    cool[i] = (p.speed < attack) and cool_t or 0
    used[i], out[i], link_hits[i] = 0, 0, 0
  end
  local total = 0
  for t = 0, (exposure or 0) - 1 do
    -- the hits that land on this tick (a pill that is off is not hit)
    for i = 1, n do
      local h = link_hits[i]
      link_hits[i] = 0
      if i == 1 and t >= flight and (t - flight) % reload == 0 then h = h + 1 end
      if ps[i].off then h = 0 end
      for _ = 1, h do
        if speed[i] > lo then
          speed[i] = floor(speed[i] / div)
          if speed[i] < lo then speed[i] = lo end
        end
        cool[i] = cool_t
      end
    end
    -- the engine's pill tick: count, calm timer, fire
    for i = 1, n do
      if cnt[i] < speed[i] then cnt[i] = cnt[i] + 1 end
      if cool[i] > 0 then
        cool[i] = cool[i] - 1
        if cool[i] == 0 then
          speed[i] = speed[i] + 1
          if speed[i] < attack then cool[i] = cool_t end
        end
      end
      local p = ps[i]
      if not p.off and cnt[i] >= speed[i] then
        cnt[i] = 0
        local avail = -used[i]
        for _, c in ipairs(p.cover or {}) do
          if c.t <= t then avail = avail + c.shots end
        end
        if avail > 0 then
          used[i] = used[i] + 1
        elseif p.to then
          link_hits[p.to] = link_hits[p.to] + 1
        else
          out[i] = out[i] + 1
          if p.count then total = total + 1 end
        end
      end
    end
  end
  return total, out
end

-- The tick walk for a chain as the callers keep it. entries (chain order):
--   { speed, to, cover_now, planned = { { mx, my, shots } }, count, off }
-- wall_t(mx, my): the tick a planned wall stands (0: it stands now); a
-- wall it does not know never stands inside the window.
function M.rise_chain(entries, exposure, wall_t, o)
  local ps = {}
  for i, e in ipairs(entries) do
    local cover = {}
    if (e.cover_now or 0) > 0 then cover[1] = { t = 0, shots = e.cover_now } end
    for _, q in ipairs(e.planned or {}) do
      cover[#cover + 1] = { t = wall_t(q.mx, q.my) or math.huge, shots = q.shots or 0 }
    end
    ps[i] = { speed = e.speed, to = e.to, cover = cover, count = e.count, off = e.off }
  end
  return M.rise_shells(ps, exposure, o)
end

-- Ticks of the tank's first shell to reach a pill at (mx, my) from
-- (px, py) (wu), at shell_speed wu per tick (classic 32).
function M.flight_ticks(px, py, mx, my, shell_speed)
  local dx, dy = mx * 256 + 128 - px, my * 256 + 128 - py
  return ceil(sqrt(dx * dx + dy * dy) / (shell_speed or 32))
end

function M.can_start(armour, shells, min_armour)
  return (armour or 0) - shells * M.SHELL_ARMOUR >= (min_armour or 5)
end

return M
