#!/usr/bin/env python3
"""
Generate the sea-pill-harvest arenas (companion to tests/sea_pills_test.py).

Field incident 20260901_000042_1_loss_b6 bot2 t=22735: three DEAD pills sat in
the first two deep-sea columns off the east shore of DH-Oil Rig for the last
5000 ticks of the game.  Every 50 ticks the pool re-evaluated them and printed
`CAPTURE_CAND ... hp=0 ... reject=deepsea_no_boat`.  Nothing in the brain knew
how to get afloat, and the shore had no river anywhere near.

capture_pill's deep-sea branch now prices an entrance instead:
  1. an existing BOAT in the pills' own water body        -> drive on;
  2. an existing RIVER in it                              -> build the boat
     straight into it (BUILDMODE_BUILD on RIVER = a boat, 20 trees);
  3. otherwise mine a shore tile that is CARDINALLY next to that water, shoot
     the mine, let the crater flood to river, then build (21 trees + 1 mine).

Arena (all variants): a land peninsula with a forest strip and a grass shore
column, NO river anywhere (except variant G), three dead pills of the bot's own
team 1-2 tiles off the shore in deep sea, and an ISOLATED two-tile pond inside
the peninsula that is NOT connected to the sea — a boat launched there could
never reach the pills, so the connectivity gate must refuse it.

Variants
  A  full loadout (open: 40 shells / 40 mines / 40 trees), no hostiles.
     Straight execution: mine -> crater -> river -> boat -> sail -> 3 pills.
  B  a hostile (NEUTRAL) pill in deep sea EAST of the cluster with a clear
     shell line onto it.  Expect REJECT pills_covered_by_pill#N, no mine.
  C  the same pill, with a BUILDING breakwater between it and the cluster so
     no line reaches.  Executes like A.
  D  tournament loadout (0 mines, 0 trees) + a friendly base holding mines.
     The refuel_mines leg runs first, then the harvest.
  E  same arena as D; the assertions are about the TREE ordering — the mine
     must never go down before 21 trees (LGM_COST_BOAT 20 + LGM_COST_MINE 1)
     and 1 mine are both in hand.
  F  tournament loadout, friendly base with mines, and NO harvestable forest
     within SEA_TREES_RADIUS of the entrance or the tank.  Expect
     REJECT no_trees_in_territory, no mine, no LGM trip.
  G  tournament loadout with NO mines anywhere (the base stocks none) and an
     existing RIVER tile in the shore column.  The entrance ladder must pick
     the river, skip the mine entirely, farm 20 trees and build the boat
     straight into it.

IMPORTANT: mapRead RECENTERS off-center maps (bolo_map.c mapCenter) — but only
when BOTH axes need shifting (`if (addX != 0 && addY != 0)`).  Every variant
here is laid out so the terrain bounding box's integer x-midpoint is exactly
127, which makes addX == 0 and the recenter a no-op, so file coordinates equal
in-game coordinates.  main() replicates mapCenter (including its quirk that
pills/bases/starts can only LOWER bestLeft/bestTop, never raise bestRight/
bestBottom) and asserts it.

Usage:
    python tests/generate_sea_pills_map.py --variant A [output_path]
"""

import struct
import sys
from pathlib import Path

BUILDING = 0
RIVER = 1
CRATER = 3
ROAD = 4
FOREST = 5
GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)

MAP_SIZE = 256
NEUTRAL = 0xFF

# ── Geometry shared by every variant (the test runner imports these) ─────
FIELD_Y0, FIELD_Y1 = 118, 134          # peninsula rows
FIELD_X1 = 136                         # shore column (grass), sea from x=137
FOREST_X = (132, 134)                  # forest strip, x0..x1 inclusive
FOREST_Y = (121, 131)
ROAD_Y = 126                           # drivable spine
SEA_PILLS = [(138, 124), (139, 126), (138, 128)]   # dead, ours, in deep sea
ISOLATED_POND = [(131, 133), (132, 133)]           # land-locked, NOT the sea
# Everything is packed tight so ONE mine->crater->river->boat cycle plus three
# pickups fits inside a single game minute (3000 brain ticks / 6000 sim ticks):
# the spawn pocket is 2 tiles from the entrance and 2 from the firing spot, our
# base is 4 tiles away, and the forest the boat's 20 trees come from is the
# firing spot itself.
BASE_OURS = (132, 130)                 # ONE tile from the spawn pocket: the
                                       # refuel_mines leg is about proving the
                                       # leg runs, not about a cross-map drive,
                                       # and a base only feeds a tank standing
                                       # on it
BASE_NEUTRAL = (130, 130)              # only so tournament loadouts get shells;
                                       # close enough to capture in passing
SPAWN = (133, 130)                     # one-tile deep-sea pocket (Bolo starts
                                       # must be water; the spawn boat beaches
                                       # on the first move ashore). Deliberately
                                       # SOUTH of the working area: a pocket of
                                       # deep sea beside the firing spot is a
                                       # drowning trap — sea_pills_G slid into
                                       # one while creeping onto its parking
                                       # tile and respawned with an empty tank,
                                       # losing the boat's 20 trees.
HOSTILE_SEA_PILL = (147, 126)          # variants B and C
BREAKWATER_X = 143                     # variant C: building column in the sea
BREAKWATER_Y = (120, 132)
RIVER_TILE = (136, 127)                # variant G: existing river in the shore
FAR_FOREST = (119, 121, 119, 121)      # variant F: forest out of reach

# Brain-side constants this arena is designed against (constants.lua).
SEA_TREES_RADIUS = 12
SEA_PILL_TREES_TOTAL = 21              # LGM_COST_BOAT 20 + LGM_COST_MINE 1
SEA_BOAT_TREES = 20
SEA_TREES_PER_FOREST = 4               # LGM_GATHER_TREE

VARIANTS = ("A", "B", "C", "D", "E", "F", "G")


def field_x0(variant):
    """Left edge chosen so (bestLeft + bestRight) // 2 == 127 -> addX == 0."""
    x1 = BREAKWATER_X if variant == "C" else FIELD_X1
    return 254 - x1


def make_map(variant):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0 = field_x0(variant)
    for y in range(FIELD_Y0, FIELD_Y1 + 1):
        for x in range(x0, FIELD_X1 + 1):
            t[y][x] = GRASS
    # Drivable spine so the tank is never stuck crawling through grass.
    for x in range(x0, FIELD_X1):
        t[ROAD_Y][x] = ROAD
    # Forest strip: the wood supply, and cover for the firing spot.
    if variant != "F":
        for y in range(FOREST_Y[0], FOREST_Y[1] + 1):
            for x in range(FOREST_X[0], FOREST_X[1] + 1):
                t[y][x] = FOREST
    else:
        # Variant F: the only forest on the map sits in the far NW corner,
        # more than SEA_TREES_RADIUS from both the entrance and the tank, so
        # the harvest is not fundable and the plan must say so.
        fx0, fx1, fy0, fy1 = FAR_FOREST
        for y in range(fy0, fy1 + 1):
            for x in range(fx0, fx1 + 1):
                t[y][x] = FOREST
    # The spawn pocket and the land-locked pond are simply unwritten.
    sp = spawn_tile(variant)
    t[sp[1]][sp[0]] = DEEP_SEA
    for (x, y) in ISOLATED_POND:
        t[y][x] = DEEP_SEA
    if variant == "C":
        for y in range(BREAKWATER_Y[0], BREAKWATER_Y[1] + 1):
            t[y][BREAKWATER_X] = BUILDING
    if variant == "G":
        t[RIVER_TILE[1]][RIVER_TILE[0]] = RIVER
    return t


def spawn_tile(variant):
    # Variant F moves the tank to the east end so the far-NW forest is out of
    # SEA_TREES_RADIUS of the TANK as well as of the entrance.
    # Variant F keeps the same pocket: its only forest is in the far NW
    # corner, out of SEA_TREES_RADIUS of both the entrance and the tank.
    return SPAWN


def pills_for(variant):
    """(x, y, owner, armour, speed). owner 0 = the bot's player, armour 0 = dead."""
    pills = [(x, y, 0, 0, 50) for (x, y) in SEA_PILLS]
    if variant in ("B", "C"):
        pills.append((HOSTILE_SEA_PILL[0], HOSTILE_SEA_PILL[1], NEUTRAL, 15, 50))
    return pills


def bases_for(variant):
    """(x, y, owner, armour, shells, mines)."""
    if variant == "G":
        # No mines ANYWHERE: if the plan wrongly needed a crater it would have
        # to reject no_mines_anywhere instead of using the river.
        out = [(BASE_OURS[0], BASE_OURS[1], 0, 90, 90, 0)]
    else:
        out = [(BASE_OURS[0], BASE_OURS[1], 0, 90, 90, 90)]
    if variant in ("D", "E", "F", "G"):
        # tournament shells are a fraction of the NEUTRAL bases on the map;
        # with none the tank would spawn with 0 shells. It also stocks mines
        # (except in G, which must have none anywhere): the bot captures it in
        # passing, and a captured base holding nothing is a trap the refuel leg
        # cannot tell from a full one until it is standing on it.
        out.append((BASE_NEUTRAL[0], BASE_NEUTRAL[1], NEUTRAL, 90, 90,
                    0 if variant == "G" else 90))
    return out


def gametype_for(variant):
    # open        -> 40 shells / 40 mines / 40 trees
    # tournament  -> shells scale with neutral bases, mines 0, trees 0
    return "open" if variant in ("A", "B", "C") else "tournament"


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


def recenter_shift(terrain, pills, bases, starts):
    """Replicate bolo_map.c mapCenter, quirks included: pills/bases/starts can
    only LOWER bestLeft/bestTop (the right/bottom comparisons in the engine are
    `<`, not `>`), so bestRight/bestBottom come from terrain alone."""
    best_left, best_right = MAP_SIZE, -1
    best_top, best_bottom = MAP_SIZE, -1
    for group in (pills, bases, starts):
        for rec in group:
            best_left = min(best_left, rec[0])
            best_top = min(best_top, rec[1])
    for y in range(MAP_SIZE):
        for x in range(MAP_SIZE):
            if terrain[y][x] is not DEEP_SEA:
                best_left = min(best_left, x)
                best_right = max(best_right, x)
                best_top = min(best_top, y)
                best_bottom = max(best_bottom, y)
    add_x = (255 // 2) - ((best_left + best_right) // 2) - 1
    add_y = (255 // 2) - ((best_top + best_bottom) // 2) - 1
    return add_x, add_y


def build(variant, output):
    terrain = make_map(variant)
    pills = pills_for(variant)
    bases = bases_for(variant)
    sp = spawn_tile(variant)
    starts = [(sp[0], sp[1], 4)]          # dir 4 = east, toward the shore

    for (x, y) in SEA_PILLS:
        assert terrain[y][x] is DEEP_SEA, f"sea pill ({x},{y}) must be in deep sea"
    assert terrain[sp[1]][sp[0]] is DEEP_SEA, "the spawn must be a water tile"
    for (x, y) in ISOLATED_POND:
        assert terrain[y][x] is DEEP_SEA
        # land-locked: all four cardinal neighbours are written land unless
        # they are the other pond tile
        for (dx, dy) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nb = (x + dx, y + dy)
            if nb in ISOLATED_POND:
                continue
            assert terrain[nb[1]][nb[0]] is not DEEP_SEA, \
                f"pond tile {(x, y)} leaks to the sea at {nb}"
    if variant == "G":
        rx, ry = RIVER_TILE
        assert terrain[ry][rx] == RIVER
        assert terrain[ry][rx + 1] is DEEP_SEA, "river tile must touch the sea"

    add_x, add_y = recenter_shift(terrain, pills, bases, starts)
    # mapCenter only moves the map when BOTH axes need shifting. The rows here
    # span y=118..134, whose midpoint is exactly the engine's target, so addY is
    # 0 and the recenter is a no-op however the columns fall.
    assert add_x == 0 or add_y == 0, (
        f"variant {variant}: addX={add_x} addY={add_y} — the map would be "
        f"recentered and file coords would stop matching game coords")

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
    return add_x, add_y


SIDECAR = r'''
-- Scenario sidecar for tests/sea_pills_{V}.map (auto-loaded as <map>.scenario.lua).
--
-- Three jobs.
--
-- 1. The map loader replaces RIVER/DEEP_SEA/BUILDING/HALFBUILDING under every
--    map-file pillbox with ROAD (bolo_map.c), so the dead pills this arena puts
--    in the sea would really sit on one-tile road pedestals -- a boat driving
--    onto one BEACHES and the tank then drowns stepping off.  Restore the deep
--    sea under all of them in on_setup, which runs before the first snapshot.
--    Same trick as tests/water_pills.scenario.lua and blocked_aim.scenario.lua.
--
-- 2. LOADOUT.  A map with a sidecar boots as gameScripted, and gameTypeGetItems
--    treats gameScripted exactly like OPEN (40 shells / 40 mines / 40 trees) --
--    neither -gametype nor scenario.game reaches a -bots tank.  game.spawn_bot's
--    mode argument is the one thing that does (it arms sim->spawnLoadout for the
--    slot BEFORE tankCreate runs, scenario.c:470), so the variants that need an
--    empty tank run with -bots 0 and spawn their own bot here.  The map's pills
--    and our base are then re-owned to whatever slot it landed in.
--
-- 3. Trace the terrain of the shore band every tick to
--    sea_pills_terrain_{V}.log so the test can assert the sequence the harvest
--    actually produces: GRASS(7) -> GRASS+MINE(15) -> CRATER(3) -> RIVER(1)
--    -> BOAT(9).  Values are the ENGINE's (global.h): a mined tile is its base
--    terrain + MINE_SUBTRACT(8).

local T_DEEP_SEA = 0xFF
local OUR_PLAYER = 0
local DEAD_PILLS = {DEAD}
local HOSTILE_PILLS = {HOSTILE}
local OUR_BASE = { {BASEX}, {BASEY} }
local SPAWN_MODE = {MODE}     -- nil = use the -bots tank as it spawned
local TRACE = "sea_pills_terrain_{V}.log"

local watched = {}
local last = {}
local spawn_tried = false

local function own_everything(g, owner)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      local hostile = false
      for _, h in ipairs(HOSTILE_PILLS) do
        if p.x == h[1] and p.y == h[2] then hostile = true end
      end
      if hostile then g.set_pill_owner(i, nil)
      else g.set_pill_owner(i, owner) end
    end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
      g.set_base_owner(i, owner)
    end
  end
end

function on_setup(g)
  for _, p in ipairs(DEAD_PILLS) do
    g.set_tile(p[1], p[2], T_DEEP_SEA)
  end
  -- A live hostile pill standing in deep sea can never be reached, repaired or
  -- driven over, so it stays exactly where the test needs it for the whole run.
  for _, p in ipairs(HOSTILE_PILLS) do
    g.set_tile(p[1], p[2], T_DEEP_SEA)
  end
  own_everything(g, OUR_PLAYER)
  -- Watch the shore band (the entrance is always in it) plus the first sea
  -- column, so a boat appearing anywhere along it is recorded.
  for y = {Y0}, {Y1} do
    for x = {WX0}, {WX1} do
      watched[#watched + 1] = { x, y }
    end
  end
  local f = io.open(TRACE, "w")
  if f then f:write("# tick x y old new\n") f:close() end
end

function on_tick(g, tick)
  if SPAWN_MODE and not spawn_tried and tick >= 2 then
    spawn_tried = true
    local s, err = g.spawn_bot("SeaHarvest", nil, 0, SPAWN_MODE)
    if s == nil then
      g.message("SEA_PILLS_TEST spawn_bot failed: " .. tostring(err))
    else
      g.message("SEA_PILLS_TEST spawned slot=" .. tostring(s)
                .. " mode=" .. SPAWN_MODE)
      own_everything(g, s)
    end
  end
  local out = nil
  for _, w in ipairs(watched) do
    local k = w[2] * 256 + w[1]
    local t = g.map_tile(w[1], w[2])
    if last[k] ~= t then
      if last[k] ~= nil then
        out = out or {}
        out[#out + 1] = string.format("%d %d %d %d %d", tick, w[1], w[2], last[k], t)
      end
      last[k] = t
    end
  end
  if out then
    local f = io.open(TRACE, "a")
    if f then
      f:write(table.concat(out, "\n") .. "\n")
      f:close()
    end
  end
end
'''


def write_sidecar(variant, path):
    dead = "{ " + ", ".join("{ %d, %d }" % p for p in SEA_PILLS) + " }"
    if variant in ("B", "C"):
        hostile = "{ { %d, %d } }" % HOSTILE_SEA_PILL
    else:
        hostile = "{ }"
    mode = ("nil" if gametype_for(variant) == "open"
            else '"%s"' % gametype_for(variant))
    text = (SIDECAR
            .replace("{MODE}", mode)
            .replace("{BASEX}", str(BASE_OURS[0]))
            .replace("{BASEY}", str(BASE_OURS[1]))
            .replace("{V}", variant)
            .replace("{DEAD}", dead)
            .replace("{HOSTILE}", hostile)
            .replace("{Y0}", str(FIELD_Y0))
            .replace("{Y1}", str(FIELD_Y1))
            .replace("{WX0}", str(FIELD_X1 - 2))
            .replace("{WX1}", str(FIELD_X1 + 1)))
    Path(path).write_text(text.lstrip("\n"), encoding="utf-8", newline="\n")


def main():
    args = sys.argv[1:]
    variant = "A"
    output = None
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            variant = args[i + 1].upper()
            i += 2
        else:
            output = args[i]
            i += 1
    if variant == "ALL":
        for v in VARIANTS:
            main_one(v, None)
        return
    main_one(variant, output)


def main_one(variant, output):
    assert variant in VARIANTS, f"unknown variant {variant}"
    here = Path(__file__).parent
    out = Path(output) if output else here / f"sea_pills_{variant}.map"
    add_x, add_y = build(variant, out)
    write_sidecar(variant, out.with_suffix("").with_suffix("")
                  .with_name(f"sea_pills_{variant}.scenario.lua"))
    print(f"Wrote {out} ({out.stat().st_size} bytes) variant={variant} "
          f"gametype={gametype_for(variant)} addX={add_x} addY={add_y}")
    print(f"  peninsula x={field_x0(variant)}..{FIELD_X1} y={FIELD_Y0}..{FIELD_Y1}, "
          f"shore column x={FIELD_X1}, sea from x={FIELD_X1 + 1}")
    print(f"  dead sea pills {SEA_PILLS}, isolated pond {ISOLATED_POND}, "
          f"spawn {spawn_tile(variant)}")


if __name__ == '__main__':
    main()
