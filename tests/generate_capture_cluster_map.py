#!/usr/bin/env python3
"""
Generate the capture-cluster arena (companion to tests/capture_cluster_test.py).

Field incident 20260901_160325_1_par2 bot3, t=17930-18600.  SIX dead pills sat
in one heap at (114-117,130-131) -- no reject on any of them -- and
capture_pill#2 still priced 234, because every member was quoted the full
~11-tile trip through a contested zone as if it were the only pill there.  Six
free pills in one heap is ONE errand: the tank is already there for the first,
and the rest are a few tiles of walking.  So each member now pays its SHARE
(goals.lua capture_cluster_refresh):

    cost / min(n, CAPTURE_CLUSTER_DIVISOR_MAX)   floored at CAPTURE_CLUSTER_MIN_COST
         x (1 + CAPTURE_CLUSTER_GUARD_MULT * k)  capped at CAPTURE_CLUSTER_GUARD_MAX

where k counts the live hostile/neutral pillboxes that can actually put a
shell on a cluster tile -- PILLBOX_RANGE plus the heated margin AND a clear
line of fire, the same test the sea harvest uses.  The discount must not walk
a tank into a fortress, which is what k prices back in.

Three variants:

  A  Five dead pills in a heap 11 tiles east of the tank, nothing guarding
     them and nothing else to do.  n=5, k=0: the capture prices at a fifth of
     the trip and should sweep the whole heap inside the minute.

  B  A LONG arena.  The heap sits 40 tiles east -- far enough that its
     undiscounted price clears the pool's IMMINENT_CAPTURE_PATH_COST (30)
     clamp, so the cluster arithmetic actually reaches the decision instead of
     being flattened to the IMMINENT floor of 5.  TWO neutral pillboxes six
     tiles north and south of the heap guard it, each on its own one-tile
     island in a deep-sea moat, so the bot can neither drive to them nor shoot
     them down (there is no river anywhere on the map, so no boat can ever be
     built and attack_pill / capture_pill on them cost INF).  Both have the
     range and a clear line onto the heap, so k=2 and the cluster prices back
     up x2.50.  A LONE dead pill sits five tiles west of the tank -- safe,
     unguarded, and the answer the bot is supposed to reach for first.

  C  B's control: the identical long arena with the two guards DELETED.  Now
     k=0, the discount stands undiluted, and the same bot should reach for the
     heap instead.  A and B alone cannot prove the guard term did anything --
     C is what pins it down, because the ONLY difference between B and C is
     those two pillboxes.

Rough arithmetic for B/C with the constants as shipped (base 20, dist scale
0.05 on raw^1.5, danger scale 0.10, free-grab bonus 30 with a floor of 20):

    safe pill,  5 tiles, raw ~10  ->  20 + 1.6 - 30    -> floored to 20
    heap,      40 tiles, raw ~80  ->  20 + 35.8 + ~1   -> ~57  (no free bonus:
                                                               too far, and guarded)
      C (k=0):  57 / 5 = 11.4                 -> the heap beats the safe 20
      B (k=2):  57 / 5 = 11.4, x2.50 = 28.5   -> the safe pill wins

Neutral rather than an enemy player's pillboxes for the guards: a neutral
pillbox shoots every tank in range (pillbox.c pillsUpdate skips only allies
and the owner), it needs no second player in the game, and the brain's
sea_threat_pills counts hostile and neutral identically -- which is exactly
the code path the guard scan reuses.

IMPORTANT: mapRead RECENTERS off-center maps (bolo_map.c) -- the terrain
bounding-box midpoint is shifted to (126,126), but only when BOTH axes need
it.  Both fields here are symmetric about x=126 and y=126, so the recenter is
a no-op and in-game coordinates match this file.  main() asserts it.

mapRead ALSO replaces RIVER/DEEP_SEA/BUILDING under every map-file pillbox
with ROAD, so no pill here is written on water: the heap and the lone pill sit
on plain grass and the guards on their island centres.

Usage:
    python3 tests/generate_capture_cluster_map.py [A|B|C] [output_path]
    Default: variant A -> tests/capture_cluster_A.map
"""

import struct
import sys
from pathlib import Path

GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)

MAP_SIZE = 256
NEUTRAL = 0xFF

# ── Geometry (the test runner imports these for its assertions) ──────────
SPAWN = (126, 126)                    # one-tile deep-sea pond (start squares
                                      # must be deep sea: startsIsValidSquare)
# Variant A: a short arena, the heap 11 tiles east.
FIELD_A = (110, 142, 112, 140)
CENTRE_A = (137, 126)
# Variants B/C: a long arena, the heap 40 tiles east so its undiscounted price
# clears the pool's IMMINENT clamp.  The field is widened symmetrically about
# x=126 so the map still needs no recentering.
FIELD_BC = (82, 170, 112, 140)
CENTRE_BC = (166, 126)
GUARDS = [(166, 120), (166, 132)]     # 6 tiles N and S of the heap centre (B only)
SAFE_PILL = (121, 126)                # 5 tiles west of the tank, 45 from the guards
BASE = (120, 134)

# ── Brain/engine constants this arena is designed against ────────────────
CLUSTER_RADIUS = 3                    # constants.lua CAPTURE_CLUSTER_RADIUS (Manhattan)
DIVISOR_MAX = 6                       # constants.lua CAPTURE_CLUSTER_DIVISOR_MAX
GUARD_MULT = 0.75                     # constants.lua CAPTURE_CLUSTER_GUARD_MULT
GUARD_MAX = 4.0                       # constants.lua CAPTURE_CLUSTER_GUARD_MAX
PILLBOX_RANGE = 8                     # tiles a pill can actually SHOOT
IMMINENT_PATH_COST = 30               # constants.lua IMMINENT_CAPTURE_PATH_COST


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def guard_mult(k):
    """The multiplier the brain will apply for k covering pills."""
    return min(GUARD_MAX, 1.0 + GUARD_MULT * k)


def field(variant):
    return FIELD_A if variant == "A" else FIELD_BC


def centre(variant):
    return CENTRE_A if variant == "A" else CENTRE_BC


def cluster(variant):
    """The heap.  Every member is within CAPTURE_CLUSTER_RADIUS (3, Manhattan)
    of the middle one, so build_pill_clusters chains all five into ONE
    cluster."""
    cx, cy = centre(variant)
    return [(cx - 1, cy), (cx, cy), (cx + 1, cy), (cx, cy - 1), (cx, cy + 1)]


def guards(variant):
    return GUARDS if variant == "B" else []


def island(cx, cy):
    """The deep-sea ring that isolates a guard pillbox on its own tile."""
    return [(x, y) for y in range(cy - 1, cy + 2) for x in range(cx - 1, cx + 2)
            if (x, y) != (cx, cy)]


def make_map(variant="A"):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    fx0, fx1, fy0, fy1 = field(variant)
    for yy in range(fy0, fy1 + 1):
        for xx in range(fx0, fx1 + 1):
            t[yy][xx] = GRASS
    for (gx, gy) in guards(variant):
        for (mx, my) in island(gx, gy):
            t[my][mx] = DEEP_SEA
    t[SPAWN[1]][SPAWN[0]] = DEEP_SEA
    return t


def encode_map_runs(terrain):
    """Nibble-run encode; one run per CONTIGUOUS non-deep span (follows
    mapProcessRun: length nibble 0-7 = that many +1 literal nibbles; 8-15 =
    run of len-6 copies of the next nibble)."""
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
    args = list(sys.argv[1:])
    variant = "A"
    if args and args[0].upper() in ("A", "B", "C"):
        variant = args.pop(0).upper()
    output = args[0] if args else str(
        Path(__file__).parent / f"capture_cluster_{variant}.map")
    terrain = make_map(variant)

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    assert terrain[SPAWN[1]][SPAWN[0]] is DEEP_SEA, (
        "the start square must be deep sea (starts.c startsIsValidSquare)")

    CL = cluster(variant)
    CC = centre(variant)
    # The heap must really be ONE cluster under the brain's own union rule:
    # greedy single-linkage on Manhattan distance <= CAPTURE_CLUSTER_RADIUS.
    for c in CL:
        assert terrain[c[1]][c[0]] is not DEEP_SEA, f"cluster tile {c} needs land"
        assert mdist(c, CC) <= CLUSTER_RADIUS, (
            f"{c} is {mdist(c, CC)} from the centre - it would start its own "
            f"cluster instead of joining this one")
    assert len(set(CL)) == len(CL), "duplicate cluster tile"

    for g in guards(variant):
        # Every guard must have the RANGE to cover the heap; the line of fire
        # is open grass, which the brain checks for itself with a shot sim.
        reach = [c for c in CL if edist(g, c) <= PILLBOX_RANGE]
        assert reach, (f"guard {g} reaches no cluster tile "
                       f"(nearest {min(edist(g, c) for c in CL):.1f} "
                       f"> PILLBOX_RANGE {PILLBOX_RANGE})")
        # ...and must NOT reach the safe pill or the spawn, or the "safe
        # alternative" this variant rests on is not safe.
        assert edist(g, SAFE_PILL) > PILLBOX_RANGE + 2, (
            f"guard {g} is {edist(g, SAFE_PILL):.1f} tiles from the safe pill")
        assert edist(g, SPAWN) > PILLBOX_RANGE + 2, (
            f"guard {g} is {edist(g, SPAWN):.1f} tiles from the spawn")
    if variant != "A":
        assert terrain[SAFE_PILL[1]][SAFE_PILL[0]] is not DEEP_SEA
        assert mdist(SPAWN, CC) >= 30, (
            "the heap must be far enough that its undiscounted price clears "
            f"IMMINENT_CAPTURE_PATH_COST ({IMMINENT_PATH_COST}), or the pool "
            "flattens every capture under it to the IMMINENT floor of 5 and "
            "the cluster arithmetic never reaches the decision")

    # Pill records: x, y, owner, armour, speed.  armour 0 = DEAD (capturable).
    pills = [(c[0], c[1], 0, 0, 100) for c in CL]
    if variant != "A":
        pills.append((SAFE_PILL[0], SAFE_PILL[1], 0, 0, 100))
    # armour 15 = alive; speed 100 = PILLBOX_ATTACK_NORMAL.
    pills += [(g[0], g[1], NEUTRAL, 15, 100) for g in guards(variant)]
    bases = [(BASE[0], BASE[1], 0, 90, 90, 90)]
    starts = [(SPAWN[0], SPAWN[1], 2)]   # dir 2 = east, facing the heap

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

    k = len(guards(variant))
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  variant {variant}: {len(CL)} dead pills in one heap around {CC}, "
          f"{mdist(SPAWN, CC)} tiles from the spawn {SPAWN}; base {BASE}")
    print(f"  expect cluster n={len(CL)} /{min(len(CL), DIVISOR_MAX)}X "
          f"guard {k} (x{guard_mult(k):.2f})")
    if variant != "A":
        print(f"  safe pill {SAFE_PILL}, {mdist(SPAWN, SAFE_PILL)} tiles from "
              f"the spawn; guards {guards(variant) or 'NONE (control)'}")


if __name__ == '__main__':
    main()
