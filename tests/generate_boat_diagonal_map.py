#!/usr/bin/env python3
"""Generate a boat-navigation test map: a narrow diagonal deep-water staircase.

Purpose: stress the boat shoreline forward-alignment throttle. The bot spawns in
a boat at the bottom of a ONE-tile-wide deep-water channel that zig-zags up-right,
up-right, ... through solid land. A DEAD pillbox sits at the top of the channel,
also in deep water, so the only way to capture it is to sail the boat all the way
up the staircase. Cutting a diagonal corner clips a land tile — which disembarks
the tank (loses the boat) and strands/drowns it — so the run only succeeds if the
bot threads the channel cardinally (up, then right, then up, ...).

Pass condition (checked by the runner): the bot ends with the pill in its tank and
never died on the map.

Coordinate convention (matches generate_test_map.py): +y is SOUTH; deep sea (0xFF)
is the implicit background, so land is what we explicitly paint. Pills are
(x, y, owner, armour, speed); armour 0 = a dead pill.

Usage:
    python generate_boat_diagonal_map.py [output.map]
    Default output: tests/boat_diagonal.map
"""

import sys
import struct
from pathlib import Path

from generate_test_map import MAP_SIZE, GRASS, DEEP_SEA

# Mined terrain: nibbles 10-15 are mined variants where (value - 8) = base tile,
# so mined grass = 7 + 8 = 15. Every land tile carries a mine, so if the bot
# corner-cuts off the boat onto land it takes mine damage — that's how the test
# distinguishes "sailed the channel" from "disembarked and drove on land".
MINED_GRASS = 15


def encode_map_runs(terrain):
    """BMAP run-length encoder that supports INTERIOR deep-sea (gaps) by emitting
    multiple runs per row — one per contiguous non-deep-sea segment. (The shared
    generate_test_map encoder only handles deep sea as a background border.)"""
    MAP_RUN_SAME = 6
    runs = bytearray()
    for y in range(MAP_SIZE):
        x = 0
        while x < MAP_SIZE:
            if terrain[y][x] is DEEP_SEA:
                x += 1
                continue
            seg_start = x
            while x < MAP_SIZE and terrain[y][x] is not DEEP_SEA:
                x += 1
            seg_end = x  # one past the last non-deep tile
            data = bytearray()
            i = seg_start
            while i < seg_end:
                run_start = i
                while (i < seg_end and terrain[y][i] == terrain[y][run_start]
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
            runs.append(seg_start)
            runs.append(seg_end)
            runs.extend(data)
    runs.extend(b'\x04\xFF\xFF\xFF')  # terminator
    return bytes(runs)

# Channel start (bottom) and the number of NE steps. Each step is two channel
# tiles: one UP (y-1) then one RIGHT (x+1), so the boat must move cardinally.
START_X, START_Y = 105, 135
STEPS = 13   # 13 up-right zig-zags -> ends 13 tiles up and 13 right

# Land pad around the channel so it's an ISOLATED pocket (not connected to the
# open sea), forcing the bot to sail the staircase rather than go around.
PAD = 4


def channel_tiles():
    """The ordered list of deep-water tiles forming the up-right staircase."""
    tiles = [(START_X, START_Y)]
    x, y = START_X, START_Y
    for _ in range(STEPS):
        y -= 1               # up
        tiles.append((x, y))
        x += 1               # right
        tiles.append((x, y))
    return tiles


def make_map(chan):
    """Solid grass covering the channel's bounding box + PAD, with the channel
    tiles carved back out to deep sea."""
    terrain = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    xs = [t[0] for t in chan]
    ys = [t[1] for t in chan]
    x0, x1 = min(xs) - PAD, max(xs) + PAD
    y0, y1 = min(ys) - PAD, max(ys) + PAD
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            terrain[yy][xx] = MINED_GRASS   # every land tile is mined
    for (cx, cy) in chan:
        terrain[cy][cx] = DEEP_SEA   # carve the (safe) water channel back out
    return terrain


def write_bmap(path, terrain, pills, bases, starts):
    with open(path, 'wb') as f:
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


def main():
    output = (sys.argv[1] if len(sys.argv) > 1
              else str(Path(__file__).parent / "boat_diagonal.map"))
    chan = channel_tiles()
    end_x, end_y = chan[-1]
    terrain = make_map(chan)

    # One DEAD pill (armour 0) at the top of the channel, in deep water, owned by
    # the bot (player 0) so its capture/recover goal drives it out there.
    pills = [(end_x, end_y, 0, 0, 50)]
    bases = []
    # Bot spawns at the channel bottom (deep water -> in a boat). dir 0 = north.
    starts = [(START_X, START_Y, 0)]

    write_bmap(output, terrain, pills, bases, starts)
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  channel: {len(chan)} deep-water tiles, {START_X},{START_Y} -> {end_x},{end_y} (up-right x{STEPS})")
    print(f"  dead pill (armour 0, owner 0) at ({end_x},{end_y}); bot start at ({START_X},{START_Y})")
    print(f"  Run with: -bots 1 -brain brains/GoalHunter_1.7/init.lua")


if __name__ == '__main__':
    main()
