-- =========================================================================
-- test_refuel_notlow.lua — standalone unit tests (2026-11-04) for the
-- NOT-LOW REFUEL MARKUP (goals.refuel_notlow_mult; knob
-- C.REFUEL_NOT_LOW_MULT):
--   * the knob is 5.0 and the KEEL preset sets 1.0;
--   * the multiplier: x5 when armour > ARMOUR_LOW and shells > SHELLS_LOW,
--     x1 when either is low, x1 exactly at each threshold (the low test is
--     <=, the same as refuel_need), x1 for any tank when the knob is 1.0;
--   * the pool-1 finalize cost (finalize_pools): a not-low tank pays 5x the
--     knob-1.0 cost, a low tank pays the same, and at knob 1.0 the cost is
--     the old max(bscore x urgency, REFUEL_MIN_COST);
--   * the eval_refuel cost (fill_pool_cache) follows the same rule;
--   * the pool-1 desc names the term as " x5.00 notlow(arm=40,sh=35)" and
--     has no term at x1.
-- No engine: the path cost is a stub (every tile costs 100).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_refuel_notlow.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_refuel_notlow.lua
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
-- The pool panel traces a path for display; no route without the engine.
_G.cpf_dijkstra_next_step = _G.cpf_dijkstra_next_step or function() return nil end
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
local function near(a, b) return type(a) == "number" and type(b) == "number" and math.abs(a - b) <= 1e-6 end

local K = C.REFUEL_NOT_LOW_MULT
local TOPOFF = C.REFUEL_TOPOFF_CANDIDATE
local AL, SL = C.ARMOUR_LOW, C.SHELLS_LOW

-- ── knobs ────────────────────────────────────────────────────────────────
check("knob REFUEL_NOT_LOW_MULT 5.0", K == 5.0, K)
check("KEEL sets 1.0", C.PRESETS and C.PRESETS.keel
      and C.PRESETS.keel.REFUEL_NOT_LOW_MULT == 1.0,
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.REFUEL_NOT_LOW_MULT)
check("ARMOUR_LOW 15", AL == 15, AL)
check("SHELLS_LOW 19", SL == 19, SL)

-- ── the multiplier ───────────────────────────────────────────────────────
local M = G.refuel_notlow_mult
check("not low both (40,35): x5", M(40, 35) == 5.0, M(40, 35))
check("armour low only (10,35): x1", M(10, 35) == 1.0, M(10, 35))
check("shells low only (40,10): x1", M(40, 10) == 1.0, M(40, 10))
check("both low (10,10): x1", M(10, 10) == 1.0, M(10, 10))
check("armour AT ARMOUR_LOW (15,35): x1 (<= is low)", M(AL, 35) == 1.0, M(AL, 35))
check("armour one above (16,35): x5", M(AL + 1, 35) == 5.0, M(AL + 1, 35))
check("shells AT SHELLS_LOW (40,19): x1 (<= is low)", M(40, SL) == 1.0, M(40, SL))
check("shells one above (40,20): x5", M(40, SL + 1) == 5.0, M(40, SL + 1))
check("both one above (16,20): x5", M(AL + 1, SL + 1) == 5.0, M(AL + 1, SL + 1))
C.REFUEL_NOT_LOW_MULT = 1.0
check("knob 1.0: (40,35) x1", M(40, 35) == 1.0, M(40, 35))
C.REFUEL_NOT_LOW_MULT = K
check("term x5", G.refuel_notlow_term(5.0, 40, 35) == " x5.00 notlow(arm=40,sh=35)",
      G.refuel_notlow_term(5.0, 40, 35))
check("term x1 is empty", G.refuel_notlow_term(1.0, 40, 35) == "")

-- ── pool-1 cost through build/step_eval_queue + finalize_pools ──────────
-- One friendly base 10 tiles from the tank. A not-low tank only queues
-- refuel with the top-off candidacy on, so the test turns that on (it does
-- not change the price: below target the fill ramp is a separate term that
-- goal_selection applies later).
local function world_with_base()
  return { pills = {}, tanks = {}, bases = {
    [1] = { mx = 136, my = 121, owner = "friendly", armour = 90, shells = 40,
            obs_armour = 90, obs_shells = 40, obs_tick = 50150, last_seen = 50150 },
  } }
end
local function mkstate()
  return { tick = 50153, cost_cache = {}, armour_target = 40, shell_target = 40,
    goal = { kind = "none" },
    perc = { friendly_pills_damaged = 0, attackable_pill_count = 0, neutral_base_count = 0,
             hostile_base_count = 0, friendly_base_count = 1, enemy_tanks = {},
             dead_neutral_pill_count = 0, threat_at_tank = 0 } }
end
local function mkinfo(arm, sh)
  return { tankx = 146 * 256, tanky = 121 * 256, man_status = C.LGM_INTANK,
    trees = 5, shells = sh, mines = 0, armour = arm, player_number = 0 }
end

local function run_finalize(arm, sh)
  C.REFUEL_TOPOFF_CANDIDATE = true
  local world, state, info = world_with_base(), mkstate(), mkinfo(arm, sh)
  local ok, err = pcall(G.build_eval_queue, state, world, info)
  if ok then
    for _ = 1, 40 do
      ok, err = pcall(G.step_eval_queue, state, world, info)
      if not ok then break end
    end
  end
  if ok then ok, err = pcall(G.finalize_pools, state, world, info) end
  C.REFUEL_TOPOFF_CANDIDATE = TOPOFF
  if not ok then return nil, err end
  local pc1 = state.pool_cache and state.pool_cache[1]
  if not pc1 then return nil, "no pool_cache[1]" end
  return pc1, state
end

local function run_eval(arm, sh)
  C.REFUEL_TOPOFF_CANDIDATE = true
  local world, state, info = world_with_base(), mkstate(), mkinfo(arm, sh)
  local ok, err = pcall(G.fill_pool_cache, state, world, info)
  C.REFUEL_TOPOFF_CANDIDATE = TOPOFF
  if not ok then return nil, err end
  local pc1 = state.pool_cache and state.pool_cache[1]
  if not pc1 then return nil, "no pool_cache[1]" end
  return pc1, state
end

_G.BRAIN_POOL_VIZ = true
for _, path in ipairs({ { "finalize", run_finalize }, { "eval_refuel", run_eval } }) do
  local pname, run = path[1], path[2]
  local on, err = run(40, 35)
  if not on then
    check(pname .. ": runs", false, err)
  else
    C.REFUEL_NOT_LOW_MULT = 1.0
    local off, st_off = run(40, 35)
    local offA = run(10, 35)
    local offS = run(40, 10)
    local offT = run(AL, 35)
    local offT2 = run(40, SL)
    C.REFUEL_NOT_LOW_MULT = K
    local onA = run(10, 35)
    local onS = run(40, 10)
    local onT = run(AL, 35)
    local onT2 = run(40, SL)
    local onU = run(AL + 1, SL + 1)
    C.REFUEL_NOT_LOW_MULT = 1.0
    local offU = run(AL + 1, SL + 1)
    C.REFUEL_NOT_LOW_MULT = K

    check(pname .. " not low both: x5", off and near(on.cost, 5.0 * off.cost),
          string.format("%s vs %s", tostring(on.cost), tostring(off and off.cost)))
    check(pname .. " armour low only: x1", onA and offA and onA.cost == offA.cost,
          onA and offA and (onA.cost .. " vs " .. offA.cost))
    check(pname .. " shells low only: x1", onS and offS and onS.cost == offS.cost,
          onS and offS and (onS.cost .. " vs " .. offS.cost))
    check(pname .. " armour at ARMOUR_LOW: x1", onT and offT and onT.cost == offT.cost,
          onT and offT and (onT.cost .. " vs " .. offT.cost))
    check(pname .. " shells at SHELLS_LOW: x1", onT2 and offT2 and onT2.cost == offT2.cost,
          onT2 and offT2 and (onT2.cost .. " vs " .. offT2.cost))
    check(pname .. " one above both: x5", onU and offU and near(onU.cost, 5.0 * offU.cost),
          onU and offU and (onU.cost .. " vs " .. offU.cost))

    -- Knob 1.0 is the old cost: max(bscore x urgency, REFUEL_MIN_COST), with
    -- urgency 1.0 at (40,35). bscore is the base's cached pool-1 score.
    if pname == "finalize" then
      local ce = st_off and st_off.cost_cache and st_off.cost_cache["1:1"]
      local old = ce and math.max(ce.cost * 1.0, C.REFUEL_MIN_COST)
      check(pname .. " knob 1.0 = old cost", off and old and off.cost == old,
            off and old and (off.cost .. " vs " .. old))
    end
    -- At knob 1.0 the desc is the old format: no term.
    check(pname .. " knob 1.0 desc: no notlow term", off and not off.desc:find("notlow", 1, true),
          off and off.desc)
    check(pname .. " desc names x5.00 notlow(arm=40,sh=35)",
          on.desc:find(" x5.00 notlow(arm=40,sh=35)=", 1, true) ~= nil, on.desc)
    -- The desc reads score x urgency = pre x5.00 notlow(...) = cost.
    local pre, post = on.desc:match("=(%d+) x5%.00 notlow%(arm=40,sh=35%)=(%d+)")
    check(pname .. " desc pre x 5 = post = cost",
          pre and post and tonumber(post) == math.floor(on.cost + 0.5)
          and math.abs(tonumber(pre) * 5 - tonumber(post)) <= 5, on.desc)
    check(pname .. " desc low armour: no notlow term", onA and not onA.desc:find("notlow", 1, true),
          onA and onA.desc)
  end
end
_G.BRAIN_POOL_VIZ = nil

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
