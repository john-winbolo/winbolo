#!/usr/bin/env python3
"""
Generate the harvest FOLLOW-THROUGH arena
(companion to tests/harvest_follow_through_test.py).

The incident (20260902_030233_1_16v17_20min, bot2):

    t=36981  HARVEST_SET tile=(109,123) score=311.3 origin=strategic
             -- the chosen pill spot is FOREST, so the engine turns the pill
             request into a tree harvest and the builder walks out to chop it.
    t=37011  the placement row VANISHES from the pool (eval_place_pill_strategic
             needs the man IN the tank to be "actionable"), refuel_at_base 22
             tiles away wins instead, and the tank drives off.
    t=37445  the builder comes back with the wood, the tile is re-scored from
             where the tank NOW stands, 311.3 -> 181.7, and the whole harvest is
             thrown away (HARVEST_DROP worse_than_margin).

So the arena has to make the bot pick a FOREST tile for a pill and then leave
the tank free to wander while the man is out.

Shape (file coordinates; the terrain bbox is centred so mapRead's recenter is a
no-op -- see main()):

    y=106..116, x=112..140   the enemy islet, flat grass.  Holds the HOSTILE
                             base and the idle opponent's start.
    y=117..121               DEEP SEA, full width.  Nothing can cross it, so the
                             opponent never interferes: no attack_tank, no
                             capture_base, no attack_base -- every pool that
                             would out-bid the follow-through is unreachable.
                             The influence STAMP does not care about terrain, so
                             the hostile base still projects its half of the
                             front line across the water.
    y=122..146, x=106..146   our arena, ALL FOREST except the pocket below.
                             Every placeable tile the scan can reach is forest,
                             so whichever tile wins the placement scan the
                             engine turns into a harvest trip -- the test does
                             not have to predict WHICH tile, it reads it off
                             HARVEST_SET.
    x=123..129, y=129..135   grass pocket: our base, our start, and the two
                             pills the scenario sidecar loads into the tank.

The front line -- and therefore where a pill is worth placing -- is set by the
two base stamps (BASE_INFLUENCE_RADIUS 12, strength 100, linear falloff over
radius+1).  With the bases 21 tiles apart the two discs overlap by one row:
y=123 nets positive, y=122 nets negative, so those two rows are the front band
and STRATEGIC_PLACE_STRICT_NEED (the portfolio's most-needed role with no pills
on the board is "front") pins placement to them.  They are 8-9 tiles from the
start -- the "~10 tiles away" the test wants -- and they are forest.

Usage:
    python3 tests/generate_harvest_follow_through_map.py [output_path]
    Default output: tests/harvest_follow_through.map
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

GRASS = 7
FOREST = 5
DEEP_SEA = None
MAP_SIZE = 256

ISLET = (112, 140, 106, 116)        # x0,x1,y0,y1  enemy islet (grass)
CHANNEL = (117, 121)                # y0,y1        deep sea, full width
ARENA = (106, 146, 122, 146)        # x0,x1,y0,y1  our half (forest)
POCKET = (123, 129, 129, 135)       # x0,x1,y0,y1  grass around base/start

OUR_BASE = (126, 133)
OUR_SPAWN = (126, 131)
FOE_BASE = (126, 112)
FOE_SPAWN = (126, 109)
# Two pills parked in the pocket; the sidecar loads both into our tank at t=2.
OUR_PILLS = [(124, 133), (128, 133)]

# constants.lua
BASE_INFLUENCE_RADIUS = 12
BASE_INFLUENCE_STRENGTH = 100
PILLS_MAX_HEALTH = 15


def stamp(cx, cy, x, y, radius, strength):
    """One brainPathfinderStampInfluence contribution at (x,y)."""
    d2 = (x - cx) ** 2 + (y - cy) ** 2
    if d2 > radius * radius:
        return 0
    return int(strength * (1.0 - (d2 ** 0.5) / (radius + 1)))


def influence(x, y):
    return (stamp(OUR_BASE[0], OUR_BASE[1], x, y,
                  BASE_INFLUENCE_RADIUS, BASE_INFLUENCE_STRENGTH)
            + stamp(FOE_BASE[0], FOE_BASE[1], x, y,
                    BASE_INFLUENCE_RADIUS, -BASE_INFLUENCE_STRENGTH))


def front_cells():
    """Tiles with influence whose N/S/E/W neighbour has the opposite sign --
    pill_portfolio.on_front_line, which is what classifies a candidate tile as
    'front' (FRONT_NEAR_RADIUS_PLACE is 0, so a placing tile must sit ON it)."""
    out = []
    x0, x1, y0, y1 = ARENA
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            v = influence(x, y)
            if v == 0:
                continue
            ns = [influence(x, y - 1), influence(x, y + 1),
                  influence(x - 1, y), influence(x + 1, y)]
            if (v > 0 and any(n < 0 for n in ns)) or \
               (v < 0 and any(n > 0 for n in ns)):
                out.append((x, y))
    return out


def in_box(box, x, y):
    x0, x1, y0, y1 = box
    return x0 <= x <= x1 and y0 <= y <= y1


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y0, y1 = ISLET
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            t[y][x] = GRASS
    x0, x1, y0, y1 = ARENA
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            t[y][x] = FOREST
    x0, x1, y0, y1 = POCKET
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            t[y][x] = GRASS
    # startsScatterFind spiral-searches for a valid DEEP SEA square: every tank
    # starts afloat.  So each start is a ONE-TILE pond ringed by land -- the
    # tank boats ashore on its first move and, crucially, cannot sail anywhere.
    # Without the ring both tanks would spawn in the channel instead and the
    # deep-sea separation that makes the opponent inert would be gone.
    for (px, py) in (OUR_SPAWN, FOE_SPAWN):
        t[py][px] = DEEP_SEA
    return t


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else str(
        Path(__file__).parent / "harvest_follow_through.map")
    terrain = make_map()

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    # mapRead shifts the terrain bbox midpoint to (126,126); centre it here so
    # file coordinates and in-game coordinates are the same thing.
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    # The channel really is a full-width gap.
    for y in range(CHANNEL[0], CHANNEL[1] + 1):
        assert all(terrain[y][x] is DEEP_SEA for x in range(MAP_SIZE)), y
    # Both start ponds are single tiles ringed by land: afloat at t=0, ashore at
    # t=1, and no water route out of either half.
    for (px, py) in (OUR_SPAWN, FOE_SPAWN):
        assert terrain[py][px] is DEEP_SEA, (px, py)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx or dy:
                    assert terrain[py + dy][px + dx] is not DEEP_SEA, \
                        (px + dx, py + dy)

    fronts = front_cells()
    assert fronts, "no front line in our half -- the two base discs do not overlap"
    forest_fronts = [c for c in fronts if terrain[c[1]][c[0]] == FOREST]
    assert len(forest_fronts) == len(fronts), \
        f"{len(fronts) - len(forest_fronts)} front tile(s) are not forest: " \
        f"{[c for c in fronts if terrain[c[1]][c[0]] != FOREST][:5]}"
    d = [abs(c[0] - OUR_SPAWN[0]) + abs(c[1] - OUR_SPAWN[1]) for c in fronts]
    assert 5 <= min(d) <= 14, f"front band {min(d)}..{max(d)} tiles from spawn"

    pills = [(x, y, 0, PILLS_MAX_HEALTH, 50) for (x, y) in OUR_PILLS]
    bases = [(OUR_BASE[0], OUR_BASE[1], 0, 90, 90, 90),
             (FOE_BASE[0], FOE_BASE[1], 0, 90, 90, 90)]
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
    print(f"  our base {OUR_BASE}, start {OUR_SPAWN}, pills {OUR_PILLS}")
    print(f"  hostile base {FOE_BASE} across the deep-sea channel "
          f"y={CHANNEL[0]}..{CHANNEL[1]}")
    print(f"  front band ({len(fronts)} tiles, all forest): "
          f"{min(c[1] for c in fronts)}..{max(c[1] for c in fronts)} in y, "
          f"x {min(c[0] for c in fronts)}..{max(c[0] for c in fronts)}, "
          f"{min(d)}..{max(d)} tiles from the start")


if __name__ == '__main__':
    main()
