-- =========================================================================
-- test_pt2_claim.lua -- standalone unit tests (2026-11-18) for
-- pt2_claim.lua (PT2_CHAIN_CLAIM): a v2 chain claims all of its pills.
--
--   * encode / decode of "/info pch <target> <id> ...", and the comms
--     receive path (set, clear, old-style /info state untouched).
--   * an ally's chain counts only while its goal is attack_pill on that
--     target.
--   * the conflict rule gives opposite answers on the two bots.
--   * chain_skip: the chain owner plans without a pill it lost; never the
--     target; dead allies do not count.
--   * the sender payload: live hostile/neutral chain pills only; cleared
--     when the take ends.
--   * drop_lost: a live plan drops a chain pill an ally now holds.
--   * pilltake_v2_old: a skipped pill ends the chain.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_claim.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

local saved = {}
for _, k in ipairs({ "PT2_CHAIN_CLAIM", "PILLTAKE_V2_EVALUATOR", "PT2_CHAINS" }) do saved[k] = C[k] end
C.PT2_CHAIN_CLAIM, C.PILLTAKE_V2_EVALUATOR, C.PT2_CHAINS = true, true, true

local AS = require("ally_state")
AS.init()
local PC = require("pt2_claim")
local comms = require("comms")

-- ── encode / decode ──────────────────────────────────────────────────────
print("encode / decode")
check("encode 13 [13,14]", PC.encode(13, { 13, 14 }) == "13 14", PC.encode(13, { 13, 14 }))
check("encode 13 [14,15,14]", PC.encode(13, { 14, 15, 14 }) == "13 14 15")
check("encode target only: nil", PC.encode(13, { 13 }) == nil)
check("encode no ids: nil", PC.encode(13, nil) == nil)
local t, set = PC.decode("13 14 15")
check("decode target", t == 13, t)
check("decode set", set and set[14] and set[15] and not set[13])
check("decode junk: nil", PC.decode("13 x") == nil)
check("decode target only: nil", PC.decode("13") == nil)
check("decode empty: nil", PC.decode("") == nil and PC.decode(nil) == nil)
local t2, set2 = PC.decode("13 14 15")
check("decode memo: same table", set2 == set and t2 == 13)

-- ── receive (comms) ──────────────────────────────────────────────────────
print("receive")
local st = { tick = 100 }
comms.process_message(2, "/info state goal=attack_pill target=13 sub=approach", 100, st)
comms.process_message(2, "/info pch 13 14", 100, st)
check("pch stored", PC.rx_get(2) == "13 14", PC.rx_get(2))
local s3 = PC.ally_set(AS.get(2).info, 2)
check("ally set has 14", s3 and s3[14] == true)
comms.process_message(2, "/info state goal=attack_pill target=13 sub=aim", 110, st)
check("pch kept over /info state", PC.rx_get(2) == "13 14")
comms.process_message(2, "/info extra cost=12 bpj=-", 111, st)
check("pch kept over /info extra", PC.ally_set(AS.get(2).info, 2) ~= nil)
check("other ally: no set", PC.ally_set(AS.get(2).info, 3) == nil)
comms.process_message(2, "/info state goal=attack_pill target=15 sub=approach", 120, st)
check("target changed: chain not honoured", PC.ally_set(AS.get(2).info, 2) == nil)
comms.process_message(2, "/info state goal=refuel_at_base target=3", 130, st)
check("goal changed: chain not honoured", PC.ally_set(AS.get(2).info, 2) == nil)
comms.process_message(2, "/info state goal=attack_pill target=13 sub=approach", 140, st)
comms.process_message(2, "/info pch", 140, st)
check("empty pch clears", PC.rx_get(2) == nil)
check("not /info pchx", (function() comms.process_message(2, "/info pchx 1 2", 140, st); return PC.rx_get(2) == nil end)())

-- ── the rule ─────────────────────────────────────────────────────────────
print("rule")
-- keep(our_cost, our_sub, our_target, ally_cost, ally_sub, ally_target, self, ally)
local function both(cA, sA, tA, cB, sB, tB, pA, pB)
  local a = PC.keep(cA, sA, tA, cB, sB, tB, pA, pB)
  local b = PC.keep(cB, sB, tB, cA, sA, tA, pB, pA)
  return a, b
end
local a, b = both(100, "aim", false, 50, "approach", true, 1, 2)
check("engaged chain beats pre-commit target", a == true and b == false)
a, b = both(100, "aim", false, 50, "shoot", true, 1, 2)
check("both engaged: target keeps", a == false and b == true)
a, b = both(100, "approach", false, 50, "plan_position", true, 1, 2)
check("neither engaged: cheaper keeps", a == false and b == true)
a, b = both(100, "approach", false, 90, "approach", true, 2, 1)
check("cost band: lower player keeps", a == false and b == true)
a, b = both(nil, "approach", false, 90, "approach", true, 1, 2)
check("no cost: lower player keeps", a == true and b == false)
a, b = both(100, "aim", false, 100, "aim", false, 3, 1)
check("both engaged chains: lower player keeps", a == false and b == true)

-- ── chain_skip (the chain owner's side) ──────────────────────────────────
print("chain_skip")
AS.init()
local P13, P14, P15 = { mx = 100, my = 100 }, { mx = 101, my = 100 }, { mx = 102, my = 100 }
local inp = { pills = { P13, P14, P15 }, ids = { [P13] = 13, [P14] = 14, [P15] = 15 }, A = P13 }
-- we: player 1, attack_pill 13 approach, cost 100; ally p2 on 14 at cost 50
local me = { goal = { kind = "attack_pill", target_id = 13, substate = "approach" },
             broadcast_state_extra = { cost = "100" } }
AS.set_info(2, 200, { goal = "attack_pill", target = "14", sub = "approach", cost = "50" })
local sk = PC.chain_skip(me, inp, 200, 1)
check("ally cheaper on 14: skip 14", sk and sk[P14] == 2 and not sk[P15])
check("never skip the target", not (sk and sk[P13]))
me.broadcast_state_extra.cost = "30"
sk = PC.chain_skip(me, inp, 200, 1)
check("we cheaper: no skip", sk == nil)
me.broadcast_state_extra.cost = "100"
me.state_dead = nil
me.tank_dead_at = { [2] = 300 }
check("dead ally: no skip", PC.chain_skip(me, inp, 300, 1) == nil)
me.tank_dead_at = nil
-- ally's CHAIN claims 15 (target 16): both chain claims -> cost band
AS.set_info(3, 200, { goal = "attack_pill", target = "16", sub = "aim", cost = "500" })
PC.rx(3, "16 15")
sk = PC.chain_skip(me, inp, 200, 1)
check("engaged ally chain holds 15", sk and sk[P15] == 3)
check("claimed_by 15 = p3 chain", (function() local by, tg = PC.claimed_by(me, 15, 200, 1); return by == 3 and tg == false end)())
check("chain_ally_claimed [13,15]", (PC.chain_ally_claimed(me, { 13, 15 }, 200, 1)) == 15)
check("chain_ally_claimed [13]: nil", PC.chain_ally_claimed(me, { 13 }, 200, 1) == nil)
C.PT2_CHAIN_CLAIM = false
check("knob off: no skip", PC.chain_skip(me, inp, 200, 1) == nil)
C.PT2_CHAIN_CLAIM = true

-- ── sender payload ───────────────────────────────────────────────────────
print("payload")
local world = { pills = {
  [13] = { owner = "neutral", health = 15 }, [14] = { owner = "hostile", health = 10 },
  [15] = { owner = "ours", health = 15 } } }
local g = { kind = "attack_pill", target_id = 13,
            _pt2 = { mode = "HOLD", ids = { 13, 14, 15 }, step = 1 } }
local s = { goal = g }
check("payload 13 14 (15 is ours)", PC.payload(s, world) == "13 14", PC.payload(s, world))
world.pills[14].health = 0
check("chain pill dead: no payload", PC.payload(s, world) == nil)
world.pills[14].health = 10
g._pt2.mode = "SINGLE"
check("single take: no payload", PC.payload(s, world) == nil)
g._pt2.mode = "HOLD"
local sent = {}
local function try_send(m) sent[#sent + 1] = m; return true end
PC.tx(s, world, 10, try_send)
check("tx sends", sent[1] == "/info pch 13 14", sent[1])
PC.tx(s, world, 11, try_send)
check("tx no change: no send", #sent == 1)
PC.tx(s, world, 311, try_send)
check("tx heartbeat at 300", #sent == 2)
s.goal = { kind = "capture_pill", target_id = 13 }
PC.tx(s, world, 312, try_send)
check("take ended: clear sent", sent[3] == "/info pch", sent[3])
PC.tx(s, world, 313, try_send)
check("cleared once", #sent == 3)

-- ── drop_lost: a live plan drops a chain pill an ally now holds ─────────
-- Seed 1 oilrig t=4933: p3 planned 12-13 while it held 13; p2 (target 13)
-- went to blitz_wait at t=4942. p3 kept the old plan to the end.
print("drop_lost")
do
  -- attack.lua's own replan reset, stubbed: loading attack.lua here rebuilds
  -- los_stamp_cache.lua. The stub records the call.
  local reset_goal
  local saved_attack = package.loaded["attack"]
  package.loaded["attack"] = { _reset_to_plan_position = function(_, g)
    reset_goal = g; g.substate = "plan_position"; g.scan_spots = nil; g._shield_scan = nil
    g._plan_show_tick = nil; g._plan_position_cleared = nil
  end }
  local w = { pills = { [12] = { owner = "hostile", health = 15 }, [13] = { owner = "hostile", health = 14 } } }
  local function mk(sub)
    return { goal = { kind = "attack_pill", target_id = 12, substate = sub, scan_spots = {}, _shield_scan = {},
                      _plan_show_tick = 4933, _plan_position_cleared = true, standoff_mx = 135,
                      _pt2 = { mode = "HOLD", ids = { 12, 13 }, step = 1 } },
             broadcast_state_extra = { cost = "160" }, _shield_scan_cache = {} }
  end
  -- ally p2: target 13, not engaged yet, pricier -> we keep the plan
  AS.set_info(2, 4940, { goal = "attack_pill", target = "13", sub = "approach", cost = "361" })
  PC.rx(2, nil)
  local st = mk("approach")
  check("ally not engaged, pricier: plan kept", PC.drop_lost(st, w, 4940, 3) == nil and st.goal._pt2 ~= nil)
  -- ally p2 engaged on 13 -> we lose 13 -> plan dropped, back to plan_position
  AS.set_info(2, 4942, { goal = "attack_pill", target = "13", sub = "blitz_wait", cost = "361" })
  local lost, by = PC.drop_lost(st, w, 4942, 3)
  check("ally engaged on 13: plan dropped", lost == 13 and by == 2, tostring(lost))
  check("dropped: back to plan_position", st.goal.substate == "plan_position" and st.goal._pt2 == nil
        and st.goal.scan_spots == nil and st.goal._shield_scan == nil and st._shield_scan_cache == nil)
  -- seed 1 t=4936: without the full reset the old show tick sent the take
  -- straight back to approach on the old plan, with no new scan
  check("dropped: attack's full replan reset ran on the goal", reset_goal == st.goal)
  check("dropped: payload cleared", PC.payload(st, w) == nil)
  -- the target itself (step) is never checked here: the sync owns it
  st = mk("approach")
  st.goal._pt2.ids = { 13, 12 }
  st.goal.target_id = 13
  check("target never dropped here", PC.drop_lost(st, w, 4942, 3) == nil)
  -- a dead chain pill does not count
  st = mk("approach")
  w.pills[13].health = 0
  check("dead chain pill: plan kept", PC.drop_lost(st, w, 4942, 3) == nil)
  w.pills[13].health = 14
  st.goal._pt2.mode = "SINGLE"
  check("single take: nothing to drop", PC.drop_lost(st, w, 4942, 3) == nil)
  st.goal._pt2.mode = "HOLD"
  C.PT2_CHAIN_CLAIM = false
  check("knob off: plan kept", PC.drop_lost(st, w, 4942, 3) == nil)
  C.PT2_CHAIN_CLAIM = true
  AS.set_info(2, 4942, { goal = "explore" })
  package.loaded["attack"] = saved_attack
end

-- ── pilltake_v2_old: a skipped pill ends the chain ───────────────────────────
print("pilltake_v2_old")
do
  local src = io.open("pilltake_v2_old.lua"):read("*a")
  check("eval_point honours chain_skip", src:find("inp.chain_skip and inp.chain_skip[nxt]", 1, true) ~= nil)
  check("evaluate_group honours chain_skip", src:find("inp.chain_skip and inp.chain_skip[p]", 1, true) ~= nil)
end

for k, v in pairs(saved) do C[k] = v end
print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
