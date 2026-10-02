#!/usr/bin/env python3
"""Generate the maps for the lead_aim_* and acquire_* scenario arenas.

Two layouts. Each arena has its own copy under tests/scenario/maps/, because
the gate runner looks for maps/<arena>.map.

LEAD (lead_aim_land, lead_aim_boat)
  * A 5x5 grass ISLAND for the shooter, in deep sea.
  * A 3-row LANE (road) running west to east, 6 squares north of the island
    centre. The arena script refills it with grass or river per case.
  * Deep sea between them, so neither tank can reach the other. The shooter
    can only fight from its island, at 4 to 6 squares.
  * Start 1 (the shooter) and start 2 (the target) are on open deep sea. The
    script moves both tanks onto their ground on the first tick.

ACQUIRE (acquire_idle, acquire_driving)
  * A grass FIELD 41 squares wide and 61 long, in deep sea, so the shooter can
    turn and drive while it fights without meeting the shore.
  * A neutral BASE near the north edge of the field. It is the shooter's
    errand in the driving case.
  * A 3x3 grass PARKING islet 80 squares west of the field centre, where the
    target waits out of sight between events.
  * Start 1 (the shooter) is in the sea south of the field, start 2 (the
    target) beside the parking islet.

SHORE (acquire_shore)
  * The ACQUIRE layout with the field cut down to a 3-wide road STRIP, so
    the shooter drives along a shore on both sides. The arena makes the
    square under the target grass when it falls in the sea.

No pills anywhere. mapRead moves the map so that the middle of the land is at
(126,126), so the arena scripts place everything relative to start 1, and
never by the absolute squares below.

Usage: python tests/generate_lead_aim_maps.py   (writes into tests/scenario/maps)
"""

import struct
from pathlib import Path

from generate_test_map import MAP_SIZE, GRASS, ROAD, DEEP_SEA
from generate_aim_map import encode_map_runs

MAPS = Path(__file__).parent / "scenario" / "maps"

# ── LEAD layout (file squares). The script reads these offsets from start 1.
LEAD_ISLAND_C = (126, 130)       # island centre; island is +-2 around it
LEAD_LANE_Y = 124                # lane centre row; lane is +-1 around it
LEAD_LANE_X = (100, 152)         # lane first and last column
LEAD_START_SHOOTER = (126, 136)  # = island centre + (0, 6)
LEAD_START_TARGET = (96, 124)    # west of the lane, on the lane row

# ── ACQUIRE layout (file squares).
ACQ_FIELD_X = (126, 20)          # field centre column, half width
ACQ_FIELD_Y = (96, 156)          # field first and last row
ACQ_BASE = (126, 98)             # neutral base near the north edge
ACQ_PARK_C = (46, 126)           # parking islet centre; islet is +-1
ACQ_START_SHOOTER = (126, 160)   # = field south edge + (0, 4)
ACQ_START_TARGET = (42, 126)     # beside the parking islet


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def fill(t, x0, y0, x1, y1, tile):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            t[y][x] = tile


def write_bmap(path, terrain, bases, starts):
    with open(path, "wb") as f:
        f.write(b"BMAPBOLO")
        f.write(struct.pack("B", 1))
        f.write(struct.pack("B", 0))              # no pills
        f.write(struct.pack("B", len(bases)))
        f.write(struct.pack("B", len(starts)))
        for x, y, owner, armour, shells, mines in bases:
            f.write(struct.pack("BBBBBB", x, y, owner, armour, shells, mines))
        for x, y, d in starts:
            f.write(struct.pack("BBB", x, y, d))
        f.write(encode_map_runs(terrain))
    print("wrote %s (%d bytes)" % (path, path.stat().st_size))


def lead_map():
    t = blank()
    cx, cy = LEAD_ISLAND_C
    fill(t, cx - 2, cy - 2, cx + 2, cy + 2, GRASS)
    fill(t, LEAD_LANE_X[0], LEAD_LANE_Y - 1, LEAD_LANE_X[1], LEAD_LANE_Y + 1, ROAD)
    starts = [(LEAD_START_SHOOTER[0], LEAD_START_SHOOTER[1], 0),
              (LEAD_START_TARGET[0], LEAD_START_TARGET[1], 4)]
    return t, [], starts


def acquire_map():
    t = blank()
    cx, hw = ACQ_FIELD_X
    fill(t, cx - hw, ACQ_FIELD_Y[0], cx + hw, ACQ_FIELD_Y[1], GRASS)
    px, py = ACQ_PARK_C
    fill(t, px - 1, py - 1, px + 1, py + 1, GRASS)
    bases = [(ACQ_BASE[0], ACQ_BASE[1], 0xFF, 90, 90, 90)]
    starts = [(ACQ_START_SHOOTER[0], ACQ_START_SHOOTER[1], 0),
              (ACQ_START_TARGET[0], ACQ_START_TARGET[1], 0)]
    return t, bases, starts


def shore_map():
    t, bases, starts = acquire_map()
    cx, hw = ACQ_FIELD_X
    fill(t, cx - hw, ACQ_FIELD_Y[0], cx + hw, ACQ_FIELD_Y[1], DEEP_SEA)
    fill(t, cx - 1, ACQ_FIELD_Y[0], cx + 1, ACQ_FIELD_Y[1], ROAD)
    return t, bases, starts


def main():
    for name in ("lead_aim_land", "lead_aim_boat"):
        write_bmap(MAPS / (name + ".map"), *lead_map())
    for name in ("acquire_idle", "acquire_driving"):
        write_bmap(MAPS / (name + ".map"), *acquire_map())
    write_bmap(MAPS / "acquire_shore.map", *shore_map())


if __name__ == "__main__":
    main()
