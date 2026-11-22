-- =========================================================================
-- test_place_front_target.lua — standalone unit tests (2026-11-04) for the
-- "front needs a target" rule of pill placement (knobs
-- C.PLACE_FRONT_NEED_TARGET, C.PLACE_FRONT_TARGET_RANGE):
--   * pill_portfolio.front_targets — which known pills and bases count
--     (enemy base, enemy pill, neutral pill; not ours, not allied, not in a
--     tank, not without a position);
--   * pill_portfolio.place_classify — Euclidean range (inclusive), and the
--     class a front spot with no target gets (back / aggro / beyond);
--   * knob false: the class is exactly classify()'s;
--   * the eval scan (reached through score_place_tile's upvalue): a front
--     spot with nothing in range is not placed as front, and the
--     PLACE_FRONT_NOTARGET line is written;
--   * the default is on, range 8, and the KEEL preset is off.
-- No engine: influence is a stub, every tile is placeable.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_place_front_target.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_place_front_target.lua
-- (the print2 lines are stripped there, so the log checks are skipped).
-- =========================================================================

package.path = "./?.lua;" .. package.path

local LOG = {}
package.loaded["print2"] = setmetatable({ set_tick = function() end },
  { __call = function(_, s) LOG[#LOG + 1] = tostring(s) end })

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local threat = require("threat")
local PP     = require("pill_portfolio")
local danger = require("danger")
local squad  = require("squad")
local G      = require("goals")

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

-- ── Engine stand-ins ─────────────────────────────────────────────────────
-- Influence: x < 100 is enemy ground (-10), x >= 100 is ours (+10), so the
-- columns x = 99 and x = 100 are the front line. Rows y >= 200 are unclaimed
-- (0) ground, for the "beyond" case.
cpf.influence_at   = function(x, y)
  if y >= 200 then return 0 end
  return (x < 100) and -10 or 10
end
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
C.FOCUS_SPIKE_ENABLED   = false
C.FRONT_NEAR_RADIUS_PLACE = 0

local function pill(mx, my, owner, in_tank)
  return { mx = mx, my = my, owner = owner, health = 15, in_tank = in_tank }
end
local function base(mx, my, owner) return { mx = mx, my = my, owner = owner } end
local function info_at(mx, my)
  return { tankx = mx * 256 + 128, tanky = my * 256 + 128,
           carried_pills = 5, man_status = C.LGM_INTANK, inboat = false,
           armour = 40, shells = 40, trees = 40, mines = 0, player_number = 0 }
end
local function fresh_state() return { tick = 1000, perc = { enemy_tanks = {} } } end

-- Class of spot (mx, my) with the knob on and the world W.
local function cls(W, mx, my)
  C.PLACE_FRONT_NEED_TARGET = true
  return PP.place_classify(mx, my, PP.front_targets(W))
end

-- ── 0. default and KEEL ──────────────────────────────────────────────────
print("constants")
check("default PLACE_FRONT_NEED_TARGET = true", C.PLACE_FRONT_NEED_TARGET == true,
      C.PLACE_FRONT_NEED_TARGET)
check("default PLACE_FRONT_TARGET_RANGE = 8 (PILL_FIRE_RANGE)",
      C.PLACE_FRONT_TARGET_RANGE == 8 and C.PLACE_FRONT_TARGET_RANGE == C.PILL_FIRE_RANGE,
      C.PLACE_FRONT_TARGET_RANGE)
check("KEEL PLACE_FRONT_NEED_TARGET = false", C.PRESETS.keel.PLACE_FRONT_NEED_TARGET == false,
      C.PRESETS.keel.PLACE_FRONT_NEED_TARGET)

-- ── 1. the stub front line ───────────────────────────────────────────────
print("classify (stub)")
check("(100,100) is front by classify", PP.classify(100, 100, false, true) == "front",
      PP.classify(100, 100, false, true))
check("(99,100) is front by classify", PP.classify(99, 100, false, true) == "front",
      PP.classify(99, 100, false, true))
check("(100,210) unclaimed is front by classify (FRONT_ZERO_IS_FRONT)",
      PP.classify(100, 210, false, true) == "front", PP.classify(100, 210, false, true))

-- ── 2. place_classify: what counts, and the range ────────────────────────
print("pill_portfolio.place_classify")
local E = { pills = {}, bases = {} }
check("no target: front spot -> back", cls(E, 100, 100) == "back", cls(E, 100, 100))
check("enemy base at 7.9 counts", cls({ pills = {}, bases = { [1] = base(107.9, 100, "hostile") } }, 100, 100) == "front")
check("enemy base at 8.1 does not", cls({ pills = {}, bases = { [1] = base(108.1, 100, "hostile") } }, 100, 100) == "back")
check("enemy base at 8.0 (inclusive) counts", cls({ pills = {}, bases = { [1] = base(108, 100, "hostile") } }, 100, 100) == "front")
check("enemy base at sqrt(65)=8.06 does not", cls({ pills = {}, bases = { [1] = base(107, 104, "hostile") } }, 100, 100) == "back")
check("enemy base at (105,105) 7.07 counts (Euclidean)", cls({ pills = {}, bases = { [1] = base(105, 105, "hostile") } }, 100, 100) == "front")
check("enemy pill counts", cls({ pills = { [1] = pill(104, 100, "hostile") }, bases = {} }, 100, 100) == "front")
check("neutral pill counts", cls({ pills = { [1] = pill(104, 100, "neutral") }, bases = {} }, 100, 100) == "front")
check("friendly pill does not", cls({ pills = { [1] = pill(104, 100, "friendly") }, bases = {} }, 100, 100) == "back")
check("allied pill does not", cls({ pills = { [1] = pill(104, 100, "allied") }, bases = {} }, 100, 100) == "back")
check("in_tank enemy pill does not", cls({ pills = { [1] = pill(104, 100, "hostile", true) }, bases = {} }, 100, 100) == "back")
check("enemy pill with no position does not", cls({ pills = { [1] = { owner = "hostile", health = 15 } }, bases = {} }, 100, 100) == "back")
check("neutral base does not", cls({ pills = {}, bases = { [1] = base(104, 100, "neutral") } }, 100, 100) == "back")
check("friendly base does not", cls({ pills = {}, bases = { [1] = base(104, 100, "friendly") } }, 100, 100) == "back")

-- The class a front spot with no target gets.
check("negative front tile -> aggro", cls(E, 99, 100) == "aggro", cls(E, 99, 100))
check("unclaimed (0) tile -> beyond", cls(E, 100, 210) == "beyond", cls(E, 100, 210))
check("back tile stays back", cls(E, 110, 100) == "back", cls(E, 110, 100))
check("aggro tile stays aggro", cls(E, 90, 100) == "aggro", cls(E, 90, 100))

-- The extra returns (for the log).
do
  local W = { pills = { [7] = pill(112, 100, "hostile") }, bases = { [3] = base(100, 110, "hostile") } }
  C.PLACE_FRONT_NEED_TARGET = true
  local c, inf, lost, e, d = PP.place_classify(100, 100, PP.front_targets(W))
  check("lost flag + nearest base#3 @10", c == "back" and lost == true and e and e[3] == "base"
        and e[4] == 3 and math.abs(d - 10) < 1e-9,
        string.format("%s %s %s %s", tostring(c), tostring(lost), e and (e[3] .. "#" .. e[4]) or "nil", tostring(d)))
  local c2, _, lost2 = PP.place_classify(100, 100, PP.front_targets({ pills = { [1] = pill(104, 100, "hostile") }, bases = {} }))
  check("kept front: lost flag nil", c2 == "front" and lost2 == nil, tostring(lost2))
end

-- ── 3. knob false keeps the old class ────────────────────────────────────
print("knob false")
C.PLACE_FRONT_NEED_TARGET = false
check("front_targets is nil", PP.front_targets(E) == nil)
local same = true
for _, p in ipairs({ { 100, 100 }, { 99, 100 }, { 100, 210 }, { 110, 100 }, { 90, 100 } }) do
  if PP.place_classify(p[1], p[2], PP.front_targets(E)) ~= PP.classify(p[1], p[2], false, true) then
    same = false
  end
end
check("every class equals classify()", same)
check("front spot with no target stays front", PP.place_classify(100, 100, PP.front_targets(E)) == "front")
C.PLACE_FRONT_NEED_TARGET = true

-- ── 4. the eval scan ─────────────────────────────────────────────────────
print("eval_place_pill_strategic scan")
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
local function has_log_lines()
  local fh = io.open("goals.lua", "r")
  if not fh then return false end
  local src = fh:read("*a"); fh:close()
  return src:find('print2(string.format("PLACE_FRONT_NOTARGET', 1, true) ~= nil
end
local function log_has(pat)
  for _, l in ipairs(LOG) do if l:find(pat, 1, true) then return l end end
  return nil
end
if eval_pps then
  PP.set_targets(0, 1, 0, 0, "test")         -- every pill is wanted as front
  -- A hostile pill at (91,107): the front tiles (x = 99/100) within 8 of it
  -- are (99,107) only. The tank sits at (100,100).
  local WP = { pills = { [1] = pill(91, 107, "hostile") }, bases = {} }
  local function run(W, knob)
    C.PLACE_FRONT_NEED_TARGET = knob
    for k in pairs(LOG) do LOG[k] = nil end
    local ok, res = pcall(eval_pps, fresh_state(), W, info_at(100, 100), 100, 100, false, 40, nil)
    C.PLACE_FRONT_NEED_TARGET = true
    if not ok then print("    error: " .. tostring(res)) end
    return ok and res or nil
  end
  local function d_to(res, x, y)
    if not res or not res.goal then return nil end
    local dx, dy = res.goal.mx - x, res.goal.my - y
    return math.sqrt(dx * dx + dy * dy)
  end
  -- Knob on: the winner covers the hostile pill.
  local r_on = run(WP, true)
  local don = d_to(r_on, 91, 107)
  check("knob on: winner within 8 of the hostile pill", don and don <= 8,
        r_on and r_on.goal and string.format("(%d,%d) d=%.2f", r_on.goal.mx, r_on.goal.my, don) or "no goal")
  if has_log_lines() then
    local l = log_has("PLACE_FRONT_NOTARGET")
    check("knob on: PLACE_FRONT_NOTARGET written", l ~= nil, l)
    if l then print("    " .. l) end
    check("log names the nearest target", l and l:find("nearest=pill#1@", 1, true) ~= nil, l)
  else
    print("  skip (stripped goals.lua: no log lines)")
  end
  -- Knob off: the old scan picks a front tile near the tank, far from the pill.
  local r_off = run(WP, false)
  local doff = d_to(r_off, 91, 107)
  check("knob off: winner is the old one (not near the pill)", doff and doff > 8,
        r_off and r_off.goal and string.format("(%d,%d) d=%.2f", r_off.goal.mx, r_off.goal.my, doff) or "no goal")
  check("knob off: no PLACE_FRONT_NOTARGET", log_has("PLACE_FRONT_NOTARGET") == nil)
  -- Knob on, no target at all: no front spot exists, nothing is placed as
  -- front (back/aggro are full at target 0), so there is no goal.
  local r_none = run({ pills = {}, bases = {} }, true)
  check("knob on, no target: no placement", r_none == nil or r_none.goal == nil,
        r_none and r_none.goal and string.format("(%d,%d)", r_none.goal.mx, r_none.goal.my))
  if has_log_lines() then
    check("knob on, no target: PLACE_NO_SPOT", log_has("PLACE_NO_SPOT") ~= nil)
    local l = log_has("PLACE_FRONT_NOTARGET")
    check("knob on, no target: nearest=none", l and l:find("nearest=none", 1, true) ~= nil, l)
  end
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
