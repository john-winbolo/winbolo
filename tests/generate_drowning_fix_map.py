#!/usr/bin/env python3
"""
Generate the drowning-FIX arenas A3 and A4 (companion to
tests/drowning_fix_test.py).  These are new arenas for the two fixes that came
out of the 2026-09-06 drowning analysis; the older arenas A and B, for the
corner-check and stop-mask knobs, live in generate_drowning_map.py and are
untouched.

  A3  THE NOTCH LEDGE -- for C.CLIFF_BRAKE_STICKY.
      The 20260906_184924_1_drown13 bot0 death at t=29523: full armour, nobody
      near, a cliff brake that fired on 5 of the last 23 ticks and a tank that
      drowned anyway.  The mechanism is that both cliff guards are re-decided
      from scratch every tick off a heading ray, so on the ticks the ray's
      sub-tile jitter misses the sea tile navigate runs normally and returns
      KEY_FASTER -- net forward creep between brakes.

      The ground: arena A's proven two-tile staircase descending south-west
      (that shape is what makes the brake fire at all -- sea due south, land to
      the south-west, tank hugging a column boundary), ending not in a wide pad
      but in a ONE-TILE-WIDE LEDGE running west with deep sea both north AND
      south of every tile, and the map's only base at its tip.  The tank has to
      drive the ledge, and every wobble off due-west puts a sea tile on the
      diagonal.  That is the jitter the sticky latch exists for -- and, just as
      importantly, it is the shape a sticky "no forward throttle" latch could
      DEADLOCK on, which is why the base at the tip is the assertion that
      matters most.

  A4  THE BOAT-SEEDED SHORE -- for C.PF_NEXTSTEP_FOOT_SEA_RULE.
      brainPathfinderDijkstraNextStep's fallbacks pick a neighbour by
      min(g_land, g_boat) with no on-foot passability test, so a boatless tank
      standing next to the water can be handed the sea tile as its next step
      whenever that tile's BOAT-layer cost is lower than the tank's own.

      For that to be possible the boat layer has to be reachable at all, and
      the Dijkstra is seeded AT THE TANK (init.lua:2077, brain_pathfinder.c
      brainPathfinderDijkstraSlateStart) with the tank's own boat state -- so a
      boatless tank only reaches boat nodes through a TT_BOAT tile.  On Oil Rig
      those are everywhere (every death leaves one).  Here they are placed
      deliberately: a line of BOAT tiles in the water one tile off a shore
      corridor.  Sea tiles beside the corridor then carry a boat-layer g of a
      few units while the tank's own land g grows as it walks, which is exactly
      the field shape in which the fallback names deep sea.

      The base is at the SOUTH end of the same landmass, not across the water:
      grass costs 2 and open sea afloat costs 2.5 (cpathfinder.lua
      DEFAULT_TERRAIN_COST / _BOAT), so walking always beats sailing and the
      traced chain stays on land.  The boat tiles are there to seed the field,
      not to offer a route.

MAP FACTS THAT BITE (the list every generator in tests/ keeps):
  * mapRead recenters the terrain bounding-box midpoint to (126,126), so every
    arena is built in convenient coordinates and then SHIFTED by recenter().
  * a start square must be DEEP SEA (starts.c startsIsValidSquare), so both
    arenas bury a one-tile pond in the middle of a 5x5 land block -- all eight
    neighbours land and two tiles of runway beyond them, because the tank comes
    off its boat at speed.  The sidecar fills the pond with grass once the tank
    is ashore, which destroys the boat it left there.
  * there is no RIVER on either map, so the bot can never build a replacement
    boat.  A4's BOAT tiles are the one deliberate exception and they are in the
    open water, not on a river.

Usage:
    python3 tests/generate_drowning_fix_map.py [variant] [output_path]
    variant: A3 | A4   (default: both)
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

GRASS = 7
BOAT = 9                        # constants.lua T_BOAT
DEEP_SEA = None                 # absent from every run == deep sea
MAP_SIZE = 256

VARIANTS = ("A3", "A4")

# ── A3: staircase + one-tile ledge ───────────────────────────────────────
A3_BLOCK = (128, 132, 118, 122)   # spawn block, 5x5 (runway for the landing)
A3_SPAWN = (130, 120)             # dead centre of it
A3_X0, A3_Y0 = 130, 122           # staircase origin; step 0 is inside the block
A3_STEPS = 9                      # last step is {(122,130), (121,130)}
A3_LEDGE_Y = 130
A3_LEDGE_X = (117, 120)           # one tile tall, deep sea north AND south
A3_BASE = (117, 130)              # the tip of the ledge: the only goal

# ── A4: the boat-seeded shore ────────────────────────────────────────────
A4_BLOCK = (110, 120, 104, 112)   # spawn block, 11x9 -- big enough for TWO
                                  # ponds, each with its own two tiles of
                                  # landing runway all round
A4_SPAWNS = ((113, 108),)         # ONE bot; see the two-bot note below
A4_SPAWN = A4_SPAWNS[0]
A4_SHORE = (115, 115, 113, 126)   # ONE column wide -- a ledge with deep sea to
                                  # both east and west.  Three columns was the
                                  # first try and it measured nothing: the
                                  # Dijkstra routed the tank down the INLAND
                                  # column, three tiles from the water, so no
                                  # 8-neighbour of the tank was ever a sea tile
                                  # and the fallback had nothing to reject.
A4_PAD = (114, 116, 127, 129)     # the goal pad at the south end
A4_BASE = (115, 128)
A4_BOAT_X = 116                   # BOAT tiles in the water immediately EAST of
A4_BOAT_YS = (114, 118, 122, 126)  # the ledge.  They have to be cardinally
                                  # adjacent to land or the boat layer is never
                                  # entered at all: deep sea is a wall on the
                                  # LAND layer (cost 9999 >= WALL_THRESHOLD), so
                                  # without a reachable TT_BOAT tile every sea
                                  # tile carries g = COST_INF, the fallback's
                                  # `if (g >= COST_INF) continue` skips it, and
                                  # even the OLD code could not have named it.
                                  # Seeding the boat layer is what makes the
                                  # arena capable of showing the bug at all.
#
# WHAT THIS ARENA STILL DOES NOT DO, MEASURED.  The fallbacks only run when the
# traced chain cannot serve the step: either the tank has drifted off the chain,
# or the chain's next tile is occupied by an ALLY (the live-obstacle veer, fed
# from init.lua's nav_avoid_tiles).  On a map this small the chain essentially
# always serves -- the two configs run to byte-identical traces here, and no
# PF_SEA_VETO is printed by either.  Two things were tried and are written down
# so nobody spends the afternoon again:
#   * a three-column shore corridor: the Dijkstra routed the tank down the
#     INLAND column, so no 8-neighbour of the tank was ever a sea tile;
#   * two allied bots queueing down the one-tile ledge, to force the veer: they
#     killed each other on the spawn block (mines) and never reached the base,
#     which measured the arena and not the knob.
# On the real map the rule is anything but idle: DH-Oil Rig, seed 13, 60000
# ticks, bots 0-1 on the defaults and 2-3 on preset=keel -- 21896 and 12250
# PF_SEA_VETO lines on the two default bots and ZERO on the two keel bots.
# What A4 is therefore for is the OTHER half: proof that turning the rule on
# does not cost a shoreline-walking tank its route, its goal or its life.


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def fill(tiles, box, tile=GRASS):
    x0, x1, y0, y1 = box
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            tiles[(xx, yy)] = tile


def staircase(tiles, x0, y0, steps, tile=GRASS):
    """Step i is the pair {(x0-i, y0+i), (x0-i-1, y0+i)}; consecutive steps
    share a column, so the walk is always a cardinal move and never a squeeze
    between two sea tiles.  Standing on the EAST tile of a pair is the
    incident: sea due south, land to the south-west."""
    for i in range(steps):
        tiles[(x0 - i, y0 + i)] = tile
        tiles[(x0 - i - 1, y0 + i)] = tile


def recenter(tiles, spots):
    """Shift everything so the terrain bbox midpoint is already (126,126)."""
    xs = [x for (x, _y) in tiles]
    ys = [y for (_x, y) in tiles]
    dx = 126 - (min(xs) + max(xs)) // 2
    dy = 126 - (min(ys) + max(ys)) // 2
    return ({(x + dx, y + dy): t for (x, y), t in tiles.items()},
            [(x + dx, y + dy) for (x, y) in spots])


def build(variant):
    tiles = {}
    if variant == "A3":
        fill(tiles, A3_BLOCK)
        staircase(tiles, A3_X0, A3_Y0, A3_STEPS)
        for x in range(A3_LEDGE_X[0], A3_LEDGE_X[1] + 1):
            tiles[(x, A3_LEDGE_Y)] = GRASS
        spots = [A3_SPAWN, A3_BASE]
        tiles, spots = recenter(tiles, spots)
        ponds, base = [spots[0]], spots[1]
        boats = []
        start_dir = 10                       # south-west, the way it must go
    else:
        fill(tiles, A4_BLOCK)
        fill(tiles, A4_SHORE)
        fill(tiles, A4_PAD)
        for by in A4_BOAT_YS:
            tiles[(A4_BOAT_X, by)] = BOAT
        spots = (list(A4_SPAWNS) + [A4_BASE]
                 + [(A4_BOAT_X, by) for by in A4_BOAT_YS])
        tiles, spots = recenter(tiles, spots)
        n = len(A4_SPAWNS)
        ponds = spots[0:n]
        base = spots[n]
        boats = spots[n + 1:]
        start_dir = 8                        # south, down the ledge

    grid = blank()
    for (x, y), t in tiles.items():
        grid[y][x] = t
    for p in ponds:
        grid[p[1]][p[0]] = DEEP_SEA           # the start pond(s)

    bases = [(base[0], base[1], 0, 90, 90, 90)]   # the sidecar makes it NEUTRAL
    starts = [(p[0], p[1], start_dir) for p in ponds]
    named = {"spawn": ponds[0], "ponds": ponds, "base": base, "boats": boats}
    return grid, [], bases, starts, named


def check(variant, grid, pills, bases, starts, named):
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if grid[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if grid[y][x] is not DEEP_SEA]
    mid = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    assert mid == (126, 126), (
        f"variant {variant}: terrain midpoint is {mid}, not (126,126) -- "
        f"mapRead would shift every coordinate in this file")

    ponds = set(named["ponds"])
    for (sx, sy, _d) in starts:
        assert grid[sy][sx] is DEEP_SEA, (
            f"variant {variant}: start ({sx},{sy}) must be deep sea "
            f"(starts.c startsIsValidSquare)")
        for dy in (-2, -1, 0, 1, 2):
            for dx in (-2, -1, 0, 1, 2):
                if (dx == 0 and dy == 0) or (sx + dx, sy + dy) in ponds:
                    continue
                assert grid[sy + dy][sx + dx] is not DEEP_SEA, (
                    f"variant {variant}: ({sx+dx},{sy+dy}) is open sea -- a "
                    f"spawn pond needs two tiles of land runway all round or "
                    f"the tank slides off the block at landing speed, and the "
                    f"pond must not open onto the sea or the bot sails instead "
                    f"of driving.  The OTHER pond is the one exception: it is "
                    f"a one-tile hole in the middle of the same block.")
    bx, by = named["base"]
    assert grid[by][bx] is not DEEP_SEA, (
        f"variant {variant}: base ({bx},{by}) is in the sea")

    # No RIVER anywhere: without that the bot builds a replacement boat and the
    # arena stops being about the shore.  BOAT is allowed in A4 only, and only
    # in the open water where it is a field seed, never a river.
    allowed = {GRASS, DEEP_SEA} | ({BOAT} if variant == "A4" else set())
    assert all(grid[y][x] in allowed
               for y in range(MAP_SIZE) for x in range(MAP_SIZE)), (
        f"variant {variant}: a tile is outside {sorted(t for t in allowed if t)} "
        f"-- a RIVER tile would let the bot build a boat and skip the arena")

    if variant == "A3":
        # The ledge is the point: every one of its tiles must have deep sea
        # BOTH north and south, so any wobble off due-west puts a sea tile on
        # the diagonal.
        ledge = [(x, by) for x in range(MAP_SIZE)
                 if grid[by][x] is not DEEP_SEA
                 and grid[by - 1][x] is DEEP_SEA
                 and grid[by + 1][x] is DEEP_SEA]
        assert len(ledge) >= 4, (
            f"variant A3: only {len(ledge)} tiles on the base's row have deep "
            f"sea both north and south -- that ledge IS the arena")
        assert (bx, by) in ledge, (
            f"variant A3: the base at ({bx},{by}) is not on the ledge, so the "
            f"tank never has to drive it")
        # ...and the incident's own corner shape has to exist on the staircase.
        corners = sum(1 for y in range(MAP_SIZE) for x in range(MAP_SIZE)
                      if grid[y][x] is not DEEP_SEA
                      and grid[y + 1][x] is DEEP_SEA
                      and grid[y + 1][x - 1] is not DEEP_SEA)
        assert corners >= A3_STEPS - 1, (
            f"variant A3: only {corners} tiles have sea due south and land to "
            f"the south-west -- the staircase is not the incident's shape")
    else:
        boats = named["boats"]
        assert len(boats) >= 3, "variant A4: too few BOAT tiles to seed the field"
        for (bxx, byy) in boats:
            assert grid[byy][bxx] == BOAT
            # A boat tile must be reachable on foot -- adjacent to land -- or
            # the boat layer is never entered and the arena tests nothing.
            assert any(grid[byy + dy][bxx + dx] == GRASS
                       for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1))), (
                f"variant A4: BOAT tile ({bxx},{byy}) has no land neighbour, so "
                f"a boatless tank can never reach the boat layer through it")
            # ...and it must be in the open water, not a pond.
            assert sum(1 for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1))
                       if grid[byy + dy][bxx + dx] is DEEP_SEA) >= 2, (
                f"variant A4: BOAT tile ({bxx},{byy}) is walled in; the boat "
                f"layer has to flood the water beside the shore")
        # The ledge is the point: a one-tile-wide walk with deep sea BOTH sides
        # of every tile, so every 8-neighbour scan the tank makes on it has sea
        # tiles in it.  Three columns was the first try and it measured nothing
        # (the Dijkstra routed down the inland column, three tiles from the
        # water).  Boat tiles do not count as land here -- they are water.
        ledge = [(x, y) for y in range(MAP_SIZE) for x in range(MAP_SIZE)
                 if grid[y][x] == GRASS
                 and grid[y][x - 1] is not GRASS and grid[y][x + 1] is not GRASS]
        assert len(ledge) >= 10, (
            f"variant A4: only {len(ledge)} one-wide ledge tiles -- the tank "
            f"has to walk with sea on both flanks or the fallback never sees a "
            f"sea tile at all")
        assert len(starts) == len(A4_SPAWNS)


def write(variant, output):
    grid, pills, bases, starts, named = build(variant)
    check(variant, grid, pills, bases, starts, named)

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
        f.write(encode_map_runs(grid))
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes) "
          f"[variant {variant}: spawn {named['spawn']}, base {named['base']}"
          + (f", boats {named['boats']}" if named['boats'] else "") + "]")


def spots(variant):
    """The arena's named tiles in FINAL (recentred) coordinates, plus the
    terrain grid, so the test file and the sidecars never repeat them."""
    grid, _p, _b, _s, named = build(variant)
    out = dict(named)
    out["grid"] = grid
    return out


def main():
    args = sys.argv[1:]
    variants = [args[0]] if args and args[0] in VARIANTS else list(VARIANTS)
    here = Path(__file__).parent
    for v in variants:
        out = (args[1] if len(args) > 1 and len(variants) == 1
               else str(here / f"drowning_fix_{v}.map"))
        write(v, out)


if __name__ == '__main__':
    main()
