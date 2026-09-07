#!/usr/bin/env python3
"""
Generate the MINE-HOARD arena (companion to tests/mine_hoard_test.py).

WHAT IS UNDER TEST (goals.lua refuel_shape, constants.lua
REFUEL_MINE_HOARD_NEEDS_SUPPLY)

    Pool 1 charges a tank that is PARKED on a base an exponential mine-hoard
    "staying cost":

        mine_cost = REFUEL_MINE_HOARD_WEIGHT(8)
                    x (REFUEL_MINE_HOARD_BASE(1.3)^(mines - REFUEL_MINE_FREE(5)) - 1)

    at 23 mines that is 8 x (1.3^18 - 1) = 891.64.  It is an EVICTION lever --
    "do not sit on a pad just to load mines" -- but it was charged whatever the
    tank still needed.  Incident 20260905_231835 bot2 t=67674: 35 armour, 20
    shells, 23 mines standing on base #4 priced its own base at 48 + 891.6 =
    939.6, defend_pill took the goal at 187, and the tank drove off the pad
    short of BOTH its targets purely because of the mines it was carrying --
    then off the pad the same base priced 48 again and it hopped back.

    The author's rule of 2026-09-06: WAIVE the term while the tank is still
    below a target (armour < armour_target or shells < shell_target) AND the
    base it is standing on still holds REFUEL_MIN_STOCK(5) of that same supply.
    At both targets, or on a base that has run dry of everything the tank still
    needs, it applies in full and still evicts.

HOW THE ARENA STAGES A MINE-HEAVY TANK BELOW ITS TARGETS

    There is no set_tank_stock in the scenario API (src/server/scenario.c), so
    the only lever over a tank's stock is the base it drinks from -- and
    bases.c basesRefueling hands out armour FIRST, then shells, then mines, one
    resource per refuel half-tick and only when the previous one is either full
    or unavailable at the base:

        armour if tank armour < 40 and base.armour - 5 >= BASE_MIN_ARMOUR(10)
        else shells if tank shells < 40 and base.shells - 1 >= 0
        else mines  if tank mines  < 40 and base.mines  - 1 >= 0

    So a mine-carrying tank that is NOT full of shells cannot happen by
    drinking alone... unless the base's SHELLS are zero, which blocks the shell
    branch and lets mines through.  Hence the staging phases below, and the one
    arena knob:

    cfg=TANK_FULL_ARMOUR=45  -- the BRAIN's idea of a full tank, not the
        engine's.  TOURNAMENT spawns the tank at the engine's full 40 armour
        (gametype.c) and nothing in a scenario can damage it, so at the stock
        40 the tank is at its armour target the moment it spawns and
            * eval_refuel's base_useless block (which needs
              `info.armour < C.TANK_FULL_ARMOUR and base.armour > 0`) fires
              the moment the pad has no shells, blocks the pad for 200 ticks
              and the tank drives off mid-staging;
            * the pool-1 at-target skip drops the refuel row entirely.
        At 45 the tank is permanently a little below its armour target, the
        pad's armour keeps it a legitimate place to stand, and the staging
        holds still.  It is a knob of the ARENA, not of the thing under test:
        the waive decision reads armour_target and shell_target, both of which
        a real game moves around anyway.

    The other two cfg tokens are pins, not levers:
        cfg=SHELLS_LOW=20              the default moved to 19 on 2026-09-06
                                       and the staged 20 has to sit exactly ON
                                       the low line for fill to be 0
        cfg=REFUEL_BASELINE_SHELLS=40  pins state.shell_target at the ordinary
                                       40 in an arena with nothing to shoot at

    BEWARE: luabrainshandler.h caps BRAIN_INIT_ARG at `char arg[128]`, i.e. 127
    characters, and the overflow is SILENT apart from one "[cfg] BAD TOKEN"
    line in the brain log.  The keel run's four tokens come to 112.
    mine_hoard_test.py asserts every token it passed was actually applied.

    PHASE 1  SHELLS.  pad = (90, 90, 0).  Armour is already full so the engine
             pours shells; the sidecar watches for STAGE_SHELLS (20) and moves
             on.  20 is exactly SHELLS_LOW, which is what makes the whole
             refuel shape NEUTRAL:
                 fill    = 0      (fill needs shells STRICTLY above SHELLS_LOW)
                 mult    = 1.00
                 bonus   = 0      (arm_def = sh_def = 0)
                 urgency = 1.00
             so final_cost = the cached cost, and the ONLY thing the flag moves
             is the +891.64.  The line is hand-computable to the digit.
    PHASE 2  MINES.  pad = (90, 0, 90), shells held at 0 so the shell branch is
             skipped and mines flow.  Stops at STAGE_MINES (23).
    PHASE 3  FREEZE.  pad = (90, 0, 0) held for FREEZE_TICKS.  The tank now
             stands on the pad at armour 40 / shells 20 / mines 23 and nothing
             moves.  The pad still has ARMOUR (18 in the engine's brain units
             -- bases.c basesGetBrainBaseItem divides base armour by five) and
             the tank is below its armour target, so the waiver fires and the
             pad stays cheap.  A keel run does not get this far parked: it is
             priced off the pad partway through phase 2, which is why the test
             compares the two runs TICK BY TICK over the staging rather than at
             one frozen instant.
    PHASE 4  the variant decides what the pad still holds.

VARIANTS
    A   pad restocked to (90, 90, 0) and held.  It can supply the shells the
        tank is short of.
          default -> mines chip WAIVED, refuel priced at the bare cached cost,
                     the tank stays on the pad and drinks to its shell target.
          keel    -> mines charged, the pad prices itself out partway through
                     the mine drink, the tank leaves with 20 shells and never
                     reaches its target.
    B   pad held at (0, 0, 90) -- nothing left but mines.
          default AND keel -> the waiver must NOT fire and the tank must leave.
          This is the safety half of the rule: a base that cannot feed the tank
          still evicts a mine-heavy one.

THE COMPETING GOAL, AND WHY IT IS A PILL
    An over-priced refuel only shows up as BEHAVIOUR if there is somewhere else
    to go.  The arena puts ONE friendly pill 4 tiles east of the pad; the bot
    scoops it (capture_pill -- shoot your own pill down and drive over it, the
    normal Bolo way to move one), which is a goal that needs no shells.

    That last part matters.  The staged tank sits at exactly SHELLS_LOW, and
    at/below the low line build_eval_queue sets has_shells=false and queues NO
    combat pool at all (measured 2026-09-06: `build_eval_queue: 1 total p1=1
    p3=0 p4=0 p5=0 p6=0 p7=0  shells=20 has_shells=false`), so attack_tank,
    attack_pill, capture_base and the rest cannot be the competitor here
    whatever the arena puts on the map -- a distant scripted enemy was tried
    and was never even perceived.

    A second refuel row cannot be it either: eval_refuel emits exactly ONE
    pool-1 entry, because nearest_resupply_base picks its winner BEFORE the
    mine surcharge is added at the shaping site.  Measured with a second
    friendly base and no pill, both runs simply sat on the pad -- the keel run
    priced its own base at 943 and stayed, because 943 was the only number in
    the pool.

    WHAT THE PILL COSTS is under the waived pad price, so both runs eventually
    go and get it.  The difference the test reads is WHEN: the default run
    finishes refuelling first (the pad stays at ~52 the whole time and wins),
    while the keel run is priced off the pad partway through the mine drink
    and leaves with its shells still at 20.

THE SECOND FRIENDLY BASE
    FAR_BASE, 10 tiles east, held full all game.  Two friendly bases means
    team(2) / friendly_bases(2) = 1, so refuel_shape's "gotta share" scarcity
    is a flat 1.00 and the printed multiplier is the plain 1 + fill^2 x 7 --
    one less unrelated factor in a line the test re-derives by hand.  It is
    also where an evicted tank goes in variant B once the pad is dry.

    The map has NO trees and no forest, so the builder pool never has a farm
    job and the LGM never leaves the tank: a returning LGM would clamp pool 1
    at LGM_WAIT_COST and hide the surcharge the test is reading.

THE MAP (same skeleton as tests/generate_refuel_topoff_map.py)
    Everything is written between x 96..156 and y 110..142, so the bounding-box
    midpoint mapRead recenters on is already (126,126) and in-game tiles match
    this file exactly.  West to east:
      x 96..97     PATROL ISLAND (grass), the scripted opponent's home
      x 98..117    OPEN SEA      (unwritten = deep sea; 20 columns, uncrossable)
      x 118..156   FIELD         (grass), everything else

Usage:
    python3 tests/generate_mine_hoard_map.py [A|B] [output_path]
    Default: variant A -> tests/mine_hoard_A.map
Writes <output>.map and the matching <output>.scenario.lua sidecar.
"""

import struct
import sys
from pathlib import Path

GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)
MAP_SIZE = 256

# -- Geometry (the test runner imports these for its assertions) -------------
BODY_Y = (110, 142)            # y extent of both land masses
ISLAND_X = (96, 97)            # patrol island
FIELD_X = (118, 156)           # the bot's world
# x 98..117 is left unwritten: 20 columns of open deep sea between the two.

BOT_SPAWN = (126, 118)         # one-tile deep-sea pond
BASE = (126, 122)              # THE PAD: 4 tiles south of the spawn
FAR_BASE = (136, 122)          # second friendly base, 10 tiles east
OUR_PILL = (130, 122)          # the competing goal, 4 tiles east of the pad
FOE_SPAWN = (96, 112)          # opponent start pond, on the island

PATROL_X = 96                  # the lane the opponent settles into
PATROL_Y0 = 116                # from tests/brains/patrol_ns.lua
PATROL_Y1 = 133

BOT_SLOT = 0                   # our bot owns both bases

# -- Staging ----------------------------------------------------------------
STAGE_SHELLS = 20              # = SHELLS_LOW: fill 0, mult 1.00, bonus 0
STAGE_MINES = 23               # the incident's mine count -> 8 x (1.3^18 - 1)
FULL_STOCK = 90                # BASE_FULL_* -- "give it everything"
FREEZE_TICKS = 500             # ENGINE ticks the staged state is held still
                               # (a brain thinks every SECOND engine tick)

# -- Brain/engine constants this arena is designed against -------------------
SHELLS_LOW = 20                # pinned by cfg (constants.lua default is 19)
ARMOUR_LOW = 15                # constants.lua
TANK_FULL_SHELLS = 40          # constants.lua == engine TANK_FULL_SHELLS
ENGINE_FULL_ARMOUR = 40        # gametype.h TANK_FULL_ARMOUR -- the real cap
ARENA_FULL_ARMOUR = 45         # cfg=TANK_FULL_ARMOUR -- the BRAIN's cap
SHELL_TARGET = 40              # pinned by cfg=REFUEL_BASELINE_SHELLS=40
REFUEL_BASE_COST = 45          # constants.lua
REFUEL_BASE_HOP_PENALTY = 500  # constants.lua
GOAL_SWITCH_RATIO = 0.7        # constants.lua
REFUEL_MINE_FREE = 5           # constants.lua
REFUEL_MINE_HOARD_BASE = 1.3   # constants.lua
REFUEL_MINE_HOARD_WEIGHT = 8   # constants.lua
REFUEL_MIN_STOCK = 5           # constants.lua
REFUEL_FULL_COST_MULT = 8.0    # constants.lua
CONTESTED_BASE_RANGE = 15      # constants.lua
PILL_FIRE_RANGE = 8            # constants.lua
PILLS_MAX_HEALTH = 15          # pillbox.h -- a pill at full health
TANK_THREAT_RADIUS = 6         # threat.lua
START_BASE_RANGE = 9           # starts.c (Chebyshev)
START_TANK_RANGE = 1           # starts.c (Chebyshev)
BASE_STATUS_RANGE_TILES = 7    # bases.h BASE_STATUS_RANGE 1792 / 256
BASE_MIN_ARMOUR = 10           # bases.h
BASE_ARMOUR_BRAIN_DIV = 5      # bases.c basesGetBrainBaseItem: armour / 5
BRAIN_INIT_ARG_MAX = 127       # luabrainshandler.h `char arg[128]`


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def mine_cost_for(mines):
    """goals.lua refuel_shape's raw mine-hoard term for `mines` carried."""
    over = max(0, mines - REFUEL_MINE_FREE)
    if over <= 0:
        return 0.0
    return REFUEL_MINE_HOARD_WEIGHT * (REFUEL_MINE_HOARD_BASE ** over - 1.0)


def base_armour_brain(raw):
    """What info.base.armour reads for a base holding `raw` armour."""
    return raw // BASE_ARMOUR_BRAIN_DIV


def fill_for(shells, armour=ENGINE_FULL_ARMOUR):
    """refuel_shape's `fill` in this arena's target frame."""
    if not (armour > ARMOUR_LOW and shells > SHELLS_LOW):
        return 0.0
    fa = (armour - ARMOUR_LOW) / float(ARENA_FULL_ARMOUR - ARMOUR_LOW)
    fs = (shells - SHELLS_LOW) / float(SHELL_TARGET - SHELLS_LOW)
    return min(fa, fs)


def mult_for(shells, scarcity=1.0, armour=ENGINE_FULL_ARMOUR):
    """refuel_shape's top-off multiplier in this arena's target frame."""
    f = fill_for(shells, armour)
    return 1.0 + f * f * (REFUEL_FULL_COST_MULT - 1.0) * scarcity


def starts_for(_variant):
    return [BOT_SPAWN, FOE_SPAWN]


def make_map(variant):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    y0, y1 = BODY_Y
    for yy in range(y0, y1 + 1):
        for xx in range(ISLAND_X[0], ISLAND_X[1] + 1):
            t[yy][xx] = GRASS
        for xx in range(FIELD_X[0], FIELD_X[1] + 1):
            t[yy][xx] = GRASS
    for (px, py) in starts_for(variant):
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
-- Scenario sidecar for tests/mine_hoard_{V}.map -- GENERATED by
-- tests/generate_mine_hoard_map.py, do not edit by hand.
--
-- Stages a tank that is PARKED on its own base, still below a target, and
-- carrying {MINES} mines -- the shape of the 20260905_231835 bot2 incident --
-- then hands it to the variant.
--
-- The engine refuels armour, THEN shells, THEN mines (bases.c basesRefueling),
-- one item per half-tick, and there is no set_tank_stock in the scenario API.
-- So the only way to a mine-heavy tank that is NOT full of shells is to take
-- the PAD's shells away and let the mine branch run.  Phases:
--
--   1 SHELLS  pad ({FULL}, {FULL}, 0) until the tank holds {SHELLS} shells
--             ({SHELLS} == SHELLS_LOW, pinned by cfg: fill 0, mult 1.00,
--             deficit bonus 0, so the printed refuel cost is the bare cached
--             cost and the mine surcharge is the only moving part).
--   2 MINES   pad ({FULL}, 0, {FULL}); shells held at 0 so the engine's shell
--             branch is skipped and mines flow.  Until the tank holds {MINES}.
--   3 FREEZE  pad ({FULL}, 0, 0), held {FREEZE} ticks.  Nothing moves: the
--             tank sits on the pad at armour 40 / shells {SHELLS} / mines
--             {MINES}.  The pad still has ARMOUR ({FULL} raw = {BARM} in the
--             engine's brain units, bases.c basesGetBrainBaseItem divides by
--             five) and the bot's armour target is 45 (cfg=TANK_FULL_ARMOUR),
--             so the pad can still supply something the tank is short of.
--   4 TEST    variant {V}: {WHAT}
--
-- THE FAR BASE ({FARX},{FARY}) IS HELD FULL THROUGHOUT, in every phase.  It is
-- the competing goal: while the tank is parked on the pad it costs
-- travel + REFUEL_BASE_COST + REFUEL_BASE_HOP_PENALTY(500), which loses to a
-- waived pad and beats a mine-charged one.
--
-- The engine regenerates base stock on its own (bases.c basesUpdateStock), so
-- every phase RE-ASSERTS stock whenever it has crept -- only when it actually
-- changed, or the log is one base-stock event per tick.
--
-- NOTE these are ENGINE ticks.  on_tick counts engine ticks and a brain thinks
-- every second one, so everything here is twice the number the brain's own t=
-- shows.
--
-- Also: pin the starts (on_choose_start fires for every placement, so player p
-- always gets start p+1), and fill the one-tile spawn ponds once every tank is
-- ashore -- a start square has to be DEEP SEA, and a tank that later drives
-- over one without a boat drowns instantly.
local FULL      = {FULL}
local SHELLS    = {SHELLS}
local MINES     = {MINES}
local OWNER     = {SLOT}
local FREEZE    = {FREEZE}
local PADX      = {PADX}
local PADY      = {PADY}
local PONDS     = {{ {PONDS} }}
local GRASS     = 7
local VARIANT   = "{V}"
local ponds_filled = false
local phase        = 1
local freeze_tick  = -1
-- Which base is the pad?  Matched on coordinates, not on index, so a change to
-- the map's base order cannot silently point the staging at the wrong one.
local function is_pad(b) return b.x == PADX and b.y == PADY end
-- Set the pad outright.  `mines` may be nil for "leave the pad's mines alone"
-- (phase 2 lets the tank drain them and must not fight it).
local function set_pad(g, armour, shells, mines)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and is_pad(b) then
      g.set_base_stock(i, armour, shells, mines or (b.mines or 0))
    end
  end
end
-- Re-assert the pad and hold every OTHER base full, but only where the stock
-- has actually drifted -- the server emits EVENT_BASE_STOCK on every write and
-- an unconditional re-assert would be one event per tick.
local function hold(g, armour, shells, mines)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      local wa, ws, wm
      if is_pad(b) then
        wa, ws, wm = armour, shells, mines
      else
        wa, ws, wm = FULL, FULL, 0
      end
      local drift = (b.armour or 0) ~= wa or (b.shells or 0) ~= ws
      if wm ~= nil and (b.mines or 0) ~= wm then drift = true end
      if drift then g.set_base_stock(i, wa, ws, wm or (b.mines or 0)) end
    end
  end
end
function on_choose_start(g, p)
  local n = p + 1
  if n >= 1 and n <= g.num_starts() then return n end
  return nil
end
function on_setup(g)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      g.set_base_owner(i, OWNER)
      g.set_base_stock(i, FULL, FULL, 0)
      g.message(string.format(
        "MINE_HOARD_ARENA base#%d (%d,%d) owner=%d armour=%d shells=%d mines=0 %s",
        i, b.x, b.y, OWNER, FULL, FULL, is_pad(b) and "PAD" or "FAR"))
    end
  end
  g.message(string.format(
    "MINE_HOARD_ARENA variant=%s pills=%d bases=%d starts=%d stage_shells=%d stage_mines=%d",
    VARIANT, g.num_pills(), g.num_bases(), g.num_starts(), SHELLS, MINES))
end
function on_tick(g, tick)
  local t = g.tank(0)
  if phase == 1 then
    -- Armour is already full (TOURNAMENT spawns at the engine's 40), so the
    -- engine pours shells.  Mines stay 0 in the pad: the tank must not pick
    -- any up before it is holding exactly SHELLS shells.
    hold(g, FULL, FULL, 0)
    if t and (t.shells or 0) >= SHELLS then
      phase = 2
      set_pad(g, FULL, 0, FULL)
      g.message(string.format(
        "MINE_HOARD_ARENA PHASE2_MINES tick=%d tank sh=%d mn=%d",
        tick, t.shells or -1, t.mines or -1))
    end
  elseif phase == 2 then
    -- Shells held at 0 (blocks the engine's shell branch); the pad's mines are
    -- left alone so the tank can actually drain them.
    hold(g, FULL, 0, nil)
    if t and (t.mines or 0) >= MINES then
      phase = 3
      freeze_tick = tick
      set_pad(g, FULL, 0, 0)
      g.message(string.format(
        "MINE_HOARD_ARENA PHASE3_FREEZE tick=%d tank arm=%d sh=%d mn=%d",
        tick, t.armour or -1, t.shells or -1, t.mines or -1))
    end
  elseif phase == 3 then
    hold(g, FULL, 0, 0)
    if tick >= freeze_tick + FREEZE then
      phase = 4
      if VARIANT == "A" then
        -- The pad can supply the shells the tank is short of.
        set_pad(g, FULL, FULL, 0)
        g.message("MINE_HOARD_ARENA PHASE4_RESTOCK tick=" .. tostring(tick))
      else
        -- Nothing left but mines.
        set_pad(g, 0, 0, FULL)
        g.message("MINE_HOARD_ARENA PHASE4_DRY tick=" .. tostring(tick))
      end
    end
  else
    if VARIANT == "A" then
      hold(g, FULL, FULL, 0)
    else
      hold(g, 0, 0, nil)
    end
  end
  if not ponds_filled and tick > 200 then
    local afloat = false
    for p = 0, g.max_tanks() - 1 do
      local tk = g.tank(p)
      if tk and tk.boat then afloat = true end
    end
    if not afloat then
      ponds_filled = true
      for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
      g.message("MINE_HOARD_ARENA spawn ponds filled at tick " .. tostring(tick))
    end
  end
end
'''

WHAT = {
    "A": "pad restocked to (90, 90, 0) -- it can still feed the tank",
    "B": "pad held at (0, 0, 90) -- nothing left but mines",
}


def write_sidecar(path, variant):
    ponds = ", ".join("{ %d, %d }" % p for p in starts_for(variant))
    text = SIDECAR.format(
        V=variant, SLOT=BOT_SLOT, FULL=FULL_STOCK, SHELLS=STAGE_SHELLS,
        MINES=STAGE_MINES, FREEZE=FREEZE_TICKS, PONDS=ponds,
        PADX=BASE[0], PADY=BASE[1], FARX=FAR_BASE[0], FARY=FAR_BASE[1],
        BARM=base_armour_brain(FULL_STOCK), WHAT=WHAT[variant])
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def main():
    args = list(sys.argv[1:])
    variant = "A"
    if args and args[0].upper() in ("A", "B"):
        variant = args.pop(0).upper()
    output = (args[0] if args else
              str(Path(__file__).parent / f"mine_hoard_{variant}.map"))
    terrain = make_map(variant)
    starts = starts_for(variant)

    # -- Recenter sanity: only written tiles define the bounding box, and its
    #    integer midpoint must already be (126,126) or every coordinate shifts.
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    for p in starts:
        assert terrain[p[1]][p[0]] is DEEP_SEA, (
            f"start {p} must be deep sea (starts.c startsIsValidSquare)")

    # -- THE POINT OF THE ARENA.  At exactly SHELLS_LOW shells and full engine
    #    armour every other term of the refuel shape is neutral, so the printed
    #    cost is `cached` and the flag's whole effect is the mine surcharge.
    assert STAGE_SHELLS == SHELLS_LOW, STAGE_SHELLS
    assert STAGE_SHELLS < SHELL_TARGET, (STAGE_SHELLS, SHELL_TARGET)
    assert fill_for(STAGE_SHELLS) == 0.0, fill_for(STAGE_SHELLS)
    assert mult_for(STAGE_SHELLS) == 1.0, mult_for(STAGE_SHELLS)
    #    the arena knob: the brain must think the tank is below its armour
    #    target while the engine holds it at 40, or base_useless evicts it
    #    mid-staging and the pool-1 at-target skip drops the row.
    assert ARENA_FULL_ARMOUR > ENGINE_FULL_ARMOUR, ARENA_FULL_ARMOUR
    #    ...and the pad's armour must read at/above REFUEL_MIN_STOCK in the
    #    engine's brain units, which is what the waiver and
    #    nearest_resupply_base's low_stock reject both compare against.
    assert base_armour_brain(FULL_STOCK) >= REFUEL_MIN_STOCK, FULL_STOCK
    #    ...while the engine refuses to hand any of it over, because the tank
    #    is already at ITS full armour.
    assert BASE_MIN_ARMOUR == 10

    # -- The surcharge the test re-derives from the printed mine count.
    assert abs(mine_cost_for(STAGE_MINES) - 891.6432556156597) < 1e-6, \
        mine_cost_for(STAGE_MINES)
    #    ...and it has to dwarf the bare refuel cost, or the flag decides
    #    nothing in this arena.
    assert mine_cost_for(STAGE_MINES) > 10 * REFUEL_BASE_COST

    # -- ONE pill, ours, alive: the competing goal (see the header).  armour
    #    15 = PILLS_MAX_HEALTH so there is nothing to repair; speed 50 = normal
    #    reload.  A friendly pill contributes no threat and no danger, so it
    #    changes no term of the refuel shape -- it is only a price to beat.
    pills = [(OUR_PILL[0], OUR_PILL[1], BOT_SLOT, PILLS_MAX_HEALTH, 50)]
    assert FIELD_X[0] <= OUR_PILL[0] <= FIELD_X[1], OUR_PILL
    assert BODY_Y[0] <= OUR_PILL[1] <= BODY_Y[1], OUR_PILL
    assert terrain[OUR_PILL[1]][OUR_PILL[0]] is not DEEP_SEA, OUR_PILL
    #    Not ON either base, and not on the tank's 4-tile walk to the pad.
    assert OUR_PILL != BASE and OUR_PILL != FAR_BASE
    assert OUR_PILL[0] != BOT_SPAWN[0], OUR_PILL

    # -- TWO bases, both ours: the pad and the competing hop target.  Zero
    #    neutral bases is also what makes TOURNAMENT hand every tank 0 shells
    #    (gametype.c: shells = 2 * (neutralBases/numBases) * 100), which is how
    #    phase 1 starts from empty.
    bases = [(BASE[0], BASE[1], BOT_SLOT, FULL_STOCK, FULL_STOCK, 0),
             (FAR_BASE[0], FAR_BASE[1], BOT_SLOT, FULL_STOCK, FULL_STOCK, 0)]
    assert mdist(BOT_SPAWN, BASE) == 4, mdist(BOT_SPAWN, BASE)
    #    The pad has to be much the closer base from the spawn, or the tank
    #    stages on the wrong one.
    assert mdist(BOT_SPAWN, FAR_BASE) > mdist(BOT_SPAWN, BASE) + 8
    #    The far base must be outside BASE_STATUS_RANGE of the pad, or the
    #    engine's single per-tick "closest base" item -- info.base, the very
    #    field the waiver reads -- could report the wrong one.
    assert mdist(FAR_BASE, BASE) > BASE_STATUS_RANGE_TILES, mdist(FAR_BASE, BASE)
    assert FIELD_X[0] <= FAR_BASE[0] <= FIELD_X[1], FAR_BASE
    assert BODY_Y[0] <= FAR_BASE[1] <= BODY_Y[1], FAR_BASE
    assert terrain[FAR_BASE[1]][FAR_BASE[0]] is not DEEP_SEA, FAR_BASE

    # -- The opponent must contribute nothing: past CONTESTED_BASE_RANGE of
    #    BOTH bases (so the contested term is a flat 0) and far past the
    #    enemy-tank threat radius (so threat.at at either base is 0).
    for y in range(PATROL_Y0, PATROL_Y1 + 1):
        for b in (BASE, FAR_BASE):
            assert mdist((PATROL_X, y), b) > CONTESTED_BASE_RANGE, (y, b)
            assert edist((PATROL_X, y), b) > TANK_THREAT_RADIUS, (y, b)
        assert terrain[y][PATROL_X] is not DEEP_SEA, (PATROL_X, y)
    assert not (PATROL_Y0 <= FOE_SPAWN[1] <= PATROL_Y1), (
        "the patrol lane runs over the opponent's start pond")

    # -- The island really is an island: a full-height sea gap and no river, so
    #    the stranded spawn boat can never be replaced and the opponent can
    #    never reach the field.
    for y in range(BODY_Y[0], BODY_Y[1] + 1):
        for x in range(ISLAND_X[1] + 1, FIELD_X[0]):
            assert terrain[y][x] is DEEP_SEA, (x, y)
    assert GRASS == 7, "the field is plain grass; no forest => no farm job"
    assert ISLAND_X[0] <= FOE_SPAWN[0] <= ISLAND_X[1], FOE_SPAWN

    # -- Starts.  on_choose_start pins them, but keep the engine's own rules
    #    satisfiable so a sidecar failure is a loud one rather than a silent
    #    swap: the bot's start has an own base within START_BASE_RANGE, and the
    #    opponent's start has none.
    assert cheb(BOT_SPAWN, BASE) <= START_BASE_RANGE, cheb(BOT_SPAWN, BASE)
    for p in starts[1:]:
        for b in (BASE, FAR_BASE):
            assert cheb(p, b) > START_BASE_RANGE, (p, b)
        assert cheb(BOT_SPAWN, p) > START_TANK_RANGE, p

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
        for (x, y) in starts:
            f.write(struct.pack('BBB', x, y, 8))   # dir 8 = south
        f.write(encode_map_runs(terrain))

    sidecar = str(Path(output).with_suffix('')) + ".scenario.lua"
    write_sidecar(sidecar, variant)

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  and {sidecar}")
    print(f"  variant {variant}: pad at {BASE}, second base at {FAR_BASE} "
          f"({mdist(FAR_BASE, BASE)} tiles), competing pill at {OUR_PILL}, "
          f"{len(starts)} starts, no trees")
    print(f"  staged tank: armour {ENGINE_FULL_ARMOUR} (brain target "
          f"{ARENA_FULL_ARMOUR}), shells {STAGE_SHELLS} (target "
          f"{SHELL_TARGET}), mines {STAGE_MINES}")
    print(f"  mine surcharge at {STAGE_MINES} mines = "
          f"{REFUEL_MINE_HOARD_WEIGHT} x ({REFUEL_MINE_HOARD_BASE}^"
          f"({STAGE_MINES}-{REFUEL_MINE_FREE}) - 1) = "
          f"{mine_cost_for(STAGE_MINES):.2f}")
    print(f"  phase 4: {WHAT[variant]}")


if __name__ == '__main__':
    main()
