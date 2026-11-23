-- =========================================================================
-- pilltake_shadow.lua -- the "shadow map" pill take evaluator (2026-11-23)
--
-- PT2_EVALUATOR = "shadow" picks this file; "old" picks pilltake_v2_old.lua.
-- The result has the same shape as the old one, and the rank is the old
-- M.better, so the callers (pt2_attack, pt2_dbl) read it unchanged.
--
-- Tank points. A fixed world grid, PT2_SHADOW_GRID_WU (64) apart; the
-- points are the cell centres (k * 64 + 32), so no point is on a square
-- edge. For a target A the points are the grid points in a band of one grid
-- step around each old ring (radius + {0, -0.4}). As in the old one, the
-- inner band is tried only when the outer band has no safe point.
--
-- Rays. A pill's shell starts at its square centre (sq * 256 + 128), so
-- the offset from any pill to any grid point is in one fixed set. The walk
-- of a shell (squares entered, entry ticks, tank-hit tick) depends only on
-- that offset and the shell numbers, never on the map: terrain does not
-- stop shadow.cast's walk. So:
--   1. GEO, the offset table: one walk per offset and shell numbers,
--      shared by every pill and every pill square, kept all game. CROSS is
--      its reverse index: relative square -> the offsets whose walk enters
--      it (also shared by every pill).
--   2. MAPS, one per hostile pill square: per tank point, what the shell
--      meets there (cover, front, link, allied pill cover). It is the
--      offset table laid on the pill's square and read through the bot's
--      own map (env.ttype, env.pill_on): no other data.
--      A map is dropped when its square no longer holds a live hostile
--      pill (the pill was picked up, died, or changed side), and a new map
--      starts at the square the pill is dropped on.
--      Each run reads the squares a map's points cross once. A square whose
--      contents changed (a wall built or shot down, a half wall, forest, a
--      boat, a pill dropped or picked up, an allied pill's health) marks
--      the points in CROSS[square - pill square]; only those are walked
--      again (exactly as shadow.cast decides cover and link).
--   A ray whose walk leaves the map is never cached: it is cast exactly
--   each time (shadow.cast).
--
-- Safety (Andrew, Nov 23). The bars are the old ones (single 15, chain 10,
-- bystander 5, finish 0 on A). A chain pill (A, each next pill, the last
-- one too) is covered only by cover (or by the next chain pill, with
-- PT2_LINK_COVERS_PREV); a shell that dies before it reaches us
-- ("reach") does not count for a chain pill. "reach" counts only for a
-- bystander. depth = the last chain pill's real cover.
--
-- Steps for one target A:
--   a. the band points; T(P) = hostile pills within pill range of P
--   b. the chain at P: A, then the pill A's ray meets first, and so on
--      (the old rule); for another A of the group the chain must hold the
--      goal pill, else the point is dropped
--   c. the set test: every ray covered, or each uncovered ray has enough
--      legal blocker squares for its need within PT2_BUILD_MAX (an upper
--      bound: a square two rays share is counted for both)
--   d. our own aim: the first of the 5 aims that is clear. With no clear
--      aim, PT2_AIM_NUDGE moves only a point that passed the set test,
--      forward toward A (8 .. 256 wu). The nudged point is judged again
--      with exact casts from every pill in range (cached per run): A still
--      in range, the same chain, and the set test again; else it is
--      dropped.
--   e. the exact blocker plans on the top PT2 search_top points that need
--      blockers (a dropped pill first, then walls; the last chain pill's
--      rays first, bystanders after; on each ray the square nearest the
--      pill, and a square two uncovered rays cross before the others).
--      A blocker never goes on our own shot lane, nor in front of any
--      chain link.
--   f. rank by the old M.better; leeway = the 8 neighbour grid points that
--      also pass with the same plan; the hold point as the old one.
-- =========================================================================

local SH  = require("shadow")
local OLD = require("pilltake_v2_old")
local bit = require("bitcompat")

local M = {}
-- The rest (to_scan_result, judge, world_env, world_inp, live_shell,
-- tank_blockers, pill_name, finish_ok, better, set_band, hold_point, AIMS)
-- is the old module's.
setmetatable(M, { __index = OLD })

local sqrt, floor, sin, cos, rad, abs, atan2 =
  math.sqrt, math.floor, math.sin, math.cos, math.rad, math.abs, math.atan2 or math.atan
local rshift = bit.rshift
local AIMS = OLD.AIMS

-- ── Persistent state (one brain = one Lua state) ─────────────────────────
local REF = 128 * 256 + 128   -- the offset table's pill centre (square 128,128)
local GEO, CROSS = {}, {}     -- [shell key] -> { [okey] = geom }, { [rkey] = { okey, ... } }
local MAPS = {}               -- [pill square] = map
local MAPS_SIG = nil          -- the env numbers the maps were read with
M.stats = { geom_new = 0, map_new = 0, map_drop = 0, pt_new = 0, pt_upd = 0, sq_read = 0,
            exact = 0, nudge_try = 0, nudge_ok = 0 }

-- Drop every map and the offset table (tests; the bench's cold runs).
function M.reset(keep_geo)
  if not keep_geo then GEO, CROSS = {}, {} end
  MAPS, MAPS_SIG = {}, nil
end

local function okey_of(ox, oy) return (ox + 8192) * 16384 + (oy + 8192) end
local function rkey_of(rx, ry) return (rx + 64) * 128 + (ry + 64) end

local function shell_numbers(env)
  local speed = env.shell_speed or SH.SHELL_SPEED
  local add = env.shell_start_add or SH.SHELL_START_ADD
  local len = env.pill_fire_len or SH.PILL_FIRE_LEN
  local n = SH.life_ticks(len, env)
  local r = env.tank_hit_r or SH.TANK_HIT_RADIUS
  return speed, add, n, r, speed .. "," .. add .. "," .. n .. "," .. r
end

-- The walk of a pill's shell toward a point (ox, oy) wu from its centre:
-- the same steps as shadow.cast. Entries: the squares entered, relative to
-- the pill's square, and the tick each was entered. When the tank is hit
-- on a tick that entered a new square, that square is the last entry
-- (hit_new): shadow.cast tests a pill there but not the terrain.
local function walk(ox, oy, speed, add, n, r)
  local g = { rx = {}, ry = {}, t = {}, n = 0 }
  local tx, ty = REF + ox, REF + oy
  local angle = SH.calc_angle(REF, REF, tx, ty)
  g.angle = angle
  local xa, ya = SH.calc_distance(angle, speed)
  local xs, ys = SH.calc_distance_hp(angle, speed)
  local x, y = REF + add * xa, REF + add * ya
  local xc, yc = 0, 0
  local r2 = r * r
  local last = -1
  local hit, ht, hit_new = "reach", n, false
  local rx, ry, gt, k = g.rx, g.ry, g.t, 0
  local lo_x, hi_x, lo_y, hi_y = 0, 0, 0, 0
  for t = 1, n do
    xc = xc + xs; yc = yc + ys
    local xm, ym = floor(xc / 256), floor(yc / 256)
    xc = xc - xm * 256; yc = yc - ym * 256
    x = x + xm; y = y + ym
    local mx, my = rshift(x, 8), rshift(y, 8)
    local sq = my * 256 + mx
    local new = sq ~= last
    if new then
      k = k + 1
      local a, b = mx - 128, my - 128
      rx[k], ry[k], gt[k] = a, b, t
      if a < lo_x then lo_x = a elseif a > hi_x then hi_x = a end
      if b < lo_y then lo_y = b elseif b > hi_y then hi_y = b end
    end
    local dx, dy = x - tx, y - ty
    if dx < r and dx > -r and dy < r and dy > -r and dx * dx + dy * dy < r2 then
      hit, ht, hit_new = "tank", t, new
      break
    end
    if new then last = sq end
  end
  g.n, g.hit, g.ht, g.hit_new = k, hit, ht, hit_new
  g.ex, g.ey = x - REF, y - REF
  g.lo_x, g.hi_x, g.lo_y, g.hi_y = lo_x, hi_x, lo_y, hi_y
  return g
end

local function geom(env, ox, oy)
  local speed, add, n, r, sk = shell_numbers(env)
  local G = GEO[sk]
  if not G then G = {}; GEO[sk] = G; CROSS[sk] = {} end
  local ok = okey_of(ox, oy)
  local g = G[ok]
  if not g then
    g = walk(ox, oy, speed, add, n, r)
    g.okey = ok
    G[ok] = g
    M.stats.geom_new = M.stats.geom_new + 1
    local X = CROSS[sk]
    for k = 1, g.n do
      local rk = rkey_of(g.rx[k], g.ry[k])
      local l = X[rk]
      if not l then l = {}; X[rk] = l end
      l[#l + 1] = ok
    end
  end
  return g, sk
end
M._geom = geom

-- ── One run: square contents read once ───────────────────────────────────
-- code = terrain kind (0 none, 1 wall, 2 half wall, 3 boat, 4 forest)
--        + 8 * pill (0 none, 1 hostile, 2 + shots for an allied pill)
local function new_run(inp)
  local env = inp.env
  local run = { inp = inp, env = env, code = {}, pobj = {}, exact = {}, reach = {} }
  run.tks = { env.wall_shots, env.half_wall_shots, env.boat_shots, 1 }
  run.tkn = { "wall", "half_wall", "boat", "forest" }
  local _, _, _, _, sk = shell_numbers(env)
  run.sk = sk
  run.sig = sk .. "|" .. tostring(env.wall_shots) .. "," .. tostring(env.half_wall_shots) .. ","
    .. tostring(env.boat_shots) .. "," .. tostring(env.forest_transparent and true or false)
  run.len = env.pill_fire_len or SH.PILL_FIRE_LEN
  return run
end

local function sq_read(run, mx, my)
  local sq = my * 256 + mx
  local c = run.code[sq]
  if c then return c, run.pobj[sq] end
  local env = run.env
  M.stats.sq_read = M.stats.sq_read + 1
  local p = env.pill_on(mx, my)
  local pc = 0
  if p then
    if env.is_ally(p) then pc = 2 + env.cover_of_pill(p) else pc = 1 end
  end
  local tt = env.ttype(mx, my)
  local tk = 0
  if tt == env.T_BUILDING then tk = 1
  elseif tt == env.T_HALFBUILD then tk = 2
  elseif tt == env.T_BOAT then tk = 3
  elseif tt == env.T_FOREST and not env.forest_transparent then tk = 4 end
  c = tk + 8 * pc
  run.code[sq] = c
  run.pobj[sq] = p or false
  return c, p or false
end

-- The cached part of a ray: cover, front, link square, link tick, ally.
local function derive_sum(run, g, pmx, pmy, e)
  local cover, front, link_sq, link_t, ally = 0, 0, nil, nil, false
  local rx, ry, gt, n = g.rx, g.ry, g.t, g.n
  local tks = run.tks
  for k = 1, n do
    local mx, my = pmx + rx[k], pmy + ry[k]
    local c = sq_read(run, mx, my)
    local pc = floor(c / 8)
    if pc >= 2 then
      local s = pc - 2
      cover = cover + s
      if not link_sq then front = front + s end
      ally = true
    elseif pc == 1 and not link_sq then
      link_sq, link_t = my * 256 + mx, gt[k]
    end
    if k < n or not g.hit_new then
      local tk = c % 8
      if tk > 0 then
        local s = tks[tk]
        cover = cover + s
        if not link_sq then front = front + s end
      end
    end
  end
  e.cover, e.front, e.link_sq, e.link_t, e.ally = cover, front, link_sq, link_t, ally
end

-- The whole ray, in shadow.cast's shape (bit-exact with it; test_pt2_shadowmap).
local function derive_ray(run, g, pmx, pmy, extra, X)
  local res = { angle = g.angle, X = X }
  local sq_list, sq_t, block = {}, {}, {}
  local nsq, nblk = 0, 0
  local cover, front, link, link_t = 0, 0, nil, nil
  local rx, ry, gt, n = g.rx, g.ry, g.t, g.n
  local tks, tkn = run.tks, run.tkn
  for k = 1, n do
    local mx, my = pmx + rx[k], pmy + ry[k]
    local sq = my * 256 + mx
    local t = gt[k]
    local c, p = sq_read(run, mx, my)
    local pc = floor(c / 8)
    if pc >= 2 then
      local s = pc - 2
      cover = cover + s
      if not link then front = front + s end
      nblk = nblk + 1
      block[nblk] = { sq = sq, shots = s, kind = "ally_pill", t = t, pill = p }
    elseif pc == 1 and not link then
      link, link_t = p, t
    end
    if k == n and g.hit_new then break end
    nsq = nsq + 1
    sq_list[nsq], sq_t[nsq] = sq, t
    local s, kind = 0, nil
    local ex = extra and extra[sq]
    if ex then
      s, kind = ex, "planned"
    else
      local tk = c % 8
      if tk > 0 then s, kind = tks[tk], tkn[tk] end
    end
    if kind then
      cover = cover + s
      if not link then front = front + s end
      nblk = nblk + 1
      block[nblk] = { sq = sq, shots = s, kind = kind, t = t }
    end
  end
  res.hit, res.t = g.hit, g.ht
  res.link, res.link_t = link, link_t
  res.cover, res.front = cover, front
  res.sq, res.sq_t, res.block = sq_list, sq_t, block
  res.n_sq, res.n_block = nsq, nblk
  res.end_x, res.end_y = pmx * 256 + 128 + g.ex, pmy * 256 + 128 + g.ey
  return res
end
M._derive_ray = derive_ray
M._new_run = new_run

local function off_map(g, pmx, pmy)
  return pmx + g.lo_x < 0 or pmx + g.hi_x > 255 or pmy + g.lo_y < 0 or pmy + g.hi_y > 255
end

-- ── Maps ─────────────────────────────────────────────────────────────────
local function map_of(run, X)
  local ps = X.my * 256 + X.mx
  local m = MAPS[ps]
  if m and m.run == run then return m end
  if not m then
    m = { pmx = X.mx, pmy = X.my, ps = ps, pts = {}, snap = {}, n = 0, n_upd = 0 }
    MAPS[ps] = m
    M.stats.map_new = M.stats.map_new + 1
    m.run = run
    return m
  end
  -- first use in this run: read the squares the map's points cross again;
  -- a changed square marks the points that cross it
  m.run = run
  local X_ = CROSS[run.sk]
  local dirty, nd = nil, 0
  local pmx, pmy, snap = m.pmx, m.pmy, m.snap
  for sq, old in pairs(snap) do
    local mx, my = sq % 256, floor(sq / 256)
    local c = sq_read(run, mx, my)
    if c ~= old then
      snap[sq] = c
      local l = X_[rkey_of(mx - pmx, my - pmy)]
      if l then
        for i = 1, #l do
          local e = m.pts[l[i]]
          if e then
            dirty = dirty or {}
            if not dirty[e] then dirty[e] = true; nd = nd + 1 end
          end
        end
      end
    end
  end
  if dirty then
    for e in pairs(dirty) do derive_sum(run, e.g, pmx, pmy, e) end
    m.n_upd = m.n_upd + nd
    M.stats.pt_upd = M.stats.pt_upd + nd
  end
  return m
end

-- At the start of a run: drop the maps whose square has no live hostile pill
-- now, and every map when the env's numbers changed.
local function sync_maps(run)
  if MAPS_SIG ~= run.sig then
    MAPS, MAPS_SIG = {}, run.sig
    return
  end
  for ps, m in pairs(MAPS) do
    local c = sq_read(run, m.pmx, m.pmy)
    if floor(c / 8) ~= 1 then
      MAPS[ps] = nil
      M.stats.map_drop = M.stats.map_drop + 1
    end
  end
end

-- The cached ray summary of pill X toward grid point (px, py), or nil when
-- the walk leaves the map (then the caller casts exactly).
local function map_point(run, X, px, py)
  local pmx, pmy = X.mx, X.my
  local ox, oy = px - (pmx * 256 + 128), py - (pmy * 256 + 128)
  local g = geom(run.env, ox, oy)
  if off_map(g, pmx, pmy) then return nil, g end
  local m = map_of(run, X)
  local e = m.pts[g.okey]
  if not e then
    e = { g = g }
    derive_sum(run, g, pmx, pmy, e)
    m.pts[g.okey] = e
    m.n = m.n + 1
    M.stats.pt_new = M.stats.pt_new + 1
    local snap = m.snap
    for k = 1, g.n do
      local mx, my = pmx + g.rx[k], pmy + g.ry[k]
      local sq = my * 256 + mx
      if snap[sq] == nil then snap[sq] = run.code[sq] end
    end
  end
  return e, g
end

-- Fill a pill's whole map (every grid point in range): the bench's map
-- build time. Returns the number of points.
function M.build_full_map(inp, X)
  local run = new_run(inp)
  sync_maps(run)
  local grid = inp.shadow_grid_wu or 64
  local half = grid / 2
  local pr = inp.pill_range + inp.range_margin
  local sx, sy = X.mx * 256 + 128, X.my * 256 + 128
  local n = 0
  local i0, i1 = floor((sx - pr) / grid), floor((sx + pr) / grid)
  local j0, j1 = floor((sy - pr) / grid), floor((sy + pr) / grid)
  for i = i0, i1 do
    local px = i * grid + half
    for j = j0, j1 do
      local py = j * grid + half
      local dx, dy = px - sx, py - sy
      if dx * dx + dy * dy <= pr * pr then
        map_point(run, X, px, py)
        n = n + 1
      end
    end
  end
  return n
end

-- ── Rays at a point ──────────────────────────────────────────────────────
local function pill_centre(p) return p.mx * 256 + 128, p.my * 256 + 128 end

local function alive(p)
  return p and (p.health or 0) > 0 and not p.in_tank and not p.pos_unknown and p.mx ~= nil
end

local function exact_cast(run, X, px, py, extra)
  local sx, sy = pill_centre(X)
  M.stats.exact = M.stats.exact + 1
  if extra and next(extra) then
    local r = SH.cast(run.env, sx, sy, px, py, run.len, extra)
    r.X = X
    return r
  end
  local k = (X.my * 256 + X.mx) .. ":" .. px .. "," .. py
  local r = run.exact[k]
  if not r then
    r = SH.cast(run.env, sx, sy, px, py, run.len, nil)
    r.X = X
    run.exact[k] = r
  end
  return r
end

-- One ray summary for the set test: { X, hit, cover, link, link_t, ally }.
-- exact: cast (a nudged point). Otherwise from the map, or cast when the
-- walk leaves the map.
local function ray_sum(run, X, px, py, exact)
  if not exact then
    local e, g = map_point(run, X, px, py)
    if e then
      local link = nil
      if e.link_sq then link = run.pobj[e.link_sq] or nil end
      return { X = X, hit = g.hit, cover = e.cover, link = link, link_t = e.link_t, ally = e.ally,
               g = g }
    end
  end
  local r = exact_cast(run, X, px, py, nil)
  local ally = false
  for k = 1, r.n_block do if r.block[k].kind == "ally_pill" then ally = true end end
  return { X = X, hit = r.hit, cover = r.cover, link = r.link, link_t = r.link_t, ally = ally,
           exact = r }
end

-- The full ray (shadow.cast's shape) for ray summary s with blockers extra.
local function full_ray(run, s, px, py, extra)
  local X = s.X
  if s.g and not off_map(s.g, X.mx, X.my) then
    return derive_ray(run, s.g, X.mx, X.my, extra, X)
  end
  if extra and next(extra) then return exact_cast(run, X, px, py, extra) end
  return s.exact or exact_cast(run, X, px, py, nil)
end

-- ── The point record ─────────────────────────────────────────────────────
local function pills_in_range(ctx, px, py)
  local inp, A = ctx.inp, ctx.A
  local pr = inp.pill_range + inp.range_margin
  local pr2 = pr * pr
  local T, hasA = {}, false
  for _, p in ipairs(ctx.hostile) do
    local cx, cy = pill_centre(p)
    local dx, dy = cx - px, cy - py
    if dx * dx + dy * dy <= pr2 then
      T[#T + 1] = p
      if p == A then hasA = true end
    end
  end
  return T, hasA
end

-- The chain: A, then the pill A's ray meets first, and so on (the old rule).
local function chain_of(ctx, T, R)
  local inp, A = ctx.inp, ctx.A
  local chain, in_chain, chain_mode = { A }, { [A] = 1 }, false
  if inp.chains then
    local cur = A
    for _ = 1, 8 do
      local r = nil
      for i, X in ipairs(T) do if X == cur then r = R[i] end end
      local nxt = r and r.link or nil
      if not nxt or in_chain[nxt] or (inp.chain_skip and inp.chain_skip[nxt]) then break end
      chain[#chain + 1] = nxt
      in_chain[nxt] = #chain
      chain_mode = true
      cur = nxt
    end
  end
  return chain, in_chain, chain_mode
end

-- Bars and cover of every ray (R: summaries or full rays). Sets rec's
-- covered, uncovered, need, have, depth, ally_cover, safe, kills.
local function judge(ctx, rec, R)
  local inp, A = ctx.inp, ctx.A
  local T, chain, in_chain, chain_mode = rec.T, rec.chain, rec.in_chain, rec.chain_mode
  local covered, uncovered = true, {}
  local need_A, have_A, ally_cover = 0, 0, false
  for i, X in ipairs(T) do
    local r = R[i]
    local bar
    if X == A and ctx.finish then bar = 0
    elseif X == A and not chain_mode then bar = inp.bar_single
    elseif in_chain[X] then bar = inp.bar_chain
    else bar = inp.bar_other or inp.wall_shots end
    r.bar = bar
    r.link_cover = inp.link_covers and in_chain[X] and r.link ~= nil
                   and r.link == chain[in_chain[X] + 1] or false
    local ok
    if in_chain[X] then
      -- a chain pill: cover only ("reach" does not count; Andrew, Nov 23)
      ok = r.link_cover or r.cover >= bar
    else
      ok = r.hit == "reach" or r.cover >= bar
    end
    r.ok = ok
    if X == A then need_A, have_A = bar, r.cover end
    if not ok then covered = false; uncovered[#uncovered + 1] = i end
    if ok and r.hit ~= "reach" and r.ally then ally_cover = true end
  end
  local max_ok = #chain <= inp.max_chain
  local last, depth = chain[#chain], nil
  for i, X in ipairs(T) do if X == last then depth = R[i].cover end end
  rec.covered, rec.uncovered = covered, uncovered
  rec.need, rec.have, rec.ally_cover = need_A, have_A, ally_cover
  rec.depth = depth or (chain_mode and inp.bar_chain or inp.bar_single)
  rec.max_ok = max_ok
  rec.safe = covered and max_ok
  rec.kills = (chain_mode and max_ok) and #chain or 1
end
M._judge = judge
M._chain_of = chain_of

-- The squares in front of a chain link: on chain pill k's ray, every square
-- entered at or before it meets chain pill k + 1.
local function chain_front(rec, R)
  local fr = {}
  for i, X in ipairs(rec.T) do
    local ci = rec.in_chain[X]
    local r = R[i]
    if ci and ci < #rec.chain and r.link == rec.chain[ci + 1] and r.link_t and r.sq then
      for k = 1, r.n_sq do
        if r.sq_t[k] <= r.link_t then fr[r.sq[k]] = true end
      end
    end
  end
  return fr
end

-- A legal blocker square: k-th square of ray r. full: also the lane, the
-- taken squares and the builder's reach (the exact search).
local function slot_ok(ctx, rec, r, k, fr, lane_set, taken, full)
  local inp, env = ctx.inp, ctx.env
  local sq = r.sq[k]
  local mx, my = sq % 256, floor(sq / 256)
  local X = r.X
  if full then
    if lane_set[sq] then return false, "lane" end
    if taken[sq] then return false, "taken" end
  end
  if r.link and r.link_t and r.sq_t[k] <= r.link_t then return false, "before_link" end
  if fr[sq] then return false, "chain_front" end
  if inp.orth_only and X == ctx.A and abs(mx - X.mx) + abs(my - X.my) ~= 1 then
    return false, "not_orth"
  end
  if not inp.in_map(mx, my) or not inp.stay_in(mx, my) then return false, "area" end
  local cx, cy = mx * 256 + 128, my * 256 + 128
  local dxp, dyp = cx - rec.px, cy - rec.py
  if dxp * dxp + dyp * dyp < (inp.blocker_min_dist * 256) ^ 2 then return false, "near_tank" end
  local sx, sy = pill_centre(X)
  local d1 = (cx - sx) ^ 2 + (cy - sy) ^ 2
  local d2 = (rec.px - sx) ^ 2 + (rec.py - sy) ^ 2
  if d1 >= d2 then return false, "behind_tank" end
  if env.pill_on(mx, my) then return false, "pill" end
  local tt = env.ttype(mx, my)
  if tt == env.T_BUILDING or tt == env.T_HALFBUILD then return false, "wall" end
  if not inp.buildable(mx, my) then return false, "not_buildable" end
  if full then
    local key = rec.omx .. "," .. rec.omy .. ":" .. sq
    local rc = ctx.run.reach[key]
    if rc == nil then
      rc = inp.lgm_reach(rec.omx, rec.omy, mx, my) and true or false
      ctx.run.reach[key] = rc
    end
    if not rc then return false, "lgm_unreachable" end
  end
  return true
end

-- The fewest builds that give `need` shots: pill drops first, then walls.
local function builds_for(inp, need)
  local k, got, pills = 0, 0, inp.num_pill_blockers or 0
  while got < need do
    k = k + 1
    if pills > 0 then got = got + inp.pill_drop_shots; pills = pills - 1
    else got = got + inp.wall_shots end
    if k > 64 then break end
  end
  return k
end

-- The rays of rec in full (no planned blockers), once.
local function materialize(ctx, rec)
  if rec.rays then return rec.rays end
  local R = {}
  for i = 1, #rec.T do R[i] = full_ray(ctx.run, rec.sums[i], rec.px, rec.py, nil) end
  judge(ctx, rec, R)
  rec.rays = R
  return R
end

-- The set test's upper bound: each uncovered ray has enough legal squares
-- for its need, and no ray needs more than PT2_BUILD_MAX builds.
local function coverable(ctx, rec)
  if rec.covered then return true end
  if not rec.max_ok then return false end
  local inp = ctx.inp
  local R = materialize(ctx, rec)
  local fr = chain_front(rec, R)
  for _, ui in ipairs(rec.uncovered) do
    local r = R[ui]
    local need = builds_for(inp, r.bar - r.cover)
    if need > inp.build_max then return false end
    local n = 0
    for k = 1, r.n_sq do
      if slot_ok(ctx, rec, r, k, fr, nil, nil, false) then
        n = n + 1
        if n >= need then break end
      end
    end
    if n < need then return false end
  end
  return true
end

local function clear_aim(ctx, px, py, extra, a1, a2)
  local env, A = ctx.env, ctx.A
  env.omx, env.omy = floor(px / 256), floor(py / 256)
  local why_out, shorts = "short", nil
  for ai = a1, a2 do
    local off = AIMS[ai]
    local ok, why, ln = SH.tank_shot(env, px, py, A.mx * 256 + off[1], A.my * 256 + off[2],
                                     A.mx, A.my, extra)
    if ok then return ai, ln end
    why_out = why
    if why == "short" then shorts = shorts or {}; shorts[#shorts + 1] = ai end
  end
  return nil, why_out, shorts
end

-- The old forward nudge (pilltake_v2_old nudge_aim): returns nx, ny, wu, aim, lane.
local function nudge_aim(ctx, px, py, shorts)
  local inp, env, A = ctx.inp, ctx.env, ctx.A
  local acx, acy = pill_centre(A)
  local dx, dy = acx - px, acy - py
  local d = sqrt(dx * dx + dy * dy)
  if d < 1 then return nil end
  local live = {}
  for i = 1, #shorts do live[i] = shorts[i] end
  local tx, ty = A.mx * 256, A.my * 256
  for step = 1, inp.nudge_max_steps do
    local nw = step * inp.nudge_step_wu
    local frac = nw / d
    local nx, ny = floor(px + dx * frac + 0.5), floor(py + dy * frac + 0.5)
    local mx, my = floor(nx / 256), floor(ny / 256)
    if inp.in_map(mx, my) and inp.stay_in(mx, my) and inp.standable(mx, my) then
      env.omx, env.omy = mx, my
      local n = 0
      for i = 1, #live do
        local ai = live[i]
        local off = AIMS[ai]
        local ok, why, ln = SH.tank_shot(env, nx, ny, tx + off[1], ty + off[2], A.mx, A.my, nil)
        if ok then return nx, ny, nw, ai, ln end
        if why == "short" then n = n + 1; live[n] = ai end
      end
      for k = n + 1, #live do live[k] = nil end
      if n == 0 then return nil end
    end
  end
  return nil
end

-- The record of tank point (px, py), or nil, why. exact: rays by cast.
local function point(ctx, px, py, exact)
  local T, hasA = pills_in_range(ctx, px, py)
  if not hasA then return nil, "A_OUT_OF_RANGE" end
  local S = {}
  for i, X in ipairs(T) do S[i] = ray_sum(ctx.run, X, px, py, exact) end
  local chain, in_chain, chain_mode = chain_of(ctx, T, S)
  local rec = { px = px, py = py, nudge_wu = 0, T = T, sums = S, chain = chain, in_chain = in_chain,
                chain_mode = chain_mode, finish = ctx.finish or false }
  judge(ctx, rec, S)
  return rec
end

local function same_chain(a, b)
  if #a ~= #b then return false end
  for i = 1, #a do if a[i] ~= b[i] then return false end end
  return true
end

-- ── Blocker search (exact) ───────────────────────────────────────────────
-- Re-judge rec with blockers extra: the rays that cross a planned square
-- are walked again.
local function with_extra(ctx, rec, R, extra, new_sq)
  local R2 = {}
  for i, r in ipairs(R) do
    local hit = false
    for k = 1, r.n_sq do if r.sq[k] == new_sq then hit = true; break end end
    R2[i] = hit and full_ray(ctx.run, rec.sums[i], rec.px, rec.py, extra) or r
  end
  local o = {}
  for k, v in pairs(rec) do o[k] = v end
  o.rays = R2
  judge(ctx, o, R2)
  return o
end

-- The uncovered rays in search order: the last chain pill first, then the
-- other chain pills from the end of the chain, then bystanders (T order).
local function search_order(rec)
  local ord = {}
  for _, ui in ipairs(rec.uncovered) do ord[#ord + 1] = ui end
  local function rank(ui)
    local ci = rec.in_chain[rec.T[ui]]
    if ci then return -ci end
    return 0
  end
  table.sort(ord, function(a, b)
    local ra, rb = rank(a), rank(b)
    if ra ~= rb then return ra < rb end
    return a < b
  end)
  return ord
end

local function search_one(ctx, rec, ai)
  local inp = ctx.inp
  local extra, plan = {}, {}
  local pills_left = inp.num_pill_blockers
  local tried = {}
  local cur = rec
  local lane_set = {}
  for _, s in ipairs(rec.lanes[ai]) do lane_set[s] = true end
  for _ = 1, inp.build_max do
    if cur.covered then break end
    local R = cur.rays
    local fr = chain_front(cur, R)
    local ord = search_order(cur)
    local picked = nil
    for oi, ui in ipairs(ord) do
      local r = R[ui]
      local first = nil
      for k = 1, r.n_sq do
        local sq = r.sq[k]
        if not tried[sq] and slot_ok(ctx, cur, r, k, fr, lane_set, extra, true) then
          first = first or sq
          -- a square another uncovered ray crosses (and may take) too
          local two = false
          for oj, uj in ipairs(ord) do
            if oj ~= oi then
              local r2 = R[uj]
              for k2 = 1, r2.n_sq do
                if r2.sq[k2] == sq then
                  two = slot_ok(ctx, cur, r2, k2, fr, lane_set, extra, true)
                  break
                end
              end
              if two then break end
            end
          end
          if two then picked = sq; break end
        end
      end
      picked = picked or first
      if picked then break end
    end
    if not picked then break end
    tried[picked] = true
    local shots, kind
    if pills_left > 0 then
      shots, kind = inp.pill_drop_shots, "pill"
      pills_left = pills_left - 1
    else
      shots, kind = inp.wall_shots, "wall"
    end
    extra[picked] = shots
    local nrec = with_extra(ctx, cur, R, extra, picked)
    if not same_chain(nrec.chain, rec.chain) then
      extra[picked] = nil
      if kind == "pill" then pills_left = pills_left + 1 end
    else
      plan[#plan + 1] = { sq = picked, mx = picked % 256, my = floor(picked / 256),
                          shots = shots, kind = kind }
      cur = nrec
    end
  end
  cur.plan, cur.extra = plan, extra
  return cur
end

local function clear_aims_of(ctx, rec)
  if rec.aims_done < #AIMS then
    local env, A = ctx.env, ctx.A
    env.omx, env.omy = floor(rec.px / 256), floor(rec.py / 256)
    for ai = rec.aims_done + 1, #AIMS do
      local off = AIMS[ai]
      local ok, _, ln = SH.tank_shot(env, rec.px, rec.py, A.mx * 256 + off[1], A.my * 256 + off[2],
                                     A.mx, A.my, nil)
      if ok then
        rec.clear_aims[#rec.clear_aims + 1] = ai
        rec.lanes[ai] = ln
      end
    end
    rec.aims_done = #AIMS
  end
  return rec.clear_aims
end

-- The old choice over the clear aims: covered first, then fewer squares to
-- build, then more cover on A's ray.
local function search_blockers(ctx, rec)
  materialize(ctx, rec)
  local best = nil
  for _, ai in ipairs(clear_aims_of(ctx, rec)) do
    local base = rec
    if ai ~= rec.aim_idx then
      base = {}
      for k, v in pairs(rec) do base[k] = v end
      base.aim_idx, base.lane = ai, rec.lanes[ai]
      base.clear_aims, base.lanes, base.aims_done = { ai }, { [ai] = rec.lanes[ai] }, ai
    end
    local r = search_one(ctx, base, ai)
    if not best
       or (r.covered and not best.covered)
       or (r.covered == best.covered and (#r.plan < #best.plan
           or (#r.plan == #best.plan and r.have > best.have))) then
      best = r
    end
    if best.covered and #best.plan == 0 then break end
  end
  return best
end

-- ── Leeway: the 8 neighbour grid points with the same plan ───────────────
-- inp.shadow_leeway_wu (the bench only; nil = the grid step): the 8
-- neighbours at that distance as the old evaluator places them (unit
-- directions), cast exactly.
local function leeway(ctx, rec)
  local grid = ctx.grid
  local lw = ctx.inp.shadow_leeway_wu
  local off = lw and lw ~= grid
  local n, res = 0, {}
  local k = 0
  for dy = -1, 1 do
    for dx = -1, 1 do
      if dx ~= 0 or dy ~= 0 then
        k = k + 1
        local qx, qy = rec.px + dx * grid, rec.py + dy * grid
        if off then
          local l = sqrt(dx * dx + dy * dy)
          qx, qy = floor(rec.px + dx / l * lw + 0.5), floor(rec.py + dy / l * lw + 0.5)
        end
        local why = nil
        local ok = false
        local T, hasA = pills_in_range(ctx, qx, qy)
        if not hasA then why = "A_OUT_OF_RANGE"
        else
          local ai = clear_aim(ctx, qx, qy, rec.extra, 1, #AIMS)
          if not ai then why = "OUTGOING"
          else
            local q = { px = qx, py = qy, T = T, sums = {} }
            local R = {}
            for i, X in ipairs(T) do
              q.sums[i] = ray_sum(ctx.run, X, qx, qy, off)
              R[i] = full_ray(ctx.run, q.sums[i], qx, qy, rec.extra)
            end
            q.chain, q.in_chain, q.chain_mode = chain_of(ctx, T, R)
            judge(ctx, q, R)
            ok = q.covered and q.max_ok and (#q.chain == #rec.chain)
            if not ok then
              for _, ui in ipairs(q.uncovered) do why = "OVERLAP:" .. OLD.pill_name(ctx.inp, R[ui].X); break end
              if not why and #q.chain ~= #rec.chain then why = "CHAIN" end
            end
          end
        end
        if ok then n = n + 1 end
        res[k] = { x = qx, y = qy, ok = ok, why = (not ok) and (why or "?") or nil }
      end
    end
  end
  return n, res
end

-- ── evaluate ─────────────────────────────────────────────────────────────
-- inp: the old evaluate's fields (pilltake_v2_old), plus shadow_grid_wu
-- (PT2_SHADOW_GRID_WU) and need_goal (set by group_step: a point whose
-- chain lacks this pill is dropped).
function M.evaluate(inp)
  local cf = inp.capture_file
  if cf and cf ~= "" then return require("pt2_capture").evaluate(M, inp, cf) end
  return M.evaluate_lua(inp)
end

local function bearing(dx, dy)
  local d = math.deg(atan2(dx, -dy))
  if d < 0 then d = d + 360 end
  return d
end

function M.evaluate_lua(inp)
  local A, env = inp.A, inp.env
  local run = new_run(inp)
  sync_maps(run)
  local ctx = { inp = inp, A = A, env = env, run = run, hostile = {}, grid = inp.shadow_grid_wu or 64 }
  for _, p in ipairs(inp.pills) do
    if alive(p) and not env.is_ally(p) then ctx.hostile[#ctx.hostile + 1] = p end
  end
  ctx.finish = OLD.finish_ok(inp, A)
  local grid = ctx.grid
  local half = grid / 2
  local acx, acy = pill_centre(A)
  local all, best = {}, nil
  local n_eval, n_reject, n_nudge = 0, 0, 0
  local need_goal = inp.need_goal
  for _, roff in ipairs(inp.radii_offsets) do
    local R = inp.radius + roff
    local Rw = R * 256
    -- the band: grid points with |distance - R| under half a grid step
    local pts = {}
    local i0, i1 = floor((acx - Rw - grid) / grid), floor((acx + Rw + grid) / grid)
    local j0, j1 = floor((acy - Rw - grid) / grid), floor((acy + Rw + grid) / grid)
    for i = i0, i1 do
      local px = i * grid + half
      for j = j0, j1 do
        local py = j * grid + half
        local dx, dy = px - acx, py - acy
        local d = sqrt(dx * dx + dy * dy)
        if d >= Rw - half and d < Rw + half then
          pts[#pts + 1] = { px, py, bearing(dx, dy) }
        end
      end
    end
    table.sort(pts, function(a, b)
      if a[3] ~= b[3] then return a[3] < b[3] end
      if a[1] ~= b[1] then return a[1] < b[1] end
      return a[2] < b[2]
    end)
    local cands = {}
    for _, pt in ipairs(pts) do
      local px, py, deg = pt[1], pt[2], pt[3]
      local cx, cy = px / 256, py / 256
      local mx, my = floor(cx), floor(cy)
      if not (inp.banned and inp.banned(deg)) and inp.in_map(mx, my) and inp.stay_in(mx, my)
         and inp.standable(mx, my) then
        n_eval = n_eval + 1
        local rec, why = point(ctx, px, py, false)
        if rec and need_goal and not rec.in_chain[need_goal] then rec, why = nil, "NO_GOAL" end
        local aim_idx, lane, shorts
        if rec then
          aim_idx, lane, shorts = clear_aim(ctx, px, py, nil, 1, #AIMS)
          if not aim_idx then
            local why_out = lane
            local fixed = false
            -- the nudge: only a point that passes the set test
            if shorts and inp.aim_nudge and coverable(ctx, rec) then
              M.stats.nudge_try = M.stats.nudge_try + 1
              local nx, ny, nw, nai, nln = nudge_aim(ctx, px, py, shorts)
              if nx then
                local q = point(ctx, nx, ny, true)
                if q and same_chain(q.chain, rec.chain) and coverable(ctx, q) then
                  q.nudge_wu, q.grid_px, q.grid_py = nw, px, py
                  rec, aim_idx, lane, fixed = q, nai, nln, true
                  cx, cy = nx / 256, ny / 256
                  mx, my = floor(cx), floor(cy)
                  n_nudge = n_nudge + 1
                  M.stats.nudge_ok = M.stats.nudge_ok + 1
                else
                  why_out = q and "NUDGE_FAILS" or "NUDGE_RANGE"
                end
              end
            end
            if not fixed then rec, why = nil, "OUTGOING:" .. tostring(why_out) end
          end
        end
        if rec then
          rec.aim_idx, rec.lane = aim_idx, lane
          rec.clear_aims, rec.lanes = { aim_idx }, { [aim_idx] = lane }
          rec.aims_done = aim_idx
          rec.deg, rec.cx, rec.cy, rec.mx, rec.my, rec.radius = deg, cx, cy, mx, my, R
          rec.base = inp.score_fn(deg, mx, my, cx, cy)
          local ax = cx + sin(rad(deg)) * inp.approach_offset
          local ay = cy - cos(rad(deg)) * inp.approach_offset
          rec.omx, rec.omy = floor(ax), floor(ay)
          cands[#cands + 1] = rec
        else
          n_reject = n_reject + 1
          if inp.keep_all then
            all[#all + 1] = { deg = deg, cx = cx, cy = cy, mx = mx, my = my, rejected = why }
          end
        end
      end
    end
    -- pre-rank without builds (the old order)
    table.sort(cands, function(a, b)
      local ka = (a.chain_mode and a.max_ok) and #a.chain or 1
      local kb = (b.chain_mode and b.max_ok) and #b.chain or 1
      if ka ~= kb then return ka > kb end
      if a.covered ~= b.covered then return a.covered end
      if a.base ~= b.base then return a.base < b.base end
      return a.deg < b.deg
    end)
    local scored, n_search = {}, 0
    for _, rec in ipairs(cands) do
      local fin = rec
      if not rec.covered and n_search < inp.search_top and coverable(ctx, rec) then
        n_search = n_search + 1
        fin = search_blockers(ctx, rec)
      else
        fin.plan, fin.extra = {}, {}
        if not rec.covered then fin.unsearched = true end
      end
      fin.n_built = #fin.plan
      fin.blk = (inp.own_pill_rank and fin.ally_cover) and 2 or 1
      fin.score = fin.base + fin.n_built * inp.build_cost
      scored[#scored + 1] = fin
    end
    local cap, band = inp.leeway_min, inp.travel_band
    OLD.set_band(scored, band)
    table.sort(scored, function(a, b) return OLD.better(a, b, cap, band) end)
    if inp.leeway_on then
      for i = 1, math.min(inp.leeway_top, #scored) do
        local rec = scored[i]
        rec.leeway, rec.leeway_pts = leeway(ctx, rec)
        if rec.safe and rec.kills >= 2 and rec.leeway < inp.leeway_min then
          rec.safe, rec.why_unsafe = false, "LEEWAY"
        end
      end
      OLD.set_band(scored, band)
      table.sort(scored, function(a, b) return OLD.better(a, b, cap, band) end)
    end
    for _, rec in ipairs(scored) do all[#all + 1] = rec end
    if scored[1] and (not best or OLD.better(scored[1], best, cap, band)) then best = scored[1] end
    if best and best.safe then break end
  end
  if best then
    materialize(ctx, best)
    if best.safe and best.plan and #best.plan > 0 then
      best.hold = OLD.hold_point(ctx, best)
    end
  end
  return { best = best, all = inp.keep_all and all or nil, n_eval = n_eval, n_reject = n_reject,
           n_nudge = n_nudge, hostile = ctx.hostile, inp = inp }
end

-- The set test alone at tank point (px, py) for target inp.A (the bench:
-- would this grid point be nudged?). Returns ok, why.
function M.set_test_at(inp, px, py)
  local env = inp.env
  local run = new_run(inp)
  sync_maps(run)
  local ctx = { inp = inp, A = inp.A, env = env, run = run, hostile = {}, grid = inp.shadow_grid_wu or 64 }
  for _, p in ipairs(inp.pills) do
    if alive(p) and not env.is_ally(p) then ctx.hostile[#ctx.hostile + 1] = p end
  end
  ctx.finish = OLD.finish_ok(inp, inp.A)
  local rec, why = point(ctx, px, py, false)
  if not rec then return false, why end
  if not rec.max_ok then return false, "CHAIN_TOO_LONG" end
  if not coverable(ctx, rec) then return false, "NOT_COVERABLE" end
  return true
end

-- ── evaluate_group (the old structure) ───────────────────────────────────
function M.evaluate_group(inp)
  local job = M.group_begin(inp)
  while not M.group_step(job) do end
  return job.res
end

function M.group_begin(inp)
  return { inp = inp, goalA = inp.A, list = nil, i = 1, res = nil, ev = M }
end

function M.group_parts_left(job)
  if not job.list then return 1 end
  return #job.list - job.i + 1
end

function M.group_step(job)
  local inp, goalA = job.inp, job.goalA
  if not job.list then
    inp.need_goal = nil
    local res = M.evaluate(inp)
    res.A, res.goal_A = goalA, goalA
    job.res, job.n_eval, job.n_reject = res, res.n_eval, res.n_reject
    job.list = {}
    if not inp.chains then return true end
    local pr = inp.pill_range + inp.range_margin
    for _, p in ipairs(res.hostile) do
      local dx, dy = (p.mx - goalA.mx) * 256, (p.my - goalA.my) * 256
      if p ~= goalA and dx * dx + dy * dy <= pr * pr
         and not (inp.chain_skip and inp.chain_skip[p]) then
        job.list[#job.list + 1] = p
      end
    end
  else
    local p = job.list[job.i]
    job.i = job.i + 1
    inp.A = p
    inp.need_goal = goalA
    if inp.set_A then inp.set_A(p) end
    local ok, r = pcall(M.evaluate, inp)
    inp.A, inp.need_goal = goalA, nil
    if inp.set_A then inp.set_A(goalA) end
    if not ok then error(r, 0) end
    job.n_eval, job.n_reject = job.n_eval + r.n_eval, job.n_reject + r.n_reject
    local b, res = r.best, job.res
    if b and b.safe and b.chain_mode and b.in_chain and b.in_chain[goalA]
       and (not res.best or OLD.better(b, res.best, inp.leeway_min, inp.travel_band)) then
      r.A, r.goal_A = p, goalA
      job.res = r
    end
  end
  if job.i > #job.list then
    job.res.n_eval, job.res.n_reject = job.n_eval, job.n_reject
    return true
  end
  return false
end

-- The knob part of inp: the old one plus the grid step.
function M.knob_inp(C, inp)
  inp = OLD.knob_inp(C, inp)
  inp.shadow_grid_wu = C.PT2_SHADOW_GRID_WU or 64
  return inp
end

-- For tests: the number of maps and points kept.
function M.map_count()
  local n, p = 0, 0
  for _, m in pairs(MAPS) do n = n + 1; p = p + m.n end
  return n, p
end
function M._maps() return MAPS end

return M
