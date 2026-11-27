-- =========================================================================
-- pilltake_v2.lua -- the "strip" pill take evaluator (2026-11-23, Andrew's
-- design). PT2_EVALUATOR = "v2" picks it; "old" picks pilltake_v2_old.lua.
--
-- The result has the same shape as the old one (evaluate, group_step), and
-- the rank is the old M.better (plus PT2_PREFER_EXISTING_COVER), so pt2_attack, pt2_dbl and the rest only
-- choose the evaluator through pt2_eval.lua.
--
--  1. Tank points: the old rings (radii_offsets, PT2_ANGLE_STEP_DEG), the
--     old in_map / stay_in / standable / banned filters; with
--     PT2_FARTHEST_RING every ring of ring_radii, the farther ring first
--     in the rank after safe, kills and the aim rule. PT2_AIM_NUDGE
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
--     PT2_AIM_AVOID_CHAIN_NEIGHBOUR, an aim beside a chain member last of
--     the same safe and kills; with PT2_PREFER_EXISTING_COVER, fewer
--     planned blockers first in the band.
--  9. A lone pill (no other hostile pill in range of any point): the points
--     are planned in travel score order, and planning stops once no point
--     left can come into the travel band (the best is the same).
-- =========================================================================

local SH  = require("shadow")
local OLD = require("pilltake_v2_old")
local PS  = require("pt2_start")

local M = {}
-- to_scan_result, world_env, world_inp, live_shell, tank_blockers,
-- pill_name, finish_ok, set_band, hold_point, AIMS: the old module's.
setmetatable(M, { __index = OLD })

-- PT2_FARTHEST_RING: is it on for this inp (the flag and a ring list)?
function M.far_on(inp)
  return (inp.farthest_ring and inp.ring_radii and #inp.ring_radii > 0) and true or false
end

-- The old knobs plus v2's own.
function M.knob_inp(C, inp)
  inp = OLD.knob_inp(C, inp)
  -- PT2_PREFER_EXISTING_COVER (E1)
  inp.prefer_existing_cover = C.PT2_PREFER_EXISTING_COVER ~= false
  -- PT2_AIM_POINTS (E3): 9 = M.AIMS9, else the old 5
  inp.aim_points = C.PT2_AIM_POINTS or 5
  -- PT2_AIM_AVOID_CHAIN_NEIGHBOUR (Nov 24): an aim beside a chain member last
  inp.aim_avoid_nb = C.PT2_AIM_AVOID_CHAIN_NEIGHBOUR ~= false
  -- PT2_FAVOR_BLOCKER_PLACE (Nov 24): the blocker pill tier and the walls
  -- nearer the tank, after safe and kills (inp.carried_pills: build_inp)
  inp.favor_blocker_place = C.PT2_FAVOR_BLOCKER_PLACE ~= false
  -- PT2_FARTHEST_RING (Nov 24): every ring of ring_radii (absolute, far
  -- to near), the farther ring first in the rank
  inp.farthest_ring = C.PT2_FARTHEST_RING ~= false
  local rr = C.PT2_FARTHEST_RING_RADII or { 7.4, 7.2, 7.0, 6.6 }
  inp.ring_radii = {}
  for i, r in ipairs(rr) do inp.ring_radii[i] = r end
  -- PT2_START_WHILE_BUILDING (E4); anger_bump: world.lua's anger per hit
  inp.start_while_building = C.PT2_START_WHILE_BUILDING ~= false
  inp.anger_bump = C.PILL_ANGER_BUMP or 0.3333
  -- PT2_START_ANGER_RISE: the start check walks the pills' reload ticks
  inp.start_anger_rise = C.PT2_START_ANGER_RISE ~= false
  -- PT2_EXTRA_COVER_WALLS (E5): 0 = off
  inp.extra_cover_walls = C.PT2_EXTRA_COVER_WALLS or 1
  -- PT2_EXTRA_COVER_SKIP_PILL_LINE: no extra wall on a ray a friendly
  -- pill already blocks
  inp.xcover_skip_pill_line = C.PT2_EXTRA_COVER_SKIP_PILL_LINE ~= false
  -- BLOCKER_MIN_LANE_BRAD: attack.lua's shield lane check (aim_wide)
  inp.min_lane_brad = C.BLOCKER_MIN_LANE_BRAD or 0
  -- PT2_QUICK_SIDE_FIRST (Nov 24): the place keys only inside the travel band
  inp.quick_side_first = C.PT2_QUICK_SIDE_FIRST ~= false
  -- PT2_MIRROR_SAFE_TIE (Nov 24): S0 nearer the tank, then north, then angle
  -- (inp.tank_wx, inp.tank_wy: build_inp)
  inp.mirror_tie = C.PT2_MIRROR_SAFE_TIE ~= false
  -- PT2_CHAINS_ONLY (S10): the runs that only a safe chain can use (a
  -- non-goal A in group_step, the pt2_dbl shadow runs) plan no single take
  inp.chains_only_runs = C.PT2_CHAINS_ONLY ~= false
  -- PT2_WALL_MEMO (M1): the wall searches' memo across evaluates
  inp.wall_memo = C.PT2_WALL_MEMO ~= false
  inp.wall_memo_max = C.PT2_WALL_MEMO_MAX or 2000
  -- PT2_V2_C: run the C copy (gh_pt2_v2.c) when the exe has it
  inp.v2_c = C.PT2_V2_C ~= false
  return inp
end

-- PT2_AIM_POINTS = 9 (Nov 23, Andrew's yes, E3): the old 5 aims (centre,
-- then the corners at inset 24) and the 4 edge midpoints at inset 24, in
-- the order top, right, bottom, left. The tank_shot tests and the CANT
-- masks go through this list (ctx.aims); aim_idx 6..9 are the midpoints.
M.AIMS9 = {}
for i, a in ipairs(OLD.AIMS) do M.AIMS9[i] = a end
M.AIMS9[6] = { 128, 24 }
M.AIMS9[7] = { 231, 128 }
M.AIMS9[8] = { 128, 231 }
M.AIMS9[9] = { 24, 128 }

-- The rank: the old better, and PT2_PREFER_EXISTING_COVER (Nov 23,
-- Andrew's yes, E1). With pec on, two points of the same safe and kills
-- that are both in the travel band rank by n_built first (fewer planned
-- blockers win; a dropped pill counts as one), before depth, blk and
-- leeway. Outside the band, the old order stands.
--
-- PT2_START_WHILE_BUILDING (Nov 23, Andrew's yes, E4): swb on, the same
-- two points (same safe and kills, both in the band) rank next by
-- start_now: a point the tank may start while the walls go up ranks above
-- one where it must wait. After n_built (E1), before depth.
--
-- PT2_AIM_AVOID_CHAIN_NEIGHBOUR (Nov 24, Andrew's yes): two points of the
-- same safe and kills rank next by aimnb: a point whose aim is beside a
-- chain member (aim_nb: another chain pill, or a blocker on a chain
-- pill's ray, that touches the aim's corner or edge) comes after one whose
-- aim is not. In or out of the band, before E1 and E4. A preference only:
-- with every point beside, the best of them still wins. aimnb is nil with
-- the knob off (the old order).
--
-- PT2_FAVOR_BLOCKER_PLACE (Nov 24, Andrew's yes): two points of the same
-- safe and kills rank next by bp_tier (lower wins), then by wall_far
-- (smaller wins), before aimnb (blocker_place below). Both are nil with
-- the knob off (the old order).
--
-- PT2_FARTHEST_RING (Nov 24, Andrew's yes): far on (M.far_on: the knob
-- and a ring_radii list), two points of the same safe and kills (and
-- aimnb) rank next by ring: the farther ring wins. Before E1, E4 and the
-- travel band. A farther ring never beats more kills or a safe point.
--
-- no_s0: skip the S0 tie-break; a full tie is then false (b stays). The
-- group step passes it (Nov 23, Andrew approved Nov 23): on a full tie
-- between plans with different first pills, the plan it has (the goal
-- pill's own, run first) wins, as before S0.
--
-- opt: the inp (or nil), for the two knobs below.
--
-- PT2_QUICK_SIDE_FIRST (Nov 24, Andrew: "don't drive all the way over,
-- it's a waste of time"): opt.quick_side_first on and a travel band, the
-- keys above (blocker tier, walls, aimnb, ring) rank two points of the
-- same safe and kills only when both are in the travel band (as E1 and
-- E4 below). A slower plan (a far side after a ban or a lane fail) wins
-- only on travel: when its travel and build score is lower.
--
-- PT2_MIRROR_SAFE_TIE (Nov 24, Andrew: a mirrored setup gives a mirrored
-- plan): opt.mirror_tie on, S0 ranks first by the distance from the tank
-- (opt.tank_wx, opt.tank_wy) to the point (nearer wins), then by py
-- (smaller wins: north first), and only then by the angle. Distance and
-- py stay the same when the map is mirrored east to west; the angle does
-- not (the smaller angle is north of the pills on the east side, south
-- of them on the west side).
--
-- floor (PT2_QUICK_SIDE_FIRST, Nov 24 review): the lowest score seen so
-- far for a's safe and kills, over every list compared before (the rings
-- of one run, the runs of a group). The band is measured from it too, so
-- a plan can not move up one band per ring or per first pill.
--
-- PT2_MIRROR_SAFE_TIE, more (Nov 24 review):
--   * the point of the last plan (opt.prev_px, opt.prev_py) wins first,
--     so a replan in the same tie does not move the point as the tank
--     moves (many ring points share one travel score per square);
--   * after py, the smaller min(deg, 360 - deg) (the same for a point
--     and its east-west mirror), and only then the smaller deg.
function M.qsf_key(r) return (r.safe and 1000 or 0) + (r.kills or 0) end

-- PT2_QUICK_SIDE_FIRST: note each record's score in floor (per key)
function M.qsf_note(floor, list)
  for _, r in ipairs(list) do
    local k = M.qsf_key(r)
    local f = floor[k]
    if r.score and (not f or r.score < f) then floor[k] = r.score end
  end
end

function M.better(a, b, cap, band, pec, swb, no_s0, far, opt, floor)
  local keys = a.safe == b.safe and a.kills == b.kills
  if keys and opt and opt.quick_side_first and band and band > 0 then
    local sa, sb = a.score, b.score
    local m = math.min(sa, sb, a.tier_min or sa, b.tier_min or sb)
    if floor and floor < m then m = floor end
    keys = sa <= m + band and sb <= m + band
  end
  if keys then
    -- PT2_FAVOR_BLOCKER_PLACE: the lower blocker pill tier, then the
    -- nearer walls (nil with the knob off: the old order)
    local ta, tb = a.bp_tier or 0, b.bp_tier or 0
    if ta ~= tb then return ta < tb end
    local wa, wb = a.wall_far or 0, b.wall_far or 0
    if wa ~= wb then return wa < wb end
    local na, nb = a.aimnb or false, b.aimnb or false
    if na ~= nb then return nb end
    if far then
      local ra, rb = a.radius or 0, b.radius or 0
      if ra ~= rb then return ra > rb end
    end
  end
  if (pec or swb) and band and band > 0 and a.safe == b.safe and a.kills == b.kills then
    local sa, sb = a.score, b.score
    local m = math.min(sa, sb, a.tier_min or sa, b.tier_min or sb)
    if sa <= m + band and sb <= m + band then
      if pec then
        local na, nb = a.n_built or 0, b.n_built or 0
        if na ~= nb then return na < nb end
      end
      if swb then
        local ga, gb = a.start_now or false, b.start_now or false
        if ga ~= gb then return ga end
      end
    end
  end
  if OLD.better(a, b, cap, band) then return true end
  if OLD.better(b, a, cap, band) then return false end
  if no_s0 then return false end
  -- S0: a last tie-break after every key above, so the winner never
  -- depends on the order table.sort leaves equal points in: the smaller
  -- angle, then the larger radius (PT2_MIRROR_SAFE_TIE: first the
  -- distance from the tank, then py)
  if opt and opt.mirror_tie and a.px and b.px then
    local qx, qy = opt.prev_px, opt.prev_py
    if qx and qy then
      local pa, pb = a.px == qx and a.py == qy, b.px == qx and b.py == qy
      if pa ~= pb then return pa end
    end
    local tx, ty = opt.tank_wx, opt.tank_wy
    if tx and ty then
      local ax, ay, bx, by = a.px - tx, a.py - ty, b.px - tx, b.py - ty
      local d2a, d2b = ax * ax + ay * ay, bx * bx + by * by
      if d2a ~= d2b then return d2a < d2b end
    end
    if a.py ~= b.py then return a.py < b.py end
    local da, db = a.deg or 0, b.deg or 0
    local ma, mb = math.min(da, 360 - da), math.min(db, 360 - db)
    if ma ~= mb then return ma < mb end
  end
  local da, db = a.deg or 0, b.deg or 0
  if da ~= db then return da < db end
  return (a.radius or 0) > (b.radius or 0)
end

local sqrt, floor, sin, cos, rad, abs, min, max =
  math.sqrt, math.floor, math.sin, math.cos, math.rad, math.abs, math.min, math.max
local atan2 = math.atan2 or math.atan
local BIT = require('bitcompat')
local bband, bbor, bbnot, blshift = BIT.band, BIT.bor, BIT.bnot, BIT.lshift
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
          if not ctx.inp.link_clear_front then infront = true end
        elseif w.near <= link.far then out.sure = false end
      else
        front = front + w.shots
      end
    end
  end
  -- PT2_LINK_CLEAR_FRONT: only cover cuts the link, and with
  -- PT2_LINK_CUT_MIN_SHOTS (E2 thin cover) only that much cover in all
  if link and ctx.inp.link_clear_front and OLD.link_cut(ctx.inp, front) then infront = true end
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
      local cover = 0
      -- (front is counted below: link_t set, it is counted again there)
      for k = 1, b.n_sq do
        local s, t = sql[k], b.sq_t[k]
        -- blockers of the base ray up to this square's tick
        while bi <= b.n_block and b.block[bi].t <= t do
          local e = b.block[bi]
          if e.kind == "ally_pill" or not (e.sq == s and extra[s]) then
            n = n + 1; blk[n] = e; cover = cover + e.shots
          end
          bi = bi + 1
        end
        local x = extra[s]
        if x then
          n = n + 1; blk[n] = { sq = s, shots = x, kind = "planned", t = t }; cover = cover + x
        end
      end
      while bi <= b.n_block do
        local e = b.block[bi]
        n = n + 1; blk[n] = e; cover = cover + e.shots
        bi = bi + 1
      end
      local front = 0
      if not link_t then front = cover end
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
  -- it, the old evaluator's test (eval_point sets v.cut with OLD.link_cut:
  -- front > 0, and at least PT2_LINK_CUT_MIN_SHOTS); cut_front is front > 0
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
-- M2: is A in range of (px, py)? pills_in_range's hasA, with no list
local function A_in_range(ctx, px, py)
  local inp, A = ctx.inp, ctx.A
  local pr = inp.pill_range + inp.range_margin
  local h = ctx.hostile
  for i = 1, #h do
    if h[i] == A then
      local cx, cy = pill_centre(A)
      local dx, dy = cx - px, cy - py
      return dx * dx + dy * dy <= pr * pr
    end
  end
  return false
end
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

-- C.BLOCKER_MIN_LANE_BRAD (inp.min_lane_brad, 0 = off): attack.lua's
-- shield lane check (M.aim_lane_wide, the SHIELD_LANE line) throws away a
-- plan whose fire aim has a clear lane narrower than this on either side,
-- bans the angle and plans again (gap_e Nov 24: v2 kept picking a far ring
-- point whose shell only just reached A). v2 asks the same of every aim it
-- calls clear, the same way: the gun angle to the fire aim
-- (attack_shield.AIM_INSET_FIRE, M.FIRE_AIMS) and aim_lane.wide_enough's
-- samples each side, each shell from (px, py) must reach A. The shells are
-- v2's own tank_shot (with the planned blockers).
M.FIRE_AIMS = { { 128, 128 }, { 16, 16 }, { 240, 16 }, { 16, 240 }, { 240, 240 },
                { 128, 16 }, { 240, 128 }, { 128, 240 }, { 16, 128 } }
local AL = require("aim_lane")
local f32 = SH.f32

-- aim_wide's known clear shots (gh_pt2_v2.c aw_note, the same rules): the
-- gun angles (float) of shots from (px, py) that reached A with the
-- planned blockers x, in this step (ctx.aw_gen), with no tanks tested.
-- The origin, A, the set (the table and its size) and the step are the
-- key; a note with another key starts a new list of at most AW_N angles.
local AW_N = 16
local function aw_size(x)
  if not x then return -1 end
  local n = 0
  for _ in pairs(x) do n = n + 1 end
  return n
end
local function aw_note(ctx, px, py, a, A, x)
  if ctx.aw_off then return end
  local m, xn = ctx.awm, aw_size(x)
  if not (m.on and m.px == px and m.py == py and m.tmx == A.mx and m.tmy == A.my
          and m.x == (x or false) and m.xn == xn and m.gen == ctx.aw_gen) then
    m.on, m.px, m.py, m.tmx, m.tmy, m.x, m.xn, m.gen, m.n = true, px, py, A.mx, A.my, x or false, xn, ctx.aw_gen, 0
  end
  if m.n < AW_N then m.n = m.n + 1; m[m.n] = a end
end
-- Is a known clear shot (same key) past gun angle f on this side of the
-- clear centre shot c, less than 64 brad from c? Blockers are whole
-- squares and the shells are checked every 32 wu, so no blocker stops a
-- shell between two shells that reach A.
local function aw_hit(ctx, px, py, A, x, c, f, side)
  local m = ctx.awm
  if ctx.aw_off or not m.on then return false end
  if not (m.px == px and m.py == py and m.tmx == A.mx and m.tmy == A.my
          and m.x == (x or false) and m.xn == aw_size(x) and m.gen == ctx.aw_gen) then return false end
  local df = side * ((f - c + 128) % 256 - 128)
  if not (df > 0) then return false end
  for i = 1, m.n do
    local d = side * ((m[i] - c + 128) % 256 - 128)
    if d > df and d < 64 then return true end
  end
  return false
end
M._aw_note = aw_note

-- aim_lane.wide_enough's answer with the same samples (step 1/16 brad,
-- n = ceil(mlb / step) each side), and the shot count it takes. Each side
-- tests the far sample first: when it reaches A so does every sample
-- between it and the centre, and the side is wide enough; a known clear
-- shot past it (aw_hit) gives the same with no walk. Only when the far
-- sample is stopped are the inner ones walked, in the old order. n_shot
-- (the slicer's work) counts the shots the one-by-one test takes.
local function aim_wide(ctx, px, py, ai, extra)
  local mlb = ctx.inp.min_lane_brad
  if not mlb or mlb <= 0 then return true end
  local A, env = ctx.A, ctx.env
  local fo = M.FIRE_AIMS[ai]
  local dx, dy = A.mx * 256 + fo[1] - px, A.my * 256 + fo[2] - py
  local a0 = (atan2(dx, -dy) * 256 / TWO_PI) % 256
  local buf = ctx.side_lane
  local step = AL.STEP
  ctx.n_shot = ctx.n_shot + 1
  local ac = a0 % 256
  if not SH.tank_shot_angle(env, px, py, ac, A.mx, A.my, extra, buf) then return false end
  local c = f32(ac)
  aw_note(ctx, px, py, c, A, extra)
  local n = math.ceil(mlb / step - 1e-9)
  local reach = {}
  for side = -1, 1, 2 do
    local r, cnt = 0, 0
    if n >= 1 then
      local fa = (a0 + side * n * step) % 256
      if aw_hit(ctx, px, py, A, extra, c, f32(fa), side) then
        r, cnt = n * step, n
      elseif SH.tank_shot_angle(env, px, py, fa, A.mx, A.my, extra, buf) then
        aw_note(ctx, px, py, f32(fa), A, extra)
        r, cnt = n * step, n
      else
        cnt = n
        for k = 1, n - 1 do
          local ka = (a0 + side * k * step) % 256
          if not SH.tank_shot_angle(env, px, py, ka, A.mx, A.my, extra, buf) then cnt = k; break end
          aw_note(ctx, px, py, f32(ka), A, extra)
          r = k * step
        end
      end
    end
    ctx.n_shot = ctx.n_shot + cnt
    reach[side] = r
  end
  return reach[-1] >= mlb - 1e-9 and reach[1] >= mlb - 1e-9
end
M._aim_wide = aim_wide

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
        local ok, why, ln = SH.tank_shot(env, nx, ny, tx + off[1], ty + off[2], A.mx, A.my, extra, ctx.lane)
        ctx.n_shot = ctx.n_shot + 1
        if ok then aw_note(ctx, nx, ny, SH.tank_angle(nx, ny, tx + off[1], ty + off[2]), A, extra) end
        if ok and not aim_wide(ctx, nx, ny, ai, extra) then ok, why = false, "narrow" end
        if ok then ctx.lane = {}; return nx, ny, nw, ai, ln end
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
local function eval_point(ctx, px, py, extra, aim_fixed, nudge, exact, lazy)
  local inp, A, env = ctx.inp, ctx.A, ctx.env
  local clk = ctx.clk
  local tp = clk and clk()
  -- M2: the pills in range only once the point has an aim (T below)
  local hasA = A_in_range(ctx, px, py)
  if tp then ctx.tm.pir = ctx.tm.pir + clk() - tp end
  if not hasA then return nil, "A_OUT_OF_RANGE" end
  env.omx, env.omy = floor(px / 256), floor(py / 256)
  local t0 = ctx.clk and ctx.clk()
  local aim_idx, lane
  local a1, a2 = 1, #ctx.aims
  if aim_fixed then a1, a2 = aim_fixed, aim_fixed end
  local why_out, shorts = "short", nil
  for ai = a1, a2 do
    local off = ctx.aims[ai]
    local ok, why, ln = SH.tank_shot(env, px, py, A.mx * 256 + off[1], A.my * 256 + off[2], A.mx, A.my, extra,
                                     ctx.lane)
    ctx.n_shot = ctx.n_shot + 1
    if ok then aw_note(ctx, px, py, SH.tank_angle(px, py, A.mx * 256 + off[1], A.my * 256 + off[2]), A, extra) end
    if ok and not aim_wide(ctx, px, py, ai, extra) then ok, why = false, "narrow" end
    if ok then aim_idx, lane = ai, ln; ctx.lane = {}; break end
    why_out = why
    if why == "short" then shorts = shorts or {}; shorts[#shorts + 1] = ai end
  end
  local nudge_wu = 0
  if not aim_idx and nudge and shorts and inp.aim_nudge and not aim_fixed then
    local tn = clk and clk()
    local nx, ny, nw, nai, nln = nudge_aim(ctx, px, py, extra, shorts)
    if nx then
      -- the nudged point is judged again: A in range there too
      if A_in_range(ctx, nx, ny) then
        px, py, nudge_wu, aim_idx, lane = nx, ny, nw, nai, nln
        ctx.stat.nudged = ctx.stat.nudged + 1
      end
      env.omx, env.omy = floor(px / 256), floor(py / 256)
    end
    if tn then ctx.tm.nudge = ctx.tm.nudge + clk() - tn end
  end
  if t0 then ctx.tm.aim = ctx.tm.aim + ctx.clk() - t0 end
  if not aim_idx then return nil, "OUTGOING:" .. tostring(why_out) end
  local T = pills_in_range(ctx, px, py)
  -- the clock of the rest (rays, chain, bars, the record), less its casts
  local tb = clk and clk()
  local cast0 = ctx.tm.cast
  -- S9 lazy: A's ray only. With no link the chain can use, the point is a
  -- single take: a stub (its point, nudge, aim and A's shortfall). The
  -- caller judges it in full (aim_fixed, no nudge) only when its plan
  -- can still reach the travel band.
  if lazy then
    local bar = ctx.finish and 0 or inp.bar_single
    -- S10 chains only: a sure MUST mask at the final point (after the
    -- nudge) with no link the chain can use is a single take without a
    -- cast (a_need is found later, only when needed)
    if ctx.chains_only then
      local pr = predict(ctx, ctx.cw(A), px, py)
      local Y = pr.link
      if pr.sure and not (Y and pr.used and Y ~= A and not (inp.chain_skip and inp.chain_skip[Y])) then
        ctx.stat.must_cut = ctx.stat.must_cut + 1
        if inp.v2_mask_stats then
          local v = ray_view(base_ray(ctx, A, px, py), extra)
          if inp.link_clear_front then v.cut = v.link ~= nil and OLD.link_cut(inp, v.front) end
          local n2 = (not v.cut) and v.link or nil
          if n2 and n2 ~= A and not (inp.chain_skip and inp.chain_skip[n2]) then
            ctx.stat.must_cut_wrong = ctx.stat.must_cut_wrong + 1
          end
        end
        if tb then ctx.tm.evb = ctx.tm.evb + (clk() - tb) - (ctx.tm.cast - cast0) end
        return { lazy = true, px = px, py = py, nudge_wu = nudge_wu, T = T, aim_idx = aim_idx,
                 chain = { A }, kills = 1, a_need = nil, a_bar = bar }
      end
    end
    local vA = ray_view(base_ray(ctx, A, px, py), extra)
    if inp.link_clear_front then vA.cut = vA.link ~= nil and OLD.link_cut(inp, vA.front) end
    local nxt = (inp.chains and inp.max_chain >= 2 and not vA.cut) and vA.link or nil
    if not nxt or nxt == A or (inp.chain_skip and inp.chain_skip[nxt]) then
      ctx.stat.lazy = ctx.stat.lazy + 1
      if tb then ctx.tm.evb = ctx.tm.evb + (clk() - tb) - (ctx.tm.cast - cast0) end
      return { lazy = true, px = px, py = py, nudge_wu = nudge_wu, T = T, aim_idx = aim_idx,
               chain = { A }, kills = 1, a_need = bar - vA.cover }
    end
  end
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
    -- PT2_LINK_CUT_MIN_SHOTS (E2 thin cover): thin cover keeps the link
    if inp.link_clear_front then v.cut = v.link ~= nil and OLD.link_cut(inp, v.front) end
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
  local out = {
    px = px, py = py, nudge_wu = nudge_wu, T = T, rays = rays, ri = ri, chain = chain, in_chain = in_chain,
    chain_mode = chain_mode, aim_idx = aim_idx, lane = lane, clear_aims = { aim_idx },
    lanes = { [aim_idx] = lane }, aims_done = aim_fixed or aim_idx, extra_in = extra,
    covered = covered_all, uncovered = uncovered,
    safe = covered_all, max_ok = true,
    kills = chain_mode and #chain or 1,
    ally_cover = ally_cover, need = need_A, have = have_A, depth = depth,
    finish = ctx.finish or false,
  }
  if tb then ctx.tm.evb = ctx.tm.evb + (clk() - tb) - (ctx.tm.cast - cast0) end
  return out
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
                                     A.mx, A.my, rec.extra_in, ctx.lane)
      ctx.n_shot = ctx.n_shot + 1
      if ok then
        aw_note(ctx, rec.px, rec.py, SH.tank_angle(rec.px, rec.py, A.mx * 256 + off[1], A.my * 256 + off[2]),
                A, rec.extra_in)
      end
      if ok and not aim_wide(ctx, rec.px, rec.py, ai, rec.extra_in) then ok = false end
      if ok then rec.clear_aims[#rec.clear_aims + 1] = ai; rec.lanes[ai] = ln; ctx.lane = {} end
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
  -- S4b: the square's own facts (the same for every pill and point), read
  -- once per group run like the square reads (ctx.sqf, the job's table):
  -- true, or the first rule of area, pill, wall, not_buildable it fails
  local f = ctx.sqf[sq]
  if f == nil then
    local env = ctx.env
    if not inp.in_map(mx, my) or not inp.stay_in(mx, my) then f = "area"
    elseif env.pill_on(mx, my) then f = "pill"
    else
      local tt = env.ttype(mx, my)
      if tt == env.T_BUILDING or tt == env.T_HALFBUILD then f = "wall"
      elseif not inp.buildable(mx, my) then f = "not_buildable"
      else f = true end
    end
    ctx.sqf[sq] = f
  end
  if f == "area" then return false, "area" end
  local cx, cy = mx * 256 + 128, my * 256 + 128
  local dxp, dyp = cx - rec.px, cy - rec.py
  if dxp * dxp + dyp * dyp < (inp.blocker_min_dist * 256) ^ 2 then return false, "near_tank" end
  local sx, sy = pill_centre(X)
  local d1 = (cx - sx) ^ 2 + (cy - sy) ^ 2
  local d2 = (rec.px - sx) ^ 2 + (rec.py - sy) ^ 2
  if d1 >= d2 then return false, "behind_tank" end
  if f ~= true then return false, f end
  -- S7: the answer depends on the builder's start (rec.omx, omy) too, so
  -- the cache key is (start, square)
  local rk = ((rec.omx or 0) * 256 + (rec.omy or 0)) * 65536 + sq
  local rc = ctx.reach_cache[rk]
  if rc == nil then
    rc = inp.lgm_reach(rec.omx, rec.omy, mx, my) and true or false
    ctx.reach_cache[rk] = rc
    ctx.stat.reach_calls = ctx.stat.reach_calls + 1
    if inp.v2_confirm_check then
      -- the bench: the answer the square-only key gave (its first answer)
      local q = ctx.reach_sq
      if not q then q = {}; ctx.reach_sq = q end
      local old = q[sq]
      if old == nil then q[sq] = rc; ctx.stat.reach_sq_calls = ctx.stat.reach_sq_calls + 1
      elseif old ~= rc then ctx.stat.reach_differs = ctx.stat.reach_differs + 1 end
    end
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

-- S4: the search state, made once and used again by every search (no
-- closure or table per call). WS.C: the candidate squares in index order;
-- WS.on[j]: path j's existing shots per square; WS.pc[j]: path j's
-- candidates as indexes into C, in order; WS.rem[j]: path j's shots still
-- short; WS.used[i]: candidate i is chosen or left out in this branch.
local WS = { C = {}, mk = {}, on = {}, pc = {}, rem = {}, used = {}, outl = {}, pick = {}, kind = {}, sols = nil,
             nU = 0, nC = 0, P = 0, W = 0, nodes = 0 }

-- Every set of the candidates with pl pills and wl walls still to place,
-- on aims mask m: a set that covers every path with no blocker left is a
-- solution. The branch is on the first path still short: its candidate i
-- (in index order) goes in, and its candidates before i stay out of that
-- branch (a set holds a smallest candidate of that path, so each set is
-- met once). A candidate whose mask ANDs m to 0 stays out too (m only
-- gets smaller). A path that its free candidates cannot cover cuts the
-- branch.
local function ws_enum(pl, wl, m, depth)
  local S = WS
  S.nodes = S.nodes + 1
  local rem, nU = S.rem, S.nU
  local j = nil
  for jj = 1, nU do if rem[jj] > 0 then j = jj; break end end
  if not j then
    if pl == 0 and wl == 0 then
      local it = {}
      for i = 1, depth do it[i] = { sq = S.pick[i], kind = S.kind[i] } end
      S.sols[#S.sols + 1] = { items = it, mask = m }
    end
    return
  end
  if pl + wl == 0 then return end
  local P, W = S.P, S.W
  local used, pcs = S.used, S.pc
  for jj = j, nU do
    local need = rem[jj]
    if need > 0 then
      local free = 0
      for _, i in ipairs(pcs[jj]) do if not used[i] then free = free + 1 end end
      local np = free < pl and free or pl
      local nw = free - np
      if nw > wl then nw = wl end
      if np * P + nw * W < need then return end
    end
  end
  local C, mk, on = S.C, S.mk, S.on
  local pc = pcs[j]
  local n_out = 0
  local outl = S.outl[depth + 1]
  if not outl then outl = {}; S.outl[depth + 1] = outl end
  for _, i in ipairs(pc) do
    if not used[i] then
      used[i] = true
      n_out = n_out + 1
      outl[n_out] = i
      local m2 = bband(m, mk[i])
      if m2 ~= 0 then
        local s = C[i]
        for pass = 1, 2 do
          local kind, shots
          if pass == 1 and pl > 0 then kind, shots = "pill", P
          elseif pass == 2 and wl > 0 then kind, shots = "wall", W end
          if kind then
            S.pick[depth + 1], S.kind[depth + 1] = s, kind
            for jj = 1, nU do
              local e = on[jj][s]
              if e then rem[jj] = rem[jj] - (shots - e) end
            end
            ws_enum(kind == "pill" and pl - 1 or pl, kind == "wall" and wl - 1 or wl, m2, depth + 1)
            for jj = 1, nU do
              local e = on[jj][s]
              if e then rem[jj] = rem[jj] + (shots - e) end
            end
          end
        end
      end
    end
  end
  for q = 1, n_out do used[outl[q]] = nil end
end

-- The solutions of size k go to each aim still without one (bit i - 1 of
-- active) whose mask holds them; returns the aims still without one.
local function ws_take(found, active, nA, sols, k)
  local got = 0
  for i = 1, nA do
    local bi = blshift(1, i - 1)
    if bband(active, bi) ~= 0 then
      local list = nil
      for _, so in ipairs(sols) do
        if bband(so.mask, bi) ~= 0 then list = list or {}; list[#list + 1] = so end
      end
      if list then found[i] = { k = k, sols = list }; got = bbor(got, bi) end
    end
  end
  return bband(active, bbnot(got))
end

-- The pick keys of a solution (the same for every aim), once each.
local function ws_keys(so, inp, rec, U, lr, lon)
  if so.depth then return end
  local it = so.items
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
  so.depth, so.two, so.trip, so.order, so.key = depth, two, tl, order, table.concat(key, ",")
end

-- PT2_WALL_MEMO (M1): the wall searches' results by their S4b key (below),
-- kept across evaluates. One memo per bot (each bot has its own Lua
-- state), one table per goal pill (WM.by): the group run's goal; its
-- start pill runs use the goal's table. M.wall_memo_check picks the goal's
-- table (inp._wm_t) and clears every table when the pill set changes
-- (squares, owners, in tank, ally or not: a team change shows in the ally
-- flags). A goal pill change keeps the other goals' tables: the bot goes
-- back to a pill it planned for before (the bench: most hits are there).
-- The key holds every input of the search, so any other change (a map
-- square, a pill's cover, a builder) gives a new key and a miss, never a
-- stale answer. A new map is a new Lua state. Eviction: when the memo
-- holds inp.wall_memo_max results in all, a new one clears it all
-- (WM.full counts it).
-- The results are read only: plan_point and confirm_plan copy what they
-- keep (confirm_plan makes its own plan list).
local WM = { sig = nil, by = {}, n = 0, hits = 0, misses = 0, clears = 0, full = 0, hit_nodes = 0, miss_nodes = 0 }
M.WM = WM
function M.wall_memo_clear()
  WM.sig, WM.by, WM.n = nil, {}, 0
end
-- goalA: the goal pill (group_begin's inp.A; a lone evaluate: inp.A)
function M.wall_memo_check(inp, goalA)
  local goal = goalA and (goalA.mx .. "," .. goalA.my) or ""
  local env = inp.env
  local t = {}
  for _, p in ipairs(inp.pills or {}) do
    t[#t + 1] = tostring(p.mx) .. "," .. tostring(p.my) .. "," .. tostring(p.owner) .. ","
      .. (p.in_tank and "t" or "") .. ((env and env.is_ally and env.is_ally(p)) and "a" or "")
  end
  local sig = table.concat(t, ";")
  if sig ~= WM.sig then
    if WM.sig ~= nil then WM.clears = WM.clears + 1 end
    WM.sig, WM.by, WM.n = sig, {}, 0
  end
  local t = WM.by[goal]
  if not t then t = {}; WM.by[goal] = t end
  inp._wm_t, inp._wm_goal = t, goal
end

-- S4b: every input the wall search and its pick read, as one string (two
-- searches with the same key return the same plans): the aims; the
-- blocker counts and shots; the builder's start square (trip); per
-- uncovered path in order, its need and its legal squares in order with
-- their existing shots and aim mask; the last chain pill's ray cover and
-- the existing shots on it of the legal squares (depth); the size cap of
-- the travel band (rec.kmax, S9).
local function ws_input_key(rec, aims, U, maskof, inp)
  local o = { table.concat(aims, ","), tostring(inp.num_pill_blockers or 0), tostring(inp.build_max),
              tostring(inp.pill_drop_shots), tostring(inp.wall_shots), tostring(rec.kmax),
              tostring(rec.omx or floor(rec.px / 256)), tostring(rec.omy or floor(rec.py / 256)) }
  local cs = {}
  for _, u in ipairs(U) do
    local t = { tostring(u.need) }
    for _, s in ipairs(u.cand) do
      t[#t + 1] = s .. ":" .. u.on[s] .. ":" .. maskof[s]
      cs[s] = true
    end
    o[#o + 1] = table.concat(t, " ")
  end
  local last = rec.chain[#rec.chain]
  local lr = rec.rays[rec.ri[last]]
  if lr then
    local ex = {}
    for b = 1, lr.n_block do
      local e = lr.block[b]
      if e.kind ~= "ally_pill" then ex[e.sq] = e.shots end
    end
    local t = { "L", tostring(lr.cover) }
    for k = 1, lr.n_sq do
      local q = lr.sq[k]
      if cs[q] then t[#t + 1] = q .. ":" .. (ex[q] or 0) end
    end
    o[#o + 1] = table.concat(t, " ")
  end
  return table.concat(o, "|")
end

-- The wall plans for record rec on each aim of aims (each aim's lane).
-- Returns { [ai] = the best solution { k, depth, two, trip, ai,
-- items = { {sq, kind, shots} } in build order } }; an aim with no plan has
-- no entry. Every set of up to build_max legal squares on the uncovered
-- paths is tried (a DFS on the first path still short of its bar, so each
-- set that covers every path is met); for each aim the first size with a
-- solution wins; the search stops at the first size where any aim has
-- one (plan_point compares the size first).
-- S3: one DFS for all aims. A square's mask has a bit for each aim whose
-- lane misses it; a branch whose chosen squares AND to 0 over the aims
-- still without a plan is cut. A set is a solution of each aim in its
-- mask, so each aim gets the same sets as a search of its own.
local ws_run   -- the search on the legal squares (below)
-- M2: the search's scratch tables (see search_walls_all)
local WL = { gen = 0, g = {}, m = {}, U = {}, UP = {}, mask = {}, mtouch = {}, nm = 0 }
local NOLANE = {}
local function search_walls_all(ctx, rec, aims)
  local inp = ctx.inp
  local out = {}
  local nA = #aims
  if nA == 0 then return out end
  -- M2: the lanes as one table, reused: WL.m[s] = the bits of the aims
  -- whose lane holds s (valid when WL.g[s] is this call's gen)
  local wg = WL.gen + 1
  WL.gen = wg
  local lg, lm = WL.g, WL.m
  for i, ai in ipairs(aims) do
    local bi = blshift(1, i - 1)
    local l = rec.lanes[ai]
    if l then
      for k = 1, #l do
        local s = l[k]
        if lg[s] ~= wg then lg[s] = wg; lm[s] = bi else lm[s] = bbor(lm[s], bi) end
      end
    end
  end
  local all_aims = blshift(1, nA) - 1
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
  -- M2: the paths' entries and the masks, reused call to call (a search
  -- runs to its end with no yield; nothing keeps them after it)
  local U, UP = WL.U, WL.UP
  local nb = #base.U
  for j = 1, nb do
    local ub = base.U[j]
    local e = UP[j]
    if not e then e = { cand = {} }; UP[j] = e end
    e.r, e.need, e.on = ub.r, ub.need, ub.on
    local c = e.cand
    for q = #c, 1, -1 do c[q] = nil end
    U[j] = e
  end
  for j = #U, nb + 1, -1 do U[j] = nil end
  -- legal squares: old's slot rules pass on some uncovered path the
  -- square is on (all but the lane rule are the same for every aim, kept
  -- on the record: rec.slot_memo), and the lane of some aim misses it
  local memo = rec.slot_memo
  if not memo then memo = {}; rec.slot_memo = memo end
  local nolane = NOLANE
  local maskof = WL.mask
  local mt = WL.mtouch
  for q = 1, WL.nm do maskof[mt[q]] = nil; mt[q] = nil end
  local nm = 0
  for _, u in ipairs(U) do
    local r = u.r
    for k = 1, r.n_sq do
      local s = r.sq[k]
      local m = maskof[s]
      if m == nil then
        m = (lg[s] == wg) and bband(all_aims, bbnot(lm[s])) or all_aims
        if m ~= 0 then
          local l = memo[s]
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
          if not l then m = 0 end
        end
        maskof[s] = m
        nm = nm + 1; mt[nm] = s
      end
      if m ~= 0 then u.cand[#u.cand + 1] = s end
    end
  end
  WL.nm = nm
  local wm = inp.wall_memo
  local key = (wm or inp.v2_wkey) and ws_input_key(rec, aims, U, maskof, inp)
  if inp.v2_wkey then inp.v2_wkey(key) end
  if not wm then return ws_run(ctx, rec, aims, U, maskof) end
  -- PT2_WALL_MEMO (M1): the same key gives the same plans; a hit adds the
  -- search's own counts to stat as if it ran (the dumps stay the same) and
  -- to ctx.memo_nodes (the slicer does not count them as work)
  local st = ctx.stat
  local mt = inp._wm_t
  local e = mt and mt[key]
  if e then
    WM.hits = WM.hits + 1
    WM.hit_nodes = WM.hit_nodes + e.nodes
    st.wall_slow = st.wall_slow + e.slow
    st.wall_nodes = st.wall_nodes + e.nodes
    ctx.memo_nodes = ctx.memo_nodes + e.nodes
    return e.out
  end
  WM.misses = WM.misses + 1
  local s0, n0 = st.wall_slow, st.wall_nodes
  local out = ws_run(ctx, rec, aims, U, maskof)
  if not mt then return out end
  if WM.n >= (inp.wall_memo_max or 2000) then
    mt = {}
    WM.by, WM.n, WM.full = { [inp._wm_goal] = mt }, 0, WM.full + 1
    inp._wm_t = mt
  end
  mt[key] = { out = out, slow = st.wall_slow - s0, nodes = st.wall_nodes - n0 }
  WM.miss_nodes = WM.miss_nodes + st.wall_nodes - n0
  WM.n = WM.n + 1
  return out
end
ws_run = function(ctx, rec, aims, U, maskof)
  local inp = ctx.inp
  local out = {}
  local nA = #aims
  local npb = inp.num_pill_blockers or 0
  local nU = #U
  local P, W = inp.pill_drop_shots, inp.wall_shots
  -- S4: each path alone first: when its legal squares cannot cover it
  -- with build_max blockers, no set can
  -- S9: rec.kmax caps the size (a plan with more blockers cannot reach
  -- the travel band)
  local kcap = inp.build_max
  if rec.kmax and rec.kmax < kcap then kcap = rec.kmax end
  if kcap < 1 then return out end
  local bp = min(npb, kcap)
  local bw = kcap - bp
  local slow = P < W
  for j = 1, nU do
    local u = U[j]
    local free = #u.cand
    local np = min(free, bp)
    local nw = min(free - np, bw)
    if np * P + nw * W < u.need then return out end
    for _, s in ipairs(u.cand) do if u.on[s] >= W then slow = true end end
  end
  local active = blshift(1, nA) - 1
  local found = {}   -- aim index i -> { k, sols }
  if slow then
    -- a blocker that adds no cover on a path (its square already holds as
    -- much): the sets are not all smallest, so the S3 search runs
    ctx.stat.wall_slow = ctx.stat.wall_slow + 1
    local sols = nil
    local rem = {}
    for j, u in ipairs(U) do rem[j] = u.need end
    local chosen, kinds = {}, {}
    local seen = {}
    local nodes = 0
    local function dfs(pl, wl, m)
      nodes = nodes + 1
      local j = nil
      for jj = 1, #U do if rem[jj] > 0 then j = jj; break end end
      if not j then
        if pl == 0 and wl == 0 then
          -- a full set: record it once (its mask: the AND of its squares)
          local ks = {}
          for i = 1, #chosen do ks[i] = chosen[i] * 2 + (kinds[i] == "pill" and 1 or 0) end
          table.sort(ks)
          local key = table.concat(ks, ",")
          if not seen[key] then
            seen[key] = true
            local it = {}
            for i = 1, #chosen do it[i] = { sq = chosen[i], kind = kinds[i] } end
            sols[#sols + 1] = { items = it, mask = m }
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
        local m2 = bband(m, maskof[s])
        local used = m2 == 0
        if not used then
          for i = 1, #chosen do if chosen[i] == s then used = true; break end end
        end
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
              dfs(kind == "pill" and pl - 1 or pl, kind == "wall" and wl - 1 or wl, m2)
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
    -- the first size where some aim has a solution: only those aims can
    -- win (plan_point compares the size first)
    for k = 1, kcap do
      sols = {}
      seen = {}
      local p = min(npb, k)
      dfs(p, k - p, active)
      if #sols > 0 then active = ws_take(found, active, nA, sols, k) end
      if next(found) then break end
    end
    ctx.stat.wall_nodes = ctx.stat.wall_nodes + nodes
  else
    -- every set once, in index order (ws_enum)
    local S = WS
    local Cs, mk = S.C, S.mk
    local gen = (S.gen or 0) + 1
    S.gen = gen
    local mark = S.mark or {}
    S.mark = mark
    local nC = 0
    for j = 1, nU do
      for _, s in ipairs(U[j].cand) do
        if mark[s] ~= gen then mark[s] = gen; nC = nC + 1; Cs[nC] = s end
      end
    end
    for i = #Cs, nC + 1, -1 do Cs[i] = nil end
    table.sort(Cs)
    local idx = S.idx or {}
    S.idx = idx
    for i = 1, nC do mk[i] = maskof[Cs[i]]; idx[Cs[i]] = i end
    for j = 1, nU do
      S.on[j] = U[j].on
      local pc = S.pc[j] or {}
      S.pc[j] = pc
      local n = 0
      for _, s in ipairs(U[j].cand) do n = n + 1; pc[n] = idx[s] end
      for q = #pc, n + 1, -1 do pc[q] = nil end
      table.sort(pc)
      S.rem[j] = U[j].need
    end
    S.nU, S.nC, S.P, S.W, S.nodes = nU, nC, P, W, 0
    for k = 1, kcap do
      S.sols = {}
      local p = min(npb, k)
      ws_enum(p, k - p, active, 0)
      if #S.sols > 0 then active = ws_take(found, active, nA, S.sols, k) end
      if next(found) then break end
    end
    S.sols = nil
    for j = 1, nU do S.on[j] = nil end
    ctx.stat.wall_nodes = ctx.stat.wall_nodes + S.nodes
  end
  if not next(found) then return out end
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
  for i, ai in ipairs(aims) do
    local f = found[i]
    if f then
      local best = nil
      for _, so in ipairs(f.sols) do
        ws_keys(so, inp, rec, U, lr, lon)
        local better = false
        if not best then better = true
        elseif so.depth ~= best.depth then better = so.depth > best.depth
        elseif so.two ~= best.two then better = so.two
        elseif abs(so.trip - best.trip) > 1e-9 then better = so.trip < best.trip
        else better = so.key < best.key end
        if better then best = so end
      end
      local items = {}
      for n, oi in ipairs(best.order) do items[n] = best.items[oi] end
      out[ai] = { k = f.k, depth = best.depth, two = best.two, trip = best.trip, order = best.order,
                  items = items, key = best.key, ai = ai, sols = f.sols }
    end
  end
  return out
end
local function search_walls(ctx, rec, ai)
  return search_walls_all(ctx, rec, { ai })[ai]
end
M._search_walls = search_walls
M._search_walls_all = search_walls_all

-- Plan blockers for rec: every clear aim, the best plan wins (covered,
-- fewer blockers, more depth, a square on two paths, shorter trip). The
-- plan is confirmed with exact casts. Returns the final record.
-- PT2_START_WHILE_BUILDING (E4, pt2_start.lua has the math): may the
-- tank start at this point while the builder builds rec.plan? Sets on rec:
--   start_now      true: start at once (no plan, or the armour holds)
--   exposure       ticks until the last planned wall stands: the builder
--                  leaves the tank point and builds rec.plan in its order,
--                  back to the tank between walls (E4-1, pt2_start);
--                  each trip is inp.lgm_ticks (the builder's own path
--                  cost) when given, else 16 ticks per square
--   shells_through the shells summed over each pill whose ray has a
--                  planned blocker and needs it (a chain pill without its
--                  link cover); cover_now = the shots on that ray that
--                  stand now
--   start_pills    { mx, my, cover_now, planned = { mx, my, shots } } of
--                  those pills (the caller checks again with its own
--                  builder, anger and walls at the hold point)
--   start_why      nil, "bystander", "armour" or "unreachable" (a trip
--                  cannot be made; exposure -1)
-- A bystander (a pill not in the chain, its ray not "reach") whose ray has
-- a planned blocker and whose standing cover is under its bar needs that
-- cover before the start: start_now false (E4-7).
-- PT2_START_ANGER_RISE: the chain (A, then each used link) as
-- pt2_start.rise_chain walks it. Chain pill k < last hits chain pill k + 1
-- (the link is used: no cover in front of it); the last chain pill's
-- shells go to the tank through the cover on its ray (standing cover from
-- tick 0, each planned wall from its own finish tick) and are counted when
-- the ray has a planned blocker. Returns the shells and the chain
-- ({ mx, my, to, cover_now, planned, count }, kept for the hold point).
local function pill_speed(inp, X)
  local iv
  if inp.pill_interval then iv = inp.pill_interval(X) end
  if iv then return iv end
  local anger = inp.anger_of and inp.anger_of(X) or (X.anger or 0)
  return PS.pill_interval(anger, inp.anger_bump, inp.pill_attack_ticks, inp.pill_attack_min_ticks,
    inp.pill_angry_divisor, inp.start_anger_rise)
end

local function rise_check(ctx, rec, ox, oy, exp)
  local inp = ctx.inp
  local chain, n = rec.chain, #rec.chain
  local ts = PS.wall_ticks(ox, oy, rec.plan, ctx.lgm_ticks) or {}
  local wt = {}
  for i, q in ipairs(rec.plan) do wt[q.my * 256 + q.mx] = ts[i] end
  local entries, keep = {}, {}
  for k = 1, n do
    local X = chain[k]
    local e = { speed = pill_speed(inp, X), mx = X.mx, my = X.my, cover_now = 0 }
    if k < n then
      e.to = k + 1
    else
      local r = rec.rays[rec.ri[X]]
      for j = 1, r.n_block do
        local b = r.block[j]
        if b.kind == "planned" then
          e.planned = e.planned or {}
          e.planned[#e.planned + 1] = { mx = b.sq % 256, my = floor(b.sq / 256), shots = b.shots }
        else
          e.cover_now = e.cover_now + b.shots
        end
      end
      e.count = e.planned ~= nil
    end
    entries[k] = e
    keep[k] = { mx = e.mx, my = e.my, to = e.to, cover_now = e.cover_now, planned = e.planned, count = e.count }
  end
  local A = chain[1]
  local o = { attack_ticks = inp.pill_attack_ticks, min_ticks = inp.pill_attack_min_ticks,
              divisor = inp.pill_angry_divisor, cooldown = inp.pill_cooldown_ticks,
              tank_reload = inp.tank_reload,
              flight = PS.flight_ticks(rec.px, rec.py, A.mx, A.my, inp.env and inp.env.shell_speed) }
  local shells = PS.rise_chain(entries, exp, function(mx, my) return wt[my * 256 + mx] end, o)
  return shells, keep
end

local function start_check(ctx, rec)
  local inp = ctx.inp
  rec.start_now, rec.exposure, rec.shells_through, rec.start_pills, rec.start_why = true, 0, 0, nil, nil
  rec.start_chain = nil
  local plan = rec.plan
  if not plan or #plan == 0 then return end
  local ox = rec.omx or floor(rec.px / 256)
  local oy = rec.omy or floor(rec.py / 256)
  local exp = PS.exposure_ticks(ox, oy, plan, ctx.lgm_ticks)
  -- E4-4: the builder cannot get to a wall: no start
  if not exp then
    rec.start_now, rec.exposure, rec.start_why = false, -1, "unreachable"
    return
  end
  local shells, pills = 0, {}
  for i, X in ipairs(rec.T) do
    local r = rec.rays[i]
    local planned, now_sh = nil, 0
    for k = 1, r.n_block do
      local e = r.block[k]
      if e.kind == "planned" then
        planned = planned or {}
        planned[#planned + 1] = { mx = e.sq % 256, my = floor(e.sq / 256), shots = e.shots }
      else
        now_sh = now_sh + e.shots
      end
    end
    if planned then
      local ci = rec.in_chain[X]
      if not ci then
        -- E4-7: a bystander whose standing cover meets its bar needs no
        -- planned blocker before the start
        if r.hit ~= "reach" and now_sh < (r.bar or 0) then
          rec.start_now, rec.exposure, rec.start_why = false, exp, "bystander"
          return
        end
      elseif not r.link_cover then
        shells = shells + PS.shells_through(exp, pill_speed(inp, X), now_sh)
        pills[#pills + 1] = { mx = X.mx, my = X.my, cover_now = now_sh, planned = planned }
      end
    end
  end
  -- PT2_START_ANGER_RISE: the tick walk over the chain replaces the sum
  -- (only a point with a counted pill; the bystander rule above stays)
  if #pills > 0 and PS.rise_on(inp.start_anger_rise) then
    shells, rec.start_chain = rise_check(ctx, rec, ox, oy, exp)
  end
  rec.exposure, rec.shells_through, rec.start_pills = exp, shells, pills
  rec.start_now = PS.can_start(inp.armour, shells, inp.finish_min_armour)
  if not rec.start_now then rec.start_why = "armour" end
end
M._start_check = start_check

-- PT2_EXTRA_COVER_WALLS (Nov 23, Andrew's yes, E5): a safe point gets up
-- to inp.extra_cover_walls more walls on the ray of the last chain pill
-- (a single take: A), after the rank (the rank never sees them). A square
-- is legal by the wall search's own rules (slot_ok with the aim's lane and
-- the squares in front of a used link), not on the plan, and the plan plus
-- the extra walls never pass PT2_BUILD_MAX. Of the legal squares the one
-- nearest the tank's approach square goes first (the shortest builder
-- trip).
-- PT2_EXTRA_COVER_SKIP_PILL_LINE (Nov 24, Andrew's yes): none when a
-- friendly pill already blocks that ray: a planned pill drop on one of its
-- squares, or a placed friendly pill it meets (block kind "ally_pill").
-- Returns { { sq, mx, my, shots, kind = "wall" } } or nil.
local function pill_on_ray(rec, r)
  for k = 1, r.n_block do
    if r.block[k].kind == "ally_pill" then return true end
  end
  local drops = nil
  for _, q in ipairs(rec.plan or {}) do
    if q.kind == "pill" then drops = drops or {}; drops[q.sq] = true end
  end
  if drops then
    for k = 1, r.n_sq do if drops[r.sq[k]] then return true end end
  end
  return false
end
M._pill_on_ray = pill_on_ray
local function extra_cover(ctx, rec)
  local inp = ctx.inp
  local n = min(inp.extra_cover_walls or 0, inp.build_max - #(rec.plan or {}))
  if n <= 0 or not rec.chain or not rec.ri then return nil end
  local last = rec.chain[#rec.chain]
  local r = rec.rays[rec.ri[last]]
  if not r then return nil end
  if inp.xcover_skip_pill_line and pill_on_ray(rec, r) then return nil end
  local lane_set = {}
  for _, q in ipairs((rec.lanes and rec.lanes[rec.aim_idx]) or {}) do lane_set[q] = true end
  local forbid = {}
  for i, X in ipairs(rec.T) do
    local ry = rec.rays[i]
    if rec.in_chain[X] and ry.link and not ry.cut and rec.in_chain[ry.link] then
      for k = 1, ry.n_sq do
        if ry.sq_t[k] <= ry.link_t then forbid[ry.sq[k]] = true end
      end
    end
  end
  local used = {}
  for _, q in ipairs(rec.plan or {}) do used[q.sq] = true end
  local ox = rec.omx or floor(rec.px / 256)
  local oy = rec.omy or floor(rec.py / 256)
  local cands = {}
  for k = 1, r.n_sq do
    local q = r.sq[k]
    if not used[q] and slot_ok(ctx, rec, r, k, lane_set, forbid) then
      local mx, my = q % 256, floor(q / 256)
      cands[#cands + 1] = { sq = q, mx = mx, my = my, d = (mx - ox) ^ 2 + (my - oy) ^ 2 }
    end
  end
  if #cands == 0 then return nil end
  table.sort(cands, function(a, b)
    if a.d ~= b.d then return a.d < b.d end
    return a.sq < b.sq
  end)
  local out = {}
  for i = 1, min(n, #cands) do
    local c = cands[i]
    out[i] = { sq = c.sq, mx = c.mx, my = c.my, shots = inp.wall_shots, kind = "wall" }
  end
  return out
end
M._extra_cover = extra_cover

-- S5: two confirms of one plan give the same answer: both nil, or the
-- same aim, cover, chain and, per ray, cover, hit, link and cut.
local function same_confirm(a, b)
  if not a or not b then return a == b end
  if a.covered ~= b.covered or a.aim_idx ~= b.aim_idx or #a.chain ~= #b.chain or #a.T ~= #b.T then
    return false
  end
  for i = 1, #a.chain do if a.chain[i] ~= b.chain[i] then return false end end
  for i = 1, #a.T do
    local x, y = a.rays[i], b.rays[i]
    if x.cover ~= y.cover or x.hit ~= y.hit or x.link ~= y.link or x.cut ~= y.cut or x.ok ~= y.ok then
      return false
    end
  end
  return true
end

-- S5: the winner's plan, confirmed again with exact casts (the bench
-- check, inp.v2_confirm_check). The answer must be the one ray_view gave
-- (stat.confirm_mismatch counts any other).
local function confirm_exact(ctx, rec)
  if not rec.plan or #rec.plan == 0 then return end
  local fx = eval_point(ctx, rec.px, rec.py, rec.extra, rec.aim_idx, false, true)
  ctx.stat.confirm_winner = ctx.stat.confirm_winner + 1
  local ok = fx and fx.covered and #fx.chain == #rec.chain
  if ok then for i = 1, #fx.chain do if fx.chain[i] ~= rec.chain[i] then ok = false end end end
  if not ok then ctx.stat.confirm_mismatch = ctx.stat.confirm_mismatch + 1 end
end

-- The confirm of one plan (items in build order) on aim ai at rec's
-- point: the record with the plan when it stands covered with the same
-- chain, else nil.
local S6_TRIES = 32
local function confirm_plan(ctx, rec, ai, items)
  local extra, plan = {}, {}
  for i, e in ipairs(items) do
    extra[e.sq] = e.shots
    plan[i] = { sq = e.sq, mx = e.sq % 256, my = floor(e.sq / 256), shots = e.shots, kind = e.kind }
  end
  local t1 = ctx.clk and ctx.clk()
  -- S5: the confirm reads the plain rays (kept, ray_view with the plan);
  -- only the winner gets exact casts (confirm_exact)
  local f = eval_point(ctx, rec.px, rec.py, extra, ai, false, false)
  if t1 then ctx.tm.confirm = ctx.tm.confirm + ctx.clk() - t1 end
  if ctx.inp.v2_confirm_check then
    -- the bench: the exact confirm too; any answer that differs counts
    local fx = eval_point(ctx, rec.px, rec.py, extra, ai, false, true)
    ctx.stat.confirm_checked = ctx.stat.confirm_checked + 1
    if not same_confirm(f, fx) then ctx.stat.confirm_mismatch = ctx.stat.confirm_mismatch + 1 end
  end
  local same = f and #f.chain == #rec.chain
  if same then for i = 1, #f.chain do if f.chain[i] ~= rec.chain[i] then same = false end end end
  -- the bench can fail a confirm on purpose (inp.v2_confirm_drop) to test S6
  local drop = ctx.inp.v2_confirm_drop
  if drop and drop(rec, ai, items) then same = false end
  if f and f.covered and same then
    f.nudge_wu = rec.nudge_wu
    f.plan, f.extra = plan, extra
    f.omx, f.omy = rec.omx, rec.omy
    f.clear_aims, f.lanes = rec.clear_aims, rec.lanes
    return f
  end
  return nil
end

-- PT2_AIM_AVOID_CHAIN_NEIGHBOUR (Nov 24, Andrew's yes): the squares of
-- a record's chain members: every chain pill but A, and every blocker on
-- a chain pill's ray (a wall or pill that stands, a planned wall, a
-- dropped pill). nil when there is none (a single take with no cover).
local function nb_set(ctx, rec)
  local A = ctx.A
  local a_sq = A.my * 256 + A.mx
  local set, any = {}, false
  for _, X in ipairs(rec.chain or {}) do
    if X ~= A then set[X.my * 256 + X.mx] = true; any = true end
    local i = rec.ri and rec.ri[X]
    local r = i and rec.rays and rec.rays[i]
    if r then
      for k = 1, r.n_block do
        local q = r.block[k].sq
        if q ~= a_sq then set[q] = true; any = true end
      end
    end
  end
  return any and set or nil
end
M._nb_set = nb_set

-- Is aim ai on A's square beside a square of set? A corner aim touches the
-- 3 squares that share its corner (the 2 side neighbours and the
-- diagonal), an edge midpoint the 1 square on its side, the centre none.
local function aim_beside(ctx, ai, set)
  if not set then return false end
  local off = ctx.aims[ai]
  local dx = (off[1] < 128 and -1) or (off[1] > 128 and 1) or 0
  local dy = (off[2] < 128 and -1) or (off[2] > 128 and 1) or 0
  local ax, ay = ctx.A.mx, ctx.A.my
  if dx ~= 0 and set[ay * 256 + ax + dx] then return true end
  if dy ~= 0 and set[(ay + dy) * 256 + ax] then return true end
  if dx ~= 0 and dy ~= 0 and set[(ay + dy) * 256 + ax + dx] then return true end
  return false
end
M._aim_beside = aim_beside

-- The record's aimnb (inp.aim_avoid_nb only): is its aim beside a chain
-- member? A covered point with no plan takes its first clear aim that is
-- not beside one, when it has one (the aim's own preference; a plan's aim
-- is picked in plan_point).
local function aim_nb(ctx, fin)
  if not ctx.inp.aim_avoid_nb then return end
  local set = nb_set(ctx, fin)
  local nb = aim_beside(ctx, fin.aim_idx, set)
  if nb and fin.covered and #fin.plan == 0 then
    for _, ai in ipairs(clear_aims_of(ctx, fin)) do
      if not aim_beside(ctx, ai, set) then
        fin.aim_idx, fin.lane = ai, fin.lanes[ai]
        nb = false
        ctx.stat.aimnb_moved = ctx.stat.aimnb_moved + 1
        break
      end
    end
  end
  fin.aimnb = nb
end

local function plan_point(ctx, rec)
  if rec.covered then rec.plan, rec.extra = {}, {}; return rec end
  -- S1: the clear aims' tank shots count in "aim" only (clear_aims_of has
  -- its own clock), so the walls clock starts after them
  local aims = clear_aims_of(ctx, rec)
  local t0 = ctx.clk and ctx.clk()
  local best = nil
  local all = search_walls_all(ctx, rec, aims)
  -- PT2_AIM_AVOID_CHAIN_NEIGHBOUR: an aim beside a chain member (of the
  -- point's chain and cover now, before the plan) comes after one that is
  -- not, before fewer walls
  local nbs = ctx.inp.aim_avoid_nb and nb_set(ctx, rec) or nil
  local best_nb = false
  for _, ai in ipairs(aims) do
    local s = all[ai]
    if s then
      local nb = aim_beside(ctx, ai, nbs)
      local b = false
      if not best then b = true
      elseif nb ~= best_nb then b = not nb
      elseif s.k ~= best.k then b = s.k < best.k
      elseif s.depth ~= best.depth then b = s.depth > best.depth
      elseif s.two ~= best.two then b = s.two
      elseif abs(s.trip - best.trip) > 1e-9 then b = s.trip < best.trip end
      if b then best, best_nb = s, nb end
    end
  end
  if t0 then ctx.tm.walls = ctx.tm.walls + ctx.clk() - t0 end
  ctx.stat.planned = ctx.stat.planned + 1
  if best then
    local f = confirm_plan(ctx, rec, best.ai, best.items)
    if f then return f end
    ctx.stat.confirm_fail = ctx.stat.confirm_fail + 1
    -- S6: the plan failed its confirm: the other solutions of the same
    -- size, of every aim, in the pick order (more depth, a square on two
    -- paths, the shorter trip, the earlier aim, the key); the first of
    -- them is the plan that failed. At most S6_TRIES confirms in all.
    local pairs_ = {}
    for n, ai in ipairs(aims) do
      local a = all[ai]
      if a and a.k == best.k then
        local nb = aim_beside(ctx, ai, nbs)
        for _, so in ipairs(a.sols) do pairs_[#pairs_ + 1] = { so = so, ai = ai, n = n, nb = nb } end
      end
    end
    table.sort(pairs_, function(x, y)
      local a, b = x.so, y.so
      if x.nb ~= y.nb then return not x.nb end
      if a.depth ~= b.depth then return a.depth > b.depth end
      if a.two ~= b.two then return a.two end
      if abs(a.trip - b.trip) > 1e-9 then return a.trip < b.trip end
      if x.n ~= y.n then return x.n < y.n end
      return a.key < b.key
    end)
    local tries = 1
    for i = 2, #pairs_ do
      if tries >= S6_TRIES then break end
      local q = pairs_[i]
      local items = {}
      for m, oi in ipairs(q.so.order) do items[m] = q.so.items[oi] end
      tries = tries + 1
      ctx.stat.confirm_retry = ctx.stat.confirm_retry + 1
      f = confirm_plan(ctx, rec, q.ai, items)
      if f then ctx.stat.confirm_rescued = ctx.stat.confirm_rescued + 1; return f end
    end
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
           confirm_fail = 0, wall_nodes = 0, cant = 0, survive = 0, lone = 0, plan_skipped = 0, lazy = 0, plan_cut = 0,
           must_cut = 0, must_cut_wrong = 0, co_stop = 0, co_single_eval = 0,
           loss_pass = 0, loss_safe = 0, chain_agree = 0, chain_differ = 0, chain_unsure = 0,
           wall_slow = 0,
           confirm_checked = 0, confirm_mismatch = 0, confirm_winner = 0,
           confirm_retry = 0, confirm_rescued = 0, reach_calls = 0, reach_sq_calls = 0, reach_differs = 0,
           leeway_more = 0, leeway_more_fail = 0, leeway_single = 0, aimnb_moved = 0 }
end
local function new_tm()
  return { wedges = 0, masks = 0, cast = 0, aim = 0, walls = 0, confirm = 0, leeway = 0,
           score_fn = 0, start = 0, xcover = 0, total = 0,
           pir = 0, nudge = 0, evb = 0, dfs = 0, sort = 0 }
end

-- The job cache: rays and square reads kept for one group run.
local function new_cache() return { rays = {}, sqm = {}, sqf = {} } end

local function new_ctx(inp, A)
  local cache = inp._v2_cache or new_cache()
  local ctx = { inp = inp, A = A, env = inp.env, reach_cache = {}, hostile = {},
                rays = cache.rays, sqm = cache.sqm, sqf = cache.sqf or {},
                aims = (inp.aim_points == 9) and M.AIMS9 or M.AIMS,
                stat = new_stat(), tm = new_tm(), clk = inp.v2_clock,
                n_shot = 0,       -- P1b: the aim tests (shadow.tank_shot), for the slicer
                memo_nodes = 0,   -- M1: the wall nodes stat got from the memo
                -- M2: the aim tests' lane buffer; a clear aim keeps it
                -- (its lane) and the next test gets a new one
                lane = {},
                side_lane = {},   -- aim_wide's shells (their squares unused)
                -- aim_wide's known clear shots (aw_note); aw_gen = the step
                awm = { on = false, n = 0 }, aw_gen = 0,
                aw_off = inp.env.ts_tanks ~= nil and #inp.env.ts_tanks > 0 }
  for _, p in ipairs(inp.pills) do
    if alive(p) and not inp.env.is_ally(p) then ctx.hostile[#ctx.hostile + 1] = p end
  end
  ctx.finish = OLD.finish_ok(inp, A)
  -- E4-5: the builder's trips (inp.lgm_ticks), one walk per trip in an
  -- evaluate (start_check asks the same trips at many points)
  if inp.lgm_ticks then
    local f, memo = inp.lgm_ticks, {}
    ctx.lgm_ticks = function(ox, oy, mx, my, bx, by)
      local k = ((ox * 256 + oy) * 256 + mx) * 256 + my
      k = (k * 256 + (bx or mx)) * 256 + (by or my)
      local v = memo[k]
      if v == nil then
        v = f(ox, oy, mx, my, bx, by)
        memo[k] = v == nil and false or v
      end
      return v or nil
    end
  end
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
  if inp.v2_c then return M.evaluate_c(inp) end
  return M.evaluate_lua(inp)
end

local function pre_rank(a, b)
  local ka, kb = a.kills, b.kills
  if ka ~= kb then return ka > kb end
  if a.covered ~= b.covered then return a.covered end
  if a.base ~= b.base then return a.base < b.base end
  return (a.deg or 0) < (b.deg or 0)
end

-- PT2_FAVOR_BLOCKER_PLACE (Nov 24, Andrew's yes): the record's bp_tier
-- and wall_far. The friendly blocker pills of a record: its planned pill
-- drops, and the placed friendly pills on its chain pills' rays. "Near":
-- within BLOCKER_NEAR_SQ (Andrew's 2.0) squares of the target pill A,
-- centre to centre.
--   carrying a pill (inp.carried_pills > 0): 0 = it uses one and every one
--   is near; 1 = it uses one and one is not near; 2 = it uses none.
--   carrying none: 0 = every one is near, or it has none; 1 = one is not.
-- wall_far: the largest distance from a planned wall to the record's
-- firing square, centre to centre, floored to whole squares (0 = no wall).
local BLOCKER_NEAR_SQ = 2.0
local function blocker_place(ctx, fin)
  if not ctx.inp.favor_blocker_place then return end
  local A = ctx.A
  local any, far = false, false
  local lim = BLOCKER_NEAR_SQ * BLOCKER_NEAR_SQ
  local function see(mx, my)
    any = true
    local dx, dy = mx - A.mx, my - A.my
    if dx * dx + dy * dy > lim then far = true end
  end
  local fmx, fmy = floor(fin.px / 256), floor(fin.py / 256)
  local wf = 0
  for _, q in ipairs(fin.plan or {}) do
    if q.kind == "pill" then
      see(q.mx, q.my)
    else
      local dx, dy = q.mx - fmx, q.my - fmy
      local d = floor(sqrt(dx * dx + dy * dy))
      if d > wf then wf = d end
    end
  end
  for _, X in ipairs(fin.chain or {}) do
    local i = fin.ri and fin.ri[X]
    local r = i and fin.rays and fin.rays[i]
    if r then
      for k = 1, r.n_block do
        local b = r.block[k]
        if b.kind == "ally_pill" then see(b.sq % 256, floor(b.sq / 256)) end
      end
    end
  end
  local tier
  if (ctx.inp.carried_pills or 0) > 0 then
    tier = (not any) and 2 or (far and 1 or 0)
  else
    tier = far and 1 or 0
  end
  fin.bp_tier, fin.wall_far = tier, wf
end
M._blocker_place = blocker_place

-- A planned point's rank numbers (S4: a module function, no closure).
local function score_of(ctx, fin)
  local inp, clk = ctx.inp, ctx.clk
  fin.n_built = #fin.plan
  fin.blk = (inp.own_pill_rank and fin.ally_cover) and 2 or 1
  fin.score = fin.base + fin.n_built * inp.build_cost
  aim_nb(ctx, fin)
  blocker_place(ctx, fin)
  if inp.start_while_building and fin.safe then
    local t3 = clk and clk()
    start_check(ctx, fin)
    if t3 then ctx.tm.start = ctx.tm.start + clk() - t3 end
  end
end
-- The point fields a plan's record takes from the point's record.
local function carry(fin, rec)
  fin.deg, fin.cx, fin.cy, fin.mx, fin.my, fin.radius = rec.deg, rec.cx, rec.cy, rec.mx, rec.my, rec.radius
  fin.base = rec.base
end

-- PT2_V2_SPLIT (P1, P1b): one evaluate over up to PT2_V2_SPLIT_TICKS work
-- ticks. The caller runs the evaluate in a coroutine and sets inp.v2_slice
-- (M.slicer). The evaluate calls it between units of work (a ring point, a
-- plan, a leeway test) with its work so far; the slicer yields when the
-- tick's budget is used, and the next tick resumes the same run, so the
-- answer is the one of the run in one call. The time between a yield and
-- its resume is taken off tm.total (the slicer returns it).
-- P1b (Andrew approved Nov 23): the work is weighted, in fixed units, so
-- the slices cost about the same and the split is the same on every
-- machine (no timers):
--   SLICE_W_CAST  one shadow.cast (a pill ray, base or exact)
--   SLICE_W_SHOT  one shadow.tank_shot (an aim test)
--   SLICE_W_NODE  one node of the wall search
--   SLICE_W_POINT one ring point (its square tests, pills in range and the
--                 mask test; a point the masks cut costs only this)
-- Bench fit (1399 cases, Nov 23): a cast, an aim test and a search node
-- each cost about 3.3 us, a ring point about 5 us more than its casts and
-- tests, so the weights are 2, 2, 2 and 3. Leeway tests are eval_point
-- runs (their casts and aim tests count); the sorts are 2% of the time
-- and get no weight.
-- The budget comes from the survivors: after the first ring's mask pass,
-- the work left is SLICE_PER_SURVIVOR x the ring's points the masks keep
-- (the bench mean of the whole evaluate's work per first-ring survivor is
-- 52 units), and budget = ceil(estimate / ticks). The last tick runs what
-- is left. One wall search is never cut: a point's plan ends in the slice
-- it starts in.
local SLICE_W_CAST, SLICE_W_SHOT, SLICE_W_NODE, SLICE_W_POINT = 2, 2, 2, 3
local SLICE_PER_SURVIVOR = 52
M.SLICE_W = { cast = SLICE_W_CAST, shot = SLICE_W_SHOT, node = SLICE_W_NODE, point = SLICE_W_POINT,
              per_survivor = SLICE_PER_SURVIVOR }

-- The slicer: s.fn(wu, est, fin) with wu = the evaluate's work so far, est
-- = the estimate of its whole work (given once, after the first ring's mask
-- pass; no yield before it), fin = the evaluate's end (no yield). s.slices, s.budget, and for the bench:
-- s.wu (the work of each slice), s.over (steps between two calls that
-- alone are over the budget: one plan's wall search, which is not cut).
-- The slicers by their fn (M.evaluate_c finds the slicer of inp.v2_slice).
local SLICERS = setmetatable({}, { __mode = "k" })
function M.slicer(inp, ticks, yield)
  ticks = ticks or 5
  local clk = inp.v2_clock
  local s = { budget = nil, slices = 1, wu = {}, over = 0, ticks = ticks, yield = yield }
  local start, last = 0, 0
  s.fn = function(wu, est, fin)
    if fin then s.wu[s.slices] = wu - start; return 0 end
    if est and not s.budget then
      local b = math.ceil(est / math.max(1, ticks))
      s.budget = b < 1 and 1 or b
    end
    local b = s.budget
    if b and wu - last > b then s.over = s.over + 1 end
    last = wu
    if not b or wu - start < b or s.slices >= ticks then return 0 end
    s.wu[s.slices] = wu - start
    start = wu
    s.slices = s.slices + 1
    local t = clk and clk()
    yield()
    return t and (clk() - t) or 0
  end
  SLICERS[s.fn] = s
  return s
end

-- PT2_V2_C: the C copy (gh_pt2_v2.c) of M.evaluate_lua, with the same
-- result. With inp.v2_slice (PT2_V2_SPLIT) the C stops where the Lua
-- calls the slicer, by the same rules, and this function yields there.
-- An exe with no gh_pt2_v2 (an older exe) runs the Lua; so do the
-- debug checks the C does not have (v2_mask_stats, v2_loss_check,
-- v2_confirm_check, v2_confirm_drop).
-- PT2_C_PROF (inp.v2_prof: C.PT2_PROF_LOG, or the bench's cprof=1): the C
-- evaluator's section profile (gh_pt2_v2.prof). Log only: it changes no
-- result. Each evaluate writes one PT2_C_PROF line (print2, debug only)
-- and one PT2EVAL line (stderr, see M.cprof_err); the
-- sums of a take plan (pt2_attack) and of the game are kept here:
--   M.cprof_plan, M.cprof_game  { evals, us, c_us, steps, yields,
--     sec = { [name] = { n, us } }, count = { [name] = n } }
-- us = the wall time in begin and step (the yields left out); c_us = the
-- sum of the C sections; us - c_us is the Lua glue around the C (the
-- loop in M.evaluate_c, the calls into the C).
local cprof_clock = rawget(_G, "clock_us") or rawget(_G, "bench_clock_us")
  or function() return os.clock() * 1e6 end
local function cprof_new() return { evals = 0, us = 0, c_us = 0, steps = 0, yields = 0, sec = {}, count = {} } end
M.cprof_plan, M.cprof_game = cprof_new(), cprof_new()
M.cprof_new = cprof_new
function M.cprof_add(acc, us, c, steps, yields)
  acc.evals = acc.evals + 1
  acc.us = acc.us + us
  acc.steps = acc.steps + steps
  acc.yields = acc.yields + yields
  for k, v in pairs(c) do
    if k == "count" then
      for ck, cv in pairs(v) do acc.count[ck] = (acc.count[ck] or 0) + cv end
    else
      local a = acc.sec[k]
      if not a then a = { n = 0, us = 0 }; acc.sec[k] = a end
      a.n = a.n + v.n
      a.us = a.us + v.us
      acc.c_us = acc.c_us + v.us
    end
  end
end
-- one line: the totals, then each section as name=calls/us (most time
-- first), then the counts
function M.cprof_line(tag, acc, head)
  local rows = {}
  for k, v in pairs(acc.sec) do rows[#rows + 1] = { k, v.n, v.us } end
  table.sort(rows, function(a, b) if a[3] ~= b[3] then return a[3] > b[3] end return a[1] < b[1] end)
  local parts = {}
  for i, r in ipairs(rows) do parts[i] = string.format("%s=%d/%.0f", r[1], r[2], r[3]) end
  local cn = {}
  for k, v in pairs(acc.count) do if v ~= 0 then cn[#cn + 1] = string.format("%s=%d", k, v) end end
  table.sort(cn)
  -- the Lua rows outside the evaluate (pt2_attack.build_inp)
  local lu = {}
  for k, v in pairs(acc.lua or {}) do lu[#lu + 1] = string.format("%s=%d/%.0f", k, v.n, v.us) end
  table.sort(lu)
  return string.format("%s %sevals=%d us=%.0f c_us=%.0f glue_us=%.0f steps=%d yields=%d | %s | %s | %s",
    tag, head or "", acc.evals, acc.us, acc.c_us, acc.us - acc.c_us, acc.steps, acc.yields,
    table.concat(parts, " "), table.concat(cn, " "), table.concat(lu, " "))
end
local cprof_print2 = nil
function M.cprof_print(line)
  if not BRAIN_DEBUG_MODE then return end
  cprof_print2 = cprof_print2 or require("print2")
  cprof_print2(line)
end
-- PT2_PROF_LOG: the plain stderr lines PT2EVAL (each evaluate) and PT2PLAN
-- (each take plan), also with no -brain-debug, for a timing bench. They go
-- to io.stderr, not print(): a release DS drops brain print() output.
function M.cprof_err(s)
  local e = io and io.stderr
  if e then e:write(s, "\n") end
end
-- the end of a take plan (pt2_attack): the plan's line and the game's sum
function M.cprof_plan_end(head)
  local pl = M.cprof_plan
  if pl.evals == 0 then return end
  M.cprof_err(string.format("PT2PLAN %sevals=%d ms=%.3f steps=%d", head or "", pl.evals, pl.us / 1000, pl.steps))
  M.cprof_print(M.cprof_line("PT2_C_PROF_PLAN", pl, head))
  M.cprof_print(M.cprof_line("PT2_C_PROF_GAME", M.cprof_game, head))
  M.cprof_plan = cprof_new()
end

function M.evaluate_c(inp)
  local g = rawget(_G, "gh_pt2_v2")
  if not g then
    if not M._warned_no_c then
      -- Log only, once per brain: a stale exe runs the Lua copy.
      M._warned_no_c = true
      print("[pilltake_v2] PT2_V2_C is on but this exe has no gh_pt2_v2:"
        .. " the Lua evaluator runs. Rebuild the exe.")
    end
    return M.evaluate_lua(inp)
  end
  if inp.v2_mask_stats or inp.v2_loss_check or inp.v2_confirm_check or inp.v2_confirm_drop then
    return M.evaluate_lua(inp)
  end
  -- M1: a lone evaluate checks the memo here; a group run in group_begin
  if inp.wall_memo and not inp._v2_cache then M.wall_memo_check(inp, inp.wall_memo_goal or inp.A) end
  local fn = inp.v2_slice
  local s = fn and SLICERS[fn]
  if fn and not s then return M.evaluate_lua(inp) end
  local pf = inp.v2_prof and g.prof_take
  -- v2_nozones (C.PT2_PROF_ZONES false): timed, but the C sections off
  local zones = pf and not inp.v2_nozones
  if zones then
    g.prof(true)
    g.prof_take()
    if g.prof_cut then g.prof_cut() end
  end
  local t0 = pf and cprof_clock()
  local paused, steps, yields = 0, 0, 0
  local DK = require("constants")
  inp._v2_dl_narrow = DK.PT2_V2_DOLL_NARROW_KEYS ~= false and 1 or 0
  local st = g.begin(inp, M, OLD, s and s.ticks or nil)
  while true do
    local done, res = g.step(st)
    steps = steps + 1
    if done then
      if s then
        local info = g.slices(st)
        s.slices, s.wu, s.over, s.budget = info.slices, info.wu, info.over, info.budget
      end
      if pf then
        local us = cprof_clock() - t0 - paused
        local c = {}
        if zones then
          c = g.prof_take()
          g.prof(false)
        end
        local one = cprof_new()
        M.cprof_add(one, us, c, steps, yields)
        M.cprof_add(M.cprof_plan, us, c, steps, yields)
        M.cprof_add(M.cprof_game, us, c, steps, yields)
        M.last_cprof = one
        local A = inp.A
        -- _prof_head: set by pt2_attack.build_inp (C.PT2_PROF_LOG) only
        if inp._prof_head then
          local w = one.sec.tank_shot_walk_angle
          M.cprof_err(string.format(
            "PT2EVAL %s A=(%s,%s) ms=%.3f steps=%d slice_ms=%.3f yields=%d c_ms=%.3f walks=%d walk_ms=%.3f",
            inp._prof_head, tostring(A and A.mx), tostring(A and A.my), us / 1000, steps,
            us / 1000 / steps, yields, one.c_us / 1000, w and w.n or 0, (w and w.us or 0) / 1000))
          -- inp.v2_zone_log (C.PT2_CPROF_LOG): this evaluate's section
          -- table as one PT2ZONES line; the lines of a game add up to the
          -- game's table
          if zones and inp.v2_zone_log then
            M.cprof_err(M.cprof_line("PT2ZONES", one, inp._prof_head .. " "))
            -- cache study (log only): stub / record set hashes and the
            -- planned singles of this evaluate (gh_pt2_v2.prof_cut)
            if g.prof_cut then
              M.cprof_err(string.format("PT2CUT %s A=(%s,%s) %s", inp._prof_head,
                tostring(A and A.mx), tostring(A and A.my), g.prof_cut()))
            end
          end
        end
        M.cprof_print(M.cprof_line("PT2_C_PROF", one, string.format("A=(%s,%s) ",
          tostring(A and A.mx), tostring(A and A.my))))
      end
      return res
    end
    if pf then
      local ty = cprof_clock()
      s.yield()
      paused = paused + (cprof_clock() - ty)
      yields = yields + 1
    else
      s.yield()
    end
  end
end

function M.evaluate_lua(inp)
  local A = inp.A
  local ctx = new_ctx(inp, A)
  local clk = ctx.clk
  local T0 = clk and clk()
  -- M1: a lone evaluate checks the memo here; a group run in group_begin
  if inp.wall_memo and not inp._v2_cache then M.wall_memo_check(inp, inp.wall_memo_goal or A) end
  -- PT2_V2_SPLIT: the slicer (see M.slicer); paused: the time between ticks
  local slice_fn, paused = inp.v2_slice, 0
  local surv1 = nil
  -- the mask pass's points of one ring (P1b), reused ring to ring
  local PP_deg, PP_cx, PP_cy, PP_mx, PP_my, PP_px, PP_py, PP_hasA, PP_cant =
    {}, {}, {}, {}, {}, {}, {}, {}, {}
  local st = ctx.stat
  local n_eval = 0
  -- the work so far, in slice units (P1b)
  local function work()
    return SLICE_W_CAST * (st.casts + st.casts_confirm) + SLICE_W_SHOT * ctx.n_shot
      + SLICE_W_NODE * (st.wall_nodes - ctx.memo_nodes) + SLICE_W_POINT * n_eval
  end
  -- a slice the slicer starts is a new step: the map may change there
  local slicer_st = slice_fn and SLICERS[slice_fn]
  local function unit(est, fin)
    if slice_fn then
      local s0 = slicer_st and slicer_st.slices
      paused = paused + (slice_fn(work(), est, fin) or 0)
      if not slicer_st or slicer_st.slices ~= s0 then ctx.aw_gen = ctx.aw_gen + 1 end
    end
  end
  local pcx, pcy = A.mx + 0.5, A.my + 0.5
  local all, best = {}, nil
  local n_reject = 0
  local tm = ctx.tm
  local t0 = clk and clk()
  local asets = aim_wedges(ctx, A)
  if t0 then tm.wedges = tm.wedges + clk() - t0 end
  local cap, band = inp.leeway_min, inp.travel_band
  -- S9: lazy single takes (not with keep_all: every point is in the result
  -- then; not without a travel band: nothing to stop at)
  local lazy = (not inp.keep_all) and band and band > 0 or false
  -- S10 chains only: a run whose result counts only as a safe chain.
  -- Single takes get no plan; a ring with no safe chain stops the run
  -- ("no chain") when it has a safe single take (that take would win).
  local co = inp.chains_only and true or false
  ctx.chains_only = co
  if co then lazy = true end
  local no_chain = nil
  -- PT2_FARTHEST_RING: the rings are ring_radii (absolute, far to near),
  -- every one runs; else the old rings (radius + radii_offsets), an inner
  -- one only when the ones before found nothing safe
  local far = M.far_on(inp)
  local rings = {}
  if far then
    for i, r in ipairs(inp.ring_radii) do rings[i] = r end
  else
    for i, roff in ipairs(inp.radii_offsets) do rings[i] = inp.radius + roff end
  end
  -- far: a ring stopped "no chain" (a safe single take, no safe chain);
  -- the run goes on, a nearer ring may have a safe chain
  local far_single = false
  -- PT2_QUICK_SIDE_FIRST: the lowest score per safe and kills over the rings
  local qfloor = (inp.quick_side_first and band and band > 0) and {} or nil
  for _, R in ipairs(rings) do
    local cands = {}
    local mpts = {}
    local nsteps = floor(360 / inp.angle_step + 0.5)
    -- P1b: the mask pass first (the square tests, pills in range, the mask
    -- test of every point of the ring), then the points it keeps; the
    -- first ring's survivors give the slicer its estimate
    local np = 0
    for s = 0, nsteps - 1 do
      local deg = s * inp.angle_step
      if not (inp.banned and inp.banned(deg)) then
        local cx = pcx + sin(rad(deg)) * R
        local cy = pcy - cos(rad(deg)) * R
        local mx, my = floor(cx), floor(cy)
        if inp.in_map(mx, my) and inp.stay_in(mx, my) and inp.standable(mx, my) then
          n_eval = n_eval + 1
          local px, py = floor(cx * 256), floor(cy * 256)
          local hasA = A_in_range(ctx, px, py)
          local is_cant = false
          if hasA then
            local t1 = clk and clk()
            is_cant = cant(ctx, asets, px, py)
            if t1 then tm.masks = tm.masks + clk() - t1 end
            if is_cant then st.cant = st.cant + 1 else st.survive = st.survive + 1 end
          end
          np = np + 1
          PP_deg[np], PP_cx[np], PP_cy[np], PP_mx[np], PP_my[np] = deg, cx, cy, mx, my
          PP_px[np], PP_py[np], PP_hasA[np], PP_cant[np] = px, py, hasA, is_cant
        end
      end
    end
    if not surv1 then
      surv1 = st.survive
      unit(SLICE_PER_SURVIVOR * surv1)
    end
    for q = 1, np do
      local deg, cx, cy, mx, my = PP_deg[q], PP_cx[q], PP_cy[q], PP_mx[q], PP_my[q]
      local px, py, hasA, is_cant = PP_px[q], PP_py[q], PP_hasA[q], PP_cant[q]
      do
        do
          local rec, why
          if not hasA then
            why = "A_OUT_OF_RANGE"
          else
            if is_cant then
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
              rec, why = eval_point(ctx, px, py, nil, nil, true, false, lazy)
              if rec and inp.v2_mask_stats then
                mpts[#mpts + 1] = { px = px, py = py, T = (pills_in_range(ctx, px, py)), rec = rec }
              end
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
          unit()
        end
      end
    end
    -- the masks' chains at the kept points (MUST over the DFS), set beside
    -- the exact chain for the logs and the bench
    if inp.v2_mask_stats and #mpts > 0 then
      local t1 = clk and clk()
      local list, at, sure = chains_dfs(ctx, mpts, A)
      if t1 then tm.dfs = tm.dfs + clk() - t1 end
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
    local ts = clk and clk()
    if not lazy then table.sort(cands, pre_rank) end
    if ts then tm.sort = tm.sort + clk() - ts end
    -- lone pill: no other hostile pill in range of any point
    local lone = #cands > 0
    for _, rec in ipairs(cands) do if #rec.T ~= 1 then lone = false; break end end
    local scored = {}
    local singles_co, kmin_co = nil, nil
    if lazy then
      -- S9: the chain points are planned first. The single takes then go in
      -- order of the least score they can have (base + build_cost * the
      -- fewest blockers A's ray needs) and stop above m + band, m the least
      -- score of a safe single take so far: a single take that is not
      -- planned cannot be in the band of m. A plan is capped at the size
      -- that can still reach the band.
      local singles = {}
      for _, rec in ipairs(cands) do
        if rec.lazy then singles[#singles + 1] = rec
        else
          local fin = plan_point(ctx, rec)
          carry(fin, rec)
          score_of(ctx, fin)
          scored[#scored + 1] = fin
          unit()
        end
      end
      local bc = inp.build_cost
      local sb = inp.wall_shots
      if co then singles_co = singles end
      if (inp.num_pill_blockers or 0) > 0 and inp.pill_drop_shots > sb then sb = inp.pill_drop_shots end
      local function kmin_of(need)
        if need <= 0 then return 0 end
        return math.ceil(need / sb)
      end
      if co then kmin_co = kmin_of end
      for _, q in ipairs(co and {} or singles) do q.lb = q.base + kmin_of(q.a_need) * bc end
      table.sort(singles, function(a, b)
        if a.lb ~= b.lb then return a.lb < b.lb end
        return a.deg < b.deg
      end)
      local m = nil
      for i, q in ipairs(co and {} or singles) do
        if m and q.lb > m + band then
          st.plan_skipped = st.plan_skipped + (#singles - i + 1)
          break
        end
        local full = eval_point(ctx, q.px, q.py, nil, q.aim_idx, false, false)
        full.nudge_wu = q.nudge_wu
        full.deg, full.cx, full.cy, full.mx, full.my, full.radius = q.deg, q.cx, q.cy, q.mx, q.my, q.radius
        full.base, full.omx, full.omy = q.base, q.omx, q.omy
        local fin = nil
        if not full.covered and m and bc > 0 then
          local kmax = floor((m + band - full.base) / bc + 1e-9)
          local km = 0
          for _, ui in ipairs(full.uncovered) do
            local r = full.rays[ui]
            local k = kmin_of(r.bar - r.cover)
            if k > km then km = k end
          end
          if km > kmax then
            full.plan, full.extra = {}, {}
            st.plan_cut = st.plan_cut + 1
            fin = full
          else
            full.kmax = kmax
          end
        end
        if not fin then fin = plan_point(ctx, full) end
        carry(fin, full)
        score_of(ctx, fin)
        scored[#scored + 1] = fin
        if fin.safe and fin.kills == 1 and (not m or fin.score < m) then m = fin.score end
        unit()
      end
    elseif lone and band and band > 0 then
      st.lone = st.lone + 1
      -- the points that stand covered now need no plan; the others are
      -- planned in base order until none left can reach the travel band
      local m = nil
      local need = {}
      for _, rec in ipairs(cands) do
        if rec.covered then
          rec.plan, rec.extra = {}, {}
          score_of(ctx, rec)
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
        score_of(ctx, fin)
        done[rec] = fin
        if fin.safe and (not m or fin.score < m) then m = fin.score end
        unit()
      end
      for _, rec in ipairs(cands) do
        local fin = rec.covered and rec or done[rec]
        if not fin then
          fin = rec
          fin.plan, fin.extra, fin.unsearched = {}, {}, true
          score_of(ctx, fin)
          st.plan_skipped = st.plan_skipped + 1
        end
        scored[#scored + 1] = fin
      end
    else
      for _, rec in ipairs(cands) do
        local fin = plan_point(ctx, rec)
        carry(fin, rec)
        score_of(ctx, fin)
        scored[#scored + 1] = fin
        unit()
      end
    end
    ts = clk and clk()
    OLD.set_band(scored, band)
    table.sort(scored, function(a, b) return M.better(a, b, cap, band, inp.prefer_existing_cover, inp.start_while_building, false, far, inp) end)
    if ts then tm.sort = tm.sort + clk() - ts end
    if inp.leeway_on then
      -- the first leeway_top points of this rank (the single takes among
      -- them get leeway below when no safe chain is left)
      local top = {}
      for i = 1, min(inp.leeway_top, #scored) do top[i] = scored[i] end
      -- S8: a chain never wins untested. X is the first safe chain; when X
      -- has no leeway yet, test it. X passes: the lowest score safe chain
      -- of X's tier (it sets the tier's band) is tested too. A chain that
      -- fails is unsafe and the rank runs again. Only these chains are
      -- tested (no fixed top leeway_top).
      local function rerank()
        local t2 = clk and clk()
        OLD.set_band(scored, band)
        table.sort(scored, function(a, b) return M.better(a, b, cap, band, inp.prefer_existing_cover, inp.start_while_building, false, far, inp) end)
        if t2 then tm.sort = tm.sort + clk() - t2 end
      end
      local X = nil
      local again = true
      while again do
        again = false
        X = nil
        for _, r in ipairs(scored) do
          if r.safe and r.kills >= 2 then X = r; break end
        end
        if X then
          local L = X
          if X.leeway ~= nil then
            for _, r in ipairs(scored) do
              if r.safe and r.kills == X.kills and r.score < L.score then L = r end
            end
          end
          if L.leeway == nil then
            unit()
            L.leeway, L.leeway_pts = leeway(ctx, L, inp.leeway_ring_wu)
            st.leeway_more = st.leeway_more + 1
            if L.leeway < inp.leeway_min then
              L.safe, L.why_unsafe = false, "LEEWAY"
              st.leeway_more_fail = st.leeway_more_fail + 1
            end
            again = true
            rerank()
          end
        end
      end
      -- no safe chain: a single take wins. Leeway never makes one unsafe,
      -- but it ranks them: the single takes of the first rank's top get it,
      -- as before.
      if not X then
        local any = false
        for _, rec in ipairs(top) do
          if rec.kills < 2 and rec.leeway == nil then
            rec.leeway, rec.leeway_pts = leeway(ctx, rec, inp.leeway_ring_wu)
            st.leeway_single = st.leeway_single + 1
            any = true
          end
        end
        if any then rerank() end
      end
    end
    -- S10 ring stop: no safe chain in this ring. A safe single take here
    -- would win the full run, so the run stops with no chain. The single
    -- takes are judged covered now first (no plan), then in S9 order
    -- (base + build_cost * kmin) until the first safe one.
    if co and singles_co and #singles_co > 0 and not (far and (far_single or (best and best.safe and best.kills >= 2))) then
      local has_chain = false
      for _, r in ipairs(scored) do
        if r.safe and r.kills >= 2 then has_chain = true; break end
      end
      if not has_chain then
        local bc = inp.build_cost
        for _, q in ipairs(singles_co) do
          if q.a_need == nil then
            q.a_need = q.a_bar - ray_view(base_ray(ctx, A, q.px, q.py), nil).cover
          end
          q.kmin = kmin_co(q.a_need)
          q.lb = q.base + q.kmin * bc
        end
        table.sort(singles_co, function(a, b)
          if (a.kmin == 0) ~= (b.kmin == 0) then return a.kmin == 0 end
          if a.lb ~= b.lb then return a.lb < b.lb end
          return a.deg < b.deg
        end)
        for _, q in ipairs(singles_co) do
          st.co_single_eval = st.co_single_eval + 1
          local full = eval_point(ctx, q.px, q.py, nil, q.aim_idx, false, false)
          full.nudge_wu = q.nudge_wu
          full.deg, full.cx, full.cy, full.mx, full.my, full.radius = q.deg, q.cx, q.cy, q.mx, q.my, q.radius
          full.base, full.omx, full.omy = q.base, q.omx, q.omy
          local fin = plan_point(ctx, full)
          carry(fin, full)
          if fin.safe then no_chain = "single"; break end
          unit()
        end
      end
    end
    if no_chain and far then
      far_single, no_chain = true, nil
    elseif no_chain then st.co_stop = st.co_stop + 1; best = nil; break
    else
      for _, rec in ipairs(scored) do all[#all + 1] = rec end
      if qfloor then M.qsf_note(qfloor, scored) end
      if scored[1] and (not best or M.better(scored[1], best, cap, band, inp.prefer_existing_cover,
                                             inp.start_while_building, false, far, inp,
                                             qfloor and qfloor[M.qsf_key(scored[1])])) then
        best = scored[1]
      end
      -- PT2_FARTHEST_RING: every ring runs (a nearer one may have more kills)
      if best and best.safe and not far then break end
    end
  end
  if far_single and not (best and best.safe and best.kills >= 2) then
    no_chain = "single"
    st.co_stop = st.co_stop + 1
    best = nil
  end
  if best and best.safe and best.plan and #best.plan > 0 then
    if inp.v2_confirm_check then confirm_exact(ctx, best) end
    best.hold = OLD.hold_point(ctx, best)
  end
  -- E5-13: a group run asks for the extra walls once, on its final result
  if best and best.safe and (inp.extra_cover_walls or 0) > 0 and not inp._xcover_later then
    local t3 = clk and clk()
    best.extra_cover = extra_cover(ctx, best)
    if t3 then tm.xcover = tm.xcover + clk() - t3 end
  end
  unit(nil, true)
  if T0 then tm.total = clk() - T0 - paused end
  M.last_work = { casts = st.casts + st.casts_confirm, shots = ctx.n_shot,
                  nodes = st.wall_nodes, points = n_eval, cant = st.cant, survive = st.survive,
                  planned = st.planned, surv1 = surv1 }
  return { best = best, all = inp.keep_all and all or nil, n_eval = n_eval, n_reject = n_reject,
           hostile = ctx.hostile, inp = inp, no_chain = no_chain, qsf_floor = qfloor,
           v2 = { stat = st, tm = tm } }
end

-- ── PT2_V2_CACHE: the slow cache store ────────────────────────────────
-- A group job's cache table (rays, square reads and the C job with its
-- ring point store, gh_pt2_v2.c) is kept from run to run of one target:
-- one entry per (source, goal pill), the newest last, at most
-- PT2_V2_CACHE_MAX_ENTRIES per source. The source is "dbl" (pt2_dbl's
-- shadow double-take runs) or "take" (the real take, pt2_attack). An
-- entry is dropped when an input that v2's kept parts read has changed
-- in its watch area: a pill (place, alive, in tank, place unknown, owner,
-- cover), a base (owner, dead, capture armour), the shell rules (in boat,
-- base hit, base capture), the terrain (a forest square read as grass) or
-- the age (PT2_V2_CACHE_MAX_AGE_TICKS). A forest change only (cut or
-- grown) drops the kept plans (the builder's walk reads forest), and the
-- rays stay: PT2_FOREST_TRANSPARENT. With PT2_FOREST_TRANSPARENT off a
-- forest change drops the entry. Knob off: a new cache table for each
-- job, as before.
--
-- The watch area (PT2_V2_CACHE_MARGIN, gh_pt2_v2.c l_pc_watch) is made at
-- the end of each group run from what the C marked. The watched pills are
-- the group's pills (each run's A) and each pill within reach + margin of
-- a ring point (reach = pill_range + range_margin, as v2's). The terrain
-- and the bases watched: those pills' disks (reach + margin), a disk of
-- margin round each ring point and builder origin, and each square a run
-- read. The pills watched: a pill on any watched square, or on a square
-- within reach + margin of a ring point (a new pill there drops it).
-- A group run can span ticks: the pills, the bases and the map are taken
-- at the run's start, and the entry's strings and hashes come from that
-- start, over the watch area made at the run's end. An entry whose last
-- run did not end is dropped ("open").
local PCK = require("constants")
local PCS = {}                    -- [src] = { entry, ... }
M.pc_count = { checks = 0, diffs = 0, kept = 0, new = 0, age = 0, pill = 0, base = 0,
               rules = 0, terrain = 0, forest = 0, full = 0, open = 0,
               w_n = 0, w_pills = 0, w_sq = 0, w_ring = 0,
               w_out = { tt = 0, po = 0, sqf = 0, base = 0, walk = 0 }, w_out_max = 0 }

local function pc_src(inp)
  if inp._v2_pc_off or not (PCK.PT2_V2_CACHE or PCK.PT2_V2_DOLL) or not inp.v2_c then return nil end
  if not rawget(_G, "gh_pt2_v2") or not inp._world or not inp.A then return nil end
  if (inp.capture_file and inp.capture_file ~= "") or inp.v2_mask_stats or inp.v2_loss_check
     or inp.v2_confirm_check or inp.v2_confirm_drop then return nil end
  local src = inp._v2_pc_src or "take"
  if src == "take" and PCK.PT2_V2_CACHE_TAKE == false and not PCK.PT2_V2_DOLL then return nil end
  -- PT2_V2_DOLL_TAKE_STORE = false: a single pill take keeps no doll job
  if src == "take" and PCK.PT2_V2_DOLL and PCK.PT2_V2_DOLL_TAKE_STORE == false then return nil end
  return src
end

-- The pills, bases and shell rules now: { mx, my, string } lists.
local function pc_take(inp, w)
  local env = inp.env
  local P = {}
  for _, p in ipairs(inp.pills or {}) do
    local mx, my = p.mx, p.my
    if mx and my then
      -- health as alive only: v2 reads no other health (A's 1 health for
      -- finish is in the C key; an ally's cover is its own field)
      P[#P + 1] = { mx, my, string.format("%d,%d,%s,%s,%s,%s,%s", mx, my, tostring((p.health or 0) > 0),
        tostring(p.in_tank), tostring(p.pos_unknown), tostring(p.owner),
        tostring(env.cover_of_pill and env.cover_of_pill(p))),
        (PCK.PT2_V2_CACHE_DROP_LOG or PCK.PT2_V2_CACHE_PART_LOG or PCK.PT2_V2_DOLL) and inp.ids and inp.ids[p] or nil,
        (PCK.PT2_V2_CACHE_PART_LOG or PCK.PT2_V2_DOLL) and (env.is_ally and env.is_ally(p) and true or false) or nil }
    end
  end
  local B = {}
  local capmin = PCK.BASE_MIN_ARMOUR_CAPTURE or 0
  for k, e in pairs(w.base_at or {}) do
    local bs = e and e.base
    B[#B + 1] = { k % 256, math.floor(k / 256), string.format("%d:%s:%s:%s", k, tostring(bs and bs.owner),
      tostring(bs and (bs.health or 1) <= 0), tostring(bs and (bs.health or 0) > capmin)) }
  end
  local sr = w._shell_rules
  local ss = sr and (tostring(sr.inboat) .. "," .. tostring(sr.hit) .. "," .. tostring(sr.cap)) or "-"
  return P, B, ss
end

-- The pill and base strings over a watch W (sorted, so the order of the
-- lists does not count).
local function pc_wstr(P, B, W)
  local wq = gh_pt2_v2.pc_wq
  local t, b = {}, {}
  for _, x in ipairs(P) do if wq(W, x[1], x[2]) ~= 0 then t[#t + 1] = x[3] end end
  for _, x in ipairs(B) do if wq(W, x[1], x[2]) % 2 == 1 then b[#b + 1] = x[3] end end
  table.sort(t); table.sort(b)
  return table.concat(t, ";"), table.concat(b, ";")
end

-- A dropped entry's C store is freed now: Lua does not see that memory, so
-- the job's GC can come late (a busy job keeps it to its GC).
local function pc_free(e)
  local j = e.cache and e.cache.c_job
  local f = rawget(_G, "gh_pt2_v2") and gh_pt2_v2.pc_free
  -- PT2_V2_DOLL_MEM_LOG (log only): the job's records to the end sums
  if j and PCK.PT2_V2_DOLL_MEM_LOG and gh_pt2_v2.dl_mem_end then
    gh_pt2_v2.dl_mem_end(j, e.src == "take" and 0 or (e.src == "dbl" and 1 or 2))
  end
  if j and f then f(j) end
end

-- PT2_V2_CACHE_DROP_LOG (log only): a PT2_V2_CACHE_DROP line per lookup.
-- PCH[src][key] = { t, why }: how the goal pill's last entry went.
local PCH = {}
local PCL_T = { [0] = "building", "river", "swamp", "crater", "road", "forest", "rubble", "grass",
  "halfbuilding", "boat", "mine_swamp", "mine_crater", "mine_road", "mine_forest", "mine_rubble",
  "mine_grass", [255] = "sea" }
local PCL_PF = { "mx", "my", "alive", "in_tank", "pos_unknown", "owner", "cover" }
local PCL_BF = { "k", "owner", "dead", "cap" }
local PCL_SF = { "inboat", "hit", "cap" }
local function pcl_split(str, sep)
  local t = {}
  for v in string.gmatch(str .. sep, "([^" .. sep .. "]*)" .. sep) do t[#t + 1] = v end
  return t
end
-- the fields of a and b that differ, as items "what@x,y:field:old>new"
local function pcl_fields(out, what, sq, a, b, sep, names, from)
  local fa, fb = pcl_split(a, sep), pcl_split(b, sep)
  for i = from, #names do
    if fa[i] ~= fb[i] then
      out[#out + 1] = string.format("%s@%s:%s:%s>%s", what, sq, names[i], tostring(fa[i]), tostring(fb[i]))
    end
  end
end
-- the watched pills of the entry's last start (Pf) against now (P), by
-- the world pill id (x[4]); a pill that leaves or joins the watch is a
-- "place" change, a pill with no old copy is "new"
local function pcl_pills(Pf, P, W)
  local wq = gh_pt2_v2.pc_wq
  local o, n, ids = {}, {}, {}
  for _, x in ipairs(Pf or {}) do
    local id = x[4] or -1
    o[id] = x; ids[id] = true
  end
  for _, x in ipairs(P) do
    local id = x[4] or -1
    n[id] = x; ids[id] = true
  end
  local list = {}
  for id in pairs(ids) do list[#list + 1] = id end
  table.sort(list)
  local out = {}
  for _, id in ipairs(list) do
    local a, b = o[id], n[id]
    local wa = a and wq(W, a[1], a[2]) ~= 0
    local wb = b and wq(W, b[1], b[2]) ~= 0
    if wa or wb then
      if wa and wb then
        if a[3] ~= b[3] then
          local sq = a[1] .. "," .. a[2]
          if a[1] ~= b[1] or a[2] ~= b[2] then
            out[#out + 1] = string.format("pill%d@%s:place:%s>%d,%d", id, sq, sq, b[1], b[2])
          end
          pcl_fields(out, "pill" .. id, sq, a[3], b[3], ",", PCL_PF, 3)
        end
      elseif wa then
        out[#out + 1] = string.format("pill%d@%d,%d:place:%d,%d>%s", id, a[1], a[2], a[1], a[2],
          b and (b[1] .. "," .. b[2] .. "(unwatched)") or "gone")
      elseif a then
        out[#out + 1] = string.format("pill%d@%d,%d:place:%d,%d(unwatched)>%d,%d", id, b[1], b[2], a[1], a[2], b[1], b[2])
      else
        out[#out + 1] = string.format("pill%d@%d,%d:new:none>%s", id, b[1], b[2], b[3])
      end
    end
  end
  return out
end
local function pcl_bases(Bf, B, W)
  local wq = gh_pt2_v2.pc_wq
  local o, n, ks = {}, {}, {}
  for _, x in ipairs(Bf or {}) do
    if wq(W, x[1], x[2]) % 2 == 1 then local k = x[2] * 256 + x[1]; o[k] = x; ks[k] = true end
  end
  for _, x in ipairs(B) do
    if wq(W, x[1], x[2]) % 2 == 1 then local k = x[2] * 256 + x[1]; n[k] = x; ks[k] = true end
  end
  local list = {}
  for k in pairs(ks) do list[#list + 1] = k end
  table.sort(list)
  local out = {}
  for _, k in ipairs(list) do
    local a, b = o[k], n[k]
    local sq = (k % 256) .. "," .. math.floor(k / 256)
    if a and b then
      if a[3] ~= b[3] then pcl_fields(out, "base", sq, a[3], b[3], ":", PCL_BF, 2) end
    else
      out[#out + 1] = string.format("base@%s:%s:%s>%s", sq, a and "gone" or "new", a and a[3] or "none", b and b[3] or "none")
    end
  end
  return out
end
-- the changed watched squares (at most 8 listed), "sq@x,y:terrain|forest:old>new"
local function pcl_squares(W)
  local n, nt, nf, l = gh_pt2_v2.pc_wdiff(W, 8)
  local out = {}
  if not n then return out, 0 end
  for _, x in ipairs(l) do
    out[#out + 1] = string.format("sq@%d,%d:%s:%s>%s", x[1] % 256, math.floor(x[1] / 256),
      x[4] == 1 and "terrain" or "forest", PCL_T[x[2]] or tostring(x[2]), PCL_T[x[3]] or tostring(x[3]))
  end
  return out, n
end
local function pcl_line(inp, src, key, res, why, age, items, n, extra)
  local now = inp._v2_pc_now or 0
  local gs = math.floor(now * 2 / 100)
  M.cprof_err(string.format("PT2_V2_CACHE_DROP t=%d gt=%d:%02d p=%s src=%s goal=%d,%d res=%s why=%s age=%s n=%d%s d=%s",
    now, math.floor(gs / 60), gs % 60, tostring(inp._v2_pc_pn), src, math.floor(key / 256), key % 256,
    res, why or "-", tostring(age or "-"), n or #items, extra or "", #items > 0 and table.concat(items, ";") or "-"))
end
-- a drop's items and their count
local function pcl_detail(e, why, P, B, ss)
  if why == "pill" then local t = pcl_pills(e.Pf, P, e.W); return t, #t end
  if why == "base" then local t = pcl_bases(e.Bf, B, e.W); return t, #t end
  if why == "rules" then
    local t = {}
    pcl_fields(t, "rules", "-", e.ss or "", ss or "", ",", PCL_SF, 1)
    return t, #t
  end
  if why == "terrain" or why == "forest" then return pcl_squares(e.W) end
  return {}, 0
end

-- PT2_V2_CACHE_PART_LOG (log only): the pill changes from Pf to P by id,
-- as pc_part_q takes them { o_mx, o_my, n_mx, n_my, fh, fa, ... } (fh: a
-- hostile pill's place, alive, in_tank, pos_unknown, owner, or it comes or
-- goes; fa: an ally pill's any change), and whether an ally pill's in_tank
-- changed (a carried pill)
local function pcp_pills(Pf, P)
  local o, n, ids = {}, {}, {}
  local function id_of(x) return x[4] or ("s" .. x[1] .. "," .. x[2]) end
  for _, x in ipairs(Pf or {}) do local id = id_of(x); o[id] = x; ids[id] = true end
  for _, x in ipairs(P or {}) do local id = id_of(x); n[id] = x; ids[id] = true end
  local out, car, nh, na = {}, false, 0, 0
  for id in pairs(ids) do
    local a, b = o[id], n[id]
    if not (a and b and a[3] == b[3] and a[5] == b[5]) then
      local fh
      if a and b then
        local fa_, fb_ = pcl_split(a[3], ","), pcl_split(b[3], ",")
        fh = a[5] ~= b[5]
        for i = 1, 6 do if fa_[i] ~= fb_[i] then fh = true end end
        if fa_[4] ~= fb_[4] and (a[5] or b[5]) then car = true end
      else
        fh = true
        if (a and a[5]) or (b and b[5]) then car = car or ((a or b)[3]:match("^[^,]*,[^,]*,[^,]*,true") ~= nil) end
      end
      fh = fh and ((a and not a[5]) or (b and not b[5])) and 1 or 0
      local fa = ((a and a[5]) or (b and b[5])) and 1 or 0
      out[#out + 1] = a and a[1] or -1; out[#out + 1] = a and a[2] or -1
      out[#out + 1] = b and b[1] or -1; out[#out + 1] = b and b[2] or -1
      out[#out + 1] = fh; out[#out + 1] = fa
      nh, na = nh + fh, na + fa
    end
  end
  return out, car, nh, na
end
-- the changed bases from Bf to B { mx, my, ... }
local function pcp_bases(Bf, B)
  local o, out = {}, {}
  for _, x in ipairs(Bf or {}) do o[x[2] * 256 + x[1]] = x end
  local seen = {}
  for _, x in ipairs(B or {}) do
    local k = x[2] * 256 + x[1]
    seen[k] = true
    if not (o[k] and o[k][3] == x[3]) then out[#out + 1] = x[1]; out[#out + 1] = x[2] end
  end
  for k, x in pairs(o) do if not seen[k] then out[#out + 1] = x[1]; out[#out + 1] = x[2] end end
  return out
end
local function pcp_reach(inp)
  return ((inp.pill_range or 0) + (inp.range_margin or 0)) / 256, PCK.PT2_V2_CACHE_MARGIN or 3
end
-- one PT2_V2_CACHE_PART line: a lookup's result and what its parts keep
local function pcp_line(inp, src, key, e, res, why, P, B, now)
  local pills, car, nh, na = pcp_pills(e.Pf, P)
  local bases = pcp_bases(e.Bf, B)
  local reach, margin = pcp_reach(inp)
  local st = gh_pt2_v2.pc_part_q(e.part, pills, bases, reach, margin, nil, car)
  M.cprof_err(string.format("PT2_V2_CACHE_PART t=%d p=%s src=%s goal=%d,%d res=%s why=%s age=%d ph=%d pa=%d car=%s %s",
    now, tostring(inp._v2_pc_pn), src, math.floor(key / 256), key % 256, res, why or "-", now - e.tick, nh, na,
    tostring(car), st or "-"))
end
-- PXS[src][k] = the last parts of the evaluate with A on square k (any goal)
local PXS = {}
local PCP_RUN = 0

-- ── PT2_V2_DOLL: one kept job per source for the whole game ──────────
-- PDS[src] = { cache, t0, last, P, B, ss, open }. Each group run first
-- syncs the C job (gh_pt2_v2.doll_sync) with the changes since the last
-- run: the C drops the parts they hit. A new job when the entry is older
-- than PT2_V2_CACHE_MAX_AGE_TICKS, the shell rules changed, the last run
-- did not end, the clock went back, or the C lost a part's marks.
local PDS = {}
M.dl_count = { runs = 0, new = 0, open = 0, time = 0, age = 0, rules = 0, lost = 0, busy = 0, nojob = 0,
               nomap = 0, sync = 0, changed = 0, sync_us = 0, own = 0, nsq = 0, nmid = 0, slots = 0,
               n_ray = 0, n_pt = 0, n_pl = 0, n_flt = 0, d_ray = 0, d_pt = 0, d_pl = 0, d_flt = 0,
               d_sqm = 0, d_sqf = 0, bytes = 0, bytes_max = 0, btot_max = 0,
               b_store = 0, b_rebuilt = 0, b_drop = 0, wn_us = 0, wn_tests = 0, wn_keep = 0, wn_fkeep = 0,
               wn_slots = 0 }
-- summed on each sync (changed or not)
local DL_ADD2 = { "b_store", "b_rebuilt", "b_drop", "wn_us", "wn_tests", "wn_keep", "wn_fkeep", "wn_slots" }
local DL_ADD = { "own", "nsq", "nmid", "slots", "n_ray", "n_pt", "n_pl", "n_flt", "d_ray", "d_pt", "d_pl",
                 "d_flt", "d_sqm", "d_sqf" }
-- every kept job's bytes now (the C's count), by source
function M.dl_bytes()
  local t, n = {}, 0
  for src, e in pairs(PDS) do t[src] = e.bytes or 0; n = n + (e.bytes or 0) end
  return n, t
end
-- PT2_V2_DOLL_EVENT_LOG (log only): each C store lookup, drop and reset
-- in a binary file dl_p<player>.bin in the debug session dir (a string
-- knob value: that dir). tools/dl_events.py reads it. A -brain-debug run
-- with PT2_V2_DOLL_VIZ_RECORD (debug only) writes it too, so
-- "dl_events.py at" can say why each recorded layer has its colour.
local DLE_CODE = { open = 1, time = 2, age = 3, rules = 4, lost = 5, busy = 6, nojob = 7, nomap = 8 }
local function dle_on()
  return PCK.PT2_V2_DOLL_EVENT_LOG or (rawget(_G, "BRAIN_DEBUG_MODE") and PCK.PT2_V2_DOLL_VIZ_RECORD) or false
end
local dle_state
local function dle_open(inp)
  if dle_state ~= nil then return dle_state end
  dle_state = false
  local K = dle_on()
  if not K or not gh_pt2_v2.dl_ev_open then return false end
  local dir = type(K) == "string" and K or rawget(_G, "DEBUG_SESSION_DIR")
  if not dir then return false end
  local path = dir .. "/dl_p" .. tostring(inp._v2_pc_pn or "x") .. ".bin"
  dle_state = gh_pt2_v2.dl_ev_open(path) and true or false
  return dle_state
end
local function doll_cache(inp, src, now)
  local cnt = M.dl_count
  -- the doll's memory knobs (C, per Lua state; no result change)
  if gh_pt2_v2.doll_opts then gh_pt2_v2.doll_opts(PCK.PT2_V2_DOLL_REAL_FILL ~= false) end
  local dle = dle_on() and dle_open(inp)
  -- BrainTest doll layers (pt2_viz.lua, debug only): C keeps a copy of
  -- each record for them (with no event log: no file)
  local dsrc = src == "take" and 0 or 1
  inp._v2_dl_src = dsrc
  local P, B, ss = pc_take(inp, inp._world)
  local e = PDS[src]
  local why
  if e then
    local j = e.cache.c_job
    -- PT2_V2_DOLL_KEEP_OPEN: a run that did not end (a split job dropped
    -- between evaluates) keeps the job: each evaluate is whole, and the
    -- sync drops what the world changes hit. A run dropped inside an
    -- evaluate leaves the C job busy: the sync says "busy", a new job.
    if e.open and not (j and PCK.PT2_V2_DOLL_KEEP_OPEN ~= false) then why = "open"
    elseif now < e.last then why = "time"
    elseif now - e.t0 > (PCK.PT2_V2_CACHE_MAX_AGE_TICKS or 3000) then why = "age"
    elseif e.ss ~= ss then why = "rules" end
    if not why and j then
      local pills = pcp_pills(e.P, P)
      local bases = pcp_bases(e.B, B)
      local reach, margin = pcp_reach(inp)
      -- PT2_V2_DOLL_PILL_NARROW: the pill shoot radius S (squares) of
      -- rule B; -1 = the wide rule
      local S = -1
      if PCK.PT2_V2_DOLL_PILL_NARROW and PCK.PT2_V2_DOLL_RULE ~= "exact" then
        S = (inp.pill_range or 0) / 256 + (PCK.PT2_V2_DOLL_PILL_SHOOT_R_EXTRA or 0.5)
      end
      local st = gh_pt2_v2.doll_sync(j, pills, bases, reach, margin, inp.env.forest_transparent ~= false,
        PCK.PT2_V2_DOLL_RULE == "exact", now, PCK.PT2_V2_DOLL_SLOT_DEAD_MAX or 0, S,
        PCK.PT2_V2_DOLL_WEDGE_NARROW or 0)
      if type(st) ~= "table" then why = tostring(st)
      else
        cnt.sync = cnt.sync + 1
        cnt.sync_us = cnt.sync_us + (st.us or 0)
        e.bytes = st.bytes or e.bytes
        e.b_ar, e.b_par, e.b_pcc, e.n_obj = st.b_ar or e.b_ar, st.b_par or e.b_par, st.b_pcc or e.b_pcc, st.n_obj or e.n_obj
        e.b_live, e.b_dead, e.sp_dropn = st.b_live or e.b_live, st.b_dead or e.b_dead, st.sp_dropn or e.sp_dropn
        e.n_pt, e.n_pl, e.n_ray = st.n_pt or e.n_pt, st.n_pl or e.n_pl, st.n_ray or e.n_ray
        if (st.bytes or 0) > cnt.bytes_max then cnt.bytes_max = st.bytes end
        for _, k in ipairs(DL_ADD2) do cnt[k] = cnt[k] + (st[k] or 0) end
        local nbt = M.dl_bytes()
        if nbt > cnt.btot_max then cnt.btot_max = nbt end
        -- PT2_V2_DOLL_TAKE_STORE = N (MB): a take job past N MB starts again
        local tcap = PCK.PT2_V2_DOLL_TAKE_STORE
        if src == "take" and type(tcap) == "number" and (st.bytes or 0) > tcap * 1048576 then why = "cap" end
        if st.changed == 1 then
          cnt.changed = cnt.changed + 1
          for _, k in ipairs(DL_ADD) do cnt[k] = cnt[k] + (st[k] or 0) end
          -- the Lua copies of the rays and square facts (a Lua run on
          -- the job reads them) are made again
          local c = e.cache
          c.rays, c.sqm, c.sqf = {}, {}, {}
        end
      end
    end
    if why then
      cnt[why] = (cnt[why] or 0) + 1
      if dle then gh_pt2_v2.dl_ev_reset(dsrc, DLE_CODE[why] or 9, now, inp._v2_pc_pn) end
      pc_free(e)
      e = nil
    end
  end
  if not e then
    e = { cache = new_cache(), t0 = now, doll = true, src = src }
    PDS[src] = e
    cnt.new = cnt.new + 1
    if dle and not why then gh_pt2_v2.dl_ev_reset(dsrc, 0, now, inp._v2_pc_pn) end
  end
  e.P, e.B, e.ss, e.last, e.open = P, B, ss, now, true
  cnt.runs = cnt.runs + 1
  inp.v2_pcache = true
  inp._v2_doll = true
  inp._v2_pc_entry = e
  inp._v2_pc_lost = (PCK.PT2_CPROF_LOG or PCK.PT2_V2_DOLL_CHECK) and true or nil
  return e.cache
end

-- PT2_V2_DOLL_MEM_LOG (log only): each N brain ticks a PT2_V2_DOLL_MEM
-- line per kept doll job (gh_pt2_v2.dl_mem), or one with no job
local DM_NEXT
local function dm_sample(inp, now)
  local K = PCK.PT2_V2_DOLL_MEM_LOG
  if not K or not gh_pt2_v2.dl_mem then return end
  if DM_NEXT == nil then gh_pt2_v2.dl_mem_on(true); DM_NEXT = now end
  if now < DM_NEXT then return end
  DM_NEXT = now + (type(K) == "number" and K or 500)
  local kb = collectgarbage("count")
  local any = false
  for _, src in ipairs({ "take", "dbl" }) do
    local e = PDS[src]
    local j = e and e.cache and e.cache.c_job
    if j then
      any = true
      M.cprof_err(string.format("PT2_V2_DOLL_MEM t=%d p=%s src=%s lua_kb=%.0f %s", now, tostring(inp._v2_pc_pn), src,
        kb, gh_pt2_v2.dl_mem(j, src == "take" and 0 or 1)))
    end
  end
  if not any then
    M.cprof_err(string.format("PT2_V2_DOLL_MEM t=%d p=%s src=none lua_kb=%.0f %s", now, tostring(inp._v2_pc_pn),
      kb, gh_pt2_v2.dl_mem(nil, 2)))
  end
end

-- The cache table for a group job on inp.A.
local function pc_cache(inp)
  inp.v2_pcache = nil
  inp._v2_pc_part = nil
  inp._v2_doll = nil
  local src = pc_src(inp)
  if not src then return new_cache() end
  local w, A = inp._world, inp.A
  local now = inp._v2_pc_now or 0
  if PCK.PT2_V2_DOLL_MEM_LOG then dm_sample(inp, now) end
  -- no engine map (a test's world): no store
  if PCK.PT2_V2_DOLL then
    if not gh_pt2_v2.doll_sync or gh_pt2_v2.pc_watch_new() == nil then return new_cache() end
    return doll_cache(inp, src, now)
  end
  local list = PCS[src]
  if not list then list = {}; PCS[src] = list end
  -- no engine map (a test's world): no store
  if not gh_pt2_v2.pc_watch_new then return new_cache() end
  local W0 = gh_pt2_v2.pc_watch_new()
  if W0 == nil then return new_cache() end
  local P, B, ss = pc_take(inp, w)
  -- PT2_V2_CACHE_CHECK_TMP (log only): a new entry that is not kept
  if inp._v2_pc_tmp then
    local e = { key = -1, tick = now, W = W0, cache = new_cache() }
    gh_pt2_v2.pc_snap(W0)
    e.P, e.B, e.ss, e.open = P, B, ss, true
    e.cache.pc_max_bytes = PCK.PT2_V2_CACHE_MAX_BYTES or 0
    inp.v2_pcache = true
    inp._v2_pc_entry = e
    return e.cache
  end
  local cnt = M.pc_count
  local maxage = PCK.PT2_V2_CACHE_MAX_AGE_TICKS or 3000
  local dlog = PCK.PT2_V2_CACHE_DROP_LOG
  local plog = PCK.PT2_V2_CACHE_PART_LOG and gh_pt2_v2.pc_part_q and true or false
  local hist = dlog and (PCH[src] or {})
  if dlog then PCH[src] = hist end
  -- the goal pill by its square: each run's pill tables are new tables
  local key = (A.mx or -1) * 256 + (A.my or -1)
  local aged
  -- entries past the age go (their goal pill may not run again)
  for i = #list, 1, -1 do
    local x = list[i]
    if now - x.tick > maxage then
      cnt.age = cnt.age + 1
      if dlog then
        hist[x.key] = { t = now, why = "age" }
        if x.key == key then aged = now - x.tick
        else pcl_line(inp, src, x.key, "evict", "age", now - x.tick, {}, 0) end
      end
      pc_free(x); table.remove(list, i)
    end
  end
  local e, at
  for i, x in ipairs(list) do if x.key == key then e, at = x, i; break end end
  local why, hf
  if e then
    if e.open then why = "open"
    elseif e.ss ~= ss then why = "rules"
    else
      local sp, sb = pc_wstr(P, B, e.W)
      local ht
      ht, hf = gh_pt2_v2.pc_wsig(e.W, false)
      if e.sp ~= sp then why = "pill"
      elseif e.sb ~= sb then why = "base"
      elseif e.ht ~= ht then why = "terrain"
      elseif e.hf ~= hf and inp.env.forest_transparent == false then why = "terrain" end
    end
    table.remove(list, at)
    if plog then
      local lw = why
      if why == "terrain" and e.ht == gh_pt2_v2.pc_wsig(e.W, false) then lw = "forest" end
      if not why and e.hf ~= hf then lw = "forest_plans" end
      if why == "open" or not e.part then
        M.cprof_err(string.format("PT2_V2_CACHE_PART t=%d p=%s src=%s goal=%d,%d res=%s why=%s age=%d nopart",
          now, tostring(inp._v2_pc_pn), src, math.floor(key / 256), key % 256, why and "drop" or "kept",
          lw or "-", now - e.tick))
      else
        pcp_line(inp, src, key, e, why and "drop" or "kept", lw, P, B, now)
      end
    end
    if why and dlog then
      -- a forest only change with PT2_FOREST_TRANSPARENT off is "forest"
      local lw = (why == "terrain" and e.ht == gh_pt2_v2.pc_wsig(e.W, false)) and "forest" or why
      local items, n = pcl_detail(e, lw, P, B, ss)
      local ex = (lw == "open") and string.format(" run_t=%s", tostring(e.run_t)) or nil
      pcl_line(inp, src, key, "drop", lw, now - e.tick, items, n, ex)
      hist[key] = { t = now, why = lw }
    end
    if why then cnt[why] = cnt[why] + 1; pc_free(e); e = nil end
  end
  if dlog and aged then pcl_line(inp, src, key, "drop", "age", aged, {}, 0) end
  if e then
    cnt.kept = cnt.kept + 1
    if plog then e.lk = "kept" end
    if e.hf ~= hf then
      cnt.forest = cnt.forest + 1
      e.cache.pc_drop_plans = true
    end
    if dlog then
      local items, n = {}, 0
      if e.hf ~= hf then items, n = pcl_squares(e.W) end
      pcl_line(inp, src, key, "kept", e.hf ~= hf and "forest_plans" or nil, now - e.tick, items, n)
    end
  else
    -- full (entries or bytes): this run has no store, and the kept
    -- entries stay (a player runs its targets in turn, so dropping the
    -- oldest for each new one would keep none of them)
    local tot = 0
    for _, x in ipairs(list) do tot = tot + (x.bytes or 0) end
    local maxb = PCK.PT2_V2_CACHE_MAX_TOTAL_BYTES or 0
    if #list >= (PCK.PT2_V2_CACHE_MAX_ENTRIES or 16) or (maxb > 0 and tot >= maxb) then
      cnt.full = cnt.full + 1
      if plog then
        -- the all-pills model has no full: does the goal pill's own last
        -- parts (any goal) exist?
        local XS = PXS[src]
        local sh = XS and XS[key]
        M.cprof_err(string.format("PT2_V2_CACHE_XFULL t=%d p=%s src=%s goal=%d,%d shadow=%s age=%s", now,
          tostring(inp._v2_pc_pn), src, math.floor(key / 256), key % 256, tostring(sh ~= nil),
          sh and tostring(now - sh.t) or "-"))
      end
      if dlog then
        local h = hist[key]
        pcl_line(inp, src, key, "full", (#list >= (PCK.PT2_V2_CACHE_MAX_ENTRIES or 16)) and "entries" or "bytes",
          nil, {}, 0, string.format(" entries=%d bytes=%d prev=%s", #list, tot,
            h and (h.why .. "@" .. h.t) or "first"))
        if not (h and h.t == now) then hist[key] = { t = now, why = "full" } end
      end
      return new_cache()
    end
    cnt.new = cnt.new + 1
    local lk = why and ("drop_" .. why) or (aged and "age") or "new"
    if dlog then
      local h = hist[key]
      pcl_line(inp, src, key, "new", h and ("after_" .. h.why) or "first", nil, {}, 0,
        h and string.format(" prev_t=%d", h.t) or nil)
    end
    e = { key = key, tick = now, W = W0, cache = new_cache() }
    if plog then
      local h = hist and hist[key]
      e.lk = (lk == "new" and h) and ("new_after_" .. h.why) or lk
    end
  end
  -- this run's start: the strings and the map that pc_finish hashes
  gh_pt2_v2.pc_snap(e.W)
  e.P, e.B, e.ss, e.open = P, B, ss, true
  e.why = why
  -- PT2_V2_CACHE_DROP_LOG: this start's lists, for the next lookup's diff
  if dlog or plog then e.Pf, e.Bf, e.run_t = P, B, now end
  if plog then
    PCP_RUN = PCP_RUN + 1
    inp._v2_pc_part = PCP_RUN
    e.src = src
  else
    inp._v2_pc_part = nil
  end
  list[#list + 1] = e
  e.cache.pc_max_bytes = PCK.PT2_V2_CACHE_MAX_BYTES or 0
  inp.v2_pcache = true
  inp._v2_pc_entry = e
  -- log only: count the walk memo answers that the store runs skip
  inp._v2_pc_lost = (PCK.PT2_CPROF_LOG or PCK.PT2_V2_CACHE_CHECK) and true or nil
  return e.cache
end

-- The pc_* counters of a run's stat (gh_pt2_v2.c push_result).
local PC_F = { "pc_eh", "pc_em", "pc_et", "pc_en", "pc_ph", "pc_pm", "pc_pt", "pc_pn", "pc_wdrop",
               "pc_lost_ts", "pc_lost_aw", "pc_lost_ts_us" }

-- PT2_V2_CACHE_PART_LOG (log only): the run's parts into the entry (for
-- the next lookup's PART line); each pill A's parts set against that pill
-- A's last parts from any goal (an XPART line), and kept as its new last.
local function pcp_finish(inp, e, j)
  local now = inp._v2_pc_now or 0
  local mk = gh_pt2_v2.pc_part_make
  local part, info, slots = mk(j, e.W)
  e.part = part
  if not part then return end
  local reach, margin = pcp_reach(inp)
  local inf = {}
  for k, v in pairs(info) do inf[#inf + 1] = k .. "=" .. string.format("%.0f", v) end
  table.sort(inf)
  local XS = PXS[e.src]
  if not XS then XS = {}; PXS[e.src] = XS end
  local maxage = PCK.PT2_V2_CACHE_MAX_AGE_TICKS or 3000
  local nsh = 0
  for k, x in pairs(XS) do if now - x.t > maxage then XS[k] = nil end end
  for _, sl in ipairs(slots) do
    local sh = XS[sl.k]
    if sh then
      nsh = nsh + 1
      local pills, car, nh, na = pcp_pills(sh.P, e.P)
      local bases = pcp_bases(sh.B, e.B)
      local st, tab = gh_pt2_v2.pc_part_q(sh.part, pills, bases, reach, margin, part, car)
      local ss = sh.slot or {}
      M.cprof_err(string.format("PT2_V2_CACHE_XPART t=%d p=%s src=%s X=%d,%d goal=%d,%d sgoal=%d,%d age=%d rules=%s"
          .. " fin=%s co=%s wsig=%s skh=%s st=%.1f ct=%.1f sh=%s ph=%d pa=%d car=%s %s",
        now, tostring(inp._v2_pc_pn), e.src, sl.k % 256, math.floor(sl.k / 256), math.floor(e.key / 256), e.key % 256,
        math.floor(sh.goal / 256), sh.goal % 256, now - sh.t, tostring(sh.ss == e.ss),
        tostring(ss.finish == sl.finish), tostring(ss.co == sl.co), tostring(ss.wsig == sl.wsig),
        tostring(ss.skh == sl.skh), ss.t or 0, sl.t or 0, tab and tab[sl.k] or "-", nh, na, tostring(car), st or "-"))
    end
    local p1, i1, s1 = mk(j, e.W, sl.k)
    if p1 then
      XS[sl.k] = { part = p1, P = e.P, B = e.B, ss = e.ss, goal = e.key, t = now, slot = s1 and s1[1],
                   bytes = i1 and i1.bytes or 0, n_ray = i1 and i1.n_ray or 0, n_filt = i1 and i1.n_filt or 0,
                   n_point = i1 and i1.n_point or 0 }
    end
  end
  local xn, xb, xr, xf, xp = 0, 0, 0, 0, 0
  for _, x in pairs(XS) do
    xn, xb = xn + 1, xb + (x.bytes or 0)
    xr, xf, xp = xr + (x.n_ray or 0), xf + (x.n_filt or 0), xp + (x.n_point or 0)
  end
  M.cprof_err(string.format("PT2_V2_CACHE_PINFO t=%d p=%s src=%s goal=%d,%d lk=%s slots=%d nshadow=%d"
      .. " xs_n=%d xs_bytes=%.0f xs_ray=%d xs_filt=%d xs_point=%d %s", now,
    tostring(inp._v2_pc_pn), e.src, math.floor(e.key / 256), e.key % 256, tostring(e.lk), #slots, nsh,
    xn, xb, xr, xf, xp, table.concat(inf, " ")))
end

-- The end of a group run with a store: the entry's watch area from the
-- C's marks, and its strings and hashes from the run's start.
-- PT2_V2_DOLL: the evaluates' doll stats (gh_pt2_v2.c push_result)
local DL_ST = { "dl_ray_old", "dl_ray_cast", "dl_emu_casts", "dl_sl_n", "dl_sl_clean", "dl_ring_n",
                "dl_ring_clean", "dl_wdrop", "pc_eh", "pc_em", "pc_et", "pc_ph", "pc_pm", "pc_pt" }
local function dl_finish(job)
  local e = job.pc_entry
  e.open = nil
  local d = M.dl_count
  d.ends = (d.ends or 0) + 1
  -- PT2_CPROF_LOG (log only): the doll's sums, a line each 100 ends
  if PCK.PT2_CPROF_LOG and d.ends % 100 == 0 then
    local t = {}
    for _, k in ipairs({ "runs", "new", "open", "time", "age", "rules", "lost", "busy", "nojob", "nomap", "sync",
                         "changed", "sync_us", "own", "nsq", "nmid", "slots", "n_ray", "n_pt", "n_pl", "n_flt",
                         "d_ray", "d_pt", "d_pl", "d_flt", "d_sqm", "d_sqf", "bytes_max", "btot_max",
                         "b_store", "b_rebuilt", "b_drop", "wn_us", "wn_tests", "wn_keep", "wn_fkeep", "wn_slots" }) do
      t[#t + 1] = string.format("%s=%.0f", k, d[k] or 0)
    end
    for _, k in ipairs(DL_ST) do t[#t + 1] = string.format("s_%s=%.0f", k, d["s_" .. k] or 0) end
    local nb, bs = M.dl_bytes()
    for _, src in ipairs({ "take", "dbl" }) do
      local x = PDS[src]
      if x then
        t[#t + 1] = string.format("%s_ar=%.0f %s_par=%.0f %s_pcc=%.0f %s_obj=%.0f %s_npt=%.0f %s_npl=%.0f %s_nray=%.0f"
          .. " %s_live=%.0f %s_dead=%.0f %s_dropn=%.0f",
          src, x.b_ar or 0, src, x.b_par or 0, src, x.b_pcc or 0, src, x.n_obj or 0, src, x.n_pt or 0,
          src, x.n_pl or 0, src, x.n_ray or 0, src, x.b_live or 0, src, x.b_dead or 0, src, x.sp_dropn or 0)
      end
    end
    M.cprof_err(string.format("PT2_V2_DOLL_SUM %s ends=%d bytes=%.0f take=%.0f dbl=%.0f %s",
      tostring(job.inp._prof_head or job.inp._v2_pc_head), d.ends, nb, bs.take or 0, bs.dbl or 0, table.concat(t, " ")))
  end
end

local function pc_finish(job)
  local e = job.pc_entry
  if e and e.doll then dl_finish(job); return end
  if not (e and e.open and e.P) then return end
  local inp = job.inp
  local j = e.cache and e.cache.c_job
  local cnt = M.pc_count
  if j then
    local xy = {}
    for _, x in ipairs(e.P) do xy[#xy + 1] = x[1]; xy[#xy + 1] = x[2] end
    local reach = ((inp.pill_range or 0) + (inp.range_margin or 0)) / 256
    local ns, nt, nr, out = gh_pt2_v2.pc_watch(j, e.W, reach, PCK.PT2_V2_CACHE_MARGIN or 3, xy)
    cnt.w_n = cnt.w_n + 1
    cnt.w_pills, cnt.w_sq, cnt.w_ring = cnt.w_pills + ns, cnt.w_sq + nt, cnt.w_ring + nr
    local o = 0
    for k, v in pairs(out) do cnt.w_out[k] = cnt.w_out[k] + v; o = o + v end
    if o > cnt.w_out_max then cnt.w_out_max = o end
    e.w_sq, e.w_out = nt, o
  end
  e.sp, e.sb = pc_wstr(e.P, e.B, e.W)
  e.ht, e.hf = gh_pt2_v2.pc_wsig(e.W, true)
  local plog = PCK.PT2_V2_CACHE_PART_LOG
  if j and plog and gh_pt2_v2.pc_part_make and e.src then pcp_finish(inp, e, j) end
  e.P, e.B, e.open = nil, nil, nil
  -- PT2_CPROF_LOG (log only): the store's sums, a line each 100 ends
  if PCK.PT2_CPROF_LOG and job.pcst and e.key ~= -1 then
    local T = M.pc_tot or {}
    M.pc_tot = T
    for _, k in ipairs(PC_F) do T[k] = (T[k] or 0) + (job.pcst[k] or 0) end
    T.n = (T.n or 0) + 1
    if T.n % 100 == 0 then
      local o = cnt.w_out
      M.cprof_err(string.format("PT2_V2_CACHE_SUM %s runs=%d eh=%d em=%d ph=%d pm=%d wd=%d lt=%d la=%d ltu=%.0f"
          .. " kept=%d new=%d age=%d pill=%d base=%d rules=%d terrain=%d forest=%d full=%d open=%d"
          .. " w_n=%d w_pills=%d w_sq=%d w_ring=%d out_tt=%d out_po=%d out_sqf=%d out_base=%d out_walk=%d out_max=%d",
        tostring(inp._prof_head or inp._v2_pc_head), T.n, T.pc_eh, T.pc_em, T.pc_ph, T.pc_pm, T.pc_wdrop,
        T.pc_lost_ts, T.pc_lost_aw, T.pc_lost_ts_us, cnt.kept, cnt.new, cnt.age, cnt.pill, cnt.base, cnt.rules,
        cnt.terrain, cnt.forest, cnt.full, cnt.open, cnt.w_n, cnt.w_pills, cnt.w_sq, cnt.w_ring,
        o.tt, o.po, o.sqf, o.base, o.walk, cnt.w_out_max))
    end
  end
end

-- The pc_* counters of a run's stat (gh_pt2_v2.c push_result) into job.pcst.
local function pc_add(job, r)
  local st = r and r.v2 and r.v2.stat
  if not (job.pcst and st) then return end
  for _, k in ipairs(PC_F) do job.pcst[k] = job.pcst[k] + (st[k] or 0) end
  job.pcst.bytes = st.pc_bytes or job.pcst.bytes
  local e = job.pc_entry
  if e and st.pc_bytes then e.bytes = st.pc_bytes end
  if e and e.doll then
    local d = M.dl_count
    for _, k in ipairs(DL_ST) do d["s_" .. k] = (d["s_" .. k] or 0) + (st[k] or 0) end
  end
end

-- PT2_V2_CACHE_CHECK (log only): the same group again with no store, in
-- the same think; a PT2_V2_CACHE_CHK line (stderr) says what differs.
local function pc_best_str(res)
  local b = res and res.best
  if not b then return "none" end
  local pl = {}
  for _, s in ipairs(b.plan or {}) do pl[#pl + 1] = tostring(s.kind) .. "@" .. tostring(s.mx) .. "," .. tostring(s.my) end
  local A = res.A
  return string.format("A=%s,%s px=%s py=%s safe=%s kills=%s chain=%s base=%s aim=%s plan=%s",
    tostring(A and A.mx), tostring(A and A.my), tostring(b.px), tostring(b.py), tostring(b.safe),
    tostring(b.kills), tostring(b.chain and #b.chain), tostring(b.base), tostring(b.aim_idx),
    table.concat(pl, "/"))
end

-- the keys of a source's entries, oldest first (the check line)
local function pc_list_str(src)
  local t = {}
  for _, x in ipairs(PCS[src] or {}) do t[#t + 1] = tostring(x.key) end
  return table.concat(t, "/")
end

local function pc_check(inp, job)
  local cnt = M.pc_count
  local doll = job.pc_entry and job.pc_entry.doll
  cnt.checks = cnt.checks + 1
  local o_off, o_sl = inp._v2_pc_off, inp.v2_slice
  inp._v2_pc_off, inp.v2_slice = true, nil
  -- PT2_V2_CACHE_CHECK_NOMEMO (log only): the fresh run skips the two walk
  -- memos as a run made for the store does (a test of those memos)
  inp._v2_pc_nomemo = PCK.PT2_V2_CACHE_CHECK_NOMEMO or nil
  local ok, j2 = pcall(function()
    local j = M.group_begin(inp)
    while not M.group_step(j) do end
    return j
  end)
  inp._v2_pc_off, inp.v2_slice = o_off, o_sl
  inp._v2_pc_nomemo = nil
  inp.v2_pcache = true
  if not ok then M.cprof_err("PT2_V2_CACHE_CHK error " .. tostring(j2)); return end
  local a, b = pc_best_str(job.res), pc_best_str(j2.res)
  local same = a == b
  -- PT2_V2_CACHE_CHECK_TWICE / _TMP (log only): the run with no cache once
  -- more, and a run with a new store that is not kept, against the first
  -- run with no cache
  local extra = ""
  local function again(tmp)
    local o_e, o_pc = inp._v2_pc_entry, inp.v2_pcache
    inp._v2_pc_off, inp.v2_slice, inp._v2_pc_tmp = (not tmp) or nil, nil, tmp or nil
    local ok2, j3 = pcall(function()
      local j = M.group_begin(inp)
      while not M.group_step(j) do end
      return j
    end)
    local te = tmp and inp._v2_pc_entry
    inp._v2_pc_off, inp.v2_slice, inp._v2_pc_tmp = o_off, o_sl, nil
    inp._v2_pc_entry, inp.v2_pcache = o_e, o_pc
    if te and te ~= o_e then pc_free(te) end
    if not ok2 then return "error" end
    local c = pc_best_str(j3.res)
    return c == b and "same" or ("DIFF{" .. c .. "}")
  end
  if PCK.PT2_V2_CACHE_CHECK_TWICE then extra = extra .. " twice=" .. again(false) end
  if PCK.PT2_V2_CACHE_CHECK_TMP then extra = extra .. " tmp=" .. again(true) end
  if not same then cnt.diffs = cnt.diffs + 1 end
  local e, s = inp._v2_pc_entry, job.pcst or {}
  -- PT2_V2_DOLL_CHECK: on a DIFF, the doll run again with some store
  -- finds skipped (1 points and plans, 2 rays, 4 filter, 8 squares, 15
  -- all, 16 points, 32 plans): which one gives the new job's answer
  local skip = ""
  if doll and not same then
    for _, k in ipairs({ 16, 32, 1, 2, 4, 8, 15 }) do
      inp._v2_dl_skip = k
      local ok3, j4 = pcall(function()
        local j = M.group_begin(inp)
        while not M.group_step(j) do end
        return j
      end)
      inp._v2_dl_skip = nil
      local c = ok3 and pc_best_str(j4.res) or "error"
      skip = skip .. string.format(" skip%d=%s", k, c == b and "same" or "DIFF")
    end
  end
  if doll then
    local d = M.dl_count
    M.cprof_err(string.format("PT2_V2_DOLL_CHK %s %s src=%s rule=%s eh=%d em=%d et=%d ph=%d pm=%d pt=%d wd=%d"
        .. " checks=%d diffs=%d runs=%d new=%d open=%d age=%d rules=%d lost=%d time=%d bytes=%s cached={%s}%s",
      tostring(inp._prof_head or inp._v2_pc_head), same and "same" or "DIFF", tostring(inp._v2_pc_src or "take"),
      tostring(PCK.PT2_V2_DOLL_RULE), s.pc_eh or 0, s.pc_em or 0, s.pc_et or 0, s.pc_ph or 0, s.pc_pm or 0,
      s.pc_pt or 0, s.pc_wdrop or 0, cnt.checks, cnt.diffs, d.runs, d.new, d.open, d.age, d.rules, d.lost,
      d.time, tostring(e and e.bytes), a, same and "" or (" fresh={" .. b .. "}" .. skip)))
    return
  end
  M.cprof_err(string.format("PT2_V2_CACHE_CHK %s %s src=%s why=%s eh=%d em=%d et=%d en=%d ph=%d pm=%d pt=%d pn=%d wd=%d"
      .. " bytes=%d checks=%d diffs=%d kept=%d new=%d age=%d pill=%d base=%d rules=%d terrain=%d forest=%d full=%d"
      .. " open=%d wsq=%s wout=%s lt=%d la=%d ltu=%.0f"
      .. " key=%s list=%s%s cached={%s}%s",
    tostring(inp._prof_head or inp._v2_pc_head), same and "same" or "DIFF", tostring(inp._v2_pc_src or "take"),
    tostring(e and e.why), s.pc_eh or 0, s.pc_em or 0, s.pc_et or 0, s.pc_en or 0,
    s.pc_ph or 0, s.pc_pm or 0, s.pc_pt or 0, s.pc_pn or 0, s.pc_wdrop or 0, s.bytes or 0,
    cnt.checks, cnt.diffs, cnt.kept, cnt.new, cnt.age, cnt.pill, cnt.base, cnt.rules, cnt.terrain,
    cnt.forest, cnt.full, cnt.open, tostring(e and e.w_sq), tostring(e and e.w_out),
    s.pc_lost_ts or 0, s.pc_lost_aw or 0, s.pc_lost_ts_us or 0, tostring(e and e.key), pc_list_str(inp._v2_pc_src or "take"), extra, a,
    same and "" or (" fresh={" .. b .. "}")))
end

-- The old world_inp; PT2_V2_CACHE also keeps the world in inp (the area
-- inputs of the store above).
function M.world_inp(world, C, U, PC, cpf, inp, no_builder)
  inp = OLD.world_inp(world, C, U, PC, cpf, inp, no_builder)
  if C.PT2_V2_CACHE or C.PT2_V2_DOLL then inp._world = world end
  return inp
end

-- ── evaluate_group (as the old one; rays and square reads kept per job) ──
function M.evaluate_group(inp)
  local job = M.group_begin(inp)
  while not M.group_step(job) do end
  if PCK.PT2_V2_CACHE_CHECK and inp.v2_pcache and not (job.pc_entry and job.pc_entry.doll) then
    pc_check(inp, job)
  end
  return job.res
end

function M.group_begin(inp)
  if inp.wall_memo then M.wall_memo_check(inp, inp.A) end
  local job = { inp = inp, goalA = inp.A, list = nil, i = 1, res = nil, cache = pc_cache(inp) }
  if inp.v2_pcache then
    job.pcst = { pc_eh = 0, pc_em = 0, pc_et = 0, pc_en = 0, pc_ph = 0, pc_pm = 0, pc_pt = 0, pc_pn = 0, pc_wdrop = 0,
                 pc_lost_ts = 0, pc_lost_aw = 0, pc_lost_ts_us = 0 }
    job.pc_entry = inp._v2_pc_entry
  end
  return job
end

function M.group_parts_left(job)
  if not job.list then return 1 end
  return #job.list - job.i + 1
end

local function eval_in_job(job, inp)
  local old, oldx = inp._v2_cache, inp._xcover_later
  inp._v2_cache, inp._xcover_later = job.cache, true
  local ok, r = pcall(M.evaluate, inp)
  inp._v2_cache, inp._xcover_later = old, oldx
  if not ok then error(r, 0) end
  if job.pcst then pc_add(job, r) end
  return r
end

-- E5-13: the extra walls of the group's final result, with the result's
-- A (as its own run had it) and the job's cache.
local job_done_x
local function job_done(job)
  job_done_x(job)
  if job.pc_entry then
    pc_finish(job)
    -- PT2_V2_DOLL_CHECK (log only): the same group with a new job (also
    -- for a run in slices)
    if PCK.PT2_V2_DOLL_CHECK and job.pc_entry.doll and not job.inp._v2_dl_skip then pc_check(job.inp, job) end
  end
end
job_done_x = function(job)
  local inp, res = job.inp, job.res
  res.n_eval, res.n_reject = job.n_eval, job.n_reject
  local b = res.best
  if not (b and b.safe and (inp.extra_cover_walls or 0) > 0) then return end
  local A, goalA = res.A or job.goalA, job.goalA
  local clk = inp.v2_clock
  local t3 = clk and clk()
  local old = inp._v2_cache
  inp._v2_cache = job.cache
  if A ~= goalA then
    inp.A = A
    if inp.set_A then inp.set_A(A) end
  end
  local ok, r = pcall(function() return extra_cover(new_ctx(inp, A), b) end)
  inp._v2_cache = old
  if A ~= goalA then
    inp.A = goalA
    if inp.set_A then inp.set_A(goalA) end
  end
  if not ok then error(r, 0) end
  b.extra_cover = r
  local tm = res.v2 and res.v2.tm
  if t3 and tm then
    local d = clk() - t3
    tm.xcover = (tm.xcover or 0) + d
    if tm.total then tm.total = tm.total + d end
  end
end

-- The group step's pick: does run p's best b take over from the group's
-- best? A safe chain plan that holds the goal pill, ranked by M.better
-- with no S0 (Nov 23, Andrew approved Nov 23): on a full tie the plan
-- the group has (the goal pill's own, run first) stays.
--
-- floor (PT2_QUICK_SIDE_FIRST): the job's lowest score per safe and
-- kills over the runs so far (M.better).
function M.group_takes(inp, goalA, b, best, floor)
  return (b and b.safe and b.chain_mode and b.in_chain and b.in_chain[goalA]
          and (not best or M.better(b, best, inp.leeway_min, inp.travel_band, inp.prefer_existing_cover,
                                    inp.start_while_building, true, M.far_on(inp), inp,
                                    floor and floor[M.qsf_key(b)]))) and true or false
end

-- PT2_QUICK_SIDE_FIRST: a run's lowest scores into the job's
local function job_floor(job, r)
  local f = r and r.qsf_floor
  if not f then return end
  job.qfloor = job.qfloor or {}
  for k, v in pairs(f) do
    if not job.qfloor[k] or v < job.qfloor[k] then job.qfloor[k] = v end
  end
end

function M.group_step(job)
  local inp, goalA = job.inp, job.goalA
  if not job.list then
    local res = eval_in_job(job, inp)
    res.A, res.goal_A = goalA, goalA
    job_floor(job, res)
    job.res, job.n_eval, job.n_reject = res, res.n_eval, res.n_reject
    job.list = {}
    if not inp.chains then job_done(job); return true end
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
    -- S10: only a safe chain of this run counts (below)
    local co0 = inp.chains_only
    if inp.chains_only_runs then inp.chains_only = true end
    local ok, r = pcall(eval_in_job, job, inp)
    inp.chains_only = co0
    inp.A = goalA
    if inp.set_A then inp.set_A(goalA) end
    if not ok then error(r, 0) end
    job.n_eval, job.n_reject = job.n_eval + r.n_eval, job.n_reject + r.n_reject
    local b, res = r.best, job.res
    job_floor(job, r)
    if M.group_takes(inp, goalA, b, res.best, job.qfloor) then
      r.A, r.goal_A = p, goalA
      job.res = r
    end
  end
  if job.i > #job.list then
    job_done(job)
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
