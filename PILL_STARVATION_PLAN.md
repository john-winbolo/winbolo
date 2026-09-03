# Pill-starvation fixes — plan for review (2026-09-03)

Source incident: `debug_sessions/20260903_105448_1_refuel3` bot2, ticks 34774–36670
(DH-Oil Rig, seed 4242, 1.6 vs 1.7). Bot2 stood next to three damaged friendly
pills with 4 pills in the tank and 21 trees, repaired nothing, placed nothing,
built no blocker, and died carrying all four. 1.7 went from 13–3 pills at
minute 14 to 1–15 at minute 30.

Three fixes, in order. Each is a behaviour change and goes in only on approval.
Fix 4 (attack demote-early + builder bail logging) was considered and dropped.

---

## What the recording proved

| Symptom | Cause |
|---|---|
| defend_pill rows all at 40, no repair | The 40s ARE repair bids (defend's arrival handoff; `HEAT_GATE … REPAIR BID 40`). The repair feeder said `can=true` every tick. The builder pool refused the 1-tree job: `BP_DENY … tree_reserve(41,21,1)` — reserve 41 = base 4 + carried pills 16 + **sea plan 21**, against 21 trees. |
| 4 pills never placed | The carry pressure worked (pool-8 row cost **1.0**, discount maxed at −300). goal_selection then pinned it to `attack_tank cost + 1` = 2163 because "placement must never out-rank attack_tank" — against an attack_tank row for an enemy across water at cost 2162. |
| No blocker during attack_pill #9 | The wall was planned twice (`build_walls`, 251 ticks each, wall at (117,114) then (117,113)), but the builder ladder bailed every tick at its sea-harvest rung (`sea_trees_reserved`: live sea plan and trees ≤ its need) before reaching the wall rung. |
| Why a sea plan at all | A deep-sea pill harvest was planned at ~t=15000. Its LGM died at t=28997. The plan (`state._sea_live`) was never retired and reserved 21 trees for the rest of the game. |

---

## Fix 1 — the sea-harvest plan and its tree reserve end with the goal

**Rule.** The sea plan (`_sea_live`: mine → boat → sail out) exists only while the
tank's current goal is that sea capture_pill. The tick the goal is anything
else, the plan is dropped and its tree reserve is 0. No grace window (T = 0).
It is also dropped the moment its LGM dies. A later harvest re-plans from the
map (mine and boat are visible terrain; the route scan is cached and cheap).

**Why T = 0.** The plan lives on `state` only so that the goal *object* being
rebuilt every replan does not restart the plan mid-harvest. That is a
same-goal concern. Keeping the plan while the tank does something else buys a
smoother resume after a refuel detour and costs 21 frozen trees for as long
as the detour lasts — which in this game was 20 minutes.

**Where.** `goals.lua` sea substate machine (the existing release path that
already clears `_sea_live`, `_sea`, `_sea_nogo` and blacklists the entrance
is reused; nothing new is invented). The builder ladder's sea rung and
`builder_pool.tree_reserve` read the plan and so follow automatically.

**Test.** New scenario `sea_plan_release_test.py`: boat-only sea pill so the
plan forms (reuse the sea_pills arena), then a scripted enemy kills the LGM
away from the entrance / the sidecar damages a friendly pill next to the
tank so defend→repair wins. Assert: the `BUILDER_POOL` line's `sea` term is
0 on the first tick the goal is not the harvest; the repair dispatches; and
a second run where the tank is left alone still harvests (no regression).

**Effect on the incident.** Reserve 41 → 20. Repairs and walls affordable.

---

## Fix 2 — the tree reserve stops starving the tank's own jobs

**Rules.**
1. Carried-pill reserve = **one placement's worth (4 trees)** regardless of
   how many pills are aboard. Getting one pill out is the priority; the next
   one triggers its own gather, and ambient farming usually covers it.
2. A **seeded** job — one the tank's own goal handed to the pool
   (defend→repair, repair_pill, wall shield) — needs only its own tree cost.
   The reserve is for opportunistic side-quests only.
3. `place_pill_strategic`'s own "enough trees?" gate asks for 4, not
   4 × carried pills, so a tank with 4 pills and 5 trees still goes and drops
   one.

**Where.** `builder_pool.lua` (`tree_reserve`, the deny check that produced
`BP_DENY … tree_reserve`), and the placement tree gate in `goals.lua` /
`builder.lua`.

**Test.** Extend `defend_repair_test.py` (or a sibling): tank starts with 4
pills and 5 trees, damaged friendly pill beside it → the top-up dispatches;
then with 8 trees the placement proceeds. Existing `builder_pool_test.py`
variants must still pass (they assert reserve behaviour for opportunistic
jobs).

**Effect on the incident.** Reserve at t=34774 would read base 4 + pills 4 +
sea 0 = 8 against 21 trees.

---

## Fix 3 — placement is pinned under attack_tank only when an enemy is in shooting range

**Rule.** The goal_selection rule "place_pill_strategic must never out-rank
attack_tank" applies only when a hostile tank is within shooting range of
our tank (new constant, default = gun range, 7 tiles). Outside that range
placement competes on its own cost, so the carry pressure decides. The
forced emergency build (`_place_forced`) stays exempt as today.

**Why range, not "winnable".** The intent of the pin is "fight first when the
fight is imminent". An enemy 15 tiles away is 5–8 s off; a placement is a
2–3 s LGM trip. attack_tank's own viable/rejected notion already called the
2162-cost row viable, so any cost or odds threshold would have to be tuned
and would drift with attack_tank's pricing. Distance is known every tick,
cheap, deterministic, and one constant to tune if an enemy "just outside
range and closing" ever catches the man out — attack_tank's engage logic
and the existing LGM recall cover that case.

**Where.** `goals.lua` goal_selection, the block that sets
`place_entry.cost = at_cost + 1`; one new constant in `constants.lua`.

**Test.** New scenario `place_pin_range_test.py` (reuse the refuel_lowstock
island arena: unreachable patrol tank across a moat): tank carrying 3 pills,
enemy at ~15 tiles → a placement happens within the carry-pressure window
(place row wins within N replans, pill count on the ground +1). Variant B:
enemy driven to ≤ 6 tiles → attack_tank (or the forced build) wins and no
opportunistic placement starts.

**Effect on the incident.** At t=34774 the cost-1 placement wins the next
replan.

---

## Verification after all three

1. Full scenario suite (`-asap`), expected 17/18 + the new tests
   (aim_test is the pre-existing failure).
2. Seed-4242 30-minute 1.6 vs 1.7 game, per-minute table against this run
   (turn at minute 14, 1–15 pills at 30), plus the death-pattern count
   (31 deaths on land, last goal refuel/flee). Success looks like: pills
   placed instead of carried (carry ≤ 2 most of the game), repairs happening
   near the tank, and no four-pill deaths. Score is reported, not promised.
3. Commit each fix separately, PIGEON together.

## Not touched

defend_pill's close-range logic (it did the right thing: repair bids), refuel
costing, builder-pool arbitration beyond the reserve rule, attack_pill's
wall timing (Fix 4, dropped).

## Open questions for review

- Fix 1: drop the plan record at T = 0 (proposed) vs keep a short grace
  window with reserve 0. I propose T = 0.
- Fix 3: gate radius = gun range (7). Say if you want the engage distance or
  a small buffer (9–10) instead.
