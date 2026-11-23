-- =========================================================================
-- pt2_bench.lua -- speed check of the pill take v2 evaluator on recorded
-- inputs. Run by the unit test gh_pt2_bench (test_gh_pt2_old_parity.c), which
-- runs it only when WB_PT2_BENCH is this file's path. It is not a pass or
-- fail test: it prints times, and `dump` writes one line per case so two
-- builds can be compared (the lines must be the same).
--
-- WB_PT2_BENCH_ARGS (space separated key=value):
--   cases=<file>[;<file>...]  files written by pt2_capture.lua
--                             (PT2_CAPTURE_FILE); one CASES line each
--   max=<n>                   at most n cases (default all)
--   reps=<n>                  timed runs per case (default 5; the median
--                             is the case's time)
--   prof=1                    also run once per case with gh_pt2_old.prof on
--                             and print the C part / callback breakdown
--   dump=<file>               write each case's result digest to the file
--   keep_all=1                inp.keep_all on (every spot in the result)
--   lua=1                     run pilltake_v2_old.evaluate_lua, not the C
--   eval=v2                   time pilltake_v2.evaluate_lua instead (wedges
--                             cold: dropped before each run; warm=1 keeps them)
--   eval=both                 old (C and Lua) and v2 on each case: costs, safe
--                             takes each finds, best points, kills and depth
--
-- The recorded case answers questions from its record only. Here each case
-- gets a world built from that record, with callbacks shaped like the
-- game's (pilltake_v2_old.world_env / world_inp): a terrain array read through
-- a U.ttype-like reader (counter, mask, change list), a pill_at table, the
-- C square tests (inp.c_std), env.ts_boats. A square the record has no
-- terrain for is grass (not standable when the record says so: deep sea).
-- lgm_reach, score_fn, banned, anger_of and base_stops answer from the
-- record (an unrecorded question: true, a fixed score, false, nil, false).
-- =========================================================================

if math.atan2 and not rawget(_G, "_PT2_ATAN_DONE") then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
  _G._PT2_ATAN_DONE = true
end
_G.print2 = _G.print2 or function() end

local C   = require("constants")
local V2  = require("pilltake_v2_old")
local CAP = require("pt2_capture")
local bit = require("bit")
local G   = rawget(_G, "gh_pt2_old")

local args = {}
for k, v in tostring(rawget(_G, "PT2_BENCH_ARGS") or ""):gmatch("([%w_]+)=(%S+)") do args[k] = v end
local MAX = tonumber(args.max or "") or math.huge
local REPS = tonumber(args.reps or "") or 5
local USE_LUA = args.lua == "1"
if not G and not USE_LUA then
  print("pt2_bench: no gh_pt2_old global")
  return 1
end

-- ── clock (us) ──────────────────────────────────────────────────────────
local now_us
do
  local ok, ffi = pcall(require, "ffi")
  if bench_clock_us then
    now_us = bench_clock_us
  elseif ok and ffi.os == "Windows" then
    ffi.cdef [[ int QueryPerformanceCounter(int64_t *c); int QueryPerformanceFrequency(int64_t *f); ]]
    local q, f = ffi.new("int64_t[1]"), ffi.new("int64_t[1]")
    ffi.C.QueryPerformanceFrequency(f)
    local fr = tonumber(f[0]) / 1e6
    now_us = function() ffi.C.QueryPerformanceCounter(q); return tonumber(q[0]) / fr end
  elseif ok then
    ffi.cdef [[ typedef struct { long tv_sec; long tv_nsec; } pt2b_ts; int clock_gettime(int, pt2b_ts *); ]]
    local ts = ffi.new("pt2b_ts")
    now_us = function() ffi.C.clock_gettime(1, ts); return tonumber(ts.tv_sec) * 1e6 + tonumber(ts.tv_nsec) / 1e3 end
  else
    now_us = function() return os.clock() * 1e6 end
  end
end

-- ── the game-like world of one case ────────────────────────────────────
local function sqkey(k)
  local a, b = k:match("^([^,]+),([^,]+)$")
  return tonumber(b) * 256 + tonumber(a)
end

local W   -- the side effects of this case (U.ttype's counter and change list)
local function game_inp(case)
  local inp, pills = CAP.replay(case, V2)
  local rec = case.rec
  local env = inp.env
  local TT = {}
  for k, v in pairs(rec.ttype) do TT[sqkey(k)] = v end
  local PA = {}
  for k, v in pairs(rec.pill_on) do if v > 0 then PA[sqkey(k)] = pills[v] end end
  for k, v in pairs(rec.standable) do
    local q = sqkey(k)
    if v == false and TT[q] == nil and PA[q] == nil then TT[q] = C.T_DEEPSEA end
  end
  local DEF = C.T_GRASS
  W = { n = 0, prev = {}, changes = {} }
  local w = W
  local function get_terrain(mx, my) return TT[my * 256 + mx] or DEF end
  local function ttype(mx, my)
    w.n = w.n + 1
    local tt = bit.band(get_terrain(mx, my), 0xFF)
    local key = my * 256 + mx
    local prev = w.prev[key]
    if prev ~= nil and prev ~= tt then w.changes[#w.changes + 1] = key end
    w.prev[key] = tt
    return tt
  end
  env.ttype = ttype
  env.pill_on = function(mx, my)
    local p = PA[my * 256 + mx]
    if p and (p.health or 0) > 0 then return p end
    return nil
  end
  env.is_ally = function(p) return p.owner == "friendly" or p.owner == "allied" end
  local ws = env.wall_shots or 5
  env.cover_of_pill = function(p)
    if p and not p.in_tank and (p.owner == "friendly" or p.owner == "allied") then
      return math.floor((tonumber(p.health) or 0) / 5) * ws
    end
    return 0
  end
  local bs = env.base_stops
  local bs_memo = {}   -- the same answers, without a string key per read
  env.base_stops = function(mx, my)
    local k = ((env.omx or mx) * 256 + (env.omy or my)) * 65536 + my * 256 + mx
    local v = bs_memo[k]
    if v == nil then
      local ok, a = pcall(bs, mx, my)
      v = ok and a or false
      bs_memo[k] = v
    end
    return v
  end
  env.count_reads = function(n) w.n = w.n + n end
  env.ts_boats = true
  inp.in_map = function(mx, my) return mx >= 0 and mx <= 255 and my >= 0 and my <= 255 end
  inp.stay_in = function() return true end
  local cost_land = C.TERRAIN_COST_LAND or {}
  inp.standable = function(mx, my)
    local tt = ttype(mx, my)
    if (cost_land[tt] or 9999) >= 9999 or tt == C.T_RIVER or tt == C.T_DEEPSEA then return false end
    if env.pill_on(mx, my) then return false end
    return true
  end
  inp.buildable = function(mx, my)
    local tt = ttype(mx, my)
    return tt ~= C.T_DEEPSEA and tt ~= C.T_RIVER and tt ~= C.T_SWAMP and
           tt ~= C.T_PILLBOX and tt ~= C.T_REFBASE and tt ~= C.T_BOAT
  end
  inp.c_std = {
    in_map = inp.in_map, stay_in = inp.stay_in, standable = inp.standable, buildable = inp.buildable,
    cost = cost_land, water = { n = 2, C.T_RIVER, C.T_DEEPSEA },
    nobuild = { n = 6, C.T_DEEPSEA, C.T_RIVER, C.T_SWAMP, C.T_PILLBOX, C.T_REFBASE, C.T_BOAT },
    no_builder = false, rects = false,
  }
  local function soft(name, def)
    local f = inp[name]
    if not f then return end
    inp[name] = function(...)
      local ok, v = pcall(f, ...)
      if ok then return v end
      return def(...)
    end
  end
  soft("lgm_reach", function() return true end)
  do   -- the same answers, without a string key per question
    local f, memo = inp.lgm_reach, {}
    if f then
      inp.lgm_reach = function(omx, omy, mx, my)
        local k = ((omx * 256 + omy) * 256 + mx) * 256 + my
        local v = memo[k]
        if v == nil then v = f(omx, omy, mx, my); memo[k] = v end
        return v
      end
    end
  end
  soft("score_fn", function(deg, mx, my) return (deg % 37) + mx * 0.01 + my * 0.001 end)
  soft("banned", function() return false end)
  soft("anger_of", function() return nil end)
  inp.keep_all = args.keep_all == "1" or nil
  return inp, pills
end

-- ── a digest of the result ─────────────────────────────────────────────
-- Every field, keys in a fixed order; a table met before is written as a
-- reference to its first place (so table sharing is in the digest too);
-- input tables (pills, inp) by name.
local fmt = string.format
local function digest(res, inp, pills)
  local out, seen, nseen = {}, {}, 0
  local inputs = { [inp] = "INP" }
  for k, p in ipairs(pills) do inputs[p] = "P" .. k end
  local function num(x)
    if x ~= x then return "nan" end
    return fmt("%.17g", x)
  end
  local function kstr(k)
    local tk = type(k)
    if tk == "table" then return inputs[k] or "?T" end
    if tk == "number" then return "#" .. num(k) end
    return tostring(k)
  end
  local function ser(v)
    local tv = type(v)
    if tv == "number" then out[#out + 1] = num(v)
    elseif tv == "table" then
      if inputs[v] then out[#out + 1] = inputs[v]; return end
      if seen[v] then out[#out + 1] = "@" .. seen[v]; return end
      nseen = nseen + 1
      seen[v] = nseen
      local ks = {}
      for k in pairs(v) do ks[#ks + 1] = k end
      table.sort(ks, function(a, b) return kstr(a) < kstr(b) end)
      out[#out + 1] = "{"
      for _, k in ipairs(ks) do
        out[#out + 1] = kstr(k) .. "="
        ser(v[k])
        out[#out + 1] = ","
      end
      out[#out + 1] = "}"
    else out[#out + 1] = tostring(v) end
  end
  ser(res)
  return table.concat(out)
end

local function fnv(s)
  local h = 2166136261
  for i = 1, #s do
    h = bit.bxor(h, s:byte(i))
    h = bit.tobit(h * 16777619)
  end
  return bit.tohex(h)
end

-- ── run ─────────────────────────────────────────────────────────────────
local dumpf = args.dump and assert(io.open(args.dump, "w"))
local EVAL = args.eval or "old"
local NV = (EVAL ~= "old") and require("pilltake_v2") or nil

local function evaluate(inp)
  if EVAL == "v2" then return NV.evaluate_lua(inp) end
  if USE_LUA then return V2.evaluate_lua(inp) end
  return G.evaluate(inp, V2)
end

local times, spots, n_case, prof, n_err = {}, {}, 0, {}, 0
local function run_case(case)
  n_case = n_case + 1
  local inp, pills = game_inp(case)
  if EVAL == "v2" then NV.reset() end
  -- the digest run (also the first, untimed, run)
  inp.env.omx, inp.env.omy = nil, nil
  local ok, res = pcall(evaluate, inp)
  if not ok then
    n_err = n_err + 1
    if dumpf then dumpf:write(fmt("%d ERR %s\n", n_case, tostring(res):gsub("\n", " "))) end
    return
  end
  if dumpf then
    local ch = {}
    local ks = {}
    for k in pairs(W.prev) do ks[#ks + 1] = k end
    table.sort(ks)
    local d = digest(res, inp, pills) .. "|reads=" .. W.n .. "|changes=" .. table.concat(W.changes, ",")
      .. "|seen=" .. table.concat(ks, ",") .. "|om=" .. tostring(inp.env.omx) .. "," .. tostring(inp.env.omy)
    local b = res.best
    dumpf:write(fmt("%d n_eval=%d n_reject=%d best=%s len=%d h=%s\n", n_case, res.n_eval, res.n_reject,
      b and fmt("%s/%s/%s/%s/%s", tostring(b.deg), tostring(b.score), tostring(b.safe), tostring(b.kills),
        tostring(b.plan and #b.plan)) or "-", #d, fnv(d)))
  end
  if REPS > 0 then
    local t = {}
    for r = 1, REPS do
      inp.env.omx, inp.env.omy = nil, nil
      if EVAL == "v2" and args.warm ~= "1" then NV.reset() end
      local t0 = now_us()
      evaluate(inp)
      t[r] = now_us() - t0
    end
    table.sort(t)
    times[#times + 1] = t[math.ceil(#t / 2)]
    spots[#spots + 1] = res.n_eval
  end
  if args.prof == "1" and not USE_LUA then
    inp.env.omx, inp.env.omy = nil, nil
    G.prof(true); G.prof_take()
    evaluate(inp)
    local pt = G.prof_take()
    G.prof(false)
    for k, v in pairs(pt) do
      local p = prof[k] or { n = 0, us = 0 }
      p.n, p.us = p.n + v.n, p.us + v.us
      prof[k] = p
    end
  end
end

-- ── eval=both: old and v2 side by side ──────────────────────────────────
local BOTH = EVAL == "both"
local B = {
  t_oc = {}, t_ol = {}, t_vc = {}, t_vw = {},
  n = 0, err_o = 0, err_v = 0,
  safe_both = 0, safe_old_only = 0, safe_v2_only = 0, safe_none = 0,
  best_same = 0, best_1deg = 0, best_far = 0,
  kills_same = 0, kills_v2_more = 0, kills_old_more = 0,
  depth_same = 0, depth_v2_more = 0, depth_old_more = 0,
  walls_same = 0, walls_v2_fewer = 0, walls_old_fewer = 0, walls_old = 0, walls_v2 = 0,
  st = {}, tm = {},
}
local function add_to(dst, src)
  for k, v in pairs(src or {}) do if type(v) == "number" then dst[k] = (dst[k] or 0) + v end end
end
local function median_of(f, reps)
  local t = {}
  for r = 1, reps do
    local t0 = now_us()
    f()
    t[r] = now_us() - t0
  end
  table.sort(t)
  return t[math.ceil(#t / 2)]
end
local function run_both(case)
  n_case = n_case + 1
  B.n = B.n + 1
  local reps = math.max(1, REPS)
  -- old: C and Lua
  local oinp = game_inp(case)
  oinp.env.omx, oinp.env.omy = nil, nil
  local ok, ro = pcall(G.evaluate, oinp, V2)
  if not ok then B.err_o = B.err_o + 1; return end
  B.t_oc[#B.t_oc + 1] = median_of(function() oinp.env.omx, oinp.env.omy = nil, nil; G.evaluate(oinp, V2) end, reps)
  B.t_ol[#B.t_ol + 1] = median_of(function() oinp.env.omx, oinp.env.omy = nil, nil; V2.evaluate_lua(oinp) end, reps)
  -- v2
  local vinp = game_inp(case)
  NV.reset()
  local ok2, rv = pcall(NV.evaluate_lua, vinp)
  if not ok2 then
    B.err_v = B.err_v + 1
    io.stderr:write(fmt("pt2_bench: v2 error case %d: %s\n", n_case, tostring(rv)))
    return
  end
  B.t_vc[#B.t_vc + 1] = median_of(function() NV.reset(); NV.evaluate_lua(vinp) end, reps)
  B.t_vw[#B.t_vw + 1] = median_of(function() NV.evaluate_lua(vinp) end, reps)
  -- one more cold v2 run, untimed, with the mask checks and the clock:
  -- mask losses (points CANT removed that pass the exact tests, and that
  -- would be safe after walls), the masks' chains against the exact ones,
  -- and the time split
  NV.reset()
  vinp.v2_loss_check, vinp.v2_mask_stats = true, true
  local rs = NV.evaluate_lua(vinp)
  vinp.v2_loss_check, vinp.v2_mask_stats = nil, nil
  add_to(B.st, rs.v2 and rs.v2.stat)
  NV.reset()
  vinp.v2_clock = now_us
  local rt = NV.evaluate_lua(vinp)
  vinp.v2_clock = nil
  add_to(B.tm, rt.v2 and rt.v2.tm)
  -- compare
  local bo, bv = ro.best, rv.best
  local so, sv = bo and bo.safe or false, bv and bv.safe or false
  if so and sv then B.safe_both = B.safe_both + 1
  elseif so then B.safe_old_only = B.safe_old_only + 1
  elseif sv then B.safe_v2_only = B.safe_v2_only + 1
  else B.safe_none = B.safe_none + 1 end
  if so and sv then
    local dd = math.abs(((bo.deg - bv.deg) + 180) % 360 - 180)
    if dd == 0 and bo.radius == bv.radius then B.best_same = B.best_same + 1
    elseif dd <= 1 then B.best_1deg = B.best_1deg + 1
    else B.best_far = B.best_far + 1 end
    if bo.kills == bv.kills then B.kills_same = B.kills_same + 1
    elseif bv.kills > bo.kills then B.kills_v2_more = B.kills_v2_more + 1
    else B.kills_old_more = B.kills_old_more + 1 end
    local wo, wv = bo.plan and #bo.plan or 0, bv.plan and #bv.plan or 0
    B.walls_old, B.walls_v2 = B.walls_old + wo, B.walls_v2 + wv
    if wo == wv then B.walls_same = B.walls_same + 1
    elseif wv < wo then B.walls_v2_fewer = B.walls_v2_fewer + 1
    else B.walls_old_fewer = B.walls_old_fewer + 1 end
    if bo.depth == bv.depth then B.depth_same = B.depth_same + 1
    elseif bv.depth > bo.depth then B.depth_v2_more = B.depth_v2_more + 1
    else B.depth_old_more = B.depth_old_more + 1 end
  end
end
if BOTH then
  if not G then print("pt2_bench: eval=both needs gh_pt2_old (run from WinBoloUnitTests)"); return 1 end
  run_case = run_both
end

local files = {}
for f in tostring(args.cases or ""):gmatch("[^;]+") do files[#files + 1] = f end
if #files == 0 then
  print("pt2_bench: no cases= file")
  return 1
end
local env = setmetatable({ CASES = {} }, { __index = _G })
for _, path in ipairs(files) do
  if n_case >= MAX then break end
  for line in io.lines(path) do
    if n_case >= MAX then break end
    if line:find("^CASES") then
      env.CASES = {}
      local chunk = assert(loadstring(line))
      setfenv(chunk, env)
      chunk()
      local case = env.CASES[1]
      if case then run_case(case) end
      if n_case % 100 == 0 then io.stderr:write(fmt("pt2_bench: %d cases\n", n_case)) end
    end
  end
end
if dumpf then dumpf:close() end

local function q(t, p)
  local s = {}
  for i, v in ipairs(t) do s[i] = v end
  table.sort(s)
  return s[math.max(1, math.ceil(p * #s))] or 0
end

if BOTH then
  local function row(name, t)
    print(fmt("  %-34s median %8.3f  p90 %8.3f  max %9.3f ms  (n %d)", name, q(t, 0.5) / 1000, q(t, 0.9) / 1000,
      q(t, 1) / 1000, #t))
  end
  print(fmt("pt2_bench both: %d cases, errors old %d v2 %d, reps %d", B.n, B.err_o, B.err_v, math.max(1, REPS)))
  print("cost per evaluate:")
  row("old C", B.t_oc)
  row("old Lua", B.t_ol)
  row("v2 Lua, cold", B.t_vc)
  row("v2 Lua, warm", B.t_vw)
  print(fmt("safe take: both %d, old only %d, v2 only %d, neither %d",
    B.safe_both, B.safe_old_only, B.safe_v2_only, B.safe_none))
  print(fmt("best point (both safe): same %d, within 1 deg %d, farther %d", B.best_same, B.best_1deg, B.best_far))
  print(fmt("kills (both safe): same %d, v2 more %d, old more %d", B.kills_same, B.kills_v2_more, B.kills_old_more))
  print(fmt("depth (both safe): same %d, v2 more %d, old more %d", B.depth_same, B.depth_v2_more, B.depth_old_more))
  print(fmt("walls planned (both safe): same %d, v2 fewer %d, old fewer %d; total old %d v2 %d",
    B.walls_same, B.walls_v2_fewer, B.walls_old_fewer, B.walls_old, B.walls_v2))
  local st = B.st
  local function g(k) return st[k] or 0 end
  print(fmt("v2 masks: CANT removed %d points, %d of them pass the exact tests, %d of those safe after walls",
    g("cant"), g("loss_pass"), g("loss_safe")))
  print(fmt("v2 masks: kept %d points; mask chain = exact chain %d, differs %d, not sure %d",
    g("survive"), g("chain_agree"), g("chain_differ"), g("chain_unsure")))
  print(fmt("v2 work: casts %d (kept and used again %d), confirm casts %d, points planned %d, plans not confirmed %d,"
    .. " lone pill runs %d (points left unplanned %d), wall search nodes %d, nudged %d",
    g("casts"), g("ray_reuse"), g("casts_confirm"), g("planned"), g("confirm_fail"), g("lone"),
    g("plan_skipped"), g("wall_nodes"), g("nudged")))
  local tm = B.tm
  local tot = tm.total or 0
  local parts = { "wedges", "masks", "cast", "aim", "walls", "score_fn" }
  local sum = 0
  local out = {}
  for _, k in ipairs(parts) do
    sum = sum + (tm[k] or 0)
    out[#out + 1] = fmt("%s %.1f%%", k, 100 * (tm[k] or 0) / math.max(1, tot))
  end
  out[#out + 1] = fmt("other %.1f%%", 100 * (tot - sum) / math.max(1, tot))
  print(fmt("v2 time split (one cold run per case, clock on, total %.1f ms): %s", tot / 1000, table.concat(out, ", ")))
  print(fmt("  (nested, already in cast and aim: wall plan confirm %.1f%%, leeway %.1f%%)",
    100 * (tm.confirm or 0) / math.max(1, tot), 100 * (tm.leeway or 0) / math.max(1, tot)))
  return 0
end
local tot_us, tot_sp = 0, 0
for i, v in ipairs(times) do tot_us = tot_us + v; tot_sp = tot_sp + spots[i] end
print(fmt("pt2_bench: %d cases (%d errors), %s, reps %d: evaluate ms median %.3f p90 %.3f max %.3f;"
  .. " spots median %d p90 %d max %d; total %.1f ms, %.3f spots/us (%.2f us/spot)",
  n_case, n_err, USE_LUA and "Lua" or "C", REPS, q(times, 0.5) / 1000, q(times, 0.9) / 1000,
  q(times, 1) / 1000, q(spots, 0.5), q(spots, 0.9), q(spots, 1), tot_us / 1000,
  tot_us > 0 and tot_sp / tot_us or 0, tot_sp > 0 and tot_us / tot_sp or 0))
if next(prof) then
  local tot = prof.total and prof.total.us or 0
  local rows = {}
  for k, v in pairs(prof) do rows[#rows + 1] = { k, v } end
  table.sort(rows, function(a, b) return a[2].us > b[2].us end)
  print(fmt("pt2_bench prof (gh_pt2_old.prof on, inclusive us; total %.1f ms):", tot / 1000))
  for _, r in ipairs(rows) do
    print(fmt("  %-22s n=%-9d us=%-10.1f %5.1f%%  %.3f us/call", r[1], r[2].n, r[2].us,
      tot > 0 and 100 * r[2].us / tot or 0, r[2].n > 0 and r[2].us / r[2].n or 0))
  end
end
return 0
