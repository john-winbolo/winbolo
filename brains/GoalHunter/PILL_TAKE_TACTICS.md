# Blitz — coordinated simultaneous take (design)

Status: **design only — not implemented.** Companion to
[`SQUAD_COORDINATION_PLAN.md`](./SQUAD_COORDINATION_PLAN.md).

**Blitz** = N bots holding at setup and **charging one target on a common
`engage_tick`** so its one-target-per-reload fire is swamped. The coordination
(recruit → self-claim spots → hold → synchronized charge) is **target-agnostic**;
only the per-bot action differs:

- **Pill blitz** (`tac="blitz"`, target = pill) — **Phase 1, this doc.** Each bot
  runs the standard **non-PPT pill take** unchanged; the only additions are
  coordinated charge timing + collision-free spot assignment. No new
  firing/aim/charge logic.
- **Base blitz** (`tac="blitz"`, target = base) — **sibling, later.** Same
  recruit/claim/hold/synchronized-charge machinery, but the per-bot action is
  **`capture_base`** instead of the pill engage. Useful to rush a contested /
  about-to-be-defended base with overwhelming simultaneous arrival. Reuses
  everything below except the spot model (base capture wants tiles ON the base /
  its approach, not a ~7.4t firing standoff).

> **Phase 1 = PILL BLITZ only.** In scope: recruitment, self-claim spots
> (chunked/jittered/validated), hold at setup, synchronized charge on a common
> `engage_tick` (quorum 2 + timeout), then the existing non-PPT engage. Plus the
> squads panel + per-stage overlays. **Out of Phase 1:** base blitz, and the
> Blocking (decoy) tactic + its cover states.

---

## Mechanics reference (verified in sim)

- **Pill targeting** (`pillbox.c:358-404`): each reload the pill fires at the
  **closest in-range non-allied tank**, **regardless of line-of-sight**.
- **Pill range** = `PILLBOX_RANGE` = 2048 WU = **8 tiles** (`pillbox.h:51`).
- **Tank hit range** ≈ `ATTACK_PILL_STANDOFF` = **7.4 tiles** (shell travel =
  `GUNSIGHT_MAX`/2). `ATTACK_PILL_RANGE` (9.5) is only a "start considering"
  threshold. ⚠ **The pill (8t) out-ranges the tank (7.4t)** — there is no
  shoot-but-can't-be-hit position; every striker must enter pill range to fire.
- **Shell vs terrain** (`shells.c:741`, `bolo_map.c:824`): a shell stops at the
  first non-passable tile — BUILDING, HALFBUILDING, **FOREST**, pills. A tree
  eats one shell (→grass); the next shell continues. (`cpf.simulate_shot` walks
  *through* terrain and over-reports reach — don't trust it alone.)
- **Anger**: more hits → faster pill fire (speed halves per hit, floor 6 vs
  normal 100); decays via cooldown.

**Why coordination wins:** the pill targets only ONE tank per reload. If N bots
open fire together, it kills at most one before combined DPS drops it; trickle-in
lets it pick attackers off one at a time.

---

## The model

Each participating bot runs the **normal non-PPT take** unchanged:

```
plan_position → approach → aim → charge → engage → shoot_pill → swerve
```

Three additions layer on top:

### 1. Self-assigned, collision-free spots
Decentralized — the commander does **not** solve spot placement. It only
broadcasts the **target pill + its own position**; each soldier picks its own
spot. Simpler, no central partition.

- Each soldier runs the **existing standoff scan** (the ellipse danger / LOS
  scoring already used by `plan_position`) and takes the **pool of top-scoring
  "winning range" spots** (clear LOS, ~7.4t, good score).
- It picks one **at random from that top pool** — random so members spread out
  instead of all converging on the single best spot.
- **Claim jitter**: wait a **random 0–5 ticks** before broadcasting the claim.
  Cheap (~0.1s) and it de-syncs simultaneous picks so two bots rarely claim the
  same/adjacent spot on the same tick.
- Before committing, reject any spot within **~1 tile** of another member's
  active claim; on collision re-pick from the pool. Deterministic tie-break
  (lower `player_number` keeps the spot, the other re-picks).

**Candidate expansion (per ellipse spot).** A single ellipse spot is too coarse
when several bots pack in, so each soldier expands each base engage spot into a
small candidate set and claims the first valid one:
- **Lateral**: up to **±2 tiles left/right** of the engage spot (perpendicular to
  the pill→spot line), in **0.5-tile steps** (0, ±0.5, ±1.0, ±1.5, ±2.0).
- **Closer**: also try **one step closer** to the pill — same "nudge in slightly,
  then re-test the position" technique the PPT engage-position search uses
  (`ATTACK_PILL_STANDOFF` → `PPT_STANDOFF`).

**Validate each candidate** (must pass both):
1. **Clear shot to the pill** — a clear shell lane to the pill, checking **all 4
   corners** of the pill tile (so a candidate slightly off-axis still qualifies
   if any corner is hittable). Reuse `pill_shots_clear` / `wall_hp_between`.
2. **Clear charge lane setup → engage** — project the **setup point** behind the
   engage spot along the pill→spot line (standoff + `ATTACK_APPROACH_OFFSET`,
   ≈9.65t, outside pill range; same projection the solo take uses), then verify a
   clear drivable path from setup to engage. The tank holds at setup and
   **charges straight in** to engage, so that lane must be unobstructed.

The setup point inherits the same ~1-tile claim spacing.

**Chunking (lower-tier bots).** The base ellipse scan already reuses
`plan_position`'s chunked sweep; the **candidate expansion + per-candidate
validation** (4-corner shot check + setup→engage charge-lane path) is extra work
on top, so spread it the same way — evaluate a slice of candidates per tick,
accumulate in per-pill progress state, and only claim once a valid candidate is
found (or the set is exhausted). **Taking ~1 s (≈50 ticks) to settle on a claim
is fine** — the soldier just holds (or finishes approaching the pill area) while
deciding; no movement commitment until a spot is claimed.

### 2. Hold at setup
- A bot reaching its setup point **holds** there (untargeted, outside pill range)
  instead of charging, and reports `rdy=1`. This is the only new substate gate —
  pause the existing `approach → charge` transition until release.

### 3. Coordinated release
- Commander releases when **all assigned bots are `rdy`**, OR **(timeout &&
  rdy_count ≥ 2)** — go with two if a third is slow; the laggard joins late or is
  dropped from this take.
- Release = broadcast a shared `engage_tick` (now + small offset to absorb the
  1-tick broadcast latency). Every ready bot does its normal `charge` on that
  tick, so they cross into range and fire the same frame.

### Abort
- Pill dead → done. Commander dead, or member losses over threshold → abort:
  survivors fall back to a solo take or retreat.

---

## Blocking (decoy) — NEW states

A blocker holds the **closest** position to the pill so it stays the pill's
target, but stands **behind cover** (building / wall / rubble) on the pill→tank
line so the shells hit the cover, not the tank. As that cover erodes
(building → rubble → gone), it **slides** to the next covered position served by
a *different* blocker. Meanwhile the strikers (farther, untargeted-relative-to
-decoy) fire freely.

**Best when:** existing rubble/walls already sit close to the pill (within ~4
tiles), so we get durable cover for free without building.

### Cover-path search
Goal: a chain of **4–5 connected, drivable positions, each shielded by as many
*different* blockers as possible**, so the decoy migrates along it and a fresh
blocker is always on the pill→decoy line as old ones erode.

1. **Candidate cover tiles** — scan tiles within radius ~4 of the pill. A tile
   qualifies if drivable AND `wall_hp_between(pill_center, tile) > 0` (a blocker
   intercepts the shell line). Record the **intercepting blocker tile(s)** on
   that line and the **blocker HP** (durability).
2. **Build chains** — from each candidate, greedily extend to walkable, adjacent
   candidate tiles, **preferring neighbors covered by a blocker not yet used in
   this chain**. Stop at length 4–5. Generate a handful of chains (different
   seeds) rather than one.
3. **Score each chain** (lower cost = better):
   - `+` distinct blockers covering it (resilience — the key term),
   - `+` total blocker HP along the chain (how long cover lasts),
   - `−` if any tile isn't the *closest* squad position to the pill (must hold
     aggro),
   - `−` poor connectivity / slow terrain on the slide path,
   - `−` lateral gaps (the slide must be short, adjacent hops).
4. Pick the best chain; the decoy enters at its **closest-to-pill** end.

### New states
```
plan_cover     — run the (chunked) cover-path search → produce the chain
approach_cover — drive to the chain's entry (closest-to-pill) tile
hold_cover     — sit as the closest target; poll wall_hp_between(pill, here)
slide_cover    — BEFORE cover fails (HP trending to 0 / predicted hits-to-break),
                 hop to the next chain tile (different blocker on the line)
abort          — chain exhausted / can't stay covered / decoy dead / pill dead
```
Slide trigger: `wall_hp_between(pill, here)` below a threshold OR predicted
hits-to-break within a lead window; pre-stage the next tile so the hop is one
adjacent move with no exposed gap.

### Chunking (lower-tier bots)
The radius-4 scan is ~80 tiles, each needing a `wall_hp_between` LOS trace, plus
chain-building — too heavy for one tick on a low budget. **Chunk it like
`plan_position`'s angle sweep** (`advance_pill_eval_chunk` /
`state._pill_eval_progress[pid]`): evaluate a slice of candidate tiles per tick,
accumulate into per-pill progress state, and only build/score chains once the
scan completes. Cache the chosen chain per pill (short TTL); re-run only on a big
cover change. `plan_cover` holds (no movement) until the chunked scan finishes.

---

## Requesting help (commander) & joining (soldier)

**Squad lifecycle is ephemeral.** Roles (commander / soldier / harasser) are
fixed at game start (`SQUAD_COORDINATION_PLAN.md`), but **squad membership exists
only for the duration of a blitz**:
- **Only commanders initiate blitzes.** Soldiers and harassers never start one.
- A soldier not in an active blitz just **does its own thing** — normal solo goal
  selection, no commander.
- When it accepts a recruit it **joins that commander's squad for the blitz**,
  follows the blitz flow, then on blitz end/abort **returns to solo** behavior.
- "Already has a commander" in the decline rules below therefore means **already
  committed to an active blitz** (don't abandon one blitz to join another).

**Commander — ask on every pill take.** A commander committing to a pill take
broadcasts a `help_request` (target pill + its position) to **nearby** soldiers
and opens a **recruitment window (~10 ticks)**. Asking is free, so it always
asks — no "should I ask" heuristic.

- **Brief stall to collect responses:** the commander **holds for ~10 ticks** to
  gather which nearby soldiers are answering before committing. ~0.2 s — cheap. It
  may keep drifting toward the pill area but does **not** lock its standoff yet, so
  the squad can self-claim spots together. (Better than charging ahead and having
  to re-coordinate when recruits arrive mid-approach.)
- **≥1 soldier accepts within the window** → convert to a blitz: it's now a
  squad, proceed to the self-claim / hold-at-setup / synchronized-release flow
  (quorum = 2 = the commander + 1).
- **Nobody accepts within the window** → drop the blitz and **do the solo take
  exactly as the code does today.** ~10 ticks lost, nothing else.

**Soldier — whether to respond.** Evaluated against the request:

- **Hard declines (never help), regardless of state:**
  - **Low health** (below a help threshold), or
  - **Low / no ammo** (can't contribute fire).
- **Answerable from these INTERRUPTIBLE states** (drop it to join the blitz):
  - **no goal / exploring / plain navigation / `reposition`**.
  - **own solo `attack_pill` in `plan_position`, `approach`, or `build_walls`**
    (not yet aiming/charging) → abandon and join.
  - **already attacking the SAME pill** (`attack_pill` on the blitz target, any
    substate) → answer and **converge** — two solo takes on one pill merge.
  - **`refuel_at_base` only if NOT desperate** — shells ≥ min AND armour ≥ min
    (it was topping off, not actually low; it can spare the trip).
  - **`place_pill_strategic`**, **`capture_base`**, **`attack_base`**,
    **`attack_tank`** → flexible objectives worth dropping for a blitz.
- **Declines from a hard-committed objective** (`decline reason = busy`):
  `capture_pill` / `repair_pill` / `defend_pill` / `kill_lgm` /
  `wait_for_lgm` / `flee`; **`refuel_at_base` when below min shells/armour**
  (genuinely needs it); **or** an `attack_pill` already past `approach`
  (aim/charge/engage) on a *different* pill — don't abandon a take that's
  already firing.
- **Already in a squad** (committed to an active blitz):
  - **Default: stay.** Don't dissolve your squad to chase a request.
  - **Exception — large squad can spare one:** if the squad size ≥ a threshold,
    **exactly one** member may peel off to help. Pick deterministically — the
    **highest-health** eligible member (tie-break lower `player_number`) — so
    only that one responds and the squad isn't gutted.

Net effect: requests pull in idle/free bots first, and only skim a single
healthy spare from a big squad; small squads and wounded/dry bots stay put.

## Integration points

- **Squad-target exemption**: assigned bots skip `ally_claimed` + ally-avoid
  **for the target pill only**, so they converge instead of repelling.
- **Self-claim**: each soldier reuses the existing standoff scan's top-pool, picks
  a random spot, and broadcasts a `claim`; others honor a ~1-tile no-overlap
  radius (deterministic tie-break). No central spot solver.
- **Role designation**: commander picks **blocking** when the cover-path search
  finds a usable chain near the pill (existing cover within ~4t); otherwise plain
  shared take. One bot gets the decoy role + chain; the rest self-claim striker
  standoffs *farther from the pill than the decoy's chain*.
- **Broadcast keys**: `help_request` (commander id + target pill), `target/mx/my`,
  `role` (decoy/striker), `claim` (chosen standoff mx/my), `rdy`, `engage_tick`.

## Visualizers

Every coordination decision is invisible without overlays — build one per stage,
each a registered, toggleable `viz.*` overlay (debug-only, stripped from `opt/`),
colored **per player** where multiple bots are shown so you can tell who's who.

### Squads panel (roster HUD)
A persistent roster anchored to the **middle-right** of the screen, one block per
active squad:

```
C3   <overall command status>...............(≥100px text area)
  S1   <soldier status / decline reason>.....(≥100px text area)
  S5   <soldier status>
C7   <overall command status>
  S2   <soldier status>
```

- **Commander** row: tank # in **RED**. The ≥100px area to its right shows the
  **overall command status** — tactic, target pill coord, readiness (`rdy 2/3`),
  and `engage_tick` countdown.
- **Soldier** rows (indented under their commander): tank # in **BLUE**. The
  ≥100px area to its right shows **that soldier's status** — `claiming` /
  `jitter Nt` / `rdy` / `charging` / `engaging`, or its **decline reason**
  (`no_ammo` / `low_hp` / `in_squad` / `busy`) if it was asked but didn't join.
- **Harasser** rows: the independent base-stealers from
  `SQUAD_COORDINATION_PLAN.md §7` (no commander — deep enemy-base steals /
  rear attacks). Shown as their own **top-level** yellow rows (NOT indented
  under any commander), tank # in **YELLOW**, status = current steal target /
  state (e.g. `base@(x,y)` / `infiltrating` / `evading`). Group them under a
  small "Harassers" header below the squads.
- Reserve **~100 px to the right of every tank #** for text so nothing clips.
- Role colors: **commander = red, soldier = blue, harasser = yellow.**

- **Recruitment** — commander→pill request line + "REQUEST" tag; a line from each
  responding soldier to the commander; **declines with reason** floating on the
  decliner (`low_hp` / `no_ammo` / `in_squad` / `busy`); "1 of N spared" tag on
  the large-squad member that peels off.
- **Role** — `DECOY` / `STRIKER` label above each participant.
- **Self-claim** — the ellipse top-pool spots (dim); the **expanded candidates**
  (lateral ±2 / step-closer) per soldier; per-candidate validity coloring
  (green = both checks pass, red = no clear shot, orange = blocked charge lane);
  the **claimed** spot (solid, player color) + its **~1-tile claim radius** ring;
  a small **claim-jitter countdown** while waiting to broadcast.
- **Shot/charge checks** — the **4 corner shot lanes** to the pill on the
  candidate under test; the **setup→engage charge lane** (green clear / red
  blocked) with the projected setup point marked.
- **Readiness / release** — `rdy` state per bot, the **quorum count** (e.g. 2/3),
  and an **`engage_tick` countdown** so you can see the synchronized fire coming.
- **Blocking** — the full **cover chain** (4–5 tiles, numbered in slide order);
  for each chain tile the **covering blocker** highlighted + its **HP**; the live
  **pill→decoy line** with `wall_hp_between` value + erosion trend; the
  **slide trigger** state and the **pre-staged next tile**.
- **Chunking** — a progress indicator (cursor / "k/N candidates") for each
  chunked scan (self-claim and cover-path), so a long search reads as "working,"
  not "stuck."
- **Abort** — the abort reason floated on the bot when a take collapses
  (pill dead / cover exhausted / losses over threshold / commander dead).

## Open questions

- Exact `engage_tick` lead (1–2 ticks) to mask latency without a visible stall.
- Member-loss threshold that converts the take to retreat.
- Spacing margin: is ~1 tile enough given tank hitbox + jostle, or wider?
- Recruitment window length (~10 ticks default) — long enough for a nearby
  soldier to answer + broadcast latency, short enough not to stall the take.
- During the window the commander holds (doesn't lock its standoff). Confirm
  ~10t is enough for 1-tick broadcast round-trips at the recruit distance.
- Help thresholds: min health + min ammo to respond; "nearby" radius (how far is
  worth abandoning current drift to help).
- Large-squad size threshold above which exactly one member may peel off.
