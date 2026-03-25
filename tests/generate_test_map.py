#!/usr/bin/env python3
"""
Generate a simple Bolo test map with close start positions.

Creates a small arena with roads, grass, forest, and buildings where all
player start positions are within view range of each other. This makes
multi-player tests reliable since players can always see each other.

The map is output in BMAP format (same as .map files used by WinBolo).

Usage:
    python3 tests/generate_test_map.py [output_path]
    Default output: tests/test_arena.map
"""

import struct
import sys
from pathlib import Path

# Terrain types (from global.h)
# DEEP_SEA (0xFF) is implicit — tiles outside run ranges default to it.
# Within the 4-bit nibble encoding, values 0-15 are:
BUILDING = 0
RIVER = 1
SWAMP = 2
CRATER = 3
ROAD = 4
FOREST = 5
RUBBLE = 6
GRASS = 7
HALFBUILDING = 8
BOAT = 9
# 10-15 are mined variants (value - 8 = base terrain)

# Use None as the background (deep sea) sentinel in our terrain grid.
DEEP_SEA = None

# Map is 256x256, but only the area 20-236 is really usable.
MAP_SIZE = 256


def make_map():
    """Build a 256x256 terrain grid for the test arena."""
    terrain = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]

    cx, cy = 128, 128  # Center of map

    # Fill arena with grass (speed 12) — well within usable bounds (20-236)
    for y in range(cy - 12, cy + 12):
        for x in range(cx - 12, cx + 12):
            terrain[y][x] = GRASS

    # Cross-shaped road through the center (speed 16)
    for i in range(cx - 10, cx + 10):
        terrain[cy][i] = ROAD      # Horizontal road
        terrain[i][cx] = ROAD      # Vertical road

    # A second horizontal road a few squares up
    for i in range(cx - 10, cx + 10):
        terrain[cy - 4][i] = ROAD

    # Some forest patches (speed 6)
    for y in range(cy + 3, cy + 7):
        for x in range(cx - 8, cx - 4):
            terrain[y][x] = FOREST

    for y in range(cy - 7, cy - 3):
        for x in range(cx + 4, cx + 8):
            terrain[y][x] = FOREST

    # A few buildings (impassable) — walls to test collision
    terrain[cy - 2][cx + 3] = BUILDING
    terrain[cy - 2][cx + 4] = BUILDING
    terrain[cy - 3][cx + 3] = BUILDING
    terrain[cy - 3][cx + 4] = BUILDING

    # Some swamp (speed 3)
    for y in range(cy + 2, cy + 4):
        for x in range(cx + 5, cx + 8):
            terrain[y][x] = SWAMP

    # Some crater (speed 3)
    terrain[cy + 1][cx - 3] = CRATER
    terrain[cy + 1][cx - 2] = CRATER

    # Some rubble (speed 3)
    terrain[cy - 1][cx - 5] = RUBBLE
    terrain[cy - 1][cx - 4] = RUBBLE

    return terrain


def encode_map_runs(terrain):
    """Encode terrain as BMAP run-length data.

    The BMAP format encodes the map row-by-row. For each row that has
    non-deep-sea tiles, a "run" is written:
      - datalen (1 byte): total bytes in this run including header
      - y (1 byte): row number
      - startx (1 byte): first non-deep-sea column
      - endx (1 byte): column after last non-deep-sea tile
      - data: nibble-encoded terrain values

    Nibble encoding (matches mapProcessRun in bolo_map.c):
      - MAP_RUN_SAME = 6, MAP_RUN_DIFF = 8
      - "Same" run (2-9 identical tiles):
          high nibble = count + 6 (range 8-15), low nibble = terrain
          Decoder: tiles = highNibble - MAP_RUN_SAME = highNibble - 6
      - "Different" run (1-8 different tiles):
          high nibble = count - 1 (range 0-7), followed by count tile nibbles
          Decoder: len = highNibble + 1
    """
    MAP_RUN_SAME = 6

    runs = bytearray()

    for y in range(MAP_SIZE):
        # Find the extent of non-deep-sea tiles in this row
        first = None
        last = None
        for x in range(MAP_SIZE):
            if terrain[y][x] is not DEEP_SEA:
                if first is None:
                    first = x
                last = x

        if first is None:
            continue  # Row is all deep sea, skip

        startx = first
        endx = last + 1  # One past the last

        # Encode the tile data using nibble-based run-length encoding.
        # We emit each tile as a "1 different tile" run for simplicity:
        #   high nibble = 0 (count-1), low nibble = terrain value
        # This always produces correct output since each byte encodes
        # exactly one tile, consuming one nibble-pair per tile.
        #
        # The decoder for "different" with high=0: len = 0+1 = 1,
        # places 1 tile (the low nibble), then moves to lowLen state
        # which reads the next nibble as a new length. So each tile
        # consumes exactly one nibble-pair (one byte = two nibbles,
        # high is length, low is terrain).
        data = bytearray()
        x = startx
        while x < endx:
            # Count consecutive identical tiles (max 9 for same-encoding)
            run_start = x
            while x < endx and terrain[y][x] == terrain[y][run_start] and (x - run_start) < 9:
                x += 1
            run_len = x - run_start
            tile = terrain[y][run_start]

            if run_len >= 2:
                # Same encoding: high = count + MAP_RUN_SAME, low = terrain
                code = ((run_len + MAP_RUN_SAME) << 4) | (tile & 0x0F)
                data.append(code)
            else:
                # Single tile: high = 0 (count-1=0), low = terrain
                code = (0 << 4) | (tile & 0x0F)
                data.append(code)

        datalen = 4 + len(data)
        runs.append(datalen)
        runs.append(y)
        runs.append(startx)
        runs.append(endx)
        runs.extend(data)

    # Terminating run: datalen=4, y=0xFF, startx=0xFF, endx=0xFF
    runs.extend(b'\x04\xFF\xFF\xFF')

    return bytes(runs)


def make_pills():
    """Create pillboxes for the test map.

    Placed far from the center so tanks don't engage them during tests.
    """
    cx, cy = 128, 128
    pills = [
        # (x, y, owner, armour, speed)
        (cx - 11, cy - 11, 0xFF, 15, 50),  # Neutral pill, far NW corner
        (cx + 11, cy + 11, 0xFF, 15, 50),  # Neutral pill, far SE corner
    ]
    return pills


def make_bases():
    """Create bases for the test map.

    Two bases near the center for refueling.
    """
    cx, cy = 128, 128
    bases = [
        # (x, y, owner, armour, shells, mines)
        (cx - 3, cy + 5, 0xFF, 90, 90, 90),  # Neutral base, south-west
        (cx + 3, cy - 5, 0xFF, 90, 90, 90),  # Neutral base, north-east
    ]
    return bases


def make_starts():
    """Create player start positions.

    All starts are clustered near the center of the map, within 5 map squares
    of each other, ensuring all players can see each other immediately.
    Direction 0 = north.
    """
    cx, cy = 128, 128
    starts = [
        (cx - 2, cy + 1, 0),   # Start 0: just SW of center, facing N
        (cx + 2, cy + 1, 0),   # Start 1: just SE of center, facing N
        (cx - 2, cy - 1, 4),   # Start 2: just NW of center, facing E
        (cx + 2, cy - 1, 4),   # Start 3: just NE of center, facing E
        (cx,     cy + 3, 0),   # Start 4: south of center
        (cx,     cy - 3, 0),   # Start 5: north of center
        (cx - 4, cy,     6),   # Start 6: west of center, facing S
        (cx + 4, cy,     2),   # Start 7: east of center, facing W
    ]
    return starts


def write_bmap(path, terrain, pills, bases, starts):
    """Write a BMAP format file."""
    with open(path, 'wb') as f:
        # Header
        f.write(b'BMAPBOLO')
        f.write(struct.pack('B', 1))  # Version 1

        # Counts
        f.write(struct.pack('B', len(pills)))
        f.write(struct.pack('B', len(bases)))
        f.write(struct.pack('B', len(starts)))

        # Pills (5 bytes each: x, y, owner, armour, speed)
        for x, y, owner, armour, speed in pills:
            f.write(struct.pack('BBBBB', x, y, owner, armour, speed))

        # Bases (6 bytes each: x, y, owner, armour, shells, mines)
        for x, y, owner, armour, shells, mines in bases:
            f.write(struct.pack('BBBBBB', x, y, owner, armour, shells, mines))

        # Starts (3 bytes each: x, y, dir)
        for x, y, d in starts:
            f.write(struct.pack('BBB', x, y, d))

        # Map terrain runs
        f.write(encode_map_runs(terrain))

    print(f"Wrote {path} ({Path(path).stat().st_size} bytes)")
    print(f"  {len(pills)} pills, {len(bases)} bases, {len(starts)} starts")
    print(f"  Arena centered at (128, 128), ~24x24 map squares")
    print(f"  Terrain: grass, road, forest, swamp, crater, rubble, buildings")
    print(f"  All starts within 6 squares of center")
    print(f"  Pillboxes at corners, well away from starts")


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).parent / "test_arena.map")
    terrain = make_map()
    pills = make_pills()
    bases = make_bases()
    starts = make_starts()
    write_bmap(output, terrain, pills, bases, starts)


if __name__ == '__main__':
    main()
