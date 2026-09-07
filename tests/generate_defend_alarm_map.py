#!/usr/bin/env python3
"""
Generate the defend_pill ALARM MODE arena (companion to
tests/defend_alarm_test.py).

WHAT THE ARENA HAS TO MAKE HAPPEN

Alarm mode (constants.lua DEFEND_ALARM_*) rejects defend_pill unless ALL of

  1. a hostile tank is VISIBLE RIGHT NOW within DEFEND_ALARM_ENEMY_TILES (11)
     of the pill,
  2. AND either the pill took enemy damage inside DEFEND_ALARM_WINDOW_TICKS
     (250 = 5 s) or a wall/hostile pill went up inside
     DEFEND_ALARM_BUILD_RADIUS (4) of it in that window WITH a hostile LGM
     seen in the same stamp,
  3. AND we are MORE than DEFEND_ALARM_MIN_DIST (9) tiles from the pill.

so one piece of ground has to be able to produce, on demand: an enemy that can
be seen at the pill, damage on the pill, a wall going up beside the pill, our
tank both FAR from the pill and CLOSE to it, and the enemy VANISHING.

THE VISIBILITY CONSTRAINT THAT SETS EVERY DISTANCE HERE.  Condition 1 is about
what the brain can SEE, and the brain sees tanks in its own view rect plus a
15x15 rect around each deployed TEAM PILL (brain_data.c, "Team pill view
sweep").  15x15 is +-7 tiles.  So an enemy meant to satisfy condition 1 while
our tank is far away has to sit within 7 tiles of the pill, not 11 -- an enemy
at 10 tiles would satisfy the RULE and still never reach the brain.  Every foe
position below is 6 tiles from the pill for that reason, and the test says so
where it matters.

SHAPE (file coordinates; mapRead's recenter is a no-op here -- main() asserts
the terrain bbox midpoint is already (126,126)).

    y=108..127  x=108..144   our field.  Our pill P at (126,126), our base at
                             (122,122).  Three start ponds (see below).
    y=128..130  DEEP SEA     a 3-row moat across the whole width.  No river
                             anywhere, so no boat can ever be built: the
                             SOUTH shooter is permanently unreachable and
                             attack_tank / kill_lgm price at INF instead of
                             pulling the bot off the experiment.  Shells fly
                             over water, so it can still hit the pill.
    y=131..144  x=108..144   the shooter's strip.

START PONDS (start squares must be DEEP SEA -- starts.c startsIsValidSquare --
so each is a one-tile pond the tank beaches itself out of on the first step):

  1  OUR_FAR   (110,126)   16 tiles WEST of the pill.  Satisfies condition 3
                           with room to spare, and stays outside it for long
                           enough to watch the alarm go on and off.  WEST, not
                           north or south: with the bot, the pill and the
                           shooter collinear the bot's own shots go through
                           its own pill (the mistake tests/defend_repair made
                           first and documents).
  2  OUR_NEAR  (120,126)    6 tiles WEST of the pill -- INSIDE
                           DEFEND_ALARM_MIN_DIST, which is the whole of
                           arena C.
  3  FOE_SOUTH (126,138)   the shooter's pond on the far strip; it drives
                           north to FOE_STANDOFF (126,132), 6 tiles from the
                           pill, and shells it from there.
  4  FOE_NORTH (126,114)   the builder's pond, on OUR side of the moat.  It
                           drives south to FOE_BUILD_SPOT (126,120), 6 tiles
                           NORTH of the pill, and sends its LGM to wall
                           (126,122) -- 4 tiles from the pill, i.e. the outer
                           ring of the build stamp, so the arena also proves
                           the stamp really is 4 tiles and not 3.

WHY THE SHOOTER IS 6 TILES OUT AND NOT 8.  A tank shell dies at
shellLifeTicks(GUNSIGHT_MAX/2 = 7) = 51 ticks x SHELL_SPEED 32 = 1632 wu,
about 6.4 tiles.  8 tiles would fall short and the pill would never be hit.
6 is inside the reach, inside the 11-tile alarm ring and inside the pill's own
15x15 view rect, which is every constraint at once.  It is also inside
PILL_FIRE_RANGE (8), so the pill shoots back once angered -- deliberate, that
is what a siege looks like, and a dead shooter is exactly the "enemy gone"
half of arena A.

NO FOREST anywhere (seek_trees must not compete), NO other pills
(attack_pill / capture_pill never bid), one base each side.  The far-side base
is not decoration: without an enemy asset south of the moat the front line
never reaches our pill, pill_portfolio calls it a BACK pill and the reposition
pool bids to shoot our own pill down and move it, which is not the experiment.

Usage:
    python3 tests/generate_defend_alarm_map.py [output_path]
    Default output: tests/defend_alarm.map
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

GRASS = 7
DEEP_SEA = None          # background sentinel (unwritten cells read as deep sea)
MAP_SIZE = 256

# ── Geometry (the test runner and the scenario sidecar import/mirror these) ──
FIELD = (108, 144, 108, 127)        # x0, x1, y0, y1 inclusive -- our side
MOAT_Y = (128, 130)                 # inclusive rows of deep sea
STRIP = (108, 144, 131, 144)        # the shooter's side

OUR_PILL = (126, 126)
OUR_BASE = (122, 122)

OUR_FAR = (110, 126)                # start 1: 18 tiles W of the pill
OUR_NEAR = (120, 126)               # start 2:  6 tiles W of the pill
FOE_SOUTH = (126, 138)              # start 3: the shooter's pond
FOE_NORTH = (126, 114)              # start 4: the builder's pond (our side)

FOE_STANDOFF = (126, 132)           # shooter brakes here, 6 tiles from P
FOE_BUILD_SPOT = (126, 120)         # builder brakes here, 6 tiles from P
FOE_WALL = (126, 122)               # the tile its LGM walls, 4 tiles from P
OUR_BASE_FOE = (126, 133)           # hostile base (owner set by the sidecar)

# ── Arena W / WC: the WELL-DEFENDED count (starts 5..10) ──────────────────
# Alarm mode's condition 4 rejects a pill that is ALREADY HELD:
#   R = ceil(their_team / our_team); held when foes_near <= allies_near * R,
# both counted within DEFEND_WELL_DEFENDED_RADIUS (10) euclidean tiles of the
# pill, the bidder itself excluded.  So the arena needs, on top of arena A's
# shooter, four more bodies:
#
#   * TWO ALLIED tanks parked close to P.  They must be inside the pill's own
#     15x15 view rect (+-7 tiles), not merely inside the 10-tile well-defended
#     radius -- our observer sits 16 tiles west and can only see them THROUGH
#     that rect, exactly as arena A's shooter is only visible through it.  6
#     and 5.7 tiles out, which is inside 7 and inside 10 with room to spare.
#   * TWO ENEMY tanks parked FAR from P (18 tiles, on the shooter's strip
#     across the moat).  Their only job is the arithmetic: with 3 a side,
#     R = ceil(3/3) = 1, so the printed reject reads `foes 1 <= allies 2 x 1`
#     and the ratio in it came from real team sizes.  Far enough out that they
#     can never be counted in foes_near themselves.
#
# WC is the CONTROL and changes exactly one thing: the two allies park FAR
# instead (14 and 18 tiles out), so allies_near is 0, the pill is NOT held and
# the alarm must fire normally.  Note that "remove one of the two" is NOT a
# control -- with R=1 a single ally still covers a single foe (1 <= 1 x 1) and
# the pill would still be held.  Zero allies near is the only way to flip it
# with one shooter, which is why the two park spots move rather than one tank
# disappearing.
#
# Each park spot is a straight single-axis drive from its own pond (see
# tests/brains/park_at.lua), and no route crosses another pond -- a tank that
# drove over a one-tile deep-sea pond would drown mid-arena.
ALLY_NEAR_A_START = (134, 126)      # pond; drives WEST to...
ALLY_NEAR_A = (132, 126)            #   6.0 tiles E of P
ALLY_NEAR_B_START = (130, 110)      # pond; drives SOUTH to...
ALLY_NEAR_B = (130, 122)            #   5.7 tiles NE of P
ALLY_FAR_A_START = (112, 112)       # pond; drives EAST to...
ALLY_FAR_A = (114, 112)             #   18.4 tiles from P (WC control)
ALLY_FAR_B_START = (112, 118)       # pond; drives EAST to...
ALLY_FAR_B = (114, 118)             #   14.4 tiles from P (WC control)
FOE_FILL_A_START = (112, 140)       # pond on the strip; drives EAST to...
FOE_FILL_A = (114, 140)             #   18.4 tiles from P
FOE_FILL_B_START = (140, 140)       # pond on the strip; drives WEST to...
FOE_FILL_B = (138, 140)             #   18.4 tiles from P

WELL_DEFENDED_RADIUS = 10           # C.DEFEND_WELL_DEFENDED_RADIUS

PILLS_MAX_HEALTH = 15

# Knob values the arena is built around; the test asserts the brain's own
# numbers against these, so a knob change that this ground no longer exercises
# fails loudly instead of quietly measuring nothing.
ALARM_ENEMY_TILES = 11
ALARM_MIN_DIST = 9
ALARM_BUILD_RADIUS = 4
ALARM_WINDOW_TICKS = 250
ALARM_BASE_COST = 100
ALARM_HIT_DISCOUNT = 10
ALARM_MIN_COST = 50
ALARM_DIJ_STOP_TILES = 9
PILL_VIEW_HALF = 7                  # the team-pill view rect is 15x15

# Start ponds, in START-RECORD ORDER: the scenario sidecar picks a start by
# INDEX (on_choose_start returns 1-based), so this list and the `starts` tuple
# written into the map below are the same order, and both are indexed by the
# names in the comment on each line.
PONDS = [
    (OUR_FAR, 4),            # 1  ours, far   -- facing east
    (OUR_NEAR, 4),           # 2  ours, near  -- facing east
    (FOE_SOUTH, 0),          # 3  shooter     -- facing north
    (FOE_NORTH, 8),          # 4  builder     -- facing south
    (ALLY_NEAR_A_START, 12), # 5  ally near A -- facing west
    (ALLY_NEAR_B_START, 8),  # 6  ally near B -- facing south
    (ALLY_FAR_A_START, 4),   # 7  ally far  A -- facing east
    (ALLY_FAR_B_START, 4),   # 8  ally far  B -- facing east
    (FOE_FILL_A_START, 4),   # 9  foe filler A -- facing east
    (FOE_FILL_B_START, 12),  # 10 foe filler B -- facing west
]
ALL_PONDS = [sq for sq, _dr in PONDS]
# Which pond each parked tank drives off, and the tile it stops on.  The
# scenario hands the destination to tests/brains/park_at.lua as BRAIN_INIT_ARG.
PARK_ROUTES = {
    "ally_near_a": (ALLY_NEAR_A_START, ALLY_NEAR_A),
    "ally_near_b": (ALLY_NEAR_B_START, ALLY_NEAR_B),
    "ally_far_a":  (ALLY_FAR_A_START, ALLY_FAR_A),
    "ally_far_b":  (ALLY_FAR_B_START, ALLY_FAR_B),
    "foe_fill_a":  (FOE_FILL_A_START, FOE_FILL_A),
    "foe_fill_b":  (FOE_FILL_B_START, FOE_FILL_B),
}


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    for (x0, x1, y0, y1) in (FIELD, STRIP):
        for yy in range(y0, y1 + 1):
            for xx in range(x0, x1 + 1):
                t[yy][xx] = GRASS
    # Dig the start ponds back out to the deep-sea background.
    for (px, py) in ALL_PONDS:
        t[py][px] = DEEP_SEA
    return t


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else str(
        Path(__file__).parent / "defend_alarm.map")
    terrain = make_map()

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))

    for i, (sq, _dr) in enumerate(PONDS, start=1):
        assert terrain[sq[1]][sq[0]] is DEEP_SEA, (
            f"start {i} at {sq} must be deep sea "
            f"(starts.c startsIsValidSquare)")
    assert len(set(ALL_PONDS)) == len(ALL_PONDS), "two starts share a pond"
    for name, sq in (("pill", OUR_PILL), ("standoff", FOE_STANDOFF),
                     ("build_spot", FOE_BUILD_SPOT), ("wall", FOE_WALL),
                     ("foe_base", OUR_BASE_FOE)):
        assert terrain[sq[1]][sq[0]] is not DEEP_SEA, name

    # ── The distances the arena's whole meaning rests on ──────────────────
    d_far = edist(OUR_FAR, OUR_PILL)
    d_near = edist(OUR_NEAR, OUR_PILL)
    d_shoot = edist(FOE_STANDOFF, OUR_PILL)
    d_build_tank = edist(FOE_BUILD_SPOT, OUR_PILL)
    d_wall = edist(FOE_WALL, OUR_PILL)
    assert d_far > ALARM_MIN_DIST + 5, (
        f"the far start is {d_far:.1f} from the pill; condition 3 needs it "
        f"comfortably past {ALARM_MIN_DIST}")
    assert d_near <= ALARM_MIN_DIST, (
        f"the near start is {d_near:.1f} from the pill; arena C needs it "
        f"INSIDE {ALARM_MIN_DIST} so the alarm reads too_close")
    assert d_shoot <= ALARM_ENEMY_TILES, (
        f"the shooter is {d_shoot:.1f} from the pill -- outside the "
        f"{ALARM_ENEMY_TILES}-tile alarm ring, so condition 1 would never hold")
    assert d_shoot <= PILL_VIEW_HALF, (
        f"the shooter is {d_shoot:.1f} from the pill -- outside the pill's "
        f"15x15 view rect, so the brain would never SEE it while our tank is "
        f"far away, and condition 1 would fail for a reason the test is not "
        f"about")
    assert (abs(FOE_STANDOFF[0] - OUR_PILL[0])
            + abs(FOE_STANDOFF[1] - OUR_PILL[1])) <= 6, (
        "standoff is further than 6 tiles -- a tank shell dies at ~6.4 and the "
        "pill would never be hit")
    assert d_build_tank <= PILL_VIEW_HALF, d_build_tank
    assert d_wall <= ALARM_BUILD_RADIUS, (
        f"the wall tile is {d_wall:.1f} from the pill -- outside the "
        f"{ALARM_BUILD_RADIUS}-tile build stamp, so trigger 2b could not fire")
    assert d_wall > ALARM_BUILD_RADIUS - 1, (
        f"the wall tile is {d_wall:.1f} from the pill -- put it on the OUTER "
        f"ring of the stamp so the arena also proves the radius is "
        f"{ALARM_BUILD_RADIUS} and not smaller")

    # ── Arena W / WC: the well-defended count ─────────────────────────────
    # The two NEAR allies have to be inside BOTH the well-defended radius (or
    # they are not counted) and the pill's 15x15 view rect (or our far-away
    # observer never sees them, and the arena would be measuring visibility
    # rather than the rule).
    for name, sq in (("ally_near_a", ALLY_NEAR_A), ("ally_near_b", ALLY_NEAR_B)):
        d = edist(sq, OUR_PILL)
        assert d <= WELL_DEFENDED_RADIUS, (
            f"{name} parks {d:.1f} tiles from the pill -- outside "
            f"DEFEND_WELL_DEFENDED_RADIUS {WELL_DEFENDED_RADIUS}, so it would "
            f"not be counted as a defender at all")
        assert d <= PILL_VIEW_HALF, (
            f"{name} parks {d:.1f} tiles from the pill -- outside the pill's "
            f"15x15 view rect, so the observer 16 tiles west would never SEE "
            f"it and allies_near would stay 0 for the wrong reason")
    # The FAR allies (the WC control) and the two filler foes must be well
    # outside the radius, or the control would be held too and prove nothing.
    for name, sq in (("ally_far_a", ALLY_FAR_A), ("ally_far_b", ALLY_FAR_B),
                     ("foe_fill_a", FOE_FILL_A), ("foe_fill_b", FOE_FILL_B)):
        d = edist(sq, OUR_PILL)
        assert d > WELL_DEFENDED_RADIUS + 3, (
            f"{name} parks {d:.1f} tiles from the pill -- too close to "
            f"DEFEND_WELL_DEFENDED_RADIUS {WELL_DEFENDED_RADIUS} to be safely "
            f"uncounted")
    # Every park route is a straight single-axis drive over solid ground that
    # never crosses another pond: park_at.lua does no pathfinding, and a tank
    # driven over a one-tile deep-sea pond drowns mid-arena.
    ponds = set(ALL_PONDS)
    for name, (start, dest) in PARK_ROUTES.items():
        assert start[0] == dest[0] or start[1] == dest[1], (
            f"{name}'s route {start}->{dest} is not single-axis; "
            f"park_at.lua drives Y then X and would cut a corner")
        if start[0] == dest[0]:
            leg = [(start[0], yy) for yy in
                   range(min(start[1], dest[1]), max(start[1], dest[1]) + 1)]
        else:
            leg = [(xx, start[1]) for xx in
                   range(min(start[0], dest[0]), max(start[0], dest[0]) + 1)]
        # Skip the START square itself -- it IS a pond by construction; every
        # OTHER tile the tank rolls over has to be solid ground.
        for sq in [s for s in leg if s != start]:
            assert terrain[sq[1]][sq[0]] is not DEEP_SEA, (
                f"{name}'s route crosses deep sea at {sq} -- it would drown")
            assert sq not in ponds, (
                f"{name}'s route crosses another start pond at {sq}")

    # ...and the moat really does separate the two sides.
    for yy in range(MOAT_Y[0], MOAT_Y[1] + 1):
        for xx in range(min(FIELD[0], STRIP[0]), max(FIELD[1], STRIP[1]) + 1):
            assert terrain[yy][xx] is DEEP_SEA, (xx, yy)

    # armour 15 = PILLS_MAX_HEALTH (alive); speed 50 = normal reload.
    # Owner 0 = the -bots tank; the sidecar re-asserts it anyway.
    pills = [(OUR_PILL[0], OUR_PILL[1], 0, PILLS_MAX_HEALTH, 50)]
    bases = [(OUR_BASE[0], OUR_BASE[1], 0, 90, 90, 90),
             (OUR_BASE_FOE[0], OUR_BASE_FOE[1], 0, 90, 90, 90)]
    starts = [(sq[0], sq[1], dr) for sq, dr in PONDS]

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

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  our pill {OUR_PILL}, our base {OUR_BASE}")
    print(f"  our far start {OUR_FAR} ({d_far:.1f} tiles from the pill, "
          f"> DEFEND_ALARM_MIN_DIST {ALARM_MIN_DIST})")
    print(f"  our near start {OUR_NEAR} ({d_near:.1f} tiles, INSIDE it)")
    print(f"  moat rows y={MOAT_Y[0]}..{MOAT_Y[1]} (no river anywhere -> the "
          f"south shooter is permanently unreachable)")
    print(f"  shooter start {FOE_SOUTH}, standoff {FOE_STANDOFF} "
          f"({d_shoot:.1f} tiles from the pill, inside both the "
          f"{ALARM_ENEMY_TILES}-tile ring and the pill's 15x15 view rect)")
    print(f"  builder start {FOE_NORTH}, brake {FOE_BUILD_SPOT} "
          f"({d_build_tank:.1f} tiles), walls {FOE_WALL} ({d_wall:.1f} tiles, "
          f"the outer ring of the {ALARM_BUILD_RADIUS}-tile stamp)")
    print(f"  arena W  allies park {ALLY_NEAR_A} "
          f"({edist(ALLY_NEAR_A, OUR_PILL):.1f} tiles) and {ALLY_NEAR_B} "
          f"({edist(ALLY_NEAR_B, OUR_PILL):.1f} tiles) -- both inside "
          f"DEFEND_WELL_DEFENDED_RADIUS {WELL_DEFENDED_RADIUS} and inside the "
          f"pill's 15x15 view rect")
    print(f"  arena WC the same allies park {ALLY_FAR_A} "
          f"({edist(ALLY_FAR_A, OUR_PILL):.1f} tiles) and {ALLY_FAR_B} "
          f"({edist(ALLY_FAR_B, OUR_PILL):.1f} tiles) -- allies_near 0, so "
          f"the pill is NOT held and the alarm must fire")
    print(f"  filler foes park {FOE_FILL_A} and {FOE_FILL_B} "
          f"({edist(FOE_FILL_A, OUR_PILL):.1f} tiles) -- teams 3v3, "
          f"R = ceil(3/3) = 1")


if __name__ == '__main__':
    main()
