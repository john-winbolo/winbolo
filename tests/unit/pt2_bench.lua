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
--   eval=shadow               time pilltake_shadow.evaluate_lua instead (maps
--                             cold: dropped before each case)
--   eval=both                 old (C) and shadow on each case: costs (old C,
--                             old Lua, shadow cold / warm / warm without the
--                             nudge), safe takes each finds, best points,
--                             kills and depth, the map build time per pill,
--                             and a second pass in file order with the maps
--                             kept (map builds, drops and updates in a game)
--
-- The shadow evaluator asks score_fn and banned about points the record
-- does not have (grid points, not ring points). It gets the score of the
-- nearest recorded point, and the ban of the recorded 5 degree bucket.
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
local SM = (EVAL ~= "old") and require("pilltake_shadow") or nil

-- score_fn and banned for points the record does not have (see the top)
local function shadow_inp(inp, case)
  local rec = case.rec
  local pts = {}
  for k, v in pairs(rec.score_fn) do
    local _, _, _, cx, cy = k:match("^([^,]+),([^,]+),([^,]+),([^,]+),([^,]+)$")
    pts[#pts + 1] = { tonumber(cx), tonumber(cy), v }
  end
  local ban = {}
  for k, v in pairs(rec.banned) do
    local b = math.floor((tonumber(k) % 360) / 5) * 5
    ban[b] = ban[b] or v
  end
  local memo = {}
  inp.score_fn = function(deg, mx, my, cx, cy)
    local key = cx .. "," .. cy
    local m = memo[key]
    if m then return m end
    local best, bd = nil, math.huge
    for _, q in ipairs(pts) do
      local dx, dy = q[1] - cx, q[2] - cy
      local d = dx * dx + dy * dy
      if d < bd then bd, best = d, q[3] end
    end
    m = best or ((deg % 37) + mx * 0.01 + my * 0.001)
    memo[key] = m
    return m
  end
  inp.banned = function(deg) return ban[math.floor((deg % 360) / 5) * 5] or false end
  inp.shadow_grid_wu = inp.shadow_grid_wu or 64
  return inp
end

local function evaluate(inp)
  if EVAL == "shadow" then return SM.evaluate_lua(inp) end
  if USE_LUA then return V2.evaluate_lua(inp) end
  return G.evaluate(inp, V2)
end

local times, spots, n_case, prof, n_err = {}, {}, 0, {}, 0
local function run_case(case)
  n_case = n_case + 1
  local inp, pills = game_inp(case)
  if EVAL == "shadow" then shadow_inp(inp, case); SM.reset(true) end
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
      if EVAL == "shadow" and args.warm ~= "1" then SM.reset(true) end
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

-- ── eval=both: old and shadow side by side ──────────────────────────────
local BOTH = EVAL == "both"
local B = {
  t_oc = {}, t_ol = {}, t_oln = {}, t_scg = {}, t_sc = {}, t_sw = {}, t_swn = {},
  map_cold = {}, map_warm = {}, map_pts = 0,
  n = 0, err_o = 0, err_s = 0,
  safe_both = 0, safe_old_only = 0, safe_sh_only = 0, safe_none = 0,
  best_cell = 0, best_step = 0, best_far = 0,
  kills_same = 0, kills_sh_more = 0, kills_old_more = 0,
  depth_same = 0, depth_sh_more = 0, depth_old_more = 0,
  old_chain_reach = 0, old_chain_reach_sh_unsafe = 0,
  old_nudged = 0, old_nudged_grid_fail = 0, old_nudged_safe = 0,
  sh_nudged = 0, sh_nudged_safe = 0,
  l32_both = 0, l32_old_only = 0, l32_sh_only = 0, l32_kills_same = 0, l32_kills_sh_more = 0,
  l32_kills_old_more = 0, l32_best_step = 0,
}
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
local function hostile_near(inp)
  local A, out = inp.A, {}
  for _, p in ipairs(inp.pills) do
    if (p.health or 0) > 0 and not p.in_tank and not inp.env.is_ally(p)
       and math.abs(p.mx - A.mx) <= 12 and math.abs(p.my - A.my) <= 12 then
      out[#out + 1] = p
    end
  end
  return out
end
-- old safe best that needed "reach" on a chain pill's ray
local function old_chain_reach(b, inp)
  for i, X in ipairs(b.T) do
    local r = b.rays[i]
    local bar = (X == inp.A and b.finish) and 0 or (b.chain_mode and inp.bar_chain or inp.bar_single)
    if args.why == "1" and b.in_chain[X] then
      print(fmt("WHYRAY chain pill %d hit=%s cover=%s bar=%s link_cover=%s", b.in_chain[X], tostring(r.hit),
        tostring(r.cover), tostring(r.bar), tostring(r.link_cover)))
    end
    if b.in_chain[X] and r.hit == "reach" and not r.link_cover and r.cover < bar then
      return true
    end
  end
  return false
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
  oinp.aim_nudge = false
  B.t_oln[#B.t_oln + 1] = median_of(function() oinp.env.omx, oinp.env.omy = nil, nil; V2.evaluate_lua(oinp) end, reps)
  oinp.aim_nudge = true
  -- shadow
  local sinp = shadow_inp(game_inp(case), case)
  SM.reset(false)
  local t0 = now_us()
  local ok2, rs = pcall(SM.evaluate_lua, sinp)
  B.t_scg[#B.t_scg + 1] = now_us() - t0
  if not ok2 then
    B.err_s = B.err_s + 1
    io.stderr:write(fmt("pt2_bench: shadow error case %d: %s\n", n_case, tostring(rs)))
    return
  end
  B.t_sc[#B.t_sc + 1] = median_of(function() SM.reset(true); SM.evaluate_lua(sinp) end, reps)
  B.t_sw[#B.t_sw + 1] = median_of(function() SM.evaluate_lua(sinp) end, reps)
  sinp.aim_nudge = false
  B.t_swn[#B.t_swn + 1] = median_of(function() SM.evaluate_lua(sinp) end, reps)
  sinp.aim_nudge = true
  -- the map build time per pill (cold: no offset table; warm: table kept)
  for _, X in ipairs(hostile_near(sinp)) do
    SM.reset(false)
    local a0 = now_us()
    local npt = SM.build_full_map(sinp, X)
    B.map_cold[#B.map_cold + 1] = now_us() - a0
    SM.reset(true)
    a0 = now_us()
    SM.build_full_map(sinp, X)
    B.map_warm[#B.map_warm + 1] = now_us() - a0
    B.map_pts = B.map_pts + npt
  end
  -- compare
  local bo, bs = ro.best, rs.best
  local so, ss = bo and bo.safe or false, bs and bs.safe or false
  if so and ss then B.safe_both = B.safe_both + 1
  elseif so then B.safe_old_only = B.safe_old_only + 1
  elseif ss then B.safe_sh_only = B.safe_sh_only + 1
  else B.safe_none = B.safe_none + 1 end
  if so and ss then
    local d = math.max(math.abs(bo.px - bs.px), math.abs(bo.py - bs.py))
    if d <= 32 then B.best_cell = B.best_cell + 1
    elseif d <= 64 then B.best_step = B.best_step + 1
    else B.best_far = B.best_far + 1 end
    if args.why == "1" and (bo.kills ~= bs.kills or d > 64) then
      local function sh(b) return fmt("(%d,%d) deg=%s kills=%d depth=%s plan=%d score=%s lee=%s", b.px, b.py,
        tostring(b.deg), b.kills, tostring(b.depth), #b.plan, tostring(b.score), tostring(b.leeway)) end
      print(fmt("WHY case %d A=(%d,%d) R=%s old %s | shadow %s", n_case, sinp.A.mx, sinp.A.my,
        tostring(sinp.radius), sh(bo), sh(bs)))
      for dx = -1, 1 do for dy = -1, 1 do
        local gx, gy = math.floor(bo.px / 64) * 64 + 32 + dx * 64, math.floor(bo.py / 64) * 64 + 32 + dy * 64
        local okp, pass, why = pcall(SM.set_test_at, sinp, gx, gy)
        local hit = nil
        for _, r in ipairs(rs.all or {}) do if r.px == gx and r.py == gy then hit = r end end
        print(fmt("WHYPT (%d,%d) set=%s %s in_all=%s %s", gx, gy, tostring(pass), tostring(why), tostring(hit ~= nil),
          hit and fmt("safe=%s kills=%d plan=%d why=%s covered=%s lee=%s unc=%s rule=%s", tostring(hit.safe), hit.kills,
            hit.plan and #hit.plan or -1, tostring(hit.why), tostring(hit.covered), tostring(hit.leeway),
            tostring(hit.uncovered and #hit.uncovered), tostring(hit.reject or hit.leeway_fail)) or ""))
        if hit and hit.leeway_pts then
          for _, q in ipairs(hit.leeway_pts) do io.write(fmt(" [%d,%d %s %s]", q.x, q.y, tostring(q.ok), tostring(q.why))) end
          print()
        end
      end end
    end
    if bo.kills == bs.kills then B.kills_same = B.kills_same + 1
    elseif bs.kills > bo.kills then B.kills_sh_more = B.kills_sh_more + 1
    else B.kills_old_more = B.kills_old_more + 1 end
    if bo.depth == bs.depth then B.depth_same = B.depth_same + 1
    elseif bs.depth > bo.depth then B.depth_sh_more = B.depth_sh_more + 1
    else B.depth_old_more = B.depth_old_more + 1 end
  end
  if so and old_chain_reach(bo, oinp) then
    B.old_chain_reach = B.old_chain_reach + 1
    if not ss then B.old_chain_reach_sh_unsafe = B.old_chain_reach_sh_unsafe + 1 end
  end
  if bo and (bo.nudge_wu or 0) > 0 then
    B.old_nudged = B.old_nudged + 1
    if so then B.old_nudged_safe = B.old_nudged_safe + 1 end
    local A = sinp.A
    local R = bo.radius or sinp.radius
    local px0 = math.floor((A.mx + 0.5 + math.sin(math.rad(bo.deg)) * R) * 256)
    local py0 = math.floor((A.my + 0.5 - math.cos(math.rad(bo.deg)) * R) * 256)
    local gx, gy = math.floor(px0 / 64) * 64 + 32, math.floor(py0 / 64) * 64 + 32
    local okp, pass = pcall(SM.set_test_at, sinp, gx, gy)
    if not okp or not pass then B.old_nudged_grid_fail = B.old_nudged_grid_fail + 1 end
  end
  if bs and (bs.nudge_wu or 0) > 0 then
    B.sh_nudged = B.sh_nudged + 1
    if ss then B.sh_nudged_safe = B.sh_nudged_safe + 1 end
  end
  -- shadow with the old leeway distance (32 wu neighbours, cast exactly)
  sinp.shadow_leeway_wu = 32
  local ok3, r32 = pcall(SM.evaluate_lua, sinp)
  sinp.shadow_leeway_wu = nil
  if ok3 then
    local b3 = r32.best
    local s3 = b3 and b3.safe or false
    if so and s3 then B.l32_both = B.l32_both + 1
    elseif so then B.l32_old_only = B.l32_old_only + 1
    elseif s3 then B.l32_sh_only = B.l32_sh_only + 1 end
    if so and s3 then
      if bo.kills == b3.kills then B.l32_kills_same = B.l32_kills_same + 1
      elseif b3.kills > bo.kills then B.l32_kills_sh_more = B.l32_kills_sh_more + 1
      else B.l32_kills_old_more = B.l32_kills_old_more + 1 end
      local d = math.max(math.abs(bo.px - b3.px), math.abs(bo.py - b3.py))
      if d <= 64 then B.l32_best_step = B.l32_best_step + 1 end
    end
  end
end

-- the second pass (file order, maps kept as in a game)
local G2 = { n = 0, t = {}, map_new = 0, map_drop = 0, pt_new = 0, pt_upd = 0, cases_new = 0, cases_upd = 0 }
local function run_kept(case)
  local sinp = shadow_inp(game_inp(case), case)
  local st = SM.stats
  local mn, md, pn, pu = st.map_new, st.map_drop, st.pt_new, st.pt_upd
  local t0 = now_us()
  local ok = pcall(SM.evaluate_lua, sinp)
  local us = now_us() - t0
  if not ok then return end
  G2.n = G2.n + 1
  G2.t[#G2.t + 1] = us
  G2.map_new = G2.map_new + st.map_new - mn
  G2.map_drop = G2.map_drop + st.map_drop - md
  G2.pt_new = G2.pt_new + st.pt_new - pn
  G2.pt_upd = G2.pt_upd + st.pt_upd - pu
  if st.map_new > mn then G2.cases_new = G2.cases_new + 1 end
  if st.pt_upd > pu then G2.cases_upd = G2.cases_upd + 1 end
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
  -- the second pass: file order, maps kept
  SM.reset(false)
  local n2 = 0
  for _, path in ipairs(files) do
    if n2 >= MAX then break end
    for line in io.lines(path) do
      if n2 >= MAX then break end
      if line:find("^CASES") then
        env.CASES = {}
        local chunk = assert(loadstring(line))
        setfenv(chunk, env)
        chunk()
        local case = env.CASES[1]
        if case then n2 = n2 + 1; run_kept(case) end
        if n2 % 100 == 0 then io.stderr:write(fmt("pt2_bench: kept pass %d cases\n", n2)) end
      end
    end
  end
  local function row(name, t)
    print(fmt("  %-34s median %8.3f  p90 %8.3f  max %9.3f ms  (n %d)", name, q(t, 0.5) / 1000, q(t, 0.9) / 1000,
      q(t, 1) / 1000, #t))
  end
  print(fmt("pt2_bench both: %d cases, errors old %d shadow %d, reps %d", B.n, B.err_o, B.err_s, math.max(1, REPS)))
  print("cost per evaluate:")
  row("old C", B.t_oc)
  row("old Lua", B.t_ol)
  row("old Lua, no nudge", B.t_oln)
  row("shadow cold (no table, no maps)", B.t_scg)
  row("shadow cold maps (table kept)", B.t_sc)
  row("shadow warm", B.t_sw)
  row("shadow warm, no nudge", B.t_swn)
  row("shadow kept maps, file order", G2.t)
  print("map build per pill (all grid points in range):")
  row("cold (no offset table)", B.map_cold)
  row("warm (offset table kept)", B.map_warm)
  print(fmt("  points per map: %.0f", #B.map_warm > 0 and B.map_pts / #B.map_warm or 0))
  print(fmt("safe take: both %d, old only %d, shadow only %d, neither %d",
    B.safe_both, B.safe_old_only, B.safe_sh_only, B.safe_none))
  print(fmt("best point (both safe): same grid cell %d, within one step %d, farther %d",
    B.best_cell, B.best_step, B.best_far))
  print(fmt("kills (both safe): same %d, shadow more %d, old more %d", B.kills_same, B.kills_sh_more, B.kills_old_more))
  print(fmt("depth (both safe): same %d, shadow more %d, old more %d", B.depth_same, B.depth_sh_more, B.depth_old_more))
  print(fmt("old safe bests that needed a chain pill's reach: %d (shadow not safe there: %d)",
    B.old_chain_reach, B.old_chain_reach_sh_unsafe))
  print(fmt("old nudged bests: %d (safe %d); grid point fails the set test: %d",
    B.old_nudged, B.old_nudged_safe, B.old_nudged_grid_fail))
  print(fmt("shadow nudged bests: %d (safe %d)", B.sh_nudged, B.sh_nudged_safe))
  print(fmt("shadow, leeway at 32 wu: safe both %d, old only %d, shadow only %d; kills same %d, shadow more %d,"
    .. " old more %d; best within one step %d", B.l32_both, B.l32_old_only, B.l32_sh_only, B.l32_kills_same,
    B.l32_kills_sh_more, B.l32_kills_old_more, B.l32_best_step))
  print(fmt("kept pass: %d cases; maps built %d (in %d cases), dropped %d; points walked new %d, walked again %d (in %d cases)",
    G2.n, G2.map_new, G2.cases_new, G2.map_drop, G2.pt_new, G2.pt_upd, G2.cases_upd))
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
