#!/usr/bin/env python3
"""
Generate the repair-priority arenas (companion to tests/repair_priority_test.py).

THE RULE UNDER TEST (author's, 2026-09-05, verbatim)
---------------------------------------------------
"We need to rebuild the score for repairing pills. it should linearly scale so
closer gets priority, but damage is the most important, up to 11 tiles. The
danger on the tile should not matter. The time to get there is what it should
count for 'distance' (close with lots of swamp in between will be slower to
repair than far but all road) -- it should easily beat any harvest trees, unless
you don't have enough trees to repair of course."

...plus, the same evening: "No path safety. Send that LGM out!"

So builder_pool.lua now prices a rebuild/topup row as

    score = BUILDER_POOL_REPAIR_HP_W(30) x missing_hp
          - BUILDER_POOL_REPAIR_TRIP_W(0.25) x round_trip_ticks

and refuses it only on tree_reserve / ally_repairing / ally_capturing /
mode_owned / fire_exchange / under_fire / reserve-ETA / out_of_leash /
unreachable / MIN_SCORE.  No front clock, no threat term, no path-safety gate.
Five arenas, one per clause.

  A  DAMAGE BEATS DISTANCE.  A 4-hp top-up TWO tiles from the tank and a DEAD
     pill NINE tiles away, both on grass, both affordable.  The corpse is worth
     30 x 15 = 450 against the top-up's 30 x 4 = 120, and 0.25 x its longer trip
     (~300t = 75) comes nowhere near closing that -- so the man walks PAST the
     near job to the far one, then comes back for it.  Under the old formula
     the corpse was not even a candidate: nine tiles is outside the old 8-tile
     leash.

  B  TIME, NOT TILES.  Two DEAD pills.  One is FIVE tiles west with a four-tile
     band of SWAMP in the way; one is EIGHT tiles east on road.  The LGM crosses
     swamp at 4 world-units a tick (64 ticks a tile) and road at 16 (16 ticks a
     tile), so the "near" pill is a ~272-tick walk and the "far" one ~128 --
     and the trip term, which is charged on the SIMULATED WALK and not on
     Manhattan distance, has to send him east first.  This is the author's
     "close with lots of swamp in between will be slower to repair than far but
     all road", laid out as ground.

  C  REPAIR BEATS FARM, and C2 THE TREE GATE.  Same ground twice: a DEAD pill
     two tiles east with the forest right beside it, and the farm row pumped to
     a value of 99 (cfg=BUILDER_POOL_VALUE_FARM=99, standing in for the 5-tree
     woodpile that produces 15 + 12 x (12 - 5) = 99 in a real game).  99 is the
     LARGEST farm value that can coexist with an affordable repair: a top-up
     needs trees - reserve >= 1, i.e. 5 trees with the default reserve of 4,
     and at 5 trees the farm row is worth exactly 99.  C asserts the repair
     goes first; C2, on the same map with cfg=BUILDER_POOL_TREES_REBUILD=99
     ("this rebuild costs more wood than you have"), asserts it is refused
     `tree_reserve(...)` and the farm goes instead.

     WHY A KNOB AND NOT AN EMPTY WOODPILE: -gametype tournament DOES start a
     tank on zero trees (gametype.c gameTypeGetItems) -- but a map with a
     scenario sidecar is force-switched to gameScripted the moment it is
     committed (server_sim_accessors.c, serverSimCommitScenario), and
     gameScripted hands out TANK_FULL_TREES exactly like Open.  Every arena in
     tests/ therefore starts on 40 trees whatever -gametype says, silently.
     Measured: `-gametype tournament` on this map still logged trees=40/res=4
     on the first pool tick.

  D  NO PATH SAFETY, and D2 THE CONTROL.  A dead friendly pill seven tiles west
     with FOUR NEUTRAL PILLBOXES sitting on an unreachable island beyond it,
     close enough that their combined danger field covers the corpse and the
     whole walk.  D (the default, linear) must dispatch the man anyway and must
     never log `path_unsafe`.  D2 is the same ground with
     cfg=BUILDER_POOL_REPAIR_LINEAR=false, and must produce the path_unsafe
     refusal -- which is what proves D's silence is the rule change and not an
     arena that was never dangerous.

     HOW MUCH DANGER (the arithmetic, so the island can be moved without
     re-measuring by trial): one pill contributes
     PILL_DANGER_BASE(8) x (1 - PILL_DANGER_EDGE_FALLOFF(0.5) x d /
     PILL_RANGE_MAP(10)) at euclidean distance d, and grass multiplies the
     total by 16/12.  The four pills sit 3.0, 4.0, 4.12 and 4.12 from the
     corpse: 6.8 + 6.4 + 6.35 + 6.35 = 25.9, x 1.33 = 34.5, comfortably over
     BUILDER_POOL_PATH_DANGER (LGM_DANGER_MED, 20).  From OUR SPAWN they are
     10.0, 11.0, 11.05 and 11.05 away: only the first is inside PILL_RANGE_MAP
     at all and it contributes 5.3, so the tank's own tile is quiet -- and all
     four are outside PILLBOX_RANGE (8 tiles), so nothing ever shoots the tank
     and the under-fire clock cannot be what stopped a dispatch.

THE SHAPE OF EVERY ARENA: a THREE-ROW corridor (y 125..127) with the spawn pond
in it.  Three rows and not more on purpose -- with capture_pill, attack_pill and
repair_pill priced out of reach the bot has nothing to do but explore, and every
tile it wanders to changes the Manhattan distances the arenas are built on.  A
3-row corridor bounds that: |dy| <= 1, so a pill placed 9 tiles away can never
drift past BUILDER_POOL_REPAIR_LEASH (11) however the tank shuffles.

MAP FACTS THAT BITE (the same list generate_builder_pool_map.py keeps, and for
the same reason -- each one cost somebody a run):
  * mapRead recenters the terrain bounding-box midpoint to (126,126), so every
    arena asserts its own midpoint is already there and nothing shifts.
  * a start square must be DEEP SEA (starts.c startsIsValidSquare) -- the
    one-tile ponds.
  * mapRead puts ROAD under every map-file pill, so a pill on a swamp field
    still stands on a fast tile; arena B counts the swamp BETWEEN, not under.
  * -gametype open starts a tank with TANK_FULL_TREES (40).
  * EVERY ALIVE friendly pill needs a friendly base within PILL_FIRE_RANGE (8)
    of it or the reposition pool decides it is badly placed and the bot shoots
    its OWN pill down to move it, which ends the experiment.

Usage:
    python3 tests/generate_repair_priority_map.py [variant] [output_path]
    variant: A | B | C | C2 | D | D2   (default: all of them)
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

SWAMP = 2
ROAD = 4
FOREST = 5
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

# ── Arena A: damage beats distance ───────────────────────────────────────
A_FIELD = (121, 131, 125, 127)     # x0, x1, y0, y1 -- midpoint (126,126)
A_SPAWN = (130, 126)               # one-tile pond at the east end
A_CORPSE = (121, 126)              # 0 armour, ours: 9 tiles from the spawn
A_TOPUP = (128, 125)               # alive, ours: 3 tiles from the spawn, and
                                   # deliberately OFF the y=126 corridor row.
                                   # A LIVE pillbox is impassable to the LGM
                                   # (lgm_man_speed[12] = 0, and the brain
                                   # stamps live pills through cpf_set_lgm_
                                   # blocked to match), so a top-up sitting
                                   # BETWEEN the tank and the corpse makes the
                                   # straight-line walk sim fail and the corpse
                                   # row reads `unreachable` -- which is what
                                   # the first run of this arena did, dispatching
                                   # the top-up at t=115 while the corpse was
                                   # still not a reachable candidate at all.
A_TOPUP_HP = 11                    # 4 missing = exactly TOPUP_MIN_MISSING, the
                                   # SMALLEST top-up the pool will look at --
                                   # i.e. the hardest case for "damage wins":
                                   # 30 x 4 = 120 against the corpse's 450.
A_BASE = (124, 125)                # full stock (no refuel bid) AND the
                                   # reposition guard: 4.1 tiles from the
                                   # top-up, inside PILL_FIRE_RANGE

# ── Arena B: time, not tiles ─────────────────────────────────────────────
B_FIELD = (118, 134, 125, 127)     # midpoint (126,126)
B_SPAWN = (126, 126)               # pond in the middle: one corpse each way
B_ROAD_CORPSE = (134, 126)         # 8 tiles east, road all the way
B_SWAMP_CORPSE = (121, 126)        # 5 tiles west, four tiles of swamp between
B_SWAMP_X = (122, 125)             # the band, full height of the corridor
B_ROAD_X = (127, 134)              # the fast leg, full height, actual T_ROAD
B_BASES = [(130, 125), (119, 126)]  # full stock, one at each end: once a
                                   # corpse is rebuilt it is an ALIVE friendly
                                   # pill again, and an alive pill with no base
                                   # inside PILL_FIRE_RANGE gets shot down by
                                   # the reposition pool

# -- Arena C / C2: repair beats farm, and the tree gate -------------------
# A DEAD pill with the forest right beside it, and that is three decisions:
#
#   DEAD, not damaged.  A live pillbox is LGM-impassable (lgm_man_speed[12] = 0,
#     and the brain stamps live pills through cpf_set_lgm_blocked to match), so
#     the walk sim only reaches one through the blessed-tile exception and a
#     top-up on the corridor read `unreachable` for the first ~140 ticks of two
#     measured runs -- during which the farm won by default.  A corpse is plain
#     walkable ground.  It also has no defend_pill bid and no reposition bid, so
#     the arena needs neither token and both fit inside the 127-byte init arg.
#
#   THE FOREST BESIDE IT.  A repair row pays BUILDER_POOL_REPAIR_TRIP_W (0.25)
#     per trip tick and a farm row still pays the old BUILDER_POOL_TRIP_W (0.5),
#     so the two are only comparable on the SAME ground.  Put them apart and the
#     arena measures which one the tank happened to be standing next to -- it
#     did exactly that twice, with the forest two tiles west of a pill six east
#     (farm 73, repair 59) and again at three west and four east.  The bot has
#     no goal it can afford, so it explores, and it had drifted west both times.
#
#   FOUR FOREST TILES.  Enough for more than one trip, so the farm row is still
#     a live candidate after the rebuild.
#
# From the spawn: corpse 2 tiles = 450 - 0.25 x 84 = 429; forest 2 tiles =
# 99 - 0.5 x 116 = 41.  Both fireable (MIN_SCORE 20), and the corpse wins by
# ten times the farm's margin -- which is the author's "it should easily beat
# any harvest trees".
C_FIELD = (122, 130, 125, 127)     # midpoint (126,126)
C_SPAWN = (127, 126)
C_CORPSE = (129, 126)              # 0 armour, ours: 2 tiles east of the spawn
C_FOREST = [(129, 125), (129, 127),   # flanking the corpse...
            (128, 125), (128, 127)]   # ...and one step back
C_BASE = (127, 127)                # full stock (no refuel bid), and once the
                                   # corpse is rebuilt it is an ALIVE friendly
                                   # pill 2.2 tiles from this base -- inside
                                   # PILL_FIRE_RANGE, so the reposition pool
                                   # leaves it alone

# ── Arena D / D2: no path safety, and the control ────────────────────────
# island | gap | corridor.  The island carries four NEUTRAL pillboxes and is
# surrounded by deep sea, so our tank can never drive to it (attack_pill is
# priced out as well, belt and braces) while its danger field still lies over
# the corpse and over every tile of the man's walk.
D_ISLAND = (114, 116, 125, 127)    # x0, x1, y0, y1
D_GAP_X = 117                      # one column of deep sea: the moat
D_FIELD = (118, 138, 125, 127)     # the corridor; bbox with the island is
                                   # x 114..138 -> midpoint 126
D_SPAWN = (126, 126)
D_CORPSE = (119, 126)              # 0 armour, ours, 7 tiles west
D_NEUTRALS = [(116, 126), (115, 126), (115, 125), (115, 127)]
D_BASE = (130, 126)                # full stock, east, well clear of the island

VARIANTS = ("A", "B", "C", "C2", "D", "D2")


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


def build(variant):
    """Returns terrain, pills, bases, starts for the variant."""
    t = blank()
    if variant == "A":
        fill(t, A_FIELD)
        pond(t, A_SPAWN)
        # (x, y, owner, armour, speed)
        pills = [(A_CORPSE[0], A_CORPSE[1], 0, 0, 50),
                 (A_TOPUP[0], A_TOPUP[1], 0, A_TOPUP_HP, 50)]
        bases = [(A_BASE[0], A_BASE[1], 0, 90, 90, 90)]
        starts = [(A_SPAWN[0], A_SPAWN[1], 12)]        # facing west
    elif variant == "B":
        fill(t, B_FIELD)
        fill(t, (B_ROAD_X[0], B_ROAD_X[1], B_FIELD[2], B_FIELD[3]), ROAD)
        fill(t, (B_SWAMP_X[0], B_SWAMP_X[1], B_FIELD[2], B_FIELD[3]), SWAMP)
        pond(t, B_SPAWN)
        pills = [(B_ROAD_CORPSE[0], B_ROAD_CORPSE[1], 0, 0, 50),
                 (B_SWAMP_CORPSE[0], B_SWAMP_CORPSE[1], 0, 0, 50)]
        bases = [(bx, by, 0, 90, 90, 90) for (bx, by) in B_BASES]
        starts = [(B_SPAWN[0], B_SPAWN[1], 4)]         # facing east
    elif variant in ("C", "C2"):
        fill(t, C_FIELD)
        for (fx, fy) in C_FOREST:
            t[fy][fx] = FOREST
        pond(t, C_SPAWN)
        pills = [(C_CORPSE[0], C_CORPSE[1], 0, 0, 50)]
        bases = [(C_BASE[0], C_BASE[1], 0, 90, 90, 90)]
        starts = [(C_SPAWN[0], C_SPAWN[1], 4)]
    else:  # D, D2
        fill(t, D_ISLAND)
        fill(t, D_FIELD)
        pond(t, D_SPAWN)
        pills = [(D_CORPSE[0], D_CORPSE[1], 0, 0, 50)]
        # The neutrals are written as slot 0's and handed to game.NEUTRAL by the
        # sidecar in on_setup: the .map owner byte has no neutral encoding that
        # mapRead is guaranteed to keep, and the sidecar has to run anyway.
        for (nx, ny) in D_NEUTRALS:
            pills.append((nx, ny, 0, PILLS_MAX_HEALTH, 50))
        bases = [(D_BASE[0], D_BASE[1], 0, 90, 90, 90)]
        starts = [(D_SPAWN[0], D_SPAWN[1], 12)]        # facing west
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

    # Per-arena invariants: every number the test file reasons about, asserted
    # here so a moved tile fails at generation rather than as a mystery run.
    if variant == "A":
        assert mdist(A_SPAWN, A_TOPUP) < mdist(A_SPAWN, A_CORPSE), (
            "arena A needs the TOP-UP to be the near job and the CORPSE the far "
            "one -- that is the whole comparison")
        far = max(mdist((x, y), A_CORPSE)
                  for y in range(A_FIELD[2], A_FIELD[3] + 1)
                  for x in range(A_FIELD[0], A_FIELD[1] + 1))
        assert far <= REPAIR_LEASH, (
            f"arena A: the tank can wander to {far} tiles (manhattan) from the "
            f"corpse, past BUILDER_POOL_REPAIR_LEASH ({REPAIR_LEASH}) -- the "
            f"row would go out_of_leash and the arena would prove nothing")
        assert PILLS_MAX_HEALTH - A_TOPUP_HP >= TOPUP_MIN_MISSING, (
            "arena A: the top-up is not damaged enough to be a candidate at all")
        assert euclid(A_BASE, A_TOPUP) <= 8, (
            "arena A: the alive pill has no friendly base inside PILL_FIRE_RANGE "
            "-- the reposition pool will shoot it down")
        assert A_TOPUP[1] != A_CORPSE[1], (
            "arena A: the ALIVE top-up is on the same row as the corpse and the "
            "spawn -- a live pillbox is impassable to the LGM, so it would sit "
            "in the middle of the walk sim's straight line and the corpse row "
            "would read `unreachable` instead of losing on score")
    if variant == "B":
        assert mdist(B_SPAWN, B_SWAMP_CORPSE) < mdist(B_SPAWN, B_ROAD_CORPSE), (
            "arena B needs the SWAMP corpse to be the nearer one in tiles")
        band = B_SWAMP_X[1] - B_SWAMP_X[0] + 1
        # LGM speeds (brain_pathfinder.c lgm_man_speed): swamp 4 wu/tick, road
        # and grass 16.  One tile is 256 wu.
        swamp_out = band * (256 // 4) + (mdist(B_SPAWN, B_SWAMP_CORPSE) - band) * 16
        road_out = mdist(B_SPAWN, B_ROAD_CORPSE) * 16
        assert road_out < swamp_out, (
            f"arena B: the road walk ({road_out}t) is not shorter than the "
            f"swamp walk ({swamp_out}t) -- widen the band or shorten the road "
            f"leg, or the arena tests nothing")
        for c in (B_ROAD_CORPSE, B_SWAMP_CORPSE):
            assert min(euclid(b, c) for b in B_BASES) <= 8, (
                f"arena B: rebuilt, the corpse at {c} becomes an ALIVE friendly "
                f"pill with no base inside PILL_FIRE_RANGE -- the reposition "
                f"pool would shoot it down")
        for x in range(B_SWAMP_X[0], B_SWAMP_X[1] + 1):
            for y in range(B_FIELD[2], B_FIELD[3] + 1):
                assert terrain[y][x] == SWAMP, (
                    f"arena B: ({x},{y}) is not swamp -- the man would walk "
                    f"around the band")
    if variant in ("C", "C2"):
        for f in C_FOREST:
            assert terrain[f[1]][f[0]] == FOREST, f"arena C: {f} is not forest"
            assert mdist(f, C_CORPSE) <= 2, (
                f"arena C: forest {f} is {mdist(f, C_CORPSE)} tiles from the "
                f"pill. The two rows pay different trip weights, so they are "
                f"only comparable when they are close to the SAME walk -- move "
                f"it back beside the pill or the arena measures the tank's "
                f"position instead of the two values")
        assert euclid(C_BASE, C_CORPSE) <= 8, (
            "arena C: rebuilt, the corpse becomes an ALIVE friendly pill with no "
            "base inside PILL_FIRE_RANGE -- the reposition pool would shoot it "
            "down")
    if variant in ("D", "D2"):
        for y in range(D_ISLAND[2], D_ISLAND[3] + 1):
            assert terrain[y][D_GAP_X] is DEEP_SEA, (
                f"arena D: ({D_GAP_X},{y}) must be the moat -- our tank must "
                f"not be able to drive to the neutral island")
        # The danger arithmetic from the docstring, asserted rather than
        # trusted: the corpse must be hot and our spawn must not.
        def field(tile):
            tot = 0.0
            for n in D_NEUTRALS:
                d = euclid(n, tile)
                if d <= 10:
                    tot += 8 * (1 - 0.5 * d / 10)
            return tot * (16.0 / 12.0)
        assert field(D_CORPSE) > 20, (
            f"arena D: the corpse's danger is only {field(D_CORPSE):.1f}, under "
            f"BUILDER_POOL_PATH_DANGER (20) -- D2 would never log path_unsafe "
            f"and D's silence would prove nothing")
        assert field(D_SPAWN) < 20, (
            f"arena D: our own spawn sits in {field(D_SPAWN):.1f} of danger -- "
            f"the tank tile has to be quiet or the run is about under_fire")
        for n in D_NEUTRALS:
            assert euclid(n, D_SPAWN) > 8, (
                f"arena D: neutral pill {n} is inside PILLBOX_RANGE (8) of our "
                f"spawn -- it would shell the tank and the under-fire clock, "
                f"not the path gate, would decide the run")


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
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes) "
          f"[variant {variant}: {len(pills)} pill(s), {len(bases)} base(s), "
          f"{len(starts)} start(s)]")


def main():
    args = sys.argv[1:]
    variants = [args[0]] if args and args[0] in VARIANTS else list(VARIANTS)
    here = Path(__file__).parent
    for v in variants:
        out = (args[1] if len(args) > 1 and len(variants) == 1
               else str(here / f"repair_priority_{v}.map"))
        write(v, out)


if __name__ == '__main__':
    main()
