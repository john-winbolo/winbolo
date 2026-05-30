# Pill Repositioning — design

Status: **design only — not implemented.** Branch: `bot-improvements-2026-05-30`.

Goal: periodically detect a **poorly-positioned friendly pill** and move it to a
better spot. Scoring is an **influence-panel portfolio model**: classify each
friendly pill by its influence value (the "3"-key overlay) into back /
front / aggressive, hold target proportions, and make pills that are badly
placed (or in an over-subscribed role) the cheap ones to move.

---

## Legacy scoring (current code — for posterity)

The shipping `eval_reposition_pill` (pool 10, `goals.lua`) uses an **ad-hoc
"badness"** metric; AFAIK these conditions came from **aIndy** (the original
68k binary's behaviour) and are kept here as the historical baseline. We are
replacing the scoring but **keeping the reject reasons** (they're good).

**Badness factors** (summed per friendly pill, health>0):
- **Orphaned**: `(dist_to_nearest_friendly_base − PILL_REPOSITION_ORPHAN_DIST(15)) × 5`
  when farther than 15 tiles from the nearest friendly base.
- **Crossfire**: `hostile/neutral pills within PILL_FIRE_RANGE × 30`.
- **Bad terrain**: `+20` on swamp / rubble / crater.

Legacy trigger: act on the highest-badness pill when `badness >
PILL_REPOSITION_THRESHOLD(50)`. Legacy cost: `max(1, travel + 200 − badness)`
(worse position = cheaper to move). **Both the threshold trigger and the
badness factors are being replaced** by the portfolio model below.

**Reject reasons (KEEP — shown in WINNERS, candidate can't win):**
- `no_team_pills` — no friendly pills exist.
- `carrying` — already holding a pill.
- `lgm_busy` — LGM not in tank.
- `inboat`.
- `opening` — opening phase (need pills in place).

---

## New model: influence-panel portfolio

Score each friendly pill on the **influence overlay** (`influence_grid`, the
"3" key; >0 = friendly territory, <0 = enemy territory). Produce a per-pill
**position quality `Q`** (higher = better placed). Reposition cost rises with
`Q`, so good pills are expensive to move and bad ones are cheap:

```
reposition_cost = travel_cost + REPOSITION_BASE_COST + Q
```

A pill in a great spot → high `Q` → high cost → left alone. A poorly-placed or
over-subscribed pill → low `Q` → low cost → becomes the move candidate.

### Classification (by influence at the pill's tile)
- **In-use (pill-take)**: a pill currently **relied on for a pill take** as a
  friendly blocker / decoy cover — whether by a **coordinated squad take** *or*
  by an **individual bot's solo take**. Either source flags it.
  **Excluded from the portfolio counts AND from reposition candidacy** — never
  taken mid-take. (4th category; see Safety below.)
- **Back protector**: influence ≥ `BACK_INFLUENCE_MIN` (clearly friendly).
- **Front line**: `−20 < influence < BACK_INFLUENCE_MIN` (around the front).
- **Aggressive**: influence in **`[−50, −20]`** (inside enemy influence on
  purpose). Deeper than −50 = overextended → penalized / pull-back candidate.

### Target portfolio (share of friendly pills, excluding in-use)
- **35% back protectors** — **guarantee at least 1 back protector** always.
- **45% front line**
- **20% aggressive** (front pushed into enemy influence, −20 to −50)

(So ~65% forward = 45 front + 20 aggressive, 35 holding the back.) Aside from
the ≥1-back guarantee, front/aggressive rounding is loose — "do whatever works"
for the remainder.

### Per-pill quality `Q` terms
- **Back protector — multi-base coverage** (the key "good back spot" signal):
  `+` for each friendly base within the pill's fire range. A protector
  covering **multiple** bases is a strong spot → high `Q` → leave it.
- **Adjacency / double-take penalty** (all categories): `−` when another
  friendly pill is in any of the **8 surrounding tiles** — a clustered pair is
  susceptible to a single enemy double-take, so a tightly-packed pill scores low
  (cheap to move apart).
- **Aggressive depth**: in `[−50, 0)` = doing its job (`+`); deeper than −50 =
  overextended (`−`).
- **On-front bonus**: front-line pills genuinely on/near the front line score
  well; ones drifted into deep-friendly or deep-enemy don't.

### Portfolio balancing (the core rebalancer)
Count current pills per category; compare to the 35/45/20 targets.
- **Penalize every pill in an over-allotted category** — surplus penalty scaled
  by how far over target the category is. This lowers their `Q` → they become
  the cheap-to-move candidates, so the system **sheds from full roles**.
- The deficit role is filled by the **re-drop** (placement biased toward the
  under-target category — see pipeline).
- This subsumes both behaviours the design calls for:
  - *Too many protectors* → protectors over target → protectors get cheaper →
    one moves out (to front/aggressive).
  - *Too few protectors* → front (or aggressive) is over target → those get
    cheaper → one moves back to protect.

### Reposition decision
Among friendly pills (not rejected), pick the **lowest reposition cost**
(= worst `Q`, after the surplus penalty). Move only if it beats a
`REPOSITION_MIN_GAIN` floor so we don't shuffle pills for trivial differences.

---

## Cadence — time-based escalating pressure (instead of edge hysteresis)

Rather than a hard interval or %-based category hysteresis, use a **time ramp**:
the longer it's been since the last reposition, the **more willing** the bot is
to do one. A `reposition_pressure` grows with elapsed time (every ~X minutes it
climbs), and that pressure lowers the effective reposition cost / `MIN_GAIN`
bar. Net effect:
- Just repositioned → pressure low → only a dramatically-better move triggers
  (no thrash / ping-pong).
- Time passes → pressure builds → progressively smaller improvements suffice, so
  the portfolio still gets tidied eventually.
Resets (or drops) when a reposition fires.

## Safety

- Skip pills **under attack / angry** (doing their job).
- Skip **in-use (pill-take)** pills entirely — never pick up a pill a squad is
  using as cover/decoy mid-take (the 4th category; also excluded from counts).
- Account for the **down-time cost** while a pill is carried.
- Reject reasons still gate (no_team_pills / carrying / lgm_busy / inboat /
  opening).

## Reuse the move pipeline
`capture_pill { reposition=true }` → drive over → auto pickup
(`info.carried_pills`) → `place_pill_strategic` re-drops. The re-drop must
**very aggressively** prefer the under-target category (bias
`place_pill_strategic` hard by the category deficits), so a shed pill reliably
fills the empty role instead of landing back in an already-full one.

## Telemetry / viz
- Render each pill's **category + `Q` (or cost)** on the **influence ("3")
  overlay**, plus the current vs target category counts (e.g. `back 4/3
  front 5/4 aggro 1/2`). Behind `BRAIN_DEBUG_MODE` / `viz.` so it strips from
  opt/. This is the panel we tune against before trusting auto-moves.

---

## New constants (proposed)
- `REPOSITION_TARGET_BACK / _FRONT / _AGGRO` = 0.35 / 0.45 / 0.20.
- `BACK_INFLUENCE_MIN`, `FRONT_LO`, `FRONT_HI`, `AGGRO_FLOOR (−50)` — influence
  band cutoffs.
- `REPOSITION_BASE_COST`, `REPOSITION_SURPLUS_PENALTY`, `REPOSITION_ADJACENCY_PENALTY`,
  `REPOSITION_BASE_COVERAGE_BONUS`, `REPOSITION_MIN_GAIN`, `REPOSITION_MIN_INTERVAL`,
  `REPOSITION_PER_PILL_COOLDOWN`.
- Retire `PILL_REPOSITION_THRESHOLD` / `ORPHAN_DIST` once the portfolio model lands.

---

## Resolved (this pass)
- **Aggressive band** = influence `[−50, −20]`; deeper than −50 = overextended.
- **Small counts**: always guarantee **≥1 back protector**; front/aggressive
  remainder is loose.
- **Re-drop bias**: bias `place_pill_strategic` **very aggressively** toward the
  under-target category.
- **Adjacency**: any of the **8 surrounding tiles**.
- **Anti-thrash**: **time-based escalating pressure**, not category-edge / %
  hysteresis.
- **Squad interaction**: add a **4th "in-use (pill-take)" category** — excluded
  from counts and never repositioned mid-take.

## Open questions (still need answers)
- **Influence scale**: confirm the actual range/units of `influence_grid` so
  `BACK_INFLUENCE_MIN` and the front/aggressive cutoffs sit sensibly (cf.
  `STRATEGIC_PLACE_FRONT_PROX_CAP = 80`). The −20 / −50 figures assume a scale
  where ±tens spans friendly↔enemy — verify against real values.
- **Pressure ramp shape**: the `X` minutes and curve for `reposition_pressure`
  (linear? how steep?), and whether it resets fully or partially on a move.
- **In-use flagging mechanism**: a pill is in-use if **either** the squad layer
  **or** an individual bot's solo take is relying on it as blocker/decoy cover.
  Both paths must mark it (e.g. broadcast it in `/info` so any bot evaluating
  reposition sees the reservation; an individual bot also flags its own). Ties
  into `SQUAD_COORDINATION_PLAN.md` / `PILL_TAKE_TACTICS.md`; needs a shared
  reservation flag those takes set.
