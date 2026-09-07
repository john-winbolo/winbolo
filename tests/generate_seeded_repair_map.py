#!/usr/bin/env python3
"""
Generate the seeded-repair arenas (companion to tests/seeded_repair_test.py).

THE INCIDENT (20260905_231835_2_oilrig_2v2_repair_seed1, bot2, t=67922)
-----------------------------------------------------------------------
bot2 held defend_pill on pill #5 at (139,142), 14/15 hp.  The goal's own repair
order SEEDED the builder pool with a one-hp top-up of it.  That seeded row
scored 30 x 1 - 0.25 x 254 = -34 -- under BUILDER_POOL_MIN_SCORE and not worth
the walk -- but it sorted FIRST by construction, and because the feeder goal was
excluded from BUILDER_POOL_TRAVEL_GOALS it also made the pool read
`mode_owned (suppressed/defend_pill)` for every OTHER row.  Ten tiles away pill
#13 at (123,139) sat on 10/15 under fire, worth ~87, and never competed:

    BP_DENY ... reason=under_fire(2t) ... score=-34

TWO KNOBS, TWO ARENAS (2026-09-06)
----------------------------------
  BUILDER_POOL_SEEDED_COMPETES (default true, keel false)
      A seeded row is scored, MIN_SCORE-checked and ORDERED like every other
      row, and its feeder goal no longer imposes a `mode_owned` on the rows it
      competes against.  It keeps the three waivers the seed is FOR -- the mode
      gate for ITSELF, the leash, and the tree reserve.

  BUILDER_POOL_GOAL_PILL_BONUS (default 1.2, keel 1.0)
      The row whose pill IS the tank goal's target (defend_pill / repair_pill
      target_id) has its whole score multiplied by this, after the linear /
      legacy arithmetic and before the MIN_SCORE bar and the ordering.

  A   THE INCIDENT, on the ground.  The tank holds defend_pill on the GOAL PILL
      (14/15 hp) and parks beside it, so the goal seeds a one-hp top-up worth
      30 - 0.25 x 52 = 17.  Six tiles the other way sits the OTHER PILL on
      10/15, worth 30 x 5 - 0.25 x ~200 = ~100.  Default: the man walks to the
      OTHER pill.  Control (both knobs at their keel values): the seeded row
      sorts first, fails MIN_SCORE, and the other row is refused `mode_owned`
      -- NO DISPATCH AT ALL, which is the whole point of the control.

  B   THE BONUS DECIDES.  Two pills 2 tiles apart on the same row, BOTH on
      4/15, with the tank's spawn six tiles due north on the column BETWEEN
      them: (126,120) is exactly 6.08 tiles from each, so the walk to one is
      the walk to the other at every tile of the drive down, and the defend
      hold tile (126,125) is a diagonal neighbour of both.  The two pool rows
      are therefore EQUAL to the tick.  Default: the goal's pill wins on
      goal_w{1.20} alone.  Control (cfg=BUILDER_POOL_GOAL_PILL_BONUS=1.0, with
      SEEDED_COMPETES left at its default -- arena B is about the bonus and
      nothing else): the tie breaks on the tile key, so the OTHER pill (lower
      x, lower key) goes first.

WHY THE GEOMETRY IS WHAT IT IS
------------------------------
  * BOTH pills are inside DEFEND_ARRIVE_RADIUS (10 Euclidean) of the SPAWN, so
    both make the flat DEFEND_REPAIR_COST(40) arrived bid from the first tick.
    Equal bids break to the LOWER PILL INDEX, i.e. to whichever pill this file
    writes FIRST -- so the goal pill is pill #1 in both arenas, and the test
    asserts the goal actually landed on it rather than assuming it.
  * A LIVE pillbox is impassable to the LGM (lgm_man_speed[12] = 0), so no pill
    may sit on the straight line between the tank and another pill.  In A they
    are on the same row but the tank parks on the FAR side of the goal pill; in
    B the man's two walks are the two diagonals out of the hold tile.
  * EVERY alive friendly pill needs a friendly base within PILL_FIRE_RANGE (8)
    or the reposition pool decides it is badly placed and shoots our own pill
    down.  One base covers both pills in each arena, and it is kept OFF the
    man's walk lines and (in B) ON the symmetry column.
  * mapRead recenters the terrain bounding-box midpoint to (126,126); every
    arena asserts its own midpoint is already there so nothing shifts.
  * a start square must be DEEP SEA (starts.c startsIsValidSquare) -- the
    one-tile pond, which the sidecar fills back to grass once the tank is
    ashore (the LGM walk sim walks a STRAIGHT LINE and a hole anywhere on it
    reads `unreachable`).
  * -gametype open starts the tank with TANK_FULL_TREES (40), and a map with a
    scenario sidecar is force-switched to gameScripted, which hands out the
    same 40 whatever -gametype says.
  * EVERY pill stays inside BUILDER_POOL_REPAIR_LEASH (11) of every tile the
    tank can stand on.  That is what keeps pool 5 (repair_pill) out of the
    experiment without spending a cfg= token on it: goals.lua prices a pool-5
    row INF with `builder_can (leash 11, eta N)` as soon as the man can already
    walk to the pill from where the tank is, so repair_pill -- the OTHER feeder
    into the same seed -- never becomes the goal and the only feeder in play is
    the defend->repair handoff.

This file writes the .map AND the .scenario.lua for each variant, so the
geometry lives in exactly one place.

Usage:
    python3 tests/generate_seeded_repair_map.py [variant] [output_path]
    variant: A | A2 | B | B2   (default: all of them)
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256
PILLS_MAX_HEALTH = 15

# constants.lua, mirrored so the test file can read them from one place.
REPAIR_LEASH = 11          # BUILDER_POOL_REPAIR_LEASH
REPAIR_HP_W = 30           # BUILDER_POOL_REPAIR_HP_W
REPAIR_TRIP_W = 0.25       # BUILDER_POOL_REPAIR_TRIP_W
MIN_SCORE = 20             # BUILDER_POOL_MIN_SCORE
TOPUP_MIN_MISSING = 4      # BUILDER_POOL_TOPUP_MIN_MISSING
GOAL_PILL_BONUS = 1.2      # BUILDER_POOL_GOAL_PILL_BONUS
DEFEND_ARRIVE_RADIUS = 10  # DEFEND_ARRIVE_RADIUS
PILL_FIRE_RANGE = 8        # PILL_FIRE_RANGE (the reposition guard)
GRASS_TICKS_PER_TILE = 16  # BUILDER_POOL_GRASS_TICKS_PER_TILE
LGM_BUILD_TIME = 20        # LGM_BUILD_TIME

# ── Arena A: the incident ────────────────────────────────────────────────
A_FIELD = (121, 131, 125, 127)     # x0, x1, y0, y1 -- midpoint (126,126)
A_SPAWN = (128, 126)               # one-tile pond, one tile east of the goal
A_GOAL_PILL = (127, 126)           # pill #1: the defend target, 14/15
A_GOAL_HP = 14                     # ONE missing: under BUILDER_POOL_TOPUP_MIN_
                                   # MISSING, so ordinary discovery does not
                                   # offer this row at all and the seed has to
                                   # SYNTHESISE it -- exactly the incident
A_OTHER_PILL = (121, 126)          # pill #2: the row that never got to bid.
                                   # SIX tiles from the parked tank and seven
                                   # from the spawn -- inside
                                   # BUILDER_POOL_REPAIR_LEASH at every tile of
                                   # the corridor, which is what keeps pool 5
                                   # (repair_pill) INF on it: builder_can_repair
                                   # says the man can already walk it, so the
                                   # tank goal never becomes a relocate and the
                                   # only feeder in play is defend->repair
A_OTHER_HP = 10                    # five missing = 150 of value
A_HOLD = (126, 125)                # where defend_hold_tile parks the tank: the
                                   # lowest-tile-key neighbour of the goal pill
A_BASE = (125, 127)                # full stock (no refuel bid); covers BOTH
                                   # pills for the reposition guard and sits
                                   # off the man's walk line (row 127)

# ── Arena B: the bonus decides ───────────────────────────────────────────
B_FIELD = (122, 130, 120, 132)     # midpoint (126,126). Sized so every
                                   # tile of it is inside
                                   # BUILDER_POOL_REPAIR_LEASH (11,
                                   # manhattan) of BOTH pills -- see the
                                   # pool-5 note in the docstring
B_SPAWN = (126, 120)               # due NORTH of the gap between the pills, so
                                   # the drive down keeps the two walks equal
B_GOAL_PILL = (127, 126)           # pill #1: the defend target
B_OTHER_PILL = (125, 126)          # pill #2: the LOWER tile key, so the keel
                                   # tie-break picks it and the two arms differ
B_HP = 4                           # eleven missing on BOTH: 330 of value each
B_HOLD = (126, 125)                # lowest-key neighbour of the goal pill, and
                                   # a diagonal neighbour of the other one too
B_BASE = (126, 129)                # on the symmetry column, off both walks

VARIANTS = ("A", "A2", "B", "B2")
GROUND = {"A": "A", "A2": "A", "B": "B", "B2": "B"}


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def fill(t, box, tile=GRASS):
    x0, x1, y0, y1 = box
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            t[yy][xx] = tile


def pond(t, sq):
    t[sq[1]][sq[0]] = DEEP_SEA


def euclid(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def trip_ticks(frm, to):
    """The pool's round trip for a walk over flat grass, in brain ticks.

    out = 16 ticks per Euclidean tile (256 wu a tile at the LGM's 16 wu/tick on
    grass), with the <=1 tile shortcut charging a flat GRASS_TICKS_PER_TILE;
    back mirrors it when the tank is standing still, which it is once parked.
    Approximate on purpose -- the test reads the REAL trip off the log; this is
    only here so the generator can assert the arena's margins are wide."""
    d = euclid(frm, to)
    out = GRASS_TICKS_PER_TILE if d <= 1.0 else GRASS_TICKS_PER_TILE * d
    return 2 * out + LGM_BUILD_TIME


def score(frm, to, hp):
    """The linear repair score of a row, before any goal-pill bonus."""
    return REPAIR_HP_W * (PILLS_MAX_HEALTH - hp) - REPAIR_TRIP_W * trip_ticks(frm, to)


def build(variant):
    """Returns terrain, pills, bases, starts for the variant."""
    t = blank()
    if GROUND[variant] == "A":
        fill(t, A_FIELD)
        pond(t, A_SPAWN)
        # (x, y, owner, armour, speed).  ORDER MATTERS: the goal pill is #1, so
        # the two equal DEFEND_REPAIR_COST bids break to it.
        pills = [(A_GOAL_PILL[0], A_GOAL_PILL[1], 0, A_GOAL_HP, 50),
                 (A_OTHER_PILL[0], A_OTHER_PILL[1], 0, A_OTHER_HP, 50)]
        bases = [(A_BASE[0], A_BASE[1], 0, 90, 90, 90)]
        starts = [(A_SPAWN[0], A_SPAWN[1], 12)]        # facing west
    else:
        fill(t, B_FIELD)
        pond(t, B_SPAWN)
        pills = [(B_GOAL_PILL[0], B_GOAL_PILL[1], 0, B_HP, 50),
                 (B_OTHER_PILL[0], B_OTHER_PILL[1], 0, B_HP, 50)]
        bases = [(B_BASE[0], B_BASE[1], 0, 90, 90, 90)]
        starts = [(B_SPAWN[0], B_SPAWN[1], 8)]         # facing south
    return t, pills, bases, starts


def check(variant, terrain, pills, bases, starts):
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    mid = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    assert mid == (126, 126), (
        f"variant {variant}: terrain midpoint is {mid}, not (126,126) -- "
        f"mapRead would shift every coordinate in this file")
    for (sx, sy, _d) in starts:
        assert terrain[sy][sx] is DEEP_SEA, (
            f"variant {variant}: start ({sx},{sy}) must be deep sea "
            f"(starts.c startsIsValidSquare)")
    for (px, py, _o, _a, _s) in pills:
        assert terrain[py][px] is not DEEP_SEA, (
            f"variant {variant}: pill ({px},{py}) is in the sea")
    for (bx, by, *_r) in bases:
        assert terrain[by][bx] is not DEEP_SEA, (
            f"variant {variant}: base ({bx},{by}) is in the sea")

    if GROUND[variant] == "A":
        goal, other, base = A_GOAL_PILL, A_OTHER_PILL, A_BASE
        spawn, hold = A_SPAWN, A_HOLD
        # Both pills must make the ARRIVED defend bid from the spawn, or the
        # cheaper-travel one wins the defend pool and the goal lands on the
        # WRONG pill (the arena would then measure nothing).
        for p in (goal, other):
            assert euclid(spawn, p) <= DEFEND_ARRIVE_RADIUS, (
                f"arena A: pill {p} is {euclid(spawn, p):.2f} tiles from the "
                f"spawn, outside DEFEND_ARRIVE_RADIUS ({DEFEND_ARRIVE_RADIUS})"
                f" -- it would bid the quiet tier (>=250) instead of the flat "
                f"arrived repair bid (40) and the defend pool would not tie")
        assert mdist(hold, other) <= REPAIR_LEASH, (
            f"arena A: parked at {hold} the other pill is "
            f"{mdist(hold, other)} tiles (manhattan) away, past "
            f"BUILDER_POOL_REPAIR_LEASH ({REPAIR_LEASH}) -- its row would read "
            f"out_of_leash and never compete")
        assert PILLS_MAX_HEALTH - A_GOAL_HP < TOPUP_MIN_MISSING, (
            "arena A: the goal pill is damaged enough for ordinary discovery, "
            "so the seed would attach to a normal row instead of SYNTHESISING "
            "one -- and the incident was a synthesised row")
        assert PILLS_MAX_HEALTH - A_OTHER_HP >= TOPUP_MIN_MISSING, (
            "arena A: the other pill is not damaged enough to be a row at all")
        # The margins the whole arena turns on, at the parked position.
        s_goal = score(hold, goal, A_GOAL_HP)
        s_other = score(hold, other, A_OTHER_HP)
        assert s_goal * GOAL_PILL_BONUS < s_other, (
            f"arena A: parked, the seeded goal row scores {s_goal:.0f} x "
            f"{GOAL_PILL_BONUS} = {s_goal * GOAL_PILL_BONUS:.0f} against the "
            f"other pill's {s_other:.0f} -- the DEFAULT run would dispatch to "
            f"the goal pill and prove nothing")
        assert s_goal < MIN_SCORE, (
            f"arena A: the seeded row scores {s_goal:.0f}, at or above "
            f"BUILDER_POOL_MIN_SCORE ({MIN_SCORE}) -- the CONTROL run would "
            f"dispatch the man to it instead of refusing everything")
        assert s_other >= MIN_SCORE, (
            f"arena A: the other pill scores {s_other:.0f}, under MIN_SCORE "
            f"({MIN_SCORE}) -- it could not fire even with the mode gate open")
    else:
        goal, other, base = B_GOAL_PILL, B_OTHER_PILL, B_BASE
        spawn, hold = B_SPAWN, B_HOLD
        assert goal[1] == other[1] and abs(goal[0] - other[0]) == 2, (
            "arena B: the two pills must straddle one free column, so the tank "
            "can park between them and neither blocks the other's walk line")
        assert spawn[0] == (goal[0] + other[0]) // 2, (
            "arena B: the spawn must sit on the symmetry column between the "
            "pills -- that is what makes the two walks equal at every tile of "
            "the drive down")
        assert abs(euclid(spawn, goal) - euclid(spawn, other)) < 1e-9, (
            "arena B: the spawn is not equidistant from the two pills")
        assert abs(euclid(hold, goal) - euclid(hold, other)) < 1e-9, (
            "arena B: the defend hold tile is not equidistant from the two "
            "pills -- parked, the pool would decide on trip and not on the "
            "goal-pill bonus")
        for p in (goal, other):
            assert euclid(spawn, p) <= DEFEND_ARRIVE_RADIUS, (
                f"arena B: pill {p} is outside DEFEND_ARRIVE_RADIUS of the "
                f"spawn, so the two defend bids would not tie")
            assert mdist(hold, p) <= REPAIR_LEASH, (
                f"arena B: pill {p} is out of the repair leash from {hold}")
        assert (other[1] * 256 + other[0]) < (goal[1] * 256 + goal[0]), (
            "arena B: the GOAL pill has the lower tile key, so the control's "
            "tie-break would pick it too and the two arms would agree")
        assert base[0] == spawn[0], (
            "arena B: the base is off the symmetry column and would tilt one "
            "walk against the other")
        s = score(hold, goal, B_HP)
        assert s >= MIN_SCORE, (
            f"arena B: the rows score {s:.0f}, under MIN_SCORE ({MIN_SCORE})")

    # Pool 5 must never get a row: every tile the tank can reach has to be
    # inside BUILDER_POOL_REPAIR_LEASH of both pills, or repair_pill becomes a
    # "relocate" goal, seeds the pool itself, and the arena is measuring the
    # wrong feeder.
    x0, x1, y0, y1 = A_FIELD if GROUND[variant] == "A" else B_FIELD
    for p in (goal, other):
        far = max(mdist((x, y), p)
                  for y in range(y0, y1 + 1) for x in range(x0, x1 + 1))
        assert far <= REPAIR_LEASH, (
            f"variant {variant}: the tank can wander to {far} tiles (manhattan) "
            f"from pill {p}, past BUILDER_POOL_REPAIR_LEASH ({REPAIR_LEASH}) -- "
            f"out there pool 5 (repair_pill) stops being INF and takes over as "
            f"the feeder")

    for p in (goal, other):
        assert euclid(base, p) <= PILL_FIRE_RANGE, (
            f"variant {variant}: alive friendly pill {p} has no base inside "
            f"PILL_FIRE_RANGE ({PILL_FIRE_RANGE}) -- the reposition pool would "
            f"shoot our own pill down and end the experiment")
        assert base != p, f"variant {variant}: the base is on a pill"


# ── the scenario sidecar, written from the same geometry ──────────────────
SCENARIO = '''\
-- Scenario sidecar for tests/seeded_repair_{v}.map (auto-loaded as
-- <map>.scenario.lua).  GENERATED by tests/generate_seeded_repair_map.py --
-- edit that file, not this one.
--
-- Arena {v}: {blurb}
--
-- It does three things and nothing else:
--   OWNERSHIP.  Both pills and the base are player 0's; the .map's owner byte
--     is not something to rely on for a scripted game.
--   THE POND.  A start square has to be DEEP SEA at map load (starts.c
--     startsIsValidSquare), which leaves a one-tile hole in the field -- and
--     the LGM walk sim (brainPathfinderLgmTravelTicks) walks a STRAIGHT LINE,
--     so a hole anywhere on it makes the target read `unreachable`.  The pond
--     is filled back to grass once the tank is ashore and OFF the tile (filling
--     it under the tank leaves the boat state stuck and the LGM never becomes
--     available at all).
--   THE TRACE.  Every change of ARMOUR, OWNER or IN_TANK on the arena's pills,
--     in SIM ticks, so "the repair actually landed" is asked of the ENGINE --
--     and so is the harder question armour alone cannot answer: that the pill
--     came back up without anybody capturing it or picking it up first.
--     Rows are `tick x y armour owner in_tank`, one per CHANGE.  x/y are the
--     pill's ORIGINAL tile (its identity here), not its live position.

local TRACE = "seeded_repair_{v}_trace.log"
local OURS  = {{ {{ {gx}, {gy} }}, {{ {ox}, {oy} }} }}
local SPAWN = {{ {sx}, {sy} }}
local GRASS = 7
local FILL_TICK = 60             -- engine ticks: the tank is ashore well before
local p0 = 0

local last = {{}}
local filled = false
local traced = nil               -- {{ {{pill_index, tile_x, tile_y}}, ... }}

local function same(p, t) return p.x == t[1] and p.y == t[2] end

-- Resolve OURS (tile coords) to PILL INDICES once, then read the pills BY
-- INDEX for ever after: a pill that is picked up moves with the carrier, so a
-- coordinate match would stop finding it at exactly the moment the trace exists
-- to prove nobody took it.
local function resolve(g)
  local out = {{}}
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      for _, t in ipairs(OURS) do
        if same(p, t) then out[#out + 1] = {{ i, t[1], t[2] }} end
      end
    end
  end
  return out
end

local function own_everything(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then g.set_pill_owner(i, p0) end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      g.set_base_owner(i, p0)
      g.set_base_stock(i, 90, 90, 90)
    end
  end
end

function on_setup(g)
  own_everything(g)
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# tick x y armour owner in_tank\\n") f:close() end
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_tick(g, tick)
  if not filled and tick >= FILL_TICK then
    local tk = g.tank(p0)
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message(string.format(
        "SEEDED_REPAIR_{v} filled the spawn pond at (%d,%d) with grass at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end
  if not traced then traced = resolve(g) end
  for _, e in ipairs(traced) do
    local p = g.pill(e[1])
    if p then
      local in_tank = p.in_tank and 1 or 0
      local sig = p.armour .. "/" .. p.owner .. "/" .. in_tank
      if last[e[1]] ~= sig then
        local f = io.open(TRACE, "a")
        if f then
          f:write(string.format("%d %d %d %d %d %d\\n",
                                tick, e[2], e[3], p.armour, p.owner, in_tank))
          f:close()
        end
        last[e[1]] = sig
      end
    end
  end
end
'''

BLURB = {
    "A": "the incident -- a seeded one-hp top-up against a five-hp repair "
         "nine tiles off. The default run walks PAST the goal pill; the "
         "control (arena A2) refuses both rows and sends nobody.",
    "A2": "the CONTROL for arena A, on the same ground, with both knobs at "
          "their keel values. NO DISPATCH IS EXPECTED HERE.",
    "B": "the bonus decides -- two identical pills, one of them the defend "
         "target, equidistant from the tank at every tile of the drive down.",
    "B2": "the CONTROL for arena B, same ground, "
          "cfg=BUILDER_POOL_GOAL_PILL_BONUS=1.0: the tie breaks on the tile "
          "key and the OTHER pill goes first.",
}


def write(variant, output):
    terrain, pills, bases, starts = build(variant)
    check(variant, terrain, pills, bases, starts)

    with open(output, 'wb') as f:
        f.write(b'BMAPBOLO')
        f.write(struct.pack('B', 1))
        f.write(struct.pack('B', len(pills)))
        f.write(struct.pack('B', len(bases)))
        f.write(struct.pack('B', len(starts)))
        for x, y, owner, armour, speed in pills:
            f.write(struct.pack('BBBBB', x, y, owner, armour, speed))
        for x, y, owner, armour, shells, mines in bases:
            f.write(struct.pack('BBBBBB', x, y, owner, armour, shells, mines))
        for x, y, dr in starts:
            f.write(struct.pack('BBB', x, y, dr))
        f.write(encode_map_runs(terrain))

    goal = A_GOAL_PILL if GROUND[variant] == "A" else B_GOAL_PILL
    other = A_OTHER_PILL if GROUND[variant] == "A" else B_OTHER_PILL
    spawn = A_SPAWN if GROUND[variant] == "A" else B_SPAWN
    sc = Path(output).with_suffix("").with_suffix("")
    sc = Path(str(Path(output))[:-len(".map")] + ".scenario.lua")
    sc.write_text(SCENARIO.format(v=variant, blurb=BLURB[variant],
                                  gx=goal[0], gy=goal[1],
                                  ox=other[0], oy=other[1],
                                  sx=spawn[0], sy=spawn[1]),
                  encoding="utf-8", newline="\n")
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes) and "
          f"{sc.name} [variant {variant}: {len(pills)} pill(s), "
          f"{len(bases)} base(s), {len(starts)} start(s)]")


def main():
    args = sys.argv[1:]
    variants = [args[0]] if args and args[0] in VARIANTS else list(VARIANTS)
    here = Path(__file__).parent
    for v in variants:
        out = (args[1] if len(args) > 1 and len(variants) == 1
               else str(here / f"seeded_repair_{v}.map"))
        write(v, out)


if __name__ == '__main__':
    main()
