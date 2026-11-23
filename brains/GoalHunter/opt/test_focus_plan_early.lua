-- =========================================================================
-- test_focus_plan_early.lua — standalone unit tests (2026-11-23) for the
-- deny plan's early exit (focus_vote.lua M.plan, M.plan_ub). The early exit
-- must give the same answer as the full plan:
--   * old_plan below is M.plan as it was before the early exit (cc39d6ce7);
--   * fixed worlds and 400 seeded random worlds (bases, owners, positionless
--     bases, hostile / neutral / our pills, forest and sea, K 0..7, tank
--     sightings, neutral cover on and off, cluster gap 9 and 16) run through
--     both, each on its own fresh state;
--   * a full new answer must equal the old one field by field (rows too);
--   * an early answer must have the old pass, why, total, nopos, K, team,
--     ftgt and ncov; on a "frac" stop the old planned must be at or under
--     its bound ub; its plan line says early=true;
--   * M.plan_ub on hand cases;
--   * the time per call, old and new, warm (one state, the route cache
--     kept, as in a game) and cold (a fresh state each call).
-- No engine: terrain is a table, PP counts and team pills are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_focus_plan_early.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C      = require("constants")
local U      = require("util")
local PP     = require("pill_portfolio")
local SP     = require("spike")
local FV     = require("focus_vote")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end

-- ── Stand-ins (as test_focus_vote.lua) ───────────────────────────────────
local terrain = {}
local function T(x, y) return terrain[y * 256 + x] or C.T_GRASS end
U.ttype = T
U.ttype_peek = T
U.is_placeable_peek = function(x, y, world)
  local tt = T(x, y)
  if not (tt == C.T_GRASS or tt == C.T_ROAD or tt == C.T_FOREST) then return false end
  local k = y * C.MAP_W + x
  if world.pill_at[k] or world.base_at[k] then return false end
  return true
end
local TEAM, FRONT = 10, 8
SP.team_pills = function() return TEAM, 0, 0, 0 end
PP.normal_targets = function() return { back = 0, front = FRONT, aggro = 0, util = 0 } end
PP.counts = function() return { back = 0, front = 0, aggro = 0, utility = 0 } end

C.FOCUS_SPIKE_ENABLED = true
C.FOCUS_SPIKE_RULE    = "deny"
C.PILL_PLACE_TURTLE   = false

-- ── The old plan (focus_vote.lua M.plan at cc39d6ce7, deny rule) ─────────
local function edist(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  return math.sqrt(dx * dx + dy * dy)
end
local function old_plan(state, world, info, now)
  local M = FV
  local pills_of, st, before = FV.pills_of, FV.st, FV.before
  local list, total, nopos = M.bases(world)
  local hp = pills_of(world, true)
  local op = pills_of(world, false)
  local cvp, ncov = M.cover_pills(world, op, list)
  local pf = PP.counts(world, now, true)
  local spike = require("spike")
  local team = spike.team_pills(world, info, pf)
  local ftgt = PP.normal_targets(team).front
  local K = team - ftgt
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
  local rows = {}
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

-- Deep equal by value; answers true or the path of the first difference.
local function deq(a, b, path, seen)
  path = path or "P"
  if type(a) ~= type(b) then return path .. " type" end
  if type(a) ~= "table" then
    if a == b then return true end
    if type(a) == "number" and a ~= a and b ~= b then return true end
    return path .. " " .. tostring(a) .. " vs " .. tostring(b)
  end
  seen = seen or {}
  if seen[a] == b then return true end
  seen[a] = b
  for k, v in pairs(a) do
    local r = deq(v, b[k], path .. "." .. tostring(k), seen)
    if r ~= true then return r end
  end
  for k in pairs(b) do
    if a[k] == nil then return path .. "." .. tostring(k) .. " only in new" end
  end
  return true
end

-- ── Worlds ───────────────────────────────────────────────────────────────
local function new_world()
  return { bases = {}, pills = {}, pill_at = {}, base_at = {}, _bases_on_map = 0 }
end
local function add_base(w, id, mx, my, owner)
  w.bases[id] = { mx = mx, my = my, owner = owner }
  if mx ~= 0 or my ~= 0 then w.base_at[my * C.MAP_W + mx] = { id = id } end
  w._bases_on_map = w._bases_on_map + 2 ^ id
end
local function add_pill(w, id, mx, my, owner, hp)
  w.pills[id] = { mx = mx, my = my, owner = owner, health = hp or 15, in_tank = false }
  w.pill_at[my * C.MAP_W + mx] = { id = id }
end
local function info_at(mx, my)
  return { player_number = 0, allies = 0, player_bots = 0,
           tankx = mx * 256 + 128, tanky = my * 256 + 128,
           carried_pills = 1, man_status = C.LGM_INTANK, inboat = false, trees = 40 }
end
local function new_state(tick) return { tick = tick or 1000, perc = { enemy_tanks = {} } } end

-- A seeded random world: { w, terr, K, ncov, gap, seen }.
local function rand_case(seed)
  math.randomseed(seed)
  local rnd = math.random
  local terr = {}
  for _ = 1, rnd(0, 6) do   -- forest blobs
    local cx, cy, r = rnd(40, 200), rnd(40, 200), rnd(2, 9)
    for y = cy - r, cy + r do for x = cx - r, cx + r do terr[y * 256 + x] = C.T_FOREST end end
  end
  for _ = 1, rnd(0, 3) do   -- sea bands with a gap
    local y, g0 = rnd(40, 200), rnd(30, 200)
    for x = 20, 235 do if x < g0 or x > g0 + rnd(4, 30) then terr[y * 256 + x] = C.T_DEEPSEA end end
  end
  local w = new_world()
  local nb = rnd(1, 16)
  local cx, cy = rnd(60, 190), rnd(60, 190)
  local owners = { "hostile", "neutral", "friendly", "allied", "hostile", "neutral" }
  local used = {}
  for id = 0, nb - 1 do
    local mx, my
    repeat
      mx, my = cx + rnd(-50, 50), cy + rnd(-50, 50)
    until not used[my * 256 + mx]
    used[my * 256 + mx] = true
    terr[my * 256 + mx] = nil
    if rnd() < 0.08 then mx, my = 0, 0 end
    add_base(w, id, mx, my, owners[rnd(1, #owners)])
  end
  if rnd() < 0.2 then w._bases_on_map = w._bases_on_map + 2 ^ 20 end
  for id = 1, rnd(0, 14) do
    local mx, my
    repeat
      mx, my = cx + rnd(-55, 55), cy + rnd(-55, 55)
    until not used[my * 256 + mx]
    used[my * 256 + mx] = true
    local o = ({ "hostile", "hostile", "neutral", "friendly" })[rnd(1, 4)]
    add_pill(w, id, mx, my, o, rnd(1, 15))
  end
  local seen = {}
  for i = 1, rnd(0, 3) do seen[i] = { mx = cx + rnd(-40, 40), my = cy + rnd(-40, 40), t = 1000 - rnd(0, 400) } end
  return { w = w, terr = terr, K = rnd(-1, 7), ncov = rnd() < 0.5, gap = (rnd() < 0.5) and 9 or 16,
           seen = seen, tank = { cx + rnd(-30, 30), cy + rnd(-30, 30) } }
end

local function fresh(case)
  local s = new_state(1000)
  local F = FV.st(s)
  for k, v in pairs(case.seen or {}) do F.seen[k] = { mx = v.mx, my = v.my, t = v.t } end
  return s
end

local function set_case(case)
  terrain = case.terr
  T = function(x, y) return terrain[y * 256 + x] or C.T_GRASS end
  U.ttype, U.ttype_peek = T, T
  FRONT = TEAM - case.K
  C.FOCUS_SPIKE_NEUTRAL_PILL_COVER = case.ncov
  C.FOCUS_SPIKE_CLUSTER_GAP = case.gap
end

local SAME_FIELDS = { "pass", "why", "total", "nopos", "K", "team", "ftgt" }
local stats = { full = 0, early = {}, pass = 0, fail = 0 }
local function compare(name, case, verbose)
  set_case(case)
  local info = info_at(case.tank[1], case.tank[2])
  local O = old_plan(fresh(case), case.w, info, 1000)
  local N = FV.plan(fresh(case), case.w, info, 1000)
  if N.pass then stats.pass = stats.pass + 1 else stats.fail = stats.fail + 1 end
  if N.early then
    stats.early[N.early] = (stats.early[N.early] or 0) + 1
    local ok, why = true, ""
    for _, f in ipairs(SAME_FIELDS) do
      if O[f] ~= N[f] then ok, why = false, f .. " " .. tostring(O[f]) .. " vs " .. tostring(N[f]) end
    end
    local r = deq(O.ncov, N.ncov, "ncov")
    if r ~= true then ok, why = false, r end
    -- the bound is made only on a "frac" stop (no_targets / no_spare stop
    -- before it; the old planned may count need-0 clusters there)
    if N.why == "frac" and O.planned > (N.ub or -1) then
      ok, why = false, "planned " .. O.planned .. " > ub " .. tostring(N.ub)
    end
    if N.early == "start" and N.ub ~= nil then ok, why = false, "start with a bound" end
    if not FV.plan_line(1000, N):find("early=true", 1, true) then ok, why = false, "plan line" end
    if not ok or verbose then check(name .. " (early " .. N.early .. ", same answer)", ok, why) end
    return ok
  end
  stats.full = stats.full + 1
  local r = deq(O, N)
  if r ~= true or verbose then check(name .. " (full plan, equal)", r == true, r) end
  return r == true
end

-- ── M.plan_ub ────────────────────────────────────────────────────────────
print("plan_ub")
check("need 0 rows count in full", FV.plan_ub({ { nb = 3, need = 0 }, { nb = 2, need = 0 } }, 0) == 5)
check("need over cap is out", FV.plan_ub({ { nb = 9, need = 4 } }, 3) == 0)
check("best nb per tile first", FV.plan_ub({ { nb = 2, need = 2 }, { nb = 3, need = 1 } }, 1) == 3)
check("a part of the last row (3 + 4 x 1/2 = 5)", FV.plan_ub({ { nb = 3, need = 1 }, { nb = 4, need = 2 } }, 2) == 5)
check("whole numbers: 1 + 1 x 2/3 -> 1", FV.plan_ub({ { nb = 1, need = 1 }, { nb = 1, need = 3 } }, 3) == 1)
check("no rows: 0", FV.plan_ub({}, 5) == 0)

-- ── Fixed cases ──────────────────────────────────────────────────────────
print("fixed worlds")
local QUAD = { { 40, 40 }, { 140, 40 }, { 40, 140 }, { 140, 140 } }
local function chew(owner_of)
  local w = new_world()
  local id = 0
  for q, o in ipairs(QUAD) do
    for j = 0, 3 do
      add_base(w, id, o[1] + (j % 2) * 12, o[2] + math.floor(j / 2) * 12, owner_of(q, j))
      id = id + 1
    end
  end
  return w
end
local fixed = {
  { "we hold SW, K 2: frac fail", chew(function(q) return q == 3 and "friendly" or "neutral" end), 2 },
  { "top half theirs, K 2: pass", chew(function(q) return q <= 2 and "hostile" or "friendly" end), 2 },
  { "top half theirs, K 4: pass", chew(function(q) return q <= 2 and "hostile" or "friendly" end), 4 },
  { "one quadrant theirs, K 1: fail", chew(function(q) return q == 2 and "hostile" or "friendly" end), 1 },
  { "one quadrant theirs, K 0: no_spare", chew(function(q) return q == 2 and "hostile" or "friendly" end), 0 },
  { "all ours: no_targets", chew(function() return "friendly" end), 3 },
  { "one base each theirs, K 3: pass", chew(function(_, j) return j == 0 and "hostile" or "friendly" end), 3 },
  { "one base each theirs, K 1: fail", chew(function(_, j) return j == 0 and "hostile" or "friendly" end), 1 },
}
for _, f in ipairs(fixed) do
  compare(f[1], { w = f[2], terr = {}, K = f[3], ncov = false, gap = 16, seen = {}, tank = { 46, 146 } }, true)
end
-- A pill line: the only cluster is defended (early at the defense step).
do
  local terr = {}
  for x = 0, 255 do if x < 86 or x > 118 then terr[80 * 256 + x] = C.T_DEEPSEA end end
  local w = new_world()
  add_base(w, 0, 96, 40, "hostile"); add_base(w, 1, 108, 40, "hostile"); add_base(w, 2, 102, 120, "friendly")
  for i, x in ipairs({ 94, 102, 110 }) do add_pill(w, 10 + i, x, 80, "hostile", 15) end
  compare("pill line, defended", { w = w, terr = terr, K = 2, ncov = false, gap = 16, seen = {}, tank = { 102, 120 } }, true)
end

-- ── Random worlds ────────────────────────────────────────────────────────
print("random worlds")
local N_RAND = 400
local bad = 0
local cases = {}
for seed = 1, N_RAND do
  local case = rand_case(seed)
  cases[#cases + 1] = case
  if not compare("seed " .. seed, case, false) then bad = bad + 1 end
end
check(string.format("%d random worlds: every answer the same", N_RAND), bad == 0, bad)
local et = {}
for k, v in pairs(stats.early) do et[#et + 1] = k .. "=" .. v end
table.sort(et)
print(string.format("  answers: pass %d, fail %d; full plans %d, early %s",
  stats.pass, stats.fail, stats.full, table.concat(et, " ")))
check("the random worlds reach every early step and full plans",
  (stats.early.start or 0) > 0 and (stats.early.bases or 0) > 0 and (stats.early.cover or 0) > 0
  and (stats.early.defense or 0) > 0 and stats.full > 0 and stats.pass > 0, table.concat(et, " "))

-- ── Time per call ────────────────────────────────────────────────────────
-- The fail cases only (the plans the early exit is for), then all cases.
print("time per call (os.clock)")
local function time_of(fn, list, warm, reps)
  local states = {}
  for i, case in ipairs(list) do states[i] = fresh(case) end
  local t0 = os.clock()
  for _ = 1, reps do
    for i, case in ipairs(list) do
      set_case(case)
      local s = warm and states[i] or fresh(case)
      fn(s, case.w, info_at(case.tank[1], case.tank[2]), 1000)
    end
  end
  return (os.clock() - t0) * 1000 / (reps * #list)
end
local fails, frac_fails = {}, {}
for _, case in ipairs(cases) do
  set_case(case)
  local P = old_plan(fresh(case), case.w, info_at(case.tank[1], case.tank[2]), 1000)
  if not P.pass then fails[#fails + 1] = case end
  if P.why == "frac" then frac_fails[#frac_fails + 1] = case end
end
local REPS = 3
for _, grp in ipairs({ { "frac fails", frac_fails }, { "all fails", fails }, { "all worlds", cases } }) do
  for _, warm in ipairs({ true, false }) do
    local to = time_of(old_plan, grp[2], warm, REPS)
    local tn = time_of(FV.plan, grp[2], warm, REPS)
    print(string.format("  %-10s %-4s n=%3d  old %.3f ms  new %.3f ms  saved %.3f ms (%.0f%%)",
      grp[1], warm and "warm" or "cold", #grp[2], to, tn, to - tn, to > 0 and (to - tn) * 100 / to or 0))
  end
end
check("frac fail worlds to time", #frac_fails > 0, #frac_fails)

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
