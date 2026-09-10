#!/usr/bin/env python3
"""
Generate the capture-pill LGM-hunt arenas (companion to
tests/capture_lgm_hunt_test.py).

WHAT IS BEING MEASURED
----------------------
A bot on capture_pill drives at a dead pillbox to grab it.  An ENEMY BUILDER
standing on or beside that corpse is rebuilding it out from under us: the
moment his repair lands the free pill is a live enemy pillbox and the capture
is gone.  KEEL drives straight past him -- the turn keys belong to navigation,
and the gunsight driver that would let the existing opportunistic LGM shot
actually open is gated on goal == kill_lgm, so under capture_pill the crosshair
sits wherever the last goal left it and the impact-point gate never opens.

The change (constants.lua CAPTURE_LGM_HUNT) makes capture_pill builder-aware:

  * a hostile LGM within CAPTURE_LGM_HUNT_RADIUS tiles (Chebyshev) OF THE
    TARGET PILL -- not of the tank -- puts the bot in the hunt;
  * so does the target pill's ARMOUR GOING UP, which is the same repair seen
    from the outside when the man himself is hidden in trees;
  * the THROTTLE stays navigation's, always (steering.lua only ever writes
    `turn_corr`, never `correction`, so no throttle branch can see the hunt);
  * the TURN keys go to the kill_lgm aim solution while it points within
    CAPTURE_LGM_HUNT_TOL_BRADS of the navigation heading (widened to
    ..._TOL_NEAR_BRADS inside ..._NEAR_TILES of the pill);
  * the goal stays capture_pill throughout -- no substate, no target_id, no
    cost change.

THREE ARENAS
------------
All three share ONE field, so nothing but the sidecar and the -bot-init tokens
moves between them.

  H1  THE HUNT.  Two tanks.  p1 runs tests/brains/farm_beside.lua: it parks at
      P1_PARK and keeps sending its man to ROAD_TILE (and to FARM_TILE when it
      runs low on trees), both of which are Chebyshev 2 from the corpse -- so a
      HOSTILE LGM is standing inside the hunt box, on GRASS where our bot can
      see him, for most of the run.  It never fires.

      WHY A SCRIPTED ENEMY.  Measured on the first draft with a second
      GoalHunter: it never sent its man near the corpse at all.  It starts with
      trees=40 against a reserve of 4, so the builder pool's FARM row has no
      candidate for the whole run (cands=0), and everything else it does --
      attack_base, capture_base, take_cover -- happens elsewhere on the map.
      The obvious alternative, a DAMAGED ENEMY PILLBOX beside the corpse to
      walk the man there, shells our bot for the entire approach and turns the
      run into a take_cover test.

  H0  THE CONTROL.  H1's arena and H1's enemy exactly, with p0 on preset=keel.
      No hunt line may appear at all.

  H2  THE ARMOUR TRIGGER.  ONE tank, no enemy and no man to see.  The sidecar
      pulses the corpse's armour 0 -> ARMOUR_PULSE -> 0 while p0 approaches,
      which is a repair landing on the pill with the builder invisible.  The
      hunt must engage on src=armour.  The pulse is short (PULSE_HOLD ticks) so
      the pill is dead again long before the next replan and the goal never
      leaves capture_pill, while CAPTURE_LGM_HUNT_ARMOUR_TICKS (100) keeps the
      trigger hot across the gap.

A REFILLING CORPSE.  Grabbing a dead pill takes one tick of driving over it,
so a single corpse buys ONE approach and the arena is spent.  The map
therefore carries SPARES, hidden at setup; whenever the live corpse goes into
a tank the sidecar waits until p0 has driven RESPAWN_AWAY tiles clear and then
pops the next spare back onto the corpse tile with show_pill(n, x, y) (which
lands it DEAD, on the ground, which is exactly what a capture wants).  Each
spare is one more full approach with the builder in the box.

GEOMETRY
--------
    x:      106 .. 110 .......... 126 127 128 ........ 133 ........ 146
    y=116                              (128,116) p1 start pond
    y=120   (110,120) p0 base
    y=121                              (128,121) P1_PARK
    y=125                              (128,125) FARM_TILE (forest)
    y=126   (110,126) p0 pond      (126,126) CORPSE  (128,126) ROAD_TILE
    y=136
    everything else GRASS

The enemy parks due NORTH of its errand tile, never due east behind its own
man.  Due east put it inside the navigate drive-by shot's window (8 tiles, 8
bradians) and that shot claimed KEY_SHOOT every tick before the kill-LGM block
could -- the hunt aimed, drove the gunsight down and reported the shot READY,
tick after tick, as `ready_busy`.  From anywhere on p0's approach lane the
park sits ~30-48 bradians off the bearing to the errand tile, clear of the aim
half of that gate however close it is.  Five tiles north is also as close as
the geometry allows, and that matters: the man's round trip is the arena's
duty cycle, and a long walk means he is standing in the hunt box for only a
sliver of each trip.

p1 has NO base: it is scripted, it never refuels, and a second base is an
attack_base / capture_base target that would pull p0 off the capture.

IMPORTANT: mapRead RECENTERS off-centre maps (bolo_map.c) -- the terrain
bounding box midpoint is shifted to (126,126).  The field is symmetric about
both axes, so the shift is a no-op and in-game coordinates match this file.
main() asserts it.  mapRead ALSO replaces RIVER/DEEP_SEA/BUILDING under every
map-file pillbox with ROAD, so no pill here is written on water.

Usage:
    python3 tests/generate_capture_lgm_hunt_map.py [H0|H1|H2] [output_path]
    Default: H1 -> tests/capture_lgm_hunt_H1.map
"""

import struct
import sys
from pathlib import Path

MAP_SIZE = 256
DEEP_SEA = None          # background sentinel (unwritten cells read as deep sea)
GRASS = 7
FOREST = 5

VARIANTS = ("H0", "H1", "H2")

# ── Geometry (the test runner and farm_beside.lua share these numbers) ───
FIELD = (106, 146, 116, 136)      # x0, x1, y0, y1 -- midpoints 126/126
CORPSE = (126, 126)
ROAD_TILE = (128, 126)            # grass; the enemy's default errand
FARM_TILE = (128, 125)            # forest; his tree top-up
P1_PARK = (128, 121)
SPAWN0 = (110, 126)               # p0: 16 tiles west of the corpse
SPAWN1 = (128, 116)               # p1: drives 5 tiles south to P1_PARK
BASE0 = (110, 120)
# Spare pills, hidden at setup, popped back onto CORPSE one at a time.
# Fourteen of them: hiddenPills in scenario.c is a uint16_t bitmask, so
# fifteen pills total is the ceiling, and every one is another approach with
# the builder in the box.  The fire half of this feature is a coin flip per
# pass by construction -- the tank never stops, and the gunsight only moves in
# half-tile steps -- so the arena's job is to hand it enough passes.
SPARES = [(108, 118), (108, 122), (108, 126), (108, 130), (108, 134),
          (144, 118), (144, 122), (144, 126), (144, 130), (144, 134),
          (112, 118), (112, 134), (140, 118), (140, 134)]

# ── Brain constants this arena is designed against (constants.lua) ───────
HUNT_RADIUS = 7                   # CAPTURE_LGM_HUNT_RADIUS: a EUCLIDEAN circle of
                                  # tile-centre deltas since 2026-09-10
                                  # (CAPTURE_LGM_HUNT_CIRCLE); was a Chebyshev 2 box
HUNT_NEAR_TILES = 4               # CAPTURE_LGM_HUNT_NEAR_TILES
HUNT_TOL_BRADS = 26               # CAPTURE_LGM_HUNT_TOL_BRADS
HUNT_TOL_NEAR_BRADS = 45          # CAPTURE_LGM_HUNT_TOL_NEAR_BRADS
HUNT_ARMOUR_TICKS = 100           # CAPTURE_LGM_HUNT_ARMOUR_TICKS
SHOOT_RANGE = 8                   # KILL_LGM_SHOOT_RANGE
OPP_TANK_RANGE = 8                # steering.lua navigate opportunistic shot
OPP_TANK_AIM = 8                  # ...and its aim gate, in bradians

# ── Sidecar numbers ─────────────────────────────────────────────────────
ARMOUR_PULSE = 2                  # H2: armour the invisible "repair" reaches
PULSE_HOLD = 6                    # H2: sim ticks it is held there
PULSE_PERIOD = 140                # H2: sim ticks between pulses
RESPAWN_AWAY = 6                  # tiles p0 must be clear before a spare pops
RESPAWN_MAX_WAIT = 400            # sim ticks after which it pops anyway


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def nbots(variant):
    """H2 measures ONE bot's armour trigger; a second tank only adds noise."""
    return 1 if variant == "H2" else 2


def make_map(variant):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y0, y1 = FIELD
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            t[yy][xx] = GRASS
    # The one forest tile exists in every variant so all three arenas are the
    # same ground; in H2 nobody chops it.
    t[FARM_TILE[1]][FARM_TILE[0]] = FOREST
    # Start squares must be deep sea (starts.c startsIsValidSquare). The
    # sidecar fills each pond once its tank is ashore.
    t[SPAWN0[1]][SPAWN0[0]] = DEEP_SEA
    if nbots(variant) > 1:
        t[SPAWN1[1]][SPAWN1[0]] = DEEP_SEA
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


def main():
    args = list(sys.argv[1:])
    variant = "H1"
    if args and args[0].upper() in VARIANTS:
        variant = args.pop(0).upper()
    output = args[0] if args else str(
        Path(__file__).parent / f"capture_lgm_hunt_{variant}.map")
    terrain = make_map(variant)

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    assert terrain[SPAWN0[1]][SPAWN0[0]] is DEEP_SEA, (
        "the start square must be deep sea (starts.c startsIsValidSquare)")

    # Both errand tiles have to sit INSIDE the hunt box, or the man can stop
    # somewhere the detector will not look and the arena proves nothing.
    for tile, what in ((ROAD_TILE, "ROAD_TILE"), (FARM_TILE, "FARM_TILE")):
        assert cheb(tile, CORPSE) <= HUNT_RADIUS, (
            f"{what} {tile} is Chebyshev {cheb(tile, CORPSE)} from the corpse "
            f"{CORPSE}, outside CAPTURE_LGM_HUNT_RADIUS {HUNT_RADIUS}")
        assert tile != CORPSE, "an errand tile cannot be the corpse tile"
    assert terrain[ROAD_TILE[1]][ROAD_TILE[0]] is GRASS, (
        "ROAD_TILE must be GRASS: the C-side LGM scan hides a tree-covered man "
        "more than 3 tiles out, and this arena needs him visible")
    assert terrain[FARM_TILE[1]][FARM_TILE[0]] is FOREST, (
        "FARM_TILE must be FOREST or there is nothing to chop")
    # The enemy must park on the far side of the corpse from p0, never between
    # them.  (The engine puts no distance limit on an LGM errand -- lgm.c has
    # no range test at all -- so the walk length is a pacing choice, not a
    # constraint; the ~5-tile figure in the brain is GoalHunter's own policy.)
    assert P1_PARK[0] > CORPSE[0] > SPAWN0[0], (
        "the enemy must park on the far side of the corpse from p0")
    # THE ENEMY TANK MUST BE OFF THE FIRING LINE, and this is the assertion
    # that took a run to find.  steering.lua's navigate-time OPPORTUNISTIC
    # shot fires at any enemy tank within OPP_TANK_RANGE tiles whose bearing is
    # within OPP_TANK_AIM bradians of where we are pointed -- and it runs
    # BEFORE init.lua's kill-LGM block, whose gate is "_shoot_busy: something
    # already claimed KEY_SHOOT this tick".  Measured with the enemy parked at
    # (131,126), dead behind the man: the hunt turned, drove the gunsight down
    # to 8, reported the shot READY -- and reported it `ready_busy` on every
    # single tick, because the drive-by tank shot had taken the trigger first.
    # So the enemy has to be far enough away, or far enough off the bearing,
    # that the drive-by can never claim it.
    for spot in (CORPSE, (CORPSE[0] - 2, CORPSE[1])):
        d = ((P1_PARK[0] - spot[0]) ** 2 + (P1_PARK[1] - spot[1]) ** 2) ** 0.5
        # Bearing separation in bradians (256 per full turn) between "at the
        # errand tile" and "at the enemy tank", seen from `spot`.
        import math as _m
        b_lgm = _m.atan2(ROAD_TILE[0] - spot[0], -(ROAD_TILE[1] - spot[1]))
        b_tnk = _m.atan2(P1_PARK[0] - spot[0], -(P1_PARK[1] - spot[1]))
        sep = abs((b_lgm - b_tnk) * 128.0 / _m.pi)
        if sep > 128:
            sep = 256 - sep
        assert d > OPP_TANK_RANGE or sep > OPP_TANK_AIM + 4, (
            f"from {spot} the enemy tank at {P1_PARK} is {d:.1f} tiles away "
            f"and only {sep:.0f} bradians off the bearing to the errand tile "
            f"{ROAD_TILE} - inside the navigate drive-by shot's "
            f"{OPP_TANK_RANGE}-tile / {OPP_TANK_AIM}-bradian window, which "
            "would take KEY_SHOOT before the kill-LGM block ever sees it")
    # p0 must start well outside shooting range so the approach is real.
    assert cheb(SPAWN0, ROAD_TILE) > SHOOT_RANGE, (
        f"p0 start {SPAWN0} is only Chebyshev {cheb(SPAWN0, ROAD_TILE)} from "
        f"the errand tile - inside KILL_LGM_SHOOT_RANGE {SHOOT_RANGE}, so "
        "there is no approach to measure")
    for s in SPARES:
        assert terrain[s[1]][s[0]] is not DEEP_SEA, f"spare {s} needs land"

    # Pill records: x, y, owner, armour, speed.  armour 0 = DEAD (capturable).
    pills = [(CORPSE[0], CORPSE[1], 0, 0, 100)]
    pills += [(s[0], s[1], 0, 0, 100) for s in SPARES]
    bases = [(BASE0[0], BASE0[1], 0, 90, 90, 90)]
    starts = [(SPAWN0[0], SPAWN0[1], 2)]           # dir 2 = east, at the corpse
    if nbots(variant) > 1:
        starts.append((SPAWN1[0], SPAWN1[1], 6))   # dir 6 = west, at the park

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

    write_sidecar(variant, Path(output).with_suffix(".scenario.lua"))

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  variant {variant}: corpse {CORPSE}, errands {ROAD_TILE}/"
          f"{FARM_TILE}, {len(SPARES)} spare(s), {nbots(variant)} tank(s)")
    print(f"  p0 start {SPAWN0} ({mdist(SPAWN0, CORPSE)} tiles from the corpse)"
          + (f", enemy parks {P1_PARK}" if nbots(variant) > 1 else ""))


# =========================================================================
# The sidecar.  Auto-loaded by the server as <map>.scenario.lua.
# =========================================================================
SIDECAR_HEAD = '''\
-- GENERATED by tests/generate_capture_lgm_hunt_map.py -- do not edit by hand.
-- Scenario sidecar for tests/capture_lgm_hunt_{V}.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/capture_lgm_hunt_test.py arena {V}.
--
-- Rows in capture_lgm_hunt_{V}_trace.log are `tick x y armour owner in_tank`,
-- one per CHANGE of the LIVE corpse, so "the pill was dead while the hunt ran"
-- and "the armour came back down" are asked of the ENGINE and never of the
-- brain's opinion of itself.  Lines starting # are events.

local TRACE   = "capture_lgm_hunt_{V}_trace.log"
local P0, P1  = 0, 1
local NBOTS   = {NBOTS}
local PULSE   = {PULSE}         -- H2: armour an invisible "repair" reaches
local PMODE   = {PMODE}      -- H2: an armour rise is the POINT, not a spent corpse
local HOLD    = {HOLD}          -- H2: sim ticks the pulse is held
local PERIOD  = {PERIOD}        -- H2: sim ticks between pulses
local AWAY    = {AWAY}          -- tiles p0 must be clear before a spare pops
local MAXWAIT = {MAXWAIT}       -- ...or this many sim ticks, whichever first
local CORPSE  = {{ {CX}, {CY} }}
local ROADT   = {{ {RX}, {RY} }}
local FARMT   = {{ {FX}, {FY} }}
local SPAWN0  = {{ {S0X}, {S0Y} }}
local SPAWN1  = {{ {S1X}, {S1Y} }}
local SPARES  = {SPARES}
local GRASS, FOREST = 7, 5
local FILL_TICK = 60

local idx_live   = nil            -- pill index currently playing the corpse
local spare_idx  = {{}}             -- resolved indices of the hidden spares
local next_spare = 1
local taken_at   = nil            -- sim tick the live corpse went into a tank
local grabs      = 0
local last_sig   = nil
local filled0, filled1 = false, false
local pulse_at, pulse_off = nil, nil
local pulses = 0
local resolved = false

local function note(line)
  local f = io.open(TRACE, "a")
  if f then f:write(line .. "\\n") f:close() end
end

local function same(p, t) return p.x == t[1] and p.y == t[2] end

-- Resolve tiles to pill INDICES once.  Everything after this reads the pill BY
-- INDEX: a pill that is picked up moves with its carrier, so a coordinate
-- match would stop finding it exactly when the trace exists to prove somebody
-- took it.
local function resolve(g)
  spare_idx = {{}}
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      if same(p, CORPSE) then
        idx_live = i
      else
        for s = 1, #SPARES do
          if same(p, SPARES[s]) then spare_idx[#spare_idx + 1] = i end
        end
      end
    end
  end
  return idx_live ~= nil
end

function on_setup(g)
  resolved = resolve(g)
  -- The corpse belongs to p0 and is DEAD: a free pill lying on the ground,
  -- which is exactly what capture_pill is for.
  if idx_live then
    g.set_pill_owner(idx_live, P0)
    g.set_pill_armour(idx_live, 0)
  end
  -- The spares are refills, not pillboxes: off the map until they are needed.
  -- NEUTRAL FIRST, AND THAT IS NOT COSMETIC.  hide_pill is pillsSetPillInTank
  -- (scenario.c), so a hidden pill still OWNED BY p0 reads to the engine and
  -- to the brain as a pillbox p0 is CARRYING.  Measured on the first draft:
  -- five hidden spares gave p0 five phantom pillboxes, it spent the whole run
  -- on place_pill_strategic (2056 goal changes) instead of the capture, and
  -- the arena measured nothing.  Ownership goes back to p0 in the refill.
  for i = 1, #spare_idx do
    g.set_pill_owner(spare_idx[i], g.NEUTRAL)
    g.hide_pill(spare_idx[i])
  end
  for i = 1, g.num_bases() do
    g.set_base_stock(i, 90, 90, 90)
  end
  local f = io.open(TRACE, "w")
  if f then f:write("# tick x y armour owner in_tank\\n") f:close() end
end

function on_choose_start(g, p)
  if p == P0 then return 1 end
  if p == P1 and NBOTS > 1 then return 2 end
  return nil
end

function on_tick(g, tick)
  -- ALLIANCE, APPLIED FROM on_tick.  botManagerSetTeams walks the bots' OWN
  -- ClientSim alliance tables and at on_setup time no bot exists yet, so a
  -- set_team there reaches the server roster and nothing else and the two
  -- tanks still read each other as OBJECT_HOSTILE.  Re-applied for the first
  -- 120 ticks so it lands whenever the slots actually come up.
  if NBOTS > 1 and tick <= 120 then
    g.set_team(P0, 0)
    g.set_team(P1, 1)
  end

  -- Fill the spawn ponds once each tank is ASHORE and off its boat.  Filling
  -- the tile while the tank still sits on it leaves the boat state stuck and
  -- the LGM never becomes available.
  if tick >= FILL_TICK then
    if not filled0 then
      local tk = g.tank(P0)
      if tk and not tk.boat and not tk.dead
         and (tk.mx ~= SPAWN0[1] or tk.my ~= SPAWN0[2]) then
        filled0 = true
        g.set_tile(SPAWN0[1], SPAWN0[2], GRASS)
        note(string.format("# pondfill0 %d", tick))
      end
    end
    if NBOTS > 1 and not filled1 then
      local tk = g.tank(P1)
      if tk and not tk.boat and not tk.dead
         and (tk.mx ~= SPAWN1[1] or tk.my ~= SPAWN1[2]) then
        filled1 = true
        g.set_tile(SPAWN1[1], SPAWN1[2], GRASS)
        note(string.format("# pondfill1 %d", tick))
      end
    end
  end

  -- KEEP THE ERRANDS ALIVE.  A paved road is not a road job any more and a
  -- chopped tree is gone for good, so without this the enemy's man stops being
  -- sent halfway through the measurement.  Re-stamping both tiles every tick
  -- costs nothing and also repairs a stray shell's damage.
  g.set_tile(ROADT[1], ROADT[2], GRASS)
  g.set_tile(FARMT[1], FARMT[2], FOREST)

  if not resolved then
    resolved = resolve(g)
    if not resolved then return end
  end
'''

SIDECAR_REFILL = '''
  -- ── ONE PILL ON THE MAP, ALWAYS THE CORPSE ──────────────────────────
  -- Anything lying on the ground that is not the live corpse gets taken off
  -- the map.  That is almost always the pill p0 just captured and then PLACED
  -- (measured: it planted it at (126,127), one tile from the corpse tile),
  -- and a LIVE pill next to the target is the worst possible scenery here --
  -- init.lua's kill-LGM LOS check treats any live pillbox in the shell's lane
  -- as a block, so the fire half of the feature could never open.
  for i = 1, g.num_pills() do
    if i ~= idx_live then
      local p = g.pill(i)
      if p and not p.in_tank
         and not (p.x == CORPSE[1] and p.y == CORPSE[2]) then
        g.set_pill_owner(i, g.NEUTRAL)
        if g.hide_pill(i) then
          note(string.format("# sweep %d pill=%d at(%d,%d) armour=%d",
                             tick, i, p.x, p.y, p.armour))
        end
      end
    end
  end

  -- ── THE REFILLING CORPSE ────────────────────────────────────────────
  -- Grabbing a dead pill is one tick of driving over it, so one corpse buys
  -- ONE approach.  The corpse is SPENT the moment it stops being "dead, on the
  -- ground, on the corpse tile" -- taken into a tank, or replanted elsewhere.
  -- Once it is spent, wait until p0 has driven AWAY tiles clear (or MAXWAIT
  -- ticks, whichever comes first, so a bot that parks on the tile cannot wedge
  -- the arena) and pop the next spare back onto the corpse tile.
  -- show_pill(n, x, y) lands it DEAD and on the ground, which is exactly the
  -- state a capture wants.
  -- PMODE (arena H2) is the exception to "armour > 0 means somebody rebuilt
  -- it, so this corpse is finished": there the pulse IS the measurement, and
  -- treating it as spent refilled a SECOND pill onto the same tile two ticks
  -- into every pulse.  world.pill_at keys a LIST per tile, the brain reads the
  -- first entry, and the armour watch then saw whichever of the two it
  -- happened to get -- the hunt engaged once and never again.
  local live = idx_live and g.pill(idx_live)
  local spent = (live == nil) or live.in_tank
                or (not PMODE and live.armour > 0)
                or live.x ~= CORPSE[1] or live.y ~= CORPSE[2]
  if spent then
    if not taken_at then
      taken_at = tick
      grabs = grabs + 1
      note(string.format("# spent %d n=%d in_tank=%s armour=%s", tick, grabs,
                         tostring(live and live.in_tank),
                         tostring(live and live.armour)))
    end
    local t0 = g.tank(P0)
    local away = 99
    if t0 and not t0.dead then
      local dx = t0.mx - CORPSE[1]; if dx < 0 then dx = -dx end
      local dy = t0.my - CORPSE[2]; if dy < 0 then dy = -dy end
      away = (dx > dy) and dx or dy
    end
    if next_spare <= #spare_idx
       and (away >= AWAY or (tick - taken_at) >= MAXWAIT) then
      local n = spare_idx[next_spare]
      next_spare = next_spare + 1
      if g.show_pill(n, CORPSE[1], CORPSE[2]) then
        g.set_pill_owner(n, P0)
        g.set_pill_armour(n, 0)
        idx_live = n
        taken_at = nil
        last_sig = nil
        note(string.format("# refill %d pill=%d away=%d", tick, n, away))
      end
    end
  else
    taken_at = nil
  end
'''

SIDECAR_PULSE = '''
  -- ── THE ARMOUR PULSE (arena H2 only) ────────────────────────────────
  -- A repair landing on the corpse with the builder invisible.  Held for HOLD
  -- ticks only: the pill is dead again long before the next replan, so the
  -- goal never leaves capture_pill, while CAPTURE_LGM_HUNT_ARMOUR_TICKS (100)
  -- keeps the hunt hot across the gap.  Starts once the tank is ashore and has
  -- had time to commit to the capture.
  if filled0 and tick >= FILL_TICK + 150 then
    local c = idx_live and g.pill(idx_live)
    if c and not c.in_tank then
      if pulse_off and tick >= pulse_off then
        -- Down again.  If the bot already shelled it back to 0 -- which is the
        -- outcome the feature is for -- say so; the test reports both.
        note(string.format("# pulse_end %d armour=%d %s", tick, c.armour,
                           c.armour == 0 and "shot_down" or "forced_down"))
        if c.armour ~= 0 then g.set_pill_armour(idx_live, 0) end
        pulse_off = nil
        pulse_at  = tick + PERIOD
      elseif pulse_off == nil and (pulse_at == nil or tick >= pulse_at) then
        g.set_pill_armour(idx_live, PULSE)
        pulses = pulses + 1
        pulse_off = tick + HOLD
        note(string.format("# pulse %d n=%d armour=%d", tick, pulses, PULSE))
      end
    end
  end
'''

SIDECAR_TAIL = '''
  -- ENGINE-SIDE TRACE of the live corpse: every change of armour, owner or
  -- in_tank, in SIM ticks.
  local q = idx_live and g.pill(idx_live)
  if q then
    local in_tank = q.in_tank and 1 or 0
    local sig = q.armour .. "/" .. q.owner .. "/" .. in_tank .. "/" .. q.x .. "," .. q.y
    if last_sig ~= sig then
      note(string.format("%d %d %d %d %d %d", tick, q.x, q.y,
                         q.armour, q.owner, in_tank))
      last_sig = sig
    end
  end
end
'''


def _lua_pairs(pts):
    return "{ " + ", ".join(f"{{ {x}, {y} }}" for (x, y) in pts) + " }"


def write_sidecar(variant, path):
    head = SIDECAR_HEAD.format(
        V=variant, NBOTS=nbots(variant),
        PULSE=ARMOUR_PULSE if variant == "H2" else 0,
        PMODE="true" if variant == "H2" else "false",
        HOLD=PULSE_HOLD, PERIOD=PULSE_PERIOD,
        AWAY=RESPAWN_AWAY, MAXWAIT=RESPAWN_MAX_WAIT,
        CX=CORPSE[0], CY=CORPSE[1],
        RX=ROAD_TILE[0], RY=ROAD_TILE[1],
        FX=FARM_TILE[0], FY=FARM_TILE[1],
        S0X=SPAWN0[0], S0Y=SPAWN0[1],
        S1X=SPAWN1[0], S1Y=SPAWN1[1],
        SPARES=_lua_pairs(SPARES))
    body = SIDECAR_REFILL + (SIDECAR_PULSE if variant == "H2" else "")
    path.write_text(head + body + SIDECAR_TAIL, encoding="utf-8", newline="\n")


if __name__ == '__main__':
    main()
