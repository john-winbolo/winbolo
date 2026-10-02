#!/usr/bin/env python3
"""Write the small fields the Pillbox Tag ROOST arenas play on.

Every arena gets the same field: open grass, 40 by 40 squares, ringed by deep
sea, with one neutral pillbox in the middle (the prize) and eight starts round
it. The arenas move the tanks where they want them with game.teleport, so the
starts only have to be on land. The map loader moves the field so the middle of
its land lands on (126, 126); the arenas read the prize's square from the game
and never use the numbers below.

Usage:
    C:/Python310/python.exe tests/generate_pillbox_tag_maps.py
Writes tests/scenario/maps/<arena>.map for every arena in ARENAS.
"""

import os
import struct

from generate_test_map import encode_map_runs, MAP_SIZE, GRASS, DEEP_SEA

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenario", "maps")

ARENAS = [
    "pilltag_hop_ahead",
    "pilltag_hop_side",
    "pilltag_man_out",
    "pilltag_man_kill",
    "pilltag_drive_over",
    "pilltag_shoot_standing",
    "pilltag_fort_last_shot",
    "pilltag_team_hop",
]

CX, CY = 128, 128
HALF = 20


def make_map():
    terrain = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    for y in range(CY - HALF, CY + HALF):
        for x in range(CX - HALF, CX + HALF):
            terrain[y][x] = GRASS
    return terrain


def write_bmap(path, terrain):
    pills = [(CX, CY, 0xFF, 15, 50)]
    starts = [
        (CX - 10, CY - 10, 0), (CX + 10, CY - 10, 0),
        (CX - 10, CY + 10, 0), (CX + 10, CY + 10, 0),
        (CX, CY - 12, 0), (CX, CY + 12, 0),
        (CX - 12, CY, 0), (CX + 12, CY, 0),
    ]
    with open(path, "wb") as f:
        f.write(b"BMAPBOLO")
        f.write(struct.pack("BBBB", 1, len(pills), 0, len(starts)))
        for p in pills:
            f.write(struct.pack("BBBBB", *p))
        for s in starts:
            f.write(struct.pack("BBB", *s))
        f.write(encode_map_runs(terrain))


def main():
    terrain = make_map()
    for name in ARENAS:
        write_bmap(os.path.join(OUT, name + ".map"), terrain)
        print("wrote", name + ".map")


if __name__ == "__main__":
    main()
