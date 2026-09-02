#!/usr/bin/env python3
"""
Generate the defend->repair arena (companion to tests/defend_repair_test.py).

What the arena has to make happen, in order:

  1. our pill is SHELLED by an enemy tank while our bot is parked beside it;
  2. the shelling STOPS and the shooter drives away;
  3. our bot repairs the pill -- but not one tick before the hits stop.

Shape (file coordinates; mapRead's recenter is a no-op here, see main()):

    y=108..127   x=108..144   our field.  Our pill P at (126,126), our base
                              at (122,122) (full stock, so a full tank skips
                              refuel outright), our start pond at (120,126).
    y=128..130   DEEP SEA     a 3-row moat spanning the whole width.  There is
                              no river anywhere, so no boat can ever be built:
                              the shooter is PERMANENTLY unreachable and
                              attack_tank / kill_lgm price at INF instead of
                              pulling the bot off the pill.  Shells fly over
                              water, so the shooter can still hit the pill.
    y=131..144   x=108..144   the shooter's strip.  Its start pond is
                              (126,138); it drives north to y=132 and stops.

Distances that matter:
  * shooter standoff (126,132) -> pill (126,126) is 6 tiles = 1536 wu.  A tank
    shell reaches shellLifeTicks(GUNSIGHT_MAX/2 = 7) = 51 ticks x SHELL_SPEED
    32 = 1632 wu plus the SHELL_START_ADD offset, so 6 tiles is inside the
    reach with a tile to spare and 7+ would not be.
  * that is also inside PILL_FIRE_RANGE (8), so our pill shoots back once it is
    angered.  Deliberate -- it is what a real siege looks like -- and the
    shooter flees before it can be killed.
  * our start (117,126) is 9 tiles from the pill: inside DEFEND_ARRIVE_RADIUS
    (10) from the first tick, so the ARRIVED branch (watch / repair) is what is
    under test -- but OUTSIDE the builder's 5-tile dispatch range, so the bot
    still has to close before the LGM can go.  That gap is what lets the
    shooter's second burst land on a live repair goal and exercise the
    under-fire hold.  WEST of the pill, not north of it: with the bot,
    the pill and the shooter collinear, the bot's own shots at the shooter went
    straight through its pill and killed it (14 hits instead of the scripted 6).
  * our base sits 6 tiles from the pill ON PURPOSE.  The pill-reposition pool
    scores a lone pill far from every base as badly placed and bids
    capture_pill/reposition_shoot on it -- i.e. the bot shoots its OWN pill down
    to move it, which is not the experiment.  A base inside PILL_FIRE_RANGE
    gives the pill a job (PILL_REPOSITION_BASE_PROTECT_W) and settles it.

NO FOREST anywhere: seek_trees (40 - carry*6 = 34 at carry 0... 40 here) must
not compete with the repair bid (DEFEND_REPAIR_COST 40) for the win.
NO other pills: attack_pill / capture_pill never bid.

Start squares must be DEEP SEA (starts.c startsIsValidSquare) or the engine
spirals off to find one, so both spawns are one-tile ponds inside their field.
Both tanks come ashore in a few ticks.

Usage:
    python3 tests/generate_defend_repair_map.py [output_path]
    Default output: tests/defend_repair.map
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

GRASS = 7
DEEP_SEA = None          # background sentinel (unwritten cells read as deep sea)
MAP_SIZE = 256

# ── Geometry (the test runner imports these) ─────────────────────────────
FIELD = (108, 144, 108, 127)        # x0, x1, y0, y1 inclusive -- our side
MOAT_Y = (128, 130)                 # inclusive rows of deep sea
STRIP = (108, 144, 131, 144)        # the shooter's side

OUR_PILL = (126, 126)
OUR_BASE = (122, 122)
OUR_SPAWN = (117, 126)              # one-tile pond, 9 tiles W of the pill
FOE_SPAWN = (126, 138)              # one-tile pond on the far strip
FOE_STANDOFF = (126, 132)           # where the shooter brakes and opens fire
FOE_BASE = (126, 133)               # hostile base (owner set by the sidecar).
                                    # Not decoration: without an enemy asset on
                                    # the far side, the front line never reaches
                                    # our pill, PP.classify calls it a BACK pill,
                                    # and the reposition pool bids
                                    # capture_pill/reposition_shoot on it -- the
                                    # bot shoots its OWN pill down to move it,
                                    # which is not the experiment.  Only BACK
                                    # pills are eligible to be repositioned.

PILLS_MAX_HEALTH = 15


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    for (x0, x1, y0, y1) in (FIELD, STRIP):
        for yy in range(y0, y1 + 1):
            for xx in range(x0, x1 + 1):
                t[yy][xx] = GRASS
    # Dig the two start ponds back out to the deep-sea background.
    for (px, py) in (OUR_SPAWN, FOE_SPAWN):
        t[py][px] = DEEP_SEA
    return t


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else str(
        Path(__file__).parent / "defend_repair.map")
    terrain = make_map()

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    for name, sq in (("our", OUR_SPAWN), ("foe", FOE_SPAWN)):
        assert terrain[sq[1]][sq[0]] is DEEP_SEA, (
            f"{name} start must be deep sea (starts.c startsIsValidSquare)")
    assert terrain[OUR_PILL[1]][OUR_PILL[0]] is not DEEP_SEA
    assert terrain[FOE_STANDOFF[1]][FOE_STANDOFF[0]] is not DEEP_SEA
    assert terrain[FOE_BASE[1]][FOE_BASE[0]] is not DEEP_SEA
    # The shell has to reach: 6 tiles, and a tank shell dies at ~7.1.
    d = abs(FOE_STANDOFF[1] - OUR_PILL[1]) + abs(FOE_STANDOFF[0] - OUR_PILL[0])
    assert d <= 6, f"standoff is {d} tiles from the pill -- shells fall short"
    # ...and the moat really does separate the two sides.
    for yy in range(MOAT_Y[0], MOAT_Y[1] + 1):
        for xx in range(FIELD[0], FIELD[1] + 1):
            assert terrain[yy][xx] is DEEP_SEA, (xx, yy)

    # armour 15 = PILLS_MAX_HEALTH (alive); speed 50 = normal reload.
    # Owner 0 = the -bots tank; the sidecar re-asserts it anyway.
    pills = [(OUR_PILL[0], OUR_PILL[1], 0, PILLS_MAX_HEALTH, 50)]
    bases = [(OUR_BASE[0], OUR_BASE[1], 0, 90, 90, 90),
             (FOE_BASE[0], FOE_BASE[1], 0, 90, 90, 90)]   # re-owned by the sidecar
    starts = [(OUR_SPAWN[0], OUR_SPAWN[1], 8),     # 1: ours,  facing south
              (FOE_SPAWN[0], FOE_SPAWN[1], 0)]     # 2: shooter, facing north

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
    print(f"  our pill {OUR_PILL}, our base {OUR_BASE}, our start {OUR_SPAWN}")
    print(f"  moat rows y={MOAT_Y[0]}..{MOAT_Y[1]} (no river anywhere -> the "
          f"shooter is permanently unreachable)")
    print(f"  shooter start {FOE_SPAWN}, standoff {FOE_STANDOFF} "
          f"({d} tiles from the pill)")


if __name__ == '__main__':
    main()
