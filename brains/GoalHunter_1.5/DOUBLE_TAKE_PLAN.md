# Double-take (multi-pill chain) — design

Branch: `double-take-pill-chain`

This is the authoritative spec for the "double-take" feature, consolidated from
the full design discussion. It supersedes any earlier draft of this file.

---

## 1. Concept

A **double-take** (generalize to **N-take**) is ONE attack engagement that
destroys a *chain* of hostile/neutral pillboxes from a **single standoff**, with
no repositioning between kills. 2 pills is by far the common case; the design is
generic over N (including N=1, which is just a normal single-pill take — see §8).

The tank does **not** kill each pill. It kills only ONE pill; the rest are
destroyed by the pillboxes' **own return fire**, which is geometrically arranged
to hit each other instead of the tank.

---

## 2. The chain-reaction mechanism

When angry, pillboxes fire at the tank. Each pill's shot is aimed at the tank but
**blocked by the nearer pill in front of it**, so it strikes *that pill* instead.
Pill shells damage pillboxes, so the pills grind each other down:

```
[tank] — [built/existing wall] — final pill — … — intermediate — … — pill #1
   ▲                                 │                    │            │
   │  tank shoots ONLY pill #1       │ fire hits wall     │ hits next  │ hits next
   └─ kills the backmost pill        ▼ (tank safe)        ▼ (kills it) ▼ (kills it)
```

- **Pill #1 (backmost / farthest from the standoff):** the only pill with nothing
  behind it, so nothing in the cascade shoots it → **the tank kills it.** It is
  the tank's *only* target.
- **Every other pill:** killed by the pill **behind** it (the cascade). The tank
  never aims at them.
- **Final pill (frontmost / nearest the standoff):** also killed by the cascade
  (hit by the pill behind it); its *own* fire is absorbed by the artificial
  blocker (wall/friendly), keeping the tank safe.

Confirmed dynamics (do NOT model timing/HP — treat as static geometry):
- Angry pills fire so fast that the whole down-line cascade is destroyed faster
  than the tank can kill pill #1.
- Pills heat up after only a few hits and cool slowly, so the cascade
  self-sustains. Validity is therefore a **purely geometric** question.

---

## 3. Roles & ordering

Order the candidate pills by distance from the standoff `S`: front (nearest) …
back (farthest).

| Role | Position | Killed by | Needs |
|------|----------|-----------|-------|
| **pill #1** | backmost (farthest) | the **tank** (only target) | tank has a clear outgoing shot to it |
| **intermediate** | middle | the pill behind it | its fire hits a nearer hostile pill |
| **final pill** | frontmost (nearest) | the pill behind it | its fire absorbed by a wall/friendly shield |

"Final" = the frontmost pill whose return fire would reach the tank if not for an
artificial blocker — i.e. the one pill the cascade can't shield because nothing
hostile sits in front of it.

---

## 4. Validity specification

Evaluated **per candidate built-wall combination `C`** (see §5). A standoff `S`
with setup point `U` and combination `C` is **valid** iff all of the following
hold.

### 4a. Cascade (kill) checks

1. **Tank → pill #1 only.** The tank has a clear outgoing shot to the backmost
   pill from `S`, evaluated with `C`'s walls present. No other pill needs to be
   tank-reachable.
2. **Connected fire-chain.** The pills form a connected chain of
   *"pill X's return fire hits a nearer hostile pill Y"* links running from pill
   #1 inward to the final pill. Links **may skip** pills (X's shot may jump over
   a pill to a farther-down nearer pill).
3. **Final pill shielded.** The final (frontmost) pill's return fire to `S` is
   absorbed by **≥2 wall-equivalents** (`C` + pre-existing). This is the
   sustained shield and is the **higher** of the two wall bars.

### 4b. Survivor position-protection

A **survivor** is any pill NOT killed by the cascade (no nearer-pill link strikes
it) and that is not the tank's target pill #1. Survivors stay alive for the whole
take, so they must be unable to knock the tank off its (very precise) position at
**both** the standoff `S` **and** the setup point `U`.

Evaluate each survivor's shot toward each point **with cascade pills treated as
already dead** (they're gone by the time survivors matter, so they can't be
relied on as blockers, and hitting them is harmless). A survivor is harmless at a
point P iff **one** of:

1. **Out of range** of P — it never fires at the tank there; or
2. The **first** thing its shot toward P hits is a **wall-equivalent** (wall or
   friendly pill) — this both shields the tank *and* guarantees it never strikes
   another survivor.

It is **INVALID** (take fails) if a survivor:
- has a clear in-range shot to `S` or `U` (no wall-equivalent in the way — the
  hit knocks the tank off the precise spot), **or**
- the **first** thing its shot hits is **another surviving pillbox** (3rd
  condition): that survivor heats up, breaks through its own block, and seriously
  damages the tank.

So the position block must be a wall-equivalent **ahead of any surviving pill** on
the line — not merely "a wall-equivalent somewhere on the line."

> Note: cascade pills are exempt from the position check at both points (they
> self-absorb and die effectively instantly). Only survivors are checked.

### 4c. Wall-equivalents

One consistent metric everywhere (matches `scan_c`: `WALL_SHOTS=5`,
`PILL_SHOTS=15`):

- **wall = 1**, **friendly pillbox = 3**.
- **Final-pill shield:** ≥ **2** wall-equivalents.
- **Survivor position-block:** ≥ **1** wall-equivalent (a single friendly pill,
  =3, more than satisfies it).

Both bars are met by built (`C`) or pre-existing walls/friendlies.

---

## 5. Per-combination evaluation

Validity is **not** a check of the current world; it is evaluated **per blocker
combination** — the set of walls we would build to shield the final pill (and,
incidentally, to block survivors from `S`/`U`). Whether a standoff works depends
on *which* walls get built, so we iterate the combinations and test the full §4
predicate against each *hypothetical built state* (the combination's wall tiles
treated as present).

Enumerating these combinations is exactly what the existing C shield scorer
(`gh_shield_stamp.c::scan_c`) already does (a subset/cover scorer over the
buildable slots on a return-fire corridor). The new method owns the chain-aware
iteration (see §7) but uses the same cover metric (§4c).

---

## 6. Inputs & outputs

**The caller passes (no pill math, no recomputing standoffs):**

- `S` — standoff point (tile + sub-tile float).
- `U` — setup point (tile + float). **Passed in**, computed by the existing
  standoff/approach code — we do NOT add another "compute the standoff" path.
- `primary_id` — the committed pill.
- **All live pills** — the raw `world.pills` list `{id, mx, my, owner, health}`
  (owner encoded: hostile/neutral vs friendly). NOT pre-filtered — see below.
- **Base tiles** — the (few) base tiles; any-owner bases block shells.

**Why pass *all* pills, not a pre-filtered "in range of S/U" subset:**
- Cheap: pill count is small and bounded; this runs once per `plan_position`
  (cached ~250 ticks).
- Pre-filtering would duplicate the range logic (two code paths) AND is a
  correctness trap — a pill just outside the S/U range envelope can still sit
  *on* a survivor's shot line and be the wall-equivalent that makes the take
  valid; a naive filter would drop it.
- The method only runs the expensive `cpf_simulate_shot` calls for in-range
  **candidates** (which it selects internally); the full list is used only for
  O(1) blocker lookups. So passing all pills does not inflate sim count.

**Output:** the winning blocker combination (walls to build) in the **same shape
`shield.scan` produces today**, so all downstream code (`build_walls`, etc.) is
unchanged, plus chain metadata (kill order, pill #1, the final pill, survivors).

---

## 7. Implementation architecture

**Language: C.** The whole brain is Lua calling C primitives; only the hot
primitives are C. The double-take logic is C, with **no Lua twin** — single code
path. (My earlier Lua prototype `pill_chain.lua` was deleted for this reason.)

**Reuse, don't duplicate, the shot path.** Trajectories come from the
authoritative `cpf_simulate_shot` (bit-exact `SHELL_SPEED` / fixed-point step;
`SHOT_PILL` auto-ranges to `PILLBOX_FIRE_DISTANCE`, so "falls short" is real
physics). Reached from C the same way `gh_attack.c` already calls
`cpf_dijkstra_cost_at` (`lua_getglobal` + `lua_call` through the `lua_State`).
The only genuinely-new code is the chain classification + invariant + cascade.

**New method, with the old one kept as a toggleable fallback (per request):**
- The new method handles N≥1 and becomes the **live** blocker-search path.
- The existing single-take blocker search (the `_shield_scan_pending` →
  `shield.scan(...)` call in `plan_position`) is **repointed** to the new method
  via a flag:
  ```lua
  if C.CHAIN_BLOCKER_SEARCH then          -- default true; chains + singles
    result = chain.search(S, U, primary_id, pills, bases, ...)
  else
    result = shield.scan(...)             -- old scan_c path, untouched
  end
  ```
- **Two toggle flags** (finer-grained safety net): one for chains, one that can
  revert *single-pill* takes to the old `scan_c` while chains stay on the new
  method — so "go back to the old method for single take pills" is one flag flip.
- `scan_c` / `attack_shield.lua`'s scorer are **left fully intact** as the
  fallback. (This pauses the earlier "gut the dead Lua scorer" idea — we keep it
  precisely so the toggle has something to fall back to.)

The new method *supersedes* `scan_c` on the live path but never replaces it in the
tree.

**C world access (from `gh_attack.c`'s established pattern):**
- Terrain (walls) read directly from the raw map byte: `world[my*256+mx] &
  TT_MASK`, walls = `TT_BUILDING`(3) / `TT_HALFBUILD`(9). NOTE: raw map numbering
  ≠ Lua `constants.lua` numbering — use the raw `TT_*` values, not `C.T_*`.
- Pills + bases are marshalled from Lua per call (few, once-per-plan); avoids the
  unknown raw refbase nibble and a persistent C mirror.

---

## 8. N=1 reduction (single-take)

For a single pill, N=1: the same pill is both pill #1 and the final pill, with no
intermediaries and no survivors. The predicate collapses to "tank can shoot it"
+ "its return fire is absorbed by ≥2 wall-equivalents" = **exactly today's
single-pill protected take**. This is how we guarantee no regression: the new
method must produce **identical shield choices to `scan_c` for N=1**, which we
verify. If it diverges, flip the single-take toggle back to `scan_c`.

---

## 9. Cost-estimator integration

Today the ellipse / crossfire cost (`gh_attack.c` `score_e`; goals.lua
`attack_pill_adjustments` crossfire) scores pill clusters as *dangerous* → the
bot avoids exactly the configurations that are exploitable double-takes. The new
method is a **more accurate safety oracle** than the crossfire heuristic: a
thumbs-up means there's a safe standoff + setup point + multi-kill that works.

So a validated chain must **override the crossfire penalty**: zero it and add a
**multi-kill value bonus** → the take becomes *attractive*, not avoided.

**Run the real blockers test in the costing phase — do NOT guess.** Crossfire
lives in goal selection, which runs *before* `plan_position`. Rather than a
cheap heuristic ("does this cluster *look* like a double-take?"), we run the
actual chain/blockers validation (§4–§5) during cost estimation, so a pill is
only discounted when the validator confirms a real safe multi-take exists. More
accurate, worth the extra compute.

**Gated on a high execution (capacity) tier.** The validation is heavier than a
heuristic, so it only runs when the per-tick CPU budget allows — gated on a high
capacity tier, the same tier system that scales other heavy scans (e.g.
`plan_position`'s angle sweep / `pp_spread`). On lower tiers the double-take
costing test is skipped and candidates fall back to the normal crossfire-
penalized cost (the bot simply won't *seek out* double-takes when CPU-
constrained — acceptable degradation, never incorrect).

---

## 10. Current code state (as of this doc)

- `gh_attack.c` has a `discover_chain(...)` Lua entry + helpers (`ch_sim_shot`
  via `cpf_simulate_shot`, `ch_outgoing_reaches`, `ch_analyze_return`,
  classification, the invariant gate). It **compiles & links** into
  `BrainTest.exe`.
- **It is the WRONG shape vs this spec** and must be restructured:
  - It evaluates a single *current* world state; it must iterate **blocker
    combinations** and evaluate the hypothetical built state (§5).
  - It currently requires the tank to reach **every** pill; the spec requires the
    tank to reach **only pill #1** (§4a.1) — the rest die by cascade.
  - It does not yet implement the **survivor position-protection** at `S`/`U`
    (§4b), including the 3rd condition (first-hit-not-a-survivor).
  - It does not yet take `U`, nor produce a `shield.scan`-shaped output.
- Nothing is wired into the live brain yet (`discover_chain` isn't called from
  Lua), so the running brain is unchanged.

---

## 11. Open items / to confirm

- [x] §9 cost sequencing: run the **real** blockers test in the costing phase
      (no heuristic guess), **gated on a high capacity tier**. CONFIRMED.
- [ ] Which capacity tier is the gate (reuse the existing `state._capacity`
      tiering used by `pp_spread`; pick the threshold).
- [ ] Toggle granularity: two flags (chains vs singles) — confirm naming/home
      (likely `constants.lua`).
- [ ] Output shape parity with `shield.scan` for the downstream `build_walls`
      path (enumerate the exact fields to reproduce).
- [ ] N=1 regression check method (how we compare new vs `scan_c` shield choice).

---

## 12. Build / workflow notes

- C: `brains/*/c/*.c` is globbed by `brains/CMakeLists.txt` (`CONFIGURE_DEPENDS`)
  into `bot_brains_static`; registration is next to `naShieldStampRegister(L)` in
  `luabrainshandler.c`. Adding a new `gh_*.c` is auto-picked-up; extending
  `gh_attack.c` needs no reconfigure.
- Build BrainTest: `MSBuild.exe build/BrainTest.vcxproj /p:Configuration=Release
  /p:Platform=x64` (add `/p:DebugInformationFormat=None
  /p:GenerateDebugInformation=false` to skip symbols).
- The post-build asset copy (`data → build/data`, `brains → build/Brains`) is
  currently failing (build-infra, not code) — the exe still links. Copy changed
  Lua/data into `build/Brains` manually when wiring the Lua side.
- C changes need a rebuild; Lua files in `build/Brains/` load fresh per run.
- opt/ regen (per CLAUDE.md) only matters once the Lua call-site changes are made.
