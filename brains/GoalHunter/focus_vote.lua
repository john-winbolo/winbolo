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

M.errline = errline

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
local function forest_free(world, tx, ty)
  if not U.in_map(tx, ty) or U.ttype_peek(tx, ty) ~= C.T_FOREST then return false end
  local k = ty * C.MAP_W + tx
  if world.pill_at and world.pill_at[k] then return false end
  if world.base_at and world.base_at[k] then return false end
  return true
end
local function forest_block(world, x, y)
  if not forest_free(world, x, y) then return false end
  for sx = -1, 1, 2 do
    if forest_free(world, x + sx, y) then
      for sy = -1, 1, 2 do
        if forest_free(world, x, y + sy) and forest_free(world, x + sx, y + sy) then return true end
      end
    end
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
      -- The shared route risk (M.path_risk).
      local r, hits, any_exposed, any_hidden = M.path_risk(world, info, hpills, ce.path, nil)
      d.route = r
      for _, h in ipairs(hits) do route_pills[h.id] = h end
      d.n_route_pills = #hits
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

-- Route risk of one tank path (a list of { mx, my }, start first). The one
-- route risk function of the vote planner (M.defense, M.risk) and of the
-- adaptive spot score (M.score_spots): each known hostile pill that sees a
-- tile of the path (pill_sees) adds FOCUS_SPIKE_DEFENSE_ROUTE_WEIGHT x
-- armour/15, once per pill. A pill in skip (the pills already counted as
-- stand risk) adds nothing. Answers the risk, the pills that add to it
-- ({ id, armour, mx, my, add, at = the first path tile it sees }, in path
-- order), any_exposed (a pill sees a path tile, skip included) and
-- any_hidden (a pill in range is blind to a path tile in a forest block).
function M.path_risk(world, info, hpills, path, skip)
  local HD = hide_tiles(info)
  local R = C.PILL_FIRE_RANGE
  local r, hits, seen = 0, {}, {}
  local any_exposed, any_hidden = false, false
  for _, t in ipairs(path) do
    for _, p in ipairs(hpills) do
      if edist(t.mx, t.my, p.mx, p.my) <= R then
        if pill_sees(world, HD, p, t.mx, t.my) then
          any_exposed = true
          if not (skip and skip[p.id]) and not seen[p.id] then
            seen[p.id] = true
            local add = (p.armour / 15) * C.FOCUS_SPIKE_DEFENSE_ROUTE_WEIGHT
            r = r + add
            hits[#hits + 1] = { id = p.id, armour = p.armour, mx = p.mx, my = p.my, add = add,
                                at = { mx = t.mx, my = t.my } }
          end
        else
          any_hidden = true
        end
      end
    end
  end
  return r, hits, any_exposed, any_hidden
end

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
  local R = C.PILL_FIRE_RANGE
  local r = 0
  local fb = nil  -- forest_block of (x,y), found once when a pill needs it
  for _, p in ipairs(hpills) do
    -- The same test as pill_sees, with forest_block found once per tile.
    if edist(x, y, p.mx, p.my) <= R then
      local seen = true
      if math.max(math.abs(x - p.mx), math.abs(y - p.my)) >= HD then
        if fb == nil then fb = forest_block(world, x, y) end
        seen = not fb
      end
      if seen then r = r + (p.armour / 15) * C.FOCUS_SPIKE_SPOT_WEIGHT end
    end
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
      -- The shared route risk (M.path_risk); the pills that see a stand
      -- place are counted once, as spot risk.
      local r, hits, ex, hid = M.path_risk(world, info, hpills, path, spot_pills)
      d.route = r
      for _, h in ipairs(hits) do route_pills[h.id] = h end
      d.n_route_pills = #hits
      local any_exposed, any_hidden = d.n_spot_pills > 0 or ex, hid
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
  local sid, sp = text:match("^/info fss (%d+) (%x%x%x%x)$")
  if sid then
    local t = dec_tiles(sp)[1]
    if t and t.mx == 255 and t.my == 255 then t = nil end
    return { verb = "fss", id = tonumber(sid), tile = t }
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
  if BRAIN_DEBUG_MODE then require("focus_viz").snapshot(F, world, pills_of(world, false)) end
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
    -- FOCUS_SPIKE_ALL_AGGRO: the spike spots teammates picked ({[pn] = tile},
    -- verb fss) and the last tick a bot had a picked spot.
    if C.FOCUS_SPIKE_ALL_AGGRO == true then F.active.picks, F.active.t_pick = {}, now end
  end
  F.votes = nil
  F.open, F.voting = nil, nil
  if BRAIN_DEBUG_MODE then require("focus_viz").note(F, "go", { id = id, tiles = tiles, t = now, claims = claims }) end
  local mine = 0
  for _, t in ipairs(tiles) do if t.pn == info.player_number then mine = mine + 1 end end
  for _, pn in pairs(claims or {}) do if pn == info.player_number then mine = mine + 1 end end
  print2(string.format("FOCUS_SPIKE_ACTIVE t=%d on id=%d from p%d tiles=%d mine=%d ttl=%d",
    now, id, from, #tiles, mine, C.FOCUS_SPIKE_APPROVAL_TTL))
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
  print2(s)
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

-- why: all_placed (each tile placed), bases_covered (adaptive: every base is
-- covered, some by pills outside the plan), all_laid (FOCUS_SPIKE_ALL_AGGRO:
-- every reserved pill is placed), tiles_gone (the rest can no longer be
-- placed), ttl, or abort_<why>. all_placed is split here by the placed count.
local function end_active(state, F, now, why, info)
  local A = F.active
  if not A then return end
  local placed, dropped = 0, 0
  for _, t in ipairs(A.tiles) do
    if t.placed then placed = placed + 1 elseif t.dropped then dropped = dropped + 1 end
  end
  if why == "all_placed" and placed + dropped < #A.tiles then why = "tiles_gone" end
  print2(string.format("FOCUS_SPIKE_ACTIVE t=%d off id=%d why=%s placed %d of %d (%d dropped) after %dt",
    now, A.id, why, placed, #A.tiles, dropped, now - A.t_go))
  errline(string.format("FOCUS_SPIKE end t=%d p%d id=%d why=%s placed=%d tiles=%d after=%d dropped=%d bases_dropped=%d%s",
    now, info and info.player_number or -1, A.id, why, placed, #A.tiles, now - A.t_go, dropped,
    A.dropped or 0, M.cluster_text(A)))
  if BRAIN_DEBUG_MODE then require("focus_viz").note(F, "end", { id = A.id, why = why, t = now }) end
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

-- ── Adaptive spike spot (FOCUS_SPIKE_ADAPTIVE_SPOT, "cluster" rule) ─────
-- A passed vote gives target BASES to the pledged bots (claims), not fixed
-- spike spots. Each claiming bot picks its spike spot itself, from where its
-- tank is now, and picks again every FOCUS_SPIKE_RESCORE_TICKS. The knob off
-- leaves every function above as it was.
local function adaptive()
  return C.FOCUS_SPIKE_ADAPTIVE_SPOT == true and cluster_rule()
end
M.adaptive = adaptive

-- ── All aggressive pills (FOCUS_SPIKE_ALL_AGGRO, needs the adaptive spot) ─
local function all_aggro()
  return C.FOCUS_SPIKE_ALL_AGGRO == true and adaptive()
end
M.all_aggro = all_aggro

-- True while an all-aggressive plan is active: every pill this bot carries
-- (also one it picks up during the plan) is for the spike.
function M.reserved(state)
  if not all_aggro() then return false end
  local F = state and state._fs
  return F ~= nil and F.active ~= nil and F.active.adaptive == true
end

-- May this bot place a NEW pill at (mx,my)? Always true with no reservation.
-- While reserved: only on the spike spot this bot picked, or on a tile where
-- a pill of ours stands already (a repair also uses BUILDMODE_PBOX).
function M.may_place(state, world, mx, my)
  if not M.reserved(state) then return true end
  local A = state._fs.active
  if A.mine and A.mine.mx == mx and A.mine.my == my then return true end
  for _, p in pairs(world and world.pills or {}) do
    if not p.in_tank and p.mx == mx and p.my == my and ours(p.owner) then return true end
  end
  return false
end

-- One FOCUS_SPIKE reserve line per tile and plan: a build path wanted to
-- place a reserved pill at (mx,my) and was stopped (where = the path).
function M.note_reserve(state, info, mx, my, now, where)
  local F = st(state)
  local A = F.active
  if not A then return end
  if F.res_id ~= A.id then F.res_id, F.res_log = A.id, {} end
  local k = tkey(mx, my)
  if F.res_log[k] then return end
  F.res_log[k] = true
  local s = string.format("FOCUS_SPIKE reserve t=%d p%d id=%d tile=%d,%d where=%s carried=%d", now or 0,
    info.player_number or -1, A.id, mx, my, tostring(where), info.carried_pills or 0)
  print2(s)
  errline(s)
end

-- The pills team bots carry, in this bot's own world model: this bot's
-- info.carried_pills, and the carried pills of the allies (world.pills in_tank,
-- carrier = their player number, world.sync_ally_carried). Answers the total
-- and {[pn] = count}.
function M.team_carried(world, info)
  local me = info.player_number or -1
  local per = {}
  local n = info.carried_pills or 0
  if n > 0 then per[me] = n end
  for _, p in pairs(world.pills or {}) do
    if p.in_tank and p.carrier and p.carrier ~= me and ours(p.owner) then
      per[p.carrier] = (per[p.carrier] or 0) + 1
      n = n + 1
    end
  end
  return n, per
end

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
      print2(string.format("FOCUS_SPIKE_ACTIVE t=%d claim id=%d base %d %s -> %s (%s)", now, A.id, c.b,
        claim_text(old), claim_text(c.pn), why))
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

-- The path our tank would drive from its tile to (x,y), read from the map
-- the bot already has: its tank-rooted KIND_NORMAL Dijkstra slates
-- (init.lua start_slate; the map the goal ETAs read). The path is traced
-- back from (x,y) through the slate's parent links
-- (cpf_dijkstra_trace_path_by_kind, braincore.c; at most 2048 tiles). No
-- new search per spot. With no slate (unit tests and offline checks) the
-- vote planner's own search M.route from the tank tile, or
-- M.tank_path_hook when set. Answers the path ({ mx, my } list, start
-- first) and its length (diagonal steps 1.414), or nil (not reachable).
function M.tank_path(world, info, hpills, tx, ty, x, y)
  local tr = rawget(_G, "cpf_dijkstra_trace_path_by_kind")
  if tr then
    local f = tr(0 --[[KIND_NORMAL]], x, y, 2048)
    if not f or #f < 2 then return nil end
    local path, plen = {}, 0
    for i = 1, #f - 1, 2 do path[#path + 1] = { mx = f[i], my = f[i + 1] } end
    for i = 2, #path do
      local a, b = path[i - 1], path[i]
      plen = plen + ((a.mx ~= b.mx and a.my ~= b.my) and math.sqrt(2) or 1)
    end
    return path, plen
  end
  if M.tank_path_hook then return M.tank_path_hook(world, info, hpills, tx, ty, x, y) end
  return M.route(world, info, tx, ty, { { mx = x, my = y } }, hpills)
end

-- The payoff of spike spot (x,y) for the bases in left (the bases of S, or
-- the claimed bases): for each base within PILL_FIRE_RANGE (euclid,
-- inclusive) BASE_PAYOFF x v, v = (R + 1 - d) / R with
-- FOCUS_SPIKE_CLOSE_PAYOFF (d = euclid spot -> base, R = PILL_FIRE_RANGE),
-- else 1.0. The sum is multiplied by FOCUS_SPIKE_MULTI_BOOST when 2 or more
-- of these bases are within FOCUS_SPIKE_MULTI_RADIUS (euclid, inclusive).
-- Answers payoff, the bases { id, d, v } (v with BASE_PAYOFF), the count
-- within MULTI_RADIUS and the boost used (1.0 when not used).
function M.spot_payoff(x, y, left)
  local R = C.PILL_FIRE_RANGE
  local close = C.FOCUS_SPIKE_CLOSE_PAYOFF
  local MR = C.FOCUS_SPIKE_MULTI_RADIUS or 0
  local sum, bl, nm = 0, {}, 0
  for _, b in ipairs(left) do
    local d = edist(x, y, b.mx, b.my)
    if d <= R + 1e-9 then
      local v = (close and (R + 1 - d) / R or 1.0) * C.FOCUS_SPIKE_BASE_PAYOFF
      sum = sum + v
      bl[#bl + 1] = { id = b.id, d = d, v = v }
      if d <= MR + 1e-9 then nm = nm + 1 end
    end
  end
  local boost = (nm >= 2) and (C.FOCUS_SPIKE_MULTI_BOOST or 1.0) or 1.0
  return sum * boost, bl, nm, boost
end

-- Wide stand search (FOCUS_SPIKE_STAND_RADIUS > 1). ti: the tile facts of
-- one rescore, per tile key (tank-passable, reached by the tank's map, stand
-- risk, travel, map cost); filled on first use. Step A: every tile within
-- the radius (Manhattan, not the spot) that is passable, reached by the map
-- and has stand risk <= DEFENSE_MAX, ranked by stand risk + builder run +
-- travel (ties: the lower map cost, then the lower tile key); the best 3
-- are kept. With no such tile, the tile with the lowest stand risk + run +
-- travel of all reached tiles (it is ruled out by its stand risk). Answers
-- the list of the best (at most 3), each { mx, my, risk, run, val, d, k,
-- travel, a, sc }, val = risk + run (the builder send check), a = the step
-- A value.
local SQRT2 = math.sqrt(2)
-- The tile offsets of the wide stand search, ring by ring (Manhattan 1..R),
-- and where each ring ends; built once per radius.
local OFFS = {}
local function offsets(R)
  local o = OFFS[R]
  if o then return o end
  o = { x = {}, y = {}, run = {}, ring = {}, last = {} }
  local n = 0
  for d = 1, R do
    for dx = -d, d do
      local w = d - math.abs(dx)
      for s = 1, (w ~= 0) and 2 or 1 do
        n = n + 1
        o.x[n], o.y[n], o.ring[n] = dx, (s == 1) and w or -w, d
        o.run[n] = math.sqrt(dx * dx + w * w)
      end
    end
    o.last[d] = n
  end
  o.n = n
  OFFS[R] = o
  return o
end

-- Answers true when (a, sc, k) ranks before q: the lower step A value, then
-- the lower map cost, then the lower tile key.
local function ranks_before(a, sc, k, q)
  local qa = q.a
  if a < qa - 1e-9 then return true end
  if a > qa + 1e-9 then return false end
  if sc ~= q.sc then return sc < q.sc end
  return k < q.k
end

-- One tile's facts for the wide stand search (ti: this rescore's cache).
local function tile_fact(world, info, hp, ti, x, y, tx, ty, slate, W)
  local k = y * 256 + x
  local base = false
  if passable(world, x, y) then
    local sc = slate and slate(0 --[[KIND_NORMAL]], x, y, 0) or edist(tx, ty, x, y)
    if sc and sc < 1e29 then
      local r = stand_risk(world, info, hp, x, y)
      local tr = math.floor(edist(tx, ty, x, y) / 10) * W
      ti.r[k], ti.t[k], ti.s[k] = r, tr, sc
      base = r + tr
    end
  end
  ti.b[k] = base
  return base
end

local function wide_stands(world, info, hp, c, ti, tx, ty, slate)
  local Rm = math.min(C.FOCUS_SPIKE_STAND_RADIUS, C.PLACE_DISPATCH_MAX_DIST or 15)
  local RC = C.FOCUS_SPIKE_LGM_RUN_COST
  local MAX = C.FOCUS_SPIKE_DEFENSE_MAX + 1e-9
  local W = C.FOCUS_SPIKE_DEFENSE_DETOUR_WEIGHT
  -- ti.b[k]: stand risk + travel of tile k (false: not passable or not
  -- reached by the map); ti.r, ti.t, ti.s: stand risk, travel, map cost.
  local TB, TR, TS = ti.b, ti.r, ti.s
  local O = offsets(Rm)
  local OX, OY, ORUN, ORING, OLAST = O.x, O.y, O.run, O.ring, O.last
  local top, n, fb = {}, 0, nil
  local cx, cy = c.mx, c.my
  local dts = edist(tx, ty, cx, cy)
  local d = 1
  for i = 1, O.n do
    local x, y = cx + OX[i], cy + OY[i]
    if x >= 0 and y >= 0 and x < 256 and y < 256 then
      local k = y * 256 + x
      local base = TB[k]
      if base == nil then
        base = U.in_map(x, y) and tile_fact(world, info, hp, ti, x, y, tx, ty, slate, W) or false
        TB[k] = base
      end
      if base then
        local run = RC * ORUN[i]
        local av = base + run
        local sc = TS[k]
        if TR[k] <= MAX then
          if n < 3 or ranks_before(av, sc, k, top[3]) then
            local q = { mx = x, my = y, risk = TR[k], run = run, val = TR[k] + run, d = ORING[i], k = k,
                        travel = ti.t[k], sc = sc, a = av }
            local j = (n < 3) and n + 1 or 3
            if n < 3 then n = n + 1 end
            top[j] = q
            while j > 1 and ranks_before(q.a, q.sc, q.k, top[j - 1]) do
              top[j], top[j - 1] = top[j - 1], q
              j = j - 1
            end
          end
        elseif not fb or ranks_before(av, sc, k, fb) then
          fb = { mx = x, my = y, risk = TR[k], run = run, val = TR[k] + run, d = ORING[i], k = k,
                 travel = ti.t[k], sc = sc, a = av }
        end
      end
    end
    -- At the end of ring d: stop when no tile of a farther ring can rank in
    -- the best 3, not even in a tie (the answer is the same as a scan of
    -- every ring). A tile on ring e (Manhattan e from the spot): stand risk
    -- >= 0, run >= RUN_COST x e / sqrt(2), and it is at least (spot -> tank
    -- - e) from the tank, so travel >= floor((spot -> tank - e) / 10) x
    -- DETOUR_WEIGHT.
    if i == OLAST[d] then
      if n == 3 then
        local lim = top[3].a + 1e-9
        local stop = true
        for e = d + 1, Rm do
          if math.floor(math.max(0, dts - e) / 10) * W + RC * e / SQRT2 <= lim then stop = false; break end
        end
        if stop then break end
      end
      d = d + 1
    end
  end
  if n == 0 and fb then top[1] = fb end
  return top
end

-- Every spike spot for the bases in left (this bot's claimed bases not yet
-- covered), scored from the tank tile (tx,ty):
--   total = payoff (M.spot_payoff: BASE_PAYOFF per base of left it covers;
--           closer and multi-base terms with FOCUS_SPIKE_CLOSE_PAYOFF and
--           FOCUS_SPIKE_MULTI_BOOST)
--           - (stand + route + travel + run)
-- A spot must cover a base of left within PILL_FIRE_RANGE (euclid,
-- inclusive), be placeable land now (not forest), and have a stand place.
-- A hostile or neutral pill near the spot does not rule it out (bug fix): a
-- pill never shoots a builder or a pill, so only the tank risk counts (stand
-- + route). It is
-- allowed only while its danger (stand + route) is at most
-- FOCUS_SPIKE_DEFENSE_MAX: the travel and builder run costs are not danger
-- and never rule a spot out by themselves. With FOCUS_SPIKE_STAND_RADIUS 1
-- (KEEL) the spots are cut first to FOCUS_SPIKE_RESCORE_MAX_SPOTS (most
-- bases, then nearest the tank; keep is one tile key that always stays) and
-- the stand place is M.adapt_stand. With a radius > 1 every spot is scored
-- and the stand place comes from wide_stands (step A) and the route risk of
-- its best 3 (step B). The best spots must pass the builder walk
-- check (at most FOCUS_SPIKE_RESCORE_REACH_CHECKS). Answers the allowed
-- spots, best total first (ties: lower tile key), each
-- { mx, my, k, n, cov, stand, parts = {payoff, stand, route, travel, run},
-- route_pills, plen, path, danger, total }.
-- The route risk (M.path_risk, the vote planner's function) is counted along
-- the path the tank would drive to the stand place (M.tank_path, the bot's
-- Dijkstra map), not a straight line. A stand place the map cannot reach
-- rules the spot out ("unreachable").
-- Second answer (log only, for the spot_eval line): the counts of each
-- check and the best rejected spot, { tiles, not_placeable, found, cut,
-- tested, no_stand, unreachable, stand_risk, route_risk, noreach,
-- unchecked, allowed, rej, all } (rej.why = the check that ruled it out;
-- all = every candidate found, the tested ones with their parts).
-- The score is cut into a pass (FOCUS_SPIKE_RESCORE_SLICES): spots_begin
-- finds and sorts the spots, spots_step scores the next spots, and
-- spots_finish sorts the allowed spots and runs the builder walk check.
-- score_spots runs all three at once. A pass keeps the tank tile, the
-- bases of left and the hostile pills of spots_begin, so the answer is the
-- same as one full pass while the world does not change.
local function spot_reject(P, c, why)
  local D = P.D
  c.why = why
  D[why] = D[why] + 1
  local r = D.rej
  if not r or c.total > r.total + 1e-9 or (math.abs(c.total - r.total) <= 1e-9 and c.k < r.k) then
    D.rej = c
  end
end

-- The route risk to stand place q, once per stand place in the pass.
local function spot_route(P, world, info, q)
  local pr = P.prc[q.k]
  if pr == nil then
    local hp = P.hp
    -- The pills that see the stand place are stand risk already.
    local skip = {}
    for _, p in ipairs(hp) do
      if pill_sees(world, P.HD, p, q.mx, q.my) then skip[p.id] = true end
    end
    local path, plen = M.tank_path(world, info, hp, P.tx, P.ty, q.mx, q.my)
    pr = false
    if path then
      local r, hits = M.path_risk(world, info, hp, path, skip)
      local last = path[#path]
      pr = { r = r, hits = hits, plen = plen or 0, path = path,
             cut = not (last and last.mx == q.mx and last.my == q.my) }
    end
    P.prc[q.k] = pr
    P.D.traced = P.D.traced + 1
  end
  return pr
end

function M.spots_begin(world, info, left, tx, ty, keep)
  local R = C.PILL_FIRE_RANGE
  local cand, ck = {}, {}
  local Ri = math.floor(R)
  local D = { tiles = 0, not_placeable = 0, found = 0, cut = 0, tested = 0, no_stand = 0,
              unreachable = 0, stand_risk = 0, route_risk = 0, noreach = 0, unchecked = 0,
              allowed = 0 }
  for _, b in ipairs(left) do
    for y = b.my - Ri, b.my + Ri do
      for x = b.mx - Ri, b.mx + Ri do
        local k = tkey(x, y)
        if not ck[k] and U.in_map(x, y) and edist(x, y, b.mx, b.my) <= R then
          ck[k] = true
          D.tiles = D.tiles + 1
          if U.is_placeable_peek(x, y, world) and U.ttype_peek(x, y) ~= C.T_FOREST then
            local n, cov = 0, {}
            for _, b2 in ipairs(left) do
              if edist(x, y, b2.mx, b2.my) <= R then
                n = n + 1
                if b2.id then cov[#cov + 1] = b2.id end
              end
            end
            cand[#cand + 1] = { mx = x, my = y, k = k, n = n, cov = cov, dt = edist(tx, ty, x, y) }
          else
            D.not_placeable = D.not_placeable + 1
          end
        end
      end
    end
  end
  D.found = #cand
  D.all = cand
  table.sort(cand, function(a, b)
    if a.k == keep then return b.k ~= keep end
    if b.k == keep then return false end
    if a.n ~= b.n then return a.n > b.n end
    if a.dt ~= b.dt then return a.dt < b.dt end
    return a.k < b.k
  end)
  -- FOCUS_SPIKE_STAND_RADIUS > 1: the wide stand search, and every spot is
  -- scored (no RESCORE_MAX_SPOTS cut).
  local wide = (C.FOCUS_SPIKE_STAND_RADIUS or 1) > 1
  D.radius = wide and math.min(C.FOCUS_SPIKE_STAND_RADIUS, C.PLACE_DISPATCH_MAX_DIST or 15) or 1
  D.tested = wide and #cand or math.min(#cand, C.FOCUS_SPIKE_RESCORE_MAX_SPOTS)
  D.cut = #cand - D.tested
  D.traced = 0
  return { left = left, tx = tx, ty = ty, keep = keep, cand = cand, D = D, wide = wide,
           hp = pills_of(world, true), HD = hide_tiles(info),
           rc = {}, prc = {}, ti = { b = {}, r = {}, t = {}, s = {} },  -- caches, this pass
           slate = wide and (rawget(_G, "cpf_dijkstra_lookup_by_kind") or M.tank_cost_hook) or nil,
           out = {}, i = 0 }
end

-- Scores the next n spots of pass P (all when n is nil). Answers true when
-- every spot to test is scored.
function M.spots_step(P, world, info, n)
  local D, cand, left = P.D, P.cand, P.left
  local last = n and math.min(D.tested, P.i + n) or D.tested
  for i = P.i + 1, last do
    local c = cand[i]
    local q, pr = nil, nil
    if P.wide then
      -- Step B: the route risk of the best 3 of step A. The stand place: the
      -- lowest stand + route + travel + run with danger allowed; else the
      -- lowest of the 3 that the map path reaches (it is ruled out).
      local top = wide_stands(world, info, P.hp, c, P.ti, P.tx, P.ty, P.slate)
      local bv, bok = nil, false
      for _, s in ipairs(top) do
        local p = spot_route(P, world, info, s)
        if p then
          local v = s.risk + p.r + s.travel + s.run
          local ok = s.risk + p.r <= C.FOCUS_SPIKE_DEFENSE_MAX + 1e-9
          if not q or (ok and not bok) or (ok == bok and v < bv - 1e-9) then
            q, pr, bv, bok = s, p, v, ok
          end
        end
      end
      if not q and top[1] then q, pr = top[1], false end
      c.stands = top
    else
      q = M.adapt_stand(world, info, P.hp, c, P.rc)
      if q then pr = spot_route(P, world, info, q) end
    end
    if q then
      local route = pr and pr.r or 0
      local travel = math.floor(edist(P.tx, P.ty, q.mx, q.my) / 10) * C.FOCUS_SPIKE_DEFENSE_DETOUR_WEIGHT
      local payoff, pb, nm, boost = M.spot_payoff(c.mx, c.my, left)
      c.pay_bases, c.pay_multi, c.boost, c.sr = pb, nm, boost, D.radius
      c.stand = q
      c.parts = { payoff = payoff, stand = q.risk, route = route, travel = travel, run = q.run }
      c.route_pills = pr and pr.hits or {}
      c.plen = pr and pr.plen or nil
      c.path = pr and pr.path or nil
      c.path_cut = pr and pr.cut or false
      c.danger = q.risk + route
      c.total = payoff - (q.risk + route + travel + q.run)
      if not pr then
        spot_reject(P, c, "unreachable")
      elseif c.danger <= C.FOCUS_SPIKE_DEFENSE_MAX + 1e-9 then
        P.out[#P.out + 1] = c
      elseif q.risk > C.FOCUS_SPIKE_DEFENSE_MAX + 1e-9 then
        spot_reject(P, c, "stand_risk")
      else
        spot_reject(P, c, "route_risk")
      end
    else
      D.no_stand = D.no_stand + 1
    end
  end
  P.i = last
  return last >= D.tested
end

-- The end of pass P: the allowed spots, best total first, and the builder
-- walk check. Answers out, D (as score_spots).
function M.spots_finish(P)
  local D, out = P.D, P.out
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
          spot_reject(P, c, "noreach")
        end
      else
        spot_reject(P, c, "unchecked")
      end
    end
    out = kept
  end
  D.allowed = #out
  return out, D
end

function M.score_spots(world, info, left, tx, ty, keep)
  local P = M.spots_begin(world, info, left, tx, ty, keep)
  M.spots_step(P, world, info, nil)
  return M.spots_finish(P)
end

-- Switch rule: the new spot must beat the current one by
-- FOCUS_SPIKE_SWITCH_MARGIN x max(|current|, 1) (20% of the current score,
-- at least 0.20).
function M.should_switch(cur_total, new_total)
  return new_total - cur_total > C.FOCUS_SPIKE_SWITCH_MARGIN * math.max(math.abs(cur_total), 1) + 1e-9
end

-- Why the current spot is out of the list (the switch reason). none_why:
-- the reason when the spot covers no base of left (default bases_covered).
local function why_out(world, A, cur, left, none_why)
  local R = C.PILL_FIRE_RANGE
  if not U.is_placeable_peek(cur.mx, cur.my, world) then return "spot_blocked" end
  local any = false
  for _, b in ipairs(left) do
    if edist(cur.mx, cur.my, b.mx, b.my) <= R then any = true; break end
  end
  if not any then return none_why or "bases_covered" end
  return "too_risky"
end

local function parts_text(c)
  if not c then return "none" end
  local p = c.parts
  return string.format("(%d,%d) stand=(%d,%d) total=%.2f = payoff %.2f - stand %.2f - route %.2f - travel %.2f - run %.2f",
    c.mx, c.my, c.stand.mx, c.stand.my, c.total, p.payoff, p.stand, p.route, p.travel, p.run)
end
M.parts_text = parts_text

-- ── spot_eval log line (log only) ──────────────────────────────────────
-- One line, stable key=value names (no spaces in a value), to print2 and
-- the event file, on each rescore of an active adaptive plan and on every
-- pick, change or drop. The parts of a spot (eval_parts):
--   st:x,y|sd:(stand Manhattan tiles from the spot)|sr:(stand radius
--   used: FOCUS_SPIKE_STAND_RADIUS, at most PLACE_DISPATCH_MAX_DIST)|
--   cov:ids|pay:(3 places)|pb:id@d=v;...(each base: euclid d, payoff v)|
--   nm:(bases within FOCUS_SPIKE_MULTI_RADIUS)|boost:(x on the sum of v)|
--   stand:|route:|plen:|rp:id/armour@x,y;...|cut:0/1|travel:|run:|
--   risk:(stand+route, the value compared with FOCUS_SPIKE_DEFENSE_MAX)|
--   total:
-- pay = (sum of v) x boost; total = pay - stand - route - travel - run.
local function f2(v) return string.format("%.2f", v or 0) end
local function eval_parts(c)
  if not c then return "-" end
  local p = c.parts or {}
  local rp = {}
  for _, h in ipairs(c.route_pills or {}) do
    rp[#rp + 1] = string.format("%d/%d@%d,%d", h.id, h.armour, h.mx, h.my)
  end
  local pb = {}
  for _, b in ipairs(c.pay_bases or {}) do
    pb[#pb + 1] = string.format("%s@%.2f=%.3f", tostring(b.id or "?"), b.d, b.v)
  end
  return string.format("st:%s|sd:%s|sr:%s|cov:%s|pay:%s|pb:%s|nm:%d|boost:%s|stand:%s|route:%s|plen:%s|rp:%s|cut:%d|travel:%s|run:%s|risk:%s|total:%s",
    c.stand and string.format("%d,%d", c.stand.mx, c.stand.my) or "none",
    c.stand and tostring(c.stand.d) or "-", tostring(c.sr or "-"),
    (c.cov and #c.cov > 0) and table.concat(c.cov, ",") or "-",
    string.format("%.3f", p.payoff or 0), #pb > 0 and table.concat(pb, ";") or "-",
    c.pay_multi or 0, f2(c.boost or 1), f2(p.stand), f2(p.route), c.plen and string.format("%.1f", c.plen) or "-",
    #rp > 0 and table.concat(rp, ";") or "-", c.path_cut and 1 or 0,
    f2(p.travel), f2(p.run), f2(c.danger), f2(c.total))
end
M.eval_parts = eval_parts

local function xy(c) return c and string.format("%d,%d", c.mx, c.my) or "none" end

-- The spot_eval line. L: the levels (nil in the claim mode), list and D: the
-- answers of score_spots (nil when the rescore was skipped), skip: why the
-- rescore was skipped (no_pill, no_target: the spot is dropped) or nil. The change
-- is the A.switches entry of this tick. Keeps A.eval_line (the viz and
-- the unit test).
local function spot_eval(A, info, now, L, list, D, skip)
  local me = info.player_number or -1
  local tx, ty = tank_tile(info)
  D = D or {}
  local o = {}
  local function kv(k, v) o[#o + 1] = k .. "=" .. tostring(v) end
  o[1] = "FOCUS_SPIKE spot_eval"
  kv("t", now)
  o[#o + 1] = "p" .. tostring(me)
  kv("id", A.id)
  kv("tank", string.format("%d,%d", tx, ty))
  kv("arm", info.armour or 0)
  kv("carried", info.carried_pills or 0)
  kv("builder", info.man_status == C.LGM_INTANK and "in" or "out")
  if L then
    local ct = {}
    for _, id in ipairs(L.ids or {}) do ct[#ct + 1] = string.format("%d:%d", id, L.c[id]) end
    kv("m", L.m ~= nil and tostring(L.m) or "none")
    kv("S", #(L.S or {}) > 0 and table.concat(L.S, ",") or "-")
    kv("c", #ct > 0 and table.concat(ct, ",") or "-")
  else
    kv("m", "-"); kv("S", "-"); kv("c", "-")
  end
  kv("skip", skip or "-")
  for _, k in ipairs({ "tiles", "not_placeable", "found", "cut", "tested", "no_stand", "unreachable",
                       "stand_risk", "route_risk", "noreach", "unchecked" }) do
    kv(k, D[k] or 0)
  end
  kv("other", (D.cut or 0) + (D.unchecked or 0))
  kv("allowed", D.allowed or 0)
  kv("risk_max", f2(C.FOCUS_SPIKE_DEFENSE_MAX))
  kv("stand_radius", D.radius or math.min(C.FOCUS_SPIKE_STAND_RADIUS or 1, C.PLACE_DISPATCH_MAX_DIST or 15))
  kv("traced", D.traced or 0)
  kv("close_pay", C.FOCUS_SPIKE_CLOSE_PAYOFF and 1 or 0)
  kv("multi_r", C.FOCUS_SPIKE_MULTI_RADIUS or 0)
  kv("multi_boost", f2(C.FOCUS_SPIKE_MULTI_BOOST or 1))
  kv("slices", D.slices or "-")
  kv("slice_ms_max", D.slice_ms_max and f2(D.slice_ms_max) or "-")
  kv("pass_ticks", D.pass_ticks or "-")
  kv("restarts", D.restarts or "-")
  local chosen = nil
  if A.mine and list then
    for _, c in ipairs(list) do
      if c.mx == A.mine.mx and c.my == A.mine.my then chosen = c; break end
    end
  end
  kv("chosen", xy(A.mine))
  if chosen then
    kv("chosen_p", eval_parts(chosen))
  elseif A.mine and A.mine.stand then
    kv("chosen_p", string.format("st:%d,%d", A.mine.stand.mx, A.mine.stand.my))
  else
    kv("chosen_p", "-")
  end
  local alts = {}
  for _, c in ipairs(list or {}) do
    if c ~= chosen and #alts < 2 then alts[#alts + 1] = c end
  end
  for i = 1, 2 do
    local c = alts[i]
    kv("alt" .. i, xy(c))
    kv("alt" .. i .. "_p", eval_parts(c))
    kv("alt" .. i .. "_margin", (c and chosen) and f2(chosen.total - c.total) or "-")
  end
  kv("switch_need", chosen and f2(C.FOCUS_SPIKE_SWITCH_MARGIN * math.max(math.abs(chosen.total), 1)) or "-")
  kv("rej", xy(D.rej))
  kv("rej_why", D.rej and D.rej.why or "-")
  kv("rej_p", eval_parts(D.rej))
  local sw = A.switches and A.switches[#A.switches]
  if sw and sw.t == now then
    local ot = sw.old_total
    local owhy = "-"
    if sw.old and D.all then
      for _, c in ipairs(D.all) do
        if c.mx == sw.old.mx and c.my == sw.old.my then
          if ot == nil and c.total then ot = c.total end
          owhy = c.why or (c.total and "allowed") or "not_tested"
          break
        end
      end
      if owhy == "-" then owhy = "not_found" end
    end
    kv("change", sw.why)
    kv("change_old", xy(sw.old))
    kv("change_old_total", ot and f2(ot) or "-")
    kv("change_old_why", owhy)
    kv("change_new", xy(sw.new))
    kv("change_new_total", sw.new_total and f2(sw.new_total) or "-")
  else
    kv("change", "-")
  end
  local s = table.concat(o, " ")
  A.eval_line = s
  print2(s)
  errline(s)
  return s
end
M.spot_eval = spot_eval

-- This bot's chosen spot changes: the plan tile list keeps the placed spots
-- and this bot's chosen one.
local function set_spot(A, info, now, c, why, old_c, state)
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
  -- FOCUS_SPIKE_ALL_AGGRO: tell the team the picked spot (FFFF = none). It
  -- counts as pending cover for the other bots.
  if state and A.picks then
    send(state, string.format("/info fss %d %s", A.id,
      c and enc_tiles({ c }) or "FFFF"))
  end
  A.switches = A.switches or {}
  A.switches[#A.switches + 1] = { t = now, why = why,
    old = old and { mx = old.mx, my = old.my } or nil, new = c and { mx = c.mx, my = c.my } or nil,
    old_total = old_c and old_c.total or nil, new_total = c and c.total or nil }
  while #A.switches > 8 do table.remove(A.switches, 1) end
  print2(string.format("FOCUS_SPIKE_ACTIVE t=%d spot id=%d %s -> %s (%s) old: %s new: %s", now, A.id,
    old and string.format("(%d,%d)", old.mx, old.my) or "none",
    c and string.format("(%d,%d)", c.mx, c.my) or "none", why, parts_text(old_c), parts_text(c)))
  errline(string.format("FOCUS_SPIKE spot t=%d p%d id=%d old=%s new=%s why=%s old_total=%s new_total=%s stand=%s new_parts=%s",
    now, me, A.id, old and string.format("%d,%d", old.mx, old.my) or "none",
    c and string.format("%d,%d", c.mx, c.my) or "none", why,
    old_c and string.format("%.2f", old_c.total) or "-", c and string.format("%.2f", c.total) or "-",
    c and string.format("%d,%d", c.stand.mx, c.stand.my) or "none",
    c and string.format("%.2f/%.2f/%.2f/%.2f/%.2f", c.parts.payoff, c.parts.stand, c.parts.route,
      c.parts.travel, c.parts.run) or "-"))
end

-- Levelling (FOCUS_SPIKE_ALL_AGGRO). For each target base b of the plan
-- (A.bases, else the claimed bases; a base that is ours now or has no known
-- place is left out): c(b) = the cover places within PILL_FIRE_RANGE (euclid,
-- inclusive), each place counted once: our live pills (cover_pills: with
-- FOCUS_SPIKE_NEUTRAL_PILL_COVER also the live neutral pills), the placed
-- spike spots of the plan, and the spots other bots picked (A.picks; this
-- bot's own pick is not counted). m = min c(b), S = the bases with c(b) = m.
-- Answers { m, S = {ids}, c = {[id] = n}, ids, list = S as {id, mx, my},
-- pos = the counted cover places {mx, my, src}, at = {[id] = base place} }
-- (m nil when there is no target base).
function M.levels(world, A, me)
  local R = C.PILL_FIRE_RANGE
  local pos, seen = {}, {}
  local function add(mx, my, src)
    local k = tkey(mx, my)
    if seen[k] then return end
    seen[k] = true
    pos[#pos + 1] = { mx = mx, my = my, src = src }
  end
  for _, p in ipairs(M.cover_pills(world, pills_of(world, false), nil)) do
    add(p.mx, p.my, p.neutral and "neutral" or "pill")
  end
  for _, t in ipairs(A.tiles or {}) do
    if t.placed then add(t.mx, t.my, "placed") end
  end
  local pk = {}
  for pn in pairs(A.picks or {}) do pk[#pk + 1] = pn end
  table.sort(pk)
  for _, pn in ipairs(pk) do
    if pn ~= me then add(A.picks[pn].mx, A.picks[pn].my, "pick") end
  end
  local src = A.bases
  if not src then
    src = {}
    for id in pairs(A.claims or {}) do src[#src + 1] = id end
  end
  local ids, dup = {}, {}
  for _, id in ipairs(src) do
    if not dup[id] then dup[id] = true; ids[#ids + 1] = id end
  end
  table.sort(ids)
  local L = { c = {}, ids = {}, S = {}, list = {}, pos = pos, at = {} }
  for _, id in ipairs(ids) do
    local b = world.bases and world.bases[id]
    if b and b.mx and not ((b.mx or 0) == 0 and (b.my or 0) == 0) and not ours(b.owner) then
      local n = 0
      for _, q in ipairs(pos) do
        if edist(q.mx, q.my, b.mx, b.my) <= R then n = n + 1 end
      end
      L.c[id] = n
      L.at[id] = { mx = b.mx, my = b.my }
      L.ids[#L.ids + 1] = id
      if L.m == nil or n < L.m then L.m = n end
    end
  end
  for _, id in ipairs(L.ids) do
    if L.c[id] == L.m then
      local b = world.bases[id]
      L.S[#L.S + 1] = id
      L.list[#L.list + 1] = { id = id, mx = b.mx, my = b.my }
    end
  end
  return L
end

-- Microseconds now (clock_us from braincore.c; os.clock in the unit tests
-- and offline checks). Log only: the slice time on the spot_eval line.
local function now_us()
  local cu = rawget(_G, "clock_us")
  if cu then return cu() end
  local o = rawget(_G, "os")
  return (o and o.clock and o.clock() or 0) * 1e6
end

-- The level key of a pass: m, S and every c(b). A pass is thrown away and
-- started again when it changes.
local function level_pass_key(L)
  local ct = {}
  for _, id in ipairs(L.ids) do ct[#ct + 1] = string.format("%d:%d", id, L.c[id]) end
  return string.format("%s|%s|%s", tostring(L.m), table.concat(L.S, ","), table.concat(ct, ","))
end

local level_step

-- One slice of the rescore pass A.pass (FOCUS_SPIKE_RESCORE_SLICES). The
-- first slice runs in the think that starts the pass. The next thinks each
-- score the next 1/N of the spots (the step A scan and the step B route of
-- those spots). Before a slice, the pass is checked against its start:
-- a new level key (m, S or a c(b)), another tank tile, or the tank in or
-- out of a boat throws the pass away and starts a new one at once (a
-- restart; the interval counts again from it). The builder out of the
-- tank or no pill carried throws it away with no new pass (level_step:
-- the builder out keeps the spot, no pill drops it). The spot of the bot
-- stays until the pass is finished; the pick, change and drop rules apply
-- only then. Answers nil.
local function level_slice(state, F, world, info, now)
  local A = F.active
  local S = A.pass
  local me = info.player_number or -1
  if S.k > 0 then
    local L = M.levels(world, A, me)
    local tx, ty = tank_tile(info)
    if info.man_status ~= C.LGM_INTANK or (info.carried_pills or 0) < 1
       or level_pass_key(L) ~= S.key or tx ~= S.tx or ty ~= S.ty
       or (info.inboat and true or false) ~= S.boat then
      -- level_step: the builder out keeps the spot, no pill drops it, else
      -- a new pass (a restart).
      A.pass = nil
      A.pass_restarts = S.restarts + 1
      return level_step(state, F, world, info, now)
    end
  end
  local t0 = now_us()
  S.k = S.k + 1
  local P = S.P
  local per = math.max(1, math.ceil(P.D.tested / S.slices))
  local done = M.spots_step(P, world, info, per)
  local list, D = nil, nil
  if done then list, D = M.spots_finish(P) end
  local ms = (now_us() - t0) / 1000
  if ms > S.ms_max then S.ms_max = ms end
  if not done then return nil end
  A.pass = nil
  D.slices, D.slice_n, D.slice_ms_max = S.slices, S.k, S.ms_max
  D.pass_ticks, D.restarts, D.pass_t0 = now - S.t0, S.restarts, S.t0
  local L, tx, ty, keep = S.L, S.tx, S.ty, S.P.keep
  local best = list[1]
  local cur = nil
  if keep then
    for _, c in ipairs(list) do if c.k == keep then cur = c; break end end
  end
  -- The spot of the bot may have changed in the pass (placed): compare with
  -- the spot now.
  local keep_now = A.mine and tkey(A.mine.mx, A.mine.my) or nil
  if keep_now ~= keep then cur = nil end
  if not A.mine then
    if best then set_spot(A, info, now, best, A.n_placed and "next" or "first", nil, state) end
  elseif not cur then
    set_spot(A, info, now, best, why_out(world, A, A.mine, L.list, "level_moved"), nil, state)
  elseif best and best.k ~= cur.k and M.should_switch(cur.total, best.total) then
    set_spot(A, info, now, best, "better", cur, state)
  else
    A.mine.stand = { mx = cur.stand.mx, my = cur.stand.my, risk = cur.stand.risk, val = cur.stand.val }
  end
  if A.mine then A.t_pick = now end
  local top = {}
  for i = 1, math.min(3, #list) do top[i] = list[i] end
  local chosen = nil
  if A.mine then
    for _, c in ipairs(list) do if c.mx == A.mine.mx and c.my == A.mine.my then chosen = c; break end end
  end
  A.score = { t = now, top = top, chosen = chosen, mine = L.S, tank = { mx = tx, my = ty }, diag = D,
              list = list }
  spot_eval(A, info, now, L, list, D, nil)
  return nil
end

-- One levelling step (FOCUS_SPIKE_ALL_AGGRO), from adapt_step. Answers nil
-- (go on), "all_laid" (no team bot carries a pill and no bot has a picked
-- spot; at least one spike pill placed by the plan) or "incomplete" (the
-- same with no pill placed, or pills are left but no bot had a picked spot
-- for FOCUS_SPIKE_RELEASE_WAIT ticks).
function level_step(state, F, world, info, now)
  local A = F.active
  local me = info.player_number or -1
  local team, per = M.team_carried(world, info)
  local laid = 0
  for _, t in ipairs(A.tiles) do if t.placed then laid = laid + 1 end end
  local picked = A.mine ~= nil
  for pn in pairs(A.picks) do if pn ~= me then picked = true end end
  if picked or A.t_pick == nil then A.t_pick = now end
  local L = M.levels(world, A, me)
  L.t, L.reserved, L.team = now, per, team
  A.level = L
  local key = string.format("%s:%s", tostring(L.m), table.concat(L.S, ","))
  if A.level_key ~= key then
    A.level_key = key
    local ct = {}
    for _, id in ipairs(L.ids) do ct[#ct + 1] = string.format("%d:%d", id, L.c[id]) end
    local rt = {}
    local pns = {}
    for pn in pairs(per) do pns[#pns + 1] = pn end
    table.sort(pns)
    for _, pn in ipairs(pns) do rt[#rt + 1] = string.format("p%d:%d", pn, per[pn]) end
    print2(string.format("FOCUS_SPIKE_ACTIVE t=%d level id=%d m=%s S=[%s] c=[%s] reserved=[%s]", now, A.id,
      tostring(L.m), table.concat(L.S, ","), table.concat(ct, " "), table.concat(rt, " ")))
    errline(string.format("FOCUS_SPIKE level t=%d p%d id=%d m=%s S=%s c=%s reserved=%s laid=%d", now, me, A.id,
      L.m and tostring(L.m) or "none", table.concat(L.S, ","), table.concat(ct, ","),
      table.concat(rt, ","), laid))
  end
  if team == 0 and not picked then
    if laid > 0 then return "all_laid" end
    return "incomplete"
  end
  if not picked and (now - A.t_pick) >= C.FOCUS_SPIKE_RELEASE_WAIT then return "incomplete" end
  -- The builder is out: keep the spot it went for.
  if info.man_status ~= C.LGM_INTANK then
    A.pass_restarts = nil
    return nil
  end
  if (info.carried_pills or 0) < 1 then
    A.pass_restarts = nil
    if A.mine then set_spot(A, info, now, nil, "no_pill", nil, state) end
    A.score = { t = now, top = {}, mine = {}, skip = "no_pill" }
    spot_eval(A, info, now, L, nil, nil, "no_pill")
    return nil
  end
  local tx, ty = tank_tile(info)
  if #L.list == 0 then
    A.pass_restarts = nil
    if A.mine then set_spot(A, info, now, nil, "no_target", nil, state) end
    A.score = { t = now, top = {}, mine = {}, tank = { mx = tx, my = ty }, skip = "no_target" }
    spot_eval(A, info, now, L, nil, nil, "no_target")
    return nil
  end
  -- A new rescore pass: the inputs of slice 1 (see level_slice).
  local keep = A.mine and tkey(A.mine.mx, A.mine.my) or nil
  A.t_rescore = now
  A.pass = { P = M.spots_begin(world, info, L.list, tx, ty, keep), key = level_pass_key(L), L = L,
             tx = tx, ty = ty, boat = info.inboat and true or false, t0 = now,
             slices = math.max(1, math.floor(C.FOCUS_SPIKE_RESCORE_SLICES or 1)), k = 0, ms_max = 0,
             restarts = A.pass_restarts or 0 }
  A.pass_restarts = nil
  return level_slice(state, F, world, info, now)
end
M.level_step = level_step

-- One adaptive step of the active plan (every FOCUS_SPIKE_RESCORE_TICKS).
-- Answers nil (go on), "all_placed" (every claimed base is covered by a
-- placed spike spot of the plan), "bases_covered" (every claimed base is
-- covered, but at least one only by another pill) or
-- "incomplete" (every base left is free for FOCUS_SPIKE_RELEASE_WAIT ticks
-- and no bot took it).
function M.adapt_step(state, F, world, info, now)
  local A = F.active
  -- A pass in work (FOCUS_SPIKE_RESCORE_SLICES > 1): its next slice.
  if A.pass then
    if A.picks and all_aggro() then return level_slice(state, F, world, info, now) end
    A.pass = nil
  end
  if A.t_rescore and (now - A.t_rescore) < C.FOCUS_SPIKE_RESCORE_TICKS then return nil end
  A.t_rescore = now
  if A.picks and all_aggro() then return level_step(state, F, world, info, now) end
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
  if all then
    -- Bug fix: a base covered only by a pill outside the plan (a teammate's
    -- normal placement, for example) is not a spike pill placed by the plan.
    local empty = {}
    for _, id in ipairs(ids) do
      local b = world.bases and world.bases[id]
      if b and b.mx and not base_covered(A, empty, b) then return "bases_covered" end
    end
    return "all_placed"
  end
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
  local list, D = M.score_spots(world, info, mine, tx, ty, keep)
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
    set_spot(A, info, now, best, why_out(world, A, A.mine, mine), nil)
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
  A.score = { t = now, top = top, chosen = chosen, mine = mids, tank = { mx = tx, my = ty }, diag = D,
              list = list }
  spot_eval(A, info, now, nil, list, D, nil)
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
      print2(string.format("FOCUS_SPIKE_ACTIVE t=%d rx placed (%d,%d) by p%d id=%d", now, mx, my, by, A.id))
      return true
    end
  end
  if not A.adaptive then return false end
  A.tiles[#A.tiles + 1] = { mx = mx, my = my, pn = by, placed = true, t_placed = now }
  if care_tag then care_tag(mx, my, now) end
  print2(string.format("FOCUS_SPIKE_ACTIVE t=%d rx placed (%d,%d) by p%d id=%d", now, mx, my, by, A.id))
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
    if BRAIN_DEBUG_MODE and v and m.from ~= me then
      local FZ = require("focus_viz")
      if v.verb == "fsy" or v.verb == "fsn" then
        FZ.note(F, "ballot", { id = v.id, pn = m.from, yes = v.verb == "fsy", pledge = v.pledge or 0,
                               eta = v.eta, reason = v.reason })
      elseif v.verb == "fsr" then
        FZ.note(F, "result", { id = v.id, pass = v.pass, why = v.reason, t = now })
      end
    end
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
          F.votes = { [v.id] = { planned = v.planned, total = v.total, cluster = v.cluster,
                                 bases = v.bases } }
          local yn, a, b = M.ballot(state, world, info, v, now)
          if BRAIN_DEBUG_MODE then
            local FZ = require("focus_viz")
            FZ.note(F, "open", { id = v.id, from = m.from, me = me, t = now, need = v.need,
                                 tiles = v.tiles, cluster = v.cluster, bases = v.bases,
                                 bots = M.team_bots(info) })
            FZ.note(F, "ballot", { id = v.id, pn = m.from, yes = true, proposer = true })
            FZ.note(F, "ballot", { id = v.id, pn = me, yes = yn == "yes",
                                   pledge = yn == "yes" and a or 0, eta = yn == "yes" and b or nil,
                                   reason = yn ~= "yes" and a or nil, armour = info.armour })
          end
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
              print2(string.format("FOCUS_SPIKE_ACTIVE t=%d rx claim id=%d base %d %s -> %s by p%d", now, A.id,
                c.b, claim_text(old), claim_text(c.pn), m.from))
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
          -- FOCUS_SPIKE_ALL_AGGRO: the pick is built; score the levels again.
          if A.picks then
            local q = A.picks[m.from]
            if q and q.mx == v.tile.mx and q.my == v.tile.my then A.picks[m.from] = nil end
            A.t_rescore = nil
          end
        end
      elseif v.verb == "fss" then
        local A = F.active
        if A and A.id == v.id and A.picks then
          A.picks[m.from] = v.tile and { mx = v.tile.mx, my = v.tile.my, t = now } or nil
          print2(string.format("FOCUS_SPIKE_ACTIVE t=%d rx pick id=%d p%d %s", now, A.id, m.from,
            v.tile and string.format("(%d,%d)", v.tile.mx, v.tile.my) or "none"))
        end
      elseif v.verb == "fsu" then
        if F.active and F.active.id == v.id and v.old then
          M.apply_repick(F.active, v.old, v.new, v.ndrop, m.from, info, now)
        end
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
  print2(string.format("FOCUS_SPIKE_VOTE t=%d result id=%d %s why=%s yes %d of %d bots (need > %d; silence = yes) pledged %d (proposer %d) need %d no=[%s] answered %d after %dt",
    now, O.id, pass and "pass" or "fail", why, yes, #O.bots, math.floor(#O.bots / 2),
    pledged, O.pledge, O.need, table.concat(reasons, ","), answered, now - O.t))
  errline(string.format("FOCUS_SPIKE result t=%d p%d id=%d %s why=%s yes=%d bots=%d pledged=%d need=%d label=%s no=%s",
    now, info.player_number or -1, O.id, pass and "pass" or "fail", why, yes, #O.bots,
    pledged, O.need, O.label, table.concat(reasons, ",")))
  if BRAIN_DEBUG_MODE then
    require("focus_viz").note(F, "result", { id = O.id, pass = pass, why = why, t = now, yes = yes,
                                             pledged = pledged, nbots = #O.bots, reasons = reasons })
  end
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
    if BRAIN_DEBUG_MODE then
      require("focus_viz").note(F, "end", { id = O.id, why = "no_assignment", t = now })
    end
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
    print2(string.format("FOCUS_SPIKE_VOTE t=%d go id=%d claims %s", now, O.id, table.concat(al, " ")))
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
  print2(string.format("FOCUS_SPIKE_VOTE t=%d go id=%d %s", now, O.id, table.concat(al, " ")))
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
  print2(string.format("FOCUS_SPIKE_ACTIVE t=%d repick id=%d (%d,%d) -> %s for p%d by p%d, %d base(s) dropped",
    now or -1, A.id, old.mx, old.my, new and string.format("(%d,%d)", new.mx, new.my) or "none",
    t.pn or -1, by, ndrop or 0))
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
    print2(string.format("FOCUS_SPIKE_ACTIVE t=%d drop base %d (%d,%d) from id=%d", now, b.id, b.mx, b.my, A.id))
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
        print2(string.format("FOCUS_SPIKE_ACTIVE t=%d placed (%d,%d) by p%d id=%d %dt after go -> spike care %dt",
          now, t.mx, t.my, t.pn, A.id, now - A.t_go, C.SPIKE_CARE_TICKS))
        if t.pn == info.player_number then
          local sd = F.sent and F.sent[tkey(t.mx, t.my)]
          local stxt = sd and string.format(" stand=(%d,%d) r=%.2f planned=%s sent_t=%d", sd.mx, sd.my, sd.r or 0,
            sd.smx and string.format("(%d,%d)", sd.smx, sd.smy) or "none", sd.t) or " stand=none"
          print2(string.format("FOCUS_SPIKE_ACTIVE t=%d placed (%d,%d) by me:%s", now, t.mx, t.my, stxt))
          errline(string.format("FOCUS_SPIKE placed t=%d p%d id=%d tile=%d,%d%s", now, t.pn, A.id, t.mx, t.my, stxt))
          -- Bug fix: tell the team at once. A bot that cannot see the new
          -- pill kept the plan on (and voted no) until the plan ttl.
          send(state, string.format("/info fsp %d %s", A.id, enc_tiles({ t })))
          if A.adaptive and A.mine == t then A.mine, A.n_placed = nil, (A.n_placed or 0) + 1 end
          if A.picks then A.t_rescore = nil end
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
    if r == "all_placed" or r == "bases_covered" or r == "all_laid" then
      local keep = {}
      for _, t in ipairs(A.tiles) do if t.placed then keep[#keep + 1] = t end end
      A.tiles, A.mine = keep, nil
      end_active(state, F, now, r, info)
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
    -- Log only: one line when the builder leaves for a plan spot, one when
    -- it lands (placed), comes back, dies or the trip times out.
    local me = info.player_number or -1
    local function trip_line(ev, why)
      local tx, ty = tank_tile(info)
      local s = string.format("FOCUS_SPIKE %s t=%d p%d id=%d to=%d,%d tank=%d,%d", ev, now, me, A.id,
        tr.mx, tr.my, tx, ty)
      if ev == "builder_out" then
        s = s .. string.format(" from=%d,%d d=%.1f", tx, ty, edist(tx, ty, tr.mx, tr.my))
      else
        s = s .. string.format(" why=%s ticks=%d from=%s", why, now - (tr.t_left or tr.t),
          tr.from and string.format("%d,%d", tr.from.mx, tr.from.my) or "-")
      end
      print2(s)
      errline(s)
    end
    if info.man_status ~= C.LGM_INTANK and not tr.left then
      local tx, ty = tank_tile(info)
      tr.from, tr.t_left = { mx = tx, my = ty }, now
      trip_line("builder_out")
    end
    if info.man_status ~= C.LGM_INTANK then tr.left = true end
    if info.man_status == C.LGM_DEAD and not tr.dead then
      tr.dead = true
      trip_line("builder_back", spike.pill_alive_at(world, tr.mx, tr.my) and "dead_after_place" or "dead")
    end
    if info.man_status == C.LGM_DEAD and M.plan_tile(state, tr.mx, tr.my)
       and not spike.pill_alive_at(world, tr.mx, tr.my) then
      F.trip = nil
      abort(state, F, info, now, "lost_pill")
      return
    end
    if tr.left and info.man_status == C.LGM_INTANK then
      if not tr.dead then
        trip_line("builder_back", spike.pill_alive_at(world, tr.mx, tr.my) and "placed" or "returned")
      end
      F.trip = nil
    elseif (now - tr.t) > (C.PLACE_TRIP_MAX_TICKS or 2000) then
      if not tr.dead then trip_line("builder_back", "timeout") end
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
  if BRAIN_DEBUG_MODE then require("focus_viz").snapshot(F, world, pills_of(world, false)) end
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
  print2(string.format("FOCUS_SPIKE_VOTE t=%d open id=%d p%d need=%d planned %d/%d label=%s bots=%d pledge=%d tiles=%s",
    now, id, me, P.need, P.planned, P.total, LABEL_TEXT[P.label], #bots, F.open.pledge, enc_tiles(P.tiles)))
  errline(string.format("FOCUS_SPIKE open t=%d p%d id=%d need=%d planned=%d total=%d label=%s bots=%d%s%s",
    now, me, id, P.need, P.planned, P.total, LABEL_TEXT[P.label], #bots,
    M.cluster_text(F.open) .. M.ncov_text(P),
    P.risk and string.format(" risk=%.2f spot=%.2f route=%.2f detour=%.2f tanks=%.2f credit=%.2f",
      P.risk.total, P.risk.spot or 0, P.risk.route or 0, P.risk.detour or 0, P.risk.tanks or 0,
      P.risk.credit or 0) or ""))
  if BRAIN_DEBUG_MODE then
    local FZ = require("focus_viz")
    FZ.note(F, "open", { id = id, from = me, me = me, t = now, need = P.need, tiles = P.tiles,
                         cluster = P.cluster, bases = P.bases, bots = bots })
    FZ.note(F, "ballot", { id = id, pn = me, yes = true, pledge = F.open.pledge, armour = info.armour })
  end
  -- A team of one bot resolves at once.
  if #bots <= 1 then resolve_open(state, F, info, now, world) end
end

return M
