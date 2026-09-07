#!/usr/bin/env python3
"""
Generate the drowning arenas (companion to tests/drowning_test.py).

THE INCIDENT UNDER TEST
-----------------------
20260905_231835_*_oilrig_2v2_repair_seed1, bot 3, engine tick 71330. The tank
sat at world (36104,36093) = tile (141,140) -- 8 world units inside column 141
and 3 units above row 141 -- with DEEP SEA at (141,141) directly south of it and
land at (140,141) to the south-west. Its heading wobbled between 133 and 135
(just west of due south). Three things lined up:

  1. THE BRAKE RAY CORNER-CUT THE KILLER TILE. steering.lua's global cliff brake
     walks the heading ray in 64 wu hops and only looks up the tile each sample
     LANDS in. At dir >= 134 the first sample jumped from (141,140) straight to
     (140,141) -- land -- and never tested (141,141) or (140,140), the two tiles
     it cut across. It fired at dir <= 133 and missed at 134-135, so the brake
     flickered on and off with the 2-brad wobble.

  2. A STOPPED TANK FALLS THROUGH THE GUARD. The brake only runs above
     CLIFF_MIN_SPEED, so once it had braked the tank to a standstill the guard
     stopped running, navigate took the tick, and its single KEY_FASTER moved
     the tank 6 wu south -- forward, into the water. Tanks have no reverse gear
     (tank.c tankAccel clamps speed at 0), so this was a drive-in, not a slide.

  3. NOTHING BACKSTOPPED IT. escape_water is recovery, not prevention; the
     stuck escalation needs 150 same-tile ticks on land and was reset to 0 by
     the very tile change that drowned it.

The fix is two knobs, both ON by default and both false in PRESETS.keel:
C.CLIFF_RAY_CORNER_CHECK (the ray also tests the two tiles a both-axes sample
hop cut across) and C.CLIFF_STOP_MASK_ALL_GOALS (M.steer clears KEY_FASTER and
sets KEY_SLOWER when the tile ahead, or a corner-cut tile, is deep sea).

THE TWO ARENAS
--------------
  A  THE STAIRCASE SHORELINE. A two-tile-wide staircase of grass descending to
     the south-west through open deep sea, from a spawn block at the north-east
     to a NEUTRAL BASE at the south-west. Step i is the tile pair
     {(X0-i, Y0+i), (X0-i-1, Y0+i)}; consecutive steps SHARE a column, so the
     walk is always a legal cardinal or shared-edge move and never a squeeze
     between two sea tiles. Standing on the EAST tile of any pair reproduces the
     incident exactly: sea directly south, land to the south-west, and the tank
     hugging the column boundary on a south-west heading. The base is the only
     thing to do on the map, and the staircase is the only way to it.

  B  WATER IN FRONT OF THE NOSE, AND THE ONLY WAY OUT BEHIND. A three-row grass
     corridor running east to a NEUTRAL BASE, plus a long way round -- a second
     corridor well to the north, joined at both ends -- that the tank has no
     reason to use. The tank drives east at its goal; when its centre is
     CUT_MARGIN_WU short of the next column the sidecar turns that WHOLE column
     into deep sea, two tiles short of the base. The tank is then nose-on to
     open water with its goal on the far side of it and its only remaining route
     ~40 tiles backwards round the ring.

     WHAT ARENA B IS AND IS NOT. It is NOT a drowning control -- see the note in
     tests/drowning_test.py; measured, no warning distance separates the knobs
     here. It is the DEADLOCK guard for C.CLIFF_STOP_MASK_ALL_GOALS: the new
     rule strips KEY_FASTER whenever deep sea is one tile ahead, and the obvious
     way for that to go wrong is a tank pinned at a shoreline forever, unable to
     accelerate and never getting round. This arena is exactly that shape -- goal
     across the water, route behind -- and the assertion is that the tank turns
     round, walks the forty tiles and captures the base anyway.

     The column is cut AT RUN TIME and not written into the .map on purpose. The
     Dijkstra navigator plans the whole route: a gap present at load time is one
     the tank routes around from the far end, arriving on the north corridor
     having never faced the water at all.

WHY NO TANK TELEPORT: the scenario API (src/server/scenario.c scBuildGameTable)
has no hook that places a tank -- it can read one (game.tank) and it can rewrite
the ground under it (game.set_tile), and that is all. Both arenas therefore have
to produce the approach with the DRIVE itself, which is what the geometry above
is for.

MAP FACTS THAT BITE (the list every generator in tests/ keeps):
  * mapRead recenters the terrain bounding-box midpoint to (126,126). Rather
    than hand-place tiles and hope, every arena here is built in whatever
    coordinates were convenient and then SHIFTED by recenter() so its own
    midpoint is already (126,126) and mapRead moves nothing.
  * a start square must be DEEP SEA (starts.c startsIsValidSquare) -- hence the
    one-tile spawn pond. Both arenas bury the pond in the middle of a 5x5 block
    of land, so all eight of its neighbours are land and there are two more
    tiles of runway beyond them (the tank comes off its boat at full speed): the tank starts afloat
    with no route to the open sea, and the sidecar fills the pond with grass as
    soon as the tank is ashore, which destroys the boat it left behind. There
    is no RIVER anywhere on either map, so it can never build another one --
    without that, "the sea is the danger" is not true, because the bot would
    simply sail to the base and the staircase would never be walked.
  * mapRead puts ROAD under every map-file pill (not used here -- neither arena
    has a pill; the goal is a base, so nothing needs an LGM).
  * -gametype open starts a tank with TANK_FULL_TREES (40).

Usage:
    python3 tests/generate_drowning_map.py [variant] [output_path]
    variant: A | B   (default: both)
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

ROAD = 4
GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256

VARIANTS = ("A", "B")

# ── Arena A: the staircase shoreline ─────────────────────────────────────
# Built around an arbitrary origin and recentred at the end.
A_STEPS = 9                     # step pairs; i = 0..A_STEPS-1
A_X0, A_Y0 = 130, 122           # step 0 is {(130,122), (129,122)}
A_BLOCK = (128, 132, 118, 122)  # x0,x1,y0,y1 -- the spawn block, and it
                                # CONTAINS step 0, so the two are connected.
                                # FIVE tiles across and not three: the tank
                                # comes off its boat at full speed (spd 64 in
                                # the first measured run) and a 3x3 block gave
                                # it ONE tile of runway -- it slid off the far
                                # edge and drowned before the arena had begun.
A_SPAWN = (130, 120)            # dead centre of the block: 8 land neighbours
                                # and two tiles of land beyond them all round
# The last step is {(130-8, 130), (121, 130)} = {(122,130), (121,130)}; the goal
# pad's top row covers both, so the staircase runs into it.
A_PAD = (119, 123, 130, 134)
A_BASE = (121, 132)             # neutral: the only goal on the map

# ── Arena B: stopped on the shore ────────────────────────────────────────
B_MAIN = (118, 136, 124, 126)   # the main corridor: three rows, west to east
B_BLOCK = (116, 120, 123, 127)  # spawn block at its west end, five across for
                                # the same reason arena A's is (see A_BLOCK)
B_SPAWN = (118, 125)            # centre of it
B_NORTH = (122, 136, 114, 116)  # the LONG WAY ROUND, three rows, well clear of
                                # the main corridor: the tank must never have
                                # deep sea a tile or two off its flank on the
                                # drive east, or the brake would be firing on
                                # the scenery instead of on the notch
B_WEST_JOINT = (121, 123, 114, 126)   # the two verticals that close the ring
B_EAST_JOINT = (134, 136, 114, 126)
B_BASE = (132, 125)             # neutral, in the main corridor and TWO TILES
                                # east of the notch: when the water opens the
                                # tank is almost on top of its goal, and the
                                # only remaining route to it is ~40 tiles the
                                # other way round the ring
B_NOTCH_X = 131                 # every main-corridor row at this column is cut
B_TRIGGER_X = 130               # ...when the tank's tile column is this one.
                                # Cutting the WHOLE column and not one tile is
                                # what makes the tank face the water with a
                                # U-TURN as its route rather than a one-tile
                                # sidestep: the first arena left the diagonal
                                # open, the tank took it at speed, and nothing
                                # was ever proved. The incident's tank was
                                # pinned between an evasive turn one way and a
                                # committed u-turn the other, which is the only
                                # state in which it stands still long enough to
                                # creep in.


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def fill(tiles, box, tile=GRASS):
    x0, x1, y0, y1 = box
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            tiles[(xx, yy)] = tile


def staircase(tiles, x0, y0, steps, tile=GRASS):
    """Step i is the pair {(x0-i, y0+i), (x0-i-1, y0+i)}.

    Steps i and i+1 share column x0-i-1, so (x0-i-1, y0+i) -> (x0-i-1, y0+i+1)
    is a plain cardinal move south and no leg of the walk ever has to squeeze
    diagonally between two deep-sea tiles. Standing on the EAST tile of a pair
    is the incident: sea due south, land to the south-west."""
    for i in range(steps):
        tiles[(x0 - i, y0 + i)] = tile
        tiles[(x0 - i - 1, y0 + i)] = tile


def recenter(tiles, spots):
    """Shift everything so the terrain bbox midpoint is already (126,126).

    mapRead does this shift itself at load time; doing it here means the
    coordinates in this file, in the sidecar and in the test are the ones the
    game actually uses. `spots` is a list of (x, y) tuples to move with the
    terrain; returns (shifted_tiles, shifted_spots)."""
    xs = [x for (x, _y) in tiles]
    ys = [y for (_x, y) in tiles]
    dx = 126 - (min(xs) + max(xs)) // 2
    dy = 126 - (min(ys) + max(ys)) // 2
    return ({(x + dx, y + dy): t for (x, y), t in tiles.items()},
            [(x + dx, y + dy) for (x, y) in spots])


def build(variant):
    """Returns terrain grid, pills, bases, starts, and the arena's named spots."""
    tiles = {}
    if variant == "A":
        fill(tiles, A_BLOCK)
        staircase(tiles, A_X0, A_Y0, A_STEPS)
        fill(tiles, A_PAD)
        spots = [A_SPAWN, A_BASE]
        tiles, spots = recenter(tiles, spots)
        spawn, base = spots
        notch = trigger = None
    else:
        fill(tiles, B_BLOCK)
        fill(tiles, B_MAIN)
        fill(tiles, B_NORTH)
        fill(tiles, B_WEST_JOINT)
        fill(tiles, B_EAST_JOINT)
        # The notch column is named by two of its tiles so recenter() moves it
        # with everything else; only the x of each is used.
        spots = [B_SPAWN, B_BASE, (B_NOTCH_X, B_MAIN[2]), (B_TRIGGER_X, B_MAIN[2])]
        tiles, spots = recenter(tiles, spots)
        spawn, base, notch, trigger = spots

    grid = blank()
    for (x, y), t in tiles.items():
        grid[y][x] = t
    grid[spawn[1]][spawn[0]] = DEEP_SEA          # the pond

    pills = []
    bases = [(base[0], base[1], 0, 90, 90, 90)]  # the sidecar makes it NEUTRAL
    # The start direction byte is one of 16 compass points: 0 N, 4 E, 8 S,
    # 12 W. A faces SOUTH-WEST (10), the way its staircase runs; B faces EAST
    # (4), down its corridor.
    starts = [(spawn[0], spawn[1], 10 if variant == "A" else 4)]
    named = {"spawn": spawn, "base": base, "notch": notch, "trigger": trigger}
    if variant == "B":
        dy = spawn[1] - B_SPAWN[1]
        named["main_rows"] = [y + dy for y in range(B_MAIN[2], B_MAIN[3] + 1)]
    return grid, pills, bases, starts, named


def check(variant, grid, pills, bases, starts, named):
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if grid[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if grid[y][x] is not DEEP_SEA]
    mid = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    assert mid == (126, 126), (
        f"variant {variant}: terrain midpoint is {mid}, not (126,126) -- "
        f"mapRead would shift every coordinate in this file")

    sx, sy, _d = starts[0]
    assert grid[sy][sx] is DEEP_SEA, (
        f"variant {variant}: start ({sx},{sy}) must be deep sea "
        f"(starts.c startsIsValidSquare)")
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dx == 0 and dy == 0:
                continue
            assert grid[sy + dy][sx + dx] is not DEEP_SEA, (
                f"variant {variant}: the spawn pond's neighbour "
                f"({sx+dx},{sy+dy}) is open sea -- the tank would sail to the "
                f"base instead of driving, and the arena would test nothing")
    for (bx, by, *_r) in bases:
        assert grid[by][bx] is not DEEP_SEA, (
            f"variant {variant}: base ({bx},{by}) is in the sea")
    # No river anywhere: without this the bot could build a replacement boat
    # after the sidecar destroys the first one.
    assert all(grid[y][x] in (GRASS, ROAD, DEEP_SEA)
               for y in range(MAP_SIZE) for x in range(MAP_SIZE)), (
        f"variant {variant}: a tile is neither grass, road nor deep sea -- if "
        f"it is RIVER the bot can build a boat and sail past the arena")

    if variant == "A":
        # The incident shape has to exist, and on every step: standing on the
        # EAST tile of a pair, the tile due SOUTH is sea and the tile to the
        # SOUTH-WEST is land. That is the corner the ray cuts.
        corners = 0
        for y in range(MAP_SIZE):
            for x in range(MAP_SIZE):
                if (grid[y][x] is not DEEP_SEA
                        and grid[y + 1][x] is DEEP_SEA
                        and grid[y + 1][x - 1] is not DEEP_SEA):
                    corners += 1
        assert corners >= A_STEPS - 1, (
            f"variant A: only {corners} tiles have sea due south and land to "
            f"the south-west -- the staircase is not the incident's shape")
    else:
        nx, _ny = named["notch"]
        tx, _ty = named["trigger"]
        bx, by = named["base"]
        rows = named["main_rows"]
        assert tx == nx - 1, (
            "variant B: the trigger column must be the one immediately WEST of "
            "the notch column, so the water opens directly in front of the tank")
        for y in rows:
            assert grid[y][nx] is not DEEP_SEA, (
                f"variant B: notch tile ({nx},{y}) is already sea in the .map "
                f"-- the column has to be cut at RUN TIME or the tank routes "
                f"around it from the far end and never faces the water")
        assert bx > nx, (
            f"variant B: the base at ({bx},{by}) is west of the notch column "
            f"{nx} -- cutting the notch has to put the goal on the FAR side or "
            f"the tank has no reason to keep facing the water")
        # The ring: after the cut the base must still be reachable, and only
        # the long way round. Flood-fill from the base over land with the notch
        # column removed and check it reaches the spawn.
        cut = {(nx, y) for y in rows}
        seen, stack = set(), [named["base"]]
        while stack:
            (x, y) = stack.pop()
            if (x, y) in seen or (x, y) in cut:
                continue
            if not (0 <= x < MAP_SIZE and 0 <= y < MAP_SIZE):
                continue
            if grid[y][x] is DEEP_SEA:
                continue
            seen.add((x, y))
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                stack.append((x + dx, y + dy))
        assert named["spawn"] in seen or any(
            (named["spawn"][0] + dx, named["spawn"][1] + dy) in seen
            for dx in (-1, 0, 1) for dy in (-1, 0, 1)), (
            "variant B: with the notch column cut the base is UNREACHABLE -- "
            "the bot would drop the goal and turn away calmly instead of "
            "standing at the water with a route that starts behind it")
        # ...and the long way round really is long. The point of the arena is
        # that the tank is TWO tiles from its goal and the only route left is
        # tens of tiles the other way, which is what commits the u-turn latch
        # and pins the heading against the evasive turn.
        assert bx - tx <= 4, (
            f"variant B: the base is {bx - tx} tiles east of the trigger -- it "
            f"has to be right under the tank's nose for the detour to read as "
            f"a u-turn")
        assert len(seen) >= 40, (
            f"variant B: the detour is only {len(seen)} tiles -- too short to "
            f"hold the u-turn latch while the tank sits at the water")


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
          + (f", notch {named['notch']} on trigger {named['trigger']}"
             if named['notch'] else "") + "]")


def spots(variant):
    """The arena's named tiles, in FINAL (recentred) coordinates -- the test
    file and the sidecars read them from here rather than repeating them."""
    _g, _p, _b, _s, named = build(variant)
    return named


def main():
    args = sys.argv[1:]
    variants = [args[0]] if args and args[0] in VARIANTS else list(VARIANTS)
    here = Path(__file__).parent
    for v in variants:
        out = (args[1] if len(args) > 1 and len(variants) == 1
               else str(here / f"drowning_{v}.map"))
        write(v, out)


if __name__ == '__main__':
    main()
