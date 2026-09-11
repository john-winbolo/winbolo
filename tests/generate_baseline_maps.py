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
                               GRASS, BOAT, HALFBUILDING)
# The writer that copes with deep sea between land segments on one row.
from generate_boat_diagonal_map import write_bmap

NEUTRAL = 0xFF
MINE_OFFSET = 8              # a mined square is the terrain nibble + 8
MINE_SWAMP = SWAMP + MINE_OFFSET
MINE_FOREST = FOREST + MINE_OFFSET
MINE_ROAD = ROAD + MINE_OFFSET

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


def builder_yard():
    """One road with a square of every kind the builder can work on beside it.

    The start at (116, 126) is deep sea, so the tank spawns afloat facing
    east; three squares of open water lead to road square 120, and the road
    runs east from there to 136. A boat is only left behind when the square
    the tank came from is river, so a tank that crosses deep sea straight
    onto the road leaves nothing on the water and the map it works on is
    the map it was given.

    A one-square grass apron runs the length of the road on both sides, and
    the work squares sit in it within a square or two of where a tank that
    brakes on reaching the road comes to rest:

        (121, 125) grass         road laid on soft ground
        (122, 125) grass         a mine laid by the man
        (123, 125) forest        harvested for trees
        (124, 125) forest        harvested again
        (121, 127) grass         a building raised
        (122, 127) swamp         road laid over swamp
        (124, 127) half building repaired to a whole one

    Two pillboxes stand on the map. The one at (126, 125) belongs to player
    0 with 8 of its 15 armour, so it never fires on the tank that owns it
    and the man can walk to it and patch it up; the repair takes a full
    load of trees whatever the damage, so the two it does not need ride
    home again. The other sits on road square 128 with no armour at all,
    which is what makes it capturable: a tank that drives over it carries
    it away, and the builder can then put it down on a square of its own.
    Its zero armour is also why it never shoots.

    The bounding box of it all (116..136 by 125..127) is centred on
    (126, 126).
    """
    t = blank()

    for x in range(120, 137):
        t[125][x] = GRASS
        t[126][x] = ROAD
        t[127][x] = GRASS

    t[125][123] = FOREST
    t[125][124] = FOREST
    t[127][122] = SWAMP
    t[127][124] = HALFBUILDING

    pills = [
        (126, 125, 0,       8,  100),
        (124, 126, NEUTRAL, 0,  100),
    ]

    starts = [(116, 126, FILE_DIR_EAST)]
    return t, pills, [], starts


def base_yard():
    """One road with three bases on it, each set up for a different job.

    The start at (116, 126) is deep sea, so the tank spawns afloat facing
    east; three squares of open water lead to road square 120 and the road
    runs east from there to 137. Everything stands on that one row:

        (124, 126) base    neutral, full, driven onto and taken
        (127, 126) mined road          ten armour off whatever passes
        (130, 126) base    neutral, full armour, three shells, full mines
        (134, 126) base    player 1's, twenty armour, full shells and mines

    The neutral bases are the ones a tank can drive onto as it finds them:
    a neutral base is never solid and taking one costs it nothing, so its
    stocks carry over to the new owner whole. Player 1's base is the
    opposite on both counts. Nobody is ever in that slot, so the base
    answers to no one in the game, which is what makes it an enemy base:
    it is solid until a tank shells it down to MIN_ARMOUR_CAPTURE, and
    taking it from a live owner empties it.

    The mined square is what makes the armour refuel visible. A tank starts
    on full armour under every game type, so a base with armour to give has
    nothing to give it until something has taken some off, and ten off one
    mine is two gives worth. It sits east of the first base, so a run that
    stops on that base never reaches it.

    The middle base's three shells are what brings its mines into reach. A
    base hands over armour first, then shells, then mines, and only moves on
    when the tank is full or the base is out; a base that runs dry of shells
    after three of them is handing over mines a dozen ticks later, rather
    than after the forty gives a full base would owe.

    The bounding box of it all (116..137 by 126..126) is centred on
    (126, 126).
    """
    t = blank()
    for x in range(120, 138):
        t[126][x] = ROAD
    t[126][127] = MINE_ROAD

    bases = [
        (124, 126, NEUTRAL, 90, 90, 90),
        (130, 126, NEUTRAL, 90,  3, 90),
        (134, 126, 1,       20, 90, 90),
    ]

    starts = [(116, 126, FILE_DIR_EAST)]
    return t, [], bases, starts


def watch_road():
    """One road with a pillbox lying in it and a base at the end, and two
    starts: one to drive it and one to watch from.

    The start at (116, 124) is deep sea, so the first tank spawns afloat
    facing east; three squares of open water lead to road square 120 and the
    road runs east from there to 137. Two things stand on it:

        (128, 124) pillbox neutral, no armour, picked up by driving over it
        (133, 124) base    neutral, full, taken by stopping on it

    Neither has to be shot first. A pillbox on zero armour never fires and is
    carried off by the tank that drives over it; a neutral base is never solid
    and hands itself over to the tank that arrives. So the whole run is one
    tank holding the throttle and then the brake, with nothing in it that
    turns on when a shell lands.

    The second start at (131, 128) is deep sea four squares south of the road,
    between the pillbox and the base and within sight of both. The tank that
    spawns there never moves: it is the second client's eyes, and what it
    reports about the two captures is the point of the map.

    The bounding box of it all (116..137 by 124..128, both starts included) is
    centred on (126, 126).
    """
    t = blank()
    for x in range(120, 138):
        t[124][x] = ROAD

    pills = [(128, 124, NEUTRAL, 0, 100)]
    bases = [(133, 124, NEUTRAL, 90, 90, 90)]
    starts = [(116, 124, FILE_DIR_EAST), (131, 128, FILE_DIR_EAST)]
    return t, pills, bases, starts


def grass_flat():
    """A plain of grass with nothing on it but the tank watching it.

    Tree growth is the only thing that ever happens here. treeGrowUpdate
    samples one random map square of the whole 256 by 256 per tank per world
    update, and skips deep sea, mines, pills and bases, so the squares it can
    do anything with are the grass and nothing else. A plain 49 by 45 is 2205
    of them, which is a sample in every thirty or so.

    Every square of the plain is grass, so treeGrowCalcScore returns the same
    225 for every square away from the shore — nine grass squares at
    TREE_GROW_GRASS each — and 200 or less on the shore, where the deep sea
    in the ring scores nothing. A candidate only replaces the standing one
    when it scores strictly higher, so once a square away from the shore has
    been sampled nothing can displace it and the TREEGROW_TIME countdown runs
    clean to the end. That is what keeps the run short: the countdown starts
    again at every improvement, and on a plain of one terrain the improvements
    are over within the first few dozen ticks.

    The start at (126, 102) is deep sea four squares north of the plain, so
    the tank spawns afloat and idles in open water, off the grass and nowhere
    near the square that grows. Nothing else is on the map: no pills, no
    bases and no trees, so the run starts on a forest count of zero and the
    growth takes it to one.

    The bounding box of it all (102..150 by 102..150, the start included) is
    centred on (126, 126).
    """
    t = blank()
    for y in range(106, 151):
        for x in range(102, 151):
            t[y][x] = GRASS

    starts = [(126, 102, FILE_DIR_EAST)]
    return t, [], [], starts


def pill_yard():
    """One road with a pillbox beside it and another standing in it.

    The start at (116, 126) is deep sea, so the tank spawns afloat facing
    east; three squares of open water lead to road square 120 and the road
    runs east from there to 137, with a grass apron either side of it and a
    three-square grass pad north and south of the apron around column 125.

    The pillbox at (125, 124) belongs to player 0, two squares north of
    road square 125. A pill never fires on the tank that owns it, so a tank
    parked on 125 can shell it for as long as it likes and read the whole
    of what being shot at does to a pill without anything shooting back.

    The pillbox at (134, 126) is neutral and on the road, on one armour, so
    one shell flattens it and the tank that drives over it carries it off.
    Nine squares lie between it and a tank parked on 125, which is past
    PILLBOX_RANGE, so it never fires on a run that stays at the other end;
    three road squares beyond it leave room to stop from full speed.

    The bounding box of it all (116..137 by 124..128) is centred on
    (126, 126).
    """
    t = blank()
    for x in range(120, 138):
        t[125][x] = GRASS
        t[126][x] = ROAD
        t[127][x] = GRASS
    for x in range(124, 127):
        t[124][x] = GRASS
        t[128][x] = GRASS

    pills = [
        (125, 124, 0,       6, 100),
        (134, 126, NEUTRAL, 1, 100),
    ]

    starts = [(116, 126, FILE_DIR_EAST)]
    return t, pills, [], starts


MAPS = {
    "Road Spit Minefield": road_spit_minefield,
    "Boat Bank": boat_bank,
    "Builder Yard": builder_yard,
    "Base Yard": base_yard,
    "Pill Yard": pill_yard,
    "Grass Flat": grass_flat,
    "Watch Road": watch_road,
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
