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
--   /info fsu <id> <XXYY old> <XXYY new|FFFF> <bases dropped>
--                                        re-pick: a plan tile moved (same
--                                        bot) or dropped (FFFF)
--
-- FOCUS_SPIKE_RULE "cluster" (2026-11-01): the same vote, but each plan is
-- one cluster that the spare pills cover in full (plan_cluster, risk). The
-- open message then ends with the cluster key and its target bases:
--   /info fso <id> <need> <planned> <total> <label> <XXYY tiles...> <key> <BB bases...>
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

local function cluster_rule() return C.FOCUS_SPIKE_RULE == "cluster" end
M.cluster_rule = cluster_rule

local function on()
  return C.FOCUS_SPIKE_ENABLED == true
         and (C.FOCUS_SPIKE_RULE == "deny" or C.FOCUS_SPIKE_RULE == "cluster")
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
-- With FOCUS_SPIKE_NEUTRAL_PILL_COVER, the hostile list also holds the
-- neutral pills (they fire at every team).
local function pills_of(world, want_hostile)
  local out = {}
  local ids = {}
  for id in pairs(world.pills or {}) do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local p = world.pills[id]
    if not p.in_tank and p.mx and (p.health or 0) > 0 then
      local h = p.owner == "hostile"
                or (p.owner == "neutral" and C.FOCUS_SPIKE_NEUTRAL_PILL_COVER == true)
      if (want_hostile and h) or (not want_hostile and p.owner == "friendly") then
        out[#out + 1] = { id = id, mx = p.mx, my = p.my, armour = p.health or 0 }
      end
    end
  end
  return out
end
M.pills_of = pills_of

-- The pills that cover target bases in the cover greedy: ours, plus the
-- known live neutral pills when FOCUS_SPIKE_NEUTRAL_PILL_COVER is on.
-- Answers the list and the ids of the target bases of list that only a
-- neutral pill covers (sorted; the ncov= of the plan line).
function M.cover_pills(world, opills, list)
  if C.FOCUS_SPIKE_NEUTRAL_PILL_COVER ~= true then return opills, {} end
  local out = {}
  for _, p in ipairs(opills) do out[#out + 1] = p end
  local np = {}
  local ids = {}
  for id in pairs(world.pills or {}) do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local p = world.pills[id]
    if not p.in_tank and p.mx and (p.health or 0) > 0 and p.owner == "neutral" then
      local q = { id = id, mx = p.mx, my = p.my, armour = p.health or 0, neutral = true }
      np[#np + 1] = q
      out[#out + 1] = q
    end
  end
  local ncov = {}
  local R = C.PILL_FIRE_RANGE
  for _, b in ipairs(list or {}) do
    if b.target then
      local by_n, by_o = false, false
      for _, p in ipairs(np) do if edist(b.mx, b.my, p.mx, p.my) <= R then by_n = true; break end end
      for _, p in ipairs(opills) do if edist(b.mx, b.my, p.mx, p.my) <= R then by_o = true; break end end
      if by_n and not by_o then ncov[#ncov + 1] = b.id end
    end
  end
  table.sort(ncov)
  return out, ncov
end

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
-- target base cannot be covered. With maxn, it stops after maxn tiles or
-- when no tile covers a base still left, and answers the tiles and the
-- bases left uncovered (the plan re-pick uses this). With sr (a function
-- of a tile: the risk of the place our tank stands to send the builder
-- there), equal counts take the lower risk first ("cluster" rule).
function M.cover(world, targets, hpills, opills, ax, ay, maxn, sr)
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
                                da = ax and edist(x, y, ax, ay) or 0,
                                sr = sr and sr(x, y) or 0 }
          end
        end
      end
    end
  end
  local tiles = {}
  while #left > 0 and not (maxn and #tiles >= maxn) do
    local best, bn = nil, 0
    for _, c in ipairs(cand) do
      if not c.used then
        local n = 0
        for _, b in ipairs(left) do
          if edist(c.mx, c.my, b.mx, b.my) <= R then n = n + 1 end
        end
        if n > 0 and (n > bn or (n == bn and (c.sr < best.sr
                      or (c.sr == best.sr and (c.da < best.da
                      or (c.da == best.da and c.k < best.k)))))) then
          best, bn = c, n
        end
      end
    end
    if not best then
      if maxn then break end
      return nil
    end
    best.used = true
    tiles[#tiles + 1] = { mx = best.mx, my = best.my }
    local keep = {}
    for _, b in ipairs(left) do
      if edist(best.mx, best.my, b.mx, b.my) > R then keep[#keep + 1] = b end
    end
    left = keep
  end
  return tiles, left
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

-- -- Risk ("cluster" rule) ------------------------------------------------
-- Does known hostile pill p see a tank on (x,y)? In range, and not hidden
-- in a forest block HD or more tiles away on either axis.
local function pill_sees(world, HD, p, x, y)
  if edist(x, y, p.mx, p.my) > C.PILL_FIRE_RANGE then return false end
  local far = math.max(math.abs(x - p.mx), math.abs(y - p.my)) >= HD
  return not (far and forest_block(world, x, y))
end
M.pill_sees = pill_sees

-- The place our tank stands to send the builder to spot t, with no route:
-- the tile on the straight line from (sx,sy) to the spot, at
-- PLACE_DISPATCH_MAX_DIST (Manhattan) from the spot, or (sx,sy) itself
-- when it is nearer.
local function stand_on_line(sx, sy, t)
  local D = C.PLACE_DISPATCH_MAX_DIST or 15
  local dx, dy = sx - t.mx, sy - t.my
  local m = math.abs(dx) + math.abs(dy)
  if m <= D then return sx, sy end
  local f = D / m
  return t.mx + math.floor(dx * f + 0.5), t.my + math.floor(dy * f + 0.5)
end
M.stand_on_line = stand_on_line

-- Spot risk of one stand place: SPOT_WEIGHT x armour of each known hostile
-- pill that sees it.
local function stand_risk(world, info, hpills, x, y)
  local HD = hide_tiles(info)
  local r = 0
  for _, p in ipairs(hpills) do
    if pill_sees(world, HD, p, x, y) then r = r + (p.armour / 15) * C.FOCUS_SPIKE_SPOT_WEIGHT end
  end
  return r
end

-- Spot risk at one tile, with this bot's known hostile pills (builder.lua
-- asks this of the tank's own tile before the long send).
function M.stand_risk_at(world, info, x, y)
  return stand_risk(world, info, pills_of(world, true), x, y)
end

-- Pills never shoot builders: pillsUpdate (src/bolo/pillbox.c) fires only at
-- the nearest non-allied TANK in range; a builder dies only to a shell blast
-- near it (shellsBlastTestBuilder, src/bolo/shells.c). So a spike spot in a
-- hostile pill's range is fine; what counts is where our tank stands for the
-- long send (builder.lua deep_send: the tank sends the builder from up to
-- PLACE_DISPATCH_MAX_DIST, Manhattan, from the spot).
--
-- Stand place candidates of spot t: every tank-passable tile within
-- PLACE_DISPATCH_MAX_DIST (Manhattan) of the spot. rc caches the risk per
-- tile for one plan. Answers the lowest risk (no reach test; the cover
-- greedy tie-break), stopping at 0.
local function stand_min(world, info, hpills, t, rc)
  local D = C.PLACE_DISPATCH_MAX_DIST or 15
  local best = math.huge
  for dy = -D, D do
    local w = D - math.abs(dy)
    for dx = -w, w do
      local x, y = t.mx + dx, t.my + dy
      if U.in_map(x, y) and passable(world, x, y) then
        local k = tkey(x, y)
        local r = rc and rc[k]
        if r == nil then
          r = stand_risk(world, info, hpills, x, y)
          if rc then rc[k] = r end
        end
        if r < best then
          best = r
          if best <= 0 then return 0 end
        end
      end
    end
  end
  return best
end
M.stand_min = stand_min

-- The safest stand place of spot t: the lowest risk, then the nearest to the
-- spot (Manhattan, the shorter builder walk), then the lower tile key. With
-- the builder walk sim (cpf_lgm_travel_ticks_map, the check builder.lua
-- lgm_can_reach makes before a send) the builder must reach the spot from it;
-- at most FOCUS_SPIKE_STAND_REACH_CHECKS places are tried. Answers
-- { mx, my, risk } or nil (no place within reach).
function M.best_stand(world, info, hpills, t, rc)
  local D = C.PLACE_DISPATCH_MAX_DIST or 15
  local cand = {}
  for dy = -D, D do
    local w = D - math.abs(dy)
    for dx = -w, w do
      local x, y = t.mx + dx, t.my + dy
      if U.in_map(x, y) and passable(world, x, y) then
        local k = tkey(x, y)
        local r = rc and rc[k]
        if r == nil then
          r = stand_risk(world, info, hpills, x, y)
          if rc then rc[k] = r end
        end
        cand[#cand + 1] = { mx = x, my = y, risk = r, d = math.abs(dx) + math.abs(dy), k = k }
      end
    end
  end
  table.sort(cand, function(a, b)
    if a.risk ~= b.risk then return a.risk < b.risk end
    if a.d ~= b.d then return a.d < b.d end
    return a.k < b.k
  end)
  local walk = rawget(_G, "cpf_lgm_travel_ticks_map")
  local n = 0
  for _, c in ipairs(cand) do
    if not walk or c.d <= 1 then return { mx = c.mx, my = c.my, risk = c.risk } end
    n = n + 1
    if n > C.FOCUS_SPIKE_STAND_REACH_CHECKS then return nil end
    if walk(c.mx, c.my, t.mx, t.my, t.mx, t.my, 2000, 150) ~= -1 then
      return { mx = c.mx, my = c.my, risk = c.risk }
    end
  end
  return nil
end

-- The stand place of spike spot t with this bot's known hostile pills
-- (goals.lua plan tile block, each replan). Cached on state._fs per spot
-- while the known hostile pills stay the same, for at most
-- FOCUS_SPIKE_STAND_CACHE_TICKS (the walk sim is not cheap). false = no
-- stand place in reach.
function M.plan_stand(state, world, info, t, now)
  local F = st(state)
  -- FOCUS_SPIKE_ADAPTIVE_SPOT: the stand place of the rescore (val = stand
  -- risk + builder run cost, the bar builder.lua sends the builder under).
  local A = F.active
  if A and A.adaptive then
    local q = A.mine and A.mine.mx == t.mx and A.mine.my == t.my and A.mine.stand
    if not q then return nil end
    return { mx = q.mx, my = q.my, risk = q.risk, val = q.val }
  end
  local hp = pills_of(world, true)
  local hs = {}
  for _, p in ipairs(hp) do hs[#hs + 1] = string.format("%d,%d,%d", p.mx, p.my, p.armour) end
  local key = tkey(t.mx, t.my) .. "|" .. table.concat(hs, ";")
  F.stand_cache = F.stand_cache or {}
  local ce = F.stand_cache[tkey(t.mx, t.my)]
  now = now or 0
  if not ce or ce.key ~= key or (now - ce.t) > C.FOCUS_SPIKE_STAND_CACHE_TICKS then
    ce = { key = key, t = now, q = M.best_stand(world, info, hp, t, nil) or false }
    F.stand_cache[tkey(t.mx, t.my)] = ce
  end
  return ce.q or nil
end

-- Risk score of one cluster (with its spike spots), from this bot's view.
-- spare: spare pills beyond the need (for the credit). Answers a table:
-- total, spot, route, detour, tanks, credit, label, plen, straight,
-- n_spot_pills, n_route_pills, stands (one per spot), start. The route is
-- cached as in M.defense.
function M.risk(state, world, info, cl, tiles, hpills, opills, now, spare)
  local F = st(state)
  local D = C.PLACE_DISPATCH_MAX_DIST or 15
  local HD = hide_tiles(info)
  local pts = {}
  for _, b in ipairs(cl.targets or cl.bases) do pts[#pts + 1] = b end
  for _, t in ipairs(tiles) do pts[#pts + 1] = t end
  local d = { total = 0, spot = 0, route = 0, detour = 0, tanks = 0, credit = 0,
              label = "shallow", plen = 0, straight = 0, n_spot_pills = 0,
              n_route_pills = 0, stands = {} }
  local sx, sy = route_start(world, opills, cl.cx, cl.cy)
  -- The safest stand place of each spot (best_stand). The route goes to the
  -- stand places, not to the spots. A spot with no stand place in reach
  -- blocks the plan (route = DEFENSE_MAX).
  local rc = {}
  local no_stand = false
  for i, t in ipairs(tiles) do
    local q = M.best_stand(world, info, hpills, t, rc)
    if q then d.stands[i] = q else no_stand = true end
  end
  -- Spot exposure: each known hostile pill that sees a stand place, once.
  local spot_pills, route_pills = {}, {}
  for _, q in pairs(d.stands) do
    for _, p in ipairs(hpills) do
      if not spot_pills[p.id] and pill_sees(world, HD, p, q.mx, q.my) then
        spot_pills[p.id] = p
        d.spot = d.spot + (p.armour / 15) * C.FOCUS_SPIKE_SPOT_WEIGHT
        d.n_spot_pills = d.n_spot_pills + 1
      end
    end
  end
  if no_stand then
    d.label, d.blocked, d.no_stand = "deep", true, true
    d.route = C.FOCUS_SPIKE_DEFENSE_MAX
  elseif sx and #tiles > 0 then
    local hsig = {}
    for _, p in ipairs(hpills) do hsig[#hsig + 1] = string.format("%d,%d", p.mx, p.my) end
    local tsig = {}
    for _, q in ipairs(d.stands) do tsig[#tsig + 1] = tkey(q.mx, q.my) end
    local key = string.format("%d,%d|%s|%s", sx, sy, table.concat(tsig, ","), table.concat(hsig, ";"))
    local ce = F.route_cache[cl.key or cl.id]
    if not ce or ce.key ~= key or (now - ce.t) > C.FOCUS_SPIKE_ROUTE_CACHE_TICKS then
      local path, plen = M.route(world, info, sx, sy, d.stands, hpills)
      ce = { key = key, t = now, path = path, plen = plen }
      F.route_cache[cl.key or cl.id] = ce
    end
    d.start = { mx = sx, my = sy }
    if not ce.path then
      d.label, d.blocked = "deep", true
      d.route = C.FOCUS_SPIKE_DEFENSE_MAX
    else
      local path = ce.path
      local cut = #path
      local any_exposed, any_hidden = d.n_spot_pills > 0, false
      for i = 1, cut do
        local t = path[i]
        for _, p in ipairs(hpills) do
          if edist(t.mx, t.my, p.mx, p.my) <= C.PILL_FIRE_RANGE then
            if pill_sees(world, HD, p, t.mx, t.my) then
              any_exposed = true
              if not spot_pills[p.id] and not route_pills[p.id] then
                route_pills[p.id] = p
                d.route = d.route + (p.armour / 15) * C.FOCUS_SPIKE_DEFENSE_ROUTE_WEIGHT
                d.n_route_pills = d.n_route_pills + 1
              end
            else
              any_hidden = true
            end
          end
        end
      end
      d.label = any_exposed and "deep" or (any_hidden and "hidden" or "shallow")
      local last = path[#path]
      d.plen = ce.plen
      d.straight = edist(sx, sy, last.mx, last.my)
      d.detour = math.floor(math.max(0, d.plen - d.straight) / 10)
                 * C.FOCUS_SPIKE_DEFENSE_DETOUR_WEIGHT
    end
  end
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
  d.credit = math.min(C.FOCUS_SPIKE_SPARE_CREDIT_MAX,
                      math.max(0, spare or 0) * C.FOCUS_SPIKE_SPARE_CREDIT)
  d.total = math.max(0, d.spot + d.route + d.detour + d.tanks - d.credit)
  -- Tanks are a factor only: with no spot, route or detour risk the tank
  -- term alone never makes a cluster too risky (M.too_risky).
  d.tanks_only = (d.spot + d.route + d.detour) <= 0
  return d
end

-- A risk or defense table at or over FOCUS_SPIKE_DEFENSE_MAX. "cluster": not
-- when the tank term is the only risk.
function M.too_risky(d)
  if cluster_rule() and d.tanks_only then return false end
  return d.total >= C.FOCUS_SPIKE_DEFENSE_MAX
end

-- Spare pills K = team pills - the NORMAL front target (as plan()).
local function spare_k(world, info, now)
  local pf = PP.counts(world, now, true)
  local team = require("spike").team_pills(world, info, pf)
  local ftgt = PP.normal_targets(team).front
  return team - ftgt, team, ftgt
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
  if cluster_rule() then return M.plan_cluster(state, world, info, now) end
  local list, total, nopos = M.bases(world)
  local hp = pills_of(world, true)
  local op = pills_of(world, false)
  local cvp, ncov = M.cover_pills(world, op, list)
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
    M.log_clusters(F, info, now)
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
      local tiles = M.cover(world, targets, hp, cvp, ax, ay)
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
  return { ncov = ncov, pass = why == "ok", why = why, total = total, nopos = nopos,
           planned = planned, frac = frac, K = K, team = team, ftgt = ftgt,
           need = need, tiles = tiles, chosen = chosen, rows = rows,
           label = label, t = now }
end

-- -- The cluster plan (FOCUS_SPIKE_RULE "cluster") ------------------------
-- One cluster per plan, judged on its own: the spare pills K must put every
-- target base of that cluster within PILL_FIRE_RANGE of our pills. Each
-- cluster: cover (nil = a base cannot be covered: skip "uncoverable"; no
-- tile = already covered: skip "covered"), risk (skip "risk" at
-- DEFENSE_MAX), need <= K (skip "no_spare") and <= MAX_TILES. The best by
-- the order rule is chosen. Same answer table as M.plan, plus cluster (the
-- key: lowest base id), bases (the target base ids) and risk (the chosen
-- risk table). Peek reads only.
function M.plan_cluster(state, world, info, now)
  local list, total, nopos = M.bases(world)
  local hp = pills_of(world, true)
  local op = pills_of(world, false)
  local cvp, ncov = M.cover_pills(world, op, list)
  local K, team, ftgt = spare_k(world, info, now)
  local F = st(state)
  local ck = {}
  for _, b in ipairs(list) do ck[#ck + 1] = string.format("%d:%d,%d", b.id, b.mx, b.my) end
  ck = table.concat(ck, ";")
  if F.cl_key ~= ck then
    F.cl_key = ck
    F.clusters = M.clusters(list, C.FOCUS_SPIKE_CLUSTER_GAP)
    F.route_cache = {}
    M.log_clusters(F, info, now)
  end
  local obases = {}
  for _, b in ipairs(list) do if not b.target then obases[#obases + 1] = b end end
  local cur = {}
  for _, b in ipairs(list) do cur[b.id] = b end
  local rows = {}
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
      local home, ax, ay = math.huge, nil, nil
      for _, b in ipairs(obases) do
        local dd = edist(b.mx, b.my, cl.cx, cl.cy)
        if dd < home then home, ax, ay = dd, b.mx, b.my end
      end
      -- Pills do not shoot builders, so a spot in a hostile pill's range is
      -- not left out ({} as the hostile list); the tie-break is the risk of
      -- its safest stand place.
      local rc = {}
      local sr = function(x, y) return stand_min(world, info, hp, { mx = x, my = y }, rc) end
      local tiles = M.cover(world, targets, {}, cvp, ax, ay, nil, sr)
      local row = { id = cl.id, key = key, nb = #targets, nall = #cl.bases, home = home, cl = cl }
      if not tiles then
        row.skip, row.need, row.rank = "uncoverable", -1, 0
        row.def = { total = 0, label = "shallow" }
      elseif #tiles == 0 then
        row.skip, row.need, row.rank = "covered", 0, 0
        row.def = { total = 0, label = "shallow" }
      else
        row.tiles = tiles
        row.need = #tiles
        row.def = M.risk(state, world, info, cl, tiles, hp, op, now, K - #tiles)
        row.rank = #targets / (row.need + C.FOCUS_SPIKE_DEFENSE_WEIGHT * row.def.total)
        if M.too_risky(row.def) then row.skip = "risk"
        elseif row.need > K then row.skip = "no_spare"
        elseif row.need > C.FOCUS_SPIKE_MAX_TILES then row.skip = "max_tiles" end
      end
      rows[#rows + 1] = row
    end
  end
  local order = {}
  for _, r in ipairs(rows) do
    if not r.skip then
      local i = #order + 1
      while i > 1 and before(r, order[i - 1]) do i = i - 1 end
      table.insert(order, i, r)
    end
  end
  local best = order[1]
  local why = "ok"
  if total == 0 then why = "no_targets"
  elseif K <= 0 then why = "no_spare"
  elseif not best then why = "no_cluster" end
  local tiles, chosen, bases = {}, {}, {}
  local planned, need, label = 0, 0, "shallow"
  if best and why == "ok" then
    best.taken = true
    chosen[1] = best
    planned, need, label = best.nb, best.need, best.def.label
    for _, t in ipairs(best.tiles) do tiles[#tiles + 1] = { mx = t.mx, my = t.my, cl = best.id } end
    for _, b in ipairs(best.cl.targets) do bases[#bases + 1] = b.id end
  end
  return { ncov = ncov, pass = why == "ok", why = why, total = total, nopos = nopos,
           planned = planned, frac = total > 0 and planned / total or 0, K = K,
           team = team, ftgt = ftgt, need = need, tiles = tiles, chosen = chosen,
           rows = rows, label = label, t = now, cluster = best and why == "ok" and best.key or nil,
           bases = bases, risk = best and best.def or nil }
end

-- " ncov=<base ids>" (target bases only a neutral pill covers), "" when
-- FOCUS_SPIKE_NEUTRAL_PILL_COVER is off; "ncov=none" when on and empty.
function M.ncov_text(P)
  if C.FOCUS_SPIKE_NEUTRAL_PILL_COVER ~= true then return "" end
  local n = P and P.ncov or {}
  return " ncov=" .. (#n > 0 and table.concat(n, ",") or "none")
end

local function plan_line_cluster(now, P)
  local parts = {}
  for _, r in ipairs(P.rows) do
    local d = r.def
    parts[#parts + 1] = string.format(
      "c%s[%d target of %d bases, need %d, risk %.2f = spot %.2f (%d pills) + route %.2f (%d pills) + detour %.2f (path %.1f vs straight %.1f) + tanks %.2f - spare credit %.2f, %s, rank %.3f = %d / (%d + %.1f x %.2f) -> %s]",
      r.key, r.nb, r.nall, r.need, d.total, d.spot or 0, d.n_spot_pills or 0, d.route or 0,
      d.n_route_pills or 0, d.detour or 0, d.plen or 0, d.straight or 0, d.tanks or 0,
      d.credit or 0, LABEL_TEXT[d.label] or d.label, r.rank, r.nb, math.max(0, r.need),
      C.FOCUS_SPIKE_DEFENSE_WEIGHT, d.total,
      r.taken and "taken" or ("skip " .. tostring(r.skip)))
  end
  return string.format(
    "FOCUS_SPIKE_PLAN t=%d rule=cluster result=%s why=%s cluster=%s bases=%s (%d of %d target bases) spare K = team pills %d - normal front target %d = %d need %d spots label=%s%s clusters: %s",
    now, P.pass and "pass" or "fail", P.why, tostring(P.cluster), table.concat(P.bases or {}, ","),
    P.planned, P.total, P.team, P.ftgt, P.K, P.need,
    LABEL_TEXT[P.label] or P.label, M.ncov_text(P), table.concat(parts, " "))
end
M.plan_line_cluster = plan_line_cluster

local function plan_line(now, P)
  if cluster_rule() then return plan_line_cluster(now, P) end
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
  local id, need, planned, total, lab, tl, ckey, bl =
    text:match("^/info fso (%d+) (%d+) (%d+) (%d+) (%a) (%x*) (%d+) (%x*)$")
  if not id then
    id, need, planned, total, lab, tl = text:match("^/info fso (%d+) (%d+) (%d+) (%d+) (%a) (%x*)$")
  end
  if id then
    local bases = nil
    if bl then
      bases = {}
      for i = 1, #bl - 1, 2 do bases[#bases + 1] = tonumber(bl:sub(i, i + 1), 16) end
    end
    return { verb = "fso", id = tonumber(id), need = tonumber(need), planned = tonumber(planned),
             total = tonumber(total), label = CODE_LABEL[lab] or "deep", tiles = dec_tiles(tl),
             cluster = ckey, bases = bases }
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
  local uid, uo, un, ud = text:match("^/info fsu (%d+) (%x%x%x%x) (%x%x%x%x) (%d+)$")
  if uid then
    local o, n = dec_tiles(uo)[1], dec_tiles(un)[1]
    if n and n.mx == 255 and n.my == 255 then n = nil end
    return { verb = "fsu", id = tonumber(uid), old = o, new = n, ndrop = tonumber(ud) }
  end
  local pid, pt = text:match("^/info fsp (%d+) (%x%x%x%x)$")
  if pid then return { verb = "fsp", id = tonumber(pid), tile = dec_tiles(pt)[1] } end
  local kid, kl = text:match("^/info fsk (%d+) (%x*)$")
  if kid then return { verb = "fsk", id = tonumber(kid), claims = M.dec_claims(kl) } end
  local cid, cl = text:match("^/info fsc (%d+) (%x*)$")
  if cid then return { verb = "fsc", id = tonumber(cid), claims = M.dec_claims(cl) } end
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
-- With "base_lost" it also answers the lost base id, its owner at the go
-- and its owner now (nil when this bot no longer knows the base).
function M.home_lost(state, world, A)
  if C.FOCUS_SPIKE_HOME_ABORT_RULE == "tank_near" then return M.home_attacked(state, world) end
  if not A.home then
    A.home = {}
    for id, b in pairs(world.bases or {}) do
      if ours(b.owner) then A.home[id] = b.owner end
    end
    return false
  end
  local ids = {}
  for id in pairs(A.home) do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local b = world.bases and world.bases[id]
    if not b or not ours(b.owner) then return true, id, A.home[id], b and b.owner end
  end
  return false
end

-- A known live hostile pill within PILL_FIRE_RANGE of a tile in the list.
function M.tile_covered(world, tiles)
  local hp = pills_of(world, true)
  for _, t in ipairs(tiles) do
    if not t.placed and not t.gone and covered_by(hp, t.mx, t.my, C.PILL_FIRE_RANGE) then return t end
  end
  return nil
end

-- "cluster" rule: the first plan spot whose safest stand place got more
-- dangerous (a hostile pill learned since the go). The risk of each spot is
-- noted on the first check; later a rise answers that spot.
function M.stand_worse(world, info, tiles)
  local hp = pills_of(world, true)
  local rc = {}
  for _, t in ipairs(tiles) do
    if not t.placed and not t.gone then
      local r = stand_min(world, info, hp, t, rc)
      if t.stand_r == nil then
        t.stand_r = r
      elseif r > t.stand_r + 1e-6 then
        return t, r
      end
    end
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
  if cluster_rule() then
    local K = spare_k(world, info, now)
    return M.risk(state, world, info, cl, pend, pills_of(world, true), pills_of(world, false), now,
                  K - #pend)
  end
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
  if not cluster_rule() and M.tile_covered(world, prop.tiles) then return "no", "tile_covered" end
  if M.not_target(world, prop.tiles) then return "no", "not_target" end
  local d = M.tiles_defense(state, world, info, prop.tiles, now, "vote")
  if M.too_risky(d) then return "no", "defended", d end
  local mine = M.plan(state, world, info, now)
  F.plan = mine
  if not cluster_rule() and mine.pass and prop.total > 0
     and mine.frac > (prop.planned / prop.total) + C.FOCUS_SPIKE_BETTER_MARGIN then
    return "no", "better"
  end
  return "yes", M.pledge(info), eta_to(info, prop.tiles)
end

-- The pills this bot pledges: its carried pills; "cluster" rule: 0 when its
-- tank armour is below FOCUS_SPIKE_PLEDGE_MIN_ARMOR (it still votes yes).
function M.pledge(info)
  local n = math.max(0, info.carried_pills or 0)
  if cluster_rule() and (info.armour or 40) < C.FOCUS_SPIKE_PLEDGE_MIN_ARMOR then return 0 end
  return n
end

-- Assignments: candidates nearest ETA first (ties: lower player number) take
-- tiles in plan order, up to their pledge: no bot gets more tiles than it
-- pledged. Answers the tile list with pn set, or nil when the pledges do not
-- cover every tile.
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
  if pledged < open.need then
    return false, cluster_rule() and "pledges" or "pledge", yes, pledged, reasons
  end
  return true, "ok", yes, pledged, reasons
end

-- ── Active phase ────────────────────────────────────────────────────────
-- planned / total: the target bases of the vote (the proposer's count, from
-- the open message), for the re-pick limit. dropped counts the bases the
-- plan gave up after a re-pick found no tile for them.
-- claims (FOCUS_SPIKE_ADAPTIVE_SPOT): {[base id] = pn} of a fsk go; the
-- plan then starts with no spike spot (each bot picks its own).
local function start_active(state, F, id, tiles, now, from, info, claims)
  local vi = F.votes and F.votes[id]
  F.active = { id = id, tiles = tiles, t_go = now, from = from,
               planned = vi and vi.planned, total = vi and vi.total, dropped = 0,
               cluster = vi and vi.cluster, bases = vi and vi.bases }
  if claims then
    F.active.adaptive, F.active.claims, F.active.rel = true, claims, {}
    F.active.switches = {}
  end
  F.votes = nil
  F.open, F.voting = nil, nil
  local mine = 0
  for _, t in ipairs(tiles) do if t.pn == info.player_number then mine = mine + 1 end end
  for _, pn in pairs(claims or {}) do if pn == info.player_number then mine = mine + 1 end end
end

-- " cluster=<key> bases=<ids>" for the event lines, or "" (no cluster).
function M.cluster_text(v)
  if not v or not v.cluster then return "" end
  return string.format(" cluster=%s bases=%s", tostring(v.cluster), table.concat(v.bases or {}, ","))
end

-- One line with every cluster (print2 and the event file), when the
-- cluster list is built: the gap, then each cluster key (its lowest base
-- id, as cluster= in the other lines) with its base ids and places.
function M.log_clusters(F, info, now)
  local parts = {}
  for _, cl in ipairs(F.clusters or {}) do
    local bs = {}
    for _, b in ipairs(cl.bases) do bs[#bs + 1] = string.format("%d@(%d,%d)", b.id, b.mx, b.my) end
    parts[#parts + 1] = string.format("c%d=[%s]", cl.bases[1].id, table.concat(bs, " "))
  end
  local s = string.format("FOCUS_SPIKE clusters t=%d p%d gap=%s n=%d %s", now or 0,
    info and info.player_number or -1, tostring(C.FOCUS_SPIKE_CLUSTER_GAP), #(F.clusters or {}),
    table.concat(parts, " "))
  errline(s)
end

-- The stand place a builder of ours was last sent from to a spike spot
-- (builder.lua, on the dispatch edge): tank tile, its stand risk and the
-- planned stand place. Read by the placed line.
function M.note_send(state, world, info, mx, my, tmx, tmy, smx, smy, now)
  if not on() then return end
  local F = st(state)
  F.sent = F.sent or {}
  F.sent[tkey(mx, my)] = { mx = tmx, my = tmy, r = M.stand_risk_at(world, info, tmx, tmy),
                           smx = smx, smy = smy, t = now or 0 }
end

local function cooldown(F, now, why)
  F.cool_until = now + C.FOCUS_SPIKE_VOTE_FAIL_COOLDOWN
end

-- why: all_placed (each tile placed), tiles_gone (the rest can no longer be
-- placed), ttl, or abort_<why>. all_placed is split here by the placed count.
local function end_active(state, F, now, why, info)
  local A = F.active
  if not A then return end
  local placed, dropped = 0, 0
  for _, t in ipairs(A.tiles) do
    if t.placed then placed = placed + 1 elseif t.dropped then dropped = dropped + 1 end
  end
  if why == "all_placed" and placed + dropped < #A.tiles then why = "tiles_gone" end
  errline(string.format("FOCUS_SPIKE end t=%d p%d id=%d why=%s placed=%d tiles=%d after=%d dropped=%d bases_dropped=%d%s",
    now, info and info.player_number or -1, A.id, why, placed, #A.tiles, now - A.t_go, dropped,
    A.dropped or 0, M.cluster_text(A)))
  F.active = nil
end

local function abort(state, F, info, now, why)
  local A = F.active
  if not A then return end
  send(state, string.format("/info fsa %d %s", A.id, why))
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

-- ── Adaptive spike spot (FOCUS_SPIKE_ADAPTIVE_SPOT, "cluster" rule) ─────
-- A passed vote gives target BASES to the pledged bots (claims), not fixed
-- spike spots. Each claiming bot picks its spike spot itself, from where its
-- tank is now, and picks again every FOCUS_SPIKE_RESCORE_TICKS. The knob off
-- leaves every function above as it was.
local function adaptive()
  return C.FOCUS_SPIKE_ADAPTIVE_SPOT == true and cluster_rule()
end
M.adaptive = adaptive

local FREE = 255   -- claim value of a released base (no player)

-- The claims of a go: the cover tiles in plan order, each with the player
-- M.assign gave it; a tile claims each target base of the vote within
-- PILL_FIRE_RANGE that no earlier tile claimed. A base no tile covers (our
-- pill covers it already) gets no claim. Answers claims {[base id] = pn}
-- and the claimed base ids, sorted.
function M.claims_of(world, tiles, bases)
  local R = C.PILL_FIRE_RANGE
  local ids = {}
  for _, id in ipairs(bases or {}) do ids[#ids + 1] = id end
  table.sort(ids)
  local claims, order = {}, {}
  for _, t in ipairs(tiles) do
    for _, id in ipairs(ids) do
      local b = world.bases and world.bases[id]
      if claims[id] == nil and b and b.mx and edist(b.mx, b.my, t.mx, t.my) <= R then
        claims[id] = t.pn
      end
    end
  end
  for id in pairs(claims) do order[#order + 1] = id end
  table.sort(order)
  return claims, order
end

local function enc_claims(list)
  local s = {}
  for _, c in ipairs(list) do s[#s + 1] = string.format("%02X%02X", c.b, c.pn) end
  return table.concat(s)
end
local function dec_claims(s)
  local out = {}
  for i = 1, #s - 3, 4 do
    local b = tonumber(s:sub(i, i + 1), 16)
    local pn = tonumber(s:sub(i + 2, i + 3), 16)
    if b and pn then out[#out + 1] = { b = b, pn = pn } end
  end
  return out
end
M.enc_claims, M.dec_claims = enc_claims, dec_claims

local function claim_text(pn)
  if pn == FREE then return "free" end
  return "p" .. tostring(pn)
end

-- One claim change from player `by` (a fsc message, or this bot's own).
-- pn = FREE: `by` releases the base (only its own claim). Else `by` takes
-- the base: from a free base, from itself, or from a higher player number
-- (two bots took the same base: the lower player number keeps it). Answers
-- true when the claim changed.
function M.apply_claim(A, b, pn, by, now)
  local cur = A.claims[b]
  if cur == nil then return false end
  if pn == FREE then
    if cur ~= by then return false end
    A.claims[b] = FREE
    A.rel[b] = now
    return true
  end
  if cur == pn then return false end
  if cur == FREE or cur == by or pn < cur then
    A.claims[b] = pn
    A.rel[b] = nil
    return true
  end
  return false
end

-- This bot's own claim changes: apply, send one fsc, one line per change.
local function set_claims(state, A, info, now, list, why)
  local me = info.player_number or -1
  local sent = {}
  for _, c in ipairs(list) do
    local old = A.claims[c.b]
    if M.apply_claim(A, c.b, c.pn, me, now) then
      sent[#sent + 1] = c
      errline(string.format("FOCUS_SPIKE claim t=%d p%d id=%d base=%d old=%s new=%s why=%s", now, me, A.id,
        c.b, claim_text(old), claim_text(c.pn), why))
    end
  end
  if #sent > 0 then send(state, string.format("/info fsc %d %s", A.id, enc_claims(sent))) end
end

-- A base is covered: a live pill of ours (and a neutral one with
-- FOCUS_SPIKE_NEUTRAL_PILL_COVER) or a placed spike spot of the plan within
-- PILL_FIRE_RANGE.
local function base_covered(A, cov, b)
  local R = C.PILL_FIRE_RANGE
  if covered_by(cov, b.mx, b.my, R) then return true end
  for _, t in ipairs(A.tiles) do
    if t.placed and edist(t.mx, t.my, b.mx, b.my) <= R then return true end
  end
  return false
end

-- The pills that rule a spike spot out: known live hostile pills and every
-- known live neutral pill (a neutral pill shoots our tank too).
local function block_pills(world)
  local out = pills_of(world, true)
  if C.FOCUS_SPIKE_NEUTRAL_PILL_COVER ~= true then
    local ids = {}
    for id in pairs(world.pills or {}) do ids[#ids + 1] = id end
    table.sort(ids)
    for _, id in ipairs(ids) do
      local p = world.pills[id]
      if not p.in_tank and p.mx and (p.health or 0) > 0 and p.owner == "neutral" then
        out[#out + 1] = { id = id, mx = p.mx, my = p.my, armour = p.health or 0 }
      end
    end
  end
  return out
end
M.block_pills = block_pills

-- The stand place of spike spot c: the tank-passable tile within
-- PLACE_DISPATCH_MAX_DIST (Manhattan, not the spot itself) with the lowest
-- stand risk + FOCUS_SPIKE_LGM_RUN_COST x builder run (euclid, stand ->
-- spot); ties: the nearer ring, then the scan order. rc caches the stand
-- risk per tile for one rescore. Answers { mx, my, risk, run, val, d } or nil.
function M.adapt_stand(world, info, hpills, c, rc)
  local D = C.PLACE_DISPATCH_MAX_DIST or 15
  local RC = C.FOCUS_SPIKE_LGM_RUN_COST
  local best = nil
  for d = 1, D do
    for dx = -d, d do
      local w = d - math.abs(dx)
      for s = -1, 1, 2 do
        local dy = w * s
        if not (w == 0 and s == 1) then
          local x, y = c.mx + dx, c.my + dy
          if U.in_map(x, y) and passable(world, x, y) then
            local k = tkey(x, y)
            local r = rc[k]
            if r == nil then
              r = stand_risk(world, info, hpills, x, y)
              rc[k] = r
            end
            local run = RC * edist(x, y, c.mx, c.my)
            local v = r + run
            if not best or v < best.val - 1e-9 then
              best = { mx = x, my = y, risk = r, run = run, val = v, d = d, k = k }
            end
          end
        end
      end
    end
    -- No tile of a farther ring can beat it: its run alone is at least
    -- RUN_COST x (d+1) / sqrt(2).
    if best and best.val <= RC * (d + 1) / math.sqrt(2) then break end
  end
  return best
end

-- Route risk from the tank to a stand place: each known hostile pill that
-- does not see the stand place but sees a tile of the straight line from the
-- tank to it, FOCUS_SPIKE_DEFENSE_ROUTE_WEIGHT x armour/15, once per pill.
function M.line_risk(world, info, hpills, tx, ty, q)
  local HD = hide_tiles(info)
  local n = math.max(math.abs(q.mx - tx), math.abs(q.my - ty))
  local r = 0
  for _, p in ipairs(hpills) do
    if not pill_sees(world, HD, p, q.mx, q.my) then
      for i = 0, n do
        local f = n > 0 and i / n or 0
        local x = math.floor(tx + (q.mx - tx) * f + 0.5)
        local y = math.floor(ty + (q.my - ty) * f + 0.5)
        if pill_sees(world, HD, p, x, y) then
          r = r + (p.armour / 15) * C.FOCUS_SPIKE_DEFENSE_ROUTE_WEIGHT
          break
        end
      end
    end
  end
  return r
end

-- Every spike spot for the bases in left (this bot's claimed bases not yet
-- covered), scored from the tank tile (tx,ty):
--   total = BASE_PAYOFF x bases of left it covers
--           - (stand + route + travel + run)
-- A spot must cover a base of left within PILL_FIRE_RANGE (euclid,
-- inclusive), be placeable land now (not forest), have no known live hostile
-- or neutral pill within PILL_FIRE_RANGE, and have a stand place. It is
-- allowed only while its danger (stand + route) is at most
-- FOCUS_SPIKE_DEFENSE_MAX: the travel and builder run costs are not danger
-- and never rule a spot out by themselves. The spots are cut first to
-- FOCUS_SPIKE_RESCORE_MAX_SPOTS (most bases, then nearest the tank; keep is
-- one tile key that always stays). The best spots must pass the builder walk
-- check (at most FOCUS_SPIKE_RESCORE_REACH_CHECKS). Answers the allowed
-- spots, best total first (ties: lower tile key), each
-- { mx, my, k, n, stand, parts = {payoff, stand, route, travel, run}, total }.
function M.score_spots(world, info, left, tx, ty, keep)
  local R = C.PILL_FIRE_RANGE
  local hp = pills_of(world, true)
  local blk = block_pills(world)
  local cand, ck = {}, {}
  local Ri = math.floor(R)
  for _, b in ipairs(left) do
    for y = b.my - Ri, b.my + Ri do
      for x = b.mx - Ri, b.mx + Ri do
        local k = tkey(x, y)
        if not ck[k] and U.in_map(x, y) and edist(x, y, b.mx, b.my) <= R then
          ck[k] = true
          if U.is_placeable_peek(x, y, world) and U.ttype_peek(x, y) ~= C.T_FOREST
             and not covered_by(blk, x, y, R) then
            local n = 0
            for _, b2 in ipairs(left) do
              if edist(x, y, b2.mx, b2.my) <= R then n = n + 1 end
            end
            cand[#cand + 1] = { mx = x, my = y, k = k, n = n, dt = edist(tx, ty, x, y) }
          end
        end
      end
    end
  end
  table.sort(cand, function(a, b)
    if a.k == keep then return b.k ~= keep end
    if b.k == keep then return false end
    if a.n ~= b.n then return a.n > b.n end
    if a.dt ~= b.dt then return a.dt < b.dt end
    return a.k < b.k
  end)
  local rc, out = {}, {}
  for i = 1, math.min(#cand, C.FOCUS_SPIKE_RESCORE_MAX_SPOTS) do
    local c = cand[i]
    local q = M.adapt_stand(world, info, hp, c, rc)
    if q then
      local route = M.line_risk(world, info, hp, tx, ty, q)
      local travel = math.floor(edist(tx, ty, q.mx, q.my) / 10) * C.FOCUS_SPIKE_DEFENSE_DETOUR_WEIGHT
      local payoff = c.n * C.FOCUS_SPIKE_BASE_PAYOFF
      c.stand = q
      c.parts = { payoff = payoff, stand = q.risk, route = route, travel = travel, run = q.run }
      c.danger = q.risk + route
      c.total = payoff - (q.risk + route + travel + q.run)
      if c.danger <= C.FOCUS_SPIKE_DEFENSE_MAX + 1e-9 then out[#out + 1] = c end
    end
  end
  table.sort(out, function(a, b)
    if math.abs(a.total - b.total) > 1e-9 then return a.total > b.total end
    return a.k < b.k
  end)
  local walk = rawget(_G, "cpf_lgm_travel_ticks_map")
  if walk then
    local kept, checks, found = {}, 0, false
    for _, c in ipairs(out) do
      if found or c.stand.d <= 1 then
        kept[#kept + 1] = c
        found = true
      elseif checks < C.FOCUS_SPIKE_RESCORE_REACH_CHECKS then
        checks = checks + 1
        if walk(c.stand.mx, c.stand.my, c.mx, c.my, c.mx, c.my, 2000, 150) ~= -1 then
          kept[#kept + 1] = c
          found = true
        else
          c.noreach = true
        end
      end
    end
    out = kept
  end
  return out
end

-- Switch rule: the new spot must beat the current one by
-- FOCUS_SPIKE_SWITCH_MARGIN x max(|current|, 1) (20% of the current score,
-- at least 0.20).
function M.should_switch(cur_total, new_total)
  return new_total - cur_total > C.FOCUS_SPIKE_SWITCH_MARGIN * math.max(math.abs(cur_total), 1) + 1e-9
end

-- Why the current spot is out of the list (the switch reason).
local function why_out(world, A, blk, cur, left)
  local R = C.PILL_FIRE_RANGE
  if not U.is_placeable_peek(cur.mx, cur.my, world) then return "spot_blocked" end
  if covered_by(blk, cur.mx, cur.my, R) then return "pill_covers_spot" end
  local any = false
  for _, b in ipairs(left) do
    if edist(cur.mx, cur.my, b.mx, b.my) <= R then any = true; break end
  end
  if not any then return "bases_covered" end
  return "too_risky"
end

local function parts_text(c)
  if not c then return "none" end
  local p = c.parts
  return string.format("(%d,%d) stand=(%d,%d) total=%.2f = payoff %.2f - stand %.2f - route %.2f - travel %.2f - run %.2f",
    c.mx, c.my, c.stand.mx, c.stand.my, c.total, p.payoff, p.stand, p.route, p.travel, p.run)
end
M.parts_text = parts_text

-- This bot's chosen spot changes: the plan tile list keeps the placed spots
-- and this bot's chosen one.
local function set_spot(A, info, now, c, why, old_c)
  local me = info.player_number or -1
  local keep = {}
  for _, t in ipairs(A.tiles) do
    if t.placed or t.pn ~= me then keep[#keep + 1] = t end
  end
  local old = A.mine
  if c then
    A.mine = { mx = c.mx, my = c.my, pn = me,
               stand = { mx = c.stand.mx, my = c.stand.my, risk = c.stand.risk, val = c.stand.val } }
    keep[#keep + 1] = A.mine
  else
    A.mine = nil
  end
  A.tiles = keep
  A.switches = A.switches or {}
  A.switches[#A.switches + 1] = { t = now, why = why,
    old = old and { mx = old.mx, my = old.my } or nil, new = c and { mx = c.mx, my = c.my } or nil,
    old_total = old_c and old_c.total or nil, new_total = c and c.total or nil }
  while #A.switches > 8 do table.remove(A.switches, 1) end
  errline(string.format("FOCUS_SPIKE spot t=%d p%d id=%d old=%s new=%s why=%s old_total=%s new_total=%s stand=%s new_parts=%s",
    now, me, A.id, old and string.format("%d,%d", old.mx, old.my) or "none",
    c and string.format("%d,%d", c.mx, c.my) or "none", why,
    old_c and string.format("%.2f", old_c.total) or "-", c and string.format("%.2f", c.total) or "-",
    c and string.format("%d,%d", c.stand.mx, c.stand.my) or "none",
    c and string.format("%.2f/%.2f/%.2f/%.2f/%.2f", c.parts.payoff, c.parts.stand, c.parts.route,
      c.parts.travel, c.parts.run) or "-"))
end

-- One adaptive step of the active plan (every FOCUS_SPIKE_RESCORE_TICKS).
-- Answers nil (go on), "all_placed" (every claimed base is covered) or
-- "incomplete" (every base left is free for FOCUS_SPIKE_RELEASE_WAIT ticks
-- and no bot took it).
function M.adapt_step(state, F, world, info, now)
  local A = F.active
  if A.t_rescore and (now - A.t_rescore) < C.FOCUS_SPIKE_RESCORE_TICKS then return nil end
  A.t_rescore = now
  local me = info.player_number or -1
  local cov = M.cover_pills(world, pills_of(world, false), nil)
  local ids = {}
  for id in pairs(A.claims) do ids[#ids + 1] = id end
  table.sort(ids)
  local mine, free, all = {}, {}, true
  for _, id in ipairs(ids) do
    local b = world.bases and world.bases[id]
    if b and b.mx and not base_covered(A, cov, b) then
      all = false
      local q = { id = id, mx = b.mx, my = b.my }
      if A.claims[id] == me then mine[#mine + 1] = q
      elseif A.claims[id] == FREE then free[#free + 1] = q end
    end
  end
  if all then return "all_placed" end
  -- A free base for too long with no taker: the plan cannot cover it.
  local stuck = #free > 0
  for _, id in ipairs(ids) do
    local b = world.bases and world.bases[id]
    if b and b.mx and not base_covered(A, cov, b) then
      if A.claims[id] ~= FREE or (now - (A.rel[id] or now)) < C.FOCUS_SPIKE_RELEASE_WAIT then stuck = false end
    end
  end
  if stuck then return "incomplete" end
  -- The builder is out: keep the spot it went for.
  if info.man_status ~= C.LGM_INTANK then return nil end
  local carried = info.carried_pills or 0
  if carried < 1 then
    if #mine > 0 then
      local rl = {}
      for _, q in ipairs(mine) do rl[#rl + 1] = { b = q.id, pn = FREE } end
      set_claims(state, A, info, now, rl, "no_pill")
      if A.mine then set_spot(A, info, now, nil, "no_pill", nil) end
    end
    return nil
  end
  local tx, ty = tank_tile(info)
  -- My claims are done: take the free bases a spike spot of mine can cover.
  if #mine == 0 and #free > 0 then
    local list = M.score_spots(world, info, free, tx, ty, nil)
    local take = {}
    for _, q in ipairs(free) do
      for _, c in ipairs(list) do
        if edist(c.mx, c.my, q.mx, q.my) <= C.PILL_FIRE_RANGE then
          take[#take + 1] = { b = q.id, pn = me }
          mine[#mine + 1] = q
          break
        end
      end
    end
    if #take > 0 then set_claims(state, A, info, now, take, "take_free") end
  end
  if #mine == 0 then
    if A.mine and not A.mine.placed then set_spot(A, info, now, nil, "no_claim", nil) end
    A.score = { t = now, top = {}, mine = {} }
    return nil
  end
  local keep = A.mine and tkey(A.mine.mx, A.mine.my) or nil
  local list = M.score_spots(world, info, mine, tx, ty, keep)
  -- Claimed bases no allowed spot covers: release them.
  local rl = {}
  for _, q in ipairs(mine) do
    local ok = false
    for _, c in ipairs(list) do
      if edist(c.mx, c.my, q.mx, q.my) <= C.PILL_FIRE_RANGE then ok = true; break end
    end
    if not ok then rl[#rl + 1] = { b = q.id, pn = FREE } end
  end
  if #rl > 0 then set_claims(state, A, info, now, rl, "no_spot") end
  local best = list[1]
  local cur = nil
  if keep then
    for _, c in ipairs(list) do if c.k == keep then cur = c; break end end
  end
  if not A.mine then
    if best then set_spot(A, info, now, best, A.n_placed and "next" or "first", nil) end
  elseif not cur then
    set_spot(A, info, now, best, why_out(world, A, block_pills(world), A.mine, mine), nil)
  elseif best and best.k ~= cur.k and M.should_switch(cur.total, best.total) then
    set_spot(A, info, now, best, "better", cur)
  else
    -- Same spot: its stand place is scored again in the same pass.
    A.mine.stand = { mx = cur.stand.mx, my = cur.stand.my, risk = cur.stand.risk, val = cur.stand.val }
  end
  local top = {}
  for i = 1, math.min(3, #list) do top[i] = list[i] end
  local chosen = nil
  if A.mine then
    for _, c in ipairs(list) do if c.mx == A.mine.mx and c.my == A.mine.my then chosen = c; break end end
  end
  local mids = {}
  for _, q in ipairs(mine) do mids[#mids + 1] = q.id end
  A.score = { t = now, top = top, chosen = chosen, mine = mids, tank = { mx = tx, my = ty } }
  return nil
end

-- A fsp message: the spike pill at (mx,my) is built (by player `by`). The
-- plan tile there is placed; in adaptive mode a placed spot not in the list
-- is added. Answers true when it changed.
function M.mark_placed(A, mx, my, by, now, care_tag)
  for _, t in ipairs(A.tiles) do
    if t.mx == mx and t.my == my then
      if t.placed then return false end
      t.placed, t.t_placed, t.gone = true, now, nil
      if care_tag then care_tag(mx, my, now) end
      return true
    end
  end
  if not A.adaptive then return false end
  A.tiles[#A.tiles + 1] = { mx = mx, my = my, pn = by, placed = true, t_placed = now }
  if care_tag then care_tag(mx, my, now) end
  return true
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
        local skip = false
        if F.open then
          if m.from < me then
            F.open = nil
          else
            skip = true
          end
        end
        if not skip then
          F.voting = { id = v.id, from = m.from, t = now }
          F.votes = { [v.id] = { planned = v.planned, total = v.total, cluster = v.cluster,
                                 bases = v.bases } }
          local yn, a, b = M.ballot(state, world, info, v, now)
          if yn == "yes" then
            send(state, string.format("/info fsy %d %d %d", v.id, a, b))
          else
            send(state, string.format("/info fsn %d %s", v.id, a))
          end
        end
      elseif (v.verb == "fsy" or v.verb == "fsn") and F.open and F.open.id == v.id then
        F.open.ballots[m.from] = { yes = v.verb == "fsy", pledge = v.pledge or 0,
                                   eta = v.eta or 255, reason = v.reason }
      elseif v.verb == "fsr" then
        if F.voting and F.voting.id == v.id then F.voting = nil end
        if not v.pass and v.reason ~= "better" then cooldown(F, now, v.reason) end
      elseif v.verb == "fsg" then
        if not F.active then start_active(state, F, v.id, v.tiles, now, m.from, info) end
      elseif v.verb == "fsk" then
        if not F.active then
          local cl = {}
          for _, c in ipairs(v.claims) do cl[c.b] = c.pn end
          start_active(state, F, v.id, {}, now, m.from, info, cl)
        end
      elseif v.verb == "fsc" then
        local A = F.active
        if A and A.id == v.id and A.adaptive then
          for _, c in ipairs(v.claims) do
            local old = A.claims[c.b]
            if M.apply_claim(A, c.b, c.pn, m.from, now) then
              if old == me then
                errline(string.format("FOCUS_SPIKE claim t=%d p%d id=%d base=%d old=%s new=%s why=lower_player_took",
                  now, me, A.id, c.b, claim_text(old), claim_text(c.pn)))
              end
            end
          end
        end
      elseif v.verb == "fsp" then
        -- Bug fix: a team bot built a spike pill for the plan. Mark it placed
        -- here too, also when this bot cannot see the new pill.
        local A = F.active
        if A and A.id == v.id and v.tile then
          M.mark_placed(A, v.tile.mx, v.tile.my, m.from, now, care_tag)
        end
      elseif v.verb == "fsu" then
        if F.active and F.active.id == v.id and v.old then
          M.apply_repick(F.active, v.old, v.new, v.ndrop, m.from, info, now)
        end
      elseif v.verb == "fsa" then
        if F.active and F.active.id == v.id then
          end_active(state, F, now, "abort_" .. v.reason, info)
          cooldown(F, now, v.reason)
        end
      end
    end
  end
end

local function resolve_open(state, F, info, now, world)
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
  -- FOCUS_SPIKE_ADAPTIVE_SPOT: the go gives target bases (claims), not
  -- spike spots. Each assigned cover tile claims the bases it covers.
  if adaptive() and world then
    local claims, order = M.claims_of(world, asg, O.bases)
    local cl, al = {}, {}
    for _, id in ipairs(order) do
      cl[#cl + 1] = { b = id, pn = claims[id] }
      al[#al + 1] = string.format("b%d->p%d", id, claims[id])
    end
    send(state, string.format("/info fsk %d %s", O.id, enc_claims(cl)))
    errline(string.format("FOCUS_SPIKE go t=%d p%d id=%d tiles=0 label=%s claims=%s%s", now,
      info.player_number or -1, O.id, O.label, table.concat(al, " "), M.cluster_text(O)))
    F.votes = { [O.id] = { planned = O.planned, total = O.total, cluster = O.cluster, bases = O.bases } }
    start_active(state, F, O.id, {}, now, O.me, info, claims)
    return
  end
  local s = {}
  for _, t in ipairs(asg) do s[#s + 1] = string.format("%02X%02X%02X", t.mx, t.my, t.pn) end
  send(state, string.format("/info fsg %d %s", O.id, table.concat(s)))
  local al = {}
  for _, t in ipairs(asg) do
    local q = O.stands and O.stands[tkey(t.mx, t.my)]
    al[#al + 1] = string.format("(%d,%d)->p%d", t.mx, t.my, t.pn)
      .. (q and string.format(" stand=(%d,%d) r=%.2f", q.mx, q.my, q.risk or 0) or "")
  end
  errline(string.format("FOCUS_SPIKE go t=%d p%d id=%d tiles=%d label=%s %s%s", now,
    info.player_number or -1, O.id, #asg, O.label, table.concat(al, " "), M.cluster_text(O)))
  F.votes = { [O.id] = { planned = O.planned, total = O.total, cluster = O.cluster, bases = O.bases } }
  start_active(state, F, O.id, asg, now, O.me, info)
end

-- -- Re-pick (FOCUS_SPIKE_TILE_COVERED_RULE "repick") --------------------
-- Apply one tile change to the plan: the tile at old moves to new (same bot),
-- or is dropped when new is nil, and ndrop bases leave the plan. Two bots can
-- re-pick the same tile at once: a change from a lower player number replaces
-- this bot's own change of that tile; a higher one is ignored.
function M.apply_repick(A, old, new, ndrop, by, info, now)
  local me = info and info.player_number or -1
  local ok = tkey(old.mx, old.my)
  local t = nil
  for _, x in ipairs(A.tiles) do
    if not x.placed and not x.dropped and x.mx == old.mx and x.my == old.my then t = x; break end
  end
  if not t then
    for _, x in ipairs(A.tiles) do
      if x.rp and x.rp.old == ok and x.rp.by == me and by < me and not x.placed then t = x; break end
    end
    if not t then return false end
    A.dropped = (A.dropped or 0) - (t.rp.ndrop or 0)
    t.dropped, t.gone = nil, nil
    t.mx, t.my = t.rp.omx, t.rp.omy
  end
  t.rp = { old = ok, by = by, ndrop = ndrop or 0, omx = old.mx, omy = old.my }
  if new then
    t.mx, t.my = new.mx, new.my
  else
    t.dropped, t.gone = true, true
  end
  A.dropped = (A.dropped or 0) + (ndrop or 0)
  return true
end

-- This bot sees a known hostile pill in fire range of plan tile ct. Find one
-- new tile for the target bases only ct was to cover: the cover greedy with
-- the other plan tiles and our pills as cover, tiles in range of known
-- hostile pills left out, and the defense skip rule. Bases with no tile leave
-- the plan. Answers true when the plan was aborted (too few bases left).
function M.repick(state, F, world, info, now, ct)
  local A = F.active
  local R = C.PILL_FIRE_RANGE
  local hp = pills_of(world, true)
  local cov = {}
  for _, p in ipairs((M.cover_pills(world, pills_of(world, false), nil))) do cov[#cov + 1] = p end
  for _, t in ipairs(A.tiles) do
    if t ~= ct and not t.dropped
       and (t.placed or cluster_rule() or not covered_by(hp, t.mx, t.my, R)) then
      cov[#cov + 1] = { mx = t.mx, my = t.my }
    end
  end
  local list, total = M.bases(world)
  local need = {}
  for _, b in ipairs(list) do
    if b.target and edist(b.mx, b.my, ct.mx, ct.my) <= R and not covered_by(cov, b.mx, b.my, R) then
      need[#need + 1] = b
    end
  end
  local new, left = nil, need
  local def = nil
  if #need > 0 then
    local excl, sr = hp, nil
    if cluster_rule() then
      local rc = {}
      excl = {}
      sr = function(x, y) return stand_min(world, info, hp, { mx = x, my = y }, rc) end
    end
    local tiles, rest = M.cover(world, need, excl, cov, ct.mx, ct.my, 1, sr)
    if tiles and tiles[1] then
      def = M.tiles_defense(state, world, info, { tiles[1] }, now, "repick")
      if not M.too_risky(def) then new, left = tiles[1], rest end
    end
  end
  local old = { mx = ct.mx, my = ct.my }
  local pn = ct.pn
  -- "cluster": the old spot is still the best one (its stand place got
  -- worse, but no spot is safer): keep it and note the new risk.
  if cluster_rule() and #left == 0 and new and new.mx == ct.mx and new.my == ct.my then
    ct.stand_r = stand_min(world, info, hp, ct, nil)
    return false
  end
  -- "cluster": the plan must cover every target base of its cluster. No spot
  -- for the bases this one was to cover: the cluster is not complete.
  if cluster_rule() and #left > 0 then
    errline(string.format("FOCUS_SPIKE repick t=%d p%d id=%d old=%d,%d new=none pn=%d need=%d drop=%d defense=%s planned=%d total=%d",
      now, info.player_number or -1, A.id, old.mx, old.my, pn or -1, #need, #left,
      def and string.format("%.2f", def.total) or "-", A.planned or #need, A.total or total))
    abort(state, F, info, now, "incomplete")
    return true
  end
  M.apply_repick(A, old, new, #left, info.player_number or -1, info, now)
  send(state, string.format("/info fsu %d %s %s %d", A.id, enc_tiles({ old }),
    new and enc_tiles({ new }) or "FFFF", #left))
  A.planned = A.planned or #need
  A.total = A.total or total
  local keep = A.planned - (A.dropped or 0)
  errline(string.format("FOCUS_SPIKE repick t=%d p%d id=%d old=%d,%d new=%s pn=%d need=%d drop=%d defense=%s planned=%d total=%d",
    now, info.player_number or -1, A.id, old.mx, old.my,
    new and string.format("%d,%d", new.mx, new.my) or "none", pn or -1, #need, #left,
    def and string.format("%.2f", def.total) or "-", keep, A.total))
  for _, b in ipairs(left) do
    errline(string.format("FOCUS_SPIKE dropbase t=%d p%d id=%d base=%d at=%d,%d", now,
      info.player_number or -1, A.id, b.id, b.mx, b.my))
  end
  if A.total > 0 and keep / A.total < C.FOCUS_SPIKE_DENY_FRAC then
    abort(state, F, info, now, "covered_frac")
    return true
  end
  return false
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
        if t.pn == info.player_number then
          local sd = F.sent and F.sent[tkey(t.mx, t.my)]
          local stxt = sd and string.format(" stand=(%d,%d) r=%.2f planned=%s sent_t=%d", sd.mx, sd.my, sd.r or 0,
            sd.smx and string.format("(%d,%d)", sd.smx, sd.smy) or "none", sd.t) or " stand=none"
          errline(string.format("FOCUS_SPIKE placed t=%d p%d id=%d tile=%d,%d%s", now, t.pn, A.id, t.mx, t.my, stxt))
          -- Bug fix: tell the team at once. A bot that cannot see the new
          -- pill kept the plan on (and voted no) until the plan ttl.
          send(state, string.format("/info fsp %d %s", A.id, enc_tiles({ t })))
          if A.adaptive and A.mine == t then A.mine, A.n_placed = nil, (A.n_placed or 0) + 1 end
        end
      elseif not A.adaptive and not U.is_placeable_peek(t.mx, t.my, world) then
        t.gone = true
      end
    end
    if not t.placed and not t.gone then all = false end
  end
  if A.adaptive then
    if (now - A.t_go) > C.FOCUS_SPIKE_APPROVAL_TTL then end_active(state, F, now, "ttl", info); return end
    local r = M.adapt_step(state, F, world, info, now)
    if r == "all_placed" then
      local keep = {}
      for _, t in ipairs(A.tiles) do if t.placed then keep[#keep + 1] = t end end
      A.tiles, A.mine = keep, nil
      end_active(state, F, now, "all_placed", info)
      return
    end
    if r == "incomplete" then abort(state, F, info, now, "incomplete"); return end
  else
    if all then end_active(state, F, now, "all_placed", info); return end
    if (now - A.t_go) > C.FOCUS_SPIKE_APPROVAL_TTL then end_active(state, F, now, "ttl", info); return end
  end
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
  local ct
  if A.adaptive then
    ct = nil
  elseif cluster_rule() then
    ct = M.stand_worse(world, info, A.tiles)
  else
    ct = M.tile_covered(world, A.tiles)
  end
  if ct then
    if C.FOCUS_SPIKE_TILE_COVERED_RULE ~= "repick" then
      abort(state, F, info, now, "tile_covered"); return
    end
    if M.repick(state, F, world, info, now, ct) then return end
  end
  if C.FOCUS_SPIKE_ABORT_HOME_ATTACKED then
    local lost, bid, was, nowo = M.home_lost(state, world, A)
    if lost then
      if bid then
        local b = world.bases and world.bases[bid]
        errline(string.format("FOCUS_SPIKE homelost t=%d p%d id=%d base=%d at=%s was=%s now=%s", now,
          info.player_number or -1, A.id, bid, b and string.format("%d,%d", b.mx or 0, b.my or 0) or "?",
          tostring(was), tostring(nowo)))
      end
      abort(state, F, info, now, "home_attacked"); return
    end
  end
  if not A.adaptive and (now - (A.t_route or -1e9)) >= C.FOCUS_SPIKE_PLAN_INTERVAL then
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
  resolve_open(state, F, info, now, world)
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
  end
  if not P.pass or not wants_to_place(state, info) then return end
  -- Open the vote.
  local bots = M.team_bots(info)
  local me = info.player_number or 0
  local id = me * 65536 + bit.band(now, 0xFFFF)
  F.open = { id = id, t = now, me = me, bots = bots, ballots = {}, need = P.need,
             tiles = P.tiles, pledge = M.pledge(info),
             eta = eta_to(info, P.tiles), label = P.label, planned = P.planned, total = P.total,
             cluster = P.cluster, bases = P.bases, stands = {} }
  -- The stand place of each spike spot (cluster rule), for the go line.
  if P.risk and P.risk.stands then
    for i, t in ipairs(P.tiles or {}) do
      local q = P.risk.stands[i]
      if q then F.open.stands[tkey(t.mx, t.my)] = q end
    end
  end
  local suffix = ""
  if P.cluster then
    local bh = {}
    for _, b in ipairs(P.bases or {}) do bh[#bh + 1] = string.format("%02X", b) end
    suffix = string.format(" %s %s", P.cluster, table.concat(bh))
  end
  send(state, string.format("/info fso %d %d %d %d %s %s%s", id, P.need, P.planned, P.total,
    LABEL_CODE[P.label], enc_tiles(P.tiles), suffix))
  errline(string.format("FOCUS_SPIKE open t=%d p%d id=%d need=%d planned=%d total=%d label=%s bots=%d%s%s",
    now, me, id, P.need, P.planned, P.total, LABEL_TEXT[P.label], #bots,
    M.cluster_text(F.open) .. M.ncov_text(P),
    P.risk and string.format(" risk=%.2f spot=%.2f route=%.2f detour=%.2f tanks=%.2f credit=%.2f",
      P.risk.total, P.risk.spot or 0, P.risk.route or 0, P.risk.detour or 0, P.risk.tanks or 0,
      P.risk.credit or 0) or ""))
  -- A team of one bot resolves at once.
  if #bots <= 1 then resolve_open(state, F, info, now, world) end
end

return M
