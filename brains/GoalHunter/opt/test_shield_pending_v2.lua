-- =========================================================================
-- test_shield_pending_v2.lua -- standalone unit tests (2026-11-22) for
-- C.SHIELD_SCAN_PENDING_SURVIVES_V2: with PT2_AFTER_SCAN_SPLIT off, pill
-- take v2 runs in one go on the plan tick (pt2_attack.one_go). A budget
-- kill inside v2 must not lose the plan.
--
-- The bug (20261121_002624_2__2v2_combined, bot1 t=9247): attack.lua
-- cleared goal._shield_scan_pending before v2 and set goal._shield_scan
-- only after it. The tick was killed inside v2's evaluate (14.33 ms of a
-- 12 ms budget), so neither stayed: the next tick drove in with no walls,
-- though v2 had found a safe double take.
--
--   1. no kill: the plan is set on the plan tick (v2's answer);
--   2. one kill inside v2's evaluate: the flag stays, the next tick takes
--      v1's answer from v1's cache and runs v2 again; the plan is v2's,
--      the same as one go;
--   3. SHIELD_SCAN_BLACKLIST_TRIES kills in a row: v1's plan stands, no loop;
--   4. every kill count ends with a plan set, never neither;
--   5. the kill marker starts again on a new scan key;
--   6. attack.lua's deferred shield scan uses pt2_attack.keeps_pending and
--      pt2_attack.one_go (the tick driver below is that code).
--
-- The tick driver does what attack.lua's deferred shield scan does with
-- the split off, in the same order: v1 (from v1's cache on the same key),
-- the flag cleared unless keeps_pending, one_go, goal._shield_scan set,
-- then the flag cleared.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_shield_pending_v2.lua
-- Arguments:
--   old        C.SHIELD_SCAN_PENDING_SURVIVES_V2 = false (the old code path;
--              2 and 4 must fail).
--   src=<dir>  run 6 on <dir>/attack.lua (default: this directory).
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local C   = require("constants")
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

local OLD, SRC = false, "."
for _, a in ipairs(arg or {}) do
  if a == "old" then OLD = true end
  local d = a:match("^src=(.+)$")
  if d then SRC = d end
end
if OLD then C.SHIELD_SCAN_PENDING_SURVIVES_V2 = false end
print(string.format("C.SHIELD_SCAN_PENDING_SURVIVES_V2 = %s", tostring(C.SHIELD_SCAN_PENDING_SURVIVES_V2)))

C.PT2_AFTER_SCAN_SPLIT = false
C.PILLTAKE_V2_EVALUATOR = true
_G.BRAIN_DEBUG_MODE = true; _G._PRINT2_ENABLED = true
_G.brain = { targetMs = 0 }

-- One brain tick of attack.lua's deferred shield scan, split off. Returns
-- "idle", "set", or "killed" (a budget kill unwound the think).
local function tick(env)
  local goal = env.goal
  if not goal._shield_scan_pending then return "idle" end
  local ok, r = pcall(function()
    local st = env.state
    local sscan
    local sc = st._shield_scan_cache
    if sc and sc.key == env.key and env.now - sc.tick < (C.SHIELD_SCAN_CACHE_TICKS or 25) then
      sscan = sc.result
    else
      sscan = env.v1
      env.v1_runs = (env.v1_runs or 0) + 1
      st._shield_scan_cache = { key = env.key, result = sscan, tick = env.now }
    end
    local keep = PA.keeps_pending(nil)
    if not keep then goal._shield_scan_pending = nil end
    local out = PA.one_go(args(goal, env.world, env.pill, sscan, env.now, st), env.key)
    goal._shield_scan = out
    if keep then goal._shield_scan_pending = nil end
    return "set"
  end)
  env.now = env.now + 1
  if ok then return r end
  if tostring(r):find("tick_budget_exceeded", 1, true) then return "killed" end
  error(r, 0)
end

local function run(env)
  local res = {}
  for _ = 1, 10 do
    local r = tick(env)
    res[#res + 1] = r
    if r == "set" or r == "idle" then break end
  end
  return res
end

local function new_env(n_group, now, state)
  local world, pill = make_world(n_group)
  local env = { world = world, pill = pill, now = now or 100, state = state or {}, key = KEY }
  env.goal = { scan_spots = spots(), _is_ppt = true, target_id = 1, mx = PMX, my = PMY,
               _shield_scan_pending = true }
  env.v1 = v1_scan(world, pill)
  return env
end

-- The one-go answer (no kill), on tick now.
local function ref_answer(n_group, now)
  local world, pill = make_world(n_group)
  local goal = { scan_spots = spots(), _is_ppt = true, target_id = 1, mx = PMX, my = PMY }
  return PA.after_scan(args(goal, world, pill, v1_scan(world, pill), now)), goal
end

-- A budget kill at the n-th call of fn and the times - 1 calls after it.
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

local tries = C.SHIELD_SCAN_BLACKLIST_TRIES or 2

-- The double take of the session: the target and one neighbour (a chain
-- of two); and a bigger group.
for _, ng in ipairs({ 1, 3 }) do
  print(string.format("== group %d: 1. no kill", ng))
  do
    local env = new_env(ng, 100)
    local res = run(env)
    local ref = ref_answer(ng, 100)
    check("set on the plan tick", #res == 1 and res[1] == "set", table.concat(res, ","))
    check("v2 answer (pt2)", env.goal._shield_scan and env.goal._shield_scan.pt2 == true, nil)
    check("same answer as after_scan", deep_eq(env.goal._shield_scan, ref), env.goal._shield_scan)
    check("no kill marker left", env.state._pt2_v2_attempt == nil, env.state._pt2_v2_attempt)
  end

  print(string.format("== group %d: 2. one kill inside v2's evaluate", ng))
  do
    local env = new_env(ng, 100)
    local undo = kill_at(V2, "evaluate_group", 1, 1)
    local res = run(env)
    undo()
    local goal = env.goal
    local ref = ref_answer(ng, 101)
    check("first tick killed", res[1] == "killed", res[1])
    check("plan still pending after the kill", #res >= 2 and res[2] ~= "idle", table.concat(res, ","))
    check("ends with a plan set", res[#res] == "set" and goal._shield_scan ~= nil, table.concat(res, ","))
    check("the plan is v2's (pt2)", goal._shield_scan ~= nil and goal._shield_scan.pt2 == true,
          goal._shield_scan and tostring(goal._shield_scan.pt2))
    check("same answer as one go on the next tick", deep_eq(goal._shield_scan, ref), goal._shield_scan)
    check("v1 ran once (its cache on the next tick)", env.v1_runs == 1, env.v1_runs)
    check("pending cleared at the end", goal._shield_scan_pending == nil, goal._shield_scan_pending)
    check("no kill marker left", env.state._pt2_v2_attempt == nil, env.state._pt2_v2_attempt)
  end

  print(string.format("== group %d: 3. %d kills in a row", ng, tries))
  do
    local env = new_env(ng, 100)
    PA.last_run = nil
    local undo = kill_at(V2, "evaluate_group", 1, tries)
    local res = run(env)
    undo()
    local goal = env.goal
    check("killed ticks: " .. tries, count(res, "killed") == tries, table.concat(res, ","))
    check("then set, no loop", res[#res] == "set" and #res == tries + 1, table.concat(res, ","))
    check("v1's plan stands", goal._shield_scan ~= nil and goal._shield_scan == env.v1, goal._shield_scan)
    check("goal._pt2 cleared", goal._pt2 == nil, goal._pt2)
    check("gave up on kills", PA.last_run ~= nil and PA.last_run.giveup == "kills",
          PA.last_run and PA.last_run.giveup)
  end
end

print("== 4. every kill count ends with a plan set")
do
  local bad = {}
  for k = 0, 4 do
    local env = new_env(1, 100)
    local undo = kill_at(V2, "evaluate_group", 1, k)
    local res = run(env)
    undo()
    if not (res[#res] == "set" and env.goal._shield_scan ~= nil) then
      bad[#bad + 1] = string.format("%d kills: %s", k, table.concat(res, ","))
    end
  end
  check("0..4 kills: a plan every time", #bad == 0, table.concat(bad, "; "))
end

print("== 5. a new scan key starts the count again")
do
  local st = {}
  local env = new_env(1, 100, st)
  local undo = kill_at(V2, "evaluate_group", 1, 1)
  tick(env)
  undo()
  local att = st._pt2_v2_attempt
  check("marker after the kill", att ~= nil and att.count == 1, att and att.count)
  -- the situation changed (say our armour): a new key, and the old key's
  -- marker at its limit
  if att then att.count = tries end
  env.key = "k2"
  local res = run(env)
  check("new key: v2 runs (pt2 answer)", res[#res] == "set" and env.goal._shield_scan ~= nil
        and env.goal._shield_scan.pt2 == true, table.concat(res, ","))
end

print(string.format("== 6. attack.lua's deferred shield scan (%s/attack.lua)", SRC))
do
  local fh = io.open(SRC .. "/attack.lua", "r")
  local src = fh and fh:read("*a") or ""
  if fh then fh:close() end
  check("calls pt2_attack.keeps_pending", src:find('require%("pt2_attack"%)%.keeps_pending%(_split_pa%)') ~= nil, nil)
  check("calls pt2_attack.one_go", src:find('require%("pt2_attack"%)%.one_go%(_a, _scan_key%)') ~= nil, nil)
  check("no flag clear before v2 on the one-go path",
        src:find("if not _split_pa then goal%._shield_scan_pending = nil end") == nil, nil)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
