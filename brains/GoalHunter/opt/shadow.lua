-- =========================================================================
-- shadow.lua -- engine-exact shell rays for pill take v2 (2026-11-16)
--
-- Every pillbox is a light. Walls, half walls, boats and other pills on its
-- ray toward a tank point cast a shadow. This module walks one ray the way
-- the engine moves a shell, and says what the shell meets first.
--
-- Engine facts used (src/bolo):
--   * A pill aims at a standing tank with utilCalcAngle (pillbox.c, util.c).
--     The result is a FLOAT brad (TURNTYPE is float, platform_types.h), not
--     one of 256 integer directions. So each (pill, point) pair has its own
--     ray; there is no fixed set of rays to cache.
--   * shellsAddItem: start = centre + shell_start_add * utilCalcDistance(32);
--     step = utilCalcDistanceHP (24.8 fixed point); life = 1 + 8 * len - 5.
--   * Each tick: move, then test the new point: live pill on the square,
--     then a tank in a 112 wu circle, then terrain that mapIsPassable
--     rejects (building, half building, forest, boat).
--   * A pill's shell never stops on a base (owner NEUTRAL).
--
-- No engine data is read here. Callers pass the bot's own map and pill list.
-- =========================================================================

local bit = require('bitcompat')

local M = {}

local floor, atan, cos, sin, frexp, ldexp, abs =
  math.floor, math.atan, math.cos, math.sin, math.frexp, math.ldexp, math.abs

-- Engine constants (global.h, shells.h, hit_rules.h). RADIANS_MAX in the
-- engine is 2 * 3.14159, not 2 * pi; the port keeps that.
local RADIANS_MAX = 2.0 * 3.14159
M.SHELL_SPEED      = 32
M.SHELL_START_ADD  = 5
M.SHELL_LIFE       = 8
M.PILL_FIRE_LEN    = 8.5
M.TANK_FIRE_LEN    = 7      -- GUNSIGHT_MAX 14 / 2
M.TANK_HIT_RADIUS  = 112
M.PILL_RANGE       = 2048

-- Round a double to the nearest IEEE single (round half to even), as a C
-- (float) cast does. LuaJIT numbers are doubles.
local function f32(x)
  if x == 0 or x ~= x or x == math.huge or x == -math.huge then return x end
  local m, e = frexp(x)            -- x = m * 2^e, 0.5 <= |m| < 1
  local s = ldexp(m, 24)           -- 24-bit mantissa as an integer part
  local r = floor(s)
  local d = s - r
  if d > 0.5 or (d == 0.5 and r % 2 == 1) then r = r + 1 end
  return ldexp(r, e - 24)
end
M.f32 = f32

local function round_double(x)
  if x >= 0 then return floor(x + 0.5) end
  return -floor(-x + 0.5)
end

-- utilCalcAngle(o1x, o1y, o2x, o2y): float brad from object 1 to object 2.
function M.calc_angle(o1x, o1y, o2x, o2y)
  local ddx, ddy = o2x - o1x, o2y - o1y
  local gx = ddx < 0 and -ddx or ddx
  local gy = ddy < 0 and -ddy or ddy
  local a = atan(gx / gy)
  a = (a / RADIANS_MAX) * 360.0
  local r = f32((256.0 / 360.0) * a)
  if ddx <= 0 and ddy <= 0 then
    r = f32(256.0 - r)
  elseif ddx > 0 and ddy >= 0 then
    r = f32(128.0 - r)
  elseif ddx <= 0 and ddy >= 0 then
    r = f32(r + 128)
  end
  return r
end

-- shellAngleFromTarget: the tank's own aim at a world point (float).
function M.tank_angle(ox, oy, tx, ty)
  local dx, dy = tx - ox, ty - oy
  if dx == 0 and dy == 0 then return 0 end
  local a = f32(f32(math.atan2(dx, -dy)) * f32(128.0 / 3.14159265358979323846))
  a = math.fmod(math.fmod(a, 256.0) + 256.0, 256.0)
  return f32(a)
end

local function sincos_args(angle)
  local a = f32(angle - 64)
  if a < 0 then a = f32(a + 256.0) end
  local db = (360.0 / 256.0) * a
  return (db / 360.0) * RADIANS_MAX
end

-- utilCalcDistance (whole wu per tick) and utilCalcDistanceHP (24.8).
function M.calc_distance(angle, speed)
  local db = sincos_args(angle)
  return round_double(speed * cos(db)), round_double(speed * sin(db))
end
function M.calc_distance_hp(angle, speed)
  local db = sincos_args(angle)
  return round_double(speed * cos(db) * 256.0), round_double(speed * sin(db) * 256.0)
end

-- shellLifeTicks: 1 + (int)(shell_life * len) - start_add, the product a
-- float as in the engine. env (optional): the live shell numbers
-- (env.shell_life, env.shell_start_add), else the classic ones.
function M.life_ticks(len, env)
  local life = env and env.shell_life or M.SHELL_LIFE
  local add = env and env.shell_start_add or M.SHELL_START_ADD
  local t = 1 + floor(f32(life * f32(len))) - add
  return t < 0 and 0 or t
end

-- The world points of a shell, one per tick, into out (flat x1,y1,x2,y2...).
-- Returns out, n_points. For tests and the BrainTest ray layer.
function M.shell_points(ox, oy, angle, len, out)
  out = out or {}
  local xa, ya = M.calc_distance(angle, M.SHELL_SPEED)
  local xs, ys = M.calc_distance_hp(angle, M.SHELL_SPEED)
  local x = ox + M.SHELL_START_ADD * xa
  local y = oy + M.SHELL_START_ADD * ya
  local xc, yc = 0, 0
  local n = M.life_ticks(len)
  for t = 1, n do
    xc = xc + xs; yc = yc + ys
    local xm, ym = floor(xc / 256), floor(yc / 256)
    xc = xc - xm * 256; yc = yc - ym * 256
    x = x + xm; y = y + ym
    out[2 * t - 1] = x; out[2 * t] = y
  end
  for k = 2 * n + 1, #out do out[k] = nil end
  return out, n
end

-- ── The ray of one pill toward a tank point ───────────────────────────────
-- env (all functions take map squares):
--   env.ttype(mx, my)          -> terrain id (constants T_*)
--   env.pill_on(mx, my)        -> live pill on the square or nil
--   env.is_ally(pill)          -> true for a pill on our side
--   env.cover_of_pill(pill)    -> shots an allied pill stops (5 * floor(hp/5))
--   env.T_BUILDING, T_HALFBUILD, T_BOAT, T_FOREST
--   env.wall_shots, half_wall_shots, boat_shots, forest_transparent
--   env.shell_speed, shell_start_add, shell_life, tank_hit_r (optional):
--     the live rules (pilltake_v2 sets them from info.rules), else the
--     classic M.SHELL_SPEED ... M.TANK_HIT_RADIUS
-- extra: nil or { [sq] = shots } planned blockers (walls or dropped pills).
--
-- Walks the whole life of the shell up to the tank-hit point and returns:
--   res.hit      "tank" | "reach" (shell dies first)
--   res.t        tick of the tank hit (or life)
--   res.link     first hostile (non-ally) pill met before the tank, or nil
--   res.link_t   tick of that meeting
--   res.cover    shots of every allied blocker met before the tank hit
--                (walls, half walls, boats, allied pills, planned blockers),
--                each square counted once, hostile pills skipped
--   res.front    shots of the same blockers met BEFORE the first link
--   res.sq       list of squares crossed before the tank hit (in order)
--   res.sq_t     the tick each of those squares was entered
--   res.block    list of { sq, shots, kind } blockers met, in order
-- res is reused when passed in.
function M.cast(env, sx, sy, tx, ty, len, extra, res)
  res = res or {}
  local angle = M.calc_angle(sx, sy, tx, ty)
  res.angle = angle
  local speed = env.shell_speed or M.SHELL_SPEED
  local add = env.shell_start_add or M.SHELL_START_ADD
  local xa, ya = M.calc_distance(angle, speed)
  local xs, ys = M.calc_distance_hp(angle, speed)
  local x = sx + add * xa
  local y = sy + add * ya
  local xc, yc = 0, 0
  local n = M.life_ticks(len or env.pill_fire_len or M.PILL_FIRE_LEN, env)
  local r = env.tank_hit_r or M.TANK_HIT_RADIUS
  local r2 = r * r
  local sq_list = res.sq or {}
  local sq_t = res.sq_t or {}
  local block = res.block or {}
  local nsq, nblk = 0, 0
  local last_sq = -1
  local cover, front = 0, 0
  local link, link_t = nil, nil
  local hit, hit_t = "reach", n
  local ttype, pill_on = env.ttype, env.pill_on
  for t = 1, n do
    xc = xc + xs; yc = yc + ys
    local xm, ym = floor(xc / 256), floor(yc / 256)
    xc = xc - xm * 256; yc = yc - ym * 256
    x = x + xm; y = y + ym
    local mx, my = bit.rshift(x, 8), bit.rshift(y, 8)
    local sq = my * 256 + mx
    local new_sq = sq ~= last_sq
    -- 1. a live pill on the square (engine tests it first)
    if new_sq then
      local p = pill_on(mx, my)
      if p then
        if env.is_ally(p) then
          local s = env.cover_of_pill(p)
          cover = cover + s
          if not link then front = front + s end
          nblk = nblk + 1
          block[nblk] = block[nblk] or {}
          local b = block[nblk]; b.sq = sq; b.shots = s; b.kind = "ally_pill"; b.t = t; b.pill = p
        elseif not link then
          link, link_t = p, t
        end
      end
    end
    -- 2. the tank circle at this point
    local dx, dy = x - tx, y - ty
    if dx < r and dx > -r and dy < r and dy > -r and dx * dx + dy * dy < r2 then
      hit, hit_t = "tank", t
      break
    end
    -- 3. terrain (once per square), then planned blockers
    if new_sq then
      nsq = nsq + 1
      sq_list[nsq] = sq; sq_t[nsq] = t
      local s, kind = 0, nil
      local e = extra and extra[sq]
      if e then
        s, kind = e, "planned"
      else
        local tt = ttype(mx, my)
        if tt == env.T_BUILDING then s, kind = env.wall_shots, "wall"
        elseif tt == env.T_HALFBUILD then s, kind = env.half_wall_shots, "half_wall"
        elseif tt == env.T_BOAT then s, kind = env.boat_shots, "boat"
        elseif tt == env.T_FOREST and not env.forest_transparent then s, kind = 1, "forest"
        end
      end
      if kind then
        cover = cover + s
        if not link then front = front + s end
        nblk = nblk + 1
        block[nblk] = block[nblk] or {}
        local b = block[nblk]; b.sq = sq; b.shots = s; b.kind = kind; b.t = t; b.pill = nil
      end
      last_sq = sq
    end
  end
  for k = nsq + 1, #sq_list do sq_list[k] = nil; sq_t[k] = nil end
  for k = nblk + 1, #block do block[k] = nil end
  res.hit, res.t = hit, hit_t
  res.link, res.link_t = link, link_t
  res.cover, res.front = cover, front
  res.sq, res.sq_t, res.block = sq_list, sq_t, block
  res.n_sq, res.n_block = nsq, nblk
  res.end_x, res.end_y = x, y
  return res
end

-- tank_shot's tank test: the shell at (x, y) is inside the hit circle of a
-- tank of the flat list tk (ntk numbers). It is a function of its own, not
-- a loop inside tank_shot's step loop: with the loop inline, LuaJIT's trace
-- compiler aborted on it in a real game, and that abort ended WinBoloDS
-- with 0xc0000409 (Nov 19, seed 1, oilrig).
local function ts_tank_hit(tk, ntk, x, y, r, r2)
  for i = 1, ntk, 2 do
    local dx, dy = x - tk[i], y - tk[i + 1]
    if dx < r and dx > -r and dy < r and dy > -r and dx * dx + dy * dy < r2 then return true end
  end
  return false
end

-- The tank's own shot at a world aim point. Returns
--   ok (the shell enters the target square before anything stops it),
--   why ("wall", "pill", "base", "planned", "boat", "tank", "short"),
--   list of squares crossed before the target square (the shot lane).
-- env.base_stops(mx, my) -> true when a base there stops a tank shell.
-- PT2_TANKSHOT_BLOCKERS (pilltake_v2 M.tank_blockers sets them):
--   env.ts_boats   a boat square stops the shell, as in the engine
--   env.ts_tanks   flat list { x1, y1, x2, y2, ... } of the other tanks this
--                  bot sees, where they are now. The engine tests every
--                  tank but the shooter's own on every step, after the pill
--                  on the square and before the terrain, on its hit circle
--                  (env.tank_hit_r); a team mate's tank stops it too.
function M.tank_shot(env, ox, oy, ax, ay, tmx, tmy, extra, lane)
  lane = lane or {}
  local angle = M.tank_angle(ox, oy, ax, ay)
  local speed = env.shell_speed or M.SHELL_SPEED
  local add = env.shell_start_add or M.SHELL_START_ADD
  local xa, ya = M.calc_distance(angle, speed)
  local xs, ys = M.calc_distance_hp(angle, speed)
  local x = ox + add * xa
  local y = oy + add * ya
  local xc, yc = 0, 0
  local n = M.life_ticks(M.TANK_FIRE_LEN, env)
  local nl, last_sq = 0, -1
  local omx, omy = bit.rshift(ox, 8), bit.rshift(oy, 8)
  local ok, why = false, "short"
  local tk = env.ts_tanks
  local ntk = tk and #tk or 0
  local r = env.tank_hit_r or M.TANK_HIT_RADIUS
  local r2 = r * r
  local boats = env.ts_boats
  for t = 1, n do
    xc = xc + xs; yc = yc + ys
    local xm, ym = floor(xc / 256), floor(yc / 256)
    xc = xc - xm * 256; yc = yc - ym * 256
    x = x + xm; y = y + ym
    local mx, my = bit.rshift(x, 8), bit.rshift(y, 8)
    local sq = my * 256 + mx
    local new_sq = false
    if sq ~= last_sq then
      last_sq = sq
      if mx == tmx and my == tmy then ok, why = true, nil; break end
      if not (mx == omx and my == omy) then
        new_sq = true
        nl = nl + 1; lane[nl] = sq
        if env.pill_on(mx, my) then why = "pill"; break end
      end
    end
    if ntk > 0 and ts_tank_hit(tk, ntk, x, y, r, r2) then why = "tank"; break end
    if new_sq then
      if extra and extra[sq] then why = "planned"; break end
      local tt = env.ttype(mx, my)
      if tt == env.T_BUILDING or tt == env.T_HALFBUILD then why = "wall"; break end
      if boats and tt == env.T_BOAT then why = "boat"; break end
      if env.base_stops and env.base_stops(mx, my) then why = "base"; break end
    end
  end
  for k = nl + 1, #lane do lane[k] = nil end
  return ok, why, lane
end

return M
