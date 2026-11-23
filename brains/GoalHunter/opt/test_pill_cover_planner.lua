-- =========================================================================
-- test_pill_cover_planner.lua — standalone unit tests (2026-11-03) for the
-- shield planner knob C.PILL_COVER_BY_HEALTH (attack_shield.lua):
--   * the Lua scorer (M.scan without the C module): a friendly pill on a
--     return-fire square is an existing blocker only when it is worth a wall
--     or more (floor(health / 5)); any other pill square is skipped; the
--     aim's cover counts 5 shots per wall the pill gives.
--   * knob off: exactly the old lists and scores.
--   * the C scan call: args 25/26 (pill health per pill_table entry, dead
--     pill squares) only with the knob on.
-- No engine: terrain is all road and the shell simulator is a stand-in.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pill_cover_planner.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local C   = require("constants")
local U   = require("util")
local cpf = require("cpathfinder")

_G.print2 = _G.print2 or function() end
U.ttype      = function() return 4 end       -- road everywhere
U.ttype_peek = function() return 4 end
U.base_tile_stops_tank_shell = function() return false end
cpf.SHOT_TANK = cpf.SHOT_TANK or 0
cpf.SHOT_PILL = cpf.SHOT_PILL or 1
cpf.lgm_travel_ticks_map = function() return 10 end

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

print("knobs")
local keel = C.PRESETS and C.PRESETS.keel or {}
check("PILL_COVER_BY_HEALTH default true", C.PILL_COVER_BY_HEALTH == true)
check("PRESETS.keel PILL_COVER_BY_HEALTH = false", keel.PILL_COVER_BY_HEALTH == false)

-- ── Shell stand-in ───────────────────────────────────────────────────────
-- Target pill at (100,100); the firing spots are east of it. The pill's
-- return fire runs along row 100 to the spot; our own shell runs along
-- row 99 (so the return-fire squares are not on our own path) and ends on
-- the pill square.
local PMX, PMY = 100, 100
local function line(ox, oy, tx, ty, row_shift)
  local out, seen = {}, {}
  local ax, ay, bx, by = ox / 256, oy / 256, tx / 256, ty / 256
  for i = 0, 64 do
    local f = i / 64
    local mx = math.floor(ax + (bx - ax) * f)
    local my = math.floor(ay + (by - ay) * f) + row_shift
    local k = my * 256 + mx
    if not seen[k] then seen[k] = true; out[#out + 1] = { mx = mx, my = my } end
  end
  return out
end
cpf.simulate_shot = function(ox, oy, tx, ty, kind)
  if kind == cpf.SHOT_PILL then
    return line(ox, oy, tx, ty, 0)
  end
  local t = line(ox, oy, tx, ty, -1)
  for i = #t, 1, -1 do if t[i].mx == PMX then table.remove(t, i) end end
  t[#t + 1] = { mx = PMX, my = PMY }
  return t
end

-- A world with the target pill and one other pill on (102,100).
local function make_world(owner, health)
  local target = { id = 1, mx = PMX, my = PMY, owner = "hostile", health = 15 }
  local other  = { id = 2, mx = 102, my = 100, owner = owner, health = health }
  return {
    pills   = { [1] = target, [2] = other },
    pill_at = { [PMY * 256 + PMX] = { { id = 1, pill = target } },
                [100 * 256 + 102] = { { id = 2, pill = other } } },
    bases = {},
  }, target
end

-- Fresh attack_shield without the C module (the Lua scorer).
_G.gh_shield = nil
package.loaded["attack_shield"] = nil
local S = require("attack_shield")

-- Scan and report, for the standoff candidate's best scoring aim: how
-- (102,100) was classed ("actual", "potential" or "none") and that aim's
-- score.
local function scan(owner, health, by_hp, no_builder)
  C.PILL_COVER_BY_HEALTH = by_hp
  local world, target = make_world(owner, health)
  local r = S.scan(target, world, 106, 100, 0, 106.5, 100.5, 6, no_builder, 40, 1, 0.5, 0)
  local cand = r.candidates and r.candidates[1]
  local cls, best = "none", nil
  if cand and cand.aims then
    for _, a in pairs(cand.aims) do
      if not a.blocked and (best == nil or (a.score or 0) > (best.score or 0)) then best = a end
    end
  end
  if best then
    for _, b in ipairs(best.blockers or {}) do
      if b.mx == 102 and b.my == 100 then cls = "actual" end
    end
    for _, b in ipairs(best.potential_blockers or {}) do
      if b.mx == 102 and b.my == 100 then cls = "potential" end
    end
  end
  return cls, best and best.score, best
end

print("Lua scorer: how a pill square is classed")
do
  local cls_off, cls_on
  cls_off = scan("friendly", 15, false, false); cls_on = scan("friendly", 15, true, false)
  check("friendly 15: actual (off)", cls_off == "actual", cls_off)
  check("friendly 15: actual (on)", cls_on == "actual", cls_on)
  cls_off = scan("friendly", 7, false, false); cls_on = scan("friendly", 7, true, false)
  check("friendly 7: actual (off)", cls_off == "actual", cls_off)
  check("friendly 7 (1 wall): actual (on)", cls_on == "actual", cls_on)
  cls_off = scan("friendly", 3, false, false); cls_on = scan("friendly", 3, true, false)
  check("friendly 3: actual (off, as before)", cls_off == "actual", cls_off)
  check("friendly 3 (0 walls): none (on)", cls_on == "none", cls_on)
  cls_off = scan("hostile", 15, false, false); cls_on = scan("hostile", 15, true, false)
  check("enemy 15: potential, a square to build on (off, as before)", cls_off == "potential", cls_off)
  check("enemy 15: none (on)", cls_on == "none", cls_on)
  cls_off = scan("neutral", 15, false, false); cls_on = scan("neutral", 15, true, false)
  check("neutral 15: potential (off, as before)", cls_off == "potential", cls_off)
  check("neutral 15: none (on)", cls_on == "none", cls_on)
  cls_off = scan("friendly", 0, false, false); cls_on = scan("friendly", 0, true, false)
  check("dead friendly: actual (off, as before)", cls_off == "actual", cls_off)
  check("dead friendly: none (on)", cls_on == "none", cls_on)
end

print("Lua scorer: aim score (no builder, so the pill is the only cover)")
do
  local _, s15_off = scan("friendly", 15, false, true)
  local _, s10_off = scan("friendly", 10, false, true)
  local _, s7_off  = scan("friendly", 7,  false, true)
  local _, s15_on  = scan("friendly", 15, true, true)
  local _, s10_on  = scan("friendly", 10, true, true)
  local _, s7_on   = scan("friendly", 7,  true, true)
  check("off: 10-health pill scores as a 15-health pill", s10_off == s15_off,
        tostring(s10_off) .. " vs " .. tostring(s15_off))
  check("off: 7-health pill scores as a 15-health pill", s7_off == s15_off)
  check("on: 15-health pill scores as off (3 walls = 15 shots)", s15_on == s15_off,
        tostring(s15_on) .. " vs " .. tostring(s15_off))
  check("on: 10-health pill is 5 shots less (2 walls)", s10_on == s15_on - 5,
        tostring(s10_on) .. " vs " .. tostring(s15_on))
  check("on: 7-health pill is 10 shots less (1 wall)", s7_on == s15_on - 10,
        tostring(s7_on) .. " vs " .. tostring(s15_on))
end

-- ── C scan call: args 25/26 ──────────────────────────────────────────────
print("C scan call")
do
  local seen = {}
  _G.gh_shield = {
    load = function() return true end,
    pill_hit = {},
    scan_c = function(...)
      seen.n = select("#", ...)
      seen.a9 = select(9, ...)
      seen.a25 = select(25, ...)
      seen.a26 = select(26, ...)
      return { 0 }
    end,
  }
  package.loaded["attack_shield"] = nil
  local SC = require("attack_shield")
  SC.load_stamp_bin("x")
  _G.gh_shield = nil

  local function run(by_hp)
    for k in pairs(seen) do seen[k] = nil end
    C.PILL_COVER_BY_HEALTH = by_hp
    local world, target = make_world("friendly", 10)
    local dead = { id = 3, mx = 103, my = 101, owner = "neutral", health = 0 }
    world.pills[3] = dead
    world.pill_at[101 * 256 + 103] = { { id = 3, pill = dead } }
    SC.scan(target, world, 106, 100, 0, 106.5, 100.5, 6, false, 40, 1, 0.5, 0)
  end

  run(false)
  check("off: scan_c called", seen.n ~= nil)
  check("off: arg 25 nil", seen.a25 == nil)
  check("off: arg 26 nil", seen.a26 == nil)

  run(true)
  check("on: arg 25 is a table", type(seen.a25) == "table")
  local pt, hp = seen.a9 or {}, seen.a25 or {}
  check("on: one health per pill_table entry", #hp * 3 == #pt, #hp .. " vs " .. #pt)
  local ok = true
  for k = 1, #hp do
    local mx, my, own = pt[3 * k - 2], pt[3 * k - 1], pt[3 * k]
    if mx == 102 and my == 100 then ok = ok and own == 1 and hp[k] == 10 end
    if mx == PMX and my == PMY then ok = ok and own == 0 and hp[k] == 15 end
  end
  check("on: health in the same order as pill_table", ok)
  check("on: arg 26 holds the dead pill square",
        type(seen.a26) == "table" and seen.a26[1] == 103 and seen.a26[2] == 101 and #seen.a26 == 2)
end

C.PILL_COVER_BY_HEALTH = true
print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
