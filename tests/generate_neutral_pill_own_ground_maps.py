#!/usr/bin/env python3
"""
Generate the ROOST arena for the neutral-pill-in-our-ground knobs
(POOL6_PICK_WITH_FACTORS, NEUTRAL_PILL_OWN_GROUND_FACTOR,
ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT):

  tests/scenario/maps/neutral_pill_own_ground_beats_far_base_approach.map

A long open grass field (100..152 x 116..136, so the map's middle is
(126,126) and the loader does not shift it). The spawn is a one-square
deep-sea pond at the west end (starts.c wants a deep-sea start); the
scenario script fills it with grass once the tank has driven off. Two
bases: ours at (118,133), the one the bot will attack at (150,126) at the
far east end (the script gives it to an empty seat). The map has no pills:
the script puts the neutral pill and the hostile decoy pill down once the
tank is on its way to the far base.

Run from the repository root:
    python tests/generate_neutral_pill_own_ground_maps.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate_lgm_wallshot_map import encode_map_runs  # noqa: E402

GRASS = 7
MAP_SIZE = 256
POND = (104, 126)
# x, y, owner (0xFF = nobody), armour, shells, mines
OUR_BASE = (118, 133, 0xFF, 90, 40, 40)
FAR_BASE = (150, 126, 0xFF, 90, 40, 40)

NAME = "neutral_pill_own_ground_beats_far_base_approach"


def build():
    t = [[None] * MAP_SIZE for _ in range(MAP_SIZE)]
    for y in range(116, 137):
        for x in range(100, 153):
            t[y][x] = GRASS
    t[POND[1]][POND[0]] = None
    pills = []
    bases = [OUR_BASE, FAR_BASE]
    starts = [(POND[0], POND[1], 0)]
    out = bytearray(b"BMAPBOLO")
    out += struct.pack("BBBB", 1, len(pills), len(bases), len(starts))
    for p in pills:
        out += struct.pack("BBBBB", *p)
    for b in bases:
        out += struct.pack("BBBBBB", *b)
    for s in starts:
        out += struct.pack("BBB", *s)
    out += encode_map_runs(t)
    return bytes(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "scenario", "maps", NAME + ".map")
    with open(path, "wb") as f:
        f.write(build())
    print("wrote", path)


if __name__ == "__main__":
    main()
