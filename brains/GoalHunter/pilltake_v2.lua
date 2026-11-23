-- =========================================================================
-- pilltake_v2.lua -- the "strip" pill take evaluator (2026-11-23, Andrew's
-- design). PT2_EVALUATOR = "v2" picks it; "old" picks pilltake_v2_old.lua.
--
-- The result has the same shape as the old one (evaluate, group_step), and
-- the rank is the old M.better (plus PT2_PREFER_EXISTING_COVER), so pt2_attack, pt2_dbl and the rest only
-- choose the evaluator through pt2_eval.lua.
--
--  1. Tank points: the old rings (radii_offsets, PT2_ANGLE_STEP_DEG), the
--     old in_map / stay_in / standable / banned filters. PT2_AIM_NUDGE
--     moves only a point whose aims are all short (no wall, pill or base
--     stops them); the nudged point is judged again with exact casts.
--  2. Wedges, kept between runs (module state, rebuilt only when the bot's
--     own map changes in range of the pill, or the pill moves, dies or
--     changes side):
--       centre wedges of a hostile pill X: from X's square centre to the 4
--         corners of every blocker square in range (wall, half wall, boat,
--         forest unless PT2_FOREST_TRANSPARENT, allied pill, and hostile
--         pills as links), with their distances;
--       aim wedges of the start pill A: from each of A's aim points to every
--         square that stops our own shell (wall, half wall, any pill but A,
--         a boat with PT2_TANKSHOT_BLOCKERS).
--  3. Masks on the ring points (angle and distance tests, no casts):
--       MUST(X, Y): X's shell meets Y with nothing in front of Y;
--       CANT(A):    all aim lines to A are blocked;
--       COVER(X):   shots in front of the point before the first hostile pill.
--     A mask test says yes, no or "not sure" (the point is near a wedge
--     edge). Only a sure CANT(A) removes a point: masks never add one.
--  4. Chains: a DFS from A over MUST (length 1 .. PT2_MAX_CHAIN, chain_skip
--     ends a chain, partial chains count). At a point the chain is the one
--     the exact rays give: A, then the pill A's shell meets first when no
--     blocker is in front of it, and so on, at most PT2_MAX_CHAIN pills. A
--     link with a blocker in front is never used: the chain is cut there.
--  5. Exact check: every point the masks keep gets shadow.cast from every
--     pill in range and the exact tank_shot aims. Each (pill, point) ray is
--     cast once per group run and kept; a ray with planned blockers is made
--     from it (the walk does not change, only the cover).
--  6. Bars as old: the last chain pill PT2_CHAIN_BLOCKER_MIN_SHOTS, a
--     single take PPT_COVER_TARGET_SHOTS, the finish rule 0 on A, a
--     bystander one wall or out of reach. A chain pill's shell that dies
--     before it reaches us does not count: it needs its cover.
--  7. Walls: every set of up to PT2_BUILD_MAX legal squares on the
--     uncovered paths (a dropped carried pill first). Fewest blockers, then
--     more depth, then a square on two paths, then the builder's shortest
--     trip. One exact evaluation with the chosen blockers confirms the plan.
--     Every point the masks keep gets a plan.
--  8. Rank: the old M.better, travel band and leeway as old; with
--     PT2_PREFER_EXISTING_COVER, fewer planned blockers first in the band.
--  9. A lone pill (no other hostile pill in range of any point): the points
--     are planned in travel score order, and planning stops once no point
--     left can come into the travel band (the best is the same).
-- =========================================================================

local SH  = require("shadow")
local OLD = require("pilltake_v2_old")

local M = {}
-- to_scan_result, world_env, world_inp, live_shell, tank_blockers,
-- pill_name, finish_ok, set_band, hold_point, AIMS: the old module's.
setmetatable(M, { __index = OLD })

-- The old knobs plus v2's own.
function M.knob_inp(C, inp)
  inp = OLD.knob_inp(C, inp)
  -- PT2_PREFER_EXISTING_COVER (E1)
  inp.prefer_existing_cover = C.PT2_PREFER_EXISTING_COVER ~= false
  return inp
end

-- The rank: the old better, and PT2_PREFER_EXISTING_COVER (Nov 23,
-- Andrew's yes, E1). With pec on, two points of the same safe and kills
-- that are both in the travel band rank by n_built first (fewer planned
-- blockers win; a dropped pill counts as one), before depth, blk and
-- leeway. Outside the band, the old order stands.
function M.better(a, b, cap, band, pec)
  if pec and band and band > 0 and a.safe == b.safe and a.kills == b.kills then
    local sa, sb = a.score, b.score
    local m = math.min(sa, sb, a.tier_min or sa, b.tier_min or sb)
    if sa <= m + band and sb <= m + band then
      local na, nb = a.n_built or 0, b.n_built or 0
      if na ~= nb then return na < nb end
    end
  end
  return OLD.better(a, b, cap, band)
end

local sqrt, floor, sin, cos, rad, abs, min, max =
  math.sqrt, math.floor, math.sin, math.cos, math.rad, math.abs, math.min, math.max
local atan2 = math.atan2 or math.atan
local PI, TWO_PI = math.pi, 2 * math.pi

-- Wedge edge margins (wu). A shell path is within a few wu of the straight
-- line (rounded start and step), and its samples are one shell step apart.
-- A square shrunk by EDGE_IN is surely crossed with a sample inside the
-- real square; a square grown by EDGE_IN is surely missed.
local EDGE_IN = 24
local SLACK = 4            -- path and start rounding
local NBIN = 128           -- angle bins per wedge set

local K_WALL, K_HALF, K_BOAT, K_FOREST, K_ALLY, K_HOST = 1, 2, 3, 4, 5, 6
M.KIND_NAME = { "wall", "half_wall", "boat", "forest", "ally_pill", "hostile_pill" }

local function pill_centre(p) return p.mx * 256 + 128, p.my * 256 + 128 end
local function alive(p)
  return p and (p.health or 0) > 0 and not p.in_tank and not p.pos_unknown and p.mx ~= nil
end

-- bearing of (dx, dy): 0 = north (-y), clockwise, in (-pi, pi]
local function bearing(dx, dy) return atan2(dx, -dy) end
local function wrap(a)
  if a > PI then a = a - TWO_PI elseif a <= -PI then a = a + TWO_PI end
  if a > PI then a = a - TWO_PI elseif a <= -PI then a = a + TWO_PI end
  return a
end
local function bin_of(th)
  local b = floor((th + PI) / TWO_PI * NBIN)
  if b >= NBIN then b = NBIN - 1 elseif b < 0 then b = 0 end
  return b
end

-- The angle span of box [x0,x1] x [y0,y1] seen from (ax, ay): its centre
-- bearing c and the offsets lo <= 0 <= hi of the corners from c. nil when
-- the apex is in the box.
local function span(ax, ay, x0, y0, x1, y1)
  if ax >= x0 and ax <= x1 and ay >= y0 and ay <= y1 then return nil end
  local c = bearing((x0 + x1) * 0.5 - ax, (y0 + y1) * 0.5 - ay)
  local lo, hi = 0, 0
  local r1 = wrap(bearing(x0 - ax, y0 - ay) - c)
  local r2 = wrap(bearing(x1 - ax, y0 - ay) - c)
  local r3 = wrap(bearing(x0 - ax, y1 - ay) - c)
  local r4 = wrap(bearing(x1 - ax, y1 - ay) - c)
  lo = min(r1, r2, r3, r4); hi = max(r1, r2, r3, r4)
  return c, lo, hi
end
M._span = span

local function box_near(ax, ay, x0, y0, x1, y1)
  local dx = max(x0 - ax, 0, ax - x1)
  local dy = max(y0 - ay, 0, ay - y1)
  return sqrt(dx * dx + dy * dy)
end
local function box_far(ax, ay, x0, y0, x1, y1)
  local dx = max(abs(x0 - ax), abs(x1 - ax))
  local dy = max(abs(y0 - ay), abs(y1 - ay))
  return sqrt(dx * dx + dy * dy)
end

-- One wedge: apex (ax, ay) to square (mx, my).
local function make_wedge(ax, ay, mx, my, kind, shots, pill)
  local x0, y0 = mx * 256, my * 256
  local x1, y1 = x0 + 255, y0 + 255
  local c, lo_o, hi_o = span(ax, ay, x0 - EDGE_IN, y0 - EDGE_IN, x1 + EDGE_IN, y1 + EDGE_IN)
  if not c then return nil end
  local ci, lo_i, hi_i = span(ax, ay, x0 + EDGE_IN, y0 + EDGE_IN, x1 - EDGE_IN, y1 - EDGE_IN)
  local w = {
    sq = my * 256 + mx, mx = mx, my = my, kind = kind, shots = shots, pill = pill,
    c = c, lo_o = lo_o, hi_o = hi_o,
    near = box_near(ax, ay, x0, y0, x1, y1), far = box_far(ax, ay, x0, y0, x1, y1),
  }
  if ci then w.lo_i, w.hi_i = wrap(ci + lo_i - c), wrap(ci + hi_i - c) else w.lo_i, w.hi_i = 1, -1 end
  return w
end
M._make_wedge = make_wedge

-- A wedge set: the list (nearest first) and NBIN angle bins.
local function wedge_set(list)
  table.sort(list, function(a, b)
    if a.near ~= b.near then return a.near < b.near end
    return a.sq < b.sq
  end)
  local bins = {}
  for b = 0, NBIN - 1 do bins[b] = {} end
  for _, w in ipairs(list) do
    local a0, a1 = w.c + w.lo_o, w.c + w.hi_o
    local b0 = floor((a0 + PI) / TWO_PI * NBIN)
    local b1 = floor((a1 + PI) / TWO_PI * NBIN)
    for b = b0, b1 do
      local bb = b % NBIN
      local t = bins[bb]
      t[#t + 1] = w
    end
  end
  return { list = list, bins = bins }
end

-- ── Module state: wedges kept between runs ───────────────────────────────
local WC = {}                 -- key -> { sig, set, pill }
local PID = setmetatable({}, { __mode = "k" })
local npid = 0
local function pid_of(p)
  local k = PID[p]
  if not k then npid = npid + 1; k = npid; PID[p] = k end
  return k
end
M.stats = { wedge_new = 0, wedge_keep = 0 }

function M.reset()
  WC = {}
  M.stats.wedge_new, M.stats.wedge_keep = 0, 0
end
function M.wedge_count()
  local n = 0
  for _ in pairs(WC) do n = n + 1 end
  return n
end

-- Square offsets by near distance from a square centre, up to `reach` wu.
local OFFS = {}
local function offsets(reach)
  local key = floor(reach)
  local o = OFFS[key]
  if o then return o end
  o = {}
  local R = math.ceil(reach / 256) + 1
  for dy = -R, R do
    for dx = -R, R do
      if dx ~= 0 or dy ~= 0 then
        local nx = max(0, abs(dx) * 256 - 128)
        local ny = max(0, abs(dy) * 256 - 128)
        local d = sqrt(nx * nx + ny * ny)
        if d <= reach then o[#o + 1] = { dx, dy, d } end
      end
    end
  end
  table.sort(o, function(a, b)
    if a[3] ~= b[3] then return a[3] < b[3] end
    if a[2] ~= b[2] then return a[2] < b[2] end
    return a[1] < b[1]
  end)
  OFFS[key] = o
  return o
end

-- What square (mx, my) holds, read once per run from the bot's own map:
-- kind (K_*), shots, pill (or nil); kind 0 = nothing that stops a shell.
local function sq_read(ctx, mx, my)
  local key = my * 256 + mx
  local m = ctx.sqm
  local v = m[key]
  if v then return v end
  local inp, env = ctx.inp, ctx.env
  if not inp.in_map(mx, my) then
    v = { 0, 0 }
  else
    local p = env.pill_on(mx, my)
    if p then
      if env.is_ally(p) then v = { K_ALLY, env.cover_of_pill(p), p }
      else v = { K_HOST, 0, p } end
    else
      local tt = env.ttype(mx, my)
      if tt == env.T_BUILDING then v = { K_WALL, env.wall_shots }
      elseif tt == env.T_HALFBUILD then v = { K_HALF, env.half_wall_shots }
      elseif tt == env.T_BOAT then v = { K_BOAT, env.boat_shots }
      elseif tt == env.T_FOREST and not env.forest_transparent then v = { K_FOREST, 1 }
      else v = { 0, 0 } end
    end
  end
  m[key] = v
  ctx.stat.sq_reads = ctx.stat.sq_reads + 1
  return v
end

-- The centre wedges of hostile pill X (kept while nothing changes).
local function centre_wedges(ctx, X)
  local inp, env = ctx.inp, ctx.env
  local reach = inp.pill_range + inp.range_margin
  local o = offsets(reach)
  local parts = { env.forest_transparent and "t" or "f", env.wall_shots, env.half_wall_shots, env.boat_shots }
  local found = {}
  for i = 1, #o do
    local mx, my = X.mx + o[i][1], X.my + o[i][2]
    local v = sq_read(ctx, mx, my)
    if v[1] ~= 0 then
      found[#found + 1] = { mx, my, v }
      parts[#parts + 1] = (my * 256 + mx) .. ":" .. v[1] .. ":" .. v[2] .. ":" .. (v[3] and pid_of(v[3]) or 0)
    end
  end
  local sig = table.concat(parts, ",")
  local key = "c" .. (X.my * 256 + X.mx)
  local e = WC[key]
  if e and e.sig == sig and e.pill == X then
    M.stats.wedge_keep = M.stats.wedge_keep + 1
    return e.set
  end
  local sx, sy = pill_centre(X)
  local list = {}
  for _, f in ipairs(found) do
    local v = f[3]
    local w = make_wedge(sx, sy, f[1], f[2], v[1], v[2], v[3])
    if w then list[#list + 1] = w end
  end
  local set = wedge_set(list)
  set.X = X
  WC[key] = { sig = sig, set = set, pill = X }
  M.stats.wedge_new = M.stats.wedge_new + 1
  return set
end
M._centre_wedges = centre_wedges

-- The aim wedges of start pill A: one wedge set per aim point.
local function aim_wedges(ctx, A)
  local inp, env = ctx.inp, ctx.env
  local ts = ctx.tshell
  local reach = ts.s_last + 400
  local o = offsets(reach)
  local boats = env.ts_boats and true or false
  local parts = { boats and "b" or "-", #ctx.aims }
  local found = {}
  for i = 1, #o do
    local mx, my = A.mx + o[i][1], A.my + o[i][2]
    local v = sq_read(ctx, mx, my)
    local k = v[1]
    if k == K_WALL or k == K_HALF or k == K_ALLY or k == K_HOST or (boats and k == K_BOAT) then
      found[#found + 1] = { mx, my, v }
      parts[#parts + 1] = (my * 256 + mx) .. ":" .. k
    end
  end
  local sig = table.concat(parts, ",")
  local key = "a" .. (A.my * 256 + A.mx)
  local e = WC[key]
  if e and e.sig == sig and e.pill == A then
    M.stats.wedge_keep = M.stats.wedge_keep + 1
    return e.sets
  end
  local sets = {}
  for ai, off in ipairs(ctx.aims) do
    local ax, ay = A.mx * 256 + off[1], A.my * 256 + off[2]
    local list = {}
    for _, f in ipairs(found) do
      local v = f[3]
      local w = make_wedge(ax, ay, f[1], f[2], v[1], v[2], v[3])
      if w then list[#list + 1] = w end
    end
    sets[ai] = wedge_set(list)
    sets[ai].ax, sets[ai].ay = ax, ay
  end
  WC[key] = { sig = sig, sets = sets, pill = A }
  M.stats.wedge_new = M.stats.wedge_new + 1
  return sets
end
M._aim_wedges = aim_wedges

-- Shell sample distances (wu from the shooter's centre along its line):
-- the first sample and the last one, from the live or classic rules.
local function shell_range(env, len)
  local speed = env.shell_speed or SH.SHELL_SPEED
  local add = env.shell_start_add or SH.SHELL_START_ADD
  local life = SH.life_ticks(len, env)
  return { s1 = (add + 1) * speed, s_last = (add + life) * speed, step = speed,
           hit_r = env.tank_hit_r or SH.TANK_HIT_RADIUS }
end

-- ── Masks ────────────────────────────────────────────────────────────────
-- wedge state at a point seen from the apex: bearing th, distance d.
-- 2 = surely crossed (a shell sample inside the square) between dmin and
-- dmax along the line; 0 = surely not crossed there; 1 = not sure.
-- dmin / dmax: the part of the line the samples cover (wu from the apex,
-- as on a straight line; SLACK allows for the rounded start and steps).
local function wedge_state(w, th, dmin, dmax)
  local rel = wrap(th - w.c)
  if rel < w.lo_o or rel > w.hi_o then return 0 end
  if w.near > dmax + SLACK or w.far < dmin - SLACK then return 0 end
  dmin, dmax = dmin + SLACK, dmax - SLACK
  -- sure: the line crosses the shrunk square (so its chord in the real
  -- square is at least 68 wu, more than one shell step) and the whole real
  -- square lies in the sampled part of the line
  if rel >= w.lo_i and rel <= w.hi_i and w.near >= dmin and w.far <= dmax then
    return 2
  end
  return 1
end
M._wedge_state = wedge_state

-- CANT(A) at (px, py): true when every aim line is surely blocked.
local function cant(ctx, sets, px, py)
  local ts = ctx.tshell
  local own = floor(py / 256) * 256 + floor(px / 256)
  for ai = 1, #sets do
    local set = sets[ai]
    local dx, dy = px - set.ax, py - set.ay
    local d = sqrt(dx * dx + dy * dy)
    local th = bearing(dx, dy)
    -- distances from the aim point: the shell's samples run from d - s1
    -- down to d - s_last
    local dmax, dmin = d - ts.s1, d - ts.s_last
    local blocked = false
    if dmax > SLACK then
      local bin = set.bins[bin_of(th)]
      for i = 1, #bin do
        local w = bin[i]
        if w.sq ~= own and wedge_state(w, th, dmin, dmax) == 2 then blocked = true; break end
      end
    end
    if not blocked then return false end
  end
  return true
end
M._cant = cant

-- What X's shell meets at (px, py), from X's centre wedges. Returns a table
-- { sure, link, used, cover, front, hit }: link = the first hostile pill,
-- used = no blocker in front of it, cover = shots met before the tank,
-- front = shots before the link, hit = "tank" | "reach". sure = false when
-- a wedge edge is too near the point to say.
local function predict(ctx, set, px, py)
  local ps = ctx.pshell
  local X = set.X
  local sx, sy = pill_centre(X)
  local dx, dy = px - sx, py - sy
  local d = sqrt(dx * dx + dy * dy)
  local th = bearing(dx, dy)
  local out = { sure = true, link = nil, used = false, cover = 0, front = 0, hit = "tank" }
  -- the walk ends at the first sample in the tank circle (about d - hit_r)
  local hit_at = d - ps.hit_r
  if ps.s_last < hit_at - ps.step - SLACK then out.hit = "reach"; hit_at = ps.s_last
  elseif ps.s_last < hit_at + ps.step + SLACK then out.sure = false end
  -- samples before hit_at are surely before the hit; none is after
  -- hit_at + one step
  local dmax_in = hit_at
  local dmax_out = hit_at + ps.step
  local bin = set.bins[bin_of(th)]
  local ins, maybe = {}, {}
  for i = 1, #bin do
    local w = bin[i]
    local s = wedge_state(w, th, ps.s1, dmax_out)
    if s ~= 0 then
      if s == 2 and wedge_state(w, th, ps.s1, dmax_in) == 2 then ins[#ins + 1] = w
      else maybe[#maybe + 1] = w end
    end
  end
  local link = nil
  for _, w in ipairs(ins) do
    if w.kind == K_HOST and (not link or w.near < link.near) then link = w end
  end
  for _, w in ipairs(maybe) do
    if w.kind == K_HOST then
      if not link or w.near <= link.far then out.sure = false end
    else
      out.sure = false
    end
  end
  local cover, front, infront = 0, 0, false
  for _, w in ipairs(ins) do
    if w.kind ~= K_HOST then
      cover = cover + w.shots
      if link then
        if w.far < link.near then
          front = front + w.shots
          -- PT2_LINK_CLEAR_FRONT: only cover (shots > 0) cuts the link
          if w.shots > 0 or not ctx.inp.link_clear_front then infront = true end
        elseif w.near <= link.far then out.sure = false end
      else
        front = front + w.shots
      end
    end
  end
  out.cover, out.front = cover, front
  if link then out.link, out.used = link.pill, not infront end
  return out
end
M._predict = predict

-- The chains from A over MUST, by DFS (length 1 .. max_chain; chain_skip
-- ends a chain; a chain pill must be in range of the point). pts: list of
-- { px, py, T = pills in range }. Returns the chain list { chain, idx }
-- (partial chains too) and, per point, the deepest chain index, and whether
-- every test on the way was sure.
local function chains_dfs(ctx, pts, A)
  local inp = ctx.inp
  local maxc = inp.chains and inp.max_chain or 1
  local out, at, sure = {}, {}, {}
  local function inT(T, X) for _, p in ipairs(T) do if p == X then return true end end return false end
  local function rec(chain, idx)
    out[#out + 1] = { chain = chain, idx = idx }
    local ci = #out
    for _, i in ipairs(idx) do at[i] = ci end
    if #chain >= maxc then return end
    local X = chain[#chain]
    local set = ctx.cw(X)
    local by = {}
    local order = {}
    for _, i in ipairs(idx) do
      local P = pts[i]
      if inT(P.T, X) then
        local pr = predict(ctx, set, P.px, P.py)
        if not pr.sure then sure[i] = false end
        local Y = pr.link
        if Y and pr.used then
          local skip = (inp.chain_skip and inp.chain_skip[Y])
          local again = false
          for _, q in ipairs(chain) do if q == Y then again = true end end
          if not skip and not again then
            if not by[Y] then by[Y] = {}; order[#order + 1] = Y end
            local t = by[Y]; t[#t + 1] = i
          end
        end
      end
    end
    for _, Y in ipairs(order) do
      local c2 = {}
      for k = 1, #chain do c2[k] = chain[k] end
      c2[#c2 + 1] = Y
      rec(c2, by[Y])
    end
  end
  local all = {}
  for i = 1, #pts do all[i] = i; sure[i] = true end
  rec({ A }, all)
  return out, at, sure
end
M._chains_dfs = chains_dfs

-- ── Exact rays ───────────────────────────────────────────────────────────
-- The plain ray of X at (px, py) (no planned blockers), cast once per group
-- run (ctx.rays is the job's table).
local function base_ray(ctx, X, px, py)
  local t = ctx.rays[X]
  if not t then t = {}; ctx.rays[X] = t end
  local k = (py + 4096) * 131072 + (px + 4096)
  local r = t[k]
  if r then ctx.stat.ray_reuse = ctx.stat.ray_reuse + 1; return r end
  local env = ctx.env
  local sx, sy = pill_centre(X)
  local t0 = ctx.clk and ctx.clk()
  r = SH.cast(env, sx, sy, px, py, env.pill_fire_len or SH.PILL_FIRE_LEN, nil)
  if t0 then ctx.tm.cast = ctx.tm.cast + ctx.clk() - t0 end
  r.X = X
  t[k] = r
  ctx.stat.casts = ctx.stat.casts + 1
  return r
end

-- The ray of base ray b with planned blockers `extra` ({[sq] = shots}):
-- the same walk, so only the blockers and the cover change. Gives what
-- shadow.cast(..., extra) gives (test_pt2_v2_strip checks it).
local function ray_view(b, extra)
  local v = {
    X = b.X, angle = b.angle, hit = b.hit, t = b.t, link = b.link, link_t = b.link_t,
    sq = b.sq, sq_t = b.sq_t, n_sq = b.n_sq, end_x = b.end_x, end_y = b.end_y,
    cover = b.cover, front = b.front, block = b.block, n_block = b.n_block,
  }
  if extra then
    local any = false
    local sql = b.sq
    for k = 1, b.n_sq do if extra[sql[k]] then any = true; break end end
    if any then
      local blk, n = {}, 0
      local bi = 1
      local link_t = b.link_t
      local cover, front = 0, 0
      local function add(e)
        n = n + 1; blk[n] = e
        cover = cover + e.shots
        if not link_t or e.t < link_t then front = front + e.shots end
      end
      for k = 1, b.n_sq do
        local s, t = sql[k], b.sq_t[k]
        -- blockers of the base ray up to this square's tick
        while bi <= b.n_block and b.block[bi].t <= t do
          local e = b.block[bi]
          if e.kind == "ally_pill" or not (e.sq == s and extra[s]) then add(e) end
          bi = bi + 1
        end
        local x = extra[s]
        if x then add({ sq = s, shots = x, kind = "planned", t = t }) end
      end
      while bi <= b.n_block do add(b.block[bi]); bi = bi + 1 end
      -- a planned blocker on the link's own square comes after the link
      if link_t then
        front = 0
        for i = 1, n do
          local e = blk[i]
          if e.t < link_t or (e.t == link_t and e.kind == "ally_pill") then front = front + e.shots end
        end
      end
      v.block, v.n_block, v.cover, v.front = blk, n, cover, front
    end
  end
  -- a blocker in front of the link cuts it (the link is not used).
  -- PT2_LINK_CLEAR_FRONT (inp.link_clear_front): only cover in front cuts
  -- it, the old evaluator's test res.front > 0 (eval_point picks v.cut)
  local cut = false
  if v.link then
    for i = 1, v.n_block do
      if v.block[i].t < v.link_t then cut = true; break end
    end
  end
  v.cut_blk = cut
  v.cut_front = (v.link ~= nil and v.front > 0) or false
  v.cut = cut
  return v
end
M._ray_view = ray_view
M._base_ray = base_ray

-- ── One tank point (exact) ────────────────────────────────────────────────
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

-- the old nudge (pilltake_v2_old nudge_aim), on this module's aim list
local function nudge_aim(ctx, px, py, extra, shorts)
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
        local off = ctx.aims[ai]
        local ok, why, ln = SH.tank_shot(env, nx, ny, tx + off[1], ty + off[2], A.mx, A.my, extra)
        if ok then return nx, ny, nw, ai, ln end
        if why == "short" then n = n + 1; live[n] = ai end
      end
      for k = n + 1, #live do live[k] = nil end
      if n == 0 then return nil end
    end
  end
  return nil
end

-- Evaluate point (px, py) for start pill A with planned blockers extra.
-- aim_fixed: only that aim. nudge: PT2_AIM_NUDGE may move the point.
-- exact: cast every ray with extra (no kept ray). Returns a record in the
-- old shape, or nil, why.
local function eval_point(ctx, px, py, extra, aim_fixed, nudge, exact)
  local inp, A, env = ctx.inp, ctx.A, ctx.env
  local T, hasA = pills_in_range(ctx, px, py)
  if not hasA then return nil, "A_OUT_OF_RANGE" end
  env.omx, env.omy = floor(px / 256), floor(py / 256)
  local t0 = ctx.clk and ctx.clk()
  local aim_idx, lane
  local a1, a2 = 1, #ctx.aims
  if aim_fixed then a1, a2 = aim_fixed, aim_fixed end
  local why_out, shorts = "short", nil
  for ai = a1, a2 do
    local off = ctx.aims[ai]
    local ok, why, ln = SH.tank_shot(env, px, py, A.mx * 256 + off[1], A.my * 256 + off[2], A.mx, A.my, extra)
    if ok then aim_idx, lane = ai, ln; break end
    why_out = why
    if why == "short" then shorts = shorts or {}; shorts[#shorts + 1] = ai end
  end
  local nudge_wu = 0
  if not aim_idx and nudge and shorts and inp.aim_nudge and not aim_fixed then
    local nx, ny, nw, nai, nln = nudge_aim(ctx, px, py, extra, shorts)
    if nx then
      -- the nudged point is judged again: A in range there too
      local T2, hasA2 = pills_in_range(ctx, nx, ny)
      if hasA2 then
        px, py, nudge_wu, aim_idx, lane, T = nx, ny, nw, nai, nln, T2
        ctx.stat.nudged = ctx.stat.nudged + 1
      end
      env.omx, env.omy = floor(px / 256), floor(py / 256)
    end
  end
  if t0 then ctx.tm.aim = ctx.tm.aim + ctx.clk() - t0 end
  if not aim_idx then return nil, "OUTGOING:" .. tostring(why_out) end
  -- rays
  local rays = {}
  for i, X in ipairs(T) do
    local v
    if exact then
      local sx, sy = pill_centre(X)
      local t1 = ctx.clk and ctx.clk()
      local r = SH.cast(env, sx, sy, px, py, env.pill_fire_len or SH.PILL_FIRE_LEN, extra)
      if t1 then ctx.tm.cast = ctx.tm.cast + ctx.clk() - t1 end
      r.X = X
      ctx.stat.casts_confirm = ctx.stat.casts_confirm + 1
      v = ray_view(r, nil)
    else
      v = ray_view(base_ray(ctx, X, px, py), extra)
    end
    if inp.link_clear_front then v.cut = v.cut_front end
    rays[i] = v
  end
  local ri = {}
  for i, X in ipairs(T) do ri[X] = i end
  -- chain: A, then each used link, at most max_chain pills
  local chain, in_chain = { A }, { [A] = 1 }
  local chain_mode = false
  if inp.chains then
    local cur = A
    while #chain < inp.max_chain do
      local i = ri[cur]
      local r = i and rays[i]
      local nxt = r and not r.cut and r.link or nil
      if not nxt or in_chain[nxt] or (inp.chain_skip and inp.chain_skip[nxt]) then break end
      chain[#chain + 1] = nxt
      in_chain[nxt] = #chain
      chain_mode = true
      cur = nxt
    end
  end
  -- cover per ray
  local covered_all, uncovered = true, {}
  local need_A, have_A = 0, 0
  local ally_cover = false
  for i, X in ipairs(T) do
    local r = rays[i]
    local bar
    local ci = in_chain[X]
    if X == A and ctx.finish then bar = 0
    elseif X == A and not chain_mode then bar = inp.bar_single
    elseif ci then bar = inp.bar_chain
    else bar = inp.bar_other or inp.wall_shots end
    r.bar = bar
    r.link_cover = (inp.link_covers and ci and not r.cut and r.link ~= nil
                    and r.link == chain[ci + 1]) or false
    local ok
    if ci then ok = r.link_cover or r.cover >= bar
    else ok = (r.hit == "reach") or r.cover >= bar end
    r.ok = ok
    if X == A then need_A, have_A = bar, r.cover end
    if not ok then covered_all = false; uncovered[#uncovered + 1] = i end
    if ok and (r.hit ~= "reach" or ci) then
      for k = 1, r.n_block do if r.block[k].kind == "ally_pill" then ally_cover = true end end
    end
  end
  local last = chain[#chain]
  local depth = rays[ri[last]] and rays[ri[last]].cover or 0
  return {
    px = px, py = py, nudge_wu = nudge_wu, T = T, rays = rays, ri = ri, chain = chain, in_chain = in_chain,
    chain_mode = chain_mode, aim_idx = aim_idx, lane = lane, clear_aims = { aim_idx },
    lanes = { [aim_idx] = lane }, aims_done = aim_fixed or aim_idx, extra_in = extra,
    covered = covered_all, uncovered = uncovered,
    safe = covered_all, max_ok = true,
    kills = chain_mode and #chain or 1,
    ally_cover = ally_cover, need = need_A, have = have_A, depth = depth,
    finish = ctx.finish or false,
  }
end
M._eval_point = eval_point

-- Every clear aim of a record (as the old clear_aims_of).
local function clear_aims_of(ctx, rec)
  if rec.aims_done < #ctx.aims then
    local env, A = ctx.env, ctx.A
    env.omx, env.omy = floor(rec.px / 256), floor(rec.py / 256)
    local t0 = ctx.clk and ctx.clk()
    for ai = rec.aims_done + 1, #ctx.aims do
      local off = ctx.aims[ai]
      local ok, _, ln = SH.tank_shot(env, rec.px, rec.py, A.mx * 256 + off[1], A.my * 256 + off[2],
                                     A.mx, A.my, rec.extra_in)
      if ok then rec.clear_aims[#rec.clear_aims + 1] = ai; rec.lanes[ai] = ln end
    end
    if t0 then ctx.tm.aim = ctx.tm.aim + ctx.clk() - t0 end
    rec.aims_done = #ctx.aims
  end
  return rec.clear_aims
end
M._clear_aims_of = clear_aims_of

-- ── Wall search ───────────────────────────────────────────────────────────
-- old slot_ok: may a planned blocker go on square k of ray r?
local function slot_ok(ctx, rec, r, k, lane_set, forbid)
  local inp = ctx.inp
  local sq = r.sq[k]
  local mx, my = sq % 256, floor(sq / 256)
  local X = r.X
  if lane_set[sq] then return false, "lane" end
  if forbid[sq] then return false, "before_link" end
  if r.link and r.link_t and r.sq_t[k] <= r.link_t then return false, "before_link" end
  if inp.orth_only and X == ctx.A and abs(mx - X.mx) + abs(my - X.my) ~= 1 then return false, "not_orth" end
  if not inp.in_map(mx, my) or not inp.stay_in(mx, my) then return false, "area" end
  local cx, cy = mx * 256 + 128, my * 256 + 128
  local dxp, dyp = cx - rec.px, cy - rec.py
  if dxp * dxp + dyp * dyp < (inp.blocker_min_dist * 256) ^ 2 then return false, "near_tank" end
  local sx, sy = pill_centre(X)
  local d1 = (cx - sx) ^ 2 + (cy - sy) ^ 2
  local d2 = (rec.px - sx) ^ 2 + (rec.py - sy) ^ 2
  if d1 >= d2 then return false, "behind_tank" end
  if ctx.env.pill_on(mx, my) then return false, "pill" end
  local tt = ctx.env.ttype(mx, my)
  if tt == ctx.env.T_BUILDING or tt == ctx.env.T_HALFBUILD then return false, "wall" end
  if not inp.buildable(mx, my) then return false, "not_buildable" end
  local rc = ctx.reach_cache[sq]
  if rc == nil then
    rc = inp.lgm_reach(rec.omx, rec.omy, mx, my) and true or false
    ctx.reach_cache[sq] = rc
  end
  if not rc then return false, "lgm_unreachable" end
  return true
end

-- The builder's shortest trip (squares) from (ox, oy) through the squares
-- of `set` (at most PT2_BUILD_MAX, so every order is tried). Returns the
-- length and the order.
local PERMS = {
  [1] = { { 1 } },
  [2] = { { 1, 2 }, { 2, 1 } },
  [3] = { { 1, 2, 3 }, { 1, 3, 2 }, { 2, 1, 3 }, { 2, 3, 1 }, { 3, 1, 2 }, { 3, 2, 1 } },
}
local function perms(n)
  if PERMS[n] then return PERMS[n] end
  local out = {}
  local function gen(a, k)
    if k > n then local c = {}; for i = 1, n do c[i] = a[i] end; out[#out + 1] = c; return end
    for i = k, n do a[k], a[i] = a[i], a[k]; gen(a, k + 1); a[k], a[i] = a[i], a[k] end
  end
  local a = {}
  for i = 1, n do a[i] = i end
  gen(a, 1)
  PERMS[n] = out
  return out
end
local function trip(ox, oy, sqs)
  local best, border = math.huge, nil
  for _, pm in ipairs(perms(#sqs)) do
    local x, y, len = ox, oy, 0
    for _, i in ipairs(pm) do
      local s = sqs[i]
      local mx, my = s % 256, floor(s / 256)
      len = len + sqrt((mx - x) ^ 2 + (my - y) ^ 2)
      x, y = mx, my
    end
    if len < best then best, border = len, pm end
  end
  return best, border
end
M._trip = trip

-- The wall plan for record rec on aim ai (lane). Returns the best solution
-- { k, depth, two, trip, items = { {sq, kind, shots} } in build order } or nil.
-- Every set of up to build_max legal squares on the uncovered paths is
-- tried (a DFS on the first path still short of its bar, so each set that
-- covers every path is met); the first size with a solution wins.
local function search_walls(ctx, rec, ai)
  local inp = ctx.inp
  local lane = rec.lanes[ai] or {}
  local lane_set = {}
  for _, s in ipairs(lane) do lane_set[s] = true end
  -- the same for every aim, kept on the record: the squares in front of a
  -- used chain link (a blocker there cuts the chain), and the uncovered
  -- paths (need, existing shots per square)
  local base = rec.wall_base
  if not base then
    local forbid = {}
    for i, X in ipairs(rec.T) do
      local r = rec.rays[i]
      if rec.in_chain[X] and r.link and not r.cut and rec.in_chain[r.link] then
        for k = 1, r.n_sq do
          if r.sq_t[k] <= r.link_t then forbid[r.sq[k]] = true end
        end
      end
    end
    local UB = {}
    for _, ui in ipairs(rec.uncovered) do
      local r = rec.rays[ui]
      local ex = {}
      for b = 1, r.n_block do
        local e = r.block[b]
        if e.kind ~= "ally_pill" then ex[e.sq] = e.shots end
      end
      local on = {}
      for k = 1, r.n_sq do on[r.sq[k]] = ex[r.sq[k]] or 0 end
      UB[#UB + 1] = { r = r, need = r.bar - r.cover, on = on }
    end
    base = { forbid = forbid, U = UB }
    rec.wall_base = base
  end
  local forbid = base.forbid
  local U = {}
  for j, ub in ipairs(base.U) do U[j] = { r = ub.r, need = ub.need, on = ub.on, cand = {} } end
  -- legal squares: old's slot rules pass on some uncovered path the
  -- square is on. All but the lane rule are the same for every aim, so
  -- they are kept on the record (rec.slot_memo).
  local memo = rec.slot_memo
  if not memo then memo = {}; rec.slot_memo = memo end
  local nolane = {}
  local legal = {}
  for _, u in ipairs(U) do
    local r = u.r
    for k = 1, r.n_sq do
      local s = r.sq[k]
      local l = legal[s]
      if l == nil then
        if lane_set[s] then
          l = false
        else
          l = memo[s]
          if l == nil then
            l = false
            for _, u2 in ipairs(U) do
              if u2.on[s] then
                local r2 = u2.r
                for k2 = 1, r2.n_sq do
                  if r2.sq[k2] == s then
                    if slot_ok(ctx, rec, r2, k2, nolane, forbid) then l = true end
                    break
                  end
                end
              end
              if l then break end
            end
            memo[s] = l
          end
        end
        legal[s] = l
      end
      if l then u.cand[#u.cand + 1] = s end
    end
  end
  local npb = inp.num_pill_blockers or 0
  local sols = nil
  local rem = {}
  for j, u in ipairs(U) do rem[j] = u.need end
  local chosen, kinds = {}, {}
  local seen = {}
  local nodes = 0
  local function dfs(pl, wl)
    nodes = nodes + 1
    local j = nil
    for jj = 1, #U do if rem[jj] > 0 then j = jj; break end end
    if not j then
      if pl == 0 and wl == 0 then
        -- a full set: record it once
        local ks = {}
        for i = 1, #chosen do ks[i] = chosen[i] * 2 + (kinds[i] == "pill" and 1 or 0) end
        table.sort(ks)
        local key = table.concat(ks, ",")
        if not seen[key] then
          seen[key] = true
          local it = {}
          for i = 1, #chosen do it[i] = { sq = chosen[i], kind = kinds[i] } end
          sols[#sols + 1] = it
        end
      end
      return
    end
    if pl + wl == 0 then return end
    -- bound: a path still short gets at most min(free squares, pl) pills
    -- and walls for the rest; when that is not enough, no set here covers
    for jj = 1, #U do
      local need = rem[jj]
      if need > 0 then
        local free = 0
        for _, s in ipairs(U[jj].cand) do
          local used = false
          for i = 1, #chosen do if chosen[i] == s then used = true; break end end
          if not used then free = free + 1 end
        end
        local np = min(free, pl)
        local nw = min(free - np, wl)
        if np * inp.pill_drop_shots + nw * inp.wall_shots < need then return end
      end
    end
    for _, s in ipairs(U[j].cand) do
      local used = false
      for i = 1, #chosen do if chosen[i] == s then used = true; break end end
      if not used then
        for pass = 1, 2 do
          local kind, shots
          if pass == 1 and pl > 0 then kind, shots = "pill", inp.pill_drop_shots
          elseif pass == 2 and wl > 0 then kind, shots = "wall", inp.wall_shots end
          if kind then
            chosen[#chosen + 1] = s; kinds[#kinds + 1] = kind
            for jj = 1, #U do
              local e = U[jj].on[s]
              if e then rem[jj] = rem[jj] - (shots - e) end
            end
            dfs(kind == "pill" and pl - 1 or pl, kind == "wall" and wl - 1 or wl)
            for jj = 1, #U do
              local e = U[jj].on[s]
              if e then rem[jj] = rem[jj] + (shots - e) end
            end
            chosen[#chosen] = nil; kinds[#kinds] = nil
          end
        end
      end
    end
  end
  local kfound = nil
  for k = 1, inp.build_max do
    sols = {}
    local p = min(npb, k)
    dfs(p, k - p)
    if #sols > 0 then kfound = k; break end
  end
  ctx.stat.wall_nodes = ctx.stat.wall_nodes + nodes
  if not kfound then return nil end
  -- pick: more depth, then a square on two paths, then the shortest trip
  local last = rec.chain[#rec.chain]
  local lr = rec.rays[rec.ri[last]]
  local lon = {}
  if lr then
    local ex = {}
    for b = 1, lr.n_block do
      local e = lr.block[b]
      if e.kind ~= "ally_pill" then ex[e.sq] = e.shots end
    end
    for k = 1, lr.n_sq do lon[lr.sq[k]] = ex[lr.sq[k]] or 0 end
  end
  local best = nil
  for _, it in ipairs(sols) do
    local depth = lr and lr.cover or 0
    local two = false
    local sqs = {}
    for i, e in ipairs(it) do
      e.shots = e.kind == "pill" and inp.pill_drop_shots or inp.wall_shots
      if lon[e.sq] then depth = depth + e.shots - lon[e.sq] end
      local n = 0
      for _, u in ipairs(U) do if u.on[e.sq] then n = n + 1 end end
      if n >= 2 then two = true end
      sqs[i] = e.sq
    end
    local tl, order = trip(rec.omx or floor(rec.px / 256), rec.omy or floor(rec.py / 256), sqs)
    local key = {}
    for i = 1, #it do key[i] = it[i].sq * 2 + (it[i].kind == "pill" and 1 or 0) end
    table.sort(key)
    local cand = { k = kfound, depth = depth, two = two, trip = tl, order = order, items = it,
                   key = table.concat(key, ",") }
    local better = false
    if not best then better = true
    elseif cand.depth ~= best.depth then better = cand.depth > best.depth
    elseif cand.two ~= best.two then better = cand.two
    elseif abs(cand.trip - best.trip) > 1e-9 then better = cand.trip < best.trip
    else better = cand.key < best.key end
    if better then best = cand end
  end
  local items = {}
  for i, oi in ipairs(best.order) do items[i] = best.items[oi] end
  best.items = items
  best.ai = ai
  return best
end
M._search_walls = search_walls

-- Plan blockers for rec: every clear aim, the best plan wins (covered,
-- fewer blockers, more depth, a square on two paths, shorter trip). The
-- plan is confirmed with exact casts. Returns the final record.
local function plan_point(ctx, rec)
  if rec.covered then rec.plan, rec.extra = {}, {}; return rec end
  local t0 = ctx.clk and ctx.clk()
  local best = nil
  for _, ai in ipairs(clear_aims_of(ctx, rec)) do
    local s = search_walls(ctx, rec, ai)
    if s then
      local b = false
      if not best then b = true
      elseif s.k ~= best.k then b = s.k < best.k
      elseif s.depth ~= best.depth then b = s.depth > best.depth
      elseif s.two ~= best.two then b = s.two
      elseif abs(s.trip - best.trip) > 1e-9 then b = s.trip < best.trip end
      if b then best = s end
    end
  end
  if t0 then ctx.tm.walls = ctx.tm.walls + ctx.clk() - t0 end
  ctx.stat.planned = ctx.stat.planned + 1
  if best then
    local extra, plan = {}, {}
    for i, e in ipairs(best.items) do
      extra[e.sq] = e.shots
      plan[i] = { sq = e.sq, mx = e.sq % 256, my = floor(e.sq / 256), shots = e.shots, kind = e.kind }
    end
    local t1 = ctx.clk and ctx.clk()
    local f = eval_point(ctx, rec.px, rec.py, extra, best.ai, false, true)
    if t1 then ctx.tm.confirm = ctx.tm.confirm + ctx.clk() - t1 end
    local same = f and #f.chain == #rec.chain
    if same then for i = 1, #f.chain do if f.chain[i] ~= rec.chain[i] then same = false end end end
    if f and f.covered and same then
      f.nudge_wu = rec.nudge_wu
      f.plan, f.extra = plan, extra
      f.omx, f.omy = rec.omx, rec.omy
      f.clear_aims, f.lanes = rec.clear_aims, rec.lanes
      return f
    end
    ctx.stat.confirm_fail = ctx.stat.confirm_fail + 1
  end
  rec.plan, rec.extra = {}, {}
  return rec
end
M._plan_point = plan_point

-- Count the 8 neighbours at ring_wu that also pass with the same plan
-- (the old leeway, on this module's point rules).
local NB = {}
for k = 0, 7 do
  local a = k * math.pi / 4
  NB[k + 1] = { cos(a), sin(a) }
end
local function leeway(ctx, rec, ring_wu)
  local n, res = 0, {}
  local t0 = ctx.clk and ctx.clk()
  for k = 1, 8 do
    local qx = floor(rec.px + NB[k][1] * ring_wu + 0.5)
    local qy = floor(rec.py + NB[k][2] * ring_wu + 0.5)
    local q, why = eval_point(ctx, qx, qy, rec.extra, nil, false, false)
    local ok = q and q.covered and q.max_ok and (#q.chain == #rec.chain)
    if ok then n = n + 1 end
    if not ok and q then
      for _, ui in ipairs(q.uncovered) do why = "OVERLAP:" .. OLD.pill_name(ctx.inp, q.rays[ui].X); break end
      if not why and #q.chain ~= #rec.chain then why = "CHAIN" end
    end
    res[k] = { x = qx, y = qy, ok = ok and true or false, why = (not ok) and (why or "?") or nil }
  end
  if t0 then ctx.tm.leeway = ctx.tm.leeway + ctx.clk() - t0 end
  return n, res
end

-- ── evaluate ──────────────────────────────────────────────────────────────
local function new_stat()
  return { sq_reads = 0, casts = 0, casts_confirm = 0, ray_reuse = 0, nudged = 0, planned = 0,
           confirm_fail = 0, wall_nodes = 0, cant = 0, survive = 0, lone = 0, plan_skipped = 0,
           loss_pass = 0, loss_safe = 0, chain_agree = 0, chain_differ = 0, chain_unsure = 0 }
end
local function new_tm()
  return { wedges = 0, masks = 0, cast = 0, aim = 0, walls = 0, confirm = 0, leeway = 0,
           score_fn = 0, total = 0 }
end

-- The job cache: rays and square reads kept for one group run.
local function new_cache() return { rays = {}, sqm = {} } end

local function new_ctx(inp, A)
  local cache = inp._v2_cache or new_cache()
  local ctx = { inp = inp, A = A, env = inp.env, reach_cache = {}, hostile = {},
                rays = cache.rays, sqm = cache.sqm, aims = M.AIMS,
                stat = new_stat(), tm = new_tm(), clk = inp.v2_clock }
  for _, p in ipairs(inp.pills) do
    if alive(p) and not inp.env.is_ally(p) then ctx.hostile[#ctx.hostile + 1] = p end
  end
  ctx.finish = OLD.finish_ok(inp, A)
  ctx.tshell = shell_range(inp.env, SH.TANK_FIRE_LEN)
  ctx.pshell = shell_range(inp.env, inp.env.pill_fire_len or SH.PILL_FIRE_LEN)
  local cws = {}
  ctx.cw = function(X)
    local s = cws[X]
    if not s then
      local t0 = ctx.clk and ctx.clk()
      s = centre_wedges(ctx, X)
      if t0 then ctx.tm.wedges = ctx.tm.wedges + ctx.clk() - t0 end
      cws[X] = s
    end
    return s
  end
  return ctx
end
M._new_ctx = new_ctx
M._new_cache = new_cache

function M.evaluate(inp)
  local cf = inp.capture_file
  if cf and cf ~= "" then return require("pt2_capture").evaluate(M, inp, cf) end
  return M.evaluate_lua(inp)
end

local function pre_rank(a, b)
  local ka, kb = a.kills, b.kills
  if ka ~= kb then return ka > kb end
  if a.covered ~= b.covered then return a.covered end
  return a.base < b.base
end

function M.evaluate_lua(inp)
  local A = inp.A
  local ctx = new_ctx(inp, A)
  local clk = ctx.clk
  local T0 = clk and clk()
  local pcx, pcy = A.mx + 0.5, A.my + 0.5
  local all, best = {}, nil
  local n_eval, n_reject = 0, 0
  local st, tm = ctx.stat, ctx.tm
  local t0 = clk and clk()
  local asets = aim_wedges(ctx, A)
  if t0 then tm.wedges = tm.wedges + clk() - t0 end
  local cap, band = inp.leeway_min, inp.travel_band
  for _, roff in ipairs(inp.radii_offsets) do
    local R = inp.radius + roff
    local cands = {}
    local mpts = {}
    local nsteps = floor(360 / inp.angle_step + 0.5)
    for s = 0, nsteps - 1 do
      local deg = s * inp.angle_step
      if not (inp.banned and inp.banned(deg)) then
        local cx = pcx + sin(rad(deg)) * R
        local cy = pcy - cos(rad(deg)) * R
        local mx, my = floor(cx), floor(cy)
        if inp.in_map(mx, my) and inp.stay_in(mx, my) and inp.standable(mx, my) then
          n_eval = n_eval + 1
          local px, py = floor(cx * 256), floor(cy * 256)
          local T, hasA = pills_in_range(ctx, px, py)
          local rec, why
          if not hasA then
            why = "A_OUT_OF_RANGE"
          else
            local t1 = clk and clk()
            local is_cant = cant(ctx, asets, px, py)
            if t1 then tm.masks = tm.masks + clk() - t1 end
            if is_cant then
              st.cant = st.cant + 1
              why = "CANT"
              if inp.v2_loss_check then
                local q = eval_point(ctx, px, py, nil, nil, true, false)
                if q then
                  st.loss_pass = st.loss_pass + 1
                  q.omx, q.omy = floor(cx + sin(rad(deg)) * inp.approach_offset),
                                 floor(cy - cos(rad(deg)) * inp.approach_offset)
                  local f = plan_point(ctx, q)
                  if f.covered then st.loss_safe = st.loss_safe + 1 end
                end
              end
            else
              st.survive = st.survive + 1
              rec, why = eval_point(ctx, px, py, nil, nil, true, false)
              if rec then mpts[#mpts + 1] = { px = px, py = py, T = T, rec = rec } end
            end
          end
          if rec then
            if rec.nudge_wu > 0 then
              cx, cy = rec.px / 256, rec.py / 256
              mx, my = floor(cx), floor(cy)
            end
            rec.deg, rec.cx, rec.cy, rec.mx, rec.my, rec.radius = deg, cx, cy, mx, my, R
            local t2 = clk and clk()
            rec.base = inp.score_fn(deg, mx, my, cx, cy)
            if t2 then tm.score_fn = tm.score_fn + clk() - t2 end
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
    end
    -- the masks' chains at the kept points (MUST over the DFS), set beside
    -- the exact chain for the logs and the bench
    if inp.v2_mask_stats and #mpts > 0 then
      local t1 = clk and clk()
      local list, at, sure = chains_dfs(ctx, mpts, A)
      for i, P in ipairs(mpts) do
        local mc = list[at[i]].chain
        P.rec.mask_chain = mc
        if not sure[i] then st.chain_unsure = st.chain_unsure + 1
        else
          local same = #mc == #P.rec.chain
          if same then for k = 1, #mc do if mc[k] ~= P.rec.chain[k] then same = false end end end
          if same then st.chain_agree = st.chain_agree + 1 else st.chain_differ = st.chain_differ + 1 end
        end
      end
      if t1 then tm.masks = tm.masks + clk() - t1 end
    end
    table.sort(cands, pre_rank)
    -- lone pill: no other hostile pill in range of any point
    local lone = #cands > 0
    for _, rec in ipairs(cands) do if #rec.T ~= 1 then lone = false; break end end
    local scored = {}
    local function score_of(fin)
      fin.n_built = #fin.plan
      fin.blk = (inp.own_pill_rank and fin.ally_cover) and 2 or 1
      fin.score = fin.base + fin.n_built * inp.build_cost
    end
    local function carry(fin, rec)
      fin.deg, fin.cx, fin.cy, fin.mx, fin.my, fin.radius = rec.deg, rec.cx, rec.cy, rec.mx, rec.my, rec.radius
      fin.base = rec.base
    end
    if lone and band and band > 0 then
      st.lone = st.lone + 1
      -- the points that stand covered now need no plan; the others are
      -- planned in base order until none left can reach the travel band
      local m = nil
      local need = {}
      for _, rec in ipairs(cands) do
        if rec.covered then
          rec.plan, rec.extra = {}, {}
          score_of(rec)
          if rec.safe and (not m or rec.score < m) then m = rec.score end
        else
          need[#need + 1] = rec
        end
      end
      local order = {}
      for i, rec in ipairs(need) do order[i] = rec end
      table.sort(order, function(a, b)
        if a.base ~= b.base then return a.base < b.base end
        return a.deg < b.deg
      end)
      local done = {}
      for _, rec in ipairs(order) do
        if m and rec.base > m + band then break end
        local fin = plan_point(ctx, rec)
        carry(fin, rec)
        score_of(fin)
        done[rec] = fin
        if fin.safe and (not m or fin.score < m) then m = fin.score end
      end
      for _, rec in ipairs(cands) do
        local fin = rec.covered and rec or done[rec]
        if not fin then
          fin = rec
          fin.plan, fin.extra, fin.unsearched = {}, {}, true
          score_of(fin)
          st.plan_skipped = st.plan_skipped + 1
        end
        scored[#scored + 1] = fin
      end
    else
      for _, rec in ipairs(cands) do
        local fin = plan_point(ctx, rec)
        carry(fin, rec)
        score_of(fin)
        scored[#scored + 1] = fin
      end
    end
    OLD.set_band(scored, band)
    table.sort(scored, function(a, b) return M.better(a, b, cap, band, inp.prefer_existing_cover) end)
    if inp.leeway_on then
      for i = 1, min(inp.leeway_top, #scored) do
        local rec = scored[i]
        rec.leeway, rec.leeway_pts = leeway(ctx, rec, inp.leeway_ring_wu)
        if rec.safe and rec.kills >= 2 and rec.leeway < inp.leeway_min then
          rec.safe, rec.why_unsafe = false, "LEEWAY"
        end
      end
      OLD.set_band(scored, band)
      table.sort(scored, function(a, b) return M.better(a, b, cap, band, inp.prefer_existing_cover) end)
    end
    for _, rec in ipairs(scored) do all[#all + 1] = rec end
    if scored[1] and (not best or M.better(scored[1], best, cap, band, inp.prefer_existing_cover)) then best = scored[1] end
    if best and best.safe then break end
  end
  if best and best.safe and best.plan and #best.plan > 0 then
    best.hold = OLD.hold_point(ctx, best)
  end
  if T0 then tm.total = clk() - T0 end
  return { best = best, all = inp.keep_all and all or nil, n_eval = n_eval, n_reject = n_reject,
           hostile = ctx.hostile, inp = inp, v2 = { stat = st, tm = tm } }
end

-- ── evaluate_group (as the old one; rays and square reads kept per job) ──
function M.evaluate_group(inp)
  local job = M.group_begin(inp)
  while not M.group_step(job) do end
  return job.res
end

function M.group_begin(inp)
  return { inp = inp, goalA = inp.A, list = nil, i = 1, res = nil, cache = new_cache() }
end

function M.group_parts_left(job)
  if not job.list then return 1 end
  return #job.list - job.i + 1
end

local function eval_in_job(job, inp)
  local old = inp._v2_cache
  inp._v2_cache = job.cache
  local ok, r = pcall(M.evaluate, inp)
  inp._v2_cache = old
  if not ok then error(r, 0) end
  return r
end

function M.group_step(job)
  local inp, goalA = job.inp, job.goalA
  if not job.list then
    local res = eval_in_job(job, inp)
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
    if inp.set_A then inp.set_A(p) end
    local ok, r = pcall(eval_in_job, job, inp)
    inp.A = goalA
    if inp.set_A then inp.set_A(goalA) end
    if not ok then error(r, 0) end
    job.n_eval, job.n_reject = job.n_eval + r.n_eval, job.n_reject + r.n_reject
    local b, res = r.best, job.res
    if b and b.safe and b.chain_mode and b.in_chain and b.in_chain[goalA]
       and (not res.best or M.better(b, res.best, inp.leeway_min, inp.travel_band, inp.prefer_existing_cover)) then
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

-- v2's tests on a given point and plan: the first failing one, "-" when
-- it passes them all (the old judge, on this module's point rules).
function M.judge(inp, px, py, extra, aim_idx)
  local ctx = new_ctx(inp, inp.A)
  local rec, why = eval_point(ctx, px, py, extra, aim_idx, false, false)
  if not rec then return why, nil end
  for _, ui in ipairs(rec.uncovered) do
    local r = rec.rays[ui]
    if r.X == inp.A then return string.format("DEPTH need=%d have=%d", r.bar, r.cover), rec end
    return "OVERLAP:" .. OLD.pill_name(inp, r.X), rec
  end
  return "-", rec
end

function M.leeway_at(inp, px, py, extra)
  local ctx = new_ctx(inp, inp.A)
  local rec = eval_point(ctx, px, py, extra, nil, false, false)
  if not rec then return 0 end
  rec.extra = extra
  return (leeway(ctx, rec, inp.leeway_ring_wu))
end

return M
