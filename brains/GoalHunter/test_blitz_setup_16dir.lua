-- =========================================================================
-- test_blitz_setup_16dir.lua — standalone unit tests (2026-11-03) for the
-- blitz soldier's setup point on the 16-direction travel line
-- (knob C.BLITZ_SETUP_16DIR, attack.lua M.blitz_setup_16dir).
--
-- The recorded game (20261103_192602 part 2, player 2, t=9555-9636): the
-- soldier waited at its radial setup point (115.18,140.00) for neutral pill
-- #2 (113,130). The charge faced the aim point (113.5,130.5), facing 248.x,
-- which the engine rounds to north, so the tank moved north only and ended
-- 0.5 tile right of the pulled spot (114.72,137.39).
--
-- Checked here:
--   * the recorded geometry gives a setup point whose 16-direction travel
--     (engine rule, facing the aim point every step) stops on the spot,
--     within 0.1 tile, and the setup stays on the old setup ring
--   * knob off: the old radial point, exactly
--   * a bearing near a 16-direction sector edge falls back to the old point
--   * a sweep of spot bearings: every accepted point ends on the spot
--   * fallbacks: building on the drive line, water setup tile, more threat
-- No engine: map tiles and threat are stand-ins.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_blitz_setup_16dir.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local LOG = {}
package.loaded["print2"] = setmetatable({}, {
  __call  = function(_, s) LOG[#LOG + 1] = tostring(s) end,
  __index = function() return function() end end,
})
_G.print2 = _G.print2 or function() end

local C  = require("constants")
local U  = require("util")
local TH = require("threat")
local A  = require("attack")

-- Map stand-in: road everywhere, with a few tiles set per test.
local TILES = {}
U.ttype      = function(mx, my) return TILES[my * 256 + mx] or 4 end
U.ttype_peek = U.ttype
local THREAT = {}
TH.at = function(mx, my) return THREAT[my * 256 + mx] or 0 end

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

-- Engine travel: each step the tank faces the aim point and moves along
-- utilGet16Dir(facing); stop when within stop_r of the pill centre (the
-- non-PPT charge stops on the pulled spot's distance from the pill).
local function travel(sx, sy, ax, ay, pcx, pcy, stop_r)
  local x, y = sx, sy
  for _ = 1, 4000 do
    local dx, dy = x - pcx, y - pcy
    if dx * dx + dy * dy <= stop_r * stop_r then break end
    local d16 = A._dir16(U.aim_at_f(x, y, ax, ay))
    local rad = d16 * C.TWO_PI / 256
    x, y = x + math.sin(rad) * 0.005, y - math.cos(rad) * 0.005
  end
  return x, y
end
local function dist(ax, ay, bx, by) return math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2) end

local PMX, PMY = 113, 130
local PCX, PCY = PMX + 0.5, PMY + 0.5
local SETUP_R = C.ATTACK_PILL_STANDOFF + C.ATTACK_APPROACH_OFFSET
local R2 = C.ATTACK_PILL_STANDOFF_CHARGE

local function commit(efx, efy, aim_fx, aim_fy, pmx, pmy)
  pmx, pmy = pmx or PMX, pmy or PMY
  local goal = { kind = "attack_pill", target_id = 2, mx = pmx, my = pmy, substate = "plan_position" }
  local state = {
    tick = 9077,
    squad_blitz_engage_mx = math.floor(efx), squad_blitz_engage_my = math.floor(efy),
    squad_blitz_engage_fx = efx, squad_blitz_engage_fy = efy, squad_blitz_engage_deg = 170,
    squad_blitz_aim_fx = aim_fx, squad_blitz_aim_fy = aim_fy,
  }
  LOG = {}
  local ok = A._blitz_commit_negotiated(goal, state, {}, {}, pmx, pmy)
  return ok, goal
end
local function last_log()
  for i = #LOG, 1, -1 do if LOG[i]:find("BLITZ_SETUP_16DIR", 1, true) then return LOG[i] end end
  return ""
end
-- The opt copy strips print2, so the log line exists only in the root copy.
local HAVE_LOG = (function()
  local f = io.open("attack.lua", "r"); if not f then return false end
  local src = f:read("*a"); f:close()
  return src:find('print2(string.format("BLITZ_SETUP_16DIR', 1, true) ~= nil
end)()
-- The fallback reason for a committed goal, from the function itself.
local function reason(g, pcx, pcy)
  pcx, pcy = pcx or PCX, pcy or PCY
  local dx, dy = g.standoff_fx - pcx, g.standoff_fy - pcy
  local l = math.sqrt(dx * dx + dy * dy)
  local nfx, _, r = A.blitz_setup_16dir({}, g, pcx, pcy, SETUP_R,
    pcx + dx / l * SETUP_R, pcy + dy / l * SETUP_R)
  return r.reason or "none", r, nfx
end

-- Recorded engage spot and aim (BLITZ_COMMIT_STANDOFF t=9077).
local EFX, EFY = 114.78499651474, 137.78757737229
local AFX, AFY = 113.5, 130.5

print("recorded geometry, knob off")
C.BLITZ_SETUP_16DIR = false
local ok0, g0 = commit(EFX, EFY, AFX, AFY)
check("commit ok", ok0)
local u = { (EFX - PCX), (EFY - PCY) }
local ul = math.sqrt(u[1] ^ 2 + u[2] ^ 2)
local ofx, ofy = PCX + u[1] / ul * SETUP_R, PCY + u[2] / ul * SETUP_R
check("knob off: old radial setup x", g0.approach_fx == ofx, g0.approach_fx)
check("knob off: old radial setup y", g0.approach_fy == ofy, g0.approach_fy)
check("knob off: no BLITZ_SETUP_16DIR line", last_log() == "", last_log())  -- empty in opt too
local spx, spy = PCX + u[1] / ul * R2, PCY + u[2] / ul * R2
local tx0, ty0 = travel(g0.approach_fx, g0.approach_fy, AFX, AFY, PCX, PCY, R2)
print(string.format("  info old setup (%.2f,%.2f) travel ends (%.2f,%.2f), spot (%.2f,%.2f), miss %.2f",
  g0.approach_fx, g0.approach_fy, tx0, ty0, spx, spy, dist(tx0, ty0, spx, spy)))
check("knob off: old travel misses the spot by > 0.1 (the recorded fault)",
  dist(tx0, ty0, spx, spy) > 0.1, dist(tx0, ty0, spx, spy))

print("recorded geometry, knob on")
C.BLITZ_SETUP_16DIR = true
local ok1, g1 = commit(EFX, EFY, AFX, AFY)
check("commit ok", ok1)
local R1, rec1 = reason(g1)
check("reason none", R1 == "none", R1)
check("D = 0 (north)", rec1.D == 0, rec1.D)
if HAVE_LOG then
  local L = last_log()
  print("  log: " .. L)
  check("log line has fallback=none and D=0", L:find("fallback=none", 1, true) ~= nil
    and L:find(" D=0 ", 1, true) ~= nil, L)
end
local tx1, ty1 = travel(g1.approach_fx, g1.approach_fy, AFX, AFY, PCX, PCY, R2)
print(string.format("  info new setup (%.2f,%.2f) travel ends (%.2f,%.2f), miss %.3f",
  g1.approach_fx, g1.approach_fy, tx1, ty1, dist(tx1, ty1, spx, spy)))
check("16-dir travel ends on the spot within 0.1", dist(tx1, ty1, spx, spy) <= 0.1, dist(tx1, ty1, spx, spy))
check("setup stays on the old setup ring", math.abs(dist(g1.approach_fx, g1.approach_fy, PCX, PCY) - SETUP_R) < 1e-6,
  dist(g1.approach_fx, g1.approach_fy, PCX, PCY))
check("setup x equals spot x (north line)", math.abs(g1.approach_fx - spx) < 1e-6, g1.approach_fx)
check("approach_mx/my follow the float", g1.approach_mx == math.floor(g1.approach_fx)
  and g1.approach_my == math.floor(g1.approach_fy), tostring(g1.approach_mx) .. "," .. tostring(g1.approach_my))

-- A spot whose bearing to the aim sits 0.3 brad from the 8-brad sector edge
-- (inside C.BLITZ_SETUP_16DIR_EDGE_BRAD, 0.5).
local function spot_for_bearing(brg_to_pill_brads, r)
  -- bearing FROM the spot TO the pill centre = brg; spot = pc - r*u(brg)
  local rad = brg_to_pill_brads * C.TWO_PI / 256
  return PCX - math.sin(rad) * r, PCY + math.cos(rad) * r
end

print("bearing near a sector edge")
do
  -- engage spot on the 7.4 ring, aim = pill centre, bearing 8.3 brads
  local sx, sy = spot_for_bearing(8.3, C.ATTACK_PILL_STANDOFF)
  local okb, gb = commit(sx, sy, PCX, PCY)
  local Rb, recb = reason(gb)
  print(string.format("  info bearing=%.2f D=%d reason=%s", recb.bearing, recb.D, Rb))
  check("near edge: fallback=near_boundary", Rb == "near_boundary", Rb)
  if HAVE_LOG then
    check("near edge: log line fallback=near_boundary", last_log():find("fallback=near_boundary", 1, true) ~= nil, last_log())
  end
  local u2x, u2y = sx - PCX, sy - PCY
  local l2 = math.sqrt(u2x * u2x + u2y * u2y)
  check("near edge: old radial point kept",
    math.abs(gb.approach_fx - (PCX + u2x / l2 * SETUP_R)) < 1e-9
    and math.abs(gb.approach_fy - (PCY + u2y / l2 * SETUP_R)) < 1e-9,
    string.format("%.3f,%.3f", gb.approach_fx, gb.approach_fy))
  -- 1 brad from the edge: accepted and on the spot
  local sx2, sy2 = spot_for_bearing(9, C.ATTACK_PILL_STANDOFF)
  local _, g2 = commit(sx2, sy2, PCX, PCY)
  local R2r = reason(g2)
  check("1 brad from edge: accepted", R2r == "none", R2r)
  local fx2, fy2 = spot_for_bearing(9, R2)
  local ex, ey = travel(g2.approach_fx, g2.approach_fy, PCX, PCY, PCX, PCY, R2)
  check("1 brad from edge: travel ends on the spot within 0.1", dist(ex, ey, fx2, fy2) <= 0.1, dist(ex, ey, fx2, fy2))
end

print("edge margin knob")
do
  check("BLITZ_SETUP_16DIR_EDGE_BRAD default 0.5", C.BLITZ_SETUP_16DIR_EDGE_BRAD == 0.5, C.BLITZ_SETUP_16DIR_EDGE_BRAD)
  check("PRESETS.keel BLITZ_SETUP_16DIR_EDGE_BRAD = 0.5",
    C.PRESETS.keel.BLITZ_SETUP_16DIR_EDGE_BRAD == 0.5, C.PRESETS.keel.BLITZ_SETUP_16DIR_EDGE_BRAD)
  local sx, sy = spot_for_bearing(8.3, C.ATTACK_PILL_STANDOFF)
  local saved = C.BLITZ_SETUP_16DIR_EDGE_BRAD
  C.BLITZ_SETUP_16DIR_EDGE_BRAD = 0.2
  local _, ga = commit(sx, sy, PCX, PCY)
  local Ra = reason(ga)
  check("edge 0.2: 0.3 brad from the edge is not near_boundary", Ra ~= "near_boundary", Ra)
  C.BLITZ_SETUP_16DIR_EDGE_BRAD = 2
  local sx2, sy2 = spot_for_bearing(9, C.ATTACK_PILL_STANDOFF)
  local _, gb = commit(sx2, sy2, PCX, PCY)
  local Rb = reason(gb)
  check("edge 2: 1 brad from the edge is near_boundary", Rb == "near_boundary", Rb)
  C.BLITZ_SETUP_16DIR_EDGE_BRAD = saved
end

print("sweep of spot bearings, aim = recorded corner offset")
do
  local n_ok, n_fb, n_bad, worst = 0, 0, 0, 0
  for b10 = 0, 2559, 7 do
    local brg = b10 / 10
    local sx, sy = spot_for_bearing(brg, C.ATTACK_PILL_STANDOFF)
    local ax, ay = PCX - 0.3, PCY + 0.3
    local _, g = commit(sx, sy, ax, ay)
    if reason(g) == "none" then
      n_ok = n_ok + 1
      local ux2, uy2 = sx - PCX, sy - PCY
      local l2 = math.sqrt(ux2 * ux2 + uy2 * uy2)
      local fx, fy = PCX + ux2 / l2 * R2, PCY + uy2 / l2 * R2
      local ex, ey = travel(g.approach_fx, g.approach_fy, ax, ay, PCX, PCY, R2)
      local m = dist(ex, ey, fx, fy)
      if m > worst then worst = m end
      if m > 0.1 then n_bad = n_bad + 1 end
    else
      n_fb = n_fb + 1
    end
  end
  print(string.format("  info accepted=%d fallback=%d off_spot=%d worst=%.3f", n_ok, n_fb, n_bad, worst))
  check("sweep: every accepted setup ends on its spot within 0.1", n_bad == 0, n_bad)
  check("sweep: most bearings accepted", n_ok > 3 * n_fb, n_ok .. "/" .. n_fb)
end

print("fallbacks")
do
  -- First find the 16-dir setup the recorded case picks.
  local _, gref = commit(EFX, EFY, AFX, AFY)
  local nmx, nmy = math.floor(gref.approach_fx), math.floor(gref.approach_fy)

  -- building on the drive line (one tile toward the spot, same column)
  TILES[(nmy - 1) * 256 + nmx] = C.T_BUILDING
  local _, gb = commit(EFX, EFY, AFX, AFY)
  check("building on drive line: fallback=drive_blocked", reason(gb) == "drive_blocked", reason(gb))
  check("building on drive line: old point kept", gb.approach_fx == ofx and gb.approach_fy == ofy,
    string.format("%.3f,%.3f", gb.approach_fx, gb.approach_fy))
  TILES = {}

  -- deep water on the new setup tile
  TILES[nmy * 256 + nmx] = C.T_DEEPSEA
  local _, gw = commit(EFX, EFY, AFX, AFY)
  check("water setup tile: fallback=not_passable", reason(gw) == "not_passable", reason(gw))
  check("water setup tile: old point kept", gw.approach_fx == ofx and gw.approach_fy == ofy,
    string.format("%.3f,%.3f", gw.approach_fx, gw.approach_fy))
  TILES = {}

  -- more threat at the new setup tile than at the old one
  THREAT[nmy * 256 + nmx] = 40
  THREAT[math.floor(ofy) * 256 + math.floor(ofx)] = 10
  local _, gt = commit(EFX, EFY, AFX, AFY)
  check("more threat: fallback=more_danger", reason(gt) == "more_danger", reason(gt))
  check("more threat: old point kept", gt.approach_fx == ofx and gt.approach_fy == ofy,
    string.format("%.3f,%.3f", gt.approach_fx, gt.approach_fy))
  -- equal threat is fine
  THREAT[math.floor(ofy) * 256 + math.floor(ofx)] = 40
  local _, ge = commit(EFX, EFY, AFX, AFY)
  check("equal threat: accepted", reason(ge) == "none", reason(ge))
  THREAT = {}

  -- already pulled (a second charge on the same goal does not pull again)
  local goal = { kind = "attack_pill", target_id = 2, mx = PMX, my = PMY,
                 standoff_fx = EFX, standoff_fy = EFY, aim_wx = AFX * 256, aim_wy = AFY * 256,
                 _charge_pulled = true }
  local nfx, nfy, r = A.blitz_setup_16dir({}, goal, PCX, PCY, SETUP_R, ofx, ofy)
  check("already pulled: spot is the unpulled standoff", r.spot_fx == EFX and r.spot_fy == EFY,
    tostring(r.spot_fx) .. "," .. tostring(r.spot_fy))
  local ex, ey = travel(nfx, nfy, AFX, AFY, PCX, PCY, dist(EFX, EFY, PCX, PCY))
  check("already pulled: travel ends on the standoff within 0.1", dist(ex, ey, EFX, EFY) <= 0.1, dist(ex, ey, EFX, EFY))
end

print("dir16 matches utilGet16Dir")
check("8 -> 0", A._dir16(8) == 0, A._dir16(8))
check("8.01 -> 16", A._dir16(8.01) == 16, A._dir16(8.01))
check("248 -> 240", A._dir16(248) == 240, A._dir16(248))
check("248.4 -> 0", A._dir16(248.4) == 0, A._dir16(248.4))
check("-7.3 -> 0", A._dir16(-7.3) == 0, A._dir16(-7.3))
check("100 -> 96", A._dir16(100) == 96, A._dir16(100))

C.BLITZ_SETUP_16DIR = true
print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
