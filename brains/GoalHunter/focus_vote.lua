local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/focus_vote.lua -- the focused spike deny plan and its team vote
-- (C.FOCUS_SPIKE_RULE = "deny"; see the FOCUS_SPIKE_* block in constants.lua).
--
-- With no passed vote, play is KEEL: nothing here writes pill roles, goals or
-- shares. A passed vote (a majority of the team's bots, humans do not vote)
-- turns on, for the whole team and only for the plan's tiles: the long send
-- to each assigned tile, utility share to aggro, the win shares and spike
-- care.
--
--   plan()     the deny plan, from this bot's own world model only:
--              clusters of bases (single link, CLUSTER_GAP), the greedy cover
--              tiles per cluster, a defense score per cluster (known hostile
--              pills, recently seen enemy tanks, the route from our side and
--              its detour), and the whole clusters that fit the spare pills.
--   update()   once per tick from spike.update: sightings, inbound verbs,
--              the vote (open, ballot, result, go), the active phase (placed
--              tiles, abort, end) and the team cooldown.
--   my_tiles() the active plan tiles assigned to this bot, not yet placed.
--   active()   true while a passed plan runs (team-wide).
--
-- Wire verbs (internal channel, through state._repo_outbox and init.lua's
-- keep-and-retry drain; comms.lua stashes inbound ones on state._fs_rx):
--   /info fso <id> <need> <planned> <total> <label s|d|h> <XXYY tiles...>
--   /info fsy <id> <pledge> <eta>        yes, with the pills this bot brings
--   /info fsn <id> <reason>              no: not_target, tile_covered,
--                                        home_attacked, better, cooldown,
--                                        defended
--   /info fsr <id> <0|1> <reason>        the proposer's result
--   /info fsg <id> <XXYYPP...>           go: tile -> player (hex)
--   /info fsa <id> <reason>              abort
--
-- State lives on state._fs. Required by spike.lua (field M.vote), so goals.lua
-- and init.lua get no new local or upvalue.
-- =========================================================================
local C      = require("constants")
local U      = require("util")
local PP     = require("pill_portfolio")
local print2 = require("print2")

local M = {}

local MAX_PLAYERS = 16

local function st(state)
  local F = state._fs
  if not F then
    F = { cool_until = -1, seen = {}, route_cache = {}, last_plan_t = nil,
          plan = nil, open = nil, voting = nil, active = nil, plan_key = nil,
          clusters = nil, cl_key = nil, care_tiles = {} }
    state._fs = F
  end
  return F
end
M.st = st

local function on()
  return C.FOCUS_SPIKE_ENABLED == true and C.FOCUS_SPIKE_RULE == "deny"
         and C.PILL_PLACE_TURTLE ~= true
end
M.on = on

local function tkey(mx, my) return my * 256 + mx end
local function edist(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  return math.sqrt(dx * dx + dy * dy)
end

-- Plain stderr line for the bench (FOCUS_SPIKE_STDERR_LOG). print2 lines
-- are stripped from opt/, this one is not. With FOCUS_SPIKE_EVENT_FILE set,
-- the line also goes to <brain dir>/fs_events/<name> (a release DS drops
-- brain print() output).
local function errline(s)
  if not C.FOCUS_SPIKE_STDERR_LOG then return end
  print(s)
  local name = C.FOCUS_SPIKE_EVENT_FILE
  if type(name) == "string" and name:match("^[%w_%.%-]+$") then
    local dir = (rawget(_G, "BRAIN_DIR") or "."):gsub("[/\\]opt$", "")
    local f = io.open(dir .. "/fs_events/" .. name, "a")
    if f then
      f:write(s, "\n")
      f:close()
    end
  end
end

local function ours(o) return o == "friendly" or o == "allied" end

-- ── Team ────────────────────────────────────────────────────────────────
-- The team's bot player numbers, self included, lowest first. Humans do not
-- vote (info.player_bots clear for them).
function M.team_bots(info)
  local me = info.player_number or 0
  local mask = bit.band(bit.bor(info.allies or 0, bit.lshift(1, me)),
                        bit.bor(info.player_bots or 0, bit.lshift(1, me)))
  local out = {}
  for pn = 0, MAX_PLAYERS - 1 do
    if bit.band(mask, bit.lshift(1, pn)) ~= 0 then out[#out + 1] = pn end
  end
  return out
end

-- YES strictly more than half of the team's bots.
function M.majority(yes, nbots) return yes * 2 > nbots end

-- ── Bases and clusters ──────────────────────────────────────────────────
-- Answers: list of positioned bases { id, mx, my, owner, target }, the count
-- of target bases (every base not ours: hostile, neutral, unknown) including
-- the ones with no known position (a record still at (0,0), or a base in the
-- map mask with no record), and the count of those positionless targets.
function M.bases(world)
  local list, known = {}, {}
  local total_t, nopos = 0, 0
  local ids = {}
  for id in pairs(world.bases or {}) do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local b = world.bases[id]
    known[id] = true
    local target = not ours(b.owner)
    if target then total_t = total_t + 1 end
    if (b.mx or 0) == 0 and (b.my or 0) == 0 then
      if target then nopos = nopos + 1 end
    else
      list[#list + 1] = { id = id, mx = b.mx, my = b.my, owner = b.owner, target = target }
    end
  end
  local mask = world._bases_on_map
  if type(mask) == "number" then
    for id = 0, 31 do
      if bit.band(mask, bit.lshift(1, id)) ~= 0 and not known[id] then
        total_t = total_t + 1
        nopos = nopos + 1
      end
    end
  end
  return list, total_t, nopos
end

-- Single-link clusters: two bases are in one cluster when a chain of bases
-- each within gap tiles (euclid) of the next joins them. Answers a list of
-- clusters { id, bases = {...} }, in order of their lowest base id.
function M.clusters(list, gap)
  local n = #list
  local parent = {}
  for i = 1, n do parent[i] = i end
  local function find(i)
    while parent[i] ~= i do parent[i] = parent[parent[i]]; i = parent[i] end
    return i
  end
  for i = 1, n do
    for j = i + 1, n do
      if edist(list[i].mx, list[i].my, list[j].mx, list[j].my) <= gap then
        local a, b = find(i), find(j)
        if a ~= b then parent[math.max(a, b)] = math.min(a, b) end
      end
    end
  end
  local by_root, out = {}, {}
  for i = 1, n do
    local r = find(i)
    local cl = by_root[r]
    if not cl then
      cl = { bases = {} }
      by_root[r] = cl
      out[#out + 1] = cl
    end
    cl.bases[#cl.bases + 1] = list[i]
  end
  for k, cl in ipairs(out) do
    cl.id = k
    local sx, sy = 0, 0
    for _, b in ipairs(cl.bases) do sx = sx + b.mx; sy = sy + b.my end
    cl.cx, cl.cy = sx / #cl.bases, sy / #cl.bases
  end
  return out
end

-- ── Pills ───────────────────────────────────────────────────────────────
-- Known live deployed pills of one side, from this bot's world model.
local function pills_of(world, want_hostile)
  local out = {}
  local ids = {}
  for id in pairs(world.pills or {}) do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local p = world.pills[id]
    if not p.in_tank and p.mx and (p.health or 0) > 0 then
      local h = p.owner == "hostile"
      if (want_hostile and h) or (not want_hostile and p.owner == "friendly") then
        out[#out + 1] = { id = id, mx = p.mx, my = p.my, armour = p.health or 0 }
      end
    end
  end
  return out
end
M.pills_of = pills_of

local function covered_by(pills, mx, my, r)
  for _, p in ipairs(pills) do
    if edist(mx, my, p.mx, p.my) <= r then return p end
  end
  return nil
end

-- ── Cover greedy ────────────────────────────────────────────────────────
-- Tiles so EVERY target base of the cluster is within PILL_FIRE_RANGE
-- (euclid, inclusive) of one of our pills. Our live pills count first. A
-- tile must be placeable land, not forest, and outside the range of every
-- known live hostile pill. Each step takes the tile that covers the most
-- bases still uncovered (ties: nearer to the anchor, then lower tile id).
-- Answers the tile list (maybe empty: already covered) or nil when some
-- target base cannot be covered.
function M.cover(world, targets, hpills, opills, ax, ay)
  local R = C.PILL_FIRE_RANGE
  local left = {}
  for _, b in ipairs(targets) do
    if not covered_by(opills, b.mx, b.my, R) then left[#left + 1] = b end
  end
  local cand, ck = {}, {}
  local Ri = math.floor(R)
  for _, b in ipairs(left) do
    for y = b.my - Ri, b.my + Ri do
      for x = b.mx - Ri, b.mx + Ri do
        local k = tkey(x, y)
        if not ck[k] and U.in_map(x, y) and edist(x, y, b.mx, b.my) <= R then
          ck[k] = true
          if U.is_placeable_peek(x, y, world) and U.ttype_peek(x, y) ~= C.T_FOREST
             and not covered_by(hpills, x, y, R) then
            cand[#cand + 1] = { mx = x, my = y, k = k,
                                da = ax and edist(x, y, ax, ay) or 0 }
          end
        end
      end
    end
  end
  local tiles = {}
  while #left > 0 do
    local best, bn = nil, 0
    for _, c in ipairs(cand) do
      if not c.used then
        local n = 0
        for _, b in ipairs(left) do
          if edist(c.mx, c.my, b.mx, b.my) <= R then n = n + 1 end
        end
        if n > 0 and (n > bn or (n == bn and (c.da < best.da
                      or (c.da == best.da and c.k < best.k)))) then
          best, bn = c, n
        end
      end
    end
    if not best then return nil end
    best.used = true
    tiles[#tiles + 1] = { mx = best.mx, my = best.my }
    local keep = {}
    for _, b in ipairs(left) do
      if edist(best.mx, best.my, b.mx, b.my) > R then keep[#keep + 1] = b end
    end
    left = keep
  end
  return tiles
end

-- ── Defense ─────────────────────────────────────────────────────────────
-- Engine rule (src/bolo/pillbox.c, utilIsTankInTrees): a pill does not see a
-- tank that sits in forest (its tile and the forest tiles next to it, with no
-- pill or base) while the pill is tree_hide_distance or more away on either
-- axis (classic 768 world units = 3 tiles), unless the tank just fired.
-- Tile model: the tile is forest and one of its four 2x2 blocks is all
-- forest with no pill or base, and max(|dx|, |dy|) x 256 >= the distance.
local function forest_block(world, x, y)
  local function f(tx, ty)
    if not U.in_map(tx, ty) or U.ttype_peek(tx, ty) ~= C.T_FOREST then return false end
    local k = ty * C.MAP_W + tx
    if world.pill_at and world.pill_at[k] then return false end
    if world.base_at and world.base_at[k] then return false end
    return true
  end
  if not f(x, y) then return false end
  for _, d in ipairs({ { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } }) do
    if f(x + d[1], y) and f(x, y + d[2]) and f(x + d[1], y + d[2]) then return true end
  end
  return false
end
M.forest_block = forest_block

local function hide_tiles(info)
  local wu = (info and info.rules and info.rules.tree_hide_distance) or 768
  return wu / 256
end

-- Tank passable for the route (a tank route; the builder walks the last
-- tiles). Live hostile pill tiles are not passable.
local function passable(world, x, y)
  local tt = U.ttype_peek(x, y)
  if tt == C.T_BUILDING or tt == C.T_HALFBUILD or tt == C.T_DEEPSEA
     or tt == C.T_PILLBOX then
    return false
  end
  if world.pill_at and world.pill_at[y * C.MAP_W + x] then return false end
  return true
end

-- Small binary heap for the route search.
local function hpush(h, n, d)
  h[#h + 1] = { n = n, d = d }
  local i = #h
  while i > 1 do
    local p = math.floor(i / 2)
    if h[p].d <= h[i].d then break end
    h[p], h[i] = h[i], h[p]
    i = p
  end
end
local function hpop(h)
  local top = h[1]
  local last = table.remove(h)
  if #h > 0 then
    h[1] = last
    local i = 1
    while true do
      local l, r, s = i * 2, i * 2 + 1, i
      if l <= #h and h[l].d < h[s].d then s = l end
      if r <= #h and h[r].d < h[s].d then s = r end
      if s == i then break end
      h[i], h[s] = h[s], h[i]
      i = s
    end
  end
  return top
end

-- Lowest-cost route from (sx,sy) to the nearest of the goal tiles, inside the
-- box around them padded by FOCUS_SPIKE_ROUTE_PAD. A tile exposed to a known
-- live hostile pill costs FOCUS_SPIKE_ROUTE_EXPOSED_COST more. Answers the
-- path (list of tiles, start first) and its length (diagonal steps 1.414),
-- or nil when no route is found.
function M.route(world, info, sx, sy, goals_t, hpills)
  local R = C.PILL_FIRE_RANGE
  local HD = hide_tiles(info)
  local pad = C.FOCUS_SPIKE_ROUTE_PAD
  local x0, x1, y0, y1 = sx, sx, sy, sy
  local goal = {}
  for _, t in ipairs(goals_t) do
    x0, x1 = math.min(x0, t.mx), math.max(x1, t.mx)
    y0, y1 = math.min(y0, t.my), math.max(y1, t.my)
    goal[tkey(t.mx, t.my)] = true
  end
  x0, x1 = U.mclamp(x0 - pad), U.mclamp(x1 + pad)
  y0, y1 = U.mclamp(y0 - pad), U.mclamp(y1 + pad)
  -- Exposure per tile, computed once per tile on first use.
  local expo = {}
  local function exposed(x, y)
    local k = tkey(x, y)
    local e = expo[k]
    if e == nil then
      e = false
      local fb = nil
      for _, p in ipairs(hpills) do
        if edist(x, y, p.mx, p.my) <= R then
          if fb == nil then fb = forest_block(world, x, y) end
          local far = math.max(math.abs(x - p.mx), math.abs(y - p.my)) >= HD
          if not (fb and far) then e = true; break end
        end
      end
      expo[k] = e
    end
    return e
  end
  local XC = C.FOCUS_SPIKE_ROUTE_EXPOSED_COST
  local dist, from, len = {}, {}, {}
  local h = {}
  local sk = tkey(sx, sy)
  dist[sk], len[sk] = 0, 0
  hpush(h, sk, 0)
  local found = nil
  local SQ2 = math.sqrt(2)
  while #h > 0 do
    local top = hpop(h)
    local k, d = top.n, top.d
    if d <= dist[k] then
      if goal[k] then found = k; break end
      local x, y = k % 256, math.floor(k / 256)
      for dy = -1, 1 do
        for dx = -1, 1 do
          if dx ~= 0 or dy ~= 0 then
            local nx, ny = x + dx, y + dy
            if nx >= x0 and nx <= x1 and ny >= y0 and ny <= y1 then
              local nk = tkey(nx, ny)
              if goal[nk] or passable(world, nx, ny) then
                local step = (dx ~= 0 and dy ~= 0) and SQ2 or 1
                local nd = d + step + (exposed(nx, ny) and XC or 0)
                if dist[nk] == nil or nd < dist[nk] then
                  dist[nk], from[nk], len[nk] = nd, k, len[k] + step
                  hpush(h, nk, nd)
                end
              end
            end
          end
        end
      end
    end
  end
  if not found then return nil end
  local path = {}
  local k = found
  while k do
    table.insert(path, 1, { mx = k % 256, my = math.floor(k / 256) })
    k = from[k]
  end
  return path, len[found]
end

-- Route start: our nearest owned base or our nearest live pill to the
-- cluster centre, whichever is nearer. nil when we have neither.
local function route_start(world, opills, cx, cy)
  local bx, by, bd = nil, nil, math.huge
  for _, b in pairs(world.bases or {}) do
    if ours(b.owner) and not ((b.mx or 0) == 0 and (b.my or 0) == 0) then
      local d = edist(b.mx, b.my, cx, cy)
      if d < bd or (d == bd and tkey(b.mx, b.my) < tkey(bx, by)) then bx, by, bd = b.mx, b.my, d end
    end
  end
  for _, p in ipairs(opills) do
    local d = edist(p.mx, p.my, cx, cy)
    if d < bd then bx, by, bd = p.mx, p.my, d end
  end
  return bx, by
end

-- Defense score of one cluster (with its cover tiles), from this bot's view.
-- Answers a table: total, pills (cluster term), tanks, route, detour, label
-- ("shallow" / "deep" / "hidden"), plen, straight, n_route_pills, start.
-- Cached per cluster: the route is searched again only when the start, the
-- tiles or the known hostile pills change (or after ROUTE_CACHE_TICKS).
function M.defense(state, world, info, cl, tiles, hpills, opills, now)
  local F = st(state)
  local R = C.PILL_FIRE_RANGE
  local pts = {}
  for _, b in ipairs(cl.targets or cl.bases) do pts[#pts + 1] = b end
  for _, t in ipairs(tiles) do pts[#pts + 1] = t end
  local d = { total = 0, pills = 0, tanks = 0, route = 0, detour = 0,
              label = "shallow", plen = 0, straight = 0, n_route_pills = 0 }
  -- Route.
  local sx, sy = route_start(world, opills, cl.cx, cl.cy)
  local route_pills = {}
  if sx and #tiles > 0 then
    local hsig = {}
    -- Positions only: armour changes the weight, not the route.
    for _, p in ipairs(hpills) do hsig[#hsig + 1] = string.format("%d,%d", p.mx, p.my) end
    local tsig = {}
    for _, t in ipairs(tiles) do tsig[#tsig + 1] = tkey(t.mx, t.my) end
    local key = string.format("%d,%d|%s|%s", sx, sy, table.concat(tsig, ","), table.concat(hsig, ";"))
    local ce = F.route_cache[cl.key or cl.id]
    if not ce or ce.key ~= key or (now - ce.t) > C.FOCUS_SPIKE_ROUTE_CACHE_TICKS then
      local path, plen = M.route(world, info, sx, sy, tiles, hpills)
      ce = { key = key, t = now, path = path, plen = plen }
      F.route_cache[cl.key or cl.id] = ce
    end
    d.start = { mx = sx, my = sy }
    if not ce.path then
      d.label, d.blocked = "deep", true
      d.route = C.FOCUS_SPIKE_DEFENSE_MAX
    else
      local HD = hide_tiles(info)
      local any_exposed, any_hidden = false, false
      for _, t in ipairs(ce.path) do
        local fb = nil
        for _, p in ipairs(hpills) do
          if edist(t.mx, t.my, p.mx, p.my) <= R then
            if fb == nil then fb = forest_block(world, t.mx, t.my) end
            local far = math.max(math.abs(t.mx - p.mx), math.abs(t.my - p.my)) >= HD
            if fb and far then
              any_hidden = true
            else
              any_exposed = true
              route_pills[p.id] = p
            end
          end
        end
      end
      for _, p in pairs(route_pills) do
        d.route = d.route + (p.armour / 15) * C.FOCUS_SPIKE_DEFENSE_ROUTE_WEIGHT
        d.n_route_pills = d.n_route_pills + 1
      end
      d.label = any_exposed and "deep" or (any_hidden and "hidden" or "shallow")
      local last = ce.path[#ce.path]
      d.plen = ce.plen
      d.straight = edist(sx, sy, last.mx, last.my)
      d.detour = math.floor(math.max(0, d.plen - d.straight) / 10)
                 * C.FOCUS_SPIKE_DEFENSE_DETOUR_WEIGHT
    end
  end
  -- Cluster pills: each known hostile pill near a target base or a cover tile
  -- (each pill once, and not again when the route already counts it).
  local PR = R + C.FOCUS_SPIKE_DEFENSE_PILL_MARGIN
  for _, p in ipairs(hpills) do
    if not route_pills[p.id] then
      for _, q in ipairs(pts) do
        if edist(p.mx, p.my, q.mx, q.my) <= PR then
          d.pills = d.pills + p.armour / 15
          break
        end
      end
    end
  end
  -- Enemy tanks seen near the cluster lately.
  local TR = C.FOCUS_SPIKE_DEFENSE_TANK_RANGE
  for _, s in pairs(F.seen) do
    if (now - s.t) <= C.FOCUS_SPIKE_DEFENSE_TANK_MEMORY then
      for _, q in ipairs(pts) do
        if edist(s.mx, s.my, q.mx, q.my) <= TR then
          d.tanks = d.tanks + C.FOCUS_SPIKE_DEFENSE_TANK_WEIGHT
          break
        end
      end
    end
  end
  d.total = d.pills + d.tanks + d.route + d.detour
  return d
end

local LABEL_TEXT = { shallow = "shallow", deep = "deep", hidden = "deep, hidden route" }
M.LABEL_TEXT = LABEL_TEXT
local LABEL_CODE = { shallow = "s", deep = "d", hidden = "h" }
local CODE_LABEL = { s = "shallow", d = "deep", h = "hidden" }

-- Rank order of two clusters (a before b?). Undefended first (defense below
-- UNDEFENDED_MAX, with FOCUS_SPIKE_UNDEFENDED_FIRST), then shallow before
-- deep when the ranks are within 10%, then the higher rank, then nearer to
-- our side (distance to our nearest base), then the lower cluster id.
local function before(a, b)
  if C.FOCUS_SPIKE_UNDEFENDED_FIRST then
    local ua = a.def.total < C.FOCUS_SPIKE_UNDEFENDED_MAX
    local ub = b.def.total < C.FOCUS_SPIKE_UNDEFENDED_MAX
    if ua ~= ub then return ua end
  end
  local hi = math.max(a.rank, b.rank)
  local close = hi <= 0 or math.abs(a.rank - b.rank) <= 0.1 * hi
  local sa, sb = a.def.label == "shallow", b.def.label == "shallow"
  if close and sa ~= sb then return sa end
  if a.rank ~= b.rank then return a.rank > b.rank end
  if a.home ~= b.home then return a.home < b.home end
  return a.id < b.id
end
M.before = before

-- ── The deny plan ───────────────────────────────────────────────────────
-- Answers a plan table:
--   pass, why ("ok", "no_targets", "no_spare", "frac", "nothing_to_place"),
--   total (target bases), planned (target bases in the chosen clusters),
--   frac, K (spare pills), need (tiles to place), tiles, chosen, rows (every
--   cluster with its numbers and why it was taken or skipped), label.
-- Peek reads only: never writes pill roles.
function M.plan(state, world, info, now)
  local list, total, nopos = M.bases(world)
  local hp = pills_of(world, true)
  local op = pills_of(world, false)
  local pf = PP.counts(world, now, true)
  local spike = require("spike")
  local team = spike.team_pills(world, info, pf)
  local ftgt = PP.normal_targets(team).front
  local K = team - ftgt
  -- Clusters: bases never move, so the list is rebuilt only when the set of
  -- positioned bases changes.
  local F = st(state)
  local ck = {}
  for _, b in ipairs(list) do ck[#ck + 1] = string.format("%d:%d,%d", b.id, b.mx, b.my) end
  ck = table.concat(ck, ";")
  if F.cl_key ~= ck then
    F.cl_key = ck
    F.clusters = M.clusters(list, C.FOCUS_SPIKE_CLUSTER_GAP)
    F.route_cache = {}
  end
  -- Our nearest base (for the tie-break).
  local obases = {}
  for _, b in ipairs(list) do if not b.target then obases[#obases + 1] = b end end
  local rows = {}
  -- The clusters keep the base list of the plan that built them, so read
  -- each base's owner from this plan's list (owners change, places do not).
  local cur = {}
  for _, b in ipairs(list) do cur[b.id] = b end
  for _, cl0 in ipairs(F.clusters) do
    local targets = {}
    for _, b0 in ipairs(cl0.bases) do
      local b = cur[b0.id]
      if b and b.target then targets[#targets + 1] = b end
    end
    if #targets > 0 then
      local key = tostring(cl0.bases[1].id)
      local cl = { id = cl0.id, key = key, bases = cl0.bases, targets = targets,
                   cx = cl0.cx, cy = cl0.cy }
      local home = math.huge
      for _, b in ipairs(obases) do home = math.min(home, edist(b.mx, b.my, cl.cx, cl.cy)) end
      local ax, ay = nil, nil
      if #obases > 0 then
        local best = math.huge
        for _, b in ipairs(obases) do
          local dd = edist(b.mx, b.my, cl.cx, cl.cy)
          if dd < best then best, ax, ay = dd, b.mx, b.my end
        end
      end
      local tiles = M.cover(world, targets, hp, op, ax, ay)
      local row = { id = cl.id, key = key, nb = #targets, nall = #cl.bases,
                    home = home, cl = cl }
      if not tiles then
        row.skip = "uncoverable"
        row.need = -1
        row.def = { total = 0, label = "shallow" }
        row.rank = 0
      else
        row.tiles = tiles
        row.need = #tiles
        row.def = M.defense(state, world, info, cl, tiles, hp, op, now)
        row.rank = #targets / (row.need + C.FOCUS_SPIKE_DEFENSE_WEIGHT * row.def.total)
        if row.def.total >= C.FOCUS_SPIKE_DEFENSE_MAX then row.skip = "defended" end
      end
      rows[#rows + 1] = row
    end
  end
  -- Insertion sort (the order rule is not a strict weak order for
  -- table.sort, and there are only a few clusters).
  local order = {}
  for _, r in ipairs(rows) do
    if not r.skip then
      local i = #order + 1
      while i > 1 and before(r, order[i - 1]) do i = i - 1 end
      table.insert(order, i, r)
    end
  end
  local left = K
  local chosen, tiles, planned, need = {}, {}, 0, 0
  for _, r in ipairs(order) do
    if r.need <= left and #tiles + r.need <= C.FOCUS_SPIKE_MAX_TILES then
      r.taken = true
      left = left - r.need
      chosen[#chosen + 1] = r
      planned = planned + r.nb
      need = need + r.need
      for _, t in ipairs(r.tiles) do tiles[#tiles + 1] = { mx = t.mx, my = t.my, cl = r.id } end
    else
      r.skip = (r.need > left) and "no_spare" or "max_tiles"
    end
  end
  local frac = total > 0 and planned / total or 0
  local why = "ok"
  if total == 0 then why = "no_targets"
  elseif K <= 0 then why = "no_spare"
  elseif frac < C.FOCUS_SPIKE_DENY_FRAC then why = "frac"
  elseif need == 0 then why = "nothing_to_place" end
  -- The plan's label: the worst route of its clusters.
  local label = "shallow"
  for _, r in ipairs(chosen) do
    if r.def.label == "deep" then label = "deep"
    elseif r.def.label == "hidden" and label ~= "deep" then label = "hidden" end
  end
  return { pass = why == "ok", why = why, total = total, nopos = nopos,
           planned = planned, frac = frac, K = K, team = team, ftgt = ftgt,
           need = need, tiles = tiles, chosen = chosen, rows = rows,
           label = label, t = now }
end

local function plan_line(now, P)
  local parts = {}
  for _, r in ipairs(P.rows) do
    local d = r.def
    parts[#parts + 1] = string.format(
      "c%d[%d target of %d bases, need %d, defense %.2f = pills %.2f + tanks %.2f + route %.2f (%d pills) + detour %.2f (path %.1f vs straight %.1f), %s, rank %.3f = %d / (%d + %.1f x %.2f) -> %s]",
      r.id, r.nb, r.nall, r.need, d.total, d.pills or 0, d.tanks or 0, d.route or 0,
      d.n_route_pills or 0, d.detour or 0, d.plen or 0, d.straight or 0,
      LABEL_TEXT[d.label] or d.label, r.rank, r.nb, math.max(0, r.need),
      C.FOCUS_SPIKE_DEFENSE_WEIGHT, d.total,
      r.taken and "taken" or ("skip " .. tostring(r.skip)))
  end
  return string.format(
    "FOCUS_SPIKE_PLAN t=%d result=%s why=%s planned %d / %d target bases = %.2f (need >= %.2f; %d have no known position) spare K = team pills %d - normal front target %d = %d need %d tiles label=%s clusters: %s",
    now, P.pass and "pass" or "fail", P.why, P.planned, P.total, P.frac,
    C.FOCUS_SPIKE_DENY_FRAC, P.nopos, P.team, P.ftgt, P.K, P.need,
    LABEL_TEXT[P.label], table.concat(parts, " "))
end
M.plan_line = plan_line

-- ── Encoding ────────────────────────────────────────────────────────────
local function enc_tiles(tiles)
  local s = {}
  for _, t in ipairs(tiles) do s[#s + 1] = string.format("%02X%02X", t.mx, t.my) end
  return table.concat(s)
end
local function dec_tiles(s)
  local out = {}
  for i = 1, #s - 3, 4 do
    local x = tonumber(s:sub(i, i + 1), 16)
    local y = tonumber(s:sub(i + 2, i + 3), 16)
    if x and y then out[#out + 1] = { mx = x, my = y } end
  end
  return out
end
M.enc_tiles, M.dec_tiles = enc_tiles, dec_tiles

local function send(state, msg)
  state._repo_outbox = state._repo_outbox or {}
  state._repo_outbox[#state._repo_outbox + 1] = msg
end

-- Parse one inbound verb (comms.lua stashes the raw text on state._fs_rx).
function M.parse(text)
  local id, need, planned, total, lab, tl = text:match("^/info fso (%d+) (%d+) (%d+) (%d+) (%a) (%x*)$")
  if id then
    return { verb = "fso", id = tonumber(id), need = tonumber(need), planned = tonumber(planned),
             total = tonumber(total), label = CODE_LABEL[lab] or "deep", tiles = dec_tiles(tl) }
  end
  local yid, pl, eta = text:match("^/info fsy (%d+) (%d+) (%d+)$")
  if yid then return { verb = "fsy", id = tonumber(yid), pledge = tonumber(pl), eta = tonumber(eta) } end
  local nid, why = text:match("^/info fsn (%d+) ([%a_]+)$")
  if nid then return { verb = "fsn", id = tonumber(nid), reason = why } end
  local rid, ok, rwhy = text:match("^/info fsr (%d+) ([01]) ([%a_]+)$")
  if rid then return { verb = "fsr", id = tonumber(rid), pass = ok == "1", reason = rwhy } end
  local gid, gl = text:match("^/info fsg (%d+) (%x*)$")
  if gid then
    local out = {}
    for i = 1, #gl - 5, 6 do
      local x = tonumber(gl:sub(i, i + 1), 16)
      local y = tonumber(gl:sub(i + 2, i + 3), 16)
      local pn = tonumber(gl:sub(i + 4, i + 5), 16)
      if x and y and pn then out[#out + 1] = { mx = x, my = y, pn = pn } end
    end
    return { verb = "fsg", id = tonumber(gid), tiles = out }
  end
  local aid, awhy = text:match("^/info fsa (%d+) ([%a_]+)$")
  if aid then return { verb = "fsa", id = tonumber(aid), reason = awhy } end
  return nil
end

-- ── Views a ballot or an abort reads (this bot's own world model) ────────
-- An enemy tank within HOME_ATTACK_RANGE of one of our bases (seen now).
function M.home_attacked(state, world)
  local et = state.perc and state.perc.enemy_tanks
  if not et then return false end
  local R = C.FOCUS_SPIKE_HOME_ATTACK_RANGE
  for _, b in pairs(world.bases or {}) do
    if ours(b.owner) and not ((b.mx or 0) == 0 and (b.my or 0) == 0) then
      for _, t in ipairs(et) do
        if t.mx and edist(t.mx, t.my, b.mx, b.my) <= R then return true end
      end
    end
  end
  return false
end

-- The abort test while a plan is active (FOCUS_SPIKE_HOME_ABORT_RULE):
-- "base_lost" = a base that was ours when the plan went is ours no more.
-- The bases are noted on the first check after the go. "tank_near" = the
-- ballot test (an enemy tank near one of our bases), which in bench games
-- ended every plan a few seconds after its go.
function M.home_lost(state, world, A)
  if C.FOCUS_SPIKE_HOME_ABORT_RULE == "tank_near" then return M.home_attacked(state, world) end
  if not A.home then
    A.home = {}
    for id, b in pairs(world.bases or {}) do
      if ours(b.owner) then A.home[id] = true end
    end
    return false
  end
  for id in pairs(A.home) do
    local b = world.bases and world.bases[id]
    if not b or not ours(b.owner) then return true end
  end
  return false
end

-- A known live hostile pill within PILL_FIRE_RANGE of a tile in the list.
function M.tile_covered(world, tiles)
  local hp = pills_of(world, true)
  for _, t in ipairs(tiles) do
    if not t.placed and covered_by(hp, t.mx, t.my, C.PILL_FIRE_RANGE) then return t end
  end
  return nil
end

-- not_target: this bot sees bases within range of the plan tiles and every
-- one of them is ours. A tile with no base it knows near it is not proof.
function M.not_target(world, tiles)
  local R = C.PILL_FIRE_RANGE
  local seen, theirs = 0, 0
  for _, b in pairs(world.bases or {}) do
    if not ((b.mx or 0) == 0 and (b.my or 0) == 0) then
      for _, t in ipairs(tiles) do
        if edist(b.mx, b.my, t.mx, t.my) <= R then
          seen = seen + 1
          if not ours(b.owner) then theirs = theirs + 1 end
          break
        end
      end
    end
  end
  return seen > 0 and theirs == 0
end

-- This bot's own defense number for the plan tiles: one pseudo cluster of
-- the tiles and the bases near them.
function M.tiles_defense(state, world, info, tiles, now, key)
  local R = C.PILL_FIRE_RANGE
  local bases = {}
  local sx, sy = 0, 0
  for _, t in ipairs(tiles) do sx = sx + t.mx; sy = sy + t.my end
  for _, b in pairs(world.bases or {}) do
    if not ours(b.owner) and not ((b.mx or 0) == 0 and (b.my or 0) == 0) then
      for _, t in ipairs(tiles) do
        if edist(b.mx, b.my, t.mx, t.my) <= R then bases[#bases + 1] = b; break end
      end
    end
  end
  local cl = { id = 0, key = key or "vote", bases = bases, targets = bases,
               cx = sx / math.max(1, #tiles), cy = sy / math.max(1, #tiles) }
  local pend = {}
  for _, t in ipairs(tiles) do if not t.placed then pend[#pend + 1] = t end end
  return M.defense(state, world, info, cl, pend, pills_of(world, true), pills_of(world, false), now)
end

local function tank_tile(info)
  return bit.rshift(info.tankx or 0, 8), bit.rshift(info.tanky or 0, 8)
end

local function eta_to(info, tiles)
  local tx, ty = tank_tile(info)
  local best = math.huge
  for _, t in ipairs(tiles) do best = math.min(best, edist(tx, ty, t.mx, t.my)) end
  if best == math.huge then return 255 end
  return math.min(9999, math.floor(best))
end

-- This bot's ballot on a proposal: "yes", pledge, eta; or "no", reason.
function M.ballot(state, world, info, prop, now)
  local F = st(state)
  if F.active or now < F.cool_until then return "no", "cooldown" end
  if M.home_attacked(state, world) then return "no", "home_attacked" end
  if M.tile_covered(world, prop.tiles) then return "no", "tile_covered" end
  if M.not_target(world, prop.tiles) then return "no", "not_target" end
  local d = M.tiles_defense(state, world, info, prop.tiles, now, "vote")
  if d.total >= C.FOCUS_SPIKE_DEFENSE_MAX then return "no", "defended", d end
  local mine = M.plan(state, world, info, now)
  F.plan = mine
  if mine.pass and prop.total > 0
     and mine.frac > (prop.planned / prop.total) + C.FOCUS_SPIKE_BETTER_MARGIN then
    return "no", "better"
  end
  return "yes", math.max(0, info.carried_pills or 0), eta_to(info, prop.tiles)
end

-- Assignments: candidates nearest ETA first (ties: lower player number) take
-- tiles in plan order, up to their pledge. Answers the tile list with pn set,
-- or nil when the pledges do not cover every tile.
function M.assign(tiles, cands)
  table.sort(cands, function(a, b)
    if a.eta ~= b.eta then return a.eta < b.eta end
    return a.pn < b.pn
  end)
  local out, i = {}, 1
  for _, c in ipairs(cands) do
    for _ = 1, c.pledge do
      if i > #tiles then break end
      out[i] = { mx = tiles[i].mx, my = tiles[i].my, pn = c.pn }
      i = i + 1
    end
  end
  if i <= #tiles then return nil end
  return out
end

-- The vote result: YES strictly more than half the team bots (silence =
-- yes, the proposer is a yes) and the pledges (proposer + YES ballots)
-- cover the need. Answers pass, reason, yes count, pledged.
function M.resolve(open, nbots)
  local yes, no, pledged = 1, 0, open.pledge
  local reasons = {}
  for _, pn in ipairs(open.bots) do
    if pn ~= open.me then
      local b = open.ballots[pn]
      if b and not b.yes then
        no = no + 1
        reasons[#reasons + 1] = b.reason
      else
        yes = yes + 1
        if b then pledged = pledged + (b.pledge or 0) end
      end
    end
  end
  local only_better = no > 0
  for _, r in ipairs(reasons) do if r ~= "better" then only_better = false end end
  if not M.majority(yes, nbots) then
    return false, only_better and "better" or "majority", yes, pledged, reasons
  end
  if pledged < open.need then return false, "pledge", yes, pledged, reasons end
  return true, "ok", yes, pledged, reasons
end

-- ── Active phase ────────────────────────────────────────────────────────
local function start_active(state, F, id, tiles, now, from, info)
  F.active = { id = id, tiles = tiles, t_go = now, from = from }
  F.open, F.voting = nil, nil
  local mine = 0
  for _, t in ipairs(tiles) do if t.pn == info.player_number then mine = mine + 1 end end
  print2(string.format("FOCUS_SPIKE_ACTIVE t=%d on id=%d from p%d tiles=%d mine=%d ttl=%d",
    now, id, from, #tiles, mine, C.FOCUS_SPIKE_APPROVAL_TTL))
end

local function cooldown(F, now, why)
  F.cool_until = now + C.FOCUS_SPIKE_VOTE_FAIL_COOLDOWN
end

-- why: all_placed (each tile placed), tiles_gone (the rest can no longer be
-- placed), ttl, or abort_<why>. all_placed is split here by the placed count.
local function end_active(state, F, now, why, info)
  local A = F.active
  if not A then return end
  local placed = 0
  for _, t in ipairs(A.tiles) do if t.placed then placed = placed + 1 end end
  if why == "all_placed" and placed < #A.tiles then why = "tiles_gone" end
  print2(string.format("FOCUS_SPIKE_ACTIVE t=%d off id=%d why=%s placed %d of %d after %dt",
    now, A.id, why, placed, #A.tiles, now - A.t_go))
  errline(string.format("FOCUS_SPIKE end t=%d p%d id=%d why=%s placed=%d tiles=%d after=%d",
    now, info and info.player_number or -1, A.id, why, placed, #A.tiles, now - A.t_go))
  F.active = nil
end

local function abort(state, F, info, now, why)
  local A = F.active
  if not A then return end
  send(state, string.format("/info fsa %d %s", A.id, why))
  print2(string.format("FOCUS_SPIKE_VOTE t=%d abort id=%d why=%s by p%d", now, A.id, why, info.player_number or -1))
  errline(string.format("FOCUS_SPIKE abort t=%d p%d id=%d why=%s", now, info.player_number or -1, A.id, why))
  end_active(state, F, now, "abort_" .. why, info)
  cooldown(F, now, why)
end

function M.active(state)
  local F = state and state._fs
  return F ~= nil and F.active ~= nil
end

-- The active tiles assigned to this bot and not placed yet.
function M.my_tiles(state, info)
  local F = state and state._fs
  local A = F and F.active
  if not A then return nil end
  local out = {}
  for _, t in ipairs(A.tiles) do
    if t.pn == info.player_number and not t.placed and not t.gone then out[#out + 1] = t end
  end
  if #out == 0 then return nil end
  return out
end

-- True when (mx,my) is a tile of the active plan (or a placed one under care).
function M.plan_tile(state, mx, my)
  local F = state and state._fs
  if not F then return false end
  local A = F.active
  if A then
    for _, t in ipairs(A.tiles) do if t.mx == mx and t.my == my then return true end end
  end
  return false
end

-- init.lua PLACE_TRIP_SET for a plan goal (spike.on_dispatch).
function M.on_dispatch(state, mx, my, now)
  local F = st(state)
  F.trip = { mx = mx, my = my, t = now, left = false }
end

-- ── Per tick ────────────────────────────────────────────────────────────
local function handle_rx(state, F, world, info, now, care_tag)
  local rx = state._fs_rx
  if not rx then return end
  state._fs_rx = nil
  local me = info.player_number
  for _, m in ipairs(rx) do
    local v = M.parse(m.text)
    if v and m.from ~= me then
      if v.verb == "fso" then
        print2(string.format("FOCUS_SPIKE_VOTE t=%d rx open id=%d from p%d need=%d planned %d/%d label=%s tiles=%d",
          now, v.id, m.from, v.need, v.planned, v.total, LABEL_TEXT[v.label], #v.tiles))
        local skip = false
        if F.open then
          if m.from < me then
            print2(string.format("FOCUS_SPIKE_VOTE t=%d drop own id=%d (p%d opened too, lower player number goes first)",
              now, F.open.id, m.from))
            F.open = nil
          else
            skip = true
          end
        end
        if not skip then
          F.voting = { id = v.id, from = m.from, t = now }
          local yn, a, b = M.ballot(state, world, info, v, now)
          if yn == "yes" then
            send(state, string.format("/info fsy %d %d %d", v.id, a, b))
          else
            send(state, string.format("/info fsn %d %s", v.id, a))
          end
          print2(string.format("FOCUS_SPIKE_VOTE t=%d ballot id=%d p%d -> %s %s%s", now, v.id, me,
            yn, tostring(a), yn == "yes" and (" eta " .. tostring(b)) or
            (b and string.format(" (defense %.2f >= %.2f)", b.total, C.FOCUS_SPIKE_DEFENSE_MAX) or "")))
        end
      elseif (v.verb == "fsy" or v.verb == "fsn") and F.open and F.open.id == v.id then
        F.open.ballots[m.from] = { yes = v.verb == "fsy", pledge = v.pledge or 0,
                                   eta = v.eta or 255, reason = v.reason }
        print2(string.format("FOCUS_SPIKE_VOTE t=%d rx ballot id=%d from p%d %s", now, v.id, m.from,
          v.verb == "fsy" and string.format("yes pledge %d eta %d", v.pledge, v.eta) or ("no " .. v.reason)))
      elseif v.verb == "fsr" then
        if F.voting and F.voting.id == v.id then F.voting = nil end
        if not v.pass and v.reason ~= "better" then cooldown(F, now, v.reason) end
        print2(string.format("FOCUS_SPIKE_VOTE t=%d rx result id=%d from p%d %s %s%s", now, v.id, m.from,
          v.pass and "pass" or "fail", v.reason,
          (not v.pass and v.reason ~= "better") and (" -> cooldown until t=" .. F.cool_until) or ""))
      elseif v.verb == "fsg" then
        if not F.active then start_active(state, F, v.id, v.tiles, now, m.from, info) end
      elseif v.verb == "fsa" then
        if F.active and F.active.id == v.id then
          print2(string.format("FOCUS_SPIKE_VOTE t=%d rx abort id=%d from p%d why=%s", now, v.id, m.from, v.reason))
          end_active(state, F, now, "abort_" .. v.reason, info)
          cooldown(F, now, v.reason)
        end
      end
    end
  end
end

local function resolve_open(state, F, info, now)
  local O = F.open
  if not O then return end
  local answered = 0
  for _, pn in ipairs(O.bots) do
    if pn ~= O.me and O.ballots[pn] then answered = answered + 1 end
  end
  if answered < #O.bots - 1 and (now - O.t) < C.FOCUS_SPIKE_VOTE_WINDOW_TICKS then return end
  local pass, why, yes, pledged, reasons = M.resolve(O, #O.bots)
  F.open = nil
  send(state, string.format("/info fsr %d %d %s", O.id, pass and 1 or 0, why))
  print2(string.format("FOCUS_SPIKE_VOTE t=%d result id=%d %s why=%s yes %d of %d bots (need > %d; silence = yes) pledged %d (proposer %d) need %d no=[%s] answered %d after %dt",
    now, O.id, pass and "pass" or "fail", why, yes, #O.bots, math.floor(#O.bots / 2),
    pledged, O.pledge, O.need, table.concat(reasons, ","), answered, now - O.t))
  errline(string.format("FOCUS_SPIKE result t=%d p%d id=%d %s why=%s yes=%d bots=%d pledged=%d need=%d label=%s no=%s",
    now, info.player_number or -1, O.id, pass and "pass" or "fail", why, yes, #O.bots,
    pledged, O.need, O.label, table.concat(reasons, ",")))
  if not pass then
    if why ~= "better" then cooldown(F, now, why) end
    return
  end
  local cands = { { pn = O.me, pledge = O.pledge, eta = O.eta } }
  for pn, b in pairs(O.ballots) do
    if b.yes and (b.pledge or 0) > 0 then cands[#cands + 1] = { pn = pn, pledge = b.pledge, eta = b.eta } end
  end
  local asg = M.assign(O.tiles, cands)
  if not asg then
    cooldown(F, now, "pledge")
    return
  end
  local s = {}
  for _, t in ipairs(asg) do s[#s + 1] = string.format("%02X%02X%02X", t.mx, t.my, t.pn) end
  send(state, string.format("/info fsg %d %s", O.id, table.concat(s)))
  local al = {}
  for _, t in ipairs(asg) do al[#al + 1] = string.format("(%d,%d)->p%d", t.mx, t.my, t.pn) end
  print2(string.format("FOCUS_SPIKE_VOTE t=%d go id=%d %s", now, O.id, table.concat(al, " ")))
  errline(string.format("FOCUS_SPIKE go t=%d p%d id=%d tiles=%d label=%s %s", now,
    info.player_number or -1, O.id, #asg, O.label, table.concat(al, " ")))
  start_active(state, F, O.id, asg, now, O.me, info)
end

-- Placed tiles, the end and the abort checks of the active plan.
local function update_active(state, F, world, info, now, care_tag)
  local A = F.active
  if not A then return end
  local spike = require("spike")
  local all = true
  for _, t in ipairs(A.tiles) do
    if not t.placed and not t.gone then
      if spike.pill_alive_at(world, t.mx, t.my) then
        t.placed, t.t_placed = true, now
        care_tag(t.mx, t.my, now)
        print2(string.format("FOCUS_SPIKE_ACTIVE t=%d placed (%d,%d) by p%d id=%d %dt after go -> spike care %dt",
          now, t.mx, t.my, t.pn, A.id, now - A.t_go, C.SPIKE_CARE_TICKS))
        if t.pn == info.player_number then
          errline(string.format("FOCUS_SPIKE placed t=%d p%d id=%d tile=%d,%d", now, t.pn, A.id, t.mx, t.my))
        end
      elseif not U.is_placeable_peek(t.mx, t.my, world) then
        t.gone = true
      end
    end
    if not t.placed and not t.gone then all = false end
  end
  if all then end_active(state, F, now, "all_placed", info); return end
  if (now - A.t_go) > C.FOCUS_SPIKE_APPROVAL_TTL then end_active(state, F, now, "ttl", info); return end
  -- Abort checks, this bot's own view.
  local tr = F.trip
  if tr then
    if info.man_status ~= C.LGM_INTANK then tr.left = true end
    if info.man_status == C.LGM_DEAD and M.plan_tile(state, tr.mx, tr.my)
       and not spike.pill_alive_at(world, tr.mx, tr.my) then
      F.trip = nil
      abort(state, F, info, now, "lost_pill")
      return
    end
    if (tr.left and info.man_status == C.LGM_INTANK) or (now - tr.t) > (C.PLACE_TRIP_MAX_TICKS or 2000) then
      F.trip = nil
    end
  end
  if M.tile_covered(world, A.tiles) then abort(state, F, info, now, "tile_covered"); return end
  if C.FOCUS_SPIKE_ABORT_HOME_ATTACKED and M.home_lost(state, world, A) then
    abort(state, F, info, now, "home_attacked"); return
  end
  if (now - (A.t_route or -1e9)) >= C.FOCUS_SPIKE_PLAN_INTERVAL then
    A.t_route = now
    local d = M.tiles_defense(state, world, info, A.tiles, now, "active")
    if (d.route or 0) >= C.FOCUS_SPIKE_DEFENSE_MAX then abort(state, F, info, now, "route_blocked") end
  end
end

-- Win-then-vote: this bot can place now and the placement goal won its pool.
local function wants_to_place(state, info)
  return (info.carried_pills or 0) >= 1 and info.man_status == C.LGM_INTANK
         and not info.inboat and state.goal ~= nil
         and state.goal.kind == "place_pill_strategic"
end

function M.update(state, world, info, now, care_tag)
  if not on() then return end
  local F = st(state)
  -- Enemy tank sightings (for the defense tank term).
  local et = state.perc and state.perc.enemy_tanks
  if et then
    for _, t in ipairs(et) do
      if t.mx then F.seen[t.id or tkey(t.mx, t.my)] = { mx = t.mx, my = t.my, t = now } end
    end
  end
  for k, s in pairs(F.seen) do
    if (now - s.t) > C.FOCUS_SPIKE_DEFENSE_TANK_MEMORY then F.seen[k] = nil end
  end
  handle_rx(state, F, world, info, now, care_tag)
  resolve_open(state, F, info, now)
  update_active(state, F, world, info, now, care_tag)
  if F.voting and (now - F.voting.t) > 2 * C.FOCUS_SPIKE_VOTE_WINDOW_TICKS then F.voting = nil end
  -- The plan, about once a second.
  if F.last_plan_t and (now - F.last_plan_t) < C.FOCUS_SPIKE_PLAN_INTERVAL then return end
  F.last_plan_t = now
  if F.active or F.open or F.voting or now < F.cool_until then return end
  local P = M.plan(state, world, info, now)
  F.plan = P
  local key = string.format("%s:%s:%d:%d:%d:%s", P.why, P.label, P.planned, P.total, P.need,
                            enc_tiles(P.tiles))
  if F.plan_key ~= key then
    F.plan_key = key
    print2(plan_line(now, P))
  end
  if not P.pass or not wants_to_place(state, info) then return end
  -- Open the vote.
  local bots = M.team_bots(info)
  local me = info.player_number or 0
  local id = me * 65536 + bit.band(now, 0xFFFF)
  F.open = { id = id, t = now, me = me, bots = bots, ballots = {}, need = P.need,
             tiles = P.tiles, pledge = math.max(0, info.carried_pills or 0),
             eta = eta_to(info, P.tiles), label = P.label, planned = P.planned, total = P.total }
  send(state, string.format("/info fso %d %d %d %d %s %s", id, P.need, P.planned, P.total,
    LABEL_CODE[P.label], enc_tiles(P.tiles)))
  print2(string.format("FOCUS_SPIKE_VOTE t=%d open id=%d p%d need=%d planned %d/%d label=%s bots=%d pledge=%d tiles=%s",
    now, id, me, P.need, P.planned, P.total, LABEL_TEXT[P.label], #bots, F.open.pledge, enc_tiles(P.tiles)))
  errline(string.format("FOCUS_SPIKE open t=%d p%d id=%d need=%d planned=%d total=%d label=%s bots=%d",
    now, me, id, P.need, P.planned, P.total, LABEL_TEXT[P.label], #bots))
  -- A team of one bot resolves at once.
  if #bots <= 1 then resolve_open(state, F, info, now) end
end

return M
