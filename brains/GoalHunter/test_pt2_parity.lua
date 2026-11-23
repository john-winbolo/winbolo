-- =========================================================================
-- test_pt2_parity.lua -- standalone unit tests (2026-11-16): pill take v1
-- and v2 on ONE pill do more or less the same thing.
--
-- v1 = attack_shield.scan on its Lua path (no C module), started from the
-- spot plan_position would pick (the clear-shot spot with the lowest travel
-- cost). v2 = pilltake_v2.evaluate with the same travel cost. Both see the
-- same hand-made map. The shell stand-in for v1 is the engine-exact walk
-- from shadow.lua, so both read the same squares.
--
-- For each layout the test asks (tolerances: proposal, awaiting Andrew's
-- value):
--   * same go / no-go;
--   * standoffs within 64 wu of each other;
--   * the same blocker squares, or v2 cover >= v1 cover (capped at the bar);
--   * approach angles within 30 degrees.
-- It prints a v1-vs-v2 table.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_parity.lua
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

_G.print2 = _G.print2 or function() end

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
local S  = require("attack_shield")
local V2 = require("pilltake_v2")

local PMX, PMY = 128, 120
local HOME = { 128.5, 134.5 }           -- where the tank comes from (south)

local function make_world(extra_pills)
  local target = { mx = PMX, my = PMY, owner = "hostile", health = 15 }
  local pills, pill_at = { [1] = target }, {}
  for i, p in ipairs(extra_pills or {}) do pills[i + 1] = p end
  for id, p in pairs(pills) do
    pill_at[sq(p.mx, p.my)] = { { id = id, pill = p } }
  end
  return { pills = pills, pill_at = pill_at, bases = {}, base_at = {} }, target
end

local function travel(cx, cy)
  local dx, dy = cx - HOME[1], cy - HOME[2]
  return math.sqrt(dx * dx + dy * dy)
end

-- v1: the clear-shot ring spot with the lowest travel cost (what
-- plan_position takes from the green set), then the shield scan around it.
local function run_v1(world, target, no_builder, npb)
  local R = C.PPT_STANDOFF or 7.0
  local pcx, pcy = PMX + 0.5, PMY + 0.5
  local env = V2.world_env(world, C, U, PC)
  local best_deg, best_cost = nil, nil
  for deg = 0, 358, 2 do
    local cx = pcx + math.sin(math.rad(deg)) * R
    local cy = pcy - math.cos(math.rad(deg)) * R
    local mx, my = math.floor(cx), math.floor(cy)
    local tt = U.ttype(mx, my)
    if (C.TERRAIN_COST_LAND[tt] or 9999) < 9999 and not U.is_water(tt) then
      local px, py = math.floor(cx * 256 + 0.5), math.floor(cy * 256 + 0.5)
      local los = false
      for ai = 1, #V2.AIMS do
        local off = V2.AIMS[ai]
        if SH.tank_shot(env, px, py, PMX * 256 + off[1], PMY * 256 + off[2], PMX, PMY) then
          los = true; break
        end
      end
      local cost = travel(cx, cy)
      if los and (not best_cost or cost < best_cost) then best_deg, best_cost = deg, cost end
    end
  end
  if not best_deg then return { go = false } end
  local cx = pcx + math.sin(math.rad(best_deg)) * R
  local cy = pcy - math.cos(math.rad(best_deg)) * R
  local r = S.scan(target, world, math.floor(cx), math.floor(cy), best_deg, cx, cy, R,
                   no_builder, 40, nil, nil, npb)
  local b = r.best
  if not b or (b.score or 0) <= 0 then return { go = false, deg = best_deg } end
  local aim = b.aims[b.best_aim_idx]
  local act, pot = {}, {}
  local cover = 0
  for _, bl in ipairs(aim.blockers or {}) do
    act[#act + 1] = sq(bl.mx, bl.my)
    if bl.kind == "friendly_pill" then
      cover = cover + PC.pill_walls(PC.pill_on_square(world, bl.mx, bl.my)) * 5
    else
      cover = cover + 5
    end
  end
  local pills_left = npb
  for _, bl in ipairs(aim.potential_blockers or {}) do
    pot[#pot + 1] = sq(bl.mx, bl.my)
    if pills_left > 0 then cover = cover + 15; pills_left = pills_left - 1
    else cover = cover + 5 end
  end
  return { go = true, deg = b.deg, cx = b.cx, cy = b.cy, act = act, pot = pot,
           cover = cover, aim = b.best_aim_idx }
end

local function run_v2(world, target, no_builder, npb)
  local inp = V2.knob_inp(C, {})
  inp = V2.world_inp(world, C, U, PC, cpf, inp, no_builder)
  inp.A = target
  inp.radius = C.PPT_STANDOFF or 7.0
  inp.num_pill_blockers = npb
  inp.score_fn = function(deg, mx, my, cx, cy) return travel(cx, cy) end
  local res = V2.evaluate(inp)
  local b = res.best
  _G._pt2_last_inp = inp
  if not b or not b.safe then return { go = false, res = res, b = b } end
  local blk = {}
  for _, r in ipairs(b.rays) do
    if r.X == target and r.hit == "tank" then
      for k = 1, r.n_block do blk[#blk + 1] = r.block[k].sq end
    end
  end
  return { go = true, deg = b.deg, cx = b.cx, cy = b.cy, blk = blk, plan = b.plan,
           cover = b.have, kills = b.kills, chain_mode = b.chain_mode, aim = b.aim_idx,
           res = res, b = b }
end

local function sqs(list)
  local p = {}
  for _, s in ipairs(list or {}) do p[#p + 1] = string.format("(%d,%d)", s % 256, math.floor(s / 256)) end
  return #p > 0 and table.concat(p, " ") or "-"
end

local rows = {}
local function layout(name, setup, opts)
  opts = opts or {}
  for k in pairs(terrain) do terrain[k] = nil end
  local extra = setup and setup() or nil
  local world, target = make_world(extra)
  local npb = opts.npb or 0
  local v1 = run_v1(world, target, opts.no_builder, npb)
  local v2 = run_v2(world, target, opts.no_builder, npb)
  print(name)
  check(name .. ": same go / no-go", v1.go == v2.go,
        string.format("v1=%s v2=%s", tostring(v1.go), tostring(v2.go)))
  local dwu, ddeg = nil, nil
  if v1.go and v2.go then
    local dx, dy = (v1.cx - v2.cx) * 256, (v1.cy - v2.cy) * 256
    dwu = math.sqrt(dx * dx + dy * dy)
    ddeg = math.abs(((v1.deg - v2.deg) + 540) % 360 - 180)
    -- v1's own spot and blockers, judged by v2's leeway test
    local inp = _G._pt2_last_inp
    local ex, left = {}, npb
    for _, s in ipairs(v1.pot) do
      if left > 0 then ex[s] = 15; left = left - 1 else ex[s] = 5 end
    end
    v1.lee = V2.leeway_at(inp, math.floor(v1.cx * 256), math.floor(v1.cy * 256), ex)
    local cap = inp.leeway_min
    local roomier = math.min(v2.b.leeway or 0, cap) > math.min(v1.lee, cap)
    if not opts.loose_standoff then
      check(name .. ": standoffs within " .. TOL_WU .. " wu, or v2's spot has more leeway",
            dwu <= TOL_WU or roomier,
            string.format("%.0f wu, leeway v1=%d v2=%d", dwu, v1.lee, v2.b.leeway or -1))
    end
    check(name .. ": angles within " .. TOL_DEG .. " deg", ddeg <= TOL_DEG, string.format("%.1f deg", ddeg))
    local bar = C.PPT_COVER_TARGET_SHOTS or 15
    local v1sq, v2sq = {}, {}
    for _, s in ipairs(v1.act) do v1sq[s] = true end
    for _, s in ipairs(v1.pot) do v1sq[s] = true end
    local same = true
    for _, s in ipairs(v2.blk) do v2sq[s] = true; if not v1sq[s] then same = false end end
    for s in pairs(v1sq) do if not v2sq[s] then same = false end end
    check(name .. ": same blocker squares or v2 cover >= v1 cover (capped at " .. bar .. ")",
          same or v2.cover >= math.min(bar, v1.cover),
          string.format("v1=%d v2=%d", v1.cover, v2.cover))
    check(name .. ": v2 takes it alone (no chain)", v2.kills == 1 and not v2.chain_mode, v2.kills)
  end
  rows[#rows + 1] = {
    name = name, v1 = v1, v2 = v2, dwu = dwu, ddeg = ddeg,
  }
end

local T = C
-- 1. open ground, builder, no pills carried: build three walls
layout("open ground")
-- 2. one wall already on the line from the pill to the south
layout("one existing wall", function() terrain[sq(128, 123)] = T.T_BUILDING end)
-- 3. two walls
layout("two walls", function()
  terrain[sq(128, 123)] = T.T_BUILDING
  terrain[sq(128, 124)] = T.T_HALFBUILD
end)
-- 4. an allied pill as the blocker
layout("allied pill blocker", function()
  return { { mx = 128, my = 123, owner = "friendly", health = 15 } }
end)
-- 5. a wall must be built, and we carry a pill to drop
layout("carried pill to drop", nil, { npb = 1 })
-- 6. water to the south and forest to the east of the pill
layout("near water and forest", function()
  for x = 120, 136 do for y = 124, 126 do terrain[sq(x, y)] = T.T_RIVER end end
  for x = 131, 137 do for y = 116, 124 do terrain[sq(x, y)] = T.T_FOREST end end
end)
-- 7. a second hostile pill beside the target that is NOT a chain: v1 does
-- not see its fire, v2 must also cover it (5 shots), so the standoff may move.
layout("second hostile pill, not a chain", function()
  return { { mx = 134, my = 118, owner = "hostile", health = 15 } }
end, { loose_standoff = true })
-- 8. no builder and open ground: neither v1 nor v2 can shield the take
layout("no builder, open ground", nil, { no_builder = true })

-- ── Table ────────────────────────────────────────────────────────────────
print("")
print(string.format("%-34s %-4s %-4s %6s %6s %6s %5s %5s %6s %5s %5s  %-30s %s",
  "layout", "v1", "v2", "v1deg", "v2deg", "d_wu", "v1cov", "v2cov", "d_deg", "v1lee", "v2lee", "v1 blockers (act | pot)", "v2 blockers (incl. planned)"))
for _, r in ipairs(rows) do
  local v1, v2 = r.v1, r.v2
  print(string.format("%-34s %-4s %-4s %6s %6s %6s %5s %5s %6s %5s %5s  %-30s %s",
    r.name, v1.go and "go" or "no", v2.go and "go" or "no",
    v1.deg and string.format("%.1f", v1.deg) or "-", v2.deg and string.format("%.1f", v2.deg) or "-",
    r.dwu and string.format("%.0f", r.dwu) or "-",
    v1.cover and tostring(v1.cover) or "-", v2.cover and tostring(v2.cover) or "-",
    r.ddeg and string.format("%.1f", r.ddeg) or "-",
    v1.lee and tostring(v1.lee) or "-", (v2.b and v2.b.leeway) and tostring(v2.b.leeway) or "-",
    v1.go and (sqs(v1.act) .. " | " .. sqs(v1.pot)) or "-",
    v2.go and sqs(v2.blk) or "-"))
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
