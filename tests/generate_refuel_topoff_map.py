#!/usr/bin/env python3
"""
Generate the refuel TOP-OFF arena (companion to tests/refuel_topoff_test.py).

WHAT IS UNDER TEST (goals.lua refuel_need, constants.lua
REFUEL_TOPOFF_CANDIDATE)

    The author's rule of 2026-09-06: "20 [shells] is a good number to be like
    'you're full enough, go do stuff unless it's worth the cost to keep
    recharging'".  A top-off past the low watermark has to be an option that
    LOSES on price, not an option that does not exist.

    Before the flag, a refuel row only existed while the tank was at/below
    ARMOUR_LOW (15) or SHELLS_LOW (20).  With REFUEL_TOPOFF_CANDIDATE on it
    exists anywhere below the dynamic full targets, and the existing quadratic
    ramp in goals.lua refuel_shape prices it:

        fill = min((sh - 20)/(sh_target - 20), (arm - 15)/(arm_target - 15))
        mult = 1 + fill^2 * (REFUEL_FULL_COST_MULT 8 - 1) * scarcity

    At 30/40 shells that is fill 0.50 -> mult 2.75 with scarcity 1.

WHAT THE ARENA HAS TO PROVE, AND WHY IT IS SHAPED LIKE THIS

    The obvious arena -- park a tank on a pad and watch it fill -- proves
    NOTHING, because it already fills to 40 today.  build_eval_queue keeps
    pool 1 alive for as long as `state.goal.kind == "refuel_at_base"`, and
    init.lua's completion test is armour_target/shell_target, not the low
    line, so a tank that reaches a base at 0 shells drinks all the way to 40
    with the flag off.  (Measured, 2026-09-06: tests/refuel_pad_reach_A,
    shells 0 at t=231 -> 40 at t=531, released at 40.)

    The flag only changes what happens to a tank that is NOT already holding a
    refuel goal and is ABOVE both low lines.  So the arena has to get the tank
    into exactly that state, and the only lever a scenario sidecar has over a
    tank's stock is the base it drinks from (there is no set_tank_stock).
    Hence two phases:

      PHASE 1 (t 0..DRAIN)  TOURNAMENT + -ranked spawns the tank with 0 shells.
                            It drives 4 tiles to the base and drinks at 1 shell
                            per BASES_HALFTICK_TYPE_SHELL (~7.5 ticks).  The
                            SIDECAR watches the tank and, the moment it reaches
                            PHASE1_SHELLS (30), sets the base's shells to 0 and
                            HOLDS them there -- the engine regenerates base
                            stock on its own (bases.c basesUpdateStock, +1 per
                            BASE_TICKS_BETWEEN_REFUEL), so without the hold the
                            tank simply keeps drinking to 40 and the arena
                            proves nothing.  (First cut of this arena did
                            exactly that: base seeded with 30 shells, tank
                            still reached 40 by t=536.)  The tank is now
                            standing on an empty base with 30 shells -- above
                            SHELLS_LOW, below full.  The base can supply
                            nothing it needs, so the brain blocks it
                            (base_useless) and the tank leaves.

      PHASE 2 (DRAIN+DRY)   DRY_TICKS after the drain the sidecar refills the
                            base to 90/90.  The restock is timed off the DRAIN,
                            not off a fixed tick, because how long the drink
                            takes depends on the engine's refuel half-tick and
                            on how fast the tank got to the pad.  Now the
                            question is the whole question: does a 30/40 tank
                            with nothing else to do get a refuel row?
                              flag on  -> yes, priced base x2.75; with an empty
                                          arena it wins and the tank tops off.
                              flag off -> no row at all; the tank stays at 30
                                          shells for the rest of the run.

    THE ARENA IS DELIBERATELY EMPTY of pills.  Two reasons, both fatal
    otherwise:
      * build_eval_queue widens the refuel gate when `combat_ahead` (any live
        hostile/neutral pill anywhere on the map) to `armour <= ARMOUR_COMBAT
        (30) or shells <= SHELLS_COMBAT (30)`.  With any pill on the map a
        30-shell tank would be a refuel candidate with the flag OFF too, and
        the test would pass for the wrong reason.
      * a reachable pill is a competing goal, and the point of phase 2 is that
        refuel is competing against nothing.

    THE BOT IS GIVEN cfg=REFUEL_BASELINE_SHELLS=40.  state.shell_target is
    max(REFUEL_BASELINE_SHELLS, mission + reserve, combat) and in an arena with
    no pills the mission term is 0, so the target would settle at 25 and a
    30-shell tank would already be "full".  Pinning the baseline at 40 makes
    the target 40 -- the ordinary value in a real game -- so `fill` spans the
    range the ramp is actually about.  It is a knob of the arena, not of the
    thing under test.

THE MAP (identical skeleton to tests/generate_refuel_pad_reach_map.py)
    Everything is written between x 96..156 and y 110..142, so the bounding-box
    midpoint mapRead recenters on is already (126,126) and in-game tiles match
    this file exactly.  West to east:
      x 96..97     PATROL ISLAND (grass), the scripted opponent's home
      x 98..117    OPEN SEA      (unwritten = deep sea; 20 columns, uncrossable)
      x 118..156   FIELD         (grass), everything else
    (126,118)  BOT_SPAWN: a one-tile deep-sea pond (starts.c startsIsValidSquare
               demands deep sea).  The sidecar fills it once nobody is afloat.
    (126,122)  BASE, 4 tiles south, the bot's, fully stocked at setup.
    (96,112)   the opponent's start pond, on the island; it then paces
               x=96, y 116..133 forever.  It exists only so TOURNAMENT is a
               real game; it is 30+ tiles away behind a full-height sea with no
               river anywhere, so it can never be reached, never contributes
               threat, and never contests the base.
    (152,138)  variant B only: an ALLIED start pond for a second bot running
               tests/brains/idle.lua.

VARIANTS
    A   one bot + the unreachable opponent, who is NOT allied.  refuel_shape
        reads team = 2 over 1 friendly base (see scarcity_for: the engine's
        allies bitmap includes the player's own slot, so a lone tank already
        counts as 2), giving scarcity 2.50 and mult 5.375 at fill 0.50.
    B   A plus an allied idle bot: team = 3 over the same 1 base, scarcity
        4.00, mult 8.000 at the same fill.  The ally is parked 26 tiles from
        the base and never moves, so it changes the scarcity term and nothing
        else -- which is the comparison the arena is for.

Usage:
    python3 tests/generate_refuel_topoff_map.py [A|B] [output_path]
    Default: variant A -> tests/refuel_topoff_A.map
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
BASE = (126, 122)              # 4 tiles south of the spawn
FOE_SPAWN = (96, 112)          # opponent start pond, on the island
ALLY_SPAWN = (152, 138)        # variant B only: the idle teammate's pond

PATROL_X = 96                  # the lane the opponent settles into
PATROL_Y0 = 116
PATROL_Y1 = 133

BOT_SLOT = 0                   # our bot owns the base
TEAM = 0                       # variant B: the ally joins this team
# Player slots the sidecar puts on TEAM. A allies nobody -- the scripted
# opponent must stay hostile, or refuel_shape counts it as a teammate and A's
# scarcity is B's.  B allies our bot (0) and the idle teammate (2); the
# opponent keeps slot 1 and stays on its own side.
ALLIED_SLOTS = {"A": (), "B": (0, 2)}

# -- The two phases ---------------------------------------------------------
PHASE1_SHELLS = 30             # what the base can give before it runs dry.
                               # Above SHELLS_LOW (20) and below the pinned
                               # shell target (40) -> fill 0.50, mult 2.75.
FULL_STOCK = 90                # BASE_FULL_SHELLS -- phase 2 refill
DRY_TICKS = 600                # ENGINE ticks the base is held empty after the
                               # drain, so the tank gives up on it and leaves.
                               # The scenario's on_tick counts ENGINE ticks and
                               # a brain thinks every SECOND engine tick, so
                               # this is ~300 brain ticks.
STOCK_PERIOD = 150             # re-assert cadence (engine ticks) after refill

# -- Brain/engine constants this arena is designed against -------------------
SHELLS_LOW = 20                # constants.lua
ARMOUR_LOW = 15                # constants.lua
TANK_FULL_SHELLS = 40          # constants.lua
TANK_FULL_ARMOUR = 40          # constants.lua
SHELL_TARGET = 40              # pinned by cfg=REFUEL_BASELINE_SHELLS=40
REFUEL_FULL_COST_MULT = 8.0    # constants.lua
REFUEL_SHARE_RATIO_K = 1.5     # constants.lua
CONTESTED_BASE_RANGE = 15      # constants.lua
TANK_THREAT_RADIUS = 6         # threat.lua
START_BASE_RANGE = 9           # starts.c (Chebyshev)
START_TANK_RANGE = 1           # starts.c (Chebyshev)
BASE_SHELLS_GIVE = 1           # bases.h -- 1 shell per refuel half-tick


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def fill_for(shells, shell_target=SHELL_TARGET):
    """refuel_shape's `fill` for a full-armour tank at `shells`."""
    if shells <= SHELLS_LOW:
        return 0.0
    return min(1.0, (shells - SHELLS_LOW) / float(shell_target - SHELLS_LOW))


def mult_for(shells, scarcity=1.0, shell_target=SHELL_TARGET):
    """refuel_shape's top-off multiplier for a full-armour tank."""
    f = fill_for(shells, shell_target)
    return 1.0 + f * f * (REFUEL_FULL_COST_MULT - 1.0) * scarcity


def scarcity_for(variant):
    """refuel_shape's scarcity for this arena: 1 + RATIO_K * (team/bases - 1).

    NOTE the +1.  refuel_shape counts `team` as 1 (self) plus the set bits of
    info.allies, and the engine's allies bitmap (players.c
    playersGetAlliesBitMap) already includes the player's own slot -- so a lone
    tank counts as a team of 2.  That is a pre-existing brain quirk, not
    something this arena is testing; it is written down here so the numbers the
    log prints are the numbers this file predicts.  One friendly base either
    way, so:
        A: 1 tank  -> team 2 -> scarcity 1 + 1.5 * 1 = 2.50
        B: 2 tanks -> team 3 -> scarcity 1 + 1.5 * 2 = 4.00
    """
    tanks = 2 if variant == "B" else 1
    team = tanks + 1
    return 1.0 + REFUEL_SHARE_RATIO_K * max(0.0, team / 1.0 - 1.0)


def starts_for(variant):
    s = [BOT_SPAWN, FOE_SPAWN]
    if variant == "B":
        s.append(ALLY_SPAWN)
    return s


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
-- Scenario sidecar for tests/refuel_topoff_{V}.map -- GENERATED by
-- tests/generate_refuel_topoff_map.py, do not edit by hand.
--
-- Four jobs.
--
-- 0. ALLY ONLY THE SLOTS THAT SHOULD BE ALLIED (variant B's idle teammate),
--    from on_tick rather than on_setup because the bots are still joining at
--    setup time.  The scripted opponent must stay hostile or refuel_shape
--    counts it in the team size and prices variant A like variant B.
--
-- 1. PIN THE STARTS.  on_choose_start fires for every placement, so player p
--    always gets start p+1: the bot at ({SPX},{SPY}) beside the base, the
--    unreachable opponent on the island, and (variant B) the idle teammate in
--    the far corner.  Without it startsGetStartTournament is free to hand the
--    bot any start with an own base within 9 tiles.
--
-- 2. PHASE 1 -- stop the drink at exactly {P1SH} shells.  The tank spawns with
--    0 (TOURNAMENT + -ranked; -ranked is required because
--    serverSimApplyScenarioCommit otherwise stamps gameScripted over the game
--    type of any map with a sidecar, and gameScripted hands out a full tank).
--    The moment it reaches {P1SH} the base's shells go to 0 and are HELD there
--    until the restock: the engine regenerates base stock by itself
--    (bases.c basesUpdateStock), so seeding the base with {P1SH} and walking
--    away is not enough -- the tank just keeps drinking to {TGT}.  Held at 0,
--    the base can supply nothing the tank needs (its armour is already full),
--    so the brain blocks it as base_useless and the tank leaves with {P1SH}
--    shells: above SHELLS_LOW ({LOW}), below the shell target ({TGT}).
--
-- 3. PHASE 2 -- {DRY} ticks after the drain the base is refilled to
--    {FULL}/{FULL} and re-asserted every {PERIOD} ticks, with the ARMOUR value
--    alternating
--    {FULL}/{FULLM1} because the server only emits EVENT_BASE_STOCK when a
--    base's stock actually CHANGES and the bot's observation has to stay
--    fresh.  NOTE these are ENGINE ticks -- on_tick counts engine ticks and a
--    brain thinks every second one, so everything here is twice the number the
--    brain's own t= shows.  Mines are held at 0 throughout: the tank must never pick any up,
--    or refuel_shape's exponential mine-hoard surcharge joins the cost and the
--    multiplier stops being hand-computable.
--
-- 4. FILL THE SPAWN PONDS once every tank is ashore.  A start square has to be
--    DEEP SEA, so the arena digs one-tile ponds in the grass; a tank that
--    later drives over one without a boat drowns instantly.
local FULL     = {FULL}
local P1SH     = {P1SH}
local OWNER    = {SLOT}
local TEAM     = {TEAM}
local ALLIED   = {{ {ALLIED} }}   -- player slots put on one team (empty in A)
local DRY      = {DRY}
local PERIOD   = {PERIOD}
local PONDS    = {{ {PONDS} }}
local GRASS    = 7
local flip = 0
local allied_done = (#ALLIED == 0)
local ponds_filled = false
local drained = false
local drain_tick = -1
local restocked = false
local function set_all(g, armour, shells)
  for i = 1, g.num_bases() do g.set_base_stock(i, armour, shells, 0) end
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
        "REFUEL_TOPOFF_ARENA base#%d (%d,%d) owner=%d armour=%d shells=%d mines=0",
        i, b.x, b.y, OWNER, FULL, FULL))
    end
  end
  g.message(string.format(
    "REFUEL_TOPOFF_ARENA pills=%d starts=%d phase1_shells=%d dry_ticks=%d",
    g.num_pills(), g.num_starts(), P1SH, DRY))
end
function on_tick(g, tick)
  -- ONLY the listed slots are allied, and NOT from on_setup: the bots are
  -- still joining there and g.tank(2) is nil, so the ally silently stayed on
  -- its own team and variant B priced exactly like variant A.  Retried from
  -- on_tick until every listed slot has a tank.
  if not allied_done then
    local all = true
    for _, p in ipairs(ALLIED) do
      if g.tank(p) then g.set_team(p, TEAM) else all = false end
    end
    if all then
      allied_done = true
      if #ALLIED > 0 then
        g.message("REFUEL_TOPOFF_ARENA allied slots on team " .. tostring(TEAM)
                  .. " at tick " .. tostring(tick))
      end
    end
  end
  if not drained then
    local t = g.tank(0)
    if t and (t.shells or 0) >= P1SH then
      drained = true
      drain_tick = tick
      set_all(g, FULL, 0)
      g.message(string.format("REFUEL_TOPOFF_ARENA DRAINED tick=%d tank_shells=%d",
                              tick, t.shells or -1))
    end
  elseif not restocked then
    if tick >= drain_tick + DRY then
      restocked = true
      set_all(g, FULL, FULL)
      g.message("REFUEL_TOPOFF_ARENA RESTOCK tick=" .. tostring(tick))
    else
      -- Beat the engine's own stock regeneration back down.  Only when it has
      -- actually crept up, so the log is not one base-stock event per tick.
      for i = 1, g.num_bases() do
        local b = g.base(i)
        if b and (b.shells or 0) > 0 then g.set_base_stock(i, FULL, 0, 0) end
      end
    end
  else
    if tick % PERIOD == 0 then
      flip = 1 - flip
      set_all(g, FULL - flip, FULL)
    end
  end
  if not ponds_filled and tick > 200 then
    local afloat = false
    for p = 0, g.max_tanks() - 1 do
      local t = g.tank(p)
      if t and t.boat then afloat = true end
    end
    if not afloat then
      ponds_filled = true
      for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
      g.message("REFUEL_TOPOFF_ARENA spawn ponds filled at tick " .. tostring(tick))
    end
  end
end
'''


def write_sidecar(path, variant):
    ponds = ", ".join("{ %d, %d }" % p for p in starts_for(variant))
    text = SIDECAR.format(
        V=variant, SLOT=BOT_SLOT, TEAM=TEAM, FULL=FULL_STOCK,
        FULLM1=FULL_STOCK - 1, P1SH=PHASE1_SHELLS, LOW=SHELLS_LOW,
        TGT=SHELL_TARGET, DRY=DRY_TICKS, PERIOD=STOCK_PERIOD,
        ALLIED=", ".join(str(i) for i in ALLIED_SLOTS[variant]),
        SPX=BOT_SPAWN[0], SPY=BOT_SPAWN[1], PONDS=ponds)
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def main():
    args = list(sys.argv[1:])
    variant = "A"
    if args and args[0].upper() in ("A", "B"):
        variant = args.pop(0).upper()
    output = (args[0] if args else
              str(Path(__file__).parent / f"refuel_topoff_{variant}.map"))
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

    # -- THE POINT OF THE ARENA: phase 1 leaves the tank strictly between the
    #    low watermark and the shell target, so the refuel row's existence is
    #    the ONLY thing the flag decides.
    assert SHELLS_LOW < PHASE1_SHELLS < SHELL_TARGET, PHASE1_SHELLS
    assert abs(fill_for(PHASE1_SHELLS) - 0.5) < 1e-9, fill_for(PHASE1_SHELLS)
    assert abs(mult_for(PHASE1_SHELLS) - 2.75) < 1e-9, mult_for(PHASE1_SHELLS)
    assert abs(scarcity_for("A") - 2.50) < 1e-9, scarcity_for("A")
    assert abs(scarcity_for("B") - 4.00) < 1e-9, scarcity_for("B")
    assert abs(mult_for(PHASE1_SHELLS, scarcity_for("A")) - 5.375) < 1e-9
    assert abs(mult_for(PHASE1_SHELLS, scarcity_for("B")) - 8.000) < 1e-9
    #    ...and B really does price the same fill higher than A.
    assert mult_for(PHASE1_SHELLS, scarcity_for("B")) > \
        mult_for(PHASE1_SHELLS, scarcity_for("A")) + 1.0

    # -- NO PILLS.  A single live pill anywhere turns build_eval_queue's
    #    combat_ahead on, which widens the refuel gate to shells <=
    #    SHELLS_COMBAT (30) and would make the 30-shell tank a candidate with
    #    the flag OFF as well.  There is no pill record in the file at all.
    pills = []

    # -- ONE base, ours.  Zero neutral bases is also what makes TOURNAMENT hand
    #    every tank 0 shells (gametype.c: shells = 2 * (neutralBases/numBases)
    #    * 100), which is how phase 1 starts from empty.
    bases = [(BASE[0], BASE[1], BOT_SLOT, FULL_STOCK, FULL_STOCK, 0)]
    assert mdist(BOT_SPAWN, BASE) == 4, mdist(BOT_SPAWN, BASE)

    # -- The dry window has to be long enough for the tank to give up on the
    #    empty base and drive off it (the block is 200 BRAIN ticks).
    assert DRY_TICKS >= 2 * 200, DRY_TICKS

    # -- The opponent must contribute nothing: past CONTESTED_BASE_RANGE of the
    #    base (so the contested term is a flat 0) and far past the enemy-tank
    #    threat radius (so threat.at at the base is 0 and the danger term with
    #    no pills anywhere is exactly 0).
    for y in range(PATROL_Y0, PATROL_Y1 + 1):
        assert mdist((PATROL_X, y), BASE) > CONTESTED_BASE_RANGE, y
        assert edist((PATROL_X, y), BASE) > TANK_THREAT_RADIUS, y
        assert terrain[y][PATROL_X] is not DEEP_SEA, (PATROL_X, y)
    assert not (PATROL_Y0 <= FOE_SPAWN[1] <= PATROL_Y1), (
        "the patrol lane runs over the opponent's start pond")

    # -- Variant B's teammate is an ordinary allied tank that never moves
    #    (tests/brains/idle.lua).  It must be far enough away to change nothing
    #    but refuel_shape's team-per-base ratio.
    if variant == "B":
        assert edist(ALLY_SPAWN, BASE) > TANK_THREAT_RADIUS, ALLY_SPAWN
        assert mdist(ALLY_SPAWN, BASE) > CONTESTED_BASE_RANGE, ALLY_SPAWN
        assert FIELD_X[0] <= ALLY_SPAWN[0] <= FIELD_X[1], ALLY_SPAWN
        assert BODY_Y[0] <= ALLY_SPAWN[1] <= BODY_Y[1], ALLY_SPAWN

    # -- The island really is an island: a full-height sea gap, and no river
    #    anywhere, so the stranded spawn boat can never be replaced.
    for y in range(BODY_Y[0], BODY_Y[1] + 1):
        for x in range(ISLAND_X[1] + 1, FIELD_X[0]):
            assert terrain[y][x] is DEEP_SEA, (x, y)
    assert GRASS == 7, "the field is plain grass; no river => no boat, ever"
    assert ISLAND_X[0] <= FOE_SPAWN[0] <= ISLAND_X[1], FOE_SPAWN

    # -- Starts.  on_choose_start pins them, but keep the engine's own rules
    #    satisfiable so a sidecar failure is a loud one rather than a silent
    #    swap: the bot's start has an own base within START_BASE_RANGE, and no
    #    other start does.
    assert cheb(BOT_SPAWN, BASE) <= START_BASE_RANGE, cheb(BOT_SPAWN, BASE)
    for p in starts[1:]:
        assert cheb(p, BASE) > START_BASE_RANGE, p
        assert cheb(BOT_SPAWN, p) > START_TANK_RANGE, p

    with open(output, 'wb') as f:
        f.write(b'BMAPBOLO')
        f.write(struct.pack('B', 1))
        f.write(struct.pack('B', len(pills)))
        f.write(struct.pack('B', len(bases)))
        f.write(struct.pack('B', len(starts)))
        for x, y, owner, armour, shells, mines in bases:
            f.write(struct.pack('BBBBBB', x, y, owner, armour, shells, mines))
        for (x, y) in starts:
            f.write(struct.pack('BBB', x, y, 8))   # dir 8 = south
        f.write(encode_map_runs(terrain))

    sidecar = str(Path(output).with_suffix('')) + ".scenario.lua"
    write_sidecar(sidecar, variant)

    scar = scarcity_for(variant)
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  and {sidecar}")
    print(f"  variant {variant}: {len(starts)} start(s), 1 base at {BASE} "
          f"(4 tiles from the spawn {BOT_SPAWN}), NO pills")
    print(f"  phase 1: base gives {PHASE1_SHELLS} shells then runs dry -> tank "
          f"sits between SHELLS_LOW {SHELLS_LOW} and target {SHELL_TARGET}")
    print(f"  phase 2: restock to {FULL_STOCK}/{FULL_STOCK} {DRY_TICKS} engine "
          f"ticks after the drain, re-asserted every {STOCK_PERIOD}")
    print(f"  expected scarcity {scar:.2f} -> at {PHASE1_SHELLS} shells "
          f"fill {fill_for(PHASE1_SHELLS):.2f}, mult "
          f"{mult_for(PHASE1_SHELLS, scar):.3f}")


if __name__ == '__main__':
    main()
