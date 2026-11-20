-- =========================================================================
-- test_repair_boost.lua — standalone unit tests (2026-11-19, board #241) for
-- the two repair knobs in constants.lua:
--   * REPAIR_BOOST: the pool-5 repair_pill cost x REPAIR_BOOST_GOAL_MULT
--     (0.8), the cost_cache entry carries _rp_boost, desc5 names the term;
--     the builder pool topup / rebuild score x REPAIR_BOOST_BUILDER_MULT
--     (1.2), row.boost_w, score_terms and row_formula show the factor;
--   * REPAIR_SMALL_TOPUP: builder_pool.topup_min_missing() is
--     BUILDER_POOL_TOPUP_MIN_MISSING_SMALL (2) when on, 4 when off; discover
--     makes a topup row from 2 missing hp; builder_can_repair refuses a live
--     pill below that minimum (no hand-off to a pool that will not take it);
--   * both knobs are false in PRESETS.keel and PRESETS.mast.
-- No engine: path costs are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_repair_boost.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local threat = require("threat")
local danger = require("danger")
local squad  = require("squad")
local ally_state = require("ally_state")
local G      = require("goals")
local BP     = require("builder_pool")

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
local function near(a, b) return type(a) == "number" and type(b) == "number" and math.abs(a - b) <= 0.01 end

-- ── Engine stand-ins (as test_spike.lua) ─────────────────────────────────
cpf.influence_at   = function() return 20 end
cpf.smart_cost_dij_only = function() return 10 end
U.is_placeable     = function() return true end
U.los_coverage     = function() return 0 end
U.ttype            = function() return 4 end
threat.at          = function() return 0 end
threat.coverage_at = function() return 0 end
threat.pill_at     = function() return 0 end
danger.should_panic_build = function() return false, 0, "test" end
danger.tank_fire_age = function() return nil, nil end
squad.blitz_shot_lines = function() return nil end
_G.cpf_dijkstra_lookup_by_kind = function() return 20 end
_G.cpf_estimate_tank_travel_ticks = function() return 50 end
_G.KIND_NORMAL = 0
ally_state.init()

local function pill(mx, my, owner, hp)
  return { mx = mx, my = my, owner = owner, health = hp or 15 }
end
local function info_at(mx, my, trees)
  return { tankx = mx * 256 + 128, tanky = my * 256 + 128,
           carried_pills = 0, man_status = C.LGM_INTANK, inboat = false,
           armour = 40, shells = 40, trees = trees or 40, mines = 0, player_number = 0 }
end
local function fresh_state(tick)
  return { tick = tick or 3000, perc = { enemy_tanks = {} },
           _spike = { pair = nil, watch = {}, care = {}, cool = {} } }
end

-- ── 1. knobs and presets ─────────────────────────────────────────────────
print("knobs")
check("REPAIR_BOOST default true", C.REPAIR_BOOST == true)
check("REPAIR_BOOST_GOAL_MULT 0.8", C.REPAIR_BOOST_GOAL_MULT == 0.8, C.REPAIR_BOOST_GOAL_MULT)
check("REPAIR_BOOST_BUILDER_MULT 1.2", C.REPAIR_BOOST_BUILDER_MULT == 1.2, C.REPAIR_BOOST_BUILDER_MULT)
check("REPAIR_SMALL_TOPUP default false (Nov 20 bench)", C.REPAIR_SMALL_TOPUP == false)
check("HUMAN_ALLY_BOOST_TEST_FORCE default false", C.HUMAN_ALLY_BOOST_TEST_FORCE == false)
check("BUILDER_POOL_TOPUP_MIN_MISSING_SMALL 2", C.BUILDER_POOL_TOPUP_MIN_MISSING_SMALL == 2)
check("BUILDER_POOL_TOPUP_MIN_MISSING still 4", C.BUILDER_POOL_TOPUP_MIN_MISSING == 4)
for _, pn in ipairs({ "keel", "mast" }) do
  local p = C.PRESETS and C.PRESETS[pn]
  check(pn .. ": REPAIR_BOOST false", p and p.REPAIR_BOOST == false)
  -- Default false, so the presets leave them out (test_preset_mast: no
  -- entry may equal the default).
  check(pn .. ": REPAIR_SMALL_TOPUP not true", p and p.REPAIR_SMALL_TOPUP ~= true)
  check(pn .. ": HUMAN_ALLY_BOOST_TEST_FORCE not true", p and p.HUMAN_ALLY_BOOST_TEST_FORCE ~= true)
end

-- ── 1b. the human-ally gate ─────────────────────────────────────────────
print("human-ally gate")
do
  -- me = slot 0; slot 1 is an ally. player_bots marks which slots are bots.
  local bots_only = { player_number = 0, allies = 0x3, player_bots = 0x3 }
  local with_human = { player_number = 0, allies = 0x3, player_bots = 0x1 }
  check("gate off: no human ally", U.human_ally_boost(bots_only) == false)
  check("gate off: no info", U.human_ally_boost(nil) == false)
  check("gate on: one human ally", U.human_ally_boost(with_human) == true)
  C.HUMAN_ALLY_BOOST_TEST_FORCE = true
  check("gate on: force knob, no human", U.human_ally_boost(bots_only) == true)
  C.HUMAN_ALLY_BOOST_TEST_FORCE = false
  -- Bot-only: the knobs on, but no factor and the old topup minimum.
  C.REPAIR_SMALL_TOPUP = true
  check("bot-only: topup_min_missing 4 with the knob on", BP.topup_min_missing(bots_only) == 4)
  check("human ally: topup_min_missing 2", BP.topup_min_missing(with_human) == 2)
  C.REPAIR_SMALL_TOPUP = false
end
-- The sections below run with the force knob on and both knobs on, to
-- check the factors themselves.
C.HUMAN_ALLY_BOOST_TEST_FORCE = true
C.REPAIR_SMALL_TOPUP = true

-- ── 2. REPAIR_SMALL_TOPUP ────────────────────────────────────────────────
print("REPAIR_SMALL_TOPUP")
C.BUILDER_POOL_REPAIR_LINEAR = true
local real_trip, real_fd = BP.lgm_trip, BP.front_distance
BP.lgm_trip = function() return 50, 100, nil end
BP.front_distance = function() return 5 end
check("on: topup_min_missing 2", BP.topup_min_missing() == 2, BP.topup_min_missing())
do
  local st = fresh_state()
  local inf = info_at(95, 104)                 -- 7 tiles from the pill
  local function rows_for(hp)
    return BP.discover(st, { pills = { [7] = pill(102, 104, "friendly", hp) }, bases = {} }, inf)
  end
  local function can(hp)
    local w = { pills = { [7] = pill(102, 104, "friendly", hp) }, bases = {} }
    return BP.builder_can_repair(st, w, inf, w.pills[7])
  end
  local r13 = rows_for(13)[1]
  check("on: 2 missing -> topup row", r13 and r13.type == "topup" and r13.missing == 2,
        r13 and r13.type)
  check("on: 1 missing -> no row", #rows_for(14) == 0, #rows_for(14))
  check("on: builder_can_repair 2 missing -> yes", can(13) ~= nil)
  check("on: builder_can_repair 1 missing -> nil (below the pool minimum)", can(14) == nil)
  check("on: builder_can_repair dead pill -> yes", can(0) ~= nil)
  C.REPAIR_SMALL_TOPUP = false
  check("off: topup_min_missing 4", BP.topup_min_missing() == 4, BP.topup_min_missing())
  check("off: 3 missing -> no row", #rows_for(12) == 0, #rows_for(12))
  check("off: 4 missing -> topup row", rows_for(11)[1] and rows_for(11)[1].type == "topup")
  check("off: builder_can_repair 1 missing -> yes (old hand-off)", can(14) ~= nil)
  C.REPAIR_SMALL_TOPUP = true
  -- Spike care keeps its own minimum (SPIKE_REPAIR_MIN_MISSING).
  C.FOCUS_SPIKE_ENABLED, C.SPIKE_REPAIR_PRIORITY = true, true
  st._spike.care[104 * 256 + 102] = { mx = 102, my = 104, t = 2900, src = "plan" }
  local rs = rows_for(14)[1]
  check("spike care: 1 missing -> topup row (spike minimum)", rs and rs.spike == true, rs and rs.type)
  check("spike care: builder_can_repair 1 missing -> yes", can(14) ~= nil)
end

-- ── 3. REPAIR_BOOST on the builder pool ──────────────────────────────────
print("REPAIR_BOOST builder pool")
do
  local st = fresh_state()
  local inf = info_at(95, 104)
  local w = { pills = { [7] = pill(102, 104, "friendly", 11) }, bases = {} }
  local ctx = { ok = true, reserve = 0 }
  local function scored()
    local r = BP.discover(st, w, inf)[1]
    BP.score_row(st, w, inf, 3000, r, ctx)
    return r
  end
  C.REPAIR_BOOST = false
  local off = scored()
  check("off: score 30x4 - 0.25x100 = 95", off and near(off.score, 95), off and off.score)
  check("off: no boost_w", off and off.boost_w == nil)
  check("off: score_terms has no repair_boost", off and not BP.score_terms(off):find("repair_boost", 1, true))
  C.REPAIR_BOOST = true
  local on = scored()
  check("on: score 95 x 1.2 = 114", on and near(on.score, 114), on and on.score)
  check("on: raw_score 95", on and near(on.raw_score, 95), on and on.raw_score)
  check("on: boost_w 1.2", on and on.boost_w == 1.2, on and on.boost_w)
  local terms = on and BP.score_terms(on) or ""
  check("on: score_terms bp_raw{95} x repair_boost{1.20}",
        terms:find("= bp_raw{95} x repair_boost{1.20}", 1, true) ~= nil, terms)
  check("on: score_terms has no goal_w{} (weight 1)", not terms:find("goal_w{", 1, true), terms)
  local form = on and BP.row_formula(on) or ""
  check("on: row_formula chain bp_raw(95) x repair_boost(1.20) = 114",
        form:find("bp_raw(95) x repair_boost(1.20) = 114", 1, true) ~= nil, form)
  check("on: row_formula repair_boost segment",
        form:find("repair_boost:REPAIR_BOOST_BUILDER_MULT(1.20)", 1, true) ~= nil, form)
  -- Dead pill: rebuild row gets the same factor.
  local wd = { pills = { [7] = pill(102, 104, "friendly", 0) }, bases = {} }
  local rd = BP.discover(st, wd, inf)[1]
  BP.score_row(st, wd, inf, 3000, rd, ctx)
  check("rebuild row: boost_w 1.2", rd and rd.type == "rebuild" and rd.boost_w == 1.2,
        rd and rd.type)
  check("rebuild row: score = raw x 1.2", rd and near(rd.score, (rd.raw_score or 0) * 1.2),
        rd and rd.score)
end
BP.lgm_trip, BP.front_distance = real_trip, real_fd

-- ── 4. REPAIR_BOOST on pool 5 ────────────────────────────────────────────
print("REPAIR_BOOST pool 5")
local PATH = 100
_G.cpf_dijkstra_lookup_by_kind = function() return PATH end
_G.cpf_influence_at = _G.cpf_influence_at or function() return 0 end
_G.TERRAIN_MASK, _G.TERRAIN_MINE_FLAG = 0x0F, 0x40
_G.get_terrain = function() return 7 end
_G.gh_threat = _G.gh_threat or setmetatable({}, { __index = function()
  return function() return 0 end
end })
_G.clock_us = _G.clock_us or function() return os.clock() * 1e6 end
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
C.REPAIR_BOOST = false
local off10, err = run(MAXHP - 10)
C.REPAIR_BOOST = true
local on10 = run(MAXHP - 10)
if not off10 or not on10 then
  check("pool 5: step_eval_queue runs", false, err)
else
  check("pool 5 dmg 10: on = 0.8 x off", near(on10.cost, 0.8 * off10.cost),
        string.format("%s vs %s", tostring(on10.cost), tostring(off10.cost)))
  check("pool 5: entry _rp_boost 0.8", on10._rp_boost == 0.8, on10._rp_boost)
  check("pool 5 off: no _rp_boost", off10._rp_boost == nil, off10._rp_boost)
  _G.BRAIN_POOL_VIZ = true
  local e, state, world, info = run(MAXHP - 10)
  local ok, perr = pcall(G.finalize_pools, state, world, info)
  local pc5 = ok and state.pool_cache and state.pool_cache[5]
  local d = pc5 and pc5.desc or (ok and "no pool_cache[5]" or tostring(perr))
  check("desc5 names x0.80 repair_boost(REPAIR_BOOST_GOAL_MULT, human ally)",
        pc5 and d:find(" x0.80 repair_boost(REPAIR_BOOST_GOAL_MULT, human ally)", 1, true) ~= nil, d)
  check("desc5 cost is the boosted cost", pc5 and near(pc5.cost, e.cost), pc5 and pc5.cost)
  C.REPAIR_BOOST = false
  e, state, world, info = run(MAXHP - 10)
  ok, perr = pcall(G.finalize_pools, state, world, info)
  pc5 = ok and state.pool_cache and state.pool_cache[5]
  d = pc5 and pc5.desc or (ok and "no pool_cache[5]" or tostring(perr))
  check("desc5 off: no repair_boost term", pc5 and not d:find("repair_boost", 1, true), d)
  C.REPAIR_BOOST = true
  _G.BRAIN_POOL_VIZ = nil
end

-- ── 5. bot-only: no factor even with REPAIR_BOOST on ────────────────────
print("bot-only")
C.HUMAN_ALLY_BOOST_TEST_FORCE = false
C.REPAIR_BOOST = true
do
  local e = run(MAXHP - 10)
  check("bot-only pool 5: no _rp_boost", e and e._rp_boost == nil, e and e._rp_boost)
  check("bot-only pool 5: cost = the off cost", e and off10 and near(e.cost, off10.cost), e and e.cost)
  BP.lgm_trip = function() return 50, 100, nil end
  BP.front_distance = function() return 5 end
  local st = fresh_state()
  local inf = info_at(95, 104)
  local w = { pills = { [7] = pill(102, 104, "friendly", 11) }, bases = {} }
  local r = BP.discover(st, w, inf)[1]
  BP.score_row(st, w, inf, 3000, r, { ok = true, reserve = 0 })
  check("bot-only builder pool: no boost_w, score 95", r and r.boost_w == nil and near(r.score, 95), r and r.score)
  BP.lgm_trip, BP.front_distance = real_trip, real_fd
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
