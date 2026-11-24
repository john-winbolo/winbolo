-- =========================================================================
-- pilltake_v2_old.lua -- the pill take v2 evaluator (2026-11-16)
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
--              chain: every chain pill needs PT2_CHAIN_BLOCKER_MIN_SHOTS
--                (with PT2_LINK_COVERS_PREV: a chain pill whose ray meets
--                the next chain pill first is covered by it; only the
--                last chain pill needs the cover)
--                (10: two walls, or one allied pill of 10+ health);
--              any other pill in range (a bystander) needs one wall (5).
--   finish (PT2_FINISH_NO_COVER_MAX_ANGER): A has 1 health, its anger is
--     at or under the knob and our armour is ATTACK_RUSH_MIN_ARMOUR or more:
--     A's own ray needs no cover (bar 0). Every other pill keeps its bar.
--   P is SAFE when every X in T(P) is out of reach or has its bar, and the
--   tank has one clear aim at A (5 aims, attack_shield's offsets).
--   PT2_AIM_NUDGE: with no clear aim at P, P moves forward toward A by
--   v1's nudge (8 .. 256 wu), and the point is judged where it lands.
-- When a ray lacks cover, v2 plans blockers on it: a dropped carried pill
-- first, then walls, on squares the ray crosses before the tank, nearest
-- the pill first, up to PT2_BUILD_MAX. A wall never goes on our own shot
-- lane, nor in front of a chain link (it would break the chain).
--
-- Rank (highest first): safe, kills (chain length), travel band
-- (PT2_TRAVEL_BAND_TICKS: points whose score, in ticks, is within the band
-- of the quickest point of the same safe/kills tier count as equal; a
-- slower point ranks below them, and slower points rank by score; 0 = no
-- band), depth (chains only:
-- the shots of cover on the last chain pill's ray, so ally pill + wall 20
-- > ally pill 15 > two walls 10), blocker rank (an allied pill in the
-- cover above walls only; the tie-break after depth), leeway (neighbour
-- points that also pass), then score (lower is better: the caller's
-- travel score plus a cost per wall to build).
--
-- Hold point (a take that builds): where the tank waits while its builder
-- puts the walls up. It is on the standoff's bearing, from the approach
-- point outwards, at the first square no hostile pill can hit with the
-- cover that stands now (hold_point).
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

-- PT2_FINISH_NO_COVER_MAX_ANGER (Andrew, Nov 17): true when A's own ray
-- needs no cover: A has 1 health, its anger (inp.anger_of(A), else A.anger)
-- is at or under inp.finish_max_anger, and our armour (inp.armour) is at
-- least inp.finish_min_armour (ATTACK_RUSH_MIN_ARMOUR). A knob under 0
-- (KEEL / MAST: -1) is never true.
function M.finish_ok(inp, p)
  local m = inp.finish_max_anger
  if not m or m < 0 or not p or (p.health or 0) ~= 1 then return false end
  if (inp.armour or 0) < (inp.finish_min_armour or 5) then return false end
  local anger = inp.anger_of and inp.anger_of(p) or (p.anger or 0)
  return anger <= m
end

-- The evaluation context for target A.
local function new_ctx(inp, A)
  local ctx = { inp = inp, A = A, env = inp.env, reach_cache = {}, hostile = {} }
  for _, p in ipairs(inp.pills) do
    if alive(p) and not inp.env.is_ally(p) then ctx.hostile[#ctx.hostile + 1] = p end
  end
  ctx.finish = M.finish_ok(inp, A)
  return ctx
end

-- ── One tank point ────────────────────────────────────────────────────────
-- T(P): every hostile pill whose centre is within pill range (plus the
-- margin) of point (px, py). Returns T, and true when A is in it.
local function pills_in_range(ctx, px, py)
  local inp, A = ctx.inp, ctx.A
  local pr = inp.pill_range + inp.range_margin
  local pr2 = pr * pr
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
  return T, hasA
end

-- PT2_AIM_NUDGE: v1's forward nudge (attack_shield.lua, "Nudge pass").
-- No aim is clear at (px, py) and the aims in `shorts` fell short (no
-- wall, pill or base stopped them). Move the point forward, toward A's
-- centre, in steps of inp.nudge_step_wu up to inp.nudge_max_steps steps
-- (v1's NUDGE_STEP_WU 8 and NUDGE_MAX_STEPS 32: 8 .. 256 wu), smallest
-- step first, rounded to the nearest wu as v1's nudge_origin. At each
-- step try the short aims in aim order; the first clear one wins. An aim
-- that something stops at a step is dropped (v1: the nudged aim is then
-- blocked). A step whose square the tank may not stand on is skipped.
-- Returns nx, ny, nudge_wu, aim_idx, lane, or nil.
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
        local off = M.AIMS[ai]
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

-- Evaluate point (px, py) (world units) for target A with planned blockers
-- `extra` ({[sq] = shots}) and, when aim_fixed is set, only that aim.
-- Returns a record (see fields at the end) or nil, why.
--
-- prev / new_sq (optional, for speed only): prev is the record of the SAME
-- point and aim, and extra is prev's blockers plus one new square new_sq
-- that is not on the aim's shot lane. Then the pills in range, the aim and
-- its lane cannot change, and a ray changes only when it crosses new_sq, so
-- those parts are taken from prev. The result is the same as a full call.
--
-- nudge (optional): with inp.aim_nudge (PT2_AIM_NUDGE) and no clear aim at
-- (px, py), try v1's forward nudge (nudge_aim). The record is then for
-- the nudged point: rec.px, rec.py are it, every ray is cast to it, and
-- rec.nudge_wu is the distance moved (0 when not nudged).
local function eval_point(ctx, px, py, extra, aim_fixed, prev, new_sq, nudge)
  local inp, A, env = ctx.inp, ctx.A, ctx.env
  local T, aim_idx, lane, clear, lanes
  local nudge_wu = prev and prev.nudge_wu or 0
  if prev then
    T, aim_idx, lane, clear, lanes = prev.T, prev.aim_idx, prev.lane, prev.clear_aims, prev.lanes
    env.omx, env.omy = floor(px / 256), floor(py / 256)
  else
    -- T(P)
    local hasA
    T, hasA = pills_in_range(ctx, px, py)
    if not hasA then return nil, "A_OUT_OF_RANGE" end
    -- the tank's square, for the base rule of our own shot
    env.omx, env.omy = floor(px / 256), floor(py / 256)
    -- OUTGOING: the first clear aim (or the fixed one). The other clear
    -- aims are found only when the blocker search asks (clear_aims_of).
    local amx, amy = A.mx, A.my
    local a1, a2 = 1, #M.AIMS
    if aim_fixed then a1, a2 = aim_fixed, aim_fixed end
    local why_out = "short"
    local shorts = nil
    for ai = a1, a2 do
      local off = M.AIMS[ai]
      local ok, why, ln = SH.tank_shot(env, px, py, amx * 256 + off[1], amy * 256 + off[2],
                                       amx, amy, extra)
      if ok then
        aim_idx, lane = ai, ln
        break
      else
        why_out = why
        if why == "short" then
          shorts = shorts or {}
          shorts[#shorts + 1] = ai
        end
      end
    end
    if not aim_idx and nudge and shorts and inp.aim_nudge and not aim_fixed then
      local nx, ny, nw, nai, nln = nudge_aim(ctx, px, py, extra, shorts)
      if nx then
        px, py, nudge_wu, aim_idx, lane = nx, ny, nw, nai, nln
        -- the nudged point is closer to A, so A is still in range
        T = pills_in_range(ctx, px, py)
        env.omx, env.omy = floor(px / 256), floor(py / 256)
      end
    end
    if not aim_idx then return nil, "OUTGOING:" .. tostring(why_out) end
    clear = { aim_idx }
    lanes = { [aim_idx] = lane }
  end
  -- rays
  local rays = {}
  for i, X in ipairs(T) do
    local pr_ray = prev and prev.rays[i]
    local same = false
    if pr_ray then
      same = true
      local sql = pr_ray.sq
      for k = 1, pr_ray.n_sq do
        if sql[k] == new_sq then same = false; break end
      end
    end
    if same then
      rays[i] = pr_ray
    else
      local sx, sy = pill_centre(X)
      rays[i] = SH.cast(env, sx, sy, px, py, env.pill_fire_len or SH.PILL_FIRE_LEN, extra)
      rays[i].X = X
    end
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
        -- PT2_LINK_CLEAR_FRONT: a blocker in front of the link (res.front,
        -- planned blockers too) stops this pill's shells first, so the link
        -- is not there: the chain ends here and the blocker is its cover
        -- PT2_LINK_CUT_MIN_SHOTS: thin cover (under the knob) keeps it
        if nxt and inp.link_clear_front and M.link_cut(inp, r.front) then nxt = nil end
      else
        -- a chain pill out of range of P: its shells do not come at us,
        -- but they still go where its last target was. Not followed.
        nxt = nil
      end
      -- inp.chain_skip (PT2_CHAIN_CLAIM): a pill an ally holds against us
      -- ends the chain; the plan does not use it.
      if not nxt or in_chain[nxt] or (inp.chain_skip and inp.chain_skip[nxt]) then break end
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
    -- finish: A's own ray needs no cover; the other pills keep their bar
    if X == A and ctx.finish then bar = 0
    elseif X == A and not chain_mode then bar = inp.bar_single
    elseif in_chain[X] then bar = inp.bar_chain
    else bar = inp.bar_other or inp.wall_shots end
    r.bar = bar
    -- the next chain pill takes this pill's shells and dies first
    r.link_cover = inp.link_covers and in_chain[X] and r.link ~= nil
                   and r.link == chain[in_chain[X] + 1] or false
    local ok = (r.hit == "reach") or r.link_cover or (r.cover >= bar)
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
  -- depth: shots of cover on the last chain pill's ray (a single take: A's).
  -- A ray whose shell dies before it reaches us counts as its bar.
  local last, depth = chain[#chain], nil
  for i, X in ipairs(T) do
    if X == last then
      local r = rays[i]
      depth = (r.hit == "reach") and math.max(r.cover, r.bar) or r.cover
    end
  end
  depth = depth or (chain_mode and inp.bar_chain or inp.bar_single)
  return {
    px = px, py = py, nudge_wu = nudge_wu, T = T, rays = rays, chain = chain, in_chain = in_chain,
    chain_mode = chain_mode, aim_idx = aim_idx, lane = lane, clear_aims = clear, lanes = lanes,
    aims_done = aim_fixed or (prev and prev.aims_done) or aim_idx, extra_in = extra,
    covered = covered_all, uncovered = uncovered,
    safe = covered_all and max_ok, max_ok = max_ok,
    kills = (chain_mode and max_ok) and #chain or 1,
    ally_cover = ally_cover, need = need_A, have = have_A, depth = depth,
    finish = ctx.finish or false,
  }
end
M._eval_point = eval_point

-- Every clear aim of a point found with aim_fixed unset: the first one
-- eval_point found, then the rest. The blocker search tries each one,
-- since a wall on one aim's lane can be fine for another (v1 scores all
-- five).
local function clear_aims_of(ctx, rec)
  if rec.aims_done < #M.AIMS then
    local env, A = ctx.env, ctx.A
    env.omx, env.omy = floor(rec.px / 256), floor(rec.py / 256)
    for ai = rec.aims_done + 1, #M.AIMS do
      local off = M.AIMS[ai]
      local ok, _, ln = SH.tank_shot(env, rec.px, rec.py, A.mx * 256 + off[1], A.my * 256 + off[2],
                                     A.mx, A.my, rec.extra_in)
      if ok then
        rec.clear_aims[#rec.clear_aims + 1] = ai
        rec.lanes[ai] = ln
      end
    end
    rec.aims_done = #M.AIMS
  end
  return rec.clear_aims
end

-- The record of the same point with another clear aim: only the aim and
-- its lane differ (the rays do not depend on our aim).
local function with_aim(rec, ai)
  local o = {}
  for k, v in pairs(rec) do o[k] = v end
  o.aim_idx, o.lane = ai, rec.lanes[ai]
  o.clear_aims, o.lanes, o.aims_done = { ai }, { [ai] = rec.lanes[ai] }, ai
  return o
end

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
    local nrec = eval_point(ctx, rec.px, rec.py, extra, rec.aim_idx, cur, picked)
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
  for _, ai in ipairs(clear_aims_of(ctx, rec)) do
    local base = rec
    if ai ~= rec.aim_idx then
      base = with_aim(rec, ai)
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

-- ── Hold point ────────────────────────────────────────────────────────────
-- Where the tank waits while its builder puts the planned walls up. The
-- walls are not there yet, so only the cover that stands now counts
-- (extra = nil). A point is safe when every hostile pill in range either
-- cannot reach it, or meets its bar there: A of a single take the single
-- bar, a chain pill the chain bar, any other pill one wall. The search
-- walks out along the standoff's bearing from the approach point (v1's
-- waiting point) in steps of 64 wu (the overlay's CELL_WU), as far as the
-- approach point plus one pill range: a pill in range of the standoff is
-- out of range there. Returns { fx, fy (squares), d (squares from the
-- standoff), safe, why }: with no safe point, the approach point, safe =
-- false, why = the pill that hits it.
local HOLD_STEP = 64 / 256
local function hold_safe(ctx, b, hx, hy)
  local inp, env = ctx.inp, ctx.env
  local pr = inp.pill_range + inp.range_margin
  for _, p in ipairs(ctx.hostile) do
    local sx, sy = pill_centre(p)
    local dx, dy = sx - hx, sy - hy
    if dx * dx + dy * dy <= pr * pr then
      local bar
      if p == ctx.A and ctx.finish then bar = 0
      elseif p == ctx.A and not b.chain_mode then bar = inp.bar_single
      elseif b.in_chain and b.in_chain[p] then bar = inp.bar_chain
      else bar = inp.bar_other or inp.wall_shots end
      local r = SH.cast(env, sx, sy, hx, hy, env.pill_fire_len or SH.PILL_FIRE_LEN, nil)
      local ci = b.in_chain and b.in_chain[p]
      local linked = inp.link_covers and ci and r.link ~= nil and r.link == b.chain[ci + 1]
                     and not (inp.link_clear_front and M.link_cut(inp, r.front))
      if r.hit == "tank" and not linked and r.cover < bar then return false, M.pill_name(inp, p) end
    end
  end
  return true
end

function M.hold_point(ctx, b)
  local inp = ctx.inp
  local ux, uy = sin(rad(b.deg)), -cos(rad(b.deg))
  local d0 = inp.approach_offset
  local dmax = d0 + (inp.pill_range + inp.range_margin) / 256
  local first_why = nil
  local d = d0
  while d <= dmax + 1e-6 do
    local fx, fy = b.cx + ux * d, b.cy + uy * d
    local mx, my = floor(fx), floor(fy)
    if inp.in_map(mx, my) and inp.stay_in(mx, my) and inp.standable(mx, my) then
      local ok, why = hold_safe(ctx, b, floor(fx * 256), floor(fy * 256))
      if ok then return { fx = fx, fy = fy, d = d, safe = true } end
      first_why = first_why or why
    end
    d = d + HOLD_STEP
  end
  return { fx = b.cx + ux * d0, fy = b.cy + uy * d0, d = d0, safe = false, why = first_why or "no_square" }
end

-- Rank key comparison: true when a ranks above b.
-- Leeway counts up to cap (PT2_LEEWAY_MIN): past "enough room", the
-- travel score decides, as in v1. Without the cap a spot one neighbour
-- roomier but far off v1's line would always win.
-- band: PT2_TRAVEL_BAND_TICKS in score units (nil or 0: no band). The
-- quickest score is the lowest of the two points and of their tiers'
-- tier_min (set_band), so inside one sort every point of a tier meets the
-- same quickest point.
function M.better(a, b, cap, band)
  if a.safe ~= b.safe then return a.safe end
  if a.kills ~= b.kills then return a.kills > b.kills end
  if band and band > 0 then
    local sa, sb = a.score, b.score
    local m = math.min(sa, sb, a.tier_min or sa, b.tier_min or sb)
    local ia, ib = sa <= m + band, sb <= m + band
    if ia ~= ib then return ia end
    if not ia and sa ~= sb then return sa < sb end
  end
  -- depth only between chains: a single take keeps v1's order (parity)
  if a.kills >= 2 and (a.depth or 0) ~= (b.depth or 0) then return (a.depth or 0) > (b.depth or 0) end
  if a.blk ~= b.blk then return a.blk > b.blk end
  -- chains: the travel score (v1's travel term, plus the wall cost) comes
  -- before leeway; a chain already needs PT2_LEEWAY_MIN to be safe. A far
  -- spot must not win on one more neighbour (Oil Rig, Nov 16).
  if a.kills >= 2 and a.score ~= b.score then return a.score < b.score end
  cap = cap or 8
  local la, lb = math.min(a.leeway or -1, cap), math.min(b.leeway or -1, cap)
  if la ~= lb then return la > lb end
  return a.score < b.score
end

-- Travel band: the quickest score of each (safe, kills) tier, and whether
-- each point is in the band of it. Run again after safe can change.
local function set_band(scored, band)
  if not band or band <= 0 then return end
  local mins = {}
  for _, r in ipairs(scored) do
    local key = (r.safe and "s" or "u") .. r.kills
    if not mins[key] or r.score < mins[key] then mins[key] = r.score end
  end
  for _, r in ipairs(scored) do
    r.tier_min = mins[(r.safe and "s" or "u") .. r.kills]
    r.in_band = r.score <= r.tier_min + band
  end
end
M.set_band = set_band

-- Leeway of a given point and plan (for logs and the parity test).
function M.leeway_at(inp, px, py, extra)
  local ctx = new_ctx(inp, inp.A)
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
--   bar_single, bar_chain, bar_other (bystanders), chains, max_chain, leeway_on, leeway_ring_wu,
--   finish_max_anger, finish_min_armour (PT2_FINISH_NO_COVER_MAX_ANGER);
--   armour (ours), anger_of(p) (optional; else p.anger)
--   leeway_top, own_pill_rank, search_top, pill_range, range_margin,
--   blocker_min_dist, orth_only
--   keep_all              (optional) keep every candidate for the overlay
--   c_eval                (optional, PT2_C_EVAL) run the C copy when the exe has it
--   capture_file          (optional, PT2_CAPTURE_FILE) log the case for the parity test
--
-- M.evaluate picks the copy: the C one (gh_pt2_old.evaluate, gh_pt2_old.c) when
-- inp.c_eval is on and the exe registered it, else M.evaluate_lua. Both give
-- the same result (test_gh_pt2_old_parity). An old exe has no gh_pt2_old: Lua runs.
function M.evaluate(inp)
  local cf = inp.capture_file
  if cf and cf ~= "" then return require("pt2_capture").evaluate(M, inp, cf) end
  local g = inp.c_eval and rawget(_G, "gh_pt2_old")
  if g then return g.evaluate(inp, M) end
  if inp.c_eval and not M._warned_no_c then
    -- Log only, once per brain: an exe built before gh_pt2_old.c (a stale Bot
    -- Host) runs the Lua copy, about 7x slower, and budget-kills ticks.
    M._warned_no_c = true
    print("[pilltake_v2_old] PT2_C_EVAL is on but this exe has no gh_pt2_old:"
      .. " the Lua evaluator runs. Rebuild the exe.")
  end
  return M.evaluate_lua(inp)
end

function M.evaluate_lua(inp)
  local A = inp.A
  local env = inp.env
  local ctx = new_ctx(inp, A)
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
          local rec, why = eval_point(ctx, px, py, nil, nil, nil, nil, true)
          if rec then
            -- PT2_AIM_NUDGE moved the point forward on the same bearing:
            -- the tank drives to the nudged point, and every test was made there.
            if rec.nudge_wu > 0 then
              cx, cy = rec.px / 256, rec.py / 256
              mx, my = floor(cx), floor(cy)
            end
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
    local cap, band = inp.leeway_min, inp.travel_band
    set_band(scored, band)
    table.sort(scored, function(a, b) return M.better(a, b, cap, band) end)
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
      set_band(scored, band)
      table.sort(scored, function(a, b) return M.better(a, b, cap, band) end)
    end
    for _, rec in ipairs(scored) do all[#all + 1] = rec end
    if scored[1] and (not best or M.better(scored[1], best, cap, band)) then best = scored[1] end
    if best and best.safe then break end
  end
  if best and best.safe and best.plan and #best.plan > 0 then
    best.hold = M.hold_point(ctx, best)
  end
  return { best = best, all = inp.keep_all and all or nil, n_eval = n_eval, n_reject = n_reject,
           hostile = ctx.hostile, inp = inp }
end

-- ── evaluate_group ────────────────────────────────────────────────────────
-- A target in a group of hostile pills: a chain that kills the target can
-- start at another pill of the group (the mirror chain on the near side).
-- With chains on, every hostile pill in pill range of the target
-- (pill_range + range_margin) is tried as A too. Another A wins only with a
-- safe chain that has the target in it, and only when it ranks above the
-- target's own best (M.better; a tie keeps the target). inp.set_A(p), when
-- given, sets the per-pill inputs (banned angles, score_fn) before each
-- run. Returns the winner's result with res.A (the pill the tank shoots)
-- and res.goal_A (the target). inp.A is the target again on return.
function M.evaluate_group(inp)
  local job = M.group_begin(inp)
  while not M.group_step(job) do end
  return job.res
end

-- evaluate_group in parts, one M.evaluate per M.group_step call (pt2_dbl's
-- PT2_DBL_SPLIT spreads a shadow run over ticks). The parts give the same
-- result as evaluate_group when the inputs do not change between them.
-- group_step returns true when the job is done; the result is job.res.
function M.group_begin(inp)
  return { inp = inp, goalA = inp.A, list = nil, i = 1, res = nil }
end

function M.group_parts_left(job)
  if not job.list then return 1 end
  return #job.list - job.i + 1
end

function M.group_step(job)
  local inp, goalA = job.inp, job.goalA
  if not job.list then
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
    if inp.set_A then inp.set_A(p) end
    local r = M.evaluate(inp)
    inp.A = goalA
    if inp.set_A then inp.set_A(goalA) end
    job.n_eval, job.n_reject = job.n_eval + r.n_eval, job.n_reject + r.n_reject
    local b, res = r.best, job.res
    if b and b.safe and b.chain_mode and b.in_chain and b.in_chain[goalA]
       and (not res.best or M.better(b, res.best, inp.leeway_min, inp.travel_band)) then
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

-- Run v2's tests on a given point and plan (v1's choice) and return the
-- first failing test: "-" when it passes them all. extra: {[sq] = shots}.
function M.judge(inp, px, py, extra, aim_idx)
  local ctx = new_ctx(inp, inp.A)
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
-- shadow.lua blocker kinds -> attack_shield's names.
local KIND_V1 = { ally_pill = "friendly_pill", wall = "wall_full", half_wall = "wall_half" }

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
          act[#act + 1] = { mx = bl.sq % 256, my = floor(bl.sq / 256), kind = KIND_V1[bl.kind] or bl.kind }
        end
      end
    end
  end
  for _, s in ipairs(b.plan) do
    pot[#pot + 1] = { mx = s.mx, my = s.my, kind = "empty", pt2_kind = s.kind,
                      adj_friendly = adj_friendly_fn and adj_friendly_fn(s.mx, s.my) or nil }
  end
  local chain_ids = {}
  local chain_idv = {}
  for i, p in ipairs(b.chain) do
    chain_ids[i] = M.pill_name(res.inp, p)
    chain_idv[i] = res.inp.ids and res.inp.ids[p]
  end
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
    unreachable_blockers = {}, nudge_wu = b.nudge_wu or 0, score = b.have,
  }
  return {
    best = best, pill = pill, standoff = standoff, pill_hp = pill.health,
    fav_bonus_by_size = {}, min_chain = 0, tier_label = "PT2",
    pt2 = true,
    chain = { pills = chain_ids, ids = chain_idv, step = 1,
              kills = b.kills, mode = b.chain_mode and "HOLD" or "SINGLE",
              need = b.need, have = b.have, safe = b.safe,
              -- PT2_FINISH_NO_COVER_MAX_ANGER picked this point: A's ray
              -- had no cover need (attack.lua keeps the swerve on)
              finish = b.finish or nil },
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
  -- For the C copy (gh_pt2_old.c): base_stops is false on a square with no
  -- base_at entry, so the C skips the call there. The C reads each
  -- square's terrain once per evaluate; count_reads(n) adds the reads it
  -- took from its cache to U.ttype's counter.
  env.base_at = world.base_at
  env.count_reads = function(n) require("metrics").inc("get_terrain", n) end
  return env
end

-- PT2_LIVE_SHELL: the live shell rules (info.rules) into the shadow env,
-- where shadow.lua and gh_pt2_old.c read them. A rule not sent (an older exe)
-- or out of range stays nil: the walk then uses the classic number.
function M.live_shell(env, r)
  if type(env) ~= "table" or type(r) ~= "table" then return env end
  local function pos(v) return (type(v) == "number" and v > 0) and v or nil end
  env.shell_speed = pos(r.shell_speed)
  env.shell_life = pos(r.shell_life)
  local a = r.shell_start_add
  env.shell_start_add = (type(a) == "number" and a >= 0) and a or nil
  env.tank_hit_r = pos(r.tank_hit_radius)
  env.pill_fire_len = pos(r.pill_fire_length)
  return env
end

-- PT2_TANKSHOT_BLOCKERS: what else stops our own shell in tank_shot, as in
-- the engine: a boat square, and the other tanks this bot sees (info.objects,
-- not its own), where they are now. Ally and enemy alike: the engine's shell
-- stops on any tank but the shooter's. Sets env.ts_boats and env.ts_tanks.
function M.tank_blockers(env, info)
  if type(env) ~= "table" then return env end
  env.ts_boats = true
  local t, n = {}, 0
  local me = info and info.player_number
  local tank_type = rawget(_G, "OBJECT_TANK")
  for _, ob in ipairs((info and info.objects) or {}) do
    if ob.type == tank_type and ob.idnum ~= me and type(ob.x) == "number" and type(ob.y) == "number" then
      t[n + 1], t[n + 2] = ob.x, ob.y
      n = n + 2
    end
  end
  env.ts_tanks = n > 0 and t or nil
  return env
end

-- The knob part of inp, read from constants each call (cfg can change).
-- PT2_LINK_CLEAR_FRONT with PT2_LINK_CUT_MIN_SHOTS (E2 thin cover): does
-- the cover in front of a link (front, in shots) cut it? Any cover when
-- the knob is 0 (or unset), else at least that many shots.
function M.link_cut(inp, front)
  return front > 0 and front >= (inp.link_cut_min_shots or 0)
end

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
  inp.bar_chain        = C.PT2_CHAIN_BLOCKER_MIN_SHOTS or 10
  inp.bar_other        = inp.wall_shots       -- a bystander: one wall
  -- finish: a 1-health A at or under this anger needs no cover on its own
  -- ray (-1 = never), with our armour at ATTACK_RUSH_MIN_ARMOUR or more
  inp.finish_max_anger = C.PT2_FINISH_NO_COVER_MAX_ANGER or -1
  inp.finish_min_armour = C.ATTACK_RUSH_MIN_ARMOUR or 5
  inp.chains           = C.PT2_CHAINS ~= false
  inp.link_covers      = C.PT2_LINK_COVERS_PREV ~= false
  inp.link_clear_front = C.PT2_LINK_CLEAR_FRONT ~= false
  inp.link_cut_min_shots = C.PT2_LINK_CUT_MIN_SHOTS or 0
  inp.max_chain        = C.PT2_MAX_CHAIN or 3
  inp.leeway_on        = C.PT2_LEEWAY ~= false
  inp.leeway_ring_wu   = 32               -- Andrew approved Nov 24
  inp.leeway_top       = C.PT2_LEEWAY_TOP or 8
  inp.leeway_min       = C.PT2_LEEWAY_MIN or 6
  inp.own_pill_rank    = C.PT2_OWN_PILL_BONUS ~= false
  -- travel band: the score is v1's Dijkstra cost (TERRAIN_COST_LAND: road
  -- 1 per square), so one score unit is a road square: 256 wu at the road
  -- speed MAP_SPEED[T_ROAD] (16 wu/tick) = 16 ticks. The band is in ticks.
  inp.ticks_per_score  = 256 / ((C.MAP_SPEED and C.MAP_SPEED[C.T_ROAD]) or 16)
  inp.travel_band_ticks = C.PT2_TRAVEL_BAND_TICKS or 0
  inp.travel_band      = inp.travel_band_ticks / inp.ticks_per_score
  inp.search_top       = 24               -- proposal, awaiting Andrew's value
  inp.pill_range       = SH.PILL_RANGE
  inp.range_margin     = C.PT2_RANGE_MARGIN_WU or 128
  inp.blocker_min_dist = 1.0              -- attack_shield.BLOCKER_MIN_DIST
  inp.orth_only        = C.BLOCKER_ORTHOGONAL_ONLY and true or false
  inp.aim_nudge        = C.PT2_AIM_NUDGE ~= false
  inp.nudge_step_wu    = 8                -- attack_shield.lua NUDGE_STEP_WU
  inp.nudge_max_steps  = 32               -- attack_shield.lua NUDGE_MAX_STEPS
  inp.c_eval           = C.PT2_C_EVAL ~= false
  inp.capture_file     = C.PT2_CAPTURE_FILE
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
  -- PT2_C_STANDABLE: the tables for the C copy of the four square tests
  -- above (gh_pt2_old.c). The C does a test itself only while inp's function is
  -- the one named here; a function put in its place later stays Lua. An
  -- older exe skips inp.c_std. standable and buildable read U.ttype as
  -- env.ttype, so only when the two are the same.
  if C.PT2_C_STANDABLE ~= false then
    local N = U.C_COPIED or {}
    local same_tt = rawequal(inp.env.ttype, U.ttype)
    inp.c_std = {
      in_map    = rawequal(U.in_map, N.in_map) and inp.in_map or nil,
      stay_in   = rawequal(U.stay_in, N.stay_in) and U.stay_rects and inp.stay_in or nil,
      standable = same_tt and rawequal(U.is_water, N.is_water) and inp.standable or nil,
      buildable = same_tt and inp.buildable or nil,
      cost      = cost_land,
      water     = { n = 2, C.T_RIVER, C.T_DEEPSEA },
      nobuild   = { n = 6, C.T_DEEPSEA, C.T_RIVER, C.T_SWAMP, C.T_PILLBOX, C.T_REFBASE, C.T_BOAT },
      no_builder = no_builder and true or false,
      rects     = U.stay_rects and U.stay_rects() or false,
    }
  else
    inp.c_std = nil
  end
  inp.lgm_reach = function(omx, omy, mx, my)
    if abs(mx - omx) + abs(my - omy) <= 1 then return true end
    return cpf.lgm_travel_ticks_map(omx, omy, mx, my, 0, 0, 2000, 150) ~= -1
  end
  return inp
end

return M
