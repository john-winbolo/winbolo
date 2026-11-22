-- =========================================================================
-- test_approach_skip_shield.lua -- standalone unit tests (2026-11-22) for
-- C.APPROACH_SKIP_RUNUP_SHIELD_CHECK (approach_skip.shield_check): past the
-- standoff, the run-up skip fires from where the tank is only when the
-- take's own cover test (pilltake_v2.judge with the take's plan) passes
-- there too. The shot test (fast_setup.final_angle) is stubbed clear, so
-- the old code fires from every place below.
--   1. knobs: on by default, off in KEEL and MAST.
--   2. a 3-blocker take (three planned walls, single take): the tank one
--      square past the standoff, 48 wu to the uncovered side: drives back.
--   3. a 2-blocker take (two planned walls, a two-pill chain): the same
--      place: drives back.
--   4. covered: the 3-blocker take, the tank half a square past on the lane
--      centre: still fires from here.
--   5. grass demo try3 (Nov 20, session 20261120_011841_1_f_try3_on,
--      t=355..366): the double take with our pill down at (124,118), the
--      tank at (30975,31095): still fires from here.
--   6. no shield plan: the old way (fires).
--   7. the cost of one check (against the 12 ms tick budget).
-- No engine: a hand-made grass map, the shadow.lua rays.
--
-- Run from this directory with the LuaJIT built beside the game:
--   <luajit> test_approach_skip_shield.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_approach_skip_shield.lua
-- Against older sources (to show the test fails there):
--   <luajit> test_approach_skip_shield.lua src=<dir with the old .lua files>
-- =========================================================================

local SRC
for _, a in ipairs(arg or {}) do
  local v = a:match("^src=(.+)$")
  if v then SRC = v end
end
package.path = (SRC and (SRC .. "/?.lua;") or "") .. "./?.lua;" .. package.path

if math.atan2 then
  local _a1, _a2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _a1(y) else return _a2(y, x) end end
end
package.loaded["print2"] = setmetatable({ set_tick = function() end }, { __call = function() end })
_G.print2 = _G.print2 or function() end

local C   = require("constants")
local U   = require("util")
local cpf = require("cpathfinder")
local PC  = require("pill_cover")
local V2  = require("pilltake_v2")
local FS  = require("fast_setup")
local AS  = require("approach_skip")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

-- ── Map and stubs ────────────────────────────────────────────────────────
local function sq(mx, my) return my * 256 + mx end
U.ttype      = function() return C.T_GRASS end
U.ttype_peek = U.ttype
U.base_tile_stops_tank_shell = function() return false end
cpf.lgm_travel_ticks_map = function() return 10 end
C.PT2_C_EVAL = false
-- the shot test: clear (the old code fires from every place below)
FS.world_env = function() return {} end
FS.final_angle = function() return 0 end

local function world_of(pills)
  local w = { pills = {}, pill_at = {}, bases = {}, base_at = {} }
  for i, p in ipairs(pills) do
    w.pills[i] = p
    w.pill_at[sq(p.mx, p.my)] = { { id = i, pill = p } }
  end
  return w
end

-- The take's plan, as attack.lua stores it: v2's best point for A (or for
-- A's group) from HOME, through to_scan_result.
local function plan_take(pills, home, group)
  local w = world_of(pills)
  local inp = V2.world_inp(w, C, U, PC, cpf, V2.knob_inp(C, {}), false)
  inp.A = pills[1]; inp.radius = 5.0; inp.num_pill_blockers = 0; inp.armour = 40
  inp.score_fn = function(deg, mx, my, cx, cy)
    local dx, dy = cx - home[1], cy - home[2]
    return math.sqrt(dx * dx + dy * dy)
  end
  local res = group and V2.evaluate_group(inp) or V2.evaluate(inp)
  local A = res.A or pills[1]
  local b = res.best
  local sscan = V2.to_scan_result(res, A, { cx = b.cx, cy = b.cy, mx = math.floor(b.cx), my = math.floor(b.cy) })
  sscan.num_pill_blockers = 0
  local id
  for i, p in ipairs(pills) do if p == A then id = i end end
  return w, sscan, b, A, id
end

-- A goal on the approach step with that plan; the tank stopped at
-- (standoff - past along the lane toward the pill + side across it).
local function goal_at(sscan, b, A, id, past, side)
  local pcx, pcy = A.mx * 256 + 128, A.my * 256 + 128
  local sx, sy = b.cx * 256, b.cy * 256
  local dx, dy = sx - pcx, sy - pcy
  local d = math.sqrt(dx * dx + dy * dy)
  local ux, uy = dx / d, dy / d
  local tx = math.floor(sx - ux * past + uy * side)
  local ty = math.floor(sy - uy * past - ux * side)
  local goal = { kind = "attack_pill", mx = A.mx, my = A.my, target_id = id, _is_ppt = true,
                 _shield_scan = sscan,
                 standoff_fx = b.cx, standoff_fy = b.cy,
                 standoff_mx = math.floor(b.cx), standoff_my = math.floor(b.cy),
                 approach_fx = b.cx + ux * 2.25, approach_fy = b.cy + uy * 2.25,
                 approach_mx = math.floor(b.cx + ux * 2.25), approach_my = math.floor(b.cy + uy * 2.25),
                 _approach_start = 100 }
  local info = { tankx = tx, tanky = ty, speed = 0, armour = 40, player_number = 0,
                 man_status = C.LGM_INTANK, carried_pills = 0, objects = {} }
  return goal, info
end

-- "1 square past": 240 wu (a little inside APPROACH_SKIP_RUNUP_MAX_PAST_WU
-- 256, so the floor to whole wu cannot put it out of the lane).
local PAST = 240

local function run(w, goal, info)
  local st = { tick = 110 }
  local fired = AS.check(goal, info, w, 110, nil, st)
  local why = AS.shield_check and select(1, AS.shield_check(st, goal, info, w, 110, info.tankx, info.tanky))
  print(string.format("    tank=(%d,%d) fired=%s shield=%s", info.tankx, info.tanky,
    tostring(fired and goal._runup_skip == "past"), tostring(why)))
  return fired and goal._runup_skip == "past", why, goal
end

-- ── 1. knobs ─────────────────────────────────────────────────────────────
print("1. knobs")
do
  local D = dofile((SRC and (SRC .. "/") or "") .. "constants.lua")
  check("APPROACH_SKIP_RUNUP_SHIELD_CHECK default true", D.APPROACH_SKIP_RUNUP_SHIELD_CHECK == true,
        D.APPROACH_SKIP_RUNUP_SHIELD_CHECK)
  check("keel false", D.PRESETS.keel.APPROACH_SKIP_RUNUP_SHIELD_CHECK == false)
  check("mast false", D.PRESETS.mast.APPROACH_SKIP_RUNUP_SHIELD_CHECK == false)
  check("APPROACH_SKIP_RUNUP_MAX_PAST_WU 256 (approved)", D.APPROACH_SKIP_RUNUP_MAX_PAST_WU == 256)
end
C.APPROACH_SKIP_RUNUP_ON_LANE = true
C.APPROACH_SKIP_RUNUP_SHIELD_CHECK = true

-- ── 2. 3-blocker take ────────────────────────────────────────────────────
-- A at (128,120), home south: v2 plans three walls (128,122..124) and a
-- standoff about 5 squares south, aim TL.
local A3 = { mx = 128, my = 120, owner = "hostile", health = 15, anger = 0 }
local w3, s3, b3, a3, id3 = plan_take({ A3 }, { 128.5, 134.5 })
print(string.format("2. 3-blocker take: so=(%.2f,%.2f) aim=%d plan=%d walls",
  b3.cx, b3.cy, b3.aim_idx, #b3.plan))
do
  check("the plan has three walls", #b3.plan == 3, #b3.plan)
  local g0 = goal_at(s3, b3, a3, id3, PAST, -48)
  local afx, afy = g0.approach_fx, g0.approach_fy
  local fired, why, g = run(w3, goal_at(s3, b3, a3, id3, PAST, -48))
  check("1 square past, 48 wu to the side: drives back (no fire from here)", not fired,
        g._runup_skip)
  check("the old run-up point and standoff stay", g.approach_fx == afx and g.approach_fy == afy
        and g.standoff_fx == b3.cx and not g._fs_off,
        string.format("%.2f,%.2f", g.approach_fx or -1, g.approach_fy or -1))
  check("why: A's ray is not covered (DEPTH)", tostring(why):sub(1, 5) == "DEPTH", why)
  -- knob off: the shot test alone, so it fires (the old way)
  C.APPROACH_SKIP_RUNUP_SHIELD_CHECK = false
  fired = run(w3, goal_at(s3, b3, a3, id3, PAST, -48))
  C.APPROACH_SKIP_RUNUP_SHIELD_CHECK = true
  check("knob off: fires from here (the old way)", fired, fired)
end

-- ── 3. 2-blocker take ────────────────────────────────────────────────────
-- A at (128,120) and B at (129,120), home east: v2 plans a two-pill chain
-- held by two walls (130,120) and (131,120) (the chain bar, 10 shots).
local A2 = { mx = 128, my = 120, owner = "hostile", health = 15, anger = 0 }
local B2 = { mx = 129, my = 120, owner = "hostile", health = 15, anger = 0 }
local w2, s2, b2, a2, id2 = plan_take({ A2, B2 }, { 140.5, 121.5 }, true)
print(string.format("3. 2-blocker take: so=(%.2f,%.2f) aim=%d kills=%d plan=%d walls",
  b2.cx, b2.cy, b2.aim_idx, b2.kills, #b2.plan))
do
  check("the plan: a chain with two walls", #b2.plan == 2 and b2.kills == 2,
        #b2.plan .. "/" .. b2.kills)
  local fired, why = run(w2, goal_at(s2, b2, a2, id2, PAST, -48))
  check("1 square past, 48 wu to the side: drives back (no fire from here)", not fired)
  check("why: a chain pill's ray is not covered (OVERLAP)", tostring(why):sub(1, 7) == "OVERLAP", why)
end

-- ── 4. covered ───────────────────────────────────────────────────────────
print("4. covered")
do
  local fired, why = run(w3, goal_at(s3, b3, a3, id3, 128, 0))
  check("3-blocker, half a square past on the lane: fires from here", fired)
  check("why: - (covered)", why == "-", why)
  fired, why = run(w2, goal_at(s2, b2, a2, id2, 224, 0))
  check("2-blocker, 224 wu past on the lane: fires from here", fired)
  check("why: - (covered)", why == "-", why)
end

-- ── 5. grass demo try3 ───────────────────────────────────────────────────
-- From the log: PT2_DRIVE t=355 A=(126,118) so=(120.56,122.21) aim=4
-- mode=HOLD chain 1-0, plan=- (our pill already down at (124,118));
-- APPROACH_SKIP_RUNUP t=366 tank=(30975,31095) along=-195 side=103.
print("5. grass demo try3")
do
  local pa = { mx = 125, my = 118, owner = "neutral", health = 15, anger = 0 }
  local pb = { mx = 126, my = 118, owner = "neutral", health = 15, anger = 0 }
  local ours = { mx = 124, my = 118, owner = "friendly", health = 15 }
  local w = world_of({ pa, pb, ours })
  local sscan = { best = { kind = "candidate", score = 1228, best_aim_idx = 4,
                           aims = { [4] = { blocked = false, blockers = {}, potential_blockers = {} } } },
                  pt2 = true, num_pill_blockers = 0 }
  local goal = { kind = "attack_pill", mx = 126, my = 118, target_id = 2, _is_ppt = true,
                 _shield_scan = sscan, standoff_fx = 120.56, standoff_fy = 122.21,
                 standoff_mx = 120, standoff_my = 122,
                 approach_fx = 118.7, approach_fy = 123.4, approach_mx = 118, approach_my = 123,
                 _approach_start = 300 }
  local info = { tankx = 30975, tanky = 31095, speed = 0, armour = 40, player_number = 0,
                 man_status = C.LGM_INTANK, carried_pills = 0, objects = {} }
  local fired, why = run(w, goal, info)
  check("try3: still fires from here", fired)
  check("try3: why: - (covered)", why == "-", why)
end

-- ── 6. no shield plan ────────────────────────────────────────────────────
print("6. no shield plan")
do
  local g, inf = goal_at(s3, b3, a3, id3, PAST, -48)
  g._shield_scan = nil
  local fired, why = run(w3, g, inf)
  check("no plan: fires from here (nothing to keep covered)", fired, fired)
  check("no plan: why -", why == "-", why)
end

-- ── 7. cost ──────────────────────────────────────────────────────────────
print("7. cost")
do
  if AS.shield_check then
    local N = 200
    for _, L in ipairs({ { "3-blocker", w3, s3, b3, a3, id3 }, { "2-blocker", w2, s2, b2, a2, id2 } }) do
      local g, inf = goal_at(L[3], L[4], L[5], L[6], PAST, -48)
      local t0 = os.clock()
      local mx = 0
      for _ = 1, N do
        local c0 = os.clock()
        AS.shield_check({}, g, inf, L[2], 110, inf.tankx, inf.tanky)
        local dt = os.clock() - c0
        if dt > mx then mx = dt end
      end
      local mean_ms = (os.clock() - t0) / N * 1000
      print(string.format("    %s: mean %.3f ms, max %.3f ms per check (%d calls)", L[1], mean_ms, mx * 1000, N))
      check(L[1] .. ": mean cost well under the 12 ms budget (< 1 ms)", mean_ms < 1.0, mean_ms)
    end
  else
    check("shield_check exists", false, "nil")
  end
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
