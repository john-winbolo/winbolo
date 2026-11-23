-- =========================================================================
-- test_pt2_v2_first.lua -- standalone unit tests (2026-11-23) for
-- C.PT2_V2_FIRST: on a protected take with v2 on, v2 runs before v1's
-- shield.scan, and the scan runs only when v2 has no plan of its own.
--
--   1. pt2_attack.v2_first: on only with the knob, the master, a protected
--      take, and (in a debug run) no PT2_CMP log;
--   2. v2 safe: the same plan as the old order (v1 first), and shield.scan
--      is not called; the standoff in the plan is the goal's own;
--   3. v2 with no safe point: v1's plan, the same as the old order;
--   4. an error inside v2: v1's plan, the same as the old order;
--   5. v2 given up (split run, a part killed SHIELD_SCAN_BLACKLIST_TRIES
--      times; one go, the same kills): v1's plan, the same as the old order;
--   6. a budget kill in the fallback scan: the plan flag stays, the next
--      tick runs the scan again and not v2;
--   7. v1's cache fresh on the plan tick: the old order (no saving lost);
--   8. attack.lua's deferred shield scan has this order.
-- Each case runs with PT2_AFTER_SCAN_SPLIT on (v2 over ticks) and off (v2
-- in one go, SHIELD_SCAN_PENDING_SURVIVES_V2 on).
--
-- The tick driver does what attack.lua's deferred shield scan does, in the
-- same order: split_get, v1 (now or later), keeps_pending, split_step or
-- one_go, the fallback scan, goal._shield_scan set, the flag cleared.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_v2_first.lua
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

local IN_OPT = io.open("strip.sh", "r") == nil

-- v1's scan from the goal's standoff, as attack.lua calls it. Counted.
local scan_calls = 0
local real_scan = S.scan
S.scan = function(...) scan_calls = scan_calls + 1; return real_scan(...) end
local R = C.PPT_STANDOFF or 7.0
local function v1_scan(env)
  local g = env.goal
  if (env.kill_v1 or 0) > 0 then env.kill_v1 = env.kill_v1 - 1; error("tick_budget_exceeded (v1)", 0) end
  return S.scan(env.pill, env.world, g.standoff_mx, g.standoff_my, 180, g.standoff_fx, g.standoff_fy,
                R, false, 40, nil, nil, 0)
end

-- One brain tick of attack.lua's deferred shield scan. order: "old" (v1
-- first, PT2_V2_FIRST off) or "new". Returns "idle", "wait", "set" or
-- "killed".
local function tick(env, order)
  local goal = env.goal
  if not goal._shield_scan_pending then return "idle" end
  C.PT2_V2_FIRST = (order == "new")
  local ok, r = pcall(function()
    local now, key, st = env.now, KEY, env.state
    local split_pa = C.PT2_AFTER_SCAN_SPLIT and PA or nil
    local sp = split_pa and PA.split_get(goal, env.pill, key, now) or nil
    local sc = st._shield_scan_cache
    local sc_hit = sc and sc.key == key and (now - sc.tick) < (C.SHIELD_SCAN_CACHE_TICKS or 25)
    local function v1()
      if sc_hit then return sc.result end
      local res = v1_scan(env)
      st._shield_scan_cache = { key = key, result = res, tick = now }
      return res
    end
    local v1_only = goal._pt2_v1_need ~= nil and goal._pt2_v1_need == key
    local v1_later = false
    local sscan
    if v1_only then sscan = v1()
    elseif sp then sscan = sp.sscan; v1_later = sp.v1_later or false
    elseif not sc_hit and PA.v2_first(goal) then v1_later = true
    else sscan = v1() end
    local keep = PA.keeps_pending(split_pa) or v1_later
    if not keep then goal._shield_scan_pending = nil end
    if not v1_only then
      local a = args(goal, env.world, env.pill, sscan, now, st)
      a.v1_later = v1_later
      if split_pa then
        local out, ready = PA.split_step(sp, a, key)
        if not ready then return "wait" end
        sscan = out
      else
        sscan = PA.one_go(a, key)
      end
      if v1_later and PA.v1_needed(sscan) then
        if split_pa then PA.split_done(goal) end
        goal._pt2_v1_need = key
        sscan = v1()
      end
    end
    if sscan and (not sscan.best or (sscan.best.score or 0) <= 0) then sscan = nil end
    goal._shield_scan = sscan
    goal._pt2_v1_need = nil
    if keep then goal._shield_scan_pending = nil end
    if split_pa then PA.split_done(goal) end
    return "set"
  end)
  env.now = env.now + 1
  if ok then return r end
  if tostring(r):find("tick_budget_exceeded", 1, true) then return "killed" end
  error(r, 0)
end

local function run(env, order)
  local res = {}
  for _ = 1, 40 do
    local r = tick(env, order)
    res[#res + 1] = r
    if r == "set" or r == "idle" then break end
  end
  return res
end

local function new_env(n_group)
  local world, pill = make_world(n_group)
  local env = { world = world, pill = pill, now = 100, state = {} }
  local cx, cy = PMX + 0.5, PMY + 0.5 + R
  env.goal = { scan_spots = spots(), _is_ppt = true, target_id = 1, mx = PMX, my = PMY,
               standoff_mx = math.floor(cx), standoff_my = math.floor(cy),
               standoff_fx = cx, standoff_fy = cy, _chosen_deg = 180,
               _shield_scan_pending = true }
  return env
end

-- Runs the old order and the new order on the same take. setup(env): stubs
-- for this case, returns an undo function; called once per run.
local function both(n_group, setup)
  local out = {}
  for _, order in ipairs({ "old", "new" }) do
    local env = new_env(n_group)
    local undo = setup and setup(env) or nil
    scan_calls = 0
    local res = run(env, order)
    if undo then undo() end
    out[order] = { env = env, res = res, scans = scan_calls, plan = env.goal._shield_scan }
  end
  return out.old, out.new
end

-- The plan less its standoff, and the standoff's four numbers (the C scan
-- gives only these four; the Lua scan in this test adds its ring fields).
local function no_standoff(p)
  if not p then return p end
  local c = {}
  for k, v in pairs(p) do if k ~= "standoff" then c[k] = v end end
  return c
end
local function so4(p)
  local s = p and p.standoff
  return s and string.format("%.4f,%.4f,%s,%s", s.cx, s.cy, tostring(s.mx), tostring(s.my)) or "-"
end

local function kill_at(tbl, name, n, times)
  local real, calls, left = tbl[name], 0, times or 1
  tbl[name] = function(...)
    calls = calls + 1
    if calls >= n and left > 0 then left = left - 1; error("tick_budget_exceeded (test)", 0) end
    return real(...)
  end
  return function() tbl[name] = real end
end

local function marks_gone(goal)
  return goal._shield_scan_pending == nil and goal._pt2_split == nil and goal._pt2_v1_need == nil
end

_G.brain = { targetMs = 0 }
C.PILLTAKE_V2_EVALUATOR = true
C.SHIELD_SCAN_PENDING_SURVIVES_V2 = true
C.PT2_AFTER_SCAN_MAX_TICKS = 5
-- no PT2_CMP log: the new order is on (the log keeps the old order)
_G.BRAIN_DEBUG_MODE = false; _G._PRINT2_ENABLED = false

-- ── 1. v2_first ──────────────────────────────────────────────────────────
print("== 1. pt2_attack.v2_first")
do
  local g = { _is_ppt = true }
  C.PT2_V2_FIRST = true
  check("knob, master, protected take: on", PA.v2_first(g) == true, PA.v2_first(g))
  check("not a protected take: off", PA.v2_first({ _is_ppt = false }) == false, nil)
  C.PILLTAKE_V2_EVALUATOR = false
  check("master off: off", PA.v2_first(g) == false, nil)
  C.PILLTAKE_V2_EVALUATOR = true
  C.PT2_V2_FIRST = false
  check("knob off: off", PA.v2_first(g) == false, nil)
  C.PT2_V2_FIRST = true
  if not IN_OPT then
    _G.BRAIN_DEBUG_MODE = true; _G._PRINT2_ENABLED = true
    local was = C.PT2_SIDE_BY_SIDE_LOG
    C.PT2_SIDE_BY_SIDE_LOG = true
    check("debug run with the PT2_CMP log: off (v1 first)", PA.v2_first(g) == false, nil)
    C.PT2_SIDE_BY_SIDE_LOG = false
    check("debug run, log off: on", PA.v2_first(g) == true, nil)
    C.PT2_SIDE_BY_SIDE_LOG = was
    _G.BRAIN_DEBUG_MODE = false; _G._PRINT2_ENABLED = false
  end
  check("v1_needed: not for v2's plan", PA.v1_needed({ pt2 = true }) == false, nil)
  check("v1_needed: for nil and for v1's plan", PA.v1_needed(nil) and PA.v1_needed({ best = {} }), nil)
  check("PRESETS.mast: PT2_V2_FIRST false", C.PRESETS.mast.PT2_V2_FIRST == false, C.PRESETS.mast.PT2_V2_FIRST)
  check("PRESETS.keel: PT2_V2_FIRST false", C.PRESETS.keel.PT2_V2_FIRST == false, C.PRESETS.keel.PT2_V2_FIRST)
end

local function unsafe()
  local real = V2.evaluate
  V2.evaluate = function(inp)
    local r = real(inp)
    if r.best then r.best.safe = false end
    return r
  end
  return function() V2.evaluate = real end
end
local function fault()
  local real = V2.evaluate
  V2.evaluate = function() error("test fault") end
  return function() V2.evaluate = real end
end
local function ticks(r) return table.concat(r.res, ",") end

for _, split in ipairs({ true, false }) do
  C.PT2_AFTER_SCAN_SPLIT = split
  local tag = split and "split" or "one go"

  print(string.format("== 2. v2 safe (%s)", tag))
  for _, ng in ipairs({ 0, 3 }) do
    local o, n = both(ng)
    local function nm(s) return string.format("group %d: %s", ng, s) end
    check(nm("old plan is v2's"), o.plan and o.plan.pt2 == true, o.plan and o.plan.pt2)
    check(nm("same plan (all but the standoff)"), deep_eq(no_standoff(o.plan), no_standoff(n.plan)), n.plan)
    check(nm("same standoff numbers"), so4(o.plan) == so4(n.plan), so4(o.plan) .. " vs " .. so4(n.plan))
    check(nm("same goal._pt2"), deep_eq(o.env.goal._pt2, n.env.goal._pt2), n.env.goal._pt2)
    check(nm("same ticks"), ticks(o) == ticks(n), ticks(o) .. " vs " .. ticks(n))
    check(nm("old order ran shield.scan once"), o.scans == 1, o.scans)
    check(nm("new order did not run shield.scan"), n.scans == 0, n.scans)
    check(nm("no v1 cache written"), n.env.state._shield_scan_cache == nil, nil)
    check(nm("flag cleared, marks gone"), marks_gone(n.env.goal), nil)
  end

  for _, case in ipairs({ { "3. v2 with no safe point", unsafe }, { "4. an error inside v2", fault } }) do
    print(string.format("== %s (%s)", case[1], tag))
    for _, ng in ipairs({ 0, 3 }) do
      local o, n = both(ng, case[2])
      local function nm(s) return string.format("group %d: %s", ng, s) end
      check(nm("old plan is v1's"), o.plan and not o.plan.pt2 and o.plan.best ~= nil, o.plan)
      check(nm("same plan as old"), deep_eq(o.plan, n.plan), n.plan)
      check(nm("goal._pt2 nil in both"), o.env.goal._pt2 == nil and n.env.goal._pt2 == nil, n.env.goal._pt2)
      check(nm("same ticks"), ticks(o) == ticks(n), ticks(o) .. " vs " .. ticks(n))
      check(nm("one scan in each"), o.scans == 1 and n.scans == 1, o.scans .. "/" .. n.scans)
      check(nm("flag cleared, marks gone"), marks_gone(n.env.goal), nil)
    end
  end

  print(string.format("== 5. v2 given up after kills (%s)", tag))
  do
    local tries = C.SHIELD_SCAN_BLACKLIST_TRIES or 2
    -- split: part 1 (v2 on the target) killed TRIES times; one go: the run
    -- killed TRIES times (one_go's own count)
    local o, n = both(3, function() return kill_at(V2, "evaluate", 1, tries) end)
    check("old plan is v1's", o.plan and not o.plan.pt2 and o.plan.best ~= nil, o.plan)
    check("same plan as old", deep_eq(o.plan, n.plan), n.plan)
    check("new order: one scan, after the give-up", n.scans == 1, n.scans)
    check("ends with a plan set", n.res[#n.res] == "set", ticks(n))
    check("flag cleared, marks gone", marks_gone(n.env.goal), nil)
  end

  print(string.format("== 6. a budget kill in the fallback scan (%s)", tag))
  do
    local env = new_env(0)
    local undo = unsafe()
    local evals = 0
    local real = V2.evaluate
    V2.evaluate = function(...) evals = evals + 1; return real(...) end
    env.kill_v1 = 1
    scan_calls = 0
    local res = run(env, "new")
    V2.evaluate = real
    undo()
    local s = table.concat(res, ",")
    check("killed once, then set", res[#res - 1] == "killed" and res[#res] == "set", s)
    check("v2 ran once (not again after the kill)", evals == 1, evals)
    check("plan is v1's", env.goal._shield_scan and not env.goal._shield_scan.pt2, env.goal._shield_scan)
    check("flag cleared, marks gone", marks_gone(env.goal), nil)
  end

  print(string.format("== 7. v1's cache fresh (%s)", tag))
  do
    local env = new_env(0)
    local cached = v1_scan(env)
    env.state._shield_scan_cache = { key = KEY, result = cached, tick = env.now }
    scan_calls = 0
    run(env, "new")
    local o = both(0)
    local p = env.goal._shield_scan
    check("v2's plan", p and p.pt2 == true, p)
    check("standoff from v1's cached answer (old order)", p and p.standoff == cached.standoff, nil)
    check("no scan run", scan_calls == 0, scan_calls)
    check("same plan as old (all but the standoff)", deep_eq(no_standoff(o.plan), no_standoff(p)), nil)
  end
end

-- ── 8. attack.lua ────────────────────────────────────────────────────────
print("== 8. attack.lua's deferred shield scan")
do
  local fh = io.open("attack.lua", "r")
  local src = fh and fh:read("*a") or ""
  if fh then fh:close() end
  src = src:gsub("\r\n", "\n")
  local function at(pat) return src:find(pat) end
  local p_fn    = at("local function _v1_scan%(%)")
  local p_v1o   = at("local _v1_only = goal%._pt2_v1_need ~= nil and goal%._pt2_v1_need == _scan_key")
  local p_first = at("elseif not _sc_hit and _pa%.v2_first%(goal%) then\n%s*_v1_later = true")
  local p_keep  = at("keeps_pending%(_split_pa%) or _v1_later")
  local p_v2    = at("and not _v1_only then\n%s*local _a = {")
  local p_a     = at("v1_later = _v1_later }")
  local p_fb    = at("if _v1_later and _pa%.v1_needed%(sscan%) then\n%s*if _split_pa then _split_pa%.split_done%(goal%) end\n%s*goal%._pt2_v1_need = _scan_key\n%s*sscan = _v1_scan%(%)")
  local p_set   = at("goal%._shield_scan = sscan\n%s*goal%._pt2_v1_need = nil")
  local p_new   = at("goal%._pt2_split = nil\n%s*goal%._pt2_v1_need = nil")
  check("the scan is a local function", p_fn ~= nil, nil)
  check("v1 only after v2 said no", p_v1o ~= nil, nil)
  check("v2 first when v1's cache is cold", p_first ~= nil, nil)
  check("the flag stays while v1 is to run", p_keep ~= nil, nil)
  check("v2 skipped on the v1-only tick", p_v2 ~= nil, nil)
  check("the split run keeps v1_later", p_a ~= nil, nil)
  check("the fallback scan after v2", p_fb ~= nil, nil)
  check("the mark cleared with the plan", p_set ~= nil, nil)
  check("the mark cleared on a new plan", p_new ~= nil, nil)
  check("in that order", (p_fn and p_v1o and p_first and p_keep and p_v2 and p_fb and p_set
    and p_fn < p_v1o and p_v1o < p_first and p_first < p_keep and p_keep < p_v2 and p_v2 < p_fb
    and p_fb < p_set) and true or false, nil)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
