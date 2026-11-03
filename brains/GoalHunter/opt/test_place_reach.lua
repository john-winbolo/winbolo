-- =========================================================================
-- test_place_reach.lua — standalone unit tests (2026-11-02) for the rch term
-- of the placement scan (knob C.PLACE_ENEMY_REACH):
--   * goals.enemy_reach — which known pills and bases count, Euclidean range
--   * goals.score_place_tile — rch = weight x (pills + bases), knob 0 vs 60,
--     and pills only on a focused spike tile (ebase pays for the bases)
--   * the debug breakdown string has rch{} and adds up to the score
--   * the default and the KEEL preset are 0
-- No engine: influence is a stub field, every tile is placeable.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_place_reach.lua
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

C.FOCUS_SPIKE_RULE = "count"

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

-- ── Engine stand-ins (as test_focus_spike.lua) ───────────────────────────
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
_G.cpf_dijkstra_lookup_by_kind = function() return 20 end
_G.cpf_estimate_tank_travel_ticks = function() return 50 end
_G.KIND_NORMAL = 0

C.BLITZ_NO_BUILD_ACTIVE    = false
C.PLACE_PILL_MAN_PATH_SAFE = false
C.PLACE_PILL_BEHIND_ONLY   = false
C.STRATEGIC_PLACE_MIN_SCORE       = -1e9
C.STRATEGIC_PLACE_MIN_SCORE_FLOOR = -1e9
C.FOCUS_SPIKE_BASE_RULE = false
C.FOCUS_SPIKE_PILL_RULE = false
C.FOCUS_SPIKE_PAIR      = "off"
C.FOCUS_SPIKE_ENABLED   = false
PP.set_targets(0, 0, 1, 0, "test")         -- every pill is wanted as aggro

local function pill(mx, my, owner, health, in_tank)
  return { mx = mx, my = my, owner = owner, health = health or 15, in_tank = in_tank }
end
local function base(mx, my, owner) return { mx = mx, my = my, owner = owner } end
local function info_at(mx, my)
  return { tankx = mx * 256 + 128, tanky = my * 256 + 128,
           carried_pills = 5, man_status = C.LGM_INTANK, inboat = false,
           armour = 40, shells = 40, trees = 40, mines = 0, player_number = 0 }
end
local function fresh_state() return { tick = 1000, perc = { enemy_tanks = {} } } end

-- ── 0. default and KEEL ──────────────────────────────────────────────────
print("constants")
check("default PLACE_ENEMY_REACH = 0", C.PLACE_ENEMY_REACH == 0, C.PLACE_ENEMY_REACH)
check("KEEL PLACE_ENEMY_REACH = 0", C.PRESETS.keel.PLACE_ENEMY_REACH == 0,
      C.PRESETS.keel.PLACE_ENEMY_REACH)
check("classic range 2048 wu = 8 tiles", (C.PILLBOX_RANGE_WU or 2048) / 256 == 8)

-- ── 1. enemy_reach: what counts ──────────────────────────────────────────
print("goals.enemy_reach")
-- Spot (100,100).
local W = { pills = {
  [1] = pill(108, 100, "hostile"),          -- 8.00: in (inclusive)
  [2] = pill(105, 105, "hostile"),          -- 7.07: in (Manhattan 10 would say out)
  [3] = pill(106, 106, "hostile"),          -- 8.49: out
  [4] = pill(100, 103, "neutral"),          -- neutral: out
  [5] = pill(100, 104, "hostile", 0),       -- dead: out
  [6] = pill(101, 100, "hostile", 15, true),-- carried: out
  [7] = pill(99, 100, "friendly"),          -- ours: out
}, bases = {
  [1] = base(100, 108, "hostile"),          -- 8.00: in
  [2] = base(94, 94, "hostile"),            -- 8.49: out
  [3] = base(97, 100, "neutral"),           -- neutral: out
  [4] = base(103, 100, "friendly"),         -- ours: out
} }
local np, nb, r = G.enemy_reach(W, 100, 100, true)
check("2 hostile pills in range", np == 2, np)
check("1 hostile base in range", nb == 1, nb)
check("range 8 tiles", r == 8, r)
local np2, nb2 = G.enemy_reach(W, 100, 100, false)
check("with_bases false: pills only", np2 == 2 and nb2 == 0, string.format("%d/%d", np2, nb2))

-- ── 2. score_place_tile: rch term ────────────────────────────────────────
print("goals.score_place_tile")
local function score(world, w, on, mx, my)
  C.PLACE_ENEMY_REACH = w
  C.FOCUS_SPIKE_ENABLED = on
  local s = G.score_place_tile(fresh_state(), world, info_at(mx, my), mx, my, -5, mx, my)
  C.PLACE_ENEMY_REACH = 0
  C.FOCUS_SPIKE_ENABLED = false
  return s
end
local s0  = score(W, 0, false, 100, 100)
local s60 = score(W, 60, false, 100, 100)
check("normal tile: both score", s0 ~= nil and s60 ~= nil, tostring(s0) .. "/" .. tostring(s60))
check("normal tile: 60 - 0 = 60 x (2 pills + 1 base) = 180", near(s60 - s0, 180),
      s60 and s0 and (s60 - s0))
-- Focused spike tile (aggro, enemy ground): ebase pays for the base, so rch
-- counts the 2 pills only.
local d0  = score(W, 0, true, 100, 100)
local d60 = score(W, 60, true, 100, 100)
check("focused spike tile: 60 - 0 = 60 x 2 pills = 120", near(d60 - d0, 120),
      d60 and d0 and (d60 - d0))
-- No hostile target in range: no change.
local WE = { pills = { [1] = pill(130, 130, "hostile") }, bases = {} }
check("no target in range: rch 0", near(score(WE, 60, false, 100, 100), score(WE, 0, false, 100, 100)))

-- ── 3. breakdown string (BRAIN_DEBUG_MODE) ───────────────────────────────
print("debug breakdown")
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
local function has_breakdown()
  local fh = io.open("goals.lua", "r")
  if not fh then return false end
  local src = fh:read("*a"); fh:close()
  return src:find("depth{%.0f} + rch{%.0f}", 1, true) ~= nil
end
if not has_breakdown() then
  print("  skip (stripped goals.lua: no debug breakdown)")
else
  C.PLACE_ENEMY_REACH = 60
  _G.BRAIN_DEBUG_MODE = true
  local ok, res = pcall(eval_pps, fresh_state(), W, info_at(100, 100), 100, 100, false, 40, nil)
  _G.BRAIN_DEBUG_MODE = nil
  C.PLACE_ENEMY_REACH = 0
  local win
  if ok and res and res.cands then
    for _, c in ipairs(res.cands) do
      if c.mx == res.goal.mx and c.my == res.goal.my then win = c.formula end
    end
  end
  check("debug run gives a winner formula", win ~= nil, ok and "no formula" or res)
  if win then
    print("    winner: " .. win:sub(1, 400))
    local total = tonumber(win:match("^score{(%-?[%d%.]+)}"))
    local sum = 0
    local body = win:match("^score{[^}]*} = (.-)%s%s") or win
    for v in body:gmatch("{(%-?[%d%.]+)}") do sum = sum + tonumber(v) end
    check("formula has rch{}", win:find("+ rch{", 1, true) ~= nil)
    check("formula explains rch", win:find("hostile pills +", 1, true) ~= nil)
    check("terms add up to the score (within rounding)", total and math.abs(sum - total) <= 1,
          string.format("sum %.1f score %s", sum, tostring(total)))
    local rv, a, b = win:match("rch{(%-?[%d%.]+)}  rch{60 x %((%d+) hostile pills %+ (%d+) hostile bases%)")
    check("rch value = 60 x (pills + bases)", rv and tonumber(rv) == 60 * (tonumber(a) + tonumber(b)),
          tostring(rv) .. " " .. tostring(a) .. " " .. tostring(b))
  end
  check("goal carries _spot_rch", ok and res and res.goal._spot_rch ~= nil,
        ok and res and tostring(res.goal._spot_rch))
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
