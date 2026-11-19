-- =========================================================================
-- test_route_keep.lua — standalone unit tests for the route keep (knobs
-- C.PF_REROUTE_KEEP_MARGIN and C.PF_REROUTE_KEEP_FRAC): cpathfinder's
-- M.route_keep decides, when a new Dijkstra slate wants another first step,
-- whether the tank keeps its old step. The C side (the old route priced
-- again, cpf_dijkstra_keep_check) is stubbed here; its numbers are tested in
-- tests/unit/test_pf_route_keep.c. No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_route_keep.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

_G.print2 = _G.print2 or function() end

local C   = require("constants")
local cpf = require("cpathfinder")

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
local function xy(x, y) return string.format("(%s,%s)", tostring(x), tostring(y)) end

-- ── Engine stand-ins ─────────────────────────────────────────────────────
-- ANS: the slate that gave the last next step. KC: what keep_check returns.
local ANS = { slate = 0, started = 100, version = 1, fresh = true }
local KC  = { rc = 0, g_new = 100, g_keep = 108 }
local calls = { answer = 0, save = 0, check = 0 }
local OVERLAY = {}
_G.cpf_dijkstra_answer = function()
  calls.answer = calls.answer + 1
  if not ANS.slate then return nil end
  return ANS.slate, ANS.started, ANS.version, ANS.fresh, 1
end
_G.cpf_dijkstra_route_save = function() calls.save = calls.save + 1; return 10 end
_G.cpf_dijkstra_keep_check = function()
  calls.check = calls.check + 1
  return KC.rc, KC.g_new, KC.g_keep
end
_G.cpf_get_overlay = function(x, y) return OVERLAY[y * 256 + x] or 0 end

local DX, DY = 130, 100
local function new_slate(started, fresh)
  ANS.slate, ANS.started, ANS.version = 0, started, (ANS.version or 0) + 1
  ANS.fresh = (fresh ~= false)
end
-- Start a route: tank on (100,100), old step (101,100) east, slate 1.
local function begin()
  cpf.route_keep_reset()
  ANS.slate, ANS.started, ANS.version, ANS.fresh = 0, 100, 1, true
  KC.rc, KC.g_new, KC.g_keep = 0, 100, 108
  local x, y = cpf.route_keep(100, 100, DX, DY, 0, 101, 100, nil)
  return x, y
end
local function rk(sx, sy, nx, ny, obstacles, in_boat, dx, dy)
  return cpf.route_keep(sx, sy, dx or DX, dy or DY, in_boat or 0, nx, ny, obstacles)
end

local M0, F0 = C.PF_REROUTE_KEEP_MARGIN, C.PF_REROUTE_KEEP_FRAC

-- ── 0. Knobs ─────────────────────────────────────────────────────────────
print("knobs")
local keel = C.PRESETS and C.PRESETS.keel or {}
check("PF_REROUTE_KEEP_MARGIN default 12", C.PF_REROUTE_KEEP_MARGIN == 12, C.PF_REROUTE_KEEP_MARGIN)
check("PF_REROUTE_KEEP_FRAC default 0.06", C.PF_REROUTE_KEEP_FRAC == 0.06, C.PF_REROUTE_KEEP_FRAC)
check("PF_REROUTE_KEEP_MAX_RESTARTS default 3", C.PF_REROUTE_KEEP_MAX_RESTARTS == 3)
check("PRESETS.keel PF_REROUTE_KEEP_MARGIN = 0", keel.PF_REROUTE_KEEP_MARGIN == 0)
check("PRESETS.keel PF_REROUTE_KEEP_FRAC = 0", keel.PF_REROUTE_KEEP_FRAC == 0)

-- ── 1. Off = the old behaviour, no C calls ──────────────────────────────
print("knobs 0 and 0")
C.PF_REROUTE_KEEP_MARGIN, C.PF_REROUTE_KEEP_FRAC = 0, 0
calls.answer, calls.save, calls.check = 0, 0, 0
cpf.route_keep_reset()
local x, y, what = rk(100, 100, 101, 100)
new_slate(110)
x, y, what = rk(100, 100, 100, 101)
check("off: new step taken", x == 100 and y == 101, xy(x, y))
check("off: no check ran", what == nil, what)
check("off: no C call made", calls.answer + calls.save + calls.check == 0,
      calls.answer + calls.save + calls.check)
C.PF_REROUTE_KEEP_MARGIN, C.PF_REROUTE_KEEP_FRAC = M0, F0

-- ── 2. Keep / switch on the margin ──────────────────────────────────────
print("keep or switch")
begin()
x, y, what = rk(100, 100, 101, 100)
check("same slate, same step: no check", what == nil and x == 101, what)
new_slate(110)
KC.g_new, KC.g_keep = 100, 111.9   -- gap 11.9 <= margin max(12, 6)
x, y, what = rk(100, 100, 100, 101)
check("gap 11.9 under margin 12 -> keep old step", what == "keep" and x == 101 and y == 100,
      tostring(what) .. " " .. xy(x, y))
x, y, what = rk(100, 100, 100, 101)
check("same new slate next tick: old step held, no new check",
      x == 101 and y == 100 and what == "keep" and calls.check == 1, xy(x, y))

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 112.1   -- gap 12.1 > 12
x, y, what = rk(100, 100, 100, 101)
check("gap 12.1 over margin 12 -> switch", what == "switch" and x == 100 and y == 101,
      tostring(what) .. " " .. xy(x, y))

begin()
new_slate(110)
KC.g_new, KC.g_keep = 300, 317.9   -- frac: 0.06 * 300 = 18 > 12
x, y, what = rk(100, 100, 100, 101)
check("g_new 300: margin 18, gap 17.9 -> keep", what == "keep", what)
begin()
new_slate(110)
KC.g_new, KC.g_keep = 300, 318.1
x, y, what = rk(100, 100, 100, 101)
check("g_new 300: margin 18, gap 18.1 -> switch", what == "switch", what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 90      -- old route now cheaper
x, y, what = rk(100, 100, 100, 101)
check("old route cheaper -> keep", what == "keep", what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 260     -- an angry pill on the old route
x, y, what = rk(100, 100, 100, 101)
check("angry pill on the old route (+160) -> switch", what == "switch", what)

-- New slate with the SAME first step: no check, adopt it.
begin()
calls.check = 0
new_slate(110)
x, y, what = rk(100, 100, 101, 100)
check("new slate, same first step: no check", what == nil and calls.check == 0, what)

-- A backup copy of the same slate is the same answer (slates 0 -> 1).
begin()
ANS.slate = 1
x, y, what = rk(100, 100, 100, 101)
check("backup copy of the same slate: no check", what == nil and x == 100 and y == 101,
      tostring(what) .. " " .. xy(x, y))

-- ── 3. Skip cases ───────────────────────────────────────────────────────
print("skip cases")
begin()
x, y, what = rk(100, 100, 101, 100, nil, 0, DX + 1, DY)
check("destination changed: start over, no check", what == nil and x == 101, what)
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
x, y, what = rk(100, 100, 100, 101, nil, 0, DX + 1, DY)
check("  and the next change is checked against the new route", what == "keep", what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
x, y, what = rk(100, 100, 100, 101, nil, 1)
check("boat change: start over, new step", what == nil and x == 100 and y == 101, what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
x, y, what = rk(140, 140, 141, 140)
check("jump of more than one square (respawn): start over", what == nil and x == 141, what)

begin()
cpf.route_keep_reset()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
x, y, what = rk(100, 100, 100, 101)
check("route_keep_reset (re-root): no keep", what == nil and x == 100 and y == 101, what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
x, y, what = rk(101, 100, 102, 100)
check("tank on the old step: normal advance, no check", what == nil and x == 102, what)

begin()
new_slate(110, false)
KC.g_new, KC.g_keep = 100, 100
x, y, what = rk(100, 100, 100, 101)
check("older slate answers (newest has not reached the goal) -> skip:backup",
      what == "skip:backup" and x == 100 and y == 101, what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
x, y, what = rk(99, 101, 99, 102)   -- old step (101,100) is two squares away
check("old step not a neighbour -> skip:not_next", what == "skip:not_next" and y == 102, what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
x, y, what = rk(100, 100, 100, 101, { 100 * 256 + 101 })
check("ally tank on the old step (key list) -> skip:ally", what == "skip:ally", what)
begin()
new_slate(110)
x, y, what = rk(100, 100, 100, 101, { [100 * 256 + 101] = true })
check("ally tank on the old step (key set) -> skip:ally", what == "skip:ally", what)

for rc, name in pairs({ [2] = "backup", [3] = "overlay", [4] = "inf", [5] = "off_route" }) do
  begin()
  new_slate(110)
  KC.rc = rc
  x, y, what = rk(100, 100, 100, 101)
  check("C rc " .. rc .. " -> skip:" .. name, what == "skip:" .. name and y == 101, what)
end
KC.rc = 0

-- ── 4. Limits on a keep ─────────────────────────────────────────────────
print("keep limits")
begin()
KC.g_new, KC.g_keep = 100, 100
local seq = {}
for i = 1, 4 do
  new_slate(100 + 10 * i)
  x, y, what = rk(100, 100, 100, 101)
  seq[#seq + 1] = what
end
check("3 new slates in a row keep, the 4th switches (limit)",
      seq[1] == "keep" and seq[2] == "keep" and seq[3] == "keep" and seq[4] == "skip:limit",
      table.concat(seq, ","))

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
rk(100, 100, 100, 101)
x, y, what = rk(101, 100, 102, 101)
check("keep ends when the tank reaches the old step", what == nil and x == 102 and y == 101, what)
new_slate(120)
x, y, what = rk(101, 100, 101, 101)
check("  and the count starts again (next change is checked)", what == "keep", what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
rk(100, 100, 100, 101)
x, y, what = rk(100, 100, 100, 101, { 100 * 256 + 101 })
check("ally moves onto the kept step -> drop:ally, new step", what == "drop:ally" and x == 100 and y == 101, what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
rk(100, 100, 100, 101)
OVERLAY[100 * 256 + 101] = 32767
x, y, what = rk(100, 100, 100, 101)
OVERLAY = {}
check("kept step gets overlay 32767 -> drop:overlay", what == "drop:overlay" and y == 101, what)

begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
rk(100, 100, 100, 101)
x, y, what = rk(99, 101, 99, 102)
check("tank slides away from the kept step -> drop:not_next", what == "drop:not_next" and y == 102, what)

-- ── 5. path_to: an A* answer ends the memory (PF_FLIP hold) ─────────────
print("path_to")
local NS = { x = 100, y = 101 }
_G.cpf_dijkstra_next_step = function() return NS.x, NS.y end
_G.cpf_sea_veto = function() return 0, -1, -1, -1, -1, -1, -1 end
_G.cpf_path_to = function() return 1, 99, 100 end
local DIJ = C.DIJKSTRA_USE_FOR_GOALS
C.DIJKSTRA_USE_FOR_GOALS = true
begin()
new_slate(110)
KC.g_new, KC.g_keep = 100, 100
local st, px, py = cpf.path_to(100, 100, DX, DY, 0, 0, 0, 0, 40, 100, false, nil, nil)
check("path_to returns the kept step", st == 1 and px == 101 and py == 100, xy(px, py))
st, px, py = cpf.path_to(100, 100, DX, DY, 0, 0, 0, 0, 40, 100, true, nil, nil)
check("path_to skip_dijkstra (the PF_FLIP fresh A*) gives the A* step", px == 99, xy(px, py))
st, px, py = cpf.path_to(100, 100, DX, DY, 0, 0, 0, 0, 40, 100, false, nil, nil)
check("after an A* step the memory starts over: slate step, no keep", px == 100 and py == 101, xy(px, py))
C.DIJKSTRA_USE_FOR_GOALS = DIJ

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
