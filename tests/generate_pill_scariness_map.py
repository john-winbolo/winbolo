#!/usr/bin/env python3
"""
Generate the pill-scariness arena (companion to tests/pill_scariness_test.py).

Field incident 20260901_160325_1_par2 bot3, t=17930-18600.  Six free dead
pills sat in a heap for 700 ticks while the bot cycled defend-watch /
take_cover / capture, because our pill #4 at (123,131) read as
"taking_damage" and its ARRIVED-watch bid (30, weighted 93) preempted the
capture 50 ticks after the capture finally won.  #4 was at 13/15 HP and the
damage was STRAY FIRE from NEUTRAL pill #8 shooting at our tank -- not a tank
attack.  Pillboxes only ever fire at TANKS (pillbox.c pillsUpdate), so a shell
that lands on a pill is always a miss aimed at somebody else.

The brain now attributes every hp drop on a team pill to a source class
(perception.lua shell_source_class + its attribution pass) and scales defend
by it: tank x1.00 > enemy pill x0.50 > neutral pill x0.25.  The ARRIVED-watch
bid additionally needs tank fire OR a pill already below
DEFEND_WATCH_MIN_HP_FRAC (2/3) of full.

Arena -- the smallest shape that reproduces the incident:

    (126,120)   NEUTRAL pillbox N, alone on a one-tile island in a deep-sea
                moat.  Unreachable on foot and there is no river anywhere, so
                no boat can ever be built: attack_pill / capture_pill on N
                cost INF and never compete.  Its map `speed` byte is the
                reload period (pillbox.c reads it straight out of the file),
                set well above PILLBOX_ATTACK_NORMAL so it fires slowly and
                our pill's HP decays over hundreds of ticks, not tens.
    (126,122)   OUR pillbox P, full health, DIRECTLY between N and the tank.
                Every shell N fires at our tank crosses P's tile, and
                pillsIsPillHit (pillbox.c) makes any shell entering a live
                deployed pill's tile damage it whoever owns it.  So P takes
                the strays and the tank takes none of them.
    (126,124)   the tank start: a one-tile deep-sea pond (startsIsValidSquare
                requires a start square to be DEEP SEA), 4 tiles from N --
                well inside PILLBOX_RANGE (8), so N really does open fire.
    (137,131)   a lone DEAD pill, ~15 tiles from N.  This is the "other work"
                the incident's bot never got to.  Going to fetch it takes the
                tank OUT of N's range, which stops the shelling -- which is
                exactly why the correct behaviour keeps P healthy and the
                incorrect one (park on watch) grinds it down.
    (120,132)   our base.  The scenario empties its stock so refuel_at_base
                does not become the arena's main event.

Variant B is the control.  It DROPS the neutral pillbox and its island
entirely and puts a HOSTILE bot tank in a second pond six tiles south of P
(the scenario spawns it).  With no pillbox anywhere on the map, ANY damage P
takes can only be tank fire -- the shell's back-ray finds no muzzle, the
presence fallback finds a hostile tank, and even the "no evidence at all"
fallback answers "tank".  So B expects the pre-change behaviour back: src=tank
and the ARRIVED watch bid firing.  That is the half of the rule that must NOT
change, and it holds whichever shot happens to land.

IMPORTANT: mapRead RECENTERS off-center maps (bolo_map.c) -- the terrain
bounding-box midpoint is shifted to (126,126), but only when BOTH axes need
it.  Written tiles here span x=110..142 and y=112..140, whose integer
midpoints are both 126, so the recenter is a no-op and in-game coordinates
match this file.  main() asserts it.

Usage:
    python3 tests/generate_pill_scariness_map.py [A|B] [output_path]
    Default: variant A -> tests/pill_scariness_A.map
"""

import struct
import sys
from pathlib import Path

GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)

MAP_SIZE = 256
NEUTRAL = 0xFF

# ── Geometry (the test runner imports these for its assertions) ──────────
FIELD = (110, 142, 112, 140)          # x0, x1, y0, y1 inclusive
NEUTRAL_PILL = (126, 120)             # N, on its island
MOAT = [(x, y) for y in range(118, 122) for x in range(124, 129)
        if (x, y) != (126, 120)]      # deep sea ring around N (variant A only)
OUR_PILL = (126, 122)                 # P, the pill that catches the strays
SPAWN = (126, 124)                    # one-tile deep-sea pond
FOE_SPAWN = (126, 128)                # variant B only: the hostile bot's pond,
                                      # 6 tiles south of P, inside its reach
DEAD_PILL = (137, 131)                # the "other work"
BASE = (120, 132)

# Reload period for N, straight into the map file's pill `speed` byte
# (pillbox.c pillsReadMapFile).  PILLBOX_ATTACK_NORMAL is 100; a bigger
# number is a SLOWER pill.  200 ticks per shell means P loses ~1 HP every
# 4 seconds while the tank is in range, so the healthy window the test
# measures in lasts many hundreds of ticks.
NEUTRAL_RELOAD = 200

# ── Brain/engine constants this arena is designed against ────────────────
PILLS_MAX_HEALTH = 15                 # constants.lua M.PILLS_MAX_HEALTH
WATCH_MIN_HP_FRAC = 2.0 / 3.0         # constants.lua DEFEND_WATCH_MIN_HP_FRAC
HEALTHY_HP = 10                       # ceil(15 * 2/3) — at or above this the
                                      # pill is "healthy" and strays get no watch
PILLBOX_RANGE = 8                     # tiles a pill can actually SHOOT
DEFEND_ARRIVE_RADIUS = 10             # constants.lua — inside this the defend
                                      # bid is the ARRIVED heat/watch action


def dist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def make_map(variant="A"):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    fx0, fx1, fy0, fy1 = FIELD
    for yy in range(fy0, fy1 + 1):
        for xx in range(fx0, fx1 + 1):
            t[yy][xx] = GRASS
    # "Digging" a pond is just clearing the tile back to the deep-sea
    # background.  A start square MUST be deep sea (startsIsValidSquare) or
    # startsScatterFind spirals off to the nearest sea tile and the carefully
    # placed geometry is gone.
    if variant == "A":
        for (px, py) in MOAT:
            t[py][px] = DEEP_SEA
    t[SPAWN[1]][SPAWN[0]] = DEEP_SEA
    if variant == "B":
        t[FOE_SPAWN[1]][FOE_SPAWN[0]] = DEEP_SEA
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
    args = [a for a in sys.argv[1:]]
    variant = "A"
    if args and args[0].upper() in ("A", "B"):
        variant = args.pop(0).upper()
    output = args[0] if args else str(
        Path(__file__).parent / f"pill_scariness_{variant}.map")
    terrain = make_map(variant)

    # Recenter sanity: only written tiles define the bounding box, and its
    # integer midpoint must already be (126,126) or every coordinate shifts.
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    assert terrain[SPAWN[1]][SPAWN[0]] is DEEP_SEA, (
        "the start square must be deep sea (starts.c startsIsValidSquare)")
    assert terrain[OUR_PILL[1]][OUR_PILL[0]] is not DEEP_SEA
    assert terrain[DEAD_PILL[1]][DEAD_PILL[0]] is not DEEP_SEA
    if variant == "A":
        assert terrain[NEUTRAL_PILL[1]][NEUTRAL_PILL[0]] is not DEEP_SEA
    else:
        assert terrain[FOE_SPAWN[1]][FOE_SPAWN[0]] is DEEP_SEA

    # The three facts the whole test rests on:
    #  1. N, P and the spawn are COLLINEAR (same column) with P in the middle,
    #     so every shell N aims at a stationary tank crosses P's tile;
    #  2. the tank spawns inside N's firing range, so N really does open fire;
    #  3. the dead pill is OUTSIDE N's range, so fetching it stops the shelling
    #     (the correct behaviour keeps P healthy; parking on watch does not).
    if variant == "A":
      assert NEUTRAL_PILL[0] == OUR_PILL[0] == SPAWN[0], "N, P, spawn must share a column"
      assert NEUTRAL_PILL[1] < OUR_PILL[1] < SPAWN[1], "P must sit BETWEEN N and the tank"
      assert dist(NEUTRAL_PILL, SPAWN) <= PILLBOX_RANGE, (
        "spawn is out of PILLBOX_RANGE of N")
      assert dist(NEUTRAL_PILL, DEAD_PILL) > PILLBOX_RANGE + 4, (
        "the dead pill must be well outside N's range so fetching it ends the shelling")
    else:
      assert dist(OUR_PILL, FOE_SPAWN) <= PILLBOX_RANGE, (
        "the foe must spawn inside our pill's own reach, so the two are "
        "immediately each other's business")
    assert dist(OUR_PILL, SPAWN) <= DEFEND_ARRIVE_RADIUS, (
        "the tank must spawn INSIDE DEFEND_ARRIVE_RADIUS of P, or defend never "
        "reaches the ARRIVED heat/watch branch this test is about")

    # Pill records: x, y, owner, armour, speed.
    #   NEUTRAL (0xFF) = shoots every tank in range, belongs to nobody.
    #   owner 0        = the bot's player slot (the scenario re-owns them if
    #                    the bot lands somewhere else).
    #   armour 15 = PILLS_MAX_HEALTH (alive); armour 0 = DEAD (capturable).
    pills = []
    if variant == "A":
        pills.append((NEUTRAL_PILL[0], NEUTRAL_PILL[1], NEUTRAL, 15, NEUTRAL_RELOAD))
    pills.append((OUR_PILL[0], OUR_PILL[1], 0, PILLS_MAX_HEALTH, 100))
    pills.append((DEAD_PILL[0], DEAD_PILL[1], 0, 0, 100))
    bases = [(BASE[0], BASE[1], 0, 90, 90, 90)]   # emptied by variant A scenario
    starts = [(SPAWN[0], SPAWN[1], 8)]            # dir 8 = south, away from N
    if variant == "B":
        # The hostile bot start. on_choose_start in the sidecar pins each tank
        # to its own index so the two never swap ends.
        starts.append((FOE_SPAWN[0], FOE_SPAWN[1], 0))   # dir 0 = north, at P

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

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    if variant == "A":
        print(f"  variant A: neutral pill N{NEUTRAL_PILL} reload={NEUTRAL_RELOAD}t"
              f" on an island, our pill P{OUR_PILL} {dist(NEUTRAL_PILL, OUR_PILL):.0f} tiles"
              f" downrange, spawn pond {SPAWN} {dist(NEUTRAL_PILL, SPAWN):.0f} tiles from N")
        print(f"  dead pill {DEAD_PILL} at {dist(NEUTRAL_PILL, DEAD_PILL):.1f} tiles from N"
              f" (outside PILLBOX_RANGE {PILLBOX_RANGE}), base {BASE}")
    else:
        print(f"  variant B: no pillbox anywhere but ours - our pill P{OUR_PILL},"
              f" our pond {SPAWN}, hostile pond {FOE_SPAWN}"
              f" ({dist(OUR_PILL, FOE_SPAWN):.0f} tiles from P)")
        print(f"  dead pill {DEAD_PILL}, base {BASE}."
              f" Any damage on P can ONLY be tank fire.")
    print(f"  healthy line: hp >= {HEALTHY_HP} of {PILLS_MAX_HEALTH}"
          f" (DEFEND_WATCH_MIN_HP_FRAC {WATCH_MIN_HP_FRAC:.3f})")


if __name__ == '__main__':
    main()
