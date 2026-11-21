-- =========================================================================
-- test_wsim_fixes.lua -- standalone tests for the world sim knob fixes
-- push in cworldsim.lua (WSIM_DRIVE_WHILE_SLIDING, WSIM_HIT_FULL_DIST,
-- WSIM_HIT_FAR_FRAC) and the capture exit leg path (WSIM_CAPTURE_EXIT_MAX).
-- The C side is tested in WinBoloUnitTests (wsim_fix_drive_while_sliding,
-- wsim_fix_hit_damage, wsim_fix_capture_exit: the C stop rule).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_wsim_fixes.lua
-- or with any lua on PATH.
--
-- 1. push_fixes sends the knob values once, again only on a change, and
--    does nothing on an exe without wsim_set_fixes.
-- 2. The knob defaults and the KEEL / MAST / R221 old values.
-- 3. capture_exit_path: onto the pill square, then the incoming squares in
--    reverse, at most max_exit, back to the path start, no made-up squares,
--    the 250 waypoint cap.
-- 4. damage_cost: exit leg damage at WSIM_CAPTURE_EXIT_DMG_WEIGHT.
-- =========================================================================

package.path = "./?.lua;" .. package.path

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

local fixes_calls = {}
_G.wsim_set_fixes = function(t) fixes_calls[#fixes_calls + 1] = t end

local C = require("constants")
local wsim = require("cworldsim")

print("1. push_fixes")
do
  wsim.reset_fixes()
  fixes_calls = {}
  wsim.push_fixes()
  wsim.push_fixes()
  check("one call for the same values", #fixes_calls == 1, #fixes_calls)
  local f = fixes_calls[1]
  check("the values are the knobs", f.drive_while_sliding == true and f.hit_full_dist == 2
        and f.hit_far_frac == 0.4, f and tostring(f.hit_far_frac))
  local old = C.WSIM_HIT_FULL_DIST
  C.WSIM_HIT_FULL_DIST = 0
  wsim.push_fixes()
  check("a change sends again", #fixes_calls == 2 and fixes_calls[2].hit_full_dist == 0, #fixes_calls)
  C.WSIM_HIT_FULL_DIST = old
  local keep = _G.wsim_set_fixes
  _G.wsim_set_fixes = nil
  wsim.reset_fixes()
  local ok = pcall(wsim.push_fixes)
  check("an exe without wsim_set_fixes is fine", ok, ok)
  _G.wsim_set_fixes = keep
end

print("2. knob defaults and old values")
do
  check("defaults", C.WSIM_DRIVE_WHILE_SLIDING == true and C.WSIM_HIT_FULL_DIST == 2
        and C.WSIM_HIT_FAR_FRAC == 0.4 and C.WSIM_CAPTURE_EXIT_MAX == 10
        and C.WSIM_CAPTURE_EXIT_DMG_WEIGHT == 0.5, "")
  local f = io.open("constants.lua", "r")
  local src = f:read("*a")
  f:close()
  local _, n_off = src:gsub("WSIM_DRIVE_WHILE_SLIDING%s*=%s*false", "")
  local _, n_full = src:gsub("WSIM_HIT_FULL_DIST%s*=%s*0[,%s]", "")
  local _, n_exit = src:gsub("WSIM_CAPTURE_EXIT_MAX%s*=%s*0[,%s]", "")
  check("KEEL and MAST keep the old sim", n_off == 2 and n_full == 2 and n_exit == 2,
        n_off .. " " .. n_full .. " " .. n_exit)
  local g = io.open("test_preset_mast.lua", "r")
  local t = g:read("*a")
  g:close()
  check("R221 keeps the old sim", t:find("WSIM_DRIVE_WHILE_SLIDING = false", 1, true)
        and t:find("WSIM_HIT_FULL_DIST = 0", 1, true)
        and t:find("WSIM_CAPTURE_EXIT_MAX = 0", 1, true), "")
end

local function fmt(path)
  local t = {}
  for i = 1, #path, 2 do t[#t + 1] = "(" .. path[i] .. "," .. path[i + 1] .. ")" end
  return table.concat(t, " ")
end

-- West along y=100: squares from..to.
local function west(from, to)
  local p = {}
  for x = from, to, -1 do p[#p + 1] = x; p[#p + 1] = 100 end
  return p
end

print("3. capture_exit_path")
do
  -- Goal pool: the path ends on the pill (100,100); the tank is on (121,100).
  local out, start, n = wsim.capture_exit_path(west(120, 100), 121, 100, 100, 100, 10)
  check("incoming = 21 waypoints, the pill last", start == 21 and out[41] == 100 and out[42] == 100, fmt(out))
  check("cap 10 exit squares", n == 10 and #out / 2 == 31, n .. " " .. #out / 2)
  check("the exit is the incoming path in reverse",
        out[43] == 101 and out[45] == 102 and out[61] == 110, fmt(out))

  -- CAPTURE_ROUTE: the path ends next to the pill, at (101,100).
  out, start, n = wsim.capture_exit_path(west(120, 101), 121, 100, 100, 100, 10)
  check("the pill square is added", start == 21 and out[41] == 100 and out[42] == 100, fmt(out))
  check("then 10 exit squares", n == 10 and out[43] == 101 and out[61] == 110, fmt(out))

  -- A short path: the exit ends on the tank square (103,100).
  out, start, n = wsim.capture_exit_path(west(102, 101), 103, 100, 100, 100, 10)
  check("short: the pill, then (101) (102) and the tank square (103)",
        start == 3 and n == 3 and out[#out - 1] == 103, fmt(out))

  -- The tank square is only added when it is next to the path start.
  out, start, n = wsim.capture_exit_path(west(102, 101), 110, 100, 100, 100, 10)
  check("no made-up square back to a far tank square", n == 2 and out[#out - 1] == 102, fmt(out))

  -- A path that ends far from the pill gets no pill square and no exit.
  out, start, n = wsim.capture_exit_path(west(120, 105), 121, 100, 100, 100, 10)
  check("not next to the pill: no exit", n == 0 and #out == 32 and start == 16, n)

  -- An empty path.
  out, start, n = wsim.capture_exit_path({}, 121, 100, 100, 100, 10)
  check("an empty path: no exit", n == 0 and #out == 0 and start == 0, n)

  -- The 250 waypoint cap.
  local long = {}
  for i = 245, 1, -1 do long[#long + 1] = 10 + i; long[#long + 1] = 100 end
  out, start, n = wsim.capture_exit_path(long, 256, 100, 10, 100, 10)
  check("245 + the pill + 4 exit squares = 250", #out / 2 == 250 and start == 246 and n == 4,
        #out / 2 .. " " .. start .. " " .. n)
  local full = {}
  for i = 250, 1, -1 do full[#full + 1] = 10 + i; full[#full + 1] = 100 end
  out, start, n = wsim.capture_exit_path(full, 261, 100, 10, 100, 10)
  check("a full path: no pill square, no exit", #out / 2 == 250 and n == 0, #out / 2 .. " " .. n)
  local exact = {}
  for i = 249, 0, -1 do exact[#exact + 1] = 10 + i; exact[#exact + 1] = 100 end
  out, start, n = wsim.capture_exit_path(exact, 260, 100, 10, 100, 10)
  check("a full path that ends on the pill: no room for an exit", #out / 2 == 250 and n == 0
        and start == 250, #out / 2 .. " " .. n)
end

print("4. damage_cost")
do
  local W = C.WSIM_DAMAGE_COST_WEIGHT
  check("no exit damage = the old cost", wsim.damage_cost(15, 0, W, 0.5) == 15 * W, wsim.damage_cost(15, 0, W, 0.5))
  check("10 incoming + 10 exit at 0.5", wsim.damage_cost(20, 10, W, 0.5) == 10 * W + 10 * W * 0.5,
        wsim.damage_cost(20, 10, W, 0.5))
  check("all on the exit at 0.5 = half", wsim.damage_cost(8, 8, W, 0.5) == 4 * W, wsim.damage_cost(8, 8, W, 0.5))
  check("weight 1 = the plain cost", wsim.damage_cost(20, 10, W, 1) == 20 * W, wsim.damage_cost(20, 10, W, 1))
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
