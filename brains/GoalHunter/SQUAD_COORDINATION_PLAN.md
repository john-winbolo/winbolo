# Squad Coordination Plan (commander / soldier / harasser)

Status: **design only — not implemented.** Branch: `bot-improvements-2026-05-30`.

Goal: let bots on a team coordinate shared pill takes via roles and squads,
using two team tactics (**swarm** and **decoy**), and divert surplus bots to
**harassing** behind enemy lines.

---

## 0. Core insight

The brain today is built to **anti-coordinate**:

- `ally_claimed` penalty (10000, `goals.lua:4150`) stops two bots attacking the
  same pill.
- ally-avoid overlay (`ALLY_AVOID_COST` 800 over a 5×5, `constants.lua:194`)
  physically pushes bots apart.

This feature is the **inverse, scoped to a squad**: squadmates must be allowed
to converge on one designated target, while bots in *other* squads keep
de-conflicting normally. The central rule:

> Squad membership exempts you from the de-confliction that otherwise keeps
> bots apart — **but only for your squad's assigned target.**

Comms substrate is sufficient — no new transport. Bot-to-bot is the
`/info state` + `/info extra` chat protocol (team-scoped, 1-tick latency,
~128-byte limit), parsed into `ally_state`. The reserved `help` key
(`comms.lua:32`) is already earmarked. **Bots-only falls out for free**: only
bots speak this protocol, so humans never appear as squad candidates.

---

## 1. Decisions (locked)

- **Roles**: `commander` / `soldier` / `harasser`. Assigned at game start,
  persistent — with one exception (death/promotion below).
- **Harassers**: a **third fixed role**, ~**20%** of the bot team (floor),
  chosen deterministically. Act independently (no commander).
- **Commander death**: the squad **promotes** its senior surviving soldier to
  commander. The dead commander **respawns as a soldier**. (So roles are fixed
  *except* through death-promotion.)
- **Squad size is adaptive to the situation**: clustered/"squished" teams →
  bigger squads (concentrate force); spread-out teams → smaller squads / more
  independent action.
- **Humans ignored**: squads form among bots only; humans remain independent
  allies handled by the existing de-confliction.

---

## 2. Architecture (layers)

1. **Role layer** (`roles.lua`, new) — deterministic role allocation + death
   promotion. Every bot computes the same assignment from shared state.
2. **Squad layer** — soldiers attach to a nearby commander with a free slot;
   yields `{commander, members[]}`. Hysteresis to prevent flapping.
3. **Order layer** — commander selects the squad objective (which pill) and
   tactic (swarm/decoy), and drives the engagement phase signal.
4. **Execution layer** — soldiers turn orders into goals; `goals.lua` /
   `attack.lua` gain squad-aware exemptions and tactic-specific positioning.

```
roles.lua ── role + squad assignment ──► broadcast (/info state keys)
                                           │
ally_state ◄── teammates' role/cmdr/rdy ──┘
   │
goals.lua ── squad-target exemption (skip ally_claimed/ally-avoid vs squadmates)
attack.lua ── tactic positioning (swarm angles / decoy aggro-pull + wall shield)
```

---

## 3. Role allocation (deterministic, shared)

All bots must reach the **same** assignment without a central authority, so it
must be a pure function of shared state.

- **Bot set** = self ∪ `ally_state` slots actively broadcasting our protocol
  (humans excluded automatically). Sort by `player_number` (every bot agrees).
- **Harassers**: `floor(0.20 * N)`, picked deterministically (e.g. highest
  player numbers) so the pick is stable as the set settles.
- **Commanders**: from the remaining 80%, `ceil(rest / BASELINE_SQUAD_SIZE)`
  commanders (baseline ~3). Remainder are **soldiers**.
- **Bootstrapping**: bots come online over a few ticks; gate allocation with
  hysteresis and a short settle window so roles don't churn at spawn.

### Death & promotion
- Commander death detected via `ally_state` staleness / death signal.
- Deterministic promotion: **lowest-player-number surviving soldier** in that
  squad becomes commander and broadcasts `role=c`.
- Respawned ex-commander returns as `soldier`.
- Race tolerance: brief (1–2 tick) ambiguity resolves as broadcasts propagate;
  the deterministic rule means all bots converge on the same promotion.

---

## 4. Squad formation (dynamic attachment)

- Each **soldier** follows the **nearest commander** that is (a) within
  `MAX_HELP_RANGE` and (b) has a free slot (`members < adaptive_max_size`).
- **Hysteresis**: don't switch commander unless meaningfully closer, or current
  commander died / went out of range. Prevents soldiers ping-ponging.
- **Adaptive max squad size**: base ~3, raised when local ally density is high
  ("squished in a tight area" → concentrate), normal/low when spread out. Key
  off local ally count within a radius and/or front-line proximity.
- A soldier with **no commander in range** acts solo (falls back to normal goal
  logic) and/or moves toward the front to find a squad.
- A commander with **no soldiers** simply behaves like a solo attacker.

---

## 5. Coordination protocol (new broadcast keys)

Added to the existing `/info state` (fast-changing) and `/info extra` (heavier)
payloads. Keep within the ~128-byte wire limit; short keys/values.

| Key     | Who        | Meaning                                            |
|---------|------------|----------------------------------------------------|
| `role`  | all bots   | `c` / `s` / `h`                                    |
| `cmdr`  | soldiers   | player# of the commander followed (self if cmdr)   |
| `sqsz`  | commander  | current adaptive squad max size                    |
| `tac`   | commander  | tactic: `sw` (swarm) / `dc` (decoy)                |
| `engph` | commander  | engagement phase: `form` / `rally` / `charge` / `abort` |
| `rdy`   | soldiers   | 0/1 — in position at assigned standoff             |
| `slot`  | soldiers   | assigned standoff index (or self-assigned by order)|

`target` / `mx` / `my` (already broadcast) carry the squad's assigned pill.

---

## 6. The two tactics (grounded in sim mechanics)

Pill fires at the **closest non-allied in-range tank** (`pillbox.c:357-409`);
wall-shield/PPT build already exists in `attack.lua`.

### Swarm
- Commander assigns each member a distinct standoff **angle** around the pill
  (partition `pick_standoff`'s 12 candidates, `attack.lua:766`).
- Members path to "just out of range," set `rdy=1`, hold in a wait substate.
- Commander watches `rdy`; on **quorum** (or timeout — not unanimous, to
  tolerate message loss) broadcasts `engph=charge`.
- All enter range the **same tick** → pill can only target one per reload, so
  it's overwhelmed instead of picking off trickle-in attackers.

### Decoy
- Commander designates **decoy** member(s) — best wall-shield / friendly-pill
  blocker position — to advance **closest-in-range** and pull the pill's
  targeting, soaking fire behind the shield.
- Remaining members attack unmolested from safer standoffs.
- Reuses existing wall-shield build to protect the decoy.

### Tactic selection (commander heuristic)
- Solo / 1 member → normal single-bot attack.
- 2 members → decoy (one soaks, one hits) if shieldable terrain; else swarm.
- ≥3 members, open approach → swarm.
- High-anger / high-HP pill with shield terrain → prefer decoy.
- Commit to the chosen tactic for the engagement (no mid-charge flip).

---

## 7. Harasser behavior (fixed 20%)

- Independent, no commander.
- Mission: infiltrate behind enemy lines — steal/attack **enemy bases**, attack
  from the rear. Bias toward **deep enemy territory** via the influence grid
  (seek enemy-influence regions, target bases far from the front).
- Hit-and-run: avoid defended pills, evade rather than slug it out.
- Largely reuses `capture_base` with a "deep target" preference + evasion bias.

---

## 8. De-confliction & stability

- **Within a squad**: exempt squadmates from `ally_claimed` + ally-avoid **on
  the assigned target only**.
- **Across squads / vs solo bots / vs humans**: existing de-confliction stands
  (squads don't pile on each other's targets). Commanders de-conflict at the
  commander level using the same claim/cost logic on commander broadcasts.
- **Anti-thrash**: role hysteresis, squad-attachment hysteresis, tactic
  commitment — all consistent with the existing goal hysteresis philosophy.

---

## 9. Failure modes to design for

- **Staggered spawn / bootstrapping** — settle window + hysteresis.
- **Message loss** (no ack/retry) — use timeouts + quorum, never unanimity.
- **Commander dies mid-charge** — soldiers fall back / re-attach; promotion.
- **Small teams / few pills** — degrade gracefully to solo behavior.
- **Divergent `ally_state` views** — deterministic rules; commander broadcast is
  source-of-truth for its own squad's orders.
- **128-byte chat limit** — budget keys; heavy payloads via `/info extra`.

---

## 10. Visualization / debugging (BRAIN_DEBUG_MODE)

- Overlay: role tag per bot, line from soldier→commander, squad target, `tac`,
  `engph`, `rdy` flags.
- Surface role/squad in the existing comms/pool panels.
- All viz behind `BRAIN_DEBUG_MODE` / `viz.`/`overlay_` so it strips from `opt/`.

---

## 11. Phasing (build + verify incrementally)

0. **Roles & squads** — allocation (incl. 20% harasser split), adaptive
   attachment, death/promotion, broadcast keys, viz. **No combat change.**
   Verify roles/squads stable and visible in BrainTest.
1. **Squad focus-fire** — exemptions so a squad converges on one pill (no sync).
2. **Swarm sync** — `rdy`/`charge` handshake + standoff partitioning.
3. **Decoy** — designate decoy, wall-shield aggro pull.
4. **Harasser behavior** — deep base steals / rear attacks.
5. **Polish** — cross-squad de-conflict, tactic-selection tuning, edge cases.

Each phase tested in BrainTest before the next (use `-threads N`; large-game UI
is now usable). Scenarios: 4-bot pill take, 8-bot squads+harassers, commander
death, clustered vs spread.

---

## 12. Open questions (to resolve before/within Phase 0)

- `BASELINE_SQUAD_SIZE`, `MAX_HELP_RANGE`, harasser % rounding for tiny teams
  (e.g. 4 bots → 0 harassers? or 1?).
- Adaptive-size signal: exact density metric (ally count in radius vs front
  proximity vs map openness).
- Standoff assignment authority: commander-assigned `slot` vs soldier
  self-assign by deterministic order.
- Quorum threshold + charge timeout values for swarm.
- Whether harasser % should itself flex with game phase (endgame push vs hold).
