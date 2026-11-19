-- =========================================================================
-- test_repair_lowdmg.lua — standalone unit tests (2026-11-04) for the
-- LOW-DAMAGE MARKUP on the repair_pill goal cost (goals.repair_lowdmg_mult;
-- knobs C.REPAIR_LOW_DMG_MULT_MAX, C.REPAIR_LOW_DMG_MULT_AT5,
-- C.REPAIR_LOW_DMG_MAX):
--   * the curve for dmg 1..7 with the approved knobs (4.00, 2.28, 1.55,
--     1.23, 1.10, 1.00, 1.00);
--   * MAX 1.0 is off; the guards (LOW_MAX < 2, AT5 <= 1, dmg 0) give no
--     NaN and no division by zero;
--   * the KEEL preset turns it off;
--   * a dead pill (dmg = full HP) is x1;
--   * the pool-5 cost from step_eval_queue: a pill missing 1 HP costs 4x
--     what it did, a pill missing 10 HP is unchanged, and the cost_cache
--     entry carries _lowdmg_mult;
--   * the pool-5 desc names the term as " x4.00 lowdmg(dmg=1)".
-- No engine: the path cost is a stub (every tile costs 100).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_repair_lowdmg.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_repair_lowdmg.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

-- The C threat grids are not loaded here: every lookup reads 0.
_G.gh_threat = _G.gh_threat or setmetatable({}, { __index = function()
  return function() return 0 end
end })
_G.clock_us = _G.clock_us or function() return os.clock() * 1e6 end
-- Every path costs 100 (no Dijkstra slates without the engine).
local PATH = 100
_G.cpf_dijkstra_lookup_by_kind = function() return PATH end
_G.cpf_influence_at = _G.cpf_influence_at or function() return 0 end
_G.TERRAIN_MASK, _G.TERRAIN_MINE_FLAG = 0x0F, 0x40
_G.get_terrain = function() return 7 end  -- grass everywhere

local C = require("constants")
local G = require("goals")

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
local function near(a, b) return type(a) == "number" and math.abs(a - b) <= 0.01 end
local function finite(x) return type(x) == "number" and x == x and x > -1e300 and x < 1e300 end

local K_MAX, K_AT5, K_LO = C.REPAIR_LOW_DMG_MULT_MAX, C.REPAIR_LOW_DMG_MULT_AT5, C.REPAIR_LOW_DMG_MAX
local function restore()
  C.REPAIR_LOW_DMG_MULT_MAX, C.REPAIR_LOW_DMG_MULT_AT5, C.REPAIR_LOW_DMG_MAX = K_MAX, K_AT5, K_LO
end

-- ── knobs ────────────────────────────────────────────────────────────────
check("knob MAX 4.0", K_MAX == 4.0, K_MAX)
check("knob AT5 1.10", K_AT5 == 1.10, K_AT5)
check("knob LOW_MAX 5", K_LO == 5, K_LO)
check("KEEL off (MAX 1.0)", C.PRESETS and C.PRESETS.keel
      and C.PRESETS.keel.REPAIR_LOW_DMG_MULT_MAX == 1.0,
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.REPAIR_LOW_DMG_MULT_MAX)

-- ── the curve ────────────────────────────────────────────────────────────
local WANT = { 4.00, 2.28, 1.55, 1.23, 1.10, 1.00, 1.00 }
for dmg = 1, 7 do
  local m = G.repair_lowdmg_mult(dmg)
  check(string.format("curve dmg %d = x%.2f", dmg, WANT[dmg]), near(m, WANT[dmg]), m)
end
check("curve dmg 6+ exactly 1", G.repair_lowdmg_mult(6) == 1 and G.repair_lowdmg_mult(14) == 1)
check("curve dmg 0 (full pill) is 1", G.repair_lowdmg_mult(0) == 1)
check("curve dmg nil is 1", G.repair_lowdmg_mult(nil) == 1)
-- Dead pill: the pool-5 call sits inside `health > 0`, so a rebuild row never
-- reads the curve. (A dead pill only reaches pool 5 when capture_pill rejects
-- it, which this stub world does not make.) Its dmg is the full HP anyway:
check("curve dmg 15 (dead pill) is 1", G.repair_lowdmg_mult(C.PILLS_MAX_HEALTH or 15) == 1)

-- ── off and the guards ───────────────────────────────────────────────────
C.REPAIR_LOW_DMG_MULT_MAX = 1.0
local all1 = true
for dmg = 0, 15 do if G.repair_lowdmg_mult(dmg) ~= 1 then all1 = false end end
check("MAX 1.0: x1 for every dmg", all1)
C.REPAIR_LOW_DMG_MULT_MAX = 0.5
check("MAX < 1: x1", G.repair_lowdmg_mult(1) == 1)
restore()
C.REPAIR_LOW_DMG_MAX = 1
check("LOW_MAX 1: dmg 1 gets MAX", G.repair_lowdmg_mult(1) == 4.0, G.repair_lowdmg_mult(1))
check("LOW_MAX 1: dmg 2 x1", G.repair_lowdmg_mult(2) == 1)
C.REPAIR_LOW_DMG_MAX = 0
check("LOW_MAX 0: dmg 1 x1", G.repair_lowdmg_mult(1) == 1)
restore()
C.REPAIR_LOW_DMG_MULT_AT5 = 0.5
local m1, m2 = G.repair_lowdmg_mult(1), G.repair_lowdmg_mult(2)
check("AT5 < 1: dmg 1 MAX, finite", m1 == 4.0 and finite(m1), m1)
check("AT5 < 1: dmg 2 x1, finite (no NaN)", m2 == 1 and finite(m2), m2)
C.REPAIR_LOW_DMG_MULT_AT5 = 1.0
check("AT5 1.0: dmg 3 x1", G.repair_lowdmg_mult(3) == 1, G.repair_lowdmg_mult(3))
restore()

-- ── pool-5 cost through step_eval_queue ──────────────────────────────────
local function run(hp)
  local world = { pills = {
    [7] = { mx = 134, my = 111, owner = "friendly", health = hp, last_seen = 50150 },
  }, bases = {}, tanks = {} }
  local state = { tick = 50153, cost_cache = {},
    perc = { friendly_pills_damaged = 1, attackable_pill_count = 0, neutral_base_count = 0,
             hostile_base_count = 0, enemy_tanks = {}, threat_at_tank = 0 } }
  local info = { tankx = 146 * 256, tanky = 121 * 256, man_status = C.LGM_INTANK,
    trees = 5, shells = 40, mines = 0, armour = 40, player_number = 0 }
  local ok, err = pcall(G.build_eval_queue, state, world, info)
  if not ok then return nil, err end
  for _ = 1, 40 do
    ok, err = pcall(G.step_eval_queue, state, world, info)
    if not ok then return nil, err end
  end
  return state.cost_cache["5:7"], state, world, info
end

local MAXHP = C.PILLS_MAX_HEALTH or 15
local e1, err1 = run(MAXHP - 1)
if not e1 then
  check("pool 5: step_eval_queue runs", false, err1)
else
  C.REPAIR_LOW_DMG_MULT_MAX = 1.0
  local off1 = run(MAXHP - 1)
  local off10 = run(MAXHP - 10)
  restore()
  local on10 = run(MAXHP - 10)
  local base1 = (C.REPAIR_BASE_COST or 30) + math.max(0, PATH - 1 * C.REPAIR_DAMAGE_BONUS)
  check("pool 5 dmg 1, off: base + path - dam", off1 and near(off1.cost, base1), off1 and off1.cost)
  check("pool 5 dmg 1: 4x the off cost", near(e1.cost, 4.0 * off1.cost), e1.cost)
  check("pool 5 dmg 1: entry _lowdmg_mult 4.0", near(e1._lowdmg_mult, 4.0), e1._lowdmg_mult)
  check("pool 5 dmg 1 off: no _lowdmg_mult", off1._lowdmg_mult == nil, off1._lowdmg_mult)
  check("pool 5 dmg 10: unchanged", on10 and off10 and on10.cost == off10.cost,
        on10 and on10.cost)
  check("pool 5 dmg 10: no _lowdmg_mult", on10 and on10._lowdmg_mult == nil)

  -- desc5: the term is printed with its dmg, so the breakdown is complete.
  _G.BRAIN_POOL_VIZ = true
  local e, state, world, info = run(MAXHP - 1)
  local ok, perr = pcall(G.finalize_pools, state, world, info)
  local pc5 = ok and state.pool_cache and state.pool_cache[5]
  local d = pc5 and pc5.desc or (ok and "no pool_cache[5]" or tostring(perr))
  check("desc5 names x4.00 lowdmg(dmg=1)", pc5 and d:find(" x4.00 lowdmg(dmg=1)", 1, true) ~= nil, d)
  check("desc5 cost is the marked-up cost", pc5 and near(pc5.cost, e.cost), pc5 and pc5.cost)
  e, state, world, info = run(MAXHP - 10)
  ok, perr = pcall(G.finalize_pools, state, world, info)
  pc5 = ok and state.pool_cache and state.pool_cache[5]
  d = pc5 and pc5.desc or (ok and "no pool_cache[5]" or tostring(perr))
  check("desc5 dmg 10: no lowdmg term", pc5 and not d:find("lowdmg", 1, true), d)
  _G.BRAIN_POOL_VIZ = nil
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
