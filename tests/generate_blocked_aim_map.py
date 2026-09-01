#!/usr/bin/env python3
"""
Generate the blocked-aim arena (companion to tests/blocked_aim_test.py).

The situation under test is NOT a blocked PLANNED shot line — the standoff
scan already screens those.  `spot_clear_aim` tries all five
AIM_OFFSETS_TILE_FIRE points from every candidate spot and hard-rejects a
spot with no clear line, so any standoff the brain adopts is clean by
construction.  (Measured: on an open arena with one diagonal blocker the
brain simply walks to a bearing whose block is a sub-tile graze — standoff
(133,129) deg 111.75, 0.34 tiles of penetration; add a second blocker on that
bearing and it moves to (129,133) deg 158.25, 0.14 tiles.  It never once
reports the line blocked, so that arena cannot discriminate.)

The real abort fires while the tank is still EN ROUTE, several tiles short of
its standoff.  `shot_path_obstacle_count(info, goal, world)` with no aim
override walks `cpf.simulate_shot_angle(info.tankx, info.tanky,
info.tank_angle)` — the tank's LIVE HULL HEADING.  In Bolo the gun is bolted
to the hull, so while the tank is driving its "shot line" is just wherever it
happens to be pointing, which during a charge is its travel direction.  The
planner's clean bearing does not protect that ray.  Any live pill the hull
sweeps across, with the target still beyond it, is an instant
CHARGE_ABORT_OBSTACLE -> clear_attack_goal("shot path blocked: ...").

So the arena is built to make the DRIVE-IN sweep across our own pills:

    * 41x41 open GRASS arena, x=106..146, y=106..146.  Everything outside is
      deep sea (the unwritten background), far from the fight.
    * TARGET pill (126,126) — NEUTRAL, so it is hostile to the bot and a
      legitimate capture target.  Armour 7, deliberately below
      PPT_HEALTH_THRESHOLD — see TARGET_ARMOUR below.
    * A partial RING of OUR OWN pills (owner 0, armour 15) in the annulus
      2-3 tiles around the target, covering the north, east and south
      bearings and deliberately leaving the WEST sector open so
      plan_position can still find a legal standoff and adopt the pill:
          (129,126) E    (128,128) SE   (129,129) SE-outer
          (126,129) S    (128,124) NE   (126,123) N
          (124,129) SW   (124,123) NW
      Open: the due-WEST corridor along y=126 — the standoff lands out west,
      e.g. (118..120, 126), whose line east to the target is clean.  The two
      diagonal pills exist so that the first bearings plan_position tries are
      ALSO blocked: each blocked line costs one SANITY_REPLAN + SANITY_BAN,
      and SANITY_PILL_REPLANS_MAX (3) of them is what escalates to
      SANITY_ABANDON on the pre-fix brain.
    * START (136,136) — 10 east / 10 south, i.e. diametrically opposite the
      open sector.  The long drive from there to a western standoff has to
      arc right around the pill field, and the hull ray sweeps across
      (128,128) / (129,129) / (126,129) with the target still ahead of it.
    * BASE (116,116) — ours, fully stocked, out on the open (west) side but
      off the y=126 firing line so it never blocks the standoff shot.

Every one of our pills gets DEEP SEA underneath it from
tests/blocked_aim.scenario.lua.  Two reasons: the map loader paves ROAD under
every map-file pill, and without the sea the bot drives over its own pills
and repositions them (measured: the lone blocker was gone by tick 1249, after
which the arena tested nothing).  A tank cannot drive onto deep sea, an LGM
cannot walk to it, and there is no boat — the only other water is the
land-locked single-tile spawn pocket, whose boat beaches on the first move,
and there is no RIVER anywhere to build a new one.

Blockers dying to crossfire (our shells at the target, and the target pill
return fire, both cross their tiles) does not matter here: the abort only has
to happen once, early in the drive.

IMPORTANT: mapRead RECENTERS off-center maps (bolo_map.c mapCenter): the
terrain bounding box midpoint is shifted to (126,126).  Only written
(non-deep) tiles count, so the arena alone defines the box: x=106..146,
y=106..146, midpoint (126,126) -> both shifts are 0 and in-game coordinates
match this file exactly.  The script prints the post-shift coordinates so the
test can assert on them.

Usage:
    python3 tests/generate_blocked_aim_map.py [output_path] [--simple]
    --simple  the older single-diagonal-blocker layout, kept because it is
              the minimal statement of the geometry; it does NOT discriminate
              fixed from pre-fix (see above).
    Default output: tests/blocked_aim.map
"""

import struct
import sys
from pathlib import Path

ROAD = 4
GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)

MAP_SIZE = 256

# x0, x1, y0, y1 inclusive.  Midpoints ((106+146)//2) == 126 on both axes, so
# mapCenter addX/addY are 0 and nothing moves.
ARENA = (106, 146, 106, 146)

TARGET = (126, 126)          # neutral pill — the take

# Our own pills.  North/east/south of the target; the WEST sector is left
# open so a legal standoff still exists and the take is adopted.
RING = [(129, 126), (128, 128), (129, 129), (126, 129), (128, 124), (126, 123),
        (124, 129), (124, 123), (125, 128), (125, 124)]

# Bearings the baseline brain escaped to after its SANITY_BANs, measured run
# by run.  Adding a pill on each in turn is whack-a-mole: block (128,128)'s
# two escapes — standoff (129,133) deg 158.25 and (133,129) deg 111.75, i.e.
# pills at (127,129) and (129,127) — and the brain simply swings north to
# standoff (133,125) deg 80.25 instead.  That 12-pill variant also stopped the
# take completing (target left at armour 1 after 4000 ticks), which fails PASS
# condition 1 for a reason that has nothing to do with the bug, so it is NOT
# the default.  Left here as the record of what was tried.
ESCAPE_BEARING_PILLS = [(127, 129), (129, 127)]

# The older minimal layout: one blocker diagonally SE, start 10 east / 7 south.
SIMPLE_RING = [(127, 127)]
SIMPLE_START = (136, 133)

START = (136, 136)           # opposite the open sector — the drive arcs round
BASE = (116, 116)            # ours, stocked, off the y=126 firing line

# Our ring pills stay at full armour so they survive the crossfire as long as
# possible.  The TARGET is deliberately BELOW constants.PPT_HEALTH_THRESHOLD
# (8): attack.lua sets `goal._is_ppt = pill_hp >= PPT_HEALTH_THRESHOLD`, and
# only a NON-PPT take runs the aim -> charge ladder.  A full-health pill takes
# the PPT route (in_range_position / in_range_aim*) instead, which never
# reaches the charge substate and so never runs the live-hull-heading
# shot_path_obstacle_count that this test is about (measured: with a 15-armour
# target the baseline brain emitted zero aborts because it never charged).
PILL_ARMOUR = 15             # PILLBOX_FULL_ARMOUR — our ring pills
TARGET_ARMOUR = 7            # < PPT_HEALTH_THRESHOLD(8) -> non-PPT -> charge
PILL_SPEED = 50
NEUTRAL = 0xFF               # pillsSetPill folds any owner > MAX_TANKS-1 to NEUTRAL
OUR_PLAYER = 0               # the single bot is player slot 0
MAX_PILLS = 16               # bolo_map.h


def make_map(ring, start):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y0, y1 = ARENA
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            t[y][x] = GRASS
    # A road pad under each pill.  mapRead only forces ROAD under a pill when
    # the terrain is impassable, so spell it out; blocked_aim.scenario.lua
    # then puts DEEP SEA back under OUR pills at round setup.
    for (px, py) in [TARGET] + list(ring):
        t[py][px] = ROAD
    # Single-tile deep-sea spawn pocket — the only water written into the map,
    # and it is land-locked, so the boat the tank spawns in beaches on the
    # first move.  No RIVER anywhere, so no replacement boat is ever possible.
    t[start[1]][start[0]] = DEEP_SEA
    return t


def encode_map_runs(terrain):
    """Nibble-run encode; one run per CONTIGUOUS non-deep span (follows
    mapProcessRun: length nibble 0-7 = that many +1 literal nibbles; 8-15 =
    run of len-6 copies of the next nibble).  One record per span so the
    interior spawn pocket correctly splits its row."""
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
            endx = x  # exclusive
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


def recenter_shift(terrain):
    """Reproduce mapCenter addX/addY for the written (non-deep) tiles, so the
    printed coordinates are the IN-GAME ones.  addX = 126 - midX (from
    `(255/2) - ((left+right)/2) - 1` with integer division), and the engine
    only moves anything when BOTH addX and addY are non-zero."""
    left, right = MAP_SIZE, -1
    top, bottom = MAP_SIZE, -1
    for y in range(MAP_SIZE):
        for x in range(MAP_SIZE):
            if terrain[y][x] is not DEEP_SEA:
                left = min(left, x); right = max(right, x)
                top = min(top, y); bottom = max(bottom, y)
    add_x = (255 // 2) - ((left + right) // 2) - 1
    add_y = (255 // 2) - ((top + bottom) // 2) - 1
    if add_x != 0 and add_y != 0:
        return add_x, add_y
    return 0, 0


def main():
    argv = sys.argv[1:]
    simple = "--simple" in argv
    positional = [a for a in argv if not a.startswith("--")]
    output = positional[0] if positional else str(
        Path(__file__).parent / "blocked_aim.map")

    ring = SIMPLE_RING if simple else RING
    start = SIMPLE_START if simple else START
    terrain = make_map(ring, start)

    # Sanity-pin the geometry under test before writing.
    assert terrain[start[1]][start[0]] is DEEP_SEA, "start must be at sea"
    assert 1 + len(ring) <= MAX_PILLS, "too many pills for the engine"
    x0, x1, y0, y1 = ARENA
    for (px, py) in [TARGET, BASE] + list(ring):
        assert x0 < px < x1 and y0 < py < y1, f"({px},{py}) outside the arena"
    # The start pocket must be land-locked: no water route from the spawn to
    # the runtime sea tiles under our pills, so the spawn boat cannot reach
    # them and the ring is permanent.
    sx, sy = start
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dx or dy:
                assert terrain[sy + dy][sx + dx] is not DEEP_SEA, \
                    "the spawn pocket must be a single land-locked tile"
    # At least one bearing sector must stay open or plan_position finds no
    # legal standoff and never adopts the pill at all.
    if not simple:
        for wx in range(TARGET[0] - 8, TARGET[0]):
            assert (wx, TARGET[1]) not in ring, \
                "the due-west firing corridor must stay open for the standoff"

    # Pill record: x, y, owner, armour, speed.  owner 0xFF -> NEUTRAL (hostile
    # to everyone, so the bot may attack it); owner 0 -> the bot own player,
    # which the bot never deliberately shoots at.
    # ORDER MATTERS: the test identifies pills by index (0 = target, 1.. =
    # ours) because a captured pill moves off its tile.
    pills = [(TARGET[0], TARGET[1], NEUTRAL, TARGET_ARMOUR, PILL_SPEED)]
    pills += [(x, y, OUR_PLAYER, PILL_ARMOUR, PILL_SPEED) for (x, y) in ring]
    bases = [(BASE[0], BASE[1], OUR_PLAYER, 90, 90, 90)]
    starts = [(start[0], start[1], 12)]        # at sea, dir 12 = west

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

    ax, ay = recenter_shift(terrain)

    def shifted(p):
        return (p[0] + ax, p[1] + ay)

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  arena GRASS {ARENA}, recenter shift = ({ax},{ay})"
          + ("   [--simple layout]" if simple else ""))
    print("  in-game (post-shift) coordinates:")
    print(f"    TARGET  = {shifted(TARGET)}   neutral pill, armour "
          f"{TARGET_ARMOUR} (< PPT_HEALTH_THRESHOLD, so the take charges)")
    print(f"    OURS    = {[shifted(p) for p in ring]}")
    print(f"              player {OUR_PLAYER}, armour {PILL_ARMOUR}, each on "
          f"DEEP SEA (blocked_aim.scenario.lua)")
    print(f"    START   = {shifted(start)}   land-locked sea pocket")
    print(f"    BASE    = {shifted(BASE)}   ours, stocked")


if __name__ == '__main__':
    main()
