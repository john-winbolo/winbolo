#!/usr/bin/env python3
"""
Generate the cliff-staircase arena (companion to tests/cliff_staircase_test.py).

Recreates the DH-Oil Rig geometry from field incident 20260831_113629_1
bot2 t=6081 — a 1-tile road staircase running diagonally NW along a
deep-sea edge:

    (143,114) -> (142,113) -> (141,112)   with deep sea at (142,112),
                                          (143,113), (144,114)...

Each diagonal step is road-to-road, but the corner being cut is DEEP SEA.
The tank hugged the top edge of (142,113), aimed at the next tile's
center, and its center clipped (142,112) -> engine drown (tank.c:565,
center-tile deep sea, correct Bolo behaviour). Both steering guards
missed it: the global cliff brake samples the heading ray one whole tile
at a time, and the lookahead cliff guard held to the A* next step whose
center-to-center segment itself cuts the deep corner.

The arena forces that exact shape, then adds a RIVER FORD on the only
route to the base — so the same test also fails if a cliff-guard fix
overshoots and makes the bot refuse legitimate water (rivers are
passable; only deep sea kills).

The whole arena is wrapped in a BUILDING ring with ONE water inlet on
the east — a first cut of this map left the north sea open and the bot
simply sailed its boat around the wall and landed beside the base,
never touching the staircase. Boats sail deep sea; the courtyard walls
must therefore seal every water approach except the landing lane, and
the cut corners become 1-tile deep-sea pockets inside the courtyard
(the engine drowns on center-tile entry either way).

Route (only path, everything else is wall or deep sea):

    sea spawn (137,127) -> boat W through the inlet (134..133,127)
    -> grounds at grass shore (131..132,127)
    -> road W along y=127
    -> staircase NW: (128,127) -> (127,126) -> (126,125) -> (125,124)
       cut corners (128,126) (127,125) (126,124) are DEEP SEA pockets
       safe corners (127,127) (126,126) (125,125) are ROAD
    -> road W along y=124
    -> RIVER ford (121,124) (122,124)
    -> neutral base (119,124)

IMPORTANT: mapRead RECENTERS off-center maps (bolo_map.c): the terrain
bounding box midpoint is shifted to (126,126). The arena is built
already-centered — written tiles span x=118..134, y=123..129, midpoints
(126,126) — so the recenter is a no-op and in-game coordinates match
this file.

Usage:
    python3 tests/generate_cliff_staircase_map.py [output_path]
    Default output: tests/cliff_staircase.map
"""

import struct
import sys
from pathlib import Path

BUILDING = 0
RIVER = 1
ROAD = 4
GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)

MAP_SIZE = 256

BASE = (119, 124)        # neutral base at the far (NW) end of the route
SPAWN = (137, 127)       # deep sea east of the inlet (Bolo starts must be at sea)
RIVER_TILES = [(121, 124), (122, 124)]     # the ford — only way to the base
STAIRCASE = [(128, 127), (127, 126), (126, 125), (125, 124)]  # SE -> NW
CUT_CORNERS = [(128, 126), (127, 125), (126, 124)]   # deep-sea pockets
SAFE_CORNERS = [(127, 127), (126, 126), (125, 125)]  # road — the L-step escape
INLET = [(133, 127), (134, 127)]           # the only boat-reachable water


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]

    # Courtyard: solid building fill x=118..134, y=123..129, then carve
    # the route out of it. Everything not carved stays wall, so the only
    # water a BOAT can reach is the east inlet — the deep-sea cut corners
    # are sealed 1-tile pockets a tank can only enter by corner-cutting.
    for y in range(123, 130):
        for x in range(118, 135):
            t[y][x] = BUILDING

    # y=124: NW corridor — base, river ford, road, staircase top.
    for x in (119, 120, 123, 124, 125):
        t[124][x] = ROAD
    for (x, y) in RIVER_TILES:
        t[y][x] = RIVER
    t[124][126] = DEEP_SEA            # cut corner of (126,125)->(125,124)

    # y=125: safe corner (125,125) + staircase tile (126,125).
    t[125][125] = ROAD
    t[125][126] = ROAD
    t[125][127] = DEEP_SEA            # cut corner of (127,126)->(126,125)

    # y=126: safe corner (126,126) + staircase tile (127,126).
    t[126][126] = ROAD
    t[126][127] = ROAD
    t[126][128] = DEEP_SEA            # cut corner of (128,127)->(127,126)

    # y=127: safe corner (127,127) + staircase foot (128,127) + approach
    # road + grass landing shore + water inlet through the east wall.
    for x in range(127, 131):
        t[127][x] = ROAD
    t[127][131] = GRASS
    t[127][132] = GRASS
    for (x, y) in INLET:
        t[y][x] = DEEP_SEA

    return t


def encode_map_runs(terrain):
    """Nibble-run encode; one run per CONTIGUOUS non-deep span so interior
    deep-sea cells (the cut corners) stay deep. Follows mapProcessRun:
    length nibble 0-7 = that many +1 literal nibbles; 8-15 = run of
    len-6 copies of the next nibble."""
    MAP_RUN_SAME = 6
    runs = bytearray()
    for y in range(MAP_SIZE):
        x = 0
        while x < MAP_SIZE:
            if terrain[y][x] is DEEP_SEA:
                x += 1
                continue
            startx = x
            while x < MAP_SIZE and terrain[y][x] is not DEEP_SEA:
                x += 1
            endx = x  # exclusive
            data = bytearray()
            i = startx
            while i < endx:
                run_start = i
                while (i < endx and terrain[y][i] == terrain[y][run_start]
                       and (i - run_start) < 9):
                    i += 1
                run_len = i - run_start
                tile = terrain[y][run_start]
                if run_len >= 2:
                    data.append(((run_len + MAP_RUN_SAME) << 4) | (tile & 0x0F))
                else:
                    data.append((0 << 4) | (tile & 0x0F))
            runs.append(4 + len(data))
            runs.append(y)
            runs.append(startx)
            runs.append(endx)
            runs.extend(data)
    runs.extend(b'\x04\xFF\xFF\xFF')
    return bytes(runs)


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else str(
        Path(__file__).parent / "cliff_staircase.map")
    terrain = make_map()

    # Sanity-pin the geometry under test before writing.
    for (x, y) in CUT_CORNERS:
        assert terrain[y][x] is DEEP_SEA, f"cut corner ({x},{y}) not deep"
    for (x, y) in SAFE_CORNERS:
        assert terrain[y][x] == ROAD, f"safe corner ({x},{y}) not road"
    for (x, y) in STAIRCASE:
        assert terrain[y][x] == ROAD, f"staircase ({x},{y}) not road"

    pills = []
    bases = [(BASE[0], BASE[1], 0xFF, 90, 90, 90)]  # neutral, stocked
    starts = [(SPAWN[0], SPAWN[1], 12)]             # at sea, facing west

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
        for x, y, d in starts:
            f.write(struct.pack('BBB', x, y, d))
        f.write(encode_map_runs(terrain))

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  sea spawn {SPAWN}, shore (131..132,127), staircase {STAIRCASE},")
    print(f"  river ford {RIVER_TILES}, base {BASE}.")


if __name__ == '__main__':
    main()
