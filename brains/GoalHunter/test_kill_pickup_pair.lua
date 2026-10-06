-- =========================================================================
-- test_kill_pickup_pair.lua — standalone unit tests for the fresh-kill pair
-- pickup (knob C.KILL_PICKUP_PAIR_MIN_SQUAD):
--   * goals.kill_pickup_rank   — rank / keep / rounding / dead allies
--   * attack.mark_kill_pickup  — blitz size recorded on the claim
--   * attack.handoff_to_capture_pill — skip only when `keep` allies capture
-- No engine: allies are fed straight into ally_state.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_kill_pickup_pair.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C  = require("constants")
local AS = require("ally_state")
local G  = require("goals")
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

local DEFAULT_MIN = C.KILL_PICKUP_PAIR_MIN_SQUAD
local PILL = 5
local NOW  = 1000

-- Fill ally_state with claimers: list of { pn, kc } (kc as broadcast, string).
local function claimers(list, extra)
  AS.init()
  for _, c in ipairs(list) do
    local h = { kg = tostring(PILL), kc = c[2] }
    if extra then for k, v in pairs(extra) do h[k] = v end end
    AS.set_info(c[1], NOW, h)
  end
end

-- Rank of bot `me` (own raw cost `mc`) with claim squad size `sq`.
local function rank_of(me, mc, sq, state)
  local kp = { id = PILL, squad_n = sq }
  local r, keep, best, best_cost, mc_cmp = G.kill_pickup_rank(state or {}, kp, mc, me, NOW)
  return r, keep, best, best_cost, mc_cmp
end
local function keeps(me, mc, sq, state)
  local r, keep = rank_of(me, mc, sq, state)
  return r <= keep
end

-- Every bot's view: bots = { {pn, raw_cost}, ... }; each bot sees the others'
-- broadcast kc = "%.0f" of their raw cost. Returns the set of pns that keep.
local function who_keeps(bots, sq)
  local out = {}
  for _, me in ipairs(bots) do
    local others = {}
    for _, o in ipairs(bots) do
      if o[1] ~= me[1] then others[#others + 1] = { o[1], string.format("%.0f", o[2]) } end
    end
    claimers(others)
    if keeps(me[1], me[2], sq) then out[#out + 1] = me[1] end
  end
  table.sort(out)
  return table.concat(out, ",")
end

-- ── 1. rank / keep ──────────────────────────────────────────────────────
print("goals.kill_pickup_rank")
C.KILL_PICKUP_PAIR_MIN_SQUAD = 3
local three = { { 1, 20 }, { 2, 20 }, { 3, 20 } }
check("squad 3, costs 20/20/20: p1 and p2 keep, p3 yields",
      who_keeps(three, 3) == "1,2", who_keeps(three, 3))
do
  claimers({ { 1, "20" }, { 2, "20" } })
  local r, keep, best, best_cost = rank_of(3, 20, 3)
  check("squad 3: p3 is rank 3 of keep 2, best ally ahead p1 at 20",
        r == 3 and keep == 2 and best == 1 and best_cost == 20,
        string.format("r=%s keep=%s best=%s/%s", r, keep, tostring(best), tostring(best_cost)))
  claimers({ { 1, "20" }, { 3, "20" } })
  r, keep, best = rank_of(2, 20, 3)
  check("squad 3: p2 is rank 2 of keep 2 (keeps), p1 ahead",
        r == 2 and keep == 2 and best == 1, string.format("r=%s keep=%s best=%s", r, keep, tostring(best)))
end
check("squad 2 (knob 3): only the best (p1) keeps",
      who_keeps({ { 1, 20 }, { 2, 20 } }, 2) == "1", who_keeps({ { 1, 20 }, { 2, 20 } }, 2))
check("squad 2 with three claimers (knob 3): only p1 keeps",
      who_keeps(three, 2) == "1", who_keeps(three, 2))
check("squad 4 (knob 3): lower cost beats lower pn: p3=18, p2=20 keep; p1=25 yields",
      who_keeps({ { 1, 25 }, { 2, 20 }, { 3, 18 } }, 4) == "2,3",
      who_keeps({ { 1, 25 }, { 2, 20 }, { 3, 18 } }, 4))

C.KILL_PICKUP_PAIR_MIN_SQUAD = 2
check("knob 2, squad 2: both p1 and p2 keep",
      who_keeps({ { 1, 20 }, { 2, 20 } }, 2) == "1,2", who_keeps({ { 1, 20 }, { 2, 20 } }, 2))
check("knob 2, squad 1 (solo claim): only p1 keeps",
      who_keeps({ { 1, 20 }, { 2, 20 } }, 1) == "1", who_keeps({ { 1, 20 }, { 2, 20 } }, 1))

-- ── 2. keel ─────────────────────────────────────────────────────────────
C.KILL_PICKUP_PAIR_MIN_SQUAD = 0
check("keel (knob 0), squad 3: only p1 keeps (old single grabber)",
      who_keeps(three, 3) == "1", who_keeps(three, 3))
do
  claimers({ { 2, "20" } })
  local r, keep, _, _, mc_cmp = rank_of(1, 20.4, 3)
  check("keel: own cost NOT rounded (20.4 vs ally 20 -> ally cheaper, rank 2 of keep 1)",
        r == 2 and keep == 1 and mc_cmp == 20.4, string.format("r=%s keep=%s mc=%s", r, keep, tostring(mc_cmp)))
end
check("keel keeps the rounding bug: raw 20.4 (p1) vs 20.2 (p2) -> BOTH yield",
      who_keeps({ { 1, 20.4 }, { 2, 20.2 } }, 3) == "",
      who_keeps({ { 1, 20.4 }, { 2, 20.2 } }, 3))
do
  local kp = { id = PILL }   -- a claim made while the knob was off: no squad_n
  claimers({ { 2, "20" } })
  local r, keep = G.kill_pickup_rank({}, kp, 20, 1, NOW)
  check("keel: missing squad_n reads as 1", r == 1 and keep == 1, string.format("r=%s keep=%s", r, keep))
end

-- ── 3. rounding fix ─────────────────────────────────────────────────────
C.KILL_PICKUP_PAIR_MIN_SQUAD = 3
local rnd = { { 1, 20.4 }, { 2, 20.2 } }
check("rounding: raw 20.4 (p1) vs 20.2 (p2), squad 2 -> exactly one keeps (p1)",
      who_keeps(rnd, 2) == "1", who_keeps(rnd, 2))
do
  claimers({ { 2, "20" } })
  local r1, _, _, _, m1 = rank_of(1, 20.4, 2)
  claimers({ { 1, "20" } })
  local r2, _, _, _, m2 = rank_of(2, 20.2, 2)
  check("rounding: both compare 20 = 20; p1 rank 1, p2 rank 2 (they agree)",
        r1 == 1 and r2 == 2 and m1 == 20 and m2 == 20,
        string.format("p1 r=%s mc=%s p2 r=%s mc=%s", r1, tostring(m1), r2, tostring(m2)))
end
check("rounding: raw 20.6 (p1) vs 20.4 (p2) -> 21 vs 20, both agree p2 keeps",
      who_keeps({ { 1, 20.6 }, { 2, 20.4 } }, 2) == "2", who_keeps({ { 1, 20.6 }, { 2, 20.4 } }, 2))
check("rounding, squad 3: 20.4/20.2/20.3 -> p1,p2 keep, p3 yields",
      who_keeps({ { 1, 20.4 }, { 2, 20.2 }, { 3, 20.3 } }, 3) == "1,2",
      who_keeps({ { 1, 20.4 }, { 2, 20.2 }, { 3, 20.3 } }, 3))
check("unreachable sentinel 1e9 still loses to any real claimer",
      who_keeps({ { 1, 1e9 }, { 2, 20 }, { 3, 30 } }, 3) == "2,3",
      who_keeps({ { 1, 1e9 }, { 2, 20 }, { 3, 30 } }, 3))

-- ── 4. dead ally ignored ────────────────────────────────────────────────
do
  -- p1 died AFTER its last broadcast: its kg/kc lingers but it can't grab.
  claimers({ { 1, "20" }, { 2, "20" }, { 3, "20" } })
  local st = { tank_dead_at = { [1] = NOW + 5 } }
  local r, keep = rank_of(4, 20, 3, st)
  check("dead p1 ignored: p4 behind p2,p3 only -> rank 3, yields",
        r == 3 and keep == 2, string.format("r=%s keep=%s", r, keep))
  r, keep = rank_of(3, 20, 3, st)
  check("dead p1 ignored: p3 ranks 2 and keeps (would be 3 counting the corpse)",
        r == 2 and keep == 2, string.format("r=%s keep=%s", r, keep))
  -- Death BEFORE the last broadcast = it respawned and is claiming again.
  st = { tank_dead_at = { [1] = NOW - 5 } }
  r = rank_of(3, 20, 3, st)
  check("respawned p1 (died before its last broadcast) counts again", r == 3, r)
end
do
  claimers({ { 1, "20" } })
  AS.set_info(2, NOW, { kg = "9", kc = "5" })   -- a claim on a DIFFERENT pill
  local r = rank_of(3, 20, 3)
  check("claimer of another pill is not counted", r == 2, r)
end

-- ── 5. blitz size recorded on the claim (attack.mark_kill_pickup) ──────
print("attack.mark_kill_pickup blitz size")
local function blitz_state(extra_goal)
  local g = { kind = "attack_pill", target_id = PILL, mx = 20, my = 20, _blitz = true }
  if extra_goal then for k, v in pairs(extra_goal) do g[k] = v end end
  return { player_number = 1, tick = NOW, goal = g }
end
C.KILL_PICKUP_PAIR_MIN_SQUAD = 3
do
  AS.init()
  AS.set_info(2, NOW, { goal = "attack_pill", target = tostring(PILL), sqst = "blitz" })
  AS.set_info(3, NOW, { goal = "attack_pill", target = tostring(PILL), sqst = "blitz" })
  AS.set_info(4, NOW, { goal = "attack_pill", target = tostring(PILL), sqst = "nego" })  -- not committed
  AS.set_info(6, NOW, { goal = "attack_pill", target = "7" })                           -- other pill
  local st = blitz_state()
  A.mark_kill_pickup(st, PILL, 20, 20, NOW)
  check("blitz of self + p2 + p3 (p4 negotiating, p6 elsewhere) -> squad_n 3",
        st.kill_pickup and st.kill_pickup.squad_n == 3, st.kill_pickup and st.kill_pickup.squad_n)
  -- Re-stamp after the others moved on to capture_pill: size must not shrink.
  AS.set_info(2, NOW + 10, { goal = "capture_pill", target = tostring(PILL) })
  AS.set_info(3, NOW + 10, { goal = "capture_pill", target = tostring(PILL) })
  A.mark_kill_pickup(st, PILL, 20, 20, NOW + 10)
  check("re-stamp after the squad broke up keeps squad_n 3",
        st.kill_pickup.squad_n == 3, st.kill_pickup.squad_n)
  -- Running max from update_attack_substate survives members dropping off.
  local st2 = blitz_state({ _kp_party_max = 4 })
  A.mark_kill_pickup(st2, PILL, 20, 20, NOW + 10)
  check("running max goal._kp_party_max 4 is used when the live count is lower",
        st2.kill_pickup.squad_n == 4, st2.kill_pickup.squad_n)
  -- Dead ally not counted.
  AS.init()
  AS.set_info(2, NOW, { goal = "attack_pill", target = tostring(PILL), sqst = "blitz" })
  AS.set_info(3, NOW, { goal = "attack_pill", target = tostring(PILL), sqst = "blitz" })
  local st3 = blitz_state()
  st3.tank_dead_at = { [3] = NOW + 1 }
  A.mark_kill_pickup(st3, PILL, 20, 20, NOW)
  check("dead p3 not counted -> squad_n 2", st3.kill_pickup.squad_n == 2, st3.kill_pickup.squad_n)
  -- Solo take (no blitz flag) -> 1 even with allies on the pill.
  local st4 = blitz_state({ _blitz = false })
  A.mark_kill_pickup(st4, PILL, 20, 20, NOW)
  check("solo take (goal._blitz false) -> squad_n 1", st4.kill_pickup.squad_n == 1, st4.kill_pickup.squad_n)
  -- Goal on a different pill -> 1.
  local st5 = blitz_state({ target_id = 9 })
  A.mark_kill_pickup(st5, PILL, 20, 20, NOW)
  check("goal on another pill -> squad_n 1", st5.kill_pickup.squad_n == 1, st5.kill_pickup.squad_n)
  -- Knob off: no count at all.
  C.KILL_PICKUP_PAIR_MIN_SQUAD = 0
  local st6 = blitz_state()
  A.mark_kill_pickup(st6, PILL, 20, 20, NOW)
  check("keel: squad_n 1 (no count taken)", st6.kill_pickup.squad_n == 1, st6.kill_pickup.squad_n)
  C.KILL_PICKUP_PAIR_MIN_SQUAD = 3
end

-- ── 6. handoff_to_capture_pill skip ─────────────────────────────────────
print("attack.handoff_to_capture_pill")
local function handoff_case(squad_n, n_capturing)
  AS.init()
  for i = 1, n_capturing do
    AS.set_info(1 + i, NOW, { goal = "capture_pill", target = tostring(PILL) })
  end
  local st = { player_number = 1, tick = NOW,
               goal = { kind = "none", mx = 20, my = 20, target_id = PILL },
               kill_pickup = { id = PILL, mx = 20, my = 20, kill_tick = NOW, created_tick = NOW,
                               squad_n = squad_n } }
  local world = { pills = { [PILL] = { mx = 20, my = 20, health = 0 } } }
  local info = { player_number = 1 }
  local took = A.handoff_to_capture_pill(st, world, info, PILL, NOW)
  return took, st.goal.kind
end
C.KILL_PICKUP_PAIR_MIN_SQUAD = 3
do
  local took, kind = handoff_case(3, 1)
  check("squad 3, one ally already capturing -> we still take capture_pill",
        took == true and kind == "capture_pill", tostring(took) .. "/" .. tostring(kind))
  took = handoff_case(3, 2)
  check("squad 3, two allies already capturing -> skip", took == false, took)
  took = handoff_case(2, 1)
  check("squad 2, one ally already capturing -> skip", took == false, took)
  took = handoff_case(3, 0)
  check("squad 3, nobody capturing -> take", took == true, took)
end
C.KILL_PICKUP_PAIR_MIN_SQUAD = 0
do
  local took = handoff_case(3, 1)
  check("keel, squad 3, one ally capturing -> skip (old behaviour)", took == false, took)
end

C.KILL_PICKUP_PAIR_MIN_SQUAD = DEFAULT_MIN
check("default knob is 3", C.KILL_PICKUP_PAIR_MIN_SQUAD == 3, C.KILL_PICKUP_PAIR_MIN_SQUAD)
check("PRESETS.keel pins the knob to 0 (off)",
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.KILL_PICKUP_PAIR_MIN_SQUAD == 0,
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.KILL_PICKUP_PAIR_MIN_SQUAD)

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
