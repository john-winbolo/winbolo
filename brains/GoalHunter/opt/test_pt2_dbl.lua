-- =========================================================================
-- test_pt2_dbl.lua -- standalone unit tests (2026-11-18) for pt2_dbl.lua,
-- the PT2_DOUBLE_TAKE_DISCOUNT cache pool 6 reads, and the row text.
--
--   * knob off: no run, no state written.
--   * the partner filter, one run per tick, the think-time limit.
--   * the cache: kept while true, dropped on health / owner / carried /
--     trees / age / a wall square near a chain pill.
--   * the row condition (safe, kills >= 2, carried pills and trees for
--     the plan; have/need not checked).
--   * the run cost before the first run (first tick, seed, last think).
--   * the exclusions: summed contrib, the other pills' coverage disks.
--   * the row text: chip and every excluded amount.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_dbl.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

-- ── Stubs: v2 answers what the test sets ──────────────────────────────────
local runs = 0
local answer = nil        -- res.best the stub returns
package.loaded["pt2_attack"] = {
  build_inp = function(state, goal, info, world, pill)
    local ids, pills = {}, {}
    for id, p in pairs(world.pills) do ids[p] = id; pills[#pills + 1] = p end
    return { ids = ids, A = pill, pills = pills }
  end,
}
-- split jobs: each group_step is one part of part_us on the fake clock
local T = 1000000
_G.clock_us = function() return T end
local part_us, n_parts_job = 900, 3
package.loaded["pilltake_v2"] = {
  evaluate_group = function(inp)
    runs = runs + 1
    return { best = answer }
  end,
  group_begin = function(inp) return { inp = inp, list = {}, i = 0, res = nil } end,
  group_step = function(job)
    runs = runs + 1
    T = T + part_us
    job.i = job.i + 1
    if job.i >= n_parts_job then job.res = { best = answer, A = job.inp.A }; return true end
    return false
  end,
  group_parts_left = function(job) return n_parts_job - job.i end,
}
_G.brain = nil

local D = require("pt2_dbl")

local function pill(mx, my, hp, owner) return { mx = mx, my = my, health = hp or 15, owner = owner or "hostile" } end
local A, B = pill(100, 100), pill(106, 100)
local FAR = pill(160, 160)
local world = { pills = { [1] = A, [2] = B, [3] = FAR } }
local info = { carried_pills = 1, trees = 10, man_status = 0 }
local function chain_answer()
  return { safe = true, chain_mode = true, kills = 2, have = 10, need = 10,
           chain = { A, B }, plan = { { kind = "pill" }, { kind = "wall" }, { kind = "wall" } } }
end

local saved = {}
for _, k in ipairs({ "PT2_DOUBLE_TAKE_DISCOUNT", "PILLTAKE_V2_EVALUATOR", "PT2_CHAINS", "PT2_DBL_SPLIT" }) do saved[k] = C[k] end
C.PT2_DBL_SPLIT = false   -- the one-go path first; the split has its own section

-- ── knob off ──────────────────────────────────────────────────────────────
print("knob off")
C.PT2_DOUBLE_TAKE_DISCOUNT = false
local state = { tick = 0 }
answer = chain_answer()
check("off: no answer", D.get(state, world, info, 1, A, 10) == nil)
check("off: no run", runs == 0, runs)
check("off: no state", state._pt2_dbl == nil)
C.PT2_DOUBLE_TAKE_DISCOUNT = true
C.PT2_CHAINS = false
check("chains off: no answer", D.get(state, world, info, 1, A, 10) == nil)
C.PT2_CHAINS = true
C.PILLTAKE_V2_EVALUATOR = true

-- ── partner filter and run limit ─────────────────────────────────────────
print("filter and limit")
check("FAR has no partner", D.has_partner(world, FAR) == false)
check("A has partner B", D.has_partner(world, A) == true)
check("FAR: no answer", D.get(state, world, info, 3, FAR, 10) == nil)
check("FAR: no run", runs == 0, runs)
local e = D.get(state, world, info, 1, A, 10)
check("A: run once", runs == 1 and e ~= nil, runs)
check("A: chain ids 1-2", e and table.concat(e.ids, "-") == "1-2", e and table.concat(e.ids, "-"))
check("A: walls 2, trees need 4", e and e.walls == 2 and e.trees_need == 4)
check("A: plan drops 1 pill", e and e.pills == 1)
check("B same tick: limit 1 per tick", D.get(state, world, info, 2, B, 10) == nil and runs == 1, runs)
check("A same tick: cached", D.get(state, world, info, 1, A, 10) == e and runs == 1, runs)
check("B next tick: runs", D.get(state, world, info, 2, B, 11) ~= nil and runs == 2, runs)
-- think-time limit
state._pt2_dbl.by_id[2] = nil
state._pt2_dbl.est_ms = 2
_G.brain = { lastThinkMs = 9, targetMs = 10 }
check("no time left: no run", D.get(state, world, info, 2, B, 12) == nil and runs == 2, runs)
_G.brain = { lastThinkMs = 7, targetMs = 10 }
check("time left: runs", D.get(state, world, info, 2, B, 13) ~= nil and runs == 3, runs)
_G.brain = nil

-- ── run cost before the first run ────────────────────────────────────────
print("first run cost")
do
  local st = { tick = 0 }
  local n0 = runs
  _G.brain = { lastThinkMs = 0, targetMs = 10 }
  check("first tick (no last think): no run", D.get(st, world, info, 1, A, 1) == nil and runs == n0, runs)
  _G.brain = { lastThinkMs = 6, targetMs = 10 }
  check("unseeded: estimate = last think", D._est_ms(st._pt2_dbl, st, 6) == 6)
  check("unseeded, last 6 of 10: no run", D.get(st, world, info, 1, A, 2) == nil and runs == n0, runs)
  st._pt2_v2_last_us = 3000
  check("seeded from the take's v2 run", D._est_ms(st._pt2_dbl, st, 6) == 3)
  check("seed 3 + last 6 < 10: runs", D.get(st, world, info, 1, A, 3) ~= nil and runs == n0 + 1, runs)
  local s = st._pt2_dbl
  check("first run sets the estimate to its own cost", s.n_runs == 1 and s.est_ms == s.run_us_sum / 1000, s.est_ms)
  check("after a run: own estimate, not the seed", D._est_ms(s, st, 6) == s.est_ms)
  _G.brain = { lastThinkMs = 0, targetMs = 0 }
  check("no target published: no limit", D._est_ms(s, st, 0) == s.est_ms)
  _G.brain = nil
end

-- ── cache drops ──────────────────────────────────────────────────────────
print("cache")
local function fresh(t)
  state._pt2_dbl.by_id = {}
  local r = D.get(state, world, info, 1, A, t)
  return r
end
local n
fresh(100); n = runs
check("kept at age 250", D.get(state, world, info, 1, A, 350) ~= nil and runs == n, runs)
check("dropped at age 251", D.get(state, world, info, 1, A, 351) ~= nil and runs == n + 1, runs)
fresh(400); n = runs
B.health = 14
check("B health change: run again", D.get(state, world, info, 1, A, 401) ~= nil and runs == n + 1, runs)
fresh(500); n = runs
B.owner = "neutral"
check("B owner change: run again", D.get(state, world, info, 1, A, 501) ~= nil and runs == n + 1, runs)
B.owner = "hostile"
fresh(600); n = runs
info.trees = 9
check("trees change: run again", D.get(state, world, info, 1, A, 601) ~= nil and runs == n + 1, runs)
fresh(700); n = runs
info.carried_pills = 0
check("carried change: run again", D.get(state, world, info, 1, A, 701) ~= nil and runs == n + 1, runs)
info.carried_pills, info.trees = 1, 10
fresh(800); n = runs
D.on_tile(state, 106 + 9, 100, C.T_GRASS)
check("grass near B: kept", state._pt2_dbl.by_id[1] ~= nil)
D.on_tile(state, 106 + 10, 100, C.T_BUILDING)
check("wall 10 from B: kept", state._pt2_dbl.by_id[1] ~= nil)
D.on_tile(state, 106 + 9, 100, C.T_BUILDING)
check("wall 9 from B: dropped", state._pt2_dbl.by_id[1] == nil)

-- ── PT2_DBL_SPLIT: a run as a job of parts ─────────────────────────────
print("split")
do
  C.PT2_DBL_SPLIT = true
  local st = { tick = 10 }
  local function at(t) st.tick = t; return t end
  local n0 = runs
  part_us, n_parts_job = 900, 3
  -- no brain: the goal alone (2.0 ms); before any part the estimate is the goal
  check("start: no answer yet", D.get(st, world, info, 1, A, at(10)) == nil)
  local s = st._pt2_dbl
  check("start: one part ran", runs == n0 + 1 and s.job and s.job.n == 1, runs - n0)
  check("start: estimate = the part", math.abs(s.est_part_ms - 0.9) < 1e-9, s.est_part_ms)
  check("open job: no second job", D.get(st, world, info, 2, B, at(10)) == nil and s.n_jobs == 1)
  D.pump(st, world, info, at(10))
  check("same tick: 1 more part fits (0.9 + 0.9 <= 2.0)", s.job and s.job.n == 2, s.job and s.job.n)
  D.pump(st, world, info, at(11))
  check("next tick: done after 3 parts", s.job == nil and s.by_id[1] ~= nil and runs == n0 + 3, runs - n0)
  check("done: 2 ticks", s.job_ticks[2] == 1)
  check("done: cached answer", D.get(st, world, info, 1, A, at(12)) == s.by_id[1])
  -- the room term: tick target less this think so far less the margin
  s.by_id[2] = nil
  _G.brain = { lastThinkMs = 5, targetMs = 10 }
  st._think_t0_us = T - 7500            -- 7.5 ms so far: room 0.5 < 0.9
  check("no room: no start", D.get(st, world, info, 2, B, at(13)) == nil and s.job == nil)
  check("goal = room", math.abs(D._goal_ms(s, st) - 0.5) < 1e-9, D._goal_ms(s, st))
  st._think_t0_us = T - 7000            -- 7.0 ms: room 1.0 >= 0.9
  check("room for one part: start", D.get(st, world, info, 2, B, at(14)) == nil and s.job and s.job.n == 1)
  D.pump(st, world, info, at(14))
  check("room spent: wait", s.job.n == 1 and s.n_wait == 1, s.n_wait)
  st._think_t0_us = T
  D.pump(st, world, info, at(15))
  check("fresh tick: 2 parts, done", s.job == nil and s.by_id[2] ~= nil)
  _G.brain = nil
  st._think_t0_us = nil
  -- drop: a snapshot pill changes
  s.by_id[1] = nil
  D.get(st, world, info, 1, A, at(20))
  B.health = 13
  D.pump(st, world, info, at(21))
  check("pill change: dropped", s.job == nil and s.n_drop == 1 and s.by_id[1] == nil)
  B.health = 15
  -- drop: a wall near the target
  D.get(st, world, info, 1, A, at(30))
  D.on_tile(st, 100 + 3, 100, C.T_BUILDING)
  D.pump(st, world, info, at(30))
  check("wall near the job: dropped", s.job == nil and s.n_drop == 2)
  -- the cap: a job not done in dbl_split ticks is dropped
  st._capacity = { dbl_split = 3 }
  part_us, n_parts_job = 1500, 9      -- 1.5 ms parts: one per tick
  D.get(st, world, info, 1, A, at(40))
  D.pump(st, world, info, at(41)); D.pump(st, world, info, at(42))
  check("cap: still open at tick 3", s.job ~= nil and s.job.n == 3, s.job and s.job.n)
  D.pump(st, world, info, at(43))
  check("cap: dropped at the cap", s.job == nil and s.n_drop_cap == 1, s.n_drop_cap)
  st._capacity = nil
  -- one part alone over the goal
  local nb = s.n_big
  part_us, n_parts_job = 2500, 1
  s.est_part_ms = 1.0
  s.by_id[3] = nil
  world.pills[4] = pill(108, 100)       -- a partner for a fresh pill
  local P4 = world.pills[4]
  D.get(st, world, info, 4, P4, at(50))
  check("big part counted", s.n_big == nb + 1 and s.big_us_sum >= 2500, s.n_big)
  check("big part: est over the goal, next waits", D._est_part_ms(s, st) > 1.0)
  world.pills[4] = nil
  part_us, n_parts_job = 900, 3
  C.PT2_DBL_SPLIT = false
end

-- ── row condition ────────────────────────────────────────────────────────
print("condition")
local base = { safe = true, kills = 2, ids = { 1, 2 }, have = 10, need = 10, trees = 4, trees_need = 4,
               pills = 1, carried = 1 }
local function with(k, v) local t = {}; for a, b in pairs(base) do t[a] = b end; t[k] = v; return t end
check("qualifies", D.qualifies(base) == true)
check("unsafe", select(2, D.qualifies(with("safe", false))) == "unsafe")
check("one kill", select(2, D.qualifies(with("kills", 1))) == "kills")
check("have 0 < need 10, plan drops our pill: qualifies", D.qualifies(with("have", 0)) == true)
check("plan drops more pills than carried", select(2, D.qualifies(with("pills", 2))) == "pills")
check("trees short", select(2, D.qualifies(with("trees", 3))) == "trees")
check("nil", select(2, D.qualifies(nil)) == "none")

-- ── exclusions ───────────────────────────────────────────────────────────
print("exclusions")
local ka, kb = 100 * 256 + 100, 100 * 256 + 106
local threat = { pill_contrib = {
  [ka] = { [5] = 10, [6] = 4 },
  [kb] = { [6] = 3, [7] = 2 },
} }
local ent = { pos = { { 100, 100 }, { 106, 100 } }, ids = { 1, 2 } }
local ex, other = D.exclusions(ent, threat)
check("contrib sums A and B", ex.contrib[5] == 10 and ex.contrib[6] == 7 and ex.contrib[7] == 2)
check("other is B only", other[5] == nil and other[6] == 3 and other[7] == 2)
local ncov, on_b, off = 0, ex.cov_excl[kb], ex.cov_excl[100 * 256 + 116]
for _ in pairs(ex.cov_excl) do ncov = ncov + 1 end
check("cov disk R=9 has 253 squares", ncov == 253, ncov)
check("cov on B = 1", on_b == 1, on_b)
check("cov 10 squares off = nil", off == nil, off)
check("cov target not excluded", ex.cov_excl[ka] == 1, ex.cov_excl[ka])  -- A is 6 from B: in B's disk
local ex2 = D.exclusions(ent, threat)
check("same tables: cached", ex2.contrib == ex.contrib)
threat.pill_contrib[kb] = { [6] = 1 }
local ex3 = D.exclusions(ent, threat)
check("threat rebuild: new sum", ex3.contrib ~= ex.contrib and ex3.contrib[6] == 5)
local sx = D.other_set({ ids = { 1, 2, 5 } })
check("other set", sx[2] and sx[5] and not sx[1])

-- ── row text ─────────────────────────────────────────────────────────────
print("row text")
local PR = require("pool_render")
local row = { _p = 6, cost = 100, raw = 12, _mx = 100, _my = 100, _id = 1, _spot = 40, _spot_mx = 95, _spot_my = 100,
  _travel = 12, _travel_wound = 1, _stale = 0, _diff = 30, _anger = 0, _xfire = 0, _intcpt = 0,
  _hp = 1, _wound = 1, _hpv = 15, _ammo = 0, _sh_now = 40, _ttc = 0, _pa = 0, _age = 0, _fin_mult = 1, _fin_wphp = 0, _fin_age = 0, _fin_wpid = -1, _commit_mult = 1, _sh_end = 25,
  _dbl = { mult = 0.6, ids = { 1, 2 }, kills = 2, have = 10, need = 10, pills = 1, walls = 2, age = 7,
           diff1 = 130, xfire_ex = 25, spot_ex = 18, travel_ex = 0 } }
local f = PR.get_formula(row) or ""
check("chip", f:find("x dbl{0.60 chain=1-2 kills=2 safe plan=1P+2W have=10/need=10 age=7t}", 1, true) ~= nil, f)
check("spot -chain", f:find("spot{40 -chain 18}", 1, true) ~= nil, f)
check("diff single", f:find("diff{30 single=130}", 1, true) ~= nil, f)
check("xfire -chain", f:find("xfire{0 -chain 25}", 1, true) ~= nil, f)
check("pickup -chain", f:find("pickup{12 -chain 0}", 1, true) ~= nil, f)
row._dbl.skip = "blitz"; row.formula = nil
f = PR.get_formula(row) or ""
check("blitz skip chip", f:find("x dbl{skip: blitz chain=1-2}", 1, true) ~= nil, f)
row._dbl = nil; row.formula = nil
f = PR.get_formula(row) or ""
check("no dbl: no chip", f:find("dbl{", 1, true) == nil and f:find("-chain", 1, true) == nil, f)

for k, v in pairs(saved) do C[k] = v end
print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
