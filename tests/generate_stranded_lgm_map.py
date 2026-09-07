#!/usr/bin/env python3
"""
Generate the stranded-LGM arenas (companion to tests/stranded_lgm_test.py).

THE FREEZE THIS ARENA IS BUILT AROUND (winbolo2, 2026-09-07)
------------------------------------------------------------
A 1v1 on Everard Island: the 1.7 bot sat on tile (85,140) from engine tick
10899 to the end of a 180000-tick game -- 87510 engine ticks without moving a
tile, alive the whole time, armour never changing. Three things had to be true
at once, and this arena reproduces all three:

  1  THE MAN IS STRANDED. He walked out on a builder-pool errand and ended up
     somewhere his walk home cannot reach. The engine's LGM return
     (lgm.c lgmReturn) walks a STRAIGHT LINE at the tank with a per-axis slide
     and has no re-route: a barrier across that line stops him for good. The
     brain's own walk sim (brainPathfinderLgmTravelTicks, the same straight
     line) returns -1, so goals.lua sets state.lgm_stranded.

  2  attack_pill HOLDS IN plan_position WAITING FOR HIM. attack.lua's
     plan_position hold is "the LGM will return and we resume" -- no timeout
     and, before this fix, no stranded check. The tank does not move a tile
     while it holds.

  3  rescue_lgm IS SUPPRESSED BY A FIELD THAT ONLY MOVING CAN CLEAR. The
     rescue's fire gate was perc.under_fire, which is
     danger.danger_at(our tile) > 0 -- the STATIC pill-danger stamp. A CALM
     hostile/neutral pill stamps PILL_DANGER_BASE (8) over a disk of
     PILL_RANGE_MAP (9) tiles whether or not it ever fires, so a tank parked
     inside that disk reads "under fire" for ever, in total silence.

  Deadlock: the rescue waits for the field to clear, the field clears only if
  the tank moves, the tank's only goal waits for the man, the man waits for the
  tank.

HOW THE GROUND MAKES EACH OF THE THREE HAPPEN, DELIBERATELY
-----------------------------------------------------------

    x:       117 .. 121  122 123 124 125 126 127  128  129  130 .. 135
    y=117 .. 121   every tile BUILDING (five deep)
    y=122     ##    ##   BB  ff  ff  ff  ff  ff   GG   ##   ##    ##
    y=123     ##    ##   ff  ff  ff  ff  ff  ff   GG   pp   ##    ##
    y=124     ##    ##   ff  ff  ~~  ff  ff  ff   GG   ##   ##    ##
    y=125     ##    ##   ##  ##  ##  ##  ff  ##   ##   ##   ##    ##
    y=126     ##    ##   ##  ##  ##  ##  ff  ##   ##   ##   ##    ##
    y=127     ##    ##   ##  ##  ##  ##  ff  ##   ##   ##   ##    ##
    y=128     ##    ##   ##  ##  ##  ##  ff  ##   ##   ##   ##    ##
    y=129     ##    ##   ##  ##  ##  ##  ff  ##   ##   ##   ##    ##
    y=130     ##    ##   ##  ##  ##  ##  NP  ##   ##   ##   ##    ##
    y=131 .. 135   every tile BUILDING (five deep)

    ## BUILDING wall     ff FOREST      ~~ spawn pond (deep sea -> forest)
    GG THE GATE column (forest at load, RIVER once the man is across)
    pp OUR pill, alive and 4 armour down (the errand)     BB our base
    NP the NEUTRAL pillbox, at the end of its own corridor

  EVERY TILE THE TANK CAN STAND ON IS FOREST, and that is the whole trick
  behind (3). pillbox.c:425 skips a target that utilIsTankInTrees() and is at
  least MIN_TREEHIDE_DIST (768 WU = 3 tiles) away on either axis, unless it
  JUST FIRED. So the neutral pillbox never shoots a tank sitting in the west
  room -- while threat.lua's stamp, which knows nothing about firing, covers
  that room the whole run (base 8, x0.1 for the tree cover = 0.8, and
  danger_at > 0 is all perc.under_fire asks). That is exactly the Everard
  reading: a permanent "under fire" with the armour bar frozen. The fix's
  danger.tank_fire_age, which wants an actual hit or an actual inbound round,
  reads nil here from the first tick to the last.

  THE GATE COLUMN (128, y=122..124) is the barrier for (1), and it is a COLUMN
  rather than a tile because the engine's return walk slides per axis: a
  one-tile hole in the wall is a hole the man walks around. Three tiles tall,
  with BUILDING above and below, is a wall he cannot slide past.

  AND THE ONLY OPEN TILE BEYOND IT IS THE PILL. That is not decoration: a LIVE
  pillbox is impassable to a tank (mapGetSpeed -> MAP_SPEED_TPILLBOX) but not
  to the man, who reaches it through the blessed-tile exception his errand
  gives him. So the TANK CAN NEVER BE EAST OF THE GATE, and flooding the column
  always leaves it on the far side from the man. The first draft had a four-
  tile east room, and in the fixed arena the tank simply followed the rescued
  man into it and stayed there for the whole run; nothing was ever stranded and
  the arena measured nothing.

  THE WALLS ARE FIVE TILES DEEP, not the usual three. A shell that misses
  chews a wall tile down a stage at a time, and this arena's tank spends the
  run trading rounds with a pillbox down a one-tile corridor: the three-deep
  first draft was breached inside 1700 ticks and the tank drove out through the
  hole, which put it on ground with no tree cover -- the one thing the whole
  arena depends on.

  It is FOREST at load -- the man has to be able to walk OUT through it -- and
  the sidecar turns it to RIVER at the moment the engine says the repair
  landed, i.e. the moment the man is provably on the far side. RIVER is the one
  terrain that separates the two: bolo_map.h MAP_MANSPEED_TRIVER is 0 (the man
  cannot enter it at all) while MAP_SPEED_TRIVER is 3 (the TANK can cross,
  slowly, paying the water drain). So the man is stranded and the RESCUE IS
  STILL POSSIBLE -- which is what lets the test assert the whole fix, man back
  in the tank and all, rather than just "the tank twitched".

  OUR PILL (129,123) is ALIVE and exactly BUILDER_POOL_TOPUP_MIN_MISSING (4)
  armour down, for the same two reasons the shell-gate arena gives: a corpse
  gets driven over and pocketed, and a top-up costs ONE tree against a
  rebuild's four.

  OUR BASE (122,122) keeps the reposition pool from shooting our own pill down
  to move it (it wants a friendly base within PILL_FIRE_RANGE of every live
  friendly pill) and keeps the tank fed. It sits in the FAR corner of the west
  room because a base tile is NOT tree cover (utilIsTankInTrees returns FALSE
  on a base or pill tile): it is the one tile a parked tank could be seen from,
  so it is the one tile that has to clear PILLBOX_RANGE outright -- by more
  than the world-position jitter inside a tile, which check() asserts.

  THE NEUTRAL'S CORRIDOR (126, y=125..129) is what keeps attack_pill a real
  candidate -- (2) needs a pill the bot can actually plan a take on. The tank
  never has to walk it: the point of the arena is that it holds in
  plan_position at the top of it.

MAP FACTS THAT BITE (the list every generator in tests/ keeps):
  * mapRead recenters the terrain bounding-box midpoint to (126,126), and the
    WALLS are terrain. check() asserts the midpoint is already there.
  * a start square must be DEEP SEA (starts.c startsIsValidSquare) -- the pond,
    which the sidecar fills back to FOREST once the tank is ashore, because a
    hole in the forest is a hole in the tree cover.
  * mapRead puts ROAD under every map-file pill.
  * -gametype open starts a tank with TANK_FULL_TREES (40).

TWO ARENAS, ONE PAIR OF TOKENS APART (the ground is identical):
  A  the fix on (both knobs at their defaults).
  B  the control, cfg=RESCUE_LGM_SUPPRESS_BY_FIRE_AGE=false and
     cfg=ATTACK_PP_HOLD_SKIP_STRANDED=false -- i.e. KEEL, the behaviour that
     froze on Everard.

Usage:
    python3 tests/generate_stranded_lgm_map.py [variant] [output_path]
    variant: A | B   (default: both)
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

BUILDING = 0
RIVER = 1
FOREST = 5
GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256
PILLS_MAX_HEALTH = 15

# Engine / brain constants this geometry is built on, mirrored here so the test
# file can read them from one place.
PILLBOX_RANGE_TILES = 8.0        # pillbox.h PILLBOX_RANGE 2048 WU
PILL_RANGE_MAP = 9               # constants.lua: the DANGER STAMP radius
PILL_FIRE_RANGE = 8              # reposition guard
MIN_TREEHIDE_TILES = 3           # tank.h MIN_TREEHIDE_DIST 768 WU
TOPUP_MIN_MISSING = 4            # constants.lua BUILDER_POOL_TOPUP_MIN_MISSING
TOPUP_ARMOUR = PILLS_MAX_HEALTH - TOPUP_MIN_MISSING   # 11

# ── geometry ─────────────────────────────────────────────────────────────
OUTER = (117, 135, 117, 135)     # x0, x1, y0, y1 -- the whole walled block
WALL_DEEP = 5                    # tiles of BUILDING on every side of open ground
WEST_ROOM = (122, 127, 122, 124) # the tank's whole world: 6 x 3, all forest
GATE = [(128, 122), (128, 123), (128, 124)]   # forest -> RIVER, the whole column
OURPILL = (129, 123)             # alive, TOPUP_ARMOUR -- the ONLY tile east of the gate
BASE = (122, 122)
NP = (126, 130)                  # the neutral pillbox
NP_CORRIDOR = [(126, y) for y in range(125, 130)]
NP_SPEED = 50                    # reload, engine ticks (it never gets to use it)
SPAWN = (124, 124)               # the pond, in the west room
# The tank's world position inside a tile moves its distance to anything by up
# to sqrt(0.5^2 + 0.5^2) tiles, and utilIsItemInRange measures WORLD distance.
# Anything that has to be out of the pillbox's reach on every world position
# inside its tile has to clear PILLBOX_RANGE by this much.
TILE_JITTER = 0.708

VARIANTS = ("A", "B")


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def box_tiles(box):
    x0, x1, y0, y1 = box
    return [(x, y) for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)]


def open_tiles():
    """Everything that is not wall, in load order (the gate is forest at load)."""
    return box_tiles(WEST_ROOM) + GATE + [OURPILL] + NP_CORRIDOR + [NP]


def euclid(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def build(_variant):
    """Both arenas share the ground; they differ only in the -bot-init tokens."""
    t = blank()
    for (x, y) in box_tiles(OUTER):
        t[y][x] = BUILDING
    for (x, y) in open_tiles():
        t[y][x] = FOREST
    t[SPAWN[1]][SPAWN[0]] = DEEP_SEA
    # (x, y, owner, armour, speed). The neutral is written as a slot-0 pill and
    # handed to game.NEUTRAL by the sidecar in on_setup: the .map owner byte has
    # no neutral encoding mapRead is guaranteed to keep.
    pills = [(OURPILL[0], OURPILL[1], 0, TOPUP_ARMOUR, 50),
             (NP[0], NP[1], 0, PILLS_MAX_HEALTH, NP_SPEED)]
    bases = [(BASE[0], BASE[1], 0, 90, 90, 90)]
    starts = [(SPAWN[0], SPAWN[1], 4)]         # facing east, into the room
    return t, pills, bases, starts


def check(variant, terrain, pills, bases, starts):
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    mid = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    assert mid == (126, 126), (
        f"variant {variant}: terrain midpoint is {mid}, not (126,126) -- "
        f"mapRead would shift every coordinate in this file")
    for (sx, sy, _d) in starts:
        assert terrain[sy][sx] is DEEP_SEA, (
            f"variant {variant}: start ({sx},{sy}) must be deep sea "
            f"(starts.c startsIsValidSquare)")

    opens = set(open_tiles())
    assert SPAWN in opens, "the spawn pond must be inside the open ground"

    # SEALED, AND SEALED THREE DEEP. Everything touching open ground is wall,
    # and no open tile is within WALL_DEEP of the block's edge -- a shell that
    # misses chews a wall down a stage at a time and a thin wall gets breached
    # inside one run, after which the bot goes exploring and the arena measures
    # nothing.
    for (xx, yy) in opens:
        for (nx, ny) in ((xx - 1, yy), (xx + 1, yy), (xx, yy - 1), (xx, yy + 1)):
            if (nx, ny) in opens:
                continue
            assert terrain[ny][nx] == BUILDING, (
                f"the arena leaks at ({nx},{ny}) -- every neighbour of open "
                f"ground must be wall")
    x0, x1, y0, y1 = OUTER
    for (xx, yy) in opens:
        assert (xx - x0 >= WALL_DEEP and x1 - xx >= WALL_DEEP
                and yy - y0 >= WALL_DEEP and y1 - yy >= WALL_DEEP), (
            f"open tile ({xx},{yy}) is less than {WALL_DEEP} tiles from the "
            f"edge of the block")

    # CONNECTED, at load: the man has to be able to walk out through the gate,
    # and the tank has to be able to reach the neutral's corridor (or
    # attack_pill would never be a candidate and the plan_position hold this
    # arena is about would never happen).
    seen, stack = {SPAWN}, [SPAWN]
    while stack:
        (xx, yy) = stack.pop()
        for n in ((xx - 1, yy), (xx + 1, yy), (xx, yy - 1), (xx, yy + 1)):
            if n in opens and n not in seen:
                seen.add(n)
                stack.append(n)
    assert seen == opens, (
        f"the open ground is not one connected piece at load -- unreachable: "
        f"{sorted(opens - seen)[:8]}")

    # ...AND CUT IN TWO once the sidecar turns the gate to RIVER. That is the
    # stranding, and it has to be a CLEAN cut: the man's return walk slides per
    # axis, so a single missing tile in the column is a way round it.
    after = opens - set(GATE)
    seen, stack = {SPAWN}, [SPAWN]
    while stack:
        (xx, yy) = stack.pop()
        for n in ((xx - 1, yy), (xx + 1, yy), (xx, yy - 1), (xx, yy + 1)):
            if n in after and n not in seen:
                seen.add(n)
                stack.append(n)
    assert OURPILL not in seen, (
        f"flooding the gate column does NOT cut {OURPILL} off -- it is still "
        f"walkable from the tank's side, so the man would simply walk home and "
        f"nothing would ever be stranded")
    gx = {x for (x, _y) in GATE}
    assert len(gx) == 1, "the gate must be a single COLUMN"
    gy = sorted(y for (_x, y) in GATE)
    assert gy == list(range(gy[0], gy[-1] + 1)), "the gate column has a hole in it"

    # THE TANK IS ALWAYS IN TREES, and always far enough away to be hidden.
    # pillbox.c:425 skips a target in trees at MIN_TREEHIDE_DIST or more on
    # EITHER axis, so this is what makes the arena silent. Checked over every
    # tile a tank can park on -- the room AND the gate column, because it can
    # sit in the gate (the measured control froze one tile west of it).
    for (xx, yy) in box_tiles(WEST_ROOM) + GATE:
        if (xx, yy) == SPAWN:
            continue          # deep sea at load; the sidecar fills it to forest
        assert terrain[yy][xx] == FOREST, (
            f"tank-reachable tile ({xx},{yy}) is not FOREST -- the neutral "
            f"would be able to see and shoot a tank parked on it, "
            f"danger.tank_fire_age would run hot and BOTH arenas would "
            f"suppress the rescue")
        assert (abs(xx - NP[0]) >= MIN_TREEHIDE_TILES
                or abs(yy - NP[1]) >= MIN_TREEHIDE_TILES), (
            f"tank-reachable tile ({xx},{yy}) is inside MIN_TREEHIDE_DIST "
            f"({MIN_TREEHIDE_TILES} tiles) of the neutral on both axes -- the "
            f"tree cover would not hide a tank there")
        d = euclid((xx, yy), NP)
        assert d <= PILL_RANGE_MAP, (
            f"tank-reachable tile ({xx},{yy}) is {d:.2f} tiles from the neutral, "
            f"outside PILL_RANGE_MAP ({PILL_RANGE_MAP}) -- a tank parked there "
            f"has NO static danger over it and the control arena would not "
            f"reproduce the freeze")

    # THE BASE IS OUT OF THE PILLBOX'S REACH, ON EVERY WORLD POSITION INSIDE
    # ITS TILE. A base tile is not tree cover (utilIsTankInTrees is FALSE on a
    # base or a pill), so it is the ONE tile in the arena where a parked tank
    # could be seen -- and being seen once starts danger.tank_fire_age, which
    # is the very clock the fix reads.
    assert euclid(BASE, NP) > PILLBOX_RANGE_TILES + TILE_JITTER, (
        f"the base at {BASE} is {euclid(BASE, NP):.2f} tiles from the neutral, "
        f"which is not clear of PILLBOX_RANGE ({PILLBOX_RANGE_TILES}) by the "
        f"{TILE_JITTER}-tile world-position jitter -- a tank sitting on the "
        f"base could be shot, and a base tile is not tree cover")
    assert euclid(BASE, OURPILL) <= PILL_FIRE_RANGE, (
        f"our alive pill is {euclid(BASE, OURPILL):.2f} tiles from the only "
        f"base -- outside PILL_FIRE_RANGE ({PILL_FIRE_RANGE}), so the "
        f"reposition pool would shoot it down to move it")

    # THE ERRAND. Alive (a corpse gets pocketed) and exactly TOPUP_MIN_MISSING
    # down (one tree an errand against the tank's 40), and on the FAR side of
    # the gate, or the man would never be stranded by flooding it.
    ourp = [p for p in pills if (p[0], p[1]) == OURPILL]
    assert ourp and ourp[0][3] == TOPUP_ARMOUR, "our pill's armour is wrong"
    assert PILLS_MAX_HEALTH - TOPUP_ARMOUR >= TOPUP_MIN_MISSING, (
        f"our pill is only {PILLS_MAX_HEALTH - TOPUP_ARMOUR} armour down, under "
        f"BUILDER_POOL_TOPUP_MIN_MISSING ({TOPUP_MIN_MISSING})")
    assert OURPILL[0] > GATE[0][0], (
        f"our pill {OURPILL} is not east of the gate column at x={GATE[0][0]}")
    assert BASE[0] < GATE[0][0], (
        f"the base {BASE} is not WEST of the gate -- it has to be on the "
        f"tank's side, or the flood would cut the tank off from its only "
        f"resupply and the arena would be measuring a starving bot")

    # THE TANK CAN NEVER BE EAST OF THE GATE, which is what makes the flood
    # safe: the only open tile over there is OURPILL, and a LIVE pillbox is
    # impassable to a tank (mapGetSpeed -> MAP_SPEED_TPILLBOX). The man gets
    # there anyway, through the blessed-tile exception his errand gives him.
    # Without this the tank follows the man across, the flood strands nobody,
    # and the arena measures nothing -- which is exactly what the first draft
    # did, with a four-tile east room the tank sat in for a whole run.
    gx = GATE[0][0]
    east_open = [t for t in open_tiles() if t[0] > gx]
    assert east_open == [OURPILL], (
        f"the only open tile east of the gate must be OURPILL, so the tank "
        f"cannot follow the man across; found {east_open}")

    for (px, py, _o, _a, _s) in pills:
        assert terrain[py][px] is not DEEP_SEA, f"pill ({px},{py}) is in the sea"
    for (bx, by, *_r) in bases:
        assert terrain[by][bx] is not DEEP_SEA, f"base ({bx},{by}) is in the sea"


def write(variant, output):
    terrain, pills, bases, starts = build(variant)
    check(variant, terrain, pills, bases, starts)

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
        for x, y, dr in starts:
            f.write(struct.pack('BBB', x, y, dr))
        f.write(encode_map_runs(terrain))
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes) "
          f"[variant {variant}: {len(pills)} pill(s), {len(bases)} base(s), "
          f"{len(starts)} start(s)]")


def main():
    args = sys.argv[1:]
    variants = [args[0]] if args and args[0] in VARIANTS else list(VARIANTS)
    here = Path(__file__).parent
    for v in variants:
        out = (args[1] if len(args) > 1 and len(variants) == 1
               else str(here / f"stranded_lgm_{v}.map"))
        write(v, out)


if __name__ == '__main__':
    main()
