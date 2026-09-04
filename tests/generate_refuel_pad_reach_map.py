#!/usr/bin/env python3
"""
Generate the refuel-pad reach arena (companion to tests/refuel_pad_reach_test.py).

Field incident 20260903_193428 bot2, brain tick 1563: refuel candidate base#0
@(138,112) was priced `raw 9 + base 45 + danger 427` (x danger{1.33}) = 640.9
while the tank sat on 10 armour two tiles away, and lost to base#5 twelve tiles
off at 301.  The whole 427 was threat.at(base) x REFUEL_DANGER_WEIGHT(20), and
the whole of THAT came from pill#2 at (138,121) -- exactly 9.0 tiles from the
base centre, i.e. sitting on the RIM of the brain's danger stamp
(PILL_RANGE_MAP 9, a deliberate 1-tile pad over the real fire range) and angry,
so the rim still read 21.4.

The engine disagrees.  pillbox.c pillsUpdate -> util.c utilIsItemInRange fires
only when the euclidean distance from the PILL's tile centre
(x + MAP_SQUARE_MIDDLE) to the TANK's world position is <= PILLBOX_RANGE
2048 wu = 8.0 tiles.  A tank parked on a base tile is at most half a tile
diagonal (0.7071 tiles) from that tile's centre.  So a pill can shell a docked
tank only when dist(pill tile, base tile) <= 8.0 + 0.7071 = 8.7071 tiles.  At
9.0 it cannot, from anywhere on the tile.

The brain change this arena exercises (author, 2026-09-03): for REFUEL-BASE
pricing only, in BOTH refuel scoring paths, when NO live deployed
hostile/neutral pill is within PILL_FIRE_RANGE + REFUEL_PAD_TANK_OFFSET of the
base tile, the PILL layer of the base's danger is removed
(danger = threat.at - threat.pill_at) and only the enemy-tank layer is left.
If any pill does reach part of the tile the stamped value stands unchanged --
no fractional scaling, because the tank does not choose where on the tile it
stops.  The driving stamp (PILL_RANGE_MAP 9, rim falloff) is NOT touched.

THE ARENA (one map per variant, one scenario sidecar)
    Everything is written between x 96..156 and y 110..142, so the bounding-box
    midpoint mapRead recenters on is already (126,126) and in-game tiles match
    this file exactly.  West to east:
      x 96..97     PATROL ISLAND (grass), the scripted opponent's home
      x 98..117    OPEN SEA      (unwritten = deep sea; 20 columns, uncrossable)
      x 118..156   FIELD         (grass), everything else
    (126,118)  BOT_SPAWN: a one-tile deep-sea pond.  A start square has to be
               DEEP SEA (starts.c startsIsValidSquare), so every arena in this
               suite digs one; the sidecar fills it once nobody is afloat.
    (126,122)  BASE_NEAR  -- 4 tiles from the spawn, fully stocked
    (138,118)  BASE_FAR   -- 12 tiles from the spawn, fully stocked, and
                            17.7 / 17.0 tiles from the pill, so it is padsafe
                            in BOTH variants (the control row)
    (126,131)  NEUTRAL PILL, variant A: EXACTLY 9.0 tiles due south of
               BASE_NEAR -- the incident's geometry.  On the RIM of the danger
               stamp, and unable to touch the base tile.
    (126,130)  NEUTRAL PILL, variant B: one tile closer, 8.0 tiles -- inside
               8.7071, so it really can shell a tank parked on BASE_NEAR.
    (96,112)   the opponent's start pond, on the island; it then paces
               x=96, y 116..133 forever.

WHY THE TANK WANTS TO REFUEL AT ALL
    TOURNAMENT with ZERO neutral bases hands every tank 0 shells / 0 mines /
    0 trees and full armour (gametype.c: shells = 2 * (neutralBases/numBases)
    * 100).  Both bases belong to the bot, so there are none.  The tank needs
    SHELLS and nothing else -- there is no mine anywhere in this arena, its
    armour stays at 40, and both bases are kept fully stocked so neither is
    rejected as low on stock and neither carries a low-stock markup.  The only
    thing separating the two bases is travel and the danger term.
    -ranked is REQUIRED: serverSimApplyScenarioCommit stamps gameScripted over
    the game type of any map with a scenario sidecar, and gameScripted gets the
    same full tank as gameOpen.  `!sim->ranked` is the one branch that leaves
    the type alone.  (Same reason tests/refuel_lowstock_test.py passes it.)

WHY THE OPPONENT IS PARKED 30 TILES AWAY BEHIND A SEA
    The arena needs a second player only so TOURNAMENT is a real game.  It must
    contribute NOTHING to either base's price, so the island sits >= 30 tiles
    (Manhattan) from both bases: past CONTESTED_BASE_RANGE (15), so the
    contested term is a flat 0, and far past the 6-tile enemy-tank threat
    radius, so threat.at() at either base is PURE pill danger.  That is what
    lets variant A assert the danger term is exactly 0 once the pill layer
    comes off.  attack_tank cannot reach it either (no river anywhere, so a
    stranded tank can never build a boat), so the bot never abandons refuel to
    hunt it.

WHY THE PILL IS NEUTRAL
    A neutral pillbox (owner 0xFF) shoots every tank in range and belongs to
    nobody, so it is "hostile/neutral" to the bot from tick 0 and stamps danger
    without needing a second team to own it.

Usage:
    python3 tests/generate_refuel_pad_reach_map.py [A|B] [output_path]
    Default: variant A -> tests/refuel_pad_reach_A.map
Writes <output>.map and the matching <output>.scenario.lua sidecar.
"""

import struct
import sys
from pathlib import Path

GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)
MAP_SIZE = 256
NEUTRAL = 0xFF

# -- Geometry (the test runner imports these for its assertions) -------------
BODY_Y = (110, 142)            # y extent of both land masses
ISLAND_X = (96, 97)            # patrol island
FIELD_X = (118, 156)           # the bot's world
# x 98..117 is left unwritten: 20 columns of open deep sea between the two.

BOT_SPAWN = (126, 118)         # one-tile deep-sea pond
BASE_NEAR = (126, 122)         # 4 tiles from the spawn
BASE_FAR = (138, 118)          # 12 tiles from the spawn
PILL = {"A": (126, 131),       # 9.0 tiles from BASE_NEAR -- CANNOT reach it
        "B": (126, 130)}       # 8.0 tiles from BASE_NEAR -- CAN reach it
FOE_SPAWN = (96, 112)          # opponent start pond, on the island

PATROL_X = 96                  # the lane the opponent settles into
PATROL_Y0 = 116
PATROL_Y1 = 133

BOT_SLOT = 0                   # our bot owns both bases
FULL_STOCK = 90                # both bases stay full: no reject, no markup
STOCK_PERIOD = 150             # sidecar re-assert cadence, in server ticks
PILL_HEALTH = 15               # PILLS_MAX_HEALTH -- alive
PILL_RELOAD = 250              # map-file `speed` byte (pillbox.c
                               # pillsReadMapFile).  PILLBOX_ATTACK_NORMAL is
                               # 100; bigger = SLOWER.  A lazy pill so variant
                               # B's tank is not chewed up while the rows the
                               # test reads are still being written.

# -- Brain/engine constants this arena is designed against -------------------
PILL_FIRE_RANGE = 8            # constants.lua -- PILLBOX_RANGE 2048wu / 256
REFUEL_PAD_TANK_OFFSET = 0.7071  # constants.lua -- half a tile diagonal
PAD_THRESHOLD = PILL_FIRE_RANGE + REFUEL_PAD_TANK_OFFSET   # 8.7071
PILL_RANGE_MAP = 9             # constants.lua -- the DANGER STAMP radius
CONTESTED_BASE_RANGE = 15      # constants.lua
TANK_THREAT_RADIUS = 6         # threat.lua
START_BASE_RANGE = 9           # starts.c (Chebyshev)
START_TANK_RANGE = 1           # starts.c (Chebyshev)


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def make_map(variant):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    y0, y1 = BODY_Y
    for yy in range(y0, y1 + 1):
        for xx in range(ISLAND_X[0], ISLAND_X[1] + 1):
            t[yy][xx] = GRASS
        for xx in range(FIELD_X[0], FIELD_X[1] + 1):
            t[yy][xx] = GRASS
    for (px, py) in (BOT_SPAWN, FOE_SPAWN):
        t[py][px] = DEEP_SEA
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


SIDECAR = '''\
-- Scenario sidecar for tests/refuel_pad_reach_{V}.map -- GENERATED by
-- tests/generate_refuel_pad_reach_map.py, do not edit by hand.
--
-- Two jobs.
--
-- 1. OWN BOTH BASES AND KEEP THEM FULL.  They are already slot {SLOT}'s in the
--    map file, but on_setup re-asserts it so the intent lives in one place.
--    Both hold armour {FULL} / shells {FULL}: neither is ever rejected as low
--    on stock and neither carries a low-stock markup, so travel and the DANGER
--    term are the only things telling the two bases apart -- which is the whole
--    point of the arena.  Re-asserted every {PERIOD} ticks with a mines value
--    that alternates 90/89, because the server only emits EVENT_BASE_STOCK when
--    a base's stock actually CHANGES and the bot's observation has to stay
--    fresh (and topped back up while the tank drinks).
--
-- 2. FILL THE SPAWN PONDS once both tanks are ashore.  A start square has to be
--    DEEP SEA (starts.c startsIsValidSquare), so the arena has two one-tile
--    ponds sitting in grass.  A tank that later drives over one without a boat
--    drowns instantly -- noise this test does not want.
local FULL   = {FULL}
local OWNER  = {SLOT}
local PERIOD = {PERIOD}
local PONDS  = {{ {{ {SPX}, {SPY} }}, {{ {EPX}, {EPY} }} }}
local GRASS  = 7
local flip = 0
local ponds_filled = false
local function assert_stock(g)
  flip = 1 - flip
  local mines = 90 - flip
  for i = 1, g.num_bases() do
    g.set_base_stock(i, FULL, FULL, mines)
  end
end
function on_setup(g)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      g.set_base_owner(i, OWNER)
      g.set_base_stock(i, FULL, FULL, 90)
      g.message(string.format(
        "REFUEL_PAD_ARENA base#%d (%d,%d) owner=%d armour=%d shells=%d",
        i, b.x, b.y, OWNER, FULL, FULL))
    end
  end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      g.message(string.format(
        "REFUEL_PAD_ARENA pill#%d (%d,%d) owner=%s health=%s",
        i, p.x, p.y, tostring(p.owner), tostring(p.armour)))
    end
  end
end
function on_tick(g, tick)
  if tick > 0 and tick % PERIOD == 0 then assert_stock(g) end
  if not ponds_filled and tick > 200 then
    local afloat = false
    for p = 0, 1 do
      local t = g.tank(p)
      if t and t.boat then afloat = true end
    end
    if not afloat then
      ponds_filled = true
      for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
      g.message("REFUEL_PAD_ARENA spawn ponds filled at tick " .. tostring(tick))
    end
  end
end
'''


def write_sidecar(path, variant):
    text = SIDECAR.format(
        V=variant, SLOT=BOT_SLOT, FULL=FULL_STOCK, PERIOD=STOCK_PERIOD,
        SPX=BOT_SPAWN[0], SPY=BOT_SPAWN[1],
        EPX=FOE_SPAWN[0], EPY=FOE_SPAWN[1])
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def main():
    args = list(sys.argv[1:])
    variant = "A"
    if args and args[0].upper() in ("A", "B"):
        variant = args.pop(0).upper()
    output = (args[0] if args else
              str(Path(__file__).parent / f"refuel_pad_reach_{variant}.map"))
    pill = PILL[variant]
    terrain = make_map(variant)

    # -- Recenter sanity: only written tiles define the bounding box, and its
    #    integer midpoint must already be (126,126) or every coordinate shifts.
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    for p in (BOT_SPAWN, FOE_SPAWN):
        assert terrain[p[1]][p[0]] is DEEP_SEA, (
            f"start {p} must be deep sea (starts.c startsIsValidSquare)")

    # -- THE POINT OF THE ARENA: variant A's pill sits on the danger stamp's
    #    rim and cannot touch BASE_NEAR; variant B's sits one tile in and can.
    d_near = edist(pill, BASE_NEAR)
    if variant == "A":
        assert abs(d_near - 9.0) < 1e-9, d_near
        assert d_near > PAD_THRESHOLD, (
            f"variant A's pill is {d_near} from BASE_NEAR, inside the "
            f"{PAD_THRESHOLD:.4f} pad threshold -- it would be padhit")
    else:
        assert abs(d_near - 8.0) < 1e-9, d_near
        assert d_near <= PAD_THRESHOLD, (
            f"variant B's pill is {d_near} from BASE_NEAR, outside the "
            f"{PAD_THRESHOLD:.4f} pad threshold -- it would be padsafe")
    #    ...and in BOTH variants it is inside the DANGER STAMP, or there is
    #    nothing for the fix to remove and the test is vacuous.
    assert d_near <= PILL_RANGE_MAP, (
        f"the pill is {d_near} from BASE_NEAR, outside the PILL_RANGE_MAP "
        f"({PILL_RANGE_MAP}) stamp -- threat.at() would already be 0 there")

    # -- BASE_FAR is the control: padsafe in both variants, and far outside
    #    the stamp so it carries no pill danger at all.
    d_far = edist(pill, BASE_FAR)
    assert d_far > PAD_THRESHOLD and d_far > PILL_RANGE_MAP, (d_far, variant)
    assert mdist(BOT_SPAWN, BASE_FAR) >= 12, mdist(BOT_SPAWN, BASE_FAR)
    assert mdist(BOT_SPAWN, BASE_NEAR) < mdist(BOT_SPAWN, BASE_FAR)

    # -- The tank must never enter the pill's REAL reach on the way to either
    #    base in variant A (spawn, the column it drives down, and the base
    #    itself all stay outside 8.0 tiles), or it gets shelled and the run
    #    stops being about pricing.
    if variant == "A":
        route = [(BOT_SPAWN[0], y)
                 for y in range(BOT_SPAWN[1], BASE_NEAR[1] + 1)]
        route += [(x, BOT_SPAWN[1])
                  for x in range(BOT_SPAWN[0], BASE_FAR[0] + 1)]
        for q in route:
            assert edist(pill, q) > PILL_FIRE_RANGE, (
                f"variant A: route tile {q} is {edist(pill, q):.2f} tiles from "
                f"the pill, inside PILL_FIRE_RANGE {PILL_FIRE_RANGE}")

    # -- The opponent must contribute nothing: past CONTESTED_BASE_RANGE of
    #    both bases (so the contested term is a flat 0) and far past the
    #    enemy-tank threat radius (so threat.at at a base is PURE pill danger).
    for y in range(PATROL_Y0, PATROL_Y1 + 1):
        for b in (BASE_NEAR, BASE_FAR):
            assert mdist((PATROL_X, y), b) > CONTESTED_BASE_RANGE, (b, y)
            assert edist((PATROL_X, y), b) > TANK_THREAT_RADIUS, (b, y)
        assert terrain[y][PATROL_X] is not DEEP_SEA, (PATROL_X, y)
    assert not (PATROL_Y0 <= FOE_SPAWN[1] <= PATROL_Y1), (
        "the patrol lane runs over the opponent's start pond")

    # -- The island really is an island: a full-height sea gap, and no river
    #    anywhere, so the stranded spawn boat can never be replaced.
    for y in range(BODY_Y[0], BODY_Y[1] + 1):
        for x in range(ISLAND_X[1] + 1, FIELD_X[0]):
            assert terrain[y][x] is DEEP_SEA, (x, y)
    assert GRASS == 7, "the field is plain grass; no river => no boat, ever"
    for p in (BOT_SPAWN, BASE_NEAR, BASE_FAR, pill):
        assert FIELD_X[0] <= p[0] <= FIELD_X[1], p
    assert ISLAND_X[0] <= FOE_SPAWN[0] <= ISLAND_X[1], FOE_SPAWN

    # -- Deterministic starts (startsGetStartTournament).  The bot's start must
    #    have an OWN base within START_BASE_RANGE so it lands in ownCandidates;
    #    the opponent's must have NONE, so the bot can never be given it.
    assert min(cheb(BOT_SPAWN, BASE_NEAR),
               cheb(BOT_SPAWN, BASE_FAR)) <= START_BASE_RANGE
    assert min(cheb(FOE_SPAWN, BASE_NEAR),
               cheb(FOE_SPAWN, BASE_FAR)) > START_BASE_RANGE, (
        "the opponent's start is within START_BASE_RANGE of a base -- it would "
        "join ownCandidates and the bot could be spawned on the island")
    assert cheb(BOT_SPAWN, FOE_SPAWN) > START_TANK_RANGE

    # Pill records: x, y, owner, armour, speed.  NEUTRAL (0xFF) shoots every
    # tank in range and belongs to nobody; armour 15 = PILLS_MAX_HEALTH (alive).
    pills = [(pill[0], pill[1], NEUTRAL, PILL_HEALTH, PILL_RELOAD)]
    # Base records: x, y, owner, armour, shells, mines.  Both full.
    bases = [(BASE_NEAR[0], BASE_NEAR[1], BOT_SLOT, FULL_STOCK, FULL_STOCK, 90),
             (BASE_FAR[0], BASE_FAR[1], BOT_SLOT, FULL_STOCK, FULL_STOCK, 90)]
    starts = [(BOT_SPAWN[0], BOT_SPAWN[1], 8),   # dir 8 = south, toward NEAR
              (FOE_SPAWN[0], FOE_SPAWN[1], 8)]   # dir 8 = south, down the lane

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

    sidecar = str(Path(output).with_suffix('')) + ".scenario.lua"
    write_sidecar(sidecar, variant)

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  and {sidecar}")
    print(f"  variant {variant}: neutral pill {pill} is {d_near:.4f} tiles from "
          f"BASE_NEAR {BASE_NEAR} -- "
          f"{'PADSAFE' if d_near > PAD_THRESHOLD else 'PADHIT'} "
          f"(threshold {PAD_THRESHOLD:.4f} = PILL_FIRE_RANGE {PILL_FIRE_RANGE} "
          f"+ REFUEL_PAD_TANK_OFFSET {REFUEL_PAD_TANK_OFFSET})")
    print(f"  BASE_FAR {BASE_FAR} is {d_far:.2f} tiles from the pill (padsafe "
          f"control) and mdist {mdist(BOT_SPAWN, BASE_FAR)} from the spawn "
          f"{BOT_SPAWN}; BASE_NEAR is mdist {mdist(BOT_SPAWN, BASE_NEAR)}")
    print(f"  patrol island x{ISLAND_X[0]}..{ISLAND_X[1]}, opponent start "
          f"{FOE_SPAWN}, lane x={PATROL_X} y={PATROL_Y0}..{PATROL_Y1} "
          f"(mdist >= {min(mdist((PATROL_X, y), b) for y in range(PATROL_Y0, PATROL_Y1 + 1) for b in (BASE_NEAR, BASE_FAR))} "
          f"from either base, past CONTESTED_BASE_RANGE {CONTESTED_BASE_RANGE})")


if __name__ == '__main__':
    main()
