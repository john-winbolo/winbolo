# Builder pool — LGM side-quests as a parallel track

Recovered from the 2026-09-01 design discussion (par2 bot2 t=21471 incident).
Status: plan, not implemented. Branch `builder-second-queue`.

## The incident that motivates it

`20260901_160325_1_par2` bot2, t=21071–21661: bot2 places its own blocker p15
at (125,114) for the take on pill #10, p15 dies to return fire at 21471, and
for the next 190 ticks the LGM sits idle in the tank with 13 trees, ~6 tiles
from the dead blocker, while the tank (correctly) finishes the take. At 21661
1.6's bot1 drives over p15 and takes it. A 4-tree LGM repair converts a dead
pill back to a live friendly one — undriveable, race over — and it never had
a chance to run, because **builder dispatch is slaved to the tank's goal**.
The tank goal was right; the architecture lost the pill.

Same shape: `take_cover` could repair a worn pill while it hides; a tank
driving to a standoff could rebuild a dead pill one tile off its route.

## What we already have (reuse, don't rebuild)

- `builder.decide()` is already a priority ladder that partially ignores the
  tank goal: drowning road (P1), slow-terrain road (P1b), road-ahead and
  tree-reserve logic fire on their own. Goal-tied jobs (sea legs P0.3, forced
  repair P0.4, base shield P0.5, trail P2.3, place P2.4, wall shield P2.5,
  gather P3) enter through `set_mode(goal)`.
- Job discovery exists: `REPAIR_DEAD_FILTER` already identifies dead friendly
  pills worth rebuilding (today only a print in the capture eval); the
  repair_pill eval scores damaged pills; `nearest_forest_near` / gather knows
  harvesting.
- Feasibility/safety plumbing exists: `lgm_can_reach`, `lgm_path_safe` with
  the LGM_DANGER tiers, the stranded/rescue machinery as backstop,
  `_place_trip`-style trip tracking, `wait_for_lgm` + `pick_wait_spot`, the
  new under-fire hold (`REPAIR_HOLD_UNDER_FIRE_ENABLED`, `last_hit_tick`).
- Claims: `/info extra` already carries `lgmd=XXYYEEEE` (LGM dispatched to
  tile, ETA); `HEAT_GATE … BLOCK ally_repair` already consumes it.

## Design

**Pool, not queue**: scored candidates compete every tick like the goal
pool; the winner (if any) gets the one LGM. The pool is a *different arbiter
for a different resource* (the man, not the tank).

1. **Side-quest candidates** (the only true pool rows):
   - rebuild a dead friendly pill within the leash (value: front-distance
     clock — the influence map gives "front distance" for free; a dead pill
     at the contact line is ticking, one deep in our rear can wait);
   - top up a damaged friendly pill within the leash;
   - opportunistic farm.
   Dead-pill rebuild outranks farm always (farm has no clock). Scored like a
   mini-pool: value, trip ETA (reuse the wall-shield LGM walk-time math),
   danger. Deterministic ordering.

2. **One new rung in `decide()` between P1b and P2.** "Suppressed" still
   wins. The rung fires only when the builder mode is idle-ish
   (`none` / `opportunistic`), never when a goal-tied mode is active — the
   pill take's wall building does not negotiate with the pool, it pre-empts
   it by mode, exactly as today.

3. **Eligibility stack** (all existing signals, every deny logged with its
   reason):
   - mode idle-ish, OR the tank goal is in a **travel-class** substate
     (`plan_position`, `approach`, `gather_trees`, capture drives, refuel,
     defend-watch, take_cover). **Fire-exchange substates deny**
     (`shoot_pill`, `engage`, `charge`, `swerve`, `kill_hardline`, in-range
     aims): shells detonating on/near the tank splash the LGM at departure
     and return — his risky moments are the two ends of the trip.
   - tank not under fire recently (danger.lua signals: armour drop within
     N ticks, incoming shells near the tank) — layered on top of the
     substate class, so a take_cover tank being shelled doesn't send him
     either.
   - `reserve_eta` fits: the goal declares when it will need the man
     (`b.reserve_eta`, e.g. approach distance ÷ speed when walls are planned
     at the standoff); a side-quest launches only if round-trip + margin fits
     inside. One number, one comparison.
   - `lgm_path_safe` for the trip at the repair danger threshold; never
     while our own shot lane crosses his path.
   - tree reserve: never spend below what the active/imminent goal needs
     (same as `road_tree_reserve` / the sea plan's reserve).

4. **Return leg**: if the tank comes under fire while the man walks home,
   hold the pickup *away* from the fight (`pick_wait_spot`'s danger-aware
   spot) rather than dragging the rendezvous through the shell zone.

5. **Collision** (a goal suddenly needs the man mid-side-quest, e.g. a blitz
   fires): lean on existing fallbacks — `gather_trees` stall/timeout demotes
   a PPT take to no-shield, `wait_for_lgm` bids when waiting is right, the
   stranded machinery recovers him if the tank must leave. Short leash
   (~8 tiles) + ETA cap bound the worst case to a few seconds. No recall
   logic (the engine cannot recall a walking LGM anyway).

6. **Multi-bot arbitration** (5 allies must not all repair the same pill):
   - claim = (job type, target tile/pill id, claim tick, ETA) on the existing
     `/info extra` channel (extend `lgmd`);
   - earlier claim tick wins; same-tick race breaks to the lower player
     number (the blitz commander / standoff-spot rule);
   - double-dispatch in the same tick: the deterministic loser recalls (cheap
     two steps out of the hatch); claims expire on `SQUAD_ALLY_MAX_AGE` or
     job completion;
   - a live claim also removes the pill from allies' "dead pill needing
     rescue" discovery (claim covers the outcome, not just the trip).
   - loser's row shows `REJECT ally_repairing (p2 eta 140t)`.

7. **repair_pill splits by whether the tank must move**:
   - in-leash repair → the builder pool's job; the tank-goal `repair_pill`
     row shows `REJECT builder_can (leash 8, eta 140)`;
   - out-of-leash → stays a tank goal meaning "relocate so the repair becomes
     leash-reachable"; on arrival it does not dispatch itself, it **seeds
     the pool** with a top-priority claim. One executor, two feeders; one job
     id cross-referenced by both rows. (The new defend→repair handoff is the
     same shape and should become a feeder too.)

8. **Walls / placement / sea legs are NOT pool rows.** They are goal-owned
   work with no existence outside their goal; the pool never decides them.

## Panel (BrainTest)

A new strip section, not new pools (pool numbering unchanged so old
recordings load):

```
BUILDER  owner=attack_pill/wall_shield  (walls 3/8, man out, eta 60)
  eligibility: mode=idle-ish · under_fire=no(38t) · reserve_eta=110 · trees 14(res 8)
  sidequest rebuild p15@(125,114)   REJECT mode_owned (wall_shield)
  sidequest topup  #4@(123,131)     REJECT mode_owned (wall_shield)
  active: rebuild p15 (outbound, eta 44t)
```

- owner line always present (`owner=none (idle)`, `owner=take_cover
  (idle-ish)`, …);
- goal-owned jobs listed under the owner when their goal exists, with
  progress;
- side-quests follow the always-show rule with REJECT reasons;
- rows use the existing renderer (cost, formula short‖long) — same as the
  take_cover strip.

## Worked examples (from real sessions)

- **Stolen blocker (par2 bot2 t=21471)**: candidate *rebuild p15* files at
  21471 (value high, trip ~6 tiles, trees 13 ≥ 4). Denied through
  `shoot_pill` (21471) and `swerve` (21496) — fire-exchange. At 21575
  (`plan_position` for the next take, not under fire 60+ ticks, no
  reservation) it dispatches; p15 is a live friendly pill again by ≈21720,
  before 1.6's bot1 arrives at 21661+drive. If the next take had been close
  (`reserve_eta` 80 < trip 250) the pool holds and p15 is probably lost —
  the reservation protecting the take's walls.
- **take_cover with a repair on the side (bot3 t≈17932)**: tank at cover,
  expo 0, no hits for N ticks; pool holds *top-up #4* (9/15, leash 8) →
  dispatch; the LGM repairs while the tank sits where take_cover wanted it.
- **Pill take with a dead pill near the route**: `plan_position`, walls
  planned at arrival in 110 ticks → `reserve_eta=110`; a 180-tick rebuild is
  `deferred (reserve 110 < trip 180)`; a 70-tick one dispatches and is home
  before the walls. `gather`/`wall_shield` modes: pool not consulted.
  `shoot_pill`/`swerve`: locked out on two grounds. Post-kill capture drive:
  the 180-tick rebuild dispatches. Tree ledger: walls reserve 8 of 14, the
  4-tree job is affordable only because 14 ≥ 8 + 4.

## Open choices (author to confirm)

- leash radius: start at 8;
- side-quests during `attack_pill` shooting substates: **no** (fire-exchange
  denies), during travel substates: yes, gated by `reserve_eta`;
- dead-pill rebuild outranks farm: yes;
- should a side-quest ever delay a goal transition (tank waits ~30 ticks at a
  swerve exit for the man) — proposed: no, rely on stranded/rescue;
- whether the engine's second request slot (`lgmAddRequest` queues one
  *next* job while the man is out) should be used to chain jobs (e.g.
  harvest → place, or side-quest → goal job) — separate item; it has no
  readback and no cancel, so it needs its own accounting.

## Verification

- Scenario test: the stolen-blocker shape — our dead pill near the contact
  line, tank busy on a take, LGM idle with trees; assert the rebuild
  dispatches in the first travel-class window after the fire-exchange, the
  pill is friendly again, and no dispatch happened during `shoot_pill`/`swerve`.
- Scenario test: take_cover + worn pill within leash → repair while covering;
  same arena with shells landing on the tank → denied `under_fire`.
- Scenario test: two allied bots, one dead pill → exactly one dispatch, the
  other shows `REJECT ally_repairing`.
- Reservation test: dead pill off a take route; assert `deferred (reserve …)`
  when the trip does not fit and dispatch when it does.
- Aggregates after a 1.6-vs-1.7 match: side-quest dispatches by job type,
  denials by reason, dead friendly pills lost to enemy drive-over
  (should fall), LGM deaths on side-quests (should be ~0).
