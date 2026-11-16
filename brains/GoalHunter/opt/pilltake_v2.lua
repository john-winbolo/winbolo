-- =========================================================================
-- pilltake_v2.lua -- the pill take v2 evaluator (2026-11-16)
--
-- Design: PILLTAKE_V2_PLAN.md (read its "Decisions (Nov 16)" first).
--
-- For a target pill A, v2 tries tank points P on a ring around A. At each P:
--   T(P)   = every live pill that is not ours and can target P
--            (centre within pill range 2048 wu + a margin).
--   rays   = for each X in T(P), the engine-exact shell walk X -> P
--            (shadow.cast): it ends on the tank or dies first ("reach"),
--            meets hostile pills (links) and our blockers (cover).
--   chain  = A, then the hostile pill A's ray meets first, then the one that
--            pill's ray meets, and so on. Those pills shoot each other.
--   bar    = shots of cover a ray needs behind its links:
--              single take (no link on A's ray): A needs
--                PPT_COVER_TARGET_SHOTS (15), as v1;
--              chain: every chain pill needs PT2_CHAIN_BLOCKER_MIN_SHOTS (5);
--              any other pill in range (a bystander) needs 5 too.
--   P is SAFE when every X in T(P) is out of reach or has its bar, and the
--   tank has one clear aim at A (5 aims, attack_shield's offsets).
-- When a ray lacks cover, v2 plans blockers on it: a dropped carried pill
-- first, then walls, on squares the ray crosses before the tank, nearest
-- the pill first, up to PT2_BUILD_MAX. A wall never goes on our own shot
-- lane, nor in front of a chain link (it would break the chain).
--
-- Rank (highest first): safe, kills (chain length), blocker rank (an
-- allied pill in the cover above walls only), leeway (neighbour points
-- that also pass), then score (lower is better: the caller's travel score
-- plus a cost per wall to build).
--
-- evaluate() is a pure function: every map and cost read goes through the
-- functions in `inp`, so the unit tests drive it with a hand-made world.
-- =========================================================================

local SH = require("shadow")

local M = {}

local sqrt, floor, sin, cos, rad, abs = math.sqrt, math.floor, math.sin, math.cos, math.rad, math.abs

-- Aim offsets inside the target square (wu): attack_shield's AIM_OFFSETS.
local AIM_INSET = 24
M.AIMS = {
  { 128, 128 },
  { AIM_INSET, AIM_INSET },
  { 255 - AIM_INSET, AIM_INSET },
  { AIM_INSET, 255 - AIM_INSET },
  { 255 - AIM_INSET, 255 - AIM_INSET },
}
M.AIM_NAMES = { "center", "TL", "TR", "BL", "BR" }

-- The 8 leeway neighbours: unit directions, scaled by the ring radius.
local NB = {}
for k = 0, 7 do
  local a = k * math.pi / 4
  NB[k + 1] = { cos(a), sin(a) }
end

-- A pill's name for logs: its world.pills id when the caller gave one.
function M.pill_name(inp, p)
  local id = inp and inp.ids and inp.ids[p]
  if id then return tostring(id) end
  return string.format("(%d,%d)", p.mx or -1, p.my or -1)
end

local function pill_centre(p)
  return p.mx * 256 + 128, p.my * 256 + 128
end

local function alive(p)
  return p and (p.health or 0) > 0 and not p.in_tank and not p.pos_unknown and p.mx ~= nil
end

-- ── One tank point ────────────────────────────────────────────────────────
-- Evaluate point (px, py) (world units) for target A with planned blockers
-- `extra` ({[sq] = shots}) and, when aim_fixed is set, only that aim.
-- Returns a record (see fields at the end) or nil, why.
local function eval_point(ctx, px, py, extra, aim_fixed)
  local inp, A, env = ctx.inp, ctx.A, ctx.env
  local pr = inp.pill_range + inp.range_margin
  local pr2 = pr * pr
  -- T(P)
  local T = {}
  local hasA = false
  for _, p in ipairs(ctx.hostile) do
    local cx, cy = pill_centre(p)
    local dx, dy = cx - px, cy - py
    if dx * dx + dy * dy <= pr2 then
      T[#T + 1] = p
      if p == A then hasA = true end
    end
  end
  if not hasA then return nil, "A_OUT_OF_RANGE" end
  -- the tank's square, for the base rule of our own shot
  env.omx, env.omy = floor(px / 256), floor(py / 256)
  -- OUTGOING: the first clear aim (or the fixed one)
  local amx, amy = A.mx, A.my
  local aim_idx, lane = nil, nil
  local a1, a2 = 1, #M.AIMS
  if aim_fixed then a1, a2 = aim_fixed, aim_fixed end
  local why_out = "short"
  -- every clear aim: the blocker search tries each one, since a wall on
  -- one aim's lane can be fine for another (v1 scores all five)
  local clear = {}
  for ai = a1, a2 do
    local off = M.AIMS[ai]
    local ok, why, ln = SH.tank_shot(env, px, py, amx * 256 + off[1], amy * 256 + off[2],
                                     amx, amy, extra)
    if ok then
      clear[#clear + 1] = ai
      if not aim_idx then aim_idx, lane = ai, ln end
    else
      why_out = why
    end
  end
  if not aim_idx then return nil, "OUTGOING:" .. tostring(why_out) end
  -- rays
  local rays = {}
  for i, X in ipairs(T) do
    local sx, sy = pill_centre(X)
    rays[i] = SH.cast(env, sx, sy, px, py, SH.PILL_FIRE_LEN, extra)
    rays[i].X = X
  end
  -- chain: follow links from A
  local chain = { A }
  local in_chain = { [A] = 1 }
  local chain_mode = false
  if inp.chains then
    local cur = A
    for _ = 1, 8 do
      local r = nil
      for i, X in ipairs(T) do if X == cur then r = rays[i] end end
      local nxt
      if r then
        nxt = r.link
      else
        -- a chain pill out of range of P: its shells do not come at us,
        -- but they still go where its last target was. Not followed.
        nxt = nil
      end
      if not nxt or in_chain[nxt] then break end
      chain[#chain + 1] = nxt
      in_chain[nxt] = #chain
      chain_mode = true
      cur = nxt
    end
  end
  -- cover per ray
  local covered_all = true
  local uncovered = {}
  local need_A, have_A = 0, 0
  local ally_cover = false
  for i, X in ipairs(T) do
    local r = rays[i]
    local bar
    if X == A and not chain_mode then bar = inp.bar_single else bar = inp.bar_chain end
    r.bar = bar
    local ok = (r.hit == "reach") or (r.cover >= bar)
    r.ok = ok
    if X == A then need_A, have_A = bar, (r.hit == "reach") and bar or r.cover end
    if not ok then
      covered_all = false
      uncovered[#uncovered + 1] = i
    end
    if ok and r.hit ~= "reach" then
      for k = 1, r.n_block do
        if r.block[k].kind == "ally_pill" then ally_cover = true end
      end
    end
  end
  local max_ok = #chain <= inp.max_chain
  return {
    px = px, py = py, T = T, rays = rays, chain = chain, in_chain = in_chain,
    chain_mode = chain_mode, aim_idx = aim_idx, lane = lane, clear_aims = clear,
    covered = covered_all, uncovered = uncovered,
    safe = covered_all and max_ok, max_ok = max_ok,
    kills = (chain_mode and max_ok) and #chain or 1,
    ally_cover = ally_cover, need = need_A, have = have_A,
  }
end
M._eval_point = eval_point

-- ── Blocker search ────────────────────────────────────────────────────────
-- Squares on ray r where a planned blocker may go, nearest the pill first.
local function slot_ok(ctx, rec, r, k, lane_set)
  local inp = ctx.inp
  local sq = r.sq[k]
  local mx, my = sq % 256, floor(sq / 256)
  local X = r.X
  if lane_set[sq] then return false, "lane" end
  if rec.extra_tmp and rec.extra_tmp[sq] then return false, "taken" end
  -- not in front of this ray's link (it would block the chain)
  if r.link and r.link_t and r.sq_t[k] <= r.link_t then return false, "before_link" end
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
  if ctx.env.pill_on(mx, my) then return false, "pill" end
  local tt = ctx.env.ttype(mx, my)
  if tt == ctx.env.T_BUILDING or tt == ctx.env.T_HALFBUILD then return false, "wall" end
  if not inp.buildable(mx, my) then return false, "not_buildable" end
  local key = sq
  local rc = ctx.reach_cache[key]
  if rc == nil then
    rc = inp.lgm_reach(rec.omx, rec.omy, mx, my) and true or false
    ctx.reach_cache[key] = rc
  end
  if not rc then return false, "lgm_unreachable" end
  return true
end

-- Plan blockers for the point in rec until every ray has its bar, or the
-- build budget runs out. Returns the final record and the plan list.
local function search_one(ctx, rec)
  local inp = ctx.inp
  local extra, plan = {}, {}
  local pills_left = inp.num_pill_blockers
  local tried = {}
  local cur = rec
  local A_link0 = rec.chain[2]
  for _ = 1, inp.build_max do
    if cur.covered then break end
    local lane_set = {}
    for _, s in ipairs(cur.lane) do lane_set[s] = true end
    cur.extra_tmp = extra
    local picked = nil
    for _, ui in ipairs(cur.uncovered) do
      local r = cur.rays[ui]
      for k = 1, r.n_sq do
        local sq = r.sq[k]
        if not tried[sq] then
          local ok = slot_ok(ctx, cur, r, k, lane_set)
          if ok then picked = sq; break end
        end
      end
      if picked then break end
    end
    cur.extra_tmp = nil
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
    local nrec = eval_point(ctx, rec.px, rec.py, extra, rec.aim_idx)
    -- the plan must keep our aim and the chain as they were
    if not nrec or nrec.chain[2] ~= A_link0 then
      extra[picked] = nil
      if kind == "pill" then pills_left = pills_left + 1 end
    else
      plan[#plan + 1] = { sq = picked, mx = picked % 256, my = floor(picked / 256),
                          shots = shots, kind = kind }
      nrec.omx, nrec.omy = rec.omx, rec.omy
      cur = nrec
    end
  end
  cur.plan, cur.extra = plan, extra
  return cur
end

-- Try the search on each clear aim and keep the best plan: covered first,
-- then fewer squares to build, then more cover on A's ray.
local function search_blockers(ctx, rec)
  local best = nil
  for _, ai in ipairs(rec.clear_aims) do
    local base = rec
    if ai ~= rec.aim_idx then
      base = eval_point(ctx, rec.px, rec.py, nil, ai)
      if base then base.omx, base.omy = rec.omx, rec.omy end
    end
    if base then
      local r = search_one(ctx, base)
      if not best
         or (r.covered and not best.covered)
         or (r.covered == best.covered and (#r.plan < #best.plan
             or (#r.plan == #best.plan and r.have > best.have))) then
        best = r
      end
      if best.covered and #best.plan == 0 then break end
    end
  end
  return best or search_one(ctx, rec)
end

-- Count the 8 neighbours at ring_wu that also pass with the same plan.
local function leeway(ctx, rec, ring_wu)
  local n, res = 0, {}
  for k = 1, 8 do
    local qx = floor(rec.px + NB[k][1] * ring_wu + 0.5)
    local qy = floor(rec.py + NB[k][2] * ring_wu + 0.5)
    local q, why = eval_point(ctx, qx, qy, rec.extra, nil)
    local ok = q and q.covered and q.max_ok and (#q.chain == #rec.chain)
    if ok then n = n + 1 end
    if not ok and q then
      for _, ui in ipairs(q.uncovered) do why = "OVERLAP:" .. M.pill_name(ctx.inp, q.rays[ui].X); break end
      if not why and #q.chain ~= #rec.chain then why = "CHAIN" end
    end
    res[k] = { x = qx, y = qy, ok = ok and true or false, why = (not ok) and (why or "?") or nil }
  end
  return n, res
end

-- Rank key comparison: true when a ranks above b.
-- Leeway counts up to cap (PT2_LEEWAY_MIN): past "enough room", the
-- travel score decides, as in v1. Without the cap a spot one neighbour
-- roomier but far off v1's line would always win.
function M.better(a, b, cap)
  if a.safe ~= b.safe then return a.safe end
  if a.kills ~= b.kills then return a.kills > b.kills end
  if a.blk ~= b.blk then return a.blk > b.blk end
  cap = cap or 8
  local la, lb = math.min(a.leeway or -1, cap), math.min(b.leeway or -1, cap)
  if la ~= lb then return la > lb end
  return a.score < b.score
end

-- Leeway of a given point and plan (for logs and the parity test).
function M.leeway_at(inp, px, py, extra)
  local ctx = { inp = inp, A = inp.A, env = inp.env, reach_cache = {}, hostile = {} }
  for _, p in ipairs(inp.pills) do
    if alive(p) and not inp.env.is_ally(p) then ctx.hostile[#ctx.hostile + 1] = p end
  end
  local rec = eval_point(ctx, px, py, extra, nil)
  if not rec then return 0 end
  rec.extra = extra
  return (leeway(ctx, rec, inp.leeway_ring_wu))
end

-- ── evaluate ──────────────────────────────────────────────────────────────
-- inp fields (all required unless marked):
--   A                     target pill {mx, my, health, owner, id}
--   pills                 list of known pills (same fields; in_tank, pos_unknown)
--   env                   shadow env (ttype, pill_on, is_ally, cover_of_pill,
--                         base_stops, T_* ids, wall_shots, ...)
--   radius                ring radius in squares
--   radii_offsets         list, e.g. {0, -0.4}: inner rings tried only when
--                         the ones before found nothing safe
--   angle_step            degrees
--   banned(deg)           -> true when the angle bucket is banned (optional)
--   standable(mx, my)     -> true when the tank may stand there
--   score_fn(deg, mx, my, cx, cy) -> base score, lower is better
--   in_map, stay_in, buildable(mx, my), lgm_reach(omx, omy, mx, my)
--   approach_offset       squares from the standoff out to the LGM origin
--   num_pill_blockers, pill_drop_shots, wall_shots, build_max, build_cost
--   bar_single, bar_chain, chains, max_chain, leeway_on, leeway_ring_wu,
--   leeway_top, own_pill_rank, search_top, pill_range, range_margin,
--   blocker_min_dist, orth_only
--   keep_all              (optional) keep every candidate for the overlay
function M.evaluate(inp)
  local A = inp.A
  local env = inp.env
  local ctx = { inp = inp, A = A, env = env, reach_cache = {}, hostile = {} }
  for _, p in ipairs(inp.pills) do
    if alive(p) and not env.is_ally(p) then ctx.hostile[#ctx.hostile + 1] = p end
  end
  local pcx, pcy = A.mx + 0.5, A.my + 0.5
  local all = {}
  local best = nil
  local n_eval, n_reject = 0, 0
  for ri, roff in ipairs(inp.radii_offsets) do
    local R = inp.radius + roff
    local cands = {}
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
          local rec, why = eval_point(ctx, px, py, nil, nil)
          if rec then
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
    end
    -- pre-rank without builds: chains first, then the base score
    table.sort(cands, function(a, b)
      local ka = (a.chain_mode and a.max_ok) and #a.chain or 1
      local kb = (b.chain_mode and b.max_ok) and #b.chain or 1
      if ka ~= kb then return ka > kb end
      if a.covered ~= b.covered then return a.covered end
      return a.base < b.base
    end)
    local scored = {}
    for i, rec in ipairs(cands) do
      local fin = rec
      if i <= inp.search_top and not rec.covered then
        fin = search_blockers(ctx, rec)
        fin.deg, fin.cx, fin.cy, fin.mx, fin.my, fin.radius = rec.deg, rec.cx, rec.cy, rec.mx, rec.my, rec.radius
        fin.base = rec.base
      else
        fin.plan, fin.extra = {}, {}
      end
      if i > inp.search_top and not rec.covered then fin.unsearched = true end
      fin.n_built = #fin.plan
      fin.blk = (inp.own_pill_rank and fin.ally_cover) and 2 or 1
      fin.score = fin.base + fin.n_built * inp.build_cost
      scored[#scored + 1] = fin
    end
    local cap = inp.leeway_min
    table.sort(scored, function(a, b) return M.better(a, b, cap) end)
    -- leeway on the top candidates, then rank them again
    if inp.leeway_on then
      for i = 1, math.min(inp.leeway_top, #scored) do
        local rec = scored[i]
        rec.leeway, rec.leeway_pts = leeway(ctx, rec, inp.leeway_ring_wu)
        -- A chain needs room to park: the plan's S6 minimum. A single take
        -- keeps v1's rule (no minimum), so it goes wherever v1 goes.
        if rec.safe and rec.kills >= 2 and rec.leeway < inp.leeway_min then
          rec.safe, rec.why_unsafe = false, "LEEWAY"
        end
      end
      table.sort(scored, function(a, b) return M.better(a, b, cap) end)
    end
    for _, rec in ipairs(scored) do all[#all + 1] = rec end
    if scored[1] and (not best or M.better(scored[1], best, cap)) then best = scored[1] end
    if best and best.safe then break end
  end
  return { best = best, all = inp.keep_all and all or nil, n_eval = n_eval, n_reject = n_reject,
           hostile = ctx.hostile, inp = inp }
end

-- Run v2's tests on a given point and plan (v1's choice) and return the
-- first failing test: "-" when it passes them all. extra: {[sq] = shots}.
function M.judge(inp, px, py, extra, aim_idx)
  local ctx = { inp = inp, A = inp.A, env = inp.env, reach_cache = {}, hostile = {} }
  for _, p in ipairs(inp.pills) do
    if alive(p) and not inp.env.is_ally(p) then ctx.hostile[#ctx.hostile + 1] = p end
  end
  local rec, why = eval_point(ctx, px, py, extra, aim_idx)
  if not rec then return why, nil end
  if not rec.max_ok then return "CHAIN_TOO_LONG", rec end
  for _, ui in ipairs(rec.uncovered) do
    local r = rec.rays[ui]
    if r.X == inp.A then
      return string.format("DEPTH need=%d have=%d", r.bar, r.cover), rec
    end
    return "OVERLAP:" .. M.pill_name(inp, r.X), rec
  end
  return "-", rec
end

-- Build the result in the shape shield.scan returns, so build_walls, charge
-- and engage read it unchanged. world_blockers(rec) gives the existing
-- blocker squares the plan relies on.
function M.to_scan_result(res, pill, standoff, adj_friendly_fn)
  local b = res and res.best
  if not b then return nil end
  local act, pot, seen = {}, {}, {}
  for i, r in ipairs(b.rays) do
    if r.hit == "tank" then
      for k = 1, r.n_block do
        local bl = r.block[k]
        if bl.kind ~= "planned" and not seen[bl.sq] then
          seen[bl.sq] = true
          act[#act + 1] = { mx = bl.sq % 256, my = floor(bl.sq / 256), kind = bl.kind }
        end
      end
    end
  end
  for _, s in ipairs(b.plan) do
    pot[#pot + 1] = { mx = s.mx, my = s.my, adj_friendly = adj_friendly_fn and adj_friendly_fn(s.mx, s.my) or nil }
  end
  local chain_ids = {}
  for i, p in ipairs(b.chain) do chain_ids[i] = M.pill_name(res.inp, p) end
  local best = {
    kind = "candidate", cx = b.cx, cy = b.cy, mx = b.mx, my = b.my, deg = b.deg,
    -- positive: plan_position demotes a result whose score is 0 or less
    score = 1000 + b.kills * 100 + b.blk * 10 + (b.leeway or 0),
    best_aim_idx = b.aim_idx,
    score_actual = b.have, score_potential = #pot, score_neighbor = b.leeway or 0,
    valid_tile = true, aims = {}, return_fire = { tiles = nil },
  }
  best.aims[b.aim_idx] = {
    blocked = false, blockers = act, potential_blockers = pot,
    unreachable_blockers = {}, nudge_wu = 0, score = b.have,
  }
  return {
    best = best, pill = pill, standoff = standoff, pill_hp = pill.health,
    fav_bonus_by_size = {}, min_chain = 0, tier_label = "PT2",
    pt2 = true,
    chain = { pills = chain_ids, kills = b.kills, mode = b.chain_mode and "HOLD" or "SINGLE",
              need = b.need, have = b.have, safe = b.safe },
  }
end

-- ── Adapters from the bot's world ─────────────────────────────────────────
-- The shadow env for this bot's view of the map. Only what the bot can see:
-- its own terrain memory (U.ttype) and its own pill list.
function M.world_env(world, C, U, PC)
  local env = {
    ttype = U.ttype,
    pill_on = function(mx, my)
      local p = PC.pill_on_square(world, mx, my)
      if p and (p.health or 0) > 0 then return p end
      return nil
    end,
    is_ally = function(p) return PC.is_friendly_owner(p.owner) end,
    cover_of_pill = function(p)
      return PC.pill_walls(p) * (C.SHIELD_WALL_COVER_SHOTS or 5)
    end,
    T_BUILDING = C.T_BUILDING, T_HALFBUILD = C.T_HALFBUILD,
    T_BOAT = C.T_BOAT, T_FOREST = C.T_FOREST,
    wall_shots = C.SHIELD_WALL_COVER_SHOTS or 5,
    half_wall_shots = C.SHIELD_WALL_COVER_SHOTS or 5,
    -- a boat stops one shell and is gone (proposal, awaiting Andrew's value)
    boat_shots = 1,
    forest_transparent = C.PT2_FOREST_TRANSPARENT ~= false,
  }
  env.base_stops = function(mx, my)
    return U.base_tile_stops_tank_shell(world, mx, my, env.omx or mx, env.omy or my)
  end
  return env
end

-- The knob part of inp, read from constants each call (cfg can change).
function M.knob_inp(C, inp)
  inp = inp or {}
  inp.angle_step       = C.PT2_ANGLE_STEP_DEG or 2
  inp.radii_offsets    = { 0, -0.4 }      -- proposal, awaiting Andrew's value
  inp.approach_offset  = C.ATTACK_APPROACH_OFFSET or 2.25
  inp.pill_drop_shots  = 15               -- a dropped pill: full health 15
  inp.wall_shots       = C.SHIELD_WALL_COVER_SHOTS or 5
  inp.build_max        = C.PT2_BUILD_MAX or 3
  inp.build_cost       = C.PT2_BUILD_WALL_COST or 10
  inp.bar_single       = C.PPT_COVER_TARGET_SHOTS or 15
  inp.bar_chain        = C.PT2_CHAIN_BLOCKER_MIN_SHOTS or 5
  inp.chains           = C.PT2_CHAINS ~= false
  inp.max_chain        = C.PT2_MAX_CHAIN or 3
  inp.leeway_on        = C.PT2_LEEWAY ~= false
  inp.leeway_ring_wu   = 32               -- proposal, awaiting Andrew's value
  inp.leeway_top       = C.PT2_LEEWAY_TOP or 8
  inp.leeway_min       = C.PT2_LEEWAY_MIN or 6
  inp.own_pill_rank    = C.PT2_OWN_PILL_BONUS ~= false
  inp.search_top       = 24               -- proposal, awaiting Andrew's value
  inp.pill_range       = SH.PILL_RANGE
  inp.range_margin     = C.PT2_RANGE_MARGIN_WU or 128
  inp.blocker_min_dist = 1.0              -- attack_shield.BLOCKER_MIN_DIST
  inp.orth_only        = C.BLOCKER_ORTHOGONAL_ONLY and true or false
  return inp
end

-- The map part of inp. no_builder: the LGM is dead. cpf: the pathfinder
-- module (lgm_travel_ticks_map), as attack_shield's score_aim uses it.
function M.world_inp(world, C, U, PC, cpf, inp, no_builder)
  inp = inp or {}
  inp.env = M.world_env(world, C, U, PC)
  local pills, ids = {}, {}
  for id, p in pairs(world.pills or {}) do
    pills[#pills + 1] = p; ids[p] = id
  end
  table.sort(pills, function(a, b) return (ids[a] or 0) < (ids[b] or 0) end)
  inp.pills, inp.ids = pills, ids
  inp.in_map, inp.stay_in = U.in_map, U.stay_in
  local cost_land = C.TERRAIN_COST_LAND or {}
  inp.standable = function(mx, my)
    local tt = U.ttype(mx, my)
    if (cost_land[tt] or 9999) >= 9999 or U.is_water(tt) then return false end
    if inp.env.pill_on(mx, my) then return false end
    return true
  end
  inp.buildable = function(mx, my)
    if no_builder then return false end
    local tt = U.ttype(mx, my)
    return tt ~= C.T_DEEPSEA and tt ~= C.T_RIVER and tt ~= C.T_SWAMP and
           tt ~= C.T_PILLBOX and tt ~= C.T_REFBASE and tt ~= C.T_BOAT
  end
  inp.lgm_reach = function(omx, omy, mx, my)
    if abs(mx - omx) + abs(my - omy) <= 1 then return true end
    return cpf.lgm_travel_ticks_map(omx, omy, mx, my, 0, 0, 2000, 150) ~= -1
  end
  return inp
end

return M
