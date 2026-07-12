#!/usr/bin/env python3
"""Generate the lead-targeting (aim) test map.

Purpose: verify the bot LEADS a moving target — it must aim AHEAD of a tank
crossing its front so the target drives into the shell, not where the target
currently is. Exercises the tank-combat lead fix (and, on a LuaJIT build, the
two-arg math.atan/atan2 heading fix).

Layout (NO bases, NO pills — nothing to distract the shooter):
  * An all-grass arena with a straight ROAD lane running west->east.
  * VICTIM (player 1, brain tests/brains/drive_east.lua) starts at the WEST end
    of the lane and drives due-east across the shooter's front at full speed.
  * SHOOTER (player 0, GoalHunter) starts SOUTH_OFFSET tiles south of the lane,
    centred on the victim's path, facing north — so the victim crosses directly
    in front of it and it must lead the shot.

Pass (checked by the runner): the shooter lands >= AIM_MIN_HITS shells on the
victim (victim armour drops) during the crossing.

Usage: python generate_aim_map.py [out.map]   (default: tests/aim_arena.map)
"""

import sys
import struct
from pathlib import Path

from generate_test_map import MAP_SIZE, GRASS, ROAD, DEEP_SEA


def encode_map_runs(terrain):
    """RLE-encode terrain. Unlike the seek_trees encoder (one span per row), this
    emits a separate run-record for every contiguous non-sea span, so interior
    deep-sea tiles (the single-tile spawn pockets) split the row correctly."""
    R = 6
    runs = bytearray()
    for y in range(MAP_SIZE):
        x = 0
        while x < MAP_SIZE:
            if terrain[y][x] is DEEP_SEA:
                x += 1
                continue
            first = x
            while x < MAP_SIZE and terrain[y][x] is not DEEP_SEA:
                x += 1
            last = x - 1                       # inclusive end of this land span
            data = bytearray()
            xx = first
            while xx <= last:
                rs = xx
                while xx <= last and terrain[y][xx] == terrain[y][rs] and (xx - rs) < 9:
                    xx += 1
                rl = xx - rs
                tile = terrain[y][rs]
                data.append(((rl + R) << 4) | (tile & 0x0F) if rl >= 2 else (tile & 0x0F))
            runs += bytes([4 + len(data), y, first, last + 1]) + data
    runs += b'\x04\xFF\xFF\xFF'
    return bytes(runs)

# WinBolo tanks spawn on BOATS in deep sea (startsGetStart requires the start
# square to be DEEP_SEA) then drive ashore. If combat happens ON WATER a hit
# destroys the boat and the victim DROWNS + respawns (armour "regenerates") —
# which muddies a lead-aim test. So the map is a big GRASS field with just a
# single-tile deep-sea POCKET at each start: the tank spawns on a boat, drives
# straight onto grass, and all fighting is on land where hits cleanly subtract
# armour. No bases/pills — nothing to distract the shooter, and (no base) no
# refuel/repair, so the victim's armour only ever goes DOWN.

# Geometry (the runner imports these for its assertions).
LANE_Y       = 121                 # east-west road the victim drives along
SOUTH_OFFSET = 7                   # shooter sits this many tiles south of the lane
SHOOTER_Y    = LANE_Y + SOUTH_OFFSET
LANE_X0      = 104                 # victim spawn pocket (west)
LANE_X1      = 162                 # victim's drive_east STOP point (STOP_MAPX=160)
SHOOTER_X    = (LANE_X0 + LANE_X1) // 2   # centred on the victim's path
# Big land field so neither tank can wander to open sea and drown.
ARENA        = (60, 200, 80, 170)  # x0,x1,y0,y1

VICTIM_START  = (LANE_X0, LANE_Y, 64)          # dir 64 = east; pocket, road begins to its east
SHOOTER_START = (SHOOTER_X, SHOOTER_Y, 0)       # dir 0 = north (toward the lane)


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y0, y1 = ARENA
    for y in range(y0, y1):
        for x in range(x0, x1):
            t[y][x] = GRASS
    # Road lane so the victim crosses at a constant, predictable full speed.
    for x in range(LANE_X0 + 1, LANE_X1):
        t[LANE_Y][x] = ROAD
    # Single-tile deep-sea spawn pockets (the only water on the map).
    t[VICTIM_START[1]][VICTIM_START[0]] = DEEP_SEA
    t[SHOOTER_START[1]][SHOOTER_START[0]] = DEEP_SEA
    return t


def write_bmap(path):
    # start[0] -> player 0 (shooter/GoalHunter), start[1] -> player 1 (victim).
    starts = [SHOOTER_START, VICTIM_START]
    with open(path, 'wb') as f:
        f.write(b'BMAPBOLO'); f.write(struct.pack('B', 1))
        f.write(struct.pack('B', 0))            # npills = 0
        f.write(struct.pack('B', 0))            # nbases = 0
        f.write(struct.pack('B', len(starts)))
        for x, y, d in starts:
            f.write(struct.pack('BBB', x, y, d))
        f.write(encode_map_runs(make_map()))
    print(f"Wrote {path} ({Path(path).stat().st_size} bytes)")
    print(f"  shooter(P0) @ {SHOOTER_START[:2]}  victim(P1) @ {VICTIM_START[:2]} -> drives east along y={LANE_Y}")
    print(f"  shooter is {SOUTH_OFFSET} tiles south of the lane, no bases/pills")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).parent / "aim_arena.map")
    write_bmap(out)


if __name__ == '__main__':
    main()
