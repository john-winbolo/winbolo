#!/usr/bin/env python3
"""
Generate the refuel low-stock arena (companion to tests/refuel_lowstock_test.py).

Field incident 20260902_120856 block 3: bot2 (0 shells) docked at base #11,
which held 4-8 shells, took THREE, and left one frame later, while #3/#4/#9
held 80-90 shells the whole time.  A base holding four shells priced exactly
the same as one holding ninety, because REFUEL_MIN_STOCK is a threshold REJECT
and nothing above it cost anything.

The brain change this arena exercises (author, 2026-09-03): a refuel candidate
with a FRESH stock observation that is below REFUEL_MIN_STOCK for a resource
the tank NEEDS is 10% dearer per such resource -- REFUEL_LOW_ARMOUR_MULT /
REFUEL_LOW_SHELLS_MULT (1.10 each, x1.21 for both).  It is a markup on the
short base, never a discount on the full one, so it cannot make refuel win
over other goals any earlier.

WHEN THE MARKUP CAN SHOW AT ALL
    The low_stock REJECT upstream drops a base that can supply NOTHING the
    tank needs.  A tank that needs only shells therefore never sees a
    shells-short base priced -- it is rejected outright -- and the markup is
    for the base that gets PAST the reject: the tank needs TWO things and the
    base is short of one.  So the arena has to make the tank need both:
      * shells: TOURNAMENT + -ranked puts 0 shells in the tank (see below);
      * armour: one hidden MINE (terrain MINE_GRASS, 15) on the only tile
        through a deep-sea barrier between the spawn and the bases.  The bot
        cannot see hidden mines, drives over it, and takes MINE_DAMAGE (10):
        40 -> 30 armour, below its armour target, so armour is now needed.
    BASE_NEAR is then pinned at NEAR_ARMOUR (4, below REFUEL_MIN_STOCK) and
    NEAR_SHELLS (6, at/above it): it can supply shells, so it survives the
    reject, and it is short of armour, so it is marked up x1.10 (lowarm).
    BASE_FAR is full and pays nothing.

THE ARENA (one map, one stock sidecar)
    Everything is written between x 110..142 and y 110..142, symmetric about
    (126,126), so mapRead's recenter is a no-op and in-game tiles match this
    file.  Vertical bands, west to east:
      x 110..111   PATROL ISLAND (grass)
      x 112        MOAT          (deep sea, the full height of the map body)
      x 113..122   WEST FIELD    (grass) -- both bases live here
      x 123        BARRIER       (deep sea) with ONE grass gap at GAP, mined
      x 124..142   EAST FIELD    (grass) -- the bot spawns here
    (126,127)  the bot's start: a ONE-TILE deep-sea pond in the east field.
               A start square has to be DEEP SEA (starts.c
               startsIsValidSquare), so every arena in this suite digs one.
    (123,127)  GAP: the only way west, and the mine.
    (120,131)  BASE_NEAR  -- mdist 10 from the spawn, pinned short of armour
    (120,118)  BASE_FAR   -- mdist 15 from the spawn, full
    (110,112)  the scripted opponent's start pond, ON THE ISLAND.
    x=110      its patrol lane: it drives south off the pond and then paces
               y=116 <-> y=133 forever.  A moving hostile tank, so both bases
               carry a real, swinging contested term -- the markup has to sit
               on top of a live price, not a toy one.

WHY THE OPPONENT IS ON AN ISLAND
    contested_penalty measures mdist to the base -- it does not care whether
    the enemy can be reached.  attack_tank does: an unreachable tank costs
    INF and never wins a pool.  Putting the enemy across a moat gives the
    arena a moving hostile tank without the bot abandoning refuel to hunt it.
    The moat and the barrier are uncrossable for good: the bot's spawn boat
    is stranded the moment it drives onto grass, and there is no river
    anywhere on the map, so no boat can ever be built.

WHY THE RUN IS TOURNAMENT + -ranked
    TOURNAMENT with ZERO neutral bases means gameTypeGetItems hands every tank
    0 shells / 0 mines / 0 trees and full armour (gametype.c: shells =
    2 * (neutralBases / numBases) * 100).  The bot has to WANT to refuel, and
    a full 40/40 open loadout wants nothing.  It also disarms the opponent.
    -ranked is REQUIRED: serverSimApplyScenarioCommit stamps gameScripted over
    the game type of any map with a scenario sidecar, and gameScripted gets the
    same full tank as gameOpen.  `!sim->ranked` is the one branch that leaves
    the type alone.  (Same reason tests/respawn_loadout_test.py passes it.)
    Tournament also makes both starts deterministic
    (startsGetStartTournament): the bot's start has an OWN base within
    START_BASE_RANGE (9) so it lands in ownCandidates, a tier the opponent --
    to whom those bases are hostile -- can never reach.

Usage:
    python3 tests/generate_refuel_lowstock_map.py [output_path]
    Default: tests/refuel_lowstock.map
Writes <output>.map and the matching <output>.scenario.lua sidecar.
"""

import struct
import sys
from pathlib import Path

GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)
MAP_SIZE = 256

# -- Geometry (the test runner imports these for its assertions) -------------
BODY_Y = (110, 142)            # y extent of every band
ISLAND_X = (110, 111)          # patrol island
MOAT_X = 112                   # the uncrossable column
FIELD_X = (113, 142)           # the bot's world (minus the barrier column)
BARRIER_X = 123                # deep-sea column between spawn and bases
GAP = (123, 127)               # the one grass tile through it -- mined
MINE_GRASS = 15                # global.h: grass with a mine on it
SPAWN = (126, 127)             # bot start: one-tile deep-sea pond
FOE_SPAWN = (110, 112)         # opponent start pond, on the island
BASE_NEAR = (120, 131)         # mdist 10 from SPAWN
BASE_FAR = (120, 118)          # mdist 15 from SPAWN -- 13 tiles from BASE_NEAR
PONDS = [SPAWN, FOE_SPAWN]

PATROL_X = 110                 # the lane the opponent settles into
PATROL_Y0 = 116                # north turnaround (south of FOE_SPAWN, so the
PATROL_Y1 = 133                # pond is never re-crossed once it is left)

BOT_SLOT = 0                   # our bot owns both bases
FULL_STOCK = 90                # BASE_FULL_ARMOUR / a full shell magazine
NEAR_SHELLS = 6                # BASE_NEAR shells: >= REFUEL_MIN_STOCK, past the reject
NEAR_ARMOUR = 4                # BASE_NEAR armour: < REFUEL_MIN_STOCK, marked up
STOCK_PERIOD = 150             # sidecar re-assert cadence, in server ticks

# -- Brain/engine constants this arena is designed against -------------------
CONTESTED_BASE_RANGE = 15      # constants.lua
CONTESTED_BASE_PENALTY = 120   # constants.lua
REFUEL_MIN_STOCK = 5           # constants.lua -- below this the row is REJECTed
REFUEL_LOW_ARMOUR_MULT = 1.10     # constants.lua
REFUEL_LOW_SHELLS_MULT = 1.10     # constants.lua
MINE_DAMAGE = 10                  # tank.h
START_BASE_RANGE = 9           # starts.c (Chebyshev)
START_TANK_RANGE = 1           # starts.c (Chebyshev)


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def contested(lane_y, base, lane_x=PATROL_X):
    """goals.lua contested_penalty for ONE moving enemy tank standing at
    (lane_x, lane_y): CONTESTED_BASE_PENALTY x (1 - d/RANGE), 0 past the
    range.  Used to DESIGN the swing, never asserted equal."""
    d = mdist((lane_x, lane_y), base)
    if d > CONTESTED_BASE_RANGE:
        return 0.0
    return CONTESTED_BASE_PENALTY * (1.0 - d / CONTESTED_BASE_RANGE)


def contested_swing(base):
    """How much the patrol moves this base's contested term over one lap."""
    vals = [contested(y, base) for y in range(PATROL_Y0, PATROL_Y1 + 1)]
    return max(vals) - min(vals)


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    y0, y1 = BODY_Y
    for yy in range(y0, y1 + 1):
        for xx in range(ISLAND_X[0], ISLAND_X[1] + 1):
            t[yy][xx] = GRASS
        for xx in range(FIELD_X[0], FIELD_X[1] + 1):
            t[yy][xx] = GRASS
        # MOAT_X is left as background deep sea.
        t[yy][BARRIER_X] = DEEP_SEA      # the barrier, full height...
    t[GAP[1]][GAP[0]] = MINE_GRASS       # ...with one mined grass gap
    for (px, py) in PONDS:
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
-- Scenario sidecar for tests/refuel_lowstock.map -- GENERATED by
-- tests/generate_refuel_lowstock_map.py, do not edit by hand.
--
-- Three jobs.
--
-- 1. OWN BOTH BASES, and set their stock.  The bases are already written into
--    the map file as slot {SLOT}'s, but on_setup re-asserts it so the intent is
--    in one place -- and it is the only place the stock lives:
--      BASE_NEAR  armour {NEAR_ARM} (below REFUEL_MIN_STOCK -> marked up),
--                 shells {NEAR_SH}  (at/above it -> survives the low_stock reject)
--      BASE_FAR   armour {FULL} shells {FULL}
--
-- 2. KEEP THE STOCK READING FRESH.  The markup only prices a base whose
--    observation is younger than REFUEL_OBS_STALE (500 ticks), and a bot only
--    learns base stock from EVENT_BASE_STOCK, which the server emits ONLY when
--    a base's armour/shells/mines actually CHANGE.  So every {PERIOD} ticks this
--    re-asserts the stock with a mines value that alternates 90/89 -- a real
--    change, so a real event.  It also puts the stock back, which is
--    deliberate: the near base has to stay pinned even while the tank drinks.
--
-- 3. FILL THE SPAWN PONDS once both tanks are ashore.  A start square has to
--    be DEEP SEA (starts.c startsIsValidSquare), so the arena has two one-tile
--    ponds sitting in grass.  A tank that later drives over one without a boat
--    drowns instantly -- noise this test does not want -- so once neither tank
--    is afloat the ponds become grass.
local NEAR   = {{ {NX}, {NY} }}
local FAR    = {{ {FX}, {FY} }}
local FULL   = {FULL}
local NEAR_SH  = {NEAR_SH}
local NEAR_ARM = {NEAR_ARM}
local OWNER  = {SLOT}
local PERIOD = {PERIOD}
local PONDS  = {{ {{ {SPX}, {SPY} }}, {{ {EPX}, {EPY} }} }}
local GRASS  = 7
local flip = 0
local ponds_filled = false
-- armour, shells for one base.  BASE_NEAR is the only one that varies.
local function stock_for(b)
  if b.x == NEAR[1] and b.y == NEAR[2] then return NEAR_ARM, NEAR_SH end
  return FULL, FULL
end
local function assert_stock(g)
  flip = 1 - flip
  local mines = 90 - flip
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      local arm, sh = stock_for(b)
      g.set_base_stock(i, arm, sh, mines)
    end
  end
end
function on_setup(g)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      g.set_base_owner(i, OWNER)
      local arm, sh = stock_for(b)
      g.set_base_stock(i, arm, sh, 90)
      g.message(string.format(
        "REFUEL_LOWSTOCK_ARENA base#%d (%d,%d) owner=%d armour=%d shells=%d",
        i, b.x, b.y, OWNER, arm, sh))
    end
  end
end
function on_tick(g, tick)
  if tick > 0 and tick % PERIOD == 0 then assert_stock(g) end
  -- Fill the ponds as soon as nobody is afloat any more (see note 3).
  if not ponds_filled and tick > 200 then
    local afloat = false
    for p = 0, 1 do
      local t = g.tank(p)
      if t and t.boat then afloat = true end
    end
    if not afloat then
      ponds_filled = true
      for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
      g.message("REFUEL_LOWSTOCK_ARENA spawn ponds filled at tick " .. tostring(tick))
    end
  end
end
'''


def write_sidecar(path):
    text = SIDECAR.format(
        SLOT=BOT_SLOT, FULL=FULL_STOCK,
        NEAR_SH=NEAR_SHELLS, NEAR_ARM=NEAR_ARMOUR, PERIOD=STOCK_PERIOD,
        NX=BASE_NEAR[0], NY=BASE_NEAR[1],
        FX=BASE_FAR[0], FY=BASE_FAR[1],
        SPX=SPAWN[0], SPY=SPAWN[1],
        EPX=FOE_SPAWN[0], EPY=FOE_SPAWN[1])
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def main():
    args = list(sys.argv[1:])
    output = args[0] if args else str(Path(__file__).parent / "refuel_lowstock.map")
    terrain = make_map()

    # -- Recenter sanity: only written tiles define the bounding box, and its
    #    integer midpoint must already be (126,126) or every coordinate shifts.
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    for p in PONDS:
        assert terrain[p[1]][p[0]] is DEEP_SEA, (
            f"start {p} must be deep sea (starts.c startsIsValidSquare)")

    # -- The moat has to run the FULL height of the map body, or the island is
    #    reachable on foot and attack_tank comes back.
    for y in range(BODY_Y[0], BODY_Y[1] + 1):
        assert terrain[y][MOAT_X] is DEEP_SEA, (MOAT_X, y)
    # ...and nothing on the bot's side of it may be a river (a river is the
    #    one terrain a stranded tank can build a boat on).
    assert GRASS == 7, "the field is plain grass; no river => no boat, ever"

    # -- Both bases and the bot's spawn live in the main field, the opponent's
    #    spawn and its lane on the island.
    for p in (SPAWN, BASE_NEAR, BASE_FAR):
        assert FIELD_X[0] <= p[0] <= FIELD_X[1], p
    assert ISLAND_X[0] <= FOE_SPAWN[0] <= ISLAND_X[1], FOE_SPAWN
    assert ISLAND_X[0] <= PATROL_X <= ISLAND_X[1], PATROL_X

    # -- The two bases are the incident's geometry, and the near one is
    #    genuinely nearer.
    assert mdist(BASE_NEAR, BASE_FAR) == 13, mdist(BASE_NEAR, BASE_FAR)
    assert mdist(SPAWN, BASE_NEAR) < mdist(SPAWN, BASE_FAR)

    # -- Deterministic starts (startsGetStartTournament).  The bot's start must
    #    have an OWN base within START_BASE_RANGE so it lands in ownCandidates;
    #    the opponent's must have NONE, so the bot can never be given it.
    assert min(cheb(SPAWN, BASE_NEAR), cheb(SPAWN, BASE_FAR)) <= START_BASE_RANGE
    assert min(cheb(FOE_SPAWN, BASE_NEAR),
               cheb(FOE_SPAWN, BASE_FAR)) > START_BASE_RANGE, (
        "the opponent's start is within START_BASE_RANGE of a base -- it would "
        "join ownCandidates and the bot could be spawned on the island")
    assert cheb(SPAWN, FOE_SPAWN) > START_TANK_RANGE

    # -- The patrol swings each base's contested term, in ANTIPHASE, by more
    #    than the two bases differ in travel...
    for b in (BASE_NEAR, BASE_FAR):
        assert contested_swing(b) > 30, (b, contested_swing(b))
    #    (each base peaks when the patrol is level with it, and reads 0 when
    #    the patrol is level with the other one -- that is what "antiphase"
    #    has to mean here, not "opposite at the turnarounds", where both
    #    bases are simply out of range).
    assert contested(BASE_NEAR[1], BASE_NEAR) > 0 and         contested(BASE_NEAR[1], BASE_FAR) == 0, "BASE_NEAR's peak is not clean"
    assert contested(BASE_FAR[1], BASE_FAR) > 0 and         contested(BASE_FAR[1], BASE_NEAR) == 0, "BASE_FAR's peak is not clean"
    assert PATROL_Y0 <= BASE_FAR[1] <= BASE_NEAR[1] <= PATROL_Y1, (
        "the lane does not run past both bases")
    # ...and the lane never crosses its own spawn pond once it is left.
    assert not (PATROL_Y0 <= FOE_SPAWN[1] <= PATROL_Y1), (
        "the patrol lane runs over the opponent's start pond -- it would drown "
        "the moment the sidecar has not yet filled it in")
    for y in range(PATROL_Y0, PATROL_Y1 + 1):
        assert terrain[y][PATROL_X] is not DEEP_SEA, (PATROL_X, y)

    # -- The barrier: deep sea the full height of the body, one grass gap,
    #    and the mine sits in that gap -- so every route west crosses it.
    for y in range(BODY_Y[0], BODY_Y[1] + 1):
        if (BARRIER_X, y) != GAP:
            assert terrain[y][BARRIER_X] is DEEP_SEA, (BARRIER_X, y)
    assert GAP[0] == BARRIER_X and terrain[GAP[1]][GAP[0]] == MINE_GRASS, GAP
    assert SPAWN[0] > BARRIER_X, "the bot must spawn EAST of the barrier"
    for b in (BASE_NEAR, BASE_FAR):
        assert b[0] < BARRIER_X, (b, "both bases must be WEST of the barrier")
    assert MINE_DAMAGE < 40, "the mine must leave the tank alive"
    # -- The near base has to SURVIVE the low_stock reject (it can supply the
    #    shells the tank needs) and still be short of the armour it also
    #    needs, so the markup -- not the reject -- is what prices it.
    assert NEAR_SHELLS >= REFUEL_MIN_STOCK, (
        f"{NEAR_SHELLS} shells is below REFUEL_MIN_STOCK ({REFUEL_MIN_STOCK}); "
        "the row would be REJECTed low_stock and never priced at all")
    assert NEAR_ARMOUR < REFUEL_MIN_STOCK, (
        f"{NEAR_ARMOUR} armour is not below REFUEL_MIN_STOCK "
        f"({REFUEL_MIN_STOCK}); nothing would be marked up")
    # Base records: x, y, owner, armour, shells, mines.  Owned by the bot from
    # the map file; the sidecar re-asserts it (and the stock).
    bases = [(BASE_NEAR[0], BASE_NEAR[1], BOT_SLOT, NEAR_ARMOUR,
              NEAR_SHELLS, 90),
             (BASE_FAR[0], BASE_FAR[1], BOT_SLOT, FULL_STOCK, FULL_STOCK, 90)]
    starts = [(SPAWN[0], SPAWN[1], 6),          # dir 6 = west, toward the bases
              (FOE_SPAWN[0], FOE_SPAWN[1], 8)]  # dir 8 = south, down the lane

    with open(output, 'wb') as f:
        f.write(b'BMAPBOLO')
        f.write(struct.pack('B', 1))
        f.write(struct.pack('B', 0))            # no pillboxes at all
        f.write(struct.pack('B', len(bases)))
        f.write(struct.pack('B', len(starts)))
        for x, y, owner, armour, shells, mines in bases:
            f.write(struct.pack('BBBBBB', x, y, owner, armour, shells, mines))
        for x, y, d in starts:
            f.write(struct.pack('BBB', x, y, d))
        f.write(encode_map_runs(terrain))

    sidecar = str(Path(output).with_suffix('')) + ".scenario.lua"
    write_sidecar(sidecar)

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  and {sidecar}")
    print(f"  spawn {SPAWN}; mined gap {GAP} in the x={BARRIER_X} barrier; "
          f"BASE_NEAR {BASE_NEAR} (mdist {mdist(SPAWN, BASE_NEAR)}, armour "
          f"{NEAR_ARMOUR}, shells {NEAR_SHELLS}); BASE_FAR {BASE_FAR} "
          f"(mdist {mdist(SPAWN, BASE_FAR)}, full)")
    print(f"  patrol island x{ISLAND_X[0]}..{ISLAND_X[1]} behind the x={MOAT_X} "
          f"moat; opponent start {FOE_SPAWN}, lane x={PATROL_X} "
          f"y={PATROL_Y0}..{PATROL_Y1}")
    print(f"  contested swing: near {contested_swing(BASE_NEAR):.0f}, "
          f"far {contested_swing(BASE_FAR):.0f}")


if __name__ == '__main__':
    main()
