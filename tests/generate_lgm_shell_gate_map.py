#!/usr/bin/env python3
"""
Generate the LGM shell-gate arenas (companion to tests/lgm_shell_gate_test.py).

THE RULE UNDER TEST (author's, 2026-09-06, verbatim)
----------------------------------------------------
"if we know we can predict shells for at most 63 ticks, let's do that, and if
any shell will kill our builder (predict the builder for 63 ticks also) then we
should HARD STOP sending it out right then. shells and builder are very
predictable so this is worth doing. I realize a tank can impact reality quicker
than 63 ticks but it's a good start."

So at the moment the builder pool would dispatch, danger.lgm_shell_gate flies
every shell already in the air forward up to LGM_SHELL_PREDICT_TICKS (63 ENGINE
ticks, the longest a shell can live) and walks the man forward beside it on the
SAME walk sim the trip is priced with, and refuses the tick outright if the
engine's own LGM kill rule (lgm.c lgmDeathCheckAtPosition) would fire.

WHAT AN ARENA FOR THIS HAS TO CONTAIN
-------------------------------------
A shell flying THROUGH the man does not touch him -- the engine only looks at
the LGM when a shell EXPLODES (shells.c calls lgmDeathCheck on the collision
path and on the end-of-life path, nowhere else). So the arena has to put an
EXPLOSION where the man is, not merely a trajectory across him. The explosion
that happens by itself, over and over, with no scripting at all, is a round
aimed at OUR TANK landing on the hull while the man is beside it: the pool
sends him out OF the tank, and the tank is exactly where the shells are going.

    x:      121 122 123 124 125 126 127 128 129 130 131
    y=122    ##  ##  ##  ##  ##  ##  ##  ##  ##  ##  ##
    y=123    ##  ##  ##  ##  ##  ##  ##  ##  ##  ##  ##
    y=124    ##  ##  ##  ##  ##  ##  ##  ##  ##  ##  ##
    y=125    ##  ##  ##  ##  ##   .   p   .  ##  ##  ##
    y=126    ##  ##  ##  NP   ~   .   .   S  ##  ##  ##
    y=127    ##  ##  ##  ##  ##   .   B   .  ##  ##  ##
    y=128    ##  ##  ##  ##  ##  ##  ##  ##  ##  ##  ##
    y=129    ##  ##  ##  ##  ##  ##  ##  ##  ##  ##  ##
    y=130    ##  ##  ##  ##  ##  ##  ##  ##  ##  ##  ##

    ## BUILDING wall   ~ deep sea (the moat)   . grass
    NP neutral pillbox   p OUR damaged pill    S spawn pond   B our base

  A SEALED 3x3 ROOM, WALLED THREE DEEP. Every open-corridor draft of this arena
  failed the same way: the bot has no goal it can afford, so it EXPLORES, and
  exploring took it onto the water and twelve tiles up the map on a boat. Once
  it is out there every pool row reads `unreachable` and the arena measures
  nothing. BUILDING is the one terrain a tank cannot enter, so the room is made
  of it -- and THREE tiles deep, because a shell that misses the tank chews a
  wall tile down a stage at a time (shellsCalcCollision's BUILDING case), and a
  one-tile wall was breached inside a single run.

  NP  (124,126) a NEUTRAL pillbox in its own walled cell -- neutral, so it
      shoots everyone and the arena needs no second bot. 3 to 4.13 tiles from
      every tile of the room, well inside PILLBOX_RANGE (2048 WU = 8 tiles), so
      it fires all run wherever the tank sits. One column of DEEP SEA keeps our
      tank from driving over to it, and its own walls keep it from being
      approached any other way.
  S   (128,126) the spawn pond -- a start square must be DEEP SEA (starts.c
      startsIsValidSquare). The sidecar fills it back to grass once the tank is
      ashore: the LGM walk sim walks a straight line, and a hole in that line
      makes the target read `unreachable`.
  p   (127,125) OUR PILL, and it is ALIVE AND DAMAGED rather than a corpse.
      Three things follow from that, and all three are why it is not a corpse:
        * a corpse gets PICKED UP. In an earlier draft the tank drove over its
          own dead pill on the way past, put it in the tank, and the arena's
          only errand vanished at t=2010 with `in_tank 1` in the trace. A live
          pillbox is impassable to a tank, so it cannot be collected;
        * a TOP-UP costs ONE tree (BUILDER_POOL_TREES_TOPUP), where a rebuild
          costs four -- and the tank's 40 trees are the hard cap on how many
          errands a run can contain;
        * the walk sim reaches it through its blessed-tile exception, which is
          the same path M.lgm_trip prices every repair with.
      It is OFF the y=126 fire lane, because a LIVE pill DOES stop a shell
      (pillsIsPillHit) and one sitting in the lane would shield the tank from
      the very rounds the arena is about.
  B   (127,127) our base, full stock, also off the lane. It keeps the tank's
      armour up so a 30-second shelling does not simply kill it, and it is two
      tiles from p, inside PILL_FIRE_RANGE (8), so our own reposition pool does
      not decide the pill is badly placed and shoot it down.

      A base ON the lane would have been harmless, as it happens -- basesCanHit
      (bases.c 1095) returns FALSE outright when the shooter is NEUTRAL, so our
      pillbox's rounds fly straight over our base. The gate makes the same call
      for the same reason. It is off the lane anyway, so the arena does not
      depend on that.

HOW OFTEN THE GATE IS EVEN ASKED. It runs on the row the pool is ABOUT to
dispatch -- after every other test has passed -- so left alone it is consulted
ONCE PER ERRAND. With a round landing every NP_SPEED (50) engine ticks and a
refusal window of about 15 ticks (the round ends within TANK_HIT_RADIUS, 112
WU, of the hull and kills within MAP_SQUARE_MIDDLE, 128 WU, of where it ends;
the man clears that at LGM grass speed, 16 WU/tick), a single errand is a coin
toss. That is why the sidecar knocks the repaired pill straight back down to
TOPUP_ARMOUR: the run becomes dozens of dispatch decisions instead of one, and
"the gate never fired" stops being a plausible innocent outcome. It is a
harness for the NUMBER of decisions and nothing else -- the gate sees exactly
the shells the engine has in the air, either way.

TWO RUNS, ONE TOKEN APART
  A   the gate on (its default). Expect BP_DENY reason=shell_will_hit while a
      round is about to land, and dispatches on the clear ticks between.
  B   cfg=BUILDER_POOL_SHELL_GATE=false, same ground. `shell_will_hit` must
      never appear, which is what makes A's refusals evidence about the rule
      rather than about an arena that was never dangerous.

MAP FACTS THAT BITE (the list generate_repair_priority_map.py keeps, same
reasons -- each one cost somebody a run):
  * mapRead recenters the terrain bounding-box midpoint to (126,126) -- and the
    WALLS are terrain, so they count toward the box. The generator asserts its
    own midpoint is already there and nothing shifts.
  * a start square must be DEEP SEA -- the pond.
  * mapRead puts ROAD under every map-file pill.
  * -gametype open starts a tank with TANK_FULL_TREES (40).
  * EVERY ALIVE friendly pill needs a friendly base within PILL_FIRE_RANGE (8)
    or the reposition pool shoots our own pill down to move it.

Usage:
    python3 tests/generate_lgm_shell_gate_map.py [variant] [output_path]
    variant: A | B   (default: both)
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

BUILDING = 0
GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256
PILLS_MAX_HEALTH = 15

# Engine constants this geometry is built on, mirrored so the test file can read
# them from one place.
PILLBOX_RANGE_TILES = 8.0        # pillbox.h PILLBOX_RANGE 2048 WU
PILL_FIRE_RANGE = 8              # reposition guard
SHELL_TICKS_PER_TILE = 8         # 256 WU / SHELL_SPEED(32)
KILL_RADIUS_WU = 128             # global.h MAP_SQUARE_MIDDLE
TANK_HIT_RADIUS_WU = 112         # tank.h TANK_HIT_RADIUS
LGM_SPEED_GRASS = 16             # brain_pathfinder.c lgm_man_speed, WU/tick
TOPUP_MIN_MISSING = 4            # constants.lua BUILDER_POOL_TOPUP_MIN_MISSING
PILL_REPAIR_AMOUNT = 4           # constants.lua PILL_REPAIR_AMOUNT

LANE_Y = 126                     # the fire lane row
OUTER = (121, 131, 122, 130)     # x0, x1, y0, y1 -- the whole walled block
ROOM = (126, 128, 125, 127)      # the 3x3 grass interior
WALL_DEEP = 3                    # tiles of BUILDING on every side of the room
NP = (124, LANE_Y)               # the neutral pillbox, in its own cell
NP_SPEED = 50                    # reload threshold, ENGINE ticks
MOAT = (125, LANE_Y)             # one tile of deep sea between pill and room
SPAWN = (128, LANE_Y)            # the pond, at the room's east end of the lane
OURPILL = (127, 125)             # OUR alive-but-damaged pill, off the lane
TOPUP_ARMOUR = PILLS_MAX_HEALTH - TOPUP_MIN_MISSING   # 11: missing exactly 4
BASE = (127, 127)                # off the lane

VARIANTS = ("A", "B")


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def fill(t, box, tile=GRASS):
    x0, x1, y0, y1 = box
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            t[yy][xx] = tile


def euclid(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def build(_variant):
    """Both variants share the ground; they differ only in the -bot-init token."""
    t = blank()
    fill(t, OUTER, BUILDING)          # one solid block...
    fill(t, ROOM, GRASS)              # ...hollowed into a 3x3 room,
    t[NP[1]][NP[0]] = GRASS           # a one-tile cell for the neutral pill,
    t[MOAT[1]][MOAT[0]] = DEEP_SEA    # the moat between the two,
    t[SPAWN[1]][SPAWN[0]] = DEEP_SEA  # and the spawn pond.
    # (x, y, owner, armour, speed). The neutral is written as a slot-0 pill and
    # handed to game.NEUTRAL by the sidecar in on_setup: the .map owner byte has
    # no neutral encoding mapRead is guaranteed to keep, and the sidecar has to
    # run anyway.
    pills = [(OURPILL[0], OURPILL[1], 0, TOPUP_ARMOUR, 50),
             (NP[0], NP[1], 0, PILLS_MAX_HEALTH, NP_SPEED)]
    bases = [(BASE[0], BASE[1], 0, 90, 90, 90)]
    starts = [(SPAWN[0], SPAWN[1], 12)]        # facing west, down the lane
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

    # THE ROOM IS SEALED, AND THE SEAL IS THREE DEEP. Everything outside the
    # room, the pill's cell and the moat has to be BUILDING, and the nearest
    # open ground beyond a wall has to be WALL_DEEP tiles away: a shell that
    # misses the tank knocks a wall tile down a stage at a time, and a
    # single-tile wall was breached inside one run.
    rx0, rx1, ry0, ry1 = ROOM
    open_tiles = {(xx, yy) for yy in range(ry0, ry1 + 1)
                  for xx in range(rx0, rx1 + 1)} | {NP, MOAT}
    for (xx, yy) in open_tiles:
        for (nx, ny) in ((xx - 1, yy), (xx + 1, yy), (xx, yy - 1), (xx, yy + 1)):
            if (nx, ny) in open_tiles:
                continue
            assert terrain[ny][nx] == BUILDING, (
                f"the room leaks at ({nx},{ny}) -- every neighbour of open "
                f"ground must be wall (or the moat / the pill's cell)")
    x0, x1, y0, y1 = OUTER
    for (xx, yy) in open_tiles:
        assert (xx - x0 >= WALL_DEEP and x1 - xx >= WALL_DEEP
                and yy - y0 >= WALL_DEEP and y1 - yy >= WALL_DEEP), (
            f"open tile ({xx},{yy}) is less than {WALL_DEEP} tiles from the "
            f"edge of the block -- shells will chew through and the bot will "
            f"go exploring on a boat")

    # The neutral has to be able to shoot the tank from EVERY tile of the room.
    worst = max(euclid(NP, (xx, yy))
                for yy in range(ry0, ry1 + 1) for xx in range(rx0, rx1 + 1))
    assert worst <= PILLBOX_RANGE_TILES, (
        f"the farthest room tile is {worst:.2f} tiles from the neutral pill, "
        f"past PILLBOX_RANGE ({PILLBOX_RANGE_TILES}) -- the tank could park "
        f"there and never be shot at")

    # ...and the fire lane has to be clear of the things that stop a shell.
    assert NP[1] == MOAT[1] == SPAWN[1] == LANE_Y, (
        "the pill, the moat and the spawn must share the lane row")
    assert OURPILL[1] != LANE_Y, (
        f"our pill at {OURPILL} is on the y={LANE_Y} fire lane -- a LIVE pill "
        f"stops any shell (pillsIsPillHit ignores the owner) and would shield "
        f"the tank from the rounds this arena is about")
    assert BASE[1] != LANE_Y, (
        f"the base at {BASE} is on the fire lane. Harmless as it happens "
        f"(basesCanHit is FALSE for a NEUTRAL shooter) but the arena should "
        f"not depend on that")
    for xx in range(NP[0] + 1, SPAWN[0] + 1):
        tt = terrain[LANE_Y][xx]
        assert tt is DEEP_SEA or tt == GRASS, (
            f"({xx},{LANE_Y}) is on the fire lane and is not something a shell "
            f"flies over")

    # OUR PILL IS ALIVE AND EXACTLY TOPUP_MIN_MISSING DOWN: alive so the tank
    # cannot drive over it and pocket it (an earlier draft lost its corpse to
    # exactly that at sim t=2010), and exactly 4 missing so it is the CHEAPEST
    # row the pool will look at -- one tree an errand against the tank's 40.
    ourp = [p for p in pills if (p[0], p[1]) == OURPILL]
    assert ourp and ourp[0][3] == TOPUP_ARMOUR, "our pill's armour is wrong"
    assert PILLS_MAX_HEALTH - TOPUP_ARMOUR >= TOPUP_MIN_MISSING, (
        f"our pill is only {PILLS_MAX_HEALTH - TOPUP_ARMOUR} armour down, under "
        f"BUILDER_POOL_TOPUP_MIN_MISSING ({TOPUP_MIN_MISSING}) -- the pool "
        f"would not offer a row for it at all")
    trees_per_errand = max(1, -(-(PILLS_MAX_HEALTH - TOPUP_ARMOUR)
                                // PILL_REPAIR_AMOUNT))
    assert trees_per_errand == 1, (
        f"an errand costs {trees_per_errand} trees, so the tank's 40 buy only "
        f"{40 // trees_per_errand} of them -- damage the pill less")
    assert euclid(BASE, OURPILL) <= PILL_FIRE_RANGE, (
        f"our alive pill is {euclid(BASE, OURPILL):.2f} tiles from the only "
        f"base -- outside PILL_FIRE_RANGE ({PILL_FIRE_RANGE}), so the "
        f"reposition pool would shoot it down to move it")

    # THE REFUSAL WINDOW, AS ARITHMETIC (the header's paragraph, asserted). The
    # round ends within TANK_HIT_RADIUS of the hull and kills within
    # KILL_RADIUS_WU of that, so the man is in danger until he is (112 + 128) WU
    # out, which at LGM grass speed is:
    exposure = (TANK_HIT_RADIUS_WU + KILL_RADIUS_WU) // LGM_SPEED_GRASS
    tof = (SPAWN[0] - NP[0]) * SHELL_TICKS_PER_TILE
    assert tof >= exposure, (
        f"time of flight from the pill to the tank is {tof} ticks against a "
        f"{exposure}-tick exposure -- every shell would be inside the window "
        f"from the moment it is fired and there would never be a clear tick to "
        f"dispatch on")
    assert NP_SPEED > exposure, (
        f"the pill reloads every {NP_SPEED} ticks against a {exposure}-tick "
        f"exposure -- the gate would refuse every tick and the man would never "
        f"go at all")

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
               else str(here / f"lgm_shell_gate_{v}.map"))
        write(v, out)


if __name__ == '__main__':
    main()
