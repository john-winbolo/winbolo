-- =========================================================================
-- test_pt2_c_parity.lua -- the C copy of the pill take v2 evaluator
-- (gh_pt2.evaluate, brains/GoalHunter/c/gh_pt2.c) against the Lua one
-- (pilltake_v2.lua M.evaluate_lua), 2026-11-18.
--
-- Needs the gh_pt2 global, so it runs inside the unit test exe
-- (tests/unit/test_gh_pt2_parity.c), not in a plain luajit.
--
-- Cases:
--   * N_RANDOM random small worlds (walls, half walls, forest, boats,
--     water, bases, hostile / neutral / allied pills, dead pills, a pill
--     in a tank, chain skips, banned angles) with random knobs;
--   * the cases in PT2_CASES_FILE (when set): inputs recorded in a real
--     game by pt2_capture.lua (PT2_CAPTURE_FILE).
-- Each case runs the Lua copy, then the C copy, on the same inp. They must
-- give:
--   * the same result, field by field (numbers exactly, no tolerance),
--     with the same table sharing (a table shared by two places in one
--     result is shared in the other too);
--   * the same calls, in the same order, to every inp function (in_map,
--     stay_in, standable, buildable, lgm_reach, score_fn, banned,
--     anger_of) and to env.base_stops on a square with a base;
--   * the same terrain reads: the same count (the C adds its cache hits
--     through env.count_reads) and the same order of first reads per
--     square; the same order of first pill_on reads per square;
--   * the same env.omx / env.omy at the end.
-- Returns n_cases, n_bad, first_failure, coverage (a string).
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 and not rawget(_G, "_PT2_ATAN_DONE") then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
  _G._PT2_ATAN_DONE = true
end

local C   = require("constants")
local U   = require("util")
local cpf = require("cpathfinder")
local PC  = require("pill_cover")
_G.print2 = _G.print2 or function() end
local V2  = require("pilltake_v2")
local CAP = require("pt2_capture")

local G = rawget(_G, "gh_pt2")
if not G then
  print("test_pt2_c_parity: no gh_pt2 global (plain luajit): skipped")
  return 0, 0, "", "skipped"
end

local N_RANDOM = tonumber(rawget(_G, "PT2_N_RANDOM") or 300)

-- ── deterministic random numbers (not math.random: same on every build) ──
local seed = 12345
local function rnd(n)                 -- 0 .. n-1
  seed = (seed * 1103515245 + 12345) % 2147483648
  return math.floor(seed / 65536) % n
end

-- ── the compare ─────────────────────────────────────────────────────────
local function same_num(a, b) return a == b or (a ~= a and b ~= b) end

-- inputs: tables both results hold by reference (pills, inp); they must be
-- the very same table.
local function compare(a, b, inputs)
  local amap, bmap = {}, {}
  local function cmp(x, y, path)
    local tx, ty = type(x), type(y)
    if tx ~= ty then return path .. ": type " .. tx .. " vs " .. ty end
    if tx == "number" then
      if not same_num(x, y) then return string.format("%s: %.17g vs %.17g", path, x, y) end
      return nil
    end
    if tx ~= "table" then
      if x ~= y then return path .. ": " .. tostring(x) .. " vs " .. tostring(y) end
      return nil
    end
    if inputs[x] or inputs[y] then
      if not rawequal(x, y) then return path .. ": not the same input table" end
      return nil
    end
    if amap[x] ~= nil or bmap[y] ~= nil then
      if amap[x] ~= y or bmap[y] ~= x then return path .. ": table sharing differs" end
      return nil
    end
    amap[x], bmap[y] = y, x
    for k, v in pairs(x) do
      if type(k) == "table" and not inputs[k] then return path .. ": a non-input table key" end
      local w = rawget(y, k)
      if w == nil then return path .. "." .. tostring(k) .. ": missing in C" end
      local e = cmp(v, w, path .. "." .. tostring(k))
      if e then return e end
    end
    for k in pairs(y) do
      if rawget(x, k) == nil then return path .. "." .. tostring(k) .. ": only in C" end
    end
    return nil
  end
  return cmp(a, b, "res")
end

-- ── call logs ───────────────────────────────────────────────────────────
-- Wrap inp's functions so each call goes into the current log.
local LOG
local function instrument(inp)
  local env = inp.env
  local function wrap(tab, name, tag)
    local f = tab[name]
    if not f then return end
    tab[name] = function(...)
      local r = f(...)
      local t = { ... }
      for i = 1, #t do
        if type(t[i]) == "number" then t[i] = string.format("%.17g", t[i])
        elseif type(t[i]) == "table" then t[i] = "pill" .. tostring(LOG.pid[t[i]] or "?")
        else t[i] = tostring(t[i]) end
      end
      LOG.calls[#LOG.calls + 1] = tag .. "(" .. table.concat(t, ",") .. ")=" .. tostring(r)
      return r
    end
  end
  for _, n in ipairs({ "in_map", "stay_in", "standable", "buildable", "lgm_reach",
                       "score_fn", "banned", "anger_of" }) do
    wrap(inp, n, n)
  end
  local bs = env.base_stops
  if bs then
    env.base_stops = function(mx, my)
      local r = bs(mx, my)
      if not env.base_at or env.base_at[my * 256 + mx] then
        LOG.calls[#LOG.calls + 1] = string.format("base_stops(%d,%d,%s,%s)=%s", mx, my,
          tostring(env.omx), tostring(env.omy), tostring(r))
      end
      return r
    end
  end
  local tt = env.ttype
  env.ttype = function(mx, my)
    LOG.tt_n = LOG.tt_n + 1
    local k = my * 256 + mx
    if not LOG.tt_seen[k] then
      LOG.tt_seen[k] = true
      LOG.tt_first[#LOG.tt_first + 1] = k
    end
    return tt(mx, my)
  end
  env.count_reads = function(n) LOG.tt_n = LOG.tt_n + n end
  local po = env.pill_on
  env.pill_on = function(mx, my)
    local k = my * 256 + mx
    if not LOG.po_seen[k] then
      LOG.po_seen[k] = true
      LOG.po_first[#LOG.po_first + 1] = k
    end
    return po(mx, my)
  end
end

local function new_log(pid)
  return { calls = {}, tt_n = 0, tt_seen = {}, tt_first = {}, po_seen = {}, po_first = {}, pid = pid }
end

local function cmp_list(a, b, what)
  local n = math.max(#a, #b)
  for i = 1, n do
    if a[i] ~= b[i] then
      return string.format("%s #%d: Lua %s, C %s (Lua %d, C %d entries)", what, i,
                           tostring(a[i]), tostring(b[i]), #a, #b)
    end
  end
  return nil
end

local cov = { cases = 0, best = 0, safe = 0, chain = 0, plan = 0, nudge = 0, hold = 0,
              hold_unsafe = 0, leeway = 0, reject = 0, finish = 0, why_unsafe = 0,
              ally_cover = 0, unsearched = 0 }

local function coverage(res)
  local b = res.best
  if b then
    cov.best = cov.best + 1
    if b.safe then cov.safe = cov.safe + 1 end
    if b.chain_mode then cov.chain = cov.chain + 1 end
    if b.plan and #b.plan > 0 then cov.plan = cov.plan + 1 end
    if b.hold then
      cov.hold = cov.hold + 1
      if not b.hold.safe then cov.hold_unsafe = cov.hold_unsafe + 1 end
    end
    if b.finish then cov.finish = cov.finish + 1 end
    if b.ally_cover then cov.ally_cover = cov.ally_cover + 1 end
  end
  for _, r in ipairs(res.all or {}) do
    if r.nudge_wu and r.nudge_wu > 0 then cov.nudge = cov.nudge + 1 end
    if r.leeway then cov.leeway = cov.leeway + 1 end
    if r.rejected then cov.reject = cov.reject + 1 end
    if r.why_unsafe then cov.why_unsafe = cov.why_unsafe + 1 end
    if r.unsearched then cov.unsearched = cov.unsearched + 1 end
  end
end

-- Run both copies on inp. Returns nil or the first difference.
local function run_case(inp, pills)
  local inputs = { [inp] = true }
  local pid = {}
  for k, p in ipairs(pills) do inputs[p] = true; pid[p] = k end
  inp.capture_file = nil
  inp.c_eval = true
  inp.env.omx, inp.env.omy = nil, nil
  LOG = new_log(pid)
  local ok1, r1 = pcall(V2.evaluate_lua, inp)
  local L1 = LOG
  local om1x, om1y = inp.env.omx, inp.env.omy
  inp.env.omx, inp.env.omy = nil, nil
  LOG = new_log(pid)
  local ok2, r2 = pcall(G.evaluate, inp, V2)
  local L2 = LOG
  LOG = nil
  if not ok1 or not ok2 then
    if ok1 ~= ok2 then return "Lua ok=" .. tostring(ok1) .. " C ok=" .. tostring(ok2) .. ": " .. tostring(ok1 and r2 or r1) end
    return nil   -- both raise (a bad input): not a parity case
  end
  cov.cases = cov.cases + 1
  coverage(r1)
  local e = compare(r1, r2, inputs)
  if e then return e end
  e = cmp_list(L1.calls, L2.calls, "call") if e then return e end
  if L1.tt_n ~= L2.tt_n then return string.format("terrain reads: Lua %d, C %d", L1.tt_n, L2.tt_n) end
  e = cmp_list(L1.tt_first, L2.tt_first, "first terrain read") if e then return e end
  e = cmp_list(L1.po_first, L2.po_first, "first pill_on read") if e then return e end
  if not same_num(om1x or -1, inp.env.omx or -1) or not same_num(om1y or -1, inp.env.omy or -1) then
    return string.format("env.omx/omy: Lua %s,%s C %s,%s", tostring(om1x), tostring(om1y),
                         tostring(inp.env.omx), tostring(inp.env.omy))
  end
  return nil
end

-- ── random worlds ───────────────────────────────────────────────────────
local terrain = {}
local base_rule = 0
local function sqk(mx, my) return my * 256 + mx end
U.ttype      = function(mx, my) return terrain[sqk(mx, my)] or C.T_GRASS end
U.ttype_peek = U.ttype
U.base_tile_stops_tank_shell = function(world, mx, my, omx, omy)
  local ba = world and world.base_at
  if not (ba and ba[my * 256 + mx]) then return false end
  return (mx * 7 + my * 3 + omx * 5 + omy + base_rule) % 3 ~= 0
end
cpf.lgm_travel_ticks_map = function(omx, omy, mx, my)
  return ((mx * 5 + my * 11 + omx + omy * 3) % 7 == 0) and -1 or 10
end

local TT = { C.T_BUILDING, C.T_HALFBUILD, C.T_FOREST, C.T_BOAT, C.T_RIVER, C.T_DEEPSEA,
             C.T_SWAMP, C.T_ROAD }
local OWNERS = { "hostile", "hostile", "hostile", "neutral", "friendly", "allied" }

local function random_case(ci)
  for k in pairs(terrain) do terrain[k] = nil end
  base_rule = rnd(3)
  local cx, cy = 100 + rnd(56), 100 + rnd(56)
  -- terrain: a few blobs around the pills
  local density = 3 + rnd(12)          -- percent
  for my = cy - 14, cy + 14 do
    for mx = cx - 14, cx + 14 do
      if rnd(100) < density then terrain[sqk(mx, my)] = TT[1 + rnd(#TT)] end
    end
  end
  -- walls in a line now and then
  if rnd(2) == 0 then
    local wx, wy = cx - 3 + rnd(7), cy - 3 + rnd(7)
    local dx, dy = rnd(2), 1 - rnd(2)
    for i = 0, 2 + rnd(4) do terrain[sqk(wx + dx * i, wy + dy * i)] = C.T_BUILDING end
  end
  local world = { pills = {}, pill_at = {}, bases = {}, base_at = {} }
  local pills = {}
  local npill = 1 + rnd(6)
  local used = {}
  for i = 1, npill do
    local mx, my
    repeat
      if i == 1 then mx, my = cx, cy else mx, my = cx - 6 + rnd(13), cy - 6 + rnd(13) end
    until not used[sqk(mx, my)]
    used[sqk(mx, my)] = true
    terrain[sqk(mx, my)] = nil
    local hp = rnd(4) == 0 and 1 or (rnd(8) == 0 and 0 or 1 + rnd(15))
    local p = { mx = mx, my = my, owner = (i == 1) and "hostile" or OWNERS[1 + rnd(#OWNERS)],
                health = hp, anger = rnd(100) / 100 }
    if i > 1 and rnd(20) == 0 then p.in_tank = true end
    pills[i] = p
    world.pills[i] = p
    world.pill_at[sqk(mx, my)] = { { id = i, pill = p } }
  end
  -- bases
  for _ = 1, rnd(4) do
    local bx, by = cx - 8 + rnd(17), cy - 8 + rnd(17)
    if not used[sqk(bx, by)] then
      world.base_at[sqk(bx, by)] = { base = { owner = "hostile", health = 5 } }
    end
  end
  local inp = V2.knob_inp(C, {})
  inp = V2.world_inp(world, C, U, PC, cpf, inp, rnd(10) == 0)
  if rnd(5) == 0 then inp.env.base_at = nil end
  inp.A = pills[1]
  inp.radius = 5 + rnd(4) + rnd(4) * 0.25
  inp.num_pill_blockers = rnd(3) == 0 and 1 or 0
  inp.chains = rnd(4) ~= 0
  inp.link_covers = rnd(3) ~= 0
  inp.leeway_on = rnd(4) ~= 0
  inp.aim_nudge = rnd(3) ~= 0
  inp.orth_only = rnd(6) == 0
  inp.own_pill_rank = rnd(4) ~= 0
  inp.travel_band = ({ 0, 6.25, 6.25, 3 })[1 + rnd(4)]
  if rnd(6) == 0 then inp.travel_band = nil end
  inp.finish_max_anger = (rnd(3) == 0) and -1 or 0.95
  inp.armour = rnd(40)
  inp.keep_all = rnd(3) ~= 0
  inp.angle_step = ({ 2, 2, 5, 10 })[1 + rnd(4)]
  inp.build_max = 1 + rnd(4)
  if rnd(4) == 0 then inp.search_top = 1 + rnd(4) end
  if rnd(4) == 0 then inp.leeway_top = 1 + rnd(3) end
  if rnd(5) == 0 then inp.radii_offsets = { 0 } end
  if rnd(4) == 0 then
    inp.chain_skip = {}
    for i = 2, npill do if rnd(3) == 0 then inp.chain_skip[pills[i]] = true end end
  end
  if rnd(4) == 0 then
    local b0 = rnd(360)
    inp.banned = function(deg) return (deg - b0) % 360 < 40 end
  end
  if rnd(3) == 0 then
    inp.anger_of = function(p) return (p.anger or 0) * 0.5 end
  end
  local hx, hy = cx + 0.5 - 6 + rnd(13), cy + 0.5 + 8
  inp.score_fn = function(deg, mx, my, x, y)
    local dx, dy = x - hx, y - hy
    return math.sqrt(dx * dx + dy * dy) + (mx % 3) * 0.125
  end
  instrument(inp)
  return inp, pills
end

local n, bad, first = 0, 0, nil
for ci = 1, N_RANDOM do
  local inp, pills = random_case(ci)
  n = n + 1
  local e = run_case(inp, pills)
  if e then
    bad = bad + 1
    first = first or ("random case " .. ci .. ": " .. e)
  end
end

-- ── recorded cases ──────────────────────────────────────────────────────
local cases_file = rawget(_G, "PT2_CASES_FILE")
local n_rec = 0
if cases_file and cases_file ~= "" then
  local f = io.open(cases_file, "r")
  if not f then
    bad = bad + 1
    first = first or ("cannot open " .. cases_file)
  else
    f:close()
    local envt = setmetatable({ CASES = {} }, { __index = _G })
    local chunk = assert(loadfile(cases_file))
    setfenv(chunk, envt)
    chunk()
    for ci, case in ipairs(envt.CASES) do
      local inp, pills = CAP.replay(case, V2)
      instrument(inp)
      n = n + 1; n_rec = n_rec + 1
      local e = run_case(inp, pills)
      if e then
        bad = bad + 1
        first = first or ("recorded case " .. ci .. ": " .. e)
      end
    end
  end
end

local cv = {}
for _, k in ipairs({ "cases", "best", "safe", "chain", "plan", "nudge", "hold", "hold_unsafe",
                     "leeway", "reject", "finish", "why_unsafe", "ally_cover", "unsearched" }) do
  cv[#cv + 1] = k .. "=" .. cov[k]
end
local cvs = string.format("%d random + %d recorded; %s", N_RANDOM, n_rec, table.concat(cv, " "))
print("test_pt2_c_parity: " .. n .. " cases, " .. bad .. " bad; " .. cvs)
if first then print("  first: " .. first) end
return n, bad, first or "", cvs
