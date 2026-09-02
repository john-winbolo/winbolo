#!/usr/bin/env python3
"""
Generate the attack_base friendly-cover arena
(companion to tests/attack_base_cover_test.py).

Two things were wrong with pool 7 (attack_base) and this arena separates them.

  * DOUBLE CHARGE.  step_eval_queue already puts the armour-aware markup and
    the threat term INSIDE the per-candidate cost; finalize_pools then added
    both again to that same number.  Base #9 in field session
    20260902_000405_1_16v17 competed at 210 when its own candidate cost was
    130.  The arena does not need special geometry for this -- any pool-7 row
    proves it -- so the test just checks the printed arithmetic closes.
  * NO CREDIT FOR OUR OWN PILLS.  refresh_base_steal has a hostile coverage
    guard (a base reachable only down a pillbox's line of fire is not free);
    there was no mirror of it.  A base sitting under two of OUR pillboxes cost
    exactly as much to attack as one in the middle of their half.

Shape (file coordinates; the recenter is a no-op, see main()):

    x=108..144, y=108..144   flat grass.  Nothing to hide behind, no forest, no
                             water except the two one-tile start ponds -- so
                             the two candidate bases differ ONLY in whether our
                             pills cover them.
    (126,126)                our start pond, exactly 8 tiles from each base.
    (118,126)  base A        HOSTILE (the sidecar re-owns it).  COVERED: our
                             pills sit 4 and 6 tiles away, both inside
                             PILL_FIRE_RANGE (8).
    (134,126)  base B        HOSTILE.  The same 8 tiles from the tank and the
                             same untouched 90 armour, but 17 and 20 tiles from
                             the nearest of our pills -- and the straight
                             approach to it runs east, away from them.
    (114,126), (118,120)     our pills.
    (126,112)                our base, full stock.
    (110,142)                the enemy's start pond, far from everything: it
                             owns the two bases and otherwise sits still.

So the only term that can separate A from B is
ATTACK_BASE_FRIENDLY_COVER_MULT^2 = 0.64 on the (markup + threat) half of the
candidate cost: 80 -> 51.2, a 28.8 saving, which is worth about 14 tiles of
grass path.  That margin is deliberately much wider than the drift in the
tank's own position over a run.

Usage:
    python3 tests/generate_attack_base_cover_map.py [output_path]
    Default output: tests/attack_base_cover.map
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256

FIELD = (108, 144, 108, 144)

OUR_SPAWN = (126, 126)
FOE_SPAWN = (110, 142)
BASE_COVERED = (118, 126)      # A -- inside our pills' fire range
BASE_BARE = (134, 126)         # B -- same distance from the tank, uncovered
OUR_PILLS = [(114, 126), (118, 120)]
OUR_BASE = (126, 112)

PILL_FIRE_RANGE = 8            # constants.lua M.PILL_FIRE_RANGE
COVER_MULT = 0.8               # M.ATTACK_BASE_FRIENDLY_COVER_MULT
PILLS_MAX_HEALTH = 15


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def n_covering(base):
    return sum(1 for p in OUR_PILLS if edist(p, base) <= PILL_FIRE_RANGE)


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y0, y1 = FIELD
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            t[yy][xx] = GRASS
    for (px, py) in (OUR_SPAWN, FOE_SPAWN):
        t[py][px] = DEEP_SEA
    return t


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else str(
        Path(__file__).parent / "attack_base_cover.map")
    terrain = make_map()

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    for sq in (OUR_SPAWN, FOE_SPAWN):
        assert terrain[sq[1]][sq[0]] is DEEP_SEA, sq

    # The two bases must be the SAME distance from the start and differ only
    # in coverage, or the test is measuring geometry instead of the discount.
    assert edist(OUR_SPAWN, BASE_COVERED) == edist(OUR_SPAWN, BASE_BARE), (
        edist(OUR_SPAWN, BASE_COVERED), edist(OUR_SPAWN, BASE_BARE))
    assert n_covering(BASE_COVERED) == 2, n_covering(BASE_COVERED)
    assert n_covering(BASE_BARE) == 0, n_covering(BASE_BARE)
    # ...and no pill may reach the straight approach the tank would drive to B
    # (base_friendly_cover tests the approach tiles as well as the base tile).
    for i in range(0, 9):
        tile = (OUR_SPAWN[0] + i, OUR_SPAWN[1])
        for p in OUR_PILLS:
            assert edist(p, tile) > PILL_FIRE_RANGE, (p, tile, edist(p, tile))

    pills = [(x, y, 0, PILLS_MAX_HEALTH, 50) for (x, y) in OUR_PILLS]
    bases = [(OUR_BASE[0], OUR_BASE[1], 0, 90, 90, 90),
             (BASE_COVERED[0], BASE_COVERED[1], 0, 90, 90, 90),
             (BASE_BARE[0], BASE_BARE[1], 0, 90, 90, 90)]
    starts = [(OUR_SPAWN[0], OUR_SPAWN[1], 0),
              (FOE_SPAWN[0], FOE_SPAWN[1], 0)]

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
    print(f"  start {OUR_SPAWN}; covered base A {BASE_COVERED} "
          f"({n_covering(BASE_COVERED)} of our pills in range), "
          f"bare base B {BASE_BARE} ({n_covering(BASE_BARE)})")
    print(f"  our pills {OUR_PILLS}, our base {OUR_BASE}, foe start {FOE_SPAWN}")
    print(f"  expected engage multiplier on A: {COVER_MULT}^2 = "
          f"{COVER_MULT ** 2:.2f}")


if __name__ == '__main__':
    main()
