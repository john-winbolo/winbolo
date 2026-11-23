-- =========================================================================
-- test_blitz_no_dbl.lua -- standalone unit tests (2026-11-22) for
-- C.BLITZ_NO_DOUBLE_TAKE. Andrew: "No blitzes with double takes! double
-- takes need pills to target specific directions so other players will
-- throw it off".
--
-- The rule: a take under a double-take plan is never a blitz.
-- pt2_dbl.no_blitz says a take is under a double-take plan when its own v2
-- plan kills 2+ pills (goal._pt2.kills), or this bot's double-take cache
-- has a fresh entry on the pill that qualifies and no ally claims a chain
-- pill. The answer latches on the goal (goal._blitz_dbl).
--
--   1. pt2_dbl.no_blitz: plan, shadow, stale entry, ally claim, one kill,
--      blitz-only bot, latch, knob off;
--   2. squad.update, commander on the take: status "dbl", no blitz target,
--      no roster (old: "blitz", target set);
--   3. squad.update, BLITZ_SWITCH: a cheaper call on another pill does not
--      pull a commander off its double take (old: switch to soldier);
--   4. squad.update, committed soldier: a commander that says "dbl" no
--      longer leads it, so it uncommits (old: stays committed);
--   5. squad.availability: no answer to a call on the pill of our own
--      double take, reason "dbl" (old: yes);
--   6. attack.update_attack_substate: a commander blitz with a double-take
--      plan drops the blitz before GO (old: stays a blitz); blitz_wait goes
--      back to approach; a committed soldier and a take after GO keep
--      their blitz;
--   7. init.lua: the call gate reads the latch (source check; the call
--      code sits inside think()).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_blitz_no_dbl.lua
-- Arguments:
--   old        C.BLITZ_NO_DOUBLE_TAKE = false (the old rule; 1 to 6 fail
--              where the rule changes the answer).
--   src=<dir>  run 7 on <dir>/init.lua (default: this directory).
-- =========================================================================

package.path = "./?.lua;" .. package.path

local OLD, SRC = false, "."
for _, a in ipairs(arg or {}) do
  if a == "old" then OLD = true end
  local d = a:match("^src=(.+)$")
  if d then SRC = d end
end

local C  = require("constants")
if OLD then C.BLITZ_NO_DOUBLE_TAKE = false end
print(string.format("C.BLITZ_NO_DOUBLE_TAKE = %s", tostring(C.BLITZ_NO_DOUBLE_TAKE)))
local AS = require("ally_state")
local SQ = require("squad")
local D  = require("pt2_dbl")
local A  = require("attack")

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

local ME, NOW = 3, 512
SQ.set_blitz_size(2, 4, "init_arg")

-- Pills: 4 and 5 side by side (our double take 4+5), 3 is an ally's call.
local function new_world()
  return { pills = {
    [4] = { mx = 123, my = 119, health = 15, owner = "hostile" },
    [5] = { mx = 125, my = 119, health = 15, owner = "hostile" },
    [3] = { mx = 118, my = 137, health = 15, owner = "hostile" },
  }, pill_at = {}, bases = {}, tanks = {} }
end
local INFO = { player_number = ME, armour = 40, shells = 40, carried_pills = 2,
               mines = 40, trees = 40, tankx = 120 * 256, tanky = 126 * 256,
               man_status = C.LGM_INTANK, objects = {}, gameinfo = {} }

-- A fresh double-take cache entry on pill 4: chain 4+5, safe, 2 kills, one
-- pill blocker (we carry 2), no walls.
local function dbl_entry(now)
  return { tick = now, carried = INFO.carried_pills, trees = INFO.trees,
           ids = { 4, 5 }, hp = { 15, 15 }, owner = { "hostile", "hostile" },
           pos = { { 123, 119 }, { 125, 119 } },
           safe = true, kills = 2, have = 1, need = 1, pills = 1, walls = 0, trees_need = 0 }
end
local function take_goal(pid, sub)
  local p = new_world().pills[pid]
  return { kind = "attack_pill", target_id = pid, substate = sub or "plan_position",
           mx = p.mx, my = p.my }
end
local function plan2() return { kills = 2, mode = "HOLD", ids = { 4, 5 }, step = 1 } end

-- ── 1. pt2_dbl.no_blitz ──────────────────────────────────────────────────
print("1. pt2_dbl.no_blitz")
AS.init()
do
  local w = new_world()
  local g = take_goal(4); g._pt2 = plan2()
  check("own v2 plan kills 2: plan", D.no_blitz({ tick = NOW }, g, w, INFO, NOW) == "plan", g._blitz_dbl)
  check("  latched on the goal", g._blitz_dbl == "plan", g._blitz_dbl)
  g._pt2 = nil
  check("  latch holds after the plan is gone", D.no_blitz({ tick = NOW }, g, w, INFO, NOW) == "plan")
  local g1 = take_goal(4); g1._pt2 = { kills = 1, mode = "SINGLE" }
  check("own v2 plan kills 1: nil", D.no_blitz({ tick = NOW }, g1, w, INFO, NOW) == nil)

  local st = { tick = NOW, _pt2_dbl = { by_id = { [4] = dbl_entry(NOW - 10) } } }
  local g2 = take_goal(4)
  check("fresh qualifying cache entry: shadow", D.no_blitz(st, g2, w, INFO, NOW) == "shadow", g2._blitz_dbl)
  local g3 = take_goal(4)
  w.pills[5].health = 9
  check("chain pill health changed (stale): nil", D.no_blitz(st, g3, w, INFO, NOW) == nil, g3._blitz_dbl)
  w.pills[5].health = 15
  local e = dbl_entry(NOW - 10); e.safe = false
  local g4 = take_goal(4)
  check("unsafe entry: nil", D.no_blitz({ tick = NOW, _pt2_dbl = { by_id = { [4] = e } } }, g4, w, INFO, NOW) == nil)
  local g5 = take_goal(4)
  check("no world (availability path), no plan: nil", D.no_blitz(st, g5, nil, INFO, NOW) == nil)
  -- an ally claims chain pill 5 (pch): the pool gives no discount, no sign
  AS.set_info(1, NOW, { goal = "attack_pill", target = "5", role = "c", sqst = "blitz", sub = "approach" })
  local g6 = take_goal(4)
  check("ally claims chain pill 5: nil", D.no_blitz(st, g6, w, INFO, NOW) == nil, g6._blitz_dbl)
  AS.init()
  local g7 = take_goal(4); g7._pt2 = plan2()
  check("blitz-only bot: nil", D.no_blitz({ tick = NOW, blitz_only = true }, g7, w, INFO, NOW) == nil)
  local g8 = take_goal(4); g8.kind = "capture_pill"; g8._pt2 = plan2()
  check("not attack_pill: nil", D.no_blitz({ tick = NOW }, g8, w, INFO, NOW) == nil)
end

-- ── 2..4. squad.update ───────────────────────────────────────────────────
local function calls() return { [6] = { pill = 3, tick = 450 } } end
local function cmdr_state(goal)
  return { tick = NOW, blitz_calls = calls(), _my_blitz_call = goal.target_id,
           _my_blitz_call_tick = 500, squad_commander_pill = goal.target_id, goal = goal }
end
local function allies_c6()
  AS.init()
  AS.set_info(6, NOW, { goal = "attack_pill", target = "3", role = "c", sqst = "blitz", sub = "approach" })
end
local function desc(st)
  return string.format("role=%s tgt=%s st=%s accept=%s switch=%s acc=%s", tostring(st.squad_role),
    tostring(st.squad_blitz_target), tostring(st.squad_status), tostring(st.squad_blitz_accept),
    tostring(st._blitz_switch_to), tostring(st.squad_blitz_accepted))
end

print("2. squad.update: commander on a double take")
do
  allies_c6()
  local g = take_goal(4, "approach"); g._pt2 = plan2()
  local st = cmdr_state(g)
  local ok, err = pcall(SQ.update, st, INFO, NOW, new_world())
  check("update runs", ok, err)
  check("  still commander", st.squad_role == "c", desc(st))
  check("  status dbl, no blitz target, no ask",
        st.squad_status == "dbl" and st.squad_blitz_target == nil and st.squad_help_target == nil, desc(st))
  check("  no roster / accept list", st.squad_blitz_roster == nil and st.squad_blitz_accept == nil, desc(st))
  -- the same take with no double take: a normal blitz ask
  allies_c6()
  local g2 = take_goal(4, "approach")
  local st2 = cmdr_state(g2)
  pcall(SQ.update, st2, INFO, NOW, new_world())
  check("no double take: status blitz, target 4 (unchanged)",
        st2.squad_status == "blitz" and st2.squad_blitz_target == 4, desc(st2))
end

print("3. squad.update: BLITZ_SWITCH does not pull a double take")
do
  local function sw(with_plan)
    allies_c6()
    local g = take_goal(4, "approach")
    if with_plan then g._pt2 = plan2() end
    local st = cmdr_state(g)
    st.cost_cache = { ["6:4"] = { _p = 6, _id = 4, cost = 93 }, ["6:3"] = { _p = 6, _id = 3, cost = 50 } }
    local ok, err = pcall(SQ.update, st, INFO, NOW, new_world())
    return st, ok, err
  end
  local st, ok, err = sw(true)
  check("update runs", ok, err)
  check("  double take on 4: no switch to C6's cheaper pill 3",
        st._blitz_switch_to == nil and st.squad_role == "c", desc(st))
  local st2 = sw(false)
  check("no double take: switch to C6 (unchanged)", st2._blitz_switch_to == 6, desc(st2))
end

print("4. squad.update: committed soldier, commander says dbl")
do
  local function sol(sqst)
    AS.init()
    AS.set_info(6, NOW, { goal = "attack_pill", target = "3", role = "c", sqst = sqst, sub = "approach" })
    AS.set_handshake(6, NOW, "bac", tostring(ME))
    local st = { tick = NOW, blitz_calls = calls(), squad_blitz_accepted = 6,
                 _blitz_commit_tick = NOW - 50, goal = take_goal(3, "approach") }
    local ok, err = pcall(SQ.update, st, INFO, NOW, new_world())
    return st, ok, err
  end
  local st, ok, err = sol("dbl")
  check("update runs", ok, err)
  check("  commander on a double take: soldier uncommits", st.squad_blitz_accepted == nil, desc(st))
  local st2 = sol("blitz")
  check("commander on a blitz: soldier stays committed (unchanged)",
        st2.squad_blitz_accepted == 6 and st2.squad_blitz_target == 3, desc(st2))
end

-- ── 5. squad.availability ────────────────────────────────────────────────
print("5. squad.availability")
do
  local st = { tick = NOW, goal = take_goal(4) }
  st.goal._pt2 = plan2()
  local ok, why = SQ.availability(st, INFO, 4)
  check("call on our double-take pill: no, reason dbl", ok == false and why == "dbl",
        tostring(ok) .. "/" .. tostring(why))
  local st2 = { tick = NOW, goal = take_goal(4) }
  local ok2 = SQ.availability(st2, INFO, 4)
  check("no double take: yes (unchanged)", ok2 == true, ok2)
end

-- ── 6. attack.update_attack_substate ─────────────────────────────────────
print("6. attack.update_attack_substate")
A.advance_pill_eval_chunk = function() return "in_progress" end
do
  local function run(sub, opts)
    opts = opts or {}
    local g = take_goal(4, sub)
    g._pt2 = plan2()
    g._blitz, g._blitz_cmdr, g._blitz_started = true, true, true
    g._blitz_committed = opts.committed
    local st = { tick = NOW, phase = "mid", goal = g, squad_role = "c", squad_blitz_target = 4,
                 squad_blitz_accepted = opts.accepted, squad_cmdr = opts.accepted,
                 _pill_eval_cache = {}, _pill_eval_progress = {} }
    local ok, err = pcall(A.update_attack_substate, g, st, new_world(), INFO)
    return g, st, ok, err
  end
  local g, _, ok, err = run("plan_position")
  check("plan_position: no error", ok, err)
  check("  commander blitz dropped (_blitz false, solo latch)", g._blitz == false and g._blitz_solo == true,
        string.format("_blitz=%s solo=%s dbl=%s", tostring(g._blitz), tostring(g._blitz_solo), tostring(g._blitz_dbl)))
  local g2, st2 = run("blitz_wait")
  check("blitz_wait: back to approach, solo",
        g2._blitz == false and g2._blitz_solo == true and g2.substate ~= "blitz_wait",
        string.format("_blitz=%s sub=%s", tostring(g2._blitz), tostring(g2.substate)))
  check("  in-position flag cleared", st2.squad_blitz_in_position == nil, st2.squad_blitz_in_position)
  local g3 = run("approach", { accepted = 6 })
  check("committed soldier keeps its blitz", g3._blitz_solo == nil and g3._blitz_dbl == nil,
        string.format("solo=%s dbl=%s", tostring(g3._blitz_solo), tostring(g3._blitz_dbl)))
  local g4 = run("charge", { committed = true })
  check("after GO (committed) keeps its blitz", g4._blitz_solo == nil and g4._blitz_dbl == nil,
        string.format("solo=%s dbl=%s", tostring(g4._blitz_solo), tostring(g4._blitz_dbl)))
end

-- ── 7. init.lua call gate ────────────────────────────────────────────────
print(string.format("7. init.lua's blitz call gate (%s/init.lua)", SRC))
do
  local fh = io.open(SRC .. "/init.lua", "r")
  local src = fh and fh:read("*a") or ""
  if fh then fh:close() end
  check("the call gate reads goal._blitz_dbl",
        src:find("and not %(C%.BLITZ_NO_DOUBLE_TAKE and _g%._blitz_dbl%)") ~= nil, nil)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
