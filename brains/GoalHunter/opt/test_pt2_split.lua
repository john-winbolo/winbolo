-- =========================================================================
-- test_pt2_split.lua -- standalone unit tests (2026-11-21, F2) for
-- PT2_AFTER_SCAN_SPLIT: pt2_attack.split_get / split_step / split_done, v2
-- after the shield scan in parts over brain ticks.
--
--   * parity: the split run gives the same answer as after_scan in one go
--     (master on and master off with the debug log);
--   * a budget kill in v1's tick, in each v2 part and in finish: the plan
--     marker stays, the part runs again, the answer is the same;
--   * the same part killed SHIELD_SCAN_BLACKLIST_TRIES times: v1's plan;
--   * every kill case ends with a plan set (v1's or v2's), never neither;
--   * the PT2_AFTER_SCAN_MAX_TICKS work-tick maximum;
--   * stale runs are dropped and start again; too many drops keep v1.
--
-- The tick driver below does what attack.lua's deferred shield scan does,
-- in the same order: split_get, v1 (unless the run holds v1's result),
-- split_step, then goal._shield_scan set and the pending flag cleared.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_split.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local C   = require("constants")
-- these tests stub pilltake_v2_old: run the old evaluator (pt2_eval)
C.PT2_EVALUATOR = "old"
local U   = require("util")
local cpf = require("cpathfinder")
local SH  = require("shadow")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

-- ── The map (test_pt2_attack.lua's stand-ins) ────────────────────────────
local terrain = {}
local function sq(mx, my) return my * 256 + mx end
U.ttype      = function(mx, my) return terrain[sq(mx, my)] or C.T_GRASS end
U.ttype_peek = U.ttype
U.base_tile_stops_tank_shell = function() return false end
cpf.SHOT_TANK = cpf.SHOT_TANK or 0
cpf.SHOT_PILL = cpf.SHOT_PILL or 1
cpf.lgm_travel_ticks_map = function(omx, omy, mx, my)
  if U.is_water(U.ttype(mx, my)) then return -1 end
  return 10
end
local _pts = {}
cpf.simulate_shot = function(ox, oy, tx, ty, kind)
  ox, oy = math.floor(ox + 0.5), math.floor(oy + 0.5)
  tx, ty = math.floor(tx + 0.5), math.floor(ty + 0.5)
  local angle, len
  if kind == cpf.SHOT_PILL then angle, len = SH.calc_angle(ox, oy, tx, ty), SH.PILL_FIRE_LEN
  else angle, len = SH.tank_angle(ox, oy, tx, ty), SH.TANK_FIRE_LEN end
  local out, last = {}, -1
  local function add(x, y)
    local mx, my = math.floor(x / 256), math.floor(y / 256)
    local k = sq(mx, my)
    if k ~= last then last = k; out[#out + 1] = { mx = mx, my = my } end
  end
  add(ox, oy)
  local xa, ya = SH.calc_distance(angle, SH.SHELL_SPEED)
  add(ox + SH.SHELL_START_ADD * xa, oy + SH.SHELL_START_ADD * ya)
  local pts, n = SH.shell_points(ox, oy, angle, len, _pts)
  for t = 1, n do add(pts[2 * t - 1], pts[2 * t]) end
  return out
end
cpf.KIND_NORMAL = cpf.KIND_NORMAL or 0
local HOME = { 128.5, 134.5 }
cpf.dijkstra_lookup_by_kind = function(kind, mx, my)
  local dx, dy = mx + 0.5 - HOME[1], my + 0.5 - HOME[2]
  return math.sqrt(dx * dx + dy * dy)
end
cpf.estimate_cost = function() return 1e9 end

local lines = {}
package.loaded["print2"] = setmetatable({}, { __call = function(_, s) lines[#lines + 1] = s end })
local function find_line(pat, from)
  for i = from or 1, #lines do if lines[i]:find(pat) then return lines[i] end end
  return nil
end

_G.gh_shield = nil
package.loaded["attack_shield"] = nil
local S   = require("attack_shield")
local V2  = require("pilltake_v2_old")
local PA  = require("pt2_attack")

local PMX, PMY = 128, 120
-- The target and a group: extra pills in pill range of it, so the run has
-- parts past the target's own evaluate.
local GROUP = { { 131, 120 }, { 125, 121 }, { 129, 117 }, { 126, 118 }, { 130, 122 }, { 127, 123 } }
local function make_world(n_group)
  local target = { mx = PMX, my = PMY, owner = "hostile", health = 15 }
  local pills, pill_at = { [1] = target }, { [sq(PMX, PMY)] = { { id = 1, pill = target } } }
  for i = 1, n_group or 0 do
    local g = GROUP[i]
    local p = { mx = g[1], my = g[2], owner = "hostile", health = 15 }
    pills[i + 1] = p
    pill_at[sq(g[1], g[2])] = { { id = i + 1, pill = p } }
  end
  return { pills = pills, pill_at = pill_at, bases = {}, base_at = {} }, target
end

local function spots()
  local out = {}
  for deg = 0, 315, 45 do out[#out + 1] = { deg = deg, has_los = true, _adj_score = 1 } end
  return out
end

local function v1_scan(world, pill)
  local R = C.PPT_STANDOFF or 7.0
  local cx, cy = PMX + 0.5, PMY + 0.5 + R
  return S.scan(pill, world, math.floor(cx), math.floor(cy), 180, cx, cy, R, false, 40, nil, nil, 0)
end

local INFO = { tankx = 128 * 256, tanky = 134 * 256, armour = 40 }
local KEY = "k1"

local function args(goal, world, pill, sscan, now, state)
  return { state = state or {}, goal = goal, info = INFO, world = world, pill = pill, sscan = sscan,
           no_builder = false, npb = 0, radius = C.PPT_STANDOFF or 7.0, now = now }
end

-- Deep equality; functions count as equal (closures are new each run).
local function deep_eq(a, b, seen)
  if type(a) ~= type(b) then return false end
  if type(a) == "function" then return true end
  if type(a) ~= "table" then
    if type(a) == "number" and a ~= a and b ~= b then return true end
    return a == b
  end
  if a == b then return true end
  seen = seen or {}
  if seen[a] == b then return true end
  seen[a] = b
  for k, v in pairs(a) do if not deep_eq(v, b[k], seen) then return false end end
  for k in pairs(b) do if a[k] == nil then return false end end
  return true
end

-- One brain tick of attack.lua's deferred shield scan. v1 is shield.scan;
-- env.kill_v1 > 0 kills it (and counts down). Returns "idle", "wait",
-- "set", or "killed" (a budget kill unwound the think).
local function tick(env)
  local goal = env.goal
  if not goal._shield_scan_pending then return "idle" end
  local ok, r = pcall(function()
    local sp = PA.split_get(goal, env.pill, env.key or KEY, env.now)
    local sscan
    if sp then
      sscan = sp.sscan
    else
      if (env.kill_v1 or 0) > 0 then env.kill_v1 = env.kill_v1 - 1; error("tick_budget_exceeded (v1)", 0) end
      sscan = env.v1
      env.v1_runs = (env.v1_runs or 0) + 1
    end
    local out, ready = PA.split_step(sp, args(goal, env.world, env.pill, sscan, env.now, env.state), env.key or KEY)
    if not ready then return "wait" end
    goal._shield_scan = out
    goal._shield_scan_pending = nil
    PA.split_done(goal)
    return "set"
  end)
  env.now = env.now + 1
  if ok then return r end
  if tostring(r):find("tick_budget_exceeded", 1, true) then return "killed" end
  error(r, 0)
end

-- Runs ticks until the plan is set (or 40 ticks). Returns the tick results.
local function run(env)
  local res = {}
  for _ = 1, 40 do
    local r = tick(env)
    res[#res + 1] = r
    if r == "set" or r == "idle" then break end
  end
  return res
end

local function new_env(n_group, now)
  local world, pill = make_world(n_group)
  local env = { world = world, pill = pill, now = now or 100, state = {} }
  env.goal = { scan_spots = spots(), _is_ppt = true, target_id = 1, mx = PMX, my = PMY,
               _shield_scan_pending = true }
  env.v1 = v1_scan(world, pill)
  return env
end

-- The one-go answer, as after_scan gives it on tick now.
local function one_go(n_group, now)
  local world, pill = make_world(n_group)
  local goal = { scan_spots = spots(), _is_ppt = true, target_id = 1, mx = PMX, my = PMY }
  local v1 = v1_scan(world, pill)
  local out = PA.after_scan(args(goal, world, pill, v1, now))
  return out, goal, v1
end

-- A budget kill at the n-th call of fn (module tbl, field name); once by
-- default, times = how many calls in a row from the n-th.
local function kill_at(tbl, name, n, times)
  local real, calls, left = tbl[name], 0, times or 1
  tbl[name] = function(...)
    calls = calls + 1
    if calls >= n and left > 0 then left = left - 1; error("tick_budget_exceeded (test)", 0) end
    return real(...)
  end
  return function() tbl[name] = real end
end

local function count(t, v) local n = 0; for _, x in ipairs(t) do if x == v then n = n + 1 end end; return n end

C.PT2_AFTER_SCAN_SPLIT = true
C.PT2_AFTER_SCAN_MAX_TICKS = 5
-- the packed rule first; the PT2_SPLIT_ONE_PART_PER_TICK section turns it on
C.PT2_SPLIT_ONE_PART_PER_TICK = false
_G.brain = { targetMs = 0 }        -- no target: every part may run at once

-- ── parity ───────────────────────────────────────────────────────────────
for _, mode in ipairs({ "master on", "master off, debug log" }) do
  print("== parity, " .. mode)
  C.PILLTAKE_V2_EVALUATOR = (mode == "master on")
  _G.BRAIN_DEBUG_MODE = (mode ~= "master on"); _G._PRINT2_ENABLED = _G.BRAIN_DEBUG_MODE
  for _, ng in ipairs({ 0, 3 }) do
    local env = new_env(ng, 100)
    local res = run(env)
    -- plan tick 100 runs v1 only; part 0 runs on tick 101: one go on 101
    local ref, rgoal, rv1 = one_go(ng, 101)
    local out = env.goal._shield_scan
    check(string.format("group %d: plan tick waits, then set", ng), res[1] == "wait" and res[#res] == "set", table.concat(res, ","))
    check(string.format("group %d: v1 ran once", ng), env.v1_runs == 1, env.v1_runs)
    if mode == "master on" then
      check(string.format("group %d: v2 answer (pt2)", ng), out and out.pt2 == true, out and out.pt2)
      check(string.format("group %d: same answer as one go", ng), deep_eq(out, ref), out)
      check(string.format("group %d: same goal._pt2", ng), deep_eq(env.goal._pt2, rgoal._pt2), env.goal._pt2)
    else
      check(string.format("group %d: v1 answer kept (same table)", ng), out == env.v1, out)
      check(string.format("group %d: goal._pt2 not set", ng), env.goal._pt2 == nil, env.goal._pt2)
    end
    check(string.format("group %d: run closed", ng), env.goal._pt2_split == nil, env.goal._pt2_split)
    check(string.format("group %d: pending cleared", ng), env.goal._shield_scan_pending == nil, env.goal._shield_scan_pending)
  end
end
_G.BRAIN_DEBUG_MODE = true; _G._PRINT2_ENABLED = true
C.PILLTAKE_V2_EVALUATOR = true

-- The opt copy strips the debug log lines: LOGS false, the line checks
-- are skipped there and PA.last_run / PA.last_drop are checked alone.
local LOGS
print("== PT2_SPLIT line")
do
  local n0 = #lines + 1
  local env = new_env(3, 100)
  run(env)
  local l = find_line("^PT2_SPLIT ", n0)
  LOGS = l ~= nil
  print("    " .. tostring(l or "(no log lines: opt copy)"))
  if LOGS then
    check("PT2_SPLIT line with parts, ms, ticks", l:find("ms=[%d.,]+ total_ms=") and l:find("work_ticks=1"), l)
  end
  local r = PA.last_run
  check("parts = build_inp + target + 3 group pills", r and r.parts == 5, r and r.parts)
  check("1 work tick, no kills", r and r.work == 1 and r.kills == 0 and not r.giveup, r and r.work)
end

-- ── budget kills ─────────────────────────────────────────────────────────
local REF3 = one_go(3, 101)

print("== kill in v1's tick")
do
  local env = new_env(3, 100)
  env.kill_v1 = 1
  local res = run(env)
  check("first tick killed, pending kept", res[1] == "killed", res[1])
  check("then plan tick, then set", res[2] == "wait" and res[#res] == "set", table.concat(res, ","))
  check("plan set", env.goal._shield_scan ~= nil, env.goal._shield_scan)
  check("v2 answer (pt2)", env.goal._shield_scan and env.goal._shield_scan.pt2 == true, nil)
end

-- part 0 = build_inp (V2.world_inp), part k >= 1 = the k-th V2.evaluate
local function kill_case(name, tbl, fname, n, times, want, ref)
  print("== " .. name)
  local env = new_env(3, 100)
  local n0 = #lines + 1
  local undo = kill_at(tbl, fname, n, times)
  local res = run(env)
  undo()
  local goal = env.goal
  check("killed ticks: " .. (times or 1), count(res, "killed") == (times or 1), table.concat(res, ","))
  check("ends with a plan set", res[#res] == "set" and goal._shield_scan ~= nil, table.concat(res, ","))
  check("pending cleared at the end only", goal._shield_scan_pending == nil, goal._shield_scan_pending)
  check("v1 ran once (kept by the run)", env.v1_runs == 1, env.v1_runs)
  if want == "v2" then
    check("same answer as one go", deep_eq(goal._shield_scan, ref or REF3), goal._shield_scan)
    local r = PA.last_run
    check("run kills=" .. (times or 1), r and r.kills == (times or 1) and not r.giveup, r and r.kills)
    if LOGS then
      local l = find_line("^PT2_SPLIT ", n0)
      check("PT2_SPLIT kills=" .. (times or 1), l and l:find("kills=" .. (times or 1), 1, true), l)
    end
  else
    check("v1's plan kept (same table)", goal._shield_scan == env.v1, goal._shield_scan)
    check("goal._pt2 cleared", goal._pt2 == nil, goal._pt2)
    check("gave up, why=kills", PA.last_run and PA.last_run.giveup == "kills", PA.last_run and PA.last_run.giveup)
    if LOGS then
      local l = find_line("^PT2_SPLIT_GIVEUP", n0)
      print("    " .. tostring(l))
      check("PT2_SPLIT_GIVEUP why=kills", l and l:find("why=kills", 1, true), l)
    end
  end
  return env, res
end

-- part 0 killed on tick 101 runs again on 102: the inputs are 102's
kill_case("kill in part 0 (build_inp)", V2, "world_inp", 1, 1, "v2", (one_go(3, 102)))
for k = 1, 4 do
  kill_case(string.format("kill in part %d (evaluate %d)", k, k), V2, "evaluate", k, 1, "v2")
end
kill_case("kill in finish", V2, "to_scan_result", 1, 1, "v2")

-- the marker after a kill in a middle part
print("== marker after a kill in part 2")
do
  local env = new_env(3, 100)
  check("plan tick waits", tick(env) == "wait", nil)
  local undo = kill_at(V2, "evaluate", 2)
  local r = tick(env)
  undo()
  local sp = env.goal._pt2_split
  check("tick killed", r == "killed", r)
  check("pending kept", env.goal._shield_scan_pending == true, env.goal._shield_scan_pending)
  check("run kept, part 2 marked", sp and sp.in_part == 2, sp and sp.in_part)
  check("parts 0 and 1 kept", sp and sp.n == 2, sp and sp.n)
  check("no plan yet, no v2 chain", env.goal._shield_scan == nil, env.goal._shield_scan)
  local res = run(env)
  check("next tick runs part 2 again and sets the plan", res[1] == "set", table.concat(res, ","))
  check("same answer as one go", deep_eq(env.goal._shield_scan, REF3), nil)
end

print("== repeated kills: give up to v1")
kill_case("part 0 killed twice", V2, "world_inp", 1, 2, "v1")
kill_case("part 3 killed twice", V2, "evaluate", 3, 2, "v1")
kill_case("finish killed twice", V2, "to_scan_result", 1, 2, "v1")
do
  -- one kill in each of two parts: no give up, the v2 answer
  print("== one kill in part 1 and one in part 3")
  local env = new_env(3, 100)
  local real, calls = V2.evaluate, 0
  V2.evaluate = function(...)
    calls = calls + 1
    if calls == 1 or calls == 4 then error("tick_budget_exceeded (test)", 0) end
    return real(...)
  end
  local res = run(env)
  V2.evaluate = real
  check("two killed ticks", count(res, "killed") == 2, table.concat(res, ","))
  check("v2 answer, same as one go", deep_eq(env.goal._shield_scan, REF3), nil)
end
do
  -- a v2 error (not a budget kill): v1's plan, as after_scan
  print("== v2 error in a part")
  local env = new_env(3, 100)
  local real = V2.evaluate
  V2.evaluate = function() error("test fault") end
  local res = run(env)
  V2.evaluate = real
  check("plan set, v1's", res[#res] == "set" and env.goal._shield_scan == env.v1, table.concat(res, ","))
end

-- ── the work-tick maximum ────────────────────────────────────────────────
print("== PT2_AFTER_SCAN_MAX_TICKS")
do
  -- no room for extra parts: a think already far over the target
  _G.brain = { targetMs = 12 }
  local env = new_env(6, 100)
  env.state._think_t0_us = -1e12
  local n0 = #lines + 1
  local res = run(env)
  if LOGS then print("    " .. tostring(find_line("^PT2_SPLIT ", n0))) end
  local wt = PA.last_run and PA.last_run.work
  local parts = PA.last_run and PA.last_run.parts
  check("8 parts (build_inp, target, 6 group pills)", parts == 8, parts)
  check("done in 5 work ticks", wt == 5, wt)
  check("plan tick + 5 ticks", #res == 6 and res[6] == "set", table.concat(res, ","))
  check("same answer as one go", deep_eq(env.goal._shield_scan, (one_go(6, 101))), nil)
  -- with kills: a killed tick is not a work tick
  local env2 = new_env(6, 100)
  env2.state._think_t0_us = -1e12
  local undo = kill_at(V2, "evaluate", 3)
  local res2 = run(env2)
  undo()
  check("one kill: still 5 work ticks", PA.last_run.work == 5 and PA.last_run.kills == 1, PA.last_run.work)
  check("one kill: 1 more tick", #res2 == 7 and res2[7] == "set", table.concat(res2, ","))
  C.PT2_AFTER_SCAN_MAX_TICKS = 2
  local env3 = new_env(6, 100)
  env3.state._think_t0_us = -1e12
  run(env3)
  check("max 2: done in 2 work ticks", PA.last_run.work == 2, PA.last_run.work)
  C.PT2_AFTER_SCAN_MAX_TICKS = 5
  _G.brain = { targetMs = 0 }
  local env4 = new_env(6, 100)
  run(env4)
  check("room for all: 1 work tick", PA.last_run.work == 1, PA.last_run.work)
end

-- ── stale runs ───────────────────────────────────────────────────────────
print("== stale: a group pill's health changes")
do
  _G.brain = { targetMs = 12 }
  local env = new_env(3, 100)
  env.state._think_t0_us = -1e12
  local n0 = #lines + 1
  check("plan tick", tick(env) == "wait", nil)
  check("part 0", tick(env) == "wait", nil)
  env.world.pills[3].health = 10
  local res = run(env)
  if LOGS then print("    " .. tostring(find_line("^PT2_SPLIT_DROP", n0))) end
  check("dropped, why=pill", PA.last_drop == "pill", PA.last_drop)
  check("v1 ran again", env.v1_runs == 2, env.v1_runs)
  check("plan set", res[#res] == "set" and env.goal._shield_scan ~= nil, table.concat(res, ","))
  _G.brain = { targetMs = 0 }
end
print("== stale: key, target owner, age")
do
  local env = new_env(3, 100)
  tick(env)
  check("other key: dropped", PA.split_get(env.goal, env.pill, "k2", env.now) == nil, nil)
  local env2 = new_env(3, 100)
  tick(env2)
  env2.pill.owner = "ours"
  check("owner change: dropped", PA.split_get(env2.goal, env2.pill, KEY, env2.now) == nil, nil)
  local env3 = new_env(3, 100)
  tick(env3)
  check("too old: dropped", PA.split_get(env3.goal, env3.pill, KEY, 100 + (C.SHIELD_SCAN_CACHE_TICKS or 25) + 1) == nil, nil)
  local env4 = new_env(3, 100)
  tick(env4)
  env4.goal.target_id = 9
  check("target switch: dropped", PA.split_get(env4.goal, env4.pill, KEY, env4.now) == nil, nil)
end
print("== drops in a row: give up to v1")
do
  local env = new_env(3, 100)
  local n0 = #lines + 1
  for _ = 1, C.SHIELD_SCAN_BLACKLIST_TRIES or 2 do
    tick(env)                                  -- the plan tick opens a run
    env.world.pills[2].health = env.world.pills[2].health - 1
    _G.brain = { targetMs = 12 }; env.state._think_t0_us = -1e12
    tick(env)                                  -- part 0 snapshots the pills
    env.world.pills[2].health = env.world.pills[2].health - 1
  end
  local res = run(env)
  _G.brain = { targetMs = 0 }
  if LOGS then print("    " .. tostring(find_line("^PT2_SPLIT_GIVEUP", n0))) end
  check("give up, why=drops", PA.last_run and PA.last_run.giveup == "drops", PA.last_run and PA.last_run.giveup)
  check("v1's plan set", res[#res] == "set" and env.goal._shield_scan == env.v1, table.concat(res, ","))
end

-- ── one heavy part per work tick ─────────────────────────────────────────
print("== PT2_SPLIT_ONE_PART_PER_TICK")
do
  C.PT2_SPLIT_ONE_PART_PER_TICK = true
  -- the ticks each v2 evaluate (a heavy part) ran on
  local function per_tick(env)
    local real, at = V2.evaluate, {}
    V2.evaluate = function(...)
      at[env.now] = (at[env.now] or 0) + 1
      return real(...)
    end
    local res = run(env)
    V2.evaluate = real
    local most = 0
    for _, n in pairs(at) do if n > most then most = n end end
    return res, at, most
  end
  for _, tgt in ipairs({ 0, 12 }) do
    _G.brain = { targetMs = tgt }
    for _, ng in ipairs({ 0, 3, 6 }) do
      local env = new_env(ng, 100)
      local res, at, most = per_tick(env)
      local name = string.format("target %d, group %d: ", tgt, ng)
      local parts = PA.last_run and PA.last_run.parts
      check(name .. "parts", parts == ng + 2, parts)
      check(name .. "one evaluate a tick at most", most == 1, most)
      check(name .. "part 0 and part 1 on the first work tick", at[101] == 1, at[101])
      check(name .. "work ticks = parts - 1", PA.last_run.work == ng + 1, PA.last_run.work)
      check(name .. "plan tick + work ticks", #res == ng + 2 and res[#res] == "set", table.concat(res, ","))
      check(name .. "same answer as one go", deep_eq(env.goal._shield_scan, (one_go(ng, 101))), nil)
    end
  end
  -- a kill: the killed part runs again on the next tick, still one a tick
  _G.brain = { targetMs = 0 }
  local env = new_env(3, 100)
  local undo = kill_at(V2, "evaluate", 2)
  local res = run(env)
  undo()
  check("one kill: 4 work ticks, 1 kill", PA.last_run.work == 4 and PA.last_run.kills == 1, PA.last_run.work)
  check("one kill: 1 more tick", #res == 6 and res[6] == "set", table.concat(res, ","))
  check("one kill: same answer as one go", deep_eq(env.goal._shield_scan, REF3), nil)
  C.PT2_SPLIT_ONE_PART_PER_TICK = false
end

-- ── PT2_V2_SPLIT: one v2 evaluate over up to PT2_V2_SPLIT_TICKS ticks ───
print("== PT2_V2_SPLIT")
do
  local NV = require("pilltake_v2")
  C.PT2_EVALUATOR = "v2"
  C.PT2_SPLIT_ONE_PART_PER_TICK = true
  _G.brain = { targetMs = 0 }
  -- the slicer with a small budget, so each evaluate takes several slices
  local real_slicer = NV.slicer
  local slices, kill_unit = 0, nil
  NV.slicer = function(inp, ticks, yield)
    local s = real_slicer(inp, ticks, yield)
    s.budget = 3   -- work units (P1b weights): several slices per evaluate
    local units, fn = 0, s.fn
    s.fn = function(...)
      units = units + 1
      if kill_unit and units == kill_unit then kill_unit = nil; error("tick_budget_exceeded (slice)", 0) end
      local before = s.slices
      local wu, _, fin = ...
      local r = fn(wu, nil, fin)   -- the fixed budget, not the estimate
      if s.slices ~= before then slices = slices + 1 end
      return r
    end
    return s
  end
  for _, ng in ipairs({ 0, 3 }) do
    C.PT2_V2_SPLIT = false
    local env0 = new_env(ng, 100)
    local res0 = run(env0)
    local work0 = PA.last_run and PA.last_run.work
    C.PT2_V2_SPLIT = true
    slices = 0
    local env = new_env(ng, 100)
    local res = run(env)
    local name = string.format("v2, group %d: ", ng)
    check(name .. "set", res[#res] == "set", table.concat(res, ","))
    check(name .. "v2 answer (pt2)", env.goal._shield_scan and env.goal._shield_scan.pt2 == true, nil)
    check(name .. "same answer as the evaluate in one tick", deep_eq(env.goal._shield_scan, env0.goal._shield_scan), nil)
    check(name .. "same goal._pt2", deep_eq(env.goal._pt2, env0.goal._pt2), nil)
    check(name .. "the evaluates were sliced", slices > 0, slices)
    check(name .. "more work ticks than one part a tick", PA.last_run.work > work0, PA.last_run.work)
    check(name .. "at most 5 ticks a part", PA.last_run.work <= 5 * (PA.last_run.parts - 1), PA.last_run.work)
    -- a budget kill in a later slice: the part runs again from its start
    kill_unit = 5
    local envk = new_env(ng, 100)
    local resk = run(envk)
    check(name .. "kill in a slice: set", resk[#resk] == "set", table.concat(resk, ","))
    check(name .. "kill in a slice: 1 kill", PA.last_run.kills == 1, PA.last_run.kills)
    check(name .. "kill in a slice: same answer", deep_eq(envk.goal._shield_scan, env0.goal._shield_scan), nil)
    kill_unit = nil
  end
  NV.slicer = real_slicer
  C.PT2_V2_SPLIT = true
  C.PT2_SPLIT_ONE_PART_PER_TICK = false
  C.PT2_EVALUATOR = "old"
end

-- ── knob off: split_get finds nothing, after_scan unchanged ──────────────
print("== knob off")
do
  C.PT2_AFTER_SCAN_SPLIT = false
  local out, goal = one_go(3, 101)
  check("after_scan in one go still gives v2", out and out.pt2 == true, nil)
  check("no run on the goal", goal._pt2_split == nil, goal._pt2_split)
  C.PT2_AFTER_SCAN_SPLIT = true
end

C.PILLTAKE_V2_EVALUATOR = false
print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
