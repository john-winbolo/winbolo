#!/usr/bin/env python3
"""Generate a reposition-stress test map for GoalHunter bots.

Scenario the map sets up (see tests/README or the reposition test runner):
  * THREE friendly GoalHunter bots (players 0,1,2, allied via -teams 3,1) hold
    the SOUTH half. They own a big, over-concentrated cluster of BACK pillboxes
    — far more back coverage than the position needs, so the reposition
    evaluator flags them as surplus and the bots vote to relocate them forward.
  * ONE enemy GoalHunter bot (player 3, team 2) holds the NORTH half, owning a
    row of front pillboxes + a base placed CLOSE to the friendly side. Those
    hostile pills fire on any friendly tank that pushes up, forcing a front line
    at mid-map instead of letting the friendlies roam.
  * A neutral no-man's-land of open grass between the two sides.

Coordinate convention (matches generate_test_map.py): +y is SOUTH, and pill/base
records are (x, y, owner, ...). owner 0-15 = player slot, 0xFF = NEUTRAL. The map
loader fixes the terrain under each pill/base, so the play area is just grass.

Usage:
    python generate_reposition_map.py [output.map]
    Default output: tests/reposition_arena.map
"""

import sys
import struct
from pathlib import Path

# Reuse the proven terrain encoder + constants from the sibling generator.
from generate_test_map import encode_map_runs, MAP_SIZE, GRASS, ROAD, DEEP_SEA

CX, CY = 128, 128

# Team ownership. Friendly bots occupy player slots 0,1,2 (team 1 via -teams
# 3,1); the enemy bot is slot 3 (team 2). Map owner bytes must match those slots.
FRIENDLY_OWNERS = (0, 1, 2)
ENEMY_OWNER = 3


def make_map():
    """Open grass arena spanning both halves, with a road strip on the front line
    so the mid-map contest zone reads clearly and tanks have clean footing."""
    terrain = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    # Play rectangle: x in [100,156], y in [100,158] — compact so the bots meet
    # quickly. Everything grass (drivable); the loader stamps pill/base tiles.
    for y in range(100, 159):
        for x in range(100, 157):
            terrain[y][x] = GRASS
    # Front-line road strip across mid-map (visual + a lane along the contest).
    for x in range(104, 153):
        terrain[CY][x] = ROAD
    return terrain


def make_pills():
    """Pillboxes: a TON of surplus friendly BACK pills (south) + a front row of
    enemy pills (north, close to center to force the line).

    (x, y, owner, armour, speed)
    """
    pills = []

    # The engine caps a map at MAX_PILLS = 16 total (bolo_map.h). We use all 16:
    # 13 over-concentrated friendly back pills + 3 enemy front pills.
    #
    # --- Friendly BACK pills: a dense southern cluster. Spacing ~4 tiles is BELOW
    # the ~5-tile target the strategic placer wants, so the cluster reads as
    # redundant/surplus and drives reposition votes.
    back = [
        (120, 144), (124, 144), (128, 144), (132, 144), (136, 144),  # row 144 (5)
        (122, 148), (126, 148), (130, 148), (134, 148),              # row 148 (4)
        (120, 152), (124, 152), (128, 152), (132, 152),              # row 152 (4)
    ]  # 13 total
    for oi, (gx, gy) in enumerate(back):
        pills.append((gx, gy, FRIENDLY_OWNERS[oi % len(FRIENDLY_OWNERS)], 15, 50))

    # --- Enemy FRONT pills: a row CLOSE to the friendly cluster (~8 tiles north
    # of the front row), in firing view, so the friendlies must hold a line and
    # fight for the mid-map rather than free-roam. No enemy bot owns them — they
    # are static team-2 pills (owner slot 3, unoccupied), hostile to team 1.
    for ex in (120, 128, 136):
        pills.append((ex, 136, ENEMY_OWNER, 15, 50))

    return pills


def make_bases():
    """Bases: friendly refuel bases in the south (near the back cluster / starts),
    and an enemy base at the front north to anchor the enemy line.

    (x, y, owner, armour, shells, mines)
    """
    bases = [
        (124, 156, FRIENDLY_OWNERS[0], 90, 90, 90),  # friendly SW refuel (deep back)
        (132, 156, FRIENDLY_OWNERS[1], 90, 90, 90),  # friendly SE refuel (deep back)
        (128, 130, ENEMY_OWNER,        90, 90, 90),  # enemy front base, just behind its pills
    ]
    return bases


def make_starts():
    """Player starts. Friendlies (0,1,2) spawn in the south among their back
    cluster, facing north (dir 0) toward the front. The enemy (3) spawns north,
    facing south (dir 128). Direction: 0=N, 4=E, 8=S(=128? see below).

    Bolo direction is a byte 0-255 (0=N, 64=E, 128=S, 192=W); the start record
    stores it directly. Friendlies face N to push the line; enemy faces S.
    """
    starts = [
        (122, 150, 0),    # bot 0: south (behind the back cluster), facing N
        (128, 150, 0),    # bot 1: south, facing N
        (134, 150, 0),    # bot 2: south, facing N
    ]
    return starts


def write_bmap(path, terrain, pills, bases, starts):
    with open(path, 'wb') as f:
        f.write(b'BMAPBOLO')
        f.write(struct.pack('B', 1))  # version 1
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

    n_friend_pills = sum(1 for p in pills if p[2] in FRIENDLY_OWNERS)
    n_enemy_pills = sum(1 for p in pills if p[2] == ENEMY_OWNER)
    print(f"Wrote {path} ({Path(path).stat().st_size} bytes)")
    print(f"  pills: {len(pills)} total = {n_friend_pills} friendly (back cluster) "
          f"+ {n_enemy_pills} enemy (front row)")
    print(f"  bases: {len(bases)} ({sum(1 for b in bases if b[2] in FRIENDLY_OWNERS)} friendly, "
          f"{sum(1 for b in bases if b[2] == ENEMY_OWNER)} enemy)")
    print(f"  starts: {len(starts)} (friendly, south)")
    print(f"  Run with: -bots 3 -allybots 1 -brain brains/GoalHunter_1.7/init.lua")
    print(f"  (enemy pills/base are static team-2, owner slot {ENEMY_OWNER}, no bot)")


def main():
    output = (sys.argv[1] if len(sys.argv) > 1
              else str(Path(__file__).parent / "reposition_arena.map"))
    terrain = make_map()
    write_bmap(output, terrain, make_pills(), make_bases(), make_starts())


if __name__ == '__main__':
    main()
