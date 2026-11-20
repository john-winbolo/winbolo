-- =========================================================================
-- test_defend_boost.lua — standalone unit tests (2026-11-20) for DEFEND_BOOST
-- (constants.lua): every defend_pill row cost x DEFEND_BOOST_GOAL_MULT (0.8),
-- only with a human ally on the team (util.human_ally_boost) or the test-only
-- knob HUMAN_ALLY_BOOST_TEST_FORCE:
--   * the knobs and the KEEL / MAST presets;
--   * the alarm pool (DEFEND_ALARM_MODE, the default) and the old pool: the
--     returned cost and every row cost are x0.8 with the gate on, x1 with no
--     human ally or the knob off; the winner is the same pill;
--   * the row formula and the winner desc carry the defend_boost chip.
-- No engine: every path costs 10.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_defend_boost.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

_G.print2 = _G.print2 or function() end
_G.gh_threat = _G.gh_threat or setmetatable({}, { __index = function()
  return function() return 0 end
end })
_G.clock_us = _G.clock_us or function() return os.clock() * 1e6 end
_G.cpf_dijkstra_lookup_by_kind = function() return 10 end
_G.cpf_influence_at = _G.cpf_influence_at or function() return 0 end
_G.TERRAIN_MASK, _G.TERRAIN_MINE_FLAG = 0x0F, 0x40
_G.get_terrain = function() return 7 end
_G.KIND_NORMAL = 0
_G.cpf_dijkstra_trace_path_by_kind = function() return nil end   -- no trace: the "disc" fallback

local C   = require("constants")
local U   = require("util")
local cpf = require("cpathfinder")
local G   = require("goals")
cpf.smart_cost_dij_only = function() return 10 end
cpf.influence_at = function() return 0 end

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

print("knobs")
check("DEFEND_BOOST default true", C.DEFEND_BOOST == true)
check("DEFEND_BOOST_GOAL_MULT 0.8", C.DEFEND_BOOST_GOAL_MULT == 0.8, C.DEFEND_BOOST_GOAL_MULT)
check("keel: DEFEND_BOOST false", C.PRESETS.keel.DEFEND_BOOST == false)
check("mast: DEFEND_BOOST false", C.PRESETS.mast.DEFEND_BOOST == false)

-- me = slot 0, slot 1 is an ally; player_bots says which slots are bots.
local BOTS, HUMAN = 0x3, 0x1
local function run(alarm, bots_mask)
  C.DEFEND_ALARM_MODE = alarm
  local now = 5000
  local world = { pills = {
    [4] = { mx = 120, my = 100, owner = "friendly", health = 12, last_seen = now,
            last_hit_tick = now - 10, attack_damage = 3,
            _alarm_hits = { { t = now - 10, src = "tank" } } },
  }, bases = {}, tanks = {} }
  local state = { tick = now, phase = "middle", cost_cache = {},
    perc = { enemy_tanks = { { id = 9, mx = 125, my = 100 } }, friendly_pills_damaged = 1 } }
  local info = { tankx = U.m2w(100), tanky = U.m2w(100), man_status = C.LGM_INTANK,
    trees = 5, shells = 40, mines = 0, armour = 40, player_number = 0,
    allies = 0x3, player_bots = bots_mask }
  local ok, r = pcall(G._eval_defend_pill, state, world, info, 100, 100, false, 40)
  if not ok then return nil, r end
  return r, state
end

_G.BRAIN_POOL_VIZ = true
for _, alarm in ipairs({ true, false }) do
  local tag = alarm and "alarm" or "old pool"
  print(tag)
  C.DEFEND_BOOST, C.HUMAN_ALLY_BOOST_TEST_FORCE = false, false
  local off, soff = run(alarm, HUMAN)
  C.DEFEND_BOOST = true
  local on, son = run(alarm, HUMAN)
  local bot, sbot = run(alarm, BOTS)
  C.HUMAN_ALLY_BOOST_TEST_FORCE = true
  local frc = run(alarm, BOTS)
  C.HUMAN_ALLY_BOOST_TEST_FORCE = false
  if type(off) ~= "table" or type(on) ~= "table" then
    check(tag .. ": pool returns a row", false, tostring(soff) .. " / " .. tostring(son))
  else
    check(tag .. ": human ally: cost x0.8", near(on.cost, 0.8 * off.cost),
          string.format("%s vs %s", tostring(on.cost), tostring(off.cost)))
    check(tag .. ": same winner", on.goal.target_id == off.goal.target_id)
    check(tag .. ": bot-only: cost unchanged", bot and near(bot.cost, off.cost), bot and bot.cost)
    check(tag .. ": force knob: cost x0.8", frc and near(frc.cost, 0.8 * off.cost), frc and frc.cost)
    check(tag .. ": desc has the chip",
          on.desc:find("*defend_boost{x0.80 DEFEND_BOOST_GOAL_MULT, human ally}", 1, true) ~= nil, on.desc)
    check(tag .. ": bot-only desc has no chip", bot and not bot.desc:find("defend_boost", 1, true), bot and bot.desc)
    local ron = son.defend_breakdown and son.defend_breakdown.rows[1]
    local roff = soff.defend_breakdown and soff.defend_breakdown.rows[1]
    check(tag .. ": row cost x0.8", ron and roff and near(ron.cost, 0.8 * roff.cost),
          ron and ron.cost)
    local head = ron and ron.formula:match("^(.-)||") or ""
    check(tag .. ": row chip before ||", head:find("*defend_boost{x0.80", 1, true) ~= nil, ron and ron.formula)
    check(tag .. ": row sel preview names xdb0.80", head:find("xdb0.80", 1, true) ~= nil, head)
  end
end
_G.BRAIN_POOL_VIZ = nil

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
