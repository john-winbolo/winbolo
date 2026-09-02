#!/usr/bin/env python3
"""
Generate the refuel-stickiness arena (companion to
tests/refuel_stickiness_test.py).

Field incident 20260902_120856: the bot sat with two of its own bases thirteen
tiles apart -- (137,143) and (137,130) -- both reachable, both priced within a
few tens of each other.  A moving enemy tank kept crossing the
CONTESTED_BASE_RANGE disk of first one and then the other, the two scores
swapped every replan, pool 1 published a different winner each time, and the
tank drove back and forth between them until it died having reached neither.
Separately, a base holding four shells priced exactly the same as one holding
ninety, because REFUEL_MIN_STOCK is a threshold REJECT and nothing above it
cost anything.

Two brain changes came out of that, and this arena exercises both:

  1. REFUEL TARGET STICKINESS.  pool 1's winner IS the refuel target, and it
     is now held: a rival must cost less than REFUEL_TARGET_SWITCH_RATIO (0.6)
     x the held target's CURRENT score, and at least
     REFUEL_TARGET_MIN_HOLD_TICKS (150) must have passed since the last
     (re)target, before the target moves.  Released on docking, on the held
     base dropping out of the viable set, on refuel completion, or after
     REFUEL_TARGET_HOLD_MAX_TICKS (3000).

  2. STOCK-AWARE PRICING.  Every refuel candidate with a FRESH stock
     observation now pays a SHORTFALL surcharge for whatever it cannot supply
     toward the tank's target, clamped at REFUEL_SHORTFALL_CAP (400).  How that
     surcharge is derived is goals.lua's business and has already changed shape
     once; this arena only guarantees that the near base genuinely cannot cover
     the top-up, so the term has to be positive whatever the formula is.

THE ARENA (one map, two stock sidecars)

    Everything is written between x 110..142 and y 110..142, symmetric about
    (126,126), so mapRead's recenter is a no-op and in-game tiles match this
    file.  Three vertical bands:

      x 110..111   PATROL ISLAND (grass)
      x 112        MOAT          (deep sea, the full height of the map body)
      x 113..142   MAIN FIELD    (grass) -- the bot's world

    (126,127)  the bot's start: a ONE-TILE deep-sea pond in the main field.
               A start square has to be DEEP SEA (starts.c
               startsIsValidSquare), so every arena in this suite digs one.
    (120,131)  BASE_NEAR  -- mdist 10 from the spawn
    (120,118)  BASE_FAR   -- mdist 15 from the spawn
               ...thirteen tiles apart, the incident's geometry, and priced
               within about ten of each other.
    (110,112)  the scripted opponent's start pond, ON THE ISLAND.
    x=110      its patrol lane: it drives south off the pond and then paces
               y=116 <-> y=133 forever, level with BASE_FAR at one end and
               with BASE_NEAR at the other.

WHY THE OPPONENT IS ON AN ISLAND

    contested_penalty measures mdist to the base -- it does not care whether
    the enemy can be reached.  attack_tank does: an unreachable tank costs
    INF and never wins a pool.  Putting the jitter source across a moat
    therefore gives the arena the one thing it wants (a moving hostile tank
    swinging each base's contested term) without the thing it does not (the
    bot abandoning refuel to go hunting, which drags the tank tens of tiles
    and swamps the contested jitter with travel-cost swings).

    The moat is uncrossable for good: the bot's spawn boat is stranded the
    moment it drives onto grass, and there is no river anywhere on the map, so
    no boat can ever be built.  Same trick as tests/generate_take_cover_map.py.

    Sizing: from lane x=110 the nearest approach to a base at x=120 is 10
    tiles, so contested_penalty peaks at 120 x (1 - 10/15) = 40 and falls to 0
    at the far end of the lane -- and the two bases peak in ANTIPHASE: level
    with BASE_FAR (lane y=118) the near base reads exactly 0, and level with
    BASE_NEAR (lane y=131) the far base reads exactly 0.  That is more than the ~10 of travel that
    separates the two bases, so the RAW winner flips every lap; it is
    comfortably less than what the 0.6 switch ratio demands, so the HELD
    target should not move.  That gap is the whole test.

WHY THE RUN IS TOURNAMENT + -ranked

    TOURNAMENT with ZERO neutral bases means gameTypeGetItems hands every tank
    0 shells / 0 mines / 0 trees and full armour (gametype.c: shells =
    2 * (neutralBases / numBases) * 100).  The bot has to WANT to refuel, and
    a full 40/40 open loadout wants nothing -- pool 1 is declined outright at
    target.  It also disarms the opponent completely: no shells means it
    cannot kill the bot, cannot shoot a base down to capturable, and cannot
    anger anything.

    -ranked is REQUIRED, and not for ranked play: serverSimApplyScenarioCommit
    stamps gameScripted over the game type of any map with a scenario sidecar,
    and gameScripted gets the same full tank as gameOpen -- which would put 40
    shells in the tank and there would be nothing to refuel.  `!sim->ranked` is
    the one branch that leaves the type alone.  (Same reason
    tests/respawn_loadout_test.py passes it.)

    Tournament also makes both starts deterministic
    (startsGetStartTournament): the bot's start has an OWN base within
    START_BASE_RANGE (9) so it lands in ownCandidates, a tier the opponent --
    to whom those bases are hostile -- can never reach; the opponent's start
    has no base within 9 of it, so it is not an ownCandidate and the bot can
    never be given it.

THE TWO VARIANTS (identical map, different sidecar stock)

  A  stickiness.  Both bases fully stocked (armour 90, shells 90).  The scores
     stay comparable all run and the patrol keeps swapping the raw order.

  B  stock-aware.  Identical, except BASE_NEAR -- the closer one -- is pinned
     at NEAR_SHELLS shells.  It must stay at or above REFUEL_MIN_STOCK (5) or
     the low_stock REJECT fires upstream and the row is never priced at all,
     which would test nothing; 6 is the smallest value that survives the
     reject (the brief asked for 4 -- at 4 the row is rejected outright).
     Against the tank's live shell target (25 in practice) it can supply 6 of
     the 25 shells wanted, so its shortfall term is large and positive while
     the full base's is exactly 0 -- far more than the ~10 that BASE_NEAR's
     five-tile head start is worth.  The bot must drive PAST the near base.

Usage:
    python3 tests/generate_refuel_stickiness_map.py [A|B] [output_path]
    Default: variant A -> tests/refuel_stickiness_A.map
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
FIELD_X = (113, 142)           # the bot's world
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
NEAR_SHELLS_B = 6              # variant B: BASE_NEAR's pinned shell stock
STOCK_PERIOD = 150             # sidecar re-assert cadence, in server ticks

# -- Brain/engine constants this arena is designed against -------------------
CONTESTED_BASE_RANGE = 15      # constants.lua
CONTESTED_BASE_PENALTY = 120   # constants.lua
REFUEL_MIN_STOCK = 5           # constants.lua -- below this the row is REJECTed
REFUEL_SHORTFALL_PER_SHELL = 12   # constants.lua
REFUEL_SHORTFALL_PER_ARMOUR = 8   # constants.lua
REFUEL_SHORTFALL_CAP = 400        # constants.lua
REFUEL_TARGET_SWITCH_RATIO = 0.6  # constants.lua
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


def shells_short(obs_shells, carried, target):
    """How many shells of the top-up this base CANNOT supply -- the quantity
    goals.lua refuel_shortfall turns into a surcharge.  The surcharge formula
    itself lives in the brain and is deliberately not modelled here (it has
    already changed shape once); the arena only has to guarantee this number is
    big for BASE_NEAR and zero for BASE_FAR."""
    need = max(0, target - carried)
    return need - min(need, obs_shells)


def near_shells(variant):
    return FULL_STOCK if variant == "A" else NEAR_SHELLS_B


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    y0, y1 = BODY_Y
    for yy in range(y0, y1 + 1):
        for xx in range(ISLAND_X[0], ISLAND_X[1] + 1):
            t[yy][xx] = GRASS
        for xx in range(FIELD_X[0], FIELD_X[1] + 1):
            t[yy][xx] = GRASS
        # MOAT_X is left as background deep sea.
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
-- Scenario sidecar for tests/refuel_stickiness_{V}.map -- GENERATED by
-- tests/generate_refuel_stickiness_map.py, do not edit by hand.
--
-- Three jobs.
--
-- 1. OWN BOTH BASES, and set their stock.  The bases are already written into
--    the map file as slot {SLOT}'s, but on_setup re-asserts it so the intent is
--    in one place -- and it is the only place the per-variant stock lives:
--      variant A  both bases armour {FULL} shells {FULL}
--      variant B  BASE_NEAR pinned at shells {NEAR_SH}, BASE_FAR left full
--
-- 2. KEEP THE STOCK READING FRESH.  refuel_shortfall only prices a base whose
--    observation is younger than REFUEL_OBS_STALE (500 ticks), and a bot only
--    learns base stock from EVENT_BASE_STOCK, which the server emits ONLY when
--    a base's armour/shells/mines actually CHANGE.  A base nobody touches goes
--    quiet, its reading ages out, the stock term prices at zero and variant B
--    tests nothing.  So every {PERIOD} ticks this re-asserts the stock with a
--    mines value that alternates 90/89 -- a real change, so a real event.  It
--    also tops the shells back up, which is deliberate: the near base has to
--    stay pinned at {NEAR_SH} even while the tank drinks from it.
--
-- 3. FILL THE SPAWN PONDS once both tanks are ashore.  A start square has to
--    be DEEP SEA (starts.c startsIsValidSquare), so the arena has two one-tile
--    ponds sitting in grass.  A tank that later drives over one without a boat
--    drowns instantly -- noise this test does not want to measure -- so once
--    neither tank is afloat the ponds become grass.

local NEAR   = {{ {NX}, {NY} }}
local FAR    = {{ {FX}, {FY} }}
local FULL   = {FULL}
local NEAR_SH = {NEAR_SH}
local OWNER  = {SLOT}
local PERIOD = {PERIOD}
local PONDS  = {{ {{ {SPX}, {SPY} }}, {{ {EPX}, {EPY} }} }}
local GRASS  = 7

local flip = 0
local ponds_filled = false

-- armour, shells for one base.  BASE_NEAR is the only one that varies.
local function stock_for(b)
  if b.x == NEAR[1] and b.y == NEAR[2] then return FULL, NEAR_SH end
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
        "REFUEL_STICK_ARENA base#%d (%d,%d) owner=%d armour=%d shells=%d",
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
      g.message("REFUEL_STICK_ARENA spawn ponds filled at tick " .. tostring(tick))
    end
  end
end
'''


def write_sidecar(path, variant):
    text = SIDECAR.format(
        V=variant, SLOT=BOT_SLOT, FULL=FULL_STOCK,
        NEAR_SH=near_shells(variant), PERIOD=STOCK_PERIOD,
        NX=BASE_NEAR[0], NY=BASE_NEAR[1],
        FX=BASE_FAR[0], FY=BASE_FAR[1],
        SPX=SPAWN[0], SPY=SPAWN[1],
        EPX=FOE_SPAWN[0], EPY=FOE_SPAWN[1])
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def main():
    args = list(sys.argv[1:])
    variant = "A"
    if args and args[0].upper() in ("A", "B"):
        variant = args.pop(0).upper()
    output = args[0] if args else str(
        Path(__file__).parent / f"refuel_stickiness_{variant}.map")
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

    # -- Variant B: the near base has to SURVIVE the low_stock reject and still
    #    pay a shortfall that no plausible travel saving can beat.  Measured
    #    against the SHELLS_LOW-ish live target the brain actually uses (25 in
    #    practice), not TANK_FULL_SHELLS, so the number quoted here is the
    #    pessimistic one.
    assert NEAR_SHELLS_B >= REFUEL_MIN_STOCK, (
        f"{NEAR_SHELLS_B} shells is below REFUEL_MIN_STOCK "
        f"({REFUEL_MIN_STOCK}); the row would be REJECTed low_stock and never "
        "priced at all")
    assert shells_short(NEAR_SHELLS_B, 0, 25) >= 15, (
        "BASE_NEAR can very nearly cover the top-up; its shortfall term would "
        "be too small to beat the travel saving")
    assert shells_short(FULL_STOCK, 0, 25) == 0, (
        "BASE_FAR must be able to cover the whole top-up, or the two rows are "
        "not a clean contrast")

    # Base records: x, y, owner, armour, shells, mines.  Owned by the bot from
    # the map file; the sidecar re-asserts it (and the per-variant stock).
    bases = [(BASE_NEAR[0], BASE_NEAR[1], BOT_SLOT, FULL_STOCK,
              near_shells(variant), 90),
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
    write_sidecar(sidecar, variant)

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  and {sidecar}")
    print(f"  variant {variant}: spawn {SPAWN}; BASE_NEAR {BASE_NEAR} "
          f"(mdist {mdist(SPAWN, BASE_NEAR)}, shells {near_shells(variant)}, "
          f"{shells_short(near_shells(variant), 0, 25)} of a 25-shell top-up "
          f"unsupplied); BASE_FAR {BASE_FAR} (mdist {mdist(SPAWN, BASE_FAR)}, "
          f"shells {FULL_STOCK}, "
          f"{shells_short(FULL_STOCK, 0, 25)} unsupplied)")
    print(f"  patrol island x{ISLAND_X[0]}..{ISLAND_X[1]} behind the x={MOAT_X} "
          f"moat; opponent start {FOE_SPAWN}, lane x={PATROL_X} "
          f"y={PATROL_Y0}..{PATROL_Y1}")
    print(f"  contested swing: near {contested_swing(BASE_NEAR):.0f}, "
          f"far {contested_swing(BASE_FAR):.0f} "
          f"(switch bar is {REFUEL_TARGET_SWITCH_RATIO} x the held score)")


if __name__ == '__main__':
    main()
