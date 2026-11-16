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
local V2  = require("pilltake_v2")
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

C.PILLTAKE_V2_EVALUATOR = false
print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
