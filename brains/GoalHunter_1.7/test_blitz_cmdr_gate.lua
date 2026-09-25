-- =========================================================================
-- test_blitz_cmdr_gate.lua — standalone unit tests for the blitz-only
-- commander gate (knob C.BLITZ_ONLY_CMDR_NEEDS_FREE, 2026-09-25):
--   * squad.free_ally_count     — who counts as a FREE live ally
--   * goals._apply_blitz_only_gate — a NEW commander take needs
--                                  1 + free allies >= blitz min; joins and
--                                  a take we already lead are untouched
--   * squad.goal_call_pick      — a soldier answers the call on its goal
--                                  pill, not only the nearest call
--   * squad.availability        — unchanged: a gated bot still says yes to
--                                  the call on its goal pill
-- Recorded case 20260925_123606_1__2v2_combined bot3 t=512: blitz min=3
-- max=4, blitzonly flag. p0 commands pill 0 (p1 committed, p2 negotiating),
-- p6 commands pill 3 (p4 committed). p3 lost its capture_pill and opened a
-- NEW call on pill 4 that could never fill.
-- No engine: allies are fed straight into ally_state.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_blitz_cmdr_gate.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C  = require("constants")
local AS = require("ally_state")
local SQ = require("squad")
local G  = require("goals")

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
SQ.set_blitz_size(3, 4, "init_arg")

-- Recorded allies (bot3's view at t=512).
local function recorded_allies()
  AS.init()
  AS.set_info(0, NOW, { goal = "attack_pill", target = "0", role = "c", sqst = "blitz", sub = "approach" })
  AS.set_handshake(0, NOW, "bac", "1")
  AS.set_info(1, NOW, { goal = "attack_pill", target = "0", role = "s", cmdr = "0", sqst = "join", sub = "approach" })
  AS.set_info(2, NOW, { goal = "attack_pill", target = "0", role = "s", cmdr = "0", sqst = "nego", sub = "plan_position" })
  AS.set_info(4, NOW, { goal = "attack_pill", target = "3", role = "s", cmdr = "6", sqst = "join", sub = "approach" })
  AS.set_info(6, NOW, { goal = "attack_pill", target = "3", role = "c", sqst = "blitz", sub = "blitz_wait" })
  AS.set_handshake(6, NOW, "bac", "4")
end
local function calls()
  return { [0] = { pill = 0, tick = 400 }, [6] = { pill = 3, tick = 450 } }
end

-- Pool-6 rows: pill 4 (nobody on it), pill 3 (C6's call), pill 0 (C0's call).
local function cache()
  return {
    ["6:4"] = { _p = 6, _id = 4, _mx = 123, _my = 119, cost = 93 },
    ["6:3"] = { _p = 6, _id = 3, _mx = 118, _my = 137, cost = 140 },
    ["6:0"] = { _p = 6, _id = 0, _mx = 138, _my = 128, cost = 160 },
  }
end
local function world()
  return { pills = {
    [4] = { mx = 123, my = 119, health = 15 },
    [3] = { mx = 118, my = 137, health = 15 },
    [0] = { mx = 138, my = 128, health = 15 },
  } }
end
local INFO = { player_number = ME, armour = 40, shells = 40, carried_pills = 2 }

local function gate(st)
  st.tick = st.tick or NOW
  st.cost_cache = st.cost_cache or cache()
  G._apply_blitz_only_gate(st, INFO, world())
  return st.cost_cache
end
local function rej(c, pid) return c["6:" .. pid]._reject end

-- ── 0. Knob ──────────────────────────────────────────────────────────────
print("knob")
check("BLITZ_ONLY_CMDR_NEEDS_FREE default true", C.BLITZ_ONLY_CMDR_NEEDS_FREE == true)
local keel = C.PRESETS and C.PRESETS.keel or {}
check("PRESETS.keel BLITZ_ONLY_CMDR_NEEDS_FREE = false", keel.BLITZ_ONLY_CMDR_NEEDS_FREE == false)

-- ── 1. free_ally_count ───────────────────────────────────────────────────
print("squad.free_ally_count")
recorded_allies()
do
  local n, desc = SQ.free_ally_count({ blitz_calls = calls() }, NOW, ME)
  check("recorded case: 0 free allies", n == 0, n)
  check("  desc names every ally's blitz", desc == "p0=call p1=C0 p2=nego:C0 p4=C6 p6=call", desc)
  local n2, desc2 = SQ.free_ally_count({}, NOW, ME)
  check("no call registry: commanders still busy by sqst", n2 == 0 and desc2 == "p0=cmdr p1=C0 p2=nego:C0 p4=C6 p6=cmdr", desc2)
end
do
  AS.init()
  AS.set_info(1, NOW, { goal = "attack_base", role = "s", sqst = "free" })
  AS.set_info(2, NOW, { goal = "capture_pill", role = "s", sqst = "bz" })
  AS.set_info(5, NOW, { goal = "refuel" })
  AS.set_info(6, NOW, { goal = "attack_pill", target = "3", role = "c", sqst = "blitz" })
  AS.set_handshake(6, NOW, "bac", "7")
  AS.set_info(7, NOW, { goal = "attack_pill", target = "3" })   -- accepted, no cmdr broadcast yet
  local n, desc = SQ.free_ally_count({}, NOW, ME)
  check("free / bz / no sqst count free; bac-listed does not", n == 3, desc)
  check("  desc", desc == "p1=free p2=free p5=free p6=cmdr p7=bac:C6", desc)
  local n2 = SQ.free_ally_count({ tank_dead_at = { [1] = NOW + 5 } }, NOW, ME)
  check("dead ally is not free", n2 == 2, n2)
  local n3 = SQ.free_ally_count({}, NOW + (C.SQUAD_ALLY_MAX_AGE or 1750) + 1, ME)
  check("stale allies are not counted", n3 == 0, n3)
end

-- ── 2. Gate ──────────────────────────────────────────────────────────────
print("goals.apply_blitz_only_gate")
recorded_allies()
do
  local c = gate({ blitz_only = true, blitz_calls = calls() })
  check("0 free, min 3: new take on pill 4 REJECTED blitz_only", rej(c, 4) == "blitz_only", rej(c, 4))
  check("  why names the gate", tostring(c["6:4"]._blitz_only_why):find("cmdr gate", 1, true) ~= nil, c["6:4"]._blitz_only_why)
  check("  ally list kept for the breakdown", c["6:4"]._blitz_only_desc == "p0=call p1=C0 p2=nego:C0 p4=C6 p6=call", c["6:4"]._blitz_only_desc)
  check("  C6's pill 3 stays a live candidate (join)", rej(c, 3) == nil, rej(c, 3))
  check("  C0's pill 0 stays a live candidate (join)", rej(c, 0) == nil, rej(c, 0))
end
do
  -- One free ally is still short of min 3.
  AS.set_info(5, NOW, { goal = "attack_base", role = "s", sqst = "free" })
  local c = gate({ blitz_only = true, blitz_calls = calls() })
  check("1 free, min 3: still rejected", rej(c, 4) == "blitz_only", rej(c, 4))
  AS.set_info(7, NOW, { goal = "refuel" })
  local c2 = gate({ blitz_only = true, blitz_calls = calls() })
  check("2 free, min 3: pill 4 allowed", rej(c2, 4) == nil, rej(c2, 4))
  -- The reject clears on the same row when allies free up.
  AS.clear(5); AS.clear(7)
  local st = { blitz_only = true, blitz_calls = calls() }
  gate(st)
  check("  (row rejected with 0 free)", rej(st.cost_cache, 4) == "blitz_only")
  AS.set_info(5, NOW, { goal = "attack_base", role = "s", sqst = "free" })
  AS.set_info(7, NOW, { goal = "refuel" })
  gate(st)
  check("  same row clears once 2 are free", rej(st.cost_cache, 4) == nil, rej(st.cost_cache, 4))
  AS.clear(5); AS.clear(7)
end
do
  C.BLITZ_ONLY_CMDR_NEEDS_FREE = false
  local c = gate({ blitz_only = true, blitz_calls = calls() })
  C.BLITZ_ONLY_CMDR_NEEDS_FREE = true
  check("knob off: pill 4 allowed (old rule)", rej(c, 4) == nil, rej(c, 4))
end
do
  local c = gate({ blitz_only = false, blitz_calls = calls() })
  check("no blitz-only: pill 4 allowed (old rule)", rej(c, 4) == nil, rej(c, 4))
  -- Byte-for-byte: no field of any row is touched without blitz-only.
  local fresh = cache()
  local same = true
  for k, e in pairs(c) do
    for f, v in pairs(e) do if fresh[k][f] ~= v then same = false end end
    for f, v in pairs(fresh[k]) do if e[f] ~= v then same = false end end
  end
  check("no blitz-only: rows unchanged field for field", same)
end
do
  -- An established commander of pill 4 (squad_blitz_target from last tick's
  -- squad.update) keeps its row: the gate never tears a running take down.
  local c = gate({ blitz_only = true, blitz_calls = calls(), squad_blitz_target = 4,
                   goal = { kind = "attack_pill", target_id = 4, substate = "approach" } })
  check("our own running take on pill 4 (0 free): not rejected", rej(c, 4) == nil, rej(c, 4))
  -- Negotiating to join a call on pill 4: not a new take either.
  local c2 = gate({ blitz_only = true, blitz_calls = calls(), squad_negotiate_pill = 4 })
  check("negotiating on pill 4 (0 free): not rejected", rej(c2, 4) == nil, rej(c2, 4))
end
do
  -- Another lead reason still wins first (unchanged wording).
  local st = { blitz_only = true, blitz_calls = calls(), squad_cmdr = 0 }
  local c = gate(st)
  check("soldier of C0: old reason kept", c["6:4"]._blitz_only_why == "soldier of C0", c["6:4"]._blitz_only_why)
  check("  no gate ally list for an old reason", c["6:4"]._blitz_only_desc == nil)
end

-- ── 3. Join path (the p3 case) ───────────────────────────────────────────
print("join path")
recorded_allies()
do
  -- Blocked bot, goal none: availability for either call is the old "bz".
  local st = { blitz_only = true, blitz_calls = calls(), tick = NOW, goal = { kind = "none" } }
  local ok, r = SQ.availability(st, INFO, 3)
  check("goal none: availability(C6 pill 3) is the old bz", ok == false and r == "bz", r)
  -- Goal selection then picks C6's pill 3 (live row): availability says yes.
  st.goal = { kind = "attack_pill", target_id = 3, substate = "plan_position" }
  local ok2 = SQ.availability(st, INFO, 3)
  check("goal attack_pill 3: availability(C6 pill 3) = yes", ok2 == true)
  -- The gate leaves the chosen pill live while we are its soldier-to-be.
  local c = gate(st)
  check("  pill 3 row still live under the gate", rej(c, 3) == nil, rej(c, 3))
end
do
  -- Nearest call is C0 (pill 0) but our goal is C6's pill 3: answer C6.
  local cmd_info = { [0] = { target = 0, dist = 20 }, [6] = { target = 3, dist = 31 } }
  local notfull = function() return false end
  local st = { blitz_only = true, goal = { kind = "attack_pill", target_id = 3 } }
  local pn, d, t = SQ.goal_call_pick(st, cmd_info, 0, 0, notfull)
  check("goal pill 3, nearest C0 -> answer C6 pill 3", pn == 6 and d == 31 and t == 3,
        string.format("%s/%s/%s", tostring(pn), tostring(d), tostring(t)))
  check("nearest already on our pill -> nil (keep it)",
        SQ.goal_call_pick(st, cmd_info, 6, 3, notfull) == nil)
  check("C6 full -> nil (keep nearest)",
        SQ.goal_call_pick(st, cmd_info, 0, 0, function(pn) return pn == 6 end) == nil)
  st._blitz_switch_to = 0
  check("cost switch already chose -> nil", SQ.goal_call_pick(st, cmd_info, 0, 0, notfull) == nil)
  st._blitz_switch_to = nil
  C.BLITZ_ONLY_CMDR_NEEDS_FREE = false
  check("knob off -> nil (old nearest pick)", SQ.goal_call_pick(st, cmd_info, 0, 0, notfull) == nil)
  C.BLITZ_ONLY_CMDR_NEEDS_FREE = true
  st.blitz_only = false
  check("no blitz-only -> nil (old nearest pick)", SQ.goal_call_pick(st, cmd_info, 0, 0, notfull) == nil)
  st.blitz_only = true
  st.goal = { kind = "attack_base", target_id = 3 }
  check("goal not attack_pill -> nil", SQ.goal_call_pick(st, cmd_info, 0, 0, notfull) == nil)
  -- Two calls on our pill: nearest, tie -> lower pn.
  st.goal = { kind = "attack_pill", target_id = 3 }
  local ci2 = { [1] = { target = 0, dist = 5 }, [6] = { target = 3, dist = 12 }, [2] = { target = 3, dist = 12 } }
  check("two calls on our pill, same dist -> lower pn", SQ.goal_call_pick(st, ci2, 1, 0, notfull) == 2)
end
do
  -- The p2 case (C0): goal pill 0 with C0 nearest -> unchanged nearest pick.
  local cmd_info = { [0] = { target = 0, dist = 31 }, [6] = { target = 3, dist = 40 } }
  local st = { blitz_only = true, goal = { kind = "attack_pill", target_id = 0 } }
  check("goal pill 0, nearest C0 -> nil (nearest is right)",
        SQ.goal_call_pick(st, cmd_info, 0, 0, function() return false end) == nil)
end

-- ── 4. squad.update end to end (recorded allies, p3 at (114,154)) ─────────
print("squad.update join")
local UPD_INFO = { player_number = ME, armour = 40, shells = 40, carried_pills = 2,
                   mines = 40, trees = 40, tankx = 114 * 256, tanky = 154 * 256,
                   man_status = C.LGM_INTANK, objects = {}, gameinfo = {} }
local UPD_WORLD = { pills = {
  [4] = { mx = 123, my = 119, health = 15, owner = "enemy" },
  [3] = { mx = 118, my = 137, health = 15, owner = "enemy" },   -- C6: nearest call
  [0] = { mx = 138, my = 128, health = 15, owner = "enemy" },   -- C0
} }
local function upd(goal_pill, blitz_only)
  recorded_allies()
  local st = { blitz_only = blitz_only, tick = NOW, blitz_calls = calls(),
               goal = { kind = "attack_pill", target_id = goal_pill, substate = "plan_position",
                        mx = UPD_WORLD.pills[goal_pill].mx, my = UPD_WORLD.pills[goal_pill].my } }
  local ok, err = pcall(SQ.update, st, UPD_INFO, NOW, UPD_WORLD)
  return st, ok, err
end
do
  local st, ok, err = upd(3, true)
  check("goal C6's pill 3: update runs", ok, err)
  check("  soldier negotiating with C6 on pill 3",
        st.squad_role == "s" and st.squad_negotiate_cmdr == 6 and st.squad_negotiate_pill == 3
        and st.squad_status == "nego",
        string.format("role=%s cmdr=%s pill=%s st=%s", tostring(st.squad_role),
          tostring(st.squad_negotiate_cmdr), tostring(st.squad_negotiate_pill), tostring(st.squad_status)))
end
do
  local st, ok, err = upd(0, true)
  check("goal C0's pill 0 (C6 nearer): update runs", ok, err)
  check("  answers C0 (goal pill), not the nearer C6",
        st.squad_negotiate_cmdr == 0 and st.squad_negotiate_pill == 0 and st.squad_status == "nego",
        string.format("cmdr=%s pill=%s st=%s", tostring(st.squad_negotiate_cmdr),
          tostring(st.squad_negotiate_pill), tostring(st.squad_status)))
end
do
  C.BLITZ_ONLY_CMDR_NEEDS_FREE = false
  local st = upd(0, true)
  C.BLITZ_ONLY_CMDR_NEEDS_FREE = true
  check("knob off, goal pill 0: old nearest pick C6 declines bz",
        st.squad_negotiate_cmdr == nil and st.squad_status == "bz", tostring(st.squad_status))
  local st2 = upd(0, false)
  check("no blitz-only, goal pill 0: old nearest pick C6 declines bz",
        st2.squad_negotiate_cmdr == nil and st2.squad_status == "bz", tostring(st2.squad_status))
end
do
  -- A commander already running its take on pill 4 stays commander.
  recorded_allies()
  local st = { blitz_only = true, tick = NOW, blitz_calls = calls(), _my_blitz_call = 4,
               _my_blitz_call_tick = 500, squad_commander_pill = 4,
               goal = { kind = "attack_pill", target_id = 4, substate = "approach", mx = 123, my = 119 } }
  local ok, err = pcall(SQ.update, st, UPD_INFO, NOW, UPD_WORLD)
  check("running take on pill 4: update runs", ok, err)
  check("  still commander of pill 4 (squad_blitz_target 4)",
        st.squad_role == "c" and st.squad_blitz_target == 4 and st.squad_status == "blitz",
        string.format("role=%s tgt=%s st=%s", tostring(st.squad_role), tostring(st.squad_blitz_target), tostring(st.squad_status)))
  -- ...and next tick's gate keeps its row.
  local c = gate({ blitz_only = true, blitz_calls = calls(), squad_blitz_target = st.squad_blitz_target })
  check("  gate keeps its pill 4 row next tick", rej(c, 4) == nil, rej(c, 4))
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
