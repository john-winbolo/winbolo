#!/usr/bin/env python3
"""
Generate the capture-target LGM-PRIORITY arenas (companion to
tests/capture_lgm_priority_test.py).

WHAT IS BEING MEASURED
----------------------
Andrew's alternative to the CAPTURE_LGM_HUNT sweep (constants.lua
CAPTURE_LGM_PRIORITY): instead of blending the turret onto a builder while the
goal stays capture_pill, leave steering alone entirely and change WHICH GOAL
WINS.  Any hostile LGM standing within CAPTURE_LGM_PRIORITY_RADIUS tiles
(CHEBYSHEV) of the CAPTURE TARGET PILL gets his kill_lgm pool row multiplied by
CAPTURE_LGM_PRIORITY_MULT, so kill_lgm outbids capture_pill, the bot goes and
shoots him, and ordinary pool competition then resumes.

THE ARENA IS THE HUNT ARENA, MOVED APART
----------------------------------------
tests/generate_capture_lgm_hunt_map.py's field, spawns, base, spare pills,
scripted enemy and scenario sidecar are all reused verbatim (imported, not
copied) -- the ONE change is where the corpse lies:

    hunt arena      corpse (126,126), errands (128,126)/(128,125)  -> cheb 2
    priority arena  corpse (125,126), errands (128,126)/(128,125)  -> cheb 3

WHY THREE AND NOT SIX.  Three is the largest gap at which the default
CAPTURE_LGM_PRIORITY_MULT (0.333) can still win the pool, and that is a
property of the OTHER end of the comparison, not of this option:

  * capture_pill for a dead pill within IMMINENT_CAPTURE_PATH_COST (30) of the
    tank is floored at IMMINENT_CAPTURE_FLOOR = 5 whatever the distance, so in
    an arena like this one it costs a flat 5 from the moment the bot sees it;
  * kill_lgm's cheapest branch is the LOS one, TANK_COMBAT_LOS_BASE_COST(5) +
    dist x TANK_COMBAT_LOS_COST_PER_TILE(3).  Times 0.333 that is under 5 only
    while dist <= 3.  (The standoff branch starts at KILL_LGM_BASE_COST 20,
    i.e. 6.7 discounted -- it can never beat an imminent capture at all.)

So a builder six tiles off the corpse is priced out no matter what, and the
arena has to put him where the option has something to win.  Measured: at cheb
6 the P1 and P0 goal series were IDENTICAL tick for tick; at cheb 3 they
diverge.  Three tiles is also still OUTSIDE CAPTURE_LGM_HUNT_RADIUS (2), which
is what keeps the two features apart -- the sweep's LGM trigger cannot fire
here, and the sweep's other trigger, the target pill's armour going UP, never
fires either because the scripted enemy only paves a road and chops a tree, he
never repairs the pill.  The man's whole walk stays at cheb >= 3 (he goes
straight down x=128 from the park at y=121 to y=126, so his Chebyshev distance
to (125,126) is max(3, |y-126|) and never less than 3), so the sweep is inert
by construction and the test asserts zero CAPTURE_LGM_HUNT lines in every
variant to prove it rather than assume it.

The builder is on the FAR SIDE of the corpse from p0, so the bot has to drive
past the free pill to reach him -- exactly the trade the option exists to make.

THREE ARENAS.  Same field, same enemy, same sidecar; only the -bot-init tokens
differ (see the test).

  P1  sweep OFF, priority ON   -- the measurement
  P0  priority OFF             -- the control
  PK  preset=keel              -- the baseline

GEOMETRY
--------
    x:      106 .. 110 ......... 125 .. 128 ........ 146
    y=116                              (128,116) p1 start pond
    y=120   (110,120) p0 base
    y=121                              (128,121) P1_PARK
    y=125                              (128,125) FARM_TILE (forest)
    y=126   (110,126) p0 pond   (125,126) CORPSE   (128,126) ROAD_TILE
    y=136
    everything else GRASS

IMPORTANT: mapRead RECENTERS off-centre maps (bolo_map.c) -- the terrain
bounding box midpoint is shifted to (126,126).  The field is inherited from the
hunt arena unchanged and is symmetric about both axes, so the shift is a no-op
and in-game coordinates match this file.  main() asserts it.

Usage:
    python3 tests/generate_capture_lgm_priority_map.py [P1|P0|PK] [output_path]
    Default: P1 -> tests/capture_lgm_priority_P1.map
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# Everything the hunt arena already owns.  The sidecar templates are imported
# too: this arena wants the SAME refilling-corpse machinery, and a second copy
# of two hundred lines of it would just drift.
from generate_capture_lgm_hunt_map import (          # noqa: E402
    MAP_SIZE, DEEP_SEA, GRASS, FOREST, FIELD,
    ROAD_TILE, FARM_TILE, P1_PARK, SPAWN0, SPAWN1, BASE0, SPARES,
    HUNT_RADIUS, SHOOT_RANGE, OPP_TANK_RANGE, OPP_TANK_AIM,
    RESPAWN_AWAY, RESPAWN_MAX_WAIT,
    SIDECAR_HEAD, SIDECAR_REFILL, SIDECAR_TAIL,
    encode_map_runs, cheb, mdist, _lua_pairs)

VARIANTS = ("P1", "P0", "PK")

# THE ONE NUMBER THIS ARENA CHANGES.  Three tiles west of the errand tiles.
CORPSE = (122, 126)   # cheb 6 from the errands: the CAP wins here, the old x0.333 did not

# ── Brain constants this arena is designed against (constants.lua) ───────
PRIO_RADIUS = 8                   # CAPTURE_LGM_PRIORITY_RADIUS (Chebyshev)
PRIO_CAP = 4                      # CAPTURE_LGM_PRIORITY_MAX_COST (cap, one under IMMINENT_CAPTURE_FLOOR)


def nbots(variant):
    """Every variant needs the scripted enemy and his man."""
    return 2


def make_map():
    """The hunt arena's field, unchanged."""
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y0, y1 = FIELD
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            t[yy][xx] = GRASS
    t[FARM_TILE[1]][FARM_TILE[0]] = FOREST
    # Start squares must be deep sea (starts.c startsIsValidSquare).  The
    # sidecar fills each pond once its tank is ashore.
    t[SPAWN0[1]][SPAWN0[0]] = DEEP_SEA
    t[SPAWN1[1]][SPAWN1[0]] = DEEP_SEA
    return t


def main():
    args = list(sys.argv[1:])
    variant = "P1"
    if args and args[0].upper() in VARIANTS:
        variant = args.pop(0).upper()
    output = args[0] if args else str(
        Path(__file__).parent / f"capture_lgm_priority_{variant}.map")
    terrain = make_map()

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    assert terrain[SPAWN0[1]][SPAWN0[0]] is DEEP_SEA, (
        "the start square must be deep sea (starts.c startsIsValidSquare)")

    # THE ARENA'S WHOLE POINT: both errand tiles are inside the PRIORITY box
    # and outside the SWEEP box, so the two features cannot be confused.
    for tile, what in ((ROAD_TILE, "ROAD_TILE"), (FARM_TILE, "FARM_TILE")):
        d = cheb(tile, CORPSE)
        assert d <= PRIO_RADIUS - 2, (
            f"{what} {tile} is Chebyshev {d} from the corpse {CORPSE}; it must "
            f"be at most CAPTURE_LGM_PRIORITY_RADIUS ({PRIO_RADIUS}) minus two "
            "tiles of margin, or the man drifting mid-walk leaves the box")
        # (the old multiplier-edge assert lived here; a CAP has no such edge)
        assert d > HUNT_RADIUS, (
            f"{what} {tile} is Chebyshev {d} from the corpse {CORPSE}, INSIDE "
            f"CAPTURE_LGM_HUNT_RADIUS ({HUNT_RADIUS}) -- the sweep would fire "
            "too and the arena could no longer attribute anything to the "
            "priority option alone")
        assert tile != CORPSE, "an errand tile cannot be the corpse tile"
    assert terrain[ROAD_TILE[1]][ROAD_TILE[0]] is GRASS, (
        "ROAD_TILE must be GRASS: the C-side LGM scan hides a tree-covered man "
        "more than 3 tiles out, and this arena needs him visible")
    assert terrain[FARM_TILE[1]][FARM_TILE[0]] is FOREST, (
        "FARM_TILE must be FOREST or there is nothing to chop")
    # The builder is on the FAR side of the corpse from p0: driving past the
    # free pill to reach him is the trade being measured.
    assert P1_PARK[0] > CORPSE[0] > SPAWN0[0], (
        "the enemy must park on the far side of the corpse from p0")
    # THE ENEMY TANK MUST BE OFF THE FIRING LINE -- same reasoning (and the
    # same numbers) as the hunt arena: steering.lua's navigate-time
    # OPPORTUNISTIC shot fires at any enemy tank within OPP_TANK_RANGE tiles
    # whose bearing is within OPP_TANK_AIM bradians of where we point, and it
    # runs BEFORE init.lua's kill-LGM block, so it would steal KEY_SHOOT.
    import math as _m
    # The spots checked are the approach lane and the standoff ring the bot
    # actually occupies while shooting at the man (KILL_LGM_SHOOT_RANGE 8 less
    # KILL_LGM_NAV_INSET 3 = a nav target ~5 tiles off him, i.e. x ~= 123).
    # The errand tile itself is NOT checked: standing on top of the man the
    # bearing to him is undefined, and the bot never parks there.
    for spot in (CORPSE, (CORPSE[0] - 2, CORPSE[1]),
                 (CORPSE[0] + 1, CORPSE[1]), (CORPSE[0] + 2, CORPSE[1]),
                 (CORPSE[0] + 4, CORPSE[1]), (CORPSE[0] + 5, CORPSE[1])):
        d = ((P1_PARK[0] - spot[0]) ** 2 + (P1_PARK[1] - spot[1]) ** 2) ** 0.5
        b_lgm = _m.atan2(ROAD_TILE[0] - spot[0], -(ROAD_TILE[1] - spot[1]))
        b_tnk = _m.atan2(P1_PARK[0] - spot[0], -(P1_PARK[1] - spot[1]))
        sep = abs((b_lgm - b_tnk) * 128.0 / _m.pi)
        if sep > 128:
            sep = 256 - sep
        assert d > OPP_TANK_RANGE or sep > OPP_TANK_AIM + 4, (
            f"from {spot} the enemy tank at {P1_PARK} is {d:.1f} tiles away "
            f"and only {sep:.0f} bradians off the bearing to the errand tile "
            f"{ROAD_TILE} - inside the navigate drive-by shot's "
            f"{OPP_TANK_RANGE}-tile / {OPP_TANK_AIM}-bradian window, which "
            "would take KEY_SHOOT before the kill-LGM block ever sees it")
    # p0 must start well outside shooting range so the approach is real, and
    # outside the priority box so the discount is not on from tick one.
    assert cheb(SPAWN0, ROAD_TILE) > SHOOT_RANGE, (
        f"p0 start {SPAWN0} is only Chebyshev {cheb(SPAWN0, ROAD_TILE)} from "
        f"the errand tile - inside KILL_LGM_SHOOT_RANGE {SHOOT_RANGE}")
    assert cheb(SPAWN0, CORPSE) > 4, (
        f"p0 start {SPAWN0} is on top of the corpse {CORPSE}; there is no "
        "approach to measure")
    for s in SPARES:
        assert terrain[s[1]][s[0]] is not DEEP_SEA, f"spare {s} needs land"
        assert tuple(s) != CORPSE, "a spare cannot sit on the corpse tile"

    # Pill records: x, y, owner, armour, speed.  armour 0 = DEAD (capturable).
    pills = [(CORPSE[0], CORPSE[1], 0, 0, 100)]
    pills += [(s[0], s[1], 0, 0, 100) for s in SPARES]
    bases = [(BASE0[0], BASE0[1], 0, 90, 90, 90)]
    starts = [(SPAWN0[0], SPAWN0[1], 2),           # dir 2 = east, at the corpse
              (SPAWN1[0], SPAWN1[1], 6)]           # dir 6 = west, at the park

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

    write_sidecar(variant, Path(output).with_suffix(".scenario.lua"))

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  variant {variant}: corpse {CORPSE}, errands {ROAD_TILE}/"
          f"{FARM_TILE} (cheb {cheb(ROAD_TILE, CORPSE)}/"
          f"{cheb(FARM_TILE, CORPSE)} from the corpse), "
          f"{len(SPARES)} spare(s), {nbots(variant)} tank(s)")
    print(f"  p0 start {SPAWN0} ({mdist(SPAWN0, CORPSE)} tiles from the "
          f"corpse), enemy parks {P1_PARK}")


def write_sidecar(variant, path):
    """The hunt arena's sidecar, with this arena's corpse tile.

    Templates come straight from generate_capture_lgm_hunt_map; only the
    numbers change.  PMODE/PULSE are the armour-pulse machinery, which is an
    H2-only thing and stays off here.  The final replace renames the trace file
    (and the comments naming it) so the two suites cannot fight over one log.
    """
    head = SIDECAR_HEAD.format(
        V=variant, NBOTS=nbots(variant),
        PULSE=0, PMODE="false", HOLD=0, PERIOD=0,
        AWAY=RESPAWN_AWAY, MAXWAIT=RESPAWN_MAX_WAIT,
        CX=CORPSE[0], CY=CORPSE[1],
        RX=ROAD_TILE[0], RY=ROAD_TILE[1],
        FX=FARM_TILE[0], FY=FARM_TILE[1],
        S0X=SPAWN0[0], S0Y=SPAWN0[1],
        S1X=SPAWN1[0], S1Y=SPAWN1[1],
        SPARES=_lua_pairs(SPARES))
    text = (head + SIDECAR_REFILL + SIDECAR_TAIL).replace(
        "capture_lgm_hunt_", "capture_lgm_priority_").replace(
        "generate_capture_lgm_hunt_map.py",
        "generate_capture_lgm_priority_map.py")
    path.write_text(text, encoding="utf-8", newline="\n")


if __name__ == '__main__':
    main()
