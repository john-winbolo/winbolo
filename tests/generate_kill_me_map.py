#!/usr/bin/env python3
"""
Generate the "loaded, builder-less" arenas (companion to tests/kill_me_test.py).

WHAT IS BEING MEASURED
----------------------
A tank carrying pillboxes it cannot place, because its builder is dead or a
long walk away, is holding a stack of free pillboxes for whoever kills it. The
brain change under test gives that state a name (state.loaded_no_lgm) and five
consequences:

  1  attacking costs x ATTACK_NO_BUILDER_MULT (10)
  2  escaping (flee_to_base / a triggered take_cover) enters with NO hysteresis
  3  capturing is priced as pure risk -- danger x20 instead of the cautious x5,
     no free-pill bonus, no cluster discount, and a route probe that refuses
     ANY predicted damage
  4  "kill me": the tank asks a team-mate whose builder IS aboard to shoot it
     and pocket the corpses
  5  (un-gated bug fix) state.lgm_stranded is cleared whenever the man is not
     out walking, not only when he climbs back into the tank

HOW THE GROUND PRODUCES THE STATE
---------------------------------
The state needs pills aboard AND a builder who is not coming back soon. Pills
aboard are given by the sidecar (game.give_pill). The builder is handled the
way tests/stranded_lgm_A.scenario.lua handles it, because that recipe is
already proven and needs nothing the scenario API does not have:

    x:      113..117  118 .. 123  124  125 126 127  128  129  130  131  132..139
    y=113..121   every tile BUILDING (walls, five deep or more)
    y=122      ##     ff  ff  ff   ff   ff  BB  ff   ff   ff   ##   ##    ##
    y=123..124 ##     ff  ff  ff   ff   ff  ff  ff   ff   ff   ##   ##    ##
    y=125      ##     ff  ff  ff   ff   ff  ff  ff   ff   ff   GG   ##    ##
    y=126      ##     ff  ff  ff   ff   ff  ff  ff   ~0   ff   GG   pp    ##
    y=127      ##     ff  ff  ff   ff   ff  ff  ff   ff   ff   GG   ##    ##
    y=128      ##     ff  ff  ff   ff   ff  ff  ff   ff   ff   ##   ##    ##
    y=129      ##     ff  ~1  ff   ff   ff  ff  ff   ff   ff   ##   ##    ##
    y=130      ##     ff  ff  ff   ff   ff  ff  ff   ff   ff   ##   ##    ##
    y=131..133 ##     ##  ##  ##   NC   ##  ##  ##   ##   ##   ##   ##    ##
    y=134      ##     ##  ##  ##   NP   ##  ##  ##   ##   ##   ##   ##    ##
    y=135..139   every tile BUILDING

    ##  BUILDING wall            ff  FOREST (every tile a tank can park on)
    ~0  p0's spawn pond          ~1  p1's spawn pond   (deep sea -> forest at t=60)
    GG  THE GATE column (forest at load, RIVER once the man is across)
    pp  OURPILL -- alive, 4 armour down: the errand that walks the man east
    cc  CORPSE  -- a second friendly pill; hidden in most arenas, knocked to
        0 armour in arena E so there is a body to price a capture on
    BB  p0's base       NC  the neutral's corridor      NP  the NEUTRAL pillbox

WHY EACH PIECE IS WHERE IT IS
-----------------------------
THE GATE is a three-tile COLUMN, not a tile. lgm.c lgmReturn walks a straight
line at the tank with a per-axis slide and never re-routes, so a one-tile hole
is a hole he walks around; a full-height column with BUILDING above and below
is a wall he cannot pass. MAP_MANSPEED_TRIVER is 0 (the man cannot enter river
at all) while MAP_SPEED_TRIVER is 3 (the tank can), so the flood strands the
man without sealing the tank in.

THE ONLY OPEN TILE EAST OF THE GATE IS OURPILL, and a LIVE pillbox is
impassable to a tank (mapGetSpeed -> MAP_SPEED_TPILLBOX). So the tank can never
be east of the gate when the sidecar floods it, and the flood always leaves the
man on the far side. The man gets there anyway, through the blessed-tile
exception his repair errand gives him.

EVERY TANK-PARKABLE TILE IS FOREST. pillbox.c skips a target that
utilIsTankInTrees() and is at least MIN_TREEHIDE_DIST (768 WU = 3 tiles) away
on either axis unless it just fired, and the neutral at (124,134) is 4+ tiles
south of every room tile on the y axis. So the neutral NEVER SHOOTS -- while
threat.lua's stamp, which knows nothing about firing, covers the room the whole
run. That is deliberate: every arena pins
cfg=RESCUE_LGM_SUPPRESS_BY_FIRE_AGE=false, whose gate is the STATIC field
(perc.under_fire = danger_at(our tile) > 0), so the permanent stamp keeps
rescue_lgm suppressed for the whole run. Without that pin the stranded man
would be fetched and the state would end before anything could be measured --
and with it, nothing ever actually shoots at the tank, so the arena is quiet.

OURPILL is ALIVE and exactly BUILDER_POOL_TOPUP_MIN_MISSING (4) armour down:
a corpse would be driven over and pocketed instead of repaired, and a top-up
costs ONE tree against a rebuild's four.

p0's BASE keeps the reposition pool from shooting our own pill down to move it
-- it wants a friendly base within PILL_FIRE_RANGE (8) of every live friendly
pill, and check() asserts that of OURPILL and of the corpse tile, so no arena
has to spend a -bot-init token on cfg=PILL_REPOSITION_ENABLED=false (the token
string is capped at 127 bytes and arena B needs the room). It also keeps the
tank fed, and it is far enough south-east to stay outside PILLBOX_RANGE of the
neutral -- a base tile is the ONE tile a parked tank is not in trees on.

TWO SPAWN PONDS, because every arena runs two bots. A start square has to be
DEEP SEA at map load (starts.c startsIsValidSquare); the sidecar fills both
back to FOREST once each tank is ashore, because a hole in the forest is a hole
in the tree cover the arena's silence depends on.

MAP FACTS THAT BITE (the list every generator in tests/ keeps):
  * mapRead recenters the terrain bounding-box midpoint to (126,126), and the
    WALLS are terrain. check() asserts the midpoint is already there.
  * mapRead puts ROAD under every map-file pill.
  * -gametype open starts a tank with TANK_FULL_TREES (40).

THE ARENAS (identical ground; the sidecar and the -bot-init tokens differ)
  A    the state, with an enemy tank in the room.  x10 on attack_tank, and the
       escape rows enter with no hysteresis.
  B    the control: A with cfg=ATTACK_NO_BUILDER_MULT=1 and
       cfg=ESCAPE_NO_BUILDER_SKIP_HYST=false -- today's behaviour.
  C    the hand-off: p1 is an ALLY with its builder aboard and full shells.
  D1   p1 is an ally whose BUILDER IS OUT (the sidecar keeps a pill damaged so
       the builder pool keeps sending him) -> the responder row rejects no_lgm.
  D2   p1 is an ally with cfg=KILL_ME_SHELL_MARGIN=90, so the shell gate can
       never be met -> the responder row rejects low_shells.
  E    capture pricing IN the state: a corpse under the neutral's fire.
  EK   the same, with the four capture knobs at their KEEL values.

This script writes BOTH the .map and the per-arena .scenario.lua, because the
seven sidecars differ by a handful of constants and hand-maintaining seven
copies of the same 150 lines is how they drift apart.

Usage:
    python3 tests/generate_kill_me_map.py [variant]
    variant: A | B | C | D1 | D2 | E | EK   (default: all)
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
MIN_TREEHIDE_TILES = 3           # tank.h MIN_TREEHIDE_DIST 768 WU
TOPUP_MIN_MISSING = 4            # constants.lua BUILDER_POOL_TOPUP_MIN_MISSING
TOPUP_ARMOUR = PILLS_MAX_HEALTH - TOPUP_MIN_MISSING   # 11

# ── geometry ─────────────────────────────────────────────────────────────
OUTER = (116, 136, 114, 138)      # x0, x1, y0, y1 -- the whole walled block
WALL_DEEP = 5
ROOM = (121, 127, 125, 129)       # the tanks' whole world: 7 x 5, all forest
# TWO gate COLUMNS, three tiles tall each. Two, because the tank's closest
# reachable tile to the errand pill is always the tile next to it -- with one
# column that tile IS the gate, and the stage-2 wall could never be laid
# without freezing the tank inside it. Three tall, because lgm.c lgmReturn
# slides per axis: a hole above or below the man's line is a hole he walks
# around.
GATE = [(gx, gy) for gx in (128, 129) for gy in (126, 127, 128)]
OURPILL = (130, 127)              # alive, TOPUP_ARMOUR: the ONLY tile east of the gate
# The second friendly pill. mapRead puts ROAD under every map-file pill, and a
# ROAD tile is not tree cover -- so WHERE this pill sits changes which tiles the
# neutral can shoot a parked tank on, and therefore changes the whole run. It
# gets two homes:
#   CORPSE_FAR  (arenas A/B/E/EK) the position those four were tuned against.
#   CORPSE_NEAR (arenas C/D1/D2)  four tiles from the neutral instead of seven,
#               which is where arena D1's ally actually walks its builder often
#               enough for the responder's no_lgm reject to be observed.
CORPSE_FAR = (122, 126)
CORPSE_NEAR = (124, 129)
CORPSE = CORPSE_FAR
NP = (124, 133)                   # the neutral pillbox
NP_CORRIDOR = [(124, y) for y in range(130, 133)]
NP_SPEED = 50
# The base sits in a SEALED pocket inside the wall block. It is there for one
# reason: the reposition pool wants a friendly base within PILL_FIRE_RANGE of
# every live friendly pill and shoots down the ones that have none, and that
# test is a DISTANCE test on world.bases -- it never asks whether the base can
# be driven to. Sealed, it satisfies the guard without ever becoming a refuel
# destination the neutral could shoot a parked tank on, and without costing a
# -bot-init token (the string is capped at 127 bytes and arena B needs room).
BASE = (126, 131)
# Three SPARE pillboxes for p0 to carry, in their own sealed pocket. mapRead
# puts ROAD under every map-file pill, so these are three road tiles with
# BUILDING on every side -- no tank and no man can ever reach them. The sidecar
# hides them at setup and hands them to p0 (show_pill + give_pill) on the flood
# tick.
SPARES = [(117, 126), (117, 127), (117, 128)]
SPAWN0 = (126, 127)               # p0: beside the gate, so its man's errand is short
SPAWN1 = (122, 128)               # p1: the far side of the room
TILE_JITTER = 0.708

VARIANTS = ("A", "B", "C", "D1", "D2", "E", "EK")


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def box_tiles(box):
    x0, x1, y0, y1 = box
    return [(x, y) for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)]


def open_tiles(np_on=True):
    """Everything that is not wall, in load order (the gate is forest at load)."""
    tiles = box_tiles(ROOM) + GATE + [OURPILL]
    if np_on:
        tiles += NP_CORRIDOR + [NP]
    return tiles


def euclid(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def build(np_on=True):
    """The ground, minus the neutral pillbox in the arenas that do not want it.

    THE NEUTRAL IS A TRADE, and it is made HERE (in the map) rather than with
    game.hide_pill, because a hidden pill is not invisible enough: the brain
    remembers a pill it has ever seen and threat.lua keeps stamping it, so a
    hidden neutral still put expo 5 on the tank's tile and still fired
    haul_flee_eval -- which is precisely the trigger arenas C/D need quiet."""
    t = blank()
    for (x, y) in box_tiles(OUTER):
        t[y][x] = BUILDING
    for (x, y) in open_tiles(np_on):
        t[y][x] = FOREST
    t[SPAWN0[1]][SPAWN0[0]] = DEEP_SEA
    t[SPAWN1[1]][SPAWN1[0]] = DEEP_SEA
    # (x, y, owner, armour, speed). Index order IS the sidecar's pill numbering:
    # 1 = OURPILL, 2 = CORPSE, 3 = the neutral. The neutral is written as a
    # slot-0 pill and handed to game.NEUTRAL by the sidecar in on_setup -- the
    # .map owner byte has no neutral encoding mapRead is guaranteed to keep.
    pills = [(OURPILL[0], OURPILL[1], 0, TOPUP_ARMOUR, 50),
             (CORPSE[0], CORPSE[1], 0, PILLS_MAX_HEALTH, 50)]
    if np_on:
        pills += [(NP[0], NP[1], 0, PILLS_MAX_HEALTH, NP_SPEED)]
    pills += [(sx, sy, 0, PILLS_MAX_HEALTH, 50) for (sx, sy) in SPARES]
    bases = [(BASE[0], BASE[1], 0, 90, 90, 90)]
    starts = [(SPAWN0[0], SPAWN0[1], 12),      # facing west, into the room
              (SPAWN1[0], SPAWN1[1], 4)]       # facing east, into the room
    return t, pills, bases, starts


def check(terrain, pills, bases, starts, np_on=True):
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    mid = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    assert mid == (126, 126), (
        f"terrain midpoint is {mid}, not (126,126) -- mapRead would shift "
        f"every coordinate in this file")
    for (sx, sy, _d) in starts:
        assert terrain[sy][sx] is DEEP_SEA, (
            f"start ({sx},{sy}) must be deep sea (starts.c startsIsValidSquare)")

    opens = set(open_tiles(np_on))
    assert SPAWN0 in opens and SPAWN1 in opens, "both spawn ponds must be open"
    assert SPAWN0 != SPAWN1, "the two bots need two different start squares"

    # SEALED, AND SEALED DEEP. A shell that misses chews a wall down a stage at
    # a time; a thin wall is breached inside one run and the bot goes exploring.
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

    # CONNECTED at load: the man walks out through the gate, and both tanks
    # share one room.
    seen, stack = {SPAWN0}, [SPAWN0]
    while stack:
        (xx, yy) = stack.pop()
        for n in ((xx - 1, yy), (xx + 1, yy), (xx, yy - 1), (xx, yy + 1)):
            if n in opens and n not in seen:
                seen.add(n)
                stack.append(n)
    assert seen == opens, (
        f"the open ground is not one connected piece at load -- unreachable: "
        f"{sorted(opens - seen)[:8]}")

    # ...AND CUT IN TWO once the sidecar floods the gate. That is the
    # stranding, and it has to be a CLEAN cut -- the return walk slides per
    # axis, so a single missing tile in the column is a way round it.
    after = opens - set(GATE)
    seen, stack = {SPAWN0}, [SPAWN0]
    while stack:
        (xx, yy) = stack.pop()
        for n in ((xx - 1, yy), (xx + 1, yy), (xx, yy - 1), (xx, yy + 1)):
            if n in after and n not in seen:
                seen.add(n)
                stack.append(n)
    assert OURPILL not in seen, (
        f"flooding the gate column does NOT cut {OURPILL} off -- the man would "
        f"walk home and nothing would ever be stranded")
    gx = {x for (x, _y) in GATE}
    assert len(gx) == 2, "the gate must be exactly TWO columns"
    assert sorted(gx) == list(range(min(gx), max(gx) + 1)), "the gate columns are not adjacent"
    gy = sorted({y for (_x, y) in GATE})
    assert gy == list(range(gy[0], gy[-1] + 1)), "a gate column has a hole in it"
    for cx in gx:
        for cy in gy:
            assert (cx, cy) in GATE, f"the gate is not a solid block at ({cx},{cy})"

    # THE ONLY OPEN TILE EAST OF THE GATE IS THE PILL, so no tank can follow
    # the man across and the flood always lands with the tanks on this side.
    gate_x = max(gx)
    east_open = [t for t in open_tiles(np_on) if t[0] > gate_x]
    assert east_open == [OURPILL], (
        f"the only open tile east of the gate must be OURPILL; found {east_open}")

    # THE ROOM IS ALWAYS TREE COVER, AND ALWAYS OUT OF THE NEUTRAL'S REACH.
    # This is what keeps the arena SILENT: the neutral stamps danger over the
    # room for ever (which is what suppresses rescue_lgm) and never fires.
    for (xx, yy) in box_tiles(ROOM) + GATE:
        if (xx, yy) in (SPAWN0, SPAWN1):
            continue          # deep sea at load; the sidecar fills it to forest
        if not np_on:
            continue
        assert terrain[yy][xx] == FOREST, (
            f"tank-reachable tile ({xx},{yy}) is not FOREST -- the neutral "
            f"could see and shoot a tank parked on it")
        assert (abs(xx - NP[0]) >= MIN_TREEHIDE_TILES
                or abs(yy - NP[1]) >= MIN_TREEHIDE_TILES), (
            f"tank-reachable tile ({xx},{yy}) is inside MIN_TREEHIDE_DIST of "
            f"the neutral at {NP} on BOTH axes -- it would be shot at there")

    # ...but still INSIDE the danger stamp, or perc.under_fire is false and the
    # pinned static rescue gate stops suppressing.
    assert (not np_on) or euclid(CORPSE, NP) <= PILL_RANGE_MAP, (
        f"the corpse tile {CORPSE} is outside the neutral's danger stamp, so "
        f"arena E would be pricing a capture with no danger in it")

    # THE WHOLE ROOM IS INSIDE THE NEUTRAL'S DANGER STAMP. Every arena pins
    # cfg=RESCUE_LGM_SUPPRESS_BY_FIRE_AGE=false, whose gate is the STATIC field
    # (perc.under_fire = danger_at(our tile) > 0). If a single tank-reachable
    # tile falls outside the stamp, the tank standing there fetches its
    # stranded man, the state ends and the arena measures nothing -- measured
    # on the first draft, whose 12x9 room left the north half uncovered and
    # whose bot picked rescue_lgm on 18 of 69 replans.
    for (xx, yy) in (box_tiles(ROOM) + GATE) if np_on else []:
        d = euclid((xx, yy), NP)
        assert d <= PILL_RANGE_MAP, (
            f"tank-reachable tile ({xx},{yy}) is {d:.2f} tiles from the "
            f"neutral at {NP}, outside the PILL_RANGE_MAP ({PILL_RANGE_MAP}) "
            f"danger stamp -- perc.under_fire would be false there and "
            f"rescue_lgm would fire")

    # THE BASE AND THE SPARES ARE SEALED. Both exist only as entries in
    # world.bases / world.pills; mapRead turns their tiles into ROAD, so the
    # only thing keeping them out of play is the wall around them.
    for spot, name in ([(BASE, "the base")]
                       + [(sp, f"spare pill {sp}") for sp in SPARES]):
        assert spot not in opens, (
            f"{name} at {spot} is on OPEN ground -- it would be a real "
            f"destination in the room instead of a bookkeeping entry")
        for (nx, ny) in ((spot[0] - 1, spot[1]), (spot[0] + 1, spot[1]),
                         (spot[0], spot[1] - 1), (spot[0], spot[1] + 1)):
            assert (nx, ny) in SPARES or (nx, ny) == BASE                    or terrain[ny][nx] == BUILDING, (
                f"{name} at {spot} leaks at ({nx},{ny}) -- a tank could drive to it")
    assert len(SPARES) >= 3, "the state needs LOADED_NO_LGM_MIN_PILLS (3) aboard"

    # REPOSITION GUARD, bought with geometry instead of a -bot-init token: the
    # reposition pool wants a friendly base within PILL_FIRE_RANGE of every
    # live friendly pill, and shoots down the ones that have none. Both ground
    # pills have to be inside that radius or the bot demolishes its own errand.
    PILL_FIRE_RANGE = 8
    for spot, name in ((OURPILL, "OURPILL"), (CORPSE, "the corpse tile")):
        assert euclid(BASE, spot) <= PILL_FIRE_RANGE, (
            f"{name} {spot} is {euclid(BASE, spot):.1f} tiles from the base "
            f"{BASE}, outside PILL_FIRE_RANGE ({PILL_FIRE_RANGE}) -- the "
            f"reposition pool would shoot it down to move it")

    for (px, py, _o, _a, _s) in pills:
        assert terrain[py][px] is not DEEP_SEA, f"pill ({px},{py}) is in the sea"
    for (bx, by, *_r) in bases:
        assert terrain[by][bx] is not DEEP_SEA, f"base ({bx},{by}) is in the sea"
    ourp = [p for p in pills if (p[0], p[1]) == OURPILL]
    assert ourp and ourp[0][3] == TOPUP_ARMOUR, "our pill's armour is wrong"
    assert PILLS_MAX_HEALTH - TOPUP_ARMOUR >= TOPUP_MIN_MISSING, (
        f"our pill is only {PILLS_MAX_HEALTH - TOPUP_ARMOUR} armour down, "
        f"under BUILDER_POOL_TOPUP_MIN_MISSING ({TOPUP_MIN_MISSING})")


# ── the sidecar ──────────────────────────────────────────────────────────
# One template, seven arenas. The per-arena knobs are the four Lua constants
# substituted at the top: whether p1 is an enemy, whether the corpse pill is
# hidden or knocked to 0, and whether the sidecar keeps re-damaging the second
# pill to hold p1's builder out of its tank.
SIDECAR = '''-- GENERATED by tests/generate_kill_me_map.py -- do not edit by hand.
-- Scenario sidecar for tests/kill_me_{V}.map (auto-loaded as <map>.scenario.lua).
-- Companion to tests/kill_me_test.py arena {V}.
--
-- WHAT THIS SIDECAR IS FOR: it puts player 0 into the LOADED, BUILDER-LESS
-- state -- three pillboxes aboard and a builder who is never coming home --
-- and it does it with the only tools the scenario API has, set_tile and
-- give_pill.  There is no LGM teleport and the sidecar cannot read the man's
-- position at all, so his whereabouts are inferred from something the engine
-- publishes and the brain cannot fake: OUR PILL'S ARMOUR.  p0's only errand in
-- this arena is a top-up of the pill at ({PX},{PY}), four armour down; when
-- game.pill() reports it at full the repair has landed, which means the man
-- walked to that tile and did the work there.  He is, at that instant, one
-- tile east of the gate.
--
-- THE GATE (x={GX0}..{GX1}, y={GY0}..{GY2}) then goes from FOREST to BUILDING, and that
-- is the whole stranding: lgm.c lgmReturn walks a straight line at the tank
-- with a per-axis slide and no re-route, so a full-height column he cannot
-- enter stops him for good.  The brain's own walk sim is that same straight
-- line, returns -1, and goals.lua's loaded_no_lgm reads a return ETA of STUCK.
--
-- WALL, NOT RIVER (which is what tests/stranded_lgm_A.scenario.lua floods
-- with).  River stops the MAN (MAP_MANSPEED_TRIVER 0) but not the TANK
-- (MAP_SPEED_TRIVER 3), and the stranded-LGM arena wants the rescue to stay
-- possible.  Here it must not be: measured on the first draft, the tank parked
-- on the river gate tile at t=1059 and the man simply walked up to it -- the
-- state ended 700 ticks after it started and there was nothing left to
-- measure.  A wall column ends the walk for good, in both directions.
--
-- ON THE SAME TICK the three pillboxes go aboard.  Order matters: give them
-- earlier and the bot places them instead of carrying them, and the errand
-- that walks the man out never happens.
--
-- TWO GUARDS ON THE FLOOD, both of which have to hold on the same tick:
--   * NEITHER tank is on the gate column, so we never drop a river under one;
--   * the pill is at full armour, still ours, and in nobody's tank -- an
--     armour rise on a pill that changed hands is not our repair.
-- AND A RETRY: when the two do not coincide the sidecar knocks the pill back
-- down to TOPUP_ARMOUR, the pool sends the man out again, and the next
-- completion is another chance.
--
-- TRACE.  Every change of ARMOUR, OWNER or IN_TANK on either of our pills,
-- plus the flood and every pill hand-out, is written to kill_me_{V}_trace.log
-- in SIM ticks, so "the state was real" and "the corpses were taken" are asked
-- of the ENGINE and never of the brain's opinion of itself.
-- Rows are `tick x y armour owner in_tank`, one per CHANGE.

local TRACE     = "kill_me_{V}_trace.log"
local P0, P1    = 0, 1
local P1_ENEMY  = {P1_ENEMY}      -- arenas A/B: p1 is a hostile tank
local CORPSE_ON = {CORPSE_ON}     -- arena E: pill 2 becomes a 0-armour body
local BUSY_P1   = {BUSY_P1}       -- arena D1: keep pill 2 damaged so p1's man walks
local NP_ON     = {NP_ON}         -- is the neutral pillbox on the map at all?
local KILL_LGM  = {KILL_LGM}      -- arenas C/D1/D2: kill p0's builder outright
                                  -- (game.kill_lgm) instead of walling him in
local last_dead0 = false          -- trace: p0's tank alive/dead edge
local GIVE_N    = 3               -- pills handed to p0 (>= LOADED_NO_LGM_MIN_PILLS)

local OURPILL   = {{ {PX}, {PY} }}
local CORPSE    = {{ {CX}, {CY} }}
local NEUTRAL   = {{ {NX}, {NY} }}
local SPAWN0    = {{ {S0X}, {S0Y} }}
local SPAWN1    = {{ {S1X}, {S1Y} }}
local GATE_X    = {{ {GX0}, {GX1} }}
local GATE_Y    = {{ {GY0}, {GY1}, {GY2} }}
local FOREST    = 5
local RIVER     = 1
local WALL      = 0
local FILL_TICK = 60
local TOPUP_ARMOUR = {TOPUP}

local last = {{}}
local filled0, filled1 = false, false
local walled = false
local retry_at = nil
local REDAMAGE_AFTER = 8
local gave = false
local traced = nil
local idx_our, idx_corpse, idx_neutral = nil, nil, nil
local spares = {{}}

local function same(p, t) return p.x == t[1] and p.y == t[2] end

local function note(line)
  local f = io.open(TRACE, "a")
  if f then f:write(line .. "\\n") f:close() end
end

-- Resolve tiles to PILL INDICES once. Everything after this reads the pill BY
-- INDEX: a pill that is picked up moves with its carrier, so a coordinate
-- match would stop finding it exactly when the trace exists to prove nobody
-- took it.
local function resolve(g)
  spares = {{}}
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      if same(p, OURPILL)     then idx_our = i
      elseif same(p, CORPSE)  then idx_corpse = i
      elseif same(p, NEUTRAL) then idx_neutral = i
      else spares[#spares + 1] = i end
    end
  end
  return (idx_our ~= nil)
end

function on_setup(g)
  resolve(g)
  for i = 1, g.num_pills() do
    if i == idx_neutral then
      g.set_pill_owner(i, g.NEUTRAL)
    else
      g.set_pill_owner(i, P0)
    end
  end
  for i = 1, g.num_bases() do
    g.set_base_owner(i, P0)
    g.set_base_stock(i, 90, 90, 90)
  end
  -- The second friendly pill is scenery in most arenas: a live pill inside the
  -- room invites defend/repair goals that have nothing to do with what is
  -- being measured.  hide_pill takes it off the map entirely.
  -- THE NEUTRAL IS A TRADE, and each arena picks its side of it.
  --   ON  : its PILL_RANGE_MAP(9) danger stamp covers every tank-reachable
  --         tile, which is what keeps rescue_lgm suppressed under the pinned
  --         static gate (perc.under_fire = danger_at(tile) > 0) once the man
  --         is stranded.  The same stamp makes threat.pill_at(tank tile) > 0,
  --         which fires haul_flee_eval's pill_shooting trigger for a tank
  --         carrying pills with its builder out -- so take_cover owns the
  --         pool at HAUL_FLOOR and the ESCAPE half of the feature is exactly
  --         what gets measured.  Arenas A, B, E, EK.
  --   OFF : nothing stamps danger, so haul_flee_eval is quiet and the
  --         kill_me_wait row can actually bid.  Arenas C, D1, D2.
  if not NP_ON and idx_neutral then g.hide_pill(idx_neutral) end
  if idx_corpse then
    if CORPSE_ON then
      -- Arena E's body is HIDDEN until the state starts. Put it on the ground
      -- at setup and the bot drives over it in the first hundred ticks, long
      -- before it is loaded and builder-less, and there is nothing left to
      -- price. show_pill puts a hidden pill back DEAD, which is exactly the
      -- state a capture wants, so the corpse appears on the wall tick.
      g.hide_pill(idx_corpse)
    elseif BUSY_P1 then
      g.set_pill_armour(idx_corpse, TOPUP_ARMOUR)   -- arena D1: p1's errand
    else
      g.hide_pill(idx_corpse)
    end
  end
  -- The spares are carry-slots, not pillboxes: off the map until the flood.
  for _, i in ipairs(spares) do g.hide_pill(i) end
  local f = io.open(TRACE, "w")
  if f then f:write("# tick x y armour owner in_tank\\n") f:close() end
end

function on_choose_start(g, p)
  if p == P0 then return 1 end
  if p == P1 then return 2 end
  return nil
end

function on_tick(g, tick)
  -- ALLIANCE, APPLIED FROM on_tick.  botManagerSetTeams walks the bots'
  -- OWN ClientSim alliance tables, and at on_setup time no bot exists yet --
  -- so a set_team there reaches the server's roster and nothing else, and the
  -- two bots still read each other as OBJECT_HOSTILE (measured: arena C's
  -- would-be ally opened with `attack_tank #1`).  Re-applied for the first
  -- ALLY_TICKS ticks so it lands whenever the slots actually come up.
  if tick <= 120 then
    g.set_team(P0, 0)
    g.set_team(P1, P1_ENEMY and 1 or 0)
  end
  -- Fill the spawn ponds once each tank is ASHORE and off its boat. Filling
  -- the tile while the tank still sits on it leaves the boat state stuck and
  -- the LGM never becomes available.
  if tick >= FILL_TICK then
    if not filled0 then
      local tk = g.tank(P0)
      if tk and not tk.boat and not tk.dead
         and (tk.mx ~= SPAWN0[1] or tk.my ~= SPAWN0[2]) then
        filled0 = true
        g.set_tile(SPAWN0[1], SPAWN0[2], FOREST)
        note(string.format("# pondfill0 %d", tick))
      end
    end
    if not filled1 then
      local tk = g.tank(P1)
      if tk and not tk.boat and not tk.dead
         and (tk.mx ~= SPAWN1[1] or tk.my ~= SPAWN1[2]) then
        filled1 = true
        g.set_tile(SPAWN1[1], SPAWN1[2], FOREST)
        note(string.format("# pondfill1 %d", tick))
      end
    end
  end

  if not traced then
    traced = resolve(g) and true or false
    if not traced then return end
  end

  -- THE FLOOD + THE HAND-OUT.  Full armour on a pill that is still ours and
  -- not in a tank means the man is standing on ({PX},{PY}), east of the gate.
  -- Both tanks have to be off the gate column on the same tick.
  -- THE FLOOD: ONE COLUMN, THE FAR ONE.
  --
  -- Fired on the exact tick the engine reports the pill full -- the man is
  -- standing on ({PX},{PY}) at that instant and nowhere else, so this is the
  -- one tick where his side of the gate is known.
  --
  -- The gate is TWO columns wide and only the EAST one (x={GX1}) is walled.
  -- That is the whole trick, and it took three drafts:
  --   * the tank's cheapest reachable tile to the errand pill is always the
  --     tile NEXT to it, so a one-column gate is a column the tank is parked
  --     on at every single completion -- and walling under a tank freezes it
  --     (MAP_SPEED_TBUILDING is 0), so that flood could never fire at all;
  --   * flooding with RIVER instead needs no guard (the tank drives on, the
  --     man cannot enter at all), but it leaves the tank able to sit ONE tile
  --     from the man, and lgm.c lgmReturn boards him from there -- measured,
  --     the man was home 160 ticks after the flood;
  --   * and walling the FAR column does not strand him either: a live pillbox
  --     is MANSPEED 0, so the man never stands ON the errand pill -- he stands
  --     BESIDE it, on the far gate column, and a wall laid under his feet is a
  --     wall he simply steps west out of.
  -- So the NEAR column (x={GX0}) is the one that gets walled, which leaves the
  -- man loose in the four-tile pocket east of it (the far column plus the pill
  -- tile) with no way out and no way for a tank to reach him.
  --
  -- The guard is therefore "neither tank is east of the room", and when it
  -- fails the pill goes back down to TOPUP_ARMOUR so the pool sends the man
  -- out again and the next completion is another attempt.
  -- THE KILL (arenas C/D1/D2). game.kill_lgm puts p0's man through the
  -- engine's own death path (lgm.c lgmDeathCheckAtPosition): he is choppered
  -- in from a start tile and walks home, exactly as a shell would have left
  -- him. Re-applied every tick afterwards so he dies again the moment he
  -- lands: the arena is seven tiles wide and one death would only buy a few
  -- hundred ticks of the state. The first kill flips `walled`, which is the
  -- latch the hand-out below already waits on.
  if KILL_LGM and traced and filled0 and filled1 and tick >= FILL_TICK + 100 then
    local killed = g.kill_lgm(P0)
    if killed and not walled then
      walled = true
      note(string.format("# kill %d", tick))
      g.message(string.format("KILL_ME_{V}: p0's builder killed at t=%d", tick))
    end
  end
  local p = idx_our and g.pill(idx_our)
  if not KILL_LGM and p and not walled and p.armour >= 15 and not p.in_tank and p.owner == P0 then
    local t0 = g.tank(P0)
    local t1 = g.tank(P1)
    local clear0 = (not t0) or t0.dead or t0.mx < GATE_X[1]
    local clear1 = (not t1) or t1.dead or t1.mx < GATE_X[1]
    if clear0 and clear1 then
      walled = true
      for _, gy in ipairs(GATE_Y) do
        g.set_tile(GATE_X[1], gy, WALL)
      end
      note(string.format("# wall %d tank0=(%s,%s)", tick,
                         t0 and tostring(t0.mx) or "?", t0 and tostring(t0.my) or "?"))
      g.message(string.format(
        "KILL_ME_{V}: gate column x=%d walled at t=%d -- p0's man is cut off",
        GATE_X[1], tick))
    else
      retry_at = retry_at or (tick + REDAMAGE_AFTER)
      if tick >= retry_at then
        g.set_pill_armour(idx_our, TOPUP_ARMOUR)
        retry_at = nil
        note(string.format("# retry %d tank0=(%s,%s)", tick,
                           t0 and tostring(t0.mx) or "?", t0 and tostring(t0.my) or "?"))
      end
    end
  end

  -- KEEP THE WALL UP. A BUILDING tile is not permanent: a shell that misses
  -- chews it to HALFBUILDING and then to RUBBLE, and both tanks spend the run
  -- firing in a seven-tile room. Re-stamping the column every tick costs
  -- nothing and stops a stray round from re-opening the pocket halfway
  -- through the measurement (measured: the man was loose again 400 sim ticks
  -- after the wall went up).
  if walled and not KILL_LGM then
    for _, gy in ipairs(GATE_Y) do
      g.set_tile(GATE_X[1], gy, WALL)
    end
  end

  -- The pills go aboard on the flood tick, never before: hand them over early
  -- and the bot places them instead of carrying them, and the errand that
  -- walks the man out never happens at all.
  if walled and not gave then
    gave = true
    local n = 0
    for _, i in ipairs(spares) do
      if n < GIVE_N then
        -- show_pill puts a hidden pill back where it was hidden, DEAD; that
        -- tile is a walled-in road square nothing can reach, so the pill
        -- exists on the map only for the instant between these two calls.
        g.show_pill(i)
        g.set_pill_owner(i, P0)
        if g.give_pill(P0, i) then n = n + 1 else g.hide_pill(i) end
      end
    end
    -- Arena E: the body appears now, with the state, not at setup.
    if CORPSE_ON and idx_corpse then
      g.show_pill(idx_corpse)
      g.set_pill_owner(idx_corpse, P0)
      note(string.format("# corpse %d", tick))
    end
    note(string.format("# gave %d n=%d", tick, n))
    g.message(string.format("KILL_ME_{V}: %d pillbox(es) loaded into p0 at t=%d",
                            n, tick))
  end

  -- Arena D1 only: keep p1's errand alive so its builder keeps walking, which
  -- is what makes the responder row read no_lgm.
  if BUSY_P1 and idx_corpse then
    local c = g.pill(idx_corpse)
    if c and c.armour >= 15 and not c.in_tank then
      g.set_pill_armour(idx_corpse, TOPUP_ARMOUR)
    end
  end

  -- ENGINE-SIDE TRACE of both our pills plus every pill that ends up on the
  -- ground: arena C proves the hand-off by watching corpses appear with p0's
  -- ownership and then go in_tank under p1.
  -- p0's tank alive/dead edge: arena C proves the hand-off landed by seeing
  -- p0 die while the request was live.
  local t0d = g.tank(P0)
  if t0d and (t0d.dead and true or false) ~= last_dead0 then
    last_dead0 = t0d.dead and true or false
    note(string.format("# p0dead %d %s", tick, tostring(last_dead0)))
  end
  for i = 1, g.num_pills() do
    if i ~= idx_neutral then
      local q = g.pill(i)
      if q then
        local in_tank = q.in_tank and 1 or 0
        local sig = q.armour .. "/" .. q.owner .. "/" .. in_tank .. "/" .. q.x .. "," .. q.y
        if last[i] ~= sig then
          note(string.format("%d %d %d %d %d %d", tick, q.x, q.y,
                             q.armour, q.owner, in_tank))
          last[i] = sig
        end
      end
    end
  end
end
'''

ARENA = {
    #        p1 enemy, corpse down, p1's builder busy, neutral on, kill p0's man
    # kill = game.kill_lgm (the engine's own death path) instead of the wall
    # stranding: a DEAD man is what the state means in a real game, sets no
    # stranded flag and needs no rescue suppression -- so the hand-off arenas
    # (C/D1/D2) can run end to end.
    "A":  ("true",  "false", "false", "true",  "false"),
    "B":  ("true",  "false", "false", "true",  "false"),
    "C":  ("false", "false", "false", "false", "true"),
    "D1": ("false", "false", "true",  "false", "true"),
    "D2": ("false", "false", "false", "false", "true"),
    "E":  ("false", "true",  "false", "true",  "false"),
    "EK": ("false", "true",  "false", "true",  "false"),
}


def write(variant, here):
    global CORPSE
    p1_enemy, corpse_on, busy_p1, np_on, kill_lgm = ARENA[variant]
    np_live = (np_on == "true")
    CORPSE = CORPSE_NEAR if variant in ("C", "D1", "D2") else CORPSE_FAR
    terrain, pills, bases, starts = build(np_live)
    check(terrain, pills, bases, starts, np_live)

    out = here / f"kill_me_{variant}.map"
    with open(out, 'wb') as f:
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

    side = SIDECAR.format(
        V=variant, P1_ENEMY=p1_enemy, CORPSE_ON=corpse_on, BUSY_P1=busy_p1,
        NP_ON=np_on, KILL_LGM=kill_lgm,
        PX=OURPILL[0], PY=OURPILL[1], CX=CORPSE[0], CY=CORPSE[1],
        NX=NP[0], NY=NP[1],
        S0X=SPAWN0[0], S0Y=SPAWN0[1], S1X=SPAWN1[0], S1Y=SPAWN1[1],
        GX0=128, GX1=129, GY0=126, GY1=127, GY2=128,
        TOPUP=TOPUP_ARMOUR)
    sideout = here / f"kill_me_{variant}.scenario.lua"
    with open(sideout, 'w', encoding='utf-8', newline='\n') as f:
        f.write(side)

    print(f"Wrote {out} ({out.stat().st_size} bytes) and {sideout.name} "
          f"[arena {variant}: {len(pills)} pill(s), {len(bases)} base(s), "
          f"{len(starts)} start(s)]")


def main():
    args = sys.argv[1:]
    variants = [args[0]] if args and args[0] in VARIANTS else list(VARIANTS)
    here = Path(__file__).resolve().parent
    for v in variants:
        write(v, here)


if __name__ == '__main__':
    main()
