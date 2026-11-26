-- =========================================================================
-- test_qr_own_hits.lua — standalone unit tests (2026-11-24, run_c14
-- beside_n / beside_s) for two quick-repair send fixes:
--   1. quick_repair.own_hits: our take's shells in flight and the shots
--      still to fire, as hit ticks on the target.
--   2. the target's fire ticks with those hits: each hit cuts its reload
--      on the engine's ladder, so it fires sooner than the old reload says.
--      (beside_n: our hit at 845 cut the reload from 13 to 6, and the shot
--      at 854 met the blocker while the builder stood on it.)
--   3. quick_repair.walk_ticks: with the world form of the walk sim it
--      starts the builder where the engine puts him down
--      (danger.lgm_start_pos), not on the tank square's centre; with only
--      the map form, the old walk.
-- No engine: the walk sim is a stub.
--
-- Run from this directory with the LuaJIT built beside the game:
--   <luajit> test_qr_own_hits.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_qr_own_hits.lua
-- =========================================================================

local SRC
for _, a in ipairs(arg or {}) do
  local v = a:match("^src=(.+)$")
  if v then SRC = v end
end
package.path = (SRC and (SRC .. "/?.lua;") or "") .. "./?.lua;" .. package.path

package.loaded["print2"] = setmetatable({ set_tick = function() end },
  { __call = function() end })

if not math.atan2 then math.atan2 = function(y, x) return math.atan(y, x) end end

_G.OBJECT_TANK, _G.OBJECT_SHOT = 0, 1
_G.BUILDMODE_FARM, _G.BUILDMODE_PBOX = 1, 3

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local QR     = require("quick_repair")

U.ttype = function() return C.T_GRASS end

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print("  ok   " .. name)
  else
    fail = fail + 1
    print("  FAIL " .. name .. (got ~= nil and ("  (got " .. tostring(got) .. ")") or ""))
  end
end
local function list(t)
  local s = {}
  for i = 1, #t do s[i] = tostring(t[i]) end
  return "{" .. table.concat(s, ",") .. "}"
end

print("1. own_hits")
do
  check("helper exists", type(QR.own_hits) == "function", type(QR.own_hits))
  if type(QR.own_hits) == "function" then
    -- beside_n t=785: T_kill 858 = first 796 + reload 13 + flight 49.
    local k = { src = "predict", first = 796, reload = 13, remaining = 2, flight = 49,
                land = {} }
    local h = QR.own_hits(k, 785, 120, {})
    table.sort(h)
    check("two shots to fire: hits at 845 and 858", #h == 2 and h[1] == 845 and h[2] == 858, list(h))
    k.land = { 790, 780 }
    h = QR.own_hits(k, 785, 120, {})
    table.sort(h)
    check("a shell in flight is a hit; one already landed is not",
          #h == 3 and h[1] == 790 and h[2] == 845, list(h))
    h = QR.own_hits(k, 785, 65, {})
    table.sort(h)
    check("horizon cuts the late hit", #h == 2 and h[2] == 845, list(h))
    h = QR.own_hits({ src = "inflight", land = { 800 } }, 785, 120, { 1 })
    check("inflight kill: its shells only, appended", #h == 2 and h[1] == 1 and h[2] == 800, list(h))
    check("no record: unchanged", #QR.own_hits(nil, 785, 120, {}) == 0)
  end
end

print("2. the target's fire ticks with our hits")
do
  -- An angry pill: its seen hits brought it to the ladder's 13.
  local x = { pill_attack_ticks = 100, pill_attack_min_ticks = 6,
              pill_angry_divisor = 2, pill_cooldown_ticks = 32 }
  local p = { mx = 126, my = 118, _eng_hits = { 700, 730, 760 } }
  local info = { objects = {} }
  local base = QR.fire_ticks(p, info, 785, x, 120, nil, 858)
  local r0 = #base >= 2 and base[2] - base[1] or nil
  check("without our hits: one reload apart", r0 and base[#base] - base[#base - 1] == r0, list(base))
  if type(QR.own_hits) == "function" then
    local k = { src = "predict", first = 796, reload = 13, remaining = 2, flight = 49, land = {} }
    local more = QR.own_hits(k, 785, 120, {})
    local f = QR.fire_ticks(p, info, 785, x, 120, more, 858)
    local gap = nil
    for i = 2, #f do if f[i - 1] > 845 then gap = f[i] - f[i - 1]; break end end
    check("after our hit at 845 the reload is shorter",
          gap and r0 and gap < r0, list(f))
    check("more shots before the kill", #f > #base, #f .. " vs " .. #base)
  end
end

print("3. walk_ticks start place")
do
  local saved_w, saved_m = _G.cpf_lgm_travel_ticks, _G.cpf_lgm_travel_ticks_map
  local got
  _G.cpf_lgm_travel_ticks_map = function(fx, fy) got = { "map", fx, fy }; return 85 end
  _G.cpf_lgm_travel_ticks = nil
  local info = { tankx = math.floor(132.42 * 256), tanky = math.floor(120.48 * 256) }
  local w = QR.walk_ticks(info, 127, 118)
  check("map form only: the tank square", w == 85 and got[1] == "map" and got[2] == 132 and got[3] == 120,
        got and got[1])
  _G.cpf_lgm_travel_ticks = function(sx, sy, dx, dy) got = { "world", sx, sy, dx, dy }; return 82 end
  w = QR.walk_ticks(info, 127, 118)
  local sx, sy = danger.lgm_start_pos(info, 127, 118)
  check("world form: walk 82", w == 82, w)
  check("world form: starts at danger.lgm_start_pos",
        got[1] == "world" and got[2] == math.floor(sx) and got[3] == math.floor(sy),
        got[1] .. " " .. tostring(got[2]) .. "," .. tostring(got[3]))
  check("world form: to the blocker's centre", got[4] == 127 * 256 + 128 and got[5] == 118 * 256 + 128)
  check("start is not the tank square's centre", math.floor(sx) ~= 132 * 256 + 128)
  check("next door: one grass tile, no walk sim", QR.walk_ticks(info, 132, 119) == (C.BUILDER_POOL_GRASS_TICKS_PER_TILE or 16))
  _G.cpf_lgm_travel_ticks, _G.cpf_lgm_travel_ticks_map = saved_w, saved_m
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
