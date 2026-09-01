#!/usr/bin/env python3
"""
Generate the water-pills arena (companion to tests/water_pills_test.py).

Field incident 20260831_173448_1 bot3 t=6819: two DEAD allied pills (#3, #5)
sat in deep sea one row off the south shore of DH-Oil Rig; the bot had just
respawned in a boat beside the east coast. A* (the BrainTest click) found the
pills; the goal pool never offered capture_pill for them, and the bot sailed
off to explore instead.

Arena: a grass/road island in open sea, NO river or shallow water anywhere
(so the bot can never build a boat — the one it spawns in is the only boat),
two dead pills owned by the bot's own player sitting in deep sea a few tiles
off the island, and the bot's start at sea right beside the island's east
shore (Bolo starts must be deep sea; a spawn at sea puts the tank in a boat).

Expected: the bot uses the boat it is in to sail to both dead pills and pick
them up. Test PASSes when both pills are in the tank.

IMPORTANT: mapRead RECENTERS off-center maps (bolo_map.c): the terrain
bounding box midpoint is shifted to (126,126). Only written (non-deep) tiles
count, so the island alone defines the box: x=120..132, y=120..132, midpoint
(126,126) -> recenter is a no-op and in-game coordinates match this file.

Usage:
    python3 tests/generate_water_pills_map.py [output_path]
    Default output: tests/water_pills.map
"""

import struct
import sys
from pathlib import Path

ROAD = 4
GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)

MAP_SIZE = 256

ISLAND = (120, 132, 120, 132)          # x0, x1, y0, y1 inclusive; midpoint (126,126)
PILLS = [(126, 136), (128, 136)]       # dead, owner = player 0, in deep sea 4 rows south of the island
SPAWN = (135, 126)                     # deep sea 3 tiles east of the island's east shore (x=132)
BASE = (126, 126)                      # a friendly base on the island so the bot has a home


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y0, y1 = ISLAND
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            t[y][x] = GRASS
    # A road ring one tile in from the shore: something for the bot to drive on.
    for x in range(x0 + 1, x1):
        t[y0 + 1][x] = ROAD
        t[y1 - 1][x] = ROAD
    for y in range(y0 + 1, y1):
        t[y][x0 + 1] = ROAD
        t[y][x1 - 1] = ROAD
    bx, by = BASE
    t[by][bx] = ROAD
    return t


def encode_map_runs(terrain):
    """Nibble-run encode; one run per CONTIGUOUS non-deep span (follows
    mapProcessRun: length nibble 0-7 = that many +1 literal nibbles; 8-15 =
    run of len-6 copies of the next nibble)."""
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
            endx = x
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
        Path(__file__).parent / "water_pills.map")
    terrain = make_map()

    for (x, y) in PILLS:
        assert terrain[y][x] is DEEP_SEA, f"pill ({x},{y}) must be in deep sea"
    assert terrain[SPAWN[1]][SPAWN[0]] is DEEP_SEA, "spawn must be at sea"
    assert terrain[SPAWN[1]][SPAWN[0] - 3] == GRASS, "spawn must be beside the island"

    # Pill record: x, y, owner, armour, speed. owner 0 = the bot's player
    # (it is the only player, slot 0); armour 0 = dead.
    pills = [(x, y, 0, 0, 50) for (x, y) in PILLS]
    bases = [(BASE[0], BASE[1], 0, 90, 90, 90)]   # owned by the bot's player
    starts = [(SPAWN[0], SPAWN[1], 12)]           # at sea, facing west (toward the island)

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
    print(f"  island {ISLAND}, dead own pills in deep sea at {PILLS}, sea spawn {SPAWN}, base {BASE}")


if __name__ == '__main__':
    main()
