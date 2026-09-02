#!/usr/bin/env python3
"""
Generate the spawn-escape arenas (companion to tests/spawn_escape_test.py).

Field incident 20260902_120856 blocks 3-4: every respawn puts the tank on a
BOAT at a fixed start square, because starts.c startsIsValidSquare requires a
start to be DEEP SEA.  Once the enemy owned a pillbox covering that square the
boat was shot out from under the tank -- one shell is enough (tank.c: a hit on
a tank riding a boat clears onBoat unconditionally) -- and the tank drowned the
very next tick, cause 1 LAST_DEATH_BY_DEEPSEA with itself as the killer.

goals.spawn_escape_tick is the answer: init.lua sets state._spawn_escape_arm on
a fresh tank, and for the next SPAWN_ESCAPE_WINDOW_TICKS (250) the escape asks
whether the tile the tank woke up on is under pill coverage or has shells
inbound.  If it is, it picks the nearest tile OUT of all coverage that is
passable and whose straight ray is clear -- biased toward the nearest friendly
base with stock -- and DENIES goal selection until the tank is clear, ashore,
arrived, or SPAWN_ESCAPE_MAX_TICKS (600) have passed.

TWO VARIANTS, ONE LANDSCAPE

    Both variants write exactly the same two landmasses, between x 112..140 and
    y 110..142, symmetric about (126,126) so mapRead's recenter is a no-op and
    in-game tiles match this file.

      x 124..128, y 110..120   NORTH PENINSULA (grass)
      (126,120)                the NEUTRAL PILLBOX on its south tip
      (126,127)                THE ONLY START -- deep sea, seven tiles south of
                               the pillbox: inside both its 8-tile fire range
                               and its 9-tile coverage disk
      x 112..140, y 136..142   SOUTH CONTINENT (grass)
      (126,141)                the bot's own BASE, full stock, 21 tiles from
                               the pillbox and 14 from the start

    A  FAR DEATH.  Nothing else on the map: open water from the start all the
       way down to the continent.  The sidecar kills the tank ONCE, thirteen
       tiles from the start, by turning the tile under its tracks into deep sea
       for a few ticks and then putting the ground back.  The respawn is a
       13-tile jump, so init.lua's RESPAWN_DETECTED fires and the escape arms
       via=respawn_jump.  This variant tests the escape itself: pick, drive,
       release, refuel.

    B  SAME-SQUARE DEATH -- the regression that made the whole feature inert.
       The start is sealed into a one-tile-wide water channel (x=126,
       y 121..127) whose walls are BUILDING tiles at (125,121..128),
       (127,121..128) and (126,128), and whose only opening faces the pillbox
       seven tiles up an unobstructed line of water.  The tank cannot leave, on
       0 shells cannot shoot the walls down, and the pillbox sinks its boat
       where it floats.  It respawns ONE TILE from where it died.

       That jump of 1 is the point.  init.lua's respawn detector only fires on
       a jump of more than RESPAWN_CACHE_WIPE_DIST (12) tiles, so for a while
       this case armed nothing at all: the exact incident the escape was
       written for was the one shape its arming condition could not see.  The
       arming now also accepts info.newtank -- the engine's own one-tick
       fresh-tank flag, which has no distance condition -- and variant B exists
       to keep it that way.  Its sidecar opens the channel's south wall on the
       first death (TANK_DEATH_WAIT is 255 frames, so the way out is ready well
       before the tank is), which is what gives life 2 somewhere to escape TO.

WHY THE PILLBOX FIRES SLOWLY, AND WHY THAT IS NOT A FUDGE

    Pill speed 100 = PILLBOX_ATTACK_NORMAL, the slowest reload the engine
    allows (pillbox.c clamps speed into [PILLBOX_MAX_FIRERATE,
    PILLBOX_ATTACK_NORMAL]) -- and also what an undamaged pill on a real map
    runs at, since a pill only speeds up when it is shot at and a 0-shell tank
    never shoots it.  On top of that, pillsUpdate's `justSeen` handshake makes
    a pill that has just re-acquired a target reset its reload and skip one
    shot, so every respawn buys the tank one reload period of grace.  That
    grace is what the escape has to spend well; the arena grants it no slack it
    would not have in a real game.

GEOMETRY THE BRAIN SIDE DEPENDS ON

    threat.coverage_at counts a pill over the PILL_RANGE_MAP disk (radius 9,
    Euclidean), one tile WIDER than the 8 a pill can actually shoot
    (PILLBOX_RANGE 2048 = 8 tiles).  So:
      spawn (126,127) is 7 from the pill: covered AND shootable;
      (126,129)       is 9:               covered, NOT shootable;
      (126,130)       is 10:              the first tile that is not covered.
    The escape has to travel 3 tiles to clear coverage but only 2 to stop being
    shot at -- so it is out of the line of fire before it is even done -- and
    the cheapest uncovered tile the scan can see is exactly (126,130):
    3 x SPAWN_ESCAPE_TRAVEL_W (3.0) + 11 x SPAWN_ESCAPE_BASE_BIAS (1.5) = 25.5,
    against 34.5 for the nearest shore tile.  EXPECT_PICK below is that tile;
    the test reports it but does not require it -- which tile wins is the
    scan's business.

WHY THE RUN IS TOURNAMENT + -ranked

    TOURNAMENT with zero neutral bases hands every tank 0 shells / 0 mines /
    0 trees and full armour (gametype.c).  Zero shells matters three times: the
    tank cannot shoot variant B's channel walls down, it cannot anger the
    pillbox into a faster reload, and -- the point of variant A's last
    assertion -- it has a real reason to go and refuel once the escape lets go.
    -ranked is required because serverSimApplyScenarioCommit stamps gameScripted
    over the game type of any map with a scenario sidecar, and gameScripted gets
    the full open loadout.  (Same reason tests/respawn_loadout_test.py passes
    it.)

Usage:
    python3 tests/generate_spawn_escape_map.py [A|B] [output_path]
    Default: variant A -> tests/spawn_escape_A.map
Writes <output>.map and the matching <output>.scenario.lua sidecar.
"""

import struct
import sys
from pathlib import Path

BUILDING = 0
GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)
MAP_SIZE = 256
NEUTRAL = 0xFF
DEEP_SEA_TILE = 0xFF   # what scenario.set_tile wants for open sea

# -- Geometry (the test runner imports these for its assertions) -------------
PENINSULA = (124, 128, 110, 120)   # x0, x1, y0, y1 inclusive
PILL = (126, 120)                  # neutral, on the peninsula's south tip
SPAWN = (126, 127)                 # the only start; deep sea, 7 from the pill
CONTINENT = (112, 140, 136, 142)   # x0, x1, y0, y1 inclusive
BASE = (126, 141)                  # ours, full stock, 14 from the spawn
EXPECT_PICK = (126, 130)           # reported, never asserted
BOT_SLOT = 0
STOCK_PERIOD = 150                 # sidecar re-assert cadence, in server ticks
KILL_DIST = 13                     # variant A: scripted drowning this far out

# -- Variant B only: the channel that seals the start square -----------------
CHANNEL_X = 126
CHANNEL_Y = (121, 127)             # deep sea; the start is its south end
WALL_X = (125, 127)                # the channel's two side walls
WALL_Y = (121, 128)                # ...and how far south they run
PLUG = (126, 128)                  # the wall that seals the channel's south end
GATE = [(125, 128), (126, 128), (127, 128)]   # opened by the sidecar on death 1

# -- Brain/engine constants this arena is designed against -------------------
PILL_RANGE_MAP = 9           # constants.lua -- the coverage disk (Euclidean),
                             # deliberately 1 beyond the 8-tile fire range
PILL_FIRE_RANGE = 8          # constants.lua == PILLBOX_RANGE 2048 / 256
PILLBOX_ATTACK_NORMAL = 100  # pillbox.h -- the SLOWEST legal reload
RESPAWN_CACHE_WIPE_DIST = 12      # constants.lua -- the respawn-detect gate
SPAWN_ESCAPE_SEARCH_RADIUS = 14   # constants.lua
SPAWN_ESCAPE_TRAVEL_W = 3.0       # constants.lua
SPAWN_ESCAPE_BASE_BIAS = 1.5      # constants.lua
SPAWN_ESCAPE_WINDOW_TICKS = 250   # constants.lua
SPAWN_ESCAPE_MAX_TICKS = 600      # constants.lua
TANK_DEATH_WAIT = 255             # tank.h

VARIANTS = ("A", "B")


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def covered(tile, pill=PILL):
    """threat.coverage_at > 0: inside the pill's PILL_RANGE_MAP disk."""
    return edist(tile, pill) <= PILL_RANGE_MAP


def shootable(tile, pill=PILL):
    """pillsUpdate would take a shot at a tank standing here."""
    return edist(tile, pill) <= PILL_FIRE_RANGE


def escape_cost(tile, frm=SPAWN, base=BASE):
    """goals.lua se_scan's cost for one candidate tile."""
    return (mdist(frm, tile) * SPAWN_ESCAPE_TRAVEL_W
            + mdist(tile, base) * SPAWN_ESCAPE_BASE_BIAS)


def channel_tiles():
    return [(CHANNEL_X, y) for y in range(CHANNEL_Y[0], CHANNEL_Y[1] + 1)]


def make_map(variant="A"):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    for (x0, x1, y0, y1) in (PENINSULA, CONTINENT):
        for yy in range(y0, y1 + 1):
            for xx in range(x0, x1 + 1):
                t[yy][xx] = GRASS
    if variant == "B":
        for yy in range(WALL_Y[0], WALL_Y[1] + 1):
            t[yy][WALL_X[0]] = BUILDING
            t[yy][WALL_X[1]] = BUILDING
        t[PLUG[1]][PLUG[0]] = BUILDING
        # The channel itself is background deep sea; nothing to write.
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


# --------------------------------------------------------------------------
# Sidecars.  Both keep the base's stock reading fresh; they differ only in how
# the tank's first death is arranged.
# --------------------------------------------------------------------------

SIDECAR_HEAD = '''\
-- Scenario sidecar for tests/spawn_escape_{V}.map -- GENERATED by
-- tests/generate_spawn_escape_map.py, do not edit by hand.
--
{JOB1}
-- 2. KEEP THE BASE'S STOCK READING FRESH.  A bot learns base stock from
--    EVENT_BASE_STOCK, which the server emits ONLY when a base's
--    armour/shells/mines actually CHANGE.  An untouched base goes quiet and
--    the reading ages out past REFUEL_OBS_STALE; then se_nearest_stocked_base
--    can no longer tell this base is worth biasing the escape toward, and the
--    post-escape refuel has nothing fresh to aim at.  So every {PERIOD} ticks
--    this re-asserts the stock with a mines value that alternates 90/89 -- a
--    real change, so a real event.

local SPAWN     = {{ {SPX}, {SPY} }}
local BASE      = {{ {BX}, {BY} }}
local OWNER     = {SLOT}
local PERIOD    = {PERIOD}
local DEEP      = {DEEPTILE}
local GRASS     = 7

local flip = 0
local deaths = 0
local was_dead = false

local function assert_stock(g)
  flip = 1 - flip
  for i = 1, g.num_bases() do
    g.set_base_stock(i, 90, 90, 90 - flip)
  end
end

function on_setup(g)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      g.set_base_owner(i, OWNER)
      g.set_base_stock(i, 90, 90, 90)
      g.message(string.format(
        "SPAWN_ESCAPE_ARENA base#%d (%d,%d) owner=%d armour=90 shells=90",
        i, b.x, b.y, OWNER))
    end
  end
end

-- One death report per dead/alive edge, with the distance the respawn will
-- have to jump -- the number the whole A/B split is about.
local function note_deaths(g, tick, t)
  if t.dead and not was_dead then
    deaths = deaths + 1
    g.message(string.format(
      "SPAWN_ESCAPE_ARENA tank death #%d at tick %d pos=(%d,%d) dist_from_spawn=%d",
      deaths, tick, t.mx, t.my,
      math.abs(t.mx - SPAWN[1]) + math.abs(t.my - SPAWN[2])))
  end
  was_dead = t.dead and true or false
end
'''

JOB1_A = '''\
-- Two jobs.
--
-- 1. KILL THE TANK ONCE, FAR FROM ITS START.  The escape cannot fire on a
--    tank's FIRST life -- there is nothing to respawn from -- so the tank has
--    to die once before the thing under test can happen.  Variant A wants that
--    death a LONG way from the start, so the respawn is a big position jump
--    and init.lua's RESPAWN_DETECTED path is the one that arms the escape.
--    (Variant B covers the opposite shape: dying ON the start square, where
--    the jump is 1 and only info.newtank can arm it.)
--
--    So the FIRST time the tank is standing on dry land at least {KILLD} tiles
--    from the start -- which on this map means walking up to its base -- this
--    turns the tile under its tracks into deep sea.  tank.c drowns any tank
--    that is not on a boat over DEEP_SEA, so it dies on the spot; the ground
--    is put straight back a few ticks later, leaving the map exactly as it
--    was.  One death, at a known place, at a known moment, and no second
--    hazard anywhere near the escape route.
--
--    It never fires twice: every later death is the bot's own doing, which is
--    what the test is measuring.
--'''

JOB1_B = '''\
-- Two jobs.
--
-- 1. OPEN THE CHANNEL ONCE THE TANK HAS DIED IN IT.  The map seals the start
--    square into a one-tile-wide water pocket whose only opening faces the
--    pillbox seven tiles away, with nothing but water in between.  The tank
--    cannot leave it and, on 0 shells, cannot shoot the walls down: the pill
--    sinks its boat where it floats and deep sea does the rest.  It respawns
--    ONE TILE from where it died, which is the whole point of this variant --
--    a jump that small is invisible to init.lua's RESPAWN_DETECTED (which
--    needs more than RESPAWN_CACHE_WIPE_DIST = {WIPE} tiles), so only
--    info.newtank can arm the escape, and for a while nothing did.
--
--    The instant the tank reads dead, this turns the pocket's south wall
--    ({GATEDESC}) into deep sea.  TANK_DEATH_WAIT is {DEATHWAIT} frames, so
--    the way out is open long before the tank comes back -- and life 2, on the
--    same start square under the same coverage, is the life the test is about.
--
--    Only the FIRST death opens it; after that the map stops changing, so a
--    later death (which the test treats as a failure) cannot be papered over.
--'''

SIDECAR_TAIL_A = '''
local scripted_kill_done = false
local restore = nil

function on_tick(g, tick)
  if tick > 0 and tick % PERIOD == 0 then assert_stock(g) end

  -- Put the ground back a few ticks after the scripted drowning.
  if restore and tick >= restore[3] + 5 then
    g.set_tile(restore[1], restore[2], GRASS)
    g.message(string.format("SPAWN_ESCAPE_ARENA restored land at (%d,%d) tick %d",
                            restore[1], restore[2], tick))
    restore = nil
  end

  local t = g.tank(0)
  if not t then return end
  note_deaths(g, tick, t)

  if not scripted_kill_done and not t.dead and not t.boat then
    local d = math.abs(t.mx - SPAWN[1]) + math.abs(t.my - SPAWN[2])
    -- Never dig out the base's own tile: mapRead forces ROAD under a base and
    -- putting sea there, even briefly, is a change this arena does not want.
    if d >= KILL_DIST and not (t.mx == BASE[1] and t.my == BASE[2]) then
      scripted_kill_done = true
      restore = { t.mx, t.my, tick }
      g.set_tile(t.mx, t.my, DEEP)
      g.message(string.format(
        "SPAWN_ESCAPE_ARENA scripted drowning at (%d,%d) tick %d (%d tiles from spawn)",
        t.mx, t.my, tick, d))
    end
  end
end
'''

SIDECAR_TAIL_B = '''
local gate_open = false

function on_tick(g, tick)
  if tick > 0 and tick % PERIOD == 0 then assert_stock(g) end

  local t = g.tank(0)
  if not t then return end
  note_deaths(g, tick, t)

  if t.dead and not gate_open then
    gate_open = true
    for _, q in ipairs(GATE) do g.set_tile(q[1], q[2], DEEP) end
    g.message("SPAWN_ESCAPE_ARENA channel opened at tick " .. tostring(tick))
  end
end
'''


def write_sidecar(path, variant):
    head = SIDECAR_HEAD.format(
        V=variant,
        JOB1=(JOB1_A.format(KILLD=KILL_DIST) if variant == "A"
              else JOB1_B.format(WIPE=RESPAWN_CACHE_WIPE_DIST,
                                 DEATHWAIT=TANK_DEATH_WAIT,
                                 GATEDESC=", ".join("(%d,%d)" % q
                                                    for q in GATE))),
        SPX=SPAWN[0], SPY=SPAWN[1], BX=BASE[0], BY=BASE[1], SLOT=BOT_SLOT,
        PERIOD=STOCK_PERIOD, DEEPTILE=DEEP_SEA_TILE)
    if variant == "A":
        extra = "local KILL_DIST = %d\n" % KILL_DIST
        tail = SIDECAR_TAIL_A
    else:
        extra = "local GATE = { %s }\n" % ", ".join("{ %d, %d }" % q
                                                    for q in GATE)
        tail = SIDECAR_TAIL_B
    Path(path).write_text(head + extra + tail, encoding="utf-8", newline="\n")


def main():
    args = list(sys.argv[1:])
    variant = "A"
    if args and args[0].upper() in VARIANTS:
        variant = args.pop(0).upper()
    output = args[0] if args else str(
        Path(__file__).parent / f"spawn_escape_{variant}.map")
    terrain = make_map(variant)

    # -- Recenter sanity: only written tiles define the bounding box, and its
    #    integer midpoint must already be (126,126) or every coordinate shifts.
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))

    # -- The start must be deep sea, and under the pill's gun.
    assert terrain[SPAWN[1]][SPAWN[0]] is DEEP_SEA, (
        "the start square must be deep sea (starts.c startsIsValidSquare)")
    assert covered(SPAWN) and shootable(SPAWN), (
        f"the pill is {edist(PILL, SPAWN):.1f} tiles from the start; it has to "
        f"be inside both the {PILL_FIRE_RANGE}-tile fire range and the "
        f"{PILL_RANGE_MAP}-tile coverage disk or the escape has no reason to "
        "arm")
    assert terrain[PILL[1]][PILL[0]] is not DEEP_SEA, "the pill needs land"

    # -- There is somewhere to escape TO (variant B: once the gate is open),
    #    it is the cheapest thing the scan can see, and it is in the search box.
    assert not covered(EXPECT_PICK) and covered((EXPECT_PICK[0],
                                                 EXPECT_PICK[1] - 1)), (
        f"{EXPECT_PICK} should be the FIRST uncovered tile down the column")
    assert mdist(SPAWN, EXPECT_PICK) <= SPAWN_ESCAPE_SEARCH_RADIUS
    best, bcost = None, 1e9
    for y in range(SPAWN[1] - SPAWN_ESCAPE_SEARCH_RADIUS,
                   SPAWN[1] + SPAWN_ESCAPE_SEARCH_RADIUS + 1):
        for x in range(SPAWN[0] - SPAWN_ESCAPE_SEARCH_RADIUS,
                       SPAWN[0] + SPAWN_ESCAPE_SEARCH_RADIUS + 1):
            if covered((x, y)) or terrain[y][x] == BUILDING:
                continue
            c = escape_cost((x, y))
            if c < bcost:
                best, bcost = (x, y), c
    assert best == EXPECT_PICK, (
        f"the scan's cheapest uncovered tile models as {best} (cost "
        f"{bcost:.0f}), not {EXPECT_PICK}")

    # -- The base is out of the pill's reach and close enough for the escape's
    #    base bias to see it.
    assert not covered(BASE) and not shootable(BASE), (
        f"the base is {edist(PILL, BASE):.1f} tiles from the pill; it has to "
        f"be outside both the {PILL_FIRE_RANGE}-tile fire range and the "
        f"{PILL_RANGE_MAP}-tile coverage disk")
    assert mdist(SPAWN, BASE) <= SPAWN_ESCAPE_SEARCH_RADIUS, (
        f"the base is {mdist(SPAWN, BASE)} tiles from the spawn, outside "
        f"SPAWN_ESCAPE_SEARCH_RADIUS ({SPAWN_ESCAPE_SEARCH_RADIUS})")
    assert terrain[BASE[1]][BASE[0]] is not DEEP_SEA

    if variant == "A":
        # The scripted drowning has to clear the respawn detector's jump gate,
        # and there has to be a land tile that far out for it to happen on.
        assert KILL_DIST > RESPAWN_CACHE_WIPE_DIST, (
            f"the scripted drowning at {KILL_DIST} tiles would not clear "
            f"RESPAWN_CACHE_WIPE_DIST ({RESPAWN_CACHE_WIPE_DIST}), so variant "
            "A would arm via info.newtank and stop being a distinct case")
        kill_tiles = [(x, y)
                      for y in range(CONTINENT[2], CONTINENT[3] + 1)
                      for x in range(CONTINENT[0], CONTINENT[1] + 1)
                      if mdist(SPAWN, (x, y)) >= KILL_DIST and (x, y) != BASE]
        assert (BASE[0], BASE[1] - 1) in kill_tiles, (
            "the tile the tank walks through on its last step to the base is "
            f"not {KILL_DIST} tiles out; the scripted drowning may never fire")
        # ...and there is genuinely nothing in the way, so life 1 sails out.
        for y in range(SPAWN[1], CONTINENT[2]):
            assert terrain[y][SPAWN[0]] is DEEP_SEA, (SPAWN[0], y)
    else:
        # -- Variant B: the start is SEALED.  Every neighbour except due north
        #    is a wall, so the tank cannot leave the channel and the death it
        #    dies is on (or one tile from) the square it will respawn on.
        for dx, dy in ((-1, 0), (1, 0), (0, 1),
                       (-1, 1), (1, 1), (-1, -1), (1, -1)):
            n = (SPAWN[0] + dx, SPAWN[1] + dy)
            assert terrain[n[1]][n[0]] == BUILDING, (
                f"{n} is not a wall -- the tank could leave the pocket and the "
                "death would stop being a same-square one")
        assert terrain[SPAWN[1] - 1][SPAWN[0]] is DEEP_SEA, (
            "the channel has to be open to the north or the pill cannot shoot "
            "down it")
        # ...the whole channel is open water with a clear shot down it...
        for (cx, cy) in channel_tiles():
            assert terrain[cy][cx] is DEEP_SEA, (cx, cy)
        assert PILL[0] == CHANNEL_X and PILL[1] == CHANNEL_Y[0] - 1, (
            "the pillbox must sit at the head of the channel, in line with it")
        # ...every tile of it is inside the pill's reach, so there is nowhere
        #    in the pocket the tank can wait the test out...
        for c in channel_tiles():
            assert shootable(c), (
                f"{c} is out of the pill's reach -- the tank could sit there "
                "and never die")
        # ...and any death inside it respawns the tank WITHIN the jump gate,
        #    which is exactly the case this variant exists to cover.
        for c in channel_tiles():
            assert mdist(SPAWN, c) <= RESPAWN_CACHE_WIPE_DIST, (
                f"a death at {c} would be a {mdist(SPAWN, c)}-tile jump, over "
                f"RESPAWN_CACHE_WIPE_DIST ({RESPAWN_CACHE_WIPE_DIST}) -- "
                "RESPAWN_DETECTED would fire and B would stop testing the "
                "info.newtank path")
        # ...and the gate the sidecar opens really is a wall to begin with.
        for q in GATE:
            assert terrain[q[1]][q[0]] == BUILDING, (
                f"gate tile {q} is not a wall, so opening it changes nothing")

    # Pill record: x, y, owner, armour, speed.  NEUTRAL shoots every tank in
    # range and needs no second player; armour 15 = PILLS_MAX_HEALTH (alive);
    # speed 100 = PILLBOX_ATTACK_NORMAL, the slowest reload the engine allows.
    pills = [(PILL[0], PILL[1], NEUTRAL, 15, PILLBOX_ATTACK_NORMAL)]
    # Base record: x, y, owner, armour, shells, mines -- ours, full.
    bases = [(BASE[0], BASE[1], BOT_SLOT, 90, 90, 90)]
    # The ONLY start, so every respawn comes back to the same covered square.
    starts = [(SPAWN[0], SPAWN[1], 8)]            # dir 8 = south

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
    print(f"  variant {variant}: start {SPAWN}"
          + (" in open sea" if variant == "A"
             else f" sealed in the x={CHANNEL_X} channel "
                  f"(y {CHANNEL_Y[0]}..{CHANNEL_Y[1]})")
          + f"; neutral pill {PILL} {edist(PILL, SPAWN):.0f} tiles north "
            f"(fire range {PILL_FIRE_RANGE}, coverage disk {PILL_RANGE_MAP}, "
            f"reload {PILLBOX_ATTACK_NORMAL})")
    if variant == "A":
        print(f"  sidecar drowns the tank ONCE on the first land tile "
              f">= {KILL_DIST} tiles out, so the respawn jump clears "
              f"RESPAWN_CACHE_WIPE_DIST ({RESPAWN_CACHE_WIPE_DIST}) and the "
              "escape arms via=respawn_jump")
    else:
        worst = max(mdist(SPAWN, c) for c in channel_tiles())
        print(f"  the pill sinks the boat in the channel; the respawn jump is "
              f"at most {worst}, well inside RESPAWN_CACHE_WIPE_DIST "
              f"({RESPAWN_CACHE_WIPE_DIST}), so only info.newtank can arm the "
              f"escape; sidecar opens {GATE} on the first death")
    print(f"  expected escape tile {EXPECT_PICK} at cost "
          f"{escape_cost(EXPECT_PICK):.0f} ({mdist(SPAWN, EXPECT_PICK)} x "
          f"{SPAWN_ESCAPE_TRAVEL_W} + {mdist(EXPECT_PICK, BASE)} x "
          f"{SPAWN_ESCAPE_BASE_BIAS})")
    print(f"  base {BASE} full stock, {edist(PILL, BASE):.0f} tiles from the "
          f"pill and {mdist(SPAWN, BASE)} from the spawn")


if __name__ == '__main__':
    main()
