# Squad / Strategy brainstorm — decision log (2026-06-01, with John)

Verbatim Q&A record from the clarification pass. The **integrated spec** lives in
`SQUAD_COORDINATION_PLAN.md` (Revisions section) and `PILL_REPOSITION_PLAN.md`
(portfolio + reposition claiming). This file is the raw decision trail.

Status: **design locked — NOT yet implemented.**

---

## R0 — Dynamic commanders

- **Relation to existing deterministic role system:** **Fully replace** it.
  Command is emergent; only harasser stays a fixed slice.
- **Commander trigger:** **Only hard takes.** Hard = target **pill health ≥ 12**.
  Easy takes stay solo (no command, no squad).
- **Harasser selection (since roles are replaced):** **Fixed ~20% stays**
  (deterministic). The other ~80% are soldiers-by-default; commander only while
  leading a hard take.
- **Squad tactics:** **Keep blitz now; add swarm and decoy later** (planned,
  not yet coded).

## R1 — Pill portfolio ratios

- **New target mix:** back 20 / front 45 / aggro 20 / **util 15**.
- **front** redefined: within pill shoot range (~9 tiles, euclidean) of a front
  "3"-overlay tile. **front & aggro never move.** back **rolls forward**.
- **util semantics:** **Enforced 15% target.** Verbatim:
  > "A blocker counts as a util pill. Once the pill take is no longer active, it
  > goes back to front/back/aggressive category as per its location. Tanks can
  > take a 'back' pill in tank to switch it to a utility. All pills in tanks are
  > utility pills."
- **util enforcement mechanism:** **Active pickup** — when under target, task a
  bot to pick up the most expendable (lowest-Q) back pill; carried = util.

## R2 — Circles (front-line regions; name still TBD)

- **Recompute:** **Hybrid** — periodic rebuild from current front "3" tiles +
  match new circles to prior ones by center proximity (so win/loss history
  carries over). Radius ~12. Every front tile is a candidate center; greedy by
  POI coverage, non-overlapping.
- **POI weighting:** **All pills + bases equal**, any owner.

## R3 — Circle win/loss + reinforcement

- **Win/loss method:** **Influence-ratio first**, and specifically a **TREND**.
  Verbatim:
  > "Losing actually needs to compare the ratio of an overlapping past circle at
  > the time it was created as a circle to the ratio of the circle now. So then we
  > can tell the DIFFERENCE of our share of influence at that same spot over time.
  > More influence = we're winning (gaining ground)."
- **Trend baseline:** **Both windows** — short (vs most-recent prior gen) +
  long (vs original/first-seen).
- **Trigger rule:** **Both windows must agree** we're losing before dispatching.
- **Brand-new circle (no prior):** **Instant signals** — ally vs enemy players,
  friendly vs enemy bases/pills — for the first read; start tracking from
  creation.
- **Who reinforces:** **Nearest uncommitted** bot. (Use the existing
  committed/available list in the codebase — `commitment`/`min_commit` goal
  system.) **Harassers: never pulled.**
- **How many:** **Scale to deficit by player count, proportional to the global
  team ratio.** Verbatim:
  > "Just match proportionally. Count our team vs enemies, if there are 8 bots and
  > 4 players, do a 2:1 etc."
  > (also: "often players will put 2 bots for 1 human for a challenge")
- **Assignment / exit:** restricted goal set (attack_tank, kill_lgm, refuel)
  while assigned. Verbatim:
  > "Until it reaches its destination tile in the circle. That destination should
  > be the highest influence (safest). It should time out after 30s (well, in
  > proportion of how long it is expected to take to get there; across a giant
  > map, give it more time)."
- **After arrival:** **Released fully** to normal goal selection.

## R3a — Reposition claiming

- **Add claiming to reposition targets** so multiple bots don't shoot/move the
  same pill (reuse `ally_claimed`). (Original note: "add claiming to
  repositioning pills so multiple don't start shooting it!")

## R4 — Harassers (fixed ~20%) — all behind experiment flags

- Implement behaviors 1, 2, AND 3, **each with an on/off flag** for experiments.
- **(1) Take unprotected bases** — #1 goal. **Unprotected = no covering pills**
  (tank presence ignored; harasser evades tanks). Deep-target bias.
- **(2) Mine back lines — DEFERRED.** The brain has no mine-laying today (only
  mine avoidance in pathing). Ship the flag as a no-op stub; real mine-laying is
  a separate later task.
- **(3) Rear push.** Verbatim:
  > "When there's no unprotected bases to steal, it can use a boat to go to a rear
  > area and then go towards front line, with attack_tank prioritized (regardless
  > of armour at that point, but charged up before going out initially)."
- **Charge-up before a rear mission:** full armour + shells **if we have it**,
  but a **timeout** — if losing and a full tank is unavailable, go with at least
  the minimum.

## R5 — Future / later

- Coordinated mass-scale attacks occasionally — for the fun of human teammates.
- Double pill takes (shoot one of two adjacent enemy pills so it fires the other
  + blockers). **Defer.**

---

## Merge with main / PR #92 ("Prepare bots for beta") — resolution plan

PR #92 (merged to main) renamed **`brains/NewAutopilot/` → `brains/GoalHunter/`**
(`na_`→`gh_`, `NA_`→`GH_`, `BRAIN_NAME`, CMake/docs) and changed content in files
our branch also edits (`goals.lua`, `attack.lua`, `init.lua`, `constants.lua`).

**Merge strategy (decided):**
- Trial-merge on a throwaway branch first to enumerate conflicts
  (`git merge --no-commit --no-ff origin/main`; inspect; `--abort`).
- Real merge with aggressive rename detection so our edits ride onto the renamed
  files: `git merge -X find-renames=40% origin/main`.
- **Do NOT hand-merge `opt/`** — resolve the base Lua files, then regenerate
  `opt/` via `lua_strip` under `GoalHunter/` (halves the conflict surface).
- Move our new untracked docs into `brains/GoalHunter/`.
- Genuine content conflicts expected only in `goals.lua` / `attack.lua` /
  `init.lua` / `constants.lua`.

**Pill-reposition collision — RESOLVED:**
- PR #92 added `C.PILL_REPOSITION_ENABLED`, but it's **vestigial**: an
  unconditional `do return { _reject="disabled" } end` block in
  `eval_reposition_pill` rejects regardless of the flag (real logic below is dead
  code).
- Resolution: **remove the hardcoded `do…return…end` disable block** so the flag
  actually governs; merge our branch's reposition **risk-scoring** additions
  (`PILL_REPOSITION_FEW_PILLS_PENALTY`, `ENEMY_TANK_W`, etc.) into the now-live
  logic.
- **`PILL_REPOSITION_ENABLED` default = `true`** ("our fixes live"): our risk
  penalties + **R3a reposition claiming** are the remedy for the "bots shoot
  their own pills" problem PR #92 disabled it for.

---

## Suggested build order (not started)

1. R3a reposition claiming (independent bug fix).
2. R1 portfolio ratios + util active-pickup.
3. R2 circles (geometry + history matching).
4. R3 win/loss trend + reinforcement (depends on R2).
5. R0 dynamic commanders (largest refactor: squad.lua, blitz, broadcasts).
6. R4 harassers (flags; mining stubbed).
