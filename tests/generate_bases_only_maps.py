#!/usr/bin/env python3
"""Write the small field the Bases Only ROOST arena plays on.

The field is the Pillbox Tag one: open grass, 40 by 40 squares, ringed by
deep sea, with eight starts round the middle. On it stand five pillboxes,
one in the middle and four round it, so the mod has more than one to take
off, and two neutral bases, full, six squares west and east of the middle.
The map loader moves the field so the middle of its land lands on
(126, 126); the arena reads every square it needs from the game and never
uses the numbers below.

Usage:
    C:/Python310/python.exe tests/generate_bases_only_maps.py
Writes tests/scenario/maps/<arena>.map for every arena in ARENAS.
"""

import os
import struct

from generate_test_map import encode_map_runs, MAP_SIZE, GRASS, DEEP_SEA

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenario", "maps")

ARENAS = [
    "bases_only_no_pills",
]

CX, CY = 128, 128
HALF = 20
NEUTRAL = 0xFF


def make_map():
    terrain = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    for y in range(CY - HALF, CY + HALF):
        for x in range(CX - HALF, CX + HALF):
            terrain[y][x] = GRASS
    return terrain


def write_bmap(path, terrain):
    # Pills are (x, y, owner, armour, speed); bases are (x, y, owner,
    # armour, shells, mines); starts are (x, y, dir).
    pills = [
        (CX, CY, NEUTRAL, 15, 50),
        (CX - 4, CY - 4, NEUTRAL, 15, 50), (CX + 4, CY - 4, NEUTRAL, 15, 50),
        (CX - 4, CY + 4, NEUTRAL, 15, 50), (CX + 4, CY + 4, NEUTRAL, 15, 50),
    ]
    bases = [
        (CX - 6, CY, NEUTRAL, 90, 90, 90),
        (CX + 6, CY, NEUTRAL, 90, 90, 90),
    ]
    starts = [
        (CX - 10, CY - 10, 0), (CX + 10, CY - 10, 0),
        (CX - 10, CY + 10, 0), (CX + 10, CY + 10, 0),
        (CX, CY - 12, 0), (CX, CY + 12, 0),
        (CX - 12, CY, 0), (CX + 12, CY, 0),
    ]
    with open(path, "wb") as f:
        f.write(b"BMAPBOLO")
        f.write(struct.pack("BBBB", 1, len(pills), len(bases), len(starts)))
        for p in pills:
            f.write(struct.pack("BBBBB", *p))
        for b in bases:
            f.write(struct.pack("BBBBBB", *b))
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
