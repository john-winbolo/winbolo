-- =========================================================================
-- test_aggro_deep.lua — standalone unit tests (2026-11-01) for deep aggro
-- placement (knob C.PILL_PLACE_AGGRO_DEEP):
--   * goals.aggro_deep_ctx — hostile base list and group centre
--   * goals.score_place_tile — the new ebase / depth terms and the changed
--     prx / inf / ctr terms, knob on vs off, hand-computed
--   * eval_place_pill_strategic (reached through score_place_tile's upvalue)
--     — the extra tiles around a hostile base outside the tank square
--   * the debug breakdown string adds up to the score
--   * builder.set_mode — the long send for an aggro deep goal
-- No engine: influence is a stub field, every tile is placeable.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_aggro_deep.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local threat = require("threat")
local PP     = require("pill_portfolio")
local danger = require("danger")
local squad  = require("squad")
local G      = require("goals")
local B      = require("builder")

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
local function near(a, b) return a ~= nil and b ~= nil and math.abs(a - b) < 1e-6 end

-- ── Engine stand-ins ─────────────────────────────────────────────────────
-- Influence: x < 150 is enemy ground (-30), x >= 150 is ours (+20).
cpf.influence_at   = function(x, y) return (x < 150) and -30 or 20 end
cpf.smart_cost_dij_only = function() return 10 end
U.is_placeable     = function() return true end
U.los_coverage     = function() return 0 end
U.ttype            = function() return 4 end
threat.at          = function() return 0 end
threat.coverage_at = function() return 0 end
threat.pill_at     = function() return 0 end
danger.should_panic_build = function() return false, 0, "test" end
squad.blitz_shot_lines = function() return nil end
_G.cpf_dijkstra_lookup_by_kind = function() return 20 end   -- smart_cost
_G.cpf_estimate_tank_travel_ticks = function() return 50 end
_G.KIND_NORMAL = 0

C.BLITZ_NO_BUILD_ACTIVE    = false
C.PLACE_PILL_MAN_PATH_SAFE = false
C.PLACE_PILL_BEHIND_ONLY   = false
C.STRATEGIC_PLACE_MIN_SCORE       = -1e9   -- no spot-quality floor in the test
C.STRATEGIC_PLACE_MIN_SCORE_FLOOR = -1e9
-- The first version of the deep rule: the spike rules (spike.lua) are tested
-- in test_spike.lua. All three off = one deep spike per pill, every aggro tile.
C.AGGRO_DEEP_BASE_GATE = false
C.AGGRO_DEEP_PILL_GATE = false
C.AGGRO_DEEP_PAIR      = "off"
PP.set_targets(0, 0, 1, 0, "test")         -- every pill is wanted as aggro

local function base(mx, my, owner) return { mx = mx, my = my, owner = owner } end
local function info_at(mx, my)
  return { tankx = mx * 256 + 128, tanky = my * 256 + 128,
           carried_pills = 5, man_status = C.LGM_INTANK, inboat = false,
           armour = 40, shells = 40, trees = 40, mines = 0, player_number = 0 }
end
local function fresh_state() return { tick = 1000, perc = { enemy_tanks = {} } } end

-- eval_place_pill_strategic is a local; score_place_tile closes over it.
local eval_pps
do
  local i = 1
  while true do
    local n, v = debug.getupvalue(G.score_place_tile, i)
    if not n then break end
    if n == "eval_place_pill_strategic" then eval_pps = v end
    i = i + 1
  end
end
check("eval_place_pill_strategic reachable", eval_pps ~= nil)

-- ── 1. aggro_deep_ctx ────────────────────────────────────────────────────
print("goals.aggro_deep_ctx")
local W1 = { pills = {}, bases = {
  [1] = base(130, 130, "hostile"),
  [2] = base(106, 100, "hostile"),
  [3] = base(100, 100, "hostile"),
  [4] = base(60, 60, "friendly"),
} }
C.PILL_PLACE_AGGRO_DEEP = true
local ad = G.aggro_deep_ctx(W1)
check("ctx exists with knob on", ad ~= nil)
check("three hostile bases, friendly left out", ad and #ad.hb == 3, ad and #ad.hb)
check("sorted by tile id: (100,100) first", ad and ad.hb[1].mx == 100 and ad.hb[2].mx == 106)
check("group centre (100,100) (tie with (106,100): lower id)",
      ad and ad.gmx == 100 and ad.gmy == 100 and ad.gn == 1,
      ad and string.format("(%s,%s)+%s", ad.gmx, ad.gmy, ad.gn))
C.PILL_PLACE_AGGRO_DEEP = false
check("ctx nil with knob off", G.aggro_deep_ctx(W1) == nil)
C.PILL_PLACE_AGGRO_DEEP = true
C.PILL_PLACE_TURTLE = true
check("ctx nil while turtling", G.aggro_deep_ctx(W1) == nil)
C.PILL_PLACE_TURTLE = false
do
  local W3 = { pills = {}, bases = {
    [1] = base(10, 10, "hostile"), [2] = base(200, 200, "hostile"),
    [3] = base(205, 200, "hostile"), [4] = base(214, 204, "hostile") } }
  local a3 = G.aggro_deep_ctx(W3)
  check("group centre = base with most hostile bases near it",
        a3.gmx == 205 and a3.gmy == 200 and a3.gn == 2,
        string.format("(%s,%s)+%s", a3.gmx, a3.gmy, a3.gn))
end

-- ── 2. score_place_tile: term deltas, knob on vs off ─────────────────────
print("goals.score_place_tile")
local function score(world, on, mx, my, tmx, tmy)
  C.PILL_PLACE_AGGRO_DEEP = on
  return G.score_place_tile(fresh_state(), world, info_at(tmx, tmy), mx, my, -5, tmx, tmy)
end
-- A: no friendly base. Old ctr 0 (no centre), old inf -100.
-- Tile (103,104), inf -30: ebase = 60 (to (100,100): 7) + 60 (to (106,100): 7)
-- = 120; depth = min(80, 2*30) = 60; ctr = (16-7)*4 = 36; inf 0 (was -100).
-- prx 0 both (no friendly base). dst pinned to -5 both.
local WA = { pills = {}, bases = { [1] = base(130, 130, "hostile"),
  [2] = base(106, 100, "hostile"), [3] = base(100, 100, "hostile") } }
local a_off = score(WA, false, 103, 104, 103, 104)
local a_on  = score(WA, true,  103, 104, 103, 104)
check("A: both score", a_off ~= nil and a_on ~= nil, tostring(a_off) .. "/" .. tostring(a_on))
check("A: on - off = ebase 120 + depth 60 + ctr 36 + inf 100 = 316",
      near(a_on - a_off, 316), a_on and a_off and (a_on - a_off))
-- B: friendly base at (90,104), 13 from the tile. Old prx = (MAX-13)*2,
-- old ctr = base_defense centre (90,104): (16-13)*4 = 12; new ctr 36, prx 0.
local WB = { pills = {}, bases = { [1] = base(130, 130, "hostile"),
  [2] = base(106, 100, "hostile"), [3] = base(100, 100, "hostile"),
  [4] = base(90, 104, "friendly") } }
local b_off = score(WB, false, 103, 104, 103, 104)
local b_on  = score(WB, true,  103, 104, 103, 104)
local prx_old = math.max(0, C.STRATEGIC_PLACE_MAX_BASE_DIST - 13) * C.STRATEGIC_PLACE_BASE_WEIGHT
check(string.format("B: on - off = 120 + 60 + (36-12) + 100 - prx %.0f", prx_old),
      near(b_on - b_off, 120 + 60 + 24 + 100 - prx_old), b_on and b_off and (b_on - b_off))
-- C: a back tile (ours, inf +20) scores the same with the knob on or off.
C.STRATEGIC_PLACE_STRICT_NEED = false
PP.set_targets(0.5, 0, 0.5, 0, "test")
local c_off = score(WB, false, 200, 104, 200, 104)
local c_on  = score(WB, true,  200, 104, 200, 104)
check("C: non-aggro tile unchanged", c_off ~= nil and near(c_off, c_on),
      tostring(c_off) .. "/" .. tostring(c_on))
C.STRATEGIC_PLACE_STRICT_NEED = true
PP.set_targets(0, 0, 1, 0, "test")
-- D: depth cap. A tile far from every hostile base on -30 ground gets 60;
-- at -50 it would be 100, capped to 80.
cpf.influence_at = function(x, y) return (x < 150) and -50 or 20 end
local d_off = score(WA, false, 20, 200, 20, 200)
local d_on  = score(WA, true,  20, 200, 20, 200)
-- Far tile: ebase 0; ctr: dist to (100,100) = 180 > 16 -> 0; inf +100.
check("D: depth capped at 80 (on - off = 80 + inf 100)",
      near(d_on - d_off, 180), d_on and d_off and (d_on - d_off))
cpf.influence_at = function(x, y) return (x < 150) and -30 or 20 end

-- ── 3. full eval: extra tiles around a far hostile base ──────────────────
print("eval_place_pill_strategic scan")
-- Tank at (60,104); scan radius 14 (aggro) + 6 (urgency, 5 carried) = 20,
-- so the tank square is x 40..80. The hostile bases sit at x 100..106.
local function run_eval(on, world, debug_on)
  C.PILL_PLACE_AGGRO_DEEP = on
  _G.BRAIN_DEBUG_MODE = debug_on or nil
  local st, inf = fresh_state(), info_at(60, 104)
  local r = eval_pps(st, world, inf, 60, 104, false, 40, nil)
  _G.BRAIN_DEBUG_MODE = nil
  return r, st
end
local r_off = run_eval(false, WA)
local r_on  = run_eval(true, WA)
check("off: a spot inside the tank square", r_off and r_off.goal.mx <= 80,
      r_off and string.format("(%d,%d)", r_off.goal.mx, r_off.goal.my))
check("off: no aggro deep fields on the goal",
      r_off and r_off.goal._aggro_deep == nil and r_off.goal._place_cat == nil)
check("off: _spot_sc7 uses 0.5",
      r_off and near(r_off.goal._spot_sc7, -U.mdist(60, 104, r_off.goal.mx, r_off.goal.my) * 0.5))
check("on: a spot outside the tank square, by the hostile bases",
      r_on and r_on.goal.mx > 80 and U.mdist(r_on.goal.mx, r_on.goal.my, 100, 100) <= 16,
      r_on and string.format("(%d,%d)", r_on.goal.mx, r_on.goal.my))
check("on: goal says aggro deep", r_on and r_on.goal._aggro_deep == true
      and r_on.goal._place_cat == "aggro")
check("on: _spot_sc7 uses 0.1",
      r_on and near(r_on.goal._spot_sc7,
        -U.mdist(60, 104, r_on.goal.mx, r_on.goal.my) * C.STRATEGIC_PLACE_AGGRO_TANK_DIST_WEIGHT))
do
  -- Knob on but no aggro room (targets all back): no extra tiles at all.
  PP.set_targets(1, 0, 0, 0, "test")
  C.STRATEGIC_PLACE_STRICT_NEED = false
  local _, st = run_eval(true, WA)
  PP.set_targets(0, 0, 1, 0, "test")
  C.STRATEGIC_PLACE_STRICT_NEED = true
  check("no aggro room: nothing logged as deep", st._aggro_deep_log == nil)
end

-- ── 4. breakdown string adds up (BRAIN_DEBUG_MODE) ───────────────────────
print("debug breakdown")
-- The opt/ copy of goals.lua has its BRAIN_DEBUG_MODE blocks stripped, so
-- there is no breakdown to read there: skip this section.
local function has_breakdown()
  local fh = io.open("goals.lua", "r")
  if not fh then return false end
  local src = fh:read("*a"); fh:close()
  return src:find("ebase{%.0f} + depth{%.0f}", 1, true) ~= nil
end
if not has_breakdown() then
  print("  skip (stripped goals.lua: no debug breakdown)")
else
  local ok, r = pcall(run_eval, true, WA, true)
  local win
  if ok and r and r.cands then
    for _, c in ipairs(r.cands) do
      if c.mx == r.goal.mx and c.my == r.goal.my then win = c.formula end
    end
  end
  check("debug run gives a winner formula", win ~= nil, ok and "no formula" or r)
  if win then
    print("    winner: " .. win:sub(1, 400))
    local total = tonumber(win:match("^score{(%-?[%d%.]+)}"))
    local sum = 0
    local body = win:match("^score{[^}]*} = (.-)%s%s") or win
    for v in body:gmatch("{(%-?[%d%.]+)}") do sum = sum + tonumber(v) end
    check("formula has ebase{} and depth{}", win:find("ebase{", 1, true) and win:find("depth{", 1, true))
    check("formula names the deep rule", win:find("aggro_deep{", 1, true) ~= nil)
    check("terms add up to the score (within rounding)", total and math.abs(sum - total) <= 1,
          string.format("sum %.1f score %s", sum, tostring(total)))
  end
end  -- has_breakdown

-- ── 5. builder.set_mode long send ───────────────────────────────────────
print("builder.set_mode")
local function setmode(goal, tmx, tmy)
  local st = { tick = 1000, builder = {}, vuln = 100, imdanger = 100 }
  B.set_mode(st, { pills = {}, bases = {} }, info_at(tmx, tmy), goal)
  return st.builder.mode
end
local deep = { kind = "place_pill_strategic", mx = 100, my = 104, _aggro_deep = true }
local plain = { kind = "place_pill_strategic", mx = 100, my = 104 }
check("aggro deep at 12, no trigger: builder sent", setmode(deep, 88, 104) == "place_pill",
      setmode(deep, 88, 104))
check("plain at 12, no trigger: no send", setmode(plain, 88, 104) ~= "place_pill",
      setmode(plain, 88, 104))
check("aggro deep at 15: builder sent", setmode(deep, 85, 104) == "place_pill")
check("aggro deep at 16: no send", setmode(deep, 84, 104) ~= "place_pill")

C.PILL_PLACE_AGGRO_DEEP = true
print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
