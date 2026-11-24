-- =========================================================================
-- test_pt2_v2_c_parity.lua -- the C copy of pill take v2 (gh_pt2_v2,
-- brains/GoalHunter/c/gh_pt2_v2.c) against the Lua one (pilltake_v2.lua
-- M.evaluate_lua), 2026-11-24.
--
-- Needs the gh_pt2_v2 global, so it runs inside the unit test exe
-- (tests/unit/test_gh_pt2_v2_parity.c), not in a plain luajit.
--
-- Cases:
--   * N_RANDOM random small worlds (as test_pt2_old_c_parity.lua) with
--     random v2 knobs (aim points, prefer existing cover, start while
--     building, anger rise, extra cover walls, chains only, wall memo,
--     aim avoid chain neighbour, farthest ring and its ring radii);
--   * N_LIVE random worlds with non-classic shell rules in env;
--   * N_BLOCK random worlds with env.ts_boats and env.ts_tanks;
--   * the cases in PT2_CASES_FILE (when set), recorded by pt2_capture.lua.
-- Each case runs the Lua copy, then the C copy, on the same inp: with no
-- inp.c_std (the C calls inp's square tests) and with it (the C does them).
-- They must give:
--   * the same result, field by field (numbers exactly), with the same
--     table sharing; the Lua's own scratch fields (wall_base, slot_memo,
--     mask_chain) are not compared;
--   * the same calls, in the same order, to every inp function and to
--     env.base_stops on a square with a base;
--   * the same terrain reads (count and order of first reads per square)
--     and the same order of first pill_on reads;
--   * the same env.omx / env.omy at the end.
-- With the wall memo on, each copy runs twice from an empty memo (the
-- second run hits it); both runs are compared.
-- Split (PT2_V2_SPLIT): every N_SPLIT-th case also runs both copies in a
-- coroutine with pilltake_v2.slicer over 1 to 7 ticks; the result, the
-- calls, the yields and the slicer's record (slices, wu, over, budget)
-- must be the same.
-- Group: every N_GROUP-th case with chains runs pilltake_v2.evaluate_group
-- with PT2_V2_C off and on; the results must be the same.
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
local OLD = require("pilltake_v2_old")
local V2  = require("pilltake_v2")
local CAP = require("pt2_capture")

local G = rawget(_G, "gh_pt2_v2")
if not G then
  print("test_pt2_v2_c_parity: no gh_pt2_v2 global (plain luajit): skipped")
  return 0, 0, "", "skipped"
end

local N_RANDOM = tonumber(rawget(_G, "PT2_N_RANDOM") or 300)
local N_LIVE   = tonumber(rawget(_G, "PT2_N_LIVE") or 100)
local N_BLOCK  = tonumber(rawget(_G, "PT2_N_BLOCK") or 100)
local N_SPLIT  = tonumber(rawget(_G, "PT2_N_SPLIT") or 3)
local N_GROUP  = tonumber(rawget(_G, "PT2_N_GROUP") or 4)

-- ── deterministic random numbers ────────────────────────────────────────
local seed = 24680
local function rnd(n)                 -- 0 .. n-1
  seed = (seed * 1103515245 + 12345) % 2147483648
  return math.floor(seed / 65536) % n
end

-- ── the compare ─────────────────────────────────────────────────────────
local function same_num(a, b) return a == b or (a ~= a and b ~= b) end
local SKIP = { wall_base = true, slot_memo = true, mask_chain = true }

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
      if not SKIP[k] then
        if type(k) == "table" and not inputs[k] then return path .. ": a non-input table key" end
        local w = rawget(y, k)
        if w == nil then return path .. "." .. tostring(k) .. ": missing in C" end
        local e = cmp(v, w, path .. "." .. tostring(k))
        if e then return e end
      end
    end
    for k in pairs(y) do
      if not SKIP[k] and rawget(x, k) == nil then return path .. "." .. tostring(k) .. ": only in C" end
    end
    return nil
  end
  return cmp(a, b, "res")
end

-- ── call logs ───────────────────────────────────────────────────────────
local LOG
local SQ_TESTS = { "in_map", "stay_in", "standable", "buildable" }
local SQ_TEST = { in_map = true, stay_in = true, standable = true, buildable = true }
local function note_tt(mx, my)
  LOG.tt_n = LOG.tt_n + 1
  local k = my * 256 + mx
  if not LOG.tt_seen[k] then
    LOG.tt_seen[k] = true
    LOG.tt_first[#LOG.tt_first + 1] = k
  end
end
local function instrument(inp)
  local env = inp.env
  inp._orig, inp._wrap = {}, {}
  local function wrap(tab, name, tag)
    local f = tab[name]
    if not f then return end
    if tab == inp then inp._orig[name] = f end
    tab[name] = function(...)
      local r = f(...)
      if LOG then
        local t = { ... }
        for i = 1, #t do
          if type(t[i]) == "number" then t[i] = string.format("%.17g", t[i])
          elseif type(t[i]) == "table" then t[i] = "pill" .. tostring(LOG.pid[t[i]] or "?")
          else t[i] = tostring(t[i]) end
        end
        LOG.calls[#LOG.calls + 1] = tag .. "(" .. table.concat(t, ",") .. ")=" .. tostring(r)
      end
      return r
    end
  end
  for _, n in ipairs({ "in_map", "stay_in", "standable", "buildable", "lgm_reach",
                       "score_fn", "banned", "anger_of" }) do
    wrap(inp, n, n)
    inp._wrap[n] = inp[n]
  end
  local bs = env.base_stops
  if bs then
    env.base_stops = function(mx, my)
      local r = bs(mx, my)
      if LOG and (not env.base_at or env.base_at[my * 256 + mx]) then
        LOG.calls[#LOG.calls + 1] = string.format("base_stops(%d,%d,%s,%s)=%s", mx, my,
          tostring(env.omx), tostring(env.omy), tostring(r))
      end
      return r
    end
  end
  local tt = env.ttype
  env.ttype = function(mx, my)
    if LOG then note_tt(mx, my); LOG.in_env = true end
    local r = tt(mx, my)
    if LOG then LOG.in_env = false end
    return r
  end
  env.count_reads = function(n) if LOG then LOG.tt_n = LOG.tt_n + n end end
  local po = env.pill_on
  env.pill_on = function(mx, my)
    if LOG then
      local k = my * 256 + mx
      if not LOG.po_seen[k] then
        LOG.po_seen[k] = true
        LOG.po_first[#LOG.po_first + 1] = k
      end
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

local function no_sq(calls)
  local t = {}
  for _, c in ipairs(calls) do
    if not SQ_TEST[c:match("^([%w_]+)%(")] then t[#t + 1] = c end
  end
  return t
end

local cov = { cases = 0, best = 0, safe = 0, chain = 0, plan = 0, nudge = 0, hold = 0,
              hold_unsafe = 0, leeway = 0, reject = 0, finish = 0, why_unsafe = 0,
              ally_cover = 0, unsearched = 0, std_on = 0, start = 0, xcover = 0, no_chain = 0,
              memo = 0, memo_hits = 0, split = 0, split_yields = 0, group = 0, aim9 = 0,
              aimnb = 0, aimnb_moved = 0, far = 0, far_near = 0 }

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
    if b.start_now ~= nil then cov.start = cov.start + 1 end
    if b.extra_cover then cov.xcover = cov.xcover + 1 end
  end
  local beside = false
  for _, r in ipairs(res.all or {}) do if r.aimnb then beside = true end end
  if beside then cov.aimnb = cov.aimnb + 1 end
  local inp = res.inp
  if inp and V2.far_on(inp) and b then
    cov.far = cov.far + 1
    if b.radius and b.radius < inp.ring_radii[1] then cov.far_near = cov.far_near + 1 end
  end
  local st = res.v2 and res.v2.stat
  if st and (st.aimnb_moved or 0) > 0 then cov.aimnb_moved = cov.aimnb_moved + 1 end
  if res.no_chain then cov.no_chain = cov.no_chain + 1 end
  for _, r in ipairs(res.all or {}) do
    if r.nudge_wu and r.nudge_wu > 0 then cov.nudge = cov.nudge + 1 end
    if r.leeway then cov.leeway = cov.leeway + 1 end
    if r.rejected then cov.reject = cov.reject + 1 end
    if r.why_unsafe then cov.why_unsafe = cov.why_unsafe + 1 end
    if r.unsearched then cov.unsearched = cov.unsearched + 1 end
  end
end

-- One run of a copy (f) from a cold start: the wedge cache dropped, the
-- wall memo empty. memo: run twice (the second hits the memo).
local function prep(inp)
  V2.reset()
  if inp.wall_memo then
    V2.wall_memo_clear()
    V2.wall_memo_check(inp, inp.A)
  end
  inp.env.omx, inp.env.omy = nil, nil
end
local function run_c(inp) return G.evaluate(inp, V2, OLD) end

local function runs(f, inp, pid)
  prep(inp)
  local out = {}
  for k = 1, inp.wall_memo and 2 or 1 do
    if k == 2 then inp.env.omx, inp.env.omy = nil, nil end
    LOG = new_log(pid)
    local ok, r = pcall(f, inp)
    out[k] = { ok = ok, r = r, log = LOG, omx = inp.env.omx, omy = inp.env.omy }
    LOG = nil
    if not ok then break end
  end
  return out
end

local function cmp_run(a, b, inputs, tag, sq_in_c)
  local e = compare(a.r, b.r, inputs)
  if e then return tag .. e end
  local ca = sq_in_c and no_sq(a.log.calls) or a.log.calls
  e = cmp_list(ca, b.log.calls, tag .. "call") if e then return e end
  if a.log.tt_n ~= b.log.tt_n then
    return string.format("%sterrain reads: Lua %d, C %d", tag, a.log.tt_n, b.log.tt_n)
  end
  e = cmp_list(a.log.tt_first, b.log.tt_first, tag .. "first terrain read") if e then return e end
  e = cmp_list(a.log.po_first, b.log.po_first, tag .. "first pill_on read") if e then return e end
  if not same_num(a.omx or -1, b.omx or -1) or not same_num(a.omy or -1, b.omy or -1) then
    return string.format("%senv.omx/omy: Lua %s,%s C %s,%s", tag, tostring(a.omx), tostring(a.omy),
                         tostring(b.omx), tostring(b.omy))
  end
  return nil
end

-- A sliced run: f in a coroutine with the slicer over ticks.
local function sliced(f, inp, pid, ticks)
  prep(inp)
  LOG = new_log(pid)
  local co = coroutine.create(function() return f(inp) end)
  local sl = V2.slicer(inp, ticks, coroutine.yield)
  inp.v2_slice = sl.fn
  local yields, ok, r = 0, nil, nil
  while true do
    ok, r = coroutine.resume(co)
    if not ok or coroutine.status(co) == "dead" then break end
    yields = yields + 1
  end
  inp.v2_slice = nil
  local out = { ok = ok, r = r, log = LOG, omx = inp.env.omx, omy = inp.env.omy, yields = yields, sl = sl }
  LOG = nil
  return out
end

local function cmp_slicer(a, b)
  if a.yields ~= b.yields then return string.format("split: yields Lua %d, C %d", a.yields, b.yields) end
  local sa, sb = a.sl, b.sl
  if sa.slices ~= sb.slices then return string.format("split: slices Lua %s, C %s", sa.slices, sb.slices) end
  if not same_num(sa.over or -1, sb.over or -1) then return "split: over differs" end
  if not same_num(sa.budget or -1, sb.budget or -1) then
    return string.format("split: budget Lua %s, C %s", tostring(sa.budget), tostring(sb.budget))
  end
  for i = 1, math.max(#sa.wu, #sb.wu) do
    if not same_num(sa.wu[i] or -1, sb.wu[i] or -1) then
      return string.format("split: wu[%d] Lua %s, C %s", i, tostring(sa.wu[i]), tostring(sb.wu[i]))
    end
  end
  return nil
end

-- Run both copies on inp. Returns nil or the first difference.
local n_case_run = 0
local function run_case(inp, pills)
  n_case_run = n_case_run + 1
  local inputs = { [inp] = true }
  local pid = {}
  for k, p in ipairs(pills) do inputs[p] = true; pid[p] = k end
  inp.capture_file = nil
  local std = inp.c_std
  local L = runs(V2.evaluate_lua, inp, pid)
  inp.c_std = nil
  local R = runs(run_c, inp, pid)
  inp.c_std = std
  if not L[1].ok or not R[1].ok then
    if L[1].ok ~= R[1].ok then
      return "Lua ok=" .. tostring(L[1].ok) .. " C ok=" .. tostring(R[1].ok) .. ": "
        .. tostring(L[1].ok and R[1].r or L[1].r)
    end
    return nil   -- both raise (a bad input): not a parity case
  end
  cov.cases = cov.cases + 1
  coverage(L[1].r)
  if inp.aim_points == 9 then cov.aim9 = cov.aim9 + 1 end
  for k = 1, #L do
    if not R[k] or R[k].ok ~= L[k].ok then return "run " .. k .. ": C ok differs" end
    local e = cmp_run(L[k], R[k], inputs, k == 2 and "memo run 2: " or "", false)
    if e then return e end
  end
  if inp.wall_memo then
    cov.memo = cov.memo + 1
    local s2 = L[2] and L[2].r and L[2].r.v2 and L[2].r.v2.stat
    if V2.WM.hits > 0 then cov.memo_hits = cov.memo_hits + 1 end
    local _ = s2
  end
  -- the square tests in C (inp.c_std, inp's own square tests)
  if std and inp._orig then
    for _, nm in ipairs(SQ_TESTS) do inp[nm] = inp._orig[nm] end
    local S = runs(run_c, inp, pid)
    for _, nm in ipairs(SQ_TESTS) do inp[nm] = inp._wrap[nm] end
    for k = 1, #L do
      if not S[k] or not S[k].ok then return "C (square tests in C) raised: " .. tostring(S[k] and S[k].r) end
      local e = cmp_run(L[k], S[k], inputs, "square tests in C: ", true)
      if e then return e end
    end
    cov.std_on = cov.std_on + 1
  end
  -- split
  if N_SPLIT > 0 and n_case_run % N_SPLIT == 0 then
    local ticks = 1 + rnd(7)
    local a = sliced(V2.evaluate_lua, inp, pid, ticks)
    local b = sliced(V2.evaluate_c, inp, pid, ticks)
    if not a.ok or not b.ok then
      return "split: Lua ok=" .. tostring(a.ok) .. " C ok=" .. tostring(b.ok) .. ": " .. tostring(a.ok and b.r or a.r)
    end
    local e = cmp_run(a, b, inputs, "split (" .. ticks .. " ticks): ", false)
    if e then return e end
    e = cmp_slicer(a, b)
    if e then return e end
    -- and the one-call Lua result: the same answer
    e = compare(L[1].r, a.r, inputs)
    if e and not inp.wall_memo then return "split Lua against one call: " .. e end
    cov.split = cov.split + 1
    cov.split_yields = cov.split_yields + a.yields
  end
  -- group
  if N_GROUP > 0 and inp.chains and n_case_run % N_GROUP == 0 then
    local A0 = inp.A
    local function grp(on)
      inp.v2_c = on
      prep(inp)
      LOG = new_log(pid)
      local ok, r = pcall(V2.evaluate_group, inp)
      local o = { ok = ok, r = r, log = LOG, omx = inp.env.omx, omy = inp.env.omy }
      LOG = nil
      inp.A = A0
      inp.v2_c = nil
      return o
    end
    local a, b = grp(false), grp(true)
    if a.ok ~= b.ok then return "group: Lua ok=" .. tostring(a.ok) .. " C ok=" .. tostring(b.ok)
      .. ": " .. tostring(a.ok and b.r or a.r) end
    if a.ok then
      local e = cmp_run(a, b, inputs, "group: ", false)
      if e then return e end
      cov.group = cov.group + 1
    end
  end
  return nil
end

-- ── random worlds ───────────────────────────────────────────────────────
local terrain = {}
local base_rule = 0
local function sqk(mx, my) return my * 256 + mx end
U.ttype      = function(mx, my)
  if LOG and not LOG.in_env then note_tt(mx, my) end
  return terrain[sqk(mx, my)] or C.T_GRASS
end
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
  local density = 3 + rnd(12)
  for my = cy - 14, cy + 14 do
    for mx = cx - 14, cx + 14 do
      if rnd(100) < density then terrain[sqk(mx, my)] = TT[1 + rnd(#TT)] end
    end
  end
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
  -- v2's knobs
  inp.aim_points = rnd(2) == 0 and 9 or 5
  inp.prefer_existing_cover = rnd(4) ~= 0
  inp.start_while_building = rnd(4) ~= 0
  inp.start_anger_rise = rnd(4) ~= 0
  inp.extra_cover_walls = ({ 0, 1, 1, 2 })[1 + rnd(4)]
  inp.chains_only = rnd(5) == 0
  inp.chains_only_runs = rnd(3) ~= 0
  inp.wall_memo = rnd(4) == 0
  inp.aim_avoid_nb = rnd(3) ~= 0
  inp.farthest_ring = rnd(2) == 0
  inp.ring_radii = ({ { 7.4, 7.2, 7.0, 6.6 }, { 7.4, 7.0 }, { 6.6 }, { 7.8, 7.4, 6.2 } })[1 + rnd(4)]
  if rnd(8) == 0 then inp.ring_radii = {} end
  if rnd(6) == 0 then inp._xcover_later = true end
  local hx, hy = cx + 0.5 - 6 + rnd(13), cy + 0.5 + 8
  inp.score_fn = function(deg, mx, my, x, y)
    local dx, dy = x - hx, y - hy
    return math.sqrt(dx * dx + dy * dy) + (mx % 3) * 0.125
  end
  instrument(inp)
  return inp, pills
end

local n, bad, first = 0, 0, nil
KINDS, KINDS_ORDER = {}, {}
local function one(tag, ci, inp, pills)
  n = n + 1
  local ok, e = pcall(run_case, inp, pills)
  if not ok then e = "harness error: " .. tostring(e) end
  if e then
    bad = bad + 1
    first = first or (tag .. " " .. ci .. ": " .. e)
    local kind = e:gsub("%-?[%d%.]+e?[%-+]?%d*", "#"):sub(1, 120)
    if not KINDS[kind] then KINDS[kind] = 0; KINDS_ORDER[#KINDS_ORDER + 1] = { kind, tag .. " " .. ci .. ": " .. e } end
    KINDS[kind] = KINDS[kind] + 1
  end
end

for ci = 1, N_RANDOM do
  local inp, pills = random_case(ci)
  one("random case", ci, inp, pills)
end

-- ── live shell rules ────────────────────────────────────────────────────
local function live_rules(env)
  local function maybe(v) if rnd(4) == 0 then return nil end return v end
  env.shell_speed = maybe(({ 24, 28, 32, 36, 40, 48 })[1 + rnd(6)])
  env.shell_start_add = maybe(rnd(8))
  env.shell_life = maybe(({ 6, 7, 8, 9, 10, 7.5 })[1 + rnd(6)])
  env.tank_hit_r = maybe(({ 80, 96, 112, 128, 144 })[1 + rnd(5)])
  env.pill_fire_len = maybe(({ 6, 7.5, 8.5, 9, 10.25 })[1 + rnd(5)])
end

for ci = 1, N_LIVE do
  local inp, pills = random_case(N_RANDOM + ci)
  live_rules(inp.env)
  one("live shell case", ci, inp, pills)
end

-- ── boats and the tanks we see ──────────────────────────────────────────
local function tank_blockers(inp)
  inp.env.ts_boats = true
  local t = {}
  local A = inp.A
  for _ = 1, rnd(7) do
    t[#t + 1] = (A.mx - 9 + rnd(19)) * 256 + rnd(256)
    t[#t + 1] = (A.my - 9 + rnd(19)) * 256 + rnd(256)
  end
  inp.env.ts_tanks = #t > 0 and t or nil
end
for ci = 1, N_BLOCK do
  local inp, pills = random_case(N_RANDOM + N_LIVE + ci)
  tank_blockers(inp)
  if ci % 2 == 0 then live_rules(inp.env) end
  one("tank shot blockers case", ci, inp, pills)
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
      local inp, pills = CAP.replay(case, OLD)
      instrument(inp)
      n_rec = n_rec + 1
      one("recorded case", ci, inp, pills)
    end
  end
end

local cv = {}
for _, k in ipairs({ "cases", "best", "safe", "chain", "plan", "nudge", "hold", "hold_unsafe",
                     "leeway", "reject", "finish", "why_unsafe", "ally_cover", "unsearched", "std_on",
                     "start", "xcover", "no_chain", "memo", "memo_hits", "split", "split_yields",
                     "group", "aim9", "aimnb", "aimnb_moved", "far", "far_near" }) do
  cv[#cv + 1] = k .. "=" .. cov[k]
end
local cvs = string.format("%d random + %d live shell + %d tank shot blockers + %d recorded; %s",
  N_RANDOM, N_LIVE, N_BLOCK, n_rec, table.concat(cv, " "))
for _, k in ipairs({ "split", "group", "std_on", "memo" }) do
  if cov[k] == 0 then
    bad = bad + 1
    first = first or ("no " .. k .. " case ran")
  end
end
print("test_pt2_v2_c_parity: " .. n .. " cases, " .. bad .. " bad; " .. cvs)
if first then print("  first: " .. first) end
for i, k in ipairs(KINDS_ORDER) do
  if i > 25 then break end
  print(string.format("  kind x%d: %s", KINDS[k[1]], k[2]:sub(1, 300)))
end
return n, bad, first or "", cvs
