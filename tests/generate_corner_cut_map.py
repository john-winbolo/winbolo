#!/usr/bin/env python3
"""
Generate the pathfinder corner-cut arena (companion to
tests/unit/test_pf_corner_cut.c and tests/corner_cut_test.py).

Recreates the Gothic Industrial geometry from field incident
20260713_233008_1 bot1 t=11467 — a rubble pocket with a halfbuilding at
(111,141) between the corridor and a base at (108,144) — inside a walled
island so the route is deterministic:

  * bot spawns AT SEA east of the island (Bolo starts must be deep sea),
    sails west and lands on the only shore (rows 141-142),
  * the neutral base at (108,144) pulls it west down the corridor,
  * at (112,141) the pre-fix Dijkstra planned the diagonal onto (111,142)
    past the solid halfbuilding corner at (111,141) and the tank ground
    against it; the fixed rule takes the cardinal step through the rubble
    (or the northern detour) instead.

Usage:
    python3 tests/generate_corner_cut_map.py [output_path]
    Default output: tests/corner_cut.map
"""

import struct
import sys
from pathlib import Path

BUILDING = 0
ROAD = 4
FOREST = 5
RUBBLE = 6
GRASS = 7
HALFBUILDING = 8
DEEP_SEA = None  # background sentinel

MAP_SIZE = 256

# Island bounds (walls on the border, sea outside).
#
# IMPORTANT: mapRead RECENTERS off-center maps (bolo_map.c "Do the
# recentering"): the terrain bounding box is shifted so its midpoint lands
# on (126,126), moving every pill/base/start with it. The arena is
# therefore built already-centered — (IX0+IX1)/2 == (IY0+IY1)/2 == 126 —
# so the recenter is a no-op and in-game coordinates match this file.
# (The original incident geometry lived at x104-116/y137-146; every
# feature below is that layout shifted by (+16,-16).)
IX0, IX1 = 120, 132     # inclusive; midpoint 126
IY0, IY1 = 121, 131     # inclusive; midpoint 126
SHORE_ROWS = (125, 126)  # east-side opening: grass shore, boat landing

BASE = (122, 128)        # in the open west chamber (a wall-notch base made
                         # the capture plow pin the tank against the wall
                         # behind it — a false positive for the grind check)
SPAWN = (136, 125)       # deep sea east of the island
PILL = (122, 123)        # dead neutral pill in the west chamber (extra pull)

# Incident features, shifted with the arena (originals in comments).
RUBBLE_TILES = [
    (125, 125), (126, 125),           # (109,141) (110,141)
    (126, 126), (127, 126),           # (110,142) (111,142)
]
HALF_TILES = [
    (127, 125),                       # (111,141) — THE corner the pre-fix diagonal cut
    (124, 123), (125, 123),           # (108,139) (109,139)
    (124, 124), (125, 124),           # (108,140) (109,140)
    (127, 127), (128, 127), (129, 127),  # (111,143) (112,143) (113,143)
    (128, 128), (129, 128),           # (112,144) (113,144)
]


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]

    # Island: SOLID WALL by default so the pocket is the ONLY way from the
    # landing shore to the base — no north/south detours for the test to
    # slip around the geometry under test.
    for y in range(IY0, IY1 + 1):
        for x in range(IX0, IX1 + 1):
            t[y][x] = BUILDING

    # East-west corridor (the incident's rows 141-142).
    for y in SHORE_ROWS:
        for x in range(IX0 + 1, IX1):
            t[y][x] = ROAD

    # East shore: grass landing strip, open through the border wall so the
    # boat can drive ashore.
    for y in SHORE_ROWS:
        for x in range(IX1 - 3, IX1 + 1):
            t[y][x] = GRASS

    # West chamber: base + pill live here, fed only by the corridor.
    for y in range(IY0 + 1, IY1):
        for x in range(IX0 + 1, IX0 + 4):
            t[y][x] = ROAD

    # The pocket, overlaid on the corridor (same shape as the incident).
    for (x, y) in RUBBLE_TILES:
        t[y][x] = RUBBLE
    for (x, y) in HALF_TILES:
        if t[y][x] != BUILDING:      # corner tiles inside walls stay walls
            t[y][x] = HALFBUILDING
    t[125][127] = HALFBUILDING       # THE corner — force it even on the corridor

    bx, by = BASE
    t[by][bx] = ROAD
    px, py = PILL
    t[py][px] = ROAD

    return t


def encode_map_runs(terrain):
    MAP_RUN_SAME = 6
    runs = bytearray()
    for y in range(MAP_SIZE):
        first = last = None
        for x in range(MAP_SIZE):
            if terrain[y][x] is not DEEP_SEA:
                if first is None:
                    first = x
                last = x
        if first is None:
            continue
        startx, endx = first, last + 1
        data = bytearray()
        x = startx
        while x < endx:
            run_start = x
            while x < endx and terrain[y][x] == terrain[y][run_start] and (x - run_start) < 9:
                x += 1
            run_len = x - run_start
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
    output = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).parent / "corner_cut.map")
    terrain = make_map()
    pills = [(PILL[0], PILL[1], 0xFF, 0, 50)]   # dead neutral pill
    bases = [(BASE[0], BASE[1], 0xFF, 90, 90, 90)]
    starts = [(SPAWN[0], SPAWN[1], 12)]         # at sea, facing west

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
    print(f"  sea spawn {SPAWN}, shore rows {SHORE_ROWS}, base {BASE},")
    print(f"  rubble pocket + halfbuilding corner at (127,125).")


if __name__ == '__main__':
    main()
