#!/usr/bin/env python3
"""
Make the maps of the pill take v2 (PILLTAKE_V2_EVALUATOR) ROOST arenas:

  tests/scenario/maps/pt2_oilrig_twin_neutral_double_take.map
  tests/scenario/maps/pt2_oilrig_twin_neutral_double_take_off.map
  tests/scenario/maps/pt2_oilrig_single_take.map
  tests/scenario/maps/pt2_oilrig_twin_carried_pills.map
      Copies of data/maps/DH-Oil Rig.map, unchanged: the arena scripts
      remove the pills they do not use and put down the bot's own pills.

  tests/scenario/maps/pt2_double_take_builds_wall.map
  tests/scenario/maps/pt2_double_take_builds_wall_off.map
  tests/scenario/maps/pt2_single_take_3walls.map
  tests/scenario/maps/pt2_single_take_3walls_off.map
  tests/scenario/maps/pt2_diag_double_take_carried.map
  tests/scenario/maps/pt2_diag_double_take_carried_off.map
      An open grass field (106..146 x 106..146, so the map's middle is
      (126,126) and the loader does not shift it), one base at (126,140)
      and a one-square deep-sea spawn pond at (126,144) (starts.c wants a
      deep-sea start; the script fills it with grass once the tank has
      driven off). No pills: the scripts put the neutral pills down.

Run from the repository root:
    python tests/generate_pt2_maps.py
"""
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate_lgm_wallshot_map import encode_map_runs  # noqa: E402

OILRIG_ARENAS = (
    "pt2_oilrig_twin_neutral_double_take",
    "pt2_oilrig_twin_neutral_double_take_off",
    "pt2_oilrig_single_take",
    "pt2_oilrig_twin_carried_pills",
    "pt2_chain_claim_two_allies",
    "pt2_chain_claim_two_allies_off",
)
FIELD_ARENAS = (
    "pt2_double_take_builds_wall",
    "pt2_double_take_builds_wall_off",
    "pt2_single_take_3walls",
    "pt2_single_take_3walls_off",
    "pt2_diag_double_take_carried",
    "pt2_diag_double_take_carried_off",
)

GRASS = 7
MAP_SIZE = 256
POND = (126, 144)
# x, y, owner (0xFF = nobody), armour, shells, mines
BASE = (126, 140, 0xFF, 90, 40, 40)


def build_field():
    t = [[None] * MAP_SIZE for _ in range(MAP_SIZE)]
    for y in range(106, 147):
        for x in range(106, 147):
            t[y][x] = GRASS
    t[POND[1]][POND[0]] = None
    pills = []
    bases = [BASE]
    starts = [(POND[0], POND[1], 0)]
    out = bytearray(b"BMAPBOLO")
    out += struct.pack("BBBB", 1, len(pills), len(bases), len(starts))
    for b in bases:
        out += struct.pack("BBBBBB", *b)
    for s in starts:
        out += struct.pack("BBB", *s)
    out += encode_map_runs(t)
    return bytes(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    src = os.path.join(root, "data", "maps", "DH-Oil Rig.map")
    out_dir = os.path.join(here, "scenario", "maps")
    for name in OILRIG_ARENAS:
        dst = os.path.join(out_dir, name + ".map")
        shutil.copyfile(src, dst)
        print("wrote", os.path.relpath(dst, root))
    field = build_field()
    for name in FIELD_ARENAS:
        dst = os.path.join(out_dir, name + ".map")
        with open(dst, "wb") as f:
            f.write(field)
        print("wrote", os.path.relpath(dst, root))


if __name__ == "__main__":
    main()
