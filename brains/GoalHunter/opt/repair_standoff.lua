local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/repair_standoff.lua — where the tank stops for a repair_pill
-- goal (knob C.REPAIR_STANDOFF_ENABLED, 2026-11-03).
--
-- THE PROBLEM (debug session 20261103_192602 part 2, player 2, t=8780-9080):
-- the repair_pill route ended on the pill square, so the tank parked one
-- square from the pill, inside the range of two neutral pills. Their misses
-- hit our pill (14 -> 8 health), so the under-fire hold kept the builder in
-- the tank, the repair never went out and the goal changed to attack_pill.
--
-- THE RULE
--   A square is SAFE from a hostile or neutral pill P (live, known to this
--   player: world.pills) when either
--     a. its distance from P is >= pill range (PILLBOX_RANGE_WU / 256, 8
--        tiles) + REPAIR_STANDOFF_MARGIN_TILES; or
--     b. REPAIR_STANDOFF_WALL_COVER is on, P is not heated
--        (P.anger <= REPAIR_STANDOFF_HEATED_ANGER), and a wall stops every
--        pill shell line from P to the tank body on that square.
--   The body test: five shell lines (cpf.simulate_shot, SHOT_PILL, the same
--   return-fire simulation the attack shield uses) from P's centre to the
--   square centre and to the points 56 and 112 world units (TANK_HIT_RADIUS)
--   to each side of it, across the line. Each line must meet a cover square
--   before it gets to the tank square. A cover square is any wall (built or
--   damaged: T_BUILDING, T_HALFBUILD), or a friendly (own or allied) pill
--   that is worth one wall or more under PILL_COVER_BY_HEALTH
--   (pill_cover.lua, floor(health / 5); with that knob off any live friendly
--   pill counts). Trees do not count. The pill we repair is never cover: a
--   shell that hits it starts the under-fire hold this rule exists to stop.
--
--   The STANDOFF is the safe square (from every such pill) closest to the
--   target pill, where also
--     * it is a square the tank can stand on (land, not a wall, not a pill);
--     * it is within the dispatch cap, REPAIR_DISPATCH_DIST_DANGEROUS (12),
--       measured as every builder leash is (U.leash_dist);
--     * the builder walk sim (cpf_lgm_travel_ticks_map, the sim the repair
--       dispatch uses for its ETA) gets from it to the pill within
--       REPAIR_STANDOFF_MAX_WALK_TICKS;
--     * the tank's route to it (the KIND_NORMAL Dijkstra slate trace) does
--       not ENTER an unsafe square. Squares at the start of the route while
--       the tank is still inside a pill's range do not count: the tank is
--       leaving that range, not entering it.
--   Ties on distance go to the square nearer the tank.
--
--   No pill that could make a square unsafe (none within cap + range +
--   margin of the target pill): mode "clear", and the goal works as it did
--   before this knob. A threat but no square passes: mode "push", the tank
--   drives at the pill as before and the feeder sends the builder by the
--   old rules.
--
-- WHEN IT PICKS AGAIN (not every tick):
--   * the target pill changes;
--   * the set of pills that matter changes (one seen, one gone, a move, a
--     pill becomes heated or calm), or a knob changes;
--   * a cover square the pick relied on stops being cover;
--   * REFRESH_TICKS have passed (a new wall, a new route).
--   None of these while the builder is out of the tank: the tank waits on
--   the square it sent the builder from.
--
-- LOG: one REPAIR_STANDOFF line per pick, with every factor.
-- VIZ: repair_pill_viz (the standoff, the safe rings, the tank square's
-- safe / unsafe reason).
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local PC     = require("pill_cover")
local print2 = require("print2")
local viz    = require("viz")

local M = {}

local sqrt, floor, huge = math.sqrt, math.floor, math.huge

-- Fixed work limits (not tunables; they bound the CPU of one pick).
M.REFRESH_TICKS   = 100   -- pick again at least this often (2 s)
M.MAX_FULL_CHECKS = 40    -- walk sims + route checks per pick
M.BODY_OFFSETS_WU = { 0, -112, 112, -56, 56 }   -- across the shell line
M.NEAR_LINE_TILES = 0.5   -- a cover square this close to the centre line
                          -- may stop a body line (112 wu = 0.44 tile)
M.ROUTE_MAX_STEPS = 2048

-- Engine calls, replaced by the unit tests.
M.deps = {
  ttype = function(mx, my) return U.ttype_peek(mx, my) end,
  shot  = function(ox, oy, tx, ty)
    local cpf = require("cpathfinder")
    local ok, tiles = pcall(cpf.simulate_shot, ox, oy, tx, ty, cpf.SHOT_PILL, 0)
    if ok then return tiles end
    return nil
  end,
  walk  = function(sx, sy, px, py, max_ticks)
    if not cpf_lgm_travel_ticks_map then return nil end
    return cpf_lgm_travel_ticks_map(sx, sy, px, py, px, py, max_ticks,
                                    C.BUILDER_POOL_LGM_STUCK_TICKS or 150)
  end,
  route = function(mx, my)
    local cpf = require("cpathfinder")
    return cpf.dijkstra_trace_path_by_kind(cpf.KIND_NORMAL, mx, my, M.ROUTE_MAX_STEPS)
  end,
  clock = function() return clock_us and clock_us() or 0 end,
}

function M.enabled()
  return C.REPAIR_STANDOFF_ENABLED == true
end

function M.safe_dist()
  return (C.PILLBOX_RANGE_WU or 2048) / 256 + (C.REPAIR_STANDOFF_MARGIN_TILES or 1.0)
end

function M.cap()
  return C.REPAIR_DISPATCH_DIST_DANGEROUS or 12
end

local function heated(p)
  return (p.anger or 0) > (C.REPAIR_STANDOFF_HEATED_ANGER or 0.34)
end
M.heated = heated

-- ── Geometry ─────────────────────────────────────────────────────────────

local function clip(p, q, t0, t1)
  if p == 0 then
    if q < 0 then return nil end
    return t0, t1
  end
  local r = q / p
  if p < 0 then
    if r > t1 then return nil end
    if r > t0 then t0 = r end
  else
    if r < t0 then return nil end
    if r < t1 then t1 = r end
  end
  return t0, t1
end

local function pt_square_d2(px, py, bx, by)
  local dx = 0
  if px < bx then dx = bx - px elseif px > bx + 1 then dx = px - bx - 1 end
  local dy = 0
  if py < by then dy = by - py elseif py > by + 1 then dy = py - by - 1 end
  return dx * dx + dy * dy
end

-- Shortest distance (tiles) from segment A-B to the square [bx,bx+1]x[by,by+1].
function M.seg_square_dist(ax, ay, ox, oy, bx, by)
  local vx, vy = ox - ax, oy - ay
  local L = sqrt(vx * vx + vy * vy)
  if L < 1e-9 then return sqrt(pt_square_d2(ax, ay, bx, by)) end
  local t0, t1 = 0, 1
  t0, t1 = clip(-vx, ax - bx, t0, t1)
  if t0 then t0, t1 = clip(vx, bx + 1 - ax, t0, t1) end
  if t0 then t0, t1 = clip(-vy, ay - by, t0, t1) end
  if t0 then t0, t1 = clip(vy, by + 1 - ay, t0, t1) end
  if t0 then return 0 end
  local ux, uy = vx / L, vy / L
  local best = huge
  for c = 1, 4 do
    local cx = (c == 2 or c == 4) and (bx + 1) or bx
    local cy = (c >= 3) and (by + 1) or by
    local t = (cx - ax) * ux + (cy - ay) * uy
    if t < 0 then t = 0 elseif t > L then t = L end
    local qx, qy = cx - (ax + ux * t), cy - (ay + uy * t)
    local d2 = qx * qx + qy * qy
    if d2 < best then best = d2 end
  end
  local da = pt_square_d2(ax, ay, bx, by)
  if da < best then best = da end
  local dob = pt_square_d2(ox, oy, bx, by)
  if dob < best then best = dob end
  return sqrt(best)
end

-- ── World reads ──────────────────────────────────────────────────────────

-- Does this friendly pill give cover? (PILL_COVER_BY_HEALTH: one wall or more.)
local function pill_gives_cover(p)
  if not p or p.in_tank or not PC.is_friendly_owner(p.owner) then return false end
  if (p.health or 0) <= 0 then return false end
  if C.PILL_COVER_BY_HEALTH then return PC.pill_walls(p) >= 1 end
  return true
end
M.pill_gives_cover = pill_gives_cover

-- Cover kind on a square: "wall", "pill" or false.
function M.cover_at(world, mx, my)
  local tt = M.deps.ttype(mx, my)
  if tt == C.T_BUILDING or tt == C.T_HALFBUILD then return "wall" end
  local p = PC.pill_on_square(world, mx, my)
  if p and pill_gives_cover(p) then return "pill" end
  return false
end

-- Can the tank stand on this square?
local function standable(world, mx, my)
  if not U.in_map(mx, my) then return false, "off_map" end
  local tt = M.deps.ttype(mx, my)
  if tt == C.T_BOAT or tt == C.T_PILLBOX or U.is_water(tt) then return false, "terrain" end
  if (C.TERRAIN_COST_LAND[tt] or 9999) >= 9999 then return false, "terrain" end
  if PC.pill_on_square(world, mx, my) then return false, "pill" end
  return true
end

-- The hostile / neutral live pills that could make a square near the target
-- pill unsafe: within cap + safe distance of it. Sorted by id.
function M.threats(world, pmx, pmy)
  local reach = M.cap() + M.safe_dist() + 1
  local out = {}
  for id, p in pairs(world.pills or {}) do
    if (p.owner == "hostile" or p.owner == "neutral") and not p.in_tank
       and (p.health or 0) > 0 and p.mx and p.my
       and not (p.mx == pmx and p.my == pmy) then
      local dx, dy = p.mx - pmx, p.my - pmy
      if sqrt(dx * dx + dy * dy) <= reach then
        out[#out + 1] = { id = id, mx = p.mx, my = p.my, pill = p,
                          heated = heated(p), anger = p.anger or 0 }
      end
    end
  end
  table.sort(out, function(a, b) return tostring(a.id) < tostring(b.id) end)
  return out
end

function M.signature(pkey, threats)
  local parts = { tostring(pkey),
                  tostring(C.REPAIR_STANDOFF_MARGIN_TILES),
                  tostring(C.REPAIR_STANDOFF_MAX_WALK_TICKS),
                  tostring(C.REPAIR_STANDOFF_WALL_COVER),
                  tostring(C.REPAIR_STANDOFF_HEATED_ANGER),
                  tostring(C.PILL_COVER_BY_HEALTH),
                  tostring(M.cap()) }
  for _, t in ipairs(threats) do
    parts[#parts + 1] = string.format("%s:%d:%d:%d", tostring(t.id), t.mx, t.my,
                                      t.heated and 1 or 0)
  end
  return table.concat(parts, "|")
end

-- ── The pick ─────────────────────────────────────────────────────────────

-- A pick context: memos for one pick (and for the viz reads between picks).
function M.new_ctx(world, pmx, pmy, threats)
  local ctx = { world = world, pmx = pmx, pmy = pmy, threats = threats,
                safe_d = M.safe_dist(), wall_cover = C.REPAIR_STANDOFF_WALL_COVER == true,
                safe = {}, cover_memo = {}, covers = nil, sims = 0,
                used_cover = {} }
  return ctx
end

-- Cover squares near the target pill (built once per context, only when
-- some pill could be covered against).
local function cover_list(ctx)
  if ctx.covers then return ctx.covers end
  local list = {}
  local r = floor(M.cap() + ctx.safe_d + 2)
  local x0, x1 = math.max(0, ctx.pmx - r), math.min(255, ctx.pmx + r)
  local y0, y1 = math.max(0, ctx.pmy - r), math.min(255, ctx.pmy + r)
  local tt_of = M.deps.ttype
  for y = y0, y1 do
    for x = x0, x1 do
      local tt = tt_of(x, y)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
        list[#list + 1] = { mx = x, my = y, kind = "wall" }
      end
    end
  end
  for _, p in pairs(ctx.world.pills or {}) do
    if p.mx and p.my and pill_gives_cover(p)
       and math.abs(p.mx - ctx.pmx) <= r and math.abs(p.my - ctx.pmy) <= r then
      list[#list + 1] = { mx = p.mx, my = p.my, kind = "pill" }
    end
  end
  ctx.covers = list
  return list
end

-- Does a wall stop every body line from threat t to the tank on (mx,my)?
-- Returns covered, and the cover squares it used.
function M.covered(ctx, t, mx, my)
  local memo = ctx.cover_memo[t.id]
  if not memo then memo = {}; ctx.cover_memo[t.id] = memo end
  local sk = my * 256 + mx
  local m = memo[sk]
  if m ~= nil then return m.ok, m.used end
  local ax, ay = t.mx + 0.5, t.my + 0.5
  local bx, by = mx + 0.5, my + 0.5
  -- Cheap test first: no cover square near the centre line -> open.
  local near = false
  for _, cv in ipairs(cover_list(ctx)) do
    if not (cv.mx == t.mx and cv.my == t.my) and not (cv.mx == mx and cv.my == my)
       and not (cv.mx == ctx.pmx and cv.my == ctx.pmy)
       and M.seg_square_dist(ax, ay, bx, by, cv.mx, cv.my) <= M.NEAR_LINE_TILES then
      near = true
      break
    end
  end
  if not near then
    memo[sk] = { ok = false }
    return false
  end
  local vx, vy = bx - ax, by - ay
  local L = sqrt(vx * vx + vy * vy)
  local nx, ny = 0, 0
  if L > 1e-9 then nx, ny = -vy / L, vx / L end
  local owx, owy = U.m2w(t.mx), U.m2w(t.my)
  local cwx, cwy = U.m2w(mx), U.m2w(my)
  local used = {}
  for _, off in ipairs(M.BODY_OFFSETS_WU) do
    local twx = floor(cwx + nx * off + 0.5)
    local twy = floor(cwy + ny * off + 0.5)
    local tiles = M.deps.shot(owx, owy, twx, twy)
    ctx.sims = ctx.sims + 1
    local blocked = false
    if tiles then
      for i = 1, #tiles do
        local s = tiles[i]
        if s.mx == mx and s.my == my then break end
        if not (s.mx == t.mx and s.my == t.my)
           and not (s.mx == ctx.pmx and s.my == ctx.pmy) then
          local kind = M.cover_at(ctx.world, s.mx, s.my)
          if kind then
            blocked = true
            used[#used + 1] = { mx = s.mx, my = s.my, kind = kind }
            break
          end
        end
      end
    end
    if not blocked then
      memo[sk] = { ok = false }
      return false
    end
  end
  memo[sk] = { ok = true, used = used }
  return true, used
end

-- Per-pill verdict for one square: list of { t, d, verdict } where verdict is
-- "far", "cover", "in_range" (wall cover off), "heated" or "open".
function M.square_detail(ctx, mx, my)
  local out = {}
  for _, t in ipairs(ctx.threats) do
    local dx, dy = mx - t.mx, my - t.my
    local d = sqrt(dx * dx + dy * dy)
    local v
    if d >= ctx.safe_d then v = "far"
    elseif not ctx.wall_cover then v = "in_range"
    elseif t.heated then v = "heated"
    elseif M.covered(ctx, t, mx, my) then v = "cover"
    else v = "open" end
    out[#out + 1] = { t = t, d = d, verdict = v }
  end
  return out
end

-- Is (mx,my) safe from every threat? Returns safe, and on unsafe the threat
-- and reason ("in_range", "heated", "open").
function M.square_safe(ctx, mx, my)
  local sk = my * 256 + mx
  local m = ctx.safe[sk]
  if m then return m.ok, m.t, m.why end
  local safe_d2 = ctx.safe_d * ctx.safe_d
  for _, t in ipairs(ctx.threats) do
    local dx, dy = mx - t.mx, my - t.my
    if dx * dx + dy * dy < safe_d2 then
      local why
      if not ctx.wall_cover then why = "in_range"
      elseif t.heated then why = "heated"
      else
        local ok, used = M.covered(ctx, t, mx, my)
        if not ok then why = "open"
        else
          for _, u in ipairs(used or {}) do
            ctx.used_cover[u.my * 256 + u.mx] = u
          end
        end
      end
      if why then
        ctx.safe[sk] = { ok = false, t = t, why = why }
        return false, t, why
      end
    end
  end
  ctx.safe[sk] = { ok = true }
  return true
end

-- The tank's route to (mx,my): ok, n squares, and on a fail the first unsafe
-- square it enters { mx, my, t, why }.
function M.route_ok(ctx, tmx, tmy, mx, my)
  if tmx == mx and tmy == my then return true, 0 end
  local path = M.deps.route(mx, my)
  if not path or #path < 2 then return false, 0, { why = "no_route" } end
  local n = floor(#path / 2)
  -- Start at the tank's own square when the route passes it.
  local first = 1
  for i = 1, n do
    if path[2 * i - 1] == tmx and path[2 * i] == tmy then first = i; break end
  end
  local entered_safe = false
  for i = first, n do
    local x, y = path[2 * i - 1], path[2 * i]
    local ok, t, why = M.square_safe(ctx, x, y)
    if ok then
      entered_safe = true
    elseif entered_safe then
      return false, n - first + 1, { mx = x, my = y, t = t, why = why }
    end
  end
  return true, n - first + 1
end

-- Builder walk ticks from (mx,my) to the pill, or nil.
function M.walk_ticks(mx, my, pmx, pmy)
  local near = C.BUILDER_POOL_GRASS_TICKS_PER_TILE or 16
  if math.abs(mx - pmx) + math.abs(my - pmy) <= 1 then return near end
  local maxw = C.REPAIR_STANDOFF_MAX_WALK_TICKS or 400
  local w = M.deps.walk(mx, my, pmx, pmy, maxw)
  if w == nil or w < 0 then return nil end
  if w == 0 then w = near end
  if w > maxw then return nil end
  return w
end

-- Candidate offsets within the cap, sorted by distance (built once per cap).
local _offs = { cap = nil, list = nil }
local function offsets()
  local cap = M.cap()
  if _offs.cap == cap then return _offs.list end
  local r = math.ceil(cap)
  local list = {}
  for dy = -r, r do
    for dx = -r, r do
      if not (dx == 0 and dy == 0) then
        list[#list + 1] = { dx = dx, dy = dy, d = sqrt(dx * dx + dy * dy) }
      end
    end
  end
  table.sort(list, function(a, b)
    if a.d ~= b.d then return a.d < b.d end
    if a.dy ~= b.dy then return a.dy < b.dy end
    return a.dx < b.dx
  end)
  _offs.cap, _offs.list = cap, list
  return list
end

-- pick(world, pmx, pmy, tmx, tmy, threats) -> record
--   mode   "standoff" (mx, my, d, walk, route_n), "push" (why), or "clear"
--   counts { looked, unsafe, terrain, cap, walk, route, full }
--   first_reject { mx, my, why }  the nearest square that failed a full check
function M.pick(world, pmx, pmy, tmx, tmy, threats)
  local t0 = M.deps.clock()
  local rec = { pmx = pmx, pmy = pmy, tmx = tmx, tmy = tmy,
                counts = { looked = 0, unsafe = 0, terrain = 0, cap = 0,
                           walk = 0, route = 0, full = 0 } }
  if #threats == 0 then
    rec.mode, rec.why = "clear", "no_threat"
    rec.us = M.deps.clock() - t0
    return rec, nil
  end
  local ctx = M.new_ctx(world, pmx, pmy, threats)
  local cap = M.cap()
  local cnt = rec.counts
  local list = offsets()
  local i, n = 1, #list
  while i <= n do
    -- One distance band at a time, nearest the tank first inside it.
    local d = list[i].d
    local j = i
    local band = {}
    while j <= n and list[j].d == d do
      local o = list[j]
      local mx, my = pmx + o.dx, pmy + o.dy
      local tdx, tdy = mx - tmx, my - tmy
      band[#band + 1] = { mx = mx, my = my, td = tdx * tdx + tdy * tdy }
      j = j + 1
    end
    table.sort(band, function(a, b)
      if a.td ~= b.td then return a.td < b.td end
      if a.my ~= b.my then return a.my < b.my end
      return a.mx < b.mx
    end)
    for _, c in ipairs(band) do
      local mx, my = c.mx, c.my
      cnt.looked = cnt.looked + 1
      if U.leash_dist(mx, my, pmx, pmy) > cap then
        cnt.cap = cnt.cap + 1
      elseif not standable(world, mx, my) then
        cnt.terrain = cnt.terrain + 1
      elseif not M.square_safe(ctx, mx, my) then
        cnt.unsafe = cnt.unsafe + 1
      else
        if cnt.full >= M.MAX_FULL_CHECKS then
          rec.mode, rec.why = "push", "check_limit"
          rec.us = M.deps.clock() - t0
          rec.sims = ctx.sims
          return rec, ctx
        end
        cnt.full = cnt.full + 1
        local w = M.walk_ticks(mx, my, pmx, pmy)
        if not w then
          cnt.walk = cnt.walk + 1
          rec.first_reject = rec.first_reject or { mx = mx, my = my, why = "walk" }
        else
          local ok, rn, bad = M.route_ok(ctx, tmx, tmy, mx, my)
          if not ok then
            cnt.route = cnt.route + 1
            rec.first_reject = rec.first_reject or { mx = mx, my = my,
              why = bad and bad.why == "no_route" and "no_route"
                    or string.format("route_enters(%d,%d)#%s:%s",
                         bad.mx, bad.my, tostring(bad.t and bad.t.id), tostring(bad.why)) }
          else
            rec.mode = "standoff"
            rec.mx, rec.my = mx, my
            rec.d = U.edist(mx, my, pmx, pmy)
            rec.walk = w
            rec.route_n = rn
            rec.us = M.deps.clock() - t0
            rec.sims = ctx.sims
            return rec, ctx
          end
        end
      end
    end
    i = j
  end
  rec.mode, rec.why = "push", "no_safe_square"
  rec.us = M.deps.clock() - t0
  rec.sims = ctx.sims
  return rec, ctx
end

-- Cover squares the pick relied on (the standoff and the route): list.
local function used_list(ctx)
  local out = {}
  if not ctx then return out end
  for _, u in pairs(ctx.used_cover) do out[#out + 1] = u end
  table.sort(out, function(a, b)
    if a.my ~= b.my then return a.my < b.my end
    return a.mx < b.mx
  end)
  return out
end

-- The REPAIR_STANDOFF log line for a pick.
function M.log_line(now, rec, ctx, reason)
  local c = rec.counts
  local head = string.format(
    "REPAIR_STANDOFF t=%d pill=(%d,%d) tank=(%d,%d) mode=%s",
    now, rec.pmx, rec.pmy, rec.tmx, rec.tmy, rec.mode)
  local body
  if rec.mode == "standoff" then
    body = string.format(" pick=(%d,%d) d=%.1f cap=%s walk=%d/%d route=ok(n=%d)",
      rec.mx, rec.my, rec.d, U.fmt_ld(M.cap()), rec.walk,
      C.REPAIR_STANDOFF_MAX_WALK_TICKS or 400, rec.route_n or 0)
  else
    body = string.format(" reason=%s", tostring(rec.why))
  end
  local pills = {}
  local det = (rec.mode == "standoff" and ctx) and M.square_detail(ctx, rec.mx, rec.my) or nil
  for k, t in ipairs((ctx and ctx.threats) or rec.threats or {}) do
    local s = string.format("#%s(%d,%d) anger=%.2f heated=%s", tostring(t.id),
      t.mx, t.my, t.anger or 0, t.heated and "yes" or "no")
    if det then
      local e = det[k]
      s = s .. string.format(" d=%.1f/%.1f %s", e.d, ctx.safe_d,
        e.verdict == "far" and "far" or ("in_range cover=" .. (e.verdict == "cover" and "yes" or ("no(" .. e.verdict .. ")"))))
    end
    pills[#pills + 1] = s
  end
  local used = {}
  for _, u in ipairs(rec.cover_used or {}) do
    used[#used + 1] = string.format("%s(%d,%d)", u.kind, u.mx, u.my)
  end
  local fr = rec.first_reject
  return head .. body .. string.format(
    " pills=[%s] cover_used=[%s] looked=%d unsafe=%d terrain=%d over_cap=%d walk_fail=%d route_fail=%d full=%d/%d%s sims=%d us=%d why_pick=%s",
    table.concat(pills, "; "), table.concat(used, " "),
    c.looked, c.unsafe, c.terrain, c.cap, c.walk, c.route, c.full, M.MAX_FULL_CHECKS,
    fr and string.format(" first_reject=(%d,%d):%s", fr.mx, fr.my, fr.why) or "",
    rec.sims or 0, rec.us or 0, tostring(reason))
end

-- ── Per-tick driver ──────────────────────────────────────────────────────

local function target_pill(world, g)
  local p = g.target_id and world.pills and world.pills[g.target_id]
  if p and p.mx == g.mx and p.my == g.my then return p end
  return PC.pill_on_square(world, g.mx, g.my)
end

-- Is a cover square still cover?
local function cover_still(world, u)
  return M.cover_at(world, u.mx, u.my) ~= false
end

-- Call once per tick, after the goal is chosen and before steering.
-- Leaves state._repair_standoff = record (or nil when not in use).
function M.update(state, world, info, now)
  local g = state.goal
  if not M.enabled() or not g or g.kind ~= "repair_pill" or not g.mx then
    state._repair_standoff = nil
    return nil
  end
  local p = target_pill(world, g)
  if not p then
    state._repair_standoff = nil
    return nil
  end
  local pkey = g.my * 256 + g.mx
  local threats = M.threats(world, g.mx, g.my)
  local sig = M.signature(pkey, threats)
  local rec = state._repair_standoff
  local reason = nil
  -- The builder is out: keep the square, so the tank waits where it sent
  -- the builder from (and comes back to the same place for it).
  local out = info.man_status ~= nil and info.man_status ~= C.LGM_INTANK
  if not rec or rec.pkey ~= pkey then reason = "new_goal"
  elseif out then reason = nil
  elseif rec.sig ~= sig then reason = "pills_changed"
  elseif now - (rec.tick or 0) >= M.REFRESH_TICKS then reason = "refresh"
  else
    for _, u in ipairs(rec.cover_used or {}) do
      if not cover_still(world, u) then
        reason = string.format("cover_lost(%d,%d)", u.mx, u.my)
        break
      end
    end
  end
  if reason then
    local tmx = bit.rshift(info.tankx, 8)
    local tmy = bit.rshift(info.tanky, 8)
    local nrec, ctx = M.pick(world, g.mx, g.my, tmx, tmy, threats)
    nrec.pkey, nrec.sig, nrec.tick = pkey, sig, now
    nrec.threats = threats
    nrec.cover_used = used_list(ctx)
    nrec.ctx = ctx
    state._repair_standoff = nrec
    rec = nrec
  end
  return rec
end

-- The standoff record for this goal when one is in use (mode "standoff").
function M.active(state, g)
  local rec = state and state._repair_standoff
  if not (M.enabled() and rec and g and g.kind == "repair_pill" and g.mx) then return nil end
  if rec.pkey ~= g.my * 256 + g.mx or rec.mode ~= "standoff" then return nil end
  return rec
end

-- Where the tank drives for a repair_pill goal: the standoff, or nil (the pill).
function M.nav_target(state, g)
  local rec = M.active(state, g)
  if rec then return rec.mx, rec.my end
  return nil
end

-- Is the tank on the standoff square?
function M.at_standoff(state, g, tmx, tmy)
  local rec = M.active(state, g)
  return rec ~= nil and rec.mx == tmx and rec.my == tmy
end

-- ── Viz ──────────────────────────────────────────────────────────────────

function M.draw(state, world, info, rec)
  if not rec or not viz.is_on or not viz.is_on("repair_pill_viz") then return end
  local id = "repair_pill_viz"
  local sd = M.safe_dist()
  for _, t in ipairs(rec.threats or {}) do
    local r, gg, b = 255, 150, 0
    if t.heated then r, gg, b = 255, 40, 40 end
  end
  if rec.mode == "standoff" then
    for _, u in ipairs(rec.cover_used or {}) do
    end
  else
  end
  -- The tank square: safe, or which pill makes it unsafe and why.
  local ctx = rec.ctx
  if ctx then
    local tmx = bit.rshift(info.tankx, 8)
    local tmy = bit.rshift(info.tanky, 8)
    local ok, t, why = M.square_safe(ctx, tmx, tmy)
    local txt = ok and "here: safe"
      or string.format("here: unsafe #%s %s", tostring(t and t.id), tostring(why))
  end
end

return M
