-- pt2_dgshadow.lua — the degraded search shadow (log only, Nov 27).
--
-- On with env WINBOLO_PT2_DG_SHADOW=1 (init.lua). At each group start
-- (pilltake_v2.group_begin) the full search and each degraded search
-- (the PT2_DG_* knobs, constants.lua) run on a copy of the same inp, in
-- the same tick. A copy has its own job and cache (no doll store, no wall
-- memo), no slicer and its own callbacks (pt2_attack binds them fresh).
-- It reads terrain with util.ttype_peek, not the U.ttype change detector
-- (a tile the copy reads first would be a terrain change on another
-- tick), and env's own fields come back after it. So the played game does
-- not change. The shadow's time is left out of clock_us and
-- brain.lastThinkMs (init.lua).
--
-- stderr lines:
--   DGS  one per run: the run's best (v = the variant, "full" = the full
--        search on the copy)
--   DGR  the full search's safe standoff points per ring (keep_all), when
--        its best is a blocker take
--   DGP  the played group's best (pilltake_v2.job_done)
-- tools: D:/Development/pt2_prof_nov25/degraded/tools/dg_stats.py

local M = {}

local floor = math.floor

M.VARS = {
  { n = "full" },
  { n = "reuse", f = { dg_reuse = true } },
  { n = "local15", f = { dg_local_win = 15, dg_local_every = 8 }, own_local = true },
  { n = "wedge3", f = { dg_step = 3, dg_fine = true } },
  { n = "early", f = { dg_early = true } },
  { n = "rings3", rings3 = true },
  { n = "half2", f = { dg_step = 2, dg_stagger = true, dg_fine = true } },
  { n = "depth2", f = { dg_kcap = 2, dg_tries = 4 } },
  { n = "step2", f = { dg_step = 2 } },
  { n = "step3", f = { dg_step = 3 } },
}

local LOCAL = {}
M.n_skip, M.n_run = 0, 0

local function raw_us()
  local f = rawget(_G, "_dg_clock_raw")
  return f and f() or 0
end

local function err(s)
  local e = io and io.stderr
  if e then e:write(s, "\n") end
end

local function copy_inp(inp, v)
  local cp = {}
  for k, x in pairs(inp) do cp[k] = x end
  cp._dg_copy = true
  cp._v2_cache, cp.v2_pcache, cp._v2_doll, cp._v2_pc_entry, cp._v2_pc_part = nil, nil, nil, nil, nil
  cp._v2_pc_off = true
  -- the wall memo (PT2_WALL_MEMO) is shared module state: a hit is not
  -- counted as slicer work, so a copy that fills it moves the played
  -- run's yields. The copy runs with no memo (same answers).
  cp.wall_memo, cp._wm_t, cp._wm_goal = false, nil, nil
  cp.v2_slice, cp.v2_prof, cp._prof_head, cp.v2_zone_log, cp.v2_nozones = nil, nil, nil, nil, nil
  cp.ring_radii = {}
  for i, r in ipairs(inp.ring_radii or {}) do cp.ring_radii[i] = r end
  inp._dg_rebind(cp)
  if v then
    for k, x in pairs(v.f or {}) do cp[k] = x end
    if v.rings3 and #cp.ring_radii == 4 then table.remove(cp.ring_radii, 2) end
    if v.own_local then cp._dg_local = LOCAL end
  end
  return cp
end

-- a record's blocker kinds (rays that reach the tank and that blockers
-- cover to the bar, the ally cover, the built plan) and its margin
local function kinds_of(b)
  local mg, kinds, nk = nil, {}, 0
  for _, r in ipairs(b.rays or {}) do
    if r.hit == "tank" then
      -- the margin: a chain ray its link covers has no cover bar
      if not r.link_cover then
        local m = (r.cover or 0) - (r.bar or 0)
        if not mg or m < mg then mg = m end
      end
      if r.ok and (r.n_block or 0) > 0 and (r.cover or 0) >= (r.bar or 0) then
        for _, bk in ipairs(r.block or {}) do
          if not kinds[bk.kind] then kinds[bk.kind] = true; nk = nk + 1 end
        end
      end
    end
  end
  if b.ally_cover and not kinds.ally_pill then kinds.ally_pill = true; nk = nk + 1 end
  if b.plan and #b.plan > 0 and not kinds.built then kinds.built = true; nk = nk + 1 end
  return kinds, nk, mg
end

local function kinds_str(kinds)
  local l = {}
  for k in pairs(kinds) do l[#l + 1] = k end
  table.sort(l)
  return #l > 0 and table.concat(l, "+") or "-"
end

local function line(tag, head, v, us, res, extra)
  local b = res and res.best
  local nH = res and res.hostile and #res.hostile or 0
  if not b then
    return string.format("%s %s v=%s us=%d eff=0 blk=0 kinds=- none=1 nH=%d%s", tag, head, v, us, nH, extra or "")
  end
  local eff = b.safe and not b.why_unsafe and not res.no_chain
  local kinds, nk, mg = kinds_of(b)
  local A = res.A
  return string.format(
    "%s %s v=%s us=%d eff=%d blk=%d kinds=%s A=%s,%s deg=%s r=%s k=%d sc=%.2f nb=%d mg=%.2f ac=%d lw=%.1f ex=%.3f st=%.3f nT=%d nH=%d px=%d py=%d%s",
    tag, head, v, us, eff and 1 or 0, (eff and nk > 0) and 1 or 0, kinds_str(kinds),
    tostring(A and A.mx), tostring(A and A.my), tostring(b.deg), tostring(b.radius), b.kills or 0,
    b.score or 0, b.plan and #b.plan or 0, mg or 99, b.ally_cover and 1 or 0, b.leeway or -1,
    b.exposure or -1, b.shells_through or -1, b.T and #b.T or 0, nH, floor(b.px or 0), floor(b.py or 0),
    extra or "")
end

local function head_of(inp)
  local g = inp.A
  return string.format("p=%s t=%s src=%s g=%d,%d", tostring(inp._v2_pc_pn), tostring(inp._v2_pc_now),
    tostring(inp._v2_pc_src or "take"), g.mx, g.my)
end

-- runs of consecutive whole degrees (around 360) in a set
local function runs(set)
  local l = {}
  for d in pairs(set) do l[#l + 1] = d end
  if #l == 0 then return "" end
  table.sort(l)
  local out, lo, prev = {}, l[1], l[1]
  for i = 2, #l do
    if l[i] ~= prev + 1 then out[#out + 1] = { lo, prev }; lo = l[i] end
    prev = l[i]
  end
  out[#out + 1] = { lo, prev }
  if #out > 1 and out[1][1] == 0 and out[#out][2] == 359 then
    out[1][1] = out[#out][1] - 360
    out[#out] = nil
  end
  local s = {}
  for _, r in ipairs(out) do s[#s + 1] = r[1] .. "_" .. r[2] end
  return table.concat(s, ",")
end

-- DGR: the full search's safe points per ring, on the best's A
local function ring_runs(inp, V2, res, head)
  local goalA = inp.A
  local A = res.A or goalA
  local cp = copy_inp(inp, nil)
  cp.keep_all = true
  if A ~= goalA then
    cp.A = A
    if cp.set_A then cp.set_A(A) end
    if cp.chains_only_runs then cp.chains_only = true end
  end
  local t0 = raw_us()
  local ok, r = pcall(V2.evaluate, cp)
  local us = raw_us() - t0
  if not ok or not r or not r.all then return end
  local S, SB, rings = {}, {}, {}
  for _, x in ipairs(r.all) do
    if x.safe and x.radius and x.deg and not x.why_unsafe then
      local R = x.radius
      if not S[R] then S[R], SB[R] = {}, {}; rings[#rings + 1] = R end
      local d = floor(x.deg + 0.5) % 360
      S[R][d] = true
      local _, nk = kinds_of(x)
      if nk > 0 then SB[R][d] = true end
    end
  end
  table.sort(rings, function(a, b) return a > b end)
  local parts = {}
  for _, R in ipairs(rings) do
    parts[#parts + 1] = string.format("%s:S=%s:SB=%s", tostring(R), runs(S[R]), runs(SB[R]))
  end
  local b = res.best
  err(string.format("DGR %s us=%d A=%d,%d bdeg=%s br=%s nall=%d %s", head, us, A.mx, A.my,
    tostring(b and b.deg), tostring(b and b.radius), #r.all, table.concat(parts, " ")))
end

-- WINBOLO_PT2_DG_ONLY (test only): a comma list of the variants to run;
-- "none" runs no search (the copy only)
local ONLY = os and os.getenv and os.getenv("WINBOLO_PT2_DG_ONLY")
local ONLY_SET = nil
if ONLY and ONLY ~= "" then
  ONLY_SET = {}
  for w in ONLY:gmatch("[^,]+") do ONLY_SET[w] = true end
end

-- inp.env is shared with the copies, and the C evaluate writes into it
-- (env.omx, env.omy: the builder's origin). Keep its fields and put them
-- back after the shadow.
local function env_keep(env)
  local k = {}
  for x, v in pairs(env or {}) do k[x] = v end
  return k
end
local function env_back(env, k)
  if not env then return end
  local del = {}
  for x in pairs(env) do if k[x] == nil then del[#del + 1] = x end end
  for _, x in ipairs(del) do env[x] = nil end
  for x, v in pairs(k) do if env[x] ~= v then env[x] = v end end
end

local run_body
function M.run(inp, V2)
  if not inp._dg_rebind or not inp.A then M.n_skip = M.n_skip + 1; return end
  -- test only: WINBOLO_PT2_DG_AT=p:t0:t1 runs the shadow only for player p
  -- on ticks t0..t1 (finding which run changes the played game)
  local at = os and os.getenv and os.getenv("WINBOLO_PT2_DG_AT")
  if at then
    local ap, a0, a1 = at:match("^(%d+):(%d+):(%d+)$")
    local now = tonumber(inp._v2_pc_now) or -1
    if tostring(inp._v2_pc_pn) ~= ap or now < tonumber(a0) or now > tonumber(a1) then return end
  end
  local t0 = raw_us()
  local keep = (not (ONLY_SET and ONLY_SET.noenv)) and env_keep(inp.env)
  local watch
  if ONLY_SET and ONLY_SET.watch then
    watch = {}
    local function snap(name, t)
      if type(t) ~= "table" then return end
      watch[#watch + 1] = { name, t, env_keep(t) }
    end
    snap("inp", inp)
    snap("env", inp.env)
    for i, pl in ipairs(inp.pills or {}) do snap("pill" .. i, pl) end
    if inp._world and inp._world.pills then
      for id, pl in pairs(inp._world.pills) do snap("wpill" .. tostring(id), pl) end
    end
    for i, ch in ipairs(type(inp.chains) == "table" and inp.chains or {}) do
      snap("chain" .. i, ch)
      if type(ch) == "table" then for k, v in pairs(ch) do if type(v) == "table" then snap("chain" .. i .. "." .. tostring(k), v) end end end
    end
    for k, v in pairs(inp) do if type(v) == "table" and k ~= "env" and k ~= "pills" then snap("inp." .. tostring(k), v) end end
  end
  -- U.ttype and U.traw are change detectors (util.lua): a tile the copy
  -- reads first is a change the played game finds on another tick. The
  -- shadow reads terrain with the peek readers only (same values).
  local U = require("util")
  local o_tt, o_raw = U.ttype, U.traw
  local env = inp.env
  local o_ett = env and env.ttype
  if not (ONLY_SET and ONLY_SET.nopeek) then
    U.ttype = U.ttype_peek
    U.traw = function(mx, my) return get_terrain(mx, my) end
    if env and rawequal(o_ett, o_tt) then env.ttype = U.ttype_peek end
  end
  local ok_b, err_b = pcall(run_body, inp, V2)
  U.ttype, U.traw = o_tt, o_raw
  if env then env.ttype = o_ett end
  if not ok_b then error(err_b, 0) end
  if watch then
    for _, w in ipairs(watch) do
      local name, t, old = w[1], w[2], w[3]
      for k, v in pairs(t) do
        if old[k] ~= v then err(string.format("DGX %s changed %s.%s %s -> %s", head_of(inp), name, tostring(k), tostring(old[k]), tostring(v))) end
      end
      for k, v in pairs(old) do
        if t[k] == nil then err(string.format("DGX %s removed %s.%s", head_of(inp), name, tostring(k))) end
      end
    end
  end
  if keep then env_back(inp.env, keep) end
  local dt = raw_us() - t0
  _G._dg_hide_us = (_G._dg_hide_us or 0) + dt
  _G._dg_think_us = (_G._dg_think_us or 0) + dt
end

run_body = function(inp, V2)
  if ONLY_SET and ONLY_SET.none then
    local t0 = raw_us()
    copy_inp(inp, nil)
    if ONLY_SET.spin then while raw_us() - t0 < (ONLY_SET.spin300 and 300000 or 20000) do end end
    if ONLY_SET.junk then
      local j = {}
      for i = 1, 20000 do j[i] = { i, i + 1, tostring(i) } end
    end
    return
  end
  local lw, lc = V2.last_work, V2.last_cprof
  local head = head_of(inp)
  local full
  for _, v in ipairs(M.VARS) do
    if ONLY_SET and not ONLY_SET[v.n] then goto next_v end
    local cp = copy_inp(inp, v)
    if ONLY_SET and ONLY_SET.noscore then
      cp.score_fn = function() return 0 end
      cp.banned = function() return false end
    end
    if ONLY_SET and ONLY_SET.nochain then cp.chains = nil end
    if ONLY_SET and ONLY_SET.noxc then cp.extra_cover_walls = 0 end
    if ONLY_SET and ONLY_SET.nolgm then cp.lgm_ticks = function() return nil end end
    local t0 = raw_us()
    local ok, res = pcall((ONLY_SET and ONLY_SET.single) and V2.evaluate or V2.evaluate_group, cp)
    local us = raw_us() - t0
    if not ok then
      err(string.format("DGS %s v=%s err=%s", head, v.n, tostring(res):gsub("%s", "_")))
    else
      local fb = res.v2 and res.v2.stat and res.v2.stat.dg_fb
      err(line("DGS", head, v.n, us, res, fb and string.format(" fb=%d", fb) or ""))
      if v.n == "full" then full = res end
    end
    ::next_v::
  end
  if full and full.best then
    local b = full.best
    local _, nk = kinds_of(b)
    if b.safe and not b.why_unsafe and not full.no_chain and nk > 0 and not ONLY_SET then ring_runs(inp, V2, full, head) end
  end
  V2.last_work, V2.last_cprof = lw, lc
  M.n_run = M.n_run + 1
end

function M.played(job, V2)
  local inp = job.inp
  if not inp or not job.goalA or not inp._dg_rebind then return end
  local g = job.goalA
  local head = string.format("p=%s t=%s src=%s g=%d,%d", tostring(inp._v2_pc_pn), tostring(inp._v2_pc_now),
    tostring(inp._v2_pc_src or "take"), g.mx, g.my)
  local t0 = raw_us()
  err(line("DGP", head, "played", 0, job.res, ""))
  local dt = raw_us() - t0
  _G._dg_hide_us = (_G._dg_hide_us or 0) + dt
  _G._dg_think_us = (_G._dg_think_us or 0) + dt
end

return M
