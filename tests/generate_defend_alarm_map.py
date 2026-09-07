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


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    for (x0, x1, y0, y1) in (FIELD, STRIP):
        for yy in range(y0, y1 + 1):
            for xx in range(x0, x1 + 1):
                t[yy][xx] = GRASS
    # Dig the four start ponds back out to the deep-sea background.
    for (px, py) in (OUR_FAR, OUR_NEAR, FOE_SOUTH, FOE_NORTH):
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

    for name, sq in (("our_far", OUR_FAR), ("our_near", OUR_NEAR),
                     ("foe_south", FOE_SOUTH), ("foe_north", FOE_NORTH)):
        assert terrain[sq[1]][sq[0]] is DEEP_SEA, (
            f"{name} start must be deep sea (starts.c startsIsValidSquare)")
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

    # ...and the moat really does separate the two sides.
    for yy in range(MOAT_Y[0], MOAT_Y[1] + 1):
        for xx in range(min(FIELD[0], STRIP[0]), max(FIELD[1], STRIP[1]) + 1):
            assert terrain[yy][xx] is DEEP_SEA, (xx, yy)

    # armour 15 = PILLS_MAX_HEALTH (alive); speed 50 = normal reload.
    # Owner 0 = the -bots tank; the sidecar re-asserts it anyway.
    pills = [(OUR_PILL[0], OUR_PILL[1], 0, PILLS_MAX_HEALTH, 50)]
    bases = [(OUR_BASE[0], OUR_BASE[1], 0, 90, 90, 90),
             (OUR_BASE_FOE[0], OUR_BASE_FOE[1], 0, 90, 90, 90)]
    starts = [(OUR_FAR[0], OUR_FAR[1], 4),        # 1: ours far,  facing east
              (OUR_NEAR[0], OUR_NEAR[1], 4),      # 2: ours near, facing east
              (FOE_SOUTH[0], FOE_SOUTH[1], 0),    # 3: shooter,   facing north
              (FOE_NORTH[0], FOE_NORTH[1], 8)]    # 4: builder,   facing south

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


if __name__ == '__main__':
    main()
