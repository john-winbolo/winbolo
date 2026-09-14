#!/usr/bin/env python3
"""
Generate the LGM wall-shot arena (companion to tests/lgm_wallshot_test.py).

Field incident 20260901_182558_1_par1b bot2 t=8410.  The tank stood two tiles
south of a shot wall it was wall-clearing and its own builder -- dispatched
north-and-west past that wall -- died to the tank's own shell.  The builder was
NEVER inside the wall square: the movement gates (mapGetManSpeed(HALFBUILDING)
= 0) pinned him flush against the wall's SOUTH edge while he slid west.  His
pinned y landed 0-1 world-units above the square boundary, and
lgmDeathCheckAtPosition maps the man with a -1/-2 world-unit nudge
((x-1)>>8, (y-2)>>8) instead of the movement code's raw >>8 -- so the check
placed him INSIDE the wall square and the "explosion on a solid square kills
the man on that square" rule fired.  PR #262 changes the death check to the
raw mapping.

The arena reproduces that pin deterministically:

  - a WALL ROW spanning the field at y = WALL_ROW (fresh BUILDING squares;
    BUILDING_LIFE 4 means each takes 5 shells to rubble, so a short volley
    leaves it standing),
  - a FOREST ROW at y = FOREST_ROW for the builder's harvest target,
  - a one-tile deep-sea spawn pond (starts.c demands a deep-sea start) at
    POND, several rows south of the wall.

The shoot-north brain (written by the test) drives north, stops two tiles
south of the wall, sends the man to harvest at (own_x - K, FOREST_ROW) --
far north, a few tiles west, exactly the par1b shape -- and taps shoot while
the man is pinned against the wall sliding across the tank's column.  Whether
the pin remainder lands in the lethal 0-1 wu band is a deterministic function
of the dispatch angle, i.e. of K; the test's probe mode sweeps K to find a
killing one, then that K repeats identically every run.

IMPORTANT: mapRead recenters off-center maps (terrain bbox midpoint moved to
(126,126)).  The field is symmetric about (126,126) so the recenter is a
no-op; main() asserts it.
"""

import struct
import sys
from pathlib import Path

GRASS = 7
FOREST = 5
BUILDING = 0        # wall terrain code (global.h BUILDING; 9 is BOAT!)
DEEP_SEA = None     # background sentinel (unwritten cells read as deep sea)
MAP_SIZE = 256
NEUTRAL = 0xFF

# -- Geometry (the test runner imports these) --------------------------------
FIELD = (110, 142, 110, 142)   # x0, x1, y0, y1 -- symmetric about (126,126)
WALL_ROW = 122                 # wall spans the field at this y
FOREST_ROW = 114               # harvest targets; 8 rows north of the wall
POND = (126, 130)              # spawn pond: 8 rows south of the wall
BASE = (140, 140)              # one neutral base in the corner (map needs one;
                               # far from the action, never touched)
STOP_ROW = WALL_ROW + 2        # brain parks here: two tiles south of the wall,
                               # the par1b tank-to-wall distance

# Wall gap on the east edge so the field is not cut in two: the wall row
# spans x = FIELD.x0 .. GAP_X-1, leaving the far east open.  The action all
# happens around x=126; the gap is 12+ tiles away and only exists so the
# harvest side of the map is reachable if the man ever wanders (he should
# not -- he walks over nothing solid on the straight line).
GAP_X = 139


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    fx0, fx1, fy0, fy1 = FIELD
    for yy in range(fy0, fy1 + 1):
        for xx in range(fx0, fx1 + 1):
            t[yy][xx] = GRASS
    for xx in range(fx0, GAP_X):
        t[WALL_ROW][xx] = BUILDING
    for xx in range(fx0, fx1 + 1):
        t[FOREST_ROW][xx] = FOREST
    t[POND[1]][POND[0]] = DEEP_SEA
    return t


def encode_map_runs(terrain):
    """Nibble-run encode (mapProcessRun): length nibble 0-7 = that many +1
    literal nibbles; 8-15 = run of len-6 copies of the next nibble."""
    MAP_RUN_SAME = 6
    runs = bytearray()
    for y in range(MAP_SIZE):
        x = 0
        while x < MAP_SIZE:
            if terrain[y][x] is DEEP_SEA:
                x += 1
                continue
            startx = x
            while x < MAP_SIZE and terrain[y][x] is not DEEP_SEA:
                x += 1
            endx = x
            data = bytearray()
            i = startx
            while i < endx:
                run_start = i
                while (i < endx and terrain[y][i] == terrain[y][run_start]
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
            runs.append(startx)
            runs.append(endx)
            runs.extend(data)
    runs.extend(b'\x04\xFF\xFF\xFF')
    return bytes(runs)


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else str(
        Path(__file__).parent / "lgm_wallshot.map")
    terrain = make_map()

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    assert terrain[POND[1]][POND[0]] is DEEP_SEA, "start square must be deep sea"
    assert terrain[WALL_ROW][126] == BUILDING
    assert terrain[FOREST_ROW][120] == FOREST
    assert POND[1] > WALL_ROW and FOREST_ROW < WALL_ROW

    pills = []
    bases = [(BASE[0], BASE[1], NEUTRAL, 90, 40, 40)]
    starts = [(POND[0], POND[1], 0)]

    with open(output, 'wb') as f:
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

    # No scenario script: the brain keeps the wall standing with the in-game
    # LGM repair action, so this arena runs on plain main.  Remove a stale one
    # left by an earlier version.
    stale = Path(str(Path(output).with_suffix('')) + ".scenario.lua")
    if stale.exists():
        stale.unlink()

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  wall row y={WALL_ROW} x={FIELD[0]}..{GAP_X-1}, forest row "
          f"y={FOREST_ROW}, pond {POND}, stop row {STOP_ROW}")


if __name__ == '__main__':
    main()
