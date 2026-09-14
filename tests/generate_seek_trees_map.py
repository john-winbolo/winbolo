#!/usr/bin/env python3
"""Generate a seek-trees test map.

Purpose: verify the bot, when loaded with pills it can't place because it's out
of wood, will TRAVEL to a distant safe forest to harvest trees (rather than
deadlocking re-issuing an unpayable build order — 20260707_044217 t=127262).

Layout:
  * A big grass arena.
  * A cluster of DEAD friendly pillboxes (armour 0) right on top of the bot's
    spawn, so it scoops them into its tank fast — heavy pressure to build.
  * A friendly base at spawn (respawn + our influence so the search prefers it).
  * The ONLY forest is a cluster in the FAR NORTH corner. The bot spends its
    starting wood placing a few pills, then must sail... er, drive all the way
    north to the forest to get more.

Pass (checked by the runner): the bot fires the seek_trees goal, reaches the
far forest, and harvests wood (trees climb back up) — i.e. it explicitly went
and got trees instead of sitting stuck.

Usage: python generate_seek_trees_map.py [out.map]   (default: tests/seek_trees.map)
"""

import sys
import struct
from pathlib import Path

from generate_test_map import MAP_SIZE, GRASS, DEEP_SEA

FOREST = 5

# Geometry (constants the runner also imports for its assertions). The arena runs
# well south of the map's start tile because Strict/Tournament perturbs the spawn
# (observed ~(130,154)) — the whole spawn neighbourhood must be land, not sea.
ARENA = (104, 156, 92, 164)          # x0,x1,y0,y1
FOREST_BOX = (126, 131, 95, 100)     # x0,x1,y0,y1 (far NORTH — only wood on the map)
SPAWN = (128, 150)                   # bot start (south); Strict perturbs this
BASE = (128, 156)                    # friendly base (deep south)
# Dead friendly pills clustered just north of spawn — a whole bunch, to pile on
# pills so the bot (spawned with NO wood in Strict) is instantly carry>=1, tr=0.
DEAD_PILLS = [(x, y) for y in (143, 146) for x in (120, 124, 128, 132, 136)]  # 10


def encode_map_runs(terrain):
    R = 6
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
        data = bytearray()
        x = first
        while x <= last:
            rs = x
            while x <= last and terrain[y][x] == terrain[y][rs] and (x - rs) < 9:
                x += 1
            rl = x - rs
            tile = terrain[y][rs]
            data.append(((rl + R) << 4) | (tile & 0x0F) if rl >= 2 else (tile & 0x0F))
        runs += bytes([4 + len(data), y, first, last + 1]) + data
    runs += b'\x04\xFF\xFF\xFF'
    return bytes(runs)


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y0, y1 = ARENA
    for y in range(y0, y1):
        for x in range(x0, x1):
            t[y][x] = GRASS
    fx0, fx1, fy0, fy1 = FOREST_BOX
    for y in range(fy0, fy1):
        for x in range(fx0, fx1):
            t[y][x] = FOREST
    return t


def write_bmap(path):
    pills = [(x, y, 0, 0, 50) for (x, y) in DEAD_PILLS]   # owner 0, armour 0 = dead friendly
    bases = [(BASE[0], BASE[1], 0, 90, 90, 90)]
    starts = [(SPAWN[0], SPAWN[1], 0)]                    # dir 0 = north (toward forest)
    with open(path, 'wb') as f:
        f.write(b'BMAPBOLO'); f.write(struct.pack('B', 1))
        f.write(struct.pack('B', len(pills)))
        f.write(struct.pack('B', len(bases)))
        f.write(struct.pack('B', len(starts)))
        for x, y, o, a, s in pills:
            f.write(struct.pack('BBBBB', x, y, o, a, s))
        for x, y, o, a, sh, mi in bases:
            f.write(struct.pack('BBBBBB', x, y, o, a, sh, mi))
        for x, y, d in starts:
            f.write(struct.pack('BBB', x, y, d))
        f.write(encode_map_runs(make_map()))
    print(f"Wrote {path} ({Path(path).stat().st_size} bytes)")
    print(f"  {len(pills)} dead pills at spawn, forest at {FOREST_BOX}, bot @ {SPAWN}")
    print(f"  Run with: -bots 1 -brain brains/GoalHunter_1.7/init.lua")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).parent / "seek_trees.map")
    write_bmap(out)


if __name__ == '__main__':
    main()
