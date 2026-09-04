#!/usr/bin/env python3
"""
Generate the steal-handshake arenas (companion to tests/steal_handshake_test.py).

Field incident 20260903_193428_1, bot3, t=1266-1270.  p4 sent bot3 TWO steal
requests back to back -- `/info stq 4 3 188` and `/info stq 0 3 379`, pills #4
and #0 -- while p4's own goal was attack_pill #5 and stayed #5 the whole time.
Bot3 yielded both, and its pool 6 then carried pills #0 and #4 as ally_claimed
by p4 for the full STEAL_YIELD_BLOCK (300 ticks; still showing at t=1487) for
takes p4 never went near.  Two causes:

  * The handshake compared RAW cost_cache costs.  Bot3's own goal selection had
    priced pill #0 at 223.6 (`total=223.6 base=315.9 pen=-92.3 hyst=type` -- the
    commitment/hysteresis discount it had earned by already being on the take)
    but advertised and replied with the raw 423, so p4's raw 379 cleared the
    10% band by 1.7 points.
  * The challenger asked for every ally-claimed row that looked cheap on paper,
    not just the one it would actually pick.

TWO VARIANTS, one map each.  Both share the same terrain, spawns, bases and
opponent; they differ ONLY in how many pills there are.

  DUEL (one pill).  PILL_A is the only take on the map and it is exactly the
      same distance from each bot, so neither owns it by geometry and the claim
      is settled entirely by cost.  Produces the full negotiation: a dual-hold
      race, an stq, a holder's sta quoting its COMPETED price, and -- once the
      winner's advert moves on to something else (the island tank, explore, its
      base) -- an early end to the loser's yield block.  This is the arena for
      "the holder's commitment counts" and "the yield block ends when the
      stealer targets something else".

  BUSY (two pills).  PILL_A is the same contested pill; PILL_B sits 5 tiles from
      bot1 and 31 from bot0, so bot1 always has a cheaper take of its own.  That
      is the incident's shape -- a challenger committed elsewhere -- and it is
      the arena for "a steal request only goes out for the row we would actually
      pick".  Bot1 prices PILL_A below bot0's advertised cost often enough to
      want to ask, and must not, because PILL_B is what it would take.

THE ARENA
    Everything is written between x 106..146 and y 106..146, symmetric about
    (126,126), so mapRead's recenter is a no-op and in-game tiles match this
    file.  Vertical bands, west to east:
      x 106..108   PATROL ISLAND (grass)
      x 109..110   MOAT          (deep sea, the full height of the map body)
      x 111..146   FIELD         (grass) -- everything else lives here
    (126,126)  PILL_A, NEUTRAL and alive: the CONTESTED pill, 13 tiles from
               each bot.  Both variants have it; it is brain pill #0.
    (113,126)  BOT0 start pond          (139,126)  BOT1 start pond
    (144,126)  PILL_B, NEUTRAL and alive: BUSY variant only, brain pill #1.
    (114,140)  BASE0 (bot0's)           (138,112)  BASE1 (bot1's)
               Both 26 tiles from PILL_A, so refuel is a real fallback goal
               with a target id (which is what a released yield reads off the
               stealer's advert) without ever out-competing the take.
    (107,110)  the scripted opponent's start pond, ON THE ISLAND, and its
               patrol lane x=107 from y=114 to y=138.

    BOTH PILLS ARE NEUTRAL.  A pill handed to an ALLIED player in the map file
    is still read as a hostile take by the other bot -- checked in-game on this
    arena: the engine reported owner=1 and bot0 shelled it through to
    shoot_pill anyway -- so "an ally's own pill" is not a thing this arena can
    build, and it does not need one.

WHY THE OPPONENT IS ON AN ISLAND
    The bots need a live hostile tank -- danger, threat, a reason for the game
    not to be over -- without a hunt: attack_tank prices an unreachable tank at
    INF and never picks it, while the danger field does not care about
    reachability.  The moat is two columns of deep sea the full height of the
    body and there is no river anywhere, so no boat can ever be built and the
    island is permanent.  (Same trick as tests/generate_refuel_lowstock_map.py.)

WHY THE RUN IS `-gametype open -teams 2,1`
    OPEN hands every tank the full 40/40 loadout (gametype.c), so both bots can
    actually shoot a pill down, and it leaves -teams alone (TOURNAMENT would
    override it).  `-teams 2,1` is the CONTIGUOUS-BLOCK form of the flag
    (servermain.c): the first 2 bots are one team -- the allies whose handshake
    is under test -- and the third is the opponent.  `-teams 2` without the
    comma is the other form entirely (round-robin into 2 teams) and would make
    the two GoalHunters enemies, which tests nothing.

Usage:
    python tests/generate_steal_handshake_map.py [duel|busy] [output_path]
    Default: writes both, as tests/steal_handshake_<variant>.map
Writes <output>.map and the matching <output>.scenario.lua sidecar.
"""

import struct
import sys
from pathlib import Path

GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)
MAP_SIZE = 256

VARIANTS = ("duel", "busy")

# -- Geometry (the test runner imports these for its assertions) -------------
BODY_Y = (106, 146)            # y extent of every band
ISLAND_X = (106, 108)          # patrol island
MOAT_X = (109, 110)            # the uncrossable columns
FIELD_X = (111, 146)           # the bots' world

PILL_A = (126, 126)            # the CONTESTED pill (both variants), brain id 0
PILL_B = (144, 126)            # bot1's own cheaper take (BUSY only), brain id 1
BOT0_SPAWN = (113, 126)        # mdist 13 to PILL_A, 31 to PILL_B
BOT1_SPAWN = (139, 126)        # mdist 13 to PILL_A,  5 to PILL_B
FOE_SPAWN = (107, 110)         # opponent start pond, on the island
BASE0 = (114, 140)             # bot0's base
BASE1 = (138, 112)             # bot1's base
PONDS = [BOT0_SPAWN, BOT1_SPAWN, FOE_SPAWN]

PATROL_X = 107                 # the lane the opponent settles into
PATROL_Y0 = 114                # north turnaround (south of FOE_SPAWN, so the
PATROL_Y1 = 138                # pond is never re-crossed once it is left)

# -- Object state ------------------------------------------------------------
NEUTRAL = 255
# Pill health both pills are born with.  It has to be ALIVE (> 0, so the pill is
# an attack_pill candidate at all) and BELOW HARD_TAKE_MIN_HP, because a pill at
# or above that is a "hard take": squad.update elects a COMMANDER for it, the
# commander opens a blitz call, and the pool-6 blitz exemption in
# sync_ally_claimed_rejects then clears the ally-claimed reject outright --
# "join via the squad layer, never steal".  A blitzed pill is never negotiated
# for, so at the full 15 HP this arena produced a textbook two-bot blitz and not
# one steal request.  At 8 HP each bot prices a normal SOLO take, the second one
# to want it is an ordinary challenger, and the stq/sta handshake is what
# decides the pill -- which is the thing under test.
ALIVE_HP = 8
HARD_TAKE_MIN_HP = 12          # constants.lua -- at/above this a blitz forms
# pillbox.c pillsReadMapFile reads this as the reload counter; PILLBOX_ATTACK_
# NORMAL is 100.  200 = fires half as often, so the pill is a real take (it
# shoots back, both bots price danger) without deleting a bot mid-negotiation.
SLOW_RELOAD = 200
FULL_STOCK = 90                # base armour / shells

# -- Brain/engine constants these arenas are designed against ----------------
PILLBOX_RANGE = 8              # tiles a pill can shoot (pillbox.c)
STEAL_YIELD_BLOCK = 300        # constants.lua -- the ceiling a release beats
STEAL_YIELD_RELEASE_GRACE = 60 # constants.lua -- the floor a release waits out
GOAL_REPLAN_INTERVAL = 50      # constants.lua -- one request per replan


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def pill_list(variant):
    """The pills of a variant, in map-file order.  Index i here is brain pill
    id i (the brain is 0-based; the scenario API is 1-based)."""
    pills = [PILL_A]
    if variant == "busy":
        pills.append(PILL_B)
    return pills


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    y0, y1 = BODY_Y
    for yy in range(y0, y1 + 1):
        for xx in range(ISLAND_X[0], ISLAND_X[1] + 1):
            t[yy][xx] = GRASS
        for xx in range(FIELD_X[0], FIELD_X[1] + 1):
            t[yy][xx] = GRASS
        # MOAT_X columns are left as background deep sea.
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
-- Scenario sidecar for tests/steal_handshake_{VAR}.map -- GENERATED by
-- tests/generate_steal_handshake_map.py, do not edit by hand.
--
-- Two jobs.  The PILLS are deliberately not touched: they are NEUTRAL in the
-- map file and stay that way (see the generator's header for why an "allied"
-- pill is not buildable here).
--
-- 1. OWN THE BASES.  They are already slot 0's and slot 1's in the map file,
--    but on_setup re-asserts owner and full stock so the intent is in one
--    place.  The bots start full, so refuel never wins; what the bases are for
--    is being a real fallback goal WITH A TARGET ID, which is what a released
--    steal-yield reads off the stealer's advert.
--
-- 2. FILL THE SPAWN PONDS once every tank is ashore.  A start square has to be
--    DEEP SEA (starts.c startsIsValidSquare), so the arena has three one-tile
--    ponds sitting in grass.  A tank that later drives over one without a boat
--    drowns instantly -- noise this test does not want -- so once nobody is
--    afloat the ponds become grass.
local BASES  = {{ {{ {B0X}, {B0Y}, 0 }}, {{ {B1X}, {B1Y}, 1 }} }}
local FULL   = {FULL}
local PONDS  = {{ {{ {P0X}, {P0Y} }}, {{ {P1X}, {P1Y} }}, {{ {P2X}, {P2Y} }} }}
local GRASS  = 7
local ponds_filled = false

function on_setup(g)
  for _, b in ipairs(BASES) do
    for i = 1, g.num_bases() do
      local bb = g.base(i)
      if bb and bb.x == b[1] and bb.y == b[2] then
        g.set_base_owner(i, b[3])
        g.set_base_stock(i, FULL, FULL, 90)
      end
    end
  end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      -- 1-based here, 0-based in the brain: scenario pill i is brain pill i-1.
      g.message(string.format("STEAL_ARENA pill#%d (%d,%d) owner=%d armour=%d",
                              i - 1, p.x, p.y, p.owner, p.armour))
    end
  end
end

function on_tick(g, tick)
  -- Fill the ponds as soon as nobody is afloat any more (see note 2).
  if not ponds_filled and tick > 200 then
    local afloat = false
    for p = 0, g.max_tanks() - 1 do
      local t = g.tank(p)
      if t and t.boat then afloat = true end
    end
    if not afloat then
      ponds_filled = true
      for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
      g.message("STEAL_ARENA spawn ponds filled at tick " .. tostring(tick))
    end
  end
end
'''


def write_sidecar(path, variant):
    text = SIDECAR.format(
        VAR=variant,
        B0X=BASE0[0], B0Y=BASE0[1], B1X=BASE1[0], B1Y=BASE1[1],
        FULL=FULL_STOCK,
        P0X=PONDS[0][0], P0Y=PONDS[0][1],
        P1X=PONDS[1][0], P1Y=PONDS[1][1],
        P2X=PONDS[2][0], P2Y=PONDS[2][1])
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def check_geometry(terrain, variant):
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

    # -- The moat has to run the FULL height of the body, or the island is
    #    reachable on foot and attack_tank comes back.
    for y in range(BODY_Y[0], BODY_Y[1] + 1):
        for x in range(MOAT_X[0], MOAT_X[1] + 1):
            assert terrain[y][x] is DEEP_SEA, (x, y)
    assert GRASS == 7, "the field is plain grass; no river => no boat, ever"

    # -- THE contested geometry: PILL_A is exactly as far from one bot as the
    #    other, so nothing but cost decides the claim.
    assert mdist(BOT0_SPAWN, PILL_A) == mdist(BOT1_SPAWN, PILL_A), (
        mdist(BOT0_SPAWN, PILL_A), mdist(BOT1_SPAWN, PILL_A))
    #    Neither bot spawns inside the pill's range: they have to drive in, so
    #    both spend time in plan_position/approach, which is the ONLY window a
    #    pre-commit holder can be asked to yield in.
    for s in (BOT0_SPAWN, BOT1_SPAWN):
        assert mdist(s, PILL_A) > PILLBOX_RANGE, (s, mdist(s, PILL_A))

    if variant == "busy":
        # PILL_B is bot1's OWN cheaper take and never bot0's business.  That is
        # what makes bot1 a challenger that is committed elsewhere: a request
        # for PILL_A has to beat PILL_B, and it does not.
        assert mdist(BOT1_SPAWN, PILL_B) < mdist(BOT1_SPAWN, PILL_A), (
            mdist(BOT1_SPAWN, PILL_A), mdist(BOT1_SPAWN, PILL_B))
        assert mdist(BOT0_SPAWN, PILL_B) > 2 * mdist(BOT0_SPAWN, PILL_A), (
            "PILL_B must never be bot0's cheapest take, or both bots chase it")
        assert mdist(PILL_A, PILL_B) > PILLBOX_RANGE, (
            "the two pills must not shoot at each other's approach")

    #    Neither pill may be a HARD TAKE, or a blitz forms on it and the pool-6
    #    blitz exemption clears the ally claim before the handshake ever runs.
    assert 0 < ALIVE_HP < HARD_TAKE_MIN_HP, (ALIVE_HP, HARD_TAKE_MIN_HP)

    # -- Bases: real fallback goals (with a target id, which is what a released
    #    yield reads off the stealer's advert) that never beat a take.
    for s, b in ((BOT0_SPAWN, BASE0), (BOT1_SPAWN, BASE1)):
        assert mdist(s, b) > mdist(s, PILL_A), (s, b)
    for b in (BASE0, BASE1):
        assert mdist(b, PILL_A) > PILLBOX_RANGE + 8, (b, mdist(b, PILL_A))

    # -- Everything in the right band.
    for p in list(pill_list(variant)) + [BOT0_SPAWN, BOT1_SPAWN, BASE0, BASE1]:
        assert FIELD_X[0] <= p[0] <= FIELD_X[1], p
        assert BODY_Y[0] <= p[1] <= BODY_Y[1], p
    assert ISLAND_X[0] <= FOE_SPAWN[0] <= ISLAND_X[1], FOE_SPAWN
    assert ISLAND_X[0] <= PATROL_X <= ISLAND_X[1], PATROL_X
    assert not (PATROL_Y0 <= FOE_SPAWN[1] <= PATROL_Y1), (
        "the patrol lane runs over the opponent's start pond -- it would drown "
        "the moment the sidecar has not yet filled it in")
    for y in range(PATROL_Y0, PATROL_Y1 + 1):
        assert terrain[y][PATROL_X] is not DEEP_SEA, (PATROL_X, y)
    # No two tanks may share a start tile, and no start may sit on an object.
    assert len(set(PONDS)) == 3, PONDS
    for p in PONDS:
        assert p not in list(pill_list(variant)) + [BASE0, BASE1], p

    # -- The release window has to be wide enough to observe: the stealer needs
    #    at least a replan after the grace period and still finish inside the
    #    block it is beating.
    assert STEAL_YIELD_RELEASE_GRACE + GOAL_REPLAN_INTERVAL < STEAL_YIELD_BLOCK, (
        STEAL_YIELD_RELEASE_GRACE, STEAL_YIELD_BLOCK)


def write_variant(variant, output):
    terrain = make_map()
    check_geometry(terrain, variant)

    # Pill records: x, y, owner, armour, speed(reload).
    pills = [(p[0], p[1], NEUTRAL, ALIVE_HP, SLOW_RELOAD)
             for p in pill_list(variant)]
    # Base records: x, y, owner, armour, shells, mines.
    bases = [(BASE0[0], BASE0[1], 0, FULL_STOCK, FULL_STOCK, 90),
             (BASE1[0], BASE1[1], 1, FULL_STOCK, FULL_STOCK, 90)]
    # dir 4 = east / 12 = west in the 16-point start encoding: point each bot
    # at the contested pill so neither wastes its first second turning round.
    starts = [(BOT0_SPAWN[0], BOT0_SPAWN[1], 4),
              (BOT1_SPAWN[0], BOT1_SPAWN[1], 12),
              (FOE_SPAWN[0], FOE_SPAWN[1], 8)]

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
    print(f"wrote {output}")
    print(f"  {variant}: pill#0 {PILL_A} NEUTRAL hp={ALIVE_HP}, "
          f"{mdist(BOT0_SPAWN, PILL_A)} tiles from each bot"
          + (f"; pill#1 {PILL_B} NEUTRAL hp={ALIVE_HP}, "
             f"{mdist(BOT1_SPAWN, PILL_B)} tiles from bot1 and "
             f"{mdist(BOT0_SPAWN, PILL_B)} from bot0"
             if variant == "busy" else ""))
    print(f"  bases {BASE0} (bot0) / {BASE1} (bot1); opponent on the island at "
          f"{FOE_SPAWN}, lane x={PATROL_X} y={PATROL_Y0}..{PATROL_Y1}")
    print(f"wrote {sidecar}")


def main():
    args = list(sys.argv[1:])
    variants = list(VARIANTS)
    if args and args[0].lower() in VARIANTS:
        variants = [args.pop(0).lower()]
    explicit = args[0] if args else None
    if explicit and len(variants) != 1:
        print("an explicit output path needs an explicit variant", file=sys.stderr)
        return 1
    here = Path(__file__).parent
    for v in variants:
        out = explicit or str(here / f"steal_handshake_{v}.map")
        write_variant(v, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
