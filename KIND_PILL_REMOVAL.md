# Removing KIND_PILL from the Brain

## Background

The pathfinder has 4 Dijkstra slates:
- Slate 0: KIND_NORMAL short-range (10-tick restart, 500 nodes/tick hard cap, ~10-tile coverage)
- Slate 1: KIND_NORMAL long-range  (250-tick restart, 560 nodes/tick, full-map coverage in ~2.5 s)
- Slate 2: KIND_PILL  short-range  (10-tick restart, 500 nodes/tick)
- Slate 3: KIND_PILL  long-range   (250-tick restart, 560 nodes/tick)

KIND_PILL is identical to KIND_NORMAL except `danger_scale = 0.1` (DIJKSTRA_PILL_DANGER_SCALE).
The `kind` integer tag in the pathfinder C code has no special tile-cost behavior — it's purely a
lookup matcher. The ONLY difference is the danger_scale baked in at `dijkstra_start` time.

**Why KIND_PILL exists:** When routing to attack a hostile pill, the bot should accept danger from
the target pill rather than routing around it. The 10x danger discount achieves this bluntly.

**Why it's problematic:**
- KIND_PILL and KIND_NORMAL costs are NOT comparable — mixing them across spots in a ranking
  comparison would skew toward whichever kind happened to cover a tile.
- KIND_PILL discounts ALL danger on the path (from every source), not just the target pill's own
  contribution. This is a blunt proxy.
- `eval_capture_pill` uses KIND_PILL even though the target pill is dead (HP=0, no danger from it).
- The self_dr technique (used in ellipse scoring) subtracts just the target pill's per-tile
  contribution via `threat.pill_contrib[pmy*256+pmx]` — more principled but requires knowing
  the path tiles.

**Key math:** tile cost = `terrain_cost + danger * danger_scale * (16 / speed)`
Cannot derive KIND_PILL from KIND_NORMAL by multiplying by 0.1 — that would also scale the
terrain_cost component. They are only equal when terrain_cost = 0 everywhere.

---

## Use sites

### 1. `goals.lua:852` — `eval_attack_pill` / `nearest_where(..., KIND_PILL)`
Path cost for ranking which hostile pill to attack next.

**Replacement:** Post-hoc self_dr correction. Use KIND_NORMAL, then subtract
`threat.pill_contrib[pmy*256+pmx]` per-tile along the approach corridor. A straight-line
approximation of the path is good enough for ranking. This is principled and per-target.

Alternatively: keep KIND_PILL here since all candidates share the same target pill, so relative
ranking is still internally consistent (the blunt discount cancels out in comparisons).

### 2. `goals.lua:2645` — pool-6 ranker `kind = pill_pool and KIND_PILL or KIND_NORMAL`
Same rationale as #1. Same replacement.

### 3. `goals.lua:854` — `dijkstra_shells_at(KIND_PILL, pill.mx, pill.my)`
Shells on arrival estimate for refuel target calculation. Feeds into strategy.lua ->
`state.shell_target` -> refuel decision (shell_target = baseline + mission_shells + combat reserve).

**Replacement:** `dijkstra_shells_at(KIND_NORMAL, ...)`. KIND_NORMAL route is slightly more
conservative but it IS the actual route taken. Marginally underestimates shell consumption —
acceptable since the refuel gate has margin built in.

### 4. `attack.lua:1528` — `dijkstra_shells_at(KIND_PILL, best_mx, best_my)` in `evaluate_tank_standoff`
Same as #3, for tank combat standoff.

**Replacement:** `dijkstra_shells_at(KIND_NORMAL, ...)`.

### 5. `goals.lua:677` — `eval_capture_pill(..., KIND_PILL, CAPTURE_THREAT_WEIGHT)`
Path cost to a dead (HP=0) neutral pill.

**Replacement:** KIND_NORMAL. The pill is dead — no danger from it. The pool-6 block already
has a comment: "the pill is dead, so KIND_PILL would give a misleading low-danger read."
Straightforward bug fix.

### 6. `goals.lua:110` — `cheapest_adjacent_dij(KIND_PILL, gmx, gmy)` in `wsim_evaluate_goal`
Fallback wsim simulation origin (cheapest adjacent tile to the pill). Fires only when no
precomputed standoff spot is available. The wsim path trace itself already uses KIND_NORMAL
(explicit comment at line 100-101: "we want the safest route to evaluate survivability").

**Replacement:** KIND_NORMAL. The tile selection is not danger-sensitive.

### 7. `attack.lua:1522-1526` — `evaluate_tank_standoff` path cost
```lua
cpf.set_config("danger_scale", 0.1)
local path_cost = smart_cost(KIND_PILL, tmx, tmy, best_mx, best_my, ...)
cpf.set_config("danger_scale", 1.0)
```
Tank COMBAT (not pill attack). Wants low danger scale so the bot doesn't refuse to approach
an enemy tank. Uses KIND_PILL slate for the cached lookup + set_config for the A* fallback.

**Replacement:** Trace the KIND_NORMAL path, compute the correction inline:
```lua
local path_cost  = cpf.dijkstra_lookup_by_kind(cpf.KIND_NORMAL, dx, dy, 0)
local path_tiles = cpf.dijkstra_trace_path(cpf.KIND_NORMAL, dx, dy)
if path_tiles then
  for _, t in ipairs(path_tiles) do
    local spd = C.TERRAIN_SPEED[U.ttype(t.mx, t.my)] or 16
    path_cost = path_cost - 0.9 * threat.at(t.mx, t.my) * (16 / spd)
  end
  path_cost = math.max(0, path_cost)
end
```
O(path_length) — faster than A*, exact, no set_config global state mutation, no dedicated slate.
Note: subtracts 90% of ALL danger (all sources), same bluntness as KIND_PILL, but via actual path.

---

## General replacement helper

A shared `corrected_normal_cost(dx, dy, boat, fraction)` helper that:
1. Gets KIND_NORMAL Dijkstra cost (O(1) lookup)
2. Traces the path (O(path_length), C-side)
3. Per tile: subtracts `fraction * threat.at(t) * (16 / speed_at(t))`
4. Returns corrected cost

With `fraction = 0.9` this approximates KIND_PILL for any use site without a dedicated slate.

---

## Suggested implementation order

1. Fix `eval_capture_pill` -> KIND_NORMAL (trivial, dead pill, clear bug)
2. Fix `wsim_evaluate_goal` fallback -> KIND_NORMAL (trivial)
3. Fix `dijkstra_shells_at` calls -> KIND_NORMAL (trivial, both sites)
4. Implement `corrected_normal_cost` helper in attack.lua or a shared util
5. Replace `evaluate_tank_standoff` with corrected helper
6. Replace `eval_attack_pill` / pool-6 with corrected helper
7. Drop KIND_PILL slates (2 and 3) or repurpose as extra KIND_NORMAL slates
   for better multi-slate coverage redundancy
