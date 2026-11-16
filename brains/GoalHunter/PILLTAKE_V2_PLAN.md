# Pill take v2 — plan (copy of MULTI_TAKE_SHADOW_PLAN.md, 2026-11-16)

## Decisions (Nov 16)

Andrew's decisions. Where the plan below says something else, these win.

1. **No STEP mode.** PT2_MODE_STEP and everything about stepping out of range
   is dropped. Chains work by BUILDING blockers (HOLD only).
2. **Chain final blocker: one wall-equivalent.** For a chain (N >= 2) the
   final blocker needs only `PT2_CHAIN_BLOCKER_MIN_SHOTS = 5` shots (one wall,
   or one allied pill worth a wall). "Even one wall is fine for a double,
   people do it all the time." "One pill is fine — we have swerve logic."
   No extra depth for the cascade clock (section 2.5 is not used as a bar).
3. **Single pill (N = 1) keeps parity with v1:** the PPT_COVER_TARGET_SHOTS
   (15) bar; wall 5, half wall 5; an allied pill by health as today. v2 at
   N = 1 should pick standoffs and blockers close to v1.
4. **Tiered ranking:** a SAFE longer chain always ranks above a shorter one;
   an allied-pill final blocker ranks above walls; leeway (neighbour points
   also pass) ranks higher inside a tier.
5. **Knobs:** master `PILLTAKE_V2_EVALUATOR`. Sub-knobs act only with the
   master on. constants.lua default = new value; PRESETS.keel and
   PRESETS.mast = old value. (This delivery ships the master default FALSE.)
6. **Side by side first:** v1 drives; v2 runs on the same inputs and logs a
   `PT2_CMP` print2 line. Behaviour stays byte-identical. Knob
   `PT2_SIDE_BY_SIDE_LOG` (debug only). The master-on path is built too.
7. **Visualisation in BrainTest** (not the Log Viewer): layers pt2_light,
   pt2_shadow, pt2_rays, pt2_chain, pt2_candidates, pt2_leeway, pt2_blocker,
   pt2_compare. They draw what the algorithm computes.
8. Other open questions (section 9): the plan's recommendation, unless it
   conflicts with the above. Every new number is marked "proposal, awaiting
   Andrew's value" in constants.lua.
9. Lua first. New C only where engine-exact physics is missing.

## Engine corrections found while building

* **The pill's aim is a float, not one of 256 brads.** `TURNTYPE` is
  `float` (src/bolo/public/platform_types.h:65), and `utilCalcAngle`
  returns the float result of its atan formula with no rounding to an
  integer. So sections 1.2, 1.8, 3.2 and 4 are wrong where they speak of
  "exactly 256 rays per pill" and a per-brad ray cache. Each (pill, point)
  pair has its own ray. v2 walks that ray directly (at most 64 integer
  steps, no cache). The "notches" of 1.8 come from where the float ray
  enters or misses a square, not from brad steps.
* **No new C primitive was needed.** `shadow.lua` ports utilCalcAngle,
  utilCalcDistance, utilCalcDistanceHP and the shell spawn and advance to
  Lua, bit for bit (float rounding emulated). `test_pt2_shadow.lua` checks
  120 vectors made by compiling the engine's own C code: angle, steps and
  shell points are equal.
* A tank 9 squares (2304 wu) from a pill is still inside 112 wu of the last
  shell point (2208 wu). Pills target only within 2048 wu, so it does not
  matter for targeting, but the walk reports it.
* Section 2.5 (cascade clock) and STEP are not built (decision 1 and 2).

## How v2 is built (summary; details in the code)

* `shadow.lua`: `cast(env, pill, tank point)` walks one pill shell and
  returns the tank hit (or "reach"), the first hostile pill met (a chain
  link), the cover of every allied blocker before the tank hit (wall 5,
  half wall 5, boat 1, allied pill 5 * floor(hp / 5), planned blockers), and
  the squares crossed. `tank_shot` walks the tank's own shot at an aim.
* `pilltake_v2.lua`: `evaluate(inp)` is a pure function. Candidates are
  angles x radii around the target; T(P) = live non-allied pills within
  2048 + margin wu of P; every X in T(P) must be covered: out of reach, or
  cover >= its bar. Bar: 15 for the target in a single take, 5 for every
  other pill and for every pill of a chain (decision 2). A pill whose first
  meeting is a hostile pill is a chain link; it still needs its cover bar
  behind the link (the same W on both rays, plan 3.4 c), so that the tank
  is not open when the link pill dies. Blocker search adds planned walls
  (and a dropped carried pill) on failing rays, nearest the pill first, up
  to PT2_BUILD_MAX. The tank needs one clear aim (5 aims, same offsets as
  attack_shield). Rank key: safe, kills, blocker rank (allied pill above
  walls), leeway, score (low is better).
* Output has the shape `shield.scan` returns, plus `chain`, so build_walls,
  charge and engage read it unchanged.

---

Date: 2026-11-16. Repo read: D:/Development/winbolo-combined (branch combined-test).
Brain: brains/GoalHunter. Engine: src/bolo. No code was changed for this document.

This is a design, not code. Every number marked PROPOSAL needs Andrew's yes.

## 0. Andrew's idea in one paragraph

Treat every pillbox as a light source. Walls and other pillboxes in its range
cast shadows. The tank stands inside the shadow of every hostile pill that can
reach it, and still has a clear shot at one pill (A). A's shells, aimed at the
tank, hit the pill in front (B). B's shells hit the next thing on its line. The
last hostile pill's shells hit a blocker we own: an allied pill, an existing
wall, or a wall we build. The pills grind each other down. The tank must stand
in the overlap of all the shadows, or a pill further up the chain hits it.

Additions from Andrew during this work:

* Score every chain length side by side. A short chain from another angle,
  where B and C are out of range, may beat a long chain.
* A safe longer chain always ranks above a shorter chain for the same target.
  Shorter chains win only when the longer ones are unsafe.
* A take that uses our own pill as the final blocker scores higher than one
  that uses a wall.
* Score a standoff higher when its neighbours also work (leeway).

## 1. The model

### 1.1 Units and names

* One map square = 256 world units (wu). A pill's centre is at
  `(mx*256+128, my*256+128)` (pillbox.c:728-734).
* `P` = the candidate tank position, as a world point (tank centre).
* `A` = the pill the tank shoots. `B, C, ...` = hostile pills on the chain.
* `W` = the final blocker: an allied pill, an existing wall, or a wall to build.
* `T(P)` = the set of hostile or neutral pills that can target the tank at P.

### 1.2 A pill's ray toward P

The engine aims a pill at a standing tank with `utilCalcAngle` (pillbox.c:1212,
util.c:244-273). The result is a TURNTYPE, an integer 0..255. So a pill has
exactly 256 possible shell directions, 1.41 degrees apart. At the far end of a
pill's reach (2208 wu) one step is 54 wu sideways. "Precise granular rays" means
exactly these 256 rays per pill. Finer than that has no meaning; coarser misses
the narrow windows the old census found (see 1.8).

The ray for brad `b` from pill centre `c`:

1. The shell starts `shell_start_add * shell_speed = 5 * 32 = 160 wu` from `c`
   along `b` (shells.c:237-238, shells.h:75,81).
2. It moves 32 wu per tick in 24.8 fixed point (shells.c:120-131).
3. It lives `1 + 8 * 8.5 - 5 = 64` ticks (shells.c:132-135, pillbox.h:104).
   Reach from the centre = 160 + 64 * 32 = 2208 wu = 8.63 squares.

The brain already has a bit-exact walker for this (brain_pathfinder.c:4610-4700,
`simulate_shot_walk`), but it returns squares only. The shadow test needs the
per-tick world points. See 3.1 for the new primitive.

### 1.3 What stops a pill's shell, in engine order

Checked each tick at the shell's new point (shells.c:840-1100):

1. A live pill on the shell's square, any owner, armour > 0, not in a tank
   (shells.c:884-885, pillbox.c:914-929). The pill takes 1 damage
   (`pill_shell_damage`, pillbox.h:57) and gets angry (shells.c:901).
   Our own pill is hit the same way: there is no owner test.
2. A tank whose centre is within 112 wu of the shell point (shells.c:953-959,
   tank.c:1585-1596, hit_rules.h:70, tank.h:107). Pill shells test the tank
   where it is now, no rewind (shells.c:916-920).
3. A base: only when `basesCanHit(..., owner)` is true, and a pill's shell
   has owner NEUTRAL, so it returns FALSE (bases.c:1478). A pill's shell never
   stops on a base. Bases cast no shadow.
4. Terrain that `mapIsPassable` rejects (shells.c:1049, bolo_map.c:1003-1057):
   BUILDING, HALFBUILDING, FOREST, BOAT. Water, road, grass, swamp, crater,
   rubble let the shell through.
   * A wall takes 1 hit to become a half wall, then 4 more to become rubble:
     5 shots (shells.c:1083-1098, building.c:97-107, building.h:48).
   * Forest becomes grass on the first hit (shells.c:1087-1093). It stops one
     shell only.
   * A boat becomes river on the first hit.
5. After 64 ticks the shell dies.

Dead pills (armour 0) do not stop shells (pillbox.c:922). Rubble does not.

### 1.4 Shadow, precisely

For pill X at centre `c` and tank point `P`:

* `b = utilCalcAngle(c, P)` (the engine's integer brad).
* Walk the ray for `b`. At each tick point `q_t`:
  * if the square of `q_t` holds a live pill: BLOCKED by that pill at `t`;
  * else if `|q_t - P| <= 112`: TANK HIT at `t`;
  * else if the square is BUILDING, HALFBUILDING or BOAT: BLOCKED by terrain;
  * else if the square is FOREST: continue (see below);
  * after 64 ticks: OUT OF REACH.
* `P` is "in the shadow of X" when BLOCKED comes before TANK HIT, or the ray
  is OUT OF REACH.

Forest is treated as transparent. It stops one shell and is gone, and the
tank's own shells clear it too. So forest neither protects nor blocks. This is
the conservative choice both ways. Knob MT_FOREST_TRANSPARENT (PROPOSAL: true).

Width: the engine tests the tank as a 112 wu circle at every 32 wu step along
the ray. The shadow is a condition on the centre point `P`, and the per-step
test is what the engine does, so the "width" of the tank is already inside the
test. For a ray that passes a blocker square, the blocker is hit when the shell
point enters the square; a tank whose centre is within 112 wu of an earlier
shell point is hit first. In practice: the blocker square must be entered at a
shell point whose distance to `P` is more than 112 wu. Because the tank cannot
overlap a wall square (tank.h:75-90), a tank standing right beside the wall on
the near side is safe; one standing beside the wall at a corner needs the
per-point test, which the walk does.

Who can target the tank: a pill targets the nearest non-allied live tank whose
centre is within `pill_range = 2048 wu` of the pill centre, as a true circle
(pillbox.c:770-807, util.c:295-320, pillbox.h:101). No line of sight test. A
tank hidden in forest beyond 768 wu is skipped unless it just fired
(pillbox.c:787-789, tank.h:244); our tank fires, so this never helps us.

`T(P)` = hostile and neutral pills with `|c - P| <= 2048 + MT_RANGE_MARGIN_WU`.
The margin covers the leeway ring and one knockback (PROPOSAL: 128 wu).

### 1.5 The overlap rule

`P` is a valid multi-take position for target A when:

* OVERLAP: every X in `T(P)` has `P` in its shadow (with the planned walls
  counted as present, and every live pill counted as a blocker).
* OUTGOING: at least one of the tank's 5 aims on A (centre + 4 corners, the
  same AIM_OFFSETS attack_shield.lua uses) is clear from `P`, with the planned
  walls and every live pill (hostile chain pills included) as obstacles. Tank
  shells also stop on hostile bases with armour above 4 (bases.c:1483,
  util.lua:624-637) and on forest for one shot.
* CLOCK: the cascade timing says the tank is never exposed (see 2.5). This
  is where Andrew's idea needs a correction.

This is the "overlap" in Andrew's words: the tank must be in the shadow of A
cast by B, AND in the shadow of B cast by W, AND so on. The chain is just the
graph of who blocks whom. A pill out of range of `P` needs no shadow; it is
simply not a threat from that angle, which is what "a shorter chain from
another angle" means.

### 1.6 Chain and kills

Build the graph: for each X in `T(P)`, `first_block(X, P)` is a pill, a wall
square, a boat, or OUT OF REACH. Chain pills are the hostile pills reachable
from A by following `first_block` links while the link is a hostile pill.

Who gets angry: a pill halves its firing interval on each shell that damages it
(pillbox.c:975-981; rules 100 -> 50 -> 25 -> 12 -> 6, pillbox.h:51-53,65).
The tank angers A. A's shells anger B. B's shells anger C. A pill not on A's
chain and blocked by a wall stays calm (one shell per 100 ticks) and is a
"bystander"; it chips its blocker very slowly and is not a kill.

Kills: A dies to the tank. B dies to A. C dies to B (or to A once B is rubble
and C is on A's ray). Which of these actually happen before the tank must leave
is what the clock decides (2.5).

### 1.7 The tank's own shot

A tank's shell lives `1 + 8 * (sight/2) - 5` ticks; sight tops out at 14
(tank.h:184), so 52 ticks, 160 + 52 * 32 = 1824 wu = 7.13 squares from the tank
centre. The standoff ring is 7.4 squares (constants.lua:646); the pill square is
entered at 6.9, so centre aims reach. Far-corner aims may fall short; the
shield stamp already records `has_pill` per aim (gh_shield_stamp.c:20-24) and
charge_leeway treats "short" as not clear. The multi-take uses only aims that
reach.

### 1.8 What the old census found, and why shadows fix it

The June plan (DOUBLE_TAKE_PLAN.md) swept bearings and called the shell sim per
bearing. Its notes (double_take.lua on improve-bots-june-29, header) report that
for a tight pair the valid bearings are "axis +/-16..42 deg with ~5-deg notches
inside (e.g. axis+30 fails while +29 and +35 work)". Those notches are the brad
quantisation of 1.2: the chosen brad flips as `P` moves, and one brad enters B's
square while its neighbour misses it. Casting the 256 rays once per pill and
reading them back for any `P` finds every notch for the cost of one walk per
brad, and lets neighbours (leeway) be tested for almost nothing.

### 1.9 Blocker depth — the shadow must be as thick as today's shield

A shadow line is not enough. A pill keeps firing into its blocker, and a wall
dies after 5 hits (1.3). So a shadow cell counts only when the blockers on the
ray add up to enough shots. Today's shield scan has that bar already:

* `PPT_COVER_TARGET_SHOTS = 15` (constants.lua:892) and `COVER_TARGET 15.0`
  (gh_shield_stamp.c:125): cover is complete at 15 shots-to-break = three
  walls or one full pill. Beyond it extra blockers add nothing.
* Wall = `WALL_SHOTS 5.0` (gh_shield_stamp.c:119), engine 5 hits (1.3).
* Friendly pill = `5 * floor(health / 5)` shots with `PILL_COVER_BY_HEALTH =
  true` (constants.lua:891, attack_shield.lua:502-535, pill_cover.lua), else
  `PPT_PILL_WALL_EQUIV 3.0` walls (constants.lua:893). Full pill = 15.
* A half wall is counted as a full wall today (attack_shield.lua:358 kind
  "wall_half" joins actual_blockers; gh_shield_stamp.c:975 blocks on
  T_HALFBUILD the same as T_BUILDING). Its real remaining life is 1..4 hits
  (building.c:97-107) and the bot cannot see which. v2 keeps parity (5) so
  that v2 at N=1 chooses as today; a stricter value is open question 14.
* The build phase ends when the slots' cover reaches the bar
  (attack.lua:8427-8440, BUILD_WALLS_COVER_DONE), or earlier at
  `PPT_BLOCKERS_ENOUGH 1` new wall when a blitz supports the take
  (attack.lua:8420-8424).

Note for the brief: the coordinator wrote "today >= 2 wall-equivalents". The
current code's bar is 15 shots = 3 wall-equivalents. v2 uses the same constant.

The v2 rule, per ray from pill X to tank point P:

```
depth(X, P, extra) = sum over squares the ray crosses before the tank-hit
                     point of: wall 5, half wall 5, friendly pill
                     5*floor(health/5), planned wall 5, dropped pill 15,
                     hostile pill on the chain: 0 (it is a link, see below)
in_shadow(X, P)     = depth(X, P) >= PT2_COVER_TARGET_SHOTS
```

`PT2_COVER_TARGET_SHOTS` is not a new number: it reads
`C.PPT_COVER_TARGET_SHOTS` (15). Several blockers in a row on one ray add up,
as the stamp's subset sum does today (gh_shield_stamp.c:616-622). Blockers
behind the tank-hit point do not count; blockers nearer than BLOCKER_MIN_DIST
(1.0 square) to the tank are not counted or built, as today.

Chain links: for a hostile link X -> Y (Y is the next hostile pill on X's ray)
the "depth" is Y's armour. It is not a cover bar but a time: Y stops X's fire
for `armour_Y * interval_X` ticks. The clock (2.5) uses it. The final link
(last hostile pill -> W) is where the cover bar applies, with the clock's
shot count as a second bar in HOLD mode:
`need = max(PT2_COVER_TARGET_SHOTS, clock_shots * PT2_CLOCK_SAFETY)`.

Built-wall search (3.4) adds planned walls on the final ray, nearest the pill
first (so one wall shields more of the leeway ring), until `need` is met or
PT2_BUILD_MAX is reached. If the bar is not met the candidate is "exposed",
never "safe". With `PT2_COVER_TARGET_SHOTS = 15` a walls-only shield builds
out to three walls, as today's BUILD_WALLS_COVER_DONE comment states.

## 2. Engine facts and what they change

Each fact, where it is, and what it does to the design.

### 2.1 Target choice and aim

* Nearest non-allied live tank within 2048 wu, true circle, no line of sight
  (pillbox.c:770-807, util.c:295-320).
  -> Any tank of another team nearer to a chain pill than ours redirects that
  pill's fire and breaks the chain. In a blitz, only the nearest soldier is the
  bait. Rule: multi-take is a solo tactic in v1; abort when another tank gets
  nearer to any chain pill (MT_ALLY_NEARER_ABORT, PROPOSAL: true).
* A standing tank (speed 0) is aimed at with a straight integer brad
  (pillbox.c:1211-1212). A moving tank is led by a solver (pillsTargetTankMove,
  pillbox.c:1213-1250) whose ray is not the standing ray.
  -> The approach into `P` must creep (PPT_CHARGE_MAX_SPEED 4, constants.lua:711)
  and the last part must stay inside the shadow corridor. While moving, the led
  ray may leave the shadow for a moment. The plan treats the standing geometry
  as the only safe state; the approach is scored, not proven.
* First shot: on the tick a pill first sees a target it only sets `justSeen`
  and resets reload (pillbox.c:833-836). So the first shell comes one full
  interval after sighting.
* Range vs reach: targeting is 2048 wu, shells fly 2208 wu (1.2).
  -> A step-back point must be beyond 2048 + margin to stop the pill firing;
  shells already in the air still land.

### 2.2 Anger and cooling

* Interval after k hits: 100, 50, 25, 12, 6 ticks (pillbox.c:975-981).
  Four hits make a pill fully angry. A fifth changes nothing.
* Cooling: +1 interval tick per 32 ticks (pillbox.c:745-752). 6 -> 100 takes
  94 * 32 = 3008 ticks (about 60 s). Each new hit halves it again and restarts
  the 32-tick cooldown.
* The brain's anger proxy: world.lua keeps `pill.anger` in 0..1 from observed
  armour drops (attack.lua:10600-10625 explains it; HEAT_MAX_HITS = 4).

### 2.3 Damage numbers

| What | Value | Where |
|---|---|---|
| Pill armour | 15 | pillbox.h:66 PILLS_MAX_ARMOUR |
| Shell damage to a pill | 1 | pillbox.h:57 |
| Shell damage to a tank | 5 | global.h:264 DAMAGE |
| Tank armour | 40 | gametype.h:41 |
| Tank reload | 13 ticks | tank.h:187 |
| Pill interval, full anger | 6 ticks | pillbox.h:52 |
| Wall shots to rubble | 5 | building.c:97-107, building.h:48 |
| Allied pill as cover | floor(health/5) walls | pill_cover.lua:1-20 |
| Knockback per hit | 32 wu first tick, then x0.75 per tick: about 125 wu total | tank.c:1499-1512, tank.c:65-67, tank.h:61,281 |
| LGM wall cost | 2 trees | lgm.h:63 |
| LGM pill repair | 1 tree per 4 armour, 4 trees per trip | lgm.h:67,74, pillbox.h:112 |

Derived: 8 pill shells kill a full tank. A full-anger pill fires 8 shells in 48
ticks. A full pill needs 15 tank shells = 14 * 13 = 182 ticks after the first
hit. One stray pill hit moves the tank half a square.

### 2.4 Shells and pills

* A pill's shell damages and angers ANY pill it lands on, our own included
  (shells.c:884-901, pillbox.c:943-986; no owner test anywhere).
  -> An allied pill used as W loses 1 armour per shell it stops.
* A pill keeps firing at a target it cannot hit. There is no "can I hit it"
  test in pillsUpdate. So A fires into B for as long as the tank is its nearest
  target in range. The cascade does not stall. Confirmed.
* The brain's tank-aware shot walker (`simulate_shot_walk_tanks`,
  brain_pathfinder.c:4743-4747 and 4780-4785) uses an INT pill range (8, not
  8.5) and a 128 wu square hit box, not the 112 wu circle. It is not
  engine-exact. The shadow code must not use it. (Separate small bug report.)

### 2.5 The cascade clock — the part of the idea the engine contradicts

Andrew's view: "angry pills fire so fast the cascade finishes". The cascade
does finish fast. That is the problem. Rates:

* Tank -> A: 1 damage per 13 ticks.
* Angry A -> B: 1 damage per 6 ticks (more than twice as fast).

Two full pills, A -> B -> W, tank at P. t0 = first tank shell lands on A.

| Event | Tick (approx.) |
|---|---|
| A fully angry (4 tank hits) | t0 + 39 |
| B fully angry (4 of A's hits) | t0 + 60 + flight A->B |
| B dead (15 of A's shells) | t0 + 100..125 |
| A dead (15 tank shells) | t0 + 182 |
| Gap: B rubble, A alive and angry | 60..80 ticks |

In the gap A's ray reaches whatever is behind B. If that is the tank, A fires
10..13 shells = 50..65 damage. The tank dies. So the far pill (A, the tank's
target) dies LAST, and the near pills die first. The order Andrew wants (A, B
grind each other down) happens, but the tank is exposed at the end unless W
also sits on A's ray behind B, OR the tank leaves before B dies.

Shells into W, same case: B's angry fire until B dies (about 11) plus A's fire
after B dies until A dies (about 10..13) = 21..24 shots. That is 5 walls, or
one full allied pill plus 2 walls. A full allied pill alone (15) does not hold.

Three-chain A -> B -> C -> W: C starts taking B's fire about 25 ticks after B
starts taking A's. B dies first, then A's ray reaches C and C dies a little
later, then A's ray reaches W. W takes C's angry fire, then A's. Same shape,
more shots.

So the design has two modes, both knobs:

* HOLD (Andrew's full idea). W is deep enough for the whole clock. The tank
  shoots A to the end. Kills = whole chain. Needs W on the LAST hostile pill's
  ray AND on A's ray (they are nearly the same line, see 3.4), with shot budget
  `>= clock_shots * MT_CLOCK_SAFETY` (PROPOSAL: 1.25).
* STEP. The tank fires only the 4 shells that anger A (reuse heat_pill,
  attack.lua:10634-11100), then holds fire in the shadow. A grinds B down. When
  B's visible health drops to MT_STEP_HP (PROPOSAL: 5), the tank backs out of
  A's range along the shadow corridor to a precomputed point beyond
  MT_STEP_BACK_TILES (PROPOSAL: 8.7 squares, 2227 wu) from A. W takes only B's
  fire (about 11 shots: 2-3 walls, or an allied pill that ends near 4 armour).
  Kills = B (and C...), not A. A stays angry for about 60 s; the existing
  ANGER_WAIT costing (goals.lua:1690-1699) handles when to come back for A as a
  normal take. Cost: 4 shells, no LGM for B. This is the cheap, common case.

The clock is a closed-form per link, not a tick simulation: shells into the
next object = (ticks the shooter is alive and angry) / interval, with the four
halving steps counted. It costs a few dozen operations per candidate.

Time to step back: B at MT_STEP_HP = 5 under 1/6 fire has about 30 ticks plus
flight. From 7.4 to 8.7 squares is 333 wu; on grass at full speed that is about
25 ticks. Tight. Alternative exit: sideways into another shadow (a wall) at the
same radius, which the planner can test with the same cache. The planner must
reject a STEP plan whose exit takes longer than the warning (knob
MT_STEP_WARN_TICKS, PROPOSAL: 30).

### 2.6 Sight rule (no cheating)

The brain's map is the bot's own 29x29 window plus a 15x15 window per deployed
team pill (brain_data.c:133-137, 153-158). `U.ttype` reads that remembered map
(util.lua:43-51). `world.pills` carries the positions the bot was told or saw,
with `pos_unknown` when a pill moved out of sight (world.lua:124-148). Pill
health comes from the status bar, as a human sees it.

Shadows are cast from `world.pills` entries with a known square and
`health > 0`, over the remembered terrain. A wall the bot has not seen since it
was shot away still casts a shadow in the plan; the live re-check (4.4) catches
the change when the bot sees it. No engine-side pill or map data is read.

## 3. Algorithm

### 3.1 New C primitives (small, in braincore.c / brain_pathfinder.c)

* `cpf_pill_aim_brad(px, py, tx, ty) -> int` : calls `utilCalcAngle`. Gives the
  exact brad the engine picks for a standing tank. Porting the formula to Lua
  is possible but the truncation and quadrant rules (util.c:262-271) are easy
  to get one brad wrong.
* `cpf_shell_points(ox, oy, brad, shooter) -> {x1,y1, x2,y2, ...}` : the
  per-tick world points of the shell, pill reach 8.5 as float, built on
  `shellSpawnPos` / `shellLifeTicks` / `utilCalcDistanceHP` like
  `simulate_shot_walk`. 64 points for a pill. Returned as a flat array.

Everything else is Lua first (Andrew: "re-code in C eventually").

### 3.2 Per-pill ray cache

`shadow.lua`, new module. For each pill X (hostile, neutral, or allied —
allied pills matter as blockers and as things that shoot enemies, but only
hostile and neutral pills threaten us):

```
rays[X] = {
  version = <world change counter for X's 19x19 window>,
  brad[b] = { pts = <64 world points>, sq = <square index per point>,
              block_t = <first tick index BLOCKED, or nil>,
              block_kind = "pill"|"wall"|"boat", block_id = <pill id or square> }
  sq_to_brads = { [square] = {b, ...} }   -- which rays cross a square
}
```

Building one pill: 256 brads x 64 points. In Lua about 0.5-1 ms per pill;
stored until a square in the window changes. `U.ttype` already records terrain
changes (`changes.terrain`, util.lua:48-51) and world.lua reports pill
changes; a per-window counter is a few lines in each.

`in_shadow(X, P, extra_walls)`: `b = cpf_pill_aim_brad`; scan `brad[b].pts`
from the start: a pill square or extra wall square or `block_t` -> BLOCKED;
`dist2(pt, P) <= 112^2` -> HIT. At most 64 distance tests, usually fewer than
40 because the block comes first. With `extra_walls` (a candidate build set)
the scan checks each point's square against the set; `sq_to_brads` tells which
cached brads a hypothetical wall changes, so most lookups are untouched.

### 3.3 Candidate standoffs

For target A: angles 0..360 in MT_ANGLE_STEP_DEG steps (PROPOSAL: 2; today's
plan uses 12 directions, the shield scan 0.5 degree nudges over +/-7 degrees),
radii MT_RADII (PROPOSAL: 7.4, 7.0, 6.6 squares). Each gives a float `P`.
Sub-square positions are kept as floats (the brain already keeps
`standoff_fx/fy`).

Filter cheaply first: standable land (same tests as pick_standoff), STAY_AREA,
blitz rejects, banned angles.

For each `P`:

1. `T(P)` from `world.pills` by distance.
2. OVERLAP: `in_shadow` for every X in `T(P)` with no extra walls. Record each
   failing X with its ray's first point that hits the tank and the squares the
   ray crosses before it (the blocker candidates).
3. OUTGOING: 5 aims from `P` to A using the existing shield stamp paths when
   `P` is on the stamp ring, else `cpf_simulate_shot_angle`. Obstacles: walls,
   half walls, live pills, hostile bases, boats.
4. If OVERLAP fails only for pills whose rays cross buildable squares, go to
   3.4 (blocker search). Otherwise `P` is a HOLD/STEP candidate as-is or
   rejected.

### 3.4 Final blocker search

Andrew's rule: the last hostile pill in the chain must end on an allied pill
or a wall. If none exists, find a square to build on so that (a) the last
pill's ray is blocked, (b) the tank's shot to A is still clear, and (c) for
HOLD mode, the same square (or a square behind it) also blocks A's ray once
the intermediate pills are rubble.

Geometry note: B sits on A's ray to P, so A, B and P are nearly on one line.
B's own ray to P is that same line bent by the small angle A-B-P. A square on
the segment B..P therefore usually lies on both rays. The two brads differ, so
both must be tested; the cache makes that two scans.

Candidate squares: the squares each failing ray crosses after its last blocker
and before the tank-hit point, at least MT_BLOCKER_MIN_DIST from the tank
square (reuse attack_shield's BLOCKER_MIN_DIST 1.0 rule), buildable (grass,
road, rubble, swamp, crater; not water; not a base; not under a pill), and
reachable for the LGM (reuse the LGM trip cost the shield scan already
computes). Combinations: up to MT_BUILD_MAX walls (PROPOSAL: 3, same as the
PPT's 3-blocker tier). For each combination, re-run OVERLAP and OUTGOING with
`extra_walls`, then the clock.

Carried pills: today's PPT can drop a carried pill into a shield slot
(`num_pill_blockers`, attack.lua:6625-6630). The same option applies here: a
dropped pill is worth 3 walls of cover and takes no build time per wall.

Order of preference inside a tier (Andrew's rule 2): an existing allied pill
(no build, no LGM exposure, 15 shots of cover at full health, pillbox.h:66),
then existing walls (no build), then a dropped carried pill, then built walls
(2 trees and one LGM trip each, lgm.h:63). The allied-pill case has a cost:
it loses one armour per shell it stops. The plan records the expected loss and
queues a repair_pill for it when the take ends (MT_REPAIR_AFTER, PROPOSAL:
true; the repair goal and standoff already exist, repair_standoff.lua).
Because a full allied pill alone cannot hold a full 2-chain HOLD (2.5), the
bonus never overrides the clock: an allied pill that would die is not "safe".

### 3.5 Safety, precisely (the top tier)

A candidate (P, chain, W, mode) is SAFE when all of these hold:

* S1 OVERLAP at P with W present, each ray at depth >= PT2_COVER_TARGET_SHOTS (1.9).
* S2 OUTGOING: one reaching aim on A clear.
* S3 CLOCK: HOLD: W's shot budget >= clock shots x MT_CLOCK_SAFETY.
  STEP: W's budget >= B-side shots x MT_CLOCK_SAFETY, AND the exit point is
  outside every chain pill's range + margin, AND the exit is reachable inside
  MT_STEP_WARN_TICKS.
* S4 BYSTANDERS: every pill in T(P) that is not on the chain is blocked by a
  wall or allied pill (this is S1 again; listed so the term is clear).
  Bystanders in range of the setup point U are a cost, not a failure, as
  today (score_standoff's approach_exposure).
* S5 NEAREST: no other non-allied-to-the-pill tank is nearer than ours to any
  chain pill, at plan time; re-checked live.
* S6 LEEWAY: at least MT_LEEWAY_MIN of the 32 wu ring neighbours also pass
  S1 and S2 with the same W (PROPOSAL: 6 of 8). Below that the point is not
  safe. Reason: PPT landing precision is about 32 wu (PPT_CHARGE_BRAKE_DIST,
  constants.lua:724), and one stray hit moves the tank 125 wu.

Everything not SAFE but passing S1 and S2 is "exposed"; it keeps a score
(for the viewer and for the rare case nothing is safe) but ranks below every
safe candidate.

### 3.6 Ranking — tiered, as Andrew asked

Compare candidates for the same target A with a key, highest first:

1. `safe` (1 or 0).
2. `kills` = pills dead at the end of the plan: HOLD = chain length; STEP =
   chain length minus A.
3. `blocker_rank`: allied pill 3, existing wall 2, dropped carried pill 1,
   built wall 0, plus MT_ALLY_PILL_BLOCKER_BONUS in the score below.
4. `leeway` = passing neighbours out of 16 (8 at 32 wu, 8 at 96 wu).
5. `score` = today's score_standoff (attack.lua:1376-1640: approach, water,
   pushback, escape, trees, orbit, threat, influence, friendly pill terms)
   with these changes: the crossfire term skips chain pills and blocked
   bystanders; add build cost (walls x LGM trip, from the shield scan's
   lgm_trip); add MT_ALLY_PILL_BLOCKER_BONUS (PROPOSAL: 150, compare
   FPILL_BARRIER_BONUS 80, constants.lua:3482) when W is an allied pill; add
   the expected armour loss of that pill x MT_ALLY_PILL_LOSS_COST (PROPOSAL:
   5 per armour point); STEP adds the deferred-A cost (ANGER_WAIT terms).

Items 1 and 2 are strict. A safe 2-kill from a poor angle beats a safe 1-kill
from a perfect angle. Item 3 is strict only inside the same (safe, kills) pair,
so Andrew's rule 2 cannot lift an allied-pill 1-kill above a wall 2-kill. If
Andrew wants the allied pill to count more than that, item 3 can move above
item 2 by a knob (MT_ALLY_PILL_ABOVE_KILLS, PROPOSAL: false). Open question 3.

How chain length and angle interact: every angle around A yields its own
`T(P)` and so its own chain. Angles where B and C are out of range give a
1-chain; angles on the A-B line give a 2-chain; the tiered key sorts them.
There is no separate "prefix" search: the shorter chains are simply the other
angles, scored in the same list.

### 3.7 N=1 reduction

With `T(P) = {A}`, the chain is A alone, HOLD mode, W = walls or an allied
pill on A's ray. That is today's protected take. The shadow test replaces the
stamp's return-fire path test; the outgoing test is the same. Regression:
for N=1 the new planner must choose the same W as `scan_c` on the recorded
takes. Until that parity test passes, N=1 stays on `scan_c`
(master off), as the June plan also proposed. With the master on, N=1 goes
through v2 too; that is why parity (phase 5) comes before the master.

### 3.8 Failure modes and the answer to each

| Failure | Answer |
|---|---|
| A pill outside the chain shoots the tank | S1 covers all of T(P); the live re-check (4.4) re-runs in_shadow for every pill in range each tick (cheap) and triggers curve_away/retreat on the first failure. |
| A chain pill dies early (someone else shot it) | The clock's assumption breaks. Live re-check: when B's health reaches 0 and the mode is HOLD, re-run OVERLAP; if A's ray now hits the tank, exit as in STEP. |
| Tank knocked off its spot | Should not happen in a safe plan (zero hits). One hit = 125 wu; leeway at 96 wu says whether the new spot still works; else curve_away. |
| Engine leads a moving tank | Creep in at PPT speed; the final 256 wu of the approach must be inside the standing shadow of every pill in range (corridor test with the cache, 8 points). |
| Another tank nearer to a chain pill | S5; abort on change (MT_ALLY_NEARER_ABORT). |
| A wall on a ray gets shot out by a bystander | Bystanders are calm (1 shot / 100 ticks); a 5-shot wall lasts 500 ticks. Counted in the wall budget as +1 shot per bystander per 100 ticks of plan length. |
| Builder exposure while building W | Reuse the PPT's build_walls flow and its builder safety (LGM trip costing, blocker_min_lane). |
| Decoy that looks like a chain but is not (B just off the ray) | The brad-exact ray says B's square is not entered; no link; no chain. That is the decoy ROOST test. |
| Stamp says a corner aim is short | Only `has_pill` aims count (1.7). |

## 4. Cost and tiers

Per-pill ray cache build: 256 x 64 = 16k steps, about 0.5-1 ms in LuaJIT
(interpreted path; the JIT does better), done once per pill per change.
Memory: 256 x 64 x 2 numbers per pill; about 260 KB per pill as Lua numbers;
pack as flat arrays, or keep only `sq` and `block_t` per brad and re-walk
points for the few brads a test needs. PROPOSAL: keep squares and block data
per brad (small), compute points on demand via `cpf_shell_points` with a
64-entry LRU per pill.

Per candidate P: |T(P)| in_shadow scans (each <= 64 distance tests) + 5 aims
(stamp lookup) + clock. About 20-40 us in Lua. Leeway adds 16 x |T(P)| scans.
180 angles x 3 radii = 540 candidates x 17 points x 3 pills x 40 steps = about
1.1 M steps, around 60-150 ms in Lua. Too much for one tick. So:

* Run inside the existing chunked plan (advance_pill_eval_chunk,
  attack.lua:2441, with pp_spread from the tier table, constants.lua:4043-4048)
  so the work spreads over ticks; the brain already defers the shield scan one
  tick for the same reason (attack.lua:6514-6538).
* Prune: compute OVERLAP for the coarse 12 directions first; only angles whose
  neighbours differ in chain structure get the 2-degree fill. Leeway only for
  the top MT_LEEWAY_TOP candidates (PROPOSAL: 8).
* Goal costing (5.2) uses a cached result from the last plan or a coarse pass
  (12 angles, no leeway, no build search): about 2-4 ms once per target per
  MT_COST_CACHE_TICKS (PROPOSAL: 500), only at tier >= MT_COST_TIER
  (PROPOSAL: 8).

Tier column (PROPOSAL, added to the capacity table at constants.lua:4043):

| tier | mt_step_deg | mt_leeway | mt_max_chain | mt_cost_pass |
|---|---|---|---|---|
| 10-9 | 2 | 16 | 3 | yes |
| 8-7 | 4 | 8 | 3 | yes |
| 6-5 | 6 | 4 | 2 | no |
| <=4 | off (today's path) | - | - | no |

C port later: the same loops in C run about 50x faster; the whole 540-candidate
search becomes 2-3 ms, which fits a single tick on this PC's 3 ms target only
when chunked, so the chunking stays.

## 5. Integration

### 5.1 Where it goes

* New `brains/GoalHunter/shadow.lua` (ray cache, in_shadow, corridor test).
* New `brains/GoalHunter/multi_take.lua` (candidates, blocker search, clock,
  tiers, plan output). Output has the same shape `shield.scan` returns
  (`best`, blocker squares actual/potential, standoff cx/cy/deg) plus
  `chain = { mode, pills = {...}, final = W, kills, exit = {fx,fy}, clock }`.
  So build_walls, charge and engage read it unchanged.
* `attack.lua` plan_position: after the spots pass and before the deferred
  shield scan (attack.lua:6538), when `C.PILLTAKE_V2_EVALUATOR` is on and tier allows, run
  `multi_take.search(A)`. If it returns a safe candidate with kills >= 1
  beyond A, or N=1 (the master covers N=1), use it in place of the shield
  scan; else fall through to today's `shield.scan`.
* `attack.lua` engage: when `goal.chain` exists: HOLD = normal engage; STEP =
  heat_pill fire control (stop after 4 landed hits), watch B's health, exit to
  `goal.chain.exit`, then hand A back to goal selection with its anger.
* `goals.lua` attack_pill_adjustments (goals.lua:1682-1726): when a cached
  multi-take result for this pill is safe, set the crossfire penalty for chain
  pills to 0 and subtract MT_KILL_VALUE per extra kill (PROPOSAL: 100, equal to
  GOAL_CROSSFIRE_PENALTY, constants.lua:1514). Also skip the anger term when
  the plan is a STEP on a different A? No: the anger term stays, it is real.

### 5.2 Knobs — one master, sub-knobs under it

Andrew: "all those knobs will have to be conditional on 'pilltake v2
evaluator' knob being on". So:

* `PILLTAKE_V2_EVALUATOR` (master). ON: the shadow evaluator drives the
  whole pill-take evaluation, N=1 included (plan_position standoff choice,
  blocker choice, build list, and the goal-cost hook). OFF: today's
  evaluator (pick_standoff / evaluate_pill_difficulty / shield.scan scan_c /
  build_walls / attack_pill_adjustments) runs untouched and every PT2_* knob
  below is ignored. The code reads the sub-knobs only inside
  `if C.PILLTAKE_V2_EVALUATOR then`.
* Sub-knobs, prefix `PT2_`, each a behaviour of v2 that can be benched alone.

All in constants.lua. Old value (no v2) in PRESETS.keel and PRESETS.mast.
Every number is a PROPOSAL.

| Knob | Default | keel / mast | Meaning |
|---|---|---|---|
| PILLTAKE_V2_EVALUATOR | true (after phase 6; false until then) | false | master |
| PT2_CHAINS | true | - | chains with N >= 2; false = v2 at N=1 only |
| PT2_LEEWAY | true | - | leeway term and S6 |
| PT2_OWN_PILL_BONUS | true | - | allied-pill blocker rank + bonus (3.6 item 3) |
| PT2_MODE_HOLD | true | - | allow HOLD plans |
| PT2_MODE_STEP | true | - | allow STEP plans |
| PT2_MAX_CHAIN | 3 | - | longest chain |
| PT2_ANGLE_STEP_DEG | 2 | - | candidate angle step |
| PT2_RADII | {7.4, 7.0, 6.6} | - | candidate radii (squares) |
| PT2_RANGE_MARGIN_WU | 128 | - | added to 2048 when building T(P) |
| PT2_FOREST_TRANSPARENT | true | - | forest neither blocks nor protects |
| PT2_COVER_TARGET_SHOTS | = C.PPT_COVER_TARGET_SHOTS (15) | - | depth bar (1.9); not a new number |
| PT2_LEEWAY_RING_WU | {32, 96} | - | neighbour rings, 8 points each |
| PT2_LEEWAY_MIN | 6 | - | passing 32 wu neighbours needed for "safe" |
| PT2_LEEWAY_TOP | 8 | - | candidates that get the leeway test |
| PT2_CLOCK_SAFETY | 1.25 | - | shot budget factor |
| PT2_STEP_HP | 5 | - | B health that starts the step-back |
| PT2_STEP_BACK_TILES | 8.7 | - | exit distance from A |
| PT2_STEP_WARN_TICKS | 30 | - | max ticks the exit may take |
| PT2_BUILD_MAX | 3 | - | walls per plan |
| PT2_OWN_PILL_BLOCKER_BONUS | 150 | - | score bonus, allied pill as W |
| PT2_OWN_PILL_LOSS_COST | 5 | - | score per expected armour lost |
| PT2_OWN_PILL_ABOVE_KILLS | false | - | rank 3 above rank 2 |
| PT2_REPAIR_AFTER | true | - | queue repair of W |
| PT2_ALLY_NEARER_ABORT | true | - | S5 live abort |
| PT2_KILL_VALUE | 100 | - | goal cost cut per extra kill |
| PT2_COST_TIER | 8 | - | min tier for the costing pass |
| PT2_COST_CACHE_TICKS | 500 | - | costing pass cache |
| PT2_TIER_MIN | 5 | - | below this tier v2 falls back to today's path |
| PT2_SIDE_BY_SIDE_LOG | false | false | debug only: run v2 beside today's evaluator and log both (5.4); no behaviour change |

Earlier sections use the names MT_*; read them as PT2_*.

Preset note: keel = behaviour of the release before MAST; mast = tag 2.2.1.
Neither had v2, so `PILLTAKE_V2_EVALUATOR = false` in both, and the PT2_*
rows need no preset entry (ignored when the master is off). They still get
one, set to the default, so a later preset diff shows them.

Bench matrix (each row one bench against the MAST baseline bot, same maps and
seeds, winbolo-mastbench recipe):

| Bench | Side A | Side B | Isolates |
|---|---|---|---|
| master | today's evaluator (master off) | v2, sub-knobs at defaults | the whole v2 |
| chains | v2, PT2_CHAINS false | v2, PT2_CHAINS true | multi-take vs v2 single |
| leeway | v2, PT2_LEEWAY false | v2, PT2_LEEWAY true | leeway term |
| own pill | v2, PT2_OWN_PILL_BONUS false | v2, PT2_OWN_PILL_BONUS true | allied-pill rank |
| step | v2, PT2_MODE_STEP false | v2, PT2_MODE_STEP true | STEP plans |

### 5.3 Can v2 replace today's single-take evaluator? Part by part

Yes for the parts that are geometry, with the depth bar of 1.9. Each part of
today's path, its v2 equivalent, and what v2 would lose if not carried over.

Today's pipeline: get_standoff (attack.lua:1971) -> pick_standoff (:1645) ->
score_standoff (:1376) with evaluate_pill_difficulty stamps (:3712) ->
plan_position (:6400-6740) -> shield.scan / scan_c (attack_shield.lua:636,
gh_shield_stamp.c) -> build_walls (attack.lua:8300-8500) -> charge / engage;
goal cost in attack_pill_adjustments (goals.lua:1682).

| Today's criterion | Where | v2 equivalent | Same / better / lost |
|---|---|---|---|
| Standoff ring R = 7.4, N = 12 directions | pick_standoff, ATTACK_PILL_STANDOFF, ATTACK_PLAN_DIRS | PT2_RADII x PT2_ANGLE_STEP_DEG | better: 180 x 3 instead of 12 x 1, and the shield's 28-point nudge becomes part of the same grid |
| Range penalty 20 / square past reach | pick_standoff range_pen | same term, kept | same |
| STAY_AREA, banned angles, blitz rejects | pick_standoff | same filter, kept | same |
| Clear-shot fallback | pick_standoff | OUTGOING is a hard rule; fallback kept for the "nothing safe" case | same |
| Clear shot to A (walls, half walls, live pills, hostile bases, forest) | standoff_clear_shot :1160 | OUTGOING, 5 aims that reach (1.5, 1.7) | same tests; better: 5 aims instead of 1 |
| Approach / setup point exposure (standoff + 2.25 radial) | score_standoff, ATTACK_APPROACH_OFFSET | same term kept, computed with in_shadow instead of a range circle | better: a setup point in a wall's shadow is no longer penalised |
| Drive-in blocked by wall or pill | score_standoff | same, kept | same |
| Water, pushback, escape, trees, orbit terms | score_standoff | kept as is (they are about the tank's ground, not the pill) | same |
| Threat and influence terms | score_standoff | kept; crossfire part replaced (next row) | same |
| Crossfire: hostile/neutral pills within PILL_RANGE_MAP 9 | score_standoff :~1560, PILL_DANGER_*; evaluate_pill_difficulty :3920-3975 | T(P) at 2048 wu + margin, each pill either in shadow (0), on the chain (0, it is a kill), or exposed (today's formula) | better: a pill blocked by a wall costs nothing; today it costs full danger. Lost: nothing, exposed pills keep today's number |
| Friendly pill terms (pill in use, adjacency) | score_standoff; adj_friendly_penalty in scan | kept | same |
| Ellipse stamps (evaluate_pill_difficulty) | attack.lua:3712, gh_attack.c | not needed: the ellipse approximates "which pills see this side"; in_shadow answers it exactly | better; the ellipse code stays for the master-off path |
| Shield candidates: 28 x 0.5 degree, +/-7 degrees | attack_shield.lua:1-70, sb_positions/sb_step | the 2-degree grid plus the 32/96 wu leeway rings around the best | same cover of the ring; finer near the winner via leeway |
| 5 aims, centre + 4 corners, has_pill per aim | stamp | same AIM_OFFSETS, same reach rule | same |
| Return-fire path: pill -> tank, blockers on it | stamp return path (1 per candidate) | in_shadow on A's exact brad, plus every other pill in T(P) | better: today tests only A's return fire; v2 tests every pill in range |
| Blocker depth: COVER_TARGET 15, wall 5, pill by health | gh_shield_stamp.c:119-125, PILL_COVER_BY_HEALTH | 1.9, same constant | same |
| BLOCKER_MIN_DIST 1.0 from tank | attack_shield.lua | same | same |
| NEIGHBOR_BONUS 4 (slot next to an existing blocker) | attack_shield.lua | kept in the build-slot order | same |
| Wounded tiers (low-health pill: fewer walls) | attack_shield.lua, PPT wounded tiers | kept: `need` drops with A's health as today | same |
| LGM reach / trip cost per slot, no_builder | shield.scan lgm_trip | same function reused | same |
| Carried pill as a dropped blocker (num_pill_blockers) | attack.lua:6625 | same | same |
| Lane test (SHIELD_LANE, builder path between walls) | attack.lua:6848 | kept | same |
| Shield ban buckets, replan every 150 ticks | attack.lua:6793, PILL_ATTACK_REPLAN_TICKS | kept; ray cache makes replans cheaper | same |
| Build order, early-done rules | build_walls :8420-8440 | unchanged, reads the same list shape | same |
| charge_stop_point / charge_leeway | attack.lua:2903, 2948 | unchanged; v2 leeway ring is a plan-time version of charge_leeway | same |
| Goal cost: health, anger, intercept, crossfire penalty, eLGM | goals.lua:1682-1760 | same terms; crossfire uses the cached v2 verdict (5.1) | better for chains, same otherwise |
| hd_no_blockers / suicider / human-near skip of the scan | attack.lua:1337, 6538 | same skips, applied before v2 | same |

What v2 at N=1 is: A alone in T(P), chain = {A}, HOLD, W = blockers on A's
exact return brad with depth >= 15 shots, tank in that shadow with a reaching
aim clear. That is today's protected single take with these differences:
exact brad instead of the stamp's 0.25 degree table (the stamp is finer than
the engine; both are exact at the tank's square scale), every other pill in
range tested instead of only A, and the leeway ring. On a map with one pill
in range the two evaluators must pick the same W. That is the parity test
(3.7, 7).

How long a blocker lasts vs the time to kill A, N=1, full-health A: tank
kills A in 182 ticks after the first hit (2.3). A fires at its own interval:
calm 100 then 50, 25, 12, 6 after the tank's 1st..4th hits (2.2). Shots A
fires in those 182 ticks: about 1 + 1 + 1 + 1 + (182 - 39 - flight) / 6 =
about 26. Three walls = 15 shots. So today's bar does NOT cover a full
single take either; the tank takes about 11 shots after the walls are gone
unless it is out of A's reach or A's remaining shots miss. Today this works
because: the tank's shells also knock A's armour down while its fire is
slowed by cooldown, the PPT keeps charge_leeway, and the take often starts
on a wounded or calm pill. v2 keeps the same bar (parity first) and reports
the real number in the log line (`need` vs `have`), so the bench can show
whether a higher bar pays. Changing the bar is open question 15.

### 5.4 Side by side first — Andrew's decision

Phase 1 of the build runs both. Today's evaluator drives the bot. v2 runs on
the same inputs in the same tick, logs its choice next to today's, and changes
nothing: no fire, steer, or build effect while the master is off. Knob
`PT2_SIDE_BY_SIDE_LOG` (debug only, default false).

Inputs both see: target pill A, world.pills, U.ttype memory, tank position,
carried trees and pills, blitz state, banned angles, tier. v2 reads them from
the same locals plan_position already has, after today's scan has produced
`state._shield_scan_cache`.

Log line, one per evaluation, print2, hand-computable:

```
PT2_CMP t=<tick> pill=(mx,my) hp=<h> ang=<anger>
  v1: so=(fx,fy) deg=<d> aim=<k> W=<n_act>a+<n_pot>p cover=<shots> score=<s>
  v2: so=(fx,fy) deg=<d> aim=<k> chain=<A>[-<B>[-<C>]] mode=<HOLD|STEP|NONE>
      W=<kind:n,...> need=<shots> have=<shots> safe=<0|1> kills=<k>
      leeway=<p>/16 blk=<r> score=<s> tier=<safe,kills,blk,leeway,score>
  diff=<same|so|W|chain|none_v2|none_v1> why=<first v2 term that rejects v1's point, or "-">
  cost_us v1=<n> v2=<n>
```

`why` is produced by running v2's tests on v1's chosen point and W: the first
failing test name (OVERLAP:<pill id>, OUTGOING, DEPTH need>have, CLOCK,
LEEWAY k<min). When v1's point passes every v2 test but v2 chose another,
`why=score` and the two score terms that differ most are appended.

Analysis script (new, `tools/pt2_compare.py`, Python 3.10): reads
`print2_bot<N>.log` files of a recorded game (or a -brain-debug session dir),
collects PT2_CMP lines, prints: evaluations; same; differ by standoff only
(and the mean distance in wu); differ by W only; v2 found a chain (count by
length and mode); v2 found nothing where v1 did (count, with why); v1 found
nothing where v2 did; mean and max cost_us for each; a list of the ten
largest `why` groups. Output is plain text, one line per figure, so it can be
pasted in a PR comment.

Overlay for the comparison: 6.4.

CPU of running both: today's scan_c is about 0.1-0.3 ms per evaluation. v2 in
Lua, full grid, is 60-150 ms spread over ticks (4). In side-by-side mode v2
must not change the bot's timing either, so it runs with the coarse pass only
(12 directions, PT2_LEEWAY_TOP 1, no build search beyond v1's slots) at
about 2-4 ms, and only on BrainTest or -brain-debug hosts where
`_PRINT2_ENABLED` is set. Recommendation: debug only; never in prod, and
never on the Bot Host while a game is on. The prod comparison is the bench
(5.2 master row), which needs no side-by-side.

Phase order for the side-by-side (replaces the start of section 8):

1. Primitives (8.1).
2. shadow.lua + BrainTest overlays (6).
3. v2 evaluator complete, callable as a pure function
   `pt2.evaluate(state, goal, inputs) -> result`, no side effects.
4. Side-by-side hook in plan_position behind PT2_SIDE_BY_SIDE_LOG + PT2_CMP
   line + tools/pt2_compare.py + overlay 6.4. Record three games on the usual
   bench maps; read the numbers.
5. Fix v2 where the comparison shows it wrong (parity at N=1 is the first
   target: same W on single-pill takes).
6. Master on: wire v2's result into plan_position in place of the scan
   (same result shape, 5.1), engage changes for STEP, live re-checks. ROOST
   arenas (7). Bench matrix (5.2). Default on when the master bench holds.

## 6. Debug visualisation (BrainTest visualizer)

### 6.1 How BrainTest draws brain overlays today

* The brain records shapes through viz.lua (`M.line` :909, `M.circle` :923,
  rect, text, hud_text, hud_rect, `M.detail_*`), which call the braincore
  overlay primitives (braincore.c:3524-3530, 3602-3604). Each command lands in
  an OverlayCmdBuffer (brain_overlay.c:48-105: line, rect, circle, text,
  hud_text, hud_rect) stamped with a `viz_idx`.
* Layers: viz.lua's layer table (:150-420; e.g. `shield_scan_candidates`,
  `shield_scan_blockers`, `shield_blocker_union`, `shield_scan_trajectory`,
  `pill_take_target`, `inspect_pill`) gives each id a short and long text and
  a default. At startup the brain registers them with
  `braintest_viz_register` (braintest_viz_registry.h:14-23;
  vizRegistryAddBrain :51). BrainTest's V dialog toggles rows and pushes
  `_BT_VIZ_<ID>` globals into Lua; `viz.is_on(id)` (viz.lua:705-728) reads
  them, so a brain draws only what is switched on. INI keeps the state.
* Rich text: `overlay_detail(id, kind, geom, label)` and
  `overlay_detail_text(id, line)` register a clickable region
  (braintest_vizdetail_registry.h); the D dialog lists them and a map click
  expands the matching entry.
* Native panels: `brains/GoalHunter/braintest_panels/*.cpp` (pool_grid,
  tier_control) and the shot-sim panel (braintest_shotsimpanel.cpp) are
  ImGui panels compiled into BrainTest for data a shape list cannot show.
* Loaded sessions: overlays are stored packed per bot per tick
  (braintest_main.c:1169-1280), so every shape drawn live is also drawn when
  a recorded -brain-debug session is scrubbed.

There is no polygon primitive, so shadow regions are drawn as cells, and
every cell comes from the same `in_shadow` call the planner uses.

### 6.2 New layers (viz.lua table entries, prefix `pt2_`)

| id | default | what it draws |
|---|---|---|
| pt2_light | off | per pill in view: a circle of 2048 wu (target range) and a thin circle of 2208 wu (shell reach), hostile red, allied green, neutral grey |
| pt2_shadow | off | for the current target and every pill in T(P) of the best candidate: 64 wu cells over the band PT2_RADII min-1 .. max+1 around A; a cell filled when `in_shadow(X, cell)` (with depth, 1.9) is true; one colour per pill, alpha 60; cells in every shadow drawn once more so the overlap is darkest |
| pt2_rays | off | for each X in T(P): the polyline of `cpf_shell_points(X, brad(X,P))` up to its block point; red = reaches the tank; orange = blocked by a hostile pill (a link); green = blocked by W; grey = out of reach. Label at the block point: `d=<have>/<need>` |
| pt2_chain | off | numbered label on each chain pill (1 = A) and an arrow X -> first_block(X); mode and kills text at A |
| pt2_candidates | off | every scored candidate as a small box, colour by tier: green safe, yellow exposed, red rejected; the best as a filled dot; click = detail entry with the tier key and every score term, so the number can be recomputed by hand |
| pt2_leeway | off | the 16 neighbour points of the best: green pass, red fail, with the failing test name |
| pt2_blocker | off | W: existing walls and pills as solid rects, walls to build as outlined rects with build order; dropped-pill slot marked P |
| pt2_exit | off | STEP mode: the exit point as a hollow dot and the corridor P -> exit as a dashed line of in_shadow cells |
| pt2_compare | off | side-by-side phase (6.4) |

Shapes are produced only when the layer is on (viz.is_on), so prod and Bot
Host pay nothing. The cell pass (about 1500 in_shadow calls) runs only with
pt2_shadow on and only on the followed bot.

### 6.3 Read-only visualiser before wiring

Phase 2 ships a tick hook like the June `double_take.lua`: when any pt2_*
layer is on, run `pt2.evaluate` on the bot's current attack target (or the
nearest hostile pill when there is none) and draw. It does not touch goals.
This is how the shadows are checked against real pill shots in recorded
sessions: the recorded shell hit log (shells.c:588) shows where a pill's
shell died; the red ray must end on the same square.

### 6.4 Side-by-side overlay (pt2_compare)

For the PT2_CMP line of the same tick:

* v1's choice: today's `pill_take_target` crosshair stays as is (magenta);
  v1's blockers in magenta outline.
* v2's choice: cyan filled dot at its standoff, cyan outlines on its W, cyan
  chain arrows.
* A line between the two standoffs with the distance in wu and the `diff`
  word; `why` as text under v1's point.
* Detail entry `pt2_cmp` with the whole PT2_CMP text, so the viewer shows the
  same numbers the script counts.

A loaded session scrubs through these frame by frame, which is the way to
read a difference: step to the tick, look at both choices, read `why`.

## 7. Test plan

ROOST arenas (tests/roost/<name>.map + .scenario.lua + .args; verdict line
`ROOST VERDICT PASS/FAIL ...` with numbers). All on the flat order_* island.

| Name | Layout | PASS when |
|---|---|---|
| mt_two_chain_existing_wall | A at (128,120), B at (128,123), three walls at (128,125),(128,126),(128,127) staggered so a corner aim passes; tank starts south | A and B dead, tank alive, walls built = 0, MT_PLAN log says kills=2 mode=HOLD |
| mt_two_chain_build_wall | same, no walls, tank has 10 trees | A and B dead, tank alive, 2-3 walls built on the recorded W squares |
| mt_three_chain_ally_pill_blocker | A, B, C in a line, our pill D as W | B and C killed by pill shells (recordDamage owner NEUTRAL), tank alive, repair of D queued |
| mt_step_back_thin_blocker | A, B, one existing wall only | MT_PLAN mode=STEP, tank fires exactly 4 shells at A, B dead, tank leaves A's range before B dies, tank alive |
| mt_decoy_rejected | B one square off A's ray so A's shells miss it | MT_PLAN kills=1 (no chain), single take runs, no chain line in log |
| mt_shorter_chain_other_angle | B and C placed so the long chain has no buildable W, but the west side has only A in range | plan picks the 1-chain from the west (chosen_deg in 240..300), tank alive |
| mt_ally_nearer_aborts | an ally parks nearer to A after the plan | MT_ABORT ally_nearer logged, tank retreats, alive |

Unit tests (Lua, brains/GoalHunter/test_mt_*.lua, run through BrainTest):

* test_mt_shadow_vs_engine: over a grid of P around three pills and walls,
  `in_shadow` equals a ground-truth walk built from `cpf_simulate_shot_angle`
  plus the hit rule, for every brad.
* test_mt_clock: the 2.5 table numbers.
* test_mt_tiers: ordering of a hand-made candidate list (safe before kills
  before blocker before leeway before score).
* test_mt_n1_parity: new planner vs `scan_c` on recorded single takes.

Bench: after the ROOST tests pass, the standard bench against the MAST
baseline (mast-bench-144), with MT_ENABLED on and off.

## 8. Phases

Each phase is a small testable step. Phases 1-5 change no bot behaviour.

1. Primitives. `cpf_pill_aim_brad`, `cpf_shell_points`. Unit test them against
   `utilCalcAngle` and `shellsAddItem` data. Also file the with_tanks walker
   mismatch (2.4) as a separate small fix.
2. shadow.lua (ray cache, in_shadow with depth) + BrainTest layers pt2_light,
   pt2_shadow, pt2_rays + the read-only visualiser (6.3). Check drawn rays
   against recorded shell deaths. Unit test test_pt2_shadow_vs_engine.
3. Chain graph, clock, safety tiers, candidate search, blocker search, leeway;
   `pt2.evaluate` as a pure function; layers pt2_chain, pt2_candidates,
   pt2_leeway, pt2_blocker, pt2_exit. Unit tests test_pt2_clock,
   test_pt2_tiers.
4. Side by side (5.4): PT2_SIDE_BY_SIDE_LOG hook, PT2_CMP line,
   tools/pt2_compare.py, layer pt2_compare. Record three bench-map games
   with -brain-debug; read the counts.
5. Parity: fix v2 until single-pill takes pick v1's W (test_pt2_n1_parity on
   the recorded games); review the chain cases v2 found by hand in BrainTest.
6. Master on. Plan output in shield.scan shape wired into plan_position
   behind PILLTAKE_V2_EVALUATOR (still default false); engage changes for
   STEP; live re-checks and aborts; goal-cost hook; tier column. ROOST arenas
   (7). Bench matrix (5.2). Default true when the master bench holds.
7. C port of the ray cache and in_shadow (gh_shadow.c), same outputs, same
   tests; then the finer grid at tiers 9-10.

## 9. Open questions for Andrew (with a recommendation each)

1. STEP mode. The clock shows the near pill dies first and the far pill (A)
   exposes the tank for 60-80 ticks unless W is 5 walls deep. STEP kills B for
   4 shells and leaves A angry for a later take. Accept STEP as a plan mode?
   Recommendation: yes, both modes; HOLD ranks above STEP by kills.
2. Does a STEP plan (kills B, not A) count as a chain for the "safe longer
   chain always wins" rule, against a single take of A? Recommendation: count
   dead pills at plan end; a STEP with 1 kill ties a single take on kills and
   wins on score (4 shells, no LGM).
3. Allied-pill bonus size and whether it can beat an extra kill.
   Recommendation: 150 inside the tier; not above kills (knob off).
4. MT_CLOCK_SAFETY 1.25 and the clock's rates (tank 1/13, angry pill 1/6).
   Recommendation: 1.25; revisit after the ROOST runs.
5. Leeway rings 32 and 96 wu; MT_LEEWAY_MIN 6 of 8 at 32 wu as a safety
   condition. Recommendation: as proposed; a point with fewer passing
   neighbours is not safe.
6. Angle step 2 degrees and the tier column. Recommendation: 2 at tiers 9-10.
7. Repair our blocker pill afterwards (MT_REPAIR_AFTER). Recommendation: yes,
   through the existing repair_pill goal.
8. Forest transparent both ways. Recommendation: yes.
9. Solo only in v1 (nearest-target rule breaks blitz chains).
   Recommendation: yes; blitz later, with the commander as bait.
10. Keep N=1 on scan_c until parity passes (master off until phase 6).
    Recommendation: yes.
11. MT_KILL_VALUE 100 and MT_COST_TIER 8. Recommendation: as proposed.
12. Use a carried pill as a dropped W, as today's PPT does.
    Recommendation: yes, reuse num_pill_blockers.
13. Lua first, then C (gh_shadow.c) once the ROOST tests are green.
    Recommendation: yes; the hot loops are small and well bounded.
14. Half wall depth. Today counts a half wall as 5 shots; its real life is
    1..4 and the bot cannot see which. Recommendation: keep 5 for parity in
    phases 1-5; after the bench, consider 2 (a knob PT2_HALF_WALL_SHOTS,
    value needs your yes).
15. The depth bar. Today's 15 shots does not cover a full single take (5.3:
    A fires about 26 shells while the tank kills it). Recommendation: keep
    15 for parity; the PT2_CMP `need/have` numbers and the bench say whether
    a higher bar (or the clock as the bar in HOLD mode) pays.
16. Side-by-side cost. Recommendation: debug hosts only (BrainTest,
    -brain-debug recordings); never in prod or on a Bot Host during a game.

## 10. What differs from the June plan

Kept: chain as a graph of "X's fire hits Y"; W counted in wall-equivalents
(wall 1, pill 3); evaluation per hypothetical build set; output in shield.scan
shape; N=1 reduction; toggles; the pair-axis census as evidence.

Changed:

* Shadows are cast per pill as 256 engine-exact rays and cached, instead of a
  shell sim per bearing per candidate. Neighbours and leeway come free.
* The June plan said "do NOT model timing/HP; validity is purely geometric".
  The engine numbers (2.5) say the near pill dies first and exposes the tank.
  Timing is modelled with a closed-form clock, and a STEP mode is added.
* The June plan said bases of any owner block shells. A pill's shell never
  stops on a base (bases.c:1478); only hostile bases with armour above 4 stop
  the tank's shell.
* The June plan required the standoff and the setup point to be survivor-safe
  as hard rules. Here the standoff is hard (S1) and the setup point is a cost,
  as today's scorer treats it.
* All chain lengths and all angles are ranked in one list with a strict tier
  key, per Andrew's new rules.
* Lua first, C later, instead of C only.
