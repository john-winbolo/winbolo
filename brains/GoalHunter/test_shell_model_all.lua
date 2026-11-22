-- =========================================================================
-- test_shell_model_all.lua — standalone unit tests (2026-11-22) for
-- C.SHELL_MODEL_RULES_ALL: every site outside the kill-LGM aim that needs
-- where a tank shell ends uses kill_lgm's rules model (shell_travel /
-- sight_for), not 128 * sight.
--
-- The engine (shells.c shellsAddItem / shellLifeTicks): the shell starts
-- shell_start_add steps of shell_speed out, moves L = max(0, 1 +
-- (int)(shell_life * sight / 2) - shell_start_add) times, and bursts where
-- it stands: shell_speed * (shell_start_add + L) wu out. Classic rules:
-- 128 * sight + 32.
--
--   1. steering.mine_shot (kill_mine and the sea-pill detonate): a shot the
--      burst test lets go bursts on the mine square (a shell sets a mine off
--      only where it ends), and the sight it asks for is the one whose burst
--      is nearest the mine.
--   2. steering.combat_reach_wu (C.TANK_COMBAT_FIRE_MAX_FLIGHT): a lead
--      point at the burst is in reach.
--   3. kill_lgm.shell_travel(sl, kill_lgm.all_on()) (the pill and tank fire
--      ranges in steering.lua and attack.lua, the crosshairs): the engine
--      burst.
--   4. danger.fb_shell (own-builder fire block): the shell starts at the
--      rule's shell_start_add, so its blast tick is the engine's.
--   5. No brain source still holds an old 128 * sight site.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_shell_model_all.lua
-- Arguments:
--   old        run 1-4 with C.SHELL_MODEL_RULES_ALL = false (the old code
--              paths; they must fail).
--   src=<dir>  run 5 on the .lua files in <dir> (default: this directory).
-- =========================================================================

package.path = "./?.lua;" .. package.path

local OLD, SRC = false, "."
for _, a in ipairs(arg or {}) do
  if a == "old" then OLD = true end
  local d = a:match("^src=(.+)$")
  if d then SRC = d end
end

local C = require("constants")
local U = require("util")
U.ttype_peek = function() return C.T_GRASS end
U.ttype = function() return C.T_GRASS end
local KL = require("kill_lgm")
local S = require("steering")
local D = require("danger")

if OLD then C.SHELL_MODEL_RULES_ALL = false end
print(string.format("C.SHELL_MODEL_RULES_ALL = %s", tostring(C.SHELL_MODEL_RULES_ALL)))

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

-- The engine burst, wu out along the nose, and the move count L.
local function engine_burst(r, sl)
  local speed = r.shell_speed or 32
  local life  = r.shell_life or 8
  local add   = r.shell_start_add or 5
  local L = 1 + math.floor(life * (sl / 2)) - add
  if L < 0 then L = 0 end
  return speed * (add + L), L
end

-- Rule sets: classic, a short shell life, a slow shell, a short start.
local RULES = {
  { name = "classic",          r = {} },
  { name = "shell_life 6",     r = { shell_life = 6 } },
  { name = "shell_speed 24",   r = { shell_speed = 24 } },
  { name = "shell_start_add 2", r = { shell_start_add = 2 } },
}
local SMAX = 14
local LAND = C.DEMINE_LAND_WU or 100

-- The mine square: (100, 100); its centre in wu. The tank stands straight
-- south of the mine, nose north (direction 0).
local MX, MY = 100, 100
local WX, WY = MX * 256 + 128, MY * 256 + 128

-- ── 1. mine shot ─────────────────────────────────────────────────────────
print("1. mine shot (steering.mine_shot: kill_mine and sea-pill detonate)")
for _, rs in ipairs(RULES) do
  KL.set_rules(rs.r)
  local bad_fire, bad_sight, n = 0, 0, 0
  local ex_fire, ex_sight
  for dist = 256, 1800, 4 do
    -- The sight it asks for: the nearest burst of all sights.
    local info = { tankx = WX, tanky = WY + dist, direction = 0, gunrange = 14 }
    local target_sl = S.mine_shot(info, WX, WY)
    local best_e = math.huge
    for sl = 2, SMAX do
      local e = math.abs(engine_burst(rs.r, sl) - dist)
      if e < best_e then best_e = e end
    end
    local got_e = math.abs(engine_burst(rs.r, target_sl) - dist)
    if got_e > best_e then
      bad_sight = bad_sight + 1
      ex_sight = ex_sight or string.format(
        "mine %d wu out: asks sight %d (burst %d wu off), best is %d wu off",
        dist, target_sl, got_e, best_e)
    end
    -- The burst test at every sight it may hold.
    for g = 2, SMAX do
      n = n + 1
      info.gunrange = g
      local _, cur_sl, land_off = S.mine_shot(info, WX, WY)
      local real = engine_burst(rs.r, g)
      local sq = math.floor((WY + dist - real) / 256)
      if land_off <= LAND and sq ~= MY then
        bad_fire = bad_fire + 1
        ex_fire = ex_fire or string.format(
          "mine %d wu out, sight %d: test says %d wu off (fire), the shell bursts %d wu out, on square %d not %d",
          dist, cur_sl, math.floor(land_off + 0.5), real, sq, MY)
      end
    end
  end
  check(string.format("%s: every shot the test lets go bursts on the mine square (%d cases)",
                      rs.name, n), bad_fire == 0,
        string.format("%d bad; first: %s", bad_fire, tostring(ex_fire)))
  check(string.format("%s: the sight asked for has the nearest burst", rs.name), bad_sight == 0,
        string.format("%d bad; first: %s", bad_sight, tostring(ex_sight)))
end
KL.set_rules(nil)
-- The coordinator's example, classic rules: mine 924 wu out, sight 8 held.
do
  local info = { tankx = WX, tanky = WY + 924, direction = 0, gunrange = 8 }
  local _, _, land_off = S.mine_shot(info, WX, WY)
  local real = engine_burst({}, 8)
  local sq = math.floor((WY + 924 - real) / 256)
  print(string.format("     classic, mine 924 wu out, sight 8: test %d wu off; the shell bursts %d wu out, on square %d (mine on %d)",
                      math.floor(land_off + 0.5), real, sq, MY))
  check("classic, mine 924 wu out, sight 8: no shot (the burst is on the next square)",
        land_off > LAND, land_off)
end

-- ── 2. tank-combat reach ─────────────────────────────────────────────────
print("2. tank-combat reach (steering.combat_reach_wu)")
for _, rs in ipairs(RULES) do
  KL.set_rules(rs.r)
  local bad, ex = 0, nil
  for g = 2, SMAX do
    local reach = S.combat_reach_wu({ gunrange = g, rules = rs.r })
    local real = engine_burst(rs.r, g)
    if reach ~= real + 64 then
      bad = bad + 1
      ex = ex or string.format("sight %d: reach %d, burst %d (+64 = %d)", g, reach, real, real + 64)
    end
  end
  check(string.format("%s: reach = burst + 64 at every sight", rs.name), bad == 0,
        string.format("%d bad; first: %s", bad, tostring(ex)))
end
KL.set_rules(nil)

-- ── 3. fire ranges and crosshairs ────────────────────────────────────────
print("3. fire ranges and crosshairs (kill_lgm.shell_travel(sl, kill_lgm.all_on()))")
for _, rs in ipairs(RULES) do
  KL.set_rules(rs.r)
  local bad, ex = 0, nil
  for g = 2, SMAX do
    local got = KL.shell_travel(g, KL.all_on())
    local real = engine_burst(rs.r, g)
    if got ~= real then
      bad = bad + 1
      ex = ex or string.format("sight %d: %d wu, burst %d wu", g, got, real)
    end
  end
  check(string.format("%s: the range is the burst at every sight", rs.name), bad == 0,
        string.format("%d bad; first: %s", bad, tostring(ex)))
  local mt = KL.max_travel(KL.all_on())
  check(string.format("%s: the longest-sight crosshair is the burst at sight 14", rs.name),
        mt == engine_burst(rs.r, SMAX), mt)
end
KL.set_rules(nil)

-- ── 4. danger.lua own-shell start ────────────────────────────────────────
print("4. own-builder fire block shell (danger.fb_shell)")
for _, rs in ipairs(RULES) do
  local TX, TY = 100 * 256 + 128, 120 * 256 + 128
  local info = { tankx = TX, tanky = TY, tank_angle = 0, direction = 0, gunrange = 14,
                 rules = rs.r, objects = {}, player_number = 0 }
  local T, ex, ey, _, how = D.fb_shell({}, info)
  local real, L = engine_burst(rs.r, 14)
  check(string.format("%s: blast tick %d (L + 1), %d wu out", rs.name, L + 1, real),
        T == L + 1 and ex == TX and ey == TY - real and how == "expiry",
        string.format("tick %s, (%s, %s) = %s wu out, %s", tostring(T), tostring(ex), tostring(ey),
                      tostring(ey and (TY - ey)), tostring(how)))
end

-- ── 5. no old 128 * sight site left ──────────────────────────────────────
print(string.format("5. no old 128 * sight site in the brain sources (%s)", SRC))
local OLD_SITES = {
  { pat = "128 %* cur_sl",                          what = "128 * cur_sl" },
  { pat = "128 %* %(info%.gunrange",                what = "128 * (info.gunrange ...)" },
  { pat = "KL%.sightlen_for%(",                     what = "KL.sightlen_for(" },
  { pat = "kill_lgm%.sightlen_for%(",               what = "kill_lgm.sightlen_for(" },
  { pat = "%(info%.gunrange or 14%) / 2%.0",        what = "(info.gunrange or 14) / 2.0" },
  { pat = "%(info%.gunrange or C%.GUNSIGHT_MAX%) / 2%.0", what = "(info.gunrange or C.GUNSIGHT_MAX) / 2.0" },
  { pat = ", 7%.0%)",                               what = "crosshair at 7.0 tiles" },
}
local SKIP = { ["kill_lgm.lua"] = true, ["los_stamp_cache.lua"] = true, ["shadow.lua"] = true }
local files = {}
local p = io.popen('ls "' .. SRC .. '"')
for f in p:lines() do
  f = f:gsub("\r", "")
  if f:match("%.lua$") and not f:match("^test_") and not SKIP[f] then files[#files + 1] = f end
end
p:close()
local hits = {}
for _, f in ipairs(files) do
  local fh = io.open(SRC .. "/" .. f, "r")
  if fh then
    local ln = 0
    for line in fh:lines() do
      ln = ln + 1
      if not line:match("^%s*%-%-") then
        for _, o in ipairs(OLD_SITES) do
          if line:find(o.pat) then hits[#hits + 1] = string.format("%s:%d %s", f, ln, o.what) end
        end
      end
    end
    fh:close()
  end
end
check(string.format("no old site in %d files", #files), #hits == 0,
      #hits .. ": " .. table.concat(hits, "; "))

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
