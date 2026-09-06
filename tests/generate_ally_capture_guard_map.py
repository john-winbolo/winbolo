#!/usr/bin/env python3
"""
Generate the ally-capture-guard arena (companion to
tests/ally_capture_guard_test.py).

ONE piece of terrain for all four variants, because three of them are controls
for the first and a control that changes the ground proves nothing.  A, B, C and
D run BYTE-IDENTICAL map files (four names only because the scenario sidecar is
found by map name) and differ solely in the tokens their bots are given and in
what the sidecar does to the ally.

THE QUESTION
------------
Our LGM must not rebuild a dead friendly pill that a TEAMMATE is driving over to
scoop.  Four trees turn the corpse into a live friendly pill, which is
undriveable -- so the pickup is impossible, the kill that made the corpse is
wasted, and the wood bought the team nothing it did not already have.

THE ARENA
---------
A grass DIAMOND of radius 8 around (126,126) and exactly one pill on it:

  * the CORPSE at (126,126) -- 0 armour, ours, the dead centre of the diamond.
    The only pill on the map, so an ally with a working brain has exactly one
    sensible goal: go and pick it up.
  * our SPAWN pond at (133,126) -- seven tiles east, as far out as a pond can go
    without eating the diamond's east tip and shifting the whole map.  The
    distance is deliberate; see "keeping our own tank off the corpse" below.
  * our BASE at (131,126), full stock, so nobody bids refuel.
  * the ALLY's SPAWN pond at (126,120), six tiles north of the corpse (a
    different approach line from ours, so the two never queue behind each
    other), and its base at (124,120).

WHY A DIAMOND AND NOT A SQUARE
------------------------------
BUILDER_POOL_LEASH is compared against U.mdist, which is MANHATTAN
(util.lua:126).  "Everywhere on this map is inside the leash" therefore means
every land tile is within LEASH of the corpse in |dx|+|dy| -- a diamond of
radius LEASH centred on it.  A 17x17 SQUARE looks right and is not: its corners
are 16 away in Manhattan, and a tank that wanders into one takes the corpse out
of the pool's reach.  The row then reads `out_of_leash` and every "the man did
not go" assertion in the test file becomes true for the wrong reason.  (Found
the hard way: variant D released its block correctly and then could not
dispatch, because the tank was sitting 14 tiles away in a corner.)

KEEPING OUR OWN TANK OFF THE CORPSE
-----------------------------------
A dead pill is picked up by DRIVING OVER IT, and there is no terrain that stops
a tank without also stopping the man -- the one tile type that blocks the LGM
and not the tank, a live pillbox, blocks him as well (brain_pathfinder.c's
lgm_man_speed is 0 for pillbox, and cpf_set_lgm_blocked stamps live pills to
match the engine).  So our own tank has to be kept off the corpse by BEHAVIOUR,
in two layers:

  1. `cfg=CAPTURE_PILL_BASE_COST=1e30` (in the test's -bot-init tokens) prices
     capture_pill past the 1e29 "unaffordable" line for OUR bot alone, so it
     never TARGETS the corpse.  A merely large value is not enough: the goal
     competition picks the CHEAPEST candidate, so a 999999 capture_pill still
     wins an otherwise empty pool and the tank drives straight over the corpse
     (measured: it did, at brain t=181).
  2. With no goal it can afford, the bot falls through to `explore`, which
     sweeps NEAREST-UNVISITED-FIRST (exploration.lua best_frontier) and will
     eventually target every land tile -- the corpse's included.  Nothing stops
     that, so the arena BUYS TIME instead: our spawn sits at the far end, seven
     tiles from the corpse, with dozens of nearer tiles to sweep first.  The test
     asserts the corpse survived long enough (and says exactly this if it did
     not), and the sidecars' timings are set well inside the measured margin.

MAP FACTS THAT BITE (the same list as generate_builder_pool_map.py,
rediscovered the same way)
  * mapRead recenters the terrain bounding-box midpoint to (126,126), so this
    file asserts its own midpoint is already (126,126) and no shift happens.
  * a start square must be DEEP SEA (starts.c startsIsValidSquare) -- hence the
    one-tile ponds.
  * mapRead puts ROAD under every map-file pill.
  * -gametype open starts a tank with TANK_FULL_TREES (40), so the rebuild is
    never wood-gated.
  * a lone ALIVE pill far from every base gets scored as badly placed and the
    bot shoots its OWN pill down to move it.  This map has no alive pill at all,
    and the test additionally turns reposition off for our bot.

Usage:
    python3 tests/generate_ally_capture_guard_map.py [output_path]
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

LEASH = 8                        # C.BUILDER_POOL_LEASH (MANHATTAN)
ALLY_CAPTURE_TTL = 175           # C.BUILDER_POOL_ALLY_CAPTURE_TTL (brain ticks)

CORPSE = (126, 126)              # 0 armour: the pill the whole test is about
FIELD_R = LEASH                  # diamond radius, so every tile is in leash
FIELD = (CORPSE[0] - FIELD_R, CORPSE[0] + FIELD_R,
         CORPSE[1] - FIELD_R, CORPSE[1] + FIELD_R)   # the diamond's bbox

OUR_SPAWN = (133, 126)           # 7 tiles east: as far from the corpse as a
                                 # pond can sit without eating the diamond's
                                 # east TIP -- ponding (134,126) would shrink
                                 # the terrain bbox and mapRead would then
                                 # shift the whole map by one tile
OUR_BASE = (131, 126)            # 5 from the corpse, 2 from our spawn
ALLY_SPAWN = (126, 120)          # 6 tiles north of the corpse
ALLY_BASE = (124, 120)           # the ally's own stock

# Pill number as the scenario API sees it (1-based, file order).
PILL_CORPSE = 1


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def fill_diamond(t, centre, r, tile=GRASS):
    cx, cy = centre
    for yy in range(cy - r, cy + r + 1):
        for xx in range(cx - r, cx + r + 1):
            if abs(xx - cx) + abs(yy - cy) <= r:
                t[yy][xx] = tile


def pond(t, sq):
    t[sq[1]][sq[0]] = DEEP_SEA


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def build():
    t = blank()
    fill_diamond(t, CORPSE, FIELD_R)
    pond(t, OUR_SPAWN)
    pond(t, ALLY_SPAWN)
    # (x, y, owner, armour, speed)
    pills = [(CORPSE[0], CORPSE[1], 0, 0, 50)]
    bases = [(OUR_BASE[0], OUR_BASE[1], 0, 90, 90, 90),
             (ALLY_BASE[0], ALLY_BASE[1], 0, 90, 90, 90)]
    starts = [(OUR_SPAWN[0], OUR_SPAWN[1], 12),     # ours, facing west
              (ALLY_SPAWN[0], ALLY_SPAWN[1], 8)]    # the ally, facing south
    return t, pills, bases, starts


def write(output):
    terrain, pills, bases, starts = build()

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    mid = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    assert mid == (126, 126), (
        f"terrain midpoint is {mid}, not (126,126) -- mapRead would shift "
        f"every coordinate in this file")
    for (sx, sy, _d) in starts:
        assert terrain[sy][sx] is DEEP_SEA, (
            f"start ({sx},{sy}) must be deep sea (starts.c startsIsValidSquare)")
    for (px, py, _o, _a, _s) in pills:
        assert terrain[py][px] is not DEEP_SEA, f"pill ({px},{py}) is in the sea"
    for (bx, by, *_r) in bases:
        assert terrain[by][bx] is not DEEP_SEA, f"base ({bx},{by}) is in the sea"
    # The leash invariant: no land tile is further than BUILDER_POOL_LEASH
    # (MANHATTAN -- see the module docstring) from the corpse, so "the man never
    # went" can never be explained by the corpse falling out of the pool's reach.
    worst = max(abs(x - CORPSE[0]) + abs(y - CORPSE[1])
                for y in range(MAP_SIZE) for x in range(MAP_SIZE)
                if terrain[y][x] is not DEEP_SEA)
    assert worst <= LEASH, (
        f"the field reaches {worst} tiles (manhattan) from the corpse, more "
        f"than BUILDER_POOL_LEASH ({LEASH}) -- shrink FIELD_R")
    # And the time-buying invariant: our spawn is the tile FURTHEST from the
    # corpse that the map has, so the explore sweep reaches it as late as the
    # geometry allows.
    assert mdist(OUR_SPAWN, CORPSE) >= LEASH - 1, (
        f"our spawn is {mdist(OUR_SPAWN, CORPSE)} from the corpse, nearly "
        f"{LEASH - mdist(OUR_SPAWN, CORPSE)} tiles closer than the diamond "
        f"allows -- that shortens the head start the arena depends on")

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
          f"[{len(pills)} pill, {len(bases)} bases, {len(starts)} starts; "
          f"worst land tile {worst} (manhattan) <= leash {LEASH}]")


VARIANTS = ("A", "B", "C", "D")


def main():
    """Writes ally_capture_guard_{A,B,C,D}.map -- BYTE-IDENTICAL on purpose.

    The scenario sidecar is found by name ("<map>.scenario.lua" next to the
    map, scenario.c), so four variants that need four different sidecars need
    four map file names even when the terrain is the same.  Keeping the bytes
    identical is what makes B, C and D real controls for A: nothing about the
    ground can differ between them.
    """
    here = Path(__file__).parent
    args = sys.argv[1:]
    if args:
        write(args[0])
        return
    for v in VARIANTS:
        write(str(here / f"ally_capture_guard_{v}.map"))


if __name__ == '__main__':
    main()
