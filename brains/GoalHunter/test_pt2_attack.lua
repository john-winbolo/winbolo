-- =========================================================================
-- =========================================================================
-- test_pt2_attack.lua -- standalone unit tests (2026-11-16) for
-- pt2_attack.after_scan, the hook in attack.lua's deferred shield scan.
--
--   * master off, no log: v1's sscan comes back as the same table and goal
--     is not written (byte-identical behaviour).
--   * master off, debug log on: still the same sscan; one PT2_CMP line.
--   * master on, protected take: v2's plan in the shield.scan shape.
--   * master on, not a protected take: v1 stays.
--   * an error inside v2: v1 stays.
--   * the BrainTest layers: kept data, the shadow test, a shape per layer.
--   * HOLD chains: the next pill from the same spot, then the end.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_attack.lua
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
local PC  = require("pill_cover")
local SH  = require("shadow")


-- Tolerances (proposal, awaiting Andrew's value).
local TOL_WU, TOL_DEG = 64, 30

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

-- ── The map ──────────────────────────────────────────────────────────────
local terrain = {}
local function sq(mx, my) return my * 256 + mx end
U.ttype      = function(mx, my) return terrain[sq(mx, my)] or C.T_GRASS end
U.ttype_peek = U.ttype
U.base_tile_stops_tank_shell = function() return false end
cpf.SHOT_TANK = cpf.SHOT_TANK or 0
cpf.SHOT_PILL = cpf.SHOT_PILL or 1
cpf.lgm_travel_ticks_map = function(omx, omy, mx, my)
  local tt = U.ttype(mx, my)
  if U.is_water(tt) then return -1 end
  return 10
end

-- The engine shell walk as a square list (cpf_simulate_shot's shape: the
-- origin square, the spawn square, then every new square to the end of life).
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

_G.gh_shield = nil
package.loaded["attack_shield"] = nil
cpf.KIND_NORMAL = cpf.KIND_NORMAL or 0
local HOME = { 128.5, 134.5 }
cpf.dijkstra_lookup_by_kind = function(kind, mx, my)
  local dx, dy = mx + 0.5 - HOME[1], my + 0.5 - HOME[2]
  return math.sqrt(dx * dx + dy * dy)
end
cpf.estimate_cost = function() return 1e9 end

-- print2 stand-in that keeps the lines.
local lines = {}
package.loaded["print2"] = setmetatable({}, { __call = function(_, s) lines[#lines + 1] = s end })

_G.gh_shield = nil
package.loaded["attack_shield"] = nil
local S   = require("attack_shield")
local V2  = require("pilltake_v2_old")
local PA  = require("pt2_attack")

local PMX, PMY = 128, 120
local function make_world()
  local target = { mx = PMX, my = PMY, owner = "hostile", health = 15 }
  local pills = { [1] = target }
  return { pills = pills, pill_at = { [sq(PMX, PMY)] = { { id = 1, pill = target } } },
           bases = {}, base_at = {} }, target
end

-- v1's ring spots as plan_position leaves them: every 45 degrees, all clear.
local function spots()
  local out = {}
  for deg = 0, 315, 45 do
    out[#out + 1] = { deg = deg, has_los = true, _adj_score = 1 }
  end
  return out
end

local function call(goal, world, pill, sscan)
  return PA.after_scan({ state = {}, goal = goal, info = { tankx = 128 * 256, tanky = 134 * 256 },
                         world = world, pill = pill, sscan = sscan, no_builder = false,
                         npb = 0, radius = C.PPT_STANDOFF or 7.0, now = 100 })
end

local function v1_scan(world, pill)
  local R = C.PPT_STANDOFF or 7.0
  local cx, cy = PMX + 0.5, PMY + 0.5 + R
  return S.scan(pill, world, math.floor(cx), math.floor(cy), 180, cx, cy, R, false, 40, nil, nil, 0)
end

print("== master off, no log")
do
  C.PILLTAKE_V2_EVALUATOR = false; _G.BRAIN_DEBUG_MODE = false; _G._PRINT2_ENABLED = false
  local world, pill = make_world()
  local goal = { scan_spots = spots(), _is_ppt = true }
  local v1 = v1_scan(world, pill)
  local n0 = #lines
  local out = call(goal, world, pill, v1)
  check("same sscan table", out == v1, out)
  check("goal._pt2 not set", goal._pt2 == nil, goal._pt2)
  check("no log line", #lines == n0, #lines - n0)
end

print("== master off, debug log on")
do
  C.PILLTAKE_V2_EVALUATOR = false; _G.BRAIN_DEBUG_MODE = true; _G._PRINT2_ENABLED = true
  local world, pill = make_world()
  local goal = { scan_spots = spots(), _is_ppt = true }
  local v1 = v1_scan(world, pill)
  local n0 = #lines
  local out = call(goal, world, pill, v1)
  check("same sscan table", out == v1, out)
  check("goal._pt2 not set", goal._pt2 == nil, goal._pt2)
  check("one PT2_CMP line", #lines == n0 + 1 and lines[#lines]:find("^PT2_CMP") ~= nil, lines[#lines])
  print("    " .. tostring(lines[#lines]))
  check("drive=v1 in the line", lines[#lines]:find("drive=v1", 1, true) ~= nil, lines[#lines])
end

print("== master on, protected take")
do
  C.PILLTAKE_V2_EVALUATOR = true; _G.BRAIN_DEBUG_MODE = false; _G._PRINT2_ENABLED = false
  local world, pill = make_world()
  local goal = { scan_spots = spots(), _is_ppt = true }
  local v1 = v1_scan(world, pill)
  local out = call(goal, world, pill, v1)
  check("v2 result", out ~= v1 and out.pt2 == true, out and out.pt2)
  local b = out.best
  check("best has score > 0", b and (b.score or 0) > 0, b and b.score)
  local aim = b and b.aims and b.aims[b.best_aim_idx]
  check("best aim has potential_blockers", aim and aim.potential_blockers ~= nil, aim)
  check("best aim is not blocked", aim and aim.blocked == false, aim and aim.blocked)
  local cover = 0
  for _ in ipairs(aim and aim.potential_blockers or {}) do cover = cover + 5 end
  check("planned walls reach 15 shots", cover >= 15, cover)
  check("goal._pt2 set, single", goal._pt2 and goal._pt2.kills == 1 and goal._pt2.mode == "SINGLE", goal._pt2 and goal._pt2.mode)
  check("standoff kept", out.standoff ~= nil, out.standoff)
end

print("== master on, not a protected take")
do
  C.PILLTAKE_V2_EVALUATOR = true
  local world, pill = make_world()
  local goal = { scan_spots = spots(), _is_ppt = false }
  local v1 = v1_scan(world, pill)
  local out = call(goal, world, pill, v1)
  check("same sscan table", out == v1, out)
end

print("== master on, v2 error")
do
  C.PILLTAKE_V2_EVALUATOR = true
  local world, pill = make_world()
  local goal = { scan_spots = spots(), _is_ppt = true }
  local v1 = v1_scan(world, pill)
  local real = V2.evaluate
  V2.evaluate = function() error("test fault") end
  local out = call(goal, world, pill, v1)
  V2.evaluate = real
  check("same sscan table", out == v1, out)
end

print("== BrainTest layers (debug, every pt2 layer on)")
do
  local viz = require("viz")
  local PV  = require("pt2_viz")
  -- host stand-ins: layer index per id, toggles on, shapes counted per layer
  local idx, by_idx = {}, {}
  for i, id in ipairs(PV.LAYERS) do idx[id] = i; by_idx[i] = id; _G["_BT_VIZ_" .. id:upper()] = true end
  _G._BT_VIZ_IDS = idx
  local shapes = {}
  -- 255 = an id the host did not list: viz.lua's own shape labels (label_overlays)
  local function count(vi) local id = by_idx[vi] or (vi == 255 and "label") or "other"; shapes[id] = (shapes[id] or 0) + 1 end
  _G.overlay_line   = function(...) count(select(9, ...)) end
  _G.overlay_rect   = function(...) count(select(11, ...)) end
  _G.overlay_circle = function(...) count(select(8, ...)) end
  _G.overlay_text   = function(...) count(select(10, ...)) end
  local details = {}
  _G.overlay_detail = function(id) details[#details + 1] = id end
  _G.overlay_detail_text = function() end
  C.PILLTAKE_V2_EVALUATOR = false; _G.BRAIN_DEBUG_MODE = true; _G._PRINT2_ENABLED = true
  local world, pill = make_world()
  local goal = { scan_spots = spots(), _is_ppt = true, mx = PMX, my = PMY }
  local v1 = v1_scan(world, pill)
  local out = call(goal, world, pill, v1)
  check("viz on: still the same sscan", out == v1, out)
  check("goal._pt2 not set", goal._pt2 == nil, goal._pt2)
  local cap = goal._pt2_viz
  check("capture kept", cap ~= nil and cap.best ~= nil, cap)
  check("candidates kept (keep_all)", cap and cap.res.all and #cap.res.all > 100, cap and cap.res.all and #cap.res.all)
  check("shadow cells computed", cap and cap.cells and #cap.cells[1] > 0, cap and cap.cells and #cap.cells[1])
  -- the shadow test is the planner's: a cell next to the best point (which
  -- is covered by its plan) must be in shadow
  if cap and cap.cells then
    local b = cap.best
    local cw = PV.CELL_WU
    local k = math.floor(b.px / cw) * 65536 + math.floor(b.py / cw)
    local found = false
    for _, c in ipairs(cap.cells[1]) do if c == k then found = true end end
    check("best point's cell is in the target's shadow", found, k)
    -- and a cell straight south of the pill with no walls is lit
    local env = cap.inp.env
    local sx, sy = PMX * 256 + 128, PMY * 256 + 128
    local SH = require("shadow")
    local r = SH.cast(env, sx, sy, sx + 20, sy + 7 * 256, SH.PILL_FIRE_LEN, nil)
    check("no-wall ray reaches the tank (lit)", r.hit == "tank" and r.cover == 0, r.hit)
  end
  PV.draw(goal)
  -- the opt/ copy has its draw calls stripped (lua_strip "viz."): the
  -- shape checks only run where pt2_viz.lua still draws
  local stripped = false
  do
    local src = debug.getinfo(PV.draw, "S").source:gsub("^@", "")
    local f = io.open(src, "rb")
    if f then
      local t = f:read("*a")
      f:close()
      stripped = not t:find("viz.line(", 1, true)
    end
  end
  if stripped then
    print("  skip layer shape checks: the draw calls are stripped in this copy")
  else
    for _, id in ipairs(PV.LAYERS) do
      check("layer " .. id .. " drew shapes", (shapes[id] or 0) > 0, shapes[id])
    end
    check("candidate detail entries", #details > 100, #details)
  end
  check("no shape tagged with another layer", shapes.other == nil, shapes.other)
  -- another pill: nothing drawn
  shapes = {}
  goal.mx = PMX + 5
  PV.draw(goal)
  check("other target: nothing drawn", next(shapes) == nil, next(shapes))
  for _, id in ipairs(PV.LAYERS) do _G["_BT_VIZ_" .. id:upper()] = nil end
  _G.overlay_line, _G.overlay_rect, _G.overlay_circle, _G.overlay_text = nil, nil, nil, nil
  _G.overlay_detail, _G.overlay_detail_text, _G._BT_VIZ_IDS = nil, nil, nil
  _G.BRAIN_DEBUG_MODE = false
end

print("== HOLD chain: next pill from the same spot")
do
  local aims = {}
  package.loaded["attack"] = {
    _clear_aim_from_world = function(ox, oy, pmx, pmy) aims[#aims + 1] = { pmx, pmy }; return 3, pmx * 256 + 200, pmy * 256 + 50 end,
    _set_goal_aim = function(goal, pmx, pmy, idx, awx, awy) goal.aim_idx, goal.aim_wx, goal.aim_wy = idx, awx, awy end,
  }
  local A = { mx = 128, my = 120, health = 0, owner = "hostile" }
  local B = { mx = 130, my = 120, health = 15, owner = "hostile" }
  local world = { pills = { [4] = A, [7] = B } }
  local goal = { target_id = 4, mx = 128, my = 120, substate = "shoot_pill", _kill_attempt = true,
                 _pt2 = { mode = "HOLD", ids = { 4, 7 }, step = 1 }, _shield_scan = { pill = A } }
  check("has_next while A is the target", PA.has_next(goal, world), false)
  local ok = PA.chain_next({}, goal, world, { tankx = 129 * 256, tanky = 127 * 256 }, 500)
  check("retargeted", ok and goal.target_id == 7 and goal.mx == 130 and goal.my == 120, goal.target_id)
  check("aim from the same spot at B", #aims == 1 and aims[1][1] == 130 and goal.aim_idx == 3, goal.aim_idx)
  check("back to in_range_aim", goal.substate == "in_range_aim", goal.substate)
  check("new shot accounting", goal._kill_attempt == nil and goal._attack_start_tick == nil, goal._kill_attempt)
  check("step 2, scan pill B", goal._pt2.step == 2 and goal._shield_scan.pill == B, goal._pt2.step)
  check("no next after the last pill", not PA.has_next(goal, world), true)
  B.health = 0
  check("chain_next false at the end", PA.chain_next({}, goal, world, { tankx = 0, tanky = 0 }, 600) == false, true)
  local g2 = { target_id = 4, _pt2 = { mode = "SINGLE", ids = { 4 }, step = 1 } }
  check("SINGLE has no next", not PA.has_next(g2, world), true)
  package.loaded["attack"] = nil
end

print("== E5-12: a new plan during a take keeps the missing background walls")
do
  C.PILLTAKE_V2_EVALUATOR = true; _G.BRAIN_DEBUG_MODE = false; _G._PRINT2_ENABLED = false
  local function has(bg, mx, my)
    for _, w in ipairs(bg and bg.walls or {}) do if w.mx == mx and w.my == my then return true end end
    return false
  end
  local world, pill = make_world()
  local goal = { scan_spots = spots(), _is_ppt = true,
                 _pt2_bg = { walls = { { mx = 60, my = 60 } }, go = true } }
  call(goal, world, pill, v1_scan(world, pill))
  check("after the hold: the missing wall stays", has(goal._pt2_bg, 60, 60), goal._pt2_bg)
  check("the new bg waits for its own hold", goal._pt2_bg and not goal._pt2_bg.go, goal._pt2_bg and goal._pt2_bg.go)
  check("counted once", goal._pt2_finish_in_take == 1, goal._pt2_finish_in_take)
  world, pill = make_world()
  goal = { scan_spots = spots(), _is_ppt = true,
           _pt2_bg = { walls = { { mx = 60, my = 60 } } } }
  call(goal, world, pill, v1_scan(world, pill))
  check("before the hold: dropped", not has(goal._pt2_bg, 60, 60), goal._pt2_bg)
  check("not counted", goal._pt2_finish_in_take == nil, goal._pt2_finish_in_take)
end

print("\nE5-9: background walls wait for the hold and a take substate")
do
  local up = {}
  local saved = PC.slot_cover_shots
  PC.slot_cover_shots = function(_, mx, my) return up[sq(mx, my)] and 5 or 0 end
  local goal = { substate = "build_walls",
                 _pt2_bg = { walls = { { mx = 30, my = 30 } }, plan = { { mx = 20, my = 20 } } } }
  check("build_walls: nil", PA.bg_wall(goal, {}) == nil)
  goal.substate = "approach"
  check("approach (not in the allow list): nil", PA.bg_wall(goal, {}) == nil)
  goal.substate = "aim"
  check("aim, a planned wall missing: nil", PA.bg_wall(goal, {}) == nil)
  up[sq(20, 20)] = true
  local w = PA.bg_wall(goal, {})
  check("aim, every planned wall up: the extra wall", w and w.mx == 30 and w.my == 30)
  up[sq(20, 20)] = nil
  check("go stays after a planned wall falls", PA.bg_wall(goal, {}) ~= nil)
  local g2 = { substate = "engage",
               _pt2_bg = { walls = { { mx = 31, my = 31 } }, plan = { { mx = 21, my = 21 } }, go = true } }
  check("start_now_check yes (go): the wall with a planned wall missing", PA.bg_wall(g2, {}) ~= nil)
  PC.slot_cover_shots = saved
end

-- Nov 24 (run_pillblk / run_rings / run_n1_pillfix): a plan square of
-- kind "pill" is a carried pill drop. It costs a pill's trees
-- (PILL_PLACE_TREE_COST, the engine's LGM_COST_PILLNEW), not a wall's,
-- and the hold places it: it never goes to the background walls, which
-- build a wall there (or nothing).
print("\nA planned pill drop: a pill's trees, not a background wall")
do
  local P  = { mx = 40, my = 40, kind = "empty", pt2_kind = "pill" }
  local Wq = { mx = 42, my = 40, kind = "empty", pt2_kind = "wall" }
  local V1 = { mx = 44, my = 40, kind = "empty" }
  local PT, WT = C.PILL_PLACE_TREE_COST, C.WALL_SHIELD_BUILD_COST
  check("plan_trees: a pill drop = a pill's trees", PA.plan_trees({ P }) == PT, PA.plan_trees({ P }))
  check("plan_trees: pill + wall", PA.plan_trees({ P, Wq }) == PT + WT, PA.plan_trees({ P, Wq }))
  check("plan_trees: a v1 square counts as a wall", PA.plan_trees({ V1 }) == WT, PA.plan_trees({ V1 }))
  check("plan_trees: nil = 0", PA.plan_trees(nil) == 0)

  local up = {}
  local saved = PC.slot_cover_shots
  PC.slot_cover_shots = function(_, mx, my) return up[sq(mx, my)] and 5 or 0 end
  local info = { armour = 40, tankx = 30 * 256, tanky = 40 * 256, man_status = C.LGM_INTANK,
                 player_number = 0 }
  local function g() return { substate = "build_walls", _pt2 = { start = { ok = true, pills = {} } } } end
  local goal = g()
  local r = PA.start_now_check(goal, { pills = {} }, info, 1000, { P })
  check("pill drop not down: no start", r == false, r)
  check("pill drop not down: no background wall", goal._pt2_bg == nil,
        goal._pt2_bg and goal._pt2_bg.walls and #goal._pt2_bg.walls)
  goal = g()
  r = PA.start_now_check(goal, { pills = {} }, info, 1000, { P, Wq })
  check("pill drop + wall, pill not down: no start", r == false, r)
  -- the pill is down: the missing wall may start as before
  up[sq(40, 40)] = true
  goal = g()
  r = PA.start_now_check(goal, { pills = {} }, info, 1000, { P, Wq })
  local bgw = goal._pt2_bg and goal._pt2_bg.walls or {}
  check("pill down, wall missing: start", r == true, r)
  check("pill down: only the wall goes to the background",
        #bgw == 1 and bgw[1].mx == 42 and bgw[1].my == 40, #bgw)
  PC.slot_cover_shots = saved
end

-- Nov 24 (run_c2_beside_e): PT2_START_BUILDER_SAFE. The early start also
-- needs the builder to live through his wall trips. The tank's point is
-- (30.5, 40.5); a hostile pill at (36,40) shoots it along the row y = 40.5.
-- Each trip leg is 10 ticks here (the lgm stub), the build 20: out 0-10,
-- on the wall 10-30, home 30-40. The pill's first shell leaves at +1.
print("PT2_START_BUILDER_SAFE: the builder's wall trips")
do
  local saved = PC.slot_cover_shots
  PC.slot_cover_shots = function() return 0 end
  local H = { mx = 36, my = 40, health = 8, owner = "hostile", anger = 0, anger_tick = 0 }
  local world = { pills = { [5] = H }, pill_at = { [sq(36, 40)] = { { id = 5, pill = H } } } }
  local info = { armour = 40, tankx = 30 * 256 + 128, tanky = 40 * 256 + 128,
                 man_status = C.LGM_INTANK, player_number = 0, objects = {} }
  local x = require("live_physics").context(info, {})
  local function g()
    return { substate = "build_walls",
             _pt2 = { start = { ok = true, pills = {}, cx = 30.5, cy = 40.5 } } }
  end
  local function txt(hit)
    return hit and string.format("#%s (%s,%s) +%s %s", tostring(hit.id), tostring(hit.sq_mx),
                                 tostring(hit.sq_my), tostring(hit.t), tostring(hit.how)) or "nil"
  end
  local on_line  = { mx = 33, my = 40, kind = "empty", pt2_kind = "wall" }
  local off_line = { mx = 31, my = 42, kind = "empty", pt2_kind = "wall" }
  local beside   = { mx = 33, my = 41, kind = "empty", pt2_kind = "wall" }
  local K = C.PT2_START_BUILDER_SAFE
  C.PT2_START_BUILDER_SAFE = true

  -- The wall (33,40) is on the pill's row: it is up from +10, and the shell
  -- fired at +1 stops on it at +22 while he still builds there.
  local hit = PA.builder_trip_hit(g(), world, info, 1000, { on_line }, 30, 40, x)
  check("wall on the shell's row: the shell hits his wall while he builds",
        hit and hit.id == 5 and hit.sq_mx == 33 and hit.sq_my == 40 and hit.t == 22, txt(hit))
  local goal = g()
  local r = PA.start_now_check(goal, world, info, 1000, { on_line })
  check("builder unsafe: no early start", r == false, r)
  check("builder unsafe: no background walls", goal._pt2_bg == nil)

  -- A wall off the row: the shells explode on the tank, the first at +49,
  -- and he is home at +40.
  hit = PA.builder_trip_hit(g(), world, info, 1000, { off_line }, 30, 40, x)
  check("trips over before the first explosion: safe", hit == nil, txt(hit))
  r = PA.start_now_check(g(), world, info, 1000, { off_line })
  check("trips over before the first explosion: start", r == true, r)

  -- He builds (33,41), right beside the row: the shells fly past him to
  -- the tank and explode there, so they do not kill him.
  hit = PA.builder_trip_hit(g(), world, info, 1000, { beside }, 30, 40, x)
  check("shells only fly past him: safe", hit == nil, txt(hit))

  C.PT2_START_BUILDER_SAFE = false
  goal = g()
  r = PA.start_now_check(goal, world, info, 1000, { on_line })
  check("knob off: the old result (start)", r == true, r)
  C.PT2_START_BUILDER_SAFE = K
  PC.slot_cover_shots = saved
end

-- Nov 24 (run_c3_beside_e): the hold is over (no missing planned walls) and
-- the builder is out on a background wall on the pill's shell row. The
-- take must not fire, and a new background wall trip there is not sent.
-- Same fixture as above: tank (30.5, 40.5), hostile pill at (36,40), the
-- take's target pill is that pill.
print("PT2_START_BUILDER_SAFE: after the hold (fire hold, background walls)")
do
  local saved = PC.slot_cover_shots
  PC.slot_cover_shots = function() return 0 end
  local H = { mx = 36, my = 40, health = 8, owner = "hostile", anger = 0, anger_tick = 0 }
  local world = { pills = { [5] = H }, pill_at = { [sq(36, 40)] = { { id = 5, pill = H } } } }
  local function inf(out)
    local i = { armour = 40, tankx = 30 * 256 + 128, tanky = 40 * 256 + 128,
                man_status = C.LGM_INTANK, player_number = 0, objects = {} }
    if out then
      i.man_status = C.LGM_MOVING
      i.man_x, i.man_y = 32 * 256 + 128, 40 * 256 + 128
    end
    return i
  end
  local x = require("live_physics").context(inf(), {})
  local function g()
    return { kind = "attack_pill", mx = 36, my = 40, substate = "in_range_aim",
             _pt2 = { start = { ok = true, pills = {}, cx = 30.5, cy = 40.5 } } }
  end
  local K = C.PT2_START_BUILDER_SAFE
  C.PT2_START_BUILDER_SAFE = true

  -- He walks out from (32,40) to the background wall (33,40): it is up
  -- from +10, the pill's shell fired at +1 stops on it at +22.
  local state = { _wall_shield_dispatch = { tick = 990, wx = 33, wy = 40, action = "BUILD" } }
  local goal = g()
  check("out on a background wall on the shell row: hold the fire",
        PA.builder_fire_hold(state, goal, world, inf(true), 1000) == true)
  check("fire hold: logged once (the answer is kept)",
        goal._pt2_bsafe_fire ~= nil and goal._pt2_bsafe_fire:find("^fire|5:33:40") ~= nil,
        goal._pt2_bsafe_fire)
  check("builder in the tank: no fire hold",
        PA.builder_fire_hold(state, goal, world, inf(false), 1001) == false)
  check("fire hold cleared after the builder is safe", goal._pt2_bsafe_fire == "fire|",
        goal._pt2_bsafe_fire)

  -- The dispatch side: from the tank, a background wall on the row is not
  -- sent; one off the row is.
  check("background wall on the shell row: not sent",
        PA.bg_trip_ok(state, g(), world, inf(false), 1000, { mx = 33, my = 40 }) == false)
  check("background wall off the row: sent",
        PA.bg_trip_ok(state, g(), world, inf(false), 1000, { mx = 31, my = 42 }) == true)

  -- The tank's fire makes the target pill angrier. He stands on the wall
  -- (33,40) from +40 to +80 and is home at +90. Calm: the shell at +1 is
  -- past before the wall is up (+22), and the next comes after he is home.
  -- Firing: the tank's first shell lands, the reload halves, and the next
  -- pill shell stops on his wall while he is on it.
  local legs = {
    { t0 = 30, t1 = 40, x0 = 30.5, y0 = 40.5, x1 = 33.5, y1 = 40.5, kind = "out", i = 1 },
    { t0 = 40, t1 = 80, x0 = 33.5, y0 = 40.5, x1 = 33.5, y1 = 40.5, kind = "build", i = 1 },
    { t0 = 80, t1 = 90, x0 = 33.5, y0 = 40.5, x1 = 30.5, y1 = 40.5, kind = "back", i = 1 },
  }
  local hit = PA.builder_trip_hit(g(), world, inf(false), 1000, {}, 0, 0, x,
                                  { legs = legs, home = 90 })
  check("calm pill: safe", hit == nil, hit and tostring(hit.t))
  hit = PA.builder_trip_hit(g(), world, inf(false), 1000, {}, 0, 0, x,
                            { legs = legs, home = 90, fire = true })
  check("tank firing: the angrier pill's shell hits his wall",
        hit ~= nil and hit.sq_mx == 33 and hit.sq_my == 40 and hit.t > 40 and hit.t <= 80,
        hit and string.format("+%d (%s,%s)", hit.t, tostring(hit.sq_mx), tostring(hit.sq_my)))

  C.PT2_START_BUILDER_SAFE = false
  check("knob off: no fire hold (the old result)",
        PA.builder_fire_hold(state, g(), world, inf(true), 1000) == false)
  check("knob off: the background wall is sent (the old result)",
        PA.bg_trip_ok(state, g(), world, inf(false), 1000, { mx = 33, my = 40 }) == true)
  C.PT2_START_BUILDER_SAFE = K
  PC.slot_cover_shots = saved
end

-- Nov 24 (run_c4): the fire hold said "safe -> go" on the very tick the
-- pill fired (beside_n t=489, diag_s 415, beside_se 421, beside_s 502) and
-- the shell already in flight killed the builder on his wall 12 to 22
-- ticks later. Shells in flight count now. And his trip comes from what the
-- brain sees of him (danger.own_builder_timeline), not from a walk estimate.
print("PT2_START_BUILDER_SAFE: shells in flight, the real trip")
do
  local saved = PC.slot_cover_shots
  PC.slot_cover_shots = function() return 0 end
  local H = { mx = 36, my = 40, health = 8, owner = "hostile", anger = 0, anger_tick = 0 }
  local world = { pills = { [5] = H }, pill_at = { [sq(36, 40)] = { { id = 5, pill = H } } } }
  local function inf(objs)
    return { armour = 40, tankx = 30 * 256 + 128, tanky = 40 * 256 + 128,
             man_status = C.LGM_MOVING, man_x = 33 * 256 + 128, man_y = 40 * 256 + 128,
             player_number = 0, objects = objs or {} }
  end
  local x = require("live_physics").context(inf(), {})
  local function g()
    return { kind = "attack_pill", mx = 36, my = 40, substate = "in_range_aim",
             _pt2 = { start = { ok = true, pills = {}, cx = 30.5, cy = 40.5 } } }
  end
  local K = C.PT2_START_BUILDER_SAFE
  C.PT2_START_BUILDER_SAFE = true
  -- He is on his wall (33,40) for 20 more ticks, then walks home.
  local legs = {
    { t0 = 0, t1 = 20, x0 = 33.5, y0 = 40.5, x1 = 33.5, y1 = 40.5, kind = "build", i = 1 },
    { t0 = 20, t1 = 30, x0 = 33.5, y0 = 40.5, x1 = 30.5, y1 = 40.5, kind = "back", i = 1 },
  }
  -- A pill shell fired this tick from (36,40), flying west at the tank.
  local west = { type = 1, owner = 255, info = 2, x = 35 * 256 + 200, y = 40 * 256 + 128,
                 angle = 192, life = 40 }
  local hit = PA.builder_trip_hit(g(), world, inf({ west }), 1000, {}, 0, 0, x,
                                  { legs = legs, home = 30 })
  check("a pill shell in flight stops on his wall: unsafe",
        hit ~= nil and hit.id == "shell" and hit.sq_mx == 33 and hit.sq_my == 40,
        hit and string.format("%s +%d (%s,%s)", tostring(hit.id), hit.t,
                              tostring(hit.sq_mx), tostring(hit.sq_my)))
  local east = { type = 1, owner = 255, info = 2, x = 36 * 256 + 200, y = 40 * 256 + 128,
                 angle = 64, life = 40 }
  hit = PA.builder_trip_hit(g(), world, inf({ east }), 1000, {}, 0, 0, x,
                            { legs = legs, home = 30 })
  check("a pill shell flying away: not his death", hit == nil or hit.id ~= "shell",
        hit and tostring(hit.id))
  local ours = { type = 1, owner = 0, info = 0, x = 35 * 256 + 200, y = 40 * 256 + 128,
                 angle = 192, life = 40 }
  hit = PA.builder_trip_hit(g(), world, inf({ ours }), 1000, {}, 0, 0, x,
                            { legs = legs, home = 30 })
  check("our own shell is not a pill shell", hit == nil or hit.id ~= "shell",
        hit and tostring(hit.id))

  -- The real trip: he reached his spot (33,40) at 995, so 15 ticks of his
  -- stay are left at 1000; then the engine walk takes him home (stub:
  -- 16 wu a tick straight at the tank's square).
  local function st()
    return { _wall_shield_dispatch = { tick = 990, wx = 33, wy = 40, action = "BUILD" },
             _lgm_dispatch = { tick = 990, x = 33, y = 40 },
             _fire_block = { own = { key = 990, arrive = 995, back = false,
                                     hist = {}, hist_tick = 1000 }, ally = {} } }
  end
  local had = rawget(_G, "cpf_lgm_walk_path_wu")
  _G.cpf_lgm_walk_path_wu = function(sx, sy, mx, my, _, _, need, _, out)
    local tx, ty = mx * 256 + 128, my * 256 + 128
    local n, px, py = 0, sx, sy
    while n < need do
      local dx, dy = tx - px, ty - py
      local d = math.sqrt(dx * dx + dy * dy)
      if d < 1 then break end
      local s = math.min(16, d)
      px, py = px + dx / d * s, py + dy / d * s
      n = n + 1
      out[n * 2 - 1], out[n * 2] = px, py
    end
    return n
  end
  local PS = require("pt2_start")
  local tl, home = PA.current_trip(st(), g(), world, inf(), 1000)
  local lx, ly
  if tl then lx, ly = PS.man_at(tl, 14) end
  check("real trip: on the wall for the rest of his stay",
        lx ~= nil and math.floor(lx) == 33 and math.floor(ly) == 40,
        lx and string.format("(%.2f,%.2f)", lx, ly))
  check("real trip: home after the stay and the engine walk",
        home ~= nil and home >= 15 + 40 and home <= 15 + 52, tostring(home))
  -- The engine walk cannot be made: he stays where he is to the end.
  _G.cpf_lgm_walk_path_wu = function() return 0 end
  tl, home = PA.current_trip(st(), g(), world, inf(), 1000)
  check("real trip: a walk that cannot be made keeps him out",
        home == (C.BUILDER_POOL_LGM_STUCK_TICKS or 150), tostring(home))
  _G.cpf_lgm_walk_path_wu = had
  -- No tracker (fire_block has not seen him): the estimate as before.
  local s2 = st(); s2._fire_block = nil
  tl, home = PA.current_trip(s2, g(), world, inf(), 1000)
  check("no tracker: the estimate (a whole build time, then home)",
        tl ~= nil and tl[1].kind == "build" and tl[1].t1 == PS.LGM_BUILD_TIME, tl and tl[1].kind)

  -- The one fire check: danger.fire_block takes the fire bit off while the
  -- hold is on (the stuck walk keeps him on the shell row).
  local D = require("danger")
  _G.cpf_lgm_walk_path_wu = function() return 0 end
  local s3 = st()
  s3.goal = g()
  if rawget(_G, "KEY_SHOOT") == nil then _G.KEY_SHOOT = 1 end
  local KS = KEY_SHOOT
  local k3, t3 = D.fire_block(s3, world, inf({ west }), KS, 0, 1000)
  check("fire_block: the take's fire is held", bit.band(k3, KS) == 0, k3)
  C.PT2_START_BUILDER_SAFE = false
  s3 = st(); s3.goal = g()
  k3 = D.fire_block(s3, world, inf({ west }), KS, 0, 1000)
  check("knob off: fire_block keeps the fire bit (no friendly shell kill here)",
        bit.band(k3, KS) ~= 0, k3)
  _G.cpf_lgm_walk_path_wu = had
  C.PT2_START_BUILDER_SAFE = K
  PC.slot_cover_shots = saved
end

C.PILLTAKE_V2_EVALUATOR = false
-- Nov 24 (c5 beside_n, Andrew's option a): the take's own wall_shield
-- wall. The builder died on (122,116) at t=511, before the tank's first
-- shot at t=513: the pill fired at the tank by itself. Same fixture: tank
-- (30.5, 40.5) in range of the hostile pill at (36,40), which shoots along
-- the row y = 40.5. No start check passed, so no tank fire is counted.
print("PT2_START_BUILDER_SAFE: the take's own wall trips")
do
  local saved = PC.slot_cover_shots
  PC.slot_cover_shots = function() return 0 end
  local H = { mx = 36, my = 40, health = 8, owner = "hostile", anger = 0, anger_tick = 0 }
  local world = { pills = { [5] = H }, pill_at = { [sq(36, 40)] = { { id = 5, pill = H } } } }
  local info = { armour = 40, tankx = 30 * 256 + 128, tanky = 40 * 256 + 128,
                 man_status = C.LGM_INTANK, player_number = 0, objects = {} }
  local list = { { mx = 33, my = 40 }, { mx = 31, my = 42 } }
  local function g()
    return { kind = "attack_pill", mx = 36, my = 40, substate = "build_walls", _pt2 = {},
             _wall_build_list = list, wall_shield = true, wall_mx = 33, wall_my = 40 }
  end
  local K = C.PT2_START_BUILDER_SAFE
  C.PT2_START_BUILDER_SAFE = true
  local goal = g()
  check("wall on the pill's shell row, no tank fire: not sent",
        PA.shield_wall_ok({}, goal, world, info, 1000, { mx = 33, my = 40 }) == false)
  check("that wall is dropped from the take's list",
        PA.shield_wall_skipped(goal, 33, 40) == true and PA.shield_wall_skipped(goal, 31, 42) == false)
  check("logged once as a skip",
        goal._pt2_bsafe_shield ~= nil and goal._pt2_bsafe_shield:find("^shield_wall=%(33,40%)|5:33:40") ~= nil,
        goal._pt2_bsafe_shield)
  check("a dropped wall stays dropped (no new trip check)",
        PA.shield_wall_ok({}, goal, world, info, 1001, { mx = 33, my = 40 }) == false)
  check("wall off the row: sent",
        PA.shield_wall_ok({}, goal, world, info, 1000, { mx = 31, my = 42 }) == true)
  goal._wall_build_list = { { mx = 33, my = 40 } }
  check("a new wall list: the wall is checked again",
        PA.shield_wall_skipped(goal, 33, 40) == false)
  -- builder.set_mode: no wall_shield dispatch for the unsafe wall
  local B = require("builder")
  local state = { builder = {}, tick = 1000 }
  goal = g()
  B.set_mode(state, world, info, goal)
  check("builder: the unsafe wall is not dispatched", state.builder.mode ~= "wall_shield"
        and PA.shield_wall_skipped(goal, 33, 40), state.builder.mode)
  goal = g(); goal.wall_mx, goal.wall_my = 31, 42
  B.set_mode(state, world, info, goal)
  check("builder: the safe wall is dispatched", state.builder.mode == "wall_shield"
        and state.builder.wall_target and state.builder.wall_target.mx == 31, state.builder.mode)
  C.PT2_START_BUILDER_SAFE = false
  goal = g()
  check("knob off: the wall on the row is sent (the old result)",
        PA.shield_wall_ok({}, goal, world, info, 1000, { mx = 33, my = 40 }) == true)
  B.set_mode(state, world, info, goal)
  check("knob off: builder dispatches it", state.builder.mode == "wall_shield", state.builder.mode)
  C.PT2_START_BUILDER_SAFE = K
  PC.slot_cover_shots = saved
end

-- Nov 24 (run_c6): three model fixes, all under PT2_START_BUILDER_SAFE.
--  B  the pill's first shot comes one reload after it first saw our tank
--     (pillbox.c justSeen), not at now + 1 (diag_s, gap_sw, gap_n, gap_nw);
--  A1 a shell in the square of a trip wall that goes up later stops on it
--     (diag_s t=422: "safe -> go", the wall went up 3 ticks later);
--  A2 the wall goes up when he is in the engine's goal box, not when he
--     enters the square.
-- Same fixture: tank (30.5, 40.5), hostile pill at (36,40) on the row.
print("PT2_START_BUILDER_SAFE: pill sight, start-square wall, goal box")
do
  local saved = PC.slot_cover_shots
  PC.slot_cover_shots = function() return 0 end
  local W = require("world")
  local H = { mx = 36, my = 40, health = 8, owner = "hostile", anger = 0, anger_tick = 0 }
  local world = { pills = { [5] = H }, pill_at = { [sq(36, 40)] = { { id = 5, pill = H } } },
                  base_at = {} }
  local function inf(objs)
    return { armour = 40, tankx = 30 * 256 + 128, tanky = 40 * 256 + 128,
             man_status = C.LGM_INTANK, player_number = 0, objects = objs or {} }
  end
  local x = require("live_physics").context(inf(), {})
  local function g()
    return { substate = "build_walls",
             _pt2 = { start = { ok = true, pills = {}, cx = 30.5, cy = 40.5 } } }
  end
  local on_line = { mx = 33, my = 40, kind = "empty", pt2_kind = "wall" }
  local K = C.PT2_START_BUILDER_SAFE
  C.PT2_START_BUILDER_SAFE = true

  -- The tracker: in range from the first tick seen, cleared out of range.
  local rt = U.ttype
  U.ttype = function() return C.T_GRASS end
  H._tank_seen_since = nil
  W.track_tank_sight(world, inf(), 950)
  W.track_tank_sight(world, inf(), 951)
  check("sight tracker: the first tick in range is kept", H._tank_seen_since == 950,
        tostring(H._tank_seen_since))
  local far = inf(); far.tankx = 10 * 256
  W.track_tank_sight(world, far, 952)
  check("sight tracker: out of range clears it", H._tank_seen_since == nil,
        tostring(H._tank_seen_since))
  -- In deep forest beyond tree_hide_distance the pill does not see us.
  U.ttype = function() return C.T_FOREST end
  W.track_tank_sight(world, inf(), 953)
  check("sight tracker: hidden in trees, not seen", H._tank_seen_since == nil,
        tostring(H._tank_seen_since))
  U.ttype = rt

  -- B: the pill saw us 50 ticks ago (reload 100): its first shell leaves
  -- at +50, after he is home (+40). The old now + 1 shell hits his wall.
  H._tank_seen_since = 950
  local hit = PA.builder_trip_hit(g(), world, inf(), 1000, { on_line }, 30, 40, x)
  check("pill saw us 50 ticks ago: its first shell is after he is home", hit == nil,
        hit and tostring(hit.t))
  -- It saw us 95 ticks ago: the shell leaves at +5 and stops on his wall.
  H._tank_seen_since = 905
  hit = PA.builder_trip_hit(g(), world, inf(), 1000, { on_line }, 30, 40, x)
  check("pill saw us 95 ticks ago: its shell at +5 hits his wall",
        hit and hit.sq_mx == 33 and hit.sq_my == 40 and hit.t == 26, hit and tostring(hit.t))
  -- It saw us 250 ticks ago: it fired at 950 + 100 k, the last at 900+100.
  H._tank_seen_since = 750
  hit = PA.builder_trip_hit(g(), world, inf(), 1000, { on_line }, 30, 40, x)
  check("pill saw us 250 ticks ago: next shell at +50, safe", hit == nil,
        hit and tostring(hit.t))
  -- No sight tick (the pill does not see us now): the old now + 1 shell.
  H._tank_seen_since = nil
  hit = PA.builder_trip_hit(g(), world, inf(), 1000, { on_line }, 30, 40, x)
  check("no sight tick: the shell at +1 hits his wall (as before)",
        hit and hit.t == 22, hit and tostring(hit.t))

  -- A1: a pill shell inside (33,40) flying west; his wall there goes up at
  -- +3, before the shell leaves the square. It stops on the wall.
  local legs = {
    { t0 = 0, t1 = 3, x0 = 32.8, y0 = 40.5, x1 = 33.5, y1 = 40.5, kind = "out", i = 1 },
    { t0 = 3, t1 = 23, x0 = 33.5, y0 = 40.5, x1 = 33.5, y1 = 40.5, kind = "build", i = 1 },
    { t0 = 23, t1 = 33, x0 = 33.5, y0 = 40.5, x1 = 30.5, y1 = 40.5, kind = "back", i = 1 },
  }
  local west = { type = 1, owner = 255, info = 2, x = 33 * 256 + 250, y = 40 * 256 + 128,
                 angle = 192, life = 40 }
  local up_at = { [sq(33, 40)] = 3 }
  hit = PA.inflight_hit(world, inf({ west }), legs, 33, up_at)
  check("shell in the square of a wall that goes up later: stops on it",
        hit ~= nil and hit.sq_mx == 33 and hit.sq_my == 40 and hit.t == 3,
        hit and string.format("+%d (%s,%s)", hit.t, tostring(hit.sq_mx), tostring(hit.sq_my)))
  -- A wall up now (t = 0) in its square: the shell is flying out of it.
  hit = PA.inflight_hit(world, inf({ west }), legs, 33, { [sq(33, 40)] = 0 })
  check("shell in the square of a wall up now: flies on", hit == nil or hit.sq_mx ~= 33,
        hit and tostring(hit.sq_mx))

  -- A2: the brain's own timeline walks him east from (32.5, 40.5) at
  -- 1/16 square a tick. He enters (33,40) at +8 and is in the goal box
  -- (16 wu from the centre) at +15: the wall goes up at +15.
  local D = require("danger")
  local otl = D.own_builder_timeline
  D.own_builder_timeline = function()
    local out = {}
    for t = 0, 40 do
      local px = 32.5 + math.min(t, 16) / 16
      out[t] = { px, 40.5 }
    end
    return out
  end
  local man = inf(); man.man_status = C.LGM_MOVING
  man.man_x, man.man_y = 32 * 256 + 128, 40 * 256 + 128
  local st = { _wall_shield_dispatch = { tick = 990, wx = 33, wy = 40, action = "BUILD" } }
  local gg = { kind = "attack_pill", mx = 36, my = 40 }
  local tl = PA.current_trip(st, gg, world, man, 1000)
  local b0
  for _, L in ipairs(tl or {}) do if L.kind == "build" then b0 = L.t0 end end
  check("real trip: the wall goes up in the goal box (+15), not on entering (+8)",
        b0 == 15, tostring(b0))
  D.own_builder_timeline = otl

  C.PT2_START_BUILDER_SAFE = K
  PC.slot_cover_shots = saved
end

-- =========================================================================
-- run_c7 model fixes (PT2_START_BUILDER_SAFE and the quick repair):
--  1 the engine's pill reload ladder from the hits seen on the pill
--    (pt2_start.engine_reload, world.note_eng_hit), used by the trip check
--    and quick_repair.pill_next_fire;
--  2 a pill that does not see the tank yet first sees it on the drive to
--    the firing point (pt2_attack.sight_tick);
--  3 our shells in flight walk on from where the engine has them
--    (quick_repair.shell_real_walk) in update_shot_accounting, and a graze
--    still gets a landing tick (quick_repair.shot_near_tick).
print("run_c7: engine reload ladder, drive-in sight, real shell walk")
do
  local PS = require("pt2_start")
  local W  = require("world")
  local QR = require("quick_repair")
  local x  = { pill_attack_ticks = 100, pill_attack_min_ticks = 6,
               pill_angry_divisor = 2, pill_cooldown_ticks = 32 }
  -- 1. The ladder (pillbox.c): halve on a hit, +1 every 32 ticks after it.
  check("ladder: no hit = calm reload 100", PS.engine_reload({}, 500, x) == 100)
  check("ladder: hit at 100 -> 50", PS.engine_reload({ 100 }, 100, x) == 50)
  check("ladder: 31 ticks on still 50", PS.engine_reload({ 100 }, 131, x) == 50)
  check("ladder: 32 ticks on 51", PS.engine_reload({ 100 }, 132, x) == 51)
  check("ladder: 64 ticks on 52", PS.engine_reload({ 100 }, 164, x) == 52)
  check("ladder: two hits on one tick -> 25", PS.engine_reload({ 100, 100 }, 100, x) == 25)
  -- hit at 100 (50), at 140 it is 51 (one step at 132), a hit halves to 25,
  -- the next step is 32 ticks after the second hit.
  check("ladder: hit at 140 after one step -> 25",
        PS.engine_reload({ 100, 140 }, 140, x) == 25, PS.engine_reload({ 100, 140 }, 140, x))
  check("ladder: and 26 at 172", PS.engine_reload({ 100, 140 }, 172, x) == 26)
  check("ladder: floored at the minimum 6",
        PS.engine_reload({ 1, 1, 1, 1, 1, 1, 1 }, 1, x) == 6)
  check("ladder: back to 100 after 50 steps", PS.engine_reload({ 100 }, 100 + 50 * 32, x) == 100)
  check("ladder: hits after t do not count", PS.engine_reload({ 100, 200 }, 150, x) == 51)
  check("ladder: predicted hits count too", PS.engine_reload({ 100 }, 150, x, { 140 }) == 25)

  -- world.note_eng_hit: the hit list, cleared once the pill is calm again.
  local K = C.PT2_START_BUILDER_SAFE
  C.PT2_START_BUILDER_SAFE = true
  local P = {}
  W.note_eng_hit(P, { rules = {} }, 10)
  W.note_eng_hit(P, { rules = {} }, 20)
  check("hit list: both hits kept", P._eng_hits and #P._eng_hits == 2)
  W.note_eng_hit(P, { rules = {} }, 20 + 100 * 32 + 1)
  check("hit list: starts over once calm", #P._eng_hits == 1)
  C.PT2_START_BUILDER_SAFE = false
  local P2 = {}
  W.note_eng_hit(P2, { rules = {} }, 10)
  check("hit list: knob off, not kept", P2._eng_hits == nil)
  C.PT2_START_BUILDER_SAFE = true

  -- quick_repair.pill_next_fire takes the ladder reload.
  local PP = { mx = 36, my = 40, anger = 0, anger_tick = 0, _eng_hits = { 990 } }
  local _, rl = QR.pill_next_fire(PP, { objects = {} }, 1000, x)
  check("pill_next_fire: ladder reload 50 after a hit 10 ticks ago", rl == 50, rl)

  -- The trip check with the ladder (fixture of the section above): the pill
  -- saw us 150 ticks ago and was hit 100 ticks ago. With anger 0 the brain
  -- thought its reload is 100 (next shell at +50, safe). The engine's reload
  -- is 53 (50 + 3 steps): it fired at 956 and fires again at 1009 (+9).
  local saved = PC.slot_cover_shots
  PC.slot_cover_shots = function() return 0 end
  local H = { mx = 36, my = 40, health = 8, owner = "hostile", anger = 0, anger_tick = 0 }
  local world = { pills = { [5] = H }, pill_at = { [sq(36, 40)] = { { id = 5, pill = H } } },
                  base_at = {} }
  local function inf()
    return { armour = 40, tankx = 30 * 256 + 128, tanky = 40 * 256 + 128,
             man_status = C.LGM_INTANK, player_number = 0, objects = {} }
  end
  local lx = require("live_physics").context(inf(), {})
  local function g()
    return { substate = "build_walls",
             _pt2 = { start = { ok = true, pills = {}, cx = 30.5, cy = 40.5 } } }
  end
  local on_line = { mx = 33, my = 40, kind = "empty", pt2_kind = "wall" }
  H._tank_seen_since = 850
  local hit = PA.builder_trip_hit(g(), world, inf(), 1000, { on_line }, 30, 40, lx)
  check("trip check, brain anger 0: safe", hit == nil, hit and tostring(hit.t))
  H._eng_hits = { 900 }
  hit = PA.builder_trip_hit(g(), world, inf(), 1000, { on_line }, 30, 40, lx)
  check("trip check, engine ladder (hit 100 ago): its shell at +9 hits his wall",
        hit ~= nil and hit.sq_mx == 33 and hit.sq_my == 40, hit and tostring(hit.t))
  H._eng_hits = nil

  -- 2. Drive-in sight. The tank is at (10.5, 40.5), out of range of the pill
  -- (36,40) (8 squares); its firing point is (30.5, 40.5). On that line it
  -- comes in range at x = 28.5 (square 28). Drive time stub: 4 ticks a square.
  local est = cpf.estimate_tank_travel_ticks
  cpf.estimate_tank_travel_ticks = function(sx, sy, dx, dy) return math.abs(dx - sx) * 4 end
  local far = inf(); far.tankx = 10 * 256 + 128
  local st = PA.sight_tick(H, far, 30.5, 40.5, 8, 1000)
  check("sight tick: drive time to the first square in range (18 squares)",
        st == 1000 + 72, st)
  check("sight tick: in range now gives nil (old rule)",
        PA.sight_tick(H, inf(), 30.5, 40.5, 8, 1000) == nil)
  check("sight tick: never in range on the line gives nil",
        PA.sight_tick(H, far, 12.5, 40.5, 8, 1000) == nil)
  H._tank_seen_since = nil
  hit = PA.builder_trip_hit(g(), world, far, 1000, { on_line }, 30, 40, lx,
                            { tx = 30.5, ty = 40.5 })
  check("trip check: pill sees us at +72, first shell at +172, safe", hit == nil,
        hit and tostring(hit.t))
  cpf.estimate_tank_travel_ticks = function() return -1 end
  hit = PA.builder_trip_hit(g(), world, far, 1000, { on_line }, 30, 40, lx,
                            { tx = 30.5, ty = 40.5 })
  check("trip check: no drive estimate, the old shell at +1", hit ~= nil,
        hit and tostring(hit.t))
  cpf.estimate_tank_travel_ticks = est
  PC.slot_cover_shots = saved

  -- 3. The real shell walk (c6 beside_e, engine tick 1320): our shell at
  -- (32209, 30466) with 1 move left, fired at angle 200.875. Its last move
  -- takes it into pill square (125,118).
  local s = { real_fx = 32209, real_fy = 30466, real_life = 1, angle = 200.875, speed = 32 }
  local wk = QR.shell_real_walk(s)
  local last = wk and wk[#wk]
  check("real walk: the last move enters (125,118)",
        last and last.mx == 125 and last.my == 118 and last.k == 1,
        last and string.format("(%d,%d) k=%d", last.mx, last.my, last.k))
  check("real walk: no match, no walk", QR.shell_real_walk({ angle = 200 }) == nil)

  -- shot_near_tick: a shell flying west along y = 119.0 + 6 wu.
  local gs = { cur_fx = 33000, cur_fy = 30470, step_x = -32, step_y = 0 }
  check("graze: no exact tile tick", QR.shot_land_tick(gs, 125, 118, 500) == nil)
  check("graze: closest pass tick", QR.shot_near_tick(gs, 125, 118, 500) == 524,
        QR.shot_near_tick(gs, 125, 118, 500))

  -- update_shot_accounting (attack.lua): the same shell seen this tick.
  local A  = require("attack")
  local ST = require("shot_tracker")
  local QK = C.QUICK_REPAIR_ON_TAKE
  C.QUICK_REPAIR_ON_TAKE = true
  local rt = U.ttype
  U.ttype = function() return C.T_GRASS end
  local ss = cpf.simulate_shot
  -- The old sim shell: it ends one square beside the pill.
  cpf.simulate_shot = function()
    return { { mx = 132, my = 120 }, { mx = 126, my = 119 }, { mx = 125, my = 119 } }
  end
  local saved_shots = ST.shots
  local shot = { fire_tick = 660, fire_fx = 33958 - 160, fire_fy = 30854 - 35,
                 step_x = -31, step_y = -6, cur_fx = 32240, cur_fy = 30470,
                 status = "in_flight", angle = 200.875, speed = 32,
                 real_fx = 32209, real_fy = 30466, real_life = 1, real_tick = 675 }
  ST.shots = { shot }
  local goal = { kind = "attack_pill", mx = 125, my = 118, _attack_start_tick = 600 }
  A._update_shot_accounting(goal, { pills = {}, pill_at = {} }, 675)
  check("accounting: the real walk counts the shell in the air",
        goal._on_target_in_flight == 1 and goal._on_target_land
        and goal._on_target_land[1] == 676,
        tostring(goal._on_target_in_flight) .. " " .. tostring(goal._on_target_land and goal._on_target_land[1]))
  shot.real_tick = 674   -- not seen this tick: the old sim shell
  A._update_shot_accounting(goal, { pills = {}, pill_at = {} }, 675)
  check("accounting: not seen this tick, the old sim (misses)",
        goal._on_target_in_flight == 0, goal._on_target_in_flight)
  ST.shots = saved_shots
  cpf.simulate_shot = ss
  U.ttype = rt
  C.QUICK_REPAIR_ON_TAKE = QK
  C.PT2_START_BUILDER_SAFE = K
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
