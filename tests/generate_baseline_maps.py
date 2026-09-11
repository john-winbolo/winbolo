#!/usr/bin/env python3
"""
Generate the purpose-built maps under tests/baseline/maps/.

One function per map returns (terrain, pills, bases, starts); main writes
each to tests/baseline/maps/<name>.map with the BMAP writer the other
generators share. Everard Island, Forest Rig and Slugfest IV are real maps
and are not produced here.

Usage:
    python3 tests/generate_baseline_maps.py            # write every map
    python3 tests/generate_baseline_maps.py NAME ...   # only the named maps

Coordinates: +x is east, +y is south. Deep sea is the implicit background,
so only land and water features are painted. Pills are (x, y, owner,
armour, speed); bases are (x, y, owner, armour, shells, mines); starts are
(x, y, dir) in the file's own direction numbering, which the loader
mirrors: file dir 0 faces east in the game, 4 north, 8 west, 12 south. A
start square must be deep sea, or the spawn is moved to the nearest square
that is.

The loader also recentres a map (mapCenter in src/bolo/bolo_map.c): it
takes the bounding box of every non-deep-sea square plus the pills, bases
and starts and shifts everything so the box's midpoint, (left+right)/2 and
(top+bottom)/2 in integer division, lands on 126. Every map here is laid
out so that midpoint is already (126, 126); check_centred asserts it, so a
layout change that would move the map fails here instead of producing a
map whose squares are not where the brains expect them.
"""

import sys
from pathlib import Path

from generate_test_map import (MAP_SIZE, DEEP_SEA, RIVER, SWAMP, ROAD, FOREST,
                               GRASS, BOAT)
# The writer that copes with deep sea between land segments on one row.
from generate_boat_diagonal_map import write_bmap

NEUTRAL = 0xFF
MINE_OFFSET = 8              # a mined square is the terrain nibble + 8
MINE_SWAMP = SWAMP + MINE_OFFSET
MINE_FOREST = FOREST + MINE_OFFSET

FILE_DIR_EAST = 0            # start dir as written in the file; see the docstring

MAPS_DIR = Path(__file__).parent / "baseline" / "maps"


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def check_centred(name, terrain, pills, bases, starts):
    """Fail unless the loader's recentring would leave this map in place."""
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    # Features widen the box on the left and top only, as mapCenter does.
    xs += [f[0] for f in pills + bases + starts]
    ys += [f[1] for f in pills + bases + starts]
    add_x = 126 - (min(xs) + max(xs)) // 2
    add_y = 126 - (min(ys) + max(ys)) // 2
    if add_x != 0 or add_y != 0:
        sys.exit(f"{name}: the loader would shift this map by ({add_x}, {add_y}); "
                 f"move the layout so its bounding box is centred on (126, 126)")


def road_spit_minefield():
    """A single road running east through deep sea.

    Row 124 carries the whole spit: the one start at (116, 124) in deep sea
    facing east, three squares of open water, then road on squares
    120..137. A neutral pillbox sits at (131, 121), three squares north of
    road square 131 and inside its 8-square reach from road squares
    124..138; it is out of reach of the start and of road squares 120..123.
    Each shell that hits a tank shoves it about half a square away from the
    pill, so a grass pad six rows deep (125..130) lies south of road
    squares 128..134 to keep a parked tank on land while it is shot; the
    pill is close so the tank is still within reach after the shoves.

    South of road squares 120..124 lies a mined swamp patch three rows deep
    (125..127). Its top-row square (122, 125) is plain swamp, left for a
    mine laid by hand; a tank entering the patch there, or on either
    neighbour, sets off the laid mine and the chain runs through the rest.

    Four bases stand to the south-east, one of them owned by player 0 and
    three neutral, so the tournament shell allowance is a fraction of full;
    a two-by-two forest beside them has one mined square, so the map's
    forest and mine counts start above zero. Everything else is deep sea,
    the square north of every road square included, and the bounding box
    of it all (116..137 by 121..131) is centred on (126, 126).
    """
    t = blank()
    y = 124
    for x in range(120, 138):
        t[y][x] = ROAD

    for yy in range(125, 128):
        for x in range(120, 125):
            t[yy][x] = MINE_SWAMP
    t[125][122] = SWAMP

    for yy in range(125, 131):
        for x in range(128, 135):
            t[yy][x] = GRASS

    t[121][131] = GRASS
    pills = [(131, 121, NEUTRAL, 15, 100)]

    base_cells = [(136, 129), (137, 129), (136, 131), (137, 131)]
    for (x, yy) in base_cells:
        t[yy][x] = GRASS
    bases = [
        (136, 129, 0,       90, 90, 90),
        (137, 129, NEUTRAL, 90, 90, 90),
        (136, 131, NEUTRAL, 90, 90, 90),
        (137, 131, NEUTRAL, 90, 90, 90),
    ]

    for yy in (130, 131):
        for x in (134, 135):
            t[yy][x] = FOREST
    t[131][135] = MINE_FOREST

    starts = [(116, 124, FILE_DIR_EAST)]
    return t, pills, bases, starts


def boat_bank():
    """Two ways out of a boat from one start: a road ford and a grass bank.

    The start at (120, 130) is deep sea, so the tank spawns afloat facing
    east, and two channels lead away from it.

    East along row 130 runs the ford lane: river on squares 121..124, then
    road on 125..127, a parked boat on 128 and road again on 129..133. A
    boat driven east leaves at road square 125 whatever its speed, because
    road is a hard surface; it drops the boat it was in back on river
    square 124. Driving on, the tank picks the parked boat up at square
    128 and is put ashore again at road square 129, and the road out to
    133 leaves room to stop from full speed.

    North along column 120 runs the bank lane: river on rows 126..129 and
    a grass headland on rows 124..125, three squares wide so a heading a
    degree off north still lands on it. Grass is soft, so a boat that
    reaches row 126 below BOAT_FAST_EXIT_SPEED is held a quarter square
    short of the bank and never lands, while one at full speed drives
    straight out onto the grass. Past the headland a second parked boat
    sits on (120, 123) with river behind it on (120, 122), so a tank that
    did land can take to the water again from soft ground.

    Nothing else is on the map: no pills, no bases, and deep sea on every
    side of both channels, so the only land a boat can meet is the ford's
    road and the headland's grass. The bounding box of it all (119..133 by
    122..130) is centred on (126, 126).
    """
    t = blank()

    for y in range(126, 130):
        t[y][120] = RIVER
    for y in (124, 125):
        for x in range(119, 122):
            t[y][x] = GRASS
    t[123][120] = BOAT
    t[122][120] = RIVER

    for x in range(121, 125):
        t[130][x] = RIVER
    for x in range(125, 128):
        t[130][x] = ROAD
    t[130][128] = BOAT
    for x in range(129, 134):
        t[130][x] = ROAD

    starts = [(120, 130, FILE_DIR_EAST)]
    return t, [], [], starts


MAPS = {
    "Road Spit Minefield": road_spit_minefield,
    "Boat Bank": boat_bank,
}


def main():
    names = sys.argv[1:] or list(MAPS)
    for name in names:
        if name not in MAPS:
            sys.exit(f"unknown map '{name}'; known: {', '.join(MAPS)}")
        terrain, pills, bases, starts = MAPS[name]()
        check_centred(name, terrain, pills, bases, starts)
        path = MAPS_DIR / f"{name}.map"
        write_bmap(path, terrain, pills, bases, starts)
        print(f"Wrote {path} ({path.stat().st_size} bytes): "
              f"{len(pills)} pills, {len(bases)} bases, {len(starts)} starts")


if __name__ == '__main__':
    main()
