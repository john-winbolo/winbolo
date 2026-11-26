-- =========================================================================
-- test_pool6_neutral.lua -- standalone unit tests (2026-11-16) for three
-- knobs that let a bot take a neutral pill in its own ground instead of
-- driving on to a far base:
--   C.POOL6_PICK_WITH_FACTORS: pool 6 (attack_pill) picks its one pill on
--     raw x phase weight x influence x neutral factor, not on raw cost, and
--     keeps the RAW cost for the competition (each factor counted once).
--   C.NEUTRAL_PILL_OWN_GROUND_FACTOR (0.75): a neutral live pill whose
--     square has influence above 50 costs x0.75 more.
--   C.ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT: ATTACK_BASE_COMMITMENT_BONUS
--     counts only after a shell has left the tank at the goal base.
-- cpf.influence_at is stubbed. No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pool6_neutral.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C   = require("constants")
_G.TERRAIN_MASK = 0x0F
_G.TERRAIN_MINE_FLAG = 0x40
_G.get_terrain = function() return C.T_GRASS end

local cpf = require("cpathfinder")
local G   = require("goals")
local S   = require("steering")

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
local function near(a, b) return math.abs(a - b) < 1e-6 end

-- Influence by square; 0 where not set.
local INF = {}
cpf.influence_at = function(mx, my) return INF[my * 256 + mx] or 0 end

local function save_knobs()
  return { C.POOL6_PICK_WITH_FACTORS, C.NEUTRAL_PILL_OWN_GROUND_FACTOR,
           C.ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT }
end
local function set_knobs(t)
  C.POOL6_PICK_WITH_FACTORS, C.NEUTRAL_PILL_OWN_GROUND_FACTOR,
    C.ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT = t[1], t[2], t[3]
end
local DEFAULTS = save_knobs()

print("defaults and presets")
check("POOL6_PICK_WITH_FACTORS default true", C.POOL6_PICK_WITH_FACTORS == true)
check("NEUTRAL_PILL_OWN_GROUND_FACTOR default 0.75", C.NEUTRAL_PILL_OWN_GROUND_FACTOR == 0.75)
check("ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT default true", C.ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT == true)
for _, pn in ipairs({ "keel", "mast" }) do
  local P = C.PRESETS[pn]
  check(pn .. ": POOL6_PICK_WITH_FACTORS false", P.POOL6_PICK_WITH_FACTORS == false)
  check(pn .. ": NEUTRAL_PILL_OWN_GROUND_FACTOR 1.0", P.NEUTRAL_PILL_OWN_GROUND_FACTOR == 1.0)
  check(pn .. ": ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT false", P.ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT == false)
end

print("influence scale")
INF = { [10 * 256 + 10] = 80, [20 * 256 + 20] = -80, [30 * 256 + 30] = 50 }
check("inf 80 -> x0.5", G.influence_mult_at(10, 10) == 0.5)
check("inf -80 -> x2", G.influence_mult_at(20, 20) == 2.0)
check("inf 50 -> x1 (above 50 only)", G.influence_mult_at(30, 30) == 1.0)
check("inf 0 -> x1", G.influence_mult_at(40, 40) == 1.0)

print("neutral own ground factor")
local NP = { owner = "neutral", health = 15, mx = 10, my = 10 }
check("neutral live, inf 80 -> 0.75", G.neutral_own_ground_mult(NP, 80) == 0.75)
check("neutral live, inf 50 -> 1", G.neutral_own_ground_mult(NP, 50) == 1.0)
check("neutral live, inf -80 -> 1", G.neutral_own_ground_mult(NP, -80) == 1.0)
check("neutral dead -> 1", G.neutral_own_ground_mult({ owner = "neutral", health = 0 }, 80) == 1.0)
check("hostile live -> 1", G.neutral_own_ground_mult({ owner = "hostile", health = 15 }, 80) == 1.0)
check("no pill -> 1", G.neutral_own_ground_mult(nil, 80) == 1.0)
C.NEUTRAL_PILL_OWN_GROUND_FACTOR = 1.0
check("knob 1.0 -> 1", G.neutral_own_ground_mult(NP, 80) == 1.0)
set_knobs(DEFAULTS)

print("phase weight (same formula as before)")
-- middle: attack_pill 0.6, fall-off 30 squares. Tank at (100,100).
local info = { tankx = 100 * 256 + 128, tanky = 100 * 256 + 128 }
local PWm = C.PHASE_WEIGHTS.middle
check("at the tank: 0.6", near(G.phase_weight_at(PWm, "attack_pill", 100, 100, info), 0.6))
check("15 squares: 0.8", near(G.phase_weight_at(PWm, "attack_pill", 110, 105, info), 1 - 0.4 * 0.5))
check("30+ squares: 1.0", near(G.phase_weight_at(PWm, "attack_pill", 130, 110, info), 1.0))
check("no table: 1.0", G.phase_weight_at(nil, "attack_pill", 100, 100, info) == 1.0)

-- The recorded game (20261116_002603 bot 2, t=30844), cut down: hostile
-- pill #10 in enemy ground, raw 110; neutral pill #15 in our ground, raw
-- 275. Raw pick: #10. Full pick: #15.
local TANK = { mx = 121, my = 125 }   -- 14 squares from #15, 22 from #10
local function scene(phase)
  INF = { [130 * 256 + 130] = 80, [118 * 256 + 136] = -80 }
  local p10 = { owner = "hostile", health = 15, mx = 136, my = 118 }
  local p15 = { owner = "neutral", health = 15, mx = 130, my = 130 }
  local state = {
    phase = phase, tick = 30844,
    cost_cache = { ["6:10"] = { cost = 110 }, ["6:15"] = { cost = 275 } },
    pool_partial = { [6] = {
      candidates = {
        { id = 10, mx = 136, my = 118, cost = 110, obj = p10, soa = 30 },
        { id = 15, mx = 130, my = 130, cost = 275, obj = p15, soa = 25 },
      },
      best_cost = 110, best_id = 10, best_obj = p10, best_shells_on_arrival = 30,
    } },
  }
  local inf = { tankx = TANK.mx * 256 + 128, tanky = TANK.my * 256 + 128 }
  return state, inf
end

print("pool 6 pick")
local st, inf6 = scene("middle")
G.repick_pool6_with_factors(st, inf6)
local pr = st.pool_partial[6]
check("all on: picks neutral #15", pr.best_id == 15, pr.best_id)
check("all on: keeps #15 RAW cost 275 (factors come once, later)", pr.best_cost == 275, pr.best_cost)
check("all on: shells-on-arrival of #15", pr.best_shells_on_arrival == 25, pr.best_shells_on_arrival)
-- Hand check: #15 pw = 1 - 0.4 x (1 - 14/30) = 0.7867; 275 x 0.7867 x 0.5 x 0.75 = 81.1
--             #10 pw = 1 - 0.4 x (1 - 22/30) = 0.8933; 110 x 0.8933 x 2 = 196.5
local pw15 = G.phase_weight_at(PWm, "attack_pill", 130, 130, inf6)
local pw10 = G.phase_weight_at(PWm, "attack_pill", 136, 118, inf6)
check("hand check #15 < #10", 275 * pw15 * 0.5 * 0.75 < 110 * pw10 * 2.0)

set_knobs({ false, 1.0, true })
st, inf6 = scene("middle")
G.repick_pool6_with_factors(st, inf6)
check("both pick knobs off: raw pick #10 stays", st.pool_partial[6].best_id == 10)
check("both pick knobs off: cost 110 stays", st.pool_partial[6].best_cost == 110)

set_knobs({ false, 0.75, true })
st, inf6 = scene("middle")
G.repick_pool6_with_factors(st, inf6)
check("only neutral factor: 275 x 0.75 = 206 > 110, #10", st.pool_partial[6].best_id == 10)

set_knobs({ true, 1.0, true })
st, inf6 = scene("middle")
G.repick_pool6_with_factors(st, inf6)
-- 275 x 0.7867 x 0.5 = 108.2 < 196.5
check("only factor pick: #15 (108 < 197)", st.pool_partial[6].best_id == 15)
set_knobs(DEFAULTS)

st, inf6 = scene("opening")
G.repick_pool6_with_factors(st, inf6)
-- opening: no influence, no neutral factor; pw 3.0 attenuated on both.
local pwo15 = G.phase_weight_at(C.PHASE_WEIGHTS.opening, "attack_pill", 130, 130, inf6)
local pwo10 = G.phase_weight_at(C.PHASE_WEIGHTS.opening, "attack_pill", 136, 118, inf6)
local want = (275 * pwo15 < 110 * pwo10) and 15 or 10
check("opening: phase weight only", st.pool_partial[6].best_id == want, st.pool_partial[6].best_id)

st, inf6 = scene("middle")
st.cost_cache["6:15"]._reject = "ally_claimed"
G.repick_pool6_with_factors(st, inf6)
check("rejected #15 is skipped", st.pool_partial[6].best_id == 10)

st, inf6 = scene("middle")
st.pool_partial[6].candidates[2].obj.owner = "hostile"
G.repick_pool6_with_factors(st, inf6)
check("#15 hostile: no neutral factor, still x0.5 -> #15 (108 < 197)", st.pool_partial[6].best_id == 15)

print("attack_base commitment after the first shell")
local goal = { kind = "attack_base", mx = 50, my = 50 }
check("approach, no shell yet: bonus 0", G.attack_base_commit_bonus(goal, { shells = 30 }) == 0)
check("no shells: no bonus (nil)", G.attack_base_commit_bonus(goal, { shells = 0 }) == nil)
check("not attack_base: nil", G.attack_base_commit_bonus({ kind = "attack_pill" }, { shells = 30 }) == nil)
-- steering: press fire at 30 shells, next tick 29 -> shot counted.
goal._ab_shoot_shells = 30
S.note_base_shot({ tick = 1 }, goal, { shells = 30 })
check("same count: not yet fired", not goal._ab_shot_fired)
S.note_base_shot({ tick = 2 }, goal, { shells = 29 })
check("one shell fewer: fired", goal._ab_shot_fired == true)
check("after the shell: bonus 250", G.attack_base_commit_bonus(goal, { shells = 29 }) == C.ATTACK_BASE_COMMITMENT_BONUS)
local g2 = { kind = "attack_pill", _ab_shoot_shells = 30 }
S.note_base_shot({ tick = 3 }, g2, { shells = 20 })
check("other goal kind: never marked", not g2._ab_shot_fired)
C.ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT = false
check("knob off: bonus 250 at once", G.attack_base_commit_bonus({ kind = "attack_base" }, { shells = 30 }) == C.ATTACK_BASE_COMMITMENT_BONUS)
set_knobs(DEFAULTS)

-- The recorded game at t=31444 (bot 2): base #13 at 221.2; pill #15 at
-- 144.9 after phase weight and x0.5, + switch 30 + commitment cap 75.
local p15 = 144.9 * C.NEUTRAL_PILL_OWN_GROUND_FACTOR + C.GOAL_SWITCH_PENALTY + 75
check(string.format("t=31444 sum: #15 %.1f < base 221.2", p15), p15 < 221.2)

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
