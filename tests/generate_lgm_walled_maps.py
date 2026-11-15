#!/usr/bin/env python3
"""
Generate the three walled-builder ROOST arenas (knob LGM_WALLED_KILL):

  tests/scenario/maps/lgm_in_wall_prison_bot_breaks_ring_builder_gets_home.map
  tests/scenario/maps/lgm_inside_wall_bot_shoots_wall_and_kills_builder.map
  tests/scenario/maps/lgm_in_thick_wall_prison_bot_breaks_all_rings.map

All maps are the same open grass field (110..142 square), a forest strip
west of the spawn for tree errands, one pill four armour down (a top-up
errand for the builder) and one base in the far corner. The spawn is a
one-square deep-sea pond (starts.c wants a deep-sea start); the scenario
script fills it with grass once the tank has driven off. The walls are not
on the map: each scenario script builds them round the builder when he is
out of the tank.

Run from the repository root:
    python tests/generate_lgm_walled_maps.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate_lgm_wallshot_map import encode_map_runs  # noqa: E402

GRASS, FOREST = 7, 5
MAP_SIZE = 256
POND = (126, 130)
BASE = (140, 140, 0xFF, 90, 40, 40)

MAPS = {
    "lgm_in_wall_prison_bot_breaks_ring_builder_gets_home": (121, 123),
    "lgm_inside_wall_bot_shoots_wall_and_kills_builder": (126, 122),
    "lgm_in_thick_wall_prison_bot_breaks_all_rings": (114, 114),
}


def build(pill_xy):
    t = [[None] * MAP_SIZE for _ in range(MAP_SIZE)]
    for y in range(110, 143):
        for x in range(110, 143):
            t[y][x] = GRASS
    for y in range(120, 133):
        for x in range(114, 118):
            t[y][x] = FOREST
    t[POND[1]][POND[0]] = None
    pills = [(pill_xy[0], pill_xy[1], 0xFF, 11, 50)]
    bases = [BASE]
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
    for name, pill in MAPS.items():
        path = os.path.join(here, "scenario", "maps", name + ".map")
        with open(path, "wb") as f:
            f.write(build(pill))
        print("wrote", path)


if __name__ == "__main__":
    main()
