-- =========================================================================
-- test_charge_leeway.lua — standalone unit tests (2026-11-03) for
-- C.CHARGE_LEEWAY: the charge tests its 5 aims (centre + 4 corners) from
-- where the tank will stop, and keeps going when one is clear.
--
-- The recorded game (20261103_192602 part 2, player 2, t=9636): blitz
-- soldier charge on pill #2 at (113,130). The tank, at (115.246,138.203)
-- with speed 44 and still braking, passed the at-spot test 0.97 tile from
-- its spot. The live gun line hit our own pill #3 at (114,131). From the
-- live position centre, TL and BL fell short and TR, BR hit pill #3, so
-- try_reaim said "blocked" and the charge banned the angle and replanned.
-- From the stop point on its north line (115.246,137.277) centre, TL and BL
-- reach the pill clear.
--
-- Checked here:
--   * knob defaults (HELM true, KEEL false)
--   * spot_margin.aim_line_class: clear / blocked / short
--   * attack.charge_stop_point: brake-now stop, and the ring point on the
--     16-direction travel line
--   * charge_leeway: the t=9636 case keeps going with the centre aim; a
--     truly blocked stop point bans (at the spot) or goes on (not yet)
--   * update_attack_substate, charge substate, the t=9636 case: knob on
--     keeps charging with the centre aim; knob off bans and replans as before
-- No engine: the shell walk is a Lua port of the engine's shell flight.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_charge_leeway.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

_G.print2 = _G.print2 or function() end
-- The C threat grids are not loaded here: every lookup reads 0.
_G.gh_threat = _G.gh_threat or setmetatable({}, { __index = function()
  return function() return 0 end
end })

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local SM     = require("spot_margin")
local A      = require("attack")

local WALLS = {}                                  -- packed my*256+mx -> true
U.ttype      = function(x, y) return WALLS[y * 256 + x] and C.T_BUILDING or 4 end
U.ttype_peek = U.ttype
cpf.SHOT_TANK = cpf.SHOT_TANK or 0

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end

-- ── Shell flight (Lua port of shells.c spawn / move, tank shell) ──────────
local SHELL_SPEED, SHELL_LIFE, SHELL_START_ADD, GUNSIGHT_MAX = 32, 8, 5, 14
local function round(v) return v >= 0 and math.floor(v + 0.5) or -math.floor(-v + 0.5) end
local function calc(angle, speed, hp)
  angle = angle - 64; if angle < 0 then angle = angle + 256 end
  local rad = (360 / 256 * angle) / 360 * 2 * math.pi
  local k = hp and 256 or 1
  return round(speed * math.cos(rad) * k), round(speed * math.sin(rad) * k)
end
local function ang_from(ox, oy, tx, ty)
  local a = math.atan2(tx - ox, -(ty - oy)) * 128 / math.pi
  return (a % 256 + 256) % 256
end
local function walk(ox, oy, angle)
  local xa, ya = calc(angle, SHELL_SPEED, false)
  local x, y = ox + SHELL_START_ADD * xa, oy + SHELL_START_ADD * ya
  local ticks = 1 + math.floor(SHELL_LIFE * (GUNSIGHT_MAX / 2)) - SHELL_START_ADD
  local xs, ys = calc(angle, SHELL_SPEED, true)
  local xacc, yacc = 0, 0
  local lmx, lmy = math.floor(ox / 256), math.floor(oy / 256)
  local tiles = { { mx = lmx, my = lmy } }
  local function add(px, py)
    local mx, my = math.floor(px / 256), math.floor(py / 256)
    if mx ~= lmx or my ~= lmy then tiles[#tiles + 1] = { mx = mx, my = my }; lmx, lmy = mx, my end
  end
  add(x, y)
  for _ = 1, ticks do
    xacc, yacc = xacc + xs, yacc + ys
    local xm, ym = math.floor(xacc / 256), math.floor(yacc / 256)
    xacc, yacc = xacc - xm * 256, yacc - ym * 256
    x, y = x + xm, y + ym
    add(x, y)
  end
  return tiles
end
cpf.simulate_shot_angle = function(ox, oy, a) return walk(ox, oy, a) end
cpf.simulate_shot = function(ox, oy, awx, awy) return walk(ox, oy, ang_from(ox, oy, awx, awy)) end

-- Brake-now stop stand-in: set per test.
local STOP = nil
cpf.predict_stop = function(x, y) if STOP then return STOP[1], STOP[2] end return x, y end

-- ── The t=9636 world ─────────────────────────────────────────────────────
local PMX, PMY = 113, 130
local function mkpill(id, mx, my, hp) return { id = id, mx = mx, my = my, health = hp or 15 } end
local function mkworld()
  local pills = { [2] = mkpill(2, 113, 130), [3] = mkpill(3, 114, 131),
                  [0] = mkpill(0, 111, 126), [1] = mkpill(1, 111, 127) }
  local pill_at = {}
  for id, p in pairs(pills) do pill_at[p.my * 256 + p.mx] = { { id = id, pill = p } } end
  return { pills = pills, pill_at = pill_at, base_at = {} }
end
local world = mkworld()

local TX, TY   = 29503, 35380                     -- tank t=9636 (115.246,138.203)
local SX, SY   = 29503, 35143                     -- north-line stop (115.246,137.277)
local SFX, SFY = 114.7155, 137.3937               -- the charge spot (7.0 ring)
local NAMES = { "center", "TL", "TR", "BL", "BR" }

print("knobs")
local keel = C.PRESETS and C.PRESETS.keel or {}
check("CHARGE_LEEWAY default true", C.CHARGE_LEEWAY == true, C.CHARGE_LEEWAY)
check("PRESETS.keel CHARGE_LEEWAY = false", keel.CHARGE_LEEWAY == false, keel.CHARGE_LEEWAY)

print("spot_margin.aim_line_class from the live tank (t=9636)")
do
  local want = { "short", "short", "blocked", "short", "blocked" }
  for i = 1, 5 do
    local cls = SM.aim_line_class(TX, TY, 115, 138, PMX, PMY, world, i)
    check(NAMES[i] .. " = " .. want[i], cls == want[i], cls)
  end
  check("old test (clear_aim_from_world) finds no aim from the tank",
        A._clear_aim_from_world(TX, TY, PMX, PMY, world, 5) == nil)
end

print("spot_margin.aim_line_class from the stop point")
do
  local want = { "clear", "clear", "blocked", "clear", "blocked" }
  for i = 1, 5 do
    local cls, trees = SM.aim_line_class(SX, SY, 115, 137, PMX, PMY, world, i)
    check(NAMES[i] .. " = " .. want[i], cls == want[i], cls)
    if cls == "clear" then check(NAMES[i] .. " trees 0", trees == 0, trees) end
  end
  -- Agrees with the old test where the old test has an answer.
  local tr = SM.aim_line_trees(SX, SY, 115, 137, PMX, PMY, world, 1)
  check("centre clear matches aim_line_trees", tr == 0, tr)
end

print("attack.charge_stop_point")
local function info9636(speed, angle)
  return { tankx = TX, tanky = TY, speed = speed or 44, tank_angle = angle or 248.5,
           direction = 248, shells = 40, armour = 40, gunrange = 14, trees = 18,
           player_number = 2 }
end
local function charge_goal(aim_idx)
  local g = { kind = "attack_pill", target_id = 2, mx = PMX, my = PMY,
              substate = "charge", standoff_fx = SFX, standoff_fy = SFY,
              standoff_mx = 114, standoff_my = 137, _charge_pulled = true,
              _chosen_deg = 170 }
  if aim_idx then
    local off = require("attack_shield").AIM_OFFSETS_TILE_FIRE[aim_idx]
    g.aim_idx = aim_idx
    g.aim_wx = PMX * 256 + math.floor(off[1] * 256)
    g.aim_wy = PMY * 256 + math.floor(off[2] * 256)
  end
  return g
end
do
  STOP = { SX, SY }                               -- braking already: stops inside the ring
  local x, y, how = A.charge_stop_point({}, charge_goal(), info9636(), PMX, PMY)
  check("braking: the brake-now stop", x == SX and y == SY and how == "brake",
        string.format("%s,%s %s", x, y, how))
  STOP = { TX, TY - 64 }                          -- brake-now stop still outside the ring
  x, y, how = A.charge_stop_point({}, charge_goal(), info9636(), PMX, PMY)
  check("not yet braking: the ring point on the north line", how == "ring"
        and x == TX and math.abs(y - SY) <= 2, string.format("%s,%s %s", x, y, how))
  x, y, how = A.charge_stop_point({}, charge_goal(), info9636(44, 248.0), PMX, PMY)
  check("facing 248.0 travels dir16 240, not north", how == "ring" and x < TX,
        string.format("%s,%s %s", x, y, how))
end

print("charge_leeway, t=9636: moving, falls short from the tank, centre clear from the stop")
do
  STOP = { SX, SY }
  local g = charge_goal(5)                        -- planned BR: the live line hits pill #3
  local r = A._charge_leeway({}, g, world, info9636(), PMX, PMY, 9636, true, "pill at (114,131) hp=15")
  check("returns ok (no ban)", r == "ok", r)
  check("switched to the centre aim", g.aim_idx == 1, g.aim_idx)
  check("aim_wx/aim_wy on the centre", g.aim_wx == PMX * 256 + 128 and g.aim_wy == PMY * 256 + 128,
        tostring(g.aim_wx) .. "," .. tostring(g.aim_wy))
  check("aim_mx/aim_my on the centre", g.aim_mx == PMX + 0.5 and g.aim_my == PMY + 0.5)
  r = A._charge_leeway({}, g, world, info9636(), PMX, PMY, 9637, true, "x")
  check("next tick: rate limited (wait)", r == "wait", r)
  local g2 = charge_goal(2)                       -- planned TL: clear from the stop, kept
  r = A._charge_leeway({}, g2, world, info9636(), PMX, PMY, 9636, true, "x")
  check("planned TL clear from the stop: kept", r == "ok" and g2.aim_idx == 2, g2.aim_idx)
end

print("charge_leeway, stopped: tests from where the tank stands")
do
  STOP = { SX, SY }                               -- must be ignored when stopped
  local g = charge_goal(5)
  local r = A._charge_leeway({}, g, world, info9636(0), PMX, PMY, 9636, true, "x")
  check("stopped at (115.246,138.203), nothing clear: blocked", r == "blocked", r)
  check("aim left alone", g.aim_idx == 5, g.aim_idx)
end

print("charge_leeway, truly blocked from the stop point")
do
  for x = 110, 118 do WALLS[132 * 256 + x] = true end   -- a wall row across every line
  STOP = { SX, SY }
  local g = charge_goal(1)
  local r = A._charge_leeway({}, g, world, info9636(), PMX, PMY, 9636, true, "x")
  check("at the spot, every aim blocked: blocked (ban)", r == "blocked", r)
  check("aim left alone", g.aim_idx == 1, g.aim_idx)
  g = charge_goal(1)
  r = A._charge_leeway({}, g, world, info9636(), PMX, PMY, 9636, false, "x")
  check("not at the spot yet: go_on (no ban)", r == "go_on", r)
  for x = 110, 118 do WALLS[132 * 256 + x] = nil end
end

-- ── The charge substate itself ───────────────────────────────────────────
print("update_attack_substate, charge, t=9636")
local function run_charge(knob)
  local saved = C.CHARGE_LEEWAY
  C.CHARGE_LEEWAY = knob
  STOP = { SX, SY }
  local state = { tick = 9636, banned_pill_angles = {}, player_number = 2 }
  local g = charge_goal(5)
  local info = info9636()
  info.tank_angle = 248.5                         -- live line hits pill #3
  local ok, err = pcall(A.update_attack_substate, g, state, mkworld(), info)
  C.CHARGE_LEEWAY = saved
  return ok, err, g, state
end
do
  local ok, err, g, state = run_charge(true)
  check("knob on: no error", ok, err)
  check("knob on: still charging", g.substate == "charge", g.substate)
  check("knob on: centre aim", g.aim_idx == 1, g.aim_idx)
  local banned = state.banned_pill_angles and next(state.banned_pill_angles)
  check("knob on: no angle ban", banned == nil, banned)
  ok, err, g, state = run_charge(false)
  check("knob off: no error", ok, err)
  check("knob off: replans as before", g.substate == "plan_position", g.substate)
  check("knob off: aim left alone", g.aim_idx == 5, g.aim_idx)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
